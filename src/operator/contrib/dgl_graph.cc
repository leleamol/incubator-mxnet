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

#include <mxnet/io.h>
#include <mxnet/base.h>
#include <mxnet/ndarray.h>
#include <mxnet/operator.h>
#include <mxnet/operator_util.h>
#include <dmlc/logging.h>
#include <dmlc/optional.h>
#include "../elemwise_op_common.h"
#include "../../imperative/imperative_utils.h"
#include "../subgraph_op_common.h"
#include "./dgl_graph-inl.h"

namespace mxnet {
namespace op {

typedef int64_t dgl_id_t;

////////////////////////////// Graph Sampling ///////////////////////////////

/*
 * ArrayHeap is used to sample elements from vector
 */
class ArrayHeap {
 public:
  explicit ArrayHeap(const std::vector<float>& prob) {
    vec_size_ = prob.size();
    bit_len_ = ceil(log2(vec_size_));
    limit_ = 1 << bit_len_;
    // allocate twice the size
    heap_.resize(limit_ << 1, 0);
    // allocate the leaves
    for (int i = limit_; i < vec_size_+limit_; ++i) {
      heap_[i] = prob[i-limit_];
    }
    // iterate up the tree (this is O(m))
    for (int i = bit_len_-1; i >= 0; --i) {
      for (int j = (1 << i); j < (1 << (i + 1)); ++j) {
        heap_[j] = heap_[j << 1] + heap_[(j << 1) + 1];
      }
    }
  }
  ~ArrayHeap() {}

  /*
   * Remove term from index (this costs O(log m) steps)
   */
  void Delete(size_t index) {
    size_t i = index + limit_;
    float w = heap_[i];
    for (int j = bit_len_; j >= 0; --j) {
      heap_[i] -= w;
      i = i >> 1;
    }
  }

  /*
   * Add value w to index (this costs O(log m) steps)
   */
  void Add(size_t index, float w) {
    size_t i = index + limit_;
    for (int j = bit_len_; j >= 0; --j) {
      heap_[i] += w;
      i = i >> 1;
    }
  }

  /*
   * Sample from arrayHeap
   */
  size_t Sample(unsigned int* seed) {
    float xi = heap_[1] * (rand_r(seed)%100/101.0);
    int i = 1;
    while (i < limit_) {
      i = i << 1;
      if (xi >= heap_[i]) {
        xi -= heap_[i];
        i += 1;
      }
    }
    return i - limit_;
  }

  /*
   * Sample a vector by given the size n
   */
  void SampleWithoutReplacement(size_t n, std::vector<size_t>* samples, unsigned int* seed) {
    // sample n elements
    for (size_t i = 0; i < n; ++i) {
      samples->at(i) = this->Sample(seed);
      this->Delete(samples->at(i));
    }
  }

