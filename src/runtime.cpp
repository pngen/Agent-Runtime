// Agent Runtime - runtime core: identity, authority, run/step/action lifecycle.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <string>
#include <utility>

#include "agent_runtime/detail/sha256.hpp"
#include "runtime_impl.hpp"

namespace agent_runtime::detail {
namespace {

[[nodiscard]] std::string join_ids(const std::vector<ActionId>& ids) {
  std::string out;
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (i != 0) {
      out.push_back(',');
    }
    out += std::to_string(ids[i].value());
  }
  return out;
}

}  // namespace

std::uint32_t BackendRegistry::find_model(BackendId id) const noexcept {
  for (std::uint32_t i = 0; i < model_incarnations.size(); ++i) {
    if (model_incarnations[i].backend_id == id) {
      return i;
    }
  }
  return kInvalidIndex;
}

std::uint32_t BackendRegistry::find_tool(BackendId id) const noexcept {
  for (std::uint32_t i = 0; i < tool_incarnations.size(); ++i) {
    if (tool_incarnations[i].backend_id == id) {
      return i;
    }
  }
  return kInvalidIndex;
}

bool BackendRegistry::add_model(BackendIncarnation incarnation,
                                std::shared_ptr<ModelBackend> backend) {
  if (!incarnation.valid()) {
    return false;
  }
  const std::uint32_t existing = find_model(incarnation.backend_id);
  if (existing != kInvalidIndex) {
    // Re-registering the same backend with a fresh generation is a legitimate
    // reincarnation. Re-registering a lower or equal generation is not.
    if (incarnation.generation <= model_incarnations[existing].generation) {
      return false;
    }
    model_incarnations[existing] = std::move(incarnation);
    model_backends[existing] = std::move(backend);
    return true;
  }
  model_incarnations.push_back(std::move(incarnation));
  model_backends.push_back(std::move(backend));
  return true;
}

bool BackendRegistry::add_tool(BackendIncarnation incarnation,
                               std::shared_ptr<ToolBackend> backend) {
  if (!incarnation.valid()) {
    return false;
  }
  const std::uint32_t existing = find_tool(incarnation.backend_id);
  if (existing != kInvalidIndex) {
    if (incarnation.generation <= tool_incarnations[existing].generation) {
      return false;
    }
    tool_incarnations[existing] = std::move(incarnation);
    tool_backends[existing] = std::move(backend);
    return true;
  }
  tool_incarnations.push_back(std::move(incarnation));
  tool_backends.push_back(std::move(backend));
  return true;
}

void BackendRegistry::clear() {
  model_incarnations.clear();
  model_backends.clear();
  tool_incarnations.clear();
  tool_backends.clear();
}

MutationResult reject(OutcomeCode code, std::string subject, std::string message) {
  Explanation explanation(code, std::move(subject));
  explanation.set_message(std::move(message));
  return MutationResult(code, std::move(explanation));
}

MutationResult accepted(std::string subject, std::string message,
                        std::vector<ExplanationFactor> factors) {
  ExplanationBuilder builder(OutcomeCode::ACCEPTED, std::move(subject));
  for (ExplanationFactor& factor : factors) {
    builder.add(std::move(factor.key), std::move(factor.value));
  }
  builder.set_message(std::move(message));
  return MutationResult(OutcomeCode::ACCEPTED, builder.build());
}

MutationResult no_change(std::string subject, std::string message) {
  Explanation explanation(OutcomeCode::NO_CHANGE, std::move(subject));
  explanation.set_message(std::move(message));
  return MutationResult(OutcomeCode::NO_CHANGE, std::move(explanation));
}

std::string action_subject(ActionId id, ActionGeneration generation) {
  return "action/" + std::to_string(id.value()) + "/" + std::to_string(generation.value());
}

std::uint32_t find_action(const CanonicalState& state, ActionId id) noexcept {
  for (std::uint32_t i = 0; i < state.actions.size(); ++i) {
    if (state.actions[i].id == id) {
      return i;
    }
  }
  return kInvalidIndex;
}

bool is_runtime_boot_fenced(const CanonicalState& state, RuntimeBootId runtime_boot_id) noexcept {
  for (const FencedBootRecord& record : state.fenced_boots) {
    if (record.runtime_boot_id == runtime_boot_id) {
      return true;
    }
  }
  return false;
}

bool is_fenced(const CanonicalState& state, RuntimeBootId runtime_boot_id,
               AgentBootId agent_boot_id) noexcept {
  for (const FencedBootRecord& record : state.fenced_boots) {
    if (record.runtime_boot_id == runtime_boot_id && record.agent_boot_id == agent_boot_id) {
      return true;
    }
  }
  return false;
}

std::uint32_t count_in_flight_attempts(const CanonicalState& state) noexcept {
  std::uint32_t count = 0;
  for (const AttemptRecord& attempt : state.attempts) {
    if (is_in_flight(attempt.state)) {
      ++count;
    }
  }
  return count;
}

std::uint32_t count_active_actions(const CanonicalState& state) noexcept {
  std::uint32_t count = 0;
  for (const ActionRecord& action : state.actions) {
    switch (action.state) {
      case ActionState::ADMITTED:
      case ActionState::AUTHORIZED:
      case ActionState::DISPATCHED:
      case ActionState::IN_FLIGHT:
      case ActionState::COMPLETED_UNVALIDATED:
      case ActionState::RETRY_PENDING:
        ++count;
        break;
      default:
        break;
    }
  }
  return count;
}

bool run_is_terminal(const CanonicalState& state) noexcept {
  if (state.current_run_index == kInvalidIndex) {
    return false;
  }
  return state.runs[state.current_run_index].terminal;
}

void record_provenance(CanonicalState& state, const Clock& clock, std::string origin,
                       std::string detail) {
  Provenance provenance;
  provenance.origin = std::move(origin);
  provenance.detail = std::move(detail);
  provenance.recorded_at_unix_millis = clock.now_unix_millis();
  state.provenance_log.push_back(std::move(provenance));
}

}  // namespace agent_runtime::detail

