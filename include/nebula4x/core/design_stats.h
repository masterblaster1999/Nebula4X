#pragma once

#include <string>

#include "nebula4x/core/game_state.h"

namespace nebula4x {

// Result of deriving ShipDesign's computed fields from its component list.
//
// This is used for validating and (re)computing stats for player-created
// designs, procedural designs, and tooling.
struct DesignDeriveResult {
  bool ok{false};
  std::string error;
};

// Recompute ShipDesign's derived stats from its component list.
//
// On success, the ShipDesign is updated in-place and the returned result has
// ok=true.
DesignDeriveResult derive_ship_design_stats(const ContentDB& content, ShipDesign& design);

}  // namespace nebula4x
