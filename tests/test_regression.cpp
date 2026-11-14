// Agent Runtime - regression tests for audited correctness defects.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "runtime_fixture.hpp"
#include "test_framework.hpp"

using namespace agent_runtime;

namespace {

[[nodiscard]] std::string scratch_file(const std::string& name) {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "agent_runtime_regression";
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  const std::filesystem::path path = directory / name;
  std::filesystem::remove(path, error);
  return path.string();
}

void remove_file(const std::string& path) {
  std::error_code error;
  std::filesystem::remove(path, error);
}

[[nodiscard]] ActionHandle declare_and_authorize(artest::Fixture& fixture, const ActionSpec& spec) {
  ActionHandle handle;
  const MutationResult declared = fixture.runtime->declare_action(spec, handle);
  AR_REQUIRE_MSG(declared.accepted(), declared.to_text());
  const MutationResult admitted =
      fixture.runtime->admit_action(handle.action_id, handle.action_generation);
  AR_REQUIRE_MSG(admitted.accepted(), admitted.to_text());
  const MutationResult authorized =
      fixture.runtime->authorize_action(handle.action_id, handle.action_generation);
  AR_REQUIRE_MSG(authorized.accepted(), authorized.to_text());
  return handle;
}

[[nodiscard]] ToolResponse response_for(const ToolRequest& request, CompletionStatus status,
                                        std::string output = "result") {
  ToolResponse response;
  response.action_id = request.action_id;
  response.call_id = request.call_id;
  response.call_generation = request.call_generation;
  response.attempt_id = request.attempt_id;
  response.attempt_generation = request.attempt_generation;
  response.status = status;
  response.output = std::move(output);
  response.backend_generation = Generation<BackendTag>(1);
  return response;
}

[[nodiscard]] RecoveryContext recovery_at(std::uint64_t boot) {
  RecoveryContext context;
  context.runtime_boot_id = RuntimeBootId(boot);
  context.agent_boot_id = AgentBootId(boot);
  context.runtime_epoch = RuntimeEpoch(boot);
  context.coordinator_epoch = CoordinatorEpoch(boot);
  return context;
}

[[nodiscard]] ResumeContext resume_at(const artest::Fixture& fixture, std::uint64_t boot) {
  ResumeContext context;
  context.runtime_boot_id = RuntimeBootId(boot);
  context.agent_boot_id = AgentBootId(boot);
  context.runtime_epoch = RuntimeEpoch(boot);
  context.coordinator_epoch = CoordinatorEpoch(boot);
  context.assignment = artest::make_assignment(fixture, 1, 1);
  context.assignment->coordinator_epoch = CoordinatorEpoch(boot);
  return context;
}

/// Re-registers the in-process backends and makes every tool and model
/// binding current again, as a restarted process does before resuming.
void rebind_everything(artest::Fixture& fixture) {
  const MutationResult tool_backend =
      fixture.runtime->register_tool_backend(fixture.tool->incarnation(), fixture.tool);
  AR_REQUIRE_MSG(tool_backend.accepted(), tool_backend.to_text());
  const MutationResult model_backend =
      fixture.runtime->register_model_backend(fixture.model->incarnation(), fixture.model);
  AR_REQUIRE_MSG(model_backend.accepted(), model_backend.to_text());
  const std::pair<const char*, SideEffectClass> tools[] = {
      {"hash", SideEffectClass::PURE},
      {"echo", SideEffectClass::READ_ONLY},
      {"commit-token", SideEffectClass::COMMIT_TOKEN_REQUIRED},
      {"non-repeatable", SideEffectClass::NON_REPEATABLE},
      {"scratch-transform", SideEffectClass::IDEMPOTENT},
  };
  for (const auto& entry : tools) {
    const MutationResult bound =
        fixture.runtime->bind_tool(artest::make_tool_binding(fixture, entry.first, entry.second));
    AR_REQUIRE_MSG(bound.accepted(), bound.to_text());
  }
  const MutationResult model =
      fixture.runtime->bind_model(artest::make_model_binding(fixture, "reference-target"));
  AR_REQUIRE_MSG(model.accepted(), model.to_text());
}

void accept_restorable_checkpoint(artest::Fixture& fixture, std::uint64_t id) {
  AR_REQUIRE(fixture.runtime
                 ->request_checkpoint(CheckpointBindingId(id), CheckpointGeneration(1), "cp")
                 .accepted());
  const RuntimeSummary summary = fixture.runtime->summary();
  CheckpointBinding binding;
  binding.binding_id = CheckpointBindingId(id);
  binding.generation = CheckpointGeneration(1);
  binding.runtime_id = summary.runtime_id;
  binding.runtime_generation = summary.runtime_generation;
  binding.run_id = summary.run_id;
  binding.run_generation = summary.run_generation;
  binding.step_id = summary.step_id;
  binding.step_generation = summary.step_generation;
  binding.runtime_epoch = summary.runtime_epoch;
  binding.progress_generation = summary.progress_generation;
  binding.checkpoint_identity = "cp";
  binding.integrity_digest = "digest";
  binding.restorable = true;
  const MutationResult accepted = fixture.runtime->accept_checkpoint(binding);
  AR_REQUIRE_MSG(accepted.accepted(), accepted.to_text());
}

[[nodiscard]] std::string code_of(const MutationResult& result) {
  return std::string(to_string(result.code));
}

}  // namespace

