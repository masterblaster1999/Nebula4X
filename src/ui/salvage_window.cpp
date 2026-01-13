#include "ui/salvage_window.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "nebula4x/core/salvage_planner.h"
#include "nebula4x/util/log.h"

namespace nebula4x::ui {
namespace {

struct SalvageWindowState {
  Id faction_id{kInvalidId};

  // Planning knobs.
  bool auto_refresh{true};
  bool require_idle{true};
  bool exclude_fleet_ships{true};
  bool restrict_to_discovered{true};
  bool avoid_hostile_systems{true};
  int max_ships{256};
  int max_wrecks{256};
  bool clear_orders_before_apply{true};

  // Cached plan.
  bool have_plan{false};
  int last_day{-1};
  int last_hour{-1};
  nebula4x::SalvagePlannerResult plan;
};

SalvageWindowState& sw_state() {
  static SalvageWindowState s;
  return s;
}

std::string fmt_tons(double tons) {
  if (!std::isfinite(tons)) return "?";
  if (std::abs(tons - std::round(tons)) < 1e-6) {
    const long long v = static_cast<long long>(std::llround(tons));
    return std::to_string(v);
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.1f", tons);
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

void focus_wreck(Id wreck_id, Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  auto& st = sim.state();
  selected_ship = kInvalidId;
  selected_colony = kInvalidId;
  selected_body = kInvalidId;

  const auto* w = find_ptr(st.wrecks, wreck_id);
  if (!w) return;
  if (w->system_id == kInvalidId) return;

  st.selected_system = w->system_id;
  ui.show_map_window = true;
  ui.request_map_tab = MapTab::System;

  // Center the system map on the wreck location.
  ui.request_system_map_center = true;
  ui.request_system_map_center_system_id = w->system_id;
  ui.request_system_map_center_x_mkm = w->position_mkm.x;
  ui.request_system_map_center_y_mkm = w->position_mkm.y;
  // Leave zoom unchanged (0 means "don't override").
}

void compute_plan(SalvageWindowState& sw, const Simulation& sim) {
  nebula4x::SalvagePlannerOptions opt;
  opt.require_idle = sw.require_idle;
  opt.exclude_fleet_ships = sw.exclude_fleet_ships;
  opt.restrict_to_discovered = sw.restrict_to_discovered;
  opt.avoid_hostile_systems = sw.avoid_hostile_systems;
  opt.max_ships = std::clamp(sw.max_ships, 1, 4096);
  opt.max_wrecks = std::clamp(sw.max_wrecks, 1, 4096);

  sw.plan = nebula4x::compute_salvage_plan(sim, sw.faction_id, opt);
  sw.have_plan = true;
  sw.last_day = static_cast<int>(sim.state().date.days_since_epoch());
  sw.last_hour = sim.state().hour_of_day;
}

}  // namespace

void draw_salvage_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  if (!ui.show_salvage_window) return;

  SalvageWindowState& sw = sw_state();
  auto& st = sim.state();

  // Default faction selection.
  if (sw.faction_id == kInvalidId) {
    Id fallback = (ui.viewer_faction_id != kInvalidId) ? ui.viewer_faction_id : kInvalidId;
    if (fallback == kInvalidId && selected_ship != kInvalidId) {
      if (const auto* sh = find_ptr(st.ships, selected_ship)) fallback = sh->faction_id;
    }
    if (fallback == kInvalidId && !st.factions.empty()) fallback = st.factions.begin()->first;
    sw.faction_id = fallback;
  }

  ImGui::SetNextWindowSize(ImVec2(980, 680), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Salvage Planner", &ui.show_salvage_window)) {
    ImGui::End();
    return;
  }

  // Build faction list.
  std::vector<Id> fids;
  fids.reserve(st.factions.size());
  for (const auto& [fid, _] : st.factions) fids.push_back(fid);
  std::sort(fids.begin(), fids.end());
  if (sw.faction_id == kInvalidId && !fids.empty()) sw.faction_id = fids.front();
  if (st.factions.find(sw.faction_id) == st.factions.end() && !fids.empty()) sw.faction_id = fids.front();

  // --- Controls row ---
  {
    const std::string fac_name = [&]() {
      if (const auto* f = find_ptr(st.factions, sw.faction_id)) return f->name;
      return std::string("<none>");
    }();

    if (ImGui::BeginCombo("Faction", fac_name.c_str())) {
      for (Id fid : fids) {
        const auto* f = find_ptr(st.factions, fid);
        if (!f) continue;
        const bool selected = (fid == sw.faction_id);
        if (ImGui::Selectable(f->name.c_str(), selected)) {
          sw.faction_id = fid;
          sw.have_plan = false;
        }
        if (selected) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::Checkbox("Auto-refresh", &sw.auto_refresh);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Recompute the plan when the game time changes");

    ImGui::SameLine();
    if (ImGui::Button("Refresh")) sw.have_plan = false;

    ImGui::Separator();

    if (ImGui::Checkbox("Only idle ships", &sw.require_idle)) sw.have_plan = false;
    ImGui::SameLine();
    if (ImGui::Checkbox("Exclude fleet ships", &sw.exclude_fleet_ships)) sw.have_plan = false;
    ImGui::SameLine();
    if (ImGui::Checkbox("Restrict to discovered", &sw.restrict_to_discovered)) sw.have_plan = false;
    ImGui::SameLine();
    if (ImGui::Checkbox("Avoid hostile systems", &sw.avoid_hostile_systems)) sw.have_plan = false;

    ImGui::PushItemWidth(120.0f);
    if (ImGui::InputInt("Max ships", &sw.max_ships)) sw.have_plan = false;
    ImGui::SameLine();
    if (ImGui::InputInt("Max wrecks", &sw.max_wrecks)) sw.have_plan = false;
    ImGui::PopItemWidth();

    if (ImGui::Checkbox("Clear ship orders before apply", &sw.clear_orders_before_apply)) {
      // no-op
    }
  }

  // Auto-refresh when time changes.
  if (sw.auto_refresh) {
    const int now_day = static_cast<int>(st.date.days_since_epoch());
    const int now_hour = st.hour_of_day;
    if (sw.have_plan && (now_day != sw.last_day || now_hour != sw.last_hour)) {
      sw.have_plan = false;
    }
  }

  if (!sw.have_plan) {
    compute_plan(sw, sim);
  }

  // --- Plan summary ---
  {
    ImGui::Text("Plan: %s", sw.plan.message.c_str());
    if (sw.plan.truncated) {
      ImGui::SameLine();
      ImGui::TextDisabled("(truncated)");
    }
    ImGui::TextDisabled("Assignments: %d", static_cast<int>(sw.plan.assignments.size()));
    if (!sw.plan.ok) {
      ImGui::Spacing();
      ImGui::TextDisabled("(No plan available.)");
      ImGui::End();
      return;
    }
  }

  // Apply all.
  if (!sw.plan.assignments.empty()) {
    if (ImGui::Button("Apply all")) {
      const bool ok = nebula4x::apply_salvage_plan(sim, sw.plan, sw.clear_orders_before_apply);
      if (!ok) {
        nebula4x::log::warn("Salvage Planner: one or more assignments failed to apply.");
      }
      sw.have_plan = false;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear plan cache")) sw.have_plan = false;
  }

  ImGui::Separator();

  // --- Table ---
  const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                ImGuiTableFlags_ScrollY | ImGuiTableFlags_Hideable;

  const float table_h = ImGui::GetContentRegionAvail().y;
  if (ImGui::BeginTable("##salvage_plan", 8, flags, ImVec2(0.0f, table_h))) {
    ImGui::TableSetupColumn("Ship");
    ImGui::TableSetupColumn("Task", ImGuiTableColumnFlags_WidthFixed, 110.0f);
    ImGui::TableSetupColumn("Wreck");
    ImGui::TableSetupColumn("To");
    ImGui::TableSetupColumn("Load", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("ETA", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("Note");
    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 130.0f);
    ImGui::TableHeadersRow();

    int row = 0;
    for (const auto& asg : sw.plan.assignments) {
      ImGui::TableNextRow();
      ImGui::PushID(row++);

      // Ship
      ImGui::TableSetColumnIndex(0);
      const std::string ship_name = [&]() {
        if (const auto* sh = find_ptr(st.ships, asg.ship_id)) return sh->name;
        return std::string("<ship>");
      }();
      if (ImGui::Selectable(ship_name.c_str())) {
        focus_ship(asg.ship_id, sim, ui, selected_ship, selected_colony, selected_body);
      }

      // Task
      ImGui::TableSetColumnIndex(1);
      const char* task = (asg.kind == nebula4x::SalvageAssignmentKind::DeliverCargo) ? "Deliver" : "Salvage";
      ImGui::TextUnformatted(task);

      // Wreck
      ImGui::TableSetColumnIndex(2);
      {
        std::string wname = "(n/a)";
        if (asg.kind == nebula4x::SalvageAssignmentKind::SalvageAndDeliver) {
          if (const auto* w = find_ptr(st.wrecks, asg.wreck_id)) wname = w->name;
          else wname = "<wreck>";
        }

        if (ImGui::Selectable(wname.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
          if (asg.wreck_id != kInvalidId) focus_wreck(asg.wreck_id, sim, ui, selected_ship, selected_colony, selected_body);
        }
        if (ImGui::IsItemHovered() && asg.kind == nebula4x::SalvageAssignmentKind::SalvageAndDeliver) {
          ImGui::BeginTooltip();
          ImGui::Text("Expected load: %s t", fmt_tons(asg.expected_salvage_tons).c_str());
          ImGui::Text("Wreck total:   %s t", fmt_tons(asg.wreck_total_tons).c_str());
          ImGui::Text("Est salvage:   %s", fmt_eta_days(asg.est_salvage_days).c_str());
          ImGui::EndTooltip();
        }
      }

      // To colony
      ImGui::TableSetColumnIndex(3);
      {
        std::string to = "<none>";
        if (asg.dest_colony_id != kInvalidId) {
          if (const auto* c = find_ptr(st.colonies, asg.dest_colony_id)) to = c->name;
        }
        if (ImGui::Selectable(to.c_str())) {
          if (asg.dest_colony_id != kInvalidId) {
            focus_colony(asg.dest_colony_id, sim, ui, selected_ship, selected_colony, selected_body);
          }
        }
      }

      // Load
      ImGui::TableSetColumnIndex(4);
      if (asg.kind == nebula4x::SalvageAssignmentKind::DeliverCargo) {
        ImGui::TextUnformatted("(cargo)");
      } else {
        ImGui::Text("%s t", fmt_tons(asg.expected_salvage_tons).c_str());
      }

      // ETA
      ImGui::TableSetColumnIndex(5);
      {
        const std::string eta = fmt_eta_days(asg.eta_total_days);
        ImGui::TextUnformatted(eta.c_str());
        if (ImGui::IsItemHovered()) {
          ImGui::BeginTooltip();
          if (asg.kind == nebula4x::SalvageAssignmentKind::SalvageAndDeliver) {
            ImGui::Text("ETA to wreck: %s", fmt_eta_days(asg.eta_to_wreck_days).c_str());
            ImGui::Text("Salvage time: %s", fmt_eta_days(asg.est_salvage_days).c_str());
          }
          ImGui::Text("ETA to dest:  %s", fmt_eta_days(asg.eta_to_dest_days).c_str());
          ImGui::Text("ETA total:    %s", fmt_eta_days(asg.eta_total_days).c_str());
          const std::string arrival = fmt_arrival_label(sim, asg.eta_total_days);
          if (!arrival.empty()) ImGui::Text("Arrive: %s", arrival.c_str());
          ImGui::EndTooltip();
        }
      }

      // Note
      ImGui::TableSetColumnIndex(6);
      ImGui::TextUnformatted(asg.note.c_str());

      // Action
      ImGui::TableSetColumnIndex(7);
      {
        if (ImGui::SmallButton("Apply")) {
          const bool ok = nebula4x::apply_salvage_assignment(sim, asg, sw.clear_orders_before_apply);
          if (!ok) {
            nebula4x::log::warn("Salvage Planner: failed to apply assignment.");
          } else {
            focus_ship(asg.ship_id, sim, ui, selected_ship, selected_colony, selected_body);
          }
          sw.have_plan = false;
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
