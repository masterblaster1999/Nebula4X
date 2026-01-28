#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
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
  Journal,
  Log,
};

enum class MapTab {
  None,
  System,
  Galaxy,
};

// UI renderer backend (runtime).
// In OpenGL builds, Nebula4X will prefer OpenGL2 but can fall back to SDL_Renderer2
// when OpenGL context creation fails.
enum class UIRendererBackend : std::uint8_t {
  SDLRenderer2 = 0,
  OpenGL2 = 1,
  Unknown = 255,
};

inline const char* ui_renderer_backend_name(UIRendererBackend b) {
  switch (b) {
    case UIRendererBackend::SDLRenderer2:
      return "SDL_Renderer2";
    case UIRendererBackend::OpenGL2:
      return "OpenGL2";
    default:
      return "Unknown";
  }
}


// Help window tabs (Help / Codex).
// UI-only; used for programmatic tab selection (e.g., guided tours).
enum class HelpTab {
  None,
  QuickStart,
  Tours,
  Shortcuts,
  Docs,
  Accessibility,
  About,
};


// UI navigation targets used by the Navigator window (history/bookmarks).
//
// These are UI-only structures (not persisted in save-games). IDs are resolved
// against the currently-loaded GameState; stale entries are treated as missing.
enum class NavTargetKind : int {
  System = 0,
  Ship,
  Colony,
  Body,
};

struct NavTarget {
  NavTargetKind kind{NavTargetKind::System};
  Id id{kInvalidId};
};

inline bool operator==(const NavTarget& a, const NavTarget& b) {
  return a.kind == b.kind && a.id == b.id;
}
inline bool operator!=(const NavTarget& a, const NavTarget& b) { return !(a == b); }

struct NavBookmark {
  std::uint64_t bookmark_id{0};
  std::string name;
  NavTarget target;
};


// --- Hotkeys / keyboard shortcuts ---
//
// Hotkeys are UI-only and are stored in ui_prefs.json (not in save-games).
// The key code is stored as an int corresponding to ImGuiKey.
struct HotkeyChord {
  bool ctrl{false};
  bool shift{false};
  bool alt{false};
  bool super{false};
  int key{0};
};

inline bool operator==(const HotkeyChord& a, const HotkeyChord& b) {
  return a.ctrl == b.ctrl && a.shift == b.shift && a.alt == b.alt && a.super == b.super && a.key == b.key;
}
inline bool operator!=(const HotkeyChord& a, const HotkeyChord& b) { return !(a == b); }


// Galaxy-map visualization overlays for procedural-generation outcomes.
//
// This is a UI-only enum (not saved in game state). Preferences may be stored
// in ui_prefs.json.
enum class ProcGenLensMode : int {
  Off = 0,
  NebulaDensity,
  StarTemperature,
  StarMass,
  StarLuminosity,
  BodyCount,
  HabitableCandidates,
  MineralWealth,
  JumpDegree,

  // Region-level procedural modifiers.
  RegionNebulaBias,
  RegionPirateRiskEffective,
  RegionPirateSuppression,
  RegionRuinsDensity,
  RegionMineralRichness,
  RegionVolatileRichness,
  RegionSalvageRichness,
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

  // --- Procedural UI: Alert rules (Watchboard -> HUD toasts) ---
  // When enabled, the watch pin can generate a toast when the condition is met.
  //
  // NOTE: Alerts are UI-only and do not change the simulation.
  bool alert_enabled{false};
  // Condition:
  //   0 = Cross above threshold
  //   1 = Cross below threshold
  //   2 = Change (abs delta)
  //   3 = Change (percent delta)
  //   4 = Any change (string/number)
  int alert_mode{0};
  // Threshold used by cross-above/cross-below.
  double alert_threshold{0.0};
  // Delta used by abs/percent change. For percent mode, 0.10 = 10%.
  double alert_delta{0.0};
  // Toast level:
  //   0=Info, 1=Warning, 2=Error
  int alert_toast_level{1};
  // Minimum real time between alerts for this pin (debounce).
  float alert_cooldown_sec{2.0f};
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




// Procedural UI: UI Forge (custom panels composed of widgets over live game JSON).
//
// Goal: let players build small dockable dashboards without writing C++.
// Panels are persisted in ui_prefs.json.
struct UiForgeWidgetConfig {
  std::uint64_t id{0};

  // 0 = KPI (value/query card)
  // 1 = Text (note card)
  // 2 = Separator (full-width divider)
  // 3 = List (array/object preview)
  int type{0};

  // Card title / label.
  std::string label;

  // JSON pointer (or query pattern when is_query=true). Used by KPI and List widgets.
  std::string path;

  // Text content for type==Text.
  std::string text;

  // Query mode (glob pattern) for KPI widgets.
  bool is_query{false};
  // Aggregation op for query mode.
  //   0=count matches, 1=sum, 2=avg, 3=min, 4=max
  int query_op{0};

  // KPI history/sparkline.
  bool track_history{true};
  bool show_sparkline{true};
  int history_len{120};

  // Simple layout hint for the responsive grid.
  // 1 = normal width, 2 = double-width, etc.
  int span{1};

  // List preview rows.
  int preview_rows{8};
};

struct UiForgePanelConfig {
  std::uint64_t id{0};
  std::string name;

  // Whether this panel is currently shown as its own window.
  bool open{false};

  // Root pointer used by the auto-generator.
  std::string root_path{"/"};

  // Layout knobs.
  // 0 = auto; otherwise fixed column count.
  int desired_columns{0};
  // Base card width in "em" (font-size units). 20em ~= 280px at default font.
  float card_width_em{20.0f};

  std::vector<UiForgeWidgetConfig> widgets;
};

// A lightweight, user-managed library entry for sharing/reusing UI Forge panels.
//
// Presets are stored as encoded Panel DNA strings (see ui_forge_dna.h) and are
// persisted in ui_prefs.json.
struct UiForgePanelPreset {
  std::string name;
  std::string dna;
};

// --- Notification Center (UI-only inbox) ---
//
// The Notification Center is a persistent triage inbox that aggregates:
//   - Simulation events (SimEvent) that the UI deems important
//   - Watchboard alerts (pins with alert_enabled)
//
// It is intentionally UI-only and is not persisted in saves.
// Preferences (capture rules, retention caps) are stored in ui_prefs.json.
enum class NotificationSource : int {
  SimEvent = 0,
  WatchboardAlert = 1,
};

struct NotificationEntry {
  // Unique id for this notification.
  // - For SimEvents, this is the SimEvent::seq.
  // - For Watchboard alerts, this is derived from the toast sequence base.
  std::uint64_t id{0};
  NotificationSource source{NotificationSource::SimEvent};

