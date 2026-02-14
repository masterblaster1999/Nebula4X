#include "ui/maintenance_planner_window.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "nebula4x/core/maintenance_planner.h"
#include "nebula4x/core/simulation.h"
#include "nebula4x/util/log.h"
#include "ui/ui_state.h"

namespace nebula4x::ui {
namespace {

struct MaintenancePlannerWindowState {
  Id faction_id{kInvalidId};

  // Planning knobs.
  bool auto_refresh{true};
  bool restrict_to_discovered{true};
  bool include_trade_partner_colonies{true};
  bool prefer_shipyards{true};
  bool require_shipyard_when_critical{true};
  bool require_supplies_available{true};

  float threshold_fraction{0.75f};
  float target_fraction{0.95f};
  float reserve_buffer_fraction{0.10f};

  bool require_idle_ships{false};
  bool exclude_fleet_ships{false};

  int max_ships{2048};
  int max_colonies{2048};
  int max_candidates_per_ship{12};

  // Apply knobs.
  bool clear_orders_before_apply{true};
  bool use_smart_travel{true};

  // Cache.
  bool have_plan{false};
  int last_day{-1};
  int last_hour{-1};
  nebula4x::MaintenancePlannerResult plan;
};

std::string fmt_days(double days) {
  if (!std::isfinite(days)) return "inf";
  if (days < 0.0) return "?";
  char buf[64];
  if (days < 1.0) {
    std::snprintf(buf, sizeof(buf), "%.2f d", days);
  } else if (days < 10.0) {
    std::snprintf(buf, sizeof(buf), "%.1f d", days);
  } else {
    std::snprintf(buf, sizeof(buf), "%.0f d", days);
  }
  return std::string(buf);
}

std::string fmt_tons(double t) {
  if (!std::isfinite(t)) return "inf";
  char buf[64];
  if (std::abs(t) < 1000.0) {
    std::snprintf(buf, sizeof(buf), "%.0f", t);
  } else if (std::abs(t) < 1000000.0) {
    std::snprintf(buf, sizeof(buf), "%.1fk", t / 1000.0);
  } else {
    std::snprintf(buf, sizeof(buf), "%.2fM", t / 1000000.0);
  }
  return std::string(buf);
}

std::string fmt_pct(double frac01) {
  if (!std::isfinite(frac01)) return "?";
  frac01 = std::clamp(frac01, 0.0, 1.0);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.0f%%", frac01 * 100.0);
  return std::string(buf);
}

void focus_ship(Id ship_id, Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  auto& st = sim.state();
  selected_ship = ship_id;
  selected_colony = kInvalidId;
  selected_body = kInvalidId;

  if (const auto* sh = find_ptr(st.ships, ship_id)) {
    st.selected_system = sh->system_id;
    ui.show_map_window = true;
    ui.request_map_tab = MapTab::System;
    ui.show_details_window = true;
    ui.request_details_tab = DetailsTab::Ship;
  }
}

void focus_colony(Id colony_id, Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  auto& st = sim.state();
  selected_ship = kInvalidId;
  selected_colony = colony_id;
  selected_body = kInvalidId;

  if (const auto* c = find_ptr(st.colonies, colony_id)) {
    if (const auto* b = find_ptr(st.bodies, c->body_id)) {
      st.selected_system = b->system_id;
      ui.show_map_window = true;
      ui.request_map_tab = MapTab::System;
      ui.show_details_window = true;
      ui.request_details_tab = DetailsTab::Colony;
    }
  }
}

void compute_plan(MaintenancePlannerWindowState& mw, const Simulation& sim) {
  nebula4x::MaintenancePlannerOptions opt;
  opt.restrict_to_discovered = mw.restrict_to_discovered;
  opt.include_trade_partner_colonies = mw.include_trade_partner_colonies;
  opt.prefer_shipyards = mw.prefer_shipyards;
  opt.require_shipyard_when_critical = mw.require_shipyard_when_critical;
  opt.require_supplies_available = mw.require_supplies_available;
  opt.threshold_fraction = std::clamp(static_cast<double>(mw.threshold_fraction), 0.0, 1.0);
  opt.target_fraction = std::clamp(static_cast<double>(mw.target_fraction), 0.0, 1.0);
  opt.reserve_buffer_fraction = std::clamp(static_cast<double>(mw.reserve_buffer_fraction), 0.0, 0.95);
  opt.require_idle_ships = mw.require_idle_ships;
  opt.exclude_fleet_ships = mw.exclude_fleet_ships;
  opt.max_ships = std::clamp(mw.max_ships, 1, 20000);
  opt.max_colonies = std::clamp(mw.max_colonies, 8, 20000);
  opt.max_candidates_per_ship = std::clamp(mw.max_candidates_per_ship, 1, 64);

  mw.plan = nebula4x::compute_maintenance_plan(sim, mw.faction_id, opt);
  mw.have_plan = true;
  mw.last_day = static_cast<int>(sim.state().date.days_since_epoch());
  mw.last_hour = sim.state().hour_of_day;
}

}  // namespace

void draw_maintenance_planner_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  static MaintenancePlannerWindowState mw;

