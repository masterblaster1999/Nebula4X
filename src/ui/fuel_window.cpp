#include "ui/fuel_window.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "nebula4x/core/fuel_planner.h"
#include "nebula4x/util/log.h"

namespace nebula4x::ui {
namespace {

struct FuelWindowState {
  Id faction_id{kInvalidId};

  // Planning knobs.
  bool auto_refresh{true};
  bool require_auto_tanker{true};
  bool require_idle{true};
  bool restrict_to_discovered{true};
  bool exclude_fleet_ships{true};
  bool exclude_auto_refuel_targets{true};

  int max_targets{4096};
  int max_tankers{256};
  int max_legs_per_tanker{4};
  bool clear_orders_before_apply{true};

  // Cached plan.
  bool have_plan{false};
  int last_day{-1};
  int last_hour{-1};
  nebula4x::FuelPlannerResult plan;
};

FuelWindowState& fuel_state() {
  static FuelWindowState s;
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

void compute_plan(FuelWindowState& fw, const Simulation& sim) {
  nebula4x::FuelPlannerOptions opt;
  opt.require_auto_tanker_flag = fw.require_auto_tanker;
  opt.require_idle = fw.require_idle;
  opt.restrict_to_discovered = fw.restrict_to_discovered;
  opt.exclude_fleet_ships = fw.exclude_fleet_ships;
  opt.exclude_ships_with_auto_refuel = fw.exclude_auto_refuel_targets;
  opt.max_targets = std::clamp(fw.max_targets, 1, 20000);
  opt.max_tankers = std::clamp(fw.max_tankers, 1, 4096);
  opt.max_legs_per_tanker = std::clamp(fw.max_legs_per_tanker, 1, 32);

  fw.plan = nebula4x::compute_fuel_plan(sim, fw.faction_id, opt);
  fw.have_plan = true;
  fw.last_day = static_cast<int>(sim.state().date.days_since_epoch());
  fw.last_hour = sim.state().hour_of_day;
}

}  // namespace

void draw_fuel_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  if (!ui.show_fuel_window) return;

  FuelWindowState& fw = fuel_state();
  auto& st = sim.state();

  // Default faction selection.
  if (fw.faction_id == kInvalidId) {
    // Prefer UI's viewer faction (if set), then the selected ship's faction, then any faction.
    Id fallback = (ui.viewer_faction_id != kInvalidId) ? ui.viewer_faction_id : kInvalidId;
    if (fallback == kInvalidId && selected_ship != kInvalidId) {
      if (const auto* sh = find_ptr(st.ships, selected_ship)) {
        fallback = sh->faction_id;
      }
    }
    if (fallback == kInvalidId && !st.factions.empty()) {
      fallback = st.factions.begin()->first;
    }
    fw.faction_id = fallback;
  }

  if (!ImGui::Begin("Fuel Planner", &ui.show_fuel_window)) {
    ImGui::End();
    return;
  }

  // Build faction list.
  std::vector<Id> fids;
  fids.reserve(st.factions.size());
  for (const auto& [fid, _] : st.factions) fids.push_back(fid);
  std::sort(fids.begin(), fids.end());

  if (fw.faction_id == kInvalidId && !fids.empty()) fw.faction_id = fids.front();
  if (st.factions.find(fw.faction_id) == st.factions.end() && !fids.empty()) fw.faction_id = fids.front();