  // Read/unread triage.
  bool unread{true};
  bool pinned{false};

  // Collapse duplicates.
  int count{1};

  // Simulation time context.
  // day = GameState::date.days_since_epoch.
  std::int64_t day{0};
  int hour{0};

  // Severity/category (stored as integers to keep ui_state.h lightweight).
  // Matches core enums:
  //   EventLevel    : 0=Info, 1=Warn, 2=Error
  //   EventCategory : see nebula4x/core/entities.h
  int level{0};
  int category{0};

  // Optional entity context.
  Id system_id{kInvalidId};
  Id ship_id{kInvalidId};
  Id colony_id{kInvalidId};
  Id body_id{kInvalidId};
  Id anomaly_id{kInvalidId};
  Id wreck_id{kInvalidId};
  Id faction_id{kInvalidId};
  Id faction_id2{kInvalidId};

  // Watchboard context.
  std::uint64_t watch_id{0};
  std::string watch_label;
  std::string watch_path;
  std::string watch_rep_ptr;

  // Human-readable message.
  std::string message;

  // UI timestamps (ImGui::GetTime()) for "arrived" / "last updated".
  // Used for stable sorting and duplicate collapse heuristics.
  double created_time_s{0.0};
  double updated_time_s{0.0};
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

  // Currently selected region/sector (UI convenience).
  // This is UI-only and not persisted.
  Id selected_region_id{kInvalidId};

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
  bool show_contact_uncertainty{true};

  bool show_minor_bodies{true};
  bool show_minor_body_labels{false};

  // Galaxy map view toggles.
  bool show_galaxy_labels{true};
  bool show_galaxy_pins{true};
  bool show_galaxy_jump_lines{true};
  bool show_galaxy_unknown_exits{true};
  bool show_galaxy_intel_alerts{true};

  // Procedural generation visualization lens.
  //
  // When enabled (mode != Off), the galaxy map will color system nodes by the
  // chosen metric to make "shape" and balance issues in the generated galaxy
  // obvious at a glance (e.g. mineral deserts, habitable clusters, nebula
  // walls, overly-central hubs).
  ProcGenLensMode galaxy_procgen_lens_mode{ProcGenLensMode::Off};
  bool galaxy_procgen_lens_show_legend{true};
  float galaxy_procgen_lens_alpha{0.80f};
  // Apply a log scale before normalization for wide-range metrics (e.g. minerals).
  bool galaxy_procgen_lens_log_scale{true};

  // Procedural field rendering (heatmap) for the selected ProcGen lens.
  //
  // When enabled, the galaxy map renders a low-res continuous field behind
  // the system nodes by interpolating the lens metric over space.
  bool galaxy_procgen_field{false};
  float galaxy_procgen_field_alpha{0.22f};
  // Approximate size of a field cell in pixels. Lower => higher resolution.
  int galaxy_procgen_field_cell_px{18};

  // Procedural contour rendering (isolines) for the selected ProcGen lens.
  //
  // When enabled, the galaxy map draws contour lines over the interpolated
  // lens field to make gradients and boundaries easier to read at a glance.
  bool galaxy_procgen_contours{false};
  float galaxy_procgen_contour_alpha{0.20f};
  // Approximate size of a contour grid cell in pixels. Lower => more detail.
  int galaxy_procgen_contour_cell_px{26};
  // Number of contour levels between min and max (evenly spaced).
  int galaxy_procgen_contour_levels{7};
  float galaxy_procgen_contour_thickness{1.2f};

  // Procedural gradient vector field rendering for the selected ProcGen lens.
  //
  // When enabled, the galaxy map draws small arrows indicating the direction
  // of increasing lens value (a quick "slope" visualization).
  bool galaxy_procgen_vectors{false};
  float galaxy_procgen_vector_alpha{0.22f};
  // Approximate size of a vector grid cell in pixels. Higher => fewer arrows.
  int galaxy_procgen_vector_cell_px{42};
  // Arrow length scaling factor (in pixels per unit gradient magnitude).
  float galaxy_procgen_vector_scale{120.0f};
  // Minimum gradient magnitude (dimensionless, in normalized lens-space) to draw a vector.
  float galaxy_procgen_vector_min_mag{0.020f};

  // Hold Alt over the galaxy map to probe the interpolated ProcGen lens value.
  bool galaxy_procgen_probe{true};

  // Star Atlas: procedural constellations overlay.
  //
  // These are UI-only helpers computed from the currently visible (discovered)
  // system set. They are meant as a "shape" layer for navigation and for
  // debugging procgen clustering.
  bool galaxy_star_atlas_constellations{false};
  bool galaxy_star_atlas_labels{true};
  float galaxy_star_atlas_alpha{0.22f};
  float galaxy_star_atlas_label_alpha{0.35f};
  int galaxy_star_atlas_target_cluster_size{8};
  int galaxy_star_atlas_max_constellations{128};
  // Hide constellations on the galaxy map when zoom is very low (prevents clutter).
  float galaxy_star_atlas_min_zoom{0.18f};

  // Logistics overlays.
  bool show_galaxy_freight_lanes{false};

  // Procedural interstellar economy overlays.
  bool show_galaxy_trade_lanes{false};
  bool show_galaxy_trade_hubs{false};

  // Trade overlay controls (UI-only preferences).
  //
  // These settings affect how the *procedural* civilian trade overlay is rendered
  // on the galaxy map (filters, danger/risk visualization, and quick inspection).
  //
  // Commodity filter: -1 = show all goods. Otherwise, filter lanes to those
  // whose dominant commodity matches the selected kind (or any of the lane's
  // top goods when galaxy_trade_filter_include_secondary is enabled).
  int galaxy_trade_good_filter{-1};
  bool galaxy_trade_filter_include_secondary{true};

  // Hide lanes below this total volume (reduces clutter).
  float galaxy_trade_min_lane_volume{0.0f};