namespace agent_runtime {

using detail::ActionRecord;
using detail::AttemptRecord;
using detail::CanonicalState;
using detail::kInvalidIndex;

AgentRuntime::Impl::Impl() : Impl(AgentRuntimeOptions{}) {}

AgentRuntime::Impl::Impl(AgentRuntimeOptions options_value) : options(std::move(options_value)) {
  limits = options.limits;
  clock = options.clock != nullptr ? options.clock : std::make_shared<SystemClock>();

  state.runtime_id = options.runtime_id.valid() ? options.runtime_id
                                                : allocate_id<AgentRuntimeTag>();
  state.runtime_generation = options.runtime_generation.valid() ? options.runtime_generation
                                                                : AgentRuntimeGeneration(1);
  state.runtime_boot_id = options.runtime_boot_id.valid() ? options.runtime_boot_id
                                                          : RuntimeBootId(1);
  state.agent_id = options.agent_id.valid() ? options.agent_id : allocate_id<AgentTag>();
  state.agent_generation = options.agent_generation.valid() ? options.agent_generation
                                                            : AgentGeneration(1);
  state.agent_boot_id = options.agent_boot_id.valid() ? options.agent_boot_id : AgentBootId(1);
  state.coordinator_epoch = options.coordinator_epoch.valid() ? options.coordinator_epoch
                                                              : CoordinatorEpoch(1);
  state.runtime_epoch = options.runtime_epoch.valid() ? options.runtime_epoch : RuntimeEpoch(1);
  state.policy = options.policy;
  state.policy_history.push_back(state.policy);
  state.lifecycle = RuntimeLifecycle::DECLARED;

  if (options.model_backend != nullptr) {
    BackendIncarnation incarnation = options.model_backend->incarnation();
    if (incarnation.valid()) {
      (void)backends.add_model(std::move(incarnation), options.model_backend);
    }
  }
  if (options.tool_backend != nullptr) {
    BackendIncarnation incarnation = options.tool_backend->incarnation();
    if (incarnation.valid()) {
      (void)backends.add_tool(std::move(incarnation), options.tool_backend);
    }
  }
  reindex();
}

void AgentRuntime::Impl::recount_locked() {
  active_actions_ = 0;
  in_flight_attempts_ = 0;
  in_flight_tool_attempts_ = 0;
  in_flight_model_attempts_ = 0;
  in_flight_effect_free_attempts_ = 0;
  for (const detail::ActionRecord& action : state.actions) {
    switch (action.state) {
      case ActionState::ADMITTED:
      case ActionState::AUTHORIZED:
      case ActionState::DISPATCHED:
      case ActionState::IN_FLIGHT:
      case ActionState::COMPLETED_UNVALIDATED:
      case ActionState::RETRY_PENDING:
        ++active_actions_;
        break;
      default:
        break;
    }
  }
  for (const detail::AttemptRecord& attempt : state.attempts) {
    if (!is_in_flight(attempt.state)) {
      continue;
    }
    ++in_flight_attempts_;
    const auto found = indexes.action_by_id.find(attempt.action_id);
    const std::uint32_t index = found == indexes.action_by_id.end() ? detail::kInvalidIndex : found->second;
    if (index == detail::kInvalidIndex) {
      continue;
    }
    const detail::ActionRecord& action = state.actions[index];
    if (action.kind == ActionKind::TOOL_CALL) {
      ++in_flight_tool_attempts_;
    } else if (action.kind == ActionKind::MODEL_CALL) {
      ++in_flight_model_attempts_;
    }
    if (is_effect_free(action.side_effect)) {
      ++in_flight_effect_free_attempts_;
    }
  }
}

void AgentRuntime::Impl::set_action_state_locked(ActionRecord& action, ActionState next) {
  if (action.state == next) {
    return;
  }
  const bool was_active = action.state == ActionState::ADMITTED ||
                          action.state == ActionState::AUTHORIZED ||
                          action.state == ActionState::DISPATCHED ||
                          action.state == ActionState::IN_FLIGHT ||
                          action.state == ActionState::COMPLETED_UNVALIDATED ||
                          action.state == ActionState::RETRY_PENDING;
  const bool now_active = next == ActionState::ADMITTED || next == ActionState::AUTHORIZED ||
                          next == ActionState::DISPATCHED || next == ActionState::IN_FLIGHT ||
                          next == ActionState::COMPLETED_UNVALIDATED ||
                          next == ActionState::RETRY_PENDING;
  if (was_active != now_active) {
    active_actions_ += now_active ? 1u : 0u;
    active_actions_ -= now_active ? 0u : 1u;
  }
  action.state = next;
  index_dirty_ = true;
}

void AgentRuntime::Impl::set_attempt_state_locked(AttemptRecord& attempt, ActionState next) {
  if (attempt.state == next) {
    return;
  }
  // The counter tracks attempt states only: every transition that sets superseded also moves the attempt out of an in-flight state.
  const bool was_in_flight = is_in_flight(attempt.state);
  const bool now_in_flight = is_in_flight(next);
  if (was_in_flight != now_in_flight) {
    if (now_in_flight) {
      ++in_flight_attempts_;
      const auto found = indexes.action_by_id.find(attempt.action_id);
      const std::uint32_t index = found == indexes.action_by_id.end() ? detail::kInvalidIndex : found->second;
      if (index != detail::kInvalidIndex) {
        const detail::ActionRecord& action = state.actions[index];
        if (action.kind == ActionKind::TOOL_CALL) {
          ++in_flight_tool_attempts_;
        } else if (action.kind == ActionKind::MODEL_CALL) {
          ++in_flight_model_attempts_;
        }
        if (is_effect_free(action.side_effect)) {
          ++in_flight_effect_free_attempts_;
        }
      }
    } else {
      if (in_flight_attempts_ > 0) {
        --in_flight_attempts_;
      }
      const auto found = indexes.action_by_id.find(attempt.action_id);
      const std::uint32_t index = found == indexes.action_by_id.end() ? detail::kInvalidIndex : found->second;
      if (index != detail::kInvalidIndex) {
        const detail::ActionRecord& action = state.actions[index];
        if (action.kind == ActionKind::TOOL_CALL && in_flight_tool_attempts_ > 0) {
          --in_flight_tool_attempts_;
        } else if (action.kind == ActionKind::MODEL_CALL && in_flight_model_attempts_ > 0) {
          --in_flight_model_attempts_;
        }
        if (is_effect_free(action.side_effect) && in_flight_effect_free_attempts_ > 0) {
          --in_flight_effect_free_attempts_;
        }
      }
    }
  }
  attempt.state = next;
  index_dirty_ = true;
}
MutationResult AgentRuntime::Impl::finish(MutationResult result) {
  return result;
}

MutationResult AgentRuntime::Impl::finish_locked(MutationResult result) {
  if (is_acceptance(result.code)) {
    bump();
    mark_dirty();
  }
  return result;
}

ActionRecord* AgentRuntime::Impl::action_locked(ActionId id) noexcept {
  const auto found = indexes.action_by_id.find(id);
  if (found == indexes.action_by_id.end()) {
    return nullptr;
  }
  return &state.actions[found->second];
}

bool AgentRuntime::Impl::check_runtime_authority_locked(std::string& message) const {
  if (state.lifecycle == RuntimeLifecycle::RETIRED) {
    message = "runtime is retired";
    return false;
  }
  if (state.lifecycle == RuntimeLifecycle::CANCELLED ||
      state.lifecycle == RuntimeLifecycle::CANCELLING) {
    message = "runtime is cancelled";
    return false;
  }
  if (state.lifecycle == RuntimeLifecycle::COMPLETED) {
    message = "runtime is completed";
    return false;
  }
  if (detail::is_runtime_boot_fenced(state, state.runtime_boot_id)) {
    message = "runtime boot " + std::to_string(state.runtime_boot_id.value()) + " is fenced";
    return false;
  }
  return true;
}

bool AgentRuntime::Impl::assignment_current_locked(std::string& message) const {
  if (!state.policy.require_assignment_for_dispatch) {
    return true;
  }
  if (!state.has_assignment) {
    message = "no scheduler assignment bound";
    return false;
  }
  const SchedulerAuthority& authority = state.current_assignment;
  if (!authority.current || !authority.complete()) {
    message = "scheduler assignment is not current";
    return false;
  }
  if (authority.agent_id != state.agent_id) {
    message = "scheduler assignment belongs to a different agent";
    return false;
  }
  if (authority.agent_generation != state.agent_generation) {
    message = "scheduler assignment agent generation is stale";
    return false;
  }
  if (authority.coordinator_epoch != state.coordinator_epoch) {
    message = "scheduler assignment coordinator epoch is stale";
    return false;
  }
  return true;
}

bool AgentRuntime::Impl::policy_allows_locked(const ActionRecord& action,
                                              std::string& message) const {
  const RuntimePolicy& policy = state.policy;
  if (!policy.generation.valid()) {
    message = "policy has no generation";
    return false;
  }
  if (policy.allowed_action_kinds.find(action.kind) == policy.allowed_action_kinds.end()) {
    message = std::string("policy does not allow action kind ") +
              std::string(to_string(action.kind));
    return false;
  }
  if (!policy.allow_side_effects && !is_effect_free(action.side_effect) &&
      action.side_effect != SideEffectClass::IDEMPOTENT &&
      action.side_effect != SideEffectClass::DEDUPLICATABLE) {
    message = "policy forbids side effects";
    return false;
  }
  if (!policy.allow_non_repeatable_side_effects &&
      (action.side_effect == SideEffectClass::NON_REPEATABLE ||
       action.side_effect == SideEffectClass::COMMIT_TOKEN_REQUIRED)) {
    message = "policy forbids non-repeatable side effects";
    return false;
  }
  if (!policy.allow_unknown_side_effects && action.side_effect == SideEffectClass::UNKNOWN) {
    message = "policy forbids unknown side-effect classes";
    return false;
  }
  if (action.kind == ActionKind::TOOL_CALL) {
    if (action.tool_name.empty()) {
      message = "tool call requires a tool name";
      return false;
    }
    if (!policy.allowed_tools.empty() &&
        policy.allowed_tools.find(action.tool_name) == policy.allowed_tools.end()) {
      message = "policy does not allow tool " + action.tool_name;
      return false;
    }
  }
  if (action.kind == ActionKind::MODEL_CALL) {
    if (action.model_target.empty()) {
      message = "model call requires a resolved model target";
      return false;
    }
    if (!policy.allowed_model_targets.empty() &&
        policy.allowed_model_targets.find(action.model_target) ==
            policy.allowed_model_targets.end()) {
      message = "policy does not allow model target " + action.model_target;
      return false;
    }
  }
  if (action.kind == ActionKind::CHECKPOINT && !policy.allow_checkpoint) {
    message = "policy forbids checkpoints";
    return false;
  }
  if ((action.kind == ActionKind::MEMORY_WRITE_INTENT) && !policy.allow_memory_mutation) {
    message = "policy forbids memory mutation";
    return false;
  }
  return true;
}

OutcomeCode AgentRuntime::Impl::budget_check_locked(const ActionRecord& action,
                                                    std::string& message) const {
  if (!action.budget_required && !state.policy.require_budget_for_dispatch) {
    return OutcomeCode::ACCEPTED;
  }
  if (!state.has_budget) {
    message = "no budget authority bound";
    return OutcomeCode::REJECT_BUDGET;
  }
  const BudgetEvidence& evidence = state.budget;
  if (action.budget_id.valid() && evidence.budget_id != action.budget_id) {
    message = "bound budget identity does not match the action budget identity";
    return OutcomeCode::REJECT_STALE_BUDGET;
  }
  if (action.budget_generation.valid() && evidence.generation != action.budget_generation) {
    message = "budget generation moved since the action was created";
    return OutcomeCode::REJECT_STALE_BUDGET;
  }
  switch (evidence.outcome) {
    case BudgetOutcome::ALLOW:
      break;
    case BudgetOutcome::DENY:
      message = "budget denied";
      return OutcomeCode::REJECT_BUDGET;
    case BudgetOutcome::DEFER:
      message = "budget deferred";
      return OutcomeCode::REJECT_BUDGET;
    case BudgetOutcome::UNKNOWN:
      message = "budget outcome is unknown; unknown is never treated as unlimited";
      return OutcomeCode::REJECT_BUDGET;
    case BudgetOutcome::kCount:
      message = "budget outcome is invalid";
      return OutcomeCode::REJECT_BUDGET;
  }
  if (evidence.limits_known) {
    if (action.kind == ActionKind::MODEL_CALL && evidence.remaining_requests == 0) {
      message = "no remaining model requests under budget authority";
      return OutcomeCode::REJECT_BUDGET;
    }
    if (action.kind == ActionKind::TOOL_CALL && evidence.remaining_tool_calls == 0) {
      message = "no remaining tool calls under budget authority";
      return OutcomeCode::REJECT_BUDGET;
    }
    if (evidence.remaining_actions == 0) {
      message = "no remaining actions under budget authority";
      return OutcomeCode::REJECT_BUDGET;
    }
  }
  return OutcomeCode::ACCEPTED;
}

bool AgentRuntime::Impl::dependencies_satisfied_locked(const ActionRecord& action,
                                                       std::string& message) const {
  for (const ActionId dependency : action.dependencies) {
    const std::uint32_t index = detail::find_action(state, dependency);
    if (index == kInvalidIndex) {
      message = "dependency " + std::to_string(dependency.value()) + " does not exist";
      return false;
    }
    const ActionRecord& other = state.actions[index];
    if (other.run_id != action.run_id || other.run_generation != action.run_generation) {
      message = "dependency " + std::to_string(dependency.value()) +
                " belongs to a different run generation";
      return false;
    }
    if (!other.committed) {
      message = "dependency " + std::to_string(dependency.value()) + " is not committed";
      return false;
    }
  }
  return true;
}

bool AgentRuntime::Impl::side_effect_allowed_locked(const ActionRecord& action,
                                                    std::string& message) const {
  if (action.side_effect == SideEffectClass::UNKNOWN && !state.policy.allow_unknown_side_effects) {
    message = "unknown side-effect class is not permitted by policy";
    return false;
  }
  if (supports_operation_key(action.side_effect) && action.operation_key.empty()) {
    message = std::string("side-effect class ") + std::string(to_string(action.side_effect)) +
              " requires a stable operation key";
    return false;
  }
  return true;
}

void AgentRuntime::Impl::advance_progress_locked(ActionRecord& action, CommitGeneration generation) {
  action.committed = true;
  action.commit_generation = generation;
  action.commit_sequence = state.next_commit_sequence++;
  set_action_state_locked(action, ActionState::COMMITTED);
  ++state.committed_actions;
  state.progress_generation = state.progress_generation.next();

  if (state.current_run_index != kInvalidIndex) {
    detail::RunRecord& run = state.runs[state.current_run_index];
    ++run.committed_actions;
    run.progress_generation = state.progress_generation;
  }
  if (state.current_step_index != kInvalidIndex) {
    detail::StepRecord& step = state.steps[state.current_step_index];
    ++step.committed_actions;
    step.progress_generation = state.progress_generation;
    step.progress = ProgressState::COMMITTED;
  }
}

void AgentRuntime::Impl::classify_unresolved_locked(ActionRecord& action, AttemptRecord& attempt,
                                                    std::string reason) {
  attempt.superseded = true;
  attempt.current = false;
  action.current_attempt = ActionAttemptId{};
  action.current_attempt_generation = AttemptGeneration{};
  if (requires_reconciliation_on_ambiguity(action.side_effect)) {
    set_attempt_state_locked(attempt, ActionState::REVALIDATION_REQUIRED);
    attempt.completion_status = CompletionStatus::AMBIGUOUS;
    attempt.ambiguous = true;
    attempt.error = std::move(reason);
    set_action_state_locked(action, ActionState::REVALIDATION_REQUIRED);
    action.ambiguous = true;
    action.completion_status = CompletionStatus::AMBIGUOUS;
    action.last_failure = RetryClass::AMBIGUOUS_COMPLETION;
    action.last_error = attempt.error;
    return;
  }
  set_attempt_state_locked(attempt, ActionState::FAILED);
  attempt.completion_status = CompletionStatus::FAILED;
  attempt.failure_class = RetryClass::TRANSIENT;
  attempt.error = std::move(reason);
  set_action_state_locked(action, ActionState::RETRY_PENDING);
  action.last_failure = RetryClass::TRANSIENT;
  action.last_error = attempt.error;
}

MutationResult AgentRuntime::Impl::mark_retry_pending_locked(ActionRecord& action,
                                                             AttemptRecord& attempt,
                                                             RetryClass failure_class,
                                                             std::string message) {
  const RetryPolicy& policy = action.has_retry_override ? action.retry : state.policy.retry;
  const std::uint32_t attempt_limit =
      std::min<std::uint32_t>(policy.max_attempts, state.policy.max_retries_per_action + 1);

  attempt.superseded = true;
  attempt.current = false;
  attempt.failure_class = failure_class;
  attempt.error = message;
  set_attempt_state_locked(attempt, ActionState::FAILED);
  attempt.completion_status = CompletionStatus::FAILED;
  action.current_attempt = ActionAttemptId{};
  action.current_attempt_generation = AttemptGeneration{};
  action.last_failure = failure_class;
  action.last_error = message;

  if (failure_class == RetryClass::AMBIGUOUS_COMPLETION ||
      requires_reconciliation_on_ambiguity(action.side_effect) && attempt.ambiguous) {
    set_action_state_locked(action, ActionState::REVALIDATION_REQUIRED);
    action.ambiguous = true;
    action.completion_status = CompletionStatus::AMBIGUOUS;
    return detail::reject(OutcomeCode::AMBIGUOUS_COMPLETION,
                          detail::action_subject(action.id, action.generation),
                          "completion of a " +
                              std::string(to_string(action.side_effect)) +
                              " action is ambiguous; external reconciliation is required");
  }

  if (!policy.allows(failure_class)) {
    set_action_state_locked(action, ActionState::FAILED);
    action.completion_status = CompletionStatus::FAILED;
    return detail::reject(OutcomeCode::REJECT_INVALID,
                          detail::action_subject(action.id, action.generation),
                          std::string("failure class ") + std::string(to_string(failure_class)) +
                              " is not retryable under the effective retry policy");
  }
  if (action.attempt_count >= attempt_limit) {
    set_action_state_locked(action, ActionState::FAILED);
    action.completion_status = CompletionStatus::FAILED;
    return detail::reject(OutcomeCode::REJECT_RETRY_EXHAUSTED,
                          detail::action_subject(action.id, action.generation),
                          "retry budget exhausted after " + std::to_string(action.attempt_count) +
                              " attempts");
  }
  if (policy.require_side_effect_safety &&
      !supports_operation_key(action.side_effect) &&
      !is_effect_free(action.side_effect)) {
    set_action_state_locked(action, ActionState::FAILED);
    action.completion_status = CompletionStatus::FAILED;
    return detail::reject(OutcomeCode::REJECT_SIDE_EFFECT_UNSAFE,
                          detail::action_subject(action.id, action.generation),
                          std::string("side-effect class ") +
                              std::string(to_string(action.side_effect)) +
                              " is not safely repeatable");
  }
  if (policy.require_budget_reenval && action.budget_required) {
    std::string budget_message;
    const OutcomeCode budget_code = budget_check_locked(action, budget_message);
    if (budget_code != OutcomeCode::ACCEPTED) {
      set_action_state_locked(action, ActionState::RETRY_PENDING);
      action.last_error = budget_message;
      return detail::reject(budget_code, detail::action_subject(action.id, action.generation),
                            budget_message);
    }
  }
  if (policy.require_deadline_gate && policy.deadline_unix_millis != 0 && now_unix() > 0 &&
      static_cast<std::uint64_t>(now_unix()) > policy.deadline_unix_millis) {
    set_action_state_locked(action, ActionState::FAILED);
    return detail::reject(OutcomeCode::REJECT_INVALID,
                          detail::action_subject(action.id, action.generation),
                          "deadline gate rejected the retry");
  }
  if (policy.checkpoint_before_retry && action.requires_checkpoint && !action.checkpoint_generation.valid()) {
    set_action_state_locked(action, ActionState::REVALIDATION_REQUIRED);
    return detail::reject(OutcomeCode::REVALIDATION_REQUIRED,
                          detail::action_subject(action.id, action.generation),
                          "policy requires a checkpoint before retry and none is bound");
  }

  set_action_state_locked(action, ActionState::RETRY_PENDING);
  action.completion_status = CompletionStatus::FAILED;
  return detail::accepted(detail::action_subject(action.id, action.generation),
                          "retry pending with a fresh attempt generation",
                          {ExplanationFactor{"failure_class", std::string(to_string(failure_class))},
                           ExplanationFactor{"attempt_count", std::to_string(action.attempt_count)}});
}

// ---------------------------------------------------------------------------
// Construction / identity
// ---------------------------------------------------------------------------

AgentRuntime::AgentRuntime(AgentRuntimeOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

AgentRuntime::~AgentRuntime() {
  // Shutdown is deterministic: signal cancellable work, then release state.
  impl_->inflight_cancellation->store(true, std::memory_order_release);
  impl_->backends.clear();
}

AgentRuntimeId AgentRuntime::id() const noexcept { return impl_->state.runtime_id; }

AgentRuntimeGeneration AgentRuntime::generation() const noexcept {
  return impl_->state.runtime_generation;
}

RuntimeBootId AgentRuntime::boot_id() const noexcept { return impl_->state.runtime_boot_id; }

RuntimeEpoch AgentRuntime::runtime_epoch() const noexcept { return impl_->state.runtime_epoch; }

CoordinatorEpoch AgentRuntime::coordinator_epoch() const noexcept {
  return impl_->state.coordinator_epoch;
}

RuntimeLifecycle AgentRuntime::lifecycle() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->state.lifecycle;
}

void AgentRuntime::request_inflight_cancellation() {
  impl_->inflight_cancellation->store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Lifecycle and authority
// ---------------------------------------------------------------------------

MutationResult AgentRuntime::initialize() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  CanonicalState& state = impl_->state;
  const std::string subject = "runtime/" + std::to_string(state.runtime_id.value());

  if (state.lifecycle != RuntimeLifecycle::DECLARED) {
    if (state.lifecycle == RuntimeLifecycle::READY || state.lifecycle == RuntimeLifecycle::RUNNING ||
        is_active(state.lifecycle)) {
      return detail::no_change(subject, "runtime is already initialized");
    }
    return detail::reject(OutcomeCode::REJECT_NOT_READY, subject,
                          std::string("runtime cannot initialize from lifecycle ") +
                              std::string(to_string(state.lifecycle)));
  }

  const std::string limits_error = impl_->limits.validate();
  if (!limits_error.empty()) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject,
                          "resource limits are invalid: " + limits_error);
  }
  const std::string policy_error = state.policy.validate();
  if (!policy_error.empty()) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject,
                          "runtime policy is invalid: " + policy_error);
  }
  if (!state.runtime_id.valid() || !state.runtime_generation.valid() ||
      !state.runtime_boot_id.valid() || !state.agent_id.valid() ||
      !state.agent_generation.valid() || !state.agent_boot_id.valid()) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject,
                          "runtime identity is incomplete");
  }

  state.lifecycle = RuntimeLifecycle::INITIALIZING;
  detail::record_provenance(state, *impl_->clock, "initialize", "runtime initialized");
  state.lifecycle = RuntimeLifecycle::READY;
  return impl_->finish_locked(
      detail::accepted(subject, "runtime initialized",
                       {ExplanationFactor{"lifecycle", std::string(to_string(state.lifecycle))}}));
}

