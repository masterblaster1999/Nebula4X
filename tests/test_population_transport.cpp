#include "test.h"

#include "nebula4x/core/simulation.h"

#include <cmath>
#include <iostream>
#include <variant>

using namespace nebula4x;

#define N4X_ASSERT(cond, msg)                                                                                          \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      std::cerr << "ASSERT FAILED: " << (msg) << " (" << __FILE__ << ":" << __LINE__ << ")\n";                   \
      return 1;                                                                                                        \
    }                                                                                                                  \
  } while (0)

int test_population_transport() {
  // Content: just the designs we reference.
  ContentDB content;

  ShipDesign transport;
  transport.id = "colony_transport";
  transport.name = "Colony Transport";
  transport.max_hp = 1000.0;
  transport.fuel_capacity_tons = 1000.0;
  transport.cargo_tons = 0.0;
  transport.speed_km_s = 2000.0;
  transport.colony_capacity_millions = 50.0;
  content.designs[transport.id] = transport;

  ShipDesign freighter;
  freighter.id = "freighter_alpha";
  freighter.name = "Freighter";
  freighter.max_hp = 1000.0;
  freighter.fuel_capacity_tons = 1000.0;
  freighter.cargo_tons = 1000.0;
  freighter.speed_km_s = 2000.0;
  content.designs[freighter.id] = freighter;

  SimConfig cfg;
  // Slow down population transfers so we can test throughput-limited behavior
  // under sub-day (hourly) ticks.
  cfg.colonist_transfer_millions_per_day_per_colony_cap = 0.2; // 50M cap => 10M/day
  cfg.colonist_transfer_millions_per_day_min = 1.0;
  Simulation sim(content, cfg);

  // Custom minimal state (single system, two bodies/colonies, two ships).
  GameState st;
  st.save_version = 36;

  Faction f;
  f.id = 1;
  f.name = "Terrans";
  st.factions[f.id] = f;

  StarSystem sys;
  sys.id = 1;
  sys.name = "Test System";
  st.systems[sys.id] = sys;

  Body b1;
  b1.id = 10;
  b1.name = "Body A";
  b1.system_id = sys.id;
  b1.orbit_radius_mkm = 0.0;
  b1.orbit_period_days = 1.0;
  st.bodies[b1.id] = b1;

  Body b2;
  b2.id = 11;
  b2.name = "Body B";
  b2.system_id = sys.id;
  b2.orbit_radius_mkm = 0.0;
  b2.orbit_period_days = 1.0;
  st.bodies[b2.id] = b2;

  Colony src;
  src.id = 100;
  src.name = "Source Colony";
  src.faction_id = f.id;
  src.body_id = b1.id;
  src.population_millions = 100.0;
  st.colonies[src.id] = src;

  Colony dst;
  dst.id = 101;
  dst.name = "Dest Colony";
  dst.faction_id = f.id;
  dst.body_id = b2.id;
  dst.population_millions = 0.0;
  st.colonies[dst.id] = dst;

  Ship sh;
  sh.id = 1000;
  sh.name = "Transport";
  sh.faction_id = f.id;
  sh.system_id = sys.id;
  sh.position_mkm = {0.0, 0.0};
  sh.design_id = transport.id;
  st.ships[sh.id] = sh;

  Ship sh2;
  sh2.id = 1001;
  sh2.name = "NoCap";
  sh2.faction_id = f.id;
  sh2.system_id = sys.id;
  sh2.position_mkm = {0.0, 0.0};
  sh2.design_id = freighter.id;
  st.ships[sh2.id] = sh2;

  sim.load_game(st);

  // Load explicit 10M (throughput-limited: 10M/day => 5M per 12h).
  N4X_ASSERT(sim.issue_load_colonists(sh.id, src.id, 10.0), "issue_load_colonists should succeed");

  // Half a day: should only load 5M and keep the order queued.
  sim.advance_hours(12);
  {
    const auto& s = sim.state();
    const auto& ship = s.ships.at(sh.id);
    const auto& csrc = s.colonies.at(src.id);
    N4X_ASSERT(std::abs(ship.colonists_millions - 5.0) < 1e-6, "ship should have 5M embarked after 12h");
    N4X_ASSERT(std::abs(csrc.population_millions - 95.0) < 1e-6, "source colony should drop to 95M after 12h");
    N4X_ASSERT(!s.ship_orders.at(sh.id).queue.empty(), "load order should still be in progress");

    const auto& ord = s.ship_orders.at(sh.id).queue.front();
    N4X_ASSERT(std::holds_alternative<LoadColonists>(ord), "front order should still be LoadColonists");
    const double remaining = std::get<LoadColonists>(ord).millions;
    N4X_ASSERT(std::abs(remaining - 5.0) < 1e-6, "remaining load should be 5M after 12h");
  }

  // Another 12 hours: should finish the remaining 5M.
  sim.advance_hours(12);
  {
    const auto& s = sim.state();
    const auto& ship = s.ships.at(sh.id);
    const auto& csrc = s.colonies.at(src.id);
    N4X_ASSERT(std::abs(ship.colonists_millions - 10.0) < 1e-6, "ship should have 10M embarked after 24h");
    N4X_ASSERT(std::abs(csrc.population_millions - 90.0) < 1e-6, "source colony should drop to 90M after 24h");
    N4X_ASSERT(s.ship_orders.at(sh.id).queue.empty(), "load order should complete");
  }

  // Load max (0) should fill remaining capacity (50M cap => +40M). At 10M/day this takes 4 days.
  N4X_ASSERT(sim.issue_load_colonists(sh.id, src.id, 0.0), "issue_load_colonists(max) should succeed");
  sim.advance_days(4);
  {
    const auto& s = sim.state();
    const auto& ship = s.ships.at(sh.id);
    const auto& csrc = s.colonies.at(src.id);
    N4X_ASSERT(std::abs(ship.colonists_millions - 50.0) < 1e-6, "ship should fill to 50M capacity");
    N4X_ASSERT(std::abs(csrc.population_millions - 50.0) < 1e-6, "source colony should now have 50M");
    N4X_ASSERT(s.ship_orders.at(sh.id).queue.empty(), "load(max) order should complete");
  }

  // Unload max (0) to destination colony. 50M at 10M/day => 5 days.
  N4X_ASSERT(sim.issue_unload_colonists(sh.id, dst.id, 0.0), "issue_unload_colonists(max) should succeed");
  sim.advance_days(5);
  {
    const auto& s = sim.state();
    const auto& ship = s.ships.at(sh.id);
    const auto& cdst = s.colonies.at(dst.id);
    N4X_ASSERT(std::abs(ship.colonists_millions) < 1e-6, "ship should have unloaded all colonists");
    N4X_ASSERT(std::abs(cdst.population_millions - 50.0) < 1e-6, "dest colony should receive 50M");
    N4X_ASSERT(s.ship_orders.at(sh.id).queue.empty(), "unload order should complete");
  }

  // Ships without colony modules should not be able to move population.
  N4X_ASSERT(!sim.issue_load_colonists(sh2.id, src.id, 1.0), "no-cap ship should reject LoadColonists");
  N4X_ASSERT(!sim.issue_unload_colonists(sh2.id, dst.id, 1.0), "no-cap ship should reject UnloadColonists");

  return 0;
}
