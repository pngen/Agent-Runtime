// Agent Runtime - versioned durable persistence: encoding, decoding, atomic replace.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "persistence_internal.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

#include "agent_runtime/detail/crc32c.hpp"
#include "agent_runtime/detail/sha256.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace agent_runtime::detail {
namespace {

enum RecordType : std::uint16_t {
  kRecordRuntime = 1,
  kRecordRun = 2,
  kRecordStep = 3,
  kRecordAction = 4,
  kRecordAttempt = 5,
  kRecordAssignment = 6,
  kRecordModelBinding = 7,
  kRecordToolBinding = 8,
  kRecordMemoryBinding = 9,
  kRecordCheckpoint = 10,
  kRecordFencedBoot = 11,
  kRecordPolicy = 12,
  kRecordBudget = 13,
  kRecordProvenance = 14,
  kRecordTypeMax = 14,
};

class Writer {
 public:
  explicit Writer(std::vector<std::uint8_t>& out) : out_(out) {}

  void u8(std::uint8_t value) { out_.push_back(value); }
  void u16(std::uint16_t value) {
    out_.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
  }
  void u32(std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
      out_.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
    }
  }
  void u64(std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
      out_.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
    }
  }
  void boolean(bool value) { u8(value ? 1u : 0u); }
  void text(std::string_view value) {
    u32(static_cast<std::uint32_t>(value.size()));
    out_.insert(out_.end(), value.begin(), value.end());
  }
  void bytes(std::span<const std::uint8_t> value) {
    u32(static_cast<std::uint32_t>(value.size()));
    out_.insert(out_.end(), value.begin(), value.end());
  }
  void u64_vector(const std::vector<std::uint64_t>& values) {
    u32(static_cast<std::uint32_t>(values.size()));
    for (const std::uint64_t value : values) {
      u64(value);
    }
  }

 private:
  std::vector<std::uint8_t>& out_;
};

class Reader {
 public:
  Reader(std::span<const std::uint8_t> data, const ResourceLimits& limits)
      : data_(data), limits_(limits) {}

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - offset_; }

  std::uint8_t u8() {
    if (!need(1)) {
      return 0;
    }
    return data_[offset_++];
  }
  std::uint16_t u16() {
    if (!need(2)) {
      return 0;
    }
    const std::uint16_t value = static_cast<std::uint16_t>(data_[offset_]) |
                                static_cast<std::uint16_t>(data_[offset_ + 1] << 8);
    offset_ += 2;
    return value;
  }
  std::uint32_t u32() {
    if (!need(4)) {
      return 0;
    }
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      value |= static_cast<std::uint32_t>(data_[offset_ + i]) << (8 * i);
    }
    offset_ += 4;
    return value;
  }
  std::uint64_t u64() {
    if (!need(8)) {
      return 0;
    }
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
      value |= static_cast<std::uint64_t>(data_[offset_ + i]) << (8 * i);
    }
    offset_ += 8;
    return value;
  }
  bool boolean() { return u8() != 0; }
  std::string text() {
    const std::uint32_t size = u32();
    if (!ok_) {
      return {};
    }
    if (size > limits_.max_string_bytes) {
      fail("string exceeds the configured string limit");
      return {};
    }
    if (!need(size)) {
      return {};
    }
    std::string value(reinterpret_cast<const char*>(data_.data() + offset_), size);
    offset_ += size;
    return value;
  }
  std::vector<std::uint64_t> u64_vector() {
    const std::uint32_t count = u32();
    if (!ok_) {
      return {};
    }
    if (count > limits_.max_collection_entries) {
      fail("collection exceeds the configured collection limit");
      return {};
    }
    if (static_cast<std::uint64_t>(count) * 8ull > remaining()) {
      fail("collection length exceeds the remaining record bytes");
      return {};
    }
    std::vector<std::uint64_t> values;
    values.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      values.push_back(u64());
    }
    return values;
  }

  void fail(std::string message) {
    if (ok_) {
      ok_ = false;
      error_ = std::move(message);
    }
  }
  void skip(std::size_t count) {
    if (!need(count)) {
      return;
    }
    offset_ += count;
  }

 private:
  bool need(std::size_t count) {
    if (!ok_) {
      return false;
    }
    if (count > data_.size() - offset_) {
      fail("record is truncated");
      return false;
    }
    return true;
  }

  std::span<const std::uint8_t> data_;
  ResourceLimits limits_;
  std::size_t offset_ = 0;
  bool ok_ = true;
  std::string error_;
};

void write_binding(Writer& writer, const MemoryBinding& binding) {
  writer.u64(binding.binding_id.value());
  writer.u64(binding.generation.value());
  writer.text(binding.state_identity);
  writer.text(binding.compatibility);
  writer.u64(binding.external_generation.value());
  writer.boolean(binding.read_authority);
  writer.boolean(binding.write_authority);
  writer.boolean(binding.fresh);
  writer.text(binding.provenance);
  writer.text(binding.digest);
}

MemoryBinding read_binding(Reader& reader) {
  MemoryBinding binding;
  binding.binding_id = MemoryBindingId(reader.u64());
  binding.generation = MemoryBindingGeneration(reader.u64());
  binding.state_identity = reader.text();
  binding.compatibility = reader.text();
  binding.external_generation = Generation<MemoryBindingTag>(reader.u64());
  binding.read_authority = reader.boolean();
  binding.write_authority = reader.boolean();
  binding.fresh = reader.boolean();
  binding.provenance = reader.text();
  binding.digest = reader.text();
  return binding;
}

}  // namespace

