// Agent Runtime - C++ ownership and lifetime regressions.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// Every test here is a focused regression for a class of lifetime defect that
// the manual audit covers: views into temporaries, references retained across
// container mutation, callbacks capturing dead objects and objects destroyed by
// their own worker thread.

#include <memory>
#include <string>
#include <vector>

#include "runtime_fixture.hpp"
#include "test_framework.hpp"

using namespace agent_runtime;

namespace {

/// Backend that re-enters the runtime from inside invoke(). The runtime must
/// not hold its canonical lock across a backend call, otherwise this deadlocks.
class ReentrantToolBackend final : public ToolBackend {
 public:
  void set_runtime(AgentRuntime* runtime) { runtime_ = runtime; }

  [[nodiscard]] BackendIncarnation incarnation() const override {
    BackendIncarnation incarnation;
    incarnation.backend_id = BackendId(0x5EED0001ull);
    incarnation.generation = Generation<BackendTag>(1);
    incarnation.name = "reentrant-tool";
    incarnation.available = true;
    return incarnation;
  }

  ToolResponse invoke(const ToolRequest& request, const CancellationProbe&) override {
    ToolResponse response;
    response.action_id = request.action_id;
    response.call_id = request.call_id;
    response.call_generation = request.call_generation;
    response.attempt_id = request.attempt_id;
    response.attempt_generation = request.attempt_generation;
    response.backend_generation = Generation<BackendTag>(1);
    if (runtime_ != nullptr) {
      // Re-entrant reads and invariant checks must not deadlock.
      const RuntimeSummary summary = runtime_->summary();
      const InvariantReport report = runtime_->check_invariants();
      reentered = summary.runtime_id.valid();
      invariant_ok = report.ok;
      const RuntimeSnapshot snapshot = runtime_->snapshot();
      snapshot_actions = static_cast<std::uint32_t>(snapshot.actions.size());
    }
    response.output = "reentrant-ok";
    response.result_digest = 0x1234;
    response.status = CompletionStatus::SUCCEEDED;
    return response;
  }

  bool reentered = false;
  bool invariant_ok = false;
  std::uint32_t snapshot_actions = 0;

 private:
  AgentRuntime* runtime_ = nullptr;
};

}  // namespace

AR_TEST(lifetime, snapshot_outlives_runtime) {
  RuntimeSnapshot snapshot;
  {
    artest::Fixture fixture = artest::make_fixture();
    artest::bring_up(fixture);
    ActionHandle handle;
    AR_REQUIRE(fixture.runtime
                   ->declare_action(artest::tool_spec("hash", SideEffectClass::PURE, "x"), handle)
                   .accepted());
    snapshot = fixture.runtime->snapshot();
  }
  // The fixture, its runtime and its backends are gone; the snapshot is a value.
  AR_CHECK_EQ(snapshot.actions.size(), std::size_t{1});
  AR_CHECK(!snapshot.render_text().empty());
  AR_CHECK(!snapshot.actions[0].tool_name.empty());
}

AR_TEST(lifetime, action_views_outlive_runtime) {
  std::vector<ActionView> views;
  {
    artest::Fixture fixture = artest::make_fixture();
    artest::bring_up(fixture);
    ActionHandle handle;
    AR_REQUIRE(fixture.runtime
                   ->declare_action(artest::tool_spec("hash", SideEffectClass::PURE, "x"), handle)
                   .accepted());
    views = fixture.runtime->actions();
  }
  AR_CHECK_EQ(views.size(), std::size_t{1});
  AR_CHECK_EQ(views[0].tool_name, std::string("hash"));
}

AR_TEST(lifetime, explanation_outlives_builder_and_runtime) {
  Explanation explanation;
  {
    artest::Fixture fixture = artest::make_fixture();
    artest::bring_up(fixture);
    ActionHandle handle;
    AR_REQUIRE(fixture.runtime
                   ->declare_action(artest::tool_spec("hash", SideEffectClass::PURE, "x"), handle)
                   .accepted());
    explanation = fixture.runtime->explain_action(handle.action_id, handle.action_generation);
  }
  AR_CHECK(explanation.find("action_state") != nullptr);
  AR_CHECK(!explanation.to_text().empty());
  AR_CHECK(!explanation.to_json().empty());
}

AR_TEST(lifetime, enum_names_have_static_lifetime) {
  // to_string returns a view into static storage; it must never point at a
  // temporary that dies before the caller copies it.
  std::vector<std::string> names;
  for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(RuntimeLifecycle::kCount); ++i) {
    names.emplace_back(to_string(static_cast<RuntimeLifecycle>(i)));
  }
  for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(ActionState::kCount); ++i) {
    names.emplace_back(to_string(static_cast<ActionState>(i)));
  }
  for (const std::string& name : names) {
    AR_CHECK(!name.empty());
    AR_CHECK(name != "UNKNOWN_LIFECYCLE");
    AR_CHECK(name != "UNKNOWN_ACTION_STATE");
  }
}

