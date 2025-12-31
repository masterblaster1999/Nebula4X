#include <cmath>
#include <iostream>
#include <string>

#include "nebula4x/core/simulation.h"
#include "nebula4x/core/tech.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";       \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

namespace {

nebula4x::Id find_colony_id(const nebula4x::GameState& st, const std::string& name) {
  for (const auto& [cid, c] : st.colonies) {
    if (c.name == name) return cid;
  }
  return nebula4x::kInvalidId;
}

} // namespace

int test_ground_ops() {
  // --- Content parsing: troop bays + ground/terraform installations ---
  {
    nebula4x::ContentDB content = nebula4x::load_content_db_from_file(
        "data/blueprints/starting_blueprints.json");

    const auto it_tb = content.components.find("troop_bay_mk1");
    N4X_ASSERT(it_tb != content.components.end());
    N4X_ASSERT(it_tb->second.type == nebula4x::ComponentType::TroopBay);
    N4X_ASSERT(it_tb->second.troop_capacity > 0.0);

    const auto it_design = content.designs.find("escort_delta");
    N4X_ASSERT(it_design != content.designs.end());
    N4X_ASSERT(it_design->second.troop_capacity >= it_tb->second.troop_capacity - 1e-9);

    const auto it_train = content.installations.find("training_facility");
    N4X_ASSERT(it_train != content.installations.end());
    N4X_ASSERT(it_train->second.troop_training_points_per_day > 0.0);

    const auto it_tf = content.installations.find("terraforming_plant");
    N4X_ASSERT(it_tf != content.installations.end());
    N4X_ASSERT(it_tf->second.terraforming_points_per_day > 0.0);

    const auto it_fort = content.installations.find("planetary_fortress");
    N4X_ASSERT(it_fort != content.installations.end());
    N4X_ASSERT(it_fort->second.fortification_points > 0.0);
  }

  // --- Simulation: training queue should convert into ground forces ---
  {
    nebula4x::ContentDB content = nebula4x::load_content_db_from_file(
        "data/blueprints/starting_blueprints.json");

    nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});

    const auto earth_id = find_colony_id(sim.state(), "Earth");
    N4X_ASSERT(earth_id != nebula4x::kInvalidId);

    nebula4x::Colony& earth = sim.state().colonies[earth_id];

    // Ensure we have at least one training facility.
    earth.installations["training_facility"] += 1;

    // Ensure minerals exist (training can optionally consume minerals depending on config).
    earth.minerals["Duranium"] = 1.0e6;
    earth.minerals["Neutronium"] = 1.0e6;

    earth.troop_training_queue = 100.0;
    const double before_gf = earth.ground_forces;

    sim.advance_days(1);

    const nebula4x::Colony& after = sim.state().colonies[earth_id];
    N4X_ASSERT(after.ground_forces > before_gf);
    N4X_ASSERT(after.troop_training_queue < 100.0);
  }

  // --- Simulation: terraforming should move a body toward its target ---
  {
    nebula4x::ContentDB content = nebula4x::load_content_db_from_file(
        "data/blueprints/starting_blueprints.json");

    nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});

    const auto earth_id = find_colony_id(sim.state(), "Earth");
    N4X_ASSERT(earth_id != nebula4x::kInvalidId);

    nebula4x::Colony& earth = sim.state().colonies[earth_id];
    earth.installations["terraforming_plant"] += 1;

    const nebula4x::Id body_id = earth.body_id;
    nebula4x::Body& body = sim.state().bodies[body_id];

    // Force the body away from its likely default values.
    body.surface_temp_k = 250.0;
    body.atmosphere_atm = 0.5;

    // Target modest deltas so we can observe movement.
    N4X_ASSERT(sim.set_terraforming_target(body_id, 252.0, 0.52));

    const double t0 = body.surface_temp_k;
    const double a0 = body.atmosphere_atm;

    sim.advance_days(2);

    const nebula4x::Body& after = sim.state().bodies[body_id];
    N4X_ASSERT(after.surface_temp_k >= t0);
    N4X_ASSERT(after.atmosphere_atm >= a0);

    // Should not overshoot beyond the targets.
    N4X_ASSERT(after.surface_temp_k <= 252.0 + 1e-6);
    N4X_ASSERT(after.atmosphere_atm <= 0.52 + 1e-6);
  }

  return 0;
}
