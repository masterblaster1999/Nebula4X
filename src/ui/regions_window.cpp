#include "ui/regions_window.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <cstdio>
#include <cstring>
#include <vector>

#include <imgui.h>

#include "nebula4x/util/strings.h"
#include "nebula4x/core/region_planner.h"

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

  int known_hideouts{0};
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

struct RegionsEditorState {
  Id last_region_id{kInvalidId};

  // Editable fields (copied from the selected region on selection change).
  char name[128]{0};
  char theme[128]{0};
  double mineral_mult{1.0};
  double volatile_mult{1.0};
  double salvage_mult{1.0};
  double nebula_bias{0.0};
  double pirate_risk{0.0};
  double pirate_suppression{0.0};
  double ruins_density{0.0};

  bool confirm_delete{false};

  // System assignment UI.
  char system_filter[128]{0};
  bool show_only_unassigned{false};
  bool show_only_discovered{false};
  std::unordered_set<Id> selected_systems;

  // Auto-partition UI.
  RegionPlannerOptions plan_opt{};
  RegionPlannerApplyOptions apply_opt{};
  RegionPlannerResult plan{};
  bool have_plan{false};
  std::string last_error;
};

RegionsEditorState& editor_state() {
  static RegionsEditorState st;
  return st;
}

double sane_nonneg(double v, double fallback) {
  if (!std::isfinite(v)) return fallback;
  if (v < 0.0) return 0.0;
  return v;
}

double clamp01(double v) {
  if (!std::isfinite(v)) return 0.0;
  if (v < 0.0) return 0.0;
  if (v > 1.0) return 1.0;
  return v;
}

void sync_editor_from_region(RegionsEditorState& es, const Region& r) {
  if (es.last_region_id == r.id) return;
  es.last_region_id = r.id;

  std::snprintf(es.name, sizeof(es.name), "%s", r.name.c_str());
  std::snprintf(es.theme, sizeof(es.theme), "%s", r.theme.c_str());

  es.mineral_mult = sane_nonneg(r.mineral_richness_mult, 1.0);
  es.volatile_mult = sane_nonneg(r.volatile_richness_mult, 1.0);
  es.salvage_mult = sane_nonneg(r.salvage_richness_mult, 1.0);
  es.nebula_bias = std::isfinite(r.nebula_bias) ? std::clamp(r.nebula_bias, -1.0, 1.0) : 0.0;
  es.pirate_risk = clamp01(r.pirate_risk);
  es.pirate_suppression = clamp01(r.pirate_suppression);
  es.ruins_density = clamp01(r.ruins_density);

  es.confirm_delete = false;
  es.selected_systems.clear();

  if (!es.have_plan) {
    es.plan_opt = RegionPlannerOptions{};
    es.apply_opt = RegionPlannerApplyOptions{};
  }
}

