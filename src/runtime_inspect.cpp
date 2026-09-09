// Agent Runtime - snapshots, summaries, explanations, invariants and digests.
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

void append_json_string(std::string& out, std::string_view text) {
  static constexpr char kDigits[] = "0123456789abcdef";
  out.push_back('"');
  for (const char raw : text) {
    const auto byte = static_cast<unsigned char>(raw);
    switch (byte) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (byte < 0x20u) {
          out += "\\u00";
          out.push_back(kDigits[(byte >> 4) & 0x0Fu]);
          out.push_back(kDigits[byte & 0x0Fu]);
        } else {
          out.push_back(raw);
        }
        break;
    }
  }
  out.push_back('"');
}

void json_key(std::string& out, const char* key) {
  if (!out.empty() && out.back() != '{' && out.back() != '[') {
    out.push_back(',');
  }
  out.push_back('"');
  out += key;
  out += "\":";
}

void json_u64(std::string& out, const char* key, std::uint64_t value) {
  json_key(out, key);
  out += std::to_string(value);
}

void json_bool(std::string& out, const char* key, bool value) {
  json_key(out, key);
  out += value ? "true" : "false";
}

void json_string(std::string& out, const char* key, std::string_view value) {
  json_key(out, key);
  append_json_string(out, value);
}

void add_violation(InvariantReport& report, std::string name, std::string detail) {
  report.ok = false;
  report.violations.push_back(InvariantViolation{std::move(name), std::move(detail)});
}

}  // namespace

void Indexes::rebuild(const CanonicalState& state) {
  action_by_id.clear();
  attempt_by_id.clear();
  attempts_by_action.clear();
  memory_by_id.clear();
  checkpoint_by_id.clear();
  for (std::size_t i = 0; i < actions_by_state.size(); ++i) {
    actions_by_state[i].clear();
  }
  pending_model_calls.clear();
  pending_tool_calls.clear();
  ambiguous_actions.clear();

  for (std::uint32_t i = 0; i < state.actions.size(); ++i) {
    const ActionRecord& action = state.actions[i];
    action_by_id.emplace(action.id, i);
    actions_by_state[static_cast<std::size_t>(action.state)].push_back(i);
    if (action.ambiguous) {
      ambiguous_actions.push_back(i);
    }
  }
  for (std::uint32_t i = 0; i < state.attempts.size(); ++i) {
    const AttemptRecord& attempt = state.attempts[i];
    attempt_by_id.emplace(attempt.id, i);
    attempts_by_action[attempt.action_id].push_back(i);
    if (is_in_flight(attempt.state) && !attempt.superseded) {
      const auto attempt_action = action_by_id.find(attempt.action_id);
    const std::uint32_t index =
        attempt_action == action_by_id.end() ? kInvalidIndex : attempt_action->second;
      if (index != kInvalidIndex) {
        if (state.actions[index].kind == ActionKind::MODEL_CALL) {
          pending_model_calls.push_back(i);
        } else if (state.actions[index].kind == ActionKind::TOOL_CALL) {
          pending_tool_calls.push_back(i);
        }
      }
    }
  }
  for (std::uint32_t i = 0; i < state.memory_bindings.size(); ++i) {
    memory_by_id.emplace(state.memory_bindings[i].binding_id, i);
  }
  for (std::uint32_t i = 0; i < state.checkpoints.size(); ++i) {
    checkpoint_by_id.emplace(state.checkpoints[i].binding.binding_id, i);
  }
}

