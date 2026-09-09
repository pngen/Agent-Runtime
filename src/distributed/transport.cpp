// Agent Runtime - loopback TCP transport implementation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "agent_runtime/distributed/transport.hpp"

#include <array>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace agent_runtime::distributed {
namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;

[[nodiscard]] int last_socket_error() noexcept { return WSAGetLastError(); }

void close_native(NativeSocket socket) noexcept { ::closesocket(socket); }
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;

[[nodiscard]] int last_socket_error() noexcept { return errno; }

void close_native(NativeSocket socket) noexcept { ::close(socket); }
#endif

std::once_flag g_socket_once;

void initialize_sockets() {
#ifdef _WIN32
  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    return;
  }
#endif
}

[[nodiscard]] bool send_all(NativeSocket socket, const std::uint8_t* data, std::size_t size,
                            std::string& error) {
  std::size_t sent = 0;
  while (sent < size) {
    const int chunk = static_cast<int>(
        (size - sent) > 1u << 20 ? (1u << 20) : (size - sent));
    const int result = ::send(socket, reinterpret_cast<const char*>(data + sent), chunk, 0);
    if (result <= 0) {
      error = "send failed with socket error " + std::to_string(last_socket_error());
      return false;
    }
    sent += static_cast<std::size_t>(result);
  }
  return true;
}

[[nodiscard]] bool recv_all(NativeSocket socket, std::uint8_t* data, std::size_t size,
                            std::string& error) {
  std::size_t received = 0;
  while (received < size) {
    const int chunk = static_cast<int>(
        (size - received) > 1u << 20 ? (1u << 20) : (size - received));
    const int result = ::recv(socket, reinterpret_cast<char*>(data + received), chunk, 0);
    if (result == 0) {
      error = "peer closed the connection";
      return false;
    }
    if (result < 0) {
      error = "receive failed with socket error " + std::to_string(last_socket_error());
      return false;
    }
    received += static_cast<std::size_t>(result);
  }
  return true;
}

void configure_stream(NativeSocket socket) {
  const char value = 1;
  ::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value));
}

}  // namespace

void SocketSystem::ensure() { std::call_once(g_socket_once, initialize_sockets); }

void SocketSystem::shutdown() {
#ifdef _WIN32
  WSACleanup();
#endif
}

// ---------------------------------------------------------------------------
// TcpConnection
// ---------------------------------------------------------------------------

TcpConnection::~TcpConnection() { close(); }

bool TcpConnection::connect(const std::string& host, std::uint16_t port, std::string& error) {
  SocketSystem::ensure();
  if (open()) {
    error = "connection is already open";
    return false;
  }
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* results = nullptr;
  const std::string service = std::to_string(port);
  const int status = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &results);
  if (status != 0 || results == nullptr) {
    error = "address resolution failed for " + host;
    return false;
  }
  NativeSocket socket = kInvalidSocket;
  for (addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
    socket = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    if (socket == kInvalidSocket) {
      continue;
    }
    if (::connect(socket, entry->ai_addr, static_cast<int>(entry->ai_addrlen)) == 0) {
      break;
    }
    close_native(socket);
    socket = kInvalidSocket;
  }
  ::freeaddrinfo(results);
  if (socket == kInvalidSocket) {
    error = "connect failed with socket error " + std::to_string(last_socket_error());
    return false;
  }
  configure_stream(socket);
  handle_ = static_cast<std::uintptr_t>(socket);
  peer_ = host + ":" + service;
  return true;
}

void TcpConnection::adopt(std::uintptr_t native_handle) {
  close();
  handle_ = native_handle;
  peer_ = "accepted";
}

void TcpConnection::close() noexcept {
  if (handle_ == kInvalidHandle) {
    return;
  }
  const NativeSocket socket = static_cast<NativeSocket>(handle_);
  // shutdown() alone does not reliably interrupt a blocked Windows recv();
  // closing the handle does, so both are performed.
  ::shutdown(socket, 2);
  close_native(socket);
  handle_ = kInvalidHandle;
}

bool TcpConnection::send_frame(const Frame& frame, std::string& error,
                               std::uint32_t max_payload_bytes) {
  const std::vector<std::uint8_t> bytes = encode_frame(frame, max_payload_bytes);
  if (bytes.empty()) {
    error = "frame could not be encoded within the configured bounds";
    return false;
  }
  std::lock_guard<std::mutex> lock(write_mutex_);
  if (!open()) {
    error = "connection is closed";
    return false;
  }
  return send_all(static_cast<NativeSocket>(handle_), bytes.data(), bytes.size(), error);
}

bool TcpConnection::recv_frame(Frame& frame, std::string& error, std::uint32_t max_payload_bytes) {
  if (!open()) {
    error = "connection is closed";
    return false;
  }
  const NativeSocket socket = static_cast<NativeSocket>(handle_);
  std::array<std::uint8_t, frame_header_bytes> header{};
  if (!recv_all(socket, header.data(), header.size(), error)) {
    return false;
  }
  // Decode the declared payload length with checked arithmetic before any
  // allocation.
  std::uint32_t payload_bytes = 0;
  for (int i = 0; i < 4; ++i) {
    payload_bytes |= static_cast<std::uint32_t>(header[20 + static_cast<std::size_t>(i)]) << (8 * i);
  }
  if (payload_bytes > max_payload_bytes) {
    error = "declared payload length exceeds the configured bound";
    return false;
  }
  std::vector<std::uint8_t> buffer(frame_header_bytes + payload_bytes + frame_trailer_bytes, 0);
  std::memcpy(buffer.data(), header.data(), header.size());
  if (!recv_all(socket, buffer.data() + frame_header_bytes, payload_bytes + frame_trailer_bytes,
                error)) {
    return false;
  }
  return decode_frame(std::span<const std::uint8_t>(buffer.data(), buffer.size()), frame, error,
                      max_payload_bytes);
}

