#include "nebula4x/core/simulation.h"

#include "simulation_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nebula4x {
namespace {

double clamp01(double x) {
  if (!std::isfinite(x)) return 1.0;
  return std::clamp(x, 0.0, 1.0);
}

double safe_nonneg(double x) {
  if (!std::isfinite(x)) return 0.0;
  return std::max(0.0, x);
}

double mode_range_multiplier(const SimConfig& cfg, SensorMode mode) {
  auto sane = [](double x) {
    if (!std::isfinite(x)) return 1.0;
    return std::clamp(x, 0.0, 100.0);
  };
  switch (mode) {
    case SensorMode::Passive: return sane(cfg.sensor_mode_passive_range_multiplier);
    case SensorMode::Active: return sane(cfg.sensor_mode_active_range_multiplier);
    case SensorMode::Normal: default: return 1.0;
  }
}

struct CommandMeshSource {
  Id faction_id{kInvalidId};
  Vec2 pos_mkm{0.0, 0.0};
  double range_mkm{0.0};
  double range_sq_mkm{0.0};
  double inv_range_sq_mkm{0.0};
  double strength{1.0};   // source reliability / coherence (0..1)
  std::uint64_t los_seed{0};
  bool backbone{false};   // true for static colony uplinks
};

struct FactionPairKey {
  Id a{kInvalidId};
  Id b{kInvalidId};
  bool operator==(const FactionPairKey& o) const { return a == o.a && b == o.b; }
};

struct FactionPairKeyHash {
  std::size_t operator()(const FactionPairKey& k) const noexcept {
    std::size_t h = std::hash<Id>{}(k.a);
    h ^= std::hash<Id>{}(k.b) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
  }
};

} // namespace

