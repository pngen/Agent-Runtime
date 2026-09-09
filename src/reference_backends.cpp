// Agent Runtime - deterministic in-process reference model and tool backends.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// These backends are REFERENCE implementations: deterministic, offline and
// suitable for tests and examples. They do not call any external model
// provider and make no claim about model quality.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "agent_runtime/agent_runtime.hpp"
#include "agent_runtime/detail/sha256.hpp"

namespace agent_runtime {
namespace {

[[nodiscard]] bool is_control_or_invalid(char value) noexcept {
  const auto byte = static_cast<unsigned char>(value);
  if (byte < 0x20u) {
    return true;
  }
  switch (value) {
    case '<':
    case '>':
    case ':':
    case '"':
    case '|':
    case '?':
    case '*':
      return true;
    default:
      return false;
  }
}

/// Rejects absolute paths, drive-relative paths, traversal components, control
/// characters and over-long names. The caller additionally verifies that the
/// resolved path stays inside the scratch root.
[[nodiscard]] bool safe_relative_path(const std::string& path, std::string& error) {
  if (path.empty()) {
    error = "path is empty";
    return false;
  }
  if (path.size() > 200) {
    error = "path exceeds the maximum length";
    return false;
  }
  if (path.front() == '/' || path.front() == '\\' || path.find(':') != std::string::npos) {
    error = "absolute or drive-qualified paths are not permitted";
    return false;
  }
  if (path.back() == '/' || path.back() == '\\') {
    error = "directory paths are not permitted";
    return false;
  }
  for (const char value : path) {
    if (is_control_or_invalid(value) && value != '/') {
      error = "path contains an invalid character";
      return false;
    }
  }
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t end = path.find_first_of("/\\", start);
    const std::string component =
        end == std::string::npos ? path.substr(start) : path.substr(start, end - start);
    if (component.empty() || component == "." || component == "..") {
      error = "path contains an empty, current or parent directory component";
      return false;
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return true;
}

[[nodiscard]] std::string to_hex(std::string_view text) {
  const auto digest = detail::sha256(text);
  return detail::hex_encode(std::span<const std::uint8_t>(digest.data(), digest.size()));
}

[[nodiscard]] std::string json_escape(std::string_view text) {
  std::string out;
  for (const char raw : text) {
    const auto byte = static_cast<unsigned char>(raw);
    if (byte == '"' || byte == '\\') {
      out.push_back('\\');
      out.push_back(raw);
    } else if (byte < 0x20u) {
      out += "?";
    } else {
      out.push_back(raw);
    }
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// ReferenceModelBackend
// ---------------------------------------------------------------------------

struct ReferenceModelBackend::Impl {
  std::string name;
  std::uint64_t generation = 1;
  mutable std::mutex mutex;
  std::uint64_t invocations = 0;
  bool fail_pending = false;
  RetryClass fail_class = RetryClass::NONE;
  std::string fail_error;
  std::uint64_t logical_latency_millis = 0;
};

ReferenceModelBackend::ReferenceModelBackend(std::string name, std::uint64_t generation)
    : impl_(std::make_unique<Impl>()) {
  impl_->name = std::move(name);
  impl_->generation = generation == 0 ? 1 : generation;
}

ReferenceModelBackend::~ReferenceModelBackend() = default;

BackendIncarnation ReferenceModelBackend::incarnation() const {
  BackendIncarnation incarnation;
  incarnation.backend_id = BackendId(0xA1B2C3D4ull);
  incarnation.generation = Generation<BackendTag>(impl_->generation);
  incarnation.name = impl_->name;
  incarnation.available = true;
  return incarnation;
}

ModelResponse ReferenceModelBackend::invoke(const ModelRequest& request,
                                            const CancellationProbe& cancellation) {
  ModelResponse response;
  response.action_id = request.action_id;
  response.call_id = request.call_id;
  response.call_generation = request.call_generation;
  response.attempt_id = request.attempt_id;
  response.attempt_generation = request.attempt_generation;
  response.backend_name = impl_->name;
  response.backend_generation = Generation<BackendTag>(impl_->generation);
  response.result_id = allocate_id<ResultTag>();

  bool fail = false;
  RetryClass failure_class = RetryClass::NONE;
  std::string error;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    ++impl_->invocations;
    response.completion_generation = CompletionGeneration(impl_->invocations);
    if (impl_->fail_pending) {
      impl_->fail_pending = false;
      fail = true;
      failure_class = impl_->fail_class;
      error = impl_->fail_error;
    }
  }
  if (cancellation.cancelled()) {
    response.status = CompletionStatus::CANCELLED;
    response.error = "cancelled before invocation";
    return response;
  }
  if (fail) {
    response.status = CompletionStatus::FAILED;
    response.failure_class = failure_class;
    response.error = error.empty() ? "injected reference model failure" : error;
    return response;
  }

  // Deterministic synthetic output. This is not a language model: it is a
  // reproducible function of the request identity and prompt.
  std::ostringstream out;
  out << "{\"reference\":true,\"target\":\"" << json_escape(request.target)
      << "\",\"prompt_bytes\":" << request.prompt.size()
      << ",\"prompt_digest\":\"" << to_hex(request.prompt)
      << "\",\"call_generation\":" << request.call_generation.value()
      << ",\"attempt_generation\":" << request.attempt_generation.value() << "}";
  response.output = out.str();
  if (response.output.size() > request.max_output_bytes) {
    response.output.resize(request.max_output_bytes);
  }
  response.output_digest = detail::content_digest(response.output);
  response.prompt_tokens = request.prompt.size() / 4 + 1;
  response.output_tokens = response.output.size() / 4 + 1;
  response.status = CompletionStatus::SUCCEEDED;
  return response;
}

void ReferenceModelBackend::fail_next(RetryClass failure_class, std::string error) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->fail_pending = true;
  impl_->fail_class = failure_class;
  impl_->fail_error = std::move(error);
}

void ReferenceModelBackend::set_latency_logical_millis(std::uint64_t millis) {
  // Recorded as metadata only. This backend never sleeps: deterministic tests
  // must not depend on wall-clock passage.
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->logical_latency_millis = millis;
}

std::uint64_t ReferenceModelBackend::invocation_count() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->invocations;
}

// ---------------------------------------------------------------------------
// ReferenceToolBackend
// ---------------------------------------------------------------------------

struct ReferenceToolBackend::Impl {
  std::string name;
  std::uint64_t generation = 1;
  std::string journal_directory;
  mutable std::mutex mutex;
  std::uint64_t invocations = 0;
  std::uint64_t receipts = 0;
  std::map<std::string, std::string> receipt_digests;
  bool fail_pending = false;
  RetryClass fail_class = RetryClass::NONE;
  std::string fail_error;
  bool crash_after_side_effect = false;
};

namespace {

[[nodiscard]] bool parse_integers(const std::string& input, std::vector<std::int64_t>& out,
                                  std::string& error) {
  std::istringstream stream(input);
  std::int64_t value = 0;
  while (stream >> value) {
    if (out.size() >= 100000) {
      error = "input contains too many integers";
      return false;
    }
    out.push_back(value);
  }
  if (!stream.eof()) {
    error = "input contains a token that is not a 64-bit integer";
    return false;
  }
  return true;
}

}  // namespace

ReferenceToolBackend::ReferenceToolBackend(std::string name, std::uint64_t generation)
    : impl_(std::make_unique<Impl>()) {
  impl_->name = std::move(name);
  impl_->generation = generation == 0 ? 1 : generation;
}

ReferenceToolBackend::~ReferenceToolBackend() = default;

BackendIncarnation ReferenceToolBackend::incarnation() const {
  BackendIncarnation incarnation;
  incarnation.backend_id = BackendId(0x7E570123ull);
  incarnation.generation = Generation<BackendTag>(impl_->generation);
  incarnation.name = impl_->name;
  incarnation.available = true;
  return incarnation;
}

void ReferenceToolBackend::set_journal_directory(std::string directory) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->journal_directory = std::move(directory);
  impl_->receipt_digests.clear();
  impl_->receipts = 0;
}

bool ReferenceToolBackend::has_receipt(std::string_view operation_key) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->receipt_digests.find(std::string(operation_key)) != impl_->receipt_digests.end();
}

