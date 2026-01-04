#pragma once

#include <cstdint>
#include <string>

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

  // Currently selected contact (hostile ship memory) for intel-centric UIs.
  // This is UI-only and not persisted.
  Id selected_contact_ship_id{kInvalidId};

  bool fog_of_war{false};
  bool show_selected_sensor_range{true};
  // Show combined sensor coverage rings for the viewer faction (includes mutual-friendly sensor sharing).
  bool show_faction_sensor_coverage{false};
  bool faction_sensor_coverage_fill{true};
  // Assumed target signature multiplier for coverage visualization (1.0 = baseline).
  float faction_sensor_coverage_signature{1.0f};
  // Safety/perf cap for how many sensor sources to draw as rings.
  int faction_sensor_coverage_max_sources{128};
  bool show_selected_weapon_range{false};
  bool show_fleet_weapon_ranges{false};
  bool show_hostile_weapon_ranges{false};
  bool show_contact_markers{true};
  bool show_contact_labels{false};

  bool show_minor_bodies{true};
  bool show_minor_body_labels{false};

  // Galaxy map view toggles.
  bool show_galaxy_labels{true};
  bool show_galaxy_jump_lines{true};
  bool show_galaxy_unknown_exits{true};
  bool show_galaxy_intel_alerts{true};

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
  bool show_production_window{false};
  bool show_economy_window{false};
  bool show_planner_window{false};
  bool show_freight_window{false};
  bool show_fuel_window{false};
  bool show_time_warp_window{false};
  bool show_timeline_window{false};
  bool show_design_studio_window{false};
  bool show_intel_window{false};
  bool show_diplomacy_window{false};
  bool show_settings_window{false};

  // Additional UI chrome.
  bool show_status_bar{true};

  // Transient helper windows.
  bool show_command_palette{false};
  bool show_help_window{false};

  // Requested tab focus (consumed by the next frame).
  DetailsTab request_details_tab{DetailsTab::None};
  MapTab request_map_tab{MapTab::None};

  // Optional: request that the details panel focus a specific faction.
  // Consumed by the details panel on the next frame.
  Id request_focus_faction_id{kInvalidId};

  // Optional: request that the system map recenters on a specific world position.
  // Consumed by the system map on the next frame.
  bool request_system_map_center{false};
  Id request_system_map_center_system_id{kInvalidId};
  double request_system_map_center_x_mkm{0.0};
  double request_system_map_center_y_mkm{0.0};
  // If > 0, the system map may also adopt this zoom level.
  double request_system_map_center_zoom{0.0};

  // Optional detail focus helpers (consumed by the next frame).
  // These are UI-only and not persisted.
  std::string request_focus_design_id;

  // Optional: request focus on a particular design inside the Design Studio
  // (blueprints) window.
  // This is UI-only and not persisted.
  std::string request_focus_design_studio_id;

  // Optional: request focus on a particular event in the Timeline window.
  // This is UI-only and not persisted.
  std::uint64_t request_focus_event_seq{0};

  // UI scaling (1.0 = default). This affects readability on high-DPI displays.
  float ui_scale{1.0f};

  // Docking behavior (ImGui IO config). These are stored in UI prefs.
  bool docking_with_shift{false};
  bool docking_always_tab_bar{false};
  bool docking_transparent_payload{true};

  // Event toast notifications (warn/error popups).
  bool show_event_toasts{true};
  float event_toast_duration_sec{6.0f};

  // --- Timeline (event visualization) ---
  bool timeline_show_minimap{true};
  bool timeline_show_grid{true};
  bool timeline_show_labels{true};
  bool timeline_compact_rows{false};
  float timeline_lane_height{34.0f};
  float timeline_marker_size{4.5f};
  bool timeline_follow_now{true};

  // --- Design Studio (blueprint visualization) ---
  bool design_studio_show_grid{true};
  bool design_studio_show_labels{true};
  bool design_studio_show_compare{true};
  bool design_studio_show_power_overlay{true};

  // --- Intel (contacts + radar) ---
  bool intel_radar_scanline{true};
  bool intel_radar_grid{true};
  bool intel_radar_show_sensors{true};
  bool intel_radar_sensor_heat{true};
  bool intel_radar_show_bodies{true};
  bool intel_radar_show_jump_points{true};
  bool intel_radar_show_friendlies{true};
  bool intel_radar_show_hostiles{true};
  bool intel_radar_show_contacts{true};
  bool intel_radar_labels{false};

  // --- Diplomacy Graph ---
  bool diplomacy_graph_starfield{true};
  bool diplomacy_graph_grid{false};
  bool diplomacy_graph_labels{true};
  bool diplomacy_graph_arrows{true};
  bool diplomacy_graph_dim_nonfocus{true};
  bool diplomacy_graph_show_hostile{true};
  bool diplomacy_graph_show_neutral{true};
  bool diplomacy_graph_show_friendly{true};
  int diplomacy_graph_layout{0}; // 0=Radial, 1=Force, 2=Circle

  // --- UI theme / colors (RGBA in 0..1) ---
  // These are UI-only preferences. The UI provides helpers to save/load these
  // preferences to a separate JSON file (not the save-game).

  // SDL renderer clear color (behind all ImGui windows).
  float clear_color[4]{0.0f, 0.0f, 0.0f, 1.0f};

  // Map backgrounds.
  // Defaults match the previous hardcoded colors.
  float system_map_bg[4]{15.0f / 255.0f, 18.0f / 255.0f, 22.0f / 255.0f, 1.0f};
  float galaxy_map_bg[4]{12.0f / 255.0f, 14.0f / 255.0f, 18.0f / 255.0f, 1.0f};

  // --- Map rendering chrome ---
  // These are UI-only preferences (persisted via ui_prefs.json).
  bool system_map_starfield{true};
  bool system_map_grid{false};
  bool system_map_order_paths{true};
  bool system_map_fleet_formation_preview{true};
  bool system_map_missile_salvos{false};
  bool system_map_follow_selected{false};

  bool galaxy_map_starfield{true};
  bool galaxy_map_grid{false};
  bool galaxy_map_selected_route{true};

  // Shared tuning knobs.
  float map_starfield_density{1.0f};
  float map_starfield_parallax{0.15f};
  float map_grid_opacity{1.0f};
  float map_route_opacity{1.0f};

  // Optional: override ImGui window background (ImGuiCol_WindowBg/ChildBg).
  bool override_window_bg{false};
  float window_bg[4]{0.10f, 0.105f, 0.11f, 0.94f};

  // If true, the UI will auto-save UI prefs to the configured ui_prefs_path on exit.
  bool autosave_ui_prefs{true};
};

} // namespace nebula4x::ui
