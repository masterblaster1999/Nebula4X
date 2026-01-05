#include "ui/game_entity_index.h"

#include <cmath>
#include <limits>
#include <string>

#include "nebula4x/util/json_pointer.h"

namespace nebula4x::ui {
namespace {

GameEntityIndex g_index;
bool g_stale = true;

bool parse_u64_from_number(double x, std::uint64_t& out) {
  if (!std::isfinite(x) || x < 0.0) return false;

  double intpart = 0.0;
  const double frac = std::modf(x, &intpart);
  if (frac != 0.0) return false;

  const double max_u64 = static_cast<double>(std::numeric_limits<std::uint64_t>::max());
  if (intpart > max_u64) return false;

  out = static_cast<std::uint64_t>(intpart);
  return true;
}

std::string best_effort_name(const nebula4x::json::Object& o) {
  if (auto it = o.find("name"); it != o.end() && it->second.is_string()) {
    return it->second.string_value();
  }
  if (auto it = o.find("message"); it != o.end() && it->second.is_string()) {
    // Useful for some objects (e.g., wrecks) that don't have a formal name.
    return it->second.string_value();
  }
  if (auto it = o.find("type"); it != o.end() && it->second.is_string()) {
    return it->second.string_value();
  }
  return {};
}

} // namespace

const GameEntityIndex& game_entity_index() {
  return g_index;
}

void invalidate_game_entity_index() {
  g_stale = true;
}

bool json_to_u64_id(const nebula4x::json::Value& v, std::uint64_t& out) {
  if (!v.is_number()) return false;
  return parse_u64_from_number(v.number_value(), out);
}

const GameEntityIndexEntry* find_game_entity(std::uint64_t id) {
  auto it = g_index.by_id.find(id);
  if (it == g_index.by_id.end()) return nullptr;
  return &it->second;
}

bool ensure_game_entity_index(const nebula4x::json::Value& root, std::uint64_t revision) {
  if (!g_stale && g_index.revision == revision) return true;

  g_index.by_id.clear();
  g_index.revision = revision;
  g_stale = false;

  const auto* obj = root.as_object();
  if (!obj) return false;

  // Best-effort: index all top-level arrays that contain objects with an "id" field.
  for (const auto& kv : *obj) {
    const std::string& kind = kv.first;
    const auto* arr = kv.second.as_array();
    if (!arr) continue;

    for (std::size_t i = 0; i < arr->size(); ++i) {
      const auto& elem = (*arr)[i];
      const auto* eo = elem.as_object();
      if (!eo) continue;

      auto it_id = eo->find("id");
      if (it_id == eo->end()) continue;

      std::uint64_t id = 0;
      if (!json_to_u64_id(it_id->second, id)) continue;

      GameEntityIndexEntry e;
      e.id = id;
      e.kind = kind;
      // Path = "/" + kind + "/" + i (with token escaping).
      std::string p = "/";
      p = nebula4x::json_pointer_join(p, kind);
      p = nebula4x::json_pointer_join_index(p, i);
      e.path = std::move(p);
      e.name = best_effort_name(*eo);

      g_index.by_id[id] = std::move(e);
    }
  }

  return true;
}

} // namespace nebula4x::ui