std::string canonical_json(const CanonicalState& state) {
  std::string out = "{";
  json_u64(out, "runtime_id", state.runtime_id.value());
  json_u64(out, "runtime_generation", state.runtime_generation.value());
  json_u64(out, "runtime_boot_id", state.runtime_boot_id.value());
  json_u64(out, "agent_id", state.agent_id.value());
  json_u64(out, "agent_generation", state.agent_generation.value());
  json_u64(out, "agent_boot_id", state.agent_boot_id.value());
  json_u64(out, "runtime_epoch", state.runtime_epoch.value());
  json_u64(out, "coordinator_epoch", state.coordinator_epoch.value());
  json_string(out, "lifecycle", to_string(state.lifecycle));
  json_string(out, "cancellation", to_string(state.cancellation));
  json_string(out, "recovery", to_string(state.recovery));
  json_string(out, "terminal_reason", state.terminal_reason);
  json_string(out, "cancellation_reason", state.cancellation_reason);
  json_string(out, "recovery_reason", state.recovery_reason);
  json_u64(out, "run_id", state.run_id.value());
  json_u64(out, "run_generation", state.run_generation.value());
  json_u64(out, "step_id", state.step_id.value());
  json_u64(out, "step_generation", state.step_generation.value());
  json_u64(out, "progress_generation", state.progress_generation.value());
  json_u64(out, "committed_actions", state.committed_actions);
  json_u64(out, "next_action_generation", state.next_action_generation.value());
  json_bool(out, "checkpoint_due", state.checkpoint_due);
  json_u64(out, "commits_at_last_checkpoint", state.commits_at_last_checkpoint);

  json_key(out, "runs");
  out.push_back('[');
  for (std::size_t i = 0; i < state.runs.size(); ++i) {
    if (i != 0) {
      out.push_back(',');
    }
    out.push_back('{');
    json_u64(out, "id", state.runs[i].id.value());
    json_u64(out, "generation", state.runs[i].generation.value());
    json_u64(out, "runtime_generation", state.runs[i].runtime_generation.value());
    json_u64(out, "progress_generation", state.runs[i].progress_generation.value());
    json_u64(out, "committed_actions", state.runs[i].committed_actions);
    json_u64(out, "steps", state.runs[i].steps);
    json_bool(out, "terminal", state.runs[i].terminal);
    json_string(out, "terminal_reason", state.runs[i].terminal_reason);
    json_u64(out, "started_at", static_cast<std::uint64_t>(state.runs[i].started_at_unix_millis));
    json_u64(out, "ended_at", static_cast<std::uint64_t>(state.runs[i].ended_at_unix_millis));
    out.push_back('}');
  }
  out.push_back(']');

  json_key(out, "steps");
  out.push_back('[');
  for (std::size_t i = 0; i < state.steps.size(); ++i) {
    if (i != 0) {
      out.push_back(',');
    }
    out.push_back('{');
    json_u64(out, "id", state.steps[i].id.value());
    json_u64(out, "generation", state.steps[i].generation.value());
    json_u64(out, "run_id", state.steps[i].run_id.value());
    json_u64(out, "run_generation", state.steps[i].run_generation.value());
    json_string(out, "progress", to_string(state.steps[i].progress));
    json_u64(out, "progress_generation", state.steps[i].progress_generation.value());
    json_u64(out, "committed_actions", state.steps[i].committed_actions);
    json_u64(out, "declared_actions", state.steps[i].declared_actions);
    out.push_back('}');
  }
  out.push_back(']');

  json_key(out, "actions");
  out.push_back('[');
  for (std::size_t i = 0; i < state.actions.size(); ++i) {
    const ActionRecord& action = state.actions[i];
    if (i != 0) {
      out.push_back(',');
    }
    out.push_back('{');
    json_u64(out, "id", action.id.value());
    json_u64(out, "generation", action.generation.value());
    json_u64(out, "run_id", action.run_id.value());
    json_u64(out, "run_generation", action.run_generation.value());
    json_u64(out, "step_id", action.step_id.value());
    json_u64(out, "step_generation", action.step_generation.value());
    json_string(out, "kind", to_string(action.kind));
    json_string(out, "label", action.label);
    json_string(out, "side_effect", to_string(action.side_effect));
    json_string(out, "state", to_string(action.state));
    json_string(out, "model_target", action.model_target);
    json_string(out, "tool_name", action.tool_name);
    json_string(out, "operation_key", action.operation_key);
    json_u64(out, "input_digest", action.input_digest);
    json_u64(out, "result_digest", action.result_digest);
    json_u64(out, "policy_generation", action.policy_generation.value());
    json_u64(out, "budget_generation", action.budget_generation.value());
    json_bool(out, "budget_required", action.budget_required);
    json_u64(out, "memory_binding_id", action.memory_binding_id.value());
    json_u64(out, "memory_binding_generation", action.memory_binding_generation.value());
    json_u64(out, "model_binding_generation", action.model_binding_generation.value());
    json_u64(out, "tool_binding_generation", action.tool_binding_generation.value());
    json_u64(out, "checkpoint_binding_id", action.checkpoint_binding_id.value());
    json_u64(out, "checkpoint_generation", action.checkpoint_generation.value());
    json_u64(out, "runtime_boot_id", action.runtime_boot_id.value());
    json_u64(out, "runtime_epoch", action.runtime_epoch.value());
    json_u64(out, "coordinator_epoch", action.coordinator_epoch.value());
    json_u64(out, "attempt_count", action.attempt_count);
    json_u64(out, "current_attempt", action.current_attempt.value());
    json_u64(out, "current_attempt_generation", action.current_attempt_generation.value());
    json_bool(out, "committed", action.committed);
    json_u64(out, "commit_generation", action.commit_generation.value());
    json_u64(out, "accepted_result", action.accepted_result.value());
    json_u64(out, "accepted_completion_generation",
             action.accepted_completion_generation.value());
    json_string(out, "completion_status", to_string(action.completion_status));
    json_string(out, "last_failure", to_string(action.last_failure));
    json_string(out, "last_error", action.last_error);
    json_bool(out, "ambiguous", action.ambiguous);
    json_bool(out, "requires_checkpoint", action.requires_checkpoint);
    json_u64(out, "declaration_sequence", action.declaration_sequence);
    json_u64(out, "commit_sequence", action.commit_sequence);
    json_key(out, "dependencies");
    out.push_back('[');
    for (std::size_t d = 0; d < action.dependencies.size(); ++d) {
      if (d != 0) {
        out.push_back(',');
      }
      out += std::to_string(action.dependencies[d].value());
    }
    out.push_back(']');
    out.push_back('}');
  }
  out.push_back(']');

  json_key(out, "attempts");
  out.push_back('[');
  for (std::size_t i = 0; i < state.attempts.size(); ++i) {
    const AttemptRecord& attempt = state.attempts[i];
    if (i != 0) {
      out.push_back(',');
    }
    out.push_back('{');
    json_u64(out, "id", attempt.id.value());
    json_u64(out, "generation", attempt.generation.value());
    json_u64(out, "action_id", attempt.action_id.value());
    json_u64(out, "action_generation", attempt.action_generation.value());
    json_u64(out, "runtime_boot_id", attempt.runtime_boot_id.value());
    json_u64(out, "runtime_epoch", attempt.runtime_epoch.value());
    json_u64(out, "coordinator_epoch", attempt.coordinator_epoch.value());
    json_u64(out, "model_call_id", attempt.model_call_id.value());
    json_u64(out, "model_call_generation", attempt.model_call_generation.value());
    json_u64(out, "tool_call_id", attempt.tool_call_id.value());
    json_u64(out, "tool_call_generation", attempt.tool_call_generation.value());
    json_u64(out, "backend_id", attempt.backend_id.value());
    json_u64(out, "backend_generation", attempt.backend_generation.value());
    json_string(out, "state", to_string(attempt.state));
    json_string(out, "completion_status", to_string(attempt.completion_status));
    json_u64(out, "result_id", attempt.result_id.value());
    json_u64(out, "completion_generation", attempt.completion_generation.value());
    json_string(out, "failure_class", to_string(attempt.failure_class));
    json_u64(out, "result_digest", attempt.result_digest);
    json_bool(out, "superseded", attempt.superseded);
    json_bool(out, "ambiguous", attempt.ambiguous);
    json_bool(out, "current", attempt.current);
    json_bool(out, "completion_recorded", attempt.completion_recorded);
    json_u64(out, "dispatched_at", static_cast<std::uint64_t>(attempt.dispatched_at_unix_millis));
    json_u64(out, "completed_at", static_cast<std::uint64_t>(attempt.completed_at_unix_millis));
    json_string(out, "error", attempt.error);
    out.push_back('}');
  }
  out.push_back(']');

  json_key(out, "memory_bindings");
  out.push_back('[');
  for (std::size_t i = 0; i < state.memory_bindings.size(); ++i) {
    const MemoryBinding& binding = state.memory_bindings[i];
    if (i != 0) {
      out.push_back(',');
    }
    out.push_back('{');
    json_u64(out, "id", binding.binding_id.value());
    json_u64(out, "generation", binding.generation.value());
    json_string(out, "state_identity", binding.state_identity);
    json_string(out, "compatibility", binding.compatibility);
    json_u64(out, "external_generation", binding.external_generation.value());
    json_bool(out, "read_authority", binding.read_authority);
    json_bool(out, "write_authority", binding.write_authority);
    json_bool(out, "fresh", binding.fresh);
    json_string(out, "digest", binding.digest);
    out.push_back('}');
  }
  out.push_back(']');

  json_key(out, "checkpoints");
  out.push_back('[');
  for (std::size_t i = 0; i < state.checkpoints.size(); ++i) {
    const CheckpointRecord& record = state.checkpoints[i];
    if (i != 0) {
      out.push_back(',');
    }
    out.push_back('{');
    json_u64(out, "binding_id", record.binding.binding_id.value());
    json_u64(out, "generation", record.binding.generation.value());
    json_u64(out, "runtime_id", record.binding.runtime_id.value());
    json_u64(out, "runtime_generation", record.binding.runtime_generation.value());
    json_u64(out, "run_id", record.binding.run_id.value());
    json_u64(out, "run_generation", record.binding.run_generation.value());
    json_u64(out, "step_generation", record.binding.step_generation.value());
    json_u64(out, "runtime_epoch", record.binding.runtime_epoch.value());
    json_u64(out, "progress_generation", record.binding.progress_generation.value());
    json_string(out, "identity", record.binding.checkpoint_identity);
    json_string(out, "integrity_digest", record.binding.integrity_digest);
    json_string(out, "state", to_string(record.state));
    json_bool(out, "restorable", record.binding.restorable);
    json_bool(out, "current", record.binding.current);
    out.push_back('}');
  }
  out.push_back(']');

  json_key(out, "fenced_boots");
  out.push_back('[');
  for (std::size_t i = 0; i < state.fenced_boots.size(); ++i) {
    if (i != 0) {
      out.push_back(',');
    }
    out.push_back('{');
    json_u64(out, "runtime_boot_id", state.fenced_boots[i].runtime_boot_id.value());
    json_u64(out, "agent_boot_id", state.fenced_boots[i].agent_boot_id.value());
    json_string(out, "reason", state.fenced_boots[i].reason);
    out.push_back('}');
  }
  out.push_back(']');

  json_key(out, "assignments");
  out.push_back('[');
  for (std::size_t i = 0; i < state.assignments.size(); ++i) {
    const AssignmentRecord& record = state.assignments[i];
    if (i != 0) {
      out.push_back(',');
    }
    out.push_back('{');
    json_u64(out, "assignment_id", record.authority.assignment_id.value());
    json_u64(out, "assignment_generation", record.authority.assignment_generation.value());
    json_u64(out, "agent_id", record.authority.agent_id.value());
    json_u64(out, "agent_generation", record.authority.agent_generation.value());
    json_u64(out, "work_id", record.authority.work_id.value());
    json_u64(out, "work_generation", record.authority.work_generation.value());
    json_u64(out, "scheduler_epoch", record.authority.scheduler_epoch.value());
    json_u64(out, "coordinator_epoch", record.authority.coordinator_epoch.value());
    json_u64(out, "lease_id", record.authority.lease_id);
    json_u64(out, "lease_generation", record.authority.lease_generation);
    json_bool(out, "current", record.current);
    json_string(out, "reason", record.reason);
    out.push_back('}');
  }
  out.push_back(']');

  json_key(out, "policy_history");
  out.push_back('[');
  for (std::size_t i = 0; i < state.policy_history.size(); ++i) {
    if (i != 0) {
      out.push_back(',');
    }
    out.push_back('{');
    json_u64(out, "policy_id", state.policy_history[i].policy_id.value());
    json_u64(out, "generation", state.policy_history[i].generation.value());
    out.push_back('}');
  }
  out.push_back(']');

  json_key(out, "budget_history");
  out.push_back('[');
  for (std::size_t i = 0; i < state.budget_history.size(); ++i) {
    if (i != 0) {
      out.push_back(',');
    }
    out.push_back('{');
    json_u64(out, "budget_id", state.budget_history[i].budget_id.value());
    json_u64(out, "generation", state.budget_history[i].generation.value());
    json_string(out, "outcome", to_string(state.budget_history[i].outcome));
    out.push_back('}');
  }
  out.push_back(']');

  out.push_back('}');
  return out;
}

