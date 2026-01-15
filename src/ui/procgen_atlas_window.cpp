#include "ui/procgen_atlas_window.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nebula4x/core/enum_strings.h"
#include "nebula4x/core/simulation.h"
#include "nebula4x/util/file_io.h"
#include "nebula4x/util/json.h"

#include "ui/procgen_metrics.h"
#include "ui/ui_state.h"

namespace nebula4x::ui {

namespace {

struct GalaxySummary {
  int systems{0};
  int bodies{0};
  int stars{0};
  int planets{0};
  int moons{0};
  int gas_giants{0};
  int asteroids{0};
  int habitable{0};
  int jump_points{0};

  double nebula_sum{0.0};
  double minerals_sum{0.0};

  std::array<int, 7> spectral_counts{}; // O,B,A,F,G,K,M
};

int spectral_bucket_index(double temp_k) {
  if (!std::isfinite(temp_k) || temp_k <= 0.0) return -1;
  if (temp_k >= 30000.0) return 0;  // O
  if (temp_k >= 10000.0) return 1;  // B
  if (temp_k >= 7500.0) return 2;   // A
  if (temp_k >= 6000.0) return 3;   // F
  if (temp_k >= 5200.0) return 4;   // G
  if (temp_k >= 3900.0) return 5;   // K
  return 6;                         // M
}

const char* spectral_bucket_label(int idx) {
  static const char* kLabels[7] = {"O", "B", "A", "F", "G", "K", "M"};
  if (idx < 0 || idx >= 7) return "?";
  return kLabels[idx];
}

GalaxySummary analyze_galaxy(const GameState& s) {
  GalaxySummary g;
  g.systems = static_cast<int>(s.systems.size());

  for (const auto& [sys_id, sys] : s.systems) {
    (void)sys_id;
    g.nebula_sum += std::clamp(sys.nebula_density, 0.0, 1.0);
    g.jump_points += static_cast<int>(sys.jump_points.size());

    const Body* primary_star = nullptr;

    for (Id body_id : sys.bodies) {
      const Body* b = find_ptr(s.bodies, body_id);
      if (!b) continue;
      g.bodies++;

      switch (b->type) {
        case BodyType::Star:
          g.stars++;
          if (!primary_star) primary_star = b;
          break;
        case BodyType::Planet:
          g.planets++;
          break;
        case BodyType::Moon:
          g.moons++;
          break;
        case BodyType::GasGiant:
          g.gas_giants++;
          break;
        case BodyType::Asteroid:
          g.asteroids++;
          break;
        default:
          break;
      }

      g.minerals_sum += body_mineral_total(*b);
    }

    if (primary_star) {
      const int idx = spectral_bucket_index(primary_star->surface_temp_k);
      if (idx >= 0) g.spectral_counts[static_cast<std::size_t>(idx)]++;
    }
  }

  // Habitable candidates are computed at system level using helper.
  for (const auto& [sys_id, sys] : s.systems) {
    (void)sys_id;
    g.habitable += count_habitable_candidates(s, sys);
  }

  return g;
}

struct SysRow {
  Id id{kInvalidId};
  const StarSystem* sys{nullptr};

  std::string name;
  std::string region;

  double dist{0.0};
  double nebula{0.0};
  int jump_degree{0};
  int bodies{0};
  int planets{0};
  int habitable{0};
  double minerals{0.0};