void Simulation::ensure_command_mesh_cache_current() const {
  if (!cfg_.enable_command_mesh) {
    invalidate_command_mesh_cache();
    return;
  }

  const auto day = state_.date.days_since_epoch();
  const int hour = std::clamp(state_.hour_of_day, 0, 23);
  const auto prev_day = command_mesh_cache_day_;
  const int prev_hour = command_mesh_cache_hour_;
  const bool had_prev_cache = command_mesh_cache_valid_;
  const bool can_blend_with_prior = had_prev_cache && command_mesh_cache_state_generation_ == state_generation_ &&
                                    command_mesh_cache_content_generation_ == content_generation_;

  if (command_mesh_cache_valid_ && command_mesh_cache_day_ == day && command_mesh_cache_hour_ == hour &&
      command_mesh_cache_state_generation_ == state_generation_ &&
      command_mesh_cache_content_generation_ == content_generation_) {
    return;
  }

  std::unordered_map<Id, double> prior_coverage_cache;
  if (can_blend_with_prior && !command_mesh_coverage_cache_.empty()) {
    prior_coverage_cache.swap(command_mesh_coverage_cache_);
  }

  std::int64_t elapsed_hours = 1;
  if (can_blend_with_prior) {
    const std::int64_t day_delta = day - prev_day;
    const std::int64_t hour_delta = static_cast<std::int64_t>(hour - prev_hour);
    elapsed_hours = std::clamp(day_delta * 24 + hour_delta, std::int64_t{1}, std::int64_t{24 * 14});
  }

  command_mesh_cache_valid_ = true;
  command_mesh_cache_day_ = day;
  command_mesh_cache_hour_ = hour;
  command_mesh_cache_state_generation_ = state_generation_;
  command_mesh_cache_content_generation_ = content_generation_;

  command_mesh_coverage_cache_.clear();
  command_mesh_coverage_cache_.reserve(state_.ships.size() * 2 + 8);

  const double falloff_exp = safe_nonneg(cfg_.command_mesh_range_falloff_exponent);
  const double colony_base = safe_nonneg(cfg_.command_mesh_colony_base_range_mkm);
  const double colony_sensor_scale = safe_nonneg(cfg_.command_mesh_colony_sensor_scale);
  const double ship_sensor_scale = safe_nonneg(cfg_.command_mesh_ship_relay_sensor_scale);
  const double jam = std::clamp(cfg_.command_mesh_environment_jamming_strength, 0.0, 1.0);
  const bool exp_is_1 = std::fabs(falloff_exp - 1.0) < 1e-6;
  const bool exp_is_2 = std::fabs(falloff_exp - 2.0) < 1e-6;
  constexpr double kSourceCutoffRatio = 3.5;
  const double cutoff_ratio_sq = kSourceCutoffRatio * kSourceCutoffRatio;
  constexpr double kEnvFloor = 0.05;
  const bool use_los = cfg_.enable_sensor_los_attenuation;
  const std::size_t source_soft_eval_cap = use_los ? std::size_t{10} : std::size_t{20};
  const std::size_t source_hard_eval_cap = use_los ? std::size_t{24} : std::size_t{64};

  std::unordered_map<Id, std::vector<CommandMeshSource>> sources_by_system;
  sources_by_system.reserve(state_.systems.size() * 2 + 8);

  std::unordered_map<FactionPairKey, bool, FactionPairKeyHash> partner_cache;
  partner_cache.reserve(state_.factions.size() * 8 + 16);

  std::unordered_map<Id, std::pair<double, double>> backbone_uplink_cache;
  backbone_uplink_cache.reserve(state_.ships.size() * 2 + 8);

  auto are_partners_cached = [&](Id fa, Id fb) {
    if (fa == kInvalidId || fb == kInvalidId) return false;
    if (fa == fb) return true;
    if (fb < fa) std::swap(fa, fb);
    const FactionPairKey key{fa, fb};
    if (auto it = partner_cache.find(key); it != partner_cache.end()) return it->second;
    const bool v = are_factions_trade_partners(fa, fb);
    partner_cache.emplace(key, v);
    return v;
  };

  auto crew_autonomy_factor = [&](const Ship& ship) {
    double gp = ship.crew_grade_points;
    if (!std::isfinite(gp) || gp < 0.0) gp = cfg_.crew_initial_grade_points;
    const double eff = std::clamp(1.0 + crew_grade_bonus_for_points(gp), 0.0, 1.75);
    double autonomy = std::clamp(eff / 1.75, 0.0, 1.0);

    double comp = ship.crew_complement;
    if (!std::isfinite(comp)) comp = 1.0;
    autonomy *= std::sqrt(std::clamp(comp, 0.0, 1.0));
    return std::clamp(autonomy, 0.0, 1.0);
  };

  auto add_source = [&](Id system_id, Id faction_id, const Vec2& pos_mkm, double range_mkm, double strength,
                        bool backbone) {
    if (system_id == kInvalidId || faction_id == kInvalidId) return;
    if (!std::isfinite(range_mkm) || range_mkm <= 1e-9) return;
    if (!std::isfinite(strength) || strength <= 1e-9) return;
    const double range_sq = range_mkm * range_mkm;
    const double inv_range_sq = (range_sq > 1e-12) ? (1.0 / range_sq) : 0.0;

    std::uint64_t los_seed = 0xD8E4B16C4F77A9D3ULL;
    los_seed ^= static_cast<std::uint64_t>(system_id) * 0x9E3779B97F4A7C15ULL;
    los_seed ^= static_cast<std::uint64_t>(std::hash<Id>{}(faction_id)) * 0xBF58476D1CE4E5B9ULL;
    los_seed ^= backbone ? 0x94D049BB133111EBULL : 0x2545F4914F6CDD1DULL;
    const std::uint64_t qx = static_cast<std::uint64_t>(std::llround(pos_mkm.x * 8.0));
    const std::uint64_t qy = static_cast<std::uint64_t>(std::llround(pos_mkm.y * 8.0));
    los_seed ^= qx * 0xD6E8FEB86659FD93ULL;
    los_seed ^= qy * 0xA5CB9243F13F7A2DULL;
    los_seed ^= (los_seed >> 30);
    los_seed *= 0xBF58476D1CE4E5B9ULL;
    los_seed ^= (los_seed >> 27);
    los_seed *= 0x94D049BB133111EBULL;
    los_seed ^= (los_seed >> 31);

    auto& vec = sources_by_system[system_id];
    vec.push_back(CommandMeshSource{
        faction_id,
        pos_mkm,
        range_mkm,
        range_sq,
        inv_range_sq,
        std::clamp(strength, 0.0, 1.0),
        los_seed,
        backbone,
    });
  };

  auto link_quality = [&](Id receiver_system_id, const Vec2& receiver_pos, const CommandMeshSource& src) {
    const Vec2 delta = receiver_pos - src.pos_mkm;
    const double d2 = delta.x * delta.x + delta.y * delta.y;
    if (!std::isfinite(d2) || d2 < 0.0) return 0.0;
    if (src.range_sq_mkm <= 1e-12) return 0.0;
    if (d2 > src.range_sq_mkm * cutoff_ratio_sq) return 0.0;

    const double x2 = d2 * std::max(0.0, src.inv_range_sq_mkm);
    double q = 0.0;
    if (exp_is_2) {
      q = std::exp(-x2);
    } else if (exp_is_1) {
      q = std::exp(-std::sqrt(std::max(0.0, x2)));
    } else {
      q = std::exp(-std::pow(std::sqrt(std::max(0.0, x2)), falloff_exp));
    }

    q *= std::clamp(src.strength, 0.0, 1.0);

    if (use_los) {
      if (sim_internal::system_line_of_sight_blocked_by_bodies(state_, receiver_system_id, src.pos_mkm, receiver_pos,
                                                                0.0)) {
        return 0.0;
      }
      std::uint64_t extra_seed = src.los_seed ^ (static_cast<std::uint64_t>(receiver_system_id) * 0x9E3779B97F4A7C15ULL);
      const double los = std::clamp(system_sensor_environment_multiplier_los(receiver_system_id, src.pos_mkm,
                                                                             receiver_pos, extra_seed),
                                    0.0, 1.0);
      q *= los;
    }

    return std::clamp(q, 0.0, 1.0);
  };

  auto fused_coverage = [&](const std::vector<CommandMeshSource>& srcs, Id receiver_system_id, Id receiver_faction,
                            const Vec2& receiver_pos, bool backbone_only, double* out_redundancy) {
    double miss_prob = 1.0;
    double q_sum = 0.0;
    double q_max = 0.0;
    int contributors = 0;
    std::size_t processed = 0;

    for (const auto& src : srcs) {
      if (backbone_only && !src.backbone) continue;
      if (!are_partners_cached(receiver_faction, src.faction_id)) continue;
      ++processed;

      const double q = link_quality(receiver_system_id, receiver_pos, src);
      if (q <= 1e-9) continue;

      miss_prob *= (1.0 - q);
      q_sum += q;
      q_max = std::max(q_max, q);
      ++contributors;

      if (miss_prob <= 1e-4) break;
      if (processed >= source_hard_eval_cap) break;
      if (processed >= source_soft_eval_cap && miss_prob <= 0.35) break;
    }

    double coverage = std::clamp(1.0 - miss_prob, 0.0, 1.0);
    double redundancy = 0.0;
    if (contributors >= 2) {
      // Extra confidence from backup links (helps avoid brittle single-relay behavior).
      redundancy = std::clamp(q_sum - q_max, 0.0, 1.0);
      coverage = 1.0 - (1.0 - coverage) * (1.0 - 0.12 * redundancy);
      coverage = std::clamp(coverage, 0.0, 1.0);
    }
    if (out_redundancy) *out_redundancy = redundancy;
    return coverage;
  };

  auto fused_coverage_prefiltered = [&](Id receiver_system_id, const std::vector<const CommandMeshSource*>& srcs,
                                        const Vec2& receiver_pos, double* out_redundancy) {
    double miss_prob = 1.0;
    double q_sum = 0.0;
    double q_max = 0.0;
    int contributors = 0;
    std::size_t processed = 0;

    for (const CommandMeshSource* sptr : srcs) {
      if (!sptr) continue;
      const CommandMeshSource& src = *sptr;
      ++processed;
      const double q = link_quality(receiver_system_id, receiver_pos, src);
      if (q <= 1e-9) continue;

      miss_prob *= (1.0 - q);
      q_sum += q;
      q_max = std::max(q_max, q);
      ++contributors;
      if (miss_prob <= 1e-4) break;
      if (processed >= source_hard_eval_cap) break;
      if (processed >= source_soft_eval_cap && miss_prob <= 0.35) break;
    }

    double coverage = std::clamp(1.0 - miss_prob, 0.0, 1.0);
    double redundancy = 0.0;
    if (contributors >= 2) {
      redundancy = std::clamp(q_sum - q_max, 0.0, 1.0);
      coverage = 1.0 - (1.0 - coverage) * (1.0 - 0.12 * redundancy);
      coverage = std::clamp(coverage, 0.0, 1.0);
    }
    if (out_redundancy) *out_redundancy = redundancy;
    return coverage;
  };

  // Build deterministic ship membership per system from system ship lists.
  std::unordered_map<Id, std::vector<Id>> ships_by_system;
  ships_by_system.reserve(state_.systems.size() * 2 + 8);
  for (const auto& [sys_id, sys] : state_.systems) {
    std::vector<Id> ids = sys.ships;
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    if (!ids.empty()) ships_by_system.emplace(sys_id, std::move(ids));
  }
  if (ships_by_system.empty()) return;

  // Colony uplink sources.
  std::vector<Id> colony_ids;
  colony_ids.reserve(state_.colonies.size());
  for (const auto& [cid, _] : state_.colonies) colony_ids.push_back(cid);
  std::sort(colony_ids.begin(), colony_ids.end());
  for (Id cid : colony_ids) {
    const auto* cptr = find_ptr(state_.colonies, cid);
    if (!cptr) continue;
    const Colony& col = *cptr;
    const auto* body = find_ptr(state_.bodies, col.body_id);
    if (!body || body->system_id == kInvalidId) continue;

    double best_sensor_mkm = 0.0;
    for (const auto& [inst_id, count] : col.installations) {
      if (count <= 0) continue;
      const auto it = content_.installations.find(inst_id);
      if (it == content_.installations.end()) continue;
      best_sensor_mkm = std::max(best_sensor_mkm, safe_nonneg(it->second.sensor_range_mkm));
    }

    double range_mkm = colony_base + best_sensor_mkm * colony_sensor_scale;
    if (range_mkm <= 1e-9) continue;

    // Environment attenuates uplink range.
    const double env_mult = std::clamp(system_sensor_environment_multiplier_at(body->system_id, body->position_mkm),
                                       kEnvFloor, 1.0);
    range_mkm *= env_mult;

    const double sensor_strength = std::clamp(best_sensor_mkm / (best_sensor_mkm + 200.0), 0.0, 1.0);
    const double pop = safe_nonneg(col.population_millions);
    const double pop_strength = std::clamp(std::log1p(pop) / std::log1p(5000.0), 0.0, 1.0);
    const double source_strength = std::clamp(0.55 + 0.35 * sensor_strength + 0.10 * pop_strength, 0.35, 1.0);

    add_source(body->system_id, col.faction_id, body->position_mkm, range_mkm, source_strength, true);
  }

  // Ship relay sources are "tethered" to backbone uplinks: if a relay ship has
  // weak uplink to a colony network, it still helps locally but with reduced reach/reliability.
  std::vector<Id> source_system_ids;
  source_system_ids.reserve(ships_by_system.size());
  for (const auto& [sys_id, _] : ships_by_system) source_system_ids.push_back(sys_id);
  std::sort(source_system_ids.begin(), source_system_ids.end());
  for (Id sys_id : source_system_ids) {
    const auto it_ship_ids = ships_by_system.find(sys_id);
    if (it_ship_ids == ships_by_system.end()) continue;

    std::vector<const Ship*> relay_ships;
    relay_ships.reserve(it_ship_ids->second.size());
    std::vector<Id> relay_receiver_factions;
    relay_receiver_factions.reserve(it_ship_ids->second.size());
    for (Id sid : it_ship_ids->second) {
      const auto* sh = find_ptr(state_.ships, sid);
      if (!sh || sh->faction_id == kInvalidId || sh->system_id != sys_id) continue;
      relay_ships.push_back(sh);
      relay_receiver_factions.push_back(sh->faction_id);
    }
    if (relay_ships.empty()) continue;
    std::sort(relay_receiver_factions.begin(), relay_receiver_factions.end());
    relay_receiver_factions.erase(std::unique(relay_receiver_factions.begin(), relay_receiver_factions.end()),
                                  relay_receiver_factions.end());

    const auto it_backbone_sources = sources_by_system.find(sys_id);
    const bool has_backbone = (it_backbone_sources != sources_by_system.end() && !it_backbone_sources->second.empty());
    std::vector<CommandMeshSource> relay_backbone_sources;
    std::unordered_map<Id, std::vector<const CommandMeshSource*>> relay_backbone_sources_by_faction;
    if (has_backbone) {
      relay_backbone_sources = it_backbone_sources->second;
      relay_backbone_sources_by_faction.reserve(relay_receiver_factions.size() * 2 + 4);
      for (Id receiver_faction : relay_receiver_factions) {
        auto& vec = relay_backbone_sources_by_faction[receiver_faction];
        vec.reserve(relay_backbone_sources.size());
        for (const auto& src : relay_backbone_sources) {
          if (!are_partners_cached(receiver_faction, src.faction_id)) continue;
          vec.push_back(&src);
        }
      }
    }

    for (const Ship* sh : relay_ships) {
      if (!sh) continue;
      const Id sid = sh->id;

      const auto* d = find_design(sh->design_id);
      if (!d) continue;

      double relay_range_mkm = safe_nonneg(d->sensor_range_mkm);
      if (relay_range_mkm <= 1e-9) continue;

      // Relay requires powered sensors.
      const auto p = sim_internal::compute_power_allocation(*d, sh->power_policy);
      if (!p.sensors_online) continue;

      // Use raw source capabilities to avoid recursive command-quality coupling.
      relay_range_mkm *= mode_range_multiplier(cfg_, sh->sensor_mode);
      relay_range_mkm *= ship_heat_sensor_range_multiplier(*sh);
      relay_range_mkm *= clamp01(sh->sensors_integrity);
      relay_range_mkm *= ship_sensor_scale;
      if (!std::isfinite(relay_range_mkm) || relay_range_mkm <= 1e-9) continue;

      const double env_mult = std::clamp(system_sensor_environment_multiplier_at(sh->system_id, sh->position_mkm),
                                         kEnvFloor, 1.0);
      relay_range_mkm *= env_mult;
      if (!std::isfinite(relay_range_mkm) || relay_range_mkm <= 1e-9) continue;

      const double crew_factor = crew_autonomy_factor(*sh);
      double maint = sh->maintenance_condition;
      if (!std::isfinite(maint)) maint = 1.0;
      maint = std::clamp(maint, 0.0, 1.0);

      double relay_strength = std::clamp(0.35 + 0.35 * crew_factor + 0.30 * clamp01(sh->sensors_integrity), 0.20, 1.0);
      relay_strength *= std::clamp(0.45 + 0.55 * maint, 0.10, 1.0);

      double backbone_cov = 0.0;
      double backbone_redundancy = 0.0;
      if (has_backbone) {
        const auto it_relay_sources = relay_backbone_sources_by_faction.find(sh->faction_id);
        if (it_relay_sources != relay_backbone_sources_by_faction.end() && !it_relay_sources->second.empty()) {
          backbone_cov = fused_coverage_prefiltered(sh->system_id, it_relay_sources->second, sh->position_mkm,
                                                    &backbone_redundancy);
        } else {
          backbone_cov = fused_coverage(relay_backbone_sources, sh->system_id, sh->faction_id, sh->position_mkm, true,
                                        &backbone_redundancy);
        }
      }
      backbone_uplink_cache[sid] = {std::clamp(backbone_cov, 0.0, 1.0), std::clamp(backbone_redundancy, 0.0, 1.0)};

      // Uplink quality gates relay impact when an actual backbone exists.
      // If there is no backbone infrastructure at all, preserve legacy behavior:
      // relay sensors should still provide full local command quality.
      if (has_backbone) {
        const double tether = std::clamp(0.15 + 0.85 * backbone_cov, 0.15, 1.0);
        relay_range_mkm *= tether;
        relay_strength *= std::clamp(0.25 + 0.75 * backbone_cov + 0.20 * backbone_redundancy, 0.10, 1.0);
        if (backbone_cov <= 0.01 && backbone_redundancy <= 0.01) relay_strength *= 0.35;
      }

      add_source(sh->system_id, sh->faction_id, sh->position_mkm, relay_range_mkm, relay_strength, false);
    }
  }

  // Sort per-system source vectors so high-value links are visited first.
  // This improves early-out behavior and preserves deterministic outcomes.
  for (auto& [sys_id, srcs] : sources_by_system) {
    (void)sys_id;
    std::sort(srcs.begin(), srcs.end(), [](const CommandMeshSource& a, const CommandMeshSource& b) {
      const double wa = std::clamp(a.strength, 0.0, 1.0) * (a.backbone ? 1.25 : 1.0);
      const double wb = std::clamp(b.strength, 0.0, 1.0) * (b.backbone ? 1.25 : 1.0);
      const double sa = a.range_sq_mkm * wa;
      const double sb = b.range_sq_mkm * wb;
      if (sa != sb) return sa > sb;
      if (a.backbone != b.backbone) return a.backbone > b.backbone;
      if (a.faction_id != b.faction_id) return a.faction_id < b.faction_id;
      if (a.pos_mkm.x != b.pos_mkm.x) return a.pos_mkm.x < b.pos_mkm.x;
      return a.pos_mkm.y < b.pos_mkm.y;
    });

    // Deterministic source thinning: cap per-faction fan-in in dense battles to
    // keep command-mesh evaluation costs bounded without changing high-value links.
    constexpr std::size_t kMaxSourcesPerFaction = 28;
    constexpr std::size_t kMaxBackboneSourcesPerFaction = 14;
    if (srcs.size() > (kMaxSourcesPerFaction * 2)) {
      std::unordered_map<Id, std::size_t> kept_total;
      std::unordered_map<Id, std::size_t> kept_backbone;
      kept_total.reserve(srcs.size() / 2 + 8);
      kept_backbone.reserve(srcs.size() / 2 + 8);

      std::vector<CommandMeshSource> kept;
      kept.reserve(srcs.size());
      for (const auto& src : srcs) {
        std::size_t& total = kept_total[src.faction_id];
        std::size_t& backbone = kept_backbone[src.faction_id];
        const bool very_strong = src.strength >= 0.88;

        if (src.backbone) {
          if (backbone >= kMaxBackboneSourcesPerFaction && !very_strong) continue;
        }
        if (total >= kMaxSourcesPerFaction && !very_strong) continue;

        kept.push_back(src);
        ++total;
        if (src.backbone) ++backbone;
      }
      srcs.swap(kept);
    }
  }

  if (sources_by_system.empty()) {
    // No command-mesh infrastructure exists in this scenario/content set.
    // Fall back to legacy baseline behavior so ships are not globally
    // penalized by a missing optional mechanic.
    for (const auto& [sys_id, ship_ids] : ships_by_system) {
      (void)sys_id;
      for (Id sid : ship_ids) command_mesh_coverage_cache_[sid] = 1.0;
    }
    return;
  }

  // Resolve command mesh coverage for each ship (deterministic order).
  std::vector<Id> eval_system_ids;
  eval_system_ids.reserve(ships_by_system.size());
  for (const auto& [sys_id, _] : ships_by_system) eval_system_ids.push_back(sys_id);
  std::sort(eval_system_ids.begin(), eval_system_ids.end());
  for (Id sys_id : eval_system_ids) {
    const auto it_ship_ids = ships_by_system.find(sys_id);
    if (it_ship_ids == ships_by_system.end()) continue;

    std::vector<const Ship*> eval_ships;
    eval_ships.reserve(it_ship_ids->second.size());
    std::vector<Id> receiver_factions;
    receiver_factions.reserve(it_ship_ids->second.size());
    for (Id sid : it_ship_ids->second) {
      const auto* sh = find_ptr(state_.ships, sid);
      if (!sh || sh->faction_id == kInvalidId || sh->system_id != sys_id) continue;
      eval_ships.push_back(sh);
      receiver_factions.push_back(sh->faction_id);
    }
    if (eval_ships.empty()) continue;

    std::sort(receiver_factions.begin(), receiver_factions.end());
    receiver_factions.erase(std::unique(receiver_factions.begin(), receiver_factions.end()), receiver_factions.end());

    const auto it_sources = sources_by_system.find(sys_id);
    if (it_sources == sources_by_system.end()) {
      for (const Ship* sh : eval_ships) {
        if (!sh) continue;
        command_mesh_coverage_cache_[sh->id] = 1.0;
      }
      continue;
    }

    const auto& srcs = it_sources->second;
    std::unordered_map<Id, std::vector<const CommandMeshSource*>> faction_sources;
    std::unordered_map<Id, std::vector<const CommandMeshSource*>> faction_backbone_sources;
    faction_sources.reserve(receiver_factions.size() * 2 + 4);
    faction_backbone_sources.reserve(receiver_factions.size() * 2 + 4);

    for (Id faction_id : receiver_factions) {
      auto& all = faction_sources[faction_id];
      auto& backbones = faction_backbone_sources[faction_id];
      all.reserve(srcs.size());
      backbones.reserve(srcs.size());
      for (const auto& src : srcs) {
        if (!are_partners_cached(faction_id, src.faction_id)) continue;
        all.push_back(&src);
        if (src.backbone) backbones.push_back(&src);
      }
    }

    for (const Ship* sh : eval_ships) {
      if (!sh) continue;
      const Id sid = sh->id;

      const auto it_all = faction_sources.find(sh->faction_id);
      if (it_all == faction_sources.end() || it_all->second.empty()) {
        // No friendly mesh source is available for this ship right now.
        // Keep legacy baseline behavior instead of globally nerfing ship
        // subsystems in scenarios that do not opt into command infrastructure.
        command_mesh_coverage_cache_[sid] = 1.0;
        continue;
      }

      double mesh_redundancy = 0.0;
      const double mesh_cov = fused_coverage_prefiltered(sh->system_id, it_all->second, sh->position_mkm,
                                                         &mesh_redundancy);

      double backbone_cov = 0.0;
      double backbone_redundancy = 0.0;
      if (auto it = backbone_uplink_cache.find(sid); it != backbone_uplink_cache.end()) {
        backbone_cov = std::clamp(it->second.first, 0.0, 1.0);
        backbone_redundancy = std::clamp(it->second.second, 0.0, 1.0);
      } else {
        const auto it_backbones = faction_backbone_sources.find(sh->faction_id);
        if (it_backbones != faction_backbone_sources.end() && !it_backbones->second.empty()) {
          backbone_cov = fused_coverage_prefiltered(sh->system_id, it_backbones->second, sh->position_mkm,
                                                    &backbone_redundancy);
        }
      }

      // Ad-hoc relay coverage is strongest when tied into backbone links.
      const auto it_backbones_any = faction_backbone_sources.find(sh->faction_id);
      const bool has_backbone_links =
          (it_backbones_any != faction_backbone_sources.end() && !it_backbones_any->second.empty());
      const double ad_hoc_cov = std::max(0.0, mesh_cov - backbone_cov);
      const double ad_hoc_gate =
          has_backbone_links ? std::clamp(0.35 + 0.65 * backbone_cov + 0.25 * backbone_redundancy, 0.25, 1.0) : 1.0;
      double coverage = std::clamp(backbone_cov + ad_hoc_cov * ad_hoc_gate, 0.0, 1.0);

      // Receiver-side environment jamming; redundancy reduces fragility under interference.
      const double env_here = std::clamp(system_sensor_environment_multiplier_at(sh->system_id, sh->position_mkm),
                                         kEnvFloor, 1.0);
      const double redundancy = std::clamp(mesh_redundancy + 0.5 * backbone_redundancy, 0.0, 1.0);
      double jam_penalty = jam * (1.0 - env_here);
      jam_penalty *= (1.0 - 0.55 * redundancy);
      jam_penalty = std::clamp(jam_penalty, 0.0, 0.95);
      coverage *= (1.0 - jam_penalty);

      if (auto it_prev = prior_coverage_cache.find(sid); it_prev != prior_coverage_cache.end()) {
        const double prev = std::clamp(it_prev->second, 0.0, 1.0);
        const double time_relax = std::clamp(static_cast<double>(elapsed_hours) / 6.0, 0.0, 1.0);
        double alpha_up = std::clamp(0.55 + 0.45 * time_relax, 0.0, 1.0);
        double alpha_down = std::clamp(0.30 + 0.70 * time_relax, 0.0, 1.0);
        alpha_up *= std::clamp(0.80 + 0.20 * backbone_cov, 0.75, 1.0);
        alpha_down *= std::clamp(1.0 - 0.25 * redundancy, 0.70, 1.0);

        const double alpha = (coverage >= prev) ? alpha_up : alpha_down;
        coverage = prev + (coverage - prev) * std::clamp(alpha, 0.0, 1.0);
      }

      command_mesh_coverage_cache_[sid] = std::clamp(coverage, 0.0, 1.0);
    }
  }
}

