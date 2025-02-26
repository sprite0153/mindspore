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
#include "runtime/hccl_adapter/hccl_adapter.h"
#include <dlfcn.h>
#include <map>
#include <algorithm>
#define google ascend_private
#include "common/opskernel/ops_kernel_info_store.h"
#include "common/opskernel/ops_kernel_builder.h"
#include "external/ge/ge_api_types.h"
#undef google
#include "hccl/hccl.h"
#include "hccl/hcom.h"
#include "utils/log_adapter.h"
#include "utils/ms_utils.h"
#include "runtime/hccl_adapter/converter.h"

static constexpr const char *kHcclPluginFileName = "libhccl_plugin.so";
static constexpr const char *kHcclDeployModeEnv = "DEPLOY_MODE";
static constexpr const char *kHcclAlgoEnv = "HCCL_ALGO";

inline static std::string GetDlErrorMsg() {
  const char *result = dlerror();
  return (result == nullptr) ? "Unknown" : result;
}

template <class T>
static T DlsymWithCast(void *handle, const char *symbol_name) {
  T symbol = reinterpret_cast<T>(dlsym(handle, symbol_name));
  if (symbol == nullptr) {
    MS_LOG(EXCEPTION) << "Dlsym symbol " << symbol_name << " failed, result = " << GetDlErrorMsg();
  }
  return symbol;
}

#define DlsymFuncObj(func_name) DlsymWithCast<func_name##FunPtr>(plugin_handle_, k##func_name##Name);

static std::map<std::string, std::string> GenHcclOptions(uint32_t device_id, std::string_view rank_id,
                                                         std::string_view rank_file) {
  auto env_deploy_mode = mindspore::common::GetEnv(kHcclDeployModeEnv);
  if (env_deploy_mode.empty()) {
    MS_LOG(WARNING) << kHcclDeployModeEnv << " is not set in ENV. Now set to default value 0";
    env_deploy_mode = "0";
  }

  std::map<std::string, std::string> default_options_map = {{ge::OPTION_EXEC_IS_USEHCOM, "1"},
                                                            {ge::OPTION_EXEC_IS_USEHVD, "0"},
                                                            {ge::OPTION_EXEC_HCCL_FLAG, "1"},
                                                            {ge::OPTION_EXEC_DEVICE_ID, std::to_string(device_id)},
                                                            {ge::OPTION_EXEC_RANK_ID, rank_id.data()},
                                                            {ge::OPTION_EXEC_POD_NAME, rank_id.data()},
                                                            {ge::OPTION_EXEC_RANK_TABLE_FILE, rank_file.data()},
                                                            {ge::OPTION_GRAPH_RUN_MODE, "1"},
                                                            {ge::OPTION_EXEC_HCCL_FLAG, "1"},
                                                            {ge::OPTION_EXEC_DEPLOY_MODE, env_deploy_mode}};

  auto env_hccl_algo = mindspore::common::GetEnv(kHcclAlgoEnv);
  if (!env_hccl_algo.empty()) {
    std::string ge_hccl_algo = "HCCL_algorithm";
    default_options_map.emplace(ge_hccl_algo, env_hccl_algo);
  }

  return default_options_map;
}

