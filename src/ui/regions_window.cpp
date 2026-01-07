#include "ui/regions_window.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>

#include "nebula4x/util/strings.h"

namespace nebula4x::ui {

namespace {

bool case_insensitive_contains(const std::string& haystack, const char* needle_cstr) {
  if (!needle_cstr) return true;
  const std::string needle(needle_cstr);
  if (needle.empty()) return true;
  const std::string h = nebula4x::to_lower(haystack);
  const std::string n = nebula4x::to_lower(needle);
  return h.find(n) != std::string::npos;
}

ImU32 region_col(Id rid, float alpha) {
  if (rid == kInvalidId) return 0;
  const float h = std::fmod(static_cast<float>((static_cast<std::uint32_t>(rid) * 0.61803398875f)), 1.0f);
  const ImVec4 c = ImColor::HSV(h, 0.55f, 0.95f, alpha);
  return ImGui::ColorConvertFloat4ToU32(c);
}

struct RegionAgg {
  bool init_bounds{false};
  double min_x{0.0}, max_x{0.0}, min_y{0.0}, max_y{0.0};

  int systems_total{0};
  int systems_visible{0};
  Vec2 sum_pos{0.0, 0.0};
  int sum_count{0};

  int colonies{0};
  double pop_millions{0.0};
};

struct RegionRow {
  Id id{kInvalidId};
  const Region* reg{nullptr};

  // Aggregates.
  RegionAgg a{};

  // Derived.
  Vec2 centroid{0.0, 0.0};
  double half_span{0.0};
};

RegionRow build_row(Id rid, const Region& r, const RegionAgg& a) {
  RegionRow row;
  row.id = rid;
  row.reg = &r;
  row.a = a;

  if (a.sum_count > 0) {
    row.centroid = Vec2{a.sum_pos.x / static_cast<double>(a.sum_count), a.sum_pos.y / static_cast<double>(a.sum_count)};
  } else {
    row.centroid = r.center;
  }

  if (a.init_bounds) {
    const double span_x = a.max_x - a.min_x;
    const double span_y = a.max_y - a.min_y;
    row.half_span = 0.5 * std::max(span_x, span_y);
  } else {
    row.half_span = 0.0;
  }
  return row;
}

int compare_rows(const RegionRow& a, const RegionRow& b, const ImGuiTableSortSpecs* sort_specs) {
  if (!sort_specs || sort_specs->SpecsCount == 0) {
    // Default: name ascending.
    const std::string an = a.reg ? a.reg->name : std::string();
    const std::string bn = b.reg ? b.reg->name : std::string();
    if (an < bn) return -1;
    if (an > bn) return 1;
    return 0;
  }

  for (int n = 0; n < sort_specs->SpecsCount; ++n) {
    const ImGuiTableColumnSortSpecs& spec = sort_specs->Specs[n];
    int delta = 0;
    switch (spec.ColumnIndex) {
      case 0: { // Name
        const std::string an = a.reg ? a.reg->name : std::string();
        const std::string bn = b.reg ? b.reg->name : std::string();
        if (an < bn) delta = -1;
        else if (an > bn) delta = 1;
        else delta = 0;
        break;
      }
      case 1: { // Theme
        const std::string an = a.reg ? a.reg->theme : std::string();
        const std::string bn = b.reg ? b.reg->theme : std::string();
        if (an < bn) delta = -1;
        else if (an > bn) delta = 1;
        else delta = 0;
        break;
      }
      case 2: { // Systems
        delta = a.a.systems_total - b.a.systems_total;
        break;
      }
      case 3: { // Visible
        delta = a.a.systems_visible - b.a.systems_visible;
        break;
      }
      case 4: { // Colonies
        delta = a.a.colonies - b.a.colonies;
        break;
      }
      case 5: { // Pop
        if (a.a.pop_millions < b.a.pop_millions) delta = -1;
        else if (a.a.pop_millions > b.a.pop_millions) delta = 1;
        else delta = 0;
        break;
      }
      case 6: { // Nebula
        const double av = a.reg ? a.reg->nebula_bias : 0.0;
        const double bv = b.reg ? b.reg->nebula_bias : 0.0;
        if (av < bv) delta = -1;
        else if (av > bv) delta = 1;
        else delta = 0;
        break;
      }
      case 7: { // Pirates
        const double av = a.reg ? a.reg->pirate_risk : 0.0;
        const double bv = b.reg ? b.reg->pirate_risk : 0.0;
        if (av < bv) delta = -1;
        else if (av > bv) delta = 1;
        else delta = 0;
        break;
      }
      case 8: { // Ruins
        const double av = a.reg ? a.reg->ruins_density : 0.0;
        const double bv = b.reg ? b.reg->ruins_density : 0.0;
        if (av < bv) delta = -1;
        else if (av > bv) delta = 1;
        else delta = 0;
        break;
      }
      case 9: { // Minerals
        const double av = a.reg ? a.reg->mineral_richness_mult : 1.0;
        const double bv = b.reg ? b.reg->mineral_richness_mult : 1.0;
        if (av < bv) delta = -1;
        else if (av > bv) delta = 1;
        else delta = 0;
        break;
      }
      case 10: { // Volatiles
        const double av = a.reg ? a.reg->volatile_richness_mult : 1.0;
        const double bv = b.reg ? b.reg->volatile_richness_mult : 1.0;
        if (av < bv) delta = -1;
        else if (av > bv) delta = 1;
        else delta = 0;
        break;
      }
      case 11: { // Salvage
        const double av = a.reg ? a.reg->salvage_richness_mult : 1.0;
        const double bv = b.reg ? b.reg->salvage_richness_mult : 1.0;
        if (av < bv) delta = -1;
        else if (av > bv) delta = 1;
        else delta = 0;
        break;
      }
      default: delta = 0; break;
    }

    if (delta != 0) {
      if (spec.SortDirection == ImGuiSortDirection_Descending) delta = -delta;
      return delta;
    }
  }
  return 0;
}

} // namespace

void draw_regions_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  if (!ui.show_regions_window) return;