  double star_temp{0.0};
  double star_mass{0.0};
  double star_lum{0.0};
};

struct BodyRow {
  Id id{kInvalidId};
  const Body* body{nullptr};
  std::string name;
  std::string type;
  std::string parent;
  double orbit_mkm{0.0};
  double temp_k{0.0};
  double atm{0.0};
  double minerals{0.0};
};

std::string region_label(const GameState& s, const StarSystem& sys) {
  if (sys.region_id == kInvalidId) return "-";
  const Region* r = find_ptr(s.regions, sys.region_id);
  if (!r) return "?";
  if (!r->theme.empty()) return r->theme;
  if (!r->name.empty()) return r->name;
  return "(region)";
}

double distance_to_ref(const StarSystem& sys, const StarSystem* ref) {
  if (!ref) return 0.0;
  return (sys.galaxy_pos - ref->galaxy_pos).length();
}

// Simple orbit chart: places each body on its orbit circle at a deterministic
// pseudo-angle derived from its id so plots remain stable frame-to-frame.
void draw_orbit_chart(const GameState& s, const StarSystem& sys, const Id selected_body) {
  ImGui::SeparatorText("Orbit chart");
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const float h = std::min(260.0f, std::max(150.0f, avail.x * 0.52f));
  ImGui::BeginChild("procgen_orbits", ImVec2(0, h), true);

  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const ImVec2 sz = ImGui::GetContentRegionAvail();
  const ImVec2 center(p0.x + sz.x * 0.5f, p0.y + sz.y * 0.5f);
  const float radius_px = std::max(10.0f, std::min(sz.x, sz.y) * 0.45f);

  double max_orbit = 0.0;
  for (Id bid : sys.bodies) {
    const Body* b = find_ptr(s.bodies, bid);
    if (!b) continue;
    max_orbit = std::max(max_orbit, std::max(0.0, b->orbit_radius_mkm));
  }
  const double max_r_log = std::log10(max_orbit + 1.0);

  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p0, ImVec2(p0.x + sz.x, p0.y + sz.y), IM_COL32(10, 10, 12, 160));

  // Draw a few reference rings.
  for (int i = 1; i <= 4; ++i) {
    const float r = radius_px * (static_cast<float>(i) / 4.0f);
    dl->AddCircle(center, r, IM_COL32(255, 255, 255, 30), 0, 1.0f);
  }

  auto body_color = [](BodyType t) {
    switch (t) {
      case BodyType::Star:
        return IM_COL32(255, 220, 140, 230);
      case BodyType::GasGiant:
        return IM_COL32(120, 200, 255, 230);
      case BodyType::Planet:
        return IM_COL32(160, 255, 160, 220);
      case BodyType::Moon:
        return IM_COL32(210, 210, 220, 210);
      case BodyType::Asteroid:
        return IM_COL32(190, 170, 150, 210);
      default:
        return IM_COL32(230, 230, 230, 200);
    }
  };

  // Plot bodies.
  Id hovered = kInvalidId;
  float hovered_dist2 = std::numeric_limits<float>::infinity();
  const ImVec2 mouse = ImGui::GetIO().MousePos;

  for (Id bid : sys.bodies) {
    const Body* b = find_ptr(s.bodies, bid);
    if (!b) continue;

    // Log-scale radius (compress very wide systems).
    double r_log = (max_r_log > 1e-9) ? (std::log10(std::max(0.0, b->orbit_radius_mkm) + 1.0) / max_r_log) : 0.0;
    r_log = std::clamp(r_log, 0.0, 1.0);
    const float r = static_cast<float>(r_log) * radius_px;

    // Deterministic pseudo angle from id.
    const std::uint32_t h = static_cast<std::uint32_t>(bid) * 2654435761u;
    const float a = (static_cast<float>(h % 10000u) / 10000.0f) * 6.2831853f;
    const ImVec2 p(center.x + std::cos(a) * r, center.y + std::sin(a) * r);

    const bool is_sel = (bid == selected_body);
    const float pr = is_sel ? 5.0f : 3.2f;
    dl->AddCircleFilled(p, pr, body_color(b->type), 0);
    if (is_sel) {
      dl->AddCircle(p, pr + 3.0f, IM_COL32(255, 255, 255, 200), 0, 2.0f);
    }

    const float dx = mouse.x - p.x;
    const float dy = mouse.y - p.y;
    const float d2 = dx * dx + dy * dy;
    if (d2 < (pr + 6.0f) * (pr + 6.0f) && d2 < hovered_dist2) {
      hovered = bid;
      hovered_dist2 = d2;
    }
  }

