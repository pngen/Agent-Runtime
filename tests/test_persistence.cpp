// Agent Runtime - durable persistence format, integrity and recovery tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "runtime_fixture.hpp"
#include "test_framework.hpp"

using namespace agent_runtime;

namespace {

[[nodiscard]] std::string scratch_directory() {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "agent_runtime_tests_persistence";
  std::error_code error;
  std::filesystem::create_directories(path, error);
  return path.string();
}

[[nodiscard]] std::vector<std::uint8_t> read_all(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(stream),
                                   std::istreambuf_iterator<char>());
}

void write_all(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

/// Brings a fixture to a state with committed progress, a checkpoint and a
/// memory binding so that recovery has something meaningful to restore.
[[nodiscard]] std::uint64_t populate(artest::Fixture& fixture) {
  artest::bring_up(fixture);
  AR_REQUIRE(fixture.runtime->bind_memory(artest::make_memory_binding("ctx", 1, 1)).accepted());
  ActionHandle handle;
  AR_REQUIRE(fixture.runtime
                 ->declare_action(artest::tool_spec("hash", SideEffectClass::PURE, "payload"),
                                  handle)
                 .accepted());
  AR_REQUIRE(fixture.runtime->admit_action(handle.action_id, handle.action_generation).accepted());
  AR_REQUIRE(fixture.runtime->authorize_action(handle.action_id, handle.action_generation).accepted());
  const LocalExecutionResult execution =
      fixture.runtime->dispatch_and_execute(handle.action_id, handle.action_generation);
  AR_REQUIRE(execution.completion.accepted());
  AR_REQUIRE(fixture.runtime->commit_action(handle.action_id, handle.action_generation).accepted());
  AR_REQUIRE(fixture.runtime
                 ->request_checkpoint(CheckpointBindingId(77), CheckpointGeneration(1), "cp")
                 .accepted());
  const RuntimeSummary summary = fixture.runtime->summary();
  CheckpointBinding binding;
  binding.binding_id = CheckpointBindingId(77);
  binding.generation = CheckpointGeneration(1);
  binding.runtime_id = summary.runtime_id;
  binding.runtime_generation = summary.runtime_generation;
  binding.run_id = summary.run_id;
  binding.run_generation = summary.run_generation;
  binding.step_id = summary.step_id;
  binding.step_generation = summary.step_generation;
  binding.runtime_epoch = summary.runtime_epoch;
  binding.progress_generation = summary.progress_generation;
  binding.checkpoint_identity = "cp";
  binding.integrity_digest = "digest";
  binding.restorable = true;
  AR_REQUIRE(fixture.runtime->accept_checkpoint(binding).accepted());
  return fixture.runtime->durable_digest();
}

}  // namespace

AR_TEST(persistence, round_trip_is_deterministic) {
  const std::string directory = scratch_directory();
  const std::string first_path = directory + "/state-a.bin";
  const std::string second_path = directory + "/state-b.bin";
  artest::Fixture fixture = artest::make_fixture();
  const std::uint64_t digest = populate(fixture);
  AR_REQUIRE_MSG(fixture.runtime->save(first_path).accepted(), "save must succeed");
  AR_REQUIRE_MSG(fixture.runtime->save(second_path).accepted(), "save must succeed");
  const std::vector<std::uint8_t> first = read_all(first_path);
  const std::vector<std::uint8_t> second = read_all(second_path);
  AR_CHECK(!first.empty());
  AR_CHECK_EQ(first.size(), second.size());
  AR_CHECK(first == second);

  const PersistenceProbe probe = persistence_inspect(first_path);
  AR_REQUIRE_MSG(probe.valid(), probe.explanation.to_text());
  AR_CHECK_EQ(probe.info.format_version, persistence_format_version);
  AR_CHECK(probe.info.record_count > 0u);
  AR_CHECK(!probe.document_json.empty());

  artest::Fixture restored = artest::make_fixture();
  AR_REQUIRE_MSG(restored.runtime->load(first_path).accepted(), "load must succeed");
  AR_CHECK_EQ(restored.runtime->summary().committed_actions, 1u);
  AR_CHECK_EQ(restored.runtime->summary().progress_generation.value(),
              fixture.runtime->summary().progress_generation.value());
  AR_CHECK_EQ(std::string(to_string(restored.runtime->lifecycle())), std::string("RECOVERING"));
  AR_CHECK(restored.runtime->check_invariants().ok);

  // A restored runtime is itself deterministically serializable: saving the
  // recovered state and loading it again reproduces exactly the same durable
  // content.
  const std::uint64_t restored_digest = restored.runtime->durable_digest();
  AR_REQUIRE(restored.runtime->save(second_path).accepted());
  artest::Fixture third = artest::make_fixture();
  AR_REQUIRE_MSG(third.runtime->load(second_path).accepted(), "second load must succeed");
  AR_CHECK_EQ(third.runtime->durable_digest(), restored_digest);
  (void)digest;
  std::filesystem::remove(first_path);
  std::filesystem::remove(second_path);
}

