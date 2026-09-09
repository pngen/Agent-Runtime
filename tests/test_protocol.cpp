// Agent Runtime - framed protocol and loopback transport tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "agent_runtime/distributed/protocol.hpp"
#include "agent_runtime/distributed/transport.hpp"
#include "test_framework.hpp"

using namespace agent_runtime;
using namespace agent_runtime::distributed;

namespace {

[[nodiscard]] Frame make_frame(MessageType type, std::string payload) {
  Frame frame;
  frame.type = type;
  frame.correlation = CorrelationId(7);
  frame.payload.assign(payload.begin(), payload.end());
  return frame;
}

}  // namespace

AR_TEST(protocol, frame_round_trip) {
  const Frame original = make_frame(MessageType::TOOL_REQUEST, "payload-bytes");
  const std::vector<std::uint8_t> bytes = encode_frame(original, default_max_payload_bytes);
  AR_CHECK(!bytes.empty());
  Frame decoded;
  std::string error;
  AR_REQUIRE_MSG(decode_frame(bytes, decoded, error), error);
  AR_CHECK_EQ(std::string(to_string(decoded.type)), std::string("TOOL_REQUEST"));
  AR_CHECK_EQ(decoded.correlation.value(), 7ull);
  AR_CHECK(decoded.payload == original.payload);
}

AR_TEST(protocol, malformed_frames_are_rejected) {
  const std::vector<std::uint8_t> good =
      encode_frame(make_frame(MessageType::QUERY, "abc"), default_max_payload_bytes);
  Frame decoded;
  std::string error;

  {
    std::vector<std::uint8_t> bytes = good;
    bytes[0] = 'X';
    AR_CHECK_MSG(!decode_frame(bytes, decoded, error), "bad magic must be rejected");
  }
  {
    std::vector<std::uint8_t> bytes = good;
    bytes[4] = 0xFF;
    AR_CHECK_MSG(!decode_frame(bytes, decoded, error), "unsupported version must be rejected");
  }
  {
    std::vector<std::uint8_t> bytes = good;
    bytes[6] = 0xFF;
    bytes[7] = 0x7F;
    AR_CHECK_MSG(!decode_frame(bytes, decoded, error), "unknown message type must be rejected");
  }
  {
    std::vector<std::uint8_t> bytes = good;
    bytes[20] = 0xFF;
    AR_CHECK_MSG(!decode_frame(bytes, decoded, error),
                 "declared length mismatch must be rejected");
  }
  {
    std::vector<std::uint8_t> bytes = good;
    bytes[frame_header_bytes] ^= 0x5A;
    AR_CHECK_MSG(!decode_frame(bytes, decoded, error), "payload corruption must be rejected");
  }
  {
    std::vector<std::uint8_t> bytes = good;
    bytes[12] ^= 0x5A;
    AR_CHECK_MSG(!decode_frame(bytes, decoded, error), "header corruption must be rejected");
  }
  {
    std::vector<std::uint8_t> bytes = good;
    bytes.push_back(0);
    AR_CHECK_MSG(!decode_frame(bytes, decoded, error), "trailing garbage must be rejected");
  }
  {
    std::vector<std::uint8_t> bytes(good.begin(), good.end() - 1);
    AR_CHECK_MSG(!decode_frame(bytes, decoded, error), "truncation must be rejected");
  }
  {
    std::vector<std::uint8_t> bytes(frame_header_bytes + frame_trailer_bytes, 0);
    AR_CHECK_MSG(!decode_frame(bytes, decoded, error), "empty frame must be rejected");
  }
  {
    Frame oversized = make_frame(MessageType::QUERY, std::string(64, 'x'));
    AR_CHECK_MSG(encode_frame(oversized, 16).empty(),
                 "oversized payload must not encode under a small bound");
    const std::vector<std::uint8_t> bytes = encode_frame(oversized, default_max_payload_bytes);
    AR_CHECK_MSG(!decode_frame(bytes, decoded, error, 16),
                 "oversized payload must be rejected at decode");
  }
}

