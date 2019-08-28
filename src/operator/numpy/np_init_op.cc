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
 * \file np_init_op.cc
 * \brief CPU Implementation of numpy init op
 */
#include "../tensor/init_op.h"
#include "../tensor/elemwise_unary_op.h"

namespace mxnet {
namespace op {

NNVM_REGISTER_OP(_npi_zeros)
.set_num_inputs(0)
.set_num_outputs(1)
.set_attr_parser(ParamParser<InitOpParam>)
.set_attr<mxnet::FInferShape>("FInferShape", InitShape<InitOpParam>)
.set_attr<nnvm::FInferType>("FInferType", InitType<InitOpParam>)
.set_attr<FInferStorageType>("FInferStorageType", InitStorageType<InitOpParam, true, true>)
.set_attr<FCompute>("FCompute<cpu>", FillCompute<cpu, 0>)
.add_arguments(InitOpParam::__FIELDS__());

NNVM_REGISTER_OP(_npi_ones)
.describe("Return a new array of given shape, type, and context, filled with ones.")
.set_num_inputs(0)
.set_num_outputs(1)
.set_attr_parser(ParamParser<InitOpParam>)
.set_attr<mxnet::FInferShape>("FInferShape", InitShape<InitOpParam>)
.set_attr<nnvm::FInferType>("FInferType", InitType<InitOpParam>)
.set_attr<FCompute>("FCompute<cpu>", FillCompute<cpu, 1>)
.add_arguments(InitOpParam::__FIELDS__());

NNVM_REGISTER_OP(_np_zeros_like)
.set_num_inputs(1)
.set_num_outputs(1)
.set_attr<mxnet::FInferShape>("FInferShape", ElemwiseShape<1, 1>)
.set_attr<nnvm::FInferType>("FInferType", ElemwiseType<1, 1>)
.set_attr<nnvm::FIgnoreInputs>("FIgnoreInputs",
  [](const NodeAttrs& attrs) {
    return std::vector<uint32_t>(1, 0);
  })
.set_attr<nnvm::FListInputNames>("FListInputNames",
  [](const NodeAttrs& attrs) {
    return std::vector<std::string>{"a"};
  })
.set_attr<FCompute>("FCompute<cpu>", FillCompute<cpu, 0>)
.set_attr<nnvm::FGradient>("FGradient", MakeZeroGradNodes)
.add_argument("a", "NDArray-or-Symbol",
              "The shape and data-type of a define these same attributes of the returned array.");

NNVM_REGISTER_OP(_np_ones_like)
.set_num_inputs(1)
.set_num_outputs(1)
.set_attr<mxnet::FInferShape>("FInferShape", ElemwiseShape<1, 1>)
.set_attr<nnvm::FInferType>("FInferType", ElemwiseType<1, 1>)
.set_attr<nnvm::FIgnoreInputs>("FIgnoreInputs",
  [](const NodeAttrs& attrs) {
    return std::vector<uint32_t>(1, 0);
  })
.set_attr<nnvm::FListInputNames>("FListInputNames",
  [](const NodeAttrs& attrs) {
    return std::vector<std::string>{"a"};
  })
.set_attr<FCompute>("FCompute<cpu>", FillCompute<cpu, 1>)
.set_attr<nnvm::FGradient>("FGradient", MakeZeroGradNodes)
.add_argument("a", "NDArray-or-Symbol",
              "The shape and data-type of a define these same attributes of the returned array.");

bool NumpyRangeShape(const nnvm::NodeAttrs& attrs,
                     mxnet::ShapeVector* in_shapes,
                     mxnet::ShapeVector* out_shapes) {
  const RangeParam& param = nnvm::get<RangeParam>(attrs.parsed);
  CHECK_EQ(in_shapes->size(), 0U);
  CHECK_EQ(out_shapes->size(), 1U);
  CHECK_NE(param.step, 0) << "_npi_arange does not support step=0";
  CHECK_EQ(param.repeat, 1) << "_npi_arange only supports repeat=1, received " << param.repeat;
  CHECK(param.stop.has_value()) << "_npi_arange requires stop to have a value";
  double out_size = std::ceil((param.stop.value() - param.start) / param.step);
  if (out_size < 0) {
    out_size = 0;
  }
  SHAPE_ASSIGN_CHECK(*out_shapes, 0, mxnet::TShape({static_cast<nnvm::dim_t>(out_size)}));
  return true;
}

NNVM_REGISTER_OP(_npi_arange)
.set_num_inputs(0)
.set_num_outputs(1)
.set_attr_parser(RangeParamParser)
.set_attr<mxnet::FInferShape>("FInferShape", NumpyRangeShape)
.set_attr<nnvm::FInferType>("FInferType", InitType<RangeParam>)
.set_attr<FCompute>("FCompute<cpu>", RangeCompute<cpu, RangeParam>)
.add_arguments(RangeParam::__FIELDS__());

}  // namespace op
}  // namespace mxnet