std::uint64_t canonical_digest(const CanonicalState& state) {
  const std::string json = canonical_json(state);
  const auto digest = sha256(json);
  return digest_prefix_u64(std::span<const std::uint8_t>(digest.data(), digest.size()));
}

std::uint32_t active_action_count(const CanonicalState& state) noexcept {
  return count_active_actions(state);
}

std::uint32_t in_flight_attempt_count(const CanonicalState& state) noexcept {
  return count_in_flight_attempts(state);
}

bool any_dispatchable_action(const CanonicalState& state) noexcept {
  for (const ActionRecord& action : state.actions) {
    if (action.state == ActionState::ADMITTED || action.state == ActionState::AUTHORIZED ||
        is_in_flight(action.state)) {
      return true;
    }
  }
  return false;
}

bool has_ambiguous_action(const CanonicalState& state) noexcept {
  for (const ActionRecord& action : state.actions) {
    if (action.ambiguous && action.state != ActionState::COMMITTED &&
        action.state != ActionState::CANCELLED && action.state != ActionState::FAILED &&
        action.state != ActionState::SUPERSEDED) {
      return true;
    }
  }
  return false;
}

InvariantReport check_canonical_invariants(const CanonicalState& state, const Indexes& indexes,
                                           const ResourceLimits& limits) {
  InvariantReport report;

  auto check = [&report](bool condition, const char* name, std::string detail) {
    ++report.checks_run;
    if (!condition) {
      add_violation(report, name, std::move(detail));
    }
  };

  check(state.runtime_epoch.valid(), "one_current_runtime_epoch", "runtime epoch is invalid");
  check(state.coordinator_epoch.valid(), "one_current_coordinator_epoch",
        "coordinator epoch is invalid");
  check(state.runtime_boot_id.valid(), "one_current_runtime_boot", "runtime boot is invalid");
  check(state.agent_boot_id.valid(), "one_current_agent_boot", "agent boot is invalid");

  if (state.current_run_index != kInvalidIndex) {
    check(state.current_run_index < state.runs.size(), "current_run_index_in_range",
          "current run index is out of range");
    if (state.current_run_index < state.runs.size()) {
      const RunRecord& run = state.runs[state.current_run_index];
      check(run.id == state.run_id && run.generation == state.run_generation,
            "one_current_run_generation", "current run record does not match run identity");
    }
  } else {
    check(!state.run_id.valid() && !state.run_generation.valid(), "no_current_run_is_consistent",
          "run identity is set without a current run record");
  }

  if (state.current_step_index != kInvalidIndex) {
    check(state.current_step_index < state.steps.size(), "current_step_index_in_range",
          "current step index is out of range");
    if (state.current_step_index < state.steps.size()) {
      const StepRecord& step = state.steps[state.current_step_index];
      check(step.id == state.step_id && step.generation == state.step_generation,
            "one_current_step_generation", "current step record does not match step identity");
      check(step.run_id == state.run_id && step.run_generation == state.run_generation,
            "current_step_belongs_to_current_run",
            "current step belongs to a different run generation");
    }
  }

  ActionGeneration previous_generation{};
  bool first_action = true;
  for (const ActionRecord& action : state.actions) {
    if (first_action) {
      first_action = false;
    } else {
      check(action.generation > previous_generation, "action_generations_monotonic",
            "action generation " + std::to_string(action.generation.value()) +
                " is not greater than the previous generation");
    }
    previous_generation = action.generation;

    if (action.run_generation != state.run_generation && !action.committed) {
      check(action.run_generation < state.run_generation, "current_action_run_generation",
            "non-committed action belongs to a future run generation");
    }
    if (action.committed) {
      check(action.accepted_result.valid() && action.accepted_completion_generation.valid(),
            "committed_progress_references_accepted_completion",
            "committed action " + std::to_string(action.id.value()) +
                " has no accepted completion");
      check(action.commit_generation.valid(), "committed_action_has_commit_generation",
            "committed action has no commit generation");
      check(action.state == ActionState::COMMITTED, "committed_action_state",
            "committed flag is set but the action state is not COMMITTED");
    } else {
      check(action.state != ActionState::COMMITTED, "uncommitted_action_state",
            "action state is COMMITTED but the committed flag is not set");
    }
    if (action.ambiguous) {
      check(action.state != ActionState::RETRY_PENDING,
            "ambiguous_action_not_automatically_retried",
            "ambiguous action is in RETRY_PENDING");
      check(!action.current_attempt.valid(), "ambiguous_action_has_no_current_attempt",
            "ambiguous action still has a current attempt");
      check(action.side_effect != SideEffectClass::PURE &&
                action.side_effect != SideEffectClass::READ_ONLY,
            "ambiguous_effect_free_action",
            "an effect-free action should never be classified ambiguous");
    }
    if (is_in_flight(action.state)) {
      check(action.current_attempt.valid() && action.current_attempt_generation.valid(),
            "in_flight_action_has_current_attempt",
            "in-flight action has no current attempt identity");
    }
  }

  std::unordered_map<ActionAttemptId, std::uint32_t> attempt_seen;
  for (const AttemptRecord& attempt : state.attempts) {
    check(attempt.generation.valid(), "attempt_generation_valid", "attempt generation is invalid");
    check(attempt_seen.emplace(attempt.id, 0).second, "attempt_identity_unique",
          "duplicate attempt identity " + std::to_string(attempt.id.value()));
    const auto attempt_action = indexes.action_by_id.find(attempt.action_id);
    const std::uint32_t index =
        attempt_action == indexes.action_by_id.end() ? kInvalidIndex : attempt_action->second;
    check(index != kInvalidIndex, "attempt_references_existing_action",
          "attempt references an unknown action");
    if (index != kInvalidIndex) {
      check(state.actions[index].generation == attempt.action_generation,
            "attempt_action_generation_matches", "attempt action generation is inconsistent");
    }
    if (attempt.state == ActionState::COMMITTED) {
      check(!attempt.current && attempt.superseded, "stale_attempt_cannot_commit",
            "a committed attempt must not remain current");
    }
    if (attempt.ambiguous) {
      check(!attempt.current && attempt.superseded, "ambiguous_attempt_not_current",
            "an ambiguous attempt must not remain current");
    }
  }

  for (const ActionRecord& action : state.actions) {
    std::uint32_t attempts_for_action = 0;
    AttemptGeneration previous{};
    bool first = true;
    for (const AttemptRecord& attempt : state.attempts) {
      if (attempt.action_id != action.id) {
        continue;
      }
      ++attempts_for_action;
      if (first) {
        first = false;
      } else {
        check(attempt.generation > previous, "attempt_generations_monotonic",
              "attempt generations are not monotonic for action " +
                  std::to_string(action.id.value()));
      }
      previous = attempt.generation;
    }
    check(attempts_for_action == action.attempt_count, "historical_attempt_accounting_exact",
          "action " + std::to_string(action.id.value()) + " records " +
              std::to_string(action.attempt_count) + " attempts but " +
              std::to_string(attempts_for_action) + " exist");
    check(action.attempt_count <= action.retry.max_attempts ||
              action.attempt_count <= state.policy.max_retries_per_action + 1,
          "retry_count_within_bound",
          "action attempt count exceeds the configured bound");
  }

  if (state.lifecycle == RuntimeLifecycle::CANCELLED) {
    check(!any_dispatchable_action(state), "cancelled_runtime_cannot_admit_actions",
          "cancelled runtime still has dispatchable actions");
    check(state.cancellation == CancellationState::COMPLETED, "cancelled_runtime_state",
          "cancelled lifecycle without completed cancellation state");
  }
  if (state.lifecycle == RuntimeLifecycle::COMPLETED) {
    check(!any_dispatchable_action(state), "completed_runtime_cannot_admit_actions",
          "completed runtime still has dispatchable actions");
  }
  if (state.lifecycle == RuntimeLifecycle::SUSPENDED) {
    check(!any_dispatchable_action(state), "suspended_runtime_has_no_dispatchable_action",
          "suspended runtime still has dispatchable actions");
  }
  if (state.lifecycle == RuntimeLifecycle::RETIRED) {
    check(state.recovery != RecoveryStatus::RESUMABLE, "retired_runtime_cannot_resume",
          "retired runtime reports RESUMABLE");
  }
  if (state.recovery != RecoveryStatus::NONE && state.recovery != RecoveryStatus::RESUMABLE) {
    check(in_flight_attempt_count(state) == 0, "recovered_in_flight_not_silently_executable",
          "recovered runtime still has in-flight attempts");
  }
  if (state.lifecycle == RuntimeLifecycle::DRAINING ||
      state.lifecycle == RuntimeLifecycle::CANCELLING) {
    for (const ActionRecord& action : state.actions) {
      check(action.state != ActionState::DECLARED, "shutdown_cannot_publish_new_work",
            "a declared action exists while the runtime is shutting down");
    }
  }

  check(is_runtime_boot_fenced(state, state.runtime_boot_id) == false,
        "fenced_runtime_boot_cannot_regain_authority",
        "the current runtime boot is listed as fenced");

  for (const CheckpointRecord& record : state.checkpoints) {
    if (record.state == CheckpointState::ACCEPTED) {
      check(record.binding.runtime_id == state.runtime_id,
            "committed_checkpoint_references_compatible_runtime",
            "accepted checkpoint belongs to a different runtime identity");
      check(record.binding.runtime_generation <= state.runtime_generation,
            "accepted_checkpoint_generation_compatible",
            "accepted checkpoint belongs to a future runtime generation");
    }
  }

  check(count_active_actions(state) <= limits.max_active_actions, "active_action_bound",
        "active action count exceeds the configured bound");
  check(count_in_flight_attempts(state) <= limits.max_active_attempts, "active_attempt_bound",
        "in-flight attempt count exceeds the configured bound");
  check(state.actions.size() <= limits.max_actions_per_run, "action_history_bound",
        "action history exceeds the configured bound");
  check(state.attempts.size() <= limits.max_retained_history, "attempt_history_bound",
        "attempt history exceeds the configured bound");
  check(state.memory_bindings.size() <= limits.max_memory_bindings, "memory_binding_bound",
        "memory binding count exceeds the configured bound");
  check(state.checkpoints.size() <= limits.max_checkpoint_bindings, "checkpoint_binding_bound",
        "checkpoint binding count exceeds the configured bound");

  std::uint64_t committed = 0;
  for (const ActionRecord& action : state.actions) {
    if (action.committed) {
      ++committed;
    }
  }
  check(committed == state.committed_actions, "committed_counter_consistent",
        "committed_actions counter does not match the committed action count");
  check(state.progress_generation.value() == state.committed_actions,
        "progress_generation_matches_commits",
        "progress generation does not match the number of commits");
  if (state.current_run_index != kInvalidIndex &&
      state.current_run_index < state.runs.size()) {
    check(state.runs[state.current_run_index].committed_actions == state.committed_actions ||
              state.runs.size() > 1,
          "run_committed_counter_consistent",
          "current run committed counter does not match the runtime counter");
  }

  // Index agreement.
  check(indexes.action_by_id.size() == state.actions.size(), "index_action_count",
        "action index size does not match canonical actions");
  for (std::uint32_t i = 0; i < state.actions.size(); ++i) {
    const auto found = indexes.action_by_id.find(state.actions[i].id);
    check(found != indexes.action_by_id.end() && found->second == i, "index_action_mapping",
          "action index does not map identity to the canonical position");
  }
  check(indexes.attempt_by_id.size() == state.attempts.size(), "index_attempt_count",
        "attempt index size does not match canonical attempts");
  for (std::uint32_t i = 0; i < state.attempts.size(); ++i) {
    const auto found = indexes.attempt_by_id.find(state.attempts[i].id);
    check(found != indexes.attempt_by_id.end() && found->second == i, "index_attempt_mapping",
          "attempt index does not map identity to the canonical position");
  }
  std::uint32_t index_state_total = 0;
  for (const std::vector<std::uint32_t>& bucket : indexes.actions_by_state) {
    index_state_total += static_cast<std::uint32_t>(bucket.size());
  }
  check(index_state_total == state.actions.size(), "index_state_buckets_complete",
        "action state index does not cover every canonical action");

  return report;
}

}  // namespace agent_runtime::detail

