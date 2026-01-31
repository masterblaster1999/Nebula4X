#include "nebula4x/core/state_validation.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace nebula4x {

namespace {

void push(std::vector<std::string>& out, std::string msg) { out.push_back(std::move(msg)); }

template <typename... Parts>
std::string join(Parts&&... parts) {
  std::ostringstream ss;
  (ss << ... << std::forward<Parts>(parts));
  return ss.str();
}

unsigned long long id_u64(Id id) { return static_cast<unsigned long long>(id); }

// Many core containers are stored as std::unordered_map for convenience.
// Iteration order of unordered_map is not specified, so relying on it can
// introduce cross-platform nondeterminism.
template <typename Map>
std::vector<typename Map::key_type> sorted_keys(const Map& m) {
  std::vector<typename Map::key_type> out;
  out.reserve(m.size());
  for (const auto& kv : m) out.push_back(kv.first);
  std::sort(out.begin(), out.end());
  return out;
}

template <typename T>
void sort_unique(std::vector<T>& v) {
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
}

bool design_exists(const GameState& s, const ContentDB& content, const std::string& id) {
  if (id.empty()) return false;
  if (s.custom_designs.find(id) != s.custom_designs.end()) return true;
  return content.designs.find(id) != content.designs.end();
}

bool installation_exists(const ContentDB& content, const std::string& id) {
  if (id.empty()) return false;
  return content.installations.find(id) != content.installations.end();
}

bool component_exists(const ContentDB& content, const std::string& id) {
  if (id.empty()) return false;
  return content.components.find(id) != content.components.end();
}

bool tech_exists(const ContentDB& content, const std::string& id) {
  if (id.empty()) return false;
  return content.techs.find(id) != content.techs.end();
}

} // namespace