 private:
  int vec_size_;  // sample size
  int bit_len_;   // bit size
  int limit_;
  std::vector<float> heap_;
};

struct NeighborSampleParam : public dmlc::Parameter<NeighborSampleParam> {
  int num_args;
  dgl_id_t num_hops;
  dgl_id_t num_neighbor;
  dgl_id_t max_num_vertices;
  DMLC_DECLARE_PARAMETER(NeighborSampleParam) {
    DMLC_DECLARE_FIELD(num_args).set_lower_bound(2)
    .describe("Number of input NDArray.");
    DMLC_DECLARE_FIELD(num_hops)
      .set_default(1)
      .describe("Number of hops.");
    DMLC_DECLARE_FIELD(num_neighbor)
      .set_default(2)
      .describe("Number of neighbor.");
    DMLC_DECLARE_FIELD(max_num_vertices)
      .set_default(100)
      .describe("Max number of vertices.");
  }
};

DMLC_REGISTER_PARAMETER(NeighborSampleParam);

/*
 * Check uniform Storage Type
 */
static bool CSRNeighborUniformSampleStorageType(const nnvm::NodeAttrs& attrs,
                                                const int dev_mask,
                                                DispatchMode* dispatch_mode,
                                                std::vector<int> *in_attrs,
                                                std::vector<int> *out_attrs) {
  const NeighborSampleParam& params = nnvm::get<NeighborSampleParam>(attrs.parsed);

  size_t num_subgraphs = params.num_args - 1;
  CHECK_EQ(out_attrs->size(), 3 * num_subgraphs);

  // input[0] is csr_graph
  CHECK_EQ(in_attrs->at(0), mxnet::kCSRStorage);
  // the rest input ndarray is seed_vector
  for (size_t i = 0; i < num_subgraphs; i++)
    CHECK_EQ(in_attrs->at(1 + i), mxnet::kDefaultStorage);

  bool success = true;
  // sample_id
  for (size_t i = 0; i < num_subgraphs; i++) {
    if (!type_assign(&(*out_attrs)[i], mxnet::kDefaultStorage)) {
      success = false;
    }
  }
  // sub_graph
  for (size_t i = 0; i < num_subgraphs; i++) {
    if (!type_assign(&(*out_attrs)[i + num_subgraphs], mxnet::kCSRStorage)) {
      success = false;
    }
  }
  // sub_layer
  for (size_t i = 0; i < num_subgraphs; i++) {
    if (!type_assign(&(*out_attrs)[i + 2*num_subgraphs], mxnet::kDefaultStorage)) {
      success = false;
    }
  }

  *dispatch_mode = DispatchMode::kFComputeEx;

  return success;
}

/*
 * Check non-uniform Storage Type
 */
static bool CSRNeighborNonUniformSampleStorageType(const nnvm::NodeAttrs& attrs,
                                                   const int dev_mask,
                                                   DispatchMode* dispatch_mode,
                                                   std::vector<int> *in_attrs,
                                                   std::vector<int> *out_attrs) {
  const NeighborSampleParam& params =
    nnvm::get<NeighborSampleParam>(attrs.parsed);

  size_t num_subgraphs = params.num_args - 2;
  CHECK_EQ(out_attrs->size(), 4 * num_subgraphs);

  // input[0] is csr_graph
  CHECK_EQ(in_attrs->at(0), mxnet::kCSRStorage);
  // input[1] is probability
  CHECK_EQ(in_attrs->at(1), mxnet::kDefaultStorage);

  // the rest input ndarray is seed_vector
  for (size_t i = 0; i < num_subgraphs; i++)
    CHECK_EQ(in_attrs->at(2 + i), mxnet::kDefaultStorage);

  bool success = true;
  // sample_id
  for (size_t i = 0; i < num_subgraphs; i++) {
    if (!type_assign(&(*out_attrs)[i], mxnet::kDefaultStorage)) {
      success = false;
    }
  }
  // sub_graph
  for (size_t i = 0; i < num_subgraphs; i++) {
    if (!type_assign(&(*out_attrs)[i + num_subgraphs], mxnet::kCSRStorage)) {
      success = false;
    }
  }
  // sub_probability
  for (size_t i = 0; i < num_subgraphs; i++) {
    if (!type_assign(&(*out_attrs)[i + 2*num_subgraphs], mxnet::kDefaultStorage)) {
      success = false;
    }
  }
  // sub_layer
  for (size_t i = 0; i < num_subgraphs; i++) {
    if (!type_assign(&(*out_attrs)[i + 3*num_subgraphs], mxnet::kDefaultStorage)) {
      success = false;
    }
  }

  *dispatch_mode = DispatchMode::kFComputeEx;

  return success;
}

/*
 * Check uniform Shape
 */
static bool CSRNeighborUniformSampleShape(const nnvm::NodeAttrs& attrs,
                                          std::vector<TShape> *in_attrs,
                                          std::vector<TShape> *out_attrs) {
  const NeighborSampleParam& params =
    nnvm::get<NeighborSampleParam>(attrs.parsed);

  size_t num_subgraphs = params.num_args - 1;
  CHECK_EQ(out_attrs->size(), 3 * num_subgraphs);
  // input[0] is csr graph
  CHECK_EQ(in_attrs->at(0).ndim(), 2U);
  CHECK_EQ(in_attrs->at(0)[0], in_attrs->at(0)[1]);

  // the rest input ndarray is seed vector
  for (size_t i = 0; i < num_subgraphs; i++) {
    CHECK_EQ(in_attrs->at(1 + i).ndim(), 1U);
  }

  // Output
  bool success = true;
  TShape out_shape(1);
  // We use the last element to store the actual
  // number of vertices in the subgraph.
  out_shape[0] = params.max_num_vertices + 1;
  for (size_t i = 0; i < num_subgraphs; i++) {
    SHAPE_ASSIGN_CHECK(*out_attrs, i, out_shape);
    success = success &&
              out_attrs->at(i).ndim() != 0U &&
              out_attrs->at(i).Size() != 0U;
  }
  // sub_csr
  TShape out_csr_shape(2);
  out_csr_shape[0] = params.max_num_vertices;
  out_csr_shape[1] = in_attrs->at(0)[1];
  for (size_t i = 0; i < num_subgraphs; i++) {
    SHAPE_ASSIGN_CHECK(*out_attrs, i + num_subgraphs, out_csr_shape);
    success = success &&
              out_attrs->at(i + num_subgraphs).ndim() != 0U &&
              out_attrs->at(i + num_subgraphs).Size() != 0U;
  }
  // sub_layer
  TShape out_layer_shape(1);
  out_layer_shape[0] = params.max_num_vertices;
  for (size_t i = 0; i < num_subgraphs; i++) {
    SHAPE_ASSIGN_CHECK(*out_attrs, i + 2*num_subgraphs, out_layer_shape);
    success = success &&
              out_attrs->at(i + 2*num_subgraphs).ndim() != 0U &&
              out_attrs->at(i + 2*num_subgraphs).Size() != 0U;
  }

  return success;
}

/*
 * Check non-uniform Shape
 */
static bool CSRNeighborNonUniformSampleShape(const nnvm::NodeAttrs& attrs,
                                             std::vector<TShape> *in_attrs,
                                             std::vector<TShape> *out_attrs) {
  const NeighborSampleParam& params =
    nnvm::get<NeighborSampleParam>(attrs.parsed);

  size_t num_subgraphs = params.num_args - 2;
  CHECK_EQ(out_attrs->size(), 4 * num_subgraphs);
  // input[0] is csr graph
  CHECK_EQ(in_attrs->at(0).ndim(), 2U);
  CHECK_EQ(in_attrs->at(0)[0], in_attrs->at(0)[1]);

  // input[1] is probability
  CHECK_EQ(in_attrs->at(1).ndim(), 1U);

  // the rest ndarray is seed vector
  for (size_t i = 0; i < num_subgraphs; i++) {
    CHECK_EQ(in_attrs->at(2 + i).ndim(), 1U);
  }

  // Output
  bool success = true;
  TShape out_shape(1);
  // We use the last element to store the actual
  // number of vertices in the subgraph.
  out_shape[0] = params.max_num_vertices + 1;
  for (size_t i = 0; i < num_subgraphs; i++) {
    SHAPE_ASSIGN_CHECK(*out_attrs, i, out_shape);
    success = success &&
              out_attrs->at(i).ndim() != 0U &&
              out_attrs->at(i).Size() != 0U;
  }
  // sub_csr
  TShape out_csr_shape(2);
  out_csr_shape[0] = params.max_num_vertices;
  out_csr_shape[1] = in_attrs->at(0)[1];
  for (size_t i = 0; i < num_subgraphs; i++) {
    SHAPE_ASSIGN_CHECK(*out_attrs, i + num_subgraphs, out_csr_shape);
    success = success &&
              out_attrs->at(i + num_subgraphs).ndim() != 0U &&
              out_attrs->at(i + num_subgraphs).Size() != 0U;
  }
  // sub_probability
  TShape out_prob_shape(1);
  out_prob_shape[0] = params.max_num_vertices;
  for (size_t i = 0; i < num_subgraphs; i++) {
    SHAPE_ASSIGN_CHECK(*out_attrs, i + 2*num_subgraphs, out_prob_shape);
    success = success &&
              out_attrs->at(i + 2*num_subgraphs).ndim() != 0U &&
              out_attrs->at(i + 2*num_subgraphs).Size() != 0U;
  }
  // sub_layer
  TShape out_layer_shape(1);
  out_layer_shape[0] = params.max_num_vertices;
  for (size_t i = 0; i < num_subgraphs; i++) {
    SHAPE_ASSIGN_CHECK(*out_attrs, i + 3*num_subgraphs, out_prob_shape);
    success = success &&
              out_attrs->at(i + 3*num_subgraphs).ndim() != 0U &&
              out_attrs->at(i + 3*num_subgraphs).Size() != 0U;
  }

  return success;
}

/*
 * Check uniform Type
 */
static bool CSRNeighborUniformSampleType(const nnvm::NodeAttrs& attrs,
                                         std::vector<int> *in_attrs,
                                         std::vector<int> *out_attrs) {
  const NeighborSampleParam& params =
    nnvm::get<NeighborSampleParam>(attrs.parsed);

  size_t num_subgraphs = params.num_args - 1;
  CHECK_EQ(out_attrs->size(), 3 * num_subgraphs);

  bool success = true;
  for (size_t i = 0; i < num_subgraphs; i++) {
    TYPE_ASSIGN_CHECK(*out_attrs, i, in_attrs->at(1));
    TYPE_ASSIGN_CHECK(*out_attrs, i + num_subgraphs, in_attrs->at(0));
    TYPE_ASSIGN_CHECK(*out_attrs, i + 2*num_subgraphs, in_attrs->at(1));
    success = success &&
               out_attrs->at(i) != -1 &&
               out_attrs->at(i + num_subgraphs) != -1 &&
               out_attrs->at(i + 2*num_subgraphs) != -1;
  }

  return success;
}

/*
 * Check non-uniform Type
 */
static bool CSRNeighborNonUniformSampleType(const nnvm::NodeAttrs& attrs,
                                            std::vector<int> *in_attrs,
                                            std::vector<int> *out_attrs) {
  const NeighborSampleParam& params =
    nnvm::get<NeighborSampleParam>(attrs.parsed);

  size_t num_subgraphs = params.num_args - 2;
  CHECK_EQ(out_attrs->size(), 4 * num_subgraphs);

  bool success = true;
  for (size_t i = 0; i < num_subgraphs; i++) {
    TYPE_ASSIGN_CHECK(*out_attrs, i, in_attrs->at(2));
    TYPE_ASSIGN_CHECK(*out_attrs, i + num_subgraphs, in_attrs->at(0));
    TYPE_ASSIGN_CHECK(*out_attrs, i + 2*num_subgraphs, in_attrs->at(1));
    TYPE_ASSIGN_CHECK(*out_attrs, i + 3*num_subgraphs, in_attrs->at(2));
    success = success &&
               out_attrs->at(i) != -1 &&
               out_attrs->at(i + num_subgraphs) != -1 &&
               out_attrs->at(i + 2*num_subgraphs) != -1 &&
               out_attrs->at(i + 3*num_subgraphs) != -1;
  }

  return success;
}

/*
 * Get src vertex and edge id for a destination vertex
 */
static void GetSrcList(const dgl_id_t* val_list,
                       const dgl_id_t* col_list,
                       const dgl_id_t* indptr,
                       const dgl_id_t dst_id,
                       std::vector<dgl_id_t>* src_list,
                       std::vector<dgl_id_t>* edge_list) {
  for (dgl_id_t i = *(indptr+dst_id); i < *(indptr+dst_id+1); ++i) {
    src_list->push_back(col_list[i]);
    edge_list->push_back(val_list[i]);
  }
}

static void RandomSample(size_t set_size,
                         size_t num,
                         std::vector<size_t>* out,
                         unsigned int* seed) {
  std::unordered_set<size_t> sampled_idxs;
  while (sampled_idxs.size() < num) {
    sampled_idxs.insert(rand_r(seed) % set_size);
  }
  out->clear();
  for (auto it = sampled_idxs.begin(); it != sampled_idxs.end(); it++) {
    out->push_back(*it);
  }
}

static void NegateSet(const std::vector<size_t> &idxs,
                      size_t set_size,
                      std::vector<size_t>* out) {
  // idxs must have been sorted.
  auto it = idxs.begin();
  size_t i = 0;
  CHECK_GT(set_size, idxs.back());
  for (; i < set_size && it != idxs.end(); i++) {
    if (*it == i) {
      it++;
      continue;
    }
    out->push_back(i);
  }
  for (; i < set_size; i++) {
    out->push_back(i);
  }
}

/*
 * Uniform sample
 */
static void GetUniformSample(const std::vector<dgl_id_t>& ver_list,
                             const std::vector<dgl_id_t>& edge_list,
                             const size_t max_num_neighbor,
                             std::vector<dgl_id_t>* out_ver,
                             std::vector<dgl_id_t>* out_edge,
                             unsigned int* seed) {
  CHECK_EQ(ver_list.size(), edge_list.size());
  // Copy ver_list to output
  if (ver_list.size() <= max_num_neighbor) {
    for (size_t i = 0; i < ver_list.size(); ++i) {
      out_ver->push_back(ver_list[i]);
      out_edge->push_back(edge_list[i]);
    }
    return;
  }
  // If we just sample a small number of elements from a large neighbor list.
  std::vector<size_t> sorted_idxs;
  if (ver_list.size() > max_num_neighbor * 2) {
    sorted_idxs.reserve(max_num_neighbor);
    RandomSample(ver_list.size(), max_num_neighbor, &sorted_idxs, seed);
    std::sort(sorted_idxs.begin(), sorted_idxs.end());
  } else {
    std::vector<size_t> negate;
    negate.reserve(ver_list.size() - max_num_neighbor);
    RandomSample(ver_list.size(), ver_list.size() - max_num_neighbor,
                 &negate, seed);
    std::sort(negate.begin(), negate.end());
    NegateSet(negate, ver_list.size(), &sorted_idxs);
  }
  // verify the result.
  CHECK_EQ(sorted_idxs.size(), max_num_neighbor);
  for (size_t i = 1; i < sorted_idxs.size(); i++) {
    CHECK_GT(sorted_idxs[i], sorted_idxs[i - 1]);
  }
  for (auto idx : sorted_idxs) {
    out_ver->push_back(ver_list[idx]);
    out_edge->push_back(edge_list[idx]);
  }
}

/*
 * Non-uniform sample via ArrayHeap
 */
static void GetNonUniformSample(const float* probability,
                                const std::vector<dgl_id_t>& ver_list,
                                const std::vector<dgl_id_t>& edge_list,
                                const size_t max_num_neighbor,
                                std::vector<dgl_id_t>* out_ver,
                                std::vector<dgl_id_t>* out_edge,
                                unsigned int* seed) {
  CHECK_EQ(ver_list.size(), edge_list.size());
  // Copy ver_list to output
  if (ver_list.size() <= max_num_neighbor) {
    for (size_t i = 0; i < ver_list.size(); ++i) {
      out_ver->push_back(ver_list[i]);
      out_edge->push_back(edge_list[i]);
    }
    return;
  }
  // Make sample
  std::vector<size_t> sp_index(max_num_neighbor);
  std::vector<float> sp_prob(ver_list.size());
  for (size_t i = 0; i < ver_list.size(); ++i) {
    sp_prob[i] = probability[ver_list[i]];
  }
  ArrayHeap arrayHeap(sp_prob);
  arrayHeap.SampleWithoutReplacement(max_num_neighbor, &sp_index, seed);
  out_ver->resize(max_num_neighbor);
  out_edge->resize(max_num_neighbor);
  for (size_t i = 0; i < max_num_neighbor; ++i) {
    size_t idx = sp_index[i];
    out_ver->at(i) = ver_list[idx];
    out_edge->at(i) = edge_list[idx];
  }
  sort(out_ver->begin(), out_ver->end());
  sort(out_edge->begin(), out_edge->end());
}

/*
 * This is used for BFS traversal
 */
struct ver_node {
  dgl_id_t vertex_id;
  int level;
};

/*
 * Used for subgraph sampling
 */
struct neigh_list {
  std::vector<dgl_id_t> neighs;
  std::vector<dgl_id_t> edges;
  neigh_list(const std::vector<dgl_id_t> &_neighs,
             const std::vector<dgl_id_t> &_edges)
    : neighs(_neighs), edges(_edges) {}
};

/*
 * Sample sub-graph from csr graph
 */
static void SampleSubgraph(const NDArray &csr,
                           const NDArray &seed_arr,
                           const NDArray &sampled_ids,
                           const NDArray &sub_csr,
                           float* sub_prob,
                           const NDArray &sub_layer,
                           const float* probability,
                           dgl_id_t num_hops,
                           dgl_id_t num_neighbor,
                           dgl_id_t max_num_vertices) {
  unsigned int time_seed = time(nullptr);
  size_t num_seeds = seed_arr.shape().Size();
  CHECK_GE(max_num_vertices, num_seeds);

  const dgl_id_t* val_list = csr.data().dptr<dgl_id_t>();
  const dgl_id_t* col_list = csr.aux_data(csr::kIdx).dptr<dgl_id_t>();
  const dgl_id_t* indptr = csr.aux_data(csr::kIndPtr).dptr<dgl_id_t>();
  const dgl_id_t* seed = seed_arr.data().dptr<dgl_id_t>();
  dgl_id_t* out = sampled_ids.data().dptr<dgl_id_t>();
  dgl_id_t* out_layer = sub_layer.data().dptr<dgl_id_t>();

  // BFS traverse the graph and sample vertices
  dgl_id_t sub_vertices_count = 0;
  // <vertex_id, layer_id>
  std::unordered_map<dgl_id_t, int> sub_ver_mp;
  std::queue<ver_node> node_queue;
  // add seed vertices
  for (size_t i = 0; i < num_seeds; ++i) {
    ver_node node;
    node.vertex_id = seed[i];
    node.level = 0;
    node_queue.push(node);
  }
  std::vector<dgl_id_t> tmp_src_list;
  std::vector<dgl_id_t> tmp_edge_list;
  std::vector<dgl_id_t> tmp_sampled_src_list;
  std::vector<dgl_id_t> tmp_sampled_edge_list;
  std::unordered_map<dgl_id_t, neigh_list> neigh_mp;
  size_t num_edges = 0;
  while (!node_queue.empty() &&
    sub_vertices_count <= max_num_vertices ) {
    ver_node& cur_node = node_queue.front();
    dgl_id_t dst_id = cur_node.vertex_id;
    if (cur_node.level < num_hops) {
      auto ret = sub_ver_mp.find(dst_id);
      if (ret != sub_ver_mp.end()) {
        node_queue.pop();
        continue;
      }
      tmp_src_list.clear();
      tmp_edge_list.clear();
      tmp_sampled_src_list.clear();
      tmp_sampled_edge_list.clear();
      GetSrcList(val_list,
                 col_list,
                 indptr,
                 dst_id,
                 &tmp_src_list,
                 &tmp_edge_list);
      if (probability == nullptr) {  // uniform-sample
        GetUniformSample(tmp_src_list,
                       tmp_edge_list,
                       num_neighbor,
                       &tmp_sampled_src_list,
                       &tmp_sampled_edge_list,
                       &time_seed);
      } else {  // non-uniform-sample
        GetNonUniformSample(probability,
                       tmp_src_list,
                       tmp_edge_list,
                       num_neighbor,
                       &tmp_sampled_src_list,
                       &tmp_sampled_edge_list,
                       &time_seed);
      }
      neigh_mp.insert(std::pair<dgl_id_t, neigh_list>(dst_id,
        neigh_list(tmp_sampled_src_list,
                   tmp_sampled_edge_list)));
      num_edges += tmp_sampled_src_list.size();
      sub_ver_mp[cur_node.vertex_id] = cur_node.level;
      for (size_t i = 0; i < tmp_sampled_src_list.size(); ++i) {
        auto ret = sub_ver_mp.find(tmp_sampled_src_list[i]);
        if (ret == sub_ver_mp.end()) {
          ver_node new_node;
          new_node.vertex_id = tmp_sampled_src_list[i];
          new_node.level = cur_node.level + 1;
          node_queue.push(new_node);
        }
      }
    } else {  // vertex without any neighbor
      auto ret = sub_ver_mp.find(dst_id);
      if (ret != sub_ver_mp.end()) {
        node_queue.pop();
        continue;
      }
      tmp_sampled_src_list.clear();
      tmp_sampled_edge_list.clear();
      neigh_mp.insert(std::pair<dgl_id_t, neigh_list>(dst_id,
        neigh_list(tmp_sampled_src_list,      // empty vector
                   tmp_sampled_edge_list)));  // empty vector
      sub_ver_mp[cur_node.vertex_id] = cur_node.level;
    }
    sub_vertices_count++;
    node_queue.pop();
  }

  // Copy sub_ver_mp to output[0]
  size_t idx = 0;
  for (auto& data : sub_ver_mp) {
    *(out+idx) = data.first;
    idx++;
  }
  size_t num_vertices = sub_ver_mp.size();
  std::sort(out, out + num_vertices);
  // The rest data will be set to -1
  for (dgl_id_t i = idx; i < max_num_vertices; ++i) {
    *(out+i) = -1;
  }
  // The last element stores the actual
  // number of vertices in the subgraph.
  out[max_num_vertices] = sub_ver_mp.size();
  // Copy sub_probability
  if (sub_prob != nullptr) {
    for (dgl_id_t i = 0; i < max_num_vertices; ++i) {
      dgl_id_t idx = out[i];
      if (idx != -1) {
        sub_prob[i] = probability[idx];
      } else {
        sub_prob[i] = -1;
      }
    }
  }
  // Copy layer
  for (dgl_id_t i = 0; i < max_num_vertices; ++i) {
    dgl_id_t idx = out[i];
    if (idx != -1) {
      out_layer[i] = sub_ver_mp[idx];
    } else {
      out_layer[i] = -1;
    }
  }
  // Construct sub_csr_graph
  TShape shape_1(1);
  TShape shape_2(1);
  shape_1[0] = num_edges;
  shape_2[0] = max_num_vertices+1;
  sub_csr.CheckAndAllocData(shape_1);
  sub_csr.CheckAndAllocAuxData(csr::kIdx, shape_1);
  sub_csr.CheckAndAllocAuxData(csr::kIndPtr, shape_2);
  dgl_id_t* val_list_out = sub_csr.data().dptr<dgl_id_t>();
  dgl_id_t* col_list_out = sub_csr.aux_data(1).dptr<dgl_id_t>();
  dgl_id_t* indptr_out = sub_csr.aux_data(0).dptr<dgl_id_t>();
  indptr_out[0] = 0;
  size_t collected_nedges = 0;
  for (size_t i = 0; i < num_vertices; i++) {
    dgl_id_t dst_id = *(out + i);
    auto it = neigh_mp.find(dst_id);
    const auto &edges = it->second.edges;
    const auto &neighs = it->second.neighs;
    CHECK_EQ(edges.size(), neighs.size());
    if (!edges.empty()) {
      std::copy(edges.begin(), edges.end(), val_list_out + collected_nedges);
      std::copy(neighs.begin(), neighs.end(), col_list_out + collected_nedges);
      collected_nedges += edges.size();
    }
    indptr_out[i+1] = indptr_out[i] + edges.size();
  }
  for (dgl_id_t i = num_vertices+1; i <= max_num_vertices; ++i) {
    indptr_out[i] = indptr_out[i-1];
  }
}

/*
 * Operator: contrib_csr_neighbor_uniform_sample
 */
static void CSRNeighborUniformSampleComputeExCPU(const nnvm::NodeAttrs& attrs,
                                          const OpContext& ctx,
                                          const std::vector<NDArray>& inputs,
                                          const std::vector<OpReqType>& req,
                                          const std::vector<NDArray>& outputs) {
  const NeighborSampleParam& params =
    nnvm::get<NeighborSampleParam>(attrs.parsed);

  int num_subgraphs = inputs.size() - 1;
  CHECK_EQ(outputs.size(), 3 * num_subgraphs);

#pragma omp parallel for
  for (int i = 0; i < num_subgraphs; i++) {
    SampleSubgraph(inputs[0],                     // graph_csr
                   inputs[i + 1],                 // seed vector
                   outputs[i],                    // sample_id
                   outputs[i + 1*num_subgraphs],  // sub_csr
                   nullptr,                       // sample_id_probability
                   outputs[i + 2*num_subgraphs],  // sample_id_layer
                   nullptr,                       // probability
                   params.num_hops,
                   params.num_neighbor,
                   params.max_num_vertices);
  }
}

NNVM_REGISTER_OP(_contrib_dgl_csr_neighbor_uniform_sample)
.describe(R"code(This operator samples sub-graph from a csr graph via an
uniform probability. 
Example::

  shape = (5, 5)
  data_np = np.array([1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20], dtype=np.int64)
  indices_np = np.array([1,2,3,4,0,2,3,4,0,1,3,4,0,1,2,4,0,1,2,3], dtype=np.int64)
  indptr_np = np.array([0,4,8,12,16,20], dtype=np.int64)
  a = mx.nd.sparse.csr_matrix((data_np, indices_np, indptr_np), shape=shape)
  a.asnumpy()
  seed = mx.nd.array([0,1,2,3,4], dtype=np.int64)
  out = mx.nd.contrib.dgl_csr_neighbor_uniform_sample(a, seed, num_args=2, num_hops=1, num_neighbor=2, max_num_vertices=5)

  out[0]
  [0 1 2 3 4 5]
  <NDArray 6 @cpu(0)>

  out[1].asnumpy()
  array([[ 0,  1,  0,  3,  0],
         [ 5,  0,  0,  7,  0],
         [ 9,  0,  0, 11,  0],
         [13,  0, 15,  0,  0],
         [17,  0, 19,  0,  0]])

  out[2]
  [0 0 0 0 0]
<NDArray 5 @cpu(0)>
)code" ADD_FILELINE)
.set_attr_parser(ParamParser<NeighborSampleParam>)
.set_num_inputs([](const NodeAttrs& attrs) {
  const NeighborSampleParam& params =
    nnvm::get<NeighborSampleParam>(attrs.parsed);
  return params.num_args;
})
.set_num_outputs([](const NodeAttrs& attrs) {
  const NeighborSampleParam& params =
    nnvm::get<NeighborSampleParam>(attrs.parsed);
  size_t num_subgraphs = params.num_args - 1;
  return num_subgraphs * 3;
})
.set_attr<FInferStorageType>("FInferStorageType", CSRNeighborUniformSampleStorageType)
.set_attr<nnvm::FInferShape>("FInferShape", CSRNeighborUniformSampleShape)
.set_attr<nnvm::FInferType>("FInferType", CSRNeighborUniformSampleType)
.set_attr<FComputeEx>("FComputeEx<cpu>", CSRNeighborUniformSampleComputeExCPU)
.add_argument("csr_matrix", "NDArray-or-Symbol", "csr matrix")
.add_argument("seed_arrays", "NDArray-or-Symbol[]", "seed vertices")
.set_attr<std::string>("key_var_num_args", "num_args")
.add_arguments(NeighborSampleParam::__FIELDS__());

/*
 * Operator: contrib_csr_neighbor_non_uniform_sample
 */
static void CSRNeighborNonUniformSampleComputeExCPU(const nnvm::NodeAttrs& attrs,
                                              const OpContext& ctx,
                                              const std::vector<NDArray>& inputs,
                                              const std::vector<OpReqType>& req,
                                              const std::vector<NDArray>& outputs) {
  const NeighborSampleParam& params =
    nnvm::get<NeighborSampleParam>(attrs.parsed);

  int num_subgraphs = inputs.size() - 2;
  CHECK_EQ(outputs.size(), 4 * num_subgraphs);

  const float* probability = inputs[1].data().dptr<float>();

#pragma omp parallel for
  for (int i = 0; i < num_subgraphs; i++) {
    float* sub_prob = outputs[i+2*num_subgraphs].data().dptr<float>();
    SampleSubgraph(inputs[0],                     // graph_csr
                   inputs[i + 2],                 // seed vector
                   outputs[i],                    // sample_id
                   outputs[i + 1*num_subgraphs],  // sub_csr
                   sub_prob,                      // sample_id_probability
                   outputs[i + 3*num_subgraphs],  // sample_id_layer
                   probability,
                   params.num_hops,
                   params.num_neighbor,
                   params.max_num_vertices);
  }
}

NNVM_REGISTER_OP(_contrib_dgl_csr_neighbor_non_uniform_sample)
.describe(R"code(This operator samples sub-graph from a csr graph via an
uniform probability. 
Example::

  shape = (5, 5)
  prob = mx.nd.array([0.9, 0.8, 0.2, 0.4, 0.1], dtype=np.float32)
  data_np = np.array([1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20], dtype=np.int64)
  indices_np = np.array([1,2,3,4,0,2,3,4,0,1,3,4,0,1,2,4,0,1,2,3], dtype=np.int64)
  indptr_np = np.array([0,4,8,12,16,20], dtype=np.int64)
  a = mx.nd.sparse.csr_matrix((data_np, indices_np, indptr_np), shape=shape)
  seed = mx.nd.array([0,1,2,3,4], dtype=np.int64)
  out = mx.nd.contrib.dgl_csr_neighbor_non_uniform_sample(a, prob, seed, num_args=3, num_hops=1, num_neighbor=2, max_num_vertices=5)

  out[0]
  [0 1 2 3 4 5]
  <NDArray 6 @cpu(0)>

  out[1].asnumpy()
  array([[ 0,  1,  2,  0,  0],
         [ 5,  0,  6,  0,  0],
         [ 9, 10,  0,  0,  0],
         [13, 14,  0,  0,  0],
         [ 0, 18, 19,  0,  0]])

  out[2]
  [0.9 0.8 0.2 0.4 0.1]
  <NDArray 5 @cpu(0)>

  out[3]
  [0 0 0 0 0]
  <NDArray 5 @cpu(0)>
)code" ADD_FILELINE)
.set_attr_parser(ParamParser<NeighborSampleParam>)
.set_num_inputs([](const NodeAttrs& attrs) {
  const NeighborSampleParam& params =
    nnvm::get<NeighborSampleParam>(attrs.parsed);
  return params.num_args;
})
.set_num_outputs([](const NodeAttrs& attrs) {
  const NeighborSampleParam& params =
    nnvm::get<NeighborSampleParam>(attrs.parsed);
  size_t num_subgraphs = params.num_args - 2;
  return num_subgraphs * 4;
})
.set_attr<FInferStorageType>("FInferStorageType", CSRNeighborNonUniformSampleStorageType)
.set_attr<nnvm::FInferShape>("FInferShape", CSRNeighborNonUniformSampleShape)
.set_attr<nnvm::FInferType>("FInferType", CSRNeighborNonUniformSampleType)
.set_attr<FComputeEx>("FComputeEx<cpu>", CSRNeighborNonUniformSampleComputeExCPU)
.add_argument("csr_matrix", "NDArray-or-Symbol", "csr matrix")
.add_argument("probability", "NDArray-or-Symbol", "probability vector")
.add_argument("seed_arrays", "NDArray-or-Symbol[]", "seed vertices")
.set_attr<std::string>("key_var_num_args", "num_args")
.add_arguments(NeighborSampleParam::__FIELDS__());

///////////////////////// Create induced subgraph ///////////////////////////

struct DGLSubgraphParam : public dmlc::Parameter<DGLSubgraphParam> {
  int num_args;
  bool return_mapping;
  DMLC_DECLARE_PARAMETER(DGLSubgraphParam) {
    DMLC_DECLARE_FIELD(num_args).set_lower_bound(2)
    .describe("Number of input arguments, including all symbol inputs.");
    DMLC_DECLARE_FIELD(return_mapping)
    .describe("Return mapping of vid and eid between the subgraph and the parent graph.");
  }
};  // struct DGLSubgraphParam

DMLC_REGISTER_PARAMETER(DGLSubgraphParam);

static bool DGLSubgraphStorageType(const nnvm::NodeAttrs& attrs,
                                   const int dev_mask,
                                   DispatchMode* dispatch_mode,
                                   std::vector<int> *in_attrs,
                                   std::vector<int> *out_attrs) {
  CHECK_EQ(in_attrs->at(0), kCSRStorage);
  for (size_t i = 1; i < in_attrs->size(); i++)
    CHECK_EQ(in_attrs->at(i), kDefaultStorage);

  bool success = true;
  *dispatch_mode = DispatchMode::kFComputeEx;
  for (size_t i = 0; i < out_attrs->size(); i++) {
    if (!type_assign(&(*out_attrs)[i], mxnet::kCSRStorage))
    success = false;
  }
  return success;
}

static bool DGLSubgraphShape(const nnvm::NodeAttrs& attrs,
                             std::vector<TShape> *in_attrs,
                             std::vector<TShape> *out_attrs) {
  const DGLSubgraphParam& params = nnvm::get<DGLSubgraphParam>(attrs.parsed);
  CHECK_EQ(in_attrs->at(0).ndim(), 2U);
  for (size_t i = 1; i < in_attrs->size(); i++)
    CHECK_EQ(in_attrs->at(i).ndim(), 1U);

  size_t num_g = params.num_args - 1;
  for (size_t i = 0; i < num_g; i++) {
    TShape gshape(2);
    gshape[0] = in_attrs->at(i + 1)[0];
    gshape[1] = in_attrs->at(i + 1)[0];
    out_attrs->at(i) = gshape;
  }
  for (size_t i = num_g; i < out_attrs->size(); i++) {
    TShape gshape(2);
    gshape[0] = in_attrs->at(i - num_g + 1)[0];
    gshape[1] = in_attrs->at(i - num_g + 1)[0];
    out_attrs->at(i) = gshape;
  }
  return true;
}

static bool DGLSubgraphType(const nnvm::NodeAttrs& attrs,
                            std::vector<int> *in_attrs,
                            std::vector<int> *out_attrs) {
  const DGLSubgraphParam& params = nnvm::get<DGLSubgraphParam>(attrs.parsed);
  size_t num_g = params.num_args - 1;
  for (size_t i = 0; i < num_g; i++) {
    CHECK_EQ(in_attrs->at(i + 1), mshadow::kInt64);
  }
  for (size_t i = 0; i < out_attrs->size(); i++) {
    out_attrs->at(i) = in_attrs->at(0);
  }
  return true;
}

class Bitmap {
  const size_t size = 1024 * 1024 * 4;
  const size_t mask = size - 1;
  std::vector<bool> map;