  // When enabled, draw an additional danger overlay on trade lanes based on
  // effective piracy risk at the endpoints (region pirate risk * (1-suppression)).
  bool galaxy_trade_risk_overlay{false};

  // Show a small "Trade security" analysis panel in the galaxy map legend.
  bool galaxy_trade_security_panel{true};
  int galaxy_trade_security_top_n{10};

  // Pinned trade lane (for persistent inspection/highlighting).
  // UI-only; not persisted in saves.
  Id galaxy_trade_pinned_from{kInvalidId};
  Id galaxy_trade_pinned_to{kInvalidId};

  // Draw mission geometry (patrol routes/circuits, jump point guards) for fleets.
  // Intended as a strategic planning overlay.
  bool show_galaxy_fleet_missions{false};
  float galaxy_fleet_mission_alpha{0.55f};

  // Highlight jump-network articulation points ("chokepoint" systems).
  bool show_galaxy_chokepoints{false};

  // Procedural region overlay.
  bool show_galaxy_regions{false};
  bool show_galaxy_region_labels{false};
  bool show_galaxy_region_boundaries{false};
  // Boundary geometry mode.
  // - Hull: convex hull of the region's *visible* systems (cheap but can be misleading).
  // - Voronoi: true Voronoi partition based on Region::center (matches procgen assignment).
  bool galaxy_region_boundary_voronoi{true};
  // Show the region seed/center points (useful for debugging procgen sectors).
  bool show_galaxy_region_centers{false};
  // Highlight jump links that cross region borders.
  bool show_galaxy_region_border_links{false};
  // When a region is selected, optionally dim non-selected regions on the galaxy map.
  bool galaxy_region_dim_nonselected{false};

  // Max age (in days) for showing contact markers on the map.
  int contact_max_age_days{30};

  // Event log UI helpers.
  // The newest SimEvent::seq the UI considers "seen".
  // Not persisted in saves.
  std::uint64_t last_seen_event_seq{0};

  // Notification Center (persistent UI inbox).
  // Stores a rolling history of important events/alerts so they can be
  // triaged even when HUD toasts are disabled or missed.
  //
  // Not persisted in saves.
  std::vector<NotificationEntry> notifications;
  // Last SimEvent::seq ingested into the inbox.
  // Used to avoid scanning the full event list each frame.
  std::uint64_t notifications_last_ingested_event_seq{0};
  // One-shot focus request: select+scroll to a specific notification id.
  std::uint64_t notifications_request_focus_id{0};

  // Capture preferences (stored in ui_prefs.json).
  bool notifications_capture_sim_events{true};
  bool notifications_capture_info_events{false};
  bool notifications_capture_watchboard_alerts{true};
  bool notifications_collapse_duplicates{true};
  bool notifications_auto_open_on_error{false};
  // Retention caps.
  int notifications_max_entries{600};
  // Age cap (in sim days). 0 = keep forever.
  int notifications_keep_days{365};

  // --- Window visibility / layout ---
  // These are UI-only preferences (not persisted in saves).
  bool show_controls_window{true};
  bool show_map_window{true};
  bool show_details_window{true};
  bool show_directory_window{true};
  bool show_production_window{false};
  bool show_economy_window{false};
  bool show_planner_window{false};
  bool show_regions_window{false};
  bool show_freight_window{false};
  bool show_mine_window{false};
  bool show_fuel_window{false};
  bool show_salvage_window{false};
  bool show_contracts_window{false};
  bool show_sustainment_window{false};
  // Fleet Manager: global fleet list + route planner + quick mission controls.
  bool show_fleet_manager_window{false};
  bool show_troop_window{false};
  bool show_colonist_window{false};
  bool show_terraforming_window{false};
  bool show_advisor_window{false};
  bool show_time_warp_window{false};
  bool show_timeline_window{false};
  bool show_notifications_window{false};
  bool show_design_studio_window{false};
  bool show_balance_lab_window{false};
  bool show_intel_window{false};
  bool show_intel_notebook_window{false};
  bool show_diplomacy_window{false};
  bool show_victory_window{false};
  bool show_colony_profiles_window{false};
  bool show_ship_profiles_window{false};
  // Bulk management of ship automation flags (missions/sustainment).
  bool show_automation_center_window{false};
  bool show_shipyard_targets_window{false};
  bool show_survey_network_window{false};
  bool show_settings_window{false};

  // Debug/tooling windows.
  bool show_save_tools_window{false};
  bool show_time_machine_window{false};
  bool show_compare_window{false};
  bool show_omni_search_window{false};
  bool show_json_explorer_window{false};
  bool show_content_validation_window{false};
  bool show_state_doctor_window{false};
  bool show_trace_viewer_window{false};
  bool show_entity_inspector_window{false};
  bool show_reference_graph_window{false};
  bool show_layout_profiles_window{false};
  bool show_window_manager_window{false};
  bool show_procgen_atlas_window{false};
  bool show_star_atlas_window{false};
  bool show_watchboard_window{false};
  bool show_data_lenses_window{false};
  bool show_dashboards_window{false};
  bool show_pivot_tables_window{false};

  bool show_ui_forge_window{false};
  bool show_context_forge_window{false};

  // --- Trace Viewer (in-process performance profiler) ---
  // Preferences are stored in ui_prefs.json.
  bool trace_viewer_autostart{false};
  bool trace_viewer_auto_refresh{true};
  float trace_viewer_refresh_sec{0.25f};
  int trace_viewer_max_events{20000};
  bool trace_viewer_follow_tail{true};
  float trace_viewer_window_ms{500.0f};
  std::string trace_viewer_export_path{"traces/nebula4x_trace.json"};

  // --- Procedural UI: Context Forge (auto-generated UI Forge panel) ---
  //
  // Context Forge creates/updates a UI Forge panel that follows selection (ship/colony/body)
  // or a pinned entity id.
  //
  // Most fields are persisted in ui_prefs.json; transient fields are noted.
  bool context_forge_enabled{false};
  bool context_forge_follow_selection{true};
  bool context_forge_auto_update{true};
  std::uint64_t context_forge_pinned_entity_id{kInvalidId};

