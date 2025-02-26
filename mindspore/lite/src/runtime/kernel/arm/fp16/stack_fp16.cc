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
#include "src/runtime/kernel/arm/fp16/stack_fp16.h"
#include <vector>
#include "schema/model_generated.h"
#include "src/kernel_registry.h"
#include "nnacl/stack_parameter.h"
#include "include/errorcode.h"
#include "src/runtime/kernel/arm/fp16/common_fp16.h"
#include "nnacl/fp16/cast_fp16.h"
#include "nnacl/base/stack_base.h"

using mindspore::lite::KernelRegistrar;
using mindspore::lite::RET_ERROR;
using mindspore::lite::RET_OK;
using mindspore::schema::PrimitiveType_Stack;

namespace mindspore::kernel {
void StackFp16CPUKernel::InitMallocFlags() {
  malloc_buffers_.resize(in_tensors_.size());
  for (size_t i = 0; i < in_tensors_.size(); ++i) {
    malloc_buffers_.at(i) = in_tensors_.at(i)->data_type() == kNumberTypeFloat32;
  }
  malloc_out_ = out_tensors_.at(0)->data_type() == kNumberTypeFloat32;
}

int StackFp16CPUKernel::MallocAssignBuffer() {
  buffers_.resize(in_tensors_.size(), nullptr);
  for (size_t i = 0; i < in_tensors_.size(); ++i) {
    buffers_.at(i) = reinterpret_cast<char *>(
      ConvertInputFp32toFp16(in_tensors_.at(i), static_cast<const lite::InnerContext *>(context_)));
    if (buffers_.at(i) == nullptr) {
      return RET_ERROR;
    }
  }

  out_buffer_ = nullptr;
  out_buffer_ = MallocOutputFp16(out_tensors_.at(0), static_cast<const lite::InnerContext *>(this->context_));
  if (out_buffer_ == nullptr) {
    return RET_ERROR;
  }
  return RET_OK;
}

void StackFp16CPUKernel::FreeBuffer() {
  for (size_t i = 0; i < buffers_.size(); ++i) {
    if (malloc_buffers_.at(i) && buffers_.at(i) != nullptr) {
      context_->allocator->Free(buffers_.at(i));
      buffers_.at(i) = nullptr;
    }
  }
  if (malloc_out_ && out_buffer_ != nullptr) {
    context_->allocator->Free(out_buffer_);
    out_buffer_ = nullptr;
  }
}

int StackFp16CPUKernel::Init() {
  data_type_size_ = sizeof(float16_t);
  if (!InferShapeDone()) {
    return RET_OK;
  }
  return ReSize();
}

void StackFp16CPUKernel::Execute(int task_id) {
  auto inputs = buffers_.data();
  char *output = reinterpret_cast<char *>(out_buffer_);
  auto step = UP_DIV(outer_size_, num_threads_);
  auto start = task_id * step;
  auto end = MSMIN(start + step, outer_size_);
  auto input_num = in_tensors_.size();
  Stack(inputs, output + input_num * start * copy_size_, input_num, copy_size_, start, end);
}

static int StackRun(void *cdata, int task_id, float lhs_scale, float rhs_scale) {
  auto stack = reinterpret_cast<StackFp16CPUKernel *>(cdata);
  stack->Execute(task_id);
  return RET_OK;
}

int StackFp16CPUKernel::Run() {
  InitMallocFlags();
  auto ret = MallocAssignBuffer();
  if (ret != RET_OK) {
    FreeBuffer();
    return ret;
  }
  // run stack
  num_threads_ = MSMIN(UP_DIV(outer_size_, 64), this->op_parameter_->thread_num_);
  ret = ParallelLaunch(this->context_, StackRun, this, num_threads_);
  if (ret != RET_OK) {
    MS_LOG(ERROR) << "StackBaseCPUKernel Run error: error_code[" << ret << "]";
    return RET_ERROR;
  }
  // if output tensor is fp32, we need to transform
  if (malloc_out_) {
    auto out_tensor = out_tensors_.at(0);
    Float16ToFloat32(out_buffer_, reinterpret_cast<float *>(out_tensor->MutableData()), out_tensor->ElementsNum());
  }
  FreeBuffer();
  return RET_OK;
}

REG_KERNEL(kCPU, kNumberTypeFloat16, PrimitiveType_Stack, LiteKernelCreator<StackFp16CPUKernel>)
}  // namespace mindspore::kernel
