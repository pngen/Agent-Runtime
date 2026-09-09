// Agent Runtime - checkpoint, suspension, cancellation, drain and recovery.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <string>
#include <utility>

#include "runtime_impl.hpp"

namespace agent_runtime {

using detail::ActionRecord;
using detail::AttemptRecord;
using detail::CanonicalState;
using detail::CheckpointRecord;
using detail::kInvalidIndex;

namespace {

[[nodiscard]] bool any_in_flight_locked(const CanonicalState& state) noexcept {
  for (const AttemptRecord& attempt : state.attempts) {
    if (is_in_flight(attempt.state) && !attempt.superseded) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool any_pending_authorized_locked(const CanonicalState& state) noexcept {
  for (const ActionRecord& action : state.actions) {
    if (action.state == ActionState::AUTHORIZED || action.state == ActionState::ADMITTED ||
        is_in_flight(action.state)) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] std::uint32_t count_ambiguous_locked(const CanonicalState& state) noexcept {
  std::uint32_t count = 0;
  for (const ActionRecord& action : state.actions) {
    if (action.ambiguous && action.state != ActionState::COMMITTED &&
        action.state != ActionState::CANCELLED && action.state != ActionState::FAILED &&
        action.state != ActionState::SUPERSEDED) {
      ++count;
    }
  }
  return count;
}

}  // namespace

MutationResult AgentRuntime::request_checkpoint(CheckpointBindingId binding_id,
                                                CheckpointGeneration generation,
                                                std::string checkpoint_identity) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  CanonicalState& state = impl_->state;
  const std::string subject = "checkpoint/" + std::to_string(binding_id.value()) + "/" +
                              std::to_string(generation.value());

  if (!state.policy.allow_checkpoint) {
    return detail::reject(OutcomeCode::REJECT_POLICY, subject, "policy forbids checkpoints");
  }
  if (!binding_id.valid() || !generation.valid()) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject,
                          "checkpoint binding identity is incomplete");
  }
  if (checkpoint_identity.size() > impl_->limits.max_string_bytes) {
    return detail::reject(OutcomeCode::REJECT_LIMIT, subject,
                          "checkpoint identity exceeds string limit");
  }
  if (state.lifecycle == RuntimeLifecycle::CANCELLED ||
      state.lifecycle == RuntimeLifecycle::CANCELLING) {
    return detail::reject(OutcomeCode::REJECT_CANCELLED, subject, "runtime is cancelled");
  }
  if (state.lifecycle == RuntimeLifecycle::RETIRED) {
    return detail::reject(OutcomeCode::REJECT_RETIRED, subject, "runtime is retired");
  }
  if (state.current_run_index == kInvalidIndex) {
    return detail::reject(OutcomeCode::REJECT_NOT_READY, subject, "no current run");
  }
  for (const CheckpointRecord& existing : state.checkpoints) {
    if (existing.binding.binding_id == binding_id) {
      if (existing.binding.generation > generation) {
        return detail::reject(OutcomeCode::REJECT_STALE_CHECKPOINT, subject,
                              "checkpoint generation is older than the recorded generation");
      }
    }
  }
  if (state.checkpoints.size() >= impl_->limits.max_checkpoint_bindings) {
    bool found = false;
    for (const CheckpointRecord& existing : state.checkpoints) {
      if (existing.binding.binding_id == binding_id) {
        found = true;
        break;
      }
    }
    if (!found) {
      return detail::reject(OutcomeCode::REJECT_LIMIT, subject,
                            "checkpoint binding limit reached");
    }
  }

  CheckpointBinding binding;
  binding.binding_id = binding_id;
  binding.generation = generation;
  binding.runtime_id = state.runtime_id;
  binding.runtime_generation = state.runtime_generation;
  binding.run_id = state.run_id;
  binding.run_generation = state.run_generation;
  binding.step_id = state.step_id;
  binding.step_generation = state.step_generation;
  binding.runtime_epoch = state.runtime_epoch;
  binding.progress_generation = state.progress_generation;
  binding.checkpoint_identity = std::move(checkpoint_identity);
  binding.provenance = "runtime request";
  binding.restorable = false;
  binding.current = false;
  binding.state = CheckpointState::REQUESTED;
  if (!state.memory_bindings.empty()) {
    binding.memory_generation = state.memory_bindings.front().generation;
  }

  bool replaced = false;
  for (CheckpointRecord& existing : state.checkpoints) {
    if (existing.binding.binding_id == binding_id) {
      existing.binding = binding;
      existing.state = CheckpointState::REQUESTED;
      existing.recorded_at_unix_millis = impl_->now_unix();
      replaced = true;
      break;
    }
  }
  if (!replaced) {
    CheckpointRecord record;
    record.binding = binding;
    record.state = CheckpointState::REQUESTED;
    record.recorded_at_unix_millis = impl_->now_unix();
    state.checkpoints.push_back(std::move(record));
  }
  state.pending_checkpoint_binding = binding_id;
  state.pending_checkpoint_generation = generation;
  if (state.lifecycle == RuntimeLifecycle::RUNNING ||
      state.lifecycle == RuntimeLifecycle::WAITING_MODEL ||
      state.lifecycle == RuntimeLifecycle::WAITING_TOOL ||
      state.lifecycle == RuntimeLifecycle::WAITING_EXTERNAL) {
    state.lifecycle = RuntimeLifecycle::CHECKPOINTING;
  }
  return impl_->finish_locked(detail::accepted(subject, "checkpoint requested"));
}

MutationResult AgentRuntime::accept_checkpoint(const CheckpointBinding& binding) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  CanonicalState& state = impl_->state;
  const std::string subject = "checkpoint/" + std::to_string(binding.binding_id.value()) + "/" +
                              std::to_string(binding.generation.value());

