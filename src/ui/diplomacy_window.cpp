#include "ui/diplomacy_window.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nebula4x/core/date.h"
#include "ui/map_render.h"

namespace nebula4x::ui {
namespace {

constexpr float kPi = 3.14159265358979323846f;

std::uint32_t hash_u32(std::uint32_t x) {
  // Simple, fast hash (xorshift + mix) for deterministic UI colors.
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

ImU32 color_faction(Id id, float alpha = 1.0f) {
  const std::uint32_t h = hash_u32(static_cast<std::uint32_t>(id));
  const float hf = static_cast<float>(h % 360) / 360.0f;
  float r = 1.0f, g = 1.0f, b = 1.0f;
  ImGui::ColorConvertHSVtoRGB(hf, 0.58f, 0.95f, r, g, b);
  return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, std::clamp(alpha, 0.0f, 1.0f)));
}

ImVec4 status_color_rgba(DiplomacyStatus st, float alpha = 1.0f) {
  alpha = std::clamp(alpha, 0.0f, 1.0f);
  switch (st) {
    case DiplomacyStatus::Friendly:
      return ImVec4(0.22f, 0.84f, 0.38f, alpha);
    case DiplomacyStatus::Neutral:
      return ImVec4(0.72f, 0.76f, 0.80f, alpha);
    case DiplomacyStatus::Hostile:
      return ImVec4(0.92f, 0.24f, 0.20f, alpha);
  }
  return ImVec4(1, 1, 1, alpha);
}

ImU32 status_color_u32(DiplomacyStatus st, float alpha = 1.0f) {
  return ImGui::ColorConvertFloat4ToU32(status_color_rgba(st, alpha));
}

const char* status_label(DiplomacyStatus st) {
  switch (st) {
    case DiplomacyStatus::Friendly:
      return "Friendly";
    case DiplomacyStatus::Neutral:
      return "Neutral";
    case DiplomacyStatus::Hostile:
      return "Hostile";
  }
  return "?";
}

const char* status_short(DiplomacyStatus st) {
  switch (st) {
    case DiplomacyStatus::Friendly:
      return "F";
    case DiplomacyStatus::Neutral:
      return "N";
    case DiplomacyStatus::Hostile:
      return "H";
  }
  return "?";
}

const char* treaty_type_label(TreatyType t) {
  switch (t) {
    case TreatyType::Ceasefire:
      return "Ceasefire";
    case TreatyType::NonAggressionPact:
      return "Non-Aggression Pact";
    case TreatyType::Alliance:
      return "Alliance";
    case TreatyType::TradeAgreement:
      return "Trade Agreement";
  }
  return "Treaty";
}

DiplomacyStatus cycle_status(DiplomacyStatus st) {
  // Cycle in a practical order for UI: Hostile -> Neutral -> Friendly -> Hostile.
  switch (st) {
    case DiplomacyStatus::Hostile:
      return DiplomacyStatus::Neutral;
    case DiplomacyStatus::Neutral:
      return DiplomacyStatus::Friendly;
    case DiplomacyStatus::Friendly:
      return DiplomacyStatus::Hostile;
  }
  return DiplomacyStatus::Hostile;
}

float clampf(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }

float dist_point_to_segment(ImVec2 p, ImVec2 a, ImVec2 b) {
  const ImVec2 ab = ImVec2(b.x - a.x, b.y - a.y);
  const ImVec2 ap = ImVec2(p.x - a.x, p.y - a.y);
  const float ab2 = ab.x * ab.x + ab.y * ab.y;
  if (ab2 <= 1e-6f) {
    const float dx = p.x - a.x;
    const float dy = p.y - a.y;
    return std::sqrt(dx * dx + dy * dy);
  }
  float t = (ap.x * ab.x + ap.y * ab.y) / ab2;
  t = clampf(t, 0.0f, 1.0f);
  const ImVec2 c = ImVec2(a.x + ab.x * t, a.y + ab.y * t);
  const float dx = p.x - c.x;
  const float dy = p.y - c.y;
  return std::sqrt(dx * dx + dy * dy);
}

ImVec2 bezier_point(ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3, float t) {
  const float u = 1.0f - t;
  const float tt = t * t;
  const float uu = u * u;
  const float uuu = uu * u;
  const float ttt = tt * t;

  ImVec2 p;
  p.x = uuu * p0.x;
  p.x += 3 * uu * t * p1.x;
  p.x += 3 * u * tt * p2.x;
  p.x += ttt * p3.x;

  p.y = uuu * p0.y;
  p.y += 3 * uu * t * p1.y;
  p.y += 3 * u * tt * p2.y;
  p.y += ttt * p3.y;
  return p;
}

ImVec2 bezier_tangent(ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3, float t) {
  // Derivative of cubic Bezier.
  const float u = 1.0f - t;
  ImVec2 d;
  d.x = 3 * u * u * (p1.x - p0.x) + 6 * u * t * (p2.x - p1.x) + 3 * t * t * (p3.x - p2.x);
  d.y = 3 * u * u * (p1.y - p0.y) + 6 * u * t * (p2.y - p1.y) + 3 * t * t * (p3.y - p2.y);
  return d;
}

ImVec2 normalize(ImVec2 v) {
  const float len2 = v.x * v.x + v.y * v.y;
  if (len2 <= 1e-6f) return ImVec2(0, 0);
  const float inv = 1.0f / std::sqrt(len2);
  return ImVec2(v.x * inv, v.y * inv);
}

void add_arrowhead(ImDrawList* dl, ImVec2 tip, ImVec2 dir, float size, ImU32 col) {
  dir = normalize(dir);
  const ImVec2 perp(-dir.y, dir.x);
  const ImVec2 a = ImVec2(tip.x - dir.x * size + perp.x * (size * 0.55f), tip.y - dir.y * size + perp.y * (size * 0.55f));
  const ImVec2 b = ImVec2(tip.x - dir.x * size - perp.x * (size * 0.55f), tip.y - dir.y * size - perp.y * (size * 0.55f));
  dl->AddTriangleFilled(tip, a, b, col);
}

struct NodeSim {
  ImVec2 pos{0, 0};
  ImVec2 vel{0, 0};
  bool pinned{false};
};

struct EdgeSel {
  Id from{kInvalidId};
  Id to{kInvalidId};
  bool valid() const { return from != kInvalidId && to != kInvalidId && from != to; }
  void clear() {
    from = kInvalidId;
    to = kInvalidId;
  }
};

struct GraphState {
  bool initialized{false};
  std::uint64_t content_hash{0};

  std::unordered_map<Id, NodeSim> nodes;

  Id perspective{kInvalidId};
  Id selected_node{kInvalidId};
  Id hovered_node{kInvalidId};

  EdgeSel selected_edge;
  EdgeSel hovered_edge;
  EdgeSel context_edge;
  Id context_node{kInvalidId};

  Id dragging_node{kInvalidId};

