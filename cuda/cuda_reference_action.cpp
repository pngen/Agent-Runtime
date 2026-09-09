// Agent Runtime - optional CUDA reference action implementation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "agent_runtime/cuda/cuda_reference_action.hpp"

#include <cuda_runtime.h>

#include <atomic>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace agent_runtime::cuda_reference {
namespace {

extern "C" int agent_runtime_cuda_vector_add(const float* a, const float* b, float* c, int n,
                                             int* blocks, int* threads);

[[nodiscard]] std::string device_name(const cudaDeviceProp& properties) {
  return std::string(properties.name);
}

[[nodiscard]] std::uint64_t local_digest(std::string_view text) noexcept {
  std::uint64_t hash = 1469598103934665603ull;
  for (const char raw : text) {
    hash ^= static_cast<std::uint8_t>(raw);
    hash *= 1099511628211ull;
  }
  return hash;
}

}  // namespace

CudaDeviceInfo query_device() {
  CudaDeviceInfo info;
  int count = 0;
  const cudaError_t count_error = cudaGetDeviceCount(&count);
  if (count_error != cudaSuccess) {
    info.error = std::string("cudaGetDeviceCount failed: ") + cudaGetErrorString(count_error);
    return info;
  }
  info.device_count = count;
  if (count <= 0) {
    info.error = "no CUDA device is present";
    return info;
  }
  const cudaError_t select_error = cudaSetDevice(0);
  if (select_error != cudaSuccess) {
    info.error = std::string("cudaSetDevice failed: ") + cudaGetErrorString(select_error);
    return info;
  }
  cudaDeviceProp properties{};
  const cudaError_t prop_error = cudaGetDeviceProperties(&properties, 0);
  if (prop_error != cudaSuccess) {
    info.error = std::string("cudaGetDeviceProperties failed: ") + cudaGetErrorString(prop_error);
    return info;
  }
  std::size_t free_bytes = 0;
  std::size_t total_bytes = 0;
  if (cudaMemGetInfo(&free_bytes, &total_bytes) != cudaSuccess) {
    info.error = "cudaMemGetInfo failed";
    return info;
  }
  int runtime_version = 0;
  int driver_version = 0;
  (void)cudaRuntimeGetVersion(&runtime_version);
  (void)cudaDriverGetVersion(&driver_version);
  info.available = true;
  info.name = device_name(properties);
  info.compute_major = properties.major;
  info.compute_minor = properties.minor;
  info.total_memory_bytes = static_cast<std::uint64_t>(total_bytes);
  info.free_memory_bytes = static_cast<std::uint64_t>(free_bytes);
  info.runtime_version = runtime_version;
  info.driver_version = driver_version;
  return info;
}

struct CudaReferenceToolBackend::Impl {
  std::uint64_t generation = 1;
  std::atomic<std::uint64_t> launches{0};
  std::atomic<std::uint64_t> outstanding{0};
};

CudaReferenceToolBackend::CudaReferenceToolBackend(std::uint64_t generation)
    : impl_(std::make_unique<Impl>()) {
  impl_->generation = generation == 0 ? 1 : generation;
}

CudaReferenceToolBackend::~CudaReferenceToolBackend() = default;

BackendIncarnation CudaReferenceToolBackend::incarnation() const {
  BackendIncarnation incarnation;
  incarnation.backend_id = BackendId(0xC0DA0001ull);
  incarnation.generation = Generation<BackendTag>(impl_->generation);
  incarnation.name = "cuda-reference-tool";
  const CudaDeviceInfo info = query_device();
  incarnation.available = info.available;
  return incarnation;
}

std::uint64_t CudaReferenceToolBackend::kernel_launches() const noexcept {
  return impl_->launches.load(std::memory_order_relaxed);
}

std::uint64_t CudaReferenceToolBackend::outstanding_allocations() const noexcept {
  return impl_->outstanding.load(std::memory_order_relaxed);
}

std::uint64_t CudaReferenceToolBackend::free_memory_bytes() const {
  return query_device().free_memory_bytes;
}

