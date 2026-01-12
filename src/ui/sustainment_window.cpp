#include "ui/sustainment_window.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <string>
#include <vector>

namespace nebula4x::ui {

namespace {

struct SustainmentWindowState {
  Id faction_id{kInvalidId};
  Id fleet_id{kInvalidId};
  Id colony_id{kInvalidId};

  // Multiplier relative to a fleet's full loadout.
  // 1.0 => keep enough stock on-hand to fully refill/rearm the fleet from empty.
  double reload_multiplier{1.0};

  // Buffer of maintenance supplies (days) to keep on-hand for the selected fleet.
  double maintenance_buffer_days{30.0};

  // If true, overwrite existing mineral targets for these resources.
  // If false, only increase targets to meet the recommendation.
  bool overwrite_targets{false};
};

struct FleetTotals {
  int ship_count{0};

  double fuel_cap{0.0};
  double fuel_have{0.0};

  int ammo_cap{0};
  int ammo_have{0};

  double mass_tons{0.0};
  double avg_maintenance_condition{1.0};
};

std::string fmt_double(double v, int precision) {
  if (!std::isfinite(v)) return "?";
  precision = std::max(0, precision);
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*f", precision, v);
  return std::string(buf);
}

SustainmentWindowState& st() {
  static SustainmentWindowState s;
  return s;
}

void select_colony(UIState& ui, const Colony* colony, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  (void)selected_ship;
  if (!colony) return;
  selected_colony = colony->id;
  selected_body = colony->body_id;
  ui.show_details_window = true;
  ui.request_details_tab = DetailsTab::Colony;
}

void select_fleet(UIState& ui, Id fleet_id) {
  ui.selected_fleet_id = fleet_id;
  ui.show_details_window = true;
  ui.request_details_tab = DetailsTab::Fleet;
}

FleetTotals compute_fleet_totals(const Simulation& sim, const Fleet& fleet) {
  FleetTotals t;

  double maint_sum = 0.0;

  for (Id sid : fleet.ship_ids) {
    const Ship* sh = find_ptr(sim.state().ships, sid);
    if (!sh) continue;
    const ShipDesign* d = sim.find_design(sh->design_id);
    if (!d) continue;

    ++t.ship_count;

    // Fuel
    const double cap = std::max(0.0, d->fuel_capacity_tons);
    double have = sh->fuel_tons;
    if (have < 0.0) have = cap;  // legacy/full sentinel
    have = std::clamp(have, 0.0, cap);
    t.fuel_cap += cap;
    t.fuel_have += have;

    // Ammo (finite magazines only)
    const int ammo_cap_i = std::max(0, d->missile_ammo_capacity);
    int ammo_have_i = sh->missile_ammo;
    if (ammo_have_i < 0) ammo_have_i = ammo_cap_i;  // legacy/full sentinel
    ammo_have_i = std::clamp(ammo_have_i, 0, ammo_cap_i);
    t.ammo_cap += ammo_cap_i;
    t.ammo_have += ammo_have_i;

    // Maintenance (mass-based)
    t.mass_tons += std::max(0.0, d->mass_tons);
    maint_sum += std::clamp(sh->maintenance_condition, 0.0, 1.0);
  }

  if (t.ship_count > 0) t.avg_maintenance_condition = maint_sum / static_cast<double>(t.ship_count);

  return t;
}

double get_have(const std::unordered_map<std::string, double>& m, const std::string& k) {
  if (auto it = m.find(k); it != m.end()) return it->second;
  return 0.0;
}

double get_target(const std::unordered_map<std::string, double>& m, const std::string& k) {
  if (auto it = m.find(k); it != m.end()) return it->second;
  return 0.0;
}

void apply_target(Colony& c, const std::string& k, double value, bool overwrite) {
  if (value <= 1e-9) return;
  if (overwrite) {
    c.mineral_targets[k] = value;
    return;
  }
  auto& v = c.mineral_targets[k];
  v = std::max(v, value);
}

void clear_target(Colony& c, const std::string& k) {
  c.mineral_targets.erase(k);
}

}  // namespace

void draw_sustainment_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  ImGui::SetNextWindowSize(ImVec2(720, 520), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Sustainment Planner", &ui.show_sustainment_window)) {
    ImGui::End();
    return;
  }

  SustainmentWindowState& s = st();
  GameState& gs = sim.state();

  // --- Faction selection ---
  if (!find_ptr(gs.factions, s.faction_id)) {
    s.faction_id = ui.viewer_faction_id;
    if (!find_ptr(gs.factions, s.faction_id) && !gs.factions.empty()) {
      s.faction_id = gs.factions.begin()->first;
    }
    s.fleet_id = kInvalidId;
    s.colony_id = kInvalidId;
  }

