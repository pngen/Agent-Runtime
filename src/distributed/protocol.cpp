// Agent Runtime - framed protocol implementation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "agent_runtime/distributed/protocol.hpp"

#include <array>
#include <cstring>

#include "agent_runtime/detail/crc32c.hpp"

namespace agent_runtime::distributed {
namespace {

constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kVersionOffset = 4;
constexpr std::size_t kTypeOffset = 6;
constexpr std::size_t kFlagsOffset = 8;
constexpr std::size_t kCorrelationOffset = 12;
constexpr std::size_t kPayloadBytesOffset = 20;
constexpr std::size_t kHeaderCrcOffset = 24;
constexpr std::size_t kPayloadLengthOffset = 20;
constexpr std::size_t kHeaderCrcCoveredBytes = 24;

[[nodiscard]] bool known_type(std::uint16_t value) noexcept {
  return value >= static_cast<std::uint16_t>(MessageType::HELLO) &&
         value < static_cast<std::uint16_t>(MessageType::kCount);
}

void put_u16(std::vector<std::uint8_t>& out, std::size_t offset, std::uint16_t value) {
  out[offset] = static_cast<std::uint8_t>(value & 0xFFu);
  out[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
}

void put_u32(std::vector<std::uint8_t>& out, std::size_t offset, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out[offset + static_cast<std::size_t>(i)] =
        static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
  }
}

void put_u64(std::vector<std::uint8_t>& out, std::size_t offset, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out[offset + static_cast<std::size_t>(i)] =
        static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
  }
}

[[nodiscard]] std::uint16_t get_u16(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset]) |
         static_cast<std::uint16_t>(bytes[offset + 1] << 8);
}

[[nodiscard]] std::uint32_t get_u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(bytes[offset + static_cast<std::size_t>(i)]) << (8 * i);
  }
  return value;
}

[[nodiscard]] std::uint64_t get_u64(std::span<const std::uint8_t> bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(bytes[offset + static_cast<std::size_t>(i)]) << (8 * i);
  }
  return value;
}

}  // namespace

std::string_view to_string(MessageType type) noexcept {
  switch (type) {
    case MessageType::HELLO:
      return "HELLO";
    case MessageType::HELLO_ACK:
      return "HELLO_ACK";
    case MessageType::REGISTER_RUNTIME:
      return "REGISTER_RUNTIME";
    case MessageType::REGISTER_BACKEND:
      return "REGISTER_BACKEND";
    case MessageType::START_RUN:
      return "START_RUN";
    case MessageType::SUSPEND_RUN:
      return "SUSPEND_RUN";
    case MessageType::RESUME_RUN:
      return "RESUME_RUN";
    case MessageType::CANCEL_RUN:
      return "CANCEL_RUN";
    case MessageType::DRAIN_RUN:
      return "DRAIN_RUN";
    case MessageType::DECLARE_ACTION:
      return "DECLARE_ACTION";
    case MessageType::AUTHORIZE_ACTION:
      return "AUTHORIZE_ACTION";
    case MessageType::DISPATCH_ACTION:
      return "DISPATCH_ACTION";
    case MessageType::MODEL_REQUEST:
      return "MODEL_REQUEST";
    case MessageType::MODEL_RESULT:
      return "MODEL_RESULT";
    case MessageType::TOOL_REQUEST:
      return "TOOL_REQUEST";
    case MessageType::TOOL_RESULT:
      return "TOOL_RESULT";
    case MessageType::CHECKPOINT_REQUEST:
      return "CHECKPOINT_REQUEST";
    case MessageType::CHECKPOINT_RESULT:
      return "CHECKPOINT_RESULT";
    case MessageType::ACTION_COMPLETION:
      return "ACTION_COMPLETION";
    case MessageType::ACTION_FAILURE:
      return "ACTION_FAILURE";
    case MessageType::HEARTBEAT:
      return "HEARTBEAT";
    case MessageType::QUERY:
      return "QUERY";
    case MessageType::QUERY_RESULT:
      return "QUERY_RESULT";
    case MessageType::ERROR_MESSAGE:
      return "ERROR_MESSAGE";
    case MessageType::kCount:
      break;
  }
  return "UNKNOWN_MESSAGE";
}

