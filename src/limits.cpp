// Agent Runtime - resource limit validation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "agent_runtime/limits.hpp"

namespace agent_runtime {
namespace {

[[nodiscard]] std::string require_positive(const char* name, std::uint64_t value) {
  if (value == 0) {
    return std::string(name) + " must be greater than zero";
  }
  return {};
}

[[nodiscard]] std::string require_at_least(const char* name, std::uint64_t value,
                                           std::uint64_t minimum) {
  if (value < minimum) {
    return std::string(name) + " must be at least " + std::to_string(minimum);
  }
  return {};
}

}  // namespace

std::string ResourceLimits::validate() const {
  struct Check {
    const char* name;
    std::uint64_t value;
    std::uint64_t minimum;
  };
  const Check checks[] = {
      {"max_runs", max_runs, 1},
      {"max_steps_per_run", max_steps_per_run, 1},
      {"max_actions_per_step", max_actions_per_step, 1},
      {"max_actions_per_run", max_actions_per_run, 1},
      {"max_active_actions", max_active_actions, 1},
      {"max_active_attempts", max_active_attempts, 1},
      {"max_attempts_per_action", max_attempts_per_action, 1},
      {"max_retry_history", max_retry_history, 1},
      {"max_model_calls", max_model_calls, 1},
      {"max_tool_calls", max_tool_calls, 1},
      {"max_memory_bindings", max_memory_bindings, 1},
      {"max_checkpoint_bindings", max_checkpoint_bindings, 1},
      {"max_policy_records", max_policy_records, 1},
      {"max_budget_records", max_budget_records, 1},
      {"max_assignment_history", max_assignment_history, 1},
      {"max_retained_history", max_retained_history, 1},
      {"max_explanation_factors", max_explanation_factors, 1},
      {"max_completions_per_action", max_completions_per_action, 1},
      {"max_connections", max_connections, 1},
      {"max_session_threads", max_session_threads, 1},
      {"max_send_queue_frames", max_send_queue_frames, 1},
      {"max_outstanding_correlations", max_outstanding_correlations, 1},
      {"max_frame_payload_bytes", max_frame_payload_bytes, 16},
      {"max_payload_bytes", max_payload_bytes, 16},
      {"max_string_bytes", max_string_bytes, 16},
      {"max_collection_entries", max_collection_entries, 1},
      {"max_persistence_bytes", max_persistence_bytes, 1024},
      {"max_temporary_files", max_temporary_files, 1},
  };
  for (const Check& check : checks) {
    std::string error = require_at_least(check.name, check.value, check.minimum);
    if (!error.empty()) {
      return error;
    }
  }
  if (max_actions_per_step > max_actions_per_run) {
    return "max_actions_per_step must not exceed max_actions_per_run";
  }
  if (max_active_attempts < max_active_actions) {
    return "max_active_attempts must be at least max_active_actions";
  }
  if (max_parallel_read_actions > max_active_actions) {
    return "max_parallel_read_actions must not exceed max_active_actions";
  }
  if (max_parallel_tool_calls > max_active_attempts) {
    return "max_parallel_tool_calls must not exceed max_active_attempts";
  }
  if (max_parallel_model_calls > max_active_attempts) {
    return "max_parallel_model_calls must not exceed max_active_attempts";
  }
  if (max_frame_payload_bytes > max_payload_bytes) {
    return "max_frame_payload_bytes must not exceed max_payload_bytes";
  }
  std::string positive = require_positive("max_persistence_bytes", max_persistence_bytes);
  if (!positive.empty()) {
    return positive;
  }
  return {};
}

ResourceLimits default_resource_limits() { return ResourceLimits{}; }

}  // namespace agent_runtime
