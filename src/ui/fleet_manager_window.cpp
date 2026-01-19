#include "ui/fleet_manager_window.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "imgui.h"

#include "nebula4x/util/log.h"
#include "nebula4x/util/sorted_keys.h"
#include "nebula4x/util/strings.h"

namespace nebula4x::ui {

namespace {

using nebula4x::find_ptr;
using nebula4x::Id;
using nebula4x::kInvalidId;
using nebula4x::to_lower;
using nebula4x::util::sorted_keys;

// --- Labels ---

const char* ship_role_label(ShipRole r) {
  switch (r) {
    case ShipRole::Freighter:
      return "Freighter";
    case ShipRole::Surveyor:
      return "Surveyor";
    case ShipRole::Combatant:
      return "Combatant";
    case ShipRole::Unknown:
    default:
      return "Unknown";
  }
}

const char* fleet_mission_label(FleetMissionType t) {
  static const char* kMissionNames[] = {
      "None",
      "Defend colony",
      "Patrol system",
      "Hunt hostiles",
      "Escort freighters",
      "Explore",
      "Patrol region",
      "Assault colony",
      "Blockade colony",
      "Patrol route",
      "Guard jump point",
      "Patrol circuit",
  };
  static_assert(IM_ARRAYSIZE(kMissionNames) == 1 + static_cast<int>(FleetMissionType::PatrolCircuit),
                "Update mission name table");
  const int idx = static_cast<int>(t);
  if (idx < 0 || idx >= static_cast<int>(IM_ARRAYSIZE(kMissionNames))) return "(unknown)";
  return kMissionNames[idx];
}

// --- Helpers ---

const Ship* resolve_fleet_leader(const GameState& s, const Fleet& f) {
  if (f.leader_ship_id != kInvalidId) {
    if (const auto* sh = find_ptr(s.ships, f.leader_ship_id)) return sh;
  }
  for (Id sid : f.ship_ids) {
    if (const auto* sh = find_ptr(s.ships, sid)) return sh;
  }
  return nullptr;
}

Id resolve_fleet_system_id(const GameState& s, const Fleet& f) {
  if (const auto* leader = resolve_fleet_leader(s, f)) return leader->system_id;
  return kInvalidId;
}

std::string system_name(const GameState& s, Id system_id) {
  if (system_id == kInvalidId) return "(unknown)";
  if (const auto* sys = find_ptr(s.systems, system_id)) return sys->name;
  return "(unknown)";
}

// Returns {has_value, min_fraction}.
std::pair<bool, double> fleet_min_fuel_fraction(const Simulation& sim, const Fleet& f) {
  const auto& s = sim.state();
  bool has = false;
  double minf = 1.0;
  for (Id sid : f.ship_ids) {
    const auto* sh = find_ptr(s.ships, sid);
    if (!sh) continue;
    const auto* d = sim.find_design(sh->design_id);
    if (!d) continue;
    if (d->fuel_capacity_tons <= 0.0) continue;
    if (sh->fuel_tons < 0.0) continue;
    const double frac = std::clamp(sh->fuel_tons / d->fuel_capacity_tons, 0.0, 1.0);
    minf = std::min(minf, frac);
    has = true;
  }
  return {has, minf};
}

std::pair<bool, double> fleet_min_hp_fraction(const Simulation& sim, const Fleet& f) {
  const auto& s = sim.state();
  bool has = false;
  double minf = 1.0;
  for (Id sid : f.ship_ids) {
    const auto* sh = find_ptr(s.ships, sid);
    if (!sh) continue;
    const auto* d = sim.find_design(sh->design_id);
    if (!d) continue;
    if (d->max_hp <= 0.0) continue;
    const double frac = std::clamp(sh->hp / d->max_hp, 0.0, 1.0);
    minf = std::min(minf, frac);
    has = true;
  }
  return {has, minf};
}

void reset_mission_runtime(FleetMission& m) {
  m.sustainment_mode = FleetSustainmentMode::None;
  m.sustainment_colony_id = kInvalidId;
  m.last_target_ship_id = kInvalidId;

  // Escort runtime.
  m.escort_active_ship_id = kInvalidId;
  m.escort_last_retarget_day = 0;

  // Guard runtime.
  m.guard_last_alert_day = 0;

  // Patrol indices.
  m.patrol_leg_index = 0;
  m.patrol_region_system_index = 0;
  m.patrol_region_waypoint_index = 0;

  // Assault runtime.
  m.assault_bombard_executed = false;
}

// Attempt to set reasonable defaults for a mission type (best-effort).
void seed_mission_defaults(const Simulation& sim, FleetMission& m, Id fleet_faction_id, Id preferred_system_id) {
  const auto& s = sim.state();
  if (m.type == FleetMissionType::DefendColony) {
    if (m.defend_colony_id == kInvalidId) {
      // Prefer a colony in preferred_system_id, otherwise first same-faction colony.
      if (preferred_system_id != kInvalidId) {
        for (Id cid : sorted_keys(s.colonies)) {
          const auto* c = find_ptr(s.colonies, cid);
          if (!c) continue;
          if (c->faction_id != fleet_faction_id) continue;
          const auto* b = find_ptr(s.bodies, c->body_id);
          if (!b) continue;
          if (b->system_id != preferred_system_id) continue;
          m.defend_colony_id = cid;
          break;
        }
      }
      if (m.defend_colony_id == kInvalidId) {
        for (Id cid : sorted_keys(s.colonies)) {
          const auto* c = find_ptr(s.colonies, cid);
          if (!c) continue;
          if (c->faction_id != fleet_faction_id) continue;
          m.defend_colony_id = cid;
          break;
        }
      }
    }
  }

  if (m.type == FleetMissionType::PatrolSystem) {
    if (m.patrol_system_id == kInvalidId) m.patrol_system_id = preferred_system_id;
  }

  if (m.type == FleetMissionType::GuardJumpPoint) {
    if (m.guard_jump_point_id == kInvalidId && preferred_system_id != kInvalidId) {
      const auto* sys = find_ptr(s.systems, preferred_system_id);
      if (sys && !sys->jump_points.empty()) {
        auto jps = sys->jump_points;
        std::sort(jps.begin(), jps.end());
        m.guard_jump_point_id = jps.front();
      }
    }
    if (m.guard_jump_radius_mkm <= 0.0) m.guard_jump_radius_mkm = 50.0;
    if (m.guard_jump_dwell_days <= 0) m.guard_jump_dwell_days = 3;
    m.guard_last_alert_day = 0;
  }

  if (m.type == FleetMissionType::PatrolCircuit) {
    if (m.patrol_circuit_system_ids.empty() && preferred_system_id != kInvalidId) {
      // Seed with the current system (player can edit waypoints in the Fleet tab).
      m.patrol_circuit_system_ids.push_back(preferred_system_id);
      m.patrol_leg_index = 0;
    }
  }
}

// --- Fleet table row ---

struct FleetRow {
  Id id{kInvalidId};
  const Fleet* fleet{nullptr};

