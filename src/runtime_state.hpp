// Agent Runtime - canonical internal state (not part of the public ABI).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef AGENT_RUNTIME_SRC_RUNTIME_STATE_HPP
#define AGENT_RUNTIME_SRC_RUNTIME_STATE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "agent_runtime/agent_runtime.hpp"

namespace agent_runtime::detail {

inline constexpr std::uint32_t kInvalidIndex = 0xFFFFFFFFu;

/// One physical attempt at an action. Attempts are append-only history; an
/// attempt never becomes current again once superseded.
struct AttemptRecord {
  ActionAttemptId id{};
  AttemptGeneration generation{};
  ActionId action_id{};
  ActionGeneration action_generation{};
  RuntimeBootId runtime_boot_id{};
  RuntimeEpoch runtime_epoch{};
  CoordinatorEpoch coordinator_epoch{};
  ModelCallId model_call_id{};
  ModelCallGeneration model_call_generation{};
  ToolCallId tool_call_id{};
  ToolCallGeneration tool_call_generation{};
  BackendId backend_id{};
  Generation<BackendTag> backend_generation{};
  ActionState state = ActionState::DECLARED;
  CompletionStatus completion_status = CompletionStatus::UNKNOWN;
  ResultId result_id{};
  CompletionGeneration completion_generation{};
  RetryClass failure_class = RetryClass::NONE;
  std::uint64_t result_digest = 0;
  bool superseded = false;
  bool ambiguous = false;
  bool current = false;
  bool completion_recorded = false;
  UnixMillis dispatched_at_unix_millis = 0;
  UnixMillis completed_at_unix_millis = 0;
  std::string error;
};

struct ActionRecord {
  ActionId id{};
  ActionGeneration generation{};
  AgentRunId run_id{};
  AgentRunGeneration run_generation{};
  StepId step_id{};
  StepGeneration step_generation{};

  ActionKind kind = ActionKind::CUSTOM;
  std::string label;
  SideEffectClass side_effect = SideEffectClass::UNKNOWN;
  std::string model_target;
  std::string tool_name;
  std::string operation_key;
  std::string input;
  std::uint64_t input_digest = 0;
  std::uint32_t max_output_bytes = 0;

  MemoryBindingId memory_binding_id{};
  MemoryBindingGeneration memory_binding_generation{};
  Generation<MemoryBindingTag> memory_external_generation{};
  bool requires_memory = false;

  PolicyId policy_id{};
  PolicyGeneration policy_generation{};
  BudgetId budget_id{};
  BudgetGeneration budget_generation{};
  bool budget_required = false;

  ModelCallGeneration model_binding_generation{};
  BackendId model_backend_id{};
  Generation<BackendTag> model_backend_generation{};
  ToolCallGeneration tool_binding_generation{};
  BackendId tool_backend_id{};
  Generation<BackendTag> tool_backend_generation{};

  CheckpointBindingId checkpoint_binding_id{};
  CheckpointGeneration checkpoint_generation{};

  RuntimeBootId runtime_boot_id{};
  RuntimeEpoch runtime_epoch{};
  CoordinatorEpoch coordinator_epoch{};

  ActionState state = ActionState::DECLARED;
  std::uint32_t attempt_count = 0;
  AttemptGeneration next_attempt_generation{1};
  ActionAttemptId current_attempt{};
  AttemptGeneration current_attempt_generation{};
  std::vector<std::uint32_t> attempt_indices;

  RetryPolicy retry{};
  bool has_retry_override = false;

  bool committed = false;
  CommitGeneration commit_generation{};
  ResultId accepted_result{};
  CompletionGeneration accepted_completion_generation{};
  CompletionStatus completion_status = CompletionStatus::UNKNOWN;
  RetryClass last_failure = RetryClass::NONE;
  std::string last_error;
  std::uint64_t result_digest = 0;
  std::uint32_t completions_seen = 0;

  std::vector<ActionId> dependencies;
  bool requires_checkpoint = false;

  Provenance provenance;
  std::uint64_t declaration_sequence = 0;
  std::uint64_t commit_sequence = 0;
  bool ambiguous = false;
};

struct StepRecord {
  StepId id{};
  StepGeneration generation{};
  AgentRunId run_id{};
  AgentRunGeneration run_generation{};
  ProgressState progress = ProgressState::NONE;
  ProgressGeneration progress_generation{};
  std::uint64_t committed_actions = 0;
  std::uint32_t declared_actions = 0;
  UnixMillis opened_at_unix_millis = 0;
  UnixMillis closed_at_unix_millis = 0;
};

struct RunRecord {
  AgentRunId id{};
  AgentRunGeneration generation{};
  AgentRuntimeGeneration runtime_generation{};
  ProgressGeneration progress_generation{};
  std::uint64_t committed_actions = 0;
  std::uint32_t steps = 0;
  bool terminal = false;
  std::string terminal_reason;
  UnixMillis started_at_unix_millis = 0;
  UnixMillis ended_at_unix_millis = 0;
};

struct AssignmentRecord {
  SchedulerAuthority authority;
  bool current = false;
  std::string reason;
  UnixMillis recorded_at_unix_millis = 0;
};

struct FencedBootRecord {
  RuntimeBootId runtime_boot_id{};
  AgentBootId agent_boot_id{};
  std::string reason;
  UnixMillis recorded_at_unix_millis = 0;
};

struct CheckpointRecord {
  CheckpointBinding binding;
  CheckpointState state = CheckpointState::NONE;
  UnixMillis recorded_at_unix_millis = 0;
};

/// Canonical authoritative state. Everything else is derived.
struct CanonicalState {
  AgentRuntimeId runtime_id{};
  AgentRuntimeGeneration runtime_generation{1};
  RuntimeBootId runtime_boot_id{1};
  AgentId agent_id{};
  AgentGeneration agent_generation{1};
  AgentBootId agent_boot_id{1};
  RuntimeEpoch runtime_epoch{1};
  CoordinatorEpoch coordinator_epoch{1};