  // --- Controls ---
  {
    const std::string fac_name = [&]() {
      if (const auto* f = find_ptr(st.factions, fw.faction_id)) return f->name;
      return std::string("<none>");
    }();

    if (ImGui::BeginCombo("Faction", fac_name.c_str())) {
      for (Id fid : fids) {
        const auto* f = find_ptr(st.factions, fid);
        if (!f) continue;
        const bool selected = (fid == fw.faction_id);
        if (ImGui::Selectable(f->name.c_str(), selected)) {
          fw.faction_id = fid;
          fw.have_plan = false;
        }
        if (selected) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }

    ImGui::SameLine();
    if (ImGui::Checkbox("Auto-refresh", &fw.auto_refresh)) {
      // no-op
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Recompute the plan when the game time changes");
    }

    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
      fw.have_plan = false;
    }

    ImGui::Separator();

    if (ImGui::Checkbox("Only tankers w/ Auto-tanker", &fw.require_auto_tanker)) fw.have_plan = false;
    ImGui::SameLine();
    if (ImGui::Checkbox("Only idle ships", &fw.require_idle)) fw.have_plan = false;
    ImGui::SameLine();
    if (ImGui::Checkbox("Restrict to discovered", &fw.restrict_to_discovered)) fw.have_plan = false;

    if (ImGui::Checkbox("Exclude fleet ships", &fw.exclude_fleet_ships)) fw.have_plan = false;
    ImGui::SameLine();
    if (ImGui::Checkbox("Exclude targets w/ Auto-refuel", &fw.exclude_auto_refuel_targets)) fw.have_plan = false;

    if (ImGui::SliderInt("Max targets", &fw.max_targets, 1, 10000)) fw.have_plan = false;
    if (ImGui::SliderInt("Max tankers", &fw.max_tankers, 1, 1024)) fw.have_plan = false;
    if (ImGui::SliderInt("Max stops / tanker", &fw.max_legs_per_tanker, 1, 16)) fw.have_plan = false;

    ImGui::Separator();

    if (ImGui::Checkbox("Clear orders before apply", &fw.clear_orders_before_apply)) {
      // no-op
    }
  }

  const int day = static_cast<int>(st.date.days_since_epoch());
  const int hour = st.hour_of_day;
  const bool time_changed = (day != fw.last_day || hour != fw.last_hour);

  if (!fw.have_plan || (fw.auto_refresh && time_changed)) {
    compute_plan(fw, sim);
  }

  // --- Plan summary ---
  {
    ImGui::Text("Plan: %s", fw.plan.message.c_str());
    if (fw.plan.truncated) {
      ImGui::SameLine();
      ImGui::TextDisabled("(truncated)");
    }
    ImGui::TextDisabled("Tankers: %d", static_cast<int>(fw.plan.assignments.size()));

    if (!fw.plan.ok) {
      ImGui::Spacing();
      ImGui::TextDisabled("(No plan available.)");
      ImGui::End();
      return;
    }
  }

  // Apply all.
  if (!fw.plan.assignments.empty()) {
    if (ImGui::Button("Apply all")) {
      const bool ok = nebula4x::apply_fuel_plan(sim, fw.plan, fw.clear_orders_before_apply);
      if (!ok) {
        nebula4x::log::warn("Fuel Planner: one or more assignments failed to apply.");
      }
      fw.have_plan = false;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear plan cache")) {
      fw.have_plan = false;
    }
  }

  ImGui::Separator();

