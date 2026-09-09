// Agent Runtime - core lifecycle, action, retry, budget, policy and commit tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include "runtime_fixture.hpp"
#include "test_framework.hpp"

using namespace agent_runtime;

namespace {

[[nodiscard]] std::uint64_t test_digest(std::string_view text) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const char raw : text) {
    hash ^= static_cast<std::uint8_t>(raw);
    hash *= 1099511628211ull;
  }
  return hash;
}

[[nodiscard]] ToolResponse make_tool_response(const ToolRequest& request,
                                              CompletionStatus status = CompletionStatus::SUCCEEDED,
                                              std::string output = "reference-output") {
  ToolResponse response;
  response.action_id = request.action_id;
  response.call_id = request.call_id;
  response.call_generation = request.call_generation;
  response.attempt_id = request.attempt_id;
  response.attempt_generation = request.attempt_generation;
  response.status = status;
  response.output = std::move(output);
  response.result_digest = test_digest(response.output);
  response.backend_generation = Generation<BackendTag>(1);
  return response;
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

}  // namespace

AR_TEST(lifecycle, initialize_and_ready) {
  artest::Fixture fixture = artest::make_fixture();
  AR_CHECK_EQ(std::string(to_string(fixture.runtime->lifecycle())), std::string("DECLARED"));
  AR_REQUIRE(fixture.runtime->initialize().accepted());
  AR_CHECK_EQ(std::string(to_string(fixture.runtime->lifecycle())), std::string("READY"));
  AR_CHECK(fixture.runtime->check_invariants().ok);
  AR_CHECK_EQ(fixture.runtime->id().value(), fixture.runtime_id.value());
}

AR_TEST(lifecycle, run_and_step_require_readiness) {
  artest::Fixture fixture = artest::make_fixture();
  const MutationResult before = fixture.runtime->start_run(AgentRunId(1), AgentRunGeneration(1));
  AR_CHECK_EQ(std::string(to_string(before.code)), std::string("REJECT_NOT_READY"));
  AR_REQUIRE(fixture.runtime->initialize().accepted());
  AR_REQUIRE(fixture.runtime->start_run(AgentRunId(1), AgentRunGeneration(1)).accepted());
  AR_CHECK_EQ(std::string(to_string(fixture.runtime->lifecycle())), std::string("RUNNING"));
  AR_REQUIRE(fixture.runtime->begin_step().accepted());
  AR_CHECK(fixture.runtime->summary().step_generation.valid());
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(lifecycle, illegal_transition_table) {
  AR_CHECK(is_legal_transition(RuntimeLifecycle::DECLARED, RuntimeLifecycle::INITIALIZING));
  AR_CHECK(!is_legal_transition(RuntimeLifecycle::DECLARED, RuntimeLifecycle::RUNNING));
  AR_CHECK(!is_legal_transition(RuntimeLifecycle::RETIRED, RuntimeLifecycle::RUNNING));
  AR_CHECK(is_legal_transition(RuntimeLifecycle::RUNNING, RuntimeLifecycle::SUSPENDING));
  AR_CHECK(is_legal_transition(RuntimeLifecycle::SUSPENDING, RuntimeLifecycle::SUSPENDED));
  AR_CHECK(!is_legal_transition(RuntimeLifecycle::SUSPENDED, RuntimeLifecycle::RUNNING));
  AR_CHECK(is_terminal(RuntimeLifecycle::CANCELLED));
  AR_CHECK(!is_terminal(RuntimeLifecycle::SUSPENDED));
  AR_CHECK(is_legal_transition(ActionState::AUTHORIZED, ActionState::DISPATCHED));
  AR_CHECK(!is_legal_transition(ActionState::COMMITTED, ActionState::AUTHORIZED));
  AR_CHECK(!is_legal_transition(ActionState::DECLARED, ActionState::COMMITTED));
}

AR_TEST(core, tool_action_commits_progress_once) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  const ActionHandle handle =
      declare_and_authorize(fixture, artest::tool_spec("hash", SideEffectClass::PURE, "payload"));

  const LocalExecutionResult execution =
      fixture.runtime->dispatch_and_execute(handle.action_id, handle.action_generation);
  AR_REQUIRE_MSG(execution.dispatch.accepted(), execution.dispatch.to_text());
  AR_REQUIRE_MSG(execution.completion.accepted(), execution.completion.to_text());

  RuntimeSummary summary = fixture.runtime->summary();
  AR_CHECK_EQ(summary.committed_actions, 0u);
  AR_CHECK_EQ(summary.in_flight_actions, 0u);
  AR_CHECK_EQ(summary.progress_generation.value(), 0ull);

  const MutationResult committed =
      fixture.runtime->commit_action(handle.action_id, handle.action_generation);
  AR_REQUIRE_MSG(committed.accepted(), committed.to_text());
  summary = fixture.runtime->summary();
  AR_CHECK_EQ(summary.committed_actions, 1u);
  AR_CHECK_EQ(summary.progress_generation.value(), 1ull);
  AR_CHECK_EQ(std::string(to_string(summary.progress)), std::string("COMMITTED"));

  const MutationResult again =
      fixture.runtime->commit_action(handle.action_id, handle.action_generation);
  AR_CHECK_EQ(std::string(to_string(again.code)), std::string("NO_CHANGE"));
  AR_CHECK_EQ(fixture.runtime->summary().committed_actions, 1u);
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(core, model_action_commits_progress) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  ActionSpec spec = artest::model_spec("summarize the runtime boundary");
  spec.side_effect = SideEffectClass::PURE;
  const ActionHandle handle = declare_and_authorize(fixture, spec);
  const LocalExecutionResult execution =
      fixture.runtime->dispatch_and_execute(handle.action_id, handle.action_generation);
  AR_REQUIRE_MSG(execution.dispatch.accepted(), execution.dispatch.to_text());
  AR_REQUIRE_MSG(execution.completion.accepted(), execution.completion.to_text());
  AR_REQUIRE(fixture.runtime->commit_action(handle.action_id, handle.action_generation).accepted());
  AR_CHECK_EQ(fixture.runtime->summary().committed_actions, 1u);
  AR_CHECK_EQ(fixture.model->invocation_count(), 1ull);
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(core, completion_is_not_commit) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  const ActionHandle handle =
      declare_and_authorize(fixture, artest::tool_spec("echo", SideEffectClass::READ_ONLY, "hi"));
  const DispatchResult dispatched = fixture.runtime->dispatch_action(
      handle.action_id, handle.action_generation, DispatchMode::EXTERNAL);
  AR_REQUIRE(dispatched.result.accepted());
  const MutationResult completion =
      fixture.runtime->submit_tool_completion(make_tool_response(dispatched.tool_request));
  AR_REQUIRE(completion.accepted());
  const std::vector<ActionView> actions = fixture.runtime->actions();
  AR_REQUIRE(actions.size() == 1);
  AR_CHECK_EQ(std::string(to_string(actions[0].state)), std::string("COMPLETED_UNVALIDATED"));
  AR_CHECK(!actions[0].committed);
  AR_CHECK_EQ(fixture.runtime->summary().progress_generation.value(), 0ull);
}

