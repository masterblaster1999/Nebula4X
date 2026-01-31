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

int test_auto_salvage() {
  using namespace nebula4x;

  ContentDB content;
  {
    ShipDesign salv;
    salv.id = "salvager";
    salv.name = "Salvager";
    salv.role = ShipRole::Freighter;
    salv.mass_tons = 100.0;
    salv.speed_km_s = 1000.0;
    salv.cargo_tons = 500.0;
    salv.fuel_capacity_tons = 50.0;
    salv.fuel_use_per_mkm = 0.0;
    content.designs[salv.id] = salv;
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
  sys.bodies = {10};
  st.systems[1] = sys;

  // Body (stationary for test)
  Body body;
  body.id = 10;
  body.name = "Base";
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

  // Colony
  Colony col;
  col.id = 100;
  col.name = "Colony";
  col.faction_id = 1;
  col.body_id = 10;
  st.colonies[100] = col;

  // Salvage ship (auto-salvage)
  Ship ship;
  ship.id = 200;
  ship.name = "Salvager-1";
  ship.faction_id = 1;
  ship.system_id = 1;
  ship.position_mkm = Vec2{0.0, 0.0};
  ship.design_id = "salvager";
  ship.auto_salvage = true;
  ship.fuel_tons = 50.0;
  st.ships[200] = ship;


  // A second auto-salvage ship with active repeating orders. The auto-salvage planner must not override it.
  Ship ship_repeat = ship;
  ship_repeat.id = 201;
  ship_repeat.name = "Salvager-Repeat";
  st.ships[201] = ship_repeat;

  ShipOrders so_repeat;
  so_repeat.repeat = true;
  so_repeat.repeat_count_remaining = -1;
  so_repeat.repeat_template.push_back(WaitDays{10});
  so_repeat.queue.clear();
  st.ship_orders[201] = so_repeat;

  // Wreck near the colony (within default docking range)
  Wreck w;
  w.id = 300;
  w.system_id = 1;
  w.position_mkm = Vec2{1.0, 0.0};
  w.minerals["Duranium"] = 100.0;
  st.wrecks[300] = w;


  // A second smaller wreck, so that an additional salvage ship would be assigned if it were considered idle.
  Wreck w2 = w;
  w2.id = 301;
  w2.minerals.clear();
  w2.minerals["Duranium"] = 50.0;
  st.wrecks[301] = w2;

  sim.load_game(st);

  // Day 1: salvage should pick up the wreck's minerals.
  sim.advance_days(1);
  {
    const GameState& s = sim.state();
    N4X_ASSERT(s.wrecks.find(300) == s.wrecks.end());
    const auto it_ship = s.ships.find(200);
    N4X_ASSERT(it_ship != s.ships.end());
    const Ship& sh = it_ship->second;
    N4X_ASSERT(sh.cargo.count("Duranium") != 0);
    N4X_ASSERT(sh.cargo.at("Duranium") >= 99.999);


    // The repeating salvage ship should not be assigned salvage orders. Its empty queue should have refilled
    // from the repeat template during tick_ships().
    const ShipOrders* so = find_ptr(s.ship_orders, 201);
    N4X_ASSERT(so != nullptr);
    N4X_ASSERT(!so->queue.empty());
    N4X_ASSERT(std::holds_alternative<WaitDays>(so->queue.front()));
    const WaitDays wd = std::get<WaitDays>(so->queue.front());
    N4X_ASSERT(wd.days_remaining == 9);
  }

  // Day 2: auto-salvage should unload minerals to the nearest friendly colony.
  sim.advance_days(1);
  {
    const GameState& s = sim.state();
    const auto it_col = s.colonies.find(100);
    N4X_ASSERT(it_col != s.colonies.end());
    const Colony& c = it_col->second;
    N4X_ASSERT(c.minerals.count("Duranium") != 0);
    N4X_ASSERT(c.minerals.at("Duranium") >= 99.999);

    const auto it_ship = s.ships.find(200);
    N4X_ASSERT(it_ship != s.ships.end());
    const Ship& sh = it_ship->second;
    const double remaining = sh.cargo.count("Duranium") ? sh.cargo.at("Duranium") : 0.0;
    N4X_ASSERT(remaining <= 1e-6);
  }

  return 0;
}
