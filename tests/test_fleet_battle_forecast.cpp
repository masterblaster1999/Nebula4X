#include <iostream>
#include <string>
#include <vector>

#include "nebula4x/core/fleet_battle_forecast.h"
#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

namespace {

nebula4x::ContentDB minimal_content_for_fleet_forecast() {
  nebula4x::ContentDB content;

  // Simple beam design.
  {
    nebula4x::ShipDesign d;
    d.id = "beam_100";
    d.name = "Beam Frigate";
    d.max_hp = 100.0;
    d.speed_km_s = 10.0;
    d.weapon_damage = 10.0;
    d.weapon_range_mkm = 10.0;
    content.designs[d.id] = d;
  }

  // Weaker beam design.
  {
    nebula4x::ShipDesign d;
    d.id = "beam_100_weak";
    d.name = "Weak Beam Frigate";
    d.max_hp = 100.0;
    d.speed_km_s = 10.0;
    d.weapon_damage = 5.0;
    d.weapon_range_mkm = 10.0;
    content.designs[d.id] = d;
  }

  // Missile design (long range).
  {
    nebula4x::ShipDesign d;
    d.id = "missile_lr";
    d.name = "Long Range Missile Boat";
    d.max_hp = 60.0;
    d.speed_km_s = 20.0;
    d.missile_damage = 40.0;
    d.missile_range_mkm = 100.0;
    d.missile_reload_days = 1.0;
    d.missile_ammo_capacity = 6;
    content.designs[d.id] = d;
  }

  // Point-defense platform.
  {
    nebula4x::ShipDesign d;
    d.id = "pd_platform";
    d.name = "PD Platform";
    d.max_hp = 120.0;
    d.speed_km_s = 5.0;
    // In this simplified model, PD directly cancels incoming missile damage.
    d.point_defense_damage = 200.0;  // per day
    content.designs[d.id] = d;
  }

  // Short range beam defender (slow).
  {
    nebula4x::ShipDesign d;
    d.id = "beam_sr_slow";
    d.name = "Short Range Laser";
    d.max_hp = 90.0;
    d.speed_km_s = 10.0;
    d.weapon_damage = 8.0;
    d.weapon_range_mkm = 10.0;
    content.designs[d.id] = d;
  }

  return content;
}

nebula4x::Ship make_ship(nebula4x::Id id, const std::string& name, const std::string& design_id) {
  nebula4x::Ship s;
  s.id = id;
  s.name = name;
  s.design_id = design_id;
  // In tests, initialize HP/shields explicitly so the forecast doesn't depend on
  // any other simulation tick having run.
  // We will set hp/shields after looking up the design in the sim.
  return s;
}

}  // namespace

