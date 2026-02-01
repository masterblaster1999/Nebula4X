#include "ui/automation_center_window.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "nebula4x/core/orders.h"

#include "ui/order_ui.h"

namespace nebula4x::ui {
namespace {

enum class MissionMode : int {
  None = 0,
  Explore,
  Freight,
  Salvage,
  Mine,
  Colonize,
  Tanker,
  TroopTransport,
};

const char* mission_mode_label(MissionMode m) {
  switch (m) {
    case MissionMode::None:
      return "None";
    case MissionMode::Explore:
      return "Explore";
    case MissionMode::Freight:
      return "Freight";
    case MissionMode::Salvage:
      return "Salvage";
    case MissionMode::Mine:
      return "Mine";
    case MissionMode::Colonize:
      return "Colonize";
    case MissionMode::Tanker:
      return "Tanker";
    case MissionMode::TroopTransport:
      return "Troops";
  }
  return "?";
}

int mission_flag_count(const Ship& sh) {
  int c = 0;
  if (sh.auto_explore) c++;
  if (sh.auto_freight) c++;
  if (sh.auto_salvage) c++;
  if (sh.auto_mine) c++;
  if (sh.auto_colonize) c++;
  if (sh.auto_tanker) c++;
  if (sh.auto_troop_transport) c++;
  return c;
}

MissionMode current_mission_mode(const Ship& sh) {
  // NOTE: The simulation runs some automations earlier than others (see simulation_tick_ai.cpp).
  // We reflect that precedence here so the UI's "effective mission" matches actual behavior.
  if (sh.auto_troop_transport) return MissionMode::TroopTransport;
  if (sh.auto_tanker) return MissionMode::Tanker;
  if (sh.auto_salvage) return MissionMode::Salvage;
  if (sh.auto_mine) return MissionMode::Mine;
  if (sh.auto_colonize) return MissionMode::Colonize;
  if (sh.auto_explore) return MissionMode::Explore;
  if (sh.auto_freight) return MissionMode::Freight;
  return MissionMode::None;
}

void set_mission_mode(Ship& sh, MissionMode m) {
  // Mission modes are treated as mutually exclusive for clarity.
  sh.auto_explore = false;
  sh.auto_freight = false;
  sh.auto_salvage = false;
  sh.auto_mine = false;
  sh.auto_colonize = false;
  sh.auto_tanker = false;
  sh.auto_troop_transport = false;

  switch (m) {
    case MissionMode::None:
      break;
    case MissionMode::Explore:
      sh.auto_explore = true;
      break;
    case MissionMode::Freight:
      sh.auto_freight = true;
      break;
    case MissionMode::Salvage:
      sh.auto_salvage = true;
      break;
    case MissionMode::Mine:
      sh.auto_mine = true;
      break;
    case MissionMode::Colonize:
      sh.auto_colonize = true;
      break;
    case MissionMode::Tanker:
      sh.auto_tanker = true;
      break;
    case MissionMode::TroopTransport:
      sh.auto_troop_transport = true;
      break;
  }
}

bool str_contains_case_insensitive(const std::string& hay, const std::string& needle) {
  if (needle.empty()) return true;
  if (hay.empty()) return false;

  // Naive ASCII-only case fold. Good enough for UI filtering.
  auto lower = [](unsigned char c) -> unsigned char {
    if (c >= 'A' && c <= 'Z') return static_cast<unsigned char>(c - 'A' + 'a');
    return c;
  };

  std::string n;
  n.reserve(needle.size());
  for (unsigned char c : needle) n.push_back(static_cast<char>(lower(c)));

  std::string h;
  h.reserve(hay.size());
  for (unsigned char c : hay) h.push_back(static_cast<char>(lower(c)));

  return h.find(n) != std::string::npos;
}

bool str_contains_case_sensitive(const std::string& hay, const std::string& needle) {
  if (needle.empty()) return true;
  if (hay.empty()) return false;
  return hay.find(needle) != std::string::npos;
}

bool ship_is_idle_for_automation_center(const GameState& s, Id ship_id) {
  auto it = s.ship_orders.find(ship_id);
  if (it == s.ship_orders.end()) return true;
  return ship_orders_is_idle_for_automation(it->second);
}

// (Order string rendering lives in ui/order_ui.*)

double clamp01(double v) {
  if (!std::isfinite(v)) return 0.0;
  if (v < 0.0) return 0.0;
  if (v > 1.0) return 1.0;
  return v;
}

Id pick_home_colony_in_system(const GameState& s, const Ship& sh) {
  if (sh.system_id == kInvalidId) return kInvalidId;

  Id best = kInvalidId;
  double best_d2 = std::numeric_limits<double>::infinity();

  for (const auto& [cid, c] : s.colonies) {
    if (c.faction_id != sh.faction_id) continue;
    if (c.body_id == kInvalidId) continue;
    const auto* b = find_ptr(s.bodies, c.body_id);
    if (!b) continue;
    if (b->system_id != sh.system_id) continue;

    const Vec2 d = b->position_mkm - sh.position_mkm;
    const double d2 = d.length_squared();
    if (d2 < best_d2 - 1e-9 || (std::abs(d2 - best_d2) <= 1e-9 && cid < best)) {
      best = cid;
      best_d2 = d2;
    }
  }
  return best;
}

ShipAutomationProfile preset_profile_for(MissionMode m, const Simulation& sim, const Ship& sh) {
  ShipAutomationProfile p;

  const auto* d = sim.find_design(sh.design_id);
  const double fuel_cap = d ? std::max(0.0, d->fuel_capacity_tons) : 0.0;
  const bool has_fuel = fuel_cap > 1e-9;
  const bool has_missiles = d && d->missile_ammo_capacity > 0;
  const bool has_hp = d && d->max_hp > 1e-9;

  // Sustainment defaults: safe but not overly aggressive.
  p.auto_refuel = has_fuel;
  p.auto_refuel_threshold_fraction = 0.30;

  p.auto_repair = has_hp;
  p.auto_repair_threshold_fraction = 0.80;

  p.auto_rearm = has_missiles;
  p.auto_rearm_threshold_fraction = 0.30;

  // Mission.
  switch (m) {
    case MissionMode::Explore:
      p.auto_explore = true;
      // Explorers should refuel earlier.
      p.auto_refuel_threshold_fraction = 0.40;
      break;
    case MissionMode::Freight:
      p.auto_freight = true;
      break;
    case MissionMode::Salvage:
      p.auto_salvage = true;
      break;
    case MissionMode::Mine:
      p.auto_mine = true;
      p.auto_mine_home_colony_id = pick_home_colony_in_system(sim.state(), sh);
      break;
    case MissionMode::Colonize:
      p.auto_colonize = true;
      p.auto_refuel_threshold_fraction = 0.45;
      p.auto_repair_threshold_fraction = 0.90;
      break;
    case MissionMode::Tanker:
      p.auto_tanker = true;
      p.auto_tanker_reserve_fraction = 0.30;
      break;
    case MissionMode::TroopTransport:
      p.auto_troop_transport = true;
      break;
    case MissionMode::None:
      break;
  }

  return p;
}

ShipAutomationProfile suggest_profile_for_ship(const Simulation& sim, const Ship& sh) {
  ShipAutomationProfile p;
  const auto* d = sim.find_design(sh.design_id);
  const double cargo = d ? std::max(0.0, d->cargo_tons) : 0.0;
  const double mine_rate = d ? std::max(0.0, d->mining_tons_per_day) : 0.0;
  const double colony_cap = d ? std::max(0.0, d->colony_capacity_millions) : 0.0;
  const double troop_cap = d ? std::max(0.0, d->troop_capacity) : 0.0;
  const double fuel_cap = d ? std::max(0.0, d->fuel_capacity_tons) : 0.0;
  const bool has_fuel = fuel_cap > 1e-9;
  const bool has_missiles = d && d->missile_ammo_capacity > 0;
  const bool combatish = d && ((d->weapon_damage > 1e-9) || (d->missile_damage > 1e-9) || (d->point_defense_damage > 1e-9));

  // Sustainment is generally desirable for anything that travels.
  p.auto_refuel = has_fuel;
  p.auto_refuel_threshold_fraction = combatish ? 0.35 : 0.40;

  p.auto_repair = d && d->max_hp > 1e-9;
  p.auto_repair_threshold_fraction = combatish ? 0.85 : 0.80;

  p.auto_rearm = has_missiles;
  p.auto_rearm_threshold_fraction = 0.35;

  // Mission heuristics (mutually exclusive):
  // - Combat ships default to "no mission" (player-controlled)
  // - Colony-capable ships default to colonization
  // - Troop-capable ships default to troop logistics
  // - Mining rigs default to auto-mine
  // - Cargo-only ships default to auto-freight
  // - Otherwise, default to exploration (scouts)

  MissionMode m = MissionMode::None;
  if (!combatish) {
    if (colony_cap > 1e-9) {
      m = MissionMode::Colonize;
      p.auto_refuel_threshold_fraction = 0.45;
      p.auto_repair_threshold_fraction = 0.90;
    } else if (troop_cap > 1e-9) {
      m = MissionMode::TroopTransport;
    } else if (mine_rate > 1e-9 && cargo > 1e-9) {
      m = MissionMode::Mine;
      p.auto_mine_home_colony_id = pick_home_colony_in_system(sim.state(), sh);
    } else if (cargo > 1e-9) {
      m = MissionMode::Freight;
    } else {
      m = MissionMode::Explore;
    }
  }

  switch (m) {
    case MissionMode::Explore:
      p.auto_explore = true;
      break;
    case MissionMode::Freight:
      p.auto_freight = true;
      break;
    case MissionMode::Salvage:
      p.auto_salvage = true;
      break;
    case MissionMode::Mine:
      p.auto_mine = true;
      break;
    case MissionMode::Colonize:
      p.auto_colonize = true;
      break;
    case MissionMode::Tanker:
      p.auto_tanker = true;
      p.auto_tanker_reserve_fraction = 0.30;
      break;
    case MissionMode::TroopTransport:
      p.auto_troop_transport = true;
      break;
    case MissionMode::None:
      break;
  }

  return p;
}

void apply_profile_to_ship(Ship& sh, const ShipAutomationProfile& p, bool set_mission, bool set_sustainment) {
  // Mission flags.
  if (set_mission) {
    if (p.auto_explore || p.auto_freight || p.auto_salvage || p.auto_mine || p.auto_colonize || p.auto_tanker ||
        p.auto_troop_transport) {
      // Apply as a single mode (clear others first).
      if (p.auto_troop_transport)
        set_mission_mode(sh, MissionMode::TroopTransport);
      else if (p.auto_tanker)
        set_mission_mode(sh, MissionMode::Tanker);
      else if (p.auto_colonize)
        set_mission_mode(sh, MissionMode::Colonize);
      else if (p.auto_mine)
        set_mission_mode(sh, MissionMode::Mine);
      else if (p.auto_salvage)
        set_mission_mode(sh, MissionMode::Salvage);
      else if (p.auto_freight)
        set_mission_mode(sh, MissionMode::Freight);
      else if (p.auto_explore)
        set_mission_mode(sh, MissionMode::Explore);
      else
        set_mission_mode(sh, MissionMode::None);
    }

    if (p.auto_mine) {
      sh.auto_mine_home_colony_id = p.auto_mine_home_colony_id;
      sh.auto_mine_mineral = p.auto_mine_mineral;
    }
  }

  // Sustainment.
  if (set_sustainment) {
    sh.auto_refuel = p.auto_refuel;
    sh.auto_refuel_threshold_fraction = std::clamp(p.auto_refuel_threshold_fraction, 0.0, 1.0);

    sh.auto_tanker_reserve_fraction = std::clamp(p.auto_tanker_reserve_fraction, 0.0, 1.0);

    sh.auto_repair = p.auto_repair;
    sh.auto_repair_threshold_fraction = std::clamp(p.auto_repair_threshold_fraction, 0.0, 1.0);

    sh.auto_rearm = p.auto_rearm;
    sh.auto_rearm_threshold_fraction = std::clamp(p.auto_rearm_threshold_fraction, 0.0, 1.0);
  }
}

struct AutomationCenterState {
  Id faction_id{kInvalidId};
  bool lock_to_viewer_faction{true};
  bool only_idle{false};
  bool only_with_any_auto{false};
  bool hide_fleet_ships{false};

