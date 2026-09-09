// Agent Runtime - SHA-256 semantic digests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef AGENT_RUNTIME_DETAIL_SHA256_HPP
#define AGENT_RUNTIME_DETAIL_SHA256_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace agent_runtime::detail {

class Sha256 {
 public:
  static constexpr std::size_t digest_bytes = 32;

  Sha256() noexcept;
  void update(std::span<const std::uint8_t> data) noexcept;
  void update(std::string_view text) noexcept;
  void update_u8(std::uint8_t value) noexcept;
  void update_u32(std::uint32_t value) noexcept;
  void update_u64(std::uint64_t value) noexcept;
  void update_length_prefixed(std::string_view text) noexcept;
  [[nodiscard]] std::array<std::uint8_t, digest_bytes> finish() noexcept;

 private:
  void compress(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_;
  std::array<std::uint8_t, 64> buffer_;
  std::size_t buffered_ = 0;
  std::uint64_t total_bytes_ = 0;
  bool finished_ = false;
};

[[nodiscard]] std::string hex_encode(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::array<std::uint8_t, Sha256::digest_bytes> sha256(
    std::span<const std::uint8_t> data) noexcept;
[[nodiscard]] std::array<std::uint8_t, Sha256::digest_bytes> sha256(std::string_view text) noexcept;
[[nodiscard]] std::uint64_t digest_prefix_u64(std::span<const std::uint8_t> digest) noexcept;

/// Deterministic 64-bit content hash used for request/result digests.
[[nodiscard]] std::uint64_t content_digest(std::string_view text) noexcept;

}  // namespace agent_runtime::detail

#endif  // AGENT_RUNTIME_DETAIL_SHA256_HPP
