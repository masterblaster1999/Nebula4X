#include "nebula4x/core/simulation.h"

#include "tests/test.h"

#include <cmath>
#include <iostream>

namespace {

#define N4X_ASSERT(cond)                                                                          \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::cerr << "ASSERT FAILED: " << #cond << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
      return 1;                                                                                    \
    }                                                                                              \
  } while (0)

double dist_mkm(const nebula4x::Vec2& a, const nebula4x::Vec2& b) {
  const double dx = a.x - b.x;
  const double dy = a.y - b.y;
  return std::sqrt(dx * dx + dy * dy);
}

} // namespace

int test_combat_doctrine() {
  using namespace nebula4x;

  // --- Content ---
  ContentDB content;

  ShipDesign attacker_d;
  attacker_d.id = "attacker";
  attacker_d.name = "Missile Attacker";
  attacker_d.speed_km_s = 800.0;      // ~69.12 mkm/day
  attacker_d.sensor_range_mkm = 1000.0;
  attacker_d.signature_multiplier = 1.0;
  attacker_d.power_use_sensors = 0.0;

  attacker_d.weapon_range_mkm = 0.0;
  attacker_d.weapon_damage = 0.0;
  attacker_d.missile_range_mkm = 50.0;
  attacker_d.missile_damage = 10.0;

  ShipDesign target_d;
  target_d.id = "target";
  target_d.name = "Target";
  target_d.speed_km_s = 0.0;
  target_d.sensor_range_mkm = 0.0;
  target_d.signature_multiplier = 1.0;
  target_d.power_use_sensors = 0.0;

  content.designs[attacker_d.id] = attacker_d;
  content.designs[target_d.id] = target_d;

  SimConfig cfg;
  cfg.enable_combat = false; // we only care about movement/positioning in this test
  Simulation sim(std::move(content), cfg);

  // --- State ---
  GameState st;
  st.date = Date::from_ymd(2100, 1, 1);

  const Id sys_id = 1;
  StarSystem sys;
  sys.id = sys_id;
  sys.name = "Test";

  const Id attacker_id = 10;
  const Id target_id = 11;
  sys.ships.push_back(attacker_id);
  sys.ships.push_back(target_id);
  st.systems[sys_id] = sys;

  Faction f1;
  f1.id = 1;
  f1.name = "A";
  st.factions[f1.id] = f1;

  Faction f2;
  f2.id = 2;
  f2.name = "B";
  st.factions[f2.id] = f2;

  // --- Scenario 1: Auto mode for a missile-only design should standoff at missile range.
  {
    GameState st1 = st;

    Ship attacker;
    attacker.id = attacker_id;
    attacker.faction_id = f1.id;
    attacker.system_id = sys_id;
    attacker.name = "Attacker";
    attacker.design_id = "attacker";
    attacker.position_mkm = {0.0, 0.0};
    st1.ships[attacker_id] = attacker;

    Ship target;
    target.id = target_id;
    target.faction_id = f2.id;
    target.system_id = sys_id;
    target.name = "Target";
    target.design_id = "target";
    target.position_mkm = {100.0, 0.0};
    st1.ships[target_id] = target;

    sim.load_game(std::move(st1));

    sim.issue_attack_ship(attacker_id, target_id, /*fog_of_war=*/false);
    sim.advance_days(1);

    const auto& out = sim.state();
    const auto& a = out.ships.at(attacker_id);
    const auto& t = out.ships.at(target_id);
    const double d = dist_mkm(a.position_mkm, t.position_mkm);

    // Default doctrine: range_fraction = 0.90, Auto uses missile range when no beams.
    N4X_ASSERT(std::abs(d - 45.0) < 1e-6);
  }

  // --- Scenario 2: Kiting enabled -> if inside standoff range, back off to it.
  {
    GameState st2 = st;

    Ship attacker;
    attacker.id = attacker_id;
    attacker.faction_id = f1.id;
    attacker.system_id = sys_id;
    attacker.name = "Attacker";
    attacker.design_id = "attacker";
    attacker.position_mkm = {90.0, 0.0}; // start 10 mkm from target
    attacker.combat_doctrine.kite_if_too_close = true;
    // Keep defaults otherwise: Auto mode, 0.90 fraction, 0.10 min.
    st2.ships[attacker_id] = attacker;

    Ship target;
    target.id = target_id;
    target.faction_id = f2.id;
    target.system_id = sys_id;
    target.name = "Target";
    target.design_id = "target";
    target.position_mkm = {100.0, 0.0};
    st2.ships[target_id] = target;

    sim.load_game(std::move(st2));

    sim.issue_attack_ship(attacker_id, target_id, /*fog_of_war=*/false);
    sim.advance_days(1);

    const auto& out = sim.state();
    const auto& a = out.ships.at(attacker_id);
    const auto& t = out.ships.at(target_id);
    const double d = dist_mkm(a.position_mkm, t.position_mkm);

    // Kiting should have increased distance to the desired standoff range.
    N4X_ASSERT(d > 40.0);
    N4X_ASSERT(std::abs(d - 45.0) < 1e-6);
  }

  return 0;
}
