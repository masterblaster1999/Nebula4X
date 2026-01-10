#include <cmath>
#include <iostream>
#include <string>
#include <utility>

#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";      \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

int test_crew_experience() {
  using namespace nebula4x;

  // --- Docked training increases crew grade points ---
  {
    ContentDB content;

    ShipDesign d;
    d.id = "crew_train_ship";
    d.name = "Crew Train Ship";
    d.max_hp = 10.0;
    d.mass_tons = 100.0;
    d.speed_km_s = 0.0;
    d.sensor_range_mkm = 0.0;
    content.designs[d.id] = d;

    InstallationDef inst;
    inst.id = "training_facility";
    inst.name = "Training Facility";
    inst.crew_training_points_per_day = 10.0;
    content.installations[inst.id] = inst;

    SimConfig cfg;
    cfg.docking_range_mkm = 1.0;
    cfg.enable_crew_experience = true;
    cfg.crew_initial_grade_points = 100.0;
    cfg.crew_training_points_multiplier = 1.0;
    cfg.max_events = 1000;

    Simulation sim(content, cfg);

    GameState s;
    s.save_version = 12;
    s.date = Date::from_ymd(2200, 1, 1);
    s.next_id = 1;

    const Id fac = allocate_id(s);
    Faction f;
    f.id = fac;
    f.name = "Faction";
    s.factions[fac] = f;

    const Id sys = allocate_id(s);
    StarSystem sysobj;
    sysobj.id = sys;
    sysobj.name = "Sol";
    sysobj.galaxy_pos = {0, 0};
    s.systems[sys] = sysobj;
    s.factions[fac].discovered_systems = {sys};

    const Id body = allocate_id(s);
    Body b;
    b.id = body;
    b.name = "Earth";
    b.type = BodyType::Planet;
    b.system_id = sys;
    b.orbit_radius_mkm = 0;
    b.orbit_period_days = 1;
    b.orbit_phase_radians = 0;
    s.bodies[body] = b;
    s.systems[sys].bodies.push_back(body);

    const Id col = allocate_id(s);
    Colony c;
    c.id = col;
    c.name = "Earth";
    c.faction_id = fac;
    c.body_id = body;
    c.population_millions = 1000.0;
    c.installations = {{"training_facility", 1}};
    s.colonies[col] = c;

    const Id ship = allocate_id(s);
    Ship sh;
    sh.id = ship;
    sh.name = "Trainee";
    sh.faction_id = fac;
    sh.system_id = sys;
    sh.design_id = d.id;
    sh.position_mkm = {0.0, 0.0};
    sh.hp = 10.0;
    s.ships[ship] = sh;
    s.ship_orders[ship] = ShipOrders{};
    s.systems[sys].ships.push_back(ship);

    s.selected_system = sys;

    sim.load_game(std::move(s));

    const Ship* before = find_ptr(sim.state().ships, ship);
    N4X_ASSERT(before);
    const double gp0 = before->crew_grade_points;
    N4X_ASSERT(gp0 > 99.0 && gp0 < 101.0);

    sim.advance_days(1);

    const Ship* after = find_ptr(sim.state().ships, ship);
    N4X_ASSERT(after);
    N4X_ASSERT(after->crew_grade_points > gp0 + 9.999);
    N4X_ASSERT(after->crew_grade_points < gp0 + 10.001);
  }

  // --- Crew bonus increases beam accuracy (damage output) ---
  auto run_duel = [](double attacker_crew_points) -> double {
    using namespace nebula4x;

    ContentDB content;

    ShipDesign attacker_d;
    attacker_d.id = "attacker";
    attacker_d.name = "Attacker";
    attacker_d.max_hp = 100.0;
    attacker_d.weapon_damage = 10.0;
    attacker_d.weapon_range_mkm = 100.0;
    attacker_d.sensor_range_mkm = 1000.0;
    attacker_d.speed_km_s = 0.0;
    content.designs[attacker_d.id] = attacker_d;

    ShipDesign target_d;
    target_d.id = "target";
    target_d.name = "Target";
    target_d.max_hp = 100.0;
    target_d.weapon_damage = 0.0;
    target_d.weapon_range_mkm = 0.0;
    target_d.sensor_range_mkm = 0.0;
    target_d.speed_km_s = 0.0;
    content.designs[target_d.id] = target_d;

    SimConfig cfg;
    cfg.enable_crew_experience = true;
    cfg.crew_combat_grade_points_per_damage = 0.0; // keep crew fixed during the duel
    cfg.max_events = 1000;

    // Make beam hit chance deterministic and non-trivial.
    cfg.enable_beam_hit_chance = true;
    cfg.beam_base_hit_chance = 0.95;
    cfg.beam_range_penalty_at_max = 0.4;
    cfg.beam_min_hit_chance = 0.05;

    Simulation sim(content, cfg);

    GameState s;
    s.save_version = 12;
    s.date = Date::from_ymd(2200, 1, 1);
    s.next_id = 1;

    const Id facA = allocate_id(s);
    Faction fA;
    fA.id = facA;
    fA.name = "A";
    s.factions[facA] = fA;

    const Id facB = allocate_id(s);
    Faction fB;
    fB.id = facB;
    fB.name = "B";
    s.factions[facB] = fB;

    const Id sys = allocate_id(s);
    StarSystem sysobj;
    sysobj.id = sys;
    sysobj.name = "Sol";
    sysobj.galaxy_pos = {0, 0};
    s.systems[sys] = sysobj;
    s.factions[facA].discovered_systems = {sys};
    s.factions[facB].discovered_systems = {sys};

    const Id body = allocate_id(s);
    Body b;
    b.id = body;
    b.name = "Ref";
    b.type = BodyType::Planet;
    b.system_id = sys;
    b.orbit_radius_mkm = 0;
    b.orbit_period_days = 1;
    b.orbit_phase_radians = 0;
    s.bodies[body] = b;
    s.systems[sys].bodies.push_back(body);

    const Id shipA = allocate_id(s);
    Ship shA;
    shA.id = shipA;
    shA.name = "A";
    shA.faction_id = facA;
    shA.system_id = sys;
    shA.design_id = "attacker";
    shA.position_mkm = {0.0, 0.0};
    shA.hp = 100.0;
    shA.crew_grade_points = attacker_crew_points;
    s.ships[shipA] = shA;
    s.ship_orders[shipA] = ShipOrders{};
    s.systems[sys].ships.push_back(shipA);

    const Id shipB = allocate_id(s);
    Ship shB;
    shB.id = shipB;
    shB.name = "B";
    shB.faction_id = facB;
    shB.system_id = sys;
    shB.design_id = "target";
    shB.position_mkm = {100.0, 0.0};
    shB.hp = 100.0;
    s.ships[shipB] = shB;
    s.ship_orders[shipB] = ShipOrders{};
    s.systems[sys].ships.push_back(shipB);

    s.selected_system = sys;

    sim.load_game(std::move(s));
    sim.advance_days(1);

    const Ship* tgt = find_ptr(sim.state().ships, shipB);
    return tgt ? tgt->hp : 0.0;
  };

  const double hp_regular = run_duel(100.0);
  const double hp_trained = run_duel(400.0); // +10% bonus

  N4X_ASSERT(hp_trained < hp_regular - 1e-6);

  return 0;
}