// --- unresolved side effects survive every loss of process authority --------

AR_TEST(regression, reload_never_redispatches_an_in_flight_non_repeatable_action) {
  const std::string path = scratch_file("in_flight.state");
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  const ActionHandle handle = declare_and_authorize(
      fixture,
      artest::tool_spec("non-repeatable", SideEffectClass::NON_REPEATABLE, "charge", "op-1"));
  const DispatchResult dispatched =
      fixture.runtime->dispatch_action(handle.action_id, handle.action_generation);
  AR_REQUIRE_MSG(dispatched.result.accepted(), dispatched.result.to_text());
  // The request may already have reached the backend when the process dies.
  AR_REQUIRE_MSG(fixture.runtime->save(path).accepted(), "save must succeed");

  artest::Fixture restored = artest::make_fixture();
  AR_REQUIRE(restored.runtime->load(path).accepted());
  AR_REQUIRE(restored.runtime->recover(recovery_at(2)).accepted());
  rebind_everything(restored);
  const MutationResult resumed = restored.runtime->resume(resume_at(restored, 2));
  AR_REQUIRE_MSG(resumed.accepted(), resumed.to_text());

  AR_CHECK_EQ(restored.runtime->summary().ambiguous_actions, 1u);
  const MutationResult reauthorized =
      restored.runtime->authorize_action(handle.action_id, handle.action_generation);
  AR_CHECK_EQ(code_of(reauthorized), std::string("MANUAL_OR_EXTERNAL_RECONCILIATION_REQUIRED"));
  const DispatchResult again =
      restored.runtime->dispatch_action(handle.action_id, handle.action_generation);
  AR_CHECK_MSG(!again.result.accepted(), "a possibly executed side effect must not run twice");
  AR_CHECK(restored.runtime->check_invariants().ok);
  remove_file(path);
}

