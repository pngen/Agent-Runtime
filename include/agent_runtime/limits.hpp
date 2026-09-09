// Agent Runtime - bounded resource limits.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef AGENT_RUNTIME_LIMITS_HPP
#define AGENT_RUNTIME_LIMITS_HPP

#include <cstdint>
#include <string>
#include <string_view>

namespace agent_runtime {

/// Every externally influenced collection is bounded. Limits are checked before
/// allocation with checked arithmetic; exceeding a limit is an explicit
/// REJECT_LIMIT outcome, never a silent truncation or an unbounded growth.
struct ResourceLimits {
  std::uint32_t max_runs = 4096;
  std::uint32_t max_steps_per_run = 100000;
  std::uint32_t max_actions_per_step = 4096;
  std::uint32_t max_actions_per_run = 1000000;
  std::uint32_t max_active_actions = 64;
  std::uint32_t max_active_attempts = 128;
  std::uint32_t max_attempts_per_action = 64;
  std::uint32_t max_retry_history = 4096;
  std::uint32_t max_model_calls = 65536;
  std::uint32_t max_tool_calls = 65536;
  std::uint32_t max_memory_bindings = 4096;
  std::uint32_t max_checkpoint_bindings = 4096;
  std::uint32_t max_policy_records = 1024;
  std::uint32_t max_budget_records = 1024;
  std::uint32_t max_assignment_history = 4096;
  std::uint32_t max_retained_history = 1000000;
  std::uint32_t max_explanation_factors = 256;
  std::uint32_t max_completions_per_action = 16;

  std::uint32_t max_connections = 256;
  std::uint32_t max_session_threads = 512;
  std::uint32_t max_send_queue_frames = 4096;
  std::uint32_t max_outstanding_correlations = 4096;

  std::uint32_t max_frame_payload_bytes = 1024 * 1024;
  std::uint32_t max_payload_bytes = 1024 * 1024;
  std::uint32_t max_string_bytes = 4096;
  std::uint32_t max_collection_entries = 100000;
  std::uint64_t max_persistence_bytes = 1024ull * 1024ull * 1024ull;
  std::uint32_t max_temporary_files = 256;

  std::uint32_t max_parallel_read_actions = 16;
  std::uint32_t max_parallel_tool_calls = 8;
  std::uint32_t max_parallel_model_calls = 8;

  /// Returns an empty string when the limits are coherent, otherwise a
  /// deterministic description of the first violated relation.
  [[nodiscard]] std::string validate() const;
};

/// Returns the default limits.
[[nodiscard]] ResourceLimits default_resource_limits();

}  // namespace agent_runtime

#endif  // AGENT_RUNTIME_LIMITS_HPP