MutationResult AgentRuntime::bind_assignment(SchedulerAuthority authority) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  CanonicalState& state = impl_->state;
  const std::string subject = "runtime/" + std::to_string(state.runtime_id.value());

  std::string authority_message;
  if (!impl_->check_runtime_authority_locked(authority_message)) {
    return detail::reject(OutcomeCode::REJECT_CANCELLED, subject, authority_message);
  }
  if (!authority.complete()) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject,
                          "scheduler assignment authority is incomplete");
  }
  if (authority.agent_id != state.agent_id) {
    return detail::reject(OutcomeCode::REJECT_STALE_ASSIGNMENT, subject,
                          "assignment targets a different agent identity");
  }
  if (authority.agent_generation < state.agent_generation) {
    return detail::reject(OutcomeCode::REJECT_STALE_ASSIGNMENT, subject,
                          "assignment agent generation is older than the runtime agent generation");
  }
  if (state.has_assignment) {
    const SchedulerAuthority& current = state.current_assignment;
    if (authority.assignment_id == current.assignment_id &&
        authority.assignment_generation == current.assignment_generation &&
        authority.work_id == current.work_id &&
        authority.work_generation == current.work_generation &&
        authority.coordinator_epoch == current.coordinator_epoch &&
        authority.lease_generation == current.lease_generation) {
      return detail::no_change(subject, "assignment is already bound");
    }
    if (authority.assignment_id == current.assignment_id &&
        authority.assignment_generation < current.assignment_generation) {
      return detail::reject(OutcomeCode::REJECT_STALE_ASSIGNMENT, subject,
                            "assignment generation is older than the bound assignment");
    }
    if (authority.assignment_id == current.assignment_id &&
        authority.work_id == current.work_id &&
        authority.work_generation < current.work_generation) {
      return detail::reject(OutcomeCode::REJECT_STALE_ASSIGNMENT, subject,
                            "work generation is older than the bound work generation");
    }
    if (authority.coordinator_epoch < state.coordinator_epoch) {
      return detail::reject(OutcomeCode::REJECT_STALE_RUNTIME_EPOCH, subject,
                            "assignment coordinator epoch is older than the runtime epoch");
    }
  }
  if (authority.coordinator_epoch < state.coordinator_epoch) {
    return detail::reject(OutcomeCode::REJECT_STALE_RUNTIME_EPOCH, subject,
                          "assignment coordinator epoch is older than the runtime epoch");
  }
  if (authority.scheduler_epoch.valid() && authority.scheduler_epoch.value() == 0) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject, "scheduler epoch is invalid");
  }

  for (detail::AssignmentRecord& record : state.assignments) {
    record.current = false;
  }
  if (state.assignments.size() >= impl_->limits.max_assignment_history) {
    state.assignments.erase(state.assignments.begin());
  }
  detail::AssignmentRecord record;
  record.authority = authority;
  record.authority.current = true;
  record.current = true;
  record.reason = "bound";
  record.recorded_at_unix_millis = impl_->now_unix();
  state.assignments.push_back(std::move(record));
  state.current_assignment = authority;
  state.current_assignment.current = true;
  state.has_assignment = true;

  detail::record_provenance(state, *impl_->clock, "bind_assignment",
                            "assignment " + std::to_string(authority.assignment_id.value()));

  // Actions that were waiting on assignment authority become revalidatable.
  for (ActionRecord& action : state.actions) {
    if (action.state == ActionState::REVALIDATION_REQUIRED && !action.ambiguous) {
      impl_->set_action_state_locked(action, ActionState::ADMITTED);
    }
  }

  return impl_->finish_locked(detail::accepted(
      subject, "scheduler assignment bound",
      {ExplanationFactor{"assignment_id", std::to_string(authority.assignment_id.value())},
       ExplanationFactor{"assignment_generation",
                         std::to_string(authority.assignment_generation.value())},
       ExplanationFactor{"work_id", std::to_string(authority.work_id.value())},
       ExplanationFactor{"work_generation", std::to_string(authority.work_generation.value())}}));
}

