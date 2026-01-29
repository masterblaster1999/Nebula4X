#include <cmath>
#include <iostream>
#include <string>

#include "nebula4x/core/maintenance_planner.h"
#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(cond, msg)                                                       \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::cerr << "ASSERT FAIL: " << (msg) << " (" << __FILE__ << ":" << __LINE__ \
                << ")\n";                                                         \
      return 1;                                                                     \
    }                                                                               \
  } while (0)

int test_maintenance_planner() {
  using namespace nebula4x;

  ContentDB content;

  ShipDesign d;
  d.id = "test";
  d.name = "Test";
  d.role = ShipRole::Combatant;
  d.mass_tons = 100.0;
  d.max_hp = 100.0;
  d.speed_km_s = 100.0;
  content.designs[d.id] = d;

  InstallationDef yard;
  yard.id = "shipyard";
  yard.name = "Shipyard";
  content.installations[yard.id] = yard;

  SimConfig cfg;
  cfg.docking_range_mkm = 0.01;
  cfg.enable_ship_maintenance = true;
  cfg.ship_maintenance_resource_id = "MSP";
  cfg.ship_maintenance_tons_per_day_per_mass_ton = 0.01;  // 1 ton/day @ 100t
  cfg.ship_maintenance_recovery_per_day = 0.10;           // +10% per day when supplied
  cfg.ship_maintenance_breakdown_start_fraction = 0.60;   // below this is "critical"

  Simulation sim(content, cfg);
  sim.new_game();

  GameState st = sim.state();
  N4X_ASSERT(!st.factions.empty(), "new_game should create a faction");
  const Faction f = st.factions.begin()->second;

  StarSystem sys;
  sys.id = 1;
  sys.name = "Sol";
  sys.galaxy_pos = Vec2{0.0, 0.0};
  st.systems[sys.id] = sys;

  Body a_body;
  a_body.id = 10;
  a_body.name = "A";
  a_body.system_id = sys.id;
  a_body.position_mkm = Vec2{0.0, 0.0};
  st.bodies[a_body.id] = a_body;

  Body b_body;
  b_body.id = 11;
  b_body.name = "B";
  b_body.system_id = sys.id;
  b_body.position_mkm = Vec2{20.0, 0.0};
  st.bodies[b_body.id] = b_body;

  // Colony A: lots of MSP, but no shipyard.
  Colony ca;
  ca.id = 20;
  ca.name = "ColonyA";
  ca.faction_id = f.id;
  ca.body_id = a_body.id;
  ca.minerals["MSP"] = 20.0;
  st.colonies[ca.id] = ca;

  // Colony B: shipyard, but only enough MSP for one ship to recover.
  Colony cb;
  cb.id = 21;
  cb.name = "ColonyB";
  cb.faction_id = f.id;
  cb.body_id = b_body.id;
  cb.minerals["MSP"] = 5.0;
  cb.installations["shipyard"] = 1;
  st.colonies[cb.id] = cb;

  // Two ships in critical condition near A.
  Ship s1;
  s1.id = 100;
  s1.name = "S1";
  s1.faction_id = f.id;
  s1.system_id = sys.id;
  s1.position_mkm = Vec2{1.0, 0.0};
  s1.design_id = d.id;
  s1.speed_km_s = d.speed_km_s;
  s1.maintenance_condition = 0.40;  // critical
  st.ships[s1.id] = s1;

  Ship s2 = s1;
  s2.id = 101;
  s2.name = "S2";
  s2.position_mkm = Vec2{1.2, 0.0};
  st.ships[s2.id] = s2;

  sim.load_game(st);

  MaintenancePlannerOptions opt;
  opt.restrict_to_discovered = false;
  opt.include_trade_partner_colonies = false;
  opt.prefer_shipyards = true;
  opt.require_shipyard_when_critical = true;
  opt.require_supplies_available = true;
  opt.reserve_buffer_fraction = 0.0;
  opt.threshold_fraction = 0.75;
  opt.target_fraction = 0.90;

  const auto plan = compute_maintenance_plan(sim, f.id, opt);
  N4X_ASSERT(plan.ok, "plan should be ok");
  N4X_ASSERT(plan.assignments.size() == 2, "should plan both ships");

  auto find_asg = [&](Id ship_id) -> const MaintenanceAssignment* {
    for (const auto& a : plan.assignments) {
      if (a.ship_id == ship_id) return &a;
    }
    return nullptr;
  };

  const auto* a1 = find_asg(s1.id);
  const auto* a2 = find_asg(s2.id);
  N4X_ASSERT(a1 && a2, "assignments should contain both ships");

  // Each ship needs (0.9-0.4)/0.1 = 5 days, at 1 ton/day => 5 MSP.
  N4X_ASSERT(std::abs(a1->supplies_needed_total_tons - 5.0) < 1e-6, "s1 supplies should be 5");
  N4X_ASSERT(std::abs(a2->supplies_needed_total_tons - 5.0) < 1e-6, "s2 supplies should be 5");

  // First ship should take the shipyard colony B (critical + shipyard preference).
  N4X_ASSERT(a1->target_colony_id == cb.id, "s1 should be assigned to shipyard colony B");

  // Second ship can't fit at B due to MSP limit, so it should fall back to A.
  N4X_ASSERT(a2->target_colony_id == ca.id, "s2 should fall back to colony A due to MSP limit");

  return 0;
}

int main() {
  int rc = 0;
  rc |= test_maintenance_planner();
  if (rc == 0) {
    std::cout << "test_maintenance_planner: ok\n";
  }
  return rc;
}
