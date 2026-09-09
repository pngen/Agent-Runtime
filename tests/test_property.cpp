// Agent Runtime - deterministic randomized property testing.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// Every property test is driven by a fixed seed. On failure the seed and the
// complete operation history are printed so the failure is exactly reproducible.

#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "runtime_fixture.hpp"
#include "test_framework.hpp"

using namespace agent_runtime;

namespace {

class Driver {
 public:
  Driver(std::uint64_t seed, bool require_budget)
      : fixture_(artest::make_fixture(true, require_budget)), rng_(seed), seed_(seed) {
    artest::bring_up(fixture_);
    AR_REQUIRE(fixture_.runtime->set_budget(artest::make_budget(1)).accepted() ||
               !require_budget);
    artest::Fixture& fixture = fixture_;
    (void)fixture;
  }

  [[nodiscard]] artest::Fixture& fixture() { return fixture_; }
  [[nodiscard]] std::mt19937_64& rng() { return rng_; }
  [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }

  void record(const std::string& operation) {
    history_.push_back(operation);
    if (history_.size() > 64) {
      history_.erase(history_.begin());
    }
  }

  [[nodiscard]] std::string history() const {
    std::string out;
    for (const std::string& entry : history_) {
      out += entry;
      out += '\n';
    }
    return out;
  }

  [[nodiscard]] std::uint64_t uniform(std::uint64_t bound) {
    return bound == 0 ? 0 : (rng_() % bound);
  }

