// Agent Runtime - replaceable model and tool backend interfaces.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef AGENT_RUNTIME_BACKENDS_HPP
#define AGENT_RUNTIME_BACKENDS_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "agent_runtime/ids.hpp"
#include "agent_runtime/lifecycle.hpp"

namespace agent_runtime {

/// Cooperative cancellation probe handed to a backend. A backend may observe
/// cancellation, but Agent Runtime never claims physical cancellation of an
/// external operation unless the backend proves it.
class CancellationProbe {
 public:
  CancellationProbe() = default;
  explicit CancellationProbe(std::shared_ptr<std::atomic<bool>> flag)
      : flag_(std::move(flag)) {}

  [[nodiscard]] bool cancelled() const noexcept {
    return flag_ != nullptr && flag_->load(std::memory_order_acquire);
  }
  [[nodiscard]] bool observable() const noexcept { return flag_ != nullptr; }
  void request() noexcept {
    if (flag_ != nullptr) {
      flag_->store(true, std::memory_order_release);
    }
  }

 private:
  std::shared_ptr<std::atomic<bool>> flag_;
};

/// Immutable identity of a backend incarnation. A process-bound backend
/// (for example a tool worker process) must supply a fresh generation whenever
/// it restarts; a restart of one backend never invalidates another.
struct BackendIncarnation {
  BackendId backend_id{};
  Generation<BackendTag> generation{};
  std::string name;
  bool available = false;

  [[nodiscard]] bool valid() const noexcept {
    return backend_id.valid() && generation.valid() && available;
  }
};

struct ModelRequest {
  ModelCallId call_id{};
  ModelCallGeneration call_generation{};
  ActionId action_id{};
  ActionGeneration action_generation{};
  ActionAttemptId attempt_id{};
  AttemptGeneration attempt_generation{};
  AgentRuntimeId runtime_id{};
  RuntimeBootId runtime_boot_id{};
  std::string target;
  std::string prompt;
  std::uint32_t max_output_bytes = 4096;
  std::uint64_t request_digest = 0;
  std::string context_identity;
  MemoryBindingGeneration context_generation{};
  std::uint64_t budget_actions_remaining = 0;
};

struct ModelResponse {
  ResultId result_id{};
  CompletionGeneration completion_generation{};
  ActionId action_id{};
  ModelCallId call_id{};
  ModelCallGeneration call_generation{};
  ActionAttemptId attempt_id{};
  AttemptGeneration attempt_generation{};
  CompletionStatus status = CompletionStatus::UNKNOWN;
  RetryClass failure_class = RetryClass::NONE;
  std::string output;
  std::uint64_t output_digest = 0;
  std::uint64_t prompt_tokens = 0;
  std::uint64_t output_tokens = 0;
  std::string error;
  std::string backend_name;
  Generation<BackendTag> backend_generation{};
};

/// Replaceable model backend. Agent Runtime consumes a resolved target; it does
/// not rank providers or choose models.
class ModelBackend {
 public:
  virtual ~ModelBackend() = default;

  [[nodiscard]] virtual BackendIncarnation incarnation() const = 0;
  [[nodiscard]] virtual ModelResponse invoke(const ModelRequest& request,
                                             const CancellationProbe& cancellation) = 0;
};

struct ToolRequest {
  ToolCallId call_id{};
  ToolCallGeneration call_generation{};
  ActionId action_id{};
  ActionGeneration action_generation{};
  ActionAttemptId attempt_id{};
  AttemptGeneration attempt_generation{};
  AgentRuntimeId runtime_id{};
  RuntimeBootId runtime_boot_id{};
  std::string tool_name;
  SideEffectClass side_effect = SideEffectClass::UNKNOWN;
  std::string operation_key;
  std::string input;
  std::uint64_t input_digest = 0;
  std::uint32_t max_output_bytes = 65536;
};

struct ToolResponse {
  ResultId result_id{};
  CompletionGeneration completion_generation{};
  ActionId action_id{};
  ToolCallId call_id{};
  ToolCallGeneration call_generation{};
  ActionAttemptId attempt_id{};
  AttemptGeneration attempt_generation{};
  CompletionStatus status = CompletionStatus::UNKNOWN;
  RetryClass failure_class = RetryClass::NONE;
  std::string output;
  std::uint64_t result_digest = 0;
  std::string error;
  std::string backend_name;
  Generation<BackendTag> backend_generation{};
  /// Set when the backend deduplicated the operation on a stable operation key
  /// and returned the previously committed receipt.
  bool deduplicated = false;
  /// Set when a commit-token protocol reached the commit step.
  bool commit_receipt = false;
};

/// Replaceable tool backend.
class ToolBackend {
 public:
  virtual ~ToolBackend() = default;

  [[nodiscard]] virtual BackendIncarnation incarnation() const = 0;
  [[nodiscard]] virtual ToolResponse invoke(const ToolRequest& request,
                                            const CancellationProbe& cancellation) = 0;
};

/// In-process deterministic reference backends. These are reference
/// implementations for tests and examples: they are deterministic, offline and
/// do not call any external model provider.
class ReferenceModelBackend final : public ModelBackend {
 public:
  explicit ReferenceModelBackend(std::string name = "reference-model",
                                 std::uint64_t generation = 1);
  ~ReferenceModelBackend() override;

  [[nodiscard]] BackendIncarnation incarnation() const override;
  [[nodiscard]] ModelResponse invoke(const ModelRequest& request,
                                     const CancellationProbe& cancellation) override;

  /// Test hooks: force the next invocation to fail with the given class.
  void fail_next(RetryClass failure_class, std::string error);
  void set_latency_logical_millis(std::uint64_t millis);
  /// Number of physical invocations performed. Used to prove that a retry
  /// performs a new physical call while committing progress exactly once.
  [[nodiscard]] std::uint64_t invocation_count() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class ReferenceToolBackend final : public ToolBackend {
 public:
  explicit ReferenceToolBackend(std::string name = "reference-tool",
                                std::uint64_t generation = 1);
  ~ReferenceToolBackend() override;

  [[nodiscard]] BackendIncarnation incarnation() const override;
  [[nodiscard]] ToolResponse invoke(const ToolRequest& request,
                                    const CancellationProbe& cancellation) override;

  void fail_next(RetryClass failure_class, std::string error);
  /// Models the outcome of a crash after the physical side effect but before a
  /// response could be delivered: the side effect is applied to the journal and
  /// the completion is reported as AMBIGUOUS. The runtime must not assume the
  /// side effect did or did not happen. This is the mechanism behind the
  /// in-process ambiguous-completion proof; the multiprocess proof uses a real
  /// killed tool worker process instead.
  void crash_after_side_effect_once();
  [[nodiscard]] std::uint64_t invocation_count() const noexcept;
  /// Number of committed side-effect receipts recorded in the journal.
  [[nodiscard]] std::uint64_t receipt_count() const noexcept;
  [[nodiscard]] bool has_receipt(std::string_view operation_key) const;
  void set_journal_directory(std::string directory);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace agent_runtime

#endif  // AGENT_RUNTIME_BACKENDS_HPP