MutationResult AgentRuntime::revalidate_assignment() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  CanonicalState& state = impl_->state;
  const std::string subject = "runtime/" + std::to_string(state.runtime_id.value());
  if (impl_->options.scheduler_authority == nullptr) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject,
                          "no scheduler authority provider is configured");
  }
  std::optional<SchedulerAuthority> fresh =
      impl_->options.scheduler_authority->current_assignment(state.runtime_id);
  if (!fresh.has_value()) {
    if (state.has_assignment) {
      state.current_assignment.current = false;
      for (detail::AssignmentRecord& record : state.assignments) {
        record.current = false;
      }
      state.has_assignment = false;
    }
    return detail::reject(OutcomeCode::REJECT_STALE_ASSIGNMENT, subject,
                          "scheduler reports no current assignment");
  }
  const SchedulerAuthority supplied = *fresh;
  // Unlock-free re-entry: bind_assignment acquires the same mutex, so the logic
  // is inlined here rather than called recursively.
  if (!supplied.complete()) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject,
                          "scheduler returned an incomplete assignment");
  }
  if (state.has_assignment) {
    const SchedulerAuthority& current = state.current_assignment;
    if (supplied.assignment_id == current.assignment_id &&
        supplied.assignment_generation < current.assignment_generation) {
      return detail::reject(OutcomeCode::REJECT_STALE_ASSIGNMENT, subject,
                            "scheduler returned a superseded assignment generation");
    }
    if (supplied.work_generation < current.work_generation &&
        supplied.work_id == current.work_id) {
      return detail::reject(OutcomeCode::REJECT_STALE_ASSIGNMENT, subject,
                            "scheduler returned a superseded work generation");
    }
  }
  if (supplied.coordinator_epoch < state.coordinator_epoch) {
    return detail::reject(OutcomeCode::REJECT_STALE_RUNTIME_EPOCH, subject,
                          "scheduler assignment coordinator epoch is stale");
  }
  for (detail::AssignmentRecord& record : state.assignments) {
    record.current = false;
  }
  if (state.assignments.size() >= impl_->limits.max_assignment_history) {
    state.assignments.erase(state.assignments.begin());
  }
  detail::AssignmentRecord record;
  record.authority = supplied;
  record.authority.current = true;
  record.current = true;
  record.reason = "revalidated";
  record.recorded_at_unix_millis = impl_->now_unix();
  state.assignments.push_back(std::move(record));
  state.current_assignment = supplied;
  state.current_assignment.current = true;
  state.has_assignment = true;
  return impl_->finish_locked(detail::accepted(subject, "assignment revalidated"));
}

MutationResult AgentRuntime::set_policy(RuntimePolicy policy) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  CanonicalState& state = impl_->state;
  const std::string subject = "runtime/" + std::to_string(state.runtime_id.value());

  const std::string error = policy.validate();
  if (!error.empty()) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject, "policy is invalid: " + error);
  }
  if (!state.policy.generation.valid() || policy.generation < state.policy.generation) {
    return detail::reject(OutcomeCode::REJECT_STALE_POLICY, subject,
                          "policy generation is older than the current policy generation");
  }
  if (policy.generation == state.policy.generation && policy.policy_id == state.policy.policy_id) {
    return detail::no_change(subject, "policy generation already installed");
  }
  state.policy = policy;
  if (state.policy_history.size() >= impl_->limits.max_policy_records) {
    state.policy_history.erase(state.policy_history.begin());
  }
  state.policy_history.push_back(policy);

  // Pending actions created under the previous policy generation must
  // revalidate before dispatch or commit.
  for (ActionRecord& action : state.actions) {
    if (action.policy_generation != policy.generation &&
        (action.state == ActionState::ADMITTED || action.state == ActionState::AUTHORIZED ||
         action.state == ActionState::RETRY_PENDING)) {
      impl_->set_action_state_locked(action, ActionState::REVALIDATION_REQUIRED);
    }
  }
  return impl_->finish_locked(detail::accepted(
      subject, "policy installed",
      {ExplanationFactor{"policy_generation", std::to_string(policy.generation.value())}}));
}

MutationResult AgentRuntime::set_budget(BudgetEvidence evidence) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  CanonicalState& state = impl_->state;
  const std::string subject = "runtime/" + std::to_string(state.runtime_id.value());

  if (!evidence.budget_id.valid() || !evidence.generation.valid()) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject,
                          "budget evidence has no identity or generation");
  }
  if (state.has_budget) {
    if (evidence.budget_id == state.budget.budget_id &&
        evidence.generation < state.budget.generation) {
      return detail::reject(OutcomeCode::REJECT_STALE_BUDGET, subject,
                            "budget generation is older than the current budget generation");
    }
    if (evidence.generation == state.budget.generation &&
        evidence.outcome == state.budget.outcome) {
      return detail::no_change(subject, "budget generation already installed");
    }
  }
  state.budget = evidence;
  state.has_budget = true;
  if (state.budget_history.size() >= impl_->limits.max_budget_records) {
    state.budget_history.erase(state.budget_history.begin());
  }
  state.budget_history.push_back(evidence);

  return impl_->finish_locked(detail::accepted(
      subject, "budget evidence installed",
      {ExplanationFactor{"budget_generation", std::to_string(evidence.generation.value())},
       ExplanationFactor{"budget_outcome", std::string(to_string(evidence.outcome))}}));
}