void PayloadWriter::u8(std::uint8_t value) { bytes_.push_back(value); }

void PayloadWriter::u16(std::uint16_t value) {
  bytes_.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  bytes_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void PayloadWriter::u32(std::uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    bytes_.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
  }
}

void PayloadWriter::u64(std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    bytes_.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
  }
}

void PayloadWriter::boolean(bool value) { u8(value ? 1u : 0u); }

void PayloadWriter::text(std::string_view value, std::uint32_t max_string_bytes) {
  const std::uint32_t size =
      value.size() > max_string_bytes ? max_string_bytes : static_cast<std::uint32_t>(value.size());
  u32(size);
  bytes_.insert(bytes_.end(), value.begin(), value.begin() + size);
}

bool PayloadReader::need(std::size_t count) {
  if (!ok_) {
    return false;
  }
  if (count > bytes_.size() - offset_) {
    ok_ = false;
    error_ = "payload is truncated";
    return false;
  }
  return true;
}

std::uint8_t PayloadReader::u8() {
  if (!need(1)) {
    return 0;
  }
  return bytes_[offset_++];
}

std::uint16_t PayloadReader::u16() {
  if (!need(2)) {
    return 0;
  }
  const std::uint16_t value = get_u16(bytes_, offset_);
  offset_ += 2;
  return value;
}

std::uint32_t PayloadReader::u32() {
  if (!need(4)) {
    return 0;
  }
  const std::uint32_t value = get_u32(bytes_, offset_);
  offset_ += 4;
  return value;
}

std::uint64_t PayloadReader::u64() {
  if (!need(8)) {
    return 0;
  }
  const std::uint64_t value = get_u64(bytes_, offset_);
  offset_ += 8;
  return value;
}

bool PayloadReader::boolean() { return u8() != 0; }

std::string PayloadReader::text(std::uint32_t max_string_bytes) {
  const std::uint32_t size = u32();
  if (!ok_) {
    return {};
  }
  if (size > max_string_bytes) {
    ok_ = false;
    error_ = "payload string exceeds the configured limit";
    return {};
  }
  if (!need(size)) {
    return {};
  }
  std::string value(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
  offset_ += size;
  return value;
}

std::vector<std::uint8_t> encode_frame(const Frame& frame, std::uint32_t max_payload_bytes) {
  if (!known_type(static_cast<std::uint16_t>(frame.type))) {
    return {};
  }
  if (frame.payload.size() > max_payload_bytes) {
    return {};
  }
  std::vector<std::uint8_t> out(frame_header_bytes + frame.payload.size() + frame_trailer_bytes, 0);
  std::memcpy(out.data(), protocol_magic.data(), protocol_magic.size());
  put_u16(out, kVersionOffset, protocol_version);
  put_u16(out, kTypeOffset, static_cast<std::uint16_t>(frame.type));
  put_u32(out, kFlagsOffset, frame.flags);
  put_u64(out, kCorrelationOffset, frame.correlation.value());
  put_u32(out, kPayloadLengthOffset, static_cast<std::uint32_t>(frame.payload.size()));
  const std::uint32_t header_crc = detail::crc32c(
      std::span<const std::uint8_t>(out.data(), kHeaderCrcCoveredBytes));
  put_u32(out, kHeaderCrcOffset, header_crc);
  std::memcpy(out.data() + frame_header_bytes, frame.payload.data(), frame.payload.size());
  const std::uint32_t payload_crc = detail::crc32c(
      std::span<const std::uint8_t>(out.data() + frame_header_bytes, frame.payload.size()));
  put_u32(out, frame_header_bytes + frame.payload.size(), payload_crc);
  return out;
}

bool decode_frame(std::span<const std::uint8_t> bytes, Frame& out, std::string& error,
                  std::uint32_t max_payload_bytes) {
  if (bytes.size() < frame_header_bytes + frame_trailer_bytes) {
    error = "frame is shorter than the minimum frame size";
    return false;
  }
  for (std::size_t i = 0; i < protocol_magic.size(); ++i) {
    if (bytes[kMagicOffset + i] != static_cast<std::uint8_t>(protocol_magic[i])) {
      error = "frame magic does not match";
      return false;
    }
  }
  const std::uint16_t version = get_u16(bytes, kVersionOffset);
  if (version != protocol_version) {
    error = "unsupported protocol version " + std::to_string(version);
    return false;
  }
  const std::uint16_t type = get_u16(bytes, kTypeOffset);
  if (!known_type(type)) {
    error = "unknown message type " + std::to_string(type);
    return false;
  }
  const std::uint32_t payload_bytes = get_u32(bytes, kPayloadLengthOffset);
  if (payload_bytes > max_payload_bytes) {
    error = "declared payload length exceeds the configured bound";
    return false;
  }
  const std::uint64_t expected =
      static_cast<std::uint64_t>(frame_header_bytes) + payload_bytes + frame_trailer_bytes;
  if (expected != bytes.size()) {
    error = "declared payload length does not match the frame length";
    return false;
  }
  const std::uint32_t header_crc =
      detail::crc32c(std::span<const std::uint8_t>(bytes.data(), kHeaderCrcCoveredBytes));
  if (header_crc != get_u32(bytes, kHeaderCrcOffset)) {
    error = "frame header integrity check failed";
    return false;
  }
  const std::uint32_t payload_crc = detail::crc32c(
      std::span<const std::uint8_t>(bytes.data() + frame_header_bytes, payload_bytes));
  if (payload_crc != get_u32(bytes, frame_header_bytes + payload_bytes)) {
    error = "frame payload integrity check failed";
    return false;
  }
  out.type = static_cast<MessageType>(type);
  out.flags = get_u32(bytes, kFlagsOffset);
  out.correlation = CorrelationId(get_u64(bytes, kCorrelationOffset));
  out.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(frame_header_bytes),
                     bytes.begin() + static_cast<std::ptrdiff_t>(frame_header_bytes + payload_bytes));
  return true;
}

