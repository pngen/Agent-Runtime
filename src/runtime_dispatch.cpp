// Agent Runtime - dispatch, completion validation and commit authority.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <string>
#include <utility>

#include "agent_runtime/detail/sha256.hpp"
#include "runtime_impl.hpp"

namespace agent_runtime {

using detail::ActionRecord;
using detail::AttemptRecord;
using detail::CanonicalState;
using detail::kInvalidIndex;

namespace {

struct AttemptLookup {
  std::uint32_t action_index = kInvalidIndex;
  std::uint32_t attempt_index = kInvalidIndex;
};

[[nodiscard]] AttemptLookup lookup_attempt(const CanonicalState& state,
                                         const detail::Indexes& indexes, ActionAttemptId id) {
  AttemptLookup lookup;
  const auto attempt_found = indexes.attempt_by_id.find(id);
  if (attempt_found == indexes.attempt_by_id.end()) {
    return lookup;
  }
  lookup.attempt_index = attempt_found->second;
  const auto action_found = indexes.action_by_id.find(state.attempts[lookup.attempt_index].action_id);
  if (action_found != indexes.action_by_id.end()) {
    lookup.action_index = action_found->second;
  }
  return lookup;
}

[[nodiscard]] std::uint32_t count_in_flight_tool_attempts(const CanonicalState& state) noexcept {
  std::uint32_t count = 0;
  for (const AttemptRecord& attempt : state.attempts) {
    if (!is_in_flight(attempt.state) || attempt.superseded) {
      continue;
    }
    const std::uint32_t index = detail::find_action(state, attempt.action_id);
    if (index != kInvalidIndex && state.actions[index].kind == ActionKind::TOOL_CALL) {
      ++count;
    }
  }
  return count;
}

[[nodiscard]] std::uint32_t count_in_flight_model_attempts(const CanonicalState& state) noexcept {
  std::uint32_t count = 0;
  for (const AttemptRecord& attempt : state.attempts) {
    if (!is_in_flight(attempt.state) || attempt.superseded) {
      continue;
    }
    const std::uint32_t index = detail::find_action(state, attempt.action_id);
    if (index != kInvalidIndex && state.actions[index].kind == ActionKind::MODEL_CALL) {
      ++count;
    }
  }
  return count;
}

[[nodiscard]] std::uint32_t count_in_flight_effect_free(const CanonicalState& state) noexcept {
  std::uint32_t count = 0;
  for (const AttemptRecord& attempt : state.attempts) {
    if (!is_in_flight(attempt.state) || attempt.superseded) {
      continue;
    }
    const std::uint32_t index = detail::find_action(state, attempt.action_id);
    if (index != kInvalidIndex && is_effect_free(state.actions[index].side_effect)) {
      ++count;
    }
  }
  return count;
}

}  // namespace

MutationResult AgentRuntime::Impl::classify_ambiguous_completion_locked(
    ActionRecord& action, AttemptRecord& attempt, std::string message) {
  attempt.completion_recorded = true;
  attempt.completed_at_unix_millis = now_unix();
  attempt.error = message;
  if (requires_reconciliation_on_ambiguity(action.side_effect)) {
    attempt.ambiguous = true;
    attempt.completion_status = CompletionStatus::AMBIGUOUS;
    set_attempt_state_locked(attempt, ActionState::REVALIDATION_REQUIRED);
    attempt.superseded = true;
    attempt.current = false;
    set_action_state_locked(action, ActionState::REVALIDATION_REQUIRED);
    action.ambiguous = true;
    action.completion_status = CompletionStatus::AMBIGUOUS;
    action.last_failure = RetryClass::AMBIGUOUS_COMPLETION;
    action.last_error = message;
    action.current_attempt = ActionAttemptId{};
    action.current_attempt_generation = AttemptGeneration{};
    return detail::reject(OutcomeCode::AMBIGUOUS_COMPLETION,
                  detail::action_subject(action.id, action.generation),
                  message + ": the runtime will not invent an outcome for a " +
                      std::string(to_string(action.side_effect)) + " action");
  }
  return mark_retry_pending_locked(action, attempt, RetryClass::TRANSIENT,
                                   message + ": effect-free action may be repeated safely");
}

DispatchResult AgentRuntime::dispatch_action(ActionId action_id, ActionGeneration action_generation,
                                             DispatchMode mode) {
  DispatchResult out;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  CanonicalState& state = impl_->state;
  const std::string subject = detail::action_subject(action_id, action_generation);

  ActionRecord* action = impl_->action_locked(action_id);
  if (action == nullptr) {
    out.result = detail::reject(OutcomeCode::REJECT_INVALID, subject, "action does not exist");
    return out;
  }
  if (action->generation != action_generation) {
    out.result = detail::reject(OutcomeCode::REJECT_STALE_ACTION, subject,
                                "action generation does not match the declared generation");
    return out;
  }
  if (action->state == ActionState::COMMITTED) {
    out.result = detail::reject(OutcomeCode::REJECT_COMPLETED, subject,
                                "action is already committed");
    return out;
  }
  if (action->state == ActionState::CANCELLED || action->state == ActionState::SUPERSEDED) {
    out.result = detail::reject(OutcomeCode::REJECT_CANCELLED, subject,
                                "action authority has been invalidated");
    return out;
  }
  if (action->state != ActionState::AUTHORIZED) {
    out.result = detail::reject(OutcomeCode::REJECT_NOT_READY, subject,
                                std::string("action state ") +
                                    std::string(to_string(action->state)) +
                                    " is not authorized for dispatch");
    return out;
  }
  if (state.cancellation != CancellationState::NONE ||
      state.lifecycle == RuntimeLifecycle::CANCELLING ||
      state.lifecycle == RuntimeLifecycle::CANCELLED) {
    out.result = detail::reject(OutcomeCode::REJECT_CANCELLED, subject,
                                "runtime cancellation is in progress");
    return out;
  }
  if (!can_dispatch(state.lifecycle)) {
    out.result = detail::reject(OutcomeCode::SHUTTING_DOWN, subject,
                                std::string("runtime lifecycle ") +
                                    std::string(to_string(state.lifecycle)) +
                                    " does not admit dispatch");
    return out;
  }
  if (state.checkpoint_due) {
    out.result = detail::reject(OutcomeCode::REVALIDATION_REQUIRED, subject,
                                "a required checkpoint is due before further dispatch");
    return out;
  }
  if (action->runtime_epoch != state.runtime_epoch) {
    out.result = detail::reject(OutcomeCode::REJECT_STALE_RUNTIME_EPOCH, subject,
                                "action belongs to a superseded runtime epoch");
    return out;
  }
  if (action->coordinator_epoch != state.coordinator_epoch) {
    out.result = detail::reject(OutcomeCode::REJECT_STALE_RUNTIME_EPOCH, subject,
                                "action belongs to a superseded coordinator epoch");
    return out;
  }
  if (detail::is_fenced(state, action->runtime_boot_id, state.agent_boot_id)) {
    out.result = detail::reject(OutcomeCode::REJECT_STALE_RUNTIME_BOOT, subject,
                                "action authority belongs to a fenced runtime boot");
    return out;
  }
  if (action->run_generation != state.run_generation) {
    out.result = detail::reject(OutcomeCode::REJECT_STALE_RUN_GENERATION, subject,
                                "action belongs to a superseded run generation");
    return out;
  }
  if (action->step_id != state.step_id || action->step_generation != state.step_generation) {
    out.result = detail::reject(OutcomeCode::REJECT_STALE_STEP, subject,
                                "action belongs to a superseded step");
    return out;
  }
  if (action->policy_generation != state.policy.generation) {
    out.result = detail::reject(OutcomeCode::REJECT_STALE_POLICY, subject,
                                "action was authorized under a superseded policy generation");
    return out;
  }

  std::string message;
  if (!impl_->policy_allows_locked(*action, message)) {
    out.result = detail::reject(OutcomeCode::REJECT_POLICY, subject, message);
    return out;
  }
  if (!impl_->assignment_current_locked(message)) {
    out.result = detail::reject(OutcomeCode::REJECT_STALE_ASSIGNMENT, subject, message);
    return out;
  }
  const OutcomeCode budget_code = impl_->budget_check_locked(*action, message);
  if (budget_code != OutcomeCode::ACCEPTED) {
    out.result = detail::reject(budget_code, subject, message);
    return out;
  }
  if (!impl_->dependencies_satisfied_locked(*action, message)) {
    out.result = detail::reject(OutcomeCode::REVALIDATION_REQUIRED, subject, message);
    return out;
  }
  if (!impl_->side_effect_allowed_locked(*action, message)) {
    out.result = detail::reject(OutcomeCode::REJECT_SIDE_EFFECT_UNSAFE, subject, message);
    return out;
  }
  if (action->attempt_count >= action->retry.max_attempts ||
      action->attempt_count >= state.policy.max_retries_per_action + 1) {
    out.result = detail::reject(OutcomeCode::REJECT_RETRY_EXHAUSTED, subject,
                                "attempt budget exhausted");
    return out;
  }
  if (impl_->in_flight_attempt_count_locked() >= impl_->limits.max_active_attempts) {
    out.result = detail::reject(OutcomeCode::REJECT_LIMIT, subject, "active attempt limit reached");
    return out;
  }
  if (impl_->active_action_count_locked() > state.policy.max_parallel_actions) {
    out.result = detail::reject(OutcomeCode::REJECT_LIMIT, subject, "parallel action limit reached");
    return out;
  }
  if (action->kind == ActionKind::TOOL_CALL &&
      impl_->in_flight_tool_count_locked() >= state.policy.max_parallel_tool_calls) {
    out.result = detail::reject(OutcomeCode::REJECT_LIMIT, subject,
                                "parallel tool call limit reached");
    return out;
  }
  if (action->kind == ActionKind::MODEL_CALL &&
      impl_->in_flight_model_count_locked() >= state.policy.max_parallel_model_calls) {
    out.result = detail::reject(OutcomeCode::REJECT_LIMIT, subject,
                                "parallel model call limit reached");
    return out;
  }
  if (is_effect_free(action->side_effect) &&
      impl_->in_flight_effect_free_count_locked() >= state.policy.max_parallel_read_actions) {
    out.result = detail::reject(OutcomeCode::REJECT_LIMIT, subject,
                                "parallel read-only limit reached");
    return out;
  }

  const ToolBinding* tool_binding = nullptr;
  const ModelBinding* model_binding = nullptr;
  if (action->kind == ActionKind::TOOL_CALL) {
    for (const ToolBinding& candidate : state.tool_bindings) {
      if (candidate.tool_name == action->tool_name) {
        tool_binding = &candidate;
        break;
      }
    }
    if (tool_binding == nullptr || !tool_binding->usable()) {
      out.result = detail::reject(OutcomeCode::REJECT_STALE_TOOL_BINDING, subject,
                                  "tool binding is not current at dispatch");
      return out;
    }
    if (tool_binding->binding_generation != action->tool_binding_generation ||
        tool_binding->backend_generation != action->tool_backend_generation) {
      out.result = detail::reject(OutcomeCode::REJECT_STALE_TOOL_BINDING, subject,
                                  "tool binding or backend incarnation moved before dispatch");
      return out;
    }
    if (tool_binding->max_input_bytes != 0 && action->input.size() > tool_binding->max_input_bytes) {
      out.result = detail::reject(OutcomeCode::REJECT_LIMIT, subject,
                                  "tool input exceeds the binding input limit");
      return out;
    }
  } else if (action->kind == ActionKind::MODEL_CALL) {
    for (const ModelBinding& candidate : state.model_bindings) {
      if (candidate.target == action->model_target) {
        model_binding = &candidate;
        break;
      }
    }
    if (model_binding == nullptr || !model_binding->usable()) {
      out.result = detail::reject(OutcomeCode::REJECT_STALE_MODEL_BINDING, subject,
                                  "model binding is not current at dispatch");
      return out;
    }
    if (model_binding->binding_generation != action->model_binding_generation ||
        model_binding->backend_generation != action->model_backend_generation) {
      out.result = detail::reject(OutcomeCode::REJECT_STALE_MODEL_BINDING, subject,
                                  "model binding or backend incarnation moved before dispatch");
      return out;
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
    if (binding == nullptr || binding->generation != action->memory_binding_generation) {
      out.result = detail::reject(OutcomeCode::REJECT_STALE_MEMORY_BINDING, subject,
                                  "memory binding generation moved before dispatch");
      return out;
    }
    if (binding->external_generation != action->memory_external_generation) {
      out.result = detail::reject(OutcomeCode::REJECT_STALE_MEMORY_BINDING, subject,
                                  "external memory generation moved before dispatch");
      return out;
    }
    const bool write = action->kind == ActionKind::MEMORY_WRITE_INTENT;
    if (write ? !binding->usable_for_write() : !binding->usable_for_read()) {
      out.result = detail::reject(OutcomeCode::REVALIDATION_REQUIRED, subject,
                                  "memory binding is not usable at dispatch");
      return out;
    }
  }

  AttemptRecord attempt;
  attempt.id = allocate_id<ActionAttemptTag>();
  attempt.generation = action->next_attempt_generation;
  action->next_attempt_generation = attempt.generation.next();
  attempt.action_id = action->id;
  attempt.action_generation = action->generation;
  attempt.runtime_boot_id = state.runtime_boot_id;
  attempt.runtime_epoch = state.runtime_epoch;
  attempt.coordinator_epoch = state.coordinator_epoch;
  impl_->set_attempt_state_locked(attempt, ActionState::DISPATCHED);
  attempt.current = true;
  attempt.dispatched_at_unix_millis = impl_->now_unix();
  if (action->kind == ActionKind::MODEL_CALL) {
    attempt.model_call_id = allocate_id<ModelCallTag>();
    attempt.model_call_generation = ModelCallGeneration(attempt.generation.value());
    attempt.backend_id = action->model_backend_id;
    attempt.backend_generation = action->model_backend_generation;
  } else if (action->kind == ActionKind::TOOL_CALL) {
    attempt.tool_call_id = allocate_id<ToolCallTag>();
    attempt.tool_call_generation = ToolCallGeneration(attempt.generation.value());
    attempt.backend_id = action->tool_backend_id;
    attempt.backend_generation = action->tool_backend_generation;
  }
  ++action->attempt_count;
  action->current_attempt = attempt.id;
  action->current_attempt_generation = attempt.generation;
  impl_->set_action_state_locked(*action, ActionState::DISPATCHED);
  action->runtime_boot_id = state.runtime_boot_id;
  action->runtime_epoch = state.runtime_epoch;
  action->coordinator_epoch = state.coordinator_epoch;

  impl_->indexes.attempt_by_id.emplace(attempt.id, static_cast<std::uint32_t>(state.attempts.size()));
  state.attempts.push_back(std::move(attempt));
  const std::uint32_t attempt_index = static_cast<std::uint32_t>(state.attempts.size() - 1);
  action->attempt_indices.push_back(attempt_index);

  out.attempt_id = state.attempts[attempt_index].id;
  out.attempt_generation = state.attempts[attempt_index].generation;

  if (mode == DispatchMode::EXTERNAL) {
    if (action->kind == ActionKind::MODEL_CALL) {
      ModelRequest& request = out.model_request;
      request.call_id = state.attempts[attempt_index].model_call_id;
      request.call_generation = state.attempts[attempt_index].model_call_generation;
      request.action_id = action->id;
      request.action_generation = action->generation;
      request.attempt_id = state.attempts[attempt_index].id;
      request.attempt_generation = state.attempts[attempt_index].generation;
      request.runtime_id = state.runtime_id;
      request.runtime_boot_id = state.runtime_boot_id;
      request.target = action->model_target;
      request.prompt = action->input;
      request.max_output_bytes = action->max_output_bytes;
      request.request_digest = action->input_digest;
      if (action->requires_memory) {
        request.context_identity = std::to_string(action->memory_binding_id.value());
        request.context_generation = action->memory_binding_generation;
      }
      request.budget_actions_remaining = state.has_budget ? state.budget.remaining_actions : 0;
      out.has_model_request = true;
    } else if (action->kind == ActionKind::TOOL_CALL) {
      ToolRequest& request = out.tool_request;
      request.call_id = state.attempts[attempt_index].tool_call_id;
      request.call_generation = state.attempts[attempt_index].tool_call_generation;
      request.action_id = action->id;
      request.action_generation = action->generation;
      request.attempt_id = state.attempts[attempt_index].id;
      request.attempt_generation = state.attempts[attempt_index].generation;
      request.runtime_id = state.runtime_id;
      request.runtime_boot_id = state.runtime_boot_id;
      request.tool_name = action->tool_name;
      request.side_effect = action->side_effect;
      request.operation_key = action->operation_key;
      request.input = action->input;
      request.input_digest = action->input_digest;
      request.max_output_bytes = action->max_output_bytes;
      out.has_tool_request = true;
    }
  }

  if (state.lifecycle == RuntimeLifecycle::RUNNING) {
    if (action->kind == ActionKind::MODEL_CALL) {
      state.lifecycle = RuntimeLifecycle::WAITING_MODEL;
    } else if (action->kind == ActionKind::TOOL_CALL) {
      state.lifecycle = RuntimeLifecycle::WAITING_TOOL;
    } else {
      state.lifecycle = RuntimeLifecycle::WAITING_EXTERNAL;
    }
  }

  out.result = impl_->finish_locked(detail::accepted(
      subject, "dispatch authority reserved",
      {ExplanationFactor{"attempt_generation", std::to_string(out.attempt_generation.value())},
       ExplanationFactor{"side_effect", std::string(to_string(action->side_effect))}}));
  return out;
}
LocalExecutionResult AgentRuntime::dispatch_and_execute(ActionId action_id,
                                                        ActionGeneration action_generation) {
  LocalExecutionResult out;
  DispatchResult dispatch = dispatch_action(action_id, action_generation, DispatchMode::EXTERNAL);
  out.dispatch = dispatch.result;
  out.attempt_id = dispatch.attempt_id;
  out.attempt_generation = dispatch.attempt_generation;
  if (!dispatch.result.accepted()) {
    return out;
  }

  ActionKind kind = ActionKind::CUSTOM;
  BackendId backend_id{};
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const ActionRecord* action = impl_->action_locked(action_id);
    if (action == nullptr) {
      out.completion = detail::reject(OutcomeCode::REJECT_INVALID,
                                      detail::action_subject(action_id, action_generation),
                                      "action disappeared between dispatch and execution");
      return out;
    }
    kind = action->kind;
    backend_id = action->tool_backend_id.valid() ? action->tool_backend_id : action->model_backend_id;
  }

  // The backend is invoked with no canonical lock held. This is the boundary
  // that keeps external calls, filesystem I/O and accelerator synchronization
  // outside every critical section.
  if (kind == ActionKind::TOOL_CALL) {
    std::shared_ptr<ToolBackend> backend;
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      const std::uint32_t index = impl_->backends.find_tool(backend_id);
      if (index != kInvalidIndex) {
        backend = impl_->backends.tool_backends[index];
      }
      const auto attempt_found = impl_->indexes.attempt_by_id.find(out.attempt_id);
      if (attempt_found != impl_->indexes.attempt_by_id.end()) {
        AttemptRecord& candidate = impl_->state.attempts[attempt_found->second];
        if (candidate.state == ActionState::DISPATCHED) {
          impl_->set_attempt_state_locked(candidate, ActionState::IN_FLIGHT);
        }
      }
    }
    if (backend == nullptr) {
      out.completion = detail::reject(OutcomeCode::REJECT_STALE_TOOL_BINDING,
                                      detail::action_subject(action_id, action_generation),
                                      "tool backend is not registered in this process");
      return out;
    }
    ToolResponse response =
        backend->invoke(dispatch.tool_request, CancellationProbe(impl_->inflight_cancellation));
    out.completion = submit_tool_completion(response);
    return out;
  }
  if (kind == ActionKind::MODEL_CALL) {
    std::shared_ptr<ModelBackend> backend;
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      const std::uint32_t index = impl_->backends.find_model(backend_id);
      if (index != kInvalidIndex) {
        backend = impl_->backends.model_backends[index];
      }
      const auto attempt_found = impl_->indexes.attempt_by_id.find(out.attempt_id);
      if (attempt_found != impl_->indexes.attempt_by_id.end()) {
        AttemptRecord& candidate = impl_->state.attempts[attempt_found->second];
        if (candidate.state == ActionState::DISPATCHED) {
          impl_->set_attempt_state_locked(candidate, ActionState::IN_FLIGHT);
        }
      }
    }
    if (backend == nullptr) {
      out.completion = detail::reject(OutcomeCode::REJECT_STALE_MODEL_BINDING,
                                      detail::action_subject(action_id, action_generation),
                                      "model backend is not registered in this process");
      return out;
    }
    ModelResponse response =
        backend->invoke(dispatch.model_request, CancellationProbe(impl_->inflight_cancellation));
    out.completion = submit_model_completion(response);
    return out;
  }
  out.completion = detail::reject(OutcomeCode::REJECT_INVALID,
                                  detail::action_subject(action_id, action_generation),
                                  std::string("action kind ") + std::string(to_string(kind)) +
                                      " has no in-process backend path");
  return out;
}

