#include <iostream>
#include <string>

#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(cond, msg) \
  do { \
    if (!(cond)) { \
      std::cerr << "ASSERT FAIL: " << (msg) << " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
      return 1; \
    } \
  } while (0)

int test_body_occlusion() {
  using namespace nebula4x;

  ContentDB content;

  // --- Ship designs ---
  ShipDesign attacker_d;
  attacker_d.id = "attacker";
  attacker_d.name = "Attacker";
  attacker_d.role = ShipRole::Combatant;
  attacker_d.mass_tons = 100.0;
  attacker_d.max_hp = 100.0;
  attacker_d.speed_km_s = 0.0;
  attacker_d.sensor_range_mkm = 0.0;  // rely on the dedicated sensor ship
  attacker_d.weapon_range_mkm = 50.0;
  attacker_d.weapon_damage = 10.0;
  attacker_d.power_generation = 10.0;
  attacker_d.power_use_weapons = 1.0;
  content.designs[attacker_d.id] = attacker_d;

  ShipDesign sensor_d;
  sensor_d.id = "sensor";
  sensor_d.name = "Sensor";
  sensor_d.role = ShipRole::Combatant;
  sensor_d.mass_tons = 100.0;
  sensor_d.max_hp = 50.0;
  sensor_d.speed_km_s = 0.0;
  sensor_d.sensor_range_mkm = 100.0;
  sensor_d.weapon_range_mkm = 0.0;
  sensor_d.weapon_damage = 0.0;
  sensor_d.power_generation = 10.0;
  sensor_d.power_use_sensors = 1.0;
  content.designs[sensor_d.id] = sensor_d;

  ShipDesign target_d;
  target_d.id = "target";
  target_d.name = "Target";
  target_d.role = ShipRole::Combatant;
  target_d.mass_tons = 100.0;
  target_d.max_hp = 100.0;
  target_d.speed_km_s = 0.0;
  target_d.sensor_range_mkm = 0.0;
  target_d.weapon_damage = 0.0;
  target_d.weapon_range_mkm = 0.0;
  content.designs[target_d.id] = target_d;

  // --- Simulation config ---
  SimConfig cfg;
  cfg.enable_combat = true;
  cfg.enable_ship_maintenance = false;
  cfg.enable_beam_hit_chance = false;      // deterministic
  cfg.enable_beam_scatter_splash = false;  // deterministic
  cfg.enable_beam_los_attenuation = false; // keep this test focused

  cfg.enable_body_occlusion_sensors = true;
  cfg.enable_body_occlusion_weapons = true;
  cfg.body_occlusion_padding_mkm = 0.0;

  Simulation sim(content, cfg);

  // --- Minimal game state ---
  GameState st;
  st.save_version = GameState{}.save_version;
  st.date = Date::from_ymd(2200, 1, 1);

  StarSystem sys;
  sys.id = 1;
  sys.name = "Occlusion System";
  st.systems[sys.id] = sys;
  st.selected_system = sys.id;

  // Occluding planet at origin, radius 1 million km = 1 mkm.
  Body planet;
  planet.id = 100;
  planet.name = "Occluder";
  planet.system_id = sys.id;
  planet.type = BodyType::Planet;
  planet.position_mkm = Vec2{0.0, 0.0};
  planet.radius_km = 1'000'000.0;
  st.bodies[planet.id] = planet;
  st.systems[sys.id].bodies.push_back(planet.id);

  Faction a;
  a.id = 1;
  a.name = "A";
  a.control = FactionControl::Player;
  a.relations[2] = DiplomacyStatus::Hostile;
  st.factions[a.id] = a;

  Faction b;
  b.id = 2;
  b.name = "B";
  b.control = FactionControl::AI_Passive;
  b.relations[1] = DiplomacyStatus::Hostile;
  st.factions[b.id] = b;

  Ship attacker;
  attacker.id = 10;
  attacker.name = "Attacker";
  attacker.faction_id = a.id;
  attacker.design_id = attacker_d.id;
  attacker.system_id = sys.id;
  attacker.position_mkm = Vec2{-10.0, 0.0};
  attacker.speed_km_s = 0.0;
  attacker.hp = attacker_d.max_hp;
  attacker.power_policy.weapons_enabled = true;
  attacker.power_policy.sensors_enabled = false;
  attacker.combat_doctrine.fire_control = FireControl::WeaponsFree;
  st.ships[attacker.id] = attacker;

  Ship sensor;
  sensor.id = 11;
  sensor.name = "Sensor";
  sensor.faction_id = a.id;
  sensor.design_id = sensor_d.id;
  sensor.system_id = sys.id;
  // Start co-located with the attacker to validate pure occlusion in a simple geometry.
  sensor.position_mkm = Vec2{-10.0, 0.0};
  sensor.speed_km_s = 0.0;
  sensor.hp = sensor_d.max_hp;
  sensor.power_policy.weapons_enabled = false;
  sensor.power_policy.sensors_enabled = true;
  st.ships[sensor.id] = sensor;

  Ship target;
  target.id = 20;
  target.name = "Target";
  target.faction_id = b.id;
  target.design_id = target_d.id;
  target.system_id = sys.id;
  target.position_mkm = Vec2{10.0, 0.0};
  target.speed_km_s = 0.0;
  target.hp = target_d.max_hp;
  st.ships[target.id] = target;

  st.systems[sys.id].ships.push_back(attacker.id);
  st.systems[sys.id].ships.push_back(sensor.id);
  st.systems[sys.id].ships.push_back(target.id);

  sim.load_game(st);

  // --- SENSOR OCCLUSION ---
  // Target is directly behind the planet relative to both sensor sources.
  N4X_ASSERT(!sim.is_ship_detected_by_faction(a.id, target.id),
             "expected: body occlusion blocks sensor detection");

  // Move the target off-axis so the segment misses the planet.
  sim.state().ships.at(target.id).position_mkm = Vec2{10.0, 10.0};
  N4X_ASSERT(sim.is_ship_detected_by_faction(a.id, target.id),
             "expected: detection succeeds when LOS does not cross the planet");

  // Legacy behavior check: disabling sensor occlusion restores distance-only detection.
  sim.set_body_occlusion_sensors_enabled(false);
  sim.state().ships.at(target.id).position_mkm = Vec2{10.0, 0.0};
  N4X_ASSERT(sim.is_ship_detected_by_faction(a.id, target.id),
             "expected: detection succeeds when body occlusion is disabled");

  // Re-enable sensor occlusion for the weapon test.
  sim.set_body_occlusion_sensors_enabled(true);

  // --- WEAPON OCCLUSION ---
  // Move the sensor ship off-axis so the *faction* detects the target, but the attacker
  // still has the planet directly between it and the target.
  sim.state().ships.at(sensor.id).position_mkm = Vec2{0.0, 20.0};
  sim.state().ships.at(target.id).position_mkm = Vec2{10.0, 0.0};

  // Ensure the attacker tries to shoot the target.
  sim.issue_attack_ship(attacker.id, target.id);

  const double hp0 = sim.state().ships.at(target.id).hp;
  sim.advance_days(1);
  const double hp1 = sim.state().ships.at(target.id).hp;

  N4X_ASSERT(hp1 == hp0, "expected: weapon LOS occlusion prevents direct-fire damage");

  // Disabling weapon occlusion should allow the same shot to land.
  sim.set_body_occlusion_weapons_enabled(false);
  sim.issue_attack_ship(attacker.id, target.id);

  const double hp2 = sim.state().ships.at(target.id).hp;
  sim.advance_days(1);
  const double hp3 = sim.state().ships.at(target.id).hp;

  N4X_ASSERT(hp3 < hp2, "expected: target takes damage when weapon occlusion is disabled");

  return 0;
}