  if (!binding.binding_id.valid() || !binding.generation.valid()) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject,
                          "checkpoint binding identity is incomplete");
  }
  CheckpointRecord* record = nullptr;
  for (CheckpointRecord& candidate : state.checkpoints) {
    if (candidate.binding.binding_id == binding.binding_id) {
      record = &candidate;
      break;
    }
  }
  if (record == nullptr) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject,
                          "checkpoint binding was never requested");
  }
  if (binding.generation < record->binding.generation) {
    return detail::reject(OutcomeCode::REJECT_STALE_CHECKPOINT, subject,
                          "checkpoint generation is older than the recorded generation");
  }
  if (binding.runtime_id != state.runtime_id) {
    return detail::reject(OutcomeCode::REJECT_STALE_CHECKPOINT, subject,
                          "checkpoint belongs to a different runtime identity");
  }
  if (binding.runtime_generation != state.runtime_generation) {
    return detail::reject(OutcomeCode::REJECT_STALE_CHECKPOINT, subject,
                          "checkpoint runtime generation is not compatible with the current one");
  }
  if (binding.run_id != state.run_id || binding.run_generation != state.run_generation) {
    return detail::reject(OutcomeCode::REJECT_STALE_RUN_GENERATION, subject,
                          "checkpoint belongs to a superseded run generation");
  }
  if (binding.runtime_epoch != state.runtime_epoch) {
    return detail::reject(OutcomeCode::REJECT_STALE_RUNTIME_EPOCH, subject,
                          "checkpoint belongs to a superseded runtime epoch");
  }
  if (record->state == CheckpointState::ACCEPTED &&
      record->binding.generation == binding.generation &&
      record->binding.integrity_digest == binding.integrity_digest) {
    return detail::no_change(subject, "checkpoint already accepted");
  }

  CheckpointBinding accepted_binding = binding;
  accepted_binding.current = true;
  accepted_binding.restorable = binding.restorable;
  accepted_binding.state = CheckpointState::ACCEPTED;
  record->binding = std::move(accepted_binding);
  record->state = CheckpointState::ACCEPTED;
  record->recorded_at_unix_millis = impl_->now_unix();

  for (ActionRecord& action : state.actions) {
    if (action.requires_checkpoint && action.state != ActionState::COMMITTED &&
        action.state != ActionState::CANCELLED && action.state != ActionState::FAILED &&
        action.state != ActionState::SUPERSEDED) {
      action.checkpoint_binding_id = record->binding.binding_id;
      action.checkpoint_generation = record->binding.generation;
      if (action.state == ActionState::REVALIDATION_REQUIRED) {
        impl_->set_action_state_locked(action, ActionState::ADMITTED);
      }
    }
  }
  state.checkpoint_due = false;
  state.commits_at_last_checkpoint = state.committed_actions;
  state.pending_checkpoint_binding = CheckpointBindingId{};
  state.pending_checkpoint_generation = CheckpointGeneration{};
  if (state.lifecycle == RuntimeLifecycle::CHECKPOINTING) {
    state.lifecycle = state.current_run_index != kInvalidIndex &&
                              !state.runs[state.current_run_index].terminal
                          ? RuntimeLifecycle::RUNNING
                          : RuntimeLifecycle::READY;
  }
  return impl_->finish_locked(detail::accepted(
      subject, "checkpoint accepted",
      {ExplanationFactor{"progress_generation",
                         std::to_string(state.progress_generation.value())},
       ExplanationFactor{"restorable", binding.restorable ? "true" : "false"}}));
}

MutationResult AgentRuntime::request_restore(CheckpointBindingId binding_id,
                                             CheckpointGeneration generation) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  CanonicalState& state = impl_->state;
  const std::string subject = "checkpoint/" + std::to_string(binding_id.value()) + "/" +
                              std::to_string(generation.value());

  CheckpointRecord* record = nullptr;
  for (CheckpointRecord& candidate : state.checkpoints) {
    if (candidate.binding.binding_id == binding_id) {
      record = &candidate;
      break;
    }
  }
  if (record == nullptr) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject, "checkpoint does not exist");
  }
  if (record->binding.generation != generation) {
    return detail::reject(OutcomeCode::REJECT_STALE_CHECKPOINT, subject,
                          "checkpoint generation does not match the requested generation");
  }
  if (record->state == CheckpointState::SUPERSEDED || record->state == CheckpointState::INVALID) {
    return detail::reject(OutcomeCode::REJECT_STALE_CHECKPOINT, subject,
                          "checkpoint is no longer valid");
  }
  if (!record->binding.restorable || !record->binding.current) {
    return detail::reject(OutcomeCode::REVALIDATION_REQUIRED, subject,
                          "checkpoint is not restorable or not current");
  }
  if (!record->binding.compatible_with(state.runtime_id, state.runtime_generation, state.run_id,
                                       state.run_generation)) {
    return detail::reject(OutcomeCode::REJECT_STALE_CHECKPOINT, subject,
                          "checkpoint is not compatible with the current runtime state");
  }
  if (state.cancellation == CancellationState::COMPLETED ||
      state.lifecycle == RuntimeLifecycle::CANCELLED) {
    return detail::reject(OutcomeCode::REJECT_CANCELLED, subject,
                          "a cancelled runtime cannot restore into execution");
  }
  if (state.lifecycle == RuntimeLifecycle::RETIRED) {
    return detail::reject(OutcomeCode::REJECT_RETIRED, subject, "runtime is retired");
  }

  record->state = CheckpointState::RESTORE_VALIDATED;
  for (CheckpointRecord& candidate : state.checkpoints) {
    if (candidate.binding.binding_id != binding_id) {
      candidate.binding.current = false;
    }
  }
  for (ActionRecord& action : state.actions) {
    if (action.state == ActionState::COMMITTED) {
      continue;
    }
    if (is_terminal(action.state)) {
      continue;
    }
    impl_->set_action_state_locked(action, ActionState::REVALIDATION_REQUIRED);
    action.current_attempt = ActionAttemptId{};
    action.current_attempt_generation = AttemptGeneration{};
  }
  for (AttemptRecord& attempt : state.attempts) {
    if (is_in_flight(attempt.state)) {
      attempt.superseded = true;
      attempt.current = false;
      impl_->set_attempt_state_locked(attempt, ActionState::REVALIDATION_REQUIRED);
      attempt.error = "restore requested while the attempt was in flight";
    }
  }
  state.lifecycle = RuntimeLifecycle::RECOVERING;
  state.recovery = RecoveryStatus::DURABLE_STATE_RESTORED;
  return impl_->finish_locked(detail::accepted(
      subject, "restore validated against the current runtime state",
      {ExplanationFactor{"checkpoint_progress_generation",
                         std::to_string(record->binding.progress_generation.value())}}));
}