AR_TEST(protocol, payload_reader_bounds) {
  PayloadWriter writer;
  writer.u8(1);
  writer.u16(2);
  writer.u32(3);
  writer.u64(4);
  writer.boolean(true);
  writer.text("text");
  const std::vector<std::uint8_t> bytes = writer.bytes();
  PayloadReader reader(bytes);
  AR_CHECK_EQ(reader.u8(), 1);
  AR_CHECK_EQ(reader.u16(), 2);
  AR_CHECK_EQ(reader.u32(), 3);
  AR_CHECK_EQ(reader.u64(), 4ull);
  AR_CHECK(reader.boolean());
  AR_CHECK_EQ(reader.text(), std::string("text"));
  AR_CHECK(reader.ok());
  AR_CHECK(reader.exhausted());

  const std::vector<std::uint8_t> truncated(bytes.begin(), bytes.end() - 2);
  PayloadReader short_reader(truncated);
  short_reader.u8();
  short_reader.u16();
  short_reader.u32();
  short_reader.u64();
  short_reader.boolean();
  short_reader.text();
  AR_CHECK(!short_reader.ok());
  AR_CHECK(!short_reader.error().empty());

  PayloadWriter huge;
  huge.text(std::string(64, 'x'));
  PayloadReader bounded(huge.bytes());
  bounded.text(8);
  AR_CHECK(!bounded.ok());
}

AR_TEST(protocol, typed_message_round_trips) {
  HelloMessage hello;
  hello.role = "agent-worker";
  hello.process_id = 4242;
  HelloMessage decoded_hello;
  std::string error;
  AR_REQUIRE(decode_hello(encode_hello(hello), decoded_hello, error));
  AR_CHECK_EQ(decoded_hello.role, std::string("agent-worker"));
  AR_CHECK_EQ(decoded_hello.process_id, 4242ull);

  RegisterRuntimeMessage registration;
  registration.runtime_id = AgentRuntimeId(1);
  registration.runtime_boot_id = RuntimeBootId(2);
  registration.agent_id = AgentId(3);
  registration.agent_boot_id = AgentBootId(4);
  RegisterRuntimeMessage decoded_registration;
  AR_REQUIRE(decode_register_runtime(encode_register_runtime(registration), decoded_registration,
                                     error));
  AR_CHECK_EQ(decoded_registration.runtime_id.value(), 1ull);
  AR_CHECK_EQ(decoded_registration.agent_boot_id.value(), 4ull);

  ToolRequest request;
  request.call_id = ToolCallId(11);
  request.action_id = ActionId(12);
  request.attempt_id = ActionAttemptId(13);
  request.attempt_generation = AttemptGeneration(14);
  request.tool_name = "hash";
  request.side_effect = SideEffectClass::PURE;
  request.input = "input";
  request.max_output_bytes = 1024;
  ToolRequest decoded_request;
  AR_REQUIRE(decode_tool_request(encode_tool_request(request), decoded_request, error));
  AR_CHECK_EQ(decoded_request.tool_name, std::string("hash"));
  AR_CHECK_EQ(decoded_request.attempt_id.value(), 13ull);
  AR_CHECK_EQ(std::string(to_string(decoded_request.side_effect)), std::string("PURE"));

  ToolResponse response;
  response.call_id = ToolCallId(11);
  response.attempt_id = ActionAttemptId(13);
  response.status = CompletionStatus::SUCCEEDED;
  response.output = "output";
  response.backend_generation = Generation<BackendTag>(3);
  ToolResponse decoded_response;
  AR_REQUIRE(decode_tool_response(encode_tool_response(response), decoded_response, error));
  AR_CHECK_EQ(decoded_response.output, std::string("output"));
  AR_CHECK_EQ(decoded_response.backend_generation.value(), 3ull);

  ModelRequest model_request;
  model_request.target = "reference-target";
  model_request.prompt = "prompt";
  ModelRequest decoded_model_request;
  AR_REQUIRE(decode_model_request(encode_model_request(model_request), decoded_model_request, error));
  AR_CHECK_EQ(decoded_model_request.target, std::string("reference-target"));

  QueryResultMessage query_result;
  query_result.subject = "summary";
  query_result.json = "{}";
  QueryResultMessage decoded_query_result;
  AR_REQUIRE(decode_query_result(encode_query_result(query_result), decoded_query_result, error));
  AR_CHECK_EQ(decoded_query_result.subject, std::string("summary"));

  ActionCompletionMessage completion;
  completion.is_tool = true;
  completion.tool = response;
  ActionCompletionMessage decoded_completion;
  AR_REQUIRE(decode_action_completion(encode_action_completion(completion), decoded_completion,
                                      error));
  AR_CHECK(decoded_completion.is_tool);
  AR_CHECK_EQ(decoded_completion.tool.output, std::string("output"));

  // Trailing bytes in a typed payload must be rejected.
  std::vector<std::uint8_t> with_trailing = encode_hello(hello);
  with_trailing.push_back(0);
  AR_CHECK(!decode_hello(with_trailing, decoded_hello, error));
}

