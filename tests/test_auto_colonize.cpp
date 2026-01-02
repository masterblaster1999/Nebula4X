#include "test.h"

#include "nebula4x/core/simulation.h"

#include <cmath>
#include <iostream>

using namespace nebula4x;

#define N4X_ASSERT(cond, msg)                                                                                          \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      std::cerr << "ASSERT FAILED: " << (msg) << " (" << __FILE__ << ":" << __LINE__ << ")\n";             \
      return 1;                                                                                                        \
    }                                                                                                                  \
  } while (0)

int test_auto_colonize() {
  // Minimal content: only a single colony ship design is required for this test.
  ContentDB content;

  ShipDesign colonizer;
  colonizer.id = "colony_ship";
  colonizer.name = "Colony Ship";
  colonizer.max_hp = 1000.0;
  colonizer.fuel_capacity_tons = 1000.0;
  colonizer.cargo_tons = 0.0;
  colonizer.speed_km_s = 10.0;  // slow enough that colonization won't complete in 1 day
  colonizer.colony_capacity_millions = 50.0;
  content.designs[colonizer.id] = colonizer;

  SimConfig cfg;
  // Keep the test simple: treat all bodies as habitable so scoring doesn't depend on atmosphere/temp.
  cfg.enable_habitability = false;
  Simulation sim(content, cfg);

  GameState st;
  st.save_version = 36;
  st.next_id = 5000;  // ensure no collisions when the colonization creates a new colony.

  Faction f;
  f.id = 1;
  f.name = "Terrans";
  st.factions[f.id] = f;

  StarSystem sys;
  sys.id = 1;
  sys.name = "Test System";
  st.systems[sys.id] = sys;

  // Home body (already colonized).
  Body home;
  home.id = 10;
  home.name = "Home";
  home.type = BodyType::Planet;
  home.system_id = sys.id;
  home.orbit_radius_mkm = 0.0;
  home.orbit_period_days = 1.0;
  st.bodies[home.id] = home;

  // Target body: uncolonized, nearby but not within docking range.
  Body target;
  target.id = 11;
  target.name = "Target";
  target.type = BodyType::Planet;
  target.system_id = sys.id;
  target.orbit_radius_mkm = 10.0;
  target.orbit_period_days = 1.0e9;  // essentially static for this test
  target.orbit_phase_radians = 0.0;
  target.mineral_deposits["Duranium"] = 100000.0;
  st.bodies[target.id] = target;

  Colony c;
  c.id = 100;
  c.name = "Home Colony";
  c.faction_id = f.id;
  c.body_id = home.id;
  c.population_millions = 1000.0;
  st.colonies[c.id] = c;

  Ship sh;
  sh.id = 1000;
  sh.name = "Colony Ship";
  sh.faction_id = f.id;
  sh.system_id = sys.id;
  sh.position_mkm = {0.0, 0.0};
  sh.design_id = colonizer.id;
  sh.auto_colonize = true;
  st.ships[sh.id] = sh;

  sim.load_game(st);

  // After 1 day, auto-colonize should have queued a ColonizeBody order, but the ship
  // should not have reached the target yet.
  sim.advance_days(1);
  {
    const auto& s = sim.state();
    N4X_ASSERT(s.ships.contains(sh.id), "colony ship should still exist after 1 day");
    N4X_ASSERT(s.ship_orders.contains(sh.id), "ship should have an orders entry");
    const auto& q = s.ship_orders.at(sh.id).queue;
    N4X_ASSERT(!q.empty(), "auto-colonize should queue an order");
    const auto* ord = std::get_if<ColonizeBody>(&q.front());
    N4X_ASSERT(ord != nullptr, "first order should be ColonizeBody");
    N4X_ASSERT(ord->body_id == target.id, "auto-colonize should target the uncolonized body");
  }

  // Eventually the ship should reach the target, colonize it, and be consumed.
  sim.advance_days(30);
  {
    const auto& s = sim.state();
    N4X_ASSERT(!s.ships.contains(sh.id), "colony ship should be consumed by colonization");
    bool found = false;
    for (const auto& [_, col] : s.colonies) {
      if (col.body_id == target.id && col.faction_id == f.id) {
        found = true;
        break;
      }
    }
    N4X_ASSERT(found, "a new colony should exist on the target body");
  }

  return 0;
}
