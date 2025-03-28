// Auto-generated file. Do not edit!
//   Template: src/f32-rminmax/scalar.c.in
//   Generator: tools/xngen
//
// Copyright 2023 Google LLC
//
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree.

#include <assert.h>

#include <xnnpack/common.h>
#include <xnnpack/math.h>
#include <xnnpack/reduce.h>


void xnn_f32_rminmax_ukernel__scalar_u3_acc3(
    size_t batch,
    const float* input,
    float* output,
    const union xnn_f32_default_params params[restrict XNN_MIN_ELEMENTS(1)])
{
  assert(batch != 0);
  assert(batch % sizeof(float) == 0);
  assert(input != NULL);
  assert(output != NULL);

  float vmin0 = *input;
  float vmax0 = *input;
  float vmin1 = vmin0;
  float vmax1 = vmax0;
  float vmin2 = vmin0;
  float vmax2 = vmax0;
  for (; batch >= 3 * sizeof(float); batch -= 3 * sizeof(float)) {
    const float vt0 = input[0];
    const float vt1 = input[1];
    const float vt2 = input[2];
    input += 3;

    vmin0 = math_min_f32(vmin0, vt0);
    vmax0 = math_max_f32(vmax0, vt0);
    vmin1 = math_min_f32(vmin1, vt1);
    vmax1 = math_max_f32(vmax1, vt1);
    vmin2 = math_min_f32(vmin2, vt2);
    vmax2 = math_max_f32(vmax2, vt2);
  }
  vmin0 = math_min_f32(vmin0, vmin1);
  vmax0 = math_max_f32(vmax0, vmax1);
  vmin0 = math_min_f32(vmin0, vmin2);
  vmax0 = math_max_f32(vmax0, vmax2);

  if XNN_UNLIKELY(batch != 0) {
    do {
      const float vt = *input++;
      vmin0 = math_min_f32(vmin0, vt);
      vmax0 = math_max_f32(vmax0, vt);
      batch -= sizeof(float);
    } while (batch != 0);
  }
  output[0] = vmin0;
  output[1] = vmax0;
}