  int context_forge_seed{1337};
  int context_forge_max_kpis{16};
  int context_forge_max_lists{4};
  int context_forge_depth{1};
  int context_forge_max_array_numeric_keys{2};
  bool context_forge_include_lists{true};
  bool context_forge_include_queries{true};
  bool context_forge_include_id_fields{false};
  bool context_forge_open_panel_on_generate{true};

  // Transient: immediate action flag.
  bool context_forge_request_regenerate{false};
  // Persisted: which UI Forge panel id is treated as the context panel.
  std::uint64_t context_forge_panel_id{0};
  // Transient: last target id we generated for.
  std::uint64_t context_forge_last_entity_id{kInvalidId};
  // Transient: last error string.
  std::string context_forge_last_error;
  // Transient: used for friendly "generated X seconds ago" display.
  double context_forge_last_success_time{0.0};

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


  // --- Procedural UI: UI Forge (custom panels over live game JSON) ---
  // These are UI preferences persisted in ui_prefs.json.
  std::uint64_t next_ui_forge_panel_id{1};
  std::uint64_t next_ui_forge_widget_id{1};
  std::vector<UiForgePanelConfig> ui_forge_panels;
  std::vector<UiForgePanelPreset> ui_forge_presets;

  // --- Procedural UI: OmniSearch (global search over live game JSON) ---
  // These are UI preferences persisted in ui_prefs.json.
  bool omni_search_match_keys{true};
  bool omni_search_match_values{true};
  bool omni_search_match_entities{true};
  bool omni_search_match_docs{true};
  bool omni_search_match_windows{true};
  bool omni_search_match_layouts{true};
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
  // 0 = Full JSON snapshots (fastest access; higher memory).
  // 1 = Delta chain of RFC 7386 JSON Merge Patches (lower memory).
  int time_machine_storage_mode{1};
  // When in delta mode, store a full checkpoint snapshot every N captures.
  // 1 => every snapshot is a checkpoint (equivalent to full snapshots, but still stores patches).
  int time_machine_checkpoint_stride{8};
  int time_machine_keep_snapshots{32};
  int time_machine_max_changes{200};
  int time_machine_max_value_chars{160};


  // --- Compare / Diff (entity comparison) ---
  // Preferences persisted in ui_prefs.json.
  // Note: the selected ids may not exist across different saves/scenarios.
  float compare_refresh_sec{0.75f};
  bool compare_include_container_sizes{true};
  bool compare_show_unchanged{false};
  bool compare_case_sensitive{false};
  int compare_max_depth{6};
  int compare_max_nodes{6000};
  int compare_max_value_chars{160};

  // UI-only compare state (not persisted).
  Id compare_a_id{kInvalidId};
  Id compare_b_id{kInvalidId};
  bool compare_a_use_snapshot{false};
  bool compare_b_use_snapshot{false};
  std::string compare_a_snapshot_label;
  std::string compare_b_snapshot_label;
  std::string compare_a_snapshot_json;
  std::string compare_b_snapshot_json;
  std::string compare_filter;


  // Additional UI chrome.
  bool show_status_bar{true};

  // --- Command Console (command palette) ---
  // Stored as command ids (stable strings) in ui_prefs.json.
  std::vector<std::string> command_favorites;
  std::vector<std::string> command_recent;
  int command_recent_limit{25};

  // Transient helper windows.
  bool show_command_palette{false};
  bool show_help_window{false};
  bool show_navigator_window{false};

  // --- Guided Tours (onboarding overlay) ---
  // UI-only; not persisted.
  bool tour_active{false};
  int tour_active_index{0};
  int tour_step_index{0};
  bool tour_dim_background{true};
  float tour_dim_alpha{0.70f};
  // When enabled, the tour blocks interactions outside the spotlight target.
  // This lets the player click inside the highlighted window while preventing
  // accidental clicks elsewhere.
  bool tour_block_outside_spotlight{true};
  bool tour_pause_toasts{true};

  // --- Navigation (selection history + bookmarks) ---
  // UI-only; cleared when a new game is loaded/created (state generation changes).
  bool nav_open_windows_on_jump{true};
  int nav_history_max{256};

  std::vector<NavTarget> nav_history;
  int nav_history_cursor{-1};
  bool nav_history_suppress_push{false};

  std::uint64_t nav_next_bookmark_id{1};
  std::vector<NavBookmark> nav_bookmarks;


  // Requested tab focus (consumed by the next frame).
  DetailsTab request_details_tab{DetailsTab::None};
  MapTab request_map_tab{MapTab::None};
  HelpTab request_help_tab{HelpTab::None};

  // Optional: request that the JSON Explorer focuses a specific JSON Pointer.
  // Consumed by the JSON Explorer window on the next frame.
  std::string request_json_explorer_goto_path;

  // Optional: request that the Codex opens a specific doc (by path or ref).
  // Consumed by the Docs Browser panel on the next frame.
  std::string request_open_doc_ref;

  // Optional: request that the Watchboard scrolls/highlights a specific watch id.
  // Consumed by the Watchboard window on the next frame.
  std::uint64_t request_watchboard_focus_id{0};

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

  // Optional: request that the galaxy map recenters on a specific galaxy position.
  // Consumed by the galaxy map on the next frame.
  bool request_galaxy_map_center{false};
  double request_galaxy_map_center_x{0.0};
  double request_galaxy_map_center_y{0.0};
  // If > 0, the galaxy map may also adopt this zoom level.
  double request_galaxy_map_center_zoom{0.0};
  // If > 0, the galaxy map may compute a zoom that "fits" a target half-span.
  // This is expressed in galaxy units (same space as StarSystem::galaxy_pos).
  double request_galaxy_map_fit_half_span{0.0};

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

  // --- Hotkeys / keyboard shortcuts ---
  //
  // These are global hotkeys processed by the App layer (before windows draw).
  // They are UI-only and persisted in ui_prefs.json.
  bool hotkeys_enabled{true};
  // When true, global hotkey dispatch is suppressed so the user can safely
  // capture a new key chord in the Hotkeys editor.
  bool hotkeys_capture_active{false};
  // If non-empty, the Hotkeys editor is waiting for a new chord for this id.
  // This is UI-only state (not persisted).
  std::string hotkeys_capture_id;
  // Overrides keyed by hotkey id (string). Unknown ids are ignored on load.
  // If a hotkey id is missing here, the default chord for that action is used.
  std::unordered_map<std::string, HotkeyChord> hotkey_overrides;