std::vector<std::uint8_t> encode_document(const CanonicalState& state,
                                          const ResourceLimits& limits) {
  std::vector<std::uint8_t> payload;
  payload.reserve(1024);

  auto emit = [&payload](std::uint16_t type, const std::vector<std::uint8_t>& body) {
    payload.push_back(static_cast<std::uint8_t>(type & 0xFFu));
    payload.push_back(static_cast<std::uint8_t>((type >> 8) & 0xFFu));
    const std::uint32_t size = static_cast<std::uint32_t>(body.size());
    for (int i = 0; i < 4; ++i) {
      payload.push_back(static_cast<std::uint8_t>((size >> (8 * i)) & 0xFFu));
    }
    payload.insert(payload.end(), body.begin(), body.end());
  };

  std::uint32_t record_count = 0;

  {
    std::vector<std::uint8_t> body;
    Writer writer(body);
    writer.u64(state.runtime_id.value());
    writer.u64(state.runtime_generation.value());
    writer.u64(state.runtime_boot_id.value());
    writer.u64(state.agent_id.value());
    writer.u64(state.agent_generation.value());
    writer.u64(state.agent_boot_id.value());
    writer.u64(state.runtime_epoch.value());
    writer.u64(state.coordinator_epoch.value());
    writer.u16(static_cast<std::uint16_t>(state.lifecycle));
    writer.u16(static_cast<std::uint16_t>(state.cancellation));
    writer.u16(static_cast<std::uint16_t>(state.recovery));
    writer.text(state.terminal_reason);
    writer.text(state.cancellation_reason);
    writer.text(state.recovery_reason);
    writer.u64(state.run_id.value());
    writer.u64(state.run_generation.value());
    writer.u32(state.current_run_index);
    writer.u64(state.step_id.value());
    writer.u64(state.step_generation.value());
    writer.u32(state.current_step_index);
    writer.u64(state.next_step_generation.value());
    writer.u64(state.next_action_generation.value());
    writer.u64(state.next_declaration_sequence);
    writer.u64(state.next_commit_sequence);
    writer.u64(state.progress_generation.value());
    writer.u64(state.committed_actions);
    writer.boolean(state.checkpoint_due);
    writer.u64(state.commits_at_last_checkpoint);
    writer.u64(state.pending_checkpoint_binding.value());
    writer.u64(state.pending_checkpoint_generation.value());
    writer.boolean(state.has_assignment);
    writer.u64(state.current_assignment.assignment_id.value());
    writer.u64(state.current_assignment.assignment_generation.value());
    writer.u64(state.current_assignment.agent_id.value());
    writer.u64(state.current_assignment.agent_generation.value());
    writer.u64(state.current_assignment.agent_boot_id.value());
    writer.u64(state.current_assignment.scheduler_epoch.value());
    writer.u64(state.current_assignment.coordinator_epoch.value());
    writer.u64(state.current_assignment.dispatch_generation.value());
    writer.u64(state.current_assignment.work_id.value());
    writer.u64(state.current_assignment.work_generation.value());
    writer.u64(state.current_assignment.lease_id);
    writer.u64(state.current_assignment.lease_generation);
    writer.boolean(state.current_assignment.current);
    writer.boolean(state.has_budget);
    writer.u64(state.budget.budget_id.value());
    writer.u64(state.budget.generation.value());
    writer.u16(static_cast<std::uint16_t>(state.budget.outcome));
    writer.u64(state.budget.remaining_actions);
    writer.u64(state.budget.remaining_requests);
    writer.u64(state.budget.remaining_tokens);
    writer.u64(state.budget.remaining_tool_calls);
    writer.boolean(state.budget.limits_known);
    writer.u64(static_cast<std::uint64_t>(state.budget.expires_at_unix_millis));
    writer.u64(static_cast<std::uint64_t>(state.budget.observed_at_unix_millis));
    writer.text(state.budget.provenance);
    writer.u64(state.mutation_sequence);
    emit(kRecordRuntime, body);
    ++record_count;
  }

  for (const RunRecord& run : state.runs) {
    std::vector<std::uint8_t> body;
    Writer writer(body);
    writer.u64(run.id.value());
    writer.u64(run.generation.value());
    writer.u64(run.runtime_generation.value());
    writer.u64(run.progress_generation.value());
    writer.u64(run.committed_actions);
    writer.u32(run.steps);
    writer.boolean(run.terminal);
    writer.text(run.terminal_reason);
    writer.u64(static_cast<std::uint64_t>(run.started_at_unix_millis));
    writer.u64(static_cast<std::uint64_t>(run.ended_at_unix_millis));
    emit(kRecordRun, body);
    ++record_count;
  }
  for (const StepRecord& step : state.steps) {
    std::vector<std::uint8_t> body;
    Writer writer(body);
    writer.u64(step.id.value());
    writer.u64(step.generation.value());
    writer.u64(step.run_id.value());
    writer.u64(step.run_generation.value());
    writer.u16(static_cast<std::uint16_t>(step.progress));
    writer.u64(step.progress_generation.value());
    writer.u64(step.committed_actions);
    writer.u32(step.declared_actions);
    writer.u64(static_cast<std::uint64_t>(step.opened_at_unix_millis));
    writer.u64(static_cast<std::uint64_t>(step.closed_at_unix_millis));
    emit(kRecordStep, body);
    ++record_count;
  }
  for (const ActionRecord& action : state.actions) {
    std::vector<std::uint8_t> body;
    Writer writer(body);
    writer.u64(action.id.value());
    writer.u64(action.generation.value());
    writer.u64(action.run_id.value());
    writer.u64(action.run_generation.value());
    writer.u64(action.step_id.value());
    writer.u64(action.step_generation.value());
    writer.u16(static_cast<std::uint16_t>(action.kind));
    writer.text(action.label);
    writer.u16(static_cast<std::uint16_t>(action.side_effect));
    writer.text(action.model_target);
    writer.text(action.tool_name);
    writer.text(action.operation_key);
    writer.text(action.input);
    writer.u64(action.input_digest);
    writer.u32(action.max_output_bytes);
    writer.u64(action.memory_binding_id.value());
    writer.u64(action.memory_binding_generation.value());
    writer.u64(action.memory_external_generation.value());
    writer.boolean(action.requires_memory);
    writer.u64(action.policy_id.value());
    writer.u64(action.policy_generation.value());
    writer.u64(action.budget_id.value());
    writer.u64(action.budget_generation.value());
    writer.boolean(action.budget_required);
    writer.u64(action.model_binding_generation.value());
    writer.u64(action.model_backend_id.value());
    writer.u64(action.model_backend_generation.value());
    writer.u64(action.tool_binding_generation.value());
    writer.u64(action.tool_backend_id.value());
    writer.u64(action.tool_backend_generation.value());
    writer.u64(action.checkpoint_binding_id.value());
    writer.u64(action.checkpoint_generation.value());
    writer.u64(action.runtime_boot_id.value());
    writer.u64(action.runtime_epoch.value());
    writer.u64(action.coordinator_epoch.value());
    writer.u16(static_cast<std::uint16_t>(action.state));
    writer.u32(action.attempt_count);
    writer.u64(action.next_attempt_generation.value());
    writer.u64(action.current_attempt.value());
    writer.u64(action.current_attempt_generation.value());
    writer.boolean(action.has_retry_override);
    writer.u32(action.retry.max_attempts);
    writer.u64(action.retry.backoff_base_millis);
    writer.u64(action.retry.backoff_max_millis);
    writer.u32(action.retry.backoff_multiplier_percent);
    std::vector<std::uint64_t> retry_classes;
    for (const RetryClass retry_class : action.retry.retryable_classes) {
      retry_classes.push_back(static_cast<std::uint64_t>(retry_class));
    }
    writer.u64_vector(retry_classes);
    writer.boolean(action.retry.checkpoint_before_retry);
    writer.boolean(action.retry.same_target);
    writer.boolean(action.retry.require_budget_reenval);
    writer.boolean(action.retry.require_deadline_gate);
    writer.u64(action.retry.deadline_unix_millis);
    writer.boolean(action.retry.require_side_effect_safety);
    writer.boolean(action.retry.require_generation_reenval);
    writer.boolean(action.committed);
    writer.u64(action.commit_generation.value());
    writer.u64(action.accepted_result.value());
    writer.u64(action.accepted_completion_generation.value());
    writer.u16(static_cast<std::uint16_t>(action.completion_status));
    writer.u16(static_cast<std::uint16_t>(action.last_failure));
    writer.text(action.last_error);
    writer.u64(action.result_digest);
    writer.u32(action.completions_seen);
    std::vector<std::uint64_t> dependencies;
    for (const ActionId dependency : action.dependencies) {
      dependencies.push_back(dependency.value());
    }
    writer.u64_vector(dependencies);
    writer.boolean(action.requires_checkpoint);
    writer.text(action.provenance.origin);
    writer.text(action.provenance.detail);
    writer.u64(static_cast<std::uint64_t>(action.provenance.recorded_at_unix_millis));
    writer.u64(action.declaration_sequence);
    writer.u64(action.commit_sequence);
    writer.boolean(action.ambiguous);
    emit(kRecordAction, body);
    ++record_count;
  }
  for (const AttemptRecord& attempt : state.attempts) {
    std::vector<std::uint8_t> body;
    Writer writer(body);
    writer.u64(attempt.id.value());
    writer.u64(attempt.generation.value());
    writer.u64(attempt.action_id.value());
    writer.u64(attempt.action_generation.value());
    writer.u64(attempt.runtime_boot_id.value());
    writer.u64(attempt.runtime_epoch.value());
    writer.u64(attempt.coordinator_epoch.value());
    writer.u64(attempt.model_call_id.value());
    writer.u64(attempt.model_call_generation.value());
    writer.u64(attempt.tool_call_id.value());
    writer.u64(attempt.tool_call_generation.value());
    writer.u64(attempt.backend_id.value());
    writer.u64(attempt.backend_generation.value());
    writer.u16(static_cast<std::uint16_t>(attempt.state));
    writer.u16(static_cast<std::uint16_t>(attempt.completion_status));
    writer.u64(attempt.result_id.value());
    writer.u64(attempt.completion_generation.value());
    writer.u16(static_cast<std::uint16_t>(attempt.failure_class));
    writer.u64(attempt.result_digest);
    writer.boolean(attempt.superseded);
    writer.boolean(attempt.ambiguous);
    writer.boolean(attempt.current);
    writer.boolean(attempt.completion_recorded);
    writer.u64(static_cast<std::uint64_t>(attempt.dispatched_at_unix_millis));
    writer.u64(static_cast<std::uint64_t>(attempt.completed_at_unix_millis));
    writer.text(attempt.error);
    emit(kRecordAttempt, body);
    ++record_count;
  }
  for (const AssignmentRecord& record : state.assignments) {
    std::vector<std::uint8_t> body;
    Writer writer(body);
    writer.u64(record.authority.assignment_id.value());
    writer.u64(record.authority.assignment_generation.value());
    writer.u64(record.authority.agent_id.value());
    writer.u64(record.authority.agent_generation.value());
    writer.u64(record.authority.agent_boot_id.value());
    writer.u64(record.authority.scheduler_epoch.value());
    writer.u64(record.authority.coordinator_epoch.value());
    writer.u64(record.authority.dispatch_generation.value());
    writer.u64(record.authority.work_id.value());
    writer.u64(record.authority.work_generation.value());
    writer.u64(record.authority.lease_id);
    writer.u64(record.authority.lease_generation);
    writer.boolean(record.authority.current);
    writer.boolean(record.current);
    writer.text(record.reason);
    writer.u64(static_cast<std::uint64_t>(record.recorded_at_unix_millis));
    emit(kRecordAssignment, body);
    ++record_count;
  }
  for (const ModelBinding& binding : state.model_bindings) {
    std::vector<std::uint8_t> body;
    Writer writer(body);
    writer.text(binding.target);
    writer.u64(binding.binding_generation.value());
    writer.u64(binding.backend_id.value());
    writer.u64(binding.backend_generation.value());
    writer.text(binding.route_provenance);
    writer.boolean(binding.current);
    emit(kRecordModelBinding, body);
    ++record_count;
  }
  for (const ToolBinding& binding : state.tool_bindings) {
    std::vector<std::uint8_t> body;
    Writer writer(body);
    writer.text(binding.tool_name);
    writer.u64(binding.binding_generation.value());
    writer.u64(binding.backend_id.value());
    writer.u64(binding.backend_generation.value());
    writer.u16(static_cast<std::uint16_t>(binding.side_effect));
    writer.u32(binding.max_input_bytes);
    writer.text(binding.provenance);
    writer.boolean(binding.current);
    emit(kRecordToolBinding, body);
    ++record_count;
  }
  for (const MemoryBinding& binding : state.memory_bindings) {
    std::vector<std::uint8_t> body;
    Writer writer(body);
    write_binding(writer, binding);
    emit(kRecordMemoryBinding, body);
    ++record_count;
  }
  for (const CheckpointRecord& record : state.checkpoints) {
    std::vector<std::uint8_t> body;
    Writer writer(body);
    writer.u64(record.binding.binding_id.value());
    writer.u64(record.binding.generation.value());
    writer.u64(record.binding.runtime_id.value());
    writer.u64(record.binding.runtime_generation.value());
    writer.u64(record.binding.run_id.value());
    writer.u64(record.binding.run_generation.value());
    writer.u64(record.binding.step_id.value());
    writer.u64(record.binding.step_generation.value());
    writer.u64(record.binding.runtime_epoch.value());
    writer.u64(record.binding.progress_generation.value());
    writer.u64(record.binding.memory_generation.value());
    writer.text(record.binding.checkpoint_identity);
    writer.text(record.binding.integrity_digest);
    writer.text(record.binding.provenance);
    writer.boolean(record.binding.restorable);
    writer.boolean(record.binding.current);
    writer.u16(static_cast<std::uint16_t>(record.state));
    writer.u64(static_cast<std::uint64_t>(record.recorded_at_unix_millis));
    emit(kRecordCheckpoint, body);
    ++record_count;
  }
  for (const FencedBootRecord& record : state.fenced_boots) {
    std::vector<std::uint8_t> body;
    Writer writer(body);
    writer.u64(record.runtime_boot_id.value());
    writer.u64(record.agent_boot_id.value());
    writer.text(record.reason);
    writer.u64(static_cast<std::uint64_t>(record.recorded_at_unix_millis));
    emit(kRecordFencedBoot, body);
    ++record_count;
  }
  for (const RuntimePolicy& policy : state.policy_history) {
    std::vector<std::uint8_t> body;
    Writer writer(body);
    writer.u64(policy.policy_id.value());
    writer.u64(policy.generation.value());
    writer.u32(policy.max_retries_per_action);
    writer.u32(policy.max_parallel_actions);
    writer.u32(policy.max_parallel_read_actions);
    writer.u32(policy.max_parallel_tool_calls);
    writer.u32(policy.max_parallel_model_calls);
    writer.boolean(policy.allow_side_effects);
    writer.boolean(policy.allow_memory_mutation);
    writer.boolean(policy.allow_checkpoint);
    writer.boolean(policy.allow_non_repeatable_side_effects);
    writer.boolean(policy.allow_unknown_side_effects);
    writer.u32(policy.checkpoint_every_commits);
    writer.boolean(policy.require_budget_for_dispatch);
    writer.boolean(policy.require_assignment_for_dispatch);
    writer.boolean(policy.cancel_requires_reconciliation);
    writer.boolean(policy.drain_waits_for_authorized_actions);
    writer.u32(policy.drain_max_actions);
    std::vector<std::uint64_t> kinds;
    for (const ActionKind kind : policy.allowed_action_kinds) {
      kinds.push_back(static_cast<std::uint64_t>(kind));
    }
    writer.u64_vector(kinds);
    writer.u32(static_cast<std::uint32_t>(policy.allowed_tools.size()));
    for (const std::string& tool : policy.allowed_tools) {
      writer.text(tool);
    }
    writer.u32(static_cast<std::uint32_t>(policy.allowed_model_targets.size()));
    for (const std::string& target : policy.allowed_model_targets) {
      writer.text(target);
    }
    writer.u32(policy.retry.max_attempts);
    writer.u64(policy.retry.backoff_base_millis);
    writer.u64(policy.retry.backoff_max_millis);
    writer.u32(policy.retry.backoff_multiplier_percent);
    std::vector<std::uint64_t> retry_classes;
    for (const RetryClass retry_class : policy.retry.retryable_classes) {
      retry_classes.push_back(static_cast<std::uint64_t>(retry_class));
    }
    writer.u64_vector(retry_classes);
    writer.boolean(policy.retry.checkpoint_before_retry);
    writer.boolean(policy.retry.same_target);
    writer.boolean(policy.retry.require_budget_reenval);
    writer.boolean(policy.retry.require_deadline_gate);
    writer.u64(policy.retry.deadline_unix_millis);
    writer.boolean(policy.retry.require_side_effect_safety);
    writer.boolean(policy.retry.require_generation_reenval);
    emit(kRecordPolicy, body);
    ++record_count;
  }
  for (const BudgetEvidence& evidence : state.budget_history) {
    std::vector<std::uint8_t> body;
    Writer writer(body);
    writer.u64(evidence.budget_id.value());
    writer.u64(evidence.generation.value());
    writer.u16(static_cast<std::uint16_t>(evidence.outcome));
    writer.u64(evidence.remaining_actions);
    writer.u64(evidence.remaining_requests);
    writer.u64(evidence.remaining_tokens);
    writer.u64(evidence.remaining_tool_calls);
    writer.boolean(evidence.limits_known);
    writer.u64(static_cast<std::uint64_t>(evidence.expires_at_unix_millis));
    writer.u64(static_cast<std::uint64_t>(evidence.observed_at_unix_millis));
    writer.text(evidence.provenance);
    emit(kRecordBudget, body);
    ++record_count;
  }
  for (const Provenance& provenance : state.provenance_log) {
    std::vector<std::uint8_t> body;
    Writer writer(body);
    writer.text(provenance.origin);
    writer.text(provenance.detail);
    writer.u64(static_cast<std::uint64_t>(provenance.recorded_at_unix_millis));
    emit(kRecordProvenance, body);
    ++record_count;
  }

  // The header carries the SHA-256 semantic digest of canonical state, so a
  // decoded candidate is proven to reproduce exactly the recorded meaning.
  const std::string canonical = canonical_json(state);
  const auto semantic_digest = sha256(canonical);

  std::vector<std::uint8_t> document;
  document.reserve(persistence_header_bytes + payload.size() + persistence_trailer_bytes);
  const char* magic = persistence_magic.data();
  for (std::size_t i = 0; i < persistence_magic.size(); ++i) {
    document.push_back(static_cast<std::uint8_t>(magic[i]));
  }
  document.push_back(static_cast<std::uint8_t>(persistence_format_version & 0xFFu));
  document.push_back(static_cast<std::uint8_t>((persistence_format_version >> 8) & 0xFFu));
  document.push_back(static_cast<std::uint8_t>(persistence_header_bytes & 0xFFu));
  document.push_back(static_cast<std::uint8_t>((persistence_header_bytes >> 8) & 0xFFu));
  for (int i = 0; i < 4; ++i) {
    document.push_back(0);  // flags
  }
  for (int i = 0; i < 8; ++i) {
    document.push_back(static_cast<std::uint8_t>((payload.size() >> (8 * i)) & 0xFFu));
  }
  for (int i = 0; i < 4; ++i) {
    document.push_back(static_cast<std::uint8_t>((record_count >> (8 * i)) & 0xFFu));
  }
  for (int i = 0; i < 4; ++i) {
    document.push_back(0);  // reserved
  }
  document.insert(document.end(), semantic_digest.begin(), semantic_digest.end());
  const std::uint32_t header_crc =
      crc32c(std::span<const std::uint8_t>(document.data(), persistence_header_bytes - 4));
  for (int i = 0; i < 4; ++i) {
    document.push_back(static_cast<std::uint8_t>((header_crc >> (8 * i)) & 0xFFu));
  }
  document.insert(document.end(), payload.begin(), payload.end());
  const std::uint32_t payload_crc =
      crc32c(std::span<const std::uint8_t>(payload.data(), payload.size()));
  for (std::size_t i = 0; i < persistence_trailer_magic.size(); ++i) {
    document.push_back(static_cast<std::uint8_t>(persistence_trailer_magic[i]));
  }
  for (int i = 0; i < 4; ++i) {
    document.push_back(static_cast<std::uint8_t>((payload_crc >> (8 * i)) & 0xFFu));
  }
  const std::uint32_t trailer_crc = crc32c(
      std::span<const std::uint8_t>(document.data() + document.size() - 8, 8));
  for (int i = 0; i < 4; ++i) {
    document.push_back(static_cast<std::uint8_t>((trailer_crc >> (8 * i)) & 0xFFu));
  }
  (void)limits;
  return document;
}

