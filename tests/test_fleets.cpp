#include <algorithm>
#include <cmath>
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

nebula4x::GameState make_jump_state() {
  using namespace nebula4x;

  GameState s;
  s.save_version = 13;
  s.date = Date::from_ymd(2200, 1, 1);
  s.next_id = 2000;

  // One faction.
  {
    Faction f;
    f.id = 1;
    f.name = "Terrans";
    s.factions[f.id] = f;
  }

  // Two star systems.
  {
    StarSystem a;
    a.id = 10;
    a.name = "Alpha";
    s.systems[a.id] = a;
  }
  {
    StarSystem b;
    b.id = 20;
    b.name = "Beta";
    s.systems[b.id] = b;
  }

  // Connected jump points.
  {
    JumpPoint jp;
    jp.id = 100;
    jp.name = "JP-Alpha";
    jp.system_id = 10;
    jp.position_mkm = Vec2{0.0, 0.0};
    jp.linked_jump_id = 101;
    s.jump_points[jp.id] = jp;
    s.systems[jp.system_id].jump_points.push_back(jp.id);
  }
  {
    JumpPoint jp;
    jp.id = 101;
    jp.name = "JP-Beta";
    jp.system_id = 20;
    jp.position_mkm = Vec2{0.0, 0.0};
    jp.linked_jump_id = 100;
    s.jump_points[jp.id] = jp;
    s.systems[jp.system_id].jump_points.push_back(jp.id);
  }

  // Two ships in the same system but at different distances from the jump point.
  {
    Ship sh;
    sh.id = 1000;
    sh.name = "Fast";
    sh.faction_id = 1;
    sh.system_id = 10;
    sh.design_id = "fast";
    sh.position_mkm = Vec2{0.0, 0.0}; // already at the jump point
    s.ships[sh.id] = sh;
    s.systems[sh.system_id].ships.push_back(sh.id);
  }
  {
    Ship sh;
    sh.id = 1001;
    sh.name = "Slow";
    sh.faction_id = 1;
    sh.system_id = 10;
    sh.design_id = "slow";
    sh.position_mkm = Vec2{17.28, 0.0}; // 2 days @ 100 km/s (8.64 mkm/day)
    s.ships[sh.id] = sh;
    s.systems[sh.system_id].ships.push_back(sh.id);
  }

  s.selected_system = 10;
  return s;
}

