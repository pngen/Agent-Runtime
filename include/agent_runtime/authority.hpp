// Agent Runtime - authority, bindings and narrow adapter contracts.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef AGENT_RUNTIME_AUTHORITY_HPP
#define AGENT_RUNTIME_AUTHORITY_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "agent_runtime/clock.hpp"
#include "agent_runtime/ids.hpp"
#include "agent_runtime/lifecycle.hpp"
#include "agent_runtime/policy.hpp"

namespace agent_runtime {

/// Scheduling authority consumed from Agent Scheduler. Agent Runtime never
/// performs candidate discovery, ranking, fairness or placement; it only
/// validates that the assignment it holds is still current.
struct SchedulerAuthority {
  SchedulerAssignmentId assignment_id{};
  AssignmentGeneration assignment_generation{};
  AgentId agent_id{};
  AgentGeneration agent_generation{};
  AgentBootId agent_boot_id{};
  Generation<CoordinatorEpochTag> scheduler_epoch{};
  CoordinatorEpoch coordinator_epoch{};
  DispatchGeneration dispatch_generation{};
  WorkId work_id{};
  WorkGeneration work_generation{};
  /// Opaque lease identity/generation owned by the scheduler.
  std::uint64_t lease_id = 0;
  std::uint64_t lease_generation = 0;
  bool current = false;

  [[nodiscard]] bool complete() const noexcept {
    return assignment_id.valid() && assignment_generation.valid() && agent_id.valid() &&
           agent_generation.valid() && work_id.valid() && work_generation.valid();
  }
};

/// Budget evidence consumed from Cost Governor (or a narrow adapter). UNKNOWN is
/// never interpreted as unlimited.
enum class BudgetOutcome : std::uint8_t {
  UNKNOWN = 0,
  ALLOW,
  DENY,
  DEFER,

  kCount
};

[[nodiscard]] std::string_view to_string(BudgetOutcome outcome) noexcept;

struct BudgetEvidence {
  BudgetId budget_id{};
  BudgetGeneration generation{};
  BudgetOutcome outcome = BudgetOutcome::UNKNOWN;
  std::uint64_t remaining_actions = 0;
  std::uint64_t remaining_requests = 0;
  std::uint64_t remaining_tokens = 0;
  std::uint64_t remaining_tool_calls = 0;
  bool limits_known = false;
  UnixMillis expires_at_unix_millis = 0;
  UnixMillis observed_at_unix_millis = 0;
  std::string provenance;

  [[nodiscard]] bool usable() const noexcept {
    return budget_id.valid() && generation.valid() && outcome == BudgetOutcome::ALLOW;
  }
};

/// Externally supplied model route/binding authority. Agent Runtime does not
/// choose models: it consumes a resolved target identity and generation.
struct ModelBinding {
  std::string target;
  ModelCallGeneration binding_generation{};
  BackendId backend_id{};
  Generation<BackendTag> backend_generation{};
  std::string route_provenance;
  bool current = false;

  [[nodiscard]] bool usable() const noexcept {
    return !target.empty() && binding_generation.valid() && backend_id.valid() &&
           backend_generation.valid() && current;
  }
};

/// Tool binding. The backend incarnation is independent per tool: restarting one
/// tool must not invalidate another.
struct ToolBinding {
  std::string tool_name;
  ToolCallGeneration binding_generation{};
  BackendId backend_id{};
  Generation<BackendTag> backend_generation{};
  SideEffectClass side_effect = SideEffectClass::UNKNOWN;
  std::uint32_t max_input_bytes = 65536;
  std::string provenance;
  bool current = false;

  [[nodiscard]] bool usable() const noexcept {
    return !tool_name.empty() && binding_generation.valid() && backend_id.valid() &&
           backend_generation.valid() && current;
  }
};

/// Generation-bound reference to external memory/context state. Agent Runtime
/// does not own distributed memory; it binds and revalidates references.
struct MemoryBinding {
  MemoryBindingId binding_id{};
  MemoryBindingGeneration generation{};
  std::string state_identity;
  std::string compatibility;
  Generation<MemoryBindingTag> external_generation{};
  bool read_authority = false;
  bool write_authority = false;
  bool fresh = false;
  std::string provenance;
  std::string digest;

