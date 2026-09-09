// Agent Runtime - real multiprocess proofs.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// Every proof here uses real independent operating-system processes, real
// loopback TCP and real process death. Nothing is simulated with flags.

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "agent_runtime/agent_runtime.hpp"
#include "coordinator.hpp"
#include "process.hpp"
#include "test_framework.hpp"

using namespace agent_runtime;
using namespace agent_runtime::reference;

namespace {

[[nodiscard]] std::string scratch_root(const std::string& name) {
  const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     ("agent_runtime_distributed_" + name);
  std::error_code error;
  std::filesystem::create_directories(path, error);
  return path.string();
}

struct WorkerSet {
  ChildProcess tool_worker;
  ChildProcess crash_tool_worker;
  ChildProcess model_worker;
  std::string tool_endpoint;
  std::string crash_endpoint;
  std::string model_endpoint;
  std::string tool_scratch;
  std::string crash_scratch;
  std::uint64_t tool_generation = 1;
};

[[nodiscard]] bool start_worker(ChildProcess& process, const std::string& executable,
                                const std::vector<std::string>& arguments, std::uint16_t& port,
                                std::string& error) {
  if (!process.spawn(executable, arguments, error)) {
    return false;
  }
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

[[nodiscard]] bool start_workers(WorkerSet& workers, const std::string& name,
                                 std::uint64_t tool_generation, bool with_crash_worker,
                                 std::string& error) {
  workers.tool_scratch = scratch_root(name + "_tool");
  workers.crash_scratch = scratch_root(name + "_crash");
  workers.tool_generation = tool_generation;

  std::uint16_t tool_port = 0;
  if (!start_worker(workers.tool_worker, sibling_executable("agent_runtime_tool_worker"),
                    {"--generation", std::to_string(tool_generation), "--scratch",
                     workers.tool_scratch},
                    tool_port, error)) {
    return false;
  }
  workers.tool_endpoint = "127.0.0.1:" + std::to_string(tool_port);

  if (with_crash_worker) {
    std::uint16_t crash_port = 0;
    if (!start_worker(workers.crash_tool_worker, sibling_executable("agent_runtime_tool_worker"),
                      {"--generation", std::to_string(tool_generation), "--scratch",
                       workers.crash_scratch, "--crash-after-side-effect"},
                      crash_port, error)) {
      return false;
    }
    workers.crash_endpoint = "127.0.0.1:" + std::to_string(crash_port);
  }

  std::uint16_t model_port = 0;
  if (!start_worker(workers.model_worker, sibling_executable("agent_runtime_model_worker"),
                    {"--generation", "1"}, model_port, error)) {
    return false;
  }
  workers.model_endpoint = "127.0.0.1:" + std::to_string(model_port);
  return true;
}

[[nodiscard]] SchedulerAuthority assignment_for(std::uint64_t agent_id,
                                                std::uint64_t coordinator_epoch) {
  SchedulerAuthority authority;
  authority.assignment_id = SchedulerAssignmentId(3001);
  authority.assignment_generation = AssignmentGeneration(1);
  authority.agent_id = AgentId(agent_id);
  authority.agent_generation = AgentGeneration(1);
  authority.agent_boot_id = AgentBootId(1);
  authority.scheduler_epoch = Generation<CoordinatorEpochTag>(1);
  authority.coordinator_epoch = CoordinatorEpoch(coordinator_epoch);
  authority.dispatch_generation = DispatchGeneration(1);
  authority.work_id = WorkId(5001);
  authority.work_generation = WorkGeneration(1);
  authority.lease_id = 4242;
  authority.lease_generation = 1;
  authority.current = true;
  return authority;
}

[[nodiscard]] bool register_remote_bindings(ReferenceCoordinator& coordinator,
                                            const WorkerSet& workers, std::string& error) {
  BackendIncarnation tool;
  tool.backend_id = BackendId(0x7E570123ull);
  tool.generation = Generation<BackendTag>(workers.tool_generation);
  tool.name = "reference-tool-worker";
  tool.available = true;
  BackendIncarnation model;
  model.backend_id = BackendId(0xA1B2C3D4ull);
  model.generation = Generation<BackendTag>(1);
  model.name = "reference-model-worker";
  model.available = true;
  if (!coordinator.runtime().register_remote_backend(tool).accepted() ||
      !coordinator.runtime().register_remote_backend(model).accepted()) {
    error = "remote backend registration failed";
    return false;
  }
  const std::pair<const char*, SideEffectClass> tools[] = {
      {"hash", SideEffectClass::PURE},
      {"echo", SideEffectClass::READ_ONLY},
      {"commit-token", SideEffectClass::COMMIT_TOKEN_REQUIRED},
      {"non-repeatable", SideEffectClass::NON_REPEATABLE},
  };
  for (const auto& entry : tools) {
    ToolBinding binding;
    binding.tool_name = entry.first;
    binding.binding_generation = ToolCallGeneration(1);
    binding.backend_id = tool.backend_id;
    binding.backend_generation = tool.generation;
    binding.side_effect = entry.second;
    binding.max_input_bytes = 65536;
    binding.provenance = "test";
    binding.current = true;
    if (!coordinator.runtime().bind_tool(binding).accepted()) {
      error = std::string("bind tool ") + entry.first + " failed";
      return false;
    }
  }
  ModelBinding model_binding;
  model_binding.target = "reference-target";
  model_binding.binding_generation = ModelCallGeneration(1);
  model_binding.backend_id = model.backend_id;
  model_binding.backend_generation = model.generation;
  model_binding.route_provenance = "test";
  model_binding.current = true;
  if (!coordinator.runtime().bind_model(model_binding).accepted()) {
    error = "bind model failed";
    return false;
  }
  return true;
}

[[nodiscard]] ActionSpec tool_spec(const std::string& tool, SideEffectClass side_effect,
                                   std::string input, std::string key = {}) {
  ActionSpec spec;
  spec.kind = ActionKind::TOOL_CALL;
  spec.tool_name = tool;
  spec.side_effect = side_effect;
  spec.input = std::move(input);
  spec.operation_key = std::move(key);
  spec.max_output_bytes = 4096;
  return spec;
}

[[nodiscard]] bool start_agent_worker(ChildProcess& process, const WorkerSet& workers,
                                      std::uint16_t coordinator_port, std::uint64_t runtime_boot,
                                      std::uint64_t agent_boot, std::uint64_t runtime_epoch,
                                      std::uint64_t coordinator_epoch, std::string& error) {
  std::vector<std::string> arguments = {
      "--coordinator", "127.0.0.1:" + std::to_string(coordinator_port),
      "--tool",       workers.tool_endpoint,
      "--model",      workers.model_endpoint,
      "--runtime-boot", std::to_string(runtime_boot),
      "--agent-boot",   std::to_string(agent_boot),
      "--runtime-epoch", std::to_string(runtime_epoch),
      "--coordinator-epoch", std::to_string(coordinator_epoch),
      "--runtime-id", std::to_string(9001),
      "--agent-id",   std::to_string(7001)};
  if (!workers.crash_endpoint.empty()) {
    arguments.push_back("--crash-tool");
    arguments.push_back(workers.crash_endpoint);
  }
  // The agent worker blocks until the coordinator completes the handshake, so
  // this function only spawns it; the caller accepts and then awaits the
  // REGISTER_RUNTIME message.
  return process.spawn(sibling_executable("agent_runtime_agent_worker"), arguments, error);
}

}  // namespace

AR_TEST(distributed, agent_process_death_and_reincarnation) {
  WorkerSet workers;
  std::string error;
  AR_REQUIRE_MSG(start_workers(workers, "death", 1, false, error), error);

  CoordinatorOptions options;
  options.runtime.runtime_id = AgentRuntimeId(9001);
  options.runtime.agent_id = AgentId(7001);
  options.runtime.runtime_boot_id = RuntimeBootId(1);
  options.runtime.agent_boot_id = AgentBootId(1);
  options.runtime.runtime_epoch = RuntimeEpoch(1);
  options.runtime.coordinator_epoch = CoordinatorEpoch(1);
  options.runtime.clock = std::make_shared<SystemClock>();
  options.runtime.policy = RuntimePolicy::permissive();
  options.runtime.policy.require_assignment_for_dispatch = true;
  ReferenceCoordinator coordinator(std::move(options));
  AR_REQUIRE_MSG(coordinator.start(error), error);

  ChildProcess agent;
  AR_REQUIRE_MSG(start_agent_worker(agent, workers, coordinator.port(), 1, 1, 1, 1, error), error);
  AR_REQUIRE_MSG(coordinator.accept_agent(error), error);
  distributed::RegisterRuntimeMessage registration;
  AR_REQUIRE_MSG(coordinator.await_registration(registration, error), error);
  AR_CHECK_EQ(registration.runtime_boot_id.value(), 1ull);
  AR_CHECK_EQ(registration.agent_boot_id.value(), 1ull);

  AgentRuntime& runtime = coordinator.runtime();
  AR_REQUIRE(runtime.initialize().accepted());
  AR_REQUIRE(runtime.bind_assignment(assignment_for(7001, 1)).accepted());
  AR_REQUIRE_MSG(register_remote_bindings(coordinator, workers, error), error);
  AR_REQUIRE(runtime.start_run(AgentRunId(101), AgentRunGeneration(1)).accepted());
  AR_REQUIRE(runtime.begin_step().accepted());

  // Action one: an effect-free tool call that completes and commits normally.
  ActionHandle first;
  DispatchResult first_dispatch;
  AR_REQUIRE_MSG(coordinator.dispatch_tool_action(tool_spec("hash", SideEffectClass::PURE, "one"),
                                                  first, first_dispatch, error),
                 error);
  ToolResponse first_result;
  AR_REQUIRE_MSG(coordinator.await_tool_result(first_dispatch, first_result, error), error);
  AR_CHECK_EQ(std::string(to_string(first_result.status)), std::string("SUCCEEDED"));
  const MutationResult first_commit =
      coordinator.complete_and_commit(first.action_id, first.action_generation, first_result);
  AR_REQUIRE_MSG(first_commit.accepted(), first_commit.to_text());
  AR_CHECK_EQ(runtime.summary().committed_actions, 1u);

  // Action two: a non-repeatable side effect is dispatched and then the agent
  // worker process is killed for real.
  ActionHandle second;
  DispatchResult second_dispatch;
  AR_REQUIRE_MSG(coordinator.dispatch_tool_action(
                     tool_spec("commit-token", SideEffectClass::COMMIT_TOKEN_REQUIRED, "charge",
                               "op-death"),
                     second, second_dispatch, error),
                 error);
  AR_REQUIRE_MSG(agent.kill(error), error);
  AR_CHECK(!agent.running());

  // The loss is observed through the real session, not through a flag.
  distributed::Frame lost;
  std::string loss_error;
  AR_CHECK_MSG(!coordinator.receive_from_agent(lost, loss_error),
               "the coordinator must observe the agent worker loss");
  coordinator.drop_agent();
  AR_CHECK(!coordinator.agent_open());

  // Fence the dead incarnation and recover under fresh authority.
  RecoveryContext recovery;
  recovery.runtime_boot_id = RuntimeBootId(2);
  recovery.agent_boot_id = AgentBootId(2);
  recovery.runtime_epoch = RuntimeEpoch(2);
  recovery.coordinator_epoch = CoordinatorEpoch(2);
  const MutationResult recovered = runtime.recover(recovery);
  AR_REQUIRE_MSG(recovered.accepted(), recovered.to_text());
  AR_CHECK_EQ(std::string(to_string(runtime.lifecycle())), std::string("REVALIDATION_REQUIRED"));

  // A stale completion from the dead incarnation must not advance progress.
  ToolResponse stale;
  stale.action_id = second_dispatch.tool_request.action_id;
  stale.call_id = second_dispatch.tool_request.call_id;
  stale.call_generation = second_dispatch.tool_request.call_generation;
  stale.attempt_id = second_dispatch.tool_request.attempt_id;
  stale.attempt_generation = second_dispatch.tool_request.attempt_generation;
  stale.status = CompletionStatus::SUCCEEDED;
  stale.output = "stale-result";
  stale.backend_generation = Generation<BackendTag>(workers.tool_generation);
  const MutationResult stale_result = runtime.submit_tool_completion(stale);
  AR_CHECK_EQ(std::string(to_string(stale_result.code)),
              std::string("REJECT_STALE_RUNTIME_BOOT"));
  AR_CHECK_EQ(runtime.summary().committed_actions, 1u);

  // Old action authority is no longer dispatchable.
  const DispatchResult stale_dispatch = runtime.dispatch_action(
      second.action_id, second.action_generation, DispatchMode::EXTERNAL);
  AR_CHECK(!stale_dispatch.result.accepted());

  // A fresh incarnation registers and resumes.
  ChildProcess agent_prime;
  AR_REQUIRE_MSG(start_agent_worker(agent_prime, workers, coordinator.port(), 2, 2, 2, 2, error),
                 error);
  AR_REQUIRE_MSG(coordinator.accept_agent(error), error);
  distributed::RegisterRuntimeMessage registration_prime;
  AR_REQUIRE_MSG(coordinator.await_registration(registration_prime, error), error);
  AR_CHECK_EQ(registration_prime.runtime_boot_id.value(), 2ull);
  AR_CHECK_EQ(registration_prime.agent_boot_id.value(), 2ull);

  AR_REQUIRE_MSG(register_remote_bindings(coordinator, workers, error), error);
  ResumeContext resume;
  resume.runtime_boot_id = RuntimeBootId(2);
  resume.agent_boot_id = AgentBootId(2);
  resume.runtime_epoch = RuntimeEpoch(2);
  resume.coordinator_epoch = CoordinatorEpoch(2);
  resume.assignment = assignment_for(7001, 2);
  const MutationResult resumed = runtime.resume(resume);
  AR_REQUIRE_MSG(resumed.accepted(), resumed.to_text());

  // The unresolved non-repeatable action stays ambiguous and is never retried.
  AR_CHECK_EQ(runtime.summary().ambiguous_actions, 1u);
  const MutationResult retry = runtime.authorize_action(second.action_id, second.action_generation);
  AR_CHECK_EQ(std::string(to_string(retry.code)),
              std::string("MANUAL_OR_EXTERNAL_RECONCILIATION_REQUIRED"));

  // A brand new action runs on the fresh incarnation and commits exactly once.
  ActionHandle third;
  DispatchResult third_dispatch;
  AR_REQUIRE_MSG(coordinator.dispatch_tool_action(
                     tool_spec("hash", SideEffectClass::PURE, "three"), third, third_dispatch,
                     error),
                 error);
  ToolResponse third_result;
  AR_REQUIRE_MSG(coordinator.await_tool_result(third_dispatch, third_result, error), error);
  const MutationResult third_commit =
      coordinator.complete_and_commit(third.action_id, third.action_generation, third_result);
  AR_REQUIRE_MSG(third_commit.accepted(), third_commit.to_text());
  AR_CHECK_EQ(runtime.summary().committed_actions, 2u);
  AR_CHECK_EQ(runtime.summary().progress_generation.value(), 2ull);

  // The old incarnation stays fenced forever.
  const MutationResult refence = runtime.recover(recovery);
  AR_CHECK(!refence.accepted());
  const MutationResult stale_again = runtime.submit_tool_completion(stale);
  AR_CHECK(!stale_again.accepted());
  AR_CHECK(runtime.check_invariants().ok);

  std::string ignored;
  (void)agent_prime.kill(ignored);
  std::error_code remove_error;
  std::filesystem::remove_all(workers.tool_scratch, remove_error);
  std::filesystem::remove_all(workers.crash_scratch, remove_error);
}

AR_TEST(distributed, ambiguous_side_effect_survives_tool_worker_death) {
  WorkerSet workers;
  std::string error;
  AR_REQUIRE_MSG(start_workers(workers, "ambiguous", 1, true, error), error);

  CoordinatorOptions options;
  options.runtime.runtime_id = AgentRuntimeId(9002);
  options.runtime.agent_id = AgentId(7002);
  options.runtime.clock = std::make_shared<SystemClock>();
  options.runtime.policy = RuntimePolicy::permissive();
  options.runtime.policy.require_assignment_for_dispatch = true;
  ReferenceCoordinator coordinator(std::move(options));
  AR_REQUIRE_MSG(coordinator.start(error), error);

  ChildProcess agent;
  AR_REQUIRE_MSG(start_agent_worker(agent, workers, coordinator.port(), 1, 1, 1, 1, error), error);
  AR_REQUIRE_MSG(coordinator.accept_agent(error), error);
  distributed::RegisterRuntimeMessage registration;
  AR_REQUIRE_MSG(coordinator.await_registration(registration, error), error);

  AgentRuntime& runtime = coordinator.runtime();
  AR_REQUIRE(runtime.initialize().accepted());
  AR_REQUIRE(runtime.bind_assignment(assignment_for(7002, 1)).accepted());
  AR_REQUIRE_MSG(register_remote_bindings(coordinator, workers, error), error);
  AR_REQUIRE(runtime.start_run(AgentRunId(102), AgentRunGeneration(1)).accepted());
  AR_REQUIRE(runtime.begin_step().accepted());

  ActionHandle handle;
  DispatchResult dispatched;
  AR_REQUIRE_MSG(coordinator.dispatch_tool_action(
                     tool_spec("commit-token", SideEffectClass::COMMIT_TOKEN_REQUIRED, "charge",
                               "op-ambiguous"),
                     handle, dispatched, error),
                 error);
  ToolResponse response;
  AR_REQUIRE_MSG(coordinator.await_tool_result(dispatched, response, error), error);
  AR_CHECK_EQ(std::string(to_string(response.status)), std::string("AMBIGUOUS"));

  // The physical side effect is durable in the crashed worker's journal.
  const std::filesystem::path receipt =
      std::filesystem::path(workers.crash_scratch) / "op-ambiguous.receipt";
  AR_CHECK_MSG(std::filesystem::exists(receipt),
               "the side effect must have been applied before the worker died");

  const MutationResult completion = runtime.submit_tool_completion(response);
  AR_CHECK_EQ(std::string(to_string(completion.code)), std::string("AMBIGUOUS_COMPLETION"));
  const RuntimeSummary summary = runtime.summary();
  AR_CHECK_EQ(summary.committed_actions, 0u);
  AR_CHECK_EQ(summary.ambiguous_actions, 1u);

  // The runtime refuses to retry the non-repeatable action automatically.
  const MutationResult retry = runtime.authorize_action(handle.action_id, handle.action_generation);
  AR_CHECK_EQ(std::string(to_string(retry.code)),
              std::string("MANUAL_OR_EXTERNAL_RECONCILIATION_REQUIRED"));
  const MutationResult commit = runtime.commit_action(handle.action_id, handle.action_generation);
  AR_CHECK(!commit.accepted());
  AR_CHECK(runtime.check_invariants().ok);

  std::string ignored;
  (void)agent.kill(ignored);
  std::error_code remove_error;
  std::filesystem::remove_all(workers.tool_scratch, remove_error);
  std::filesystem::remove_all(workers.crash_scratch, remove_error);
}

AR_TEST(distributed, coordinator_restart_preserves_progress_and_fences_old_epoch) {
  WorkerSet workers;
  std::string error;
  AR_REQUIRE_MSG(start_workers(workers, "restart", 1, false, error), error);
  const std::string state_path = scratch_root("restart") + "/coordinator.state";

  // Phase one runs as a real coordinator process.
  ChildProcess coordinator_one;
  AR_REQUIRE_MSG(coordinator_one.spawn(sibling_executable("agent_runtime_coordinator"),
                                       {"--phase", "1", "--state", state_path, "--tool",
                                        workers.tool_endpoint, "--model", workers.model_endpoint,
                                        "--tool-generation", "1", "--runtime-boot", "1",
                                        "--agent-boot", "1", "--runtime-epoch", "1",
                                        "--coordinator-epoch", "1"},
                                       error),
                 error);
  std::string line;
  AR_REQUIRE_MSG(coordinator_one.read_line(line, error), error);
  AR_REQUIRE_MSG(line.rfind("PORT ", 0) == 0, line);
  const std::uint16_t coordinator_port =
      static_cast<std::uint16_t>(std::strtoul(line.c_str() + 5, nullptr, 10));

  ChildProcess agent;
  AR_REQUIRE_MSG(start_agent_worker(agent, workers, coordinator_port, 1, 1, 1, 1, error), error);
  std::string phase_one;
  bool saw_phase_one = false;
  while (coordinator_one.read_line(phase_one, error)) {
    if (phase_one.rfind("PHASE1", 0) == 0) {
      saw_phase_one = true;
      break;
    }
  }
  int exit_code = 0;
  AR_REQUIRE_MSG(coordinator_one.wait(exit_code, error), error);
  AR_CHECK_EQ(exit_code, 0);
  AR_CHECK_MSG(saw_phase_one, "phase one must report completion");
  AR_CHECK_MSG(phase_one.find("committed=1") != std::string::npos, phase_one);
  AR_CHECK(std::filesystem::exists(state_path));

  // The agent worker is killed together with the coordinator: a restart must
  // not revive the previous process authority.
  AR_REQUIRE_MSG(agent.kill(error), error);

  ChildProcess coordinator_two;
  AR_REQUIRE_MSG(coordinator_two.spawn(sibling_executable("agent_runtime_coordinator"),
                                       {"--phase", "2", "--state", state_path, "--tool",
                                        workers.tool_endpoint, "--model", workers.model_endpoint,
                                        "--tool-generation", "1", "--runtime-boot", "2",
                                        "--agent-boot", "2", "--runtime-epoch", "2",
                                        "--coordinator-epoch", "2"},
                                       error),
                 error);
  AR_REQUIRE_MSG(coordinator_two.read_line(line, error), error);
  AR_REQUIRE_MSG(line.rfind("PORT ", 0) == 0, line);
  const std::uint16_t coordinator_two_port =
      static_cast<std::uint16_t>(std::strtoul(line.c_str() + 5, nullptr, 10));

  ChildProcess agent_prime;
  AR_REQUIRE_MSG(
      start_agent_worker(agent_prime, workers, coordinator_two_port, 2, 2, 2, 2, error), error);
  std::string phase_two;
  bool saw_phase_two = false;
  while (coordinator_two.read_line(phase_two, error)) {
    if (phase_two.rfind("PHASE2", 0) == 0) {
      saw_phase_two = true;
      break;
    }
  }
  AR_REQUIRE_MSG(coordinator_two.wait(exit_code, error), error);
  AR_CHECK_EQ(exit_code, 0);
  AR_CHECK_MSG(saw_phase_two, "phase two must report completion");
  AR_CHECK_MSG(phase_two.find("committed=2") != std::string::npos, phase_two);

  // The durable state proves the old incarnation was fenced and that progress
  // advanced exactly once per committed action.
  const PersistenceProbe probe = persistence_inspect(state_path);
  AR_REQUIRE_MSG(probe.valid(), probe.explanation.to_text());
  AR_CHECK(probe.document_json.find("\"fenced_boots\"") != std::string::npos);

  std::string ignored;
  (void)agent_prime.kill(ignored);
  std::error_code remove_error;
  std::filesystem::remove_all(workers.tool_scratch, remove_error);
  std::filesystem::remove_all(workers.crash_scratch, remove_error);
  std::filesystem::remove(state_path, remove_error);
}