  // --- Screen reader / narration (accessibility) ---
  //
  // This is not a native OS accessibility tree; it's an in-game narration layer
  // that can speak key UI feedback (toasts, selection changes, focused controls).
  // These values are persisted in ui_prefs.json.
  bool screen_reader_enabled{false};
  bool screen_reader_speak_focus{true};
  bool screen_reader_speak_hover{false};
  bool screen_reader_speak_windows{true};
  bool screen_reader_speak_toasts{true};
  bool screen_reader_speak_selection{true};
  float screen_reader_rate{1.0f};               // 0.50 .. 2.00
  float screen_reader_volume{1.0f};             // 0.00 .. 1.00
  float screen_reader_hover_delay_sec{0.65f};   // seconds

  // UI style preset (ImGui colors + rounding + chrome).
  // 0 = Dark (default), 1 = Light, 2 = Classic, 3 = Nebula, 4 = High Contrast, 5 = Procedural
  int ui_style_preset{0};

  // --- Procedural theme (ui_style_preset = 5) ---
  // The procedural theme generates a full accent palette from a small set of parameters
  // so players can quickly create/share custom UI skins.
  int ui_procedural_theme_seed{1337};
  bool ui_procedural_theme_use_seed_hue{true};
  float ui_procedural_theme_hue_deg{190.0f};
  // 0=Analogous, 1=Complementary, 2=Triad, 3=Monochrome
  int ui_procedural_theme_variant{0};
  float ui_procedural_theme_saturation{0.72f};
  float ui_procedural_theme_value{0.90f};
  float ui_procedural_theme_bg_value{0.11f};
  float ui_procedural_theme_accent_strength{0.28f};
  bool ui_procedural_theme_animate_hue{false};
  float ui_procedural_theme_animate_speed_deg_per_sec{6.0f};
  // When enabled, the theme also drives SDL clear + map backgrounds to keep the UI cohesive.
  bool ui_procedural_theme_sync_backgrounds{false};

  // UI density affects padding/spacing sizing. Useful for data-heavy windows.
  // 0 = Comfortable (default), 1 = Compact, 2 = Spacious
  int ui_density{0};

  // When true, scale ImGui style sizes (padding/spacing) along with ui_scale.
  // When false, only fonts scale.
  bool ui_scale_style{true};


  // Docking behavior (ImGui IO config). These are stored in UI prefs.
  bool docking_with_shift{false};
  bool docking_always_tab_bar{false};
  bool docking_transparent_payload{true};

  // Multi-Viewport (detachable OS windows).
  // Note: Requires a renderer backend with platform-window support (e.g. SDL2+OpenGL2).
  bool viewports_enable{true};
  bool viewports_no_taskbar_icon{true};
  bool viewports_no_auto_merge{false};
  bool viewports_no_decoration{false};

  // Runtime renderer info (not persisted).
  // Filled by src/main.cpp after the UI backend has been created.
  UIRendererBackend runtime_renderer_backend{UIRendererBackend::SDLRenderer2};
  bool runtime_renderer_supports_viewports{false};
  bool runtime_renderer_used_fallback{false};
  std::string runtime_renderer_fallback_reason{};
  std::string runtime_opengl_vendor{};
  std::string runtime_opengl_renderer{};
  std::string runtime_opengl_version{};
  std::string runtime_opengl_glsl_version{};
  bool show_graphics_safe_mode_popup{false};
  bool graphics_safe_mode_popup_opened{false};

  // Popup window management.
  // When enabled, newly opened windows appear as floating popups instead of docking into the workspace.
  // This reduces clutter and makes it easy to drag windows out into detachable OS windows (multi-viewport).
  bool window_popup_first_mode{true};
  bool window_popup_auto_focus{true};
  float window_popup_cascade_step_px{24.0f};
  // Optional per-window override: id -> 0 (Docked), 1 (Popup). Missing = use defaults.
  std::unordered_map<std::string, int> window_launch_overrides;

  // UI-only (not persisted): runtime helpers for popup placement and focus-mode.
  bool window_focus_mode{false};
  std::unordered_map<std::string, bool> window_focus_restore;
  std::unordered_map<std::string, bool> window_open_prev;
  std::unordered_map<std::string, bool> window_popout_request;
  int window_popup_cascade_index{0};

  // --- Procedural dock layout synthesizer ---
  //
  // Generates a docking layout using DockBuilder from a compact parameter set
  // (seed + archetype). The resulting dock layout can be saved as a layout
  // profile (ImGui ini file).
  //
  // These values are persisted in ui_prefs.json.
  int ui_procedural_layout_seed{1337};
  // 0=Balanced, 1=Command, 2=Data, 3=Debug, 4=Forge
  int ui_procedural_layout_mode{0};
  // 0..1: how much randomness to inject into splits/window assignments.
  float ui_procedural_layout_variation{0.45f};
  bool ui_procedural_layout_include_tools{false};
  bool ui_procedural_layout_include_forge_panels{true};
  // Limit how many custom UI Forge panel windows are auto-docked (0 = all).
  int ui_procedural_layout_max_forge_panels{4};
  // When true, generating a layout also toggles windows on so the layout is visible immediately.
  bool ui_procedural_layout_auto_open_windows{true};
  // When enabled, generating a layout also saves the active layout profile ini file.
  bool ui_procedural_layout_autosave_profile{false};

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

  // One-shot request: rebuild a procedural dock layout next frame.
  bool request_generate_procedural_layout{false};

  // UI-only feedback (not persisted).
  std::string layout_profile_status;
  // UI-only time marker (ImGui::GetTime()) for layout_profile_status.
  // Useful for fading/auto-clearing status messages.
  double layout_profile_status_time{0.0};

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
  bool design_studio_show_heat_overlay{false};

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
  bool system_map_show_minimap{true};
  // --- System map planning / time preview ---
  // When enabled, the System Map draws a non-simulative "future overlay" that
  // predicts orbital positions for bodies and extrapolates ship motion from the
  // last-tick velocity vector. This is purely a UI planning tool.
  bool system_map_time_preview{false};
  // Relative offset from the current in-game time (days). Can be negative.
  float system_map_time_preview_days{30.0f};
  // Draw now->future connector arrows (bodies/ships).
  bool system_map_time_preview_vectors{true};
  // When false, only the selected ship / fleet leader gets a motion overlay.
  bool system_map_time_preview_all_ships{false};
  // Draw swept trails between now and the preview time.
  bool system_map_time_preview_trails{true};

