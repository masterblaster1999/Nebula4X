#include "nebula4x/core/simulation.h"

#include "simulation_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "nebula4x/util/log.h"
#include "nebula4x/util/trace_events.h"
#include "nebula4x/util/hash_rng.h"

namespace nebula4x {
namespace {
using sim_internal::sorted_keys;
using sim_internal::stable_sum_nonneg_sorted_ld;

static bool is_player_faction(const GameState& s, Id faction_id) {
  const auto* f = find_ptr(s.factions, faction_id);
  return f && f->control == FactionControl::Player;
}

static double clamp01(double v) {
  return std::clamp(v, 0.0, 1.0);
}

static double rand01(std::uint64_t& s) {
  const std::uint64_t v = ::nebula4x::util::next_splitmix64(s);
  return ::nebula4x::util::u01_from_u64(v);
}

static int rand_int(std::uint64_t& s, int n) {
  if (n <= 1) return 0;
  return static_cast<int>(::nebula4x::util::bounded_u64(s, static_cast<std::uint64_t>(n)));
}


static const char* diplomacy_status_label(DiplomacyStatus s) {
  switch (s) {
    case DiplomacyStatus::Friendly: return "Friendly";
    case DiplomacyStatus::Neutral: return "Neutral";
    case DiplomacyStatus::Hostile: return "Hostile";
  }
  return "(Unknown)";
}

static std::string faction_label(const GameState& st, Id fid) {
  if (fid == kInvalidId) return "(None)";
  if (const auto* f = find_ptr(st.factions, fid)) {
    if (!f->name.empty()) return f->name;
  }
  return "Faction " + std::to_string(static_cast<unsigned long long>(fid));
}

// Applies a small directed diplomacy adjustment between a contract issuer and
// assignee on success/failure.
// - Success: Hostile→Neutral→Friendly (issuer toward assignee)
// - Failure/abandon (only if accepted): Friendly→Neutral
//
// Returns a short, user-facing note if a change was applied.
static std::string maybe_apply_contract_diplomacy_delta(Simulation& sim, Contract& c, bool success, bool was_accepted) {
  if (!success && !was_accepted) return {};
  if (c.issuer_faction_id == kInvalidId || c.assignee_faction_id == kInvalidId) return {};
  if (c.issuer_faction_id == c.assignee_faction_id) return {};

  auto& st = sim.state();
  const auto* issuer = find_ptr(st.factions, c.issuer_faction_id);
  const auto* assignee = find_ptr(st.factions, c.assignee_faction_id);
  if (!issuer || !assignee) return {};
  if (issuer->control == FactionControl::AI_Pirate) return {};

  const DiplomacyStatus base = sim.diplomatic_status_base(c.issuer_faction_id, c.assignee_faction_id);
  DiplomacyStatus next = base;

  if (success) {
    if (base == DiplomacyStatus::Hostile) next = DiplomacyStatus::Neutral;
    else if (base == DiplomacyStatus::Neutral) next = DiplomacyStatus::Friendly;
    else return {};
  } else {
    if (base == DiplomacyStatus::Friendly) next = DiplomacyStatus::Neutral;
    else return {};
  }

  if (next == base) return {};

  sim.set_diplomatic_status(c.issuer_faction_id, c.assignee_faction_id, next,
                            /*reciprocal=*/false, /*push_event_on_change=*/false);

  std::string note = "Diplomacy: " + faction_label(st, c.issuer_faction_id) + " is now ";
  note += diplomacy_status_label(next);
  note += " toward ";
  note += faction_label(st, c.assignee_faction_id);
  return note;
}


struct ContractCandidate {
  ContractKind kind{ContractKind::InvestigateAnomaly};
  Id target_id{kInvalidId};
  Id system_id{kInvalidId};

  // Optional secondary target (kind-specific).
  Id target_id2{kInvalidId};

  // For EscortConvoy: estimated number of jumps in the escorted leg.
  int leg_hops{0};

  // Estimated route/target risk in [0,1] used for reward/selection heuristics.
  double risk{0.0};