  RuntimeLifecycle lifecycle = RuntimeLifecycle::DECLARED;
  CancellationState cancellation = CancellationState::NONE;
  RecoveryStatus recovery = RecoveryStatus::NONE;
  std::string terminal_reason;
  std::string cancellation_reason;
  std::string recovery_reason;

  std::vector<RunRecord> runs;
  AgentRunId run_id{};
  AgentRunGeneration run_generation{};
  std::uint32_t current_run_index = kInvalidIndex;

  std::vector<StepRecord> steps;
  StepId step_id{};
  StepGeneration step_generation{};
  std::uint32_t current_step_index = kInvalidIndex;
  StepGeneration next_step_generation{1};

  std::vector<ActionRecord> actions;
  std::vector<AttemptRecord> attempts;
  ActionGeneration next_action_generation{1};
  std::uint64_t next_declaration_sequence = 1;
  std::uint64_t next_commit_sequence = 1;

  std::vector<AssignmentRecord> assignments;
  SchedulerAuthority current_assignment{};
  bool has_assignment = false;

  std::vector<ModelBinding> model_bindings;
  std::vector<ToolBinding> tool_bindings;
  std::vector<MemoryBinding> memory_bindings;
  std::vector<CheckpointRecord> checkpoints;
  std::vector<FencedBootRecord> fenced_boots;

  std::vector<RuntimePolicy> policy_history;
  RuntimePolicy policy{};
  std::vector<BudgetEvidence> budget_history;
  BudgetEvidence budget{};
  bool has_budget = false;

  ProgressGeneration progress_generation{};
  std::uint64_t committed_actions = 0;
  /// Set when policy requires a checkpoint and one is due before any further
  /// dispatch. Cleared only by accepting a compatible checkpoint.
  bool checkpoint_due = false;
  std::uint64_t commits_at_last_checkpoint = 0;
  CheckpointBindingId pending_checkpoint_binding{};
  CheckpointGeneration pending_checkpoint_generation{};

  std::vector<Provenance> provenance_log;
  std::uint64_t mutation_sequence = 0;
};

/// Derived indexes. Never a source of truth: verified against a canonical scan.
struct Indexes {
  std::unordered_map<ActionId, std::uint32_t> action_by_id;
  std::unordered_map<ActionAttemptId, std::uint32_t> attempt_by_id;
  std::unordered_map<ActionId, std::vector<std::uint32_t>> attempts_by_action;
  std::unordered_map<MemoryBindingId, std::uint32_t> memory_by_id;
  std::unordered_map<CheckpointBindingId, std::uint32_t> checkpoint_by_id;
  std::array<std::vector<std::uint32_t>, static_cast<std::size_t>(ActionState::kCount)>
      actions_by_state;
  std::vector<std::uint32_t> pending_model_calls;
  std::vector<std::uint32_t> pending_tool_calls;
  std::vector<std::uint32_t> ambiguous_actions;

  void rebuild(const CanonicalState& state);
};

/// Deterministic semantic digest of canonical durable content.
[[nodiscard]] std::uint64_t canonical_digest(const CanonicalState& state);

/// Canonical JSON rendering of canonical durable content.
[[nodiscard]] std::string canonical_json(const CanonicalState& state);

/// Structural + semantic invariant checking over canonical state.
[[nodiscard]] InvariantReport check_canonical_invariants(const CanonicalState& state,
                                                         const Indexes& indexes,
                                                         const ResourceLimits& limits);

[[nodiscard]] std::uint32_t active_action_count(const CanonicalState& state) noexcept;
[[nodiscard]] std::uint32_t in_flight_attempt_count(const CanonicalState& state) noexcept;
[[nodiscard]] bool any_dispatchable_action(const CanonicalState& state) noexcept;
[[nodiscard]] bool has_ambiguous_action(const CanonicalState& state) noexcept;

}  // namespace agent_runtime::detail

#endif  // AGENT_RUNTIME_SRC_RUNTIME_STATE_HPP