MutationResult AgentRuntime::register_model_backend(BackendIncarnation incarnation,
                                                    std::shared_ptr<ModelBackend> backend) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const std::string subject = "backend/" + std::to_string(incarnation.backend_id.value());
  if (backend == nullptr) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject, "model backend is null");
  }
  if (!incarnation.valid()) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject,
                          "model backend incarnation is incomplete");
  }
  const std::uint32_t existing_model = impl_->backends.find_model(incarnation.backend_id);
  if (existing_model != kInvalidIndex &&
      impl_->backends.model_incarnations[existing_model].generation == incarnation.generation &&
      impl_->backends.model_backends[existing_model] == backend) {
    return detail::no_change(subject, "model backend incarnation already registered");
  }
  if (!impl_->backends.add_model(incarnation, std::move(backend))) {
    return detail::reject(OutcomeCode::REJECT_STALE_MODEL_BINDING, subject,
                          "model backend incarnation is not newer than the registered one");
  }
  return impl_->finish_locked(detail::accepted(
      subject, "model backend registered",
      {ExplanationFactor{"backend_generation", std::to_string(incarnation.generation.value())}}));
}

MutationResult AgentRuntime::register_tool_backend(BackendIncarnation incarnation,
                                                   std::shared_ptr<ToolBackend> backend) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const std::string subject = "backend/" + std::to_string(incarnation.backend_id.value());
  if (backend == nullptr) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject, "tool backend is null");
  }
  if (!incarnation.valid()) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject,
                          "tool backend incarnation is incomplete");
  }
  const std::uint32_t existing_tool = impl_->backends.find_tool(incarnation.backend_id);
  if (existing_tool != kInvalidIndex &&
      impl_->backends.tool_incarnations[existing_tool].generation == incarnation.generation &&
      impl_->backends.tool_backends[existing_tool] == backend) {
    return detail::no_change(subject, "tool backend incarnation already registered");
  }
  if (!impl_->backends.add_tool(incarnation, std::move(backend))) {
    return detail::reject(OutcomeCode::REJECT_STALE_TOOL_BINDING, subject,
                          "tool backend incarnation is not newer than the registered one");
  }
  return impl_->finish_locked(detail::accepted(
      subject, "tool backend registered",
      {ExplanationFactor{"backend_generation", std::to_string(incarnation.generation.value())}}));
}

MutationResult AgentRuntime::register_remote_backend(BackendIncarnation incarnation) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const std::string subject = "backend/" + std::to_string(incarnation.backend_id.value());
  if (!incarnation.valid()) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject,
                          "remote backend incarnation is incomplete");
  }
  const std::uint32_t existing_tool = impl_->backends.find_tool(incarnation.backend_id);
  if (existing_tool != kInvalidIndex) {
    if (impl_->backends.tool_incarnations[existing_tool].generation == incarnation.generation) {
      return detail::no_change(subject, "remote tool backend incarnation already registered");
    }
    if (impl_->backends.tool_incarnations[existing_tool].generation > incarnation.generation) {
      return detail::reject(OutcomeCode::REJECT_STALE_TOOL_BINDING, subject,
                            "tool backend incarnation is not newer than the registered one");
    }
  }
  const std::uint32_t existing_model = impl_->backends.find_model(incarnation.backend_id);
  if (existing_model != kInvalidIndex) {
    if (impl_->backends.model_incarnations[existing_model].generation == incarnation.generation) {
      return detail::no_change(subject, "remote model backend incarnation already registered");
    }
    if (impl_->backends.model_incarnations[existing_model].generation > incarnation.generation) {
      return detail::reject(OutcomeCode::REJECT_STALE_MODEL_BINDING, subject,
                            "model backend incarnation is not newer than the registered one");
    }
  }
  if (!impl_->backends.add_tool(incarnation, nullptr)) {
    return detail::reject(OutcomeCode::REJECT_STALE_TOOL_BINDING, subject,
                          "remote backend incarnation could not be registered");
  }
  (void)impl_->backends.add_model(incarnation, nullptr);
  return impl_->finish_locked(detail::accepted(
      subject, "remote backend incarnation registered (identity only)",
      {ExplanationFactor{"backend_generation", std::to_string(incarnation.generation.value())}}));
}

MutationResult AgentRuntime::bind_model(ModelBinding binding) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  CanonicalState& state = impl_->state;
  const std::string subject = "model/" + binding.target;
  if (binding.target.empty() || !binding.binding_generation.valid()) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject,
                          "model binding is incomplete");
  }
  if (binding.target.size() > impl_->limits.max_string_bytes) {
    return detail::reject(OutcomeCode::REJECT_LIMIT, subject, "model target exceeds string limit");
  }
  if (impl_->backends.find_model(binding.backend_id) == kInvalidIndex) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject,
                          "model backend is not registered in this process");
  }
  const std::uint32_t backend_index = impl_->backends.find_model(binding.backend_id);
  if (impl_->backends.model_incarnations[backend_index].generation != binding.backend_generation) {
    return detail::reject(OutcomeCode::REJECT_STALE_MODEL_BINDING, subject,
                          "model backend incarnation does not match the registered incarnation");
  }
  for (ModelBinding& existing : state.model_bindings) {
    if (existing.target == binding.target) {
      if (binding.binding_generation < existing.binding_generation) {
        return detail::reject(OutcomeCode::REJECT_STALE_MODEL_BINDING, subject,
                              "model binding generation is older than the bound generation");
      }
      if (binding.binding_generation == existing.binding_generation &&
          binding.backend_generation == existing.backend_generation &&
          existing.current == binding.current) {
        return detail::no_change(subject, "model binding already installed");
      }
      existing = binding;
      return impl_->finish_locked(detail::accepted(subject, "model binding updated"));
    }
  }
  state.model_bindings.push_back(binding);
  return impl_->finish_locked(detail::accepted(
      subject, "model binding installed",
      {ExplanationFactor{"binding_generation", std::to_string(binding.binding_generation.value())}}));
}

MutationResult AgentRuntime::bind_tool(ToolBinding binding) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  CanonicalState& state = impl_->state;
  const std::string subject = "tool/" + binding.tool_name;
  if (binding.tool_name.empty() || !binding.binding_generation.valid()) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject, "tool binding is incomplete");
  }
  if (binding.tool_name.size() > impl_->limits.max_string_bytes) {
    return detail::reject(OutcomeCode::REJECT_LIMIT, subject, "tool name exceeds string limit");
  }
  if (binding.side_effect >= SideEffectClass::kCount) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject,
                          "tool binding side-effect class is invalid");
  }
  if (impl_->backends.find_tool(binding.backend_id) == kInvalidIndex) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject,
                          "tool backend is not registered in this process");
  }
  const std::uint32_t backend_index = impl_->backends.find_tool(binding.backend_id);
  if (impl_->backends.tool_incarnations[backend_index].generation != binding.backend_generation) {
    return detail::reject(OutcomeCode::REJECT_STALE_TOOL_BINDING, subject,
                          "tool backend incarnation does not match the registered incarnation");
  }
  for (ToolBinding& existing : state.tool_bindings) {
    if (existing.tool_name == binding.tool_name) {
      if (binding.binding_generation < existing.binding_generation) {
        return detail::reject(OutcomeCode::REJECT_STALE_TOOL_BINDING, subject,
                              "tool binding generation is older than the bound generation");
      }
      if (binding.binding_generation == existing.binding_generation &&
          binding.backend_generation == existing.backend_generation &&
          existing.current == binding.current) {
        return detail::no_change(subject, "tool binding already installed");
      }
      existing = binding;
      // A tool incarnation change invalidates authorized-but-undispatched
      // actions bound to the previous incarnation.
      for (ActionRecord& action : state.actions) {
        if (action.tool_name == binding.tool_name &&
            action.tool_binding_generation != binding.binding_generation &&
            (action.state == ActionState::AUTHORIZED || action.state == ActionState::ADMITTED)) {
          impl_->set_action_state_locked(action, ActionState::REVALIDATION_REQUIRED);
        }
      }
      return impl_->finish_locked(detail::accepted(subject, "tool binding updated"));
    }
  }
  state.tool_bindings.push_back(binding);
  return impl_->finish_locked(detail::accepted(
      subject, "tool binding installed",
      {ExplanationFactor{"binding_generation", std::to_string(binding.binding_generation.value())}}));
}

MutationResult AgentRuntime::bind_memory(MemoryBinding binding) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  CanonicalState& state = impl_->state;
  const std::string subject = "memory/" + std::to_string(binding.binding_id.value());
  if (!binding.binding_id.valid() || !binding.generation.valid() || binding.state_identity.empty()) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject, "memory binding is incomplete");
  }
  if (binding.state_identity.size() > impl_->limits.max_string_bytes ||
      binding.compatibility.size() > impl_->limits.max_string_bytes) {
    return detail::reject(OutcomeCode::REJECT_LIMIT, subject, "memory binding exceeds string limit");
  }
  bool binding_exists = false;
  for (const MemoryBinding& existing : state.memory_bindings) {
    if (existing.binding_id == binding.binding_id) {
      binding_exists = true;
      break;
    }
  }
  if (!binding_exists && state.memory_bindings.size() >= impl_->limits.max_memory_bindings) {
    return detail::reject(OutcomeCode::REJECT_LIMIT, subject, "memory binding limit reached");
  }
  for (MemoryBinding& existing : state.memory_bindings) {
    if (existing.binding_id == binding.binding_id) {
      if (binding.generation < existing.generation) {
        return detail::reject(OutcomeCode::REJECT_STALE_MEMORY_BINDING, subject,
                              "memory binding generation is older than the bound generation");
      }
      const bool generation_moved = binding.external_generation != existing.external_generation;
      if (binding.generation == existing.generation && !generation_moved &&
          existing.fresh == binding.fresh) {
        return detail::no_change(subject, "memory binding already installed");
      }
      existing = binding;
      if (generation_moved) {
        // A moved memory generation invalidates affected pending actions.
        for (ActionRecord& action : state.actions) {
          if (action.memory_binding_id == binding.binding_id &&
              action.state != ActionState::COMMITTED && !is_terminal(action.state)) {
            impl_->set_action_state_locked(action, ActionState::REVALIDATION_REQUIRED);
          }
        }
      }
      return impl_->finish_locked(detail::accepted(subject, "memory binding updated"));
    }
  }
  state.memory_bindings.push_back(binding);
  return impl_->finish_locked(detail::accepted(subject, "memory binding installed"));
}

