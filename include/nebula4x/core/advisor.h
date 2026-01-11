#pragma once

#include <string>
#include <vector>

#include "nebula4x/core/simulation.h"

namespace nebula4x {

// A lightweight, deterministic "advisor" system that scans the current game
// state for actionable issues (logistics shortfalls, ship readiness problems,
// colony health concerns, etc.).
//
// The advisor is designed to be UI-agnostic so it can be used by:
//  - UI windows (to show issue lists + quick actions)
//  - CLI tooling (future: export a report for CI/regressions)
//  - Tests (deterministic issue detection)

enum class AdvisorIssueKind {
  LogisticsNeed,
  ShipLowFuel,
  ShipDamaged,

  // Missile ammunition below threshold (finite magazines only).
  ShipLowAmmo,

  // Maintenance condition below threshold (when ship maintenance is enabled).
  ShipLowMaintenance,

  ColonyHabitationShortfall,
  ColonyGarrisonProblem,
};

struct AdvisorIssue {
  AdvisorIssueKind kind{AdvisorIssueKind::LogisticsNeed};
  EventLevel level{EventLevel::Info};

  // A numeric severity used for deterministic sorting. Interpretation depends
  // on kind (e.g. missing tons, missing HP, missing habitation, etc.).
  double severity{0.0};

  // Primary context identifiers (kInvalidId when not applicable).
  Id faction_id{kInvalidId};
  Id system_id{kInvalidId};
  Id ship_id{kInvalidId};
  Id colony_id{kInvalidId};

  // Optional structured details.
  LogisticsNeedKind logistics_kind{LogisticsNeedKind::Shipyard};
  std::string resource;     // e.g. "Duranium", "Fuel"
  std::string context_id;   // e.g. installation id for construction needs
  double desired{0.0};
  double have{0.0};
  double missing{0.0};

  // Human-readable summary intended for UI filter/search.
  std::string summary;
};

struct AdvisorIssueOptions {
  bool include_logistics{true};
  bool include_ships{true};
  bool include_colonies{true};

  bool include_habitability{true};
  bool include_garrison{true};

  // Thresholds for ship readiness issues.
  // Example: 0.25 means "flag ships below 25% fuel".
  double low_fuel_fraction{0.25};
  double low_hp_fraction{0.75};

  // Missile ammo fraction threshold for finite-magazine ships.
  // Example: 0.25 means "flag ships below 25% ammo".
  double low_ammo_fraction{0.25};

  // Maintenance condition threshold (0..1) when ship maintenance is enabled.
  // Example: 0.70 means "flag ships below 70% maintenance condition".
  double low_maintenance_fraction{0.70};

  // Caps (safety guards for huge saves).
  int max_logistics_issues{250};
  int max_ship_issues{250};
  int max_colony_issues{250};
  int max_total_issues{1000};
};

// Compute advisor issues for a given faction.
//
// Notes:
// - Deterministic: stable ordering across platforms for the same input state.
// - Side-effect-free: does not mutate the Simulation.
std::vector<AdvisorIssue> advisor_issues_for_faction(const Simulation& sim, Id faction_id,
                                                    const AdvisorIssueOptions& opt = {});

const char* advisor_issue_kind_label(AdvisorIssueKind k);
const char* logistics_need_kind_label(LogisticsNeedKind k);

}  // namespace nebula4x
