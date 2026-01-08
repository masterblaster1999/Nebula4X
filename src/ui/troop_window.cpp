#include "ui/troop_window.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "nebula4x/core/troop_planner.h"
#include "nebula4x/util/log.h"

namespace nebula4x::ui {
namespace {

struct TroopWindowState {
  Id faction_id{kInvalidId};

  bool auto_refresh{true};
  bool require_auto_troop{true};
  bool require_idle{true};
  bool restrict_to_discovered{true};
  bool exclude_fleet_ships{true};

  int max_ships{256};

  bool clear_orders_before_apply{false};

  nebula4x::TroopPlannerResult plan;
  bool have_plan{false};
  int last_day{-1};
  int last_hour{-1};
};

TroopWindowState& tw_state() {
  static TroopWindowState st;
  return st;
}

std::string fmt_strength(double v) {
  if (!std::isfinite(v)) return "∞";
  if (v < 0.0) v = 0.0;
  if (std::abs(v - std::llround(v)) < 1e-6) {
    const long long iv = static_cast<long long>(std::llround(v));
    return std::to_string(iv);
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.1f", v);
  return std::string(buf);
}

std::string fmt_eta_days(double days) {
  if (!std::isfinite(days)) return "∞";
  if (days < 0.0) days = 0.0;
  char buf[64];
  if (days < 10.0) {
    std::snprintf(buf, sizeof(buf), "%.2fd", days);
  } else if (days < 100.0) {
    std::snprintf(buf, sizeof(buf), "%.1fd", days);
  } else {
    std::snprintf(buf, sizeof(buf), "%.0fd", days);
  }
  return std::string(buf);
}

std::string fmt_arrival_label(const Simulation& sim, double eta_days) {
  if (!std::isfinite(eta_days)) return "";
  const auto& st = sim.state();
  const int dplus = static_cast<int>(std::ceil(std::max(0.0, eta_days)));
  const Date arrive = st.date.add_days(dplus);
  return "D+" + std::to_string(dplus) + " (" + arrive.to_string() + ")";
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
    ui.request_focus_faction_id = sh->faction_id;
  }
}

void focus_colony(Id colony_id, Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  auto& st = sim.state();
  selected_ship = kInvalidId;
  selected_colony = colony_id;
  selected_body = kInvalidId;

  if (const auto* c = find_ptr(st.colonies, colony_id)) {
    selected_body = c->body_id;
    if (const auto* b = find_ptr(st.bodies, c->body_id)) {
      st.selected_system = b->system_id;
      ui.show_map_window = true;
      ui.request_map_tab = MapTab::System;
      ui.request_focus_faction_id = c->faction_id;
    }
  }
}

void compute_plan(TroopWindowState& tw, const Simulation& sim) {
  nebula4x::TroopPlannerOptions opt;
  opt.require_auto_troop_transport_flag = tw.require_auto_troop;
  opt.require_idle = tw.require_idle;
  opt.restrict_to_discovered = tw.restrict_to_discovered;
  opt.exclude_fleet_ships = tw.exclude_fleet_ships;
  opt.max_ships = std::clamp(tw.max_ships, 1, 4096);

  tw.plan = nebula4x::compute_troop_plan(sim, tw.faction_id, opt);
  tw.have_plan = true;
  tw.last_day = static_cast<int>(sim.state().date.days_since_epoch());
  tw.last_hour = sim.state().hour_of_day;
}

}  // namespace

void draw_troop_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  if (!ui.show_troop_window) return;

  TroopWindowState& tw = tw_state();
  auto& st = sim.state();

  // Default faction selection.
  if (tw.faction_id == kInvalidId) {
    Id fallback = (ui.viewer_faction_id != kInvalidId) ? ui.viewer_faction_id : kInvalidId;
    if (fallback == kInvalidId && selected_ship != kInvalidId) {
      if (const auto* sh = find_ptr(st.ships, selected_ship)) fallback = sh->faction_id;
    }
    if (fallback == kInvalidId && !st.factions.empty()) fallback = st.factions.begin()->first;
    tw.faction_id = fallback;
  }