DecodeResult decode_document(std::span<const std::uint8_t> bytes, const ResourceLimits& limits) {
  DecodeResult result;

  if (bytes.size() < persistence_header_bytes + persistence_trailer_bytes) {
    result.code = OutcomeCode::REJECT_INVALID;
    result.message = "file is shorter than the minimum durable document size";
    return result;
  }
  if (bytes.size() > limits.max_persistence_bytes) {
    result.code = OutcomeCode::REJECT_LIMIT;
    result.message = "file exceeds the configured persistence byte limit";
    return result;
  }
  for (std::size_t i = 0; i < persistence_magic.size(); ++i) {
    if (bytes[i] != static_cast<std::uint8_t>(persistence_magic[i])) {
      result.code = OutcomeCode::REJECT_INVALID;
      result.message = "durable document magic does not match";
      return result;
    }
  }
  const std::uint16_t version =
      static_cast<std::uint16_t>(bytes[4]) | static_cast<std::uint16_t>(bytes[5] << 8);
  result.info.format_version = version;
  if (version != persistence_format_version) {
    result.code = OutcomeCode::REJECT_INVALID;
    result.message = "unsupported persistence format version " + std::to_string(version) +
                     " (this build supports " +
                     std::to_string(persistence_format_version) + ")";
    return result;
  }
  const std::uint16_t header_bytes =
      static_cast<std::uint16_t>(bytes[6]) | static_cast<std::uint16_t>(bytes[7] << 8);
  if (header_bytes != persistence_header_bytes) {
    result.code = OutcomeCode::REJECT_INVALID;
    result.message = "durable document header size does not match the supported layout";
    return result;
  }
  std::uint64_t payload_bytes = 0;
  for (int i = 0; i < 8; ++i) {
    payload_bytes |= static_cast<std::uint64_t>(bytes[12 + i]) << (8 * i);
  }
  std::uint32_t record_count = 0;
  for (int i = 0; i < 4; ++i) {
    record_count |= static_cast<std::uint32_t>(bytes[20 + i]) << (8 * i);
  }
  result.info.record_count = record_count;
  result.info.payload_bytes = payload_bytes;
  result.info.file_bytes = bytes.size();

  const std::uint64_t expected_size = static_cast<std::uint64_t>(persistence_header_bytes) +
                                      payload_bytes + persistence_trailer_bytes;
  if (expected_size != bytes.size()) {
    result.code = OutcomeCode::REJECT_INVALID;
    result.message = "declared payload length does not match the file length";
    return result;
  }
  const std::uint32_t stored_header_crc = [&bytes] {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      value |= static_cast<std::uint32_t>(bytes[persistence_header_bytes - 4 + i]) << (8 * i);
    }
    return value;
  }();
  const std::uint32_t computed_header_crc = crc32c(
      std::span<const std::uint8_t>(bytes.data(), persistence_header_bytes - 4));
  if (stored_header_crc != computed_header_crc) {
    result.code = OutcomeCode::REJECT_INVALID;
    result.message = "header integrity check failed";
    return result;
  }
  for (std::size_t i = 0; i < persistence_trailer_magic.size(); ++i) {
    if (bytes[bytes.size() - persistence_trailer_bytes + i] !=
        static_cast<std::uint8_t>(persistence_trailer_magic[i])) {
      result.code = OutcomeCode::REJECT_INVALID;
      result.message = "durable document trailer marker is missing";
      return result;
    }
  }
  const std::span<const std::uint8_t> payload(bytes.data() + persistence_header_bytes,
                                              static_cast<std::size_t>(payload_bytes));
  std::uint32_t stored_payload_crc = 0;
  for (int i = 0; i < 4; ++i) {
    stored_payload_crc |=
        static_cast<std::uint32_t>(bytes[bytes.size() - 8 + i]) << (8 * i);
  }
  if (stored_payload_crc != crc32c(payload)) {
    result.code = OutcomeCode::REJECT_INVALID;
    result.message = "payload integrity check failed";
    return result;
  }
  std::uint32_t stored_trailer_crc = 0;
  for (int i = 0; i < 4; ++i) {
    stored_trailer_crc |=
        static_cast<std::uint32_t>(bytes[bytes.size() - 4 + i]) << (8 * i);
  }
  if (stored_trailer_crc !=
      crc32c(std::span<const std::uint8_t>(
          bytes.data() + bytes.size() - persistence_trailer_bytes, 8))) {
    result.code = OutcomeCode::REJECT_INVALID;
    result.message = "trailer integrity check failed";
    return result;
  }
  std::array<std::uint8_t, Sha256::digest_bytes> stored_digest{};
  std::copy_n(bytes.begin() + 28, Sha256::digest_bytes, stored_digest.begin());
  result.info.semantic_digest_hex =
      hex_encode(std::span<const std::uint8_t>(stored_digest.data(), stored_digest.size()));
  std::array<std::uint8_t, 4> payload_crc_bytes{};
  for (int i = 0; i < 4; ++i) {
    payload_crc_bytes[static_cast<std::size_t>(i)] =
        static_cast<std::uint8_t>((stored_payload_crc >> (8 * i)) & 0xFFu);
  }
  result.info.payload_crc32c_hex = hex_encode(
      std::span<const std::uint8_t>(payload_crc_bytes.data(), payload_crc_bytes.size()));

  CanonicalState state;
  Reader reader(payload, limits);
  std::uint32_t decoded_records = 0;
  bool runtime_record_seen = false;

  while (reader.remaining() != 0) {
    if (decoded_records >= record_count) {
      result.code = OutcomeCode::REJECT_INVALID;
      result.message = "trailing garbage after the declared record count";
      return result;
    }
    const std::uint16_t type = reader.u16();
    const std::uint32_t body_bytes = reader.u32();
    if (!reader.ok()) {
      result.code = OutcomeCode::REJECT_INVALID;
      result.message = "record header is truncated: " + reader.error();
      return result;
    }
    if (body_bytes > reader.remaining()) {
      result.code = OutcomeCode::REJECT_INVALID;
      result.message = "record body length exceeds the remaining payload";
      return result;
    }
    if (type == 0 || type > static_cast<std::uint16_t>(kRecordTypeMax)) {
      result.code = OutcomeCode::REJECT_INVALID;
      result.message = "unknown record type " + std::to_string(type);
      return result;
    }
    const std::span<const std::uint8_t> body(payload.data() + (payload.size() - reader.remaining()),
                                            body_bytes);
    Reader record(body, limits);
    switch (type) {
      case kRecordRuntime: {
        if (runtime_record_seen) {
          result.code = OutcomeCode::REJECT_INVALID;
          result.message = "duplicate runtime record";
          return result;
        }
        runtime_record_seen = true;
        state.runtime_id = AgentRuntimeId(record.u64());
        state.runtime_generation = AgentRuntimeGeneration(record.u64());
        state.runtime_boot_id = RuntimeBootId(record.u64());
        state.agent_id = AgentId(record.u64());
        state.agent_generation = AgentGeneration(record.u64());
        state.agent_boot_id = AgentBootId(record.u64());
        state.runtime_epoch = RuntimeEpoch(record.u64());
        state.coordinator_epoch = CoordinatorEpoch(record.u64());
        const std::uint16_t lifecycle = record.u16();
        const std::uint16_t cancellation = record.u16();
        const std::uint16_t recovery = record.u16();
        if (lifecycle >= static_cast<std::uint16_t>(RuntimeLifecycle::kCount) ||
            cancellation >= static_cast<std::uint16_t>(CancellationState::kCount) ||
            recovery >= static_cast<std::uint16_t>(RecoveryStatus::kCount)) {
          result.code = OutcomeCode::REJECT_INVALID;
          result.message = "runtime record contains an out-of-range enumeration value";
          return result;
        }
        state.lifecycle = static_cast<RuntimeLifecycle>(lifecycle);
        state.cancellation = static_cast<CancellationState>(cancellation);
        state.recovery = static_cast<RecoveryStatus>(recovery);
        state.terminal_reason = record.text();
        state.cancellation_reason = record.text();
        state.recovery_reason = record.text();
        state.run_id = AgentRunId(record.u64());
        state.run_generation = AgentRunGeneration(record.u64());
        state.current_run_index = record.u32();
        state.step_id = StepId(record.u64());
        state.step_generation = StepGeneration(record.u64());
        state.current_step_index = record.u32();
        state.next_step_generation = StepGeneration(record.u64());
        state.next_action_generation = ActionGeneration(record.u64());
        state.next_declaration_sequence = record.u64();
        state.next_commit_sequence = record.u64();
        state.progress_generation = ProgressGeneration(record.u64());
        state.committed_actions = record.u64();
        state.checkpoint_due = record.boolean();
        state.commits_at_last_checkpoint = record.u64();
        state.pending_checkpoint_binding = CheckpointBindingId(record.u64());
        state.pending_checkpoint_generation = CheckpointGeneration(record.u64());
        state.has_assignment = record.boolean();
        state.current_assignment.assignment_id = SchedulerAssignmentId(record.u64());
        state.current_assignment.assignment_generation = AssignmentGeneration(record.u64());
        state.current_assignment.agent_id = AgentId(record.u64());
        state.current_assignment.agent_generation = AgentGeneration(record.u64());
        state.current_assignment.agent_boot_id = AgentBootId(record.u64());
        state.current_assignment.scheduler_epoch = Generation<CoordinatorEpochTag>(record.u64());
        state.current_assignment.coordinator_epoch = CoordinatorEpoch(record.u64());
        state.current_assignment.dispatch_generation = DispatchGeneration(record.u64());
        state.current_assignment.work_id = WorkId(record.u64());
        state.current_assignment.work_generation = WorkGeneration(record.u64());
        state.current_assignment.lease_id = record.u64();
        state.current_assignment.lease_generation = record.u64();
        state.current_assignment.current = record.boolean();
        state.has_budget = record.boolean();
        state.budget.budget_id = BudgetId(record.u64());
        state.budget.generation = BudgetGeneration(record.u64());
        const std::uint16_t budget_outcome = record.u16();
        if (budget_outcome >= static_cast<std::uint16_t>(BudgetOutcome::kCount)) {
          result.code = OutcomeCode::REJECT_INVALID;
          result.message = "runtime record contains an invalid budget outcome";
          return result;
        }
        state.budget.outcome = static_cast<BudgetOutcome>(budget_outcome);
        state.budget.remaining_actions = record.u64();
        state.budget.remaining_requests = record.u64();
        state.budget.remaining_tokens = record.u64();
        state.budget.remaining_tool_calls = record.u64();
        state.budget.limits_known = record.boolean();
        state.budget.expires_at_unix_millis = static_cast<UnixMillis>(record.u64());
        state.budget.observed_at_unix_millis = static_cast<UnixMillis>(record.u64());
        state.budget.provenance = record.text();
        state.mutation_sequence = record.u64();
        break;
      }
      case kRecordRun: {
        RunRecord run;
        run.id = AgentRunId(record.u64());
        run.generation = AgentRunGeneration(record.u64());
        run.runtime_generation = AgentRuntimeGeneration(record.u64());
        run.progress_generation = ProgressGeneration(record.u64());
        run.committed_actions = record.u64();
        run.steps = record.u32();
        run.terminal = record.boolean();
        run.terminal_reason = record.text();
        run.started_at_unix_millis = static_cast<UnixMillis>(record.u64());
        run.ended_at_unix_millis = static_cast<UnixMillis>(record.u64());
        state.runs.push_back(std::move(run));
        break;
      }
      case kRecordStep: {
        StepRecord step;
        step.id = StepId(record.u64());
        step.generation = StepGeneration(record.u64());
        step.run_id = AgentRunId(record.u64());
        step.run_generation = AgentRunGeneration(record.u64());
        const std::uint16_t progress = record.u16();
        if (progress >= static_cast<std::uint16_t>(ProgressState::kCount)) {
          result.code = OutcomeCode::REJECT_INVALID;
          result.message = "step record contains an invalid progress state";
          return result;
        }
        step.progress = static_cast<ProgressState>(progress);
        step.progress_generation = ProgressGeneration(record.u64());
        step.committed_actions = record.u64();
        step.declared_actions = record.u32();
        step.opened_at_unix_millis = static_cast<UnixMillis>(record.u64());
        step.closed_at_unix_millis = static_cast<UnixMillis>(record.u64());
        state.steps.push_back(std::move(step));
        break;
      }
      case kRecordAction: {
        ActionRecord action;
        action.id = ActionId(record.u64());
        action.generation = ActionGeneration(record.u64());
        action.run_id = AgentRunId(record.u64());
        action.run_generation = AgentRunGeneration(record.u64());
        action.step_id = StepId(record.u64());
        action.step_generation = StepGeneration(record.u64());
        const std::uint16_t kind = record.u16();
        if (kind >= static_cast<std::uint16_t>(ActionKind::kCount)) {
          result.code = OutcomeCode::REJECT_INVALID;
          result.message = "action record contains an invalid action kind";
          return result;
        }
        action.kind = static_cast<ActionKind>(kind);
        action.label = record.text();
        const std::uint16_t side_effect = record.u16();
        if (side_effect >= static_cast<std::uint16_t>(SideEffectClass::kCount)) {
          result.code = OutcomeCode::REJECT_INVALID;
          result.message = "action record contains an invalid side-effect class";
          return result;
        }
        action.side_effect = static_cast<SideEffectClass>(side_effect);
        action.model_target = record.text();
        action.tool_name = record.text();
        action.operation_key = record.text();
        action.input = record.text();
        action.input_digest = record.u64();
        action.max_output_bytes = record.u32();
        action.memory_binding_id = MemoryBindingId(record.u64());
        action.memory_binding_generation = MemoryBindingGeneration(record.u64());
        action.memory_external_generation = Generation<MemoryBindingTag>(record.u64());
        action.requires_memory = record.boolean();
        action.policy_id = PolicyId(record.u64());
        action.policy_generation = PolicyGeneration(record.u64());
        action.budget_id = BudgetId(record.u64());
        action.budget_generation = BudgetGeneration(record.u64());
        action.budget_required = record.boolean();
        action.model_binding_generation = ModelCallGeneration(record.u64());
        action.model_backend_id = BackendId(record.u64());
        action.model_backend_generation = Generation<BackendTag>(record.u64());
        action.tool_binding_generation = ToolCallGeneration(record.u64());
        action.tool_backend_id = BackendId(record.u64());
        action.tool_backend_generation = Generation<BackendTag>(record.u64());
        action.checkpoint_binding_id = CheckpointBindingId(record.u64());
        action.checkpoint_generation = CheckpointGeneration(record.u64());
        action.runtime_boot_id = RuntimeBootId(record.u64());
        action.runtime_epoch = RuntimeEpoch(record.u64());
        action.coordinator_epoch = CoordinatorEpoch(record.u64());
        const std::uint16_t action_state = record.u16();
        if (action_state >= static_cast<std::uint16_t>(ActionState::kCount)) {
          result.code = OutcomeCode::REJECT_INVALID;
          result.message = "action record contains an invalid action state";
          return result;
        }
        action.state = static_cast<ActionState>(action_state);
        action.attempt_count = record.u32();
        action.next_attempt_generation = AttemptGeneration(record.u64());
        action.current_attempt = ActionAttemptId(record.u64());
        action.current_attempt_generation = AttemptGeneration(record.u64());
        action.has_retry_override = record.boolean();
        action.retry.max_attempts = record.u32();
        action.retry.backoff_base_millis = record.u64();
        action.retry.backoff_max_millis = record.u64();
        action.retry.backoff_multiplier_percent = record.u32();
        action.retry.retryable_classes.clear();
        for (const std::uint64_t value : record.u64_vector()) {
          if (value >= static_cast<std::uint64_t>(RetryClass::kCount)) {
            result.code = OutcomeCode::REJECT_INVALID;
            result.message = "action retry policy contains an invalid retry class";
            return result;
          }
          action.retry.retryable_classes.insert(static_cast<RetryClass>(value));
        }
        action.retry.checkpoint_before_retry = record.boolean();
        action.retry.same_target = record.boolean();
        action.retry.require_budget_reenval = record.boolean();
        action.retry.require_deadline_gate = record.boolean();
        action.retry.deadline_unix_millis = record.u64();
        action.retry.require_side_effect_safety = record.boolean();
        action.retry.require_generation_reenval = record.boolean();
        action.committed = record.boolean();
        action.commit_generation = CommitGeneration(record.u64());
        action.accepted_result = ResultId(record.u64());
        action.accepted_completion_generation = CompletionGeneration(record.u64());
        const std::uint16_t completion_status = record.u16();
        if (completion_status >= static_cast<std::uint16_t>(CompletionStatus::kCount)) {
          result.code = OutcomeCode::REJECT_INVALID;
          result.message = "action record contains an invalid completion status";
          return result;
        }
        action.completion_status = static_cast<CompletionStatus>(completion_status);
        const std::uint16_t last_failure = record.u16();
        if (last_failure >= static_cast<std::uint16_t>(RetryClass::kCount)) {
          result.code = OutcomeCode::REJECT_INVALID;
          result.message = "action record contains an invalid failure class";
          return result;
        }
        action.last_failure = static_cast<RetryClass>(last_failure);
        action.last_error = record.text();
        action.result_digest = record.u64();
        action.completions_seen = record.u32();
        for (const std::uint64_t value : record.u64_vector()) {
          action.dependencies.push_back(ActionId(value));
        }
        action.requires_checkpoint = record.boolean();
        action.provenance.origin = record.text();
        action.provenance.detail = record.text();
        action.provenance.recorded_at_unix_millis = static_cast<UnixMillis>(record.u64());
        action.declaration_sequence = record.u64();
        action.commit_sequence = record.u64();
        action.ambiguous = record.boolean();
        state.actions.push_back(std::move(action));
        break;
      }
      case kRecordAttempt: {
        AttemptRecord attempt;
        attempt.id = ActionAttemptId(record.u64());
        attempt.generation = AttemptGeneration(record.u64());
        attempt.action_id = ActionId(record.u64());
        attempt.action_generation = ActionGeneration(record.u64());
        attempt.runtime_boot_id = RuntimeBootId(record.u64());
        attempt.runtime_epoch = RuntimeEpoch(record.u64());
        attempt.coordinator_epoch = CoordinatorEpoch(record.u64());
        attempt.model_call_id = ModelCallId(record.u64());
        attempt.model_call_generation = ModelCallGeneration(record.u64());
        attempt.tool_call_id = ToolCallId(record.u64());
        attempt.tool_call_generation = ToolCallGeneration(record.u64());
        attempt.backend_id = BackendId(record.u64());
        attempt.backend_generation = Generation<BackendTag>(record.u64());
        const std::uint16_t attempt_state = record.u16();
        if (attempt_state >= static_cast<std::uint16_t>(ActionState::kCount)) {
          result.code = OutcomeCode::REJECT_INVALID;
          result.message = "attempt record contains an invalid action state";
          return result;
        }
        attempt.state = static_cast<ActionState>(attempt_state);
        const std::uint16_t completion_status = record.u16();
        if (completion_status >= static_cast<std::uint16_t>(CompletionStatus::kCount)) {
          result.code = OutcomeCode::REJECT_INVALID;
          result.message = "attempt record contains an invalid completion status";
          return result;
        }
        attempt.completion_status = static_cast<CompletionStatus>(completion_status);
        attempt.result_id = ResultId(record.u64());
        attempt.completion_generation = CompletionGeneration(record.u64());
        const std::uint16_t failure_class = record.u16();
        if (failure_class >= static_cast<std::uint16_t>(RetryClass::kCount)) {
          result.code = OutcomeCode::REJECT_INVALID;
          result.message = "attempt record contains an invalid failure class";
          return result;
        }
        attempt.failure_class = static_cast<RetryClass>(failure_class);
        attempt.result_digest = record.u64();
        attempt.superseded = record.boolean();
        attempt.ambiguous = record.boolean();
        attempt.current = record.boolean();
        attempt.completion_recorded = record.boolean();
        attempt.dispatched_at_unix_millis = static_cast<UnixMillis>(record.u64());
        attempt.completed_at_unix_millis = static_cast<UnixMillis>(record.u64());
        attempt.error = record.text();
        state.attempts.push_back(std::move(attempt));
        break;
      }
      case kRecordAssignment: {
        AssignmentRecord record_value;
        record_value.authority.assignment_id = SchedulerAssignmentId(record.u64());
        record_value.authority.assignment_generation = AssignmentGeneration(record.u64());
        record_value.authority.agent_id = AgentId(record.u64());
        record_value.authority.agent_generation = AgentGeneration(record.u64());
        record_value.authority.agent_boot_id = AgentBootId(record.u64());
        record_value.authority.scheduler_epoch = Generation<CoordinatorEpochTag>(record.u64());
        record_value.authority.coordinator_epoch = CoordinatorEpoch(record.u64());
        record_value.authority.dispatch_generation = DispatchGeneration(record.u64());
        record_value.authority.work_id = WorkId(record.u64());
        record_value.authority.work_generation = WorkGeneration(record.u64());
        record_value.authority.lease_id = record.u64();
        record_value.authority.lease_generation = record.u64();
        record_value.authority.current = record.boolean();
        record_value.current = record.boolean();
        record_value.reason = record.text();
        record_value.recorded_at_unix_millis = static_cast<UnixMillis>(record.u64());
        state.assignments.push_back(std::move(record_value));
        break;
      }
      case kRecordModelBinding: {
        ModelBinding binding;
        binding.target = record.text();
        binding.binding_generation = ModelCallGeneration(record.u64());
        binding.backend_id = BackendId(record.u64());
        binding.backend_generation = Generation<BackendTag>(record.u64());
        binding.route_provenance = record.text();
        binding.current = record.boolean();
        state.model_bindings.push_back(std::move(binding));
        break;
      }
      case kRecordToolBinding: {
        ToolBinding binding;
        binding.tool_name = record.text();
        binding.binding_generation = ToolCallGeneration(record.u64());
        binding.backend_id = BackendId(record.u64());
        binding.backend_generation = Generation<BackendTag>(record.u64());
        const std::uint16_t side_effect = record.u16();
        if (side_effect >= static_cast<std::uint16_t>(SideEffectClass::kCount)) {
          result.code = OutcomeCode::REJECT_INVALID;
          result.message = "tool binding contains an invalid side-effect class";
          return result;
        }
        binding.side_effect = static_cast<SideEffectClass>(side_effect);
        binding.max_input_bytes = record.u32();
        binding.provenance = record.text();
        binding.current = record.boolean();
        state.tool_bindings.push_back(std::move(binding));
        break;
      }
      case kRecordMemoryBinding: {
        state.memory_bindings.push_back(read_binding(record));
        break;
      }
      case kRecordCheckpoint: {
        CheckpointRecord checkpoint;
        checkpoint.binding.binding_id = CheckpointBindingId(record.u64());
        checkpoint.binding.generation = CheckpointGeneration(record.u64());
        checkpoint.binding.runtime_id = AgentRuntimeId(record.u64());
        checkpoint.binding.runtime_generation = AgentRuntimeGeneration(record.u64());
        checkpoint.binding.run_id = AgentRunId(record.u64());
        checkpoint.binding.run_generation = AgentRunGeneration(record.u64());
        checkpoint.binding.step_id = StepId(record.u64());
        checkpoint.binding.step_generation = StepGeneration(record.u64());
        checkpoint.binding.runtime_epoch = RuntimeEpoch(record.u64());
        checkpoint.binding.progress_generation = ProgressGeneration(record.u64());
        checkpoint.binding.memory_generation = MemoryBindingGeneration(record.u64());
        checkpoint.binding.checkpoint_identity = record.text();
        checkpoint.binding.integrity_digest = record.text();
        checkpoint.binding.provenance = record.text();
        checkpoint.binding.restorable = record.boolean();
        checkpoint.binding.current = record.boolean();
        const std::uint16_t checkpoint_state = record.u16();
        if (checkpoint_state >= static_cast<std::uint16_t>(CheckpointState::kCount)) {
          result.code = OutcomeCode::REJECT_INVALID;
          result.message = "checkpoint record contains an invalid checkpoint state";
          return result;
        }
        checkpoint.state = static_cast<CheckpointState>(checkpoint_state);
        checkpoint.recorded_at_unix_millis = static_cast<UnixMillis>(record.u64());
        state.checkpoints.push_back(std::move(checkpoint));
        break;
      }
      case kRecordFencedBoot: {
        FencedBootRecord fenced;
        fenced.runtime_boot_id = RuntimeBootId(record.u64());
        fenced.agent_boot_id = AgentBootId(record.u64());
        fenced.reason = record.text();
        fenced.recorded_at_unix_millis = static_cast<UnixMillis>(record.u64());
        state.fenced_boots.push_back(std::move(fenced));
        break;
      }
      case kRecordPolicy: {
        RuntimePolicy policy;
        policy.policy_id = PolicyId(record.u64());
        policy.generation = PolicyGeneration(record.u64());
        policy.max_retries_per_action = record.u32();
        policy.max_parallel_actions = record.u32();
        policy.max_parallel_read_actions = record.u32();
        policy.max_parallel_tool_calls = record.u32();
        policy.max_parallel_model_calls = record.u32();
        policy.allow_side_effects = record.boolean();
        policy.allow_memory_mutation = record.boolean();
        policy.allow_checkpoint = record.boolean();
        policy.allow_non_repeatable_side_effects = record.boolean();
        policy.allow_unknown_side_effects = record.boolean();
        policy.checkpoint_every_commits = record.u32();
        policy.require_budget_for_dispatch = record.boolean();
        policy.require_assignment_for_dispatch = record.boolean();
        policy.cancel_requires_reconciliation = record.boolean();
        policy.drain_waits_for_authorized_actions = record.boolean();
        policy.drain_max_actions = record.u32();
        policy.allowed_action_kinds.clear();
        for (const std::uint64_t value : record.u64_vector()) {
          if (value >= static_cast<std::uint64_t>(ActionKind::kCount)) {
            result.code = OutcomeCode::REJECT_INVALID;
            result.message = "policy record contains an invalid action kind";
            return result;
          }
          policy.allowed_action_kinds.insert(static_cast<ActionKind>(value));
        }
        const std::uint32_t tool_count = record.u32();
        if (tool_count > limits.max_collection_entries) {
          result.code = OutcomeCode::REJECT_LIMIT;
          result.message = "policy tool list exceeds the configured limit";
          return result;
        }
        for (std::uint32_t i = 0; i < tool_count; ++i) {
          policy.allowed_tools.insert(record.text());
        }
        const std::uint32_t target_count = record.u32();
        if (target_count > limits.max_collection_entries) {
          result.code = OutcomeCode::REJECT_LIMIT;
          result.message = "policy model target list exceeds the configured limit";
          return result;
        }
        for (std::uint32_t i = 0; i < target_count; ++i) {
          policy.allowed_model_targets.insert(record.text());
        }
        policy.retry.max_attempts = record.u32();
        policy.retry.backoff_base_millis = record.u64();
        policy.retry.backoff_max_millis = record.u64();
        policy.retry.backoff_multiplier_percent = record.u32();
        policy.retry.retryable_classes.clear();
        for (const std::uint64_t value : record.u64_vector()) {
          if (value >= static_cast<std::uint64_t>(RetryClass::kCount)) {
            result.code = OutcomeCode::REJECT_INVALID;
            result.message = "policy retry classes contain an invalid class";
            return result;
          }
          policy.retry.retryable_classes.insert(static_cast<RetryClass>(value));
        }
        policy.retry.checkpoint_before_retry = record.boolean();
        policy.retry.same_target = record.boolean();
        policy.retry.require_budget_reenval = record.boolean();
        policy.retry.require_deadline_gate = record.boolean();
        policy.retry.deadline_unix_millis = record.u64();
        policy.retry.require_side_effect_safety = record.boolean();
        policy.retry.require_generation_reenval = record.boolean();
        state.policy_history.push_back(std::move(policy));
        break;
      }
      case kRecordBudget: {
        BudgetEvidence evidence;
        evidence.budget_id = BudgetId(record.u64());
        evidence.generation = BudgetGeneration(record.u64());
        const std::uint16_t budget_outcome = record.u16();
        if (budget_outcome >= static_cast<std::uint16_t>(BudgetOutcome::kCount)) {
          result.code = OutcomeCode::REJECT_INVALID;
          result.message = "budget record contains an invalid outcome";
          return result;
        }
        evidence.outcome = static_cast<BudgetOutcome>(budget_outcome);
        evidence.remaining_actions = record.u64();
        evidence.remaining_requests = record.u64();
        evidence.remaining_tokens = record.u64();
        evidence.remaining_tool_calls = record.u64();
        evidence.limits_known = record.boolean();
        evidence.expires_at_unix_millis = static_cast<UnixMillis>(record.u64());
        evidence.observed_at_unix_millis = static_cast<UnixMillis>(record.u64());
        evidence.provenance = record.text();
        state.budget_history.push_back(std::move(evidence));
        break;
      }
      case kRecordProvenance: {
        Provenance provenance;
        provenance.origin = record.text();
        provenance.detail = record.text();
        provenance.recorded_at_unix_millis = static_cast<UnixMillis>(record.u64());
        state.provenance_log.push_back(std::move(provenance));
        break;
      }
      default: {
        result.code = OutcomeCode::REJECT_INVALID;
        result.message = "unknown record type " + std::to_string(type);
        return result;
      }
    }
    if (!record.ok()) {
      result.code = OutcomeCode::REJECT_INVALID;
      result.message = "record body could not be decoded: " + record.error();
      return result;
    }
    if (record.remaining() != 0) {
      result.code = OutcomeCode::REJECT_INVALID;
      result.message = "record body has trailing bytes";
      return result;
    }
    reader.skip(body_bytes);
    ++decoded_records;
  }

  if (decoded_records != record_count) {
    result.code = OutcomeCode::REJECT_INVALID;
    result.message = "decoded record count does not match the declared record count";
    return result;
  }
  if (!runtime_record_seen) {
    result.code = OutcomeCode::REJECT_INVALID;
    result.message = "durable document contains no runtime record";
    return result;
  }
  if (state.current_run_index != kInvalidIndex && state.current_run_index >= state.runs.size()) {
    result.code = OutcomeCode::REJECT_INVALID;
    result.message = "current run index is out of range";
    return result;
  }
  if (state.current_step_index != kInvalidIndex &&
      state.current_step_index >= state.steps.size()) {
    result.code = OutcomeCode::REJECT_INVALID;
    result.message = "current step index is out of range";
    return result;
  }
  if (!state.policy_history.empty()) {
    state.policy = state.policy_history.back();
  } else {
    state.policy = RuntimePolicy::permissive();
    state.policy_history.push_back(state.policy);
  }

  Indexes indexes;
  indexes.rebuild(state);
  const InvariantReport report = check_canonical_invariants(state, indexes, limits);
  if (!report.ok) {
    result.code = OutcomeCode::REJECT_INVALID;
    result.subject = "persistence";
    result.message = "decoded candidate state violates invariants: " +
                     (report.violations.empty() ? std::string("unknown")
                                                : report.violations.front().name + ": " +
                                                      report.violations.front().detail);
    return result;
  }

  // The semantic digest of the decoded candidate must equal the stored digest.
  const std::string json = canonical_json(state);
  const auto digest = sha256(json);
  if (digest != stored_digest) {
    result.code = OutcomeCode::REJECT_INVALID;
    result.message = "decoded candidate state digest does not match the stored digest";
    return result;
  }
  result.info.semantic_digest_hex = hex_encode(
      std::span<const std::uint8_t>(stored_digest.data(), stored_digest.size()));
  result.document_json = json;
  result.state = std::move(state);
  result.code = OutcomeCode::ACCEPTED;
  result.subject = "persistence";
  result.message = "durable document decoded and validated";
  return result;
}