AR_TEST(stale, stale_action_generation) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  ActionHandle handle;
  AR_REQUIRE(fixture.runtime
                 ->declare_action(artest::tool_spec("hash", SideEffectClass::PURE, "x"), handle)
                 .accepted());
  const MutationResult admitted =
      fixture.runtime->admit_action(handle.action_id, ActionGeneration(999));
  AR_CHECK_EQ(std::string(to_string(admitted.code)), std::string("REJECT_STALE_ACTION"));
}

AR_TEST(stale, stale_attempt_generation) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  const ActionHandle handle =
      declare_and_authorize(fixture, artest::tool_spec("echo", SideEffectClass::READ_ONLY, "x"));
  const DispatchResult dispatched = fixture.runtime->dispatch_action(
      handle.action_id, handle.action_generation, DispatchMode::EXTERNAL);
  AR_REQUIRE(dispatched.result.accepted());
  ToolResponse response = make_tool_response(dispatched.tool_request);
  response.attempt_generation = AttemptGeneration(999);
  const MutationResult completion = fixture.runtime->submit_tool_completion(response);
  AR_CHECK_EQ(std::string(to_string(completion.code)), std::string("REJECT_STALE_ATTEMPT"));
  AR_CHECK_EQ(fixture.runtime->summary().committed_actions, 0u);
}

AR_TEST(stale, stale_run_generation) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  const MutationResult invalid =
      fixture.runtime->start_run(AgentRunId(101), AgentRunGeneration(0));
  AR_CHECK_EQ(std::string(to_string(invalid.code)), std::string("REJECT_INVALID"));
  const MutationResult conflict =
      fixture.runtime->start_run(AgentRunId(102), AgentRunGeneration(1));
  AR_CHECK_EQ(std::string(to_string(conflict.code)), std::string("REJECT_CONFLICT"));
}

AR_TEST(stale, stale_step_blocks_completion) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  const ActionHandle handle =
      declare_and_authorize(fixture, artest::tool_spec("echo", SideEffectClass::READ_ONLY, "x"));
  const DispatchResult dispatched = fixture.runtime->dispatch_action(
      handle.action_id, handle.action_generation, DispatchMode::EXTERNAL);
  AR_REQUIRE(dispatched.result.accepted());
  AR_REQUIRE(fixture.runtime->begin_step().accepted());
  const MutationResult completion =
      fixture.runtime->submit_tool_completion(make_tool_response(dispatched.tool_request));
  AR_CHECK_EQ(std::string(to_string(completion.code)), std::string("REJECT_STALE_STEP"));
}