  if (!ImGui::Begin("Troop Logistics", &ui.show_troop_window)) {
    ImGui::End();
    return;
  }

  // Build faction list.
  std::vector<Id> fids;
  fids.reserve(st.factions.size());
  for (const auto& [fid, _] : st.factions) fids.push_back(fid);
  std::sort(fids.begin(), fids.end());

  if (tw.faction_id == kInvalidId && !fids.empty()) tw.faction_id = fids.front();
  if (st.factions.find(tw.faction_id) == st.factions.end() && !fids.empty()) tw.faction_id = fids.front();

  // --- Controls ---
  {
    const std::string fac_name = [&]() {
      if (const auto* f = find_ptr(st.factions, tw.faction_id)) return f->name;
      return std::string("<none>");
    }();

    if (ImGui::BeginCombo("Faction", fac_name.c_str())) {
      for (Id fid : fids) {
        const auto* f = find_ptr(st.factions, fid);
        if (!f) continue;
        const bool selected = (fid == tw.faction_id);
        if (ImGui::Selectable(f->name.c_str(), selected)) {
          tw.faction_id = fid;
          tw.have_plan = false;
        }
        if (selected) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::Checkbox("Auto-refresh", &tw.auto_refresh);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Recompute the plan when the game time changes");
    }

    ImGui::SameLine();
    if (ImGui::Button("Refresh")) tw.have_plan = false;

    ImGui::Separator();

    if (ImGui::Checkbox("Only ships w/ Auto-troop", &tw.require_auto_troop)) tw.have_plan = false;
    ImGui::SameLine();
    if (ImGui::Checkbox("Only idle ships", &tw.require_idle)) tw.have_plan = false;
    ImGui::SameLine();
    if (ImGui::Checkbox("Restrict to discovered", &tw.restrict_to_discovered)) tw.have_plan = false;

    if (ImGui::Checkbox("Exclude fleet ships", &tw.exclude_fleet_ships)) tw.have_plan = false;

    if (ImGui::SliderInt("Max ships", &tw.max_ships, 1, 1024)) tw.have_plan = false;

    ImGui::Separator();

    ImGui::Checkbox("Clear orders before apply", &tw.clear_orders_before_apply);
  }

  const int day = static_cast<int>(st.date.days_since_epoch());
  const int hour = st.hour_of_day;
  const bool time_changed = (day != tw.last_day || hour != tw.last_hour);

  if (!tw.have_plan || (tw.auto_refresh && time_changed)) {
    compute_plan(tw, sim);
  }

  // --- Plan summary ---
  {
    ImGui::Text("Plan: %s", tw.plan.message.c_str());
    if (tw.plan.truncated) {
      ImGui::SameLine();
      ImGui::TextDisabled("(truncated)");
    }

    double total_strength = 0.0;
    for (const auto& a : tw.plan.assignments) total_strength += std::max(0.0, a.strength);

    ImGui::TextDisabled("Assignments: %d", static_cast<int>(tw.plan.assignments.size()));
    ImGui::SameLine();
    ImGui::TextDisabled("Total strength moved: %s", fmt_strength(total_strength).c_str());

    if (!tw.plan.ok) {
      ImGui::Spacing();
      ImGui::TextDisabled("(No plan available.)");
      ImGui::End();
      return;
    }
  }

  // Apply all.
  if (!tw.plan.assignments.empty()) {
    if (ImGui::Button("Apply all")) {
      const bool ok = nebula4x::apply_troop_plan(sim, tw.plan, tw.clear_orders_before_apply);
      if (!ok) {
        nebula4x::log::warn("Troop Logistics: one or more assignments failed to apply.");
      }
      tw.have_plan = false;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear plan cache")) {
      tw.have_plan = false;
    }
  }

  ImGui::Separator();

  // --- Table ---
  const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                ImGuiTableFlags_ScrollY | ImGuiTableFlags_Hideable;

  const float table_h = ImGui::GetContentRegionAvail().y;
  if (ImGui::BeginTable("##troop_plan", 7, flags, ImVec2(0.0f, table_h))) {
    ImGui::TableSetupColumn("Ship");
    ImGui::TableSetupColumn("From");
    ImGui::TableSetupColumn("To");
    ImGui::TableSetupColumn("Strength");
    ImGui::TableSetupColumn("ETA");
    ImGui::TableSetupColumn("Note");
    ImGui::TableSetupColumn("Action");
    ImGui::TableHeadersRow();

    int row = 0;
    for (const auto& asg : tw.plan.assignments) {
      ImGui::TableNextRow();
      ImGui::PushID(row++);

      // Ship
      ImGui::TableSetColumnIndex(0);
      {
        const std::string ship_name = [&]() {
          if (const auto* sh = find_ptr(st.ships, asg.ship_id)) return sh->name;
          return std::string("<ship>");
        }();
        if (ImGui::Selectable(ship_name.c_str())) {
          focus_ship(asg.ship_id, sim, ui, selected_ship, selected_colony, selected_body);
        }
      }

      // From
      ImGui::TableSetColumnIndex(1);
      {
        std::string from = "(embarked)";
        if (asg.kind == nebula4x::TroopAssignmentKind::PickupAndDeliver) {
          if (const auto* c = find_ptr(st.colonies, asg.source_colony_id)) from = c->name;
        }
        if (ImGui::Selectable(from.c_str())) {
          if (asg.source_colony_id != kInvalidId) {
            focus_colony(asg.source_colony_id, sim, ui, selected_ship, selected_colony, selected_body);
          }
        }
      }

      // To
      ImGui::TableSetColumnIndex(2);
      {
        std::string to = "<dest>";
        if (const auto* c = find_ptr(st.colonies, asg.dest_colony_id)) to = c->name;
        if (ImGui::Selectable(to.c_str())) {
          focus_colony(asg.dest_colony_id, sim, ui, selected_ship, selected_colony, selected_body);
        }
      }

      // Strength
      ImGui::TableSetColumnIndex(3);
      ImGui::TextUnformatted(fmt_strength(asg.strength).c_str());

      // ETA
      ImGui::TableSetColumnIndex(4);
      {
        const std::string eta = fmt_eta_days(asg.eta_total_days);
        ImGui::Text("%s", eta.c_str());
        if (ImGui::IsItemHovered()) {
          ImGui::BeginTooltip();
          ImGui::Text("ETA to source: %s", fmt_eta_days(asg.eta_to_source_days).c_str());
          ImGui::Text("ETA to dest:   %s", fmt_eta_days(asg.eta_to_dest_days).c_str());
          ImGui::Text("ETA total:     %s", fmt_eta_days(asg.eta_total_days).c_str());
          const std::string arrival = fmt_arrival_label(sim, asg.eta_total_days);
          if (!arrival.empty()) ImGui::Text("Arrive: %s", arrival.c_str());
          ImGui::EndTooltip();
        }
      }

      // Note
      ImGui::TableSetColumnIndex(5);
      if (!asg.reason.empty()) {
        ImGui::TextUnformatted(asg.reason.c_str());
      } else {
        ImGui::TextUnformatted(asg.note.c_str());
      }
      if (ImGui::IsItemHovered() && !asg.note.empty() && asg.note != asg.reason) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(asg.note.c_str());
        ImGui::EndTooltip();
      }

      // Action
      ImGui::TableSetColumnIndex(6);
      {
        if (ImGui::SmallButton("Apply")) {
          const bool ok = nebula4x::apply_troop_assignment(sim, asg, tw.clear_orders_before_apply);
          if (!ok) {
            nebula4x::log::warn("Troop Logistics: failed to apply assignment.");
          } else {
            focus_ship(asg.ship_id, sim, ui, selected_ship, selected_colony, selected_body);
          }
          tw.have_plan = false;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Ship")) {
          focus_ship(asg.ship_id, sim, ui, selected_ship, selected_colony, selected_body);
        }
      }

      ImGui::PopID();
    }

    ImGui::EndTable();
  }

  ImGui::End();
}

}  // namespace nebula4x::ui
