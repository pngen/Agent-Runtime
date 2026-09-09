// Agent Runtime - public runtime API.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef AGENT_RUNTIME_RUNTIME_HPP
#define AGENT_RUNTIME_RUNTIME_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "agent_runtime/authority.hpp"
#include "agent_runtime/backends.hpp"
#include "agent_runtime/clock.hpp"
#include "agent_runtime/ids.hpp"
#include "agent_runtime/limits.hpp"
#include "agent_runtime/lifecycle.hpp"
#include "agent_runtime/outcome.hpp"
#include "agent_runtime/policy.hpp"

namespace agent_runtime {

struct AgentRuntimeOptions {
  AgentRuntimeId runtime_id{};
  AgentRuntimeGeneration runtime_generation{1};
  RuntimeBootId runtime_boot_id{1};

  AgentId agent_id{};
  AgentGeneration agent_generation{1};
  AgentBootId agent_boot_id{1};

  CoordinatorEpoch coordinator_epoch{1};
  RuntimeEpoch runtime_epoch{1};

  ResourceLimits limits{};
  RuntimePolicy policy = RuntimePolicy::embedded();

  std::shared_ptr<Clock> clock;

  std::shared_ptr<SchedulerAuthorityProvider> scheduler_authority;
  std::shared_ptr<BudgetProvider> budget_provider;
  std::shared_ptr<PolicyProvider> policy_provider;
  std::shared_ptr<MemoryProvider> memory_provider;
  std::shared_ptr<CheckpointProvider> checkpoint_provider;
  std::shared_ptr<ExecutionAuthorityProvider> execution_authority_provider;

  /// Optional default backends. Additional backends may be registered later
  /// with register_model_backend / register_tool_backend.
  std::shared_ptr<ModelBackend> model_backend;
  std::shared_ptr<ToolBackend> tool_backend;

  /// When true, dynamic state restored from persistence is never treated as
  /// current. It is true by default and cannot be disabled.
  bool conservative_recovery = true;
};

/// Specification used to declare an action.
struct ActionSpec {
  ActionKind kind = ActionKind::CUSTOM;
  std::string label;
  SideEffectClass side_effect = SideEffectClass::UNKNOWN;

  std::string model_target;
  std::string tool_name;
  std::string operation_key;

  std::string input;
  std::uint64_t input_digest = 0;
  std::uint32_t max_output_bytes = 65536;

  std::string memory_binding_identity;
  MemoryBindingGeneration memory_binding_generation{};

  std::vector<ActionId> dependencies;
  bool requires_checkpoint = false;
  /// Optional per-action retry override. When has_retry_override is false the
  /// runtime policy retry configuration applies.
  bool has_retry_override = false;
  RetryPolicy retry;
};

struct ActionHandle {
  ActionId action_id{};
  ActionGeneration action_generation{};
  StepId step_id{};
  StepGeneration step_generation{};
  AgentRunGeneration run_generation{};
  ActionState state = ActionState::DECLARED;
};

enum class DispatchMode : std::uint8_t {
  /// The attempt is reserved and the request is returned to the caller, which
  /// is responsible for delivering it and reporting the completion. Used by the
  /// distributed coordinator.
  EXTERNAL = 0,
  /// The attempt is reserved and no request is produced. Used when an external
  /// transport already owns delivery.
  NONE,
};

/// Result of reserving dispatch authority for an action.
struct DispatchResult {
  MutationResult result;
  ActionAttemptId attempt_id{};
  AttemptGeneration attempt_generation{};
  ModelRequest model_request;
  ToolRequest tool_request;
  bool has_model_request = false;
  bool has_tool_request = false;
};

/// Result of an embedded dispatch that invokes an in-process backend.
struct LocalExecutionResult {
  MutationResult dispatch;
  MutationResult completion;
  ActionAttemptId attempt_id{};
  AttemptGeneration attempt_generation{};
};

struct ActionFailure {
  ActionId action_id{};
  ActionGeneration action_generation{};
  ActionAttemptId attempt_id{};
  AttemptGeneration attempt_generation{};
  RetryClass failure_class = RetryClass::UNKNOWN;
  CompletionStatus status = CompletionStatus::FAILED;
  std::string message;
  bool completion_ambiguous = false;
};

struct ResumeContext {
  RuntimeBootId runtime_boot_id{};
  AgentBootId agent_boot_id{};
  RuntimeEpoch runtime_epoch{};
  CoordinatorEpoch coordinator_epoch{};
  std::optional<SchedulerAuthority> assignment;
};

struct RecoveryContext {
  RuntimeBootId runtime_boot_id{};
  AgentBootId agent_boot_id{};
  RuntimeEpoch runtime_epoch{};
  CoordinatorEpoch coordinator_epoch{};
  bool advance_generation = true;
};

struct InvariantViolation {
  std::string name;
  std::string detail;
};

struct InvariantReport {
  bool ok = true;
  std::vector<InvariantViolation> violations;
  std::uint32_t checks_run = 0;