AR_TEST(stale, stale_runtime_boot_after_recovery) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  const ActionHandle handle =
      declare_and_authorize(fixture,
                            artest::tool_spec("commit-token", SideEffectClass::COMMIT_TOKEN_REQUIRED,
                                              "payload", "op-key-1"));
  const DispatchResult dispatched = fixture.runtime->dispatch_action(
      handle.action_id, handle.action_generation, DispatchMode::EXTERNAL);
  AR_REQUIRE(dispatched.result.accepted());

  RecoveryContext context;
  context.runtime_boot_id = RuntimeBootId(2);
  context.agent_boot_id = AgentBootId(2);
  context.runtime_epoch = RuntimeEpoch(2);
  context.coordinator_epoch = CoordinatorEpoch(2);
  AR_REQUIRE_MSG(fixture.runtime->recover(context).accepted(), "recovery must be accepted");

  const MutationResult completion =
      fixture.runtime->submit_tool_completion(make_tool_response(dispatched.tool_request));
  AR_CHECK_EQ(std::string(to_string(completion.code)), std::string("REJECT_STALE_RUNTIME_BOOT"));
  AR_CHECK_EQ(fixture.runtime->summary().committed_actions, 0u);
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(stale, stale_policy_at_commit) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  const ActionHandle handle =
      declare_and_authorize(fixture, artest::tool_spec("hash", SideEffectClass::PURE, "x"));
  const LocalExecutionResult execution =
      fixture.runtime->dispatch_and_execute(handle.action_id, handle.action_generation);
  AR_REQUIRE(execution.completion.accepted());

  RuntimePolicy policy = RuntimePolicy::permissive();
  policy.require_assignment_for_dispatch = true;
  policy.generation = PolicyGeneration(2);
  AR_REQUIRE(fixture.runtime->set_policy(policy).accepted());
  const MutationResult committed =
      fixture.runtime->commit_action(handle.action_id, handle.action_generation);
  AR_CHECK_EQ(std::string(to_string(committed.code)), std::string("REJECT_STALE_POLICY"));
  AR_CHECK_EQ(fixture.runtime->summary().committed_actions, 0u);
}

AR_TEST(stale, stale_budget_at_dispatch) {
  artest::Fixture fixture = artest::make_fixture(true, true);
  artest::bring_up(fixture);
  AR_REQUIRE(fixture.runtime->set_budget(artest::make_budget(1)).accepted());
  const ActionHandle handle =
      declare_and_authorize(fixture, artest::tool_spec("hash", SideEffectClass::PURE, "x"));
  AR_REQUIRE(fixture.runtime->set_budget(artest::make_budget(2)).accepted());
  const DispatchResult dispatched = fixture.runtime->dispatch_action(
      handle.action_id, handle.action_generation, DispatchMode::EXTERNAL);
  AR_CHECK_EQ(std::string(to_string(dispatched.result.code)), std::string("REJECT_STALE_BUDGET"));
  AR_REQUIRE(fixture.runtime->authorize_action(handle.action_id, handle.action_generation).accepted());
  const DispatchResult retried = fixture.runtime->dispatch_action(
      handle.action_id, handle.action_generation, DispatchMode::EXTERNAL);
  AR_REQUIRE_MSG(retried.result.accepted(), retried.result.to_text());
}

AR_TEST(stale, stale_memory_binding) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  AR_REQUIRE(fixture.runtime->bind_memory(artest::make_memory_binding("ctx", 1, 1)).accepted());
  ActionSpec spec = artest::tool_spec("hash", SideEffectClass::PURE, "x");
  spec.memory_binding_identity = "ctx";
  ActionHandle handle;
  AR_REQUIRE(fixture.runtime->declare_action(spec, handle).accepted());
  AR_REQUIRE(fixture.runtime->admit_action(handle.action_id, handle.action_generation).accepted());
  AR_REQUIRE(fixture.runtime->authorize_action(handle.action_id, handle.action_generation).accepted());
  AR_REQUIRE(fixture.runtime->bind_memory(artest::make_memory_binding("ctx", 1, 2)).accepted());
  const MutationResult authorized =
      fixture.runtime->authorize_action(handle.action_id, handle.action_generation);
  AR_CHECK_EQ(std::string(to_string(authorized.code)), std::string("REJECT_STALE_MEMORY_BINDING"));
}

