// Agent Runtime - durable state inspection tool.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "agent_runtime/agent_runtime.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using namespace agent_runtime;

namespace {

struct Options {
  std::string state_path;
  bool validate = false;
  bool summary = false;
  bool run = false;
  bool actions = false;
  bool attempts = false;
  bool bindings = false;
  bool checkpoints = false;
  bool fenced = false;
  bool json = false;
  bool help = false;
  bool version = false;
};

constexpr const char* kUsage =
    "agent_runtime_inspect - inspect an Agent Runtime durable state file\n"
    "\n"
    "Usage:\n"
    "  agent_runtime_inspect --state <file> [options]\n"
    "\n"
    "Options:\n"
    "  --state <file>   durable state file to inspect\n"
    "  --validate       decode and validate the file without printing state\n"
    "  --summary        print the runtime summary\n"
    "  --run            print current run and step identity\n"
    "  --actions        print the action history\n"
    "  --attempts       print the attempt history\n"
    "  --bindings       print model, tool and memory bindings\n"
    "  --checkpoints    print checkpoint bindings\n"
    "  --fenced         print fenced runtime and agent boots\n"
    "  --json           render machine-readable JSON\n"
    "  --version        print the version and exit\n"
    "  --help           print this help and exit\n";

[[nodiscard]] bool parse(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--state" && i + 1 < argc) {
      options.state_path = argv[++i];
    } else if (argument == "--validate") {
      options.validate = true;
    } else if (argument == "--summary") {
      options.summary = true;
    } else if (argument == "--run") {
      options.run = true;
    } else if (argument == "--actions") {
      options.actions = true;
    } else if (argument == "--attempts") {
      options.attempts = true;
    } else if (argument == "--bindings") {
      options.bindings = true;
    } else if (argument == "--checkpoints") {
      options.checkpoints = true;
    } else if (argument == "--fenced") {
      options.fenced = true;
    } else if (argument == "--json") {
      options.json = true;
    } else if (argument == "--help" || argument == "-h") {
      options.help = true;
    } else if (argument == "--version") {
      options.version = true;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", argument.c_str());
      return false;
    }
  }
  return true;
}

void print_actions(const AgentRuntime& runtime, bool json) {
  const std::vector<ActionView> actions = runtime.actions();
  if (json) {
    std::printf("\"actions\":[");
    for (std::size_t i = 0; i < actions.size(); ++i) {
      const ActionView& action = actions[i];
      if (i != 0) {
        std::printf(",");
      }
      std::printf(
          "{\"id\":%llu,\"generation\":%llu,\"kind\":\"%s\",\"state\":\"%s\","
          "\"side_effect\":\"%s\",\"attempts\":%u,\"committed\":%s,\"failure\":\"%s\"}",
          static_cast<unsigned long long>(action.action_id.value()),
          static_cast<unsigned long long>(action.action_generation.value()),
          std::string(to_string(action.kind)).c_str(), std::string(to_string(action.state)).c_str(),
          std::string(to_string(action.side_effect)).c_str(), action.attempt_count,
          action.committed ? "true" : "false",
          std::string(to_string(action.last_failure)).c_str());
    }
    std::printf("]");
    return;
  }
  std::printf("actions=%zu\n", actions.size());
  for (const ActionView& action : actions) {
    std::printf("  %llu/%llu %s %s side_effect=%s attempts=%u committed=%s%s\n",
                static_cast<unsigned long long>(action.action_id.value()),
                static_cast<unsigned long long>(action.action_generation.value()),
                std::string(to_string(action.kind)).c_str(),
                std::string(to_string(action.state)).c_str(),
                std::string(to_string(action.side_effect)).c_str(), action.attempt_count,
                action.committed ? "true" : "false",
                action.last_error.empty() ? "" : (" error=" + action.last_error).c_str());
  }
}