AR_TEST(persistence, unsupported_version_is_rejected_before_parsing) {
  const std::string directory = scratch_directory();
  const std::string path = directory + "/version.bin";
  artest::Fixture fixture = artest::make_fixture();
  (void)populate(fixture);
  AR_REQUIRE(fixture.runtime->save(path).accepted());
  std::vector<std::uint8_t> bytes = read_all(path);
  bytes[4] = 0xFE;
  bytes[5] = 0x00;
  write_all(path, bytes);
  const PersistenceProbe probe = persistence_inspect(path);
  AR_CHECK_EQ(std::string(to_string(probe.code)), std::string("REJECT_INVALID"));
  AR_CHECK(probe.explanation.to_text().find("unsupported persistence format version") !=
           std::string::npos);
  std::filesystem::remove(path);
}

AR_TEST(persistence, bad_magic_is_rejected) {
  const std::string directory = scratch_directory();
  const std::string path = directory + "/magic.bin";
  artest::Fixture fixture = artest::make_fixture();
  (void)populate(fixture);
  AR_REQUIRE(fixture.runtime->save(path).accepted());
  std::vector<std::uint8_t> bytes = read_all(path);
  bytes[0] = 'X';
  write_all(path, bytes);
  AR_CHECK(!persistence_inspect(path).valid());
  std::filesystem::remove(path);
}

AR_TEST(persistence, payload_corruption_is_rejected) {
  const std::string directory = scratch_directory();
  const std::string path = directory + "/corrupt.bin";
  artest::Fixture fixture = artest::make_fixture();
  (void)populate(fixture);
  AR_REQUIRE(fixture.runtime->save(path).accepted());
  const std::vector<std::uint8_t> original = read_all(path);
  bool any_rejected = false;
  for (std::size_t offset = persistence_header_bytes; offset + persistence_trailer_bytes <
                                                           original.size();
       offset += 17) {
    std::vector<std::uint8_t> bytes = original;
    bytes[offset] ^= 0x5A;
    write_all(path, bytes);
    if (!persistence_inspect(path).valid()) {
      any_rejected = true;
    } else {
      AR_CHECK_MSG(false, "corruption at offset was accepted");
    }
  }
  AR_CHECK(any_rejected);
  std::filesystem::remove(path);
}

