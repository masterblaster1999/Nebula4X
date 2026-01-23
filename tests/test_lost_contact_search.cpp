#include "test.h"

#include "nebula4x/core/simulation.h"

#include <cmath>
#include <iostream>
#include <variant>

using namespace nebula4x;

#define N4X_ASSERT(cond, msg)                                                                                          \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      std::cerr << "ASSERT FAILED: " << (msg) << " (" << __FILE__ << ":" << __LINE__ << ")\n";              \
      return 1;                                                                                                        \
    }                                                                                                                  \
  } while (0)

int test_lost_contact_search() {
  ContentDB content;

  ShipDesign hunter;
  hunter.id = "hunter";
  hunter.name = "Hunter";
  hunter.max_hp = 1000.0;
  hunter.speed_km_s = 10.0; // slow enough that waypoints take multiple days
  hunter.sensor_range_mkm = 0.0; // ensure the target stays undetected
  content.designs[hunter.id] = hunter;

  ShipDesign target_design;
  target_design.id = "target";
  target_design.name = "Target";
  target_design.max_hp = 1000.0;
  target_design.speed_km_s = 50.0;
  content.designs[target_design.id] = target_design;

  SimConfig cfg;
  cfg.contact_search_offset_fraction = 1.0;
  cfg.contact_search_pattern_points = 64;
  // Keep uncertainty stable for deterministic assertions.
  cfg.contact_uncertainty_growth_fraction_of_speed = 0.0;
  cfg.contact_uncertainty_growth_min_mkm_per_day = 0.0;
  cfg.contact_prediction_max_days = 30;

  Simulation sim(content, cfg);

  GameState st;
  st.save_version = 36;

  Faction fa;
  fa.id = 1;
  fa.name = "Hunters";
  st.factions[fa.id] = fa;

  Faction fb;
  fb.id = 2;
  fb.name = "Targets";
  st.factions[fb.id] = fb;

  StarSystem sys;
  sys.id = 1;
  sys.name = "Test System";
  st.systems[sys.id] = sys;

  Ship hunter_ship;
  hunter_ship.id = 100;
  hunter_ship.name = "H";
  hunter_ship.faction_id = fa.id;
  hunter_ship.system_id = sys.id;
  hunter_ship.position_mkm = {0.0, 0.0};
  hunter_ship.design_id = hunter.id;
  st.ships[hunter_ship.id] = hunter_ship;

  Ship target_ship;
  target_ship.id = 200;
  target_ship.name = "T";
  target_ship.faction_id = fb.id;
  target_ship.system_id = sys.id;
  target_ship.position_mkm = {1000.0, 0.0}; // actual position is irrelevant; it must remain undetected
  target_ship.design_id = target_design.id;
  st.ships[target_ship.id] = target_ship;

  // Seed an intel contact so AttackShip can be issued under fog-of-war.
  Contact c;
  c.ship_id = target_ship.id;
  c.system_id = sys.id;
  c.last_seen_day = 0;
  c.last_seen_position_mkm = {0.0, 0.0};
  c.last_seen_position_uncertainty_mkm = 100.0;
  c.last_seen_design_id = target_design.id;
  c.last_seen_faction_id = fb.id;
  st.factions[fa.id].ship_contacts[target_ship.id] = c;

  sim.load_game(st);

  N4X_ASSERT(sim.issue_attack_ship(hunter_ship.id, target_ship.id, /*restrict_to_discovered=*/false),
             "issue_attack_ship should succeed");

  // One day of pursuit should seed a persistent search waypoint offset.
  sim.advance_days(1);

  int idx1 = 0;
  bool has1 = false;
  Vec2 off1{0.0, 0.0};
  {
    const auto& s = sim.state();
    N4X_ASSERT(!s.ship_orders.at(hunter_ship.id).queue.empty(), "AttackShip order should exist");
    const auto& ord_any = s.ship_orders.at(hunter_ship.id).queue.front();
    N4X_ASSERT(std::holds_alternative<AttackShip>(ord_any), "front order should be AttackShip");
    const auto& ord = std::get<AttackShip>(ord_any);
    idx1 = ord.search_waypoint_index;
    has1 = ord.has_search_offset;
    off1 = ord.search_offset_mkm;
  }

  N4X_ASSERT(has1, "after one day, lost-contact search should have an active offset");
  N4X_ASSERT(idx1 == 1, "first search waypoint index should be 1");
  N4X_ASSERT(off1.length() > 1e-6, "search_offset_mkm should be non-zero");

  // Another day: because the ship is still en route, the search offset should *not*
  // be recomputed (no daily retargeting / zig-zag).
  sim.advance_days(1);

  {
    const auto& s = sim.state();
    const auto& ord_any = s.ship_orders.at(hunter_ship.id).queue.front();
    const auto& ord = std::get<AttackShip>(ord_any);

    N4X_ASSERT(ord.search_waypoint_index == idx1,
               "search_waypoint_index should not advance until the waypoint is reached");
    N4X_ASSERT(ord.has_search_offset == has1, "has_search_offset should remain stable");
    const double d = (ord.search_offset_mkm - off1).length();
    N4X_ASSERT(d < 1e-9, "search_offset_mkm should persist across days while en route");
  }

  return 0;
}
