#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "nebula4x/core/date.h"
#include "nebula4x/core/entities.h"
#include "nebula4x/core/orders.h"
#include "nebula4x/core/tech_tree.h"

namespace nebula4x {

// Static content loaded from JSON files.
struct ContentDB {
  std::unordered_map<std::string, ComponentDef> components;
  std::unordered_map<std::string, ShipDesign> designs;
  std::unordered_map<std::string, InstallationDef> installations;
  std::unordered_map<std::string, TechDef> techs;
};

// A single save-game state.
struct GameState {
  int save_version{13};
  Date date;

  Id next_id{1};

  // Monotonic id for SimEvent::seq.
  // Persisted so that clearing/pruning the event log does not reset the sequence.
  std::uint64_t next_event_seq{1};

  std::unordered_map<Id, StarSystem> systems;
  std::unordered_map<Id, Body> bodies;
  std::unordered_map<Id, JumpPoint> jump_points;
  std::unordered_map<Id, Ship> ships;
  std::unordered_map<Id, Colony> colonies;
  std::unordered_map<Id, Faction> factions;

  // Fleets are lightweight groupings of ships for convenience.
  std::unordered_map<Id, Fleet> fleets;

  // Player-created designs persisted in saves.
  std::unordered_map<std::string, ShipDesign> custom_designs;

  std::unordered_map<Id, ShipOrders> ship_orders;

  // Persistent simulation event log.
  // Events are appended during ticks and saved/loaded with the game.
  std::vector<SimEvent> events;

  // UI convenience: which system is selected.
  Id selected_system{kInvalidId};
};

Id allocate_id(GameState& s);

// Small helper for safe lookups.
template <typename Map>
auto* find_ptr(Map& m, const typename Map::key_type& k) {
  auto it = m.find(k);
  if (it == m.end()) return static_cast<decltype(&it->second)>(nullptr);
  return &it->second;
}

template <typename Map>
const auto* find_ptr(const Map& m, const typename Map::key_type& k) {
  auto it = m.find(k);
  if (it == m.end()) return static_cast<const decltype(&it->second)>(nullptr);
  return &it->second;
}

} // namespace nebula4x