AR_TEST(stale, missing_assignment_blocks_authorization) {
  artest::Fixture fixture = artest::make_fixture(true, false);
  AR_REQUIRE(fixture.runtime->initialize().accepted());
  AR_REQUIRE(fixture.runtime->register_tool_backend(fixture.tool->incarnation(), fixture.tool)
                 .accepted());
  AR_REQUIRE(fixture.runtime
                 ->bind_tool(artest::make_tool_binding(fixture, "hash", SideEffectClass::PURE))
                 .accepted());
  AR_REQUIRE(fixture.runtime->start_run(AgentRunId(1), AgentRunGeneration(1)).accepted());
  AR_REQUIRE(fixture.runtime->begin_step().accepted());
  ActionHandle handle;
  AR_REQUIRE(fixture.runtime
                 ->declare_action(artest::tool_spec("hash", SideEffectClass::PURE, "x"), handle)
                 .accepted());
  AR_REQUIRE(fixture.runtime->admit_action(handle.action_id, handle.action_generation).accepted());
  const MutationResult authorized =
      fixture.runtime->authorize_action(handle.action_id, handle.action_generation);
  AR_CHECK_EQ(std::string(to_string(authorized.code)), std::string("REJECT_STALE_ASSIGNMENT"));
}

AR_TEST(stale, stale_assignment_generation) {
  artest::Fixture fixture = artest::make_fixture();
  AR_REQUIRE(fixture.runtime->initialize().accepted());
  AR_REQUIRE(fixture.runtime->bind_assignment(artest::make_assignment(fixture, 2, 2)).accepted());
  const MutationResult older =
      fixture.runtime->bind_assignment(artest::make_assignment(fixture, 1, 1));
  AR_CHECK_EQ(std::string(to_string(older.code)), std::string("REJECT_STALE_ASSIGNMENT"));
}

AR_TEST(stale, stale_tool_binding) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  ActionHandle handle;
  AR_REQUIRE(fixture.runtime
                 ->declare_action(artest::tool_spec("hash", SideEffectClass::PURE, "x"), handle)
                 .accepted());
  AR_REQUIRE(fixture.runtime->admit_action(handle.action_id, handle.action_generation).accepted());
  AR_REQUIRE(fixture.runtime->authorize_action(handle.action_id, handle.action_generation).accepted());
  AR_REQUIRE(fixture.runtime
                 ->bind_tool(artest::make_tool_binding(fixture, "hash", SideEffectClass::PURE, 2, 1))
                 .accepted());
  const MutationResult authorized =
      fixture.runtime->authorize_action(handle.action_id, handle.action_generation);
  AR_CHECK_EQ(std::string(to_string(authorized.code)), std::string("REJECT_STALE_TOOL_BINDING"));
}

