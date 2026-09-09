// Agent Runtime - deterministic test fixtures.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef AGENT_RUNTIME_TESTS_RUNTIME_FIXTURE_HPP
#define AGENT_RUNTIME_TESTS_RUNTIME_FIXTURE_HPP

#include <memory>
#include <string>

#include "agent_runtime/agent_runtime.hpp"
#include "test_framework.hpp"

namespace artest {

using namespace agent_runtime;

struct Fixture {
  std::shared_ptr<LogicalClock> clock;
  std::shared_ptr<ReferenceModelBackend> model;
  std::shared_ptr<ReferenceToolBackend> tool;
  std::unique_ptr<AgentRuntime> runtime;
  AgentRuntimeId runtime_id{};
  AgentId agent_id{};
  WorkId work_id{};
  SchedulerAssignmentId assignment_id{};
};

/// Builds a runtime with deterministic reference backends, a permissive policy
/// that still requires scheduler assignment authority, and a logical clock.
[[nodiscard]] inline Fixture make_fixture(bool require_assignment = true,
                                          bool require_budget = false) {
  Fixture fixture;
  fixture.clock = std::make_shared<LogicalClock>();
  fixture.model = std::make_shared<ReferenceModelBackend>("reference-model", 1);
  fixture.tool = std::make_shared<ReferenceToolBackend>("reference-tool", 1);
  fixture.runtime_id = AgentRuntimeId(9001);
  fixture.agent_id = AgentId(7001);
  fixture.work_id = WorkId(5001);
  fixture.assignment_id = SchedulerAssignmentId(3001);

  AgentRuntimeOptions options;
  options.runtime_id = fixture.runtime_id;
  options.agent_id = fixture.agent_id;
  options.clock = fixture.clock;
  options.policy = RuntimePolicy::permissive();
  options.policy.require_assignment_for_dispatch = require_assignment;
  options.policy.require_budget_for_dispatch = require_budget;
  options.model_backend = fixture.model;
  options.tool_backend = fixture.tool;
  fixture.runtime = std::make_unique<AgentRuntime>(std::move(options));
  return fixture;
}

[[nodiscard]] inline SchedulerAuthority make_assignment(const Fixture& fixture,
                                                        std::uint64_t assignment_generation = 1,
                                                        std::uint64_t work_generation = 1) {
  SchedulerAuthority authority;
  authority.assignment_id = fixture.assignment_id;
  authority.assignment_generation = AssignmentGeneration(assignment_generation);
  authority.agent_id = fixture.agent_id;
  authority.agent_generation = AgentGeneration(1);
  authority.agent_boot_id = AgentBootId(1);
  authority.scheduler_epoch = Generation<CoordinatorEpochTag>(1);
  authority.coordinator_epoch = CoordinatorEpoch(1);
  authority.dispatch_generation = DispatchGeneration(1);
  authority.work_id = fixture.work_id;
  authority.work_generation = WorkGeneration(work_generation);
  authority.lease_id = 4242;
  authority.lease_generation = 1;
  authority.current = true;
  return authority;
}

[[nodiscard]] inline ToolBinding make_tool_binding(const Fixture& fixture, const std::string& name,
                                                   SideEffectClass side_effect,
                                                   std::uint64_t binding_generation = 1,
                                                   std::uint64_t backend_generation = 1) {
  ToolBinding binding;
  binding.tool_name = name;
  binding.binding_generation = ToolCallGeneration(binding_generation);
  binding.backend_id = fixture.tool->incarnation().backend_id;
  binding.backend_generation = Generation<BackendTag>(backend_generation);
  binding.side_effect = side_effect;
  binding.max_input_bytes = 65536;
  binding.provenance = "test";
  binding.current = true;
  return binding;
}

[[nodiscard]] inline ModelBinding make_model_binding(const Fixture& fixture,
                                                     const std::string& target,
                                                     std::uint64_t binding_generation = 1,
                                                     std::uint64_t backend_generation = 1) {
  ModelBinding binding;
  binding.target = target;
  binding.binding_generation = ModelCallGeneration(binding_generation);
  binding.backend_id = fixture.model->incarnation().backend_id;
  binding.backend_generation = Generation<BackendTag>(backend_generation);
  binding.route_provenance = "test";
  binding.current = true;
  return binding;
}

[[nodiscard]] inline MemoryBinding make_memory_binding(const std::string& identity,
                                                       std::uint64_t generation = 1,
                                                       std::uint64_t external_generation = 1,
                                                       bool write_authority = true) {
  MemoryBinding binding;
  binding.binding_id = MemoryBindingId(6001);
  binding.generation = MemoryBindingGeneration(generation);
  binding.state_identity = identity;
  binding.compatibility = "v1";
  binding.external_generation = Generation<MemoryBindingTag>(external_generation);
  binding.read_authority = true;
  binding.write_authority = write_authority;
  binding.fresh = true;
  binding.provenance = "test";
  binding.digest = "digest";
  return binding;
}

[[nodiscard]] inline BudgetEvidence make_budget(std::uint64_t generation = 1,
                                                BudgetOutcome outcome = BudgetOutcome::ALLOW) {
  BudgetEvidence evidence;
  evidence.budget_id = BudgetId(8100);
  evidence.generation = BudgetGeneration(generation);
  evidence.outcome = outcome;
  evidence.remaining_actions = 1000;
  evidence.remaining_requests = 1000;
  evidence.remaining_tokens = 1000000;
  evidence.remaining_tool_calls = 1000;
  evidence.limits_known = true;
  evidence.provenance = "test";
  return evidence;
}

/// Brings a fixture to a running state with one open step.
inline void bring_up(Fixture& fixture) {
  AR_REQUIRE(fixture.runtime->initialize().accepted());
  AR_REQUIRE(fixture.runtime->bind_assignment(make_assignment(fixture)).accepted());
  AR_REQUIRE(fixture.runtime->register_tool_backend(fixture.tool->incarnation(), fixture.tool)
                 .accepted());
  AR_REQUIRE(fixture.runtime->register_model_backend(fixture.model->incarnation(), fixture.model)
                 .accepted());
  AR_REQUIRE(fixture.runtime
                 ->bind_tool(make_tool_binding(fixture, "hash", SideEffectClass::PURE))
                 .accepted());
  AR_REQUIRE(fixture.runtime
                 ->bind_tool(make_tool_binding(fixture, "echo", SideEffectClass::READ_ONLY))
                 .accepted());
  AR_REQUIRE(fixture.runtime
                 ->bind_tool(make_tool_binding(fixture, "commit-token",
                                               SideEffectClass::COMMIT_TOKEN_REQUIRED))
                 .accepted());
  AR_REQUIRE(fixture.runtime
                 ->bind_tool(make_tool_binding(fixture, "non-repeatable",
                                               SideEffectClass::NON_REPEATABLE))
                 .accepted());
  AR_REQUIRE(fixture.runtime
                 ->bind_tool(make_tool_binding(fixture, "scratch-transform",
                                               SideEffectClass::IDEMPOTENT))
                 .accepted());
  AR_REQUIRE(fixture.runtime->bind_model(make_model_binding(fixture, "reference-target"))
                 .accepted());
  AR_REQUIRE(fixture.runtime->start_run(AgentRunId(101), AgentRunGeneration(1)).accepted());
  AR_REQUIRE(fixture.runtime->begin_step().accepted());
}

[[nodiscard]] inline ActionSpec tool_spec(const std::string& tool, SideEffectClass side_effect,
                                          std::string input, std::string operation_key = {}) {
  ActionSpec spec;
  spec.kind = ActionKind::TOOL_CALL;
  spec.tool_name = tool;
  spec.side_effect = side_effect;
  spec.input = std::move(input);
  spec.operation_key = std::move(operation_key);
  spec.max_output_bytes = 4096;
  return spec;
}

[[nodiscard]] inline ActionSpec model_spec(std::string prompt) {
  ActionSpec spec;
  spec.kind = ActionKind::MODEL_CALL;
  spec.model_target = "reference-target";
  spec.input = std::move(prompt);
  spec.max_output_bytes = 4096;
  return spec;
}

}  // namespace artest

#endif  // AGENT_RUNTIME_TESTS_RUNTIME_FIXTURE_HPP