  // --- Per-tanker routes ---
  int tanker_idx = 0;
  for (const auto& asg : fw.plan.assignments) {
    const auto* tanker = find_ptr(st.ships, asg.tanker_ship_id);
    const std::string tanker_name = tanker ? tanker->name : (std::string("<ship ") + std::to_string(asg.tanker_ship_id) + ">");

    std::string header = tanker_name;
    header += "  (" + std::to_string(asg.legs.size()) + " stop" + (asg.legs.size() == 1 ? "" : "s");
    header += ", " + fmt_tons(asg.fuel_transfer_total_tons) + "t";
    header += ", ETA " + fmt_eta_days(asg.eta_total_days) + ")";

    ImGui::PushID(tanker_idx++);
    if (ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::TextDisabled("Tanker fuel: %s / %s t (reserve %s, available %s)",
                          fmt_tons(asg.tanker_fuel_before_tons).c_str(), fmt_tons(asg.tanker_fuel_capacity_tons).c_str(),
                          fmt_tons(asg.tanker_fuel_reserved_tons).c_str(), fmt_tons(asg.tanker_fuel_available_tons).c_str());

      if (ImGui::SmallButton("Apply route")) {
        const bool ok = nebula4x::apply_fuel_assignment(sim, asg, fw.clear_orders_before_apply);
        if (!ok) {
          nebula4x::log::warn("Fuel Planner: failed to apply tanker route.");
        } else {
          focus_ship(asg.tanker_ship_id, sim, ui, selected_ship, selected_colony, selected_body);
        }
        fw.have_plan = false;
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Focus tanker")) {
        focus_ship(asg.tanker_ship_id, sim, ui, selected_ship, selected_colony, selected_body);
      }

      ImGui::Spacing();

      const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                    ImGuiTableFlags_ScrollY | ImGuiTableFlags_Hideable;

      const float table_h = std::min(220.0f, ImGui::GetContentRegionAvail().y);
      if (ImGui::BeginTable("##fuel_legs", 7, flags, ImVec2(0.0f, table_h))) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 28.0f);
        ImGui::TableSetupColumn("Target");
        ImGui::TableSetupColumn("Fuel");
        ImGui::TableSetupColumn("Tons", ImGuiTableColumnFlags_WidthFixed, 64.0f);
        ImGui::TableSetupColumn("ETA leg", ImGuiTableColumnFlags_WidthFixed, 64.0f);
        ImGui::TableSetupColumn("ETA total", ImGuiTableColumnFlags_WidthFixed, 76.0f);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableHeadersRow();

        double eta_cum = 0.0;
        int stop = 0;
        for (const auto& leg : asg.legs) {
          ++stop;
          eta_cum += leg.eta_days;

          const auto* target = find_ptr(st.ships, leg.target_ship_id);
          const std::string target_name = target ? target->name : (std::string("<ship ") + std::to_string(leg.target_ship_id) + ">");

          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          ImGui::Text("%d", stop);

          ImGui::TableSetColumnIndex(1);
          if (ImGui::Selectable(target_name.c_str())) {
            focus_ship(leg.target_ship_id, sim, ui, selected_ship, selected_colony, selected_body);
          }

          ImGui::TableSetColumnIndex(2);
          {
            const std::string fuel = fmt_pct(leg.target_fuel_frac_before) + " -> " + fmt_pct(leg.target_fuel_frac_after);
            ImGui::TextUnformatted(fuel.c_str());
          }

          ImGui::TableSetColumnIndex(3);
          ImGui::TextUnformatted(fmt_tons(leg.tons).c_str());

          ImGui::TableSetColumnIndex(4);
          ImGui::TextUnformatted(fmt_eta_days(leg.eta_days).c_str());

          ImGui::TableSetColumnIndex(5);
          {
            const std::string eta = fmt_eta_days(eta_cum);
            ImGui::TextUnformatted(eta.c_str());
            if (ImGui::IsItemHovered()) {
              const std::string arrival = fmt_arrival_label(sim, eta_cum);
              if (!arrival.empty()) {
                ImGui::BeginTooltip();
                ImGui::Text("Arrive: %s", arrival.c_str());
                ImGui::EndTooltip();
              }
            }
          }

          ImGui::TableSetColumnIndex(6);
          {
            ImGui::PushID(stop);
            if (ImGui::SmallButton("Apply")) {
              if (fw.clear_orders_before_apply) {
                sim.clear_orders(asg.tanker_ship_id);
              }
              const bool ok = sim.issue_transfer_fuel_to_ship(asg.tanker_ship_id, leg.target_ship_id, leg.tons,
                                                             asg.restrict_to_discovered);
              if (!ok) {
                nebula4x::log::warn("Fuel Planner: failed to apply transfer leg.");
              } else {
                focus_ship(asg.tanker_ship_id, sim, ui, selected_ship, selected_colony, selected_body);
              }
              fw.have_plan = false;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Tanker")) {
              focus_ship(asg.tanker_ship_id, sim, ui, selected_ship, selected_colony, selected_body);
            }
            ImGui::PopID();
          }
        }

        ImGui::EndTable();
      }
    }
    ImGui::PopID();

    ImGui::Separator();
  }

  ImGui::End();
}

}  // namespace nebula4x::ui