MutationResult AgentRuntime::submit_model_completion(const ModelResponse& response) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  CanonicalState& state = impl_->state;
  const std::string initial_subject = detail::action_subject(response.action_id, ActionGeneration{});

  const AttemptLookup lookup = lookup_attempt(state, impl_->indexes, response.attempt_id);
  if (lookup.attempt_index == kInvalidIndex || lookup.action_index == kInvalidIndex) {
    return detail::reject(OutcomeCode::REJECT_INVALID, initial_subject,
                          "model completion references an unknown attempt");
  }
  AttemptRecord& attempt = state.attempts[lookup.attempt_index];
  ActionRecord& action = state.actions[lookup.action_index];
  const std::string subject = detail::action_subject(action.id, action.generation);

  if (attempt.generation != response.attempt_generation) {
    return detail::reject(OutcomeCode::REJECT_STALE_ATTEMPT, subject,
                          "attempt generation does not match the reserved attempt");
  }
  if (attempt.model_call_id != response.call_id ||
      attempt.model_call_generation != response.call_generation) {
    return detail::reject(OutcomeCode::REJECT_CONFLICT, subject,
                          "model completion call identity does not match the reserved call");
  }
  if (response.action_id.valid() && response.action_id != action.id) {
    return detail::reject(OutcomeCode::REJECT_CONFLICT, subject,
                          "model completion action identity does not match the attempt");
  }
  if (attempt.completion_recorded) {
    if (attempt.result_digest == response.output_digest &&
        attempt.completion_status == response.status) {
      return detail::no_change(subject, "identical model completion already recorded");
    }
    return detail::reject(OutcomeCode::REJECT_CONFLICT, subject,
                          "conflicting duplicate model completion");
  }
  if (attempt.runtime_boot_id != state.runtime_boot_id) {
    return detail::reject(OutcomeCode::REJECT_STALE_RUNTIME_BOOT, subject,
                          "completion was produced under a superseded runtime boot");
  }
  if (attempt.runtime_epoch != state.runtime_epoch) {
    return detail::reject(OutcomeCode::REJECT_STALE_RUNTIME_EPOCH, subject,
                          "completion was produced under a superseded runtime epoch");
  }
  if (attempt.coordinator_epoch != state.coordinator_epoch) {
    return detail::reject(OutcomeCode::REJECT_STALE_RUNTIME_EPOCH, subject,
                          "completion was produced under a superseded coordinator epoch");
  }
  if (action.generation != attempt.action_generation) {
    return detail::reject(OutcomeCode::REJECT_STALE_ACTION, subject,
                          "completion action generation is stale");
  }
  if (action.run_generation != state.run_generation) {
    return detail::reject(OutcomeCode::REJECT_STALE_RUN_GENERATION, subject,
                          "completion belongs to a superseded run generation");
  }
  if (action.step_id != state.step_id || action.step_generation != state.step_generation) {
    return detail::reject(OutcomeCode::REJECT_STALE_STEP, subject,
                          "completion belongs to a superseded step");
  }
  if (!attempt.current || attempt.superseded) {
    return detail::reject(OutcomeCode::REJECT_STALE_ATTEMPT, subject,
                          "attempt has been superseded");
  }
  if (!is_in_flight(attempt.state)) {
    return detail::reject(OutcomeCode::REJECT_STALE_ATTEMPT, subject, "attempt is not in flight");
  }
  if (state.cancellation != CancellationState::NONE ||
      state.lifecycle == RuntimeLifecycle::CANCELLING ||
      state.lifecycle == RuntimeLifecycle::CANCELLED) {
    attempt.superseded = true;
    attempt.current = false;
    impl_->set_attempt_state_locked(attempt, ActionState::CANCELLED);
    attempt.error = "late model completion rejected by cancellation";
    return detail::reject(OutcomeCode::REJECT_CANCELLED, subject,
                          "late model completion rejected by cancellation");
  }
  if (state.lifecycle == RuntimeLifecycle::COMPLETED ||
      state.lifecycle == RuntimeLifecycle::RETIRED) {
    return detail::reject(OutcomeCode::REJECT_COMPLETED, subject, "runtime is terminal");
  }
  if (action.completions_seen >= impl_->limits.max_completions_per_action) {
    return detail::reject(OutcomeCode::REJECT_LIMIT, subject,
                          "completion limit reached for this action");
  }
  const ModelBinding* binding = nullptr;
  for (const ModelBinding& candidate : state.model_bindings) {
    if (candidate.target == action.model_target) {
      binding = &candidate;
      break;
    }
  }
  if (binding == nullptr || binding->backend_generation != attempt.backend_generation) {
    attempt.superseded = true;
    attempt.current = false;
    impl_->set_attempt_state_locked(attempt, ActionState::REVALIDATION_REQUIRED);
    attempt.error = "model backend incarnation changed before completion was observed";
    return detail::reject(OutcomeCode::REJECT_STALE_MODEL_BINDING, subject, attempt.error);
  }
  if (response.backend_generation.valid() &&
      response.backend_generation != attempt.backend_generation) {
    return detail::reject(OutcomeCode::REJECT_STALE_MODEL_BINDING, subject,
                          "completion reports a different backend incarnation");
  }

  ++action.completions_seen;
  attempt.completion_recorded = true;
  attempt.result_id =
      response.result_id.valid() ? response.result_id : allocate_id<ResultTag>();
  attempt.completion_generation = response.completion_generation.valid()
                                      ? response.completion_generation
                                      : CompletionGeneration(attempt.generation.value());
  attempt.result_digest = response.output_digest != 0 ? response.output_digest
                                                      : detail::content_digest(response.output);
  attempt.completed_at_unix_millis = impl_->now_unix();
  attempt.error = response.error;

  if (response.status == CompletionStatus::AMBIGUOUS) {
    return impl_->finish_locked(impl_->classify_ambiguous_completion_locked(
        action, attempt,
        response.error.empty() ? "model completion is ambiguous" : response.error));
  }
  if (response.status == CompletionStatus::SUCCEEDED) {
    impl_->set_attempt_state_locked(attempt, ActionState::COMPLETED_UNVALIDATED);
    attempt.completion_status = CompletionStatus::SUCCEEDED;
    impl_->set_action_state_locked(action, ActionState::COMPLETED_UNVALIDATED);
    action.completion_status = CompletionStatus::SUCCEEDED;
    action.result_digest = attempt.result_digest;
    return impl_->finish_locked(detail::accepted(
        subject, "model completion recorded and awaiting commit validation",
        {ExplanationFactor{"completion_generation",
                           std::to_string(attempt.completion_generation.value())},
         ExplanationFactor{"committed", "false"}}));
  }
  if (response.status == CompletionStatus::CANCELLED) {
    impl_->set_attempt_state_locked(attempt, ActionState::CANCELLED);
    attempt.completion_status = CompletionStatus::CANCELLED;
    attempt.current = false;
    attempt.superseded = true;
    impl_->set_action_state_locked(action, ActionState::CANCELLED);
    action.current_attempt = ActionAttemptId{};
    action.current_attempt_generation = AttemptGeneration{};
    return detail::reject(OutcomeCode::REJECT_CANCELLED, subject,
                          "model backend reports the call was cancelled");
  }
  return impl_->finish_locked(impl_->mark_retry_pending_locked(
      action, attempt, response.failure_class == RetryClass::NONE ? RetryClass::UNKNOWN
                                                                 : response.failure_class,
      response.error.empty() ? "model call failed" : response.error));
}
MutationResult AgentRuntime::submit_tool_completion(const ToolResponse& response) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  CanonicalState& state = impl_->state;
  const std::string initial_subject = detail::action_subject(response.action_id, ActionGeneration{});

  const AttemptLookup lookup = lookup_attempt(state, impl_->indexes, response.attempt_id);
  if (lookup.attempt_index == kInvalidIndex || lookup.action_index == kInvalidIndex) {
    return detail::reject(OutcomeCode::REJECT_INVALID, initial_subject,
                          "tool completion references an unknown attempt");
  }
  AttemptRecord& attempt = state.attempts[lookup.attempt_index];
  ActionRecord& action = state.actions[lookup.action_index];
  const std::string subject = detail::action_subject(action.id, action.generation);

  if (attempt.generation != response.attempt_generation) {
    return detail::reject(OutcomeCode::REJECT_STALE_ATTEMPT, subject,
                          "attempt generation does not match the reserved attempt");
  }
  if (attempt.tool_call_id != response.call_id ||
      attempt.tool_call_generation != response.call_generation) {
    return detail::reject(OutcomeCode::REJECT_CONFLICT, subject,
                          "tool completion call identity does not match the reserved call");
  }
  if (response.action_id.valid() && response.action_id != action.id) {
    return detail::reject(OutcomeCode::REJECT_CONFLICT, subject,
                          "tool completion action identity does not match the attempt");
  }
  if (attempt.completion_recorded) {
    if (attempt.result_digest == response.result_digest &&
        attempt.completion_status == response.status) {
      return detail::no_change(subject, "identical tool completion already recorded");
    }
    return detail::reject(OutcomeCode::REJECT_CONFLICT, subject,
                          "conflicting duplicate tool completion");
  }
  if (attempt.runtime_boot_id != state.runtime_boot_id) {
    return detail::reject(OutcomeCode::REJECT_STALE_RUNTIME_BOOT, subject,
                          "completion was produced under a superseded runtime boot");
  }
  if (attempt.runtime_epoch != state.runtime_epoch) {
    return detail::reject(OutcomeCode::REJECT_STALE_RUNTIME_EPOCH, subject,
                          "completion was produced under a superseded runtime epoch");
  }
  if (attempt.coordinator_epoch != state.coordinator_epoch) {
    return detail::reject(OutcomeCode::REJECT_STALE_RUNTIME_EPOCH, subject,
                          "completion was produced under a superseded coordinator epoch");
  }
  if (action.generation != attempt.action_generation) {
    return detail::reject(OutcomeCode::REJECT_STALE_ACTION, subject,
                          "completion action generation is stale");
  }
  if (action.run_generation != state.run_generation) {
    return detail::reject(OutcomeCode::REJECT_STALE_RUN_GENERATION, subject,
                          "completion belongs to a superseded run generation");
  }
  if (action.step_id != state.step_id || action.step_generation != state.step_generation) {
    return detail::reject(OutcomeCode::REJECT_STALE_STEP, subject,
                          "completion belongs to a superseded step");
  }
  if (!attempt.current || attempt.superseded) {
    return detail::reject(OutcomeCode::REJECT_STALE_ATTEMPT, subject,
                          "attempt has been superseded");
  }
  if (!is_in_flight(attempt.state)) {
    return detail::reject(OutcomeCode::REJECT_STALE_ATTEMPT, subject, "attempt is not in flight");
  }
  if (state.cancellation != CancellationState::NONE ||
      state.lifecycle == RuntimeLifecycle::CANCELLING ||
      state.lifecycle == RuntimeLifecycle::CANCELLED) {
    attempt.superseded = true;
    attempt.current = false;
    impl_->set_attempt_state_locked(attempt, ActionState::CANCELLED);
    attempt.error = "late tool completion rejected by cancellation";
    if (requires_reconciliation_on_ambiguity(action.side_effect)) {
      attempt.ambiguous = true;
      action.ambiguous = true;
      impl_->set_action_state_locked(action, ActionState::REVALIDATION_REQUIRED);
      action.completion_status = CompletionStatus::AMBIGUOUS;
      action.last_failure = RetryClass::AMBIGUOUS_COMPLETION;
      action.last_error = attempt.error;
      action.current_attempt = ActionAttemptId{};
      action.current_attempt_generation = AttemptGeneration{};
      return detail::reject(OutcomeCode::AMBIGUOUS_COMPLETION, subject,
                            "tool completion arrived after cancellation and the side-effect class "
                            "cannot be reconciled automatically");
    }
    return detail::reject(OutcomeCode::REJECT_CANCELLED, subject,
                          "late tool completion rejected by cancellation");
  }
  if (state.lifecycle == RuntimeLifecycle::COMPLETED ||
      state.lifecycle == RuntimeLifecycle::RETIRED) {
    return detail::reject(OutcomeCode::REJECT_COMPLETED, subject, "runtime is terminal");
  }
  if (action.completions_seen >= impl_->limits.max_completions_per_action) {
    return detail::reject(OutcomeCode::REJECT_LIMIT, subject,
                          "completion limit reached for this action");
  }
  const ToolBinding* binding = nullptr;
  for (const ToolBinding& candidate : state.tool_bindings) {
    if (candidate.tool_name == action.tool_name) {
      binding = &candidate;
      break;
    }
  }
  if (binding == nullptr || binding->backend_generation != attempt.backend_generation) {
    attempt.superseded = true;
    attempt.current = false;
    impl_->set_attempt_state_locked(attempt, ActionState::REVALIDATION_REQUIRED);
    attempt.error = "tool backend incarnation changed before completion was observed";
    if (requires_reconciliation_on_ambiguity(action.side_effect)) {
      attempt.ambiguous = true;
      action.ambiguous = true;
      impl_->set_action_state_locked(action, ActionState::REVALIDATION_REQUIRED);
      action.completion_status = CompletionStatus::AMBIGUOUS;
      action.last_failure = RetryClass::AMBIGUOUS_COMPLETION;
      action.last_error = attempt.error;
      action.current_attempt = ActionAttemptId{};
      action.current_attempt_generation = AttemptGeneration{};
      return detail::reject(OutcomeCode::AMBIGUOUS_COMPLETION, subject, attempt.error);
    }
    return detail::reject(OutcomeCode::REJECT_STALE_TOOL_BINDING, subject, attempt.error);
  }
  if (response.backend_generation.valid() &&
      response.backend_generation != attempt.backend_generation) {
    return detail::reject(OutcomeCode::REJECT_STALE_TOOL_BINDING, subject,
                          "completion reports a different tool backend incarnation");
  }
  if (response.output.size() > action.max_output_bytes ||
      response.output.size() > impl_->limits.max_payload_bytes) {
    return detail::reject(OutcomeCode::REJECT_LIMIT, subject,
                          "tool completion payload exceeds the bound limit");
  }

  ++action.completions_seen;
  attempt.completion_recorded = true;
  attempt.result_id =
      response.result_id.valid() ? response.result_id : allocate_id<ResultTag>();
  attempt.completion_generation = response.completion_generation.valid()
                                      ? response.completion_generation
                                      : CompletionGeneration(attempt.generation.value());
  attempt.result_digest = response.result_digest != 0 ? response.result_digest
                                                      : detail::content_digest(response.output);
  attempt.completed_at_unix_millis = impl_->now_unix();
  attempt.error = response.error;

  if (response.status == CompletionStatus::AMBIGUOUS) {
    return impl_->finish_locked(impl_->classify_ambiguous_completion_locked(
        action, attempt,
        response.error.empty() ? "tool completion is ambiguous" : response.error));
  }
  if (response.status == CompletionStatus::SUCCEEDED) {
    impl_->set_attempt_state_locked(attempt, ActionState::COMPLETED_UNVALIDATED);
    attempt.completion_status = CompletionStatus::SUCCEEDED;
    impl_->set_action_state_locked(action, ActionState::COMPLETED_UNVALIDATED);
    action.completion_status = CompletionStatus::SUCCEEDED;
    action.result_digest = attempt.result_digest;
    return impl_->finish_locked(detail::accepted(
        subject, "tool completion recorded and awaiting commit validation",
        {ExplanationFactor{"completion_generation",
                           std::to_string(attempt.completion_generation.value())},
         ExplanationFactor{"committed", "false"},
         ExplanationFactor{"deduplicated", response.deduplicated ? "true" : "false"}}));
  }
  if (response.status == CompletionStatus::CANCELLED) {
    impl_->set_attempt_state_locked(attempt, ActionState::CANCELLED);
    attempt.completion_status = CompletionStatus::CANCELLED;
    attempt.current = false;
    attempt.superseded = true;
    impl_->set_action_state_locked(action, ActionState::CANCELLED);
    action.current_attempt = ActionAttemptId{};
    action.current_attempt_generation = AttemptGeneration{};
    return detail::reject(OutcomeCode::REJECT_CANCELLED, subject,
                          "tool backend reports the call was cancelled");
  }
  return impl_->finish_locked(impl_->mark_retry_pending_locked(
      action, attempt,
      response.failure_class == RetryClass::NONE ? RetryClass::UNKNOWN : response.failure_class,
      response.error.empty() ? "tool call failed" : response.error));
}

