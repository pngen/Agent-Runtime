// Agent Runtime - SHA-256 implementation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "agent_runtime/detail/sha256.hpp"

#include <cstring>

namespace agent_runtime::detail {
namespace {

constexpr std::uint32_t kRoundConstants[64] = {
    0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u, 0x3956C25Bu, 0x59F111F1u, 0x923F82A4u,
    0xAB1C5ED5u, 0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u, 0x72BE5D74u, 0x80DEB1FEu,
    0x9BDC06A7u, 0xC19BF174u, 0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu, 0x2DE92C6Fu,
    0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu, 0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u,
    0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u, 0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu,
    0x53380D13u, 0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u, 0xA2BFE8A1u, 0xA81A664Bu,
    0xC24B8B70u, 0xC76C51A3u, 0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u, 0x19A4C116u,
    0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u, 0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
    0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u, 0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u,
    0xC67178F2u};

[[nodiscard]] constexpr std::uint32_t rotr(std::uint32_t value, std::uint32_t bits) noexcept {
  return (value >> bits) | (value << (32u - bits));
}

}  // namespace

Sha256::Sha256() noexcept
    : state_{0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au, 0x510E527Fu, 0x9B05688Cu,
             0x1F83D9ABu, 0x5BE0CD19u} {
  buffer_.fill(0);
}

void Sha256::compress(const std::uint8_t* block) noexcept {
  std::uint32_t w[64];
  for (std::uint32_t i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(block[i * 4 + 0]) << 24) |
           (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
           static_cast<std::uint32_t>(block[i * 4 + 3]);
  }
  for (std::uint32_t i = 16; i < 64; ++i) {
    const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::uint32_t i = 0; i < 64; ++i) {
    const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + s1 + ch + kRoundConstants[i] + w[i];
    const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(std::span<const std::uint8_t> data) noexcept {
  if (finished_) {
    return;
  }
  total_bytes_ += data.size();
  std::size_t offset = 0;
  if (buffered_ != 0) {
    while (buffered_ < 64 && offset < data.size()) {
      buffer_[buffered_++] = data[offset++];
    }
    if (buffered_ == 64) {
      compress(buffer_.data());
      buffered_ = 0;
    }
  }
  while (data.size() - offset >= 64) {
    compress(data.data() + offset);
    offset += 64;
  }
  while (offset < data.size()) {
    buffer_[buffered_++] = data[offset++];
  }
}

void Sha256::update(std::string_view text) noexcept {
  update(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()),
                                       text.size()));
}

void Sha256::update_u8(std::uint8_t value) noexcept {
  const std::uint8_t byte = value;
  update(std::span<const std::uint8_t>(&byte, 1));
}

void Sha256::update_u32(std::uint32_t value) noexcept {
  const std::uint8_t bytes[4] = {static_cast<std::uint8_t>(value & 0xFFu),
                                 static_cast<std::uint8_t>((value >> 8) & 0xFFu),
                                 static_cast<std::uint8_t>((value >> 16) & 0xFFu),
                                 static_cast<std::uint8_t>((value >> 24) & 0xFFu)};
  update(std::span<const std::uint8_t>(bytes, 4));
}

void Sha256::update_u64(std::uint64_t value) noexcept {
  update_u32(static_cast<std::uint32_t>(value & 0xFFFFFFFFull));
  update_u32(static_cast<std::uint32_t>(value >> 32));
}

void Sha256::update_length_prefixed(std::string_view text) noexcept {
  update_u64(static_cast<std::uint64_t>(text.size()));
  update(text);
}

std::array<std::uint8_t, Sha256::digest_bytes> Sha256::finish() noexcept {
  if (!finished_) {
    const std::uint64_t bit_length = total_bytes_ * 8ull;
    const std::uint8_t pad = 0x80u;
    update(std::span<const std::uint8_t>(&pad, 1));
    const std::uint8_t zero = 0x00u;
    while (buffered_ != 56) {
      update(std::span<const std::uint8_t>(&zero, 1));
    }
    std::uint8_t length_bytes[8];
    for (std::uint32_t i = 0; i < 8; ++i) {
      length_bytes[i] = static_cast<std::uint8_t>((bit_length >> (8u * (7u - i))) & 0xFFu);
    }
    update(std::span<const std::uint8_t>(length_bytes, 8));
    finished_ = true;
  }
  std::array<std::uint8_t, digest_bytes> digest{};
  for (std::uint32_t i = 0; i < 8; ++i) {
    digest[i * 4 + 0] = static_cast<std::uint8_t>((state_[i] >> 24) & 0xFFu);
    digest[i * 4 + 1] = static_cast<std::uint8_t>((state_[i] >> 16) & 0xFFu);
    digest[i * 4 + 2] = static_cast<std::uint8_t>((state_[i] >> 8) & 0xFFu);
    digest[i * 4 + 3] = static_cast<std::uint8_t>(state_[i] & 0xFFu);
  }
  return digest;
}

std::string hex_encode(std::span<const std::uint8_t> bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const std::uint8_t byte : bytes) {
    out.push_back(kDigits[byte >> 4]);
    out.push_back(kDigits[byte & 0x0Fu]);
  }
  return out;
}

std::array<std::uint8_t, Sha256::digest_bytes> sha256(
    std::span<const std::uint8_t> data) noexcept {
  Sha256 hasher;
  hasher.update(data);
  return hasher.finish();
}

std::array<std::uint8_t, Sha256::digest_bytes> sha256(std::string_view text) noexcept {
  return sha256(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()),
                                              text.size()));
}

std::uint64_t digest_prefix_u64(std::span<const std::uint8_t> digest) noexcept {
  std::uint64_t value = 0;
  const std::size_t count = digest.size() < 8 ? digest.size() : 8;
  for (std::size_t i = 0; i < count; ++i) {
    value |= static_cast<std::uint64_t>(digest[i]) << (8u * static_cast<unsigned>(i));
  }
  return value;
}

std::uint64_t content_digest(std::string_view text) noexcept {
  const auto digest = sha256(text);
  return digest_prefix_u64(std::span<const std::uint8_t>(digest.data(), digest.size()));
}

}  // namespace agent_runtime::detail
