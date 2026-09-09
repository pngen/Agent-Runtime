// Agent Runtime - reference agent worker (independent OS process).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// The agent worker holds one agent/runtime incarnation. It receives requests
// from the coordinator, forwards them to independent tool/model worker
// processes and reports the result. When a downstream worker disappears after a
// side effect, the agent worker reports AMBIGUOUS rather than inventing an
// outcome.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "agent_runtime/agent_runtime.hpp"
#include "agent_runtime/distributed/protocol.hpp"
#include "agent_runtime/distributed/transport.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using namespace agent_runtime;
using namespace agent_runtime::distributed;

namespace {

struct Endpoint {
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;
};

struct Options {
  Endpoint coordinator;
  Endpoint tool;
  Endpoint crash_tool;
  std::string crash_tool_name = "commit-token";
  Endpoint model;
  std::uint64_t runtime_boot = 1;
  std::uint64_t agent_boot = 1;
  std::uint64_t runtime_epoch = 1;
  std::uint64_t coordinator_epoch = 1;
  AgentRuntimeId runtime_id{};
  AgentId agent_id{};
  std::uint64_t request_limit = 0;
};

[[nodiscard]] bool parse_endpoint(const std::string& text, Endpoint& endpoint) {
  const std::size_t separator = text.rfind(':');
  if (separator == std::string::npos) {
    return false;
  }
  endpoint.host = text.substr(0, separator);
  endpoint.port = static_cast<std::uint16_t>(std::strtoul(text.c_str() + separator + 1, nullptr, 10));
  return endpoint.port != 0;
}

[[nodiscard]] bool parse(int argc, char** argv, Options& options, std::string& error) {
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    const bool has_value = i + 1 < argc;
    if (argument == "--coordinator" && has_value) {
      if (!parse_endpoint(argv[++i], options.coordinator)) {
        error = "invalid coordinator endpoint";
        return false;
      }
    } else if (argument == "--tool" && has_value) {
      if (!parse_endpoint(argv[++i], options.tool)) {
        error = "invalid tool endpoint";
        return false;
      }
    } else if (argument == "--crash-tool" && has_value) {
      if (!parse_endpoint(argv[++i], options.crash_tool)) {
        error = "invalid crash tool endpoint";
        return false;
      }
    } else if (argument == "--crash-tool-name" && has_value) {
      options.crash_tool_name = argv[++i];
    } else if (argument == "--model" && has_value) {
      if (!parse_endpoint(argv[++i], options.model)) {
        error = "invalid model endpoint";
        return false;
      }
    } else if (argument == "--runtime-boot" && has_value) {
      options.runtime_boot = std::strtoull(argv[++i], nullptr, 10);
    } else if (argument == "--agent-boot" && has_value) {
      options.agent_boot = std::strtoull(argv[++i], nullptr, 10);
    } else if (argument == "--runtime-epoch" && has_value) {
      options.runtime_epoch = std::strtoull(argv[++i], nullptr, 10);
    } else if (argument == "--coordinator-epoch" && has_value) {
      options.coordinator_epoch = std::strtoull(argv[++i], nullptr, 10);
    } else if (argument == "--runtime-id" && has_value) {
      options.runtime_id = AgentRuntimeId(std::strtoull(argv[++i], nullptr, 10));
    } else if (argument == "--agent-id" && has_value) {
      options.agent_id = AgentId(std::strtoull(argv[++i], nullptr, 10));
    } else if (argument == "--request-limit" && has_value) {
      options.request_limit = std::strtoull(argv[++i], nullptr, 10);
    } else {
      error = "unknown argument: " + argument;
      return false;
    }
  }
  return options.coordinator.port != 0;
}

[[nodiscard]] ToolResponse failure_response(const ToolRequest& request, CompletionStatus status,
                                            RetryClass failure_class, std::string detail) {
  ToolResponse response;
  response.action_id = request.action_id;
  response.call_id = request.call_id;
  response.call_generation = request.call_generation;
  response.attempt_id = request.attempt_id;
  response.attempt_generation = request.attempt_generation;
  response.status = status;
  response.failure_class = failure_class;
  response.error = std::move(detail);
  response.backend_name = "agent-worker";
  return response;
}