std::vector<std::uint8_t> encode_hello(const HelloMessage& message) {
  PayloadWriter writer;
  writer.text(message.role);
  writer.u16(message.version);
  writer.u64(message.process_id);
  return writer.take();
}

bool decode_hello(std::span<const std::uint8_t> payload, HelloMessage& out, std::string& error) {
  PayloadReader reader(payload);
  out.role = reader.text();
  out.version = reader.u16();
  out.process_id = reader.u64();
  if (!reader.ok() || !reader.exhausted()) {
    error = reader.ok() ? "hello payload has trailing bytes" : reader.error();
    return false;
  }
  return true;
}

std::vector<std::uint8_t> encode_register_runtime(const RegisterRuntimeMessage& message) {
  PayloadWriter writer;
  writer.u64(message.runtime_id.value());
  writer.u64(message.runtime_generation.value());
  writer.u64(message.runtime_boot_id.value());
  writer.u64(message.agent_id.value());
  writer.u64(message.agent_generation.value());
  writer.u64(message.agent_boot_id.value());
  writer.u64(message.coordinator_epoch.value());
  writer.u64(message.runtime_epoch.value());
  return writer.take();
}

bool decode_register_runtime(std::span<const std::uint8_t> payload, RegisterRuntimeMessage& out,
                             std::string& error) {
  PayloadReader reader(payload);
  out.runtime_id = AgentRuntimeId(reader.u64());
  out.runtime_generation = AgentRuntimeGeneration(reader.u64());
  out.runtime_boot_id = RuntimeBootId(reader.u64());
  out.agent_id = AgentId(reader.u64());
  out.agent_generation = AgentGeneration(reader.u64());
  out.agent_boot_id = AgentBootId(reader.u64());
  out.coordinator_epoch = CoordinatorEpoch(reader.u64());
  out.runtime_epoch = RuntimeEpoch(reader.u64());
  if (!reader.ok() || !reader.exhausted()) {
    error = reader.ok() ? "register_runtime payload has trailing bytes" : reader.error();
    return false;
  }
  return true;
}

