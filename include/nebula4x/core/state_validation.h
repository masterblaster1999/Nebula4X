#pragma once

#include <string>
#include <vector>

#include "nebula4x/core/game_state.h"

namespace nebula4x {

// Validate referential integrity and basic invariants of a GameState.
//
// This is primarily intended as a developer/modder tool:
// - detect broken saves (hand edits, incomplete migrations)
// - sanity-check scenarios produced by generators
//
// If `content` is provided, additional checks are performed for IDs that refer
// to content definitions (designs, installations, tech ids, etc.).
//
// Returns a stable-sorted list of human-readable error strings.
// Empty => state is considered valid.
std::vector<std::string> validate_game_state(const GameState& s, const ContentDB* content = nullptr);

} // namespace nebula4x
