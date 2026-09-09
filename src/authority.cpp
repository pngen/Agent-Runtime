// Agent Runtime - authority rendering helpers.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "agent_runtime/authority.hpp"

namespace agent_runtime {

std::string_view to_string(BudgetOutcome outcome) noexcept {
  switch (outcome) {
    case BudgetOutcome::UNKNOWN:
      return "UNKNOWN";
    case BudgetOutcome::ALLOW:
      return "ALLOW";
    case BudgetOutcome::DENY:
      return "DENY";
    case BudgetOutcome::DEFER:
      return "DEFER";
    case BudgetOutcome::kCount:
      break;
  }
  return "UNKNOWN_BUDGET_OUTCOME";
}

}  // namespace agent_runtime
