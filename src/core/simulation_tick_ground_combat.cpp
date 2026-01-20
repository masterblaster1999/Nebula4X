#include "nebula4x/core/simulation.h"

#include "nebula4x/util/trace_events.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace nebula4x {

namespace {

// Clamp x to [0, +inf).
double clamp_nonneg(double x) { return (x < 0.0) ? 0.0 : x; }

// Deterministic casualty intensity decay ("fatigue") multiplier used for ground battles.
//
// The multiplier is applied equally to attacker/defender losses so it does not
// change who wins under the square-law model; it only stretches/compresses time.
double ground_combat_fatigue_multiplier(const SimConfig& cfg, int days_fought) {
  const double k = std::max(0.0, cfg.ground_combat_fatigue_per_day);
  if (k <= 1e-12) return 1.0;
  const double min_mult = std::clamp(cfg.ground_combat_fatigue_min_multiplier, 0.0, 1.0);
  const int d = std::max(0, days_fought);
  double mult = 1.0 / (1.0 + k * static_cast<double>(d));
  if (!std::isfinite(mult)) mult = 1.0;
  return std::clamp(mult, min_mult, 1.0);
}

static bool is_player_faction(const GameState& s, Id faction_id) {
  auto it = s.factions.find(faction_id);
  if (it == s.factions.end()) return false;
  return it->second.control == FactionControl::Player;
}

} // namespace

