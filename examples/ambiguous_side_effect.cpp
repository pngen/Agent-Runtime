// Agent Runtime - ambiguous side-effect example.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// A non-repeatable action completes physically but its completion cannot be
// proven. The runtime preserves the uncertainty instead of inventing safety.

#include <cstdio>
#include <memory>

#include "agent_runtime/agent_runtime.hpp"

using namespace agent_runtime;

int main() {
  auto clock = std::make_shared<LogicalClock>();
  auto tool = std::make_shared<ReferenceToolBackend>("reference-tool", 1);

  AgentRuntimeOptions options;
  options.runtime_id = AgentRuntimeId(3);
  options.agent_id = AgentId(3);
  options.clock = clock;
  options.policy = RuntimePolicy::embedded();
  options.tool_backend = tool;
  AgentRuntime runtime(std::move(options));
  (void)runtime.initialize();

  ToolBinding binding;
  binding.tool_name = "non-repeatable";
  binding.binding_generation = ToolCallGeneration(1);
  binding.backend_id = tool->incarnation().backend_id;
  binding.backend_generation = tool->incarnation().generation;
  binding.side_effect = SideEffectClass::NON_REPEATABLE;
  binding.current = true;
  (void)runtime.bind_tool(binding);

  (void)runtime.start_run(AgentRunId(3), AgentRunGeneration(1));
  (void)runtime.begin_step();

  ActionSpec spec;
  spec.kind = ActionKind::TOOL_CALL;
  spec.tool_name = "non-repeatable";
  spec.side_effect = SideEffectClass::NON_REPEATABLE;
  spec.input = "wire-transfer-1000";
  spec.operation_key = "transfer-99";

  ActionHandle handle;
  (void)runtime.declare_action(spec, handle);
  (void)runtime.admit_action(handle.action_id, handle.action_generation);
  (void)runtime.authorize_action(handle.action_id, handle.action_generation);

  tool->crash_after_side_effect_once();
  const LocalExecutionResult execution =
  runtime.dispatch_and_execute(handle.action_id, handle.action_generation);
  std::printf("completion: %s\n", execution.completion.to_text().c_str());

  const MutationResult retry = runtime.authorize_action(handle.action_id, handle.action_generation);
  std::printf("retry attempt: %s\n", retry.to_text().c_str());
  const MutationResult commit = runtime.commit_action(handle.action_id, handle.action_generation);
  std::printf("commit attempt: %s\n", commit.to_text().c_str());
  std::printf("ambiguous_actions=%u committed_actions=%u side_effect_present=%s\n",
              runtime.summary().ambiguous_actions, runtime.summary().committed_actions,
              tool->has_receipt("transfer-99") ? "true" : "false");
  return runtime.summary().ambiguous_actions == 1 ? 0 : 1;
}
