#include <iostream>

#include "nebula4x/core/date.h"
#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(expr) \
  do { \
    if (!(expr)) { \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
      return 1; \
    } \
  } while (0)

int test_auto_mine() {
  using namespace nebula4x;

  ContentDB content;
  {
    ShipDesign miner;
    miner.id = "miner";
    miner.name = "Miner";
    miner.role = ShipRole::Freighter;
    miner.mass_tons = 100.0;
    miner.speed_km_s = 1000.0;
    miner.cargo_tons = 50.0;
    miner.mining_tons_per_day = 50.0;
    // No fuel required for this unit test.
    miner.fuel_capacity_tons = 0.0;
    miner.fuel_use_per_mkm = 0.0;
    content.designs[miner.id] = miner;
  }

  Simulation sim(std::move(content), SimConfig{});

  GameState st;
  st.save_version = 39;
  st.date = Date::from_ymd(2200, 1, 1);
  st.hour_of_day = 0;
  st.next_id = 1000;
  st.selected_system = 1;

  // System
  StarSystem sys;
  sys.id = 1;
  sys.name = "Test";
  sys.galaxy_pos = Vec2{0.0, 0.0};
  sys.bodies = {10, 11};
  st.systems[1] = sys;

  // Base body + colony.
  Body base;
  base.id = 10;
  base.name = "Base";
  base.type = BodyType::Planet;
  base.system_id = 1;
  base.orbit_radius_mkm = 0.0;
  base.orbit_period_days = 1.0;
  base.orbit_phase_radians = 0.0;
  base.position_mkm = Vec2{0.0, 0.0};
  // Ensure deposits are modeled but do not include Duranium, so the miner won't target Base.
  base.mineral_deposits["Sorium"] = 0.0;
  st.bodies[10] = base;

  // Mineable body.
  Body ast;
  ast.id = 11;
  ast.name = "Asteroid";
  ast.type = BodyType::Asteroid;
  ast.system_id = 1;
  ast.orbit_radius_mkm = 0.0;
  ast.orbit_period_days = 1.0;
  ast.orbit_phase_radians = 0.0;
  ast.position_mkm = Vec2{1.0, 0.0};
  ast.mineral_deposits["Duranium"] = 100.0;
  st.bodies[11] = ast;

  // Faction
  Faction fac;
  fac.id = 1;
  fac.name = "Player";
  fac.control = FactionControl::Player;
  fac.discovered_systems.push_back(1);
  st.factions[1] = fac;

  // Colony
  Colony col;
  col.id = 100;
  col.name = "Colony";
  col.faction_id = 1;
  col.body_id = 10;
  st.colonies[100] = col;

  // Auto-mine ship.
  Ship ship;
  ship.id = 200;
  ship.name = "Miner-1";
  ship.faction_id = 1;
  ship.system_id = 1;
  ship.position_mkm = Vec2{0.0, 0.0};
  ship.design_id = "miner";
  ship.auto_mine = true;
  ship.auto_mine_home_colony_id = 100;
  ship.auto_mine_mineral = "Duranium";
  st.ships[200] = ship;

  sim.load_game(st);

  // Day 1: auto-mine should mine Duranium into the ship's cargo.
  sim.advance_days(1);
  {
    const GameState& s = sim.state();
    const auto it_ship = s.ships.find(200);
    N4X_ASSERT(it_ship != s.ships.end());
    const Ship& sh = it_ship->second;
    N4X_ASSERT(sh.cargo.count("Duranium") != 0);
    N4X_ASSERT(sh.cargo.at("Duranium") > 1.0);
  }

  // Day 2: auto-mine should unload minerals to the configured home colony.
  sim.advance_days(1);
  {
    const GameState& s = sim.state();
    const auto it_col = s.colonies.find(100);
    N4X_ASSERT(it_col != s.colonies.end());
    const Colony& c = it_col->second;
    N4X_ASSERT(c.minerals.count("Duranium") != 0);
    N4X_ASSERT(c.minerals.at("Duranium") > 1.0);

    const auto it_ship = s.ships.find(200);
    N4X_ASSERT(it_ship != s.ships.end());
    const Ship& sh = it_ship->second;
    const double remaining = sh.cargo.count("Duranium") ? sh.cargo.at("Duranium") : 0.0;
    N4X_ASSERT(remaining <= 1e-6);
  }

  return 0;
}
