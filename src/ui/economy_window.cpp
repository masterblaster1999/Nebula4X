#include "ui/economy_window.h"

#include <imgui.h>

#include <algorithm>
#include <functional>
#include <cctype>
#include <cmath>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "nebula4x/core/research_planner.h"
#include "nebula4x/util/strings.h"

namespace nebula4x::ui {
namespace {

// Many core containers are stored as std::unordered_map for convenience.
// Iteration order of unordered_map is not specified, so relying on it can
// introduce cross-platform nondeterminism in UI ordering.
template <typename Map>
std::vector<typename Map::key_type> sorted_keys(const Map& m) {
  std::vector<typename Map::key_type> keys;
  keys.reserve(m.size());
  for (const auto& [k, _] : m) keys.push_back(k);
  std::sort(keys.begin(), keys.end());
  return keys;
}

// ImGui expects UTF-8 in `const char*`, but C++20 `u8"..."` literals are `const char8_t*`.
// This helper bridges the type gap without changing the underlying byte sequence.
inline const char* u8_cstr(const char8_t* s) {
  return reinterpret_cast<const char*>(s);
}


bool case_insensitive_contains(const std::string& haystack, const char* needle_cstr) {
  if (!needle_cstr) return true;
  if (needle_cstr[0] == '\0') return true;
  const std::string needle(needle_cstr);
  const auto it = std::search(
      haystack.begin(), haystack.end(), needle.begin(), needle.end(),
      [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
      });
  return it != haystack.end();
}

bool vec_contains(const std::vector<std::string>& v, const std::string& x) {
  return std::find(v.begin(), v.end(), x) != v.end();
}

void push_unique(std::vector<std::string>& v, const std::string& x) {
  if (!vec_contains(v, x)) v.push_back(x);
}

double colony_research_points_per_day(const Simulation& sim, const Colony& c) {
  double rp = 0.0;
  for (const auto& [inst_id, count_raw] : c.installations) {
    const int count = std::max(0, count_raw);
    if (count <= 0) continue;
    const auto it = sim.content().installations.find(inst_id);
    if (it == sim.content().installations.end()) continue;
    rp += std::max(0.0, it->second.research_points_per_day) * static_cast<double>(count);
  }
  return rp;
}

int colony_mining_units(const Simulation& sim, const Colony& c) {
  int mines = 0;
  for (const auto& [inst_id, count_raw] : c.installations) {
    const int count = std::max(0, count_raw);
    if (count <= 0) continue;
    const auto it = sim.content().installations.find(inst_id);
    if (it == sim.content().installations.end()) continue;
    const InstallationDef& def = it->second;
    const bool mining = def.mining ||
                        (!def.mining && nebula4x::to_lower(def.id).find("mine") != std::string::npos);
    if (!mining) continue;
    mines += count;
  }
  return mines;
}

std::unordered_map<std::string, double> colony_mining_request_per_day(const Simulation& sim, const Colony& c) {
  std::unordered_map<std::string, double> out;
  for (const auto& [inst_id, count_raw] : c.installations) {
    const int count = std::max(0, count_raw);
    if (count <= 0) continue;
    const auto it = sim.content().installations.find(inst_id);
    if (it == sim.content().installations.end()) continue;
    const InstallationDef& def = it->second;
    if (def.produces_per_day.empty()) continue;
    const bool mining = def.mining ||
                        (!def.mining && nebula4x::to_lower(def.id).find("mine") != std::string::npos);
    if (!mining) continue;
    for (const auto& [mineral, per_day] : def.produces_per_day) {
      out[mineral] += std::max(0.0, per_day) * static_cast<double>(count);
    }
  }
  return out;
}

std::unordered_map<std::string, double> colony_synthetic_production_per_day(const Simulation& sim, const Colony& c) {
  // "Synthetic" = non-mining produces_per_day (prototype behavior),
  // e.g. Fuel refineries.
  std::unordered_map<std::string, double> out;
  for (const auto& [inst_id, count_raw] : c.installations) {
    const int count = std::max(0, count_raw);
    if (count <= 0) continue;
    const auto it = sim.content().installations.find(inst_id);
    if (it == sim.content().installations.end()) continue;
    const InstallationDef& def = it->second;
    if (def.produces_per_day.empty()) continue;

    const bool mining = def.mining ||
                        (!def.mining && nebula4x::to_lower(def.id).find("mine") != std::string::npos);
    if (mining) continue;

    for (const auto& [mineral, per_day] : def.produces_per_day) {
      out[mineral] += std::max(0.0, per_day) * static_cast<double>(count);
    }
  }
  return out;
}

double get_mineral_tons(const Colony& c, const std::string& mineral) {
  auto it = c.minerals.find(mineral);
  return (it == c.minerals.end()) ? 0.0 : it->second;
}

double get_mineral_reserve(const Colony& c, const std::string& mineral) {
  auto it = c.mineral_reserves.find(mineral);
  return (it == c.mineral_reserves.end()) ? 0.0 : it->second;
}

struct TechTierLayout {
  std::vector<std::vector<std::string>> tiers;
  std::unordered_map<std::string, int> tier_by_id;
};

// Compute a "tier" (distance from prerequisites) layout for techs.
TechTierLayout compute_tech_tiers(const ContentDB& content) {
  TechTierLayout out;

  std::unordered_map<std::string, int> memo;
  std::unordered_set<std::string> visiting;

  std::function<int(const std::string&)> dfs = [&](const std::string& id) -> int {
    auto it = memo.find(id);
    if (it != memo.end()) return it->second;
    if (visiting.count(id)) return 0;  // cycle guard; content validation should prevent.
    visiting.insert(id);

    int t = 0;
    auto it_def = content.techs.find(id);
    if (it_def != content.techs.end()) {
      for (const auto& pre : it_def->second.prereqs) {
        t = std::max(t, dfs(pre) + 1);
      }
    }

    visiting.erase(id);
    memo[id] = t;
    return t;
  };

  int max_tier = 0;
  for (const auto& [id, _] : content.techs) {
    max_tier = std::max(max_tier, dfs(id));
  }

  out.tiers.assign(static_cast<std::size_t>(max_tier + 1), {});
  for (const auto& id : sorted_keys(content.techs)) {
    const int t = dfs(id);
    out.tier_by_id[id] = t;
    out.tiers[static_cast<std::size_t>(t)].push_back(id);
  }

  // Within a tier, sort by tech name (then id) for readability.
  for (auto& tier : out.tiers) {
    std::sort(tier.begin(), tier.end(), [&](const std::string& a, const std::string& b) {
      const auto ita = content.techs.find(a);
      const auto itb = content.techs.find(b);
      const std::string na = (ita == content.techs.end()) ? a : ita->second.name;
      const std::string nb = (itb == content.techs.end()) ? b : itb->second.name;
      if (na != nb) return na < nb;
      return a < b;
    });
  }

  return out;
}


// ---- Tech tree graph view --------------------------------------------------
// The table-based tech list is useful for scanning, but a graph view provides
// much better context for prerequisites and research planning.
struct TechGraphNode {
  std::string id;
  ImVec2 pos_world{0.0f, 0.0f};   // top-left in "world" pixels
  ImVec2 size_world{0.0f, 0.0f};  // size in "world" pixels
  bool match_filter{true};
  bool known{false};
  bool active{false};
  bool queued{false};
  bool prereqs_met{false};
};

inline bool point_in_rect(const ImVec2& p, const ImVec2& a, const ImVec2& b) {
  return (p.x >= a.x && p.x <= b.x && p.y >= a.y && p.y <= b.y);
}

inline ImVec2 world_to_screen(const ImVec2& w, const ImVec2& origin, float zoom, const ImVec2& pan_world) {
  return ImVec2(origin.x + (w.x + pan_world.x) * zoom, origin.y + (w.y + pan_world.y) * zoom);
}

inline ImVec2 screen_to_world(const ImVec2& s, const ImVec2& origin, float zoom, const ImVec2& pan_world) {
  return ImVec2((s.x - origin.x) / zoom - pan_world.x, (s.y - origin.y) / zoom - pan_world.y);
}

static bool prereqs_met_for(const Faction& fac, const TechDef& def) {
  for (const auto& pre : def.prereqs) {
    if (!vec_contains(fac.known_techs, pre)) return false;
  }
  return true;
}

static void collect_prereqs_recursive(const ContentDB& content,
                                      const std::string& tech_id,
                                      std::unordered_set<std::string>& out,
                                      int depth = 0) {
  if (depth > 128) return;
  const auto it = content.techs.find(tech_id);
  if (it == content.techs.end()) return;
  for (const auto& pre : it->second.prereqs) {
    if (out.insert(pre).second) collect_prereqs_recursive(content, pre, out, depth + 1);
  }
}

static std::string ellipsize_for_width(const std::string& s, ImFont* font, float font_size, float max_width) {
  if (!font) return s;
  if (s.empty()) return s;
  const ImVec2 sz = font->CalcTextSizeA(font_size, std::numeric_limits<float>::max(), 0.0f, s.c_str());
  if (sz.x <= max_width) return s;

  const char* ell = "...";
  const ImVec2 ell_sz = font->CalcTextSizeA(font_size, std::numeric_limits<float>::max(), 0.0f, ell);
  if (ell_sz.x > max_width) return "";

  // Binary search for the longest prefix that fits.
  int lo = 0;
  int hi = static_cast<int>(s.size());
  while (lo < hi) {
    const int mid = (lo + hi + 1) / 2;
    const std::string cand = s.substr(0, static_cast<std::size_t>(mid)) + ell;
    const ImVec2 cand_sz = font->CalcTextSizeA(font_size, std::numeric_limits<float>::max(), 0.0f, cand.c_str());
    if (cand_sz.x <= max_width) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  return s.substr(0, static_cast<std::size_t>(lo)) + ell;
}

static void center_view_on_world(const ImVec2& world_center, const ImVec2& canvas_sz, float zoom, ImVec2& pan_world) {
  // screen_center = origin + (world_center + pan) * zoom
  // => pan = screen_center/zoom - world_center
  pan_world.x = (canvas_sz.x * 0.5f) / zoom - world_center.x;
  pan_world.y = (canvas_sz.y * 0.5f) / zoom - world_center.y;
}

static void reset_view_to_bounds(const ImVec2& bounds_min,
                                 const ImVec2& bounds_max,
                                 const ImVec2& canvas_sz,
                                 float& zoom,
                                 ImVec2& pan_world) {
  const float margin = 120.0f;
  const float bw = std::max(1.0f, (bounds_max.x - bounds_min.x) + margin * 2.0f);
  const float bh = std::max(1.0f, (bounds_max.y - bounds_min.y) + margin * 2.0f);

  const float zx = canvas_sz.x / bw;
  const float zy = canvas_sz.y / bh;
  zoom = std::clamp(std::min(zx, zy), 0.25f, 2.50f);

  const ImVec2 center((bounds_min.x + bounds_max.x) * 0.5f, (bounds_min.y + bounds_max.y) * 0.5f);
  center_view_on_world(center, canvas_sz, zoom, pan_world);
}

static ImU32 col32a(int r, int g, int b, float a01) {
  const int a = static_cast<int>(std::clamp(a01, 0.0f, 1.0f) * 255.0f + 0.5f);
  return IM_COL32(r, g, b, a);
}

static void add_arrow(ImDrawList* draw, const ImVec2& tip, const ImVec2& dir_norm, float size, ImU32 col) {
  // Simple triangular arrow head.
  const ImVec2 d(dir_norm.x * size, dir_norm.y * size);
  const ImVec2 n(-dir_norm.y * size * 0.65f, dir_norm.x * size * 0.65f);
  const ImVec2 p0(tip.x, tip.y);
  const ImVec2 p1(tip.x - d.x + n.x, tip.y - d.y + n.y);
  const ImVec2 p2(tip.x - d.x - n.x, tip.y - d.y - n.y);
  draw->AddTriangleFilled(p0, p1, p2, col);
}

static void draw_tech_tree_graph(Simulation& sim,
                                 UIState& ui,
                                 Faction& fac,
                                 const TechTierLayout& layout,
                                 const char* filter,
                                 std::string& selected_tech) {
  // Persistent per-session view state (good enough for now; UI prefs can later persist these).
  static float zoom = 1.0f;
  static ImVec2 pan_world(40.0f, 40.0f);
  static bool init = true;

  static bool show_grid = true;
  static bool show_edges = true;
  static bool dim_non_matching = true;
  static bool hide_non_matching = false;
  static bool show_minimap = true;

  // Layout constants (in world pixels at zoom=1).
  const float node_w = 260.0f;
  const float node_h = 62.0f;
  const float gap_x = 120.0f;
  const float gap_y = 18.0f;

  // Build node positions (cached layout is tiered but we also need per-id coordinates).
  std::unordered_map<std::string, TechGraphNode> nodes;
  nodes.reserve(sim.content().techs.size());

  ImVec2 bounds_min(0.0f, 0.0f);
  ImVec2 bounds_max(0.0f, 0.0f);

  for (std::size_t t = 0; t < layout.tiers.size(); ++t) {
    const auto& tier = layout.tiers[t];
    for (std::size_t r = 0; r < tier.size(); ++r) {
      const std::string& tid = tier[r];
      const auto it = sim.content().techs.find(tid);
      if (it == sim.content().techs.end()) continue;

      const TechDef& def = it->second;

      TechGraphNode n;
      n.id = tid;
      n.pos_world = ImVec2(static_cast<float>(t) * (node_w + gap_x), static_cast<float>(r) * (node_h + gap_y));
      n.size_world = ImVec2(node_w, node_h);

      const std::string hay = def.name + " " + tid;
      n.match_filter = case_insensitive_contains(hay, filter);

      n.known = vec_contains(fac.known_techs, tid);
      n.active = (!fac.active_research_id.empty() && fac.active_research_id == tid);
      n.queued = vec_contains(fac.research_queue, tid);
      n.prereqs_met = prereqs_met_for(fac, def);

      bounds_max.x = std::max(bounds_max.x, n.pos_world.x + n.size_world.x);
      bounds_max.y = std::max(bounds_max.y, n.pos_world.y + n.size_world.y);

      nodes.emplace(tid, std::move(n));
    }
  }

  // If the selected node no longer exists (e.g. content reload), clear.
  if (!selected_tech.empty() && nodes.find(selected_tech) == nodes.end()) selected_tech.clear();

  // --- Controls (rendered above the canvas) ---
  {
    ImGui::Checkbox("Grid", &show_grid);
    ImGui::SameLine();
    ImGui::Checkbox("Edges", &show_edges);
    ImGui::SameLine();
    ImGui::Checkbox("Dim non-matching", &dim_non_matching);
    ImGui::SameLine();
    ImGui::Checkbox("Hide non-matching", &hide_non_matching);
    ImGui::SameLine();
    ImGui::Checkbox("Minimap", &show_minimap);

    ImGui::Spacing();

    bool do_reset = false;
    bool do_focus = false;

    if (ImGui::SmallButton("Reset view (R)")) do_reset = true;
    ImGui::SameLine();

    const bool can_focus = (!selected_tech.empty() && nodes.find(selected_tech) != nodes.end());
    if (!can_focus) ImGui::BeginDisabled();
    if (ImGui::SmallButton("Focus selected (F)")) do_focus = true;
    if (!can_focus) ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextDisabled("Pan: MMB drag   Zoom: Wheel   Select: LMB   Double-click: Set Active (Shift=Queue)");

    // --- Canvas ---
    const ImVec2 canvas_p0 = ImGui::GetCursorScreenPos();
    ImVec2 canvas_sz = ImGui::GetContentRegionAvail();
    canvas_sz.x = std::max(120.0f, canvas_sz.x);
    canvas_sz.y = std::max(220.0f, canvas_sz.y);
    const ImVec2 canvas_p1(canvas_p0.x + canvas_sz.x, canvas_p0.y + canvas_sz.y);

    // Minimap rectangle (computed early so hit-testing can ignore it).
    const float minimap_w = 210.0f;
    const float minimap_h = 140.0f;
    ImVec2 minimap_p0(0.0f, 0.0f);
    ImVec2 minimap_p1(0.0f, 0.0f);
    bool over_minimap = false;
    if (show_minimap) {
      minimap_p1 = ImVec2(canvas_p1.x - 10.0f, canvas_p1.y - 10.0f);
      minimap_p0 = ImVec2(minimap_p1.x - minimap_w, minimap_p1.y - minimap_h);
      over_minimap = point_in_rect(ImGui::GetIO().MousePos, minimap_p0, minimap_p1);
    }

    ImGui::InvisibleButton("tech_tree_canvas", canvas_sz,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();

    ImDrawList* draw = ImGui::GetWindowDrawList();

    // Background.
    draw->AddRectFilled(canvas_p0, canvas_p1, IM_COL32(16, 18, 22, 255));
    draw->AddRect(canvas_p0, canvas_p1, IM_COL32(70, 70, 80, 255));

    // Initial fit-to-view.
    if (init) {
      reset_view_to_bounds(bounds_min, bounds_max, canvas_sz, zoom, pan_world);
      init = false;
    }

    // Keyboard shortcuts only when canvas is hovered to avoid stealing keys.
    if (hovered && !ImGui::GetIO().WantTextInput) {
      if (ImGui::IsKeyPressed(ImGuiKey_R)) do_reset = true;
      if (ImGui::IsKeyPressed(ImGuiKey_F)) do_focus = true;
    }

    // Apply requested actions.
    if (do_reset) {
      reset_view_to_bounds(bounds_min, bounds_max, canvas_sz, zoom, pan_world);
    }
    if (do_focus && can_focus) {
      const auto itn = nodes.find(selected_tech);
      if (itn != nodes.end()) {
        const ImVec2 c(itn->second.pos_world.x + itn->second.size_world.x * 0.5f,
                       itn->second.pos_world.y + itn->second.size_world.y * 0.5f);
        center_view_on_world(c, canvas_sz, zoom, pan_world);
      }
    }

    // Panning.
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
      const ImVec2 d = ImGui::GetIO().MouseDelta;
      pan_world.x += d.x / zoom;
      pan_world.y += d.y / zoom;
    }

    // Zoom-to-cursor.
    if (hovered) {
      const float wheel = ImGui::GetIO().MouseWheel;
      if (wheel != 0.0f) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const ImVec2 before = screen_to_world(mouse, canvas_p0, zoom, pan_world);

        const float zoom_factor = std::pow(1.18f, wheel);
        zoom = std::clamp(zoom * zoom_factor, 0.20f, 3.00f);

        // Adjust pan so the world point under the cursor remains stable.
        pan_world.x = (mouse.x - canvas_p0.x) / zoom - before.x;
        pan_world.y = (mouse.y - canvas_p0.y) / zoom - before.y;
      }
    }

    // Clip all drawlist operations to the canvas.
    draw->PushClipRect(canvas_p0, canvas_p1, true);

    // Grid.
    if (show_grid) {
      float step = 120.0f;
      // Keep grid density reasonable on screen.
      while (step * zoom < 60.0f) step *= 2.0f;
      while (step * zoom > 240.0f) step *= 0.5f;

      const float major = step * 5.0f;

      const float min_x = -pan_world.x;
      const float max_x = (canvas_sz.x / zoom) - pan_world.x;
      const float min_y = -pan_world.y;
      const float max_y = (canvas_sz.y / zoom) - pan_world.y;

      auto floor_to = [](float v, float s) {
        return std::floor(v / s) * s;
      };

      for (float x = floor_to(min_x, step); x <= max_x; x += step) {
        const float sx = canvas_p0.x + (x + pan_world.x) * zoom;
        draw->AddLine(ImVec2(sx, canvas_p0.y), ImVec2(sx, canvas_p1.y), IM_COL32(60, 60, 70, 30), 1.0f);
      }
      for (float y = floor_to(min_y, step); y <= max_y; y += step) {
        const float sy = canvas_p0.y + (y + pan_world.y) * zoom;
        draw->AddLine(ImVec2(canvas_p0.x, sy), ImVec2(canvas_p1.x, sy), IM_COL32(60, 60, 70, 30), 1.0f);
      }

      for (float x = floor_to(min_x, major); x <= max_x; x += major) {
        const float sx = canvas_p0.x + (x + pan_world.x) * zoom;
        draw->AddLine(ImVec2(sx, canvas_p0.y), ImVec2(sx, canvas_p1.y), IM_COL32(90, 90, 100, 50), 1.0f);
      }
      for (float y = floor_to(min_y, major); y <= max_y; y += major) {
        const float sy = canvas_p0.y + (y + pan_world.y) * zoom;
        draw->AddLine(ImVec2(canvas_p0.x, sy), ImVec2(canvas_p1.x, sy), IM_COL32(90, 90, 100, 50), 1.0f);
      }
    }

    // Highlight prerequisite chain for the selected tech.
    std::unordered_set<std::string> prereq_chain;
    if (!selected_tech.empty()) {
      collect_prereqs_recursive(sim.content(), selected_tech, prereq_chain);
    }

    // Determine hovered node (manual hit test).
    std::string hovered_id;
    if (hovered && !over_minimap) {
      const ImVec2 m = ImGui::GetIO().MousePos;
      for (const auto& [tid, n] : nodes) {
        if (hide_non_matching && !n.match_filter) continue;
        const ImVec2 a = world_to_screen(n.pos_world, canvas_p0, zoom, pan_world);
        const ImVec2 b(a.x + n.size_world.x * zoom, a.y + n.size_world.y * zoom);
        if (point_in_rect(m, a, b)) {
          hovered_id = tid;
          break;
        }
      }
    }

    // Edges (behind nodes).
    if (show_edges) {
      for (const auto& [tid, n] : nodes) {
        if (hide_non_matching && !n.match_filter) continue;

        const auto it = sim.content().techs.find(tid);
        if (it == sim.content().techs.end()) continue;

        const TechDef& def = it->second;
        if (def.prereqs.empty()) continue;

        for (const auto& pre : def.prereqs) {
          const auto itp = nodes.find(pre);
          if (itp == nodes.end()) continue;

          const TechGraphNode& a_node = itp->second;
          if (hide_non_matching && !a_node.match_filter) continue;

          const ImVec2 start_w(a_node.pos_world.x + a_node.size_world.x, a_node.pos_world.y + a_node.size_world.y * 0.5f);
          const ImVec2 end_w(n.pos_world.x, n.pos_world.y + n.size_world.y * 0.5f);

          const ImVec2 start = world_to_screen(start_w, canvas_p0, zoom, pan_world);
          const ImVec2 end = world_to_screen(end_w, canvas_p0, zoom, pan_world);

          // Visual priority: selected chain edges pop more.
          const bool in_chain = (!selected_tech.empty() && (tid == selected_tech || prereq_chain.count(tid) || prereq_chain.count(pre)));

          float a01 = 0.28f;
          if (in_chain) a01 = 0.75f;
          if (dim_non_matching && filter && filter[0] != '\0') {
            if (!n.match_filter && !a_node.match_filter && !in_chain) a01 *= 0.25f;
          }

          const float thickness = std::max(1.0f, 2.2f * zoom * (in_chain ? 1.15f : 1.0f));
          const ImU32 col = in_chain ? col32a(210, 210, 255, a01) : col32a(170, 170, 190, a01);

          const float dx = std::max(40.0f, 90.0f * zoom);
          const ImVec2 cp1(start.x + dx, start.y);
          const ImVec2 cp2(end.x - dx, end.y);

          draw->AddBezierCubic(start, cp1, cp2, end, col, thickness, 0);

          // Arrow head at end.
          ImVec2 dir(end.x - cp2.x, end.y - cp2.y);
          const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
          if (len > 0.0001f) {
            dir.x /= len;
            dir.y /= len;
            add_arrow(draw, end, dir, std::max(5.0f, 7.0f * zoom), col);
          }
        }
      }
    }

    // Nodes (foreground).
    ImFont* font = ImGui::GetFont();
    const float base_font_size = ImGui::GetFontSize();

    for (const auto& [tid, n] : nodes) {
      if (hide_non_matching && !n.match_filter) continue;

      const auto it = sim.content().techs.find(tid);
      if (it == sim.content().techs.end()) continue;
      const TechDef& def = it->second;

      const bool is_sel = (selected_tech == tid);
      const bool is_hover = (!hovered_id.empty() && hovered_id == tid);
      const bool in_chain = (!selected_tech.empty() && (tid == selected_tech || prereq_chain.count(tid)));

      float alpha = 1.0f;
      if (dim_non_matching && filter && filter[0] != '\0' && !n.match_filter && !in_chain) alpha = 0.25f;

      // Base palette by status.
      ImU32 border = IM_COL32(120, 120, 130, 255);
      ImU32 fill = IM_COL32(28, 30, 36, 255);
      ImU32 fill_hi = IM_COL32(42, 44, 52, 255);

      if (n.known) {
        border = IM_COL32(90, 235, 150, 255);
        fill = IM_COL32(18, 44, 28, 255);
        fill_hi = IM_COL32(26, 66, 40, 255);
      } else if (n.active) {
        border = IM_COL32(255, 220, 120, 255);
        fill = IM_COL32(56, 42, 18, 255);
        fill_hi = IM_COL32(78, 60, 22, 255);
      } else if (n.queued) {
        border = IM_COL32(170, 210, 255, 255);
        fill = IM_COL32(18, 30, 52, 255);
        fill_hi = IM_COL32(24, 44, 78, 255);
      } else if (n.prereqs_met) {
        border = IM_COL32(210, 210, 220, 255);
        fill = IM_COL32(34, 34, 42, 255);
        fill_hi = IM_COL32(46, 46, 58, 255);
      } else {
        border = IM_COL32(120, 120, 120, 255);
        fill = IM_COL32(26, 26, 30, 255);
        fill_hi = IM_COL32(34, 34, 40, 255);
      }

      const ImVec2 a = world_to_screen(n.pos_world, canvas_p0, zoom, pan_world);
      const ImVec2 b(a.x + n.size_world.x * zoom, a.y + n.size_world.y * zoom);

      const float rounding = std::max(4.0f, 9.0f * zoom);

      // Drop shadow (subtle).
      draw->AddRectFilled(ImVec2(a.x + 3.0f, a.y + 3.0f), ImVec2(b.x + 3.0f, b.y + 3.0f), col32a(0, 0, 0, 0.35f * alpha),
                          rounding);

      // Body.
      draw->AddRectFilled(a, b, col32a((fill >> IM_COL32_R_SHIFT) & 0xFF, (fill >> IM_COL32_G_SHIFT) & 0xFF,
                                      (fill >> IM_COL32_B_SHIFT) & 0xFF, alpha),
                          rounding);

      // Inner highlight band (fake gradient).
      const float inset = std::max(1.0f, 2.0f * zoom);
      const ImVec2 g0(a.x + inset, a.y + inset);
      const ImVec2 g1(b.x - inset, a.y + (b.y - a.y) * 0.55f);

      draw->AddRectFilledMultiColor(g0, g1,
                                    col32a((fill_hi >> IM_COL32_R_SHIFT) & 0xFF, (fill_hi >> IM_COL32_G_SHIFT) & 0xFF,
                                           (fill_hi >> IM_COL32_B_SHIFT) & 0xFF, 0.95f * alpha),
                                    col32a((fill_hi >> IM_COL32_R_SHIFT) & 0xFF, (fill_hi >> IM_COL32_G_SHIFT) & 0xFF,
                                           (fill_hi >> IM_COL32_B_SHIFT) & 0xFF, 0.95f * alpha),
                                    col32a((fill >> IM_COL32_R_SHIFT) & 0xFF, (fill >> IM_COL32_G_SHIFT) & 0xFF,
                                           (fill >> IM_COL32_B_SHIFT) & 0xFF, 0.55f * alpha),
                                    col32a((fill >> IM_COL32_R_SHIFT) & 0xFF, (fill >> IM_COL32_G_SHIFT) & 0xFF,
                                           (fill >> IM_COL32_B_SHIFT) & 0xFF, 0.55f * alpha));

      // Outline.
      draw->AddRect(a, b, col32a((border >> IM_COL32_R_SHIFT) & 0xFF, (border >> IM_COL32_G_SHIFT) & 0xFF,
                                (border >> IM_COL32_B_SHIFT) & 0xFF, alpha),
                    rounding, 0, std::max(1.0f, 1.6f * zoom));

      if (is_sel) {
        draw->AddRect(ImVec2(a.x - 2.0f, a.y - 2.0f), ImVec2(b.x + 2.0f, b.y + 2.0f), col32a(255, 255, 255, 0.65f),
                      rounding + 1.0f, 0, std::max(1.5f, 2.6f * zoom));
      } else if (is_hover) {
        draw->AddRect(ImVec2(a.x - 1.5f, a.y - 1.5f), ImVec2(b.x + 1.5f, b.y + 1.5f), col32a(255, 255, 255, 0.35f * alpha),
                      rounding + 1.0f, 0, std::max(1.0f, 2.1f * zoom));
      }

      // Status glyph prefix.
      std::string prefix;
      if (n.known) prefix = u8_cstr(u8"✓ ");
      else if (n.active) prefix = u8_cstr(u8"▶ ");
      else if (n.queued) prefix = u8_cstr(u8"⏳ ");
      else if (n.prereqs_met) prefix = u8_cstr(u8"• ");
      else prefix = "  ";

      const float font_size = base_font_size * zoom;
      const float font_size_small = base_font_size * zoom * 0.78f;
      const float pad = std::max(4.0f, 8.0f * zoom);

      // Title (clipped).
      const float max_text_w = std::max(10.0f, (b.x - a.x) - pad * 2.0f);
      const std::string title = prefix + ellipsize_for_width(def.name, font, font_size, max_text_w);

      draw->PushClipRect(a, b, true);
      draw->AddText(font, font_size, ImVec2(a.x + pad, a.y + pad), col32a(245, 245, 250, alpha), title.c_str());

      // Subline: cost + short id.
      {
        std::string sub = "Cost " + std::to_string(static_cast<int>(std::round(def.cost)));
        if (!def.id.empty()) {
          sub += "  •  " + ellipsize_for_width(def.id, font, font_size_small, max_text_w);
        }
        draw->AddText(font, font_size_small, ImVec2(a.x + pad, a.y + pad + font_size * 1.05f),
                      col32a(210, 210, 220, 0.92f * alpha), sub.c_str());
      }
      draw->PopClipRect();

      // Small status badge (top-right).
      {
        const float r = std::max(4.0f, 6.0f * zoom);
        const ImVec2 c(b.x - pad * 0.75f, a.y + pad * 0.75f);
        ImU32 badge = border;
        if (!n.prereqs_met && !n.known && !n.active && !n.queued) badge = IM_COL32(120, 120, 120, 255);
        draw->AddCircleFilled(c, r, col32a((badge >> IM_COL32_R_SHIFT) & 0xFF, (badge >> IM_COL32_G_SHIFT) & 0xFF,
                                          (badge >> IM_COL32_B_SHIFT) & 0xFF, 0.85f * alpha),
                              0);
        draw->AddCircle(c, r, col32a(0, 0, 0, 0.40f * alpha), 0, std::max(1.0f, 1.3f * zoom));
      }
    }

    // Canvas interactions (selection + quick actions).
    if (!hovered_id.empty()) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

      const auto itn = nodes.find(hovered_id);
      const auto it_def = sim.content().techs.find(hovered_id);
      if (itn != nodes.end() && it_def != sim.content().techs.end()) {
        TechGraphNode& n = itn->second;
        const TechDef& def = it_def->second;

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
          selected_tech = hovered_id;
        }

        // Double-click quick action.
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
          if (!n.known && n.prereqs_met) {
            if (ImGui::GetIO().KeyShift) {
              push_unique(fac.research_queue, def.id);
            } else {
              fac.active_research_id = def.id;
              fac.active_research_progress = 0.0;
              fac.research_queue.erase(std::remove(fac.research_queue.begin(), fac.research_queue.end(), def.id),
                                       fac.research_queue.end());
              ui.request_details_tab = DetailsTab::Research;
            }
          }
        }

        // Tooltip with details + actions.
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
          ImGui::BeginTooltip();
          ImGui::Text("%s", def.name.c_str());
          ImGui::TextDisabled("%s", def.id.c_str());
          ImGui::Separator();
          ImGui::Text("Cost: %.0f", def.cost);

          const bool known = n.known;
          const bool active_now = n.active;
          const bool queued_now = n.queued;
          const bool avail = n.prereqs_met;

          if (known) ImGui::TextColored(ImVec4(0.47f, 1.0f, 0.55f, 1.0f), "Status: Known");
          else if (active_now) ImGui::TextColored(ImVec4(1.0f, 0.86f, 0.47f, 1.0f), "Status: Active");
          else if (queued_now) ImGui::TextColored(ImVec4(0.65f, 0.82f, 1.0f, 1.0f), "Status: Queued");
          else if (avail) ImGui::Text("Status: Available");
          else ImGui::TextDisabled("Status: Locked");

          if (!def.prereqs.empty()) {
            ImGui::Separator();
            ImGui::Text("Prereqs:");
            for (const auto& pre : def.prereqs) {
              const bool have = vec_contains(fac.known_techs, pre);
              ImGui::BulletText("%s%s", pre.c_str(), have ? " (known)" : " (missing)");
            }
          }

          ImGui::Separator();
          ImGui::Text("Actions");
          if (!known && avail) {
            if (ImGui::SmallButton("Set Active")) {
              fac.active_research_id = def.id;
              fac.active_research_progress = 0.0;
              fac.research_queue.erase(std::remove(fac.research_queue.begin(), fac.research_queue.end(), def.id),
                                       fac.research_queue.end());
              ui.request_details_tab = DetailsTab::Research;
            }
            ImGui::SameLine();
            if (!queued_now) {
              if (ImGui::SmallButton("Queue")) push_unique(fac.research_queue, def.id);
            } else {
              if (ImGui::SmallButton("Unqueue")) {
                fac.research_queue.erase(std::remove(fac.research_queue.begin(), fac.research_queue.end(), def.id),
                                         fac.research_queue.end());
              }
            }
          } else {
            ImGui::TextDisabled("(no actions)");
          }

          ImGui::EndTooltip();
        }
      }
    }

    // Minimap (bottom-right overlay).
    if (show_minimap) {
      const float mm_w = minimap_w;
      const float mm_h = minimap_h;
      const ImVec2 mm_p0 = minimap_p0;
      const ImVec2 mm_p1 = minimap_p1;

      // Background.
      draw->AddRectFilled(mm_p0, mm_p1, IM_COL32(0, 0, 0, 120), 6.0f);
      draw->AddRect(mm_p0, mm_p1, IM_COL32(160, 160, 180, 80), 6.0f);

      const float bw = std::max(1.0f, bounds_max.x - bounds_min.x);
      const float bh = std::max(1.0f, bounds_max.y - bounds_min.y);

      auto world_to_mm = [&](const ImVec2& w) -> ImVec2 {
        const float nx = (w.x - bounds_min.x) / bw;
        const float ny = (w.y - bounds_min.y) / bh;
        return ImVec2(mm_p0.x + nx * mm_w, mm_p0.y + ny * mm_h);
      };

      // Nodes as dots.
      for (const auto& [tid, n] : nodes) {
        if (hide_non_matching && !n.match_filter) continue;
        float a01 = 0.55f;
        if (dim_non_matching && filter && filter[0] != '\0' && !n.match_filter) a01 *= 0.25f;
        const ImVec2 c = world_to_mm(ImVec2(n.pos_world.x + n.size_world.x * 0.5f, n.pos_world.y + n.size_world.y * 0.5f));
        draw->AddCircleFilled(c, 2.2f, col32a(220, 220, 235, a01), 0);
      }

      // View rectangle.
      const float view_w = canvas_sz.x / zoom;
      const float view_h = canvas_sz.y / zoom;
      const ImVec2 view_min(-pan_world.x, -pan_world.y);
      const ImVec2 view_max(view_min.x + view_w, view_min.y + view_h);

      const ImVec2 vm0 = world_to_mm(view_min);
      const ImVec2 vm1 = world_to_mm(view_max);
      draw->AddRect(vm0, vm1, IM_COL32(255, 255, 255, 120));

      // Click minimap to recenter.
      if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        if (point_in_rect(m, mm_p0, mm_p1)) {
          const float nx = std::clamp((m.x - mm_p0.x) / mm_w, 0.0f, 1.0f);
          const float ny = std::clamp((m.y - mm_p0.y) / mm_h, 0.0f, 1.0f);
          const ImVec2 w(bounds_min.x + nx * bw, bounds_min.y + ny * bh);
          center_view_on_world(w, canvas_sz, zoom, pan_world);
        }
      }
    }

    draw->PopClipRect();
  }
}


}  // namespace

