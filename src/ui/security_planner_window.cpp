#include "ui/security_planner_window.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "imgui.h"

#include "nebula4x/core/security_planner.h"
#include "nebula4x/util/sorted_keys.h"

namespace nebula4x::ui {

namespace {

using nebula4x::find_ptr;
using nebula4x::Id;
using nebula4x::kInvalidId;
using nebula4x::SecurityPlannerOptions;
using nebula4x::SecurityPlannerResult;
using nebula4x::SecurityChokepoint;
using nebula4x::SecurityCorridor;
using nebula4x::SecurityRegionNeed;
using nebula4x::SecuritySystemNeed;
using nebula4x::TradeGoodFlow;
using nebula4x::util::sorted_keys;

void reset_mission_runtime(FleetMission& m) {
  m.sustainment_mode = FleetSustainmentMode::None;
  m.sustainment_colony_id = kInvalidId;
  m.last_target_ship_id = kInvalidId;
  m.escort_active_ship_id = kInvalidId;
  m.escort_last_retarget_day = 0;
  m.guard_last_alert_day = 0;
  m.patrol_leg_index = 0;
  m.patrol_region_system_index = 0;
  m.patrol_region_waypoint_index = 0;
  m.assault_bombard_executed = false;
}

const char* mission_label(FleetMissionType t) {
  switch (t) {
    case FleetMissionType::None: return "None";
    case FleetMissionType::PatrolSystem: return "Patrol system";
    case FleetMissionType::PatrolRegion: return "Patrol region";
    case FleetMissionType::PatrolRoute: return "Patrol route";
    case FleetMissionType::PatrolCircuit: return "Patrol circuit";
    case FleetMissionType::GuardJumpPoint: return "Guard jump point";
    default: return "(mission)";
  }
}

std::string system_name(const GameState& s, Id sys_id) {
  if (sys_id == kInvalidId) return "(unknown)";
  if (const auto* ss = find_ptr(s.systems, sys_id)) return ss->name;
  return "(unknown)";
}

std::string region_name(const GameState& s, Id rid) {
  if (rid == kInvalidId) return "(none)";
  if (const auto* r = find_ptr(s.regions, rid)) return r->name;
  return "(unknown)";
}

void focus_galaxy_system(Simulation& sim, UIState& ui, Id sys_id) {
  if (sys_id == kInvalidId) return;
  sim.state().selected_system = sys_id;
  ui.show_map_window = true;
  ui.request_map_tab = MapTab::Galaxy;
}

bool apply_mission_patrol_region(Simulation& sim, Id fleet_id, Id region_id) {
  auto& st = sim.state();
  Fleet* fl = find_ptr(st.fleets, fleet_id);
  if (!fl) return false;
  fl->mission.type = FleetMissionType::PatrolRegion;
  reset_mission_runtime(fl->mission);
  fl->mission.patrol_region_id = region_id;
  fl->mission.patrol_region_dwell_days = std::clamp(fl->mission.patrol_region_dwell_days, 3, 8);
  fl->mission.patrol_region_system_index = 0;
  fl->mission.patrol_region_waypoint_index = 0;
  return true;
}

bool apply_mission_patrol_system(Simulation& sim, Id fleet_id, Id system_id) {
  auto& st = sim.state();
  Fleet* fl = find_ptr(st.fleets, fleet_id);
  if (!fl) return false;
  fl->mission.type = FleetMissionType::PatrolSystem;
  reset_mission_runtime(fl->mission);
  fl->mission.patrol_system_id = system_id;
  fl->mission.patrol_dwell_days = std::clamp(fl->mission.patrol_dwell_days, 3, 10);
  fl->mission.patrol_leg_index = 0;
  return true;
}

bool apply_mission_patrol_route(Simulation& sim, Id fleet_id, Id a, Id b) {
  auto& st = sim.state();
  Fleet* fl = find_ptr(st.fleets, fleet_id);
  if (!fl) return false;
  fl->mission.type = FleetMissionType::PatrolRoute;
  reset_mission_runtime(fl->mission);
  fl->mission.patrol_route_a_system_id = a;
  fl->mission.patrol_route_b_system_id = b;
  fl->mission.patrol_dwell_days = std::clamp(fl->mission.patrol_dwell_days, 2, 8);
  fl->mission.patrol_leg_index = 0;
  return true;
}

bool apply_mission_guard_jump(Simulation& sim, Id fleet_id, Id jump_id) {
  auto& st = sim.state();
  Fleet* fl = find_ptr(st.fleets, fleet_id);
  if (!fl) return false;
  if (jump_id == kInvalidId) return false;
  fl->mission.type = FleetMissionType::GuardJumpPoint;
  reset_mission_runtime(fl->mission);
  fl->mission.guard_jump_point_id = jump_id;
  fl->mission.guard_jump_radius_mkm = std::clamp(fl->mission.guard_jump_radius_mkm, 10.0, 120.0);
  fl->mission.guard_jump_dwell_days = std::clamp(fl->mission.guard_jump_dwell_days, 2, 8);
  return true;
}

void draw_goods_tooltip(const std::vector<TradeGoodFlow>& flows) {
  if (flows.empty()) return;
  ImGui::SeparatorText("Top flows");
  for (const auto& f : flows) {
    if (!(f.volume > 1e-9)) continue;
    ImGui::Text("%s: %.1f", trade_good_kind_label(f.good), f.volume);
  }
}

}  // namespace

void draw_security_planner_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  (void)selected_ship;
  (void)selected_colony;
  (void)selected_body;

