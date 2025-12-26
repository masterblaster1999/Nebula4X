#include "nebula4x/core/scenario.h"

#include "nebula4x/core/date.h"

namespace nebula4x {

GameState make_sol_scenario() {
  GameState s;
  s.save_version = 1;
  s.date = Date::from_ymd(2200, 1, 1);

  const Id terrans = allocate_id(s);
  s.factions[terrans] = Faction{terrans, "Terran Union", 0.0, {"chemistry_1"}};

  const Id sol = allocate_id(s);
  StarSystem system;
  system.id = sol;
  system.name = "Sol";
  system.galaxy_pos = {0.0, 0.0};
  s.systems[sol] = system;
  s.selected_system = sol;

  auto add_body = [&](const std::string& name, BodyType type, double radius_mkm, double period_days, double phase) {
    const Id id = allocate_id(s);
    Body b;
    b.id = id;
    b.name = name;
    b.type = type;
    b.system_id = sol;
    b.orbit_radius_mkm = radius_mkm;
    b.orbit_period_days = period_days;
    b.orbit_phase_radians = phase;
    s.bodies[id] = b;
    s.systems[sol].bodies.push_back(id);
    return id;
  };

  const Id sun = add_body("Sun", BodyType::Star, 0.0, 1.0, 0.0);
  (void)sun;
  const Id earth = add_body("Earth", BodyType::Planet, 149.6, 365.25, 0.0);
  const Id mars = add_body("Mars", BodyType::Planet, 227.9, 686.98, 1.0);
  const Id jupiter = add_body("Jupiter", BodyType::GasGiant, 778.5, 4332.6, 2.0);
  (void)mars;
  (void)jupiter;

  const Id earth_colony = allocate_id(s);
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
      {"shipyard", 1},
  };
  s.colonies[earth_colony] = c;

  auto add_ship = [&](const std::string& name, const std::string& design_id) {
    const Id id = allocate_id(s);
    Ship ship;
    ship.id = id;
    ship.name = name;
    ship.faction_id = terrans;
    ship.system_id = sol;
    ship.design_id = design_id;
    ship.position_mkm = {149.6, 0.0};
    s.ships[id] = ship;
    s.ship_orders[id] = ShipOrders{};
    s.systems[sol].ships.push_back(id);
    return id;
  };

  (void)add_ship("Freighter Alpha", "freighter_alpha");
  (void)add_ship("Surveyor Beta", "surveyor_beta");

  return s;
}

} // namespace nebula4x
