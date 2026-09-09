// Agent Runtime - bounded versioned framed protocol.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef AGENT_RUNTIME_DISTRIBUTED_PROTOCOL_HPP
#define AGENT_RUNTIME_DISTRIBUTED_PROTOCOL_HPP

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "agent_runtime/agent_runtime.hpp"

namespace agent_runtime::distributed {

inline constexpr std::string_view protocol_magic = "ARPF";
inline constexpr std::uint16_t protocol_version = 1;
/// magic u32 | version u16 | type u16 | flags u32 | correlation u64
/// | payload_bytes u32 | header_crc32c u32  == 28 bytes
inline constexpr std::size_t frame_header_bytes = 28;
inline constexpr std::size_t frame_trailer_bytes = 4;
inline constexpr std::uint32_t default_max_payload_bytes = 1024 * 1024;

enum class MessageType : std::uint16_t {
  HELLO = 1,
  HELLO_ACK = 2,
  REGISTER_RUNTIME = 3,
  REGISTER_BACKEND = 4,
  START_RUN = 5,
  SUSPEND_RUN = 6,
  RESUME_RUN = 7,
  CANCEL_RUN = 8,
  DRAIN_RUN = 9,
  DECLARE_ACTION = 10,
  AUTHORIZE_ACTION = 11,
  DISPATCH_ACTION = 12,
  MODEL_REQUEST = 13,
  MODEL_RESULT = 14,
  TOOL_REQUEST = 15,
  TOOL_RESULT = 16,
  CHECKPOINT_REQUEST = 17,
  CHECKPOINT_RESULT = 18,
  ACTION_COMPLETION = 19,
  ACTION_FAILURE = 20,
  HEARTBEAT = 21,
  QUERY = 22,
  QUERY_RESULT = 23,
  ERROR_MESSAGE = 24,

  kCount
};

[[nodiscard]] std::string_view to_string(MessageType type) noexcept;

struct Frame {
  MessageType type = MessageType::ERROR_MESSAGE;
  std::uint32_t flags = 0;
  CorrelationId correlation{};
  std::vector<std::uint8_t> payload;
};

/// Encodes a frame. Returns an empty vector when the frame cannot be encoded
/// within the bound (oversized payload) or the type is unknown.
[[nodiscard]] std::vector<std::uint8_t> encode_frame(const Frame& frame,
                                                     std::uint32_t max_payload_bytes);

/// Decodes exactly one frame from a complete buffer. Rejects malformed magic,
/// unsupported protocol version, unknown message type, inconsistent declared
/// length, integrity mismatch and trailing bytes.
[[nodiscard]] bool decode_frame(std::span<const std::uint8_t> bytes, Frame& out,
                                std::string& error,
                                std::uint32_t max_payload_bytes = default_max_payload_bytes);

/// Bounded payload writer. Every length is checked before it is written.
class PayloadWriter {
 public:
  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void boolean(bool value);
  void text(std::string_view value, std::uint32_t max_string_bytes = 4096);
  [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::vector<std::uint8_t> take() { return std::move(bytes_); }

 private:
  std::vector<std::uint8_t> bytes_;
};

/// Bounded payload reader. Every read is checked against the remaining length.
class PayloadReader {
 public:
  explicit PayloadReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }
  [[nodiscard]] bool exhausted() const noexcept { return remaining() == 0; }

  std::uint8_t u8();
  std::uint16_t u16();
  std::uint32_t u32();
  std::uint64_t u64();
  bool boolean();
  std::string text(std::uint32_t max_string_bytes = 4096);

 private:
  [[nodiscard]] bool need(std::size_t count);

  std::span<const std::uint8_t> bytes_;
  std::size_t offset_ = 0;
  bool ok_ = true;
  std::string error_;
};

// --- typed messages -------------------------------------------------------

struct HelloMessage {
  std::string role;
  std::uint16_t version = protocol_version;
  std::uint64_t process_id = 0;
};

struct RegisterRuntimeMessage {
  AgentRuntimeId runtime_id{};
  AgentRuntimeGeneration runtime_generation{};
  RuntimeBootId runtime_boot_id{};
  AgentId agent_id{};
  AgentGeneration agent_generation{};
  AgentBootId agent_boot_id{};
  CoordinatorEpoch coordinator_epoch{};
  RuntimeEpoch runtime_epoch{};
};

struct RegisterBackendMessage {
  BackendId backend_id{};
  Generation<BackendTag> generation{};
  std::string name;
  std::string kind;
  std::string endpoint;
};

struct ErrorMessage {
  std::uint16_t code = 0;
  std::string detail;
};

struct QueryMessage {
  std::string subject;
};

struct QueryResultMessage {
  std::string subject;
  std::string json;
};

struct ActionCompletionMessage {
  ToolResponse tool;
  ModelResponse model;
  bool is_tool = true;
};

[[nodiscard]] std::vector<std::uint8_t> encode_hello(const HelloMessage& message);
[[nodiscard]] bool decode_hello(std::span<const std::uint8_t> payload, HelloMessage& out,
                                std::string& error);
[[nodiscard]] std::vector<std::uint8_t> encode_register_runtime(
    const RegisterRuntimeMessage& message);
[[nodiscard]] bool decode_register_runtime(std::span<const std::uint8_t> payload,
                                           RegisterRuntimeMessage& out, std::string& error);
[[nodiscard]] std::vector<std::uint8_t> encode_register_backend(
    const RegisterBackendMessage& message);
[[nodiscard]] bool decode_register_backend(std::span<const std::uint8_t> payload,
                                           RegisterBackendMessage& out, std::string& error);
[[nodiscard]] std::vector<std::uint8_t> encode_error(const ErrorMessage& message);
[[nodiscard]] bool decode_error(std::span<const std::uint8_t> payload, ErrorMessage& out,
                                std::string& error);
[[nodiscard]] std::vector<std::uint8_t> encode_query(const QueryMessage& message);
[[nodiscard]] bool decode_query(std::span<const std::uint8_t> payload, QueryMessage& out,
                                std::string& error);
[[nodiscard]] std::vector<std::uint8_t> encode_query_result(const QueryResultMessage& message);
[[nodiscard]] bool decode_query_result(std::span<const std::uint8_t> payload,
                                       QueryResultMessage& out, std::string& error);
[[nodiscard]] std::vector<std::uint8_t> encode_tool_request(const ToolRequest& request);
[[nodiscard]] bool decode_tool_request(std::span<const std::uint8_t> payload, ToolRequest& out,
                                       std::string& error);
[[nodiscard]] std::vector<std::uint8_t> encode_tool_response(const ToolResponse& response);
[[nodiscard]] bool decode_tool_response(std::span<const std::uint8_t> payload, ToolResponse& out,
                                        std::string& error);
[[nodiscard]] std::vector<std::uint8_t> encode_model_request(const ModelRequest& request);
[[nodiscard]] bool decode_model_request(std::span<const std::uint8_t> payload, ModelRequest& out,
                                        std::string& error);
[[nodiscard]] std::vector<std::uint8_t> encode_model_response(const ModelResponse& response);
[[nodiscard]] bool decode_model_response(std::span<const std::uint8_t> payload, ModelResponse& out,
                                         std::string& error);
[[nodiscard]] std::vector<std::uint8_t> encode_action_completion(
    const ActionCompletionMessage& message);
[[nodiscard]] bool decode_action_completion(std::span<const std::uint8_t> payload,
                                            ActionCompletionMessage& out, std::string& error);

}  // namespace agent_runtime::distributed

#endif  // AGENT_RUNTIME_DISTRIBUTED_PROTOCOL_HPP
