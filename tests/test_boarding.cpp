#include <iostream>

#include "nebula4x/core/simulation.h"

#include "test.h"


#define N4X_ASSERT(cond)                                             \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::cerr << "Assertion failed: " << #cond << " at "        \
                << __FILE__ << ":" << __LINE__ << "\n";         \
      return 1;                                                     \
    }                                                               \
  } while (0)

using namespace nebula4x;

int test_boarding() {
  SimConfig cfg;
  cfg.enable_combat = true;
  cfg.enable_boarding = true;
  cfg.boarding_range_mkm = 0.1;
  cfg.boarding_target_hp_fraction = 0.25;
  cfg.boarding_require_shields_down = true;
  cfg.boarding_min_attacker_troops = 1.0;
  cfg.boarding_defense_hp_factor = 0.0; // Chance becomes 1.0 (no defender strength).
  cfg.boarding_attacker_casualty_fraction = 0.0;
  cfg.boarding_defender_casualty_fraction = 0.0;

  ContentDB content;

  ShipDesign boarder;
  boarder.id = "boarder";
  boarder.name = "Boarder";
  boarder.speed_km_s = 0.0;
  boarder.max_hp = 100.0;
  boarder.max_shields = 0.0;
  boarder.sensor_range_mkm = 10.0;
  boarder.weapon_damage = 0.0;
  boarder.weapon_range_mkm = 0.0;
  boarder.troop_capacity = 100.0;
  content.designs[boarder.id] = boarder;

  ShipDesign target_design;
  target_design.id = "target";
  target_design.name = "Target";
  target_design.speed_km_s = 0.0;
  target_design.max_hp = 100.0;
  target_design.max_shields = 0.0;
  target_design.sensor_range_mkm = 0.0;
  target_design.weapon_damage = 0.0;
  target_design.weapon_range_mkm = 0.0;
  target_design.troop_capacity = 0.0;
  content.designs[target_design.id] = target_design;

  Simulation sim(content, cfg);

  GameState st;
  const Id sys_id = 1;
  StarSystem sys;
  sys.id = sys_id;
  sys.name = "Test System";
  st.systems[sys_id] = sys;

  const Id fac_a = 10;
  const Id fac_b = 11;

  Faction fa;
  fa.id = fac_a;
  fa.name = "A";
  Faction fb;
  fb.id = fac_b;
  fb.name = "B";

  st.factions[fac_a] = fa;
  st.factions[fac_b] = fb;

  const Id attacker_id = 100;
  const Id target_id = 101;

  Ship attacker;
  attacker.id = attacker_id;
  attacker.name = "Attacker";
  attacker.faction_id = fac_a;
  attacker.system_id = sys_id;
  attacker.design_id = boarder.id;
  attacker.position_mkm = Vec2{0.0, 0.0};
  attacker.hp = 100.0;
  attacker.shields = 0.0;
  attacker.troops = 50.0;

  Ship target;
  target.id = target_id;
  target.name = "Target";
  target.faction_id = fac_b;
  target.system_id = sys_id;
  target.design_id = target_design.id;
  target.position_mkm = Vec2{0.05, 0.0}; // within boarding range
  target.hp = 10.0;                      // disabled
  target.shields = 0.0;
  target.troops = 0.0;

  st.ships[attacker_id] = attacker;
  st.ships[target_id] = target;

  st.systems[sys_id].ships.push_back(attacker_id);
  st.systems[sys_id].ships.push_back(target_id);

  st.ship_orders[attacker_id] = ShipOrders{};
  st.ship_orders[target_id] = ShipOrders{};

  st.next_id = 1000;

  sim.load_game(st);
  N4X_ASSERT(sim.issue_attack_ship(attacker_id, target_id));

  sim.advance_days(1);

  const auto& st2 = sim.state();
  auto it = st2.ships.find(target_id);
  N4X_ASSERT(it != st2.ships.end());
  N4X_ASSERT(it->second.faction_id == fac_a);

  std::cout << "test_boarding passed\n";
  return 0;
}
