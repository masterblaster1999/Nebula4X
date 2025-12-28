#include "nebula4x/core/state_validation.h"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <type_traits>
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
  }

  // --- Colonies ---
  for (const auto& [cid, c] : s.colonies) {
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

} // namespace nebula4x
