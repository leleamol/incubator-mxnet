/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 *  Copyright (c) 2019 by Contributors
 * \file np_reduce_op_value.cc
 * \brief CPU Implementation of broadcast and reduce functions based on value.
 */

#include "np_broadcast_reduce_op.h"

namespace mxnet {
namespace op {

DMLC_REGISTER_PARAMETER(NumpyReduceAxesParam);

inline bool NumpySumType(const nnvm::NodeAttrs& attrs,
                         std::vector<int> *in_attrs,
                         std::vector<int> *out_attrs) {
  CHECK_EQ(in_attrs->size(), 1U);
  CHECK_EQ(out_attrs->size(), 1U);
  const NumpyReduceAxesParam &param = nnvm::get<NumpyReduceAxesParam>(attrs.parsed);

  if (param.dtype.has_value()) {
    TYPE_ASSIGN_CHECK(*out_attrs, 0, param.dtype.value());
  } else {
    TYPE_ASSIGN_CHECK(*out_attrs, 0, in_attrs->at(0));
    TYPE_ASSIGN_CHECK(*in_attrs, 0, out_attrs->at(0));
  }

  return out_attrs->at(0) != -1 && in_attrs->at(0) != -1;
}

NNVM_REGISTER_OP(_np_sum)
.describe(R"code()code" ADD_FILELINE)
.set_num_inputs(1)
.set_num_outputs(1)
.set_attr_parser(ParamParser<NumpyReduceAxesParam>)
.set_attr<mxnet::FInferShape>("FInferShape", NumpyReduceAxesShape)
.set_attr<nnvm::FInferType>("FInferType", NumpySumType)
.set_attr<nnvm::FListInputNames>("FListInputNames",
  [](const NodeAttrs& attrs) {
    return std::vector<std::string>{"a"};
  })
.add_argument("a", "NDArray-or-Symbol", "The input")
.add_arguments(NumpyReduceAxesParam::__FIELDS__())
.set_attr<FCompute>("FCompute<cpu>", NumpyReduceAxesCompute<cpu, mshadow_op::sum, true>)
.set_attr<FResourceRequest>("FResourceRequest",
  [](const NodeAttrs& attrs) {
    return std::vector<ResourceRequest>{ResourceRequest::kTempSpace};
  })
.set_attr<nnvm::FGradient>("FGradient", ElemwiseGradUseNone{"_backward_np_sum"});

NNVM_REGISTER_OP(_backward_np_sum)
.set_num_outputs(1)
.set_attr_parser(ParamParser<NumpyReduceAxesParam>)
.set_attr<nnvm::TIsBackward>("TIsBackward", true)
.set_num_inputs(1)
.set_attr<FCompute>("FCompute<cpu>", NumpyReduceAxesBackwardUseNone<cpu>);

NNVM_REGISTER_OP(_np_prod)
.set_num_inputs(1)
.set_num_outputs(1)
.set_attr_parser(ParamParser<NumpyReduceAxesParam>)
.set_attr<mxnet::FInferShape>("FInferShape", NumpyReduceAxesShape)
.set_attr<nnvm::FInferType>("FInferType", NumpySumType)
.add_arguments(NumpyReduceAxesParam::__FIELDS__())
.set_attr<nnvm::FListInputNames>("FListInputNames",
  [](const NodeAttrs& attrs) {
    return std::vector<std::string>{"a"};
  })
.add_argument("a", "NDArray-or-Symbol", "The input")
.set_attr<FCompute>("FCompute<cpu>", NumpyReduceAxesCompute<cpu, mshadow_op::product, true>)
.set_attr<FResourceRequest>("FResourceRequest",
  [](const NodeAttrs& attrs) {
    return std::vector<ResourceRequest>{ResourceRequest::kTempSpace};
  })
.set_attr<nnvm::FGradient>("FGradient", ReduceGrad{"_backward_np_prod"});

NNVM_REGISTER_OP(_backward_np_prod)
.set_num_inputs(1)
.set_num_outputs(1)
.set_attr_parser(ParamParser<NumpyReduceAxesParam>)
.set_attr<nnvm::TIsBackward>("TIsBackward", true)
.set_attr<FCompute>("FCompute<cpu>", NumpyReduceAxesBackwardUseInOut<cpu, mshadow_op::rdiv>);

bool NumpyBroadcastToShape(const nnvm::NodeAttrs& attrs,
                           mxnet::ShapeVector *in_attrs,
                           mxnet::ShapeVector *out_attrs) {
  CHECK_EQ(in_attrs->size(), 1U);
  CHECK_EQ(out_attrs->size(), 1U);
  mxnet::TShape& ishape = (*in_attrs)[0];
  if (!mxnet::shape_is_known(ishape)) return false;
  const BroadcastToParam& param = nnvm::get<BroadcastToParam>(attrs.parsed);
  CHECK(mxnet::shape_is_known(param.shape))
      << "the objective shape for broadcasting array must be known";
  CHECK_LE(ishape.ndim(), param.shape.ndim())
      << "shape " << ishape << " is not broadcastable to " << param.shape;
  for (int i = param.shape.ndim() - 1; i >= 0; --i) {
    int j = i - param.shape.ndim() + ishape.ndim();
    if (j < 0) break;
    CHECK(ishape[j] == param.shape[i] || ishape[j] == 1)
        << "shape " << ishape << " is not broadcastable to " << param.shape;
  }
  SHAPE_ASSIGN_CHECK(*out_attrs, 0, param.shape);
  return true;
}

NNVM_REGISTER_OP(_np_broadcast_to)
.set_num_inputs(1)
.set_num_outputs(1)
.set_attr<nnvm::FListInputNames>("FListInputNames",
  [](const NodeAttrs& attrs) {
    return std::vector<std::string>{"array"};
  })
.set_attr<nnvm::FInferType>("FInferType", ElemwiseType<1, 1>)
.set_attr<nnvm::FGradient>("FGradient",
  [](const nnvm::NodePtr& n,
    const std::vector<nnvm::NodeEntry>& ograds) {
    return MakeNonlossGradNode("_backward_np_broadcast_to", n, ograds, {}, n->attrs.dict);
  })
.add_argument("array", "NDArray-or-Symbol", "The input")
.set_attr_parser(ParamParser<BroadcastToParam>)
.add_arguments(BroadcastToParam::__FIELDS__())
.set_attr<mxnet::FInferShape>("FInferShape", NumpyBroadcastToShape)
.set_attr<FCompute>("FCompute<cpu>", NumpyBroadcastToForward<cpu>);

NNVM_REGISTER_OP(_backward_np_broadcast_to)
.set_attr_parser(ParamParser<BroadcastToParam>)
.set_attr<nnvm::TIsBackward>("TIsBackward", true)
.set_attr<FCompute>("FCompute<cpu>", NumpyBroadcastToBackward<cpu>)
.set_attr<FResourceRequest>("FResourceRequest",
  [](const NodeAttrs& attrs) {
    return std::vector<ResourceRequest>{ResourceRequest::kTempSpace};
  });

}  // namespace op
}  // namespace mxnet
