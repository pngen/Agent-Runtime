// Agent Runtime - shared runtime implementation internals.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef AGENT_RUNTIME_SRC_RUNTIME_IMPL_HPP
#define AGENT_RUNTIME_SRC_RUNTIME_IMPL_HPP

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "agent_runtime/agent_runtime.hpp"
#include "runtime_state.hpp"

namespace agent_runtime::detail {

/// Backend registry. Backends are process-bound dynamic state and are never
/// persisted: a restored runtime must re-register them with fresh incarnations.
struct BackendRegistry {
  std::vector<BackendIncarnation> model_incarnations;
  std::vector<std::shared_ptr<ModelBackend>> model_backends;
  std::vector<BackendIncarnation> tool_incarnations;
  std::vector<std::shared_ptr<ToolBackend>> tool_backends;

  [[nodiscard]] std::uint32_t find_model(BackendId id) const noexcept;
  [[nodiscard]] std::uint32_t find_tool(BackendId id) const noexcept;
  /// A null implementation registers identity only: the backend executes in
  /// another process and is reachable solely through EXTERNAL dispatch.
  [[nodiscard]] bool add_model(BackendIncarnation incarnation,
                               std::shared_ptr<ModelBackend> backend);
  [[nodiscard]] bool add_tool(BackendIncarnation incarnation, std::shared_ptr<ToolBackend> backend);
  void clear();
};

/// Deterministic mutation helpers shared by the runtime translation units.
[[nodiscard]] MutationResult reject(OutcomeCode code, std::string subject, std::string message);

[[nodiscard]] MutationResult accepted(std::string subject, std::string message,
                                      std::vector<ExplanationFactor> factors = {});

[[nodiscard]] MutationResult no_change(std::string subject, std::string message);

/// Deterministic subject string for an action.
[[nodiscard]] std::string action_subject(ActionId id, ActionGeneration generation);

/// Finds the index of an action by identity. Returns kInvalidIndex when absent.
[[nodiscard]] std::uint32_t find_action(const CanonicalState& state, ActionId id) noexcept;

/// True when the runtime boot/agent boot pair has been fenced.
[[nodiscard]] bool is_fenced(const CanonicalState& state, RuntimeBootId runtime_boot_id,
                             AgentBootId agent_boot_id) noexcept;

/// True when the runtime boot has been fenced.
[[nodiscard]] bool is_runtime_boot_fenced(const CanonicalState& state,
                                          RuntimeBootId runtime_boot_id) noexcept;

[[nodiscard]] std::uint32_t count_in_flight_attempts(const CanonicalState& state) noexcept;

[[nodiscard]] std::uint32_t count_active_actions(const CanonicalState& state) noexcept;

[[nodiscard]] bool run_is_terminal(const CanonicalState& state) noexcept;

/// Records provenance on canonical state.
void record_provenance(CanonicalState& state, const Clock& clock, std::string origin,
                       std::string detail);

}  // namespace agent_runtime::detail

namespace agent_runtime {

/// Runtime implementation. Defined here so that every translation unit of the
/// library shares one definition while the public header stays opaque.
struct AgentRuntime::Impl {
  mutable std::mutex mutex;
  AgentRuntimeOptions options;
  ResourceLimits limits;
  std::shared_ptr<Clock> clock;
  detail::CanonicalState state;
  mutable detail::Indexes indexes;
  detail::BackendRegistry backends;
  mutable bool index_dirty_ = false;
  std::uint32_t active_actions_ = 0;
  std::uint32_t in_flight_attempts_ = 0;
  std::uint32_t in_flight_tool_attempts_ = 0;
  std::uint32_t in_flight_model_attempts_ = 0;
  std::uint32_t in_flight_effect_free_attempts_ = 0;
  std::shared_ptr<std::atomic<bool>> inflight_cancellation =
      std::make_shared<std::atomic<bool>>(false);

  Impl();
  explicit Impl(AgentRuntimeOptions options_value);

  [[nodiscard]] UnixMillis now_unix() const { return clock->now_unix_millis(); }