  const Faction* faction{nullptr};
  const Ship* leader{nullptr};

  Id system_id{kInvalidId};
  std::string system_name;

  int ship_count{0};
  FleetMissionType mission{FleetMissionType::None};

  bool has_min_fuel{false};
  double min_fuel{1.0};

  bool has_min_hp{false};
  double min_hp{1.0};

  double leader_speed_km_s{0.0};
};

enum FleetTableCol {
  ColName = 0,
  ColFaction = 1,
  ColSystem = 2,
  ColShips = 3,
  ColMission = 4,
  ColFuel = 5,
  ColHP = 6,
  ColSpeed = 7,
  ColActions = 8,
};

int compare_rows(const FleetRow& a, const FleetRow& b, const ImGuiTableSortSpecs* sort_specs) {
  if (!sort_specs || sort_specs->SpecsCount == 0) {
    // Default sort: name, then id.
    const std::string& an = a.fleet ? a.fleet->name : std::string();
    const std::string& bn = b.fleet ? b.fleet->name : std::string();
    const int c = an.compare(bn);
    if (c != 0) return c;
    if (a.id < b.id) return -1;
    if (a.id > b.id) return 1;
    return 0;
  }

  for (int n = 0; n < sort_specs->SpecsCount; ++n) {
    const ImGuiTableColumnSortSpecs& spec = sort_specs->Specs[n];
    int c = 0;

    switch (spec.ColumnIndex) {
      case ColName: {
        const std::string& an = a.fleet ? a.fleet->name : std::string();
        const std::string& bn = b.fleet ? b.fleet->name : std::string();
        c = an.compare(bn);
        break;
      }
      case ColFaction: {
        const std::string& an = a.faction ? a.faction->name : std::string();
        const std::string& bn = b.faction ? b.faction->name : std::string();
        c = an.compare(bn);
        break;
      }
      case ColSystem: {
        c = a.system_name.compare(b.system_name);
        break;
      }
      case ColShips: {
        if (a.ship_count < b.ship_count) c = -1;
        else if (a.ship_count > b.ship_count) c = 1;
        else c = 0;
        break;
      }
      case ColMission: {
        const int ai = static_cast<int>(a.mission);
        const int bi = static_cast<int>(b.mission);
        if (ai < bi) c = -1;
        else if (ai > bi) c = 1;
        else c = 0;
        break;
      }
      case ColFuel: {
        // Missing fuel sorts last.
        if (a.has_min_fuel != b.has_min_fuel) {
          c = a.has_min_fuel ? -1 : 1;
        } else if (!a.has_min_fuel) {
          c = 0;
        } else if (a.min_fuel < b.min_fuel) {
          c = -1;
        } else if (a.min_fuel > b.min_fuel) {
          c = 1;
        }
        break;
      }
      case ColHP: {
        if (a.has_min_hp != b.has_min_hp) {
          c = a.has_min_hp ? -1 : 1;
        } else if (!a.has_min_hp) {
          c = 0;
        } else if (a.min_hp < b.min_hp) {
          c = -1;
        } else if (a.min_hp > b.min_hp) {
          c = 1;
        }
        break;
      }
      case ColSpeed: {
        if (a.leader_speed_km_s < b.leader_speed_km_s) c = -1;
        else if (a.leader_speed_km_s > b.leader_speed_km_s) c = 1;
        else c = 0;
        break;
      }
      default:
        break;
    }

    if (c != 0) {
      if (spec.SortDirection == ImGuiSortDirection_Ascending) return c;
      return -c;
    }
  }

  // Final tiebreak.
  if (a.id < b.id) return -1;
  if (a.id > b.id) return 1;
  return 0;
}

// --- Mission summary helpers ---

std::string fmt_id(Id id) {
  return std::to_string(static_cast<unsigned long long>(id));
}

std::string fleet_mission_target_brief(const Simulation& sim, const Fleet& f) {
  const auto& s = sim.state();
  const auto& m = f.mission;
  switch (m.type) {
    case FleetMissionType::DefendColony: {
      if (const auto* c = find_ptr(s.colonies, m.defend_colony_id)) return c->name;
      return "(no colony)";
    }
    case FleetMissionType::PatrolSystem: {
      if (const auto* sys = find_ptr(s.systems, m.patrol_system_id)) return sys->name;
      return "(no system)";
    }
    case FleetMissionType::PatrolRoute: {
      const auto* a = find_ptr(s.systems, m.patrol_route_a_system_id);
      const auto* b = find_ptr(s.systems, m.patrol_route_b_system_id);
      if (a && b) return a->name + " <-> " + b->name;
      return "(endpoints)";
    }
    case FleetMissionType::GuardJumpPoint: {
      const auto* jp = find_ptr(s.jump_points, m.guard_jump_point_id);
      if (jp) {
        const auto* sys = find_ptr(s.systems, jp->system_id);
        if (sys) return sys->name + ": " + jp->name;
        return jp->name;
      }
      return "(no jump point)";
    }
    case FleetMissionType::PatrolCircuit: {
      if (m.patrol_circuit_system_ids.empty()) return "(no waypoints)";
      std::string out;
      const std::size_t n = m.patrol_circuit_system_ids.size();
      out.reserve(32);
      out += std::to_string(static_cast<int>(n));
      out += " waypoint";
      if (n != 1) out += "s";
      return out;
    }
    case FleetMissionType::PatrolRegion: {
      if (const auto* r = find_ptr(s.regions, m.patrol_region_id)) return r->name;
      return "(no region)";
    }
    case FleetMissionType::EscortFreighters: {
      if (m.escort_target_ship_id != kInvalidId) {
        if (const auto* sh = find_ptr(s.ships, m.escort_target_ship_id)) return sh->name;
        return "(missing target)";
      }
      return "(auto)";
    }
    case FleetMissionType::AssaultColony: {
      if (const auto* c = find_ptr(s.colonies, m.assault_colony_id)) return c->name;
      return "(no colony)";
    }
    case FleetMissionType::BlockadeColony: {
      if (const auto* c = find_ptr(s.colonies, m.blockade_colony_id)) return c->name;
      return "(no colony)";
    }
    case FleetMissionType::HuntHostiles:
    case FleetMissionType::Explore:
    case FleetMissionType::None:
    default:
      return "";
  }
}

// --- Fleet Forge ---

struct ForgeKey {
  Id faction_id{kInvalidId};
  Id system_id{kInvalidId};
  ShipRole role{ShipRole::Unknown};

