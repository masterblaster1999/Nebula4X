#include "nebula4x/core/simulation.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace nebula4x {

namespace {

// Clamp x to [0, +inf).
double clamp_nonneg(double x) { return (x < 0.0) ? 0.0 : x; }

} // namespace

void Simulation::tick_ground_combat() {
  // --- Troop training (per-colony) ---
  for (auto& [_, col] : state_.colonies) {
    if (col.troop_training_queue <= 1e-9) continue;

    const double points = std::max(0.0, troop_training_points_per_day(col));
    if (points <= 1e-9) continue;

    double strength = points * std::max(0.0, cfg_.troop_strength_per_training_point);
    strength = std::min(strength, col.troop_training_queue);
    if (strength <= 1e-9) continue;

    // Optional mineral costs.
    // If minerals are missing, scale down to the max affordable.
    double afford = 1.0;
    if (cfg_.troop_training_duranium_per_strength > 1e-9) {
      const double need = strength * cfg_.troop_training_duranium_per_strength;
      const double have = col.minerals.contains("Duranium") ? col.minerals["Duranium"] : 0.0;
      if (need > 1e-9) afford = std::min(afford, have / need);
    }
    if (cfg_.troop_training_neutronium_per_strength > 1e-9) {
      const double need = strength * cfg_.troop_training_neutronium_per_strength;
      const double have = col.minerals.contains("Neutronium") ? col.minerals["Neutronium"] : 0.0;
      if (need > 1e-9) afford = std::min(afford, have / need);
    }
    afford = std::clamp(afford, 0.0, 1.0);
    strength *= afford;
    if (strength <= 1e-9) continue;

    if (cfg_.troop_training_duranium_per_strength > 1e-9) {
      col.minerals["Duranium"] = clamp_nonneg(col.minerals["Duranium"] - strength * cfg_.troop_training_duranium_per_strength);
    }
    if (cfg_.troop_training_neutronium_per_strength > 1e-9) {
      col.minerals["Neutronium"] = clamp_nonneg(col.minerals["Neutronium"] - strength * cfg_.troop_training_neutronium_per_strength);
    }

    col.troop_training_queue = clamp_nonneg(col.troop_training_queue - strength);
    col.ground_forces += strength;
  }

  // --- Battles (deterministic order) ---
  std::vector<Id> battle_keys;
  battle_keys.reserve(state_.ground_battles.size());
  for (const auto& [cid, _] : state_.ground_battles) battle_keys.push_back(cid);
  std::sort(battle_keys.begin(), battle_keys.end());

  const double loss_factor = std::max(0.0, cfg_.ground_combat_loss_factor);
  const double fort_scale = std::max(0.0, cfg_.fortification_defense_scale);

  for (Id cid : battle_keys) {
    auto itb = state_.ground_battles.find(cid);
    if (itb == state_.ground_battles.end()) continue;
    GroundBattle& b = itb->second;

    auto* col = find_ptr(state_.colonies, cid);
    if (!col) {
      state_.ground_battles.erase(itb);
      continue;
    }

    // Keep colony garrison in sync with the battle record.
    b.defender_strength = std::max(0.0, b.defender_strength);
    b.attacker_strength = std::max(0.0, b.attacker_strength);
    col->ground_forces = b.defender_strength;

    const double forts = std::max(0.0, fortification_points(*col));
    const double defense_bonus = 1.0 + forts * fort_scale;

    // Losses proportional to opposing strength.
    double attacker_loss = loss_factor * b.defender_strength;
    double defender_loss = (defense_bonus > 1e-9) ? (loss_factor * b.attacker_strength / defense_bonus)
                                                 : (loss_factor * b.attacker_strength);

    attacker_loss = std::min(attacker_loss, b.attacker_strength);
    defender_loss = std::min(defender_loss, b.defender_strength);

    b.attacker_strength = clamp_nonneg(b.attacker_strength - attacker_loss);
    b.defender_strength = clamp_nonneg(b.defender_strength - defender_loss);
    b.days_fought += 1;

    col->ground_forces = b.defender_strength;

    // Resolution.
    const bool attacker_dead = (b.attacker_strength <= 1e-6);
    const bool defender_dead = (b.defender_strength <= 1e-6);

    if (defender_dead && !attacker_dead) {
      // Colony captured.
      const Id old_owner = col->faction_id;
      col->faction_id = b.attacker_faction_id;
      col->ground_forces = b.attacker_strength;
      col->troop_training_queue = 0.0;
      state_.ground_battles.erase(itb);

      EventContext ctx;
      ctx.faction_id = b.attacker_faction_id;
      ctx.faction_id2 = old_owner;
      ctx.system_id = b.system_id;
      ctx.colony_id = col->id;
      push_event(EventLevel::Warn, EventCategory::Combat,
                 "Colony captured: " + col->name, ctx);
    } else if (attacker_dead) {
      // Defense holds.
      const Id attacker = b.attacker_faction_id;
      state_.ground_battles.erase(itb);

      EventContext ctx;
      ctx.faction_id = col->faction_id;
      ctx.faction_id2 = attacker;
      ctx.system_id = b.system_id;
      ctx.colony_id = col->id;
      push_event(EventLevel::Info, EventCategory::Combat,
                 "Invasion repelled at " + col->name, ctx);
    }
  }
}

} // namespace nebula4x
