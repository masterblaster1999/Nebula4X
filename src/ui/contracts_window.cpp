#include "ui/contracts_window.h"

#include "ui/imgui_includes.h"

#include "nebula4x/core/contract_planner.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace nebula4x::ui {
namespace {

const char* contract_kind_label(ContractKind k) {
  switch (k) {
    case ContractKind::InvestigateAnomaly: return "Investigate Anomaly";
    case ContractKind::SalvageWreck: return "Salvage Wreck";
    case ContractKind::SurveyJumpPoint: return "Survey Jump Point";
  }
  return "(Unknown)";
}

const char* contract_status_label(ContractStatus s) {
  switch (s) {
    case ContractStatus::Offered: return "Offered";
    case ContractStatus::Accepted: return "Accepted";
    case ContractStatus::Completed: return "Completed";
    case ContractStatus::Expired: return "Expired";
    case ContractStatus::Failed: return "Failed";
  }
  return "(Unknown)";
}

std::string system_label(const GameState& st, Id sys_id) {
  if (sys_id == kInvalidId) return "(None)";
  if (const auto* sys = find_ptr(st.systems, sys_id)) {
    if (!sys->name.empty()) return sys->name;
  }
  return "System " + std::to_string((unsigned long long)sys_id);
}

std::string ship_label(const GameState& st, Id ship_id) {
  if (ship_id == kInvalidId) return "(None)";
  if (const auto* sh = find_ptr(st.ships, ship_id)) {
    if (!sh->name.empty()) return sh->name;
  }
  return "Ship " + std::to_string((unsigned long long)ship_id);
}

std::string contract_target_label(const GameState& st, const Contract& c) {
  if (c.target_id == kInvalidId) return "(None)";
  switch (c.kind) {
    case ContractKind::InvestigateAnomaly: {
      if (const auto* a = find_ptr(st.anomalies, c.target_id)) {
        const std::string nm = a->name.empty() ? (std::string("Anomaly ") + std::to_string((unsigned long long)a->id))
                                               : a->name;
        return nm;
      }
      return "Anomaly " + std::to_string((unsigned long long)c.target_id);
    }
    case ContractKind::SalvageWreck: {
      return "Wreck " + std::to_string((unsigned long long)c.target_id);
    }
    case ContractKind::SurveyJumpPoint: {
      const auto* jp = find_ptr(st.jump_points, c.target_id);
      if (!jp) return "JumpPoint " + std::to_string((unsigned long long)c.target_id);
      const auto* other = find_ptr(st.jump_points, jp->linked_jump_id);
      if (!other) return "JumpPoint " + std::to_string((unsigned long long)c.target_id);
      return "Exit to " + system_label(st, other->system_id);
    }
  }
  return "(Unknown)";
}

bool contract_target_pos(const GameState& st, const Contract& c, Id* out_sys, Vec2* out_pos) {
  if (out_sys) *out_sys = kInvalidId;
  if (out_pos) *out_pos = Vec2{0.0, 0.0};
  if (c.target_id == kInvalidId) return false;

  switch (c.kind) {
    case ContractKind::InvestigateAnomaly: {
      const auto* a = find_ptr(st.anomalies, c.target_id);
      if (!a) return false;
      if (out_sys) *out_sys = a->system_id;
      if (out_pos) *out_pos = a->position_mkm;
      return a->system_id != kInvalidId;
    }
    case ContractKind::SalvageWreck: {
      const auto* w = find_ptr(st.wrecks, c.target_id);
      if (!w) return false;
      if (out_sys) *out_sys = w->system_id;
      if (out_pos) *out_pos = w->position_mkm;
      return w->system_id != kInvalidId;
    }
    case ContractKind::SurveyJumpPoint: {
      const auto* jp = find_ptr(st.jump_points, c.target_id);
      if (!jp) return false;
      if (out_sys) *out_sys = jp->system_id;
      if (out_pos) *out_pos = jp->position_mkm;
      return jp->system_id != kInvalidId;
    }
  }
  return false;
}

bool is_ship_idle(const GameState& st, Id ship_id) {
  const auto it = st.ship_orders.find(ship_id);
  if (it == st.ship_orders.end()) return true;
  const ShipOrders& so = it->second;
  if (!so.queue.empty()) return false;
  if (so.repeat && !so.repeat_template.empty() && so.repeat_count_remaining != 0) return false;
  return true;
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
  if (!std::isfinite(eta_days)) return {};
  const auto& st = sim.state();
  const int dplus = static_cast<int>(std::ceil(std::max(0.0, eta_days)));
  const Date arrive = st.date.add_days(dplus);
  return "D+" + std::to_string(dplus) + " (" + arrive.to_string() + ")";
}

void focus_contract_target(const Contract& c, Simulation& sim, UIState& ui) {
  auto& st = sim.state();
  Id sys_id = kInvalidId;
  Vec2 pos{0.0, 0.0};
  if (!contract_target_pos(st, c, &sys_id, &pos)) return;
  if (sys_id == kInvalidId) return;

  st.selected_system = sys_id;
  ui.show_map_window = true;
  ui.request_map_tab = MapTab::System;

  ui.request_system_map_center = true;
  ui.request_system_map_center_system_id = sys_id;
  ui.request_system_map_center_x_mkm = pos.x;
  ui.request_system_map_center_y_mkm = pos.y;
}

void focus_ship(Id ship_id, Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  auto& st = sim.state();
  selected_ship = ship_id;
  selected_colony = kInvalidId;
  selected_body = kInvalidId;

  const auto* sh = find_ptr(st.ships, ship_id);
  if (!sh) return;

  st.selected_system = sh->system_id;
  ui.show_map_window = true;
  ui.request_map_tab = MapTab::System;
  ui.show_details_window = true;
  ui.request_details_tab = DetailsTab::Ship;
}

struct ContractsWindowState {
  Id selected_contract{kInvalidId};
  bool show_offered{true};
  bool show_accepted{true};
  bool show_completed{false};
  bool show_expired{false};
  bool show_failed{false};

