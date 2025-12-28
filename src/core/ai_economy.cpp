#include "nebula4x/core/ai_economy.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_set>
#include <vector>

#include "nebula4x/core/game_state.h"

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
      return d.cargo_tons * 10.0 + d.speed_km_s * 5.0;
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
    recommended = {"chemistry_1", "nuclear_1", "propulsion_1", "sensors_1", "armor_1",
                   "weapons_1",   "reactors_2", "propulsion_2"};
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
    int best_cost = 0;
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

  const auto it_yard = sim.content().installations.find("shipyard");
  const auto it_mine = sim.content().installations.find("automated_mine");
  if (it_yard == sim.content().installations.end() || it_mine == sim.content().installations.end()) return 0;

  const InstallationDef& yard = it_yard->second;
  const InstallationDef& mine = it_mine->second;

  const double rate_per_day = std::max(0.0, yard.build_rate_tons_per_day) * static_cast<double>(yards);
  if (rate_per_day <= 1e-9) return 0;

  int target = 0;
  for (const auto& [mineral, cost_per_ton] : yard.build_costs_per_ton) {
    if (cost_per_ton <= 0.0) continue;
    const double required = rate_per_day * cost_per_ton;

    auto it_prod = mine.produces_per_day.find(mineral);
    if (it_prod == mine.produces_per_day.end()) continue;
    const double per_day = it_prod->second;
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
    } else if (d->role == ShipRole::Freighter) {
      sh->auto_freight = true;
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
  int budget = 1;
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
