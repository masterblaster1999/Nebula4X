#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "nebula4x/core/simulation.h"
#include "nebula4x/core/tech.h"

#define N4X_ASSERT(expr, msg) \
  do { \
    if (!(expr)) { \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << "): " << (msg) << "\n"; \
      return 1; \
    } \
  } while (0)

int test_piracy_suppression() {
  using namespace nebula4x;

  auto content_db = load_content_db_from_file("data/blueprints/starting_blueprints.json");
  content_db.techs = load_tech_db_from_file("data/tech/tech_tree.json");

  SimConfig cfg;
  cfg.enable_combat = false;
  cfg.enable_pirate_raids = false;  // keep the test deterministic
  cfg.enable_pirate_suppression = true;
  cfg.pirate_suppression_adjust_fraction_per_day = 1.0;  // immediate convergence for test
  cfg.pirate_suppression_power_scale = 20.0;

  Simulation sim(content_db, cfg);

  // Find the "Sol" system (fallback: first system in scenario).
  Id sol_id = kInvalidId;
  for (const auto& [sid, sys] : sim.state().systems) {
    if (sys.name == "Sol") {
      sol_id = sid;
      break;
    }
  }
  if (sol_id == kInvalidId) sol_id = sim.state().systems.begin()->first;

  // Create a region and assign Sol to it so we can test region suppression even
  // in the handcrafted Sol scenario (which normally has no regions).
  Region reg;
  reg.id = sim.state().next_id++;
  reg.name = "Test Region";
  reg.pirate_risk = 1.0;
  reg.pirate_suppression = 0.0;
  sim.state().regions[reg.id] = reg;
  sim.state().systems[sol_id].region_id = reg.id;

  // Find a non-pirate armed ship to patrol with.
  Id patrol_ship = kInvalidId;
  for (const auto& [sid, sh] : sim.state().ships) {
    const auto* d = sim.find_design(sh.design_id);
    if (!d) continue;
    const double weapons =
        std::max(0.0, d->weapon_damage) + std::max(0.0, d->missile_damage) +
        std::max(0.0, d->point_defense_damage);
    if (weapons > 0.0) {
      patrol_ship = sid;
      break;
    }
  }
  N4X_ASSERT(patrol_ship != kInvalidId, "Expected an armed ship in the Sol scenario");

  const Id fac_id = sim.state().ships.at(patrol_ship).faction_id;
  N4X_ASSERT(fac_id != kInvalidId, "Patrol ship has no owning faction");

  std::string error;
  const Id fleet_id = sim.create_fleet(fac_id, "Test Patrol Fleet", {patrol_ship}, &error);
  if (fleet_id == kInvalidId) {
    N4X_ASSERT(false, std::string("create_fleet failed: ") + error);
  }

  // Assign a patrol mission so the suppression tick will count this fleet.
  auto& fl = sim.state().fleets.at(fleet_id);
  fl.mission.type = FleetMissionType::PatrolSystem;
  fl.mission.patrol_system_id = sol_id;
  fl.mission.patrol_dwell_days = 1;

  sim.advance_days(1);

  const double s1 = sim.state().regions.at(reg.id).pirate_suppression;
  N4X_ASSERT(s1 > 0.01, "Expected piracy suppression to increase with an active patrol mission");
  N4X_ASSERT(s1 <= 1.0 + 1e-9, "Suppression should stay within [0,1]");

  // Remove the patrol mission and ensure suppression decays back to 0 when the
  // adjust fraction is 1.0 (target is 0.0 with no patrol power).
  fl.mission.type = FleetMissionType::None;
  sim.advance_days(1);

  const double s2 = sim.state().regions.at(reg.id).pirate_suppression;
  N4X_ASSERT(std::abs(s2) < 1e-9, "Expected suppression to decay to ~0 without patrol mission");

  return 0;
}
