#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

#include "nebula4x/core/serialization.h"
#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(cond, msg)                                                       \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::cerr << "ASSERT FAIL: " << (msg) << " (" << __FILE__ << ":" << __LINE__ \
                << ")\n";                                                           \
      return 1;                                                                     \
    }                                                                               \
  } while (0)

int test_diplomacy() {
  using namespace nebula4x;

  ContentDB content;

  // Minimal combat-capable designs with long sensors and range.
  ShipDesign blue;
  blue.id = "blue";
  blue.name = "Blue Corvette";
  blue.role = ShipRole::Combatant;
  blue.mass_tons = 100.0;
  blue.max_hp = 100.0;
  blue.speed_km_s = 0.0;
  blue.weapon_damage = 5.0;
  blue.weapon_range_mkm = 1000.0;
  blue.sensor_range_mkm = 1000.0;
  content.designs[blue.id] = blue;

  ShipDesign red;
  red.id = "red";
  red.name = "Red Corvette";
  red.role = ShipRole::Combatant;
  red.mass_tons = 100.0;
  red.max_hp = 100.0;
  red.speed_km_s = 0.0;
  red.weapon_damage = 3.0;
  red.weapon_range_mkm = 1000.0;
  red.sensor_range_mkm = 1000.0;
  content.designs[red.id] = red;

  SimConfig cfg;
  cfg.combat_damage_event_min_abs = 0.0; // Don't suppress small-damage events in tests.

  Simulation sim(content, cfg);

  GameState st;
  st.save_version = GameState{}.save_version;

  // Factions.
  Faction f1;
  f1.id = 1;
  f1.name = "Blue";
  st.factions[f1.id] = f1;

  Faction f2;
  f2.id = 2;
  f2.name = "Red";
  st.factions[f2.id] = f2;

  // One empty system.
  StarSystem sys;
  sys.id = 1;
  sys.name = "Sys";
  sys.galaxy_pos = Vec2{0.0, 0.0};
  st.systems[sys.id] = sys;

  // Two ships in the same system, within range.
  Ship s1;
  s1.id = 10;
  s1.name = "Blue-1";
  s1.faction_id = f1.id;
  s1.design_id = blue.id;
  s1.system_id = sys.id;
  s1.position_mkm = Vec2{0.0, 0.0};
  st.ships[s1.id] = s1;

  Ship s2;
  s2.id = 20;
  s2.name = "Red-1";
  s2.faction_id = f2.id;
  s2.design_id = red.id;
  s2.system_id = sys.id;
  s2.position_mkm = Vec2{10.0, 0.0};
  st.ships[s2.id] = s2;

  // System ship list is used by sensors/detection.
  st.systems[sys.id].ships.push_back(s1.id);
  st.systems[sys.id].ships.push_back(s2.id);

  sim.load_game(st);

  // Neutral stance should prevent auto-engagement.
  N4X_ASSERT(sim.set_diplomatic_status(f1.id, f2.id, DiplomacyStatus::Neutral, /*reciprocal=*/true, /*push_event=*/false),
             "set_diplomatic_status(neutral) succeeds");
  N4X_ASSERT(!sim.are_factions_hostile(f1.id, f2.id), "neutral means not hostile (A->B)");
  N4X_ASSERT(!sim.are_factions_hostile(f2.id, f1.id), "neutral means not hostile (B->A)");

  const double hp1 = sim.state().ships.at(s1.id).hp;
  const double hp2 = sim.state().ships.at(s2.id).hp;

  sim.advance_days(1);

  N4X_ASSERT(std::abs(sim.state().ships.at(s1.id).hp - hp1) < 1e-9, "no combat damage while neutral (ship 1)");
  N4X_ASSERT(std::abs(sim.state().ships.at(s2.id).hp - hp2) < 1e-9, "no combat damage while neutral (ship 2)");

  // Issuing an Attack order against a non-hostile target should escalate to Hostile when contact is confirmed.
  N4X_ASSERT(sim.issue_attack_ship(s1.id, s2.id, /*fog_of_war=*/false), "issue_attack_ship succeeds");
  sim.advance_days(1);

  N4X_ASSERT(sim.are_factions_hostile(f1.id, f2.id), "attack order escalates to hostile (A->B)");
  N4X_ASSERT(sim.are_factions_hostile(f2.id, f1.id), "attack order escalates to hostile (B->A)");
  N4X_ASSERT(sim.state().ships.at(s1.id).hp < hp1 || sim.state().ships.at(s2.id).hp < hp2, "combat damage occurred after escalation");

  // Serialization roundtrip preserves non-hostile relation entries.
  N4X_ASSERT(sim.set_diplomatic_status(f1.id, f2.id, DiplomacyStatus::Friendly, /*reciprocal=*/true, /*push_event=*/false),
             "set_diplomatic_status(friendly) succeeds");

  const std::string json = serialize_game_to_json(sim.state());
  const GameState loaded = deserialize_game_from_json(json);

  auto it1 = loaded.factions.find(f1.id);
  auto it2 = loaded.factions.find(f2.id);
  N4X_ASSERT(it1 != loaded.factions.end(), "loaded factions contains f1");
  N4X_ASSERT(it2 != loaded.factions.end(), "loaded factions contains f2");
  N4X_ASSERT(it1->second.relations.at(f2.id) == DiplomacyStatus::Friendly, "friendly relation A->B preserved");
  N4X_ASSERT(it2->second.relations.at(f1.id) == DiplomacyStatus::Friendly, "friendly relation B->A preserved");

  // Treaties override hostility and persist through serialization.
  N4X_ASSERT(sim.set_diplomatic_status(f1.id, f2.id, DiplomacyStatus::Hostile, /*reciprocal=*/true, /*push_event=*/false),
             "reset stance to hostile");

  std::string treaty_err;
  const Id tid = sim.create_treaty(f1.id, f2.id, TreatyType::Ceasefire, /*duration_days=*/3, /*push_event=*/false, &treaty_err);
  N4X_ASSERT(tid != kInvalidId, std::string("create_treaty succeeds: ") + treaty_err);
  N4X_ASSERT(sim.diplomatic_status(f1.id, f2.id) == DiplomacyStatus::Neutral, "ceasefire forces at least neutral");
  N4X_ASSERT(!sim.are_factions_hostile(f1.id, f2.id), "ceasefire means not hostile (A->B)");
  N4X_ASSERT(!sim.are_factions_hostile(f2.id, f1.id), "ceasefire means not hostile (B->A)");

  const std::string json2 = serialize_game_to_json(sim.state());
  const GameState loaded2 = deserialize_game_from_json(json2);
  N4X_ASSERT(loaded2.treaties.size() == 1, "treaty serialized");
  const auto it_t = loaded2.treaties.find(tid);
  N4X_ASSERT(it_t != loaded2.treaties.end(), "treaty id preserved");
  const Id lo = (f1.id < f2.id) ? f1.id : f2.id;
  const Id hi = (f1.id < f2.id) ? f2.id : f1.id;
  N4X_ASSERT(it_t->second.faction_a == lo, "treaty faction_a normalized");
  N4X_ASSERT(it_t->second.faction_b == hi, "treaty faction_b normalized");
  N4X_ASSERT(it_t->second.type == TreatyType::Ceasefire, "treaty type preserved");
  N4X_ASSERT(it_t->second.duration_days == 3, "treaty duration preserved");

  // Expiration (duration measured in whole days from creation).
  sim.advance_days(3);
  N4X_ASSERT(sim.treaties_between(f1.id, f2.id).empty(), "treaty expired and removed");
  N4X_ASSERT(sim.diplomatic_status(f1.id, f2.id) == DiplomacyStatus::Hostile, "after expiry, default hostility returns");

  return 0;
}
