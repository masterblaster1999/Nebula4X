#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "nebula4x/core/date.h"
#include "nebula4x/core/entities.h"
#include "nebula4x/core/orders.h"

namespace nebula4x {

struct ContentDB {
  std::unordered_map<std::string, ShipDesign> designs;
  std::unordered_map<std::string, InstallationDef> installations;
};

struct GameState {
  int save_version{1};
  Date date;

  Id next_id{1};

  std::unordered_map<Id, StarSystem> systems;
  std::unordered_map<Id, Body> bodies;
  std::unordered_map<Id, Ship> ships;
  std::unordered_map<Id, Colony> colonies;
  std::unordered_map<Id, Faction> factions;

  std::unordered_map<Id, ShipOrders> ship_orders;

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