  size_t hash(dgl_id_t id) const {
    return id & mask;
  }
 public:
  Bitmap(const dgl_id_t *vid_data, int64_t len): map(size) {
    for (int64_t i = 0; i < len; ++i) {
      map[hash(vid_data[i])] = 1;
    }
  }

  bool test(dgl_id_t id) const {
    return map[hash(id)];
  }
};

/*
 * This uses a hashtable to check if a node is in the given node list.
 */
class HashTableChecker {
  std::unordered_map<dgl_id_t, dgl_id_t> oldv2newv;
  Bitmap map;

 public:
  HashTableChecker(const dgl_id_t *vid_data, int64_t len): map(vid_data, len) {
    oldv2newv.reserve(len);
    for (int64_t i = 0; i < len; ++i) {
      oldv2newv[vid_data[i]] = i;
    }
  }

  void CollectOnRow(const dgl_id_t col_idx[], const dgl_id_t eids[], size_t row_len,
                    std::vector<dgl_id_t> *new_col_idx,
                    std::vector<dgl_id_t> *orig_eids) {
    // TODO(zhengda) I need to make sure the column index in each row is sorted.
    for (size_t j = 0; j < row_len; ++j) {
      const dgl_id_t oldsucc = col_idx[j];
      const dgl_id_t eid = eids[j];
      Collect(oldsucc, eid, new_col_idx, orig_eids);
    }
  }