  if (!ImGui::Begin("Regions", &ui.show_regions_window)) {
    ImGui::End();
    return;
  }

  auto& s = sim.state();

  // Determine the viewer faction (for FoW visibility stats).
  const Ship* viewer_ship = (selected_ship != kInvalidId) ? find_ptr(s.ships, selected_ship) : nullptr;
  const Id viewer_faction_id = viewer_ship ? viewer_ship->faction_id : ui.viewer_faction_id;

  static char filter[128] = "";
  ImGui::TextDisabled("Filter");
  ImGui::SameLine();
  ImGui::InputTextWithHint("##region_filter", "name / theme", filter, IM_ARRAYSIZE(filter));
  ImGui::SameLine();
  if (ImGui::SmallButton("Clear##region_filter")) {
    filter[0] = '\0';
  }

  // Overlay controls (shortcut for map readability).
  ImGui::SeparatorText("Galaxy map overlay");
  ImGui::Checkbox("Region halos", &ui.show_galaxy_regions);
  ImGui::SameLine();
  ImGui::Checkbox("Boundaries", &ui.show_galaxy_region_boundaries);
  ImGui::BeginDisabled(!ui.show_galaxy_regions);
  ImGui::SameLine();
  ImGui::Checkbox("Labels", &ui.show_galaxy_region_labels);
  ImGui::EndDisabled();
  ImGui::BeginDisabled(!(ui.show_galaxy_regions || ui.show_galaxy_region_boundaries));
  ImGui::SameLine();
  ImGui::Checkbox("Dim non-selected", &ui.galaxy_region_dim_nonselected);
  ImGui::EndDisabled();

  // Aggregate per-region stats.
  std::unordered_map<Id, RegionAgg> agg;
  agg.reserve(s.regions.size() * 2);

  // Systems per region.
  for (const auto& [sid, sys] : s.systems) {
    if (sys.region_id == kInvalidId) continue;
    RegionAgg& a = agg[sys.region_id];
    a.systems_total++;
    a.sum_pos = a.sum_pos + sys.galaxy_pos;
    a.sum_count++;

    if (!a.init_bounds) {
      a.init_bounds = true;
      a.min_x = a.max_x = sys.galaxy_pos.x;
      a.min_y = a.max_y = sys.galaxy_pos.y;
    } else {
      a.min_x = std::min(a.min_x, sys.galaxy_pos.x);
      a.max_x = std::max(a.max_x, sys.galaxy_pos.x);
      a.min_y = std::min(a.min_y, sys.galaxy_pos.y);
      a.max_y = std::max(a.max_y, sys.galaxy_pos.y);
    }

    if (!ui.fog_of_war || viewer_faction_id == kInvalidId) {
      a.systems_visible++;
    } else {
      if (sim.is_system_discovered_by_faction(viewer_faction_id, sid)) a.systems_visible++;
    }
  }

  // Colonies per region.
  for (const auto& [cid, c] : s.colonies) {
    const auto* b = find_ptr(s.bodies, c.body_id);
    if (!b) continue;
    const auto* sys = find_ptr(s.systems, b->system_id);
    if (!sys) continue;
    const Id rid = sys->region_id;
    if (rid == kInvalidId) continue;
    RegionAgg& a = agg[rid];
    a.colonies++;
    a.pop_millions += c.population_millions;
  }

