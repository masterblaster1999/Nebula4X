#include "nebula4x/core/simulation.h"

#include "simulation_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "nebula4x/util/log.h"
#include "nebula4x/util/spatial_index.h"

namespace nebula4x {
namespace {
using sim_internal::sorted_keys;
using sim_internal::compute_power_allocation;
} // namespace

void Simulation::tick_combat() {
  std::unordered_map<Id, double> incoming_damage;
  std::unordered_map<Id, std::vector<Id>> attackers_for_target;

  auto is_hostile = [&](const Ship& a, const Ship& b) { return are_factions_hostile(a.faction_id, b.faction_id); };

  const auto ship_ids = sorted_keys(state_.ships);

  // Build per-system spatial indices lazily. These let us find nearby targets
  // without scanning every ship in the entire simulation.
  std::unordered_map<Id, SpatialIndex2D> system_index;
  system_index.reserve(state_.systems.size());

  auto index_for_system = [&](Id system_id) -> SpatialIndex2D& {
    auto it = system_index.find(system_id);
    if (it != system_index.end()) return it->second;

    SpatialIndex2D idx;
    if (const auto* sys = find_ptr(state_.systems, system_id)) {
      idx.build_from_ship_ids(sys->ships, state_.ships);
    }
    auto [ins, _ok] = system_index.emplace(system_id, std::move(idx));
    return ins->second;
  };

  // Cache detected hostile ships for (faction, system) pairs. Combat can query
  // detection many times (each ship firing), so we compute it once per pair.
  struct DetKey {
    Id faction_id{kInvalidId};
    Id system_id{kInvalidId};
    bool operator==(const DetKey& o) const { return faction_id == o.faction_id && system_id == o.system_id; }
  };
  struct DetKeyHash {
    size_t operator()(const DetKey& k) const {
      return std::hash<long long>()((static_cast<long long>(k.faction_id) << 32) ^ static_cast<long long>(k.system_id));
    }
  };

  std::unordered_map<DetKey, std::vector<Id>, DetKeyHash> detected_hostiles_cache;
  detected_hostiles_cache.reserve(state_.factions.size() * 4);

  auto detected_hostiles_for = [&](Id faction_id, Id system_id) -> const std::vector<Id>& {
    const DetKey key{faction_id, system_id};
    auto it = detected_hostiles_cache.find(key);
    if (it != detected_hostiles_cache.end()) return it->second;

    auto hostiles = detected_hostile_ships_in_system(faction_id, system_id);
    std::sort(hostiles.begin(), hostiles.end());
    hostiles.erase(std::unique(hostiles.begin(), hostiles.end()), hostiles.end());

    auto [ins, _ok] = detected_hostiles_cache.emplace(key, std::move(hostiles));
    return ins->second;
  };

  for (Id aid : ship_ids) {
    const auto* attacker_ptr = find_ptr(state_.ships, aid);
    if (!attacker_ptr) continue;
    const auto& attacker = *attacker_ptr;
    const auto* ad = find_design(attacker.design_id);
    if (!ad) continue;
    if (ad->weapon_damage <= 0.0 || ad->weapon_range_mkm <= 0.0) continue;

    // Power gating: if weapons draw power and the ship can't allocate it,
    // it cannot fire.
    {
      const auto p = compute_power_allocation(*ad);
      if (!p.weapons_online && ad->power_use_weapons > 1e-9) {
        continue;
      }
    }

    Id chosen = kInvalidId;
    double chosen_dist = 1e300;

    const auto& detected_hostiles = detected_hostiles_for(attacker.faction_id, attacker.system_id);

    auto oit = state_.ship_orders.find(aid);
    if (oit != state_.ship_orders.end() && !oit->second.queue.empty() &&
        std::holds_alternative<AttackShip>(oit->second.queue.front())) {
      const Id tid = std::get<AttackShip>(oit->second.queue.front()).target_ship_id;
      const auto* tgt = find_ptr(state_.ships, tid);
      if (tgt && tgt->system_id == attacker.system_id && is_hostile(attacker, *tgt) &&
          std::binary_search(detected_hostiles.begin(), detected_hostiles.end(), tid)) {
        const double dist = (tgt->position_mkm - attacker.position_mkm).length();
        if (dist <= ad->weapon_range_mkm) {
          chosen = tid;
          chosen_dist = dist;
        }
      }
    }

    if (chosen == kInvalidId) {
      if (!detected_hostiles.empty()) {
        auto& idx = index_for_system(attacker.system_id);
        const auto nearby = idx.query_radius(attacker.position_mkm, ad->weapon_range_mkm, 0.0);

        for (Id bid : nearby) {
          if (bid == aid) continue;

          // Only consider targets that are both hostile and detected.
          if (!std::binary_search(detected_hostiles.begin(), detected_hostiles.end(), bid)) continue;

          const auto* target_ptr = find_ptr(state_.ships, bid);
          if (!target_ptr) continue;
          const auto& target = *target_ptr;

          // Defensive checks: should already hold due to how detected_hostiles
          // and the per-system index are built.
          if (target.system_id != attacker.system_id) continue;
          if (!is_hostile(attacker, target)) continue;

          const double dist = (target.position_mkm - attacker.position_mkm).length();
          if (dist > ad->weapon_range_mkm) continue;
          if (dist < chosen_dist) {
            chosen = bid;
            chosen_dist = dist;
          }
        }
      }
    }

    if (chosen != kInvalidId) {
      incoming_damage[chosen] += ad->weapon_damage;
      attackers_for_target[chosen].push_back(aid);
    }
  }

  if (incoming_damage.empty()) return;

  auto fmt1 = [](double x) {
    std::ostringstream ss;
    ss.setf(std::ios::fixed);
    ss << std::setprecision(1) << x;
    return ss.str();
  };

  std::vector<Id> destroyed;
  destroyed.reserve(incoming_damage.size());

  // Track how much damage was absorbed by shields vs applied to hull.
  std::unordered_map<Id, double> shield_damage;
  std::unordered_map<Id, double> hull_damage;
  std::unordered_map<Id, double> pre_hp;
  std::unordered_map<Id, double> pre_shields;
  shield_damage.reserve(incoming_damage.size());
  hull_damage.reserve(incoming_damage.size());
  pre_hp.reserve(incoming_damage.size());
  pre_shields.reserve(incoming_damage.size());

  for (Id tid : sorted_keys(incoming_damage)) {
    const double dmg = incoming_damage[tid];
    auto* tgt = find_ptr(state_.ships, tid);
    if (!tgt) continue;

    pre_hp[tid] = tgt->hp;
    pre_shields[tid] = std::max(0.0, tgt->shields);

    double remaining = dmg;
    double absorbed = 0.0;
    if (tgt->shields > 0.0 && remaining > 0.0) {
      absorbed = std::min(tgt->shields, remaining);
      tgt->shields -= absorbed;
      remaining -= absorbed;
    }
    if (tgt->shields < 0.0) tgt->shields = 0.0;

    shield_damage[tid] = absorbed;
    hull_damage[tid] = remaining;

    tgt->hp -= remaining;
    if (tgt->hp <= 0.0) destroyed.push_back(tid);
  }

  // Damage events for ships that survive.
  // Destruction is logged separately below.
  {
    const double min_abs = std::max(0.0, cfg_.combat_damage_event_min_abs);
    const double min_frac = std::max(0.0, cfg_.combat_damage_event_min_fraction);
    const double warn_frac = std::clamp(cfg_.combat_damage_event_warn_remaining_fraction, 0.0, 1.0);

    for (Id tid : sorted_keys(incoming_damage)) {
      if (incoming_damage[tid] <= 1e-12) continue;

      const auto* tgt = find_ptr(state_.ships, tid);
      if (!tgt) continue;
      if (tgt->hp <= 0.0) continue; // handled by destruction log

      const double sh_dmg = (shield_damage.find(tid) != shield_damage.end()) ? shield_damage.at(tid) : 0.0;
      const double hull_dmg = (hull_damage.find(tid) != hull_damage.end()) ? hull_damage.at(tid) : 0.0;
      if (sh_dmg <= 1e-12 && hull_dmg <= 1e-12) continue;

      const auto* sys = find_ptr(state_.systems, tgt->system_id);
      const std::string sys_name = sys ? sys->name : std::string("(unknown)");

      // Use design max stats when available; otherwise approximate from pre-damage values.
      double max_hp = std::max(1.0, pre_hp.find(tid) != pre_hp.end() ? pre_hp.at(tid) : tgt->hp);
      double max_sh = std::max(0.0, pre_shields.find(tid) != pre_shields.end() ? pre_shields.at(tid) : 0.0);
      if (const auto* d = find_design(tgt->design_id)) {
        if (d->max_hp > 1e-9) max_hp = d->max_hp;
        max_sh = std::max(0.0, d->max_shields);
      }

      // Threshold on either hull damage or (if no hull damage) shield damage.
      double abs_metric = 0.0;
      double frac_metric = 0.0;
      if (hull_dmg > 1e-12) {
        abs_metric = hull_dmg;
        frac_metric = hull_dmg / std::max(1e-9, max_hp);
      } else {
        abs_metric = sh_dmg;
        frac_metric = (max_sh > 1e-9) ? (sh_dmg / std::max(1e-9, max_sh)) : 1.0;
      }
      if (abs_metric + 1e-12 < min_abs && frac_metric + 1e-12 < min_frac) continue;

      // Summarize attackers for context.
      Id attacker_ship_id = kInvalidId;
      Id attacker_fid = kInvalidId;
      std::string attacker_ship_name;
      std::string attacker_fac_name;
      std::size_t attackers_count = 0;

      if (auto ita = attackers_for_target.find(tid); ita != attackers_for_target.end()) {
        auto& vec = ita->second;
        std::sort(vec.begin(), vec.end());
        vec.erase(std::unique(vec.begin(), vec.end()), vec.end());
        attackers_count = vec.size();
        if (!vec.empty()) {
          attacker_ship_id = vec.front();
          if (const auto* atk = find_ptr(state_.ships, attacker_ship_id)) {
            attacker_fid = atk->faction_id;
            attacker_ship_name = atk->name;
            if (const auto* af = find_ptr(state_.factions, attacker_fid)) attacker_fac_name = af->name;
          }
        }
      }

      EventContext ctx;
      ctx.faction_id = tgt->faction_id;
      ctx.faction_id2 = attacker_fid;
      ctx.system_id = tgt->system_id;
      ctx.ship_id = tid;

      std::string msg;
      if (hull_dmg > 1e-12) {
        msg = "Ship damaged: " + tgt->name;
        msg += " took " + fmt1(hull_dmg) + " hull";
        if (sh_dmg > 1e-12) msg += " + " + fmt1(sh_dmg) + " shield";
        msg += " dmg";
      } else {
        msg = "Shields hit: " + tgt->name;
        msg += " took " + fmt1(sh_dmg) + " dmg";
      }

      msg += " (";
      if (max_sh > 1e-9) {
        msg += "Shields " + fmt1(std::max(0.0, tgt->shields)) + "/" + fmt1(max_sh) + ", ";
      }
      msg += "HP " + fmt1(std::max(0.0, tgt->hp)) + "/" + fmt1(max_hp) + ")";
      msg += " in " + sys_name;

      if (attacker_ship_id != kInvalidId) {
        msg += " (attacked by " + (attacker_ship_name.empty() ? (std::string("Ship ") + std::to_string(attacker_ship_id))
                                                          : attacker_ship_name);
        if (!attacker_fac_name.empty()) msg += " / " + attacker_fac_name;
        if (attackers_count > 1) msg += " +" + std::to_string(attackers_count - 1) + " more";
        msg += ")";
      }

      const double hp_frac = std::clamp(tgt->hp / std::max(1e-9, max_hp), 0.0, 1.0);
      double sh_frac = 1.0;
      if (max_sh > 1e-9) {
        sh_frac = std::clamp(tgt->shields / std::max(1e-9, max_sh), 0.0, 1.0);
      }
      const double remaining_frac = std::min(hp_frac, sh_frac);
      const EventLevel lvl = (remaining_frac <= warn_frac) ? EventLevel::Warn : EventLevel::Info;
      push_event(lvl, EventCategory::Combat, msg, ctx);
    }
  }

  std::sort(destroyed.begin(), destroyed.end());

  struct DestructionEvent {
    std::string msg;
    EventContext ctx;
  };
  std::vector<DestructionEvent> death_events;
  death_events.reserve(destroyed.size());

  for (Id dead_id : destroyed) {
    const auto it = state_.ships.find(dead_id);
    if (it == state_.ships.end()) continue;

    const Ship& victim = it->second;
    const Id sys_id = victim.system_id;
    const Id victim_fid = victim.faction_id;

    const auto* sys = find_ptr(state_.systems, sys_id);
    const std::string sys_name = sys ? sys->name : std::string("(unknown)");

    const auto* victim_fac = find_ptr(state_.factions, victim_fid);
    const std::string victim_fac_name = victim_fac ? victim_fac->name : std::string("(unknown)");

    Id attacker_ship_id = kInvalidId;
    Id attacker_fid = kInvalidId;
    std::string attacker_ship_name;
    std::string attacker_fac_name;
    std::size_t attackers_count = 0;

    if (auto ita = attackers_for_target.find(dead_id); ita != attackers_for_target.end()) {
      auto& vec = ita->second;
      std::sort(vec.begin(), vec.end());
      vec.erase(std::unique(vec.begin(), vec.end()), vec.end());
      attackers_count = vec.size();
      if (!vec.empty()) {
        attacker_ship_id = vec.front();
        if (const auto* atk = find_ptr(state_.ships, attacker_ship_id)) {
          attacker_fid = atk->faction_id;
          attacker_ship_name = atk->name;
          if (const auto* af = find_ptr(state_.factions, attacker_fid)) attacker_fac_name = af->name;
        }
      }
    }

    EventContext ctx;
    ctx.faction_id = victim_fid;
    ctx.faction_id2 = attacker_fid;
    ctx.system_id = sys_id;
    ctx.ship_id = dead_id;

    std::string msg = "Ship destroyed: " + victim.name;
    msg += " (" + victim_fac_name + ")";
    msg += " in " + sys_name;

    if (attacker_ship_id != kInvalidId) {
      msg += " (killed by " + (attacker_ship_name.empty() ? std::string("Ship ") + std::to_string(attacker_ship_id)
                                                         : attacker_ship_name);
      if (!attacker_fac_name.empty()) msg += " / " + attacker_fac_name;
      if (attackers_count > 1) msg += " +" + std::to_string(attackers_count - 1) + " more";
      msg += ")";
    }

    death_events.push_back(DestructionEvent{std::move(msg), ctx});
  }

  for (Id dead_id : destroyed) {
    auto it = state_.ships.find(dead_id);
    if (it == state_.ships.end()) continue;

    const Id sys_id = it->second.system_id;

    if (auto* sys = find_ptr(state_.systems, sys_id)) {
      sys->ships.erase(std::remove(sys->ships.begin(), sys->ships.end(), dead_id), sys->ships.end());
    }

    state_.ship_orders.erase(dead_id);
    state_.ships.erase(dead_id);

    // Keep fleet membership consistent.
    remove_ship_from_fleets(dead_id);

    for (auto& [_, fac] : state_.factions) {
      fac.ship_contacts.erase(dead_id);
    }
  }

  for (const auto& e : death_events) {
    nebula4x::log::warn(e.msg);
    push_event(EventLevel::Warn, EventCategory::Combat, e.msg, e.ctx);
  }
}


} // namespace nebula4x
