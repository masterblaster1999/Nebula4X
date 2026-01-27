#include "ui/mine_window.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nebula4x/core/mine_planner.h"
#include "nebula4x/util/log.h"

namespace nebula4x::ui {
namespace {

struct MineWindowState {
  Id faction_id{kInvalidId};

  // Planning knobs.
  bool auto_refresh{true};
  bool require_auto_mine_flag{true};
  bool exclude_conflicting_automation_flags{true};
  bool require_idle{true};
  bool exclude_fleet_ships{true};
  bool restrict_to_discovered{true};
  bool avoid_hostile_systems{true};
  bool reserve_bodies_targeted_by_existing_orders{true};
  double min_tons{0.0};
  int max_ships{256};
  int max_bodies{256};
  bool clear_orders_before_apply{true};

  // Cached plan.
  bool have_plan{false};
  int last_day{-1};
  int last_hour{-1};
  nebula4x::MinePlannerResult plan;
};

MineWindowState& mw_state() {
  static MineWindowState s;
  return s;
}

double cargo_used_tons(const Ship& sh) {
  double used = 0.0;
  for (const auto& [_, tons] : sh.cargo) used += tons;
  return used;
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

void focus_body(Id body_id, Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  auto& st = sim.state();
  selected_ship = kInvalidId;
  selected_colony = kInvalidId;
  selected_body = body_id;

  if (const auto* b = find_ptr(st.bodies, body_id)) {
    st.selected_system = b->system_id;
    ui.show_map_window = true;
    ui.request_map_tab = MapTab::System;

    // Center the system map on the body location.
    ui.request_system_map_center = true;
    ui.request_system_map_center_system_id = b->system_id;
    ui.request_system_map_center_x_mkm = b->position_mkm.x;
    ui.request_system_map_center_y_mkm = b->position_mkm.y;
  }
}

void compute_plan(MineWindowState& mw, const Simulation& sim) {
  nebula4x::MinePlannerOptions opt;
  opt.require_auto_mine_flag = mw.require_auto_mine_flag;
  opt.exclude_conflicting_automation_flags = mw.exclude_conflicting_automation_flags;
  opt.require_idle = mw.require_idle;
  opt.exclude_fleet_ships = mw.exclude_fleet_ships;
  opt.restrict_to_discovered = mw.restrict_to_discovered;
  opt.avoid_hostile_systems = mw.avoid_hostile_systems;
  opt.reserve_bodies_targeted_by_existing_orders = mw.reserve_bodies_targeted_by_existing_orders;
  opt.min_tons = mw.min_tons;
  opt.max_ships = std::clamp(mw.max_ships, 1, 4096);
  opt.max_bodies = std::clamp(mw.max_bodies, 1, 4096);

  mw.plan = nebula4x::compute_mine_plan(sim, mw.faction_id, opt);
  mw.have_plan = true;
  mw.last_day = static_cast<int>(sim.state().date.days_since_epoch());
  mw.last_hour = sim.state().hour_of_day;
}

}  // namespace

void draw_mine_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  if (!ui.show_mine_window) return;

  MineWindowState& mw = mw_state();
  auto& st = sim.state();

  // Default faction selection.
  if (mw.faction_id == kInvalidId) {
    Id fallback = (ui.viewer_faction_id != kInvalidId) ? ui.viewer_faction_id : kInvalidId;
    if (fallback == kInvalidId && selected_ship != kInvalidId) {
      if (const auto* sh = find_ptr(st.ships, selected_ship)) fallback = sh->faction_id;
    }
    if (fallback == kInvalidId && !st.factions.empty()) fallback = st.factions.begin()->first;
    mw.faction_id = fallback;
  }

  ImGui::SetNextWindowSize(ImVec2(1040, 690), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Mine Planner", &ui.show_mine_window)) {
    ImGui::End();
    return;
  }

  // Build faction list.
  std::vector<Id> fids;
  fids.reserve(st.factions.size());
  for (const auto& [fid, _] : st.factions) fids.push_back(fid);
  std::sort(fids.begin(), fids.end());
  if (mw.faction_id == kInvalidId && !fids.empty()) mw.faction_id = fids.front();
  if (st.factions.find(mw.faction_id) == st.factions.end() && !fids.empty()) mw.faction_id = fids.front();

  // --- Controls row ---
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

    if (ImGui::Checkbox("Only auto-mine ships", &mw.require_auto_mine_flag)) mw.have_plan = false;
    ImGui::SameLine();
    if (ImGui::Checkbox("Exclude conflicting missions", &mw.exclude_conflicting_automation_flags)) mw.have_plan = false;
    ImGui::SameLine();
    if (ImGui::Checkbox("Only idle ships", &mw.require_idle)) mw.have_plan = false;
    ImGui::SameLine();
    if (ImGui::Checkbox("Exclude fleet ships", &mw.exclude_fleet_ships)) mw.have_plan = false;

