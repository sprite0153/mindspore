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

#include "fl/server/distributed_count_service.h"
#include <string>
#include <memory>
#include <vector>

namespace mindspore {
namespace fl {
namespace server {
void DistributedCountService::Initialize(const std::shared_ptr<ps::core::ServerNode> &server_node,
                                         uint32_t counting_server_rank) {
  server_node_ = server_node;
  MS_EXCEPTION_IF_NULL(server_node_);
  local_rank_ = server_node_->rank_id();
  server_num_ = ps::PSContext::instance()->initial_server_num();
  counting_server_rank_ = counting_server_rank;
  return;
}

void DistributedCountService::RegisterMessageCallback(const std::shared_ptr<ps::core::TcpCommunicator> &communicator) {
  communicator_ = communicator;
  MS_EXCEPTION_IF_NULL(communicator_);
  communicator_->RegisterMsgCallBack(
    "count", std::bind(&DistributedCountService::HandleCountRequest, this, std::placeholders::_1));
  communicator_->RegisterMsgCallBack(
    "countReachThreshold",
    std::bind(&DistributedCountService::HandleCountReachThresholdRequest, this, std::placeholders::_1));
  communicator_->RegisterMsgCallBack(
    "counterEvent", std::bind(&DistributedCountService::HandleCounterEvent, this, std::placeholders::_1));
}

void DistributedCountService::RegisterCounter(const std::string &name, size_t global_threshold_count,
                                              const CounterHandlers &counter_handlers) {
  if (!counter_handlers.first_count_handler || !counter_handlers.last_count_handler) {
    MS_LOG(EXCEPTION) << "First count handler or last count handler is not set.";
    return;
  }
  if (global_threshold_count_.count(name) != 0) {
    MS_LOG(INFO) << "Counter for " << name << " is already set.";
    return;
  }

  MS_LOG(INFO) << "Rank " << local_rank_ << " register counter for " << name << " count:" << global_threshold_count;
  // If the server is the leader server, it needs to set the counter handlers and do the real counting.
  if (local_rank_ == counting_server_rank_) {
    global_current_count_[name] = {};
    global_threshold_count_[name] = global_threshold_count;
    mutex_[name];
  }
  counter_handlers_[name] = counter_handlers;
  return;
}

bool DistributedCountService::Count(const std::string &name, const std::string &id) {
  MS_LOG(INFO) << "Rank " << local_rank_ << " reports count for " << name << " of " << id;
  if (local_rank_ == counting_server_rank_) {
    if (global_threshold_count_.count(name) == 0) {
      MS_LOG(ERROR) << "Counter for " << name << " is not registered.";
      return false;
    }

    std::unique_lock<std::mutex> lock(mutex_[name]);
    if (global_current_count_[name].size() >= global_threshold_count_[name]) {
      MS_LOG(ERROR) << "Count for " << name << " is already enough. Threshold count is "
                    << global_threshold_count_[name];
      return false;
    }

    MS_LOG(INFO) << "Leader server increase count for " << name << " of " << id;
    global_current_count_[name].insert(id);
    if (!TriggerCounterEvent(name)) {
      MS_LOG(ERROR) << "Leader server trigger count event failed.";
      return false;
    }
  } else {
    // If this server is a follower server, it needs to send CountRequest to the leader server.
    CountRequest report_count_req;
    report_count_req.set_name(name);
    report_count_req.set_id(id);

    std::shared_ptr<std::vector<unsigned char>> report_cnt_rsp_msg = nullptr;
    if (!communicator_->SendPbRequest(report_count_req, counting_server_rank_, ps::core::TcpUserCommand::kCount,
                                      &report_cnt_rsp_msg)) {
      MS_LOG(ERROR) << "Sending reporting count message to leader server failed for " << name;
      return false;
    }

    CountResponse count_rsp;
    count_rsp.ParseFromArray(report_cnt_rsp_msg->data(), SizeToInt(report_cnt_rsp_msg->size()));
    if (!count_rsp.result()) {
      MS_LOG(ERROR) << "Reporting count failed:" << count_rsp.reason();
      return false;
    }
  }
  return true;
}

bool DistributedCountService::CountReachThreshold(const std::string &name) {
  MS_LOG(INFO) << "Rank " << local_rank_ << " query whether count reaches threshold for " << name;
  if (local_rank_ == counting_server_rank_) {
    if (global_threshold_count_.count(name) == 0) {
      MS_LOG(ERROR) << "Counter for " << name << " is not set.";
      return false;
    }

    std::unique_lock<std::mutex> lock(mutex_[name]);
    return global_current_count_[name].size() == global_threshold_count_[name];
  } else {
    CountReachThresholdRequest count_reach_threshold_req;
    count_reach_threshold_req.set_name(name);

    std::shared_ptr<std::vector<unsigned char>> query_cnt_enough_rsp_msg = nullptr;
    if (!communicator_->SendPbRequest(count_reach_threshold_req, counting_server_rank_,
                                      ps::core::TcpUserCommand::kReachThreshold, &query_cnt_enough_rsp_msg)) {
      MS_LOG(ERROR) << "Sending querying whether count reaches threshold message to leader server failed for " << name;
      return false;
    }

    CountReachThresholdResponse count_reach_threshold_rsp;
    count_reach_threshold_rsp.ParseFromArray(query_cnt_enough_rsp_msg->data(),
                                             SizeToInt(query_cnt_enough_rsp_msg->size()));
    return count_reach_threshold_rsp.is_enough();
  }
}

void DistributedCountService::ResetCounter(const std::string &name) {
  if (local_rank_ == counting_server_rank_) {
    MS_LOG(DEBUG) << "Leader server reset count for " << name;
    global_current_count_[name].clear();
  }
  return;
}

bool DistributedCountService::ReInitForScaling() {
  // If DistributedCountService is not initialized yet but the scaling event is triggered, do not throw exception.
  if (server_node_ == nullptr) {
    return true;
  }

  MS_LOG(INFO) << "Cluster scaling completed. Reinitialize for distributed count service.";
  local_rank_ = server_node_->rank_id();
  server_num_ = IntToUint(server_node_->server_num());
  MS_LOG(INFO) << "After scheduler scaling, this server's rank is " << local_rank_ << ", server number is "
               << server_num_;

  // Clear old counter data of this server.
  global_current_count_.clear();
  global_threshold_count_.clear();
  counter_handlers_.clear();
  return true;
}

void DistributedCountService::HandleCountRequest(const std::shared_ptr<ps::core::MessageHandler> &message) {
  if (message == nullptr) {
    MS_LOG(ERROR) << "Message is nullptr.";
    return;
  }

  CountRequest report_count_req;
  report_count_req.ParseFromArray(message->data(), SizeToInt(message->len()));
  const std::string &name = report_count_req.name();
  const std::string &id = report_count_req.id();

  CountResponse count_rsp;
  std::unique_lock<std::mutex> lock(mutex_[name]);
  // If leader server has no counter for the name registered, return an error.
  if (global_threshold_count_.count(name) == 0) {
    std::string reason = "Counter for " + name + " is not registered.";
    count_rsp.set_result(false);
    count_rsp.set_reason(reason);
    MS_LOG(ERROR) << reason;
    communicator_->SendResponse(count_rsp.SerializeAsString().data(), count_rsp.SerializeAsString().size(), message);
    return;
  }

  // If leader server already has enough count for the name, return an error.
  if (global_current_count_[name].size() >= global_threshold_count_[name]) {
    std::string reason =
      "Count for " + name + " is already enough. Threshold count is " + std::to_string(global_threshold_count_[name]);
    count_rsp.set_result(false);
    count_rsp.set_reason(reason);
    MS_LOG(ERROR) << reason;
    communicator_->SendResponse(count_rsp.SerializeAsString().data(), count_rsp.SerializeAsString().size(), message);
    return;
  }

  // Insert the id for the counter, which means the count for the name is increased.
  MS_LOG(INFO) << "Leader server increase count for " << name << " of " << id;
  global_current_count_[name].insert(id);
  if (!TriggerCounterEvent(name)) {
    std::string reason = "Trigger count event for " + name + " of " + id + " failed.";
    count_rsp.set_result(false);
    count_rsp.set_reason(reason);
  } else {
    count_rsp.set_result(true);
    count_rsp.set_reason("success");
  }
  communicator_->SendResponse(count_rsp.SerializeAsString().data(), count_rsp.SerializeAsString().size(), message);
  return;
}

void DistributedCountService::HandleCountReachThresholdRequest(
  const std::shared_ptr<ps::core::MessageHandler> &message) {
  if (message == nullptr) {
    MS_LOG(ERROR) << "Message is nullptr.";
    return;
  }

  CountReachThresholdRequest count_reach_threshold_req;
  count_reach_threshold_req.ParseFromArray(message->data(), SizeToInt(message->len()));
  const std::string &name = count_reach_threshold_req.name();

  std::unique_lock<std::mutex> lock(mutex_[name]);
  if (global_threshold_count_.count(name) == 0) {
    MS_LOG(ERROR) << "Counter for " << name << " is not registered.";
    return;
  }

  CountReachThresholdResponse count_reach_threshold_rsp;
  count_reach_threshold_rsp.set_is_enough(global_current_count_[name].size() == global_threshold_count_[name]);
  communicator_->SendResponse(count_reach_threshold_rsp.SerializeAsString().data(),
                              count_reach_threshold_rsp.SerializeAsString().size(), message);
  return;
}

void DistributedCountService::HandleCounterEvent(const std::shared_ptr<ps::core::MessageHandler> &message) {
  if (message == nullptr) {
    MS_LOG(ERROR) << "Message is nullptr.";
    return;
  }

  // Respond as soon as possible so the leader server won't wait for each follower servers to finish calling the
  // callbacks.
  std::string couter_event_rsp_msg = "success";
  communicator_->SendResponse(couter_event_rsp_msg.data(), couter_event_rsp_msg.size(), message);

  CounterEvent counter_event;
  counter_event.ParseFromArray(message->data(), SizeToInt(message->len()));
  const auto &type = counter_event.type();
  const auto &name = counter_event.name();

  MS_LOG(DEBUG) << "Rank " << local_rank_ << " do counter event " << type << " for " << name;
  if (type == CounterEventType::FIRST_CNT) {
    counter_handlers_[name].first_count_handler(message);
  } else if (type == CounterEventType::LAST_CNT) {
    counter_handlers_[name].last_count_handler(message);
  } else {
    MS_LOG(ERROR) << "DistributedCountService event type " << type << " is invalid.";
    return;
  }
  return;
}

bool DistributedCountService::TriggerCounterEvent(const std::string &name) {
  MS_LOG(INFO) << "Current count for " << name << " is " << global_current_count_[name].size()
               << ", threshold count is " << global_threshold_count_[name];
  // The threshold count may be 1 so the first and last count event should be both activated.
  if (global_current_count_[name].size() == 1) {
    if (!TriggerFirstCountEvent(name)) {
      return false;
    }
  }
  if (global_current_count_[name].size() == global_threshold_count_[name]) {
    if (!TriggerLastCountEvent(name)) {
      return false;
    }
  }
  return true;
}

bool DistributedCountService::TriggerFirstCountEvent(const std::string &name) {
  MS_LOG(DEBUG) << "Activating first count event for " << name;
  CounterEvent first_count_event;
  first_count_event.set_type(CounterEventType::FIRST_CNT);
  first_count_event.set_name(name);

  // Broadcast to all follower servers.
  for (uint32_t i = 1; i < server_num_; i++) {
    if (!communicator_->SendPbRequest(first_count_event, i, ps::core::TcpUserCommand::kCounterEvent)) {
      MS_LOG(ERROR) << "Activating first count event to server " << i << " failed.";
      return false;
    }
  }
  // Leader server directly calls the callback.
  counter_handlers_[name].first_count_handler(nullptr);
  return true;
}

bool DistributedCountService::TriggerLastCountEvent(const std::string &name) {
  MS_LOG(INFO) << "Activating last count event for " << name;
  CounterEvent last_count_event;
  last_count_event.set_type(CounterEventType::LAST_CNT);
  last_count_event.set_name(name);

  // Broadcast to all follower servers.
  for (uint32_t i = 1; i < server_num_; i++) {
    if (!communicator_->SendPbRequest(last_count_event, i, ps::core::TcpUserCommand::kCounterEvent)) {
      MS_LOG(ERROR) << "Activating last count event to server " << i << " failed.";
      return false;
    }
  }
  // Leader server directly calls the callback.
  counter_handlers_[name].last_count_handler(nullptr);
  return true;
}
}  // namespace server
}  // namespace fl
}  // namespace mindspore