void Simulation::invalidate_command_mesh_cache() const {
  command_mesh_cache_valid_ = false;
  command_mesh_coverage_cache_.clear();
  command_mesh_cache_day_ = state_.date.days_since_epoch();
  command_mesh_cache_hour_ = std::clamp(state_.hour_of_day, 0, 23);
  command_mesh_cache_state_generation_ = state_generation_;
  command_mesh_cache_content_generation_ = content_generation_;
}

double Simulation::ship_command_mesh_coverage(const Ship& ship) const {
  if (!cfg_.enable_command_mesh) return 1.0;
  if (ship.faction_id == kInvalidId || ship.system_id == kInvalidId) return 1.0;

  ensure_command_mesh_cache_current();
  if (auto it = command_mesh_coverage_cache_.find(ship.id); it != command_mesh_coverage_cache_.end()) {
    return std::clamp(it->second, 0.0, 1.0);
  }

  // Valid ship but no relay source in system this hour.
  return 0.0;
}

double Simulation::ship_command_mesh_quality(const Ship& ship) const {
  if (!cfg_.enable_command_mesh) return 1.0;

  const double coverage = ship_command_mesh_coverage(ship);

  // Crew autonomy: map combat effectiveness to [0..1].
  double gp = ship.crew_grade_points;
  if (!std::isfinite(gp) || gp < 0.0) gp = cfg_.crew_initial_grade_points;
  const double eff = std::clamp(1.0 + crew_grade_bonus_for_points(gp), 0.0, 1.75);
  double autonomy = eff / 1.75;

  if (cfg_.enable_crew_casualties) {
    double comp = ship.crew_complement;
    if (!std::isfinite(comp)) comp = 1.0;
    comp = std::clamp(comp, 0.0, 1.0);
    autonomy *= std::sqrt(comp);
  }

  const double blend = std::clamp(cfg_.command_mesh_autonomy_blend, 0.0, 1.0);
  const double q = coverage + (1.0 - coverage) * blend * autonomy;
  return std::clamp(q, 0.0, 1.0);
}

