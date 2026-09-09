// Agent Runtime - suspend and resume example.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// Commits a checkpoint, suspends the runtime, resumes under fresh process
// authority with revalidated bindings, and continues.

#include <cstdio>
#include <memory>

#include "agent_runtime/agent_runtime.hpp"

using namespace agent_runtime;

int main() {
  auto clock = std::make_shared<LogicalClock>();
  auto tool = std::make_shared<ReferenceToolBackend>("reference-tool", 1);

  AgentRuntimeOptions options;
  options.runtime_id = AgentRuntimeId(4);
  options.agent_id = AgentId(4);
  options.clock = clock;
  options.policy = RuntimePolicy::embedded();
  options.tool_backend = tool;
  AgentRuntime runtime(std::move(options));
  (void)runtime.initialize();

  ToolBinding binding;
  binding.tool_name = "hash";
  binding.binding_generation = ToolCallGeneration(1);
  binding.backend_id = tool->incarnation().backend_id;
  binding.backend_generation = tool->incarnation().generation;
  binding.side_effect = SideEffectClass::PURE;
  binding.current = true;
  (void)runtime.bind_tool(binding);

  (void)runtime.start_run(AgentRunId(4), AgentRunGeneration(1));
  (void)runtime.begin_step();

  ActionSpec spec;
  spec.kind = ActionKind::TOOL_CALL;
  spec.tool_name = "hash";
  spec.side_effect = SideEffectClass::PURE;
  spec.input = "before-suspend";

  ActionHandle handle;
  (void)runtime.declare_action(spec, handle);
  (void)runtime.admit_action(handle.action_id, handle.action_generation);
  (void)runtime.authorize_action(handle.action_id, handle.action_generation);
  (void)runtime.dispatch_and_execute(handle.action_id, handle.action_generation);
  (void)runtime.commit_action(handle.action_id, handle.action_generation);

  const RuntimeSummary before = runtime.summary();
  const MutationResult checkpoint =
  runtime.request_checkpoint(CheckpointBindingId(1), CheckpointGeneration(1), "cp-1");
  std::printf("checkpoint request: %s\n", checkpoint.to_text().c_str());
  CheckpointBinding checkpoint_binding;
  checkpoint_binding.binding_id = CheckpointBindingId(1);
  checkpoint_binding.generation = CheckpointGeneration(1);
  checkpoint_binding.runtime_id = before.runtime_id;
  checkpoint_binding.runtime_generation = before.runtime_generation;
  checkpoint_binding.run_id = before.run_id;
  checkpoint_binding.run_generation = before.run_generation;
  checkpoint_binding.step_id = before.step_id;
  checkpoint_binding.step_generation = before.step_generation;
  checkpoint_binding.runtime_epoch = before.runtime_epoch;
  checkpoint_binding.progress_generation = before.progress_generation;
  checkpoint_binding.integrity_digest = "reference-digest";
  checkpoint_binding.restorable = true;
  std::printf("checkpoint accept: %s\n",
              runtime.accept_checkpoint(checkpoint_binding).to_text().c_str());

  std::printf("suspend: %s\n", runtime.suspend("waiting for external event").to_text().c_str());
  std::printf("lifecycle=%s\n", std::string(to_string(runtime.lifecycle())).c_str());

  ResumeContext context;
  context.runtime_boot_id = RuntimeBootId(2);
  context.agent_boot_id = AgentBootId(2);
  context.runtime_epoch = RuntimeEpoch(2);
  context.coordinator_epoch = CoordinatorEpoch(2);
  std::printf("resume: %s\n", runtime.resume(context).to_text().c_str());

  spec.input = "after-resume";
  (void)runtime.declare_action(spec, handle);
  (void)runtime.admit_action(handle.action_id, handle.action_generation);
  (void)runtime.authorize_action(handle.action_id, handle.action_generation);
  (void)runtime.dispatch_and_execute(handle.action_id, handle.action_generation);
  const MutationResult committed = runtime.commit_action(handle.action_id, handle.action_generation);
  std::printf("post-resume commit: %s\n", committed.to_text().c_str());

  const RuntimeSummary after = runtime.summary();
  std::printf("committed_actions=%u progress_generation=%llu fenced_boots=%u lifecycle=%s\n",
              after.committed_actions,
              static_cast<unsigned long long>(after.progress_generation.value()),
              after.fenced_boots, std::string(to_string(after.lifecycle)).c_str());
  return after.committed_actions == 2 ? 0 : 1;
}
