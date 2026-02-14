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

int test_sensor_coverage() {
  using namespace nebula4x;

  ContentDB content;

  ShipDesign sensor;
  sensor.id = "sensor";
  sensor.name = "Sensor Ship";
  sensor.role = ShipRole::Combatant;
  sensor.mass_tons = 100.0;
  sensor.max_hp = 10.0;
  sensor.speed_km_s = 0.0;
  sensor.sensor_range_mkm = 100.0;
  sensor.power_generation = 10.0;
  sensor.power_use_sensors = 1.0;
  content.designs[sensor.id] = sensor;

  ShipDesign tgt;
  tgt.id = "tgt";
  tgt.name = "Target";
  tgt.role = ShipRole::Combatant;
  tgt.mass_tons = 100.0;
  tgt.max_hp = 10.0;
  tgt.speed_km_s = 0.0;
  tgt.sensor_range_mkm = 0.0;   // no EMCON effects
  tgt.signature_multiplier = 1.0;
  content.designs[tgt.id] = tgt;

  ShipDesign stealth = tgt;
  stealth.id = "stealth";
  stealth.name = "Stealth Target";
  stealth.signature_multiplier = 0.5;
  content.designs[stealth.id] = stealth;

  InstallationDef radar;
  radar.id = "radar";
  radar.name = "Radar Station";
  radar.sensor_range_mkm = 200.0;
  content.installations[radar.id] = radar;

  SimConfig cfg;
  cfg.sensor_mode_passive_range_multiplier = 0.5;
  cfg.sensor_mode_active_range_multiplier = 1.5;
  cfg.sensor_mode_passive_signature_multiplier = 0.8;
  cfg.sensor_mode_active_signature_multiplier = 1.5;

  Simulation sim(content, cfg);

  GameState st;
  st.save_version = GameState{}.save_version;
  st.date = Date::from_ymd(2200, 1, 1);

  StarSystem sys;
  sys.id = 1;
  sys.name = "Test System";
  st.systems[sys.id] = sys;
  st.selected_system = sys.id;

  Faction f1;
  f1.id = 1;
  f1.name = "A";
  f1.control = FactionControl::Player;
  st.factions[f1.id] = f1;

  Faction f2;
  f2.id = 2;
  f2.name = "B";
  f2.control = FactionControl::AI_Passive;
  st.factions[f2.id] = f2;

  Ship sensor_ship;
  sensor_ship.id = 10;
  sensor_ship.name = "Sensor";
  sensor_ship.faction_id = f1.id;
  sensor_ship.design_id = sensor.id;
  sensor_ship.system_id = sys.id;
  sensor_ship.position_mkm = Vec2{0.0, 0.0};
  sensor_ship.speed_km_s = 0.0;
  sensor_ship.hp = 10.0;
  sensor_ship.power_policy.sensors_enabled = true;
  sensor_ship.sensor_mode = SensorMode::Normal;
  st.ships[sensor_ship.id] = sensor_ship;

  Ship target_ship;
  target_ship.id = 20;
  target_ship.name = "Target";
  target_ship.faction_id = f2.id;
  target_ship.design_id = tgt.id;
  target_ship.system_id = sys.id;
  target_ship.position_mkm = Vec2{90.0, 0.0};
  target_ship.speed_km_s = 0.0;
  target_ship.hp = 10.0;
  st.ships[target_ship.id] = target_ship;

  // Populate the system ship index used by sensors/detection.
  st.systems[sys.id].ships.push_back(sensor_ship.id);
  st.systems[sys.id].ships.push_back(target_ship.id);

  sim.load_game(st);

  // Normal mode (range 100) should detect a baseline target at 90 mkm.
  N4X_ASSERT(sim.is_ship_detected_by_faction(f1.id, target_ship.id),
            "normal: expected detection at 90 mkm");

  // Passive mode reduces range (50); target at 90 should be lost.
  sim.state().ships.at(sensor_ship.id).sensor_mode = SensorMode::Passive;
  N4X_ASSERT(!sim.is_ship_detected_by_faction(f1.id, target_ship.id),
            "passive: expected no detection at 90 mkm");

  // Active mode increases range (150); target at 140 should be detected.
  sim.state().ships.at(sensor_ship.id).sensor_mode = SensorMode::Active;
  sim.state().ships.at(target_ship.id).position_mkm = Vec2{140.0, 0.0};
  N4X_ASSERT(sim.is_ship_detected_by_faction(f1.id, target_ship.id),
            "active: expected detection at 140 mkm");

  // Stealth target (signature 0.5) halves effective detection range.
  sim.state().ships.at(sensor_ship.id).sensor_mode = SensorMode::Normal;
  sim.state().ships.at(target_ship.id).design_id = stealth.id;
  sim.state().ships.at(target_ship.id).position_mkm = Vec2{60.0, 0.0};
  N4X_ASSERT(!sim.is_ship_detected_by_faction(f1.id, target_ship.id),
            "stealth: expected no detection at 60 mkm");

  // Colony sensors should provide detection even if ship sensors are disabled.
  {
    Body body;
    body.id = 100;
    body.name = "Radar World";
    body.system_id = sys.id;
    body.type = BodyType::Planet;
    body.position_mkm = Vec2{0.0, 0.0};
    sim.state().bodies[body.id] = body;
    sim.state().systems[sys.id].bodies.push_back(body.id);

    Colony col;
    col.id = 200;
    col.name = "Radar Colony";
    col.faction_id = f1.id;
    col.body_id = body.id;
    col.population_millions = 1.0;
    col.installations[radar.id] = 1;
    sim.state().colonies[col.id] = col;

    // Disable ship sensors to ensure the colony is the only source.
    sim.state().ships.at(sensor_ship.id).power_policy.sensors_enabled = false;
    sim.state().ships.at(sensor_ship.id).sensor_mode = SensorMode::Normal;

    // Reset target to baseline design/signature and place within colony range.
    sim.state().ships.at(target_ship.id).design_id = tgt.id;
    sim.state().ships.at(target_ship.id).position_mkm = Vec2{150.0, 0.0};

    N4X_ASSERT(sim.is_ship_detected_by_faction(f1.id, target_ship.id),
              "colony radar: expected detection at 150 mkm with ship sensors disabled");
  }

  return 0;
}
