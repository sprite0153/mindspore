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

#include "src/runtime/kernel/arm/fp16_grad/layernorm_fp16_grad.h"
#include <vector>

#include "schema/model_generated.h"
#include "src/kernel_registry.h"
#include "nnacl/fp16_grad/layernorm_grad.h"
#include "nnacl/fp32_grad/layernormgrad_parameter.h"
#include "include/errorcode.h"

using mindspore::kernel::KERNEL_ARCH::kCPU;
using mindspore::lite::KernelRegistrar;
using mindspore::lite::RET_ERROR;
using mindspore::lite::RET_OK;
using mindspore::schema::PrimitiveType_LayerNormGrad;

namespace mindspore::kernel {
int LayerNormGradCPUKernelFp16::ReSize() { return RET_OK; }

int LayerNormGradCPUKernelFp16::Init() {
  auto lngrad_param = reinterpret_cast<LayerNormGradParameter *>(op_parameter_);
  auto *input_x = in_tensors_.at(0);
  std::vector<int> x_shape = input_x->shape();
  int begin_norm_axis = lngrad_param->begin_norm_axis_;
  if (begin_norm_axis < 0) {
    begin_norm_axis += x_shape.size();
  }
  auto begin_params_axis = lngrad_param->begin_params_axis_;
  if (begin_params_axis < 0) {
    begin_params_axis += x_shape.size();
  }
  for (size_t i = 0; i < static_cast<size_t>(begin_norm_axis); i++) {
    block_num_ *= x_shape[i];
  }
  for (size_t i = static_cast<size_t>(begin_norm_axis); i < x_shape.size(); i++) {
    block_size_ *= x_shape[i];
  }
  for (size_t i = 0; i < static_cast<size_t>(begin_params_axis); i++) {
    param_size_ *= x_shape[i];
  }
  for (size_t i = begin_params_axis; i < x_shape.size(); i++) {
    param_num_ *= x_shape[i];
  }
  if (block_num_ <= 0 || block_size_ <= 0) {
    MS_LOG(ERROR) << "LayerNormGradCPUKernelFp16 input shape error, input shape: " << x_shape;
  }
  return RET_OK;
}

int LayerNormGradCPUKernelFp16::Execute(int task_id) {
  auto input_x = in_tensors_.at(0);
  auto input_dy = in_tensors_.at(1);
  auto input_var = in_tensors_.at(2);
  auto input_mean = in_tensors_.at(3);
  auto input_gamma = in_tensors_.at(4);
  auto output_dx = out_tensors_.at(0);
  auto output_dg = out_tensors_.at(1);
  auto output_db = out_tensors_.at(2);

  float16_t *x = reinterpret_cast<float16_t *>(input_x->data_c());
  float16_t *dy = reinterpret_cast<float16_t *>(input_dy->data_c());
  float16_t *var = reinterpret_cast<float16_t *>(input_var->data_c());
  float16_t *mean = reinterpret_cast<float16_t *>(input_mean->data_c());
  float16_t *gamma = reinterpret_cast<float16_t *>(input_gamma->data_c());
  float16_t *dx = reinterpret_cast<float16_t *>(output_dx->data_c());
  float16_t *dg = reinterpret_cast<float16_t *>(output_dg->data_c());
  float16_t *db = reinterpret_cast<float16_t *>(output_db->data_c());
  LayerNormFp16Grad(x, dy, var, mean, gamma, param_num_, param_size_, block_num_, block_size_, dx, dg, db);
  return RET_OK;
}

int LayerNormF16GradRun(void *cdata, int task_id, float lhs_scale, float rhs_scale) {
  MS_ASSERT(cdata != nullptr);
  auto ln_kernel = reinterpret_cast<LayerNormGradCPUKernelFp16 *>(cdata);
  auto error_code = ln_kernel->Execute(task_id);
  if (error_code != RET_OK) {
    MS_LOG(ERROR) << "LayerNormGradRun error task_id[" << task_id << "] error_code[" << error_code << "]";
    return RET_ERROR;
  }
  return RET_OK;
}

int LayerNormGradCPUKernelFp16::Run() {
  int error_code = ParallelLaunch(this->context_, LayerNormF16GradRun, this, 1);
  if (error_code != RET_OK) {
    MS_LOG(ERROR) << "LayerNorm function error error_code[" << error_code << "]";
    return RET_ERROR;
  }
  return RET_OK;
}

REG_KERNEL(kCPU, kNumberTypeFloat16, PrimitiveType_LayerNormGrad, LiteKernelCreator<LayerNormGradCPUKernelFp16>)
}  // namespace mindspore::kernel
