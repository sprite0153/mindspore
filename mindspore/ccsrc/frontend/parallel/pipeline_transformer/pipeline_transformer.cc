/**
 * Copyright 2020 Huawei Technologies Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <unordered_map>
#include <set>
#include <vector>
#include <string>
#include <utility>
#include <algorithm>
#include <memory>
#include "frontend/parallel/pipeline_transformer/pipeline_transformer.h"
#include "frontend/parallel/auto_parallel/graph_costmodel.h"
#include "frontend/parallel/ops_info/ops_utils.h"
#include "frontend/parallel/group_manager.h"
#include "frontend/parallel/context.h"
#include "frontend/parallel/step_parallel.h"
#include "frontend/parallel/node_check.h"
#include "frontend/parallel/graph_util/node_info.h"
#include "frontend/parallel/graph_util/pipeline_split_utils.h"
#include "ir/anf.h"
#include "ir/graph_utils.h"
#include "base/core_ops.h"
#include "utils/comm_manager.h"
#include "utils/ms_context.h"
#include "mindspore/core/utils/parallel_node_check.h"

namespace mindspore {
namespace parallel {
static std::unordered_map<AnfNodePtr, std::set<int64_t>> parameter_color_map;
// map<rank, tag>
static std::unordered_map<int64_t, int64_t> send_tag_map;
static std::unordered_map<int64_t, int64_t> recv_tag_map;
const std::set<PrimitivePtr> WHITE_LIST = {prim::kPrimCast, prim::kPrimTupleGetItem};

static bool IsInWhiteList(const CNodePtr &cnode) {
  for (auto &prim : WHITE_LIST) {
    if (IsPrimitiveCNode(cnode, prim)) {
      return true;
    }
  }
  return false;
}

void PipelineTransformer::MainGraph() {
  if (!root_->has_flag(TRAINING)) {
    main_graph_ = root_;
    return;
  }
  for (auto &fg : manager_->func_graphs()) {
    for (auto &node : fg->nodes()) {
      if (IsPrimitiveCNode(node, prim::kPrimVirtualDataset)) {
        main_graph_ = fg;
        main_graph_->set_flag(MAIN_GRAPH, true);
        virtual_dataset_ = node;
        return;
      }
    }
  }
  MS_LOG(EXCEPTION) << "Can't find main graph, possible reason is can't find virtual dataset.";
}

ValuePtr PipelineTransformer::SetMicroBatch(const AnfNodePtr &node, int64_t micro_size) {
  if (!IsPrimitiveCNode(node, prim::kPrimStridedSlice)) {
    MS_LOG(EXCEPTION) << "Can't find MicroBatch information.";
  }
  auto cnode = node->cast<CNodePtr>();
  auto value = GetValueNode(cnode->input(2));
  MS_EXCEPTION_IF_NULL(value);
  auto tuple = GetValue<std::vector<int64_t>>(value);
  auto input_shape = GetNodeShape(cnode->input(1)).at(0);
  int64_t micro = tuple.at(0) * micro_size / input_shape.at(0);
  cnode->AddPrimalAttr(MICRO, MakeValue(micro));
  cnode->AddPrimalAttr(PIPELINE_BEGIN, MakeValue(micro));
  return MakeValue(micro);
}

bool PipelineTransformer::NeedGrad(const CNodePtr &cnode) {
  for (auto &input : cnode->inputs()) {
    if (input->isa<Parameter>() && ParameterRequireGrad(input)) {
      return true;
    }
    if (IsPrimitiveCNode(input, prim::kPrimLoad)) {
      auto load = input->cast<CNodePtr>();
      if (load->input(1)->isa<Parameter>() && ParameterRequireGrad(load->input(1))) {
        return true;
      }
    }
  }
  return false;
}

void PipelineTransformer::LabelParameterStart(const FuncGraphPtr &graph) {
  auto orders = graph->GetOrderedCnodes();
  for (auto &node : orders) {
    auto cnode = node->cast<CNodePtr>();
    MS_EXCEPTION_IF_NULL(cnode);
    if (IsValueNode<FuncGraph>(cnode->input(0))) {
      auto sub_graph = GetValueNode<FuncGraphPtr>(cnode->input(0));
      return LabelParameterStart(sub_graph);
    }
    if (!IsPipelineCareNode(cnode)) {
      continue;
    }
    if (NeedGrad(cnode)) {
      auto prim = GetCNodePrimitive(cnode);
      prim->AddAttr(PARAMETER_START, MakeValue(0));
      return;
    }
  }
}

void PipelineTransformer::LabelMicroBatch() {
  if (!root_->has_flag(TRAINING)) {
    return;
  }
  MS_EXCEPTION_IF_NULL(main_graph_);
  LabelParameterStart(main_graph_);
  MS_EXCEPTION_IF_NULL(virtual_dataset_);
  auto node_user_map = manager_->node_users();
  auto node_users = node_user_map[virtual_dataset_];
  for (auto &node_user : node_users) {
    if (IsPrimitiveCNode(node_user.first, prim::kPrimTupleGetItem)) {
      auto data_users = manager_->node_users()[node_user.first];
      auto node_first = data_users.front().first;
      if (!IsPrimitiveCNode(node_first, prim::kPrimStridedSlice)) {
        data_users.clear();
        data_users = node_user_map[node_first];
      }
      auto micro_size = int64_t(data_users.size());
      micro_size_ = micro_size;
      MS_LOG(INFO) << "Micro Size is: " << micro_size;
      for (auto &data_user : data_users) {
        auto micro = SetMicroBatch(data_user.first, micro_size);
        SetStridedSliceStrategy(data_user.first);
        auto cnode = data_user.first->cast<CNodePtr>();
        BroadCastMicroBatch(cnode, &node_user_map, micro);
      }
    }
  }
}

void PipelineTransformer::CreateForwardGroup() {
  std::vector<int64_t> rank_list;
  auto rank_id = g_device_manager->global_rank();
  auto stage_id = g_device_manager->stage_id();
  auto stage_num = g_device_manager->stage_num();
  for (int64_t i = 0; i < stage_num; ++i) {
    rank_list.push_back(rank_id + per_stage_rank_num_ * (i - stage_id));
  }
  auto dev_list = g_device_manager->CreateDeviceListByRankList(rank_list);
  auto g = g_device_manager->CreateGroup(rank_list);
  auto g_back_name = g.name() + BACKWARD;
  auto g_back = g_device_manager->CreateGroup(g_back_name, dev_list);
  group_.push_back(g.name());
  group_.push_back(g_back.name());
}

void PipelineTransformer::Coloring() {
  auto need_coloring = true;
  std::set<int64_t> stage_set;
  while (need_coloring) {
    need_coloring = false;
    for (auto &fg : manager_->func_graphs()) {
      if (fg == root_ && root_->has_flag(TRAINING)) {
        continue;
      }
      auto value_nodes = fg->value_nodes();
      for (auto &value_pair : value_nodes) {
        auto node = value_pair.first;
        if (!IsValueNode<FuncGraph>(node)) {
          continue;
        }
        auto graph = GetValueNode<FuncGraphPtr>(node);
        if (graph->stage() == -1) {
          continue;
        }
        stage_set.insert(graph->stage());
        auto node_users = manager_->node_users()[node];
        for (auto &user_pair : node_users) {
          auto user_node = user_pair.first->cast<CNodePtr>();
          user_node->set_stage(graph->stage());
          auto user_node_graph = user_node->func_graph();
          if (graph->stage() == stage_ && user_node_graph->stage() == -1) {
            user_node_graph->set_stage(graph->stage());
            need_coloring = true;
          }
        }
      }
    }
  }
  MS_EXCEPTION_IF_NULL(g_device_manager);
  auto stage_num = g_device_manager->stage_num();
  if (SizeToLong(stage_set.size()) != stage_num) {
    MS_LOG(EXCEPTION) << "Stage num is " << stage_num << " is not equal to stage used: " << stage_set.size();
  }
}

void PipelineTransformer::BroadCastColoring() {
  auto need_coloring = true;
  while (need_coloring) {
    need_coloring = false;
    auto all_nodes = main_graph_->nodes();
    auto node_users = manager_->node_users();
    for (auto &node : all_nodes) {
      if (!node->isa<CNode>() || node->stage() == -1) {
        continue;
      }
      auto stage = node->stage();
      for (auto &user_pair : node_users[node]) {
        auto user_node = user_pair.first->cast<CNodePtr>();
        auto user_node_stage = user_node->stage();
        if (stage > user_node_stage) {
          user_node->set_stage(stage);
          need_coloring = true;
        }
      }
    }
  }
}

bool PipelineTransformer::IsPipelineCareNode(const CNodePtr &cnode) {
  MS_EXCEPTION_IF_NULL(cnode);
  auto prim = GetValueNode<PrimitivePtr>(cnode->input(0));
  if (!prim) {
    return false;
  }
  if (IsInWhiteList(cnode)) {
    return false;
  }
  if (IsInParallelBlackList(prim)) {
    MS_LOG(INFO) << "PipelineSplit don't care node:" << prim->name();
    return false;
  }
  return true;
}

CNodePtr PipelineTransformer::GraphOutNode(const AnfNodePtr &node, int tuple_index) {
  auto cnode = node->cast<CNodePtr>();
  MS_EXCEPTION_IF_NULL(cnode);
  if (IsPrimitiveCNode(node, prim::kPrimDepend)) {
    return GraphOutNode(cnode->input(1), tuple_index);
  }
  if (IsPrimitiveCNode(node, prim::kPrimMakeTuple)) {
    return cnode->input(IntToSize(tuple_index) + 1)->cast<CNodePtr>();
  }
  return cnode;
}

OperatorInfoPtr PipelineTransformer::CreateOpInfo(const CNodePtr &cnode, int tuple_index = 0) {
  MS_EXCEPTION_IF_NULL(cnode);
  auto temp_node = cnode;
  if (IsValueNode<FuncGraph>(cnode->input(0))) {
    auto output = GetValueNode<FuncGraphPtr>(cnode->input(0))->output();
    MS_EXCEPTION_IF_NULL(output);
    temp_node = GraphOutNode(output, tuple_index);
  }
  if (!IsPipelineCareNode(temp_node)) {
    MS_LOG(EXCEPTION) << "Node: " << temp_node->DebugString() << " is not a Pipeline Care Node.";
  }
  if (IsPrimitiveCNode(temp_node, prim::kPrimVirtualDataset)) {
    SetVirtualDatasetStrategy(temp_node);
  }
  auto shape_list = ExtractShape(temp_node);
  if (shape_list.empty()) {
    MS_LOG(EXCEPTION) << "Node: " << temp_node->DebugString() << " failed to extract shape.";
  }
  auto prim = GetValueNode<PrimitivePtr>(temp_node->input(0));
  MS_EXCEPTION_IF_NULL(prim);
  if (prim->name() == RESHAPE) {
    MS_LOG(EXCEPTION) << "Reshape op can't be a border. node:" << temp_node->DebugString();
  }
  auto attrs = prim->attrs();
  auto op_info = OperatorInstance(prim, attrs, shape_list);
  auto &inputs = temp_node->inputs();
  std::vector<ValuePtr> input_value;
  for (size_t index = 1; index < inputs.size(); ++index) {
    if (inputs[index]->isa<ValueNode>()) {
      input_value.push_back(GetValueNode(inputs[index]));
    } else {
      input_value.emplace_back(nullptr);
    }
  }
  op_info->set_input_value(input_value);
  op_info->set_outputs_dtype(temp_node->Type());
  op_info->set_cnode(temp_node);
  StrategyPtr strategy = nullptr;
  if (!StrategyFound(attrs)) {
    strategy = GenerateBatchParallelStrategy(op_info, prim);
  } else {
    strategy = ExtractStrategy(attrs[STRATEGY]);
  }
  MS_EXCEPTION_IF_NULL(strategy);
  if (op_info->Init(strategy) == FAILED) {
    MS_LOG(EXCEPTION) << "operator: " << prim->name() << " init failed.";
  }
  return op_info;
}

std::pair<OperatorInfoPtr, int> PipelineTransformer::GetOpInfo(const AnfNodePtr &node) {
  MS_EXCEPTION_IF_NULL(node);
  auto cnode = node->cast<CNodePtr>();
  MS_EXCEPTION_IF_NULL(cnode);
  // Handle Cast and TupleGetitem situation
  int tensor_info_index = 0;
  OperatorInfoPtr op_info;
  if (IsPrimitiveCNode(node, prim::kPrimReceive)) {
    op_info = node->user_data<OperatorInfo>();
  } else {
    if (IsPrimitiveCNode(node, prim::kPrimCast)) {
      cnode = cnode->input(1)->cast<CNodePtr>();
    } else if (IsPrimitiveCNode(cnode, prim::kPrimTupleGetItem)) {
      tensor_info_index = LongToInt(GetTupleGetItemIndex(cnode));
      cnode = cnode->input(1)->cast<CNodePtr>();
    }
    // Create OperatorInfo to get slice_shape for send/recv
    MS_EXCEPTION_IF_NULL(cnode);
    op_info = CreateOpInfo(cnode, tensor_info_index);
  }
  return std::make_pair(op_info, tensor_info_index);
}

std::pair<OperatorInfoPtr, int> PipelineTransformer::GetParameterPair(const AnfNodePtr &node) {
  MS_EXCEPTION_IF_NULL(node);
  auto node_users_map = manager_->node_users();
  auto node_users = node_users_map[node];
  for (auto &node_user : node_users) {
    auto load = node_user.first->cast<CNodePtr>();
    if (IsPrimitiveCNode(load, prim::kPrimLoad)) {
      node_users = node_users_map[load];
      break;
    }
  }
  for (auto &user_pair : node_users) {
    auto user_node = user_pair.first->cast<CNodePtr>();
    MS_EXCEPTION_IF_NULL(user_node);
    auto user_node_graph = user_node->func_graph();
    MS_EXCEPTION_IF_NULL(user_node_graph);
    if (user_node_graph->stage() == -1) {
      continue;
    }
    auto care_node = user_node;
    auto index = user_pair.second;
    if (IsValueNode<FuncGraph>(user_node->input(0))) {
      auto graph = GetValueNode<FuncGraphPtr>(user_node->input(0));
      auto temp_params = graph->parameters();
      if (temp_params.size() < IntToSize(user_pair.second)) {
        MS_LOG(EXCEPTION) << "parameter:" << node->DebugString() << " out of graph: " << graph->ToString()
                          << "'s range.";
      }
      auto temp_param = temp_params[user_pair.second - 1];
      auto temp_users = node_users_map[temp_param];
      for (auto &temp_user : temp_users) {
        auto load_temp = temp_user.first->cast<CNodePtr>();
        if (IsPrimitiveCNode(load_temp, prim::kPrimLoad)) {
          temp_users = node_users_map[load_temp];
          break;
        }
      }
      for (auto &temp_pair : temp_users) {
        auto temp_cnode = temp_pair.first->cast<CNodePtr>();
        if (!IsPipelineCareNode(temp_cnode)) {
          continue;
        }
        care_node = temp_cnode;
        index = temp_pair.second;
        break;
      }
    }
    if (!IsPipelineCareNode(care_node)) {
      continue;
    }
    auto op_info = CreateOpInfo(care_node);
    return std::make_pair(op_info, index - 1);
  }
  return std::make_pair(nullptr, 0);
}

std::vector<AnfNodePtr> PipelineTransformer::HandleSharedParameter() {
  auto parameters = root_->parameters();
  std::vector<AnfNodePtr> make_tuple_input = {NewValueNode(prim::kPrimMakeTuple)};
  std::vector<AnfNodePtr> recvs = {};
  for (auto &parameter : parameters) {
    auto parameter_stage = parameter_color_map[parameter];
    if (parameter_stage.size() <= 1) {
      continue;
    }
    auto users = manager_->node_users()[parameter];
    for (auto &user : users) {
      auto node = user.first;
      auto cnode = node->cast<CNodePtr>();
      auto graph = node->func_graph();
      if (IsValueNode<FuncGraph>(cnode->input(0))) {
        graph = GetValueNode<FuncGraphPtr>(cnode->input(0));
      }
      if (graph == root_ || graph->stage() == -1 || !parameter_stage.count(stage_)) {
        continue;
      }
      auto micro = cnode->GetPrimalAttr(MICRO);
      if (!micro) {
        MS_LOG(INFO) << "parameter: " << parameter->ToString() << " doesn't have micro batch";
        micro = MakeValue(int64_t(0));
      }
      auto user_stage = node->stage();
      if (stage_ == *parameter_stage.begin()) {
        if (graph->stage() == stage_) {
          continue;
        }
        if (Reuse(parameter, user_stage, make_tuple_input, DEST_RANK)) {
          continue;
        }
        auto send_out = InsertSend(main_graph_, parameter, user_stage, stage_, micro);
        make_tuple_input.push_back(send_out.depend);
      } else {
        auto receive = Reuse(parameter, *parameter_stage.begin(), recvs, SRC_RANK);
        if (receive) {
          manager_->SetEdge(node, user.second, receive);
        } else {
          auto recv = InsertReceive(main_graph_, parameter, node, user.second, stage_, *parameter_stage.begin(), micro);
          recvs.push_back(recv);
        }
      }
    }
  }
  return make_tuple_input;
}

void PipelineTransformer::ParameterColoring() {
  auto parameters = root_->parameters();
  for (auto &parameter : parameters) {
    auto users = manager_->node_users()[parameter];
    std::set<int64_t> parameter_stage;
    for (auto &user : users) {
      auto node = user.first->cast<CNodePtr>();
      auto graph = node->func_graph();
      if (IsValueNode<FuncGraph>(node->input(0))) {
        graph = GetValueNode<FuncGraphPtr>(node->input(0));
      }
      if (graph != root_ && graph->stage() != -1) {
        parameter_stage.insert(graph->stage());
        parameter->set_stage(graph->stage());
      }
    }
    auto param_info = parameter->cast<ParameterPtr>()->param_info();
    if (!param_info) {
      parameter_color_map[parameter] = parameter_stage;
      continue;
    }
    MS_EXCEPTION_IF_NULL(param_info);
    auto requires_grad = param_info->requires_grad();
    if (*parameter_stage.begin() == stage_ && !virtual_param_ && requires_grad) {
      virtual_param_ = parameter;
    }
    parameter_color_map[parameter] = parameter_stage;
  }
}

static std::pair<ValueListPtr, TypePtr> GetShapeType(const AnfNodePtr &node, const Shape &shape) {
  TypePtr type;
  auto cnode = node->cast<CNodePtr>();
  if (cnode != nullptr && IsValueNode<FuncGraph>(cnode->input(0))) {
    auto graph = GetValueNode<FuncGraphPtr>(cnode->input(0));
    auto graph_output = graph->output();
    type = graph_output->Type();
  } else {
    type = node->Type();
  }
  MS_EXCEPTION_IF_NULL(type);
  std::vector<ValuePtr> element;
  std::transform(shape.begin(), shape.end(), std::back_inserter(element), [](int elem) { return MakeValue(elem); });
  auto shape_list = std::make_shared<ValueList>(element);
  auto tensor_type = type->cast<mindspore::TensorTypePtr>();
  MS_EXCEPTION_IF_NULL(tensor_type);
  auto dtype = tensor_type->element();
  MS_EXCEPTION_IF_NULL(dtype);
  return std::make_pair(shape_list, dtype);
}

AnfNodePtr PipelineTransformer::FindPipelineCareNode(const AnfNodePtr &node) {
  MS_EXCEPTION_IF_NULL(node);
  auto cnode = node->cast<CNodePtr>();
  MS_EXCEPTION_IF_NULL(cnode);
  if (IsValueNode<FuncGraph>(cnode->input(0))) {
    auto graph = GetValueNode<FuncGraphPtr>(cnode->input(0));
    auto output = graph->output();
    MS_EXCEPTION_IF_NULL(output);
    if (output->isa<Parameter>()) {
      auto parameters = graph->parameters();
      auto pos_iter = std::find(parameters.begin(), parameters.end(), output);
      auto pos = std::distance(parameters.begin(), pos_iter);
      return FindPipelineCareNode(cnode->input(pos + 1));
    }
    cnode = output->cast<CNodePtr>();
    MS_EXCEPTION_IF_NULL(cnode);
  }
  if (IsPrimitiveCNode(cnode, prim::kPrimDepend)) {
    return FindPipelineCareNode(cnode->input(1));
  }
  if (IsInWhiteList(cnode)) {
    return cnode->cast<AnfNodePtr>();
  }
  if (!IsPipelineCareNode(cnode)) {
    MS_LOG(EXCEPTION) << "Only PipelineSplit cared node can be a border."
                      << " border node: " << cnode->DebugString();
  }
  return cnode->cast<AnfNodePtr>();
}

SendAttr PipelineTransformer::InsertSend(const FuncGraphPtr &graph, const AnfNodePtr &parameter,
                                         int64_t user_node_stage, int64_t node_stage, const ValuePtr &value) {
  auto dest_rank = global_rank_ + (user_node_stage - node_stage) * per_stage_rank_num_;
  int64_t send_tag;
  if (send_tag_map.find(dest_rank) != send_tag_map.end()) {
    send_tag = send_tag_map[dest_rank] + 1;
    send_tag_map[dest_rank] += 1;
  } else {
    send_tag = 0;
    send_tag_map[dest_rank] = 0;
  }
  Attr attr_tag = std::make_pair(SR_TAG, MakeValue(send_tag));
  Attr attr_rank = std::make_pair(DEST_RANK, MakeValue(user_node_stage));
  Attr attr_group = std::make_pair(GROUP, MakeValue(group_[0]));
  Attr attr_group_back = std::make_pair(GROUP_BACK, MakeValue(group_[1]));
  OperatorAttrs attrs = {attr_tag, attr_rank, attr_group, attr_group_back};
  auto send_op = CreatOpInstance(attrs, SEND, SEND);
  auto send_node = NewValueNode(send_op);
  auto prim = GetValueNode<PrimitivePtr>(send_node);
  std::pair<OperatorInfoPtr, int> op_info_pair;
  AnfNodePtr care_node;
  TensorInfo tensor_info;
  if (parameter->isa<Parameter>()) {
    op_info_pair = GetParameterPair(parameter);
    tensor_info = op_info_pair.first->inputs_tensor_info().at(IntToSize(op_info_pair.second));
  } else {
    if (IsPrimitiveCNode(parameter, prim::kPrimCast)) {
      auto parameter_cnode = parameter->cast<CNodePtr>();
      care_node = FindPipelineCareNode(parameter_cnode->input(1));
    } else {
      care_node = FindPipelineCareNode(parameter);
    }
    if (care_node->isa<Parameter>()) {
      op_info_pair = GetParameterPair(care_node);
      tensor_info = op_info_pair.first->inputs_tensor_info().at(IntToSize(op_info_pair.second));
    } else {
      op_info_pair = GetOpInfo(care_node);
      tensor_info = op_info_pair.first->outputs_tensor_info().at(IntToSize(op_info_pair.second));
    }
  }
  auto index = op_info_pair.second;
  auto op_info = op_info_pair.first;
  auto slice_shape = tensor_info.slice_shape();
  auto shape_type_pair = GetShapeType(parameter, slice_shape);
  prim->set_attr(SHAPE, shape_type_pair.first);
  prim->set_attr(DTYPE, shape_type_pair.second);
  std::vector<AnfNodePtr> send_input = {send_node, parameter};
  auto send = main_graph_->NewCNode(send_input);
  if (!parameter->isa<Parameter>() && care_node != nullptr && !care_node->isa<Parameter>()) {
    send->AddPrimalAttr(PIPELINE_END, value);
  } else {
    send->AddPrimalAttr(PIPELINE_PARAM, value);
    send->AddPrimalAttr(MICRO, value);
    send->set_user_data<OperatorInfo>(op_info);
    send->AddPrimalAttr(PARAM_INDEX, MakeValue(index));
  }
  OperatorAttrs depend_attrs;
  auto depend_op = CreatOpInstance(depend_attrs, DEPEND, DEPEND);
  std::vector<AnfNodePtr> depend_input = {NewValueNode(depend_op), parameter, send};
  auto depend = main_graph_->NewCNode(depend_input);
  auto abstract = parameter->abstract();
  if (care_node) {
    abstract = care_node->abstract();
  }
  depend->set_abstract(abstract);
  send->set_abstract(abstract);
  SendAttr send_out = {shape_type_pair.first, shape_type_pair.second, depend};
  return send_out;
}

AnfNodePtr PipelineTransformer::InsertReceive(const FuncGraphPtr &graph, const AnfNodePtr &node,
                                              const AnfNodePtr &use_node, int index, int64_t user_node_stage,
                                              int64_t node_stage, const ValuePtr &value) {
  auto src_rank = global_rank_ - (user_node_stage - node_stage) * per_stage_rank_num_;
  int64_t recv_tag;
  if (recv_tag_map.find(src_rank) != recv_tag_map.end()) {
    recv_tag = recv_tag_map[src_rank] + 1;
    recv_tag_map[src_rank] += 1;
  } else {
    recv_tag = 0;
    recv_tag_map[src_rank] = 0;
  }
  Attr attr_tag = std::make_pair(SR_TAG, MakeValue(recv_tag));
  Attr attr_rank = std::make_pair(SRC_RANK, MakeValue(node_stage));
  std::pair<OperatorInfoPtr, int> op_info_pair;
  bool is_param = true;
  TensorInfo tensor_info;
  if (node->isa<Parameter>()) {
    op_info_pair = GetParameterPair(node);
    tensor_info = op_info_pair.first->inputs_tensor_info().at(IntToSize(op_info_pair.second));
  } else {
    auto care_node = FindPipelineCareNode(node);
    if (care_node->isa<Parameter>()) {
      op_info_pair = GetParameterPair(care_node);
      tensor_info = op_info_pair.first->inputs_tensor_info().at(IntToSize(op_info_pair.second));
    } else {
      op_info_pair = GetOpInfo(care_node);
      tensor_info = op_info_pair.first->outputs_tensor_info().at(IntToSize(op_info_pair.second));
      is_param = false;
    }
  }
  auto tensor_layout = tensor_info.tensor_layout();
  Shape slice_shape = tensor_info.slice_shape();
  auto shape_type_pair = GetShapeType(node, slice_shape);
  Attr attr_shape = std::make_pair(SHAPE, shape_type_pair.first);
  Attr attr_dtype = std::make_pair(DTYPE, shape_type_pair.second);
  Attr attr_group = std::make_pair(GROUP, MakeValue(group_[0]));
  Attr attr_group_back = std::make_pair(GROUP_BACK, MakeValue(group_[1]));
  OperatorAttrs attrs = {attr_tag, attr_rank, attr_shape, attr_dtype, attr_group, attr_group_back};
  auto recv_op = CreatOpInstance(attrs, RECEIVE, RECEIVE);
  std::vector<AnfNodePtr> recv_input;
  if (node->isa<Parameter>()) {
    recv_input = {NewValueNode(recv_op), node};
  } else {
    recv_input = {NewValueNode(recv_op), virtual_param_};
  }
  auto recv = graph->NewCNode(recv_input);
  if (is_param) {
    recv->set_user_data<AnfNode>(PIPELINE_PARAM, node);
    recv->AddPrimalAttr(PIPELINE_PARAM, value);
  } else {
    recv->AddPrimalAttr(PIPELINE_BEGIN, value);
  }
  recv->AddPrimalAttr(MICRO, value);
  auto node_abstract = node->abstract();
  if (node->isa<CNode>()) {
    auto cnode = node->cast<CNodePtr>();
    MS_EXCEPTION_IF_NULL(cnode);
    if (IsValueNode<FuncGraph>(cnode->input(0))) {
      auto output = GetValueNode<FuncGraphPtr>(cnode->input(0))->output();
      MS_EXCEPTION_IF_NULL(output);
      node_abstract = output->abstract();
    }
  }
  MS_EXCEPTION_IF_NULL(node_abstract);
  recv->set_abstract(node_abstract);
  if (node->isa<Parameter>()) {
    BaseShapePtr parallel_shape = std::make_shared<abstract::Shape>(slice_shape);
    auto abstract_clone = node->abstract()->Clone();
    MS_EXCEPTION_IF_NULL(abstract_clone);
    abstract_clone->set_shape(parallel_shape);
    node->set_abstract(abstract_clone);
    node->set_user_data<TensorLayout>(std::make_shared<TensorLayout>(tensor_layout));
  }
  recv->set_user_data<TensorLayout>(std::make_shared<TensorLayout>(tensor_layout));
  recv->set_user_data<OperatorInfo>(op_info_pair.first);

  manager_->SetEdge(use_node, index, recv);
  return recv;
}

AnfNodePtr PipelineTransformer::Reuse(const AnfNodePtr &node, int64_t stage, const std::vector<AnfNodePtr> &out_input,
                                      const std::string &tag) {
  for (auto &input : out_input) {
    auto cnode = input->cast<CNodePtr>();
    if (!cnode) {
      continue;
    }
    if (IsPrimitiveCNode(cnode, prim::kPrimDepend)) {
      cnode = cnode->input(2)->cast<CNodePtr>();
    }
    if (cnode->input(1) == node) {
      auto prim = GetValueNode<PrimitivePtr>(cnode->input(0));
      auto dest_rank_send = GetValue<int64_t>(prim->GetAttr(tag));
      if (dest_rank_send == stage) {
        return input;
      }
    }
  }
  return nullptr;
}

std::pair<std::vector<AnfNodePtr>, std::vector<AnfNodePtr>> PipelineTransformer::CutBorder(const FuncGraphPtr &graph) {
  OperatorAttrs depend_attrs;
  auto depend_op = CreatOpInstance(depend_attrs, DEPEND, DEPEND);
  std::vector<AnfNodePtr> receive_ops;
  std::vector<AnfNodePtr> send_ops;
  auto ret = graph->get_return();
  MS_EXCEPTION_IF_NULL(ret);
  std::vector<AnfNodePtr> all_nodes = DeepScopedGraphSearch(ret);
  std::reverse(all_nodes.begin(), all_nodes.end());
  auto stage_num = g_device_manager->stage_num();
  if (root_->has_flag(TRAINING) && (stage_num > micro_size_)) {
    MS_LOG(EXCEPTION) << "MicroBatch size: " << micro_size_ << " can't less than stage num: " << stage_num;
  }
  for (auto &node : all_nodes) {
    if (!node->isa<CNode>() || node->stage() == -1) {
      continue;
    }
    auto node_users = manager_->node_users()[node];
    AnfNodePtr receive = nullptr;
    for (auto &user_pair : node_users) {
      auto user_node = user_pair.first;
      auto node_stage = node->stage();
      auto user_node_stage = user_node->stage();
      if (node_stage != stage_ && user_node_stage != stage_) {
        continue;
      }
      auto micro = user_node->cast<CNodePtr>()->GetPrimalAttr(MICRO);
      if (!micro) {
        MS_LOG(INFO) << "Can't find micro_batch information, use micro(0)";
        micro = MakeValue(int64_t(0));
      }
      if (node_stage < user_node_stage) {
        if (node_stage == stage_) {
          if (Reuse(node, user_node_stage, send_ops, DEST_RANK)) {
            continue;
          }
          auto send_out = InsertSend(graph, node, user_node_stage, node_stage, micro);
          MS_EXCEPTION_IF_NULL(send_out.depend);
          send_ops.push_back(send_out.depend);
          send_out.depend->set_user_data<Type>(DTYPE, send_out.type);
          send_out.depend->set_user_data<ValueList>(SHAPE, send_out.shape);
        } else {
          if (!receive) {
            receive = InsertReceive(graph, node, user_node, user_pair.second, user_node_stage, node_stage, micro);
            receive_ops.push_back(receive);
          } else {
            manager_->SetEdge(user_node, user_pair.second, receive);
          }
        }
        continue;
      }
      if (node_stage > user_node_stage) {
        MS_LOG(EXCEPTION) << "node_stage: " << node_stage
                          << " must be smaller than user_node_stage: " << user_node_stage;
      }
    }
  }
  return std::make_pair(send_ops, receive_ops);
}

void PipelineTransformer::CutGraph() {
  std::vector<AnfNodePtr> make_tuple_inputs;
  CreateForwardGroup();
  MS_EXCEPTION_IF_NULL(main_graph_);
  if (make_tuple_inputs.empty()) {
    make_tuple_inputs = HandleSharedParameter();
  }
  auto send_recv_ops = CutBorder(main_graph_);
  auto send_ops = send_recv_ops.first;
  if (IsLastStage()) {
    return;
  }
  if (send_ops.empty() && !root_->has_flag(TRAINING)) {
    return;
  }
  make_tuple_inputs.insert(make_tuple_inputs.end(), send_ops.begin(), send_ops.end());
  if (!send_ops.empty()) {
    type_ptr_ = send_ops.back()->user_data<Type>(DTYPE);
    shape_ = send_ops.back()->user_data<ValueList>(SHAPE);
  }
  auto make_tuple = main_graph_->NewCNode(make_tuple_inputs);
  std::vector<AnfNodePtr> out = {NewValueNode(prim::kPrimDepend)};
  out.push_back(send_ops.back());
  out.push_back(make_tuple);
  auto out_node = main_graph_->NewCNode(out);
  manager_->Replace(main_graph_->output(), out_node);
}

void PipelineTransformer::ElimGraphStage() {
  for (auto &fg : manager_->func_graphs()) {
    fg->set_stage(-1);
  }
}

std::pair<CNodePtr, FuncGraphPtr> PipelineTransformer::FindSensNode() {
  std::pair<CNodePtr, FuncGraphPtr> sens_graph_pair;
  CNodePtr sens_cnode;
  FuncGraphPtr func_graph;
  for (auto &node : root_->nodes()) {
    if (!node->isa<CNode>()) {
      continue;
    }
    sens_cnode = node->cast<CNodePtr>();
    AnfNodePtr expect_tuple_getitem = sens_cnode->input(0);
    MS_EXCEPTION_IF_NULL(expect_tuple_getitem);
    if (!expect_tuple_getitem->isa<CNode>()) {
      continue;
    }

    auto expect_tuple_getitem_cnode = expect_tuple_getitem->cast<CNodePtr>();
    if (!IsPrimitiveCNode(expect_tuple_getitem_cnode, prim::kPrimTupleGetItem)) {
      continue;
    }
    auto expect_anonymous = expect_tuple_getitem_cnode->input(1);
    if (!expect_anonymous->isa<CNode>()) {
      continue;
    }
    auto expect_anonymous_cnode = expect_anonymous->cast<CNodePtr>();
    AnfNodePtr expect_j = expect_anonymous_cnode->input(0);
    if (!expect_j->isa<CNode>()) {
      continue;
    }
    auto expect_j_cnode = expect_j->cast<CNodePtr>();
    if (!IsPrimitiveCNode(expect_j_cnode, prim::kPrimJ)) {
      continue;
    }
    func_graph = GetValueNode<FuncGraphPtr>(expect_j_cnode->input(1));
    break;
  }
  sens_graph_pair = std::make_pair(sens_cnode, func_graph);
  return sens_graph_pair;
}

void PipelineTransformer::CoverSensShape() {
  if (IsLastStage()) {
    return;
  }
  auto sens_graph_pair = FindSensNode();
  auto sens_cnode = sens_graph_pair.first;
  MS_EXCEPTION_IF_NULL(sens_cnode);
  OperatorAttrs attrs;
  auto fill_op = CreatOpInstance(attrs, "Fill", "");
  MS_EXCEPTION_IF_NULL(type_ptr_);
  MS_EXCEPTION_IF_NULL(shape_);
  std::vector<AnfNodePtr> fill_input = {NewValueNode(fill_op), NewValueNode(type_ptr_),
                                        NewValueNode(MakeValue(shape_->value())), NewValueNode(0)};
  auto fill = root_->NewCNode(fill_input);
  std::vector<AnfNodePtr> new_sens_input = {sens_cnode->input(0), fill};
  auto new_sens_node = root_->NewCNode(new_sens_input);
  manager_->Replace(sens_cnode, new_sens_node);
}

void PipelineTransformer::ElimParameter() {
  auto parameters = root_->parameters();
  std::vector<AnfNodePtr> parameter_list;
  for (auto &parameter : parameters) {
    if (!manager_->node_users()[parameter].empty()) {
      parameter_list.push_back(parameter);
    }
  }
  auto del_num = parameters.size() - parameter_list.size();
  root_->set_hyper_param_count(root_->hyper_param_count() - del_num);
  manager_->SetParameters(root_, parameter_list);
}
}  // namespace parallel
}  // namespace mindspore