/// Forwards one request to a worker process. A missing or dead worker is
/// reported honestly: ambiguous for side-effect classes that cannot be safely
/// repeated, and a retryable tool-unavailable failure otherwise.
[[nodiscard]] ToolResponse forward_tool(const Endpoint& endpoint, const ToolRequest& request,
                                        MessageType request_type, MessageType result_type) {
  std::string error;
  auto connection = std::make_shared<TcpConnection>();
  if (!connection->connect(endpoint.host, endpoint.port, error)) {
    if (requires_reconciliation_on_ambiguity(request.side_effect)) {
      return failure_response(request, CompletionStatus::AMBIGUOUS, RetryClass::AMBIGUOUS_COMPLETION,
                              "tool worker is unreachable: " + error);
    }
    return failure_response(request, CompletionStatus::FAILED, RetryClass::TOOL_UNAVAILABLE, error);
  }
  FrameSession session(connection, 16);
  if (!session.start(error)) {
    return failure_response(request, CompletionStatus::FAILED, RetryClass::TOOL_UNAVAILABLE, error);
  }
  Frame frame;
  frame.type = request_type;
  frame.correlation = CorrelationId(request.attempt_id.value());
  frame.payload = encode_tool_request(request);
  if (!session.send(frame, error)) {
    session.stop();
    return failure_response(request, CompletionStatus::FAILED, RetryClass::TOOL_UNAVAILABLE, error);
  }
  Frame reply;
  if (!session.receive(reply)) {
    session.stop();
    if (requires_reconciliation_on_ambiguity(request.side_effect)) {
      return failure_response(request, CompletionStatus::AMBIGUOUS, RetryClass::AMBIGUOUS_COMPLETION,
                              "tool worker disappeared before reporting the result");
    }
    return failure_response(request, CompletionStatus::FAILED, RetryClass::TOOL_UNAVAILABLE,
                            "tool worker disappeared before reporting the result");
  }
  session.stop();
  if (reply.type != result_type) {
    return failure_response(request, CompletionStatus::FAILED, RetryClass::TOOL_UNAVAILABLE,
                            "unexpected reply type from tool worker");
  }
  ToolResponse response;
  if (!decode_tool_response(reply.payload, response, error)) {
    return failure_response(request, CompletionStatus::FAILED, RetryClass::TOOL_UNAVAILABLE, error);
  }
  return response;
}

