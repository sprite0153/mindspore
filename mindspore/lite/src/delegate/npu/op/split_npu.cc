/**
 * Copyright 2020-2021 Huawei Technologies Co., Ltd
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

#include "src/delegate/npu/op/split_npu.h"
#include <memory>
#include "src/delegate/npu/npu_converter_utils.h"

namespace mindspore {
int SplitNPUOp::Init(const schema::Primitive *primitive, const std::vector<tensor::MSTensor *> &in_tensors,
                     const std::vector<tensor::MSTensor *> &out_tensors) {
  split_ = new (std::nothrow) hiai::op::SplitV(name_);
  if (split_ == nullptr) {
    MS_LOG(ERROR) << "New split npu operator for op " << name_ << " failed.";
    return RET_ERROR;
  }
  auto split_prim = primitive->value_as_Split();
  if (split_prim == nullptr) {
    MS_LOG(ERROR) << "Get null primitive value for op ." << name_;
    return RET_ERROR;
  }

  auto sizes_split = split_prim->size_splits();
  std::vector<int> sizes_split_vec;
  if (sizes_split != nullptr) {
    sizes_split_vec = std::vector<int>(sizes_split->begin(), sizes_split->end());
  } else {
    return RET_ERROR;
  }
  int size = split_prim->output_num();
  ge::TensorDesc size_splits_tensor_desc(ge::Shape({size}), ge::FORMAT_NCHW, ge::DT_INT32);
  ge::TensorPtr size_splits_tensor = std::make_shared<hiai::Tensor>(size_splits_tensor_desc);
  size_splits_tensor->SetData(reinterpret_cast<uint8_t *>(sizes_split_vec.data()), size * sizeof(int));
  size_splits_ = new hiai::op::Const(name_ + "_size");
  size_splits_->set_attr_value(size_splits_tensor);
  split_->set_input_size_splits(*size_splits_);

  axis_ = static_cast<int>(split_prim->axis());
  split_->set_attr_num_split(size);
  split_->create_dynamic_output_y(size);
  return RET_OK;
}

int SplitNPUOp::SetNPUInputs(const std::vector<tensor::MSTensor *> &in_tensors,
                             const std::vector<tensor::MSTensor *> &out_tensors,
                             const std::vector<ge::Operator *> &npu_inputs) {
  ge::TensorDesc split_dim_tensor_desc(ge::Shape({1}), ge::FORMAT_NCHW, ge::DT_INT32);
  ge::TensorPtr split_dim_tensor = std::make_shared<hiai::Tensor>(split_dim_tensor_desc);
  vector<int32_t> split_dim_data_value = {axis_};
  split_dim_tensor->SetData(reinterpret_cast<uint8_t *>(split_dim_data_value.data()), 1 * sizeof(int));
  split_dim_ = new hiai::op::Const(name_ + "_dim");
  split_dim_->set_attr_value(split_dim_tensor);
  split_->set_input_split_dim(*split_dim_);

  split_->set_input_x(*npu_inputs[0]);
  return RET_OK;
}

ge::Operator *SplitNPUOp::GetNPUOp() { return this->split_; }

int SplitNPUOp::HandleAxis() {
  axis_ = TransFormAxis(axis_);
  return RET_OK;
}

SplitNPUOp::~SplitNPUOp() {
  if (split_ != nullptr) {
    delete split_;
    split_ = nullptr;
  }
  if (size_splits_ != nullptr) {
    delete size_splits_;
    size_splits_ = nullptr;
  }
  if (split_dim_ != nullptr) {
    delete split_dim_;
    split_dim_ = nullptr;
  }
}
}  // namespace mindspore
