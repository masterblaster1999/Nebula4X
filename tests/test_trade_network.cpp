#include <cmath>
#include <iostream>
#include <string>

#include "nebula4x/core/simulation.h"
#include "nebula4x/core/trade_network.h"

#define N4X_ASSERT(cond, msg)                                                       \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::cerr << "ASSERT FAIL: " << (msg) << " (" << __FILE__ << ":" << __LINE__ \
                << ")\n";                                                         \
      return 1;                                                                     \
    }                                                                               \
  } while (0)

int test_trade_network() {
  using namespace nebula4x;

  // Minimal content: a handful of resources so TradeGoodKind classification works.
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
  Simulation sim(content, cfg);
  sim.new_game();

  GameState st;
  st.date = Date{0};

  // One region so the generator has something to reference.
  Region reg;
  reg.id = 1;
  reg.name = "Test Region";
  reg.theme = "Core";
  reg.mineral_richness_mult = 1.2;
  reg.volatile_richness_mult = 1.1;
  reg.pirate_risk = 0.2;
  reg.ruins_density = 0.1;
  st.regions[reg.id] = reg;

  // Three systems in a chain A <-> B <-> C.
  StarSystem a;
  a.id = 10;
  a.name = "A";
  a.region_id = reg.id;
  a.galaxy_pos = Vec2{-10.0, 0.0};
  st.systems[a.id] = a;

  StarSystem b;
  b.id = 11;
  b.name = "B";
  b.region_id = reg.id;
  b.galaxy_pos = Vec2{0.0, 0.0};
  st.systems[b.id] = b;

  StarSystem c;
  c.id = 12;
  c.name = "C";
  c.region_id = reg.id;
  c.galaxy_pos = Vec2{10.0, 0.0};
  st.systems[c.id] = c;

  // Bodies with deposits.
  Body a_body;
  a_body.id = 100;
  a_body.system_id = a.id;
  a_body.type = BodyType::Asteroid;
  a_body.mineral_deposits["Duranium"] = 5.0e6;
  st.bodies[a_body.id] = a_body;
  st.systems[a.id].bodies.push_back(a_body.id);

  Body b_body;
  b_body.id = 101;
  b_body.system_id = b.id;
  b_body.type = BodyType::Planet;
  b_body.surface_temp_k = 288.0;
  b_body.atmosphere_atm = 1.0;
  b_body.mass_earths = 1.0;
  b_body.radius_km = 6371.0;
  // Light deposits.
  b_body.mineral_deposits["Sorium"] = 2.0e5;
  st.bodies[b_body.id] = b_body;
  st.systems[b.id].bodies.push_back(b_body.id);

  Body c_body;
  c_body.id = 102;
  c_body.system_id = c.id;
  c_body.type = BodyType::Asteroid;
  c_body.mineral_deposits["Sorium"] = 4.0e6;
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

  sim.load_game(std::move(st));

  TradeNetworkOptions opt;
  opt.max_lanes = 128;
  opt.distance_exponent = 1.35;
  opt.include_uncolonized_markets = true;
  opt.include_colony_contributions = false;

  const TradeNetwork net1 = compute_trade_network(sim, opt);
  const TradeNetwork net2 = compute_trade_network(sim, opt);

  N4X_ASSERT(net1.nodes.size() == 3, "expected 3 nodes");
  N4X_ASSERT(net1.nodes.size() == net2.nodes.size(), "determinism: node count mismatch");
  N4X_ASSERT(net1.lanes.size() == net2.lanes.size(), "determinism: lane count mismatch");

  // Basic invariants: no self-lanes, finite volumes.
  for (const auto& lane : net1.lanes) {
    N4X_ASSERT(lane.from_system_id != lane.to_system_id, "lane must not self-loop");
    N4X_ASSERT(std::isfinite(lane.total_volume), "lane volume must be finite");
    N4X_ASSERT(lane.total_volume >= 0.0, "lane volume must be non-negative");
    N4X_ASSERT(lane.top_flows.size() <= 3, "top_flows clamped");
  }

  // Determinism check (strict): same ordering and same endpoints.
  for (std::size_t i = 0; i < net1.lanes.size(); ++i) {
    const auto& a1 = net1.lanes[i];
    const auto& a2 = net2.lanes[i];
    N4X_ASSERT(a1.from_system_id == a2.from_system_id, "lane from mismatch");
    N4X_ASSERT(a1.to_system_id == a2.to_system_id, "lane to mismatch");
    N4X_ASSERT(std::fabs(a1.total_volume - a2.total_volume) < 1e-9, "lane volume mismatch");
  }

  // Ensure at least one lane exists in this contrived chain.
  N4X_ASSERT(!net1.lanes.empty(), "expected at least one lane");

  return 0;
}