  // Build display rows.
  std::vector<RegionRow> rows;
  rows.reserve(s.regions.size());
  for (const auto& [rid, r] : s.regions) {
    const RegionAgg a = (agg.find(rid) != agg.end()) ? agg[rid] : RegionAgg{};
    RegionRow row = build_row(rid, r, a);

    // Filter.
    if (filter[0] != '\0') {
      const bool match = case_insensitive_contains(r.name, filter) || case_insensitive_contains(r.theme, filter);
      if (!match) continue;
    }

    rows.push_back(std::move(row));
  }

  // If selection is stale, clear it.
  if (ui.selected_region_id != kInvalidId) {
    if (s.regions.find(ui.selected_region_id) == s.regions.end()) {
      ui.selected_region_id = kInvalidId;
    }
  }

  ImGui::SeparatorText("Regions");

  const ImGuiTableFlags table_flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuter |
                                     ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable |
                                     ImGuiTableFlags_SortMulti;

  const float table_h = std::min(360.0f, ImGui::GetContentRegionAvail().y * 0.55f);
  if (ImGui::BeginTable("regions_table", 12, table_flags, ImVec2(0.0f, table_h))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort);
    ImGui::TableSetupColumn("Theme");
    ImGui::TableSetupColumn("Systems", ImGuiTableColumnFlags_PreferSortDescending);
    ImGui::TableSetupColumn("Visible", ImGuiTableColumnFlags_PreferSortDescending);
    ImGui::TableSetupColumn("Colonies", ImGuiTableColumnFlags_PreferSortDescending);
    ImGui::TableSetupColumn("Pop (M)", ImGuiTableColumnFlags_PreferSortDescending);
    ImGui::TableSetupColumn("Nebula", ImGuiTableColumnFlags_PreferSortDescending);
    ImGui::TableSetupColumn("Pirates", ImGuiTableColumnFlags_PreferSortDescending);
    ImGui::TableSetupColumn("Ruins", ImGuiTableColumnFlags_PreferSortDescending);
    ImGui::TableSetupColumn("Mineral", ImGuiTableColumnFlags_PreferSortDescending);
    ImGui::TableSetupColumn("Volatile", ImGuiTableColumnFlags_PreferSortDescending);
    ImGui::TableSetupColumn("Salvage", ImGuiTableColumnFlags_PreferSortDescending);
    ImGui::TableHeadersRow();

    if (ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs()) {
      if (sort_specs->SpecsDirty) {
        std::sort(rows.begin(), rows.end(), [&](const RegionRow& a, const RegionRow& b) {
          return compare_rows(a, b, sort_specs) < 0;
        });
        sort_specs->SpecsDirty = false;
      }
    }

    // Draw rows.
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const RegionRow& row = rows[i];
      const Region& r = *row.reg;
      const bool selected = (ui.selected_region_id == row.id);

      ImGui::TableNextRow();

