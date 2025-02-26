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
#ifndef MINDSPORE_NNACL_ADAM_FP32_H
#define MINDSPORE_NNACL_ADAM_FP32_H
#include <math.h>
#include "nnacl/op_base.h"
#include "nnacl/errorcode.h"

#ifdef ENABLE_SSE
#include <x86intrin.h>
#endif

#ifdef ENABLE_AVX
#include <immintrin.h>
#endif
#ifdef ENABLE_AVX512
struct AVX_Data {
  __m512 data;
};

static inline void LoadStep4(struct AVX_Data *inp0, const float *inp1) {
  inp0[0].data = _mm512_loadu_ps(inp1);
  inp0[1].data = _mm512_loadu_ps(inp1 + C16NUM);
  inp0[2].data = _mm512_loadu_ps(inp1 + C16NUM * 2);
  inp0[3].data = _mm512_loadu_ps(inp1 + C16NUM * 3);
}

static inline void StoreStep4(float *inp0, struct AVX_Data *inp1) {
  _mm512_storeu_ps(inp0, inp1[0].data);
  _mm512_storeu_ps(inp0 + C16NUM, inp1[1].data);
  _mm512_storeu_ps(inp0 + C16NUM * 2, inp1[2].data);
  _mm512_storeu_ps(inp0 + C16NUM * 3, inp1[3].data);
}
#endif
#ifdef __cplusplus
extern "C" {
#endif
int AdamFp32(float *var, float *m, float *v, float lr, float beta1, float beta2, float epsilon, const float *gradient,
             size_t start, size_t end, bool use_nesterov);
int AdamDeltaFp32(float *delta, float *m, float *v, float lr, float beta1, float beta2, float epsilon,
                  const float *gradient, size_t start, size_t end, bool use_nesterov);
int AdamWeightDecayFp32(float *var, float *m, float *v, float lr, float beta1, float beta2, float epsilon, float *decay,
                        const float *gradient, size_t start, size_t end);
int FusedAdamFp32(float *var, float *m, float *v, float lr, float beta1, float beta2, float epsilon, float *decay,
                  const int16_t *gradient16, size_t start, size_t end);
#ifdef __cplusplus
}
#endif
#endif  // MINDSPORE_NNACL_ADAM_FP32_H