  ImGui::SetNextWindowSize(ImVec2(1180.0f, 760.0f), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Security Planner", &ui.show_security_planner_window)) {
    ImGui::End();
    return;
  }

  auto& st = sim.state();

  // --- Controls / cache ---
  static SecurityPlannerOptions opt;
  static bool opt_init = false;
  static SecurityPlannerResult cached;
  static std::uint64_t cached_state_gen = 0;
  static std::uint64_t cached_content_gen = 0;
  static std::int64_t cached_day = std::numeric_limits<std::int64_t>::min();
  static bool auto_refresh_daily = true;

  if (!opt_init) {
    opt_init = true;
    opt.faction_id = ui.viewer_faction_id;
    opt.restrict_to_discovered = ui.fog_of_war;
    opt.require_own_colony_endpoints = true;
    opt.max_lanes = 48;
    opt.min_lane_volume = 1.0;
    opt.risk_weight = 1.2;
    opt.own_colony_weight = 1.5;
    opt.desired_region_suppression = 0.75;
    opt.max_results = 32;
  }

  // Keep faction id synced with the viewer by default.
  if (opt.faction_id == kInvalidId && ui.viewer_faction_id != kInvalidId) opt.faction_id = ui.viewer_faction_id;

  const std::int64_t day = st.date.days_since_epoch();
  const bool gens_changed = (cached_state_gen != sim.state_generation()) || (cached_content_gen != sim.content_generation());
  const bool day_changed = (cached_day != day);

  bool force_recompute = false;
  ImGui::SeparatorText("Analysis");

  // Faction selector (local override).
  {
    const Faction* cur = (opt.faction_id != kInvalidId) ? find_ptr(st.factions, opt.faction_id) : nullptr;
    const char* label = cur ? cur->name.c_str() : "(select faction)";
    if (ImGui::BeginCombo("Faction", label)) {
      for (Id fid : sorted_keys(st.factions)) {
        const Faction* f = find_ptr(st.factions, fid);
        if (!f) continue;
        if (f->control == FactionControl::AI_Pirate) continue;
        const bool sel = (fid == opt.faction_id);
        std::string item = f->name + "##sec_fac_" + std::to_string(fid);
        if (ImGui::Selectable(item.c_str(), sel)) {
          opt.faction_id = fid;
          force_recompute = true;
        }
      }
      ImGui::EndCombo();
    }
  }

  ImGui::SameLine();
  ImGui::Checkbox("Auto refresh daily", &auto_refresh_daily);

  ImGui::Checkbox("Restrict to discovered", &opt.restrict_to_discovered);
  ImGui::SameLine();
  ImGui::Checkbox("Only lanes touching our colonies", &opt.require_own_colony_endpoints);

  ImGui::SliderInt("Max lanes", &opt.max_lanes, 8, 180);
  opt.max_lanes = std::clamp(opt.max_lanes, 1, 500);

  float min_lane_vol = static_cast<float>(opt.min_lane_volume);
  ImGui::SliderFloat("Min lane volume", &min_lane_vol, 0.0f, 20.0f, "%.1f");
  opt.min_lane_volume = std::clamp(static_cast<double>(min_lane_vol), 0.0, 1e9);

  float risk_w = static_cast<float>(opt.risk_weight);
  ImGui::SliderFloat("Risk weight", &risk_w, 0.0f, 3.0f, "%.2f");
  opt.risk_weight = std::clamp(static_cast<double>(risk_w), 0.0, 10.0);

