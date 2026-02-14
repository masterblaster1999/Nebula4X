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

    // A design that includes at least one troop bay should expose a non-zero derived
    // troop_capacity. (Escort Delta is a pure combatant and may legitimately have
    // no troop capacity.)
    const auto it_design = content.designs.find("troop_transport_mk1");
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

  // --- Simulation: O2 terraforming should move oxygen toward its target and respect safety caps ---
  {
    nebula4x::ContentDB content = nebula4x::load_content_db_from_file("data/blueprints/starting_blueprints.json");

    nebula4x::SimConfig cfg;
    cfg.terraforming_split_points_between_axes = true;
    cfg.terraforming_o2_max_fraction_of_atm = 0.30;

    nebula4x::Simulation sim(std::move(content), cfg);

    const auto earth_id = find_colony_id(sim.state(), "Earth");
    N4X_ASSERT(earth_id != nebula4x::kInvalidId);

    nebula4x::Colony& earth = sim.state().colonies[earth_id];
    earth.installations["terraforming_plant"] += 1;

    const nebula4x::Id body_id = earth.body_id;
    nebula4x::Body& body = sim.state().bodies[body_id];

    body.surface_temp_k = 288.0;
    body.atmosphere_atm = 0.40;
    body.oxygen_atm = 0.05;

    // Invalid: O2 target without an atmosphere target.
    N4X_ASSERT(!sim.set_terraforming_target(body_id, 0.0, 0.0, 0.05));
    // Invalid: O2 exceeds safety cap (30% of 1 atm).
    N4X_ASSERT(!sim.set_terraforming_target(body_id, 0.0, 1.0, 0.50));

    // Valid target: raise atm and oxygen modestly (O2 below 30% cap).
    N4X_ASSERT(sim.set_terraforming_target(body_id, 0.0, 0.50, 0.10));

    const double o0 = body.oxygen_atm;
    sim.advance_days(5);

    const nebula4x::Body& after = sim.state().bodies[body_id];
    N4X_ASSERT(after.oxygen_atm >= o0 - 1e-9);
    N4X_ASSERT(after.oxygen_atm <= 0.10 + 1e-6);
    N4X_ASSERT(after.oxygen_atm <= after.atmosphere_atm + 1e-9);
    N4X_ASSERT(after.atmosphere_atm >= 0.40 - 1e-9);
    N4X_ASSERT(after.oxygen_atm <= after.atmosphere_atm * cfg.terraforming_o2_max_fraction_of_atm + 1e-6);
  }

  // --- Simulation: manual terraforming axis weights should control split allocation ---
  {
    nebula4x::ContentDB content = nebula4x::load_content_db_from_file(
        "data/blueprints/starting_blueprints.json");

    nebula4x::SimConfig cfg;
    cfg.terraforming_split_points_between_axes = true;
    cfg.terraforming_o2_max_fraction_of_atm = 0.30;

    nebula4x::Simulation sim(std::move(content), cfg);

    const auto earth_id = find_colony_id(sim.state(), "Earth");
    N4X_ASSERT(earth_id != nebula4x::kInvalidId);

    nebula4x::Colony& earth = sim.state().colonies[earth_id];
    earth.installations["terraforming_plant"] += 1;

    const nebula4x::Id body_id = earth.body_id;
    nebula4x::Body& body = sim.state().bodies[body_id];

    body.surface_temp_k = 250.0;
    body.atmosphere_atm = 0.40;
    body.oxygen_atm = 0.05;

    N4X_ASSERT(sim.set_terraforming_target(body_id, 252.0, 0.50, 0.10));

    // 1) O2-only allocation should change only oxygen.
    N4X_ASSERT(sim.set_terraforming_axis_weights(body_id, 0.0, 0.0, 1.0));
    const double t0 = body.surface_temp_k;
    const double a0 = body.atmosphere_atm;
    const double o0 = body.oxygen_atm;

    sim.advance_days(1);
    const nebula4x::Body& b1 = sim.state().bodies[body_id];
    N4X_ASSERT(b1.oxygen_atm > o0 + 1e-12);
    N4X_ASSERT(std::abs(b1.surface_temp_k - t0) <= 1e-9);
    N4X_ASSERT(std::abs(b1.atmosphere_atm - a0) <= 1e-12);

    // 2) Atmosphere-only allocation should change only atmosphere.
    N4X_ASSERT(sim.set_terraforming_axis_weights(body_id, 0.0, 1.0, 0.0));
    const double a1 = b1.atmosphere_atm;
    const double o1 = b1.oxygen_atm;

    sim.advance_days(1);
    const nebula4x::Body& b2 = sim.state().bodies[body_id];
    N4X_ASSERT(b2.atmosphere_atm > a1 + 1e-12);
    N4X_ASSERT(std::abs(b2.oxygen_atm - o1) <= 1e-9);
    N4X_ASSERT(std::abs(b2.surface_temp_k - t0) <= 1e-9);

    // 3) Clearing weights should fall back to delta-based allocation and advance temperature.
    N4X_ASSERT(sim.clear_terraforming_axis_weights(body_id));
    const double t_before = b2.surface_temp_k;
    sim.advance_days(1);
    const nebula4x::Body& b3 = sim.state().bodies[body_id];
    N4X_ASSERT(b3.surface_temp_k > t_before + 1e-12);
  }

  // --- Simulation: terraforming operational mineral costs should throttle output ---
  {
    nebula4x::SimConfig cfg;
    cfg.terraforming_duranium_per_point = 1.0;
    cfg.terraforming_neutronium_per_point = 0.0;
    cfg.terraforming_split_points_between_axes = true;
    cfg.enable_blockades = false; // Isolate mineral-throttling behavior from fleet movement.
    cfg.enable_colony_conditions = false;
    cfg.enable_colony_stability_output_scaling = false;

    double delta_temp_abundant = 0.0;
    double spent_d_abundant = 0.0;
    {
      nebula4x::ContentDB content = nebula4x::load_content_db_from_file(
          "data/blueprints/starting_blueprints.json");

      nebula4x::Simulation sim(std::move(content), cfg);

      const auto earth_id = find_colony_id(sim.state(), "Earth");
      N4X_ASSERT(earth_id != nebula4x::kInvalidId);

      nebula4x::Colony& earth = sim.state().colonies[earth_id];
      earth.faction_id = nebula4x::kInvalidId;
      earth.installations.clear();
      earth.installations["terraforming_plant"] = 10;

      const nebula4x::Id body_id = earth.body_id;
      nebula4x::Body& body = sim.state().bodies[body_id];

      body.surface_temp_k = 250.0;
      body.atmosphere_atm = 0.5;
      N4X_ASSERT(sim.set_terraforming_target(body_id, 260.0, 0.0));

      const double initial_d = 1.0e6;
      earth.minerals["Duranium"] = initial_d;
      const double t0 = body.surface_temp_k;

      sim.advance_days(1);

      const nebula4x::Body& after = sim.state().bodies[body_id];
      const nebula4x::Colony& after_col = sim.state().colonies[earth_id];

      delta_temp_abundant = after.surface_temp_k - t0;
      spent_d_abundant = initial_d - after_col.minerals.at("Duranium");
      N4X_ASSERT(delta_temp_abundant > 1e-12);
      N4X_ASSERT(spent_d_abundant > 1e-6);
    }

    // Run an equivalent setup with constrained Duranium and verify lower progress.
    {
      nebula4x::ContentDB content = nebula4x::load_content_db_from_file(
          "data/blueprints/starting_blueprints.json");

      nebula4x::Simulation sim(std::move(content), cfg);

      const auto earth_id = find_colony_id(sim.state(), "Earth");
      N4X_ASSERT(earth_id != nebula4x::kInvalidId);

      nebula4x::Colony& earth = sim.state().colonies[earth_id];
      earth.faction_id = nebula4x::kInvalidId;
      earth.installations.clear();
      earth.installations["terraforming_plant"] = 10;

      const nebula4x::Id body_id = earth.body_id;
      nebula4x::Body& body = sim.state().bodies[body_id];

      body.surface_temp_k = 250.0;
      body.atmosphere_atm = 0.5;
      N4X_ASSERT(sim.set_terraforming_target(body_id, 260.0, 0.0));

      const double initial_d = std::max(1e-6, 0.5 * spent_d_abundant);
      earth.minerals["Duranium"] = initial_d;
      const double t0 = body.surface_temp_k;

      sim.advance_days(1);

      const nebula4x::Body& after = sim.state().bodies[body_id];
      const nebula4x::Colony& after_col = sim.state().colonies[earth_id];

      const double delta_temp_scarce = after.surface_temp_k - t0;
      const double spent_d_scarce = initial_d - after_col.minerals.at("Duranium");

      N4X_ASSERT(delta_temp_scarce > 1e-12);
      N4X_ASSERT(spent_d_scarce > 1e-6);
      N4X_ASSERT(spent_d_scarce < spent_d_abundant);
      N4X_ASSERT(delta_temp_scarce < delta_temp_abundant);
    }
  }

  // --- Simulation: terraforming mass scaling should make small bodies easier ---
  {
    nebula4x::ContentDB content = nebula4x::load_content_db_from_file(
        "data/blueprints/starting_blueprints.json");

    nebula4x::SimConfig cfg;
    cfg.terraforming_scale_with_body_mass = true;
    cfg.terraforming_min_mass_earths = 0.10;
    cfg.terraforming_mass_scaling_exponent = 1.0;
    cfg.terraforming_split_points_between_axes = true;

    nebula4x::Simulation sim(std::move(content), cfg);

    const auto earth_id = find_colony_id(sim.state(), "Earth");
    N4X_ASSERT(earth_id != nebula4x::kInvalidId);

    nebula4x::Colony& earth = sim.state().colonies[earth_id];
    earth.installations["terraforming_plant"] += 1;

    const nebula4x::Id body_id = earth.body_id;
    nebula4x::Body& body = sim.state().bodies[body_id];

    // Fake a small body.
    body.mass_earths = 0.10;
    body.surface_temp_k = 250.0;
    body.atmosphere_atm = 0.5;

    N4X_ASSERT(sim.set_terraforming_target(body_id, 255.0, 0.0));

    // With mass scaling: scale = 1 / 0.1 = 10; dT_per_point = 1.0 K.
    // 1 plant => 8 pts/day => max delta = 8K/day, so we should hit the 5K target in a single day.
    sim.advance_days(1);
    const nebula4x::Body& after = sim.state().bodies[body_id];
    N4X_ASSERT(std::abs(after.surface_temp_k - 255.0) <= 1e-6);
  }

  return 0;
}
