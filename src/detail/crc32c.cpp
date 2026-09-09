// Agent Runtime - CRC-32C implementation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "agent_runtime/detail/crc32c.hpp"

namespace agent_runtime::detail {
namespace {

// Reflected CRC-32C polynomial (Castagnoli), 0x1EDC6F41 reflected = 0x82F63B78.
constexpr std::uint32_t kPolynomial = 0x82F63B78u;

struct Table {
  std::uint32_t entries[256]{};
  constexpr Table() noexcept {
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t crc = i;
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc & 1u) != 0u ? (crc >> 1) ^ kPolynomial : (crc >> 1);
      }
      entries[i] = crc;
    }
  }
};

constexpr Table kTable{};

}  // namespace

std::uint32_t crc32c(std::span<const std::uint8_t> data, std::uint32_t seed) noexcept {
  std::uint32_t crc = ~seed;
  for (const std::uint8_t byte : data) {
    crc = kTable.entries[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
  }
  return ~crc;
}

}  // namespace agent_runtime::detail