  float own_w = static_cast<float>(opt.own_colony_weight);
  ImGui::SliderFloat("Own colony weight", &own_w, 1.0f, 3.0f, "%.2f");
  opt.own_colony_weight = std::clamp(static_cast<double>(own_w), 1.0, 10.0);

  float sup = static_cast<float>(opt.desired_region_suppression);
  ImGui::SliderFloat("Target suppression (regions)", &sup, 0.05f, 0.98f, "%.2f");
  opt.desired_region_suppression = std::clamp(static_cast<double>(sup), 0.0, 0.999999);
  ImGui::SliderInt("Max rows", &opt.max_results, 8, 80);
  opt.max_results = std::clamp(opt.max_results, 1, 200);

  if (ImGui::Button("Recompute")) {
    force_recompute = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("Focus: highest-need system")) {
    if (!cached.top_systems.empty()) {
      focus_galaxy_system(sim, ui, cached.top_systems.front().system_id);
    }
  }

  if (force_recompute || gens_changed || (auto_refresh_daily && day_changed)) {
    cached = nebula4x::compute_security_plan(sim, opt);
    cached_state_gen = sim.state_generation();
    cached_content_gen = sim.content_generation();
    cached_day = day;
  }

  if (!cached.ok) {
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Security analysis failed: %s", cached.message.c_str());
    ImGui::End();
    return;
  }

  ImGui::TextDisabled("Status: %s%s", cached.message.c_str(), cached.truncated ? " (truncated)" : "");

  // --- Fleet selector (for applying missions) ---
  static Id selected_fleet_id = kInvalidId;
  {
    std::vector<Id> fleets;
    fleets.reserve(st.fleets.size());
    for (Id flid : sorted_keys(st.fleets)) {
      const Fleet* fl = find_ptr(st.fleets, flid);
      if (!fl) continue;
      if (opt.faction_id != kInvalidId && fl->faction_id != opt.faction_id) continue;
      fleets.push_back(flid);
    }
    if (selected_fleet_id != kInvalidId) {
      const Fleet* cur = find_ptr(st.fleets, selected_fleet_id);
      if (!cur || (opt.faction_id != kInvalidId && cur->faction_id != opt.faction_id)) selected_fleet_id = kInvalidId;
    }
    const Fleet* cur = (selected_fleet_id != kInvalidId) ? find_ptr(st.fleets, selected_fleet_id) : nullptr;
    std::string label = cur ? (cur->name + " (" + std::string(mission_label(cur->mission.type)) + ")") : "(select fleet to assign missions)";
    if (ImGui::BeginCombo("Apply missions to fleet", label.c_str())) {
      for (Id flid : fleets) {
        const Fleet* fl = find_ptr(st.fleets, flid);
        if (!fl) continue;
        const bool sel = (flid == selected_fleet_id);
        std::string item = fl->name + "##sec_fleet_" + std::to_string(flid);
        if (ImGui::Selectable(item.c_str(), sel)) selected_fleet_id = flid;
      }
      ImGui::EndCombo();
    }
  }