#ifdef _WIN32
namespace {

[[nodiscard]] std::wstring widen(const std::string& text) {
  if (text.empty()) {
    return {};
  }
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                       nullptr, 0);
  if (size <= 0) {
    return {};
  }
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), size);
  return wide;
}

[[nodiscard]] std::wstring extended_prefix(const std::wstring& path) {
  if (path.size() < 240) {
    return path;
  }
  const wchar_t backslash = static_cast<wchar_t>(0x5C);
  std::wstring prefix;
  prefix.push_back(backslash);
  prefix.push_back(backslash);
  prefix.push_back(L'?');
  prefix.push_back(backslash);
  if (path.size() >= 2 && path[0] == backslash && path[1] == backslash) {
    prefix += L"UNC";
    prefix.push_back(backslash);
    return prefix + path.substr(2);
  }
  return prefix + path;
}

}  // namespace
#endif

bool read_file_bounded(const std::string& path, std::uint64_t max_bytes,
                       std::vector<std::uint8_t>& out, std::string& error) {
#ifdef _WIN32
  const std::wstring wide = extended_prefix(widen(path));
  if (wide.empty()) {
    error = "path is empty or is not valid UTF-8";
    return false;
  }
  HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    error = "cannot open file for reading (windows error " +
            std::to_string(GetLastError()) + ")";
    return false;
  }
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(handle, &size)) {
    error = "cannot determine file size";
    CloseHandle(handle);
    return false;
  }
  if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > max_bytes) {
    error = "file exceeds the configured byte limit";
    CloseHandle(handle);
    return false;
  }
  out.assign(static_cast<std::size_t>(size.QuadPart), 0);
  std::size_t offset = 0;
  while (offset < out.size()) {
    DWORD read = 0;
    const DWORD request =
        static_cast<DWORD>(std::min<std::size_t>(out.size() - offset, 1u << 20));
    if (!ReadFile(handle, out.data() + offset, request, &read, nullptr)) {
      error = "read failed";
      CloseHandle(handle);
      return false;
    }
    if (read == 0) {
      break;
    }
    offset += read;
  }
  CloseHandle(handle);
  if (offset != out.size()) {
    error = "file was truncated while being read";
    return false;
  }
  return true;
