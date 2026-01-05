#pragma once

#include <cstdint>
#include <string>
#include <vector>

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

struct JsonWatchConfig {
  std::uint64_t id{0};
  std::string label;
  std::string path;
  bool track_history{true};
  bool show_sparkline{true};
  int history_len{120};
  // When true, treat `path` as a wildcard query pattern instead of a single RFC 6901 pointer.
  // Wildcards:
  //   *  matches any key/index at one segment
  //   ** matches zero or more segments (recursive)
  bool is_query{false};
  // Aggregation op for query mode.
  //   0=count matches, 1=sum, 2=avg, 3=min, 4=max
  int query_op{0};
};

// Procedural UI: data lenses (tables generated from JSON arrays).
struct JsonTableColumnConfig {
  std::string label;
  // JSON pointer relative to the row element (starts with '/').
  std::string rel_path;
  bool enabled{true};
};

struct JsonTableViewConfig {
  std::uint64_t id{0};
  std::string name;
  // JSON pointer to an array (or object) inside the root document.
  std::string array_path{"/"};

  // Column inference.
  int sample_rows{64};
  int max_depth{2};
  bool include_container_sizes{true};
  int max_infer_columns{64};

  // Filtering.
  std::string filter;
  bool filter_case_sensitive{false};
  // When false, filtering only searches through the configured columns.
  // When true, filtering scans all scalar fields in each row element.
  bool filter_all_fields{false};

  // Display caps.
  int max_rows{5000};

  // Column list (relative pointers).
  std::vector<JsonTableColumnConfig> columns;
};

// Procedural UI: dashboards (charts generated from Data Lenses).
struct JsonDashboardConfig {
  std::uint64_t id{0};
  std::string name;

  // Source lens (JsonTableViewConfig::id). This is the primary data source.
  std::uint64_t table_view_id{0};

  // How many rows to scan for stats (cap for very large arrays).
  int scan_rows{2000};
  // Work budget for incremental scanning; processed each frame while building.
  int rows_per_frame{250};

  // Layout / generation knobs.
  int histogram_bins{16};
  int max_numeric_charts{6};
  int max_category_cards{6};
  int top_n{8};

  // When true, the dashboard uses the linked Data Lens filter text.
  bool link_to_lens_filter{true};
  // When false, only enabled lens columns are considered.
  bool use_all_lens_columns{false};

  // Optional: which numeric column to use for the "Top rows" widget.
  // Relative JSON pointer (starts with '/'); empty means auto-pick.
  std::string top_rows_rel_path;
};



// Procedural UI: pivot tables (group-by aggregations over Data Lenses).
struct JsonPivotConfig {
  std::uint64_t id{0};
  std::string name;

  // Source lens (JsonTableViewConfig::id). This is the primary data source.
  std::uint64_t table_view_id{0};

  // How many rows to scan for aggregates (cap for very large arrays).
  int scan_rows{2000};
  // Work budget for incremental scanning; processed each frame while building.
  int rows_per_frame{250};

  // When true, the pivot uses the linked Data Lens filter text/options.
  bool link_to_lens_filter{true};
  // When false, only enabled lens columns are considered for filtering/group/value suggestions.
  bool use_all_lens_columns{false};

  // Group key path relative to the row element. Leading '/' required.
  std::string group_by_rel_path;

  // Optional numeric value column to aggregate.
  bool value_enabled{false};
  std::string value_rel_path;
  // 0=sum, 1=avg, 2=min, 3=max.
  int value_op{0};

  // Optional display cap; 0 = show all groups.
  int top_groups{0};
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
  bool show_balance_lab_window{false};
  bool show_intel_window{false};
  bool show_diplomacy_window{false};
  bool show_settings_window{false};

  // Debug/tooling windows.
  bool show_save_tools_window{false};
  bool show_time_machine_window{false};
  bool show_omni_search_window{false};
  bool show_json_explorer_window{false};
  bool show_entity_inspector_window{false};
  bool show_reference_graph_window{false};
  bool show_layout_profiles_window{false};
  bool show_watchboard_window{false};
  bool show_data_lenses_window{false};
  bool show_dashboards_window{false};
  bool show_pivot_tables_window{false};

  // --- Procedural UI: JSON Watchboard (pins) ---
  // These are UI preferences persisted in ui_prefs.json.
  std::uint64_t next_json_watch_id{1};
  std::vector<JsonWatchConfig> json_watch_items;
  // Query evaluation safety caps for wildcard pins.
  int watchboard_query_max_matches{5000};
  int watchboard_query_max_nodes{200000};

  // --- Procedural UI: Data Lenses (tables) ---
  // These are UI preferences persisted in ui_prefs.json.
  std::uint64_t next_json_table_view_id{1};
  std::vector<JsonTableViewConfig> json_table_views;

  // --- Procedural UI: Dashboards (charts/widgets over Data Lenses) ---
  // These are UI preferences persisted in ui_prefs.json.
  std::uint64_t next_json_dashboard_id{1};
  std::vector<JsonDashboardConfig> json_dashboards;


