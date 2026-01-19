#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "nebula4x/util/json.h"

namespace nebula4x {
class Simulation;
}

namespace nebula4x::ui {

// Shared cache of a JSON snapshot of the *current* in-memory game state.
//
// Several procedural/debug UI windows (Watchboard, Data Lenses, Dashboards, Pivot Tables,
// OmniSearch, JSON Explorer) need a "live" JSON representation of the running simulation.
// Previously each window independently serialized the entire state to text and then parsed
// it back into a JSON DOM, often multiple times per second, causing redundant work and hitchy
// UI when multiple tools were open.
//
// This cache centralizes that work so windows can share the same JSON document.
// Windows are still free to *snapshot* a particular revision (via shared_ptr) so they
// can keep stable pointers while doing incremental work across multiple frames.
struct GameJsonCache {
  // Monotonic revision counter. Increments when a *new* JSON snapshot is produced.
  // (If the serialized JSON is identical to the previous snapshot, revision will not change.)
  std::uint64_t revision{0};

  // Simulation::state_generation() of the snapshot.
  std::uint64_t state_generation{0};

  // True if we currently have a snapshot.
  bool loaded{false};

  // Error from the most recent refresh attempt (empty on success).
  std::string error;

  // Last successful snapshot (serialized JSON).
  // Kept for cheap "did it change" comparisons and for tools that want text output.
  std::string text;

  // JSON root for the snapshot (in-memory DOM).
  // NOTE: shared_ptr so windows can hold onto older revisions safely.
  std::shared_ptr<const nebula4x::json::Value> root;

  // ImGui time (seconds) when we last attempted a refresh.
  double last_refresh_time{0.0};
};

// Get the global cache instance.
const GameJsonCache& game_json_cache();

// Mark the cache as stale so the next ensure() will force a refresh.
void invalidate_game_json_cache();

// Ensure the cache is refreshed if needed.
//
// - now_sec: pass ImGui::GetTime() (or 0 if unknown).
// - min_refresh_sec: if > 0, we won't refresh again until at least that many seconds
//   have elapsed since the last refresh attempt.
// - force: bypass the min_refresh_sec gate.
//
// Returns true if a snapshot is available after the call (either newly created or a
// previously cached snapshot).
bool ensure_game_json_cache(const Simulation& sim, double now_sec, double min_refresh_sec, bool force = false);

} // namespace nebula4x::ui
