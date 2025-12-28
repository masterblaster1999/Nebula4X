#include "nebula4x/core/scenario.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#include "nebula4x/core/date.h"

namespace nebula4x {

GameState make_sol_scenario() {
  GameState s;
  // Bump when the save schema changes.
  // v8: adds WaitDays order type.
  // v9: adds ShipOrders repeat fields (repeat + repeat_template).
  // v10: adds persistent GameState::events (simulation event log).
  // v11: adds structured event fields (category + context ids).
  // v12: adds SimEvent::seq + GameState::next_event_seq (monotonic event ids).
  // New games should start at the current save schema version.
  s.save_version = GameState{}.save_version;
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
    f.control = FactionControl::AI_Pirate;
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

  // A third system to make the early exploration loop more interesting.
  const Id barnard = allocate_id(s);
  {
    StarSystem system;
    system.id = barnard;
    system.name = "Barnard's Star";
    system.galaxy_pos = {6.0, -1.4};
    s.systems[barnard] = system;
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
  const Id mars = add_body(sol, "Mars", BodyType::Planet, 227.9, 686.98, 1.0);
  (void)add_body(sol, "Jupiter", BodyType::GasGiant, 778.5, 4332.6, 2.0);

  // --- Alpha Centauri bodies ---
  (void)add_body(centauri, "Alpha Centauri A", BodyType::Star, 0.0, 1.0, 0.0);
  const Id centauri_prime = add_body(centauri, "Centauri Prime", BodyType::Planet, 110.0, 320.0, 0.4);
  (void)centauri_prime;

  // --- Barnard's Star bodies ---
  (void)add_body(barnard, "Barnard's Star", BodyType::Star, 0.0, 1.0, 0.0);
  (void)add_body(barnard, "Barnard b", BodyType::Planet, 60.0, 233.0, 0.2);

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

  // --- Jump points (Alpha Centauri <-> Barnard's Star) ---
  const Id jp_cen2 = allocate_id(s);
  const Id jp_bar = allocate_id(s);
  {
    JumpPoint a;
    a.id = jp_cen2;
    a.name = "Centauri Outer Jump";
    a.system_id = centauri;
    a.position_mkm = {140.0, -35.0};
    a.linked_jump_id = jp_bar;
    s.jump_points[a.id] = a;
    s.systems[centauri].jump_points.push_back(a.id);
  }
  {
    JumpPoint b;
    b.id = jp_bar;
    b.name = "Barnard Jump Point";
    b.system_id = barnard;
    b.position_mkm = {55.0, 10.0};
    b.linked_jump_id = jp_cen2;
    s.jump_points[b.id] = b;
    s.systems[barnard].jump_points.push_back(b.id);
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

  const Id mars_colony = allocate_id(s);
  {
    Colony c;
    c.id = mars_colony;
    c.name = "Mars Outpost";
    c.faction_id = terrans;
    c.body_id = mars;
    c.population_millions = 250.0;
    c.minerals = {
        {"Duranium", 200.0},
        {"Neutronium", 20.0},
    };
    c.installations = {
        {"automated_mine", 5},
        {"construction_factory", 1},
        {"research_lab", 2},
    };
    s.colonies[c.id] = c;
  }

  // --- Pirate base colony (Alpha Centauri) ---
  // Gives the default pirates an economy so they can scale up over time.
  {
    Colony c;
    c.id = allocate_id(s);
    c.name = "Haven";
    c.faction_id = pirates;
    c.body_id = centauri_prime;
    c.population_millions = 200.0;
    c.installations["shipyard"] = 1;
    c.installations["construction_factory"] = 1;
    c.installations["research_lab"] = 5;
    c.installations["sensor_station"] = 1;
    c.installations["automated_mine"] = 10;
    c.minerals["Duranium"] = 15000.0;
    c.minerals["Neutronium"] = 1500.0;
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

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

double rand_real(std::mt19937& rng, double lo, double hi) {
  std::uniform_real_distribution<double> dist(lo, hi);
  return dist(rng);
}

int rand_int(std::mt19937& rng, int lo, int hi) {
  std::uniform_int_distribution<int> dist(lo, hi);
  return dist(rng);
}

// Hash an undirected (a,b) pair into a single integer.
std::uint64_t edge_key(int a, int b) {
  const auto lo = static_cast<std::uint32_t>(std::min(a, b));
  const auto hi = static_cast<std::uint32_t>(std::max(a, b));
  return (static_cast<std::uint64_t>(lo) << 32) | static_cast<std::uint64_t>(hi);
}

} // namespace

GameState make_random_scenario(std::uint32_t seed, int num_systems) {
  GameState s;
  // Bump when the save schema changes.
  // v8: adds WaitDays order type.
  // v9: adds ShipOrders repeat fields (repeat + repeat_template).
  // v10: adds persistent GameState::events (simulation event log).
  // v11: adds structured event fields (category + context ids).
  // New games should start at the current save schema version.
  s.save_version = GameState{}.save_version;
  s.date = Date::from_ymd(2200, 1, 1);

  if (num_systems < 1) num_systems = 1;
  // Keep things reasonably small so the prototype UI stays responsive.
  if (num_systems > 64) num_systems = 64;

  std::mt19937 rng(seed);

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
    f.control = FactionControl::AI_Pirate;
    f.research_points = 0.0;
    s.factions[pirates] = f;
  }

  struct SysInfo {
    Id id{kInvalidId};
    std::string name;
    Id star_body{kInvalidId};
    std::vector<Id> planet_bodies;
  };

  std::vector<SysInfo> systems;
  systems.reserve(static_cast<std::size_t>(num_systems));

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

  // --- Systems + bodies ---
  // Galaxy layout is arbitrary units; spread systems in a loose disc.
  const double spread = std::max(3.0, std::sqrt(static_cast<double>(num_systems)) * 2.5);

  for (int i = 0; i < num_systems; ++i) {
    const Id sys_id = allocate_id(s);

    StarSystem sys;
    sys.id = sys_id;
    sys.name = "System " + std::to_string(i + 1);
    sys.galaxy_pos = {rand_real(rng, -spread, spread), rand_real(rng, -spread, spread)};
    s.systems[sys_id] = sys;

    SysInfo info;
    info.id = sys_id;
    info.name = sys.name;

    // Star (anchor at origin)
    info.star_body = add_body(sys_id, sys.name + " Star", BodyType::Star, 0.0, 1.0, 0.0);

    // Planets
    const int planet_count = rand_int(rng, 1, 4);
    for (int p = 0; p < planet_count; ++p) {
      const bool gas = rand_real(rng, 0.0, 1.0) < 0.20;
      const BodyType type = gas ? BodyType::GasGiant : BodyType::Planet;

      // Simple increasing orbits with some jitter.
      const double base_radius = 60.0 + static_cast<double>(p) * 70.0;
      const double radius = std::max(10.0, base_radius + rand_real(rng, -12.0, 12.0));

      // Roughly scale period with radius (not physically accurate, just "feels" right).
      const double base_period = 120.0 + static_cast<double>(p) * 260.0;
      const double period = std::max(20.0, base_period + rand_real(rng, -25.0, 25.0));

      double phase = rand_real(rng, 0.0, kTwoPi);
      // Keep the very first planet of the first system at a stable place so ships can spawn there.
      if (i == 0 && p == 0) phase = 0.0;

      const std::string pname = info.name + " " + std::to_string(p + 1);
      const Id pid = add_body(sys_id, pname, type, radius, period, phase);
      info.planet_bodies.push_back(pid);
    }

    systems.push_back(std::move(info));
  }

  const Id home_system = systems.front().id;
  const Id pirate_system = (systems.size() > 1) ? systems.back().id : home_system;
  s.selected_system = home_system;

  // Seed initial discovery: each faction knows its start system.
  s.factions[terrans].discovered_systems = {home_system};
  s.factions[pirates].discovered_systems = {pirate_system};

  // --- Jump points ---
  // Build a connected graph via a random spanning tree, then add a few extra links.
  std::unordered_set<std::uint64_t> edges;
  edges.reserve(static_cast<std::size_t>(num_systems) * 2);

  auto add_jump_link = [&](int ai, int bi) {
    if (ai == bi) return;
    const std::uint64_t k = edge_key(ai, bi);
    if (edges.find(k) != edges.end()) return;
    edges.insert(k);

    const auto& a = systems[ai];
    const auto& b = systems[bi];

    const Id jp_a_id = allocate_id(s);
    const Id jp_b_id = allocate_id(s);

    auto make_pos = [&]() {
      const double r = rand_real(rng, 120.0, 220.0);
      const double ang = rand_real(rng, 0.0, kTwoPi);
      return Vec2{r * std::cos(ang), r * std::sin(ang)};
    };

    {
      JumpPoint jp;
      jp.id = jp_a_id;
      jp.name = "JP to " + b.name;
      jp.system_id = a.id;
      jp.position_mkm = make_pos();
      jp.linked_jump_id = jp_b_id;
      s.jump_points[jp.id] = jp;
      s.systems[a.id].jump_points.push_back(jp.id);
    }

    {
      JumpPoint jp;
      jp.id = jp_b_id;
      jp.name = "JP to " + a.name;
      jp.system_id = b.id;
      jp.position_mkm = make_pos();
      jp.linked_jump_id = jp_a_id;
      s.jump_points[jp.id] = jp;
      s.systems[b.id].jump_points.push_back(jp.id);
    }
  };

  // Spanning tree.
  for (int i = 1; i < num_systems; ++i) {
    const int parent = rand_int(rng, 0, i - 1);
    add_jump_link(i, parent);
  }

  // Extra edges to create loops.
  const int extra = std::max(0, num_systems / 3);
  for (int e = 0; e < extra; ++e) {
    const int a = rand_int(rng, 0, num_systems - 1);
    const int b = rand_int(rng, 0, num_systems - 1);
    add_jump_link(a, b);
  }

  // --- Colonies ---
  const Id homeworld_body = !systems.front().planet_bodies.empty() ? systems.front().planet_bodies.front()
                                                                   : systems.front().star_body;

  const Id home_colony = allocate_id(s);
  {
    Colony c;
    c.id = home_colony;
    c.name = "Homeworld";
    c.faction_id = terrans;
    c.body_id = homeworld_body;
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


  // --- Pirate base colony ---
  // Gives pirates a home industry so they can grow beyond their starting ships.
  {
    const SysInfo& ps = (pirate_system == home_system) ? systems.front() : systems.back();
    Id base_body = ps.star_body;
    if (!ps.planet_bodies.empty()) base_body = ps.planet_bodies.front();
    if (pirate_system == home_system && ps.planet_bodies.size() >= 2) base_body = ps.planet_bodies[1];

    Colony c;
    c.id = allocate_id(s);
    c.name = "Pirate Haven";
    c.faction_id = pirates;
    c.body_id = base_body;
    c.population_millions = 200.0;
    c.installations = {
        {"shipyard", 1},
        {"construction_factory", 1},
        {"research_lab", 5},
        {"sensor_station", 1},
        {"automated_mine", 10},
    };
    c.minerals = {
        {"Duranium", 15000.0},
        {"Neutronium", 1500.0},
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

  // Spawn near the homeworld (t=0, so phase is position directly).
  Vec2 home_pos{0.0, 0.0};
  if (const auto* b = find_ptr(s.bodies, homeworld_body)) {
    home_pos = {b->orbit_radius_mkm * std::cos(b->orbit_phase_radians),
                b->orbit_radius_mkm * std::sin(b->orbit_phase_radians)};
  }

  // --- Starting Terran fleet ---
  (void)add_ship(terrans, home_system, home_pos, "Freighter Alpha", "freighter_alpha");
  (void)add_ship(terrans, home_system, home_pos + Vec2{0.0, 0.8}, "Surveyor Beta", "surveyor_beta");
  (void)add_ship(terrans, home_system, home_pos + Vec2{0.0, -0.8}, "Escort Gamma", "escort_gamma");

  // --- Pirate presence ---
  // Keep pirates out of the home system when possible.
  const Vec2 pirate_pos = {80.0, 0.5};
  if (pirate_system != home_system) {
    (void)add_ship(pirates, pirate_system, pirate_pos, "Raider I", "pirate_raider");
    (void)add_ship(pirates, pirate_system, pirate_pos + Vec2{0.7, -0.3}, "Raider II", "pirate_raider");
  } else {
    (void)add_ship(pirates, pirate_system, home_pos + Vec2{10.0, 10.0}, "Raider I", "pirate_raider");
  }

  return s;
}

} // namespace nebula4x