ToolResponse CudaReferenceToolBackend::invoke(const ToolRequest& request,
                                              const CancellationProbe& cancellation) {
  ToolResponse response;
  response.action_id = request.action_id;
  response.call_id = request.call_id;
  response.call_generation = request.call_generation;
  response.attempt_id = request.attempt_id;
  response.attempt_generation = request.attempt_generation;
  response.backend_name = "cuda-reference-tool";
  response.backend_generation = Generation<BackendTag>(impl_->generation);
  response.result_id = allocate_id<ResultTag>();

  auto fail = [&response](RetryClass failure_class, const std::string& detail) {
    response.status = CompletionStatus::FAILED;
    response.failure_class = failure_class;
    response.error = detail;
    return response;
  };

  if (cancellation.cancelled()) {
    response.status = CompletionStatus::CANCELLED;
    response.error = "cancelled before device work";
    return response;
  }

  const CudaDeviceInfo info = query_device();
  if (!info.available) {
    return fail(RetryClass::RESOURCE_UNAVAILABLE, info.error);
  }

  const long requested = std::strtol(request.input.c_str(), nullptr, 10);
  if (requested <= 0 || requested > 8 * 1024 * 1024) {
    return fail(RetryClass::PERMANENT, "cuda-vector-add input must be an element count in 1..8388608");
  }
  const int elements = static_cast<int>(requested);
  const std::size_t bytes = static_cast<std::size_t>(elements) * sizeof(float);

  std::vector<float> host_a(static_cast<std::size_t>(elements));
  std::vector<float> host_b(static_cast<std::size_t>(elements));
  std::vector<float> host_c(static_cast<std::size_t>(elements), 0.0f);
  for (int i = 0; i < elements; ++i) {
    host_a[static_cast<std::size_t>(i)] = static_cast<float>(i) * 0.5f;
    host_b[static_cast<std::size_t>(i)] = static_cast<float>(i) * 0.25f;
  }

  float* device_a = nullptr;
  float* device_b = nullptr;
  float* device_c = nullptr;
  auto release = [&]() {
    if (device_a != nullptr) {
      cudaFree(device_a);
      impl_->outstanding.fetch_sub(1, std::memory_order_relaxed);
      device_a = nullptr;
    }
    if (device_b != nullptr) {
      cudaFree(device_b);
      impl_->outstanding.fetch_sub(1, std::memory_order_relaxed);
      device_b = nullptr;
    }
    if (device_c != nullptr) {
      cudaFree(device_c);
      impl_->outstanding.fetch_sub(1, std::memory_order_relaxed);
      device_c = nullptr;
    }
  };

  if (cudaMalloc(&device_a, bytes) != cudaSuccess) {
    return fail(RetryClass::RESOURCE_UNAVAILABLE, "cudaMalloc failed for input A");
  }
  impl_->outstanding.fetch_add(1, std::memory_order_relaxed);
  if (cudaMalloc(&device_b, bytes) != cudaSuccess) {
    release();
    return fail(RetryClass::RESOURCE_UNAVAILABLE, "cudaMalloc failed for input B");
  }
  impl_->outstanding.fetch_add(1, std::memory_order_relaxed);
  if (cudaMalloc(&device_c, bytes) != cudaSuccess) {
    release();
    return fail(RetryClass::RESOURCE_UNAVAILABLE, "cudaMalloc failed for output C");
  }
  impl_->outstanding.fetch_add(1, std::memory_order_relaxed);

  if (cudaMemcpy(device_a, host_a.data(), bytes, cudaMemcpyHostToDevice) != cudaSuccess ||
      cudaMemcpy(device_b, host_b.data(), bytes, cudaMemcpyHostToDevice) != cudaSuccess) {
    release();
    return fail(RetryClass::RESOURCE_UNAVAILABLE, "host to device copy failed");
  }

  int blocks = 0;
  int threads = 0;
  const int launch = agent_runtime_cuda_vector_add(device_a, device_b, device_c, elements, &blocks,
                                                   &threads);
  if (launch != 0) {
    release();
    return fail(RetryClass::RESOURCE_UNAVAILABLE, "kernel launch failed");
  }
  impl_->launches.fetch_add(1, std::memory_order_relaxed);

  if (cudaDeviceSynchronize() != cudaSuccess) {
    release();
    return fail(RetryClass::RESOURCE_UNAVAILABLE, "device synchronize failed");
  }
  if (cudaMemcpy(host_c.data(), device_c, bytes, cudaMemcpyDeviceToHost) != cudaSuccess) {
    release();
    return fail(RetryClass::RESOURCE_UNAVAILABLE, "device to host copy failed");
  }
  release();

  std::size_t mismatches = 0;
  for (int i = 0; i < elements; ++i) {
    const float expected = host_a[static_cast<std::size_t>(i)] + host_b[static_cast<std::size_t>(i)];
    if (host_c[static_cast<std::size_t>(i)] != expected) {
      ++mismatches;
    }
  }

  std::ostringstream out;
  out << "{\"device\":\"" << info.name << "\",\"compute_capability\":\"" << info.compute_major
      << "." << info.compute_minor << "\",\"elements\":" << elements << ",\"blocks\":" << blocks
      << ",\"threads\":" << threads << ",\"mismatches\":" << mismatches
      << ",\"runtime_boot\":" << request.runtime_boot_id.value()
      << ",\"attempt_generation\":" << request.attempt_generation.value() << "}";
  response.output = out.str();
  if (mismatches != 0) {
    return fail(RetryClass::PERMANENT, "device result did not match the CPU reference");
  }
  response.result_digest = local_digest(response.output);
  response.status = CompletionStatus::SUCCEEDED;
  return response;
}

}  // namespace agent_runtime::cuda_reference