AR_TEST(protocol, invalid_enum_payload_is_rejected) {
  PayloadWriter writer;
  for (int i = 0; i < 7; ++i) {
    writer.u64(1);
  }
  writer.text("tool");
  writer.u16(0xFFFF);
  std::string error;
  ToolRequest request;
  AR_CHECK(!decode_tool_request(writer.bytes(), request, error));
  AR_CHECK(!error.empty());
}

AR_TEST(transport, loopback_send_and_receive) {
  SocketSystem::ensure();
  TcpListener listener;
  std::string error;
  AR_REQUIRE_MSG(listener.listen_loopback(0, error), error);
  AR_CHECK(listener.port() != 0);

  std::shared_ptr<TcpConnection> client = std::make_shared<TcpConnection>();
  std::atomic<bool> connected{false};
  std::thread connector([&] {
    std::string connect_error;
    if (client->connect("127.0.0.1", listener.port(), connect_error)) {
      connected.store(true, std::memory_order_release);
    }
  });
  std::shared_ptr<TcpConnection> server = listener.accept(error);
  connector.join();
  AR_REQUIRE_MSG(server != nullptr, error);
  AR_REQUIRE(connected.load(std::memory_order_acquire));

  Frame outgoing = make_frame(MessageType::HEARTBEAT, "ping");
  AR_REQUIRE_MSG(client->send_frame(outgoing, error), error);
  Frame incoming;
  AR_REQUIRE_MSG(server->recv_frame(incoming, error), error);
  AR_CHECK_EQ(std::string(to_string(incoming.type)), std::string("HEARTBEAT"));
  AR_CHECK(incoming.payload == outgoing.payload);

  server->close();
  client->close();
  listener.close();
}

AR_TEST(transport, blocked_receive_returns_when_closed) {
  SocketSystem::ensure();
  TcpListener listener;
  std::string error;
  AR_REQUIRE(listener.listen_loopback(0, error));
  std::shared_ptr<TcpConnection> client = std::make_shared<TcpConnection>();
  std::thread connector([&] {
    std::string connect_error;
    (void)client->connect("127.0.0.1", listener.port(), connect_error);
  });
  std::shared_ptr<TcpConnection> server = listener.accept(error);
  connector.join();
  AR_REQUIRE(server != nullptr);

  std::atomic<bool> returned{false};
  std::thread blocked([&] {
    Frame frame;
    std::string receive_error;
    (void)server->recv_frame(frame, receive_error);
    returned.store(true, std::memory_order_release);
  });
  // Closing the handle interrupts the blocked receive. No timeout is used:
  // the test blocks until the worker thread observes the closure.
  server->close();
  blocked.join();
  AR_CHECK(returned.load(std::memory_order_acquire));
  client->close();
  listener.close();
}

