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

#include "fl/server/server.h"
#include <memory>
#include <string>
#include <csignal>
#ifdef ENABLE_ARMOUR
#include "fl/armour/secure_protocol/secret_sharing.h"
#endif
#include "fl/server/round.h"
#include "fl/server/model_store.h"
#include "fl/server/iteration.h"
#include "fl/server/collective_ops_impl.h"
#include "fl/server/distributed_metadata_store.h"
#include "fl/server/distributed_count_service.h"
#include "fl/server/kernel/round/round_kernel_factory.h"

namespace mindspore {
namespace fl {
namespace server {
void Server::Initialize(bool use_tcp, bool use_http, uint16_t http_port, const std::vector<RoundConfig> &rounds_config,
                        const CipherConfig &cipher_config, const FuncGraphPtr &func_graph, size_t executor_threshold) {
  MS_EXCEPTION_IF_NULL(func_graph);
  func_graph_ = func_graph;

  if (rounds_config.empty()) {
    MS_LOG(EXCEPTION) << "Rounds are empty.";
    return;
  }
  rounds_config_ = rounds_config;
  cipher_config_ = cipher_config;

  use_tcp_ = use_tcp;
  use_http_ = use_http;
  http_port_ = http_port;
  executor_threshold_ = executor_threshold;
  return;
}

// Each step of the server pipeline may have dependency on other steps, which includes:

// InitServerContext must be the first step to set contexts for later steps.

// Server Running relies on URL or Message Type Register:
// StartCommunicator---->InitIteration

// Metadata Register relies on Hash Ring of Servers which relies on Network Building Completion:
// RegisterRoundKernel---->StartCommunicator

// Kernel Initialization relies on Executor Initialization:
// RegisterRoundKernel---->InitExecutor

// Getting Model Size relies on ModelStorage Initialization which relies on Executor Initialization:
// InitCipher---->InitExecutor
void Server::Run() {
  std::unique_lock<std::mutex> lock(scaling_mtx_);
  InitServerContext();
  InitCluster();
  InitIteration();
  RegisterCommCallbacks();
  StartCommunicator();
  InitExecutor();
  std::string encrypt_type = ps::PSContext::instance()->encrypt_type();
  if (encrypt_type != ps::kNotEncryptType) {
    InitCipher();
    MS_LOG(INFO) << "Parameters for secure aggregation have been initiated.";
  }
  RegisterRoundKernel();
  MS_LOG(INFO) << "Server started successfully.";
  safemode_ = false;
  lock.unlock();

  // Wait communicators to stop so the main thread is blocked.
  std::for_each(communicators_with_worker_.begin(), communicators_with_worker_.end(),
                [](const std::shared_ptr<ps::core::CommunicatorBase> &communicator) { communicator->Join(); });
  communicator_with_server_->Join();
  MsException::Instance().CheckException();
  return;
}

void Server::SwitchToSafeMode() {
  MS_LOG(INFO) << "Server switch to safemode.";
  safemode_ = true;
}

void Server::CancelSafeMode() {
  MS_LOG(INFO) << "Server cancel safemode.";
  safemode_ = false;
}

bool Server::IsSafeMode() { return safemode_.load(); }

void Server::InitServerContext() {
  ps::PSContext::instance()->GenerateResetterRound();
  scheduler_ip_ = ps::PSContext::instance()->scheduler_host();
  scheduler_port_ = ps::PSContext::instance()->scheduler_port();
  worker_num_ = ps::PSContext::instance()->initial_worker_num();
  server_num_ = ps::PSContext::instance()->initial_server_num();
  return;
}

void Server::InitCluster() {
  server_node_ = std::make_shared<ps::core::ServerNode>();
  MS_EXCEPTION_IF_NULL(server_node_);
  task_executor_ = std::make_shared<ps::core::TaskExecutor>(32);
  MS_EXCEPTION_IF_NULL(task_executor_);
  if (!InitCommunicatorWithServer()) {
    MS_LOG(EXCEPTION) << "Initializing cross-server communicator failed.";
    return;
  }
  if (!InitCommunicatorWithWorker()) {
    MS_LOG(EXCEPTION) << "Initializing worker-server communicator failed.";
    return;
  }
  return;
}

bool Server::InitCommunicatorWithServer() {
  MS_EXCEPTION_IF_NULL(task_executor_);
  MS_EXCEPTION_IF_NULL(server_node_);

  communicator_with_server_ =
    server_node_->GetOrCreateTcpComm(scheduler_ip_, scheduler_port_, worker_num_, server_num_, task_executor_);
  MS_EXCEPTION_IF_NULL(communicator_with_server_);
  return true;
}

bool Server::InitCommunicatorWithWorker() {
  MS_EXCEPTION_IF_NULL(server_node_);
  MS_EXCEPTION_IF_NULL(task_executor_);
  if (!use_tcp_ && !use_http_) {
    MS_LOG(EXCEPTION) << "At least one type of protocol should be set.";
    return false;
  }
  if (use_tcp_) {
    auto tcp_comm = communicator_with_server_;
    MS_EXCEPTION_IF_NULL(tcp_comm);
    communicators_with_worker_.push_back(tcp_comm);
  }
  if (use_http_) {
    auto http_comm = server_node_->GetOrCreateHttpComm(server_node_->BoundIp(), http_port_, task_executor_);
    MS_EXCEPTION_IF_NULL(http_comm);
    communicators_with_worker_.push_back(http_comm);
  }
  return true;
}

void Server::InitIteration() {
  iteration_ = &Iteration::GetInstance();
  MS_EXCEPTION_IF_NULL(iteration_);

  // 1.Add rounds to the iteration according to the server mode.
  for (const RoundConfig &config : rounds_config_) {
    std::shared_ptr<Round> round =
      std::make_shared<Round>(config.name, config.check_timeout, config.time_window, config.check_count,
                              config.threshold_count, config.server_num_as_threshold);
    MS_LOG(INFO) << "Add round " << config.name << ", check_timeout: " << config.check_timeout
                 << ", time window: " << config.time_window << ", check_count: " << config.check_count
                 << ", threshold: " << config.threshold_count
                 << ", server_num_as_threshold: " << config.server_num_as_threshold;
    iteration_->AddRound(round);
  }

#ifdef ENABLE_ARMOUR
  std::string encrypt_type = ps::PSContext::instance()->encrypt_type();
  if (encrypt_type == ps::kPWEncryptType) {
    cipher_initial_client_cnt_ = rounds_config_[0].threshold_count;
    cipher_exchange_secrets_cnt_ = cipher_initial_client_cnt_ * 1.0;
    cipher_share_secrets_cnt_ = cipher_initial_client_cnt_ * cipher_config_.share_secrets_ratio;
    cipher_get_clientlist_cnt_ = rounds_config_[1].threshold_count;
    cipher_reconstruct_secrets_up_cnt_ = rounds_config_[1].threshold_count;
    cipher_reconstruct_secrets_down_cnt_ = cipher_config_.reconstruct_secrets_threshhold;
    cipher_time_window_ = cipher_config_.cipher_time_window;

    MS_LOG(INFO) << "Initializing cipher:";
    MS_LOG(INFO) << " cipher_initial_client_cnt_: " << cipher_initial_client_cnt_
                 << " cipher_exchange_secrets_cnt_: " << cipher_exchange_secrets_cnt_
                 << " cipher_share_secrets_cnt_: " << cipher_share_secrets_cnt_;
    MS_LOG(INFO) << " cipher_get_clientlist_cnt_: " << cipher_get_clientlist_cnt_
                 << " cipher_reconstruct_secrets_up_cnt_: " << cipher_reconstruct_secrets_up_cnt_
                 << " cipher_time_window_: " << cipher_time_window_
                 << " cipher_reconstruct_secrets_down_cnt_: " << cipher_reconstruct_secrets_down_cnt_;

    std::shared_ptr<Round> exchange_keys_round =
      std::make_shared<Round>("exchangeKeys", true, cipher_time_window_, true, cipher_exchange_secrets_cnt_);
    iteration_->AddRound(exchange_keys_round);
    std::shared_ptr<Round> get_keys_round =
      std::make_shared<Round>("getKeys", true, cipher_time_window_, true, cipher_exchange_secrets_cnt_);
    iteration_->AddRound(get_keys_round);
    std::shared_ptr<Round> share_secrets_round =
      std::make_shared<Round>("shareSecrets", true, cipher_time_window_, true, cipher_share_secrets_cnt_);
    iteration_->AddRound(share_secrets_round);
    std::shared_ptr<Round> get_secrets_round =
      std::make_shared<Round>("getSecrets", true, cipher_time_window_, true, cipher_share_secrets_cnt_);
    iteration_->AddRound(get_secrets_round);
    std::shared_ptr<Round> get_clientlist_round =
      std::make_shared<Round>("getClientList", true, cipher_time_window_, true, cipher_get_clientlist_cnt_);
    iteration_->AddRound(get_clientlist_round);
    std::shared_ptr<Round> reconstruct_secrets_round = std::make_shared<Round>(
      "reconstructSecrets", true, cipher_time_window_, true, cipher_reconstruct_secrets_up_cnt_);
    iteration_->AddRound(reconstruct_secrets_round);
    MS_LOG(INFO) << "Cipher rounds has been added.";
  }
#endif

  // 2.Initialize all the rounds.
  TimeOutCb time_out_cb =
    std::bind(&Iteration::MoveToNextIteration, iteration_, std::placeholders::_1, std::placeholders::_2);
  FinishIterCb finish_iter_cb =
    std::bind(&Iteration::MoveToNextIteration, iteration_, std::placeholders::_1, std::placeholders::_2);
  iteration_->InitRounds(communicators_with_worker_, time_out_cb, finish_iter_cb);
  return;
}

void Server::InitCipher() {
#ifdef ENABLE_ARMOUR
  cipher_init_ = &armour::CipherInit::GetInstance();

  int cipher_t = cipher_reconstruct_secrets_down_cnt_;
  unsigned char cipher_p[SECRET_MAX_LEN] = {0};
  int cipher_g = 1;
  unsigned char cipher_prime[PRIME_MAX_LEN] = {0};
  float dp_eps = ps::PSContext::instance()->dp_eps();
  float dp_delta = ps::PSContext::instance()->dp_delta();
  float dp_norm_clip = ps::PSContext::instance()->dp_norm_clip();
  std::string encrypt_type = ps::PSContext::instance()->encrypt_type();

  mpz_t prim;
  mpz_init(prim);
  mindspore::armour::GetRandomPrime(prim);
  mindspore::armour::PrintBigInteger(prim, 16);

  size_t len_cipher_prime;
  mpz_export((unsigned char *)cipher_prime, &len_cipher_prime, sizeof(unsigned char), 1, 0, 0, prim);
  mindspore::armour::CipherPublicPara param;
  param.g = cipher_g;
  param.t = cipher_t;
  memcpy_s(param.p, SECRET_MAX_LEN, cipher_p, SECRET_MAX_LEN);
  memcpy_s(param.prime, PRIME_MAX_LEN, cipher_prime, PRIME_MAX_LEN);
  param.dp_delta = dp_delta;
  param.dp_eps = dp_eps;
  param.dp_norm_clip = dp_norm_clip;
  param.encrypt_type = encrypt_type;
  cipher_init_->Init(param, 0, cipher_initial_client_cnt_, cipher_exchange_secrets_cnt_, cipher_share_secrets_cnt_,
                     cipher_get_clientlist_cnt_, cipher_reconstruct_secrets_down_cnt_,
                     cipher_reconstruct_secrets_up_cnt_);
#endif
}

void Server::RegisterCommCallbacks() {
  // The message callbacks of round kernels are already set in method InitIteration, so here we don't need to register
  // rounds' callbacks.

  auto tcp_comm = std::dynamic_pointer_cast<ps::core::TcpCommunicator>(communicator_with_server_);
  MS_EXCEPTION_IF_NULL(tcp_comm);

  // Set message callbacks for server-to-server communication.
  DistributedMetadataStore::GetInstance().RegisterMessageCallback(tcp_comm);
  DistributedCountService::GetInstance().RegisterMessageCallback(tcp_comm);
  iteration_->RegisterMessageCallback(tcp_comm);
  iteration_->RegisterEventCallback(server_node_);

  // Set exception event callbacks for server.
  RegisterExceptionEventCallback(tcp_comm);

  if (!server_node_->InitFollowerScaler()) {
    MS_LOG(EXCEPTION) << "Initializing follower elastic scaler failed.";
    return;
  }
  // Set scaling barriers before scaling.
  server_node_->RegisterFollowerScalerBarrierBeforeScaleOut("ServerPipeline",
                                                            std::bind(&Server::ProcessBeforeScalingOut, this));
  server_node_->RegisterFollowerScalerBarrierBeforeScaleIn("ServerPipeline",
                                                           std::bind(&Server::ProcessBeforeScalingIn, this));
  // Set handlers after scheduler scaling operations are done.
  server_node_->RegisterFollowerScalerHandlerAfterScaleOut("ServerPipeline",
                                                           std::bind(&Server::ProcessAfterScalingOut, this));
  server_node_->RegisterFollowerScalerHandlerAfterScaleIn("ServerPipeline",
                                                          std::bind(&Server::ProcessAfterScalingIn, this));
}

void Server::RegisterExceptionEventCallback(const std::shared_ptr<ps::core::TcpCommunicator> &communicator) {
  MS_EXCEPTION_IF_NULL(communicator);
  communicator->RegisterEventCallback(ps::core::ClusterEvent::SCHEDULER_TIMEOUT, [&]() {
    MS_LOG(ERROR) << "Event SCHEDULER_TIMEOUT is captured. This is because scheduler node is finalized or crashed.";
    safemode_ = true;
    std::for_each(communicators_with_worker_.begin(), communicators_with_worker_.end(),
                  [](const std::shared_ptr<ps::core::CommunicatorBase> &communicator) { communicator->Stop(); });
    communicator_with_server_->Stop();
  });

  communicator->RegisterEventCallback(ps::core::ClusterEvent::NODE_TIMEOUT, [&]() {
    MS_LOG(ERROR)
      << "Event NODE_TIMEOUT is captured. This is because some server nodes are finalized or crashed after the "
         "network building phase.";
    safemode_ = true;
    std::for_each(communicators_with_worker_.begin(), communicators_with_worker_.end(),
                  [](const std::shared_ptr<ps::core::CommunicatorBase> &communicator) { communicator->Stop(); });
    communicator_with_server_->Stop();
  });
}

void Server::InitExecutor() {
  if (executor_threshold_ == 0) {
    MS_LOG(EXCEPTION) << "The executor's threshold should greater than 0.";
    return;
  }
  // The train engine instance is used in both push-type and pull-type kernels,
  // so the required_cnt of these kernels must be the same as executor_threshold_.
  MS_LOG(INFO) << "Required count for push-type and pull-type kernels is " << executor_threshold_;
  Executor::GetInstance().Initialize(func_graph_, executor_threshold_);
  ModelStore::GetInstance().Initialize();
  return;
}

void Server::RegisterRoundKernel() {
  MS_EXCEPTION_IF_NULL(iteration_);
  auto &rounds = iteration_->rounds();
  if (rounds.empty()) {
    MS_LOG(EXCEPTION) << "Server has no round registered.";
    return;
  }

  for (auto &round : rounds) {
    const std::string &name = round->name();
    std::shared_ptr<kernel::RoundKernel> round_kernel = kernel::RoundKernelFactory::GetInstance().Create(name);
    if (round_kernel == nullptr) {
      MS_LOG(EXCEPTION) << "Round kernel for round " << name << " is not registered.";
      return;
    }

    // For some round kernels, the threshold count should be set.
    round_kernel->InitKernel(round->threshold_count());
    round->BindRoundKernel(round_kernel);
  }
  return;
}

void Server::StartCommunicator() {
  MS_EXCEPTION_IF_NULL(communicator_with_server_);
  if (communicators_with_worker_.empty()) {
    MS_LOG(EXCEPTION) << "Communicators for communication with worker is empty.";
    return;
  }

  MS_LOG(INFO) << "Start communicator with server.";
  communicator_with_server_->Start();
  DistributedMetadataStore::GetInstance().Initialize(server_node_);
  CollectiveOpsImpl::GetInstance().Initialize(server_node_);
  DistributedCountService::GetInstance().Initialize(server_node_, kLeaderServerRank);
  MS_LOG(INFO) << "This server rank is " << server_node_->rank_id();

  MS_LOG(INFO) << "Start communicator with worker.";
  std::for_each(communicators_with_worker_.begin(), communicators_with_worker_.end(),
                [](const std::shared_ptr<ps::core::CommunicatorBase> &communicator) { communicator->Start(); });
}

void Server::ProcessBeforeScalingOut() {
  iteration_->ScalingBarrier();
  safemode_ = true;
}

void Server::ProcessBeforeScalingIn() {
  iteration_->ScalingBarrier();
  safemode_ = true;
}

void Server::ProcessAfterScalingOut() {
  std::unique_lock<std::mutex> lock(scaling_mtx_);
  if (server_node_ == nullptr) {
    return;
  }

  if (!DistributedMetadataStore::GetInstance().ReInitForScaling()) {
    MS_LOG(WARNING) << "DistributedMetadataStore reinitializing failed.";
  }
  if (!CollectiveOpsImpl::GetInstance().ReInitForScaling()) {
    MS_LOG(WARNING) << "DistributedMetadataStore reinitializing failed.";
  }
  if (!DistributedCountService::GetInstance().ReInitForScaling()) {
    MS_LOG(WARNING) << "DistributedCountService reinitializing failed.";
  }
  if (!iteration_->ReInitForScaling(IntToUint(server_node_->server_num()), server_node_->rank_id())) {
    MS_LOG(WARNING) << "Iteration reinitializing failed.";
  }
  if (!Executor::GetInstance().ReInitForScaling()) {
    MS_LOG(WARNING) << "Executor reinitializing failed.";
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  safemode_ = false;
}

void Server::ProcessAfterScalingIn() {
  std::unique_lock<std::mutex> lock(scaling_mtx_);
  if (server_node_ == nullptr) {
    return;
  }

  if (server_node_->rank_id() == UINT32_MAX) {
    MS_LOG(WARNING) << "This server the one to be scaled in. Server exiting.";
    std::for_each(communicators_with_worker_.begin(), communicators_with_worker_.end(),
                  [](const std::shared_ptr<ps::core::CommunicatorBase> &communicator) { communicator->Stop(); });
    communicator_with_server_->Stop();
    return;
  }

  // If the server is not the one to be scaled in, reintialize modules and recover service.
  if (!DistributedMetadataStore::GetInstance().ReInitForScaling()) {
    MS_LOG(WARNING) << "DistributedMetadataStore reinitializing failed.";
  }
  if (!CollectiveOpsImpl::GetInstance().ReInitForScaling()) {
    MS_LOG(WARNING) << "DistributedMetadataStore reinitializing failed.";
  }
  if (!DistributedCountService::GetInstance().ReInitForScaling()) {
    MS_LOG(WARNING) << "DistributedCountService reinitializing failed.";
  }
  if (!iteration_->ReInitForScaling(IntToUint(server_node_->server_num()), server_node_->rank_id())) {
    MS_LOG(WARNING) << "Iteration reinitializing failed.";
  }
  if (!Executor::GetInstance().ReInitForScaling()) {
    MS_LOG(WARNING) << "Executor reinitializing failed.";
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  safemode_ = false;
}
}  // namespace server
}  // namespace fl
}  // namespace mindspore