MutationResult AgentRuntime::apply_restore(CheckpointBindingId binding_id,
                                           CheckpointGeneration generation) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  CanonicalState& state = impl_->state;
  const std::string subject = "checkpoint/" + std::to_string(binding_id.value()) + "/" +
                              std::to_string(generation.value());

  CheckpointRecord* record = nullptr;
  for (CheckpointRecord& candidate : state.checkpoints) {
    if (candidate.binding.binding_id == binding_id) {
      record = &candidate;
      break;
    }
  }
  if (record == nullptr) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject, "checkpoint does not exist");
  }
  if (record->binding.generation != generation) {
    return detail::reject(OutcomeCode::REJECT_STALE_CHECKPOINT, subject,
                          "checkpoint generation does not match");
  }
  if (record->state != CheckpointState::RESTORE_VALIDATED) {
    return detail::reject(OutcomeCode::REJECT_NOT_READY, subject,
                          "restore has not been validated for this checkpoint");
  }
  record->state = CheckpointState::RESTORE_APPLIED;
  // A restore is a new authoritative generation, never a decrement of history.
  state.runtime_generation = state.runtime_generation.next();
  state.recovery = RecoveryStatus::DURABLE_STATE_RESTORED;
  state.lifecycle = RuntimeLifecycle::REVALIDATION_REQUIRED;
  detail::record_provenance(state, *impl_->clock, "apply_restore", subject);
  return impl_->finish_locked(detail::accepted(
      subject, "restore applied as a new authoritative runtime generation",
      {ExplanationFactor{"runtime_generation", std::to_string(state.runtime_generation.value())}}));
}

MutationResult AgentRuntime::suspend(std::string reason) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  CanonicalState& state = impl_->state;
  const std::string subject = "runtime/" + std::to_string(state.runtime_id.value());

  if (state.lifecycle == RuntimeLifecycle::SUSPENDED) {
    return detail::no_change(subject, "runtime is already suspended");
  }
  if (is_terminal(state.lifecycle)) {
    return detail::reject(OutcomeCode::REJECT_COMPLETED, subject,
                          std::string("runtime lifecycle ") + std::string(to_string(state.lifecycle)) +
                              " cannot suspend");
  }
  if (state.checkpoint_due) {
    state.lifecycle = RuntimeLifecycle::SUSPENDING;
    return MutationResult(OutcomeCode::DEFERRED,
                          ExplanationBuilder(OutcomeCode::DEFERRED, subject)
                              .set_message("a required checkpoint must be accepted before suspension")
                              .build());
  }

  state.lifecycle = RuntimeLifecycle::SUSPENDING;
  for (ActionRecord& action : state.actions) {
    switch (action.state) {
      case ActionState::DECLARED:
      case ActionState::ADMITTED:
      case ActionState::AUTHORIZED:
        impl_->set_action_state_locked(action, ActionState::SUSPENDED);
        break;
      default:
        break;
    }
  }
  for (AttemptRecord& attempt : state.attempts) {
    if (is_in_flight(attempt.state) && !attempt.superseded) {
      ActionRecord* action = nullptr;
      const std::uint32_t index = detail::find_action(state, attempt.action_id);
      if (index != kInvalidIndex) {
        action = &state.actions[index];
      }
      if (action != nullptr) {
        impl_->classify_unresolved_locked(*action, attempt,
                                          "runtime suspended while the attempt was in flight");
      }
    }
  }

  if (any_in_flight_locked(state)) {
    return MutationResult(OutcomeCode::DEFERRED,
                          ExplanationBuilder(OutcomeCode::DEFERRED, subject)
                              .add("reason", reason)
                              .set_message("in-flight work is still being classified")
                              .build());
  }
  state.lifecycle = RuntimeLifecycle::SUSPENDED;
  state.recovery = RecoveryStatus::NONE;
  detail::record_provenance(state, *impl_->clock, "suspend", reason);
  return impl_->finish_locked(detail::accepted(
      subject, "runtime suspended: " + reason,
      {ExplanationFactor{"ambiguous_actions", std::to_string(count_ambiguous_locked(state))},
       ExplanationFactor{"dispatchable_actions", "0"}}));
}

