#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

#include "nebula4x/core/simulation.h"
#include "nebula4x/core/serialization.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";  \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

int test_anomalies() {
  using namespace nebula4x;

  // Minimal content (design + a component to unlock).
  ContentDB content;
  {
    ShipDesign d;
    d.id = "scout";
    d.name = "Scout";
    d.speed_km_s = 100.0;
    d.sensor_range_mkm = 10.0;
    d.cargo_tons = 50.0;
    d.max_hp = 10.0;
    content.designs[d.id] = d;
  }
  {
    ShipDesign d;
    d.id = "blind";
    d.name = "Blind Hull";
    d.speed_km_s = 100.0;
    d.sensor_range_mkm = 0.0;
    d.cargo_tons = 50.0;
    d.max_hp = 10.0;
    content.designs[d.id] = d;
  }
  {
    ComponentDef c;
    c.id = "anomaly_comp";
    c.name = "Recovered Xeno Sensor";
    c.type = ComponentType::Sensor;
    c.sensor_range_mkm = 25.0;
    content.components[c.id] = c;
  }

  Simulation sim(std::move(content), SimConfig{});

  // Build a minimal state with a single system, faction, ship, and anomaly.
  GameState s;
  s.save_version = GameState{}.save_version;
  s.date = Date::from_ymd(2200, 1, 1);
  s.next_id = 1;

  const Id fac_id = allocate_id(s);
  {
    Faction f;
    f.id = fac_id;
    f.name = "Faction";
    f.research_queue.clear();
    s.factions[fac_id] = f;
  }

  const Id sys_id = allocate_id(s);
  {
    StarSystem sys;
    sys.id = sys_id;
    sys.name = "Test System";
    sys.galaxy_pos = {0.0, 0.0};
    s.systems[sys_id] = sys;
  }
  s.factions[fac_id].discovered_systems = {sys_id};

  const Id ship_id = allocate_id(s);
  {
    Ship sh;
    sh.id = ship_id;
    sh.name = "Scout";
    sh.faction_id = fac_id;
    sh.system_id = sys_id;
    sh.position_mkm = {0.0, 0.0};
    sh.design_id = "scout";
    s.ships[ship_id] = sh;
    s.systems[sys_id].ships = {ship_id};
  }

  const Id blind_ship_id = allocate_id(s);
  {
    Ship sh;
    sh.id = blind_ship_id;
    sh.name = "Blind";
    sh.faction_id = fac_id;
    sh.system_id = sys_id;
    sh.position_mkm = {0.0, 0.0};
    sh.design_id = "blind";
    s.ships[blind_ship_id] = sh;
    s.systems[sys_id].ships.push_back(blind_ship_id);
  }

  const Id anom_id = allocate_id(s);
  {
    Anomaly a;
    a.id = anom_id;
    a.system_id = sys_id;
    a.position_mkm = {0.0, 0.0};
    a.name = "Test Anomaly";
    a.kind = AnomalyKind::Signal;
    a.investigation_days = 1;
    a.research_reward = 123.0;
    a.unlock_component_id = "anomaly_comp";
    a.mineral_reward = {{"Duranium", 40.0}, {"Neutronium", 30.0}};
    a.hazard_chance = 1.0;
    a.hazard_damage = 3.0;
    s.anomalies[anom_id] = a;
  }

  // Roundtrip through JSON to exercise anomaly serialization (rewards/hazards).
  const std::string json = serialize_game_to_json(s);
  GameState s2 = deserialize_game_from_json(json);

  sim.load_game(std::move(s2));

  // Ships without sensors cannot take investigation orders.
  N4X_ASSERT(sim.clear_orders(blind_ship_id));
  N4X_ASSERT(!sim.issue_investigate_anomaly(blind_ship_id, anom_id, /*restrict_to_discovered=*/false));

  // Issue the investigation order and advance time until completion.
  N4X_ASSERT(sim.clear_orders(ship_id));
  N4X_ASSERT(sim.issue_investigate_anomaly(ship_id, anom_id, /*restrict_to_discovered=*/false));

  sim.advance_days(3);

  const auto* a = find_ptr(sim.state().anomalies, anom_id);
  N4X_ASSERT(a);
  N4X_ASSERT(a->resolved);
  N4X_ASSERT(a->resolved_by_faction_id == fac_id);

  const auto* fac = find_ptr(sim.state().factions, fac_id);
  N4X_ASSERT(fac);
  N4X_ASSERT(fac->research_points >= 123.0 - 1e-6);

  N4X_ASSERT(std::find(fac->unlocked_components.begin(), fac->unlocked_components.end(), "anomaly_comp") !=
             fac->unlocked_components.end());

  const auto* sh = find_ptr(sim.state().ships, ship_id);
  N4X_ASSERT(sh);

  // Minerals: 50t cargo cap should load all 40t Duranium and 10t Neutronium; remaining 20t Neutronium becomes a cache wreck.
  {
    const double dur = sh->cargo.contains("Duranium") ? sh->cargo.at("Duranium") : 0.0;
    const double neu = sh->cargo.contains("Neutronium") ? sh->cargo.at("Neutronium") : 0.0;
    N4X_ASSERT(std::abs(dur - 40.0) < 1e-6);
    N4X_ASSERT(std::abs(neu - 10.0) < 1e-6);
  }

  N4X_ASSERT(sim.state().wrecks.size() == 1);
  {
    const auto& w = sim.state().wrecks.begin()->second;
    const double left = w.minerals.contains("Neutronium") ? w.minerals.at("Neutronium") : 0.0;
    N4X_ASSERT(std::abs(left - 20.0) < 1e-6);

    // The overflow is stored as a mineral cache wreck, not a ship hull wreck.
    // It should not carry source ship/design metadata that could accidentally
    // enable reverse-engineering when salvaged by another faction.
    N4X_ASSERT(w.kind == WreckKind::Cache);
    N4X_ASSERT(w.source_ship_id == kInvalidId);
    N4X_ASSERT(w.source_faction_id == kInvalidId);
    N4X_ASSERT(w.source_design_id.empty());
  }

  // Hazard: 3 damage should be applied non-lethally to the ship (no shields in this test).
  N4X_ASSERT(std::abs(sh->hp - 7.0) < 1e-6);

  const auto& q = sim.state().ship_orders.at(ship_id).queue;
  N4X_ASSERT(q.empty());

  return 0;
}
