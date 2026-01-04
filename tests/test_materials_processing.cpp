#include <cmath>
#include <iostream>
#include <string>

#include "nebula4x/core/simulation.h"
#include "nebula4x/core/tech.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";           \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

int test_materials_processing() {
  using namespace nebula4x;

  ContentDB content = load_content_db_from_file("data/blueprints/starting_blueprints.json");
  Simulation sim(std::move(content), SimConfig{});

  GameState s;
  s.save_version = GameState{}.save_version;
  s.date = Date::from_ymd(2200, 1, 1);

  const Id sys_id = 1;
  {
    StarSystem sys;
    sys.id = sys_id;
    sys.name = "Test System";
    s.systems[sys.id] = sys;
  }

  const Id body_id = 2;
  {
    Body b;
    b.id = body_id;
    b.name = "Test Planet";
    b.type = BodyType::Planet;
    b.system_id = sys_id;
    b.orbit_radius_mkm = 0.0;
    b.orbit_period_days = 1.0;
    b.orbit_phase_radians = 0.0;
    s.bodies[b.id] = b;
  }

  const Id faction_id = 3;
  {
    Faction f;
    f.id = faction_id;
    f.name = "Test Faction";
    f.control = FactionControl::Player;
    s.factions[f.id] = f;
  }

  const Id colony_id = 4;
  {
    Colony c;
    c.id = colony_id;
    c.name = "Test Colony";
    c.faction_id = faction_id;
    c.body_id = body_id;
    c.population_millions = 0.0;
    c.installations = {{"metal_smelter", 1}, {"mineral_processor", 1}};
    c.minerals = {
        {"Duranium", 100.0},   {"Tritanium", 50.0},  {"Boronide", 50.0},
        {"Corundium", 50.0},  {"Gallicite", 50.0},  {"Uridium", 50.0},
        {"Mercassium", 50.0},
    };
    s.colonies[c.id] = c;
  }

  s.next_id = 100;
  sim.load_game(std::move(s));

  auto read = [&](const std::string& rid) -> double {
    auto itc = sim.state().colonies.find(colony_id);
    if (itc == sim.state().colonies.end()) return 0.0;
    const Colony& col = itc->second;
    auto it = col.minerals.find(rid);
    return (it == col.minerals.end()) ? 0.0 : it->second;
  };

  sim.advance_days(1);

  N4X_ASSERT(std::abs(read("Metals") - 100.0) < 1e-9);
  N4X_ASSERT(std::abs(read("Minerals") - 100.0) < 1e-9);

  N4X_ASSERT(std::abs(read("Duranium") - 98.0) < 1e-9);
  N4X_ASSERT(std::abs(read("Tritanium") - 49.5) < 1e-9);
  N4X_ASSERT(std::abs(read("Boronide") - 49.5) < 1e-9);

  N4X_ASSERT(std::abs(read("Corundium") - 48.5) < 1e-9);
  N4X_ASSERT(std::abs(read("Gallicite") - 49.0) < 1e-9);
  N4X_ASSERT(std::abs(read("Uridium") - 49.5) < 1e-9);
  N4X_ASSERT(std::abs(read("Mercassium") - 49.5) < 1e-9);

  // Input-limited behavior: if a required input is missing, the installation should not produce.
  const double metals_before = read("Metals");
  {
    Colony& c = sim.state().colonies.at(colony_id);
    c.minerals["Tritanium"] = 0.0;  // required by metal_smelter
    c.minerals["Duranium"] = 1000.0;
    c.minerals["Boronide"] = 1000.0;
  }

  sim.advance_days(1);
  N4X_ASSERT(std::abs(read("Metals") - metals_before) < 1e-9);

  return 0;
}
