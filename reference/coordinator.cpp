// Agent Runtime - reference multiprocess coordinator implementation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "coordinator.hpp"

#include <utility>

namespace agent_runtime::reference {

ReferenceCoordinator::ReferenceCoordinator(CoordinatorOptions options)
    : options_(std::move(options)) {
  runtime_ = std::make_unique<AgentRuntime>(options_.runtime);
}

ReferenceCoordinator::~ReferenceCoordinator() {
  drop_agent();
  listener_.close();
}

bool ReferenceCoordinator::start(std::string& error) {
  distributed::SocketSystem::ensure();
  return listener_.listen_loopback(options_.listen_port, error);
}

bool ReferenceCoordinator::accept_agent(std::string& error) {
  drop_agent();
  std::shared_ptr<distributed::TcpConnection> connection = listener_.accept(error);
  if (connection == nullptr) {
    return false;
  }
  session_ = std::make_shared<distributed::FrameSession>(connection, options_.max_receive_queue,
                                                         options_.max_payload_bytes);
  if (!session_->start(error)) {
    session_.reset();
    return false;
  }
  // HELLO handshake. No runtime lock is held here.
  distributed::Frame frame;
  if (!session_->receive(frame)) {
    error = "agent worker closed before HELLO";
    drop_agent();
    return false;
  }
  if (frame.type != distributed::MessageType::HELLO) {
    error = "expected HELLO from agent worker";
    drop_agent();
    return false;
  }
  distributed::HelloMessage hello;
  if (!distributed::decode_hello(frame.payload, hello, error)) {
    drop_agent();
    return false;
  }
  if (hello.version != distributed::protocol_version) {
    error = "agent worker protocol version is unsupported";
    drop_agent();
    return false;
  }
  distributed::Frame ack;
  ack.type = distributed::MessageType::HELLO_ACK;
  ack.correlation = frame.correlation;
  distributed::HelloMessage ack_body;
  ack_body.role = "coordinator";
  ack_body.version = distributed::protocol_version;
  ack_body.process_id = 0;
  ack.payload = distributed::encode_hello(ack_body);
  if (!session_->send(ack, error)) {
    drop_agent();
    return false;
  }
  handshake_done_ = true;
  return true;
}

bool ReferenceCoordinator::await_registration(distributed::RegisterRuntimeMessage& registration,
                                              std::string& error) {
  if (session_ == nullptr) {
    error = "no agent worker is connected";
    return false;
  }
  distributed::Frame frame;
  if (!session_->receive(frame)) {
    error = "agent worker closed before registering the runtime";
    return false;
  }
  if (frame.type != distributed::MessageType::REGISTER_RUNTIME) {
    error = "expected REGISTER_RUNTIME from agent worker";
    return false;
  }
  return distributed::decode_register_runtime(frame.payload, registration, error);
}

void ReferenceCoordinator::drop_agent() noexcept {
  if (session_ != nullptr) {
    session_->stop();
    session_.reset();
  }
  handshake_done_ = false;
}

bool ReferenceCoordinator::send_to_agent(const distributed::Frame& frame, std::string& error) {
  if (session_ == nullptr) {
    error = "no agent worker is connected";
    return false;
  }
  return session_->send(frame, error);
}

bool ReferenceCoordinator::receive_from_agent(distributed::Frame& frame, std::string& error) {
  if (session_ == nullptr) {
    error = "no agent worker is connected";
    return false;
  }
  if (!session_->receive(frame)) {
    error = "agent worker session closed";
    return false;
  }
  return true;
}

bool ReferenceCoordinator::try_receive_from_agent(distributed::Frame& frame) {
  return session_ != nullptr && session_->try_receive(frame);
}

bool ReferenceCoordinator::dispatch_tool_action(const ActionSpec& spec, ActionHandle& out,
                                                DispatchResult& dispatched, std::string& error) {
  const MutationResult declared = runtime_->declare_action(spec, out);
  if (!declared.accepted()) {
    error = "declare_action: " + declared.to_text();
    return false;
  }
  const MutationResult admitted = runtime_->admit_action(out.action_id, out.action_generation);
  if (!admitted.accepted()) {
    error = "admit_action: " + admitted.to_text();
    return false;
  }
  const MutationResult authorized = runtime_->authorize_action(out.action_id, out.action_generation);
  if (!authorized.accepted()) {
    error = "authorize_action: " + authorized.to_text();
    return false;
  }
  dispatched = runtime_->dispatch_action(out.action_id, out.action_generation,
                                         DispatchMode::EXTERNAL);
  if (!dispatched.result.accepted() || !dispatched.has_tool_request) {
    error = "dispatch_action: " + dispatched.result.to_text();
    return false;
  }
  distributed::Frame frame;
  frame.type = distributed::MessageType::TOOL_REQUEST;
  frame.correlation = CorrelationId(dispatched.attempt_id.value());
  frame.payload = distributed::encode_tool_request(dispatched.tool_request);
  return send_to_agent(frame, error);
}

bool ReferenceCoordinator::await_tool_result(const DispatchResult& dispatched,
                                             ToolResponse& response, std::string& error) {
  for (;;) {
    distributed::Frame frame;
    if (!receive_from_agent(frame, error)) {
      return false;
    }
    if (frame.type == distributed::MessageType::HEARTBEAT) {
      continue;
    }
    if (frame.type != distributed::MessageType::TOOL_RESULT &&
        frame.type != distributed::MessageType::ACTION_COMPLETION) {
      error = "unexpected message " + std::string(distributed::to_string(frame.type)) +
              " while awaiting a tool result";
      return false;
    }
    ToolResponse candidate;
    if (frame.type == distributed::MessageType::TOOL_RESULT) {
      if (!distributed::decode_tool_response(frame.payload, candidate, error)) {
        return false;
      }
    } else {
      distributed::ActionCompletionMessage completion;
      if (!distributed::decode_action_completion(frame.payload, completion, error)) {
        return false;
      }
      candidate = completion.tool;
    }
    if (candidate.attempt_id != dispatched.attempt_id) {
      error = "tool result does not match the reserved attempt";
      return false;
    }
    response = candidate;
    return true;
  }
}

MutationResult ReferenceCoordinator::complete_and_commit(ActionId action_id,
                                                         ActionGeneration action_generation,
                                                         const ToolResponse& response) {
  const MutationResult completion = runtime_->submit_tool_completion(response);
  if (!completion.accepted()) {
    return completion;
  }
  return runtime_->commit_action(action_id, action_generation);
}

}  // namespace agent_runtime::reference
