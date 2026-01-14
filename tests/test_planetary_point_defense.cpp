#include <iostream>

#include "nebula4x/core/simulation.h"

#include "test.h"


#define N4X_ASSERT(cond)                                             \
  do {                                                               \
    if (!(cond)) {                                                   \
      std::cerr << "Assertion failed: " << #cond << " at "         \
                << __FILE__ << ":" << __LINE__ << "\n";            \
      return 1;                                                      \
    }                                                                \
  } while (0)

using namespace nebula4x;

int test_planetary_point_defense() {
  SimConfig cfg;
  cfg.enable_combat = true;
  // Make the test deterministic: if any missile payload remains at impact,
  // it will hit (no random miss chance).
  cfg.enable_missile_hit_chance = false;

  ContentDB content;

  // Defender target ship (no point defense).
  ShipDesign escort;
  escort.id = "escort_gamma";
  escort.name = "Escort Gamma";
  escort.speed_km_s = 0.0;
  escort.max_hp = 100.0;
  escort.max_shields = 0.0;
  escort.power_generation = 0.0;
  escort.sensor_range_mkm = 10.0;
  escort.weapon_damage = 0.0;
  escort.weapon_range_mkm = 0.0;
  escort.point_defense_damage = 0.0;
  escort.point_defense_range_mkm = 0.0;
  content.designs[escort.id] = escort;

  // Attacker with missiles.
  ShipDesign raider;
  raider.id = "pirate_raider";
  raider.name = "Raider";
  raider.speed_km_s = 0.0;
  raider.max_hp = 100.0;
  raider.max_shields = 0.0;
  raider.power_generation = 0.0;
  raider.sensor_range_mkm = 20.0;
  raider.weapon_damage = 0.0;
  raider.weapon_range_mkm = 0.0;
  raider.missile_damage = 10.0;
  raider.missile_range_mkm = 5.0;
  raider.missile_speed_mkm_per_day = 0.2;
  raider.missile_reload_days = 1.0;
  raider.point_defense_damage = 0.0;
  raider.point_defense_range_mkm = 0.0;
  content.designs[raider.id] = raider;

  // Minimal tech entries used by the default scenario setup.
  for (const char* id : {"chemistry_1", "nuclear_1", "propulsion_1"}) {
    TechDef t;
    t.id = id;
    t.name = id;
    content.techs[t.id] = t;
  }

  // Colony PD installation (very strong so this test is robust).
  InstallationDef pd;
  pd.id = "point_defense_battery";
  pd.name = "Point Defense Battery";
  pd.point_defense_damage = 1000.0;
  pd.point_defense_range_mkm = 1000.0;
  content.installations[pd.id] = pd;

  Simulation sim(content, cfg);

  auto& st = sim.state();

  // Find Earth colony and its body position.
  Id earth_cid = kInvalidId;
  for (const auto& [cid, col] : st.colonies) {
    if (col.name == "Earth") {
      earth_cid = cid;
      break;
    }
  }
  N4X_ASSERT(earth_cid != kInvalidId);
  auto* earth_body = find_ptr(st.bodies, st.colonies[earth_cid].body_id);
  N4X_ASSERT(earth_body != nullptr);

  // Ensure the colony has our PD installation.
  st.colonies[earth_cid].installations[pd.id] += 1;

  // Find a defender ship and the pirate raider in the default scenario.
  Id target_id = kInvalidId;
  Id attacker_id = kInvalidId;
  for (const auto& [sid, sh] : st.ships) {
    if (sh.name == "Escort Gamma") target_id = sid;
    if (sh.name == "Raider I") attacker_id = sid;
  }
  N4X_ASSERT(target_id != kInvalidId);
  N4X_ASSERT(attacker_id != kInvalidId);

  // Put the defender ship close to Earth, with the attacker nearby.
  const Vec2 earth_pos = earth_body->position_mkm;
  st.ships[target_id].position_mkm = earth_pos + Vec2{0.10, 0.00};
  st.ships[attacker_id].position_mkm = earth_pos + Vec2{0.30, 0.00};

  const double hp0 = st.ships[target_id].hp;
  N4X_ASSERT(hp0 > 1e-9);

  // Force an attack order so missiles are launched.
  N4X_ASSERT(sim.issue_attack_ship(attacker_id, target_id));

  // One day: launch + in-flight; second day: impact would occur if not intercepted.
  sim.advance_days(2);

  const auto& st2 = sim.state();
  const auto it_target = st2.ships.find(target_id);
  N4X_ASSERT(it_target != st2.ships.end());
  const double hp1 = it_target->second.hp;

  // Without PD the defender should take missile damage. With colony PD, payload is intercepted.
  N4X_ASSERT(std::abs(hp1 - hp0) <= 1e-6);

  // Verify our new event was generated (helps catch regressions where missiles miss for other reasons).
  bool saw_pd_event = false;
  for (const auto& ev : st2.events) {
    if (ev.colony_id == earth_cid && ev.category == EventCategory::Combat &&
        ev.message.find("Colony point defense") != std::string::npos) {
      saw_pd_event = true;
      break;
    }
  }
  N4X_ASSERT(saw_pd_event);

  return 0;
}