  // --- Procedural UI: Pivot Tables (group-by aggregations over Data Lenses) ---
  // These are UI preferences persisted in ui_prefs.json.
  std::uint64_t next_json_pivot_id{1};
  std::vector<JsonPivotConfig> json_pivots;

  // --- Procedural UI: OmniSearch (global search over live game JSON) ---
  // These are UI preferences persisted in ui_prefs.json.
  bool omni_search_match_keys{true};
  bool omni_search_match_values{true};
  bool omni_search_case_sensitive{false};
  bool omni_search_auto_refresh{false};
  float omni_search_refresh_sec{1.0f};
  int omni_search_nodes_per_frame{2500};
  int omni_search_max_results{2000};


  // --- Procedural UI: Entity Inspector (ID resolver + reference finder) ---
  // These are UI preferences persisted in ui_prefs.json.
  std::uint64_t entity_inspector_id{0};
  bool entity_inspector_auto_scan{true};
  float entity_inspector_refresh_sec{0.75f};
  int entity_inspector_nodes_per_frame{3500};
  int entity_inspector_max_refs{2500};


  // --- Procedural UI: Reference Graph (entity id relationships) ---
  // These are UI preferences persisted in ui_prefs.json.
  std::uint64_t reference_graph_focus_id{0};
  bool reference_graph_show_inbound{true};
  bool reference_graph_show_outbound{true};
  bool reference_graph_strict_id_keys{true};
  bool reference_graph_auto_layout{true};
  float reference_graph_refresh_sec{0.75f};
  int reference_graph_nodes_per_frame{4000};
  int reference_graph_max_nodes{250};

  // Global scan mode: build a whole-entity reference graph incrementally.
  bool reference_graph_global_mode{false};
  int reference_graph_entities_per_frame{6};
  int reference_graph_scan_nodes_per_entity{60000};
  int reference_graph_max_edges{12000};


  // --- Time Machine (state history + diffs) ---
  // These are UI preferences persisted in ui_prefs.json.
  bool time_machine_recording{false};
  float time_machine_refresh_sec{0.75f};
  int time_machine_keep_snapshots{32};
  int time_machine_max_changes{200};
  int time_machine_max_value_chars{160};


  // Additional UI chrome.
  bool show_status_bar{true};

  // Transient helper windows.
  bool show_command_palette{false};
  bool show_help_window{false};

  // Requested tab focus (consumed by the next frame).
  DetailsTab request_details_tab{DetailsTab::None};
  MapTab request_map_tab{MapTab::None};

  // Optional: request that the JSON Explorer focuses a specific JSON Pointer.
  // Consumed by the JSON Explorer window on the next frame.
  std::string request_json_explorer_goto_path;

  // Optional: request that the Data Lenses window selects a specific table view id.
  // Consumed by the Data Lenses window on the next frame.
  std::uint64_t request_select_json_table_view_id{0};

  // Optional: request that the Dashboards window selects a specific dashboard id.
  // Consumed by the Dashboards window on the next frame.
  std::uint64_t request_select_json_dashboard_id{0};

  // Optional: request that the Pivot Tables window selects a specific pivot id.
  // Consumed by the Pivot Tables window on the next frame.
  std::uint64_t request_select_json_pivot_id{0};

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

  // --- Dock layout profiles ---
  //
  // Dear ImGui stores docking state and window positions in an ini file
  // (io.IniFilename). Nebula4X exposes that ini file as a named "layout
  // profile" so you can keep multiple workspaces (economy, design, intel...)
  // and switch between them at runtime.
  //
  // These values are persisted in ui_prefs.json.
  char layout_profiles_dir[256] = "ui_layouts"; // directory containing *.ini files
  char layout_profile[64] = "default";          // active profile name (stem)

  // One-shot requests consumed by the App.
  bool request_reload_layout_profile{false};
  bool request_reset_window_layout{false};

  // UI-only feedback (not persisted).
  std::string layout_profile_status;

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

  // --- Rolling game autosaves (save-game snapshots) ---
  //
  // These are *not* the same as autosave_ui_prefs (theme/layout). When enabled,
  // the app writes a copy of the current save-game JSON every N simulated hours,
  // keeping the newest autosave_game_keep_files snapshots.
  bool autosave_game_enabled{true};
  int autosave_game_interval_hours{24};
  int autosave_game_keep_files{12};
  char autosave_game_dir[256]{"saves/autosaves"};

  // One-shot UI request (consumed by App::frame).
  bool request_autosave_game_now{false};

  // UI status strings (not persisted).
  std::string last_autosave_game_path;
  std::string last_autosave_game_error;

  // --- New Game dialog (UI-only) ---
  //
  // The simulation currently supports multiple built-in scenarios (Sol and a
  // deterministic procedural generator). These fields persist the last
  // selections so the user can quickly restart into the same kind of game.
  bool show_new_game_modal{false};
  int new_game_scenario{0}; // 0 = Sol, 1 = Random
  std::uint32_t new_game_random_seed{12345u};
  int new_game_random_num_systems{12};
};

} // namespace nebula4x::ui
