// Agent Runtime - policy validation and defaults.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "agent_runtime/policy.hpp"

#include <algorithm>
#include <limits>

namespace agent_runtime {

std::uint64_t RetryPolicy::backoff_for_attempt(std::uint32_t attempt_index) const noexcept {
  if (backoff_base_millis == 0) {
    return 0;
  }
  if (attempt_index == 0) {
    return backoff_base_millis;
  }
  std::uint64_t value = backoff_base_millis;
  for (std::uint32_t i = 0; i < attempt_index; ++i) {
    if (backoff_multiplier_percent <= 100) {
      break;
    }
    if (value > std::numeric_limits<std::uint64_t>::max() / backoff_multiplier_percent) {
      value = std::numeric_limits<std::uint64_t>::max();
      break;
    }
    value = (value * backoff_multiplier_percent) / 100;
  }
  if (backoff_max_millis != 0 && value > backoff_max_millis) {
    return backoff_max_millis;
  }
  return value;
}

std::string RetryPolicy::validate() const {
  if (max_attempts == 0) {
    return "retry.max_attempts must be greater than zero";
  }
  if (backoff_multiplier_percent == 0) {
    return "retry.backoff_multiplier_percent must be greater than zero";
  }
  if (backoff_max_millis != 0 && backoff_max_millis < backoff_base_millis) {
    return "retry.backoff_max_millis must not be less than backoff_base_millis";
  }
  for (const RetryClass retry_class : retryable_classes) {
    if (retry_class == RetryClass::NONE || retry_class == RetryClass::AMBIGUOUS_COMPLETION) {
      return "retry.retryable_classes must not contain NONE or AMBIGUOUS_COMPLETION";
    }
    if (!is_retryable_class(retry_class)) {
      return std::string("retry.retryable_classes contains a class that is never retryable: ") +
             std::string(to_string(retry_class));
    }
  }
  return {};
}

RuntimePolicy RuntimePolicy::embedded() {
  RuntimePolicy policy;
  policy.policy_id = PolicyId(1);
  policy.generation = PolicyGeneration(1);
  for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(ActionKind::kCount); ++i) {
    policy.allowed_action_kinds.insert(static_cast<ActionKind>(i));
  }
  policy.max_retries_per_action = 3;
  policy.max_parallel_actions = 1;
  policy.max_parallel_read_actions = 1;
  policy.max_parallel_tool_calls = 1;
  policy.max_parallel_model_calls = 1;
  policy.allow_side_effects = true;
  policy.allow_memory_mutation = true;
  policy.allow_checkpoint = true;
  policy.allow_non_repeatable_side_effects = true;
  policy.allow_unknown_side_effects = true;
  policy.require_budget_for_dispatch = false;
  policy.require_assignment_for_dispatch = false;
  policy.retry.max_attempts = 3;
  return policy;
}

RuntimePolicy RuntimePolicy::permissive() {
  RuntimePolicy policy = embedded();
  policy.max_parallel_actions = 16;
  policy.max_parallel_read_actions = 16;
  policy.max_parallel_tool_calls = 8;
  policy.max_parallel_model_calls = 8;
  policy.allow_side_effects = true;
  policy.allow_memory_mutation = true;
  policy.allow_checkpoint = true;
  policy.allow_non_repeatable_side_effects = true;
  policy.allow_unknown_side_effects = true;
  policy.require_budget_for_dispatch = false;
  policy.require_assignment_for_dispatch = false;
  policy.retry.max_attempts = 3;
  return policy;
}

RuntimePolicy RuntimePolicy::conservative() {
  RuntimePolicy policy;
  policy.policy_id = PolicyId(1);
  policy.generation = PolicyGeneration(1);
  policy.allowed_action_kinds.insert(ActionKind::MODEL_CALL);
  policy.allowed_action_kinds.insert(ActionKind::TOOL_CALL);
  policy.allowed_action_kinds.insert(ActionKind::MEMORY_READ);
  policy.allowed_action_kinds.insert(ActionKind::INTERNAL_TRANSITION);
  policy.allowed_action_kinds.insert(ActionKind::EXTERNAL_WAIT);
  policy.max_retries_per_action = 2;
  policy.max_parallel_actions = 1;
  policy.max_parallel_read_actions = 1;
  policy.max_parallel_tool_calls = 1;
  policy.max_parallel_model_calls = 1;
  policy.allow_side_effects = false;
  policy.allow_memory_mutation = false;
  policy.allow_checkpoint = true;
  policy.allow_non_repeatable_side_effects = false;
  policy.allow_unknown_side_effects = false;
  policy.require_budget_for_dispatch = true;
  policy.require_assignment_for_dispatch = true;
  policy.retry.max_attempts = 2;
  policy.retry.checkpoint_before_retry = true;
  return policy;
}

std::string RuntimePolicy::validate() const {
  if (!policy_id.valid()) {
    return "policy_id must be valid";
  }
  if (!generation.valid()) {
    return "policy generation must be valid";
  }
  if (allowed_action_kinds.empty()) {
    return "allowed_action_kinds must not be empty";
  }
  for (const ActionKind kind : allowed_action_kinds) {
    if (kind >= ActionKind::kCount) {
      return "allowed_action_kinds contains an invalid action kind";
    }
  }
  if (max_parallel_actions == 0) {
    return "max_parallel_actions must be greater than zero";
  }
  if (max_parallel_read_actions > max_parallel_actions &&
      !allowed_action_kinds.empty()) {
    // read-only parallelism is a subset of overall parallelism
    return "max_parallel_read_actions must not exceed max_parallel_actions";
  }
  if (max_parallel_tool_calls == 0 || max_parallel_model_calls == 0) {
    return "parallel tool/model call bounds must be greater than zero";
  }
  if (max_parallel_tool_calls > max_parallel_actions) {
    return "max_parallel_tool_calls must not exceed max_parallel_actions";
  }
  if (max_parallel_model_calls > max_parallel_actions) {
    return "max_parallel_model_calls must not exceed max_parallel_actions";
  }
  if (!allow_side_effects && allow_non_repeatable_side_effects) {
    return "allow_non_repeatable_side_effects requires allow_side_effects";
  }
  if (!allow_side_effects && allow_unknown_side_effects) {
    return "allow_unknown_side_effects requires allow_side_effects";
  }
  if (!allow_side_effects && allow_memory_mutation) {
    return "allow_memory_mutation requires allow_side_effects";
  }
  const std::string retry_error = retry.validate();
  if (!retry_error.empty()) {
    return retry_error;
  }
  if (max_retries_per_action == 0) {
    return "max_retries_per_action must be greater than zero";
  }
  if (max_retries_per_action > retry.max_attempts) {
    return "max_retries_per_action must not exceed retry.max_attempts";
  }
  return {};
}

}  // namespace agent_runtime
