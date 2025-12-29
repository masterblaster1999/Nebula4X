#include <cmath>
#include <iostream>

#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

namespace {

double run_case(double power_generation) {
  nebula4x::ContentDB content;

  // Attacker has sensors online but weapons offline if power is insufficient.
  nebula4x::ShipDesign attacker;
  attacker.id = "attacker_design";
  attacker.name = "Attacker";
  attacker.role = nebula4x::ShipRole::Combatant;
  attacker.mass_tons = 100.0;
  attacker.speed_km_s = 0.0;
  attacker.max_hp = 50.0;
  attacker.sensor_range_mkm = 1000.0;
  attacker.weapon_damage = 10.0;
  attacker.weapon_range_mkm = 10.0;
  attacker.power_generation = power_generation;
  attacker.power_use_sensors = 1.0;
  attacker.power_use_weapons = 3.0;
  attacker.power_use_total = attacker.power_use_sensors + attacker.power_use_weapons;
  content.designs[attacker.id] = attacker;

  nebula4x::ShipDesign target;
  target.id = "target_design";
  target.name = "Target";
  target.role = nebula4x::ShipRole::Combatant;
  target.mass_tons = 100.0;
  target.speed_km_s = 0.0;
  target.max_hp = 100.0;
  content.designs[target.id] = target;

  nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});

  nebula4x::GameState s;
  s.save_version = nebula4x::GameState{}.save_version;
  s.date = nebula4x::Date::from_ymd(2200, 1, 1);

  const nebula4x::Id sys_id = 1;
  {
    nebula4x::StarSystem sys;
    sys.id = sys_id;
    sys.name = "Test System";
    s.systems[sys.id] = sys;
  }

  {
    nebula4x::Faction f;
    f.id = 2;
    f.name = "Blue";
    f.control = nebula4x::FactionControl::Player;
    s.factions[f.id] = f;
  }
  {
    nebula4x::Faction f;
    f.id = 3;
    f.name = "Red";
    f.control = nebula4x::FactionControl::AI_Passive;
    s.factions[f.id] = f;
  }

  const nebula4x::Id attacker_id = 10;
  const nebula4x::Id target_id = 11;
  {
    nebula4x::Ship sh;
    sh.id = attacker_id;
    sh.name = "Attacker";
    sh.faction_id = 2;
    sh.system_id = sys_id;
    sh.design_id = attacker.id;
    sh.position_mkm = {0.0, 0.0};
    sh.hp = 0.0;
    sh.shields = 0.0;
    s.ships[sh.id] = sh;
    s.systems[sys_id].ships.push_back(sh.id);
  }
  {
    nebula4x::Ship sh;
    sh.id = target_id;
    sh.name = "Target";
    sh.faction_id = 3;
    sh.system_id = sys_id;
    sh.design_id = target.id;
    sh.position_mkm = {5.0, 0.0};
    sh.hp = 0.0;
    sh.shields = 0.0;
    s.ships[sh.id] = sh;
    s.systems[sys_id].ships.push_back(sh.id);
  }

  s.next_id = 100;
  sim.load_game(std::move(s));

  sim.advance_days(1);

  const auto* tgt = nebula4x::find_ptr(sim.state().ships, target_id);
  N4X_ASSERT(tgt);
  return tgt->hp;
}

}  // namespace

int test_power_system() {
  // Insufficient generation: sensors stay online (1 power), but weapons (3 power)
  // are shed and cannot fire.
  const double hp_offline = run_case(2.0);
  N4X_ASSERT(std::abs(hp_offline - 100.0) < 1e-9);

  // Sufficient generation: weapons come online and deal damage.
  const double hp_online = run_case(10.0);
  N4X_ASSERT(hp_online < 100.0);
  N4X_ASSERT(hp_online > 0.0);

  return 0;
}
