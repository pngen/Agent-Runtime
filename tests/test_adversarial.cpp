// Agent Runtime - adversarial hardening tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "runtime_fixture.hpp"
#include "test_framework.hpp"

using namespace agent_runtime;

namespace {

[[nodiscard]] std::string scratch() {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "agent_runtime_tests_adversarial";
  std::error_code error;
  std::filesystem::create_directories(path, error);
  return path.string();
}

[[nodiscard]] ToolResponse response_for(const ToolRequest& request, std::string output) {
  ToolResponse response;
  response.action_id = request.action_id;
  response.call_id = request.call_id;
  response.call_generation = request.call_generation;
  response.attempt_id = request.attempt_id;
  response.attempt_generation = request.attempt_generation;
  response.backend_generation = Generation<BackendTag>(1);
  response.status = CompletionStatus::SUCCEEDED;
  response.output = std::move(output);
  response.result_digest = 9001;
  return response;
}

}  // namespace

AR_TEST(adversarial, conflicting_duplicate_completion_is_rejected) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  ActionHandle handle;
  AR_REQUIRE(fixture.runtime
                 ->declare_action(artest::tool_spec("echo", SideEffectClass::READ_ONLY, "x"), handle)
                 .accepted());
  AR_REQUIRE(fixture.runtime->admit_action(handle.action_id, handle.action_generation).accepted());
  AR_REQUIRE(fixture.runtime->authorize_action(handle.action_id, handle.action_generation).accepted());
  const DispatchResult dispatched = fixture.runtime->dispatch_action(
      handle.action_id, handle.action_generation, DispatchMode::EXTERNAL);
  AR_REQUIRE(dispatched.result.accepted());
  AR_REQUIRE(fixture.runtime->submit_tool_completion(
                 response_for(dispatched.tool_request, "first"))
                 .accepted());
  ToolResponse conflicting = response_for(dispatched.tool_request, "second");
  conflicting.result_digest = 12345;
  const MutationResult result = fixture.runtime->submit_tool_completion(conflicting);
  AR_CHECK_EQ(std::string(to_string(result.code)), std::string("REJECT_CONFLICT"));
  AR_CHECK_EQ(fixture.runtime->summary().committed_actions, 0u);
}

AR_TEST(adversarial, identical_duplicate_completion_is_idempotent) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  ActionHandle handle;
  AR_REQUIRE(fixture.runtime
                 ->declare_action(artest::tool_spec("echo", SideEffectClass::READ_ONLY, "x"), handle)
                 .accepted());
  AR_REQUIRE(fixture.runtime->admit_action(handle.action_id, handle.action_generation).accepted());
  AR_REQUIRE(fixture.runtime->authorize_action(handle.action_id, handle.action_generation).accepted());
  const DispatchResult dispatched = fixture.runtime->dispatch_action(
      handle.action_id, handle.action_generation, DispatchMode::EXTERNAL);
  AR_REQUIRE(dispatched.result.accepted());
  const ToolResponse response = response_for(dispatched.tool_request, "same");
  AR_REQUIRE(fixture.runtime->submit_tool_completion(response).accepted());
  const MutationResult repeated = fixture.runtime->submit_tool_completion(response);
  AR_CHECK_EQ(std::string(to_string(repeated.code)), std::string("NO_CHANGE"));
}

AR_TEST(adversarial, cross_action_completion_injection_is_rejected) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  ActionHandle first;
  ActionHandle second;
  AR_REQUIRE(fixture.runtime
                 ->declare_action(artest::tool_spec("echo", SideEffectClass::READ_ONLY, "a"), first)
                 .accepted());
  AR_REQUIRE(fixture.runtime
                 ->declare_action(artest::tool_spec("echo", SideEffectClass::READ_ONLY, "b"), second)
                 .accepted());
  AR_REQUIRE(fixture.runtime->admit_action(first.action_id, first.action_generation).accepted());
  AR_REQUIRE(fixture.runtime->authorize_action(first.action_id, first.action_generation).accepted());
  const DispatchResult dispatched = fixture.runtime->dispatch_action(
      first.action_id, first.action_generation, DispatchMode::EXTERNAL);
  AR_REQUIRE(dispatched.result.accepted());
  ToolResponse injected = response_for(dispatched.tool_request, "injected");
  injected.action_id = second.action_id;
  const MutationResult result = fixture.runtime->submit_tool_completion(injected);
  AR_CHECK_EQ(std::string(to_string(result.code)), std::string("REJECT_CONFLICT"));
}

