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

#ifndef MINDSPORE_LITE_SRC_RUNTIME_DELEGATE_NPU_OP_MAX_POOLING_NPU_H_
#define MINDSPORE_LITE_SRC_RUNTIME_DELEGATE_NPU_OP_MAX_POOLING_NPU_H_
#include <vector>
#include <string>
#include "include/graph/op/all_ops.h"
#include "src/delegate/npu/op/convolution_base_npu.h"
namespace mindspore {
class MaxPoolingNPUOp : public ConvolutionBaseNPUOp {
 public:
  MaxPoolingNPUOp(const schema::Primitive *primitive, const std::vector<tensor::MSTensor *> &in_tensors,
                  const std::vector<tensor::MSTensor *> &out_tensors, std::string name)
      : ConvolutionBaseNPUOp(primitive, in_tensors, out_tensors, name) {}

  ~MaxPoolingNPUOp() override;

  int IsSupport(const schema::Primitive *primitive, const std::vector<tensor::MSTensor *> &in_tensors,
                const std::vector<tensor::MSTensor *> &out_tensors) override;

  int Init(const schema::Primitive *primitive, const std::vector<tensor::MSTensor *> &in_tensors,
           const std::vector<tensor::MSTensor *> &out_tensors) override;

  int SetNPUInputs(const std::vector<tensor::MSTensor *> &in_tensors,
                   const std::vector<tensor::MSTensor *> &out_tensors,
                   const std::vector<ge::Operator *> &npu_inputs) override;

  ge::Operator *GetNPUOp() override;

 private:
  int SetPoolingParam(const schema::MaxPoolFusion *pooling_prim);
  schema::ActivationType act_type_ = schema::ActivationType_NO_ACTIVATION;
  hiai::op::PoolingD *pooling_ = nullptr;
};
}  // namespace mindspore
#endif  // MINDSPORE_LITE_SRC_RUNTIME_DELEGATE_NPU_OP_MAX_POOLING_NPU_H_