void apply_editor_to_region(const RegionsEditorState& es, Region& r) {
  r.name = std::string(es.name);
  r.theme = std::string(es.theme);

  r.mineral_richness_mult = sane_nonneg(es.mineral_mult, 1.0);
  r.volatile_richness_mult = sane_nonneg(es.volatile_mult, 1.0);
  r.salvage_richness_mult = sane_nonneg(es.salvage_mult, 1.0);
  r.nebula_bias = std::clamp(std::isfinite(es.nebula_bias) ? es.nebula_bias : 0.0, -1.0, 1.0);
  r.pirate_risk = clamp01(es.pirate_risk);
  r.pirate_suppression = clamp01(es.pirate_suppression);
  r.ruins_density = clamp01(es.ruins_density);
}

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
      case 7: { // Pirates (effective)
        const auto eff = [](const Region* r) -> double {
          if (!r) return 0.0;
          const double base = std::clamp(r->pirate_risk, 0.0, 1.0);
          const double supp = std::clamp(r->pirate_suppression, 0.0, 1.0);
          return std::clamp(base * (1.0 - supp), 0.0, 1.0);
        };
        const double av = eff(a.reg);
        const double bv = eff(b.reg);
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

  // Known pirate hideouts per region.
  //
  // Under Fog-of-War: derive from the viewer faction's ship contacts (no leakage).
  // Without Fog-of-War: count actual hideout ships.
  if (!ui.fog_of_war || viewer_faction_id == kInvalidId) {
    for (const auto& [sid, sh] : s.ships) {
      (void)sid;
      if (sh.design_id != "pirate_hideout") continue;
      if (sh.system_id == kInvalidId) continue;
      const auto* sys = find_ptr(s.systems, sh.system_id);
      if (!sys) continue;
      const Id rid = sys->region_id;
      if (rid == kInvalidId) continue;
      agg[rid].known_hideouts += 1;
    }
  } else {
    const auto* fac = find_ptr(s.factions, viewer_faction_id);
    if (fac) {
      for (const auto& [_, c] : fac->ship_contacts) {
        if (c.system_id == kInvalidId) continue;
        if (c.last_seen_design_id != "pirate_hideout") continue;
        if (!sim.is_system_discovered_by_faction(viewer_faction_id, c.system_id)) continue;
        const auto* sys = find_ptr(s.systems, c.system_id);
        if (!sys) continue;
        const Id rid = sys->region_id;
        if (rid == kInvalidId) continue;
        agg[rid].known_hideouts += 1;
      }
    }
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
      const double base_risk = std::clamp(r.pirate_risk, 0.0, 1.0);
      const double supp = std::clamp(r.pirate_suppression, 0.0, 1.0);
      const double eff_risk = std::clamp(base_risk * (1.0 - supp), 0.0, 1.0);
      ImGui::Text("%.2f", eff_risk);
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Base risk %.2f\nSuppression %.2f\nEffective %.2f\nKnown hideouts %d", base_risk, supp, eff_risk, row.a.known_hideouts);
      }

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

  Region* r = find_ptr(s.regions, ui.selected_region_id);
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
  const double base_risk = std::clamp(r->pirate_risk, 0.0, 1.0);
  const double supp = std::clamp(r->pirate_suppression, 0.0, 1.0);
  const double eff_risk = std::clamp(base_risk * (1.0 - supp), 0.0, 1.0);
  ImGui::TextDisabled(
      "Nebula bias: %+.2f   Pirate risk: %.2f (base %.2f, supp %.2f)   Known hideouts: %d   Ruins density: %.2f",
      r->nebula_bias, eff_risk, base_risk, supp, row.a.known_hideouts, r->ruins_density);

  RegionsEditorState& es = editor_state();
  sync_editor_from_region(es, *r);

  // --- Region editing ---
  ImGui::SeparatorText("Editor");
  if (ImGui::CollapsingHeader("Edit region properties", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::InputText("Name##region_name", es.name, IM_ARRAYSIZE(es.name));
    ImGui::InputText("Theme##region_theme", es.theme, IM_ARRAYSIZE(es.theme));

    ImGui::Spacing();
    ImGui::TextDisabled("Modifiers (affect piracy risk, procgen bias, and some contract risk estimates):");

    ImGui::SetNextItemWidth(140.0f);
    ImGui::InputDouble("Mineral x", &es.mineral_mult, 0.05, 0.25, "%.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    ImGui::InputDouble("Volatile x", &es.volatile_mult, 0.05, 0.25, "%.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    ImGui::InputDouble("Salvage x", &es.salvage_mult, 0.05, 0.25, "%.2f");

    ImGui::SetNextItemWidth(140.0f);
    ImGui::InputDouble("Nebula bias", &es.nebula_bias, 0.05, 0.25, "%+.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    ImGui::InputDouble("Pirate risk", &es.pirate_risk, 0.02, 0.10, "%.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    ImGui::InputDouble("Ruins density", &es.ruins_density, 0.02, 0.10, "%.2f");

    ImGui::SetNextItemWidth(140.0f);
    ImGui::InputDouble("Pirate suppression", &es.pirate_suppression, 0.02, 0.10, "%.2f");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset suppression")) {
      es.pirate_suppression = 0.0;
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Suppression is normally updated by patrol missions.\nManual edits are allowed for scenario authoring.");
    }

    // Clamp to sane ranges in the UI before applying.
    es.mineral_mult = sane_nonneg(es.mineral_mult, 1.0);
    es.volatile_mult = sane_nonneg(es.volatile_mult, 1.0);
    es.salvage_mult = sane_nonneg(es.salvage_mult, 1.0);
    es.nebula_bias = std::clamp(std::isfinite(es.nebula_bias) ? es.nebula_bias : 0.0, -1.0, 1.0);
    es.pirate_risk = clamp01(es.pirate_risk);
    es.pirate_suppression = clamp01(es.pirate_suppression);
    es.ruins_density = clamp01(es.ruins_density);

    if (ImGui::Button("Apply edits")) {
      apply_editor_to_region(es, *r);
      es.last_error.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Center = centroid")) {
      r->center = row.centroid;
      es.last_error.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Duplicate")) {
      Region copy = *r;
      copy.id = allocate_id(s);
      if (copy.name.empty()) copy.name = "Region " + std::to_string((unsigned long long)copy.id);
      copy.name += " (copy)";
      s.regions[copy.id] = copy;
      ui.selected_region_id = copy.id;
      es.last_error.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("New region")) {
      Region nr;
      nr.id = allocate_id(s);
      nr.name = "Region " + std::to_string((unsigned long long)nr.id);
      nr.center = row.centroid;
      s.regions[nr.id] = nr;
      ui.selected_region_id = nr.id;
      es.last_error.clear();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("Danger zone:");
    ImGui::Checkbox("Confirm delete", &es.confirm_delete);
    ImGui::SameLine();
    ImGui::BeginDisabled(!es.confirm_delete);
    if (ImGui::Button("Delete region")) {
      // Unassign member systems then delete.
      for (auto& [sid, sys] : s.systems) {
        (void)sid;
        if (sys.region_id == r->id) sys.region_id = kInvalidId;
      }
      s.regions.erase(r->id);
      ui.selected_region_id = kInvalidId;
      es.last_region_id = kInvalidId;
      es.confirm_delete = false;
      es.last_error.clear();
      ImGui::EndDisabled();
      ImGui::End();
      return;
    }
    ImGui::EndDisabled();

    if (!es.last_error.empty()) {
      ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", es.last_error.c_str());
    }
  }

  // --- System assignment ---
  if (ImGui::CollapsingHeader("Assign systems", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::InputTextWithHint("Filter##assign_sys_filter", "system name / id", es.system_filter, IM_ARRAYSIZE(es.system_filter));
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear##assign_sys_filter")) es.system_filter[0] = '\0';
    ImGui::SameLine();
    ImGui::Checkbox("Only unassigned", &es.show_only_unassigned);
    if (ui.fog_of_war && viewer_faction_id != kInvalidId) {
      ImGui::SameLine();
      ImGui::Checkbox("Only discovered", &es.show_only_discovered);
    } else {
      es.show_only_discovered = false;
    }

    auto name_matches = [&](const StarSystem& sys) -> bool {
      if (es.system_filter[0] == '\0') return true;
      const std::string id_str = std::to_string((unsigned long long)sys.id);
      if (case_insensitive_contains(id_str, es.system_filter)) return true;
      return case_insensitive_contains(sys.name, es.system_filter);
    };

    // Bulk actions.
    if (ImGui::SmallButton("Select all filtered")) {
      es.selected_systems.clear();
      for (const auto& [sid, sys] : s.systems) {
        (void)sid;
        if (!name_matches(sys)) continue;
        if (es.show_only_unassigned && sys.region_id != kInvalidId) continue;
        if (es.show_only_discovered && !sim.is_system_discovered_by_faction(viewer_faction_id, sys.id)) continue;
        es.selected_systems.insert(sys.id);
      }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear selection")) {
      es.selected_systems.clear();
    }
    ImGui::SameLine();
    const bool any_sel = !es.selected_systems.empty();
    ImGui::BeginDisabled(!any_sel);
    if (ImGui::SmallButton("Assign selected -> this region")) {
      for (Id sid : es.selected_systems) {
        auto it = s.systems.find(sid);
        if (it != s.systems.end()) it->second.region_id = r->id;
      }
      es.selected_systems.clear();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Unassign selected")) {
      for (Id sid : es.selected_systems) {
        auto it = s.systems.find(sid);
        if (it != s.systems.end() && it->second.region_id == r->id) it->second.region_id = kInvalidId;
      }
      es.selected_systems.clear();
    }
    ImGui::EndDisabled();

    ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
                         ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    const float th = std::min(240.0f, ImGui::GetContentRegionAvail().y * 0.40f);
    if (ImGui::BeginTable("assign_systems_table", 4, tf, ImVec2(0.0f, th))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Sel", ImGuiTableColumnFlags_WidthFixed, 40.0f);
      ImGui::TableSetupColumn("System");
      ImGui::TableSetupColumn("Current region");
      ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 90.0f);
      ImGui::TableHeadersRow();

      // Sorted by name for UX.
      std::vector<const StarSystem*> all;
      all.reserve(s.systems.size());
      for (const auto& [sid, sys] : s.systems) {
        (void)sid;
        all.push_back(&sys);
      }
      std::sort(all.begin(), all.end(), [](const StarSystem* a, const StarSystem* b) {
        if (!a || !b) return false;
        if (a->name != b->name) return a->name < b->name;
        return a->id < b->id;
      });

      for (const StarSystem* sys : all) {
        if (!sys) continue;
        if (!name_matches(*sys)) continue;
        if (es.show_only_unassigned && sys->region_id != kInvalidId) continue;
        if (es.show_only_discovered && !sim.is_system_discovered_by_faction(viewer_faction_id, sys->id)) continue;

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        bool sel = es.selected_systems.find(sys->id) != es.selected_systems.end();
        if (ImGui::Checkbox(("##sel_" + std::to_string((unsigned long long)sys->id)).c_str(), &sel)) {
          if (sel) es.selected_systems.insert(sys->id);
          else es.selected_systems.erase(sys->id);
        }

        ImGui::TableSetColumnIndex(1);
        // Under FoW, avoid leaking undiscovered system names.
        bool visible = true;
        if (ui.fog_of_war && viewer_faction_id != kInvalidId) {
          visible = sim.is_system_discovered_by_faction(viewer_faction_id, sys->id);
        }
        if (visible) {
          ImGui::TextUnformatted(sys->name.c_str());
        } else {
          ImGui::TextDisabled("(undiscovered)");
        }

        ImGui::TableSetColumnIndex(2);
        if (sys->region_id == kInvalidId) {
          ImGui::TextDisabled("(none)");
        } else if (const Region* cr = find_ptr(s.regions, sys->region_id)) {
          ImGui::TextUnformatted(cr->name.c_str());
        } else {
          ImGui::TextDisabled("(missing)");
        }

        ImGui::TableSetColumnIndex(3);
        if (sys->region_id == r->id) {
          if (ImGui::SmallButton(("Unassign##u_" + std::to_string((unsigned long long)sys->id)).c_str())) {
            s.systems.at(sys->id).region_id = kInvalidId;
          }
        } else {
          if (ImGui::SmallButton(("Assign##a_" + std::to_string((unsigned long long)sys->id)).c_str())) {
            s.systems.at(sys->id).region_id = r->id;
          }
        }
      }

      ImGui::EndTable();
    }
  }

  // --- Auto partitioning ---
  if (ImGui::CollapsingHeader("Auto-partition regions (k-means)", ImGuiTreeNodeFlags_DefaultOpen)) {
    es.plan_opt.viewer_faction_id = viewer_faction_id;
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt("Regions (k)", &es.plan_opt.k);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    int seed_i = (int)es.plan_opt.seed;
    if (ImGui::InputInt("Seed", &seed_i)) es.plan_opt.seed = (std::uint32_t)std::max(0, seed_i);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt("Max iters", &es.plan_opt.max_iters);

    ImGui::Checkbox("Only unassigned systems", &es.plan_opt.only_unassigned_systems);
    if (ui.fog_of_war && viewer_faction_id != kInvalidId) {
      ImGui::SameLine();
      ImGui::Checkbox("Restrict to discovered", &es.plan_opt.restrict_to_discovered);
    } else {
      es.plan_opt.restrict_to_discovered = false;
    }

    if (ImGui::Button("Compute plan")) {
      es.plan = nebula4x::compute_region_partition_plan(sim, es.plan_opt);
      es.have_plan = es.plan.ok;
      es.last_error = es.plan.ok ? std::string{} : es.plan.message;
      if (es.plan.ok) {
        es.apply_opt = RegionPlannerApplyOptions{};
        es.apply_opt.name_prefix = "Region";
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear plan")) {
      es.have_plan = false;
      es.plan = RegionPlannerResult{};
      es.last_error.clear();
    }

    if (!es.last_error.empty()) {
      ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", es.last_error.c_str());
    }

    if (es.have_plan) {
      ImGui::TextDisabled("%s  (inertia %.2f)", es.plan.message.c_str(), es.plan.total_inertia);

      ImGuiTableFlags pf = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
                           ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
      const float ph = std::min(210.0f, ImGui::GetContentRegionAvail().y * 0.35f);
      if (ImGui::BeginTable("region_plan_table", 6, pf, ImVec2(0.0f, ph))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 32.0f);
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Theme");
        ImGui::TableSetupColumn("Systems", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Pirate risk", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Nebula bias", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < (int)es.plan.clusters.size(); ++i) {
          const auto& cl = es.plan.clusters[(size_t)i];
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          ImGui::Text("%d", i + 1);
          ImGui::TableSetColumnIndex(1);
          ImGui::TextUnformatted(cl.region.name.c_str());
          ImGui::TableSetColumnIndex(2);
          ImGui::TextUnformatted(cl.region.theme.c_str());
          ImGui::TableSetColumnIndex(3);
          ImGui::Text("%d", (int)cl.system_ids.size());
          ImGui::TableSetColumnIndex(4);
          ImGui::Text("%.2f", std::clamp(cl.region.pirate_risk, 0.0, 1.0));
          ImGui::TableSetColumnIndex(5);
          ImGui::Text("%+.2f", std::clamp(cl.region.nebula_bias, -1.0, 1.0));
        }

        ImGui::EndTable();
      }

      ImGui::Spacing();
      ImGui::TextDisabled("Apply options:");
      ImGui::Checkbox("Wipe existing regions", &es.apply_opt.wipe_existing_regions);
      ImGui::SameLine();
      ImGui::Checkbox("Clear unplanned assignments", &es.apply_opt.clear_unplanned_system_assignments);
      ImGui::SetNextItemWidth(200.0f);
      static char prefix_buf[64] = "Region";
      if (ImGui::InputText("Name prefix", prefix_buf, IM_ARRAYSIZE(prefix_buf))) {
        es.apply_opt.name_prefix = prefix_buf;
      } else {
        es.apply_opt.name_prefix = prefix_buf;
      }

      if (ImGui::Button("Apply plan")) {
        std::string err;
        if (!nebula4x::apply_region_partition_plan(s, es.plan, es.apply_opt, &err)) {
          es.last_error = err.empty() ? "Apply failed." : err;
        } else {
          es.last_error.clear();
          ui.show_galaxy_regions = true;
          ui.show_galaxy_region_boundaries = true;
          ui.selected_region_id = kInvalidId;
          es.last_region_id = kInvalidId;
          es.have_plan = false;
        }
      }
    }
  }

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