  bool clear_orders_on_assign{true};
  bool restrict_to_discovered{true};
  Id assign_ship{kInvalidId};

  // --- Auto planner (multi-contract ship assignment) ---
  bool planner_auto_refresh{false};
  bool planner_require_idle{true};
  bool planner_exclude_fleet_ships{true};
  bool planner_avoid_hostile_systems{true};
  bool planner_include_offered{true};
  bool planner_include_accepted_unassigned{true};
  bool planner_include_already_assigned{false};
  bool planner_clear_orders_before_apply{true};

  int planner_max_ships{256};
  int planner_max_contracts{64};
  float planner_risk_penalty{0.35f};
  float planner_hop_overhead_days{0.25f};

  bool planner_have_plan{false};
  int planner_last_day{-1};
  int planner_last_hour{-1};
  nebula4x::ContractPlannerResult planner_plan;
  std::string planner_last_message;

  std::string last_error;
};

ContractsWindowState& win_state() {
  static ContractsWindowState s;
  return s;
}

bool status_enabled(const ContractsWindowState& ws, ContractStatus st) {
  switch (st) {
    case ContractStatus::Offered: return ws.show_offered;
    case ContractStatus::Accepted: return ws.show_accepted;
    case ContractStatus::Completed: return ws.show_completed;
    case ContractStatus::Expired: return ws.show_expired;
    case ContractStatus::Failed: return ws.show_failed;
  }
  return true;
}

} // namespace