  void Collect(const dgl_id_t old_id, const dgl_id_t old_eid,
               std::vector<dgl_id_t> *col_idx,
               std::vector<dgl_id_t> *orig_eids) {
    if (!map.test(old_id))
      return;

    auto it = oldv2newv.find(old_id);
    if (it != oldv2newv.end()) {
      const dgl_id_t new_id = it->second;
      col_idx->push_back(new_id);
      if (orig_eids)
        orig_eids->push_back(old_eid);
    }
  }
};

static void GetSubgraph(const NDArray &csr_arr, const NDArray &varr,
                        const NDArray &sub_csr, const NDArray *old_eids) {
  const TBlob &data = varr.data();
  int64_t num_vertices = csr_arr.shape()[0];
  const size_t len = varr.shape()[0];
  const dgl_id_t *vid_data = data.dptr<dgl_id_t>();
  HashTableChecker def_check(vid_data, len);
  // check if varr is sorted.
  CHECK(std::is_sorted(vid_data, vid_data + len)) << "The input vertex list has to be sorted";

  // Collect the non-zero entries in from the original graph.
  std::vector<dgl_id_t> row_idx(len + 1);
  std::vector<dgl_id_t> col_idx;
  std::vector<dgl_id_t> orig_eids;
  col_idx.reserve(len * 50);
  orig_eids.reserve(len * 50);
  const dgl_id_t *eids = csr_arr.data().dptr<dgl_id_t>();
  const dgl_id_t *indptr = csr_arr.aux_data(csr::kIndPtr).dptr<dgl_id_t>();
  const dgl_id_t *indices = csr_arr.aux_data(csr::kIdx).dptr<dgl_id_t>();
  for (size_t i = 0; i < len; ++i) {
    const dgl_id_t oldvid = vid_data[i];
    CHECK_LT(oldvid, num_vertices) << "Vertex Id " << oldvid << " isn't in a graph of "
        << num_vertices << " vertices";
    size_t row_start = indptr[oldvid];
    size_t row_len = indptr[oldvid + 1] - indptr[oldvid];
    def_check.CollectOnRow(indices + row_start, eids + row_start, row_len,
                           &col_idx, old_eids == nullptr ? nullptr : &orig_eids);

    row_idx[i + 1] = col_idx.size();
  }

  TShape nz_shape(1);
  nz_shape[0] = col_idx.size();
  TShape indptr_shape(1);
  indptr_shape[0] = row_idx.size();

  // Store the non-zeros in a subgraph with edge attributes of new edge ids.
  sub_csr.CheckAndAllocData(nz_shape);
  sub_csr.CheckAndAllocAuxData(csr::kIdx, nz_shape);
  sub_csr.CheckAndAllocAuxData(csr::kIndPtr, indptr_shape);
  dgl_id_t *indices_out = sub_csr.aux_data(csr::kIdx).dptr<dgl_id_t>();
  dgl_id_t *indptr_out = sub_csr.aux_data(csr::kIndPtr).dptr<dgl_id_t>();
  std::copy(col_idx.begin(), col_idx.end(), indices_out);
  std::copy(row_idx.begin(), row_idx.end(), indptr_out);
  dgl_id_t *sub_eids = sub_csr.data().dptr<dgl_id_t>();
  for (int64_t i = 0; i < nz_shape[0]; i++)
    sub_eids[i] = i;

  // Store the non-zeros in a subgraph with edge attributes of old edge ids.
  if (old_eids) {
    old_eids->CheckAndAllocData(nz_shape);
    old_eids->CheckAndAllocAuxData(csr::kIdx, nz_shape);
    old_eids->CheckAndAllocAuxData(csr::kIndPtr, indptr_shape);
    dgl_id_t *indices_out = old_eids->aux_data(csr::kIdx).dptr<dgl_id_t>();
    dgl_id_t *indptr_out = old_eids->aux_data(csr::kIndPtr).dptr<dgl_id_t>();
    dgl_id_t *sub_eids = old_eids->data().dptr<dgl_id_t>();
    std::copy(col_idx.begin(), col_idx.end(), indices_out);
    std::copy(row_idx.begin(), row_idx.end(), indptr_out);
    std::copy(orig_eids.begin(), orig_eids.end(), sub_eids);
  }
}

static void DGLSubgraphComputeExCPU(const nnvm::NodeAttrs& attrs,
                                    const OpContext& ctx,
                                    const std::vector<NDArray>& inputs,
                                    const std::vector<OpReqType>& req,
                                    const std::vector<NDArray>& outputs) {
  const DGLSubgraphParam& params = nnvm::get<DGLSubgraphParam>(attrs.parsed);
  int num_g = params.num_args - 1;
#pragma omp parallel for
  for (int i = 0; i < num_g; i++) {
    const NDArray *old_eids = params.return_mapping ? &outputs[i + num_g] : nullptr;
    GetSubgraph(inputs[0], inputs[i + 1], outputs[i], old_eids);
  }
}

NNVM_REGISTER_OP(_contrib_dgl_subgraph)
.describe(R"code(This operator constructs an induced subgraph for
a given set of vertices from a graph. The operator accepts multiple
sets of vertices as input. For each set of vertices, it returns a pair
of CSR matrices if return_mapping is True: the first matrix contains edges
with new edge Ids, the second matrix contains edges with the original
edge Ids.

Example:

   .. code:: python

     x=[[1, 0, 0, 2],
       [3, 0, 4, 0],
       [0, 5, 0, 0],
       [0, 6, 7, 0]]
     v = [0, 1, 2]
     dgl_subgraph(x, v, return_mapping=True) =
       [[1, 0, 0],
        [2, 0, 3],
        [0, 4, 0]],
       [[1, 0, 0],
        [3, 0, 4],
        [0, 5, 0]]

)code" ADD_FILELINE)
.set_attr_parser(ParamParser<DGLSubgraphParam>)
.set_num_inputs([](const NodeAttrs& attrs) {
  const DGLSubgraphParam& params = nnvm::get<DGLSubgraphParam>(attrs.parsed);
  return params.num_args;
})
.set_num_outputs([](const NodeAttrs& attrs) {
  const DGLSubgraphParam& params = nnvm::get<DGLSubgraphParam>(attrs.parsed);
  int num_varray = params.num_args - 1;
  if (params.return_mapping)
    return num_varray * 2;
  else
    return num_varray;
})
.set_attr<nnvm::FListInputNames>("FListInputNames",
    [](const NodeAttrs& attrs) {
  const DGLSubgraphParam& params = nnvm::get<DGLSubgraphParam>(attrs.parsed);
  std::vector<std::string> names;
  names.reserve(params.num_args);
  names.emplace_back("graph");
  for (int i = 1; i < params.num_args; ++i)
    names.push_back("varray" + std::to_string(i - 1));
  return names;
})
.set_attr<FInferStorageType>("FInferStorageType", DGLSubgraphStorageType)
.set_attr<nnvm::FInferShape>("FInferShape", DGLSubgraphShape)
.set_attr<nnvm::FInferType>("FInferType", DGLSubgraphType)
.set_attr<FComputeEx>("FComputeEx<cpu>", DGLSubgraphComputeExCPU)
.set_attr<std::string>("key_var_num_args", "num_args")
.add_argument("graph", "NDArray-or-Symbol", "Input graph where we sample vertices.")
.add_argument("data", "NDArray-or-Symbol[]",
              "The input arrays that include data arrays and states.")
.add_arguments(DGLSubgraphParam::__FIELDS__());

///////////////////////// Edge Id ///////////////////////////

inline bool EdgeIDShape(const nnvm::NodeAttrs& attrs,
                        std::vector<TShape>* in_attrs,
                        std::vector<TShape>* out_attrs) {
  CHECK_EQ(in_attrs->size(), 3U);
  CHECK_EQ(out_attrs->size(), 1U);
  CHECK_EQ(in_attrs->at(1).ndim(), 1U);
  CHECK_EQ(in_attrs->at(2).ndim(), 1U);
  CHECK_EQ(in_attrs->at(1)[0], in_attrs->at(2)[0]);

  SHAPE_ASSIGN_CHECK(*out_attrs, 0, in_attrs->at(1));
  SHAPE_ASSIGN_CHECK(*in_attrs, 1, out_attrs->at(0));
  SHAPE_ASSIGN_CHECK(*in_attrs, 2, out_attrs->at(0));
  return out_attrs->at(0).ndim() != 0U && out_attrs->at(0).Size() != 0U;
}

inline bool EdgeIDType(const nnvm::NodeAttrs& attrs,
                       std::vector<int>* in_attrs,
                       std::vector<int>* out_attrs) {
  CHECK_EQ(in_attrs->size(), 3U);
  CHECK_EQ(out_attrs->size(), 1U);

  TYPE_ASSIGN_CHECK(*out_attrs, 0, in_attrs->at(0));
  TYPE_ASSIGN_CHECK(*in_attrs, 0, out_attrs->at(0));
  return out_attrs->at(0) != -1;
}

inline bool EdgeIDStorageType(const nnvm::NodeAttrs& attrs,
                              const int dev_mask,
                              DispatchMode* dispatch_mode,
                              std::vector<int>* in_attrs,
                              std::vector<int>* out_attrs) {
  CHECK_EQ(in_attrs->size(), 3U) << "Only works for 2d arrays";
  CHECK_EQ(out_attrs->size(), 1U);
  int& in_stype = in_attrs->at(0);
  int& out_stype = out_attrs->at(0);
  bool dispatched = false;
  if (!dispatched && in_stype == kCSRStorage) {
    // csr -> dns
    dispatched = storage_type_assign(&out_stype, kDefaultStorage,
                                     dispatch_mode, DispatchMode::kFComputeEx);
  }
  if (!dispatched) {
    LOG(ERROR) << "Cannot dispatch edge_id storage type, only works for csr matrices";
  }
  return dispatched;
}

struct edge_id_csr_forward {
  template<typename DType, typename IType, typename CType>
  MSHADOW_XINLINE static void Map(int i, DType* out_data, const DType* in_data,
                                  const IType* in_indices, const IType* in_indptr,
                                  const CType* u, const CType* v) {
    const int64_t target_row_id = static_cast<int64_t>(u[i]);
    const IType target_col_id = static_cast<IType>(v[i]);
    auto ptr = std::find(in_indices + in_indptr[target_row_id],
                         in_indices + in_indptr[target_row_id + 1], target_col_id);
    if (ptr == in_indices + in_indptr[target_row_id + 1]) {
      // does not exist in the range
      out_data[i] = DType(-1);
    } else {
      out_data[i] = *(in_data + (ptr - in_indices));
    }
  }
};

template<typename xpu>
void EdgeIDForwardCsrImpl(const OpContext& ctx,
                          const std::vector<NDArray>& inputs,
                          const OpReqType req,
                          const NDArray& output) {
  using namespace mshadow;
  using namespace mxnet_op;
  using namespace csr;
  if (req == kNullOp) return;
  CHECK_EQ(inputs.size(), 3U);
  CHECK_EQ(req, kWriteTo) << "EdgeID with CSR only supports kWriteTo";
  Stream<xpu> *s = ctx.get_stream<xpu>();
  const NDArray& u = inputs[1];
  const nnvm::dim_t out_elems = u.shape().Size();
  if (!inputs[0].storage_initialized()) {
    MSHADOW_TYPE_SWITCH(output.dtype(), DType, {
      Kernel<mxnet_op::op_with_req<mshadow_op::identity, kWriteTo>, xpu>::Launch(
        s, out_elems, output.data().dptr<DType>(), DType(-1));
    });
    return;
  }
  const NDArray& data = inputs[0];
  const TBlob& in_data = data.data();
  const TBlob& in_indices = data.aux_data(kIdx);
  const TBlob& in_indptr = data.aux_data(kIndPtr);
  const NDArray& v = inputs[2];

  CHECK_EQ(data.aux_type(kIdx), data.aux_type(kIndPtr))
    << "The dtypes of indices and indptr don't match";
  MSHADOW_TYPE_SWITCH(data.dtype(), DType, {
    MSHADOW_IDX_TYPE_SWITCH(data.aux_type(kIdx), IType, {
      MSHADOW_TYPE_SWITCH(u.dtype(), CType, {
        Kernel<edge_id_csr_forward, xpu>::Launch(
            s, out_elems, output.data().dptr<DType>(), in_data.dptr<DType>(),
            in_indices.dptr<IType>(), in_indptr.dptr<IType>(),
            u.data().dptr<CType>(), v.data().dptr<CType>());
      });
    });
  });
}

template<typename xpu>
void EdgeIDForwardEx(const nnvm::NodeAttrs& attrs,
                     const OpContext& ctx,
                     const std::vector<NDArray>& inputs,
                     const std::vector<OpReqType>& req,
                     const std::vector<NDArray>& outputs) {
  CHECK_EQ(inputs.size(), 3U);
  CHECK_EQ(outputs.size(), 1U);
  CHECK_EQ(req.size(), 1U);
  const auto in_stype = inputs[0].storage_type();
  const auto out_stype = outputs[0].storage_type();
  if (in_stype == kCSRStorage && out_stype == kDefaultStorage) {
    EdgeIDForwardCsrImpl<xpu>(ctx, inputs, req[0], outputs[0]);
  } else {
    LogUnimplementedOp(attrs, ctx, inputs, req, outputs);
  }
}

NNVM_REGISTER_OP(_contrib_edge_id)
.describe(R"code(This operator implements the edge_id function for a graph
stored in a CSR matrix (the value of the CSR stores the edge Id of the graph).
output[i] = input[u[i], v[i]] if there is an edge between u[i] and v[i]],
otherwise output[i] will be -1. Both u and v should be 1D vectors.

Example:

   .. code:: python

      x = [[ 1, 0, 0 ],
           [ 0, 2, 0 ],
           [ 0, 0, 3 ]]
      u = [ 0, 0, 1, 1, 2, 2 ]
      v = [ 0, 1, 1, 2, 0, 2 ]
      edge_id(x, u, v) = [ 1, -1, 2, -1, -1, 3 ]

The storage type of ``edge_id`` output depends on storage types of inputs
  - edge_id(csr, default, default) = default
  - default and rsp inputs are not supported

)code" ADD_FILELINE)
.set_num_inputs(3)
.set_num_outputs(1)
.set_attr<nnvm::FListInputNames>("FListInputNames",
  [](const NodeAttrs& attrs) {
    return std::vector<std::string>{"data", "u", "v"};
  })
.set_attr<nnvm::FInferShape>("FInferShape", EdgeIDShape)
.set_attr<nnvm::FInferType>("FInferType", EdgeIDType)
.set_attr<FInferStorageType>("FInferStorageType", EdgeIDStorageType)
.set_attr<FComputeEx>("FComputeEx<cpu>", EdgeIDForwardEx<cpu>)
.add_argument("data", "NDArray-or-Symbol", "Input ndarray")
.add_argument("u", "NDArray-or-Symbol", "u ndarray")
.add_argument("v", "NDArray-or-Symbol", "v ndarray");

///////////////////////// DGL Adjacency ///////////////////////////

inline bool DGLAdjacencyShape(const nnvm::NodeAttrs& attrs,
                              std::vector<TShape>* in_attrs,
                              std::vector<TShape>* out_attrs) {
  CHECK_EQ(in_attrs->size(), 1U);
  CHECK_EQ(out_attrs->size(), 1U);

  SHAPE_ASSIGN_CHECK(*out_attrs, 0, in_attrs->at(0));
  SHAPE_ASSIGN_CHECK(*in_attrs, 0, out_attrs->at(0));
  return out_attrs->at(0).ndim() != 0U && out_attrs->at(0).Size() != 0U;
}

inline bool DGLAdjacencyType(const nnvm::NodeAttrs& attrs,
                             std::vector<int>* in_attrs,
                             std::vector<int>* out_attrs) {
  CHECK_EQ(in_attrs->size(), 1U);
  CHECK_EQ(out_attrs->size(), 1U);
  CHECK_EQ(in_attrs->at(0), mshadow::kInt64);
  TYPE_ASSIGN_CHECK(*out_attrs, 0, mshadow::kFloat32);
  return out_attrs->at(0) != -1;
}

inline bool DGLAdjacencyStorageType(const nnvm::NodeAttrs& attrs,
                                    const int dev_mask,
                                    DispatchMode* dispatch_mode,
                                    std::vector<int>* in_attrs,
                                    std::vector<int>* out_attrs) {
  CHECK_EQ(in_attrs->size(), 1U) << "Only works for 2d arrays";
  CHECK_EQ(out_attrs->size(), 1U);
  int& out_stype = out_attrs->at(0);
  bool dispatched = storage_type_assign(&out_stype, kCSRStorage,
                                        dispatch_mode, DispatchMode::kFComputeEx);
  if (!dispatched) {
    LOG(ERROR) << "Cannot dispatch output storage type: " << common::stype_string(out_stype)
        << ". dgl_adjacency only works for csr matrices";
  }
  return dispatched;
}

NNVM_REGISTER_OP(_contrib_dgl_adjacency)
.describe(R"code(This operator converts a CSR matrix whose values are edge Ids
to an adjacency matrix whose values are ones. The output CSR matrix always has
the data value of float32.

Example:

  x = [[ 1, 0, 0 ],
       [ 0, 2, 0 ],
       [ 0, 0, 3 ]]
  dgl_adjacency(x) =
      [[ 1, 0, 0 ],
       [ 0, 1, 0 ],
       [ 0, 0, 1 ]]

)code" ADD_FILELINE)
.set_num_inputs(1)
.set_num_outputs(1)
.set_attr<nnvm::FListInputNames>("FListInputNames",
  [](const NodeAttrs& attrs) {
    return std::vector<std::string>{"data"};
  })
.set_attr<nnvm::FInferShape>("FInferShape", DGLAdjacencyShape)
.set_attr<nnvm::FInferType>("FInferType", DGLAdjacencyType)
.set_attr<FInferStorageType>("FInferStorageType", DGLAdjacencyStorageType)
.set_attr<FComputeEx>("FComputeEx<cpu>", DGLAdjacencyForwardEx<cpu>)
.add_argument("data", "NDArray-or-Symbol", "Input ndarray");

}  // namespace op
}  // namespace mxnet