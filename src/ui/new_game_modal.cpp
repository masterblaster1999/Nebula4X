#include "ui/new_game_modal.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <queue>
#include <vector>
#include <limits>

#include "nebula4x/core/scenario.h"
#include "nebula4x/util/log.h"

namespace nebula4x::ui {
namespace {

constexpr int kScenarioSol = 0;
constexpr int kScenarioRandom = 1;

std::uint32_t time_seed_u32() {
  using namespace std::chrono;
  const std::uint64_t t = static_cast<std::uint64_t>(
      duration_cast<nanoseconds>(high_resolution_clock::now().time_since_epoch()).count());
  // Mix bits a bit to avoid obvious patterns when only low bits change.
  std::uint64_t x = t;
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return static_cast<std::uint32_t>(x ^ (x >> 32));
}

struct RandomPreviewCache {
  bool valid{false};
  std::uint32_t seed{0};
  int num_systems{0};
  int galaxy_shape{0};
  int placement_style{0};
  int placement_quality{24};
  int jump_style{0};
  int jump_density_pct{100};
  int ai_empires{0};
  bool enable_pirates{true};
  int pirate_strength_pct{100};
  bool enable_regions{true};
  int num_regions{-1};
  GameState state;
  std::string error;
};

void ensure_preview(RandomPreviewCache& cache, const nebula4x::RandomScenarioConfig& cfg_in) {
  nebula4x::RandomScenarioConfig cfg = cfg_in;
  cfg.num_systems = std::clamp(cfg.num_systems, 1, 64);
  cfg.galaxy_shape = static_cast<nebula4x::RandomGalaxyShape>(
      std::clamp(static_cast<int>(cfg.galaxy_shape), 0, 4));
  cfg.placement_style = static_cast<nebula4x::RandomPlacementStyle>(
      std::clamp(static_cast<int>(cfg.placement_style), 0, 1));
  cfg.placement_quality = std::clamp(cfg.placement_quality, 4, 96);
  cfg.num_ai_empires = std::clamp(cfg.num_ai_empires, -1, 12);
  cfg.num_regions = std::clamp(cfg.num_regions, -1, 12);
  cfg.pirate_strength = std::clamp(cfg.pirate_strength, 0.0, 5.0);

  cfg.jump_network_style = static_cast<nebula4x::RandomJumpNetworkStyle>(
      std::clamp(static_cast<int>(cfg.jump_network_style), 0, 5));
  cfg.jump_density = std::clamp(cfg.jump_density, 0.0, 2.0);

  const int shape_i = std::clamp(static_cast<int>(cfg.galaxy_shape), 0, 4);
  const int placement_style_i = std::clamp(static_cast<int>(cfg.placement_style), 0, 1);
  const int placement_quality_i = std::clamp(cfg.placement_quality, 4, 96);
  const int jump_style_i = std::clamp(static_cast<int>(cfg.jump_network_style), 0, 5);
  const int strength_pct = static_cast<int>(std::llround(cfg.pirate_strength * 100.0));
  const int jump_density_pct = static_cast<int>(std::llround(cfg.jump_density * 100.0));

  if (cache.valid && cache.seed == cfg.seed && cache.num_systems == cfg.num_systems && cache.galaxy_shape == shape_i &&
      cache.placement_style == placement_style_i && cache.placement_quality == placement_quality_i &&
      cache.jump_style == jump_style_i && cache.jump_density_pct == jump_density_pct &&
      cache.ai_empires == cfg.num_ai_empires && cache.enable_pirates == cfg.enable_pirates &&
      cache.enable_regions == cfg.enable_regions && cache.num_regions == cfg.num_regions &&
      cache.pirate_strength_pct == strength_pct) {
    return;
  }

  cache.valid = false;
  cache.seed = cfg.seed;
  cache.num_systems = cfg.num_systems;
  cache.galaxy_shape = shape_i;
  cache.placement_style = placement_style_i;
  cache.placement_quality = placement_quality_i;
  cache.jump_style = jump_style_i;
  cache.jump_density_pct = jump_density_pct;
  cache.ai_empires = cfg.num_ai_empires;
  cache.enable_pirates = cfg.enable_pirates;
  cache.pirate_strength_pct = strength_pct;
  cache.enable_regions = cfg.enable_regions;
  cache.num_regions = cfg.num_regions;
  cache.error.clear();

  try {
    cache.state = nebula4x::make_random_scenario(cfg);
    cache.valid = true;
  } catch (const std::exception& e) {
    cache.error = e.what();
  }
}


struct JumpGraphStats {
  int nodes{0};
  int undirected_edges{0};
  double avg_degree{0.0};
  double avg_edge_length{0.0};
  double edge_length_std{0.0};
  int edge_crossings{0};
  int diameter_hops{0};
  int articulation_points{0};
  bool connected{true};
};

struct StarPlacementStats {
  int nodes{0};
  double min_nearest_neighbor{0.0};
  double avg_nearest_neighbor{0.0};
  double nearest_neighbor_std{0.0};
};

StarPlacementStats compute_star_placement_stats(const GameState& s) {
  StarPlacementStats st;
  st.nodes = static_cast<int>(s.systems.size());
  if (st.nodes <= 1) return st;

  std::vector<Vec2> pos;
  pos.reserve(s.systems.size());
  for (const auto& [_, sys] : s.systems) pos.push_back(sys.galaxy_pos);

  double min_nn = std::numeric_limits<double>::infinity();
  double sum = 0.0;
  double sum2 = 0.0;

  for (std::size_t i = 0; i < pos.size(); ++i) {
    double best = std::numeric_limits<double>::infinity();
    for (std::size_t j = 0; j < pos.size(); ++j) {
      if (i == j) continue;
      best = std::min(best, (pos[i] - pos[j]).length());
    }

    if (!std::isfinite(best)) best = 0.0;
    min_nn = std::min(min_nn, best);
    sum += best;
    sum2 += best * best;
  }

  const double m = static_cast<double>(pos.size());
  st.min_nearest_neighbor = std::isfinite(min_nn) ? min_nn : 0.0;
  st.avg_nearest_neighbor = sum / std::max(1e-9, m);
  const double var = std::max(0.0, (sum2 / std::max(1e-9, m)) - st.avg_nearest_neighbor * st.avg_nearest_neighbor);
  st.nearest_neighbor_std = std::sqrt(var);
  return st;
}

JumpGraphStats compute_jump_graph_stats(const GameState& s) {
  JumpGraphStats st;
  st.nodes = static_cast<int>(s.systems.size());
  if (st.nodes <= 1) {
    st.connected = true;
    return st;
  }

  // Build a stable indexing for the unordered system map.
  std::vector<Id> ids;
  ids.reserve(s.systems.size());
  for (const auto& [id, _] : s.systems) ids.push_back(id);

  std::unordered_map<Id, int> idx;
  idx.reserve(ids.size() * 2);
  for (int i = 0; i < static_cast<int>(ids.size()); ++i) idx[ids[static_cast<std::size_t>(i)]] = i;

  std::vector<std::vector<int>> adj(ids.size());

  // Positions in a stable index order.
  std::vector<Vec2> pos;
  pos.reserve(ids.size());
  for (Id id : ids) {
    const auto* sys = nebula4x::find_ptr(s.systems, id);
    pos.push_back(sys ? sys->galaxy_pos : Vec2{0.0, 0.0});
  }

  // Deduplicate bi-directional jump points into undirected graph edges.
  std::unordered_set<std::uint64_t> edges;
  edges.reserve(s.jump_points.size() * 2);

  for (const auto& [_, jp] : s.jump_points) {
    const auto* other = nebula4x::find_ptr(s.jump_points, jp.linked_jump_id);
    if (!other) continue;
    const Id a = jp.system_id;
    const Id b = other->system_id;
    if (a == kInvalidId || b == kInvalidId) continue;

    const auto ita = idx.find(a);
    const auto itb = idx.find(b);
    if (ita == idx.end() || itb == idx.end()) continue;
    const int ia = ita->second;
    const int ib = itb->second;
    if (ia == ib) continue;

    const auto lo = static_cast<std::uint32_t>(std::min(ia, ib));
    const auto hi = static_cast<std::uint32_t>(std::max(ia, ib));
    const std::uint64_t key = (static_cast<std::uint64_t>(lo) << 32) | static_cast<std::uint64_t>(hi);
    if (!edges.insert(key).second) continue;

    adj[static_cast<std::size_t>(ia)].push_back(ib);
    adj[static_cast<std::size_t>(ib)].push_back(ia);
  }

  st.undirected_edges = static_cast<int>(edges.size());

  // Collect edge list for geometry metrics.
  std::vector<std::pair<int, int>> edge_list;
  edge_list.reserve(edges.size());
  for (const std::uint64_t k : edges) {
    const int a = static_cast<int>(k >> 32);
    const int b = static_cast<int>(k & 0xFFFFFFFFu);
    if (a == b) continue;
    if (a < 0 || b < 0 || a >= st.nodes || b >= st.nodes) continue;
    edge_list.emplace_back(a, b);
  }

  // Average edge length (galaxy units).
  if (!edge_list.empty()) {
    double sum = 0.0;
    double sum2 = 0.0;
    for (const auto& e : edge_list) {
      const Vec2 d = pos[static_cast<std::size_t>(e.first)] - pos[static_cast<std::size_t>(e.second)];
      const double len = d.length();
      sum += len;
      sum2 += len * len;
    }
    const double m = static_cast<double>(edge_list.size());
    st.avg_edge_length = sum / m;
    const double var = std::max(0.0, (sum2 / m) - st.avg_edge_length * st.avg_edge_length);
    st.edge_length_std = std::sqrt(var);
  }

  // Edge crossing count (strict segment intersection, ignoring shared endpoints).
  {
    auto orient = [](const Vec2& a, const Vec2& b, const Vec2& c) {
      return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    };
    auto proper_intersect = [&](const Vec2& a, const Vec2& b, const Vec2& c, const Vec2& d) {
      const double o1 = orient(a, b, c);
      const double o2 = orient(a, b, d);
      const double o3 = orient(c, d, a);
      const double o4 = orient(c, d, b);
      const double eps = 1e-12;
      // Ignore degenerate / collinear / touching cases; we only care about
      // true crossings that add visual clutter.
      if (std::fabs(o1) < eps || std::fabs(o2) < eps || std::fabs(o3) < eps || std::fabs(o4) < eps) return false;
      return (o1 * o2 < 0.0) && (o3 * o4 < 0.0);
    };

    int crossings = 0;
    for (std::size_t i = 0; i < edge_list.size(); ++i) {
      const auto [a, b] = edge_list[i];
      const Vec2 pa = pos[static_cast<std::size_t>(a)];
      const Vec2 pb = pos[static_cast<std::size_t>(b)];
      for (std::size_t j = i + 1; j < edge_list.size(); ++j) {
        const auto [c, d] = edge_list[j];
        // Shared endpoint isn't a crossing.
        if (a == c || a == d || b == c || b == d) continue;
        const Vec2 pc = pos[static_cast<std::size_t>(c)];
        const Vec2 pd = pos[static_cast<std::size_t>(d)];
        if (proper_intersect(pa, pb, pc, pd)) ++crossings;
      }
    }
    st.edge_crossings = crossings;
  }

  int sum_deg = 0;
  for (const auto& v : adj) sum_deg += static_cast<int>(v.size());
  st.avg_degree = (st.nodes > 0) ? (static_cast<double>(sum_deg) / static_cast<double>(st.nodes)) : 0.0;

  // Connectivity + diameter (unweighted graph): BFS from each node.
  {
    std::vector<int> dist(static_cast<std::size_t>(st.nodes), -1);
    std::queue<int> q;
    dist[0] = 0;
    q.push(0);

    while (!q.empty()) {
      const int u = q.front();
      q.pop();
      for (const int v : adj[static_cast<std::size_t>(u)]) {
        if (dist[static_cast<std::size_t>(v)] != -1) continue;
        dist[static_cast<std::size_t>(v)] = dist[static_cast<std::size_t>(u)] + 1;
        q.push(v);
      }
    }

    st.connected = true;
    for (int d : dist) {
      if (d == -1) {
        st.connected = false;
        break;
      }
    }

    int diameter = 0;
    if (st.connected) {
      for (int src = 0; src < st.nodes; ++src) {
        std::vector<int> d(static_cast<std::size_t>(st.nodes), -1);
        std::queue<int> qq;
        d[static_cast<std::size_t>(src)] = 0;
        qq.push(src);

        while (!qq.empty()) {
          const int u = qq.front();
          qq.pop();
          for (const int v : adj[static_cast<std::size_t>(u)]) {
            if (d[static_cast<std::size_t>(v)] != -1) continue;
            d[static_cast<std::size_t>(v)] = d[static_cast<std::size_t>(u)] + 1;
            qq.push(v);
          }
        }

        for (int dd : d) diameter = std::max(diameter, dd);
      }
    }
    st.diameter_hops = diameter;
  }

  // Articulation points ("chokepoints") via DFS lowlink (Tarjan).
  {
    std::vector<int> disc(static_cast<std::size_t>(st.nodes), -1);
    std::vector<int> low(static_cast<std::size_t>(st.nodes), -1);
    std::vector<int> parent(static_cast<std::size_t>(st.nodes), -1);
    std::vector<bool> ap(static_cast<std::size_t>(st.nodes), false);

    int t = 0;
    auto dfs = [&](auto&& self, int u) -> void {
      disc[static_cast<std::size_t>(u)] = low[static_cast<std::size_t>(u)] = t++;
      int children = 0;

      for (const int v : adj[static_cast<std::size_t>(u)]) {
        if (disc[static_cast<std::size_t>(v)] == -1) {
          parent[static_cast<std::size_t>(v)] = u;
          ++children;

          self(self, v);

          low[static_cast<std::size_t>(u)] = std::min(low[static_cast<std::size_t>(u)], low[static_cast<std::size_t>(v)]);

          // Root with 2+ children.
          if (parent[static_cast<std::size_t>(u)] == -1 && children > 1) ap[static_cast<std::size_t>(u)] = true;

          // Non-root: if v can't reach above u.
          if (parent[static_cast<std::size_t>(u)] != -1 &&
              low[static_cast<std::size_t>(v)] >= disc[static_cast<std::size_t>(u)]) {
            ap[static_cast<std::size_t>(u)] = true;
          }
        } else if (v != parent[static_cast<std::size_t>(u)]) {
          low[static_cast<std::size_t>(u)] = std::min(low[static_cast<std::size_t>(u)], disc[static_cast<std::size_t>(v)]);
        }
      }
    };

    for (int i = 0; i < st.nodes; ++i) {
      if (disc[static_cast<std::size_t>(i)] == -1) dfs(dfs, i);
    }

    st.articulation_points = 0;
    for (bool b : ap) {
      if (b) ++st.articulation_points;
    }
  }

  return st;
}


struct RegionStats {
  int regions{0};
  int assigned_systems{0};
  int min_systems{0};
  int max_systems{0};
  double avg_systems{0.0};

