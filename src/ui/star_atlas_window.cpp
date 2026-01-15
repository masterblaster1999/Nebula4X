#include "ui/star_atlas_window.h"

#include "ui/galaxy_constellations.h"
#include "ui/imgui_includes.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "nebula4x/core/procgen_obscure.h"

namespace nebula4x::ui {
namespace {

bool case_insensitive_contains_sv(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) return true;
  if (haystack.empty()) return false;

  auto tolower_ascii = [](unsigned char c) {
    if (c >= 'A' && c <= 'Z') return static_cast<unsigned char>(c - 'A' + 'a');
    return c;
  };

  for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
    bool ok = true;
    for (std::size_t j = 0; j < needle.size(); ++j) {
      const unsigned char a = tolower_ascii(static_cast<unsigned char>(haystack[i + j]));
      const unsigned char b = tolower_ascii(static_cast<unsigned char>(needle[j]));
      if (a != b) {
        ok = false;
        break;
      }
    }
    if (ok) return true;
  }
  return false;
}

bool case_insensitive_contains(const std::string& haystack, const char* needle_cstr) {
  if (!needle_cstr) return true;
  return case_insensitive_contains_sv(std::string_view(haystack), std::string_view(needle_cstr));
}

// Helper: are we allowed to show the destination system name/links?
bool can_show_system(Id viewer_faction_id, bool fog_of_war, const Simulation& sim, Id system_id) {
  if (!fog_of_war) return true;
  if (viewer_faction_id == kInvalidId) return false;
  return sim.is_system_discovered_by_faction(viewer_faction_id, system_id);
}

std::string system_label(const GameState& st, Id sys_id) {
  if (sys_id == kInvalidId) return "(None)";
  if (const auto* sys = find_ptr(st.systems, sys_id)) {
    if (!sys->name.empty()) return sys->name;
  }
  return "System " + std::to_string((unsigned long long)sys_id);
}

std::string region_label(const GameState& st, Id rid) {
  if (rid == kInvalidId) return "(Unassigned)";
  if (const auto* r = find_ptr(st.regions, rid)) {
    if (!r->name.empty()) return r->name;
  }
  return "Region " + std::to_string((unsigned long long)rid);
}

void focus_system(Id sys_id, Simulation& sim, UIState& ui) {
  if (sys_id == kInvalidId) return;
  const auto* sys = find_ptr(sim.state().systems, sys_id);
  if (!sys) return;

  sim.state().selected_system = sys_id;
  ui.show_map_window = true;
  ui.request_map_tab = MapTab::Galaxy;
  ui.request_galaxy_map_center = true;
  ui.request_galaxy_map_center_x = sys->galaxy_pos.x;
  ui.request_galaxy_map_center_y = sys->galaxy_pos.y;
  ui.request_galaxy_map_center_zoom = std::max(0.6, ui.request_galaxy_map_center_zoom);
}

void focus_constellation(const GalaxyConstellation& c, Simulation& sim, UIState& ui) {
  if (c.systems.empty()) return;

  // Fit bounding box.
  double min_x = 0.0;
  double max_x = 0.0;
  double min_y = 0.0;
  double max_y = 0.0;
  bool init = false;
  for (Id sid : c.systems) {
    const auto* sys = find_ptr(sim.state().systems, sid);
    if (!sys) continue;
    if (!init) {
      init = true;
      min_x = max_x = sys->galaxy_pos.x;
      min_y = max_y = sys->galaxy_pos.y;
    } else {
      min_x = std::min(min_x, sys->galaxy_pos.x);
      max_x = std::max(max_x, sys->galaxy_pos.x);
      min_y = std::min(min_y, sys->galaxy_pos.y);
      max_y = std::max(max_y, sys->galaxy_pos.y);
    }
  }
  if (!init) return;

  const double cx = (min_x + max_x) * 0.5;
  const double cy = (min_y + max_y) * 0.5;
  const double half_span = std::max(1e-6, std::max(max_x - min_x, max_y - min_y) * 0.6);

  ui.show_map_window = true;
  ui.request_map_tab = MapTab::Galaxy;
  ui.request_galaxy_map_center = true;
  ui.request_galaxy_map_center_x = cx;
  ui.request_galaxy_map_center_y = cy;
  ui.request_galaxy_map_fit_half_span = half_span;
}

}  // namespace

