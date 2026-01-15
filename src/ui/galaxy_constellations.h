#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "nebula4x/core/game_state.h"
#include "nebula4x/core/ids.h"
#include "nebula4x/core/vec2.h"

namespace nebula4x::ui {

struct GalaxyConstellationEdge {
  Id a{kInvalidId};
  Id b{kInvalidId};
};

// A light-weight, UI-only procedural "constellation": a small, coherent
// cluster of stars with a deterministic name, signature, and connective
// skeleton (MST).
//
// Constellations are computed from the already-visible system set, so under
// fog-of-war they do not leak information.
struct GalaxyConstellation {
  std::uint64_t id{0};
  Id region_id{kInvalidId};

  std::string name;
  std::string code;   // short stable signature (e.g., "AB12-CD34")
  std::string glyph;  // 8x8 ASCII glyph

  // Member systems (StarSystem ids).
  std::vector<Id> systems;

  // MST edges between member systems.
  std::vector<GalaxyConstellationEdge> edges;

  // Centroid in galaxy_pos units.
  Vec2 centroid{0.0, 0.0};
};

struct GalaxyConstellationParams {
  // Typical size of a constellation cluster. Actual cluster sizes vary
  // slightly per region for visual variety.
  int target_cluster_size{8};
  // Safety cap to avoid generating a huge number of constellations.
  int max_constellations{128};
};

// Build constellations from the provided visible system id set.
std::vector<GalaxyConstellation> build_galaxy_constellations(
    const GameState& st,
    const std::vector<Id>& visible_system_ids,
    const GalaxyConstellationParams& params);

}  // namespace nebula4x::ui