  // Star marker at center.
  dl->AddCircleFilled(center, 4.0f, IM_COL32(255, 255, 255, 200), 0);

  // Tooltip for hovered.
  if (hovered != kInvalidId) {
    if (const Body* b = find_ptr(s.bodies, hovered)) {
      ImGui::BeginTooltip();
      ImGui::Text("%s", b->name.c_str());
      ImGui::TextDisabled("%s", body_type_to_string(b->type).c_str());
      if (b->orbit_radius_mkm > 0.0) {
        ImGui::TextDisabled("Orbit: %.0f mkm", b->orbit_radius_mkm);
      }
      if (b->surface_temp_k > 0.0) {
        ImGui::TextDisabled("Temp: %.0f K", b->surface_temp_k);
      }
      if (b->atmosphere_atm > 0.0) {
        ImGui::TextDisabled("Atm: %.2f atm", b->atmosphere_atm);
      }
      const double minerals = body_mineral_total(*b);
      if (minerals > 0.0) {
        ImGui::TextDisabled("Minerals: %.0f", minerals);
      }
      ImGui::EndTooltip();
    }
  }

  // Reserve draw area.
  ImGui::Dummy(sz);
  ImGui::EndChild();
}

} // namespace

void draw_procgen_atlas_window(Simulation& sim, UIState& ui, Id& selected_body) {
  if (!ui.show_procgen_atlas_window) return;

  if (!ImGui::Begin("ProcGen Atlas", &ui.show_procgen_atlas_window, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return;
  }

  GameState& s = sim.state();
  const StarSystem* selected_sys = find_ptr(s.systems, s.selected_system);

  ImGui::TextDisabled("In-game procedural generation dossier (current save)");
  ImGui::Separator();

  if (s.systems.empty()) {
    ImGui::TextDisabled("No systems loaded.");
    ImGui::End();
    return;
  }

  static char filter_name[64] = {0};
  static int min_habitable = 0;
  static double min_nebula = 0.0;

  const GalaxySummary summary = analyze_galaxy(s);

  if (ImGui::BeginTabBar("procgen_tabs")) {
    if (ImGui::BeginTabItem("Overview")) {
      ImGui::Columns(2, "pg_overview", false);

      ImGui::Text("Galaxy summary");
      ImGui::Separator();
      ImGui::BulletText("Systems: %d", summary.systems);
      ImGui::BulletText("Bodies: %d", summary.bodies);
      ImGui::BulletText("Stars: %d", summary.stars);
      ImGui::BulletText("Planets: %d", summary.planets);
      ImGui::BulletText("Moons: %d", summary.moons);
      ImGui::BulletText("Gas giants: %d", summary.gas_giants);
      ImGui::BulletText("Asteroids: %d", summary.asteroids);
      ImGui::BulletText("Habitable candidates: %d", summary.habitable);
      ImGui::BulletText("Jump points: %d", summary.jump_points);
      if (summary.systems > 0) {
        ImGui::BulletText("Avg nebula density: %.2f", summary.nebula_sum / summary.systems);
      }
      ImGui::BulletText("Total minerals: %.0f", summary.minerals_sum);

      ImGui::NextColumn();
      ImGui::Text("Primary star spectral buckets");
      ImGui::Separator();
      const int max_count = *std::max_element(summary.spectral_counts.begin(), summary.spectral_counts.end());
      for (int i = 0; i < 7; ++i) {
        const int c = summary.spectral_counts[static_cast<std::size_t>(i)];
        const float frac = (max_count > 0) ? (static_cast<float>(c) / static_cast<float>(max_count)) : 0.0f;
        ImGui::Text("%s", spectral_bucket_label(i));
        ImGui::SameLine();
        ImGui::ProgressBar(frac, ImVec2(-1, 0), (std::to_string(c)).c_str());
      }

      ImGui::Columns(1);

      ImGui::Spacing();
      ImGui::SeparatorText("ProcGen lens quick toggle");
      {
        int mode = static_cast<int>(ui.galaxy_procgen_lens_mode);
        ImGui::Combo("Galaxy map lens", &mode, procgen_lens_mode_combo_items());
        ui.galaxy_procgen_lens_mode = static_cast<ProcGenLensMode>(mode);
        ImGui::SameLine();
        if (ImGui::SmallButton("Center on selected system") && selected_sys) {
          ui.request_map_tab = MapTab::Galaxy;
          ui.request_galaxy_map_center = true;
          ui.request_galaxy_map_center_x = selected_sys->galaxy_pos.x;
          ui.request_galaxy_map_center_y = selected_sys->galaxy_pos.y;
        }
      }
      ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Systems")) {
      // Build rows.
      std::vector<SysRow> rows;
      rows.reserve(s.systems.size());

      for (const auto& [id, sys] : s.systems) {
        SysRow r;
        r.id = id;
        r.sys = &sys;
        r.name = sys.name;
        r.region = region_label(s, sys);
        r.nebula = sys.nebula_density;
        r.jump_degree = static_cast<int>(sys.jump_points.size());
        r.bodies = static_cast<int>(sys.bodies.size());
        r.planets = count_planets(s, sys);
        r.habitable = count_habitable_candidates(s, sys);
        r.minerals = system_mineral_total(s, sys);
        if (selected_sys) {
          r.dist = distance_to_ref(sys, selected_sys);
        }

        r.star_temp = primary_star_temperature(s, sys);
        r.star_mass = primary_star_mass_solar(s, sys);
        r.star_lum = primary_star_luminosity_solar(s, sys);
        rows.push_back(std::move(r));
      }

      ImGui::InputTextWithHint("Filter", "name contains...", filter_name, sizeof(filter_name));
      ImGui::SameLine();
      ImGui::SetNextItemWidth(120);
      ImGui::SliderInt("Min habitable", &min_habitable, 0, 10);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(160);
      static const double kNebulaMin = 0.0;
      static const double kNebulaMax = 1.0;
      ImGui::SliderScalar("Min nebula", ImGuiDataType_Double, &min_nebula, &kNebulaMin, &kNebulaMax, "%.2f");

      const ImGuiTableFlags flags = ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuterH |
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable |
                                  ImGuiTableFlags_Reorderable;

      if (ImGui::BeginTable("procgen_systems_table", 11, flags, ImVec2(0.0f, 420.0f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort, 200.0f, 0);
        ImGui::TableSetupColumn("Region", ImGuiTableColumnFlags_None, 160.0f, 1);
        ImGui::TableSetupColumn("Dist", ImGuiTableColumnFlags_PreferSortDescending, 60.0f, 2);
        ImGui::TableSetupColumn("Nebula", ImGuiTableColumnFlags_PreferSortDescending, 60.0f, 3);
        ImGui::TableSetupColumn("Jumps", ImGuiTableColumnFlags_PreferSortDescending, 50.0f, 4);
        ImGui::TableSetupColumn("Bodies", ImGuiTableColumnFlags_PreferSortDescending, 55.0f, 5);
        ImGui::TableSetupColumn("Planets", ImGuiTableColumnFlags_PreferSortDescending, 55.0f, 6);
        ImGui::TableSetupColumn("Hab", ImGuiTableColumnFlags_PreferSortDescending, 45.0f, 7);
        ImGui::TableSetupColumn("Minerals", ImGuiTableColumnFlags_PreferSortDescending, 70.0f, 8);
        ImGui::TableSetupColumn("T★ (K)", ImGuiTableColumnFlags_PreferSortDescending, 60.0f, 9);
        ImGui::TableSetupColumn("M★ (M☉)", ImGuiTableColumnFlags_PreferSortDescending, 70.0f, 10);
        ImGui::TableHeadersRow();

        if (ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs()) {
          if (sort->SpecsCount > 0 && sort->SpecsDirty) {
            const ImGuiTableColumnSortSpecs s0 = sort->Specs[0];
            const int col = s0.ColumnUserID;
            const bool asc = (s0.SortDirection == ImGuiSortDirection_Ascending);

            auto cmp = [&](const SysRow& a, const SysRow& b) {
              auto less = [&](auto va, auto vb) { return asc ? (va < vb) : (va > vb); };
              switch (col) {
                case 0:
                  return asc ? (a.name < b.name) : (a.name > b.name);
                case 1:
                  return asc ? (a.region < b.region) : (a.region > b.region);
                case 2:
                  return less(a.dist, b.dist);
                case 3:
                  return less(a.nebula, b.nebula);
                case 4:
                  return less(a.jump_degree, b.jump_degree);
                case 5:
                  return less(a.bodies, b.bodies);
                case 6:
                  return less(a.planets, b.planets);
                case 7:
                  return less(a.habitable, b.habitable);
                case 8:
                  return less(a.minerals, b.minerals);
                case 9:
                  return less(a.star_temp, b.star_temp);
                case 10:
                  return less(a.star_mass, b.star_mass);
                default:
                  return asc ? (a.name < b.name) : (a.name > b.name);
              }
            };

            std::sort(rows.begin(), rows.end(), cmp);
            sort->SpecsDirty = false;
          }
        }

        // Filter indices (after sorting).
        std::vector<int> idx;
        idx.reserve(rows.size());
        const std::string_view needle(filter_name);
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
          const SysRow& r = rows[static_cast<std::size_t>(i)];
          if (!needle.empty()) {
            if (r.name.find(needle) == std::string::npos) continue;
          }
          if (r.habitable < min_habitable) continue;
          if (r.nebula < min_nebula) continue;
          idx.push_back(i);
        }

        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(idx.size()));
        while (clip.Step()) {
          for (int row_n = clip.DisplayStart; row_n < clip.DisplayEnd; ++row_n) {
            const SysRow& r = rows[static_cast<std::size_t>(idx[static_cast<std::size_t>(row_n)])];
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            const bool is_sel = (r.id == s.selected_system);
            if (ImGui::Selectable(r.name.c_str(), is_sel, ImGuiSelectableFlags_SpanAllColumns)) {
              s.selected_system = r.id;
              selected_sys = r.sys;
              // Helpful default: center galaxy map to selected system.
              if (selected_sys) {
                ui.request_map_tab = MapTab::Galaxy;
                ui.request_galaxy_map_center = true;
                ui.request_galaxy_map_center_x = selected_sys->galaxy_pos.x;
                ui.request_galaxy_map_center_y = selected_sys->galaxy_pos.y;
              }
            }

            if (ImGui::BeginPopupContextItem()) {
              if (ImGui::MenuItem("Center Galaxy Map")) {
                if (r.sys) {
                  ui.request_map_tab = MapTab::Galaxy;
                  ui.request_galaxy_map_center = true;
                  ui.request_galaxy_map_center_x = r.sys->galaxy_pos.x;
                  ui.request_galaxy_map_center_y = r.sys->galaxy_pos.y;
                }
              }
              if (ImGui::MenuItem("Open System Map")) {
                ui.request_map_tab = MapTab::System;
                ui.request_system_map_center = true;
                ui.request_system_map_center_x_mkm = 0.0;
                ui.request_system_map_center_y_mkm = 0.0;
              }
              ImGui::EndPopup();
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(r.region.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.2f", r.dist);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.2f", r.nebula);
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%d", r.jump_degree);
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%d", r.bodies);
            ImGui::TableSetColumnIndex(6);
            ImGui::Text("%d", r.planets);
            ImGui::TableSetColumnIndex(7);
            ImGui::Text("%d", r.habitable);
            ImGui::TableSetColumnIndex(8);
            ImGui::Text("%.0f", r.minerals);
            ImGui::TableSetColumnIndex(9);
            ImGui::Text("%.0f", r.star_temp);
            ImGui::TableSetColumnIndex(10);
            ImGui::Text("%.2f", r.star_mass);
          }
        }

        ImGui::EndTable();
      }

      ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Bodies")) {
      if (!selected_sys) {
        ImGui::TextDisabled("No system selected.");
        ImGui::EndTabItem();
      } else {
        ImGui::Text("%s", selected_sys->name.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Center system map")) {
          ui.request_map_tab = MapTab::System;
          ui.request_system_map_center = true;
          ui.request_system_map_center_x_mkm = 0.0;
          ui.request_system_map_center_y_mkm = 0.0;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Center galaxy map")) {
          ui.request_map_tab = MapTab::Galaxy;
          ui.request_galaxy_map_center = true;
          ui.request_galaxy_map_center_x = selected_sys->galaxy_pos.x;
          ui.request_galaxy_map_center_y = selected_sys->galaxy_pos.y;
        }

        draw_orbit_chart(s, *selected_sys, selected_body);

        // Body table.
        std::vector<BodyRow> bodies;
        bodies.reserve(selected_sys->bodies.size());
        for (Id bid : selected_sys->bodies) {
          const Body* b = find_ptr(s.bodies, bid);
          if (!b) continue;

          BodyRow r;
          r.id = bid;
          r.body = b;
          r.name = b->name;
          r.type = body_type_to_string(b->type);
          r.parent = "-";
          if (b->parent_body_id != kInvalidId) {
            if (const Body* p = find_ptr(s.bodies, b->parent_body_id)) {
              r.parent = p->name;
            }
          }
          r.orbit_mkm = b->orbit_radius_mkm;
          r.temp_k = b->surface_temp_k;
          r.atm = b->atmosphere_atm;
          r.minerals = body_mineral_total(*b);
          bodies.push_back(std::move(r));
        }

        const ImGuiTableFlags flags = ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuterH |
                                    ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                    ImGuiTableFlags_ScrollY;
        if (ImGui::BeginTable("procgen_bodies", 8, flags, ImVec2(0.0f, 420.0f))) {
          ImGui::TableSetupScrollFreeze(0, 1);
          ImGui::TableSetupColumn("Name", 0, 220.0f);
          ImGui::TableSetupColumn("Type", 0, 90.0f);
          ImGui::TableSetupColumn("Parent", 0, 140.0f);
          ImGui::TableSetupColumn("Orbit (mkm)", 0, 90.0f);
          ImGui::TableSetupColumn("Temp (K)", 0, 75.0f);
          ImGui::TableSetupColumn("Atm", 0, 55.0f);
          ImGui::TableSetupColumn("Minerals", 0, 70.0f);
          ImGui::TableSetupColumn("Hab?", 0, 45.0f);
          ImGui::TableHeadersRow();

          ImGuiListClipper clip;
          clip.Begin(static_cast<int>(bodies.size()));
          while (clip.Step()) {
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
              const BodyRow& r = bodies[static_cast<std::size_t>(i)];
              const bool is_sel = (r.id == selected_body);

              ImGui::TableNextRow();
              ImGui::TableSetColumnIndex(0);
              if (ImGui::Selectable(r.name.c_str(), is_sel, ImGuiSelectableFlags_SpanAllColumns)) {
                selected_body = r.id;
                ui.request_details_tab = DetailsTab::Body;
              }
              ImGui::TableSetColumnIndex(1);
              ImGui::TextUnformatted(r.type.c_str());
              ImGui::TableSetColumnIndex(2);
              ImGui::TextUnformatted(r.parent.c_str());
              ImGui::TableSetColumnIndex(3);
              ImGui::Text("%.0f", r.orbit_mkm);
              ImGui::TableSetColumnIndex(4);
              ImGui::Text("%.0f", r.temp_k);
              ImGui::TableSetColumnIndex(5);
              ImGui::Text("%.2f", r.atm);
              ImGui::TableSetColumnIndex(6);
              ImGui::Text("%.0f", r.minerals);
              ImGui::TableSetColumnIndex(7);
              const bool hab = is_habitable_candidate(r.temp_k, r.atm, r.body ? r.body->type : BodyType::Planet);
              ImGui::TextUnformatted(hab ? "✓" : "");
            }
          }

          ImGui::EndTable();
        }
        ImGui::EndTabItem();
      }
    }

    if (ImGui::BeginTabItem("Export")) {
      static char out_path[256] = "procgen_report.json";
      ImGui::InputText("Path", out_path, sizeof(out_path));
      ImGui::SameLine();

      if (ImGui::Button("Export JSON report")) {
        json::Object root;
        root["systems"] = summary.systems;
        root["bodies"] = summary.bodies;
        root["stars"] = summary.stars;
        root["planets"] = summary.planets;
        root["moons"] = summary.moons;
        root["gas_giants"] = summary.gas_giants;
        root["asteroids"] = summary.asteroids;
        root["habitable_candidates"] = summary.habitable;
        root["jump_points"] = summary.jump_points;
        root["avg_nebula_density"] = (summary.systems > 0) ? (summary.nebula_sum / summary.systems) : 0.0;
        root["total_minerals"] = summary.minerals_sum;

        json::Array sys_arr;
        sys_arr.reserve(s.systems.size());
        for (const auto& [id, sys] : s.systems) {
          json::Object o;
          o["id"] = static_cast<std::int64_t>(id);
          o["name"] = sys.name;
          o["region_id"] = static_cast<std::int64_t>(sys.region_id);
          o["region"] = region_label(s, sys);
          o["galaxy_x"] = sys.galaxy_pos.x;
          o["galaxy_y"] = sys.galaxy_pos.y;
          o["nebula_density"] = sys.nebula_density;
          o["jump_degree"] = static_cast<int>(sys.jump_points.size());
          o["body_count"] = static_cast<int>(sys.bodies.size());
          o["planet_count"] = count_planets(s, sys);
          o["habitable_candidates"] = count_habitable_candidates(s, sys);
          o["mineral_total"] = system_mineral_total(s, sys);
          o["primary_star_temp_k"] = primary_star_temperature(s, sys);
          o["primary_star_mass_solar"] = primary_star_mass_solar(s, sys);
          o["primary_star_luminosity_solar"] = primary_star_luminosity_solar(s, sys);
          sys_arr.push_back(std::move(o));
        }
        root["systems_list"] = std::move(sys_arr);

        const std::string txt = json::stringify(root, /*pretty=*/true);
        if (!write_text_file(out_path, txt)) {
          ImGui::OpenPopup("procgen_export_failed");
        }
      }

      if (ImGui::BeginPopupModal("procgen_export_failed", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Failed to write report.");
        if (ImGui::Button("OK")) {
          ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
      }

      ImGui::SeparatorText("Clipboard");
      if (ImGui::Button("Copy galaxy summary")) {
        std::string text;
        text.reserve(256);
        text += "ProcGen Atlas summary\n";
        text += "Systems: " + std::to_string(summary.systems) + "\n";
        text += "Bodies: " + std::to_string(summary.bodies) + "\n";
        text += "Habitable candidates: " + std::to_string(summary.habitable) + "\n";
        text += "Total minerals: " + std::to_string(static_cast<std::int64_t>(summary.minerals_sum)) + "\n";
        ImGui::SetClipboardText(text.c_str());
      }

      ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
  }

  ImGui::End();
}

} // namespace nebula4x::ui
