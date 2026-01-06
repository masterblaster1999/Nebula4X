#pragma once

#include "nebula4x/core/entities.h"

namespace nebula4x {

// Options controlling which colony knobs a profile application touches.
// This is useful when you want to apply only (say) installation targets but not
// mineral reserves.
struct ColonyProfileApplyOptions {
  bool apply_installation_targets{true};
  bool apply_mineral_reserves{true};
  bool apply_mineral_targets{true};
  bool apply_garrison_target{true};
};

// Capture a colony's current automation knobs into a reusable profile.
ColonyAutomationProfile make_colony_profile_from_colony(const Colony& c);

// Apply a profile to a colony (replacing the chosen maps/targets).
void apply_colony_profile(Colony& c, const ColonyAutomationProfile& profile,
                          const ColonyProfileApplyOptions& opt = ColonyProfileApplyOptions{});

}  // namespace nebula4x
