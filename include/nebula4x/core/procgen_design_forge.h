#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "nebula4x/core/entities.h"
#include "nebula4x/core/game_state.h"

namespace nebula4x {

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

  // Naming.
  std::string id_prefix{"forge"};
  std::string name_prefix{"Forge"};

  // Flavor toggles.
  bool prefer_missiles{false};
  bool prefer_shields{true};
  bool include_ecm_eccm{true};
};

// Returned candidate designs and their heuristic scores.
struct ForgedDesign {
  ShipDesign design;
  double score{0.0};
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
