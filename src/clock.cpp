// Agent Runtime - clock implementations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "agent_runtime/clock.hpp"

#include <chrono>

namespace agent_runtime {

UnixMillis SystemClock::now_unix_millis() const {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

MonotonicMillis SystemClock::now_monotonic_millis() const {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<MonotonicMillis>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

}  // namespace agent_runtime