// ---------------------------------------------------------------------------
// TcpListener
// ---------------------------------------------------------------------------

TcpListener::~TcpListener() { close(); }

bool TcpListener::listen_loopback(std::uint16_t port, std::string& error) {
  SocketSystem::ensure();
  if (handle_ != static_cast<std::uintptr_t>(~0ull)) {
    error = "listener is already bound";
    return false;
  }
  const NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == kInvalidSocket) {
    error = "listener socket creation failed";
    return false;
  }
  const char reuse = 1;
  ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = ::htonl(0x7F000001u);  // 127.0.0.1 only
  address.sin_port = ::htons(port);
  if (::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    error = "bind failed with socket error " + std::to_string(last_socket_error());
    close_native(socket);
    return false;
  }
  if (::listen(socket, 8) != 0) {
    error = "listen failed with socket error " + std::to_string(last_socket_error());
    close_native(socket);
    return false;
  }
  sockaddr_in bound{};
#ifdef _WIN32
  int bound_size = sizeof(bound);
#else
  socklen_t bound_size = sizeof(bound);
#endif
  if (::getsockname(socket, reinterpret_cast<sockaddr*>(&bound), &bound_size) != 0) {
    error = "getsockname failed";
    close_native(socket);
    return false;
  }
  port_ = ::ntohs(bound.sin_port);
  handle_ = static_cast<std::uintptr_t>(socket);
  return true;
}

std::shared_ptr<TcpConnection> TcpListener::accept(std::string& error) {
  if (handle_ == static_cast<std::uintptr_t>(~0ull)) {
    error = "listener is closed";
    return nullptr;
  }
  const NativeSocket socket = ::accept(static_cast<NativeSocket>(handle_), nullptr, nullptr);
  if (socket == kInvalidSocket) {
    error = "accept failed with socket error " + std::to_string(last_socket_error());
    return nullptr;
  }
  configure_stream(socket);
  auto connection = std::make_shared<TcpConnection>();
  connection->adopt(static_cast<std::uintptr_t>(socket));
  return connection;
}

void TcpListener::close() noexcept {
  if (handle_ == static_cast<std::uintptr_t>(~0ull)) {
    return;
  }
  const NativeSocket socket = static_cast<NativeSocket>(handle_);
  ::shutdown(socket, 2);
  close_native(socket);
  handle_ = static_cast<std::uintptr_t>(~0ull);
  port_ = 0;
}

// ---------------------------------------------------------------------------
// FrameSession
// ---------------------------------------------------------------------------

FrameSession::FrameSession(std::shared_ptr<TcpConnection> connection, std::uint32_t max_queue,
                           std::uint32_t max_payload_bytes)
    : connection_(std::move(connection)),
      max_queue_(max_queue == 0 ? 1 : max_queue),
      max_payload_bytes_(max_payload_bytes) {}

FrameSession::~FrameSession() { stop(); }

bool FrameSession::start(std::string& error) {
  if (connection_ == nullptr || !connection_->open()) {
    error = "session requires an open connection";
    return false;
  }
  if (reader_.joinable()) {
    error = "session reader is already running";
    return false;
  }
  open_.store(true, std::memory_order_release);
  stopping_.store(false, std::memory_order_release);
  reader_ = std::thread([this] { reader_main(); });
  return true;
}

void FrameSession::reader_main() {
  Frame frame;
  while (!stopping_.load(std::memory_order_acquire)) {
    std::string error;
    if (!connection_->recv_frame(frame, error, max_payload_bytes_)) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (last_error_.empty()) {
          last_error_ = error;
        }
      }
      break;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (queue_.size() >= max_queue_) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        last_error_ = "receive queue overflow; session closed";
        break;
      }
      queue_.push_back(std::move(frame));
    }
    condition_.notify_all();
  }
  open_.store(false, std::memory_order_release);
  condition_.notify_all();
}

void FrameSession::stop() noexcept {
  stopping_.store(true, std::memory_order_release);
  if (connection_ != nullptr) {
    connection_->close();
  }
  condition_.notify_all();
  if (reader_.joinable()) {
    if (std::this_thread::get_id() == reader_.get_id()) {
      // Never self-join: the reader thread detaches and exits on its own.
      reader_.detach();
    } else {
      reader_.join();
    }
  }
  open_.store(false, std::memory_order_release);
}

bool FrameSession::send(const Frame& frame, std::string& error) {
  if (connection_ == nullptr) {
    error = "session has no connection";
    return false;
  }
  if (!open()) {
    error = "session is closed";
    return false;
  }
  return connection_->send_frame(frame, error, max_payload_bytes_);
}

bool FrameSession::receive(Frame& frame) {
  std::unique_lock<std::mutex> lock(mutex_);
  condition_.wait(lock, [this] { return !queue_.empty() || !open(); });
  if (queue_.empty()) {
    return false;
  }
  frame = std::move(queue_.front());
  queue_.pop_front();
  return true;
}

bool FrameSession::try_receive(Frame& frame) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (queue_.empty()) {
    return false;
  }
  frame = std::move(queue_.front());
  queue_.pop_front();
  return true;
}

std::string FrameSession::last_error() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_error_;
}

}  // namespace agent_runtime::distributed
