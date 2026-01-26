#include <gtest/gtest.h>

#include "nebula4x/core/simulation.h"
#include "nebula4x/core/tech.h"

// A focused regression test for ship auto-refuel planning.
// The older version of this test relied on deprecated APIs and field names.
TEST(AutoRefuel, PlansMoveToFuelColonyWhenBelowThreshold) {
  using namespace nebula4x;

  // Minimal content DB containing a single ship design with fuel stats.
  ContentDB content;
  {
    ShipDesign d;
    d.id = 100;
    d.name = "Test Tanker";
    d.role = ShipRole::Freighter;
    d.speed_km_s = 1000.0;
    d.fuel_capacity_tons = 100.0;
    d.fuel_use_per_mkm = 0.1;  // 0.1 tons per mkm
    content.designs[d.id] = d;
  }

  SimConfig cfg;
  Simulation sim(std::move(content), cfg);

  // Build a small custom game state with one system, one fuel colony, one ship.
  GameState st;
  st.date = Date(0);
  st.hour_of_day = 0;

  // Faction.
  {
    Faction f;
    f.id = 1;
    f.name = "Player";
    f.control = FactionControl::Player;
    f.discovered_systems.insert(1);
    st.factions[f.id] = f;
  }

  // System.
  {
    StarSystem sys;
    sys.id = 1;
    sys.name = "Alpha";
    sys.galaxy_pos_mly = Vec2{0.0, 0.0};
    sys.bodies.push_back(10);
    st.systems[sys.id] = sys;
  }

  // Body hosting the refuel colony.
  {
    Body b;
    b.id = 10;
    b.name = "Fuel Depot";
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
    s.design_id = 100;
    s.system_id = 1;
    s.position_mkm = Vec2{0.0, 0.0};

    s.auto_refuel = true;
    s.auto_refuel_threshold_fraction = 0.5;  // refuel below 50%
    s.fuel_tons = 10.0;  // 10% of capacity (100)

    st.ships[s.id] = s;
  }

  sim.load_game(std::move(st));

  // Run AI planning – should enqueue a MoveToBody order to the fuel depot.
  sim.run_ai_planning();

  const auto& orders = sim.state().ship_orders;
  auto it = orders.find(42);
  ASSERT_TRUE(it != orders.end());
  ASSERT_FALSE(it->second.empty());

  const auto* move = std::get_if<MoveToBody>(&it->second.front());
  ASSERT_NE(move, nullptr);
  EXPECT_EQ(move->body_id, 10);
}
