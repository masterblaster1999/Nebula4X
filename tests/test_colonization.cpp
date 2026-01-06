#include <cmath>
#include <iostream>
#include <string>

#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

namespace {

nebula4x::ContentDB minimal_content_for_colonization() {
  nebula4x::ContentDB c;

  nebula4x::ShipDesign d;
  d.id = "colony_ship";
  d.name = "Colony Ship";
  d.role = nebula4x::ShipRole::Freighter;
  d.mass_tons = 100.0;
  d.speed_km_s = 10.0;
  d.cargo_tons = 0.0;
  d.sensor_range_mkm = 0.0;
  d.colony_capacity_millions = 50.0;
  d.max_hp = 200.0;
  c.designs[d.id] = d;

  return c;
}

nebula4x::GameState minimal_state_for_colonization() {
  using namespace nebula4x;

  GameState s;
  s.next_id = 1000;

  StarSystem sys;
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

int test_colonization() {
  using namespace nebula4x;

  // 1) ColonizeBody creates a new colony and consumes the colonizer ship.
  {
    ContentDB content = minimal_content_for_colonization();
    Simulation sim(content, SimConfig{});
    sim.load_game(minimal_state_for_colonization());

    N4X_ASSERT(sim.issue_colonize_body(100, 10, "New Terra Colony"));
    sim.advance_days(1);

    const auto& st = sim.state();
    N4X_ASSERT(st.ships.find(100) == st.ships.end());
    N4X_ASSERT(st.colonies.size() == 1);

    const Colony& col = st.colonies.begin()->second;
    N4X_ASSERT(col.body_id == 10);
    N4X_ASSERT(col.faction_id == 2);
    N4X_ASSERT(col.name == "New Terra Colony");
    N4X_ASSERT(std::abs(col.population_millions - 50.0) < 1e-9);
    N4X_ASSERT(col.minerals.count("Duranium") && std::abs(col.minerals.at("Duranium") - 123.0) < 1e-9);
  }

  // 2) Optional colony founding profile is applied to newly established colonies.
  {
    ContentDB content = minimal_content_for_colonization();
    Simulation sim(content, SimConfig{});
    GameState s = minimal_state_for_colonization();

    // Configure faction-level founding defaults.
    auto& f = s.factions.at(2);
    f.auto_apply_colony_founding_profile = true;
    f.colony_founding_profile_name = "Default Outpost";
    f.colony_founding_profile.garrison_target_strength = 250.0;
    f.colony_founding_profile.installation_targets["mine"] = 7;
    f.colony_founding_profile.mineral_reserves["Duranium"] = 1000.0;
    f.colony_founding_profile.mineral_targets["Duranium"] = 5000.0;

    sim.load_game(std::move(s));

    N4X_ASSERT(sim.issue_colonize_body(100, 10, "Profiled Colony"));
    sim.advance_days(1);

    const auto& st = sim.state();
    N4X_ASSERT(st.colonies.size() == 1);
    const Colony& col = st.colonies.begin()->second;

    N4X_ASSERT(col.name == "Profiled Colony");
    N4X_ASSERT(col.garrison_target_strength > 249.9);
    N4X_ASSERT(col.installation_targets.count("mine") && col.installation_targets.at("mine") == 7);
    N4X_ASSERT(col.mineral_reserves.count("Duranium") && std::abs(col.mineral_reserves.at("Duranium") - 1000.0) < 1e-9);
    N4X_ASSERT(col.mineral_targets.count("Duranium") && std::abs(col.mineral_targets.at("Duranium") - 5000.0) < 1e-9);
  }

  // 3) ColonizeBody aborts if the body already has a colony (ship remains).
  {
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
    N4X_ASSERT(sim.issue_colonize_body(100, 10));
    sim.advance_days(1);

    const auto& st = sim.state();
    N4X_ASSERT(st.ships.find(100) != st.ships.end());
    N4X_ASSERT(st.colonies.size() == 1);
  }

  return 0;
}