MutationResult AgentRuntime::resume(const ResumeContext& context) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  CanonicalState& state = impl_->state;
  const std::string subject = "runtime/" + std::to_string(state.runtime_id.value());

  if (state.lifecycle == RuntimeLifecycle::RETIRED) {
    return detail::reject(OutcomeCode::REJECT_RETIRED, subject, "retired runtime cannot resume");
  }
  if (state.lifecycle == RuntimeLifecycle::CANCELLED ||
      state.cancellation == CancellationState::COMPLETED) {
    return detail::reject(OutcomeCode::REJECT_CANCELLED, subject, "cancelled runtime cannot resume");
  }
  if (state.lifecycle == RuntimeLifecycle::COMPLETED) {
    return detail::reject(OutcomeCode::REJECT_COMPLETED, subject,
                          "completed runtime cannot resume");
  }
  // Recovery may already have installed the fresh incarnation; resuming under
  // exactly that incarnation is legal. Any other boot must be strictly newer.
  const bool same_incarnation = context.runtime_boot_id == state.runtime_boot_id &&
                                context.agent_boot_id == state.agent_boot_id;
  if (!same_incarnation) {
    if (context.runtime_boot_id < state.runtime_boot_id) {
      return detail::reject(OutcomeCode::REJECT_STALE_RUNTIME_BOOT, subject,
                            "resume runtime boot is older than the current boot");
    }
    if (context.agent_boot_id < state.agent_boot_id) {
      return detail::reject(OutcomeCode::REJECT_STALE_AGENT_BOOT, subject,
                            "resume agent boot is older than the current boot");
    }
  }
  if (context.runtime_epoch < state.runtime_epoch) {
    return detail::reject(OutcomeCode::REJECT_STALE_RUNTIME_EPOCH, subject,
                          "resume runtime epoch is older than the current epoch");
  }  if (!can_resume(state.lifecycle) && state.lifecycle != RuntimeLifecycle::READY) {
    return detail::reject(OutcomeCode::REJECT_NOT_RESUMABLE, subject,
                          std::string("runtime lifecycle ") + std::string(to_string(state.lifecycle)) +
                              " is not resumable");
  }
  if (!context.runtime_boot_id.valid() || !context.agent_boot_id.valid() ||
      !context.runtime_epoch.valid() || !context.coordinator_epoch.valid()) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject,
                          "resume context does not supply fresh process authority");
  }

  if (context.coordinator_epoch < state.coordinator_epoch) {
    return detail::reject(OutcomeCode::REJECT_STALE_RUNTIME_EPOCH, subject,
                          "resume coordinator epoch is older than the current epoch");
  }

  // Revalidate scheduler authority before any action becomes runnable again.
  if (context.assignment.has_value()) {
    const SchedulerAuthority& supplied = *context.assignment;
    if (!supplied.complete()) {
      return detail::reject(OutcomeCode::REJECT_INVALID, subject,
                            "resume assignment is incomplete");
    }
    if (supplied.coordinator_epoch < context.coordinator_epoch) {
      return detail::reject(OutcomeCode::REJECT_STALE_ASSIGNMENT, subject,
                            "resume assignment coordinator epoch is older than the resume epoch");
    }
    if (state.has_assignment) {
      const SchedulerAuthority& current = state.current_assignment;
      if (supplied.assignment_id == current.assignment_id &&
          supplied.assignment_generation < current.assignment_generation) {
        return detail::reject(OutcomeCode::REJECT_STALE_ASSIGNMENT, subject,
                              "resume assignment generation is superseded");
      }
      if (supplied.work_id == current.work_id &&
          supplied.work_generation < current.work_generation) {
        return detail::reject(OutcomeCode::REJECT_STALE_ASSIGNMENT, subject,
                              "resume work generation is superseded");
      }
    }
    for (detail::AssignmentRecord& record : state.assignments) {
      record.current = false;
    }
    detail::AssignmentRecord record;
    record.authority = supplied;
    record.authority.current = true;
    record.current = true;
    record.reason = "resume";
    record.recorded_at_unix_millis = impl_->now_unix();
    state.assignments.push_back(std::move(record));
    state.current_assignment = supplied;
    state.current_assignment.current = true;
    state.has_assignment = true;
  } else if (state.policy.require_assignment_for_dispatch) {
    if (impl_->options.scheduler_authority != nullptr) {
      std::optional<SchedulerAuthority> fresh =
          impl_->options.scheduler_authority->current_assignment(state.runtime_id);
      if (!fresh.has_value()) {
        state.lifecycle = RuntimeLifecycle::REVALIDATION_REQUIRED;
        return detail::reject(OutcomeCode::REJECT_STALE_ASSIGNMENT, subject,
                              "no current scheduler assignment is available for resume");
      }
      state.current_assignment = *fresh;
      state.current_assignment.current = true;
      state.has_assignment = true;
    } else if (!state.has_assignment || !state.current_assignment.current) {
      state.lifecycle = RuntimeLifecycle::REVALIDATION_REQUIRED;
      return detail::reject(OutcomeCode::REJECT_STALE_ASSIGNMENT, subject,
                            "policy requires an assignment and none is current");
    }
  }

  // Revalidate bindings through the configured providers where available.
  if (impl_->options.memory_provider != nullptr) {
    for (MemoryBinding& binding : state.memory_bindings) {
      std::optional<MemoryBinding> refreshed =
          impl_->options.memory_provider->revalidate(binding);
      if (!refreshed.has_value()) {
        binding.fresh = false;
        binding.read_authority = false;
        binding.write_authority = false;
      } else {
        binding = *refreshed;
      }
    }
  }
  if (impl_->options.policy_provider != nullptr) {
    std::optional<RuntimePolicy> refreshed =
        impl_->options.policy_provider->current_policy(state.runtime_id);
    if (refreshed.has_value()) {
      const std::string error = refreshed->validate();
      if (error.empty() && refreshed->generation >= state.policy.generation) {
        state.policy = *refreshed;
        state.policy_history.push_back(*refreshed);
      }
    }
  }
  if (impl_->options.budget_provider != nullptr) {
    BudgetEvidence evidence = impl_->options.budget_provider->query(state.runtime_id, state.budget.budget_id);
    if (evidence.budget_id.valid() && evidence.generation.valid() &&
        (!state.has_budget || evidence.generation >= state.budget.generation)) {
      state.budget = evidence;
      state.has_budget = true;
      state.budget_history.push_back(evidence);
    }
  }

  bool needs_revalidation = false;
  std::string revalidation_reason;
  if (state.policy.require_budget_for_dispatch) {
    if (!state.has_budget || state.budget.outcome != BudgetOutcome::ALLOW) {
      needs_revalidation = true;
      revalidation_reason = "budget authority is not current and allowing";
    }
  }
  for (ModelBinding& binding : state.model_bindings) {
    if (impl_->backends.find_model(binding.backend_id) == kInvalidIndex) {
      binding.current = false;
      needs_revalidation = true;
      revalidation_reason = "model backend is not registered in this process";
      continue;
    }
    const std::uint32_t index = impl_->backends.find_model(binding.backend_id);
    if (impl_->backends.model_incarnations[index].generation != binding.backend_generation) {
      binding.current = false;
      needs_revalidation = true;
      revalidation_reason = "model backend incarnation moved";
    }
  }
  for (ToolBinding& binding : state.tool_bindings) {
    if (impl_->backends.find_tool(binding.backend_id) == kInvalidIndex) {
      binding.current = false;
      needs_revalidation = true;
      revalidation_reason = "tool backend is not registered in this process";
      continue;
    }
    const std::uint32_t index = impl_->backends.find_tool(binding.backend_id);
    if (impl_->backends.tool_incarnations[index].generation != binding.backend_generation) {
      binding.current = false;
      needs_revalidation = true;
      revalidation_reason = "tool backend incarnation moved";
    }
  }

  // Apply the new process authority, fencing the previous incarnation only when
  // it actually changes.
  if (!same_incarnation) {
    state.fenced_boots.push_back(detail::FencedBootRecord{
        state.runtime_boot_id, state.agent_boot_id, "superseded by resume", impl_->now_unix()});
    state.runtime_boot_id = context.runtime_boot_id;
    state.agent_boot_id = context.agent_boot_id;
  }
  state.runtime_epoch = context.runtime_epoch;
  if (context.coordinator_epoch > state.coordinator_epoch) {
    state.coordinator_epoch = context.coordinator_epoch;
  }
  state.cancellation = CancellationState::NONE;
  state.cancellation_reason.clear();

  std::uint32_t resumed = 0;
  std::uint32_t held = 0;
  for (ActionRecord& action : state.actions) {
    if (action.state == ActionState::COMMITTED || is_terminal(action.state)) {
      continue;
    }
    if (action.ambiguous || action.state == ActionState::REVALIDATION_REQUIRED) {
      if (action.ambiguous) {
        ++held;
        continue;
      }
      impl_->set_action_state_locked(action, ActionState::ADMITTED);
      action.runtime_boot_id = state.runtime_boot_id;
      action.runtime_epoch = state.runtime_epoch;
      action.coordinator_epoch = state.coordinator_epoch;
      ++resumed;
      continue;
    }
    if (action.state == ActionState::SUSPENDED || action.state == ActionState::RETRY_PENDING ||
        is_in_flight(action.state) || action.state == ActionState::COMPLETED_UNVALIDATED) {
      if (is_in_flight(action.state)) {
        impl_->set_action_state_locked(action, ActionState::RETRY_PENDING);
      } else if (action.state == ActionState::SUSPENDED) {
        impl_->set_action_state_locked(action, ActionState::ADMITTED);
      }
      action.runtime_boot_id = state.runtime_boot_id;
      action.runtime_epoch = state.runtime_epoch;
      action.coordinator_epoch = state.coordinator_epoch;
      ++resumed;
    }
  }
  for (AttemptRecord& attempt : state.attempts) {
    if (is_in_flight(attempt.state)) {
      attempt.superseded = true;
      attempt.current = false;
      impl_->set_attempt_state_locked(attempt, ActionState::REVALIDATION_REQUIRED);
      attempt.error = "attempt invalidated by resume";
    }
  }
  for (ActionRecord& action : state.actions) {
    if (action.state == ActionState::ADMITTED || action.state == ActionState::RETRY_PENDING) {
      action.current_attempt = ActionAttemptId{};
      action.current_attempt_generation = AttemptGeneration{};
    }
  }

  if (needs_revalidation) {
    state.lifecycle = RuntimeLifecycle::REVALIDATION_REQUIRED;
    state.recovery = RecoveryStatus::BINDINGS_RECONSTRUCTED;
    return MutationResult(OutcomeCode::REVALIDATION_REQUIRED,
                          ExplanationBuilder(OutcomeCode::REVALIDATION_REQUIRED, subject)
                              .add("reason", revalidation_reason)
                              .add_u64("resumed_actions", resumed)
                              .add_u64("held_actions", held)
                              .set_message("resume requires binding revalidation")
                              .build());
  }
  state.lifecycle = state.current_run_index != kInvalidIndex &&
                            !state.runs[state.current_run_index].terminal
                        ? RuntimeLifecycle::RUNNING
                        : RuntimeLifecycle::READY;
  state.recovery = RecoveryStatus::RESUMABLE;
  detail::record_provenance(state, *impl_->clock, "resume", "fresh process authority installed");
  return impl_->finish_locked(detail::accepted(
      subject, "runtime resumed under fresh process authority",
      {ExplanationFactor{"held_actions", std::to_string(held)},
       ExplanationFactor{"resumed_actions", std::to_string(resumed)},
       ExplanationFactor{"runtime_boot", std::to_string(state.runtime_boot_id.value())},
       ExplanationFactor{"runtime_epoch", std::to_string(state.runtime_epoch.value())}}));
}