namespace agent_runtime {

using detail::ActionRecord;
using detail::AttemptRecord;
using detail::CanonicalState;
using detail::CheckpointRecord;
using detail::kInvalidIndex;

namespace {

[[nodiscard]] ProgressState progress_state_locked(const CanonicalState& state) {
  if (state.lifecycle == RuntimeLifecycle::CANCELLED ||
      state.lifecycle == RuntimeLifecycle::FAILED) {
    return ProgressState::TERMINAL;
  }
  if (state.lifecycle == RuntimeLifecycle::COMPLETED) {
    return ProgressState::TERMINAL;
  }
  if (state.checkpoint_due) {
    return ProgressState::BOUNDARY_READY;
  }
  if (detail::has_ambiguous_action(state)) {
    return ProgressState::BLOCKED;
  }
  if (state.committed_actions != 0) {
    return ProgressState::COMMITTED;
  }
  if (state.current_run_index != kInvalidIndex) {
    return ProgressState::IN_PROGRESS;
  }
  return ProgressState::NONE;
}

}  // namespace

InvariantReport AgentRuntime::Impl::invariants_locked() const {
  ensure_indexes_locked();
  InvariantReport report = detail::check_canonical_invariants(state, indexes, limits);
  // The maintained counters are an optimization over canonical scans; they must
  // always agree with what a full scan computes.
  const std::uint32_t scanned_active = detail::count_active_actions(state);
  const std::uint32_t scanned_in_flight = detail::count_in_flight_attempts(state);
  ++report.checks_run;
  if (scanned_active != active_actions_) {
    report.ok = false;
    report.violations.push_back(InvariantViolation{
        "active_action_counter_agrees_with_canonical_scan",
        "counter=" + std::to_string(active_actions_) +
            " scan=" + std::to_string(scanned_active)});
  }
  ++report.checks_run;
  if (scanned_in_flight != in_flight_attempts_) {
    report.ok = false;
    report.violations.push_back(InvariantViolation{
        "in_flight_counter_agrees_with_canonical_scan",
        "counter=" + std::to_string(in_flight_attempts_) +
            " scan=" + std::to_string(scanned_in_flight)});
  }
  return report;
}

InvariantReport AgentRuntime::check_invariants() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->invariants_locked();
}

