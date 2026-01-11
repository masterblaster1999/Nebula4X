#include "nebula4x/core/ai_economy.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "nebula4x/core/game_state.h"

// Internal helper for tech-driven economy modifiers.
#include "simulation_internal.h"

namespace nebula4x {
namespace {

// Many core containers are stored as std::unordered_map for convenience.
// Iteration order of unordered_map is not specified, so relying on it can
// introduce cross-platform nondeterminism.
template <typename Map>
std::vector<typename Map::key_type> sorted_keys(const Map& m) {
  std::vector<typename Map::key_type> out;
  out.reserve(m.size());
  for (const auto& kv : m) out.push_back(kv.first);
  std::sort(out.begin(), out.end());
  return out;
}

template <typename T>
bool vec_contains(const std::vector<T>& v, const T& x) {
  return std::find(v.begin(), v.end(), x) != v.end();
}

template <typename T>
void push_unique(std::vector<T>& v, const T& x) {
  if (!vec_contains(v, x)) v.push_back(x);
}

bool tech_known(const Faction& f, const std::string& id) { return vec_contains(f.known_techs, id); }

int queued_installation_units(const Colony& c, const std::string& inst_id) {
  int q = 0;
  for (const auto& o : c.construction_queue) {
    if (o.installation_id != inst_id) continue;
    q += std::max(0, o.quantity_remaining);
  }
  return q;
}

struct ShipRoleCounts {
  int freighters = 0;
  int surveyors = 0;
  int combatants = 0;
};

ShipRoleCounts count_ships_by_role(const Simulation& sim, Id faction_id) {
  ShipRoleCounts out;
  const auto ship_ids = sorted_keys(sim.state().ships);
  for (Id sid : ship_ids) {
    const auto* sh = find_ptr(sim.state().ships, sid);
    if (!sh || sh->faction_id != faction_id) continue;

    const auto* d = sim.find_design(sh->design_id);
    const ShipRole r = d ? d->role : ShipRole::Unknown;

    if (r == ShipRole::Freighter) out.freighters++;
    if (r == ShipRole::Surveyor) out.surveyors++;
    if (r == ShipRole::Combatant) out.combatants++;
  }
  return out;
}

ShipRoleCounts count_queued_ships_by_role(const Simulation& sim, Id faction_id) {
  ShipRoleCounts out;
  const auto colony_ids = sorted_keys(sim.state().colonies);
  for (Id cid : colony_ids) {
    const auto* c = find_ptr(sim.state().colonies, cid);
    if (!c || c->faction_id != faction_id) continue;

    for (const auto& bo : c->shipyard_queue) {
      if (bo.refit_ship_id != kInvalidId) continue;  // refits aren't new hulls
      const auto* d = sim.find_design(bo.design_id);
      const ShipRole r = d ? d->role : ShipRole::Unknown;
      if (r == ShipRole::Freighter) out.freighters++;
      if (r == ShipRole::Surveyor) out.surveyors++;
      if (r == ShipRole::Combatant) out.combatants++;
    }
  }
  return out;
}

struct ShipTargets {
  int freighters = 0;
  int surveyors = 0;
  int combatants = 0;
};

ShipTargets desired_ship_counts(const Simulation& sim, const Faction& f) {
  ShipTargets t;

  if (f.control == FactionControl::AI_Explorer) {
    t.freighters = 2;
    t.surveyors = 2;
    t.combatants = 1;
  } else if (f.control == FactionControl::AI_Pirate) {
    // Pirates scale up as they gain propulsion tech; this ensures they keep
    // producing hulls and gradually transition to better designs.
    t.combatants = 4;
    if (tech_known(f, "nuclear_1")) t.combatants += 2;
    if (tech_known(f, "propulsion_1")) t.combatants += 2;
    if (tech_known(f, "propulsion_2")) t.combatants += 2;
  }

  // Small bonus if they have multiple colonies: helps keep logistics alive.
  int colony_count = 0;
  for (const auto& [_, c] : sim.state().colonies) {
    if (c.faction_id == f.id) colony_count++;
  }
  if (f.control == FactionControl::AI_Explorer && colony_count >= 2) {
    t.freighters = std::max(t.freighters, 3);
  }

  return t;
}

double design_score_for_role(const ShipDesign& d, ShipRole role) {
  switch (role) {
    case ShipRole::Combatant:
      return d.weapon_damage * 1000.0 + d.max_hp * 10.0 + d.speed_km_s * 20.0 + d.sensor_range_mkm;
    case ShipRole::Surveyor:
      return d.sensor_range_mkm * 100.0 + d.speed_km_s * 20.0;
    case ShipRole::Freighter:
      // Freight ships are scored primarily by throughput (cargo) and speed.
      //
      // However, colony ships and troop transports often share the same role in
      // content because they are "civilian hulls". For logistics planning, we
      // prefer dedicated freighters so that colonizers/transports are not
      // accidentally consumed by other automation.
      return d.cargo_tons * 10.0 + d.speed_km_s * 5.0 - d.colony_capacity_millions * 0.1 - d.troop_capacity * 0.1;
    default:
      return 0.0;
  }
}

std::string best_design_for_role(const Simulation& sim, Id faction_id, ShipRole role) {
  std::string best;
  double best_score = -1.0;

  auto consider = [&](const std::string& id, const ShipDesign& d) {
    if (d.role != role) return;
    if (!sim.is_design_buildable_for_faction(faction_id, id)) return;

    const double score = design_score_for_role(d, role);
    if (best.empty() || score > best_score + 1e-9 || (std::abs(score - best_score) <= 1e-9 && id < best)) {
      best = id;
      best_score = score;
    }
  };

  for (const auto& id : sorted_keys(sim.content().designs)) consider(id, sim.content().designs.at(id));
  for (const auto& id : sorted_keys(sim.state().custom_designs)) consider(id, sim.state().custom_designs.at(id));

  return best;
}

bool design_is_colonizer(const ShipDesign& d) {
  return d.colony_capacity_millions > 0.0;
}

int count_colonizer_ships(const Simulation& sim, Id faction_id) {
  int n = 0;
  for (const auto& [_, sh] : sim.state().ships) {
    if (sh.faction_id != faction_id) continue;
    const auto* d = sim.find_design(sh.design_id);
    if (!d) continue;
    if (design_is_colonizer(*d)) n += 1;
  }
  return n;
}

int count_queued_colonizer_ships(const Simulation& sim, Id faction_id) {
  int n = 0;
  for (const auto& [_, c] : sim.state().colonies) {
    if (c.faction_id != faction_id) continue;
    for (const auto& bo : c.shipyard_queue) {
      if (bo.is_refit()) continue;
      const auto* d = sim.find_design(bo.design_id);
      if (!d) continue;
      if (design_is_colonizer(*d)) n += 1;
    }
  }
  return n;
}

bool faction_has_colonization_target(const Simulation& sim, Id faction_id) {
  std::unordered_set<Id> colonized_bodies;
  colonized_bodies.reserve(sim.state().colonies.size() * 2 + 8);
  for (const auto& [_, c] : sim.state().colonies) colonized_bodies.insert(c.body_id);

  for (Id bid : sorted_keys(sim.state().bodies)) {
    const auto* b = find_ptr(sim.state().bodies, bid);
    if (!b) continue;
    if (b->system_id == kInvalidId) continue;
    if (!sim.is_system_discovered_by_faction(faction_id, b->system_id)) continue;
    if (colonized_bodies.count(bid)) continue;

    // Keep this conservative: only consider obvious colonizable bodies.
    if (b->type != BodyType::Planet && b->type != BodyType::Moon && b->type != BodyType::Asteroid) continue;
    return true;
  }
  return false;
}

std::string best_colonizer_design(const Simulation& sim, Id faction_id) {
  std::string best;
  double best_score = -1.0;

  auto consider = [&](const std::string& id, const ShipDesign& d) {
    if (!design_is_colonizer(d)) return;
    if (!sim.is_design_buildable_for_faction(faction_id, id)) return;

    // Prefer higher colony capacity, then speed.
    const double score = d.colony_capacity_millions * 1000.0 + d.speed_km_s * 20.0 + d.cargo_tons * 0.1;
    if (best.empty() || score > best_score + 1e-9 || (std::abs(score - best_score) <= 1e-9 && id < best)) {
      best = id;
      best_score = score;
    }
  };

  for (const auto& id : sorted_keys(sim.content().designs)) consider(id, sim.content().designs.at(id));
  for (const auto& id : sorted_keys(sim.state().custom_designs)) consider(id, sim.state().custom_designs.at(id));

  return best;
}

void prune_research_queue(const ContentDB& content, Faction& f) {
  std::vector<std::string> cleaned;
  cleaned.reserve(f.research_queue.size());
  for (const auto& tid : f.research_queue) {
    if (tid.empty()) continue;
    if (tech_known(f, tid)) continue;
    if (content.techs.find(tid) == content.techs.end()) continue;
    if (!vec_contains(cleaned, tid)) cleaned.push_back(tid);
  }
  f.research_queue = std::move(cleaned);
}

void ensure_research_plan(const Simulation& sim, Faction& f) {
  prune_research_queue(sim.content(), f);

  // Don't interfere with a valid in-progress research item.
  if (!f.active_research_id.empty()) {
    const bool exists = sim.content().techs.find(f.active_research_id) != sim.content().techs.end();
    if (exists && !tech_known(f, f.active_research_id)) return;
  }

  std::vector<std::string> recommended;
  if (f.control == FactionControl::AI_Explorer) {
    recommended = {"chemistry_1",    "nuclear_1",   "propulsion_1", "colonization_1",
                   "sensors_1",      "armor_1",     "weapons_1",    "reactors_2",
                   "propulsion_2"};
  } else if (f.control == FactionControl::AI_Pirate) {
    // Pirates need chemistry -> nuclear -> propulsion to start upgrading hulls.
    recommended = {"chemistry_1", "nuclear_1", "propulsion_1", "weapons_1", "sensors_1",
                   "armor_1",     "reactors_2", "propulsion_2"};
  }

  for (const auto& tid : recommended) {
    if (sim.content().techs.find(tid) == sim.content().techs.end()) continue;
    if (tech_known(f, tid)) continue;
    if (tid == f.active_research_id) continue;
    push_unique(f.research_queue, tid);
  }

  // If still nothing queued, pick a cheapest currently-researchable tech.
  if (f.research_queue.empty()) {
    std::string best;
    double best_cost = 0.0;
    for (const auto& tid : sorted_keys(sim.content().techs)) {
      const auto& t = sim.content().techs.at(tid);
      if (tech_known(f, tid)) continue;
      bool prereqs_met = true;
      for (const auto& pre : t.prereqs) {
        if (!tech_known(f, pre)) {
          prereqs_met = false;
          break;
        }
      }
      if (!prereqs_met) continue;

      if (best.empty() || t.cost < best_cost || (t.cost == best_cost && tid < best)) {
        best = tid;
        best_cost = t.cost;
      }
    }
    if (!best.empty()) f.research_queue.push_back(best);
  }
}

Id primary_shipyard_colony(const Simulation& sim, Id faction_id) {
  Id best = kInvalidId;
  int best_yards = -1;

  const auto colony_ids = sorted_keys(sim.state().colonies);
  for (Id cid : colony_ids) {
    const auto* c = find_ptr(sim.state().colonies, cid);
    if (!c || c->faction_id != faction_id) continue;

    const int yards = c->installations.count("shipyard") ? c->installations.at("shipyard") : 0;
    if (yards > best_yards || (yards == best_yards && (best == kInvalidId || cid < best))) {
      best = cid;
      best_yards = yards;
    }
  }
  return best;
}

int mine_target_for_colony(const Simulation& sim, const Colony& c) {
  const int yards = c.installations.count("shipyard") ? c.installations.at("shipyard") : 0;
  if (yards <= 0) return 0;

  // Tech-driven multipliers (e.g. shipyard throughput and mining efficiency)
  // should be reflected in planning targets; otherwise AIs will under-build
  // mines after researching economic upgrades.
  sim_internal::FactionEconomyMultipliers mult;
  if (const auto* fac = find_ptr(sim.state().factions, c.faction_id)) {
    mult = sim_internal::compute_faction_economy_multipliers(sim.content(), *fac);
    // Match Simulation::tick_colonies: trade agreements boost non-mining outputs.
    const double trade = sim_internal::trade_agreement_output_multiplier(sim.state(), c.faction_id);
    mult.shipyard *= trade;
  }
  const double shipyard_mult = std::max(0.0, mult.shipyard);
  const double mining_mult = std::max(0.0, mult.mining);

  const auto it_yard = sim.content().installations.find("shipyard");
  const auto it_mine = sim.content().installations.find("automated_mine");
  if (it_yard == sim.content().installations.end() || it_mine == sim.content().installations.end()) return 0;

  const InstallationDef& yard = it_yard->second;
  const InstallationDef& mine = it_mine->second;

  const double rate_per_day = std::max(0.0, yard.build_rate_tons_per_day) * static_cast<double>(yards) * shipyard_mult;
  if (rate_per_day <= 1e-9) return 0;

  // Determine expected per-mine yields. If the mine uses the modern
  // 'tons per day' model, distribute output across the body's deposits
  // by remaining composition (mirrors Simulation::tick_colonies).
  std::unordered_map<std::string, double> per_mine_yield;
  bool using_generic = false;

  if (mine.mining_tons_per_day > 0.0) {
    if (const Body* b = find_ptr(sim.state().bodies, c.body_id)) {
      if (!b->mineral_deposits.empty()) {
        double total_remaining = 0.0;
        for (const auto& [mineral, rem_raw] : b->mineral_deposits) {
          const double rem = std::max(0.0, rem_raw);
          if (rem > 1e-12) total_remaining += rem;
        }
        if (total_remaining > 1e-12) {
          using_generic = true;
          const double cap = mine.mining_tons_per_day * mining_mult; // per mine per day
          for (const auto& [mineral, rem_raw] : b->mineral_deposits) {
            const double rem = std::max(0.0, rem_raw);
            if (rem <= 1e-12) continue;
            per_mine_yield[mineral] = cap * (rem / total_remaining);
          }
        }
      }
    }
  }

  // Legacy fixed-output mines.
  if (!using_generic) {
    for (const auto& [mineral, per_day_raw] : mine.produces_per_day) {
      const double per_day = std::max(0.0, per_day_raw) * mining_mult;
      if (per_day <= 1e-12) continue;
      per_mine_yield[mineral] = per_day;
    }
  }

  int target = 0;
  for (const auto& [mineral, cost_per_ton_raw] : yard.build_costs_per_ton) {
    const double cost_per_ton = std::max(0.0, cost_per_ton_raw);
    if (cost_per_ton <= 1e-12) continue;
    const double required = rate_per_day * cost_per_ton;

    auto it = per_mine_yield.find(mineral);
    if (it == per_mine_yield.end()) continue; // not mineable locally
    const double per_day = it->second;
    if (per_day <= 1e-9) continue;

    const int needed = static_cast<int>(std::ceil(required / per_day));
    target = std::max(target, needed);
  }

  // Small buffer.
  return std::max(0, target + 2);
}

void ensure_installations_for_colony(Simulation& sim, Id faction_id, Colony& c, const Faction& f) {
  // A minimal baseline so AI colonies can "do things".
  const int desired_shipyards = 1;
  const int desired_sensors = 1;

  const int desired_factories = (f.control == FactionControl::AI_Pirate) ? 3 : 5;
  const int desired_labs = (f.control == FactionControl::AI_Pirate) ? 5 : 20;

  int mine_target = mine_target_for_colony(sim, c);
  if (f.control == FactionControl::AI_Pirate) mine_target = std::max(mine_target, 12);
  if (f.control == FactionControl::AI_Explorer) mine_target = std::max(mine_target, 20);

  struct Want {
    const char* id;
    int count;
  };

  const Want wants[] = {
      {"shipyard", desired_shipyards},
      {"sensor_station", desired_sensors},
      {"construction_factory", desired_factories},
      {"research_lab", desired_labs},
      {"automated_mine", mine_target},
  };

  for (const auto& w : wants) {
    if (w.count <= 0) continue;
    if (!sim.is_installation_buildable_for_faction(faction_id, w.id)) continue;

    const int have = (c.installations.count(w.id) ? c.installations.at(w.id) : 0);
    const int queued = queued_installation_units(c, w.id);
    const int total = have + queued;
    if (total >= w.count) continue;

    const int add = w.count - total;
    (void)sim.enqueue_installation_build(c.id, w.id, add);
  }
}

void enable_ship_automation_for_faction(Simulation& sim, Id faction_id, const Faction& f) {
  if (f.control != FactionControl::AI_Explorer) return;

  auto& st = sim.state();
  const auto ship_ids = sorted_keys(st.ships);
  for (Id sid : ship_ids) {
    auto* sh = find_ptr(st.ships, sid);
    if (!sh || sh->faction_id != faction_id) continue;

    const auto* d = sim.find_design(sh->design_id);
    if (!d) continue;

    if (d->role == ShipRole::Surveyor) {
      sh->auto_explore = true;
      sh->auto_freight = false;
      sh->auto_colonize = false;
    } else if (d->role == ShipRole::Freighter) {
      // Prefer dedicated cargo hulls for logistics. If the ship has a colony
      // module, treat it as a colonizer instead.
      if (d->colony_capacity_millions > 0.0) {
        sh->auto_colonize = true;
        sh->auto_freight = false;
        sh->auto_explore = false;
      } else {
        sh->auto_freight = true;
        sh->auto_explore = false;
        sh->auto_colonize = false;
      }
    }
  }
}

void ensure_shipbuilding_pipeline(Simulation& sim, Id faction_id, const Faction& f) {
  const Id colony_id = primary_shipyard_colony(sim, faction_id);
  if (colony_id == kInvalidId) return;

  auto* col = find_ptr(sim.state().colonies, colony_id);
  if (!col) return;

  const int yards = col->installations.count("shipyard") ? col->installations.at("shipyard") : 0;
  if (yards <= 0) return;

  const ShipTargets desired = desired_ship_counts(sim, f);
  const ShipRoleCounts have = count_ships_by_role(sim, faction_id);
  const ShipRoleCounts queued = count_queued_ships_by_role(sim, faction_id);

  const int have_f = have.freighters + queued.freighters;
  const int have_s = have.surveyors + queued.surveyors;
  const int have_c = have.combatants + queued.combatants;

  // Keep the shipyard queue short-ish so upgrades can naturally take effect when
  // new designs become buildable.
  constexpr std::size_t kMaxQueue = 3;
  if (col->shipyard_queue.size() >= kMaxQueue) return;

  struct Need {
    ShipRole role;
    int missing;
  };

  std::vector<Need> needs;
  needs.reserve(3);

  if (desired.surveyors > have_s) needs.push_back({ShipRole::Surveyor, desired.surveyors - have_s});
  if (desired.freighters > have_f) needs.push_back({ShipRole::Freighter, desired.freighters - have_f});
  if (desired.combatants > have_c) needs.push_back({ShipRole::Combatant, desired.combatants - have_c});

  // Enqueue a small number of ships (typically 1/day) so the AI doesn't explode
  // the queue and so design upgrades can take effect.
  int budget = 1;

  // Explorer AI: keep at least one colonizer available when there are
  // discovered uncolonized bodies. We avoid stealing the daily build slot
  // from early exploration by only doing this once surveyors are satisfied.
  if (f.control == FactionControl::AI_Explorer && budget > 0) {
    const int total_colonizers = count_colonizer_ships(sim, faction_id) + count_queued_colonizer_ships(sim, faction_id);
    if (total_colonizers < 1 && desired.surveyors <= have_s && faction_has_colonization_target(sim, faction_id)) {
      const std::string best = best_colonizer_design(sim, faction_id);
      if (!best.empty()) {
        (void)sim.enqueue_build(colony_id, best);
        budget -= 1;
      }
    }
  }

  if (budget <= 0) return;
  if (needs.empty()) return;

  auto role_prio = [&](ShipRole r) {
    if (f.control == FactionControl::AI_Pirate) {
      return (r == ShipRole::Combatant) ? 0 : 10;
    }
    if (r == ShipRole::Surveyor) return 0;
    if (r == ShipRole::Freighter) return 1;
    if (r == ShipRole::Combatant) return 2;
    return 3;
  };

  std::sort(needs.begin(), needs.end(), [&](const Need& a, const Need& b) {
    if (role_prio(a.role) != role_prio(b.role)) return role_prio(a.role) < role_prio(b.role);
    if (a.missing != b.missing) return a.missing > b.missing;
    return static_cast<int>(a.role) < static_cast<int>(b.role);
  });

  // Enqueue a small number of ships (typically 1/day) so the AI doesn't explode
  // the queue and so design upgrades can take effect.
  for (const auto& need : needs) {
    if (budget <= 0) break;
    if (need.missing <= 0) continue;

    const std::string best = best_design_for_role(sim, faction_id, need.role);
    if (best.empty()) continue;

    (void)sim.enqueue_build(colony_id, best);
    budget -= 1;
  }
}

}  // namespace

void tick_ai_economy(Simulation& sim) {
  auto& st = sim.state();
  const auto faction_ids = sorted_keys(st.factions);

  for (Id fid : faction_ids) {
    auto* f = find_ptr(st.factions, fid);
    if (!f) continue;

    if (f->control == FactionControl::Player) continue;
    if (f->control == FactionControl::AI_Passive) continue;

    // Skip factions that do not (yet) own colonies.
    bool has_colony = false;
    for (const auto& [_, c] : st.colonies) {
      if (c.faction_id == fid) {
        has_colony = true;
        break;
      }
    }
    if (!has_colony) continue;

    ensure_research_plan(sim, *f);

    // Colony infrastructure baseline.
    const auto colony_ids = sorted_keys(st.colonies);
    for (Id cid : colony_ids) {
      auto* c = find_ptr(st.colonies, cid);
      if (!c || c->faction_id != fid) continue;
      ensure_installations_for_colony(sim, fid, *c, *f);
    }

    // Ship automation flags for explorer AI.
    enable_ship_automation_for_faction(sim, fid, *f);

    // Maintain a small shipbuilding pipeline at the primary shipyard.
    ensure_shipbuilding_pipeline(sim, fid, *f);
  }
}

}  // namespace nebula4x
