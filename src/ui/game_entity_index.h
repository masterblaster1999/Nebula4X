#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "nebula4x/util/json.h"

namespace nebula4x::ui {

// Entry describing an entity discovered in the current game-state JSON snapshot.
struct GameEntityIndexEntry {
  std::uint64_t id{0};

  // JSON Pointer to the entity object inside the root document (e.g. "/ships/12").
  std::string path;

  // Top-level collection key (e.g. "ships", "systems").
  std::string kind;

  // Best-effort friendly name (if present).
  std::string name;
};

// Index built from the current in-memory game-state JSON snapshot.
struct GameEntityIndex {
  // Revision of the cached game JSON snapshot used to build this index.
  std::uint64_t revision{0};

  // Map: entity id -> entry.
  std::unordered_map<std::uint64_t, GameEntityIndexEntry> by_id;
};

// Get the global index instance.
const GameEntityIndex& game_entity_index();

// Mark the index as stale so the next ensure() will rebuild it.
void invalidate_game_entity_index();

// Ensure the entity index is built for the given JSON root and revision.
//
// Returns true if an index is available after the call.
bool ensure_game_entity_index(const nebula4x::json::Value& root, std::uint64_t revision);

// Best-effort parse of a non-negative integer id from a JSON scalar.
bool json_to_u64_id(const nebula4x::json::Value& v, std::uint64_t& out);

// Lookup by id (returns nullptr if missing).
const GameEntityIndexEntry* find_game_entity(std::uint64_t id);

} // namespace nebula4x::ui
