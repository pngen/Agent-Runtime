// Agent Runtime - internal durable document encoding.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef AGENT_RUNTIME_SRC_PERSISTENCE_INTERNAL_HPP
#define AGENT_RUNTIME_SRC_PERSISTENCE_INTERNAL_HPP

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "agent_runtime/limits.hpp"
#include "agent_runtime/persistence.hpp"
#include "runtime_state.hpp"

namespace agent_runtime::detail {

struct DecodeResult {
  OutcomeCode code = OutcomeCode::INTERNAL_ERROR;
  std::string subject;
  std::string message;
  std::optional<CanonicalState> state;
  PersistenceInfo info;
  std::string document_json;

  [[nodiscard]] bool ok() const noexcept { return code == OutcomeCode::ACCEPTED; }
};

/// Encodes canonical state into the versioned durable document.
[[nodiscard]] std::vector<std::uint8_t> encode_document(const CanonicalState& state,
                                                        const ResourceLimits& limits);

/// Decodes and fully validates a durable document. Nothing is applied to a live
/// runtime: the caller receives a candidate state that has already passed
/// structural and semantic invariant validation.
[[nodiscard]] DecodeResult decode_document(std::span<const std::uint8_t> bytes,
                                           const ResourceLimits& limits);

/// Reads a file with a hard byte bound.
[[nodiscard]] bool read_file_bounded(const std::string& path, std::uint64_t max_bytes,
                                     std::vector<std::uint8_t>& out, std::string& error);

/// Writes bytes to a temporary file, flushes them durably and atomically
/// replaces the target. On any failure the target is left untouched and the
/// temporary file is removed.
[[nodiscard]] bool atomic_write(const std::string& path, std::span<const std::uint8_t> bytes,
                                const PersistenceOptions& options, std::string& error);

}  // namespace agent_runtime::detail

#endif  // AGENT_RUNTIME_SRC_PERSISTENCE_INTERNAL_HPP