  const Faction* fac = find_ptr(gs.factions, s.faction_id);
  const char* fac_label = fac ? fac->name.c_str() : "(none)";

  if (ImGui::BeginCombo("Faction", fac_label)) {
    std::vector<Id> fids;
    fids.reserve(gs.factions.size());
    for (const auto& [fid, _] : gs.factions) fids.push_back(fid);
    std::sort(fids.begin(), fids.end(), [&](Id a, Id b) {
      const Faction* fa = find_ptr(gs.factions, a);
      const Faction* fb = find_ptr(gs.factions, b);
      const std::string an = fa ? fa->name : "";
      const std::string bn = fb ? fb->name : "";
      if (an != bn) return an < bn;
      return a < b;
    });

    for (Id fid : fids) {
      const Faction* f = find_ptr(gs.factions, fid);
      if (!f) continue;
      const bool selected = (fid == s.faction_id);
      if (ImGui::Selectable(f->name.c_str(), selected)) {
        s.faction_id = fid;
        s.fleet_id = kInvalidId;
        s.colony_id = kInvalidId;
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }

  // --- Fleet selection ---
  std::vector<Id> fleet_ids;
  fleet_ids.reserve(gs.fleets.size());
  for (const auto& [fid, fl] : gs.fleets) {
    if (fl.faction_id == s.faction_id) fleet_ids.push_back(fid);
  }
  std::sort(fleet_ids.begin(), fleet_ids.end(), [&](Id a, Id b) {
    const Fleet* fa = find_ptr(gs.fleets, a);
    const Fleet* fb = find_ptr(gs.fleets, b);
    const std::string an = fa ? fa->name : "";
    const std::string bn = fb ? fb->name : "";
    if (an != bn) return an < bn;
    return a < b;
  });

  if (!find_ptr(gs.fleets, s.fleet_id)) {
    s.fleet_id = fleet_ids.empty() ? kInvalidId : fleet_ids.front();
  }

  const Fleet* fleet = find_ptr(gs.fleets, s.fleet_id);
  const char* fleet_label = fleet ? fleet->name.c_str() : "(no fleet)";

  if (ImGui::BeginCombo("Fleet", fleet_label)) {
    for (Id fid : fleet_ids) {
      const Fleet* fl = find_ptr(gs.fleets, fid);
      if (!fl) continue;
      const bool selected = (fid == s.fleet_id);
      if (ImGui::Selectable(fl->name.c_str(), selected)) {
        s.fleet_id = fid;
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  if (fleet && ImGui::SmallButton("Select Fleet")) {
    select_fleet(ui, fleet->id);
  }

  // --- Support colony selection ---
  std::vector<Id> colony_ids;
  colony_ids.reserve(gs.colonies.size());
  for (const auto& [cid, c] : gs.colonies) {
    if (c.faction_id == s.faction_id) colony_ids.push_back(cid);
  }
  std::sort(colony_ids.begin(), colony_ids.end(), [&](Id a, Id b) {
    const Colony* ca = find_ptr(gs.colonies, a);
    const Colony* cb = find_ptr(gs.colonies, b);
    const std::string an = ca ? ca->name : "";
    const std::string bn = cb ? cb->name : "";
    if (an != bn) return an < bn;
    return a < b;
  });

  if (!find_ptr(gs.colonies, s.colony_id)) {
    s.colony_id = colony_ids.empty() ? kInvalidId : colony_ids.front();
  }

  Colony* colony = find_ptr(gs.colonies, s.colony_id);
  const char* colony_label = colony ? colony->name.c_str() : "(no colony)";

  if (ImGui::BeginCombo("Support Colony", colony_label)) {
    for (Id cid : colony_ids) {
      const Colony* c = find_ptr(gs.colonies, cid);
      if (!c) continue;
      const bool selected = (cid == s.colony_id);
      if (ImGui::Selectable(c->name.c_str(), selected)) {
        s.colony_id = cid;
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  if (colony && ImGui::SmallButton("Select Colony")) {
    select_colony(ui, colony, selected_ship, selected_colony, selected_body);
  }

  ImGui::Separator();

  if (!fleet) {
    ImGui::TextUnformatted("No fleet selected.");
    ImGui::End();
    return;
  }

  const FleetTotals totals = compute_fleet_totals(sim, *fleet);

  // --- Inputs ---
  ImGui::Text("Fleet ships: %d", totals.ship_count);
  ImGui::Text("Fuel: %s / %s", fmt_double(totals.fuel_have, 1).c_str(), fmt_double(totals.fuel_cap, 1).c_str());
  if (totals.fuel_cap > 1e-9) {
    const double pct = std::clamp(totals.fuel_have / totals.fuel_cap, 0.0, 1.0) * 100.0;
    ImGui::SameLine();
    ImGui::Text("(%.0f%%)", pct);
  }

  if (totals.ammo_cap > 0) {
    ImGui::Text("Munitions: %d / %d", totals.ammo_have, totals.ammo_cap);
    const double pct = std::clamp(static_cast<double>(totals.ammo_have) / std::max(1.0, static_cast<double>(totals.ammo_cap)), 0.0, 1.0) * 100.0;
    ImGui::SameLine();
    ImGui::Text("(%.0f%%)", pct);
  } else {
    ImGui::TextUnformatted("Munitions: (fleet has no finite magazines)");
  }

  if (sim.cfg().enable_ship_maintenance) {
    ImGui::Text("Avg maint condition: %.0f%%", std::clamp(totals.avg_maintenance_condition, 0.0, 1.0) * 100.0);
  } else {
    ImGui::TextUnformatted("Ship maintenance: disabled");
  }

  ImGui::Spacing();

  // Reload multiplier slider.
  ImGui::SetNextItemWidth(240);
  const double reload_min = 0.0;
  const double reload_max = 3.0;
  ImGui::SliderScalar("Reload multiplier", ImGuiDataType_Double, &s.reload_multiplier, &reload_min, &reload_max, "%.2fx");
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Recommended stockpile relative to the fleet's full loadout.\n1.0 => enough to refill/rearm from empty.");
  }

  // Maintenance buffer slider.
  ImGui::SetNextItemWidth(240);
  const double maint_min = 0.0;
  const double maint_max = 180.0;
  ImGui::SliderScalar("Maintenance buffer (days)", ImGuiDataType_Double, &s.maintenance_buffer_days, &maint_min, &maint_max, "%.0f");
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Recommended maintenance supplies to keep at the support colony for this fleet.");
  }

  ImGui::Checkbox("Overwrite existing targets", &s.overwrite_targets);

  ImGui::Separator();

  if (!colony) {
    ImGui::TextUnformatted("No colony selected.");
    ImGui::End();
    return;
  }

  // --- Recommendations ---
  const double rec_fuel = std::max(0.0, totals.fuel_cap * std::max(0.0, s.reload_multiplier));
  const double rec_mun = std::max(0.0, static_cast<double>(totals.ammo_cap) * std::max(0.0, s.reload_multiplier));

  double rec_maint = 0.0;
  if (sim.cfg().enable_ship_maintenance && sim.cfg().ship_maintenance_tons_per_day_per_mass_ton > 0.0 &&
      !sim.cfg().ship_maintenance_resource_id.empty()) {
    const double per_day = totals.mass_tons * sim.cfg().ship_maintenance_tons_per_day_per_mass_ton;
    rec_maint = std::max(0.0, per_day * std::max(0.0, s.maintenance_buffer_days));
  }

  const std::string maint_res = sim.cfg().ship_maintenance_resource_id;

  if (ImGui::BeginTable("sustainment_table", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
    ImGui::TableSetupColumn("Resource");
    ImGui::TableSetupColumn("Colony Have");
    ImGui::TableSetupColumn("Current Target");
    ImGui::TableSetupColumn("Recommended Target");
    ImGui::TableSetupColumn("Delta (Need)" );
    ImGui::TableHeadersRow();

    auto row = [&](const std::string& res, double recommended) {
      const double have = get_have(colony->minerals, res);
      const double cur_tgt = get_target(colony->mineral_targets, res);
      const double delta = std::max(0.0, recommended - have);

      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(res.c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(fmt_double(have, 1).c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(fmt_double(cur_tgt, 1).c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(fmt_double(recommended, 1).c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(fmt_double(delta, 1).c_str());
    };

    row("Fuel", rec_fuel);
    if (totals.ammo_cap > 0) row("Munitions", rec_mun);
    if (rec_maint > 1e-9 && !maint_res.empty()) row(maint_res, rec_maint);

    ImGui::EndTable();
  }

  ImGui::Spacing();

  if (ImGui::SmallButton("Apply Targets")) {
    apply_target(*colony, "Fuel", rec_fuel, s.overwrite_targets);
    if (totals.ammo_cap > 0) apply_target(*colony, "Munitions", rec_mun, s.overwrite_targets);
    if (rec_maint > 1e-9 && !maint_res.empty()) apply_target(*colony, maint_res, rec_maint, s.overwrite_targets);
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Clear Targets")) {
    clear_target(*colony, "Fuel");
    clear_target(*colony, "Munitions");
    if (!maint_res.empty()) clear_target(*colony, maint_res);
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Open Freight Planner")) {
    ui.show_freight_window = true;
  }

  ImGui::End();
}

}  // namespace nebula4x::ui