[[nodiscard]] ModelResponse forward_model(const Endpoint& endpoint, const ModelRequest& request) {
  ModelResponse response;
  response.action_id = request.action_id;
  response.call_id = request.call_id;
  response.call_generation = request.call_generation;
  response.attempt_id = request.attempt_id;
  response.attempt_generation = request.attempt_generation;
  std::string error;
  auto connection = std::make_shared<TcpConnection>();
  if (!connection->connect(endpoint.host, endpoint.port, error)) {
    response.status = CompletionStatus::FAILED;
    response.failure_class = RetryClass::MODEL_UNAVAILABLE;
    response.error = "model worker is unreachable: " + error;
    return response;
  }
  FrameSession session(connection, 16);
  if (!session.start(error)) {
    response.status = CompletionStatus::FAILED;
    response.failure_class = RetryClass::MODEL_UNAVAILABLE;
    response.error = error;
    return response;
  }
  Frame frame;
  frame.type = MessageType::MODEL_REQUEST;
  frame.correlation = CorrelationId(request.attempt_id.value());
  frame.payload = encode_model_request(request);
  if (!session.send(frame, error)) {
    session.stop();
    response.status = CompletionStatus::FAILED;
    response.failure_class = RetryClass::MODEL_UNAVAILABLE;
    response.error = error;
    return response;
  }
  Frame reply;
  if (!session.receive(reply) || reply.type != MessageType::MODEL_RESULT) {
    session.stop();
    response.status = CompletionStatus::FAILED;
    response.failure_class = RetryClass::MODEL_UNAVAILABLE;
    response.error = "model worker disappeared before reporting the result";
    return response;
  }
  session.stop();
  ModelResponse decoded;
  if (!decode_model_response(reply.payload, decoded, error)) {
    response.status = CompletionStatus::FAILED;
    response.failure_class = RetryClass::MODEL_UNAVAILABLE;
    response.error = error;
    return response;
  }
  return decoded;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
  Options options;
  std::string error;
  if (!parse(argc, argv, options, error)) {
    std::fprintf(stderr, "agent worker: %s\n", error.c_str());
    return 2;
  }

  SocketSystem::ensure();
  auto connection = std::make_shared<TcpConnection>();
  if (!connection->connect(options.coordinator.host, options.coordinator.port, error)) {
    std::fprintf(stderr, "agent worker: %s\n", error.c_str());
    return 3;
  }
  FrameSession session(connection, 64);
  if (!session.start(error)) {
    std::fprintf(stderr, "agent worker: %s\n", error.c_str());
    return 4;
  }

  Frame hello_frame;
  hello_frame.type = MessageType::HELLO;
  HelloMessage hello;
  hello.role = "agent-worker";
  hello.process_id = 0;
#ifdef _WIN32
  hello.process_id = GetCurrentProcessId();
#endif
  hello_frame.payload = encode_hello(hello);
  if (!session.send(hello_frame, error)) {
    std::fprintf(stderr, "agent worker: %s\n", error.c_str());
    return 5;
  }
  Frame ack;
  if (!session.receive(ack) || ack.type != MessageType::HELLO_ACK) {
    std::fprintf(stderr, "agent worker: handshake failed\n");
    return 6;
  }

  Frame register_frame;
  register_frame.type = MessageType::REGISTER_RUNTIME;
  RegisterRuntimeMessage registration;
  registration.runtime_id = options.runtime_id;
  registration.runtime_generation = AgentRuntimeGeneration(1);
  registration.runtime_boot_id = RuntimeBootId(options.runtime_boot);
  registration.agent_id = options.agent_id;
  registration.agent_generation = AgentGeneration(1);
  registration.agent_boot_id = AgentBootId(options.agent_boot);
  registration.coordinator_epoch = CoordinatorEpoch(options.coordinator_epoch);
  registration.runtime_epoch = RuntimeEpoch(options.runtime_epoch);
  register_frame.payload = encode_register_runtime(registration);
  if (!session.send(register_frame, error)) {
    std::fprintf(stderr, "agent worker: %s\n", error.c_str());
    return 7;
  }
  std::printf("REGISTERED %llu %llu\n", static_cast<unsigned long long>(options.runtime_boot),
              static_cast<unsigned long long>(options.agent_boot));
  std::fflush(stdout);

  std::uint64_t served = 0;
  for (;;) {
    Frame frame;
    if (!session.receive(frame)) {
      break;
    }
    if (frame.type == MessageType::HEARTBEAT) {
      Frame reply;
      reply.type = MessageType::HEARTBEAT;
      reply.correlation = frame.correlation;
      (void)session.send(reply, error);
      continue;
    }
    if (frame.type == MessageType::QUERY) {
      QueryMessage query;
      if (!decode_query(frame.payload, query, error)) {
        continue;
      }
      Frame reply;
      reply.type = MessageType::QUERY_RESULT;
      reply.correlation = frame.correlation;
      QueryResultMessage result;
      result.subject = query.subject;
      result.json = std::string("{\"worker\":\"agent\",\"runtime_boot\":") +
                    std::to_string(options.runtime_boot) +
                    ",\"agent_boot\":" + std::to_string(options.agent_boot) +
                    ",\"served\":" + std::to_string(served) + "}";
      reply.payload = encode_query_result(result);
      (void)session.send(reply, error);
      continue;
    }
    if (frame.type == MessageType::TOOL_REQUEST) {
      ToolRequest request;
      if (!decode_tool_request(frame.payload, request, error)) {
        continue;
      }
      const Endpoint& endpoint =
          (options.crash_tool.port != 0 && request.tool_name == options.crash_tool_name)
              ? options.crash_tool
              : options.tool;
      ToolResponse response = forward_tool(endpoint, request, MessageType::TOOL_REQUEST,
                                           MessageType::TOOL_RESULT);
      Frame reply;
      reply.type = MessageType::TOOL_RESULT;
      reply.correlation = frame.correlation;
      reply.payload = encode_tool_response(response);
      if (!session.send(reply, error)) {
        break;
      }
      ++served;
      continue;
    }
    if (frame.type == MessageType::MODEL_REQUEST) {
      ModelRequest request;
      if (!decode_model_request(frame.payload, request, error)) {
        continue;
      }
      ModelResponse response = forward_model(options.model, request);
      Frame reply;
      reply.type = MessageType::MODEL_RESULT;
      reply.correlation = frame.correlation;
      reply.payload = encode_model_response(response);
      if (!session.send(reply, error)) {
        break;
      }
      ++served;
      continue;
    }
    Frame reply;
    reply.type = MessageType::ERROR_MESSAGE;
    reply.correlation = frame.correlation;
    ErrorMessage message;
    message.code = 2;
    message.detail = "agent worker does not accept " + std::string(to_string(frame.type));
    reply.payload = encode_error(message);
    if (!session.send(reply, error)) {
      break;
    }
  }
  session.stop();
  return 0;
}