  // Theme counts, sorted for display.
  std::vector<std::pair<std::string, int>> themes;
};

RegionStats compute_region_stats(const GameState& s) {
  RegionStats out;
  out.regions = static_cast<int>(s.regions.size());

  std::unordered_map<Id, int> counts;
  counts.reserve(s.regions.size() * 2);

  std::unordered_map<std::string, int> theme_counts;
  theme_counts.reserve(s.regions.size() * 2);

  for (const auto& [sid, sys] : s.systems) {
    (void)sid;
    if (sys.region_id == kInvalidId) continue;
    ++out.assigned_systems;
    ++counts[sys.region_id];
  }

  if (!counts.empty()) {
    out.min_systems = std::numeric_limits<int>::max();
    out.max_systems = 0;
    int total = 0;

    for (const auto& [rid, c] : counts) {
      out.min_systems = std::min(out.min_systems, c);
      out.max_systems = std::max(out.max_systems, c);
      total += c;

      if (const auto* reg = nebula4x::find_ptr(s.regions, rid)) {
        if (!reg->theme.empty()) ++theme_counts[reg->theme];
      }
    }

    out.avg_systems = counts.empty() ? 0.0 : (double)total / (double)counts.size();
  } else {
    out.min_systems = 0;
    out.max_systems = 0;
    out.avg_systems = 0.0;
  }

  out.themes.reserve(theme_counts.size());
  for (auto& kv : theme_counts) out.themes.push_back(kv);
  std::sort(out.themes.begin(), out.themes.end(),
            [](const auto& a, const auto& b) { return (a.second != b.second) ? (a.second > b.second) : (a.first < b.first); });
  return out;
}


void draw_galaxy_preview(const GameState& s) {
  ImVec2 avail = ImGui::GetContentRegionAvail();
  const float h = std::clamp(avail.y, 120.0f, 240.0f);
  const ImVec2 size(avail.x, h);

  if (ImGui::BeginChild("##new_game_galaxy_preview", size, true)) {
    const ImVec2 region = ImGui::GetContentRegionAvail();

    ImGui::InvisibleButton("##galaxy_preview_canvas", region);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 rmin = ImGui::GetItemRectMin();
    const ImVec2 rmax = ImGui::GetItemRectMax();

    // Background.
    const ImU32 bg = ImGui::GetColorU32(ImGuiCol_FrameBg);
    dl->AddRectFilled(rmin, rmax, bg);

    if (s.systems.empty()) {
      ImGui::EndChild();
      return;
    }

    // Bounds.
    double minx = 1e30, maxx = -1e30, miny = 1e30, maxy = -1e30;
    for (const auto& [_, sys] : s.systems) {
      minx = std::min(minx, sys.galaxy_pos.x);
      maxx = std::max(maxx, sys.galaxy_pos.x);
      miny = std::min(miny, sys.galaxy_pos.y);
      maxy = std::max(maxy, sys.galaxy_pos.y);
    }
    const double dx = std::max(1e-6, maxx - minx);
    const double dy = std::max(1e-6, maxy - miny);

    const float pad = 10.0f;
    const float w = std::max(1.0f, region.x - pad * 2.0f);
    const float h2 = std::max(1.0f, region.y - pad * 2.0f);

    const double sx = static_cast<double>(w) / dx;
    const double sy = static_cast<double>(h2) / dy;
    const double scale = std::min(sx, sy);

    const float ox = pad + static_cast<float>((static_cast<double>(w) - dx * scale) * 0.5);
    const float oy = pad + static_cast<float>((static_cast<double>(h2) - dy * scale) * 0.5);

    auto to_screen = [&](const Vec2& gp) -> ImVec2 {
      const float x = rmin.x + ox + static_cast<float>((gp.x - minx) * scale);
      // Flip Y so positive galaxy_pos.y is "up".
      const float y = rmin.y + oy + static_cast<float>((maxy - gp.y) * scale);
      return ImVec2(x, y);
    };

    auto region_col = [&](Id rid, float alpha) -> ImU32 {
      if (rid == kInvalidId) return 0;
      const float h = std::fmod(static_cast<float>((static_cast<std::uint32_t>(rid) * 0.61803398875f)), 1.0f);
      const ImVec4 c = ImColor::HSV(h, 0.55f, 0.95f, alpha);
      return ImGui::ColorConvertFloat4ToU32(c);
    };

    // Draw jump connections.
    std::unordered_set<std::uint64_t> drawn;
    drawn.reserve(s.jump_points.size() * 2);

    const ImU32 line_col = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    for (const auto& [_, jp] : s.jump_points) {
      const auto* other = nebula4x::find_ptr(s.jump_points, jp.linked_jump_id);
      if (!other) continue;
      const Id a = jp.system_id;
      const Id b = other->system_id;
      if (a == kInvalidId || b == kInvalidId) continue;
      const auto lo = static_cast<std::uint32_t>(std::min(a, b));
      const auto hi = static_cast<std::uint32_t>(std::max(a, b));
      const std::uint64_t key = (static_cast<std::uint64_t>(lo) << 32) | static_cast<std::uint64_t>(hi);
      if (!drawn.insert(key).second) continue;

      const auto* sys_a = nebula4x::find_ptr(s.systems, a);
      const auto* sys_b = nebula4x::find_ptr(s.systems, b);
      if (!sys_a || !sys_b) continue;

      dl->AddLine(to_screen(sys_a->galaxy_pos), to_screen(sys_b->galaxy_pos), line_col, 1.0f);
    }

    // Draw systems.
    const ImU32 star_col = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 home_col = ImGui::GetColorU32(ImGuiCol_PlotHistogram);

    for (const auto& [id, sys] : s.systems) {
      const ImVec2 p = to_screen(sys.galaxy_pos);
      const float r = (id == s.selected_system) ? 6.0f : 4.0f;
      const ImU32 col = (id == s.selected_system) ? home_col : star_col;
      if (sys.region_id != kInvalidId) {
        dl->AddCircleFilled(p, r + 6.5f, region_col(sys.region_id, 0.10f));
      }
      const float neb = (float)std::clamp(sys.nebula_density, 0.0, 1.0);
      if (neb > 0.01f) {
        const float nr = r + 10.0f + neb * 14.0f;
        const ImU32 ncol = ImGui::GetColorU32(ImGuiCol_PlotHistogramHovered, 0.06f + 0.22f * neb);
        dl->AddCircleFilled(p, nr, ncol);
      }
      dl->AddCircleFilled(p, r, col);
    }

    // Hover tooltip.
    if (ImGui::IsItemHovered()) {
      const ImVec2 m = ImGui::GetIO().MousePos;
      const float hit_r2 = 8.0f * 8.0f;

      Id best_id = kInvalidId;
      float best_d2 = hit_r2;

      for (const auto& [id, sys] : s.systems) {
        const ImVec2 p = to_screen(sys.galaxy_pos);
        const float dx2 = m.x - p.x;
        const float dy2 = m.y - p.y;
        const float d2 = dx2 * dx2 + dy2 * dy2;
        if (d2 <= best_d2) {
          best_d2 = d2;
          best_id = id;
        }
      }

      if (best_id != kInvalidId) {
        const auto* sys = nebula4x::find_ptr(s.systems, best_id);
        if (sys) {
          ImGui::BeginTooltip();
          ImGui::Text("%s", sys->name.c_str());
          const double neb = std::clamp(sys->nebula_density, 0.0, 1.0);
          if (neb > 0.01) {
            ImGui::TextDisabled("Nebula: %.0f%%", neb * 100.0);
          } else {
            ImGui::TextDisabled("Nebula: none");
          }
          if (sys->region_id != kInvalidId) {
            if (const auto* reg = find_ptr(s.regions, sys->region_id)) {
              ImGui::TextDisabled("Region: %s", reg->name.c_str());
              if (!reg->theme.empty()) ImGui::TextDisabled("Theme: %s", reg->theme.c_str());
            }
          }

          ImGui::Separator();
          ImGui::TextDisabled("Systems: %d", static_cast<int>(s.systems.size()));
          ImGui::TextDisabled("Jump points: %d", static_cast<int>(s.jump_points.size()));
          ImGui::EndTooltip();
        }
      }
    }
  }

  ImGui::EndChild();
}

} // namespace

void draw_new_game_modal(Simulation& sim, UIState& ui) {
  if (!ui.show_new_game_modal) return;

  // Keep the popup open while the flag is set.
  ImGui::OpenPopup("New Game");

  bool open = true;
  if (ImGui::BeginPopupModal("New Game", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
    ui.new_game_scenario = std::clamp(ui.new_game_scenario, kScenarioSol, kScenarioRandom);

    ImGui::Text("Choose scenario");

    if (ImGui::RadioButton("Sol (classic)", ui.new_game_scenario == kScenarioSol)) {
      ui.new_game_scenario = kScenarioSol;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Random galaxy (procedural)", ui.new_game_scenario == kScenarioRandom)) {
      ui.new_game_scenario = kScenarioRandom;
    }

    ImGui::Separator();

    static RandomPreviewCache preview;

    if (ui.new_game_scenario == kScenarioSol) {
      ImGui::TextWrapped(
          "A compact starter scenario in the Sol system. Good for learning the UI and testing early ship designs.");

    } else {
      // --- Random scenario settings ---
      ui.new_game_random_num_systems = std::clamp(ui.new_game_random_num_systems, 1, 64);

      ImGui::Text("Random galaxy settings");

      // Seed.
      {
        std::uint32_t seed = ui.new_game_random_seed;
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputScalar("Seed", ImGuiDataType_U32, &seed);
        ui.new_game_random_seed = seed;

        ImGui::SameLine();
        if (ImGui::Button("Randomize")) {
          ui.new_game_random_seed = time_seed_u32();
          preview.valid = false;
        }
      }

      // System count.
      {
        int n = ui.new_game_random_num_systems;
        ImGui::SetNextItemWidth(180.0f);
        ImGui::SliderInt("Systems", &n, 1, 64);
        ui.new_game_random_num_systems = std::clamp(n, 1, 64);
      }

      // Galaxy archetype.
      {
        int shape = std::clamp(ui.new_game_random_galaxy_shape, 0, 5);
        ImGui::SetNextItemWidth(180.0f);
        ImGui::Combo("Galaxy shape", &shape,
                     "Spiral disc\0Uniform disc\0Ring\0Clusters\0Filaments\0Barred spiral\0");
        ui.new_game_random_galaxy_shape = std::clamp(shape, 0, 5);
      }

      // System placement style.
      {
        int ps = std::clamp(ui.new_game_random_placement_style, 0, 1);
        ImGui::SetNextItemWidth(180.0f);
        ImGui::Combo("Placement", &ps, "Classic\0Blue noise\0");
        ui.new_game_random_placement_style = std::clamp(ps, 0, 1);

        if (ui.new_game_random_placement_style == 1) {
          int q = std::clamp(ui.new_game_random_placement_quality, 4, 96);
          ImGui::SetNextItemWidth(180.0f);
          ImGui::SliderInt("Placement quality", &q, 8, 64);
          ui.new_game_random_placement_quality = std::clamp(q, 4, 96);
          ImGui::SameLine();
          ImGui::TextDisabled("candidates");
        }
      }


      // Jump network archetype.
      {
        int style = std::clamp(ui.new_game_random_jump_network_style, 0, 6);
        ImGui::SetNextItemWidth(180.0f);
        ImGui::Combo("Jump network", &style,
                     "Balanced\0Dense web\0Sparse lanes\0Cluster bridges\0Hub & spoke\0Planar proximity\0Subspace rivers\0");
        ui.new_game_random_jump_network_style = std::clamp(style, 0, 6);
      }

      // Jump density (scales how many additional links get added for the chosen archetype).
      {
        float dens = ui.new_game_random_jump_density;
        ImGui::SetNextItemWidth(180.0f);
        ImGui::SliderFloat("Jump density", &dens, 0.0f, 2.0f, "%.2fx");
        ui.new_game_random_jump_density = std::clamp(dens, 0.0f, 2.0f);
      }

      // Additional AI empires (besides the player and pirates).
      {
        int ai = ui.new_game_random_ai_empires;
        ImGui::SetNextItemWidth(180.0f);
        ImGui::SliderInt("AI empires", &ai, -1, 8);
        ui.new_game_random_ai_empires = std::clamp(ai, -1, 8);
        if (ui.new_game_random_ai_empires == -1) {
          ImGui::SameLine();
          ImGui::TextDisabled("Auto");
        }
      }

      // Pirates.
      {
        ImGui::Checkbox("Enable pirates", &ui.new_game_random_enable_pirates);
        if (ui.new_game_random_enable_pirates) {
          float strength = ui.new_game_random_pirate_strength;
          ImGui::SetNextItemWidth(180.0f);
          ImGui::SliderFloat("Pirate strength", &strength, 0.5f, 3.0f, "%.2fx");
          ui.new_game_random_pirate_strength = std::clamp(strength, 0.0f, 5.0f);
        }
      }

      // Galaxy regions / sectors.
      {
        ImGui::Checkbox("Enable regions (sectors)", &ui.new_game_random_enable_regions);
        if (ui.new_game_random_enable_regions) {
          int regions = ui.new_game_random_num_regions;
          ImGui::SetNextItemWidth(180.0f);
          ImGui::SliderInt("Regions", &regions, -1, 12);
          ui.new_game_random_num_regions = std::clamp(regions, -1, 12);
          if (ui.new_game_random_num_regions == -1) {
            ImGui::SameLine();
            ImGui::TextDisabled("Auto");
          }
        }
      }

      // Build the generator config.
      nebula4x::RandomScenarioConfig cfg;
      cfg.seed = ui.new_game_random_seed;
      cfg.num_systems = ui.new_game_random_num_systems;
      cfg.galaxy_shape = static_cast<nebula4x::RandomGalaxyShape>(std::clamp(ui.new_game_random_galaxy_shape, 0, 4));
      cfg.placement_style = static_cast<nebula4x::RandomPlacementStyle>(
          std::clamp(ui.new_game_random_placement_style, 0, 1));
      cfg.placement_quality = std::clamp(ui.new_game_random_placement_quality, 4, 96);
      cfg.jump_network_style = static_cast<nebula4x::RandomJumpNetworkStyle>(
          std::clamp(ui.new_game_random_jump_network_style, 0, 5));
      cfg.jump_density = static_cast<double>(ui.new_game_random_jump_density);
      cfg.enable_regions = ui.new_game_random_enable_regions;
      cfg.num_regions = ui.new_game_random_num_regions;
      cfg.num_ai_empires = ui.new_game_random_ai_empires;
      cfg.enable_pirates = ui.new_game_random_enable_pirates;
      cfg.pirate_strength = static_cast<double>(ui.new_game_random_pirate_strength);

      ImGui::SameLine();
      const bool manual = ImGui::Button("Generate preview");

      // Auto-preview when the user isn't actively editing inputs.
      const int strength_pct = static_cast<int>(std::llround(cfg.pirate_strength * 100.0));
      const int placement_style_i = std::clamp(ui.new_game_random_placement_style, 0, 1);
      const int placement_quality_i = std::clamp(ui.new_game_random_placement_quality, 4, 96);
      const int jump_style_i = std::clamp(ui.new_game_random_jump_network_style, 0, 5);
      const int jump_density_pct = static_cast<int>(std::llround(cfg.jump_density * 100.0));
      const bool config_changed = (!preview.valid) || preview.seed != cfg.seed || preview.num_systems != cfg.num_systems ||
                                  preview.galaxy_shape != static_cast<int>(cfg.galaxy_shape) ||
                                  preview.placement_style != placement_style_i ||
                                  preview.placement_quality != placement_quality_i ||
                                  preview.jump_style != jump_style_i || preview.jump_density_pct != jump_density_pct ||
                                  preview.ai_empires != cfg.num_ai_empires ||
                                  preview.enable_regions != cfg.enable_regions || preview.num_regions != cfg.num_regions ||
                                  preview.enable_pirates != cfg.enable_pirates ||
                                  preview.pirate_strength_pct != strength_pct;

      const bool auto_trigger = (!preview.valid) && !ImGui::IsAnyItemActive();
      if (manual || auto_trigger || config_changed) {
        // Debounce: only regenerate when inputs aren't active, unless explicitly requested.
        if (manual || !ImGui::IsAnyItemActive()) {
          ensure_preview(preview, cfg);
        }
      }

      if (!preview.error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "Preview failed: %s", preview.error.c_str());
      }

      if (preview.valid) {
        const GameState& s = preview.state;

        ImGui::Separator();
        ImGui::Text("Preview");
        ImGui::TextDisabled("Systems: %d", static_cast<int>(s.systems.size()));
        ImGui::SameLine();
        ImGui::TextDisabled("Bodies: %d", static_cast<int>(s.bodies.size()));
        ImGui::SameLine();
        ImGui::TextDisabled("Jump points: %d", static_cast<int>(s.jump_points.size()));
        ImGui::TextDisabled("Colonies: %d", static_cast<int>(s.colonies.size()));
        ImGui::SameLine();
        ImGui::TextDisabled("Ships: %d", static_cast<int>(s.ships.size()));
        ImGui::SameLine();
        ImGui::TextDisabled("Factions: %d", static_cast<int>(s.factions.size()));

        static const char* kJumpNames[] = {"Balanced", "Dense web", "Sparse lanes", "Cluster bridges", "Hub & spoke",
                                           "Planar proximity"};
        static const char* kPlaceNames[] = {"Classic", "Blue noise"};
        const StarPlacementStats ps = compute_star_placement_stats(s);
        const JumpGraphStats gs = compute_jump_graph_stats(s);

        ImGui::TextDisabled("Placement: %s", kPlaceNames[std::clamp(placement_style_i, 0, 1)]);
        if (placement_style_i == 1) {
          ImGui::SameLine();
          ImGui::TextDisabled("Q: %d", placement_quality_i);
        }
        if (ps.nodes > 1) {
          ImGui::TextDisabled("Nearest neighbor: min %.2f u  avg %.2f u  \xcf\x83 %.2f u",
                              ps.min_nearest_neighbor, ps.avg_nearest_neighbor, ps.nearest_neighbor_std);
        }

        ImGui::TextDisabled("Network: %s  Density: %.2fx", kJumpNames[std::clamp(jump_style_i, 0, 5)], cfg.jump_density);
        ImGui::TextDisabled("Edges: %d", gs.undirected_edges);
        ImGui::SameLine();
        ImGui::TextDisabled("Avg deg: %.2f", gs.avg_degree);
        ImGui::SameLine();
        ImGui::TextDisabled("Avg len: %.2f u", gs.avg_edge_length);
        ImGui::SameLine();
        ImGui::TextDisabled("Crossings: %d", gs.edge_crossings);
        ImGui::TextDisabled("Diameter: %d", gs.diameter_hops);
        ImGui::SameLine();
        ImGui::TextDisabled("Chokepoints: %d", gs.articulation_points);

        const RegionStats rs = compute_region_stats(s);
        if (rs.regions > 0) {
          ImGui::TextDisabled("Regions: %d  size %d-%d (avg %.1f)", rs.regions, rs.min_systems, rs.max_systems, rs.avg_systems);
          if (!rs.themes.empty()) {
            std::string themes;
            const std::size_t n = std::min<std::size_t>(3, rs.themes.size());
            for (std::size_t i = 0; i < n; ++i) {
              if (i) themes += ", ";
              themes += rs.themes[i].first + " x" + std::to_string(rs.themes[i].second);
            }
            if (rs.themes.size() > n) themes += ", ...";
            ImGui::TextDisabled("Themes: %s", themes.c_str());
          }
        } else {
          ImGui::TextDisabled("Regions: disabled");
        }

        if (!gs.connected) {
          ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "WARNING: jump network is disconnected");
        }

        draw_galaxy_preview(s);
      }
    }

    ImGui::Separator();

    // Buttons.
    const float bw = 140.0f;
    if (ImGui::Button("Start", ImVec2(bw, 0.0f))) {
      if (ui.new_game_scenario == kScenarioSol) {
        sim.new_game();
        ui.request_map_tab = MapTab::System;
        nebula4x::log::info("New game: Sol scenario");
      } else {
        nebula4x::RandomScenarioConfig cfg;
        cfg.seed = ui.new_game_random_seed;
        cfg.num_systems = ui.new_game_random_num_systems;
        cfg.galaxy_shape = static_cast<nebula4x::RandomGalaxyShape>(std::clamp(ui.new_game_random_galaxy_shape, 0, 4));
        cfg.placement_style = static_cast<nebula4x::RandomPlacementStyle>(
            std::clamp(ui.new_game_random_placement_style, 0, 1));
        cfg.placement_quality = std::clamp(ui.new_game_random_placement_quality, 4, 96);
        cfg.jump_network_style = static_cast<nebula4x::RandomJumpNetworkStyle>(
            std::clamp(ui.new_game_random_jump_network_style, 0, 5));
        cfg.jump_density = static_cast<double>(ui.new_game_random_jump_density);
        cfg.enable_regions = ui.new_game_random_enable_regions;
        cfg.num_regions = ui.new_game_random_num_regions;
        cfg.num_ai_empires = ui.new_game_random_ai_empires;
        cfg.enable_pirates = ui.new_game_random_enable_pirates;
        cfg.pirate_strength = static_cast<double>(ui.new_game_random_pirate_strength);

        sim.load_game(nebula4x::make_random_scenario(cfg));
        ui.request_map_tab = MapTab::Galaxy;
        nebula4x::log::info("New game: random galaxy (seed=" + std::to_string(ui.new_game_random_seed) +
                           ", systems=" + std::to_string(ui.new_game_random_num_systems) +
                           ", ai=" + std::to_string(ui.new_game_random_ai_empires) +
                           ", jump=" + std::to_string(ui.new_game_random_jump_network_style) +
                           ", density=" + std::to_string(ui.new_game_random_jump_density) +
                           ", pirates=" + std::string(ui.new_game_random_enable_pirates ? "on" : "off") + ")");
      }

      ui.show_new_game_modal = false;
      ImGui::CloseCurrentPopup();
    }

    ImGui::SameLine();

    if (ImGui::Button("Cancel", ImVec2(bw, 0.0f))) {
      ui.show_new_game_modal = false;
      ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
  }

  if (!open) {
    ui.show_new_game_modal = false;
  }
}

} // namespace nebula4x::ui
