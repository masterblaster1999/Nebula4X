#include <cmath>
#include <iostream>
#include <string>

#include "nebula4x/core/invasion_planner.h"
#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(cond, msg)                                                       \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::cerr << "ASSERT FAIL: " << (msg) << " (" << __FILE__ << ":" << __LINE__ \
                << ")\n";                                                         \
      return 1;                                                                     \
    }                                                                               \
  } while (0)

int test_invasion_planner() {
  using namespace nebula4x;

  ContentDB content;

  // Minimal defensive installations: forts + artillery.
  InstallationDef fort;
  fort.id = "Fort";
  fort.name = "Fort";
  fort.fortification_points = 10.0;
  content.installations[fort.id] = fort;

  InstallationDef gun;
  gun.id = "Gun";
  gun.name = "Gun";
  gun.weapon_damage = 5.0;
  gun.weapon_range_mkm = 1000.0;
  content.installations[gun.id] = gun;

  SimConfig cfg;
  Simulation sim(content, cfg);

  GameState st;
  st.date = Date{0};

  // Single system for simple ETA computations.
  StarSystem sys;
  sys.id = 10;
  sys.name = "TestSys";
  sys.galaxy_pos = Vec2{0.0, 0.0};
  st.systems[sys.id] = sys;

  auto add_body = [&](Id id, Vec2 pos) {
    Body b;
    b.id = id;
    b.system_id = sys.id;
    b.type = BodyType::Planet;
    b.position_mkm = pos;
    b.surface_temp_k = 288.0;
    b.atmosphere_atm = 1.0;
    b.mass_earths = 1.0;
    b.radius_km = 6371.0;
    st.bodies[b.id] = b;
    st.systems[sys.id].bodies.push_back(b.id);
    return b.id;
  };

  const Id body_stage_near = add_body(101, Vec2{0.0, 0.0});
  const Id body_stage_far = add_body(102, Vec2{1000.0, 0.0});
  const Id body_target = add_body(100, Vec2{10.0, 0.0});

  // Factions.
  Faction attacker;
  attacker.id = 1;
  attacker.name = "Attacker";
  attacker.control = FactionControl::Player;
  attacker.discovered_systems = {sys.id};
  st.factions[attacker.id] = attacker;

  Faction defender;
  defender.id = 2;
  defender.name = "Defender";
  defender.control = FactionControl::AI_Passive;
  defender.discovered_systems = {sys.id};
  st.factions[defender.id] = defender;

  // Staging colonies (equal surplus; near should win on ETA).
  Colony c_near;
  c_near.id = 201;
  c_near.name = "StageNear";
  c_near.faction_id = attacker.id;
  c_near.body_id = body_stage_near;
  c_near.ground_forces = 200.0;
  c_near.garrison_target_strength = 100.0;
  st.colonies[c_near.id] = c_near;

  Colony c_far;
  c_far.id = 202;
  c_far.name = "StageFar";
  c_far.faction_id = attacker.id;
  c_far.body_id = body_stage_far;
  c_far.ground_forces = 200.0;
  c_far.garrison_target_strength = 100.0;
  st.colonies[c_far.id] = c_far;

  // Target colony.
  Colony tgt;
  tgt.id = 200;
  tgt.name = "Target";
  tgt.faction_id = defender.id;
  tgt.body_id = body_target;
  tgt.ground_forces = 100.0;
  tgt.installations["Fort"] = 5;  // 50 fort points.
  tgt.installations["Gun"] = 3;   // 15 damage/day.
  st.colonies[tgt.id] = tgt;

  sim.load_game(std::move(st));

  InvasionPlannerOptions opt;
  opt.attacker_faction_id = attacker.id;
  opt.restrict_to_discovered = true;
  opt.start_system_id = sys.id;
  opt.start_pos_mkm = Vec2{0.0, 0.0};
  opt.planning_speed_km_s = 1000.0;
  opt.max_staging_options = 8;

  const double margin = 1.20;
  const auto res = analyze_invasion_target(sim, tgt.id, opt, margin, /*attacker_strength_for_forecast=*/10.0);
  N4X_ASSERT(res.ok, res.message);

  // Defender snapshot.
  N4X_ASSERT(std::abs(res.target.defender_strength - 100.0) < 1e-6, "defender_strength should match colony ground_forces");
  N4X_ASSERT(std::abs(res.target.forts_total - 50.0) < 1e-6, "forts_total should sum fortification installations");
  N4X_ASSERT(std::abs(res.target.defender_artillery_weapon_damage_per_day - 15.0) < 1e-6, "artillery should sum weapon_damage of installations");

  // Required strength should exceed the defender due to forts/artillery/margin.
  N4X_ASSERT(res.target.required_attacker_strength > 100.0, "expected required attacker strength > defender strength");
  N4X_ASSERT(res.target.forecast_at_required.winner == GroundBattleWinner::Attacker,
             "forecast at required strength should predict attacker win");

  N4X_ASSERT(res.target.has_attacker_strength_forecast, "expected attacker strength forecast present");
  N4X_ASSERT(res.target.forecast_at_attacker_strength.winner != GroundBattleWinner::Attacker,
             "forecast at tiny attacker strength should not predict attacker win");

  // Staging: both have equal surplus; near should win on ETA.
  N4X_ASSERT(res.staging_options.size() >= 2, "expected at least 2 staging options");
  N4X_ASSERT(res.staging_options.front().colony_id == c_near.id, "near staging colony should be ranked first");

  // Now inject an active ground battle with fort damage and reduced defender strength.
  GroundBattle gb;
  gb.colony_id = tgt.id;
  gb.attacker_faction_id = attacker.id;
  gb.defender_faction_id = defender.id;
  gb.attacker_strength = 0.0;
  gb.defender_strength = 80.0;
  gb.fortification_damage_points = 25.0;  // half the forts suppressed.
  sim.state().ground_battles[tgt.id] = gb;

  const auto res2 = analyze_invasion_target(sim, tgt.id, opt, margin);
  N4X_ASSERT(res2.ok, res2.message);
  N4X_ASSERT(std::abs(res2.target.defender_strength - 80.0) < 1e-6, "defender_strength should use active battle snapshot");
  N4X_ASSERT(res2.target.forts_effective < res2.target.forts_total, "forts_effective should account for fort damage");

  return 0;
}