#else
  FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    error = "cannot open file for reading";
    return false;
  }
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  if (size < 0 || static_cast<std::uint64_t>(size) > max_bytes) {
    error = "file exceeds the configured byte limit";
    std::fclose(file);
    return false;
  }
  out.assign(static_cast<std::size_t>(size), 0);
  const std::size_t read = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), file);
  std::fclose(file);
  if (read != out.size()) {
    error = "file was truncated while being read";
    return false;
  }
  return true;
#endif
}

bool atomic_write(const std::string& path, std::span<const std::uint8_t> bytes,
                  const PersistenceOptions& options, std::string& error) {
#ifdef _WIN32
  std::string directory;
  const std::size_t separator = path.find_last_of("\\/");
  directory = separator == std::string::npos ? std::string(".") : path.substr(0, separator);
  if (!options.temporary_directory.empty()) {
    directory = options.temporary_directory;
  }
  const std::string temporary =
      directory + "\\.agent_runtime-" + std::to_string(GetCurrentProcessId()) + "-" +
      std::to_string(reinterpret_cast<std::uintptr_t>(bytes.data())) + ".tmp";
  const std::wstring wide_temporary = extended_prefix(widen(temporary));
  const std::wstring wide_target = extended_prefix(widen(path));
  if (wide_temporary.empty() || wide_target.empty()) {
    error = "path is empty or is not valid UTF-8";
    return false;
  }
  HANDLE handle = CreateFileW(wide_temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    error = "cannot create temporary file (windows error " +
            std::to_string(GetLastError()) + ")";
    return false;
  }
  std::size_t offset = 0;
  bool ok = true;
  while (offset < bytes.size()) {
    DWORD written = 0;
    const DWORD request =
        static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1u << 20));
    if (!WriteFile(handle, bytes.data() + offset, request, &written, nullptr) || written == 0) {
      error = "write failed";
      ok = false;
      break;
    }
    offset += written;
  }
  if (ok && options.durable && !FlushFileBuffers(handle)) {
    error = "durable flush failed";
    ok = false;
  }
  CloseHandle(handle);
  if (!ok) {
    DeleteFileW(wide_temporary.c_str());
    return false;
  }
  DWORD move_flags = MOVEFILE_REPLACE_EXISTING;
  if (options.durable) {
    move_flags |= MOVEFILE_WRITE_THROUGH;
  }
  if (!MoveFileExW(wide_temporary.c_str(), wide_target.c_str(), move_flags)) {
    error = "atomic replacement failed (windows error " + std::to_string(GetLastError()) + ")";
    DeleteFileW(wide_temporary.c_str());
    return false;
  }
  return true;
