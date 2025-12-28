#include <cmath>
#include <iostream>

#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";           \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

namespace {

nebula4x::ContentDB minimal_content() {
  nebula4x::ContentDB c;

  // Minimal installation defs referenced by the built-in Sol scenario.
  {
    nebula4x::InstallationDef d;
    d.id = "automated_mine";
    d.name = "Automated Mine";
    c.installations[d.id] = d;
  }
  {
    nebula4x::InstallationDef d;
    d.id = "construction_factory";
    d.name = "Construction Factory";
    d.construction_points_per_day = 0.0;
    c.installations[d.id] = d;
  }
  {
    nebula4x::InstallationDef d;
    d.id = "shipyard";
    d.name = "Shipyard";
    d.build_rate_tons_per_day = 0.0;
    c.installations[d.id] = d;
  }
  {
    nebula4x::InstallationDef d;
    d.id = "research_lab";
    d.name = "Research Lab";
    d.research_points_per_day = 0.0;
    c.installations[d.id] = d;
  }
  {
    nebula4x::InstallationDef d;
    d.id = "sensor_station";
    d.name = "Sensor Station";
    d.sensor_range_mkm = 0.0;
    c.installations[d.id] = d;
  }

  // Minimal ship designs referenced by the built-in Sol scenario.
  auto add_design = [&](const std::string& id) {
    nebula4x::ShipDesign d;
    d.id = id;
    d.name = id;
    d.max_hp = 100.0;
    d.speed_km_s = 0.0;
    c.designs[d.id] = d;
  };
  add_design("freighter_alpha");
  add_design("surveyor_beta");
  add_design("escort_gamma");
  add_design("pirate_raider");

  return c;
}

nebula4x::Id find_colony_by_name(const nebula4x::GameState& s, const std::string& name) {
  for (const auto& [id, c] : s.colonies) {
    if (c.name == name) return id;
  }
  return nebula4x::kInvalidId;
}

} // namespace

int test_population_growth() {
  nebula4x::SimConfig cfg;

  // Choose a rate that yields a clean daily multiplier:
  // per_day = rate/365.25 => 0.36525/365.25 = 0.001 (= +0.1% per day).
  cfg.population_growth_rate_per_year = 0.36525;

  nebula4x::Simulation sim(minimal_content(), cfg);

  const auto earth_id = find_colony_by_name(sim.state(), "Earth");
  N4X_ASSERT(earth_id != nebula4x::kInvalidId);

  // Use a stable, test-controlled starting population.
  sim.state().colonies[earth_id].population_millions = 1000.0;
  const double initial = sim.state().colonies[earth_id].population_millions;

  sim.advance_days(1);

  const double expected = initial * (1.0 + cfg.population_growth_rate_per_year / 365.25);
  const double got = sim.state().colonies[earth_id].population_millions;

  N4X_ASSERT(std::fabs(got - expected) < 1e-9);
  return 0;
}
