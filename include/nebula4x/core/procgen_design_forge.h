#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "nebula4x/core/entities.h"
#include "nebula4x/core/game_state.h"

namespace nebula4x {

// Optional hard/soft constraints for the design forge.
//
// All values are interpreted as *minimums* unless otherwise stated.
// Any field left at 0 (or false) is ignored.
struct DesignForgeConstraints {
  // Kinematics / logistics.
  double min_speed_km_s{0.0};

  // Min fuel range expressed in million km.
  // Range is estimated as fuel_capacity_tons / fuel_use_per_mkm.
  double min_range_mkm{0.0};

  // If > 0, candidates heavier than this are considered invalid.
  double max_mass_tons{0.0};

  // Industrial roles.
  double min_cargo_tons{0.0};
  double min_mining_tons_per_day{0.0};
  double min_colony_capacity_millions{0.0};
  double min_troop_capacity{0.0};

  // Sensors / stealth.
  double min_sensor_range_mkm{0.0};
  double max_signature_multiplier{0.0};  // 0 = ignore; otherwise require design.signature_multiplier <= max.
  double min_ecm_strength{0.0};
  double min_eccm_strength{0.0};

  // Combat.
  double min_beam_damage{0.0};
  double min_missile_damage{0.0};
  double min_point_defense_damage{0.0};
  double min_shields{0.0};
  double min_hp{0.0};

  // Power model.
  // If true, designs with power_use_total > power_generation are considered invalid.
  bool require_power_balance{false};

  // Additional non-negative margin required beyond balance.
  // (Only applied if require_power_balance is true.)
  double min_power_margin{0.0};
};

// Tuning knobs for the procedural design generator.
//
// The goal is not to create perfectly optimal designs, but to quickly create
// plausible *variants* that feel distinct and let players (and AI) explore the
// design space without hand-editing every component list.
struct DesignForgeOptions {
  // Desired role for the forged designs. If Unknown, base_design.role is used.
  ShipRole role{ShipRole::Unknown};

  // How many designs to return (best-scoring unique candidates).
  int desired_count{6};

  // How many random candidates to generate per output design.
  // Higher = better results, but slower UI.
  int candidate_multiplier{8};

  // How many mutation steps to apply per candidate when starting from the base
  // design.
  int mutations_per_candidate{4};

  // Upper bound on the number of components in a generated design.
  // This is a soft bound used by the forge's add/tuning steps.
  int max_components{14};

  // Naming.
  std::string id_prefix{"forge"};
  std::string name_prefix{"Forge"};

  // Flavor toggles.
  bool prefer_missiles{false};
  bool prefer_shields{true};
  bool include_ecm_eccm{true};

  // Optional constraints (min speed/range/cargo/etc).
  DesignForgeConstraints constraints{};

  // If true, only designs that satisfy all constraints are returned.
  // If no candidate meets constraints, the forge returns an empty vector.
  bool only_meeting_constraints{false};
};

// Returned candidate designs and their heuristic scores.
struct ForgedDesign {
  ShipDesign design;
  double score{0.0};

  // Whether the design met the provided constraints.
  bool meets_constraints{true};

  // Penalty applied for constraint violations (0 for valid designs).
  double constraint_penalty{0.0};
};

// Generate procedurally mutated variants of a base design using a faction's
// unlocked component pool.
//
// - The returned ShipDesign objects include derived stats (mass/speed/range/etc)
//   computed from their components.
// - Component IDs are restricted to `unlocked_components`.
// - The caller is responsible for inserting the designs into the simulation
//   (e.g. via Simulation::upsert_custom_design).
std::vector<ForgedDesign> forge_design_variants(const ContentDB& content,
                                                const std::vector<std::string>& unlocked_components,
                                                const ShipDesign& base_design,
                                                std::uint64_t seed,
                                                const DesignForgeOptions& options,
                                                std::string* out_debug = nullptr);

}  // namespace nebula4x
