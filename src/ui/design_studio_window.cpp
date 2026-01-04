#include "ui/design_studio_window.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nebula4x/util/strings.h"

#include "ui/map_render.h"

namespace nebula4x::ui {

namespace {

// --- small ui helpers ---

bool case_insensitive_contains(const std::string& haystack, const char* needle_cstr) {
  if (!needle_cstr || needle_cstr[0] == '\0') return true;
  std::string h = nebula4x::to_lower(haystack);
  std::string n = nebula4x::to_lower(std::string(needle_cstr));
  return h.find(n) != std::string::npos;
}

const char* ship_role_label(ShipRole r) {
  switch (r) {
    case ShipRole::Freighter: return "Freighter";
    case ShipRole::Surveyor: return "Surveyor";
    case ShipRole::Combatant: return "Combatant";
    default: return "Unknown";
  }
}

const char* component_type_label(ComponentType t) {
  switch (t) {
    case ComponentType::Engine: return "Engine";
    case ComponentType::Reactor: return "Reactor";
    case ComponentType::FuelTank: return "Fuel Tank";
    case ComponentType::Cargo: return "Cargo";
    case ComponentType::Mining: return "Mining";
    case ComponentType::Sensor: return "Sensor";
    case ComponentType::Weapon: return "Weapon";
    case ComponentType::Armor: return "Armor";
    case ComponentType::Shield: return "Shield";
    case ComponentType::ColonyModule: return "Colony Module";
    case ComponentType::TroopBay: return "Troop Bay";
    default: return "Unknown";
  }
}

int type_rank(ComponentType t) {
  switch (t) {
    case ComponentType::Engine: return 0;
    case ComponentType::Reactor: return 1;
    case ComponentType::FuelTank: return 2;
    case ComponentType::Cargo: return 3;
    case ComponentType::Mining: return 4;
    case ComponentType::ColonyModule: return 5;
    case ComponentType::TroopBay: return 6;
    case ComponentType::Sensor: return 7;
    case ComponentType::Weapon: return 8;
    case ComponentType::Armor: return 9;
    case ComponentType::Shield: return 10;
    default: return 99;
  }
}

ImU32 with_alpha(ImU32 c, float a) {
  const std::uint32_t ca = static_cast<std::uint32_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f);
  return (c & 0x00FFFFFFu) | (ca << 24);
}

ImU32 mul_rgb(ImU32 c, float m, float a_mul = 1.0f) {
  const std::uint32_t r = (c >> 0) & 0xFFu;
  const std::uint32_t g = (c >> 8) & 0xFFu;
  const std::uint32_t b = (c >> 16) & 0xFFu;
  const std::uint32_t a = (c >> 24) & 0xFFu;

  auto clamp_u8 = [](int v) { return static_cast<std::uint32_t>(std::clamp(v, 0, 255)); };
  const std::uint32_t nr = clamp_u8(static_cast<int>(std::round((double)r * m)));
  const std::uint32_t ng = clamp_u8(static_cast<int>(std::round((double)g * m)));
  const std::uint32_t nb = clamp_u8(static_cast<int>(std::round((double)b * m)));
  const std::uint32_t na = clamp_u8(static_cast<int>(std::round((double)a * a_mul)));
  return (nr << 0) | (ng << 8) | (nb << 16) | (na << 24);
}

ImU32 color_for_component_type(ComponentType t) {
  switch (t) {
    case ComponentType::Engine: return IM_COL32(90, 170, 255, 220);
    case ComponentType::Reactor: return IM_COL32(255, 220, 90, 220);
    case ComponentType::FuelTank: return IM_COL32(90, 235, 150, 220);
    case ComponentType::Cargo: return IM_COL32(255, 170, 90, 220);
    case ComponentType::Mining: return IM_COL32(140, 255, 140, 220);
    case ComponentType::ColonyModule: return IM_COL32(220, 130, 255, 220);
    case ComponentType::TroopBay: return IM_COL32(255, 200, 150, 220);
    case ComponentType::Sensor: return IM_COL32(150, 150, 255, 220);
    case ComponentType::Weapon: return IM_COL32(255, 100, 100, 220);
    case ComponentType::Armor: return IM_COL32(190, 190, 190, 220);
    case ComponentType::Shield: return IM_COL32(90, 255, 255, 220);
    default: return IM_COL32(220, 220, 220, 220);
  }
}

std::vector<std::string> sorted_all_design_ids(const Simulation& sim) {
  std::vector<std::string> ids;
  ids.reserve(sim.content().designs.size() + sim.state().custom_designs.size());
  for (const auto& [id, _] : sim.content().designs) ids.push_back(id);
  for (const auto& [id, _] : sim.state().custom_designs) ids.push_back(id);
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

// --- squarified treemap layout ---

struct RectD {
  double x{0.0};
  double y{0.0};
  double w{0.0};
  double h{0.0};
};

struct LayoutRect {
  int id{-1};
  RectD r;
};

double rect_area(const RectD& r) { return std::max(0.0, r.w) * std::max(0.0, r.h); }

struct Node {
  int id{-1};
  double area{0.0};
};

double sum_area(const std::vector<Node>& row) {
  double s = 0.0;
  for (const auto& n : row) s += n.area;
  return s;
}

double worst_ratio(const std::vector<Node>& row, double w) {
  if (row.empty()) return std::numeric_limits<double>::infinity();
  const double s = sum_area(row);
  if (s <= 0.0 || w <= 1e-9) return std::numeric_limits<double>::infinity();

  double max_a = 0.0;
  double min_a = std::numeric_limits<double>::infinity();
  for (const auto& n : row) {
    max_a = std::max(max_a, n.area);
    min_a = std::min(min_a, n.area);
  }
  if (min_a <= 1e-9) return std::numeric_limits<double>::infinity();

  const double w2 = w * w;
  const double s2 = s * s;
  const double r1 = (w2 * max_a) / s2;
  const double r2 = s2 / (w2 * min_a);
  return std::max(r1, r2);
}

void layout_row(const std::vector<Node>& row, RectD bounds, std::vector<LayoutRect>& out) {
  const double s = sum_area(row);
  if (s <= 0.0) return;

  // Lay out along the longer side; consume a strip from the shorter side.
  if (bounds.w >= bounds.h) {
    // Horizontal strip.
    const double strip_h = (bounds.w > 1e-9) ? (s / bounds.w) : 0.0;
    double x = bounds.x;
    for (const auto& n : row) {
      const double rw = (strip_h > 1e-9) ? (n.area / strip_h) : 0.0;
      out.push_back(LayoutRect{n.id, RectD{x, bounds.y, rw, strip_h}});
      x += rw;
    }
  } else {
    // Vertical strip.
    const double strip_w = (bounds.h > 1e-9) ? (s / bounds.h) : 0.0;
    double y = bounds.y;
    for (const auto& n : row) {
      const double rh = (strip_w > 1e-9) ? (n.area / strip_w) : 0.0;
      out.push_back(LayoutRect{n.id, RectD{bounds.x, y, strip_w, rh}});
      y += rh;
    }
  }
}

RectD consume_row_bounds(const std::vector<Node>& row, RectD bounds) {
  const double s = sum_area(row);
  if (s <= 0.0) return bounds;
  if (bounds.w >= bounds.h) {
    const double strip_h = (bounds.w > 1e-9) ? (s / bounds.w) : 0.0;
    bounds.y += strip_h;
    bounds.h = std::max(0.0, bounds.h - strip_h);
  } else {
    const double strip_w = (bounds.h > 1e-9) ? (s / bounds.h) : 0.0;
    bounds.x += strip_w;
    bounds.w = std::max(0.0, bounds.w - strip_w);
  }
  return bounds;
}

std::vector<LayoutRect> squarify(std::vector<Node> nodes, RectD bounds) {
  std::vector<LayoutRect> out;
  out.reserve(nodes.size());

  // Normalize node areas to the container.
  {
    double total = 0.0;
    for (const auto& n : nodes) total += std::max(0.0, n.area);
    const double target = rect_area(bounds);
    if (total > 1e-9 && target > 0.0) {
      const double k = target / total;
      for (auto& n : nodes) n.area = std::max(0.0, n.area) * k;
    }
  }

  // Sort descending.
  std::sort(nodes.begin(), nodes.end(), [](const Node& a, const Node& b) { return a.area > b.area; });

  while (!nodes.empty() && rect_area(bounds) > 1e-6) {
    std::vector<Node> row;
    row.reserve(8);

    const double w = std::min(bounds.w, bounds.h);
    // Build a row while the worst aspect ratio improves.
    while (!nodes.empty()) {
      const Node next = nodes.front();
      if (row.empty()) {
        row.push_back(next);
        nodes.erase(nodes.begin());
        continue;
      }

      const double prev = worst_ratio(row, w);
      std::vector<Node> cand = row;
      cand.push_back(next);
      const double now = worst_ratio(cand, w);

      if (now <= prev) {
        row = std::move(cand);
        nodes.erase(nodes.begin());
      } else {
        break;
      }
    }

    layout_row(row, bounds, out);
    bounds = consume_row_bounds(row, bounds);
  }

  return out;
}

struct CompDraw {
  std::string component_id;
  std::string name;
  ComponentType type{ComponentType::Unknown};
  double mass_tons{0.0};
  double power_out{0.0};
  double power_use{0.0};
  double mining_tpd{0.0};
  double hp_bonus{0.0};
  double shield_hp{0.0};
  double weapon_dmg{0.0};
  double weapon_range{0.0};
  double sensor_range{0.0};
  double cargo_tons{0.0};
  double fuel_cap{0.0};
  double fuel_use_per_mkm{0.0};
  double colony_cap{0.0};
};

struct GroupDraw {
  ComponentType type{ComponentType::Unknown};
  std::vector<CompDraw> comps;
  double total_mass{0.0};
};

void draw_power_overlay(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1, const ShipDesign& d) {
  const float w = std::max(1.0f, p1.x - p0.x);
  const float h = std::max(1.0f, p1.y - p0.y);

  const double gen = std::max(0.0, d.power_generation);
  const double use = std::max(0.0, d.power_use_total);
  const double denom = std::max(1.0, std::max(gen, use));
  const float gen_frac = static_cast<float>(std::clamp(gen / denom, 0.0, 1.0));
  const float use_frac = static_cast<float>(std::clamp(use / denom, 0.0, 1.0));

  const ImU32 bg = IM_COL32(25, 25, 28, 200);
  const ImU32 outline = IM_COL32(0, 0, 0, 180);
  dl->AddRectFilled(p0, p1, bg, 4.0f);
  dl->AddRect(p0, p1, outline, 4.0f);

  // Generation (green) and usage (orange/red if deficit).
  const ImVec2 gen_p1(p0.x + w * gen_frac, p1.y);
  const ImVec2 use_p1(p0.x + w * use_frac, p1.y);
  dl->AddRectFilled(p0, gen_p1, IM_COL32(80, 220, 140, 190), 4.0f, ImDrawFlags_RoundCornersLeft);
  const ImU32 use_col = (use <= gen + 1e-9) ? IM_COL32(255, 200, 80, 200) : IM_COL32(255, 90, 90, 210);
  dl->AddRectFilled(p0, use_p1, use_col, 4.0f, ImDrawFlags_RoundCornersLeft);
  dl->AddRect(p0, p1, outline, 4.0f);

  char buf[128];
  if (use <= gen + 1e-9) {
    std::snprintf(buf, sizeof(buf), "Power: %.1f gen / %.1f use", gen, use);
  } else {
    std::snprintf(buf, sizeof(buf), "Power: %.1f gen / %.1f use (DEFICIT %.1f)", gen, use, use - gen);
  }
  const ImVec2 ts = ImGui::CalcTextSize(buf);
  const ImVec2 tp(p0.x + 6.0f, p0.y + (h - ts.y) * 0.5f);
  dl->AddText(tp, IM_COL32(235, 235, 235, 255), buf);
}

std::string compact_name(const std::string& s, int max_chars) {
  if (max_chars <= 0) return std::string{};
  if ((int)s.size() <= max_chars) return s;
  if (max_chars <= 1) return s.substr(0, 1);
  return s.substr(0, (size_t)(max_chars - 1)) + "…";
}

}  // namespace

void draw_design_studio_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  (void)selected_colony;
  (void)selected_body;