std::uint64_t AgentRuntime::durable_digest() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return detail::canonical_digest(impl_->state);
}

RuntimeSummary AgentRuntime::summary() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const CanonicalState& state = impl_->state;
  RuntimeSummary summary;
  summary.runtime_id = state.runtime_id;
  summary.runtime_generation = state.runtime_generation;
  summary.agent_id = state.agent_id;
  summary.agent_generation = state.agent_generation;
  summary.agent_boot_id = state.agent_boot_id;
  summary.runtime_boot_id = state.runtime_boot_id;
  summary.runtime_epoch = state.runtime_epoch;
  summary.coordinator_epoch = state.coordinator_epoch;
  summary.lifecycle = state.lifecycle;
  summary.run_id = state.run_id;
  summary.run_generation = state.run_generation;
  summary.step_id = state.step_id;
  summary.step_generation = state.step_generation;
  summary.progress = progress_state_locked(state);
  summary.progress_generation = state.progress_generation;
  summary.cancellation = state.cancellation;
  summary.recovery = state.recovery;
  summary.validity = SnapshotValidity::CURRENT;
  summary.total_actions = static_cast<std::uint32_t>(state.actions.size());
  summary.total_attempts = static_cast<std::uint32_t>(state.attempts.size());
  summary.checkpoints = static_cast<std::uint32_t>(state.checkpoints.size());
  summary.memory_bindings = static_cast<std::uint32_t>(state.memory_bindings.size());
  summary.fenced_boots = static_cast<std::uint32_t>(state.fenced_boots.size());
  summary.terminal_reason = state.terminal_reason;
  for (const ActionRecord& action : state.actions) {
    if (action.committed) {
      ++summary.committed_actions;
    }
    if (is_in_flight(action.state)) {
      ++summary.in_flight_actions;
    }
    if (action.state == ActionState::RETRY_PENDING) {
      ++summary.retry_pending_actions;
    }
    if (action.ambiguous && action.state != ActionState::COMMITTED) {
      ++summary.ambiguous_actions;
    }
    if (action.state == ActionState::REVALIDATION_REQUIRED) {
      ++summary.revalidation_actions;
    }
  }
  summary.semantic_digest = detail::canonical_digest(state);
  return summary;
}

std::vector<ActionView> AgentRuntime::actions() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<ActionView> views;
  views.reserve(impl_->state.actions.size());
  for (const ActionRecord& action : impl_->state.actions) {
    ActionView view;
    view.action_id = action.id;
    view.action_generation = action.generation;
    view.kind = action.kind;
    view.label = action.label;
    view.side_effect = action.side_effect;
    view.state = action.state;
    view.step_id = action.step_id;
    view.step_generation = action.step_generation;
    view.run_generation = action.run_generation;
    view.attempt_count = action.attempt_count;
    view.current_attempt = action.current_attempt;
    view.current_attempt_generation = action.current_attempt_generation;
    view.committed = action.committed;
    view.accepted_result = action.accepted_result;
    view.accepted_completion_generation = action.accepted_completion_generation;
    view.last_failure = action.last_failure;
    view.completion_status = action.completion_status;
    view.policy_generation = action.policy_generation;
    view.budget_generation = action.budget_generation;
    view.tool_name = action.tool_name;
    view.model_target = action.model_target;
    view.operation_key = action.operation_key;
    view.input_digest = action.input_digest;
    view.result_digest = action.result_digest;
    view.last_error = action.last_error;
    views.push_back(std::move(view));
  }
  return views;
}