std::vector<std::string> validate_game_state(const GameState& s, const ContentDB* content) {
  std::vector<std::string> errors;

  auto has_system = [&](Id id) { return s.systems.find(id) != s.systems.end(); };
  auto has_body = [&](Id id) { return s.bodies.find(id) != s.bodies.end(); };
  auto has_jump = [&](Id id) { return s.jump_points.find(id) != s.jump_points.end(); };
  auto has_ship = [&](Id id) { return s.ships.find(id) != s.ships.end(); };
  auto has_wreck = [&](Id id) { return s.wrecks.find(id) != s.wrecks.end(); };
  auto has_anomaly = [&](Id id) { return s.anomalies.find(id) != s.anomalies.end(); };
  auto has_colony = [&](Id id) { return s.colonies.find(id) != s.colonies.end(); };
  auto has_faction = [&](Id id) { return s.factions.find(id) != s.factions.end(); };

  // --- Map key/id mismatches + compute max id ---
  Id max_id = 0;
  const auto bump_max = [&](Id id) {
    if (id != kInvalidId && id > max_id) max_id = id;
  };

  for (const auto& [id, sys] : s.systems) {
    bump_max(id);
    if (sys.id != kInvalidId && sys.id != id) {
      push(errors, join("StarSystem id mismatch: key=", id_u64(id), " value.id=", id_u64(sys.id)));
    }
  }
  for (const auto& [id, b] : s.bodies) {
    bump_max(id);
    if (b.id != kInvalidId && b.id != id) {
      push(errors, join("Body id mismatch: key=", id_u64(id), " value.id=", id_u64(b.id)));
    }
  }
  for (const auto& [id, jp] : s.jump_points) {
    bump_max(id);
    if (jp.id != kInvalidId && jp.id != id) {
      push(errors, join("JumpPoint id mismatch: key=", id_u64(id), " value.id=", id_u64(jp.id)));
    }
  }
  for (const auto& [id, sh] : s.ships) {
    bump_max(id);
    if (sh.id != kInvalidId && sh.id != id) {
      push(errors, join("Ship id mismatch: key=", id_u64(id), " value.id=", id_u64(sh.id)));
    }
  }
  for (const auto& [id, w] : s.wrecks) {
    bump_max(id);
    if (w.id != kInvalidId && w.id != id) {
      push(errors, join("Wreck id mismatch: key=", id_u64(id), " value.id=", id_u64(w.id)));
    }
  }
  for (const auto& [id, c] : s.colonies) {
    bump_max(id);
    if (c.id != kInvalidId && c.id != id) {
      push(errors, join("Colony id mismatch: key=", id_u64(id), " value.id=", id_u64(c.id)));
    }
  }
  for (const auto& [id, f] : s.factions) {
    bump_max(id);
    if (f.id != kInvalidId && f.id != id) {
      push(errors, join("Faction id mismatch: key=", id_u64(id), " value.id=", id_u64(f.id)));
    }
  }
  for (const auto& [id, fl] : s.fleets) {
    bump_max(id);
    if (fl.id != kInvalidId && fl.id != id) {
      push(errors, join("Fleet id mismatch: key=", id_u64(id), " value.id=", id_u64(fl.id)));
    }
  }
  for (const auto& [id, c] : s.contracts) {
    bump_max(id);
    if (c.id != kInvalidId && c.id != id) {
      push(errors, join("Contract id mismatch: key=", id_u64(id), " value.id=", id_u64(c.id)));
    }
  }

  if (s.next_id != kInvalidId && max_id != kInvalidId && s.next_id <= max_id) {
    push(errors,
         join("next_id is not monotonic: next_id=", id_u64(s.next_id), " max_existing_id=", id_u64(max_id)));
  }

  // --- selected system ---
  if (s.selected_system != kInvalidId && !has_system(s.selected_system)) {
    push(errors, join("selected_system references unknown system id ", id_u64(s.selected_system)));
  }

  // --- Contracts ---
  for (const auto& [cid, c] : s.contracts) {
    if (c.issuer_faction_id != kInvalidId && !has_faction(c.issuer_faction_id)) {
      push(errors, join("Contract ", id_u64(cid), " references missing issuer_faction_id ", id_u64(c.issuer_faction_id)));
    }
    if (c.assignee_faction_id != kInvalidId && !has_faction(c.assignee_faction_id)) {
      push(errors, join("Contract ", id_u64(cid), " references missing assignee_faction_id ", id_u64(c.assignee_faction_id)));
    }
    if (c.system_id != kInvalidId && !has_system(c.system_id)) {
      push(errors, join("Contract ", id_u64(cid), " references missing system_id ", id_u64(c.system_id)));
    }
    if (c.assigned_ship_id != kInvalidId && !has_ship(c.assigned_ship_id)) {
      push(errors, join("Contract ", id_u64(cid), " references missing assigned_ship_id ", id_u64(c.assigned_ship_id)));
    }
    if (c.assigned_fleet_id != kInvalidId && s.fleets.find(c.assigned_fleet_id) == s.fleets.end()) {
      push(errors, join("Contract ", id_u64(cid), " references missing assigned_fleet_id ", id_u64(c.assigned_fleet_id)));
    }

    if (c.target_id != kInvalidId) {
      switch (c.kind) {
        case ContractKind::InvestigateAnomaly:
          if (!has_anomaly(c.target_id)) {
            push(errors, join("Contract ", id_u64(cid), " targets missing anomaly id ", id_u64(c.target_id)));
          }
          break;
        case ContractKind::SalvageWreck:
          if (!has_wreck(c.target_id)) {
            push(errors, join("Contract ", id_u64(cid), " targets missing wreck id ", id_u64(c.target_id)));
          }
          break;
        case ContractKind::SurveyJumpPoint:
          if (!has_jump(c.target_id)) {
            push(errors, join("Contract ", id_u64(cid), " targets missing jump point id ", id_u64(c.target_id)));
          }
          break;
        case ContractKind::BountyPirate:
          // Bounty contracts may legitimately have a missing target_id if the target
          // has already been destroyed and attribution has been recorded.
          if (!has_ship(c.target_id) && c.target_destroyed_day == 0) {
            push(errors, join("Contract ", id_u64(cid), " targets missing bounty ship id ", id_u64(c.target_id)));
          }
          if (c.target_id2 != kInvalidId && !has_system(c.target_id2)) {
            push(errors, join("Contract ", id_u64(cid), " bounty contract references missing system id ", id_u64(c.target_id2)));
          }
          break;
        case ContractKind::EscortConvoy:
          if (!has_ship(c.target_id)) {
            push(errors, join("Contract ", id_u64(cid), " targets missing convoy ship id ", id_u64(c.target_id)));
          }
          if (c.target_id2 == kInvalidId) {
            push(errors, join("Contract ", id_u64(cid), " escort convoy contract is missing destination system id"));
          } else if (!has_system(c.target_id2)) {
            push(errors,
                 join("Contract ", id_u64(cid), " escort convoy contract targets missing destination system id ",
                      id_u64(c.target_id2)));
          }
          break;
      }
    }
  }

  // --- Systems cross-reference lists ---
  for (const auto& [sys_id, sys] : s.systems) {
    for (const auto bid : sys.bodies) {
      if (!has_body(bid)) {
        push(errors, join("System ", id_u64(sys_id), " ('", sys.name, "') references missing body ", id_u64(bid)));
        continue;
      }
      const auto& b = s.bodies.at(bid);
      if (b.system_id != sys_id) {
        push(errors,
             join("Body ", id_u64(bid), " ('", b.name, "') has system_id=", id_u64(b.system_id),
                  " but is listed under system ", id_u64(sys_id)));
      }
    }

    for (const auto sid : sys.ships) {
      if (!has_ship(sid)) {
        push(errors, join("System ", id_u64(sys_id), " ('", sys.name, "') references missing ship ", id_u64(sid)));
        continue;
      }
      const auto& sh = s.ships.at(sid);
      if (sh.system_id != sys_id) {
        push(errors,
             join("Ship ", id_u64(sid), " ('", sh.name, "') has system_id=", id_u64(sh.system_id),
                  " but is listed under system ", id_u64(sys_id)));
      }
    }

    for (const auto jpid : sys.jump_points) {
      if (!has_jump(jpid)) {
        push(errors,
             join("System ", id_u64(sys_id), " ('", sys.name, "') references missing jump point ", id_u64(jpid)));
        continue;
      }
      const auto& jp = s.jump_points.at(jpid);
      if (jp.system_id != sys_id) {
        push(errors,
             join("JumpPoint ", id_u64(jpid), " ('", jp.name, "') has system_id=", id_u64(jp.system_id),
                  " but is listed under system ", id_u64(sys_id)));
      }
    }
  }

  // --- Bodies ---
  for (const auto& [bid, b] : s.bodies) {
    if (b.system_id == kInvalidId || !has_system(b.system_id)) {
      push(errors, join("Body ", id_u64(bid), " ('", b.name, "') references unknown system_id ", id_u64(b.system_id)));
    }

    // Validate parent-body orbits (moons, binaries, etc).
    if (b.parent_body_id != kInvalidId) {
      if (b.parent_body_id == bid) {
        push(errors, join("Body ", id_u64(bid), " ('", b.name, "') parent_body_id references itself"));
      } else if (!has_body(b.parent_body_id)) {
        push(errors, join("Body ", id_u64(bid), " ('", b.name, "') references missing parent_body_id ",
                          id_u64(b.parent_body_id)));
      } else {
        const auto& parent = s.bodies.at(b.parent_body_id);
        if (parent.system_id != b.system_id) {
          push(errors, join("Body ", id_u64(bid), " ('", b.name, "') parent_body_id ", id_u64(b.parent_body_id),
                            " is in a different system (parent.system_id=", id_u64(parent.system_id),
                            ", body.system_id=", id_u64(b.system_id), ")"));
        }
      }
    }

    // Validate mineral deposit data (finite mining).
    for (const auto& [mineral, tons] : b.mineral_deposits) {
      if (mineral.empty()) {
        push(errors, join("Body ", id_u64(bid), " ('", b.name, "') has an empty mineral key in mineral_deposits"));
        continue;
      }
      if (!std::isfinite(tons)) {
        push(errors,
             join("Body ", id_u64(bid), " ('", b.name, "') has non-finite deposit for '", mineral, "': ", tons));
        continue;
      }
      if (tons < -1e-9) {
        push(errors,
             join("Body ", id_u64(bid), " ('", b.name, "') has negative deposit for '", mineral, "': ", tons));
      }
    }
  }

  // --- Jump points ---
  for (const auto& [jpid, jp] : s.jump_points) {
    if (jp.system_id == kInvalidId || !has_system(jp.system_id)) {
      push(errors,
           join("JumpPoint ", id_u64(jpid), " ('", jp.name, "') references unknown system_id ",
                id_u64(jp.system_id)));
    }

    if (jp.linked_jump_id != kInvalidId) {
      if (jp.linked_jump_id == jpid) {
        push(errors, join("JumpPoint ", id_u64(jpid), " ('", jp.name, "') links to itself"));
      } else if (!has_jump(jp.linked_jump_id)) {
        push(errors,
             join("JumpPoint ", id_u64(jpid), " ('", jp.name, "') links to missing jump ",
                  id_u64(jp.linked_jump_id)));
      } else {
        const auto& other = s.jump_points.at(jp.linked_jump_id);
        if (other.linked_jump_id != jpid) {
          push(errors,
               join("JumpPoint ", id_u64(jpid), " ('", jp.name, "') links to ", id_u64(jp.linked_jump_id),
                    " but that jump links to ", id_u64(other.linked_jump_id)));
        }
      }
    }
  }

  // --- Ships ---
  for (const auto& [sid, sh] : s.ships) {
    if (sh.system_id == kInvalidId || !has_system(sh.system_id)) {
      push(errors,
           join("Ship ", id_u64(sid), " ('", sh.name, "') references unknown system_id ", id_u64(sh.system_id)));
    }
    if (sh.faction_id == kInvalidId || !has_faction(sh.faction_id)) {
      push(errors,
           join("Ship ", id_u64(sid), " ('", sh.name, "') references unknown faction_id ", id_u64(sh.faction_id)));
    }
    if (sh.design_id.empty()) {
      push(errors, join("Ship ", id_u64(sid), " ('", sh.name, "') has empty design_id"));
    } else if (content) {
      if (!design_exists(s, *content, sh.design_id)) {
        push(errors,
             join("Ship ", id_u64(sid), " ('", sh.name, "') references unknown design_id '", sh.design_id, "'"));
      }
    }

    // SensorMode validation.
    {
      const int sm = static_cast<int>(sh.sensor_mode);
      if (sm < 0 || sm > 2) {
        push(errors,
             join("Ship ", id_u64(sid), " ('", sh.name, "') has invalid sensor_mode ", sm));
      }
    }

    // Auto-* threshold validation.
    {
      if (!std::isfinite(sh.auto_refuel_threshold_fraction) || sh.auto_refuel_threshold_fraction < 0.0 ||
          sh.auto_refuel_threshold_fraction > 1.0) {
        push(errors,
             join("Ship ", id_u64(sid), " ('", sh.name, "') has invalid auto_refuel_threshold_fraction ",
                  sh.auto_refuel_threshold_fraction));
      }
      if (!std::isfinite(sh.auto_repair_threshold_fraction) || sh.auto_repair_threshold_fraction < 0.0 ||
          sh.auto_repair_threshold_fraction > 1.0) {
        push(errors,
             join("Ship ", id_u64(sid), " ('", sh.name, "') has invalid auto_repair_threshold_fraction ",
                  sh.auto_repair_threshold_fraction));
      }

      if (!std::isfinite(sh.auto_tanker_reserve_fraction) || sh.auto_tanker_reserve_fraction < 0.0 ||
          sh.auto_tanker_reserve_fraction > 1.0) {
        push(errors,
             join("Ship ", id_u64(sid), " ('", sh.name, "') has invalid auto_tanker_reserve_fraction ",
                  sh.auto_tanker_reserve_fraction));
      }
    }

    // RepairPriority validation.
    {
      const int rp = static_cast<int>(sh.repair_priority);
      if (rp < 0 || rp > 2) {
        push(errors,
             join("Ship ", id_u64(sid), " ('", sh.name, "') has invalid repair_priority ", rp));
      }
    }

  }

  // --- Wrecks ---
  for (const auto& [wid, w] : s.wrecks) {
    if (w.system_id == kInvalidId || !has_system(w.system_id)) {
      push(errors,
           join("Wreck ", id_u64(wid), " ('", w.name, "') references unknown system_id ", id_u64(w.system_id)));
    }
    if (w.minerals.empty()) {
      push(errors, join("Wreck ", id_u64(wid), " ('", w.name, "') has no minerals"));
    }
    for (const auto& [m, tons] : w.minerals) {
      if (m.empty()) {
        push(errors, join("Wreck ", id_u64(wid), " has empty mineral key"));
      }
      if (!std::isfinite(tons) || tons < 0.0) {
        push(errors,
             join("Wreck ", id_u64(wid), " ('", w.name, "') has invalid mineral tons for '", m,
                  "': ", tons));
      }
    }
    // WreckKind validation.
    {
      const int k = static_cast<int>(w.kind);
      if (k < static_cast<int>(WreckKind::Ship) || k > static_cast<int>(WreckKind::Debris)) {
        push(errors, join("Wreck ", id_u64(wid), " ('", w.name, "') has invalid kind ", k));
      }
    }

    // Cache wrecks (e.g. anomaly overflow caches) are not ship hulls and must
    // not carry source-ship metadata that could enable reverse engineering.
    if (w.kind == WreckKind::Cache) {
      if (w.source_ship_id != kInvalidId || w.source_faction_id != kInvalidId || !w.source_design_id.empty()) {
        push(errors, join("Wreck ", id_u64(wid), " ('", w.name, "') is kind=cache but has non-empty source metadata"));
      }
    }

    if (w.created_day < 0) {
      push(errors, join("Wreck ", id_u64(wid), " ('", w.name, "') has negative created_day ", w.created_day));
    }
  }

  // --- Ship orders ---
  for (const auto& [ship_id, so] : s.ship_orders) {
    const auto it_ship = s.ships.find(ship_id);
    const bool ship_exists = (it_ship != s.ships.end());
    const std::string ship_name = ship_exists ? it_ship->second.name : std::string{};

    if (!ship_exists) {
      push(errors, join("ship_orders contains entry for missing ship id ", id_u64(ship_id)));
    }

    auto validate_order_list = [&](const std::vector<Order>& list, const char* list_name) {
      for (std::size_t i = 0; i < list.size(); ++i) {
        const Order& ord_any = list[i];
        std::visit(
            [&](const auto& ord) {
              using T = std::decay_t<decltype(ord)>;

              auto prefix = [&]() {
                if (ship_exists) {
                  return join("Ship ", id_u64(ship_id), " ('", ship_name, "') ", list_name, "[", i, "] ");
                }
                return join("Ship ", id_u64(ship_id), " ", list_name, "[", i, "] ");
              };

              if constexpr (std::is_same_v<T, MoveToBody>) {
                if (ord.body_id == kInvalidId) {
                  push(errors, prefix() + "MoveToBody has invalid body_id");
                } else if (!has_body(ord.body_id)) {
                  push(errors, prefix() + join("MoveToBody references missing body_id ", id_u64(ord.body_id)));
                }
              } else if constexpr (std::is_same_v<T, ColonizeBody>) {
                if (ord.body_id == kInvalidId) {
                  push(errors, prefix() + "ColonizeBody has invalid body_id");
                } else if (!has_body(ord.body_id)) {
                  push(errors, prefix() + join("ColonizeBody references missing body_id ", id_u64(ord.body_id)));
                }
              } else if constexpr (std::is_same_v<T, TravelViaJump>) {
                if (ord.jump_point_id == kInvalidId) {
                  push(errors, prefix() + "TravelViaJump has invalid jump_point_id");
                } else if (!has_jump(ord.jump_point_id)) {
                  push(errors,
                       prefix() + join("TravelViaJump references missing jump_point_id ", id_u64(ord.jump_point_id)));
                }
              } else if constexpr (std::is_same_v<T, SurveyJumpPoint>) {
                if (ord.jump_point_id == kInvalidId) {
                  push(errors, prefix() + "SurveyJumpPoint has invalid jump_point_id");
                } else if (!has_jump(ord.jump_point_id)) {
                  push(errors,
                       prefix() +
                           join("SurveyJumpPoint references missing jump_point_id ", id_u64(ord.jump_point_id)));
                }
              } else if constexpr (std::is_same_v<T, AttackShip>) {
                if (ord.target_ship_id == kInvalidId) {
                  push(errors, prefix() + "AttackShip has invalid target_ship_id");
                } else if (!has_ship(ord.target_ship_id)) {
                  push(errors,
                       prefix() + join("AttackShip references missing target_ship_id ", id_u64(ord.target_ship_id)));
                } else if (ord.target_ship_id == ship_id) {
                  push(errors, prefix() + "AttackShip targets itself");
                }

                if (ord.search_waypoint_index < 0) {
                  push(errors,
                       prefix() + join("AttackShip has negative search_waypoint_index ", ord.search_waypoint_index));
                }
                if (ord.has_search_offset) {
                  if (!std::isfinite(ord.search_offset_mkm.x) || !std::isfinite(ord.search_offset_mkm.y)) {
                    push(errors, prefix() + "AttackShip has non-finite search_offset_mkm");
                  }
                  if (ord.search_waypoint_index <= 0) {
                    push(errors, prefix() + "AttackShip has search_offset but search_waypoint_index <= 0");
                  }
                }
              } else if constexpr (std::is_same_v<T, EscortShip>) {
                if (ord.target_ship_id == kInvalidId) {
                  push(errors, prefix() + "EscortShip has invalid target_ship_id");
                } else if (!has_ship(ord.target_ship_id)) {
                  push(errors,
                       prefix() + join("EscortShip references missing target_ship_id ", id_u64(ord.target_ship_id)));
                } else if (ord.target_ship_id == ship_id) {
                  push(errors, prefix() + "EscortShip targets itself");
                }
                if (!std::isfinite(ord.follow_distance_mkm) || ord.follow_distance_mkm < 0.0) {
                  push(errors, prefix() + join("EscortShip has invalid follow_distance_mkm ", ord.follow_distance_mkm));
                }
              } else if constexpr (std::is_same_v<T, WaitDays>) {
                if (ord.days_remaining < 0) {
                  push(errors, prefix() + join("WaitDays has negative days_remaining ", ord.days_remaining));
                }
              } else if constexpr (std::is_same_v<T, LoadMineral>) {
                if (ord.colony_id == kInvalidId) {
                  push(errors, prefix() + "LoadMineral has invalid colony_id");
                } else if (!has_colony(ord.colony_id)) {
                  push(errors, prefix() + join("LoadMineral references missing colony_id ", id_u64(ord.colony_id)));
                }
              } else if constexpr (std::is_same_v<T, UnloadMineral>) {
                if (ord.colony_id == kInvalidId) {
                  push(errors, prefix() + "UnloadMineral has invalid colony_id");
                } else if (!has_colony(ord.colony_id)) {
                  push(errors, prefix() + join("UnloadMineral references missing colony_id ", id_u64(ord.colony_id)));
                }
              } else if constexpr (std::is_same_v<T, MineBody>) {
                if (ord.body_id == kInvalidId) {
                  push(errors, prefix() + "MineBody has invalid body_id");
                } else if (!has_body(ord.body_id)) {
                  push(errors, prefix() + join("MineBody references missing body_id ", id_u64(ord.body_id)));
                }
              } else if constexpr (std::is_same_v<T, LoadTroops>) {
                if (ord.colony_id == kInvalidId) {
                  push(errors, prefix() + "LoadTroops has invalid colony_id");
                } else if (!has_colony(ord.colony_id)) {
                  push(errors, prefix() + join("LoadTroops references missing colony_id ", id_u64(ord.colony_id)));
                }
              } else if constexpr (std::is_same_v<T, UnloadTroops>) {
                if (ord.colony_id == kInvalidId) {
                  push(errors, prefix() + "UnloadTroops has invalid colony_id");
                } else if (!has_colony(ord.colony_id)) {
                  push(errors, prefix() + join("UnloadTroops references missing colony_id ", id_u64(ord.colony_id)));
                }
              } else if constexpr (std::is_same_v<T, LoadColonists>) {
                if (ord.colony_id == kInvalidId) {
                  push(errors, prefix() + "LoadColonists has invalid colony_id");
                } else if (!has_colony(ord.colony_id)) {
                  push(errors, prefix() + join("LoadColonists references missing colony_id ", id_u64(ord.colony_id)));
                }
                if (!std::isfinite(ord.millions) || ord.millions < 0.0) {
                  push(errors, prefix() + join("LoadColonists has invalid millions ", ord.millions));
                }
              } else if constexpr (std::is_same_v<T, UnloadColonists>) {
                if (ord.colony_id == kInvalidId) {
                  push(errors, prefix() + "UnloadColonists has invalid colony_id");
                } else if (!has_colony(ord.colony_id)) {
                  push(errors, prefix() + join("UnloadColonists references missing colony_id ", id_u64(ord.colony_id)));
                }
                if (!std::isfinite(ord.millions) || ord.millions < 0.0) {
                  push(errors, prefix() + join("UnloadColonists has invalid millions ", ord.millions));
                }
              } else if constexpr (std::is_same_v<T, InvadeColony>) {
                if (ord.colony_id == kInvalidId) {
                  push(errors, prefix() + "InvadeColony has invalid colony_id");
                } else if (!has_colony(ord.colony_id)) {
                  push(errors, prefix() + join("InvadeColony references missing colony_id ", id_u64(ord.colony_id)));
                }
              } else if constexpr (std::is_same_v<T, BombardColony>) {
                if (ord.colony_id == kInvalidId) {
                  push(errors, prefix() + "BombardColony has invalid colony_id");
                } else if (!has_colony(ord.colony_id)) {
                  push(errors, prefix() + join("BombardColony references missing colony_id ", id_u64(ord.colony_id)));
                }
                if (ord.duration_days < -1) {
                  push(errors, prefix() + join("BombardColony has invalid duration_days ", ord.duration_days));
                }
              } else if constexpr (std::is_same_v<T, SalvageWreck>) {
                if (ord.wreck_id == kInvalidId) {
                  push(errors, prefix() + "SalvageWreck has invalid wreck_id");
                } else if (!has_wreck(ord.wreck_id)) {
                  push(errors, prefix() + join("SalvageWreck references missing wreck_id ", id_u64(ord.wreck_id)));
                }
                if (!std::isfinite(ord.tons) || ord.tons < 0.0) {
                  push(errors, prefix() + join("SalvageWreck has invalid tons ", ord.tons));
                }
              } else if constexpr (std::is_same_v<T, SalvageWreckLoop>) {
              if (ord.wreck_id == kInvalidId) {
                push(errors, prefix() + "SalvageWreckLoop has invalid wreck_id");
              } else if (!has_wreck(ord.wreck_id)) {
                push(errors, prefix() + join("SalvageWreckLoop references missing wreck_id ", id_u64(ord.wreck_id)));
              }
              if (ord.dropoff_colony_id != kInvalidId && !has_colony(ord.dropoff_colony_id)) {
                push(errors, prefix() + join("SalvageWreckLoop references missing dropoff_colony_id ",
                                             id_u64(ord.dropoff_colony_id)));
              }
              if (ord.mode != 0 && ord.mode != 1) {
                push(errors, prefix() + join("SalvageWreckLoop has invalid mode ", ord.mode));
              }
            } else if constexpr (std::is_same_v<T, InvestigateAnomaly>) {
                if (ord.anomaly_id == kInvalidId) {
                  push(errors, prefix() + "InvestigateAnomaly has invalid anomaly_id");
                } else if (!has_anomaly(ord.anomaly_id)) {
                  push(errors, prefix() + join("InvestigateAnomaly references missing anomaly_id ", id_u64(ord.anomaly_id)));
                }
                if (ord.duration_days < 0) {
                  push(errors, prefix() + join("InvestigateAnomaly has invalid duration_days ", ord.duration_days));
                }
                if (!std::isfinite(ord.progress_days) || ord.progress_days < 0.0) {
                  push(errors, prefix() + join("InvestigateAnomaly has invalid progress_days ", ord.progress_days));
                }
              } else if constexpr (std::is_same_v<T, OrbitBody>) {
                if (ord.body_id == kInvalidId) {
                  push(errors, prefix() + "OrbitBody has invalid body_id");
                } else if (!has_body(ord.body_id)) {
                  push(errors, prefix() + join("OrbitBody references missing body_id ", id_u64(ord.body_id)));
                }
                if (ord.duration_days < -1) {
                  push(errors, prefix() + join("OrbitBody has invalid duration_days ", ord.duration_days));
                }
              } else if constexpr (std::is_same_v<T, TransferCargoToShip>) {
                if (ord.target_ship_id == kInvalidId) {
                  push(errors, prefix() + "TransferCargoToShip has invalid target_ship_id");
                } else if (!has_ship(ord.target_ship_id)) {
                  push(errors,
                       prefix() + join("TransferCargoToShip references missing target_ship_id ",
                                       id_u64(ord.target_ship_id)));
                } else if (ord.target_ship_id == ship_id) {
                  push(errors, prefix() + "TransferCargoToShip targets itself");
                }
              } else if constexpr (std::is_same_v<T, TransferFuelToShip>) {
                if (ord.target_ship_id == kInvalidId) {
                  push(errors, prefix() + "TransferFuelToShip has invalid target_ship_id");
                } else if (!has_ship(ord.target_ship_id)) {
                  push(errors,
                       prefix() + join("TransferFuelToShip references missing target_ship_id ",
                                       id_u64(ord.target_ship_id)));
                } else if (ord.target_ship_id == ship_id) {
                  push(errors, prefix() + "TransferFuelToShip targets itself");
                }
              } else if constexpr (std::is_same_v<T, TransferTroopsToShip>) {
                if (ord.target_ship_id == kInvalidId) {
                  push(errors, prefix() + "TransferTroopsToShip has invalid target_ship_id");
                } else if (!has_ship(ord.target_ship_id)) {
                  push(errors,
                       prefix() + join("TransferTroopsToShip references missing target_ship_id ",
                                       id_u64(ord.target_ship_id)));
                } else if (ord.target_ship_id == ship_id) {
                  push(errors, prefix() + "TransferTroopsToShip targets itself");
                }
              } else if constexpr (std::is_same_v<T, TransferColonistsToShip>) {
                if (ord.target_ship_id == kInvalidId) {
                  push(errors, prefix() + "TransferColonistsToShip has invalid target_ship_id");
                } else if (!has_ship(ord.target_ship_id)) {
                  push(errors,
                       prefix() + join("TransferColonistsToShip references missing target_ship_id ",
                                       id_u64(ord.target_ship_id)));
                } else if (ord.target_ship_id == ship_id) {
                  push(errors, prefix() + "TransferColonistsToShip targets itself");
                }
              } else if constexpr (std::is_same_v<T, ScrapShip>) {
                if (ord.colony_id == kInvalidId) {
                  push(errors, prefix() + "ScrapShip has invalid colony_id");
                } else if (!has_colony(ord.colony_id)) {
                  push(errors, prefix() + join("ScrapShip references missing colony_id ", id_u64(ord.colony_id)));
                }
              } else if constexpr (std::is_same_v<T, MoveToPoint>) {
                // No ids.
              } else {
                // If a new Order type is added but not handled here, catch it in validation.
                push(errors, prefix() + "Unknown order type");
              }
            },
            ord_any);
      }
    };

    validate_order_list(so.queue, "order_queue");
    validate_order_list(so.repeat_template, "repeat_template");

    if (so.suspended) {
      validate_order_list(so.suspended_queue, "suspended_queue");
      validate_order_list(so.suspended_repeat_template, "suspended_repeat_template");

      if (so.suspended_repeat_count_remaining < -1) {
        push(errors,
             join("ShipOrders for ship ", id_u64(ship_id), " has invalid suspended_repeat_count_remaining ",
                  so.suspended_repeat_count_remaining));
      }
    }

    if (so.repeat_count_remaining < -1) {
      push(errors,
           join("ShipOrders for ship ", id_u64(ship_id), " has invalid repeat_count_remaining ",
                so.repeat_count_remaining));
    }
  }

  // --- Order templates ---
  // Templates are saved as raw Order lists and can be applied to any ship.
  // Validate references so corrupted/hand-edited saves can't crash order execution.
  for (const auto& [tmpl_name, list] : s.order_templates) {
    if (tmpl_name.empty()) {
      push(errors, "order_templates contains an empty template name key");
    }
    if (list.empty()) {
      push(errors, join("Order template '", tmpl_name, "' is empty"));
      continue;
    }

    for (std::size_t i = 0; i < list.size(); ++i) {
      const Order& ord_any = list[i];
      std::visit(
          [&](const auto& ord) {
            using T = std::decay_t<decltype(ord)>;

            auto prefix = [&]() {
              return join("Order template '", tmpl_name, "'[", i, "] ");
            };

            if constexpr (std::is_same_v<T, MoveToBody>) {
              if (ord.body_id == kInvalidId) {
                push(errors, prefix() + "MoveToBody has invalid body_id");
              } else if (!has_body(ord.body_id)) {
                push(errors, prefix() + join("MoveToBody references missing body_id ", id_u64(ord.body_id)));
              }
            } else if constexpr (std::is_same_v<T, ColonizeBody>) {
              if (ord.body_id == kInvalidId) {
                push(errors, prefix() + "ColonizeBody has invalid body_id");
              } else if (!has_body(ord.body_id)) {
                push(errors, prefix() + join("ColonizeBody references missing body_id ", id_u64(ord.body_id)));
              }
            } else if constexpr (std::is_same_v<T, TravelViaJump>) {
              if (ord.jump_point_id == kInvalidId) {
                push(errors, prefix() + "TravelViaJump has invalid jump_point_id");
              } else if (!has_jump(ord.jump_point_id)) {
                push(errors,
                     prefix() + join("TravelViaJump references missing jump_point_id ", id_u64(ord.jump_point_id)));
              }
            } else if constexpr (std::is_same_v<T, SurveyJumpPoint>) {
              if (ord.jump_point_id == kInvalidId) {
                push(errors, prefix() + "SurveyJumpPoint has invalid jump_point_id");
              } else if (!has_jump(ord.jump_point_id)) {
                push(errors,
                     prefix() +
                         join("SurveyJumpPoint references missing jump_point_id ", id_u64(ord.jump_point_id)));
              }
            } else if constexpr (std::is_same_v<T, AttackShip>) {
              if (ord.target_ship_id == kInvalidId) {
                push(errors, prefix() + "AttackShip has invalid target_ship_id");
              } else if (!has_ship(ord.target_ship_id)) {
                push(errors,
                     prefix() + join("AttackShip references missing target_ship_id ", id_u64(ord.target_ship_id)));
              }

              if (ord.search_waypoint_index < 0) {
                push(errors,
                     prefix() + join("AttackShip has negative search_waypoint_index ", ord.search_waypoint_index));
              }
              if (ord.has_search_offset) {
                if (!std::isfinite(ord.search_offset_mkm.x) || !std::isfinite(ord.search_offset_mkm.y)) {
                  push(errors, prefix() + "AttackShip has non-finite search_offset_mkm");
                }
                if (ord.search_waypoint_index <= 0) {
                  push(errors, prefix() + "AttackShip has search_offset but search_waypoint_index <= 0");
                }
              }
            } else if constexpr (std::is_same_v<T, EscortShip>) {
              if (ord.target_ship_id == kInvalidId) {
                push(errors, prefix() + "EscortShip has invalid target_ship_id");
              } else if (!has_ship(ord.target_ship_id)) {
                push(errors,
                     prefix() + join("EscortShip references missing target_ship_id ", id_u64(ord.target_ship_id)));
              }
              if (!std::isfinite(ord.follow_distance_mkm) || ord.follow_distance_mkm < 0.0) {
                push(errors, prefix() + join("EscortShip has invalid follow_distance_mkm ", ord.follow_distance_mkm));
              }
            } else if constexpr (std::is_same_v<T, WaitDays>) {
              if (ord.days_remaining < 0) {
                push(errors, prefix() + join("WaitDays has negative days_remaining ", ord.days_remaining));
              }
            } else if constexpr (std::is_same_v<T, LoadMineral>) {
              if (ord.colony_id == kInvalidId) {
                push(errors, prefix() + "LoadMineral has invalid colony_id");
              } else if (!has_colony(ord.colony_id)) {
                push(errors, prefix() + join("LoadMineral references missing colony_id ", id_u64(ord.colony_id)));
              }
            } else if constexpr (std::is_same_v<T, UnloadMineral>) {
              if (ord.colony_id == kInvalidId) {
                push(errors, prefix() + "UnloadMineral has invalid colony_id");
              } else if (!has_colony(ord.colony_id)) {
                push(errors, prefix() + join("UnloadMineral references missing colony_id ", id_u64(ord.colony_id)));
              }
            } else if constexpr (std::is_same_v<T, MineBody>) {
              if (ord.body_id == kInvalidId) {
                push(errors, prefix() + "MineBody has invalid body_id");
              } else if (!has_body(ord.body_id)) {
                push(errors, prefix() + join("MineBody references missing body_id ", id_u64(ord.body_id)));
              }
            } else if constexpr (std::is_same_v<T, LoadTroops>) {
              if (ord.colony_id == kInvalidId) {
                push(errors, prefix() + "LoadTroops has invalid colony_id");
              } else if (!has_colony(ord.colony_id)) {
                push(errors, prefix() + join("LoadTroops references missing colony_id ", id_u64(ord.colony_id)));
              }
            } else if constexpr (std::is_same_v<T, UnloadTroops>) {
              if (ord.colony_id == kInvalidId) {
                push(errors, prefix() + "UnloadTroops has invalid colony_id");
              } else if (!has_colony(ord.colony_id)) {
                push(errors, prefix() + join("UnloadTroops references missing colony_id ", id_u64(ord.colony_id)));
              }
            } else if constexpr (std::is_same_v<T, LoadColonists>) {
              if (ord.colony_id == kInvalidId) {
                push(errors, prefix() + "LoadColonists has invalid colony_id");
              } else if (!has_colony(ord.colony_id)) {
                push(errors, prefix() + join("LoadColonists references missing colony_id ", id_u64(ord.colony_id)));
              }
              if (!std::isfinite(ord.millions) || ord.millions < 0.0) {
                push(errors, prefix() + join("LoadColonists has invalid millions ", ord.millions));
              }
            } else if constexpr (std::is_same_v<T, UnloadColonists>) {
              if (ord.colony_id == kInvalidId) {
                push(errors, prefix() + "UnloadColonists has invalid colony_id");
              } else if (!has_colony(ord.colony_id)) {
                push(errors, prefix() + join("UnloadColonists references missing colony_id ", id_u64(ord.colony_id)));
              }
              if (!std::isfinite(ord.millions) || ord.millions < 0.0) {
                push(errors, prefix() + join("UnloadColonists has invalid millions ", ord.millions));
              }
            } else if constexpr (std::is_same_v<T, InvadeColony>) {
              if (ord.colony_id == kInvalidId) {
                push(errors, prefix() + "InvadeColony has invalid colony_id");
              } else if (!has_colony(ord.colony_id)) {
                push(errors, prefix() + join("InvadeColony references missing colony_id ", id_u64(ord.colony_id)));
              }
            } else if constexpr (std::is_same_v<T, BombardColony>) {
              if (ord.colony_id == kInvalidId) {
                push(errors, prefix() + "BombardColony has invalid colony_id");
              } else if (!has_colony(ord.colony_id)) {
                push(errors, prefix() + join("BombardColony references missing colony_id ", id_u64(ord.colony_id)));
              }
              if (ord.duration_days < -1) {
                push(errors, prefix() + join("BombardColony has invalid duration_days ", ord.duration_days));
              }
            } else if constexpr (std::is_same_v<T, SalvageWreck>) {
              if (ord.wreck_id == kInvalidId) {
                push(errors, prefix() + "SalvageWreck has invalid wreck_id");
              } else if (!has_wreck(ord.wreck_id)) {
                push(errors, prefix() + join("SalvageWreck references missing wreck_id ", id_u64(ord.wreck_id)));
              }
              if (!std::isfinite(ord.tons) || ord.tons < 0.0) {
                push(errors, prefix() + join("SalvageWreck has invalid tons ", ord.tons));
              }
            } else if constexpr (std::is_same_v<T, SalvageWreckLoop>) {
              if (ord.wreck_id == kInvalidId) {
                push(errors, prefix() + "SalvageWreckLoop has invalid wreck_id");
              } else if (!has_wreck(ord.wreck_id)) {
                push(errors, prefix() + join("SalvageWreckLoop references missing wreck_id ", id_u64(ord.wreck_id)));
              }
              if (ord.dropoff_colony_id != kInvalidId && !has_colony(ord.dropoff_colony_id)) {
                push(errors, prefix() + join("SalvageWreckLoop references missing dropoff_colony_id ",
                                             id_u64(ord.dropoff_colony_id)));
              }
              if (ord.mode != 0 && ord.mode != 1) {
                push(errors, prefix() + join("SalvageWreckLoop has invalid mode ", ord.mode));
              }
            } else if constexpr (std::is_same_v<T, InvestigateAnomaly>) {
              if (ord.anomaly_id == kInvalidId) {
                push(errors, prefix() + "InvestigateAnomaly has invalid anomaly_id");
              } else if (!has_anomaly(ord.anomaly_id)) {
                push(errors, prefix() + join("InvestigateAnomaly references missing anomaly_id ", id_u64(ord.anomaly_id)));
              }
              if (ord.duration_days < 0) {
                push(errors, prefix() + join("InvestigateAnomaly has invalid duration_days ", ord.duration_days));
              }
              if (!std::isfinite(ord.progress_days) || ord.progress_days < 0.0) {
                push(errors, prefix() + join("InvestigateAnomaly has invalid progress_days ", ord.progress_days));
              }
            } else if constexpr (std::is_same_v<T, OrbitBody>) {
              if (ord.body_id == kInvalidId) {
                push(errors, prefix() + "OrbitBody has invalid body_id");
              } else if (!has_body(ord.body_id)) {
                push(errors, prefix() + join("OrbitBody references missing body_id ", id_u64(ord.body_id)));
              }
              if (ord.duration_days < -1) {
                push(errors, prefix() + join("OrbitBody has invalid duration_days ", ord.duration_days));
              }
            } else if constexpr (std::is_same_v<T, TransferCargoToShip>) {
              if (ord.target_ship_id == kInvalidId) {
                push(errors, prefix() + "TransferCargoToShip has invalid target_ship_id");
              } else if (!has_ship(ord.target_ship_id)) {
                push(errors,
                     prefix() + join("TransferCargoToShip references missing target_ship_id ",
                                     id_u64(ord.target_ship_id)));
              }
            } else if constexpr (std::is_same_v<T, TransferFuelToShip>) {
              if (ord.target_ship_id == kInvalidId) {
                push(errors, prefix() + "TransferFuelToShip has invalid target_ship_id");
              } else if (!has_ship(ord.target_ship_id)) {
                push(errors,
                     prefix() + join("TransferFuelToShip references missing target_ship_id ",
                                     id_u64(ord.target_ship_id)));
              }
            } else if constexpr (std::is_same_v<T, TransferTroopsToShip>) {
              if (ord.target_ship_id == kInvalidId) {
                push(errors, prefix() + "TransferTroopsToShip has invalid target_ship_id");
              } else if (!has_ship(ord.target_ship_id)) {
                push(errors,
                     prefix() + join("TransferTroopsToShip references missing target_ship_id ",
                                     id_u64(ord.target_ship_id)));
              }
            } else if constexpr (std::is_same_v<T, TransferColonistsToShip>) {
              if (ord.target_ship_id == kInvalidId) {
                push(errors, prefix() + "TransferColonistsToShip has invalid target_ship_id");
              } else if (!has_ship(ord.target_ship_id)) {
                push(errors,
                     prefix() + join("TransferColonistsToShip references missing target_ship_id ",
                                     id_u64(ord.target_ship_id)));
              }
            } else if constexpr (std::is_same_v<T, ScrapShip>) {
              if (ord.colony_id == kInvalidId) {
                push(errors, prefix() + "ScrapShip has invalid colony_id");
              } else if (!has_colony(ord.colony_id)) {
                push(errors, prefix() + join("ScrapShip references missing colony_id ", id_u64(ord.colony_id)));
              }
            } else if constexpr (std::is_same_v<T, MoveToPoint>) {
              // No ids.
            } else {
              // If a new Order type is added but not handled here, catch it in validation.
              push(errors, prefix() + "Unknown order type");
            }
          },
          ord_any);
    }
  }

  // --- Colonies ---
  std::unordered_set<Id> colony_body_ids;
  for (const auto& [cid, c] : s.colonies) {
    if (c.body_id != kInvalidId) {
      if (!colony_body_ids.insert(c.body_id).second) {
        push(errors,
             join("Multiple colonies share the same body_id ", id_u64(c.body_id), " (example: colony ", id_u64(cid),
                  " '", c.name, "')"));
      }
    }
    if (c.body_id == kInvalidId || !has_body(c.body_id)) {
      push(errors,
           join("Colony ", id_u64(cid), " ('", c.name, "') references missing body_id ", id_u64(c.body_id)));
    }
    if (c.faction_id == kInvalidId || !has_faction(c.faction_id)) {
      push(errors,
           join("Colony ", id_u64(cid), " ('", c.name, "') references unknown faction_id ", id_u64(c.faction_id)));
    }

    if (content) {
      for (const auto& [inst_id, count] : c.installations) {
        if (inst_id.empty()) {
          push(errors, join("Colony ", id_u64(cid), " ('", c.name, "') has an installation entry with empty id"));
          continue;
        }
        if (count < 0) {
          push(errors,
               join("Colony ", id_u64(cid), " ('", c.name, "') has negative installation count for '", inst_id, "': ",
                    count));
        }
        if (!installation_exists(*content, inst_id)) {
          push(errors,
               join("Colony ", id_u64(cid), " ('", c.name, "') references unknown installation id '", inst_id, "'"));
        }
      }

      for (std::size_t i = 0; i < c.shipyard_queue.size(); ++i) {
        const auto& bo = c.shipyard_queue[i];
        if (bo.design_id.empty()) {
          push(errors, join("Colony ", id_u64(cid), " shipyard_queue[", i, "] has empty design_id"));
        } else if (!design_exists(s, *content, bo.design_id)) {
          push(errors,
               join("Colony ", id_u64(cid), " shipyard_queue[", i, "] references unknown design_id '", bo.design_id,
                    "'"));
        }

        // Optional build/refit metadata references.
        if (!bo.apply_ship_profile_name.empty()) {
          const auto itf = s.factions.find(c.faction_id);
          if (itf == s.factions.end()) {
            push(errors,
                 join("Colony ", id_u64(cid), " shipyard_queue[", i,
                      "] has apply_ship_profile_name but colony faction is missing"));
          } else if (itf->second.ship_profiles.find(bo.apply_ship_profile_name) == itf->second.ship_profiles.end()) {
            push(errors,
                 join("Colony ", id_u64(cid), " shipyard_queue[", i,
                      "] references unknown ship profile '", bo.apply_ship_profile_name, "'"));
          }
        }

        if (bo.assign_to_fleet_id != kInvalidId) {
          const auto itfl = s.fleets.find(bo.assign_to_fleet_id);
          if (itfl == s.fleets.end()) {
            push(errors,
                 join("Colony ", id_u64(cid), " shipyard_queue[", i,
                      "] references missing assign_to_fleet_id ", id_u64(bo.assign_to_fleet_id)));
          } else if (itfl->second.faction_id != kInvalidId && itfl->second.faction_id != c.faction_id) {
            push(errors,
                 join("Colony ", id_u64(cid), " shipyard_queue[", i,
                      "] assign_to_fleet_id ", id_u64(bo.assign_to_fleet_id),
                      " faction mismatch: fleet faction=", id_u64(itfl->second.faction_id),
                      " colony faction=", id_u64(c.faction_id)));
          }
        }

        if (bo.rally_to_colony_id != kInvalidId) {
          const auto itc2 = s.colonies.find(bo.rally_to_colony_id);
          if (itc2 == s.colonies.end()) {
            push(errors,
                 join("Colony ", id_u64(cid), " shipyard_queue[", i,
                      "] references missing rally_to_colony_id ", id_u64(bo.rally_to_colony_id)));
          } else if (itc2->second.faction_id != kInvalidId && itc2->second.faction_id != c.faction_id) {
            push(errors,
                 join("Colony ", id_u64(cid), " shipyard_queue[", i,
                      "] rally_to_colony_id ", id_u64(bo.rally_to_colony_id),
                      " faction mismatch: target colony faction=", id_u64(itc2->second.faction_id),
                      " source colony faction=", id_u64(c.faction_id)));
          }
        }
      }

      for (std::size_t i = 0; i < c.construction_queue.size(); ++i) {
        const auto& io = c.construction_queue[i];
        if (io.installation_id.empty()) {
          push(errors, join("Colony ", id_u64(cid), " construction_queue[", i, "] has empty installation_id"));
        } else if (!installation_exists(*content, io.installation_id)) {
          push(errors,
               join("Colony ", id_u64(cid), " construction_queue[", i, "] references unknown installation_id '",
                    io.installation_id, "'"));
        }
        if (io.quantity_remaining < 0) {
          push(errors,
               join("Colony ", id_u64(cid), " construction_queue[", i, "] has negative quantity_remaining: ",
                    io.quantity_remaining));
        }
        if (io.cp_remaining < 0.0) {
          push(errors,
               join("Colony ", id_u64(cid), " construction_queue[", i, "] has negative cp_remaining: ", io.cp_remaining));
        }
      }
    }

    // Validate mineral stockpiles.
    for (const auto& [mineral, tons] : c.minerals) {
      if (mineral.empty()) {
        push(errors, join("Colony ", id_u64(cid), " ('", c.name, "') has an empty mineral key in minerals"));
        continue;
      }
      if (!std::isfinite(tons)) {
        push(errors,
             join("Colony ", id_u64(cid), " ('", c.name, "') has non-finite mineral stockpile for '", mineral,
                  "': ", tons));
        continue;
      }
      if (tons < -1e-6) {
        push(errors,
             join("Colony ", id_u64(cid), " ('", c.name, "') has negative mineral stockpile for '", mineral,
                  "': ", tons));
      }
    }

    // Validate manual mineral reserves (auto-freight).
    for (const auto& [mineral, tons] : c.mineral_reserves) {
      if (mineral.empty()) {
        push(errors, join("Colony ", id_u64(cid), " ('", c.name, "') has an empty mineral key in mineral_reserves"));
        continue;
      }
      if (!std::isfinite(tons)) {
        push(errors,
             join("Colony ", id_u64(cid), " ('", c.name, "') has non-finite reserve for '", mineral, "': ", tons));
        continue;
      }
      if (tons < -1e-6) {
        push(errors, join("Colony ", id_u64(cid), " ('", c.name, "') has negative reserve for '", mineral, "': ", tons));
      }
    }

    // Validate mineral stockpile targets (auto-freight import).
    for (const auto& [mineral, tons] : c.mineral_targets) {
      if (mineral.empty()) {
        push(errors, join("Colony ", id_u64(cid), " ('", c.name, "') has an empty mineral key in mineral_targets"));
        continue;
      }
      if (!std::isfinite(tons)) {
        push(errors, join("Colony ", id_u64(cid), " ('", c.name, "') has non-finite target for '", mineral, "': ", tons));
        continue;
      }
      if (tons < -1e-6) {
        push(errors, join("Colony ", id_u64(cid), " ('", c.name, "') has negative target for '", mineral, "': ", tons));
      }
    }

    // Validate installation targets (auto-build).
    for (const auto& [inst_id, target] : c.installation_targets) {
      if (inst_id.empty()) {
        push(errors, join("Colony ", id_u64(cid), " ('", c.name, "') has an empty installation key in installation_targets"));
        continue;
      }
      if (target < 0) {
        push(errors, join("Colony ", id_u64(cid), " ('", c.name, "') has negative installation target for '", inst_id,
                          "': ", target));
      }
      if (content && !installation_exists(*content, inst_id)) {
        push(errors, join("Colony ", id_u64(cid), " ('", c.name, "') installation_targets references unknown installation_id '",
                          inst_id, "'"));
      }
    }

    // Validate ground combat / training values.
    if (!std::isfinite(c.ground_forces)) {
      push(errors, join("Colony ", id_u64(cid), " ('", c.name, "') has non-finite ground_forces: ", c.ground_forces));
    } else if (c.ground_forces < -1e-6) {
      push(errors, join("Colony ", id_u64(cid), " ('", c.name, "') has negative ground_forces: ", c.ground_forces));
    }

    if (!std::isfinite(c.troop_training_queue)) {
      push(errors, join("Colony ", id_u64(cid), " ('", c.name, "') has non-finite troop_training_queue: ",
                        c.troop_training_queue));
    } else if (c.troop_training_queue < -1e-6) {
      push(errors, join("Colony ", id_u64(cid), " ('", c.name, "') has negative troop_training_queue: ",
                        c.troop_training_queue));
    }

    if (!std::isfinite(c.troop_training_auto_queued)) {
      push(errors, join("Colony ", id_u64(cid), " ('", c.name, "') has non-finite troop_training_auto_queued: ",
                        c.troop_training_auto_queued));
    } else if (c.troop_training_auto_queued < -1e-6) {
      push(errors, join("Colony ", id_u64(cid), " ('", c.name, "') has negative troop_training_auto_queued: ",
                        c.troop_training_auto_queued));
    } else if (c.troop_training_auto_queued > c.troop_training_queue + 1e-6) {
      push(errors, join("Colony ", id_u64(cid), " ('", c.name, "') has troop_training_auto_queued > troop_training_queue (",
                        c.troop_training_auto_queued, " > ", c.troop_training_queue, ")"));
    }

    if (!std::isfinite(c.garrison_target_strength)) {
      push(errors, join("Colony ", id_u64(cid), " ('", c.name, "') has non-finite garrison_target_strength: ",
                        c.garrison_target_strength));
    } else if (c.garrison_target_strength < -1e-6) {
      push(errors, join("Colony ", id_u64(cid), " ('", c.name, "') has negative garrison_target_strength: ",
                        c.garrison_target_strength));
    }


  }

  // --- Factions ---
  for (const auto& [fid, f] : s.factions) {
    // discovered systems: exist + no duplicates.
    {
      std::unordered_set<Id> seen;
      seen.reserve(f.discovered_systems.size() * 2);
      for (const auto sid : f.discovered_systems) {
        if (sid == kInvalidId) {
          push(errors, join("Faction ", id_u64(fid), " ('", f.name, "') has kInvalidId in discovered_systems"));
          continue;
        }
        if (!has_system(sid)) {
          push(errors,
               join("Faction ", id_u64(fid), " ('", f.name, "') discovered_systems references missing system ",
                    id_u64(sid)));
          continue;
        }
        if (!seen.insert(sid).second) {
          push(errors,
               join("Faction ", id_u64(fid), " ('", f.name, "') discovered_systems contains duplicate system ",
                    id_u64(sid)));
        }
      }
    }

    // surveyed jump points: exist + no duplicates.
    {
      std::unordered_set<Id> seen;
      seen.reserve(f.surveyed_jump_points.size() * 2);
      for (const auto jid : f.surveyed_jump_points) {
        if (jid == kInvalidId) {
          push(errors, join("Faction ", id_u64(fid), " ('", f.name, "') has kInvalidId in surveyed_jump_points"));
          continue;
        }
        if (!has_jump(jid)) {
          push(errors,
               join("Faction ", id_u64(fid), " ('", f.name, "') surveyed_jump_points references missing jump_point ",
                    id_u64(jid)));
          continue;
        }
        if (!seen.insert(jid).second) {
          push(errors,
               join("Faction ", id_u64(fid), " ('", f.name, "') surveyed_jump_points contains duplicate jump_point ",
                    id_u64(jid)));
        }
      }
    }

    // contact map is keyed by ship id; ensure internal field matches.
    for (const auto& [sid, c] : f.ship_contacts) {
      if (c.ship_id != kInvalidId && c.ship_id != sid) {
        push(errors,
             join("Faction ", id_u64(fid), " ('", f.name, "') ship_contacts key ", id_u64(sid),
                  " does not match contact.ship_id ", id_u64(c.ship_id)));
      }
      if (c.system_id == kInvalidId || !has_system(c.system_id)) {
        push(errors,
             join("Faction ", id_u64(fid), " ('", f.name, "') contact for ship ", id_u64(sid),
                  " references unknown system_id ", id_u64(c.system_id)));
      }

      if (!std::isfinite(c.last_seen_position_uncertainty_mkm) || c.last_seen_position_uncertainty_mkm < 0.0) {
        push(errors,
             join("Faction ", id_u64(fid), " ('", f.name, "') contact for ship ", id_u64(sid),
                  " has invalid last_seen_position_uncertainty_mkm ", c.last_seen_position_uncertainty_mkm));
      }
    }

    if (content) {
      if (!f.active_research_id.empty() && !tech_exists(*content, f.active_research_id)) {
        push(errors,
             join("Faction ", id_u64(fid), " ('", f.name, "') active_research_id is unknown: '", f.active_research_id,
                  "'"));
      }
      for (const auto& t : f.research_queue) {
        if (!tech_exists(*content, t)) {
          push(errors, join("Faction ", id_u64(fid), " ('", f.name, "') research_queue contains unknown tech '", t, "'"));
        }
      }
      for (const auto& t : f.known_techs) {
        if (!tech_exists(*content, t)) {
          push(errors, join("Faction ", id_u64(fid), " ('", f.name, "') known_techs contains unknown tech '", t, "'"));
        }
      }
      for (const auto& c_id : f.unlocked_components) {
        if (!component_exists(*content, c_id)) {
          push(errors,
               join("Faction ", id_u64(fid), " ('", f.name, "') unlocked_components contains unknown component '", c_id,
                    "'"));
        }
      }
      for (const auto& i_id : f.unlocked_installations) {
        if (!installation_exists(*content, i_id)) {
          push(errors,
               join("Faction ", id_u64(fid), " ('", f.name, "') unlocked_installations contains unknown installation '",
                    i_id, "'"));
        }
      }
    }

    // Ship design targets (auto-shipyards).
    for (const auto& [design_id, target] : f.ship_design_targets) {
      if (design_id.empty()) {
        push(errors, join("Faction ", id_u64(fid), " ('", f.name, "') has an empty key in ship_design_targets"));
        continue;
      }
      if (target < 0) {
        push(errors, join("Faction ", id_u64(fid), " ('", f.name, "') has negative ship design target for '", design_id, "': ",
                          target));
      }
      if (content && !design_exists(s, *content, design_id)) {
        push(errors, join("Faction ", id_u64(fid), " ('", f.name, "') ship_design_targets references unknown design_id '",
                          design_id, "'"));
      }
    }

    // Colony founding defaults (optional).
    {
      const ColonyAutomationProfile& p = f.colony_founding_profile;
      if (!std::isfinite(p.garrison_target_strength) || p.garrison_target_strength < 0.0) {
        push(errors,
             join("Faction ", id_u64(fid), " ('", f.name,
                  "') colony_founding_profile has invalid garrison_target_strength: ", p.garrison_target_strength));
      }
      for (const auto& [k, v] : p.installation_targets) {
        if (k.empty()) {
          push(errors,
               join("Faction ", id_u64(fid), " ('", f.name, "') colony_founding_profile has empty installation_targets key"));
          continue;
        }
        if (v < 0) {
          push(errors,
               join("Faction ", id_u64(fid), " ('", f.name, "') colony_founding_profile has negative installation target for '",
                    k, "': ", v));
        }
      }
      for (const auto& [k, v] : p.mineral_reserves) {
        if (k.empty()) {
          push(errors,
               join("Faction ", id_u64(fid), " ('", f.name, "') colony_founding_profile has empty mineral_reserves key"));
          continue;
        }
        if (!std::isfinite(v) || v < 0.0) {
          push(errors,
               join("Faction ", id_u64(fid), " ('", f.name,
                    "') colony_founding_profile has invalid mineral_reserve for '", k, "': ", v));
        }
      }
      for (const auto& [k, v] : p.mineral_targets) {
        if (k.empty()) {
          push(errors,
               join("Faction ", id_u64(fid), " ('", f.name, "') colony_founding_profile has empty mineral_targets key"));
          continue;
        }
        if (!std::isfinite(v) || v < 0.0) {
          push(errors,
               join("Faction ", id_u64(fid), " ('", f.name,
                    "') colony_founding_profile has invalid mineral_target for '", k, "': ", v));
        }
      }
    }
  }

  // --- Fleets ---
  // Validate basic invariants:
  // - fleet faction exists
  // - ship_ids exist, belong to faction, and do not appear in multiple fleets
  // - leader_ship_id (if set) is a member ship
  {
    std::unordered_map<Id, Id> ship_to_fleet;
    ship_to_fleet.reserve(s.ships.size() * 2);

    for (const auto& [fleet_id, fl] : s.fleets) {
      if (fl.faction_id == kInvalidId || !has_faction(fl.faction_id)) {
        push(errors,
             join("Fleet ", id_u64(fleet_id), " ('", fl.name, "') references unknown faction_id ", id_u64(fl.faction_id)));
      }

      if (fl.ship_ids.empty()) {
        push(errors, join("Fleet ", id_u64(fleet_id), " ('", fl.name, "') has empty ship_ids"));
        continue;
      }

      std::unordered_set<Id> seen;
      seen.reserve(fl.ship_ids.size() * 2);
      for (const auto sid : fl.ship_ids) {
        if (sid == kInvalidId) {
          push(errors, join("Fleet ", id_u64(fleet_id), " ('", fl.name, "') contains kInvalidId in ship_ids"));
          continue;
        }
        if (!has_ship(sid)) {
          push(errors, join("Fleet ", id_u64(fleet_id), " ('", fl.name, "') references missing ship_id ", id_u64(sid)));
          continue;
        }
        const auto& sh = s.ships.at(sid);
        if (fl.faction_id != kInvalidId && sh.faction_id != fl.faction_id) {
          push(errors,
               join("Fleet ", id_u64(fleet_id), " ('", fl.name, "') contains ship ", id_u64(sid), " ('", sh.name,
                    "') with faction_id=", id_u64(sh.faction_id), " != fleet faction_id=", id_u64(fl.faction_id)));
        }
        if (!seen.insert(sid).second) {
          push(errors,
               join("Fleet ", id_u64(fleet_id), " ('", fl.name, "') ship_ids contains duplicate ship ", id_u64(sid)));
        }

        auto [it, inserted] = ship_to_fleet.insert({sid, fleet_id});
        if (!inserted && it->second != fleet_id) {
          push(errors,
               join("Ship ", id_u64(sid), " appears in multiple fleets: ", id_u64(it->second), " and ",
                    id_u64(fleet_id)));
        }
      }

      if (fl.leader_ship_id != kInvalidId && !seen.count(fl.leader_ship_id)) {
        push(errors,
             join("Fleet ", id_u64(fleet_id), " ('", fl.name, "') leader_ship_id ", id_u64(fl.leader_ship_id),
                  " is not present in ship_ids"));
      }
    }
  }

  // --- Events ---
  {
    std::uint64_t max_seq = 0;
    std::unordered_set<std::uint64_t> seen;
    seen.reserve(s.events.size() * 2);

    for (std::size_t i = 0; i < s.events.size(); ++i) {
      const auto& e = s.events[i];
      if (e.seq == 0) {
        push(errors, join("events[", i, "] has seq=0"));
      }
      if (e.seq > max_seq) max_seq = e.seq;
      if (e.seq != 0 && !seen.insert(e.seq).second) {
        push(errors, join("events[", i, "] has duplicate seq=", e.seq));
      }

      if (e.faction_id != kInvalidId && !has_faction(e.faction_id)) {
        push(errors, join("events[", i, "] references missing faction_id ", id_u64(e.faction_id)));
      }
      if (e.faction_id2 != kInvalidId && !has_faction(e.faction_id2)) {
        push(errors, join("events[", i, "] references missing faction_id2 ", id_u64(e.faction_id2)));
      }
      if (e.system_id != kInvalidId && !has_system(e.system_id)) {
        push(errors, join("events[", i, "] references missing system_id ", id_u64(e.system_id)));
      }
      if (e.ship_id != kInvalidId && !has_ship(e.ship_id)) {
        push(errors, join("events[", i, "] references missing ship_id ", id_u64(e.ship_id)));
      }
      if (e.colony_id != kInvalidId && !has_colony(e.colony_id)) {
        push(errors, join("events[", i, "] references missing colony_id ", id_u64(e.colony_id)));
      }
    }

    if (s.next_event_seq != 0 && max_seq != 0 && s.next_event_seq <= max_seq) {
      push(errors,
           join("next_event_seq is not monotonic: next_event_seq=", s.next_event_seq, " max_event_seq=", max_seq));
    }
  }

  std::sort(errors.begin(), errors.end());
  return errors;
}