  bool filter_case_sensitive{false};
  char filter[128]{};

  // Procedural UI: Automatically choose which columns to show based on the
  // currently filtered ship set.
  bool procedural_columns{true};
  bool show_threshold_columns{false};
  bool show_first_order{true};

  // Selection.
  std::unordered_set<Id> selected;

  // Bulk apply.
  bool bulk_set_mission{true};
  bool bulk_set_sustainment{true};
  int bulk_preset_idx{0};
  int bulk_mission_idx{0};
  std::string bulk_ship_profile;
  float bulk_refuel_threshold{0.30f};
  float bulk_repair_threshold{0.80f};
  float bulk_rearm_threshold{0.30f};
  float bulk_tanker_reserve{0.30f};
};

AutomationCenterState& ac_state() {
  static AutomationCenterState st;
  return st;
}

void focus_ship(Id ship_id, Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  auto& st = sim.state();
  selected_ship = ship_id;
  selected_colony = kInvalidId;
  selected_body = kInvalidId;
  ui.selected_fleet_id = sim.fleet_for_ship(ship_id);

  if (const auto* sh = find_ptr(st.ships, ship_id)) {
    st.selected_system = sh->system_id;
    ui.show_map_window = true;
    ui.request_map_tab = MapTab::System;
    ui.show_details_window = true;
    ui.request_details_tab = DetailsTab::Ship;
  }
}

}  // namespace

void draw_automation_center_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony,
                                   Id& selected_body) {
  if (!ui.show_automation_center_window) return;

  AutomationCenterState& ac = ac_state();
  auto& s = sim.state();

  // Default faction selection.
  if (ac.lock_to_viewer_faction && ui.viewer_faction_id != kInvalidId) {
    ac.faction_id = ui.viewer_faction_id;
  }
  if (ac.faction_id == kInvalidId) {
    if (ui.viewer_faction_id != kInvalidId) {
      ac.faction_id = ui.viewer_faction_id;
    } else if (selected_ship != kInvalidId) {
      if (const auto* sh = find_ptr(s.ships, selected_ship)) ac.faction_id = sh->faction_id;
    }
    if (ac.faction_id == kInvalidId && !s.factions.empty()) {
      ac.faction_id = s.factions.begin()->first;
    }
  }

  ImGui::SetNextWindowSize(ImVec2(1080, 720), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Automation Center", &ui.show_automation_center_window)) {
    ImGui::End();
    return;
  }

  // ---- Filter / header controls ----
  {
    // Faction combo.
    std::vector<Id> fids;
    fids.reserve(s.factions.size());
    for (const auto& [fid, _] : s.factions) fids.push_back(fid);
    std::sort(fids.begin(), fids.end());

    if (ac.faction_id != kInvalidId && s.factions.find(ac.faction_id) == s.factions.end() && !fids.empty()) {
      ac.faction_id = fids.front();
    }

    const std::string fac_name = [&]() {
      if (const auto* f = find_ptr(s.factions, ac.faction_id)) return f->name;
      return std::string("<none>");
    }();

    if (ImGui::BeginCombo("Faction", fac_name.c_str())) {
      for (Id fid : fids) {
        const auto* f = find_ptr(s.factions, fid);
        if (!f) continue;
        const bool selected = (fid == ac.faction_id);
        if (ImGui::Selectable(f->name.c_str(), selected)) {
          ac.faction_id = fid;
          ac.selected.clear();
        }
        if (selected) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::Checkbox("Lock to viewer", &ac.lock_to_viewer_faction);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("When enabled, the window tracks the current viewer faction (fog-of-war context)");
    }

    ImGui::SameLine();
    ImGui::Checkbox("Idle only", &ac.only_idle);
    ImGui::SameLine();
    ImGui::Checkbox("Only ships w/ automation", &ac.only_with_any_auto);
    ImGui::SameLine();
    ImGui::Checkbox("Hide fleet ships", &ac.hide_fleet_ships);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Many ship-level mission automations ignore ships assigned to fleets.\n"
                        "This filter helps you focus on ships that can actually be automated.");
    }

    ImGui::InputTextWithHint("##ac_filter", "Filter (ship/design/system/fleet)", ac.filter, IM_ARRAYSIZE(ac.filter));
    ImGui::SameLine();
    ImGui::Checkbox("Aa", &ac.filter_case_sensitive);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Case sensitive filter");

    ImGui::Separator();
  }

  const std::string filter_text(ac.filter);

  // ---- Build filtered ship list ----
  struct ShipRow {
    Id id{kInvalidId};
    std::string name;
    std::string system_name;
    std::string fleet_name;
    std::string design_name;
    MissionMode mission{MissionMode::None};
    int mission_conflicts{0};
    bool in_fleet{false};
    bool idle{true};
    bool any_auto{false};
    bool has_fuel{false};
    bool has_missiles{false};
    bool has_hp{false};
    bool can_mine{false};
  };

  std::vector<ShipRow> rows;
  rows.reserve(s.ships.size());

  bool any_fuel = false;
  bool any_missiles = false;
  bool any_hp = false;
  bool any_mine = false;

  for (const auto& [sid, sh] : s.ships) {
    if (ac.faction_id != kInvalidId && sh.faction_id != ac.faction_id) continue;

    const Id fleet_id = sim.fleet_for_ship(sid);
    const bool in_fleet = (fleet_id != kInvalidId);
    if (ac.hide_fleet_ships && in_fleet) continue;

    const bool idle = ship_is_idle_for_automation_center(s, sid);
    if (ac.only_idle && !idle) continue;

    const int conflicts = mission_flag_count(sh);
    const bool any_auto = conflicts > 0 || sh.auto_refuel || sh.auto_repair || sh.auto_rearm;
    if (ac.only_with_any_auto && !any_auto) continue;

    const auto* sys = (sh.system_id != kInvalidId) ? find_ptr(s.systems, sh.system_id) : nullptr;
    const auto* fl = (fleet_id != kInvalidId) ? find_ptr(s.fleets, fleet_id) : nullptr;
    const auto* d = sim.find_design(sh.design_id);

    ShipRow r;
    r.id = sid;
    r.name = sh.name;
    r.system_name = sys ? sys->name : std::string("<none>");
    r.fleet_name = fl ? fl->name : std::string();
    r.design_name = d ? d->name : sh.design_id;
    r.mission = current_mission_mode(sh);
    r.mission_conflicts = conflicts;
    r.in_fleet = in_fleet;
    r.idle = idle;
    r.any_auto = any_auto;
    r.has_fuel = d && std::max(0.0, d->fuel_capacity_tons) > 1e-9;
    r.has_missiles = d && d->missile_ammo_capacity > 0;
    r.has_hp = d && std::max(0.0, d->max_hp) > 1e-9;
    r.can_mine = d && std::max(0.0, d->mining_tons_per_day) > 1e-9 && std::max(0.0, d->cargo_tons) > 1e-9;

    // Filter match.
    if (!filter_text.empty()) {
      const bool ok = ac.filter_case_sensitive
                          ? (str_contains_case_sensitive(r.name, filter_text) ||
                             str_contains_case_sensitive(r.design_name, filter_text) ||
                             str_contains_case_sensitive(r.system_name, filter_text) ||
                             str_contains_case_sensitive(r.fleet_name, filter_text))
                          : (str_contains_case_insensitive(r.name, filter_text) ||
                             str_contains_case_insensitive(r.design_name, filter_text) ||
                             str_contains_case_insensitive(r.system_name, filter_text) ||
                             str_contains_case_insensitive(r.fleet_name, filter_text));
      if (!ok) continue;
    }

    any_fuel = any_fuel || r.has_fuel;
    any_missiles = any_missiles || r.has_missiles;
    any_hp = any_hp || r.has_hp;
    any_mine = any_mine || r.can_mine;

    rows.push_back(std::move(r));
  }

  std::sort(rows.begin(), rows.end(), [](const ShipRow& a, const ShipRow& b) {
    if (a.system_name != b.system_name) return a.system_name < b.system_name;
    return a.name < b.name;
  });

  // ---- Selection helpers ----
  const int selected_count = static_cast<int>(ac.selected.size());
  ImGui::TextDisabled("Ships: %d  |  Selected: %d", (int)rows.size(), selected_count);

  ImGui::SameLine();
  if (ImGui::SmallButton("Select shown")) {
    for (const auto& r : rows) ac.selected.insert(r.id);
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Clear selection")) {
    ac.selected.clear();
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Invert")) {
    std::unordered_set<Id> next;
    next.reserve(rows.size() * 2 + 1);
    for (const auto& r : rows) {
      if (ac.selected.find(r.id) == ac.selected.end()) next.insert(r.id);
    }
    ac.selected.swap(next);
  }

  ImGui::SameLine();
  ImGui::Checkbox("Procedural columns", &ac.procedural_columns);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("When enabled, the table adapts to the current ship set (e.g. no missile ships => hide Rearm columns)");
  }

  ImGui::SameLine();
  ImGui::Checkbox("Show thresholds", &ac.show_threshold_columns);
  ImGui::SameLine();
  ImGui::Checkbox("Show first order", &ac.show_first_order);

  ImGui::Separator();

  // ---- Bulk actions ----
  if (ImGui::CollapsingHeader("Bulk actions", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::TextDisabled("Mission automation is treated as mutually exclusive here (to avoid silent conflicts).\n"
                        "Sustainment automation (refuel/repair/rearm) can stack with missions.");

    ImGui::Checkbox("Apply mission", &ac.bulk_set_mission);
    ImGui::SameLine();
    ImGui::Checkbox("Apply sustainment", &ac.bulk_set_sustainment);

    const char* presets[] = {"(No preset)",
                             "Explorer",
                             "Freighter",
                             "Salvager",
                             "Miner",
                             "Colonizer",
                             "Tanker",
                             "Troop Transport",
                             "Clear ALL automation"};
    ImGui::Combo("Preset", &ac.bulk_preset_idx, presets, IM_ARRAYSIZE(presets));

    const char* missions[] = {"None", "Explore", "Freight", "Salvage", "Mine", "Colonize", "Tanker", "Troops"};
    ImGui::Combo("Mission", &ac.bulk_mission_idx, missions, IM_ARRAYSIZE(missions));

    auto apply_to_selected = [&](const std::function<void(Ship&)>& fn) {
      if (ac.selected.empty()) return;
      for (Id sid : ac.selected) {
        Ship* sh = find_ptr(s.ships, sid);
        if (!sh) continue;
        if (ac.faction_id != kInvalidId && sh->faction_id != ac.faction_id) continue;
        fn(*sh);
      }
    };

    // Apply existing ship automation profiles (defined per-faction).
    if (const auto* fac = find_ptr(s.factions, ac.faction_id)) {
      std::vector<std::string> names;
      names.reserve(fac->ship_profiles.size());
      for (const auto& [name, _] : fac->ship_profiles) names.push_back(name);
      std::sort(names.begin(), names.end());

      if (!names.empty()) {
        if (ac.bulk_ship_profile.empty() ||
            std::find(names.begin(), names.end(), ac.bulk_ship_profile) == names.end()) {
          ac.bulk_ship_profile = names.front();
        }

        const char* current = ac.bulk_ship_profile.c_str();
        if (ImGui::BeginCombo("Ship profile", current)) {
          for (const auto& n : names) {
            const bool sel = (n == ac.bulk_ship_profile);
            if (ImGui::Selectable(n.c_str(), sel)) ac.bulk_ship_profile = n;
            if (sel) ImGui::SetItemDefaultFocus();
          }
          ImGui::EndCombo();
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("Apply profile")) {
          const auto* prof = find_ptr(fac->ship_profiles, ac.bulk_ship_profile);
          if (prof) {
            apply_to_selected([&](Ship& sh) { apply_profile_to_ship(sh, *prof, ac.bulk_set_mission, ac.bulk_set_sustainment); });
          }
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("Applies the selected faction ship profile to all selected ships");
        }
      }
    }

    if (any_fuel || !ac.procedural_columns) {
      ImGui::SliderFloat("Refuel threshold", &ac.bulk_refuel_threshold, 0.05f, 0.95f, "%.2f");
    }
    if (any_hp || !ac.procedural_columns) {
      ImGui::SliderFloat("Repair threshold", &ac.bulk_repair_threshold, 0.05f, 0.99f, "%.2f");
    }
    if (any_missiles || !ac.procedural_columns) {
      ImGui::SliderFloat("Rearm threshold", &ac.bulk_rearm_threshold, 0.05f, 0.95f, "%.2f");
    }
    ImGui::SliderFloat("Tanker reserve", &ac.bulk_tanker_reserve, 0.05f, 0.95f, "%.2f");

    ImGui::Spacing();
    if (ImGui::Button("Apply preset to selected")) {
      apply_to_selected([&](Ship& sh) {
        ShipAutomationProfile p;
        switch (ac.bulk_preset_idx) {
          case 1:
            p = preset_profile_for(MissionMode::Explore, sim, sh);
            break;
          case 2:
            p = preset_profile_for(MissionMode::Freight, sim, sh);
            break;
          case 3:
            p = preset_profile_for(MissionMode::Salvage, sim, sh);
            break;
          case 4:
            p = preset_profile_for(MissionMode::Mine, sim, sh);
            break;
          case 5:
            p = preset_profile_for(MissionMode::Colonize, sim, sh);
            break;
          case 6:
            p = preset_profile_for(MissionMode::Tanker, sim, sh);
            break;
          case 7:
            p = preset_profile_for(MissionMode::TroopTransport, sim, sh);
            break;
          case 8: {
            // Clear all automation.
            set_mission_mode(sh, MissionMode::None);
            sh.auto_refuel = false;
            sh.auto_repair = false;
            sh.auto_rearm = false;
            sh.auto_mine_home_colony_id = kInvalidId;
            sh.auto_mine_mineral.clear();
            return;
          }
          default:
            return;
        }

        // Override thresholds from bulk sliders if we are applying sustainment.
        if (ac.bulk_set_sustainment) {
          p.auto_refuel_threshold_fraction = std::clamp((double)ac.bulk_refuel_threshold, 0.0, 1.0);
          p.auto_repair_threshold_fraction = std::clamp((double)ac.bulk_repair_threshold, 0.0, 1.0);
          p.auto_rearm_threshold_fraction = std::clamp((double)ac.bulk_rearm_threshold, 0.0, 1.0);
          p.auto_tanker_reserve_fraction = std::clamp((double)ac.bulk_tanker_reserve, 0.0, 1.0);
        }

        apply_profile_to_ship(sh, p, ac.bulk_set_mission, ac.bulk_set_sustainment);
      });
    }

    ImGui::SameLine();
    if (ImGui::Button("Suggest for selected")) {
      apply_to_selected([&](Ship& sh) {
        ShipAutomationProfile p = suggest_profile_for_ship(sim, sh);
        if (ac.bulk_set_sustainment) {
          p.auto_refuel_threshold_fraction = std::clamp((double)ac.bulk_refuel_threshold, 0.0, 1.0);
          p.auto_repair_threshold_fraction = std::clamp((double)ac.bulk_repair_threshold, 0.0, 1.0);
          p.auto_rearm_threshold_fraction = std::clamp((double)ac.bulk_rearm_threshold, 0.0, 1.0);
          p.auto_tanker_reserve_fraction = std::clamp((double)ac.bulk_tanker_reserve, 0.0, 1.0);
        }
        apply_profile_to_ship(sh, p, ac.bulk_set_mission, ac.bulk_set_sustainment);
      });
    }

    ImGui::SameLine();
    if (ImGui::Button("Set mission (from Mission combo)")) {
      const MissionMode m = static_cast<MissionMode>(std::clamp(ac.bulk_mission_idx, 0, 7));
      apply_to_selected([&](Ship& sh) { set_mission_mode(sh, m); });
    }

    ImGui::SameLine();
    if (ImGui::Button("Clear mission")) {
      apply_to_selected([&](Ship& sh) { set_mission_mode(sh, MissionMode::None); });
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Tip: Most mission automations ignore ships assigned to fleets.\n"
                        "Detach ships from fleets if you want ship-level automation to control them.");

    ImGui::Separator();
  }

  // ---- Table ----
  // Column selection (procedural).
  const bool show_refuel = ac.procedural_columns ? any_fuel : true;
  const bool show_rearm = ac.procedural_columns ? any_missiles : true;
  const bool show_repair = ac.procedural_columns ? any_hp : true;
  const bool show_mine_cols = ac.procedural_columns ? any_mine : true;

  int cols = 0;
  cols += 1;  // select
  cols += 1;  // ship
  cols += 1;  // system
  cols += 1;  // fleet
  cols += 1;  // design
  cols += 1;  // idle
  cols += 1;  // mission
  if (show_refuel) cols += ac.show_threshold_columns ? 2 : 1;  // refuel flag + thr
  if (show_repair) cols += ac.show_threshold_columns ? 2 : 1;
  if (show_rearm) cols += ac.show_threshold_columns ? 2 : 1;
  cols += 1;  // conflicts/notes
  if (show_mine_cols) cols += 1;  // mine home colony
  if (ac.show_first_order) cols += 1;

  const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit;

  const float table_h = ImGui::GetContentRegionAvail().y;
  if (ImGui::BeginTable("##automation_table", cols, flags, ImVec2(0, table_h))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    int c = 0;
    ImGui::TableSetupColumn("Sel", ImGuiTableColumnFlags_WidthFixed, 34.0f);
    c++;
    ImGui::TableSetupColumn("Ship", ImGuiTableColumnFlags_WidthStretch, 200.0f);
    c++;
    ImGui::TableSetupColumn("System", ImGuiTableColumnFlags_WidthFixed, 120.0f);
    c++;
    ImGui::TableSetupColumn("Fleet", ImGuiTableColumnFlags_WidthFixed, 120.0f);
    c++;
    ImGui::TableSetupColumn("Design", ImGuiTableColumnFlags_WidthFixed, 160.0f);
    c++;
    ImGui::TableSetupColumn("Idle", ImGuiTableColumnFlags_WidthFixed, 44.0f);
    c++;
    ImGui::TableSetupColumn("Mission", ImGuiTableColumnFlags_WidthFixed, 110.0f);
    c++;

    if (show_refuel) {
      ImGui::TableSetupColumn("Rf", ImGuiTableColumnFlags_WidthFixed, 34.0f);
      c++;
      if (ac.show_threshold_columns) {
        ImGui::TableSetupColumn("Rf%", ImGuiTableColumnFlags_WidthFixed, 52.0f);
        c++;
      }
    }
    if (show_repair) {
      ImGui::TableSetupColumn("Rp", ImGuiTableColumnFlags_WidthFixed, 34.0f);
      c++;
      if (ac.show_threshold_columns) {
        ImGui::TableSetupColumn("Rp%", ImGuiTableColumnFlags_WidthFixed, 52.0f);
        c++;
      }
    }
    if (show_rearm) {
      ImGui::TableSetupColumn("Ra", ImGuiTableColumnFlags_WidthFixed, 34.0f);
      c++;
      if (ac.show_threshold_columns) {
        ImGui::TableSetupColumn("Ra%", ImGuiTableColumnFlags_WidthFixed, 52.0f);
        c++;
      }
    }

    ImGui::TableSetupColumn("Notes", ImGuiTableColumnFlags_WidthFixed, 72.0f);
    c++;
    if (show_mine_cols) {
      ImGui::TableSetupColumn("Mine Home", ImGuiTableColumnFlags_WidthFixed, 140.0f);
      c++;
    }
    if (ac.show_first_order) {
      ImGui::TableSetupColumn("First order", ImGuiTableColumnFlags_WidthStretch, 240.0f);
      c++;
    }

    ImGui::TableHeadersRow();

    for (const auto& r : rows) {
      Ship* sh = find_ptr(s.ships, r.id);
      if (!sh) continue;

      const auto* d = sim.find_design(sh->design_id);

      ImGui::TableNextRow();
      int col = 0;

      // Selection checkbox.
      ImGui::TableSetColumnIndex(col++);
      bool is_sel = (ac.selected.find(r.id) != ac.selected.end());
      const std::string chk_id = "##sel_" + std::to_string(static_cast<unsigned long long>(r.id));
      if (ImGui::Checkbox(chk_id.c_str(), &is_sel)) {
        if (is_sel)
          ac.selected.insert(r.id);
        else
          ac.selected.erase(r.id);
      }

      // Ship name (click to focus).
      ImGui::TableSetColumnIndex(col++);
      {
        const std::string lbl = r.name.empty() ? ("Ship " + std::to_string(static_cast<unsigned long long>(r.id))) : r.name;
        if (ImGui::Selectable(lbl.c_str(), false)) {
          focus_ship(r.id, sim, ui, selected_ship, selected_colony, selected_body);
        }

        if (ImGui::IsItemHovered()) {
          const double hp_frac = (d && d->max_hp > 1e-9) ? (clamp01(sh->hp / d->max_hp)) : 0.0;
          const double fuel_frac = (d && d->fuel_capacity_tons > 1e-9 && sh->fuel_tons >= 0.0)
                                       ? (clamp01(sh->fuel_tons / d->fuel_capacity_tons))
                                       : 0.0;

          ImGui::BeginTooltip();
          ImGui::TextUnformatted(lbl.c_str());
          if (!r.design_name.empty()) ImGui::Text("Design: %s", r.design_name.c_str());
          ImGui::Text("System: %s", r.system_name.c_str());
          if (!r.fleet_name.empty()) ImGui::Text("Fleet: %s", r.fleet_name.c_str());
          ImGui::Separator();
          if (d && d->max_hp > 1e-9) ImGui::Text("HP: %.0f / %.0f (%.0f%%)", sh->hp, d->max_hp, hp_frac * 100.0);
          if (d && d->fuel_capacity_tons > 1e-9 && sh->fuel_tons >= 0.0)
            ImGui::Text("Fuel: %.0f / %.0f (%.0f%%)", sh->fuel_tons, d->fuel_capacity_tons, fuel_frac * 100.0);
          ImGui::EndTooltip();
        }
      }

      // System.
      ImGui::TableSetColumnIndex(col++);
      ImGui::TextUnformatted(r.system_name.c_str());

      // Fleet.
      ImGui::TableSetColumnIndex(col++);
      if (!r.fleet_name.empty()) {
        ImGui::TextUnformatted(r.fleet_name.c_str());
      } else {
        ImGui::TextDisabled("-");
      }

      // Design.
      ImGui::TableSetColumnIndex(col++);
      ImGui::TextUnformatted(r.design_name.c_str());

      // Idle.
      ImGui::TableSetColumnIndex(col++);
      ImGui::TextUnformatted(r.idle ? "Yes" : "No");

      // Mission.
      ImGui::TableSetColumnIndex(col++);
      {
        int mm = static_cast<int>(current_mission_mode(*sh));
        const std::string id = "##mission_" + std::to_string(static_cast<unsigned long long>(r.id));
        if (ImGui::Combo(id.c_str(), &mm,
                         "None\0Explore\0Freight\0Salvage\0Mine\0Colonize\0Tanker\0Troops\0\0")) {
          set_mission_mode(*sh, static_cast<MissionMode>(mm));
        }
      }

      if (show_refuel) {
        ImGui::TableSetColumnIndex(col++);
        ImGui::Checkbox(("##rf_" + std::to_string(static_cast<unsigned long long>(r.id))).c_str(), &sh->auto_refuel);
        if (ac.show_threshold_columns) {
          ImGui::TableSetColumnIndex(col++);
          float thr = static_cast<float>(std::clamp(sh->auto_refuel_threshold_fraction, 0.0, 1.0));
          if (ImGui::DragFloat(("##rfthr_" + std::to_string(static_cast<unsigned long long>(r.id))).c_str(), &thr, 0.01f,
                               0.0f, 1.0f, "%.2f")) {
            sh->auto_refuel_threshold_fraction = std::clamp(static_cast<double>(thr), 0.0, 1.0);
          }
        }
      }

      if (show_repair) {
        ImGui::TableSetColumnIndex(col++);
        ImGui::Checkbox(("##rp_" + std::to_string(static_cast<unsigned long long>(r.id))).c_str(), &sh->auto_repair);
        if (ac.show_threshold_columns) {
          ImGui::TableSetColumnIndex(col++);
          float thr = static_cast<float>(std::clamp(sh->auto_repair_threshold_fraction, 0.0, 1.0));
          if (ImGui::DragFloat(("##rpthr_" + std::to_string(static_cast<unsigned long long>(r.id))).c_str(), &thr, 0.01f,
                               0.0f, 1.0f, "%.2f")) {
            sh->auto_repair_threshold_fraction = std::clamp(static_cast<double>(thr), 0.0, 1.0);
          }
        }
      }

      if (show_rearm) {
        ImGui::TableSetColumnIndex(col++);
        ImGui::Checkbox(("##ra_" + std::to_string(static_cast<unsigned long long>(r.id))).c_str(), &sh->auto_rearm);
        if (ac.show_threshold_columns) {
          ImGui::TableSetColumnIndex(col++);
          float thr = static_cast<float>(std::clamp(sh->auto_rearm_threshold_fraction, 0.0, 1.0));
          if (ImGui::DragFloat(("##rathr_" + std::to_string(static_cast<unsigned long long>(r.id))).c_str(), &thr, 0.01f,
                               0.0f, 1.0f, "%.2f")) {
            sh->auto_rearm_threshold_fraction = std::clamp(static_cast<double>(thr), 0.0, 1.0);
          }
        }
      }

      // Notes / conflicts.
      ImGui::TableSetColumnIndex(col++);
      {
        std::string note;
        if (r.mission_conflicts > 1) note += "!conf ";
        if (r.in_fleet && current_mission_mode(*sh) != MissionMode::None) note += "fleet ";
        if (!note.empty()) {
          ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "%s", note.c_str());
          if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            if (r.mission_conflicts > 1) {
              ImGui::TextUnformatted("This ship has multiple mission flags enabled.\n"
                                     "The Automation Center treats missions as exclusive to prevent silent priority bugs.");
            }
            if (r.in_fleet && current_mission_mode(*sh) != MissionMode::None) {
              ImGui::TextUnformatted("This ship is assigned to a fleet.\n"
                                     "Most ship-level mission automations ignore fleet ships.");
            }
            ImGui::EndTooltip();
          }
        } else {
          ImGui::TextDisabled("-");
        }
      }

      if (show_mine_cols) {
        ImGui::TableSetColumnIndex(col++);
        if (sh->auto_mine && sh->auto_mine_home_colony_id != kInvalidId) {
          const auto* home_col = find_ptr(s.colonies, sh->auto_mine_home_colony_id);
          if (home_col) {
            ImGui::TextUnformatted(home_col->name.c_str());
          } else {
            ImGui::TextDisabled("(invalid)");
          }
        } else {
          ImGui::TextDisabled("-");
        }
      }

      if (ac.show_first_order) {
        ImGui::TableSetColumnIndex(col++);
        const auto* so = find_ptr(s.ship_orders, r.id);
        const std::string ord = ship_orders_first_action_label(sim, so, ui.viewer_faction_id, ui.fog_of_war);
        if (!ord.empty()) {
          ImGui::TextUnformatted(ord.c_str());
          if (ImGui::IsItemHovered()) {
            draw_ship_orders_tooltip(sim, so, ui.viewer_faction_id, ui.fog_of_war);
          }
        } else {
          ImGui::TextDisabled("(none)");
          if (ImGui::IsItemHovered()) {
            draw_ship_orders_tooltip(sim, so, ui.viewer_faction_id, ui.fog_of_war);
          }
        }
      }
    }

    ImGui::EndTable();
  }

  ImGui::End();
}

}  // namespace nebula4x::ui