AR_TEST(adversarial, oversized_completion_payload_is_rejected) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  ActionSpec spec = artest::tool_spec("echo", SideEffectClass::READ_ONLY, "x");
  spec.max_output_bytes = 64;
  ActionHandle handle;
  AR_REQUIRE(fixture.runtime->declare_action(spec, handle).accepted());
  AR_REQUIRE(fixture.runtime->admit_action(handle.action_id, handle.action_generation).accepted());
  AR_REQUIRE(fixture.runtime->authorize_action(handle.action_id, handle.action_generation).accepted());
  const DispatchResult dispatched = fixture.runtime->dispatch_action(
      handle.action_id, handle.action_generation, DispatchMode::EXTERNAL);
  AR_REQUIRE(dispatched.result.accepted());
  const MutationResult result = fixture.runtime->submit_tool_completion(
      response_for(dispatched.tool_request, std::string(4096, 'A')));
  AR_CHECK_EQ(std::string(to_string(result.code)), std::string("REJECT_LIMIT"));
}

AR_TEST(adversarial, absurd_declared_payload_length_is_rejected) {
  const std::string path = scratch() + "/absurd.bin";
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  AR_REQUIRE(fixture.runtime->save(path).accepted());
  std::ifstream stream(path, std::ios::binary);
  std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(stream)),
                                  std::istreambuf_iterator<char>());
  stream.close();
  for (int i = 0; i < 8; ++i) {
    bytes[8 + static_cast<std::size_t>(i)] = 0xFF;
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  out.close();
  const PersistenceProbe probe = persistence_inspect(path);
  AR_CHECK(!probe.valid());
  std::filesystem::remove(path);
}

AR_TEST(adversarial, long_and_awkward_paths_are_handled) {
  const std::string directory = scratch();
  const std::string nested = directory + "/a directory with spaces/and more";
  std::error_code error;
  std::filesystem::create_directories(nested, error);
  const std::string path = nested + "/state file with spaces.bin";
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  AR_REQUIRE_MSG(fixture.runtime->save(path).accepted(), "save to a path with spaces must work");
  AR_CHECK(persistence_inspect(path).valid());
  artest::Fixture restored = artest::make_fixture();
  AR_REQUIRE(restored.runtime->load(path).accepted());
  std::filesystem::remove_all(directory, error);
}

AR_TEST(adversarial, malformed_metadata_is_rejected) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  ActionSpec spec = artest::tool_spec("hash", SideEffectClass::PURE, "x");
  spec.label.assign(1024 * 1024, 'L');
  ActionHandle handle;
  AR_CHECK_EQ(std::string(to_string(fixture.runtime->declare_action(spec, handle).code)),
              std::string("REJECT_LIMIT"));

  spec = artest::tool_spec("hash", SideEffectClass::PURE, "x");
  spec.kind = ActionKind::kCount;
  AR_CHECK_EQ(std::string(to_string(fixture.runtime->declare_action(spec, handle).code)),
              std::string("REJECT_INVALID"));

  spec = artest::tool_spec("hash", SideEffectClass::PURE, "x");
  spec.side_effect = SideEffectClass::kCount;
  AR_CHECK_EQ(std::string(to_string(fixture.runtime->declare_action(spec, handle).code)),
              std::string("REJECT_INVALID"));

  spec = artest::tool_spec("hash", SideEffectClass::PURE, std::string(2 * 1024 * 1024, 'x'));
  AR_CHECK_EQ(std::string(to_string(fixture.runtime->declare_action(spec, handle).code)),
              std::string("REJECT_LIMIT"));
}

AR_TEST(adversarial, unknown_tool_and_model_are_rejected) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  ActionHandle handle;
  AR_CHECK_EQ(std::string(to_string(fixture.runtime
                                        ->declare_action(artest::tool_spec("no-such-tool",
                                                                           SideEffectClass::PURE,
                                                                           "x"),
                                                         handle)
                                        .code)),
              std::string("REJECT_STALE_TOOL_BINDING"));
  ActionSpec model = artest::model_spec("prompt");
  model.model_target = "no-such-target";
  AR_CHECK_EQ(std::string(to_string(fixture.runtime->declare_action(model, handle).code)),
              std::string("REJECT_STALE_MODEL_BINDING"));
}

