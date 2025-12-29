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

// Report returned by fix_game_state().
//
// The fixer is intended to help recover from:
// - partially-migrated / hand-edited saves
// - mod content changes that orphan references
// - bugs that left dangling ids / negative values
//
// It tries to be conservative (prefer pruning invalid references over
// "inventing" new entities), but it will delete obviously broken entries
// (e.g. colonies pointing at missing bodies) to restore internal consistency.
struct FixReport {
  int changes{0};
  std::vector<std::string> actions;
};

// Attempt to repair common integrity issues in-place.
//
// If `content` is provided, additional fixes are performed for references that
// depend on the content DB (unknown installation/tech/design ids, etc.).
FixReport fix_game_state(GameState& s, const ContentDB* content = nullptr);

} // namespace nebula4x
