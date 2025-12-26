#pragma once

#include "nebula4x/core/ids.h"

namespace nebula4x::ui {

// Shared UI toggle/state so multiple panels can respect the same fog-of-war settings.
// This is intentionally not persisted in saves.
struct UIState {
  // Which faction is currently used as the "viewer" for fog-of-war/exploration.
  // If a ship is selected, its faction typically overrides this.
  Id viewer_faction_id{kInvalidId};

  bool fog_of_war{false};
  bool show_selected_sensor_range{true};
  bool show_contact_markers{true};
  bool show_contact_labels{false};

  // Max age (in days) for showing contact markers on the map.
  int contact_max_age_days{30};
};

} // namespace nebula4x::ui