AR_TEST(retry, idempotent_retry_uses_fresh_attempt_generation) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  const ActionHandle handle =
      declare_and_authorize(fixture, artest::tool_spec("hash", SideEffectClass::PURE, "retry-me"));
  fixture.tool->fail_next(RetryClass::TRANSIENT, "injected transient failure");
  const LocalExecutionResult first =
      fixture.runtime->dispatch_and_execute(handle.action_id, handle.action_generation);
  AR_REQUIRE(first.dispatch.accepted());
  AR_CHECK_EQ(std::string(to_string(first.completion.code)), std::string("ACCEPTED"));
  const std::vector<ActionView> after_failure = fixture.runtime->actions();
  AR_REQUIRE(after_failure.size() == 1);
  AR_CHECK_EQ(std::string(to_string(after_failure[0].state)), std::string("RETRY_PENDING"));
  AR_CHECK_EQ(after_failure[0].attempt_count, 1u);

  AR_REQUIRE(fixture.runtime->authorize_action(handle.action_id, handle.action_generation).accepted());
  const LocalExecutionResult second =
      fixture.runtime->dispatch_and_execute(handle.action_id, handle.action_generation);
  AR_REQUIRE_MSG(second.completion.accepted(), second.completion.to_text());
  AR_CHECK(second.attempt_generation > first.attempt_generation);
  AR_REQUIRE(fixture.runtime->commit_action(handle.action_id, handle.action_generation).accepted());
  AR_CHECK_EQ(fixture.runtime->summary().committed_actions, 1u);
  AR_CHECK_EQ(fixture.tool->invocation_count(), 2ull);
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(retry, exhaustion_is_explicit) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  ActionSpec spec = artest::tool_spec("hash", SideEffectClass::PURE, "exhaust");
  spec.has_retry_override = true;
  spec.retry = RetryPolicy{};
  spec.retry.max_attempts = 2;
  const ActionHandle handle = declare_and_authorize(fixture, spec);

  fixture.tool->fail_next(RetryClass::TRANSIENT, "failure one");
  AR_REQUIRE(fixture.runtime->dispatch_and_execute(handle.action_id, handle.action_generation)
                 .completion.accepted());
  AR_REQUIRE(fixture.runtime->authorize_action(handle.action_id, handle.action_generation).accepted());
  fixture.tool->fail_next(RetryClass::TRANSIENT, "failure two");
  const LocalExecutionResult second =
      fixture.runtime->dispatch_and_execute(handle.action_id, handle.action_generation);
  AR_CHECK_EQ(std::string(to_string(second.completion.code)),
              std::string("REJECT_RETRY_EXHAUSTED"));
  const std::vector<ActionView> actions = fixture.runtime->actions();
  AR_CHECK_EQ(std::string(to_string(actions[0].state)), std::string("FAILED"));
  AR_CHECK_EQ(fixture.runtime->summary().committed_actions, 0u);
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(retry, non_repeatable_ambiguous_is_never_auto_retried) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  const ActionHandle handle = declare_and_authorize(
      fixture,
      artest::tool_spec("non-repeatable", SideEffectClass::NON_REPEATABLE, "charge-card", "op-1"));
  fixture.tool->crash_after_side_effect_once();
  const LocalExecutionResult execution =
      fixture.runtime->dispatch_and_execute(handle.action_id, handle.action_generation);
  AR_CHECK_EQ(std::string(to_string(execution.completion.code)),
              std::string("AMBIGUOUS_COMPLETION"));

  const std::vector<ActionView> actions = fixture.runtime->actions();
  AR_REQUIRE(actions.size() == 1);
  AR_CHECK_EQ(std::string(to_string(actions[0].state)), std::string("REVALIDATION_REQUIRED"));
  AR_CHECK_EQ(fixture.runtime->summary().ambiguous_actions, 1u);
  AR_CHECK_EQ(fixture.runtime->summary().committed_actions, 0u);

  const MutationResult retry_attempt =
      fixture.runtime->authorize_action(handle.action_id, handle.action_generation);
  AR_CHECK_EQ(std::string(to_string(retry_attempt.code)),
              std::string("MANUAL_OR_EXTERNAL_RECONCILIATION_REQUIRED"));
  const MutationResult committed =
      fixture.runtime->commit_action(handle.action_id, handle.action_generation);
  AR_CHECK(!committed.accepted());
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(retry, effect_free_ambiguous_degrades_to_retry) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  const ActionHandle handle =
      declare_and_authorize(fixture, artest::tool_spec("hash", SideEffectClass::PURE, "x"));
  const DispatchResult dispatched = fixture.runtime->dispatch_action(
      handle.action_id, handle.action_generation, DispatchMode::EXTERNAL);
  AR_REQUIRE(dispatched.result.accepted());
  ToolResponse response = make_tool_response(dispatched.tool_request,
                                             CompletionStatus::AMBIGUOUS, "");
  const MutationResult completion = fixture.runtime->submit_tool_completion(response);
  AR_CHECK(completion.accepted());
  const std::vector<ActionView> actions = fixture.runtime->actions();
  AR_CHECK_EQ(std::string(to_string(actions[0].state)), std::string("RETRY_PENDING"));
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(policy, denied_tool_is_rejected) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  RuntimePolicy policy = RuntimePolicy::permissive();
  policy.require_assignment_for_dispatch = true;
  policy.generation = PolicyGeneration(2);
  policy.allowed_tools.insert("hash");
  AR_REQUIRE(fixture.runtime->set_policy(policy).accepted());
  ActionHandle handle;
  const MutationResult declared = fixture.runtime
                                      ->declare_action(artest::tool_spec("echo",
                                                                         SideEffectClass::READ_ONLY, "x"),
                                                       handle);
  AR_CHECK_EQ(std::string(to_string(declared.code)), std::string("REJECT_POLICY"));
}

AR_TEST(budget, denied_and_unknown_are_not_unlimited) {
  artest::Fixture fixture = artest::make_fixture(true, true);
  artest::bring_up(fixture);
  AR_REQUIRE(fixture.runtime->set_budget(artest::make_budget(1, BudgetOutcome::DENY)).accepted());
  ActionHandle handle;
  AR_REQUIRE(fixture.runtime
                 ->declare_action(artest::tool_spec("hash", SideEffectClass::PURE, "x"), handle)
                 .accepted());
  AR_REQUIRE(fixture.runtime->admit_action(handle.action_id, handle.action_generation).accepted());
  const MutationResult denied =
      fixture.runtime->authorize_action(handle.action_id, handle.action_generation);
  AR_CHECK_EQ(std::string(to_string(denied.code)), std::string("REJECT_BUDGET"));

  AR_REQUIRE(fixture.runtime->set_budget(artest::make_budget(2, BudgetOutcome::UNKNOWN)).accepted());
  const MutationResult unknown =
      fixture.runtime->authorize_action(handle.action_id, handle.action_generation);
  AR_CHECK_EQ(std::string(to_string(unknown.code)), std::string("REJECT_BUDGET"));
  AR_CHECK(!fixture.runtime->authorize_action(handle.action_id, handle.action_generation).accepted());
}

