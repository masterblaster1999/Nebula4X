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

} // namespace

int test_attack_lead_pursuit() {
  using namespace nebula4x;

  // --- Content ---
  ContentDB content;

  ShipDesign attacker_d;
  attacker_d.id = "attacker";
  attacker_d.name = "attacker";
  attacker_d.speed_km_s = 100.0;        // ~8.64 mkm/day
  attacker_d.sensor_range_mkm = 1000.0; // guaranteed detection in this test
  attacker_d.weapon_range_mkm = 0.0;
  attacker_d.signature_multiplier = 1.0;
  attacker_d.power_use_sensors = 0.0;

  ShipDesign target_d;
  target_d.id = "target";
  target_d.name = "target";
  target_d.speed_km_s = 0.0;
  target_d.sensor_range_mkm = 0.0;
  target_d.weapon_range_mkm = 0.0;
  target_d.signature_multiplier = 1.0;
  target_d.power_use_sensors = 0.0;

  content.designs[attacker_d.id] = attacker_d;
  content.designs[target_d.id] = target_d;

  SimConfig cfg;
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

  Ship attacker;
  attacker.id = attacker_id;
  attacker.faction_id = f1.id;
  attacker.system_id = sys_id;
  attacker.name = "Attacker";
  attacker.design_id = "attacker";
  attacker.position_mkm = {0.0, 0.0};
  st.ships[attacker_id] = attacker;

  Ship target;
  target.id = target_id;
  target.faction_id = f2.id;
  target.system_id = sys_id;
  target.name = "Target";
  target.design_id = "target";
  target.position_mkm = {10.0, 0.0};
  st.ships[target_id] = target;

  sim.load_game(std::move(st));

  // Seed a contact track with velocity (0, +1) mkm/day so lead pursuit has data.
  {
    auto& fac = sim.state().factions[f1.id];
    Contact ctc;
    ctc.ship_id = target_id;
    ctc.system_id = sys_id;

    const int day = static_cast<int>(sim.state().date.days_since_epoch());
    ctc.last_seen_day = day;
    ctc.last_seen_position_mkm = {10.0, 0.0};

    ctc.prev_seen_day = day - 1;
    ctc.prev_seen_position_mkm = {10.0, -1.0};

    ctc.last_seen_name = "Target";
    ctc.last_seen_design_id = "target";
    ctc.last_seen_faction_id = f2.id;

    fac.ship_contacts[target_id] = ctc;
  }

  // Issue an attack order. Since the target is detected, tick_ships will use
  // lead pursuit based on the contact velocity estimate.
  sim.issue_attack_ship(attacker_id, target_id, /*fog_of_war=*/false);

  sim.advance_days(1);

  const auto& out = sim.state();
  const auto it = out.ships.find(attacker_id);
  N4X_ASSERT(it != out.ships.end());

  // With lead pursuit, we should have moved to a point with +Y component.
  // (Without lead pursuit we'd move purely along +X.)
  N4X_ASSERT(it->second.position_mkm.y > 0.05);

  return 0;
}