AR_TEST(persistence, truncation_at_every_boundary_is_rejected) {
  const std::string directory = scratch_directory();
  const std::string path = directory + "/truncated.bin";
  artest::Fixture fixture = artest::make_fixture();
  (void)populate(fixture);
  AR_REQUIRE(fixture.runtime->save(path).accepted());
  const std::vector<std::uint8_t> original = read_all(path);
  for (std::size_t size = 0; size < original.size(); size += 7) {
    write_all(path, std::vector<std::uint8_t>(original.begin(),
                                              original.begin() + static_cast<std::ptrdiff_t>(size)));
    AR_CHECK_MSG(!persistence_inspect(path).valid(), "truncated document was accepted");
  }
  write_all(path, std::vector<std::uint8_t>(original.begin(),
                                            original.begin() +
                                                static_cast<std::ptrdiff_t>(original.size() - 1)));
  AR_CHECK(!persistence_inspect(path).valid());
  std::filesystem::remove(path);
}

AR_TEST(persistence, trailing_garbage_is_rejected) {
  const std::string directory = scratch_directory();
  const std::string path = directory + "/trailing.bin";
  artest::Fixture fixture = artest::make_fixture();
  (void)populate(fixture);
  AR_REQUIRE(fixture.runtime->save(path).accepted());
  std::vector<std::uint8_t> bytes = read_all(path);
  bytes.push_back(0x00);
  write_all(path, bytes);
  AR_CHECK(!persistence_inspect(path).valid());
  std::filesystem::remove(path);
}

AR_TEST(persistence, load_never_treats_dynamic_state_as_current) {
  const std::string directory = scratch_directory();
  const std::string path = directory + "/dynamic.bin";
  artest::Fixture fixture = artest::make_fixture();
  (void)populate(fixture);
  ActionHandle pending;
  AR_REQUIRE(fixture.runtime
                 ->declare_action(artest::tool_spec("commit-token",
                                                    SideEffectClass::COMMIT_TOKEN_REQUIRED,
                                                    "payload", "op-pending"),
                                  pending)
                 .accepted());
  AR_REQUIRE(fixture.runtime->admit_action(pending.action_id, pending.action_generation).accepted());
  AR_REQUIRE(fixture.runtime->authorize_action(pending.action_id, pending.action_generation).accepted());
  const DispatchResult dispatched = fixture.runtime->dispatch_action(
      pending.action_id, pending.action_generation, DispatchMode::EXTERNAL);
  AR_REQUIRE(dispatched.result.accepted());
  AR_REQUIRE(fixture.runtime->save(path).accepted());

  artest::Fixture restored = artest::make_fixture();
  const MutationResult loaded = restored.runtime->load(path);
  AR_REQUIRE_MSG(loaded.accepted(), loaded.to_text());
  const RuntimeSnapshot snapshot = restored.runtime->snapshot();
  AR_CHECK_EQ(std::string(to_string(snapshot.validity)), std::string("REVALIDATION_REQUIRED"));
  AR_CHECK(!snapshot.authorizes_execution());
  AR_CHECK(snapshot.attempts.size() >= std::size_t{1});
  bool found_unresolved = false;
  for (const AttemptView& attempt : snapshot.attempts) {
    if (attempt.state == ActionState::REVALIDATION_REQUIRED) {
      found_unresolved = true;
      AR_CHECK(!attempt.current);
      AR_CHECK(attempt.superseded);
    }
  }
  AR_CHECK(found_unresolved);
  for (const ToolBinding& binding : snapshot.tool_bindings) {
    AR_CHECK(!binding.current);
  }
  for (const MemoryBinding& binding : snapshot.memory_bindings) {
    AR_CHECK(!binding.fresh);
  }
  AR_CHECK(restored.runtime->check_invariants().ok);
  std::filesystem::remove(path);
}