AR_TEST(adversarial, side_effect_class_mismatch_is_rejected) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  ActionHandle handle;
  const MutationResult declared = fixture.runtime->declare_action(
      artest::tool_spec("hash", SideEffectClass::NON_REPEATABLE, "x", "key"), handle);
  AR_CHECK_EQ(std::string(to_string(declared.code)), std::string("REJECT_CONFLICT"));
}

AR_TEST(adversarial, operation_key_is_required_for_deduplicatable_classes) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  ActionHandle handle;
  const MutationResult declared = fixture.runtime->declare_action(
      artest::tool_spec("commit-token", SideEffectClass::COMMIT_TOKEN_REQUIRED, "x"), handle);
  AR_CHECK_EQ(std::string(to_string(declared.code)), std::string("REJECT_SIDE_EFFECT_UNSAFE"));
}

AR_TEST(adversarial, action_limit_is_enforced) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  bool saw_limit = false;
  for (int i = 0; i < 20000; ++i) {
    ActionHandle handle;
    const MutationResult declared = fixture.runtime->declare_action(
        artest::tool_spec("hash", SideEffectClass::PURE, "x"), handle);
    if (!declared.accepted()) {
      saw_limit = std::string(to_string(declared.code)) == std::string("REJECT_LIMIT");
      break;
    }
  }
  AR_CHECK(saw_limit);
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(adversarial, repeated_suspend_resume_cycles) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  std::uint64_t boot = fixture.runtime->boot_id().value();
  std::uint64_t epoch = fixture.runtime->runtime_epoch().value();
  for (int i = 0; i < 12; ++i) {
    AR_REQUIRE_MSG(fixture.runtime->suspend("cycle").accepted(), "suspend must succeed");
    ResumeContext context;
    context.runtime_boot_id = RuntimeBootId(++boot);
    context.agent_boot_id = AgentBootId(boot);
    context.runtime_epoch = RuntimeEpoch(++epoch);
    context.coordinator_epoch = CoordinatorEpoch(epoch);
    context.assignment = artest::make_assignment(fixture, 1, 1);
    context.assignment->coordinator_epoch = CoordinatorEpoch(epoch);
    AR_REQUIRE_MSG(fixture.runtime->resume(context).accepted(), "resume must succeed");
    AR_CHECK(fixture.runtime->check_invariants().ok);
  }
  AR_CHECK_EQ(fixture.runtime->summary().committed_actions, 0u);
}

AR_TEST(adversarial, recovery_rejects_stale_and_duplicate_authority) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  RecoveryContext context;
  context.runtime_boot_id = RuntimeBootId(2);
  context.agent_boot_id = AgentBootId(2);
  context.runtime_epoch = RuntimeEpoch(2);
  context.coordinator_epoch = CoordinatorEpoch(2);
  AR_REQUIRE(fixture.runtime->recover(context).accepted());
  const MutationResult again = fixture.runtime->recover(context);
  AR_CHECK(!again.accepted());
  RecoveryContext older;
  older.runtime_boot_id = RuntimeBootId(2);
  older.agent_boot_id = AgentBootId(2);
  older.runtime_epoch = RuntimeEpoch(2);
  older.coordinator_epoch = CoordinatorEpoch(2);
  AR_CHECK(!fixture.runtime->recover(older).accepted());
}

AR_TEST(adversarial, memory_write_requires_write_authority) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  AR_REQUIRE(fixture.runtime
                 ->bind_memory(artest::make_memory_binding("ctx", 1, 1, false))
                 .accepted());
  ActionSpec spec = artest::tool_spec("hash", SideEffectClass::PURE, "x");
  spec.memory_binding_identity = "ctx";
  ActionHandle handle;
  AR_REQUIRE(fixture.runtime->declare_action(spec, handle).accepted());
  AR_REQUIRE(fixture.runtime->admit_action(handle.action_id, handle.action_generation).accepted());
  AR_REQUIRE(fixture.runtime->authorize_action(handle.action_id, handle.action_generation).accepted());

  ActionSpec write_spec = artest::tool_spec("hash", SideEffectClass::PURE, "x");
  write_spec.kind = ActionKind::MEMORY_WRITE_INTENT;
  write_spec.memory_binding_identity = "ctx";
  ActionHandle write_handle;
  AR_REQUIRE(fixture.runtime->declare_action(write_spec, write_handle).accepted());
  AR_REQUIRE(fixture.runtime->admit_action(write_handle.action_id, write_handle.action_generation)
                 .accepted());
  const MutationResult authorized = fixture.runtime->authorize_action(write_handle.action_id,
                                                                      write_handle.action_generation);
  AR_CHECK_EQ(std::string(to_string(authorized.code)), std::string("REVALIDATION_REQUIRED"));
}