  if (!ui.show_design_studio_window) return;

  ImGui::SetNextWindowSize(ImVec2(1040, 760), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Design Studio", &ui.show_design_studio_window)) {
    ImGui::End();
    return;
  }

  const auto all_ids = sorted_all_design_ids(sim);
  if (all_ids.empty()) {
    ImGui::TextDisabled("No designs available.");
    ImGui::End();
    return;
  }

  // Persistent in-window selection.
  static std::string selected_id;
  static std::string compare_id;
  static std::string selected_component_id;
  static char search_buf[64] = "";
  static bool initialized = false;
  static std::string last_selected_id;

  if (!initialized) {
    initialized = true;
    selected_id = all_ids.front();
    compare_id.clear();
  }

  // External focus request.
  if (!ui.request_focus_design_studio_id.empty()) {
    const auto it = std::find(all_ids.begin(), all_ids.end(), ui.request_focus_design_studio_id);
    if (it != all_ids.end()) {
      selected_id = *it;
    }
    ui.request_focus_design_studio_id.clear();
  }

  if (selected_id.empty()) selected_id = all_ids.front();
  if (std::find(all_ids.begin(), all_ids.end(), selected_id) == all_ids.end()) {
    selected_id = all_ids.front();
  }

  const ShipDesign* design = sim.find_design(selected_id);
  if (!design) {
    ImGui::TextDisabled("Design not found.");
    ImGui::End();
    return;
  }