  bool panning{false};
  ImVec2 pan{0, 0};
  float zoom{1.0f};

  bool show_all_edges{true};
  bool show_matrix{true};
  bool show_recent_events{true};
  bool reciprocal_edits{true};

  // New treaty UI state (per-window).
  int new_treaty_type_index{0};
  int new_treaty_duration_days{30};
  bool new_treaty_indefinite{false};
  std::string new_treaty_error{};
  std::string last_treaty_error;

  // Offer UI state.
  int new_offer_expires_days{30};
  std::string offer_error{};

  void ensure_defaults(const Simulation& sim) {
    const auto& s = sim.state();
    if (s.factions.empty()) {
      perspective = kInvalidId;
      selected_node = kInvalidId;
      return;
    }
    if (perspective == kInvalidId || s.factions.find(perspective) == s.factions.end()) {
      perspective = s.factions.begin()->first;
    }
    if (selected_node == kInvalidId || s.factions.find(selected_node) == s.factions.end()) {
      selected_node = perspective;
    }
  }
};

static GraphState g;

std::uint64_t compute_factions_hash(const std::vector<const Faction*>& facs) {
  // FNV-1a-ish on ids and name lengths for stability.
  std::uint64_t h = 1469598103934665603ULL;
  for (const auto* f : facs) {
    const std::uint64_t v = static_cast<std::uint64_t>(f->id) ^ (static_cast<std::uint64_t>(f->name.size()) << 32);
    h ^= v;
    h *= 1099511628211ULL;
  }
  return h;
}

void prune_and_seed_nodes(const std::vector<const Faction*>& facs) {
  // Remove nodes that no longer exist.
  {
    std::vector<Id> to_remove;
    to_remove.reserve(g.nodes.size());
    for (const auto& kv : g.nodes) {
      bool found = false;
      for (const auto* f : facs) {
        if (f->id == kv.first) {
          found = true;
          break;
        }
      }
      if (!found) to_remove.push_back(kv.first);
    }
    for (Id id : to_remove) g.nodes.erase(id);
  }

  // Seed new nodes on a circle.
  const float radius = 420.0f;
  const int n = (int)facs.size();
  for (int i = 0; i < n; ++i) {
    const Id id = facs[(std::size_t)i]->id;
    if (g.nodes.find(id) != g.nodes.end()) continue;
    const float a = (n > 0) ? (2.0f * kPi * (float)i / (float)n) : 0.0f;
    NodeSim ns;
    ns.pos = ImVec2(std::cos(a) * radius, std::sin(a) * radius);
    ns.vel = ImVec2(0, 0);
    ns.pinned = false;
    g.nodes.emplace(id, ns);
  }
}

float status_affinity(DiplomacyStatus a, DiplomacyStatus b) {
  // Higher value = stronger attraction in force layout.
  const int friendly = (a == DiplomacyStatus::Friendly) + (b == DiplomacyStatus::Friendly);
  const int neutral = (a == DiplomacyStatus::Neutral) + (b == DiplomacyStatus::Neutral);
  if (friendly == 2) return 1.0f;
  if (friendly == 1) return 0.65f;
  if (neutral == 2) return 0.35f;
  if (neutral == 1) return 0.18f;
  return 0.0f;
}

void step_force_layout(const Simulation& sim, const std::vector<Id>& ids, float dt) {
  // Light-weight force-directed layout with relationship-weighted springs.
  const float repulsion = 180000.0f;
  const float spring_k = 0.035f;
  const float damping = 0.90f;
  const float center_k = 0.020f;

  const int n = (int)ids.size();
  for (int i = 0; i < n; ++i) {
    NodeSim& ni = g.nodes[ids[(std::size_t)i]];
    if (ni.pinned) continue;
    // Gentle pull toward origin to prevent drift.
    ni.vel.x += (-ni.pos.x) * center_k * dt;
    ni.vel.y += (-ni.pos.y) * center_k * dt;
  }

  for (int i = 0; i < n; ++i) {
    for (int j = i + 1; j < n; ++j) {
      NodeSim& ni = g.nodes[ids[(std::size_t)i]];
      NodeSim& nj = g.nodes[ids[(std::size_t)j]];

      ImVec2 d(nj.pos.x - ni.pos.x, nj.pos.y - ni.pos.y);
      float dist2 = d.x * d.x + d.y * d.y;
      dist2 = std::max(dist2, 25.0f);
      const float dist = std::sqrt(dist2);
      const ImVec2 dir = ImVec2(d.x / dist, d.y / dist);

      // Repulsion.
      const float f_rep = repulsion / dist2;
      if (!ni.pinned) {
        ni.vel.x -= dir.x * f_rep * dt;
        ni.vel.y -= dir.y * f_rep * dt;
      }
      if (!nj.pinned) {
        nj.vel.x += dir.x * f_rep * dt;
        nj.vel.y += dir.y * f_rep * dt;
      }

      // Relationship-weighted attraction.
      const Id a = ids[(std::size_t)i];
      const Id b = ids[(std::size_t)j];
      const float aff = status_affinity(sim.diplomatic_status(a, b), sim.diplomatic_status(b, a));
      if (aff > 0.0f) {
        const float rest = 520.0f - 260.0f * aff; // friendly pulls closer.
        const float f_spring = spring_k * aff * (dist - rest);
        if (!ni.pinned) {
          ni.vel.x += dir.x * f_spring * dt;
          ni.vel.y += dir.y * f_spring * dt;
        }
        if (!nj.pinned) {
          nj.vel.x -= dir.x * f_spring * dt;
          nj.vel.y -= dir.y * f_spring * dt;
        }
      }
    }
  }

  for (int i = 0; i < n; ++i) {
    NodeSim& ni = g.nodes[ids[(std::size_t)i]];
    if (ni.pinned) continue;
    ni.pos.x += ni.vel.x;
    ni.pos.y += ni.vel.y;
    ni.vel.x *= damping;
    ni.vel.y *= damping;
  }
}

void apply_radial_targets(const Simulation& sim, const std::vector<const Faction*>& facs, float dt) {
  if (facs.empty()) return;
  const Id focus = g.perspective;

  std::vector<Id> friendly, neutral, hostile;
  friendly.reserve(facs.size());
  neutral.reserve(facs.size());
  hostile.reserve(facs.size());

  for (const auto* f : facs) {
    if (f->id == focus) continue;
    const DiplomacyStatus st = sim.diplomatic_status(focus, f->id);
    if (st == DiplomacyStatus::Friendly)
      friendly.push_back(f->id);
    else if (st == DiplomacyStatus::Neutral)
      neutral.push_back(f->id);
    else
      hostile.push_back(f->id);
  }

  auto place_ring = [&](const std::vector<Id>& ids, float radius, float start_angle) {
    const int n = (int)ids.size();
    for (int i = 0; i < n; ++i) {
      const float a = start_angle + (n > 0 ? (2.0f * kPi * (float)i / (float)n) : 0.0f);
      const ImVec2 target(std::cos(a) * radius, std::sin(a) * radius);
      NodeSim& ns = g.nodes[ids[(std::size_t)i]];
      if (ns.pinned) continue;
      const float k = 1.0f - std::pow(0.001f, dt * 60.0f); // frame-rate independent-ish.
      ns.pos.x += (target.x - ns.pos.x) * k;
      ns.pos.y += (target.y - ns.pos.y) * k;
      ns.vel = ImVec2(0, 0);
    }
  };

  place_ring(friendly, 240.0f, -kPi / 2.0f);
  place_ring(neutral, 380.0f, -kPi / 2.0f);
  place_ring(hostile, 560.0f, -kPi / 2.0f);

  // Keep the focused faction centered.
  if (auto it = g.nodes.find(focus); it != g.nodes.end()) {
    NodeSim& c = it->second;
    if (!c.pinned) {
      const float k = 1.0f - std::pow(0.001f, dt * 60.0f);
      c.pos.x += (0.0f - c.pos.x) * k;
      c.pos.y += (0.0f - c.pos.y) * k;
      c.vel = ImVec2(0, 0);
    }
  }
}

void apply_circle_targets(const std::vector<const Faction*>& facs, float dt) {
  const int n = (int)facs.size();
  if (n <= 0) return;
  const float radius = 520.0f;
  for (int i = 0; i < n; ++i) {
    const float a = -kPi / 2.0f + (2.0f * kPi * (float)i / (float)n);
    const ImVec2 target(std::cos(a) * radius, std::sin(a) * radius);
    NodeSim& ns = g.nodes[facs[(std::size_t)i]->id];
    if (ns.pinned) continue;
    const float k = 1.0f - std::pow(0.001f, dt * 60.0f);
    ns.pos.x += (target.x - ns.pos.x) * k;
    ns.pos.y += (target.y - ns.pos.y) * k;
    ns.vel = ImVec2(0, 0);
  }
}

struct CanvasXform {
  ImVec2 origin;
  ImVec2 size;
  ImVec2 center;

