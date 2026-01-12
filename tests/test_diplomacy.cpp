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

  // While a treaty is active, hostile orders should be blocked.
  N4X_ASSERT(!sim.issue_attack_ship(s1.id, s2.id, /*fog_of_war=*/false), "issue_attack_ship blocked by ceasefire");

  // Advancing time should not break the treaty due to pre-existing queued hostile orders.
  sim.advance_days(1);
  N4X_ASSERT(!sim.treaties_between(f1.id, f2.id).empty(), "treaty still active after 1 day");
  N4X_ASSERT(sim.diplomatic_status(f1.id, f2.id) != DiplomacyStatus::Hostile, "still not hostile under ceasefire");

  // The previously queued AttackShip order should have been cancelled by the ceasefire.
  {
    const auto it_so = sim.state().ship_orders.find(s1.id);
    N4X_ASSERT(it_so != sim.state().ship_orders.end(), "ship orders exist for s1");
    N4X_ASSERT(it_so->second.queue.empty(), "queued hostile orders cleared under ceasefire");
  }

  // Expiration (duration measured in whole days from creation).
  sim.advance_days(2);
  N4X_ASSERT(sim.treaties_between(f1.id, f2.id).empty(), "treaty expired and removed");
  N4X_ASSERT(sim.diplomatic_status(f1.id, f2.id) == DiplomacyStatus::Hostile, "after expiry, default hostility returns");

  // --- Treaty intel sharing (Alliance vs Trade Agreement) ---
  // Alliances should immediately exchange star charts *and* contact intel.
  // Trade agreements should exchange star charts but not share contacts.
  {
    ContentDB c2;
    Simulation sim2(c2, SimConfig{});

    GameState st2;
    st2.save_version = GameState{}.save_version;

    Faction a;
    a.id = 1;
    a.name = "Alpha";
    st2.factions[a.id] = a;

    Faction b;
    b.id = 2;
    b.name = "Beta";
    st2.factions[b.id] = b;

    // Two systems; Alpha has a ship in Sys-2 so only Alpha knows it initially.
    StarSystem sys1;
    sys1.id = 1;
    sys1.name = "Sys-1";
    st2.systems[sys1.id] = sys1;

    StarSystem sys2;
    sys2.id = 2;
    sys2.name = "Sys-2";
    st2.systems[sys2.id] = sys2;

    // A jump point in Sys-2 so survey sharing can be tested.
    JumpPoint jp;
    jp.id = 500;
    jp.name = "JP";
    jp.system_id = sys2.id;
    st2.jump_points[jp.id] = jp;
    st2.systems[sys2.id].jump_points.push_back(jp.id);

    // Alpha's ship in Sys-2.
    Ship sa;
    sa.id = 10;
    sa.name = "A";
    sa.faction_id = a.id;
    sa.system_id = sys2.id;
    st2.ships[sa.id] = sa;
    st2.systems[sys2.id].ships.push_back(sa.id);

    // A third-party ship to serve as a contact that can be shared.
    Ship sx;
    sx.id = 300;
    sx.name = "X";
    sx.faction_id = 3;
    sx.system_id = sys2.id;
    st2.ships[sx.id] = sx;
    st2.systems[sys2.id].ships.push_back(sx.id);

    // Alpha has an intel contact for the third-party ship.
    Contact cx;
    cx.ship_id = sx.id;
    cx.system_id = sys2.id;
    cx.last_seen_day = 0;
    cx.last_seen_faction_id = sx.faction_id;
    cx.last_seen_position_mkm = Vec2{0.0, 0.0};
    st2.factions[a.id].ship_contacts[cx.ship_id] = cx;

    sim2.load_game(st2);

    // Precondition: Beta does not know Sys-2 and has no contact.
    {
      const auto& db = sim2.state().factions.at(b.id).discovered_systems;
      N4X_ASSERT(std::find(db.begin(), db.end(), sys2.id) == db.end(), "pre: trade target system is unknown to Beta");
    }
    N4X_ASSERT(sim2.state().factions.at(b.id).ship_contacts.find(sx.id) == sim2.state().factions.at(b.id).ship_contacts.end(),
               "pre: contact not present for Beta");

    std::string err;
    const Id t_alliance = sim2.create_treaty(a.id, b.id, TreatyType::Alliance, /*duration_days=*/-1, /*push_event=*/false, &err);
    N4X_ASSERT(t_alliance != kInvalidId, std::string("create_treaty(alliance) succeeds: ") + err);

    // Alliance should exchange charts and contacts immediately.
    {
      const auto& db = sim2.state().factions.at(b.id).discovered_systems;
      N4X_ASSERT(std::find(db.begin(), db.end(), sys2.id) != db.end(), "alliance shares discovered systems");
    }
    {
      const auto& sj = sim2.state().factions.at(b.id).surveyed_jump_points;
      N4X_ASSERT(std::find(sj.begin(), sj.end(), jp.id) != sj.end(), "alliance shares jump surveys");
    }
    N4X_ASSERT(sim2.state().factions.at(b.id).ship_contacts.find(sx.id) != sim2.state().factions.at(b.id).ship_contacts.end(),
               "alliance shares contacts");
  }

  {
    ContentDB c3;
    Simulation sim3(c3, SimConfig{});

    GameState st3;
    st3.save_version = GameState{}.save_version;

    Faction a;
    a.id = 1;
    a.name = "Alpha";
    st3.factions[a.id] = a;

    Faction b;
    b.id = 2;
    b.name = "Beta";
    st3.factions[b.id] = b;

    StarSystem sys1;
    sys1.id = 1;
    sys1.name = "Sys-1";
    st3.systems[sys1.id] = sys1;

    StarSystem sys2;
    sys2.id = 2;
    sys2.name = "Sys-2";
    st3.systems[sys2.id] = sys2;

    JumpPoint jp;
    jp.id = 500;
    jp.name = "JP";
    jp.system_id = sys2.id;
    st3.jump_points[jp.id] = jp;
    st3.systems[sys2.id].jump_points.push_back(jp.id);

    Ship sa;
    sa.id = 10;
    sa.name = "A";
    sa.faction_id = a.id;
    sa.system_id = sys2.id;
    st3.ships[sa.id] = sa;
    st3.systems[sys2.id].ships.push_back(sa.id);

    Ship sx;
    sx.id = 300;
    sx.name = "X";
    sx.faction_id = 3;
    sx.system_id = sys2.id;
    st3.ships[sx.id] = sx;
    st3.systems[sys2.id].ships.push_back(sx.id);

    Contact cx;
    cx.ship_id = sx.id;
    cx.system_id = sys2.id;
    cx.last_seen_day = 0;
    cx.last_seen_faction_id = sx.faction_id;
    cx.last_seen_position_mkm = Vec2{0.0, 0.0};
    st3.factions[a.id].ship_contacts[cx.ship_id] = cx;

    sim3.load_game(st3);

    std::string err;
    const Id t_trade = sim3.create_treaty(a.id, b.id, TreatyType::TradeAgreement, /*duration_days=*/-1, /*push_event=*/false, &err);
    N4X_ASSERT(t_trade != kInvalidId, std::string("create_treaty(trade) succeeds: ") + err);

    // Trade agreement should share charts, but not contacts.
    {
      const auto& db = sim3.state().factions.at(b.id).discovered_systems;
      N4X_ASSERT(std::find(db.begin(), db.end(), sys2.id) != db.end(), "trade shares discovered systems");
    }
    {
      const auto& sj = sim3.state().factions.at(b.id).surveyed_jump_points;
      N4X_ASSERT(std::find(sj.begin(), sj.end(), jp.id) != sj.end(), "trade shares jump surveys");
    }
    N4X_ASSERT(sim3.state().factions.at(b.id).ship_contacts.find(sx.id) == sim3.state().factions.at(b.id).ship_contacts.end(),
               "trade does not share contacts");
  }


  {
    ContentDB c4;

    ShipDesign hauler;
    hauler.id = "hauler";
    hauler.name = "Hauler";
    hauler.role = ShipRole::Freighter;
    hauler.mass_tons = 100.0;
    hauler.max_hp = 100.0;
    hauler.cargo_tons = 100.0;
    hauler.fuel_capacity_tons = 100.0;
    hauler.missile_ammo_capacity = 10.0;
    c4.designs[hauler.id] = hauler;

    Simulation sim4(c4, SimConfig{});

    GameState st4;
    st4.save_version = GameState{}.save_version;

    Faction a;
    a.id = 1;
    a.name = "Alpha";
    st4.factions[a.id] = a;

    Faction b;
    b.id = 2;
    b.name = "Beta";
    st4.factions[b.id] = b;

    StarSystem sys;
    sys.id = 1;
    sys.name = "Sys";
    st4.systems[sys.id] = sys;

    Body body;
    body.id = 10;
    body.name = "Body";
    body.system_id = sys.id;
    body.position_mkm = Vec2{0.0, 0.0};
    st4.bodies[body.id] = body;
    st4.systems[sys.id].bodies.push_back(body.id);

    Colony col;
    col.id = 100;
    col.name = "Beta Colony";
    col.faction_id = b.id;
    col.body_id = body.id;
    col.minerals["Duranium"] = 100.0;
    col.minerals["Fuel"] = 1000.0;
    col.minerals["Munitions"] = 1000.0;
    st4.colonies[col.id] = col;

    Ship sh;
    sh.id = 200;
    sh.name = "Hauler";
    sh.faction_id = a.id;
    sh.design_id = hauler.id;
    sh.system_id = sys.id;
    sh.position_mkm = Vec2{0.0, 0.0};
    sh.fuel_tons = 0.0;
    sh.missile_ammo = 0.0;
    st4.ships[sh.id] = sh;
    st4.systems[sys.id].ships.push_back(sh.id);

    sim4.load_game(st4);

    // Without a treaty or alliance, the colony is not a trade partner.
    N4X_ASSERT(!sim4.are_factions_trade_partners(a.id, b.id), "no trade access by default");

    // Without trade access, mineral transfers are blocked.
    N4X_ASSERT(!sim4.issue_load_mineral(sh.id, col.id, "Duranium", 10.0, /*restrict_to_discovered=*/false),
               "load mineral blocked without trade access");

    std::string err;
    const Id tid = sim4.create_treaty(a.id, b.id, TreatyType::TradeAgreement, /*duration_days=*/-1, /*push_event=*/false, &err);
    N4X_ASSERT(tid != kInvalidId, std::string("create_treaty(trade) succeeds: ") + err);

    N4X_ASSERT(sim4.are_factions_trade_partners(a.id, b.id), "trade agreement grants trade access");

    // With a trade agreement, mineral transfers and port logistics should be allowed.
    N4X_ASSERT(sim4.issue_load_mineral(sh.id, col.id, "Duranium", 10.0, /*restrict_to_discovered=*/false),
               "load mineral allowed under trade agreement");

    const double dur_before = sim4.state().colonies.at(col.id).minerals.at("Duranium");
    const double fuel_before = sim4.state().colonies.at(col.id).minerals.at("Fuel");
    const double mun_before = sim4.state().colonies.at(col.id).minerals.at("Munitions");

    sim4.advance_days(1);

    const auto& st_after = sim4.state();
    const auto& sh_after = st_after.ships.at(sh.id);
    const auto& col_after = st_after.colonies.at(col.id);

    // Mineral transfer occurred.
    {
      const auto it = sh_after.cargo.find("Duranium");
      N4X_ASSERT(it != sh_after.cargo.end() && it->second > 0.0, "trade partner mineral load transfers cargo");
    }
    N4X_ASSERT(col_after.minerals.at("Duranium") < dur_before, "trade partner mineral load reduces colony stockpile");

    // Port logistics occurred (refuel + rearm).
    N4X_ASSERT(sh_after.fuel_tons > 0.0, "trade partner refuels ship");
    N4X_ASSERT(sh_after.missile_ammo > 0.0, "trade partner rearms ship");
    N4X_ASSERT(col_after.minerals.at("Fuel") < fuel_before, "refuel consumes colony fuel");
    N4X_ASSERT(col_after.minerals.at("Munitions") < mun_before, "rearm consumes colony munitions");
  }


  // --- Diplomatic offers ---
  {
    ContentDB content;
    SimConfig cfg;
    Simulation sim(content, cfg);

    GameState st;
    st.save_version = GameState{}.save_version;

    Faction a;
    a.id = 1;
    a.name = "Alpha";
    a.control = FactionControl::Player;

    Faction b;
    b.id = 2;
    b.name = "Beta";
    b.control = FactionControl::AI_Explorer;

    st.factions[a.id] = a;
    st.factions[b.id] = b;

    sim.load_game(st);

    std::string err;

    const Id oid = sim.create_diplomatic_offer(a.id, b.id, TreatyType::NonAggressionPact,
                                               /*treaty_duration_days=*/180,
                                               /*offer_expires_in_days=*/30,
                                               /*push_event=*/false, &err);
    N4X_ASSERT(oid != kInvalidId, std::string("Failed to create diplomatic offer: ") + err);
    N4X_ASSERT(sim.state().diplomatic_offers.size() == 1u, "Offer not stored in state");

    // Save/load roundtrip should preserve offers.
    const std::string json = serialize_game_to_json(sim.state());
    GameState loaded = deserialize_game_from_json(json);
    N4X_ASSERT(loaded.diplomatic_offers.size() == 1u, "Offer not serialized/deserialized");
    const auto it = loaded.diplomatic_offers.find(oid);
    N4X_ASSERT(it != loaded.diplomatic_offers.end(), "Offer id not preserved");
    N4X_ASSERT(it->second.from_faction_id == a.id, "Offer from_faction_id mismatch");
    N4X_ASSERT(it->second.to_faction_id == b.id, "Offer to_faction_id mismatch");
    N4X_ASSERT(it->second.treaty_type == TreatyType::NonAggressionPact, "Offer treaty_type mismatch");

    // Accept should create a treaty and remove the offer.
    const bool ok = sim.accept_diplomatic_offer(oid, /*push_event=*/false, &err);
    N4X_ASSERT(ok, std::string("Failed to accept offer: ") + err);
    N4X_ASSERT(sim.state().diplomatic_offers.empty(), "Offer not removed after accept");

    auto treaties = sim.treaties_between(a.id, b.id);
    N4X_ASSERT(!treaties.empty(), "Accepting offer did not create treaty");
    N4X_ASSERT(treaties[0].type == TreatyType::NonAggressionPact, "Created wrong treaty type");

    // Expiry tick: create an offer that expires quickly and ensure it is removed by ticking.
    const Id oid2 = sim.create_diplomatic_offer(a.id, b.id, TreatyType::TradeAgreement,
                                                /*treaty_duration_days=*/-1,
                                                /*offer_expires_in_days=*/1,
                                                /*push_event=*/false, &err);
    N4X_ASSERT(oid2 != kInvalidId, std::string("Failed to create second offer: ") + err);
    sim.advance_days(2);
    N4X_ASSERT(sim.state().diplomatic_offers.find(oid2) == sim.state().diplomatic_offers.end(),
               "Offer did not expire as expected");
  }

  return 0;
}
