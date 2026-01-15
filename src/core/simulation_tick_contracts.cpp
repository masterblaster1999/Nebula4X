#include "nebula4x/core/simulation.h"

#include "simulation_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "nebula4x/util/log.h"
#include "nebula4x/util/trace_events.h"

namespace nebula4x {
namespace {
using sim_internal::sorted_keys;

static bool is_player_faction(const GameState& s, Id faction_id) {
  const auto* f = find_ptr(s.factions, faction_id);
  return f && f->control == FactionControl::Player;
}

static double clamp01(double v) {
  return std::clamp(v, 0.0, 1.0);
}

static std::uint64_t splitmix64(std::uint64_t& x) {
  // Public-domain SplitMix64.
  std::uint64_t z = (x += 0x9e3779b97f4a7c15ULL);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

static double rand01(std::uint64_t& s) {
  // 53-bit mantissa.
  const std::uint64_t v = splitmix64(s) >> 11;
  return static_cast<double>(v) / static_cast<double>(0x1FFFFFFFFFFFFFULL);
}

static int rand_int(std::uint64_t& s, int n) {
  if (n <= 1) return 0;
  const double u = rand01(s);
  const int idx = static_cast<int>(u * static_cast<double>(n));
  return std::clamp(idx, 0, n - 1);
}

struct ContractCandidate {
  ContractKind kind{ContractKind::InvestigateAnomaly};
  Id target_id{kInvalidId};
  Id system_id{kInvalidId};

  // Used for reward/selection heuristics.
  double value{0.0};
};

static std::string contract_kind_label(ContractKind k) {
  switch (k) {
    case ContractKind::InvestigateAnomaly: return "Investigate";
    case ContractKind::SalvageWreck: return "Salvage";
    case ContractKind::SurveyJumpPoint: return "Survey";
  }
  return "Contract";
}

static double anomaly_value_rp(const Anomaly& a) {
  double minerals_total = 0.0;
  for (const auto& [_, t] : a.mineral_reward) minerals_total += std::max(0.0, t);
  double value = std::max(0.0, a.research_reward);
  value += minerals_total * 0.05; // heuristic: 20t ~ 1 RP
  if (!a.unlock_component_id.empty()) value += 25.0;
  return value;
}

static double wreck_value_rp(const Wreck& w) {
  double total = 0.0;
  for (const auto& [_, t] : w.minerals) total += std::max(0.0, t);
  // Wreck cargo isn't research directly, but we can treat it as an opportunity cost.
  // Conservative scaling: 50t -> 1 RP.
  return total * 0.02;
}

static double survey_value_rp(const GameState& s, const Faction& f, const JumpPoint& jp) {
  // Surveying a jump point that leads to an undiscovered system is more valuable.
  double v = 10.0;
  if (jp.linked_jump_id != kInvalidId) {
    if (const auto* other = find_ptr(s.jump_points, jp.linked_jump_id)) {
      const Id dest_sys = other->system_id;
      if (dest_sys != kInvalidId) {
        const bool discovered = std::find(f.discovered_systems.begin(), f.discovered_systems.end(), dest_sys) !=
                                f.discovered_systems.end();
        if (!discovered) v += 20.0;
      }
    }
  }
  return v;
}

} // namespace

// --- Public contract APIs (UI + AI) ---

bool Simulation::accept_contract(Id contract_id, bool push_event, std::string* error) {
  auto fail = [&](const std::string& msg) {
    if (error) *error = msg;
    return false;
  };

  auto it = state_.contracts.find(contract_id);
  if (it == state_.contracts.end()) return fail("Contract not found");
  Contract& c = it->second;
  if (c.status != ContractStatus::Offered) return fail("Contract is not in Offered state");

  const std::int64_t now = state_.date.days_since_epoch();
  c.status = ContractStatus::Accepted;
  c.accepted_day = now;

  if (push_event && is_player_faction(state_, c.assignee_faction_id)) {
    EventContext ctx;
    ctx.faction_id = c.assignee_faction_id;
    ctx.system_id = c.system_id;
    push_event(EventLevel::Info, EventCategory::Exploration, "Contract accepted: " + c.name, ctx);
  }

  return true;
}

bool Simulation::abandon_contract(Id contract_id, bool push_event, std::string* error) {
  auto fail = [&](const std::string& msg) {
    if (error) *error = msg;
    return false;
  };

  auto it = state_.contracts.find(contract_id);
  if (it == state_.contracts.end()) return fail("Contract not found");
  Contract& c = it->second;
  if (c.status != ContractStatus::Accepted && c.status != ContractStatus::Offered) {
    return fail("Only Offered/Accepted contracts can be abandoned");
  }

  const std::int64_t now = state_.date.days_since_epoch();
  c.status = ContractStatus::Failed;
  c.resolved_day = now;
  c.assigned_ship_id = kInvalidId;
  c.assigned_fleet_id = kInvalidId;

  if (push_event && is_player_faction(state_, c.assignee_faction_id)) {
    EventContext ctx;
    ctx.faction_id = c.assignee_faction_id;
    ctx.system_id = c.system_id;
    push_event(EventLevel::Warn, EventCategory::Exploration, "Contract abandoned: " + c.name, ctx);
  }

  return true;
}

bool Simulation::clear_contract_assignment(Id contract_id, std::string* error) {
  auto fail = [&](const std::string& msg) {
    if (error) *error = msg;
    return false;
  };
  auto it = state_.contracts.find(contract_id);
  if (it == state_.contracts.end()) return fail("Contract not found");
  it->second.assigned_ship_id = kInvalidId;
  it->second.assigned_fleet_id = kInvalidId;
  return true;
}

bool Simulation::assign_contract_to_ship(Id contract_id, Id ship_id, bool clear_existing_orders,
                                        bool restrict_to_discovered, bool push_event,
                                        std::string* error) {
  auto fail = [&](const std::string& msg) {
    if (error) *error = msg;
    return false;
  };

  auto it = state_.contracts.find(contract_id);
  if (it == state_.contracts.end()) return fail("Contract not found");
  Contract& c = it->second;

  Ship* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return fail("Ship not found");

  if (c.assignee_faction_id != kInvalidId && ship->faction_id != c.assignee_faction_id) {
    return fail("Ship faction does not match contract assignee");
  }

  if (c.status == ContractStatus::Offered) {
    // Convenience: assigning an offered contract implicitly accepts it.
    std::string err;
    if (!accept_contract(contract_id, push_event, &err)) {
      return fail("Could not accept contract: " + err);
    }
  }
  if (c.status != ContractStatus::Accepted) return fail("Contract is not Accepted");

  // Assignment is a UI convenience only; fleet assignment is not yet wired.
  c.assigned_ship_id = ship_id;
  c.assigned_fleet_id = kInvalidId;

  if (clear_existing_orders) {
    state_.ship_orders[ship_id].queue.clear();
  }

  // Issue the corresponding order.
  bool ok = false;
  switch (c.kind) {
    case ContractKind::InvestigateAnomaly:
      ok = issue_investigate_anomaly(ship_id, c.target_id, restrict_to_discovered);
      break;
    case ContractKind::SalvageWreck:
      ok = issue_salvage_wreck(ship_id, c.target_id, /*mineral=*/"", /*tons=*/0.0, restrict_to_discovered);
      break;
    case ContractKind::SurveyJumpPoint:
      ok = issue_survey_jump_point(ship_id, c.target_id, /*transit_when_done=*/false, restrict_to_discovered);
      break;
  }

  if (!ok) {
    // Roll back assignment if we couldn't issue the order.
    c.assigned_ship_id = kInvalidId;
    return fail("Failed to issue contract orders");
  }

  return true;
}


// --- Daily tick: generate/expire/complete procedural contracts ---

void Simulation::tick_contracts() {
  NEBULA4X_TRACE_SCOPE("tick_contracts", "sim");
  if (!cfg_.enable_contracts) return;

  const std::int64_t now = state_.date.days_since_epoch();

  // 1) Resolve contract completion/expiration.
  for (Id cid : sorted_keys(state_.contracts)) {
    auto it = state_.contracts.find(cid);
    if (it == state_.contracts.end()) continue;
    Contract& c = it->second;

    if (c.status == ContractStatus::Completed || c.status == ContractStatus::Expired || c.status == ContractStatus::Failed) {
      continue;
    }

    // Offered contracts can expire.
    if (c.status == ContractStatus::Offered && c.expires_day > 0 && now >= c.expires_day) {
      c.status = ContractStatus::Expired;
      c.resolved_day = now;
      c.assigned_ship_id = kInvalidId;
      c.assigned_fleet_id = kInvalidId;
      continue;
    }

    if (c.status != ContractStatus::Accepted) continue;

    bool complete = false;
    switch (c.kind) {
      case ContractKind::InvestigateAnomaly: {
        const auto* a = find_ptr(state_.anomalies, c.target_id);
        if (a && a->resolved && (c.assignee_faction_id == kInvalidId || a->resolved_by_faction_id == c.assignee_faction_id)) {
          complete = true;
        }
        break;
      }
      case ContractKind::SalvageWreck: {
        const auto* w = find_ptr(state_.wrecks, c.target_id);
        // Wrecks are erased when their minerals hit zero.
        complete = (w == nullptr);
        break;
      }
      case ContractKind::SurveyJumpPoint: {
        if (c.assignee_faction_id != kInvalidId) {
          complete = is_jump_point_surveyed_by_faction(c.assignee_faction_id, c.target_id);
        }
        break;
      }
    }

    if (!complete) continue;

    c.status = ContractStatus::Completed;
    c.resolved_day = now;

    // Award research points.
    if (c.assignee_faction_id != kInvalidId) {
      if (auto* f = find_ptr(state_.factions, c.assignee_faction_id)) {
        f->research_points += std::max(0.0, c.reward_research_points);
      }
    }

    // Clear assignment to avoid dangling UI pointers.
    c.assigned_ship_id = kInvalidId;
    c.assigned_fleet_id = kInvalidId;

    if (is_player_faction(state_, c.assignee_faction_id)) {
      EventContext ctx;
      ctx.faction_id = c.assignee_faction_id;
      ctx.system_id = c.system_id;
      push_event(EventLevel::Info, EventCategory::Exploration,
                 "Contract completed: " + c.name + " (+" + std::to_string(static_cast<int>(std::round(c.reward_research_points))) + " RP)",
                 ctx);

      JournalEntry je;
      je.day = now;
      je.hour = state_.hour_of_day;
      je.category = EventCategory::Exploration;
      je.title = "Contract Completed";
      je.text = c.name;
      je.system_id = c.system_id;
      push_journal_entry(c.assignee_faction_id, std::move(je));
    }
  }

  // 2) Generate new offers (per faction).
  const int max_offers = std::clamp(cfg_.contract_max_offers_per_faction, 0, 64);
  const int daily_new = std::clamp(cfg_.contract_daily_new_offers_per_faction, 0, 64);
  if (max_offers <= 0 || daily_new <= 0) return;

  // Sorted for determinism.
  const std::vector<Id> faction_ids = sorted_keys(state_.factions);

  for (Id fid : faction_ids) {
    const auto* fac = find_ptr(state_.factions, fid);
    if (!fac) continue;

    int offered_count = 0;
    std::unordered_set<Id> used_anoms;
    std::unordered_set<Id> used_wrecks;
    std::unordered_set<Id> used_jumps;
    used_anoms.reserve(64);
    used_wrecks.reserve(64);
    used_jumps.reserve(64);

    for (const auto& [_, c] : state_.contracts) {
      if (c.assignee_faction_id != fid) continue;
      if (c.status == ContractStatus::Offered) offered_count++;
      if (c.status == ContractStatus::Offered || c.status == ContractStatus::Accepted) {
        if (c.target_id == kInvalidId) continue;
        switch (c.kind) {
          case ContractKind::InvestigateAnomaly: used_anoms.insert(c.target_id); break;
          case ContractKind::SalvageWreck: used_wrecks.insert(c.target_id); break;
          case ContractKind::SurveyJumpPoint: used_jumps.insert(c.target_id); break;
        }
      }
    }

    if (offered_count >= max_offers) continue;
    const int want = std::min(daily_new, max_offers - offered_count);
    if (want <= 0) continue;

    // Build candidate lists.
    std::vector<ContractCandidate> anom;
    std::vector<ContractCandidate> wreck;
    std::vector<ContractCandidate> jump;
    anom.reserve(64);
    wreck.reserve(64);
    jump.reserve(64);

    // Anomalies (discovered but unresolved).
    for (Id aid : fac->discovered_anomalies) {
      if (used_anoms.contains(aid)) continue;
      const auto* a = find_ptr(state_.anomalies, aid);
      if (!a) continue;
      if (a->resolved) continue;
      if (a->system_id == kInvalidId) continue;
      if (!find_ptr(state_.systems, a->system_id)) continue;
      ContractCandidate c;
      c.kind = ContractKind::InvestigateAnomaly;
      c.target_id = aid;
      c.system_id = a->system_id;
      c.value = anomaly_value_rp(*a);
      anom.push_back(std::move(c));
    }

    // Wrecks (in discovered systems).
    for (Id wid : sorted_keys(state_.wrecks)) {
      if (used_wrecks.contains(wid)) continue;
      const auto* w = find_ptr(state_.wrecks, wid);
      if (!w) continue;
      if (w->system_id == kInvalidId) continue;
      if (!is_system_discovered_by_faction(fid, w->system_id)) continue;
      if (!find_ptr(state_.systems, w->system_id)) continue;
      if (w->minerals.empty()) continue;
      ContractCandidate c;
      c.kind = ContractKind::SalvageWreck;
      c.target_id = wid;
      c.system_id = w->system_id;
      c.value = wreck_value_rp(*w);
      wreck.push_back(std::move(c));
    }

    // Jump points (unsurveyed, in discovered systems).
    for (Id sys_id : fac->discovered_systems) {
      const auto* sys = find_ptr(state_.systems, sys_id);
      if (!sys) continue;
      for (Id jid : sys->jump_points) {
        if (jid == kInvalidId) continue;
        if (used_jumps.contains(jid)) continue;
        if (is_jump_point_surveyed_by_faction(fid, jid)) continue;
        const auto* jp = find_ptr(state_.jump_points, jid);
        if (!jp) continue;
        ContractCandidate c;
        c.kind = ContractKind::SurveyJumpPoint;
        c.target_id = jid;
        c.system_id = sys_id;
        c.value = survey_value_rp(state_, *fac, *jp);
        jump.push_back(std::move(c));
      }
    }

    if (anom.empty() && wreck.empty() && jump.empty()) continue;

    // Determine a home system/position for hop estimation.
    Id home_sys = kInvalidId;
    Vec2 home_pos{0.0, 0.0};
    for (Id cid : sorted_keys(state_.colonies)) {
      const auto* col = find_ptr(state_.colonies, cid);
      if (!col) continue;
      if (col->faction_id != fid) continue;
      const auto* b = find_ptr(state_.bodies, col->body_id);
      if (!b) continue;
      if (b->system_id == kInvalidId) continue;
      home_sys = b->system_id;
      home_pos = b->position_mkm;
      break;
    }
    if (home_sys == kInvalidId) {
      for (Id sid : sorted_keys(state_.ships)) {
        const auto* sh = find_ptr(state_.ships, sid);
        if (!sh) continue;
        if (sh->faction_id != fid) continue;
        if (sh->system_id == kInvalidId) continue;
        home_sys = sh->system_id;
        home_pos = sh->position_mkm;
        break;
      }
    }
    if (home_sys == kInvalidId && !fac->discovered_systems.empty()) {
      home_sys = fac->discovered_systems.front();
      home_pos = Vec2{0.0, 0.0};
    }

    // Deterministic per-faction-per-day RNG seed.
    std::uint64_t rng = 0xC0FFEEULL;
    rng ^= static_cast<std::uint64_t>(now) * 0x9e3779b97f4a7c15ULL;
    rng ^= static_cast<std::uint64_t>(fid) * 0xbf58476d1ce4e5b9ULL;

    auto pick_from = [&](std::vector<ContractCandidate>& v) -> std::optional<ContractCandidate> {
      if (v.empty()) return std::nullopt;
      // Bias toward higher value while still providing variety.
      int best_i = 0;
      double best_score = -std::numeric_limits<double>::infinity();
      for (int i = 0; i < static_cast<int>(v.size()); ++i) {
        const double score = v[i].value + rand01(rng) * 0.5;
        if (score > best_score) {
          best_score = score;
          best_i = i;
        }
      }
      ContractCandidate out = v[best_i];
      v.erase(v.begin() + best_i);
      return out;
    };

    for (int i = 0; i < want; ++i) {
      // Pick a kind with simple weights.
      struct Bucket { int w; ContractKind kind; };
      std::vector<Bucket> buckets;
      buckets.reserve(3);
      if (!anom.empty()) buckets.push_back({3, ContractKind::InvestigateAnomaly});
      if (!jump.empty()) buckets.push_back({2, ContractKind::SurveyJumpPoint});
      if (!wreck.empty()) buckets.push_back({1, ContractKind::SalvageWreck});
      if (buckets.empty()) break;

      int total_w = 0;
      for (const auto& b : buckets) total_w += b.w;
      int r = rand_int(rng, std::max(1, total_w));
      ContractKind chosen = buckets.front().kind;
      for (const auto& b : buckets) {
        if (r < b.w) {
          chosen = b.kind;
          break;
        }
        r -= b.w;
      }

      std::optional<ContractCandidate> cand;
      if (chosen == ContractKind::InvestigateAnomaly) cand = pick_from(anom);
      else if (chosen == ContractKind::SurveyJumpPoint) cand = pick_from(jump);
      else cand = pick_from(wreck);
      if (!cand) break;

      Contract c;
      c.id = allocate_id(state_);
      c.kind = cand->kind;
      c.status = ContractStatus::Offered;
      c.issuer_faction_id = fid;
      c.assignee_faction_id = fid;
      c.system_id = cand->system_id;
      c.target_id = cand->target_id;
      c.offered_day = now;
      if (cfg_.contract_offer_expiry_days > 0) {
        c.expires_day = now + static_cast<std::int64_t>(cfg_.contract_offer_expiry_days);
      } else {
        c.expires_day = 0;
      }

      // Hops estimate (route) and risk estimate (environment).
      c.hops_estimate = 0;
      if (home_sys != kInvalidId && c.system_id != kInvalidId) {
        const auto plan = plan_jump_route_cached(home_sys, home_pos, fid, /*speed*/1.0, c.system_id,
                                                /*restrict_to_discovered=*/true);
        if (plan) c.hops_estimate = static_cast<int>(plan->jump_ids.size());
      }

      c.risk_estimate = 0.0;
      if (const auto* sys = find_ptr(state_.systems, c.system_id)) {
        double risk = 0.0;
        if (sys->region_id != kInvalidId) {
          if (const auto* reg = find_ptr(state_.regions, sys->region_id)) {
            const double pr = clamp01(reg->pirate_risk) * (1.0 - clamp01(reg->pirate_suppression));
            risk += pr * 0.6;
          }
        }
        risk += clamp01(sys->nebula_density) * 0.2;
        risk += clamp01(system_storm_intensity(sys->id)) * 0.2;

        // Anomaly hazard adds extra risk.
        if (c.kind == ContractKind::InvestigateAnomaly) {
          if (const auto* a = find_ptr(state_.anomalies, c.target_id)) {
            const double hz = clamp01(a->hazard_chance) * std::max(0.0, a->hazard_damage);
            const double hz01 = 1.0 - std::exp(-hz / 20.0);
            risk = std::max(risk, clamp01(hz01));
          }
        }

        c.risk_estimate = clamp01(risk);
      }

      // Reward heuristic: value + distance + risk.
      c.reward_research_points = std::max(0.0, cand->value) + std::max(0.0, cfg_.contract_reward_base_rp);
      c.reward_research_points += static_cast<double>(std::max(0, c.hops_estimate)) *
                                  std::max(0.0, cfg_.contract_reward_rp_per_hop);
      c.reward_research_points += c.risk_estimate * std::max(0.0, cfg_.contract_reward_rp_per_risk);

      // Name.
      std::string target_name;
      if (c.kind == ContractKind::InvestigateAnomaly) {
        if (const auto* a = find_ptr(state_.anomalies, c.target_id)) {
          target_name = !a->name.empty() ? a->name : ("Anomaly " + std::to_string(static_cast<std::uint64_t>(a->id)));
        }
      } else if (c.kind == ContractKind::SalvageWreck) {
        if (const auto* w = find_ptr(state_.wrecks, c.target_id)) {
          target_name = !w->name.empty() ? w->name : ("Wreck " + std::to_string(static_cast<std::uint64_t>(w->id)));
        }
      } else if (c.kind == ContractKind::SurveyJumpPoint) {
        if (const auto* jp = find_ptr(state_.jump_points, c.target_id)) {
          target_name = !jp->name.empty() ? jp->name
                                          : ("Jump " + std::to_string(static_cast<std::uint64_t>(jp->id)));
        }
      }
      if (target_name.empty()) target_name = "Target " + std::to_string(static_cast<std::uint64_t>(c.target_id));

      c.name = contract_kind_label(c.kind) + ": " + target_name;

      state_.contracts[c.id] = std::move(c);
    }
  }
}

} // namespace nebula4x