MutationResult AgentRuntime::cancel(std::string reason) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  CanonicalState& state = impl_->state;
  const std::string subject = "runtime/" + std::to_string(state.runtime_id.value());

  if (state.lifecycle == RuntimeLifecycle::RETIRED) {
    return detail::reject(OutcomeCode::REJECT_RETIRED, subject, "runtime is retired");
  }
  if (state.lifecycle == RuntimeLifecycle::CANCELLED ||
      state.cancellation == CancellationState::COMPLETED) {
    return detail::no_change(subject, "runtime is already cancelled");
  }
  if (state.lifecycle == RuntimeLifecycle::COMPLETED) {
    return detail::reject(OutcomeCode::REJECT_COMPLETED, subject,
                          "completed runtime cannot be cancelled");
  }

  impl_->inflight_cancellation->store(true, std::memory_order_release);
  state.cancellation = CancellationState::REQUESTED;
  state.cancellation_reason = reason;
  state.lifecycle = RuntimeLifecycle::CANCELLING;

  std::uint32_t invalidated = 0;
  std::uint32_t ambiguous = 0;
  for (ActionRecord& action : state.actions) {
    switch (action.state) {
      case ActionState::DECLARED:
      case ActionState::ADMITTED:
      case ActionState::AUTHORIZED:
      case ActionState::RETRY_PENDING:
      case ActionState::SUSPENDED:
      case ActionState::REVALIDATION_REQUIRED:
        impl_->set_action_state_locked(action, ActionState::CANCELLED);
        action.current_attempt = ActionAttemptId{};
        action.current_attempt_generation = AttemptGeneration{};
        ++invalidated;
        break;
      default:
        break;
    }
  }
  for (AttemptRecord& attempt : state.attempts) {
    if (!is_in_flight(attempt.state) || attempt.superseded) {
      continue;
    }
    const std::uint32_t index = detail::find_action(state, attempt.action_id);
    if (index == kInvalidIndex) {
      attempt.superseded = true;
      attempt.current = false;
      impl_->set_attempt_state_locked(attempt, ActionState::CANCELLED);
      continue;
    }
    ActionRecord& action = state.actions[index];
    if (requires_reconciliation_on_ambiguity(action.side_effect)) {
      attempt.superseded = true;
      attempt.current = false;
      impl_->set_attempt_state_locked(attempt, ActionState::REVALIDATION_REQUIRED);
      attempt.ambiguous = true;
      attempt.completion_status = CompletionStatus::AMBIGUOUS;
      attempt.error = "cancellation could not prove the external side effect did not occur";
      impl_->set_action_state_locked(action, ActionState::REVALIDATION_REQUIRED);
      action.ambiguous = true;
      action.completion_status = CompletionStatus::AMBIGUOUS;
      action.last_failure = RetryClass::AMBIGUOUS_COMPLETION;
      action.last_error = attempt.error;
      action.current_attempt = ActionAttemptId{};
      action.current_attempt_generation = AttemptGeneration{};
      ++ambiguous;
    } else {
      attempt.superseded = true;
      attempt.current = false;
      impl_->set_attempt_state_locked(attempt, ActionState::CANCELLED);
      attempt.completion_status = CompletionStatus::CANCELLED;
      attempt.error = "cancelled";
      impl_->set_action_state_locked(action, ActionState::CANCELLED);
      action.current_attempt = ActionAttemptId{};
      action.current_attempt_generation = AttemptGeneration{};
      ++invalidated;
    }
  }

  state.cancellation = CancellationState::COMPLETED;
  state.lifecycle = RuntimeLifecycle::CANCELLED;
  state.terminal_reason = "cancelled: " + reason;
  if (state.current_run_index != kInvalidIndex) {
    detail::RunRecord& run = state.runs[state.current_run_index];
    run.terminal = true;
    run.terminal_reason = state.terminal_reason;
    run.ended_at_unix_millis = impl_->now_unix();
  }
  if (state.current_step_index != kInvalidIndex) {
    state.steps[state.current_step_index].progress = ProgressState::TERMINAL;
    state.steps[state.current_step_index].closed_at_unix_millis = impl_->now_unix();
  }
  state.has_assignment = false;
  state.current_assignment.current = false;
  for (detail::AssignmentRecord& record : state.assignments) {
    record.current = false;
  }
  detail::record_provenance(state, *impl_->clock, "cancel", reason);
  return impl_->finish_locked(detail::accepted(
      subject, "runtime cancelled: " + reason,
      {ExplanationFactor{"ambiguous_actions", std::to_string(ambiguous)},
       ExplanationFactor{"invalidated_actions", std::to_string(invalidated)},
       ExplanationFactor{"progress_committed", "false"}}));
}

