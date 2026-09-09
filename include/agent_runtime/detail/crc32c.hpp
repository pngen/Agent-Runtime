// Agent Runtime - CRC-32C (Castagnoli) storage/transport integrity.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef AGENT_RUNTIME_DETAIL_CRC32C_HPP
#define AGENT_RUNTIME_DETAIL_CRC32C_HPP

#include <cstddef>
#include <cstdint>
#include <span>

namespace agent_runtime::detail {

/// Hardware-independent CRC-32C. Deterministic across platforms and builds.
[[nodiscard]] std::uint32_t crc32c(std::span<const std::uint8_t> data,
                                   std::uint32_t seed = 0) noexcept;

[[nodiscard]] inline std::uint32_t crc32c(std::uint32_t seed,
                                          std::span<const std::uint8_t> data) noexcept {
  return crc32c(data, seed);
}

}  // namespace agent_runtime::detail

#endif  // AGENT_RUNTIME_DETAIL_CRC32C_HPP
