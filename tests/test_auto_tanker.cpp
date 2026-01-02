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

int test_auto_tanker() {
  using namespace nebula4x;

  ContentDB content;
  {
    ShipDesign tanker;
    tanker.id = "tanker";
    tanker.name = "Tanker";
    tanker.role = ShipRole::Freighter;
    tanker.mass_tons = 200.0;
    tanker.speed_km_s = 1000.0;
    tanker.fuel_capacity_tons = 1000.0;
    tanker.fuel_use_per_mkm = 0.0;
    content.designs[tanker.id] = tanker;

    ShipDesign scout;
    scout.id = "scout";
    scout.name = "Scout";
    scout.role = ShipRole::Surveyor;
    scout.mass_tons = 50.0;
    scout.speed_km_s = 1200.0;
    scout.fuel_capacity_tons = 200.0;
    scout.fuel_use_per_mkm = 0.0;
    content.designs[scout.id] = scout;
  }

  SimConfig cfg;
  cfg.auto_tanker_request_threshold_fraction = 0.25;
  cfg.auto_tanker_fill_target_fraction = 0.90;
  cfg.auto_tanker_min_transfer_tons = 1.0;

  Simulation sim(std::move(content), cfg);

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
  sys.bodies = {10};
  st.systems[1] = sys;

  // Body
  Body body;
  body.id = 10;
  body.name = "Anchor";
  body.type = BodyType::Planet;
  body.system_id = 1;
  body.orbit_radius_mkm = 0.0;
  body.orbit_period_days = 1.0;
  body.orbit_phase_radians = 0.0;
  body.position_mkm = Vec2{0.0, 0.0};
  st.bodies[10] = body;

  // Faction
  Faction fac;
  fac.id = 1;
  fac.name = "Player";
  fac.control = FactionControl::Player;
  fac.discovered_systems.push_back(1);
  st.factions[1] = fac;

  // Tanker ship
  Ship t;
  t.id = 200;
  t.name = "Tanker-1";
  t.faction_id = 1;
  t.system_id = 1;
  t.position_mkm = Vec2{0.0, 0.0};
  t.design_id = "tanker";
  t.fuel_tons = 600.0;
  t.auto_tanker = true;
  t.auto_tanker_reserve_fraction = 0.50;  // keep 500t in reserve
  st.ships[t.id] = t;

  // Stranded scout (auto-refuel disabled; low fuel)
  Ship s;
  s.id = 201;
  s.name = "Scout-1";
  s.faction_id = 1;
  s.system_id = 1;
  s.position_mkm = Vec2{1.0, 0.0};  // within default docking range
  s.design_id = "scout";
  s.fuel_tons = 0.0;
  s.auto_refuel = false;
  st.ships[s.id] = s;

  sim.load_game(st);

  // Day 1: tanker should transfer 100 tons (600 - 500 reserve) to the scout.
  sim.advance_days(1);

  const GameState& out = sim.state();
  const Ship& tanker_out = out.ships.at(200);
  const Ship& scout_out = out.ships.at(201);

  N4X_ASSERT(tanker_out.fuel_tons >= 499.999);
  N4X_ASSERT(tanker_out.fuel_tons <= 500.001);

  N4X_ASSERT(scout_out.fuel_tons >= 99.999);
  N4X_ASSERT(scout_out.fuel_tons <= 100.001);

  return 0;
}
