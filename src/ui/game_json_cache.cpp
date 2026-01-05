#include "ui/game_json_cache.h"

#include <algorithm>
#include <exception>

#include "nebula4x/core/serialization.h"
#include "nebula4x/core/simulation.h"
#include "nebula4x/util/log.h"

namespace nebula4x::ui {
namespace {

GameJsonCache g_cache;
bool g_dirty = true;

} // namespace

const GameJsonCache& game_json_cache() { return g_cache; }

void invalidate_game_json_cache() { g_dirty = true; }

bool ensure_game_json_cache(const Simulation& sim, double now_sec, double min_refresh_sec, bool force) {
  // If the simulation swapped out its GameState, any previous snapshot is stale.
  if (g_cache.state_generation != sim.state_generation()) {
    g_cache.state_generation = sim.state_generation();
    g_cache.loaded = false;
    g_cache.root.reset();
    g_cache.text.clear();
    g_cache.error.clear();
    g_dirty = true;
  }

  if (g_dirty) force = true;

  // Throttle refresh attempts.
  if (!force && min_refresh_sec > 0.0 && now_sec > 0.0) {
    const double dt = now_sec - g_cache.last_refresh_time;
    if (dt >= 0.0 && dt < min_refresh_sec) {
      return g_cache.loaded && (bool)g_cache.root;
    }
  }

  g_cache.last_refresh_time = now_sec;
  g_dirty = false;

  try {
    std::string text = serialize_game_to_json(sim.state());

    // If the serialized snapshot is identical, keep the existing parsed doc.
    if (g_cache.loaded && !g_cache.text.empty() && text == g_cache.text && g_cache.root) {
      g_cache.error.clear();
      return true;
    }

    auto parsed = std::make_shared<nebula4x::json::Value>(nebula4x::json::parse(text));
    g_cache.text = std::move(text);
    g_cache.root = std::move(parsed);
    g_cache.loaded = true;
    g_cache.error.clear();
    g_cache.revision++;
    return true;
  } catch (const std::exception& e) {
    // Keep the previous snapshot if it exists, but surface the error.
    g_cache.error = e.what();

    // If we don't have any usable snapshot, mark as not loaded.
    if (!g_cache.root) {
      g_cache.loaded = false;
      g_cache.text.clear();
    } else {
      g_cache.loaded = true;
    }

    // Still bump revision so windows can notice the refresh attempt and show the error.
    g_cache.revision++;
    NEBULA_LOG_WARN("Game JSON cache refresh failed: %s", g_cache.error.c_str());
    return g_cache.loaded && (bool)g_cache.root;
  }
}

} // namespace nebula4x::ui
