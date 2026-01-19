#pragma once

#include <string>

#include "nebula4x/core/game_state.h"
#include "nebula4x/util/json.h"

namespace nebula4x {

// Serialize the game state into an in-memory JSON document.
//
// This is primarily used by UI/debug tooling that wants to query the live
// simulation via JSON Pointer patterns without paying an extra JSON text parse.
json::Value serialize_game_to_json_value(const GameState& state);

// Serialize the game state into a JSON text document (pretty-printed).
std::string serialize_game_to_json(const GameState& state);

// Parse a saved game from JSON text.
GameState deserialize_game_from_json(const std::string& json_text);

} // namespace nebula4x