MutationResult AgentRuntime::drain() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  CanonicalState& state = impl_->state;
  const std::string subject = "runtime/" + std::to_string(state.runtime_id.value());

  if (state.lifecycle == RuntimeLifecycle::CANCELLED ||
      state.lifecycle == RuntimeLifecycle::CANCELLING) {
    return detail::reject(OutcomeCode::REJECT_CANCELLED, subject, "runtime is cancelled");
  }
  if (state.lifecycle == RuntimeLifecycle::RETIRED) {
    return detail::reject(OutcomeCode::REJECT_RETIRED, subject, "runtime is retired");
  }
  if (state.lifecycle == RuntimeLifecycle::COMPLETED) {
    return detail::no_change(subject, "runtime is already complete");
  }
  if (state.lifecycle == RuntimeLifecycle::FAILED) {
    return detail::reject(OutcomeCode::REJECT_COMPLETED, subject, "runtime has failed");
  }

  state.lifecycle = RuntimeLifecycle::DRAINING;
  for (ActionRecord& action : state.actions) {
    if (action.state == ActionState::DECLARED || action.state == ActionState::ADMITTED ||
        action.state == ActionState::RETRY_PENDING) {
      impl_->set_action_state_locked(action, ActionState::CANCELLED);
      action.current_attempt = ActionAttemptId{};
      action.current_attempt_generation = AttemptGeneration{};
    }
  }

  const bool pending = state.policy.drain_waits_for_authorized_actions
                           ? any_pending_authorized_locked(state)
                           : any_in_flight_locked(state);
  if (pending) {
    return MutationResult(OutcomeCode::DEFERRED,
                          ExplanationBuilder(OutcomeCode::DEFERRED, subject)
                              .set_message("draining: already-authorized obligations remain")
                              .build());
  }
  if (state.checkpoint_due) {
    return MutationResult(OutcomeCode::DEFERRED,
                          ExplanationBuilder(OutcomeCode::DEFERRED, subject)
                              .set_message("draining: a required checkpoint must be accepted")
                              .build());
  }

  state.lifecycle = RuntimeLifecycle::COMPLETED;
  state.terminal_reason = "drained";
  if (state.current_run_index != kInvalidIndex) {
    detail::RunRecord& run = state.runs[state.current_run_index];
    run.terminal = true;
    run.terminal_reason = state.terminal_reason;
    run.ended_at_unix_millis = impl_->now_unix();
  }
  if (state.current_step_index != kInvalidIndex) {
    state.steps[state.current_step_index].progress = ProgressState::TERMINAL;
    state.steps[state.current_step_index].closed_at_unix_millis = impl_->now_unix();
  }
  state.has_assignment = false;
  state.current_assignment.current = false;
  for (detail::AssignmentRecord& record : state.assignments) {
    record.current = false;
  }
  detail::record_provenance(state, *impl_->clock, "drain", "runtime drained");
  return impl_->finish_locked(detail::accepted(
      subject, "runtime drained",
      {ExplanationFactor{"committed_actions", std::to_string(state.committed_actions)}}));
}

