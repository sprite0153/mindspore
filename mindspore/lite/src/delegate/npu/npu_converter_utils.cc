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

#include "src/delegate/npu/npu_converter_utils.h"
#include <arm_neon.h>
#include "src/common/log_adapter.h"
namespace mindspore {
#define C8NUM 8
#ifdef ENABLE_ARM64
void Float32ToFloat16(const float *__restrict input, float16_t *__restrict output, int number) {
  int count = (number & ~(C8NUM - 1));
  int i = 0;
  for (; i < count; i += C8NUM) {
    float32x4_t in1 = vld1q_f32(input + i);
    float16x4_t out1 = vcvt_f16_f32(in1);
    float32x4_t in2 = vld1q_f32(input + i + 4);
    float16x4_t out2 = vcvt_f16_f32(in2);
    float16x8_t out = vcombine_f16(out1, out2);
    vst1q_f16(output + i, out);
  }
  for (; i < number; ++i) {
    output[i] = static_cast<float16_t>(input[i]);
  }
}

void Float16ToFloat32(const float16_t *__restrict input, float *__restrict output, int number) {
  int count = number & ~(C8NUM - 1);
  int i = 0;
  for (; i < count; i += C8NUM) {
    float16x8_t in = vld1q_f16(input + i);
    float16x4_t in1 = vget_low_f16(in);
    float16x4_t in2 = vget_high_f16(in);
    float32x4_t out1 = vcvt_f32_f16(in1);
    vst1q_f32(output + i, out1);
    float32x4_t out2 = vcvt_f32_f16(in2);
    vst1q_f32(output + i + 4, out2);
  }
  for (; i < number; ++i) {
    output[i] = static_cast<float>(input[i]);
  }
}
#endif

ge::Shape ConverterToNPUShape(const std::vector<int> &src_shape) {
  vector<int64_t> shapes;
  shapes.reserve(src_shape.size());
  for (int i = 0; i < src_shape.size(); i++) {
    shapes.push_back(src_shape[i]);
  }
  return ge::Shape({shapes});
}

ge::Format ConverterToNPUFormat(schema::Format format) {
  ge::Format ge_format;
  switch (format) {
    case schema::Format_NCHW:
      ge_format = ge::FORMAT_NCHW;
      break;
    case schema::Format_NHWC:
    case schema::Format_KHWC:
      ge_format = ge::FORMAT_NHWC;
      break;
    default:
      MS_LOG(ERROR) << "Unsupported format:" << format;
      // use unused format to indicate errors.
      ge_format = ge::FORMAT_ND;
      break;
  }
  return ge_format;
}

ge::DataType ConverterToNPUDataType(TypeId type_id) {
  ge::DataType data_type;
  switch (type_id) {
    case kNumberTypeFloat:
    case kNumberTypeFloat32:
    case kNumberTypeFloat16:
      data_type = ge::DT_FLOAT;
      break;
    case kNumberTypeInt8:
      data_type = ge::DT_INT8;
      break;
    case kNumberTypeUInt8:
      data_type = ge::DT_UINT8;
      break;
    case kNumberTypeInt16:
      data_type = ge::DT_INT16;
      break;
    case kNumberTypeInt32:
      data_type = ge::DT_INT32;
      break;
    case kNumberTypeUInt32:
      data_type = ge::DT_UINT32;
      break;
    default:
      data_type = ge::DT_UNDEFINED;
      break;
  }
  return data_type;
}

hiai::op::Data *ConverterToNPUData(tensor::MSTensor *src, const std::string &name) {
  auto data = new (std::nothrow) hiai::op::Data(name);
  if (data == nullptr) {
    MS_LOG(ERROR) << "new data failed.";
    return data;
  }
  ge::TensorDesc tensor_desc(ConverterToNPUShape(src->shape()), ge::FORMAT_NCHW,
                             ConverterToNPUDataType(src->data_type()));
  data->update_input_desc_x(tensor_desc);
  return data;
}

std::shared_ptr<ge::Tensor> ConverterToNPUTensor(tensor::MSTensor *src) {
  std::shared_ptr<ge::Tensor> ge_tensor = std::shared_ptr<ge::Tensor>(new (std::nothrow) ge::Tensor());
  if (ge_tensor == nullptr) {
    MS_LOG(ERROR) << "new ge_tensor failed.";
    return nullptr;
  }
  ge::TensorDesc tensor_desc(ConverterToNPUShape(src->shape()), ge::FORMAT_NCHW,
                             ConverterToNPUDataType(src->data_type()));

  ge_tensor->SetTensorDesc(tensor_desc);

  if (src->data() != nullptr) {
    if (src->data_type() == kNumberTypeFloat16) {
#ifdef ENABLE_ARM64
      auto fp32_data = malloc(src->ElementsNum() * sizeof(float));
      Float16ToFloat32(reinterpret_cast<float16_t *>(src->data()), reinterpret_cast<float *>(fp32_data),
                       src->ElementsNum());
      ge_tensor->SetData(reinterpret_cast<const uint8_t *>(fp32_data), src->ElementsNum() * sizeof(float));
      free(fp32_data);
#else
      MS_LOG(ERROR) << "This platform does not support fp16.";
      return nullptr;
#endif
    } else {
      ge_tensor->SetData(reinterpret_cast<const uint8_t *>(src->data()), src->Size());
    }
  }
  return ge_tensor;
}

// mode  : Either 0 (product), 1 (sum), 2 (max), 3 (mean). Defaults to 1 (sum).
int ConverterToNPUEltwiseMode(schema::EltwiseMode mode) {
  int mode_num = 1;
  switch (mode) {
    case schema::EltwiseMode_PROD:
      mode_num = 0;
      break;
    case schema::EltwiseMode_SUM:
      mode_num = 1;
      break;
    case schema::EltwiseMode_MAXIMUM:
      mode_num = 2;
      break;
    default:
      MS_LOG(ERROR) << "Unsupported Eltwise mode.";
  }
  return mode_num;
}

int TransFormAxis(int axis) {
  switch (axis) {
    case 0:
      return 0;
    case 1:
      return 2;
    case 2:
      return 3;
    case 3:
    case -1:
      return 1;
    default:
      return -2;
  }
}

bool IsContainMSTensor(const std::vector<tensor::MSTensor *> &tensor_vec, const tensor::MSTensor *tensor) {
  return find(tensor_vec.begin(), tensor_vec.end(), tensor) != tensor_vec.end();
}
}  // namespace mindspore
