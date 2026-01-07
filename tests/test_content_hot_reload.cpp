#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "nebula4x/core/simulation.h"
#include "nebula4x/core/tech.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";      \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

static bool approx(double a, double b, double eps = 1e-9) {
  return std::abs(a - b) <= eps;
}

int test_content_hot_reload() {
  // Load a minimal content bundle (blueprints only) and create a minimal state.
  {
    std::vector<std::string> base_paths = {"tests/data/content_base.json"};
    auto content = nebula4x::load_content_db_from_files(base_paths);
    content.techs.clear();
    content.tech_source_paths.clear();

    nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});

    nebula4x::GameState s;

    // One system, one faction, two ships.
    nebula4x::StarSystem sys;
    sys.id = 1;
    sys.name = "Test System";
    sys.ships = {1, 2};
    s.systems[sys.id] = sys;

    nebula4x::Faction f;
    f.id = 1;
    f.name = "Test Faction";
    s.factions[f.id] = f;

    nebula4x::Ship sh1;
    sh1.id = 1;
    sh1.name = "Builtin";
    sh1.faction_id = 1;
    sh1.system_id = 1;
    sh1.design_id = "ship_test";
    s.ships[sh1.id] = sh1;

    nebula4x::Ship sh2;
    sh2.id = 2;
    sh2.name = "Custom";
    sh2.faction_id = 1;
    sh2.system_id = 1;
    sh2.design_id = "custom_ship";
    s.ships[sh2.id] = sh2;

    // One custom design that references engine_test.
    nebula4x::ShipDesign cd;
    cd.id = "custom_ship";
    cd.name = "Custom Ship";
    cd.role = nebula4x::ShipRole::Surveyor;
    cd.components = {"engine_test"};
    s.custom_designs[cd.id] = cd;

    s.next_id = 3;

    sim.load_game(std::move(s));

    const auto* d_builtin = sim.find_design("ship_test");
    N4X_ASSERT(d_builtin);
    N4X_ASSERT(approx(d_builtin->speed_km_s, 5.0));

    const auto* d_custom = sim.find_design("custom_ship");
    N4X_ASSERT(d_custom);
    N4X_ASSERT(approx(d_custom->speed_km_s, 5.0));

    N4X_ASSERT(sim.state().ships.find(1) != sim.state().ships.end());
    N4X_ASSERT(sim.state().ships.find(2) != sim.state().ships.end());
    N4X_ASSERT(approx(sim.state().ships.at(1).speed_km_s, 5.0));
    N4X_ASSERT(approx(sim.state().ships.at(2).speed_km_s, 5.0));

    // Reload content with an overlay that changes engine speed from 5 -> 9.
    auto new_content = nebula4x::load_content_db_from_files({"tests/data/content_base.json", "tests/data/content_mod.json"});
    new_content.techs.clear();
    new_content.tech_source_paths.clear();

    const auto res = sim.reload_content_db(std::move(new_content), true);
    N4X_ASSERT(res.ok);
    N4X_ASSERT(res.errors.empty());

    d_builtin = sim.find_design("ship_test");
    N4X_ASSERT(d_builtin);
    N4X_ASSERT(approx(d_builtin->speed_km_s, 9.0));

    d_custom = sim.find_design("custom_ship");
    N4X_ASSERT(d_custom);
    N4X_ASSERT(approx(d_custom->speed_km_s, 9.0));

    N4X_ASSERT(approx(sim.state().ships.at(1).speed_km_s, 9.0));
    N4X_ASSERT(approx(sim.state().ships.at(2).speed_km_s, 9.0));
  }

  return 0;
}