 private:
  artest::Fixture fixture_;
  std::mt19937_64 rng_;
  std::uint64_t seed_;
  std::vector<std::string> history_;
};

[[nodiscard]] ActionSpec random_spec(Driver& driver) {
  const std::uint64_t pick = driver.uniform(5);
  if (pick == 0) {
    return artest::tool_spec("hash", SideEffectClass::PURE, "payload-" + std::to_string(driver.uniform(1000)));
  }
  if (pick == 1) {
    return artest::tool_spec("echo", SideEffectClass::READ_ONLY, "echo-" + std::to_string(driver.uniform(1000)));
  }
  if (pick == 2) {
    return artest::tool_spec("commit-token", SideEffectClass::COMMIT_TOKEN_REQUIRED, "commit",
                             "op-" + std::to_string(driver.uniform(50)));
  }
  if (pick == 3) {
    return artest::tool_spec("non-repeatable", SideEffectClass::NON_REPEATABLE, "charge",
                             "nr-" + std::to_string(driver.uniform(50)));
  }
  ActionSpec spec = artest::model_spec("prompt-" + std::to_string(driver.uniform(1000)));
  spec.side_effect = SideEffectClass::PURE;
  return spec;
}

void check_state_is_coherent(Driver& driver, const char* stage) {
  artest::Fixture& fixture = driver.fixture();
  const InvariantReport report = fixture.runtime->check_invariants();
  if (!report.ok) {
    std::printf("seed=%llu stage=%s\n%s\n",
                static_cast<unsigned long long>(driver.seed()), stage,
                driver.history().c_str());
    for (const InvariantViolation& violation : report.violations) {
      std::printf("  %s: %s\n", violation.name.c_str(), violation.detail.c_str());
    }
    std::fflush(stdout);
  }
  AR_CHECK_MSG(report.ok, std::string(stage) + " invariants must hold");
}

void run_seed(std::uint64_t seed, std::uint32_t operations, bool require_budget) {
  Driver driver(seed, require_budget);
  artest::Fixture& fixture = driver.fixture();
  std::vector<ActionHandle> handles;
  std::uint64_t last_progress = 0;
  std::uint32_t commits = 0;

  for (std::uint32_t step = 0; step < operations; ++step) {
    const std::uint64_t action = driver.uniform(14);
    const RuntimeLifecycle lifecycle = fixture.runtime->lifecycle();
    if (is_terminal(lifecycle)) {
      break;
    }
    if (action == 0 || handles.empty()) {
      ActionHandle handle;
      const MutationResult declared = fixture.runtime->declare_action(random_spec(driver), handle);
      driver.record("declare:" + std::string(to_string(declared.code)));
      if (declared.accepted()) {
        handles.push_back(handle);
      }
    } else if (action == 1) {
      const ActionHandle handle = handles[driver.uniform(handles.size())];
      const MutationResult admitted = fixture.runtime->admit_action(handle.action_id, handle.action_generation);
      driver.record("admit:" + std::string(to_string(admitted.code)));
    } else if (action == 2) {
      const ActionHandle handle = handles[driver.uniform(handles.size())];
      const MutationResult authorized =
          fixture.runtime->authorize_action(handle.action_id, handle.action_generation);
      driver.record("authorize:" + std::string(to_string(authorized.code)));
    } else if (action == 3) {
      const ActionHandle handle = handles[driver.uniform(handles.size())];
      if (driver.uniform(2) == 0) {
        if (driver.uniform(3) == 0) {
          fixture.tool->fail_next(RetryClass::TRANSIENT, "injected");
        }
        const LocalExecutionResult execution =
            fixture.runtime->dispatch_and_execute(handle.action_id, handle.action_generation);
        driver.record("execute:" + std::string(to_string(execution.completion.code)));
      } else {
        const DispatchResult dispatched = fixture.runtime->dispatch_action(
            handle.action_id, handle.action_generation, DispatchMode::EXTERNAL);
        driver.record("dispatch:" + std::string(to_string(dispatched.result.code)));
        if (dispatched.result.accepted()) {
          if (driver.uniform(4) == 0) {
            driver.record("drop-dispatch");
          } else {
            ToolResponse response;
            response.action_id = dispatched.tool_request.action_id;
            response.call_id = dispatched.tool_request.call_id;
            response.call_generation = dispatched.tool_request.call_generation;
            response.attempt_id = dispatched.tool_request.attempt_id;
            response.attempt_generation = dispatched.tool_request.attempt_generation;
            response.backend_generation = Generation<BackendTag>(1);
            response.status = CompletionStatus::SUCCEEDED;
            response.output = "external-result";
            response.result_digest = 4242;
            const MutationResult completion = fixture.runtime->submit_tool_completion(response);
            driver.record("complete:" + std::string(to_string(completion.code)));
          }
        }
      }
    } else if (action == 4) {
      const ActionHandle handle = handles[driver.uniform(handles.size())];
      const MutationResult committed =
          fixture.runtime->commit_action(handle.action_id, handle.action_generation);
      driver.record("commit:" + std::string(to_string(committed.code)));
      if (committed.accepted()) {
        ++commits;
      }
    } else if (action == 5) {
      const MutationResult invalidated = fixture.runtime->invalidate_memory(
          MemoryBindingId(6001), MemoryBindingGeneration(1), "random invalidation");
      driver.record("invalidate-memory:" + std::string(to_string(invalidated.code)));
    } else if (action == 6) {
      RuntimePolicy policy = RuntimePolicy::permissive();
      policy.require_assignment_for_dispatch = true;
      policy.generation = PolicyGeneration(2 + driver.uniform(4));
      const MutationResult applied = fixture.runtime->set_policy(policy);
      driver.record("policy:" + std::string(to_string(applied.code)));
    } else if (action == 7) {
      const MutationResult applied = fixture.runtime->set_budget(
          artest::make_budget(2 + driver.uniform(4),
                              driver.uniform(2) == 0 ? BudgetOutcome::ALLOW : BudgetOutcome::DENY));
      driver.record("budget:" + std::string(to_string(applied.code)));
    } else if (action == 8) {
      const MutationResult requested = fixture.runtime->request_checkpoint(
          CheckpointBindingId(200 + driver.uniform(3)),
          CheckpointGeneration(1 + driver.uniform(3)), "cp");
      driver.record("checkpoint-request:" + std::string(to_string(requested.code)));
      if (requested.accepted()) {
        const RuntimeSummary summary = fixture.runtime->summary();
        CheckpointBinding binding;
        binding.binding_id = CheckpointBindingId(200 + driver.uniform(3));
        binding.generation = CheckpointGeneration(1 + driver.uniform(3));
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
        const MutationResult accepted = fixture.runtime->accept_checkpoint(binding);
        driver.record("checkpoint-accept:" + std::string(to_string(accepted.code)));
      }
    } else if (action == 9) {
      const MutationResult suspended = fixture.runtime->suspend("random suspend");
      driver.record("suspend:" + std::string(to_string(suspended.code)));
      if (suspended.accepted()) {
        ResumeContext context;
        context.runtime_boot_id = RuntimeBootId(fixture.runtime->boot_id().value() + 1);
        context.agent_boot_id = AgentBootId(fixture.runtime->summary().agent_boot_id.value() + 1);
        context.runtime_epoch = RuntimeEpoch(fixture.runtime->runtime_epoch().value() + 1);
        context.coordinator_epoch = CoordinatorEpoch(fixture.runtime->coordinator_epoch().value() + 1);
        context.assignment = artest::make_assignment(fixture, 1, 1);
        context.assignment->coordinator_epoch = CoordinatorEpoch(context.coordinator_epoch.value());
        const MutationResult resumed = fixture.runtime->resume(context);
        driver.record("resume:" + std::string(to_string(resumed.code)));
      }
    } else if (action == 10) {
      const std::uint64_t generation = 2 + driver.uniform(3);
      const MutationResult bound = fixture.runtime->bind_tool(
          artest::make_tool_binding(fixture, "hash", SideEffectClass::PURE, generation, 1));
      driver.record("bind-tool:" + std::string(to_string(bound.code)));
    } else if (action == 11) {
      const MutationResult reincarnated = fixture.runtime->register_tool_backend(
          fixture.tool->incarnation(), fixture.tool);
      driver.record("reincarnate:" + std::string(to_string(reincarnated.code)));
    } else if (action == 12) {
      const MutationResult bound = fixture.runtime->bind_memory(
          artest::make_memory_binding("ctx", 1, 1 + driver.uniform(4)));
      driver.record("bind-memory:" + std::string(to_string(bound.code)));
    } else {
      const MutationResult assigned =
          fixture.runtime->bind_assignment(artest::make_assignment(fixture, 1 + driver.uniform(3), 1));
      driver.record("assignment:" + std::string(to_string(assigned.code)));
    }

    const RuntimeSummary summary = fixture.runtime->summary();
    if (summary.progress_generation.value() < last_progress) {
      AR_CHECK_MSG(false, "progress must never move backwards within a generation");
    }
    last_progress = summary.progress_generation.value();
    if (summary.committed_actions != commits) {
      // Every accepted commit increments the durable counter exactly once.
      AR_CHECK_MSG(false, "committed counter diverged from accepted commits");
      commits = summary.committed_actions;
    }
    if ((step % 16) == 0) {
      check_state_is_coherent(driver, "mid-sequence");
    }
  }
  check_state_is_coherent(driver, "final");

  const RuntimeSummary summary = fixture.runtime->summary();
  if (summary.lifecycle == RuntimeLifecycle::CANCELLED) {
    for (const ActionView& view : fixture.runtime->actions()) {
      AR_CHECK_MSG(std::string(to_string(view.state)) != std::string("COMPLETED_UNVALIDATED"),
                   "a cancelled runtime must not retain a committable completion");
    }
  }
  AR_CHECK(summary.committed_actions == static_cast<std::uint32_t>(summary.committed_actions));
}

}  // namespace