std::vector<std::uint8_t> encode_register_backend(const RegisterBackendMessage& message) {
  PayloadWriter writer;
  writer.u64(message.backend_id.value());
  writer.u64(message.generation.value());
  writer.text(message.name);
  writer.text(message.kind);
  writer.text(message.endpoint);
  return writer.take();
}

bool decode_register_backend(std::span<const std::uint8_t> payload, RegisterBackendMessage& out,
                             std::string& error) {
  PayloadReader reader(payload);
  out.backend_id = BackendId(reader.u64());
  out.generation = Generation<BackendTag>(reader.u64());
  out.name = reader.text();
  out.kind = reader.text();
  out.endpoint = reader.text();
  if (!reader.ok() || !reader.exhausted()) {
    error = reader.ok() ? "register_backend payload has trailing bytes" : reader.error();
    return false;
  }
  return true;
}

std::vector<std::uint8_t> encode_error(const ErrorMessage& message) {
  PayloadWriter writer;
  writer.u16(message.code);
  writer.text(message.detail);
  return writer.take();
}

bool decode_error(std::span<const std::uint8_t> payload, ErrorMessage& out, std::string& error) {
  PayloadReader reader(payload);
  out.code = reader.u16();
  out.detail = reader.text();
  if (!reader.ok() || !reader.exhausted()) {
    error = reader.ok() ? "error payload has trailing bytes" : reader.error();
    return false;
  }
  return true;
}

std::vector<std::uint8_t> encode_query(const QueryMessage& message) {
  PayloadWriter writer;
  writer.text(message.subject);
  return writer.take();
}

bool decode_query(std::span<const std::uint8_t> payload, QueryMessage& out, std::string& error) {
  PayloadReader reader(payload);
  out.subject = reader.text();
  if (!reader.ok() || !reader.exhausted()) {
    error = reader.ok() ? "query payload has trailing bytes" : reader.error();
    return false;
  }
  return true;
}

std::vector<std::uint8_t> encode_query_result(const QueryResultMessage& message) {
  PayloadWriter writer;
  writer.text(message.subject);
  writer.text(message.json, 1024 * 1024);
  return writer.take();
}

bool decode_query_result(std::span<const std::uint8_t> payload, QueryResultMessage& out,
                         std::string& error) {
  PayloadReader reader(payload);
  out.subject = reader.text();
  out.json = reader.text(1024 * 1024);
  if (!reader.ok() || !reader.exhausted()) {
    error = reader.ok() ? "query_result payload has trailing bytes" : reader.error();
    return false;
  }
  return true;
}

std::vector<std::uint8_t> encode_tool_request(const ToolRequest& request) {
  PayloadWriter writer;
  writer.u64(request.call_id.value());
  writer.u64(request.call_generation.value());
  writer.u64(request.action_id.value());
  writer.u64(request.action_generation.value());
  writer.u64(request.attempt_id.value());
  writer.u64(request.attempt_generation.value());
  writer.u64(request.runtime_id.value());
  writer.u64(request.runtime_boot_id.value());
  writer.text(request.tool_name);
  writer.u16(static_cast<std::uint16_t>(request.side_effect));
  writer.text(request.operation_key);
  writer.text(request.input, 1024 * 1024);
  writer.u64(request.input_digest);
  writer.u32(request.max_output_bytes);
  return writer.take();
}

bool decode_tool_request(std::span<const std::uint8_t> payload, ToolRequest& out,
                         std::string& error) {
  PayloadReader reader(payload);
  out.call_id = ToolCallId(reader.u64());
  out.call_generation = ToolCallGeneration(reader.u64());
  out.action_id = ActionId(reader.u64());
  out.action_generation = ActionGeneration(reader.u64());
  out.attempt_id = ActionAttemptId(reader.u64());
  out.attempt_generation = AttemptGeneration(reader.u64());
  out.runtime_id = AgentRuntimeId(reader.u64());
  out.runtime_boot_id = RuntimeBootId(reader.u64());
  out.tool_name = reader.text();
  const std::uint16_t side_effect = reader.u16();
  if (side_effect >= static_cast<std::uint16_t>(SideEffectClass::kCount)) {
    error = "tool request carries an invalid side-effect class";
    return false;
  }
  out.side_effect = static_cast<SideEffectClass>(side_effect);
  out.operation_key = reader.text();
  out.input = reader.text(1024 * 1024);
  out.input_digest = reader.u64();
  out.max_output_bytes = reader.u32();
  if (!reader.ok() || !reader.exhausted()) {
    error = reader.ok() ? "tool_request payload has trailing bytes" : reader.error();
    return false;
  }
  return true;
}

