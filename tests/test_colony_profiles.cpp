#include <cmath>
#include <iostream>
#include <string>

#include "nebula4x/core/colony_profiles.h"
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

int test_colony_profiles() {
  using namespace nebula4x;

  ContentDB content;
  Simulation sim(content, SimConfig{});

  GameState st;
  st.save_version = GameState{}.save_version;

  // Faction with one profile.
  Faction f;
  f.id = 1;
  f.name = "Player";

  ColonyAutomationProfile p;
  p.garrison_target_strength = 250.0;
  p.mineral_reserves["Duranium"] = 500.0;
  p.mineral_targets["Duranium"] = 2000.0;
  p.installation_targets["mine"] = 12;

  f.colony_profiles["Core Worlds"] = p;
  st.factions[f.id] = f;

  // A colony belonging to the faction.
  Colony c;
  c.id = 10;
  c.name = "Earth";
  c.faction_id = f.id;
  c.body_id = 1;
  c.population_millions = 100.0;
  c.installation_targets["mine"] = 1;
  c.mineral_reserves["Duranium"] = 1.0;
  c.mineral_targets["Duranium"] = 1.0;
  c.garrison_target_strength = 10.0;
  st.colonies[c.id] = c;

  sim.load_game(st);

  // Serialization roundtrip preserves profiles.
  const std::string json = serialize_game_to_json(sim.state());
  const GameState loaded = deserialize_game_from_json(json);

  auto itf = loaded.factions.find(f.id);
  N4X_ASSERT(itf != loaded.factions.end(), "loaded factions contains f");
  auto itp = itf->second.colony_profiles.find("Core Worlds");
  N4X_ASSERT(itp != itf->second.colony_profiles.end(), "profile preserved");
  N4X_ASSERT(std::abs(itp->second.garrison_target_strength - 250.0) < 1e-9, "garrison target preserved");
  N4X_ASSERT(itp->second.mineral_reserves.count("Duranium") == 1, "mineral reserves preserved");
  N4X_ASSERT(std::abs(itp->second.mineral_reserves.at("Duranium") - 500.0) < 1e-9, "reserve value preserved");
  N4X_ASSERT(itp->second.mineral_targets.count("Duranium") == 1, "mineral targets preserved");
  N4X_ASSERT(std::abs(itp->second.mineral_targets.at("Duranium") - 2000.0) < 1e-9, "target value preserved");
  N4X_ASSERT(itp->second.installation_targets.count("mine") == 1, "installation targets preserved");
  N4X_ASSERT(itp->second.installation_targets.at("mine") == 12, "installation target value preserved");

  // Apply profile replaces settings (by default).
  Colony cc;
  cc.installation_targets["x"] = 1;
  cc.mineral_reserves["x"] = 1.0;
  cc.mineral_targets["x"] = 1.0;
  cc.garrison_target_strength = 0.0;

  apply_colony_profile(cc, itp->second);

  N4X_ASSERT(cc.installation_targets.size() == 1 && cc.installation_targets.at("mine") == 12, "apply sets installations");
  N4X_ASSERT(cc.mineral_reserves.size() == 1 && std::abs(cc.mineral_reserves.at("Duranium") - 500.0) < 1e-9,
             "apply sets reserves");
  N4X_ASSERT(cc.mineral_targets.size() == 1 && std::abs(cc.mineral_targets.at("Duranium") - 2000.0) < 1e-9,
             "apply sets targets");
  N4X_ASSERT(std::abs(cc.garrison_target_strength - 250.0) < 1e-9, "apply sets garrison target");

  // Sanitization: negative/NaN entries are dropped/clamped.
  ColonyAutomationProfile bad;
  bad.garrison_target_strength = -5.0;
  bad.installation_targets["bad_inst"] = -1;
  bad.mineral_targets["bad_min"] = -1.0;
  bad.mineral_reserves["nan"] = std::nan("");

  Colony cc2;
  apply_colony_profile(cc2, bad);

  N4X_ASSERT(cc2.garrison_target_strength == 0.0, "negative garrison clamps to 0");
  N4X_ASSERT(cc2.installation_targets.empty(), "negative installation targets dropped");
  N4X_ASSERT(cc2.mineral_targets.empty(), "negative mineral targets dropped");
  N4X_ASSERT(cc2.mineral_reserves.empty(), "NaN mineral reserves dropped");

  return 0;
}
