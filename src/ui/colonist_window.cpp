#include "ui/colonist_window.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "nebula4x/core/colonist_planner.h"
#include "nebula4x/util/log.h"

namespace nebula4x::ui {
namespace {

struct ColonistWindowState {
  Id faction_id{kInvalidId};

  bool auto_refresh{true};
  bool require_auto_colonist{true};
  bool require_idle{true};
  bool restrict_to_discovered{true};
  bool exclude_fleet_ships{true};

  int max_ships{256};

  bool clear_orders_before_apply{false};

  char assignment_filter[128]{0};

  nebula4x::ColonistPlannerResult plan;
  bool have_plan{false};
  int last_day{-1};
  int last_hour{-1};
};

ColonistWindowState& cw_state() {
  static ColonistWindowState st;
  return st;
}

std::string fmt_millions(double v) {
  if (!std::isfinite(v)) return "∞";
  if (v < 0.0) v = 0.0;
  if (std::abs(v - std::llround(v)) < 1e-6) {
    const long long iv = static_cast<long long>(std::llround(v));
    return std::to_string(iv);
  }
  char buf[64];
  if (v < 10.0) {
    std::snprintf(buf, sizeof(buf), "%.2f", v);
  } else if (v < 100.0) {
    std::snprintf(buf, sizeof(buf), "%.1f", v);
  } else {
    std::snprintf(buf, sizeof(buf), "%.0f", v);
  }
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

bool filter_match(const char* filter, const std::string& a, const std::string& b, const std::string& c) {
  if (!filter || filter[0] == '\0') return true;
  std::string f(filter);
  // Lowercase naive (ASCII) for case-insensitive search.
  for (char& ch : f) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

  auto contains_ci = [&](const std::string& s) {
    std::string t = s;
    for (char& ch : t) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return t.find(f) != std::string::npos;
  };

  return contains_ci(a) || contains_ci(b) || contains_ci(c);
}

void compute_plan(ColonistWindowState& cw, const Simulation& sim) {
  nebula4x::ColonistPlannerOptions opt;
  opt.require_auto_colonist_transport_flag = cw.require_auto_colonist;
  opt.require_idle = cw.require_idle;
  opt.restrict_to_discovered = cw.restrict_to_discovered;
  opt.exclude_fleet_ships = cw.exclude_fleet_ships;
  opt.max_ships = std::clamp(cw.max_ships, 1, 4096);

  cw.plan = nebula4x::compute_colonist_plan(sim, cw.faction_id, opt);
  cw.have_plan = true;
  cw.last_day = static_cast<int>(sim.state().date.days_since_epoch());
  cw.last_hour = sim.state().hour_of_day;
}

}  // namespace

void draw_colonist_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  if (!ui.show_colonist_window) return;

  ColonistWindowState& cw = cw_state();
  auto& st = sim.state();

  // Default faction selection.
  if (cw.faction_id == kInvalidId) {
    Id fallback = (ui.viewer_faction_id != kInvalidId) ? ui.viewer_faction_id : kInvalidId;
    if (fallback == kInvalidId && selected_ship != kInvalidId) {
      if (const auto* sh = find_ptr(st.ships, selected_ship)) fallback = sh->faction_id;
    }
    if (fallback == kInvalidId && !st.factions.empty()) fallback = st.factions.begin()->first;
    cw.faction_id = fallback;
  }

  if (!ImGui::Begin("Population Logistics", &ui.show_colonist_window)) {
    ImGui::End();
    return;
  }

  // Build faction list.
  std::vector<Id> fids;
  fids.reserve(st.factions.size());
  for (const auto& [fid, _] : st.factions) fids.push_back(fid);
  std::sort(fids.begin(), fids.end());

  if (cw.faction_id == kInvalidId && !fids.empty()) cw.faction_id = fids.front();
  if (st.factions.find(cw.faction_id) == st.factions.end() && !fids.empty()) cw.faction_id = fids.front();