AR_TEST(regression, recovery_never_redispatches_an_executed_non_repeatable_action) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  const ActionHandle handle = declare_and_authorize(
      fixture,
      artest::tool_spec("non-repeatable", SideEffectClass::NON_REPEATABLE, "charge", "op-2"));
  const LocalExecutionResult executed =
      fixture.runtime->dispatch_and_execute(handle.action_id, handle.action_generation);
  AR_REQUIRE_MSG(executed.completion.accepted(), executed.completion.to_text());
  // The side effect happened; the process dies before the commit.
  AR_REQUIRE(fixture.runtime->recover(recovery_at(2)).accepted());
  AR_CHECK_EQ(fixture.runtime->summary().ambiguous_actions, 1u);
  rebind_everything(fixture);
  const MutationResult resumed = fixture.runtime->resume(resume_at(fixture, 2));
  AR_REQUIRE_MSG(resumed.accepted(), resumed.to_text());

  const MutationResult reauthorized =
      fixture.runtime->authorize_action(handle.action_id, handle.action_generation);
  AR_CHECK_EQ(code_of(reauthorized), std::string("MANUAL_OR_EXTERNAL_RECONCILIATION_REQUIRED"));
  const DispatchResult again =
      fixture.runtime->dispatch_action(handle.action_id, handle.action_generation);
  AR_CHECK_MSG(!again.result.accepted(), "an executed side effect must not run twice");
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(regression, restore_records_an_in_flight_non_repeatable_action_as_ambiguous) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  accept_restorable_checkpoint(fixture, 61);
  const ActionHandle handle = declare_and_authorize(
      fixture,
      artest::tool_spec("non-repeatable", SideEffectClass::NON_REPEATABLE, "charge", "op-3"));
  const DispatchResult dispatched =
      fixture.runtime->dispatch_action(handle.action_id, handle.action_generation);
  AR_REQUIRE_MSG(dispatched.result.accepted(), dispatched.result.to_text());
  const MutationResult restore =
      fixture.runtime->request_restore(CheckpointBindingId(61), CheckpointGeneration(1));
  AR_REQUIRE_MSG(restore.accepted(), restore.to_text());
  AR_CHECK_EQ(fixture.runtime->summary().ambiguous_actions, 1u);
  AR_REQUIRE(fixture.runtime->apply_restore(CheckpointBindingId(61), CheckpointGeneration(1))
                 .accepted());
  rebind_everything(fixture);
  const MutationResult resumed = fixture.runtime->resume(resume_at(fixture, 2));
  AR_REQUIRE_MSG(resumed.accepted(), resumed.to_text());
  const MutationResult reauthorized =
      fixture.runtime->authorize_action(handle.action_id, handle.action_generation);
  AR_CHECK_EQ(code_of(reauthorized), std::string("MANUAL_OR_EXTERNAL_RECONCILIATION_REQUIRED"));
}

AR_TEST(regression, runtime_failure_does_not_report_an_in_flight_side_effect_as_failed) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  const ActionHandle handle = declare_and_authorize(
      fixture,
      artest::tool_spec("non-repeatable", SideEffectClass::NON_REPEATABLE, "charge", "op-4"));
  const DispatchResult dispatched =
      fixture.runtime->dispatch_action(handle.action_id, handle.action_generation);
  AR_REQUIRE_MSG(dispatched.result.accepted(), dispatched.result.to_text());
  AR_REQUIRE(fixture.runtime->fail("host process failed").accepted());
  AR_CHECK_EQ(fixture.runtime->summary().ambiguous_actions, 1u);
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(regression, memory_invalidation_never_starts_a_second_attempt_while_one_is_in_flight) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  AR_REQUIRE(fixture.runtime->bind_memory(artest::make_memory_binding("ctx")).accepted());
  ActionSpec spec =
      artest::tool_spec("non-repeatable", SideEffectClass::NON_REPEATABLE, "charge", "op-5");
  spec.memory_binding_identity = "ctx";
  const ActionHandle handle = declare_and_authorize(fixture, spec);
  const DispatchResult first =
      fixture.runtime->dispatch_action(handle.action_id, handle.action_generation);
  AR_REQUIRE_MSG(first.result.accepted(), first.result.to_text());

  AR_REQUIRE(fixture.runtime
                 ->invalidate_memory(MemoryBindingId(6001), MemoryBindingGeneration(1), "moved")
                 .accepted());
  AR_REQUIRE(fixture.runtime->bind_memory(artest::make_memory_binding("ctx")).accepted());
  const MutationResult reauthorized =
      fixture.runtime->authorize_action(handle.action_id, handle.action_generation);
  AR_CHECK_MSG(!reauthorized.accepted(), reauthorized.to_text());
  const DispatchResult second =
      fixture.runtime->dispatch_action(handle.action_id, handle.action_generation);
  AR_CHECK_MSG(!second.result.accepted(), "the in-flight attempt is still outstanding");
  AR_CHECK_EQ(fixture.runtime->summary().total_attempts, 1u);
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