void draw_star_atlas_window(Simulation& sim, UIState& ui) {
  if (!ui.show_star_atlas_window) return;

  if (!ImGui::Begin("Star Atlas", &ui.show_star_atlas_window)) {
    ImGui::End();
    return;
  }

  auto& st = sim.state();
  const Id viewer_faction_id = ui.viewer_faction_id;

  // Visible systems (respect discovery under FoW).
  std::vector<Id> visible_ids;
  visible_ids.reserve(st.systems.size());
  for (const auto& [id, _] : st.systems) {
    if (ui.fog_of_war && !can_show_system(viewer_faction_id, ui.fog_of_war, sim, id)) continue;
    visible_ids.push_back(id);
  }

  GalaxyConstellationParams params;
  params.target_cluster_size = ui.galaxy_star_atlas_target_cluster_size;
  params.max_constellations = ui.galaxy_star_atlas_max_constellations;

  // Cache (per viewer settings + visible set).
  struct Cache {
    std::uint64_t key{0};
    std::vector<GalaxyConstellation> constellations;
  };
  static Cache cache;

  auto make_key = [&]() {
    std::uint64_t h = 0x7A1E5F3D2C9B4D11ULL;
    h ^= (std::uint64_t)params.target_cluster_size * 0x9e3779b97f4a7c15ULL;
    h ^= (std::uint64_t)params.max_constellations * 0xbf58476d1ce4e5b9ULL;
    h ^= (std::uint64_t)ui.fog_of_war * 0x94d049bb133111ebULL;
    // Order-independent id hash.
    std::uint64_t acc = 0;
    for (Id id : visible_ids) {
      acc ^= nebula4x::procgen_obscure::splitmix64((std::uint64_t)id * 0xA24BAED4963EE407ULL);
    }
    h ^= acc;
    return nebula4x::procgen_obscure::splitmix64(h);
  };

  const std::uint64_t key = make_key();
  if (key != cache.key) {
    cache.key = key;
    cache.constellations = build_galaxy_constellations(st, visible_ids, params);
  }

  // Controls: toggle overlay settings.
  ImGui::TextDisabled("Map overlay");
  ImGui::Checkbox("Show constellations on Galaxy Map", &ui.galaxy_star_atlas_constellations);
  ImGui::SameLine();
  ImGui::Checkbox("Labels", &ui.galaxy_star_atlas_labels);
  ImGui::SliderFloat("Line alpha", &ui.galaxy_star_atlas_alpha, 0.0f, 1.0f, "%.2f");
  ImGui::SliderFloat("Label alpha", &ui.galaxy_star_atlas_label_alpha, 0.0f, 1.0f, "%.2f");
  ImGui::SliderInt("Target cluster size", &ui.galaxy_star_atlas_target_cluster_size, 4, 18);
  ImGui::SliderInt("Max constellations", &ui.galaxy_star_atlas_max_constellations, 0, 512);
  ImGui::SliderFloat("Min zoom (map)", &ui.galaxy_star_atlas_min_zoom, 0.05f, 2.0f, "%.2f");
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Hide constellations when the Galaxy Map is very zoomed out to prevent clutter.");
  }

  ImGui::Separator();

  static char search_buf[128] = "";
  ImGui::InputTextWithHint("Search", "Filter constellations...", search_buf, IM_ARRAYSIZE(search_buf));

  ImGui::TextDisabled("Visible constellations: %d", (int)cache.constellations.size());
  ImGui::SameLine();
  if (ImGui::SmallButton("Recenter Galaxy Map")) {
    if (!cache.constellations.empty()) focus_constellation(cache.constellations.front(), sim, ui);
  }

  if (cache.constellations.empty()) {
    ImGui::TextDisabled("(no constellations: discover more systems or increase Max constellations)");
    ImGui::End();
    return;
  }

  // List.
  for (const auto& c : cache.constellations) {
    const std::string header = c.name + "  [" + c.code + "]  (" + region_label(st, c.region_id) + ", " +
                               std::to_string((int)c.systems.size()) + " systems)";
    if (!case_insensitive_contains(header, search_buf)) continue;

    if (ImGui::TreeNode((header + "##const_" + std::to_string((unsigned long long)c.id)).c_str())) {
      if (ImGui::SmallButton("Focus")) {
        focus_constellation(c, sim, ui);
      }

      if (!c.glyph.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy glyph")) {
          ImGui::SetClipboardText(c.glyph.c_str());
        }
        ImGui::TextUnformatted(c.glyph.c_str());
      }

      if (ImGui::BeginTable(("##const_tbl_" + std::to_string((unsigned long long)c.id)).c_str(), 2,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("System", ImGuiTableColumnFlags_WidthStretch, 0.75f);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 0.25f);
        ImGui::TableHeadersRow();

        for (Id sid : c.systems) {
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          const std::string nm = system_label(st, sid);
          ImGui::TextUnformatted(nm.c_str());
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("System id: %llu", (unsigned long long)sid);
          }

          ImGui::TableSetColumnIndex(1);
          if (ImGui::SmallButton(("View##view_sys_" + std::to_string((unsigned long long)c.id) + "_" +
                                  std::to_string((unsigned long long)sid))
                                     .c_str())) {
            focus_system(sid, sim, ui);
          }
        }

        ImGui::EndTable();
      }

      ImGui::TreePop();
    }
  }

  ImGui::End();
}

}  // namespace nebula4x::ui