  [[nodiscard]] std::string to_text() const;
};

struct ActionView {
  ActionId action_id{};
  ActionGeneration action_generation{};
  ActionKind kind = ActionKind::CUSTOM;
  std::string label;
  SideEffectClass side_effect = SideEffectClass::UNKNOWN;
  ActionState state = ActionState::DECLARED;
  StepId step_id{};
  StepGeneration step_generation{};
  AgentRunGeneration run_generation{};
  std::uint32_t attempt_count = 0;
  ActionAttemptId current_attempt{};
  AttemptGeneration current_attempt_generation{};
  bool committed = false;
  ResultId accepted_result{};
  CompletionGeneration accepted_completion_generation{};
  RetryClass last_failure = RetryClass::NONE;
  CompletionStatus completion_status = CompletionStatus::UNKNOWN;
  PolicyGeneration policy_generation{};
  BudgetGeneration budget_generation{};
  std::string tool_name;
  std::string model_target;
  std::string operation_key;
  std::uint64_t input_digest = 0;
  std::uint64_t result_digest = 0;
  std::string last_error;
};

struct AttemptView {
  ActionAttemptId attempt_id{};
  AttemptGeneration attempt_generation{};
  ActionId action_id{};
  ActionGeneration action_generation{};
  RuntimeBootId runtime_boot_id{};
  RuntimeEpoch runtime_epoch{};
  CoordinatorEpoch coordinator_epoch{};
  ActionState state = ActionState::DECLARED;
  CompletionStatus completion_status = CompletionStatus::UNKNOWN;
  ResultId result_id{};
  CompletionGeneration completion_generation{};
  RetryClass failure_class = RetryClass::NONE;
  BackendId backend_id{};
  Generation<BackendTag> backend_generation{};
  std::uint64_t result_digest = 0;
  bool superseded = false;
  bool ambiguous = false;
  bool current = false;
  UnixMillis dispatched_at_unix_millis = 0;
  UnixMillis completed_at_unix_millis = 0;
  std::string error;
};

struct CheckpointView {
  CheckpointBindingId binding_id{};
  CheckpointGeneration generation{};
  CheckpointState state = CheckpointState::NONE;
  AgentRuntimeGeneration runtime_generation{};
  AgentRunGeneration run_generation{};
  StepGeneration step_generation{};
  ProgressGeneration progress_generation{};
  RuntimeEpoch runtime_epoch{};
  std::string checkpoint_identity;
  std::string integrity_digest;
  bool restorable = false;
  bool current = false;
};

struct RuntimeSummary {
  AgentRuntimeId runtime_id{};
  AgentRuntimeGeneration runtime_generation{};
  AgentId agent_id{};
  AgentGeneration agent_generation{};
  AgentBootId agent_boot_id{};
  RuntimeBootId runtime_boot_id{};
  RuntimeEpoch runtime_epoch{};
  CoordinatorEpoch coordinator_epoch{};
  RuntimeLifecycle lifecycle = RuntimeLifecycle::DECLARED;
  AgentRunId run_id{};
  AgentRunGeneration run_generation{};
  StepId step_id{};
  StepGeneration step_generation{};
  ProgressState progress = ProgressState::NONE;
  ProgressGeneration progress_generation{};
  CancellationState cancellation = CancellationState::NONE;
  RecoveryStatus recovery = RecoveryStatus::NONE;
  SnapshotValidity validity = SnapshotValidity::CURRENT;
  std::uint32_t total_actions = 0;
  std::uint32_t committed_actions = 0;
  std::uint32_t in_flight_actions = 0;
  std::uint32_t retry_pending_actions = 0;
  std::uint32_t ambiguous_actions = 0;
  std::uint32_t revalidation_actions = 0;
  std::uint32_t total_attempts = 0;
  std::uint32_t checkpoints = 0;
  std::uint32_t memory_bindings = 0;
  std::uint32_t fenced_boots = 0;
  std::uint64_t semantic_digest = 0;
  std::string terminal_reason;
};

/// Immutable snapshot. A snapshot bound to a superseded generation can never
/// authorize current execution; its validity says so explicitly.
struct RuntimeSnapshot {
  SnapshotValidity validity = SnapshotValidity::REVALIDATION_REQUIRED;
  RuntimeSummary summary;
  std::vector<ActionView> actions;
  std::vector<AttemptView> attempts;
  std::vector<CheckpointView> checkpoints;
  std::vector<MemoryBinding> memory_bindings;
  std::vector<ToolBinding> tool_bindings;
  std::vector<ModelBinding> model_bindings;
  std::vector<RuntimeBootId> fenced_runtime_boots;
  std::vector<AgentBootId> fenced_agent_boots;
  std::uint64_t semantic_digest = 0;

