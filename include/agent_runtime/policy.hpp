// Agent Runtime - retry policy and runtime policy.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef AGENT_RUNTIME_POLICY_HPP
#define AGENT_RUNTIME_POLICY_HPP

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "agent_runtime/ids.hpp"
#include "agent_runtime/lifecycle.hpp"

namespace agent_runtime {

/// Explicit retry policy. Retries are never implicit: a failure class is only
/// retried when the policy names it, the attempt budget is not exhausted, the
/// side-effect class permits repetition, and budget/deadline gates pass.
struct RetryPolicy {
  std::uint32_t max_attempts = 3;
  /// Logical (not wall-clock) delay sequence in milliseconds. Tests never wait
  /// on these values; a runtime that uses an injected clock schedules them.
  std::uint64_t backoff_base_millis = 0;
  std::uint64_t backoff_max_millis = 0;
  /// Multiplier expressed in percent, applied per attempt.
  std::uint32_t backoff_multiplier_percent = 100;

  std::set<RetryClass> retryable_classes{RetryClass::TRANSIENT, RetryClass::RESOURCE_UNAVAILABLE,
                                         RetryClass::MODEL_UNAVAILABLE, RetryClass::TOOL_UNAVAILABLE};

  bool checkpoint_before_retry = false;
  /// When true a retry must keep the same resolved target; otherwise the caller
  /// must re-resolve the target (reroute needed).
  bool same_target = true;
  bool require_budget_reenval = true;
  bool require_deadline_gate = false;
  std::uint64_t deadline_unix_millis = 0;
  /// When true, a retry is refused for side-effect classes that are not safely
  /// repeatable unless a stable operation key is available.
  bool require_side_effect_safety = true;
  /// When true, every attempt revalidates runtime/run/step/action generations.
  bool require_generation_reenval = true;

  [[nodiscard]] bool allows(RetryClass retry_class) const noexcept {
    return retryable_classes.find(retry_class) != retryable_classes.end();
  }
  [[nodiscard]] std::uint64_t backoff_for_attempt(std::uint32_t attempt_index) const noexcept;
  [[nodiscard]] std::string validate() const;
};

/// Runtime policy. Generation-bound: a pending action created under a stale
/// policy must revalidate before dispatch or commit.
struct RuntimePolicy {
  PolicyId policy_id{};
  PolicyGeneration generation{};

  std::set<ActionKind> allowed_action_kinds;
  std::set<std::string> allowed_tools;
  std::set<std::string> allowed_model_targets;

  std::uint32_t max_retries_per_action = 3;
  std::uint32_t max_parallel_actions = 1;
  std::uint32_t max_parallel_read_actions = 1;
  std::uint32_t max_parallel_tool_calls = 1;
  std::uint32_t max_parallel_model_calls = 1;

  bool allow_side_effects = true;
  bool allow_memory_mutation = false;
  bool allow_checkpoint = true;
  bool allow_non_repeatable_side_effects = true;
  bool allow_unknown_side_effects = true;

  std::uint32_t checkpoint_every_commits = 0;
  bool require_budget_for_dispatch = true;
  bool require_assignment_for_dispatch = true;
  bool cancel_requires_reconciliation = true;
  bool drain_waits_for_authorized_actions = true;
  std::uint32_t drain_max_actions = 1024;

  RetryPolicy retry;

  /// Builds the conservative single-action policy used by embedded
  /// single-process use: one active action, no scheduler assignment or budget
  /// adapter required.
  [[nodiscard]] static RuntimePolicy embedded();
  /// Builds a policy with higher concurrency bounds. Used by tests and by
  /// callers that explicitly permit bounded parallel actions.
  [[nodiscard]] static RuntimePolicy permissive();
  /// Builds the conservative default policy.
  [[nodiscard]] static RuntimePolicy conservative();
  [[nodiscard]] std::string validate() const;
};

}  // namespace agent_runtime

#endif  // AGENT_RUNTIME_POLICY_HPP
