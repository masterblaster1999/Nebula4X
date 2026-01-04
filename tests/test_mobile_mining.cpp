#include "test.h"

#include <cmath>
#include <iostream>

#include "nebula4x/core/simulation.h"

using namespace nebula4x;

#define N4X_ASSERT(cond)                                                                            \
  do {                                                                                              \
    if (!(cond)) {                                                                                  \
      std::cerr << "ASSERT FAIL (" << __FILE__ << ":" << __LINE__ << "): " << #cond << std::endl;   \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

static ContentDB make_content(double cargo_cap_tons, double mine_rate_tpd) {
  ContentDB content;

  ComponentDef mining;
  mining.id = "mining_laser_test";
  mining.name = "Mining Laser (Test)";
  mining.type = ComponentType::Mining;
  mining.mass_tons = 10.0;
  mining.mining_tons_per_day = mine_rate_tpd;
  content.components[mining.id] = mining;

  ComponentDef cargo;
  cargo.id = "cargo_hold_test";
  cargo.name = "Cargo Hold (Test)";
  cargo.type = ComponentType::Cargo;
  cargo.mass_tons = 5.0;
  cargo.cargo_tons = cargo_cap_tons;
  content.components[cargo.id] = cargo;

  ShipDesign d;
  d.id = "miner_test";
  d.name = "Miner (Test)";
  d.role = ShipRole::Freighter;
  d.components = {mining.id, cargo.id};
  d.cargo_tons = cargo_cap_tons;
  d.mining_tons_per_day = mine_rate_tpd;
  d.speed_km_s = 0.0;  // stationary is fine: mining is handled at docking range
  content.designs[d.id] = d;

  return content;
}

static GameState make_state(double duranium_deposit_tons, const std::string& design_id, bool stop_when_full, double cargo_used_initial = 0.0) {
  GameState s;
  s.next_id = 1000;

  StarSystem sys;
  sys.id = 1;
  sys.name = "Test System";

  Body body;
  body.id = 2;
  body.name = "Test Body";
  body.system_id = sys.id;
  body.position_mkm = Vec2{0.0, 0.0};
  body.orbit_radius_mkm = 0.0;
  body.mineral_deposits["Duranium"] = duranium_deposit_tons;

  sys.bodies.push_back(body.id);

  Faction fac;
  fac.id = 3;
  fac.name = "Test Faction";
  fac.discovered_systems.push_back(sys.id);

  Ship ship;
  ship.id = 10;
  ship.name = "Test Miner";
  ship.design_id = design_id;
  ship.system_id = sys.id;
  ship.faction_id = fac.id;
  ship.position_mkm = body.position_mkm;
  if (cargo_used_initial > 0.0) ship.cargo["Duranium"] = cargo_used_initial;

  sys.ships.push_back(ship.id);

  ShipOrders so;
  MineBody mb;
  mb.body_id = body.id;
  mb.mineral = "Duranium";
  mb.stop_when_cargo_full = stop_when_full;
  so.queue.push_back(mb);

  s.systems[sys.id] = sys;
  s.bodies[body.id] = body;
  s.factions[fac.id] = fac;
  s.ships[ship.id] = ship;
  s.ship_orders[ship.id] = so;

  return s;
}

int test_mobile_mining() {
  // Case A: deposit depletion ends the order.
  {
    auto content = make_content(/*cargo_cap_tons=*/100.0, /*mine_rate_tpd=*/10.0);

    SimConfig cfg;

    Simulation sim(std::move(content), cfg);

    auto state = make_state(/*deposit=*/15.0, /*design_id=*/"miner_test", /*stop_when_full=*/true);

    sim.load_game(std::move(state));
    sim.advance_days(1);

    {
      const auto& st = sim.state();
      const auto* ship = find_ptr(st.ships, Id{10});
      const auto* body = find_ptr(st.bodies, Id{2});
      const auto* so = find_ptr(st.ship_orders, Id{10});
      N4X_ASSERT(ship);
      N4X_ASSERT(body);
      N4X_ASSERT(so);
      N4X_ASSERT(ship->cargo.count("Duranium") == 1);
      N4X_ASSERT(std::abs(ship->cargo.at("Duranium") - 10.0) < 1e-6);
      N4X_ASSERT(std::abs(body->mineral_deposits.at("Duranium") - 5.0) < 1e-6);
      N4X_ASSERT(!so->queue.empty());
    }

    sim.advance_days(1);

    {
      const auto& st = sim.state();
      const auto* ship = find_ptr(st.ships, Id{10});
      const auto* body = find_ptr(st.bodies, Id{2});
      const auto* so = find_ptr(st.ship_orders, Id{10});
      N4X_ASSERT(ship);
      N4X_ASSERT(body);
      N4X_ASSERT(so);
      N4X_ASSERT(std::abs(ship->cargo.at("Duranium") - 15.0) < 1e-6);
      N4X_ASSERT(std::abs(body->mineral_deposits.at("Duranium") - 0.0) < 1e-6);
      N4X_ASSERT(so->queue.empty());
    }
  }

  // Case B: stop_when_cargo_full ends the order when cargo fills.
  {
    auto content = make_content(/*cargo_cap_tons=*/12.0, /*mine_rate_tpd=*/10.0);

    SimConfig cfg;

    Simulation sim(std::move(content), cfg);

    auto state = make_state(/*deposit=*/100.0, /*design_id=*/"miner_test", /*stop_when_full=*/true);

    sim.load_game(std::move(state));
    sim.advance_days(1);

    {
      const auto& st = sim.state();
      const auto* ship = find_ptr(st.ships, Id{10});
      const auto* body = find_ptr(st.bodies, Id{2});
      const auto* so = find_ptr(st.ship_orders, Id{10});
      N4X_ASSERT(ship);
      N4X_ASSERT(body);
      N4X_ASSERT(so);
      N4X_ASSERT(std::abs(ship->cargo.at("Duranium") - 10.0) < 1e-6);
      N4X_ASSERT(std::abs(body->mineral_deposits.at("Duranium") - 90.0) < 1e-6);
      N4X_ASSERT(!so->queue.empty());
    }

    sim.advance_days(1);

    {
      const auto& st = sim.state();
      const auto* ship = find_ptr(st.ships, Id{10});
      const auto* body = find_ptr(st.bodies, Id{2});
      const auto* so = find_ptr(st.ship_orders, Id{10});
      N4X_ASSERT(ship);
      N4X_ASSERT(body);
      N4X_ASSERT(so);
      N4X_ASSERT(std::abs(ship->cargo.at("Duranium") - 12.0) < 1e-6);
      N4X_ASSERT(std::abs(body->mineral_deposits.at("Duranium") - 88.0) < 1e-6);
      N4X_ASSERT(so->queue.empty());
    }
  }

  return 0;
}