  // --- System map heatmaps ---
  // Optional raster overlays that summarize "coverage" fields without drawing
  // hundreds of individual circles. These are UI-only preferences.
  bool system_map_sensor_heatmap{false};
  bool system_map_threat_heatmap{false};
  // Experimental: a line-of-sight shaded sensor heatmap that samples the
  // nebula/storm environment along the ray from each sensor source.
  //
  // This is currently visualization-only (it does not change simulation
  // detection mechanics).
  bool system_map_sensor_heatmap_raytrace{false};
  int system_map_sensor_raytrace_max_depth{6};
  float system_map_sensor_raytrace_error_threshold{0.06f};
  int system_map_sensor_raytrace_spp{1};
  int system_map_sensor_raytrace_los_samples{8};
  float system_map_sensor_raytrace_los_strength{0.85f};
  bool system_map_sensor_raytrace_debug{false};
  // Global opacity multiplier for heatmaps (0..1).
  float system_map_heatmap_opacity{0.35f};
  // Approximate number of cells across the map width (higher = sharper, slower).
  int system_map_heatmap_resolution{64};

  // --- System map nebula microfield overlay ---
  // Visualizes Simulation::system_nebula_density_at() as a subtle raster.
  // This helps players understand the new in-system "terrain" created by
  // nebula microfields.
  bool system_map_nebula_microfield_overlay{true};
  float system_map_nebula_overlay_opacity{0.22f};
  int system_map_nebula_overlay_resolution{84};

  
  // --- System map storm cell overlay ---
  // Visualizes Simulation::system_storm_intensity_at() as a subtle raster
  // (spatial storm fronts/calm pockets).
  bool system_map_storm_cell_overlay{true};
  float system_map_storm_overlay_opacity{0.18f};
  int system_map_storm_overlay_resolution{84};

  bool galaxy_map_starfield{true};
  bool galaxy_map_grid{false};
  bool galaxy_map_selected_route{true};
  bool galaxy_map_fuel_range{false};
  bool galaxy_map_show_minimap{true};

  // Shared tuning knobs.
  float map_starfield_density{1.0f};
  float map_starfield_parallax{0.15f};
  float map_grid_opacity{1.0f};
  float map_route_opacity{1.0f};

  // --- Procedural particle field (map dust) ---
  // Deterministic screen-space points with parallax, using
  // correlated multi-jittered sampling (CMJ) for a blue-noise-like distribution.
  // Pure map chrome: no simulation impact.
  bool galaxy_map_particle_field{true};
  bool system_map_particle_field{true};

  // Shared particle field tuning knobs.
  int map_particle_tile_px{256};
  int map_particle_particles_per_tile{64};
  int map_particle_layers{2};
  float map_particle_opacity{0.22f};
  float map_particle_base_radius_px{1.0f};
  float map_particle_radius_jitter_px{1.6f};
  float map_particle_twinkle_strength{0.55f};
  float map_particle_twinkle_speed{1.0f};
  bool map_particle_drift{true};
  float map_particle_drift_px_per_day{4.0f};
  float map_particle_layer0_parallax{0.10f};
  float map_particle_layer1_parallax{0.28f};
  float map_particle_layer2_parallax{0.45f};
  bool map_particle_sparkles{true};
  float map_particle_sparkle_chance{0.06f};
  float map_particle_sparkle_length_px{6.0f};
  bool map_particle_debug_tiles{false};

  // Runtime stats (not persisted).
  int map_particle_last_frame_layers_drawn{0};
  int map_particle_last_frame_tiles_drawn{0};
  int map_particle_last_frame_particles_drawn{0};

  // --- Map ray-marched nebula (experimental) ---
  // A signed-distance-field (SDF) raymarch renderer used as subtle background
  // chrome. It uses adaptive subdivision and deterministic stochastic sampling
  // so it stays stable while panning/zooming.
  bool map_raymarch_nebula{false};
  float map_raymarch_nebula_alpha{0.18f};
  float map_raymarch_nebula_parallax{0.06f};
  int map_raymarch_nebula_max_depth{6};
  float map_raymarch_nebula_error_threshold{0.05f};
  int map_raymarch_nebula_spp{1};
  int map_raymarch_nebula_max_steps{48};
  bool map_raymarch_nebula_animate{true};
  float map_raymarch_nebula_time_scale{0.20f};
  bool map_raymarch_nebula_debug{false};

  // --- Map procedural background engine (tile raster) ---
  //
  // A custom deterministic renderer that procedurally generates background
  // tiles (stars + optional nebula haze) on the CPU, uploads them as textures
  // to the active UI renderer backend, and then draws them as cached quads.
  //
  // This dramatically reduces per-frame CPU work when panning/zooming vs.
  // drawing thousands of primitives each frame.
  bool map_proc_render_engine{false};
  int map_proc_render_tile_px{256};
  int map_proc_render_cache_tiles{96};
  bool map_proc_render_nebula_enable{true};
  float map_proc_render_nebula_strength{0.35f};
  float map_proc_render_nebula_scale{1.0f};
  float map_proc_render_nebula_warp{0.70f};
  bool map_proc_render_debug_tiles{false};
  bool map_proc_render_clear_cache_requested{false};

  // Runtime stats (not persisted).
  int map_proc_render_stats_cache_tiles{0};
  int map_proc_render_stats_generated_this_frame{0};
  float map_proc_render_stats_gen_ms_this_frame{0.0f};
  float map_proc_render_stats_upload_ms_this_frame{0.0f};