std::vector<AttemptView> AgentRuntime::attempts() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<AttemptView> views;
  views.reserve(impl_->state.attempts.size());
  for (const AttemptRecord& attempt : impl_->state.attempts) {
    AttemptView view;
    view.attempt_id = attempt.id;
    view.attempt_generation = attempt.generation;
    view.action_id = attempt.action_id;
    view.action_generation = attempt.action_generation;
    view.runtime_boot_id = attempt.runtime_boot_id;
    view.runtime_epoch = attempt.runtime_epoch;
    view.coordinator_epoch = attempt.coordinator_epoch;
    view.state = attempt.state;
    view.completion_status = attempt.completion_status;
    view.result_id = attempt.result_id;
    view.completion_generation = attempt.completion_generation;
    view.failure_class = attempt.failure_class;
    view.backend_id = attempt.backend_id;
    view.backend_generation = attempt.backend_generation;
    view.result_digest = attempt.result_digest;
    view.superseded = attempt.superseded;
    view.ambiguous = attempt.ambiguous;
    view.current = attempt.current;
    view.dispatched_at_unix_millis = attempt.dispatched_at_unix_millis;
    view.completed_at_unix_millis = attempt.completed_at_unix_millis;
    view.error = attempt.error;
    views.push_back(std::move(view));
  }
  return views;
}

RuntimeSnapshot AgentRuntime::snapshot() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const CanonicalState& state = impl_->state;
  RuntimeSnapshot snapshot;
  snapshot.summary = RuntimeSummary{};
  snapshot.summary.runtime_id = state.runtime_id;
  snapshot.summary.runtime_generation = state.runtime_generation;
  snapshot.summary.agent_id = state.agent_id;
  snapshot.summary.agent_generation = state.agent_generation;
  snapshot.summary.agent_boot_id = state.agent_boot_id;
  snapshot.summary.runtime_boot_id = state.runtime_boot_id;
  snapshot.summary.runtime_epoch = state.runtime_epoch;
  snapshot.summary.coordinator_epoch = state.coordinator_epoch;
  snapshot.summary.lifecycle = state.lifecycle;
  snapshot.summary.run_id = state.run_id;
  snapshot.summary.run_generation = state.run_generation;
  snapshot.summary.step_id = state.step_id;
  snapshot.summary.step_generation = state.step_generation;
  snapshot.summary.progress = progress_state_locked(state);
  snapshot.summary.progress_generation = state.progress_generation;
  snapshot.summary.cancellation = state.cancellation;
  snapshot.summary.recovery = state.recovery;
  snapshot.summary.total_actions = static_cast<std::uint32_t>(state.actions.size());
  snapshot.summary.total_attempts = static_cast<std::uint32_t>(state.attempts.size());
  snapshot.summary.checkpoints = static_cast<std::uint32_t>(state.checkpoints.size());
  snapshot.summary.memory_bindings = static_cast<std::uint32_t>(state.memory_bindings.size());
  snapshot.summary.fenced_boots = static_cast<std::uint32_t>(state.fenced_boots.size());
  snapshot.summary.terminal_reason = state.terminal_reason;
  for (const ActionRecord& action : state.actions) {
    if (action.committed) {
      ++snapshot.summary.committed_actions;
    }
    if (is_in_flight(action.state)) {
      ++snapshot.summary.in_flight_actions;
    }
    if (action.state == ActionState::RETRY_PENDING) {
      ++snapshot.summary.retry_pending_actions;
    }
    if (action.ambiguous && action.state != ActionState::COMMITTED) {
      ++snapshot.summary.ambiguous_actions;
    }
    if (action.state == ActionState::REVALIDATION_REQUIRED) {
      ++snapshot.summary.revalidation_actions;
    }
  }

  switch (state.lifecycle) {
    case RuntimeLifecycle::DECLARED:
    case RuntimeLifecycle::INITIALIZING:
    case RuntimeLifecycle::READY:
    case RuntimeLifecycle::RUNNING:
    case RuntimeLifecycle::WAITING_MODEL:
    case RuntimeLifecycle::WAITING_TOOL:
    case RuntimeLifecycle::WAITING_EXTERNAL:
    case RuntimeLifecycle::CHECKPOINTING:
    case RuntimeLifecycle::SUSPENDING:
    case RuntimeLifecycle::DRAINING:
    case RuntimeLifecycle::CANCELLING:
      snapshot.validity = state.recovery == RecoveryStatus::NONE ? SnapshotValidity::CURRENT
                                                                 : SnapshotValidity::REVALIDATION_REQUIRED;
      break;
    case RuntimeLifecycle::SUSPENDED:
    case RuntimeLifecycle::RECOVERING:
    case RuntimeLifecycle::REVALIDATION_REQUIRED:
      snapshot.validity = SnapshotValidity::REVALIDATION_REQUIRED;
      break;
    case RuntimeLifecycle::CANCELLED:
    case RuntimeLifecycle::COMPLETED:
    case RuntimeLifecycle::FAILED:
    case RuntimeLifecycle::RETIRED:
      snapshot.validity = SnapshotValidity::CURRENT;
      break;
    case RuntimeLifecycle::kCount:
      snapshot.validity = SnapshotValidity::STALE;
      break;
  }
  snapshot.summary.validity = snapshot.validity;

  for (const ActionRecord& action : state.actions) {
    ActionView view;
    view.action_id = action.id;
    view.action_generation = action.generation;
    view.kind = action.kind;
    view.label = action.label;
    view.side_effect = action.side_effect;
    view.state = action.state;
    view.step_id = action.step_id;
    view.step_generation = action.step_generation;
    view.run_generation = action.run_generation;
    view.attempt_count = action.attempt_count;
    view.current_attempt = action.current_attempt;
    view.current_attempt_generation = action.current_attempt_generation;
    view.committed = action.committed;
    view.accepted_result = action.accepted_result;
    view.accepted_completion_generation = action.accepted_completion_generation;
    view.last_failure = action.last_failure;
    view.completion_status = action.completion_status;
    view.policy_generation = action.policy_generation;
    view.budget_generation = action.budget_generation;
    view.tool_name = action.tool_name;
    view.model_target = action.model_target;
    view.operation_key = action.operation_key;
    view.input_digest = action.input_digest;
    view.result_digest = action.result_digest;
    view.last_error = action.last_error;
    snapshot.actions.push_back(std::move(view));
  }
  for (const AttemptRecord& attempt : state.attempts) {
    AttemptView view;
    view.attempt_id = attempt.id;
    view.attempt_generation = attempt.generation;
    view.action_id = attempt.action_id;
    view.action_generation = attempt.action_generation;
    view.runtime_boot_id = attempt.runtime_boot_id;
    view.runtime_epoch = attempt.runtime_epoch;
    view.coordinator_epoch = attempt.coordinator_epoch;
    view.state = attempt.state;
    view.completion_status = attempt.completion_status;
    view.result_id = attempt.result_id;
    view.completion_generation = attempt.completion_generation;
    view.failure_class = attempt.failure_class;
    view.backend_id = attempt.backend_id;
    view.backend_generation = attempt.backend_generation;
    view.result_digest = attempt.result_digest;
    view.superseded = attempt.superseded;
    view.ambiguous = attempt.ambiguous;
    view.current = attempt.current;
    view.dispatched_at_unix_millis = attempt.dispatched_at_unix_millis;
    view.completed_at_unix_millis = attempt.completed_at_unix_millis;
    view.error = attempt.error;
    snapshot.attempts.push_back(std::move(view));
  }
  for (const CheckpointRecord& record : state.checkpoints) {
    CheckpointView view;
    view.binding_id = record.binding.binding_id;
    view.generation = record.binding.generation;
    view.state = record.state;
    view.runtime_generation = record.binding.runtime_generation;
    view.run_generation = record.binding.run_generation;
    view.step_generation = record.binding.step_generation;
    view.progress_generation = record.binding.progress_generation;
    view.runtime_epoch = record.binding.runtime_epoch;
    view.checkpoint_identity = record.binding.checkpoint_identity;
    view.integrity_digest = record.binding.integrity_digest;
    view.restorable = record.binding.restorable;
    view.current = record.binding.current;
    snapshot.checkpoints.push_back(std::move(view));
  }
  snapshot.memory_bindings = state.memory_bindings;
  snapshot.tool_bindings = state.tool_bindings;
  snapshot.model_bindings = state.model_bindings;
  for (const detail::FencedBootRecord& record : state.fenced_boots) {
    snapshot.fenced_runtime_boots.push_back(record.runtime_boot_id);
    snapshot.fenced_agent_boots.push_back(record.agent_boot_id);
  }
  snapshot.semantic_digest = detail::canonical_digest(state);
  snapshot.summary.semantic_digest = snapshot.semantic_digest;
  return snapshot;
}

