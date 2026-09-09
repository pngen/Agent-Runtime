// Agent Runtime - deterministic race tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// Races are released with std::latch/atomic flags, never with sleeps, and every
// race asserts that the observed outcome is one of the legal outcomes and that
// canonical state stays consistent.

#include <atomic>
#include <latch>
#include <string>
#include <thread>
#include <vector>

#include "runtime_fixture.hpp"
#include "test_framework.hpp"

using namespace agent_runtime;

namespace {

[[nodiscard]] ToolResponse race_response(const ToolRequest& request, std::string output) {
  ToolResponse response;
  response.action_id = request.action_id;
  response.call_id = request.call_id;
  response.call_generation = request.call_generation;
  response.attempt_id = request.attempt_id;
  response.attempt_generation = request.attempt_generation;
  response.backend_generation = Generation<BackendTag>(1);
  response.status = CompletionStatus::SUCCEEDED;
  response.output = std::move(output);
  response.result_digest = 777;
  return response;
}

/// Runs two operations concurrently, released by a latch, and returns both
/// outcomes.
template <class First, class Second>
std::pair<MutationResult, MutationResult> race(First first, Second second) {
  std::latch ready(2);
  std::latch go(1);
  MutationResult first_result;
  MutationResult second_result;
  std::thread first_thread([&] {
    ready.count_down();
    go.wait();
    first_result = first();
  });
  std::thread second_thread([&] {
    ready.count_down();
    go.wait();
    second_result = second();
  });
  ready.wait();
  go.count_down();
  first_thread.join();
  second_thread.join();
  return {first_result, second_result};
}

}  // namespace

