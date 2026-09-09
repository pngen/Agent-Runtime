// Agent Runtime - structured deterministic outcomes and explanations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef AGENT_RUNTIME_OUTCOME_HPP
#define AGENT_RUNTIME_OUTCOME_HPP

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "agent_runtime/ids.hpp"

namespace agent_runtime {

/// Deterministic outcome of a runtime mutation. No meaningful mutation returns a
/// bare boolean: every mutation names exactly why it did or did not take effect.
enum class OutcomeCode : std::uint16_t {
  ACCEPTED = 0,
  NO_CHANGE,
  DEFERRED,
  REVALIDATION_REQUIRED,

  REJECT_STALE_RUNTIME_EPOCH,
  REJECT_STALE_RUNTIME_BOOT,
  REJECT_STALE_AGENT_BOOT,
  REJECT_STALE_RUN_GENERATION,
  REJECT_STALE_STEP,
  REJECT_STALE_ACTION,
  REJECT_STALE_ATTEMPT,
  REJECT_STALE_ASSIGNMENT,
  REJECT_STALE_MODEL_BINDING,
  REJECT_STALE_TOOL_BINDING,
  REJECT_STALE_MEMORY_BINDING,
  REJECT_STALE_CHECKPOINT,
  REJECT_STALE_POLICY,
  REJECT_STALE_BUDGET,

  REJECT_CANCELLED,
  REJECT_COMPLETED,
  REJECT_RETIRED,
  REJECT_NOT_READY,
  REJECT_NOT_RESUMABLE,
  REJECT_SIDE_EFFECT_UNSAFE,
  REJECT_RETRY_EXHAUSTED,
  REJECT_BUDGET,
  REJECT_POLICY,
  REJECT_CONFLICT,
  REJECT_INVALID,
  REJECT_LIMIT,

  AMBIGUOUS_COMPLETION,
  MANUAL_OR_EXTERNAL_RECONCILIATION_REQUIRED,

  SHUTTING_DOWN,
  INTERNAL_ERROR,

  kCount
};

[[nodiscard]] std::string_view to_string(OutcomeCode code) noexcept;

/// True when the outcome means the requested effect took place (or was already
/// exactly in the requested state).
[[nodiscard]] bool is_acceptance(OutcomeCode code) noexcept;
/// True when the outcome rejected the request without changing canonical state.
[[nodiscard]] bool is_rejection(OutcomeCode code) noexcept;
/// True when the caller must revalidate authority before retrying the request.
[[nodiscard]] bool requires_revalidation(OutcomeCode code) noexcept;
/// True when the outcome leaves an external side effect in an unknown state.
[[nodiscard]] bool is_ambiguous(OutcomeCode code) noexcept;

/// One deterministic explanation factor. Factors are always emitted in
/// canonical (key-ascending) order so that explanations are byte-stable.
struct ExplanationFactor {
  std::string key;
  std::string value;

  friend bool operator==(const ExplanationFactor&, const ExplanationFactor&) = default;
};

/// A deterministic, canonically ordered explanation of an outcome.
class Explanation {
 public:
  Explanation() = default;
  Explanation(OutcomeCode code, std::string subject)
      : code_(code), subject_(std::move(subject)) {}

  [[nodiscard]] OutcomeCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string& subject() const noexcept { return subject_; }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }
  [[nodiscard]] const std::vector<ExplanationFactor>& factors() const noexcept { return factors_; }

  [[nodiscard]] const std::string* find(std::string_view key) const noexcept;

  /// Canonical single-line rendering: "code subject {k=v,...} message".
  [[nodiscard]] std::string to_text() const;
  /// Canonical JSON object rendering with factors in key order.
  [[nodiscard]] std::string to_json() const;

  void set_message(std::string message) { message_ = std::move(message); }
  void add_factor(std::string key, std::string value);

 private:
  OutcomeCode code_{OutcomeCode::INTERNAL_ERROR};
  std::string subject_;
  std::string message_;
  std::vector<ExplanationFactor> factors_;
};

/// Builder used internally and by callers to assemble explanations.
class ExplanationBuilder {
 public:
  ExplanationBuilder(OutcomeCode code, std::string subject)
      : code_(code), subject_(std::move(subject)) {}

  ExplanationBuilder& add(std::string key, std::string value) {
    factors_.emplace_back(std::move(key), std::move(value));
    return *this;
  }
  ExplanationBuilder& add_u64(std::string key, std::uint64_t value) {
    return add(std::move(key), std::to_string(value));
  }
  ExplanationBuilder& add_i64(std::string key, std::int64_t value) {
    return add(std::move(key), std::to_string(value));
  }
  ExplanationBuilder& add_bool(std::string key, bool value) {
    return add(std::move(key), value ? "true" : "false");
  }
  template <class Tag>
  ExplanationBuilder& add_id(std::string key, EntityId<Tag> id) {
    return add(std::move(key), id.valid() ? std::to_string(id.value()) : "none");
  }
  template <class Tag>
  ExplanationBuilder& add_generation(std::string key, Generation<Tag> generation) {
    return add(std::move(key), generation.valid() ? std::to_string(generation.value()) : "none");
  }
  ExplanationBuilder& set_message(std::string message) {
    message_ = std::move(message);
    return *this;
  }

  [[nodiscard]] Explanation build() const;

 private:
  OutcomeCode code_;
  std::string subject_;
  std::string message_;
  std::vector<ExplanationFactor> factors_;
};

/// Result of a runtime mutation.
struct MutationResult {
  OutcomeCode code{OutcomeCode::INTERNAL_ERROR};
  Explanation explanation;

  MutationResult() = default;
  MutationResult(OutcomeCode code_value, Explanation explanation_value)
      : code(code_value), explanation(std::move(explanation_value)) {}

  [[nodiscard]] bool accepted() const noexcept { return is_acceptance(code); }
  [[nodiscard]] bool rejected() const noexcept { return is_rejection(code); }
  [[nodiscard]] bool needs_revalidation() const noexcept { return requires_revalidation(code); }
  [[nodiscard]] bool ambiguous() const noexcept { return is_ambiguous(code); }

  [[nodiscard]] std::string to_text() const { return explanation.to_text(); }

  [[nodiscard]] static MutationResult make(OutcomeCode code_value, std::string subject,
                                           std::string message);
};

}  // namespace agent_runtime

#endif  // AGENT_RUNTIME_OUTCOME_HPP
