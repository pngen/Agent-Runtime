// Agent Runtime - runtime save/load with candidate-state validation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include <string>
#include <utility>

#include "persistence_internal.hpp"
#include "runtime_impl.hpp"

namespace agent_runtime {

using detail::CanonicalState;

MutationResult AgentRuntime::save(const std::string& path) const {
  std::unique_lock<std::mutex> lock(impl_->mutex);
  const std::string subject = "runtime/" + std::to_string(impl_->state.runtime_id.value());

  if (path.empty()) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject, "persistence path is empty");
  }
  const std::string limits_error = impl_->limits.validate();
  if (!limits_error.empty()) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject,
                          "resource limits are invalid: " + limits_error);
  }

  std::vector<std::uint8_t> document = detail::encode_document(impl_->state, impl_->limits);
  if (document.size() > impl_->limits.max_persistence_bytes) {
    return detail::reject(OutcomeCode::REJECT_LIMIT, subject,
                          "encoded durable document exceeds the persistence byte limit");
  }
  const std::uint64_t digest = detail::canonical_digest(impl_->state);
  const std::uint64_t sequence = impl_->state.mutation_sequence;

  // Filesystem I/O happens with no canonical lock held. The mutation sequence
  // is rechecked afterwards so that a save that raced with a mutation is
  // reported rather than silently treated as the current durable state.
  PersistenceOptions options;
  options.max_bytes = impl_->limits.max_persistence_bytes;
  std::string error;
  lock.unlock();
  const bool written = detail::atomic_write(path, std::span<const std::uint8_t>(document.data(),
                                                                               document.size()),
                                            options, error);
  lock.lock();

  if (!written) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject,
                          "durable save failed: " + error);
  }
  if (impl_->state.mutation_sequence != sequence) {
    return MutationResult(OutcomeCode::REVALIDATION_REQUIRED,
                          ExplanationBuilder(OutcomeCode::REVALIDATION_REQUIRED, subject)
                              .add_u64("mutation_sequence_at_encode", sequence)
                              .add_u64("mutation_sequence_now", impl_->state.mutation_sequence)
                              .set_message("runtime mutated while durable state was being written")
                              .build());
  }
  return detail::accepted(subject, "durable state saved",
                          {ExplanationFactor{"bytes", std::to_string(document.size())},
                           ExplanationFactor{"semantic_digest", std::to_string(digest)}});
}

MutationResult AgentRuntime::load(const std::string& path) {
  std::unique_lock<std::mutex> lock(impl_->mutex);
  const std::string subject = "persistence/" + path;

  if (path.empty()) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject, "persistence path is empty");
  }
  if (impl_->state.lifecycle != RuntimeLifecycle::DECLARED) {
    return detail::reject(OutcomeCode::REJECT_CONFLICT, subject,
                          "state can only be loaded into a declared runtime");
  }

  std::vector<std::uint8_t> bytes;
  std::string error;
  const std::uint64_t max_bytes = impl_->limits.max_persistence_bytes;
  const ResourceLimits limits = impl_->limits;
  lock.unlock();
  const bool read_ok = detail::read_file_bounded(path, max_bytes, bytes, error);
  lock.lock();
  if (!read_ok) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject, "durable load failed: " + error);
  }

  detail::DecodeResult decoded = detail::decode_document(
      std::span<const std::uint8_t>(bytes.data(), bytes.size()), limits);
  if (!decoded.ok() || !decoded.state.has_value()) {
    return detail::reject(decoded.code, subject,
                          decoded.message.empty() ? "durable document could not be decoded"
                                                  : decoded.message);
  }
  if (decoded.state->runtime_id != impl_->state.runtime_id) {
    return detail::reject(OutcomeCode::REJECT_INVALID, subject,
                          "durable document belongs to a different runtime identity");
  }

  // The candidate has already passed structural and semantic validation. Apply
  // it, then immediately mark every dynamic binding as requiring revalidation.
  CanonicalState restored = std::move(*decoded.state);
  std::uint64_t highest_id = 0;
  auto observe = [&highest_id](std::uint64_t value) {
    if (value > highest_id) {
      highest_id = value;
    }
  };
  observe(restored.runtime_id.value());
  observe(restored.agent_id.value());
  observe(restored.run_id.value());
  observe(restored.step_id.value());
  for (const detail::RunRecord& run : restored.runs) {
    observe(run.id.value());
  }
  for (const detail::StepRecord& step : restored.steps) {
    observe(step.id.value());
  }
  for (const detail::ActionRecord& action : restored.actions) {
    observe(action.id.value());
    observe(action.memory_binding_id.value());
    observe(action.checkpoint_binding_id.value());
    observe(action.budget_id.value());
    observe(action.policy_id.value());
    for (const ActionId dependency : action.dependencies) {
      observe(dependency.value());
    }
  }
  for (const detail::AttemptRecord& attempt : restored.attempts) {
    observe(attempt.id.value());
    observe(attempt.result_id.value());
    observe(attempt.model_call_id.value());
    observe(attempt.tool_call_id.value());
  }
  for (const MemoryBinding& binding : restored.memory_bindings) {
    observe(binding.binding_id.value());
  }
  for (const detail::CheckpointRecord& checkpoint : restored.checkpoints) {
    observe(checkpoint.binding.binding_id.value());
  }
  for (const detail::AssignmentRecord& assignment : restored.assignments) {
    observe(assignment.authority.assignment_id.value());
  }
  observe(restored.budget.budget_id.value());
  IdAllocator::observe(highest_id);

  restored.lifecycle = RuntimeLifecycle::RECOVERING;
  restored.recovery = RecoveryStatus::DURABLE_STATE_RESTORED;
  restored.recovery_reason = "durable state loaded; dynamic authority is not current";
  restored.has_assignment = false;
  restored.current_assignment.current = false;
  restored.has_budget = false;
  for (detail::AssignmentRecord& assignment : restored.assignments) {
    assignment.current = false;
  }
  for (ModelBinding& binding : restored.model_bindings) {
    binding.current = false;
  }
  for (ToolBinding& binding : restored.tool_bindings) {
    binding.current = false;
  }
  for (MemoryBinding& binding : restored.memory_bindings) {
    binding.fresh = false;
  }
  for (detail::CheckpointRecord& checkpoint : restored.checkpoints) {
    if (checkpoint.state == CheckpointState::ACCEPTED ||
        checkpoint.state == CheckpointState::REQUESTED ||
        checkpoint.state == CheckpointState::IN_PROGRESS) {
      checkpoint.state = CheckpointState::REVALIDATION_REQUIRED;
      checkpoint.binding.current = false;
    }
  }
  for (detail::AttemptRecord& attempt : restored.attempts) {
    if (is_in_flight(attempt.state)) {
      attempt.superseded = true;
      attempt.current = false;
      attempt.state = ActionState::REVALIDATION_REQUIRED;
      attempt.error = "process authority was lost while the attempt was in flight";
    }
  }
  for (detail::ActionRecord& action : restored.actions) {
    if (is_in_flight(action.state) || action.state == ActionState::COMPLETED_UNVALIDATED) {
      action.state = ActionState::REVALIDATION_REQUIRED;
      action.current_attempt = ActionAttemptId{};
      action.current_attempt_generation = AttemptGeneration{};
    }
  }
  restored.mutation_sequence += 1;

  impl_->state = std::move(restored);
  impl_->backends.clear();
  impl_->inflight_cancellation = std::make_shared<std::atomic<bool>>(false);
  impl_->reindex();

  detail::record_provenance(impl_->state, *impl_->clock, "load", path);
  return impl_->finish_locked(detail::accepted(
      subject, "durable state loaded; runtime requires recovery and revalidation",
      {ExplanationFactor{"committed_actions",
                         std::to_string(impl_->state.committed_actions)},
       ExplanationFactor{"lifecycle", std::string(to_string(impl_->state.lifecycle))},
       ExplanationFactor{"progress_generation",
                         std::to_string(impl_->state.progress_generation.value())},
       ExplanationFactor{"record_count", std::to_string(decoded.info.record_count)}}));
}