namespace mindspore::hccl {
HcclAdapter &HcclAdapter::GetInstance() {
  static HcclAdapter instance;
  return instance;
}

void HcclAdapter::InitPlugin() {
  if (plugin_handle_ != nullptr) {
    return;
  }

  plugin_handle_ = dlopen(kHcclPluginFileName, RTLD_NOW | RTLD_GLOBAL);
  if (plugin_handle_ == nullptr) {
    MS_LOG(EXCEPTION) << "Dlopen " << kHcclPluginFileName << " failed, result = " << GetDlErrorMsg();
  }

  init_hcom_graph_adapter_ = DlsymFuncObj(InitHcomGraphAdapter);
  finalize_hcom_graph_adapter_ = DlsymFuncObj(FinalizeHcomGraphAdapter);
  get_hccl_kernel_info_store_ = DlsymFuncObj(GetHcclKernelInfoStore);
  get_all_kernel_builder_ = DlsymFuncObj(GetAllKernelBuilder);
  init_hccl_comm_ = DlsymFuncObj(HcclCommInitClusterInfo);
  finalize_hccl_comm_ = DlsymFuncObj(HcclCommDestroy);
  launch_hccl_broadcast_ = DlsymFuncObj(HcclBroadcast);
  launch_hccl_all_reduce_ = DlsymFuncObj(HcclAllReduce);
  hccl_create_group_ = DlsymFuncObj(HcomCreateGroup);
  hccl_destroy_group_ = DlsymFuncObj(HcomDestroyGroup);
  hccl_get_rank_id_ = DlsymFuncObj(HcomGetRankId);
  hccl_get_rank_size_ = DlsymFuncObj(HcomGetRankSize);
  hccl_exec_initialize_ = DlsymFuncObj(HcomExecInitialize);
  hccl_exec_finalize_ = DlsymFuncObj(HcomExecFinalize);
  hccl_exec_enqueue_op_ = DlsymFuncObj(HcomExecEnqueueOperation);
}

void HcclAdapter::FinalizePlugin() {
  if (plugin_handle_ == nullptr) {
    return;
  }

  init_hcom_graph_adapter_ = nullptr;
  finalize_hcom_graph_adapter_ = nullptr;
  get_hccl_kernel_info_store_ = nullptr;
  get_all_kernel_builder_ = nullptr;
  init_hccl_comm_ = nullptr;
  finalize_hccl_comm_ = nullptr;
  launch_hccl_broadcast_ = nullptr;
  launch_hccl_all_reduce_ = nullptr;
  hccl_create_group_ = nullptr;
  hccl_destroy_group_ = nullptr;
  hccl_get_rank_id_ = nullptr;
  hccl_get_rank_size_ = nullptr;
  hccl_exec_initialize_ = nullptr;
  hccl_exec_finalize_ = nullptr;
  hccl_exec_enqueue_op_ = nullptr;
  (void)dlclose(plugin_handle_);
  plugin_handle_ = nullptr;
}

bool HcclAdapter::InitHccl(uint32_t device_id, std::string_view rank_id, std::string_view rank_file) {
  MS_LOG(INFO) << "Start init hccl adapter.";
  std::lock_guard<std::mutex> lock(init_mutex_);
  if (init_flag_) {
    MS_LOG(INFO) << "Hccl has been inited, skip.";
    return true;
  }

  InitPlugin();
  bool ret = InitKernelInfoStore(device_id, rank_id, rank_file);
  if (!ret) {
    return false;
  }
  ret = InitHcclComm(rank_id, rank_file);
  if (!ret) {
    return false;
  }

  ret = InitHcclExec();
  if (!ret) {
    return false;
  }

  init_flag_ = true;
  MS_LOG(INFO) << "Init hccl adapter success.";
  return true;
}

bool HcclAdapter::FinalizeHccl() {
  MS_LOG(INFO) << "Start destroy hccl adapter.";
  std::lock_guard<std::mutex> lock(init_mutex_);
  if (!init_flag_) {
    MS_LOG(INFO) << "Hccl has never been inited, skip.";
    return true;
  }

  (void)FinalizeHcclExec();
  (void)FinalizeHcclComm();
  (void)FinalizeKernelInfoStore();
  FinalizePlugin();
  init_flag_ = false;
  MS_LOG(INFO) << "Destroy hccl adapter success.";
  return true;
}

bool HcclAdapter::GenTask(const AnfNodePtr &node, HcclDataType datatype,
                          std::vector<HcclTaskInfo> *task_info_lists) const {
  MS_EXCEPTION_IF_NULL(node);
  MS_EXCEPTION_IF_NULL(task_info_lists);
  MS_LOG(INFO) << "Start generate task for hccl node " << node->DebugString();
  auto [ge_node, ge_graph] = GenerateStubGeNode(node, datatype);
  MS_EXCEPTION_IF_NULL(ge_node);
  auto op = ge_node->GetOpDesc();
  MS_EXCEPTION_IF_NULL(op);

  MS_LOG(INFO) << "Start to call CalcOpRunningParam";
  MS_EXCEPTION_IF_NULL(ops_kernel_builder_);
  ge::Status ret = ops_kernel_builder_->CalcOpRunningParam(*ge_node);
  if (ret != ge::SUCCESS) {
    MS_LOG(ERROR) << "OpsKernelBuilder CalcOpRunningParam failed, ret = " << ret;
    return false;
  }
  MS_LOG(INFO) << "Start to call GenerateTask";
  ge::RunContext unused_ctx;
  std::vector<domi::TaskDef> domi_tasks;
  ret = ops_kernel_builder_->GenerateTask(*ge_node, unused_ctx, domi_tasks);
  if (ret != ge::SUCCESS) {
    MS_LOG(ERROR) << "OpsKernelBuilder GenerateTask failed, ret = " << ret;
    return false;
  }

  task_info_lists->clear();
  std::transform(domi_tasks.begin(), domi_tasks.end(), std::back_inserter(*task_info_lists),
                 [&op](const domi::TaskDef &task_def) -> HcclTaskInfo { return ParseDomiTask(op, task_def); });
  MS_LOG(INFO) << "Generate task for node " << node->DebugString() << " success.";
  ge_graph.reset();
  return true;
}

int64_t HcclAdapter::CalcWorkspaceSize(const AnfNodePtr &node, HcclDataType datatype) const {
  MS_EXCEPTION_IF_NULL(ops_kernel_builder_);
  MS_LOG(INFO) << "Start calc workspace size for hccl node " << node->DebugString() << " ,dtype is " << datatype;
  auto [ge_node, ge_graph] = GenerateStubGeNode(node, datatype);
  MS_EXCEPTION_IF_NULL(ge_node);
  auto op = ge_node->GetOpDesc();
  MS_EXCEPTION_IF_NULL(op);

  MS_LOG(INFO) << "Start to call CalcOpRunningParam";
  ge::Status ret = ops_kernel_builder_->CalcOpRunningParam(*ge_node);
  if (ret != ge::SUCCESS) {
    MS_LOG(ERROR) << "OpsKernelBuilder CalcOpRunningParam failed, ret = " << ret;
    return false;
  }

  auto workspace_sizes = op->GetWorkspaceBytes();
  if (workspace_sizes.size() != 1) {
    MS_LOG(EXCEPTION) << "Unexpected workspace size " << workspace_sizes.size();
  }
  int64_t workspace_size = workspace_sizes[0];
  MS_LOG(INFO) << "Node " << node->DebugString() << " workspace size is " << workspace_size;
  ge_graph.reset();
  return workspace_size;
}

void *HcclAdapter::GetHcclOpsKernelInfoStore() const { return ops_kernel_info_store_.get(); }

std::string HcclAdapter::GetHcclType(const AnfNodePtr &node) {
  MS_EXCEPTION_IF_NULL(node);
  auto cnode = node->cast<CNodePtr>();
  MS_EXCEPTION_IF_NULL(cnode);
  return GetGeNodeName(cnode);
}

HcclResult HcclAdapter::HcclBroadcast(void *buf, uint64_t count, HcclDataType dataType, uint32_t root,
                                      aclrtStream stream) const {
  MS_EXCEPTION_IF_NULL(launch_hccl_broadcast_);
  return launch_hccl_broadcast_(buf, count, dataType, root, hccl_comm_, stream);
}

HcclResult HcclAdapter::HcclAllReduce(void *sendBuf, void *recvBuf, uint64_t count, HcclDataType dataType,
                                      HcclReduceOp op, aclrtStream stream) const {
  MS_EXCEPTION_IF_NULL(launch_hccl_all_reduce_);
  return launch_hccl_all_reduce_(sendBuf, recvBuf, count, dataType, op, hccl_comm_, stream);
}

bool HcclAdapter::InitKernelInfoStore(uint32_t device_id, std::string_view rank_id, std::string_view rank_file) {
  MS_LOG(INFO) << "Start init hccl kernel info store.";
  MS_EXCEPTION_IF_NULL(init_hcom_graph_adapter_);
  MS_EXCEPTION_IF_NULL(get_hccl_kernel_info_store_);
  // get ops_kernel_builder
  std::map<std::string, std::shared_ptr<ge::OpsKernelBuilder>> all_builders;
  get_all_kernel_builder_(&all_builders);
  auto iter = all_builders.find(kHcclOpsKernelInfoStore);
  if (iter == all_builders.end()) {
    std::string all_builders_name = "[";
    for (const auto &it : all_builders) {
      all_builders_name += it.first + " ";
    }
    all_builders_name += "]";
    MS_LOG(EXCEPTION) << "Builders size " << all_builders.size() << ", cannot find " << kHcclOpsKernelInfoStore
                      << ", full list of builders: " << all_builders_name;
  }

  MS_LOG(INFO) << "Get builder " << iter->first;
  ops_kernel_builder_ = iter->second;
  MS_EXCEPTION_IF_NULL(ops_kernel_builder_);
  // init ops_kernel_builder
  auto options = GenHcclOptions(device_id, rank_id, rank_file);
  auto ret = ops_kernel_builder_->Initialize(options);
  if (ret != ge::SUCCESS) {
    MS_LOG(EXCEPTION) << "Init hccl kernel builder failed, ret = " << ret;
  }

  // get ops_kernel_info_store
  ret = init_hcom_graph_adapter_(options);
  if (ret != ge::SUCCESS) {
    MS_LOG(EXCEPTION) << "Init hccl graph adapter failed, ret = " << ret;
  }

  get_hccl_kernel_info_store_(&ops_kernel_info_store_);
  MS_EXCEPTION_IF_NULL(ops_kernel_info_store_);
  ret = ops_kernel_info_store_->Initialize(options);
  if (ret != ge::SUCCESS) {
    MS_LOG(EXCEPTION) << "Init info store failed, ret = " << ret;
  }
  MS_LOG(INFO) << "Init hccl kernel info store success.";
  return true;
}

bool HcclAdapter::FinalizeKernelInfoStore() {
  MS_LOG(INFO) << "Start destroy hccl kernel info store.";
  if (ops_kernel_info_store_ != nullptr) {
    auto ret = ops_kernel_info_store_->Finalize();
    if (ret != ge::SUCCESS) {
      MS_LOG(ERROR) << "Destroy info store failed, ret = " << ret;
      return false;
    }
  }

  if (ops_kernel_builder_ != nullptr) {
    auto ret = ops_kernel_builder_->Finalize();
    if (ret != ge::SUCCESS) {
      MS_LOG(ERROR) << "Destroy builder failed, ret = " << ret;
      return false;
    }
  }

  MS_EXCEPTION_IF_NULL(finalize_hcom_graph_adapter_);
  finalize_hcom_graph_adapter_();
  ops_kernel_info_store_.reset();
  ops_kernel_builder_.reset();
  MS_LOG(INFO) << "Destroy hccl kernel info store success.";
  return true;
}

bool HcclAdapter::InitHcclComm(std::string_view rank_id, std::string_view rank_file) {
  MS_LOG(INFO) << "Start init hccl comm.";
  int rank_id_i = -1;
  try {
    rank_id_i = std::stoi(rank_id.data());
  } catch (std::invalid_argument &) {
    MS_LOG(EXCEPTION) << "Invalid rank id env:" << rank_id;
  }
  if (rank_id_i < 0) {
    MS_LOG(ERROR) << "rank_id cannot be negative";
    return false;
  }
  MS_EXCEPTION_IF_NULL(init_hccl_comm_);
  auto hccl_result = init_hccl_comm_(rank_file.data(), rank_id_i, &hccl_comm_);
  if (hccl_result != HCCL_SUCCESS) {
    MS_LOG(ERROR) << "HcclCommInitClusterInfo failed, ret:" << hccl_result;
    return false;
  }
  MS_LOG(INFO) << "InitHcclComm success";
  return true;
}

bool HcclAdapter::FinalizeHcclComm() {
  MS_LOG(INFO) << "Start finalize hccl comm.";
  if (hccl_comm_ == nullptr) {
    return true;
  }

  MS_EXCEPTION_IF_NULL(finalize_hccl_comm_);
  auto hccl_result = finalize_hccl_comm_(hccl_comm_);
  if (hccl_result != HCCL_SUCCESS) {
    MS_LOG(ERROR) << "HcclComm destroy failed, ret:" << hccl_result;
    return false;
  }
  hccl_comm_ = nullptr;
  MS_LOG(INFO) << "HcclComm destroy success";
  return true;
}

HcclResult HcclAdapter::HcclCreateGroup(const std::string &group, uint32_t rank_num, uint32_t *rank_ids) const {
  MS_EXCEPTION_IF_NULL(hccl_create_group_);
  return hccl_create_group_(group.c_str(), rank_num, rank_ids);
}

HcclResult HcclAdapter::HcclDestroyGroup(const std::string &group) const {
  MS_EXCEPTION_IF_NULL(hccl_destroy_group_);
  return hccl_destroy_group_(group.c_str());
}

HcclResult HcclAdapter::HcclGetRankId(const std::string &group, uint32_t *rank_id) const {
  MS_EXCEPTION_IF_NULL(hccl_get_rank_id_);
  return hccl_get_rank_id_(group.c_str(), rank_id);
}

HcclResult HcclAdapter::HcclGetRankSize(const std::string &group, uint32_t *rank_size) const {
  MS_EXCEPTION_IF_NULL(hccl_get_rank_size_);
  return hccl_get_rank_size_(group.c_str(), rank_size);
}

bool HcclAdapter::InitHcclExec() {
  MS_LOG(INFO) << "Start init hccl exec.";
  MS_EXCEPTION_IF_NULL(hccl_exec_initialize_);
  HcclResult hccl_ret = hccl_exec_initialize_();
  if (hccl_ret == HCCL_E_PTR) {
    MS_LOG(WARNING) << "Hccl comm is null, hcom executor initialize is not required";
  } else if (hccl_ret == HCCL_SUCCESS) {
    MS_LOG(INFO) << "Hcom DynamicKernel Initialize success";
  } else {
    MS_LOG(ERROR) << "Hcom DynamicKernel Initialize failed";
    return false;
  }
  MS_LOG(INFO) << "InitHcclExec success";
  return true;
}

bool HcclAdapter::FinalizeHcclExec() {
  MS_LOG(INFO) << "Start finalize hccl exec.";
  MS_EXCEPTION_IF_NULL(hccl_exec_finalize_);
  HcclResult hccl_ret = hccl_exec_finalize_();
  if (hccl_ret != HCCL_SUCCESS) {
    MS_LOG(ERROR) << "Hcom DynamicKernel Finalize failed";
    return false;
  }
  MS_LOG(INFO) << "HcclExec destroy success";
  return true;
}

HcclResult HcclAdapter::HcclExecEnqueueOp(const ::HcomOperation &op_info, const HExecCallBack &callback) const {
  MS_EXCEPTION_IF_NULL(hccl_exec_enqueue_op_);
  return hccl_exec_enqueue_op_(op_info, callback);
}
}  // namespace mindspore::hccl