void print_attempts(const AgentRuntime& runtime, bool json) {
  const std::vector<AttemptView> attempts = runtime.attempts();
  if (json) {
    std::printf("\"attempts\":[");
    for (std::size_t i = 0; i < attempts.size(); ++i) {
      const AttemptView& attempt = attempts[i];
      if (i != 0) {
        std::printf(",");
      }
      std::printf(
          "{\"id\":%llu,\"generation\":%llu,\"action\":%llu,\"state\":\"%s\","
          "\"completion\":\"%s\",\"superseded\":%s,\"ambiguous\":%s,\"boot\":%llu}",
          static_cast<unsigned long long>(attempt.attempt_id.value()),
          static_cast<unsigned long long>(attempt.attempt_generation.value()),
          static_cast<unsigned long long>(attempt.action_id.value()),
          std::string(to_string(attempt.state)).c_str(),
          std::string(to_string(attempt.completion_status)).c_str(),
          attempt.superseded ? "true" : "false", attempt.ambiguous ? "true" : "false",
          static_cast<unsigned long long>(attempt.runtime_boot_id.value()));
    }
    std::printf("]");
    return;
  }
  std::printf("attempts=%zu\n", attempts.size());
  for (const AttemptView& attempt : attempts) {
    std::printf("  %llu/%llu action=%llu %s completion=%s boot=%llu superseded=%s ambiguous=%s\n",
                static_cast<unsigned long long>(attempt.attempt_id.value()),
                static_cast<unsigned long long>(attempt.attempt_generation.value()),
                static_cast<unsigned long long>(attempt.action_id.value()),
                std::string(to_string(attempt.state)).c_str(),
                std::string(to_string(attempt.completion_status)).c_str(),
                static_cast<unsigned long long>(attempt.runtime_boot_id.value()),
                attempt.superseded ? "true" : "false", attempt.ambiguous ? "true" : "false");
  }
}

void print_bindings(const RuntimeSnapshot& snapshot, bool json) {
  if (json) {
    std::printf("\"bindings\":{\"model\":[");
    for (std::size_t i = 0; i < snapshot.model_bindings.size(); ++i) {
      const ModelBinding& binding = snapshot.model_bindings[i];
      if (i != 0) {
        std::printf(",");
      }
      std::printf("{\"target\":\"%s\",\"generation\":%llu,\"current\":%s}",
                  binding.target.c_str(),
                  static_cast<unsigned long long>(binding.binding_generation.value()),
                  binding.current ? "true" : "false");
    }
    std::printf("],\"tool\":[");
    for (std::size_t i = 0; i < snapshot.tool_bindings.size(); ++i) {
      const ToolBinding& binding = snapshot.tool_bindings[i];
      if (i != 0) {
        std::printf(",");
      }
      std::printf("{\"name\":\"%s\",\"generation\":%llu,\"side_effect\":\"%s\","
                  "\"current\":%s}",
                  binding.tool_name.c_str(),
                  static_cast<unsigned long long>(binding.binding_generation.value()),
                  std::string(to_string(binding.side_effect)).c_str(),
                  binding.current ? "true" : "false");
    }
    std::printf("],\"memory\":[");
    for (std::size_t i = 0; i < snapshot.memory_bindings.size(); ++i) {
      const MemoryBinding& binding = snapshot.memory_bindings[i];
      if (i != 0) {
        std::printf(",");
      }
      std::printf("{\"identity\":\"%s\",\"generation\":%llu,\"external_generation\":%llu,"
                  "\"fresh\":%s}",
                  binding.state_identity.c_str(),
                  static_cast<unsigned long long>(binding.generation.value()),
                  static_cast<unsigned long long>(binding.external_generation.value()),
                  binding.fresh ? "true" : "false");
    }
    std::printf("]}");
    return;
  }
  std::printf("model_bindings=%zu\n", snapshot.model_bindings.size());
  for (const ModelBinding& binding : snapshot.model_bindings) {
    std::printf("  model %s generation=%llu current=%s\n", binding.target.c_str(),
                static_cast<unsigned long long>(binding.binding_generation.value()),
                binding.current ? "true" : "false");
  }
  std::printf("tool_bindings=%zu\n", snapshot.tool_bindings.size());
  for (const ToolBinding& binding : snapshot.tool_bindings) {
    std::printf("  tool %s generation=%llu side_effect=%s current=%s\n", binding.tool_name.c_str(),
                static_cast<unsigned long long>(binding.binding_generation.value()),
                std::string(to_string(binding.side_effect)).c_str(),
                binding.current ? "true" : "false");
  }
  std::printf("memory_bindings=%zu\n", snapshot.memory_bindings.size());
  for (const MemoryBinding& binding : snapshot.memory_bindings) {
    std::printf("  memory %s generation=%llu external=%llu fresh=%s\n",
                binding.state_identity.c_str(),
                static_cast<unsigned long long>(binding.generation.value()),
                static_cast<unsigned long long>(binding.external_generation.value()),
                binding.fresh ? "true" : "false");
  }
}