  // --- Controls ---
  {
    const std::string fac_name = [&]() {
      if (const auto* f = find_ptr(st.factions, cw.faction_id)) return f->name;
      return std::string("<none>");
    }();

    if (ImGui::BeginCombo("Faction", fac_name.c_str())) {
      for (Id fid : fids) {
        const auto* f = find_ptr(st.factions, fid);
        if (!f) continue;
        const bool selected = (fid == cw.faction_id);
        if (ImGui::Selectable(f->name.c_str(), selected)) {
          cw.faction_id = fid;
          cw.have_plan = false;
        }
        if (selected) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::Checkbox("Auto-refresh", &cw.auto_refresh);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Recompute the plan when the game time changes");
    }

    ImGui::SameLine();
    if (ImGui::Button("Refresh")) cw.have_plan = false;

    ImGui::Separator();

    if (ImGui::Checkbox("Only ships w/ Auto-colonist", &cw.require_auto_colonist)) cw.have_plan = false;
    ImGui::SameLine();
    if (ImGui::Checkbox("Only idle ships", &cw.require_idle)) cw.have_plan = false;
    ImGui::SameLine();
    if (ImGui::Checkbox("Restrict to discovered", &cw.restrict_to_discovered)) cw.have_plan = false;

    if (ImGui::Checkbox("Exclude fleet ships", &cw.exclude_fleet_ships)) cw.have_plan = false;

    if (ImGui::SliderInt("Max ships", &cw.max_ships, 1, 1024)) cw.have_plan = false;

    ImGui::Separator();

    ImGui::Checkbox("Clear orders before apply", &cw.clear_orders_before_apply);

    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##pop_plan_filter", "Filter (ship/source/dest)", cw.assignment_filter,
                             IM_ARRAYSIZE(cw.assignment_filter));
  }

  // Helpful rule summary.
  if (ImGui::CollapsingHeader("Planner rules", ImGuiTreeNodeFlags_DefaultOpen)) {
    const auto& cfg = sim.cfg();
    ImGui::BulletText("Min transfer: %.2f M", std::max(0.0, cfg.auto_colonist_min_transfer_millions));
    ImGui::BulletText("Max take fraction of surplus: %.2f",
                      std::clamp(cfg.auto_colonist_max_take_fraction_of_surplus, 0.0, 1.0));
    ImGui::BulletText("Require source floor (target/reserve) to export: %s",
                      cfg.auto_colonist_require_source_floor ? "Yes" : "No");
  }

  const int day = static_cast<int>(st.date.days_since_epoch());
  const int hour = st.hour_of_day;
  const bool time_changed = (day != cw.last_day || hour != cw.last_hour);

  if (!cw.have_plan || (cw.auto_refresh && time_changed)) {
    compute_plan(cw, sim);
  }

  // --- Plan summary ---
  {
    ImGui::Text("Plan: %s", cw.plan.message.c_str());
    if (cw.plan.truncated) {
      ImGui::SameLine();
      ImGui::TextDisabled("(truncated)");
    }

    double total_m = 0.0;
    for (const auto& a : cw.plan.assignments) total_m += std::max(0.0, a.millions);

    ImGui::TextDisabled("Assignments: %d", static_cast<int>(cw.plan.assignments.size()));
    ImGui::SameLine();
    ImGui::TextDisabled("Total moved: %s M", fmt_millions(total_m).c_str());

    if (!cw.plan.ok) {
      ImGui::Spacing();
      ImGui::TextDisabled("(No plan available.)");
      ImGui::End();
      return;
    }
  }

  // Apply all.
  if (!cw.plan.assignments.empty()) {
    if (ImGui::Button("Apply all")) {
      const bool ok = nebula4x::apply_colonist_plan(sim, cw.plan, cw.clear_orders_before_apply);
      if (!ok) {
        nebula4x::log::warn("Population Logistics: one or more assignments failed to apply.");
      }
      cw.have_plan = false;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear plan cache")) {
      cw.have_plan = false;
    }
  }

