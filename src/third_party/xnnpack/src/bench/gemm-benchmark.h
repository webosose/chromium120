// Copyright 2023 Google LLC
//
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <functional>
#include <limits>
#include <random>
#include <vector>

#include <benchmark/benchmark.h>
#include "bench/gemm.h"
#include "bench/utils.h"

#include <xnnpack/aligned-allocator.h>
#include <xnnpack/common.h>
#include <xnnpack/gemm.h>
#include <xnnpack/math.h>
#include <xnnpack/microfnptr.h>
#include <xnnpack/microparams-init.h>
#include <xnnpack/pack.h>

namespace {
void GEMMBenchmark(benchmark::State& state,
  xnn_qs8_gemm_minmax_ukernel_fn gemm,
  xnn_init_qs8_conv_minmax_params_fn init_params,
  size_t mr, size_t nr, size_t kr, size_t sr,
  benchmark::utils::IsaCheckFunction isa_check,
  bool extended_weights)
{
  if (isa_check != nullptr && !isa_check(state)) {
    return;
  }

  const size_t mc = state.range(0);
  const size_t nc = state.range(1);
  const size_t kc = state.range(2);

  const size_t nc_stride = benchmark::utils::RoundUp(nc, nr);
  const size_t kc_stride = benchmark::utils::RoundUp(kc, kr * sr);

  std::random_device random_device;
  auto rng = std::mt19937(random_device());
  auto i32rng = std::bind(std::uniform_int_distribution<int32_t>(-10000, 10000), std::ref(rng));
  auto i8rng = std::bind(
    std::uniform_int_distribution<int32_t>(-std::numeric_limits<int8_t>::max(), std::numeric_limits<int8_t>::max()), std::ref(rng));

  std::vector<int8_t> a(mc * kc + XNN_EXTRA_BYTES / sizeof(int8_t));
  std::generate(a.begin(), a.end(), std::ref(i8rng));
  std::vector<int8_t> k(nc * kc);
  std::generate(k.begin(), k.end(), std::ref(i8rng));
  std::vector<int32_t> b(nc);
  std::generate(b.begin(), b.end(), std::ref(i32rng));

  const size_t w_element_size = extended_weights ? sizeof(int16_t) : sizeof(int8_t);
  const size_t w_size = nc_stride * sizeof(int32_t) + kc_stride * nc_stride * w_element_size;
  const size_t c_elements = mc * nc;
  const size_t num_buffers = 1 +
    benchmark::utils::DivideRoundUp<size_t>(benchmark::utils::GetMaxCacheSize(), w_size + c_elements * sizeof(int8_t));

  std::vector<char, AlignedAllocator<char, 64>> w(w_size * num_buffers);
  std::fill(w.begin(), w.end(), 0);

  const xnn_qs8_packing_params packing_params = { 127 };
  if (extended_weights) {
    xnn_pack_qs8_gemm_xw_goi_w(/*g=*/1, nc, kc, nr, kr, sr,
      k.data(), b.data(), /*scale=*/nullptr, w.data(), /*extra_bytes=*/0, &packing_params);
  } else {
    xnn_pack_qs8_gemm_goi_w(/*g=*/1, nc, kc, nr, kr, sr,
      k.data(), b.data(), /*scale=*/nullptr, w.data(), /*extra_bytes=*/0, &packing_params);
  }
  std::vector<int8_t> c(c_elements * num_buffers);
  std::fill(c.begin(), c.end(), INT8_C(0xA5));

  union xnn_qs8_conv_minmax_params quantization_params;
  init_params(&quantization_params,
              /*scale=*/0.75f,
              /*output_zero_point=*/127,
              /*output_min=*/-127,
              /*output_max=*/126);

  size_t buffer_index = 0;
  for (auto _ : state) {
    // Use circular buffers (exceeding cache size) and prefetch to control cache state:
    // - A is always in L1 cache (if fits, otherwise L2, L3, etc)
    // - W is not in cache (for any cache level)
    // - C is not in cache (for any cache level)
    state.PauseTiming();
    benchmark::utils::PrefetchToL1(a.data(), a.size() * sizeof(int8_t));
    buffer_index = (buffer_index + 1) % num_buffers;
    state.ResumeTiming();

    for (uint32_t m = 0; m < mc; m += mr) {
      const uint32_t mb = min(mc - m, mr);
      for (uint32_t n = 0; n < nc; n += nr) {
        const uint32_t nb = min(nc - n, nr);
        gemm(
          mb, nb, kc * sizeof(int8_t),
          a.data() + m * kc, kc * sizeof(int8_t),
          w.data() + w_size * buffer_index + n * (kc_stride * w_element_size + sizeof(int32_t)),
          c.data() + (mc * buffer_index + m) * nc + n, nc * sizeof(int8_t), nr * sizeof(int8_t),
          &quantization_params);
      }
    }
  }

  const uint64_t cpu_frequency = benchmark::utils::GetCurrentCpuFrequency();
  if (cpu_frequency != 0) {
    state.counters["cpufreq"] = cpu_frequency;
  }

  state.counters["OPS"] = benchmark::Counter(
    uint64_t(state.iterations()) * 2 * mc * nc * kc, benchmark::Counter::kIsRate);
}

void GEMMBenchmark(benchmark::State& state,
  xnn_qs8_qc8w_gemm_minmax_ukernel_fn gemm,
  xnn_init_qs8_qc8w_conv_minmax_params_fn init_params,
  size_t mr, size_t nr, size_t kr, size_t sr,
  benchmark::utils::IsaCheckFunction isa_check,
  bool extended_weights)
{
  if (isa_check != nullptr && !isa_check(state)) {
    return;
  }

  const size_t mc = state.range(0);
  const size_t nc = state.range(1);
  const size_t kc = state.range(2);

  const size_t nc_stride = benchmark::utils::RoundUp(nc, nr);
  const size_t kc_stride = benchmark::utils::RoundUp(kc, kr * sr);

  std::random_device random_device;
  auto rng = std::mt19937(random_device());
  auto i32rng = std::bind(std::uniform_int_distribution<int32_t>(-10000, 10000), std::ref(rng));
  auto i8rng = std::bind(
    std::uniform_int_distribution<int32_t>(-std::numeric_limits<int8_t>::max(), std::numeric_limits<int8_t>::max()), std::ref(rng));

  std::vector<int8_t> a(mc * kc + XNN_EXTRA_BYTES / sizeof(int8_t));
  std::generate(a.begin(), a.end(), std::ref(i8rng));
  std::vector<int8_t> k(nc * kc);
  std::generate(k.begin(), k.end(), std::ref(i8rng));
  std::vector<int32_t> b(nc);
  std::generate(b.begin(), b.end(), std::ref(i32rng));

  const size_t w_element_size = extended_weights ? sizeof(int16_t) : sizeof(int8_t);
  const size_t w_size = nc_stride * sizeof(int32_t) + kc_stride * nc_stride * w_element_size;
  const size_t c_elements = mc * nc;
  const size_t num_buffers = 1 +
    benchmark::utils::DivideRoundUp<size_t>(benchmark::utils::GetMaxCacheSize(), w_size + c_elements * sizeof(int8_t));

  std::vector<char, AlignedAllocator<char, 64>> w(w_size * num_buffers);
  std::fill(w.begin(), w.end(), 0);

  const xnn_qs8_packing_params packing_params = { int8_t(127 - 0x80) };
  if (extended_weights) {
    xnn_pack_qs8_gemm_xw_goi_w(/*g=*/1, nc, kc, nr, kr, sr,
      k.data(), b.data(), /*scale=*/nullptr, w.data(), nr * sizeof(float), &packing_params);
  } else {
    xnn_pack_qs8_gemm_goi_w(/*g=*/1, nc, kc, nr, kr, sr,
      k.data(), b.data(), /*scale=*/nullptr, w.data(), nr * sizeof(float), &packing_params);
  }
  std::vector<int8_t> c(c_elements * num_buffers);
  std::fill(c.begin(), c.end(), INT8_C(0xA5));

  union xnn_qs8_qc8w_conv_minmax_params quantization_params;
  init_params(&quantization_params,
              /*output_zero_point=*/127,
              /*output_min=*/-127,
              /*output_max=*/126);

  size_t buffer_index = 0;
  for (auto _ : state) {
    // Use circular buffers (exceeding cache size) and prefetch to control cache state:
    // - A is always in L1 cache (if fits, otherwise L2, L3, etc)
    // - W is not in cache (for any cache level)
    // - C is not in cache (for any cache level)
    state.PauseTiming();
    benchmark::utils::PrefetchToL1(a.data(), a.size() * sizeof(int8_t));
    buffer_index = (buffer_index + 1) % num_buffers;
    state.ResumeTiming();

    for (uint32_t m = 0; m < mc; m += mr) {
      const uint32_t mb = min(mc - m, mr);
      for (uint32_t n = 0; n < nc; n += nr) {
        const uint32_t nb = min(nc - n, nr);
        gemm(
          mb, nb, kc * sizeof(int8_t),
          a.data() + m * kc, kc * sizeof(int8_t),
          w.data() + w_size * buffer_index + n * (kc_stride * w_element_size + sizeof(int32_t)),
          c.data() + (mc * buffer_index + m) * nc + n, nc * sizeof(int8_t), nr * sizeof(int8_t),
          &quantization_params);
      }
    }
  }

  const uint64_t cpu_frequency = benchmark::utils::GetCurrentCpuFrequency();
  if (cpu_frequency != 0) {
    state.counters["cpufreq"] = cpu_frequency;
  }

  state.counters["OPS"] = benchmark::Counter(
    uint64_t(state.iterations()) * 2 * mc * nc * kc, benchmark::Counter::kIsRate);
}

void GEMMBenchmark(benchmark::State& state,
  xnn_qs8_qc8w_gemm_minmax_ukernel_fn gemm,
  xnn_init_qs8_qc8w_conv_minmax_params_fn init_params,
  size_t mr, size_t nr, size_t kr, size_t sr,
  benchmark::utils::IsaCheckFunction isa_check)
{
  return GEMMBenchmark(state, gemm, init_params, mr, nr, kr, sr, isa_check, /*extended_weights=*/false);
}

void GEMMBenchmark(benchmark::State& state,
  xnn_qs8_gemm_minmax_ukernel_fn gemm,
  xnn_init_qs8_conv_minmax_params_fn init_params,
  size_t mr, size_t nr, size_t kr, size_t sr,
  benchmark::utils::IsaCheckFunction isa_check)
{
  return GEMMBenchmark(state, gemm, init_params, mr, nr, kr, sr, isa_check, /*extended_weights=*/false);
}

void GEMMBenchmark(benchmark::State& state,
  xnn_qd8_f32_qc8w_gemm_ukernel_fn gemm,
  xnn_init_f32_minmax_params_fn init_params,
  size_t mr, size_t nr, size_t kr, size_t sr,
  benchmark::utils::IsaCheckFunction isa_check)
{
  if (isa_check != nullptr && !isa_check(state)) {
    return;
  }

  const size_t mc = state.range(0);
  const size_t nc = state.range(1);
  const size_t kc = state.range(2);

  const size_t nc_stride = benchmark::utils::RoundUp(nc, nr);
  const size_t kc_stride = benchmark::utils::RoundUp(kc, kr * sr);

  std::random_device random_device;
  auto rng = std::mt19937(random_device());
  auto i8rng = std::bind(
    std::uniform_int_distribution<int32_t>(-std::numeric_limits<int8_t>::max(), std::numeric_limits<int8_t>::max()), std::ref(rng));

  std::vector<int8_t> a(mc * kc + XNN_EXTRA_BYTES / sizeof(int8_t));
  std::generate(a.begin(), a.end(), std::ref(i8rng));
  std::vector<int8_t> k(nc * kc);
  std::generate(k.begin(), k.end(), std::ref(i8rng));

  std::vector<xnn_qd8_quantization_params> quantization_params(mc + XNN_EXTRA_QUANTIZATION_PARAMS);
  const size_t w_elements = nc_stride * (sizeof(float) * 2 + sizeof(int32_t)) + kc_stride * nc_stride;

  const size_t c_elements = mc * nc;
  const size_t num_buffers = 1 +
    benchmark::utils::DivideRoundUp<size_t>(benchmark::utils::GetMaxCacheSize(),
      sizeof(float) * (w_elements + c_elements));

  std::vector<char, AlignedAllocator<char, 64>> w(w_elements * num_buffers);
  std::fill(w.begin(), w.end(), 0);

  const xnn_qs8_packing_params packing_params = { /*input_zero_point=*/1 };
  xnn_pack_qs8_gemm_goi_w(1, nc, kc, nr, kr, sr,
                          k.data(), /*bias=*/nullptr, /*scale=*/nullptr, w.data(), sizeof(float) * 2 * nr, &packing_params);
  std::vector<float> c(c_elements * num_buffers);
  std::fill(c.begin(), c.end(), std::nanf(""));

  // Prepare parameters.
  xnn_f32_minmax_params params;
  init_params(&params, std::numeric_limits<int8_t>::min(), std::numeric_limits<int8_t>::max());

  size_t buffer_index = 0;
  for (auto _ : state) {
    // Use circular buffers (exceeding cache size) and prefetch to control cache state:
    // - A is always in L1 cache (if fits, otherwise L2, L3, etc)
    // - W is not in cache (for any cache level)
    // - C is not in cache (for any cache level)
    state.PauseTiming();
    benchmark::utils::PrefetchToL1(a.data(), a.size());
    buffer_index = (buffer_index + 1) % num_buffers;
    state.ResumeTiming();

    for (uint32_t m = 0; m < mc; m += mr) {
      const uint32_t mb = min(mc - m, mr);
      gemm(
        mb, nc, kc,
        a.data() + m * kc, kc * sizeof(int8_t),
        w.data() + w_elements * buffer_index,
        c.data() + (buffer_index * mc + m) * nc, nc * sizeof(float), nr * sizeof(float),
        &params, quantization_params.data() + m);
    }
  }

  const uint64_t cpu_frequency = benchmark::utils::GetCurrentCpuFrequency();
  if (cpu_frequency != 0) {
    state.counters["cpufreq"] = cpu_frequency;
  }

  state.counters["OPS"] = benchmark::Counter(
    uint64_t(state.iterations()) * 2 * mc * nc * kc, benchmark::Counter::kIsRate);
}

void GEMMBenchmark(benchmark::State& state,
  xnn_qd8_f32_qc4w_gemm_ukernel_fn gemm,
  xnn_init_f32_qc4w_minmax_params_fn init_params,
  size_t mr, size_t nr, size_t kr, size_t sr,
  benchmark::utils::IsaCheckFunction isa_check)
{
  if (isa_check != nullptr && !isa_check(state)) {
    return;
  }

  const size_t mc = state.range(0);
  const size_t nc = state.range(1);
  const size_t kc = state.range(2);

  const size_t nc_stride = benchmark::utils::RoundUp(nc, nr);
  const size_t kc_stride = benchmark::utils::RoundUp(kc, kr * sr) / 2;

  std::random_device random_device;
  auto rng = std::mt19937(random_device());
  auto i8rng = std::bind(
    std::uniform_int_distribution<int32_t>(-std::numeric_limits<int8_t>::max(), std::numeric_limits<int8_t>::max()), std::ref(rng));
  auto u8rng = std::bind(
    std::uniform_int_distribution<int32_t>(0, std::numeric_limits<uint8_t>::max()), std::ref(rng));

  std::vector<int8_t> a(mc * kc + XNN_EXTRA_BYTES);
  std::generate(a.begin(), a.end(), std::ref(i8rng));
  std::vector<uint8_t> k(nc * kc / 2);
  std::generate(k.begin(), k.end(), std::ref(u8rng));

  std::vector<xnn_qd8_quantization_params> quantization_params(mc + XNN_EXTRA_QUANTIZATION_PARAMS);
  const size_t w_elements = nc_stride * (sizeof(float) * 2 + sizeof(int32_t)) + kc_stride * nc_stride;

  const size_t c_elements = mc * nc;
  const size_t num_buffers = 1 +
    benchmark::utils::DivideRoundUp<size_t>(benchmark::utils::GetMaxCacheSize(),
      sizeof(float) * (w_elements + c_elements));

  std::vector<char, AlignedAllocator<char, 64>> w(w_elements * num_buffers);
  std::fill(w.begin(), w.end(), 0);

  const xnn_qs8_packing_params packing_params = { /*input_zero_point=*/1 };
  // Note that bias will be incorrect with qs8 pack.  Use qc4w variation when available
  xnn_pack_qs8_gemm_goi_w(1, nc, kc / 2, nr, kr, sr,
                          (const int8_t*) k.data(), /*bias=*/nullptr, /*scale=*/nullptr, w.data(), sizeof(float) * 2 * nr, &packing_params);
  std::vector<float> c(c_elements * num_buffers);
  std::fill(c.begin(), c.end(), std::nanf(""));

  // Prepare parameters.
  xnn_f32_qc4w_minmax_params params;
  init_params(&params, std::numeric_limits<int8_t>::min(), std::numeric_limits<int8_t>::max(), 0);

  size_t buffer_index = 0;
  for (auto _ : state) {
    // Use circular buffers (exceeding cache size) and prefetch to control cache state:
    // - A is always in L1 cache (if fits, otherwise L2, L3, etc)
    // - W is not in cache (for any cache level)
    // - C is not in cache (for any cache level)
    state.PauseTiming();
    benchmark::utils::PrefetchToL1(a.data(), a.size());
    buffer_index = (buffer_index + 1) % num_buffers;
    state.ResumeTiming();

    for (uint32_t m = 0; m < mc; m += mr) {
      const uint32_t mb = min(mc - m, mr);
      gemm(
        mb, nc, kc,
        a.data() + m * kc, kc * sizeof(int8_t),
        w.data() + w_elements * buffer_index,
        c.data() + (buffer_index * mc + m) * nc, nc * sizeof(float), nr * sizeof(float),
        &params, quantization_params.data() + m);
    }
  }

  const uint64_t cpu_frequency = benchmark::utils::GetCurrentCpuFrequency();
  if (cpu_frequency != 0) {
    state.counters["cpufreq"] = cpu_frequency;
  }

  state.counters["OPS"] = benchmark::Counter(
    uint64_t(state.iterations()) * 2 * mc * nc * kc, benchmark::Counter::kIsRate);
}

static void GEMMBenchmark(benchmark::State& state,
  xnn_qu8_gemm_minmax_ukernel_fn gemm,
  xnn_init_qu8_conv_minmax_params_fn init_params,
  size_t mr, size_t nr, size_t kr, size_t sr,
  benchmark::utils::IsaCheckFunction isa_check = nullptr)
{
  if (isa_check != nullptr && !isa_check(state)) {
    return;
  }

  const size_t mc = state.range(0);
  const size_t nc = state.range(1);
  const size_t kc = state.range(2);

  const size_t nc_stride = benchmark::utils::RoundUp(nc, nr);
  const size_t kc_stride = benchmark::utils::RoundUp(kc, kr * sr);

  std::random_device random_device;
  auto rng = std::mt19937(random_device());
  auto i32rng = std::bind(std::uniform_int_distribution<int32_t>(-10000, 10000), std::ref(rng));
  auto u8rng = std::bind(std::uniform_int_distribution<uint32_t>(0, std::numeric_limits<uint8_t>::max()), std::ref(rng));

  std::vector<uint8_t> a(mc * kc + XNN_EXTRA_BYTES / sizeof(uint8_t));
  std::generate(a.begin(), a.end(), std::ref(u8rng));
  std::vector<uint8_t> k(nc * kc);
  std::generate(k.begin(), k.end(), std::ref(u8rng));
  std::vector<int32_t> b(nc);
  std::generate(b.begin(), b.end(), std::ref(i32rng));

  const size_t w_elements = kc_stride * nc_stride + nc_stride * sizeof(int32_t) / sizeof(uint8_t);
  const size_t c_elements = mc * nc;
  const size_t num_buffers = 1 +
    benchmark::utils::DivideRoundUp<size_t>(benchmark::utils::GetMaxCacheSize(),
      sizeof(uint8_t) * (w_elements + c_elements));

  std::vector<uint8_t, AlignedAllocator<uint8_t, 64>> w(w_elements * num_buffers);
  std::fill(w.begin(), w.end(), 0);
  const xnn_qu8_packing_params packing_params = { 127, 127 };
  xnn_pack_qu8_gemm_goi_w(/*groups=*/1, nc, kc, nr, kr, sr,
    k.data(), b.data(), /*scale=*/nullptr, w.data(), /*extra_bytes=*/0, &packing_params);
  std::vector<uint8_t> c(c_elements * num_buffers);
  std::fill(c.begin(), c.end(), 0xA5);

  union xnn_qu8_conv_minmax_params quantization_params;
  init_params(&quantization_params, 127, 0.75f, 127, 1, 254);

  size_t buffer_index = 0;
  for (auto _ : state) {
    // Use circular buffers (exceeding cache size) and prefetch to control cache state:
    // - A is always in L1 cache (if fits, otherwise L2, L3, etc)
    // - W is not in cache (for any cache level)
    // - C is not in cache (for any cache level)
    state.PauseTiming();
    benchmark::utils::PrefetchToL1(a.data(), a.size() * sizeof(uint8_t));
    buffer_index = (buffer_index + 1) % num_buffers;
    state.ResumeTiming();

    for (uint32_t m = 0; m < mc; m += mr) {
      const uint32_t mb = min(mc - m, mr);
      for (uint32_t n = 0; n < nc; n += nr) {
        const uint32_t nb = min(nc - n, nr);
        gemm(
          mb, nb, kc * sizeof(uint8_t),
          a.data() + m * kc, kc * sizeof(uint8_t),
          w.data() + (w_elements * buffer_index + n * (kc_stride + sizeof(int32_t))) / sizeof(uint8_t),
          c.data() + (mc * buffer_index + m) * nc + n, nc * sizeof(uint8_t), nr * sizeof(uint8_t),
          &quantization_params);
      }
    }
  }

  const uint64_t cpu_frequency = benchmark::utils::GetCurrentCpuFrequency();
  if (cpu_frequency != 0) {
    state.counters["cpufreq"] = cpu_frequency;
  }

  state.counters["OPS"] = benchmark::Counter(
    uint64_t(state.iterations()) * 2 * mc * nc * kc, benchmark::Counter::kIsRate);
}

}
