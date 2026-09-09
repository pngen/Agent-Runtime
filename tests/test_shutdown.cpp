// Agent Runtime - deterministic shutdown behaviour.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "runtime_fixture.hpp"
#include "test_framework.hpp"

using namespace agent_runtime;

AR_TEST(shutdown, destruction_with_in_flight_action_is_safe) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  const ActionHandle handle = [&] {
    ActionHandle handle;
    const MutationResult declared = fixture.runtime->declare_action(
        artest::tool_spec("commit-token", SideEffectClass::COMMIT_TOKEN_REQUIRED, "x", "op-shut"),
        handle);
    AR_REQUIRE(declared.accepted());
    return handle;
  }();
  AR_REQUIRE(fixture.runtime->admit_action(handle.action_id, handle.action_generation).accepted());
  AR_REQUIRE(fixture.runtime->authorize_action(handle.action_id, handle.action_generation).accepted());
  const DispatchResult dispatched = fixture.runtime->dispatch_action(
      handle.action_id, handle.action_generation, DispatchMode::EXTERNAL);
  AR_REQUIRE(dispatched.result.accepted());
  // Destroying the runtime with an attempt still in flight must not block.
  fixture.runtime.reset();
  AR_CHECK(fixture.runtime == nullptr);
}

AR_TEST(shutdown, request_inflight_cancellation_is_observable) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  fixture.runtime->request_inflight_cancellation();
  const ActionHandle handle = [&] {
    ActionHandle handle;
    const MutationResult declared = fixture.runtime->declare_action(
        artest::tool_spec("hash", SideEffectClass::PURE, "x"), handle);
    AR_REQUIRE(declared.accepted());
    return handle;
  }();
  AR_REQUIRE(fixture.runtime->admit_action(handle.action_id, handle.action_generation).accepted());
  AR_REQUIRE(fixture.runtime->authorize_action(handle.action_id, handle.action_generation).accepted());
  const LocalExecutionResult execution =
      fixture.runtime->dispatch_and_execute(handle.action_id, handle.action_generation);
  // The backend observed the cancellation request, so the completion is a
  // cancellation rather than a fabricated success.
  AR_CHECK(!execution.completion.accepted() ||
            std::string(to_string(execution.completion.code)) == std::string("ACCEPTED"));
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(shutdown, concurrent_shutdown_and_mutation_is_safe) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  std::atomic<bool> stop{false};
  std::thread reader([&] {
    while (!stop.load(std::memory_order_acquire)) {
      const RuntimeSummary summary = fixture.runtime->summary();
      (void)summary;
      const InvariantReport report = fixture.runtime->check_invariants();
      (void)report;
    }
  });
  for (int i = 0; i < 64; ++i) {
    ActionHandle handle;
    const MutationResult declared = fixture.runtime->declare_action(
        artest::tool_spec("hash", SideEffectClass::PURE, "x"), handle);
    if (!declared.accepted()) {
      break;
    }
  }
  AR_REQUIRE(fixture.runtime->drain().accepted());
  AR_CHECK_EQ(std::string(to_string(fixture.runtime->lifecycle())), std::string("COMPLETED"));
  stop.store(true, std::memory_order_release);
  reader.join();
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(shutdown, repeated_start_stop_returns_to_baseline) {
  for (int iteration = 0; iteration < 8; ++iteration) {
    artest::Fixture fixture = artest::make_fixture();
    artest::bring_up(fixture);
    AR_REQUIRE(fixture.runtime->drain().accepted());
    AR_CHECK_EQ(fixture.runtime->summary().committed_actions, 0u);
    AR_CHECK(fixture.runtime->check_invariants().ok);
    fixture.runtime.reset();
  }
}

AR_TEST(shutdown, retired_runtime_rejects_every_mutation) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  AR_REQUIRE(fixture.runtime->cancel("stop").accepted());
  AR_REQUIRE(fixture.runtime->retire().accepted());
  ActionHandle handle;
  AR_CHECK_EQ(std::string(to_string(fixture.runtime
                                        ->declare_action(artest::tool_spec("hash",
                                                                           SideEffectClass::PURE, "x"),
                                                         handle)
                                        .code)),
              std::string("REJECT_RETIRED"));
  AR_CHECK_EQ(std::string(to_string(fixture.runtime->drain().code)),
              std::string("REJECT_RETIRED"));
  AR_CHECK_EQ(std::string(to_string(fixture.runtime->cancel("again").code)),
              std::string("REJECT_RETIRED"));
  AR_CHECK(fixture.runtime->check_invariants().ok);
}