void print_checkpoints(const RuntimeSnapshot& snapshot, bool json) {
  if (json) {
    std::printf("\"checkpoints\":[");
    for (std::size_t i = 0; i < snapshot.checkpoints.size(); ++i) {
      const CheckpointView& checkpoint = snapshot.checkpoints[i];
      if (i != 0) {
        std::printf(",");
      }
      std::printf("{\"binding\":%llu,\"generation\":%llu,\"state\":\"%s\","
                  "\"restorable\":%s,\"current\":%s}",
                  static_cast<unsigned long long>(checkpoint.binding_id.value()),
                  static_cast<unsigned long long>(checkpoint.generation.value()),
                  std::string(to_string(checkpoint.state)).c_str(),
                  checkpoint.restorable ? "true" : "false",
                  checkpoint.current ? "true" : "false");
    }
    std::printf("]");
    return;
  }
  std::printf("checkpoints=%zu\n", snapshot.checkpoints.size());
  for (const CheckpointView& checkpoint : snapshot.checkpoints) {
    std::printf("  %llu/%llu %s restorable=%s current=%s\n",
                static_cast<unsigned long long>(checkpoint.binding_id.value()),
                static_cast<unsigned long long>(checkpoint.generation.value()),
                std::string(to_string(checkpoint.state)).c_str(),
                checkpoint.restorable ? "true" : "false",
                checkpoint.current ? "true" : "false");
  }
}

void print_fenced(const RuntimeSnapshot& snapshot, bool json) {
  if (json) {
    std::printf("\"fenced_runtime_boots\":[");
    for (std::size_t i = 0; i < snapshot.fenced_runtime_boots.size(); ++i) {
      if (i != 0) {
        std::printf(",");
      }
      std::printf("%llu", static_cast<unsigned long long>(snapshot.fenced_runtime_boots[i].value()));
    }
    std::printf("],\"fenced_agent_boots\":[");
    for (std::size_t i = 0; i < snapshot.fenced_agent_boots.size(); ++i) {
      if (i != 0) {
        std::printf(",");
      }
      std::printf("%llu", static_cast<unsigned long long>(snapshot.fenced_agent_boots[i].value()));
    }
    std::printf("]");
    return;
  }
  std::printf("fenced_runtime_boots=%zu fenced_agent_boots=%zu\n",
              snapshot.fenced_runtime_boots.size(), snapshot.fenced_agent_boots.size());
  for (std::size_t i = 0; i < snapshot.fenced_runtime_boots.size(); ++i) {
    std::printf("  runtime_boot=%llu agent_boot=%llu\n",
                static_cast<unsigned long long>(snapshot.fenced_runtime_boots[i].value()),
                i < snapshot.fenced_agent_boots.size()
                    ? static_cast<unsigned long long>(snapshot.fenced_agent_boots[i].value())
                    : 0ull);
  }
}