  // --- Galaxy map procedural territory overlay ---
  //
  // UI-only: approximates faction influence using colonies and renders a
  // translucent "political map" overlay (a weighted Voronoi / power diagram)
  // on the galaxy map.
  bool galaxy_map_territory_overlay{false};
  bool galaxy_map_territory_fill{true};
  bool galaxy_map_territory_boundaries{true};
  float galaxy_map_territory_fill_opacity{0.16f};
  float galaxy_map_territory_boundary_opacity{0.42f};
  float galaxy_map_territory_boundary_thickness_px{1.6f};
  int galaxy_map_territory_tile_px{420};
  int galaxy_map_territory_cache_tiles{220};
  int galaxy_map_territory_samples_per_tile{28};
  float galaxy_map_territory_influence_base_spacing_mult{1.10f};
  float galaxy_map_territory_influence_pop_spacing_mult{0.28f};
  float galaxy_map_territory_influence_pop_log_bias{5.0f};
  float galaxy_map_territory_presence_falloff_spacing{2.0f};
  float galaxy_map_territory_dominance_softness_spacing{0.65f};
  bool galaxy_map_territory_contested_dither{true};
  float galaxy_map_territory_contested_threshold{0.22f};
  float galaxy_map_territory_contested_dither_strength{0.55f};
  bool galaxy_map_territory_debug_tiles{false};
  bool galaxy_map_territory_clear_cache_requested{false};

  // Runtime stats (not persisted).
  int galaxy_map_territory_stats_cache_tiles{0};
  int galaxy_map_territory_stats_tiles_used_this_frame{0};
  int galaxy_map_territory_stats_tiles_generated_this_frame{0};
  int galaxy_map_territory_stats_cells_drawn{0};
  float galaxy_map_territory_stats_gen_ms_this_frame{0.0f};
  // --- Procedural body sprites (system map) ---
  // CPU-rastered planet/gas giant/moon/star sprites cached as backend textures.
  // This gives the system map richer visuals without relying on external assets.
  bool system_map_body_sprites{true};
  int system_map_body_sprite_px{96};
  int system_map_body_sprite_cache{384};
  int system_map_body_sprite_light_steps{32};
  bool system_map_body_sprite_rings{true};
  float system_map_body_sprite_ring_chance{0.25f};
  float system_map_body_sprite_ambient{0.22f};
  float system_map_body_sprite_diffuse{1.0f};
  float system_map_body_sprite_specular{0.35f};
  float system_map_body_sprite_specular_power{24.0f};
  bool system_map_body_sprite_clear_cache_requested{false};

  // Runtime stats (not persisted).
  int system_map_body_sprite_stats_cache_sprites{0};
  int system_map_body_sprite_stats_generated_this_frame{0};
  float system_map_body_sprite_stats_gen_ms_this_frame{0.0f};
  float system_map_body_sprite_stats_upload_ms_this_frame{0.0f};

  // --- Procedural contact icons (system map) ---
  // CPU-rastered, cached sprite icons for ships, missiles, wrecks and anomalies.
  //
  // This is distinct from "procedural body sprites" (planets/stars). Contact icons
  // are drawn at a constant pixel size (for readability at any zoom) and rotated
  // to indicate motion.
  bool system_map_contact_icons{true};
  int system_map_contact_icon_px{64};
  int system_map_contact_icon_cache{768};
  float system_map_ship_icon_size_px{18.0f};
  bool system_map_ship_icon_thrusters{true};
  float system_map_ship_icon_thruster_opacity{0.60f};
  float system_map_ship_icon_thruster_length_px{14.0f};
  float system_map_ship_icon_thruster_width_px{7.0f};
  float system_map_missile_icon_size_px{10.0f};
  float system_map_wreck_icon_size_px{14.0f};
  float system_map_anomaly_icon_size_px{16.0f};
  bool system_map_anomaly_icon_pulse{true};
  bool system_map_contact_icon_debug_bounds{false};
  bool system_map_contact_icon_clear_cache_requested{false};

  // Runtime stats (not persisted).
  int system_map_contact_icon_stats_cache_sprites{0};
  int system_map_contact_icon_stats_generated_this_frame{0};
  float system_map_contact_icon_stats_gen_ms_this_frame{0.0f};
  float system_map_contact_icon_stats_upload_ms_this_frame{0.0f};

  // --- Procedural jump-point phenomena (system map) ---
  //
  // Visual layer for jump points that encodes their procedurally generated
  // phenomena (stability / turbulence / shear) into a cached sprite + optional
  // vector filaments.
  bool system_map_jump_phenomena{true};
  bool system_map_jump_phenomena_reveal_unsurveyed{false};
  int system_map_jump_phenomena_sprite_px{96};
  int system_map_jump_phenomena_cache{256};
  float system_map_jump_phenomena_size_mult{5.6f};
  float system_map_jump_phenomena_opacity{0.55f};
  bool system_map_jump_phenomena_animate{true};
  float system_map_jump_phenomena_anim_speed_cycles_per_day{0.14f};
  bool system_map_jump_phenomena_pulse{true};
  float system_map_jump_phenomena_pulse_cycles_per_day{0.08f};
  bool system_map_jump_phenomena_filaments{true};
  int system_map_jump_phenomena_filaments_max{6};
  float system_map_jump_phenomena_filament_strength{1.0f};
  bool system_map_jump_phenomena_debug_bounds{false};
  bool system_map_jump_phenomena_clear_cache_requested{false};

  // Procedural anomaly phenomena overlays (system map). This is a purely visual layer
  // that decorates discovered, unresolved anomalies with a deterministic procedural sprite
  // + optional filament arcs.
  bool system_map_anomaly_phenomena{true};
  int system_map_anomaly_phenomena_sprite_px{96};
  int system_map_anomaly_phenomena_cache{256};
  float system_map_anomaly_phenomena_size_mult{6.0f};
  float system_map_anomaly_phenomena_opacity{0.55f};
  bool system_map_anomaly_phenomena_animate{true};
  float system_map_anomaly_phenomena_anim_speed_cycles_per_day{0.12f};
  bool system_map_anomaly_phenomena_pulse{true};
  float system_map_anomaly_phenomena_pulse_cycles_per_day{0.07f};
  bool system_map_anomaly_phenomena_filaments{true};
  int system_map_anomaly_phenomena_filaments_max{7};
  float system_map_anomaly_phenomena_filament_strength{1.0f};
  bool system_map_anomaly_phenomena_glyph_overlay{true};
  float system_map_anomaly_phenomena_glyph_strength{0.65f};
  bool system_map_anomaly_phenomena_debug_bounds{false};
  bool system_map_anomaly_phenomena_clear_cache_requested{false};

