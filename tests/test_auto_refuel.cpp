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

// Regression test: ship auto-refuel planning should enqueue a MoveToBody order
// to the nearest reachable trade-partner colony that has fuel.
int test_auto_refuel() {
  using namespace nebula4x;

  ContentDB content;
  {
    ShipDesign d;
    d.id = "test_tanker";
    d.name = "Test Tanker";
    d.role = ShipRole::Freighter;
    d.mass_tons = 100.0;
    d.speed_km_s = 1000.0;
    d.fuel_capacity_tons = 100.0;
    d.fuel_use_per_mkm = 0.1;  // 0.1 tons per mkm
    content.designs[d.id] = d;
  }

  SimConfig cfg;
  Simulation sim(std::move(content), cfg);

  GameState st;
  st.save_version = GameState{}.save_version;
  st.date = Date::from_ymd(2200, 1, 1);
  st.hour_of_day = 0;
  st.next_id = 1000;
  st.selected_system = 1;

  // Faction.
  {
    Faction f;
    f.id = 1;
    f.name = "Player";
    f.control = FactionControl::Player;
    f.discovered_systems.push_back(1);
    st.factions[f.id] = f;
  }

  // System.
  {
    StarSystem sys;
    sys.id = 1;
    sys.name = "Alpha";
    sys.galaxy_pos = Vec2{0.0, 0.0};
    sys.bodies.push_back(10);
    sys.ships.push_back(42);
    st.systems[sys.id] = sys;
  }

  // Body hosting the refuel colony.
  {
    Body b;
    b.id = 10;
    b.name = "Fuel Depot";
    b.type = BodyType::Planet;
    b.system_id = 1;
    b.position_mkm = Vec2{100.0, 0.0};
    b.radius_km = 1000.0;
    st.bodies[b.id] = b;
  }

  // Colony with fuel available.
  {
    Colony c;
    c.id = 500;
    c.name = "Depot Colony";
    c.faction_id = 1;
    c.body_id = 10;
    c.minerals["Fuel"] = 10000.0;
    st.colonies[c.id] = c;
  }

  // Ship with low fuel and auto-refuel enabled.
  {
    Ship s;
    s.id = 42;
    s.name = "Tanker 1";
    s.faction_id = 1;
    s.design_id = "test_tanker";
    s.system_id = 1;
    s.position_mkm = Vec2{0.0, 0.0};

    s.auto_refuel = true;
    s.auto_refuel_threshold_fraction = 0.5;  // refuel below 50%
    s.fuel_tons = 10.0;                      // 10% of capacity (100)

    st.ships[s.id] = s;
  }

  sim.load_game(std::move(st));

  // AI planning should enqueue a MoveToBody order to the fuel depot.
  sim.run_ai_planning();

  const auto& orders = sim.state().ship_orders;
  auto it = orders.find(42);
  N4X_ASSERT(it != orders.end());
  N4X_ASSERT(!it->second.queue.empty());

  const auto* move = std::get_if<MoveToBody>(&it->second.queue.front());
  N4X_ASSERT(move != nullptr);
  N4X_ASSERT(move->body_id == 10);

  return 0;
}
