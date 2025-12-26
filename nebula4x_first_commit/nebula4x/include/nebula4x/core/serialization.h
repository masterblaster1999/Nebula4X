#pragma once

#include <string>

#include "nebula4x/core/game_state.h"

namespace nebula4x {

std::string serialize_game_to_json(const GameState& state);
GameState deserialize_game_from_json(const std::string& json_text);

} // namespace nebula4x
