// Agent Runtime - optional CUDA reference action.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// Agent Runtime is not an accelerator scheduler. This component exists only to
// prove that runtime authority can govern a real accelerator action: the action
// is dispatched through the same admission, authorization, attempt and commit
// path as any other tool call, and a stale action or attempt authority is
// rejected before any device work happens.
//
// The core library never depends on CUDA.

#ifndef AGENT_RUNTIME_CUDA_REFERENCE_ACTION_HPP
#define AGENT_RUNTIME_CUDA_REFERENCE_ACTION_HPP

#include <cstdint>
#include <memory>
#include <string>

#include "agent_runtime/agent_runtime.hpp"

namespace agent_runtime::cuda_reference {

struct CudaDeviceInfo {
  bool available = false;
  std::string error;
  std::string name;
  int compute_major = 0;
  int compute_minor = 0;
  std::uint64_t total_memory_bytes = 0;
  std::uint64_t free_memory_bytes = 0;
  int device_count = 0;
  int runtime_version = 0;
  int driver_version = 0;
};

/// Queries the local CUDA runtime. Returns available=false with a concrete
/// error string when no usable device is present.
[[nodiscard]] CudaDeviceInfo query_device();

/// Tool backend that executes a real vector-add kernel on the local device and
/// verifies the result against a CPU reference before reporting success.
class CudaReferenceToolBackend final : public ToolBackend {
 public:
  explicit CudaReferenceToolBackend(std::uint64_t generation = 1);
  ~CudaReferenceToolBackend() override;

  [[nodiscard]] BackendIncarnation incarnation() const override;
  [[nodiscard]] ToolResponse invoke(const ToolRequest& request,
                                    const CancellationProbe& cancellation) override;

  /// Number of device kernels actually launched. Used to prove that stale
  /// authority is rejected before any device work happens.
  [[nodiscard]] std::uint64_t kernel_launches() const noexcept;
  /// Device allocations that are still outstanding. Zero after every call.
  [[nodiscard]] std::uint64_t outstanding_allocations() const noexcept;
  /// Free device memory in bytes, as reported by the CUDA runtime.
  [[nodiscard]] std::uint64_t free_memory_bytes() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace agent_runtime::cuda_reference

#endif  // AGENT_RUNTIME_CUDA_REFERENCE_ACTION_HPP