AR_TEST(adversarial, checkpoint_from_wrong_runtime_generation_is_rejected) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  AR_REQUIRE(fixture.runtime
                 ->request_checkpoint(CheckpointBindingId(88), CheckpointGeneration(1), "cp")
                 .accepted());
  const RuntimeSummary summary = fixture.runtime->summary();
  CheckpointBinding binding;
  binding.binding_id = CheckpointBindingId(88);
  binding.generation = CheckpointGeneration(1);
  binding.runtime_id = AgentRuntimeId(999999);
  binding.runtime_generation = summary.runtime_generation;
  binding.run_id = summary.run_id;
  binding.run_generation = summary.run_generation;
  binding.runtime_epoch = summary.runtime_epoch;
  binding.restorable = true;
  AR_CHECK_EQ(std::string(to_string(fixture.runtime->accept_checkpoint(binding).code)),
              std::string("REJECT_STALE_CHECKPOINT"));
}

AR_TEST(adversarial, tool_reincarnation_during_flight_invalidates_completion) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  ActionHandle handle;
  AR_REQUIRE(fixture.runtime
                 ->declare_action(artest::tool_spec("echo", SideEffectClass::READ_ONLY, "x"), handle)
                 .accepted());
  AR_REQUIRE(fixture.runtime->admit_action(handle.action_id, handle.action_generation).accepted());
  AR_REQUIRE(fixture.runtime->authorize_action(handle.action_id, handle.action_generation).accepted());
  const DispatchResult dispatched = fixture.runtime->dispatch_action(
      handle.action_id, handle.action_generation, DispatchMode::EXTERNAL);
  AR_REQUIRE(dispatched.result.accepted());
  BackendIncarnation reincarnated = fixture.tool->incarnation();
  reincarnated.generation = Generation<BackendTag>(2);
  AR_REQUIRE(fixture.runtime->register_tool_backend(reincarnated, fixture.tool).accepted());
  AR_REQUIRE(fixture.runtime
                 ->bind_tool(artest::make_tool_binding(fixture, "echo", SideEffectClass::READ_ONLY, 2,
                                                       2))
                 .accepted());
  const MutationResult completion = fixture.runtime->submit_tool_completion(
      response_for(dispatched.tool_request, "stale"));
  AR_CHECK_EQ(std::string(to_string(completion.code)), std::string("REJECT_STALE_TOOL_BINDING"));
  AR_CHECK_EQ(fixture.runtime->summary().committed_actions, 0u);
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(adversarial, randomized_completion_corruption_never_commits) {
  std::mt19937_64 rng(0xC0FFEE);
  for (int iteration = 0; iteration < 64; ++iteration) {
    artest::Fixture fixture = artest::make_fixture();
    artest::bring_up(fixture);
    ActionHandle handle;
    AR_REQUIRE(fixture.runtime
                   ->declare_action(artest::tool_spec("echo", SideEffectClass::READ_ONLY, "x"),
                                    handle)
                   .accepted());
    AR_REQUIRE(fixture.runtime->admit_action(handle.action_id, handle.action_generation).accepted());
    AR_REQUIRE(fixture.runtime->authorize_action(handle.action_id, handle.action_generation).accepted());
    const DispatchResult dispatched = fixture.runtime->dispatch_action(
        handle.action_id, handle.action_generation, DispatchMode::EXTERNAL);
    AR_REQUIRE(dispatched.result.accepted());
    ToolResponse response = response_for(dispatched.tool_request, "output");
    switch (rng() % 6) {
      case 0:
        response.attempt_generation = AttemptGeneration(999);
        break;
      case 1:
        response.call_id = ToolCallId(999);
        break;
      case 2:
        response.call_generation = ToolCallGeneration(999);
        break;
      case 3:
        response.attempt_id = ActionAttemptId(999);
        break;
      case 4:
        response.backend_generation = Generation<BackendTag>(77);
        break;
      default:
        response.status = CompletionStatus::FAILED;
        response.failure_class = RetryClass::PERMANENT;
        break;
    }
    (void)fixture.runtime->submit_tool_completion(response);
    AR_CHECK_MSG(fixture.runtime->summary().committed_actions == 0,
                 "a corrupted completion must never commit progress");
    AR_CHECK(fixture.runtime->check_invariants().ok);
  }
}
