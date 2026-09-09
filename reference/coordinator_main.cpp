// Agent Runtime - reference coordinator process.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// The coordinator hosts one AgentRuntime and talks to an agent worker over real
// loopback TCP. It is used by the multiprocess proofs to demonstrate a genuine
// coordinator restart: the process is started twice against the same durable
// state file.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "agent_runtime/agent_runtime.hpp"
#include "coordinator.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using namespace agent_runtime;
using namespace agent_runtime::reference;

namespace {

struct Options {
  int phase = 1;
  std::string state_path;
  std::string tool_endpoint;
  std::string model_endpoint;
  std::uint64_t tool_generation = 1;
  std::uint64_t model_generation = 1;
  std::uint64_t runtime_id = 9001;
  std::uint64_t agent_id = 7001;
  std::uint64_t work_id = 5001;
  std::uint64_t assignment_id = 3001;
  std::uint64_t runtime_boot = 1;
  std::uint64_t agent_boot = 1;
  std::uint64_t runtime_epoch = 1;
  std::uint64_t coordinator_epoch = 1;
  std::uint16_t port = 0;
};

[[nodiscard]] bool parse(int argc, char** argv, Options& options, std::string& error) {
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    const bool has_value = i + 1 < argc;
    if (argument == "--phase" && has_value) {
      options.phase = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
    } else if (argument == "--port" && has_value) {
      options.port = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (argument == "--state" && has_value) {
      options.state_path = argv[++i];
    } else if (argument == "--tool" && has_value) {
      options.tool_endpoint = argv[++i];
    } else if (argument == "--model" && has_value) {
      options.model_endpoint = argv[++i];
    } else if (argument == "--tool-generation" && has_value) {
      options.tool_generation = std::strtoull(argv[++i], nullptr, 10);
    } else if (argument == "--model-generation" && has_value) {
      options.model_generation = std::strtoull(argv[++i], nullptr, 10);
    } else if (argument == "--runtime-id" && has_value) {
      options.runtime_id = std::strtoull(argv[++i], nullptr, 10);
    } else if (argument == "--agent-id" && has_value) {
      options.agent_id = std::strtoull(argv[++i], nullptr, 10);
    } else if (argument == "--work-id" && has_value) {
      options.work_id = std::strtoull(argv[++i], nullptr, 10);
    } else if (argument == "--assignment-id" && has_value) {
      options.assignment_id = std::strtoull(argv[++i], nullptr, 10);
    } else if (argument == "--runtime-boot" && has_value) {
      options.runtime_boot = std::strtoull(argv[++i], nullptr, 10);
    } else if (argument == "--agent-boot" && has_value) {
      options.agent_boot = std::strtoull(argv[++i], nullptr, 10);
    } else if (argument == "--runtime-epoch" && has_value) {
      options.runtime_epoch = std::strtoull(argv[++i], nullptr, 10);
    } else if (argument == "--coordinator-epoch" && has_value) {
      options.coordinator_epoch = std::strtoull(argv[++i], nullptr, 10);
    } else {
      error = "unknown argument: " + argument;
      return false;
    }
  }
  if (options.state_path.empty()) {
    error = "--state is required";
    return false;
  }
  return true;
}

[[nodiscard]] SchedulerAuthority make_assignment(const Options& options) {
  SchedulerAuthority authority;
  authority.assignment_id = SchedulerAssignmentId(options.assignment_id);
  authority.assignment_generation = AssignmentGeneration(1);
  authority.agent_id = AgentId(options.agent_id);
  authority.agent_generation = AgentGeneration(1);
  authority.agent_boot_id = AgentBootId(options.agent_boot);
  authority.scheduler_epoch = Generation<CoordinatorEpochTag>(1);
  authority.coordinator_epoch = CoordinatorEpoch(options.coordinator_epoch);
  authority.dispatch_generation = DispatchGeneration(1);
  authority.work_id = WorkId(options.work_id);
  authority.work_generation = WorkGeneration(1);
  authority.lease_id = 4242;
  authority.lease_generation = 1;
  authority.current = true;
  return authority;
}

[[nodiscard]] BackendIncarnation tool_incarnation(const Options& options) {
  BackendIncarnation incarnation;
  incarnation.backend_id = BackendId(0x7E570123ull);
  incarnation.generation = Generation<BackendTag>(options.tool_generation);
  incarnation.name = "reference-tool-worker";
  incarnation.available = true;
  return incarnation;
}

[[nodiscard]] BackendIncarnation model_incarnation(const Options& options) {
  BackendIncarnation incarnation;
  incarnation.backend_id = BackendId(0xA1B2C3D4ull);
  incarnation.generation = Generation<BackendTag>(options.model_generation);
  incarnation.name = "reference-model-worker";
  incarnation.available = true;
  return incarnation;
}

[[nodiscard]] ToolBinding make_tool_binding(const Options& options, const std::string& name,
                                            SideEffectClass side_effect) {
  ToolBinding binding;
  binding.tool_name = name;
  binding.binding_generation = ToolCallGeneration(1);
  binding.backend_id = tool_incarnation(options).backend_id;
  binding.backend_generation = tool_incarnation(options).generation;
  binding.side_effect = side_effect;
  binding.max_input_bytes = 65536;
  binding.provenance = "coordinator";
  binding.current = true;
  return binding;
}

[[nodiscard]] ModelBinding make_model_binding(const Options& options) {
  ModelBinding binding;
  binding.target = "reference-target";
  binding.binding_generation = ModelCallGeneration(1);
  binding.backend_id = model_incarnation(options).backend_id;
  binding.backend_generation = model_incarnation(options).generation;
  binding.route_provenance = "coordinator";
  binding.current = true;
  return binding;
}

[[nodiscard]] bool register_bindings(ReferenceCoordinator& coordinator, const Options& options,
                                     std::string& error) {
  const MutationResult tool_registration =
      coordinator.runtime().register_remote_backend(tool_incarnation(options));
  if (!tool_registration.accepted()) {
    error = "register remote tool backend: " + tool_registration.to_text();
    return false;
  }
  const MutationResult model_registration =
      coordinator.runtime().register_remote_backend(model_incarnation(options));
  if (!model_registration.accepted()) {
    error = "register remote model backend: " + model_registration.to_text();
    return false;
  }
  const std::pair<const char*, SideEffectClass> tools[] = {
      {"hash", SideEffectClass::PURE},
      {"echo", SideEffectClass::READ_ONLY},
      {"commit-token", SideEffectClass::COMMIT_TOKEN_REQUIRED},
      {"non-repeatable", SideEffectClass::NON_REPEATABLE},
  };
  for (const auto& entry : tools) {
    const MutationResult bound =
        coordinator.runtime().bind_tool(make_tool_binding(options, entry.first, entry.second));
    if (!bound.accepted()) {
      error = std::string("bind tool ") + entry.first + ": " + bound.to_text();
      return false;
    }
  }
  const MutationResult model_bound = coordinator.runtime().bind_model(make_model_binding(options));
  if (!model_bound.accepted()) {
    error = "bind model: " + model_bound.to_text();
    return false;
  }
  return true;
}

[[nodiscard]] ActionSpec hash_spec(const std::string& payload) {
  ActionSpec spec;
  spec.kind = ActionKind::TOOL_CALL;
  spec.tool_name = "hash";
  spec.side_effect = SideEffectClass::PURE;
  spec.input = payload;
  spec.max_output_bytes = 4096;
  return spec;
}

int run_phase_one(const Options& options) {
  CoordinatorOptions coordinator_options;
  coordinator_options.runtime.runtime_id = AgentRuntimeId(options.runtime_id);
  coordinator_options.runtime.agent_id = AgentId(options.agent_id);
  coordinator_options.runtime.runtime_boot_id = RuntimeBootId(options.runtime_boot);
  coordinator_options.runtime.agent_boot_id = AgentBootId(options.agent_boot);
  coordinator_options.runtime.runtime_epoch = RuntimeEpoch(options.runtime_epoch);
  coordinator_options.runtime.coordinator_epoch = CoordinatorEpoch(options.coordinator_epoch);
  coordinator_options.runtime.clock = std::make_shared<SystemClock>();
  coordinator_options.runtime.policy = RuntimePolicy::permissive();
  coordinator_options.runtime.policy.require_assignment_for_dispatch = true;
  coordinator_options.listen_port = options.port;

  ReferenceCoordinator coordinator(std::move(coordinator_options));
  std::string error;
  if (!coordinator.start(error)) {
    std::fprintf(stderr, "coordinator: %s\n", error.c_str());
    return 3;
  }
  std::printf("PORT %u\n", static_cast<unsigned>(coordinator.port()));
  std::fflush(stdout);

  AgentRuntime& runtime = coordinator.runtime();
  if (!runtime.initialize().accepted()) {
    std::fprintf(stderr, "coordinator: initialize failed\n");
    return 4;
  }
  if (!runtime.bind_assignment(make_assignment(options)).accepted()) {
    std::fprintf(stderr, "coordinator: assignment binding failed\n");
    return 5;
  }
  if (!register_bindings(coordinator, options, error)) {
    std::fprintf(stderr, "coordinator: %s\n", error.c_str());
    return 6;
  }
  if (!runtime.start_run(AgentRunId(101), AgentRunGeneration(1)).accepted()) {
    std::fprintf(stderr, "coordinator: start_run failed\n");
    return 7;
  }
  if (!runtime.begin_step().accepted()) {
    std::fprintf(stderr, "coordinator: begin_step failed\n");
    return 8;
  }

  if (!coordinator.accept_agent(error)) {
    std::fprintf(stderr, "coordinator: %s\n", error.c_str());
    return 9;
  }
  distributed::RegisterRuntimeMessage registration;
  if (!coordinator.await_registration(registration, error)) {
    std::fprintf(stderr, "coordinator: %s\n", error.c_str());
    return 10;
  }
  if (registration.runtime_boot_id != runtime.boot_id() ||
      registration.agent_boot_id != runtime.summary().agent_boot_id) {
    std::fprintf(stderr, "coordinator: agent worker registered a different incarnation\n");
    return 11;
  }

  ActionHandle handle;
  DispatchResult dispatched;
  if (!coordinator.dispatch_tool_action(hash_spec("phase-one-payload"), handle, dispatched, error)) {
    std::fprintf(stderr, "coordinator: %s\n", error.c_str());
    return 12;
  }
  ToolResponse response;
  if (!coordinator.await_tool_result(dispatched, response, error)) {
    std::fprintf(stderr, "coordinator: %s\n", error.c_str());
    return 13;
  }
  const MutationResult committed =
      coordinator.complete_and_commit(handle.action_id, handle.action_generation, response);
  if (!committed.accepted()) {
    std::fprintf(stderr, "coordinator: commit failed: %s\n", committed.to_text().c_str());
    return 14;
  }
  const MutationResult saved = runtime.save(options.state_path);
  if (!saved.accepted()) {
    std::fprintf(stderr, "coordinator: save failed: %s\n", saved.to_text().c_str());
    return 15;
  }
  const RuntimeSummary summary = runtime.summary();
  std::printf("PHASE1 committed=%u progress=%llu attempts=%u lifecycle=%s\n",
              summary.committed_actions,
              static_cast<unsigned long long>(summary.progress_generation.value()),
              summary.total_attempts, std::string(to_string(summary.lifecycle)).c_str());
  std::fflush(stdout);
  return 0;
}

int run_phase_two(const Options& options) {
  CoordinatorOptions coordinator_options;
  coordinator_options.runtime.runtime_id = AgentRuntimeId(options.runtime_id);
  coordinator_options.runtime.agent_id = AgentId(options.agent_id);
  coordinator_options.runtime.runtime_boot_id = RuntimeBootId(options.runtime_boot);
  coordinator_options.runtime.agent_boot_id = AgentBootId(options.agent_boot);
  coordinator_options.runtime.runtime_epoch = RuntimeEpoch(options.runtime_epoch);
  coordinator_options.runtime.coordinator_epoch = CoordinatorEpoch(options.coordinator_epoch);
  coordinator_options.runtime.clock = std::make_shared<SystemClock>();
  coordinator_options.runtime.policy = RuntimePolicy::permissive();
  coordinator_options.runtime.policy.require_assignment_for_dispatch = true;
  coordinator_options.listen_port = options.port;

  ReferenceCoordinator coordinator(std::move(coordinator_options));
  std::string error;
  if (!coordinator.start(error)) {
    std::fprintf(stderr, "coordinator: %s\n", error.c_str());
    return 3;
  }
  std::printf("PORT %u\n", static_cast<unsigned>(coordinator.port()));
  std::fflush(stdout);

  AgentRuntime& runtime = coordinator.runtime();
  const MutationResult loaded = runtime.load(options.state_path);
  if (!loaded.accepted()) {
    std::fprintf(stderr, "coordinator: load failed: %s\n", loaded.to_text().c_str());
    return 4;
  }
  RecoveryContext recovery;
  recovery.runtime_boot_id = RuntimeBootId(options.runtime_boot);
  recovery.agent_boot_id = AgentBootId(options.agent_boot);
  recovery.runtime_epoch = RuntimeEpoch(options.runtime_epoch);
  recovery.coordinator_epoch = CoordinatorEpoch(options.coordinator_epoch);
  const MutationResult recovered = runtime.recover(recovery);
  if (!recovered.accepted()) {
    std::fprintf(stderr, "coordinator: recover failed: %s\n", recovered.to_text().c_str());
    return 5;
  }

  if (!coordinator.accept_agent(error)) {
    std::fprintf(stderr, "coordinator: %s\n", error.c_str());
    return 6;
  }
  distributed::RegisterRuntimeMessage registration;
  if (!coordinator.await_registration(registration, error)) {
    std::fprintf(stderr, "coordinator: %s\n", error.c_str());
    return 7;
  }
  if (registration.runtime_boot_id != runtime.boot_id() ||
      registration.agent_boot_id != runtime.summary().agent_boot_id) {
    std::fprintf(stderr, "coordinator: agent worker incarnation does not match recovery\n");
    return 8;
  }
  if (!register_bindings(coordinator, options, error)) {
    std::fprintf(stderr, "coordinator: %s\n", error.c_str());
    return 9;
  }

  ResumeContext resume;
  resume.runtime_boot_id = RuntimeBootId(options.runtime_boot);
  resume.agent_boot_id = AgentBootId(options.agent_boot);
  resume.runtime_epoch = RuntimeEpoch(options.runtime_epoch);
  resume.coordinator_epoch = CoordinatorEpoch(options.coordinator_epoch);
  resume.assignment = make_assignment(options);
  const MutationResult resumed = runtime.resume(resume);
  if (!resumed.accepted()) {
    std::fprintf(stderr, "coordinator: resume failed: %s\n", resumed.to_text().c_str());
    return 10;
  }

  ActionHandle handle;
  DispatchResult dispatched;
  if (!coordinator.dispatch_tool_action(hash_spec("phase-two-payload"), handle, dispatched, error)) {
    std::fprintf(stderr, "coordinator: %s\n", error.c_str());
    return 11;
  }
  ToolResponse response;
  if (!coordinator.await_tool_result(dispatched, response, error)) {
    std::fprintf(stderr, "coordinator: %s\n", error.c_str());
    return 12;
  }
  const MutationResult committed =
      coordinator.complete_and_commit(handle.action_id, handle.action_generation, response);
  if (!committed.accepted()) {
    std::fprintf(stderr, "coordinator: commit failed: %s\n", committed.to_text().c_str());
    return 13;
  }
  const RuntimeSummary summary = runtime.summary();
  std::printf("PHASE2 committed=%u progress=%llu fenced=%u lifecycle=%s\n",
              summary.committed_actions,
              static_cast<unsigned long long>(summary.progress_generation.value()),
              summary.fenced_boots, std::string(to_string(summary.lifecycle)).c_str());
  std::fflush(stdout);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
  Options options;
  std::string error;
  if (!parse(argc, argv, options, error)) {
    std::fprintf(stderr, "coordinator: %s\n", error.c_str());
    return 2;
  }
  return options.phase == 2 ? run_phase_two(options) : run_phase_one(options);
}
