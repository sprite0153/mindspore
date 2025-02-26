# Copyright 2021 Huawei Technologies Co., Ltd
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ============================================================================

import argparse
import numpy as np

import mindspore.context as context
import mindspore.nn as nn
from mindspore import Tensor
from mindspore.nn import TrainOneStepCell, WithLossCell
from src.model import LeNet5
from src.adam import AdamWeightDecayOp

parser = argparse.ArgumentParser(description="test_fl_lenet")
parser.add_argument("--device_target", type=str, default="CPU")
parser.add_argument("--server_mode", type=str, default="FEDERATED_LEARNING")
parser.add_argument("--ms_role", type=str, default="MS_WORKER")
parser.add_argument("--worker_num", type=int, default=0)
parser.add_argument("--server_num", type=int, default=1)
parser.add_argument("--scheduler_ip", type=str, default="127.0.0.1")
parser.add_argument("--scheduler_port", type=int, default=8113)
parser.add_argument("--fl_server_port", type=int, default=6666)
parser.add_argument("--start_fl_job_threshold", type=int, default=1)
parser.add_argument("--start_fl_job_time_window", type=int, default=3000)
parser.add_argument("--update_model_ratio", type=float, default=1.0)
parser.add_argument("--update_model_time_window", type=int, default=3000)
parser.add_argument("--fl_name", type=str, default="Lenet")
parser.add_argument("--fl_iteration_num", type=int, default=25)
parser.add_argument("--client_epoch_num", type=int, default=20)
parser.add_argument("--client_batch_size", type=int, default=32)
parser.add_argument("--client_learning_rate", type=float, default=0.1)
parser.add_argument("--scheduler_manage_port", type=int, default=11202)
parser.add_argument("--config_file_path", type=str, default="")
parser.add_argument("--encrypt_type", type=str, default="NOT_ENCRYPT")
# parameters for encrypt_type='DP_ENCRYPT'
parser.add_argument("--dp_eps", type=float, default=50.0)
parser.add_argument("--dp_delta", type=float, default=0.01)  # 1/worker_num
parser.add_argument("--dp_norm_clip", type=float, default=1.0)
# parameters for encrypt_type='PW_ENCRYPT'
parser.add_argument("--share_secrets_ratio", type=float, default=1.0)
parser.add_argument("--cipher_time_window", type=int, default=300000)
parser.add_argument("--reconstruct_secrets_threshhold", type=int, default=3)

args, _ = parser.parse_known_args()
device_target = args.device_target
server_mode = args.server_mode
ms_role = args.ms_role
worker_num = args.worker_num
server_num = args.server_num
scheduler_ip = args.scheduler_ip
scheduler_port = args.scheduler_port
fl_server_port = args.fl_server_port
start_fl_job_threshold = args.start_fl_job_threshold
start_fl_job_time_window = args.start_fl_job_time_window
update_model_ratio = args.update_model_ratio
update_model_time_window = args.update_model_time_window
share_secrets_ratio = args.share_secrets_ratio
cipher_time_window = args.cipher_time_window
reconstruct_secrets_threshhold = args.reconstruct_secrets_threshhold
fl_name = args.fl_name
fl_iteration_num = args.fl_iteration_num
client_epoch_num = args.client_epoch_num
client_batch_size = args.client_batch_size
client_learning_rate = args.client_learning_rate
scheduler_manage_port = args.scheduler_manage_port
config_file_path = args.config_file_path
dp_eps = args.dp_eps
dp_delta = args.dp_delta
dp_norm_clip = args.dp_norm_clip
encrypt_type = args.encrypt_type

ctx = {
    "enable_fl": True,
    "server_mode": server_mode,
    "ms_role": ms_role,
    "worker_num": worker_num,
    "server_num": server_num,
    "scheduler_ip": scheduler_ip,
    "scheduler_port": scheduler_port,
    "fl_server_port": fl_server_port,
    "start_fl_job_threshold": start_fl_job_threshold,
    "start_fl_job_time_window": start_fl_job_time_window,
    "update_model_ratio": update_model_ratio,
    "update_model_time_window": update_model_time_window,
    "share_secrets_ratio": share_secrets_ratio,
    "cipher_time_window": cipher_time_window,
    "reconstruct_secrets_threshhold": reconstruct_secrets_threshhold,
    "fl_name": fl_name,
    "fl_iteration_num": fl_iteration_num,
    "client_epoch_num": client_epoch_num,
    "client_batch_size": client_batch_size,
    "client_learning_rate": client_learning_rate,
    "scheduler_manage_port": scheduler_manage_port,
    "config_file_path": config_file_path,
    "dp_eps": dp_eps,
    "dp_delta": dp_delta,
    "dp_norm_clip": dp_norm_clip,
    "encrypt_type": encrypt_type
}

context.set_context(mode=context.GRAPH_MODE, device_target=device_target, save_graphs=False)
context.set_fl_context(**ctx)

if __name__ == "__main__":
    epoch = 5
    np.random.seed(0)
    network = LeNet5(62)
    criterion = nn.SoftmaxCrossEntropyWithLogits(sparse=True, reduction="mean")
    net_opt = nn.Momentum(network.trainable_params(), 0.01, 0.9)
    net_adam_opt = AdamWeightDecayOp(network.trainable_params(), weight_decay=0.1)
    net_with_criterion = WithLossCell(network, criterion)
    train_network = TrainOneStepCell(net_with_criterion, net_opt)
    train_network.set_train()
    losses = []

    for _ in range(epoch):
        data = Tensor(np.random.rand(32, 3, 32, 32).astype(np.float32))
        label = Tensor(np.random.randint(0, 61, (32)).astype(np.int32))
        loss = train_network(data, label).asnumpy()
        losses.append(loss)
    print(losses)