  [[nodiscard]] std::string render_text() const;
  [[nodiscard]] std::string render_json() const;
  [[nodiscard]] bool authorizes_execution() const noexcept {
    return validity == SnapshotValidity::CURRENT;
  }
};

/// One logical agent execution context. Instances are independent; a process may
/// host several. All methods are thread-safe and use narrow critical sections:
/// no canonical lock is ever held across network I/O, filesystem I/O, external
/// backend calls, thread joins, callbacks or accelerator synchronization.
class AgentRuntime {
 public:
  explicit AgentRuntime(AgentRuntimeOptions options);
  ~AgentRuntime();

  AgentRuntime(const AgentRuntime&) = delete;
  AgentRuntime& operator=(const AgentRuntime&) = delete;
  AgentRuntime(AgentRuntime&&) = delete;
  AgentRuntime& operator=(AgentRuntime&&) = delete;

  // --- identity -------------------------------------------------------------
  [[nodiscard]] AgentRuntimeId id() const noexcept;
  [[nodiscard]] AgentRuntimeGeneration generation() const noexcept;
  [[nodiscard]] RuntimeBootId boot_id() const noexcept;
  [[nodiscard]] RuntimeEpoch runtime_epoch() const noexcept;
  [[nodiscard]] CoordinatorEpoch coordinator_epoch() const noexcept;
  [[nodiscard]] RuntimeLifecycle lifecycle() const noexcept;

  // --- lifecycle and authority ---------------------------------------------
  [[nodiscard]] MutationResult initialize();
  [[nodiscard]] MutationResult bind_assignment(SchedulerAuthority authority);
  [[nodiscard]] MutationResult revalidate_assignment();
  [[nodiscard]] MutationResult set_policy(RuntimePolicy policy);
  [[nodiscard]] MutationResult set_budget(BudgetEvidence evidence);
  [[nodiscard]] MutationResult register_model_backend(BackendIncarnation incarnation,
                                                      std::shared_ptr<ModelBackend> backend);
  [[nodiscard]] MutationResult register_tool_backend(BackendIncarnation incarnation,
                                                     std::shared_ptr<ToolBackend> backend);
  /// Registers a backend incarnation whose implementation runs in another
  /// process. Dispatch in EXTERNAL mode may bind to it; local execution paths
  /// reject it because no local implementation exists.
  [[nodiscard]] MutationResult register_remote_backend(BackendIncarnation incarnation);

  [[nodiscard]] MutationResult bind_model(ModelBinding binding);
  [[nodiscard]] MutationResult bind_tool(ToolBinding binding);
  [[nodiscard]] MutationResult bind_memory(MemoryBinding binding);
  [[nodiscard]] MutationResult invalidate_memory(MemoryBindingId binding_id,
                                                 MemoryBindingGeneration generation,
                                                 std::string reason);
  [[nodiscard]] MutationResult fence_boot(RuntimeBootId runtime_boot_id,
                                          AgentBootId agent_boot_id, std::string reason);