void print_summary(const RuntimeSummary& summary, bool json) {
  if (json) {
    std::printf(
        "\"summary\":{\"runtime_id\":%llu,\"runtime_generation\":%llu,"
        "\"runtime_boot\":%llu,\"runtime_epoch\":%llu,\"coordinator_epoch\":%llu,"
        "\"agent_id\":%llu,\"agent_boot\":%llu,\"lifecycle\":\"%s\","
        "\"progress\":\"%s\",\"progress_generation\":%llu,\"cancellation\":\"%s\","
        "\"recovery\":\"%s\",\"actions\":%u,\"committed\":%u,\"attempts\":%u,"
        "\"ambiguous\":%u,\"revalidation\":%u,\"fenced_boots\":%u}",
        static_cast<unsigned long long>(summary.runtime_id.value()),
        static_cast<unsigned long long>(summary.runtime_generation.value()),
        static_cast<unsigned long long>(summary.runtime_boot_id.value()),
        static_cast<unsigned long long>(summary.runtime_epoch.value()),
        static_cast<unsigned long long>(summary.coordinator_epoch.value()),
        static_cast<unsigned long long>(summary.agent_id.value()),
        static_cast<unsigned long long>(summary.agent_boot_id.value()),
        std::string(to_string(summary.lifecycle)).c_str(),
        std::string(to_string(summary.progress)).c_str(),
        static_cast<unsigned long long>(summary.progress_generation.value()),
        std::string(to_string(summary.cancellation)).c_str(),
        std::string(to_string(summary.recovery)).c_str(), summary.total_actions,
        summary.committed_actions, summary.total_attempts, summary.ambiguous_actions,
        summary.revalidation_actions, summary.fenced_boots);
    return;
  }
  std::printf("runtime=%llu generation=%llu boot=%llu epoch=%llu coordinator_epoch=%llu\n",
              static_cast<unsigned long long>(summary.runtime_id.value()),
              static_cast<unsigned long long>(summary.runtime_generation.value()),
              static_cast<unsigned long long>(summary.runtime_boot_id.value()),
              static_cast<unsigned long long>(summary.runtime_epoch.value()),
              static_cast<unsigned long long>(summary.coordinator_epoch.value()));
  std::printf("agent=%llu generation=%llu boot=%llu\n",
              static_cast<unsigned long long>(summary.agent_id.value()),
              static_cast<unsigned long long>(summary.agent_generation.value()),
              static_cast<unsigned long long>(summary.agent_boot_id.value()));
  std::printf("lifecycle=%s progress=%s/%llu cancellation=%s recovery=%s\n",
              std::string(to_string(summary.lifecycle)).c_str(),
              std::string(to_string(summary.progress)).c_str(),
              static_cast<unsigned long long>(summary.progress_generation.value()),
              std::string(to_string(summary.cancellation)).c_str(),
              std::string(to_string(summary.recovery)).c_str());
  std::printf("actions=%u committed=%u attempts=%u ambiguous=%u revalidation=%u fenced_boots=%u\n",
              summary.total_actions, summary.committed_actions, summary.total_attempts,
              summary.ambiguous_actions, summary.revalidation_actions, summary.fenced_boots);
  std::printf("validity=%s\n", std::string(to_string(summary.validity)).c_str());
  if (!summary.terminal_reason.empty()) {
    std::printf("terminal_reason=%s\n", summary.terminal_reason.c_str());
  }
}