MutationResult AgentRuntime::complete(std::string reason) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  CanonicalState& state = impl_->state;
  const std::string subject = "runtime/" + std::to_string(state.runtime_id.value());

  if (state.lifecycle == RuntimeLifecycle::CANCELLED ||
      state.lifecycle == RuntimeLifecycle::CANCELLING) {
    return detail::reject(OutcomeCode::REJECT_CANCELLED, subject,
                          "a cancelled runtime must not report completion");
  }
  if (state.lifecycle == RuntimeLifecycle::COMPLETED) {
    return detail::no_change(subject, "runtime is already complete");
  }
  if (state.lifecycle == RuntimeLifecycle::RETIRED ||
      state.lifecycle == RuntimeLifecycle::FAILED) {
    return detail::reject(OutcomeCode::REJECT_COMPLETED, subject,
                          std::string("runtime lifecycle ") + std::string(to_string(state.lifecycle)) +
                              " cannot complete");
  }
  if (any_in_flight_locked(state)) {
    return detail::reject(OutcomeCode::REJECT_NOT_READY, subject,
                          "in-flight attempts must be resolved before completion");
  }
  if (count_ambiguous_locked(state) != 0) {
    return detail::reject(OutcomeCode::MANUAL_OR_EXTERNAL_RECONCILIATION_REQUIRED, subject,
                          "ambiguous side effects must be reconciled before completion");
  }

  state.lifecycle = RuntimeLifecycle::COMPLETED;
  state.terminal_reason = std::move(reason);
  if (state.current_run_index != kInvalidIndex) {
    detail::RunRecord& run = state.runs[state.current_run_index];
    run.terminal = true;
    run.terminal_reason = state.terminal_reason;
    run.ended_at_unix_millis = impl_->now_unix();
  }
  if (state.current_step_index != kInvalidIndex) {
    state.steps[state.current_step_index].progress = ProgressState::TERMINAL;
    state.steps[state.current_step_index].closed_at_unix_millis = impl_->now_unix();
  }
  state.has_assignment = false;
  state.current_assignment.current = false;
  for (detail::AssignmentRecord& record : state.assignments) {
    record.current = false;
  }
  detail::record_provenance(state, *impl_->clock, "complete", state.terminal_reason);
  return impl_->finish_locked(detail::accepted(
      subject, "runtime completed",
      {ExplanationFactor{"committed_actions", std::to_string(state.committed_actions)}}));
}

MutationResult AgentRuntime::fail(std::string reason) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  CanonicalState& state = impl_->state;
  const std::string subject = "runtime/" + std::to_string(state.runtime_id.value());

  if (state.lifecycle == RuntimeLifecycle::CANCELLED) {
    return detail::reject(OutcomeCode::REJECT_CANCELLED, subject, "runtime is cancelled");
  }
  if (state.lifecycle == RuntimeLifecycle::RETIRED) {
    return detail::reject(OutcomeCode::REJECT_RETIRED, subject, "runtime is retired");
  }
  if (state.lifecycle == RuntimeLifecycle::FAILED) {
    return detail::no_change(subject, "runtime has already failed");
  }
  if (state.lifecycle == RuntimeLifecycle::COMPLETED) {
    return detail::reject(OutcomeCode::REJECT_COMPLETED, subject,
                          "completed runtime cannot fail");
  }
  state.lifecycle = RuntimeLifecycle::FAILED;
  state.terminal_reason = std::move(reason);
  for (AttemptRecord& attempt : state.attempts) {
    if (is_in_flight(attempt.state)) {
      attempt.superseded = true;
      attempt.current = false;
      impl_->set_attempt_state_locked(attempt, ActionState::FAILED);
      attempt.error = "runtime failed";
    }
  }
  for (ActionRecord& action : state.actions) {
    if (!action.committed && !is_terminal(action.state)) {
      impl_->set_action_state_locked(action, ActionState::FAILED);
      action.last_failure = RetryClass::UNKNOWN;
    }
  }
  if (state.current_run_index != kInvalidIndex) {
    detail::RunRecord& run = state.runs[state.current_run_index];
    run.terminal = true;
    run.terminal_reason = state.terminal_reason;
    run.ended_at_unix_millis = impl_->now_unix();
  }
  state.has_assignment = false;
  state.current_assignment.current = false;
  detail::record_provenance(state, *impl_->clock, "fail", state.terminal_reason);
  return impl_->finish_locked(detail::accepted(subject, "runtime failed: " + state.terminal_reason));
}