// --- terminal lifecycles stay terminal --------------------------------------

AR_TEST(regression, cancelled_runtime_cannot_complete_through_a_pending_restore) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  accept_restorable_checkpoint(fixture, 62);
  AR_REQUIRE(fixture.runtime->request_restore(CheckpointBindingId(62), CheckpointGeneration(1))
                 .accepted());
  AR_REQUIRE(fixture.runtime->cancel("operator").accepted());
  const MutationResult applied =
      fixture.runtime->apply_restore(CheckpointBindingId(62), CheckpointGeneration(1));
  AR_CHECK_MSG(!applied.accepted(), applied.to_text());
  const MutationResult completed = fixture.runtime->complete("done");
  AR_CHECK_MSG(!completed.accepted(), completed.to_text());
  AR_CHECK_EQ(std::string(to_string(fixture.runtime->lifecycle())), std::string("CANCELLED"));
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(regression, completed_or_failed_runtime_cannot_be_recovered_or_restored) {
  {
    artest::Fixture fixture = artest::make_fixture();
    artest::bring_up(fixture);
    accept_restorable_checkpoint(fixture, 63);
    AR_REQUIRE(fixture.runtime->complete("done").accepted());
    const MutationResult restore =
        fixture.runtime->request_restore(CheckpointBindingId(63), CheckpointGeneration(1));
    AR_CHECK_MSG(!restore.accepted(), restore.to_text());
    const MutationResult recovered = fixture.runtime->recover(recovery_at(2));
    AR_CHECK_MSG(!recovered.accepted(), recovered.to_text());
    AR_CHECK_EQ(std::string(to_string(fixture.runtime->lifecycle())), std::string("COMPLETED"));
  }
  {
    artest::Fixture fixture = artest::make_fixture();
    artest::bring_up(fixture);
    AR_REQUIRE(fixture.runtime->fail("broken").accepted());
    const MutationResult recovered = fixture.runtime->recover(recovery_at(2));
    AR_CHECK_MSG(!recovered.accepted(), recovered.to_text());
    AR_CHECK_EQ(std::string(to_string(fixture.runtime->lifecycle())), std::string("FAILED"));
  }
}

AR_TEST(regression, reload_keeps_a_cancelled_runtime_cancelled) {
  const std::string path = scratch_file("cancelled.state");
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  AR_REQUIRE(fixture.runtime->cancel("operator").accepted());
  AR_REQUIRE(fixture.runtime->save(path).accepted());

  artest::Fixture restored = artest::make_fixture();
  AR_REQUIRE(restored.runtime->load(path).accepted());
  AR_CHECK_EQ(std::string(to_string(restored.runtime->lifecycle())), std::string("CANCELLED"));
  const MutationResult completed = restored.runtime->complete("done");
  AR_CHECK_MSG(!completed.accepted(), completed.to_text());
  AR_CHECK(restored.runtime->check_invariants().ok);
  remove_file(path);
}

AR_TEST(regression, admission_after_cancellation_reports_the_cancellation) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  ActionHandle handle;
  AR_REQUIRE(fixture.runtime
                 ->declare_action(artest::tool_spec("hash", SideEffectClass::PURE, "x"), handle)
                 .accepted());
  AR_REQUIRE(fixture.runtime->cancel("operator").accepted());
  const MutationResult admitted =
      fixture.runtime->admit_action(handle.action_id, handle.action_generation);
  AR_CHECK_EQ(code_of(admitted), std::string("REJECT_CANCELLED"));
}

// --- incarnation fencing ----------------------------------------------------

AR_TEST(regression, current_incarnation_cannot_be_fenced) {
  const std::string path = scratch_file("fenced.state");
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  const MutationResult fenced =
      fixture.runtime->fence_boot(fixture.runtime->boot_id(), AgentBootId(1), "operator");
  AR_CHECK_MSG(!fenced.accepted(), fenced.to_text());
  AR_CHECK(fixture.runtime->check_invariants().ok);
  AR_REQUIRE(fixture.runtime->save(path).accepted());
  artest::Fixture restored = artest::make_fixture();
  const MutationResult loaded = restored.runtime->load(path);
  AR_CHECK_MSG(loaded.accepted(), loaded.to_text());
  remove_file(path);
}

