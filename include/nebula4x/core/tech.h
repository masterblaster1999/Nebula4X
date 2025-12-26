#pragma once

#include <string>

#include "nebula4x/core/game_state.h"

namespace nebula4x {

// Loads designs + installations from a JSON file (see data/blueprints/*.json).
ContentDB load_content_db_from_file(const std::string& path);

} // namespace nebula4x
