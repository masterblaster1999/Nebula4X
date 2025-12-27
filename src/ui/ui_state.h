#pragma once

#include <cstdint>

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

  // Galaxy map view toggles.
  bool show_galaxy_labels{true};
  bool show_galaxy_jump_lines{true};
  bool show_galaxy_unknown_exits{true};

  // Max age (in days) for showing contact markers on the map.
  int contact_max_age_days{30};

  // Event log UI helpers.
  // The newest SimEvent::seq the UI considers "seen".
  // Not persisted in saves.
  std::uint64_t last_seen_event_seq{0};
};

} // namespace nebula4x::ui
