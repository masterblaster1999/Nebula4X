#include "nebula4x/core/colony_profiles.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

namespace nebula4x {

namespace {

double clamp_nonneg_finite(double v) {
  if (!std::isfinite(v)) return 0.0;
  if (v < 0.0) return 0.0;
  return v;
}

int clamp_nonneg(int v) { return std::max(0, v); }

}  // namespace

ColonyAutomationProfile make_colony_profile_from_colony(const Colony& c) {
  ColonyAutomationProfile p;
  p.installation_targets = c.installation_targets;
  p.mineral_reserves = c.mineral_reserves;
  p.mineral_targets = c.mineral_targets;
  p.garrison_target_strength = clamp_nonneg_finite(c.garrison_target_strength);
  p.population_target_millions = clamp_nonneg_finite(c.population_target_millions);
  p.population_reserve_millions = clamp_nonneg_finite(c.population_reserve_millions);
  return p;
}

void apply_colony_profile(Colony& c, const ColonyAutomationProfile& profile,
                          const ColonyProfileApplyOptions& opt) {
  if (opt.apply_installation_targets) {
    c.installation_targets.clear();
    c.installation_targets.reserve(profile.installation_targets.size());
    for (const auto& [k, v] : profile.installation_targets) {
      const int vv = clamp_nonneg(v);
      if (vv <= 0) continue;
      c.installation_targets[k] = vv;
    }
  }

  if (opt.apply_mineral_reserves) {
    c.mineral_reserves.clear();
    c.mineral_reserves.reserve(profile.mineral_reserves.size());
    for (const auto& [k, v] : profile.mineral_reserves) {
      const double vv = clamp_nonneg_finite(v);
      if (vv <= 1e-9) continue;
      c.mineral_reserves[k] = vv;
    }
  }

  if (opt.apply_mineral_targets) {
    c.mineral_targets.clear();
    c.mineral_targets.reserve(profile.mineral_targets.size());
    for (const auto& [k, v] : profile.mineral_targets) {
      const double vv = clamp_nonneg_finite(v);
      if (vv <= 1e-9) continue;
      c.mineral_targets[k] = vv;
    }
  }

  if (opt.apply_garrison_target) {
    c.garrison_target_strength = clamp_nonneg_finite(profile.garrison_target_strength);
  }

  if (opt.apply_population_target) {
    c.population_target_millions = clamp_nonneg_finite(profile.population_target_millions);
  }

  if (opt.apply_population_reserve) {
    c.population_reserve_millions = clamp_nonneg_finite(profile.population_reserve_millions);
  }
}

}  // namespace nebula4x