AR_TEST(persistence, recovery_requires_fresh_authority_and_rebinding) {
  const std::string directory = scratch_directory();
  const std::string path = directory + "/recover.bin";
  artest::Fixture fixture = artest::make_fixture();
  (void)populate(fixture);
  AR_REQUIRE(fixture.runtime->save(path).accepted());
  const std::uint64_t digest = fixture.runtime->durable_digest();

  artest::Fixture restored = artest::make_fixture();
  const MutationResult loaded = restored.runtime->load(path);
  AR_REQUIRE_MSG(loaded.accepted(), loaded.to_text());
  RecoveryContext context;
  context.runtime_boot_id = RuntimeBootId(2);
  context.agent_boot_id = AgentBootId(2);
  context.runtime_epoch = RuntimeEpoch(2);
  context.coordinator_epoch = CoordinatorEpoch(2);
  const MutationResult recovered = restored.runtime->recover(context);
  AR_REQUIRE_MSG(recovered.accepted(), recovered.to_text());
  AR_CHECK_EQ(restored.runtime->durable_digest() != digest, true);

  // The runtime is not resumable until backends are re-registered and bindings
  // are re-established under the new incarnation.
  ResumeContext resume;
  resume.runtime_boot_id = RuntimeBootId(3);
  resume.agent_boot_id = AgentBootId(3);
  resume.runtime_epoch = RuntimeEpoch(3);
  resume.coordinator_epoch = CoordinatorEpoch(3);
  const MutationResult without_bindings = restored.runtime->resume(resume);
  AR_CHECK(!without_bindings.accepted());

  AR_REQUIRE(restored.runtime->register_tool_backend(restored.tool->incarnation(), restored.tool)
                 .accepted());
  AR_REQUIRE(restored.runtime->register_model_backend(restored.model->incarnation(),
                                                      restored.model)
                 .accepted());
  AR_REQUIRE(restored.runtime->bind_tool(artest::make_tool_binding(restored, "hash",
                                                                  SideEffectClass::PURE))
                 .accepted());
  AR_REQUIRE(restored.runtime->bind_tool(artest::make_tool_binding(restored, "echo",
                                                                  SideEffectClass::READ_ONLY))
                 .accepted());
  AR_REQUIRE(restored.runtime->bind_model(artest::make_model_binding(restored, "reference-target"))
                 .accepted());
  AR_REQUIRE(restored.runtime->bind_memory(artest::make_memory_binding("ctx", 1, 1)).accepted());
  resume.assignment = artest::make_assignment(restored, 1, 1);
  resume.assignment->coordinator_epoch = CoordinatorEpoch(4);
  resume.runtime_boot_id = RuntimeBootId(4);
  resume.agent_boot_id = AgentBootId(4);
  resume.runtime_epoch = RuntimeEpoch(4);
  const MutationResult resumed = restored.runtime->resume(resume);
  AR_REQUIRE_MSG(resumed.accepted(), resumed.to_text());
  AR_CHECK_EQ(restored.runtime->summary().committed_actions, 1u);
  AR_CHECK(restored.runtime->check_invariants().ok);
  std::filesystem::remove(path);
}

AR_TEST(persistence, save_rejects_empty_path_and_missing_directory) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  AR_CHECK(!fixture.runtime->save("").accepted());
  const std::string missing = scratch_directory() + "/no-such-directory/state.bin";
  AR_CHECK(!fixture.runtime->save(missing).accepted());
  AR_CHECK(!fixture.runtime->load(missing).accepted());
}

AR_TEST(persistence, repeated_save_load_is_stable) {
  const std::string directory = scratch_directory();
  const std::string path = directory + "/repeat.bin";
  artest::Fixture fixture = artest::make_fixture();
  const std::uint64_t digest = populate(fixture);
  AR_REQUIRE(fixture.runtime->save(path).accepted());
  for (int i = 0; i < 5; ++i) {
    artest::Fixture restored = artest::make_fixture();
    const MutationResult loaded = restored.runtime->load(path);
    AR_REQUIRE_MSG(loaded.accepted(), loaded.to_text());
    AR_CHECK_EQ(restored.runtime->summary().committed_actions, 1u);
    AR_CHECK(restored.runtime->check_invariants().ok);
    AR_REQUIRE(restored.runtime->save(path).accepted());
    (void)digest;
  }
  AR_CHECK(fixture.runtime->check_invariants().ok);
  std::filesystem::remove(path);
}
