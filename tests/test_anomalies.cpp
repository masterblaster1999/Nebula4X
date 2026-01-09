#include <algorithm>
#include <iostream>
#include <string>

#include "nebula4x/core/simulation.h"

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

  const Id anom_id = allocate_id(s);
  {
    Anomaly a;
    a.id = anom_id;
    a.system_id = sys_id;
    a.position_mkm = {0.0, 0.0};
    a.name = "Test Anomaly";
    a.kind = "signal";
    a.investigation_days = 2;
    a.research_reward = 123.0;
    a.unlock_component_id = "anomaly_comp";
    s.anomalies[anom_id] = a;
  }

  sim.load_game(std::move(s));

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

  const auto& q = sim.state().ship_orders.at(ship_id).queue;
  N4X_ASSERT(q.empty());

  return 0;
}