std::vector<std::uint8_t> encode_tool_response(const ToolResponse& response) {
  PayloadWriter writer;
  writer.u64(response.result_id.value());
  writer.u64(response.completion_generation.value());
  writer.u64(response.action_id.value());
  writer.u64(response.call_id.value());
  writer.u64(response.call_generation.value());
  writer.u64(response.attempt_id.value());
  writer.u64(response.attempt_generation.value());
  writer.u16(static_cast<std::uint16_t>(response.status));
  writer.u16(static_cast<std::uint16_t>(response.failure_class));
  writer.text(response.output, 1024 * 1024);
  writer.u64(response.result_digest);
  writer.text(response.error);
  writer.text(response.backend_name);
  writer.u64(response.backend_generation.value());
  writer.boolean(response.deduplicated);
  writer.boolean(response.commit_receipt);
  return writer.take();
}

bool decode_tool_response(std::span<const std::uint8_t> payload, ToolResponse& out,
                          std::string& error) {
  PayloadReader reader(payload);
  out.result_id = ResultId(reader.u64());
  out.completion_generation = CompletionGeneration(reader.u64());
  out.action_id = ActionId(reader.u64());
  out.call_id = ToolCallId(reader.u64());
  out.call_generation = ToolCallGeneration(reader.u64());
  out.attempt_id = ActionAttemptId(reader.u64());
  out.attempt_generation = AttemptGeneration(reader.u64());
  const std::uint16_t status = reader.u16();
  if (status >= static_cast<std::uint16_t>(CompletionStatus::kCount)) {
    error = "tool response carries an invalid completion status";
    return false;
  }
  out.status = static_cast<CompletionStatus>(status);
  const std::uint16_t failure = reader.u16();
  if (failure >= static_cast<std::uint16_t>(RetryClass::kCount)) {
    error = "tool response carries an invalid failure class";
    return false;
  }
  out.failure_class = static_cast<RetryClass>(failure);
  out.output = reader.text(1024 * 1024);
  out.result_digest = reader.u64();
  out.error = reader.text();
  out.backend_name = reader.text();
  out.backend_generation = Generation<BackendTag>(reader.u64());
  out.deduplicated = reader.boolean();
  out.commit_receipt = reader.boolean();
  if (!reader.ok() || !reader.exhausted()) {
    error = reader.ok() ? "tool_response payload has trailing bytes" : reader.error();
    return false;
  }
  return true;
}

std::vector<std::uint8_t> encode_model_request(const ModelRequest& request) {
  PayloadWriter writer;
  writer.u64(request.call_id.value());
  writer.u64(request.call_generation.value());
  writer.u64(request.action_id.value());
  writer.u64(request.action_generation.value());
  writer.u64(request.attempt_id.value());
  writer.u64(request.attempt_generation.value());
  writer.u64(request.runtime_id.value());
  writer.u64(request.runtime_boot_id.value());
  writer.text(request.target);
  writer.text(request.prompt, 1024 * 1024);
  writer.u32(request.max_output_bytes);
  writer.u64(request.request_digest);
  writer.text(request.context_identity);
  writer.u64(request.context_generation.value());
  writer.u64(request.budget_actions_remaining);
  return writer.take();
}