int test_fleet_battle_forecast() {
  nebula4x::SimConfig cfg;
  nebula4x::ContentDB content = minimal_content_for_fleet_forecast();
  nebula4x::Simulation sim(std::move(content), cfg);

  // --- Test 1: basic beam-vs-beam (attacker should win) ---
  {
    const auto* dA = sim.find_design("beam_100");
    const auto* dD = sim.find_design("beam_100_weak");
    N4X_ASSERT(dA && dD);

    sim.state().ships.clear();

    nebula4x::Ship a1 = make_ship(1, "A1", dA->id);
    a1.hp = dA->max_hp;
    a1.shields = 0.0;

    nebula4x::Ship a2 = make_ship(2, "A2", dA->id);
    a2.hp = dA->max_hp;
    a2.shields = 0.0;

    nebula4x::Ship d1 = make_ship(3, "D1", dD->id);
    d1.hp = dD->max_hp;
    d1.shields = 0.0;

    sim.state().ships[a1.id] = a1;
    sim.state().ships[a2.id] = a2;
    sim.state().ships[d1.id] = d1;

    nebula4x::FleetBattleForecastOptions opt;
    opt.max_days = 30;
    opt.dt_days = 0.25;
    opt.range_model = nebula4x::FleetBattleRangeModel::Instant;
    opt.damage_model = nebula4x::FleetBattleDamageModel::FocusFire;
    opt.include_missiles = true;
    opt.include_point_defense = true;

    auto r = nebula4x::forecast_fleet_battle(sim, {a1.id, a2.id}, {d1.id}, opt);
    N4X_ASSERT(r.ok);
    N4X_ASSERT(!r.truncated);
    N4X_ASSERT(r.winner == nebula4x::FleetBattleWinner::Attacker);
    N4X_ASSERT(r.defender.end_ships == 0);
    N4X_ASSERT(r.attacker.end_ships >= 1);
  }

  // --- Test 2: point defense should meaningfully reduce missile lethality ---
  {
    const auto* dM = sim.find_design("missile_lr");
    const auto* dP = sim.find_design("pd_platform");
    N4X_ASSERT(dM && dP);

    sim.state().ships.clear();

    nebula4x::Ship m1 = make_ship(10, "M1", dM->id);
    m1.hp = dM->max_hp;
    m1.shields = 0.0;
    m1.missile_ammo = dM->missile_ammo_capacity;

    nebula4x::Ship p1 = make_ship(11, "P1", dP->id);
    p1.hp = dP->max_hp;
    p1.shields = 0.0;

    sim.state().ships[m1.id] = m1;
    sim.state().ships[p1.id] = p1;

    nebula4x::FleetBattleForecastOptions opt;
    opt.max_days = 10;
    opt.dt_days = 0.25;
    opt.range_model = nebula4x::FleetBattleRangeModel::Instant;
    opt.damage_model = nebula4x::FleetBattleDamageModel::FocusFire;
    opt.include_beams = false;
    opt.include_missiles = true;
    opt.include_shields = false;
    opt.include_shield_regen = false;

    // Without PD, missiles should kill the platform.
    {
      auto opt_no_pd = opt;
      opt_no_pd.include_point_defense = false;
      auto r = nebula4x::forecast_fleet_battle(sim, {m1.id}, {p1.id}, opt_no_pd);
      N4X_ASSERT(r.ok);
      N4X_ASSERT(r.winner == nebula4x::FleetBattleWinner::Attacker);
      N4X_ASSERT(r.defender.end_ships == 0);
    }

    // With PD, the defender should survive (battle likely truncates because the PD platform has no offense).
    {
      auto opt_pd = opt;
      opt_pd.include_point_defense = true;
      auto r = nebula4x::forecast_fleet_battle(sim, {m1.id}, {p1.id}, opt_pd);
      N4X_ASSERT(r.ok);
      N4X_ASSERT(r.defender.end_ships == 1);
    }
  }

  // --- Test 3: range advantage model (long-range fast missiles vs slow short-range beams) ---
  {
    const auto* dM = sim.find_design("missile_lr");
    const auto* dS = sim.find_design("beam_sr_slow");
    N4X_ASSERT(dM && dS);

    sim.state().ships.clear();

    nebula4x::Ship m1 = make_ship(20, "M1", dM->id);
    m1.hp = dM->max_hp;
    m1.shields = 0.0;
    m1.missile_ammo = dM->missile_ammo_capacity;

    nebula4x::Ship s1 = make_ship(21, "S1", dS->id);
    s1.hp = dS->max_hp;
    s1.shields = 0.0;

    sim.state().ships[m1.id] = m1;
    sim.state().ships[s1.id] = s1;

    nebula4x::FleetBattleForecastOptions opt;
    opt.max_days = 30;
    opt.dt_days = 0.25;
    opt.range_model = nebula4x::FleetBattleRangeModel::RangeAdvantage;
    opt.damage_model = nebula4x::FleetBattleDamageModel::FocusFire;
    opt.include_beams = true;
    opt.include_missiles = true;
    opt.include_point_defense = true;
    opt.include_shields = false;
    opt.include_shield_regen = false;

    auto r = nebula4x::forecast_fleet_battle(sim, {m1.id}, {s1.id}, opt);
    N4X_ASSERT(r.ok);
    N4X_ASSERT(r.winner == nebula4x::FleetBattleWinner::Attacker);
    N4X_ASSERT(r.defender.end_ships == 0);
  }

  return 0;
}