MutationResult AgentRuntime::retire() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  CanonicalState& state = impl_->state;
  const std::string subject = "runtime/" + std::to_string(state.runtime_id.value());
  if (state.lifecycle == RuntimeLifecycle::RETIRED) {
    return detail::no_change(subject, "runtime is already retired");
  }
  if (!is_terminal(state.lifecycle)) {
    return detail::reject(OutcomeCode::REJECT_NOT_READY, subject,
                          "runtime must be terminal before retirement");
  }
  state.lifecycle = RuntimeLifecycle::RETIRED;
  detail::record_provenance(state, *impl_->clock, "retire", "runtime retired");
  return impl_->finish_locked(detail::accepted(subject, "runtime retired"));
}

MutationResult AgentRuntime::recover(const RecoveryContext& context) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  CanonicalState& state = impl_->state;
  const std::string subject = "runtime/" + std::to_string(state.runtime_id.value());

  if (!context.runtime_boot_id.valid() || !context.agent_boot_id.valid() ||
      !context.runtime_epoch.valid() || !context.coordinator_epoch.valid()) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject,
                          "recovery context does not supply fresh process authority");
  }
  if (state.lifecycle == RuntimeLifecycle::RETIRED) {
    return detail::reject(OutcomeCode::REJECT_RETIRED, subject, "retired runtime cannot recover");
  }
  if (state.lifecycle == RuntimeLifecycle::CANCELLED) {
    return detail::reject(OutcomeCode::REJECT_CANCELLED, subject,
                          "cancelled runtime cannot recover into execution");
  }
  if (context.runtime_boot_id <= state.runtime_boot_id) {
    return detail::reject(OutcomeCode::REJECT_STALE_RUNTIME_BOOT, subject,
                          "recovery runtime boot is not newer than the current boot");
  }
  if (context.agent_boot_id <= state.agent_boot_id) {
    return detail::reject(OutcomeCode::REJECT_STALE_AGENT_BOOT, subject,
                          "recovery agent boot is not newer than the current boot");
  }
  if (context.runtime_epoch <= state.runtime_epoch) {
    return detail::reject(OutcomeCode::REJECT_STALE_RUNTIME_EPOCH, subject,
                          "recovery runtime epoch is not newer than the current epoch");
  }
  if (context.coordinator_epoch < state.coordinator_epoch) {
    return detail::reject(OutcomeCode::REJECT_STALE_RUNTIME_EPOCH, subject,
                          "recovery coordinator epoch is older than the current epoch");
  }

  // Fence the previous incarnation permanently.
  state.fenced_boots.push_back(detail::FencedBootRecord{state.runtime_boot_id, state.agent_boot_id,
                                                        "superseded by recovery",
                                                        impl_->now_unix()});
  state.runtime_boot_id = context.runtime_boot_id;
  state.agent_boot_id = context.agent_boot_id;
  state.runtime_epoch = context.runtime_epoch;
  if (context.coordinator_epoch > state.coordinator_epoch) {
    state.coordinator_epoch = context.coordinator_epoch;
  }
  if (context.advance_generation) {
    state.runtime_generation = state.runtime_generation.next();
  }

  // Dynamic bindings never become fresh automatically.
  for (ModelBinding& binding : state.model_bindings) {
    binding.current = false;
  }
  for (ToolBinding& binding : state.tool_bindings) {
    binding.current = false;
  }
  for (MemoryBinding& binding : state.memory_bindings) {
    binding.fresh = false;
  }
  for (CheckpointRecord& record : state.checkpoints) {
    if (record.state == CheckpointState::ACCEPTED || record.state == CheckpointState::REQUESTED ||
        record.state == CheckpointState::IN_PROGRESS) {
      record.state = CheckpointState::REVALIDATION_REQUIRED;
      record.binding.current = false;
    }
  }
  state.has_budget = false;
  state.has_assignment = false;
  state.current_assignment.current = false;
  for (detail::AssignmentRecord& record : state.assignments) {
    record.current = false;
  }

  // Unresolved in-flight work is classified conservatively.
  for (AttemptRecord& attempt : state.attempts) {
    if (!is_in_flight(attempt.state) || attempt.superseded) {
      continue;
    }
    const std::uint32_t index = detail::find_action(state, attempt.action_id);
    if (index == kInvalidIndex) {
      attempt.superseded = true;
      attempt.current = false;
      impl_->set_attempt_state_locked(attempt, ActionState::REVALIDATION_REQUIRED);
      continue;
    }
    impl_->classify_unresolved_locked(state.actions[index], attempt,
                                      "process authority was lost while the attempt was in flight");
  }
  for (ActionRecord& action : state.actions) {
    if (is_in_flight(action.state) || action.state == ActionState::COMPLETED_UNVALIDATED) {
      impl_->set_action_state_locked(action, ActionState::REVALIDATION_REQUIRED);
      action.current_attempt = ActionAttemptId{};
      action.current_attempt_generation = AttemptGeneration{};
    }
  }

  state.lifecycle = RuntimeLifecycle::REVALIDATION_REQUIRED;
  state.recovery = RecoveryStatus::BINDINGS_RECONSTRUCTED;
  state.recovery_reason = "recovered under a fresh runtime incarnation";
  detail::record_provenance(state, *impl_->clock, "recover", state.recovery_reason);
  return impl_->finish_locked(detail::accepted(
      subject, "durable state restored; dynamic authority requires revalidation",
      {ExplanationFactor{"coordinator_epoch", std::to_string(state.coordinator_epoch.value())},
       ExplanationFactor{"runtime_boot", std::to_string(state.runtime_boot_id.value())},
       ExplanationFactor{"runtime_epoch", std::to_string(state.runtime_epoch.value())},
       ExplanationFactor{"runtime_generation", std::to_string(state.runtime_generation.value())}}));
}

}  // namespace agent_runtime