  /// Rebuilds derived indexes from canonical state.
  void reindex() {
    indexes.rebuild(state);
    index_dirty_ = false;
    recount_locked();
  }

  /// Marks derived indexes as stale. Identity maps stay current because
  /// identities are only ever inserted, never removed.
  void mark_dirty() noexcept { index_dirty_ = true; }

  /// Rebuilds derived indexes only when a reader actually needs them.
  void ensure_indexes_locked() const {
    if (index_dirty_) {
      indexes.rebuild(state);
      index_dirty_ = false;
    }
  }

  /// Recomputes the maintained counters from canonical state.
  void recount_locked();

  /// Single choke point for action state changes: keeps the active-action
  /// counter and index freshness correct.
  void set_action_state_locked(detail::ActionRecord& action, ActionState next);

  /// Single choke point for attempt state changes: keeps the in-flight
  /// counters correct.
  void set_attempt_state_locked(detail::AttemptRecord& attempt, ActionState next);

  [[nodiscard]] std::uint32_t active_action_count_locked() const noexcept {
    return active_actions_;
  }
  [[nodiscard]] std::uint32_t in_flight_attempt_count_locked() const noexcept {
    return in_flight_attempts_;
  }
  [[nodiscard]] std::uint32_t in_flight_tool_count_locked() const noexcept {
    return in_flight_tool_attempts_;
  }
  [[nodiscard]] std::uint32_t in_flight_model_count_locked() const noexcept {
    return in_flight_model_attempts_;
  }
  [[nodiscard]] std::uint32_t in_flight_effect_free_count_locked() const noexcept {
    return in_flight_effect_free_attempts_;
  }

  /// Bumps the mutation sequence. Called inside the critical section on every
  /// accepted mutation so that stale post-I/O commits can be detected.
  void bump() { ++state.mutation_sequence; }

  [[nodiscard]] MutationResult finish(MutationResult result);
  [[nodiscard]] MutationResult finish_locked(MutationResult result);

  /// Validates the runtime-level authority required for most mutations.
  [[nodiscard]] bool check_runtime_authority_locked(std::string& message) const;

  [[nodiscard]] detail::ActionRecord* action_locked(ActionId id) noexcept;

  [[nodiscard]] bool policy_allows_locked(const detail::ActionRecord& action,
                                          std::string& message) const;

  /// Returns ACCEPTED, REJECT_BUDGET or REJECT_STALE_BUDGET and fills message.
  [[nodiscard]] OutcomeCode budget_check_locked(const detail::ActionRecord& action,
                                                std::string& message) const;

  [[nodiscard]] bool assignment_current_locked(std::string& message) const;

  [[nodiscard]] bool dependencies_satisfied_locked(const detail::ActionRecord& action,
                                                   std::string& message) const;

  [[nodiscard]] bool side_effect_allowed_locked(const detail::ActionRecord& action,
                                                std::string& message) const;

  /// Advances progress after a successful commit.
  void advance_progress_locked(detail::ActionRecord& action, CommitGeneration generation);

  [[nodiscard]] const RuntimePolicy& effective_policy_locked() const { return state.policy; }

  [[nodiscard]] InvariantReport invariants_locked() const;

  /// Classifies an unresolved in-flight action conservatively after process
  /// loss, recovery or cancellation.
  void classify_unresolved_locked(detail::ActionRecord& action, detail::AttemptRecord& attempt,
                                  std::string reason);

  /// Classifies an unprovable completion according to the side-effect class:
  /// reconciliation is required for classes that cannot be safely repeated, and
  /// an explicit retry is scheduled for classes that can.
  [[nodiscard]] MutationResult classify_ambiguous_completion_locked(
      detail::ActionRecord& action, detail::AttemptRecord& attempt, std::string message);

  [[nodiscard]] MutationResult mark_retry_pending_locked(detail::ActionRecord& action,
                                                         detail::AttemptRecord& attempt,
                                                         RetryClass failure_class,
                                                         std::string message);
};

}  // namespace agent_runtime

#endif  // AGENT_RUNTIME_SRC_RUNTIME_IMPL_HPP
