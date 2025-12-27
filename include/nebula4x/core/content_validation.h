#pragma once

#include <string>
#include <vector>

#include "nebula4x/core/game_state.h"

namespace nebula4x {

// Validate a ContentDB for internal consistency.
//
// Returns a list of human-readable error strings. An empty list means "valid".
//
// This is meant for:
//  - quick sanity checks in CI/tests,
//  - CLI validation tooling,
//  - content modding workflows.
std::vector<std::string> validate_content_db(const ContentDB& db);

} // namespace nebula4x
