#pragma once

#include <string>
#include <unordered_map>

#include "nebula4x/core/tech_tree.h"

namespace nebula4x {

// Export the tech tree as structured JSON.
//
// The output is a JSON array of tech objects with stable ordering (sorted by id)
// and ends with a trailing newline.
std::string tech_tree_to_json(const std::unordered_map<std::string, TechDef>& techs);

// Export the tech tree graph in Graphviz DOT format.
//
// Nodes are tech ids and edges are prereq -> tech.
// Output ends with a trailing newline.
std::string tech_tree_to_dot(const std::unordered_map<std::string, TechDef>& techs);

} // namespace nebula4x