AR_TEST(memory, generation_move_invalidates_pending_actions) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  AR_REQUIRE(fixture.runtime->bind_memory(artest::make_memory_binding("ctx", 1, 1)).accepted());
  ActionSpec spec = artest::tool_spec("hash", SideEffectClass::PURE, "x");
  spec.memory_binding_identity = "ctx";
  ActionHandle handle;
  AR_REQUIRE(fixture.runtime->declare_action(spec, handle).accepted());
  AR_REQUIRE(fixture.runtime->admit_action(handle.action_id, handle.action_generation).accepted());
  AR_REQUIRE(fixture.runtime->authorize_action(handle.action_id, handle.action_generation).accepted());
  AR_REQUIRE(fixture.runtime->bind_memory(artest::make_memory_binding("ctx", 1, 7)).accepted());
  const std::vector<ActionView> actions = fixture.runtime->actions();
  AR_CHECK_EQ(std::string(to_string(actions[0].state)), std::string("REVALIDATION_REQUIRED"));
}

AR_TEST(checkpoint, commit_requires_accepted_compatible_checkpoint) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  ActionSpec spec = artest::tool_spec("hash", SideEffectClass::PURE, "checkpointed");
  spec.requires_checkpoint = true;
  const ActionHandle handle = declare_and_authorize(fixture, spec);
  const LocalExecutionResult execution =
      fixture.runtime->dispatch_and_execute(handle.action_id, handle.action_generation);
  AR_REQUIRE(execution.completion.accepted());

  const MutationResult premature =
      fixture.runtime->commit_action(handle.action_id, handle.action_generation);
  AR_CHECK_EQ(std::string(to_string(premature.code)), std::string("REVALIDATION_REQUIRED"));

  AR_REQUIRE(fixture.runtime
                 ->request_checkpoint(CheckpointBindingId(51), CheckpointGeneration(1), "cp-1")
                 .accepted());
  const RuntimeSummary summary = fixture.runtime->summary();
  CheckpointBinding binding;
  binding.binding_id = CheckpointBindingId(51);
  binding.generation = CheckpointGeneration(1);
  binding.runtime_id = summary.runtime_id;
  binding.runtime_generation = summary.runtime_generation;
  binding.run_id = summary.run_id;
  binding.run_generation = summary.run_generation;
  binding.step_id = summary.step_id;
  binding.step_generation = summary.step_generation;
  binding.runtime_epoch = summary.runtime_epoch;
  binding.progress_generation = summary.progress_generation;
  binding.checkpoint_identity = "cp-1";
  binding.integrity_digest = "digest";
  binding.restorable = true;
  AR_REQUIRE_MSG(fixture.runtime->accept_checkpoint(binding).accepted(), "checkpoint must accept");
  AR_REQUIRE(fixture.runtime->commit_action(handle.action_id, handle.action_generation).accepted());
  AR_CHECK_EQ(fixture.runtime->summary().committed_actions, 1u);
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(checkpoint, stale_checkpoint_generation_is_rejected) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  AR_REQUIRE(fixture.runtime
                 ->request_checkpoint(CheckpointBindingId(52), CheckpointGeneration(2), "cp-2")
                 .accepted());
  const MutationResult restore =
      fixture.runtime->request_restore(CheckpointBindingId(52), CheckpointGeneration(1));
  AR_CHECK_EQ(std::string(to_string(restore.code)), std::string("REJECT_STALE_CHECKPOINT"));
}

