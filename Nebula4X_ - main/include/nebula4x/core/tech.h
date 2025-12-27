#pragma once

#include <string>
#include <unordered_map>

#include "nebula4x/core/game_state.h"
#include "nebula4x/core/tech_tree.h"

namespace nebula4x {

// Loads components + designs + installations from a JSON file (see data/blueprints/*.json).
ContentDB load_content_db_from_file(const std::string& path);

// Loads a technology tree definition from a JSON file (see data/tech/tech_tree.json).
std::unordered_map<std::string, TechDef> load_tech_db_from_file(const std::string& path);

} // namespace nebula4x