  // Used for reward/selection heuristics.
  double value{0.0};
};

static std::string contract_kind_label(ContractKind k) {
  switch (k) {
    case ContractKind::InvestigateAnomaly: return "Investigate";
    case ContractKind::SalvageWreck: return "Salvage";
    case ContractKind::SurveyJumpPoint: return "Survey";
    case ContractKind::BountyPirate: return "Bounty";
    case ContractKind::EscortConvoy: return "Escort";
  }
  return "Contract";
}

static double anomaly_value_rp(const Anomaly& a) {
  const double minerals_total = static_cast<double>(stable_sum_nonneg_sorted_ld(a.mineral_reward));
  double value = std::max(0.0, a.research_reward);
  value += minerals_total * 0.05; // heuristic: 20t ~ 1 RP
  if (!a.unlock_component_id.empty()) value += 25.0;
  return value;
}

static double wreck_value_rp(const Wreck& w) {
  const double total = static_cast<double>(stable_sum_nonneg_sorted_ld(w.minerals));
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
  if (c.expires_day > 0 && now >= c.expires_day) return fail("Contract offer has expired");

  // Kind-specific staleness validation.
  if (c.kind == ContractKind::BountyPirate) {
    if (c.target_destroyed_day != 0) return fail("Bounty target already destroyed");
    const auto* sh = find_ptr(state_.ships, c.target_id);
    if (!sh || sh->hp <= 0.0) return fail("Bounty target missing");
  }
  c.status = ContractStatus::Accepted;
  c.accepted_day = now;

  if (push_event && is_player_faction(state_, c.assignee_faction_id)) {
    EventContext ctx;
    ctx.faction_id = c.assignee_faction_id;
    ctx.system_id = c.system_id;
    std::string msg = "Contract accepted: " + c.name;
    if (c.issuer_faction_id != kInvalidId && c.issuer_faction_id != c.assignee_faction_id) {
      msg += " (Issuer: " + faction_label(state_, c.issuer_faction_id) + ")";
    }
    this->push_event(EventLevel::Info, EventCategory::Exploration, msg, ctx);
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

  const bool was_accepted = (c.status == ContractStatus::Accepted);
  const std::int64_t now = state_.date.days_since_epoch();
  c.status = ContractStatus::Failed;
  c.resolved_day = now;
  c.assigned_ship_id = kInvalidId;
  c.assigned_fleet_id = kInvalidId;

  const std::string dip_note = maybe_apply_contract_diplomacy_delta(*this, c, /*success=*/false, was_accepted);

  if (push_event && is_player_faction(state_, c.assignee_faction_id)) {
    EventContext ctx;
    ctx.faction_id = c.assignee_faction_id;
    ctx.system_id = c.system_id;
    std::string msg = "Contract abandoned: " + c.name;
    if (c.issuer_faction_id != kInvalidId && c.issuer_faction_id != c.assignee_faction_id) {
      msg += " (Issuer: " + faction_label(state_, c.issuer_faction_id) + ")";
    }
    if (!dip_note.empty()) {
      msg += " [" + dip_note + "]";
    }
    this->push_event(EventLevel::Warn, EventCategory::Exploration, msg, ctx);
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

  // Assignment is a UI convenience only.
  c.assigned_ship_id = ship_id;
  c.assigned_fleet_id = kInvalidId;

  if (clear_existing_orders) {
    // Use the canonical helper so we also disable order repeating.
    (void)clear_orders(ship_id);
  }

  // Issue the corresponding order.
  bool ok = false;
  switch (c.kind) {
    case ContractKind::InvestigateAnomaly:
      ok = issue_investigate_anomaly(ship_id, c.target_id, restrict_to_discovered);
      break;
    case ContractKind::SalvageWreck:
      ok = issue_salvage_wreck_loop(ship_id, c.target_id, /*dropoff_colony_id=*/kInvalidId, restrict_to_discovered);
      break;
    case ContractKind::SurveyJumpPoint:
      ok = issue_survey_jump_point(ship_id, c.target_id, /*transit_when_done=*/false, restrict_to_discovered);
      break;
    case ContractKind::BountyPirate:
      // Attack orders use fog-of-war contact prediction to pursue the target.
      ok = issue_attack_ship(ship_id, c.target_id, restrict_to_discovered);
      break;
    case ContractKind::EscortConvoy:
      // Escort neutral convoys is allowed (non-hostile factions) by setting
      // allow_neutral. The order itself handles cross-system routing.
      ok = issue_escort_ship(ship_id, c.target_id, /*follow_distance_mkm=*/1.0,
                             restrict_to_discovered, /*allow_neutral=*/true);
      break;
  }

  if (!ok) {
    // Roll back assignment if we couldn't issue the order.
    c.assigned_ship_id = kInvalidId;
    return fail("Failed to issue contract orders");
  }

  return true;
}

bool Simulation::assign_contract_to_fleet(Id contract_id, Id fleet_id, bool clear_existing_orders,
                                         bool restrict_to_discovered, bool push_event,
                                         std::string* error) {
  auto fail = [&](const std::string& msg) {
    if (error) *error = msg;
    return false;
  };

  // Keep fleet invariants consistent (valid members, leader, no duplicates).
  prune_fleets();

  auto it = state_.contracts.find(contract_id);
  if (it == state_.contracts.end()) return fail("Contract not found");
  Contract& c = it->second;

  Fleet* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return fail("Fleet not found");
  if (fl->ship_ids.empty()) return fail("Fleet has no ships");

  if (c.assignee_faction_id != kInvalidId && fl->faction_id != c.assignee_faction_id) {
    return fail("Fleet faction does not match contract assignee");
  }

  if (c.status == ContractStatus::Offered) {
    // Convenience: assigning an offered contract implicitly accepts it.
    std::string err;
    if (!accept_contract(contract_id, push_event, &err)) {
      return fail("Could not accept contract: " + err);
    }
  }
  if (c.status != ContractStatus::Accepted) return fail("Contract is not Accepted");

  // Choose a primary ship (used for UI focus + as the contract executor).
  // Prefer the fleet leader if it can execute the contract; otherwise select
  // the best candidate based on simple capability heuristics.
  auto can_execute = [&](Id sid) -> bool {
    const Ship* sh = find_ptr(state_.ships, sid);
    if (!sh) return false;
    if (c.assignee_faction_id != kInvalidId && sh->faction_id != c.assignee_faction_id) return false;
    if (c.kind == ContractKind::InvestigateAnomaly) {
      const auto* d = find_design(sh->design_id);
      const double sensor = d ? std::max(0.0, d->sensor_range_mkm) : 0.0;
      return sensor > 1e-9;
    }
    if (c.kind == ContractKind::BountyPirate) {
      const auto* d = find_design(sh->design_id);
      const double weapons = d ? (std::max(0.0, d->weapon_damage) + std::max(0.0, d->missile_damage)) : 0.0;
      return weapons > 1e-9;
    }
    // Salvage + Survey + Escort have no hard capability gates (cargo helps salvage throughput).
    return true;
  };

  auto score_ship = [&](Id sid) -> double {
    const Ship* sh = find_ptr(state_.ships, sid);
    if (!sh) return -1e300;
    const auto* d = find_design(sh->design_id);
    const double sp = std::max(0.0, sh->speed_km_s);

    double cap = 1.0;
    if (c.kind == ContractKind::InvestigateAnomaly) {
      const double sensor = d ? std::max(0.0, d->sensor_range_mkm) : 0.0;
      cap = 1.0 + sensor;
    } else if (c.kind == ContractKind::SalvageWreck) {
      const double cargo = d ? std::max(0.0, d->cargo_tons) : 0.0;
      cap = 1.0 + cargo;
    } else if (c.kind == ContractKind::SurveyJumpPoint) {
      cap = 1.0;
    } else if (c.kind == ContractKind::BountyPirate) {
      // Bounties reward combat power; speed still matters for pursuit.
      const double weap = d ? (std::max(0.0, d->weapon_damage) + std::max(0.0, d->missile_damage)) : 0.0;
      const double rng = d ? (std::max(0.0, d->weapon_range_mkm) + std::max(0.0, d->missile_range_mkm)) : 0.0;
      cap = 1.0 + 50.0 * weap + 0.5 * rng;
    } else if (c.kind == ContractKind::EscortConvoy) {
      // Escort is primarily a mobility task; speed dominates.
      cap = 1.0;
    }

    // Speed is a smaller term; capability dominates.
    return cap * 1000.0 + sp;
  };

  Id primary_ship_id = kInvalidId;
  if (fl->leader_ship_id != kInvalidId && can_execute(fl->leader_ship_id)) {
    primary_ship_id = fl->leader_ship_id;
  } else {
    double best = -1e300;
    for (Id sid : fl->ship_ids) {
      if (sid == kInvalidId) continue;
      if (!can_execute(sid)) continue;
      const double sc = score_ship(sid);
      if (primary_ship_id == kInvalidId || sc > best + 1e-9 || (std::abs(sc - best) <= 1e-9 && sid < primary_ship_id)) {
        primary_ship_id = sid;
        best = sc;
      }
    }
  }
  if (primary_ship_id == kInvalidId) return fail("No suitable fleet ship can execute this contract");

  // Assignment is a UI convenience only.
  c.assigned_ship_id = primary_ship_id;
  c.assigned_fleet_id = fleet_id;

  if (clear_existing_orders) {
    // Clear for the whole fleet so escorts participate immediately.
    (void)clear_fleet_orders(fleet_id);
  }

  // Issue the corresponding order to the primary ship.
  bool ok = false;
  switch (c.kind) {
    case ContractKind::InvestigateAnomaly:
      ok = issue_investigate_anomaly(primary_ship_id, c.target_id, restrict_to_discovered);
      break;
    case ContractKind::SalvageWreck:
      ok = issue_salvage_wreck_loop(primary_ship_id, c.target_id, /*dropoff_colony_id=*/kInvalidId, restrict_to_discovered);
      break;
    case ContractKind::SurveyJumpPoint:
      ok = issue_survey_jump_point(primary_ship_id, c.target_id, /*transit_when_done=*/false, restrict_to_discovered);
      break;
    case ContractKind::BountyPirate:
      ok = issue_attack_ship(primary_ship_id, c.target_id, restrict_to_discovered);
      break;
    case ContractKind::EscortConvoy:
      ok = issue_escort_ship(primary_ship_id, c.target_id, /*follow_distance_mkm=*/1.0,
                             restrict_to_discovered, /*allow_neutral=*/true);
      break;
  }

  if (!ok) {
    // Roll back assignment if we couldn't issue the order.
    c.assigned_ship_id = kInvalidId;
    c.assigned_fleet_id = kInvalidId;
    return fail("Failed to issue contract orders");
  }

  // Issue escort orders to the rest of the fleet.
  // This keeps the fleet moving as a group without duplicating the contract action.
  const double follow_mkm = std::max(0.0, fl->formation_spacing_mkm);
  const double follow = (follow_mkm > 1e-9) ? follow_mkm : 1.0;
  for (Id sid : fl->ship_ids) {
    if (sid == kInvalidId || sid == primary_ship_id) continue;
    (void)issue_escort_ship(sid, primary_ship_id, follow, restrict_to_discovered);
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

    auto mark_expired = [&](std::string reason) {
      c.status = ContractStatus::Expired;
      c.resolved_day = now;
      c.assigned_ship_id = kInvalidId;
      c.assigned_fleet_id = kInvalidId;

      if (is_player_faction(state_, c.assignee_faction_id)) {
        EventContext ctx;
        ctx.faction_id = c.assignee_faction_id;
        ctx.system_id = c.system_id;

        std::string msg = "Contract expired: " + c.name;
        if (!reason.empty()) msg += " (" + reason + ")";
        if (c.issuer_faction_id != kInvalidId && c.issuer_faction_id != c.assignee_faction_id) {
          msg += " (Issuer: " + faction_label(state_, c.issuer_faction_id) + ")";
        }

        push_event(EventLevel::Warn, EventCategory::Exploration, msg, ctx);

        JournalEntry je;
        je.day = now;
        je.hour = state_.hour_of_day;
        je.category = EventCategory::Exploration;
        je.title = "Contract Expired";
        je.text = msg;
        je.system_id = c.system_id;
        push_journal_entry(c.assignee_faction_id, std::move(je));
      }
    };

    auto mark_failed = [&](std::string reason) {
      const bool was_accepted = (c.status == ContractStatus::Accepted);
      c.status = ContractStatus::Failed;
      c.resolved_day = now;
      c.assigned_ship_id = kInvalidId;
      c.assigned_fleet_id = kInvalidId;

      const std::string dip_note = maybe_apply_contract_diplomacy_delta(*this, c, /*success=*/false, was_accepted);

      if (is_player_faction(state_, c.assignee_faction_id)) {
        EventContext ctx;
        ctx.faction_id = c.assignee_faction_id;
        ctx.system_id = c.system_id;

        std::string msg = "Contract failed: " + c.name;
        if (!reason.empty()) msg += " (" + reason + ")";
        if (c.issuer_faction_id != kInvalidId && c.issuer_faction_id != c.assignee_faction_id) {
          msg += " (Issuer: " + faction_label(state_, c.issuer_faction_id) + ")";
        }
        if (!dip_note.empty()) {
          msg += " [" + dip_note + "]";
        }

        push_event(EventLevel::Warn, EventCategory::Exploration, msg, ctx);

        JournalEntry je;
        je.day = now;
        je.hour = state_.hour_of_day;
        je.category = EventCategory::Exploration;
        je.title = "Contract Failed";
        je.text = msg;
        je.system_id = c.system_id;
        push_journal_entry(c.assignee_faction_id, std::move(je));
      }
    };

    // Target validity checks (stale offers/accepted contracts).
    // This primarily matters for anomalies, which become impossible to complete
    // after another faction resolves them.
    if (c.kind == ContractKind::InvestigateAnomaly) {
      const auto* a = find_ptr(state_.anomalies, c.target_id);
      if (!a) {
        if (c.status == ContractStatus::Offered) {
          mark_expired("target missing");
        } else if (c.status == ContractStatus::Accepted) {
          mark_failed("target missing");
        }
        continue;
      }
      if (a->resolved) {
        if (c.status == ContractStatus::Offered) {
          mark_expired("target already resolved");
          continue;
        }
        if (c.status == ContractStatus::Accepted && c.assignee_faction_id != kInvalidId &&
            a->resolved_by_faction_id != kInvalidId && a->resolved_by_faction_id != c.assignee_faction_id) {
          mark_failed("resolved by another faction");
          continue;
        }
      }
    } else if (c.kind == ContractKind::SalvageWreck) {
      const auto* w = find_ptr(state_.wrecks, c.target_id);
      if ((!w || w->minerals.empty()) && c.status == ContractStatus::Offered) {
        mark_expired("wreck already salvaged");
        continue;
      }
    } else if (c.kind == ContractKind::SurveyJumpPoint) {
      const auto* jp = find_ptr(state_.jump_points, c.target_id);
      if (!jp) {
        if (c.status == ContractStatus::Offered) {
          mark_expired("target missing");
        } else if (c.status == ContractStatus::Accepted) {
          mark_failed("target missing");
        }
        continue;
      }
    } else if (c.kind == ContractKind::BountyPirate) {
      // Bounties target ships; the target may be missing from the world once it
      // is destroyed, so we treat target_destroyed_day as the authoritative flag
      // for whether the contract can still be resolved.
      if (c.target_destroyed_day != 0) {
        if (c.status == ContractStatus::Offered) {
          mark_expired("target already destroyed");
          continue;
        }
        // Accepted contracts resolve in the completion logic below.
      } else {
        const auto* sh = find_ptr(state_.ships, c.target_id);
        if (!sh || sh->hp <= 0.0) {
          if (c.status == ContractStatus::Offered) {
            mark_expired("target missing");
          } else if (c.status == ContractStatus::Accepted) {
            mark_failed("target missing");
          }
          continue;
        }
      }
    } else if (c.kind == ContractKind::EscortConvoy) {
      const auto* sh = find_ptr(state_.ships, c.target_id);
      if (!sh || sh->hp <= 0.0) {
        if (c.status == ContractStatus::Offered) {
          mark_expired("target missing");
        } else if (c.status == ContractStatus::Accepted) {
          mark_failed("target missing");
        }
        continue;
      }
      if (c.target_id2 == kInvalidId) {
        if (c.status == ContractStatus::Offered) {
          mark_expired("bad destination");
        } else if (c.status == ContractStatus::Accepted) {
          mark_failed("bad destination");
        }
        continue;
      }

      // If the convoy already arrived before the player even accepted, the
      // offer is stale.
      if (c.status == ContractStatus::Offered && sh->system_id == c.target_id2) {
        mark_expired("convoy already arrived");
        continue;
      }
    }

    // Offered contracts can expire.
    if (c.status == ContractStatus::Offered && c.expires_day > 0 && now >= c.expires_day) {
      mark_expired("offer expired");
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
        complete = (w == nullptr) || (w && w->minerals.empty());
        break;
      }
      case ContractKind::SurveyJumpPoint: {
        if (c.assignee_faction_id != kInvalidId) {
          complete = is_jump_point_surveyed_by_faction(c.assignee_faction_id, c.target_id);
        }
        break;
      }
      case ContractKind::BountyPirate: {
        if (c.assignee_faction_id == kInvalidId) break;
        if (c.target_destroyed_day == 0) break;

        // Prevent accepting a bounty after it was already destroyed (stale UI
        // races); the offer should have expired.
        if (c.accepted_day > 0 && c.accepted_day > c.target_destroyed_day) {
          mark_failed("target destroyed before acceptance");
          break;
        }

        if (c.target_destroyed_by_faction_id == c.assignee_faction_id) {
          complete = true;
        } else {
          std::string reason = "target destroyed by another faction";
          if (c.target_destroyed_by_faction_id == kInvalidId) {
            reason = "target destroyed (unknown attacker)";
          } else if (const auto* f = find_ptr(state_.factions, c.target_destroyed_by_faction_id)) {
            reason = "target destroyed by " + f->name;
          }
          mark_failed(std::move(reason));
        }
        break;
      }
      case ContractKind::EscortConvoy: {
        const auto* convoy = find_ptr(state_.ships, c.target_id);
        if (!convoy || convoy->hp <= 0.0) break;
        if (c.target_id2 == kInvalidId) break;
        if (convoy->system_id != c.target_id2) break; // not at destination yet

        if (c.assignee_faction_id == kInvalidId) break;

        // Completion requires at least one assignee ship to be physically near
        // the convoy at the time it arrives at its destination.
        constexpr double kEscortCompleteRadiusMkm = 5.0;
        bool escorted = false;
        for (const auto& [sid, sh] : state_.ships) {
          (void)sid;
          if (sh.hp <= 0.0) continue;
          if (sh.faction_id != c.assignee_faction_id) continue;
          if (sh.system_id != convoy->system_id) continue;

          // Require an active escort order targeting this convoy to prevent
          // "coincidental" proximity at the destination.
          bool has_order = false;
          if (auto itso = state_.ship_orders.find(sh.id); itso != state_.ship_orders.end()) {
            for (const auto& ord : itso->second.queue) {
              if (std::holds_alternative<EscortShip>(ord)) {
                const auto& e = std::get<EscortShip>(ord);
                if (e.target_ship_id == c.target_id) {
                  has_order = true;
                  break;
                }
              }
            }
          }
          if (!has_order) continue;

          const Vec2 d = sh.position_mkm - convoy->position_mkm;
          const double dist = std::sqrt(d.x * d.x + d.y * d.y);
          if (dist <= kEscortCompleteRadiusMkm + 1e-9) {
            escorted = true;
            break;
          }
        }

        if (escorted) {
          complete = true;
        } else {
          // The convoy reached its destination without an escort present.
          mark_failed("convoy arrived without escort");
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

    // Bounty contracts also provide a small security benefit: destroying pirate
    // assets reduces local pirate effectiveness.
    if (c.kind == ContractKind::BountyPirate && cfg_.enable_pirate_suppression) {
      // Prefer the recorded kill system (target_id2) if available.
      const Id sys_id = (c.target_id2 != kInvalidId) ? c.target_id2 : c.system_id;
      const auto* sys = find_ptr(state_.systems, sys_id);
      const Id rid = sys ? sys->region_id : kInvalidId;
      if (rid != kInvalidId) {
        if (auto* reg = find_ptr(state_.regions, rid)) {
          const double boost = std::clamp(0.02 + 0.06 * clamp01(c.risk_estimate), 0.0, 0.08);
          reg->pirate_suppression = std::clamp(reg->pirate_suppression + boost, 0.0, 1.0);
        }
      }
    }

    // Escort contracts provide a small local security benefit: successful
    // escorts slightly improve pirate suppression along the escorted corridor.
    if (c.kind == ContractKind::EscortConvoy && cfg_.enable_pirate_suppression) {
      const double boost = std::clamp(0.02 + 0.05 * clamp01(c.risk_estimate), 0.0, 0.05);
      if (boost > 1e-12) {
        std::vector<Id> route_systems;
        route_systems.reserve(16);

        const Id start_sys = c.system_id;
        const Id dest_sys = c.target_id2;

        if (start_sys != kInvalidId && dest_sys != kInvalidId && start_sys != dest_sys &&
            c.assignee_faction_id != kInvalidId) {
          // Compute a representative route (jump ids) and expand to systems.
          const auto plan = plan_jump_route_cached(start_sys, Vec2{0.0, 0.0}, c.assignee_faction_id,
                                                  /*speed*/1.0, dest_sys,
                                                  /*restrict_to_discovered=*/true);
          route_systems.push_back(start_sys);
          Id cur_sys = start_sys;
          if (plan) {
            for (Id jid : plan->jump_ids) {
              const auto* jp = find_ptr(state_.jump_points, jid);
              if (!jp || jp->system_id != cur_sys || jp->linked_jump_id == kInvalidId) break;
              const auto* other = find_ptr(state_.jump_points, jp->linked_jump_id);
              if (!other || other->system_id == kInvalidId) break;
              cur_sys = other->system_id;
              route_systems.push_back(cur_sys);
            }
          }
        }

        if (route_systems.empty() && c.target_id2 != kInvalidId) {
          route_systems.push_back(c.target_id2);
        }

        std::unordered_set<Id> touched_regions;
        touched_regions.reserve(route_systems.size() * 2 + 4);
        for (Id sys_id : route_systems) {
          const auto* sys = find_ptr(state_.systems, sys_id);
          if (!sys) continue;
          const Id rid = sys->region_id;
          if (rid == kInvalidId) continue;
          if (!touched_regions.insert(rid).second) continue;
          if (auto* reg = find_ptr(state_.regions, rid)) {
            reg->pirate_suppression = std::clamp(reg->pirate_suppression + boost, 0.0, 1.0);
          }
        }
      }
    }

    // Clear assignment to avoid dangling UI pointers.
    const std::string dip_note = maybe_apply_contract_diplomacy_delta(*this, c, /*success=*/true, /*was_accepted=*/true);
    c.assigned_ship_id = kInvalidId;
    c.assigned_fleet_id = kInvalidId;

    if (is_player_faction(state_, c.assignee_faction_id)) {
      EventContext ctx;
      ctx.faction_id = c.assignee_faction_id;
      ctx.system_id = c.system_id;
            std::string msg = "Contract completed: " + c.name + " (+" +
                         std::to_string(static_cast<int>(std::round(c.reward_research_points))) + " RP)";
      if (c.issuer_faction_id != kInvalidId && c.issuer_faction_id != c.assignee_faction_id) {
        msg += " (Issuer: " + faction_label(state_, c.issuer_faction_id) + ")";
      }
      if (!dip_note.empty()) {
        msg += " [" + dip_note + "]";
      }
      push_event(EventLevel::Info, EventCategory::Exploration, msg, ctx);

      JournalEntry je;
      je.day = now;
      je.hour = state_.hour_of_day;
      je.category = EventCategory::Exploration;
      je.title = "Contract Completed";
      je.text = msg;
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
  // Precompute local faction presence per system for choosing external contract
  // issuers. Population (colonies) dominates; if a system has no colonies, we
  // fall back to ship presence. This is best-effort and purely for flavor/UI:
  // contract availability should never depend on this.
  std::unordered_map<Id, std::unordered_map<Id, double>> pop_by_sys;
  std::unordered_map<Id, std::unordered_map<Id, double>> ships_by_sys;
  pop_by_sys.reserve(state_.systems.size() * 2 + 4);
  ships_by_sys.reserve(state_.systems.size() * 2 + 4);

  for (const auto& [cid, col] : state_.colonies) {
    (void)cid;
    if (col.faction_id == kInvalidId) continue;
    const auto* b = find_ptr(state_.bodies, col.body_id);
    if (!b) continue;
    if (b->system_id == kInvalidId) continue;
    pop_by_sys[b->system_id][col.faction_id] += std::max(0.0, col.population_millions);
  }

  for (const auto& [sid, sh] : state_.ships) {
    (void)sid;
    if (sh.faction_id == kInvalidId) continue;
    if (sh.system_id == kInvalidId) continue;
    if (sh.hp <= 0.0) continue;
    ships_by_sys[sh.system_id][sh.faction_id] += 1.0;
  }

  std::unordered_map<Id, std::vector<std::pair<Id, double>>> ranked_presence_cache;
  ranked_presence_cache.reserve(state_.systems.size() * 2 + 4);

  auto ranked_presence = [&](Id sys_id) -> const std::vector<std::pair<Id, double>>& {
    auto itc = ranked_presence_cache.find(sys_id);
    if (itc != ranked_presence_cache.end()) return itc->second;

    std::vector<std::pair<Id, double>> out;
    const auto itp = pop_by_sys.find(sys_id);
    const bool use_pop = (itp != pop_by_sys.end() && !itp->second.empty());
    if (use_pop) {
      out.reserve(itp->second.size());
      for (const auto& [fid, score] : itp->second) out.push_back({fid, score});
    } else {
      const auto its = ships_by_sys.find(sys_id);
      if (its != ships_by_sys.end()) {
        out.reserve(its->second.size());
        for (const auto& [fid, score] : its->second) out.push_back({fid, score});
      }
    }

    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
      if (a.second != b.second) return a.second > b.second;
      return a.first < b.first;
    });

    auto [it_ins, _] = ranked_presence_cache.emplace(sys_id, std::move(out));
    return it_ins->second;
  };

  auto pick_contract_issuer_for_system = [&](Id sys_id, Id assignee_fid) -> Id {
    if (sys_id == kInvalidId || assignee_fid == kInvalidId) return assignee_fid;

    // Prefer an external issuer, but fall back to "self-issued" contracts.
    const auto& ranked = ranked_presence(sys_id);
    for (const auto& [cand_fid, _score] : ranked) {
      if (cand_fid == kInvalidId) continue;
      if (cand_fid == assignee_fid) continue;

      const auto* f = find_ptr(state_.factions, cand_fid);
      if (!f) continue;
      if (f->control == FactionControl::AI_Pirate) continue;

      // Require mutual non-hostility to avoid nonsensical issuers.
      if (diplomatic_status(cand_fid, assignee_fid) == DiplomacyStatus::Hostile) continue;
      if (diplomatic_status(assignee_fid, cand_fid) == DiplomacyStatus::Hostile) continue;

      return cand_fid;
    }

    return assignee_fid;
  };


  for (Id fid : faction_ids) {
    const auto* fac = find_ptr(state_.factions, fid);
    if (!fac) continue;

    int offered_count = 0;
    std::unordered_set<Id> used_anoms;
    std::unordered_set<Id> used_wrecks;
    std::unordered_set<Id> used_jumps;
    std::unordered_set<Id> used_bounties;
    std::unordered_set<Id> used_convoys;
    used_anoms.reserve(64);
    used_wrecks.reserve(64);
    used_jumps.reserve(64);
    used_bounties.reserve(64);
    used_convoys.reserve(64);

    for (Id contract_id : sorted_keys(state_.contracts)) {
      const Contract& c = state_.contracts.at(contract_id);
      if (c.assignee_faction_id != fid) continue;
      if (c.status == ContractStatus::Offered) offered_count++;
      if (c.status == ContractStatus::Offered || c.status == ContractStatus::Accepted) {
        if (c.target_id == kInvalidId) continue;
        switch (c.kind) {
          case ContractKind::InvestigateAnomaly: used_anoms.insert(c.target_id); break;
          case ContractKind::SalvageWreck: used_wrecks.insert(c.target_id); break;
          case ContractKind::SurveyJumpPoint: used_jumps.insert(c.target_id); break;
          case ContractKind::BountyPirate: used_bounties.insert(c.target_id); break;
          case ContractKind::EscortConvoy: used_convoys.insert(c.target_id); break;
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
    std::vector<ContractCandidate> bounty;
    std::vector<ContractCandidate> escort;
    anom.reserve(64);
    wreck.reserve(64);
    jump.reserve(64);
    bounty.reserve(32);
    escort.reserve(32);

    // Normalize discovery lists for deterministic offer generation.
    std::vector<Id> discovered_anoms = fac->discovered_anomalies;
    std::sort(discovered_anoms.begin(), discovered_anoms.end());
    discovered_anoms.erase(std::unique(discovered_anoms.begin(), discovered_anoms.end()), discovered_anoms.end());

    std::vector<Id> discovered_systems = fac->discovered_systems;
    std::sort(discovered_systems.begin(), discovered_systems.end());
    discovered_systems.erase(std::unique(discovered_systems.begin(), discovered_systems.end()), discovered_systems.end());

    // Anomalies (discovered but unresolved).
    for (Id aid : discovered_anoms) {
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
    for (Id sys_id : discovered_systems) {
      const auto* sys = find_ptr(state_.systems, sys_id);
      if (!sys) continue;
      std::vector<Id> jump_ids = sys->jump_points;
      std::sort(jump_ids.begin(), jump_ids.end());
      jump_ids.erase(std::unique(jump_ids.begin(), jump_ids.end()), jump_ids.end());
      for (Id jid : jump_ids) {
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

    // Bounties (known pirate ships, based on faction intel contacts).
    //
    // We offer bounties only for relatively fresh contacts in discovered space
    // to avoid frustrating "needle in a haystack" missions.
    auto corridor_risk_for_system = [&](Id sys_id) -> double {
      const auto* sys = find_ptr(state_.systems, sys_id);
      if (!sys) return 0.0;
      double risk = 0.0;
      // Combine live piracy presence with a short memory of recent merchant
      // losses. This makes bounty/escort offers more responsive to "raids" that
      // leave behind wrecks but not necessarily a persistent pirate presence.
      const double piracy = clamp01(piracy_risk_for_system(sys_id));
      const double loss = clamp01(civilian_shipping_loss_pressure_for_system(sys_id));
      const double security = 1.0 - (1.0 - piracy) * (1.0 - loss);
      risk += clamp01(security) * 0.60;
      risk += clamp01(sys->nebula_density) * 0.20;
      risk += clamp01(system_storm_intensity(sys_id)) * 0.20;
      return clamp01(risk);
    };

    const std::int64_t max_bounty_contact_age_days =
        std::max<std::int64_t>(30, static_cast<std::int64_t>(cfg_.contract_offer_expiry_days));

    for (Id sid : sorted_keys(fac->ship_contacts)) {
      if (sid == kInvalidId) continue;
      if (used_bounties.contains(sid)) continue;

      const auto itc = fac->ship_contacts.find(sid);
      if (itc == fac->ship_contacts.end()) continue;
      const Contact& contact = itc->second;

      if (contact.last_seen_faction_id == kInvalidId) continue;
      const auto* tf = find_ptr(state_.factions, contact.last_seen_faction_id);
      if (!tf || tf->control != FactionControl::AI_Pirate) continue;

      // Only target ships that are (still) hostile to the assignee.
      if (!are_factions_hostile(fid, contact.last_seen_faction_id) &&
          !are_factions_hostile(contact.last_seen_faction_id, fid)) {
        continue;
      }

      if (contact.system_id == kInvalidId) continue;
      if (!is_system_discovered_by_faction(fid, contact.system_id)) continue;
      if (!find_ptr(state_.systems, contact.system_id)) continue;

      // Avoid offering bounties on already-destroyed ships (contacts are pruned,
      // but in edge cases a ship could disappear between ticks).
      if (const auto* sh = find_ptr(state_.ships, sid); !sh || sh->hp <= 0.0) continue;

      const std::int64_t age = std::max<std::int64_t>(0, now - contact.last_seen_day);
      if (max_bounty_contact_age_days > 0 && age > max_bounty_contact_age_days) continue;

      const auto* d = find_design(contact.last_seen_design_id);
      const double weap = d ? (std::max(0.0, d->weapon_damage) + std::max(0.0, d->missile_damage)) : 0.0;
      const double rng = d ? (std::max(0.0, d->weapon_range_mkm) + std::max(0.0, d->missile_range_mkm)) : 0.0;
      const double hp = d ? std::max(0.0, d->max_hp) : 0.0;
      const double shields = d ? std::max(0.0, d->max_shields) : 0.0;
      const double speed = d ? std::max(0.0, d->speed_km_s) : 0.0;

      // A light-weight combat "threat" heuristic.
      const double threat = 10.0 * weap + 0.05 * rng + 0.02 * (hp + shields) + 0.0005 * speed;
      const double freshness = (max_bounty_contact_age_days > 0)
                                 ? (1.0 - std::min(1.0, static_cast<double>(age) /
                                                         static_cast<double>(max_bounty_contact_age_days)))
                                 : 1.0;

      ContractCandidate c;
      c.kind = ContractKind::BountyPirate;
      c.target_id = sid;
      c.system_id = contact.system_id;

      // Candidate value scales with threat but is reduced for stale intel.
      c.value = std::max(0.0, (12.0 + threat) * (0.6 + 0.4 * std::clamp(freshness, 0.0, 1.0)));

      // Candidate risk scales with local environment and target threat.
      const double env_risk = corridor_risk_for_system(contact.system_id);
      const double tgt_risk = clamp01(0.12 * weap + 0.002 * (hp + shields) + 0.0002 * speed);
      const double stale_risk = clamp01(static_cast<double>(age) / 90.0) * 0.20;
      c.risk = clamp01(std::max(env_risk, tgt_risk) + stale_risk);

      bounty.push_back(std::move(c));
    }

    // Escort convoys (neutral Merchant Guild ships currently on a jump-route
    // leg through discovered space).
    //
    // This is intentionally conservative: we only offer escorts for convoys
    // that are currently en-route and where piracy risk along the corridor is
    // non-trivial.
    Id merchant_fid = kInvalidId;
    for (const auto& [mfid, mf] : state_.factions) {
      if (mf.control == FactionControl::AI_Passive && mf.name == "Merchant Guild") {
        merchant_fid = mfid;
        break;
      }
    }



    if (merchant_fid != kInvalidId) {
      for (const auto& [sid, sh] : state_.ships) {
        if (sid == kInvalidId) continue;
        if (sh.hp <= 0.0) continue;
        if (sh.faction_id != merchant_fid) continue;
        if (sh.system_id == kInvalidId) continue;
        if (!sh.name.empty() && sh.name.find("Merchant Convoy") != 0) continue;
        if (used_convoys.contains(sid)) continue;

        // Only offer escorts when the convoy is in a discovered system.
        if (!is_system_discovered_by_faction(fid, sh.system_id)) continue;

        // Determine the destination system of the convoy's current jump leg by
        // expanding the leading sequence of TravelViaJump orders.
        std::vector<Id> leg_jumps;
        if (auto itso = state_.ship_orders.find(sid); itso != state_.ship_orders.end()) {
          for (const auto& ord : itso->second.queue) {
            if (!std::holds_alternative<TravelViaJump>(ord)) break;
            const Id jid = std::get<TravelViaJump>(ord).jump_point_id;
            if (jid == kInvalidId) break;
            leg_jumps.push_back(jid);
          }
        }
        if (leg_jumps.empty()) continue;

        std::vector<Id> corridor_systems;
        corridor_systems.reserve(leg_jumps.size() + 1);
        Id cur_sys = sh.system_id;
        corridor_systems.push_back(cur_sys);

        bool ok = true;
        for (Id jid : leg_jumps) {
          const auto* jp = find_ptr(state_.jump_points, jid);
          if (!jp || jp->system_id != cur_sys || jp->linked_jump_id == kInvalidId) { ok = false; break; }
          const auto* other = find_ptr(state_.jump_points, jp->linked_jump_id);
          if (!other || other->system_id == kInvalidId) { ok = false; break; }
          cur_sys = other->system_id;
          corridor_systems.push_back(cur_sys);
        }
        if (!ok) continue;
        const Id dest_sys = cur_sys;
        if (dest_sys == kInvalidId || dest_sys == sh.system_id) continue;

        // To avoid fog-of-war spoilers, only offer convoys whose destination is
        // already discovered by the assignee faction.
        if (!is_system_discovered_by_faction(fid, dest_sys)) continue;

        double corridor_risk = 0.0;
        for (Id sys_id : corridor_systems) {
          corridor_risk = std::max(corridor_risk, corridor_risk_for_system(sys_id));
        }

        // Skip trivial / safe routes; escorts should feel meaningful.
        if (corridor_risk < 0.15) continue;

        ContractCandidate c;
        c.kind = ContractKind::EscortConvoy;
        c.target_id = sid;
        c.system_id = sh.system_id;
        c.target_id2 = dest_sys;
        c.leg_hops = static_cast<int>(leg_jumps.size());
        c.risk = clamp01(corridor_risk);
        c.value = 10.0 + 6.0 * c.risk + 0.5 * static_cast<double>(c.leg_hops);
        escort.push_back(std::move(c));
      }
    }

    if (anom.empty() && wreck.empty() && jump.empty() && bounty.empty() && escort.empty()) continue;

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
    if (home_sys == kInvalidId && !discovered_systems.empty()) {
      home_sys = discovered_systems.front();
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
      buckets.reserve(5);
      if (!anom.empty()) buckets.push_back({3, ContractKind::InvestigateAnomaly});
      if (!jump.empty()) buckets.push_back({2, ContractKind::SurveyJumpPoint});
      if (!bounty.empty()) buckets.push_back({2, ContractKind::BountyPirate});
      if (!wreck.empty()) buckets.push_back({1, ContractKind::SalvageWreck});
      if (!escort.empty()) buckets.push_back({1, ContractKind::EscortConvoy});
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
      else if (chosen == ContractKind::BountyPirate) cand = pick_from(bounty);
      else if (chosen == ContractKind::SalvageWreck) cand = pick_from(wreck);
      else cand = pick_from(escort);
      if (!cand) break;

      Contract c;
      c.id = allocate_id(state_);
      c.kind = cand->kind;
      c.status = ContractStatus::Offered;
      c.assignee_faction_id = fid;
      c.issuer_faction_id = fid;
      // Select an issuer faction (may differ from assignee for external contracts).
      {
        Id issuer = fid;
        if (c.kind == ContractKind::EscortConvoy) {
          if (const auto* convoy = find_ptr(state_.ships, c.target_id)) issuer = convoy->faction_id;
        } else {
          issuer = pick_contract_issuer_for_system(c.system_id, fid);
        }

        if (issuer != kInvalidId && issuer != fid) {
          if (const auto* ifac = find_ptr(state_.factions, issuer)) {
            if (ifac->control != FactionControl::AI_Pirate &&
                diplomatic_status(issuer, fid) != DiplomacyStatus::Hostile &&
                diplomatic_status(fid, issuer) != DiplomacyStatus::Hostile) {
              c.issuer_faction_id = issuer;
            }
          }
        }
      }
      c.system_id = cand->system_id;
      c.target_id = cand->target_id;
      c.target_id2 = cand->target_id2;
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

      // Escort contracts include the length of the escorted leg in their hop
      // estimate so reward scales more plausibly with distance.
      if (c.kind == ContractKind::EscortConvoy) {
        c.hops_estimate += std::max(0, cand->leg_hops);
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

      // Escort risk is computed over the full corridor (not just the start
      // system). Bounties also incorporate target threat and intel staleness.
      // In both cases we prefer the candidate's precomputed estimate.
      if (c.kind == ContractKind::EscortConvoy || c.kind == ContractKind::BountyPirate) {
        c.risk_estimate = clamp01(std::max(c.risk_estimate, cand->risk));
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
      } else if (c.kind == ContractKind::BountyPirate) {
        // Use intel contacts for naming (avoid omniscience).
        std::string ship_name;
        std::int64_t age_days = 0;
        if (auto itc = fac->ship_contacts.find(c.target_id); itc != fac->ship_contacts.end()) {
          const Contact& ct = itc->second;
          ship_name = !ct.last_seen_name.empty() ? ct.last_seen_name
                                                 : ("Pirate " + std::to_string(static_cast<std::uint64_t>(ct.ship_id)));
          age_days = std::max<std::int64_t>(0, now - ct.last_seen_day);
          // Keep the contract's system_id aligned to the last seen system.
          if (ct.system_id != kInvalidId) c.system_id = ct.system_id;
        } else if (const auto* sh = find_ptr(state_.ships, c.target_id)) {
          ship_name = !sh->name.empty() ? sh->name : ("Ship " + std::to_string(static_cast<std::uint64_t>(sh->id)));
        }

        if (ship_name.empty()) ship_name = "Pirate " + std::to_string(static_cast<std::uint64_t>(c.target_id));

        target_name = ship_name;
        if (const auto* sys = find_ptr(state_.systems, c.system_id)) {
          const std::string sys_name =
              !sys->name.empty() ? sys->name : ("System " + std::to_string(static_cast<std::uint64_t>(sys->id)));
          target_name += " @ " + sys_name;
        }
        if (age_days > 0) {
          target_name += " (" + std::to_string(static_cast<long long>(age_days)) + "d old)";
        }
      } else if (c.kind == ContractKind::EscortConvoy) {
        const auto* sh = find_ptr(state_.ships, c.target_id);
        const auto* dest = (c.target_id2 != kInvalidId) ? find_ptr(state_.systems, c.target_id2) : nullptr;
        if (sh) {
          target_name = !sh->name.empty() ? sh->name : ("Convoy " + std::to_string(static_cast<std::uint64_t>(sh->id)));
          if (dest) {
            const std::string dest_name = !dest->name.empty() ? dest->name
                                                             : ("System " + std::to_string(static_cast<std::uint64_t>(dest->id)));
            target_name += " -> " + dest_name;
          }
        }
      }
      if (target_name.empty()) target_name = "Target " + std::to_string(static_cast<std::uint64_t>(c.target_id));

      c.name = contract_kind_label(c.kind) + ": " + target_name;

      state_.contracts[c.id] = std::move(c);
    }
  }
}

} // namespace nebula4x
