// Agent Runtime - completed-operation benchmarks.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// Every measurement covers a completed runtime operation, not an enqueue. The
// action lifecycle benchmarks include real invocation of the deterministic
// reference backends; they are not model inference throughput and must never be
// presented as such. Persistence benchmarks include the normal durability work
// (flush plus atomic replacement) performed by a production save.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "agent_runtime/agent_runtime.hpp"

using namespace agent_runtime;

namespace {

using Clock = std::chrono::steady_clock;

struct Measurement {
  std::string name;
  std::uint64_t operations = 0;
  double millis = 0.0;

  [[nodiscard]] double micros_per_operation() const {
    return operations == 0 ? 0.0 : (millis * 1000.0) / static_cast<double>(operations);
  }
  [[nodiscard]] double operations_per_second() const {
    return millis == 0.0 ? 0.0 : (static_cast<double>(operations) * 1000.0) / millis;
  }
};

class Stopwatch {
 public:
  Stopwatch() : start_(Clock::now()) {}
  [[nodiscard]] double millis() const {
    return std::chrono::duration<double, std::milli>(Clock::now() - start_).count();
  }

 private:
  Clock::time_point start_;
};

[[nodiscard]] ResourceLimits benchmark_limits() {
  ResourceLimits limits;
  limits.max_actions_per_step = 1000000;
  limits.max_actions_per_run = 1000000;
  limits.max_steps_per_run = 1000000;
  limits.max_retained_history = 2000000;
  return limits;
}

[[nodiscard]] std::string scratch_directory() {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "agent_runtime_benchmarks";
  std::error_code error;
  std::filesystem::create_directories(path, error);
  return path.string();
}

struct Harness {
  std::shared_ptr<LogicalClock> clock = std::make_shared<LogicalClock>();
  std::shared_ptr<ReferenceToolBackend> tool =
      std::make_shared<ReferenceToolBackend>("benchmark-tool", 1);
  std::shared_ptr<ReferenceModelBackend> model =
      std::make_shared<ReferenceModelBackend>("benchmark-model", 1);