AR_TEST(regression, resume_requires_a_new_runtime_boot_for_a_new_incarnation) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  AR_REQUIRE(fixture.runtime->suspend("idle").accepted());
  ResumeContext context = resume_at(fixture, 2);
  context.runtime_boot_id = fixture.runtime->boot_id();
  const MutationResult resumed = fixture.runtime->resume(context);
  AR_CHECK_EQ(code_of(resumed), std::string("REJECT_STALE_RUNTIME_BOOT"));
  AR_CHECK(fixture.runtime->check_invariants().ok);
  const MutationResult fresh = fixture.runtime->resume(resume_at(fixture, 2));
  AR_CHECK_MSG(fresh.accepted(), fresh.to_text());
  AR_CHECK(fixture.runtime->begin_step().accepted());
}

AR_TEST(regression, reloaded_incarnation_must_recover_before_resuming) {
  const std::string path = scratch_file("crashed.state");
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  AR_REQUIRE(fixture.runtime->save(path).accepted());

  artest::Fixture restored = artest::make_fixture();
  AR_REQUIRE(restored.runtime->load(path).accepted());
  rebind_everything(restored);
  // The crashed process ran as boot 1; continuing under boot 1 would leave it
  // unfenced while its workers may still be alive.
  const MutationResult resumed = restored.runtime->resume(resume_at(restored, 1));
  AR_CHECK_MSG(!resumed.accepted(), resumed.to_text());
  AR_CHECK_EQ(restored.runtime->summary().fenced_boots, 0u);
  AR_REQUIRE(restored.runtime->recover(recovery_at(2)).accepted());
  const MutationResult after_recovery = restored.runtime->resume(resume_at(restored, 2));
  AR_CHECK_MSG(after_recovery.accepted(), after_recovery.to_text());
  remove_file(path);
}

// --- drain ------------------------------------------------------------------

AR_TEST(regression, drain_lets_already_authorized_actions_finish) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  const ActionHandle handle =
      declare_and_authorize(fixture, artest::tool_spec("hash", SideEffectClass::PURE, "x"));
  const MutationResult first = fixture.runtime->drain();
  AR_CHECK_EQ(code_of(first), std::string("DEFERRED"));
  const LocalExecutionResult executed =
      fixture.runtime->dispatch_and_execute(handle.action_id, handle.action_generation);
  AR_REQUIRE_MSG(executed.dispatch.accepted(), executed.dispatch.to_text());
  AR_REQUIRE_MSG(executed.completion.accepted(), executed.completion.to_text());
  AR_REQUIRE(fixture.runtime->commit_action(handle.action_id, handle.action_generation).accepted());
  const MutationResult drained = fixture.runtime->drain();
  AR_CHECK_MSG(drained.accepted(), drained.to_text());
  AR_CHECK_EQ(std::string(to_string(fixture.runtime->lifecycle())), std::string("COMPLETED"));
  AR_CHECK_EQ(fixture.runtime->summary().committed_actions, 1u);
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(regression, drain_without_waiting_leaves_no_dispatchable_action) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  RuntimePolicy policy = RuntimePolicy::permissive();
  policy.require_assignment_for_dispatch = true;
  policy.drain_waits_for_authorized_actions = false;
  policy.generation = policy.generation.next();
  AR_REQUIRE(fixture.runtime->set_policy(policy).accepted());
  const ActionHandle handle =
      declare_and_authorize(fixture, artest::tool_spec("hash", SideEffectClass::PURE, "x"));
  const MutationResult drained = fixture.runtime->drain();
  AR_REQUIRE_MSG(drained.accepted(), drained.to_text());
  const DispatchResult late =
      fixture.runtime->dispatch_action(handle.action_id, handle.action_generation);
  AR_CHECK(!late.result.accepted());
  const InvariantReport report = fixture.runtime->check_invariants();
  AR_CHECK_MSG(report.ok, report.violations.empty() ? std::string()
                                                    : report.violations.front().name);
}