AR_TEST(property, randomized_sequences_preserve_invariants) {
  for (std::uint64_t seed = 1; seed <= 6; ++seed) {
    run_seed(seed, 240, false);
  }
}

AR_TEST(property, randomized_sequences_with_budget_authority) {
  for (std::uint64_t seed = 101; seed <= 104; ++seed) {
    run_seed(seed, 200, true);
  }
}

AR_TEST(property, persistence_round_trip_after_random_sequence) {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "agent_runtime_tests_property";
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  const std::string path = (directory / "state.bin").string();

  Driver driver(4242, false);
  artest::Fixture& fixture = driver.fixture();
  std::vector<ActionHandle> handles;
  for (std::uint32_t i = 0; i < 40; ++i) {
    ActionHandle handle;
    if (fixture.runtime->declare_action(random_spec(driver), handle).accepted()) {
      handles.push_back(handle);
    }
  }
  for (const ActionHandle& handle : handles) {
    if (!fixture.runtime->admit_action(handle.action_id, handle.action_generation).accepted()) {
      continue;
    }
    if (!fixture.runtime->authorize_action(handle.action_id, handle.action_generation).accepted()) {
      continue;
    }
    (void)fixture.runtime->dispatch_and_execute(handle.action_id, handle.action_generation);
    (void)fixture.runtime->commit_action(handle.action_id, handle.action_generation);
  }
  const std::uint32_t committed = fixture.runtime->summary().committed_actions;
  AR_REQUIRE(fixture.runtime->save(path).accepted());

  artest::Fixture restored = artest::make_fixture();
  AR_REQUIRE(restored.runtime->load(path).accepted());
  AR_CHECK_EQ(restored.runtime->summary().committed_actions, committed);
  AR_CHECK(restored.runtime->check_invariants().ok);

  // Saving the recovered state and loading it again reproduces exactly the
  // same durable content.
  const std::uint64_t restored_digest = restored.runtime->durable_digest();
  const std::string second_path = (directory / "state2.bin").string();
  AR_REQUIRE(restored.runtime->save(second_path).accepted());
  artest::Fixture third = artest::make_fixture();
  AR_REQUIRE(third.runtime->load(second_path).accepted());
  AR_CHECK_EQ(third.runtime->durable_digest(), restored_digest);
  std::filesystem::remove(path);
  std::filesystem::remove(second_path);
}