  [[nodiscard]] std::unique_ptr<AgentRuntime> make_runtime(AgentId agent_id) {
    AgentRuntimeOptions options;
    options.runtime_id = allocate_id<AgentRuntimeTag>();
    options.agent_id = agent_id;
    options.clock = clock;
    options.limits = benchmark_limits();
    options.policy = RuntimePolicy::permissive();
    options.policy.require_assignment_for_dispatch = false;
    options.tool_backend = tool;
    options.model_backend = model;
    auto runtime = std::make_unique<AgentRuntime>(std::move(options));
    (void)runtime->initialize();
    ToolBinding tool_binding;
    tool_binding.tool_name = "hash";
    tool_binding.binding_generation = ToolCallGeneration(1);
    tool_binding.backend_id = tool->incarnation().backend_id;
    tool_binding.backend_generation = tool->incarnation().generation;
    tool_binding.side_effect = SideEffectClass::PURE;
    tool_binding.current = true;
    (void)runtime->bind_tool(tool_binding);
    ModelBinding model_binding;
    model_binding.target = "reference-target";
    model_binding.binding_generation = ModelCallGeneration(1);
    model_binding.backend_id = model->incarnation().backend_id;
    model_binding.backend_generation = model->incarnation().generation;
    model_binding.current = true;
    (void)runtime->bind_model(model_binding);
    (void)runtime->start_run(AgentRunId(1), AgentRunGeneration(1));
    (void)runtime->begin_step();
    return runtime;
  }
};

[[nodiscard]] ActionSpec hash_spec(std::uint64_t index) {
  ActionSpec spec;
  spec.kind = ActionKind::TOOL_CALL;
  spec.tool_name = "hash";
  spec.side_effect = SideEffectClass::PURE;
  spec.input = "payload-" + std::to_string(index);
  spec.max_output_bytes = 4096;
  return spec;
}

void report(const Measurement& measurement) {
  std::printf("  %-34s ops=%-8llu total=%-10.2fms  %-10.2f us/op  %12.0f ops/s\n",
              measurement.name.c_str(),
              static_cast<unsigned long long>(measurement.operations), measurement.millis,
              measurement.micros_per_operation(), measurement.operations_per_second());
}

/// Benchmarks run creation: each runtime is created, initialized, given a run
/// and one open step.
void benchmark_runs(std::uint64_t count) {
  Harness harness;
  std::vector<std::unique_ptr<AgentRuntime>> runtimes;
  runtimes.reserve(static_cast<std::size_t>(count));
  Stopwatch stopwatch;
  for (std::uint64_t i = 0; i < count; ++i) {
    runtimes.push_back(harness.make_runtime(AgentId(1000 + i)));
  }
  Measurement measurement{"run creation (runtime+run+step)", count, stopwatch.millis()};
  report(measurement);
  bool ok = true;
  for (const std::unique_ptr<AgentRuntime>& runtime : runtimes) {
    const RuntimeSummary summary = runtime->summary();
    ok = ok && summary.total_actions == 0 && summary.run_id.value() == 1;
  }
  std::printf("    correctness guard: %s\n", ok ? "OK" : "FAILED");
}

/// Benchmarks the full action lifecycle including reference backend invocation.
///
/// Declaration is measured over a large population, then each action is driven
/// through admission, authorization, dispatch, completion and commit one at a
/// time so that the measured work is the completed operation itself.
void benchmark_action_lifecycle(std::uint64_t count, bool model_calls) {
  Harness harness;
  auto runtime = harness.make_runtime(AgentId(2000));
  std::vector<ActionHandle> handles;
  handles.reserve(static_cast<std::size_t>(count));

  Stopwatch declare_watch;
  for (std::uint64_t i = 0; i < count; ++i) {
    ActionHandle handle;
    if (model_calls) {
      ActionSpec spec;
      spec.kind = ActionKind::MODEL_CALL;
      spec.model_target = "reference-target";
      spec.side_effect = SideEffectClass::PURE;
      spec.input = "prompt-" + std::to_string(i);
      if (!runtime->declare_action(spec, handle).accepted()) {
        break;
      }
    } else if (!runtime->declare_action(hash_spec(i), handle).accepted()) {
      break;
    }
    handles.push_back(handle);
  }
  report(Measurement{"action declaration", handles.size(), declare_watch.millis()});

  double admit_ms = 0.0;
  double authorize_ms = 0.0;
  double dispatch_ms = 0.0;
  double commit_ms = 0.0;
  std::uint64_t admitted = 0;
  std::uint64_t authorized = 0;
  std::uint64_t dispatched = 0;
  std::uint64_t committed = 0;
  for (const ActionHandle& handle : handles) {
    {
      Stopwatch watch;
      const bool ok = runtime->admit_action(handle.action_id, handle.action_generation).accepted();
      admit_ms += watch.millis();
      admitted += ok ? 1 : 0;
      if (!ok) {
        continue;
      }
    }
    {
      Stopwatch watch;
      const bool ok =
          runtime->authorize_action(handle.action_id, handle.action_generation).accepted();
      authorize_ms += watch.millis();
      authorized += ok ? 1 : 0;
      if (!ok) {
        continue;
      }
    }
    LocalExecutionResult execution;
    {
      Stopwatch watch;
      execution = runtime->dispatch_and_execute(handle.action_id, handle.action_generation);
      dispatch_ms += watch.millis();
      dispatched += execution.completion.accepted() ? 1 : 0;
      if (!execution.completion.accepted()) {
        continue;
      }
    }
    {
      Stopwatch watch;
      if (runtime->commit_action(handle.action_id, handle.action_generation).accepted()) {
        ++committed;
      }
      commit_ms += watch.millis();
    }
  }
  report(Measurement{"action admission", admitted, admit_ms});
  report(Measurement{"action authorization", authorized, authorize_ms});
  report(Measurement{model_calls ? "model dispatch+completion" : "tool dispatch+completion",
                     dispatched, dispatch_ms});
  report(Measurement{"completion validation+progress commit", committed, commit_ms});

  Stopwatch snapshot_watch;
  const RuntimeSnapshot snapshot = runtime->snapshot();
  report(Measurement{"snapshot", snapshot.actions.size(), snapshot_watch.millis()});

  Stopwatch invariant_watch;
  const InvariantReport report_result = runtime->check_invariants();
  report(Measurement{"invariant check", snapshot.actions.size(), invariant_watch.millis()});

  const RuntimeSummary summary = runtime->summary();
  std::printf("    correctness guard: %s (committed=%u expected=%llu invariants=%s)\n",
              (summary.committed_actions == committed && report_result.ok) ? "OK" : "FAILED",
              summary.committed_actions, static_cast<unsigned long long>(committed),
              report_result.ok ? "OK" : "VIOLATED");
}

/// Benchmarks retry classification: each action fails once and is retried with
/// a fresh attempt generation, then commits.
void benchmark_retry(std::uint64_t count) {
  Harness harness;
  auto runtime = harness.make_runtime(AgentId(3000));
  std::uint64_t retries = 0;
  std::uint64_t committed = 0;
  Stopwatch stopwatch;
  for (std::uint64_t i = 0; i < count; ++i) {
    ActionHandle handle;
    if (!runtime->declare_action(hash_spec(i), handle).accepted()) {
      break;
    }
    (void)runtime->admit_action(handle.action_id, handle.action_generation);
    (void)runtime->authorize_action(handle.action_id, handle.action_generation);
    harness.tool->fail_next(RetryClass::TRANSIENT, "benchmark injected failure");
    const LocalExecutionResult first =
        runtime->dispatch_and_execute(handle.action_id, handle.action_generation);
    if (std::string(to_string(first.completion.code)) == std::string("ACCEPTED")) {
      ++retries;
    }
    (void)runtime->authorize_action(handle.action_id, handle.action_generation);
    (void)runtime->dispatch_and_execute(handle.action_id, handle.action_generation);
    if (runtime->commit_action(handle.action_id, handle.action_generation).accepted()) {
      ++committed;
    }
  }
  Measurement measurement{"retry classification+commit", count, stopwatch.millis()};
  report(measurement);
  std::printf("    correctness guard: %s (retries=%llu committed=%llu)\n",
              (retries == count && committed == count) ? "OK" : "FAILED",
              static_cast<unsigned long long>(retries),
              static_cast<unsigned long long>(committed));
}

void benchmark_persistence(std::uint64_t count) {
  Harness harness;
  auto runtime = harness.make_runtime(AgentId(4000));
  for (std::uint64_t i = 0; i < count; ++i) {
    ActionHandle handle;
    if (!runtime->declare_action(hash_spec(i), handle).accepted()) {
      break;
    }
    (void)runtime->admit_action(handle.action_id, handle.action_generation);
    (void)runtime->authorize_action(handle.action_id, handle.action_generation);
    (void)runtime->dispatch_and_execute(handle.action_id, handle.action_generation);
    (void)runtime->commit_action(handle.action_id, handle.action_generation);
  }
  const std::string path = scratch_directory() + "/benchmark.state";
  Stopwatch save_watch;
  const MutationResult saved = runtime->save(path);
  Measurement save_measurement{"persistence save (durable)", 1, save_watch.millis()};
  report(save_measurement);

  auto restored = harness.make_runtime(AgentId(4001));
  restored.reset();
  auto loaded = harness.make_runtime(AgentId(4000));
  loaded.reset();

  MutationResult load_result;
  Stopwatch load_watch;
  std::unique_ptr<AgentRuntime> opened = AgentRuntime::open_durable_state(path, load_result);
  Measurement load_measurement{"persistence open+decode+validate", 1, load_watch.millis()};
  report(load_measurement);

  const std::uint64_t bytes = std::filesystem::file_size(path);
  std::printf("    durable bytes=%llu committed_actions=%u guard: %s\n",
              static_cast<unsigned long long>(bytes), runtime->summary().committed_actions,
              (saved.accepted() && opened != nullptr &&
               opened->summary().committed_actions == runtime->summary().committed_actions)
                  ? "OK"
                  : "FAILED");
  std::error_code error;
  std::filesystem::remove(path, error);
}

void benchmark_recovery(std::uint64_t count) {
  Harness harness;
  auto runtime = harness.make_runtime(AgentId(5000));
  for (std::uint64_t i = 0; i < count; ++i) {
    ActionHandle handle;
    if (!runtime->declare_action(hash_spec(i), handle).accepted()) {
      break;
    }
    (void)runtime->admit_action(handle.action_id, handle.action_generation);
    (void)runtime->authorize_action(handle.action_id, handle.action_generation);
    (void)runtime->dispatch_and_execute(handle.action_id, handle.action_generation);
    (void)runtime->commit_action(handle.action_id, handle.action_generation);
  }
  Stopwatch stopwatch;
  RecoveryContext context;
  context.runtime_boot_id = RuntimeBootId(2);
  context.agent_boot_id = AgentBootId(2);
  context.runtime_epoch = RuntimeEpoch(2);
  context.coordinator_epoch = CoordinatorEpoch(2);
  const MutationResult recovered = runtime->recover(context);
  Measurement measurement{"conservative recovery", 1, stopwatch.millis()};
  report(measurement);
  std::printf("    correctness guard: %s (recovery=%s lifecycle=%s)\n",
              (recovered.accepted() &&
               std::string(to_string(runtime->lifecycle())) ==
                   std::string("REVALIDATION_REQUIRED"))
                  ? "OK"
                  : "FAILED",
              std::string(to_string(recovered.code)).c_str(),
              std::string(to_string(runtime->lifecycle())).c_str());
}

}  // namespace

