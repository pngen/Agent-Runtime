// Agent Runtime - identity allocation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "agent_runtime/ids.hpp"

namespace agent_runtime {
namespace {

std::atomic<std::uint64_t> g_next_id{1};

}  // namespace

std::uint64_t IdAllocator::next() noexcept {
  const std::uint64_t value = g_next_id.fetch_add(1, std::memory_order_relaxed);
  // Identities are never reused and never wrap. Reaching the ceiling is a hard
  // stop rather than silent aliasing with an already-issued identity.
  if (value == 0) {
    return Generation<AgentTag>::kMax;
  }
  return value;
}

void IdAllocator::observe(std::uint64_t highest_seen) noexcept {
  std::uint64_t current = g_next_id.load(std::memory_order_relaxed);
  while (highest_seen >= current) {
    if (g_next_id.compare_exchange_weak(current, highest_seen + 1, std::memory_order_relaxed,
                                        std::memory_order_relaxed)) {
      return;
    }
  }
}

std::uint64_t IdAllocator::highest_issued() noexcept {
  const std::uint64_t value = g_next_id.load(std::memory_order_relaxed);
  return value == 0 ? 0 : value - 1;
}

}  // namespace agent_runtime
