// Agent Runtime - reference backend behaviour and independent incarnations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include <filesystem>
#include <string>

#include "runtime_fixture.hpp"
#include "test_framework.hpp"

using namespace agent_runtime;

namespace {

[[nodiscard]] std::string scratch_root() {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "agent_runtime_tests_tools";
  std::error_code error;
  std::filesystem::create_directories(path, error);
  return path.string();
}

[[nodiscard]] ToolRequest make_request(const std::string& tool, SideEffectClass side_effect,
                                       std::string input, std::string key = {}) {
  ToolRequest request;
  request.action_id = ActionId(1);
  request.call_id = ToolCallId(1);
  request.call_generation = ToolCallGeneration(1);
  request.attempt_id = ActionAttemptId(1);
  request.attempt_generation = AttemptGeneration(1);
  request.runtime_id = AgentRuntimeId(1);
  request.runtime_boot_id = RuntimeBootId(1);
  request.tool_name = tool;
  request.side_effect = side_effect;
  request.input = std::move(input);
  request.operation_key = std::move(key);
  request.max_output_bytes = 65536;
  return request;
}

}  // namespace

AR_TEST(backends, reference_model_is_deterministic) {
  ReferenceModelBackend backend("reference-model", 1);
  ModelRequest request;
  request.target = "reference-target";
  request.prompt = "deterministic prompt";
  request.max_output_bytes = 4096;
  request.call_generation = ModelCallGeneration(1);
  request.attempt_generation = AttemptGeneration(1);
  const ModelResponse first = backend.invoke(request, CancellationProbe());
  const ModelResponse second = backend.invoke(request, CancellationProbe());
  AR_CHECK_EQ(std::string(to_string(first.status)), std::string("SUCCEEDED"));
  AR_CHECK_EQ(first.output, second.output);
  AR_CHECK_EQ(backend.invocation_count(), 2ull);
  AR_CHECK(!first.output.empty());
}

AR_TEST(backends, reference_model_honours_cancellation_probe) {
  ReferenceModelBackend backend("reference-model", 1);
  auto flag = std::make_shared<std::atomic<bool>>(true);
  ModelRequest request;
  request.target = "reference-target";
  request.prompt = "cancelled";
  request.call_generation = ModelCallGeneration(1);
  request.attempt_generation = AttemptGeneration(1);
  const ModelResponse response = backend.invoke(request, CancellationProbe(flag));
  AR_CHECK_EQ(std::string(to_string(response.status)), std::string("CANCELLED"));
}

AR_TEST(backends, deterministic_tools) {
  ReferenceToolBackend backend("reference-tool", 1);
  const ToolResponse hash =
      backend.invoke(make_request("hash", SideEffectClass::PURE, "abc"), CancellationProbe());
  AR_CHECK_EQ(std::string(to_string(hash.status)), std::string("SUCCEEDED"));
  AR_CHECK_EQ(hash.output.size(), std::size_t{64});
  const ToolResponse sum = backend.invoke(
      make_request("vector-sum", SideEffectClass::PURE, "1 2 3 4 5"), CancellationProbe());
  AR_CHECK_EQ(sum.output, std::string("15"));
  const ToolResponse overflow = backend.invoke(
      make_request("vector-sum", SideEffectClass::PURE,
                   "9223372036854775807 1"),
      CancellationProbe());
  AR_CHECK_EQ(std::string(to_string(overflow.status)), std::string("FAILED"));
  AR_CHECK_EQ(std::string(to_string(overflow.failure_class)), std::string("PERMANENT"));
  const ToolResponse unknown =
      backend.invoke(make_request("no-such-tool", SideEffectClass::PURE, "x"), CancellationProbe());
  AR_CHECK_EQ(std::string(to_string(unknown.status)), std::string("FAILED"));
}

AR_TEST(backends, scratch_transform_stays_inside_root) {
  ReferenceToolBackend backend("reference-tool", 1);
  const std::string root = scratch_root();
  backend.set_journal_directory(root);
  const ToolResponse ok = backend.invoke(
      make_request("scratch-transform", SideEffectClass::IDEMPOTENT, "nested/file.txt|hello"),
      CancellationProbe());
  AR_CHECK_EQ(std::string(to_string(ok.status)), std::string("SUCCEEDED"));
  AR_CHECK(std::filesystem::exists(std::filesystem::path(root) / "nested" / "file.txt"));

  const ToolResponse traversal = backend.invoke(
      make_request("scratch-transform", SideEffectClass::IDEMPOTENT, "../escape.txt|bad"),
      CancellationProbe());
  AR_CHECK_EQ(std::string(to_string(traversal.status)), std::string("FAILED"));
  const ToolResponse absolute = backend.invoke(
      make_request("scratch-transform", SideEffectClass::IDEMPOTENT, "C:/windows/system32/x|bad"),
      CancellationProbe());
  AR_CHECK_EQ(std::string(to_string(absolute.status)), std::string("FAILED"));
  const ToolResponse drive_relative = backend.invoke(
      make_request("scratch-transform", SideEffectClass::IDEMPOTENT, "..\\escape|bad"),
      CancellationProbe());
  AR_CHECK_EQ(std::string(to_string(drive_relative.status)), std::string("FAILED"));
  AR_CHECK(!std::filesystem::exists(std::filesystem::path(root).parent_path() / "escape.txt"));
  std::error_code error;
  std::filesystem::remove_all(root, error);
}