#else
  std::string directory;
  const std::size_t separator = path.find_last_of('/');
  directory = separator == std::string::npos ? std::string(".") : path.substr(0, separator);
  if (!options.temporary_directory.empty()) {
    directory = options.temporary_directory;
  }
  const std::string temporary = directory + "/.agent_runtime-" +
                                std::to_string(static_cast<long>(getpid())) + ".tmp";
  const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    error = "cannot create temporary file";
    return false;
  }
  std::size_t offset = 0;
  bool ok = true;
  while (offset < bytes.size()) {
    const ssize_t written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (written <= 0) {
      error = "write failed";
      ok = false;
      break;
    }
    offset += static_cast<std::size_t>(written);
  }
  if (ok && options.durable && ::fsync(fd) != 0) {
    error = "durable flush failed";
    ok = false;
  }
  ::close(fd);
  if (!ok) {
    std::remove(temporary.c_str());
    return false;
  }
  if (std::rename(temporary.c_str(), path.c_str()) != 0) {
    error = "atomic replacement failed";
    std::remove(temporary.c_str());
    return false;
  }
  return true;
#endif
}

}  // namespace agent_runtime::detail

namespace agent_runtime {

std::uint16_t supported_persistence_format_version() noexcept {
  return persistence_format_version;
}

PersistenceProbe persistence_inspect(const std::string& path, const PersistenceOptions& options) {
  PersistenceProbe probe;
  std::vector<std::uint8_t> bytes;
  std::string error;
  if (!detail::read_file_bounded(path, options.max_bytes, bytes, error)) {
    probe.code = OutcomeCode::REJECT_INVALID;
    probe.explanation = ExplanationBuilder(OutcomeCode::REJECT_INVALID, path)
                            .set_message(error)
                            .build();
    return probe;
  }
  detail::DecodeResult decoded =
      detail::decode_document(std::span<const std::uint8_t>(bytes.data(), bytes.size()),
                              default_resource_limits());
  probe.code = decoded.code;
  probe.info = decoded.info;
  probe.document_json = std::move(decoded.document_json);
  probe.explanation = ExplanationBuilder(decoded.code, path)
                          .add_u64("format_version", decoded.info.format_version)
                          .add_u64("record_count", decoded.info.record_count)
                          .add_u64("payload_bytes", decoded.info.payload_bytes)
                          .add_u64("file_bytes", decoded.info.file_bytes)
                          .add("semantic_digest", decoded.info.semantic_digest_hex)
                          .set_message(decoded.message)
                          .build();
  return probe;
}

}  // namespace agent_runtime
