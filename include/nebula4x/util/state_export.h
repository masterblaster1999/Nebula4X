#pragma once

#include <string>

#include "nebula4x/core/game_state.h"

namespace nebula4x {

// Export ships as structured JSON.
//
// If `content` is provided, the output will include resolved design names and
// design-derived stats.
//
// Output is a JSON array and ends with a trailing newline.
std::string ships_to_json(const GameState& state, const ContentDB* content = nullptr);

// Export colonies as structured JSON.
//
// If `content` is provided, the output will include resolved installation/design
// names as well as some computed per-day values (mineral production, colony
// construction points, shipyard capacity).
//
// Output is a JSON array and ends with a trailing newline.
std::string colonies_to_json(const GameState& state, const ContentDB* content = nullptr);

// Export fleets as structured JSON.
//
// The output includes resolved names and a per-fleet embedded ship summary.
//
// Output is a JSON array and ends with a trailing newline.
std::string fleets_to_json(const GameState& state);

// Export bodies as structured JSON.
//
// Includes basic orbit parameters and (if present) Body::mineral_deposits.
//
// Output is a JSON array and ends with a trailing newline.
std::string bodies_to_json(const GameState& state);

} // namespace nebula4x
