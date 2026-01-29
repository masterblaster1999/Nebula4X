#include <cmath>
#include <iostream>
#include <string>

#include "nebula4x/core/security_planner.h"
#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(cond, msg)                                                       \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::cerr << "ASSERT FAIL: " << (msg) << " (" << __FILE__ << ":" << __LINE__ \
                << ")\n";                                                         \
      return 1;                                                                     \
    }                                                                               \
  } while (0)

int test_security_planner() {
  using namespace nebula4x;

  // Minimal content so deposits/categories are valid for trade network.
  ContentDB content;
  auto add_res = [&](const std::string& id, const std::string& cat, bool mineable) {
    ResourceDef r;
    r.id = id;
    r.name = id;
    r.category = cat;
    r.mineable = mineable;
    content.resources[id] = r;
  };
  add_res("Duranium", "metal", true);
  add_res("Sorium", "volatile", true);
  add_res("Corbomite", "exotic", true);
  add_res("Metals", "metal", false);
  add_res("Minerals", "mineral", false);
  add_res("Fuel", "fuel", false);
  add_res("Munitions", "munitions", false);

  SimConfig cfg;
  cfg.enable_trade_network_diplomacy_multipliers = false;
  Simulation sim(content, cfg);
  sim.new_game();

  GameState st;
  st.date = Date{0};

  // Two regions: low piracy and high piracy.
  Region core;
  core.id = 1;
  core.name = "Core";
  core.theme = "Core";
  core.pirate_risk = 0.05;
  st.regions[core.id] = core;

  Region fringe;
  fringe.id = 2;
  fringe.name = "Fringe";
  fringe.theme = "Fringe";
  fringe.pirate_risk = 0.80;
  st.regions[fringe.id] = fringe;

  // Systems A <-> B <-> C, where C sits in the high-piracy region.
  StarSystem a;
  a.id = 10;
  a.name = "A";
  a.region_id = core.id;
  a.galaxy_pos = Vec2{-10.0, 0.0};
  st.systems[a.id] = a;

  StarSystem b;
  b.id = 11;
  b.name = "B";
  b.region_id = core.id;
  b.galaxy_pos = Vec2{0.0, 0.0};
  st.systems[b.id] = b;

  StarSystem c;
  c.id = 12;
  c.name = "C";
  c.region_id = fringe.id;
  c.galaxy_pos = Vec2{10.0, 0.0};
  st.systems[c.id] = c;

  // Bodies with deposits.
  Body a_body;
  a_body.id = 100;
  a_body.system_id = a.id;
  a_body.type = BodyType::Planet;
  a_body.position_mkm = Vec2{-2.0, 0.0};
  a_body.surface_temp_k = 288.0;
  a_body.atmosphere_atm = 1.0;
  a_body.mass_earths = 1.0;
  a_body.radius_km = 6371.0;
  a_body.mineral_deposits["Duranium"] = 5.0e6;
  st.bodies[a_body.id] = a_body;
  st.systems[a.id].bodies.push_back(a_body.id);

  Body b_body;
  b_body.id = 101;
  b_body.system_id = b.id;
  b_body.type = BodyType::Asteroid;
  b_body.position_mkm = Vec2{0.0, 0.0};
  b_body.mineral_deposits["Corbomite"] = 2.0e6;
  st.bodies[b_body.id] = b_body;
  st.systems[b.id].bodies.push_back(b_body.id);

  Body c_body;
  c_body.id = 102;
  c_body.system_id = c.id;
  c_body.type = BodyType::Planet;
  c_body.position_mkm = Vec2{2.0, 0.0};
  c_body.surface_temp_k = 255.0;
  c_body.atmosphere_atm = 0.2;
  c_body.mass_earths = 0.9;
  c_body.radius_km = 6000.0;
  c_body.mineral_deposits["Sorium"] = 6.0e6;
  st.bodies[c_body.id] = c_body;
  st.systems[c.id].bodies.push_back(c_body.id);

  // Jump points A<->B and B<->C.
  JumpPoint jp_ab;
  jp_ab.id = 200;
  jp_ab.system_id = a.id;
  jp_ab.position_mkm = Vec2{0, 0};
  jp_ab.linked_jump_id = 201;
  st.jump_points[jp_ab.id] = jp_ab;
  st.systems[a.id].jump_points.push_back(jp_ab.id);

  JumpPoint jp_ba;
  jp_ba.id = 201;
  jp_ba.system_id = b.id;
  jp_ba.position_mkm = Vec2{0, 0};
  jp_ba.linked_jump_id = 200;
  st.jump_points[jp_ba.id] = jp_ba;
  st.systems[b.id].jump_points.push_back(jp_ba.id);

  JumpPoint jp_bc;
  jp_bc.id = 202;
  jp_bc.system_id = b.id;
  jp_bc.position_mkm = Vec2{0, 0};
  jp_bc.linked_jump_id = 203;
  st.jump_points[jp_bc.id] = jp_bc;
  st.systems[b.id].jump_points.push_back(jp_bc.id);

  JumpPoint jp_cb;
  jp_cb.id = 203;
  jp_cb.system_id = c.id;
  jp_cb.position_mkm = Vec2{0, 0};
  jp_cb.linked_jump_id = 202;
  st.jump_points[jp_cb.id] = jp_cb;
  st.systems[c.id].jump_points.push_back(jp_cb.id);

  // Player faction with discovery of the entire chain.
  Faction f;
  f.id = 1;
  f.name = "Player";
  f.control = FactionControl::Player;
  f.discovered_systems = {a.id, b.id, c.id};
  st.factions[f.id] = f;

  // Colonize A and C (endpoints). B is uncolonized transit.
  Colony ca;
  ca.id = 300;
  ca.name = "A-Prime";
  ca.faction_id = f.id;
  ca.body_id = a_body.id;
  ca.population_millions = 500.0;
  st.colonies[ca.id] = ca;

  Colony cc;
  cc.id = 301;
  cc.name = "C-Prime";
  cc.faction_id = f.id;
  cc.body_id = c_body.id;
  cc.population_millions = 500.0;
  st.colonies[cc.id] = cc;

  sim.load_game(std::move(st));

  SecurityPlannerOptions opt;
  opt.faction_id = f.id;
  opt.restrict_to_discovered = true;
  opt.require_own_colony_endpoints = true;
  opt.max_lanes = 64;
  opt.min_lane_volume = 0.0;
  opt.max_results = 16;

  const auto plan = compute_security_plan(sim, opt);
  N4X_ASSERT(plan.ok, "plan should be ok");
  N4X_ASSERT(!plan.top_systems.empty(), "expected at least one system result");

  // The high-piracy endpoint should dominate need.
  const Id top = plan.top_systems.front().system_id;
  N4X_ASSERT(top == c.id, "expected C (high piracy region) to be the top-need system");

  // Ensure we identified at least one corridor and one chokepoint edge.
  N4X_ASSERT(!plan.top_corridors.empty(), "expected corridors");
  N4X_ASSERT(!plan.top_chokepoints.empty(), "expected chokepoints");

  return 0;
}
