#include "nebula4x/core/simulation.h"

#include "simulation_internal.h"
#include "simulation_procgen.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <limits>
#include <optional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "nebula4x/core/scenario.h"
#include "nebula4x/core/ai_economy.h"
#include "nebula4x/core/ship_profiles.h"
#include "nebula4x/util/log.h"
#include "nebula4x/util/trace_events.h"
#include "nebula4x/util/spatial_index.h"

namespace nebula4x {
namespace {
using sim_internal::kTwoPi;
using sim_internal::ascii_to_lower;
using sim_internal::is_mining_installation;
using sim_internal::mkm_per_day_from_speed;
using sim_internal::push_unique;
using sim_internal::vec_contains;
using sim_internal::sorted_keys;
using sim_internal::faction_has_tech;
using sim_internal::FactionEconomyMultipliers;
using sim_internal::compute_faction_economy_multipliers;
using sim_internal::trade_agreement_output_multiplier;
using sim_internal::compute_power_allocation;
} // namespace

void Simulation::tick_colonies(double dt_days, bool emit_daily_events) {
  if (dt_days <= 0.0) return;
  NEBULA4X_TRACE_SCOPE("tick_colonies", "sim.econ");
  // Precompute faction-wide economy modifiers once per tick for determinism
  // and to avoid repeated tech scanning in inner loops.
  std::unordered_map<Id, FactionEconomyMultipliers> fac_mult;
  fac_mult.reserve(state_.factions.size());
  for (Id fid : sorted_keys(state_.factions)) {
    auto m = compute_faction_economy_multipliers(content_, state_.factions.at(fid));
    const double trade = trade_agreement_output_multiplier(state_, fid);
    m.industry *= trade;
    m.research *= trade;
    m.construction *= trade;
    m.shipyard *= trade;
    fac_mult.emplace(fid, m);
  }

  const FactionEconomyMultipliers default_mult;
  auto mult_for = [&](Id fid) -> const FactionEconomyMultipliers& {
    auto it = fac_mult.find(fid);
    if (it == fac_mult.end()) return default_mult;
    return it->second;
  };

  // Aggregate mining requests so that multiple colonies on the same body share
  // finite deposits fairly and deterministically.
  //
  // Structure: body_id -> mineral -> [(colony_id, requested_tons_this_tick), ...]
  std::unordered_map<Id, std::unordered_map<std::string, std::vector<std::pair<Id, double>>>> mine_reqs;
  mine_reqs.reserve(state_.colonies.size());

  for (Id cid : sorted_keys(state_.colonies)) {
    auto& colony = state_.colonies.at(cid);
    const ColonyConditionMultipliers cond_mult = colony_condition_multipliers(colony);
    const double stability_mult = colony_stability_output_multiplier_for_colony(colony);

    const double mining_mult = std::max(0.0, mult_for(colony.faction_id).mining) * cond_mult.mining * stability_mult;

    // --- Installation-based production ---
    for (const auto& [inst_id, count] : colony.installations) {
      if (count <= 0) continue;
      const auto dit = content_.installations.find(inst_id);
      if (dit == content_.installations.end()) continue;

      const InstallationDef& def = dit->second;
      if (!is_mining_installation(def)) continue;
      if (def.mining_tons_per_day <= 0.0 && def.produces_per_day.empty()) continue;

      // Mining: generate a request against body deposits.
      const Id body_id = colony.body_id;

      // If the body is missing (invalid save / hand-edited state), fall back to
      // the older "unlimited" behaviour to avoid silently losing resources.
      const Body* body = find_ptr(state_.bodies, body_id);
      if (!body) {
        for (const auto& [mineral, per_day] : def.produces_per_day) {
          colony.minerals[mineral] += per_day * static_cast<double>(count) * mining_mult * dt_days;
        }
        continue;
      }

      // New model: generic mining capacity distributed across all deposits.
      if (def.mining_tons_per_day > 0.0 && !body->mineral_deposits.empty()) {
        const double cap = def.mining_tons_per_day * static_cast<double>(count) * mining_mult * dt_days;
        if (cap > 1e-12) {
          // Deterministic iteration over deposit keys.
          const auto keys = sorted_keys(body->mineral_deposits);
          double total_remaining = 0.0;
          for (const auto& k : keys) {
            const double rem = std::max(0.0, body->mineral_deposits.at(k));
            if (rem > 1e-12) total_remaining += rem;
          }

          if (total_remaining > 1e-12) {
            for (const auto& k : keys) {
              const double rem = std::max(0.0, body->mineral_deposits.at(k));
              if (rem <= 1e-12) continue;
              const double req = cap * (rem / total_remaining);
              if (req <= 1e-12) continue;
              mine_reqs[body_id][k].push_back({cid, req});
            }
          }
        }
        continue;
      }

      // Legacy model: fixed per-mineral extraction rates.
      for (const auto& [mineral, per_day] : def.produces_per_day) {
        const double req = per_day * static_cast<double>(count) * mining_mult * dt_days;
        if (req <= 1e-12) continue;
        mine_reqs[body_id][mineral].push_back({cid, req});
      }
    }

    // --- Population growth/decline ---
    //
    // This intentionally does not generate events for *normal* growth/decline
    // (would be too spammy). However, if habitability is enabled we will emit
    // throttled warning events for severe habitation shortfalls.
    if (colony.population_millions > 0.0) {
      double pop = colony.population_millions;

      const double base_per_day = cfg_.population_growth_rate_per_year / 365.25;
      if (std::fabs(base_per_day) > 1e-12) {
        double growth_mult = 1.0;
        if (cfg_.enable_habitability) {
          const double hab = body_habitability_for_faction(colony.body_id, colony.faction_id);
          if (hab >= 0.999) {
            growth_mult = 1.0;
          } else {
            // Hostile worlds: growth is slower even when supported.
            growth_mult = std::max(0.0, cfg_.habitation_supported_growth_multiplier) * std::clamp(hab, 0.0, 1.0);
          }
        }
        // Apply innate faction trait multiplier (procedural species/empires).
        if (const auto* fac = find_ptr(state_.factions, colony.faction_id)) {
          const double t = fac->traits.pop_growth;
          if (std::isfinite(t) && t >= 0.0) {
            growth_mult *= t;
          }
        }
        growth_mult *= cond_mult.pop_growth;
        pop *= (1.0 + base_per_day * growth_mult * dt_days);
      }

      if (cfg_.enable_habitability) {
        const double hab = body_habitability_for_faction(colony.body_id, colony.faction_id);
        if (hab < 0.999) {
          const double required = std::max(0.0, pop) * std::clamp(1.0 - hab, 0.0, 1.0);
          const double have = habitation_capacity_millions(colony);
          if (required > 1e-9 && have + 1e-9 < required) {
            const double shortfall_frac = std::clamp(1.0 - (have / required), 0.0, 1.0);
            const double decline_per_day = std::max(0.0, cfg_.habitation_shortfall_decline_rate_per_year) / 365.25;
            pop *= (1.0 - decline_per_day * shortfall_frac * dt_days);

            // Throttled warning events so the player understands why population is dropping.
            if (emit_daily_events &&
                shortfall_frac >= std::max(0.0, cfg_.habitation_shortfall_event_min_fraction)) {
              const int interval = std::max(0, cfg_.habitation_shortfall_event_interval_days);
              if (interval <= 0 || (state_.date.days_since_epoch() % interval) == 0) {
                const auto* body = find_ptr(state_.bodies, colony.body_id);
                const std::string body_name = body ? body->name : std::string("(unknown body)");
                std::ostringstream ss;
                ss << "Habitation shortfall at colony '" << colony.name << "' (" << body_name
                   << "): need " << required << "M support, have " << have << "M (habitability "
                   << std::fixed << std::setprecision(2) << (hab * 100.0) << "%). Population is declining.";

                EventContext ctx;
                ctx.faction_id = colony.faction_id;
                ctx.colony_id = colony.id;
                push_event(EventLevel::Warn, EventCategory::General, ss.str(), ctx);
              }
            }
          }
        }
      }

      if (!std::isfinite(pop)) pop = 0.0;
      colony.population_millions = std::max(0.0, pop);
    }
  }

  // --- Execute mining extraction against finite deposits ---
  if (!mine_reqs.empty()) {
    std::vector<Id> body_ids;
    body_ids.reserve(mine_reqs.size());
    for (const auto& [bid, _] : mine_reqs) body_ids.push_back(bid);
    std::sort(body_ids.begin(), body_ids.end());

    for (Id bid : body_ids) {
      Body* body = find_ptr(state_.bodies, bid);
      if (!body) continue;

      auto& per_mineral = mine_reqs.at(bid);
      std::vector<std::string> minerals;
      minerals.reserve(per_mineral.size());
      for (const auto& [m, _] : per_mineral) minerals.push_back(m);
      std::sort(minerals.begin(), minerals.end());

      for (const std::string& mineral : minerals) {
        auto it_list = per_mineral.find(mineral);
        if (it_list == per_mineral.end()) continue;
        auto& list = it_list->second;
        if (list.empty()) continue;

        // Total requested extraction for this mineral on this body.
        double total_req = 0.0;
        for (const auto& [_, req] : list) {
          if (req > 0.0) total_req += req;
        }
        if (total_req <= 1e-12) continue;

        // Deposit semantics:
        // - If the body's mineral_deposits map is empty, treat missing keys as
        //   "unlimited" (legacy saves / content that predates finite deposits).
        // - Otherwise, missing keys mean this mineral is not present.
        auto it_dep = body->mineral_deposits.find(mineral);
        if (it_dep == body->mineral_deposits.end()) {
          if (body->mineral_deposits.empty()) {
            for (const auto& [colony_id, req] : list) {
              if (req <= 1e-12) continue;
              if (auto* c = find_ptr(state_.colonies, colony_id)) {
                c->minerals[mineral] += req;
              }
            }
          }
          continue;
        }

        const double before_raw = it_dep->second;
        const double before = std::max(0.0, before_raw);
        if (before <= 1e-9) {
          it_dep->second = 0.0;
          continue;
        }

        if (before + 1e-9 >= total_req) {
          // Enough deposit to satisfy everyone fully.
          for (const auto& [colony_id, req] : list) {
            if (req <= 1e-12) continue;
            if (auto* c = find_ptr(state_.colonies, colony_id)) {
              c->minerals[mineral] += req;
            }
          }
          it_dep->second = std::max(0.0, before - total_req);
        } else {
          // Not enough deposit:
          // - legacy mode: proportional allocation by request
          // - scarcity-aware mode: boost colonies that are short on local buffer
          //   (still deterministic and request-bounded).
          struct ScarcityAllocation {
            Id colony_id{kInvalidId};
            double req{0.0};
            double weight{0.0};
            double alloc{0.0};
          };

          std::vector<ScarcityAllocation> allocs;
          allocs.reserve(list.size());

          const double safe_dt_days = std::max(1e-6, dt_days);
          const double buffer_days = std::max(0.0, cfg_.mining_scarcity_buffer_days);
          const double need_boost = std::max(0.0, cfg_.mining_scarcity_need_boost);
          const bool scarcity_priority = cfg_.enable_mining_scarcity_priority && need_boost > 1e-12 &&
                                         buffer_days > 1e-12 && list.size() > 1;

          for (const auto& [colony_id, req] : list) {
            if (req <= 1e-12) continue;
            ScarcityAllocation a;
            a.colony_id = colony_id;
            a.req = req;
            if (!scarcity_priority) {
              a.weight = req;
            } else {
              double stock = 0.0;
              if (const Colony* c = find_ptr(state_.colonies, colony_id)) {
                if (auto it_stock = c->minerals.find(mineral); it_stock != c->minerals.end()) {
                  stock = std::max(0.0, it_stock->second);
                }
              }

              const double req_per_day = req / safe_dt_days;
              const double target_buffer = req_per_day * buffer_days;
              double shortage = 0.0;
              if (target_buffer > 1e-9) {
                shortage = std::clamp((target_buffer - stock) / target_buffer, 0.0, 1.0);
              }
              const double mult = 1.0 + need_boost * shortage;
              a.weight = req * std::max(0.0, mult);
              if (!std::isfinite(a.weight) || a.weight <= 1e-12) a.weight = req;
            }
            allocs.push_back(a);
          }

          double remaining = before;
          constexpr int kMaxPasses = 8;
          for (int pass = 0; pass < kMaxPasses && remaining > 1e-9; ++pass) {
            double weight_sum = 0.0;
            int active = 0;
            for (const auto& a : allocs) {
              const double room = a.req - a.alloc;
              if (room <= 1e-12) continue;
              weight_sum += std::max(1e-12, a.weight);
              ++active;
            }
            if (active <= 0) break;

            double given = 0.0;
            if (weight_sum <= 1e-12) {
              const double equal_share = remaining / static_cast<double>(active);
              for (auto& a : allocs) {
                const double room = a.req - a.alloc;
                if (room <= 1e-12) continue;
                const double add = std::min(room, equal_share);
                if (add <= 1e-12) continue;
                a.alloc += add;
                given += add;
              }
            } else {
              for (auto& a : allocs) {
                const double room = a.req - a.alloc;
                if (room <= 1e-12) continue;
                const double frac = std::max(1e-12, a.weight) / weight_sum;
                const double target = remaining * frac;
                const double add = std::min(room, target);
                if (add <= 1e-12) continue;
                a.alloc += add;
                given += add;
              }
            }

            if (given <= 1e-12) break;
            remaining = std::max(0.0, remaining - given);
          }

          if (remaining > 1e-9) {
            // Deterministic final pass to consume residual due to capping/rounding.
            for (auto& a : allocs) {
              if (remaining <= 1e-9) break;
              const double room = a.req - a.alloc;
              if (room <= 1e-12) continue;
              const double add = std::min(room, remaining);
              a.alloc += add;
              remaining = std::max(0.0, remaining - add);
            }
          }

          double distributed = 0.0;
          for (const auto& a : allocs) {
            if (a.alloc <= 1e-12) continue;
            if (auto* c = find_ptr(state_.colonies, a.colony_id)) {
              c->minerals[mineral] += a.alloc;
              distributed += a.alloc;
            }
          }

          // Keep mass conservation robust under floating-point roundoff.
          distributed = std::clamp(distributed, 0.0, before);
          it_dep->second = std::max(0.0, before - distributed);
          if (it_dep->second <= 1e-9) it_dep->second = 0.0;
          if (it_dep->second > 0.0 && it_dep->second < before * 1e-12) it_dep->second = 0.0;
        }

        if (it_dep->second <= 1e-9) {
          it_dep->second = 0.0;
        }

        // Depletion warning (once, at the moment a deposit hits zero).
        if (before > 1e-9 && it_dep->second <= 1e-9) {
          Id best_cid = kInvalidId;
          Id best_fid = kInvalidId;
          for (const auto& [colony_id, req] : list) {
            if (req <= 1e-12) continue;
            if (best_cid == kInvalidId || colony_id < best_cid) {
              best_cid = colony_id;
              if (const Colony* c = find_ptr(state_.colonies, colony_id)) {
                best_fid = c->faction_id;
              }
            }
          }

          EventContext ctx;
          ctx.system_id = body->system_id;
          ctx.colony_id = best_cid;
          ctx.faction_id = best_fid;

          const std::string msg = "Mineral deposit depleted on " + body->name + ": " + mineral;
          push_event(EventLevel::Warn, EventCategory::Construction, msg, ctx);
        }
      }
    }

  }

  // --- Execute non-mining industry production/consumption ---
  //
  // This stage runs *after* mining extraction so that freshly mined minerals can
  // be consumed by industry in the same day.
  for (Id cid : sorted_keys(state_.colonies)) {
    Colony& colony = state_.colonies.at(cid);
    const ColonyConditionMultipliers cond_mult = colony_condition_multipliers(colony);
    const double stability_mult = colony_stability_output_multiplier_for_colony(colony);

    double industry_mult = std::max(0.0, mult_for(colony.faction_id).industry);
    // Trade prosperity bonus (market access / hub activity), reduced by piracy/blockade disruption.
    industry_mult *= trade_prosperity_output_multiplier_for_colony(colony.id);
    industry_mult *= cond_mult.industry;
    industry_mult *= stability_mult;
    if (cfg_.enable_blockades) industry_mult *= blockade_output_multiplier_for_colony(colony.id);

    // Deterministic processing: installation iteration order of unordered_map is unspecified.
    std::vector<std::string> inst_ids;
    inst_ids.reserve(colony.installations.size());
    for (const auto& [inst_id, _] : colony.installations) inst_ids.push_back(inst_id);
    std::sort(inst_ids.begin(), inst_ids.end());

    for (const std::string& inst_id : inst_ids) {
      auto it_count = colony.installations.find(inst_id);
      if (it_count == colony.installations.end()) continue;
      const int count = std::max(0, it_count->second);
      if (count <= 0) continue;

      const auto it_def = content_.installations.find(inst_id);
      if (it_def == content_.installations.end()) continue;
      const InstallationDef& def = it_def->second;

      // Mining is handled above against finite deposits.
      if (is_mining_installation(def)) continue;

      if (def.produces_per_day.empty() && def.consumes_per_day.empty()) continue;

      // Compute the fraction of full-rate operation we can support with available inputs.
      double frac = 1.0;
      for (const auto& [mineral, per_day_raw] : def.consumes_per_day) {
        const double per_day = std::max(0.0, per_day_raw);
        if (per_day <= 1e-12) continue;

        const double req = per_day * static_cast<double>(count) * dt_days;

        const double have = [&]() {
          const auto it = colony.minerals.find(mineral);
          if (it == colony.minerals.end()) return 0.0;
          return std::max(0.0, it->second);
        }();

        if (req > 1e-12) frac = std::min(frac, have / req);
      }

      frac = std::clamp(frac, 0.0, 1.0);
      if (frac <= 1e-12) continue;

      // Consume inputs first (based on the computed fraction), then produce outputs.
      for (const auto& [mineral, per_day_raw] : def.consumes_per_day) {
        const double per_day = std::max(0.0, per_day_raw);
        if (per_day <= 1e-12) continue;
        const double amt = per_day * static_cast<double>(count) * frac * dt_days;
        if (amt <= 1e-12) continue;

        double& stock = colony.minerals[mineral]; // creates entry if missing
        stock = std::max(0.0, stock - amt);
        if (stock <= 1e-9) stock = 0.0;
      }

      for (const auto& [mineral, per_day_raw] : def.produces_per_day) {
        const double per_day = std::max(0.0, per_day_raw);
        if (per_day <= 1e-12) continue;
        const double amt = per_day * static_cast<double>(count) * frac * industry_mult * dt_days;
        if (amt <= 1e-12) continue;
        colony.minerals[mineral] += amt;
      }
    }
  }

  // --- Geological surveys (procedural deposit discovery) ---
  //
  // When enabled, colonies can build the "geological_survey" installation to
  // occasionally discover additional mineral deposits on their body over time.
  // This is intentionally *conservative* (low probability, bounded yields) so
  // it acts as a long-term pressure valve rather than an infinite free-money
  // button. All rolls are deterministic based on (day, colony id, body id).
  if (emit_daily_events && cfg_.enable_geological_survey) {
    // Build a deterministic list of mineable resources (candidate deposits).
    std::vector<std::string> mineables;
    mineables.reserve(content_.resources.size());
    for (const auto& [rid, rd] : content_.resources) {
      if (rid.empty()) continue;
      if (!rd.mineable) continue;
      mineables.push_back(rid);
    }
    std::sort(mineables.begin(), mineables.end());

    const std::int64_t now_day = state_.date.days_since_epoch();

    for (Id cid : sorted_keys(state_.colonies)) {
      Colony& colony = state_.colonies.at(cid);

      const auto it_survey = colony.installations.find("geological_survey");
      const int survey_count = (it_survey == colony.installations.end()) ? 0 : std::max(0, it_survey->second);
      if (survey_count <= 0) continue;

      Body* body = find_ptr(state_.bodies, colony.body_id);
      if (!body) continue;

      // Avoid changing legacy "unlimited deposits" bodies (empty map).
      if (body->mineral_deposits.empty()) continue;

      // Total remaining deposits on the body (used for depletion scaling + composition bias).
      double total_remaining = 0.0;
      for (const auto& [_, rem_raw] : body->mineral_deposits) {
        const double rem = std::max(0.0, rem_raw);
        if (rem > 1e-12) total_remaining += rem;
      }

      // Region richness multipliers (procedural galaxy).
      double mineral_rich = 1.0;
      double volatile_rich = 1.0;
      if (const auto* sys = find_ptr(state_.systems, body->system_id)) {
        if (const auto* reg = find_ptr(state_.regions, sys->region_id)) {
          mineral_rich = std::max(0.0, reg->mineral_richness_mult);
          volatile_rich = std::max(0.0, reg->volatile_richness_mult);
        }
      }

      // Depletion fraction: 0 when deposits >= threshold, 1 when fully depleted.
      double depletion_frac = 0.0;
      const double dep_thr = std::max(0.0, cfg_.geological_survey_depletion_threshold_tons);
      if (dep_thr > 1e-9) {
        depletion_frac = std::clamp(1.0 - (total_remaining / dep_thr), 0.0, 1.0);
      }

      const double mining_mult = std::max(0.0, mult_for(colony.faction_id).mining) *
                                colony_condition_multipliers(colony).mining;

      // Probability per installation.
      const double base_p =
          std::clamp(cfg_.geological_survey_discovery_chance_per_day_per_installation, 0.0, 1.0);
      double p = base_p * (0.5 + 0.5 * std::clamp(mining_mult, 0.0, 4.0));
      p *= (1.0 + std::max(0.0, cfg_.geological_survey_depletion_chance_boost) * depletion_frac);
      p = std::clamp(p, 0.0, 0.25);

      // Deterministic RNG seed (day + colony id + body id).
      std::uint64_t seed = 0x47534C5645525955ull; // 'GSLVERYU' - arbitrary tag
      seed ^= static_cast<std::uint64_t>(now_day) * 0x9E3779B97F4A7C15ull;
      seed ^= static_cast<std::uint64_t>(cid) * 0xBF58476D1CE4E5B9ull;
      seed ^= static_cast<std::uint64_t>(body->id) * 0x94D049BB133111EBull;
      sim_procgen::HashRng rng(seed);

      // Record discoveries to emit a single aggregated event.
      std::unordered_map<std::string, double> discovered_tons;
      discovered_tons.reserve(4);

      int discoveries = 0;
      const int max_disc = std::max(0, cfg_.geological_survey_max_discoveries_per_colony_per_day);

      auto pick_mineral_id = [&]() -> std::string {
        if (mineables.empty()) return {};

        double sum_w = 0.0;
        std::vector<double> weights;
        weights.reserve(mineables.size());

        for (const std::string& rid : mineables) {
          const auto it_rd = content_.resources.find(rid);
          if (it_rd == content_.resources.end()) {
            weights.push_back(0.0);
            continue;
          }
          const ResourceDef& rd = it_rd->second;

          double w = 1.0;
          const bool is_volatile = (rd.category == "volatile");
          w *= is_volatile ? volatile_rich : mineral_rich;

          // Body-type bias: comets skew heavily toward volatiles.
          if (body->type == BodyType::Comet) {
            w *= is_volatile ? 6.0 : 0.25;
          } else if (body->type == BodyType::Asteroid) {
            w *= 0.25;
          } else if (body->type == BodyType::Moon) {
            w *= 0.6;
          }

          // Bias toward the body's existing deposit composition (if non-empty).
          if (total_remaining > 1e-9) {
            const auto it_dep = body->mineral_deposits.find(rid);
            if (it_dep != body->mineral_deposits.end()) {
              const double rem = std::max(0.0, it_dep->second);
              const double share = std::clamp(rem / total_remaining, 0.0, 1.0);
              w *= (0.4 + 1.6 * share);
            } else {
              w *= 0.6;
            }
          }

          if (!std::isfinite(w) || w < 0.0) w = 0.0;
          weights.push_back(w);
          sum_w += w;
        }

        if (!(sum_w > 0.0) || !std::isfinite(sum_w)) {
          const int idx = rng.range_int(0, static_cast<int>(mineables.size()) - 1);
          return mineables[static_cast<std::size_t>(idx)];
        }

        const double r = rng.next_u01() * sum_w;
        double acc = 0.0;
        for (std::size_t i = 0; i < mineables.size(); ++i) {
          acc += weights[i];
          if (r <= acc) return mineables[i];
        }
        return mineables.back();
      };

      const double min_tons = std::max(0.0, cfg_.geological_survey_min_deposit_tons);
      const double max_tons = std::max(min_tons, cfg_.geological_survey_max_deposit_tons);

      for (int i = 0; i < survey_count; ++i) {
        if (max_disc > 0 && discoveries >= max_disc) break;
        if (!(rng.next_u01() < p)) continue;

        const std::string mineral_id = pick_mineral_id();
        if (mineral_id.empty()) continue;

        const auto it_rd = content_.resources.find(mineral_id);
        if (it_rd == content_.resources.end() || !it_rd->second.mineable) continue;

        const bool is_volatile = (it_rd->second.category == "volatile");

        // Base yield and modifiers.
        double amt = rng.range(min_tons, max_tons);

        double body_mult = 1.0;
        switch (body->type) {
          case BodyType::Planet:
            body_mult = 1.0;
            break;
          case BodyType::GasGiant:
            body_mult = 0.8;
            break;
          case BodyType::Moon:
            body_mult = 0.45;
            break;
          case BodyType::Asteroid:
            body_mult = 0.18;
            break;
          case BodyType::Comet:
            body_mult = 0.12;
            break;
          default:
            body_mult = 0.25;
            break;
        }

        const double rich = is_volatile ? volatile_rich : mineral_rich;
        const double rich_mult = std::clamp(rich, 0.2, 3.0);

        const double tech_mult = std::clamp(0.5 + 0.5 * mining_mult, 0.25, 2.5);

        // Slightly increase yields as deposits deplete (deeper drilling).
        const double dep_mult = 1.0 + 0.5 * depletion_frac;

        amt *= body_mult * rich_mult * tech_mult * dep_mult;
        if (!std::isfinite(amt) || amt <= 1e-6) continue;

        // Apply discovery.
        body->mineral_deposits[mineral_id] = std::max(0.0, body->mineral_deposits[mineral_id] + amt);
        discovered_tons[mineral_id] += amt;
        discoveries += 1;
      }

      if (!discovered_tons.empty()) {
        std::vector<std::string> keys;
        keys.reserve(discovered_tons.size());
        for (const auto& [k, _] : discovered_tons) keys.push_back(k);
        std::sort(keys.begin(), keys.end());

        std::ostringstream oss;
        oss << "Geological survey on " << body->name << " uncovered deposits: ";
        bool first = true;
        for (const auto& k : keys) {
          const double t = std::max(0.0, discovered_tons[k]);
          const long long tons = static_cast<long long>(std::llround(t));
          if (!first) oss << "; ";
          first = false;
          oss << k << " +" << tons << "t";
        }

        EventContext ctx;
        ctx.system_id = body->system_id;
        ctx.colony_id = cid;
        ctx.faction_id = colony.faction_id;
        push_event(EventLevel::Info, EventCategory::Exploration, oss.str(), ctx);
      }
    }
  }

}



void Simulation::tick_colony_conditions(double dt_days, bool day_advanced) {
  if (!cfg_.enable_colony_conditions) return;
  if (!(dt_days > 0.0)) return;

  const std::int64_t now_day = state_.date.days_since_epoch();

  // --- Condition duration decay / cleanup ---
  for (Id cid : sorted_keys(state_.colonies)) {
    Colony& colony = state_.colonies.at(cid);
    if (colony.conditions.empty()) continue;

    for (auto& cond : colony.conditions) {
      if (!std::isfinite(cond.remaining_days)) cond.remaining_days = 0.0;
      cond.remaining_days -= dt_days;
    }

    colony.conditions.erase(std::remove_if(colony.conditions.begin(), colony.conditions.end(),
                                          [](const ColonyCondition& c) {
                                            if (c.id.empty()) return true;
                                            if (!std::isfinite(c.remaining_days)) return true;
                                            return c.remaining_days <= 1e-9;
                                          }),
                            colony.conditions.end());

    // Safety cap (should rarely/never trigger).
    if (cfg_.colony_condition_max_active > 0 &&
        static_cast<int>(colony.conditions.size()) > cfg_.colony_condition_max_active) {
      std::stable_sort(colony.conditions.begin(), colony.conditions.end(),
                       [](const ColonyCondition& a, const ColonyCondition& b) {
                         if (a.remaining_days != b.remaining_days) return a.remaining_days > b.remaining_days;
                         return a.started_day > b.started_day;
                       });
      colony.conditions.resize(cfg_.colony_condition_max_active);
    }
  }

  // --- Event rolls (day boundaries only) ---
  if (!cfg_.enable_colony_events) return;
  if (!day_advanced) return;

  const int interval_days = std::max(1, cfg_.colony_event_roll_interval_days);
  const double base_neg = std::max(0.0, cfg_.colony_event_negative_chance_per_roll);
  const double base_pos = std::max(0.0, cfg_.colony_event_positive_chance_per_roll);
  const double cap = std::max(0.0, cfg_.colony_event_max_combined_chance_per_roll);
  const double fatigue_factor = std::clamp(cfg_.colony_event_existing_condition_chance_factor, 0.0, 1.0);

  for (Id cid : sorted_keys(state_.colonies)) {
    Colony& colony = state_.colonies.at(cid);

    const auto* fac = find_ptr(state_.factions, colony.faction_id);
    if (!fac) continue;
    if (!(colony.population_millions > 0.1)) continue;

    if (cfg_.colony_condition_max_active > 0 &&
        static_cast<int>(colony.conditions.size()) >= cfg_.colony_condition_max_active) {
      continue;
    }

    // Roll cadence (deterministic per colony).
    if (((now_day + static_cast<std::int64_t>(cid)) % interval_days) != 0) continue;

    std::uint64_t seed = 0xC0110C0110C0FFEEull;
    seed ^= static_cast<std::uint64_t>(now_day) * 0x9E3779B97F4A7C15ull;
    seed ^= static_cast<std::uint64_t>(cid) * 0xBF58476D1CE4E5B9ull;
    seed ^= static_cast<std::uint64_t>(colony.body_id) * 0x94D049BB133111EBull;
    sim_procgen::HashRng rng(seed);

    const ColonyStabilityStatus stab = colony_stability_status_for_colony(cid);
    const double stability = std::clamp(stab.stability, 0.0, 1.0);

    const double pop = std::max(0.0, colony.population_millions);
    const double pop_factor = std::clamp(std::log10(pop + 1.0) / 4.0, 0.10, 1.0);
    const double fatigue = std::pow(fatigue_factor, static_cast<double>(colony.conditions.size()));

    double p_neg = base_neg * pop_factor * fatigue * (0.25 + 1.5 * (1.0 - stability));
    double p_pos = base_pos * pop_factor * fatigue * (0.25 + 1.5 * stability);

    p_neg = std::clamp(p_neg, 0.0, 1.0);
    p_pos = std::clamp(p_pos, 0.0, 1.0);

    double sum = p_neg + p_pos;
    if (cap > 0.0 && sum > cap) {
      const double s = cap / sum;
      p_neg *= s;
      p_pos *= s;
      sum = cap;
    }
    if (sum <= 1e-9) continue;

    const double roll = rng.next_u01();
    const bool is_neg = roll < p_neg;
    const bool is_pos = (!is_neg) && (roll < p_neg + p_pos);
    if (!is_neg && !is_pos) continue;

    // Colony characteristics for weighting.
    int industrial_units = 0;
    double tf_points = 0.0;
    double shipyard_rate = 0.0;
    double construction_pts = 0.0;
    double research_pts = 0.0;

    for (const auto& [inst_id, count] : colony.installations) {
      if (count <= 0) continue;
      auto it = content_.installations.find(inst_id);
      if (it == content_.installations.end()) continue;
      const InstallationDef& d = it->second;

      // Heuristic: treat any installation that contributes meaningfully to
      // extraction/production/throughput as "industrial" for the purpose of
      // weighting colony events.
      const bool industrial =
          d.mining || d.mining_tons_per_day > 0.0 ||
          d.construction_points_per_day > 0.0 || d.build_rate_tons_per_day > 0.0 ||
          d.research_points_per_day > 0.0 || d.terraforming_points_per_day > 0.0 ||
          d.troop_training_points_per_day > 0.0 || d.crew_training_points_per_day > 0.0 ||
          !d.produces_per_day.empty() || !d.consumes_per_day.empty();
      if (industrial) industrial_units += count;

      tf_points += std::max(0.0, d.terraforming_points_per_day) * static_cast<double>(count);
      shipyard_rate += std::max(0.0, d.build_rate_tons_per_day) * static_cast<double>(count);
      construction_pts += std::max(0.0, d.construction_points_per_day) * static_cast<double>(count);
      research_pts += std::max(0.0, d.research_points_per_day) * static_cast<double>(count);
    }

    struct Candidate {
      const char* id{nullptr};
      double w{0.0};
      int dur_min{1};
      int dur_max{1};
      double sev_min{1.0};
      double sev_max{1.0};
    };

    std::vector<Candidate> candidates;
    candidates.reserve(4);

    if (is_neg) {
      Candidate accident{"industrial_accident",
                         0.8 + 0.02 * industrial_units + 0.01 * std::sqrt(std::max(0.0, construction_pts)),
                         14,
                         45,
                         0.90,
                         1.30};
      Candidate strike{
          "labor_strike", 1.0 + pop / 800.0 + 0.01 * industrial_units, 21, 75, 0.80, 1.30};
      Candidate disease{"disease_outbreak",
                        1.0 + 2.0 * (1.0 - stab.habitability) + 3.0 * stab.habitation_shortfall_frac,
                        28,
                        90,
                        0.80,
                        1.40};

      candidates.push_back(accident);
      candidates.push_back(strike);
      candidates.push_back(disease);
    } else {
      Candidate festival{
          "cultural_festival", 1.0 + pop / 1200.0 + 0.02 * std::sqrt(std::max(0.0, research_pts)), 7, 21, 0.90, 1.20};
      Candidate engineering{"engineering_breakthrough",
                            1.0 + 0.03 * industrial_units + 0.015 * std::sqrt(std::max(0.0, shipyard_rate)),
                            14,
                            60,
                            0.90,
                            1.30};
      Candidate terra{"terraforming_breakthrough",
                      tf_points > 1e-9 ? (1.0 + 0.15 * std::sqrt(tf_points)) : 0.0,
                      21,
                      90,
                      0.90,
                      1.30};

      candidates.push_back(festival);
      candidates.push_back(engineering);
      if (terra.w > 0.0) candidates.push_back(terra);
    }

    double total_w = 0.0;
    for (const auto& c : candidates) total_w += std::max(0.0, c.w);
    if (total_w <= 1e-9) continue;

    double pick = rng.range(0.0, total_w);
    const Candidate* chosen = nullptr;
    for (const auto& c : candidates) {
      pick -= std::max(0.0, c.w);
      if (pick <= 0.0) {
        chosen = &c;
        break;
      }
    }
    if (!chosen) chosen = &candidates.back();

    ColonyCondition cond;
    cond.id = chosen->id ? chosen->id : "";
    cond.started_day = now_day;
    cond.remaining_days = static_cast<double>(rng.range_int(chosen->dur_min, chosen->dur_max));

    double sev = rng.range(chosen->sev_min, chosen->sev_max);
    if (is_neg) {
      sev *= std::clamp(0.8 + 0.8 * (1.0 - stability), 0.8, 1.6);
    } else {
      sev *= std::clamp(0.9 + 0.3 * stability, 0.9, 1.3);
    }
    cond.severity = std::clamp(sev, 0.25, 3.0);

    if (cond.id.empty() || cond.remaining_days <= 0.0) continue;

    // Merge with existing condition of the same id (refresh duration / severity).
    bool merged = false;
    for (auto& existing : colony.conditions) {
      if (existing.id != cond.id) continue;
      existing.remaining_days = std::max(existing.remaining_days, cond.remaining_days);
      existing.severity = std::max(existing.severity, cond.severity);
      existing.started_day = cond.started_day;
      merged = true;
      break;
    }
    if (!merged) colony.conditions.push_back(cond);

    // Keep within cap (drop shortest-lived conditions first).
    if (cfg_.colony_condition_max_active > 0 &&
        static_cast<int>(colony.conditions.size()) > cfg_.colony_condition_max_active) {
      std::stable_sort(colony.conditions.begin(), colony.conditions.end(),
                       [](const ColonyCondition& a, const ColonyCondition& b) {
                         if (a.remaining_days != b.remaining_days) return a.remaining_days > b.remaining_days;
                         return a.started_day > b.started_day;
                       });
      colony.conditions.resize(cfg_.colony_condition_max_active);
    }

    // Emit event.
    const std::string name = colony_condition_display_name(cond.id);
    const auto eff = colony_condition_multipliers_for_condition(cond);

    std::vector<std::string> eff_parts;
    eff_parts.reserve(6);

    auto add_eff = [&](const char* label, double v) {
      if (!std::isfinite(v)) return;
      if (std::abs(v - 1.0) < 0.05) return;
      std::ostringstream oss;
      oss << label << " x" << std::fixed << std::setprecision(2) << v;
      eff_parts.push_back(oss.str());
    };

    add_eff("Mining", eff.mining);
    add_eff("Industry", eff.industry);
    add_eff("Research", eff.research);
    add_eff("Construction", eff.construction);
    add_eff("Shipyard", eff.shipyard);
    add_eff("Terraforming", eff.terraforming);
    add_eff("Pop", eff.pop_growth);

    std::string eff_str;
    for (std::size_t i = 0; i < eff_parts.size(); ++i) {
      if (i > 0) eff_str += ", ";
      eff_str += eff_parts[i];
    }
    if (!eff_str.empty()) eff_str = " Effects: " + eff_str + ".";

    EventContext ctx;
    ctx.faction_id = colony.faction_id;
    ctx.colony_id = cid;
    if (const auto* body = find_ptr(state_.bodies, colony.body_id)) ctx.system_id = body->system_id;

    std::ostringstream msg;
    msg << "Colony event at " << colony.name << ": " << name << " (" << static_cast<int>(cond.remaining_days) << "d)." << eff_str;
    push_event(is_neg ? EventLevel::Warn : EventLevel::Info, EventCategory::General, msg.str(), ctx);
  }
}


void Simulation::tick_research(double dt_days) {
  if (dt_days <= 0.0) return;
  NEBULA4X_TRACE_SCOPE("tick_research", "sim.econ");

  // --- Research agreements (diplomacy-driven research cooperation) ---
  //
  // A Research Agreement is a mid-tier treaty between a Trade Agreement and an
  // Alliance. It provides:
  //  1) A small research output multiplier based on the number of research partners.
  //  2) A collaboration bonus based on the *shared* daily research capacity of the
  //     partners (to avoid "free riding").
  //  3) A tech assistance multiplier when researching a tech already known by a
  //     research partner (knowledge diffusion).
  //
  // Alliances also count as research cooperation.
  std::unordered_map<Id, std::vector<Id>> research_partners;
  research_partners.reserve(state_.factions.size());
  for (Id fid : sorted_keys(state_.factions)) {
    research_partners.emplace(fid, std::vector<Id>{});
  }

  // Unique normalized (a<b) pairs with research cooperation.
  std::vector<std::pair<Id, Id>> coop_pairs;
  coop_pairs.reserve(state_.treaties.size());

  if (!state_.treaties.empty()) {
    for (Id tid : sorted_keys(state_.treaties)) {
      const Treaty& t = state_.treaties.at(tid);
      if (t.type != TreatyType::Alliance && t.type != TreatyType::ResearchAgreement) continue;
      coop_pairs.emplace_back(t.faction_a, t.faction_b);
      research_partners[t.faction_a].push_back(t.faction_b);
      research_partners[t.faction_b].push_back(t.faction_a);
    }
  }

  std::sort(coop_pairs.begin(), coop_pairs.end());
  coop_pairs.erase(std::unique(coop_pairs.begin(), coop_pairs.end()), coop_pairs.end());

  for (Id fid : sorted_keys(state_.factions)) {
    auto& v = research_partners[fid];
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
  }

  auto research_agreement_output_multiplier_for_faction = [&](Id fid) -> double {
    if (!cfg_.enable_research_agreement_bonuses) return 1.0;
    const auto it = research_partners.find(fid);
    const int partners = (it == research_partners.end()) ? 0 : static_cast<int>(it->second.size());
    if (partners <= 0) return 1.0;
    const double bonus = std::clamp(cfg_.research_agreement_output_bonus_per_partner * static_cast<double>(partners),
                                    0.0, std::max(0.0, cfg_.research_agreement_output_bonus_cap));
    return 1.0 + bonus;
  };

  auto tech_assistance_multiplier_for_faction = [&](Id fid, const std::string& tech_id) -> double {
    if (!cfg_.enable_research_agreement_bonuses) return 1.0;
    if (tech_id.empty()) return 1.0;
    const auto it = research_partners.find(fid);
    if (it == research_partners.end() || it->second.empty()) return 1.0;

    int helpers = 0;
    for (Id pid : it->second) {
      const auto pit = state_.factions.find(pid);
      if (pit == state_.factions.end()) continue;
      if (faction_has_tech(pit->second, tech_id)) ++helpers;
    }
    if (helpers <= 0) return 1.0;

    const double bonus = std::clamp(cfg_.research_agreement_tech_help_bonus_per_partner * static_cast<double>(helpers),
                                    0.0, std::max(0.0, cfg_.research_agreement_tech_help_bonus_cap));
    return 1.0 + bonus;
  };

  // Precompute faction-wide output multipliers (tech bonuses + trade treaties + research treaties).
  std::unordered_map<Id, FactionEconomyMultipliers> fac_mult;
  fac_mult.reserve(state_.factions.size());
  for (Id fid : sorted_keys(state_.factions)) {
    auto m = compute_faction_economy_multipliers(content_, state_.factions.at(fid));
    const double trade = trade_agreement_output_multiplier(state_, fid);
    m.industry *= trade;
    m.research *= trade;
    m.construction *= trade;
    m.shipyard *= trade;

    const double ra = research_agreement_output_multiplier_for_faction(fid);
    m.research *= ra;

    fac_mult.emplace(fid, m);
  }

  const FactionEconomyMultipliers default_mult;
  auto mult_for = [&](Id fid) -> const FactionEconomyMultipliers& {
    auto it = fac_mult.find(fid);
    if (it == fac_mult.end()) return default_mult;
    return it->second;
  };

  // Track per-faction research generation this tick so we can compute symmetric collaboration bonuses.
  std::unordered_map<Id, double> generated_rp;
  generated_rp.reserve(state_.factions.size());

  for (Id cid : sorted_keys(state_.colonies)) {
    auto& col = state_.colonies.at(cid);
    double rp_per_day = 0.0;
    for (const auto& [inst_id, count] : col.installations) {
      const auto dit = content_.installations.find(inst_id);
      if (dit == content_.installations.end()) continue;
      rp_per_day += dit->second.research_points_per_day * static_cast<double>(count);
    }
    if (rp_per_day <= 0.0) continue;
    rp_per_day *= std::max(0.0, mult_for(col.faction_id).research);
    // Trade prosperity bonus (system market access / hub activity).
    rp_per_day *= trade_prosperity_output_multiplier_for_colony(col.id);
    rp_per_day *= colony_condition_multipliers(col).research;
    rp_per_day *= colony_stability_output_multiplier_for_colony(col);
    if (cfg_.enable_blockades) rp_per_day *= blockade_output_multiplier_for_colony(col.id);
    if (rp_per_day <= 0.0) continue;

    auto fit = state_.factions.find(col.faction_id);
    if (fit == state_.factions.end()) continue;

    const double add = rp_per_day * dt_days;
    fit->second.research_points += add;
    generated_rp[col.faction_id] += add;
  }

  // Collaboration bonus: each partner gains a small bonus derived from the shared
  // research capacity (min of the two). This is intentionally symmetric and
  // discourages one-sided agreements.
  if (cfg_.enable_research_agreement_bonuses && cfg_.research_agreement_collaboration_bonus_fraction > 0.0 &&
      !coop_pairs.empty()) {
    const double frac = std::max(0.0, cfg_.research_agreement_collaboration_bonus_fraction);
    for (const auto& [a, b] : coop_pairs) {
      const double ga = generated_rp.count(a) ? generated_rp[a] : 0.0;
      const double gb = generated_rp.count(b) ? generated_rp[b] : 0.0;
      const double base = std::min(ga, gb);
      if (base <= 0.0) continue;

      const double bonus = base * frac;
      if (bonus <= 0.0) continue;

      auto ita = state_.factions.find(a);
      auto itb = state_.factions.find(b);
      if (ita != state_.factions.end()) ita->second.research_points += bonus;
      if (itb != state_.factions.end()) itb->second.research_points += bonus;
    }
  }

  auto prereqs_met = [&](const Faction& f, const TechDef& t) {
    for (const auto& p : t.prereqs) {
      if (!faction_has_tech(f, p)) return false;
    }
    return true;
  };

  for (Id fid : sorted_keys(state_.factions)) {
    auto& fac = state_.factions.at(fid);
    auto enqueue_unique = [&](const std::string& tech_id) {
      if (tech_id.empty()) return;
      if (faction_has_tech(fac, tech_id)) return;
      if (std::find(fac.research_queue.begin(), fac.research_queue.end(), tech_id) != fac.research_queue.end()) return;
      fac.research_queue.push_back(tech_id);
    };

    auto clean_queue = [&]() {
      auto keep = [&](const std::string& id) {
        if (id.empty()) return false;
        if (faction_has_tech(fac, id)) return false;
        return (content_.techs.find(id) != content_.techs.end());
      };
      fac.research_queue.erase(std::remove_if(fac.research_queue.begin(), fac.research_queue.end(),
                                             [&](const std::string& id) { return !keep(id); }),
                               fac.research_queue.end());
    };

    auto select_next_available = [&]() {
      clean_queue();
      fac.active_research_id.clear();
      fac.active_research_progress = 0.0;

      for (std::size_t i = 0; i < fac.research_queue.size(); ++i) {
        const std::string& id = fac.research_queue[i];
        const auto it = content_.techs.find(id);
        if (it == content_.techs.end()) continue;
        if (!prereqs_met(fac, it->second)) continue;

        fac.active_research_id = id;
        fac.active_research_progress = 0.0;
        fac.research_queue.erase(fac.research_queue.begin() + static_cast<std::ptrdiff_t>(i));
        return;
      }
    };

    if (!fac.active_research_id.empty()) {
      if (faction_has_tech(fac, fac.active_research_id)) {
        fac.active_research_id.clear();
        fac.active_research_progress = 0.0;
      } else {
        const auto it = content_.techs.find(fac.active_research_id);
        if (it == content_.techs.end()) {
          fac.active_research_id.clear();
          fac.active_research_progress = 0.0;
        } else if (!prereqs_met(fac, it->second)) {
          enqueue_unique(fac.active_research_id);
          fac.active_research_id.clear();
          fac.active_research_progress = 0.0;
        }
      }
    }

    if (fac.active_research_id.empty()) select_next_available();

    for (;;) {
      if (fac.active_research_id.empty()) break;
      const auto it2 = content_.techs.find(fac.active_research_id);
      if (it2 == content_.techs.end()) {
        fac.active_research_id.clear();
        fac.active_research_progress = 0.0;
        select_next_available();
        continue;
      }

      const TechDef& tech = it2->second;
      if (faction_has_tech(fac, tech.id)) {
        fac.active_research_id.clear();
        fac.active_research_progress = 0.0;
        select_next_available();
        continue;
      }

      if (!prereqs_met(fac, tech)) {
        enqueue_unique(tech.id);
        fac.active_research_id.clear();
        fac.active_research_progress = 0.0;
        select_next_available();
        continue;
      }

      const double remaining = std::max(0.0, tech.cost - fac.active_research_progress);

      if (remaining <= 0.0) {
        fac.known_techs.push_back(tech.id);
        for (const auto& eff : tech.effects) {
          if (eff.type == "unlock_component") {
            push_unique(fac.unlocked_components, eff.value);
          } else if (eff.type == "unlock_installation") {
            push_unique(fac.unlocked_installations, eff.value);
          }
        }
        {
          const std::string msg = "Research complete for " + fac.name + ": " + tech.name;
          log::info(msg);
          EventContext ctx;
          ctx.faction_id = fac.id;
          push_event(EventLevel::Info, EventCategory::Research, msg, ctx);
        }
        fac.active_research_id.clear();
        fac.active_research_progress = 0.0;
        select_next_available();
        continue;
      }

      if (fac.research_points <= 0.0) break;

      const double assist_mult = tech_assistance_multiplier_for_faction(fid, tech.id);
      const double eff_remaining = remaining / std::max(1e-9, assist_mult);
      const double spend = std::min(fac.research_points, eff_remaining);
      fac.research_points -= spend;
      fac.active_research_progress += spend * assist_mult;
    }
  }
}



void Simulation::tick_shipyards(double dt_days) {
  if (dt_days <= 0.0) return;
  NEBULA4X_TRACE_SCOPE("tick_shipyards", "sim.econ");
  const auto it_def = content_.installations.find("shipyard");
  if (it_def == content_.installations.end()) return;

  std::unordered_map<Id, FactionEconomyMultipliers> fac_mult;
  fac_mult.reserve(state_.factions.size());
  for (Id fid : sorted_keys(state_.factions)) {
    auto m = compute_faction_economy_multipliers(content_, state_.factions.at(fid));
    const double trade = trade_agreement_output_multiplier(state_, fid);
    m.industry *= trade;
    m.research *= trade;
    m.construction *= trade;
    m.shipyard *= trade;
    fac_mult.emplace(fid, m);
  }

  const FactionEconomyMultipliers default_mult;
  auto mult_for = [&](Id fid) -> const FactionEconomyMultipliers& {
    auto it = fac_mult.find(fid);
    if (it == fac_mult.end()) return default_mult;
    return it->second;
  };

  const InstallationDef& shipyard_def = it_def->second;
  const double base_rate = shipyard_def.build_rate_tons_per_day;
  if (base_rate <= 0.0) return;

  const auto& costs_per_ton = shipyard_def.build_costs_per_ton;

  auto max_build_by_minerals = [&](const Colony& colony, double desired_tons) {
    double max_tons = desired_tons;
    for (const auto& [mineral, cost_per_ton] : costs_per_ton) {
      if (cost_per_ton <= 0.0) continue;
      const auto it = colony.minerals.find(mineral);
      const double available = (it == colony.minerals.end()) ? 0.0 : it->second;
      max_tons = std::min(max_tons, available / cost_per_ton);
    }
    return max_tons;
  };

  auto consume_minerals = [&](Colony& colony, double built_tons) {
    for (const auto& [mineral, cost_per_ton] : costs_per_ton) {
      if (cost_per_ton <= 0.0) continue;
      const double cost = built_tons * cost_per_ton;
      colony.minerals[mineral] = std::max(0.0, colony.minerals[mineral] - cost);
    }
  };


  // --- Shipyard completion metadata (QoL) ---
  //
  // Shipyard BuildOrders can optionally carry post-completion instructions:
  //  - apply a ship automation profile
  //  - assign to a fleet
  //  - rally to a colony
  //
  // This is deliberately lightweight: it piggybacks on the existing build/refit
  // order pipeline and stays fully backward-compatible in save files.
  struct ShipyardMetaResult {
    bool profile_applied{false};
    bool fleet_assigned{false};
    bool rally_ordered{false};

    std::string profile_name;
    std::string fleet_name;
    std::string rally_colony_name;
  };

  auto apply_shipyard_metadata = [&](Ship& ship, const BuildOrder& bo, const Colony& source_colony) {
    ShipyardMetaResult r;

    EventContext ctx;
    ctx.faction_id = source_colony.faction_id;
    ctx.system_id = ship.system_id;
    ctx.ship_id = ship.id;
    ctx.colony_id = source_colony.id;

    // Apply ship automation profile.
    if (!bo.apply_ship_profile_name.empty()) {
      r.profile_name = bo.apply_ship_profile_name;
      auto* fac = find_ptr(state_.factions, ship.faction_id);
      if (!fac) {
        push_event(EventLevel::Warn, EventCategory::Shipyard,
                  "Shipyard order: could not apply ship profile (missing faction)", ctx);
      } else {
        auto itp = fac->ship_profiles.find(bo.apply_ship_profile_name);
        if (itp == fac->ship_profiles.end()) {
          push_event(EventLevel::Warn, EventCategory::Shipyard,
                    "Shipyard order: unknown ship profile '" + bo.apply_ship_profile_name + "'", ctx);
        } else {
          apply_ship_profile(ship, itp->second);
          r.profile_applied = true;
        }
      }
    }

    // Assign to fleet.
    if (bo.assign_to_fleet_id != kInvalidId) {
      r.fleet_name = std::to_string(static_cast<unsigned long long>(bo.assign_to_fleet_id));
      if (const auto* fl = find_ptr(state_.fleets, bo.assign_to_fleet_id)) {
        if (!fl->name.empty()) r.fleet_name = fl->name;
      }

      std::string err;
      if (add_ship_to_fleet(bo.assign_to_fleet_id, ship.id, &err)) {
        r.fleet_assigned = true;
      } else {
        push_event(EventLevel::Warn, EventCategory::Shipyard,
                  "Shipyard order: could not assign ship to fleet: " + err, ctx);
      }
    }

    // Rally (only if fleet assignment was not used successfully).
    if (!r.fleet_assigned && bo.rally_to_colony_id != kInvalidId) {
      const auto* rally_colony = find_ptr(state_.colonies, bo.rally_to_colony_id);
      if (!rally_colony) {
        push_event(EventLevel::Warn, EventCategory::Shipyard,
                  "Shipyard order: rally target colony not found", ctx);
      } else if (rally_colony->body_id == kInvalidId) {
        push_event(EventLevel::Warn, EventCategory::Shipyard,
                  "Shipyard order: rally target colony has invalid body_id", ctx);
      } else if (const auto* rally_body = find_ptr(state_.bodies, rally_colony->body_id); !rally_body) {
        push_event(EventLevel::Warn, EventCategory::Shipyard,
                  "Shipyard order: rally target colony body not found", ctx);
      } else {
        r.rally_colony_name = rally_colony->name;
        if (issue_move_to_body(ship.id, rally_body->id, /*restrict_to_discovered=*/true)) {
          r.rally_ordered = true;
        } else {
          push_event(EventLevel::Warn, EventCategory::Shipyard,
                    "Shipyard order: could not issue rally move order (no known route)", ctx);
        }
      }
    }

    return r;
  };



  // --- Auto-build ship design targets (auto-shipyards) ---
  //
  // Factions can define desired counts of ship designs to maintain in
  // Faction::ship_design_targets. The simulation will automatically enqueue
  // build orders (marked auto_queued=true) across the faction's colonies that
  // have shipyards, without touching manual orders.
  //
  // Targets count *existing ships* plus *manual new-build orders* across the
  // faction. Auto orders are adjusted to cover the remaining gap.
  for (Id fid : sorted_keys(state_.factions)) {
    auto& fac = state_.factions.at(fid);
    if (fac.ship_design_targets.empty()) continue;

    // Find all colonies belonging to this faction with at least one shipyard.
    //
    // Colonies may opt out of the faction-level ship design target auto-builder
    // via Colony::shipyard_auto_build_enabled. Those shipyards remain fully usable
    // for manual orders, but will not receive (or keep) auto_queued build orders.
    std::vector<Id> all_yard_colonies;
    std::vector<Id> enabled_yard_colonies;
    all_yard_colonies.reserve(state_.colonies.size());
    enabled_yard_colonies.reserve(state_.colonies.size());
    for (Id cid2 : sorted_keys(state_.colonies)) {
      const auto& colony = state_.colonies.at(cid2);
      if (colony.faction_id != fid) continue;
      const auto it_yard = colony.installations.find("shipyard");
      const int yards = (it_yard != colony.installations.end()) ? it_yard->second : 0;
      if (yards <= 0) continue;
      all_yard_colonies.push_back(cid2);
      if (colony.shipyard_auto_build_enabled) enabled_yard_colonies.push_back(cid2);
    }
    if (all_yard_colonies.empty()) continue;

    auto target_for = [&](const std::string& design_id) -> int {
      auto it = fac.ship_design_targets.find(design_id);
      if (it == fac.ship_design_targets.end()) return 0;
      return it->second;
    };

    auto can_cancel_auto = [&](const BuildOrder& bo) -> bool {
      if (!bo.auto_queued) return false;
      if (bo.is_refit()) return false;
      const ShipDesign* d = find_design(bo.design_id);
      if (!d) return true;
      const double initial = std::max(1.0, d->mass_tons);
      return bo.tons_remaining >= initial - 1e-9;
    };

    // Remove stale auto orders whose design is no longer targeted (or is invalid/unbuildable),
    // but never cancel an order that has already started construction.
    //
    // Additionally, colonies that have opted out of auto-build will have any *unstarted*
    // auto_queued orders canceled here (manual orders are never touched).
    for (Id cid2 : all_yard_colonies) {
      auto& colony = state_.colonies.at(cid2);
      const bool allow_auto_here = colony.shipyard_auto_build_enabled;

      for (int i = static_cast<int>(colony.shipyard_queue.size()) - 1; i >= 0; --i) {
        const auto& bo = colony.shipyard_queue[static_cast<size_t>(i)];
        if (!bo.auto_queued || bo.is_refit()) continue;

        const int t = target_for(bo.design_id);
        const bool design_ok = (find_design(bo.design_id) != nullptr) && is_design_buildable_for_faction(fid, bo.design_id);
        const bool should_remove = (!allow_auto_here) || (t <= 0) || (!design_ok);
        if (should_remove && can_cancel_auto(bo)) {
          colony.shipyard_queue.erase(colony.shipyard_queue.begin() + i);
        }
      }
    }


    // Count current ships and pending build orders by design.
    std::unordered_map<std::string, int> have;
    have.reserve(state_.ships.size());
    for (const auto& [sid, sh] : state_.ships) {
      if (sh.faction_id != fid) continue;
      if (sh.design_id.empty()) continue;
      have[sh.design_id] += 1;
    }

    std::unordered_map<std::string, int> manual_pending;
    std::unordered_map<std::string, int> auto_pending;
    for (Id cid2 : all_yard_colonies) {
      const auto& colony = state_.colonies.at(cid2);
      for (const auto& bo : colony.shipyard_queue) {
        if (bo.is_refit()) continue;
        if (bo.design_id.empty()) continue;
        if (bo.auto_queued) auto_pending[bo.design_id] += 1;
        else manual_pending[bo.design_id] += 1;
      }
    }

    auto yard_eta = [&](Id cid2) -> double {
      const auto& colony = state_.colonies.at(cid2);
      const auto it_yard = colony.installations.find("shipyard");
      const int yards = (it_yard != colony.installations.end()) ? it_yard->second : 0;
      const double shipyard_mult = std::max(0.0, mult_for(fid).shipyard) *
                                 colony_condition_multipliers(colony).shipyard *
                                 colony_stability_output_multiplier_for_colony(colony);
      const double prosperity = trade_prosperity_output_multiplier_for_colony(cid2);
      const double blockade = cfg_.enable_blockades ? blockade_output_multiplier_for_colony(cid2) : 1.0;
      const double rate = base_rate * static_cast<double>(yards) * shipyard_mult * prosperity * blockade;
      if (rate <= 1e-9) return std::numeric_limits<double>::infinity();

      double load_tons = 0.0;
      for (const auto& bo : colony.shipyard_queue) {
        load_tons += std::max(0.0, bo.tons_remaining);
      }
      return load_tons / rate;
    };

    auto pick_best_yard = [&]() -> Id {
      Id best = kInvalidId;
      double best_eta = std::numeric_limits<double>::infinity();
      for (Id cid2 : enabled_yard_colonies) {
        const double eta = yard_eta(cid2);
        if (eta < best_eta - 1e-9 || (std::abs(eta - best_eta) <= 1e-9 && cid2 < best)) {
          best = cid2;
          best_eta = eta;
        }
      }
      return best;
    };

    // Ensure auto pending matches the target gap for each design.
    std::vector<std::string> design_ids;
    design_ids.reserve(fac.ship_design_targets.size());
    for (const auto& [did, t] : fac.ship_design_targets) {
      if (t > 0) design_ids.push_back(did);
    }
    std::sort(design_ids.begin(), design_ids.end());
    design_ids.erase(std::unique(design_ids.begin(), design_ids.end()), design_ids.end());

    for (const auto& design_id : design_ids) {
      const int target = std::max(0, target_for(design_id));
      if (target <= 0) continue;

      const ShipDesign* d = find_design(design_id);
      if (!d) continue;
      if (!is_design_buildable_for_faction(fid, design_id)) continue;

      const int have_n = (have.find(design_id) != have.end()) ? have[design_id] : 0;
      const int man_n = (manual_pending.find(design_id) != manual_pending.end()) ? manual_pending[design_id] : 0;
      const int cur_auto = (auto_pending.find(design_id) != auto_pending.end()) ? auto_pending[design_id] : 0;

      const int required_auto = std::max(0, target - (have_n + man_n));

      // Trim excess cancelable auto orders.
      int to_remove = std::max(0, cur_auto - required_auto);
      if (to_remove > 0) {
        for (auto it = all_yard_colonies.rbegin(); it != all_yard_colonies.rend() && to_remove > 0; ++it) {
          auto& colony = state_.colonies.at(*it);
          for (int i = static_cast<int>(colony.shipyard_queue.size()) - 1; i >= 0 && to_remove > 0; --i) {
            const auto& bo = colony.shipyard_queue[static_cast<size_t>(i)];
            if (!bo.auto_queued || bo.is_refit()) continue;
            if (bo.design_id != design_id) continue;
            if (!can_cancel_auto(bo)) continue;
            colony.shipyard_queue.erase(colony.shipyard_queue.begin() + i);
            to_remove -= 1;
            auto_pending[design_id] -= 1;
          }
        }
      }

      // Add missing auto orders.
      const int now_auto = (auto_pending.find(design_id) != auto_pending.end()) ? auto_pending[design_id] : 0;
      int to_add = std::max(0, required_auto - now_auto);
      if (to_add <= 0) continue;

      const double initial_tons = std::max(1.0, d->mass_tons);
      for (int k = 0; k < to_add; ++k) {
        const Id best = pick_best_yard();
        if (best == kInvalidId) break;
        auto& colony = state_.colonies.at(best);
        BuildOrder bo;
        bo.design_id = design_id;
        bo.tons_remaining = initial_tons;
        bo.auto_queued = true;
        colony.shipyard_queue.push_back(bo);
        auto_pending[design_id] += 1;
      }
    }
  }

  for (Id cid : sorted_keys(state_.colonies)) {
    auto& colony = state_.colonies.at(cid);
    const auto it_yard = colony.installations.find("shipyard");
    const int yards = (it_yard != colony.installations.end()) ? std::max(0, it_yard->second) : 0;
    if (yards <= 0) continue;
    if (colony.shipyard_queue.empty()) continue;

    const double shipyard_mult = std::max(0.0, mult_for(colony.faction_id).shipyard) *
                                 colony_condition_multipliers(colony).shipyard *
                                 colony_stability_output_multiplier_for_colony(colony);
    const double prosperity = trade_prosperity_output_multiplier_for_colony(colony.id);
    const double blockade = cfg_.enable_blockades ? blockade_output_multiplier_for_colony(colony.id) : 1.0;
    const double per_team_capacity_tons = base_rate * shipyard_mult * prosperity * blockade * dt_days;
    if (per_team_capacity_tons <= 1e-9) continue;

    // Pre-clean invalid orders so they don't permanently stall shipyard progress.
    //
    // This is especially important now that shipyards can process multiple
    // orders per tick: one bad refit order should not block all other work.
    for (int i = static_cast<int>(colony.shipyard_queue.size()) - 1; i >= 0; --i) {
      const auto& bo = colony.shipyard_queue[static_cast<size_t>(i)];
      const bool is_refit = bo.is_refit();

      if (bo.design_id.empty() || !find_design(bo.design_id)) {
        const std::string msg =
            std::string("Dropping shipyard order with unknown design: ") +
            (bo.design_id.empty() ? "<empty>" : bo.design_id) + " at " + colony.name;
        nebula4x::log::warn(msg);
        EventContext ctx;
        ctx.faction_id = colony.faction_id;
        ctx.colony_id = colony.id;
        if (is_refit) ctx.ship_id = bo.refit_ship_id;
        push_event(EventLevel::Warn, EventCategory::Shipyard, msg, ctx);
        colony.shipyard_queue.erase(colony.shipyard_queue.begin() + i);
        continue;
      }

      if (is_refit) {
        Ship* refit_ship = find_ptr(state_.ships, bo.refit_ship_id);
        if (!refit_ship) {
          const std::string msg = "Shipyard refit target ship not found; dropping order at " + colony.name;
          nebula4x::log::warn(msg);
          EventContext ctx;
          ctx.faction_id = colony.faction_id;
          ctx.colony_id = colony.id;
          push_event(EventLevel::Warn, EventCategory::Shipyard, msg, ctx);
          colony.shipyard_queue.erase(colony.shipyard_queue.begin() + i);
          continue;
        }

        if (refit_ship->faction_id != colony.faction_id) {
          const std::string msg = "Shipyard refit target ship faction mismatch; dropping order at " + colony.name;
          nebula4x::log::warn(msg);
          EventContext ctx;
          ctx.faction_id = colony.faction_id;
          ctx.colony_id = colony.id;
          ctx.ship_id = refit_ship->id;
          push_event(EventLevel::Warn, EventCategory::Shipyard, msg, ctx);
          colony.shipyard_queue.erase(colony.shipyard_queue.begin() + i);
          continue;
        }
      }
    }

    if (colony.shipyard_queue.empty()) continue;

    const auto* body = find_ptr(state_.bodies, colony.body_id);

    // --- Shipyard "team" allocation ---
    //
    // Old behavior: all shipyard capacity pooled onto the front order.
    // New behavior: shipyards can work on multiple orders per tick.
    //
    // Model:
    // - Each shipyard installation provides one build "team".
    // - Teams are assigned in queue order to workable orders (skipping stalled refits).
    // - Any remaining teams are pooled onto the first workable order (preserves the
    //   ability to focus capacity when the queue is short).
    std::vector<int> teams_for_order(colony.shipyard_queue.size(), 0);
    int teams_assigned = 0;
    int first_workable = -1;

    auto order_workable = [&](const BuildOrder& bo) -> bool {
      if (bo.tons_remaining <= 1e-9) return false;
      if (!bo.is_refit()) return true;

      if (!body) return false;
      Ship* refit_ship = find_ptr(state_.ships, bo.refit_ship_id);
      if (!refit_ship) return false;
      if (refit_ship->faction_id != colony.faction_id) return false;
      return is_ship_docked_at_colony(refit_ship->id, colony.id);
    };

    for (size_t i = 0; i < colony.shipyard_queue.size() && teams_assigned < yards; ++i) {
      if (!order_workable(colony.shipyard_queue[i])) continue;
      teams_for_order[i] = 1;
      if (first_workable < 0) first_workable = static_cast<int>(i);
      teams_assigned += 1;
    }

    if (first_workable >= 0 && teams_assigned < yards) {
      teams_for_order[static_cast<size_t>(first_workable)] += (yards - teams_assigned);
    }

    // --- Apply build progress (possibly to multiple orders) ---
    for (size_t i = 0; i < colony.shipyard_queue.size(); ++i) {
      const int teams_here = teams_for_order[i];
      if (teams_here <= 0) continue;

      auto& bo = colony.shipyard_queue[i];
      if (bo.tons_remaining <= 1e-9) continue;

      const double capacity_tons = per_team_capacity_tons * static_cast<double>(teams_here);
      if (capacity_tons <= 1e-9) continue;

      Ship* refit_ship = nullptr;
      if (bo.is_refit()) {
        if (!body) continue;
        refit_ship = find_ptr(state_.ships, bo.refit_ship_id);
        if (!refit_ship) continue;
        if (refit_ship->faction_id != colony.faction_id) continue;
        if (!is_ship_docked_at_colony(refit_ship->id, colony.id)) continue;

        // Prototype drydock behavior: refitting ships are pinned to the colony body and cannot
        // execute other queued orders while their refit is being processed.
        refit_ship->position_mkm = body->position_mkm;
        state_.ship_orders[refit_ship->id].queue.clear();
        refit_ship->auto_explore = false;
        refit_ship->auto_freight = false;
      }

      double build_tons = std::min(capacity_tons, bo.tons_remaining);

      if (!costs_per_ton.empty()) {
        build_tons = max_build_by_minerals(colony, build_tons);
      }

      if (build_tons <= 1e-9) continue;

      if (!costs_per_ton.empty()) consume_minerals(colony, build_tons);
      bo.tons_remaining -= build_tons;
    }

    // --- Completion pass ---
    //
    // Multiple orders may complete in a single tick now (e.g. multiple shipyards / time warp),
    // so we resolve all finished orders rather than only checking the front.
    for (size_t i = 0; i < colony.shipyard_queue.size(); /*increment inside*/) {
      auto& bo = colony.shipyard_queue[i];
      if (bo.tons_remaining > 1e-9) {
        ++i;
        continue;
      }

      const bool is_refit = bo.is_refit();

      if (is_refit) {
        if (!body) {
          const std::string msg = "Shipyard refit failed (missing colony body): " + colony.name;
          nebula4x::log::error(msg);
          EventContext ctx;
          ctx.faction_id = colony.faction_id;
          ctx.colony_id = colony.id;
          push_event(EventLevel::Error, EventCategory::Shipyard, msg, ctx);
          colony.shipyard_queue.erase(colony.shipyard_queue.begin() + i);
          continue;
        }

        Ship* refit_ship = find_ptr(state_.ships, bo.refit_ship_id);
        if (!refit_ship) {
          const std::string msg = "Shipyard refit target ship not found; dropping order at " + colony.name;
          nebula4x::log::warn(msg);
          EventContext ctx;
          ctx.faction_id = colony.faction_id;
          ctx.colony_id = colony.id;
          push_event(EventLevel::Warn, EventCategory::Shipyard, msg, ctx);
          colony.shipyard_queue.erase(colony.shipyard_queue.begin() + i);
          continue;
        }

        if (refit_ship->faction_id != colony.faction_id) {
          const std::string msg = "Shipyard refit target ship faction mismatch; dropping order at " + colony.name;
          nebula4x::log::warn(msg);
          EventContext ctx;
          ctx.faction_id = colony.faction_id;
          ctx.colony_id = colony.id;
          ctx.ship_id = refit_ship->id;
          push_event(EventLevel::Warn, EventCategory::Shipyard, msg, ctx);
          colony.shipyard_queue.erase(colony.shipyard_queue.begin() + i);
          continue;
        }

        // If the ship isn't docked, keep the order (it will resume once docked).
        if (!is_ship_docked_at_colony(refit_ship->id, colony.id)) {
          ++i;
          continue;
        }

        const auto* target = find_design(bo.design_id);
        if (!target) {
          const std::string msg = std::string("Shipyard refit failed (unknown design): ") + bo.design_id;
          nebula4x::log::warn(msg);
          EventContext ctx;
          ctx.faction_id = colony.faction_id;
          ctx.colony_id = colony.id;
          ctx.ship_id = refit_ship->id;
          push_event(EventLevel::Warn, EventCategory::Shipyard, msg, ctx);
          colony.shipyard_queue.erase(colony.shipyard_queue.begin() + i);
          continue;
        }

        // Apply the new design. Treat a completed refit as a full overhaul (fully repaired).
        refit_ship->design_id = bo.design_id;
        refit_ship->hp = std::max(1.0, target->max_hp);
        apply_design_stats_to_ship(*refit_ship);

        refit_ship->position_mkm = body->position_mkm;

        // If the refit reduces cargo capacity, move excess cargo back into colony stockpiles.
        const double cap = std::max(0.0, target->cargo_tons);
        double used = 0.0;
        for (const auto& [_, tons] : refit_ship->cargo) used += std::max(0.0, tons);

        if (used > cap + 1e-9) {
          double excess = used - cap;
          for (const auto& mineral : sorted_keys(refit_ship->cargo)) {
            if (excess <= 1e-9) break;
            auto it = refit_ship->cargo.find(mineral);
            if (it == refit_ship->cargo.end()) continue;
            const double have = std::max(0.0, it->second);
            if (have <= 1e-9) continue;

            const double move = std::min(have, excess);
            it->second -= move;
            colony.minerals[mineral] += move;
            excess -= move;

            if (it->second <= 1e-9) refit_ship->cargo.erase(it);
          }
        }

        const auto meta = apply_shipyard_metadata(*refit_ship, bo, colony);

        std::string msg =
            "Refit ship " + refit_ship->name + " -> " + target->name + " (" + refit_ship->design_id + ") at " + colony.name;
        if (meta.profile_applied) msg += " [Profile:" + meta.profile_name + "]";
        if (meta.fleet_assigned) msg += " [Fleet:" + meta.fleet_name + "]";
        if (meta.rally_ordered) msg += " [Rally:" + meta.rally_colony_name + "]";

        nebula4x::log::info(msg);
        EventContext ctx;
        ctx.faction_id = colony.faction_id;
        ctx.system_id = refit_ship->system_id;
        ctx.ship_id = refit_ship->id;
        ctx.colony_id = colony.id;
        push_event(EventLevel::Info, EventCategory::Shipyard, msg, ctx);

        colony.shipyard_queue.erase(colony.shipyard_queue.begin() + i);
        continue;
      }

      // Build new ship.
      const auto* design = find_design(bo.design_id);
      if (!design) {
        const std::string msg = std::string("Unknown design in build queue: ") + bo.design_id;
        nebula4x::log::warn(msg);
        EventContext ctx;
        ctx.faction_id = colony.faction_id;
        ctx.colony_id = colony.id;
        push_event(EventLevel::Warn, EventCategory::Shipyard, msg, ctx);
        colony.shipyard_queue.erase(colony.shipyard_queue.begin() + i);
        continue;
      }

      if (!body) {
        const std::string msg = "Shipyard build failed (missing colony body): " + colony.name;
        nebula4x::log::error(msg);
        EventContext ctx;
        ctx.faction_id = colony.faction_id;
        ctx.colony_id = colony.id;
        push_event(EventLevel::Error, EventCategory::Shipyard, msg, ctx);
        colony.shipyard_queue.erase(colony.shipyard_queue.begin() + i);
        continue;
      }

      if (auto* sys = find_ptr(state_.systems, body->system_id); !sys) {
        const std::string msg = "Shipyard build failed (missing system): colony=" + colony.name;
        nebula4x::log::error(msg);
        EventContext ctx;
        ctx.faction_id = colony.faction_id;
        ctx.colony_id = colony.id;
        push_event(EventLevel::Error, EventCategory::Shipyard, msg, ctx);
        colony.shipyard_queue.erase(colony.shipyard_queue.begin() + i);
        continue;
      }

      Ship sh;
      sh.id = allocate_id(state_);
      sh.faction_id = colony.faction_id;
      sh.system_id = body->system_id;
      sh.design_id = bo.design_id;
      sh.position_mkm = body->position_mkm;
      sh.fuel_tons = 0.0;
      apply_design_stats_to_ship(sh);
      sh.name = design->name + " #" + std::to_string(sh.id);
      state_.ships[sh.id] = sh;
      state_.ship_orders[sh.id] = ShipOrders{};
      state_.systems[sh.system_id].ships.push_back(sh.id);

      ShipyardMetaResult meta;
      if (auto* built = find_ptr(state_.ships, sh.id)) {
        meta = apply_shipyard_metadata(*built, bo, colony);
      }

      std::string msg = "Built ship " + sh.name + " (" + sh.design_id + ") at " + colony.name;
      if (meta.profile_applied) msg += " [Profile:" + meta.profile_name + "]";
      if (meta.fleet_assigned) msg += " [Fleet:" + meta.fleet_name + "]";
      if (meta.rally_ordered) msg += " [Rally:" + meta.rally_colony_name + "]";

      nebula4x::log::info(msg);
      EventContext ctx;
      ctx.faction_id = colony.faction_id;
      ctx.system_id = sh.system_id;
      ctx.ship_id = sh.id;
      ctx.colony_id = colony.id;
      push_event(EventLevel::Info, EventCategory::Shipyard, msg, ctx);

      colony.shipyard_queue.erase(colony.shipyard_queue.begin() + i);
    }
  }

}

void Simulation::tick_construction(double dt_days) {
  if (dt_days <= 0.0) return;
  NEBULA4X_TRACE_SCOPE("tick_construction", "sim.econ");
  for (Id cid : sorted_keys(state_.colonies)) {
    auto& colony = state_.colonies.at(cid);

    Id colony_system_id = kInvalidId;
    if (auto* b = find_ptr(state_.bodies, colony.body_id)) colony_system_id = b->system_id;

    double cp_available = construction_points_per_day(colony) * dt_days;
    if (cp_available <= 1e-9) continue;

    auto can_pay_minerals = [&](const InstallationDef& def) {
      for (const auto& [mineral, cost] : def.build_costs) {
        if (cost <= 0.0) continue;
        const auto it = colony.minerals.find(mineral);
        const double have = (it == colony.minerals.end()) ? 0.0 : it->second;
        if (have + 1e-9 < cost) return false;
      }
      return true;
    };

    auto pay_minerals = [&](const InstallationDef& def) {
      for (const auto& [mineral, cost] : def.build_costs) {
        if (cost <= 0.0) continue;
        colony.minerals[mineral] = std::max(0.0, colony.minerals[mineral] - cost);
      }
    };


    // Auto-build installation targets.
    //
    // Colony::installation_targets lets the player declare desired counts of
    // installations to maintain. The simulation will automatically manage
    // *auto-queued* construction orders to reach those counts.
    //
    // Rules:
    // - Manually-queued construction orders are never modified.
    // - Auto-queued orders are created/trimmed to match:
    //     target - (current installations + manual pending)
    // - Lowering/removing a target will only prune *pending* auto-queued units;
    //   it will not cancel a unit already in-progress (minerals paid or CP spent).
    if (!colony.installation_targets.empty()) {
      auto target_for = [&](const std::string& inst_id) -> int {
        auto it = colony.installation_targets.find(inst_id);
        if (it == colony.installation_targets.end()) return 0;
        return std::max(0, it->second);
      };

      auto committed_units = [&](const InstallationBuildOrder& ord) -> int {
        // If we're already building the current unit (minerals paid or CP started),
        // treat one unit as committed and do not prune it.
        const bool in_prog = ord.minerals_paid || ord.cp_remaining > 1e-9;
        return in_prog ? 1 : 0;
      };

      // 1) Prune auto-queued orders whose target is now zero/missing.
      for (int i = static_cast<int>(colony.construction_queue.size()) - 1; i >= 0; --i) {
        auto& ord = colony.construction_queue[static_cast<std::size_t>(i)];
        if (!ord.auto_queued) continue;
        if (target_for(ord.installation_id) > 0) continue;

        const int committed = std::min(std::max(0, ord.quantity_remaining), committed_units(ord));
        if (ord.quantity_remaining > committed) {
          ord.quantity_remaining = committed;
        }
        if (ord.quantity_remaining <= 0) {
          colony.construction_queue.erase(colony.construction_queue.begin() + i);
        }
      }

      // 2) Compute pending quantities by installation id, split by manual vs auto.
      std::unordered_map<std::string, int> manual_pending;
      std::unordered_map<std::string, int> auto_pending;
      manual_pending.reserve(colony.construction_queue.size() * 2);
      auto_pending.reserve(colony.construction_queue.size() * 2);

      for (const auto& ord : colony.construction_queue) {
        if (ord.installation_id.empty()) continue;
        const int qty = std::max(0, ord.quantity_remaining);
        if (qty <= 0) continue;
        if (ord.auto_queued) {
          auto_pending[ord.installation_id] += qty;
        } else {
          manual_pending[ord.installation_id] += qty;
        }
      }

      // Sorted keys for determinism.
      std::vector<std::string> ids;
      ids.reserve(colony.installation_targets.size());
      for (const auto& [inst_id, _] : colony.installation_targets) ids.push_back(inst_id);
      std::sort(ids.begin(), ids.end());
      ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

      for (const auto& inst_id : ids) {
        if (inst_id.empty()) continue;
        const int target = target_for(inst_id);
        if (target <= 0) continue;

        const int have = [&]() -> int {
          auto it = colony.installations.find(inst_id);
          return (it == colony.installations.end()) ? 0 : std::max(0, it->second);
        }();

        const int man = manual_pending[inst_id];
        const int aut = auto_pending[inst_id];

        const int required_auto = std::max(0, target - (have + man));

        // 3) Trim excess auto-queued units for this installation id.
        if (aut > required_auto) {
          int remove = aut - required_auto;
          for (int i = static_cast<int>(colony.construction_queue.size()) - 1; i >= 0 && remove > 0; --i) {
            auto& ord = colony.construction_queue[static_cast<std::size_t>(i)];
            if (!ord.auto_queued) continue;
            if (ord.installation_id != inst_id) continue;

            const int committed = std::min(std::max(0, ord.quantity_remaining), committed_units(ord));
            const int cancelable = std::max(0, ord.quantity_remaining - committed);
            if (cancelable <= 0) continue;

            const int take = std::min(cancelable, remove);
            ord.quantity_remaining -= take;
            remove -= take;

            if (ord.quantity_remaining <= 0) {
              colony.construction_queue.erase(colony.construction_queue.begin() + i);
            }
          }
        }

        // 4) Add missing auto-queued units.
        //
        // Recompute current auto pending for this id after trimming.
        int aut_after = 0;
        for (const auto& ord : colony.construction_queue) {
          if (!ord.auto_queued) continue;
          if (ord.installation_id != inst_id) continue;
          aut_after += std::max(0, ord.quantity_remaining);
        }

        const int missing = std::max(0, required_auto - aut_after);
        if (missing > 0) {
          // Only auto-queue installations the faction can build.
          if (is_installation_buildable_for_faction(colony.faction_id, inst_id)) {
            InstallationBuildOrder ord;
            ord.installation_id = inst_id;
            ord.quantity_remaining = missing;
            ord.auto_queued = true;
            colony.construction_queue.push_back(ord);
          }
        }
      }
    }

    // Construction queue processing:
    //
    // Previous behavior was strictly "front-of-queue only" which meant a single
    // unaffordable order (missing minerals) could block the entire queue forever.
    //
    // New behavior:
    // - The sim will *skip* stalled orders (can't pay minerals) and continue trying
    //   later orders in the same day. This prevents total queue lock-ups.
    // - If construction points remain, the sim may also apply CP to multiple queued
    //   orders in a single day (a simple form of parallelization).
    //
    // This keeps the model simple while making colony production far less brittle.
    auto& q = colony.construction_queue;

    int safety_steps = 0;
    constexpr int kMaxSteps = 100000;

    while (cp_available > 1e-9 && !q.empty() && safety_steps++ < kMaxSteps) {
      bool progressed_any = false;

      for (std::size_t i = 0; i < q.size() && cp_available > 1e-9;) {
        auto& ord = q[i];

        if (ord.quantity_remaining <= 0) {
          q.erase(q.begin() + static_cast<std::ptrdiff_t>(i));
          progressed_any = true;
          continue;
        }

        auto it_def = content_.installations.find(ord.installation_id);
        if (it_def == content_.installations.end()) {
          q.erase(q.begin() + static_cast<std::ptrdiff_t>(i));
          progressed_any = true;
          continue;
        }
        const InstallationDef& def = it_def->second;

        auto complete_one = [&]() {
          colony.installations[def.id] += 1;
          ord.quantity_remaining -= 1;
          ord.minerals_paid = false;
          ord.cp_remaining = 0.0;

          const std::string msg = "Constructed " + def.name + " at " + colony.name;
          EventContext ctx;
          ctx.faction_id = colony.faction_id;
          ctx.system_id = colony_system_id;
          ctx.colony_id = colony.id;
          push_event(EventLevel::Info, EventCategory::Construction, msg, ctx);

          progressed_any = true;

          if (ord.quantity_remaining <= 0) {
            q.erase(q.begin() + static_cast<std::ptrdiff_t>(i));
            return;
          }

          // Keep i the same so we can immediately attempt the next unit of this
          // same order in the same day (if we still have CP and minerals).
        };

        // If we haven't started the current unit, attempt to pay minerals.
        if (!ord.minerals_paid) {
          if (!can_pay_minerals(def)) {
            // Stalled: skip this order for now (do not block the whole queue).
            ++i;
            continue;
          }

          pay_minerals(def);
          ord.minerals_paid = true;
          ord.cp_remaining = std::max(0.0, def.construction_cost);
          progressed_any = true;

          if (ord.cp_remaining <= 1e-9) {
            // Instant build (0 CP cost).
            complete_one();
            continue;
          }
        } else {
          // Defensive repair: if an in-progress unit was loaded with cp_remaining == 0
          // but the definition has a CP cost, restore the remaining CP from the def.
          if (ord.cp_remaining <= 1e-9 && def.construction_cost > 0.0) {
            ord.cp_remaining = def.construction_cost;
          }
        }

        // Spend CP on the in-progress unit.
        if (ord.minerals_paid && ord.cp_remaining > 1e-9) {
          const double spend = std::min(cp_available, ord.cp_remaining);
          ord.cp_remaining -= spend;
          cp_available -= spend;
          progressed_any = true;

          if (ord.cp_remaining <= 1e-9) {
            complete_one();
            continue;
          }
        }

        ++i;
      }

      // If we made no progress in an entire scan of the queue, stop to avoid an
      // infinite loop (e.g. all remaining orders are stalled on minerals).
      if (!progressed_any) break;
    }
  }
}



} // namespace nebula4x