  // --- run and steps --------------------------------------------------------
  [[nodiscard]] MutationResult start_run(AgentRunId run_id, AgentRunGeneration run_generation);
  [[nodiscard]] MutationResult begin_step();

  // --- action lifecycle -----------------------------------------------------
  [[nodiscard]] MutationResult declare_action(const ActionSpec& spec, ActionHandle& out);
  [[nodiscard]] MutationResult admit_action(ActionId action_id, ActionGeneration action_generation);
  [[nodiscard]] MutationResult authorize_action(ActionId action_id,
                                                ActionGeneration action_generation);
  [[nodiscard]] DispatchResult dispatch_action(ActionId action_id, ActionGeneration action_generation,
                                               DispatchMode mode = DispatchMode::EXTERNAL);
  [[nodiscard]] LocalExecutionResult dispatch_and_execute(ActionId action_id,
                                                          ActionGeneration action_generation);
  [[nodiscard]] MutationResult submit_model_completion(const ModelResponse& response);
  [[nodiscard]] MutationResult submit_tool_completion(const ToolResponse& response);
  [[nodiscard]] MutationResult submit_failure(const ActionFailure& failure);
  [[nodiscard]] MutationResult commit_action(ActionId action_id, ActionGeneration action_generation);

  // --- checkpoint -----------------------------------------------------------
  [[nodiscard]] MutationResult request_checkpoint(CheckpointBindingId binding_id,
                                                  CheckpointGeneration generation,
                                                  std::string checkpoint_identity);
  [[nodiscard]] MutationResult accept_checkpoint(const CheckpointBinding& binding);
  [[nodiscard]] MutationResult request_restore(CheckpointBindingId binding_id,
                                               CheckpointGeneration generation);
  [[nodiscard]] MutationResult apply_restore(CheckpointBindingId binding_id,
                                             CheckpointGeneration generation);

  // --- suspension, cancellation, drain, terminal ----------------------------
  [[nodiscard]] MutationResult suspend(std::string reason);
  [[nodiscard]] MutationResult resume(const ResumeContext& context);
  [[nodiscard]] MutationResult cancel(std::string reason);
  [[nodiscard]] MutationResult drain();
  [[nodiscard]] MutationResult complete(std::string reason);
  [[nodiscard]] MutationResult fail(std::string reason);
  [[nodiscard]] MutationResult retire();
  [[nodiscard]] MutationResult recover(const RecoveryContext& context);

  // --- inspection -----------------------------------------------------------
  [[nodiscard]] RuntimeSnapshot snapshot() const;
  [[nodiscard]] RuntimeSummary summary() const;
  [[nodiscard]] InvariantReport check_invariants() const;
  [[nodiscard]] Explanation explain_action(ActionId action_id,
                                           ActionGeneration action_generation) const;
  [[nodiscard]] std::vector<ActionView> actions() const;
  [[nodiscard]] std::vector<AttemptView> attempts() const;

  // --- persistence ----------------------------------------------------------
  [[nodiscard]] MutationResult save(const std::string& path) const;
  [[nodiscard]] MutationResult load(const std::string& path);

  /// Opens a durable state file for inspection. The returned runtime adopts the
  /// stored identity and is placed in RECOVERING with every dynamic binding
  /// marked as requiring revalidation: opening a file never makes stored
  /// authority current. Returns nullptr when the file cannot be decoded.
  [[nodiscard]] static std::unique_ptr<AgentRuntime> open_durable_state(
      const std::string& path, MutationResult& result);

  /// Durable semantic digest of canonical state. Stable across serialization
  /// round-trips and independent of insertion order of derived indexes.
  [[nodiscard]] std::uint64_t durable_digest() const;

  /// Requests cancellation of in-flight cancellable work without changing
  /// lifecycle. Used by the distributed coordinator on transport loss.
  void request_inflight_cancellation();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace agent_runtime

#endif  // AGENT_RUNTIME_RUNTIME_HPP
