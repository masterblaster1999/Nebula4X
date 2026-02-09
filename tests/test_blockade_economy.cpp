#include <cmath>
#include <iostream>
#include <string>

#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(cond, msg)                                                       \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::cerr << "ASSERT FAIL: " << (msg) << " (" << __FILE__ << ":" << __LINE__ \
                << ")\n";                                                         \
      return 1;                                                                     \
    }                                                                               \
  } while (0)

namespace {

using namespace nebula4x;

ContentDB make_content() {
  ContentDB content;

  InstallationDef factory;
  factory.id = "factory";
  factory.name = "Factory";
  factory.produces_per_day["Goods"] = 100.0;
  content.installations[factory.id] = factory;

  InstallationDef lab;
  lab.id = "lab";
  lab.name = "Research Lab";
  lab.research_points_per_day = 100.0;
  content.installations[lab.id] = lab;

  InstallationDef cf;
  cf.id = "construction_facility";
  cf.name = "Construction Facility";
  cf.construction_points_per_day = 100.0;
  content.installations[cf.id] = cf;

  InstallationDef yard;
  yard.id = "shipyard";
  yard.name = "Shipyard";
  yard.build_rate_tons_per_day = 100.0;
  content.installations[yard.id] = yard;

  ShipDesign frigate;
  frigate.id = "test_frigate";
  frigate.name = "Test Frigate";
  frigate.role = ShipRole::Combatant;
  frigate.mass_tons = 200.0;
  frigate.max_hp = 100.0;
  frigate.speed_km_s = 100.0;
  content.designs[frigate.id] = frigate;

  ShipDesign raider;
  raider.id = "hostile_raider";
  raider.name = "Hostile Raider";
  raider.role = ShipRole::Combatant;
  raider.mass_tons = 100.0;
  raider.max_hp = 100.0;
  raider.speed_km_s = 100.0;
  raider.weapon_damage = 10.0;
  content.designs[raider.id] = raider;

  return content;
}

SimConfig make_cfg() {
  SimConfig cfg;
  cfg.enable_combat = false;

  // Isolate blockade effects: disable additional output scaling layers.
  cfg.enable_trade_prosperity = false;
  cfg.enable_colony_conditions = false;
  cfg.enable_colony_events = false;
  cfg.enable_colony_stability_output_scaling = false;

  cfg.population_growth_rate_per_year = 0.0;

  cfg.enable_blockades = true;

  // Make blockade math deterministic and easy to validate.
  cfg.blockade_base_resistance_power = 0.0;
  cfg.blockade_max_output_penalty = 0.50;
  cfg.blockade_radius_mkm = 1000.0;

  return cfg;
}

GameState make_state(bool blockaded) {
  GameState st;
  st.save_version = GameState{}.save_version;

  Faction player;
  player.id = 1;
  player.name = "Player";
  player.control = FactionControl::Player;
  st.factions[player.id] = player;

  Faction enemy;
  enemy.id = 2;
  enemy.name = "Enemy";
  enemy.control = FactionControl::AI_Passive;
  st.factions[enemy.id] = enemy;

  StarSystem sys;
  sys.id = 1;
  sys.name = "Sol";
  sys.galaxy_pos = Vec2{0.0, 0.0};

  Body earth;
  earth.id = 10;
  earth.name = "Earth";
  earth.system_id = sys.id;
  earth.position_mkm = Vec2{0.0, 0.0};
  earth.orbit_radius_mkm = 0.0;
  earth.orbit_period_days = 1.0;
  earth.orbit_phase_radians = 0.0;
  st.bodies[earth.id] = earth;
  sys.bodies.push_back(earth.id);

  Colony col;
  col.id = 20;
  col.name = "Colony";
  col.faction_id = player.id;
  col.body_id = earth.id;
  col.population_millions = 100.0;
  col.installations["factory"] = 1;
  col.installations["lab"] = 1;
  col.installations["construction_facility"] = 1;
  col.installations["shipyard"] = 1;

  BuildOrder bo;
  bo.design_id = "test_frigate";
  bo.tons_remaining = 200.0;
  col.shipyard_queue.push_back(bo);

  st.colonies[col.id] = col;

  if (blockaded) {
    Ship sh;
    sh.id = 100;
    sh.name = "Raider";
    sh.faction_id = enemy.id;
    sh.system_id = sys.id;
    sh.design_id = "hostile_raider";
    sh.position_mkm = Vec2{0.0, 0.0};
    sh.hp = 100.0;
    st.ships[sh.id] = sh;
    sys.ships.push_back(sh.id);
  }

  st.systems[sys.id] = sys;

  return st;
}

struct Results {
  double blockade_mult{1.0};
  double construction_cp_per_day{0.0};
  double goods{0.0};
  double research_points{0.0};
  double shipyard_tons_remaining{0.0};
};

Results run_case(bool blockaded) {
  const ContentDB content = make_content();
  const SimConfig cfg = make_cfg();

  Simulation sim(content, cfg);
  sim.load_game(make_state(blockaded));

  const Id cid = 20;
  const Colony& c0 = sim.state().colonies.at(cid);

  Results r;
  r.blockade_mult = sim.blockade_output_multiplier_for_colony(cid);
  r.construction_cp_per_day = sim.construction_points_per_day(c0);

  sim.advance_days(1);

  const Colony& c1 = sim.state().colonies.at(cid);
  if (auto it = c1.minerals.find("Goods"); it != c1.minerals.end()) {
    r.goods = it->second;
  }

  r.research_points = sim.state().factions.at(1).research_points;

  if (!c1.shipyard_queue.empty()) {
    r.shipyard_tons_remaining = c1.shipyard_queue.front().tons_remaining;
  }

  return r;
}

}  // namespace

int test_blockade_economy() {
  const Results open = run_case(false);
  const Results blocked = run_case(true);

  N4X_ASSERT(std::abs(open.blockade_mult - 1.0) < 1e-9, "unblockaded colony has x1.0 output multiplier");
  N4X_ASSERT(std::abs(blocked.blockade_mult - 0.5) < 1e-9, "blockaded colony has expected x0.5 output multiplier");

  const double base_construction = 100.0 * 0.01 + 100.0;
  N4X_ASSERT(std::abs(open.construction_cp_per_day - base_construction) < 1e-9,
             "construction points per day unaffected when not blockaded");
  N4X_ASSERT(std::abs(blocked.construction_cp_per_day - base_construction * 0.5) < 1e-9,
             "construction points per day reduced under blockade");

  N4X_ASSERT(std::abs(open.goods - 100.0) < 1e-6, "industry output at full rate when not blockaded");
  N4X_ASSERT(std::abs(blocked.goods - 50.0) < 1e-6, "industry output reduced under blockade");

  N4X_ASSERT(std::abs(open.research_points - 100.0) < 1e-6, "research points generated at full rate");
  N4X_ASSERT(std::abs(blocked.research_points - 50.0) < 1e-6, "research points reduced under blockade");

  N4X_ASSERT(std::abs(open.shipyard_tons_remaining - 100.0) < 1e-6, "shipyard progressed 100t/day without blockade");
  N4X_ASSERT(std::abs(blocked.shipyard_tons_remaining - 150.0) < 1e-6,
             "shipyard progressed 50t/day under blockade");

  return 0;
}