AR_TEST(backends, operation_key_deduplicates_commit_token) {
  ReferenceToolBackend backend("reference-tool", 1);
  backend.set_journal_directory(scratch_root());
  const ToolRequest request =
      make_request("commit-token", SideEffectClass::COMMIT_TOKEN_REQUIRED, "charge", "op-42");
  const ToolResponse first = backend.invoke(request, CancellationProbe());
  AR_CHECK_EQ(std::string(to_string(first.status)), std::string("SUCCEEDED"));
  AR_CHECK(!first.deduplicated);
  const ToolResponse second = backend.invoke(request, CancellationProbe());
  AR_CHECK(second.deduplicated);
  AR_CHECK_EQ(first.output, second.output);
  AR_CHECK_EQ(backend.receipt_count(), 1ull);
  AR_CHECK(backend.has_receipt("op-42"));
  std::error_code error;
  std::filesystem::remove_all(scratch_root(), error);
}

AR_TEST(backends, missing_operation_key_is_rejected) {
  ReferenceToolBackend backend("reference-tool", 1);
  const ToolResponse response = backend.invoke(
      make_request("commit-token", SideEffectClass::COMMIT_TOKEN_REQUIRED, "charge"),
      CancellationProbe());
  AR_CHECK_EQ(std::string(to_string(response.status)), std::string("FAILED"));
  AR_CHECK_EQ(std::string(to_string(response.failure_class)), std::string("PERMANENT"));
}

AR_TEST(backends, ambiguous_completion_preserves_uncertainty) {
  ReferenceToolBackend backend("reference-tool", 1);
  backend.set_journal_directory(scratch_root());
  backend.crash_after_side_effect_once();
  const ToolResponse response = backend.invoke(
      make_request("commit-token", SideEffectClass::COMMIT_TOKEN_REQUIRED, "charge", "op-amb"),
      CancellationProbe());
  AR_CHECK_EQ(std::string(to_string(response.status)), std::string("AMBIGUOUS"));
  AR_CHECK(backend.has_receipt("op-amb"));
  std::error_code error;
  std::filesystem::remove_all(scratch_root(), error);
}

AR_TEST(backends, independent_incarnations_do_not_invalidate_each_other) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);

  auto second_tool = std::make_shared<ReferenceToolBackend>("reference-tool-2", 1);
  BackendIncarnation second_incarnation = second_tool->incarnation();
  second_incarnation.backend_id = BackendId(0x7E570999ull);
  AR_REQUIRE(fixture.runtime->register_tool_backend(second_incarnation, second_tool).accepted());
  ToolBinding second_binding = artest::make_tool_binding(fixture, "hash", SideEffectClass::PURE);
  second_binding.backend_id = second_incarnation.backend_id;
  second_binding.backend_generation = second_incarnation.generation;
  second_binding.tool_name = "second-hash";
  AR_REQUIRE(fixture.runtime->bind_tool(second_binding).accepted());

  // Reincarnating tool 2 must not disturb the binding of tool 1.
  BackendIncarnation reincarnated = second_incarnation;
  reincarnated.generation = Generation<BackendTag>(2);
  AR_REQUIRE(fixture.runtime->register_tool_backend(reincarnated, second_tool).accepted());
  const RuntimeSnapshot snapshot = fixture.runtime->snapshot();
  bool first_binding_current = false;
  for (const ToolBinding& binding : snapshot.tool_bindings) {
    if (binding.tool_name == "hash") {
      first_binding_current = binding.current;
    }
  }
  AR_CHECK(first_binding_current);

  const MutationResult stale = fixture.runtime->register_tool_backend(second_incarnation,
                                                                      second_tool);
  AR_CHECK_EQ(std::string(to_string(stale.code)), std::string("REJECT_STALE_TOOL_BINDING"));
}

AR_TEST(backends, model_backends_are_independent) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  auto second_model = std::make_shared<ReferenceModelBackend>("reference-model-2", 1);
  BackendIncarnation incarnation = second_model->incarnation();
  incarnation.backend_id = BackendId(0xA1B2C999ull);
  AR_REQUIRE(fixture.runtime->register_model_backend(incarnation, second_model).accepted());
  BackendIncarnation reincarnated = incarnation;
  reincarnated.generation = Generation<BackendTag>(3);
  AR_REQUIRE(fixture.runtime->register_model_backend(reincarnated, second_model).accepted());
  const RuntimeSnapshot snapshot = fixture.runtime->snapshot();
  bool first_current = false;
  for (const ModelBinding& binding : snapshot.model_bindings) {
    if (binding.target == "reference-target") {
      first_current = binding.current;
    }
  }
  AR_CHECK(first_current);
  AR_CHECK(fixture.runtime->check_invariants().ok);
}