MutationResult AgentRuntime::invalidate_memory(MemoryBindingId binding_id,
                                               MemoryBindingGeneration generation,
                                               std::string reason) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  CanonicalState& state = impl_->state;
  const std::string subject = "memory/" + std::to_string(binding_id.value());
  std::uint32_t affected = 0;
  bool found = false;
  for (MemoryBinding& existing : state.memory_bindings) {
    if (existing.binding_id == binding_id) {
      if (generation.valid() && existing.generation != generation) {
        return detail::reject(OutcomeCode::REJECT_STALE_MEMORY_BINDING, subject,
                              "supplied memory generation does not match the bound generation");
      }
      existing.fresh = false;
      existing.read_authority = false;
      existing.write_authority = false;
      found = true;
      break;
    }
  }
  if (!found) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject, "memory binding does not exist");
  }
  for (ActionRecord& action : state.actions) {
    if (action.memory_binding_id == binding_id && !action.committed &&
        !is_terminal(action.state)) {
      impl_->set_action_state_locked(action, ActionState::REVALIDATION_REQUIRED);
      ++affected;
    }
  }
  return impl_->finish_locked(detail::accepted(
      subject, "memory binding invalidated: " + reason,
      {ExplanationFactor{"invalidated_actions", std::to_string(affected)}}));
}

MutationResult AgentRuntime::fence_boot(RuntimeBootId runtime_boot_id, AgentBootId agent_boot_id,
                                        std::string reason) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  CanonicalState& state = impl_->state;
  const std::string subject = "boot/" + std::to_string(runtime_boot_id.value()) + "/" +
                              std::to_string(agent_boot_id.value());
  if (!runtime_boot_id.valid() || !agent_boot_id.valid()) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject, "boot identity is incomplete");
  }
  if (detail::is_fenced(state, runtime_boot_id, agent_boot_id)) {
    return detail::no_change(subject, "boot is already fenced");
  }
  detail::FencedBootRecord record;
  record.runtime_boot_id = runtime_boot_id;
  record.agent_boot_id = agent_boot_id;
  record.reason = std::move(reason);
  record.recorded_at_unix_millis = impl_->now_unix();
  state.fenced_boots.push_back(std::move(record));
  return impl_->finish_locked(detail::accepted(subject, "boot fenced"));
}

// ---------------------------------------------------------------------------
// Run and steps
// ---------------------------------------------------------------------------

MutationResult AgentRuntime::start_run(AgentRunId run_id, AgentRunGeneration run_generation) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  CanonicalState& state = impl_->state;
  const std::string subject = "run/" + std::to_string(run_id.value());

  std::string authority_message;
  if (!impl_->check_runtime_authority_locked(authority_message)) {
    return detail::reject(OutcomeCode::REJECT_CANCELLED, subject, authority_message);
  }
  if (state.lifecycle != RuntimeLifecycle::READY && state.lifecycle != RuntimeLifecycle::RUNNING) {
    return detail::reject(OutcomeCode::REJECT_NOT_READY, subject,
                          std::string("runtime is not ready to start a run (lifecycle ") +
                              std::string(to_string(state.lifecycle)) + ")");
  }
  if (!run_id.valid() || !run_generation.valid()) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject,
                          "run identity or generation is incomplete");
  }
  if (state.current_run_index != kInvalidIndex) {
    const detail::RunRecord& current = state.runs[state.current_run_index];
    if (current.id == run_id && current.generation == run_generation && !current.terminal) {
      return detail::no_change(subject, "run is already current");
    }
    if (current.id == run_id && run_generation < current.generation) {
      return detail::reject(OutcomeCode::REJECT_STALE_RUN_GENERATION, subject,
                            "run generation is older than the current run generation");
    }
    if (!current.terminal) {
      return detail::reject(OutcomeCode::REJECT_CONFLICT, subject,
                            "a different run is still current");
    }
  }
  if (state.runs.size() >= impl_->limits.max_runs) {
    return detail::reject(OutcomeCode::REJECT_LIMIT, subject, "run limit reached");
  }

  detail::RunRecord record;
  record.id = run_id;
  record.generation = run_generation;
  record.runtime_generation = state.runtime_generation;
  record.started_at_unix_millis = impl_->now_unix();
  state.runs.push_back(std::move(record));
  state.current_run_index = static_cast<std::uint32_t>(state.runs.size() - 1);
  state.run_id = run_id;
  state.run_generation = run_generation;
  state.current_step_index = kInvalidIndex;
  state.step_id = StepId{};
  state.step_generation = StepGeneration{};
  if (state.lifecycle == RuntimeLifecycle::READY) {
    state.lifecycle = RuntimeLifecycle::RUNNING;
  }
  detail::record_provenance(state, *impl_->clock, "start_run", subject);
  return impl_->finish_locked(detail::accepted(
      subject, "run started",
      {ExplanationFactor{"run_generation", std::to_string(run_generation.value())}}));
}

MutationResult AgentRuntime::begin_step() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  CanonicalState& state = impl_->state;
  const std::string subject = "runtime/" + std::to_string(state.runtime_id.value());

  std::string authority_message;
  if (!impl_->check_runtime_authority_locked(authority_message)) {
    return detail::reject(OutcomeCode::REJECT_CANCELLED, subject, authority_message);
  }
  if (state.current_run_index == kInvalidIndex) {
    return detail::reject(OutcomeCode::REJECT_NOT_READY, subject, "no current run");
  }
  detail::RunRecord& run = state.runs[state.current_run_index];
  if (run.terminal) {
    return detail::reject(OutcomeCode::REJECT_COMPLETED, subject, "current run is terminal");
  }
  if (!can_admit(state.lifecycle)) {
    return detail::reject(OutcomeCode::REJECT_NOT_READY, subject,
                          std::string("runtime lifecycle ") + std::string(to_string(state.lifecycle)) +
                              " does not admit new steps");
  }
  if (state.steps.size() >= impl_->limits.max_steps_per_run) {
    return detail::reject(OutcomeCode::REJECT_LIMIT, subject, "step limit reached");
  }
  if (state.current_step_index != kInvalidIndex) {
    detail::StepRecord& previous = state.steps[state.current_step_index];
    if (previous.run_id == state.run_id && previous.run_generation == state.run_generation &&
        !previous.closed_at_unix_millis) {
      previous.closed_at_unix_millis = impl_->now_unix();
    }
  }

  detail::StepRecord step;
  step.id = allocate_id<StepTag>();
  step.generation = state.next_step_generation.next();
  state.next_step_generation = step.generation;
  step.run_id = state.run_id;
  step.run_generation = state.run_generation;
  step.progress = ProgressState::IN_PROGRESS;
  step.progress_generation = state.progress_generation;
  step.opened_at_unix_millis = impl_->now_unix();
  state.steps.push_back(std::move(step));
  state.current_step_index = static_cast<std::uint32_t>(state.steps.size() - 1);
  state.step_id = state.steps[state.current_step_index].id;
  state.step_generation = state.steps[state.current_step_index].generation;
  ++run.steps;

  return impl_->finish_locked(detail::accepted(
      "step/" + std::to_string(state.step_id.value()), "step opened",
      {ExplanationFactor{"step_generation", std::to_string(state.step_generation.value())}}));
}

// ---------------------------------------------------------------------------
// Action declaration, admission, authorization
// ---------------------------------------------------------------------------