int main(int argc, char** argv) {
  bool quick = false;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--quick") {
      quick = true;
    }
  }
  const std::uint64_t run_scales[] = {100, 1000, 10000};
  const std::uint64_t action_scales[] = {1000, 10000, 100000};

  std::printf("%s benchmarks\n", product_string().c_str());
  std::printf("measurements cover completed runtime operations only; action lifecycles include\n");
  std::printf("invocation of the deterministic reference backends and are not model inference.\n\n");

  for (const std::uint64_t scale : run_scales) {
    std::printf("runs scale=%llu\n", static_cast<unsigned long long>(scale));
    benchmark_runs(scale);
    std::fflush(stdout);
  }

  for (const std::uint64_t scale : action_scales) {
    if (quick && scale > 10000) {
      continue;
    }
    std::printf("\ntool action lifecycle scale=%llu\n", static_cast<unsigned long long>(scale));
    benchmark_action_lifecycle(scale, false);
    std::fflush(stdout);
    std::printf("\nmodel action lifecycle scale=%llu\n", static_cast<unsigned long long>(scale));
    benchmark_action_lifecycle(scale, true);
    std::fflush(stdout);
  }

  for (const std::uint64_t scale : action_scales) {
    if (quick && scale > 10000) {
      continue;
    }
    std::printf("\nretry scale=%llu\n", static_cast<unsigned long long>(scale));
    benchmark_retry(scale);
    std::fflush(stdout);
    std::printf("\npersistence scale=%llu\n", static_cast<unsigned long long>(scale));
    benchmark_persistence(scale);
    std::fflush(stdout);
    std::printf("\nrecovery scale=%llu\n", static_cast<unsigned long long>(scale));
    benchmark_recovery(scale);
    std::fflush(stdout);
  }
  std::error_code error;
  std::filesystem::remove_all(scratch_directory(), error);
  return 0;
}