    if (ImGui::Checkbox("Restrict to discovered", &mw.restrict_to_discovered)) mw.have_plan = false;
    ImGui::SameLine();
    if (ImGui::Checkbox("Avoid hostile systems", &mw.avoid_hostile_systems)) mw.have_plan = false;
    ImGui::SameLine();
    if (ImGui::Checkbox("Reserve already-targeted bodies", &mw.reserve_bodies_targeted_by_existing_orders)) mw.have_plan = false;

    ImGui::PushItemWidth(120.0f);
    if (ImGui::InputInt("Max ships", &mw.max_ships)) mw.have_plan = false;
    ImGui::SameLine();
    if (ImGui::InputInt("Max bodies", &mw.max_bodies)) mw.have_plan = false;
    ImGui::SameLine();
    {
      float min_tons_f = static_cast<float>(mw.min_tons);
      if (ImGui::DragFloat("Min tons", &min_tons_f, 10.0f, 0.0f, 1e9f, "%.0f")) {
        mw.min_tons = std::max(0.0, static_cast<double>(min_tons_f));
        mw.have_plan = false;
      }
    }
    ImGui::PopItemWidth();

    if (ImGui::Checkbox("Clear orders before apply", &mw.clear_orders_before_apply)) {
      // purely apply behavior, no need to recompute
    }
  }

  // Auto-refresh cache invalidation.
  const int day = static_cast<int>(st.date.days_since_epoch());
  const int hour = st.hour_of_day;
  if (mw.auto_refresh && mw.have_plan && (mw.last_day != day || mw.last_hour != hour)) {
    mw.have_plan = false;
  }

  if (!mw.have_plan) {
    compute_plan(mw, sim);
  }