AR_TEST(lifetime, references_are_not_retained_across_container_mutation) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  ActionHandle first;
  AR_REQUIRE(fixture.runtime
                 ->declare_action(artest::tool_spec("hash", SideEffectClass::PURE, "1"), first)
                 .accepted());
  const ActionId first_id = first.action_id;
  // Declaring many further actions reallocates the canonical container. A
  // retained reference would dangle here; identity-based lookup must not.
  for (int i = 0; i < 256; ++i) {
    ActionHandle handle;
    AR_REQUIRE(fixture.runtime
                   ->declare_action(artest::tool_spec("hash", SideEffectClass::PURE, "x"), handle)
                   .accepted());
  }
  AR_REQUIRE(fixture.runtime->admit_action(first_id, first.action_generation).accepted());
  const Explanation explanation =
      fixture.runtime->explain_action(first_id, first.action_generation);
  AR_CHECK(explanation.find("action_state") != nullptr);
  AR_CHECK_EQ(*explanation.find("action_state"), std::string("ADMITTED"));
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(lifetime, backend_callback_may_reenter_runtime) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  auto backend = std::make_shared<ReentrantToolBackend>();
  backend->set_runtime(fixture.runtime.get());
  BackendIncarnation incarnation = backend->incarnation();
  AR_REQUIRE(fixture.runtime->register_tool_backend(incarnation, backend).accepted());
  ToolBinding binding = artest::make_tool_binding(fixture, "hash", SideEffectClass::PURE);
  binding.tool_name = "reentrant";
  binding.backend_id = incarnation.backend_id;
  binding.backend_generation = incarnation.generation;
  AR_REQUIRE(fixture.runtime->bind_tool(binding).accepted());

  ActionHandle handle;
  AR_REQUIRE(fixture.runtime
                 ->declare_action(artest::tool_spec("reentrant", SideEffectClass::PURE, "x"), handle)
                 .accepted());
  AR_REQUIRE(fixture.runtime->admit_action(handle.action_id, handle.action_generation).accepted());
  AR_REQUIRE(fixture.runtime->authorize_action(handle.action_id, handle.action_generation).accepted());
  const LocalExecutionResult execution =
      fixture.runtime->dispatch_and_execute(handle.action_id, handle.action_generation);
  AR_REQUIRE_MSG(execution.completion.accepted(), execution.completion.to_text());
  AR_CHECK(backend->reentered);
  AR_CHECK(backend->invariant_ok);
  AR_REQUIRE(fixture.runtime->commit_action(handle.action_id, handle.action_generation).accepted());
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(lifetime, repeated_construction_and_destruction_is_safe) {
  for (int iteration = 0; iteration < 16; ++iteration) {
    artest::Fixture fixture = artest::make_fixture();
    artest::bring_up(fixture);
    const ActionHandle handle =
        [&] {
          ActionHandle handle;
          const MutationResult result = fixture.runtime->declare_action(
              artest::tool_spec("hash", SideEffectClass::PURE, "x"), handle);
          AR_REQUIRE(result.accepted());
          return handle;
        }();
    AR_REQUIRE(fixture.runtime->admit_action(handle.action_id, handle.action_generation).accepted());
    AR_REQUIRE(fixture.runtime->authorize_action(handle.action_id, handle.action_generation).accepted());
    AR_REQUIRE(fixture.runtime->dispatch_and_execute(handle.action_id, handle.action_generation)
                   .completion.accepted());
    AR_REQUIRE(fixture.runtime->commit_action(handle.action_id, handle.action_generation).accepted());
  }
}

AR_TEST(lifetime, moved_from_options_are_not_used_as_authority) {
  // Options are moved into the runtime; the caller's copy must not be required
  // to stay alive and the runtime must own everything it needs.
  AgentRuntimeOptions options;
  options.runtime_id = AgentRuntimeId(1234);
  options.agent_id = AgentId(5678);
  options.clock = std::make_shared<LogicalClock>();
  options.policy = RuntimePolicy::permissive();
  options.policy.require_assignment_for_dispatch = false;
  AgentRuntime runtime(std::move(options));
  AR_REQUIRE(runtime.initialize().accepted());
  AR_CHECK_EQ(runtime.id().value(), 1234ull);
  AR_CHECK_EQ(runtime.summary().agent_id.value(), 5678ull);
  AR_CHECK(runtime.check_invariants().ok);
}
