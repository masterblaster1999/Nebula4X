#include "nebula4x/core/simulation.h"

#include <catch2/catch_test_macros.hpp>

using namespace nebula4x;

namespace {

ContentDB minimal_content_for_colonization() {
  ContentDB c;

  ShipDesign d;
  d.id = "colony_ship";
  d.name = "Colony Ship";
  d.role = ShipRole::Freighter;
  d.mass_tons = 100.0;
  d.speed_km_s = 10.0;
  d.cargo_tons = 0.0;
  d.sensor_range_mkm = 0.0;
  d.colony_capacity_millions = 50.0;
  d.max_hp = 200.0;
  c.designs[d.id] = d;

  return c;
}

GameState minimal_state_for_colonization() {
  GameState s;
  s.next_id = 1000;

  System sys;
  sys.id = 1;
  sys.name = "Test System";
  sys.bodies = {10};
  sys.ships = {100};
  s.systems[sys.id] = sys;

  Body b;
  b.id = 10;
  b.system_id = 1;
  b.name = "New Terra";
  b.type = BodyType::Planet;
  b.orbit_radius_mkm = 0.0;
  b.orbit_period_days = 0.0;
  b.position_mkm = {0.0, 0.0};
  s.bodies[b.id] = b;

  Faction f;
  f.id = 2;
  f.name = "Testers";
  f.discovered_systems = {1};
  s.factions[f.id] = f;

  Ship ship;
  ship.id = 100;
  ship.faction_id = 2;
  ship.system_id = 1;
  ship.name = "Colony Ship 001";
  ship.design_id = "colony_ship";
  ship.position_mkm = {0.0, 0.0};
  ship.cargo["Duranium"] = 123.0;
  s.ships[ship.id] = ship;

  s.ship_orders[ship.id] = ShipOrders{};

  return s;
}

}  // namespace

TEST_CASE("ColonizeBody creates a new colony and consumes the colonizer ship") {
  ContentDB content = minimal_content_for_colonization();
  Simulation sim(content, SimConfig{});
  sim.load_game(minimal_state_for_colonization());

  REQUIRE(sim.issue_colonize_body(100, 10, "New Terra Colony"));
  sim.advance_days(1);

  const auto& st = sim.state();
  REQUIRE(st.ships.find(100) == st.ships.end());
  REQUIRE(st.colonies.size() == 1);

  const Colony& col = st.colonies.begin()->second;
  CHECK(col.body_id == 10);
  CHECK(col.faction_id == 2);
  CHECK(col.name == "New Terra Colony");
  CHECK(col.population_millions == Approx(50.0));
  CHECK(col.minerals.at("Duranium") == Approx(123.0));
}

TEST_CASE("ColonizeBody does nothing if body already has a colony") {
  ContentDB content = minimal_content_for_colonization();
  Simulation sim(content, SimConfig{});
  GameState s = minimal_state_for_colonization();

  Colony existing;
  existing.id = 200;
  existing.faction_id = 2;
  existing.body_id = 10;
  existing.name = "Existing Colony";
  existing.population_millions = 10.0;
  s.colonies[existing.id] = existing;

  sim.load_game(std::move(s));
  REQUIRE(sim.issue_colonize_body(100, 10));
  sim.advance_days(1);

  const auto& st = sim.state();
  // Ship should remain because colonization was rejected.
  CHECK(st.ships.find(100) != st.ships.end());
  CHECK(st.colonies.size() == 1);
}
