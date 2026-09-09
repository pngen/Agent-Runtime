// Agent Runtime - embedded runtime example.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// Creates a run, executes one model action and one tool action through the
// deterministic reference backends, and commits durable progress exactly once
// per action.

#include <cstdio>
#include <memory>

#include "agent_runtime/agent_runtime.hpp"

using namespace agent_runtime;

int main() {
  auto clock = std::make_shared<LogicalClock>();
  auto model = std::make_shared<ReferenceModelBackend>("reference-model", 1);
  auto tool = std::make_shared<ReferenceToolBackend>("reference-tool", 1);

  AgentRuntimeOptions options;
  options.runtime_id = AgentRuntimeId(1);
  options.agent_id = AgentId(1);
  options.clock = clock;
  options.policy = RuntimePolicy::embedded();
  options.model_backend = model;
  options.tool_backend = tool;

  AgentRuntime runtime(std::move(options));
  if (!runtime.initialize().accepted()) {
    std::printf("initialize failed\n");
    return 1;
  }

  ModelBinding model_binding;
  model_binding.target = "reference-target";
  model_binding.binding_generation = ModelCallGeneration(1);
  model_binding.backend_id = model->incarnation().backend_id;
  model_binding.backend_generation = model->incarnation().generation;
  model_binding.current = true;
  (void)runtime.bind_model(model_binding);

  ToolBinding tool_binding;
  tool_binding.tool_name = "hash";
  tool_binding.binding_generation = ToolCallGeneration(1);
  tool_binding.backend_id = tool->incarnation().backend_id;
  tool_binding.backend_generation = tool->incarnation().generation;
  tool_binding.side_effect = SideEffectClass::PURE;
  tool_binding.current = true;
  (void)runtime.bind_tool(tool_binding);

  (void)runtime.start_run(AgentRunId(1), AgentRunGeneration(1));
  (void)runtime.begin_step();

  auto run_action = [&runtime](const ActionSpec& spec, const char* label) {
    ActionHandle handle;
    if (!runtime.declare_action(spec, handle).accepted()) {
      std::printf("%s: declare failed\n", label);
      return false;
    }
    if (!runtime.admit_action(handle.action_id, handle.action_generation).accepted()) {
      std::printf("%s: admit failed\n", label);
      return false;
    }
    if (!runtime.authorize_action(handle.action_id, handle.action_generation).accepted()) {
      std::printf("%s: authorize failed\n", label);
      return false;
    }
    const LocalExecutionResult execution =
  runtime.dispatch_and_execute(handle.action_id, handle.action_generation);
    if (!execution.completion.accepted()) {
      std::printf("%s: completion failed: %s\n", label, execution.completion.to_text().c_str());
      return false;
    }
    const MutationResult committed =
  runtime.commit_action(handle.action_id, handle.action_generation);
    if (!committed.accepted()) {
      std::printf("%s: commit failed: %s\n", label, committed.to_text().c_str());
      return false;
    }
    return true;
  };

  ActionSpec model_spec;
  model_spec.kind = ActionKind::MODEL_CALL;
  model_spec.model_target = "reference-target";
  model_spec.side_effect = SideEffectClass::PURE;
  model_spec.input = "summarize the runtime boundary";
  if (!run_action(model_spec, "model action")) {
    return 1;
  }

  ActionSpec tool_spec;
  tool_spec.kind = ActionKind::TOOL_CALL;
  tool_spec.tool_name = "hash";
  tool_spec.side_effect = SideEffectClass::PURE;
  tool_spec.input = "payload";
  if (!run_action(tool_spec, "tool action")) {
    return 1;
  }

  const RuntimeSummary summary = runtime.summary();
  std::printf("committed_actions=%u progress_generation=%llu lifecycle=%s\n",
              summary.committed_actions,
              static_cast<unsigned long long>(summary.progress_generation.value()),
              std::string(to_string(summary.lifecycle)).c_str());
  const InvariantReport report = runtime.check_invariants();
  std::printf("invariants: %s\n", report.ok ? "OK" : "VIOLATED");
  return report.ok ? 0 : 1;
}