  // --- Summary ---
  {
    if (!mw.plan.message.empty()) {
      ImGui::TextDisabled("%s", mw.plan.message.c_str());
    }
    if (mw.plan.truncated) {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "(truncated)");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The plan hit a max ships/bodies cap. Increase limits if needed.");
      }
    }

    const int total_asg = static_cast<int>(mw.plan.assignments.size());
    int mine_asg = 0;
    int deliver_asg = 0;
    double expected_mined_total = 0.0;
    std::unordered_map<std::string, double> by_mineral;
    by_mineral.reserve(32);

    for (const auto& a : mw.plan.assignments) {
      if (a.kind == nebula4x::MineAssignmentKind::MineAndDeliver) {
        mine_asg++;
        expected_mined_total += std::max(0.0, a.expected_mined_tons);
        const std::string key = a.mineral.empty() ? std::string("(all)") : a.mineral;
        by_mineral[key] += std::max(0.0, a.expected_mined_tons);
      } else {
        deliver_asg++;
      }
    }

    ImGui::Text("Assignments: %d  (Mine: %d, Deliver: %d)", total_asg, mine_asg, deliver_asg);
    ImGui::SameLine();
    ImGui::TextDisabled("Expected mined: %st", fmt_tons(expected_mined_total).c_str());

    if (!by_mineral.empty()) {
      std::vector<std::pair<std::string, double>> v;
      v.reserve(by_mineral.size());
      for (const auto& [k, t] : by_mineral) v.push_back({k, t});
      std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
      });

      std::string top;
      const int show = std::min<int>(static_cast<int>(v.size()), 6);
      for (int i = 0; i < show; ++i) {
        if (i > 0) top += ", ";
        top += v[i].first + " " + fmt_tons(v[i].second) + "t";
      }
      ImGui::TextDisabled("Top minerals: %s", top.c_str());
    }

    if (!mw.plan.ok) {
      ImGui::Spacing();
      ImGui::TextDisabled("(No plan available.)");
      ImGui::End();
      return;
    }
  }

  // Apply all.
  if (!mw.plan.assignments.empty()) {
    if (ImGui::Button("Apply all")) {
      const bool ok = nebula4x::apply_mine_plan(sim, mw.plan, mw.clear_orders_before_apply);
      if (!ok) {
        nebula4x::log::warn("Mine Planner: one or more assignments failed to apply.");
      }
      mw.have_plan = false;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear plan cache")) {
      mw.have_plan = false;
    }
  }

  ImGui::Separator();

  // --- Table ---
  const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                ImGuiTableFlags_ScrollY | ImGuiTableFlags_Hideable;

  const float table_h = ImGui::GetContentRegionAvail().y;
  if (ImGui::BeginTable("##mine_plan", 8, flags, ImVec2(0.0f, table_h))) {
    ImGui::TableSetupColumn("Ship");
    ImGui::TableSetupColumn("Kind");
    ImGui::TableSetupColumn("Target");
    ImGui::TableSetupColumn("Dest");
    ImGui::TableSetupColumn("Mineral");
    ImGui::TableSetupColumn("Tons");
    ImGui::TableSetupColumn("ETA");
    ImGui::TableSetupColumn("Action");
    ImGui::TableHeadersRow();

    int row = 0;
    for (const auto& asg : mw.plan.assignments) {
      ImGui::TableNextRow();
      ImGui::PushID(row++);

      const Ship* sh = find_ptr(st.ships, asg.ship_id);
      const std::string ship_name = sh ? sh->name : std::string("<ship>");

      // Ship.
      ImGui::TableSetColumnIndex(0);
      if (ImGui::Selectable(ship_name.c_str())) {
        focus_ship(asg.ship_id, sim, ui, selected_ship, selected_colony, selected_body);
      }

      // Kind.
      ImGui::TableSetColumnIndex(1);
      {
        const char* k = (asg.kind == nebula4x::MineAssignmentKind::MineAndDeliver) ? "Mine" : "Deliver";
        ImGui::TextUnformatted(k);
      }

      // Target.
      ImGui::TableSetColumnIndex(2);
      {
        std::string tgt = (asg.kind == nebula4x::MineAssignmentKind::MineAndDeliver) ? "<body>" : "(cargo)";
        if (asg.kind == nebula4x::MineAssignmentKind::MineAndDeliver) {
          if (const auto* b = find_ptr(st.bodies, asg.body_id)) tgt = b->name.empty() ? ("Body #" + std::to_string(asg.body_id)) : b->name;
        }
        if (ImGui::Selectable(tgt.c_str())) {
          if (asg.kind == nebula4x::MineAssignmentKind::MineAndDeliver && asg.body_id != kInvalidId) {
            focus_body(asg.body_id, sim, ui, selected_ship, selected_colony, selected_body);
          }
        }
      }

      // Dest.
      ImGui::TableSetColumnIndex(3);
      {
        std::string dest = "-";
        if (const auto* c = find_ptr(st.colonies, asg.dest_colony_id)) dest = c->name;
        if (ImGui::Selectable(dest.c_str())) {
          if (asg.dest_colony_id != kInvalidId) {
            focus_colony(asg.dest_colony_id, sim, ui, selected_ship, selected_colony, selected_body);
          }
        }
      }

      // Mineral.
      ImGui::TableSetColumnIndex(4);
      {
        const std::string m = asg.mineral.empty() ? std::string("(all)") : asg.mineral;
        ImGui::TextUnformatted(m.c_str());
      }

      // Tons.
      ImGui::TableSetColumnIndex(5);
      {
        double tons = 0.0;
        if (asg.kind == nebula4x::MineAssignmentKind::MineAndDeliver) {
          tons = asg.expected_mined_tons;
        } else if (sh) {
          tons = cargo_used_tons(*sh);
        }
        ImGui::Text("%st", fmt_tons(tons).c_str());

        if (asg.kind == nebula4x::MineAssignmentKind::MineAndDeliver && ImGui::IsItemHovered()) {
          ImGui::BeginTooltip();
          ImGui::TextDisabled("Deposit: %st", fmt_tons(asg.deposit_tons).c_str());
          ImGui::TextDisabled("Mine rate: %st/day", fmt_tons(asg.mine_tons_per_day).c_str());
          ImGui::TextDisabled("Mine time: %s", fmt_eta_days(asg.est_mine_days).c_str());
          ImGui::EndTooltip();
        }
      }

      // ETA.
      ImGui::TableSetColumnIndex(6);
      {
        ImGui::TextUnformatted(fmt_eta_days(asg.eta_total_days).c_str());
        if (ImGui::IsItemHovered()) {
          ImGui::BeginTooltip();
          if (asg.kind == nebula4x::MineAssignmentKind::MineAndDeliver) {
            ImGui::TextDisabled("To mine: %s", fmt_eta_days(asg.eta_to_mine_days).c_str());
            ImGui::TextDisabled("Mine:   %s", fmt_eta_days(asg.est_mine_days).c_str());
            ImGui::TextDisabled("To dest:%s", fmt_eta_days(asg.eta_to_dest_days).c_str());
          } else {
            ImGui::TextDisabled("To dest:%s", fmt_eta_days(asg.eta_to_dest_days).c_str());
          }
          const std::string arrive = fmt_arrival_label(sim, asg.eta_total_days);
          if (!arrive.empty()) ImGui::TextDisabled("Arrive: %s", arrive.c_str());
          if (!asg.note.empty()) {
            ImGui::Separator();
            ImGui::TextUnformatted(asg.note.c_str());
          }
          ImGui::EndTooltip();
        }
      }

      // Action.
      ImGui::TableSetColumnIndex(7);
      {
        if (ImGui::SmallButton("Apply")) {
          const bool ok = nebula4x::apply_mine_assignment(sim, asg, mw.clear_orders_before_apply);
          if (!ok) {
            nebula4x::log::warn("Mine Planner: failed to apply assignment for ship {}", static_cast<unsigned long long>(asg.ship_id));
          }
          mw.have_plan = false;
        }
      }

      ImGui::PopID();
    }

    ImGui::EndTable();
  }

  ImGui::End();
}

}  // namespace nebula4x::ui