Explanation AgentRuntime::explain_action(ActionId action_id,
                                         ActionGeneration action_generation) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const CanonicalState& state = impl_->state;
  const std::string subject = detail::action_subject(action_id, action_generation);
  ExplanationBuilder builder(OutcomeCode::ACCEPTED, subject);
  const std::uint32_t index = detail::find_action(state, action_id);
  if (index == kInvalidIndex) {
    return ExplanationBuilder(OutcomeCode::REJECT_INVALID, subject)
        .set_message("action does not exist")
        .build();
  }
  const ActionRecord& action = state.actions[index];
  builder.add("action_generation", std::to_string(action.generation.value()));
  builder.add("supplied_action_generation", std::to_string(action_generation.value()));
  builder.add("action_state", std::string(to_string(action.state)));
  builder.add("kind", std::string(to_string(action.kind)));
  builder.add("side_effect", std::string(to_string(action.side_effect)));
  builder.add("runtime_lifecycle", std::string(to_string(state.lifecycle)));
  builder.add("runtime_generation", std::to_string(state.runtime_generation.value()));
  builder.add("runtime_epoch", std::to_string(state.runtime_epoch.value()));
  builder.add("action_runtime_epoch", std::to_string(action.runtime_epoch.value()));
  builder.add("coordinator_epoch", std::to_string(state.coordinator_epoch.value()));
  builder.add("action_coordinator_epoch", std::to_string(action.coordinator_epoch.value()));
  builder.add("run_generation", std::to_string(state.run_generation.value()));
  builder.add("action_run_generation", std::to_string(action.run_generation.value()));
  builder.add("step_generation", std::to_string(state.step_generation.value()));
  builder.add("action_step_generation", std::to_string(action.step_generation.value()));
  builder.add("policy_generation", std::to_string(state.policy.generation.value()));
  builder.add("action_policy_generation", std::to_string(action.policy_generation.value()));
  builder.add("budget_generation",
              std::to_string(state.has_budget ? state.budget.generation.value() : 0));
  builder.add("action_budget_generation", std::to_string(action.budget_generation.value()));
  builder.add("budget_outcome",
              std::string(state.has_budget ? to_string(state.budget.outcome) : "UNKNOWN"));
  builder.add("attempt_count", std::to_string(action.attempt_count));
  builder.add("current_attempt", std::to_string(action.current_attempt.value()));
  builder.add("current_attempt_generation",
              std::to_string(action.current_attempt_generation.value()));
  builder.add("last_failure", std::string(to_string(action.last_failure)));
  builder.add("completion_status", std::string(to_string(action.completion_status)));
  builder.add("committed", action.committed ? "true" : "false");
  builder.add("ambiguous", action.ambiguous ? "true" : "false");
  builder.add("progress_committed", action.committed ? "true" : "false");
  builder.add("recovery_status", std::string(to_string(state.recovery)));
  builder.add("revalidation_required",
              action.state == ActionState::REVALIDATION_REQUIRED ? "true" : "false");
  builder.add("memory_binding_id", std::to_string(action.memory_binding_id.value()));
  builder.add("memory_binding_generation",
              std::to_string(action.memory_binding_generation.value()));
  builder.add("checkpoint_binding_id", std::to_string(action.checkpoint_binding_id.value()));
  builder.add("checkpoint_generation", std::to_string(action.checkpoint_generation.value()));
  builder.add("assignment_current",
              (state.has_assignment && state.current_assignment.current) ? "true" : "false");

  std::string deficit;
  if (action.state == ActionState::AUTHORIZED || action.state == ActionState::ADMITTED) {
    std::string message;
    if (!impl_->policy_allows_locked(action, message)) {
      deficit = "policy: " + message;
    } else if (!impl_->assignment_current_locked(message)) {
      deficit = "assignment: " + message;
    } else if ((impl_->budget_check_locked(action, message) != OutcomeCode::ACCEPTED)) {
      deficit = "budget: " + message;
    } else if (!impl_->dependencies_satisfied_locked(action, message)) {
      deficit = "dependency: " + message;
    }
  }
  builder.add("authority_deficit", deficit.empty() ? "none" : deficit);
  if (!action.last_error.empty()) {
    builder.add("last_error", action.last_error);
  }
  return builder.set_message("deterministic action explanation").build();
}

std::string InvariantReport::to_text() const {
  std::string out;
  out += ok ? "invariants: OK" : "invariants: VIOLATED";
  out += " checks=" + std::to_string(checks_run);
  out += " violations=" + std::to_string(violations.size());
  for (const InvariantViolation& violation : violations) {
    out += "\n  ";
    out += violation.name;
    out += ": ";
    out += violation.detail;
  }
  return out;
}