AR_TEST(regression, drain_does_not_complete_over_an_ambiguous_side_effect) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  const ActionHandle handle = declare_and_authorize(
      fixture,
      artest::tool_spec("non-repeatable", SideEffectClass::NON_REPEATABLE, "charge", "op-6"));
  const DispatchResult dispatched =
      fixture.runtime->dispatch_action(handle.action_id, handle.action_generation);
  AR_REQUIRE_MSG(dispatched.result.accepted(), dispatched.result.to_text());
  const MutationResult ambiguous = fixture.runtime->submit_tool_completion(
      response_for(dispatched.tool_request, CompletionStatus::AMBIGUOUS, ""));
  AR_CHECK_EQ(code_of(ambiguous), std::string("AMBIGUOUS_COMPLETION"));
  AR_CHECK_EQ(code_of(fixture.runtime->complete("done")),
              std::string("MANUAL_OR_EXTERNAL_RECONCILIATION_REQUIRED"));
  const MutationResult drained = fixture.runtime->drain();
  AR_CHECK_MSG(!drained.accepted(), drained.to_text());
  AR_CHECK(fixture.runtime->lifecycle() != RuntimeLifecycle::COMPLETED);
}

// --- durable state ----------------------------------------------------------

AR_TEST(regression, action_input_larger_than_the_string_limit_survives_a_reload) {
  const std::string path = scratch_file("large_input.state");
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  ActionHandle handle;
  const MutationResult declared = fixture.runtime->declare_action(
      artest::tool_spec("hash", SideEffectClass::PURE, std::string(6000, 'i')), handle);
  AR_REQUIRE_MSG(declared.accepted(), declared.to_text());
  AR_REQUIRE(fixture.runtime->save(path).accepted());
  artest::Fixture restored = artest::make_fixture();
  const MutationResult loaded = restored.runtime->load(path);
  AR_CHECK_MSG(loaded.accepted(), loaded.to_text());
  remove_file(path);
}

AR_TEST(regression, long_terminal_reason_survives_a_reload) {
  const std::string path = scratch_file("long_reason.state");
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  AR_REQUIRE(fixture.runtime->cancel(std::string(6000, 'r')).accepted());
  AR_REQUIRE(fixture.runtime->save(path).accepted());
  artest::Fixture restored = artest::make_fixture();
  const MutationResult loaded = restored.runtime->load(path);
  AR_CHECK_MSG(loaded.accepted(), loaded.to_text());
  remove_file(path);
}

AR_TEST(regression, unbounded_free_text_survives_a_reload) {
  const std::string path = scratch_file("free_text.state");
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  // Provenance and digests are accepted without the identifier string limit.
  MemoryBinding memory = artest::make_memory_binding("ctx");
  memory.provenance = std::string(5000, 'p');
  memory.digest = std::string(5000, 'd');
  AR_REQUIRE(fixture.runtime->bind_memory(memory).accepted());
  AR_REQUIRE(fixture.runtime->cancel(std::string(2u * 1024u * 1024u, 'r')).accepted());
  const MutationResult saved = fixture.runtime->save(path);
  AR_REQUIRE_MSG(saved.accepted(), saved.to_text());
  artest::Fixture restored = artest::make_fixture();
  const MutationResult loaded = restored.runtime->load(path);
  AR_CHECK_MSG(loaded.accepted(), loaded.to_text());
  remove_file(path);
}

// --- accepted states stay loadable -------------------------------------------