  // --- Tabs ---
  if (ImGui::BeginTabBar("security_tabs")) {
    // --- Regions ---
    if (ImGui::BeginTabItem("Regions")) {
      ImGui::TextDisabled("Top regions by estimated security need (trade exposure × risk). ");
      if (ImGui::BeginTable("sec_regions", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
                            ImVec2(0, 360))) {
        ImGui::TableSetupColumn("Region", ImGuiTableColumnFlags_WidthStretch, 0.26f);
        ImGui::TableSetupColumn("Need", ImGuiTableColumnFlags_WidthFixed, 0.10f);
        ImGui::TableSetupColumn("Pirate", ImGuiTableColumnFlags_WidthFixed, 0.08f);
        ImGui::TableSetupColumn("Supp", ImGuiTableColumnFlags_WidthFixed, 0.08f);
        ImGui::TableSetupColumn("Eff", ImGuiTableColumnFlags_WidthFixed, 0.08f);
        ImGui::TableSetupColumn("+Power", ImGuiTableColumnFlags_WidthFixed, 0.10f);
        ImGui::TableSetupColumn("Example system", ImGuiTableColumnFlags_WidthStretch, 0.22f);
        ImGui::TableSetupColumn("Focus", ImGuiTableColumnFlags_WidthFixed, 0.06f);
        ImGui::TableSetupColumn("Assign", ImGuiTableColumnFlags_WidthFixed, 0.10f);
        ImGui::TableHeadersRow();

        for (const auto& r : cached.top_regions) {
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          ImGui::TextUnformatted(region_name(st, r.region_id).c_str());

          ImGui::TableSetColumnIndex(1);
          ImGui::Text("%.2f", r.need);
          ImGui::TableSetColumnIndex(2);
          ImGui::Text("%.2f", r.pirate_risk);
          ImGui::TableSetColumnIndex(3);
          ImGui::Text("%.2f", r.pirate_suppression);
          ImGui::TableSetColumnIndex(4);
          ImGui::Text("%.2f", r.effective_piracy_risk);
          ImGui::TableSetColumnIndex(5);
          ImGui::Text("%.1f", r.additional_patrol_power);

          ImGui::TableSetColumnIndex(6);
          ImGui::TextUnformatted(system_name(st, r.representative_system_id).c_str());
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Need: %.2f", r.representative_system_need);
          }

          ImGui::TableSetColumnIndex(7);
          std::string fbtn = "Focus##sec_reg_focus_" + std::to_string(r.region_id);
          if (ImGui::SmallButton(fbtn.c_str())) {
            focus_galaxy_system(sim, ui, r.representative_system_id);
          }

          ImGui::TableSetColumnIndex(8);
          if (selected_fleet_id == kInvalidId) {
            ImGui::TextDisabled("(select fleet)");
          } else {
            std::string abtn = "Patrol##sec_reg_patrol_" + std::to_string(r.region_id);
            if (ImGui::SmallButton(abtn.c_str())) {
              (void)apply_mission_patrol_region(sim, selected_fleet_id, r.region_id);
            }
          }
        }
        ImGui::EndTable();
      }

      ImGui::EndTabItem();
    }

    // --- Systems ---
    if (ImGui::BeginTabItem("Systems")) {
      ImGui::TextDisabled("Top systems by need (volume share × risk). ");
      if (ImGui::BeginTable("sec_systems", 10, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
                            ImVec2(0, 360))) {
        ImGui::TableSetupColumn("System", ImGuiTableColumnFlags_WidthStretch, 0.22f);
        ImGui::TableSetupColumn("Region", ImGuiTableColumnFlags_WidthStretch, 0.18f);
        ImGui::TableSetupColumn("Need", ImGuiTableColumnFlags_WidthFixed, 0.09f);
        ImGui::TableSetupColumn("Throughput", ImGuiTableColumnFlags_WidthFixed, 0.11f);
        ImGui::TableSetupColumn("Risk", ImGuiTableColumnFlags_WidthFixed, 0.07f);
        ImGui::TableSetupColumn("Piracy", ImGuiTableColumnFlags_WidthFixed, 0.07f);
        ImGui::TableSetupColumn("Block", ImGuiTableColumnFlags_WidthFixed, 0.07f);
        ImGui::TableSetupColumn("Loss", ImGuiTableColumnFlags_WidthFixed, 0.07f);
        ImGui::TableSetupColumn("Focus", ImGuiTableColumnFlags_WidthFixed, 0.06f);
        ImGui::TableSetupColumn("Assign", ImGuiTableColumnFlags_WidthFixed, 0.10f);
        ImGui::TableHeadersRow();

        for (const auto& e : cached.top_systems) {
          ImGui::TableNextRow();

          ImGui::TableSetColumnIndex(0);
          ImGui::TextUnformatted(system_name(st, e.system_id).c_str());
          if (e.has_own_colony) {
            ImGui::SameLine();
            ImGui::TextDisabled("*");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Contains your colony");
          }

          ImGui::TableSetColumnIndex(1);
          ImGui::TextUnformatted(region_name(st, e.region_id).c_str());
          ImGui::TableSetColumnIndex(2);
          ImGui::Text("%.2f", e.need);
          ImGui::TableSetColumnIndex(3);
          ImGui::Text("%.1f", e.trade_throughput);
          ImGui::TableSetColumnIndex(4);
          ImGui::Text("%.2f", e.endpoint_risk);
          ImGui::TableSetColumnIndex(5);
          ImGui::Text("%.2f", e.piracy_risk);
          ImGui::TableSetColumnIndex(6);
          ImGui::Text("%.2f", e.blockade_pressure);
          ImGui::TableSetColumnIndex(7);
          ImGui::Text("%.2f", e.shipping_loss_pressure);

          ImGui::TableSetColumnIndex(8);
          std::string fbtn = "Focus##sec_sys_focus_" + std::to_string(e.system_id);
          if (ImGui::SmallButton(fbtn.c_str())) {
            focus_galaxy_system(sim, ui, e.system_id);
          }

          ImGui::TableSetColumnIndex(9);
          if (selected_fleet_id == kInvalidId) {
            ImGui::TextDisabled("(select fleet)");
          } else {
            std::string abtn = "Patrol##sec_sys_patrol_" + std::to_string(e.system_id);
            if (ImGui::SmallButton(abtn.c_str())) {
              (void)apply_mission_patrol_system(sim, selected_fleet_id, e.system_id);
            }
          }
        }
        ImGui::EndTable();
      }
      ImGui::EndTabItem();
    }