AR_TEST(transport, session_delivers_frames_and_stops_cleanly) {
  SocketSystem::ensure();
  TcpListener listener;
  std::string error;
  AR_REQUIRE(listener.listen_loopback(0, error));
  std::shared_ptr<TcpConnection> client = std::make_shared<TcpConnection>();
  std::thread connector([&] {
    std::string connect_error;
    (void)client->connect("127.0.0.1", listener.port(), connect_error);
  });
  std::shared_ptr<TcpConnection> server = listener.accept(error);
  connector.join();
  AR_REQUIRE(server != nullptr);

  auto session = std::make_shared<FrameSession>(server, 8);
  AR_REQUIRE_MSG(session->start(error), error);
  for (int i = 0; i < 4; ++i) {
    AR_REQUIRE(client->send_frame(make_frame(MessageType::HEARTBEAT, std::to_string(i)), error));
  }
  for (int i = 0; i < 4; ++i) {
    Frame frame;
    AR_REQUIRE(session->receive(frame));
    AR_CHECK_EQ(std::string(frame.payload.begin(), frame.payload.end()), std::to_string(i));
  }
  session->stop();
  AR_CHECK(!session->open());
  client->close();
  listener.close();
}

AR_TEST(transport, session_overflow_closes_rather_than_growing_unbounded) {
  SocketSystem::ensure();
  TcpListener listener;
  std::string error;
  AR_REQUIRE(listener.listen_loopback(0, error));
  std::shared_ptr<TcpConnection> client = std::make_shared<TcpConnection>();
  std::thread connector([&] {
    std::string connect_error;
    (void)client->connect("127.0.0.1", listener.port(), connect_error);
  });
  std::shared_ptr<TcpConnection> server = listener.accept(error);
  connector.join();
  AR_REQUIRE(server != nullptr);

  auto session = std::make_shared<FrameSession>(server, 2);
  AR_REQUIRE(session->start(error));
  // Nothing consumes the queue, so the reader must close the session once the
  // bound is exceeded instead of growing without limit. The check polls
  // immediately rather than waiting on a timer.
  for (int i = 0; i < 64; ++i) {
    if (!client->send_frame(make_frame(MessageType::HEARTBEAT, "x"), error)) {
      break;
    }
  }
  for (std::uint64_t spin = 0; spin < 100000000ull && session->open(); ++spin) {
  }
  AR_CHECK_MSG(!session->open(), "an overflowing session must close");
  AR_CHECK(!session->last_error().empty());
  // Whatever was queued before closure is still drainable.
  Frame frame;
  while (session->try_receive(frame)) {
  }
  session->stop();
  client->close();
  listener.close();
}

AR_TEST(transport, session_destruction_from_reader_thread_is_safe) {
  SocketSystem::ensure();
  TcpListener listener;
  std::string error;
  AR_REQUIRE(listener.listen_loopback(0, error));
  std::shared_ptr<TcpConnection> client = std::make_shared<TcpConnection>();
  std::thread connector([&] {
    std::string connect_error;
    (void)client->connect("127.0.0.1", listener.port(), connect_error);
  });
  std::shared_ptr<TcpConnection> server = listener.accept(error);
  connector.join();
  AR_REQUIRE(server != nullptr);

  // The session is destroyed while its reader thread is blocked in recv. The
  // destructor must stop the reader without self-joining.
  {
    FrameSession session(server, 4);
    AR_REQUIRE(session.start(error));
    AR_REQUIRE(client->send_frame(make_frame(MessageType::HEARTBEAT, "x"), error));
    Frame frame;
    AR_REQUIRE(session.receive(frame));
  }
  AR_CHECK(!server->open());
  client->close();
  listener.close();
}

AR_TEST(transport, repeated_connect_and_close_is_safe) {
  SocketSystem::ensure();
  TcpListener listener;
  std::string error;
  AR_REQUIRE(listener.listen_loopback(0, error));
  for (int i = 0; i < 16; ++i) {
    std::shared_ptr<TcpConnection> client = std::make_shared<TcpConnection>();
    std::thread connector([&] {
      std::string connect_error;
      (void)client->connect("127.0.0.1", listener.port(), connect_error);
    });
    std::shared_ptr<TcpConnection> server = listener.accept(error);
    connector.join();
    AR_REQUIRE(server != nullptr);
    client->close();
    server->close();
  }
  listener.close();
}
