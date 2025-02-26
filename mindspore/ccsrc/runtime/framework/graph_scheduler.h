/**
 * Copyright 2021 Huawei Technologies Co., Ltd
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

#ifndef MINDSPORE_CCSRC_RUNTIME_FRAMEWORK_GRAPH_SCHEDULER_H_
#define MINDSPORE_CCSRC_RUNTIME_FRAMEWORK_GRAPH_SCHEDULER_H_

#include <vector>
#include <string>
#include <memory>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <set>
#include <algorithm>
#include <fstream>
#include "runtime/framework/actor/data_source_actor.h"
#include "runtime/framework/actor/loop_count_actor.h"
#include "runtime/framework/actor/kernel_actor.h"
#include "runtime/framework/actor/output_actor.h"
#include "runtime/framework/actor/switch_actor.h"
#include "runtime/framework/actor/gather_actor.h"
#include "runtime/framework/actor/copy_actor.h"
#include "runtime/hardware/device_context.h"
#include "backend/session/kernel_graph.h"
#include "thread/actor_threadpool.h"

namespace mindspore {
namespace runtime {
using mindspore::device::DeviceContext;
using mindspore::session::KernelGraph;
using mindspore::session::KernelWithIndex;
// Position of kernel with index, the value pair<branch_id, vector<pos>> means the branch id of the kernel and the pos
// of the kernel. Generally, there is only one branch, and the branch id is 0 at this time. In control flow, there are
// multiple branch scenarios, and pos represents the position of the kernel in the branch.
using KernelMapPosition = std::map<KernelWithIndex, std::vector<size_t>, session::KernelWithIndexCmp>;
using ActorInfo = std::string;

// The second element of pair represents the output index of op actor corresponding to the graph output node.
using GraphOutputPair = std::pair<OpActor<DeviceTensor> *, size_t>;

// DataArrowPair represent data edge between from actor and to actor.
// The first element of pair is the AID of from actor, and
// second element is op arrow between actors.
using DataArrowPair = std::pair<AID, DataArrowPtr>;

// The graph compiler info generated by graph compiler is the express of executable graph.
// The device context is unified interface of interaction with device of corresponding graph.
// The tensors mask is used to distinguish input tensor's type.
// The input tensor is used to link graphs in the dynamic build scenario.
// The control node is used to link graphs in the control flow scenario.
// The control node parser is used to parse the edge info in control nodes.
// The origin parameters order is used to correspond to the input args.
// The origin outputs order is used to correspond to the output args.
struct GraphCompilerInfo {
  GraphCompilerInfo(const std::vector<KernelGraphPtr> &graphs, const std::vector<DeviceContext *> &device_contexts,
                    const std::vector<std::vector<int64_t> *> &tensors_mask,
                    const std::vector<std::vector<TensorPtr> *> &input_tensors,
                    const std::vector<AnfNodePtr> &control_nodes,
                    const std::vector<AnfNodePtr> &origin_parameters_order, const ControlNodeParserPtr &parser,
                    const KernelMapPosition &origin_outputs_order, const size_t outputs_num, const std::string &name,
                    GraphExecutionStrategy strategy)
      : graphs_(graphs),
        device_contexts_(device_contexts),
        tensors_mask_(tensors_mask),
        input_tensors_(input_tensors),
        control_nodes_(control_nodes),
        control_node_parser_(parser),
        origin_parameters_order_(origin_parameters_order),
        origin_outputs_order_(origin_outputs_order),
        outputs_num_(outputs_num),
        name_(name),
        strategy_(strategy) {}
  std::vector<KernelGraphPtr> graphs_;
  std::vector<DeviceContext *> device_contexts_;
  std::vector<std::vector<int64_t> *> tensors_mask_;
  std::vector<std::vector<TensorPtr> *> input_tensors_;
  std::vector<AnfNodePtr> control_nodes_;
  ControlNodeParserPtr control_node_parser_;
  std::vector<AnfNodePtr> origin_parameters_order_;
  KernelMapPosition origin_outputs_order_;
  size_t outputs_num_;
  std::string name_;
  GraphExecutionStrategy strategy_;
};

// The actor set generated by graph transformer is the execution unit of actor runtime.
// It includes data source actor, kernel actor, switch actor, copy actor, loop count actor and output actor.
// The data source actor is used to obtain data and process them into device tensors, and send them to kernel actor.
// The kernel actor is used to receive the device tensors to luanch kernel. Specifically notice the no input
// kernel actor, it means that this actor has no input device tensor, need be triggered externally.
// The switch actor is used to run different branches in the control flow scenario.
// The gather actor is used to collect the inputs of graph and send branch id to loop count actor in multi-branch
// output scenario.
// The copy actor is used to convert the device tensor between the different device kernel.
// The loop count actor is used to receive the control of tail kernel actor to represent the end of one step
// and decide whether to loop execution by loop count.
// The output actor is used to receive the output result of actor which represents the graph output.
struct ActorSet {
  explicit ActorSet(const ActorInfo &name) : name_(name) {}
  std::vector<DataSourceActorPtr> data_source_actors_;
  std::vector<KernelActorPtr> kernel_actors_;
  // No input kernel actors need be triggered specifically.
  std::vector<KernelActorPtr> no_input_kernel_actors_;
  std::vector<SwitchActorPtr> switch_actors_;
  std::vector<GatherActorPtr> gather_actors_;
  std::vector<CopyActorPtr> copy_actors_;
  LoopCountActorPtr loop_count_actor_{nullptr};
  OutputActorPtr output_actor_{nullptr};
  ActorInfo name_;
};
using ActorSetPtr = std::shared_ptr<ActorSet>;

class GraphScheduler {
 public:
  static GraphScheduler &GetInstance() {
    static GraphScheduler instance;
    return instance;
  }

  // 1. Thread pool creating.
  // 2. The global actors creating and scheduling.
  void Initialize();

  // Clear the members.
  void Clear();

  // Transform graph to actor DAG, contains build and link.
  ActorSet *Transform(const GraphCompilerInfo &graph_compiler_info);

  // Schedule actors in the actor runtime. Single machine scheduling is supported currently, and distributed scheduling
  // will be supported in the future.
  void Schedule(const ActorSet *actor_set);

  // The prepare processing before run. (used in pipeline mode):
  // 1. Prepare the data of device tensor store(such as weights and value nodes of graph).
  // 2. Prepare the data of host tensor queue(such as non weighted parameters of graph).
  // 3. Prepare the continuous memory for communication kernel.
  void PrepareRun(const ActorSet *actor_set, const GraphCompilerInfo &graph_compiler_info,
                  const std::vector<std::vector<TensorPtr>> &input_tensors);

  // The prepare processing before run. (used in step mode):
  // 1. Prepare the data of device tensor store(such as weights and value nodes of graph).
  // 2. Prepare the data of host tensor queue(such as non weighted parameters of graph).
  void PrepareRunOp(const ActorSet *actor_set, const GraphCompilerInfo &graph_compiler_info,
                    const std::vector<std::vector<TensorPtr>> &input_tensors);

  // The processing entry of actors running.
  bool Run(const ActorSet *actor_set, GraphExecutionStrategy strategy = GraphExecutionStrategy::kPipeline,
           const std::vector<TensorPtr> *input_tensors = nullptr);

  // Fetch the actor set by actor info.
  ActorSet *Fetch(const ActorInfo &actor_info) const;

 private:
  GraphScheduler() = default;
  ~GraphScheduler() = default;
  DISABLE_COPY_AND_ASSIGN(GraphScheduler);

  // The Global actors contain memory manager actor, recorder actor and debug actor.
  void BuildAndScheduleGlobalActor();

  // Transform the nodes of graph to actors.
  ActorSetPtr Build(const GraphCompilerInfo &graph_compiler_info);
  // Link actors to DAG through the edge connection of graph and graph execution strategy.
  void Link(ActorSet *actor_set, const GraphCompilerInfo &graph_compiler_info);

  // The processing of actors build.
  std::vector<DataSourceActorPtr> BuildDataSourceActor(const GraphCompilerInfo &graph_compiler_info,
                                                       const HostTensorQueuePtr &host_queue);
  std::vector<KernelActorPtr> BuildKernelActor(const GraphCompilerInfo &graph_compiler_info);
  LoopCountActorPtr BuildLoopCountActor(const GraphCompilerInfo &graph_compiler_info);
  OutputActorPtr BuildOutputActor(const GraphCompilerInfo &graph_compiler_info);
  std::vector<KernelActorPtr> BuildNoInputKernelActor(const ActorSet *actor_set, GraphExecutionStrategy strategy);
  std::vector<SwitchActorPtr> BuildSwitchActor(const GraphCompilerInfo &graph_compiler_info);
  std::vector<GatherActorPtr> BuildGatherActor(const GraphCompilerInfo &graph_compiler_info);

  // Cache the information of graph output node to actor between “build” and “link”, for linking between the tail of
  // previous graph and the head of next graph.
  void CacheGraphOutputToActor(const GraphCompilerInfo &graph_compiler_info);

  // The processing of actors link statically.
  // 1. The processing of linking data arrows.
  // The gather of linking data arrows of kernel, it will call following functions by the different from actor type.
  void LinkDataArrow(KernelActor *to_actor, const GraphCompilerInfo &graph_compiler_info, const KernelGraphPtr &graph,
                     KernelWithIndex from_kernel_with_output_idx, KernelWithIndex to_kernel_with_input_idx,
                     const TensorPtr &tensor);
  // Link data arrows for internal parameter, convert internal parameter to actor by internal parameter cache to link.
  void LinkDataArrowForInternalParameter(const AnfNodePtr &internal_parameter,
                                         const std::vector<AnfNodePtr> &host_parameters, const KernelGraphPtr &graph,
                                         KernelActor *to_actor, KernelWithIndex to_kernel_with_input_idx);
  // Link data arrows in the copy actor scene, insert the copy actor between from_actor and to_actor.
  void LinkDataArrowForCopyActor(OpActor<DeviceTensor> *from_actor, KernelActor *to_actor,
                                 KernelWithIndex from_kernel_with_output_idx, KernelWithIndex to_kernel_with_input_idx);
  void LinkDataArrowForDeviceDSActor(DeviceQueueDataSourceActor *from_actor, KernelActor *to_actor,
                                     KernelWithIndex from_kernel_with_output_idx,
                                     KernelWithIndex to_to_kernel_with_input_idx);
  void LinkDataArrowForHostDSActor(HostQueueDataSourceActor *from_actor, KernelActor *to_actor,
                                   KernelWithIndex from_kernel_with_output_idx,
                                   KernelWithIndex to_kernel_with_input_idx);
  void LinkDataArrowForKernelActor(KernelActor *from_actor, KernelActor *to_actor,
                                   KernelWithIndex from_kernel_with_output_idx,
                                   KernelWithIndex to_kernel_with_input_idx);

  // 2. The processing of linking control arrows.
  void LinkControlArrowForLoopCountActor(LoopCountActor *loop_count_actor, const ActorSet *actor_set,
                                         const ControlNodeParserPtr &parser);
  void LinkControlArrowByAutoMonad(KernelActor *to_actor, const AnfNodePtr &from_node);
  // The skipped node doesn't run, so need link the control arrow between the inputs and user of skipped node.
  void LinkControlArrowBySkippedNode(KernelActor *to_actor, const AnfNodePtr &skipped_node);
  // Link the control arrows for allreduce kernel by the send/recv nodes in the kernel graph.
  void LinkControlArrowBySendRecvNodes(const KernelGraphPtr &graph);
  // Link the control arrows by the communication nodes in the kernel graph to ensure communication nodes running order.
  void LinkControlArrowByCommunicationNode(const std::vector<CNodePtr> &communication_nodes,
                                           const GraphCompilerInfo &graph_compiler_info);
  void LinkDeviceTensorStoreForAutoMonadActor(const std::vector<KernelActor *> &auto_monad_actors);

  // 3. The processing of linking output result arrows.
  void LinkOutputResultArrowForOutputActor(OutputActor *to_actor, const GraphCompilerInfo &graph_compiler_info);

  // 4. The processing of control flow linking.
  void LinkArrowByControlNode(const GraphCompilerInfo &graph_compiler_info, ActorSet *actor_set);
  void LinkDataArrowForGatherActor(GatherActor *from_actor, KernelActor *to_actor,
                                   const KernelWithIndex &front_node_with_index,
                                   const KernelWithIndex &to_node_with_index);
  void LinkDataArrowForSwitchActor(const GraphCompilerInfo &graph_compiler_info, SwitchActor *actor);
  // Connect the input of the actor.
  void LinkDataArrowByControlNode(const GraphCompilerInfo &graph_compiler_info, const KernelWithIndex &input_node,
                                  const FuncGraphPtr &from_func_graph, OpActor<DeviceTensor> *to_actor,
                                  const size_t to_index);
  // When the input of the actor is a call node, the output of the funcgraph called by the call node needs to be
  // connected.
  void LinkDataArrowByCallInput(const KernelWithIndex &call_node_with_index, const ControlNodeParserPtr &parser,
                                const FuncGraphPtr &from_func_graph, OpActor<DeviceTensor> *to_actor,
                                const size_t to_index);
  void LinkDataArrowForSwitchActor(SwitchActor *from_actor, const size_t from_index, OpActor<DeviceTensor> *to_actor,
                                   const size_t to_index, const size_t branch_index = SIZE_MAX);

  void LinkControlArrowForGatherActor(std::vector<KernelActorPtr> *kernel_actors,
                                      const std::vector<KernelGraphPtr> &graphs, const ControlNodeParserPtr &parser);

  void LinkControlArrowForSwitchActor(std::vector<SwitchActorPtr> *switch_actors, LoopCountActor *to_actor,
                                      const KernelMapPosition &origin_outputs_order);
  // In control flow, there are scenarios where there are multi-branch outputs, and the gather actor needs to
  // send the branch id to the loop count actor.
  void LinkBranchArrowForSwitchActor(const GraphCompilerInfo &graph_compiler_info, const ActorSet *actor_set);
  void LinkBranchArrowForGatherActor(const GraphCompilerInfo &graph_compiler_info, const ActorSet *actor_set);
  void LinkOutputResultArrowForGatherActor(const GraphCompilerInfo &graph_compiler_info, const ActorSet *actor_set);
  void LinkOutputResultArrowForSwitchActor(const GraphCompilerInfo &graph_compiler_info, const ActorSet *actor_set);
  void PrepareDataForControlNode(HostQueueDataSourceActor *host_data_source_actor,
                                 const ControlNodeParserPtr &control_node_parser,
                                 const std::vector<AnfNodePtr> &origin_parameters,
                                 const std::vector<TensorPtr> &tensors, std::vector<TensorPtr> *host_tensors);
  // Add input for switch actor. Since part of the input of funcgraph is on call node, these inputs need to be added
  // to switch actor.
  void PrepareInputNodeForSwitchActor(const std::vector<AnfNodePtr> &control_nodes);

  // The processing of actors link dynamically.
  // Analyze necessary input data of current actor, generate and cache op arrow
  // between current actor and prev actor, the method executes before calling Schedule.
  void PrepareForDynamiclyLink(ActorSet *actor_set, const CNodePtr &kernel, const AID &aid,
                               const std::vector<TensorPtr> *input_tensors);
  // Link to prev actor dynamically, and send message to prev actor to add the
  // new DataArrow and send output data back, the method must execute after calling Schedule.
  void LinkDataArrowForKernelActorDynamicly(const ActorSet *actor_set);

  // Check whether the actor set is valid.
  bool CheckActorValid(const ActorSet *actor_set,
                       GraphExecutionStrategy strategy = GraphExecutionStrategy::kPipeline) const;

  // Persist device tensors of graph's some nodes(such as weights and value nodes).
  void PersistDeviceTensor(const GraphCompilerInfo &graph_compiler_info);

  // Fetch the hsot tensor queue by actor info.
  HostTensorQueue *FetchHostQueue(const ActorInfo &actor_info) const;

  // The operation of the map of actor_name_to_actor_.
  void InsertActor(OpActor<DeviceTensor> *actor);
  OpActor<DeviceTensor> *FetchActor(const std::string &actor_name) const;

  // Host parameters are parameters of root funcgraph, in control flow, only the parameters of the root funcgraph are
  // in the host data source.
  bool IsHostQueueDSActor(const AnfNodePtr &node, const KernelGraphPtr &graph = nullptr,
                          const TensorPtr &tensor = nullptr, const std::vector<AnfNodePtr> &host_parameters = {},
                          GraphExecutionStrategy strategy = GraphExecutionStrategy::kPipeline);

  // Display the actor information of corresponding kernel graph.
  void DumpActor(const ActorSet *actor_set, const GraphCompilerInfo &graph_compiler_info) const;
  void DumpBaseActor(const OpActor<DeviceTensor> *actor, std::ofstream &ofs) const;
  void DumpDSActor(const DataSourceActor *actor, std::ofstream &ofs) const;
  void DumpLoopCountActor(const LoopCountActor *actor, std::ofstream &ofs) const;
  void DumpKernelActor(const KernelActor *actor, std::ofstream &ofs) const;
  void DumpOutputActor(const OutputActor *actor, std::ofstream &ofs) const;
  void DumpCopyActor(const CopyActor *actor, std::ofstream &ofs) const;
  void DumpGatherActor(const GatherActor *actor, std::ofstream &ofs) const;
  void DumpSwitchActor(const SwitchActor *actor, std::ofstream &ofs) const;
  void DumpDeviceTensorStore(const GraphCompilerInfo &graph_compiler_info, std::ofstream &ofs) const;

  // The global maps, only be cleared in the deconstruction.
  std::unordered_map<ActorInfo, ActorSetPtr> actors_;
  std::unordered_map<std::string, OpActor<DeviceTensor> *> actor_name_to_actor_;
  std::unordered_map<ActorInfo, HostTensorQueuePtr> actor_to_host_queue_;
  // The second element of pair represents the output index of op actor corresponding to the device tensor.
  std::unordered_map<DeviceTensorPtr, GraphOutputPair> device_tensor_to_actor_;

  // The local maps and vectors, will be cleared at the beginning of each graph transform:
  // 1.The second element of pair represents the output index of op actor corresponding to the graph output front node.
  std::map<KernelWithIndex, GraphOutputPair, session::KernelWithIndexCmp> graph_output_to_actor_;
  // 2.Since the control node does not have a backend node, it can only be connected through the relationship between
  // the front node, so the mapping relationship between the front node and the actor needs to be recorded.
  std::unordered_map<AnfNodePtr, KernelActorPtr> front_node_to_actor_;
  // 3.Beaceuse the copy actors are built in the link, so need record the all copy actors in the link process to push
  // into the actor set after link.
  std::vector<CopyActorPtr> copy_actors_;

  // The id of global actor.
  AID memory_manager_aid_;
  const AID *recorder_aid_{nullptr};
  const AID *debug_aid_{nullptr};

  ActorThreadPool *thread_pool_{nullptr};

  bool init_{false};
};
}  // namespace runtime
}  // namespace mindspore

#endif  // MINDSPORE_CCSRC_RUNTIME_FRAMEWORK_GRAPH_SCHEDULER_H_
