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

#include "ps/core/scheduler_node.h"

namespace mindspore {
namespace ps {
namespace core {
SchedulerNode::~SchedulerNode() {
  MS_LOG(INFO) << "Stop scheduler node!";
  if (!Stop()) {
    MS_LOG(WARNING) << "Scheduler node stop failed.";
  }
}

bool SchedulerNode::Start(const uint32_t &timeout) {
  MS_LOG(INFO) << "[Scheudler start]: 1. Begin to start scheduler node!";
  if (PSContext::instance()->scheduler_manage_port() != 0) {
    MS_LOG(WARNING) << "Start the scheduler http service, the ip:" << PSContext::instance()->scheduler_ip()
                    << ", the port:" << PSContext::instance()->scheduler_manage_port();
    StartRestfulServer(PSContext::instance()->scheduler_ip(), PSContext::instance()->scheduler_manage_port(), 1);
  }
  Initialize();
  StartUpdateClusterStateTimer();
  if (!WaitForStart(timeout)) {
    MS_LOG(ERROR) << "Start Scheduler node timeout!";
    return false;
  }
  node_manager_.UpdateClusterState(ClusterState::CLUSTER_READY);
  MS_LOG(INFO) << "[Scheduler start]: 4. Successfully start scheduler, there are " << node_manager_.worker_num()
               << " workers and " << node_manager_.server_num() << " servers registered.";

  return true;
}

void SchedulerNode::ProcessHeartbeat(std::shared_ptr<TcpServer> server, std::shared_ptr<TcpConnection> conn,
                                     std::shared_ptr<MessageMeta> meta, const void *data, size_t size) {
  MS_EXCEPTION_IF_NULL(server);
  MS_EXCEPTION_IF_NULL(conn);
  MS_EXCEPTION_IF_NULL(meta);
  MS_EXCEPTION_IF_NULL(data);
  HeartbeatMessage heartbeat_message;
  heartbeat_message.ParseFromArray(data, size);

  node_manager_.UpdateHeartbeat(heartbeat_message.node_id());

  HeartbeatRespMessage heartbeat_resp_message;

  MS_LOG(DEBUG) << "The cluster state:" << CommUtil::ClusterStateToString(node_manager_.GetClusterState());
  heartbeat_resp_message.set_cluster_state(node_manager_.GetClusterState());

  std::vector<ServersMeta> servers_meta_list = node_manager_.FetchAllNodesMeta();

  *heartbeat_resp_message.mutable_servers_meta() = {servers_meta_list.begin(), servers_meta_list.end()};

  heartbeat_resp_message.set_is_worker_or_server0(node_manager_.IsWorkerOrServer0());

  if (!server->SendMessage(conn, meta, Protos::PROTOBUF, heartbeat_resp_message.SerializeAsString().data(),
                           heartbeat_resp_message.ByteSizeLong())) {
    MS_LOG(WARNING) << "Send heart beat failed.";
  }
}

void SchedulerNode::Initialize() {
  InitCommandHandler();
  CreateTcpServer();
  is_already_stopped_ = false;
  node_info_.node_id_ = CommUtil::GenerateUUID();
  node_info_.node_role_ = NodeRole::SCHEDULER;
  leader_scaler_ = std::make_unique<LeaderScaler>(this);
  MS_LOG(INFO) << "[Scheduler start]: 2. The node role is:" << CommUtil::NodeRoleToString(node_info_.node_role_)
               << ", the node id is:" << node_info_.node_id_ << " create a tcp server.";
}

void SchedulerNode::InitCommandHandler() {
  handlers_[NodeCommand::HEARTBEAT] = &SchedulerNode::ProcessHeartbeat;
  handlers_[NodeCommand::REGISTER] = &SchedulerNode::ProcessRegister;
  handlers_[NodeCommand::FINISH] = &SchedulerNode::ProcessFinish;
  handlers_[NodeCommand::FETCH_METADATA] = &SchedulerNode::ProcessFetchMetadata;
  handlers_[NodeCommand::SCALE_OUT_DONE] = &SchedulerNode::ProcessScaleOutDone;
  handlers_[NodeCommand::SCALE_IN_DONE] = &SchedulerNode::ProcessScaleInDone;
  handlers_[NodeCommand::SEND_EVENT] = &SchedulerNode::ProcessSendEvent;
}

void SchedulerNode::CreateTcpServer() {
  node_manager_.InitNode();

  std::string scheduler_host = PSContext::instance()->cluster_config().scheduler_host;
  uint32_t scheduler_port = PSContext::instance()->cluster_config().scheduler_port;
  server_ = std::make_shared<TcpServer>(scheduler_host, scheduler_port);
  server_->SetMessageCallback([&](std::shared_ptr<TcpConnection> conn, std::shared_ptr<MessageMeta> meta,
                                  const Protos &, const void *data, size_t size) {
    if (handlers_.count(meta->cmd()) == 0) {
      MS_LOG(EXCEPTION) << "The cmd:" << meta->cmd() << " is not supported!";
    }
    const auto &handler_ptr = handlers_[meta->cmd()];
    (this->*handler_ptr)(server_, conn, meta, data, size);
  });

  server_->Init();

  scheduler_thread_ = std::make_unique<std::thread>([&]() {
    MS_LOG(INFO) << "The scheduler node start a tcp server!";
    server_->Start();
  });
}

void SchedulerNode::ProcessRegister(std::shared_ptr<TcpServer> server, std::shared_ptr<TcpConnection> conn,
                                    std::shared_ptr<MessageMeta> meta, const void *data, size_t size) {
  MS_EXCEPTION_IF_NULL(server);
  MS_EXCEPTION_IF_NULL(conn);
  MS_EXCEPTION_IF_NULL(meta);
  MS_EXCEPTION_IF_NULL(data);
  MS_LOG(INFO) << "The scheduler process a register message!";
  RegisterMessage register_message;
  if (!register_message.ParseFromArray(data, SizeToInt(size))) {
    MS_LOG(WARNING) << "Parse data failed.";
  }

  // assign worker node and server node rank id
  uint32_t rank_id = node_manager_.NextRankId(register_message, meta);
  if (rank_id == UINT32_MAX) {
    MS_LOG(WARNING) << "The rank id is wrong!";
  }
  const std::string &node_id = register_message.node_id();
  node_manager_.UpdateHeartbeat(node_id);

  RegisterRespMessage register_resp_message;
  register_resp_message.set_node_id(node_id);

  server->SendMessage(conn, meta, Protos::PROTOBUF, register_resp_message.SerializeAsString().data(),
                      register_resp_message.ByteSizeLong());

  if (node_manager_.IsAllNodesRegistered()) {
    is_ready_ = true;
    MS_LOG(INFO) << "There are " << node_manager_.worker_num() << " workers and " << node_manager_.server_num()
                 << " servers registered to scheduer, so the scheduler send meta data to worker/server.";
    if (node_manager_.GetClusterState() == ClusterState::CLUSTER_SCALE_IN) {
      auto nodes = node_manager_.nodes_info();
      for (const auto &id : scale_in_node_ids_) {
        MS_LOG(INFO) << "The scheduler send metadata to scale in node:" << id;
        auto scale_in_client = GetOrCreateClient(nodes[id]);
        SendMetadata(scale_in_client, nodes[id].rank_id_);
      }
    }
    node_manager_.UpdateNodesInfo();
    auto node_infos = node_manager_.nodes_info();
    for (const auto &kvs : node_infos) {
      auto client = GetOrCreateClient(kvs.second);
      SendMetadata(client, kvs.second.rank_id_);
    }
    node_manager_.UpdateClusterState(ClusterState::CLUSTER_READY);
    wait_start_cond_.notify_all();
  }
}

void SchedulerNode::ProcessFinish(std::shared_ptr<TcpServer> server, std::shared_ptr<TcpConnection> conn,
                                  std::shared_ptr<MessageMeta> meta, const void *data, size_t size) {
  MS_EXCEPTION_IF_NULL(server);
  MS_EXCEPTION_IF_NULL(conn);
  MS_EXCEPTION_IF_NULL(meta);
  MS_EXCEPTION_IF_NULL(data);
  auto finish_message = std::make_unique<std::string>(reinterpret_cast<const char *>(data), size);
  std::string node_id = *finish_message;
  MS_LOG(INFO) << "Process finish message from node id:" << node_id;
  server->SendMessage(conn, meta, Protos::PROTOBUF, data, size);

  auto iter = std::find_if(scale_in_node_ids_.begin(), scale_in_node_ids_.end(), [node_id](auto item) {
    if (node_id == item) {
      MS_LOG(INFO) << "The finish node is a scale in node.";
      return true;
    }
    return false;
  });
  if (iter != scale_in_node_ids_.end()) {
    return;
  }

  node_manager_.AddFinishNode(node_id);
  if (node_manager_.IsAllNodesFinished()) {
    auto node_infos = node_manager_.nodes_info();
    for (const auto &kvs : node_infos) {
      auto client = GetOrCreateClient(kvs.second);
      SendFinish(client);
    }
    is_finish_ = true;
    node_manager_.UpdateClusterState(ClusterState::CLUSTER_EXIT);
    wait_finish_cond_.notify_all();
  }
}

void SchedulerNode::ProcessFetchMetadata(std::shared_ptr<TcpServer> server, std::shared_ptr<TcpConnection> conn,
                                         std::shared_ptr<MessageMeta> meta, const void *data, size_t size) {
  MS_EXCEPTION_IF_NULL(server);
  MS_EXCEPTION_IF_NULL(conn);
  MS_EXCEPTION_IF_NULL(meta);
  MS_EXCEPTION_IF_NULL(data);
  FetchServersRespMessage fetch_servers_message;
  std::vector<ServersMeta> servers_meta_list = node_manager_.FetchServersMeta();

  *fetch_servers_message.mutable_servers_meta() = {servers_meta_list.begin(), servers_meta_list.end()};

  server->SendMessage(conn, meta, Protos::PROTOBUF, fetch_servers_message.SerializeAsString().data(),
                      fetch_servers_message.ByteSizeLong());
}

void SchedulerNode::ProcessScaleOutDone(std::shared_ptr<TcpServer> server, std::shared_ptr<TcpConnection> conn,
                                        std::shared_ptr<MessageMeta> meta, const void *data, size_t size) {
  MS_EXCEPTION_IF_NULL(server);
  MS_EXCEPTION_IF_NULL(conn);
  MS_EXCEPTION_IF_NULL(meta);
  MS_EXCEPTION_IF_NULL(data);
  ScaleOutDoneMessage scale_out_done_message;
  scale_out_done_message.ParseFromArray(data, size);
  std::string node_id = scale_out_done_message.node_id();
  MS_LOG(INFO) << "The scheduler process a scale_out_done message from node id:" << node_id;
  node_manager_.AddScaleOutDoneNode(node_id);

  server->SendMessage(conn, meta, Protos::PROTOBUF, data, size);

  if (node_manager_.IsAllNodesScaleOutDone()) {
    auto node_infos = node_manager_.nodes_info();
    for (const auto &kvs : node_infos) {
      auto client = GetOrCreateClient(kvs.second);
      SendScaleOutDone(client);
    }
    is_ready_ = true;
    node_manager_.UpdateClusterState(ClusterState::CLUSTER_READY);
  }
}

void SchedulerNode::ProcessScaleInDone(std::shared_ptr<TcpServer> server, std::shared_ptr<TcpConnection> conn,
                                       std::shared_ptr<MessageMeta> meta, const void *data, size_t size) {
  MS_EXCEPTION_IF_NULL(server);
  MS_EXCEPTION_IF_NULL(conn);
  MS_EXCEPTION_IF_NULL(meta);
  MS_EXCEPTION_IF_NULL(data);
  ScaleInDoneMessage scale_in_done_message;
  scale_in_done_message.ParseFromArray(data, size);
  std::string node_id = scale_in_done_message.node_id();
  MS_LOG(INFO) << "The scheduler process a scale_in_done message from node id:" << node_id;
  node_manager_.AddScaleInDoneNode(node_id);

  server->SendMessage(conn, meta, Protos::PROTOBUF, data, size);

  if (node_manager_.IsAllNodesScaleInDone()) {
    auto node_infos = node_manager_.nodes_info();
    for (const auto &kvs : node_infos) {
      auto client = GetOrCreateClient(kvs.second);
      SendScaleInDone(client);
    }
    is_ready_ = true;
    node_manager_.UpdateClusterState(ClusterState::CLUSTER_READY);
  }
}

void SchedulerNode::ProcessSendEvent(std::shared_ptr<TcpServer> server, std::shared_ptr<TcpConnection> conn,
                                     std::shared_ptr<MessageMeta> meta, const void *data, size_t size) {
  MS_EXCEPTION_IF_NULL(server);
  MS_EXCEPTION_IF_NULL(conn);
  MS_EXCEPTION_IF_NULL(meta);
  MS_EXCEPTION_IF_NULL(data);
  EventMessage event_message;
  event_message.ParseFromArray(data, size);
  std::string node_id = event_message.node_id();
  uint32_t event = event_message.event();
  MS_LOG(DEBUG) << "The scheduler process a event message from node id:" << node_id;

  server->SendMessage(conn, meta, Protos::PROTOBUF, data, size);

  auto node_infos = node_manager_.nodes_info();
  for (const auto &kvs : node_infos) {
    auto client = GetOrCreateClient(kvs.second);
    SendEvent(client, event);
  }
}

void SchedulerNode::SendMetadata(const std::shared_ptr<TcpClient> &client, uint32_t rank_id) {
  MS_EXCEPTION_IF_NULL(client);
  auto message_meta = std::make_shared<MessageMeta>();
  message_meta->set_cmd(NodeCommand::SEND_METADATA);

  SendMetadataMessage send_metadata_message;
  std::vector<ServersMeta> servers_meta_list = node_manager_.FetchServersMeta();
  send_metadata_message.set_worker_num(node_manager_.worker_num());
  send_metadata_message.set_server_num(node_manager_.server_num());
  send_metadata_message.set_cluster_state(node_manager_.GetClusterState());
  send_metadata_message.set_rank_id(rank_id);

  *send_metadata_message.mutable_servers_meta() = {servers_meta_list.begin(), servers_meta_list.end()};

  if (!SendMessageAsync(client, message_meta, Protos::PROTOBUF, send_metadata_message.SerializeAsString().data(),
                        send_metadata_message.ByteSizeLong())) {
    MS_LOG(EXCEPTION) << "The node role:" << CommUtil::NodeRoleToString(node_info_.node_role_)
                      << " the node id:" << node_info_.node_id_ << " send metadata timeout!";
  }

  MS_LOG(INFO) << "The node role:" << CommUtil::NodeRoleToString(node_info_.node_role_)
               << " the node id:" << node_info_.node_id_ << "is sending metadata to workers and servers!";
}

void SchedulerNode::SendFinish(const std::shared_ptr<TcpClient> &client) {
  MS_EXCEPTION_IF_NULL(client);
  auto message_meta = std::make_shared<MessageMeta>();
  message_meta->set_cmd(NodeCommand::FINISH);

  // The scheduler does not need to bring any data when sending the finish command
  std::string resp_data;

  if (!SendMessageSync(client, message_meta, Protos::PROTOBUF, resp_data.data(), resp_data.size())) {
    MS_LOG(EXCEPTION) << "The node role:" << CommUtil::NodeRoleToString(node_info_.node_role_)
                      << " the node id:" << node_info_.node_id_ << " send finish timeout!";
  }

  MS_LOG(INFO) << "The node role:" << CommUtil::NodeRoleToString(node_info_.node_role_)
               << " the node id:" << node_info_.node_id_ << "is sending finish to workers and servers!";
}

void SchedulerNode::SendScaleOutDone(const std::shared_ptr<TcpClient> &client) {
  MS_EXCEPTION_IF_NULL(client);
  auto message_meta = std::make_shared<MessageMeta>();
  message_meta->set_cmd(NodeCommand::SCALE_OUT_DONE);

  // The scheduler does not need to bring any data when sending the scale_out_done command
  std::string resp_data;

  if (!SendMessageSync(client, message_meta, Protos::PROTOBUF, resp_data.data(), resp_data.size())) {
    MS_LOG(EXCEPTION) << "The node role:" << CommUtil::NodeRoleToString(node_info_.node_role_)
                      << " the node id:" << node_info_.node_id_ << " send scale_out_done timeout!";
  }

  MS_LOG(INFO) << "The node role:" << CommUtil::NodeRoleToString(node_info_.node_role_)
               << " the node id:" << node_info_.node_id_ << "is sending scale_out_done to workers and servers!";
}

void SchedulerNode::SendScaleInDone(const std::shared_ptr<TcpClient> &client) {
  MS_EXCEPTION_IF_NULL(client);
  auto message_meta = std::make_shared<MessageMeta>();
  message_meta->set_cmd(NodeCommand::SCALE_IN_DONE);

  // The scheduler does not need to bring any data when sending the scale_in_done command
  std::string resp_data;

  if (!SendMessageSync(client, message_meta, Protos::PROTOBUF, resp_data.data(), resp_data.size())) {
    MS_LOG(EXCEPTION) << "The node role:" << CommUtil::NodeRoleToString(node_info_.node_role_)
                      << " the node id:" << node_info_.node_id_ << " send scale_in_done timeout!";
  }

  MS_LOG(INFO) << "The node role:" << CommUtil::NodeRoleToString(node_info_.node_role_)
               << " the node id:" << node_info_.node_id_ << "is sending scale_in_done to workers and servers!";
}

void SchedulerNode::SendEvent(const std::shared_ptr<TcpClient> &client, const uint32_t &event) {
  MS_EXCEPTION_IF_NULL(client);
  auto message_meta = std::make_shared<MessageMeta>();
  message_meta->set_cmd(NodeCommand::SEND_EVENT);

  EventRespMessage event_resp_message;
  event_resp_message.set_event(event);

  if (!SendMessageSync(client, message_meta, Protos::PROTOBUF, event_resp_message.SerializeAsString().data(),
                       event_resp_message.ByteSizeLong())) {
    MS_LOG(ERROR) << "The node role:" << CommUtil::NodeRoleToString(node_info_.node_role_)
                  << " the node id:" << node_info_.node_id_ << " send event resp timeout!";
    return;
  }

  MS_LOG(DEBUG) << "The node role:" << CommUtil::NodeRoleToString(node_info_.node_role_)
                << " the node id:" << node_info_.node_id_ << "is sending event resp to workers and servers!";
}

void SchedulerNode::StartUpdateClusterStateTimer() {
  MS_LOG(INFO) << "[Scheduler start]: 3. The scheduler start a heartbeat timer!";
  update_state_thread_ = std::make_unique<std::thread>([&]() {
    auto start_time = std::chrono::steady_clock::now();
    while (!is_finish_.load()) {
      // 1. update cluster timeout
      if (!is_ready_ && (std::chrono::steady_clock::now() - start_time >
                         std::chrono::seconds(PSContext::instance()->cluster_config().cluster_available_timeout))) {
        node_manager_.CheckClusterTimeout();
      }
      std::this_thread::sleep_for(std::chrono::seconds(PSContext::instance()->cluster_config().heartbeat_interval));
      node_manager_.UpdateCluster();

      if (node_manager_.GetClusterState() == ClusterState::CLUSTER_EXIT) {
        std::this_thread::sleep_for(
          std::chrono::seconds(PSContext::instance()->cluster_config().heartbeat_interval * 2));
        is_finish_ = true;
        wait_finish_cond_.notify_all();
      }
    }
  });
}

const std::shared_ptr<TcpClient> &SchedulerNode::GetOrCreateClient(const NodeInfo &node_info) {
  if (connected_nodes_.count(node_info.node_id_)) {
    return connected_nodes_[node_info.node_id_];
  } else {
    std::string ip = node_info.ip_;
    uint16_t port = node_info.port_;
    auto client = std::make_shared<TcpClient>(ip, port);
    client->SetMessageCallback([&](std::shared_ptr<MessageMeta> meta, const Protos &protos, const void *data,
                                   size_t size) { NotifyMessageArrival(meta); });
    client->Init();
    if (is_client_started_ == false) {
      is_client_started_ = true;
      client_thread_ = std::make_unique<std::thread>([&]() {
        MS_LOG(INFO) << "The node start a tcp client!";
        client->Start();
      });
    }
    connected_nodes_[node_info.node_id_] = client;
    return connected_nodes_[node_info.node_id_];
  }
}

bool SchedulerNode::Stop() {
  MS_LOG(INFO) << "Stop scheduler node!";
  if (!is_already_stopped_) {
    is_already_stopped_ = true;
    update_state_thread_->join();
    server_->Stop();
    scheduler_thread_->join();
    if (!connected_nodes_.empty()) {
      for (auto &connected_node : connected_nodes_) {
        connected_node.second->Stop();
      }
    }
    if (client_thread_ != nullptr && client_thread_->joinable()) {
      client_thread_->join();
    }
    is_ready_ = true;
  }
  if (PSContext::instance()->scheduler_manage_port() != 0) {
    MS_LOG(WARNING) << "Stop the scheduler http service, the ip:" << PSContext::instance()->scheduler_ip()
                    << ", the port:" << PSContext::instance()->scheduler_manage_port();
    StopRestfulServer();
  }
  return true;
}

bool SchedulerNode::Finish(const uint32_t &) {
  MS_LOG(INFO) << "[Scheduler finish]: 1. Begin to finish scheduler node!";
  std::unique_lock<std::mutex> lock(wait_finish_mutex_);
  wait_finish_cond_.wait(lock, [this] {
    if (this->is_finish_.load()) {
      MS_LOG(INFO) << "[Scheduler finish]: 2. Successfully finish scheduler!";
    }
    return this->is_finish_.load();
  });
  return true;
}

void SchedulerNode::ProcessScaleOut(std::shared_ptr<HttpMessageHandler> resp) {
  RequestProcessResult status(RequestProcessResultCode::kSuccess);
  status = resp->ParsePostMessageToJson();
  if (status != RequestProcessResultCode::kSuccess) {
    resp->ErrorResponse(HTTP_BADREQUEST, status);
    return;
  }

  int32_t scale_worker_num = 0;
  status = resp->ParseValueFromKey(kWorkerNum, &scale_worker_num);
  if (status != RequestProcessResultCode::kSuccess) {
    resp->ErrorResponse(HTTP_BADREQUEST, status);
    return;
  }

  int32_t scale_server_num = 0;
  status = resp->ParseValueFromKey(kServerNum, &scale_server_num);
  if (status != RequestProcessResultCode::kSuccess) {
    resp->ErrorResponse(HTTP_BADREQUEST, status);
    return;
  }

  status = CheckIfClusterReady();
  if (status != RequestProcessResultCode::kSuccess) {
    resp->ErrorResponse(HTTP_BADREQUEST, status);
    return;
  }

  int32_t total_worker_num = scale_worker_num + node_manager_.worker_num();
  int32_t total_server_num = scale_server_num + node_manager_.server_num();

  MS_LOG(INFO) << "After scale out, the total worker num:" << total_worker_num
               << ", the total server num:" << total_server_num;

  node_manager_.set_worker_num(total_worker_num);
  node_manager_.set_server_num(total_server_num);
  node_manager_.set_total_node_num(total_worker_num + total_server_num);

  node_manager_.UpdateClusterState(ClusterState::CLUSTER_SCALE_OUT);
  auto node_infos = node_manager_.nodes_info();
  node_manager_.ResetMetadata();
  for (const auto &kvs : node_infos) {
    auto client = GetOrCreateClient(kvs.second);
    leader_scaler_->ScaleOutAsync(client, node_manager_);
  }
  MS_LOG(INFO) << "Scheduler send scale out successful.";

  nlohmann::json js;
  js["message"] = "Cluster begin to scale out.";
  resp->AddRespString(js.dump());
  resp->AddRespHeadParam("Content-Type", "application/json");

  resp->SetRespCode(HTTP_OK);
  resp->SendResponse();
}

/*
 * The response body format.
 * {
 *    "node_ids": ["node_id1", "node_id2"]
 * }
 */
void SchedulerNode::ProcessScaleIn(std::shared_ptr<HttpMessageHandler> resp) {
  RequestProcessResult status(RequestProcessResultCode::kSuccess);
  status = resp->ParsePostMessageToJson();
  if (status != RequestProcessResultCode::kSuccess) {
    resp->ErrorResponse(HTTP_BADREQUEST, status);
  }

  status = CheckIfClusterReady();
  if (status != RequestProcessResultCode::kSuccess) {
    resp->ErrorResponse(HTTP_BADREQUEST, status);
    return;
  }

  scale_in_node_ids_.clear();
  status = resp->ParseNodeIdsFromKey(kNodesIds, &scale_in_node_ids_);
  if (status != RequestProcessResultCode::kSuccess) {
    resp->ErrorResponse(HTTP_BADREQUEST, status);
    return;
  }

  status = CheckIfNodeIdLegal(scale_in_node_ids_);
  if (status != RequestProcessResultCode::kSuccess) {
    resp->ErrorResponse(HTTP_BADREQUEST, status);
    return;
  }

  MS_LOG(WARNING) << "The scale in node ids:" << scale_in_node_ids_;

  std::unordered_map<std::string, bool> scale_in_nodes;

  int32_t scale_worker_num = 0;
  int32_t scale_server_num = 0;
  auto node_infos = node_manager_.nodes_info();
  node_manager_.UpdateClusterState(ClusterState::CLUSTER_SCALE_IN);
  node_manager_.ResetMetadata(scale_in_node_ids_);
  for (auto const &val : scale_in_node_ids_) {
    if (node_infos.count(val)) {
      scale_in_nodes[val] = true;
      NodeInfo info = node_infos[val];
      if (info.node_role_ == NodeRole::WORKER) {
        scale_worker_num++;
      } else if (info.node_role_ == NodeRole::SERVER) {
        scale_server_num++;
      }
    }
  }

  MS_LOG(INFO) << "The scale worker num:" << scale_worker_num << ", the scale server num:" << scale_server_num;

  int32_t total_worker_num = node_manager_.worker_num() - scale_worker_num;
  int32_t total_server_num = node_manager_.server_num() - scale_server_num;

  node_manager_.set_worker_num(total_worker_num);
  node_manager_.set_server_num(total_server_num);
  node_manager_.set_total_node_num(total_worker_num + total_server_num);
  for (const auto &kvs : node_infos) {
    auto client = GetOrCreateClient(kvs.second);
    bool is_node_scale_in = false;
    if (scale_in_nodes.count(kvs.first)) {
      is_node_scale_in = true;
    }
    leader_scaler_->ScaleInAsync(client, node_manager_, is_node_scale_in);
  }

  nlohmann::json js;
  js["message"] = "Cluster begin to scale in.";
  resp->AddRespString(js.dump());
  resp->AddRespHeadParam("Content-Type", "application/json");

  resp->SetRespCode(HTTP_OK);
  resp->SendResponse();
}

/*
 * The response body format.
 * {
 *    "message": "Get nodes info successful.",
 *    "node_ids": [
 *        {
 *            "node_id": "node_id1",
 *            "rank_id": "0",
 *            "role": "SERVER"
 *        },
 *        {
 *            "node_id": "node_id2",
 *            "rank_id": "1",
 *            "role": "WORKER"
 *        }
 *    ]
 * }
 */
void SchedulerNode::ProcessGetNodesInfo(std::shared_ptr<HttpMessageHandler> resp) {
  nlohmann::json js;
  js["message"] = "Get nodes info successful.";
  auto node_infos = node_manager_.nodes_info();
  for (const auto &kvs : node_infos) {
    std::unordered_map<std::string, std::string> res;
    res["node_id"] = kvs.second.node_id_;
    res["rank_id"] = std::to_string(kvs.second.rank_id_);
    res["role"] = CommUtil::NodeRoleToString(kvs.second.node_role_);
    js["node_ids"].push_back(res);
  }

  resp->AddRespString(js.dump());
  resp->AddRespHeadParam("Content-Type", "application/json");

  resp->SetRespCode(HTTP_OK);
  resp->SendResponse();
}

/*
 * The response body format.
 * {
 *    "message": "Get cluster state successful.",
 *    "cluster_state": "CLUSTER_READY"
 * }
 */
void SchedulerNode::ProcessGetClusterState(std::shared_ptr<HttpMessageHandler> resp) {
  nlohmann::json js;
  js["message"] = "Get cluster state successful.";
  auto cluster_state = node_manager_.GetClusterState();
  js["cluster_state"] = CommUtil::ClusterStateToString(cluster_state);

  resp->AddRespString(js.dump());
  resp->AddRespHeadParam("Content-Type", "application/json");

  resp->SetRespCode(HTTP_OK);
  resp->SendResponse();
}

RequestProcessResult SchedulerNode::CheckIfClusterReady() {
  RequestProcessResult result(RequestProcessResultCode::kSuccess);
  if (node_manager_.GetClusterState() != ClusterState::CLUSTER_READY) {
    std::string message = "The cluster is not ready.";
    ERROR_STATUS(result, RequestProcessResultCode::kSystemError, message);
    return result;
  }
  return result;
}

RequestProcessResult SchedulerNode::CheckIfNodeIdLegal(const std::vector<std::string> &node_ids) {
  RequestProcessResult result(RequestProcessResultCode::kSuccess);
  if (node_ids.size() == 0) {
    std::string message = "The node ids should not be empty.";
    ERROR_STATUS(result, RequestProcessResultCode::kInvalidInputs, message);
    return result;
  }

  auto node_infos = node_manager_.nodes_info();

  for (auto val : node_ids) {
    if (!node_infos.count(val)) {
      std::string message = "The node id:" + val + " is illegal.";
      MS_LOG(ERROR) << message;
      ERROR_STATUS(result, RequestProcessResultCode::kInvalidInputs, message);
      return result;
    }

    if (node_infos[val].node_role_ == NodeRole::SERVER && node_infos[val].rank_id_ == 0) {
      std::string error_message = "The node id:" + val + " is rank 0 of server, should not be scale in.";
      MS_LOG(ERROR) << error_message;
      ERROR_STATUS(result, RequestProcessResultCode::kInvalidInputs, error_message);
      return result;
    }

    if (node_infos[val].node_role_ == NodeRole::WORKER) {
      std::string error_message = "The node id:" + val + " is the role of worker, should not be scale in.";
      MS_LOG(ERROR) << error_message;
      ERROR_STATUS(result, RequestProcessResultCode::kInvalidInputs, error_message);
      return result;
    }
  }

  return result;
}

void SchedulerNode::StartRestfulServer(const std::string &address, std::uint16_t port, size_t thread_num) {
  MS_LOG(INFO) << "Scheduler start https server.";
  http_server_ = std::make_shared<HttpServer>(address, port, thread_num);

  OnRequestReceive scale_out = std::bind(&SchedulerNode::ProcessScaleOut, this, std::placeholders::_1);
  callbacks_["/scaleout"] = scale_out;
  http_server_->RegisterRoute("/scaleout", &callbacks_["/scaleout"]);

  OnRequestReceive scale_in = std::bind(&SchedulerNode::ProcessScaleIn, this, std::placeholders::_1);
  callbacks_["/scalein"] = scale_in;
  http_server_->RegisterRoute("/scalein", &callbacks_["/scalein"]);

  OnRequestReceive nodes = std::bind(&SchedulerNode::ProcessGetNodesInfo, this, std::placeholders::_1);
  callbacks_["/nodes"] = nodes;
  http_server_->RegisterRoute("/nodes", &callbacks_["/nodes"]);

  OnRequestReceive cluster_state = std::bind(&SchedulerNode::ProcessGetClusterState, this, std::placeholders::_1);
  callbacks_["/state"] = cluster_state;
  http_server_->RegisterRoute("/state", &callbacks_["/state"]);

  if (!http_server_->InitServer()) {
    MS_LOG(EXCEPTION) << "The scheduler init http server failed.";
  }

  http_server_->Start(false);
  restful_thread_ = std::make_unique<std::thread>([&]() { http_server_->Wait(); });
}

void SchedulerNode::StopRestfulServer() {
  MS_LOG(INFO) << "Scheduler stop https server.";
  http_server_->Stop();
  if (restful_thread_ != nullptr && restful_thread_->joinable()) {
    restful_thread_->join();
  }
}
}  // namespace core
}  // namespace ps
}  // namespace mindspore