void draw_contracts_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  auto& ws = win_state();
  auto& st = sim.state();

  if (!ui.show_contracts_window) return;

  if (!ImGui::Begin("Contracts", &ui.show_contracts_window)) {
    ImGui::End();
    return;
  }

  const Id fid = ui.viewer_faction_id;
  const auto* fac = (fid != kInvalidId) ? find_ptr(st.factions, fid) : nullptr;
  if (!fac) {
    ImGui::TextDisabled("No viewer faction selected.");
    ImGui::End();
    return;
  }

  ImGui::Text("Faction: %s", fac->name.c_str());
  ImGui::SameLine();
  ImGui::TextDisabled("(Contracts: %zu)", st.contracts.size());

  auto recompute_planner = [&]() {
    nebula4x::ContractPlannerOptions opt;
    opt.require_idle = ws.planner_require_idle;
    opt.exclude_fleet_ships = ws.planner_exclude_fleet_ships;
    opt.restrict_to_discovered = ws.restrict_to_discovered;
    opt.avoid_hostile_systems = ws.planner_avoid_hostile_systems;
    opt.include_offered = ws.planner_include_offered;
    opt.include_accepted_unassigned = ws.planner_include_accepted_unassigned;
    opt.include_already_assigned = ws.planner_include_already_assigned;
    opt.clear_orders_before_apply = ws.planner_clear_orders_before_apply;
    opt.max_ships = ws.planner_max_ships;
    opt.max_contracts = ws.planner_max_contracts;
    opt.risk_penalty = std::max(0.0, (double)ws.planner_risk_penalty);
    opt.hop_overhead_days = std::max(0.0, (double)ws.planner_hop_overhead_days);

    ws.planner_plan = nebula4x::compute_contract_plan(sim, fid, opt);
    ws.planner_have_plan = true;
    ws.planner_last_day = (int)st.date.days_since_epoch();
    ws.planner_last_hour = st.hour_of_day;
    ws.planner_last_message = ws.planner_plan.message;
  };

  // Optional: keep the planner preview fresh as time advances.
  if (ws.planner_auto_refresh && ws.planner_have_plan) {
    const int cur_day = (int)st.date.days_since_epoch();
    const int cur_hour = st.hour_of_day;
    if (cur_day != ws.planner_last_day || cur_hour != ws.planner_last_hour) {
      recompute_planner();
    }
  }

  // Status filters.
  ImGui::Checkbox("Offered", &ws.show_offered);
  ImGui::SameLine();
  ImGui::Checkbox("Accepted", &ws.show_accepted);
  ImGui::SameLine();
  ImGui::Checkbox("Completed", &ws.show_completed);
  ImGui::SameLine();
  ImGui::Checkbox("Expired", &ws.show_expired);
  ImGui::SameLine();
  ImGui::Checkbox("Failed", &ws.show_failed);

  // Ensure at least one filter is enabled.
  if (!ws.show_offered && !ws.show_accepted && !ws.show_completed && !ws.show_expired && !ws.show_failed) {
    ws.show_offered = true;
    ws.show_accepted = true;
  }

  std::vector<Id> contract_ids;
  contract_ids.reserve(st.contracts.size());
  for (const auto& [cid, c] : st.contracts) {
    if (c.assignee_faction_id != fid) continue;
    if (!status_enabled(ws, c.status)) continue;
    contract_ids.push_back(cid);
  }

  // Sort: newest-first by offered day, then id.
  std::sort(contract_ids.begin(), contract_ids.end(), [&](Id a, Id b) {
    const auto* ca = find_ptr(st.contracts, a);
    const auto* cb = find_ptr(st.contracts, b);
    if (!ca || !cb) return a < b;
    if (ca->offered_day != cb->offered_day) return ca->offered_day > cb->offered_day;
    return a < b;
  });

  ImGui::Separator();

  ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
  if (ImGui::BeginTable("contracts_table", 7, flags, ImVec2(0.0f, 320.0f))) {
    ImGui::TableSetupColumn("Name");
    ImGui::TableSetupColumn("Kind");
    ImGui::TableSetupColumn("Status");
    ImGui::TableSetupColumn("Reward (RP)", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("Risk", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableSetupColumn("Hops", ImGuiTableColumnFlags_WidthFixed, 55.0f);
    ImGui::TableSetupColumn("Assigned");
    ImGui::TableHeadersRow();

    for (Id cid : contract_ids) {
      const auto* c = find_ptr(st.contracts, cid);
      if (!c) continue;

      ImGui::TableNextRow();

      ImGui::TableNextColumn();
      const bool selected = (ws.selected_contract == cid);
      const std::string row_label = c->name.empty() ? (std::string("Contract ") + std::to_string((unsigned long long)cid))
                                                    : c->name;
      if (ImGui::Selectable(row_label.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
        ws.selected_contract = cid;
        ws.last_error.clear();
      }

      ImGui::TableNextColumn();
      ImGui::TextUnformatted(contract_kind_label(c->kind));

      ImGui::TableNextColumn();
      ImGui::TextUnformatted(contract_status_label(c->status));

      ImGui::TableNextColumn();
      ImGui::Text("%.0f", std::max(0.0, c->reward_research_points));

      ImGui::TableNextColumn();
      ImGui::Text("%.2f", std::clamp(c->risk_estimate, 0.0, 1.0));

      ImGui::TableNextColumn();
      ImGui::Text("%d", std::max(0, c->hops_estimate));

      ImGui::TableNextColumn();
      if (c->assigned_ship_id != kInvalidId) {
        const std::string sh = ship_label(st, c->assigned_ship_id);
        ImGui::TextUnformatted(sh.c_str());
      } else {
        ImGui::TextDisabled("(Unassigned)");
      }
    }

    ImGui::EndTable();
  } else {
    ImGui::TextDisabled("No contracts match the current filters.");
  }

  ImGui::Separator();

  Contract* c = (ws.selected_contract != kInvalidId) ? find_ptr(st.contracts, ws.selected_contract) : nullptr;
  if (!c) {
    ImGui::TextDisabled("Select a contract to see details and actions.");
    ImGui::End();
    return;
  }

  ImGui::Text("%s", c->name.empty() ? "(Unnamed Contract)" : c->name.c_str());
  ImGui::TextDisabled("ID: %llu", (unsigned long long)c->id);
  ImGui::Text("Kind: %s", contract_kind_label(c->kind));
  ImGui::Text("Status: %s", contract_status_label(c->status));
  ImGui::Text("Target: %s", contract_target_label(st, *c).c_str());
  ImGui::Text("System: %s", system_label(st, c->system_id).c_str());
  ImGui::Text("Reward: %.0f RP", std::max(0.0, c->reward_research_points));
  ImGui::Text("Risk: %.2f   Hops: %d", std::clamp(c->risk_estimate, 0.0, 1.0), std::max(0, c->hops_estimate));

  ImGui::TextDisabled("Offered day: %lld", (long long)c->offered_day);
  if (c->status == ContractStatus::Offered && c->expires_day > 0) {
    ImGui::SameLine();
    ImGui::TextDisabled("(expires day: %lld)", (long long)c->expires_day);
  }
  if (c->status == ContractStatus::Accepted) {
    ImGui::TextDisabled("Accepted day: %lld", (long long)c->accepted_day);
  }
  if (c->status == ContractStatus::Completed || c->status == ContractStatus::Expired || c->status == ContractStatus::Failed) {
    ImGui::TextDisabled("Resolved day: %lld", (long long)c->resolved_day);
  }

  if (!ws.last_error.empty()) {
    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", ws.last_error.c_str());
  }

  // Actions.
  if (ImGui::Button("Focus Target")) {
    focus_contract_target(*c, sim, ui);
  }
  ImGui::SameLine();
  if (c->assigned_ship_id != kInvalidId) {
    if (ImGui::Button("Focus Assigned Ship")) {
      focus_ship(c->assigned_ship_id, sim, ui, selected_ship, selected_colony, selected_body);
    }
  } else {
    ImGui::TextDisabled("(No assigned ship)");
  }

  // Accept/abandon.
  if (c->status == ContractStatus::Offered) {
    if (ImGui::Button("Accept")) {
      ws.last_error.clear();
      std::string err;
      if (!sim.accept_contract(c->id, /*push_event=*/true, &err)) ws.last_error = err.empty() ? "Failed to accept contract." : err;
    }
  } else if (c->status == ContractStatus::Accepted) {
    if (ImGui::Button("Abandon")) {
      ws.last_error.clear();
      std::string err;
      if (!sim.abandon_contract(c->id, /*push_event=*/true, &err)) ws.last_error = err.empty() ? "Failed to abandon contract." : err;
    }
  }

  // Assignment.
  ImGui::Separator();
  ImGui::Text("Assign to ship");

  // Choose a ship.
  std::vector<Id> ship_ids;
  ship_ids.reserve(st.ships.size());
  for (const auto& [sid, sh] : st.ships) {
    if (sh.faction_id != fid) continue;
    ship_ids.push_back(sid);
  }
  std::sort(ship_ids.begin(), ship_ids.end());

  if (ws.assign_ship == kInvalidId && !ship_ids.empty()) ws.assign_ship = ship_ids.front();

  const std::string cur_ship = ship_label(st, ws.assign_ship);
  if (ImGui::BeginCombo("Ship", cur_ship.c_str())) {
    for (Id sid : ship_ids) {
      const bool is_sel = (sid == ws.assign_ship);
      const std::string nm = ship_label(st, sid);
      const bool idle = is_ship_idle(st, sid);
      const std::string label = nm + (idle ? "" : "  [busy]");
      if (ImGui::Selectable(label.c_str(), is_sel)) ws.assign_ship = sid;
      if (is_sel) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }

  ImGui::Checkbox("Clear existing orders", &ws.clear_orders_on_assign);
  ImGui::SameLine();
  ImGui::Checkbox("Restrict to surveyed routes", &ws.restrict_to_discovered);

  // Show a best-effort ETA preview for the currently selected ship.
  {
    Id target_sys = kInvalidId;
    Vec2 target_pos{0.0, 0.0};
    if (ws.assign_ship != kInvalidId && contract_target_pos(st, *c, &target_sys, &target_pos) && target_sys != kInvalidId) {
      const bool include_queued_jumps = !ws.clear_orders_on_assign;
      const auto plan = sim.plan_jump_route_for_ship_to_pos(ws.assign_ship, target_sys, target_pos,
                                                           ws.restrict_to_discovered, include_queued_jumps);
      if (plan) {
        const std::string eta = fmt_eta_days(plan->total_eta_days);
        const std::string arr = fmt_arrival_label(sim, plan->total_eta_days);
        ImGui::TextDisabled("ETA: %s  %s", eta.c_str(), arr.c_str());
      } else {
        ImGui::TextDisabled("ETA: (no route)");
      }
    }
  }

  bool can_assign = (ws.assign_ship != kInvalidId) && (c->status == ContractStatus::Offered || c->status == ContractStatus::Accepted);
  if (!can_assign) {
    ImGui::BeginDisabled();
  }
  if (ImGui::Button("Assign Contract")) {
    ws.last_error.clear();
    std::string err;
    if (!sim.assign_contract_to_ship(c->id, ws.assign_ship, ws.clear_orders_on_assign,
                                     ws.restrict_to_discovered, /*push_event=*/true, &err)) {
      ws.last_error = err.empty() ? "Failed to assign contract." : err;
    }
  }
  if (!can_assign) {
    ImGui::EndDisabled();
  }

  ImGui::SameLine();
  if (c->assigned_ship_id != kInvalidId) {
    if (ImGui::Button("Clear Assignment")) {
      ws.last_error.clear();
      std::string err;
      if (!sim.clear_contract_assignment(c->id, &err)) ws.last_error = err.empty() ? "Failed to clear assignment." : err;
    }
  }

  // --- Auto planner ---
  ImGui::Separator();
  if (ImGui::CollapsingHeader("Auto Planner (Assign Multiple Contracts)", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::Checkbox("Auto refresh", &ws.planner_auto_refresh);
    ImGui::SameLine();
    ImGui::Checkbox("Require idle ships", &ws.planner_require_idle);
    ImGui::SameLine();
    ImGui::Checkbox("Exclude fleet ships", &ws.planner_exclude_fleet_ships);

    ImGui::Checkbox("Avoid hostile systems", &ws.planner_avoid_hostile_systems);

    ImGui::Checkbox("Include Offered", &ws.planner_include_offered);
    ImGui::SameLine();
    ImGui::Checkbox("Include Accepted (unassigned)", &ws.planner_include_accepted_unassigned);
    ImGui::SameLine();
    ImGui::Checkbox("Include Already Assigned", &ws.planner_include_already_assigned);

    ImGui::Checkbox("Clear orders before apply", &ws.planner_clear_orders_before_apply);

    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt("Max ships", &ws.planner_max_ships);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt("Max contracts", &ws.planner_max_contracts);

    ImGui::SetNextItemWidth(200.0f);
    ImGui::SliderFloat("Risk penalty", &ws.planner_risk_penalty, 0.0f, 1.0f, "%.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    ImGui::SliderFloat("Hop overhead (days)", &ws.planner_hop_overhead_days, 0.0f, 2.0f, "%.2f");

    if (ImGui::Button("Compute Plan")) {
      ws.last_error.clear();
      recompute_planner();
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear Plan")) {
      ws.planner_have_plan = false;
      ws.planner_plan = {};
      ws.planner_last_message.clear();
    }

    if (ws.planner_have_plan) {
      if (!ws.planner_last_message.empty()) {
        ImGui::TextDisabled("%s", ws.planner_last_message.c_str());
      }
      if (ws.planner_plan.truncated) {
        ImGui::TextDisabled("(Planner truncated results; increase caps for more coverage)");
      }

      const bool can_apply = ws.planner_plan.ok && !ws.planner_plan.assignments.empty();
      if (!can_apply) ImGui::BeginDisabled();
      if (ImGui::Button("Apply Plan")) {
        ws.last_error.clear();
        std::string err;
        if (!nebula4x::apply_contract_plan(sim, ws.planner_plan, /*push_event=*/true, &err)) {
          ws.last_error = err.empty() ? "Failed to apply contract plan." : err;
        } else {
          // Refresh immediately after applying.
          recompute_planner();
        }
      }
      if (!can_apply) ImGui::EndDisabled();

      ImGuiTableFlags pflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
                               ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
      if (ImGui::BeginTable("contract_planner_table", 6, pflags, ImVec2(0.0f, 220.0f))) {
        ImGui::TableSetupColumn("Contract");
        ImGui::TableSetupColumn("Kind");
        ImGui::TableSetupColumn("Ship");
        ImGui::TableSetupColumn("ETA", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Work", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();

        for (const auto& asg : ws.planner_plan.assignments) {
          const auto* pc = find_ptr(st.contracts, asg.contract_id);
          const auto* psh = find_ptr(st.ships, asg.ship_id);
          if (!pc || !psh) continue;

          ImGui::TableNextRow();

          ImGui::TableNextColumn();
          {
            const std::string nm = pc->name.empty() ? (std::string("Contract ") + std::to_string((unsigned long long)pc->id))
                                                    : pc->name;
            const std::string label = nm + "##plan_contract_" + std::to_string((unsigned long long)pc->id);
            if (ImGui::Selectable(label.c_str(), ws.selected_contract == pc->id, ImGuiSelectableFlags_SpanAllColumns)) {
              ws.selected_contract = pc->id;
              ws.last_error.clear();
            }
          }

          ImGui::TableNextColumn();
          ImGui::TextUnformatted(contract_kind_label(pc->kind));

          ImGui::TableNextColumn();
          {
            const std::string nm = ship_label(st, psh->id);
            const std::string label = nm + "##plan_ship_" + std::to_string((unsigned long long)psh->id);
            if (ImGui::Selectable(label.c_str(), false)) {
              focus_ship(psh->id, sim, ui, selected_ship, selected_colony, selected_body);
            }
          }

          ImGui::TableNextColumn();
          {
            const std::string eta = fmt_eta_days(asg.eta_days);
            const std::string arr = fmt_arrival_label(sim, asg.eta_days);
            ImGui::TextUnformatted((eta + " " + arr).c_str());
          }

          ImGui::TableNextColumn();
          ImGui::TextUnformatted(fmt_eta_days(asg.work_days).c_str());

          ImGui::TableNextColumn();
          ImGui::Text("%.3f", asg.score);
        }

        ImGui::EndTable();
      }
    } else {
      ImGui::TextDisabled("No plan computed.");
    }
  }

  ImGui::End();
}

} // namespace nebula4x::ui