void draw_economy_window(Simulation& sim, UIState& ui, Id& selected_colony, Id& selected_body) {
  if (!ui.show_economy_window) return;

  if (!ImGui::Begin("Economy", &ui.show_economy_window)) {
    ImGui::End();
    return;
  }

  GameState& s = sim.state();
  const Date::YMD ymd = s.date.to_ymd();
  ImGui::Text("Date: %04d-%02d-%02d", ymd.year, ymd.month, ymd.day);

  // --- Faction selector ---
  static Id view_faction_id = kInvalidId;

  auto faction_ids = sorted_keys(s.factions);

  auto ensure_valid_faction = [&]() {
    if (view_faction_id != kInvalidId && s.factions.find(view_faction_id) != s.factions.end()) return;

    // Prefer the current viewer faction, then the selected colony faction.
    if (ui.viewer_faction_id != kInvalidId && s.factions.find(ui.viewer_faction_id) != s.factions.end()) {
      view_faction_id = ui.viewer_faction_id;
      return;
    }
    if (selected_colony != kInvalidId) {
      if (const Colony* c = find_ptr(s.colonies, selected_colony)) {
        if (s.factions.find(c->faction_id) != s.factions.end()) {
          view_faction_id = c->faction_id;
          return;
        }
      }
    }
    view_faction_id = faction_ids.empty() ? kInvalidId : faction_ids.front();
  };
  ensure_valid_faction();

  if (view_faction_id == kInvalidId) {
    ImGui::TextDisabled("No factions in game state.");
    ImGui::End();
    return;
  }

  const Faction* view_faction = find_ptr(s.factions, view_faction_id);

  // Combo list.
  {
    std::string label = view_faction ? view_faction->name : std::string("(unknown)");
    if (ImGui::BeginCombo("Faction", label.c_str())) {
      for (Id fid : faction_ids) {
        const Faction* f = find_ptr(s.factions, fid);
        if (!f) continue;
        const bool sel = (fid == view_faction_id);
        if (ImGui::Selectable((f->name + "##econ_faction_" + std::to_string(static_cast<unsigned long long>(fid))).c_str(), sel)) {
          view_faction_id = fid;
        }
        if (sel) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
  }

  if (ImGui::BeginTabBar("economy_tabs")) {
    // --- Industry ---
    if (ImGui::BeginTabItem("Industry")) {
      std::vector<Id> colony_ids;
      colony_ids.reserve(s.colonies.size());
      for (Id cid : sorted_keys(s.colonies)) {
        const Colony* c = find_ptr(s.colonies, cid);
        if (!c || c->faction_id != view_faction_id) continue;
        colony_ids.push_back(cid);
      }

      double total_pop = 0.0;
      double total_cp = 0.0;
      double total_rp = 0.0;
      int total_mines = 0;
      int total_shipyards = 0;

      for (Id cid : colony_ids) {
        const Colony* c = find_ptr(s.colonies, cid);
        if (!c) continue;
        total_pop += std::max(0.0, c->population_millions);
        total_cp += std::max(0.0, sim.construction_points_per_day(*c));
        total_rp += std::max(0.0, colony_research_points_per_day(sim, *c));
        total_mines += colony_mining_units(sim, *c);
        total_shipyards += (c->installations.count("shipyard") ? c->installations.at("shipyard") : 0);
      }

      ImGui::Text("Colonies: %d", static_cast<int>(colony_ids.size()));
      ImGui::SameLine();
      ImGui::Text("Population: %.1f M", total_pop);
      ImGui::SameLine();
      ImGui::Text("CP/day: %.1f", total_cp);
      ImGui::SameLine();
      ImGui::Text("RP/day: %.1f", total_rp);

      const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                    ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX |
                                    ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
      const float table_h = std::max(200.0f, ImGui::GetContentRegionAvail().y * 0.70f);
      if (ImGui::BeginTable("economy_industry_table", 13, flags, ImVec2(0, table_h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Colony", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("System", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Pop (M)", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("CP/d", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("RP/d", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Mines", ImGuiTableColumnFlags_WidthFixed, 45.0f);
        ImGui::TableSetupColumn("Dur/d", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Neu/d", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Fuel/d", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Yards", ImGuiTableColumnFlags_WidthFixed, 45.0f);
        ImGui::TableSetupColumn("CQ", ImGuiTableColumnFlags_WidthFixed, 35.0f);
        ImGui::TableSetupColumn("SQ", ImGuiTableColumnFlags_WidthFixed, 35.0f);
        ImGui::TableSetupColumn("Fuel", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableHeadersRow();

        for (Id cid : colony_ids) {
          const Colony* c = find_ptr(s.colonies, cid);
          if (!c) continue;
          const Body* b = find_ptr(s.bodies, c->body_id);
          const StarSystem* sys = b ? find_ptr(s.systems, b->system_id) : nullptr;

          const double pop = std::max(0.0, c->population_millions);
          const double cp = std::max(0.0, sim.construction_points_per_day(*c));
          const double rp = std::max(0.0, colony_research_points_per_day(sim, *c));

          const int mines = colony_mining_units(sim, *c);
          const auto mine_req = colony_mining_request_per_day(sim, *c);
          const double dur_d = mine_req.count("Duranium") ? mine_req.at("Duranium") : 0.0;
          const double neu_d = mine_req.count("Neutronium") ? mine_req.at("Neutronium") : 0.0;

          const auto synth = colony_synthetic_production_per_day(sim, *c);
          const double fuel_d = synth.count("Fuel") ? synth.at("Fuel") : 0.0;

          const int yards = c->installations.count("shipyard") ? c->installations.at("shipyard") : 0;
          const int cq = static_cast<int>(c->construction_queue.size());
          const int sq = static_cast<int>(c->shipyard_queue.size());

          const double fuel_stock = get_mineral_tons(*c, "Fuel");

          ImGui::TableNextRow();

          ImGui::TableSetColumnIndex(0);
          const bool is_sel = (selected_colony == cid);
          const std::string label = c->name + "##econ_col_" + std::to_string(static_cast<unsigned long long>(cid));
          if (ImGui::Selectable(label.c_str(), is_sel, ImGuiSelectableFlags_SpanAllColumns)) {
            selected_colony = cid;
            selected_body = c->body_id;
          }

          ImGui::TableSetColumnIndex(1);
          ImGui::TextUnformatted(sys ? sys->name.c_str() : "(unknown)");

          ImGui::TableSetColumnIndex(2);
          ImGui::Text("%.1f", pop);

          ImGui::TableSetColumnIndex(3);
          ImGui::Text("%.1f", cp);

          ImGui::TableSetColumnIndex(4);
          ImGui::Text("%.1f", rp);

          ImGui::TableSetColumnIndex(5);
          ImGui::Text("%d", mines);

          ImGui::TableSetColumnIndex(6);
          ImGui::Text("%.1f", dur_d);

          ImGui::TableSetColumnIndex(7);
          ImGui::Text("%.1f", neu_d);

          ImGui::TableSetColumnIndex(8);
          ImGui::Text("%.1f", fuel_d);

          ImGui::TableSetColumnIndex(9);
          ImGui::Text("%d", yards);

          ImGui::TableSetColumnIndex(10);
          ImGui::Text("%d", cq);

          ImGui::TableSetColumnIndex(11);
          ImGui::Text("%d", sq);

          ImGui::TableSetColumnIndex(12);
          ImGui::Text("%.1f", fuel_stock);
        }

        ImGui::EndTable();
      }

      ImGui::Separator();
      ImGui::TextDisabled("Tip: set per-colony mineral reserves in the Colony tab to keep local stockpiles safe from auto-freight.");

      ImGui::EndTabItem();
    }

    // --- Mining ---
    if (ImGui::BeginTabItem("Mining")) {
      static char body_filter[96] = "";
      static Id body_sel = kInvalidId;

      // Build list of bodies that have deposits OR have a colony with mines.
      struct BodyRow {
        Id body_id{kInvalidId};
        std::string label;
      };

      std::vector<BodyRow> body_rows;
      body_rows.reserve(s.bodies.size());

      // Precompute which bodies have any mines (any faction).
      std::unordered_set<Id> bodies_with_mines;
      for (Id cid : sorted_keys(s.colonies)) {
        const Colony* c = find_ptr(s.colonies, cid);
        if (!c) continue;
        const Body* b = find_ptr(s.bodies, c->body_id);
        if (!b) continue;
        if (colony_mining_units(sim, *c) > 0) bodies_with_mines.insert(b->id);
      }

      for (Id bid : sorted_keys(s.bodies)) {
        const Body* b = find_ptr(s.bodies, bid);
        if (!b) continue;
        const bool has_deposits = !b->mineral_deposits.empty();
        const bool has_mines = bodies_with_mines.count(bid) != 0;
        if (!has_deposits && !has_mines) continue;

        const StarSystem* sys = find_ptr(s.systems, b->system_id);
        const std::string label = (sys ? sys->name : std::string("(unknown)")) + " / " + b->name;
        body_rows.push_back(BodyRow{bid, label});
      }

      if (body_sel == kInvalidId) {
        // Prefer current selection, then first available.
        if (selected_body != kInvalidId) body_sel = selected_body;
        if (body_sel == kInvalidId && !body_rows.empty()) body_sel = body_rows.front().body_id;
      }

      // Left list / right details.
      const float left_w = 280.0f;
      ImGui::BeginChild("mining_left", ImVec2(left_w, 0), true);
      ImGui::Text("Bodies");
      ImGui::InputText("Filter##mining_body_filter", body_filter, IM_ARRAYSIZE(body_filter));
      ImGui::Separator();

      for (const auto& row : body_rows) {
        if (!case_insensitive_contains(row.label, body_filter)) continue;
        const bool sel = (row.body_id == body_sel);
        if (ImGui::Selectable((row.label + "##mine_body_" + std::to_string(static_cast<unsigned long long>(row.body_id))).c_str(), sel)) {
          body_sel = row.body_id;
          selected_body = row.body_id;
        }
      }
      ImGui::EndChild();

      ImGui::SameLine();

      ImGui::BeginChild("mining_right", ImVec2(0, 0), true);

      const Body* body = find_ptr(s.bodies, body_sel);
      if (!body) {
        ImGui::TextDisabled("Select a body.");
        ImGui::EndChild();
        ImGui::EndTabItem();
      } else {
        const StarSystem* sys = find_ptr(s.systems, body->system_id);
        ImGui::Text("%s", body->name.c_str());
        ImGui::TextDisabled("System: %s", sys ? sys->name.c_str() : "(unknown)");

        // Gather colonies on this body (all factions).
        struct ColMining {
          Id colony_id{kInvalidId};
          std::unordered_map<std::string, double> req;
        };

        std::vector<ColMining> cols;
        for (Id cid : sorted_keys(s.colonies)) {
          const Colony* c = find_ptr(s.colonies, cid);
          if (!c) continue;
          if (c->body_id != body->id) continue;
          cols.push_back(ColMining{cid, colony_mining_request_per_day(sim, *c)});
        }

        // Mineral -> list of (colony, req)
        struct ReqEntry { Id colony_id; double req; };
        std::unordered_map<std::string, std::vector<ReqEntry>> req_by_mineral;
        for (const auto& cm : cols) {
          for (const auto& [mineral, req] : cm.req) {
            if (req <= 1e-9) continue;
            req_by_mineral[mineral].push_back(ReqEntry{cm.colony_id, req});
          }
        }

        // Union minerals: deposits + req.
        std::vector<std::string> minerals;
        for (const auto& [m, _] : body->mineral_deposits) minerals.push_back(m);
        for (const auto& [m, _] : req_by_mineral) minerals.push_back(m);
        std::sort(minerals.begin(), minerals.end());
        minerals.erase(std::unique(minerals.begin(), minerals.end()), minerals.end());

        // Compute allocation based on current deposits.
        // mineral -> colony -> actual/day
        std::unordered_map<std::string, std::unordered_map<Id, double>> actual_by_mineral;
        std::unordered_map<std::string, double> total_req_by_mineral;

        for (const auto& mineral : minerals) {
          double total_req = 0.0;
          if (auto it = req_by_mineral.find(mineral); it != req_by_mineral.end()) {
            for (const auto& e : it->second) total_req += std::max(0.0, e.req);
          }
          total_req_by_mineral[mineral] = total_req;

          const double left = body->mineral_deposits.count(mineral) ? body->mineral_deposits.at(mineral)
                                                                    : std::numeric_limits<double>::infinity();
          const double actual_total = (std::isfinite(left) ? std::min(left, total_req) : total_req);
          const double ratio = (total_req > 1e-12) ? (actual_total / total_req) : 0.0;

          if (auto it = req_by_mineral.find(mineral); it != req_by_mineral.end()) {
            for (const auto& e : it->second) {
              actual_by_mineral[mineral][e.colony_id] += e.req * ratio;
            }
          }
        }

        ImGui::Separator();
        ImGui::Text("Deposits / depletion");
        if (minerals.empty()) {
          ImGui::TextDisabled("(no deposits / no mines)");
        } else {
          const ImGuiTableFlags dflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                         ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
          if (ImGui::BeginTable("mining_deposits_table", 6, dflags)) {
            ImGui::TableSetupColumn("Mineral", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Left", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Req/d", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Act/d", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("ETA (d)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("ETA (y)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableHeadersRow();

            for (const auto& mineral : minerals) {
              const double left = body->mineral_deposits.count(mineral) ? body->mineral_deposits.at(mineral)
                                                                        : std::numeric_limits<double>::infinity();
              const double req = total_req_by_mineral.count(mineral) ? total_req_by_mineral.at(mineral) : 0.0;
              const double act = (std::isfinite(left) ? std::min(left, req) : req);

              ImGui::TableNextRow();

              ImGui::TableSetColumnIndex(0);
              ImGui::TextUnformatted(mineral.c_str());

              ImGui::TableSetColumnIndex(1);
              if (std::isfinite(left)) ImGui::Text("%.0f", left);
              else ImGui::TextDisabled("∞");

              ImGui::TableSetColumnIndex(2);
              ImGui::Text("%.2f", req);

              ImGui::TableSetColumnIndex(3);
              ImGui::Text("%.2f", act);

              ImGui::TableSetColumnIndex(4);
              if (std::isfinite(left) && req > 1e-9) {
                const double eta_d = left / req;
                ImGui::Text("%.0f", eta_d);
              } else if (std::isfinite(left)) {
                ImGui::TextDisabled("-");
              } else {
                ImGui::TextDisabled("∞");
              }

              ImGui::TableSetColumnIndex(5);
              if (std::isfinite(left) && req > 1e-9) {
                const double eta_y = (left / req) / 365.25;
                ImGui::Text("%.1f", eta_y);
              } else if (std::isfinite(left)) {
                ImGui::TextDisabled("-");
              } else {
                ImGui::TextDisabled("∞");
              }
            }

            ImGui::EndTable();
          }
        }

        ImGui::Separator();
        ImGui::Text("Colony mining (predicted for today)");
        if (cols.empty()) {
          ImGui::TextDisabled("(no colonies on this body)");
        } else {
          for (const auto& cm : cols) {
            const Colony* c = find_ptr(s.colonies, cm.colony_id);
            if (!c) continue;
            const Faction* f = find_ptr(s.factions, c->faction_id);
            const std::string header = c->name + " (" + (f ? f->name : std::string("Unknown")) + ")";
            if (ImGui::TreeNode((header + "##mine_col_" + std::to_string(static_cast<unsigned long long>(c->id))).c_str())) {
              const int mine_units = colony_mining_units(sim, *c);
              ImGui::Text("Mines: %d", mine_units);

              // Show actual mining per mineral for this colony.
              std::vector<std::string> mlist;
              for (const auto& [m, _] : actual_by_mineral) mlist.push_back(m);
              std::sort(mlist.begin(), mlist.end());

              bool any = false;
              for (const auto& m : mlist) {
                const double act = actual_by_mineral[m].count(c->id) ? actual_by_mineral[m].at(c->id) : 0.0;
                if (act <= 1e-9) continue;
                any = true;
                ImGui::BulletText("%s: %.2f / day", m.c_str(), act);
              }
              if (!any) ImGui::TextDisabled("(no active mining)");

              ImGui::TreePop();
            }
          }
        }

        ImGui::EndChild();
        ImGui::EndTabItem();
      }
    }

    // --- Tech Tree ---
    if (ImGui::BeginTabItem("Tech Tree")) {
      const auto itf = s.factions.find(view_faction_id);
      if (itf == s.factions.end()) {
        ImGui::TextDisabled("Faction not found.");
        ImGui::EndTabItem();
      } else {
        Faction& fac = s.factions.at(view_faction_id);

        static char tech_filter[128] = "";
        static std::string selected_tech;
        static bool graph_view = true;

        // Cache tiers (content is static).
        static int cached_tech_count = -1;
        static TechTierLayout cached_layout;

        if (cached_tech_count != static_cast<int>(sim.content().techs.size())) {
          cached_layout = compute_tech_tiers(sim.content());
          cached_tech_count = static_cast<int>(sim.content().techs.size());
        }

        ImGui::InputText("Filter##tech_tree_filter", tech_filter, IM_ARRAYSIZE(tech_filter));
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##tech_tree_filter_clear")) tech_filter[0] = '\0';

        ImGui::SameLine();
        ImGui::Checkbox("Graph view##tech_tree_graph_view", &graph_view);

        const float left_w = ImGui::GetContentRegionAvail().x * 0.62f;
        const ImGuiWindowFlags left_flags =
            graph_view ? (ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)
                       : ImGuiWindowFlags_HorizontalScrollbar;
        ImGui::BeginChild("tech_tree_left", ImVec2(left_w, 0), true, left_flags);

        if (graph_view) {
          draw_tech_tree_graph(sim, ui, fac, cached_layout, tech_filter, selected_tech);
        } else {
          // Table layout: one column per tier.
          const int tiers = static_cast<int>(cached_layout.tiers.size());
          int max_rows = 0;
          for (const auto& t : cached_layout.tiers) max_rows = std::max(max_rows, static_cast<int>(t.size()));

          const ImGuiTableFlags tflags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit |
                                         ImGuiTableFlags_RowBg;
          if (ImGui::BeginTable("tech_tree_table", std::max(1, tiers), tflags)) {
            for (int i = 0; i < std::max(1, tiers); ++i) {
              const std::string col = "Tier " + std::to_string(i);
              ImGui::TableSetupColumn(col.c_str(), ImGuiTableColumnFlags_WidthFixed, 240.0f);
            }
            ImGui::TableHeadersRow();

            for (int r = 0; r < max_rows; ++r) {
              ImGui::TableNextRow();
              for (int t = 0; t < tiers; ++t) {
                ImGui::TableSetColumnIndex(t);
                if (r >= static_cast<int>(cached_layout.tiers[static_cast<std::size_t>(t)].size())) {
                  ImGui::TextUnformatted("");
                  continue;
                }
                const std::string& tid =
                    cached_layout.tiers[static_cast<std::size_t>(t)][static_cast<std::size_t>(r)];
                const auto it = sim.content().techs.find(tid);
                if (it == sim.content().techs.end()) continue;

                const TechDef& def = it->second;

                // Filter by id or name.
                const std::string hay = def.name + " " + tid;
                if (!case_insensitive_contains(hay, tech_filter)) {
                  ImGui::TextUnformatted("");
                  continue;
                }

                const bool known = vec_contains(fac.known_techs, tid);
                const bool active = (!fac.active_research_id.empty() && fac.active_research_id == tid);
                const bool queued = vec_contains(fac.research_queue, tid);

                bool prereqs_met = true;
                for (const auto& pre : def.prereqs) {
                  if (!vec_contains(fac.known_techs, pre)) {
                    prereqs_met = false;
                    break;
                  }
                }

                std::string prefix;
                if (known) prefix = u8_cstr(u8"✓ ");
                else if (active) prefix = u8_cstr(u8"▶ ");
                else if (queued) prefix = u8_cstr(u8"⏳ ");
                else if (prereqs_met) prefix = u8_cstr(u8"• ");
                else prefix = "  ";

                const bool sel = (selected_tech == tid);

                if (known) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(120, 255, 140, 255));
                else if (active) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 220, 120, 255));
                else if (queued) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(170, 210, 255, 255));
                else if (prereqs_met) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
                else ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(170, 170, 170, 255));

                const std::string lbl = prefix + def.name + "##technode_" + tid;
                if (ImGui::Selectable(lbl.c_str(), sel)) {
                  selected_tech = tid;
                }

                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                  ImGui::BeginTooltip();
                  ImGui::Text("%s", def.name.c_str());
                  ImGui::TextDisabled("%s", tid.c_str());
                  ImGui::Text("Cost: %.0f", def.cost);
                  if (!def.prereqs.empty()) {
                    ImGui::Separator();
                    ImGui::Text("Prereqs:");
                    for (const auto& pre : def.prereqs) {
                      ImGui::BulletText("%s", pre.c_str());
                    }
                  }
                  ImGui::EndTooltip();
                }

                ImGui::PopStyleColor();
              }
            }

            ImGui::EndTable();
          }
        }

        ImGui::EndChild();
        ImGui::SameLine();

        ImGui::BeginChild("tech_tree_right", ImVec2(0, 0), true);

        if (selected_tech.empty()) {
          ImGui::TextDisabled("Select a tech node to see details.");
        } else if (auto it = sim.content().techs.find(selected_tech); it == sim.content().techs.end()) {
          ImGui::TextDisabled("Tech not found.");
        } else {
          const TechDef& def = sim.content().techs.at(selected_tech);
          const bool known = vec_contains(fac.known_techs, def.id);
          const bool active = (!fac.active_research_id.empty() && fac.active_research_id == def.id);
          const bool queued = vec_contains(fac.research_queue, def.id);

          bool prereqs_met = true;
          for (const auto& pre : def.prereqs) {
            if (!vec_contains(fac.known_techs, pre)) {
              prereqs_met = false;
              break;
            }
          }

          ImGui::Text("%s", def.name.c_str());
          ImGui::TextDisabled("%s", def.id.c_str());
          ImGui::Separator();
          ImGui::Text("Cost: %.0f", def.cost);

          if (known) {
            ImGui::TextColored(ImVec4(0.47f, 1.0f, 0.55f, 1.0f), "Status: Known");
          } else if (active) {
            ImGui::TextColored(ImVec4(1.0f, 0.86f, 0.47f, 1.0f), "Status: Active (%.0f / %.0f)",
                               fac.active_research_progress, def.cost);
          } else if (queued) {
            ImGui::TextColored(ImVec4(0.65f, 0.82f, 1.0f, 1.0f), "Status: Queued");
          } else if (prereqs_met) {
            ImGui::Text("Status: Available");
          } else {
            ImGui::TextDisabled("Status: Locked (missing prereqs)");
          }

          if (!def.prereqs.empty()) {
            ImGui::Separator();
            ImGui::Text("Prerequisites");
            for (const auto& pre : def.prereqs) {
              const bool have = vec_contains(fac.known_techs, pre);
              if (have) {
                ImGui::BulletText("%s  (known)", pre.c_str());
              } else {
                ImGui::BulletText("%s  (missing)", pre.c_str());
              }
            }
          }

          if (!def.effects.empty()) {
            ImGui::Separator();
            ImGui::Text("Effects");
            for (const auto& eff : def.effects) {
              ImGui::BulletText("%s: %s", eff.type.c_str(), eff.value.c_str());
            }
          }

          ImGui::Separator();
          ImGui::Text("Actions");

          if (!known) {
            if (ImGui::Button("Set Active")) {
              fac.active_research_id = def.id;
              fac.active_research_progress = 0.0;
              // Avoid duplicates: remove from queue if present.
              fac.research_queue.erase(std::remove(fac.research_queue.begin(), fac.research_queue.end(), def.id),
                                       fac.research_queue.end());
              ui.request_details_tab = DetailsTab::Research;
            }
            ImGui::SameLine();
            if (!queued) {
              if (ImGui::Button("Queue")) {
                push_unique(fac.research_queue, def.id);
              }
            } else {
              if (ImGui::Button("Unqueue")) {
                fac.research_queue.erase(std::remove(fac.research_queue.begin(), fac.research_queue.end(), def.id),
                                         fac.research_queue.end());
              }
            }

            if (ImGui::Button("Queue prereq plan")) {
              const auto plan_res = compute_research_plan(sim.content(), fac, def.id);
              if (plan_res.ok()) {
                for (const auto& tid : plan_res.plan.tech_ids) {
                  if (tid == fac.active_research_id) continue;
                  if (vec_contains(fac.known_techs, tid)) continue;
                  push_unique(fac.research_queue, tid);
                }
              }
            }

            // Plan preview.
            const auto plan_res = compute_research_plan(sim.content(), fac, def.id);
            if (plan_res.ok() && !plan_res.plan.tech_ids.empty()) {
              ImGui::Separator();
              ImGui::Text("Prereq plan (queue order)");
              ImGui::TextDisabled("Total cost (sum): %.0f", plan_res.plan.total_cost);
              for (const auto& tid : plan_res.plan.tech_ids) {
                const auto it2 = sim.content().techs.find(tid);
                const std::string nm = (it2 == sim.content().techs.end()) ? tid : it2->second.name;
                ImGui::BulletText("%s", nm.c_str());
              }
            } else if (!plan_res.ok()) {
              ImGui::Separator();
              ImGui::TextDisabled("Planner errors:");
              for (const auto& e : plan_res.errors) ImGui::BulletText("%s", e.c_str());
            }
          }

          ImGui::Separator();
          ImGui::Text("Research banked: %.0f RP", fac.research_points);
          if (ImGui::Button("Clear Research Queue")) {
            fac.research_queue.clear();
          }
        }

        ImGui::EndChild();

        ImGui::EndTabItem();
      }
    }

    ImGui::EndTabBar();
  }

  ImGui::End();
}

}  // namespace nebula4x::ui
