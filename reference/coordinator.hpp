// Agent Runtime - reference multiprocess coordinator.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef AGENT_RUNTIME_REFERENCE_COORDINATOR_HPP
#define AGENT_RUNTIME_REFERENCE_COORDINATOR_HPP

#include <memory>
#include <string>

#include "agent_runtime/agent_runtime.hpp"
#include "agent_runtime/distributed/protocol.hpp"
#include "agent_runtime/distributed/transport.hpp"

namespace agent_runtime::reference {

struct CoordinatorOptions {
  AgentRuntimeOptions runtime;
  std::uint32_t max_receive_queue = 256;
  /// Loopback port to listen on. Zero selects an ephemeral port.
  std::uint16_t listen_port = 0;
  std::uint32_t max_payload_bytes = distributed::default_max_payload_bytes;
};

/// One reference coordinator. It owns exactly one AgentRuntime and one agent
/// worker session at a time. All canonical runtime locks are released before
/// any socket operation: the coordinator never holds runtime state while
/// sending or receiving.
class ReferenceCoordinator {
 public:
  explicit ReferenceCoordinator(CoordinatorOptions options);
  ~ReferenceCoordinator();
  ReferenceCoordinator(const ReferenceCoordinator&) = delete;
  ReferenceCoordinator& operator=(const ReferenceCoordinator&) = delete;

  [[nodiscard]] bool start(std::string& error);
  [[nodiscard]] std::uint16_t port() const noexcept { return listener_.port(); }
  [[nodiscard]] AgentRuntime& runtime() noexcept { return *runtime_; }
  [[nodiscard]] const AgentRuntime& runtime() const noexcept { return *runtime_; }

  /// Accepts one agent worker connection and completes the HELLO handshake.
  [[nodiscard]] bool accept_agent(std::string& error);
  /// Waits for the REGISTER_RUNTIME message of the accepted worker.
  [[nodiscard]] bool await_registration(distributed::RegisterRuntimeMessage& registration,
                                        std::string& error);
  [[nodiscard]] bool agent_open() const noexcept {
    return session_ != nullptr && session_->open();
  }
  void drop_agent() noexcept;

  [[nodiscard]] bool send_to_agent(const distributed::Frame& frame, std::string& error);
  [[nodiscard]] bool receive_from_agent(distributed::Frame& frame, std::string& error);
  [[nodiscard]] bool try_receive_from_agent(distributed::Frame& frame);

  /// Declares, admits, authorizes and dispatches a tool action, then sends the
  /// request to the connected agent worker.
  [[nodiscard]] bool dispatch_tool_action(const ActionSpec& spec, ActionHandle& out,
                                          DispatchResult& dispatched, std::string& error);
  /// Waits for the TOOL_RESULT that matches the reserved attempt.
  [[nodiscard]] bool await_tool_result(const DispatchResult& dispatched, ToolResponse& response,
                                       std::string& error);
  /// Submits the completion and commits it when the runtime accepts it.
  [[nodiscard]] MutationResult complete_and_commit(ActionId action_id,
                                                   ActionGeneration action_generation,
                                                   const ToolResponse& response);

 private:
  CoordinatorOptions options_;
  std::unique_ptr<AgentRuntime> runtime_;
  distributed::TcpListener listener_;
  std::shared_ptr<distributed::FrameSession> session_;
  bool handshake_done_ = false;
};

}  // namespace agent_runtime::reference

#endif  // AGENT_RUNTIME_REFERENCE_COORDINATOR_HPP