MutationResult AgentRuntime::declare_action(const ActionSpec& spec, ActionHandle& out) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  CanonicalState& state = impl_->state;
  const std::string subject = "runtime/" + std::to_string(state.runtime_id.value());

  if (state.lifecycle == RuntimeLifecycle::CANCELLING ||
      state.lifecycle == RuntimeLifecycle::CANCELLED) {
    return detail::reject(OutcomeCode::REJECT_CANCELLED, subject,
                          "runtime is cancelled or cancelling");
  }
  if (state.lifecycle == RuntimeLifecycle::COMPLETED) {
    return detail::reject(OutcomeCode::REJECT_COMPLETED, subject, "runtime is completed");
  }
  if (state.lifecycle == RuntimeLifecycle::RETIRED) {
    return detail::reject(OutcomeCode::REJECT_RETIRED, subject, "runtime is retired");
  }
  if (state.lifecycle == RuntimeLifecycle::DRAINING ||
      state.lifecycle == RuntimeLifecycle::SUSPENDING ||
      state.lifecycle == RuntimeLifecycle::SUSPENDED) {
    return detail::reject(OutcomeCode::SHUTTING_DOWN, subject,
                          std::string("runtime lifecycle ") + std::string(to_string(state.lifecycle)) +
                              " does not admit new actions");
  }
  if (!can_admit(state.lifecycle)) {
    return detail::reject(OutcomeCode::REJECT_NOT_READY, subject,
                          std::string("runtime lifecycle ") + std::string(to_string(state.lifecycle)) +
                              " does not admit new actions");
  }
  if (state.cancellation != CancellationState::NONE) {
    return detail::reject(OutcomeCode::REJECT_CANCELLED, subject,
                          "cancellation has been requested");
  }
  if (state.current_run_index == kInvalidIndex) {
    return detail::reject(OutcomeCode::REJECT_NOT_READY, subject, "no current run");
  }
  detail::RunRecord& run = state.runs[state.current_run_index];
  if (run.terminal) {
    return detail::reject(OutcomeCode::REJECT_COMPLETED, subject, "current run is terminal");
  }
  if (state.current_step_index == kInvalidIndex) {
    return detail::reject(OutcomeCode::REJECT_NOT_READY, subject, "no current step");
  }
  detail::StepRecord& step = state.steps[state.current_step_index];
  if (step.run_id != state.run_id || step.run_generation != state.run_generation) {
    return detail::reject(OutcomeCode::REJECT_STALE_STEP, subject,
                          "current step belongs to a superseded run generation");
  }
  if (spec.kind >= ActionKind::kCount) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject, "action kind is invalid");
  }
  if (spec.side_effect >= SideEffectClass::kCount) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject,
                          "side-effect class is invalid");
  }
  if (spec.input.size() > impl_->limits.max_payload_bytes) {
    return detail::reject(OutcomeCode::REJECT_LIMIT, subject, "action input exceeds payload limit");
  }
  if (spec.label.size() > impl_->limits.max_string_bytes ||
      spec.tool_name.size() > impl_->limits.max_string_bytes ||
      spec.model_target.size() > impl_->limits.max_string_bytes ||
      spec.operation_key.size() > impl_->limits.max_string_bytes) {
    return detail::reject(OutcomeCode::REJECT_LIMIT, subject, "action metadata exceeds string limit");
  }
  if (step.declared_actions >= impl_->limits.max_actions_per_step) {
    return detail::reject(OutcomeCode::REJECT_LIMIT, subject, "actions per step limit reached");
  }
  if (state.actions.size() >= impl_->limits.max_actions_per_run) {
    return detail::reject(OutcomeCode::REJECT_LIMIT, subject, "actions per run limit reached");
  }

  ActionRecord action;
  action.id = allocate_id<ActionTag>();
  action.generation = state.next_action_generation.next();
  state.next_action_generation = action.generation;
  action.run_id = state.run_id;
  action.run_generation = state.run_generation;
  action.step_id = state.step_id;
  action.step_generation = state.step_generation;
  action.kind = spec.kind;
  action.label = spec.label;
  action.side_effect = spec.side_effect;
  action.model_target = spec.model_target;
  action.tool_name = spec.tool_name;
  action.operation_key = spec.operation_key;
  action.input = spec.input;
  action.input_digest = spec.input_digest != 0 ? spec.input_digest
                                               : detail::content_digest(spec.input);
  action.max_output_bytes = spec.max_output_bytes == 0 ? 4096 : spec.max_output_bytes;
  if (action.max_output_bytes > impl_->limits.max_payload_bytes) {
    return detail::reject(OutcomeCode::REJECT_LIMIT, subject, "max_output_bytes exceeds payload limit");
  }
  action.dependencies = spec.dependencies;
  if (action.dependencies.size() > impl_->limits.max_collection_entries) {
    return detail::reject(OutcomeCode::REJECT_LIMIT, subject, "dependency list exceeds limit");
  }
  std::vector<ActionId> sorted_dependencies = action.dependencies;
  std::sort(sorted_dependencies.begin(), sorted_dependencies.end());
  if (std::adjacent_find(sorted_dependencies.begin(), sorted_dependencies.end()) !=
      sorted_dependencies.end()) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject,
                          "dependency list contains duplicate action identities");
  }
  for (const ActionId dependency : action.dependencies) {
    if (dependency == action.id) {
      return detail::reject(OutcomeCode::REJECT_INVALID, subject,
                            "an action cannot depend on itself");
    }
  }
  action.requires_checkpoint = spec.requires_checkpoint;
  action.has_retry_override = spec.has_retry_override;
  action.retry = spec.has_retry_override ? spec.retry : state.policy.retry;
  if (spec.has_retry_override) {
    const std::string retry_error = action.retry.validate();
    if (!retry_error.empty()) {
      return detail::reject(OutcomeCode::REJECT_INVALID, subject,
                            "action retry policy is invalid: " + retry_error);
    }
  }
  action.policy_id = state.policy.policy_id;
  action.policy_generation = state.policy.generation;
  action.budget_id = state.has_budget ? state.budget.budget_id : BudgetId{};
  action.budget_generation = state.has_budget ? state.budget.generation : BudgetGeneration{};
  action.budget_required = state.policy.require_budget_for_dispatch;
  action.runtime_boot_id = state.runtime_boot_id;
  action.runtime_epoch = state.runtime_epoch;
  action.coordinator_epoch = state.coordinator_epoch;
  action.declaration_sequence = state.next_declaration_sequence++;
  impl_->set_action_state_locked(action, ActionState::DECLARED);
  action.provenance.origin = "declare_action";
  action.provenance.detail = spec.label;
  action.provenance.recorded_at_unix_millis = impl_->now_unix();

  if (action.kind == ActionKind::MODEL_CALL) {
    const ModelBinding* binding = nullptr;
    for (const ModelBinding& candidate : state.model_bindings) {
      if (candidate.target == action.model_target) {
        binding = &candidate;
        break;
      }
    }
    if (binding == nullptr) {
      return detail::reject(OutcomeCode::REJECT_STALE_MODEL_BINDING, subject,
                            "no model binding exists for target " + action.model_target);
    }
    action.model_binding_generation = binding->binding_generation;
    action.model_backend_id = binding->backend_id;
    action.model_backend_generation = binding->backend_generation;
  } else if (action.kind == ActionKind::TOOL_CALL) {
    const ToolBinding* binding = nullptr;
    for (const ToolBinding& candidate : state.tool_bindings) {
      if (candidate.tool_name == action.tool_name) {
        binding = &candidate;
        break;
      }
    }
    if (binding == nullptr) {
      return detail::reject(OutcomeCode::REJECT_STALE_TOOL_BINDING, subject,
                            "no tool binding exists for tool " + action.tool_name);
    }
    if (binding->side_effect != action.side_effect) {
      return detail::reject(OutcomeCode::REJECT_CONFLICT, subject,
                            "declared side-effect class does not match the tool binding");
    }
    action.tool_binding_generation = binding->binding_generation;
    action.tool_backend_id = binding->backend_id;
    action.tool_backend_generation = binding->backend_generation;
  }

  if (!spec.memory_binding_identity.empty() || spec.memory_binding_generation.valid()) {
    const MemoryBinding* binding = nullptr;
    for (const MemoryBinding& candidate : state.memory_bindings) {
      if (candidate.state_identity == spec.memory_binding_identity) {
        binding = &candidate;
        break;
      }
    }
    if (binding == nullptr) {
      return detail::reject(OutcomeCode::REJECT_STALE_MEMORY_BINDING, subject,
                            "no memory binding exists for identity " +
                                spec.memory_binding_identity);
    }
    if (spec.memory_binding_generation.valid() &&
        binding->generation != spec.memory_binding_generation) {
      return detail::reject(OutcomeCode::REJECT_STALE_MEMORY_BINDING, subject,
                            "supplied memory binding generation does not match the bound generation");
    }
    action.memory_binding_id = binding->binding_id;
    action.memory_binding_generation = binding->generation;
    action.memory_external_generation = binding->external_generation;
    action.requires_memory = true;
  }

  std::string policy_message;
  if (!impl_->policy_allows_locked(action, policy_message)) {
    return detail::reject(OutcomeCode::REJECT_POLICY, subject, policy_message);
  }
  std::string side_effect_message;
  if (!impl_->side_effect_allowed_locked(action, side_effect_message)) {
    return detail::reject(OutcomeCode::REJECT_SIDE_EFFECT_UNSAFE, subject, side_effect_message);
  }
  std::string dependency_message;
  if (!impl_->dependencies_satisfied_locked(action, dependency_message)) {
    return detail::reject(OutcomeCode::REVALIDATION_REQUIRED, subject, dependency_message);
  }

  impl_->indexes.action_by_id.emplace(action.id, static_cast<std::uint32_t>(state.actions.size()));
  state.actions.push_back(std::move(action));
  ++step.declared_actions;
  impl_->mark_dirty();
  const ActionRecord& stored = state.actions.back();
  out.action_id = stored.id;
  out.action_generation = stored.generation;
  out.step_id = stored.step_id;
  out.step_generation = stored.step_generation;
  out.run_generation = stored.run_generation;
  out.state = stored.state;

  return impl_->finish_locked(detail::accepted(
      detail::action_subject(stored.id, stored.generation), "action declared",
      {ExplanationFactor{"kind", std::string(to_string(stored.kind))},
       ExplanationFactor{"side_effect", std::string(to_string(stored.side_effect))}}));
}

