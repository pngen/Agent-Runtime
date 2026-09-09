// Agent Runtime - lifecycle state machines.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "agent_runtime/lifecycle.hpp"

#include <array>

namespace agent_runtime {
namespace {

using Mask = std::uint32_t;
constexpr std::size_t kRuntimeCount = static_cast<std::size_t>(RuntimeLifecycle::kCount);
constexpr std::size_t kActionCount = static_cast<std::size_t>(ActionState::kCount);

[[nodiscard]] constexpr Mask bit(RuntimeLifecycle value) noexcept {
  return Mask{1} << static_cast<unsigned>(value);
}
[[nodiscard]] constexpr Mask bit(ActionState value) noexcept {
  return Mask{1} << static_cast<unsigned>(value);
}

/// Legal runtime lifecycle transitions. A transition not listed here is a
/// defect in the caller, not a recoverable runtime condition.
constexpr std::array<Mask, kRuntimeCount> kRuntimeTransitions = [] {
  std::array<Mask, kRuntimeCount> table{};
  table[static_cast<std::size_t>(RuntimeLifecycle::DECLARED)] =
      bit(RuntimeLifecycle::INITIALIZING) | bit(RuntimeLifecycle::FAILED) |
      bit(RuntimeLifecycle::CANCELLING);
  table[static_cast<std::size_t>(RuntimeLifecycle::INITIALIZING)] =
      bit(RuntimeLifecycle::READY) | bit(RuntimeLifecycle::REVALIDATION_REQUIRED) |
      bit(RuntimeLifecycle::FAILED) | bit(RuntimeLifecycle::CANCELLING) |
      bit(RuntimeLifecycle::SUSPENDING);
  table[static_cast<std::size_t>(RuntimeLifecycle::READY)] =
      bit(RuntimeLifecycle::RUNNING) | bit(RuntimeLifecycle::SUSPENDED) |
      bit(RuntimeLifecycle::SUSPENDING) | bit(RuntimeLifecycle::DRAINING) |
      bit(RuntimeLifecycle::CANCELLING) | bit(RuntimeLifecycle::FAILED) |
      bit(RuntimeLifecycle::RETIRED) | bit(RuntimeLifecycle::REVALIDATION_REQUIRED) |
      bit(RuntimeLifecycle::CHECKPOINTING) | bit(RuntimeLifecycle::COMPLETED);
  table[static_cast<std::size_t>(RuntimeLifecycle::RUNNING)] =
      bit(RuntimeLifecycle::WAITING_MODEL) | bit(RuntimeLifecycle::WAITING_TOOL) |
      bit(RuntimeLifecycle::WAITING_EXTERNAL) | bit(RuntimeLifecycle::CHECKPOINTING) |
      bit(RuntimeLifecycle::SUSPENDING) | bit(RuntimeLifecycle::REVALIDATION_REQUIRED) |
      bit(RuntimeLifecycle::DRAINING) | bit(RuntimeLifecycle::CANCELLING) |
      bit(RuntimeLifecycle::COMPLETED) | bit(RuntimeLifecycle::FAILED) |
      bit(RuntimeLifecycle::READY);
  const Mask waiting = bit(RuntimeLifecycle::RUNNING) | bit(RuntimeLifecycle::CHECKPOINTING) |
                       bit(RuntimeLifecycle::SUSPENDING) | bit(RuntimeLifecycle::DRAINING) |
                       bit(RuntimeLifecycle::CANCELLING) | bit(RuntimeLifecycle::FAILED) |
                       bit(RuntimeLifecycle::REVALIDATION_REQUIRED) |
                       bit(RuntimeLifecycle::COMPLETED);
  table[static_cast<std::size_t>(RuntimeLifecycle::WAITING_MODEL)] = waiting;
  table[static_cast<std::size_t>(RuntimeLifecycle::WAITING_TOOL)] = waiting;
  table[static_cast<std::size_t>(RuntimeLifecycle::WAITING_EXTERNAL)] = waiting;
  table[static_cast<std::size_t>(RuntimeLifecycle::CHECKPOINTING)] =
      bit(RuntimeLifecycle::RUNNING) | bit(RuntimeLifecycle::WAITING_MODEL) |
      bit(RuntimeLifecycle::WAITING_TOOL) | bit(RuntimeLifecycle::WAITING_EXTERNAL) |
      bit(RuntimeLifecycle::SUSPENDING) | bit(RuntimeLifecycle::SUSPENDED) |
      bit(RuntimeLifecycle::REVALIDATION_REQUIRED) | bit(RuntimeLifecycle::FAILED) |
      bit(RuntimeLifecycle::CANCELLING) | bit(RuntimeLifecycle::DRAINING) |
      bit(RuntimeLifecycle::READY);
  table[static_cast<std::size_t>(RuntimeLifecycle::SUSPENDING)] =
      bit(RuntimeLifecycle::SUSPENDED) | bit(RuntimeLifecycle::RUNNING) |
      bit(RuntimeLifecycle::FAILED) | bit(RuntimeLifecycle::REVALIDATION_REQUIRED) |
      bit(RuntimeLifecycle::CANCELLING);
  table[static_cast<std::size_t>(RuntimeLifecycle::SUSPENDED)] =
      bit(RuntimeLifecycle::RECOVERING) | bit(RuntimeLifecycle::REVALIDATION_REQUIRED) |
      bit(RuntimeLifecycle::CANCELLING) | bit(RuntimeLifecycle::DRAINING) |
      bit(RuntimeLifecycle::RETIRED) | bit(RuntimeLifecycle::FAILED) |
      bit(RuntimeLifecycle::READY);
  table[static_cast<std::size_t>(RuntimeLifecycle::RECOVERING)] =
      bit(RuntimeLifecycle::REVALIDATION_REQUIRED) | bit(RuntimeLifecycle::READY) |
      bit(RuntimeLifecycle::SUSPENDED) | bit(RuntimeLifecycle::FAILED) |
      bit(RuntimeLifecycle::CANCELLING) | bit(RuntimeLifecycle::RUNNING);
  table[static_cast<std::size_t>(RuntimeLifecycle::REVALIDATION_REQUIRED)] =
      bit(RuntimeLifecycle::READY) | bit(RuntimeLifecycle::RECOVERING) |
      bit(RuntimeLifecycle::SUSPENDED) | bit(RuntimeLifecycle::FAILED) |
      bit(RuntimeLifecycle::CANCELLING) | bit(RuntimeLifecycle::DRAINING) |
      bit(RuntimeLifecycle::RUNNING);
  table[static_cast<std::size_t>(RuntimeLifecycle::DRAINING)] =
      bit(RuntimeLifecycle::COMPLETED) | bit(RuntimeLifecycle::FAILED) |
      bit(RuntimeLifecycle::CANCELLING) | bit(RuntimeLifecycle::RETIRED) |
      bit(RuntimeLifecycle::SUSPENDED);
  table[static_cast<std::size_t>(RuntimeLifecycle::CANCELLING)] =
      bit(RuntimeLifecycle::CANCELLED) | bit(RuntimeLifecycle::FAILED);
  table[static_cast<std::size_t>(RuntimeLifecycle::CANCELLED)] = bit(RuntimeLifecycle::RETIRED);
  table[static_cast<std::size_t>(RuntimeLifecycle::COMPLETED)] = bit(RuntimeLifecycle::RETIRED);
  table[static_cast<std::size_t>(RuntimeLifecycle::FAILED)] = bit(RuntimeLifecycle::RETIRED);
  table[static_cast<std::size_t>(RuntimeLifecycle::RETIRED)] = 0;
  return table;
}();

constexpr std::array<Mask, kActionCount> kActionTransitions = [] {
  std::array<Mask, kActionCount> table{};
  table[static_cast<std::size_t>(ActionState::DECLARED)] =
      bit(ActionState::ADMITTED) | bit(ActionState::CANCELLED) | bit(ActionState::SUPERSEDED) |
      bit(ActionState::REVALIDATION_REQUIRED) | bit(ActionState::FAILED);
  table[static_cast<std::size_t>(ActionState::ADMITTED)] =
      bit(ActionState::AUTHORIZED) | bit(ActionState::CANCELLED) | bit(ActionState::SUPERSEDED) |
      bit(ActionState::REVALIDATION_REQUIRED) | bit(ActionState::FAILED);
  table[static_cast<std::size_t>(ActionState::AUTHORIZED)] =
      bit(ActionState::DISPATCHED) | bit(ActionState::CANCELLED) | bit(ActionState::SUPERSEDED) |
      bit(ActionState::REVALIDATION_REQUIRED) | bit(ActionState::FAILED) |
      bit(ActionState::SUSPENDED);
  const Mask in_flight = bit(ActionState::COMPLETED_UNVALIDATED) | bit(ActionState::RETRY_PENDING) |
                         bit(ActionState::CANCELLED) | bit(ActionState::FAILED) |
                         bit(ActionState::REVALIDATION_REQUIRED) | bit(ActionState::SUPERSEDED) |
                         bit(ActionState::SUSPENDED);
  table[static_cast<std::size_t>(ActionState::DISPATCHED)] = in_flight | bit(ActionState::IN_FLIGHT);
  table[static_cast<std::size_t>(ActionState::IN_FLIGHT)] = in_flight;
  table[static_cast<std::size_t>(ActionState::COMPLETED_UNVALIDATED)] =
      bit(ActionState::COMMITTED) | bit(ActionState::RETRY_PENDING) | bit(ActionState::FAILED) |
      bit(ActionState::CANCELLED) | bit(ActionState::SUPERSEDED) |
      bit(ActionState::REVALIDATION_REQUIRED);
  table[static_cast<std::size_t>(ActionState::COMMITTED)] = 0;
  table[static_cast<std::size_t>(ActionState::RETRY_PENDING)] =
      bit(ActionState::AUTHORIZED) | bit(ActionState::CANCELLED) | bit(ActionState::FAILED) |
      bit(ActionState::SUPERSEDED) | bit(ActionState::REVALIDATION_REQUIRED) |
      bit(ActionState::SUSPENDED);
  table[static_cast<std::size_t>(ActionState::SUSPENDED)] =
      bit(ActionState::REVALIDATION_REQUIRED) | bit(ActionState::CANCELLED) |
      bit(ActionState::FAILED) | bit(ActionState::SUPERSEDED) | bit(ActionState::AUTHORIZED);
  table[static_cast<std::size_t>(ActionState::CANCELLED)] = 0;
  table[static_cast<std::size_t>(ActionState::FAILED)] = 0;
  table[static_cast<std::size_t>(ActionState::SUPERSEDED)] = 0;
  table[static_cast<std::size_t>(ActionState::REVALIDATION_REQUIRED)] =
      bit(ActionState::ADMITTED) | bit(ActionState::AUTHORIZED) | bit(ActionState::RETRY_PENDING) |
      bit(ActionState::CANCELLED) | bit(ActionState::FAILED) | bit(ActionState::SUPERSEDED) |
      bit(ActionState::DECLARED);
  return table;
}();

}  // namespace

std::string_view to_string(RuntimeLifecycle lifecycle) noexcept {
  switch (lifecycle) {
    case RuntimeLifecycle::DECLARED:
      return "DECLARED";
    case RuntimeLifecycle::INITIALIZING:
      return "INITIALIZING";
    case RuntimeLifecycle::READY:
      return "READY";
    case RuntimeLifecycle::RUNNING:
      return "RUNNING";
    case RuntimeLifecycle::WAITING_MODEL:
      return "WAITING_MODEL";
    case RuntimeLifecycle::WAITING_TOOL:
      return "WAITING_TOOL";
    case RuntimeLifecycle::WAITING_EXTERNAL:
      return "WAITING_EXTERNAL";
    case RuntimeLifecycle::CHECKPOINTING:
      return "CHECKPOINTING";
    case RuntimeLifecycle::SUSPENDING:
      return "SUSPENDING";
    case RuntimeLifecycle::SUSPENDED:
      return "SUSPENDED";
    case RuntimeLifecycle::RECOVERING:
      return "RECOVERING";
    case RuntimeLifecycle::REVALIDATION_REQUIRED:
      return "REVALIDATION_REQUIRED";
    case RuntimeLifecycle::DRAINING:
      return "DRAINING";
    case RuntimeLifecycle::CANCELLING:
      return "CANCELLING";
    case RuntimeLifecycle::CANCELLED:
      return "CANCELLED";
    case RuntimeLifecycle::COMPLETED:
      return "COMPLETED";
    case RuntimeLifecycle::FAILED:
      return "FAILED";
    case RuntimeLifecycle::RETIRED:
      return "RETIRED";
    case RuntimeLifecycle::kCount:
      break;
  }
  return "UNKNOWN_LIFECYCLE";
}

bool is_terminal(RuntimeLifecycle lifecycle) noexcept {
  switch (lifecycle) {
    case RuntimeLifecycle::CANCELLED:
    case RuntimeLifecycle::COMPLETED:
    case RuntimeLifecycle::FAILED:
    case RuntimeLifecycle::RETIRED:
      return true;
    default:
      return false;
  }
}

bool is_active(RuntimeLifecycle lifecycle) noexcept {
  switch (lifecycle) {
    case RuntimeLifecycle::RUNNING:
    case RuntimeLifecycle::WAITING_MODEL:
    case RuntimeLifecycle::WAITING_TOOL:
    case RuntimeLifecycle::WAITING_EXTERNAL:
    case RuntimeLifecycle::CHECKPOINTING:
      return true;
    default:
      return false;
  }
}

bool can_admit(RuntimeLifecycle lifecycle) noexcept {
  switch (lifecycle) {
    case RuntimeLifecycle::READY:
    case RuntimeLifecycle::RUNNING:
    case RuntimeLifecycle::WAITING_MODEL:
    case RuntimeLifecycle::WAITING_TOOL:
    case RuntimeLifecycle::WAITING_EXTERNAL:
    case RuntimeLifecycle::CHECKPOINTING:
      return true;
    default:
      return false;
  }
}

bool can_dispatch(RuntimeLifecycle lifecycle) noexcept {
  switch (lifecycle) {
    case RuntimeLifecycle::RUNNING:
    case RuntimeLifecycle::WAITING_MODEL:
    case RuntimeLifecycle::WAITING_TOOL:
    case RuntimeLifecycle::WAITING_EXTERNAL:
    case RuntimeLifecycle::CHECKPOINTING:
      return true;
    default:
      return false;
  }
}

bool can_resume(RuntimeLifecycle lifecycle) noexcept {
  switch (lifecycle) {
    case RuntimeLifecycle::SUSPENDED:
    case RuntimeLifecycle::REVALIDATION_REQUIRED:
    case RuntimeLifecycle::RECOVERING:
    case RuntimeLifecycle::READY:
      return true;
    default:
      return false;
  }
}

bool is_legal_transition(RuntimeLifecycle from, RuntimeLifecycle to) noexcept {
  const auto index = static_cast<std::size_t>(from);
  if (index >= kRuntimeCount) {
    return false;
  }
  if (from == to) {
    return true;
  }
  return (kRuntimeTransitions[index] & bit(to)) != 0;
}

std::string_view to_string(ActionState state) noexcept {
  switch (state) {
    case ActionState::DECLARED:
      return "DECLARED";
    case ActionState::ADMITTED:
      return "ADMITTED";
    case ActionState::AUTHORIZED:
      return "AUTHORIZED";
    case ActionState::DISPATCHED:
      return "DISPATCHED";
    case ActionState::IN_FLIGHT:
      return "IN_FLIGHT";
    case ActionState::COMPLETED_UNVALIDATED:
      return "COMPLETED_UNVALIDATED";
    case ActionState::COMMITTED:
      return "COMMITTED";
    case ActionState::RETRY_PENDING:
      return "RETRY_PENDING";
    case ActionState::SUSPENDED:
      return "SUSPENDED";
    case ActionState::CANCELLED:
      return "CANCELLED";
    case ActionState::FAILED:
      return "FAILED";
    case ActionState::SUPERSEDED:
      return "SUPERSEDED";
    case ActionState::REVALIDATION_REQUIRED:
      return "REVALIDATION_REQUIRED";
    case ActionState::kCount:
      break;
  }
  return "UNKNOWN_ACTION_STATE";
}

bool is_terminal(ActionState state) noexcept {
  switch (state) {
    case ActionState::COMMITTED:
    case ActionState::CANCELLED:
    case ActionState::FAILED:
    case ActionState::SUPERSEDED:
      return true;
    default:
      return false;
  }
}

bool is_dispatchable(ActionState state) noexcept {
  return state == ActionState::AUTHORIZED;
}

bool is_in_flight(ActionState state) noexcept {
  return state == ActionState::DISPATCHED || state == ActionState::IN_FLIGHT;
}

bool is_legal_transition(ActionState from, ActionState to) noexcept {
  const auto index = static_cast<std::size_t>(from);
  if (index >= kActionCount) {
    return false;
  }
  if (from == to) {
    return true;
  }
  return (kActionTransitions[index] & bit(to)) != 0;
}

std::string_view to_string(ActionKind kind) noexcept {
  switch (kind) {
    case ActionKind::MODEL_CALL:
      return "MODEL_CALL";
    case ActionKind::TOOL_CALL:
      return "TOOL_CALL";
    case ActionKind::MEMORY_READ:
      return "MEMORY_READ";
    case ActionKind::MEMORY_WRITE_INTENT:
      return "MEMORY_WRITE_INTENT";
    case ActionKind::CHECKPOINT:
      return "CHECKPOINT";
    case ActionKind::INTERNAL_TRANSITION:
      return "INTERNAL_TRANSITION";
    case ActionKind::EXTERNAL_WAIT:
      return "EXTERNAL_WAIT";
    case ActionKind::EMIT_RESULT:
      return "EMIT_RESULT";
    case ActionKind::CUSTOM:
      return "CUSTOM";
    case ActionKind::kCount:
      break;
  }
  return "UNKNOWN_ACTION_KIND";
}

std::string_view to_string(SideEffectClass side_effect) noexcept {
  switch (side_effect) {
    case SideEffectClass::PURE:
      return "PURE";
    case SideEffectClass::READ_ONLY:
      return "READ_ONLY";
    case SideEffectClass::IDEMPOTENT:
      return "IDEMPOTENT";
    case SideEffectClass::DEDUPLICATABLE:
      return "DEDUPLICATABLE";
    case SideEffectClass::COMMIT_TOKEN_REQUIRED:
      return "COMMIT_TOKEN_REQUIRED";
    case SideEffectClass::NON_REPEATABLE:
      return "NON_REPEATABLE";
    case SideEffectClass::UNKNOWN:
      return "UNKNOWN";
    case SideEffectClass::kCount:
      break;
  }
  return "UNKNOWN_SIDE_EFFECT";
}

bool requires_reconciliation_on_ambiguity(SideEffectClass side_effect) noexcept {
  switch (side_effect) {
    case SideEffectClass::PURE:
    case SideEffectClass::READ_ONLY:
      return false;
    case SideEffectClass::IDEMPOTENT:
    case SideEffectClass::DEDUPLICATABLE:
      return false;
    case SideEffectClass::COMMIT_TOKEN_REQUIRED:
    case SideEffectClass::NON_REPEATABLE:
    case SideEffectClass::UNKNOWN:
      return true;
    case SideEffectClass::kCount:
      break;
  }
  return true;
}

bool supports_operation_key(SideEffectClass side_effect) noexcept {
  switch (side_effect) {
    case SideEffectClass::IDEMPOTENT:
    case SideEffectClass::DEDUPLICATABLE:
    case SideEffectClass::COMMIT_TOKEN_REQUIRED:
      return true;
    default:
      return false;
  }
}

bool is_effect_free(SideEffectClass side_effect) noexcept {
  return side_effect == SideEffectClass::PURE || side_effect == SideEffectClass::READ_ONLY;
}

std::string_view to_string(RetryClass retry_class) noexcept {
  switch (retry_class) {
    case RetryClass::NONE:
      return "NONE";
    case RetryClass::TRANSIENT:
      return "TRANSIENT";
    case RetryClass::PERMANENT:
      return "PERMANENT";
    case RetryClass::STALE_AUTHORITY:
      return "STALE_AUTHORITY";
    case RetryClass::BUDGET_EXHAUSTED:
      return "BUDGET_EXHAUSTED";
    case RetryClass::POLICY_REJECTED:
      return "POLICY_REJECTED";
    case RetryClass::CANCELLED:
      return "CANCELLED";
    case RetryClass::INCOMPATIBLE:
      return "INCOMPATIBLE";
    case RetryClass::AMBIGUOUS_COMPLETION:
      return "AMBIGUOUS_COMPLETION";
    case RetryClass::RESOURCE_UNAVAILABLE:
      return "RESOURCE_UNAVAILABLE";
    case RetryClass::MODEL_UNAVAILABLE:
      return "MODEL_UNAVAILABLE";
    case RetryClass::TOOL_UNAVAILABLE:
      return "TOOL_UNAVAILABLE";
    case RetryClass::CORRUPT_STATE:
      return "CORRUPT_STATE";
    case RetryClass::UNKNOWN:
      return "UNKNOWN";
    case RetryClass::kCount:
      break;
  }
  return "UNKNOWN_RETRY_CLASS";
}

bool is_retryable_class(RetryClass retry_class) noexcept {
  switch (retry_class) {
    case RetryClass::TRANSIENT:
    case RetryClass::RESOURCE_UNAVAILABLE:
    case RetryClass::MODEL_UNAVAILABLE:
    case RetryClass::TOOL_UNAVAILABLE:
    case RetryClass::UNKNOWN:
      return true;
    default:
      return false;
  }
}

std::string_view to_string(ProgressState state) noexcept {
  switch (state) {
    case ProgressState::NONE:
      return "NONE";
    case ProgressState::IN_PROGRESS:
      return "IN_PROGRESS";
    case ProgressState::BOUNDARY_READY:
      return "BOUNDARY_READY";
    case ProgressState::COMMITTED:
      return "COMMITTED";
    case ProgressState::BLOCKED:
      return "BLOCKED";
    case ProgressState::TERMINAL:
      return "TERMINAL";
    case ProgressState::kCount:
      break;
  }
  return "UNKNOWN_PROGRESS";
}

std::string_view to_string(CancellationState state) noexcept {
  switch (state) {
    case CancellationState::NONE:
      return "NONE";
    case CancellationState::REQUESTED:
      return "REQUESTED";
    case CancellationState::IN_FLIGHT:
      return "IN_FLIGHT";
    case CancellationState::COMPLETED:
      return "COMPLETED";
    case CancellationState::kCount:
      break;
  }
  return "UNKNOWN_CANCELLATION";
}

std::string_view to_string(RecoveryStatus status) noexcept {
  switch (status) {
    case RecoveryStatus::NONE:
      return "NONE";
    case RecoveryStatus::DURABLE_STATE_RESTORED:
      return "DURABLE_STATE_RESTORED";
    case RecoveryStatus::BINDINGS_RECONSTRUCTED:
      return "BINDINGS_RECONSTRUCTED";
    case RecoveryStatus::AUTHORITY_REVALIDATED:
      return "AUTHORITY_REVALIDATED";
    case RecoveryStatus::RESUMABLE:
      return "RESUMABLE";
    case RecoveryStatus::kCount:
      break;
  }
  return "UNKNOWN_RECOVERY";
}

std::string_view to_string(SnapshotValidity validity) noexcept {
  switch (validity) {
    case SnapshotValidity::CURRENT:
      return "CURRENT";
    case SnapshotValidity::STALE:
      return "STALE";
    case SnapshotValidity::RECONSTRUCTED:
      return "RECONSTRUCTED";
    case SnapshotValidity::REVALIDATION_REQUIRED:
      return "REVALIDATION_REQUIRED";
    case SnapshotValidity::kCount:
      break;
  }
  return "UNKNOWN_VALIDITY";
}

std::string_view to_string(CheckpointState state) noexcept {
  switch (state) {
    case CheckpointState::NONE:
      return "NONE";
    case CheckpointState::REQUESTED:
      return "REQUESTED";
    case CheckpointState::IN_PROGRESS:
      return "IN_PROGRESS";
    case CheckpointState::ACCEPTED:
      return "ACCEPTED";
    case CheckpointState::SUPERSEDED:
      return "SUPERSEDED";
    case CheckpointState::INVALID:
      return "INVALID";
    case CheckpointState::RESTORE_REQUESTED:
      return "RESTORE_REQUESTED";
    case CheckpointState::RESTORE_VALIDATED:
      return "RESTORE_VALIDATED";
    case CheckpointState::RESTORE_APPLIED:
      return "RESTORE_APPLIED";
    case CheckpointState::REVALIDATION_REQUIRED:
      return "REVALIDATION_REQUIRED";
    case CheckpointState::kCount:
      break;
  }
  return "UNKNOWN_CHECKPOINT_STATE";
}

std::string_view to_string(CompletionStatus status) noexcept {
  switch (status) {
    case CompletionStatus::UNKNOWN:
      return "UNKNOWN";
    case CompletionStatus::SUCCEEDED:
      return "SUCCEEDED";
    case CompletionStatus::FAILED:
      return "FAILED";
    case CompletionStatus::CANCELLED:
      return "CANCELLED";
    case CompletionStatus::AMBIGUOUS:
      return "AMBIGUOUS";
    case CompletionStatus::kCount:
      break;
  }
  return "UNKNOWN_COMPLETION";
}

}  // namespace agent_runtime
