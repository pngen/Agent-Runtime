// Agent Runtime - strongly typed identities and generations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef AGENT_RUNTIME_IDS_HPP
#define AGENT_RUNTIME_IDS_HPP

#include <atomic>
#include <compare>
#include <cstdint>
#include <functional>
#include <string>

namespace agent_runtime {

/// Identity tag types. Each tag produces a distinct EntityId/Generation type so
/// that identities with different semantics can never be interchanged.
struct AgentTag {};
struct AgentRuntimeTag {};
struct AgentRunTag {};
struct WorkTag {};
struct SchedulerAssignmentTag {};
struct DispatchTag {};
struct StepTag {};
struct ActionTag {};
struct ActionAttemptTag {};
struct ModelCallTag {};
struct ToolCallTag {};
struct MemoryBindingTag {};
struct CheckpointBindingTag {};
struct BudgetTag {};
struct PolicyTag {};
struct ResultTag {};
struct SideEffectTag {};
struct SessionTag {};
struct BackendTag {};
struct AgentBootTag {};
struct RuntimeBootTag {};
struct CoordinatorEpochTag {};
struct RuntimeEpochTag {};
struct ProgressTag {};
struct CorrelationTag {};
struct EventTag {};

/// A durable entity identity. Value 0 is the invalid/absent identity.
template <class Tag>
class EntityId {
 public:
  using tag = Tag;

  constexpr EntityId() noexcept = default;
  constexpr explicit EntityId(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
  explicit constexpr operator bool() const noexcept { return valid(); }

  friend constexpr bool operator==(const EntityId&, const EntityId&) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(const EntityId&,
                                                    const EntityId&) noexcept = default;

 private:
  std::uint64_t value_{0};
};

/// A monotonically increasing generation/incarnation/epoch number.
/// Value 0 means "unknown / never issued". Generations only move forward.
template <class Tag>
class Generation {
 public:
  using tag = Tag;

  constexpr Generation() noexcept = default;
  constexpr explicit Generation(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
  explicit constexpr operator bool() const noexcept { return valid(); }

  /// Returns the next generation. Saturates at the maximum representable value
  /// so that generation arithmetic can never wrap around into a stale value.
  [[nodiscard]] constexpr Generation next() const noexcept {
    return Generation(value_ == kMax ? kMax : value_ + 1);
  }

  static constexpr std::uint64_t kMax = 0xFFFFFFFFFFFFFFFFull;

  friend constexpr bool operator==(const Generation&, const Generation&) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(const Generation&,
                                                    const Generation&) noexcept = default;

 private:
  std::uint64_t value_{0};
};

using AgentId = EntityId<AgentTag>;
using AgentGeneration = Generation<AgentTag>;
using AgentBootId = Generation<AgentBootTag>;

using AgentRuntimeId = EntityId<AgentRuntimeTag>;
using AgentRuntimeGeneration = Generation<AgentRuntimeTag>;
using RuntimeBootId = Generation<RuntimeBootTag>;

using AgentRunId = EntityId<AgentRunTag>;
using AgentRunGeneration = Generation<AgentRunTag>;

using WorkId = EntityId<WorkTag>;
using WorkGeneration = Generation<WorkTag>;

using SchedulerAssignmentId = EntityId<SchedulerAssignmentTag>;
using AssignmentGeneration = Generation<SchedulerAssignmentTag>;
using DispatchGeneration = Generation<DispatchTag>;

using StepId = EntityId<StepTag>;
using StepGeneration = Generation<StepTag>;

using ActionId = EntityId<ActionTag>;
using ActionGeneration = Generation<ActionTag>;

using ActionAttemptId = EntityId<ActionAttemptTag>;
using AttemptGeneration = Generation<ActionAttemptTag>;

using ModelCallId = EntityId<ModelCallTag>;
using ModelCallGeneration = Generation<ModelCallTag>;

using ToolCallId = EntityId<ToolCallTag>;
using ToolCallGeneration = Generation<ToolCallTag>;

using MemoryBindingId = EntityId<MemoryBindingTag>;
using MemoryBindingGeneration = Generation<MemoryBindingTag>;

using CheckpointBindingId = EntityId<CheckpointBindingTag>;
using CheckpointGeneration = Generation<CheckpointBindingTag>;

using BudgetId = EntityId<BudgetTag>;
using BudgetGeneration = Generation<BudgetTag>;

using PolicyId = EntityId<PolicyTag>;
using PolicyGeneration = Generation<PolicyTag>;

using ResultId = EntityId<ResultTag>;
using CompletionGeneration = Generation<ResultTag>;

using SideEffectId = EntityId<SideEffectTag>;
using CommitGeneration = Generation<SideEffectTag>;

using SessionId = EntityId<SessionTag>;
using BackendId = EntityId<BackendTag>;

using CoordinatorEpoch = Generation<CoordinatorEpochTag>;
using RuntimeEpoch = Generation<RuntimeEpochTag>;
using ProgressGeneration = Generation<ProgressTag>;
using CorrelationId = EntityId<CorrelationTag>;
using EventId = EntityId<EventTag>;

/// Process-wide monotonic identity source. Identities are never reused and never
/// wrap: reaching the ceiling is a hard failure rather than silent aliasing.
class IdAllocator {
 public:
  IdAllocator() = delete;

  [[nodiscard]] static std::uint64_t next() noexcept;
  /// Seeds the allocator above a value observed in durable state, so that a
  /// restarted process cannot reissue identities that were already persisted.
  static void observe(std::uint64_t highest_seen) noexcept;
  [[nodiscard]] static std::uint64_t highest_issued() noexcept;
};

template <class Tag>
[[nodiscard]] EntityId<Tag> allocate_id() noexcept {
  return EntityId<Tag>(IdAllocator::next());
}

template <class Tag>
[[nodiscard]] std::string to_string(EntityId<Tag> id) {
  return std::to_string(id.value());
}

template <class Tag>
[[nodiscard]] std::string to_string(Generation<Tag> generation) {
  return std::to_string(generation.value());
}

}  // namespace agent_runtime

namespace std {

template <class Tag>
struct hash<agent_runtime::EntityId<Tag>> {
  [[nodiscard]] size_t operator()(agent_runtime::EntityId<Tag> id) const noexcept {
    return std::hash<std::uint64_t>{}(id.value());
  }
};

template <class Tag>
struct hash<agent_runtime::Generation<Tag>> {
  [[nodiscard]] size_t operator()(agent_runtime::Generation<Tag> generation) const noexcept {
    return std::hash<std::uint64_t>{}(generation.value());
  }
};

}  // namespace std

#endif  // AGENT_RUNTIME_IDS_HPP