std::unique_ptr<AgentRuntime> AgentRuntime::open_durable_state(const std::string& path,
                                                                  MutationResult& result) {
  if (path.empty()) {
    result = detail::reject(OutcomeCode::REJECT_INVALID, "persistence", "path is empty");
    return nullptr;
  }
  std::vector<std::uint8_t> bytes;
  std::string error;
  const ResourceLimits limits = default_resource_limits();
  if (!detail::read_file_bounded(path, limits.max_persistence_bytes, bytes, error)) {
    result = detail::reject(OutcomeCode::REJECT_INVALID, "persistence", error);
    return nullptr;
  }
  detail::DecodeResult decoded =
      detail::decode_document(std::span<const std::uint8_t>(bytes.data(), bytes.size()), limits);
  if (!decoded.ok() || !decoded.state.has_value()) {
    result = detail::reject(decoded.code, "persistence", decoded.message);
    return nullptr;
  }
  AgentRuntimeOptions options;
  options.runtime_id = decoded.state->runtime_id;
  options.runtime_generation = decoded.state->runtime_generation;
  options.runtime_boot_id = decoded.state->runtime_boot_id;
  options.agent_id = decoded.state->agent_id;
  options.agent_generation = decoded.state->agent_generation;
  options.agent_boot_id = decoded.state->agent_boot_id;
  options.runtime_epoch = decoded.state->runtime_epoch;
  options.coordinator_epoch = decoded.state->coordinator_epoch;
  options.clock = std::make_shared<SystemClock>();
  options.limits = limits;
  options.policy = decoded.state->policy;
  auto runtime = std::make_unique<AgentRuntime>(std::move(options));

  CanonicalState restored = std::move(*decoded.state);
  restored.lifecycle = RuntimeLifecycle::RECOVERING;
  restored.recovery = RecoveryStatus::DURABLE_STATE_RESTORED;
  restored.recovery_reason = "opened for inspection; dynamic authority is not current";
  restored.has_assignment = false;
  restored.current_assignment.current = false;
  restored.has_budget = false;
  for (detail::AssignmentRecord& assignment : restored.assignments) {
    assignment.current = false;
  }
  for (ModelBinding& binding : restored.model_bindings) {
    binding.current = false;
  }
  for (ToolBinding& binding : restored.tool_bindings) {
    binding.current = false;
  }
  for (MemoryBinding& binding : restored.memory_bindings) {
    binding.fresh = false;
  }
  for (detail::AttemptRecord& attempt : restored.attempts) {
    if (is_in_flight(attempt.state)) {
      attempt.superseded = true;
      attempt.current = false;
      attempt.state = ActionState::REVALIDATION_REQUIRED;
      attempt.error = "process authority was lost while the attempt was in flight";
    }
  }
  for (detail::ActionRecord& action : restored.actions) {
    if (is_in_flight(action.state) || action.state == ActionState::COMPLETED_UNVALIDATED) {
      action.state = ActionState::REVALIDATION_REQUIRED;
      action.current_attempt = ActionAttemptId{};
      action.current_attempt_generation = AttemptGeneration{};
    }
  }
  runtime->impl_->state = std::move(restored);
  runtime->impl_->reindex();
  result = detail::accepted("persistence/" + path, "durable state opened for inspection",
                            {ExplanationFactor{"record_count",
                                               std::to_string(decoded.info.record_count)}});
  return runtime;
}

}  // namespace agent_runtime
