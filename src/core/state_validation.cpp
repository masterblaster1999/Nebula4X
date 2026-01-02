#include "nebula4x/core/state_validation.h"

#include <algorithm>
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

  if (s.next_id != kInvalidId && max_id != kInvalidId && s.next_id <= max_id) {
    push(errors,
         join("next_id is not monotonic: next_id=", id_u64(s.next_id), " max_existing_id=", id_u64(max_id)));
  }

  // --- selected system ---
  if (s.selected_system != kInvalidId && !has_system(s.selected_system)) {
    push(errors, join("selected_system references unknown system id ", id_u64(s.selected_system)));
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
              } else if constexpr (std::is_same_v<T, AttackShip>) {
                if (ord.target_ship_id == kInvalidId) {
                  push(errors, prefix() + "AttackShip has invalid target_ship_id");
                } else if (!has_ship(ord.target_ship_id)) {
                  push(errors,
                       prefix() + join("AttackShip references missing target_ship_id ", id_u64(ord.target_ship_id)));
                } else if (ord.target_ship_id == ship_id) {
                  push(errors, prefix() + "AttackShip targets itself");
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
            } else if constexpr (std::is_same_v<T, AttackShip>) {
              if (ord.target_ship_id == kInvalidId) {
                push(errors, prefix() + "AttackShip has invalid target_ship_id");
              } else if (!has_ship(ord.target_ship_id)) {
                push(errors,
                     prefix() + join("AttackShip references missing target_ship_id ", id_u64(ord.target_ship_id)));
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
          const auto& bo = c.shipyard_queue[i];
          if (bo.design_id.empty() || !design_exists(s, *content, bo.design_id)) {
            note(join("Fix: Colony ", id_u64(cid), " dropped shipyard_queue entry with unknown design_id '",
                      bo.design_id, "'"));
            c.shipyard_queue.erase(c.shipyard_queue.begin() + static_cast<long>(i));
            continue;
          }
          if (!std::isfinite(bo.tons_remaining) || bo.tons_remaining < 0.0) {
            const double old = bo.tons_remaining;
            c.shipyard_queue[i].tons_remaining = std::max(0.0, std::isfinite(old) ? old : 0.0);
            note(join("Fix: Colony ", id_u64(cid), " shipyard_queue tons_remaining clamped ", old, " -> ",
                      c.shipyard_queue[i].tons_remaining));
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
            } else if constexpr (std::is_same_v<T, AttackShip>) {
              if (ord.target_ship_id == kInvalidId || !has_ship(ord.target_ship_id) ||
                  (self_ship_id != kInvalidId && ord.target_ship_id == self_ship_id)) {
                keep = false;
                drop(i, "AttackShip invalid target_ship_id");
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
