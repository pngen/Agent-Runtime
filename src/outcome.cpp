// Agent Runtime - outcome and explanation rendering.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "agent_runtime/outcome.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace agent_runtime {
namespace {

constexpr std::string_view kOutcomeNames[] = {
    "ACCEPTED",
    "NO_CHANGE",
    "DEFERRED",
    "REVALIDATION_REQUIRED",
    "REJECT_STALE_RUNTIME_EPOCH",
    "REJECT_STALE_RUNTIME_BOOT",
    "REJECT_STALE_AGENT_BOOT",
    "REJECT_STALE_RUN_GENERATION",
    "REJECT_STALE_STEP",
    "REJECT_STALE_ACTION",
    "REJECT_STALE_ATTEMPT",
    "REJECT_STALE_ASSIGNMENT",
    "REJECT_STALE_MODEL_BINDING",
    "REJECT_STALE_TOOL_BINDING",
    "REJECT_STALE_MEMORY_BINDING",
    "REJECT_STALE_CHECKPOINT",
    "REJECT_STALE_POLICY",
    "REJECT_STALE_BUDGET",
    "REJECT_CANCELLED",
    "REJECT_COMPLETED",
    "REJECT_RETIRED",
    "REJECT_NOT_READY",
    "REJECT_NOT_RESUMABLE",
    "REJECT_SIDE_EFFECT_UNSAFE",
    "REJECT_RETRY_EXHAUSTED",
    "REJECT_BUDGET",
    "REJECT_POLICY",
    "REJECT_CONFLICT",
    "REJECT_INVALID",
    "REJECT_LIMIT",
    "AMBIGUOUS_COMPLETION",
    "MANUAL_OR_EXTERNAL_RECONCILIATION_REQUIRED",
    "SHUTTING_DOWN",
    "INTERNAL_ERROR",
};

static_assert(std::size(kOutcomeNames) == static_cast<std::size_t>(OutcomeCode::kCount),
              "outcome name table must cover every outcome code");

[[nodiscard]] bool starts_with(std::string_view text, std::string_view prefix) noexcept {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

void append_json_string(std::string& out, std::string_view text) {
  out.push_back('"');
  for (const char raw : text) {
    const auto byte = static_cast<unsigned char>(raw);
    switch (byte) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (byte < 0x20u) {
          static constexpr char kDigits[] = "0123456789abcdef";
          out += "\\u00";
          out.push_back(kDigits[(byte >> 4) & 0x0Fu]);
          out.push_back(kDigits[byte & 0x0Fu]);
        } else {
          out.push_back(raw);
        }
        break;
    }
  }
  out.push_back('"');
}

}  // namespace

std::string_view to_string(OutcomeCode code) noexcept {
  const auto index = static_cast<std::size_t>(code);
  if (index >= std::size(kOutcomeNames)) {
    return "UNKNOWN_OUTCOME";
  }
  return kOutcomeNames[index];
}

bool is_acceptance(OutcomeCode code) noexcept {
  return code == OutcomeCode::ACCEPTED || code == OutcomeCode::NO_CHANGE;
}

bool is_rejection(OutcomeCode code) noexcept {
  return starts_with(to_string(code), "REJECT_");
}

bool requires_revalidation(OutcomeCode code) noexcept {
  if (code == OutcomeCode::REVALIDATION_REQUIRED) {
    return true;
  }
  switch (code) {
    case OutcomeCode::REJECT_STALE_RUNTIME_EPOCH:
    case OutcomeCode::REJECT_STALE_RUNTIME_BOOT:
    case OutcomeCode::REJECT_STALE_AGENT_BOOT:
    case OutcomeCode::REJECT_STALE_RUN_GENERATION:
    case OutcomeCode::REJECT_STALE_STEP:
    case OutcomeCode::REJECT_STALE_ACTION:
    case OutcomeCode::REJECT_STALE_ATTEMPT:
    case OutcomeCode::REJECT_STALE_ASSIGNMENT:
    case OutcomeCode::REJECT_STALE_MODEL_BINDING:
    case OutcomeCode::REJECT_STALE_TOOL_BINDING:
    case OutcomeCode::REJECT_STALE_MEMORY_BINDING:
    case OutcomeCode::REJECT_STALE_CHECKPOINT:
    case OutcomeCode::REJECT_STALE_POLICY:
    case OutcomeCode::REJECT_STALE_BUDGET:
      return true;
    default:
      return false;
  }
}

bool is_ambiguous(OutcomeCode code) noexcept {
  return code == OutcomeCode::AMBIGUOUS_COMPLETION ||
         code == OutcomeCode::MANUAL_OR_EXTERNAL_RECONCILIATION_REQUIRED;
}

const std::string* Explanation::find(std::string_view key) const noexcept {
  for (const ExplanationFactor& factor : factors_) {
    if (factor.key == key) {
      return &factor.value;
    }
  }
  return nullptr;
}

std::string Explanation::to_text() const {
  std::string out(to_string(code_));
  out.push_back(' ');
  out += subject_.empty() ? std::string("<none>") : subject_;
  out += " {";
  for (std::size_t i = 0; i < factors_.size(); ++i) {
    if (i != 0) {
      out.push_back(',');
    }
    out += factors_[i].key;
    out.push_back('=');
    out += factors_[i].value;
  }
  out += "}";
  if (!message_.empty()) {
    out.push_back(' ');
    out += message_;
  }
  return out;
}

std::string Explanation::to_json() const {
  std::string out;
  out += "{\"code\":";
  append_json_string(out, to_string(code_));
  out += ",\"subject\":";
  append_json_string(out, subject_);
  out += ",\"message\":";
  append_json_string(out, message_);
  out += ",\"factors\":[";
  for (std::size_t i = 0; i < factors_.size(); ++i) {
    if (i != 0) {
      out.push_back(',');
    }
    out += "{\"key\":";
    append_json_string(out, factors_[i].key);
    out += ",\"value\":";
    append_json_string(out, factors_[i].value);
    out.push_back('}');
  }
  out += "]}";
  return out;
}

void Explanation::add_factor(std::string key, std::string value) {
  factors_.push_back(ExplanationFactor{std::move(key), std::move(value)});
}

Explanation ExplanationBuilder::build() const {
  Explanation explanation(code_, subject_);
  explanation.set_message(message_);
  std::vector<ExplanationFactor> sorted = factors_;
  std::stable_sort(sorted.begin(), sorted.end(),
                   [](const ExplanationFactor& left, const ExplanationFactor& right) {
                     return left.key < right.key;
                   });
  for (const ExplanationFactor& factor : sorted) {
    explanation.add_factor(factor.key, factor.value);
  }
  return explanation;
}

MutationResult MutationResult::make(OutcomeCode code_value, std::string subject,
                                    std::string message) {
  Explanation explanation(code_value, std::move(subject));
  explanation.set_message(std::move(message));
  return MutationResult(code_value, std::move(explanation));
}

}  // namespace agent_runtime
