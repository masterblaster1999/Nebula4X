#include <cmath>
#include <iostream>
#include <string>

#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";     \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

int test_ship_repairs() {
  using namespace nebula4x;

  ContentDB content;
  {
    ShipDesign d;
    d.id = "repair_test";
    d.name = "Repair Test";
    d.max_hp = 10.0;
    d.speed_km_s = 0.0;
    d.cargo_tons = 0.0;
    content.designs[d.id] = d;
  }

  SimConfig cfg;
  cfg.repair_hp_per_day_per_shipyard = 2.0;
  cfg.docking_range_mkm = 1.0;
  cfg.max_events = 1000;

  Simulation sim(content, cfg);

  GameState s;
  s.save_version = 12;
  s.date = Date::from_ymd(2200, 1, 1);
  s.next_id = 1;

  const Id fac_id = allocate_id(s);
  {
    Faction f;
    f.id = fac_id;
    f.name = "Faction";
    s.factions[fac_id] = f;
  }

  const Id sys_id = allocate_id(s);
  {
    StarSystem sys;
    sys.id = sys_id;
    sys.name = "Sol";
    sys.galaxy_pos = {0.0, 0.0};
    s.systems[sys_id] = sys;
  }

  // Pre-seed discovery to keep load_game deterministic.
  s.factions[fac_id].discovered_systems = {sys_id};

  const Id body_id = allocate_id(s);
  {
    Body b;
    b.id = body_id;
    b.name = "Earth";
    b.type = BodyType::Planet;
    b.system_id = sys_id;
    b.orbit_radius_mkm = 0.0;
    b.orbit_period_days = 1.0;
    b.orbit_phase_radians = 0.0;
    s.bodies[body_id] = b;
    s.systems[sys_id].bodies.push_back(body_id);
  }

  const Id colony_id = allocate_id(s);
  {
    Colony c;
    c.id = colony_id;
    c.name = "Earth";
    c.faction_id = fac_id;
    c.body_id = body_id;
    c.population_millions = 1000.0;
    c.installations = {{"shipyard", 1}};
    s.colonies[colony_id] = c;
  }

  const Id ship_id = allocate_id(s);
  {
    Ship sh;
    sh.id = ship_id;
    sh.name = "Damaged";
    sh.faction_id = fac_id;
    sh.system_id = sys_id;
    sh.design_id = "repair_test";
    sh.position_mkm = {0.0, 0.0};
    sh.hp = 5.0;
    s.ships[ship_id] = sh;
    s.ship_orders[ship_id] = ShipOrders{};
    s.systems[sys_id].ships.push_back(ship_id);
  }

  const Id ship_far_id = allocate_id(s);
  {
    Ship sh;
    sh.id = ship_far_id;
    sh.name = "Far Away";
    sh.faction_id = fac_id;
    sh.system_id = sys_id;
    sh.design_id = "repair_test";
    sh.position_mkm = {100.0, 0.0};
    sh.hp = 5.0;
    s.ships[ship_far_id] = sh;
    s.ship_orders[ship_far_id] = ShipOrders{};
    s.systems[sys_id].ships.push_back(ship_far_id);
  }

  s.selected_system = sys_id;

  sim.load_game(std::move(s));

  // After one day: docked ship repairs +2 HP, far ship does not.
  sim.advance_days(1);
  {
    const auto* docked = find_ptr(sim.state().ships, ship_id);
    const auto* far = find_ptr(sim.state().ships, ship_far_id);
    N4X_ASSERT(docked && far);
    N4X_ASSERT(std::abs(docked->hp - 7.0) <= 1e-9);
    N4X_ASSERT(std::abs(far->hp - 5.0) <= 1e-9);
  }

  // After total 3 days: 5 -> 7 -> 9 -> 10 (capped).
  sim.advance_days(2);
  {
    const auto* docked = find_ptr(sim.state().ships, ship_id);
    N4X_ASSERT(docked);
    N4X_ASSERT(std::abs(docked->hp - 10.0) <= 1e-9);
  }

  // We only log an event when the ship becomes fully repaired (avoid spam).
  int repair_events = 0;
  for (const auto& ev : sim.state().events) {
    if (ev.category != EventCategory::Shipyard) continue;
    if (ev.ship_id != ship_id) continue;
    if (ev.message.find("Ship repaired") == std::string::npos) continue;
    repair_events += 1;
  }
  N4X_ASSERT(repair_events == 1);


  // --- Ship maintenance / spare parts ---
  {
    ContentDB content2;
    {
      ShipDesign d;
      d.id = "maint_test";
      d.name = "Maintenance Test";
      d.max_hp = 10.0;
      d.mass_tons = 100.0;
      d.speed_km_s = 0.0;
      d.cargo_tons = 10.0;
      content2.designs[d.id] = d;
    }

    SimConfig cfg2;
    cfg2.enable_ship_maintenance = true;
    cfg2.ship_maintenance_resource_id = "Metals";
    cfg2.ship_maintenance_tons_per_day_per_mass_ton = 0.1;  // 10t/day for a 100t ship.
    cfg2.ship_maintenance_recovery_per_day = 1.0;
    cfg2.ship_maintenance_decay_per_day = 0.5;
    cfg2.docking_range_mkm = 1.0;
    cfg2.max_events = 1000;

    Simulation sim2(content2, cfg2);

    GameState s2;
    s2.save_version = 12;
    s2.date = Date::from_ymd(2200, 1, 1);
    s2.next_id = 1;

    const Id fac2 = allocate_id(s2);
    {
      Faction f;
      f.id = fac2;
      f.name = "Faction";
      s2.factions[fac2] = f;
    }

    const Id sys2 = allocate_id(s2);
    {
      StarSystem sys;
      sys.id = sys2;
      sys.name = "Sol";
      sys.galaxy_pos = {0.0, 0.0};
      s2.systems[sys2] = sys;
    }

    s2.factions[fac2].discovered_systems = {sys2};

    const Id body2 = allocate_id(s2);
    {
      Body b;
      b.id = body2;
      b.name = "Earth";
      b.type = BodyType::Planet;
      b.system_id = sys2;
      b.orbit_radius_mkm = 0.0;
      b.orbit_period_days = 1.0;
      b.orbit_phase_radians = 0.0;
      s2.bodies[body2] = b;
      s2.systems[sys2].bodies.push_back(body2);
    }

    const Id colony2 = allocate_id(s2);
    {
      Colony c;
      c.id = colony2;
      c.name = "Earth";
      c.faction_id = fac2;
      c.body_id = body2;
      c.population_millions = 1000.0;
      c.minerals["Metals"] = 100.0;
      s2.colonies[colony2] = c;
    }

    const Id ship2 = allocate_id(s2);
    {
      Ship sh;
      sh.id = ship2;
      sh.name = "Maint";
      sh.faction_id = fac2;
      sh.system_id = sys2;
      sh.design_id = "maint_test";
      sh.position_mkm = {0.0, 0.0};
      sh.hp = 10.0;
      sh.maintenance_condition = 0.0;
      s2.ships[ship2] = sh;
      s2.ship_orders[ship2] = ShipOrders{};
      s2.systems[sys2].ships.push_back(ship2);
    }

    s2.selected_system = sys2;

    sim2.load_game(std::move(s2));

    // With sufficient "Metals" on the colony, the ship consumes supplies and recovers to full maintenance.
    sim2.advance_days(1);
    {
      const auto* sh = find_ptr(sim2.state().ships, ship2);
      const auto* col = find_ptr(sim2.state().colonies, colony2);
      N4X_ASSERT(sh && col);
      N4X_ASSERT(std::abs(sh->maintenance_condition - 1.0) <= 1e-9);
      N4X_ASSERT(std::abs(col->minerals.at("Metals") - 90.0) <= 1e-9);
    }

    // No supplies -> condition decays.
    sim2.state().colonies[colony2].minerals["Metals"] = 0.0;
    sim2.state().ships[ship2].maintenance_condition = 1.0;

    sim2.advance_days(2);
    {
      const auto* sh = find_ptr(sim2.state().ships, ship2);
      N4X_ASSERT(sh);
      N4X_ASSERT(std::abs(sh->maintenance_condition - 0.0) <= 1e-9);
    }
  }

  return 0;
}
