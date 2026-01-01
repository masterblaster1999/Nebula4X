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

// Deterministic pseudo-random generator for combat sub-systems.
// This keeps simulation deterministic across runs while still allowing
// probabilistic mechanics (e.g. boarding).
static uint64_t splitmix64(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

static double u01_from_u64(uint64_t x) {
  // Use the top 53 bits to build a double in [0,1).
  const uint64_t v = x >> 11;
  return static_cast<double>(v) * (1.0 / 9007199254740992.0); // 2^53
}

static double clamp01(double x) {
  if (x < 0.0) return 0.0;
  if (x > 1.0) return 1.0;
  return x;
}

} // namespace

void Simulation::tick_combat() {
  std::unordered_map<Id, double> incoming_damage;
  std::unordered_map<Id, std::vector<Id>> attackers_for_target;
  std::unordered_map<Id, std::vector<Id>> colony_attackers_for_target;

  const bool do_boarding = cfg_.enable_boarding && cfg_.boarding_range_mkm > 1e-9;

  auto is_hostile = [&](const Ship& a, const Ship& b) { return are_factions_hostile(a.faction_id, b.faction_id); };

  const auto ship_ids = sorted_keys(state_.ships);

  // Build per-system spatial indices lazily. These let us find nearby targets
  // without scanning every ship in the entire simulation.
  std::unordered_map<Id, SpatialIndex2D> system_index;
  system_index.reserve(state_.systems.size());

  // Precompute colony weapon platforms (planetary defenses).
  //
  // We treat each colony as having at most one aggregated "battery": all
  // installations with weapon stats contribute damage, and range is the
  // maximum range across those installations.
  struct ColonyBattery {
    Id colony_id{kInvalidId};
    Id faction_id{kInvalidId};
    Id system_id{kInvalidId};
    Vec2 position_mkm{0.0, 0.0};
    double weapon_damage{0.0};
    double weapon_range_mkm{0.0};
  };

  std::vector<ColonyBattery> colony_batteries;
  colony_batteries.reserve(state_.colonies.size());

  for (const auto& [cid, col] : state_.colonies) {
    const auto* body = find_ptr(state_.bodies, col.body_id);
    if (!body) continue;

    double dmg = 0.0;
    double range = 0.0;
    for (const auto& [inst_id, count] : col.installations) {
      if (count <= 0) continue;
      const auto it = content_.installations.find(inst_id);
      if (it == content_.installations.end()) continue;
      const auto& def = it->second;
      if (def.weapon_damage <= 0.0 || def.weapon_range_mkm <= 0.0) continue;
      dmg += def.weapon_damage * static_cast<double>(count);
      range = std::max(range, def.weapon_range_mkm);
    }

    if (dmg > 1e-9 && range > 1e-9) {
      ColonyBattery b;
      b.colony_id = cid;
      b.faction_id = col.faction_id;
      b.system_id = body->system_id;
      b.position_mkm = body->position_mkm;
      b.weapon_damage = dmg;
      b.weapon_range_mkm = range;
      colony_batteries.push_back(b);
    }
  }

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
  // detection many times (each ship firing, each boarding attempt), so we compute
  // it once per pair.
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

  auto ship_max_hp = [&](const Ship& s) -> double {
    if (const auto* d = find_design(s.design_id)) {
      if (d->max_hp > 1e-9) return d->max_hp;
    }
    return std::max(1.0, s.hp);
  };

  auto is_target_boardable = [&](const Ship& attacker, const Ship& target) -> bool {
    if (!do_boarding) return false;
    if (attacker.troops + 1e-9 < cfg_.boarding_min_attacker_troops) return false;
    if (!is_hostile(attacker, target)) return false;

    const double hp_frac = clamp01(target.hp / std::max(1e-9, ship_max_hp(target)));
    if (hp_frac > clamp01(cfg_.boarding_target_hp_fraction) + 1e-12) return false;

    if (cfg_.boarding_require_shields_down && target.shields > 1e-9) return false;
    return true;
  };

  auto attack_order_target = [&](Id attacker_id) -> Id {
    auto oit = state_.ship_orders.find(attacker_id);
    if (oit == state_.ship_orders.end()) return kInvalidId;
    if (oit->second.queue.empty()) return kInvalidId;
    if (!std::holds_alternative<AttackShip>(oit->second.queue.front())) return kInvalidId;
    return std::get<AttackShip>(oit->second.queue.front()).target_ship_id;
  };

  auto bombard_order_ptr = [&](Id attacker_id) -> BombardColony* {
    auto oit = state_.ship_orders.find(attacker_id);
    if (oit == state_.ship_orders.end()) return nullptr;
    if (oit->second.queue.empty()) return nullptr;
    if (!std::holds_alternative<BombardColony>(oit->second.queue.front())) return nullptr;
    return &std::get<BombardColony>(oit->second.queue.front());
  };

  auto fmt1 = [](double x) {
    std::ostringstream ss;
    ss.setf(std::ios::fixed);
    ss << std::setprecision(1) << x;
    return ss.str();
  };

  // --- weapon fire ---
  for (Id aid : ship_ids) {
    const auto* attacker_ptr = find_ptr(state_.ships, aid);
    if (!attacker_ptr) continue;
    const auto& attacker = *attacker_ptr;

    const auto* ad = find_design(attacker.design_id);
    if (!ad) continue;
    if (ad->weapon_damage <= 0.0 || ad->weapon_range_mkm <= 0.0) continue;

    // Power gating: if weapons are offline (due to power deficit or the
    // ship's power policy), it cannot fire.
    {
      const auto p = compute_power_allocation(*ad, attacker.power_policy);
      if (!p.weapons_online) continue;
    }

    // --- Orbital bombardment ---
    // If the current order is BombardColony and the target is in range, use
    // this ship's daily weapon fire to damage the colony.
    {
      BombardColony* bo = bombard_order_ptr(aid);
      if (bo) {
        // Sanity: duration 0 means "complete immediately".
        if (bo->duration_days == 0) {
          auto itq = state_.ship_orders.find(aid);
          if (itq != state_.ship_orders.end() && !itq->second.queue.empty() &&
              std::holds_alternative<BombardColony>(itq->second.queue.front())) {
            itq->second.queue.erase(itq->second.queue.begin());
          }
        } else {
          Colony* col = find_ptr(state_.colonies, bo->colony_id);
          if (!col || col->faction_id == attacker.faction_id) {
            // Target vanished or changed hands.
            auto itq = state_.ship_orders.find(aid);
            if (itq != state_.ship_orders.end() && !itq->second.queue.empty() &&
                std::holds_alternative<BombardColony>(itq->second.queue.front())) {
              itq->second.queue.erase(itq->second.queue.begin());
            }
          } else {
            const Body* body = find_ptr(state_.bodies, col->body_id);
            if (body && body->system_id == attacker.system_id) {
              const double dist = (body->position_mkm - attacker.position_mkm).length();
              if (dist <= ad->weapon_range_mkm + 1e-9) {
                // Apply damage in the order: ground forces -> installations -> population.
                double remaining = std::max(0.0, ad->weapon_damage);
                double killed_ground = 0.0;
                double pop_loss_m = 0.0;
                std::vector<std::pair<std::string, int>> destroyed;

                const double gf_per_dmg = std::max(0.0, cfg_.bombard_ground_strength_per_damage);
                if (remaining > 1e-12 && gf_per_dmg > 1e-12 && col->ground_forces > 1e-12) {
                  const double possible = remaining * gf_per_dmg;
                  killed_ground = std::min(col->ground_forces, possible);
                  col->ground_forces = std::max(0.0, col->ground_forces - killed_ground);
                  remaining -= killed_ground / gf_per_dmg;
                  remaining = std::max(0.0, remaining);

                  // Keep any ongoing ground battle in sync.
                  auto itb = state_.ground_battles.find(col->id);
                  if (itb != state_.ground_battles.end()) {
                    itb->second.defender_strength = std::max(0.0, itb->second.defender_strength - killed_ground);
                  }
                }

                const double hp_per_cost = std::max(0.0, cfg_.bombard_installation_hp_per_construction_cost);
                if (remaining > 1e-12 && !col->installations.empty()) {
                  struct Cand {
                    std::string id;
                    int count{0};
                    int pri{3};
                    double hp{1.0};
                  };
                  std::vector<Cand> cands;
                  cands.reserve(col->installations.size());

                  for (const auto& [inst_id, count] : col->installations) {
                    if (count <= 0) continue;
                    Cand c;
                    c.id = inst_id;
                    c.count = count;
                    c.pri = 3;
                    c.hp = 1.0;
                    if (auto it = content_.installations.find(inst_id); it != content_.installations.end()) {
                      const auto& def = it->second;
                      if (def.weapon_damage > 0.0 && def.weapon_range_mkm > 0.0) {
                        c.pri = 0;
                      } else if (def.fortification_points > 0.0) {
                        c.pri = 1;
                      } else if (def.sensor_range_mkm > 0.0) {
                        c.pri = 2;
                      }
                      c.hp = std::max(1.0, static_cast<double>(def.construction_cost) * hp_per_cost);
                    }
                    cands.push_back(std::move(c));
                  }

                  std::sort(cands.begin(), cands.end(), [&](const Cand& a, const Cand& b) {
                    if (a.pri != b.pri) return a.pri < b.pri;
                    return a.id < b.id;
                  });

                  for (auto& c : cands) {
                    if (remaining <= 1e-12) break;
                    if (c.count <= 0) continue;
                    if (c.hp <= 1e-12) c.hp = 1.0;

                    const int can_kill = static_cast<int>(std::floor((remaining + 1e-9) / c.hp));
                    const int kill = std::min(c.count, std::max(0, can_kill));
                    if (kill <= 0) continue;

                    auto itc = col->installations.find(c.id);
                    if (itc != col->installations.end()) {
                      itc->second -= kill;
                      if (itc->second <= 0) col->installations.erase(itc);
                    }
                    remaining -= static_cast<double>(kill) * c.hp;
                    remaining = std::max(0.0, remaining);
                    destroyed.push_back({c.id, kill});
                  }
                }

                const double pop_per_dmg = std::max(0.0, cfg_.bombard_population_millions_per_damage);
                if (remaining > 1e-12 && pop_per_dmg > 1e-12 && col->population_millions > 1e-12) {
                  pop_loss_m = std::min(col->population_millions, remaining * pop_per_dmg);
                  col->population_millions = std::max(0.0, col->population_millions - pop_loss_m);
                  remaining = 0.0;
                }

                const bool did_effect = (killed_ground > 1e-12) || (!destroyed.empty()) || (pop_loss_m > 1e-12);
                if (did_effect) {
                  const auto* sys = find_ptr(state_.systems, attacker.system_id);
                  const std::string sys_name = sys ? sys->name : std::string("(unknown)");

                  int destroyed_total = 0;
                  for (const auto& p : destroyed) destroyed_total += p.second;

                  std::string msg = "Bombardment: Ship " + attacker.name + " bombarded " + col->name;
                  msg += " in " + sys_name + " (";
                  bool first = true;
                  if (killed_ground > 1e-12) {
                    msg += "killed " + fmt1(killed_ground) + " ground";
                    first = false;
                  }
                  if (destroyed_total > 0) {
                    if (!first) msg += ", ";
                    msg += "destroyed " + std::to_string(destroyed_total) + " installations";
                    first = false;
                  }
                  if (pop_loss_m > 1e-12) {
                    if (!first) msg += ", ";
                    msg += "casualties " + fmt1(pop_loss_m) + "M";
                    first = false;
                  }
                  msg += ")";

                  EventContext ctx;
                  ctx.faction_id = attacker.faction_id;
                  ctx.faction_id2 = col->faction_id;
                  ctx.system_id = attacker.system_id;
                  ctx.ship_id = aid;
                  ctx.colony_id = col->id;
                  push_event(EventLevel::Info, EventCategory::Combat, msg, ctx);

                  // Also notify the defender.
                  EventContext ctx2 = ctx;
                  ctx2.faction_id = col->faction_id;
                  ctx2.faction_id2 = attacker.faction_id;
                  push_event(EventLevel::Info, EventCategory::Combat, msg, ctx2);
                }

                // Tick down duration only when we actually fired.
                if (bo->duration_days > 0) {
                  bo->duration_days -= 1;
                  if (bo->duration_days == 0) {
                    auto itq = state_.ship_orders.find(aid);
                    if (itq != state_.ship_orders.end() && !itq->second.queue.empty() &&
                        std::holds_alternative<BombardColony>(itq->second.queue.front())) {
                      itq->second.queue.erase(itq->second.queue.begin());
                    }
                  }
                }

                // This ship spent its weapon fire on bombardment.
                continue;
              }
            }
          }
        }
      }
    }

    Id chosen = kInvalidId;
    double chosen_dist = 1e300;

    const auto& detected_hostiles = detected_hostiles_for(attacker.faction_id, attacker.system_id);

    // If the ship has an explicit AttackShip order, prefer its target.
    // Additionally, if the target is already disabled and we have troops,
    // withhold fire to avoid accidentally destroying a ship we intend to board.
    {
      const Id tid = attack_order_target(aid);
      if (tid != kInvalidId) {
        const auto* tgt = find_ptr(state_.ships, tid);
        if (tgt && tgt->system_id == attacker.system_id && is_hostile(attacker, *tgt) &&
            std::binary_search(detected_hostiles.begin(), detected_hostiles.end(), tid)) {
          const double dist = (tgt->position_mkm - attacker.position_mkm).length();
          if (dist <= ad->weapon_range_mkm) {
            if (is_target_boardable(attacker, *tgt)) {
              // Intent to capture: stop firing once disabled.
              continue;
            }
            chosen = tid;
            chosen_dist = dist;
          }
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

  // --- planetary / colony defenses ---
  for (const auto& bat : colony_batteries) {
    if (bat.weapon_damage <= 1e-12 || bat.weapon_range_mkm <= 1e-12) continue;

    const auto& detected_hostiles = detected_hostiles_for(bat.faction_id, bat.system_id);
    if (detected_hostiles.empty()) continue;

    auto& idx = index_for_system(bat.system_id);
    const auto nearby = idx.query_radius(bat.position_mkm, bat.weapon_range_mkm, 0.0);

    Id chosen = kInvalidId;
    double chosen_dist = 1e300;
    for (Id tid : nearby) {
      if (!std::binary_search(detected_hostiles.begin(), detected_hostiles.end(), tid)) continue;

      const auto* tgt = find_ptr(state_.ships, tid);
      if (!tgt) continue;
      if (tgt->system_id != bat.system_id) continue;

      const double dist = (tgt->position_mkm - bat.position_mkm).length();
      if (dist > bat.weapon_range_mkm + 1e-9) continue;

      if (dist + 1e-9 < chosen_dist || (std::abs(dist - chosen_dist) <= 1e-9 && (chosen == kInvalidId || tid < chosen))) {
        chosen = tid;
        chosen_dist = dist;
      }
    }

    if (chosen != kInvalidId) {
      incoming_damage[chosen] += bat.weapon_damage;
      colony_attackers_for_target[chosen].push_back(bat.colony_id);
    }
  }

  // If nothing happened and boarding is disabled, exit early.
  if (incoming_damage.empty() && !do_boarding) return;

  // --- apply damage ---
  std::vector<Id> destroyed;

  // Track how much damage was absorbed by shields vs applied to hull.
  std::unordered_map<Id, double> shield_damage;
  std::unordered_map<Id, double> hull_damage;
  std::unordered_map<Id, double> pre_hp;
  std::unordered_map<Id, double> pre_shields;

  if (!incoming_damage.empty()) {
    destroyed.reserve(incoming_damage.size());
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

        // Colony attacker (planetary defenses).
        Id attacker_colony_id = kInvalidId;
        Id attacker_col_fid = kInvalidId;
        std::string attacker_col_name;
        std::string attacker_col_fac_name;
        std::size_t colony_attackers_count = 0;

        if (auto itc = colony_attackers_for_target.find(tid); itc != colony_attackers_for_target.end()) {
          auto& vec = itc->second;
          std::sort(vec.begin(), vec.end());
          vec.erase(std::unique(vec.begin(), vec.end()), vec.end());
          colony_attackers_count = vec.size();
          if (!vec.empty()) {
            attacker_colony_id = vec.front();
            if (const auto* col = find_ptr(state_.colonies, attacker_colony_id)) {
              attacker_col_fid = col->faction_id;
              attacker_col_name = col->name;
              if (const auto* af = find_ptr(state_.factions, attacker_col_fid)) attacker_col_fac_name = af->name;
            }
          }
        }

        const Id effective_attacker_fid = (attacker_fid != kInvalidId) ? attacker_fid : attacker_col_fid;

        EventContext ctx;
        ctx.faction_id = tgt->faction_id;
        ctx.faction_id2 = effective_attacker_fid;
        ctx.system_id = tgt->system_id;
        ctx.ship_id = tid;
        if (attacker_ship_id == kInvalidId && attacker_colony_id != kInvalidId) ctx.colony_id = attacker_colony_id;

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

        if (attacker_ship_id != kInvalidId || attacker_colony_id != kInvalidId) {
          msg += " (attacked by ";
          bool first = true;

          if (attacker_ship_id != kInvalidId) {
            msg += (attacker_ship_name.empty() ? (std::string("Ship ") + std::to_string(attacker_ship_id))
                                               : attacker_ship_name);
            if (!attacker_fac_name.empty()) msg += " / " + attacker_fac_name;
            if (attackers_count > 1) msg += " +" + std::to_string(attackers_count - 1) + " more";
            first = false;
          }

          if (attacker_colony_id != kInvalidId) {
            if (!first) msg += ", ";
            msg += "Colony defenses at " + (attacker_col_name.empty() ? (std::string("Colony ") + std::to_string(attacker_colony_id))
                                                                      : attacker_col_name);
            if (!attacker_col_fac_name.empty()) msg += " / " + attacker_col_fac_name;
            if (colony_attackers_count > 1) msg += " +" + std::to_string(colony_attackers_count - 1) + " more";
          }

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

      // Colony attacker (planetary defenses).
      Id attacker_colony_id = kInvalidId;
      Id attacker_col_fid = kInvalidId;
      std::string attacker_col_name;
      std::string attacker_col_fac_name;
      std::size_t colony_attackers_count = 0;

      if (auto itc = colony_attackers_for_target.find(dead_id);
          itc != colony_attackers_for_target.end()) {
        auto& vec = itc->second;
        std::sort(vec.begin(), vec.end());
        vec.erase(std::unique(vec.begin(), vec.end()), vec.end());
        colony_attackers_count = vec.size();
        if (!vec.empty()) {
          attacker_colony_id = vec.front();
          if (const auto* col = find_ptr(state_.colonies, attacker_colony_id)) {
            attacker_col_fid = col->faction_id;
            attacker_col_name = col->name;
            if (const auto* af = find_ptr(state_.factions, attacker_col_fid)) attacker_col_fac_name = af->name;
          }
        }
      }

      const Id effective_attacker_fid = (attacker_fid != kInvalidId) ? attacker_fid : attacker_col_fid;

      EventContext ctx;
      ctx.faction_id = victim_fid;
      ctx.faction_id2 = effective_attacker_fid;
      ctx.system_id = sys_id;
      ctx.ship_id = dead_id;
      if (attacker_ship_id == kInvalidId && attacker_colony_id != kInvalidId) ctx.colony_id = attacker_colony_id;

      std::string msg = "Ship destroyed: " + victim.name;
      msg += " (" + victim_fac_name + ")";
      msg += " in " + sys_name;

      if (attacker_ship_id != kInvalidId || attacker_colony_id != kInvalidId) {
        msg += " (killed by ";
        bool first = true;

        if (attacker_ship_id != kInvalidId) {
          msg += (attacker_ship_name.empty() ? (std::string("Ship ") + std::to_string(attacker_ship_id)) : attacker_ship_name);
          if (!attacker_fac_name.empty()) msg += " / " + attacker_fac_name;
          if (attackers_count > 1) msg += " +" + std::to_string(attackers_count - 1) + " more";
          first = false;
        }

        if (attacker_colony_id != kInvalidId) {
          if (!first) msg += ", ";
          msg += "Colony defenses at " + (attacker_col_name.empty() ? (std::string("Colony ") + std::to_string(attacker_colony_id))
                                                                    : attacker_col_name);
          if (!attacker_col_fac_name.empty()) msg += " / " + attacker_col_fac_name;
          if (colony_attackers_count > 1) msg += " +" + std::to_string(colony_attackers_count - 1) + " more";
        }

        msg += ")";
      }

      death_events.push_back(DestructionEvent{std::move(msg), ctx});

      // Spawn a salvageable wreck at the destruction site.
      //
      // Wreck mineral contents are a coarse approximation:
      //  - A fraction of the destroyed ship's carried cargo.
      //  - A fraction of the destroyed ship's hull mass converted using the (default) shipyard
      //    build_costs_per_ton (fallback: Duranium/Neutronium).
      if (cfg_.enable_wrecks) {
        std::unordered_map<std::string, double> salvage;

        const double cargo_frac = std::clamp(cfg_.wreck_cargo_salvage_fraction, 0.0, 1.0);
        if (cargo_frac > 1e-9) {
          for (const auto& [mineral, tons] : victim.cargo) {
            if (tons > 1e-9) salvage[mineral] += tons * cargo_frac;
          }
        }

        const double hull_frac = std::max(0.0, cfg_.wreck_hull_salvage_fraction);
        if (hull_frac > 1e-9) {
          double mass_tons = 0.0;
          if (const auto* d = find_design(victim.design_id)) {
            mass_tons = std::max(0.0, d->mass_tons);
          }
          const double hull_tons = mass_tons * hull_frac;

          // Prefer an explicit shipyard mineral recipe if available.
          const InstallationDef* yard = nullptr;
          if (auto it_y = content_.installations.find("shipyard"); it_y != content_.installations.end()) {
            yard = &it_y->second;
          }

          if (yard && !yard->build_costs_per_ton.empty()) {
            for (const auto& [mineral, cost_per_ton] : yard->build_costs_per_ton) {
              if (cost_per_ton > 1e-12) salvage[mineral] += hull_tons * cost_per_ton;
            }
          } else {
            salvage["Duranium"] += hull_tons * 1.0;
            salvage["Neutronium"] += hull_tons * 0.1;
          }
        }

        // Prune non-positive / non-finite entries.
        for (auto it = salvage.begin(); it != salvage.end();) {
          const double v = it->second;
          if (!(v > 1e-9) || std::isnan(v) || std::isinf(v)) {
            it = salvage.erase(it);
          } else {
            ++it;
          }
        }

        if (!salvage.empty()) {
          Wreck w;
          w.id = allocate_id(state_);
          w.name = "Wreck: " + victim.name;
          w.system_id = victim.system_id;
          w.position_mkm = victim.position_mkm;
          w.minerals = std::move(salvage);
          w.source_ship_id = victim.id;
          w.source_faction_id = victim.faction_id;
          w.source_design_id = victim.design_id;
          w.created_day = state_.date.days_since_epoch();
          state_.wrecks[w.id] = std::move(w);
        }
      }
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
  } // end damage application

  // --- boarding / capture ---
  if (do_boarding) {
    struct BestBoarder {
      Id attacker_id{kInvalidId};
      double troops{0.0};
    };

    std::unordered_map<Id, BestBoarder> best_for_target;
    best_for_target.reserve(32);

    for (Id aid : ship_ids) {
      const auto* attacker_ptr = find_ptr(state_.ships, aid);
      if (!attacker_ptr) continue;
      const Ship& attacker = *attacker_ptr;

      if (attacker.troops + 1e-9 < cfg_.boarding_min_attacker_troops) continue;

      const Id tid = attack_order_target(aid);
      if (tid == kInvalidId) continue;

      const auto* tgt_ptr = find_ptr(state_.ships, tid);
      if (!tgt_ptr) continue;
      const Ship& target = *tgt_ptr;

      if (target.system_id != attacker.system_id) continue;
      if (target.faction_id == attacker.faction_id) continue;
      if (!is_target_boardable(attacker, target)) continue;

      const auto& detected_hostiles = detected_hostiles_for(attacker.faction_id, attacker.system_id);
      if (!std::binary_search(detected_hostiles.begin(), detected_hostiles.end(), tid)) continue;

      const double dist = (target.position_mkm - attacker.position_mkm).length();
      if (dist > cfg_.boarding_range_mkm + 1e-9) continue;

      auto it = best_for_target.find(tid);
      if (it == best_for_target.end()) {
        best_for_target.emplace(tid, BestBoarder{aid, attacker.troops});
      } else {
        if (attacker.troops > it->second.troops + 1e-9 ||
            (std::abs(attacker.troops - it->second.troops) <= 1e-9 && aid < it->second.attacker_id)) {
          it->second = BestBoarder{aid, attacker.troops};
        }
      }
    }

    for (Id tid : sorted_keys(best_for_target)) {
      const auto itb = best_for_target.find(tid);
      if (itb == best_for_target.end()) continue;

      const Id aid = itb->second.attacker_id;
      auto* attacker = find_ptr(state_.ships, aid);
      auto* target = find_ptr(state_.ships, tid);
      if (!attacker || !target) continue;

      // Re-validate (state may have changed due to earlier captures this tick).
      if (target->faction_id == attacker->faction_id) continue;
      if (target->system_id != attacker->system_id) continue;
      if (!is_target_boardable(*attacker, *target)) continue;

      const double dist = (target->position_mkm - attacker->position_mkm).length();
      if (dist > cfg_.boarding_range_mkm + 1e-9) continue;

      const double attacker_strength = std::max(0.0, attacker->troops);
      if (attacker_strength + 1e-9 < cfg_.boarding_min_attacker_troops) continue;

      const double max_hp = ship_max_hp(*target);
      const double defender_strength =
          std::max(0.0, target->troops) + std::max(0.0, cfg_.boarding_defense_hp_factor) * std::max(0.0, max_hp);

      const double denom = std::max(1e-9, attacker_strength + defender_strength);
      const double chance = clamp01(attacker_strength / denom);

      const uint64_t day = static_cast<uint64_t>(std::max<long long>(0, state_.date.days_since_epoch()));
      uint64_t seed = day;
      seed ^= (static_cast<uint64_t>(aid) * 0x9e3779b97f4a7c15ULL);
      seed ^= (static_cast<uint64_t>(tid) * 0xbf58476d1ce4e5b9ULL);
      const double roll = u01_from_u64(splitmix64(seed));

      const double att_loss_frac = clamp01(cfg_.boarding_attacker_casualty_fraction);
      const double def_loss_frac = clamp01(cfg_.boarding_defender_casualty_fraction);

      const double ratio_def = defender_strength / denom;
      const double ratio_att = attacker_strength / denom;

      const double att_loss = attacker_strength * att_loss_frac * ratio_def;
      const double def_loss = std::max(0.0, target->troops) * def_loss_frac * ratio_att;

      attacker->troops = std::max(0.0, attacker->troops - att_loss);
      target->troops = std::max(0.0, target->troops - def_loss);

      const auto* sys = find_ptr(state_.systems, target->system_id);
      const std::string sys_name = sys ? sys->name : std::string("(unknown)");

      const Id old_fid = target->faction_id;
      const Id new_fid = attacker->faction_id;

      if (roll < chance) {
        // Capture!
        target->faction_id = new_fid;

        // Clear orders: the original owner no longer controls the ship.
        if (auto oit = state_.ship_orders.find(tid); oit != state_.ship_orders.end()) {
          oit->second.queue.clear();
          oit->second.repeat = false;
          oit->second.repeat_count_remaining = 0;
          oit->second.repeat_template.clear();
        }

        // Remove from fleets (enemy fleet membership is invalid after capture).
        remove_ship_from_fleets(tid);

        // Purge existing contacts for this ship so everyone re-identifies it next tick.
        for (auto& [_, fac] : state_.factions) {
          fac.ship_contacts.erase(tid);
        }

        // Capture is an act of hostility.
        if (!are_factions_hostile(new_fid, old_fid)) {
          set_diplomatic_status(new_fid, old_fid, DiplomacyStatus::Hostile, /*reciprocal=*/true,
                                /*push_event_on_change=*/true);
        }

        EventContext ctx;
        ctx.faction_id = old_fid;
        ctx.faction_id2 = new_fid;
        ctx.system_id = target->system_id;
        ctx.ship_id = tid;

        std::string msg = "Ship captured: " + target->name;
        msg += " in " + sys_name;
        msg += " (boarded by " + attacker->name;
        if (const auto* af = find_ptr(state_.factions, new_fid)) {
          msg += " / " + af->name;
        }
        msg += ", troops lost " + fmt1(att_loss) + ")";

        nebula4x::log::warn(msg);
        push_event(EventLevel::Warn, EventCategory::Combat, msg, ctx);
      } else {
        // Boarding attempt failed.
        if (!cfg_.boarding_log_failures) {
          continue;
        }

        EventContext ctx;
        ctx.faction_id = new_fid;
        ctx.faction_id2 = old_fid;
        ctx.system_id = target->system_id;
        ctx.ship_id = tid;

        std::string msg = "Boarding failed: " + attacker->name + " -> " + target->name;
        msg += " in " + sys_name;
        msg += " (p=" + fmt1(chance * 100.0) + "%, lost " + fmt1(att_loss) + ")";

        push_event(EventLevel::Info, EventCategory::Combat, msg, ctx);
      }
    }
  }
}

} // namespace nebula4x
