#include <cmath>
#include <iostream>
#include <string>

#include "nebula4x/core/repair_planner.h"
#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(cond, msg)                                                       \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::cerr << "ASSERT FAIL: " << (msg) << " (" << __FILE__ << ":" << __LINE__ \
                << ")\n";                                                         \
      return 1;                                                                     \
    }                                                                               \
  } while (0)

int test_repair_planner() {
  using namespace nebula4x;

  ContentDB content;

  // Minimal ship design (100 HP, 100 km/s).
  ShipDesign d;
  d.id = "test";
  d.name = "Test";
  d.role = ShipRole::Combatant;
  d.mass_tons = 100.0;
  d.max_hp = 100.0;
  d.speed_km_s = 100.0;
  content.designs[d.id] = d;

  // Minimal shipyard installation (only the id matters for counting).
  InstallationDef yard;
  yard.id = "shipyard";
  yard.name = "Shipyard";
  content.installations[yard.id] = yard;

  SimConfig cfg;
  cfg.docking_range_mkm = 0.01;
  cfg.repair_hp_per_day_per_shipyard = 10.0;
  cfg.ship_subsystem_repair_hp_equiv_per_integrity = 0.0;  // ignore subsystems in this test
  cfg.enable_blockades = false;

  Simulation sim(content, cfg);
  sim.new_game();

  GameState st = sim.state();
  N4X_ASSERT(!st.factions.empty(), "new_game should create a faction");
  const Faction f = st.factions.begin()->second;

  // One system.
  StarSystem sys;
  sys.id = 1;
  sys.name = "Sol";
  sys.galaxy_pos = Vec2{0.0, 0.0};
  st.systems[sys.id] = sys;

  // Two bodies separated in the same system.
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

  // Colony A: 1 shipyard (10 HP/day).
  Colony ca;
  ca.id = 20;
  ca.name = "ColonyA";
  ca.faction_id = f.id;
  ca.body_id = a_body.id;
  ca.installations["shipyard"] = 1;
  st.colonies[ca.id] = ca;

  // Colony B: 2 shipyards (20 HP/day).
  Colony cb;
  cb.id = 21;
  cb.name = "ColonyB";
  cb.faction_id = f.id;
  cb.body_id = b_body.id;
  cb.installations["shipyard"] = 2;
  st.colonies[cb.id] = cb;

  // Damaged ship near A (will travel farther to B but repairs much faster there).
  Ship big;
  big.id = 100;
  big.name = "Big";
  big.faction_id = f.id;
  big.system_id = sys.id;
  big.position_mkm = Vec2{1.0, 0.0};
  big.design_id = d.id;
  big.speed_km_s = d.speed_km_s;
  big.hp = 0.0;  // missing 100
  big.repair_priority = RepairPriority::Normal;
  st.ships[big.id] = big;

  // Lightly damaged ship near B.
  Ship small;
  small.id = 101;
  small.name = "Small";
  small.faction_id = f.id;
  small.system_id = sys.id;
  small.position_mkm = Vec2{21.0, 0.0};
  small.design_id = d.id;
  small.speed_km_s = d.speed_km_s;
  small.hp = 90.0;  // missing 10
  small.repair_priority = RepairPriority::Normal;
  st.ships[small.id] = small;

  sim.load_game(st);

  RepairPlannerOptions opt;
  opt.restrict_to_discovered = false;
  opt.include_subsystem_repairs = false;
  opt.include_trade_partner_yards = false;

  const auto plan = compute_repair_plan(sim, f.id, opt);
  N4X_ASSERT(plan.ok, "plan should be ok");
  N4X_ASSERT(plan.assignments.size() == 2, "should plan both damaged ships");

  auto find_asg = [&](Id ship_id) -> const RepairAssignment* {
    for (const auto& a : plan.assignments) {
      if (a.ship_id == ship_id) return &a;
    }
    return nullptr;
  };

  const auto* a_big = find_asg(big.id);
  const auto* a_small = find_asg(small.id);
  N4X_ASSERT(a_big && a_small, "assignments should contain both ships");

  // Both ships should be assigned to ColonyB due to higher capacity and release-aware scheduling.
  N4X_ASSERT(a_big->target_colony_id == cb.id, "big ship should pick the faster yard (B)");
  N4X_ASSERT(a_small->target_colony_id == cb.id, "small ship should still pick B despite big ship backlog");

  // Small ship should finish first because it arrives earlier.
  N4X_ASSERT(a_small->finish_repair_days < a_big->finish_repair_days, "small ship should complete sooner");

  return 0;
}