AR_TEST(races, cancel_versus_action_admission) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  ActionHandle handle;
  AR_REQUIRE(fixture.runtime
                 ->declare_action(artest::tool_spec("hash", SideEffectClass::PURE, "x"), handle)
                 .accepted());
  const auto outcomes = race(
      [&] { return fixture.runtime->cancel("race"); },
      [&] { return fixture.runtime->admit_action(handle.action_id, handle.action_generation); });
  const std::string first = std::string(to_string(outcomes.first.code));
  const std::string second = std::string(to_string(outcomes.second.code));
  AR_CHECK(first == "ACCEPTED" || first == "NO_CHANGE");
  AR_CHECK(second == "ACCEPTED" || second == "REJECT_CANCELLED" ||
           second == "SHUTTING_DOWN" || second == "NO_CHANGE");
  if (second == "REJECT_CANCELLED" || second == "SHUTTING_DOWN") {
    AR_CHECK_EQ(fixture.runtime->summary().committed_actions, 0u);
  }
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(races, cancel_versus_dispatch) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  ActionHandle handle;
  AR_REQUIRE(fixture.runtime
                 ->declare_action(artest::tool_spec("hash", SideEffectClass::PURE, "x"), handle)
                 .accepted());
  AR_REQUIRE(fixture.runtime->admit_action(handle.action_id, handle.action_generation).accepted());
  AR_REQUIRE(fixture.runtime->authorize_action(handle.action_id, handle.action_generation).accepted());
  DispatchResult dispatched;
  const auto outcomes = race(
      [&] { return fixture.runtime->cancel("race"); },
      [&] {
        dispatched = fixture.runtime->dispatch_action(handle.action_id, handle.action_generation,
                                                      DispatchMode::EXTERNAL);
        return dispatched.result;
      });
  AR_CHECK(std::string(to_string(outcomes.first.code)) == std::string("ACCEPTED"));
  const std::string second = std::string(to_string(outcomes.second.code));
  AR_CHECK(second == "ACCEPTED" || second == "REJECT_CANCELLED" || second == "SHUTTING_DOWN");
  if (second == "ACCEPTED") {
    const MutationResult completion =
        fixture.runtime->submit_tool_completion(race_response(dispatched.tool_request, "late"));
    AR_CHECK(!completion.accepted());
  }
  AR_CHECK_EQ(fixture.runtime->summary().committed_actions, 0u);
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(races, cancel_versus_commit) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  ActionHandle handle;
  AR_REQUIRE(fixture.runtime
                 ->declare_action(artest::tool_spec("hash", SideEffectClass::PURE, "x"), handle)
                 .accepted());
  AR_REQUIRE(fixture.runtime->admit_action(handle.action_id, handle.action_generation).accepted());
  AR_REQUIRE(fixture.runtime->authorize_action(handle.action_id, handle.action_generation).accepted());
  AR_REQUIRE(fixture.runtime->dispatch_and_execute(handle.action_id, handle.action_generation)
                 .completion.accepted());
  const auto outcomes = race(
      [&] { return fixture.runtime->cancel("race"); },
      [&] { return fixture.runtime->commit_action(handle.action_id, handle.action_generation); });
  AR_CHECK(std::string(to_string(outcomes.first.code)) == std::string("ACCEPTED"));
  const std::string second = std::string(to_string(outcomes.second.code));
  AR_CHECK(second == "ACCEPTED" || second == "REJECT_CANCELLED");
  const RuntimeSummary summary = fixture.runtime->summary();
  if (second == "REJECT_CANCELLED") {
    AR_CHECK_EQ(summary.committed_actions, 0u);
  } else {
    AR_CHECK_EQ(summary.committed_actions, 1u);
  }
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(races, cancel_versus_tool_completion) {
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
  const auto outcomes = race(
      [&] { return fixture.runtime->cancel("race"); },
      [&] {
        return fixture.runtime->submit_tool_completion(
            race_response(dispatched.tool_request, "late"));
      });
  AR_CHECK(std::string(to_string(outcomes.first.code)) == std::string("ACCEPTED"));
  const std::string second = std::string(to_string(outcomes.second.code));
  AR_CHECK(second == "ACCEPTED" || second == "REJECT_CANCELLED" ||
           second == "REJECT_STALE_ATTEMPT");
  AR_CHECK_EQ(fixture.runtime->summary().committed_actions, 0u);
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(races, policy_change_versus_dispatch) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  ActionHandle handle;
  AR_REQUIRE(fixture.runtime
                 ->declare_action(artest::tool_spec("hash", SideEffectClass::PURE, "x"), handle)
                 .accepted());
  AR_REQUIRE(fixture.runtime->admit_action(handle.action_id, handle.action_generation).accepted());
  AR_REQUIRE(fixture.runtime->authorize_action(handle.action_id, handle.action_generation).accepted());
  const auto outcomes = race(
      [&] {
        RuntimePolicy policy = RuntimePolicy::permissive();
        policy.require_assignment_for_dispatch = true;
        policy.generation = PolicyGeneration(2);
        return fixture.runtime->set_policy(policy);
      },
      [&] {
        return fixture.runtime
            ->dispatch_action(handle.action_id, handle.action_generation, DispatchMode::EXTERNAL)
            .result;
      });
  AR_CHECK(std::string(to_string(outcomes.first.code)) == std::string("ACCEPTED"));
  const std::string second = std::string(to_string(outcomes.second.code));
  AR_CHECK(second == "ACCEPTED" || second == "REJECT_STALE_POLICY" ||
           second == "REJECT_NOT_READY" || second == "REVALIDATION_REQUIRED");
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(races, budget_change_versus_dispatch) {
  artest::Fixture fixture = artest::make_fixture(true, true);
  artest::bring_up(fixture);
  AR_REQUIRE(fixture.runtime->set_budget(artest::make_budget(1)).accepted());
  ActionHandle handle;
  AR_REQUIRE(fixture.runtime
                 ->declare_action(artest::tool_spec("hash", SideEffectClass::PURE, "x"), handle)
                 .accepted());
  AR_REQUIRE(fixture.runtime->admit_action(handle.action_id, handle.action_generation).accepted());
  AR_REQUIRE(fixture.runtime->authorize_action(handle.action_id, handle.action_generation).accepted());
  const auto outcomes = race(
      [&] { return fixture.runtime->set_budget(artest::make_budget(2)); },
      [&] {
        return fixture.runtime
            ->dispatch_action(handle.action_id, handle.action_generation, DispatchMode::EXTERNAL)
            .result;
      });
  AR_CHECK(std::string(to_string(outcomes.first.code)) == std::string("ACCEPTED"));
  const std::string second = std::string(to_string(outcomes.second.code));
  AR_CHECK(second == "ACCEPTED" || second == "REJECT_STALE_BUDGET");
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(races, memory_change_versus_commit) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  AR_REQUIRE(fixture.runtime->bind_memory(artest::make_memory_binding("ctx", 1, 1)).accepted());
  ActionSpec spec = artest::tool_spec("hash", SideEffectClass::PURE, "x");
  spec.memory_binding_identity = "ctx";
  ActionHandle handle;
  AR_REQUIRE(fixture.runtime->declare_action(spec, handle).accepted());
  AR_REQUIRE(fixture.runtime->admit_action(handle.action_id, handle.action_generation).accepted());
  AR_REQUIRE(fixture.runtime->authorize_action(handle.action_id, handle.action_generation).accepted());
  AR_REQUIRE(fixture.runtime->dispatch_and_execute(handle.action_id, handle.action_generation)
                 .completion.accepted());
  const auto outcomes = race(
      [&] { return fixture.runtime->bind_memory(artest::make_memory_binding("ctx", 1, 9)); },
      [&] { return fixture.runtime->commit_action(handle.action_id, handle.action_generation); });
  AR_CHECK(std::string(to_string(outcomes.first.code)) == std::string("ACCEPTED"));
  const std::string second = std::string(to_string(outcomes.second.code));
  AR_CHECK(second == "ACCEPTED" || second == "REJECT_STALE_MEMORY_BINDING" ||
           second == "REVALIDATION_REQUIRED" || second == "REJECT_NOT_READY");
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(races, tool_incarnation_change_versus_completion) {
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
  const auto outcomes = race(
      [&] {
        return fixture.runtime->bind_tool(artest::make_tool_binding(
            fixture, "echo", SideEffectClass::READ_ONLY, 2, 2));
      },
      [&] {
        return fixture.runtime->submit_tool_completion(
            race_response(dispatched.tool_request, "racy"));
      });
  AR_CHECK(std::string(to_string(outcomes.first.code)) == std::string("ACCEPTED"));
  const std::string second = std::string(to_string(outcomes.second.code));
  AR_CHECK(second == "ACCEPTED" || second == "REJECT_STALE_TOOL_BINDING");
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(races, suspend_versus_new_action) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  ActionHandle handle;
  const auto outcomes = race(
      [&] { return fixture.runtime->suspend("race"); },
      [&] {
        return fixture.runtime->declare_action(
            artest::tool_spec("hash", SideEffectClass::PURE, "x"), handle);
      });
  const std::string first = std::string(to_string(outcomes.first.code));
  const std::string second = std::string(to_string(outcomes.second.code));
  AR_CHECK(first == "ACCEPTED" || first == "DEFERRED");
  AR_CHECK(second == "ACCEPTED" || second == "SHUTTING_DOWN" || second == "REJECT_NOT_READY");
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(races, drain_versus_action_creation) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  ActionHandle handle;
  const auto outcomes = race(
      [&] { return fixture.runtime->drain(); },
      [&] {
        return fixture.runtime->declare_action(
            artest::tool_spec("hash", SideEffectClass::PURE, "x"), handle);
      });
  const std::string first = std::string(to_string(outcomes.first.code));
  const std::string second = std::string(to_string(outcomes.second.code));
  AR_CHECK(first == "ACCEPTED" || first == "DEFERRED");
  AR_CHECK(second == "ACCEPTED" || second == "SHUTTING_DOWN" || second == "REJECT_NOT_READY" ||
           second == "REJECT_COMPLETED");
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(races, concurrent_completions_commit_once) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  std::vector<ActionHandle> handles;
  std::vector<ToolRequest> requests;
  for (int i = 0; i < 8; ++i) {
    ActionHandle handle;
    AR_REQUIRE(fixture.runtime
                   ->declare_action(artest::tool_spec("hash", SideEffectClass::PURE, "x"), handle)
                   .accepted());
    AR_REQUIRE(fixture.runtime->admit_action(handle.action_id, handle.action_generation).accepted());
    AR_REQUIRE(fixture.runtime->authorize_action(handle.action_id, handle.action_generation).accepted());
    handles.push_back(handle);
  }
  for (const ActionHandle& handle : handles) {
    const DispatchResult dispatched = fixture.runtime->dispatch_action(
        handle.action_id, handle.action_generation, DispatchMode::EXTERNAL);
    AR_REQUIRE(dispatched.result.accepted());
    requests.push_back(dispatched.tool_request);
  }
  std::latch ready(static_cast<std::ptrdiff_t>(requests.size()));
  std::latch go(1);
  std::vector<std::thread> threads;
  for (const ToolRequest& request : requests) {
    threads.emplace_back([&, request] {
      ready.count_down();
      go.wait();
      (void)fixture.runtime->submit_tool_completion(race_response(request, "concurrent"));
    });
  }
  ready.wait();
  go.count_down();
  for (std::thread& thread : threads) {
    thread.join();
  }
  for (const ActionHandle& handle : handles) {
    (void)fixture.runtime->commit_action(handle.action_id, handle.action_generation);
  }
  AR_CHECK_EQ(fixture.runtime->summary().committed_actions, 8u);
  AR_CHECK_EQ(fixture.runtime->summary().progress_generation.value(), 8ull);
  AR_CHECK(fixture.runtime->check_invariants().ok);
}

AR_TEST(races, concurrent_readers_never_observe_inconsistent_state) {
  artest::Fixture fixture = artest::make_fixture();
  artest::bring_up(fixture);
  // Readers perform a bounded number of observations: the proof is that no
  // inconsistent state is ever observed, not that the threads spin forever.
  std::vector<std::thread> readers;
  for (int i = 0; i < 4; ++i) {
    readers.emplace_back([&] {
      for (int iteration = 0; iteration < 128; ++iteration) {
        const RuntimeSnapshot snapshot = fixture.runtime->snapshot();
        if (snapshot.summary.committed_actions > snapshot.summary.total_actions) {
          AR_CHECK_MSG(false, "committed count exceeded total actions in a snapshot");
          return;
        }
        if (!fixture.runtime->check_invariants().ok) {
          AR_CHECK_MSG(false, "invariant violation observed by a concurrent reader");
          return;
        }
      }
    });
  }
  for (int i = 0; i < 32; ++i) {
    ActionHandle handle;
    if (!fixture.runtime->declare_action(artest::tool_spec("hash", SideEffectClass::PURE, "x"),
                                         handle)
             .accepted()) {
      break;
    }
    if (!fixture.runtime->admit_action(handle.action_id, handle.action_generation).accepted()) {
      continue;
    }
    if (!fixture.runtime->authorize_action(handle.action_id, handle.action_generation).accepted()) {
      continue;
    }
    (void)fixture.runtime->dispatch_and_execute(handle.action_id, handle.action_generation);
    (void)fixture.runtime->commit_action(handle.action_id, handle.action_generation);
  }
  for (std::thread& thread : readers) {
    thread.join();
  }
  AR_CHECK(fixture.runtime->check_invariants().ok);
}
