#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

#include "nebula4x/core/serialization.h"
#include "nebula4x/core/simulation.h"
#include "nebula4x/util/json.h"

#define N4X_ASSERT(expr) \
  do { \
    if (!(expr)) { \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
      return 1; \
    } \
  } while (0)

int test_anomaly_discovery() {
  // Minimal content DB for a scout with sensors and mobility.
  nebula4x::ContentDB content;

  nebula4x::ShipDesign scout;
  scout.id = "scout";
  scout.name = "Scout";
  scout.mass_tons = 50;
  scout.speed_km_s = 200;          // ~17.28 mkm/day
  scout.sensor_range_mkm = 5.0;    // Passive sensor range
  scout.fuel_capacity_tons = 1000.0;
  content.designs[scout.id] = scout;

  nebula4x::SimConfig cfg;
  cfg.anomaly_detection_range_multiplier = 1.0;  // Use strict sensor range for this test.

  nebula4x::Simulation sim(std::move(content), cfg);

  nebula4x::GameState s;

  // One system.
  nebula4x::StarSystem sys;
  sys.id = 1;
  sys.name = "Test System";
  sys.galaxy_pos = nebula4x::Vec2{0.0, 0.0};
  s.systems[sys.id] = sys;

  // One faction that already discovered the system.
  nebula4x::Faction f;
  f.id = 1;
  f.name = "Testers";
  f.control = nebula4x::FactionControl::Player;
  f.discovered_systems.push_back(sys.id);
  s.factions[f.id] = f;

  // One anomaly, initially outside strict sensor range.
  nebula4x::Anomaly a;
  a.id = 1;
  a.system_id = sys.id;
  a.name = "Mysterious Signal";
  a.kind = nebula4x::AnomalyKind::Signal;
  a.position_mkm = nebula4x::Vec2{10.0, 0.0};
  a.investigation_days = 5;
  s.anomalies[a.id] = a;

  // One ship with sensors.
  nebula4x::Ship sh;
  sh.id = 1;
  sh.name = "Scout-1";
  sh.faction_id = f.id;
  sh.system_id = sys.id;
  sh.position_mkm = nebula4x::Vec2{0.0, 0.0};
  sh.design_id = scout.id;
  sh.fuel_tons = 1000.0;
  s.ships[sh.id] = sh;

  // Link ship to system.
  s.systems[sys.id].ships.push_back(sh.id);

  sim.load_game(s);

  // Not discovered yet (out of sensor range).
  N4X_ASSERT(!sim.is_anomaly_discovered_by_faction(f.id, a.id));
  N4X_ASSERT(sim.state().factions.at(f.id).discovered_anomalies.empty());

  // Move the ship onto the anomaly position.
  N4X_ASSERT(sim.issue_move_to_point(sh.id, a.position_mkm));
  sim.advance_days(1);

  // Now the anomaly should be discovered.
  N4X_ASSERT(sim.is_anomaly_discovered_by_faction(f.id, a.id));
  const auto& da = sim.state().factions.at(f.id).discovered_anomalies;
  N4X_ASSERT(std::find(da.begin(), da.end(), a.id) != da.end());

  // Round-trip serialization should preserve discovered anomalies.
  {
    const std::string json_text = nebula4x::serialize_game_to_json(sim.state());
    const auto loaded = nebula4x::deserialize_game_from_json(json_text);
    const auto itf = loaded.factions.find(f.id);
    N4X_ASSERT(itf != loaded.factions.end());
    const auto& da2 = itf->second.discovered_anomalies;
    N4X_ASSERT(std::find(da2.begin(), da2.end(), a.id) != da2.end());
  }

  // Backwards compatibility: old saves (save_version < 48) may not have
  // discovered_anomalies. They should be backfilled based on discovered systems.
  {
    const std::string json_text = nebula4x::serialize_game_to_json(sim.state());
    nebula4x::json::Value root = nebula4x::json::parse(json_text);
    auto* root_obj = root.as_object();
    N4X_ASSERT(root_obj != nullptr);

    // Simulate an older save.
    (*root_obj)["save_version"] = 47.0;

    // Remove discovered_anomalies from all factions.
    auto it_factions = root_obj->find("factions");
    N4X_ASSERT(it_factions != root_obj->end());
    auto* factions_arr = it_factions->second.as_array();
    N4X_ASSERT(factions_arr != nullptr);
    for (auto& fv : *factions_arr) {
      auto* fo = fv.as_object();
      N4X_ASSERT(fo != nullptr);
      fo->erase("discovered_anomalies");
    }

    const std::string legacy_json = nebula4x::json::stringify(root, 2);
    const auto loaded_legacy = nebula4x::deserialize_game_from_json(legacy_json);

    const auto itf = loaded_legacy.factions.find(f.id);
    N4X_ASSERT(itf != loaded_legacy.factions.end());
    const auto& da3 = itf->second.discovered_anomalies;
    N4X_ASSERT(std::find(da3.begin(), da3.end(), a.id) != da3.end());
  }

  return 0;
}
