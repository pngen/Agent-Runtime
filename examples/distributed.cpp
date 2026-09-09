// Agent Runtime - distributed reference example.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// Starts a real tool worker process and a real agent worker process, then runs
// one governed tool action end to end over loopback TCP.

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "agent_runtime/agent_runtime.hpp"
#include "coordinator.hpp"
#include "process.hpp"

using namespace agent_runtime;
using namespace agent_runtime::reference;

namespace {

[[nodiscard]] bool read_port(ChildProcess& process, std::uint16_t& port, std::string& error) {
  std::string line;
  if (!process.read_line(line, error)) {
    return false;
  }
  if (line.rfind("PORT ", 0) != 0) {
    error = "worker did not report a port: " + line;
    return false;
  }
  port = static_cast<std::uint16_t>(std::strtoul(line.c_str() + 5, nullptr, 10));
  return port != 0;
}

}  // namespace

int main() {
  std::string error;

  ChildProcess tool_worker;
  std::uint16_t tool_port = 0;
  if (!tool_worker.spawn(sibling_executable("agent_runtime_tool_worker"), {"--generation", "1"},
                         error) ||
      !read_port(tool_worker, tool_port, error)) {
    std::printf("tool worker failed: %s\n", error.c_str());
    return 1;
  }
  std::printf("tool worker listening on 127.0.0.1:%u\n", static_cast<unsigned>(tool_port));

  ChildProcess model_worker;
  std::uint16_t model_port = 0;
  if (!model_worker.spawn(sibling_executable("agent_runtime_model_worker"), {"--generation", "1"},
                          error) ||
      !read_port(model_worker, model_port, error)) {
    std::printf("model worker failed: %s\n", error.c_str());
    return 1;
  }
  std::printf("model worker listening on 127.0.0.1:%u\n", static_cast<unsigned>(model_port));

  CoordinatorOptions options;
  options.runtime.runtime_id = AgentRuntimeId(8001);
  options.runtime.agent_id = AgentId(8002);
  options.runtime.clock = std::make_shared<SystemClock>();
  options.runtime.policy = RuntimePolicy::embedded();
  ReferenceCoordinator coordinator(std::move(options));
  if (!coordinator.start(error)) {
    std::printf("coordinator failed: %s\n", error.c_str());
    return 1;
  }
  std::printf("coordinator listening on 127.0.0.1:%u\n",
              static_cast<unsigned>(coordinator.port()));

  ChildProcess agent_worker;
  if (!agent_worker.spawn(sibling_executable("agent_runtime_agent_worker"),
                          {"--coordinator", "127.0.0.1:" + std::to_string(coordinator.port()),
                           "--tool", "127.0.0.1:" + std::to_string(tool_port), "--model",
                           "127.0.0.1:" + std::to_string(model_port), "--runtime-id", "8001",
                           "--agent-id", "8002", "--runtime-boot", "1", "--agent-boot", "1",
                           "--runtime-epoch", "1", "--coordinator-epoch", "1"},
                          error)) {
    std::printf("agent worker failed: %s\n", error.c_str());
    return 1;
  }

  AgentRuntime& runtime = coordinator.runtime();
  if (!coordinator.accept_agent(error)) {
    std::printf("accept failed: %s\n", error.c_str());
    return 1;
  }
  distributed::RegisterRuntimeMessage registration;
  if (!coordinator.await_registration(registration, error)) {
    std::printf("registration failed: %s\n", error.c_str());
    return 1;
  }
  std::printf("agent worker registered: runtime_boot=%llu agent_boot=%llu\n",
              static_cast<unsigned long long>(registration.runtime_boot_id.value()),
              static_cast<unsigned long long>(registration.agent_boot_id.value()));

  (void)runtime.initialize();
  BackendIncarnation tool_incarnation;
  tool_incarnation.backend_id = BackendId(0x7E570123ull);
  tool_incarnation.generation = Generation<BackendTag>(1);
  tool_incarnation.name = "reference-tool-worker";
  tool_incarnation.available = true;
  (void)runtime.register_remote_backend(tool_incarnation);
  ToolBinding binding;
  binding.tool_name = "hash";
  binding.binding_generation = ToolCallGeneration(1);
  binding.backend_id = tool_incarnation.backend_id;
  binding.backend_generation = tool_incarnation.generation;
  binding.side_effect = SideEffectClass::PURE;
  binding.current = true;
  (void)runtime.bind_tool(binding);
  (void)runtime.start_run(AgentRunId(800), AgentRunGeneration(1));
  (void)runtime.begin_step();

  ActionSpec spec;
  spec.kind = ActionKind::TOOL_CALL;
  spec.tool_name = "hash";
  spec.side_effect = SideEffectClass::PURE;
  spec.input = "distributed-payload";
  ActionHandle handle;
  DispatchResult dispatched;
  if (!coordinator.dispatch_tool_action(spec, handle, dispatched, error)) {
    std::printf("dispatch failed: %s\n", error.c_str());
    return 1;
  }
  ToolResponse response;
  if (!coordinator.await_tool_result(dispatched, response, error)) {
    std::printf("result failed: %s\n", error.c_str());
    return 1;
  }
  std::printf("tool result: %s digest=%llu\n", std::string(to_string(response.status)).c_str(),
              static_cast<unsigned long long>(response.result_digest));
  const MutationResult committed =
      coordinator.complete_and_commit(handle.action_id, handle.action_generation, response);
  std::printf("commit: %s\n", committed.to_text().c_str());

  const RuntimeSummary summary = runtime.summary();
  std::printf("committed_actions=%u attempts=%u lifecycle=%s invariants=%s\n",
              summary.committed_actions, summary.total_attempts,
              std::string(to_string(summary.lifecycle)).c_str(),
              runtime.check_invariants().ok ? "OK" : "VIOLATED");

  std::string ignored;
  (void)agent_worker.kill(ignored);
  (void)tool_worker.kill(ignored);
  (void)model_worker.kill(ignored);
  return summary.committed_actions == 1 ? 0 : 1;
}
