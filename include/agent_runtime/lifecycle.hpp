// Agent Runtime - lifecycle, action state, side-effect and retry classification.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef AGENT_RUNTIME_LIFECYCLE_HPP
#define AGENT_RUNTIME_LIFECYCLE_HPP

#include <cstdint>
#include <string_view>

namespace agent_runtime {

/// Lifecycle of one agent runtime instance. WAITING, SUSPENDED, FAILED and
/// CANCELLED are deliberately distinct: a process that is alive is not thereby
/// authoritative, and a suspended runtime is not a failed one.
enum class RuntimeLifecycle : std::uint8_t {
  DECLARED = 0,
  INITIALIZING,
  READY,
  RUNNING,
  WAITING_MODEL,
  WAITING_TOOL,
  WAITING_EXTERNAL,
  CHECKPOINTING,
  SUSPENDING,
  SUSPENDED,
  RECOVERING,
  REVALIDATION_REQUIRED,
  DRAINING,
  CANCELLING,
  CANCELLED,
  COMPLETED,
  FAILED,
  RETIRED,

  kCount
};

[[nodiscard]] std::string_view to_string(RuntimeLifecycle lifecycle) noexcept;
[[nodiscard]] bool is_terminal(RuntimeLifecycle lifecycle) noexcept;
[[nodiscard]] bool is_active(RuntimeLifecycle lifecycle) noexcept;
/// True when the lifecycle may admit a newly declared action.
[[nodiscard]] bool can_admit(RuntimeLifecycle lifecycle) noexcept;
/// True when the lifecycle may dispatch an already authorized action.
[[nodiscard]] bool can_dispatch(RuntimeLifecycle lifecycle) noexcept;
/// True when the runtime may resume from this lifecycle.
[[nodiscard]] bool can_resume(RuntimeLifecycle lifecycle) noexcept;
/// True when the transition between the two lifecycles is legal.
[[nodiscard]] bool is_legal_transition(RuntimeLifecycle from, RuntimeLifecycle to) noexcept;

/// Lifecycle of one action within a step.
enum class ActionState : std::uint8_t {
  DECLARED = 0,
  ADMITTED,
  AUTHORIZED,
  DISPATCHED,
  IN_FLIGHT,
  COMPLETED_UNVALIDATED,
  COMMITTED,
  RETRY_PENDING,
  SUSPENDED,
  CANCELLED,
  FAILED,
  SUPERSEDED,
  REVALIDATION_REQUIRED,

  kCount
};

[[nodiscard]] std::string_view to_string(ActionState state) noexcept;
[[nodiscard]] bool is_terminal(ActionState state) noexcept;
[[nodiscard]] bool is_dispatchable(ActionState state) noexcept;
[[nodiscard]] bool is_in_flight(ActionState state) noexcept;
[[nodiscard]] bool is_legal_transition(ActionState from, ActionState to) noexcept;

/// Kind of agent operation. Extensible: CUSTOM carries a caller-supplied label.
enum class ActionKind : std::uint8_t {
  MODEL_CALL = 0,
  TOOL_CALL,
  MEMORY_READ,
  MEMORY_WRITE_INTENT,
  CHECKPOINT,
  INTERNAL_TRANSITION,
  EXTERNAL_WAIT,
  EMIT_RESULT,
  CUSTOM,

  kCount
};

[[nodiscard]] std::string_view to_string(ActionKind kind) noexcept;

/// Side-effect classification. UNKNOWN is conservative: it is never assumed
/// repeatable, deduplicatable or free of external effect.
enum class SideEffectClass : std::uint8_t {
  PURE = 0,
  READ_ONLY,
  IDEMPOTENT,
  DEDUPLICATABLE,
  COMMIT_TOKEN_REQUIRED,
  NON_REPEATABLE,
  UNKNOWN,

  kCount
};

[[nodiscard]] std::string_view to_string(SideEffectClass side_effect) noexcept;
/// True when repeating the action after an ambiguous failure is unsafe without
/// external reconciliation.
[[nodiscard]] bool requires_reconciliation_on_ambiguity(SideEffectClass side_effect) noexcept;
/// True when the action may be repeated with a stable operation key and the
/// backend deduplicates on that key.
[[nodiscard]] bool supports_operation_key(SideEffectClass side_effect) noexcept;
/// True when the class is known to be free of externally observable effect.
[[nodiscard]] bool is_effect_free(SideEffectClass side_effect) noexcept;

/// Failure classification driving retry policy.
enum class RetryClass : std::uint8_t {
  NONE = 0,
  TRANSIENT,
  PERMANENT,
  STALE_AUTHORITY,
  BUDGET_EXHAUSTED,
  POLICY_REJECTED,
  CANCELLED,
  INCOMPATIBLE,
  AMBIGUOUS_COMPLETION,
  RESOURCE_UNAVAILABLE,
  MODEL_UNAVAILABLE,
  TOOL_UNAVAILABLE,
  CORRUPT_STATE,
  UNKNOWN,

  kCount
};

[[nodiscard]] std::string_view to_string(RetryClass retry_class) noexcept;
/// True when this class may be retried at all under a policy that lists it.
[[nodiscard]] bool is_retryable_class(RetryClass retry_class) noexcept;

/// Durable progress state. Activity is not progress: a dispatched or even
/// completed action does not advance this until the runtime accepts it.
enum class ProgressState : std::uint8_t {
  NONE = 0,
  IN_PROGRESS,
  BOUNDARY_READY,
  COMMITTED,
  BLOCKED,
  TERMINAL,

  kCount
};

[[nodiscard]] std::string_view to_string(ProgressState state) noexcept;

/// Cancellation state of a runtime instance.
enum class CancellationState : std::uint8_t {
  NONE = 0,
  REQUESTED,
  IN_FLIGHT,
  COMPLETED,

  kCount
};

[[nodiscard]] std::string_view to_string(CancellationState state) noexcept;

/// Recovery status after restart or process loss.
enum class RecoveryStatus : std::uint8_t {
  NONE = 0,
  DURABLE_STATE_RESTORED,
  BINDINGS_RECONSTRUCTED,
  AUTHORITY_REVALIDATED,
  RESUMABLE,

  kCount
};

[[nodiscard]] std::string_view to_string(RecoveryStatus status) noexcept;

/// Validity of a snapshot with respect to current authority.
enum class SnapshotValidity : std::uint8_t {
  CURRENT = 0,
  STALE,
  RECONSTRUCTED,
  REVALIDATION_REQUIRED,

  kCount
};

[[nodiscard]] std::string_view to_string(SnapshotValidity validity) noexcept;

/// Lifecycle of a checkpoint binding.
enum class CheckpointState : std::uint8_t {
  NONE = 0,
  REQUESTED,
  IN_PROGRESS,
  ACCEPTED,
  SUPERSEDED,
  INVALID,
  RESTORE_REQUESTED,
  RESTORE_VALIDATED,
  RESTORE_APPLIED,
  REVALIDATION_REQUIRED,

  kCount
};

[[nodiscard]] std::string_view to_string(CheckpointState state) noexcept;

/// Completion outcome classification for a model or tool call.
enum class CompletionStatus : std::uint8_t {
  UNKNOWN = 0,
  SUCCEEDED,
  FAILED,
  CANCELLED,
  AMBIGUOUS,

  kCount
};

[[nodiscard]] std::string_view to_string(CompletionStatus status) noexcept;

}  // namespace agent_runtime

#endif  // AGENT_RUNTIME_LIFECYCLE_HPP
