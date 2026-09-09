// Agent Runtime - real loopback TCP transport for the reference architecture.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef AGENT_RUNTIME_DISTRIBUTED_TRANSPORT_HPP
#define AGENT_RUNTIME_DISTRIBUTED_TRANSPORT_HPP

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "agent_runtime/distributed/protocol.hpp"

namespace agent_runtime::distributed {

/// Process-wide socket subsystem lifetime. Safe to call from many threads.
class SocketSystem {
 public:
  static void ensure();
  static void shutdown();
};

/// Winsock/BSD socket handle wrapper. close() interrupts a blocked recv by
/// shutting the socket down and then closing the handle; the reference tests
/// prove that a blocked receive returns rather than hanging.
class TcpConnection {
 public:
  TcpConnection() = default;
  ~TcpConnection();
  TcpConnection(const TcpConnection&) = delete;
  TcpConnection& operator=(const TcpConnection&) = delete;

  [[nodiscard]] bool connect(const std::string& host, std::uint16_t port, std::string& error);
  void adopt(std::uintptr_t native_handle);

  [[nodiscard]] bool open() const noexcept { return handle_ != kInvalidHandle; }
  void close() noexcept;

  /// Sends one complete frame. Serialized internally: concurrent senders never
  /// interleave bytes.
  [[nodiscard]] bool send_frame(const Frame& frame, std::string& error,
                                std::uint32_t max_payload_bytes = default_max_payload_bytes);
  /// Receives exactly one frame.
  [[nodiscard]] bool recv_frame(Frame& frame, std::string& error,
                                std::uint32_t max_payload_bytes = default_max_payload_bytes);

  [[nodiscard]] std::string peer() const { return peer_; }

 private:
  static constexpr std::uintptr_t kInvalidHandle = static_cast<std::uintptr_t>(~0ull);

  std::uintptr_t handle_ = kInvalidHandle;
  std::string peer_;
  mutable std::mutex write_mutex_;
};

/// Listens on the loopback interface only.
class TcpListener {
 public:
  TcpListener() = default;
  ~TcpListener();
  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;

  [[nodiscard]] bool listen_loopback(std::uint16_t port, std::string& error);
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] std::shared_ptr<TcpConnection> accept(std::string& error);
  void close() noexcept;

 private:
  std::uintptr_t handle_ = static_cast<std::uintptr_t>(~0ull);
  std::uint16_t port_ = 0;
};

/// Framed session: one reader thread plus a bounded receive queue. The reader
/// thread never owns the session, so destroying the session from any thread
/// (including the reader itself) is safe.
class FrameSession {
 public:
  FrameSession(std::shared_ptr<TcpConnection> connection, std::uint32_t max_queue,
               std::uint32_t max_payload_bytes = default_max_payload_bytes);
  ~FrameSession();
  FrameSession(const FrameSession&) = delete;
  FrameSession& operator=(const FrameSession&) = delete;

  [[nodiscard]] bool start(std::string& error);
  /// Stops the reader and closes the connection. Safe to call repeatedly and
  /// safe to call from the reader thread itself.
  void stop() noexcept;

  [[nodiscard]] bool send(const Frame& frame, std::string& error);
  /// Blocks until a frame is available or the session closes.
  [[nodiscard]] bool receive(Frame& frame);
  /// Returns false immediately when no frame is queued.
  [[nodiscard]] bool try_receive(Frame& frame);

  [[nodiscard]] bool open() const noexcept { return open_.load(std::memory_order_acquire); }
  [[nodiscard]] std::size_t dropped_frames() const noexcept {
    return dropped_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::string last_error() const;

 private:
  void reader_main();

  std::shared_ptr<TcpConnection> connection_;
  std::uint32_t max_queue_;
  std::uint32_t max_payload_bytes_;
  std::thread reader_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<Frame> queue_;
  std::atomic<bool> open_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<std::size_t> dropped_{0};
  std::string last_error_;
};

}  // namespace agent_runtime::distributed

#endif  // AGENT_RUNTIME_DISTRIBUTED_TRANSPORT_HPP
