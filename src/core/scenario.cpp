#include "nebula4x/core/scenario.h"

#include "nebula4x/core/date.h"

namespace nebula4x {

GameState make_sol_scenario() {
  GameState s;
  s.save_version = 6;
  s.date = Date::from_ymd(2200, 1, 1);

  // --- Factions ---
  const Id terrans = allocate_id(s);
  {
    Faction f;
    f.id = terrans;
    f.name = "Terran Union";
    f.research_points = 0.0;
    f.known_techs = {"chemistry_1"};
    f.research_queue = {"nuclear_1", "propulsion_1"};
    s.factions[terrans] = f;
  }

  const Id pirates = allocate_id(s);
  {
    Faction f;
    f.id = pirates;
    f.name = "Pirate Raiders";
    f.research_points = 0.0;
    s.factions[pirates] = f;
  }

  // --- Systems ---
  const Id sol = allocate_id(s);
  {
    StarSystem system;
    system.id = sol;
    system.name = "Sol";
    system.galaxy_pos = {0.0, 0.0};
    s.systems[sol] = system;
  }

  const Id centauri = allocate_id(s);
  {
    StarSystem system;
    system.id = centauri;
    system.name = "Alpha Centauri";
    system.galaxy_pos = {4.3, 0.0};
    s.systems[centauri] = system;
  }

  s.selected_system = sol;

  auto add_body = [&](Id system_id, const std::string& name, BodyType type, double radius_mkm, double period_days,
                      double phase) {
    const Id id = allocate_id(s);
    Body b;
    b.id = id;
    b.name = name;
    b.type = type;
    b.system_id = system_id;
    b.orbit_radius_mkm = radius_mkm;
    b.orbit_period_days = period_days;
    b.orbit_phase_radians = phase;
    s.bodies[id] = b;
    s.systems[system_id].bodies.push_back(id);
    return id;
  };

  // --- Sol bodies ---
  (void)add_body(sol, "Sun", BodyType::Star, 0.0, 1.0, 0.0);
  const Id earth = add_body(sol, "Earth", BodyType::Planet, 149.6, 365.25, 0.0);
  (void)add_body(sol, "Mars", BodyType::Planet, 227.9, 686.98, 1.0);
  (void)add_body(sol, "Jupiter", BodyType::GasGiant, 778.5, 4332.6, 2.0);

  // --- Alpha Centauri bodies ---
  (void)add_body(centauri, "Alpha Centauri A", BodyType::Star, 0.0, 1.0, 0.0);
  const Id centauri_prime = add_body(centauri, "Centauri Prime", BodyType::Planet, 110.0, 320.0, 0.4);
  (void)centauri_prime;

  // --- Jump points (Sol <-> Alpha Centauri) ---
  const Id jp_sol = allocate_id(s);
  const Id jp_cen = allocate_id(s);
  {
    JumpPoint a;
    a.id = jp_sol;
    a.name = "Sol Jump Point";
    a.system_id = sol;
    a.position_mkm = {170.0, 0.0};
    a.linked_jump_id = jp_cen;
    s.jump_points[a.id] = a;
    s.systems[sol].jump_points.push_back(a.id);
  }
  {
    JumpPoint b;
    b.id = jp_cen;
    b.name = "Centauri Jump Point";
    b.system_id = centauri;
    b.position_mkm = {80.0, 0.0};
    b.linked_jump_id = jp_sol;
    s.jump_points[b.id] = b;
    s.systems[centauri].jump_points.push_back(b.id);
  }

  // --- Colony ---
  const Id earth_colony = allocate_id(s);
  {
    Colony c;
    c.id = earth_colony;
    c.name = "Earth";
    c.faction_id = terrans;
    c.body_id = earth;
    c.population_millions = 8500.0;
    c.minerals = {
        {"Duranium", 10000.0},
        {"Neutronium", 1500.0},
    };
    c.installations = {
        {"automated_mine", 50},
        {"construction_factory", 5},
        {"shipyard", 1},
        {"research_lab", 20},
        {"sensor_station", 1},
    };
    s.colonies[c.id] = c;
  }

  auto add_ship = [&](Id faction_id, Id system_id, const Vec2& pos, const std::string& name,
                      const std::string& design_id) {
    const Id id = allocate_id(s);
    Ship ship;
    ship.id = id;
    ship.name = name;
    ship.faction_id = faction_id;
    ship.system_id = system_id;
    ship.design_id = design_id;
    ship.position_mkm = pos;
    s.ships[id] = ship;
    s.ship_orders[id] = ShipOrders{};
    s.systems[system_id].ships.push_back(id);
    return id;
  };

  // --- Starting Terran fleet (Sol) ---
  const Vec2 earth_pos = {149.6, 0.0};
  (void)add_ship(terrans, sol, earth_pos, "Freighter Alpha", "freighter_alpha");
  (void)add_ship(terrans, sol, earth_pos + Vec2{0.0, 0.8}, "Surveyor Beta", "surveyor_beta");
  (void)add_ship(terrans, sol, earth_pos + Vec2{0.0, -0.8}, "Escort Gamma", "escort_gamma");

  // --- Pirate presence (Alpha Centauri) ---
  const Vec2 pirate_pos = {80.0, 0.5};
  (void)add_ship(pirates, centauri, pirate_pos, "Raider I", "pirate_raider");
  (void)add_ship(pirates, centauri, pirate_pos + Vec2{0.7, -0.3}, "Raider II", "pirate_raider");

  return s;
}

} // namespace nebula4x
