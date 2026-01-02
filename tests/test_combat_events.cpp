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

int test_combat_events() {
  // Verify that combat logs an event when ships take damage but survive.
  {
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

    // Armed escort + raider for the combat test.
    {
      nebula4x::ShipDesign d;
      d.id = "escort_gamma";
      d.name = "Escort Gamma";
      d.max_hp = 50.0;
      d.speed_km_s = 0.0;
      d.sensor_range_mkm = 1000.0;
      d.weapon_damage = 2.0;
      d.weapon_range_mkm = 5.0;
      content.designs[d.id] = d;
    }
    {
      nebula4x::ShipDesign d;
      d.id = "pirate_raider";
      d.name = "Pirate Raider";
      d.max_hp = 50.0;
      d.speed_km_s = 0.0;
      d.sensor_range_mkm = 1000.0;
      d.weapon_damage = 1.0;
      d.weapon_range_mkm = 5.0;
      content.designs[d.id] = d;
    }

    // Minimal techs referenced by the scenario (avoid churn in research logic).
    for (const char* id : {"chemistry_1", "nuclear_1", "propulsion_1"}) {
      nebula4x::TechDef t;
      t.id = id;
      t.name = id;
      t.cost = 1e9;  // very high so nothing completes during the test
      content.techs[id] = t;
    }

    nebula4x::SimConfig cfg;
    // Ensure events are emitted for the small damage values in this test.
    cfg.combat_damage_event_min_abs = 0.0;
    cfg.combat_damage_event_min_fraction = 0.0;
    cfg.combat_damage_event_warn_remaining_fraction = 0.0;  // always Info for this test

    nebula4x::Simulation sim(std::move(content), cfg);

    auto& st = sim.state();
    const auto sol = find_system_id(st, "Sol");
    N4X_ASSERT(sol != nebula4x::kInvalidId);

    const auto escort_id = find_ship_id(st, "Escort Gamma");
    N4X_ASSERT(escort_id != nebula4x::kInvalidId);

    const auto raider_id = find_ship_id(st, "Raider I");
    N4X_ASSERT(raider_id != nebula4x::kInvalidId);

    auto* escort = nebula4x::find_ptr(st.ships, escort_id);
    auto* raider = nebula4x::find_ptr(st.ships, raider_id);
    N4X_ASSERT(escort);
    N4X_ASSERT(raider);

    // Move the pirate raider into Sol near the escort to force combat.
    remove_ship_from_all_system_lists(st, raider_id);
    raider->system_id = sol;
    raider->position_mkm = escort->position_mkm + nebula4x::Vec2{0.2, 0.0};
    st.systems[sol].ships.push_back(raider_id);

    // Sanity: they should be in the same system and within weapon + sensor range.
    N4X_ASSERT(escort->system_id == raider->system_id);
    N4X_ASSERT((escort->position_mkm - raider->position_mkm).length() < 5.0);

    sim.advance_days(1);

    bool found_damage_event = false;
    for (const auto& ev : sim.state().events) {
      if (ev.category != nebula4x::EventCategory::Combat) continue;
      if (ev.level != nebula4x::EventLevel::Info) continue;
      if (ev.message.find("Ship damaged:") == std::string::npos) continue;
      found_damage_event = true;
      break;
    }

    N4X_ASSERT(found_damage_event);
  }

  
  
  
  // Missile salvos: basic launch + impact.
  {
    nebula4x::ContentDB content;

    {
      nebula4x::ShipDesign d;
      d.id = "escort_gamma";
      d.name = "Escort Gamma";
      d.max_hp = 50.0;
      d.speed_km_s = 0.0;
      d.sensor_range_mkm = 1000.0;
      d.weapon_damage = 0.0;
      d.weapon_range_mkm = 0.0;
      d.max_shields = 0.0;
      d.shield_regen_per_day = 0.0;
      d.point_defense_damage = 0.0;
      d.point_defense_range_mkm = 0.0;
      d.power_generation = 1000.0;
      d.power_use_weapons = 1.0;
      content.designs[d.id] = d;
    }
    {
      nebula4x::ShipDesign d;
      d.id = "pirate_raider";
      d.name = "Pirate Raider";
      d.max_hp = 50.0;
      d.speed_km_s = 0.0;
      d.sensor_range_mkm = 1000.0;
      d.weapon_damage = 0.0;
      d.weapon_range_mkm = 0.0;
      d.missile_damage = 10.0;
      d.missile_range_mkm = 5.0;
      // Keep the flight time ~1 day for a 0.2 mkm separation so the test
      // remains deterministic under sub-day combat ticks.
      d.missile_speed_mkm_per_day = 0.2;
      d.missile_reload_days = 1.0;
      d.power_generation = 1000.0;
      d.power_use_weapons = 1.0;
      content.designs[d.id] = d;
    }

    // Minimal techs referenced by the scenario (avoid churn in research logic).
    for (const char* id : {"chemistry_1", "nuclear_1", "propulsion_1"}) {
      nebula4x::TechDef t;
      t.id = id;
      t.name = id;
      t.cost = 1e9;  // very high so nothing completes during the test
      content.techs[id] = t;
    }

    nebula4x::SimConfig cfg;
    cfg.combat_damage_event_min_abs = 0.0;
    cfg.combat_damage_event_min_fraction = 0.0;
    cfg.combat_damage_event_warn_remaining_fraction = 0.0;

    nebula4x::Simulation sim(std::move(content), cfg);

    auto& st = sim.state();
    const auto sol = find_system_id(st, "Sol");
    N4X_ASSERT(sol != nebula4x::kInvalidId);

    const auto escort_id = find_ship_id(st, "Escort Gamma");
    N4X_ASSERT(escort_id != nebula4x::kInvalidId);

    const auto raider_id = find_ship_id(st, "Raider I");
    N4X_ASSERT(raider_id != nebula4x::kInvalidId);

    auto* escort = nebula4x::find_ptr(st.ships, escort_id);
    auto* raider = nebula4x::find_ptr(st.ships, raider_id);
    N4X_ASSERT(escort);
    N4X_ASSERT(raider);

    // Move the pirate raider into Sol near the escort to force missile combat.
    remove_ship_from_all_system_lists(st, raider_id);
    raider->system_id = sol;
    raider->position_mkm = escort->position_mkm + nebula4x::Vec2{0.2, 0.0};
    st.systems[sol].ships.push_back(raider_id);

    const double hp0 = escort->hp;

    // Day 1: missiles launched, no impact yet.
    sim.advance_days(1);

    N4X_ASSERT(!st.missile_salvos.empty());
    N4X_ASSERT(std::abs(escort->hp - hp0) < 1e-9);

    bool saw_launch = false;
    for (const auto& ev : sim.state().events) {
      if (ev.category != nebula4x::EventCategory::Combat) continue;
      if (ev.message.find("launched missiles") == std::string::npos) continue;
      saw_launch = true;
      break;
    }
    N4X_ASSERT(saw_launch);

    // Day 2: impact applies damage.
    sim.advance_days(1);

    bool saw_impact = false;
    for (const auto& ev : sim.state().events) {
      if (ev.category != nebula4x::EventCategory::Combat) continue;
      if (ev.message.find("Missile impacts on") == std::string::npos) continue;
      saw_impact = true;
      break;
    }
    N4X_ASSERT(saw_impact);
    N4X_ASSERT(escort->hp < hp0);
  }

  // Missile salvos: point defense interception (no HP damage).
  {
    nebula4x::ContentDB content;

    {
      nebula4x::ShipDesign d;
      d.id = "escort_gamma";
      d.name = "Escort Gamma";
      d.max_hp = 50.0;
      d.speed_km_s = 0.0;
      d.sensor_range_mkm = 1000.0;
      d.weapon_damage = 0.0;
      d.weapon_range_mkm = 0.0;
      d.max_shields = 0.0;
      d.shield_regen_per_day = 0.0;
      d.point_defense_damage = 100.0;
      d.point_defense_range_mkm = 1000.0;
      d.power_generation = 1000.0;
      d.power_use_weapons = 1.0;
      content.designs[d.id] = d;
    }
    {
      nebula4x::ShipDesign d;
      d.id = "pirate_raider";
      d.name = "Pirate Raider";
      d.max_hp = 50.0;
      d.speed_km_s = 0.0;
      d.sensor_range_mkm = 1000.0;
      d.weapon_damage = 0.0;
      d.weapon_range_mkm = 0.0;
      d.missile_damage = 10.0;
      d.missile_range_mkm = 5.0;
      // Keep the flight time ~1 day for a 0.2 mkm separation so the test
      // remains deterministic under sub-day combat ticks.
      d.missile_speed_mkm_per_day = 0.2;
      d.missile_reload_days = 1.0;
      d.power_generation = 1000.0;
      d.power_use_weapons = 1.0;
      content.designs[d.id] = d;
    }

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
    cfg.combat_damage_event_warn_remaining_fraction = 0.0;

    nebula4x::Simulation sim(std::move(content), cfg);

    auto& st = sim.state();
    const auto sol = find_system_id(st, "Sol");
    N4X_ASSERT(sol != nebula4x::kInvalidId);

    const auto escort_id = find_ship_id(st, "Escort Gamma");
    N4X_ASSERT(escort_id != nebula4x::kInvalidId);

    const auto raider_id = find_ship_id(st, "Raider I");
    N4X_ASSERT(raider_id != nebula4x::kInvalidId);

    auto* escort = nebula4x::find_ptr(st.ships, escort_id);
    auto* raider = nebula4x::find_ptr(st.ships, raider_id);
    N4X_ASSERT(escort);
    N4X_ASSERT(raider);

    remove_ship_from_all_system_lists(st, raider_id);
    raider->system_id = sol;
    raider->position_mkm = escort->position_mkm + nebula4x::Vec2{0.2, 0.0};
    st.systems[sol].ships.push_back(raider_id);

    const double hp0 = escort->hp;

    // Two days: launch then impact (fully intercepted).
    sim.advance_days(2);

    N4X_ASSERT(std::abs(escort->hp - hp0) < 1e-9);
  }

return 0;
}
