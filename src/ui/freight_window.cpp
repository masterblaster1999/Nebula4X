#include "ui/freight_window.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "nebula4x/core/freight_planner.h"
#include "nebula4x/util/log.h"

namespace nebula4x::ui {
namespace {

struct FreightWindowState {
  Id faction_id{kInvalidId};

  // Planning knobs.
  bool auto_refresh{true};
  bool require_auto_freight{true};
  bool require_idle{true};
  bool restrict_to_discovered{true};

  bool override_bundle_multi{false};
  bool bundle_multi{true};

  int max_ships{256};
  bool clear_orders_before_apply{true};

  // Cached plan.
  bool have_plan{false};
  int last_day{-1};
  int last_hour{-1};
  nebula4x::FreightPlannerResult plan;
};

FreightWindowState& fw_state() {
  static FreightWindowState s;
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

std::string fmt_items_short(const std::vector<nebula4x::FreightPlanItem>& items) {
  std::string out;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i) out += ", ";
    out += items[i].mineral;
    out += " ";
    out += fmt_tons(items[i].tons);
  }
  return out;
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

void compute_plan(FreightWindowState& fw, const Simulation& sim) {
  nebula4x::FreightPlannerOptions opt;
  opt.require_auto_freight_flag = fw.require_auto_freight;
  opt.require_idle = fw.require_idle;
  opt.restrict_to_discovered = fw.restrict_to_discovered;
  opt.max_ships = std::clamp(fw.max_ships, 1, 4096);
  if (fw.override_bundle_multi) {
    opt.bundle_multi_mineral = fw.bundle_multi;
  }

  fw.plan = nebula4x::compute_freight_plan(sim, fw.faction_id, opt);
  fw.have_plan = true;
  fw.last_day = static_cast<int>(sim.state().date.days_since_epoch());
  fw.last_hour = sim.state().hour_of_day;
}

}  // namespace

void draw_freight_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  if (!ui.show_freight_window) return;

  FreightWindowState& fw = fw_state();
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

  if (!ImGui::Begin("Freight Planner", &ui.show_freight_window)) {
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

  // --- Controls row ---
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

    if (ImGui::Checkbox("Only ships w/ Auto-freight", &fw.require_auto_freight)) fw.have_plan = false;
    ImGui::SameLine();
    if (ImGui::Checkbox("Only idle ships", &fw.require_idle)) fw.have_plan = false;
    ImGui::SameLine();
    if (ImGui::Checkbox("Restrict to discovered", &fw.restrict_to_discovered)) fw.have_plan = false;

    if (ImGui::Checkbox("Override bundle-multi", &fw.override_bundle_multi)) fw.have_plan = false;
    if (fw.override_bundle_multi) {
      ImGui::SameLine();
      if (ImGui::Checkbox("Bundle multiple minerals", &fw.bundle_multi)) fw.have_plan = false;
    }

    if (ImGui::SliderInt("Max ships", &fw.max_ships, 1, 1024)) fw.have_plan = false;

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
    ImGui::TextDisabled("Assignments: %d", static_cast<int>(fw.plan.assignments.size()));

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
      const bool ok = nebula4x::apply_freight_plan(sim, fw.plan, fw.clear_orders_before_apply);
      if (!ok) {
        nebula4x::log::warn("Freight Planner: one or more assignments failed to apply.");
      }
      // Recompute next frame after state mutation.
      fw.have_plan = false;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear plan cache")) {
      fw.have_plan = false;
    }
  }

  ImGui::Separator();

  // --- Table ---
  const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                ImGuiTableFlags_ScrollY | ImGuiTableFlags_Hideable;

  const float table_h = ImGui::GetContentRegionAvail().y;
  if (ImGui::BeginTable("##freight_plan", 7, flags, ImVec2(0.0f, table_h))) {
    ImGui::TableSetupColumn("Ship");
    ImGui::TableSetupColumn("From");
    ImGui::TableSetupColumn("To");
    ImGui::TableSetupColumn("Cargo");
    ImGui::TableSetupColumn("ETA");
    ImGui::TableSetupColumn("Note");
    ImGui::TableSetupColumn("Action");
    ImGui::TableHeadersRow();

    int row = 0;
    for (const auto& asg : fw.plan.assignments) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);

      const std::string ship_name = [&]() {
        if (const auto* sh = find_ptr(st.ships, asg.ship_id)) return sh->name;
        return std::string("<ship>");
      }();

      ImGui::PushID(row++);

      if (ImGui::Selectable(ship_name.c_str())) {
        focus_ship(asg.ship_id, sim, ui, selected_ship, selected_colony, selected_body);
      }

      ImGui::TableSetColumnIndex(1);
      {
        std::string from = "(cargo)";
        if (asg.kind == nebula4x::FreightAssignmentKind::PickupAndDeliver) {
          if (const auto* c = find_ptr(st.colonies, asg.source_colony_id)) from = c->name;
        }
        if (ImGui::Selectable(from.c_str())) {
          if (asg.source_colony_id != kInvalidId) {
            focus_colony(asg.source_colony_id, sim, ui, selected_ship, selected_colony, selected_body);
          }
        }
      }

      ImGui::TableSetColumnIndex(2);
      {
        std::string to = "<dest>";
        if (const auto* c = find_ptr(st.colonies, asg.dest_colony_id)) to = c->name;
        if (ImGui::Selectable(to.c_str())) {
          focus_colony(asg.dest_colony_id, sim, ui, selected_ship, selected_colony, selected_body);
        }
      }

      ImGui::TableSetColumnIndex(3);
      {
        const std::string cargo = fmt_items_short(asg.items);
        ImGui::TextUnformatted(cargo.c_str());
        if (ImGui::IsItemHovered()) {
          ImGui::BeginTooltip();
          ImGui::TextUnformatted("Items:");
          for (const auto& it : asg.items) {
            if (!it.reason.empty()) {
              ImGui::BulletText("%s: %s  (%s)", it.mineral.c_str(), fmt_tons(it.tons).c_str(), it.reason.c_str());
            } else {
              ImGui::BulletText("%s: %s", it.mineral.c_str(), fmt_tons(it.tons).c_str());
            }
          }
          ImGui::EndTooltip();
        }
      }

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

      ImGui::TableSetColumnIndex(5);
      ImGui::TextUnformatted(asg.note.c_str());

      ImGui::TableSetColumnIndex(6);
      {
        if (ImGui::SmallButton("Apply")) {
          const bool ok = nebula4x::apply_freight_assignment(sim, asg, fw.clear_orders_before_apply);
          if (!ok) {
            nebula4x::log::warn("Freight Planner: failed to apply assignment.");
          } else {
            // Focus the ship after applying to make it easy to see the queue.
            focus_ship(asg.ship_id, sim, ui, selected_ship, selected_colony, selected_body);
          }
          fw.have_plan = false;  // plan is stale after mutation
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
