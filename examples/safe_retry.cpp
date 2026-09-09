// Agent Runtime - safe retry example.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// An idempotent action fails transiently, is retried under a fresh attempt
// generation, and commits durable progress exactly once.

#include <cstdio>
#include <memory>

#include "agent_runtime/agent_runtime.hpp"

using namespace agent_runtime;

int main() {
  auto clock = std::make_shared<LogicalClock>();
  auto tool = std::make_shared<ReferenceToolBackend>("reference-tool", 1);

  AgentRuntimeOptions options;
  options.runtime_id = AgentRuntimeId(2);
  options.agent_id = AgentId(2);
  options.clock = clock;
  options.policy = RuntimePolicy::embedded();
  options.tool_backend = tool;
  AgentRuntime runtime(std::move(options));
  (void)runtime.initialize();

  ToolBinding binding;
  binding.tool_name = "commit-token";
  binding.binding_generation = ToolCallGeneration(1);
  binding.backend_id = tool->incarnation().backend_id;
  binding.backend_generation = tool->incarnation().generation;
  binding.side_effect = SideEffectClass::COMMIT_TOKEN_REQUIRED;
  binding.current = true;
  (void)runtime.bind_tool(binding);

  (void)runtime.start_run(AgentRunId(2), AgentRunGeneration(1));
  (void)runtime.begin_step();

  ActionSpec spec;
  spec.kind = ActionKind::TOOL_CALL;
  spec.tool_name = "commit-token";
  spec.side_effect = SideEffectClass::COMMIT_TOKEN_REQUIRED;
  spec.input = "charge-100";
  spec.operation_key = "order-4711";

  ActionHandle handle;
  (void)runtime.declare_action(spec, handle);
  (void)runtime.admit_action(handle.action_id, handle.action_generation);
  (void)runtime.authorize_action(handle.action_id, handle.action_generation);

  tool->fail_next(RetryClass::TRANSIENT, "simulated transient network failure");
  const LocalExecutionResult first =
  runtime.dispatch_and_execute(handle.action_id, handle.action_generation);
  std::printf("first attempt: %s\n", first.completion.to_text().c_str());

  (void)runtime.authorize_action(handle.action_id, handle.action_generation);
  const LocalExecutionResult second =
  runtime.dispatch_and_execute(handle.action_id, handle.action_generation);
  std::printf("second attempt: attempt_generation=%llu completion=%s\n",
              static_cast<unsigned long long>(second.attempt_generation.value()),
              second.completion.to_text().c_str());

  const MutationResult committed = runtime.commit_action(handle.action_id, handle.action_generation);
  std::printf("commit: %s\n", committed.to_text().c_str());
  std::printf("physical invocations=%llu committed_actions=%u\n",
              static_cast<unsigned long long>(tool->invocation_count()),
              runtime.summary().committed_actions);
  return runtime.summary().committed_actions == 1 ? 0 : 1;
}