bool decode_model_request(std::span<const std::uint8_t> payload, ModelRequest& out,
                          std::string& error) {
  PayloadReader reader(payload);
  out.call_id = ModelCallId(reader.u64());
  out.call_generation = ModelCallGeneration(reader.u64());
  out.action_id = ActionId(reader.u64());
  out.action_generation = ActionGeneration(reader.u64());
  out.attempt_id = ActionAttemptId(reader.u64());
  out.attempt_generation = AttemptGeneration(reader.u64());
  out.runtime_id = AgentRuntimeId(reader.u64());
  out.runtime_boot_id = RuntimeBootId(reader.u64());
  out.target = reader.text();
  out.prompt = reader.text(1024 * 1024);
  out.max_output_bytes = reader.u32();
  out.request_digest = reader.u64();
  out.context_identity = reader.text();
  out.context_generation = MemoryBindingGeneration(reader.u64());
  out.budget_actions_remaining = reader.u64();
  if (!reader.ok() || !reader.exhausted()) {
    error = reader.ok() ? "model_request payload has trailing bytes" : reader.error();
    return false;
  }
  return true;
}

std::vector<std::uint8_t> encode_model_response(const ModelResponse& response) {
  PayloadWriter writer;
  writer.u64(response.result_id.value());
  writer.u64(response.completion_generation.value());
  writer.u64(response.action_id.value());
  writer.u64(response.call_id.value());
  writer.u64(response.call_generation.value());
  writer.u64(response.attempt_id.value());
  writer.u64(response.attempt_generation.value());
  writer.u16(static_cast<std::uint16_t>(response.status));
  writer.u16(static_cast<std::uint16_t>(response.failure_class));
  writer.text(response.output, 1024 * 1024);
  writer.u64(response.output_digest);
  writer.u64(response.prompt_tokens);
  writer.u64(response.output_tokens);
  writer.text(response.error);
  writer.text(response.backend_name);
  writer.u64(response.backend_generation.value());
  return writer.take();
}

bool decode_model_response(std::span<const std::uint8_t> payload, ModelResponse& out,
                           std::string& error) {
  PayloadReader reader(payload);
  out.result_id = ResultId(reader.u64());
  out.completion_generation = CompletionGeneration(reader.u64());
  out.action_id = ActionId(reader.u64());
  out.call_id = ModelCallId(reader.u64());
  out.call_generation = ModelCallGeneration(reader.u64());
  out.attempt_id = ActionAttemptId(reader.u64());
  out.attempt_generation = AttemptGeneration(reader.u64());
  const std::uint16_t status = reader.u16();
  if (status >= static_cast<std::uint16_t>(CompletionStatus::kCount)) {
    error = "model response carries an invalid completion status";
    return false;
  }
  out.status = static_cast<CompletionStatus>(status);
  const std::uint16_t failure = reader.u16();
  if (failure >= static_cast<std::uint16_t>(RetryClass::kCount)) {
    error = "model response carries an invalid failure class";
    return false;
  }
  out.failure_class = static_cast<RetryClass>(failure);
  out.output = reader.text(1024 * 1024);
  out.output_digest = reader.u64();
  out.prompt_tokens = reader.u64();
  out.output_tokens = reader.u64();
  out.error = reader.text();
  out.backend_name = reader.text();
  out.backend_generation = Generation<BackendTag>(reader.u64());
  if (!reader.ok() || !reader.exhausted()) {
    error = reader.ok() ? "model_response payload has trailing bytes" : reader.error();
    return false;
  }
  return true;
}

std::vector<std::uint8_t> encode_action_completion(const ActionCompletionMessage& message) {
  PayloadWriter writer;
  writer.boolean(message.is_tool);
  const std::vector<std::uint8_t> body =
      message.is_tool ? encode_tool_response(message.tool) : encode_model_response(message.model);
  writer.u32(static_cast<std::uint32_t>(body.size()));
  std::vector<std::uint8_t> out = writer.take();
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

bool decode_action_completion(std::span<const std::uint8_t> payload,
                              ActionCompletionMessage& out, std::string& error) {
  PayloadReader reader(payload);
  out.is_tool = reader.boolean();
  const std::uint32_t size = reader.u32();
  if (!reader.ok()) {
    error = reader.error();
    return false;
  }
  if (size > reader.remaining()) {
    error = "action completion body exceeds the remaining payload";
    return false;
  }
  const std::span<const std::uint8_t> body(payload.data() + (payload.size() - reader.remaining()),
                                           size);
  const bool decoded = out.is_tool ? decode_tool_response(body, out.tool, error)
                                   : decode_model_response(body, out.model, error);
  return decoded;
}

}  // namespace agent_runtime::distributed