  // Runtime stats (not persisted).
  int system_map_jump_phenomena_stats_cache_sprites{0};
  int system_map_jump_phenomena_stats_generated_this_frame{0};
  float system_map_jump_phenomena_stats_gen_ms_this_frame{0.0f};
  float system_map_jump_phenomena_stats_upload_ms_this_frame{0.0f};

  int system_map_anomaly_phenomena_stats_cache_sprites{0};
  int system_map_anomaly_phenomena_stats_generated_this_frame{0};
  float system_map_anomaly_phenomena_stats_gen_ms_this_frame{0.0f};
  float system_map_anomaly_phenomena_stats_upload_ms_this_frame{0.0f};

  // --- Procedural motion trails (system map) ---
  //
  // A UI-only vector FX layer that records recent positions of moving entities
  // and draws a fading trail behind them.
  //
  // Note: the engine itself is runtime-only (not serialized). These values are
  // persisted UI prefs.
  bool system_map_motion_trails{false};
  bool system_map_motion_trails_all_ships{false};
  bool system_map_motion_trails_missiles{false};
  float system_map_motion_trails_max_age_days{7.0f};
  float system_map_motion_trails_sample_hours{2.0f};
  float system_map_motion_trails_min_seg_px{4.0f};
  float system_map_motion_trails_thickness_px{2.0f};
  float system_map_motion_trails_alpha{0.55f};
  bool system_map_motion_trails_speed_brighten{true};
  bool system_map_motion_trails_clear_requested{false};

  // Runtime stats (not persisted).
  int system_map_motion_trails_stats_systems{0};
  int system_map_motion_trails_stats_tracks{0};
  int system_map_motion_trails_stats_points{0};
  int system_map_motion_trails_stats_pruned_points_this_frame{0};
  int system_map_motion_trails_stats_pruned_tracks_this_frame{0};

  // --- Procedural space-weather flow field (system map) ---
  //
  // A deterministic curl-noise streamline overlay used to visualize "space weather"
  // (nebula microfields / storm flow) with a lightweight cached renderer.
  //
  // Note: the engine cache is runtime-only. These values are persisted UI prefs.
  bool system_map_flow_field_overlay{true};
  bool system_map_flow_field_animate{true};
  bool system_map_flow_field_mask_nebula{true};
  bool system_map_flow_field_mask_storms{false};
  bool system_map_flow_field_debug_tiles{false};

  float system_map_flow_field_opacity{0.35f};
  float system_map_flow_field_thickness_px{1.25f};
  float system_map_flow_field_step_px{10.0f};
  float system_map_flow_field_highlight_wavelength_px{220.0f};
  float system_map_flow_field_animate_speed_cycles_per_day{0.08f};
  float system_map_flow_field_nebula_threshold{0.02f};
  float system_map_flow_field_storm_threshold{0.05f};
  float system_map_flow_field_scale_mkm{12000.0f};

  int system_map_flow_field_tile_px{420};
  int system_map_flow_field_cache_tiles{180};
  int system_map_flow_field_lines_per_tile{10};
  int system_map_flow_field_steps_per_line{48};

  bool system_map_flow_field_clear_requested{false};

  // Runtime stats (not persisted).
  int system_map_flow_field_stats_cache_tiles{0};
  int system_map_flow_field_stats_tiles_used{0};
  int system_map_flow_field_stats_tiles_generated{0};
  int system_map_flow_field_stats_lines_drawn{0};
  int system_map_flow_field_stats_segments_drawn{0};

  // --- Procedural gravity contours (system map) ---
  //
  // A cached iso-line overlay (marching squares) over a simplified
  // gravitational potential field derived from system body masses.
  //
  // Note: the engine cache is runtime-only. These values are persisted UI prefs.
  bool system_map_gravity_contours_overlay{false};
  bool system_map_gravity_contours_debug_tiles{false};

  float system_map_gravity_contours_opacity{0.22f};
  float system_map_gravity_contours_thickness_px{1.25f};

  int system_map_gravity_contours_tile_px{420};
  int system_map_gravity_contours_cache_tiles{180};
  int system_map_gravity_contours_samples_per_tile{32};
  int system_map_gravity_contours_levels{10};
  float system_map_gravity_contours_level_spacing_decades{0.35f};
  float system_map_gravity_contours_level_offset_decades{0.0f};
  float system_map_gravity_contours_softening_min_mkm{0.05f};
  float system_map_gravity_contours_softening_radius_mult{2.0f};

  bool system_map_gravity_contours_clear_requested{false};

  // Runtime stats (not persisted).
  int system_map_gravity_contours_stats_cache_tiles{0};
  int system_map_gravity_contours_stats_tiles_used{0};
  int system_map_gravity_contours_stats_tiles_generated{0};
  int system_map_gravity_contours_stats_segments_drawn{0};

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
  int new_game_random_galaxy_shape{0}; // see RandomGalaxyShape
  int new_game_random_placement_style{0}; // see RandomPlacementStyle
  int new_game_random_placement_quality{24};
  int new_game_random_jump_network_style{0}; // see RandomJumpNetworkStyle
  float new_game_random_jump_density{1.0f};

  bool new_game_random_enable_regions{true};
  int new_game_random_num_regions{-1}; // -1 = auto
  int new_game_random_ai_empires{-1}; // -1 = auto
  bool new_game_random_enable_pirates{true};
  float new_game_random_pirate_strength{1.0f};

  // Independent neutral outposts (procedural minor faction).
  bool new_game_random_enable_independents{true};
  int new_game_random_num_independent_outposts{-1}; // -1 = auto

  // Keep the player home system readable by clamping nebula density.
  bool new_game_random_ensure_clear_home{true};

  // --- Random galaxy preview (New Game modal) ---
  // These are UI-only visualization toggles.
  bool new_game_preview_show_jumps{true};
  bool new_game_preview_show_labels{true};
  bool new_game_preview_show_regions{true};
  bool new_game_preview_show_nebula{true};
  bool new_game_preview_color_by_component{false};
  bool new_game_preview_show_chokepoints{false};

  // --- Seed explorer (New Game modal) ---
  // 0=Balanced, 1=Readable (few crossings), 2=Chokepoints, 3=Webby (redundant routes).
  int new_game_seed_search_objective{0};
  int new_game_seed_search_tries{64};
  int new_game_seed_search_steps_per_frame{8};
};

} // namespace nebula4x::ui