  // Colony status table (quick-edit targets/reserves).
  if (ImGui::CollapsingHeader("Colony targets & reserves", ImGuiTreeNodeFlags_DefaultOpen)) {
    struct Row {
      Id colony_id{kInvalidId};
      std::string name;
      double pop{0.0};
      double target{0.0};
      double reserve{0.0};
      double deficit{0.0};
      double surplus{0.0};
    };

    std::vector<Row> rows;
    rows.reserve(st.colonies.size());
    const auto& cfg = sim.cfg();
    for (const auto& [cid, c] : st.colonies) {
      if (c.faction_id != cw.faction_id) continue;
      Row r;
      r.colony_id = cid;
      r.name = c.name;
      r.pop = std::max(0.0, c.population_millions);
      r.target = std::max(0.0, c.population_target_millions);
      r.reserve = std::max(0.0, c.population_reserve_millions);
      r.deficit = std::max(0.0, r.target - r.pop);
      const double floor = std::max(r.target, r.reserve);
      const bool allow_export = (floor > 1e-9) || !cfg.auto_colonist_require_source_floor;
      if (allow_export) {
        r.surplus = std::max(0.0, r.pop - floor);
      } else {
        r.surplus = 0.0;
      }
      rows.push_back(std::move(r));
    }

    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
      if (a.deficit > b.deficit + 1e-9) return true;
      if (b.deficit > a.deficit + 1e-9) return false;
      if (a.surplus > b.surplus + 1e-9) return true;
      if (b.surplus > a.surplus + 1e-9) return false;
      return a.colony_id < b.colony_id;
    });

    const ImGuiTableFlags tflags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_Hideable;
    const float th = std::min(ImGui::GetContentRegionAvail().y * 0.35f, ImGui::GetTextLineHeightWithSpacing() * 12.0f);
    if (ImGui::BeginTable("##pop_colonies", 7, tflags, ImVec2(0.0f, th))) {
      ImGui::TableSetupColumn("Colony", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Pop (M)", ImGuiTableColumnFlags_WidthFixed, 70.0f);
      ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthFixed, 70.0f);
      ImGui::TableSetupColumn("Reserve", ImGuiTableColumnFlags_WidthFixed, 70.0f);
      ImGui::TableSetupColumn("Deficit", ImGuiTableColumnFlags_WidthFixed, 70.0f);
      ImGui::TableSetupColumn("Surplus", ImGuiTableColumnFlags_WidthFixed, 70.0f);
      ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 90.0f);
      ImGui::TableHeadersRow();

      int row = 0;
      for (const auto& r : rows) {
        Colony* col = find_ptr(st.colonies, r.colony_id);
        if (!col) continue;

        ImGui::TableNextRow();
        ImGui::PushID(row++);

        ImGui::TableSetColumnIndex(0);
        if (ImGui::Selectable(r.name.c_str())) {
          focus_colony(r.colony_id, sim, ui, selected_ship, selected_colony, selected_body);
        }

        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(fmt_millions(r.pop).c_str());

        ImGui::TableSetColumnIndex(2);
        {
          double v = std::max(0.0, col->population_target_millions);
          ImGui::SetNextItemWidth(-FLT_MIN);
          if (ImGui::InputDouble("##target", &v, 0.0, 0.0, "%.2f")) {
            col->population_target_millions = std::max(0.0, v);
            cw.have_plan = false;
          }
        }

        ImGui::TableSetColumnIndex(3);
        {
          double v = std::max(0.0, col->population_reserve_millions);
          ImGui::SetNextItemWidth(-FLT_MIN);
          if (ImGui::InputDouble("##reserve", &v, 0.0, 0.0, "%.2f")) {
            col->population_reserve_millions = std::max(0.0, v);
            cw.have_plan = false;
          }
        }

        ImGui::TableSetColumnIndex(4);
        ImGui::TextUnformatted(fmt_millions(r.deficit).c_str());

        ImGui::TableSetColumnIndex(5);
        ImGui::TextUnformatted(fmt_millions(r.surplus).c_str());

        ImGui::TableSetColumnIndex(6);
        if (ImGui::SmallButton("Focus")) {
          focus_colony(r.colony_id, sim, ui, selected_ship, selected_colony, selected_body);
        }

        ImGui::PopID();
      }

