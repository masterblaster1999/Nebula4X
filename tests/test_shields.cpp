#include <algorithm>
#include <iostream>
#include <string>

#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

namespace {

nebula4x::Id find_ship_id(const nebula4x::GameState& st, const std::string& name) {
  for (const auto& [sid, sh] : st.ships) {
    if (sh.name == name) return sid;
  }
  return nebula4x::kInvalidId;
}

nebula4x::Id find_system_id(const nebula4x::GameState& st, const std::string& name) {
  for (const auto& [sid, sys] : st.systems) {
    if (sys.name == name) return sid;
  }
  return nebula4x::kInvalidId;
}

void remove_ship_from_all_system_lists(nebula4x::GameState& st, nebula4x::Id ship_id) {
  for (auto& [_, sys] : st.systems) {
    sys.ships.erase(std::remove(sys.ships.begin(), sys.ships.end(), ship_id), sys.ships.end());
  }
}

}  // namespace

int test_shields() {
  // Verify that shields absorb damage before hull, and that shields recharge over time.
  nebula4x::ContentDB content;

  // Minimal installations referenced by the default scenario.
  for (const char* id : {"automated_mine", "construction_factory", "shipyard", "research_lab", "sensor_station"}) {
    nebula4x::InstallationDef def;
    def.id = id;
    def.name = id;
    content.installations[id] = def;
  }

  auto make_min_design = [](const std::string& id) {
    nebula4x::ShipDesign d;
    d.id = id;
    d.name = id;
    d.max_hp = 50.0;
    d.speed_km_s = 0.0;
    d.sensor_range_mkm = 0.0;
    return d;
  };

  // Scenario ships.
  content.designs["freighter_alpha"] = make_min_design("freighter_alpha");
  content.designs["surveyor_beta"] = make_min_design("surveyor_beta");

  // Escort with shields (the defender).
  {
    nebula4x::ShipDesign d;
    d.id = "escort_gamma";
    d.name = "Escort Gamma";
    d.max_hp = 50.0;
    d.speed_km_s = 0.0;
    d.sensor_range_mkm = 1000.0;
    d.weapon_damage = 0.0;      // keep the raider alive for the test
    d.weapon_range_mkm = 5.0;
    d.max_shields = 10.0;
    d.shield_regen_per_day = 2.0;
    content.designs[d.id] = d;
  }

  // Pirate raider with a small weapon.
  {
    nebula4x::ShipDesign d;
    d.id = "pirate_raider";
    d.name = "Pirate Raider";
    d.max_hp = 50.0;
    d.speed_km_s = 0.0;
    d.sensor_range_mkm = 1000.0;
    d.weapon_damage = 3.0;
    d.weapon_range_mkm = 5.0;
    content.designs[d.id] = d;
  }

  // Minimal techs referenced by the scenario.
  for (const char* id : {"chemistry_1", "nuclear_1", "propulsion_1"}) {
    nebula4x::TechDef t;
    t.id = id;
    t.name = id;
    t.cost = 1e9;
    content.techs[id] = t;
  }

  nebula4x::SimConfig cfg;
  cfg.combat_damage_event_min_abs = 0.0;
  cfg.combat_damage_event_min_fraction = 0.0;
  cfg.combat_damage_event_warn_remaining_fraction = 0.0;  // always Info

  nebula4x::Simulation sim(std::move(content), cfg);
  auto& st = sim.state();

  const auto sol = find_system_id(st, "Sol");
  N4X_ASSERT(sol != nebula4x::kInvalidId);

  const auto alpha = find_system_id(st, "Alpha Centauri");
  N4X_ASSERT(alpha != nebula4x::kInvalidId);

  const auto escort_id = find_ship_id(st, "Escort Gamma");
  N4X_ASSERT(escort_id != nebula4x::kInvalidId);

  const auto raider_id = find_ship_id(st, "Raider I");
  N4X_ASSERT(raider_id != nebula4x::kInvalidId);

  auto* escort = nebula4x::find_ptr(st.ships, escort_id);
  auto* raider = nebula4x::find_ptr(st.ships, raider_id);
  N4X_ASSERT(escort);
  N4X_ASSERT(raider);

  // Move the raider into Sol near the escort to force combat.
  remove_ship_from_all_system_lists(st, raider_id);
  raider->system_id = sol;
  raider->position_mkm = escort->position_mkm + nebula4x::Vec2{0.2, 0.0};
  st.systems[sol].ships.push_back(raider_id);

  // Sanity checks.
  N4X_ASSERT(escort->system_id == raider->system_id);
  N4X_ASSERT((escort->position_mkm - raider->position_mkm).length() < 5.0);

  const double hp0 = escort->hp;
  const double sh0 = escort->shields;

  // With the design shielded, shields should start at max (10) after design stats are applied.
  N4X_ASSERT(sh0 >= 9.9);

  sim.advance_days(1);

  const double hp1 = escort->hp;
  const double sh1 = escort->shields;

  // Day 1: shields should absorb all incoming damage, so hull stays intact.
  N4X_ASSERT(hp1 == hp0);
  N4X_ASSERT(sh1 < sh0);

  // Move the raider away so we can observe shield recharge.
  remove_ship_from_all_system_lists(st, raider_id);
  raider->system_id = alpha;
  raider->position_mkm = {0.0, 0.0};
  st.systems[alpha].ships.push_back(raider_id);

  sim.advance_days(1);

  const double sh2 = escort->shields;
  N4X_ASSERT(sh2 > sh1);

  // Regen should be clamped to the max.
  N4X_ASSERT(sh2 <= sh0 + 1e-6);

  return 0;
}
