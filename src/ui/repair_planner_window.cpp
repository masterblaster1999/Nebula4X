#include "ui/repair_planner_window.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "nebula4x/core/repair_planner.h"
#include "nebula4x/core/simulation.h"
#include "nebula4x/util/log.h"
#include "ui/ui_state.h"

namespace nebula4x::ui {
namespace {

struct RepairPlannerWindowState {
  Id faction_id{kInvalidId};

  // Planning knobs.
  bool auto_refresh{true};
  bool restrict_to_discovered{true};
  bool include_trade_partner_yards{true};
  bool include_subsystem_repairs{true};
  bool include_blockade_multiplier{true};
  bool apply_mineral_limits{false};
  bool require_idle_ships{false};
  bool exclude_fleet_ships{false};

  int max_ships{2048};
  int max_yards{512};
  int max_candidates_per_ship{12};

  // Apply knobs.
  bool clear_orders_before_apply{true};
  bool use_smart_travel{true};

  // Cache.
  bool have_plan{false};
  int last_day{-1};
  int last_hour{-1};
  nebula4x::RepairPlannerResult plan;
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

std::string fmt_hp(double hp) {
  if (!std::isfinite(hp)) return "inf";
  char buf[64];
  if (std::abs(hp) < 1000.0) {
    std::snprintf(buf, sizeof(buf), "%.0f", hp);
  } else if (std::abs(hp) < 1000000.0) {
    std::snprintf(buf, sizeof(buf), "%.1fk", hp / 1000.0);
  } else {
    std::snprintf(buf, sizeof(buf), "%.2fM", hp / 1000000.0);
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

void compute_plan(RepairPlannerWindowState& rw, const Simulation& sim) {
  nebula4x::RepairPlannerOptions opt;
  opt.restrict_to_discovered = rw.restrict_to_discovered;
  opt.include_trade_partner_yards = rw.include_trade_partner_yards;
  opt.include_subsystem_repairs = rw.include_subsystem_repairs;
  opt.include_blockade_multiplier = rw.include_blockade_multiplier;
  opt.apply_mineral_limits = rw.apply_mineral_limits;
  opt.require_idle_ships = rw.require_idle_ships;
  opt.exclude_fleet_ships = rw.exclude_fleet_ships;
  opt.max_ships = std::clamp(rw.max_ships, 1, 20000);
  opt.max_yards = std::clamp(rw.max_yards, 1, 5000);
  opt.max_candidates_per_ship = std::clamp(rw.max_candidates_per_ship, 1, 64);

  rw.plan = nebula4x::compute_repair_plan(sim, rw.faction_id, opt);
  rw.have_plan = true;
  rw.last_day = static_cast<int>(sim.state().date.days_since_epoch());
  rw.last_hour = sim.state().hour_of_day;
}

}  // namespace

void draw_repair_planner_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  static RepairPlannerWindowState rw;

  auto& st = sim.state();
  std::vector<Id> fids;
  fids.reserve(st.factions.size());
  for (const auto& [fid, _] : st.factions) fids.push_back(fid);
  std::sort(fids.begin(), fids.end());

  if (rw.faction_id == kInvalidId) rw.faction_id = ui.viewer_faction_id;
  if (rw.faction_id == kInvalidId && !fids.empty()) rw.faction_id = fids.front();
  if (st.factions.find(rw.faction_id) == st.factions.end() && !fids.empty()) rw.faction_id = fids.front();

  // --- Controls ---
  {
    const std::string fac_name = [&]() {
      if (const auto* f = find_ptr(st.factions, rw.faction_id)) return f->name;
      return std::string("<none>");
    }();

    if (ImGui::BeginCombo("Faction", fac_name.c_str())) {
      for (Id fid : fids) {
        const auto* f = find_ptr(st.factions, fid);
        if (!f) continue;
        const bool selected = (fid == rw.faction_id);
        if (ImGui::Selectable(f->name.c_str(), selected)) {
          rw.faction_id = fid;
          rw.have_plan = false;
        }
        if (selected) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::Checkbox("Auto-refresh", &rw.auto_refresh);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Recompute the plan when the game time changes");

    ImGui::SameLine();
    if (ImGui::Button("Refresh")) rw.have_plan = false;

    ImGui::Separator();

    bool invalidate = false;
    invalidate |= ImGui::Checkbox("Restrict to discovered systems", &rw.restrict_to_discovered);
    invalidate |= ImGui::Checkbox("Include trade partner shipyards", &rw.include_trade_partner_yards);
    invalidate |= ImGui::Checkbox("Include subsystem repairs (integrity)", &rw.include_subsystem_repairs);
    invalidate |= ImGui::Checkbox("Scale capacity by blockade pressure", &rw.include_blockade_multiplier);
    invalidate |= ImGui::Checkbox("Cap capacity by minerals", &rw.apply_mineral_limits);
    invalidate |= ImGui::Checkbox("Only idle ships", &rw.require_idle_ships);
    invalidate |= ImGui::Checkbox("Exclude fleet ships", &rw.exclude_fleet_ships);

    invalidate |= ImGui::SliderInt("Max ships", &rw.max_ships, 32, 20000);
    invalidate |= ImGui::SliderInt("Max shipyards", &rw.max_yards, 8, 5000);
    invalidate |= ImGui::SliderInt("Max candidates per ship", &rw.max_candidates_per_ship, 1, 64);

    ImGui::Separator();
    ImGui::TextUnformatted("Apply:");
    ImGui::Checkbox("Clear existing orders", &rw.clear_orders_before_apply);
    ImGui::SameLine();
    ImGui::Checkbox("Smart travel (refuel stops)", &rw.use_smart_travel);

    if (invalidate) rw.have_plan = false;
  }

  // --- Compute if needed ---
  if (!rw.have_plan) {
    compute_plan(rw, sim);
  } else if (rw.auto_refresh) {
    const int day = static_cast<int>(st.date.days_since_epoch());
    const int hour = st.hour_of_day;
    if (day != rw.last_day || hour != rw.last_hour) {
      compute_plan(rw, sim);
    }
  }

  ImGui::Separator();
  ImGui::TextUnformatted(rw.plan.message.c_str());
  if (!rw.plan.ok) {
    if (!rw.plan.message.empty()) ImGui::TextWrapped("%s", rw.plan.message.c_str());
    if (!rw.plan.assignments.empty()) {
      ImGui::TextUnformatted("Some ships could not be planned:");
      for (const auto& a : rw.plan.assignments) {
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
    const bool ok = nebula4x::apply_repair_plan(sim, rw.plan, rw.clear_orders_before_apply, rw.use_smart_travel);
    nebula4x::log::info(ok ? "Repair Planner: applied repair routing plan"
                           : "Repair Planner: applied plan (with failures)");
    rw.have_plan = false;
  }
  ImGui::SameLine();
  ImGui::TextDisabled("(Ships orbit indefinitely at the destination body)");

  // --- Shipyard table ---
  ImGui::Separator();
  ImGui::TextUnformatted("Repair yards (shipyards):");
  if (ImGui::BeginTable("repair_yards", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                            ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY,
                        ImVec2(0.0f, 220.0f))) {
    ImGui::TableSetupColumn("Colony");
    ImGui::TableSetupColumn("Shipyards", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableSetupColumn("Cap/day", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("Backlog", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("Proc", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableSetupColumn("Makespan", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("Util", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableSetupColumn("Go", ImGuiTableColumnFlags_WidthFixed, 40.0f);
    ImGui::TableHeadersRow();

    for (const auto& y : rw.plan.yards) {
      const auto* c = find_ptr(st.colonies, y.colony_id);
      const std::string cname = c ? c->name : ("Colony " + std::to_string(y.colony_id));

      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(cname.c_str());

      ImGui::TableNextColumn();
      ImGui::Text("%d", y.shipyards);

      ImGui::TableNextColumn();
      ImGui::TextUnformatted(fmt_hp(y.effective_capacity_hp_per_day).c_str());

      ImGui::TableNextColumn();
      ImGui::TextUnformatted(fmt_hp(y.backlog_hp_equiv).c_str());

      ImGui::TableNextColumn();
      ImGui::TextUnformatted(fmt_days(y.processing_days).c_str());

      ImGui::TableNextColumn();
      ImGui::TextUnformatted(fmt_days(y.makespan_days).c_str());

      ImGui::TableNextColumn();
      ImGui::TextUnformatted(fmt_pct(y.utilization).c_str());

      ImGui::TableNextColumn();
      ImGui::PushID(static_cast<int>(y.colony_id));
      if (ImGui::SmallButton("Go")) {
        focus_colony(y.colony_id, sim, ui, selected_ship, selected_colony, selected_body);
      }
      ImGui::PopID();
    }

    ImGui::EndTable();
  }

  // --- Ship assignment table ---
  ImGui::Separator();
  ImGui::TextUnformatted("Damaged ships:");
  if (ImGui::BeginTable("repair_ships", 10,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                            ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY,
                        ImVec2(0.0f, 0.0f))) {
    ImGui::TableSetupColumn("Prio", ImGuiTableColumnFlags_WidthFixed, 45.0f);
    ImGui::TableSetupColumn("Ship");
    ImGui::TableSetupColumn("Hull", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableSetupColumn("Subsys", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableSetupColumn("Target Yard");
    ImGui::TableSetupColumn("Travel", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableSetupColumn("Wait", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableSetupColumn("Repair", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableSetupColumn("Finish", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableHeadersRow();

    for (const auto& a : rw.plan.assignments) {
      const auto* sh = find_ptr(st.ships, a.ship_id);
      const std::string sname = sh ? sh->name : ("Ship " + std::to_string(a.ship_id));

      const std::string prio = [&]() {
        switch (a.priority) {
          case RepairPriority::High:
            return std::string("H");
          case RepairPriority::Low:
            return std::string("L");
          default:
            return std::string("N");
        }
      }();

      const std::string yard_name = [&]() {
        if (a.target_colony_id == kInvalidId) return std::string("<none>");
        const auto* c = find_ptr(st.colonies, a.target_colony_id);
        return c ? c->name : ("Colony " + std::to_string(a.target_colony_id));
      }();

      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(prio.c_str());

      ImGui::TableNextColumn();
      ImGui::TextUnformatted(sname.c_str());
      if (ImGui::IsItemHovered() && sh) {
        ImGui::SetTooltip("Ship id %lld", static_cast<long long>(a.ship_id));
      }

      ImGui::TableNextColumn();
      ImGui::TextUnformatted(fmt_hp(a.missing_hull_hp).c_str());

      ImGui::TableNextColumn();
      ImGui::TextUnformatted(fmt_hp(a.missing_subsystem_hp_equiv).c_str());

      ImGui::TableNextColumn();
      ImGui::TextUnformatted(yard_name.c_str());

      ImGui::TableNextColumn();
      ImGui::TextUnformatted(fmt_days(a.travel_eta_days).c_str());

      ImGui::TableNextColumn();
      ImGui::TextUnformatted(fmt_days(a.queue_wait_days).c_str());

      ImGui::TableNextColumn();
      ImGui::TextUnformatted(fmt_days(a.repair_days).c_str());

      ImGui::TableNextColumn();
      ImGui::TextUnformatted(fmt_days(a.finish_repair_days).c_str());

      ImGui::TableNextColumn();
      ImGui::PushID(static_cast<int>(a.ship_id));
      if (ImGui::SmallButton("Select")) {
        focus_ship(a.ship_id, sim, ui, selected_ship, selected_colony, selected_body);
      }
      ImGui::SameLine();
      bool can_send = (a.target_colony_id != kInvalidId);
      if (!can_send) ImGui::BeginDisabled();
      if (ImGui::SmallButton("Send")) {
        const bool ok = nebula4x::apply_repair_assignment(sim, a, rw.clear_orders_before_apply, rw.use_smart_travel);
        nebula4x::log::info(ok ? "Repair Planner: issued repair orders"
                               : "Repair Planner: failed to issue repair orders");
        rw.have_plan = false;
      }
      if (!can_send) ImGui::EndDisabled();
      ImGui::PopID();

      if (!a.note.empty() && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", a.note.c_str());
      }
    }

    ImGui::EndTable();
  }
}

}  // namespace nebula4x::ui