void print_run(const RuntimeSummary& summary, bool json) {
  if (json) {
    std::printf(
        "\"run\":{\"id\":%llu,\"generation\":%llu,\"step\":%llu,"
        "\"step_generation\":%llu,\"progress_generation\":%llu}",
        static_cast<unsigned long long>(summary.run_id.value()),
        static_cast<unsigned long long>(summary.run_generation.value()),
        static_cast<unsigned long long>(summary.step_id.value()),
        static_cast<unsigned long long>(summary.step_generation.value()),
        static_cast<unsigned long long>(summary.progress_generation.value()));
    return;
  }
  std::printf("run=%llu/%llu step=%llu/%llu progress_generation=%llu\n",
              static_cast<unsigned long long>(summary.run_id.value()),
              static_cast<unsigned long long>(summary.run_generation.value()),
              static_cast<unsigned long long>(summary.step_id.value()),
              static_cast<unsigned long long>(summary.step_generation.value()),
              static_cast<unsigned long long>(summary.progress_generation.value()));
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
  Options options;
  if (!parse(argc, argv, options)) {
    std::fprintf(stderr, "\n%s", kUsage);
    return 2;
  }
  if (options.help) {
    std::printf("%s", kUsage);
    return 0;
  }
  if (options.version) {
    std::printf("%s\n", product_string().c_str());
    std::printf("persistence format version %u\n",
                static_cast<unsigned>(supported_persistence_format_version()));
    return 0;
  }
  if (options.state_path.empty()) {
    std::fprintf(stderr, "error: --state <file> is required\n\n%s", kUsage);
    return 2;
  }

  const PersistenceProbe probe = persistence_inspect(options.state_path);
  if (!probe.valid()) {
    std::fprintf(stderr, "validation failed: %s\n", probe.explanation.to_text().c_str());
    return 1;
  }
  if (options.validate) {
    std::printf("valid: format_version=%u records=%u payload_bytes=%llu digest=%s\n",
                static_cast<unsigned>(probe.info.format_version), probe.info.record_count,
                static_cast<unsigned long long>(probe.info.payload_bytes),
                probe.info.semantic_digest_hex.c_str());
    return 0;
  }

  MutationResult opened;
  std::unique_ptr<AgentRuntime> runtime = AgentRuntime::open_durable_state(options.state_path, opened);
  if (runtime == nullptr) {
    std::fprintf(stderr, "open failed: %s\n", opened.to_text().c_str());
    return 1;
  }
  const RuntimeSummary summary = runtime->summary();
  const RuntimeSnapshot snapshot = runtime->snapshot();
  const bool any_section = options.summary || options.run || options.actions || options.attempts ||
                           options.bindings || options.checkpoints || options.fenced;
  const bool print_all = !any_section;

  if (options.json) {
    std::printf("{");
    bool first = true;
    auto separator = [&first] {
      if (!first) {
        std::printf(",");
      }
      first = false;
    };
    if (print_all || options.summary) {
      separator();
      print_summary(summary, true);
    }
    if (print_all || options.run) {
      separator();
      print_run(summary, true);
    }
    if (print_all || options.actions) {
      separator();
      print_actions(*runtime, true);
    }
    if (print_all || options.attempts) {
      separator();
      print_attempts(*runtime, true);
    }
    if (print_all || options.bindings) {
      separator();
      print_bindings(snapshot, true);
    }
    if (print_all || options.checkpoints) {
      separator();
      print_checkpoints(snapshot, true);
    }
    if (print_all || options.fenced) {
      separator();
      print_fenced(snapshot, true);
    }
    std::printf("}\n");
    return 0;
  }

  if (print_all || options.summary) {
    print_summary(summary, false);
  }
  if (print_all || options.run) {
    print_run(summary, false);
  }
  if (print_all || options.actions) {
    print_actions(*runtime, false);
  }
  if (print_all || options.attempts) {
    print_attempts(*runtime, false);
  }
  if (print_all || options.bindings) {
    print_bindings(snapshot, false);
  }
  if (print_all || options.checkpoints) {
    print_checkpoints(snapshot, false);
  }
  if (print_all || options.fenced) {
    print_fenced(snapshot, false);
  }
  const InvariantReport report = runtime->check_invariants();
  std::printf("invariants: %s checks=%u violations=%zu\n", report.ok ? "OK" : "VIOLATED",
              report.checks_run, report.violations.size());
  return report.ok ? 0 : 1;
}
