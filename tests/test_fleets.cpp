#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include "nebula4x/core/serialization.h"
#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";        \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

namespace {

nebula4x::GameState make_minimal_state() {
  using namespace nebula4x;

  GameState s;
  s.save_version = 13;
  s.date = Date::from_ymd(2200, 1, 1);
  s.next_id = 100;

  // Two factions.
  {
    Faction f;
    f.id = 1;
    f.name = "Terrans";
    s.factions[f.id] = f;
  }
  {
    Faction f;
    f.id = 2;
    f.name = "Martians";
    s.factions[f.id] = f;
  }

  // Minimal star system.
  {
    StarSystem sys;
    sys.id = 10;
    sys.name = "Sol";
    s.systems[sys.id] = sys;
  }

  // Three ships.
  {
    Ship a;
    a.id = 10;
    a.name = "A";
    a.faction_id = 1;
    a.system_id = 10;
    a.design_id = "";
    s.ships[a.id] = a;
    s.systems[a.system_id].ships.push_back(a.id);
  }
  {
    Ship b;
    b.id = 11;
    b.name = "B";
    b.faction_id = 1;
    b.system_id = 10;
    b.design_id = "";
    s.ships[b.id] = b;
    s.systems[b.system_id].ships.push_back(b.id);
  }
  {
    Ship c;
    c.id = 12;
    c.name = "C";
    c.faction_id = 2;
    c.system_id = 10;
    c.design_id = "";
    s.ships[c.id] = c;
    s.systems[c.system_id].ships.push_back(c.id);
  }

  s.selected_system = 10;
  return s;
}

} // namespace

int test_fleets() {
  using namespace nebula4x;

  ContentDB content;
  Simulation sim(content, SimConfig{});
  sim.load_game(make_minimal_state());

  // --- Create fleet ---
  {
    std::string err;
    const Id fid = sim.create_fleet(1, "1st Fleet", std::vector<Id>{10, 11}, &err);
    N4X_ASSERT(fid != kInvalidId);
    N4X_ASSERT(err.empty());
    N4X_ASSERT(sim.state().fleets.size() == 1);

    const Fleet& fl = sim.state().fleets.at(fid);
    N4X_ASSERT(fl.faction_id == 1);
    N4X_ASSERT(fl.name == "1st Fleet");
    N4X_ASSERT(fl.leader_ship_id == 10);
    N4X_ASSERT(fl.ship_ids.size() == 2);
    N4X_ASSERT(sim.fleet_for_ship(10) == fid);
    N4X_ASSERT(sim.fleet_for_ship(11) == fid);
    N4X_ASSERT(sim.fleet_for_ship(12) == kInvalidId);

    // Creating another fleet using a ship already in a fleet should fail.
    err.clear();
    const Id bad = sim.create_fleet(1, "Dup", std::vector<Id>{10}, &err);
    N4X_ASSERT(bad == kInvalidId);
    N4X_ASSERT(!err.empty());

    // Adding a ship of the wrong faction should fail.
    err.clear();
    N4X_ASSERT(!sim.add_ship_to_fleet(fid, 12, &err));
    N4X_ASSERT(!err.empty());

    // Leader switch.
    N4X_ASSERT(sim.set_fleet_leader(fid, 11));
    N4X_ASSERT(sim.state().fleets.at(fid).leader_ship_id == 11);

    // Bulk order issuing: wait 3 days.
    N4X_ASSERT(sim.issue_fleet_wait_days(fid, 3));
    N4X_ASSERT(sim.state().ship_orders.at(10).queue.size() == 1);
    N4X_ASSERT(sim.state().ship_orders.at(11).queue.size() == 1);
    N4X_ASSERT(std::holds_alternative<WaitDays>(sim.state().ship_orders.at(10).queue.front()));

    // Clear fleet orders should clear both.
    N4X_ASSERT(sim.clear_fleet_orders(fid));
    N4X_ASSERT(sim.state().ship_orders.at(10).queue.empty());
    N4X_ASSERT(sim.state().ship_orders.at(11).queue.empty());

    // Remove members: removing last member should auto-disband.
    N4X_ASSERT(sim.remove_ship_from_fleet(fid, 10));
    N4X_ASSERT(sim.fleet_for_ship(10) == kInvalidId);
    // fleet still exists with ship 11
    N4X_ASSERT(sim.state().fleets.count(fid) == 1);
    N4X_ASSERT(sim.remove_ship_from_fleet(fid, 11));
    N4X_ASSERT(sim.state().fleets.empty());
  }

  // --- Serialization roundtrip ---
  {
    std::string err;
    const Id fid = sim.create_fleet(1, "SerializeMe", std::vector<Id>{10, 11}, &err);
    N4X_ASSERT(fid != kInvalidId);

    const std::string json_text = serialize_game_to_json(sim.state());
    const GameState loaded = deserialize_game_from_json(json_text);

    N4X_ASSERT(loaded.fleets.size() == 1);
    const auto it = loaded.fleets.begin();
    const Fleet& fl = it->second;
    N4X_ASSERT(fl.name == "SerializeMe");
    N4X_ASSERT(fl.faction_id == 1);
    N4X_ASSERT(fl.ship_ids.size() == 2);
    N4X_ASSERT(std::find(fl.ship_ids.begin(), fl.ship_ids.end(), 10) != fl.ship_ids.end());
    N4X_ASSERT(std::find(fl.ship_ids.begin(), fl.ship_ids.end(), 11) != fl.ship_ids.end());
  }

  return 0;
}