namespace {

[[nodiscard]] std::string first_violation(const InvariantReport& report) {
  return report.violations.empty() ? std::string()
                                   : report.violations.front().name + ": " +
                                         report.violations.front().detail;
}

void expect_loadable(artest::Fixture& fixture, const std::string& name) {
  const InvariantReport report = fixture.runtime->check_invariants();
  AR_CHECK_MSG(report.ok, first_violation(report));
  const std::string path = scratch_file(name);
  const MutationResult saved = fixture.runtime->save(path);
  AR_CHECK_MSG(saved.accepted(), saved.to_text());
  artest::Fixture restored = artest::make_fixture();
  const MutationResult loaded = restored.runtime->load(path);
  AR_CHECK_MSG(loaded.accepted(), loaded.to_text());
  remove_file(path);
}

[[nodiscard]] RuntimePolicy next_policy(std::uint64_t generation) {
  RuntimePolicy policy = RuntimePolicy::permissive();
  policy.require_assignment_for_dispatch = true;
  policy.generation = PolicyGeneration(generation);
  return policy;
}

}  // namespace

AR_TEST(regression, completion_cancels_work_that_never_started) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  const ActionHandle handle =
      declare_and_authorize(fixture, artest::tool_spec("hash", SideEffectClass::PURE, "x"));
  AR_REQUIRE(fixture.runtime->complete("done").accepted());
  const DispatchResult late =
      fixture.runtime->dispatch_action(handle.action_id, handle.action_generation);
  AR_CHECK(!late.result.accepted());
  expect_loadable(fixture, "completed.state");
}

AR_TEST(regression, retired_runtime_is_not_resumable) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  AR_REQUIRE(fixture.runtime->suspend("idle").accepted());
  AR_REQUIRE(fixture.runtime->resume(resume_at(fixture, 2)).accepted());
  AR_REQUIRE(fixture.runtime->complete("done").accepted());
  AR_REQUIRE(fixture.runtime->retire().accepted());
  expect_loadable(fixture, "retired.state");
}

AR_TEST(regression, resume_does_not_admit_actions_that_were_never_admitted) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  ActionHandle handle;
  AR_REQUIRE(fixture.runtime
                 ->declare_action(artest::tool_spec("hash", SideEffectClass::PURE, "x"), handle)
                 .accepted());
  AR_REQUIRE(fixture.runtime->suspend("idle").accepted());
  AR_REQUIRE(fixture.runtime->resume(resume_at(fixture, 2)).accepted());
  const MutationResult admitted =
      fixture.runtime->admit_action(handle.action_id, handle.action_generation);
  AR_CHECK_EQ(code_of(admitted), std::string("ACCEPTED"));
  const MutationResult authorized =
      fixture.runtime->authorize_action(handle.action_id, handle.action_generation);
  AR_CHECK_MSG(authorized.accepted(), authorized.to_text());
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(regression, new_assignment_does_not_admit_work_into_a_suspended_runtime) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  (void)declare_and_authorize(fixture, artest::tool_spec("hash", SideEffectClass::PURE, "x"));
  AR_REQUIRE(fixture.runtime->set_policy(next_policy(2)).accepted());
  AR_REQUIRE(fixture.runtime->suspend("idle").accepted());
  AR_REQUIRE(fixture.runtime->bind_assignment(artest::make_assignment(fixture, 2, 1)).accepted());
  expect_loadable(fixture, "suspended.state");
}

AR_TEST(regression, new_assignment_respects_the_active_action_bound) {
  artest::Fixture fixture = artest::make_fixture();
  AgentRuntimeOptions options;
  options.runtime_id = fixture.runtime_id;
  options.agent_id = fixture.agent_id;
  options.clock = fixture.clock;
  options.policy = RuntimePolicy::permissive();
  options.policy.require_assignment_for_dispatch = true;
  options.model_backend = fixture.model;
  options.tool_backend = fixture.tool;
  options.limits = default_resource_limits();
  options.limits.max_active_actions = 2;
  options.limits.max_parallel_read_actions = 2;
  fixture.runtime = std::make_unique<AgentRuntime>(std::move(options));
  artest::bring_up(fixture);
  (void)declare_and_authorize(fixture, artest::tool_spec("hash", SideEffectClass::PURE, "a"));
  (void)declare_and_authorize(fixture, artest::tool_spec("hash", SideEffectClass::PURE, "b"));
  // The policy change sends both back to revalidation; two more are admitted.
  AR_REQUIRE(fixture.runtime->set_policy(next_policy(2)).accepted());
  (void)declare_and_authorize(fixture, artest::tool_spec("hash", SideEffectClass::PURE, "c"));
  (void)declare_and_authorize(fixture, artest::tool_spec("hash", SideEffectClass::PURE, "d"));
  AR_REQUIRE(fixture.runtime->bind_assignment(artest::make_assignment(fixture, 2, 1)).accepted());
  const InvariantReport report = fixture.runtime->check_invariants();
  AR_CHECK_MSG(report.ok, first_violation(report));
}

