#pragma once

namespace nebula4x::ui {

// Shared UI toggle/state so multiple panels can respect the same fog-of-war settings.
// This is intentionally not persisted in saves.
struct UIState {
  bool fog_of_war{false};
  bool show_selected_sensor_range{true};
  bool show_contact_markers{true};
  bool show_contact_labels{false};

  // Max age (in days) for showing contact markers on the map.
  int contact_max_age_days{30};
};

} // namespace nebula4x::ui