MutationResult AgentRuntime::submit_failure(const ActionFailure& failure) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  CanonicalState& state = impl_->state;
  const AttemptLookup lookup = lookup_attempt(state, impl_->indexes, failure.attempt_id);
  if (lookup.attempt_index == kInvalidIndex || lookup.action_index == kInvalidIndex) {
    return detail::reject(OutcomeCode::REJECT_INVALID,
                          detail::action_subject(failure.action_id, failure.action_generation),
                          "failure references an unknown attempt");
  }
  AttemptRecord& attempt = state.attempts[lookup.attempt_index];
  ActionRecord& action = state.actions[lookup.action_index];
  const std::string subject = detail::action_subject(action.id, action.generation);

  if (action.generation != failure.action_generation ||
      attempt.generation != failure.attempt_generation) {
    return detail::reject(OutcomeCode::REJECT_STALE_ATTEMPT, subject,
                          "failure references a stale action or attempt generation");
  }
  if (attempt.completion_recorded) {
    return detail::no_change(subject, "attempt already has a recorded completion");
  }
  if (attempt.runtime_boot_id != state.runtime_boot_id) {
    return detail::reject(OutcomeCode::REJECT_STALE_RUNTIME_BOOT, subject,
                          "failure was reported under a superseded runtime boot");
  }
  if (attempt.runtime_epoch != state.runtime_epoch) {
    return detail::reject(OutcomeCode::REJECT_STALE_RUNTIME_EPOCH, subject,
                          "failure was reported under a superseded runtime epoch");
  }
  if (attempt.coordinator_epoch != state.coordinator_epoch) {
    return detail::reject(OutcomeCode::REJECT_STALE_RUNTIME_EPOCH, subject,
                          "failure was reported under a superseded coordinator epoch");
  }
  if (!attempt.current || attempt.superseded) {
    return detail::reject(OutcomeCode::REJECT_STALE_ATTEMPT, subject,
                          "attempt has been superseded");
  }
  if (!is_in_flight(attempt.state)) {
    return detail::reject(OutcomeCode::REJECT_STALE_ATTEMPT, subject, "attempt is not in flight");
  }
  if (state.cancellation != CancellationState::NONE) {
    return detail::reject(OutcomeCode::REJECT_CANCELLED, subject,
                          "runtime cancellation is in progress");
  }
  ++action.completions_seen;
  attempt.completion_recorded = true;
  attempt.completed_at_unix_millis = impl_->now_unix();

  if (failure.completion_ambiguous || failure.failure_class == RetryClass::AMBIGUOUS_COMPLETION ||
      failure.status == CompletionStatus::AMBIGUOUS) {
    return impl_->finish_locked(impl_->classify_ambiguous_completion_locked(
        action, attempt,
        failure.message.empty() ? "action completion is ambiguous" : failure.message));
  }
  return impl_->finish_locked(
      impl_->mark_retry_pending_locked(action, attempt, failure.failure_class, failure.message));
}
MutationResult AgentRuntime::commit_action(ActionId action_id, ActionGeneration action_generation) {
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
  if (action->committed) {
    if (action->accepted_result.valid() && action->accepted_completion_generation.valid()) {
      return detail::no_change(subject, "action generation already has an authoritative commit");
    }
    return detail::reject(OutcomeCode::REJECT_CONFLICT, subject,
                          "action is committed without an accepted completion");
  }
  if (action->state != ActionState::COMPLETED_UNVALIDATED) {
    return detail::reject(OutcomeCode::REJECT_NOT_READY, subject,
                          std::string("action state ") + std::string(to_string(action->state)) +
                              " has no completion to validate");
  }
  if (state.cancellation != CancellationState::NONE ||
      state.lifecycle == RuntimeLifecycle::CANCELLING ||
      state.lifecycle == RuntimeLifecycle::CANCELLED) {
    impl_->set_action_state_locked(*action, ActionState::CANCELLED);
    action->current_attempt = ActionAttemptId{};
    action->current_attempt_generation = AttemptGeneration{};
    return detail::reject(OutcomeCode::REJECT_CANCELLED, subject,
                          "cancelled runtime must not commit progress");
  }
  if (state.lifecycle == RuntimeLifecycle::COMPLETED ||
      state.lifecycle == RuntimeLifecycle::RETIRED) {
    return detail::reject(OutcomeCode::REJECT_COMPLETED, subject, "runtime is terminal");
  }
  if (action->runtime_epoch != state.runtime_epoch) {
    return detail::reject(OutcomeCode::REJECT_STALE_RUNTIME_EPOCH, subject,
                          "action was dispatched under a superseded runtime epoch");
  }
  if (action->coordinator_epoch != state.coordinator_epoch) {
    return detail::reject(OutcomeCode::REJECT_STALE_RUNTIME_EPOCH, subject,
                          "action was dispatched under a superseded coordinator epoch");
  }
  if (action->run_generation != state.run_generation) {
    return detail::reject(OutcomeCode::REJECT_STALE_RUN_GENERATION, subject,
                          "action belongs to a superseded run generation");
  }
  if (action->step_id != state.step_id || action->step_generation != state.step_generation) {
    return detail::reject(OutcomeCode::REJECT_STALE_STEP, subject,
                          "action belongs to a superseded step");
  }
  if (action->policy_generation != state.policy.generation) {
    return detail::reject(OutcomeCode::REJECT_STALE_POLICY, subject,
                          "action was created under a superseded policy generation");
  }
  if (state.checkpoint_due && !action->requires_checkpoint) {
    return detail::reject(OutcomeCode::REVALIDATION_REQUIRED, subject,
                          "a required checkpoint is due before further progress commits");
  }

  AttemptRecord* attempt = nullptr;
  const auto current_attempt_found = impl_->indexes.attempt_by_id.find(action->current_attempt);
  if (current_attempt_found != impl_->indexes.attempt_by_id.end()) {
    attempt = &state.attempts[current_attempt_found->second];
  }
  if (attempt == nullptr) {
    return detail::reject(OutcomeCode::REJECT_STALE_ATTEMPT, subject,
                          "action has no current attempt to commit");
  }
  if (attempt->generation != action->current_attempt_generation) {
    return detail::reject(OutcomeCode::REJECT_STALE_ATTEMPT, subject,
                          "current attempt generation is inconsistent");
  }
  if (attempt->action_generation != action->generation) {
    return detail::reject(OutcomeCode::REJECT_STALE_ACTION, subject,
                          "attempt belongs to a different action generation");
  }
  if (!attempt->current || attempt->superseded) {
    return detail::reject(OutcomeCode::REJECT_STALE_ATTEMPT, subject,
                          "attempt has been superseded");
  }
  if (attempt->state != ActionState::COMPLETED_UNVALIDATED) {
    return detail::reject(OutcomeCode::REJECT_NOT_READY, subject,
                          "attempt has no validated completion");
  }
  if (attempt->runtime_boot_id != state.runtime_boot_id) {
    return detail::reject(OutcomeCode::REJECT_STALE_RUNTIME_BOOT, subject,
                          "attempt was dispatched under a superseded runtime boot");
  }
  if (attempt->runtime_epoch != state.runtime_epoch) {
    return detail::reject(OutcomeCode::REJECT_STALE_RUNTIME_EPOCH, subject,
                          "attempt was dispatched under a superseded runtime epoch");
  }
  if (attempt->coordinator_epoch != state.coordinator_epoch) {
    return detail::reject(OutcomeCode::REJECT_STALE_RUNTIME_EPOCH, subject,
                          "attempt was dispatched under a superseded coordinator epoch");
  }
  if (attempt->completion_status != CompletionStatus::SUCCEEDED) {
    return detail::reject(OutcomeCode::REJECT_CONFLICT, subject,
                          "attempt completion is not a success");
  }

  std::string message;
  if (!impl_->policy_allows_locked(*action, message)) {
    return detail::reject(OutcomeCode::REJECT_POLICY, subject, message);
  }
  if (!impl_->assignment_current_locked(message)) {
    return detail::reject(OutcomeCode::REJECT_STALE_ASSIGNMENT, subject, message);
  }
  const OutcomeCode budget_code = impl_->budget_check_locked(*action, message);
  if (budget_code != OutcomeCode::ACCEPTED) {
    return detail::reject(budget_code, subject, message);
  }
  if (!impl_->dependencies_satisfied_locked(*action, message)) {
    return detail::reject(OutcomeCode::REVALIDATION_REQUIRED, subject, message);
  }
  if (action->kind == ActionKind::TOOL_CALL) {
    const ToolBinding* binding = nullptr;
    for (const ToolBinding& candidate : state.tool_bindings) {
      if (candidate.tool_name == action->tool_name) {
        binding = &candidate;
        break;
      }
    }
    if (binding == nullptr) {
      return detail::reject(OutcomeCode::REJECT_STALE_TOOL_BINDING, subject,
                            "tool binding no longer exists");
    }
    if (binding->binding_generation != action->tool_binding_generation) {
      return detail::reject(OutcomeCode::REJECT_STALE_TOOL_BINDING, subject,
                            "tool binding generation moved before commit");
    }
    if (binding->backend_generation != attempt->backend_generation) {
      return detail::reject(OutcomeCode::REJECT_STALE_TOOL_BINDING, subject,
                            "tool backend incarnation moved before commit");
    }
  } else if (action->kind == ActionKind::MODEL_CALL) {
    const ModelBinding* binding = nullptr;
    for (const ModelBinding& candidate : state.model_bindings) {
      if (candidate.target == action->model_target) {
        binding = &candidate;
        break;
      }
    }
    if (binding == nullptr) {
      return detail::reject(OutcomeCode::REJECT_STALE_MODEL_BINDING, subject,
                            "model binding no longer exists");
    }
    if (binding->binding_generation != action->model_binding_generation) {
      return detail::reject(OutcomeCode::REJECT_STALE_MODEL_BINDING, subject,
                            "model binding generation moved before commit");
    }
    if (binding->backend_generation != attempt->backend_generation) {
      return detail::reject(OutcomeCode::REJECT_STALE_MODEL_BINDING, subject,
                            "model backend incarnation moved before commit");
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
                            "memory binding generation moved before commit");
    }
    if (binding->external_generation != action->memory_external_generation) {
      return detail::reject(OutcomeCode::REJECT_STALE_MEMORY_BINDING, subject,
                            "external memory generation moved before commit");
    }
    if (!binding->fresh) {
      return detail::reject(OutcomeCode::REVALIDATION_REQUIRED, subject,
                            "memory binding is no longer current");
    }
  }
  if (action->requires_checkpoint) {
    const detail::CheckpointRecord* checkpoint = nullptr;
    for (const detail::CheckpointRecord& candidate : state.checkpoints) {
      if (candidate.binding.binding_id == action->checkpoint_binding_id &&
          candidate.binding.generation == action->checkpoint_generation) {
        checkpoint = &candidate;
        break;
      }
    }
    if (checkpoint == nullptr || checkpoint->state != CheckpointState::ACCEPTED ||
        !checkpoint->binding.current || !checkpoint->binding.restorable) {
      return detail::reject(OutcomeCode::REVALIDATION_REQUIRED, subject,
                            "a compatible accepted checkpoint is required before commit");
    }
    if (checkpoint->binding.runtime_generation != state.runtime_generation ||
        checkpoint->binding.run_generation != state.run_generation) {
      return detail::reject(OutcomeCode::REJECT_STALE_CHECKPOINT, subject,
                            "bound checkpoint is not compatible with the current generation");
    }
  }

  const CommitGeneration generation = CommitGeneration(state.progress_generation.value() + 1);
  impl_->set_attempt_state_locked(*attempt, ActionState::COMMITTED);
  attempt->current = false;
  attempt->superseded = true;
  action->accepted_result = attempt->result_id;
  action->accepted_completion_generation = attempt->completion_generation;
  action->result_digest = attempt->result_digest;
  impl_->advance_progress_locked(*action, generation);

  if (state.lifecycle == RuntimeLifecycle::WAITING_MODEL ||
      state.lifecycle == RuntimeLifecycle::WAITING_TOOL ||
      state.lifecycle == RuntimeLifecycle::WAITING_EXTERNAL) {
    // The maintained counter makes this an O(1) check instead of a full scan.
    if (impl_->in_flight_attempt_count_locked() == 0) {
      state.lifecycle = RuntimeLifecycle::RUNNING;
    }
  }
  if (state.policy.checkpoint_every_commits != 0 &&
      state.committed_actions - state.commits_at_last_checkpoint >=
          state.policy.checkpoint_every_commits) {
    state.checkpoint_due = true;
    if (state.lifecycle == RuntimeLifecycle::RUNNING) {
      state.lifecycle = RuntimeLifecycle::CHECKPOINTING;
    }
  }
  detail::record_provenance(state, *impl_->clock, "commit_action", subject);

  return impl_->finish_locked(detail::accepted(
      subject, "completion validated and durable progress committed",
      {ExplanationFactor{"commit_generation", std::to_string(generation.value())},
       ExplanationFactor{"committed", "true"},
       ExplanationFactor{"progress_generation",
                         std::to_string(state.progress_generation.value())}}));
}

}  // namespace agent_runtime
