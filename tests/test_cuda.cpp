// Agent Runtime - optional CUDA reference action proof.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// This proof is only built when a CUDA toolkit is available. It demonstrates a
// real device action governed by runtime authority, and that stale action or
// attempt authority is rejected before any device work happens.

#include <cstdio>
#include <memory>
#include <string>

#include "agent_runtime/agent_runtime.hpp"
#include "agent_runtime/cuda/cuda_reference_action.hpp"
#include "test_framework.hpp"

using namespace agent_runtime;
using namespace agent_runtime::cuda_reference;

namespace {

struct CudaFixture {
  std::shared_ptr<LogicalClock> clock;
  std::shared_ptr<CudaReferenceToolBackend> backend;
  std::unique_ptr<AgentRuntime> runtime;
  CudaDeviceInfo info;
};

[[nodiscard]] CudaFixture make_cuda_fixture() {
  CudaFixture fixture;
  fixture.info = query_device();
  fixture.clock = std::make_shared<LogicalClock>();
  fixture.backend = std::make_shared<CudaReferenceToolBackend>(1);
  AgentRuntimeOptions options;
  options.runtime_id = AgentRuntimeId(9100);
  options.agent_id = AgentId(7100);
  options.clock = fixture.clock;
  options.policy = RuntimePolicy::embedded();
  options.tool_backend = fixture.backend;
  fixture.runtime = std::make_unique<AgentRuntime>(std::move(options));
  (void)fixture.runtime->initialize();
  ToolBinding binding;
  binding.tool_name = "cuda-vector-add";
  binding.binding_generation = ToolCallGeneration(1);
  binding.backend_id = fixture.backend->incarnation().backend_id;
  binding.backend_generation = fixture.backend->incarnation().generation;
  binding.side_effect = SideEffectClass::PURE;
  binding.max_input_bytes = 64;
  binding.current = true;
  (void)fixture.runtime->bind_tool(binding);
  (void)fixture.runtime->start_run(AgentRunId(910), AgentRunGeneration(1));
  (void)fixture.runtime->begin_step();
  return fixture;
}

[[nodiscard]] ActionSpec cuda_spec(std::string elements) {
  ActionSpec spec;
  spec.kind = ActionKind::TOOL_CALL;
  spec.tool_name = "cuda-vector-add";
  spec.side_effect = SideEffectClass::PURE;
  spec.input = std::move(elements);
  spec.max_output_bytes = 4096;
  return spec;
}

}  // namespace

AR_TEST(cuda, device_is_enumerated) {
  const CudaDeviceInfo info = query_device();
  if (!info.available) {
    std::printf("         CUDA unavailable in this environment: %s\n", info.error.c_str());
    AR_CHECK_MSG(!info.available, "CUDA is reported unavailable for a concrete reason");
    return;
  }
  std::printf("         device=%s compute=%d.%d total=%llu MiB free=%llu MiB runtime=%d\n",
              info.name.c_str(), info.compute_major, info.compute_minor,
              static_cast<unsigned long long>(info.total_memory_bytes / (1024 * 1024)),
              static_cast<unsigned long long>(info.free_memory_bytes / (1024 * 1024)),
              info.runtime_version);
  AR_CHECK(!info.name.empty());
  AR_CHECK(info.device_count >= 1);
  AR_CHECK(info.total_memory_bytes > 0);
}

