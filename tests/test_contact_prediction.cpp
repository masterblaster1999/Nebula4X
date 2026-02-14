#include <cmath>
#include <iostream>
#include <string>

#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(cond, msg)                                                       \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::cerr << "ASSERT FAIL: " << (msg) << " (" << __FILE__ << ":" << __LINE__ \
                << ")\n";                                                           \
      return 1;                                                                     \
    }                                                                               \
  } while (0)

int test_contact_prediction() {
  using namespace nebula4x;

  ContentDB content;

  ShipDesign attacker;
  attacker.id = "att";
  attacker.name = "Attacker";
  attacker.role = ShipRole::Combatant;
  attacker.mass_tons = 100.0;
  attacker.max_hp = 10.0;
  attacker.speed_km_s = 0.0;
  attacker.sensor_range_mkm = 0.0; // ensure no detection sources
  content.designs[attacker.id] = attacker;

  ShipDesign target;
  target.id = "tgt";
  target.name = "Target";
  target.role = ShipRole::Combatant;
  target.mass_tons = 100.0;
  target.max_hp = 10.0;
  target.speed_km_s = 10.0;
  target.sensor_range_mkm = 0.0;
  content.designs[target.id] = target;

  SimConfig cfg;
  Simulation sim(content, cfg);

  GameState st;
  st.save_version = GameState{}.save_version;
  // Keep now >= 11 so (now-11) remains a valid previous contact day.
  st.date = Date::from_ymd(2200, 1, 12);
  const int now = static_cast<int>(st.date.days_since_epoch());

  StarSystem sys;
  sys.id = 1;
  sys.name = "Test System";
  st.systems[sys.id] = sys;

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

  Ship sh1;
  sh1.id = 10;
  sh1.name = "A1";
  sh1.faction_id = f1.id;
  sh1.design_id = attacker.id;
  sh1.system_id = sys.id;
  sh1.position_mkm = Vec2{0.0, 0.0};
  sh1.speed_km_s = 0.0;
  sh1.hp = 10.0;
  st.ships[sh1.id] = sh1;

  Ship sh2;
  sh2.id = 20;
  sh2.name = "B1";
  sh2.faction_id = f2.id;
  sh2.design_id = target.id;
  sh2.system_id = sys.id;
  sh2.position_mkm = Vec2{100.0, 0.0};
  sh2.speed_km_s = 0.0;
  sh2.hp = 10.0;
  st.ships[sh2.id] = sh2;

  // Give faction A a stale contact with a 2-point track:
  // Day (now-11): x=-1
  // Day (now-10): x= 0  => v = +1 mkm/day
  Contact c;
  c.ship_id = sh2.id;
  c.system_id = sys.id;
  c.last_seen_day = now - 10;
  c.last_seen_position_mkm = Vec2{0.0, 0.0};
  c.prev_seen_day = now - 11;
  c.prev_seen_position_mkm = Vec2{-1.0, 0.0};
  c.last_seen_name = sh2.name;
  c.last_seen_design_id = sh2.design_id;
  c.last_seen_faction_id = sh2.faction_id;
  st.factions[f1.id].ship_contacts[sh2.id] = c;

  sim.load_game(st);

  // Attack order should seed a predicted position at 'now': x = 10.
  N4X_ASSERT(sim.issue_attack_ship(sh1.id, sh2.id, /*restrict_to_discovered=*/false), "issue_attack_ship failed");

  auto& q0 = sim.state().ship_orders[sh1.id].queue;
  N4X_ASSERT(!q0.empty(), "ship_orders queue empty");
  N4X_ASSERT(std::holds_alternative<AttackShip>(q0.back()), "last order not AttackShip");
  const auto ord0 = std::get<AttackShip>(q0.back());
  N4X_ASSERT(ord0.has_last_known, "AttackShip missing last_known");
  N4X_ASSERT(std::abs(ord0.last_known_position_mkm.x - 10.0) < 1e-6, "unexpected predicted x @now");
  N4X_ASSERT(std::abs(ord0.last_known_position_mkm.y - 0.0) < 1e-6, "unexpected predicted y @now");

  // When the contact remains lost, tick_ships should keep extrapolating that
  // last-known position each day.
  sim.advance_days(1);

  auto& q1 = sim.state().ship_orders[sh1.id].queue;
  N4X_ASSERT(!q1.empty(), "ship_orders queue empty after advance_days");
  N4X_ASSERT(std::holds_alternative<AttackShip>(q1.front()), "front order not AttackShip after advance_days");
  const auto ord1 = std::get<AttackShip>(q1.front());
  N4X_ASSERT(ord1.has_last_known, "AttackShip missing last_known after advance_days");
  N4X_ASSERT(std::abs(ord1.last_known_position_mkm.x - 11.0) < 1e-6, "unexpected predicted x @now+1");
  N4X_ASSERT(std::abs(ord1.last_known_position_mkm.y - 0.0) < 1e-6, "unexpected predicted y @now+1");

  return 0;
}