MutationResult AgentRuntime::admit_action(ActionId action_id, ActionGeneration action_generation) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  CanonicalState& state = impl_->state;
  const std::string subject = detail::action_subject(action_id, action_generation);

  ActionRecord* action = impl_->action_locked(action_id);
  if (action == nullptr) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject, "action does not exist");
  }
  if (action->generation != action_generation) {
    return detail::reject(OutcomeCode::REJECT_STALE_ACTION, subject,
                          "action generation does not match the declared generation");
  }
  if (action->state == ActionState::ADMITTED || action->state == ActionState::AUTHORIZED ||
      action->state == ActionState::DISPATCHED || action->state == ActionState::IN_FLIGHT ||
      action->state == ActionState::COMPLETED_UNVALIDATED ||
      action->state == ActionState::COMMITTED) {
    return detail::no_change(subject, "action is already admitted or beyond admission");
  }
  if (action->state != ActionState::DECLARED && action->state != ActionState::RETRY_PENDING &&
      action->state != ActionState::REVALIDATION_REQUIRED) {
    return detail::reject(OutcomeCode::REJECT_CONFLICT, subject,
                          std::string("action state ") + std::string(to_string(action->state)) +
                              " does not admit admission");
  }
  if (state.cancellation != CancellationState::NONE ||
      state.lifecycle == RuntimeLifecycle::CANCELLING ||
      state.lifecycle == RuntimeLifecycle::CANCELLED) {
    return detail::reject(OutcomeCode::REJECT_CANCELLED, subject,
                          "runtime cancellation is in progress");
  }
  if (!can_admit(state.lifecycle)) {
    return detail::reject(OutcomeCode::SHUTTING_DOWN, subject,
                          std::string("runtime lifecycle ") + std::string(to_string(state.lifecycle)) +
                              " does not admit new actions");
  }
  if (action->run_generation != state.run_generation) {
    return detail::reject(OutcomeCode::REJECT_STALE_RUN_GENERATION, subject,
                          "action belongs to a superseded run generation");
  }
  if (action->step_generation != state.step_generation && action->step_id != state.step_id) {
    return detail::reject(OutcomeCode::REJECT_STALE_STEP, subject,
                          "action belongs to a superseded step");
  }
  if (impl_->active_action_count_locked() >= impl_->limits.max_active_actions) {
    return detail::reject(OutcomeCode::REJECT_LIMIT, subject, "active action limit reached");
  }
  if (action->ambiguous && requires_reconciliation_on_ambiguity(action->side_effect)) {
    return detail::reject(OutcomeCode::MANUAL_OR_EXTERNAL_RECONCILIATION_REQUIRED, subject,
                          "ambiguous side effect requires external reconciliation before "
                          "continuation");
  }
  std::string policy_message;
  if (!impl_->policy_allows_locked(*action, policy_message)) {
    return detail::reject(OutcomeCode::REJECT_POLICY, subject, policy_message);
  }
  impl_->set_action_state_locked(*action, ActionState::ADMITTED);
  return impl_->finish_locked(detail::accepted(subject, "action admitted"));
}

MutationResult AgentRuntime::authorize_action(ActionId action_id,
                                              ActionGeneration action_generation) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  CanonicalState& state = impl_->state;
  const std::string subject = detail::action_subject(action_id, action_generation);

  ActionRecord* action = impl_->action_locked(action_id);
  if (action == nullptr) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject, "action does not exist");
  }
  if (action->generation != action_generation) {
    return detail::reject(OutcomeCode::REJECT_STALE_ACTION, subject,
                          "action generation does not match the declared generation");
  }
  if (action->state == ActionState::AUTHORIZED) {
    const bool budget_moved = action->budget_required && state.has_budget &&
                              action->budget_generation != state.budget.generation;
    const bool policy_moved = action->policy_generation != state.policy.generation;
    if (!budget_moved && !policy_moved) {
      return detail::no_change(subject, "action is already authorized");
    }
  } else if (action->state != ActionState::ADMITTED &&
             action->state != ActionState::RETRY_PENDING &&
             action->state != ActionState::REVALIDATION_REQUIRED) {
    return detail::reject(OutcomeCode::REJECT_CONFLICT, subject,
                          std::string("action state ") + std::string(to_string(action->state)) +
                              " does not admit authorization");
  }
  // Ambiguity is a semantic blocker that no authority refresh can resolve, so
  // it is reported before any staleness outcome.
  if (action->ambiguous && requires_reconciliation_on_ambiguity(action->side_effect)) {
    return detail::reject(OutcomeCode::MANUAL_OR_EXTERNAL_RECONCILIATION_REQUIRED, subject,
                          "ambiguous side effect requires external reconciliation before "
                          "continuation");
  }
  if (state.cancellation != CancellationState::NONE) {
    return detail::reject(OutcomeCode::REJECT_CANCELLED, subject,
                          "runtime cancellation is in progress");
  }
  if (!can_admit(state.lifecycle)) {
    return detail::reject(OutcomeCode::SHUTTING_DOWN, subject,
                          std::string("runtime lifecycle ") + std::string(to_string(state.lifecycle)) +
                              " does not admit authorization");
  }
  if (action->run_generation != state.run_generation) {
    return detail::reject(OutcomeCode::REJECT_STALE_RUN_GENERATION, subject,
                          "action belongs to a superseded run generation");
  }
  if (action->runtime_epoch != state.runtime_epoch) {
    return detail::reject(OutcomeCode::REJECT_STALE_RUNTIME_EPOCH, subject,
                          "action belongs to a superseded runtime epoch");
  }
  std::string message;
  if (!impl_->policy_allows_locked(*action, message)) {
    return detail::reject(OutcomeCode::REJECT_POLICY, subject, message);
  }
  if (!impl_->assignment_current_locked(message)) {
    return detail::reject(OutcomeCode::REJECT_STALE_ASSIGNMENT, subject, message);
  }
  // Explicit revalidation re-baselines authority generations that moved while
  // the action was pending. Dispatch and commit stay strict and reject a
  // generation that moved after authorization.
  if (action->budget_required && state.has_budget) {
    action->budget_id = state.budget.budget_id;
    action->budget_generation = state.budget.generation;
  }
  const OutcomeCode budget_code = impl_->budget_check_locked(*action, message);
  if (budget_code != OutcomeCode::ACCEPTED) {
    return detail::reject(budget_code, subject, message);
  }
  if (!impl_->dependencies_satisfied_locked(*action, message)) {
    return detail::reject(OutcomeCode::REVALIDATION_REQUIRED, subject, message);
  }
  if (!impl_->side_effect_allowed_locked(*action, message)) {
    return detail::reject(OutcomeCode::REJECT_SIDE_EFFECT_UNSAFE, subject, message);
  }

  if (action->kind == ActionKind::MODEL_CALL) {
    const ModelBinding* binding = nullptr;
    for (const ModelBinding& candidate : state.model_bindings) {
      if (candidate.target == action->model_target) {
        binding = &candidate;
        break;
      }
    }
    if (binding == nullptr || !binding->usable()) {
      return detail::reject(OutcomeCode::REJECT_STALE_MODEL_BINDING, subject,
                            "model binding is not current");
    }
    if (binding->binding_generation != action->model_binding_generation) {
      return detail::reject(OutcomeCode::REJECT_STALE_MODEL_BINDING, subject,
                            "model binding generation moved since the action was declared");
    }
    if (binding->backend_generation != action->model_backend_generation) {
      return detail::reject(OutcomeCode::REJECT_STALE_MODEL_BINDING, subject,
                            "model backend incarnation moved since the action was declared");
    }
    if (impl_->backends.find_model(binding->backend_id) == kInvalidIndex) {
      return detail::reject(OutcomeCode::REJECT_STALE_MODEL_BINDING, subject,
                            "model backend is not registered in this process");
    }
  } else if (action->kind == ActionKind::TOOL_CALL) {
    const ToolBinding* binding = nullptr;
    for (const ToolBinding& candidate : state.tool_bindings) {
      if (candidate.tool_name == action->tool_name) {
        binding = &candidate;
        break;
      }
    }
    if (binding == nullptr || !binding->usable()) {
      return detail::reject(OutcomeCode::REJECT_STALE_TOOL_BINDING, subject,
                            "tool binding is not current");
    }
    if (binding->binding_generation != action->tool_binding_generation) {
      return detail::reject(OutcomeCode::REJECT_STALE_TOOL_BINDING, subject,
                            "tool binding generation moved since the action was declared");
    }
    if (binding->backend_generation != action->tool_backend_generation) {
      return detail::reject(OutcomeCode::REJECT_STALE_TOOL_BINDING, subject,
                            "tool backend incarnation moved since the action was declared");
    }
    if (impl_->backends.find_tool(binding->backend_id) == kInvalidIndex) {
      return detail::reject(OutcomeCode::REJECT_STALE_TOOL_BINDING, subject,
                            "tool backend is not registered in this process");
    }
  }

  if (action->requires_memory) {
    const MemoryBinding* binding = nullptr;
    for (const MemoryBinding& candidate : state.memory_bindings) {
      if (candidate.binding_id == action->memory_binding_id) {
        binding = &candidate;
        break;
      }
    }
    if (binding == nullptr) {
      return detail::reject(OutcomeCode::REJECT_STALE_MEMORY_BINDING, subject,
                            "memory binding no longer exists");
    }
    if (binding->generation != action->memory_binding_generation) {
      return detail::reject(OutcomeCode::REJECT_STALE_MEMORY_BINDING, subject,
                            "memory binding generation moved since the action was declared");
    }
    if (binding->external_generation != action->memory_external_generation) {
      return detail::reject(OutcomeCode::REJECT_STALE_MEMORY_BINDING, subject,
                            "external memory generation moved since the action was declared");
    }
    const bool write = action->kind == ActionKind::MEMORY_WRITE_INTENT;
    const bool usable = write ? binding->usable_for_write() : binding->usable_for_read();
    if (!usable) {
      return detail::reject(OutcomeCode::REVALIDATION_REQUIRED, subject,
                            "memory binding is not current for this action");
    }
  }

  action->policy_id = state.policy.policy_id;
  action->policy_generation = state.policy.generation;
  impl_->set_action_state_locked(*action, ActionState::AUTHORIZED);
  action->runtime_boot_id = state.runtime_boot_id;
  action->runtime_epoch = state.runtime_epoch;
  action->coordinator_epoch = state.coordinator_epoch;
  return impl_->finish_locked(detail::accepted(
      subject, "action authorized",
      {ExplanationFactor{"policy_generation", std::to_string(state.policy.generation.value())},
       ExplanationFactor{"budget_generation",
                         std::to_string(state.has_budget ? state.budget.generation.value() : 0)}}));
}

}  // namespace agent_runtime