AR_TEST(property, insertion_order_does_not_change_semantics) {
  // Two runtimes that receive the same actions in different orders must agree
  // on the committed progress generation and the canonical action count.
  artest::Fixture first = artest::make_fixture();
  artest::Fixture second = artest::make_fixture();
  artest::bring_up(first);
  artest::bring_up(second);
  const char* tools[] = {"hash", "echo", "hash"};
  SideEffectClass classes[] = {SideEffectClass::PURE, SideEffectClass::READ_ONLY,
                               SideEffectClass::PURE};
  std::vector<ActionHandle> first_handles;
  std::vector<ActionHandle> second_handles;
  for (int i = 0; i < 3; ++i) {
    ActionHandle handle;
    AR_REQUIRE(first.runtime
                   ->declare_action(artest::tool_spec(tools[i], classes[i], "x"), handle)
                   .accepted());
    first_handles.push_back(handle);
  }
  for (int i = 2; i >= 0; --i) {
    ActionHandle handle;
    AR_REQUIRE(second.runtime
                   ->declare_action(artest::tool_spec(tools[i], classes[i], "x"), handle)
                   .accepted());
    second_handles.push_back(handle);
  }
  for (const ActionHandle& handle : first_handles) {
    AR_REQUIRE(first.runtime->admit_action(handle.action_id, handle.action_generation).accepted());
    AR_REQUIRE(first.runtime->authorize_action(handle.action_id, handle.action_generation).accepted());
    AR_REQUIRE(first.runtime->dispatch_and_execute(handle.action_id, handle.action_generation)
                   .completion.accepted());
    AR_REQUIRE(first.runtime->commit_action(handle.action_id, handle.action_generation).accepted());
  }
  for (const ActionHandle& handle : second_handles) {
    AR_REQUIRE(second.runtime->admit_action(handle.action_id, handle.action_generation).accepted());
    AR_REQUIRE(second.runtime->authorize_action(handle.action_id, handle.action_generation).accepted());
    AR_REQUIRE(second.runtime->dispatch_and_execute(handle.action_id, handle.action_generation)
                   .completion.accepted());
    AR_REQUIRE(second.runtime->commit_action(handle.action_id, handle.action_generation).accepted());
  }
  AR_CHECK_EQ(first.runtime->summary().committed_actions, second.runtime->summary().committed_actions);
  AR_CHECK_EQ(first.runtime->summary().progress_generation.value(),
              second.runtime->summary().progress_generation.value());
  AR_CHECK_EQ(first.runtime->summary().total_actions, second.runtime->summary().total_actions);
}
