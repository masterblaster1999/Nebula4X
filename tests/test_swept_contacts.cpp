#include <iostream>

#include "nebula4x/core/orders.h"
#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(cond, msg) \
  do { \
    if (!(cond)) { \
      std::cerr << "ASSERT FAIL: " << (msg) << " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
      return 1; \
    } \
  } while (0)

// Regression test for contact detection under sub-day turn ticks.
//
// Previously, tick_contacts() only checked end-of-tick positions. Fast ships
// could pass through sensor range between tick boundaries without ever being
// recorded as a contact.
int test_swept_contacts() {
  using namespace nebula4x;

  ContentDB content;

  ShipDesign sensor;
  sensor.id = "sensor";
  sensor.name = "Sensor Ship";
  sensor.role = ShipRole::Combatant;
  sensor.mass_tons = 100.0;
  sensor.max_hp = 10.0;
  sensor.speed_km_s = 0.0;
  sensor.sensor_range_mkm = 10.0;
  sensor.power_generation = 10.0;
  sensor.power_use_sensors = 1.0;
  content.designs[sensor.id] = sensor;

  ShipDesign runner;
  runner.id = "runner";
  runner.name = "Fast Runner";
  runner.role = ShipRole::Combatant;
  runner.mass_tons = 100.0;
  runner.max_hp = 10.0;
  // ~43 mkm/day at default seconds_per_day (86400).
  runner.speed_km_s = 500.0;
  runner.sensor_range_mkm = 0.0;
  runner.signature_multiplier = 1.0;
  content.designs[runner.id] = runner;

  Simulation sim(content, SimConfig{});

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
  f1.name = "Observer";
  f1.control = FactionControl::Player;
  st.factions[f1.id] = f1;

  Faction f2;
  f2.id = 2;
  f2.name = "Target";
  f2.control = FactionControl::AI_Passive;
  st.factions[f2.id] = f2;

  Ship s;
  s.id = 10;
  s.name = "Sensor";
  s.faction_id = f1.id;
  s.design_id = sensor.id;
  s.system_id = sys.id;
  s.position_mkm = Vec2{0.0, 0.0};
  s.hp = 10.0;
  st.ships[s.id] = s;

  Ship t;
  t.id = 20;
  t.name = "Runner";
  t.faction_id = f2.id;
  t.design_id = runner.id;
  t.system_id = sys.id;
  t.position_mkm = Vec2{-20.0, 0.0};
  t.hp = 10.0;
  st.ships[t.id] = t;

  st.systems[sys.id].ships.push_back(s.id);
  st.systems[sys.id].ships.push_back(t.id);

  // Ensure both ships are processed by tick_ships.
  st.ship_orders[s.id] = ShipOrders{};
  ShipOrders ord;
  ord.queue.push_back(MoveToPoint{Vec2{20.0, 0.0}});
  st.ship_orders[t.id] = ord;

  sim.load_game(st);

  N4X_ASSERT(sim.state().factions[f1.id].ship_contacts.count(t.id) == 0,
             "expected no initial contact at -20 mkm");

  // Advance one 24h step: the runner crosses the origin (inside the 10 mkm
  // sensor bubble) but ends outside at +20 mkm.
  sim.advance_hours(24);

  N4X_ASSERT(sim.state().factions[f1.id].ship_contacts.count(t.id) == 1,
             "expected contact to be recorded for mid-step pass-by");

  const Contact& c = sim.state().factions[f1.id].ship_contacts.at(t.id);
  const double seen_dist = c.last_seen_position_mkm.length();
  N4X_ASSERT(seen_dist <= 10.0 + 1e-6,
             "expected last_seen_position to be near the closest approach");

  return 0;
}
