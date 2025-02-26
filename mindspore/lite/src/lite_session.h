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

#ifndef MINDSPORE_LITE_SRC_LITE_SESSION_H_
#define MINDSPORE_LITE_SRC_LITE_SESSION_H_

#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <atomic>
#include "src/lite_kernel.h"
#include "include/ms_tensor.h"
#include "include/lite_session.h"
#include "include/model.h"
#include "src/inner_context.h"
#include "schema/model_generated.h"
#include "src/executor.h"
#include "src/tensor.h"
#include "src/tensorlist.h"
#include "include/delegate.h"
#if GPU_OPENCL
#include "src/runtime/gpu/opencl/opencl_runtime.h"
#elif GPU_VULKAN
#include "src/runtime/gpu/vulkan/vulkan_runtime.h"
#endif
#include "src/scheduler_cb.h"

namespace mindspore {
namespace lite {
class LiteSession : public session::LiteSession {
 public:
  LiteSession();

  ~LiteSession() override;

  virtual int Init(const Context *context);

  void BindThread(bool if_bind) override;

  int CompileGraph(Model *model) override;

  std::vector<mindspore::tensor::MSTensor *> GetInputs() const override;

  mindspore::tensor::MSTensor *GetInputsByTensorName(const std::string &name) const override;

  int RunGraph(const KernelCallBack &before = nullptr, const KernelCallBack &after = nullptr) override;

  std::vector<mindspore::tensor::MSTensor *> GetOutputsByNodeName(const std::string &node_name) const override;

  std::vector<std::string> GetOutputTensorNames() const override;

  mindspore::tensor::MSTensor *GetOutputByTensorName(const std::string &tensor_name) const override;

  std::unordered_map<std::string, mindspore::tensor::MSTensor *> GetOutputs() const override;

  int Resize(const std::vector<mindspore::tensor::MSTensor *> &inputs,
             const std::vector<std::vector<int>> &dims) override;

  void set_model(Model *model) { this->model_ = model; }

  const std::vector<kernel::LiteKernel *> &get_kernels() const { return this->kernels_; }

 protected:
  static void ConvertTensorsQuantParam(const schema::Tensor *src_tensor, lite::Tensor *dst_tensor);

  int ConvertTensorsData(const lite::Model *model, size_t tensor_index, const schema::Tensor *src_tensor,
                         lite::Tensor *dst_tensor);

  lite::Tensor *ConvertTensor(const schema::Tensor &src_tensor);

  int ConvertTensors(const lite::Model *model);

  void InitGraphInOutTensorsMap(const lite::Model *model);

  void IsolateOutputTensor();

  void InitGraphInputTensors(const lite::Model *model);

  void InitGraphInputMSTensors();

  void InitGraphOutputTensors(const lite::Model *model);

  void InitGraphInputMap(const lite::Model *model);

  void InitGraphOutputNodeMap(const lite::Model *model);

  void InitGraphOutputTensorMap(const lite::Model *model);

  void AdjustModelOutputTensorInitRefCount(const lite::Model *model);

  int ResizeInputs(const std::vector<mindspore::tensor::MSTensor *> &inputs, const std::vector<std::vector<int>> &dims);

  int PrepareKernels(Model *model, bool use_mindrt_run);

  static int ReSizeKernels(const std::vector<kernel::LiteKernel *> &kernels);

  static void FreePackOpWeight(const std::vector<kernel::LiteKernel *> &kernels);

 private:
  void ResetInputsShape(const std::vector<std::vector<int>> &dims);

  int InitGPURuntime();

  bool IfUseMindrtExecutor();

  bool IsIsolatedSubGraph(kernel::LiteKernel *kernel);

 protected:
  InnerContext *context_ = nullptr;
  std::vector<kernel::LiteKernel *> kernels_;
  std::vector<Tensor *> tensors_;
  // graph input tensors
  std::vector<Tensor *> inputs_;
  // graph output tensors
  std::vector<Tensor *> outputs_;
  // graph input MSTensors
  std::vector<mindspore::tensor::MSTensor *> input_vec_;
  // graph input tensor name -- input tensors
  std::unordered_map<std::string, mindspore::tensor::MSTensor *> input_map_;
  // graph output node name -- output tensors
  std::unordered_map<std::string, std::vector<mindspore::tensor::MSTensor *>> output_node_map_;

  std::vector<std::string> output_tensor_names_;
  // graph output tensor name -- output tensor
  std::unordered_map<std::string, mindspore::tensor::MSTensor *> output_tensor_map_;
  std::unordered_map<Tensor *, Tensor *> graph_output_map_; /* <calculate-tensor,  graph-output-tensor> */
  Executor *executor_ = nullptr;
  Model *model_ = nullptr;
  std::atomic<bool> is_running_ = {false};
  bool is_train_session_ = false;
  friend class TransferSession;
#if GPU_OPENCL
  opencl::OpenCLRuntimeWrapper *opencl_runtime_wrapper_{nullptr};
#elif GPU_VULKAN
  gpu::GpuRuntimeWrapper<vulkan::VulkanRuntime> *vk_runtime_wrap_{nullptr};
#endif
  std::unique_ptr<SchedulerCb> sched_cb_;
  std::shared_ptr<Delegate> delegate_ = nullptr;
};
}  // namespace lite
}  // namespace mindspore
#endif  // MINDSPORE_LITE_SRC_LITE_SESSION_H_