AR_TEST(cancel, cancelled_runtime_cannot_commit_success) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  const ActionHandle handle =
      declare_and_authorize(fixture, artest::tool_spec("hash", SideEffectClass::PURE, "x"));
  const LocalExecutionResult execution =
      fixture.runtime->dispatch_and_execute(handle.action_id, handle.action_generation);
  AR_REQUIRE(execution.completion.accepted());
  AR_REQUIRE(fixture.runtime->cancel("operator requested cancellation").accepted());
  AR_CHECK_EQ(std::string(to_string(fixture.runtime->lifecycle())), std::string("CANCELLED"));
  const MutationResult committed =
      fixture.runtime->commit_action(handle.action_id, handle.action_generation);
  AR_CHECK_EQ(std::string(to_string(committed.code)), std::string("REJECT_CANCELLED"));
  AR_CHECK_EQ(fixture.runtime->summary().committed_actions, 0u);

  ActionHandle new_handle;
  const MutationResult declared =
      fixture.runtime->declare_action(artest::tool_spec("hash", SideEffectClass::PURE, "y"),
                                      new_handle);
  AR_CHECK_EQ(std::string(to_string(declared.code)), std::string("REJECT_CANCELLED"));
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(cancel, non_repeatable_in_flight_becomes_ambiguous) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  const ActionHandle handle = declare_and_authorize(
      fixture,
      artest::tool_spec("non-repeatable", SideEffectClass::NON_REPEATABLE, "payload", "op-cancel"));
  const DispatchResult dispatched = fixture.runtime->dispatch_action(
      handle.action_id, handle.action_generation, DispatchMode::EXTERNAL);
  AR_REQUIRE(dispatched.result.accepted());
  AR_REQUIRE(fixture.runtime->cancel("stop now").accepted());
  AR_CHECK_EQ(fixture.runtime->summary().ambiguous_actions, 1u);
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(drain, refuses_new_work_and_completes) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  const ActionHandle handle =
      declare_and_authorize(fixture, artest::tool_spec("hash", SideEffectClass::PURE, "x"));
  const LocalExecutionResult execution =
      fixture.runtime->dispatch_and_execute(handle.action_id, handle.action_generation);
  AR_REQUIRE(execution.completion.accepted());
  AR_REQUIRE(fixture.runtime->commit_action(handle.action_id, handle.action_generation).accepted());

  const MutationResult drained = fixture.runtime->drain();
  AR_REQUIRE_MSG(drained.accepted(), drained.to_text());
  AR_CHECK_EQ(std::string(to_string(fixture.runtime->lifecycle())), std::string("COMPLETED"));
  ActionHandle new_handle;
  const MutationResult declared =
      fixture.runtime->declare_action(artest::tool_spec("hash", SideEffectClass::PURE, "y"),
                                      new_handle);
  AR_CHECK_EQ(std::string(to_string(declared.code)), std::string("REJECT_COMPLETED"));
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(suspend, suspend_and_resume_under_fresh_authority) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  const ActionHandle handle =
      declare_and_authorize(fixture, artest::tool_spec("hash", SideEffectClass::PURE, "x"));
  const LocalExecutionResult execution =
      fixture.runtime->dispatch_and_execute(handle.action_id, handle.action_generation);
  AR_REQUIRE(execution.completion.accepted());
  AR_REQUIRE(fixture.runtime->commit_action(handle.action_id, handle.action_generation).accepted());

  AR_REQUIRE_MSG(fixture.runtime->suspend("idle").accepted(), "suspend must reach SUSPENDED");
  AR_CHECK_EQ(std::string(to_string(fixture.runtime->lifecycle())), std::string("SUSPENDED"));
  AR_CHECK(fixture.runtime->check_invariants().ok);

  ResumeContext context;
  context.runtime_boot_id = RuntimeBootId(2);
  context.agent_boot_id = AgentBootId(2);
  context.runtime_epoch = RuntimeEpoch(2);
  context.coordinator_epoch = CoordinatorEpoch(2);
  context.assignment = artest::make_assignment(fixture, 1, 1);
  context.assignment->coordinator_epoch = CoordinatorEpoch(2);
  const MutationResult resumed = fixture.runtime->resume(context);
  AR_REQUIRE_MSG(resumed.accepted(), resumed.to_text());
  AR_CHECK_EQ(std::string(to_string(fixture.runtime->lifecycle())), std::string("RUNNING"));
  AR_CHECK_EQ(fixture.runtime->summary().committed_actions, 1u);

  const MutationResult stale_resume = fixture.runtime->resume(context);
  AR_CHECK(!stale_resume.accepted());
}

AR_TEST(suspend, resume_rejects_stale_boot_authority) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  AR_REQUIRE(fixture.runtime->suspend("idle").accepted());
  ResumeContext fresh;
  fresh.runtime_boot_id = RuntimeBootId(2);
  fresh.agent_boot_id = AgentBootId(2);
  fresh.runtime_epoch = RuntimeEpoch(2);
  fresh.coordinator_epoch = CoordinatorEpoch(2);
  fresh.assignment = artest::make_assignment(fixture, 1, 1);
  fresh.assignment->coordinator_epoch = CoordinatorEpoch(2);
  AR_REQUIRE_MSG(fixture.runtime->resume(fresh).accepted(), "fresh resume must succeed");

  AR_REQUIRE(fixture.runtime->suspend("idle again").accepted());
  ResumeContext stale_boot = fresh;
  stale_boot.runtime_boot_id = RuntimeBootId(1);
  stale_boot.agent_boot_id = AgentBootId(1);
  const MutationResult boot_rejected = fixture.runtime->resume(stale_boot);
  AR_CHECK_EQ(std::string(to_string(boot_rejected.code)),
              std::string("REJECT_STALE_RUNTIME_BOOT"));

  ResumeContext stale_epoch = fresh;
  stale_epoch.runtime_epoch = RuntimeEpoch(1);
  const MutationResult epoch_rejected = fixture.runtime->resume(stale_epoch);
  AR_CHECK_EQ(std::string(to_string(epoch_rejected.code)),
              std::string("REJECT_STALE_RUNTIME_EPOCH"));
}