    // --- Corridors ---
    if (ImGui::BeginTabItem("Corridors")) {
      ImGui::TextDisabled("High-volume trade lanes and suggested patrol routes.");
      if (ImGui::BeginTable("sec_corridors", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
                            ImVec2(0, 360))) {
        ImGui::TableSetupColumn("From", ImGuiTableColumnFlags_WidthStretch, 0.18f);
        ImGui::TableSetupColumn("To", ImGuiTableColumnFlags_WidthStretch, 0.18f);
        ImGui::TableSetupColumn("Vol", ImGuiTableColumnFlags_WidthFixed, 0.10f);
        ImGui::TableSetupColumn("Avg risk", ImGuiTableColumnFlags_WidthFixed, 0.10f);
        ImGui::TableSetupColumn("Max risk", ImGuiTableColumnFlags_WidthFixed, 0.10f);
        ImGui::TableSetupColumn("Hops", ImGuiTableColumnFlags_WidthFixed, 0.07f);
        ImGui::TableSetupColumn("Focus", ImGuiTableColumnFlags_WidthFixed, 0.08f);
        ImGui::TableSetupColumn("Assign", ImGuiTableColumnFlags_WidthFixed, 0.19f);
        ImGui::TableHeadersRow();

        for (const auto& c : cached.top_corridors) {
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          ImGui::TextUnformatted(system_name(st, c.from_system_id).c_str());
          ImGui::TableSetColumnIndex(1);
          ImGui::TextUnformatted(system_name(st, c.to_system_id).c_str());

          ImGui::TableSetColumnIndex(2);
          ImGui::Text("%.1f", c.volume);
          ImGui::TableSetColumnIndex(3);
          ImGui::Text("%.2f", c.avg_risk);
          ImGui::TableSetColumnIndex(4);
          ImGui::Text("%.2f", c.max_risk);
          ImGui::TableSetColumnIndex(5);
          const int hops = std::max(0, (int)c.route_systems.size() - 1);
          ImGui::Text("%d", hops);

          ImGui::TableSetColumnIndex(6);
          std::string fbtn = "A##sec_corr_focus_a_" + std::to_string(c.from_system_id) + "_" + std::to_string(c.to_system_id);
          if (ImGui::SmallButton(fbtn.c_str())) focus_galaxy_system(sim, ui, c.from_system_id);
          ImGui::SameLine();
          std::string fbtn2 = "B##sec_corr_focus_b_" + std::to_string(c.from_system_id) + "_" + std::to_string(c.to_system_id);
          if (ImGui::SmallButton(fbtn2.c_str())) focus_galaxy_system(sim, ui, c.to_system_id);

          ImGui::TableSetColumnIndex(7);
          if (selected_fleet_id == kInvalidId) {
            ImGui::TextDisabled("(select fleet)");
          } else {
            std::string abtn = "PatrolRoute##sec_corr_patrol_" + std::to_string(c.from_system_id) + "_" + std::to_string(c.to_system_id);
            if (ImGui::SmallButton(abtn.c_str())) {
              (void)apply_mission_patrol_route(sim, selected_fleet_id, c.from_system_id, c.to_system_id);
            }
          }
          if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Route:");
            for (std::size_t i = 0; i < c.route_systems.size(); ++i) {
              if (i > 0) ImGui::SameLine();
              ImGui::TextUnformatted(system_name(st, c.route_systems[i]).c_str());
              if (i + 1 < c.route_systems.size()) {
                ImGui::SameLine();
                ImGui::TextUnformatted("->");
              }
            }
            draw_goods_tooltip(c.top_flows);
            ImGui::EndTooltip();
          }
        }

        ImGui::EndTable();
      }
      ImGui::EndTabItem();
    }

    // --- Chokepoints ---
    if (ImGui::BeginTabItem("Chokepoints")) {
      ImGui::TextDisabled("Jump links that carry high trade traffic. Guarding jump points helps piracy suppression.");
      if (ImGui::BeginTable("sec_chok", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
                            ImVec2(0, 360))) {
        ImGui::TableSetupColumn("A", ImGuiTableColumnFlags_WidthStretch, 0.18f);
        ImGui::TableSetupColumn("B", ImGuiTableColumnFlags_WidthStretch, 0.18f);
        ImGui::TableSetupColumn("Traffic", ImGuiTableColumnFlags_WidthFixed, 0.12f);
        ImGui::TableSetupColumn("Avg risk", ImGuiTableColumnFlags_WidthFixed, 0.10f);
        ImGui::TableSetupColumn("Max risk", ImGuiTableColumnFlags_WidthFixed, 0.10f);
        ImGui::TableSetupColumn("JP A", ImGuiTableColumnFlags_WidthFixed, 0.06f);
        ImGui::TableSetupColumn("JP B", ImGuiTableColumnFlags_WidthFixed, 0.06f);
        ImGui::TableSetupColumn("Focus", ImGuiTableColumnFlags_WidthFixed, 0.08f);
        ImGui::TableSetupColumn("Assign", ImGuiTableColumnFlags_WidthFixed, 0.12f);
        ImGui::TableHeadersRow();

        for (const auto& c : cached.top_chokepoints) {
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          ImGui::TextUnformatted(system_name(st, c.system_a_id).c_str());
          ImGui::TableSetColumnIndex(1);
          ImGui::TextUnformatted(system_name(st, c.system_b_id).c_str());

          ImGui::TableSetColumnIndex(2);
          ImGui::Text("%.1f", c.traffic);
          ImGui::TableSetColumnIndex(3);
          ImGui::Text("%.2f", c.avg_risk);
          ImGui::TableSetColumnIndex(4);
          ImGui::Text("%.2f", c.max_risk);
          ImGui::TableSetColumnIndex(5);
          ImGui::TextUnformatted(c.jump_a_to_b != kInvalidId ? "Y" : "-");
          ImGui::TableSetColumnIndex(6);
          ImGui::TextUnformatted(c.jump_b_to_a != kInvalidId ? "Y" : "-");

          ImGui::TableSetColumnIndex(7);
          std::string fbtn = "A##sec_chok_focus_a_" + std::to_string(c.system_a_id) + "_" + std::to_string(c.system_b_id);
          if (ImGui::SmallButton(fbtn.c_str())) focus_galaxy_system(sim, ui, c.system_a_id);
          ImGui::SameLine();
          std::string fbtn2 = "B##sec_chok_focus_b_" + std::to_string(c.system_a_id) + "_" + std::to_string(c.system_b_id);
          if (ImGui::SmallButton(fbtn2.c_str())) focus_galaxy_system(sim, ui, c.system_b_id);

          ImGui::TableSetColumnIndex(8);
          if (selected_fleet_id == kInvalidId) {
            ImGui::TextDisabled("(select fleet)");
          } else {
            if (c.jump_a_to_b != kInvalidId) {
              std::string abtn = "Guard A##sec_guard_a_" + std::to_string(c.jump_a_to_b);
              if (ImGui::SmallButton(abtn.c_str())) {
                (void)apply_mission_guard_jump(sim, selected_fleet_id, c.jump_a_to_b);
              }
              ImGui::SameLine();
            }
            if (c.jump_b_to_a != kInvalidId) {
              std::string bbtn = "Guard B##sec_guard_b_" + std::to_string(c.jump_b_to_a);
              if (ImGui::SmallButton(bbtn.c_str())) {
                (void)apply_mission_guard_jump(sim, selected_fleet_id, c.jump_b_to_a);
              }
            }
            if (c.jump_a_to_b == kInvalidId && c.jump_b_to_a == kInvalidId) {
              ImGui::TextDisabled("(no JP ids)");
            }
          }
        }

        ImGui::EndTable();
      }
      ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
  }

  ImGui::Separator();
  ImGui::TextDisabled("Tip: high piracy regions can be stabilized by PatrolRegion, PatrolRoute, and GuardJumpPoint missions (suppression ramps over time). ");

  ImGui::End();
}

}  // namespace nebula4x::ui