  ImVec2 world_to_screen(ImVec2 w) const {
    return ImVec2(center.x + (w.x + g.pan.x) * g.zoom, center.y + (w.y + g.pan.y) * g.zoom);
  }
  ImVec2 screen_to_world(ImVec2 s) const {
    return ImVec2((s.x - center.x) / g.zoom - g.pan.x, (s.y - center.y) / g.zoom - g.pan.y);
  }
};

} // namespace

void draw_diplomacy_window(Simulation& sim, UIState& ui, Id& /*selected_ship*/, Id& /*selected_colony*/,
                           Id& /*selected_body*/) {
  if (!ui.show_diplomacy_window) return;

  ImGui::SetNextWindowSize(ImVec2(1080, 720), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Diplomacy Graph", &ui.show_diplomacy_window)) {
    ImGui::End();
    return;
  }

  auto& s = sim.state();
  if (s.factions.empty()) {
    ImGui::TextDisabled("No factions.");
    ImGui::End();
    return;
  }

  // Sorted faction list (stable UI).
  std::vector<const Faction*> facs;
  facs.reserve(s.factions.size());
  for (const auto& kv : s.factions) facs.push_back(&kv.second);
  std::sort(facs.begin(), facs.end(), [](const Faction* a, const Faction* b) { return a->name < b->name; });

  // One-time / change-driven initialization.
  const std::uint64_t h = compute_factions_hash(facs);
  if (!g.initialized || g.content_hash != h) {
    g.content_hash = h;
    prune_and_seed_nodes(facs);
    g.initialized = true;
  }

  // Default perspective.
  if (g.perspective == kInvalidId) {
    // Prefer the current UI viewer faction when available.
    if (ui.viewer_faction_id != kInvalidId && s.factions.find(ui.viewer_faction_id) != s.factions.end()) {
      g.perspective = ui.viewer_faction_id;
    } else {
      g.perspective = facs.front()->id;
    }
  }

  g.ensure_defaults(sim);

  // --- Top controls ---
  {
    // Perspective selector.
    int persp_idx = 0;
    for (int i = 0; i < (int)facs.size(); ++i) {
      if (facs[(std::size_t)i]->id == g.perspective) {
        persp_idx = i;
        break;
      }
    }
    std::vector<const char*> names;
    names.reserve(facs.size());
    for (const auto* f : facs) names.push_back(f->name.c_str());

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Perspective");
    ImGui::SameLine();
    if (ImGui::Combo("##perspective", &persp_idx, names.data(), (int)names.size())) {
      g.perspective = facs[(std::size_t)persp_idx]->id;
      g.selected_node = g.perspective;
      g.selected_edge.clear();
    }

    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();

    // Layout selector.
    ui.diplomacy_graph_layout = std::clamp(ui.diplomacy_graph_layout, 0, 2);
    const char* layout_labels[] = {"Radial", "Force", "Circle"};
    ImGui::SetNextItemWidth(160.0f);
    ImGui::Combo("Layout", &ui.diplomacy_graph_layout, layout_labels, IM_ARRAYSIZE(layout_labels));

    ImGui::SameLine();
    ImGui::Checkbox("All edges", &g.show_all_edges);
    ImGui::SameLine();
    ImGui::Checkbox("Matrix", &g.show_matrix);
    ImGui::SameLine();
    ImGui::Checkbox("Recent events", &g.show_recent_events);

    ImGui::SameLine();
    ImGui::Checkbox("Reciprocal edits", &g.reciprocal_edits);

    ImGui::SameLine();
    if (ImGui::Button("Re-center")) {
      g.pan = ImVec2(0, 0);
      g.zoom = 1.0f;
    }

    ImGui::SameLine();
    if (ImGui::Button("Unpin all")) {
      for (auto& kv : g.nodes) kv.second.pinned = false;
    }

    ImGui::SameLine();
    ImGui::Checkbox("Starfield", &ui.diplomacy_graph_starfield);
    ImGui::SameLine();
    ImGui::Checkbox("Grid", &ui.diplomacy_graph_grid);
    ImGui::SameLine();
    ImGui::Checkbox("Labels", &ui.diplomacy_graph_labels);
    ImGui::SameLine();
    ImGui::Checkbox("Arrows", &ui.diplomacy_graph_arrows);
  }

  ImGui::Separator();

  // --- Split: canvas + inspector ---
  const ImGuiTableFlags split_flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
  if (ImGui::BeginTable("diplomacy_split", 2, split_flags)) {
    ImGui::TableSetupColumn("Graph", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Inspector", ImGuiTableColumnFlags_WidthFixed, 360.0f);
    ImGui::TableNextRow();

    // --- Canvas ---
    ImGui::TableSetColumnIndex(0);
    {
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
      ImGui::BeginChild("diplomacy_canvas", ImVec2(0, 0), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

      const ImVec2 canvas_p0 = ImGui::GetCursorScreenPos();
      const ImVec2 canvas_sz = ImGui::GetContentRegionAvail();
      const ImVec2 canvas_p1 = ImVec2(canvas_p0.x + canvas_sz.x, canvas_p0.y + canvas_sz.y);

      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImU32 bg = ImGui::ColorConvertFloat4ToU32(ImVec4(0.05f, 0.055f, 0.065f, 1.0f));
      dl->AddRectFilled(canvas_p0, canvas_p1, bg);

      // Input capture.
      ImGui::InvisibleButton("##canvas_btn", canvas_sz,
                             ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                 ImGuiButtonFlags_MouseButtonMiddle);
      const bool hovered = ImGui::IsItemHovered();
      const bool active = ImGui::IsItemActive();
      (void)active;

      const ImGuiIO& io = ImGui::GetIO();
      CanvasXform xf;
      xf.origin = canvas_p0;
      xf.size = canvas_sz;
      xf.center = ImVec2(canvas_p0.x + canvas_sz.x * 0.5f, canvas_p0.y + canvas_sz.y * 0.5f);

      // Pan (MMB drag).
      if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
        g.panning = true;
      }
      if (g.panning) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
          const ImVec2 d = io.MouseDelta;
          g.pan.x += d.x / g.zoom;
          g.pan.y += d.y / g.zoom;
        } else {
          g.panning = false;
        }
      }

      // Zoom (wheel).
      if (hovered && io.MouseWheel != 0.0f) {
        const float zoom_before = g.zoom;
        const float zoom_factor = std::pow(1.12f, io.MouseWheel);
        g.zoom = clampf(g.zoom * zoom_factor, 0.20f, 3.75f);

        // Zoom to cursor.
        const ImVec2 mouse = io.MousePos;
        const ImVec2 w_before = xf.screen_to_world(mouse);
        // Update center-based transform by adjusting pan so that w_before maps to the same screen.
        const ImVec2 w_after = xf.screen_to_world(mouse); // uses old pan but new zoom through g.zoom
        (void)w_after;
        // Derivation: screen = center + (w + pan)*zoom => pan' = (screen-center)/zoom' - w
        const ImVec2 sc_rel(mouse.x - xf.center.x, mouse.y - xf.center.y);
        g.pan.x = (sc_rel.x / g.zoom) - w_before.x;
        g.pan.y = (sc_rel.y / g.zoom) - w_before.y;

        // Small snap to prevent precision drift.
        if (std::fabs(g.zoom - zoom_before) < 1e-6f) g.zoom = zoom_before;
      }

      // Background chrome.
      if (ui.diplomacy_graph_starfield) {
        StarfieldStyle sf;
        sf.enabled = true;
        sf.density = ui.map_starfield_density * 0.75f;
        sf.parallax = ui.map_starfield_parallax;
        sf.alpha = 1.0f;

        // The map helpers want pan expressed in pixels.
        const float pan_px_x = -g.pan.x * g.zoom;
        const float pan_px_y = -g.pan.y * g.zoom;
        const std::uint32_t seed = hash_u32(static_cast<std::uint32_t>(g.perspective)) ^ 0x34400u;
        draw_starfield(dl, canvas_p0, canvas_sz, bg, pan_px_x, pan_px_y, seed, sf);
      }
      if (ui.diplomacy_graph_grid) {
        GridStyle gs;
        gs.enabled = true;
        gs.desired_minor_px = 95.0f;
        gs.major_every = 5;

        const float op = ui.map_grid_opacity * 0.35f;
        gs.minor_alpha = 0.10f * op;
        gs.major_alpha = 0.18f * op;
        gs.axis_alpha = 0.25f * op;
        gs.label_alpha = 0.70f * op;

        // The diplomacy graph is already quite busy; keep it minimal.
        gs.labels = false;

        draw_grid(dl, canvas_p0, canvas_sz, xf.center, /*scale_px_per_unit=*/1.0, /*zoom=*/g.zoom,
                  Vec2{static_cast<double>(g.pan.x), static_cast<double>(g.pan.y)},
                  IM_COL32(220, 220, 230, 255), gs, "u");
      }

      // Layout updates (purely visual; uses current diplomacy state).
      {
        const float dt = std::clamp(io.DeltaTime, 0.0f, 0.05f);
        if (ui.diplomacy_graph_layout == 1) {
          // Force.
          std::vector<Id> ids;
          ids.reserve(facs.size());
          for (const auto* f : facs) ids.push_back(f->id);
          // Multiple micro-steps for stability.
          const float sub_dt = dt / 2.0f;
          step_force_layout(sim, ids, sub_dt);
          step_force_layout(sim, ids, sub_dt);
        } else if (ui.diplomacy_graph_layout == 0) {
          // Radial.
          apply_radial_targets(sim, facs, dt);
        } else {
          // Circle.
          apply_circle_targets(facs, dt);
        }
      }

      const float node_r = 18.0f * ui.ui_scale;
      const float node_r2 = node_r * node_r;

      // Hover detection: nodes.
      g.hovered_node = kInvalidId;
      float best_node_d2 = 1e30f;
      for (const auto* f : facs) {
        const Id id = f->id;
        const ImVec2 p = xf.world_to_screen(g.nodes[id].pos);
        const float dx = io.MousePos.x - p.x;
        const float dy = io.MousePos.y - p.y;
        const float d2 = dx * dx + dy * dy;
        if (d2 <= node_r2 * 1.15f && d2 < best_node_d2) {
          best_node_d2 = d2;
          g.hovered_node = id;
        }
      }

      // Precompute screen positions.
      std::unordered_map<Id, ImVec2> spos;
      spos.reserve(facs.size());
      for (const auto* f : facs) {
        const Id id = f->id;
        spos.emplace(id, xf.world_to_screen(g.nodes[id].pos));
      }

      auto status_enabled = [&](DiplomacyStatus st) {
        if (st == DiplomacyStatus::Hostile) return ui.diplomacy_graph_show_hostile;
        if (st == DiplomacyStatus::Neutral) return ui.diplomacy_graph_show_neutral;
        if (st == DiplomacyStatus::Friendly) return ui.diplomacy_graph_show_friendly;
        return true;
      };

      // Draw edges (behind nodes) + edge hover detection.
      g.hovered_edge.clear();
      float best_edge_dist = 1e30f;

      auto consider_edge_pick = [&](Id a, Id b, ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3, bool bezier) {
        const ImVec2 mouse = io.MousePos;
        float d = 1e30f;
        if (!bezier) {
          d = dist_point_to_segment(mouse, p0, p3);
        } else {
          // Sample the curve into a polyline and compute min distance.
          const int steps = 18;
          ImVec2 prev = p0;
          for (int i = 1; i <= steps; ++i) {
            const float t = (float)i / (float)steps;
            const ImVec2 cur = bezier_point(p0, p1, p2, p3, t);
            d = std::min(d, dist_point_to_segment(mouse, prev, cur));
            prev = cur;
          }
        }
        if (d < best_edge_dist) {
          best_edge_dist = d;
          g.hovered_edge.from = a;
          g.hovered_edge.to = b;
        }
      };

      const float curve_off = 28.0f;
      const float pick_thresh = 8.0f;

      for (std::size_t i = 0; i < facs.size(); ++i) {
        for (std::size_t j = i + 1; j < facs.size(); ++j) {
          const Id a = facs[i]->id;
          const Id b = facs[j]->id;

          if (!g.show_all_edges) {
            if (a != g.perspective && b != g.perspective) continue;
          }

          const DiplomacyStatus ab = sim.diplomatic_status(a, b);
          const DiplomacyStatus ba = sim.diplomatic_status(b, a);

          const bool draw_ab = status_enabled(ab);
          const bool draw_ba = status_enabled(ba);
          if (!draw_ab && !draw_ba) continue;

          const ImVec2 pa = spos[a];
          const ImVec2 pb = spos[b];

          const ImVec2 d(pb.x - pa.x, pb.y - pa.y);
          const float len = std::sqrt(d.x * d.x + d.y * d.y);
          if (len <= 1.0f) continue;
          const ImVec2 dir = ImVec2(d.x / len, d.y / len);
          const ImVec2 perp(-dir.y, dir.x);

          const bool same = (ab == ba);
          const bool is_focus_pair = (a == g.perspective || b == g.perspective);

          auto edge_alpha_base = [&](DiplomacyStatus st) {
            if (st == DiplomacyStatus::Friendly) return 0.85f;
            if (st == DiplomacyStatus::Neutral) return 0.62f;
            return 0.75f;
          };

          auto alpha_mul = [&](bool focused) {
            if (!ui.diplomacy_graph_dim_nonfocus) return 1.0f;
            if (!g.show_all_edges) return 1.0f;
            if (focused) return 1.0f;
            return 0.18f;
          };

          const bool hovered_any = g.hovered_edge.valid() &&
                                   ((g.hovered_edge.from == a && g.hovered_edge.to == b) ||
                                    (g.hovered_edge.from == b && g.hovered_edge.to == a));
          (void)hovered_any;

          if (same) {
            const float alpha = edge_alpha_base(ab) * alpha_mul(is_focus_pair);
            const float thick = (g.selected_edge.valid() &&
                                 ((g.selected_edge.from == a && g.selected_edge.to == b) ||
                                  (g.selected_edge.from == b && g.selected_edge.to == a)))
                                    ? 3.0f
                                    : 2.0f;
            dl->AddLine(pa, pb, status_color_u32(ab, alpha), thick);

            // Pick.
            consider_edge_pick(a, b, pa, pa, pb, pb, false);

            // Arrows (both ends if mutual).
            if (ui.diplomacy_graph_arrows) {
              const float asz = 10.0f * ui.ui_scale;
              add_arrowhead(dl, ImVec2(pb.x - dir.x * node_r, pb.y - dir.y * node_r), dir, asz,
                           status_color_u32(ab, alpha));
              add_arrowhead(dl, ImVec2(pa.x + dir.x * node_r, pa.y + dir.y * node_r), ImVec2(-dir.x, -dir.y), asz,
                           status_color_u32(ab, alpha));
            }
          } else {
            // Two directed curves.
            auto draw_curve = [&](Id from, Id to, DiplomacyStatus st, float side) {
              if (!status_enabled(st)) return;
              const bool focused = (from == g.perspective || to == g.perspective);
              const float alpha = edge_alpha_base(st) * alpha_mul(focused);

              const ImVec2 p_from = spos[from];
              const ImVec2 p_to = spos[to];
              const ImVec2 dd(p_to.x - p_from.x, p_to.y - p_from.y);
              const float l2 = dd.x * dd.x + dd.y * dd.y;
              if (l2 <= 1.0f) return;
              const float l = std::sqrt(l2);
              const ImVec2 dir2(dd.x / l, dd.y / l);
              const ImVec2 perp2(-dir2.y, dir2.x);

              const ImVec2 off(perp2.x * curve_off * side, perp2.y * curve_off * side);
              const ImVec2 p0(p_from.x, p_from.y);
              const ImVec2 p3(p_to.x, p_to.y);
              const ImVec2 p1(p_from.x + dd.x * 0.25f + off.x, p_from.y + dd.y * 0.25f + off.y);
              const ImVec2 p2(p_from.x + dd.x * 0.75f + off.x, p_from.y + dd.y * 0.75f + off.y);

              const bool selected = g.selected_edge.valid() && g.selected_edge.from == from && g.selected_edge.to == to;
              const float thick = selected ? 3.0f : 2.0f;

              dl->AddBezierCubic(p0, p1, p2, p3, status_color_u32(st, alpha), thick);

              if (ui.diplomacy_graph_arrows) {
                const float asz = 10.0f * ui.ui_scale;
                const ImVec2 tan = bezier_tangent(p0, p1, p2, p3, 0.98f);
                add_arrowhead(dl, ImVec2(p3.x - dir2.x * node_r, p3.y - dir2.y * node_r), tan, asz,
                             status_color_u32(st, alpha));
              }

              // Pick.
              consider_edge_pick(from, to, p0, p1, p2, p3, true);
            };

            draw_curve(a, b, ab, +1.0f);
            draw_curve(b, a, ba, -1.0f);
          }
        }
      }

      if (best_edge_dist > pick_thresh) {
        g.hovered_edge.clear();
      }

      // Node selection + drag.
      if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (g.hovered_node != kInvalidId) {
          g.selected_node = g.hovered_node;
          g.selected_edge.clear();
          g.dragging_node = g.hovered_node;
          g.nodes[g.dragging_node].pinned = true;
        } else if (g.hovered_edge.valid()) {
          g.selected_edge = g.hovered_edge;
          g.selected_node = kInvalidId;
        } else {
          g.selected_node = kInvalidId;
          g.selected_edge.clear();
        }
      }

      if (g.dragging_node != kInvalidId) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
          const ImVec2 d = io.MouseDelta;
          g.nodes[g.dragging_node].pos.x += d.x / g.zoom;
          g.nodes[g.dragging_node].pos.y += d.y / g.zoom;
        } else {
          g.dragging_node = kInvalidId;
        }
      }

      // Context menu (node or edge).
      if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        g.context_node = g.hovered_node;
        g.context_edge = g.hovered_edge;
        ImGui::OpenPopup("diplomacy_context");
      }

      if (ImGui::BeginPopup("diplomacy_context")) {
        if (g.context_node != kInvalidId) {
          const Faction* f = find_ptr(s.factions, g.context_node);
          ImGui::TextUnformatted(f ? f->name.c_str() : "Faction");
          ImGui::Separator();

          if (ImGui::MenuItem("Set as Perspective")) {
            g.perspective = g.context_node;
          }
          if (ImGui::MenuItem("Focus in Details -> Diplomacy")) {
            ui.show_details_window = true;
            ui.request_focus_faction_id = g.context_node;
            ui.request_details_tab = DetailsTab::Diplomacy;
          }
          if (ImGui::MenuItem("Center view")) {
            g.pan = ImVec2(-g.nodes[g.context_node].pos.x, -g.nodes[g.context_node].pos.y);
          }
          if (ImGui::MenuItem(g.nodes[g.context_node].pinned ? "Unpin" : "Pin")) {
            g.nodes[g.context_node].pinned = !g.nodes[g.context_node].pinned;
          }
        } else if (g.context_edge.valid()) {
          const Faction* a = find_ptr(s.factions, g.context_edge.from);
          const Faction* b = find_ptr(s.factions, g.context_edge.to);
          ImGui::Text("%s -> %s", a ? a->name.c_str() : "?", b ? b->name.c_str() : "?");
          ImGui::Separator();

          ImGui::Checkbox("Reciprocal", &g.reciprocal_edits);

          const DiplomacyStatus cur = sim.diplomatic_status(g.context_edge.from, g.context_edge.to);
          for (DiplomacyStatus st : {DiplomacyStatus::Hostile, DiplomacyStatus::Neutral, DiplomacyStatus::Friendly}) {
            const bool is_sel = (st == cur);
            if (ImGui::MenuItem(status_label(st), nullptr, is_sel)) {
              sim.set_diplomatic_status(g.context_edge.from, g.context_edge.to, st, g.reciprocal_edits);
              g.selected_edge = g.context_edge;
            }
          }
        } else {
          ImGui::TextDisabled("(no item)");
        }
        ImGui::EndPopup();
      }

      // Draw nodes on top.
      for (const auto* f : facs) {
        const Id id = f->id;
        const ImVec2 p = spos[id];

        const bool is_persp = (id == g.perspective);
        const bool is_sel = (id == g.selected_node);
        const bool is_hover = (id == g.hovered_node);

        float alpha = 1.0f;
        if (ui.diplomacy_graph_dim_nonfocus && g.show_all_edges && g.perspective != kInvalidId) {
          if (!is_persp && id != g.perspective) {
            // Dim nodes not tied to focus when in full-graph mode.
            alpha = 0.70f;
          }
        }

        const ImU32 fill = color_faction(id, alpha * (is_persp ? 1.0f : 0.95f));
        const ImU32 border = is_persp ? IM_COL32(250, 250, 255, 220) : IM_COL32(20, 20, 24, 220);

        // Glow.
        if (is_hover || is_sel || is_persp) {
          const float glow = node_r * (is_persp ? 1.65f : 1.35f);
          dl->AddCircleFilled(p, glow, ImGui::ColorConvertFloat4ToU32(ImVec4(1, 1, 1, (is_persp ? 0.10f : 0.07f))));
        }

        dl->AddCircleFilled(p, node_r, fill);
        dl->AddCircle(p, node_r, border, 0, is_persp ? 3.0f : (is_sel ? 2.5f : 1.5f));

        // Pin marker.
        if (g.nodes[id].pinned) {
          dl->AddCircleFilled(ImVec2(p.x + node_r * 0.72f, p.y - node_r * 0.72f), node_r * 0.22f,
                              IM_COL32(255, 255, 255, 210));
        }

        if (ui.diplomacy_graph_labels) {
          const char* name = f->name.c_str();
          const ImVec2 ts = ImGui::CalcTextSize(name);
          dl->AddText(ImVec2(p.x - ts.x * 0.5f, p.y - ts.y * 0.5f), IM_COL32(10, 10, 12, 230), name);
        }
      }

      // Hover tooltip.
      if (hovered && (g.hovered_node != kInvalidId || g.hovered_edge.valid())) {
        if (g.hovered_node != kInvalidId) {
          const Faction* f = find_ptr(s.factions, g.hovered_node);
          if (f) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(f->name.c_str());
            if (g.hovered_node != g.perspective) {
              const DiplomacyStatus st = sim.diplomatic_status(g.perspective, g.hovered_node);
              ImGui::Separator();
              ImGui::Text("%s -> %s: %s", find_ptr(s.factions, g.perspective)->name.c_str(), f->name.c_str(),
                          status_label(st));
            }
            ImGui::TextDisabled("LMB: select/drag  |  RMB: context menu");
            ImGui::EndTooltip();
          }
        } else if (g.hovered_edge.valid()) {
          const Faction* a = find_ptr(s.factions, g.hovered_edge.from);
          const Faction* b = find_ptr(s.factions, g.hovered_edge.to);
          if (a && b) {
            const DiplomacyStatus st = sim.diplomatic_status(g.hovered_edge.from, g.hovered_edge.to);
            ImGui::BeginTooltip();
            ImGui::Text("%s -> %s", a->name.c_str(), b->name.c_str());
            ImGui::Text("%s", status_label(st));
            ImGui::TextDisabled("LMB: select  |  RMB: edit");
            ImGui::EndTooltip();
          }
        }
      }

      // Legend.
      {
        const ImVec2 p = ImVec2(canvas_p0.x + 10.0f, canvas_p0.y + 10.0f);
        const float box = 10.0f * ui.ui_scale;
        dl->AddRectFilled(ImVec2(p.x - 6, p.y - 6), ImVec2(p.x + 210, p.y + 64), IM_COL32(10, 10, 12, 140), 6.0f);
        dl->AddRect(ImVec2(p.x - 6, p.y - 6), ImVec2(p.x + 210, p.y + 64), IM_COL32(255, 255, 255, 40), 6.0f);

        auto legend_row = [&](int row, DiplomacyStatus st) {
          const ImVec2 r0(p.x, p.y + row * 18.0f * ui.ui_scale);
          dl->AddRectFilled(r0, ImVec2(r0.x + box, r0.y + box), status_color_u32(st, 0.95f));
          dl->AddRect(r0, ImVec2(r0.x + box, r0.y + box), IM_COL32(20, 20, 24, 200));
          dl->AddText(ImVec2(r0.x + box + 8.0f, r0.y - 2.0f), IM_COL32(230, 230, 240, 210), status_label(st));
        };

        legend_row(0, DiplomacyStatus::Friendly);
        legend_row(1, DiplomacyStatus::Neutral);
        legend_row(2, DiplomacyStatus::Hostile);
      }

      ImGui::EndChild();
      ImGui::PopStyleVar();
    }

    // --- Inspector ---
    ImGui::TableSetColumnIndex(1);
    {
      ImGui::BeginChild("diplomacy_inspector", ImVec2(0, 0), true);

      // Selected faction panel.
      if (g.selected_node != kInvalidId) {
        if (const Faction* f = find_ptr(s.factions, g.selected_node)) {
          ImGui::SeparatorText("Faction");
          ImGui::TextUnformatted(f->name.c_str());
          ImGui::SameLine();
          if (g.selected_node == g.perspective) {
            ImGui::TextDisabled("(perspective)");
          }

          if (ImGui::Button("Set perspective")) {
            g.perspective = g.selected_node;
          }
          ImGui::SameLine();
          if (ImGui::Button("Focus in Details")) {
            ui.show_details_window = true;
            ui.request_focus_faction_id = g.selected_node;
            ui.request_details_tab = DetailsTab::Diplomacy;
          }
          ImGui::SameLine();
          if (ImGui::Button("Center")) {
            g.pan = ImVec2(-g.nodes[g.selected_node].pos.x, -g.nodes[g.selected_node].pos.y);
          }

          ImGui::Spacing();

          // Quick stance table (directed, from selected -> other).
          ImGui::SeparatorText("Stances (from selected)");
          ImGui::Checkbox("Reciprocal edits##ins", &g.reciprocal_edits);

          if (ImGui::BeginTable("stance_table", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("To", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Stance", ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableHeadersRow();

            for (const auto* other : facs) {
              if (other->id == f->id) continue;
              const DiplomacyStatus cur = sim.diplomatic_status(f->id, other->id);

              ImGui::TableNextRow();
              ImGui::TableSetColumnIndex(0);
              ImGui::TextUnformatted(other->name.c_str());
              ImGui::TableSetColumnIndex(1);

              int idx = (cur == DiplomacyStatus::Friendly) ? 2 : (cur == DiplomacyStatus::Neutral ? 1 : 0);
              const char* items[] = {"Hostile", "Neutral", "Friendly"};
              ImGui::PushID((int)other->id);
              ImGui::SetNextItemWidth(-1);
              if (ImGui::Combo("##stance", &idx, items, IM_ARRAYSIZE(items))) {
                DiplomacyStatus st = DiplomacyStatus::Hostile;
                if (idx == 1) st = DiplomacyStatus::Neutral;
                if (idx == 2) st = DiplomacyStatus::Friendly;
                sim.set_diplomatic_status(f->id, other->id, st, g.reciprocal_edits);
                g.selected_edge.from = f->id;
                g.selected_edge.to = other->id;
              }
              ImGui::PopID();
            }
            ImGui::EndTable();
          }
        }
      }

      // Selected edge editor.
      if (g.selected_edge.valid()) {
        const Faction* a = find_ptr(s.factions, g.selected_edge.from);
        const Faction* b = find_ptr(s.factions, g.selected_edge.to);
        if (a && b) {
          ImGui::Spacing();
          ImGui::SeparatorText("Selected relation");
          ImGui::Text("%s -> %s", a->name.c_str(), b->name.c_str());
          ImGui::Checkbox("Reciprocal##edge", &g.reciprocal_edits);
          const DiplomacyStatus cur = sim.diplomatic_status(g.selected_edge.from, g.selected_edge.to);

          for (DiplomacyStatus st : {DiplomacyStatus::Hostile, DiplomacyStatus::Neutral, DiplomacyStatus::Friendly}) {
            ImGui::PushID((int)st);
            ImGui::PushStyleColor(ImGuiCol_Button, status_color_rgba(st, 0.9f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, status_color_rgba(st, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, status_color_rgba(st, 1.0f));
            const bool pressed = ImGui::Button(status_label(st), ImVec2(-1, 0));
            ImGui::PopStyleColor(3);
            if (pressed) {
              sim.set_diplomatic_status(g.selected_edge.from, g.selected_edge.to, st, g.reciprocal_edits);
            }
            ImGui::PopID();
            if (st == cur) {
              ImGui::SameLine();
              ImGui::TextDisabled("(current)");
            }
          }

          ImGui::Spacing();
          ImGui::SeparatorText("Treaties");

          const std::int64_t now_day = s.date.days_since_epoch();
          auto treaties = sim.treaties_between(a->id, b->id);
          if (treaties.empty()) {
            ImGui::TextDisabled("No active treaties.");
          } else {
            const ImGuiTableFlags tflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV;
            if (ImGui::BeginTable("treaty_table", 3, tflags)) {
              ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch);
              ImGui::TableSetupColumn("Remaining", ImGuiTableColumnFlags_WidthFixed, 110.0f);
              ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 90.0f);
              ImGui::TableHeadersRow();

              for (const Treaty& t : treaties) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(treaty_type_label(t.type));

                ImGui::TableSetColumnIndex(1);
                if (t.duration_days > 0) {
                  const std::int64_t elapsed = now_day - t.start_day;
                  const int rem = std::max(0, t.duration_days - (int)elapsed);
                  ImGui::Text("%d d", rem);
                } else {
                  ImGui::TextUnformatted("∞");
                }

                ImGui::TableSetColumnIndex(2);
                ImGui::PushID((void*)(uintptr_t)t.id);
                if (ImGui::SmallButton("Break")) {
                  std::string err;
                  sim.cancel_treaty(t.id, /*push_event=*/true, &err);
                  g.new_treaty_error = err;
                }
                ImGui::PopID();
              }

              ImGui::EndTable();
            }
          }

          ImGui::Spacing();
          ImGui::SeparatorText("Offers");

          auto offers = sim.diplomatic_offers_between(a->id, b->id);
          if (offers.empty()) {
            ImGui::TextDisabled("No pending offers.");
          } else {
            const ImGuiTableFlags oflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV;
            if (ImGui::BeginTable("offer_table", 5, oflags)) {
              ImGui::TableSetupColumn("From", ImGuiTableColumnFlags_WidthStretch);
              ImGui::TableSetupColumn("To", ImGuiTableColumnFlags_WidthStretch);
              ImGui::TableSetupColumn("Offer", ImGuiTableColumnFlags_WidthStretch);
              ImGui::TableSetupColumn("Expires", ImGuiTableColumnFlags_WidthFixed, 90.0f);
              ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 140.0f);
              ImGui::TableHeadersRow();

              for (const DiplomaticOffer& o : offers) {
                const Faction* ofrom = find_ptr(s.factions, o.from_faction_id);
                const Faction* oto = find_ptr(s.factions, o.to_faction_id);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(ofrom ? ofrom->name.c_str() : "(unknown)");

                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(oto ? oto->name.c_str() : "(unknown)");

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%s%s", treaty_type_label(o.treaty_type),
                            (o.treaty_duration_days < 0) ? " (∞)" : "");

                ImGui::TableSetColumnIndex(3);
                if (o.expire_day < 0) {
                  ImGui::TextUnformatted("∞");
                } else {
                  const int rem = std::max(0, (int)(o.expire_day - now_day));
                  ImGui::Text("%d d", rem);
                }

                ImGui::TableSetColumnIndex(4);
                ImGui::PushID((void*)(uintptr_t)o.id);
                if (ImGui::SmallButton("Accept")) {
                  std::string err;
                  (void)sim.accept_diplomatic_offer(o.id, /*push_event=*/true, &err);
                  g.offer_error = err;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Decline")) {
                  std::string err;
                  (void)sim.decline_diplomatic_offer(o.id, /*push_event=*/true, &err);
                  g.offer_error = err;
                }
                ImGui::PopID();
              }

              ImGui::EndTable();
            }
          }

          if (!g.offer_error.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.55f, 1.0f), "%s", g.offer_error.c_str());
          }

          ImGui::Spacing();
          ImGui::SeparatorText("Sign / renew treaty");

          static const TreatyType kTreatyTypes[] = {TreatyType::Ceasefire, TreatyType::NonAggressionPact,
                                                    TreatyType::Alliance, TreatyType::TradeAgreement};
          static const char* kTreatyTypeLabels[] = {"Ceasefire", "Non-Aggression Pact", "Alliance",
                                                    "Trade Agreement"};
          static_assert(IM_ARRAYSIZE(kTreatyTypes) == IM_ARRAYSIZE(kTreatyTypeLabels));

          g.new_treaty_type_index = std::clamp(g.new_treaty_type_index, 0, (int)IM_ARRAYSIZE(kTreatyTypes) - 1);
          ImGui::SetNextItemWidth(-1);
          ImGui::Combo("Type##new_treaty", &g.new_treaty_type_index, kTreatyTypeLabels,
                      IM_ARRAYSIZE(kTreatyTypeLabels));

          ImGui::Checkbox("Indefinite##new_treaty", &g.new_treaty_indefinite);
          if (!g.new_treaty_indefinite) {
            g.new_treaty_duration_days = std::clamp(g.new_treaty_duration_days, 1, 36500);
            ImGui::InputInt("Duration (days)##new_treaty", &g.new_treaty_duration_days);
            g.new_treaty_duration_days = std::clamp(g.new_treaty_duration_days, 1, 36500);
          } else {
            ImGui::TextDisabled("Duration: indefinite");
          }

          if (ImGui::Button("Sign treaty##new_treaty", ImVec2(-1, 0))) {
            std::string err;
            const TreatyType tt = kTreatyTypes[g.new_treaty_type_index];
            const int dur = g.new_treaty_indefinite ? -1 : g.new_treaty_duration_days;
            const Id tid = sim.create_treaty(a->id, b->id, tt, dur, /*push_event=*/true, &err);
            g.new_treaty_error = err;
            (void)tid;
          }

          if (!g.new_treaty_error.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.55f, 1.0f), "%s", g.new_treaty_error.c_str());
          }

          ImGui::Spacing();
          ImGui::SeparatorText("Send offer");

          g.new_offer_expires_days = std::clamp(g.new_offer_expires_days, 1, 365);
          ImGui::InputInt("Offer expiry (days)##new_offer", &g.new_offer_expires_days);
          g.new_offer_expires_days = std::clamp(g.new_offer_expires_days, 1, 365);

          if (ImGui::Button("Send offer##new_offer", ImVec2(-1, 0))) {
            g.offer_error.clear();
            const TreatyType tt = kTreatyTypes[g.new_treaty_type_index];
            const int dur = g.new_treaty_indefinite ? -1 : g.new_treaty_duration_days;
            std::string err;
            (void)sim.create_diplomatic_offer(a->id, b->id, tt, dur, g.new_offer_expires_days,
                                              /*push_event=*/true, &err);
            g.offer_error = err;
          }
        }
      }

      // Matrix editor.
      if (g.show_matrix) {
        ImGui::Spacing();
        ImGui::SeparatorText("Matrix (row -> col)");
        ImGui::TextDisabled("Tip: click a cell to cycle stance. Hold Shift to apply reciprocally.");

        const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX |
                                      ImGuiTableFlags_SizingFixedFit;
        const float matrix_h = 280.0f;
        if (ImGui::BeginChild("matrix_child", ImVec2(0, matrix_h), true, ImGuiWindowFlags_HorizontalScrollbar)) {
          const int cols = (int)facs.size() + 1;
          if (ImGui::BeginTable("matrix", cols, flags)) {
            ImGui::TableSetupScrollFreeze(1, 1);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            for (const auto* f : facs) {
              ImGui::TableSetupColumn(f->name.c_str(), ImGuiTableColumnFlags_WidthFixed, 38.0f);
            }
            ImGui::TableHeadersRow();

            for (const auto* rowf : facs) {
              ImGui::TableNextRow();
              ImGui::TableSetColumnIndex(0);
              ImGui::TextUnformatted(rowf->name.c_str());

              for (int c = 0; c < (int)facs.size(); ++c) {
                const auto* colf = facs[(std::size_t)c];
                ImGui::TableSetColumnIndex(c + 1);
                if (rowf->id == colf->id) {
                  ImGui::TextDisabled("-");
                  continue;
                }

                const DiplomacyStatus cur = sim.diplomatic_status(rowf->id, colf->id);
                ImGui::PushID((int)rowf->id);
                ImGui::PushID((int)colf->id);
                ImGui::PushStyleColor(ImGuiCol_Button, status_color_rgba(cur, 0.92f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, status_color_rgba(cur, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, status_color_rgba(cur, 1.0f));
                if (ImGui::Button(status_short(cur), ImVec2(30.0f, 0))) {
                  const DiplomacyStatus next = cycle_status(cur);
                  const bool recip = ImGui::GetIO().KeyShift ? true : g.reciprocal_edits;
                  sim.set_diplomatic_status(rowf->id, colf->id, next, recip);
                  g.selected_edge.from = rowf->id;
                  g.selected_edge.to = colf->id;
                }
                ImGui::PopStyleColor(3);
                ImGui::PopID();
                ImGui::PopID();
              }
            }

            ImGui::EndTable();
          }
        }
        ImGui::EndChild();
      }

      // Recent diplomacy events (links to timeline).
      if (g.show_recent_events) {
        ImGui::Spacing();
        ImGui::SeparatorText("Recent diplomacy events");

        int shown = 0;
        for (int i = (int)s.events.size() - 1; i >= 0 && shown < 10; --i) {
          const SimEvent& ev = s.events[(std::size_t)i];
          if (ev.category != EventCategory::Diplomacy) continue;

          const std::string d = nebula4x::Date(ev.day).to_string();
          ImGui::PushID((int)ev.seq);
          if (ImGui::Selectable((d + "  " + ev.message).c_str(), false)) {
            ui.show_timeline_window = true;
            ui.request_focus_event_seq = ev.seq;
          }
          ImGui::PopID();
          ++shown;
        }

        if (shown == 0) {
          ImGui::TextDisabled("No diplomacy events yet.");
        } else {
          ImGui::TextDisabled("Click to jump to Timeline.");
        }
      }

      ImGui::EndChild();
    }

    ImGui::EndTable();
  }

  ImGui::End();
}

} // namespace nebula4x::ui