AR_TEST(explain, deterministic_explanation) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  ActionHandle handle;
  AR_REQUIRE(fixture.runtime
                 ->declare_action(artest::tool_spec("hash", SideEffectClass::PURE, "x"), handle)
                 .accepted());
  const Explanation first = fixture.runtime->explain_action(handle.action_id, handle.action_generation);
  const Explanation second = fixture.runtime->explain_action(handle.action_id, handle.action_generation);
  AR_CHECK_EQ(first.to_text(), second.to_text());
  AR_CHECK(first.find("action_state") != nullptr);
  AR_CHECK(first.find("side_effect") != nullptr);
  AR_CHECK(first.find("authority_deficit") != nullptr);
  AR_CHECK(!first.to_json().empty());
}

AR_TEST(snapshot, validity_and_render) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  RuntimeSnapshot snapshot = fixture.runtime->snapshot();
  AR_CHECK_EQ(std::string(to_string(snapshot.validity)), std::string("CURRENT"));
  AR_CHECK(snapshot.authorizes_execution());
  AR_CHECK(!snapshot.render_text().empty());
  AR_CHECK(!snapshot.render_json().empty());
  AR_REQUIRE(fixture.runtime->suspend("idle").accepted());
  snapshot = fixture.runtime->snapshot();
  AR_CHECK_EQ(std::string(to_string(snapshot.validity)), std::string("REVALIDATION_REQUIRED"));
  AR_CHECK(!snapshot.authorizes_execution());
}

AR_TEST(limits, duplicate_identities_are_rejected) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  ActionHandle first;
  AR_REQUIRE(fixture.runtime
                 ->declare_action(artest::tool_spec("hash", SideEffectClass::PURE, "x"), first)
                 .accepted());
  ActionSpec bad = artest::tool_spec("hash", SideEffectClass::PURE, "y");
  bad.dependencies.push_back(first.action_id);
  bad.dependencies.push_back(first.action_id);
  ActionHandle second;
  const MutationResult declared = fixture.runtime->declare_action(bad, second);
  AR_CHECK_EQ(std::string(to_string(declared.code)), std::string("REJECT_INVALID"));
}

AR_TEST(limits, unsupported_side_effect_class_rejected) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  ActionSpec spec = artest::tool_spec("hash", SideEffectClass::PURE, "x");
  spec.side_effect = SideEffectClass::UNKNOWN;
  ActionHandle handle;
  const MutationResult declared = fixture.runtime->declare_action(spec, handle);
  AR_CHECK_EQ(std::string(to_string(declared.code)), std::string("REJECT_CONFLICT"));
}

AR_TEST(outcome, code_classification) {
  AR_CHECK(is_acceptance(OutcomeCode::ACCEPTED));
  AR_CHECK(is_acceptance(OutcomeCode::NO_CHANGE));
  AR_CHECK(is_rejection(OutcomeCode::REJECT_STALE_ATTEMPT));
  AR_CHECK(!is_rejection(OutcomeCode::ACCEPTED));
  AR_CHECK(requires_revalidation(OutcomeCode::REJECT_STALE_POLICY));
  AR_CHECK(is_ambiguous(OutcomeCode::AMBIGUOUS_COMPLETION));
  AR_CHECK_EQ(std::string(to_string(OutcomeCode::REJECT_STALE_RUNTIME_BOOT)),
              std::string("REJECT_STALE_RUNTIME_BOOT"));
  AR_CHECK_EQ(std::string(to_string(SideEffectClass::COMMIT_TOKEN_REQUIRED)),
              std::string("COMMIT_TOKEN_REQUIRED"));
  AR_CHECK(requires_reconciliation_on_ambiguity(SideEffectClass::NON_REPEATABLE));
  AR_CHECK(requires_reconciliation_on_ambiguity(SideEffectClass::UNKNOWN));
  AR_CHECK(!requires_reconciliation_on_ambiguity(SideEffectClass::IDEMPOTENT));
  AR_CHECK(!requires_reconciliation_on_ambiguity(SideEffectClass::PURE));
  AR_CHECK(supports_operation_key(SideEffectClass::IDEMPOTENT));
  AR_CHECK(is_effect_free(SideEffectClass::READ_ONLY));
}
