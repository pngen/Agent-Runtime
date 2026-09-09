// Agent Runtime - versioned durable persistence.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef AGENT_RUNTIME_PERSISTENCE_HPP
#define AGENT_RUNTIME_PERSISTENCE_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "agent_runtime/outcome.hpp"

namespace agent_runtime {

/// Durable state file format.
///
/// Layout (all integers little-endian, all offsets checked):
///   header  : magic "ARST" | format_version u16 | header_bytes u16 | flags u32
///             | payload_bytes u64 | record_count u32 | reserved u32
///             | semantic_digest u8[32] | header_crc32c u32
///   payload : record_count records, each: record_type u16 | record_bytes u32
///             | body[record_bytes]; records appear in canonical order
///   trailer : trailer_magic "AREN" | payload_crc32c u32 | trailer_crc32c u32
///
/// Any incompatible layout change must increment the format version. An
/// unsupported version is rejected before any ambiguous parsing occurs.
inline constexpr std::uint16_t persistence_format_version = 1;
inline constexpr std::string_view persistence_magic = "ARST";
inline constexpr std::string_view persistence_trailer_magic = "AREN";
inline constexpr std::size_t persistence_header_bytes = 64;
inline constexpr std::size_t persistence_trailer_bytes = 12;

struct PersistenceOptions {
  std::uint64_t max_bytes = 1024ull * 1024ull * 1024ull;
  /// Durability: flush file buffers and (on Windows) move with write-through.
  bool durable = true;
  /// Directory used for the temporary replacement file. Empty means "next to
  /// the target file". Must exist and be writable.
  std::string temporary_directory;
};

struct PersistenceInfo {
  std::uint16_t format_version = 0;
  std::uint32_t record_count = 0;
  std::uint64_t payload_bytes = 0;
  std::uint64_t file_bytes = 0;
  std::string semantic_digest_hex;
  std::string payload_crc32c_hex;
};

struct PersistenceProbe {
  OutcomeCode code = OutcomeCode::INTERNAL_ERROR;
  Explanation explanation;
  PersistenceInfo info;
  /// Canonical JSON rendering of the decoded durable document. Empty when the
  /// file could not be decoded.
  std::string document_json;

  [[nodiscard]] bool valid() const noexcept { return code == OutcomeCode::ACCEPTED; }
};

/// Validates and decodes a durable state file without applying it to a runtime.
/// Every structural failure is reported as a specific deterministic outcome.
[[nodiscard]] PersistenceProbe persistence_inspect(const std::string& path,
                                                   const PersistenceOptions& options = {});

/// Returns the persistence format version supported by this build.
[[nodiscard]] std::uint16_t supported_persistence_format_version() noexcept;

}  // namespace agent_runtime

#endif  // AGENT_RUNTIME_PERSISTENCE_HPP