FixReport fix_game_state(GameState& s, const ContentDB* content) {
  FixReport report;

  auto note = [&](std::string msg) {
    report.changes += 1;
    report.actions.push_back(std::move(msg));
  };

  auto has_system = [&](Id id) { return s.systems.find(id) != s.systems.end(); };
  auto has_body = [&](Id id) { return s.bodies.find(id) != s.bodies.end(); };
  auto has_jump = [&](Id id) { return s.jump_points.find(id) != s.jump_points.end(); };
  auto has_ship = [&](Id id) { return s.ships.find(id) != s.ships.end(); };
  auto has_wreck = [&](Id id) { return s.wrecks.find(id) != s.wrecks.end(); };
  auto has_anomaly = [&](Id id) { return s.anomalies.find(id) != s.anomalies.end(); };
  auto has_colony = [&](Id id) { return s.colonies.find(id) != s.colonies.end(); };
  auto has_faction = [&](Id id) { return s.factions.find(id) != s.factions.end(); };

  // --- Fix map key/id mismatches ---
  for (auto& [id, sys] : s.systems) {
    if (sys.id != id) {
      note(join("Fix: StarSystem id mismatch: key=", id_u64(id), " value.id=", id_u64(sys.id)));
      sys.id = id;
    }
  }
  for (auto& [id, b] : s.bodies) {
    if (b.id != id) {
      note(join("Fix: Body id mismatch: key=", id_u64(id), " value.id=", id_u64(b.id)));
      b.id = id;
    }
  }
  for (auto& [id, jp] : s.jump_points) {
    if (jp.id != id) {
      note(join("Fix: JumpPoint id mismatch: key=", id_u64(id), " value.id=", id_u64(jp.id)));
      jp.id = id;
    }
  }
  for (auto& [id, sh] : s.ships) {
    if (sh.id != id) {
      note(join("Fix: Ship id mismatch: key=", id_u64(id), " value.id=", id_u64(sh.id)));
      sh.id = id;
    }
  }
  for (auto& [id, w] : s.wrecks) {
    if (w.id != id) {
      note(join("Fix: Wreck id mismatch: key=", id_u64(id), " value.id=", id_u64(w.id)));
      w.id = id;
    }
  }
  for (auto& [id, c] : s.colonies) {
    if (c.id != id) {
      note(join("Fix: Colony id mismatch: key=", id_u64(id), " value.id=", id_u64(c.id)));
      c.id = id;
    }
  }
  for (auto& [id, f] : s.factions) {
    if (f.id != id) {
      note(join("Fix: Faction id mismatch: key=", id_u64(id), " value.id=", id_u64(f.id)));
      f.id = id;
    }
  }
  for (auto& [id, fl] : s.fleets) {
    if (fl.id != id) {
      note(join("Fix: Fleet id mismatch: key=", id_u64(id), " value.id=", id_u64(fl.id)));
      fl.id = id;
    }
  }
  for (auto& [id, c] : s.contracts) {
    if (c.id != id) {
      note(join("Fix: Contract id mismatch: key=", id_u64(id), " value.id=", id_u64(c.id)));
      c.id = id;
    }
  }

  // --- Choose deterministic fallbacks ---
  const Id first_system = (!s.systems.empty()) ? sorted_keys(s.systems).front() : kInvalidId;
  if (s.selected_system == kInvalidId || !has_system(s.selected_system)) {
    if (first_system != kInvalidId) {
      note(join("Fix: selected_system was invalid (", id_u64(s.selected_system), "); set to ", id_u64(first_system)));
      s.selected_system = first_system;
    }
  }
  const Id fallback_system = (has_system(s.selected_system)) ? s.selected_system : first_system;

  const Id first_faction = (!s.factions.empty()) ? sorted_keys(s.factions).front() : kInvalidId;

  auto pick_fallback_design = [&]() -> std::string {
    if (!content) return {};

    // Prefer a well-known "starter" id if present, otherwise choose the first
    // lexicographic design id from content.
    static const char* kPreferred[] = {
        "starting_freighter",
        "starting_surveyor",
        "starting_fighter",
        "starter_freighter",
        "starter_surveyor",
        "starter_fighter",
    };
    for (const char* id : kPreferred) {
      if (content->designs.find(id) != content->designs.end()) return std::string(id);
    }

    if (!content->designs.empty()) {
      auto ids = sorted_keys(content->designs);
      return ids.front();
    }

    return {};
  };
  const std::string fallback_design = pick_fallback_design();

  // --- Normalize per-entity system/faction references + basic numeric sanity ---
  for (auto& [bid, b] : s.bodies) {
    if (b.system_id == kInvalidId || !has_system(b.system_id)) {
      if (fallback_system != kInvalidId) {
        note(join("Fix: Body ", id_u64(bid), " had unknown system_id ", id_u64(b.system_id), "; set to ",
                  id_u64(fallback_system)));
        b.system_id = fallback_system;
      }
    }

    // Parent relationships must point to an existing body in the same system.
    if (b.parent_body_id != kInvalidId) {
      if (b.parent_body_id == bid || !has_body(b.parent_body_id) || s.bodies.at(b.parent_body_id).system_id != b.system_id) {
        note(join("Fix: Body ", id_u64(bid), " had invalid parent_body_id ", id_u64(b.parent_body_id), "; cleared"));
        b.parent_body_id = kInvalidId;
      }
    }

    for (auto it = b.mineral_deposits.begin(); it != b.mineral_deposits.end();) {
      if (it->first.empty()) {
        note(join("Fix: Body ", id_u64(bid), " had empty mineral_deposits key; removed"));
        it = b.mineral_deposits.erase(it);
        continue;
      }
      if (!std::isfinite(it->second) || it->second < 0.0) {
        const double old = it->second;
        it->second = std::max(0.0, std::isfinite(it->second) ? it->second : 0.0);
        note(join("Fix: Body ", id_u64(bid), " mineral_deposits['", it->first, "'] clamped ", old, " -> ", it->second));
      }
      ++it;
    }
  }

  for (auto& [jid, jp] : s.jump_points) {
    if (jp.system_id == kInvalidId || !has_system(jp.system_id)) {
      if (fallback_system != kInvalidId) {
        note(join("Fix: JumpPoint ", id_u64(jid), " had unknown system_id ", id_u64(jp.system_id), "; set to ",
                  id_u64(fallback_system)));
        jp.system_id = fallback_system;
      }
    }

    if (jp.linked_jump_id == jid) {
      note(join("Fix: JumpPoint ", id_u64(jid), " linked to itself; cleared linked_jump_id"));
      jp.linked_jump_id = kInvalidId;
    }
    if (jp.linked_jump_id != kInvalidId && !has_jump(jp.linked_jump_id)) {
      note(join("Fix: JumpPoint ", id_u64(jid), " linked_jump_id ", id_u64(jp.linked_jump_id), " missing; cleared"));
      jp.linked_jump_id = kInvalidId;
    }
  }

  for (auto& [sid, sh] : s.ships) {
    if (sh.system_id == kInvalidId || !has_system(sh.system_id)) {
      if (fallback_system != kInvalidId) {
        note(join("Fix: Ship ", id_u64(sid), " had unknown system_id ", id_u64(sh.system_id), "; set to ",
                  id_u64(fallback_system)));
        sh.system_id = fallback_system;
      }
    }
    if (sh.faction_id == kInvalidId || !has_faction(sh.faction_id)) {
      if (first_faction != kInvalidId) {
        note(join("Fix: Ship ", id_u64(sid), " had unknown faction_id ", id_u64(sh.faction_id), "; set to ",
                  id_u64(first_faction)));
        sh.faction_id = first_faction;
      }
    }

    if (content && !design_exists(s, *content, sh.design_id)) {
      if (!fallback_design.empty()) {
        note(join("Fix: Ship ", id_u64(sid), " had unknown design_id '", sh.design_id, "'; set to '", fallback_design,
                  "'"));
        sh.design_id = fallback_design;
      }
    }

    // Sanitize power policy in case a save/mod introduced duplicates or
    // missing subsystems in the priority list.
    {
      const auto before = sh.power_policy.priority;
      sanitize_power_policy(sh.power_policy);
      if (sh.power_policy.priority != before) {
        note(join("Fix: Ship ", id_u64(sid), " had invalid power_policy priority; sanitized"));
      }
    }

    // Clamp persisted sensor_mode.
    {
      const int sm = static_cast<int>(sh.sensor_mode);
      if (sm < 0 || sm > 2) {
        note(join("Fix: Ship ", id_u64(sid), " had invalid sensor_mode; clamped to Normal"));
        sh.sensor_mode = SensorMode::Normal;
      }
    }

    // Clamp persisted repair_priority.
    {
      const int rp = static_cast<int>(sh.repair_priority);
      if (rp < 0 || rp > 2) {
        note(join("Fix: Ship ", id_u64(sid), " had invalid repair_priority; set to Normal"));
        sh.repair_priority = RepairPriority::Normal;
      }
    }

    // Clamp persisted crew_complement (0..1).
    {
      if (!std::isfinite(sh.crew_complement) || sh.crew_complement < 0.0) {
        note(join("Fix: Ship ", id_u64(sid), " had NaN/inf crew_complement; set to 1.0"));
        sh.crew_complement = 1.0;
      }
      const double before = sh.crew_complement;
      sh.crew_complement = std::clamp(sh.crew_complement, 0.0, 1.0);
      if (sh.crew_complement != before) {
        note(join("Fix: Ship ", id_u64(sid), " had out-of-range crew_complement; clamped"));
      }
    }

    // Clamp persisted auto_* thresholds.
    {
      if (!std::isfinite(sh.auto_refuel_threshold_fraction)) {
        note(join("Fix: Ship ", id_u64(sid), " had NaN/inf auto_refuel_threshold_fraction; set to 0.25"));
        sh.auto_refuel_threshold_fraction = 0.25;
      }
      const double before_refuel = sh.auto_refuel_threshold_fraction;
      sh.auto_refuel_threshold_fraction = std::clamp(sh.auto_refuel_threshold_fraction, 0.0, 1.0);
      if (sh.auto_refuel_threshold_fraction != before_refuel) {
        note(join("Fix: Ship ", id_u64(sid), " had out-of-range auto_refuel_threshold_fraction; clamped"));
      }

      if (!std::isfinite(sh.auto_rearm_threshold_fraction)) {
        note(join("Fix: Ship ", id_u64(sid), " had NaN/inf auto_rearm_threshold_fraction; set to 0.25"));
        sh.auto_rearm_threshold_fraction = 0.25;
      }
      const double before_rearm = sh.auto_rearm_threshold_fraction;
      sh.auto_rearm_threshold_fraction = std::clamp(sh.auto_rearm_threshold_fraction, 0.0, 1.0);
      if (sh.auto_rearm_threshold_fraction != before_rearm) {
        note(join("Fix: Ship ", id_u64(sid), " had out-of-range auto_rearm_threshold_fraction; clamped"));
      }

      if (!std::isfinite(sh.auto_repair_threshold_fraction)) {
        note(join("Fix: Ship ", id_u64(sid), " had NaN/inf auto_repair_threshold_fraction; set to 0.75"));
        sh.auto_repair_threshold_fraction = 0.75;
      }
      const double before_repair = sh.auto_repair_threshold_fraction;
      sh.auto_repair_threshold_fraction = std::clamp(sh.auto_repair_threshold_fraction, 0.0, 1.0);
      if (sh.auto_repair_threshold_fraction != before_repair) {
        note(join("Fix: Ship ", id_u64(sid), " had out-of-range auto_repair_threshold_fraction; clamped"));
      }

      if (!std::isfinite(sh.auto_tanker_reserve_fraction)) {
        note(join("Fix: Ship ", id_u64(sid), " had NaN/inf auto_tanker_reserve_fraction; set to 0.25"));
        sh.auto_tanker_reserve_fraction = 0.25;
      }
      const double before_reserve = sh.auto_tanker_reserve_fraction;
      sh.auto_tanker_reserve_fraction = std::clamp(sh.auto_tanker_reserve_fraction, 0.0, 1.0);
      if (sh.auto_tanker_reserve_fraction != before_reserve) {
        note(join("Fix: Ship ", id_u64(sid), " had out-of-range auto_tanker_reserve_fraction; clamped"));
      }
    }
  }

  // --- Wrecks ---
  {
    const std::int64_t now = s.date.days_since_epoch();
    for (Id wid : sorted_keys(s.wrecks)) {
      auto it = s.wrecks.find(wid);
      if (it == s.wrecks.end()) continue;
      auto& w = it->second;

      if (w.system_id == kInvalidId || !has_system(w.system_id)) {
        note(join("Fix: Wreck ", id_u64(wid), " ('", w.name, "') referenced missing system_id ", id_u64(w.system_id),
                  "; removed"));
        s.wrecks.erase(it);
        continue;
      }

      if (w.name.empty()) {
        note(join("Fix: Wreck ", id_u64(wid), " had empty name; set"));
        w.name = join("Wreck ", id_u64(wid));
      }

      // Ensure wreck kind is in-range (corrupt/old saves).
      {
        const int k = static_cast<int>(w.kind);
        if (k < static_cast<int>(WreckKind::Ship) || k > static_cast<int>(WreckKind::Debris)) {
          note(join("Fix: Wreck ", id_u64(wid), " ('", w.name, "') had invalid kind ", k, "; set to ship"));
          w.kind = WreckKind::Ship;
        }
      }

      // Heuristic cleanup for pre-kind saves: anomaly overflow caches were historically stored
      // as plain wrecks with source metadata pointing at the investigating ship. That could
      // enable unintended reverse engineering if another faction salvaged the cache.
      //
      // If a wreck is (or appears to be) a salvage cache, force kind=Cache and clear source metadata.
      const bool name_says_cache = (w.name.rfind("Salvage Cache", 0) == 0);
      const bool should_be_cache = name_says_cache || (w.kind == WreckKind::Cache);
      if (should_be_cache) {
        if (w.kind != WreckKind::Cache) {
          note(join("Fix: Wreck ", id_u64(wid), " ('", w.name, "') looked like a salvage cache; set kind=cache"));
          w.kind = WreckKind::Cache;
        }
        if (w.source_ship_id != kInvalidId || w.source_faction_id != kInvalidId || !w.source_design_id.empty()) {
          note(join("Fix: Wreck ", id_u64(wid), " ('", w.name, "') is a salvage cache but had source metadata; cleared"));
          w.source_ship_id = kInvalidId;
          w.source_faction_id = kInvalidId;
          w.source_design_id.clear();
        }
      }

      if (w.created_day <= 0 || w.created_day > now) {
        note(join("Fix: Wreck ", id_u64(wid), " had invalid created_day ", w.created_day, "; set to ", now));
        w.created_day = now;
      }

      for (auto mit = w.minerals.begin(); mit != w.minerals.end(); ) {
        double v = mit->second;
        if (!std::isfinite(v) || v <= 1e-9) {
          mit = w.minerals.erase(mit);
          continue;
        }
        if (v < 0.0) {
          note(join("Fix: Wreck ", id_u64(wid), " mineral '", mit->first, "' had negative value ", v, "; clamped"));
          v = 0.0;
          mit->second = v;
        }
        ++mit;
      }

      if (w.minerals.empty()) {
        note(join("Fix: Wreck ", id_u64(wid), " ('", w.name, "') had no salvage; removed"));
        s.wrecks.erase(it);
      }
    }
  }



  // --- Contracts ---
  for (Id cid : sorted_keys(s.contracts)) {
    auto it = s.contracts.find(cid);
    if (it == s.contracts.end()) continue;
    auto& c = it->second;

    // Normalize enum ranges (defensive vs corrupt saves).
    {
      const int k = static_cast<int>(c.kind);
      // Keep this in sync with the last ContractKind enumerator.
      const int kMaxKind = static_cast<int>(ContractKind::BountyPirate);
      if (k < 0 || k > kMaxKind) {
        note(join("Fix: Contract ", id_u64(cid), " had invalid kind ", k, "; set to investigate_anomaly"));
        c.kind = ContractKind::InvestigateAnomaly;
      }
      const int st = static_cast<int>(c.status);
      if (st < 0 || st > 4) {
        note(join("Fix: Contract ", id_u64(cid), " had invalid status ", st, "; set to offered"));
        c.status = ContractStatus::Offered;
      }
    }

    if (c.name.empty()) {
      note(join("Fix: Contract ", id_u64(cid), " had empty name; set"));
      c.name = join("Contract ", id_u64(cid));
    }

    if (c.issuer_faction_id != kInvalidId && !has_faction(c.issuer_faction_id)) {
      note(join("Fix: Contract ", id_u64(cid), " had missing issuer_faction_id ", id_u64(c.issuer_faction_id),
                "; cleared"));
      c.issuer_faction_id = kInvalidId;
    }
    if (c.assignee_faction_id != kInvalidId && !has_faction(c.assignee_faction_id)) {
      note(join("Fix: Contract ", id_u64(cid), " had missing assignee_faction_id ", id_u64(c.assignee_faction_id),
                "; cleared"));
      c.assignee_faction_id = kInvalidId;
    }

    if (c.system_id != kInvalidId && !has_system(c.system_id)) {
      if (fallback_system != kInvalidId) {
        note(join("Fix: Contract ", id_u64(cid), " had missing system_id ", id_u64(c.system_id), "; set to ",
                  id_u64(fallback_system)));
        c.system_id = fallback_system;
      } else {
        note(join("Fix: Contract ", id_u64(cid), " had missing system_id ", id_u64(c.system_id), "; cleared"));
        c.system_id = kInvalidId;
      }
    }

    if (c.assigned_ship_id != kInvalidId && !has_ship(c.assigned_ship_id)) {
      note(join("Fix: Contract ", id_u64(cid), " had missing assigned_ship_id ", id_u64(c.assigned_ship_id),
                "; cleared"));
      c.assigned_ship_id = kInvalidId;
    }
    if (c.assigned_fleet_id != kInvalidId && s.fleets.find(c.assigned_fleet_id) == s.fleets.end()) {
      note(join("Fix: Contract ", id_u64(cid), " had missing assigned_fleet_id ", id_u64(c.assigned_fleet_id),
                "; cleared"));
      c.assigned_fleet_id = kInvalidId;
    }

    if (c.target_destroyed_by_faction_id != kInvalidId && !has_faction(c.target_destroyed_by_faction_id)) {
      note(join("Fix: Contract ", id_u64(cid), " had missing target_destroyed_by_faction_id ",
                id_u64(c.target_destroyed_by_faction_id), "; cleared"));
      c.target_destroyed_by_faction_id = kInvalidId;
    }
    if (c.target_destroyed_day < 0) {
      note(join("Fix: Contract ", id_u64(cid), " had negative target_destroyed_day; clamped"));
      c.target_destroyed_day = 0;
    }
    if (c.kind == ContractKind::BountyPirate) {
      // target_id2 is optionally used as last-known / kill system id; keep it valid.
      if (c.target_id2 != kInvalidId && !has_system(c.target_id2)) {
        note(join("Fix: Contract ", id_u64(cid), " bounty contract had missing target_id2 system ",
                  id_u64(c.target_id2), "; cleared"));
        c.target_id2 = kInvalidId;
      }
    }

    // Target integrity: if the target entity is missing, drop the contract entirely.
    bool target_ok = true;
    if (c.kind == ContractKind::EscortConvoy) {
      // EscortConvoy relies on *two* references: convoy ship + destination system.
      if (c.target_id == kInvalidId || !has_ship(c.target_id)) target_ok = false;
      if (c.target_id2 == kInvalidId || !has_system(c.target_id2)) target_ok = false;
    } else if (c.target_id != kInvalidId) {
      switch (c.kind) {
        case ContractKind::InvestigateAnomaly: target_ok = has_anomaly(c.target_id); break;
        case ContractKind::SalvageWreck: target_ok = has_wreck(c.target_id); break;
        case ContractKind::SurveyJumpPoint: target_ok = has_jump(c.target_id); break;
        case ContractKind::BountyPirate:
          // If the target ship has been destroyed and the save captured the attribution,
          // keep the contract so it can be resolved later (e.g. next daily tick).
          target_ok = has_ship(c.target_id) || (c.target_destroyed_day != 0);
          break;
        case ContractKind::EscortConvoy: break;
      }
    }
    if (!target_ok) {
      note(join("Fix: Contract ", id_u64(cid), " referenced missing target ", id_u64(c.target_id), "; removed"));
      s.contracts.erase(it);
      continue;
    }

    if (!std::isfinite(c.risk_estimate) || c.risk_estimate < 0.0) {
      note(join("Fix: Contract ", id_u64(cid), " had invalid risk_estimate; set to 0"));
      c.risk_estimate = 0.0;
    }
    if (c.hops_estimate < 0) {
      note(join("Fix: Contract ", id_u64(cid), " had negative hops_estimate; clamped"));
      c.hops_estimate = 0;
    }
    if (!std::isfinite(c.reward_research_points) || c.reward_research_points < 0.0) {
      note(join("Fix: Contract ", id_u64(cid), " had invalid reward_research_points; set to 0"));
      c.reward_research_points = 0.0;
    }
  }
  // --- Colonies ---
  // Remove colonies that point at missing bodies, and ensure body_id uniqueness.
  {
    std::unordered_map<Id, Id> body_owner;
    body_owner.reserve(s.colonies.size() * 2);
    for (Id cid : sorted_keys(s.colonies)) {
      auto it = s.colonies.find(cid);
      if (it == s.colonies.end()) continue;
      Colony& c = it->second;

      if (c.body_id == kInvalidId || !has_body(c.body_id)) {
        note(join("Fix: Removed Colony ", id_u64(cid), " ('", c.name, "') missing body_id ", id_u64(c.body_id)));
        s.colonies.erase(it);
        continue;
      }

      auto [own_it, inserted] = body_owner.insert({c.body_id, cid});
      if (!inserted) {
        note(join("Fix: Removed duplicate Colony ", id_u64(cid), " ('", c.name, "') sharing body_id ",
                  id_u64(c.body_id), " with colony ", id_u64(own_it->second)));
        s.colonies.erase(it);
        continue;
      }

      if (c.faction_id == kInvalidId || !has_faction(c.faction_id)) {
        if (first_faction != kInvalidId) {
          note(join("Fix: Colony ", id_u64(cid), " had unknown faction_id ", id_u64(c.faction_id), "; set to ",
                    id_u64(first_faction)));
          c.faction_id = first_faction;
        }
      }

      // Clamp ground combat / training automation fields.
      {
        if (!std::isfinite(c.ground_forces) || c.ground_forces < 0.0) {
          const double old = c.ground_forces;
          c.ground_forces = std::max(0.0, std::isfinite(old) ? old : 0.0);
          note(join("Fix: Colony ", id_u64(cid), " ground_forces clamped ", old, " -> ", c.ground_forces));
        }

        if (!std::isfinite(c.troop_training_queue) || c.troop_training_queue < 0.0) {
          const double old = c.troop_training_queue;
          c.troop_training_queue = std::max(0.0, std::isfinite(old) ? old : 0.0);
          note(join("Fix: Colony ", id_u64(cid), " troop_training_queue clamped ", old, " -> ", c.troop_training_queue));
        }

        if (!std::isfinite(c.troop_training_auto_queued) || c.troop_training_auto_queued < 0.0) {
          const double old = c.troop_training_auto_queued;
          c.troop_training_auto_queued = std::max(0.0, std::isfinite(old) ? old : 0.0);
          note(join("Fix: Colony ", id_u64(cid), " troop_training_auto_queued clamped ", old, " -> ",
                    c.troop_training_auto_queued));
        }

        const double before_auto = c.troop_training_auto_queued;
        c.troop_training_auto_queued = std::clamp(c.troop_training_auto_queued, 0.0, c.troop_training_queue);
        if (c.troop_training_auto_queued != before_auto) {
          note(join("Fix: Colony ", id_u64(cid), " troop_training_auto_queued clamped to queue: ", before_auto, " -> ",
                    c.troop_training_auto_queued));
        }

        if (!std::isfinite(c.garrison_target_strength) || c.garrison_target_strength < 0.0) {
          const double old = c.garrison_target_strength;
          c.garrison_target_strength = std::max(0.0, std::isfinite(old) ? old : 0.0);
          note(join("Fix: Colony ", id_u64(cid), " garrison_target_strength clamped ", old, " -> ",
                    c.garrison_target_strength));
        }
      }

      // Clamp mineral stockpiles/reserves.
      for (auto it2 = c.minerals.begin(); it2 != c.minerals.end();) {
        if (it2->first.empty()) {
          note(join("Fix: Colony ", id_u64(cid), " had empty mineral key in minerals; removed"));
          it2 = c.minerals.erase(it2);
          continue;
        }
        if (!std::isfinite(it2->second) || it2->second < 0.0) {
          const double old = it2->second;
          it2->second = std::max(0.0, std::isfinite(it2->second) ? it2->second : 0.0);
          note(join("Fix: Colony ", id_u64(cid), " minerals['", it2->first, "'] clamped ", old, " -> ", it2->second));
        }
        ++it2;
      }
      for (auto it2 = c.mineral_reserves.begin(); it2 != c.mineral_reserves.end();) {
        if (it2->first.empty()) {
          note(join("Fix: Colony ", id_u64(cid), " had empty mineral key in mineral_reserves; removed"));
          it2 = c.mineral_reserves.erase(it2);
          continue;
        }
        if (!std::isfinite(it2->second) || it2->second < 0.0) {
          const double old = it2->second;
          it2->second = std::max(0.0, std::isfinite(it2->second) ? it2->second : 0.0);
          note(join("Fix: Colony ", id_u64(cid), " mineral_reserves['", it2->first, "'] clamped ", old, " -> ",
                    it2->second));
        }
        ++it2;
      }
      for (auto it2 = c.mineral_targets.begin(); it2 != c.mineral_targets.end();) {
        if (it2->first.empty()) {
          note(join("Fix: Colony ", id_u64(cid), " had empty mineral key in mineral_targets; removed"));
          it2 = c.mineral_targets.erase(it2);
          continue;
        }
        if (!std::isfinite(it2->second) || it2->second < 0.0) {
          const double old = it2->second;
          it2->second = std::max(0.0, std::isfinite(it2->second) ? it2->second : 0.0);
          note(join("Fix: Colony ", id_u64(cid), " mineral_targets['", it2->first, "'] clamped ", old, " -> ", it2->second));
        }
        ++it2;
      }

      for (auto it2 = c.installation_targets.begin(); it2 != c.installation_targets.end();) {
        if (it2->first.empty()) {
          note(join("Fix: Colony ", id_u64(cid), " had empty installation key in installation_targets; removed"));
          it2 = c.installation_targets.erase(it2);
          continue;
        }
        if (it2->second < 0) {
          const int old = it2->second;
          it2->second = std::max(0, it2->second);
          note(join("Fix: Colony ", id_u64(cid), " installation_targets['", it2->first, "'] clamped ", old, " -> ",
                    it2->second));
        }
        ++it2;
      }


      if (content) {
        // Installations: remove unknown ids and clamp negative counts.
        for (auto it3 = c.installations.begin(); it3 != c.installations.end();) {
          const std::string& inst_id = it3->first;
          const int count = it3->second;
          if (inst_id.empty() || !installation_exists(*content, inst_id)) {
            note(join("Fix: Colony ", id_u64(cid), " removed unknown installation id '", inst_id, "'"));
            it3 = c.installations.erase(it3);
            continue;
          }
          if (count < 0) {
            note(join("Fix: Colony ", id_u64(cid), " installation '", inst_id, "' count clamped ", count, " -> 0"));
            it3->second = 0;
          }
          if (it3->second == 0) {
            it3 = c.installations.erase(it3);
            continue;
          }
          ++it3;
        }

        // Installation targets: remove unknown installation ids and clamp negatives.
        for (auto it3 = c.installation_targets.begin(); it3 != c.installation_targets.end();) {
          const std::string& inst_id = it3->first;
          const int target = it3->second;
          if (inst_id.empty() || !installation_exists(*content, inst_id)) {
            note(join("Fix: Colony ", id_u64(cid), " removed unknown installation target id '", inst_id, "'"));
            it3 = c.installation_targets.erase(it3);
            continue;
          }
          if (target < 0) {
            const int old = target;
            it3->second = std::max(0, target);
            note(join("Fix: Colony ", id_u64(cid), " installation_targets['", inst_id, "'] clamped ", old, " -> ",
                      it3->second));
          }
          ++it3;
        }


        // Shipyard queue: drop entries with unknown designs.
        for (std::size_t i = 0; i < c.shipyard_queue.size();) {
          auto& bo = c.shipyard_queue[i];
          if (bo.design_id.empty() || !design_exists(s, *content, bo.design_id)) {
            note(join("Fix: Colony ", id_u64(cid), " dropped shipyard_queue entry with unknown design_id '",
                      bo.design_id, "'"));
            c.shipyard_queue.erase(c.shipyard_queue.begin() + static_cast<long>(i));
            continue;
          }
          if (!std::isfinite(bo.tons_remaining) || bo.tons_remaining < 0.0) {
            const double old = bo.tons_remaining;
            bo.tons_remaining = std::max(0.0, std::isfinite(old) ? old : 0.0);
            note(join("Fix: Colony ", id_u64(cid), " shipyard_queue tons_remaining clamped ", old, " -> ", bo.tons_remaining));
          }

          // Optional metadata: clear invalid references.
          if (!bo.apply_ship_profile_name.empty()) {
            const auto itf = s.factions.find(c.faction_id);
            const bool ok = (itf != s.factions.end() &&
                             itf->second.ship_profiles.find(bo.apply_ship_profile_name) != itf->second.ship_profiles.end());
            if (!ok) {
              note(join("Fix: Colony ", id_u64(cid), " shipyard_queue cleared unknown ship profile '",
                        bo.apply_ship_profile_name, "'"));
              bo.apply_ship_profile_name.clear();
            }
          }

          if (bo.assign_to_fleet_id != kInvalidId) {
            const auto itfl = s.fleets.find(bo.assign_to_fleet_id);
            const bool ok = (itfl != s.fleets.end() &&
                             (itfl->second.faction_id == kInvalidId || itfl->second.faction_id == c.faction_id));
            if (!ok) {
              note(join("Fix: Colony ", id_u64(cid), " shipyard_queue cleared invalid assign_to_fleet_id ",
                        id_u64(bo.assign_to_fleet_id)));
              bo.assign_to_fleet_id = kInvalidId;
            }
          }

          if (bo.rally_to_colony_id != kInvalidId) {
            const auto itc2 = s.colonies.find(bo.rally_to_colony_id);
            const bool ok = (itc2 != s.colonies.end() &&
                             (itc2->second.faction_id == kInvalidId || itc2->second.faction_id == c.faction_id));
            if (!ok) {
              note(join("Fix: Colony ", id_u64(cid), " shipyard_queue cleared invalid rally_to_colony_id ",
                        id_u64(bo.rally_to_colony_id)));
              bo.rally_to_colony_id = kInvalidId;
            }
          }

          ++i;
        }

        // Construction queue: drop entries with unknown installations and clamp negatives.
        for (std::size_t i = 0; i < c.construction_queue.size();) {
          auto& io = c.construction_queue[i];
          if (io.installation_id.empty() || !installation_exists(*content, io.installation_id)) {
            note(join("Fix: Colony ", id_u64(cid), " dropped construction_queue entry with unknown installation_id '",
                      io.installation_id, "'"));
            c.construction_queue.erase(c.construction_queue.begin() + static_cast<long>(i));
            continue;
          }
          if (io.quantity_remaining < 0) {
            note(join("Fix: Colony ", id_u64(cid), " construction_queue quantity_remaining clamped ",
                      io.quantity_remaining, " -> 0"));
            io.quantity_remaining = 0;
          }
          if (!std::isfinite(io.cp_remaining) || io.cp_remaining < 0.0) {
            const double old = io.cp_remaining;
            io.cp_remaining = std::max(0.0, std::isfinite(old) ? old : 0.0);
            note(join("Fix: Colony ", id_u64(cid), " construction_queue cp_remaining clamped ", old, " -> ",
                      io.cp_remaining));
          }
          if (io.quantity_remaining == 0) {
            c.construction_queue.erase(c.construction_queue.begin() + static_cast<long>(i));
            continue;
          }
          ++i;
        }
      }
    }
  }

  // --- Jump point reciprocity ---
  // First, opportunistically repair one-way links when the target has no link.
  {
    const auto jp_ids = sorted_keys(s.jump_points);
    for (Id id : jp_ids) {
      auto it = s.jump_points.find(id);
      if (it == s.jump_points.end()) continue;
      JumpPoint& jp = it->second;
      if (jp.linked_jump_id == kInvalidId) continue;
      auto it2 = s.jump_points.find(jp.linked_jump_id);
      if (it2 == s.jump_points.end()) {
        jp.linked_jump_id = kInvalidId;
        continue;
      }
      if (it2->second.linked_jump_id == kInvalidId) {
        it2->second.linked_jump_id = id;
        note(join("Fix: JumpPoint ", id_u64(jp.linked_jump_id), " linked back to ", id_u64(id)));
      }
    }

    // Then, drop any remaining non-reciprocal links.
    for (Id id : jp_ids) {
      auto it = s.jump_points.find(id);
      if (it == s.jump_points.end()) continue;
      JumpPoint& jp = it->second;
      if (jp.linked_jump_id == kInvalidId) continue;

      auto it2 = s.jump_points.find(jp.linked_jump_id);
      if (it2 == s.jump_points.end() || it2->second.linked_jump_id != id) {
        note(join("Fix: JumpPoint ", id_u64(id), " had non-reciprocal link to ", id_u64(jp.linked_jump_id), "; cleared"));
        jp.linked_jump_id = kInvalidId;
      }
    }
  }

  // --- Fleets ---
  {
    std::unordered_map<Id, Id> ship_to_fleet;
    ship_to_fleet.reserve(s.ships.size() * 2);

    for (Id fid : sorted_keys(s.fleets)) {
      auto it = s.fleets.find(fid);
      if (it == s.fleets.end()) continue;
      Fleet& fl = it->second;

      // Prune ship ids.
      std::vector<Id> kept;
      kept.reserve(fl.ship_ids.size());
      std::unordered_set<Id> seen;
      seen.reserve(fl.ship_ids.size() * 2);

      for (Id sid : fl.ship_ids) {
        if (sid == kInvalidId) {
          note(join("Fix: Fleet ", id_u64(fid), " removed kInvalidId from ship_ids"));
          continue;
        }
        if (!has_ship(sid)) {
          note(join("Fix: Fleet ", id_u64(fid), " removed missing ship_id ", id_u64(sid)));
          continue;
        }
        if (!seen.insert(sid).second) {
          note(join("Fix: Fleet ", id_u64(fid), " removed duplicate ship_id ", id_u64(sid)));
          continue;
        }
        kept.push_back(sid);
      }

      fl.ship_ids = std::move(kept);
      sort_unique(fl.ship_ids);

      if (fl.ship_ids.empty()) {
        note(join("Fix: Removed empty Fleet ", id_u64(fid), " ('", fl.name, "')"));
        s.fleets.erase(it);
        continue;
      }

      // Fix faction_id: if invalid, infer from the first ship.
      if (fl.faction_id == kInvalidId || !has_faction(fl.faction_id)) {
        const Id inferred = s.ships.at(fl.ship_ids.front()).faction_id;
        if (inferred != kInvalidId && has_faction(inferred)) {
          note(join("Fix: Fleet ", id_u64(fid), " had invalid faction_id ", id_u64(fl.faction_id), "; set to ",
                    id_u64(inferred)));
          fl.faction_id = inferred;
        } else if (first_faction != kInvalidId) {
          note(join("Fix: Fleet ", id_u64(fid), " had invalid faction_id ", id_u64(fl.faction_id), "; set to ",
                    id_u64(first_faction)));
          fl.faction_id = first_faction;
        }
      }

      // Remove ships that don't match the fleet's faction.
      {
        std::vector<Id> filtered;
        filtered.reserve(fl.ship_ids.size());
        for (Id sid : fl.ship_ids) {
          if (!has_ship(sid)) continue;
          const Id sf = s.ships.at(sid).faction_id;
          if (fl.faction_id != kInvalidId && sf != fl.faction_id) {
            note(join("Fix: Fleet ", id_u64(fid), " removed ship ", id_u64(sid), " faction mismatch"));
            continue;
          }
          filtered.push_back(sid);
        }
        fl.ship_ids = std::move(filtered);
        sort_unique(fl.ship_ids);
      }

      if (fl.ship_ids.empty()) {
        note(join("Fix: Removed Fleet ", id_u64(fid), " after pruning mismatched ships"));
        s.fleets.erase(fid);
        continue;
      }

      // Ensure ships appear in at most one fleet (keep lowest fleet id).
      {
        std::vector<Id> filtered;
        filtered.reserve(fl.ship_ids.size());
        for (Id sid : fl.ship_ids) {
          auto [it_sf, inserted] = ship_to_fleet.insert({sid, fid});
          if (!inserted && it_sf->second != fid) {
            note(join("Fix: Removed ship ", id_u64(sid), " from Fleet ", id_u64(fid),
                      " (already in Fleet ", id_u64(it_sf->second), ")"));
            continue;
          }
          filtered.push_back(sid);
        }
        fl.ship_ids = std::move(filtered);
        sort_unique(fl.ship_ids);
      }

      if (fl.ship_ids.empty()) {
        note(join("Fix: Removed Fleet ", id_u64(fid), " after resolving multi-fleet ships"));
        s.fleets.erase(fid);
        continue;
      }

      if (fl.leader_ship_id == kInvalidId || std::find(fl.ship_ids.begin(), fl.ship_ids.end(), fl.leader_ship_id) == fl.ship_ids.end()) {
        const Id new_leader = fl.ship_ids.front();
        note(join("Fix: Fleet ", id_u64(fid), " leader_ship_id set to ", id_u64(new_leader)));
        fl.leader_ship_id = new_leader;
      }
    }
  }

  // --- Ship orders + templates ---
  auto fix_order_list = [&](std::vector<Order>& list, Id self_ship_id, const char* list_name, const std::string& owner) {
    std::vector<Order> out;
    out.reserve(list.size());

    auto drop = [&](std::size_t idx, const char* why) {
      note(join("Fix: ", owner, " ", list_name, "[", idx, "] dropped: ", why));
    };

    for (std::size_t i = 0; i < list.size(); ++i) {
      Order ord_any = list[i];
      bool keep = true;
      std::visit(
          [&](auto& ord) {
            using T = std::decay_t<decltype(ord)>;
            if constexpr (std::is_same_v<T, MoveToBody>) {
              if (ord.body_id == kInvalidId || !has_body(ord.body_id)) {
                keep = false;
                drop(i, "MoveToBody invalid body_id");
              }
            } else if constexpr (std::is_same_v<T, ColonizeBody>) {
              if (ord.body_id == kInvalidId || !has_body(ord.body_id)) {
                keep = false;
                drop(i, "ColonizeBody invalid body_id");
              }
            } else if constexpr (std::is_same_v<T, TravelViaJump>) {
              if (ord.jump_point_id == kInvalidId || !has_jump(ord.jump_point_id)) {
                keep = false;
                drop(i, "TravelViaJump invalid jump_point_id");
              }
            } else if constexpr (std::is_same_v<T, SurveyJumpPoint>) {
              if (ord.jump_point_id == kInvalidId || !has_jump(ord.jump_point_id)) {
                keep = false;
                drop(i, "SurveyJumpPoint invalid jump_point_id");
              }
            } else if constexpr (std::is_same_v<T, AttackShip>) {
              if (ord.target_ship_id == kInvalidId || !has_ship(ord.target_ship_id) ||
                  (self_ship_id != kInvalidId && ord.target_ship_id == self_ship_id)) {
                keep = false;
                drop(i, "AttackShip invalid target_ship_id");
              }

              // Normalize lost-contact search state.
              if (ord.search_waypoint_index < 0) {
                note(join("Fix: ", owner, " ", list_name, "[", i, "] AttackShip search_waypoint_index clamped ",
                          ord.search_waypoint_index, " -> 0"));
                ord.search_waypoint_index = 0;
              }
              if (!std::isfinite(ord.search_offset_mkm.x) || !std::isfinite(ord.search_offset_mkm.y)) {
                note(join("Fix: ", owner, " ", list_name, "[", i, "] AttackShip search_offset_mkm cleared"));
                ord.search_offset_mkm = Vec2{0.0, 0.0};
                ord.has_search_offset = false;
              }
              if (ord.has_search_offset && ord.search_waypoint_index <= 0) {
                note(join("Fix: ", owner, " ", list_name, "[", i,
                          "] AttackShip search_waypoint_index promoted 0 -> 1 (has_search_offset)"));
                ord.search_waypoint_index = 1;
              }
              if (!ord.has_search_offset) {
                ord.search_offset_mkm = Vec2{0.0, 0.0};
              }
            } else if constexpr (std::is_same_v<T, EscortShip>) {
              if (ord.target_ship_id == kInvalidId || !has_ship(ord.target_ship_id) ||
                  (self_ship_id != kInvalidId && ord.target_ship_id == self_ship_id)) {
                keep = false;
                drop(i, "EscortShip invalid target_ship_id");
              }
              if (!std::isfinite(ord.follow_distance_mkm) || ord.follow_distance_mkm < 0.0) {
                note(join("Fix: ", owner, " ", list_name, "[", i, "] EscortShip follow_distance_mkm clamped ",
                          ord.follow_distance_mkm, " -> 1.0"));
                ord.follow_distance_mkm = 1.0;
              }
            } else if constexpr (std::is_same_v<T, WaitDays>) {
              if (ord.days_remaining < 0) {
                note(join("Fix: ", owner, " ", list_name, "[", i, "] WaitDays clamped ", ord.days_remaining,
                          " -> 0"));
                ord.days_remaining = 0;
              }
            } else if constexpr (std::is_same_v<T, LoadMineral>) {
              if (ord.colony_id == kInvalidId || !has_colony(ord.colony_id)) {
                keep = false;
                drop(i, "LoadMineral invalid colony_id");
              }
            } else if constexpr (std::is_same_v<T, UnloadMineral>) {
              if (ord.colony_id == kInvalidId || !has_colony(ord.colony_id)) {
                keep = false;
                drop(i, "UnloadMineral invalid colony_id");
              }
            } else if constexpr (std::is_same_v<T, MineBody>) {
              if (ord.body_id == kInvalidId || !has_body(ord.body_id)) {
                keep = false;
                drop(i, "MineBody invalid body_id");
              }
            } else if constexpr (std::is_same_v<T, LoadTroops>) {
              if (ord.colony_id == kInvalidId || !has_colony(ord.colony_id)) {
                keep = false;
                drop(i, "LoadTroops invalid colony_id");
              }
            } else if constexpr (std::is_same_v<T, UnloadTroops>) {
              if (ord.colony_id == kInvalidId || !has_colony(ord.colony_id)) {
                keep = false;
                drop(i, "UnloadTroops invalid colony_id");
              }
            } else if constexpr (std::is_same_v<T, LoadColonists>) {
              if (ord.colony_id == kInvalidId || !has_colony(ord.colony_id)) {
                keep = false;
                drop(i, "LoadColonists invalid colony_id");
              }
              if (!std::isfinite(ord.millions) || ord.millions < 0.0) {
                note(join("Fix: ", owner, " ", list_name, "[", i, "] LoadColonists millions clamped ", ord.millions,
                          " -> 0"));
                ord.millions = 0.0;
              }
            } else if constexpr (std::is_same_v<T, UnloadColonists>) {
              if (ord.colony_id == kInvalidId || !has_colony(ord.colony_id)) {
                keep = false;
                drop(i, "UnloadColonists invalid colony_id");
              }
              if (!std::isfinite(ord.millions) || ord.millions < 0.0) {
                note(join("Fix: ", owner, " ", list_name, "[", i, "] UnloadColonists millions clamped ", ord.millions,
                          " -> 0"));
                ord.millions = 0.0;
              }
            } else if constexpr (std::is_same_v<T, InvadeColony>) {
              if (ord.colony_id == kInvalidId || !has_colony(ord.colony_id)) {
                keep = false;
                drop(i, "InvadeColony invalid colony_id");
              }
            } else if constexpr (std::is_same_v<T, BombardColony>) {
              if (ord.colony_id == kInvalidId || !has_colony(ord.colony_id)) {
                keep = false;
                drop(i, "BombardColony invalid colony_id");
              }
              if (ord.duration_days < -1) {
                note(join("Fix: ", owner, " ", list_name, "[", i, "] BombardColony duration clamped ",
                          ord.duration_days, " -> -1"));
                ord.duration_days = -1;
              }
            } else if constexpr (std::is_same_v<T, SalvageWreck>) {
              if (ord.wreck_id == kInvalidId || !has_wreck(ord.wreck_id)) {
                keep = false;
                drop(i, "SalvageWreck invalid wreck_id");
              }
              if (!std::isfinite(ord.tons) || ord.tons < 0.0) {
                note(join("Fix: ", owner, " ", list_name, "[", i, "] SalvageWreck tons clamped ", ord.tons,
                          " -> 0"));
                ord.tons = 0.0;
              }
            } else if constexpr (std::is_same_v<T, SalvageWreckLoop>) {
              if (ord.wreck_id == kInvalidId || !has_wreck(ord.wreck_id)) {
                keep = false;
                drop(i, "SalvageWreckLoop invalid wreck_id");
              }
              if (ord.dropoff_colony_id != kInvalidId && !has_colony(ord.dropoff_colony_id)) {
                note(join("Fix: ", owner, " ", list_name, "[", i, "] SalvageWreckLoop dropoff_colony_id cleared ",
                          id_u64(ord.dropoff_colony_id)));
                ord.dropoff_colony_id = kInvalidId;
              }
              if (ord.mode != 0 && ord.mode != 1) {
                note(join("Fix: ", owner, " ", list_name, "[", i, "] SalvageWreckLoop mode clamped ", ord.mode,
                          " -> 0"));
                ord.mode = 0;
              }
            } else if constexpr (std::is_same_v<T, InvestigateAnomaly>) {
              if (ord.anomaly_id == kInvalidId || !has_anomaly(ord.anomaly_id)) {
                keep = false;
                drop(i, "InvestigateAnomaly invalid anomaly_id");
              }
              if (ord.duration_days < 0) {
                note(join("Fix: ", owner, " ", list_name, "[", i, "] InvestigateAnomaly duration clamped ",
                          ord.duration_days, " -> 0"));
                ord.duration_days = 0;
              }
              if (!std::isfinite(ord.progress_days) || ord.progress_days < 0.0) {
                note(join("Fix: ", owner, " ", list_name, "[", i, "] InvestigateAnomaly progress_days clamped ",
                          ord.progress_days, " -> 0"));
                ord.progress_days = 0.0;
              }
            } else if constexpr (std::is_same_v<T, OrbitBody>) {
              if (ord.body_id == kInvalidId || !has_body(ord.body_id)) {
                keep = false;
                drop(i, "OrbitBody invalid body_id");
              }
              if (ord.duration_days < -1) {
                note(join("Fix: ", owner, " ", list_name, "[", i, "] OrbitBody duration clamped ", ord.duration_days,
                          " -> -1"));
                ord.duration_days = -1;
              }
            } else if constexpr (std::is_same_v<T, TransferCargoToShip>) {
              if (ord.target_ship_id == kInvalidId || !has_ship(ord.target_ship_id) ||
                  (self_ship_id != kInvalidId && ord.target_ship_id == self_ship_id)) {
                keep = false;
                drop(i, "TransferCargoToShip invalid target_ship_id");
              }
            } else if constexpr (std::is_same_v<T, TransferFuelToShip>) {
              if (ord.target_ship_id == kInvalidId || !has_ship(ord.target_ship_id) ||
                  (self_ship_id != kInvalidId && ord.target_ship_id == self_ship_id)) {
                keep = false;
                drop(i, "TransferFuelToShip invalid target_ship_id");
              }
            } else if constexpr (std::is_same_v<T, TransferTroopsToShip>) {
              if (ord.target_ship_id == kInvalidId || !has_ship(ord.target_ship_id) ||
                  (self_ship_id != kInvalidId && ord.target_ship_id == self_ship_id)) {
                keep = false;
                drop(i, "TransferTroopsToShip invalid target_ship_id");
              }
            } else if constexpr (std::is_same_v<T, TransferColonistsToShip>) {
              if (ord.target_ship_id == kInvalidId || !has_ship(ord.target_ship_id) ||
                  (self_ship_id != kInvalidId && ord.target_ship_id == self_ship_id)) {
                keep = false;
                drop(i, "TransferColonistsToShip invalid target_ship_id");
              }
            } else if constexpr (std::is_same_v<T, ScrapShip>) {
              if (ord.colony_id == kInvalidId || !has_colony(ord.colony_id)) {
                keep = false;
                drop(i, "ScrapShip invalid colony_id");
              }
            } else if constexpr (std::is_same_v<T, MoveToPoint>) {
              // ok
            } else {
              keep = false;
              drop(i, "Unknown order type");
            }
          },
          ord_any);

      if (keep) out.push_back(std::move(ord_any));
    }

    list = std::move(out);
  };

  for (auto it = s.ship_orders.begin(); it != s.ship_orders.end();) {
    const Id sid = it->first;
    if (!has_ship(sid)) {
      note(join("Fix: Removed ship_orders entry for missing ship ", id_u64(sid)));
      it = s.ship_orders.erase(it);
      continue;
    }
    ShipOrders& so = it->second;
    fix_order_list(so.queue, sid, "order_queue", join("Ship ", id_u64(sid)));
    fix_order_list(so.repeat_template, sid, "repeat_template", join("Ship ", id_u64(sid)));

    if (so.suspended) {
      fix_order_list(so.suspended_queue, sid, "suspended_queue", join("Ship ", id_u64(sid)));
      fix_order_list(so.suspended_repeat_template, sid, "suspended_repeat_template", join("Ship ", id_u64(sid)));

      if (so.suspended_repeat_count_remaining < -1) {
        note(join("Fix: Ship ", id_u64(sid), " suspended_repeat_count_remaining clamped ",
                  so.suspended_repeat_count_remaining, " -> -1"));
        so.suspended_repeat_count_remaining = -1;
      }

      if (!so.suspended_repeat && so.suspended_repeat_count_remaining != 0) {
        note(join("Fix: Ship ", id_u64(sid), " suspended_repeat_count_remaining reset ",
                  so.suspended_repeat_count_remaining, " -> 0 (suspended_repeat disabled)"));
        so.suspended_repeat_count_remaining = 0;
      }

      if (so.suspended_repeat && so.suspended_repeat_template.empty()) {
        note(join("Fix: Ship ", id_u64(sid), " suspended_repeat disabled (empty suspended_repeat_template)"));
        so.suspended_repeat = false;
        so.suspended_repeat_count_remaining = 0;
      }
    }

    if (so.repeat_count_remaining < -1) {
      note(join("Fix: Ship ", id_u64(sid), " repeat_count_remaining clamped ", so.repeat_count_remaining,
                " -> -1"));
      so.repeat_count_remaining = -1;
    }

    if (!so.repeat && so.repeat_count_remaining != 0) {
      note(join("Fix: Ship ", id_u64(sid), " repeat_count_remaining reset ", so.repeat_count_remaining,
                " -> 0 (repeat disabled)"));
      so.repeat_count_remaining = 0;
    }

    if (so.repeat && so.repeat_template.empty()) {
      note(join("Fix: Ship ", id_u64(sid), " repeat disabled (empty repeat_template)"));
      so.repeat = false;
      so.repeat_count_remaining = 0;
    }
    ++it;
  }

  for (auto it = s.order_templates.begin(); it != s.order_templates.end();) {
    const std::string name = it->first;
    if (name.empty() || it->second.empty()) {
      note(join("Fix: Removed empty order template '", name, "'"));
      it = s.order_templates.erase(it);
      continue;
    }
    fix_order_list(it->second, kInvalidId, "orders", join("Template '", name, "'"));
    if (it->second.empty()) {
      note(join("Fix: Removed order template '", name, "' after dropping invalid orders"));
      it = s.order_templates.erase(it);
      continue;
    }
    ++it;
  }

  // --- Factions ---
  for (auto& [fid, f] : s.factions) {
    // discovered systems: remove missing + duplicates.
    {
      std::vector<Id> cleaned;
      cleaned.reserve(f.discovered_systems.size());
      for (Id sid : f.discovered_systems) {
        if (sid == kInvalidId || !has_system(sid)) {
          note(join("Fix: Faction ", id_u64(fid), " removed invalid discovered_system ", id_u64(sid)));
          continue;
        }
        cleaned.push_back(sid);
      }
      sort_unique(cleaned);
      f.discovered_systems = std::move(cleaned);
    }

    // surveyed jump points: remove missing + duplicates.
    {
      std::vector<Id> cleaned;
      cleaned.reserve(f.surveyed_jump_points.size());
      for (Id jid : f.surveyed_jump_points) {
        if (jid == kInvalidId || !has_jump(jid)) {
          note(join("Fix: Faction ", id_u64(fid), " removed invalid surveyed_jump_point ", id_u64(jid)));
          continue;
        }
        cleaned.push_back(jid);
      }
      sort_unique(cleaned);
      f.surveyed_jump_points = std::move(cleaned);
    }

    // contacts: drop entries for missing ships; fix ids + system_id.
    for (auto it = f.ship_contacts.begin(); it != f.ship_contacts.end();) {
      const Id sid = it->first;
      Contact& c = it->second;

      if (!has_ship(sid)) {
        note(join("Fix: Faction ", id_u64(fid), " removed contact for missing ship ", id_u64(sid)));
        it = f.ship_contacts.erase(it);
        continue;
      }
      if (c.ship_id != sid) {
        note(join("Fix: Faction ", id_u64(fid), " contact.ship_id set to key ", id_u64(sid)));
        c.ship_id = sid;
      }
      if (c.system_id == kInvalidId || !has_system(c.system_id)) {
        const Id sys = s.ships.at(sid).system_id;
        const Id fixed = (has_system(sys)) ? sys : fallback_system;
        note(join("Fix: Faction ", id_u64(fid), " contact.system_id set to ", id_u64(fixed)));
        c.system_id = fixed;
      }
      if (!std::isfinite(c.last_seen_position_uncertainty_mkm) || c.last_seen_position_uncertainty_mkm < 0.0) {
        note(join("Fix: Faction ", id_u64(fid), " contact.last_seen_position_uncertainty_mkm clamped ",
                  c.last_seen_position_uncertainty_mkm, " -> 0"));
        c.last_seen_position_uncertainty_mkm = 0.0;
      }

      // Contact track hygiene: prev_seen_day uses -1 as "unset".
      if (c.last_seen_day < 0) {
        note(join("Fix: Faction ", id_u64(fid), " contact.last_seen_day clamped ", c.last_seen_day, " -> 0"));
        c.last_seen_day = 0;
      }

      if (c.prev_seen_day < -1) {
        note(join("Fix: Faction ", id_u64(fid), " contact.prev_seen_day clamped ", c.prev_seen_day, " -> -1"));
        c.prev_seen_day = -1;
        c.prev_seen_position_mkm = Vec2{0.0, 0.0};
      }

      // If prev_seen_day is present, it must be strictly older than last_seen_day.
      if (c.prev_seen_day >= 0 && c.prev_seen_day >= c.last_seen_day) {
        note(join("Fix: Faction ", id_u64(fid), " contact.prev_seen_day reset (", c.prev_seen_day,
                  " >= last_seen_day ", c.last_seen_day, ")"));
        c.prev_seen_day = -1;
        c.prev_seen_position_mkm = Vec2{0.0, 0.0};
      }

      if (!std::isfinite(c.prev_seen_position_mkm.x) || !std::isfinite(c.prev_seen_position_mkm.y)) {
        note(join("Fix: Faction ", id_u64(fid), " contact.prev_seen_position_mkm sanitized"));
        c.prev_seen_position_mkm = Vec2{0.0, 0.0};
      }

      ++it;
    }

    if (content) {
      if (!f.active_research_id.empty() && !tech_exists(*content, f.active_research_id)) {
        note(join("Fix: Faction ", id_u64(fid), " cleared unknown active_research_id '", f.active_research_id, "'"));
        f.active_research_id.clear();
        f.active_research_progress = 0.0;
      }

      auto prune_string_list = [&](std::vector<std::string>& v, const char* what,
                                   auto exists_fn) {
        std::vector<std::string> out;
        out.reserve(v.size());
        for (const auto& x : v) {
          if (x.empty() || !exists_fn(x)) {
            note(join("Fix: Faction ", id_u64(fid), " removed unknown ", what, " '", x, "'"));
            continue;
          }
          out.push_back(x);
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        v = std::move(out);
      };

      prune_string_list(f.research_queue, "research tech", [&](const std::string& x) { return tech_exists(*content, x); });
      prune_string_list(f.known_techs, "known tech", [&](const std::string& x) { return tech_exists(*content, x); });
      prune_string_list(f.unlocked_components, "component", [&](const std::string& x) { return component_exists(*content, x); });
      prune_string_list(f.unlocked_installations, "installation", [&](const std::string& x) { return installation_exists(*content, x); });

      // Reverse engineering progress: drop unknown components, non-finite values,
      // and entries that are already unlocked.
      for (auto it = f.reverse_engineering_progress.begin(); it != f.reverse_engineering_progress.end();) {
        const std::string& cid = it->first;
        const double pts = it->second;

        if (cid.empty() || !component_exists(*content, cid)) {
          note(join("Fix: Faction ", id_u64(fid), " removed reverse engineering progress for unknown component '", cid, "'"));
          it = f.reverse_engineering_progress.erase(it);
          continue;
        }
        if (!std::isfinite(pts) || pts <= 0.0) {
          note(join("Fix: Faction ", id_u64(fid), " removed invalid reverse engineering progress for '", cid, "': ", pts));
          it = f.reverse_engineering_progress.erase(it);
          continue;
        }
        if (std::find(f.unlocked_components.begin(), f.unlocked_components.end(), cid) != f.unlocked_components.end()) {
          note(join("Fix: Faction ", id_u64(fid), " dropped reverse engineering progress for already-unlocked component '", cid, "'"));
          it = f.reverse_engineering_progress.erase(it);
          continue;
        }
        ++it;
      }
    }
  }

  // --- Events ---
  {
    std::uint64_t max_seq = 0;
    for (const auto& e : s.events) {
      if (e.seq > max_seq) max_seq = e.seq;
    }

    std::unordered_set<std::uint64_t> seen;
    seen.reserve(s.events.size() * 2);

    std::uint64_t next_seq = std::max<std::uint64_t>(1, std::max<std::uint64_t>(s.next_event_seq, max_seq + 1));

    for (std::size_t i = 0; i < s.events.size(); ++i) {
      auto& e = s.events[i];
      if (e.seq == 0 || !seen.insert(e.seq).second) {
        const std::uint64_t old = e.seq;
        e.seq = next_seq++;
        note(join("Fix: events[", i, "] seq reassigned ", old, " -> ", e.seq));
      }
      if (e.seq > max_seq) max_seq = e.seq;

      if (e.faction_id != kInvalidId && !has_faction(e.faction_id)) {
        note(join("Fix: events[", i, "] cleared missing faction_id ", id_u64(e.faction_id)));
        e.faction_id = kInvalidId;
      }
      if (e.faction_id2 != kInvalidId && !has_faction(e.faction_id2)) {
        note(join("Fix: events[", i, "] cleared missing faction_id2 ", id_u64(e.faction_id2)));
        e.faction_id2 = kInvalidId;
      }
      if (e.system_id != kInvalidId && !has_system(e.system_id)) {
        note(join("Fix: events[", i, "] cleared missing system_id ", id_u64(e.system_id)));
        e.system_id = kInvalidId;
      }
      if (e.ship_id != kInvalidId && !has_ship(e.ship_id)) {
        note(join("Fix: events[", i, "] cleared missing ship_id ", id_u64(e.ship_id)));
        e.ship_id = kInvalidId;
      }
      if (e.colony_id != kInvalidId && !has_colony(e.colony_id)) {
        note(join("Fix: events[", i, "] cleared missing colony_id ", id_u64(e.colony_id)));
        e.colony_id = kInvalidId;
      }
    }

    if (s.next_event_seq == 0 || s.next_event_seq <= max_seq) {
      const std::uint64_t old = s.next_event_seq;
      s.next_event_seq = max_seq + 1;
      note(join("Fix: next_event_seq set ", old, " -> ", s.next_event_seq));
    }
  }

  // --- Rebuild system indices (bodies/ships/jump_points) ---
  for (auto& [sys_id, sys] : s.systems) {
    sys.bodies.clear();
    sys.ships.clear();
    sys.jump_points.clear();
  }
  for (const auto& [bid, b] : s.bodies) {
    if (b.system_id != kInvalidId && has_system(b.system_id)) {
      s.systems.at(b.system_id).bodies.push_back(bid);
    }
  }
  for (const auto& [sid, sh] : s.ships) {
    if (sh.system_id != kInvalidId && has_system(sh.system_id)) {
      s.systems.at(sh.system_id).ships.push_back(sid);
    }
  }
  for (const auto& [jid, jp] : s.jump_points) {
    if (jp.system_id != kInvalidId && has_system(jp.system_id)) {
      s.systems.at(jp.system_id).jump_points.push_back(jid);
    }
  }
  for (auto& [sys_id, sys] : s.systems) {
    sort_unique(sys.bodies);
    sort_unique(sys.ships);
    sort_unique(sys.jump_points);
  }

  // --- Fix next_id ---
  Id max_id = 0;
  auto bump_max = [&](Id id) {
    if (id != kInvalidId && id > max_id) max_id = id;
  };
  for (const auto& [id, _] : s.systems) bump_max(id);
  for (const auto& [id, _] : s.bodies) bump_max(id);
  for (const auto& [id, _] : s.jump_points) bump_max(id);
  for (const auto& [id, _] : s.ships) bump_max(id);
  for (const auto& [id, _] : s.wrecks) bump_max(id);
  for (const auto& [id, _] : s.colonies) bump_max(id);
  for (const auto& [id, _] : s.factions) bump_max(id);
  for (const auto& [id, _] : s.fleets) bump_max(id);

  if (s.next_id == kInvalidId || s.next_id <= max_id) {
    const Id old = s.next_id;
    s.next_id = max_id + 1;
    note(join("Fix: next_id set ", id_u64(old), " -> ", id_u64(s.next_id)));
  }

  return report;
}

} // namespace nebula4x