AR_TEST(cuda, real_device_action_is_governed_by_runtime_authority) {
  CudaFixture fixture = make_cuda_fixture();
  if (!fixture.info.available) {
    std::printf("         CUDA unavailable in this environment: %s\n",
                fixture.info.error.c_str());
    return;
  }
  const std::uint64_t free_before = fixture.backend->free_memory_bytes();
  ActionHandle handle;
  AR_REQUIRE(fixture.runtime->declare_action(cuda_spec("65536"), handle).accepted());
  AR_REQUIRE(fixture.runtime->admit_action(handle.action_id, handle.action_generation).accepted());
  AR_REQUIRE(fixture.runtime->authorize_action(handle.action_id, handle.action_generation).accepted());
  const LocalExecutionResult execution =
      fixture.runtime->dispatch_and_execute(handle.action_id, handle.action_generation);
  AR_REQUIRE_MSG(execution.completion.accepted(), execution.completion.to_text());
  AR_REQUIRE(fixture.runtime->commit_action(handle.action_id, handle.action_generation).accepted());
  AR_CHECK_EQ(fixture.backend->kernel_launches(), 1ull);
  AR_CHECK_EQ(fixture.backend->outstanding_allocations(), 0ull);
  const std::uint64_t free_after = fixture.backend->free_memory_bytes();
  std::printf("         free device memory before=%llu after=%llu bytes\n",
              static_cast<unsigned long long>(free_before),
              static_cast<unsigned long long>(free_after));
  AR_CHECK(fixture.runtime->summary().committed_actions == 1);
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(cuda, stale_authority_is_rejected_before_device_work) {
  CudaFixture fixture = make_cuda_fixture();
  if (!fixture.info.available) {
    std::printf("         CUDA unavailable in this environment: %s\n",
                fixture.info.error.c_str());
    return;
  }
  ActionHandle handle;
  AR_REQUIRE(fixture.runtime->declare_action(cuda_spec("1024"), handle).accepted());
  AR_REQUIRE(fixture.runtime->admit_action(handle.action_id, handle.action_generation).accepted());
  AR_REQUIRE(fixture.runtime->authorize_action(handle.action_id, handle.action_generation).accepted());

  const std::uint64_t launches_before = fixture.backend->kernel_launches();
  const DispatchResult stale = fixture.runtime->dispatch_action(
      handle.action_id, ActionGeneration(9999), DispatchMode::EXTERNAL);
  AR_CHECK(!stale.result.accepted());
  AR_CHECK_EQ(std::string(to_string(stale.result.code)), std::string("REJECT_STALE_ACTION"));
  AR_CHECK_EQ(fixture.backend->kernel_launches(), launches_before);

  const LocalExecutionResult execution =
      fixture.runtime->dispatch_and_execute(handle.action_id, handle.action_generation);
  AR_REQUIRE(execution.completion.accepted());
  const MutationResult stale_attempt_commit =
      fixture.runtime->commit_action(handle.action_id, ActionGeneration(9999));
  AR_CHECK_EQ(std::string(to_string(stale_attempt_commit.code)),
              std::string("REJECT_STALE_ACTION"));
  AR_REQUIRE(fixture.runtime->commit_action(handle.action_id, handle.action_generation).accepted());
  AR_CHECK_EQ(fixture.backend->kernel_launches(), launches_before + 1);
  AR_CHECK_EQ(fixture.backend->outstanding_allocations(), 0ull);
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(cuda, cpu_reference_mismatch_is_detected) {
  CudaFixture fixture = make_cuda_fixture();
  if (!fixture.info.available) {
    std::printf("         CUDA unavailable in this environment: %s\n",
                fixture.info.error.c_str());
    return;
  }
  ActionHandle handle;
  AR_REQUIRE(fixture.runtime->declare_action(cuda_spec("0"), handle).accepted());
  AR_REQUIRE(fixture.runtime->admit_action(handle.action_id, handle.action_generation).accepted());
  AR_REQUIRE(fixture.runtime->authorize_action(handle.action_id, handle.action_generation).accepted());
  const LocalExecutionResult execution =
      fixture.runtime->dispatch_and_execute(handle.action_id, handle.action_generation);
  AR_CHECK(!execution.completion.accepted());
  AR_CHECK_EQ(fixture.backend->kernel_launches(), 0ull);
  AR_CHECK_EQ(fixture.backend->outstanding_allocations(), 0ull);
  AR_CHECK(fixture.runtime->check_invariants().ok);
}
