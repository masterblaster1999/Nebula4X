#pragma once

#include <cstdint>

#include "nebula4x/core/ids.h"

namespace nebula4x::ui {

// Programmatic tab selection helpers.
// These are UI-only (not persisted in saves). They allow other UI surfaces
// (status bar, command palette, toast notifications) to request that a
// particular tab becomes active on the next frame.
enum class DetailsTab {
  None,
  Ship,
  Fleet,
  Colony,
  Body,
  Logistics,
  Research,
  Diplomacy,
  Design,
  Contacts,
  Log,
};

enum class MapTab {
  None,
  System,
  Galaxy,
};

// Shared UI toggle/state so multiple panels can respect the same fog-of-war settings.
// This is intentionally not persisted in saves.
struct UIState {
  // Which faction is currently used as the "viewer" for fog-of-war/exploration.
  // If a ship is selected, its faction typically overrides this.
  Id viewer_faction_id{kInvalidId};

  // Currently selected fleet (UI convenience).
  // Not persisted in saves.
  Id selected_fleet_id{kInvalidId};

  bool fog_of_war{false};
  bool show_selected_sensor_range{true};
  bool show_contact_markers{true};
  bool show_contact_labels{false};

  bool show_minor_bodies{true};
  bool show_minor_body_labels{false};

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

  // --- Window visibility / layout ---
  // These are UI-only preferences (not persisted in saves).
  bool show_controls_window{true};
  bool show_map_window{true};
  bool show_details_window{true};
  bool show_directory_window{true};
  bool show_economy_window{false};
  bool show_settings_window{false};

  // Additional UI chrome.
  bool show_status_bar{true};

  // Transient helper windows.
  bool show_command_palette{false};
  bool show_help_window{false};

  // Requested tab focus (consumed by the next frame).
  DetailsTab request_details_tab{DetailsTab::None};
  MapTab request_map_tab{MapTab::None};

  // UI scaling (1.0 = default). This affects readability on high-DPI displays.
  float ui_scale{1.0f};

  // Docking behavior (ImGui IO config). These are stored in UI prefs.
  bool docking_with_shift{false};
  bool docking_always_tab_bar{false};
  bool docking_transparent_payload{true};

  // Event toast notifications (warn/error popups).
  bool show_event_toasts{true};
  float event_toast_duration_sec{6.0f};

  // --- UI theme / colors (RGBA in 0..1) ---
  // These are UI-only preferences. The UI provides helpers to save/load these
  // preferences to a separate JSON file (not the save-game).

  // SDL renderer clear color (behind all ImGui windows).
  float clear_color[4]{0.0f, 0.0f, 0.0f, 1.0f};

  // Map backgrounds.
  // Defaults match the previous hardcoded colors.
  float system_map_bg[4]{15.0f / 255.0f, 18.0f / 255.0f, 22.0f / 255.0f, 1.0f};
  float galaxy_map_bg[4]{12.0f / 255.0f, 14.0f / 255.0f, 18.0f / 255.0f, 1.0f};

  // Optional: override ImGui window background (ImGuiCol_WindowBg/ChildBg).
  bool override_window_bg{false};
  float window_bg[4]{0.10f, 0.105f, 0.11f, 0.94f};

  // If true, the UI will auto-save UI prefs to the configured ui_prefs_path on exit.
  bool autosave_ui_prefs{true};
};

} // namespace nebula4x::ui