      ImGui::EndTable();
    }
  }

  ImGui::Separator();

  // --- Assignments table ---
  const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                ImGuiTableFlags_ScrollY | ImGuiTableFlags_Hideable;

  const float table_h = ImGui::GetContentRegionAvail().y;
  if (ImGui::BeginTable("##pop_plan", 7, flags, ImVec2(0.0f, table_h))) {
    ImGui::TableSetupColumn("Ship");
    ImGui::TableSetupColumn("From");
    ImGui::TableSetupColumn("To");
    ImGui::TableSetupColumn("Millions");
    ImGui::TableSetupColumn("ETA");
    ImGui::TableSetupColumn("Note");
    ImGui::TableSetupColumn("Action");
    ImGui::TableHeadersRow();

    int row = 0;
    for (const auto& asg : cw.plan.assignments) {
      const std::string ship_name = [&]() {
        if (const auto* sh = find_ptr(st.ships, asg.ship_id)) return sh->name;
        return std::string("<ship>");
      }();

      std::string from = "(embarked)";
      if (asg.kind == nebula4x::ColonistAssignmentKind::PickupAndDeliver) {
        if (const auto* c = find_ptr(st.colonies, asg.source_colony_id)) from = c->name;
      }

      std::string to = "<dest>";
      if (const auto* c = find_ptr(st.colonies, asg.dest_colony_id)) to = c->name;

      if (!filter_match(cw.assignment_filter, ship_name, from, to)) continue;

      ImGui::TableNextRow();
      ImGui::PushID(row++);

      // Ship
      ImGui::TableSetColumnIndex(0);
      if (ImGui::Selectable(ship_name.c_str())) {
        focus_ship(asg.ship_id, sim, ui, selected_ship, selected_colony, selected_body);
      }

      // From
      ImGui::TableSetColumnIndex(1);
      if (ImGui::Selectable(from.c_str())) {
        if (asg.source_colony_id != kInvalidId) {
          focus_colony(asg.source_colony_id, sim, ui, selected_ship, selected_colony, selected_body);
        }
      }

      // To
      ImGui::TableSetColumnIndex(2);
      if (ImGui::Selectable(to.c_str())) {
        focus_colony(asg.dest_colony_id, sim, ui, selected_ship, selected_colony, selected_body);
      }

      // Millions
      ImGui::TableSetColumnIndex(3);
      ImGui::TextUnformatted(fmt_millions(asg.millions).c_str());

      // ETA
      ImGui::TableSetColumnIndex(4);
      {
        const std::string eta = fmt_eta_days(asg.eta_total_days);
        ImGui::Text("%s", eta.c_str());
        if (ImGui::IsItemHovered()) {
          ImGui::BeginTooltip();
          if (asg.kind == nebula4x::ColonistAssignmentKind::PickupAndDeliver) {
            ImGui::Text("ETA to source: %s", fmt_eta_days(asg.eta_to_source_days).c_str());
          }
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
      if (ImGui::SmallButton("Apply")) {
        const bool ok = nebula4x::apply_colonist_assignment(sim, asg, cw.clear_orders_before_apply);
        if (!ok) {
          nebula4x::log::warn("Population Logistics: failed to apply assignment.");
        } else {
          focus_ship(asg.ship_id, sim, ui, selected_ship, selected_colony, selected_body);
        }
        cw.have_plan = false;
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Ship")) {
        focus_ship(asg.ship_id, sim, ui, selected_ship, selected_colony, selected_body);
      }

      ImGui::PopID();
    }

    ImGui::EndTable();
  }

  ImGui::End();
}

}  // namespace nebula4x::ui