// --- external providers ------------------------------------------------------

namespace {

/// A scheduler adapter that consults the runtime it serves, as the class
/// contract allows: providers are called with no canonical lock held.
class ConsultingScheduler final : public SchedulerAuthorityProvider {
 public:
  explicit ConsultingScheduler(SchedulerAuthority authority) : authority_(authority) {}
  void attach(AgentRuntime* runtime) noexcept { runtime_ = runtime; }
  [[nodiscard]] std::optional<SchedulerAuthority> current_assignment(
      AgentRuntimeId runtime_id) override {
    ++calls;
    if (runtime_ != nullptr && runtime_->id() == runtime_id) {
      observed_epoch = runtime_->summary().runtime_epoch.value();
    }
    return authority_;
  }
  std::uint32_t calls = 0;
  std::uint64_t observed_epoch = 0;

 private:
  SchedulerAuthority authority_;
  AgentRuntime* runtime_ = nullptr;
};

class RefreshingMemory final : public MemoryProvider {
 public:
  [[nodiscard]] std::optional<MemoryBinding> revalidate(const MemoryBinding& binding) override {
    ++calls;
    MemoryBinding refreshed = binding;
    refreshed.fresh = true;
    refreshed.read_authority = true;
    refreshed.write_authority = true;
    return refreshed;
  }
  std::uint32_t calls = 0;
};

}  // namespace

AR_TEST(regression, resume_and_revalidation_consult_providers_outside_the_runtime_lock) {
  artest::Fixture fixture = artest::make_fixture();
  SchedulerAuthority authority = artest::make_assignment(fixture, 1, 1);
  authority.coordinator_epoch = CoordinatorEpoch(2);
  auto scheduler = std::make_shared<ConsultingScheduler>(authority);
  auto memory = std::make_shared<RefreshingMemory>();
  AgentRuntimeOptions options;
  options.runtime_id = fixture.runtime_id;
  options.agent_id = fixture.agent_id;
  options.clock = fixture.clock;
  options.policy = RuntimePolicy::permissive();
  options.policy.require_assignment_for_dispatch = true;
  options.model_backend = fixture.model;
  options.tool_backend = fixture.tool;
  options.scheduler_authority = scheduler;
  options.memory_provider = memory;
  fixture.runtime = std::make_unique<AgentRuntime>(std::move(options));
  scheduler->attach(fixture.runtime.get());
  artest::bring_up(fixture);
  AR_REQUIRE(fixture.runtime->bind_memory(artest::make_memory_binding("ctx")).accepted());
  AR_REQUIRE(fixture.runtime->suspend("idle").accepted());

  ResumeContext context = resume_at(fixture, 2);
  context.assignment.reset();
  const MutationResult resumed = fixture.runtime->resume(context);
  AR_REQUIRE_MSG(resumed.accepted(), resumed.to_text());
  AR_CHECK_EQ(scheduler->calls, 1u);
  AR_CHECK_EQ(memory->calls, 1u);
  AR_CHECK_EQ(std::string(to_string(fixture.runtime->lifecycle())), std::string("RUNNING"));

  const MutationResult revalidated = fixture.runtime->revalidate_assignment();
  AR_CHECK_MSG(revalidated.accepted(), revalidated.to_text());
  AR_CHECK_EQ(scheduler->calls, 2u);
  AR_CHECK_EQ(scheduler->observed_epoch, 2ull);
  AR_CHECK(fixture.runtime->check_invariants().ok);
}
