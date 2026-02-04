#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "nebula4x/core/game_state.h"
#include "nebula4x/core/tech_tree.h"

namespace nebula4x {

// Loads components + designs + installations from a JSON file (see data/blueprints/*.json).
//
// Modding / composability helpers:
// - Optional top-level "include" / "includes" (string or array of strings) to include
//   additional blueprint JSON files. Includes are processed depth-first and merged
//   in order, then the including file overrides the included content.
// - Components and installations are merged using an RFC 7396-like "merge patch"
//   semantics (objects patch objects; null deletes keys).
// - Designs may use helper keys "components_add" / "components_remove" to patch the
//   component list without rewriting it.
ContentDB load_content_db_from_file(const std::string& path);

// Loads and merges blueprint data from multiple root files.
//
// Paths are processed in order; later roots override earlier ones.
// Each root may also use the "include" / "includes" directive.
ContentDB load_content_db_from_files(const std::vector<std::string>& paths);

// Loads a technology tree definition from a JSON file (see data/tech/tech_tree.json).
//
// Supports the same "include" / "includes" directive and basic overlay/patch helpers.
std::unordered_map<std::string, TechDef> load_tech_db_from_file(const std::string& path);

// Loads and merges technology definitions from multiple root files.
std::unordered_map<std::string, TechDef> load_tech_db_from_files(const std::vector<std::string>& paths);

} // namespace nebula4x