nebula4x::GameState make_formation_state() {
  using namespace nebula4x;

  GameState s;
  s.save_version = GameState{}.save_version;
  s.date = Date::from_ymd(2200, 1, 1);
  s.next_id = 9000;

  // One faction.
  {
    Faction f;
    f.id = 1;
    f.name = "Terrans";
    s.factions[f.id] = f;
  }

  // One star system.
  {
    StarSystem sys;
    sys.id = 10;
    sys.name = "TestSys";
    s.systems[sys.id] = sys;
  }

  // Three ships in the same system.
  for (Id sid : std::vector<Id>{1000, 1001, 1002}) {
    Ship sh;
    sh.id = sid;
    sh.name = std::string("Ship-") + std::to_string(static_cast<unsigned long long>(sid));
    sh.faction_id = 1;
    sh.system_id = 10;
    sh.design_id = "fast";
    sh.position_mkm = Vec2{0.0, 0.0};
    s.ships[sh.id] = sh;
    s.systems[sh.system_id].ships.push_back(sh.id);
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

    // Newer saves should persist optional formation settings.
    N4X_ASSERT(sim.configure_fleet_formation(fid, FleetFormation::Wedge, 2.5));

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

    N4X_ASSERT(fl.formation == FleetFormation::Wedge);
    N4X_ASSERT(std::fabs(fl.formation_spacing_mkm - 2.5) < 1e-9);
  }

  // --- Coordinated fleet jump transit ---
  {
    ContentDB content;

    // Two simple designs with different speeds.
    {
      ShipDesign d;
      d.id = "fast";
      d.name = "Fast";
      d.speed_km_s = 200.0;
      d.max_hp = 10.0;
      content.designs[d.id] = d;
    }
    {
      ShipDesign d;
      d.id = "slow";
      d.name = "Slow";
      d.speed_km_s = 100.0;
      d.max_hp = 10.0;
      content.designs[d.id] = d;
    }

    Simulation sim(content, SimConfig{});
    sim.load_game(make_jump_state());

    std::string err;
    const Id fid = sim.create_fleet(1, "JumpFleet", std::vector<Id>{1000, 1001}, &err);
    N4X_ASSERT(fid != kInvalidId);
    N4X_ASSERT(err.empty());

    // Issue a fleet jump. The leader is already at the jump point.
    N4X_ASSERT(sim.issue_fleet_travel_via_jump(fid, 100));

    // Day 1: leader should *not* jump yet, because the slower ship hasn't arrived.
    sim.advance_days(1);
    N4X_ASSERT(sim.state().ships.at(1000).system_id == 10);
    N4X_ASSERT(sim.state().ships.at(1001).system_id == 10);
    N4X_ASSERT(!sim.state().ship_orders.at(1000).queue.empty());
    N4X_ASSERT(std::holds_alternative<TravelViaJump>(sim.state().ship_orders.at(1000).queue.front()));
    N4X_ASSERT(std::fabs(sim.state().ships.at(1001).position_mkm.x - 8.64) < 1e-6);

    // Day 2: slower ship arrives; still no jump until the following day.
    sim.advance_days(1);
    N4X_ASSERT(sim.state().ships.at(1000).system_id == 10);
    N4X_ASSERT(sim.state().ships.at(1001).system_id == 10);
    N4X_ASSERT(std::fabs(sim.state().ships.at(1001).position_mkm.x) < 1e-6);
    N4X_ASSERT(!sim.state().ship_orders.at(1000).queue.empty());
    N4X_ASSERT(std::holds_alternative<TravelViaJump>(sim.state().ship_orders.at(1000).queue.front()));

    // Day 3: both ships are at the jump point at the start of the tick, so they
    // should transit together.
    sim.advance_days(1);
    N4X_ASSERT(sim.state().ships.at(1000).system_id == 20);
    N4X_ASSERT(sim.state().ships.at(1001).system_id == 20);
    N4X_ASSERT(sim.state().ship_orders.at(1000).queue.empty());
    N4X_ASSERT(sim.state().ship_orders.at(1001).queue.empty());
  }

  // --- Fleet formations (move-to-point) ---
  {
    ContentDB content;

    // A very fast design so ships snap to targets in a single day.
    {
      ShipDesign d;
      d.id = "fast";
      d.name = "Fast";
      d.speed_km_s = 10000.0;
      d.max_hp = 10.0;
      content.designs[d.id] = d;
    }

    Simulation sim(content, SimConfig{});
    sim.load_game(make_formation_state());

    std::string err;
    const Id fid = sim.create_fleet(1, "FormFleet", std::vector<Id>{1000, 1001, 1002}, &err);
    N4X_ASSERT(fid != kInvalidId);
    N4X_ASSERT(err.empty());

    N4X_ASSERT(sim.configure_fleet_formation(fid, FleetFormation::LineAbreast, 2.0));
    N4X_ASSERT(sim.issue_fleet_move_to_point(fid, Vec2{100.0, 0.0}));

    sim.advance_days(1);

    const auto& a = sim.state().ships.at(1000);
    const auto& b = sim.state().ships.at(1001);
    const auto& c = sim.state().ships.at(1002);

    N4X_ASSERT(std::fabs(a.position_mkm.x - 100.0) < 1e-9);
    N4X_ASSERT(std::fabs(a.position_mkm.y - 0.0) < 1e-9);

    N4X_ASSERT(std::fabs(b.position_mkm.x - 100.0) < 1e-9);
    N4X_ASSERT(std::fabs(c.position_mkm.x - 100.0) < 1e-9);

    // With forward=(1,0) and right=(0,1), line-abreast offsets should be in +/-Y.
    N4X_ASSERT(std::fabs(std::fabs(b.position_mkm.y) - 2.0) < 1e-9);
    N4X_ASSERT(std::fabs(std::fabs(c.position_mkm.y) - 2.0) < 1e-9);
    N4X_ASSERT(b.position_mkm.y * c.position_mkm.y < 0.0);
  }

  return 0;
}