std::uint64_t ReferenceToolBackend::invocation_count() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->invocations;
}

std::uint64_t ReferenceToolBackend::receipt_count() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->receipts;
}

void ReferenceToolBackend::fail_next(RetryClass failure_class, std::string error) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->fail_pending = true;
  impl_->fail_class = failure_class;
  impl_->fail_error = std::move(error);
}

void ReferenceToolBackend::crash_after_side_effect_once() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->crash_after_side_effect = true;
}

ToolResponse ReferenceToolBackend::invoke(const ToolRequest& request,
                                          const CancellationProbe& cancellation) {
  ToolResponse response;
  response.action_id = request.action_id;
  response.call_id = request.call_id;
  response.call_generation = request.call_generation;
  response.attempt_id = request.attempt_id;
  response.attempt_generation = request.attempt_generation;
  response.backend_name = impl_->name;
  response.backend_generation = Generation<BackendTag>(impl_->generation);
  response.result_id = allocate_id<ResultTag>();

  bool fail = false;
  bool crash_after = false;
  RetryClass failure_class = RetryClass::NONE;
  std::string error;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    ++impl_->invocations;
    response.completion_generation = CompletionGeneration(impl_->invocations);
    if (impl_->fail_pending) {
      impl_->fail_pending = false;
      fail = true;
      failure_class = impl_->fail_class;
      error = impl_->fail_error;
    }
    crash_after = impl_->crash_after_side_effect;
    impl_->crash_after_side_effect = false;
  }
  if (cancellation.cancelled()) {
    response.status = CompletionStatus::CANCELLED;
    response.error = "cancelled before invocation";
    return response;
  }
  if (fail) {
    response.status = CompletionStatus::FAILED;
    response.failure_class = failure_class;
    response.error = error.empty() ? "injected reference tool failure" : error;
    return response;
  }
  if (request.input.size() > 65536) {
    response.status = CompletionStatus::FAILED;
    response.failure_class = RetryClass::PERMANENT;
    response.error = "input exceeds the reference tool input bound";
    return response;
  }

  const bool has_key = !request.operation_key.empty();
  const bool dedup_capable = supports_operation_key(request.side_effect);
  if (dedup_capable && has_key) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->receipt_digests.find(request.operation_key);
    if (found != impl_->receipt_digests.end()) {
      response.output = "receipt:" + found->second;
      response.result_digest = detail::content_digest(response.output);
      response.status = CompletionStatus::SUCCEEDED;
      response.deduplicated = true;
      response.commit_receipt = true;
      return response;
    }
  }

  const std::string& tool = request.tool_name;
  std::string output;
  bool side_effect_applied = false;

  if (tool == "hash") {
    output = to_hex(request.input);
  } else if (tool == "vector-sum") {
    std::vector<std::int64_t> values;
    std::string parse_error;
    if (!parse_integers(request.input, values, parse_error)) {
      response.status = CompletionStatus::FAILED;
      response.failure_class = RetryClass::PERMANENT;
      response.error = parse_error;
      return response;
    }
    std::int64_t sum = 0;
    for (const std::int64_t value : values) {
      if ((value > 0 && sum > INT64_MAX - value) || (value < 0 && sum < INT64_MIN - value)) {
        response.status = CompletionStatus::FAILED;
        response.failure_class = RetryClass::PERMANENT;
        response.error = "integer overflow";
        return response;
      }
      sum += value;
    }
    output = std::to_string(sum);
  } else if (tool == "echo") {
    output = "{\"echo\":\"" + json_escape(request.input) + "\",\"bytes\":" +
             std::to_string(request.input.size()) + ",\"digest\":\"" +
             std::to_string(request.input_digest) + "\"}";
  } else if (tool == "scratch-transform") {
    const std::size_t separator = request.input.find('|');
    if (separator == std::string::npos) {
      response.status = CompletionStatus::FAILED;
      response.failure_class = RetryClass::PERMANENT;
      response.error = "scratch-transform input must be '<relative path>|<content>'";
      return response;
    }
    const std::string relative = request.input.substr(0, separator);
    const std::string content = request.input.substr(separator + 1);
    std::string path_error;
    if (!safe_relative_path(relative, path_error)) {
      response.status = CompletionStatus::FAILED;
      response.failure_class = RetryClass::PERMANENT;
      response.error = "unsafe path: " + path_error;
      return response;
    }
    if (content.size() > 65536) {
      response.status = CompletionStatus::FAILED;
      response.failure_class = RetryClass::PERMANENT;
      response.error = "content exceeds the bounded scratch write size";
      return response;
    }
    std::string root;
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      root = impl_->journal_directory;
    }
    if (root.empty()) {
      response.status = CompletionStatus::FAILED;
      response.failure_class = RetryClass::PERMANENT;
      response.error = "scratch root is not configured";
      return response;
    }
    std::error_code error_code;
    const std::filesystem::path root_path = std::filesystem::weakly_canonical(root, error_code);
    const std::filesystem::path target =
        std::filesystem::weakly_canonical(root_path / relative, error_code);
    const std::string root_text = root_path.generic_string();
    const std::string target_text = target.generic_string();
    if (target_text.size() <= root_text.size() ||
        target_text.compare(0, root_text.size(), root_text) != 0) {
      response.status = CompletionStatus::FAILED;
      response.failure_class = RetryClass::PERMANENT;
      response.error = "resolved path escapes the scratch root";
      return response;
    }
    std::filesystem::create_directories(target.parent_path(), error_code);
    std::ofstream stream(target, std::ios::binary | std::ios::trunc);
    if (!stream) {
      response.status = CompletionStatus::FAILED;
      response.failure_class = RetryClass::RESOURCE_UNAVAILABLE;
      response.error = "cannot open the scratch target for writing";
      return response;
    }
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    stream.close();
    if (!stream) {
      response.status = CompletionStatus::FAILED;
      response.failure_class = RetryClass::RESOURCE_UNAVAILABLE;
      response.error = "scratch write failed";
      return response;
    }
    output = "wrote " + std::to_string(content.size()) + " bytes to " + relative + " (" +
             to_hex(content) + ")";
    side_effect_applied = true;
  } else if (tool == "commit-token" || tool == "non-repeatable") {
    if (!has_key) {
      response.status = CompletionStatus::FAILED;
      response.failure_class = RetryClass::PERMANENT;
      response.error = "commit-token operations require a stable operation key";
      return response;
    }
    const std::string receipt_digest = to_hex(request.operation_key + "|" + request.input);
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      impl_->receipt_digests[request.operation_key] = receipt_digest;
      ++impl_->receipts;
      std::string root = impl_->journal_directory;
      if (!root.empty()) {
        std::error_code error_code;
        std::filesystem::create_directories(root, error_code);
        std::ofstream journal(std::filesystem::path(root) / (request.operation_key + ".receipt"),
                              std::ios::binary | std::ios::trunc);
        if (journal) {
          journal << receipt_digest << "\n" << request.input;
        }
      }
    }
    output = "receipt:" + receipt_digest;
    side_effect_applied = true;
    response.commit_receipt = true;
  } else {
    response.status = CompletionStatus::FAILED;
    response.failure_class = RetryClass::PERMANENT;
    response.error = "unknown reference tool: " + tool;
    return response;
  }

  if (output.size() > request.max_output_bytes) {
    output.resize(request.max_output_bytes);
  }
  response.output = std::move(output);
  response.result_digest = detail::content_digest(response.output);
  if (crash_after && side_effect_applied) {
    // The physical side effect happened; the runtime cannot know that. It must
    // preserve the uncertainty instead of inventing safety.
    response.status = CompletionStatus::AMBIGUOUS;
    response.error = "tool worker lost after the side effect; completion unprovable";
    return response;
  }
  response.status = CompletionStatus::SUCCEEDED;
  return response;
}

}  // namespace agent_runtime