  bool operator==(const ForgeKey& o) const {
    return faction_id == o.faction_id && system_id == o.system_id && role == o.role;
  }
};

struct ForgeKeyHash {
  std::size_t operator()(const ForgeKey& k) const {
    // Simple mixing.
    const std::size_t a = std::hash<unsigned long long>{}(static_cast<unsigned long long>(k.faction_id));
    const std::size_t b = std::hash<unsigned long long>{}(static_cast<unsigned long long>(k.system_id));
    const std::size_t c = std::hash<int>{}(static_cast<int>(k.role));
    return a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2)) ^ (c << 1);
  }
};

struct ForgeSuggestion {
  ForgeKey key;
  std::vector<Id> ship_ids;
};

std::string make_unique_fleet_name(const GameState& s, Id faction_id, const std::string& base) {
  // Build a set of existing fleet names for this faction.
  std::unordered_set<std::string> names;
  names.reserve(s.fleets.size());
  for (const auto& kv : s.fleets) {
    const Fleet& f = kv.second;
    if (f.faction_id != faction_id) continue;
    names.insert(f.name);
  }

  if (names.find(base) == names.end()) return base;

  // Try numeric suffix.
  for (int i = 2; i < 999; ++i) {
    std::string cand = base + " " + std::to_string(i);
    if (names.find(cand) == names.end()) return cand;
  }

  // Fallback (shouldn't happen).
  return base + " " + std::to_string(static_cast<unsigned long long>(std::hash<std::string>{}(base) & 0xffff));
}

FleetMissionType suggested_mission_for_role(ShipRole role) {
  switch (role) {
    case ShipRole::Surveyor:
      return FleetMissionType::Explore;
    case ShipRole::Combatant:
      return FleetMissionType::PatrolSystem;
    default:
      return FleetMissionType::None;
  }
}

}  // namespace