      // Name (with color chip).
      ImGui::TableSetColumnIndex(0);
      {
        const ImU32 c = region_col(row.id, selected ? 0.9f : 0.55f);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + 10.0f, p.y + ImGui::GetTextLineHeight()), c);
        ImGui::Dummy(ImVec2(12.0f, 0.0f));
        ImGui::SameLine();

        if (ImGui::Selectable(r.name.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
          ui.selected_region_id = row.id;
          // Make it more likely the user sees something change immediately.
          ui.show_galaxy_regions = true;
        }
      }

      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(r.theme.empty() ? "" : r.theme.c_str());

      ImGui::TableSetColumnIndex(2);
      ImGui::Text("%d", row.a.systems_total);

      ImGui::TableSetColumnIndex(3);
      if (ui.fog_of_war && viewer_faction_id != kInvalidId) {
        ImGui::Text("%d", row.a.systems_visible);
      } else {
        ImGui::TextDisabled("%d", row.a.systems_visible);
      }

      ImGui::TableSetColumnIndex(4);
      ImGui::Text("%d", row.a.colonies);

      ImGui::TableSetColumnIndex(5);
      ImGui::Text("%.1f", row.a.pop_millions);

      ImGui::TableSetColumnIndex(6);
      ImGui::Text("%+.2f", r.nebula_bias);

      ImGui::TableSetColumnIndex(7);
      ImGui::Text("%.2f", r.pirate_risk);

      ImGui::TableSetColumnIndex(8);
      ImGui::Text("%.2f", r.ruins_density);

      ImGui::TableSetColumnIndex(9);
      ImGui::Text("%.2f", r.mineral_richness_mult);

      ImGui::TableSetColumnIndex(10);
      ImGui::Text("%.2f", r.volatile_richness_mult);

      ImGui::TableSetColumnIndex(11);
      ImGui::Text("%.2f", r.salvage_richness_mult);
    }

    ImGui::EndTable();
  }

  // Detail panel.
  ImGui::SeparatorText("Selected region");
  if (ui.selected_region_id == kInvalidId) {
    ImGui::TextDisabled("Select a region from the table above.");
    ImGui::End();
    return;
  }

  const Region* r = find_ptr(s.regions, ui.selected_region_id);
  if (!r) {
    ui.selected_region_id = kInvalidId;
    ImGui::TextDisabled("(selected region is missing)");
    ImGui::End();
    return;
  }

  // Find the matching row (to reuse precomputed centroid/span).
  RegionRow row;
  row.id = r->id;
  row.reg = r;
  if (agg.find(r->id) != agg.end()) row.a = agg[r->id];
  row = build_row(r->id, *r, row.a);

  ImGui::Text("%s", r->name.c_str());
  if (!r->theme.empty()) ImGui::TextDisabled("Theme: %s", r->theme.c_str());
  ImGui::TextDisabled("Center: %.2f, %.2f u", row.centroid.x, row.centroid.y);

  ImGui::Spacing();

  if (ImGui::Button("Focus Galaxy Map (fit)")) {
    ui.show_map_window = true;
    ui.request_map_tab = MapTab::Galaxy;
    ui.request_galaxy_map_center = true;
    ui.request_galaxy_map_center_x = row.centroid.x;
    ui.request_galaxy_map_center_y = row.centroid.y;

    // Add a bit of padding so the hull isn't touching the window.
    if (row.half_span > 1e-9) {
      ui.request_galaxy_map_fit_half_span = row.half_span * 1.15;
    }

    // Encourage visibility of the selection.
    ui.selected_region_id = r->id;
    ui.show_galaxy_region_boundaries = true;
    ui.show_galaxy_regions = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("Clear selection")) {
    ui.selected_region_id = kInvalidId;
  }

  ImGui::Spacing();

  // Modifiers.
  ImGui::SeparatorText("Procedural modifiers");
  ImGui::TextDisabled("Minerals:  x%.2f   Volatiles: x%.2f   Salvage: x%.2f", r->mineral_richness_mult, r->volatile_richness_mult,
                      r->salvage_richness_mult);
  ImGui::TextDisabled("Nebula bias: %+.2f   Pirate risk: %.2f   Ruins density: %.2f", r->nebula_bias, r->pirate_risk, r->ruins_density);

  // Systems list.
  ImGui::SeparatorText("Systems in region");

  std::vector<const StarSystem*> systems;
  systems.reserve(static_cast<std::size_t>(row.a.systems_total));
  for (const auto& [sid, sys] : s.systems) {
    if (sys.region_id != r->id) continue;
    systems.push_back(&sys);
  }
  std::sort(systems.begin(), systems.end(), [](const StarSystem* a, const StarSystem* b) {
    return a && b ? a->name < b->name : false;
  });

  const float list_h = std::min(220.0f, ImGui::GetContentRegionAvail().y);
  if (ImGui::BeginChild("##region_systems", ImVec2(0.0f, list_h), true)) {
    if (systems.empty()) {
      ImGui::TextDisabled("(none)");
    }

    for (const StarSystem* sys : systems) {
      if (!sys) continue;

      // Under FoW, don't leak undiscovered system names.
      bool visible = true;
      if (ui.fog_of_war && viewer_faction_id != kInvalidId) {
        visible = sim.is_system_discovered_by_faction(viewer_faction_id, sys->id);
      }

      ImGui::PushID(static_cast<int>(sys->id));

      if (visible) {
        ImGui::TextUnformatted(sys->name.c_str());
      } else {
        ImGui::TextDisabled("(undiscovered)");
      }

      ImGui::SameLine();
      if (ImGui::SmallButton("Galaxy")) {
        // Select + focus this system on the galaxy map.
        s.selected_system = sys->id;
        ui.show_map_window = true;
        ui.request_map_tab = MapTab::Galaxy;
        ui.request_galaxy_map_center = true;
        ui.request_galaxy_map_center_x = sys->galaxy_pos.x;
        ui.request_galaxy_map_center_y = sys->galaxy_pos.y;

        // Deselect ship/colony selections that no longer apply.
        if (selected_ship != kInvalidId) {
          const auto* sh = find_ptr(s.ships, selected_ship);
          if (!sh || sh->system_id != sys->id) selected_ship = kInvalidId;
        }
        if (selected_colony != kInvalidId) {
          const auto* c = find_ptr(s.colonies, selected_colony);
          const auto* b = c ? find_ptr(s.bodies, c->body_id) : nullptr;
          if (!b || b->system_id != sys->id) {
            selected_colony = kInvalidId;
            selected_body = kInvalidId;
          }
        }
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("System")) {
        // Switch to the system map tab.
        s.selected_system = sys->id;
        ui.show_map_window = true;
        ui.request_map_tab = MapTab::System;
      }

      ImGui::PopID();
    }

    ImGui::EndChild();
  }

  ImGui::End();
}

} // namespace nebula4x::ui