std::string RuntimeSnapshot::render_text() const {
  std::string out;
  out += "runtime=" + std::to_string(summary.runtime_id.value());
  out += " generation=" + std::to_string(summary.runtime_generation.value());
  out += " boot=" + std::to_string(summary.runtime_boot_id.value());
  out += " epoch=" + std::to_string(summary.runtime_epoch.value());
  out += " coordinator_epoch=" + std::to_string(summary.coordinator_epoch.value());
  out += " lifecycle=" + std::string(to_string(summary.lifecycle));
  out += " validity=" + std::string(to_string(validity));
  out += "\nrun=" + std::to_string(summary.run_id.value()) + "/" +
         std::to_string(summary.run_generation.value());
  out += " step=" + std::to_string(summary.step_id.value()) + "/" +
         std::to_string(summary.step_generation.value());
  out += " progress=" + std::string(to_string(summary.progress)) + "/" +
         std::to_string(summary.progress_generation.value());
  out += "\nactions=" + std::to_string(summary.total_actions) +
         " committed=" + std::to_string(summary.committed_actions) +
         " in_flight=" + std::to_string(summary.in_flight_actions) +
         " retry_pending=" + std::to_string(summary.retry_pending_actions) +
         " ambiguous=" + std::to_string(summary.ambiguous_actions) +
         " revalidation=" + std::to_string(summary.revalidation_actions);
  out += " attempts=" + std::to_string(summary.total_attempts);
  out += " checkpoints=" + std::to_string(summary.checkpoints);
  out += " fenced_boots=" + std::to_string(summary.fenced_boots);
  out += "\ndigest=" + std::to_string(semantic_digest);
  for (const ActionView& action : actions) {
    out += "\n  action " + std::to_string(action.action_id.value()) + "/" +
           std::to_string(action.action_generation.value()) + " " +
           std::string(to_string(action.kind)) + " " + std::string(to_string(action.state)) +
           " side_effect=" + std::string(to_string(action.side_effect)) +
           " attempts=" + std::to_string(action.attempt_count) +
           " committed=" + (action.committed ? "true" : "false");
  }
  return out;
}

std::string RuntimeSnapshot::render_json() const {
  std::string out = "{";
  out += "\"validity\":\"" + std::string(to_string(validity)) + "\",";
  out += "\"digest\":" + std::to_string(semantic_digest) + ",";
  out += "\"summary\":{";
  out += "\"runtime_id\":" + std::to_string(summary.runtime_id.value()) + ",";
  out += "\"runtime_generation\":" + std::to_string(summary.runtime_generation.value()) + ",";
  out += "\"runtime_boot_id\":" + std::to_string(summary.runtime_boot_id.value()) + ",";
  out += "\"runtime_epoch\":" + std::to_string(summary.runtime_epoch.value()) + ",";
  out += "\"coordinator_epoch\":" + std::to_string(summary.coordinator_epoch.value()) + ",";
  out += "\"agent_id\":" + std::to_string(summary.agent_id.value()) + ",";
  out += "\"agent_generation\":" + std::to_string(summary.agent_generation.value()) + ",";
  out += "\"agent_boot_id\":" + std::to_string(summary.agent_boot_id.value()) + ",";
  out += "\"lifecycle\":\"" + std::string(to_string(summary.lifecycle)) + "\",";
  out += "\"run_id\":" + std::to_string(summary.run_id.value()) + ",";
  out += "\"run_generation\":" + std::to_string(summary.run_generation.value()) + ",";
  out += "\"step_id\":" + std::to_string(summary.step_id.value()) + ",";
  out += "\"step_generation\":" + std::to_string(summary.step_generation.value()) + ",";
  out += "\"progress\":\"" + std::string(to_string(summary.progress)) + "\",";
  out += "\"progress_generation\":" + std::to_string(summary.progress_generation.value()) + ",";
  out += "\"cancellation\":\"" + std::string(to_string(summary.cancellation)) + "\",";
  out += "\"recovery\":\"" + std::string(to_string(summary.recovery)) + "\",";
  out += "\"total_actions\":" + std::to_string(summary.total_actions) + ",";
  out += "\"committed_actions\":" + std::to_string(summary.committed_actions) + ",";
  out += "\"in_flight_actions\":" + std::to_string(summary.in_flight_actions) + ",";
  out += "\"retry_pending_actions\":" + std::to_string(summary.retry_pending_actions) + ",";
  out += "\"ambiguous_actions\":" + std::to_string(summary.ambiguous_actions) + ",";
  out += "\"revalidation_actions\":" + std::to_string(summary.revalidation_actions) + ",";
  out += "\"total_attempts\":" + std::to_string(summary.total_attempts) + ",";
  out += "\"checkpoints\":" + std::to_string(summary.checkpoints) + ",";
  out += "\"fenced_boots\":" + std::to_string(summary.fenced_boots);
  out += "},\"actions\":[";
  for (std::size_t i = 0; i < actions.size(); ++i) {
    if (i != 0) {
      out.push_back(',');
    }
    const ActionView& action = actions[i];
    out += "{\"id\":" + std::to_string(action.action_id.value());
    out += ",\"generation\":" + std::to_string(action.action_generation.value());
    out += ",\"kind\":\"" + std::string(to_string(action.kind)) + "\"";
    out += ",\"side_effect\":\"" + std::string(to_string(action.side_effect)) + "\"";
    out += ",\"state\":\"" + std::string(to_string(action.state)) + "\"";
    out += ",\"attempts\":" + std::to_string(action.attempt_count);
    out += ",\"committed\":" + std::string(action.committed ? "true" : "false");
    out += ",\"last_failure\":\"" + std::string(to_string(action.last_failure)) + "\"";
    out += ",\"tool\":\"" + action.tool_name + "\"";
    out += ",\"model_target\":\"" + action.model_target + "\"";
    out.push_back('}');
  }
  out += "],\"attempts\":[";
  for (std::size_t i = 0; i < attempts.size(); ++i) {
    if (i != 0) {
      out.push_back(',');
    }
    const AttemptView& attempt = attempts[i];
    out += "{\"id\":" + std::to_string(attempt.attempt_id.value());
    out += ",\"generation\":" + std::to_string(attempt.attempt_generation.value());
    out += ",\"action_id\":" + std::to_string(attempt.action_id.value());
    out += ",\"state\":\"" + std::string(to_string(attempt.state)) + "\"";
    out += ",\"completion_status\":\"" + std::string(to_string(attempt.completion_status)) + "\"";
    out += ",\"runtime_boot_id\":" + std::to_string(attempt.runtime_boot_id.value());
    out += ",\"runtime_epoch\":" + std::to_string(attempt.runtime_epoch.value());
    out += ",\"backend_id\":" + std::to_string(attempt.backend_id.value());
    out += ",\"backend_generation\":" + std::to_string(attempt.backend_generation.value());
    out += ",\"superseded\":" + std::string(attempt.superseded ? "true" : "false");
    out += ",\"ambiguous\":" + std::string(attempt.ambiguous ? "true" : "false");
    out.push_back('}');
  }
  out += "],\"checkpoints\":[";
  for (std::size_t i = 0; i < checkpoints.size(); ++i) {
    if (i != 0) {
      out.push_back(',');
    }
    const CheckpointView& checkpoint = checkpoints[i];
    out += "{\"binding_id\":" + std::to_string(checkpoint.binding_id.value());
    out += ",\"generation\":" + std::to_string(checkpoint.generation.value());
    out += ",\"state\":\"" + std::string(to_string(checkpoint.state)) + "\"";
    out += ",\"restorable\":" + std::string(checkpoint.restorable ? "true" : "false");
    out += ",\"current\":" + std::string(checkpoint.current ? "true" : "false");
    out.push_back('}');
  }
  out += "],\"fenced_runtime_boots\":[";
  for (std::size_t i = 0; i < fenced_runtime_boots.size(); ++i) {
    if (i != 0) {
      out.push_back(',');
    }
    out += std::to_string(fenced_runtime_boots[i].value());
  }
  out += "]}";
  return out;
}

}  // namespace agent_runtime