void draw_fleet_manager_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  (void)selected_colony;
  (void)selected_body;

  ImGui::SetNextWindowSize(ImVec2(1180.0f, 760.0f), ImGuiCond_FirstUseEver);

  if (!ImGui::Begin("Fleet Manager", &ui.show_fleet_manager_window)) {
    ImGui::End();
    return;
  }

  auto& s = sim.state();

  // Keep selection valid.
  if (ui.selected_fleet_id != kInvalidId && !find_ptr(s.fleets, ui.selected_fleet_id)) {
    ui.selected_fleet_id = kInvalidId;
  }

  if (ImGui::BeginTabBar("fleet_manager_tabs")) {
    // --- Fleets tab ---
    if (ImGui::BeginTabItem("Fleets")) {
      // Split: list + inspector.
      const ImGuiTableFlags split_flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
      if (ImGui::BeginTable("fleet_manager_split", 2, split_flags)) {
        ImGui::TableSetupColumn("Fleets", ImGuiTableColumnFlags_WidthStretch, 0.58f);
        ImGui::TableSetupColumn("Inspector", ImGuiTableColumnFlags_WidthStretch, 0.42f);
        ImGui::TableNextRow();

        // --- Left: list ---
        ImGui::TableSetColumnIndex(0);
        {
          static char search_buf[128] = "";
          static bool only_viewer_faction = false;
          static bool only_selected_system = false;

          ImGui::TextDisabled("Global list of fleets (sortable). Click a row to inspect; double-click to focus.");
          ImGui::InputTextWithHint("##fleet_search", "Search fleets (name/faction/system/mission)", search_buf,
                                  IM_ARRAYSIZE(search_buf));
          ImGui::SameLine();
          ImGui::Checkbox("Only viewer faction", &only_viewer_faction);
          ImGui::SameLine();
          ImGui::Checkbox("Only current system", &only_selected_system);

          const std::string q = to_lower(std::string(search_buf));

          // Build rows.
          std::vector<FleetRow> rows;
          rows.reserve(s.fleets.size());

          for (const auto& kv : s.fleets) {
            const Fleet& f = kv.second;

            if (only_viewer_faction && ui.viewer_faction_id != kInvalidId && f.faction_id != ui.viewer_faction_id) {
              continue;
            }

            const Id sys_id = resolve_fleet_system_id(s, f);
            if (only_selected_system && s.selected_system != kInvalidId && sys_id != s.selected_system) {
              continue;
            }

            FleetRow row;
            row.id = f.id;
            row.fleet = &f;
            row.faction = find_ptr(s.factions, f.faction_id);
            row.leader = resolve_fleet_leader(s, f);
            row.system_id = sys_id;
            row.system_name = system_name(s, sys_id);
            row.ship_count = static_cast<int>(f.ship_ids.size());
            row.mission = f.mission.type;

            const auto [has_fuel, min_fuel] = fleet_min_fuel_fraction(sim, f);
            row.has_min_fuel = has_fuel;
            row.min_fuel = min_fuel;

            const auto [has_hp, min_hp] = fleet_min_hp_fraction(sim, f);
            row.has_min_hp = has_hp;
            row.min_hp = min_hp;

            if (row.leader) row.leader_speed_km_s = row.leader->speed_km_s;

            // Query filter.
            if (!q.empty()) {
              std::string hay;
              hay.reserve(128);
              hay += to_lower(f.name);
              if (row.faction) {
                hay += " ";
                hay += to_lower(row.faction->name);
              }
              if (!row.system_name.empty()) {
                hay += " ";
                hay += to_lower(row.system_name);
              }
              hay += " ";
              hay += to_lower(std::string(fleet_mission_label(row.mission)));

              if (hay.find(q) == std::string::npos) continue;
            }

            rows.push_back(std::move(row));
          }

          // Summary.
          ImGui::Separator();
          int ships_total = 0;
          int missions_enabled = 0;
          for (const FleetRow& r : rows) {
            ships_total += r.ship_count;
            if (r.mission != FleetMissionType::None) ++missions_enabled;
          }
          ImGui::Text("Fleets: %d  |  Ships in fleets: %d  |  Missions enabled: %d", (int)rows.size(), ships_total,
                      missions_enabled);

          // Table.
          ImGui::Separator();
          const ImGuiTableFlags table_flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersV |
                                             ImGuiTableFlags_BordersOuter | ImGuiTableFlags_Resizable |
                                             ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable |
                                             ImGuiTableFlags_SortMulti;

          const float table_h = std::max(240.0f, ImGui::GetContentRegionAvail().y);
          if (ImGui::BeginTable("fleet_manager_table", 9, table_flags, ImVec2(0.0f, table_h))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort);
            ImGui::TableSetupColumn("Faction");
            ImGui::TableSetupColumn("System");
            ImGui::TableSetupColumn("Ships", ImGuiTableColumnFlags_PreferSortDescending);
            ImGui::TableSetupColumn("Mission");
            ImGui::TableSetupColumn("Fuel%", ImGuiTableColumnFlags_PreferSortDescending);
            ImGui::TableSetupColumn("HP%", ImGuiTableColumnFlags_PreferSortDescending);
            ImGui::TableSetupColumn("Speed", ImGuiTableColumnFlags_PreferSortDescending);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed, 64.0f);
            ImGui::TableHeadersRow();

            if (ImGuiTableSortSpecs* sort_specs = ImGui::TableGetSortSpecs()) {
              if (sort_specs->SpecsDirty) {
                std::sort(rows.begin(), rows.end(), [&](const FleetRow& a, const FleetRow& b) {
                  return compare_rows(a, b, sort_specs) < 0;
                });
                sort_specs->SpecsDirty = false;
              }
            }

            for (const FleetRow& r : rows) {
              const Fleet& f = *r.fleet;
              const bool selected = (ui.selected_fleet_id == r.id);

              ImGui::TableNextRow();

              // Name (selectable spans all columns).
              ImGui::TableSetColumnIndex(ColName);
              {
                std::string label = f.name + "##fm_row_" + fmt_id(r.id);
                const ImGuiSelectableFlags sel_flags = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick;
                if (ImGui::Selectable(label.c_str(), selected, sel_flags)) {
                  ui.selected_fleet_id = r.id;
                  ui.show_details_window = true;
                  ui.request_details_tab = DetailsTab::Fleet;

                  if (ImGui::IsMouseDoubleClicked(0)) {
                    // Focus the map on the leader.
                    if (r.leader) {
                      selected_ship = r.leader->id;
                      s.selected_system = r.leader->system_id;
                      ui.show_map_window = true;
                      ui.request_map_tab = MapTab::System;
                    }
                  }
                }
              }

              ImGui::TableSetColumnIndex(ColFaction);
              ImGui::TextUnformatted(r.faction ? r.faction->name.c_str() : "(unknown)");

              ImGui::TableSetColumnIndex(ColSystem);
              ImGui::TextUnformatted(r.system_name.c_str());

              ImGui::TableSetColumnIndex(ColShips);
              ImGui::Text("%d", r.ship_count);

              ImGui::TableSetColumnIndex(ColMission);
              ImGui::TextUnformatted(fleet_mission_label(r.mission));

              ImGui::TableSetColumnIndex(ColFuel);
              if (r.has_min_fuel) {
                ImGui::Text("%d", (int)std::lround(r.min_fuel * 100.0));
              } else {
                ImGui::TextDisabled("--");
              }

              ImGui::TableSetColumnIndex(ColHP);
              if (r.has_min_hp) {
                ImGui::Text("%d", (int)std::lround(r.min_hp * 100.0));
              } else {
                ImGui::TextDisabled("--");
              }

              ImGui::TableSetColumnIndex(ColSpeed);
              if (r.leader_speed_km_s > 0.0) {
                ImGui::Text("%.0f", r.leader_speed_km_s);
              } else {
                ImGui::TextDisabled("--");
              }

              ImGui::TableSetColumnIndex(ColActions);
              {
                const bool can_focus = (r.leader != nullptr);
                if (!can_focus) {
                  ImGui::TextDisabled("(no leader)");
                } else {
                  if (ImGui::SmallButton(("Focus##fm_focus_" + fmt_id(r.id)).c_str())) {
                    selected_ship = r.leader->id;
                    s.selected_system = r.leader->system_id;
                    ui.show_map_window = true;
                    ui.request_map_tab = MapTab::System;
                  }
                }
              }
            }

            ImGui::EndTable();
          }
        }

        // --- Right: inspector ---
        ImGui::TableSetColumnIndex(1);
        {
          ImGui::SeparatorText("Inspector");

          const Fleet* selected_fleet = (ui.selected_fleet_id != kInvalidId) ? find_ptr(s.fleets, ui.selected_fleet_id) : nullptr;
          if (!selected_fleet) {
            ImGui::TextDisabled("Select a fleet from the list to inspect it.");
            ImGui::EndTable();
            ImGui::EndTabItem();
            ImGui::EndTabBar();
            ImGui::End();
            return;
          }

          const Faction* fac = find_ptr(s.factions, selected_fleet->faction_id);
          const Ship* leader = resolve_fleet_leader(s, *selected_fleet);
          const Id sys_id = resolve_fleet_system_id(s, *selected_fleet);
          const StarSystem* sys = (sys_id != kInvalidId) ? find_ptr(s.systems, sys_id) : nullptr;

          ImGui::Text("%s", selected_fleet->name.c_str());
          ImGui::TextDisabled("Faction: %s", fac ? fac->name.c_str() : "(unknown)");
          ImGui::TextDisabled("Ships: %d", (int)selected_fleet->ship_ids.size());
          if (sys) ImGui::TextDisabled("System: %s", sys->name.c_str());

          if (ImGui::Button("Open Fleet tab")) {
            ui.show_details_window = true;
            ui.request_details_tab = DetailsTab::Fleet;
          }
          ImGui::SameLine();
          if (leader && ImGui::Button("Focus leader")) {
            selected_ship = leader->id;
            s.selected_system = leader->system_id;
            ui.show_map_window = true;
            ui.request_map_tab = MapTab::System;
          }

          // --- Rename / disband ---
          {
            static Id rename_for = kInvalidId;
            static char rename_buf[128] = "";
            if (rename_for != selected_fleet->id) {
              std::snprintf(rename_buf, sizeof(rename_buf), "%s", selected_fleet->name.c_str());
              rename_for = selected_fleet->id;
            }

            ImGui::Separator();
            ImGui::InputText("Name##fleet_mgr_rename", rename_buf, IM_ARRAYSIZE(rename_buf));
            if (ImGui::SmallButton("Rename##fleet_mgr_rename_btn")) {
              if (!sim.rename_fleet(selected_fleet->id, rename_buf)) {
                nebula4x::log::warn("Fleet rename failed (empty name?)");
              }
            }

            ImGui::SameLine();
            if (ImGui::SmallButton("Disband##fleet_mgr_disband")) {
              ImGui::OpenPopup("fleet_mgr_disband_confirm");
            }

            if (ImGui::BeginPopupModal("fleet_mgr_disband_confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
              ImGui::TextWrapped("Disband fleet '%s'? Ships will become unassigned.", selected_fleet->name.c_str());
              if (ImGui::Button("Disband", ImVec2(120, 0))) {
                sim.disband_fleet(selected_fleet->id);
                ui.selected_fleet_id = kInvalidId;
                ImGui::CloseCurrentPopup();
              }
              ImGui::SameLine();
              if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
              }
              ImGui::EndPopup();
            }
          }

          // --- Leader selection ---
          {
            ImGui::SeparatorText("Leader");
            const char* leader_label = leader ? leader->name.c_str() : "(none)";
            if (ImGui::BeginCombo("Leader##fleet_mgr_leader", leader_label)) {
              for (Id sid : selected_fleet->ship_ids) {
                const auto* sh = find_ptr(s.ships, sid);
                if (!sh) continue;
                const bool sel = (selected_fleet->leader_ship_id == sid);
                std::string item = sh->name + "##fm_leader_pick_" + fmt_id(sid);
                if (ImGui::Selectable(item.c_str(), sel)) {
                  sim.set_fleet_leader(selected_fleet->id, sid);
                }
              }
              ImGui::EndCombo();
            }
          }

          // --- Mission quick controls ---
          {
            ImGui::SeparatorText("Mission");
            ImGui::TextDisabled("Current: %s", fleet_mission_label(selected_fleet->mission.type));
            const std::string target = fleet_mission_target_brief(sim, *selected_fleet);
            if (!target.empty()) ImGui::TextDisabled("Target: %s", target.c_str());

            Fleet* fm = find_ptr(s.fleets, selected_fleet->id);
            if (fm) {
              const Id preferred_system_id = sys_id;

              if (ImGui::SmallButton("None##fm_m_none")) {
                fm->mission.type = FleetMissionType::None;
                reset_mission_runtime(fm->mission);
              }
              ImGui::SameLine();
              if (ImGui::SmallButton("Patrol##fm_m_patrol")) {
                fm->mission.type = FleetMissionType::PatrolSystem;
                reset_mission_runtime(fm->mission);
                if (fm->mission.patrol_system_id == kInvalidId) fm->mission.patrol_system_id = preferred_system_id;
              }
              ImGui::SameLine();
              if (ImGui::SmallButton("Explore##fm_m_explore")) {
                fm->mission.type = FleetMissionType::Explore;
                reset_mission_runtime(fm->mission);
              }
              ImGui::SameLine();
              if (ImGui::SmallButton("Guard JP##fm_m_guard")) {
                fm->mission.type = FleetMissionType::GuardJumpPoint;
                reset_mission_runtime(fm->mission);
                seed_mission_defaults(sim, fm->mission, fm->faction_id, preferred_system_id);
              }
              ImGui::SameLine();
              if (ImGui::SmallButton("Defend##fm_m_defend")) {
                fm->mission.type = FleetMissionType::DefendColony;
                reset_mission_runtime(fm->mission);
                seed_mission_defaults(sim, fm->mission, fm->faction_id, preferred_system_id);
              }

              ImGui::Spacing();
              ImGui::TextDisabled("Tip: use the Fleet tab for full mission parameter editing.");
            }
          }

          // --- Route planner (jump network) ---
          {
            ImGui::SeparatorText("Route Planner");
            ImGui::TextDisabled("Preview jump-network routes and issue Travel-to-System orders.");

            static Id target_system_id = kInvalidId;
            static bool include_queued_jumps = false;
            static bool clear_existing_orders = false;

            // Ensure target is valid under fog-of-war constraints.
            if (target_system_id != kInvalidId && ui.fog_of_war && fac) {
              if (!sim.is_system_discovered_by_faction(fac->id, target_system_id)) {
                target_system_id = kInvalidId;
              }
            }

            const StarSystem* tgt_sys = (target_system_id != kInvalidId) ? find_ptr(s.systems, target_system_id) : nullptr;
            const char* tgt_label = tgt_sys ? tgt_sys->name.c_str() : "(select destination system)";

            if (ImGui::BeginCombo("Destination##fm_route_dest", tgt_label)) {
              for (Id sid : sorted_keys(s.systems)) {
                const auto* ss = find_ptr(s.systems, sid);
                if (!ss) continue;
                if (ui.fog_of_war && fac) {
                  if (!sim.is_system_discovered_by_faction(fac->id, sid)) continue;
                }
                const bool sel = (target_system_id == sid);
                std::string item = ss->name + "##fm_route_sys_" + fmt_id(sid);
                if (ImGui::Selectable(item.c_str(), sel)) {
                  target_system_id = sid;
                }
              }
              ImGui::EndCombo();
            }

            ImGui::Checkbox("Include queued jumps", &include_queued_jumps);
            ImGui::SameLine();
            ImGui::Checkbox("Clear existing orders", &clear_existing_orders);

            if (target_system_id == kInvalidId) {
              ImGui::TextDisabled("Select a destination to preview a route.");
            } else {
              // Small memoization to avoid recompute spam.
              static Id last_fleet_id = kInvalidId;
              static Id last_target_id = kInvalidId;
              static bool last_include_queued = false;
              static std::optional<JumpRoutePlan> cached;

              if (last_fleet_id != selected_fleet->id || last_target_id != target_system_id ||
                  last_include_queued != include_queued_jumps) {
                cached = sim.plan_jump_route_for_fleet(selected_fleet->id, target_system_id, ui.fog_of_war,
                                                     include_queued_jumps);
                last_fleet_id = selected_fleet->id;
                last_target_id = target_system_id;
                last_include_queued = include_queued_jumps;
              }

              if (!cached.has_value()) {
                ImGui::TextDisabled("No route found (unknown exits or disconnected network)." );
              } else {
                const JumpRoutePlan& plan = *cached;
                ImGui::Text("Hops: %d", (int)std::max<std::size_t>(0, plan.systems.size() > 0 ? plan.systems.size() - 1 : 0));
                ImGui::SameLine();
                ImGui::Text("ETA: %.1f d", plan.eta_days);
                ImGui::SameLine();
                ImGui::TextDisabled("Dist: %.1f mkm", plan.distance_mkm);

                if (ImGui::BeginChild("##fm_route_list", ImVec2(0, 120), true)) {
                  for (std::size_t i = 0; i < plan.systems.size(); ++i) {
                    const Id sid = plan.systems[i];
                    const auto* ss = find_ptr(s.systems, sid);
                    if (!ss) continue;
                    if (i == 0) {
                      ImGui::BulletText("%s (start)", ss->name.c_str());
                    } else if (i + 1 == plan.systems.size()) {
                      ImGui::BulletText("%s (dest)", ss->name.c_str());
                    } else {
                      ImGui::BulletText("%s", ss->name.c_str());
                    }
                  }
                  ImGui::EndChild();
                }

                if (ImGui::Button("Issue Travel Orders")) {
                  if (clear_existing_orders) {
                    sim.clear_fleet_orders(selected_fleet->id);
                  }
                  const bool ok = sim.issue_fleet_travel_to_system(selected_fleet->id, target_system_id, ui.fog_of_war);
                  if (!ok) {
                    nebula4x::log::warn("Couldn't issue fleet travel orders (route may be invalid under fog-of-war)." );
                  }
                }
              }
            }
          }

          // --- Members (quick glance) ---
          {
            ImGui::SeparatorText("Members");
            if (ImGui::BeginChild("##fm_members", ImVec2(0, 0), true)) {
              for (Id sid : selected_fleet->ship_ids) {
                const auto* sh = find_ptr(s.ships, sid);
                if (!sh) continue;

                const auto* d = sim.find_design(sh->design_id);
                const char* role = d ? ship_role_label(d->role) : "?";

                // Lightweight health/fuel.
                double hp_frac = 0.0;
                double fuel_frac = 0.0;
                bool has_hp = false;
                bool has_fuel = false;
                if (d && d->max_hp > 0.0) {
                  hp_frac = std::clamp(sh->hp / d->max_hp, 0.0, 1.0);
                  has_hp = true;
                }
                if (d && d->fuel_capacity_tons > 0.0 && sh->fuel_tons >= 0.0) {
                  fuel_frac = std::clamp(sh->fuel_tons / d->fuel_capacity_tons, 0.0, 1.0);
                  has_fuel = true;
                }

                std::string line = sh->name + " (" + role + ")";
                if (ImGui::Selectable((line + "##fm_ship_" + fmt_id(sid)).c_str(), selected_ship == sid)) {
                  selected_ship = sid;
                  s.selected_system = sh->system_id;
                  ui.show_details_window = true;
                  ui.request_details_tab = DetailsTab::Ship;
                  ui.show_map_window = true;
                  ui.request_map_tab = MapTab::System;
                }

                ImGui::SameLine();
                if (has_hp) {
                  ImGui::TextDisabled("HP %d%%", (int)std::lround(hp_frac * 100.0));
                }
                if (has_fuel) {
                  ImGui::SameLine();
                  ImGui::TextDisabled("Fuel %d%%", (int)std::lround(fuel_frac * 100.0));
                }
              }
              ImGui::EndChild();
            }
          }
        }

        ImGui::EndTable();
      }

      ImGui::EndTabItem();
    }

    // --- Fleet Forge tab ---
    if (ImGui::BeginTabItem("Fleet Forge")) {
      ImGui::TextDisabled("Suggestions for creating fleets from unassigned ships (grouped by system + role).");

      static bool only_viewer_faction = true;
      static int min_ships = 2;
      static bool auto_assign_mission = true;

      ImGui::Checkbox("Only viewer faction", &only_viewer_faction);
      ImGui::SameLine();
      ImGui::SliderInt("Min ships", &min_ships, 1, 12);
      ImGui::SameLine();
      ImGui::Checkbox("Auto-assign mission", &auto_assign_mission);

      // Build suggestions.
      std::unordered_map<ForgeKey, ForgeSuggestion, ForgeKeyHash> groups;
      groups.reserve(s.ships.size() / 2);

      for (const auto& kv : s.ships) {
        const Ship& sh = kv.second;

        if (only_viewer_faction && ui.viewer_faction_id != kInvalidId && sh.faction_id != ui.viewer_faction_id) {
          continue;
        }

        if (sim.fleet_for_ship(sh.id) != kInvalidId) continue;  // already in fleet

        const auto* d = sim.find_design(sh.design_id);
        const ShipRole role = d ? d->role : ShipRole::Unknown;

        ForgeKey key;
        key.faction_id = sh.faction_id;
        key.system_id = sh.system_id;
        key.role = role;

        auto& g = groups[key];
        g.key = key;
        g.ship_ids.push_back(sh.id);
      }

      std::vector<ForgeSuggestion> sugg;
      sugg.reserve(groups.size());
      for (auto& kv : groups) {
        ForgeSuggestion& g = kv.second;
        if ((int)g.ship_ids.size() < min_ships) continue;
        // Sort member ids for determinism.
        std::sort(g.ship_ids.begin(), g.ship_ids.end());
        sugg.push_back(std::move(g));
      }

      // Deterministic ordering: by system name, then role, then ship count desc.
      std::sort(sugg.begin(), sugg.end(), [&](const ForgeSuggestion& a, const ForgeSuggestion& b) {
        const std::string asn = system_name(s, a.key.system_id);
        const std::string bsn = system_name(s, b.key.system_id);
        if (asn != bsn) return asn < bsn;
        if (a.key.role != b.key.role) return (int)a.key.role < (int)b.key.role;
        if (a.ship_ids.size() != b.ship_ids.size()) return a.ship_ids.size() > b.ship_ids.size();
        return a.key.faction_id < b.key.faction_id;
      });

      ImGui::Separator();
      ImGui::Text("Suggestions: %d", (int)sugg.size());

      const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV |
                                 ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
      const float h = std::max(260.0f, ImGui::GetContentRegionAvail().y);
      if (ImGui::BeginTable("fleet_forge_table", 6, tf, ImVec2(0, h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Faction");
        ImGui::TableSetupColumn("System");
        ImGui::TableSetupColumn("Role");
        ImGui::TableSetupColumn("Ships");
        ImGui::TableSetupColumn("Name preview");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableHeadersRow();

        for (const ForgeSuggestion& g : sugg) {
          const Faction* fac = find_ptr(s.factions, g.key.faction_id);
          const std::string sysn = system_name(s, g.key.system_id);

          std::string base_name = sysn;
          base_name += " ";
          base_name += ship_role_label(g.key.role);
          base_name += " Fleet";

          const std::string name_preview = make_unique_fleet_name(s, g.key.faction_id, base_name);

          ImGui::TableNextRow();

          ImGui::TableSetColumnIndex(0);
          ImGui::TextUnformatted(fac ? fac->name.c_str() : "(unknown)");

          ImGui::TableSetColumnIndex(1);
          ImGui::TextUnformatted(sysn.c_str());

          ImGui::TableSetColumnIndex(2);
          ImGui::TextUnformatted(ship_role_label(g.key.role));

          ImGui::TableSetColumnIndex(3);
          ImGui::Text("%d", (int)g.ship_ids.size());

          ImGui::TableSetColumnIndex(4);
          ImGui::TextUnformatted(name_preview.c_str());

          ImGui::TableSetColumnIndex(5);
          if (ImGui::SmallButton(("Create##forge_create_" + fmt_id(g.key.faction_id) + "_" + fmt_id(g.key.system_id) + "_" + std::to_string((int)g.key.role)).c_str())) {
            std::string err;
            const Id fid = sim.create_fleet(g.key.faction_id, name_preview, g.ship_ids, &err);
            if (fid == kInvalidId) {
              nebula4x::log::warn(std::string("Create fleet failed: ") + (err.empty() ? "(unknown)" : err));
            } else {
              ui.selected_fleet_id = fid;
              ui.show_details_window = true;
              ui.request_details_tab = DetailsTab::Fleet;

              if (auto_assign_mission) {
                if (Fleet* nf = find_ptr(s.fleets, fid)) {
                  const FleetMissionType mt = suggested_mission_for_role(g.key.role);
                  if (mt != FleetMissionType::None) {
                    nf->mission.type = mt;
                    reset_mission_runtime(nf->mission);
                    seed_mission_defaults(sim, nf->mission, nf->faction_id, g.key.system_id);
                  }
                }
              }

              // Focus the new fleet's leader when possible.
              if (const auto* fl = find_ptr(s.fleets, fid)) {
                const auto* leader = resolve_fleet_leader(s, *fl);
                if (leader) {
                  selected_ship = leader->id;
                  s.selected_system = leader->system_id;
                  ui.show_map_window = true;
                  ui.request_map_tab = MapTab::System;
                }
              }
            }
          }
        }

        ImGui::EndTable();
      }

      ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
  }

  ImGui::End();
}

}  // namespace nebula4x::ui