  // Reset per-design transient selection when the design changes.
  if (selected_id != last_selected_id) {
    selected_component_id.clear();
    last_selected_id = selected_id;
  }

  // Layout: left design list + main (canvas + info).
  const ImGuiTableFlags outer_flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV |
                                      ImGuiTableFlags_SizingStretchProp;
  if (ImGui::BeginTable("design_studio_outer", 2, outer_flags)) {
    ImGui::TableSetupColumn("Designs", ImGuiTableColumnFlags_WidthFixed, 280.0f);
    ImGui::TableSetupColumn("Main", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableNextRow();

    // --- Left: designs list ---
    ImGui::TableSetColumnIndex(0);
    {
      ImGui::TextDisabled("Designs (%zu)", all_ids.size());
      ImGui::InputTextWithHint("##ds_search", "Search…", search_buf, IM_ARRAYSIZE(search_buf));
      ImGui::Separator();

      if (ImGui::BeginChild("##ds_design_list", ImVec2(0, 0), false)) {
        for (const auto& id : all_ids) {
          const auto* d = sim.find_design(id);
          const std::string name = d ? d->name : id;
          if (search_buf[0] != '\0') {
            if (!case_insensitive_contains(name, search_buf) && !case_insensitive_contains(id, search_buf)) continue;
          }

          const bool sel = (id == selected_id);
          std::string label = name;
          if (d) {
            label += "  [";
            label += ship_role_label(d->role);
            label += "]";
          }
          label += "##" + id;

          if (ImGui::Selectable(label.c_str(), sel)) {
            selected_id = id;
          }
          if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("%s", name.c_str());
            ImGui::Separator();
            ImGui::Text("ID: %s", id.c_str());
            if (d) {
              ImGui::Text("Mass: %.0f t", d->mass_tons);
              ImGui::Text("Speed: %.1f km/s", d->speed_km_s);
              if (d->fuel_use_per_mkm > 0.0 && d->fuel_capacity_tons > 0.0) {
                ImGui::Text("Range: %.0f mkm", d->fuel_capacity_tons / d->fuel_use_per_mkm);
              }
              if (d->weapon_damage > 0.0) {
                ImGui::Text("Weapons: %.1f (%.1f mkm)", d->weapon_damage, d->weapon_range_mkm);
              }
            }
            ImGui::EndTooltip();
          }
        }
      }
      ImGui::EndChild();
    }

    // --- Right: main ---
    ImGui::TableSetColumnIndex(1);
    {
      ImGui::SeparatorText("Design");
      ImGui::Text("%s", design->name.c_str());
      ImGui::SameLine();
      ImGui::TextDisabled("(%s)", selected_id.c_str());
      ImGui::SameLine();
      ImGui::TextDisabled("[%s]", ship_role_label(design->role));

      ImGui::SameLine();
      ImGui::Dummy(ImVec2(12, 0));
      ImGui::SameLine();
      if (ImGui::SmallButton("Open in Details")) {
        ui.show_details_window = true;
        ui.request_details_tab = DetailsTab::Design;
        ui.request_focus_design_id = selected_id;
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Jump to the existing Details > Design tab for editing / cloning.");

      ImGui::SameLine();
      ImGui::Checkbox("Grid", &ui.design_studio_show_grid);
      ImGui::SameLine();
      ImGui::Checkbox("Labels", &ui.design_studio_show_labels);
      ImGui::SameLine();
      ImGui::Checkbox("Compare", &ui.design_studio_show_compare);
      ImGui::SameLine();
      ImGui::Checkbox("Power", &ui.design_studio_show_power_overlay);

      if (ui.design_studio_show_compare) {
        if (compare_id.empty() || std::find(all_ids.begin(), all_ids.end(), compare_id) == all_ids.end()) {
          compare_id = (all_ids.size() >= 2) ? all_ids[1] : all_ids[0];
        }

        std::vector<const char*> labels;
        labels.reserve(all_ids.size());
        std::vector<std::string> label_storage;
        label_storage.reserve(all_ids.size());

        int cur_idx = 0;
        for (int i = 0; i < (int)all_ids.size(); ++i) {
          const auto* d = sim.find_design(all_ids[(size_t)i]);
          std::string lbl = d ? d->name : all_ids[(size_t)i];
          lbl += "##cmp_" + all_ids[(size_t)i];
          label_storage.push_back(std::move(lbl));
          labels.push_back(label_storage.back().c_str());
          if (all_ids[(size_t)i] == compare_id) cur_idx = i;
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(240.0f);
        if (ImGui::Combo("##ds_compare", &cur_idx, labels.data(), (int)labels.size())) {
          compare_id = all_ids[(size_t)cur_idx];
        }
      }

      // Split: blueprint canvas + info panel.
      const ImGuiTableFlags inner_flags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
      if (ImGui::BeginTable("design_studio_inner", 2, inner_flags, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("Canvas", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Info", ImGuiTableColumnFlags_WidthFixed, 330.0f);
        ImGui::TableNextRow();

        // ---- Canvas ----
        ImGui::TableSetColumnIndex(0);
        {
          const ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
          const ImVec2 canvas_size = ImGui::GetContentRegionAvail();
          const ImVec2 canvas_p1(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y);

          ImGui::InvisibleButton("##ds_canvas_btn", canvas_size, ImGuiButtonFlags_MouseButtonLeft |
                                                           ImGuiButtonFlags_MouseButtonRight |
                                                           ImGuiButtonFlags_MouseButtonMiddle);
          const bool hovered = ImGui::IsItemHovered();
          ImDrawList* dl = ImGui::GetWindowDrawList();

          // Persistent view state.
          static double user_zoom = 1.0;
          static Vec2 pan{0.0, 0.0};
          static std::string view_design_id;

          const double hull_w = 220.0;
          const double hull_h = 86.0;
          const double fit_scale = std::min((double)std::max(1.0f, canvas_size.x) / (hull_w * 1.25),
                                            (double)std::max(1.0f, canvas_size.y) / (hull_h * 1.65));

          if (view_design_id != selected_id) {
            // When switching designs, reset to a nice fit.
            view_design_id = selected_id;
            user_zoom = 1.0;
            pan = Vec2{0.0, 0.0};
          }

          const double zoom = std::clamp(user_zoom, 0.25, 8.0);
          const ImVec2 center(canvas_pos.x + canvas_size.x * 0.5f, canvas_pos.y + canvas_size.y * 0.5f);
          const double scale = std::max(0.0001, fit_scale);

          auto to_screen = [&](const Vec2& w) -> ImVec2 {
            return ImVec2(static_cast<float>(center.x + (w.x + pan.x) * scale * zoom),
                          static_cast<float>(center.y + (w.y + pan.y) * scale * zoom));
          };
          auto to_world = [&](const ImVec2& s) -> Vec2 {
            return Vec2{(s.x - center.x) / (scale * zoom) - pan.x, (s.y - center.y) / (scale * zoom) - pan.y};
          };

          // Input.
          ImGuiIO& io = ImGui::GetIO();
          if (hovered && !io.WantTextInput) {
            if (io.MouseWheel != 0.0f) {
              const Vec2 before = to_world(io.MousePos);
              user_zoom *= std::pow(1.12, (double)io.MouseWheel);
              user_zoom = std::clamp(user_zoom, 0.25, 8.0);
              const Vec2 after = to_world(io.MousePos);
              pan.x += (before.x - after.x);
              pan.y += (before.y - after.y);
            }
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
              const ImVec2 d = io.MouseDelta;
              pan.x += (double)d.x / (scale * zoom);
              pan.y += (double)d.y / (scale * zoom);
            }
          }

          // Background.
          const ImU32 bg_top = ImGui::GetColorU32(ImVec4(0.06f, 0.065f, 0.075f, 1.0f));
          const ImU32 bg_bot = ImGui::GetColorU32(ImVec4(0.03f, 0.035f, 0.042f, 1.0f));
          dl->AddRectFilledMultiColor(canvas_pos, canvas_p1, bg_top, bg_top, bg_bot, bg_bot);

          // Clip so our draws don't bleed into other columns.
          dl->PushClipRect(canvas_pos, canvas_p1, true);

          // Grid.
          if (ui.design_studio_show_grid) {
            GridStyle gs;
            gs.enabled = true;
            gs.desired_minor_px = 54.0f;
            gs.major_every = 5;
            gs.labels = false;
            gs.minor_alpha = 0.08f;
            gs.major_alpha = 0.13f;
            gs.axis_alpha = 0.18f;
            draw_grid(dl, canvas_pos, canvas_size, center, scale, zoom, pan, IM_COL32(220, 220, 220, 255), gs, "");
          }

          // Hull outline.
          const RectD hull{-hull_w * 0.5, -hull_h * 0.5, hull_w, hull_h};
          const RectD inner{hull.x + 6.0, hull.y + 6.0, hull.w - 12.0, hull.h - 12.0};

          const ImVec2 hull_p0 = to_screen(Vec2{hull.x, hull.y});
          const ImVec2 hull_p1 = to_screen(Vec2{hull.x + hull.w, hull.y + hull.h});

          // Soft glow + outline.
          const ImU32 glow = modulate_alpha(IM_COL32(120, 190, 255, 255), 0.10f);
          dl->AddRectFilled(hull_p0, hull_p1, glow, 18.0f);
          dl->AddRect(hull_p0, hull_p1, IM_COL32(0, 0, 0, 220), 18.0f, 0, 2.0f);
          dl->AddRect(hull_p0, hull_p1, IM_COL32(210, 210, 220, 160), 18.0f, 0, 1.25f);

          // Build groups.
          std::unordered_map<ComponentType, GroupDraw> groups;
          groups.reserve(12);

          for (const auto& cid : design->components) {
            CompDraw cd;
            cd.component_id = cid;

            const auto it = sim.content().components.find(cid);
            if (it != sim.content().components.end()) {
              const auto& c = it->second;
              cd.name = c.name;
              cd.type = c.type;
              cd.mass_tons = c.mass_tons;
              cd.power_out = c.power_output;
              cd.power_use = c.power_use;
              cd.mining_tpd = c.mining_tons_per_day;
              cd.hp_bonus = c.hp_bonus;
              cd.shield_hp = c.shield_hp;
              cd.weapon_dmg = c.weapon_damage;
              cd.weapon_range = c.weapon_range_mkm;
              cd.sensor_range = c.sensor_range_mkm;
              cd.cargo_tons = c.cargo_tons;
              cd.fuel_cap = c.fuel_capacity_tons;
              cd.fuel_use_per_mkm = c.fuel_use_per_mkm;
              cd.colony_cap = c.colony_capacity_millions;
            } else {
              cd.name = cid;
              cd.type = ComponentType::Unknown;
              cd.mass_tons = 1.0;
            }

            auto& g = groups[cd.type];
            g.type = cd.type;
            g.total_mass += std::max(0.0, cd.mass_tons);
            g.comps.push_back(std::move(cd));
          }

          // Convert groups to a stable ordered list.
          std::vector<GroupDraw> group_list;
          group_list.reserve(groups.size());
          for (auto& [_, g] : groups) {
            group_list.push_back(std::move(g));
          }
          std::sort(group_list.begin(), group_list.end(), [](const GroupDraw& a, const GroupDraw& b) {
            const int ra = type_rank(a.type);
            const int rb = type_rank(b.type);
            if (ra != rb) return ra < rb;
            return a.total_mass > b.total_mass;
          });

          // Layout groups in inner hull.
          std::vector<Node> group_nodes;
          group_nodes.reserve(group_list.size());
          for (int i = 0; i < (int)group_list.size(); ++i) {
            const double wgt = std::max(1e-6, group_list[(size_t)i].total_mass);
            group_nodes.push_back(Node{i, wgt});
          }
          const auto group_rects = squarify(std::move(group_nodes), inner);

          // Track hover for component tooltip.
          std::string hovered_comp;
          CompDraw hovered_comp_data;
          RectD hovered_comp_rect;
          bool has_hover = false;

          // Draw groups + components.
          for (const auto& gr : group_rects) {
            if (gr.id < 0 || gr.id >= (int)group_list.size()) continue;
            auto& g = group_list[(size_t)gr.id];

            RectD gb = gr.r;
            // Padding so group outlines don't overlap.
            gb.x += 1.2;
            gb.y += 1.2;
            gb.w = std::max(0.0, gb.w - 2.4);
            gb.h = std::max(0.0, gb.h - 2.4);

            const ImU32 base = color_for_component_type(g.type);
            const ImU32 group_fill = mul_rgb(base, 0.55f, 0.20f);
            const ImU32 group_outline = mul_rgb(base, 0.95f, 0.55f);

            const ImVec2 gp0 = to_screen(Vec2{gb.x, gb.y});
            const ImVec2 gp1 = to_screen(Vec2{gb.x + gb.w, gb.y + gb.h});
            dl->AddRectFilled(gp0, gp1, group_fill, 7.0f);
            dl->AddRect(gp0, gp1, group_outline, 7.0f, 0, 1.25f);

            // Group label.
            if (ui.design_studio_show_labels) {
              const char* tl = component_type_label(g.type);
              const ImVec2 ts = ImGui::CalcTextSize(tl);
              if ((gp1.x - gp0.x) > ts.x + 10.0f && (gp1.y - gp0.y) > ts.y + 10.0f) {
                dl->AddText(ImVec2(gp0.x + 6.0f, gp0.y + 4.0f), IM_COL32(235, 235, 235, 180), tl);
              }
            }

            // Component layout inside group.
            RectD cb = gb;
            cb.x += 4.0;
            cb.y += 18.0;
            cb.w = std::max(0.0, cb.w - 8.0);
            cb.h = std::max(0.0, cb.h - 22.0);
            if (cb.w <= 1.0 || cb.h <= 1.0 || g.comps.empty()) continue;

            // Sort components by mass desc.
            std::vector<int> comp_order(g.comps.size());
            for (int i = 0; i < (int)g.comps.size(); ++i) comp_order[(size_t)i] = i;
            std::sort(comp_order.begin(), comp_order.end(), [&](int a, int b) {
              return g.comps[(size_t)a].mass_tons > g.comps[(size_t)b].mass_tons;
            });

            std::vector<Node> comp_nodes;
            comp_nodes.reserve(g.comps.size());
            for (int i = 0; i < (int)comp_order.size(); ++i) {
              const int idx = comp_order[(size_t)i];
              const double wgt = std::max(1e-6, std::max(0.0, g.comps[(size_t)idx].mass_tons));
              comp_nodes.push_back(Node{idx, wgt});
            }
            const auto comp_rects = squarify(std::move(comp_nodes), cb);

            for (const auto& cr : comp_rects) {
              if (cr.id < 0 || cr.id >= (int)g.comps.size()) continue;
              const auto& c = g.comps[(size_t)cr.id];

              RectD rr = cr.r;
              rr.x += 0.8;
              rr.y += 0.8;
              rr.w = std::max(0.0, rr.w - 1.6);
              rr.h = std::max(0.0, rr.h - 1.6);

              const ImVec2 p0 = to_screen(Vec2{rr.x, rr.y});
              const ImVec2 p1 = to_screen(Vec2{rr.x + rr.w, rr.y + rr.h});

              const bool is_hover = hovered && (io.MousePos.x >= p0.x) && (io.MousePos.x <= p1.x) && (io.MousePos.y >= p0.y) &&
                                    (io.MousePos.y <= p1.y);
              const bool is_sel = (!selected_component_id.empty() && selected_component_id == c.component_id);

              const ImU32 fill = mul_rgb(base, 1.00f, is_sel ? 0.95f : 0.80f);
              const ImU32 border = is_sel ? IM_COL32(255, 255, 255, 220) : IM_COL32(0, 0, 0, 160);
              const ImU32 shadow = IM_COL32(0, 0, 0, 90);

              // Drop shadow for depth.
              dl->AddRectFilled(ImVec2(p0.x + 1.0f, p0.y + 1.0f), ImVec2(p1.x + 1.0f, p1.y + 1.0f), shadow, 4.0f);
              dl->AddRectFilled(p0, p1, fill, 4.0f);
              dl->AddRect(p0, p1, border, 4.0f, 0, is_sel ? 2.0f : 1.0f);

              if (is_hover) {
                dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 160), 4.0f, 0, 2.0f);
                hovered_comp = c.component_id;
                hovered_comp_data = c;
                hovered_comp_rect = rr;
                has_hover = true;
              }

              // Label.
              if (ui.design_studio_show_labels) {
                const float sw = std::max(0.0f, p1.x - p0.x);
                const float sh = std::max(0.0f, p1.y - p0.y);
                if (sw >= 52.0f && sh >= 18.0f) {
                  const int max_chars = std::clamp((int)std::floor(sw / 7.2f), 6, 22);
                  const std::string txt = compact_name(c.name, max_chars);
                  dl->AddText(ImVec2(p0.x + 4.0f, p0.y + 3.0f), IM_COL32(20, 20, 22, 170), txt.c_str());
                  dl->AddText(ImVec2(p0.x + 3.0f, p0.y + 2.0f), IM_COL32(240, 240, 245, 210), txt.c_str());
                }
              }

              // Click to select.
              if (is_hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                selected_component_id = c.component_id;
              }
            }
          }

          // Power overlay strip.
          if (ui.design_studio_show_power_overlay) {
            const float strip_h = 26.0f;
            const ImVec2 sp0(canvas_pos.x + 10.0f, canvas_pos.y + 10.0f);
            const ImVec2 sp1(canvas_p1.x - 10.0f, canvas_pos.y + 10.0f + strip_h);
            draw_power_overlay(dl, sp0, sp1, *design);
          }

          // Tooltip for hovered component.
          if (has_hover) {
            ImGui::BeginTooltip();
            ImGui::Text("%s", hovered_comp_data.name.c_str());
            ImGui::Separator();
            ImGui::Text("Type: %s", component_type_label(hovered_comp_data.type));
            ImGui::Text("Mass: %.0f t", hovered_comp_data.mass_tons);
            if (hovered_comp_data.power_out > 0.0) ImGui::Text("Power out: %.1f", hovered_comp_data.power_out);
            if (hovered_comp_data.power_use > 0.0) ImGui::Text("Power use: %.1f", hovered_comp_data.power_use);
            if (hovered_comp_data.fuel_cap > 0.0) ImGui::Text("Fuel cap: %.0f t", hovered_comp_data.fuel_cap);
            if (hovered_comp_data.cargo_tons > 0.0) ImGui::Text("Cargo: %.0f t", hovered_comp_data.cargo_tons);
            if (hovered_comp_data.mining_tpd > 0.0) ImGui::Text("Mining: %.1f t/day", hovered_comp_data.mining_tpd);
            if (hovered_comp_data.sensor_range > 0.0) ImGui::Text("Sensor: %.0f mkm", hovered_comp_data.sensor_range);
            if (hovered_comp_data.weapon_dmg > 0.0) {
              ImGui::Text("Weapon: %.1f (%.1f mkm)", hovered_comp_data.weapon_dmg, hovered_comp_data.weapon_range);
            }
            if (hovered_comp_data.shield_hp > 0.0) ImGui::Text("Shield: %.0f", hovered_comp_data.shield_hp);
            if (hovered_comp_data.hp_bonus > 0.0) ImGui::Text("HP bonus: %.0f", hovered_comp_data.hp_bonus);
            ImGui::TextDisabled("ID: %s", hovered_comp.c_str());
            ImGui::EndTooltip();
          }

          // Canvas border.
          dl->AddRect(canvas_pos, canvas_p1, IM_COL32(0, 0, 0, 170));

          dl->PopClipRect();

          // Right-click: quick reset.
          if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            user_zoom = 1.0;
            pan = Vec2{0.0, 0.0};
          }

          // Hint.
          if (hovered) {
            ImGui::SetTooltip("Wheel: zoom  |  Middle drag: pan  |  Right click: reset view");
          }
        }

        // ---- Info panel ----
        ImGui::TableSetColumnIndex(1);
        {
          ImGui::BeginChild("##ds_info", ImVec2(0, 0), false);
          ImGui::SeparatorText("Stats");
          ImGui::Text("Mass: %.0f t", design->mass_tons);
          ImGui::Text("Speed: %.1f km/s", design->speed_km_s);
          if (design->fuel_use_per_mkm > 0.0) {
            if (design->fuel_capacity_tons > 0.0) {
              ImGui::Text("Fuel: %.0f t", design->fuel_capacity_tons);
              ImGui::Text("Range: %.0f mkm", design->fuel_capacity_tons / design->fuel_use_per_mkm);
            } else {
              ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Fuel: 0 t (needs tanks)");
            }
          } else if (design->fuel_capacity_tons > 0.0) {
            ImGui::Text("Fuel: %.0f t", design->fuel_capacity_tons);
          } else {
            ImGui::TextDisabled("Fuel: (none)");
          }
          ImGui::Text("Cargo: %.0f t", design->cargo_tons);
          if (design->mining_tons_per_day > 0.0) {
            ImGui::Text("Mining: %.1f t/day", design->mining_tons_per_day);
          } else {
            ImGui::TextDisabled("Mining: (none)");
          }
          ImGui::Text("Sensor: %.0f mkm", design->sensor_range_mkm);
          if (design->weapon_damage > 0.0) {
            ImGui::Text("Weapons: %.1f", design->weapon_damage);
            ImGui::Text("Weapon range: %.1f mkm", design->weapon_range_mkm);
          } else {
            ImGui::TextDisabled("Weapons: (none)");
          }
          ImGui::Text("HP: %.0f", design->max_hp);
          if (design->max_shields > 0.0) {
            ImGui::Text("Shields: %.0f (+%.1f/day)", design->max_shields, design->shield_regen_per_day);
          } else {
            ImGui::TextDisabled("Shields: (none)");
          }

          if (ui.design_studio_show_compare && !compare_id.empty() && compare_id != selected_id) {
            const ShipDesign* cd = sim.find_design(compare_id);
            if (cd) {
              ImGui::SeparatorText("Compare");
              auto delta = [](double a, double b) { return a - b; };
              auto colored_delta = [&](const char* label, double a, double b, const char* fmt = "%.1f") {
                const double d = delta(a, b);
                const ImVec4 good(0.4f, 1.0f, 0.6f, 1.0f);
                const ImVec4 bad(1.0f, 0.4f, 0.4f, 1.0f);
                const ImVec4 neutral(0.8f, 0.8f, 0.8f, 1.0f);
                const ImVec4 col = (std::abs(d) <= 1e-6) ? neutral : (d > 0.0 ? good : bad);
                ImGui::Text("%s", label);
                ImGui::SameLine(140.0f);
                ImGui::Text(fmt, a);
                ImGui::SameLine();
                ImGui::TextDisabled("vs");
                ImGui::SameLine();
                ImGui::Text(fmt, b);
                ImGui::SameLine();
                ImGui::TextColored(col, "%+.2f", d);
              };

              colored_delta("Speed", design->speed_km_s, cd->speed_km_s);
              colored_delta("Mass", design->mass_tons, cd->mass_tons, "%.0f");

              const double range_a = (design->fuel_use_per_mkm > 0.0) ? (design->fuel_capacity_tons / design->fuel_use_per_mkm) : 0.0;
              const double range_b = (cd->fuel_use_per_mkm > 0.0) ? (cd->fuel_capacity_tons / cd->fuel_use_per_mkm) : 0.0;
              colored_delta("Range", range_a, range_b, "%.0f");
              colored_delta("Cargo", design->cargo_tons, cd->cargo_tons, "%.0f");
              colored_delta("Mining", design->mining_tons_per_day, cd->mining_tons_per_day, "%.1f");
              colored_delta("Sensors", design->sensor_range_mkm, cd->sensor_range_mkm, "%.0f");
              colored_delta("Weapons", design->weapon_damage, cd->weapon_damage);
              colored_delta("HP", design->max_hp, cd->max_hp, "%.0f");
              colored_delta("Shields", design->max_shields, cd->max_shields, "%.0f");
            }
          }

          ImGui::SeparatorText("Components");
          ImGui::TextDisabled("%zu total", design->components.size());

          if (!selected_component_id.empty()) {
            const auto it = sim.content().components.find(selected_component_id);
            if (it != sim.content().components.end()) {
              const auto& c = it->second;
              ImGui::SeparatorText("Selected");
              ImGui::Text("%s", c.name.c_str());
              ImGui::TextDisabled("%s", selected_component_id.c_str());
              ImGui::Text("Type: %s", component_type_label(c.type));
              ImGui::Text("Mass: %.0f t", c.mass_tons);
              if (c.power_output > 0.0) ImGui::Text("Power out: %.1f", c.power_output);
              if (c.power_use > 0.0) ImGui::Text("Power use: %.1f", c.power_use);
              if (c.fuel_capacity_tons > 0.0) ImGui::Text("Fuel cap: %.0f t", c.fuel_capacity_tons);
              if (c.cargo_tons > 0.0) ImGui::Text("Cargo: %.0f t", c.cargo_tons);
              if (c.mining_tons_per_day > 0.0) ImGui::Text("Mining: %.1f t/day", c.mining_tons_per_day);
              if (c.sensor_range_mkm > 0.0) ImGui::Text("Sensor: %.0f mkm", c.sensor_range_mkm);
              if (c.weapon_damage > 0.0) ImGui::Text("Weapon: %.1f (%.1f mkm)", c.weapon_damage, c.weapon_range_mkm);
              if (c.shield_hp > 0.0) ImGui::Text("Shield: %.0f (+%.1f/day)", c.shield_hp, c.shield_regen_per_day);
              if (c.hp_bonus > 0.0) ImGui::Text("HP bonus: %.0f", c.hp_bonus);
              ImGui::Spacing();
              if (ImGui::SmallButton("Clear selection")) {
                selected_component_id.clear();
              }
            }
          }

          ImGui::SeparatorText("Ships using this design");
          int ship_count = 0;
          for (const auto& [sid, sh] : sim.state().ships) {
            if (sh.design_id == selected_id) ship_count++;
          }
          if (ship_count == 0) {
            ImGui::TextDisabled("(none)");
          } else {
            ImGui::TextDisabled("%d ships", ship_count);
            if (ImGui::BeginChild("##ds_ship_list", ImVec2(0, 160), true)) {
              for (const auto& [sid, sh] : sim.state().ships) {
                if (sh.design_id != selected_id) continue;
                const auto* sys = find_ptr(sim.state().systems, sh.system_id);
                std::string lbl = sh.name;
                if (sys) {
                  lbl += "  (" + sys->name + ")";
                }
                lbl += "##ds_ship_" + std::to_string(sid);

                if (ImGui::Selectable(lbl.c_str(), selected_ship == sid)) {
                  selected_ship = sid;
                  // Jump context to the ship's system.
                  sim.state().selected_system = sh.system_id;
                  ui.show_map_window = true;
                  ui.request_map_tab = MapTab::System;
                  ui.show_details_window = true;
                  ui.request_details_tab = DetailsTab::Ship;
                }
              }
              ImGui::EndChild();
            }
          }

          ImGui::EndChild();
        }

        ImGui::EndTable();
      }
    }

    ImGui::EndTable();
  }

  ImGui::End();
}

}  // namespace nebula4x::ui
