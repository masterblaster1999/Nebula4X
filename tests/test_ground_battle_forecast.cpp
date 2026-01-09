#include <cmath>
#include <iostream>

#include "nebula4x/core/ground_battle_forecast.h"
#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(expr) \
  do { \
    if (!(expr)) { \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
      return 1; \
    } \
  } while (0)

int test_ground_battle_forecast() {
  using nebula4x::ContentDB;
  using nebula4x::FactionControl;
  using nebula4x::GroundBattle;
  using nebula4x::GroundBattleForecast;
  using nebula4x::GroundBattleWinner;
  using nebula4x::Id;
  using nebula4x::InstallationDef;
  using nebula4x::SimConfig;
  using nebula4x::Simulation;
  using nebula4x::forecast_ground_battle;
  using nebula4x::square_law_required_attacker_strength;
  using nebula4x::kInvalidId;

  auto make_sim = [] (const SimConfig& cfg) {
    ContentDB content;
    InstallationDef fort;
    fort.id = "fort";
    fort.fortification_points = 10.0;
    content.installations[fort.id] = fort;
    return Simulation(std::move(content), cfg);
  };

  auto pick_target_colony = [](Simulation& sim) -> Id {
    // Prefer Earth if present; otherwise pick the smallest colony id.
    for (const auto& [cid, c] : sim.state().colonies) {
      if (c.name == "Earth") return cid;
    }
    if (sim.state().colonies.empty()) return kInvalidId;
    Id best = sim.state().colonies.begin()->first;
    for (const auto& [cid, _] : sim.state().colonies) best = std::min(best, cid);
    return best;
  };

  auto pick_attacker_faction = [](Simulation& sim, Id defender) -> Id {
    for (const auto& [fid, _] : sim.state().factions) {
      if (fid != defender) return fid;
    }
    return kInvalidId;
  };

  // --- Case 1: both sides hit zero on the same day => defender holds (matches tick ordering) ---
  {
    SimConfig cfg;
    cfg.ground_combat_loss_factor = 1.0;
    cfg.fortification_defense_scale = 0.0;

    Simulation sim = make_sim(cfg);
    for (auto& [_, f] : sim.state().factions) f.control = FactionControl::Player;

    const Id colony_id = pick_target_colony(sim);
    N4X_ASSERT(colony_id != kInvalidId);

    auto& col = sim.state().colonies[colony_id];
    col.troop_training_queue = 0.0;
    col.installations.clear();
    col.ground_forces = 10.0;

    const Id defender_fid = col.faction_id;
    const Id attacker_fid = pick_attacker_faction(sim, defender_fid);
    N4X_ASSERT(attacker_fid != kInvalidId);

    const double forts = sim.fortification_points(col);

    GroundBattle b;
    b.attacker_faction_id = attacker_fid;
    b.system_id = sim.state().bodies[col.body_id].system_id;
    b.attacker_strength = 10.0;
    b.defender_strength = 10.0;
    sim.state().ground_battles[colony_id] = b;

    const GroundBattleForecast fc = forecast_ground_battle(sim.cfg(), b.attacker_strength, b.defender_strength, forts, 0.0);
    N4X_ASSERT(fc.ok);
    N4X_ASSERT(!fc.truncated);
    N4X_ASSERT(fc.days_to_resolve == 1);
    N4X_ASSERT(fc.winner == GroundBattleWinner::Defender);

    int days = 0;
    while (sim.state().ground_battles.find(colony_id) != sim.state().ground_battles.end() && days < 16) {
      sim.advance_days(1);
      ++days;
    }
    N4X_ASSERT(sim.state().ground_battles.find(colony_id) == sim.state().ground_battles.end());
    N4X_ASSERT(days == fc.days_to_resolve);
    N4X_ASSERT(sim.state().colonies[colony_id].faction_id == defender_fid);
  }

  // --- Case 2: fortifications reduce defender losses; forecast matches sim tick-to-resolution ---
  {
    SimConfig cfg;
    cfg.ground_combat_loss_factor = 0.05;
    cfg.fortification_defense_scale = 0.01;

    Simulation sim = make_sim(cfg);
    for (auto& [_, f] : sim.state().factions) f.control = FactionControl::Player;

    const Id colony_id = pick_target_colony(sim);
    N4X_ASSERT(colony_id != kInvalidId);

    auto& col = sim.state().colonies[colony_id];
    col.troop_training_queue = 0.0;
    col.installations.clear();
    col.installations["fort"] = 10; // fort_points = 100

    col.ground_forces = 100.0;

    const Id defender_fid = col.faction_id;
    const Id attacker_fid = pick_attacker_faction(sim, defender_fid);
    N4X_ASSERT(attacker_fid != kInvalidId);

    const double forts = sim.fortification_points(col);
    N4X_ASSERT(std::abs(forts - 100.0) < 1e-9);

    const double bonus = 1.0 + forts * cfg.fortification_defense_scale;
    const double off = 1.0 + forts * cfg.fortification_attack_scale;
    const double expected_req = std::sqrt(bonus * off) * col.ground_forces;
    const double req = square_law_required_attacker_strength(sim.cfg(), col.ground_forces, forts, 0.0);
    N4X_ASSERT(std::abs(req - expected_req) < 1e-6);

    GroundBattle b;
    b.attacker_faction_id = attacker_fid;
    b.system_id = sim.state().bodies[col.body_id].system_id;
    b.attacker_strength = 200.0;
    b.defender_strength = 100.0;
    sim.state().ground_battles[colony_id] = b;

    const GroundBattleForecast fc = forecast_ground_battle(sim.cfg(), b.attacker_strength, b.defender_strength, forts, 0.0);
    N4X_ASSERT(fc.ok);
    N4X_ASSERT(!fc.truncated);
    N4X_ASSERT(fc.days_to_resolve > 0);
    N4X_ASSERT(fc.winner == GroundBattleWinner::Attacker);

    int days = 0;
    while (sim.state().ground_battles.find(colony_id) != sim.state().ground_battles.end() && days < 512) {
      sim.advance_days(1);
      ++days;
    }
    N4X_ASSERT(sim.state().ground_battles.find(colony_id) == sim.state().ground_battles.end());
    N4X_ASSERT(days == fc.days_to_resolve);
    N4X_ASSERT(sim.state().colonies[colony_id].faction_id == attacker_fid);
  }

  return 0;
}