  [[nodiscard]] bool usable_for_read() const noexcept {
    return binding_id.valid() && generation.valid() && !state_identity.empty() &&
           external_generation.valid() && read_authority && fresh;
  }
  [[nodiscard]] bool usable_for_write() const noexcept {
    return usable_for_read() && write_authority;
  }
};

/// Checkpoint authority bound to a runtime state. Agent Runtime decides when a
/// checkpoint is required; Checkpoint Fabric owns capture/persist/restore.
struct CheckpointBinding {
  CheckpointBindingId binding_id{};
  CheckpointGeneration generation{};
  AgentRuntimeId runtime_id{};
  AgentRuntimeGeneration runtime_generation{};
  AgentRunId run_id{};
  AgentRunGeneration run_generation{};
  StepId step_id{};
  StepGeneration step_generation{};
  RuntimeEpoch runtime_epoch{};
  ProgressGeneration progress_generation{};
  MemoryBindingGeneration memory_generation{};
  std::string checkpoint_identity;
  std::string integrity_digest;
  std::string provenance;
  bool restorable = false;
  bool current = false;
  CheckpointState state = CheckpointState::NONE;

  /// True when this checkpoint may be restored into the given runtime state.
  [[nodiscard]] bool compatible_with(AgentRuntimeId rt, AgentRuntimeGeneration rt_gen,
                                     AgentRunId run, AgentRunGeneration run_gen) const noexcept {
    return restorable && current && runtime_id == rt && runtime_generation == rt_gen &&
           run_id == run && run_generation == run_gen;
  }
};

/// Narrow adapters. Every adjacent system is consumed through one of these
/// interfaces; none of them is implemented inside Agent Runtime.
class SchedulerAuthorityProvider {
 public:
  virtual ~SchedulerAuthorityProvider() = default;
  /// Returns the currently authoritative assignment for this runtime, or
  /// nullopt when no assignment is current. Never performs ranking or selection.
  [[nodiscard]] virtual std::optional<SchedulerAuthority> current_assignment(
      AgentRuntimeId runtime_id) = 0;
};

class BudgetProvider {
 public:
  virtual ~BudgetProvider() = default;
  [[nodiscard]] virtual BudgetEvidence query(AgentRuntimeId runtime_id, BudgetId budget_id) = 0;
};

class PolicyProvider {
 public:
  virtual ~PolicyProvider() = default;
  [[nodiscard]] virtual std::optional<RuntimePolicy> current_policy(AgentRuntimeId runtime_id) = 0;
};

class MemoryProvider {
 public:
  virtual ~MemoryProvider() = default;
  /// Revalidates a memory binding against the external state store and returns
  /// the refreshed binding, or nullopt when the binding no longer exists.
  [[nodiscard]] virtual std::optional<MemoryBinding> revalidate(
      const MemoryBinding& binding) = 0;
};

class CheckpointProvider {
 public:
  virtual ~CheckpointProvider() = default;
  /// Requests that Checkpoint Fabric capture state for the given binding
  /// request. Returns the checkpoint binding it accepted, or nullopt.
  [[nodiscard]] virtual std::optional<CheckpointBinding> capture(
      const CheckpointBinding& request) = 0;
  [[nodiscard]] virtual std::optional<CheckpointBinding> validate_restore(
      const CheckpointBinding& binding) = 0;
};

class ExecutionAuthorityProvider {
 public:
  virtual ~ExecutionAuthorityProvider() = default;
  /// Returns true when the supplied execution attempt identity is still the
  /// authoritative one for the given action generation.
  [[nodiscard]] virtual bool authorize(const ActionId& action_id,
                                       ActionGeneration action_generation,
                                       ActionAttemptId attempt_id,
                                       AttemptGeneration attempt_generation) = 0;
};

/// Provenance attached to durable records.
struct Provenance {
  std::string origin;
  std::string detail;
  UnixMillis recorded_at_unix_millis = 0;
};

}  // namespace agent_runtime

#endif  // AGENT_RUNTIME_AUTHORITY_HPP