  auto& st = sim.state();
  std::vector<Id> fids;
  fids.reserve(st.factions.size());
  for (const auto& [fid, _] : st.factions) fids.push_back(fid);
  std::sort(fids.begin(), fids.end());

  if (mw.faction_id == kInvalidId) mw.faction_id = ui.viewer_faction_id;
  if (mw.faction_id == kInvalidId && !fids.empty()) mw.faction_id = fids.front();
  if (st.factions.find(mw.faction_id) == st.factions.end() && !fids.empty()) mw.faction_id = fids.front();

  // --- Controls ---
  {
    const std::string fac_name = [&]() {
      if (const auto* f = find_ptr(st.factions, mw.faction_id)) return f->name;
      return std::string("<none>");
    }();

    if (ImGui::BeginCombo("Faction", fac_name.c_str())) {
      for (Id fid : fids) {
        const auto* f = find_ptr(st.factions, fid);
        if (!f) continue;
        const bool selected = (fid == mw.faction_id);
        if (ImGui::Selectable(f->name.c_str(), selected)) {
          mw.faction_id = fid;
          mw.have_plan = false;
        }
        if (selected) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::Checkbox("Auto-refresh", &mw.auto_refresh);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Recompute the plan when the game time changes");

    ImGui::SameLine();
    if (ImGui::Button("Refresh")) mw.have_plan = false;

    ImGui::Separator();

    bool invalidate = false;
    invalidate |= ImGui::Checkbox("Restrict to discovered systems", &mw.restrict_to_discovered);
    invalidate |= ImGui::Checkbox("Include trade partner colonies", &mw.include_trade_partner_colonies);
    invalidate |= ImGui::Checkbox("Prefer shipyards", &mw.prefer_shipyards);
    invalidate |= ImGui::Checkbox("Require shipyard when critical", &mw.require_shipyard_when_critical);
    invalidate |= ImGui::Checkbox("Require supplies available", &mw.require_supplies_available);

    invalidate |= ImGui::SliderFloat("Threshold", &mw.threshold_fraction, 0.0f, 1.0f, "%.2f");
    invalidate |= ImGui::SliderFloat("Target", &mw.target_fraction, 0.0f, 1.0f, "%.2f");
    invalidate |= ImGui::SliderFloat("Reserve buffer", &mw.reserve_buffer_fraction, 0.0f, 0.95f, "%.2f");

    invalidate |= ImGui::Checkbox("Only idle ships", &mw.require_idle_ships);
    invalidate |= ImGui::Checkbox("Exclude fleet ships", &mw.exclude_fleet_ships);

    invalidate |= ImGui::SliderInt("Max ships", &mw.max_ships, 32, 20000);
    invalidate |= ImGui::SliderInt("Max colonies", &mw.max_colonies, 32, 20000);
    invalidate |= ImGui::SliderInt("Max candidates per ship", &mw.max_candidates_per_ship, 1, 64);

    ImGui::Separator();
    ImGui::TextUnformatted("Apply:");
    ImGui::Checkbox("Clear existing orders", &mw.clear_orders_before_apply);
    ImGui::SameLine();
    ImGui::Checkbox("Smart travel (refuel stops)", &mw.use_smart_travel);

    if (invalidate) mw.have_plan = false;
  }

  // --- Compute if needed ---
  if (!mw.have_plan) {
    compute_plan(mw, sim);
  } else if (mw.auto_refresh) {
    const int day = static_cast<int>(st.date.days_since_epoch());
    const int hour = st.hour_of_day;
    if (day != mw.last_day || hour != mw.last_hour) {
      compute_plan(mw, sim);
    }
  }

  ImGui::Separator();
  if (!mw.plan.message.empty()) ImGui::TextWrapped("%s", mw.plan.message.c_str());
  if (!mw.plan.ok) {
    if (!mw.plan.assignments.empty()) {
      ImGui::TextUnformatted("Some ships could not be planned:");
      for (const auto& a : mw.plan.assignments) {
        if (a.target_colony_id != kInvalidId) continue;
        const auto* sh = find_ptr(st.ships, a.ship_id);
        const std::string nm = sh ? sh->name : ("Ship " + std::to_string(a.ship_id));
        ImGui::BulletText("%s: %s", nm.c_str(), a.note.c_str());
      }
    }
    return;
  }

  // --- Bulk apply ---
  if (ImGui::Button("Apply plan: route all assigned ships")) {
    const bool ok = nebula4x::apply_maintenance_plan(sim, mw.plan, mw.clear_orders_before_apply, mw.use_smart_travel);
    nebula4x::log::info(ok ? "Maintenance Planner: applied routing plan"
                           : "Maintenance Planner: applied plan (with failures)");
    mw.have_plan = false;
  }
  ImGui::SameLine();
  ImGui::TextDisabled("(Ships orbit indefinitely at the destination body)");

  // --- Colonies table ---
  ImGui::Separator();
  const std::string supply_lbl = mw.plan.resource_id.empty() ? std::string("supplies") : mw.plan.resource_id;
  ImGui::Text("Maintenance colonies (%s):", supply_lbl.c_str());

  if (ImGui::BeginTable("maint_cols", 8,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                            ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY,
                        ImVec2(0.0f, 200.0f))) {
    ImGui::TableSetupColumn("Colony");
    ImGui::TableSetupColumn("Shipyard", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableSetupColumn("Owned", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableSetupColumn("Avail", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("Reserved", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("Remain", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("Ships", ImGuiTableColumnFlags_WidthFixed, 55.0f);
    ImGui::TableSetupColumn("Note");
    ImGui::TableHeadersRow();

    for (const auto& c : mw.plan.colonies) {
      const Colony* col = find_ptr(st.colonies, c.colony_id);
      const std::string nm = col ? col->name : ("Colony " + std::to_string(c.colony_id));

      ImGui::TableNextRow();

      ImGui::TableSetColumnIndex(0);
      if (ImGui::SmallButton(("Focus##col" + std::to_string(c.colony_id)).c_str())) {
        focus_colony(c.colony_id, sim, ui, selected_ship, selected_colony, selected_body);
      }
      ImGui::SameLine();
      ImGui::TextUnformatted(nm.c_str());

      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(c.has_shipyard ? "Yes" : "No");

      ImGui::TableSetColumnIndex(2);
      ImGui::TextUnformatted(c.owned_by_faction ? "Yes" : "No");

      ImGui::TableSetColumnIndex(3);
      ImGui::TextUnformatted(fmt_tons(c.available_supplies_tons).c_str());

      ImGui::TableSetColumnIndex(4);
      ImGui::TextUnformatted(fmt_tons(c.reserved_supplies_tons).c_str());

      ImGui::TableSetColumnIndex(5);
      ImGui::TextUnformatted(fmt_tons(c.remaining_supplies_tons).c_str());

      ImGui::TableSetColumnIndex(6);
      ImGui::Text("%d", c.assigned_ship_count);

      ImGui::TableSetColumnIndex(7);
      ImGui::TextUnformatted(c.note.c_str());
    }

    ImGui::EndTable();
  }

  // --- Assignments table ---
  ImGui::Separator();
  ImGui::TextUnformatted("Ship assignments:");
  if (ImGui::BeginTable("maint_asg", 12,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                            ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY,
                        ImVec2(0.0f, 0.0f))) {
    ImGui::TableSetupColumn("Ship");
    ImGui::TableSetupColumn("Maint", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableSetupColumn("Target");
    ImGui::TableSetupColumn("Shipyard", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableSetupColumn("ETA", ImGuiTableColumnFlags_WidthFixed, 65.0f);
    ImGui::TableSetupColumn("Recov", ImGuiTableColumnFlags_WidthFixed, 65.0f);
    ImGui::TableSetupColumn("Finish", ImGuiTableColumnFlags_WidthFixed, 65.0f);
    ImGui::TableSetupColumn("Sup/day", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableSetupColumn("Sup total", ImGuiTableColumnFlags_WidthFixed, 75.0f);
    ImGui::TableSetupColumn("Ship cargo", ImGuiTableColumnFlags_WidthFixed, 75.0f);
    ImGui::TableSetupColumn("Colony", ImGuiTableColumnFlags_WidthFixed, 75.0f);
    ImGui::TableSetupColumn("Note");
    ImGui::TableHeadersRow();

    for (const auto& a : mw.plan.assignments) {
      const Ship* sh = find_ptr(st.ships, a.ship_id);
      const std::string sh_name = sh ? sh->name : ("Ship " + std::to_string(a.ship_id));
      const Colony* col = find_ptr(st.colonies, a.target_colony_id);
      const std::string col_name = (a.target_colony_id == kInvalidId)
                                       ? std::string("<none>")
                                       : (col ? col->name : ("Colony " + std::to_string(a.target_colony_id)));

      ImGui::TableNextRow();

      ImGui::TableSetColumnIndex(0);
      if (ImGui::SmallButton(("Focus##sh" + std::to_string(a.ship_id)).c_str())) {
        focus_ship(a.ship_id, sim, ui, selected_ship, selected_colony, selected_body);
      }
      ImGui::SameLine();
      ImGui::TextUnformatted(sh_name.c_str());

      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(fmt_pct(a.start_condition).c_str());
      if (ImGui::IsItemHovered() && (a.breakdown_p_per_day > 1e-12 || a.breakdown_p_during_travel > 1e-12)) {
        ImGui::BeginTooltip();
        ImGui::Text("Breakdown risk/day: %s", fmt_pct(a.breakdown_p_per_day).c_str());
        ImGui::Text("Risk during travel: %s", fmt_pct(a.breakdown_p_during_travel).c_str());
        ImGui::Text("Rate (lambda/day): %.4g", a.breakdown_rate_per_day);
        ImGui::TextUnformatted("(Failures are suppressed while docked at a shipyard.)");
        ImGui::EndTooltip();
      }

      ImGui::TableSetColumnIndex(2);
      if (a.target_colony_id != kInvalidId) {
        if (ImGui::SmallButton(("Go##c" + std::to_string(a.ship_id)).c_str())) {
          focus_colony(a.target_colony_id, sim, ui, selected_ship, selected_colony, selected_body);
        }
        ImGui::SameLine();
      }
      ImGui::TextUnformatted(col_name.c_str());

      ImGui::TableSetColumnIndex(3);
      ImGui::TextUnformatted(a.target_has_shipyard ? "Yes" : "No");

      ImGui::TableSetColumnIndex(4);
      ImGui::TextUnformatted(fmt_days(a.travel_eta_days).c_str());

      ImGui::TableSetColumnIndex(5);
      ImGui::TextUnformatted(fmt_days(a.maintenance_days).c_str());

      ImGui::TableSetColumnIndex(6);
      ImGui::TextUnformatted(fmt_days(a.finish_days).c_str());

      ImGui::TableSetColumnIndex(7);
      ImGui::TextUnformatted(fmt_tons(a.supplies_per_day_tons).c_str());

      ImGui::TableSetColumnIndex(8);
      ImGui::TextUnformatted(fmt_tons(a.supplies_needed_total_tons).c_str());

      ImGui::TableSetColumnIndex(9);
      ImGui::TextUnformatted(fmt_tons(a.supplies_from_ship_cargo_tons).c_str());

      ImGui::TableSetColumnIndex(10);
      ImGui::TextUnformatted(fmt_tons(a.supplies_from_colony_tons).c_str());

      ImGui::TableSetColumnIndex(11);
      ImGui::TextUnformatted(a.note.c_str());
    }

    ImGui::EndTable();
  }
}

}  // namespace nebula4x::ui