double Simulation::ship_command_efficiency_multiplier(const Ship& ship) const {
  if (!cfg_.enable_command_mesh) return 1.0;
  const double min_eff = std::clamp(cfg_.command_mesh_min_efficiency_multiplier, 0.0, 1.0);
  const double q = ship_command_mesh_quality(ship);
  return std::clamp(min_eff + (1.0 - min_eff) * q, 0.0, 1.0);
}

double Simulation::ship_subsystem_engine_multiplier(const Ship& ship) const {
  // Integrity effects are always applied; SimConfig::enable_ship_subsystem_damage controls
  // whether *combat* can inflict subsystem damage.
  const double base = clamp01(ship.engines_integrity);
  return std::clamp(base * ship_command_efficiency_multiplier(ship), 0.0, 1.0);
}

double Simulation::ship_subsystem_weapon_output_multiplier(const Ship& ship) const {
  const double base = clamp01(ship.weapons_integrity);
  return std::clamp(base * ship_command_efficiency_multiplier(ship), 0.0, 1.0);
}

double Simulation::ship_subsystem_sensor_range_multiplier(const Ship& ship) const {
  const double base = clamp01(ship.sensors_integrity);
  return std::clamp(base * ship_command_efficiency_multiplier(ship), 0.0, 1.0);
}

double Simulation::ship_subsystem_shield_multiplier(const Ship& ship) const {
  return clamp01(ship.shields_integrity);
}

} // namespace nebula4x