void Simulation::tick_ground_combat() {
  NEBULA4X_TRACE_SCOPE("tick_ground_combat", "sim.ground");

  // --- Sync active ground battles into colonies before any training/automation ---
  // GroundBattle stores the authoritative defender strength while a battle is active.
  // This avoids a long-standing edge case where troop training would add garrison
  // strength to Colony::ground_forces, only for the battle loop to immediately
  // overwrite it from GroundBattle::defender_strength.
  for (auto& [cid, b] : state_.ground_battles) {
    b.defender_strength = std::max(0.0, b.defender_strength);
    b.attacker_strength = std::max(0.0, b.attacker_strength);
    b.fortification_damage_points = std::max(0.0, b.fortification_damage_points);
    if (auto* col = find_ptr(state_.colonies, cid)) {
      col->ground_forces = b.defender_strength;
    }
  }

  // --- Troop training (per-colony) ---
  for (auto& [_, col] : state_.colonies) {
    // Gameplay QoL: colony garrison target automation.
    //
    // If the player sets garrison_target_strength, the simulation keeps enough
    // *auto-queued* training in the queue to reach that target.
    //
    // We track auto-queued strength separately so that reducing the target can
    // prune only the auto-generated portion without deleting manual training.
    col.ground_forces = clamp_nonneg(col.ground_forces);
    col.troop_training_queue = clamp_nonneg(col.troop_training_queue);
    col.troop_training_auto_queued = std::clamp(col.troop_training_auto_queued, 0.0, col.troop_training_queue);
    col.garrison_target_strength = clamp_nonneg(col.garrison_target_strength);

    if (col.garrison_target_strength > 1e-9) {
      const double desired = col.garrison_target_strength;
      const double manual_q = clamp_nonneg(col.troop_training_queue - col.troop_training_auto_queued);

      // Total queue needed to reach the target (ignoring ongoing battles).
      const double required_queue_total = clamp_nonneg(desired - col.ground_forces);

      // Auto portion required after accounting for manual queue already present.
      const double required_auto = clamp_nonneg(required_queue_total - manual_q);
      const double current_auto = col.troop_training_auto_queued;

      if (required_auto > current_auto + 1e-9) {
        const double add = required_auto - current_auto;
        col.troop_training_auto_queued = required_auto;
        col.troop_training_queue = clamp_nonneg(col.troop_training_queue + add);
      } else if (required_auto + 1e-9 < current_auto) {
        const double remove = current_auto - required_auto;
        col.troop_training_auto_queued = required_auto;
        col.troop_training_queue = clamp_nonneg(col.troop_training_queue - remove);
      }

      col.troop_training_auto_queued = std::clamp(col.troop_training_auto_queued, 0.0, col.troop_training_queue);
    }

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

    // Treat manual training as being "ahead" of the auto-queued tail. This keeps
    // the manual portion stable unless the total queue drops below it.
    col.troop_training_auto_queued = std::min(col.troop_training_auto_queued, col.troop_training_queue);

    col.ground_forces += strength;

    // If this colony is in an active ground battle, training reinforces the defender.
    if (auto itb = state_.ground_battles.find(col.id); itb != state_.ground_battles.end()) {
      itb->second.defender_strength = col.ground_forces;
    }
  }

  // --- Battles (deterministic order) ---
  std::vector<Id> battle_keys;
  battle_keys.reserve(state_.ground_battles.size());
  for (const auto& [cid, _] : state_.ground_battles) battle_keys.push_back(cid);
  std::sort(battle_keys.begin(), battle_keys.end());

  const double base_loss_factor = std::max(0.0, cfg_.ground_combat_loss_factor);
  const double fort_def_scale = std::max(0.0, cfg_.fortification_defense_scale);
  const double fort_atk_scale = std::max(0.0, cfg_.fortification_attack_scale);
  const double arty_strength_per_weapon =
      std::max(0.0, cfg_.ground_combat_defender_artillery_strength_per_weapon_damage);
  const double base_fort_damage_rate =
      std::max(0.0, cfg_.ground_combat_fortification_damage_per_attacker_strength_day);

  const double inst_dmg_per_loss = std::max(0.0, cfg_.ground_combat_installation_damage_per_strength_lost);
  const double pop_millions_per_loss = std::max(0.0, cfg_.ground_combat_population_millions_per_strength_lost);
  const int max_collateral_inst = cfg_.ground_combat_collateral_max_installations_destroyed_per_day;
  const double inst_hp_per_cost = std::max(0.0, cfg_.bombard_installation_hp_per_construction_cost);

  auto fmt1 = [](double x) {
    std::ostringstream ss;
    ss.setf(std::ios::fixed);
    ss << std::setprecision(1) << x;
    return ss.str();
  };

  // Helper: total colony weapon damage/day from installations.
  // (Used as a proxy for defender artillery / prepared fire support.)
  auto colony_weapon_damage_per_day = [&](const Colony& c) -> double {
    double dmg = 0.0;
    for (const auto& [inst_id, count] : c.installations) {
      if (count <= 0) continue;
      const auto it = content_.installations.find(inst_id);
      if (it == content_.installations.end()) continue;
      const double wd = it->second.weapon_damage;
      if (wd <= 0.0) continue;
      dmg += wd * static_cast<double>(count);
    }
    return std::max(0.0, dmg);
  };

  // Apply accumulated fortification damage by destroying fortification installations.
  //
  // Returns (installations_destroyed, fort_points_destroyed).
  auto apply_fortification_damage_to_colony = [&](Colony& c, double damage_points) -> std::pair<int, double> {
    double dmg = clamp_nonneg(damage_points);
    if (dmg <= 1e-9) return {0, 0.0};

    std::vector<std::string> forts;
    forts.reserve(c.installations.size());
    for (const auto& [inst_id, count] : c.installations) {
      if (count <= 0) continue;
      const auto it = content_.installations.find(inst_id);
      if (it == content_.installations.end()) continue;
      if (it->second.fortification_points <= 0.0) continue;
      forts.push_back(inst_id);
    }
    if (forts.empty()) return {0, 0.0};
    std::sort(forts.begin(), forts.end());
    forts.erase(std::unique(forts.begin(), forts.end()), forts.end());

    int destroyed_inst = 0;
    double destroyed_points = 0.0;

    for (const auto& inst_id : forts) {
      if (dmg <= 1e-9) break;
      auto itc = c.installations.find(inst_id);
      if (itc == c.installations.end() || itc->second <= 0) continue;
      const int count = itc->second;
      const auto itdef = content_.installations.find(inst_id);
      if (itdef == content_.installations.end()) continue;
      const double per = std::max(0.0, itdef->second.fortification_points);
      if (per <= 1e-9) continue;

      const double max_points = per * static_cast<double>(count);
      const double spend = std::min(dmg, max_points);

      int destroy = static_cast<int>(std::floor(spend / per + 1e-9));
      const double rem = spend - static_cast<double>(destroy) * per;
      // Deterministic rounding: if we have at least half an installation's worth
      // of damage, destroy one additional installation (if any remain).
      if (destroy < count && rem >= per * 0.5) destroy += 1;
      destroy = std::clamp(destroy, 0, count);
      if (destroy <= 0) continue;

      itc->second = count - destroy;
      if (itc->second <= 0) c.installations.erase(itc);

      destroyed_inst += destroy;
      destroyed_points += per * static_cast<double>(destroy);
      dmg = clamp_nonneg(dmg - per * static_cast<double>(destroy));
    }

    return {destroyed_inst, destroyed_points};
  };

  // Apply collateral damage to *non-fortification* installations on a colony.
  //
  // damage_points is in the same abstract units as orbital bombardment damage and
  // is converted into installation kills using the same cost-derived HP model
  // (bombard_installation_hp_per_construction_cost).
  //
  // Returns a list of (installation_id, destroyed_count).
  auto apply_collateral_installation_damage_to_colony =
      [&](Colony& c, double damage_points, int max_destroy) -> std::vector<std::pair<std::string, int>> {
    std::vector<std::pair<std::string, int>> destroyed;
    damage_points = clamp_nonneg(damage_points);
    if (damage_points <= 1e-9) return destroyed;
    if (inst_hp_per_cost <= 1e-12) return destroyed;

    int remaining_cap = max_destroy;
    if (remaining_cap < 0) remaining_cap = std::numeric_limits<int>::max();
    if (remaining_cap == 0) return destroyed;

    struct Cand {
      std::string id;
      int count{0};
      int pri{4};
      double hp{1.0};
    };
    std::vector<Cand> cands;
    cands.reserve(c.installations.size());

    for (const auto& [inst_id, count] : c.installations) {
      if (count <= 0) continue;
      auto it = content_.installations.find(inst_id);
      if (it == content_.installations.end()) continue;
      const auto& def = it->second;
      // Fortifications are handled separately by fortification_damage_points.
      if (def.fortification_points > 0.0) continue;

      Cand cnd;
      cnd.id = inst_id;
      cnd.count = count;
      cnd.pri = 4;
      if (def.weapon_damage > 0.0 && def.weapon_range_mkm > 0.0) {
        cnd.pri = 0;
      } else if (inst_id == "shipyard") {
        cnd.pri = 1;
      } else if (inst_id == "construction_factory" || inst_id == "munitions_factory" ||
                 inst_id == "fuel_refinery" || inst_id == "automated_mine" ||
                 inst_id == "metal_smelter" || inst_id == "mineral_processor") {
        cnd.pri = 2;
      } else if (def.research_points_per_day > 0.0 || def.sensor_range_mkm > 0.0 ||
                 def.troop_training_points_per_day > 0.0 || def.crew_training_points_per_day > 0.0) {
        cnd.pri = 3;
      }

      cnd.hp = std::max(1.0, static_cast<double>(def.construction_cost) * inst_hp_per_cost);
      cands.push_back(std::move(cnd));
    }

    if (cands.empty()) return destroyed;

    std::sort(cands.begin(), cands.end(), [&](const Cand& a, const Cand& b) {
      if (a.pri != b.pri) return a.pri < b.pri;
      return a.id < b.id;
    });

    for (auto& cand : cands) {
      if (damage_points <= 1e-9) break;
      if (remaining_cap <= 0) break;
      if (cand.count <= 0) continue;
      if (cand.hp <= 1e-12) cand.hp = 1.0;

      int kill = static_cast<int>(std::floor((damage_points + 1e-9) / cand.hp));
      // Deterministic half-rounding so small damage can still have an effect.
      const double rem = damage_points - static_cast<double>(kill) * cand.hp;
      if (rem >= 0.5 * cand.hp) kill += 1;

      kill = std::min(kill, cand.count);
      kill = std::min(kill, remaining_cap);
      if (kill <= 0) continue;

      auto itc = c.installations.find(cand.id);
      if (itc != c.installations.end()) {
        itc->second -= kill;
        if (itc->second <= 0) c.installations.erase(itc);
      }

      destroyed.push_back({cand.id, kill});
      remaining_cap -= kill;
      damage_points -= static_cast<double>(kill) * cand.hp;
      damage_points = std::max(0.0, damage_points);
    }

    return destroyed;
  };

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
    // (Battle record is authoritative during the battle, but troop training above
    // may have reinforced it already.)
    b.defender_strength = std::max(0.0, b.defender_strength);
    b.attacker_strength = std::max(0.0, b.attacker_strength);
    b.fortification_damage_points = clamp_nonneg(b.fortification_damage_points);
    col->ground_forces = b.defender_strength;

    // Fortifications.
    const double total_forts = std::max(0.0, fortification_points(*col));
    if (total_forts <= 1e-9) b.fortification_damage_points = 0.0;
    if (b.fortification_damage_points > total_forts) b.fortification_damage_points = total_forts;

    const double eff_forts = std::max(0.0, total_forts - b.fortification_damage_points);
    const double defense_bonus = 1.0 + eff_forts * fort_def_scale;
    const double offense_bonus = 1.0 + eff_forts * fort_atk_scale;

    // Defender artillery (installation weapon platforms). We scale it down as
    // fortifications are degraded during the battle.
    double arty_weapon = colony_weapon_damage_per_day(*col);
    double fort_integrity = 1.0;
    if (total_forts > 1e-9) {
      fort_integrity = std::clamp(eff_forts / total_forts, 0.0, 1.0);
    }
    arty_weapon *= fort_integrity;
    const double arty_loss = arty_weapon * arty_strength_per_weapon;

    // Intensity decay (fatigue) scales daily losses and fortification damage.
    const double fatigue_mult = ground_combat_fatigue_multiplier(cfg_, b.days_fought);
    const double loss_factor = base_loss_factor * fatigue_mult;
    const double fort_damage_rate = base_fort_damage_rate * fatigue_mult;

    // Losses proportional to opposing strength, modified by fortifications and
    // defender artillery, then scaled by fatigue.
    double attacker_loss = loss_factor * b.defender_strength * offense_bonus + (arty_loss * fatigue_mult);
    double defender_loss = (defense_bonus > 1e-9)
                               ? (loss_factor * b.attacker_strength / defense_bonus)
                               : (loss_factor * b.attacker_strength);

    attacker_loss = std::min(attacker_loss, b.attacker_strength);
    defender_loss = std::min(defender_loss, b.defender_strength);

    const double casualties_today = attacker_loss + defender_loss;

    b.attacker_strength = clamp_nonneg(b.attacker_strength - attacker_loss);
    b.defender_strength = clamp_nonneg(b.defender_strength - defender_loss);
    b.days_fought += 1;

    // Fortification degradation happens alongside combat.
    if (fort_damage_rate > 1e-9 && total_forts > 1e-9 && b.attacker_strength > 1e-9) {
      b.fortification_damage_points += b.attacker_strength * fort_damage_rate;
      if (b.fortification_damage_points > total_forts) b.fortification_damage_points = total_forts;
    }

    col->ground_forces = b.defender_strength;

    // Collateral damage (optional): battle casualties can destroy installations
    // and kill civilians.
    std::vector<std::pair<std::string, int>> destroyed_inst;
    double pop_loss_m = 0.0;

    if (casualties_today > 1e-9) {
      if (inst_dmg_per_loss > 1e-12 && inst_hp_per_cost > 1e-12 && !col->installations.empty()) {
        destroyed_inst = apply_collateral_installation_damage_to_colony(
            *col, casualties_today * inst_dmg_per_loss, max_collateral_inst);
      }

      if (pop_millions_per_loss > 1e-12 && col->population_millions > 1e-12) {
        pop_loss_m = std::min(col->population_millions, casualties_today * pop_millions_per_loss);
        col->population_millions = std::max(0.0, col->population_millions - pop_loss_m);
      }
    }

    int destroyed_total = 0;
    for (const auto& p : destroyed_inst) destroyed_total += p.second;

    const bool did_collateral = (destroyed_total > 0) || (pop_loss_m > 1e-9);
    if (did_collateral && (is_player_faction(state_, b.attacker_faction_id) || is_player_faction(state_, col->faction_id))) {
      EventLevel lvl = EventLevel::Info;
      if (destroyed_total >= 5 || pop_loss_m >= 1.0) lvl = EventLevel::Warn;

      EventContext ctx;
      ctx.faction_id = b.attacker_faction_id;
      ctx.faction_id2 = col->faction_id;
      ctx.system_id = b.system_id;
      ctx.colony_id = col->id;

      std::string msg = "Ground combat devastation at " + col->name + " (day " + std::to_string(b.days_fought) + "): ";
      bool first = true;
      if (destroyed_total > 0) {
        msg += "destroyed " + std::to_string(destroyed_total) + " installations";
        first = false;
      }
      if (pop_loss_m > 1e-9) {
        if (!first) msg += ", ";
        msg += "civilian casualties " + fmt1(pop_loss_m) + "M";
        first = false;
      }
      msg += ".";

      push_event(lvl, EventCategory::Combat, msg, ctx);
    }

    // Resolution.
    const bool attacker_dead = (b.attacker_strength <= 1e-6);
    const bool defender_dead = (b.defender_strength <= 1e-6);

    if (defender_dead && !attacker_dead) {
      // Apply fortification damage before transferring ownership.
      auto [fort_destroyed, _pts] = apply_fortification_damage_to_colony(*col, b.fortification_damage_points);

      // Colony captured.
      const Id old_owner = col->faction_id;
      col->faction_id = b.attacker_faction_id;
      col->ground_forces = b.attacker_strength;
      col->troop_training_queue = 0.0;
      col->troop_training_auto_queued = 0.0;
      col->garrison_target_strength = 0.0;
      state_.ground_battles.erase(itb);

      EventContext ctx;
      ctx.faction_id = b.attacker_faction_id;
      ctx.faction_id2 = old_owner;
      ctx.system_id = b.system_id;
      ctx.colony_id = col->id;
      std::string msg = "Colony captured: " + col->name;
      if (fort_destroyed > 0) {
        msg += " (fortifications destroyed: " + std::to_string(fort_destroyed) + ")";
      }
      push_event(EventLevel::Warn, EventCategory::Combat, msg, ctx);
    } else if (attacker_dead) {
      // Apply fortification damage (the attacker may have done lasting damage
      // even if the invasion ultimately failed).
      auto [fort_destroyed, _pts] = apply_fortification_damage_to_colony(*col, b.fortification_damage_points);

      // Defense holds.
      const Id attacker = b.attacker_faction_id;
      state_.ground_battles.erase(itb);

      EventContext ctx;
      ctx.faction_id = col->faction_id;
      ctx.faction_id2 = attacker;
      ctx.system_id = b.system_id;
      ctx.colony_id = col->id;
      std::string msg = "Invasion repelled at " + col->name;
      if (fort_destroyed > 0) {
        msg += " (fortifications destroyed: " + std::to_string(fort_destroyed) + ")";
      }
      push_event(EventLevel::Info, EventCategory::Combat, msg, ctx);
    }
  }
}

} // namespace nebula4x
