#include "ui/app.h"

#include <imgui.h>
// DockBuilder* API lives in the internal header (docking branch).
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include <imgui_impl_sdlrenderer2.h>
#if NEBULA4X_UI_RENDERER_OPENGL2
#include <imgui_impl_opengl2.h>
#endif

#include "nebula4x/util/json.h"
#include "nebula4x/util/log.h"
#include "nebula4x/util/trace_events.h"
#include "nebula4x/util/time.h"

#include "nebula4x/core/serialization.h"
#include "nebula4x/core/trade_network.h"
#include "nebula4x/util/file_io.h"
#include "ui/panels.h"
#include "ui/new_game_modal.h"
#include "ui/screen_reader.h"
#include "ui/guided_tour.h"
#include "ui/economy_window.h"
#include "ui/research_roadmap_window.h"
#include "ui/planner_window.h"
#include "ui/regions_window.h"
#include "ui/security_planner_window.h"
#include "ui/freight_window.h"
#include "ui/mine_window.h"
#include "ui/fuel_window.h"
#include "ui/salvage_window.h"
#include "ui/contracts_window.h"
#include "ui/sustainment_window.h"
#include "ui/repair_planner_window.h"
#include "ui/maintenance_planner_window.h"
#include "ui/fleet_manager_window.h"
#include "ui/advisor_window.h"
#include "ui/colony_profiles_window.h"
#include "ui/ship_profiles_window.h"
#include "ui/automation_center_window.h"
#include "ui/troop_window.h"
#include "ui/colonist_window.h"
#include "ui/terraforming_window.h"
#include "ui/shipyard_targets_window.h"
#include "ui/survey_network_window.h"
#include "ui/time_warp_window.h"
#include "ui/production_window.h"
#include "ui/galaxy_map.h"
#include "ui/system_map.h"
#include "ui/timeline_window.h"
#include "ui/design_studio_window.h"
#include "ui/balance_lab_window.h"
#include "ui/battle_forecast_window.h"
#include "ui/procgen_atlas_window.h"
#include "ui/star_atlas_window.h"
#include "ui/intel_window.h"
#include "ui/intel_notebook_window.h"
#include "ui/victory_window.h"
#include "ui/diplomacy_window.h"
#include "ui/save_tools_window.h"
#include "ui/time_machine_window.h"
#include "ui/compare_window.h"
#include "ui/navigation.h"
#include "ui/hotkeys.h"
#include "ui/navigator_window.h"
#include "ui/omni_search_window.h"
#include "ui/json_explorer_window.h"
#include "ui/content_validation_window.h"
#include "ui/state_doctor_window.h"
#include "ui/trace_viewer_window.h"
#include "ui/entity_inspector_window.h"
#include "ui/reference_graph_window.h"
#include "ui/watchboard_window.h"
#include "ui/watchboard_alerts.h"
#include "ui/notifications.h"
#include "ui/notifications_window.h"
#include "ui/data_lenses_window.h"
#include "ui/dashboards_window.h"
#include "ui/pivot_tables_window.h"
#include "ui/ui_forge_window.h"
#include "ui/context_forge_window.h"
#include "ui/layout_profiles.h"
#include "ui/layout_profiles_window.h"
#include "ui/procedural_theme.h"
#include "ui/procedural_layout.h"
#include "ui/window_management.h"
#include "ui/window_manager_window.h"

namespace nebula4x::ui {

namespace {

// Default ImGui font size is 13px. Keep it as the baseline so ui_scale=1.0 matches
// prior behavior while allowing crisp HiDPI rasterization.
constexpr float kDefaultBaseFontSizePx = 13.0f;
constexpr float kMinUiScale = 0.65f;
constexpr float kMaxUiScale = 2.5f;
constexpr int kMinAtlasFontPx = 8;
constexpr int kMaxAtlasFontPx = 64;

float clamp_finite(float v, float lo, float hi, float fallback) {
  if (!std::isfinite(v)) return fallback;
  return std::clamp(v, lo, hi);
}

float effective_framebuffer_scale(const ImGuiIO& io) {
  // Prefer uniform scale. When they differ, take the smaller scale to avoid
  // over-allocating the atlas.
  float sx = clamp_finite(io.DisplayFramebufferScale.x, 0.1f, 8.0f, 1.0f);
  float sy = clamp_finite(io.DisplayFramebufferScale.y, 0.1f, 8.0f, sx);
  return std::min(sx, sy);
}

void recreate_imgui_device_objects(UIRendererBackend backend) {
  switch (backend) {
    case UIRendererBackend::SDLRenderer2:
      ImGui_ImplSDLRenderer2_DestroyDeviceObjects();
      (void)ImGui_ImplSDLRenderer2_CreateDeviceObjects();
      break;
#if NEBULA4X_UI_RENDERER_OPENGL2
    case UIRendererBackend::OpenGL2:
      ImGui_ImplOpenGL2_DestroyDeviceObjects();
      (void)ImGui_ImplOpenGL2_CreateDeviceObjects();
      break;
#endif
    default:
      break;
  }
}

} // namespace

App::App(Simulation sim) : sim_(std::move(sim)) {
  last_seen_state_generation_ = sim_.state_generation();
  if (!sim_.state().colonies.empty()) {
    selected_colony_ = sim_.state().colonies.begin()->first;
    if (const auto* c = find_ptr(sim_.state().colonies, selected_colony_)) {
      selected_body_ = c->body_id;
    }
  }

  // Best-effort auto-load of UI preferences (colors/layout).
  std::string err;
  (void)load_ui_prefs(ui_prefs_path_, &err);

  // Initialize the ImGui ini file path from the loaded prefs.
  update_imgui_ini_path_from_ui();


  // Optional: auto-start performance tracing for NEBULA4X_TRACE_SCOPE.
  if (ui_.trace_viewer_autostart) {
    ui_.trace_viewer_max_events = std::clamp(ui_.trace_viewer_max_events, 0, 500000);
    nebula4x::trace::TraceRecorder::instance().set_max_events(
        static_cast<std::size_t>(ui_.trace_viewer_max_events));
    nebula4x::trace::TraceRecorder::instance().start("nebula4x");
  }
}

void App::set_renderer_context(UIRendererBackend backend, SDL_Renderer* sdl_renderer) {
  proc_render_engine_.set_backend(backend, sdl_renderer);
  proc_body_sprite_engine_.set_backend(backend, sdl_renderer);
  proc_icon_sprite_engine_.set_backend(backend, sdl_renderer);
  proc_jump_phenomena_sprite_engine_.set_backend(backend, sdl_renderer);
  proc_anomaly_phenomena_sprite_engine_.set_backend(backend, sdl_renderer);
}

void App::shutdown_renderer_resources() {
  proc_render_engine_.shutdown();
  proc_body_sprite_engine_.shutdown();
  proc_icon_sprite_engine_.shutdown();
  proc_jump_phenomena_sprite_engine_.shutdown();
  proc_anomaly_phenomena_sprite_engine_.shutdown();
}

const char* App::imgui_ini_filename() const {
  return imgui_ini_path_.empty() ? nullptr : imgui_ini_path_.c_str();
}

void App::on_event(const SDL_Event& /*e*/) {
  // Reserved for future (resize, etc.)
}

void App::update_imgui_ini_path_from_ui() {
  // Ensure a usable directory.
  if (ui_.layout_profiles_dir[0] == '\0') {
    std::snprintf(ui_.layout_profiles_dir, sizeof(ui_.layout_profiles_dir), "%s", "ui_layouts");
    ui_.layout_profiles_dir[sizeof(ui_.layout_profiles_dir) - 1] = '\0';
  }

  const std::string safe_profile = sanitize_layout_profile_name(ui_.layout_profile);
  if (safe_profile != ui_.layout_profile) {
    std::snprintf(ui_.layout_profile, sizeof(ui_.layout_profile), "%s", safe_profile.c_str());
    ui_.layout_profile[sizeof(ui_.layout_profile) - 1] = '\0';
  }

  imgui_ini_path_ = make_layout_profile_ini_path(ui_.layout_profiles_dir, ui_.layout_profile);
  if (imgui_ini_path_.empty()) imgui_ini_path_ = "ui_layouts/default.ini";
}

void App::apply_imgui_font_overrides() {
  ImGuiIO& io = ImGui::GetIO();

  ui_.ui_scale = std::clamp(ui_.ui_scale, kMinUiScale, kMaxUiScale);

  const float fb = std::clamp(effective_framebuffer_scale(io), 0.75f, 4.0f);
  const float desired_px = std::clamp(kDefaultBaseFontSizePx * ui_.ui_scale,
                                      static_cast<float>(kMinAtlasFontPx),
                                      static_cast<float>(kMaxAtlasFontPx));

  // Rasterize at an integer pixel size for stable glyph metrics; use a small
  // global scale factor to preserve fractional sizes.
  const int atlas_px = std::clamp(static_cast<int>(std::lround(desired_px)), kMinAtlasFontPx, kMaxAtlasFontPx);
  const float global_scale = desired_px / static_cast<float>(atlas_px);

  const bool need_rebuild =
      (atlas_px != last_font_atlas_size_px_) || (std::fabs(fb - last_font_rasterizer_density_) > 1e-3f) ||
      io.Fonts->Fonts.empty();

  if (need_rebuild) {
    io.Fonts->Clear();

    ImFontConfig cfg;
    cfg.SizePixels = static_cast<float>(atlas_px);
    cfg.RasterizerDensity = fb;

    // Keep default oversampling and hinting; RasterizerDensity is the key for
    // crisp HiDPI text without changing logical UI sizes.
    io.FontDefault = io.Fonts->AddFontDefault(&cfg);
    io.Fonts->Build();

    // Force the active renderer backend to re-upload the font atlas immediately
    // (this frame), avoiding stale texture references.
    recreate_imgui_device_objects(ui_.runtime_renderer_backend);

    last_font_atlas_size_px_ = atlas_px;
    last_font_rasterizer_density_ = fb;
  }

  io.FontGlobalScale = global_scale;
}

void App::pre_frame() {
  // If there is no ImGui context yet, do nothing.
  if (ImGui::GetCurrentContext() == nullptr) return;

  update_imgui_ini_path_from_ui();

  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = imgui_ini_filename();

  // Apply persisted docking and viewport behavior before ImGui::NewFrame().
  io.ConfigDockingWithShift = ui_.docking_with_shift;
  io.ConfigDockingAlwaysTabBar = ui_.docking_always_tab_bar;
  io.ConfigDockingTransparentPayload = ui_.docking_transparent_payload;

#ifdef IMGUI_HAS_VIEWPORT
  if (!ui_.runtime_renderer_supports_viewports) {
    // Active backend cannot render platform windows; ensure viewports are off.
    ui_.viewports_enable = false;
    io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
  } else {
    // Multi-viewport enables detachable OS windows for floating ImGui windows.
    if (ui_.viewports_enable) {
      io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    } else {
      io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
    }
    io.ConfigViewportsNoTaskBarIcon = ui_.viewports_no_taskbar_icon;
    io.ConfigViewportsNoAutoMerge = ui_.viewports_no_auto_merge;
    io.ConfigViewportsNoDecoration = ui_.viewports_no_decoration;
  }
#endif

  // HiDPI-aware font atlas (must happen before ImGui::NewFrame()).
  apply_imgui_font_overrides();

  // Reload request or ini path change: load before NewFrame for best results.
  const bool path_changed = (imgui_ini_path_ != last_imgui_ini_path_applied_);
  const bool reload = ui_.request_reload_layout_profile || path_changed;
  if (!reload) return;

  ui_.request_reload_layout_profile = false;
  last_imgui_ini_path_applied_ = imgui_ini_path_;

  // Ensure the directory exists so ImGui can save into it.
  if (io.IniFilename && io.IniFilename[0]) {
    std::error_code ec;
    const std::filesystem::path p(io.IniFilename);
    if (p.has_parent_path()) {
      std::filesystem::create_directories(p.parent_path(), ec);
    }
  }

  // Load the ini for this profile.
  bool has_file = false;
  if (io.IniFilename && io.IniFilename[0]) {
    std::error_code ec;
    has_file = std::filesystem::exists(io.IniFilename, ec) && !ec;
  }

  // Clear prior docking state to avoid mixing layouts.
  ImGui::LoadIniSettingsFromMemory("");

  if (has_file) {
    ImGui::LoadIniSettingsFromDisk(io.IniFilename);
  }

  dock_layout_checked_ini_ = true;
  dock_layout_has_existing_ini_ = has_file;

  // Force the dockspace to rebuild its default layout if needed.
  dock_layout_initialized_ = false;
}


static void draw_graphics_safe_mode_popup(UIState& ui) {
  if (!ui.show_graphics_safe_mode_popup) return;

  if (!ui.graphics_safe_mode_popup_opened) {
    ImGui::OpenPopup("Graphics Safe Mode");
    ui.graphics_safe_mode_popup_opened = true;
  }

  bool open = true;
  if (ImGui::BeginPopupModal("Graphics Safe Mode", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextWrapped("Nebula4X started in a graphics safe mode.");
    ImGui::TextWrapped(
        "This usually means an OpenGL context could not be created, so the UI is running with an "
        "alternate renderer backend.");
    ImGui::Separator();

    ImGui::Text("Active backend: %s", ui_renderer_backend_name(ui.runtime_renderer_backend));

    if (!ui.runtime_renderer_fallback_reason.empty()) {
      ImGui::SeparatorText("Details");
      ImGui::TextWrapped("%s", ui.runtime_renderer_fallback_reason.c_str());
    }

    if (!ui.runtime_opengl_vendor.empty()) {
      ImGui::SeparatorText("OpenGL Driver Info");
      ImGui::Text("Vendor:   %s", ui.runtime_opengl_vendor.c_str());
      ImGui::Text("Renderer: %s", ui.runtime_opengl_renderer.c_str());
      ImGui::Text("Version:  %s", ui.runtime_opengl_version.c_str());
      if (!ui.runtime_opengl_glsl_version.empty()) {
        ImGui::Text("GLSL:     %s", ui.runtime_opengl_glsl_version.c_str());
      }
    }

    ImGui::SeparatorText("Tips");
    ImGui::BulletText("Update your GPU driver.");
    ImGui::BulletText("If you're running over Remote Desktop, try launching locally.");
    ImGui::BulletText("Force safe mode: --renderer sdl (or set NEBULA4X_RENDERER=sdl).");
    ImGui::BulletText("Retry OpenGL: --renderer opengl.");

    // Copy a diagnostics bundle for bug reports.
    std::string copy;
    copy.reserve(512 + ui.runtime_renderer_fallback_reason.size());
    copy += "Nebula4X Graphics Safe Mode\n";
    copy += "Backend: ";
    copy += ui_renderer_backend_name(ui.runtime_renderer_backend);
    copy += "\n";
    if (!ui.runtime_opengl_vendor.empty()) {
      copy += "GL_VENDOR: ";
      copy += ui.runtime_opengl_vendor;
      copy += "\nGL_RENDERER: ";
      copy += ui.runtime_opengl_renderer;
      copy += "\nGL_VERSION: ";
      copy += ui.runtime_opengl_version;
      if (!ui.runtime_opengl_glsl_version.empty()) {
        copy += "\nGLSL: ";
        copy += ui.runtime_opengl_glsl_version;
      }
      copy += "\n";
    }
    if (!ui.runtime_renderer_fallback_reason.empty()) {
      copy += "\n";
      copy += ui.runtime_renderer_fallback_reason;
      copy += "\n";
    }

    if (ImGui::Button("Copy details")) {
      ImGui::SetClipboardText(copy.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) {
      ui.show_graphics_safe_mode_popup = false;
      ui.graphics_safe_mode_popup_opened = false;
      ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
  }

  if (!open) {
    ui.show_graphics_safe_mode_popup = false;
    ui.graphics_safe_mode_popup_opened = false;
  }
}


void App::frame() {
  NEBULA4X_TRACE_SCOPE("ui.frame", "ui");
  auto sync_on_state_generation_change = [&]() {
    const std::uint64_t gen = sim_.state_generation();
    if (gen == last_seen_state_generation_) return;

    last_seen_state_generation_ = gen;

    // Clear any selection that might reference entities from the previous state.
    selected_ship_ = kInvalidId;
    selected_colony_ = kInvalidId;
    selected_body_ = kInvalidId;
    if (!sim_.state().colonies.empty()) {
      selected_colony_ = sim_.state().colonies.begin()->first;
      if (const auto* c = find_ptr(sim_.state().colonies, selected_colony_)) {
        selected_body_ = c->body_id;
      }
    }

    // Selection Navigator state is UI-only; reset it so we don't carry stale entity IDs
    // across a load/new-game.
    nav_history_reset(ui_);
    ui_.nav_bookmarks.clear();
    ui_.nav_next_bookmark_id = 1;

    // The Notification Center is UI-only and holds pointers/IDs into the prior
    // simulation snapshot; clear it on load.
    notifications_reset(ui_);

    // Clear UI-only motion trails; entity IDs may be reused across loads.
    proc_trail_engine_.clear_all();

    // Reset autosave cadence when the underlying state is replaced.
    autosave_mgr_.reset();
    ui_.last_autosave_game_path.clear();
    ui_.last_autosave_game_error.clear();
    victory_window_autoopened_ = false;
  };

  // UI scaling is applied in pre_frame() via the font atlas rebuild.
  ui_.ui_scale = std::clamp(ui_.ui_scale, kMinUiScale, kMaxUiScale);

  // Per-frame bookkeeping for procedural render caches.
  proc_render_engine_.begin_frame();
  proc_particle_field_engine_.begin_frame(sim_time_days(sim_.state()), ImGui::GetTime());
  proc_body_sprite_engine_.begin_frame();
  proc_icon_sprite_engine_.begin_frame();
  proc_jump_phenomena_sprite_engine_.begin_frame();
  proc_anomaly_phenomena_sprite_engine_.begin_frame();
  proc_trail_engine_.begin_frame(sim_time_days(sim_.state()));
  proc_flow_field_engine_.begin_frame(sim_time_days(sim_.state()));
  proc_gravity_contour_engine_.begin_frame(sim_time_days(sim_.state()));
  proc_territory_field_engine_.begin_frame();

  // UI action: clear cached motion trails (runtime only, not persisted).
  if (ui_.system_map_motion_trails_clear_requested) {
    proc_trail_engine_.clear_all();
    ui_.system_map_motion_trails_clear_requested = false;
  }

  // UI action: clear cached flow field tiles (runtime only, not persisted).
  if (ui_.system_map_flow_field_clear_requested) {
    proc_flow_field_engine_.clear();
    ui_.system_map_flow_field_clear_requested = false;
  }

  // UI action: clear cached gravity contour tiles (runtime only, not persisted).
  if (ui_.system_map_gravity_contours_clear_requested) {
    proc_gravity_contour_engine_.clear();
    ui_.system_map_gravity_contours_clear_requested = false;
  }

  // UI action: clear cached galaxy territory tiles (runtime only, not persisted).
  if (ui_.galaxy_map_territory_clear_cache_requested) {
    proc_territory_field_engine_.clear();
    ui_.galaxy_map_territory_clear_cache_requested = false;
  }

  // Apply last-frame style overrides so the menu/settings windows reflect them.
  apply_imgui_style_overrides();

  // Keep the in-game screen reader/narration engine in sync with UI prefs.
  auto sync_screen_reader = [&]() {
    ScreenReader& sr = ScreenReader::instance();
    sr.set_enabled(ui_.screen_reader_enabled);
    sr.set_rate(ui_.screen_reader_rate);
    sr.set_volume(ui_.screen_reader_volume);
    sr.set_hover_delay(ui_.screen_reader_hover_delay_sec);
    sr.set_speak_focus(ui_.screen_reader_speak_focus);
    sr.set_speak_hover(ui_.screen_reader_speak_hover);
    sr.set_speak_windows(ui_.screen_reader_speak_windows);
    sr.set_speak_toasts(ui_.screen_reader_speak_toasts);
    sr.set_speak_selection(ui_.screen_reader_speak_selection);
  };
  sync_screen_reader();
  ScreenReader::instance().begin_frame();

  // Snapshot the current navigation target so we can record selection changes into
  // the selection-history stack later in the frame.
  const NavTarget nav_before = current_nav_target(sim_, selected_ship_, selected_colony_, selected_body_);

  // --- Global keyboard shortcuts (UI focus) ---
  {
    const ImGuiIO& io = ImGui::GetIO();

    // If the settings window isn't open, abort any pending hotkey capture.
    // This prevents global hotkeys from being permanently suppressed if the
    // user closes Settings mid-capture.
    if (!ui_.show_settings_window) {
      ui_.hotkeys_capture_id.clear();
    }
    ui_.hotkeys_capture_active = !ui_.hotkeys_capture_id.empty();

    // Avoid stealing shortcuts when the user is typing in an input field, or while
    // the Hotkeys editor is capturing a new chord.
    if (ui_.hotkeys_enabled && !ui_.hotkeys_capture_active && !io.WantTextInput) {
      auto hk = [&](const char* id) { return hotkey_pressed(ui_, id, io); };

      // Command palette / help.
      if (hk("ui.command_console")) ui_.show_command_palette = true;
      if (hk("ui.toggle.omnisearch")) ui_.show_omni_search_window = !ui_.show_omni_search_window;
      if (hk("ui.toggle.entity_inspector")) ui_.show_entity_inspector_window = !ui_.show_entity_inspector_window;
      if (hk("ui.toggle.reference_graph")) ui_.show_reference_graph_window = !ui_.show_reference_graph_window;
      if (hk("ui.toggle.time_machine")) ui_.show_time_machine_window = !ui_.show_time_machine_window;
      if (hk("ui.toggle.compare")) ui_.show_compare_window = !ui_.show_compare_window;
      if (hk("ui.toggle.navigator")) ui_.show_navigator_window = !ui_.show_navigator_window;
      if (hk("ui.toggle.advisor")) ui_.show_advisor_window = !ui_.show_advisor_window;
      if (hk("ui.toggle.colony_profiles")) ui_.show_colony_profiles_window = !ui_.show_colony_profiles_window;
      if (hk("ui.toggle.ship_profiles")) ui_.show_ship_profiles_window = !ui_.show_ship_profiles_window;
      if (hk("ui.toggle.shipyard_targets")) ui_.show_shipyard_targets_window = !ui_.show_shipyard_targets_window;
      if (hk("ui.toggle.survey_network")) ui_.show_survey_network_window = !ui_.show_survey_network_window;
      if (hk("ui.toggle.regions")) ui_.show_regions_window = !ui_.show_regions_window;
      if (hk("ui.toggle.fleet_manager")) ui_.show_fleet_manager_window = !ui_.show_fleet_manager_window;
      if (hk("ui.toggle.content_validation")) ui_.show_content_validation_window = !ui_.show_content_validation_window;
      if (hk("ui.toggle.state_doctor")) ui_.show_state_doctor_window = !ui_.show_state_doctor_window;
      if (hk("ui.toggle.help")) ui_.show_help_window = !ui_.show_help_window;
      if (hk("ui.toggle.tours")) {
        // Guided tours overlay toggle.
        ui_.tour_active = !ui_.tour_active;
        if (ui_.tour_active) {
          // Start/resume the currently selected tour; hide Help so the spotlight is not obscured.
          ui_.show_help_window = false;
          // Clamp indexes defensively.
          if (ui_.tour_active_index < 0) ui_.tour_active_index = 0;
          if (ui_.tour_step_index < 0) ui_.tour_step_index = 0;
        }
      }
      if (hk("ui.toggle.notifications")) ui_.show_notifications_window = !ui_.show_notifications_window;
      if (hk("ui.toggle.settings")) ui_.show_settings_window = !ui_.show_settings_window;

      // Selection history navigation.
      if (hk("nav.back")) {
        nav_history_back(sim_, ui_, selected_ship_, selected_colony_, selected_body_, ui_.nav_open_windows_on_jump);
      }
      if (hk("nav.forward")) {
        nav_history_forward(sim_, ui_, selected_ship_, selected_colony_, selected_body_, ui_.nav_open_windows_on_jump);
      }

      // Quick window toggles.
      if (hk("ui.toggle.controls")) ui_.show_controls_window = !ui_.show_controls_window;
      if (hk("ui.toggle.map")) ui_.show_map_window = !ui_.show_map_window;
      if (hk("ui.toggle.details")) ui_.show_details_window = !ui_.show_details_window;
      if (hk("ui.toggle.directory")) ui_.show_directory_window = !ui_.show_directory_window;
      if (hk("ui.toggle.economy")) ui_.show_economy_window = !ui_.show_economy_window;
      if (hk("ui.toggle.production")) ui_.show_production_window = !ui_.show_production_window;
      if (hk("ui.toggle.timeline")) ui_.show_timeline_window = !ui_.show_timeline_window;
      if (hk("ui.toggle.design_studio")) ui_.show_design_studio_window = !ui_.show_design_studio_window;
      if (hk("ui.toggle.intel")) ui_.show_intel_window = !ui_.show_intel_window;
      if (hk("ui.toggle.intel_notebook")) ui_.show_intel_notebook_window = !ui_.show_intel_notebook_window;
      if (hk("ui.toggle.diplomacy")) ui_.show_diplomacy_window = !ui_.show_diplomacy_window;
      if (hk("ui.toggle.layout_profiles")) ui_.show_layout_profiles_window = !ui_.show_layout_profiles_window;
      if (hk("ui.toggle.window_manager")) ui_.show_window_manager_window = !ui_.show_window_manager_window;
      if (hk("ui.toggle.focus_mode")) toggle_focus_mode(ui_);
      if (hk("ui.toggle.ui_forge")) ui_.show_ui_forge_window = !ui_.show_ui_forge_window;
      if (hk("ui.toggle.context_forge")) ui_.show_context_forge_window = !ui_.show_context_forge_window;

      // Screen reader / narration.
      if (hk("accessibility.toggle_screen_reader")) {
        ui_.screen_reader_enabled = !ui_.screen_reader_enabled;
        sync_screen_reader();
        if (ui_.screen_reader_enabled) {
          ScreenReader::instance().speak("Screen reader enabled", true);
        }
      }
      if (hk("accessibility.repeat_last")) {
        ScreenReader::instance().repeat_last();
      }

      // Save/load.
      if (hk("game.save")) {
        try {
          nebula4x::write_text_file(save_path_, serialize_game_to_json(sim_.state()));
          nebula4x::log::info("Saved game.");
        } catch (const std::exception& e) {
          nebula4x::log::error(std::string("Save failed: ") + e.what());
        }
      }
      if (hk("game.load")) {
        try {
          sim_.load_game(deserialize_game_from_json(nebula4x::read_text_file(load_path_)));
          selected_ship_ = kInvalidId;
          selected_colony_ = kInvalidId;
          selected_body_ = kInvalidId;
          nebula4x::log::info("Loaded game.");
        } catch (const std::exception& e) {
          nebula4x::log::error(std::string("Load failed: ") + e.what());
        }
      }

      // Turn advance.
      if (hk("time.advance_1")) sim_.advance_days(1);
      if (hk("time.advance_5")) sim_.advance_days(5);
      if (hk("time.advance_30")) sim_.advance_days(30);
    }
  }

  UIPrefActions actions;
  draw_main_menu(sim_, ui_, save_path_, load_path_, ui_prefs_path_, actions);

    draw_graphics_safe_mode_popup(ui_);
  if (ui_.show_settings_window) {
    draw_settings_window(ui_, ui_prefs_path_, actions);
  }

  // Menu/settings may have modified narration prefs.
  sync_screen_reader();

  // New Game (scenario picker) modal.
  draw_new_game_modal(sim_, ui_);

  // Guided tours can open/bring-to-front windows for the current step.
  // This must happen before we draw the workspace windows.
  guided_tour_preframe(ui_);

  // If the user loaded/created a new game via the menu, immediately clear
  // any stale selections before drawing the rest of the UI.
  sync_on_state_generation_change();

  // Handle actions after both the menu and settings window have had a chance
  // to set action flags.
  if (actions.reset_ui_theme) reset_ui_theme_defaults();
  if (actions.reset_window_layout) reset_window_layout_defaults();
  if (ui_.request_reset_window_layout) {
    ui_.request_reset_window_layout = false;
    reset_window_layout_defaults();
  }

  if (actions.load_ui_prefs) {
    std::string err;
    if (!load_ui_prefs(ui_prefs_path_, &err)) {
      nebula4x::log::warn(std::string("Load UI prefs failed: ") + (err.empty() ? std::string("(unknown)") : err));
    } else {
      nebula4x::log::info("Loaded UI prefs.");
    }
  }
  if (actions.save_ui_prefs) {
    std::string err;
    if (!save_ui_prefs(ui_prefs_path_, &err)) {
      nebula4x::log::warn(std::string("Save UI prefs failed: ") + (err.empty() ? std::string("(unknown)") : err));
    } else {
      nebula4x::log::info("Saved UI prefs.");
    }
  }

  // Re-apply style overrides in case theme values changed this frame.
  apply_imgui_style_overrides();

  // Create a fullscreen dockspace so the user can rearrange panels.
  draw_dockspace();

  // Update the persistent Notification Center (and watchboard alerts) before
  // drawing windows so badge counts and the inbox update immediately on turn
  // advancement.
  notifications_ingest_sim_events(sim_, ui_);
  update_watchboard_alert_toasts(sim_, ui_, hud_);

  // Track selection changes for narration.
  const Id prev_selected_ship = selected_ship_;
  const Id prev_selected_colony = selected_colony_;
  const Id prev_selected_body = selected_body_;

  // Primary workspace windows (dockable).
  if (ui_.show_controls_window) {
    ImGui::SetNextWindowSize(ImVec2(320, 720), ImGuiCond_FirstUseEver);
    prepare_window_for_draw(ui_, "controls");
    if (ImGui::Begin("Controls", &ui_.show_controls_window)) {
      draw_left_sidebar(sim_, ui_, selected_ship_, selected_colony_);
    }
    ImGui::End();
  }

  if (ui_.show_map_window) {
    ImGui::SetNextWindowSize(ImVec2(900, 720), ImGuiCond_FirstUseEver);
    prepare_window_for_draw(ui_, "map");
    if (ImGui::Begin("Map", &ui_.show_map_window)) {
      if (ImGui::BeginTabBar("map_tabs")) {
        const MapTab req = ui_.request_map_tab;

        ImGuiTabItemFlags sys_flags = 0;
        ImGuiTabItemFlags gal_flags = 0;
        if (req == MapTab::System) sys_flags |= ImGuiTabItemFlags_SetSelected;
        if (req == MapTab::Galaxy) gal_flags |= ImGuiTabItemFlags_SetSelected;

        if (ImGui::BeginTabItem("System", nullptr, sys_flags)) {
          draw_system_map(sim_, ui_, selected_ship_, selected_colony_, selected_body_, map_zoom_, map_pan_,
                          &proc_render_engine_, &proc_particle_field_engine_, &proc_body_sprite_engine_,
                          &proc_icon_sprite_engine_, &proc_jump_phenomena_sprite_engine_, &proc_anomaly_phenomena_sprite_engine_,
                          &proc_trail_engine_,
                          &proc_flow_field_engine_,
                          &proc_gravity_contour_engine_);
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Galaxy", nullptr, gal_flags)) {
          draw_galaxy_map(sim_, ui_, selected_ship_, galaxy_zoom_, galaxy_pan_, &proc_render_engine_, &proc_particle_field_engine_, &proc_territory_field_engine_);
          ImGui::EndTabItem();
        }
        ImGui::EndTabBar();

        // Consume tab request if we drew the tab bar.
        if (req != MapTab::None) ui_.request_map_tab = MapTab::None;
      }
    }
    ImGui::End();
  }

  if (ui_.show_details_window) {
    ImGui::SetNextWindowSize(ImVec2(360, 720), ImGuiCond_FirstUseEver);
    prepare_window_for_draw(ui_, "details");
    if (ImGui::Begin("Details", &ui_.show_details_window)) {
      draw_right_sidebar(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
    }
    ImGui::End();
  }

  // Optional secondary windows (also dockable).
  if (ui_.show_directory_window) {
    prepare_window_for_draw(ui_, "directory");
    draw_directory_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_production_window) {
    prepare_window_for_draw(ui_, "production");
    draw_production_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_economy_window) {
    prepare_window_for_draw(ui_, "economy");
    draw_economy_window(sim_, ui_, selected_colony_, selected_body_);
  if (ui_.show_research_roadmap_window)
    draw_research_roadmap_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_planner_window) {
    prepare_window_for_draw(ui_, "planner");
    draw_planner_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_regions_window) {
    prepare_window_for_draw(ui_, "regions");
    draw_regions_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_security_planner_window) {
    prepare_window_for_draw(ui_, "security_planner");
    draw_security_planner_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_freight_window) {
    prepare_window_for_draw(ui_, "freight");
    draw_freight_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_mine_window) {
    prepare_window_for_draw(ui_, "mine");
    draw_mine_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_fuel_window) {
    prepare_window_for_draw(ui_, "fuel");
    draw_fuel_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_salvage_window) {
    prepare_window_for_draw(ui_, "salvage");
    draw_salvage_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_contracts_window) {
    prepare_window_for_draw(ui_, "contracts");
    draw_contracts_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_sustainment_window) {
    prepare_window_for_draw(ui_, "sustainment");
    draw_sustainment_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }

  if (ui_.show_repair_planner_window) {
    prepare_window_for_draw(ui_, "repair_planner");
    draw_repair_planner_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_maintenance_planner_window) {
    prepare_window_for_draw(ui_, "maintenance_planner");
    draw_maintenance_planner_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_fleet_manager_window) {
    prepare_window_for_draw(ui_, "fleet_manager");
    draw_fleet_manager_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_battle_forecast_window) {
    prepare_window_for_draw(ui_, "battle_forecast");
    draw_battle_forecast_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_troop_window) {
    prepare_window_for_draw(ui_, "troops");
    draw_troop_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_colonist_window) {
    prepare_window_for_draw(ui_, "population");
    draw_colonist_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_terraforming_window) {
    prepare_window_for_draw(ui_, "terraforming");
    draw_terraforming_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_advisor_window) {
    prepare_window_for_draw(ui_, "advisor");
    draw_advisor_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_colony_profiles_window) {
    prepare_window_for_draw(ui_, "colony_profiles");
    draw_colony_profiles_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_ship_profiles_window) {
    prepare_window_for_draw(ui_, "ship_profiles");
    draw_ship_profiles_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_automation_center_window) {
    prepare_window_for_draw(ui_, "automation_center");
    draw_automation_center_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_shipyard_targets_window) {
    prepare_window_for_draw(ui_, "shipyard_targets");
    draw_shipyard_targets_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_survey_network_window) {
    prepare_window_for_draw(ui_, "survey_network");
    draw_survey_network_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_time_warp_window) {
    prepare_window_for_draw(ui_, "time_warp");
    draw_time_warp_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_timeline_window) {
    prepare_window_for_draw(ui_, "timeline");
    draw_timeline_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_notifications_window) {
    prepare_window_for_draw(ui_, "notifications");
    draw_notifications_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_design_studio_window) {
    prepare_window_for_draw(ui_, "design_studio");
    draw_design_studio_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_balance_lab_window) {
    prepare_window_for_draw(ui_, "balance_lab");
    draw_balance_lab_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_procgen_atlas_window) {
    prepare_window_for_draw(ui_, "procgen_atlas");
    draw_procgen_atlas_window(sim_, ui_, selected_body_);
  }
  if (ui_.show_star_atlas_window) {
    prepare_window_for_draw(ui_, "star_atlas");
    draw_star_atlas_window(sim_, ui_);
  }
  if (ui_.show_intel_window) {
    prepare_window_for_draw(ui_, "intel");
    draw_intel_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_intel_notebook_window) {
    prepare_window_for_draw(ui_, "intel_notebook");
    draw_intel_notebook_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_diplomacy_window) {
    prepare_window_for_draw(ui_, "diplomacy");
    draw_diplomacy_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_victory_window) {
    prepare_window_for_draw(ui_, "victory");
    draw_victory_window(sim_, ui_);
  }

  if (ui_.show_save_tools_window) {
    prepare_window_for_draw(ui_, "save_tools");
    draw_save_tools_window(sim_, ui_, save_path_, load_path_);
  }
  if (ui_.show_time_machine_window) {
    prepare_window_for_draw(ui_, "time_machine");
    draw_time_machine_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_compare_window) {
    prepare_window_for_draw(ui_, "compare");
    draw_compare_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_navigator_window) {
    prepare_window_for_draw(ui_, "navigator");
    draw_navigator_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_omni_search_window) {
    prepare_window_for_draw(ui_, "omni_search");
    draw_omni_search_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_json_explorer_window) {
    prepare_window_for_draw(ui_, "json_explorer");
    draw_json_explorer_window(sim_, ui_);
  }
  if (ui_.show_content_validation_window) {
    prepare_window_for_draw(ui_, "content_validation");
    draw_content_validation_window(sim_, ui_);
  }
  if (ui_.show_state_doctor_window) {
    prepare_window_for_draw(ui_, "state_doctor");
    draw_state_doctor_window(sim_, ui_);
  }
  if (ui_.show_trace_viewer_window) {
    prepare_window_for_draw(ui_, "trace_viewer");
    draw_trace_viewer_window(sim_, ui_);
  }
  if (ui_.show_watchboard_window) {
    prepare_window_for_draw(ui_, "watchboard");
    draw_watchboard_window(sim_, ui_);
  }
  if (ui_.show_data_lenses_window) {
    prepare_window_for_draw(ui_, "data_lenses");
    draw_data_lenses_window(sim_, ui_);
  }
  if (ui_.show_dashboards_window) {
    prepare_window_for_draw(ui_, "dashboards");
    draw_dashboards_window(sim_, ui_);
  }
  if (ui_.show_pivot_tables_window) {
    prepare_window_for_draw(ui_, "pivot_tables");
    draw_pivot_tables_window(sim_, ui_);
  }
  if (ui_.show_entity_inspector_window) {
    prepare_window_for_draw(ui_, "entity_inspector");
    draw_entity_inspector_window(sim_, ui_);
  }
  if (ui_.show_reference_graph_window) {
    prepare_window_for_draw(ui_, "reference_graph");
    draw_reference_graph_window(sim_, ui_);
  }
  if (ui_.show_layout_profiles_window) {
    prepare_window_for_draw(ui_, "layout_profiles");
    draw_layout_profiles_window(ui_);
  }

  // Window Manager: centralized launcher, pop-out controls, and declutter tools.
  prepare_window_for_draw(ui_, "window_manager");
  draw_window_manager_window(ui_);

  if (ui_.show_context_forge_window) {
    prepare_window_for_draw(ui_, "context_forge");
    draw_context_forge_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }

  // Keep Context Forge panel synced (selection-following / pinned entity).
  update_context_forge(sim_, ui_, selected_ship_, selected_colony_, selected_body_);

  // UI Forge: user-defined procedural panels (custom dashboards).
  draw_ui_forge_panel_windows(sim_, ui_);
  if (ui_.show_ui_forge_window) {
    prepare_window_for_draw(ui_, "ui_forge");
    draw_ui_forge_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }

  // Help overlay/window.
  prepare_window_for_draw(ui_, "help");
  draw_help_window(ui_);

  // HUD chrome (status bar, command palette, event toasts).
  if (ui_.show_status_bar) {
    draw_status_bar(sim_, ui_, hud_, selected_ship_, selected_colony_, selected_body_, save_path_, load_path_);
  }
  draw_command_palette(sim_, ui_, hud_, selected_ship_, selected_colony_, selected_body_, save_path_, load_path_);

  // Load/new-game can also be triggered via the status bar or command palette.
  // Ensure we react in the same frame (avoids dereferencing stale selections).
  sync_on_state_generation_change();

  // Auto-open the Victory window once when the game ends.
  if (sim_.state().victory_state.game_over && !victory_window_autoopened_) {
    ui_.show_victory_window = true;
    victory_window_autoopened_ = true;
  }

  // --- Rolling autosave (save-game snapshots) ---
  {
    nebula4x::AutosaveConfig cfg;
    cfg.enabled = ui_.autosave_game_enabled;
    cfg.interval_hours = ui_.autosave_game_interval_hours;
    cfg.keep_files = ui_.autosave_game_keep_files;
    cfg.dir = ui_.autosave_game_dir;
    cfg.prefix = "autosave_";
    cfg.extension = ".json";

    nebula4x::AutosaveResult r;
    if (ui_.request_autosave_game_now) {
      ui_.request_autosave_game_now = false;
      r = autosave_mgr_.force_autosave(sim_.state(), cfg, [&]() { return serialize_game_to_json(sim_.state()); });
    } else {
      r = autosave_mgr_.maybe_autosave(sim_.state(), cfg, [&]() { return serialize_game_to_json(sim_.state()); });
    }

    if (!r.error.empty()) {
      ui_.last_autosave_game_error = r.error;
    }
    if (r.saved) {
      ui_.last_autosave_game_path = r.path;
      ui_.last_autosave_game_error.clear();

      if (r.pruned > 0) {
        nebula4x::log::info("Autosaved: " + r.path + " (pruned " + std::to_string(r.pruned) + ")");
      } else {
        nebula4x::log::info("Autosaved: " + r.path);
      }
    }
  }
  update_event_toasts(sim_, ui_, hud_);
  if (!(ui_.tour_active && ui_.tour_pause_toasts)) {
    draw_event_toasts(sim_, ui_, hud_, selected_ship_, selected_colony_, selected_body_);
  }

  // Guided tours overlay renders last so it can spotlight existing panels.
  draw_guided_tour_overlay(ui_);

  // Selection Navigator: record selection changes into history.
  const NavTarget nav_after = current_nav_target(sim_, selected_ship_, selected_colony_, selected_body_);
  if (ui_.nav_history.empty()) {
    if (nav_after.id != kInvalidId) nav_history_push(ui_, nav_after);
  } else if (nav_after != nav_before) {
    nav_history_push(ui_, nav_after);
  }

  // Narrate selection changes (best-effort).
  if (ui_.screen_reader_enabled && ui_.screen_reader_speak_selection) {
    const auto& st = sim_.state();

    auto speak_selected = [&](const std::string& prefix, const std::string& name, Id id) {
      if (!name.empty()) {
        ScreenReader::instance().speak(prefix + name, false);
      } else {
        ScreenReader::instance().speak(prefix + "#" + std::to_string((unsigned long long)id), false);
      }
    };

    if (selected_ship_ != prev_selected_ship && selected_ship_ != kInvalidId) {
      if (const auto* sh = nebula4x::find_ptr(st.ships, selected_ship_)) {
        speak_selected("Ship selected: ", sh->name, selected_ship_);
      }
    } else if (selected_colony_ != prev_selected_colony && selected_colony_ != kInvalidId) {
      if (const auto* c = nebula4x::find_ptr(st.colonies, selected_colony_)) {
        speak_selected("Colony selected: ", c->name, selected_colony_);
      }
    } else if (selected_body_ != prev_selected_body && selected_body_ != kInvalidId) {
      if (const auto* b = nebula4x::find_ptr(st.bodies, selected_body_)) {
        speak_selected("Body selected: ", b->name, selected_body_);
      }
    }
  }

  // Expose procedural render engine telemetry to the UI (runtime only).
  {
    const auto& st = proc_render_engine_.stats();
    ui_.map_proc_render_stats_cache_tiles = st.cache_tiles;
    ui_.map_proc_render_stats_generated_this_frame = st.generated_this_frame;
    ui_.map_proc_render_stats_gen_ms_this_frame = static_cast<float>(st.gen_ms_this_frame);
    ui_.map_proc_render_stats_upload_ms_this_frame = static_cast<float>(st.upload_ms_this_frame);
  }

  // Expose procedural body sprite telemetry to the UI (runtime only).
  {
    const auto& st = proc_body_sprite_engine_.stats();
    ui_.system_map_body_sprite_stats_cache_sprites = st.cache_sprites;
    ui_.system_map_body_sprite_stats_generated_this_frame = st.generated_this_frame;
    ui_.system_map_body_sprite_stats_gen_ms_this_frame = static_cast<float>(st.gen_ms_this_frame);
    ui_.system_map_body_sprite_stats_upload_ms_this_frame = static_cast<float>(st.upload_ms_this_frame);
  }

  // Expose procedural contact icon telemetry to the UI (runtime only).
  {
    const auto& st = proc_icon_sprite_engine_.stats();
    ui_.system_map_contact_icon_stats_cache_sprites = st.cache_sprites;
    ui_.system_map_contact_icon_stats_generated_this_frame = st.generated_this_frame;
    ui_.system_map_contact_icon_stats_gen_ms_this_frame = static_cast<float>(st.gen_ms_this_frame);
    ui_.system_map_contact_icon_stats_upload_ms_this_frame = static_cast<float>(st.upload_ms_this_frame);
  }

  // Expose procedural jump-point phenomena telemetry to the UI (runtime only).
  {
    const auto& st = proc_jump_phenomena_sprite_engine_.stats();
    ui_.system_map_jump_phenomena_stats_cache_sprites = st.cache_sprites;
    ui_.system_map_jump_phenomena_stats_generated_this_frame = st.generated_this_frame;
    ui_.system_map_jump_phenomena_stats_gen_ms_this_frame = static_cast<float>(st.gen_ms_this_frame);
    ui_.system_map_jump_phenomena_stats_upload_ms_this_frame = static_cast<float>(st.upload_ms_this_frame);
  }

  {
    const auto& st = proc_anomaly_phenomena_sprite_engine_.stats();
    ui_.system_map_anomaly_phenomena_stats_cache_sprites = st.cache_sprites;
    ui_.system_map_anomaly_phenomena_stats_generated_this_frame = st.generated_this_frame;
    ui_.system_map_anomaly_phenomena_stats_gen_ms_this_frame = static_cast<float>(st.gen_ms_this_frame);
    ui_.system_map_anomaly_phenomena_stats_upload_ms_this_frame = static_cast<float>(st.upload_ms_this_frame);
  }



  // Expose galaxy territory overlay telemetry to the UI (runtime only).
  {
    const auto st = proc_territory_field_engine_.stats();
    ui_.galaxy_map_territory_stats_cache_tiles = st.cache_tiles;
    ui_.galaxy_map_territory_stats_tiles_used_this_frame = st.tiles_used_this_frame;
    ui_.galaxy_map_territory_stats_tiles_generated_this_frame = st.tiles_generated_this_frame;
    ui_.galaxy_map_territory_stats_cells_drawn = st.cells_drawn;
    ui_.galaxy_map_territory_stats_gen_ms_this_frame = static_cast<float>(st.gen_ms_this_frame);
  }
  // Update popup/launch tracking after all UI interactions for the frame.
  window_management_end_frame(ui_);
}

void App::draw_dockspace() {
  ImGuiIO& io = ImGui::GetIO();
  if ((io.ConfigFlags & ImGuiConfigFlags_DockingEnable) == 0) return;

  ImGuiViewport* viewport = ImGui::GetMainViewport();

  // Respect the menu bar (viewport->WorkPos/WorkSize) and reserve space for the status bar.
  ImVec2 pos = viewport->WorkPos;
  ImVec2 size = viewport->WorkSize;

  if (ui_.show_status_bar) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float status_h = ImGui::GetFrameHeight() + style.WindowPadding.y * 2.0f;
    size.y = std::max(0.0f, size.y - status_h);
  }

  ImGui::SetNextWindowPos(pos);
  ImGui::SetNextWindowSize(size);
  ImGui::SetNextWindowViewport(viewport->ID);

  ImGuiDockNodeFlags dock_flags = ImGuiDockNodeFlags_PassthruCentralNode;

  ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                                  ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                                  ImGuiWindowFlags_NoBackground;

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

  if (ImGui::Begin("##nebula4x_dockspace", nullptr, window_flags)) {
    const unsigned int dockspace_id = ImGui::GetID("Nebula4XDockSpace");
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dock_flags);

    // Procedural docking layout synthesizer: rebuild a workspace from a seed.
    if (ui_.request_generate_procedural_layout) {
      ui_.request_generate_procedural_layout = false;
    
      // Clear prior docking state so the generated layout is deterministic and
      // doesn't fight persisted window placements.
      ImGui::LoadIniSettingsFromMemory("");
    
      ProceduralLayoutParams p;
      p.seed = static_cast<std::uint32_t>(ui_.ui_procedural_layout_seed);
      p.mode = ui_.ui_procedural_layout_mode;
      p.variation = ui_.ui_procedural_layout_variation;
      p.include_tools = ui_.ui_procedural_layout_include_tools;
      p.include_forge_panels = ui_.ui_procedural_layout_include_forge_panels;
      p.max_forge_panels = ui_.ui_procedural_layout_max_forge_panels;
      p.auto_open_windows = ui_.ui_procedural_layout_auto_open_windows;
      p.auto_save_profile = ui_.ui_procedural_layout_autosave_profile;
    
      if (p.auto_open_windows) {
        apply_procedural_layout_visibility(ui_, p);
      }
    
      std::vector<std::string> extra;
      if (p.include_forge_panels) {
        extra = gather_ui_forge_panel_window_titles(ui_, p.max_forge_panels);
      }
    
      build_procedural_dock_layout(dockspace_id, size, p, extra);
    
      if (p.auto_save_profile) {
        const char* ini = io.IniFilename;
        if (ini && ini[0] != '\0') {
          ImGui::SaveIniSettingsToDisk(ini);
        }
      }
    
      dock_layout_initialized_ = true;
      dock_layout_checked_ini_ = true;
      dock_layout_has_existing_ini_ = false;
    }

    // Only auto-build a default layout when there isn't already a persisted layout.
    if (!dock_layout_checked_ini_) {
      dock_layout_checked_ini_ = true;
      dock_layout_has_existing_ini_ = false;

      const char* ini = io.IniFilename;
      if (ini && ini[0] != '\0') {
        try {
          dock_layout_has_existing_ini_ = std::filesystem::exists(ini);
        } catch (...) {
          dock_layout_has_existing_ini_ = false;
        }
      }
    }

    if (!dock_layout_initialized_) {
      if (!dock_layout_has_existing_ini_) {
        build_default_dock_layout(dockspace_id);
      }
      dock_layout_initialized_ = true;
    }
  }

  ImGui::End();
  ImGui::PopStyleVar(3);
}

void App::build_default_dock_layout(unsigned int dockspace_id) {
  if (dockspace_id == 0) return;
  if ((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_DockingEnable) == 0) return;

  ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImVec2 size = viewport->WorkSize;
  if (ui_.show_status_bar) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float status_h = ImGui::GetFrameHeight() + style.WindowPadding.y * 2.0f;
    size.y = std::max(0.0f, size.y - status_h);
  }

  ImGui::DockBuilderRemoveNode(dockspace_id);               // clear previous layout
  ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(dockspace_id, size);

  ImGuiID dock_main = dockspace_id;
  ImGuiID dock_left = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Left, 0.22f, nullptr, &dock_main);
  ImGuiID dock_right = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Right, 0.26f, nullptr, &dock_main);
  ImGuiID dock_bottom = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Down, 0.30f, nullptr, &dock_main);

  ImGui::DockBuilderDockWindow("Controls", dock_left);
  ImGui::DockBuilderDockWindow("Details", dock_right);
  ImGui::DockBuilderDockWindow("Map", dock_main);
  ImGui::DockBuilderDockWindow("Directory", dock_bottom);
  ImGui::DockBuilderDockWindow("Production", dock_bottom);
  ImGui::DockBuilderDockWindow("Economy", dock_bottom);
  ImGui::DockBuilderDockWindow("Timeline", dock_bottom);
  ImGui::DockBuilderDockWindow("Design Studio", dock_bottom);
  ImGui::DockBuilderDockWindow("Intel", dock_bottom);
  ImGui::DockBuilderDockWindow("Diplomacy Graph", dock_bottom);

  ImGui::DockBuilderFinish(dockspace_id);
}

namespace {

template <typename T>
T clamp01(T v) {
  return std::max<T>(0, std::min<T>(1, v));
}

nebula4x::json::Value color_to_json(const float c[4]) {
  nebula4x::json::Array a;
  a.emplace_back(static_cast<double>(c[0]));
  a.emplace_back(static_cast<double>(c[1]));
  a.emplace_back(static_cast<double>(c[2]));
  a.emplace_back(static_cast<double>(c[3]));
  return nebula4x::json::array(std::move(a));
}

void json_to_color(const nebula4x::json::Value& v, float out[4], const float def[4]) {
  for (int i = 0; i < 4; ++i) out[i] = def[i];
  const auto* arr = v.as_array();
  if (!arr) return;
  if (arr->size() < 3) return;

  out[0] = static_cast<float>(clamp01((*arr)[0].number_value(def[0])));
  out[1] = static_cast<float>(clamp01((*arr)[1].number_value(def[1])));
  out[2] = static_cast<float>(clamp01((*arr)[2].number_value(def[2])));
  out[3] = static_cast<float>(clamp01((arr->size() >= 4) ? (*arr)[3].number_value(def[3]) : def[3]));
}

} // namespace

bool App::load_ui_prefs(const char* path, std::string* error) {
  try {
    if (!path || path[0] == '\0') return true;
    if (!std::filesystem::exists(path)) {
      if (error) *error = std::string("File not found: ") + path;
      return false;
    }

    const std::string text = nebula4x::read_text_file(path);
    const auto root = nebula4x::json::parse(text);
    const auto* obj = root.as_object();
    if (!obj) {
      if (error) *error = "UI prefs JSON root is not an object.";
      return false;
    }

    // Theme.
    {
      const float def_clear[4]{0.0f, 0.0f, 0.0f, 1.0f};
      const float def_sys[4]{15.0f / 255.0f, 18.0f / 255.0f, 22.0f / 255.0f, 1.0f};
      const float def_gal[4]{12.0f / 255.0f, 14.0f / 255.0f, 18.0f / 255.0f, 1.0f};
      const float def_win[4]{0.10f, 0.105f, 0.11f, 0.94f};

      if (auto it = obj->find("clear_color"); it != obj->end()) {
        json_to_color(it->second, ui_.clear_color, def_clear);
      }
      if (auto it = obj->find("system_map_bg"); it != obj->end()) {
        json_to_color(it->second, ui_.system_map_bg, def_sys);
      }
      if (auto it = obj->find("galaxy_map_bg"); it != obj->end()) {
        json_to_color(it->second, ui_.galaxy_map_bg, def_gal);
      }
      if (auto it = obj->find("override_window_bg"); it != obj->end()) {
        ui_.override_window_bg = it->second.bool_value(ui_.override_window_bg);
      }
      if (auto it = obj->find("window_bg"); it != obj->end()) {
        json_to_color(it->second, ui_.window_bg, def_win);
      }
      if (auto it = obj->find("autosave_ui_prefs"); it != obj->end()) {
        ui_.autosave_ui_prefs = it->second.bool_value(ui_.autosave_ui_prefs);
      }

      // Rolling game autosaves.
      if (auto it = obj->find("autosave_game_enabled"); it != obj->end()) {
        ui_.autosave_game_enabled = it->second.bool_value(ui_.autosave_game_enabled);
      }
      if (auto it = obj->find("autosave_game_interval_hours"); it != obj->end()) {
        ui_.autosave_game_interval_hours = static_cast<int>(it->second.number_value(ui_.autosave_game_interval_hours));
        ui_.autosave_game_interval_hours = std::clamp(ui_.autosave_game_interval_hours, 1, 24 * 365);
      }
      if (auto it = obj->find("autosave_game_keep_files"); it != obj->end()) {
        ui_.autosave_game_keep_files = static_cast<int>(it->second.number_value(ui_.autosave_game_keep_files));
        ui_.autosave_game_keep_files = std::clamp(ui_.autosave_game_keep_files, 1, 500);
      }
      if (auto it = obj->find("autosave_game_dir"); it != obj->end()) {
        const std::string dir = it->second.string_value(std::string(ui_.autosave_game_dir));
        std::snprintf(ui_.autosave_game_dir, sizeof(ui_.autosave_game_dir), "%s", dir.c_str());
      }

      // Navigator prefs.
      if (auto it = obj->find("nav_open_windows_on_jump"); it != obj->end()) {
        ui_.nav_open_windows_on_jump = it->second.bool_value(ui_.nav_open_windows_on_jump);
      }
      if (auto it = obj->find("nav_history_max"); it != obj->end()) {
        ui_.nav_history_max = static_cast<int>(it->second.number_value(ui_.nav_history_max));
        ui_.nav_history_max = std::clamp(ui_.nav_history_max, 16, 1024);
      }

      // New Game dialog defaults.
      if (auto it = obj->find("new_game_scenario"); it != obj->end()) {
        ui_.new_game_scenario = static_cast<int>(it->second.number_value(ui_.new_game_scenario));
        ui_.new_game_scenario = std::clamp(ui_.new_game_scenario, 0, 1);
      }
      if (auto it = obj->find("new_game_random_seed"); it != obj->end()) {
        const std::uint64_t v = static_cast<std::uint64_t>(it->second.number_value(ui_.new_game_random_seed));
        ui_.new_game_random_seed = static_cast<std::uint32_t>(v & 0xffffffffu);
      }
      if (auto it = obj->find("new_game_random_num_systems"); it != obj->end()) {
        ui_.new_game_random_num_systems = static_cast<int>(it->second.number_value(ui_.new_game_random_num_systems));
        ui_.new_game_random_num_systems = std::clamp(ui_.new_game_random_num_systems, 1, 64);
      }
      if (auto it = obj->find("new_game_random_galaxy_shape"); it != obj->end()) {
        ui_.new_game_random_galaxy_shape = static_cast<int>(it->second.number_value(ui_.new_game_random_galaxy_shape));
        ui_.new_game_random_galaxy_shape = std::clamp(ui_.new_game_random_galaxy_shape, 0, 5);
      }
      if (auto it = obj->find("new_game_random_placement_style"); it != obj->end()) {
        ui_.new_game_random_placement_style = static_cast<int>(it->second.number_value(ui_.new_game_random_placement_style));
        ui_.new_game_random_placement_style = std::clamp(ui_.new_game_random_placement_style, 0, 1);
      }
      if (auto it = obj->find("new_game_random_placement_quality"); it != obj->end()) {
        ui_.new_game_random_placement_quality = static_cast<int>(it->second.number_value(ui_.new_game_random_placement_quality));
        ui_.new_game_random_placement_quality = std::clamp(ui_.new_game_random_placement_quality, 4, 96);
      }
      if (auto it = obj->find("new_game_random_jump_network_style"); it != obj->end()) {
        ui_.new_game_random_jump_network_style = static_cast<int>(it->second.number_value(ui_.new_game_random_jump_network_style));
        ui_.new_game_random_jump_network_style = std::clamp(ui_.new_game_random_jump_network_style, 0, 6);
      }
      if (auto it = obj->find("new_game_random_jump_density"); it != obj->end()) {
        ui_.new_game_random_jump_density = static_cast<float>(it->second.number_value(ui_.new_game_random_jump_density));
        ui_.new_game_random_jump_density = std::clamp(ui_.new_game_random_jump_density, 0.0f, 2.0f);
      }
      if (auto it = obj->find("new_game_random_enable_regions"); it != obj->end()) {
        ui_.new_game_random_enable_regions = it->second.bool_value(ui_.new_game_random_enable_regions);
      }
      if (auto it = obj->find("new_game_random_num_regions"); it != obj->end()) {
        ui_.new_game_random_num_regions = static_cast<int>(it->second.number_value(ui_.new_game_random_num_regions));
        ui_.new_game_random_num_regions = std::clamp(ui_.new_game_random_num_regions, -1, 12);
      }
      if (auto it = obj->find("new_game_random_ai_empires"); it != obj->end()) {
        ui_.new_game_random_ai_empires = static_cast<int>(it->second.number_value(ui_.new_game_random_ai_empires));
        ui_.new_game_random_ai_empires = std::clamp(ui_.new_game_random_ai_empires, -1, 12);
      }
      if (auto it = obj->find("new_game_random_enable_pirates"); it != obj->end()) {
        ui_.new_game_random_enable_pirates = it->second.bool_value(ui_.new_game_random_enable_pirates);
      }
      if (auto it = obj->find("new_game_random_pirate_strength"); it != obj->end()) {
        ui_.new_game_random_pirate_strength = static_cast<float>(it->second.number_value(ui_.new_game_random_pirate_strength));
        ui_.new_game_random_pirate_strength = std::clamp(ui_.new_game_random_pirate_strength, 0.0f, 5.0f);
      }
      if (auto it = obj->find("new_game_random_enable_independents"); it != obj->end()) {
        ui_.new_game_random_enable_independents = it->second.bool_value(ui_.new_game_random_enable_independents);
      }
      if (auto it = obj->find("new_game_random_num_independent_outposts"); it != obj->end()) {
        ui_.new_game_random_num_independent_outposts = static_cast<int>(it->second.number_value(ui_.new_game_random_num_independent_outposts));
        ui_.new_game_random_num_independent_outposts = std::clamp(ui_.new_game_random_num_independent_outposts, -1, 64);
      }
      if (auto it = obj->find("new_game_random_ensure_clear_home"); it != obj->end()) {
        ui_.new_game_random_ensure_clear_home = it->second.bool_value(ui_.new_game_random_ensure_clear_home);
      }

      // Random galaxy preview options.
      if (auto it = obj->find("new_game_preview_show_jumps"); it != obj->end()) {
        ui_.new_game_preview_show_jumps = it->second.bool_value(ui_.new_game_preview_show_jumps);
      }
      if (auto it = obj->find("new_game_preview_show_labels"); it != obj->end()) {
        ui_.new_game_preview_show_labels = it->second.bool_value(ui_.new_game_preview_show_labels);
      }
      if (auto it = obj->find("new_game_preview_show_regions"); it != obj->end()) {
        ui_.new_game_preview_show_regions = it->second.bool_value(ui_.new_game_preview_show_regions);
      }
      if (auto it = obj->find("new_game_preview_show_nebula"); it != obj->end()) {
        ui_.new_game_preview_show_nebula = it->second.bool_value(ui_.new_game_preview_show_nebula);
      }
      if (auto it = obj->find("new_game_preview_color_by_component"); it != obj->end()) {
        ui_.new_game_preview_color_by_component = it->second.bool_value(ui_.new_game_preview_color_by_component);
      }
      if (auto it = obj->find("new_game_preview_show_chokepoints"); it != obj->end()) {
        ui_.new_game_preview_show_chokepoints = it->second.bool_value(ui_.new_game_preview_show_chokepoints);
      }

      // Seed explorer defaults.
      if (auto it = obj->find("new_game_seed_search_objective"); it != obj->end()) {
        ui_.new_game_seed_search_objective = static_cast<int>(it->second.number_value(ui_.new_game_seed_search_objective));
        ui_.new_game_seed_search_objective = std::clamp(ui_.new_game_seed_search_objective, 0, 3);
      }
      if (auto it = obj->find("new_game_seed_search_tries"); it != obj->end()) {
        ui_.new_game_seed_search_tries = static_cast<int>(it->second.number_value(ui_.new_game_seed_search_tries));
        ui_.new_game_seed_search_tries = std::clamp(ui_.new_game_seed_search_tries, 1, 2000);
      }
      if (auto it = obj->find("new_game_seed_search_steps_per_frame"); it != obj->end()) {
        ui_.new_game_seed_search_steps_per_frame = static_cast<int>(it->second.number_value(ui_.new_game_seed_search_steps_per_frame));
        ui_.new_game_seed_search_steps_per_frame = std::clamp(ui_.new_game_seed_search_steps_per_frame, 1, 200);
      }

      // UI scale (accessibility). This is a UI preference (not a save-game setting).
      if (auto it = obj->find("ui_scale"); it != obj->end()) {
        ui_.ui_scale = static_cast<float>(it->second.number_value(ui_.ui_scale));
        ui_.ui_scale = std::clamp(ui_.ui_scale, 0.65f, 2.5f);
      }

      // Hotkeys (keyboard shortcuts). These are UI-only and are stored in ui_prefs.json.
      if (auto it = obj->find("hotkeys_enabled"); it != obj->end()) {
        ui_.hotkeys_enabled = it->second.bool_value(ui_.hotkeys_enabled);
      }
      if (auto it = obj->find("hotkeys"); it != obj->end()) {
        if (const auto* hk_obj = it->second.as_object()) {
          ui_.hotkey_overrides.clear();
          for (const auto& d : hotkey_defs()) {
            const auto hit = hk_obj->find(d.id);
            if (hit == hk_obj->end()) continue;
            const std::string s = hit->second.string_value("");
            HotkeyChord c;
            std::string perr;
            if (!parse_hotkey(s, &c, &perr)) continue;
            (void)hotkey_set(ui_, d.id, c);
          }
        }
      }

      // Screen reader / narration (accessibility).
      if (auto it = obj->find("screen_reader_enabled"); it != obj->end()) {
        ui_.screen_reader_enabled = it->second.bool_value(ui_.screen_reader_enabled);
      }
      if (auto it = obj->find("screen_reader_speak_focus"); it != obj->end()) {
        ui_.screen_reader_speak_focus = it->second.bool_value(ui_.screen_reader_speak_focus);
      }
      if (auto it = obj->find("screen_reader_speak_hover"); it != obj->end()) {
        ui_.screen_reader_speak_hover = it->second.bool_value(ui_.screen_reader_speak_hover);
      }
      if (auto it = obj->find("screen_reader_speak_windows"); it != obj->end()) {
        ui_.screen_reader_speak_windows = it->second.bool_value(ui_.screen_reader_speak_windows);
      }
      if (auto it = obj->find("screen_reader_speak_toasts"); it != obj->end()) {
        ui_.screen_reader_speak_toasts = it->second.bool_value(ui_.screen_reader_speak_toasts);
      }
      if (auto it = obj->find("screen_reader_speak_selection"); it != obj->end()) {
        ui_.screen_reader_speak_selection = it->second.bool_value(ui_.screen_reader_speak_selection);
      }
      if (auto it = obj->find("screen_reader_rate"); it != obj->end()) {
        ui_.screen_reader_rate = static_cast<float>(it->second.number_value(ui_.screen_reader_rate));
        ui_.screen_reader_rate = std::clamp(ui_.screen_reader_rate, 0.5f, 2.0f);
      }
      if (auto it = obj->find("screen_reader_volume"); it != obj->end()) {
        ui_.screen_reader_volume = static_cast<float>(it->second.number_value(ui_.screen_reader_volume));
        ui_.screen_reader_volume = std::clamp(ui_.screen_reader_volume, 0.0f, 1.0f);
      }
      if (auto it = obj->find("screen_reader_hover_delay_sec"); it != obj->end()) {
        ui_.screen_reader_hover_delay_sec = static_cast<float>(it->second.number_value(ui_.screen_reader_hover_delay_sec));
        ui_.screen_reader_hover_delay_sec = std::clamp(ui_.screen_reader_hover_delay_sec, 0.0f, 5.0f);
      }

      // UI style preferences (visual presets + density).
      if (auto it = obj->find("ui_scale_style"); it != obj->end()) {
        ui_.ui_scale_style = it->second.bool_value(ui_.ui_scale_style);
      }
      if (auto it = obj->find("ui_style_preset"); it != obj->end()) {
        ui_.ui_style_preset = static_cast<int>(it->second.number_value(ui_.ui_style_preset));
        ui_.ui_style_preset = std::clamp(ui_.ui_style_preset, 0, 5);
      }
      if (auto it = obj->find("ui_density"); it != obj->end()) {
        ui_.ui_density = static_cast<int>(it->second.number_value(ui_.ui_density));
        ui_.ui_density = std::clamp(ui_.ui_density, 0, 2);
      }

      // Procedural theme preferences (used by ui_style_preset = 5).
      if (auto it = obj->find("ui_procedural_theme_seed"); it != obj->end()) {
        ui_.ui_procedural_theme_seed = static_cast<int>(it->second.number_value(ui_.ui_procedural_theme_seed));
      }
      if (auto it = obj->find("ui_procedural_theme_use_seed_hue"); it != obj->end()) {
        ui_.ui_procedural_theme_use_seed_hue = it->second.bool_value(ui_.ui_procedural_theme_use_seed_hue);
      }
      if (auto it = obj->find("ui_procedural_theme_hue_deg"); it != obj->end()) {
        ui_.ui_procedural_theme_hue_deg = static_cast<float>(it->second.number_value(ui_.ui_procedural_theme_hue_deg));
        ui_.ui_procedural_theme_hue_deg = std::clamp(ui_.ui_procedural_theme_hue_deg, 0.0f, 360.0f);
      }
      if (auto it = obj->find("ui_procedural_theme_variant"); it != obj->end()) {
        ui_.ui_procedural_theme_variant = static_cast<int>(it->second.number_value(ui_.ui_procedural_theme_variant));
        ui_.ui_procedural_theme_variant = std::clamp(ui_.ui_procedural_theme_variant, 0, 3);
      }
      if (auto it = obj->find("ui_procedural_theme_saturation"); it != obj->end()) {
        ui_.ui_procedural_theme_saturation = static_cast<float>(it->second.number_value(ui_.ui_procedural_theme_saturation));
        ui_.ui_procedural_theme_saturation = std::clamp(ui_.ui_procedural_theme_saturation, 0.0f, 1.0f);
      }
      if (auto it = obj->find("ui_procedural_theme_value"); it != obj->end()) {
        ui_.ui_procedural_theme_value = static_cast<float>(it->second.number_value(ui_.ui_procedural_theme_value));
        ui_.ui_procedural_theme_value = std::clamp(ui_.ui_procedural_theme_value, 0.0f, 1.0f);
      }
      if (auto it = obj->find("ui_procedural_theme_bg_value"); it != obj->end()) {
        ui_.ui_procedural_theme_bg_value = static_cast<float>(it->second.number_value(ui_.ui_procedural_theme_bg_value));
        ui_.ui_procedural_theme_bg_value = std::clamp(ui_.ui_procedural_theme_bg_value, 0.0f, 1.0f);
      }
      if (auto it = obj->find("ui_procedural_theme_accent_strength"); it != obj->end()) {
        ui_.ui_procedural_theme_accent_strength = static_cast<float>(it->second.number_value(ui_.ui_procedural_theme_accent_strength));
        ui_.ui_procedural_theme_accent_strength = std::clamp(ui_.ui_procedural_theme_accent_strength, 0.0f, 1.0f);
      }
      if (auto it = obj->find("ui_procedural_theme_animate_hue"); it != obj->end()) {
        ui_.ui_procedural_theme_animate_hue = it->second.bool_value(ui_.ui_procedural_theme_animate_hue);
      }
      if (auto it = obj->find("ui_procedural_theme_animate_speed_deg_per_sec"); it != obj->end()) {
        ui_.ui_procedural_theme_animate_speed_deg_per_sec = static_cast<float>(it->second.number_value(ui_.ui_procedural_theme_animate_speed_deg_per_sec));
        ui_.ui_procedural_theme_animate_speed_deg_per_sec = std::clamp(ui_.ui_procedural_theme_animate_speed_deg_per_sec, 0.0f, 180.0f);
      }
      if (auto it = obj->find("ui_procedural_theme_sync_backgrounds"); it != obj->end()) {
        ui_.ui_procedural_theme_sync_backgrounds = it->second.bool_value(ui_.ui_procedural_theme_sync_backgrounds);
      }

      // Toast defaults.
      if (auto it = obj->find("show_event_toasts"); it != obj->end()) {
        ui_.show_event_toasts = it->second.bool_value(ui_.show_event_toasts);
      }
      if (auto it = obj->find("event_toast_duration_sec"); it != obj->end()) {
        ui_.event_toast_duration_sec = static_cast<float>(it->second.number_value(ui_.event_toast_duration_sec));
        ui_.event_toast_duration_sec = std::clamp(ui_.event_toast_duration_sec, 0.5f, 60.0f);
      }

      // Notification Center defaults.
      if (auto it = obj->find("notifications_capture_sim_events"); it != obj->end()) {
        ui_.notifications_capture_sim_events = it->second.bool_value(ui_.notifications_capture_sim_events);
      }
      if (auto it = obj->find("notifications_capture_info_events"); it != obj->end()) {
        ui_.notifications_capture_info_events = it->second.bool_value(ui_.notifications_capture_info_events);
      }
      if (auto it = obj->find("notifications_capture_watchboard_alerts"); it != obj->end()) {
        ui_.notifications_capture_watchboard_alerts = it->second.bool_value(ui_.notifications_capture_watchboard_alerts);
      }
      if (auto it = obj->find("notifications_collapse_duplicates"); it != obj->end()) {
        ui_.notifications_collapse_duplicates = it->second.bool_value(ui_.notifications_collapse_duplicates);
      }
      if (auto it = obj->find("notifications_auto_open_on_error"); it != obj->end()) {
        ui_.notifications_auto_open_on_error = it->second.bool_value(ui_.notifications_auto_open_on_error);
      }
      if (auto it = obj->find("notifications_max_entries"); it != obj->end()) {
        ui_.notifications_max_entries = static_cast<int>(it->second.number_value(ui_.notifications_max_entries));
        ui_.notifications_max_entries = std::clamp(ui_.notifications_max_entries, 50, 5000);
      }
      if (auto it = obj->find("notifications_keep_days"); it != obj->end()) {
        ui_.notifications_keep_days = static_cast<int>(it->second.number_value(ui_.notifications_keep_days));
        ui_.notifications_keep_days = std::clamp(ui_.notifications_keep_days, 0, 100000);
      }

      // Timeline view defaults.
      if (auto it = obj->find("timeline_show_minimap"); it != obj->end()) {
        ui_.timeline_show_minimap = it->second.bool_value(ui_.timeline_show_minimap);
      }
      if (auto it = obj->find("timeline_show_grid"); it != obj->end()) {
        ui_.timeline_show_grid = it->second.bool_value(ui_.timeline_show_grid);
      }
      if (auto it = obj->find("timeline_show_labels"); it != obj->end()) {
        ui_.timeline_show_labels = it->second.bool_value(ui_.timeline_show_labels);
      }
      if (auto it = obj->find("timeline_compact_rows"); it != obj->end()) {
        ui_.timeline_compact_rows = it->second.bool_value(ui_.timeline_compact_rows);
      }
      if (auto it = obj->find("timeline_lane_height"); it != obj->end()) {
        ui_.timeline_lane_height = static_cast<float>(it->second.number_value(ui_.timeline_lane_height));
        ui_.timeline_lane_height = std::clamp(ui_.timeline_lane_height, 18.0f, 80.0f);
      }
      if (auto it = obj->find("timeline_marker_size"); it != obj->end()) {
        ui_.timeline_marker_size = static_cast<float>(it->second.number_value(ui_.timeline_marker_size));
        ui_.timeline_marker_size = std::clamp(ui_.timeline_marker_size, 2.0f, 12.0f);
      }
      if (auto it = obj->find("timeline_follow_now"); it != obj->end()) {
        ui_.timeline_follow_now = it->second.bool_value(ui_.timeline_follow_now);
      }

      // Design Studio view defaults.
      if (auto it = obj->find("design_studio_show_grid"); it != obj->end()) {
        ui_.design_studio_show_grid = it->second.bool_value(ui_.design_studio_show_grid);
      }
      if (auto it = obj->find("design_studio_show_labels"); it != obj->end()) {
        ui_.design_studio_show_labels = it->second.bool_value(ui_.design_studio_show_labels);
      }
      if (auto it = obj->find("design_studio_show_compare"); it != obj->end()) {
        ui_.design_studio_show_compare = it->second.bool_value(ui_.design_studio_show_compare);
      }
      if (auto it = obj->find("design_studio_show_power_overlay"); it != obj->end()) {
        ui_.design_studio_show_power_overlay = it->second.bool_value(ui_.design_studio_show_power_overlay);
      }
      if (auto it = obj->find("design_studio_show_heat_overlay"); it != obj->end()) {
        ui_.design_studio_show_heat_overlay = it->second.bool_value(ui_.design_studio_show_heat_overlay);
      }

      // Intel view defaults.
      if (auto it = obj->find("intel_radar_scanline"); it != obj->end()) {
        ui_.intel_radar_scanline = it->second.bool_value(ui_.intel_radar_scanline);
      }
      if (auto it = obj->find("intel_radar_grid"); it != obj->end()) {
        ui_.intel_radar_grid = it->second.bool_value(ui_.intel_radar_grid);
      }
      if (auto it = obj->find("intel_radar_show_sensors"); it != obj->end()) {
        ui_.intel_radar_show_sensors = it->second.bool_value(ui_.intel_radar_show_sensors);
      }
      if (auto it = obj->find("intel_radar_sensor_heat"); it != obj->end()) {
        ui_.intel_radar_sensor_heat = it->second.bool_value(ui_.intel_radar_sensor_heat);
      }
      if (auto it = obj->find("intel_radar_show_bodies"); it != obj->end()) {
        ui_.intel_radar_show_bodies = it->second.bool_value(ui_.intel_radar_show_bodies);
      }
      if (auto it = obj->find("intel_radar_show_jump_points"); it != obj->end()) {
        ui_.intel_radar_show_jump_points = it->second.bool_value(ui_.intel_radar_show_jump_points);
      }
      if (auto it = obj->find("intel_radar_show_friendlies"); it != obj->end()) {
        ui_.intel_radar_show_friendlies = it->second.bool_value(ui_.intel_radar_show_friendlies);
      }
      if (auto it = obj->find("intel_radar_show_hostiles"); it != obj->end()) {
        ui_.intel_radar_show_hostiles = it->second.bool_value(ui_.intel_radar_show_hostiles);
      }
      if (auto it = obj->find("intel_radar_show_contacts"); it != obj->end()) {
        ui_.intel_radar_show_contacts = it->second.bool_value(ui_.intel_radar_show_contacts);
      }
      if (auto it = obj->find("intel_radar_labels"); it != obj->end()) {
        ui_.intel_radar_labels = it->second.bool_value(ui_.intel_radar_labels);
      }

      // Diplomacy Graph defaults.
      if (auto it = obj->find("diplomacy_graph_starfield"); it != obj->end()) {
        ui_.diplomacy_graph_starfield = it->second.bool_value(ui_.diplomacy_graph_starfield);
      }
      if (auto it = obj->find("diplomacy_graph_grid"); it != obj->end()) {
        ui_.diplomacy_graph_grid = it->second.bool_value(ui_.diplomacy_graph_grid);
      }
      if (auto it = obj->find("diplomacy_graph_labels"); it != obj->end()) {
        ui_.diplomacy_graph_labels = it->second.bool_value(ui_.diplomacy_graph_labels);
      }
      if (auto it = obj->find("diplomacy_graph_arrows"); it != obj->end()) {
        ui_.diplomacy_graph_arrows = it->second.bool_value(ui_.diplomacy_graph_arrows);
      }
      if (auto it = obj->find("diplomacy_graph_dim_nonfocus"); it != obj->end()) {
        ui_.diplomacy_graph_dim_nonfocus = it->second.bool_value(ui_.diplomacy_graph_dim_nonfocus);
      }
      if (auto it = obj->find("diplomacy_graph_show_hostile"); it != obj->end()) {
        ui_.diplomacy_graph_show_hostile = it->second.bool_value(ui_.diplomacy_graph_show_hostile);
      }
      if (auto it = obj->find("diplomacy_graph_show_neutral"); it != obj->end()) {
        ui_.diplomacy_graph_show_neutral = it->second.bool_value(ui_.diplomacy_graph_show_neutral);
      }
      if (auto it = obj->find("diplomacy_graph_show_friendly"); it != obj->end()) {
        ui_.diplomacy_graph_show_friendly = it->second.bool_value(ui_.diplomacy_graph_show_friendly);
      }
      if (auto it = obj->find("diplomacy_graph_layout"); it != obj->end()) {
        ui_.diplomacy_graph_layout = static_cast<int>(it->second.number_value(ui_.diplomacy_graph_layout));
        ui_.diplomacy_graph_layout = std::clamp(ui_.diplomacy_graph_layout, 0, 2);
      }

      // Docking behavior.
      if (auto it = obj->find("docking_with_shift"); it != obj->end()) {
        ui_.docking_with_shift = it->second.bool_value(ui_.docking_with_shift);
      }
      if (auto it = obj->find("docking_always_tab_bar"); it != obj->end()) {
        ui_.docking_always_tab_bar = it->second.bool_value(ui_.docking_always_tab_bar);
      }
      if (auto it = obj->find("docking_transparent_payload"); it != obj->end()) {
        ui_.docking_transparent_payload = it->second.bool_value(ui_.docking_transparent_payload);
      }

      // Multi-Viewport (detachable OS windows).
      if (auto it = obj->find("viewports_enable"); it != obj->end()) {
        ui_.viewports_enable = it->second.bool_value(ui_.viewports_enable);
      }
      if (auto it = obj->find("viewports_no_taskbar_icon"); it != obj->end()) {
        ui_.viewports_no_taskbar_icon = it->second.bool_value(ui_.viewports_no_taskbar_icon);
      }
      if (auto it = obj->find("viewports_no_auto_merge"); it != obj->end()) {
        ui_.viewports_no_auto_merge = it->second.bool_value(ui_.viewports_no_auto_merge);
      }
      if (auto it = obj->find("viewports_no_decoration"); it != obj->end()) {
        ui_.viewports_no_decoration = it->second.bool_value(ui_.viewports_no_decoration);
      }

      // Popup window management.
      if (auto it = obj->find("window_popup_first_mode"); it != obj->end()) {
        ui_.window_popup_first_mode = it->second.bool_value(ui_.window_popup_first_mode);
      }
      if (auto it = obj->find("window_popup_auto_focus"); it != obj->end()) {
        ui_.window_popup_auto_focus = it->second.bool_value(ui_.window_popup_auto_focus);
      }
      if (auto it = obj->find("window_popup_cascade_step_px"); it != obj->end()) {
        ui_.window_popup_cascade_step_px = static_cast<float>(it->second.number_value(ui_.window_popup_cascade_step_px));
        ui_.window_popup_cascade_step_px = std::clamp(ui_.window_popup_cascade_step_px, 0.0f, 128.0f);
      }
      if (auto it = obj->find("window_launch_overrides"); it != obj->end()) {
        if (const auto* ov_obj = it->second.as_object()) {
          ui_.window_launch_overrides.clear();
          for (const auto& kv : *ov_obj) {
            const std::string& id = kv.first;
            const int mode = static_cast<int>(kv.second.number_value(-1));
            if (mode == 0 || mode == 1) {
              ui_.window_launch_overrides[id] = mode;
            }
          }
        }
      }

      // Dock layout profiles (ImGui ini files).
      if (auto it = obj->find("layout_profiles_dir"); it != obj->end()) {
        const std::string v = it->second.string_value(std::string(ui_.layout_profiles_dir));
        std::snprintf(ui_.layout_profiles_dir, sizeof(ui_.layout_profiles_dir), "%s", v.c_str());
        ui_.layout_profiles_dir[sizeof(ui_.layout_profiles_dir) - 1] = '\0';
      }
      if (auto it = obj->find("layout_profile"); it != obj->end()) {
        const std::string v = it->second.string_value(std::string(ui_.layout_profile));
        std::snprintf(ui_.layout_profile, sizeof(ui_.layout_profile), "%s", v.c_str());
        ui_.layout_profile[sizeof(ui_.layout_profile) - 1] = '\0';
      }

      // Procedural dock layout synthesizer (DockBuilder presets).
      if (auto it = obj->find("ui_procedural_layout_seed"); it != obj->end()) {
        ui_.ui_procedural_layout_seed = static_cast<int>(it->second.number_value(ui_.ui_procedural_layout_seed));
      }
      if (auto it = obj->find("ui_procedural_layout_mode"); it != obj->end()) {
        ui_.ui_procedural_layout_mode = static_cast<int>(it->second.number_value(ui_.ui_procedural_layout_mode));
        ui_.ui_procedural_layout_mode = std::clamp(ui_.ui_procedural_layout_mode, 0, 4);
      }
      if (auto it = obj->find("ui_procedural_layout_variation"); it != obj->end()) {
        ui_.ui_procedural_layout_variation = static_cast<float>(it->second.number_value(ui_.ui_procedural_layout_variation));
        ui_.ui_procedural_layout_variation = std::clamp(ui_.ui_procedural_layout_variation, 0.0f, 1.0f);
      }
      if (auto it = obj->find("ui_procedural_layout_include_tools"); it != obj->end()) {
        ui_.ui_procedural_layout_include_tools = it->second.bool_value(ui_.ui_procedural_layout_include_tools);
      }
      if (auto it = obj->find("ui_procedural_layout_include_forge_panels"); it != obj->end()) {
        ui_.ui_procedural_layout_include_forge_panels = it->second.bool_value(ui_.ui_procedural_layout_include_forge_panels);
      }
      if (auto it = obj->find("ui_procedural_layout_max_forge_panels"); it != obj->end()) {
        ui_.ui_procedural_layout_max_forge_panels = static_cast<int>(it->second.number_value(ui_.ui_procedural_layout_max_forge_panels));
        ui_.ui_procedural_layout_max_forge_panels = std::clamp(ui_.ui_procedural_layout_max_forge_panels, 0, 32);
      }
      if (auto it = obj->find("ui_procedural_layout_auto_open_windows"); it != obj->end()) {
        ui_.ui_procedural_layout_auto_open_windows = it->second.bool_value(ui_.ui_procedural_layout_auto_open_windows);
      }
      if (auto it = obj->find("ui_procedural_layout_autosave_profile"); it != obj->end()) {
        ui_.ui_procedural_layout_autosave_profile = it->second.bool_value(ui_.ui_procedural_layout_autosave_profile);
      }

      // Map rendering chrome.
      if (auto it = obj->find("system_map_starfield"); it != obj->end()) {
        ui_.system_map_starfield = it->second.bool_value(ui_.system_map_starfield);
      }

      if (auto it = obj->find("system_map_grid"); it != obj->end()) {
        ui_.system_map_grid = it->second.bool_value(ui_.system_map_grid);
      }
      if (auto it = obj->find("system_map_order_paths"); it != obj->end()) {
        ui_.system_map_order_paths = it->second.bool_value(ui_.system_map_order_paths);
      }
      if (auto it = obj->find("system_map_fleet_formation_preview"); it != obj->end()) {
        ui_.system_map_fleet_formation_preview = it->second.bool_value(ui_.system_map_fleet_formation_preview);
      }
      if (auto it = obj->find("system_map_missile_salvos"); it != obj->end()) {
        ui_.system_map_missile_salvos = it->second.bool_value(ui_.system_map_missile_salvos);
      }
      if (auto it = obj->find("system_map_follow_selected"); it != obj->end()) {
        ui_.system_map_follow_selected = it->second.bool_value(ui_.system_map_follow_selected);
      }
      if (auto it = obj->find("system_map_show_minimap"); it != obj->end()) {
        ui_.system_map_show_minimap = it->second.bool_value(ui_.system_map_show_minimap);
      }
      if (auto it = obj->find("system_map_time_preview"); it != obj->end()) {
        ui_.system_map_time_preview = it->second.bool_value(ui_.system_map_time_preview);
      }
      if (auto it = obj->find("system_map_time_preview_days"); it != obj->end()) {
        ui_.system_map_time_preview_days =
            static_cast<float>(it->second.number_value(ui_.system_map_time_preview_days));
        ui_.system_map_time_preview_days = std::clamp(ui_.system_map_time_preview_days, -365.0f, 365.0f);
      }
      if (auto it = obj->find("system_map_time_preview_vectors"); it != obj->end()) {
        ui_.system_map_time_preview_vectors = it->second.bool_value(ui_.system_map_time_preview_vectors);
      }
      if (auto it = obj->find("system_map_time_preview_all_ships"); it != obj->end()) {
        ui_.system_map_time_preview_all_ships = it->second.bool_value(ui_.system_map_time_preview_all_ships);
      }
      if (auto it = obj->find("system_map_time_preview_trails"); it != obj->end()) {
        ui_.system_map_time_preview_trails = it->second.bool_value(ui_.system_map_time_preview_trails);
      }
      if (auto it = obj->find("system_map_sensor_heatmap"); it != obj->end()) {
        ui_.system_map_sensor_heatmap = it->second.bool_value(ui_.system_map_sensor_heatmap);
      }
      if (auto it = obj->find("system_map_threat_heatmap"); it != obj->end()) {
        ui_.system_map_threat_heatmap = it->second.bool_value(ui_.system_map_threat_heatmap);
      }
      if (auto it = obj->find("system_map_heatmap_opacity"); it != obj->end()) {
        ui_.system_map_heatmap_opacity =
            static_cast<float>(it->second.number_value(ui_.system_map_heatmap_opacity));
        ui_.system_map_heatmap_opacity = std::clamp(ui_.system_map_heatmap_opacity, 0.0f, 1.0f);
      }
      if (auto it = obj->find("system_map_heatmap_resolution"); it != obj->end()) {
        ui_.system_map_heatmap_resolution =
            static_cast<int>(it->second.number_value(ui_.system_map_heatmap_resolution));
        ui_.system_map_heatmap_resolution = std::clamp(ui_.system_map_heatmap_resolution, 16, 200);
      }

      // Experimental: LOS ray-traced sensor heatmap (UI-only).
      if (auto it = obj->find("system_map_sensor_heatmap_raytrace"); it != obj->end()) {
        ui_.system_map_sensor_heatmap_raytrace = it->second.bool_value(ui_.system_map_sensor_heatmap_raytrace);
      }
      if (auto it = obj->find("system_map_sensor_raytrace_max_depth"); it != obj->end()) {
        ui_.system_map_sensor_raytrace_max_depth = static_cast<int>(it->second.number_value(ui_.system_map_sensor_raytrace_max_depth));
        ui_.system_map_sensor_raytrace_max_depth = std::clamp(ui_.system_map_sensor_raytrace_max_depth, 0, 10);
      }
      if (auto it = obj->find("system_map_sensor_raytrace_error_threshold"); it != obj->end()) {
        ui_.system_map_sensor_raytrace_error_threshold =
            static_cast<float>(it->second.number_value(ui_.system_map_sensor_raytrace_error_threshold));
        ui_.system_map_sensor_raytrace_error_threshold = std::clamp(ui_.system_map_sensor_raytrace_error_threshold, 0.0f, 0.5f);
      }
      if (auto it = obj->find("system_map_sensor_raytrace_spp"); it != obj->end()) {
        ui_.system_map_sensor_raytrace_spp = static_cast<int>(it->second.number_value(ui_.system_map_sensor_raytrace_spp));
        ui_.system_map_sensor_raytrace_spp = std::clamp(ui_.system_map_sensor_raytrace_spp, 1, 16);
      }
      if (auto it = obj->find("system_map_sensor_raytrace_los_samples"); it != obj->end()) {
        ui_.system_map_sensor_raytrace_los_samples =
            static_cast<int>(it->second.number_value(ui_.system_map_sensor_raytrace_los_samples));
        ui_.system_map_sensor_raytrace_los_samples = std::clamp(ui_.system_map_sensor_raytrace_los_samples, 1, 64);
      }
      if (auto it = obj->find("system_map_sensor_raytrace_los_strength"); it != obj->end()) {
        ui_.system_map_sensor_raytrace_los_strength =
            static_cast<float>(it->second.number_value(ui_.system_map_sensor_raytrace_los_strength));
        ui_.system_map_sensor_raytrace_los_strength = std::clamp(ui_.system_map_sensor_raytrace_los_strength, 0.0f, 1.0f);
      }
      if (auto it = obj->find("system_map_sensor_raytrace_debug"); it != obj->end()) {
        ui_.system_map_sensor_raytrace_debug = it->second.bool_value(ui_.system_map_sensor_raytrace_debug);
      }
      if (auto it = obj->find("system_map_nebula_microfield_overlay"); it != obj->end()) {
        ui_.system_map_nebula_microfield_overlay = it->second.bool_value(ui_.system_map_nebula_microfield_overlay);
      }
      if (auto it = obj->find("system_map_nebula_overlay_opacity"); it != obj->end()) {
        ui_.system_map_nebula_overlay_opacity =
            static_cast<float>(it->second.number_value(ui_.system_map_nebula_overlay_opacity));
        ui_.system_map_nebula_overlay_opacity = std::clamp(ui_.system_map_nebula_overlay_opacity, 0.0f, 1.0f);
      }
      if (auto it = obj->find("system_map_nebula_overlay_resolution"); it != obj->end()) {
        ui_.system_map_nebula_overlay_resolution =
            static_cast<int>(it->second.number_value(ui_.system_map_nebula_overlay_resolution));
        ui_.system_map_nebula_overlay_resolution = std::clamp(ui_.system_map_nebula_overlay_resolution, 16, 260);
      }
      // System map storm cell overlay prefs.
      if (auto it = obj->find("system_map_storm_cell_overlay"); it != obj->end()) {
        ui_.system_map_storm_cell_overlay = it->second.bool_value(ui_.system_map_storm_cell_overlay);
      }
      if (auto it = obj->find("system_map_storm_overlay_opacity"); it != obj->end()) {
        ui_.system_map_storm_overlay_opacity =
            static_cast<float>(it->second.number_value(ui_.system_map_storm_overlay_opacity));
        ui_.system_map_storm_overlay_opacity = std::clamp(ui_.system_map_storm_overlay_opacity, 0.0f, 1.0f);
      }
      if (auto it = obj->find("system_map_storm_overlay_resolution"); it != obj->end()) {
        ui_.system_map_storm_overlay_resolution =
            static_cast<int>(it->second.number_value(ui_.system_map_storm_overlay_resolution));
        ui_.system_map_storm_overlay_resolution = std::clamp(ui_.system_map_storm_overlay_resolution, 16, 260);
      }

      if (auto it = obj->find("galaxy_map_starfield"); it != obj->end()) {
        ui_.galaxy_map_starfield = it->second.bool_value(ui_.galaxy_map_starfield);
      }

      if (auto it = obj->find("galaxy_map_grid"); it != obj->end()) {
        ui_.galaxy_map_grid = it->second.bool_value(ui_.galaxy_map_grid);
      }
      if (auto it = obj->find("galaxy_map_selected_route"); it != obj->end()) {
        ui_.galaxy_map_selected_route = it->second.bool_value(ui_.galaxy_map_selected_route);
      }
      if (auto it = obj->find("galaxy_map_show_minimap"); it != obj->end()) {
        ui_.galaxy_map_show_minimap = it->second.bool_value(ui_.galaxy_map_show_minimap);
      }
      if (auto it = obj->find("galaxy_map_fuel_range"); it != obj->end()) {
        ui_.galaxy_map_fuel_range = it->second.bool_value(ui_.galaxy_map_fuel_range);
      }
      if (auto it = obj->find("map_starfield_density"); it != obj->end()) {
        ui_.map_starfield_density = static_cast<float>(it->second.number_value(ui_.map_starfield_density));
        ui_.map_starfield_density = std::clamp(ui_.map_starfield_density, 0.0f, 4.0f);
      }
      if (auto it = obj->find("map_starfield_parallax"); it != obj->end()) {
        ui_.map_starfield_parallax = static_cast<float>(it->second.number_value(ui_.map_starfield_parallax));
        ui_.map_starfield_parallax = std::clamp(ui_.map_starfield_parallax, 0.0f, 1.0f);
      }
      if (auto it = obj->find("map_grid_opacity"); it != obj->end()) {
        ui_.map_grid_opacity = static_cast<float>(it->second.number_value(ui_.map_grid_opacity));
        ui_.map_grid_opacity = std::clamp(ui_.map_grid_opacity, 0.0f, 1.0f);
      }
      if (auto it = obj->find("map_route_opacity"); it != obj->end()) {
        ui_.map_route_opacity = static_cast<float>(it->second.number_value(ui_.map_route_opacity));
        ui_.map_route_opacity = std::clamp(ui_.map_route_opacity, 0.0f, 1.0f);
      }

      // Procedural particle field (dust)
      if (auto it = obj->find("galaxy_map_particle_field"); it != obj->end()) {
        ui_.galaxy_map_particle_field = it->second.bool_value(ui_.galaxy_map_particle_field);
      }
      if (auto it = obj->find("system_map_particle_field"); it != obj->end()) {
        ui_.system_map_particle_field = it->second.bool_value(ui_.system_map_particle_field);
      }
      if (auto it = obj->find("map_particle_tile_px"); it != obj->end()) {
        ui_.map_particle_tile_px = static_cast<int>(it->second.int_value(ui_.map_particle_tile_px));
        ui_.map_particle_tile_px = std::clamp(ui_.map_particle_tile_px, 64, 1024);
      }
      if (auto it = obj->find("map_particle_particles_per_tile"); it != obj->end()) {
        ui_.map_particle_particles_per_tile = static_cast<int>(it->second.int_value(ui_.map_particle_particles_per_tile));
        ui_.map_particle_particles_per_tile = std::clamp(ui_.map_particle_particles_per_tile, 8, 1024);
      }
      if (auto it = obj->find("map_particle_layers"); it != obj->end()) {
        ui_.map_particle_layers = static_cast<int>(it->second.int_value(ui_.map_particle_layers));
        ui_.map_particle_layers = std::clamp(ui_.map_particle_layers, 1, 3);
      }
      if (auto it = obj->find("map_particle_opacity"); it != obj->end()) {
        ui_.map_particle_opacity = static_cast<float>(it->second.number_value(ui_.map_particle_opacity));
        ui_.map_particle_opacity = std::clamp(ui_.map_particle_opacity, 0.0f, 1.0f);
      }
      if (auto it = obj->find("map_particle_base_radius_px"); it != obj->end()) {
        ui_.map_particle_base_radius_px = static_cast<float>(it->second.number_value(ui_.map_particle_base_radius_px));
        ui_.map_particle_base_radius_px = std::clamp(ui_.map_particle_base_radius_px, 0.1f, 8.0f);
      }
      if (auto it = obj->find("map_particle_radius_jitter_px"); it != obj->end()) {
        ui_.map_particle_radius_jitter_px = static_cast<float>(it->second.number_value(ui_.map_particle_radius_jitter_px));
        ui_.map_particle_radius_jitter_px = std::clamp(ui_.map_particle_radius_jitter_px, 0.0f, 12.0f);
      }
      if (auto it = obj->find("map_particle_twinkle_strength"); it != obj->end()) {
        ui_.map_particle_twinkle_strength = static_cast<float>(it->second.number_value(ui_.map_particle_twinkle_strength));
        ui_.map_particle_twinkle_strength = std::clamp(ui_.map_particle_twinkle_strength, 0.0f, 1.0f);
      }
      if (auto it = obj->find("map_particle_twinkle_speed"); it != obj->end()) {
        ui_.map_particle_twinkle_speed = static_cast<float>(it->second.number_value(ui_.map_particle_twinkle_speed));
        ui_.map_particle_twinkle_speed = std::clamp(ui_.map_particle_twinkle_speed, 0.0f, 8.0f);
      }
      if (auto it = obj->find("map_particle_drift"); it != obj->end()) {
        ui_.map_particle_drift = it->second.bool_value(ui_.map_particle_drift);
      }
      if (auto it = obj->find("map_particle_drift_px_per_day"); it != obj->end()) {
        ui_.map_particle_drift_px_per_day = static_cast<float>(it->second.number_value(ui_.map_particle_drift_px_per_day));
        ui_.map_particle_drift_px_per_day = std::clamp(ui_.map_particle_drift_px_per_day, 0.0f, 500.0f);
      }
      if (auto it = obj->find("map_particle_layer0_parallax"); it != obj->end()) {
        ui_.map_particle_layer0_parallax = static_cast<float>(it->second.number_value(ui_.map_particle_layer0_parallax));
        ui_.map_particle_layer0_parallax = std::clamp(ui_.map_particle_layer0_parallax, 0.0f, 1.0f);
      }
      if (auto it = obj->find("map_particle_layer1_parallax"); it != obj->end()) {
        ui_.map_particle_layer1_parallax = static_cast<float>(it->second.number_value(ui_.map_particle_layer1_parallax));
        ui_.map_particle_layer1_parallax = std::clamp(ui_.map_particle_layer1_parallax, 0.0f, 1.0f);
      }
      if (auto it = obj->find("map_particle_layer2_parallax"); it != obj->end()) {
        ui_.map_particle_layer2_parallax = static_cast<float>(it->second.number_value(ui_.map_particle_layer2_parallax));
        ui_.map_particle_layer2_parallax = std::clamp(ui_.map_particle_layer2_parallax, 0.0f, 1.0f);
      }
      if (auto it = obj->find("map_particle_sparkles"); it != obj->end()) {
        ui_.map_particle_sparkles = it->second.bool_value(ui_.map_particle_sparkles);
      }
      if (auto it = obj->find("map_particle_sparkle_chance"); it != obj->end()) {
        ui_.map_particle_sparkle_chance = static_cast<float>(it->second.number_value(ui_.map_particle_sparkle_chance));
        ui_.map_particle_sparkle_chance = std::clamp(ui_.map_particle_sparkle_chance, 0.0f, 1.0f);
      }
      if (auto it = obj->find("map_particle_sparkle_length_px"); it != obj->end()) {
        ui_.map_particle_sparkle_length_px = static_cast<float>(it->second.number_value(ui_.map_particle_sparkle_length_px));
        ui_.map_particle_sparkle_length_px = std::clamp(ui_.map_particle_sparkle_length_px, 0.0f, 64.0f);
      }
      if (auto it = obj->find("map_particle_debug_tiles"); it != obj->end()) {
        ui_.map_particle_debug_tiles = it->second.bool_value(ui_.map_particle_debug_tiles);
      }

      // Ray-marched SDF nebula (map background chrome).
      if (auto it = obj->find("map_raymarch_nebula"); it != obj->end()) {
        ui_.map_raymarch_nebula = it->second.bool_value(ui_.map_raymarch_nebula);
      }
      if (auto it = obj->find("map_raymarch_nebula_alpha"); it != obj->end()) {
        ui_.map_raymarch_nebula_alpha = static_cast<float>(it->second.number_value(ui_.map_raymarch_nebula_alpha));
        ui_.map_raymarch_nebula_alpha = std::clamp(ui_.map_raymarch_nebula_alpha, 0.0f, 1.0f);
      }
      if (auto it = obj->find("map_raymarch_nebula_parallax"); it != obj->end()) {
        ui_.map_raymarch_nebula_parallax = static_cast<float>(it->second.number_value(ui_.map_raymarch_nebula_parallax));
        ui_.map_raymarch_nebula_parallax = std::clamp(ui_.map_raymarch_nebula_parallax, 0.0f, 1.0f);
      }
      if (auto it = obj->find("map_raymarch_nebula_max_depth"); it != obj->end()) {
        ui_.map_raymarch_nebula_max_depth = static_cast<int>(it->second.number_value(ui_.map_raymarch_nebula_max_depth));
        ui_.map_raymarch_nebula_max_depth = std::clamp(ui_.map_raymarch_nebula_max_depth, 0, 10);
      }
      if (auto it = obj->find("map_raymarch_nebula_error_threshold"); it != obj->end()) {
        ui_.map_raymarch_nebula_error_threshold =
            static_cast<float>(it->second.number_value(ui_.map_raymarch_nebula_error_threshold));
        ui_.map_raymarch_nebula_error_threshold = std::clamp(ui_.map_raymarch_nebula_error_threshold, 0.0f, 0.5f);
      }
      if (auto it = obj->find("map_raymarch_nebula_spp"); it != obj->end()) {
        ui_.map_raymarch_nebula_spp = static_cast<int>(it->second.number_value(ui_.map_raymarch_nebula_spp));
        ui_.map_raymarch_nebula_spp = std::clamp(ui_.map_raymarch_nebula_spp, 1, 8);
      }
      if (auto it = obj->find("map_raymarch_nebula_max_steps"); it != obj->end()) {
        ui_.map_raymarch_nebula_max_steps = static_cast<int>(it->second.number_value(ui_.map_raymarch_nebula_max_steps));
        ui_.map_raymarch_nebula_max_steps = std::clamp(ui_.map_raymarch_nebula_max_steps, 8, 160);
      }
      if (auto it = obj->find("map_raymarch_nebula_animate"); it != obj->end()) {
        ui_.map_raymarch_nebula_animate = it->second.bool_value(ui_.map_raymarch_nebula_animate);
      }
      if (auto it = obj->find("map_raymarch_nebula_time_scale"); it != obj->end()) {
        ui_.map_raymarch_nebula_time_scale =
            static_cast<float>(it->second.number_value(ui_.map_raymarch_nebula_time_scale));
        ui_.map_raymarch_nebula_time_scale = std::clamp(ui_.map_raymarch_nebula_time_scale, 0.0f, 3.0f);
      }
      if (auto it = obj->find("map_raymarch_nebula_debug"); it != obj->end()) {
        ui_.map_raymarch_nebula_debug = it->second.bool_value(ui_.map_raymarch_nebula_debug);
      }

      // --- Map procedural background engine ---
      if (auto it = obj->find("map_proc_render_engine"); it != obj->end()) {
        ui_.map_proc_render_engine = it->second.bool_value(ui_.map_proc_render_engine);
      }
      if (auto it = obj->find("map_proc_render_tile_px"); it != obj->end()) {
        ui_.map_proc_render_tile_px = static_cast<int>(it->second.number_value(ui_.map_proc_render_tile_px));
        ui_.map_proc_render_tile_px = std::clamp(ui_.map_proc_render_tile_px, 64, 1024);
      }
      if (auto it = obj->find("map_proc_render_cache_tiles"); it != obj->end()) {
        ui_.map_proc_render_cache_tiles = static_cast<int>(it->second.number_value(ui_.map_proc_render_cache_tiles));
        ui_.map_proc_render_cache_tiles = std::clamp(ui_.map_proc_render_cache_tiles, 8, 2048);
      }
      if (auto it = obj->find("map_proc_render_nebula_enable"); it != obj->end()) {
        ui_.map_proc_render_nebula_enable = it->second.bool_value(ui_.map_proc_render_nebula_enable);
      }
      if (auto it = obj->find("map_proc_render_nebula_strength"); it != obj->end()) {
        ui_.map_proc_render_nebula_strength =
            static_cast<float>(it->second.number_value(ui_.map_proc_render_nebula_strength));
        ui_.map_proc_render_nebula_strength = std::clamp(ui_.map_proc_render_nebula_strength, 0.0f, 2.0f);
      }
      if (auto it = obj->find("map_proc_render_nebula_scale"); it != obj->end()) {
        ui_.map_proc_render_nebula_scale =
            static_cast<float>(it->second.number_value(ui_.map_proc_render_nebula_scale));
        ui_.map_proc_render_nebula_scale = std::clamp(ui_.map_proc_render_nebula_scale, 0.1f, 8.0f);
      }
      if (auto it = obj->find("map_proc_render_nebula_warp"); it != obj->end()) {
        ui_.map_proc_render_nebula_warp =
            static_cast<float>(it->second.number_value(ui_.map_proc_render_nebula_warp));
        ui_.map_proc_render_nebula_warp = std::clamp(ui_.map_proc_render_nebula_warp, 0.0f, 3.0f);
      }
      if (auto it = obj->find("map_proc_render_debug_tiles"); it != obj->end()) {
        ui_.map_proc_render_debug_tiles = it->second.bool_value(ui_.map_proc_render_debug_tiles);
      }

      // --- System map procedural body sprites ---
      if (auto it = obj->find("system_map_body_sprites"); it != obj->end()) {
        ui_.system_map_body_sprites = it->second.bool_value(ui_.system_map_body_sprites);
      }
      if (auto it = obj->find("system_map_body_sprite_px"); it != obj->end()) {
        ui_.system_map_body_sprite_px = static_cast<int>(it->second.number_value(ui_.system_map_body_sprite_px));
        ui_.system_map_body_sprite_px = std::clamp(ui_.system_map_body_sprite_px, 24, 512);
      }
      if (auto it = obj->find("system_map_body_sprite_cache"); it != obj->end()) {
        ui_.system_map_body_sprite_cache = static_cast<int>(it->second.number_value(ui_.system_map_body_sprite_cache));
        ui_.system_map_body_sprite_cache = std::clamp(ui_.system_map_body_sprite_cache, 16, 4096);
      }
      if (auto it = obj->find("system_map_body_sprite_light_steps"); it != obj->end()) {
        ui_.system_map_body_sprite_light_steps =
            static_cast<int>(it->second.number_value(ui_.system_map_body_sprite_light_steps));
        ui_.system_map_body_sprite_light_steps = std::clamp(ui_.system_map_body_sprite_light_steps, 4, 128);
      }
      if (auto it = obj->find("system_map_body_sprite_rings"); it != obj->end()) {
        ui_.system_map_body_sprite_rings = it->second.bool_value(ui_.system_map_body_sprite_rings);
      }
      if (auto it = obj->find("system_map_body_sprite_ring_chance"); it != obj->end()) {
        ui_.system_map_body_sprite_ring_chance =
            static_cast<float>(it->second.number_value(ui_.system_map_body_sprite_ring_chance));
        ui_.system_map_body_sprite_ring_chance = std::clamp(ui_.system_map_body_sprite_ring_chance, 0.0f, 1.0f);
      }
      if (auto it = obj->find("system_map_body_sprite_ambient"); it != obj->end()) {
        ui_.system_map_body_sprite_ambient =
            static_cast<float>(it->second.number_value(ui_.system_map_body_sprite_ambient));
        ui_.system_map_body_sprite_ambient = std::clamp(ui_.system_map_body_sprite_ambient, 0.0f, 1.0f);
      }
      if (auto it = obj->find("system_map_body_sprite_diffuse"); it != obj->end()) {
        ui_.system_map_body_sprite_diffuse =
            static_cast<float>(it->second.number_value(ui_.system_map_body_sprite_diffuse));
        ui_.system_map_body_sprite_diffuse = std::clamp(ui_.system_map_body_sprite_diffuse, 0.0f, 2.0f);
      }
      if (auto it = obj->find("system_map_body_sprite_specular"); it != obj->end()) {
        ui_.system_map_body_sprite_specular =
            static_cast<float>(it->second.number_value(ui_.system_map_body_sprite_specular));
        ui_.system_map_body_sprite_specular = std::clamp(ui_.system_map_body_sprite_specular, 0.0f, 2.0f);
      }
      if (auto it = obj->find("system_map_body_sprite_specular_power"); it != obj->end()) {
        ui_.system_map_body_sprite_specular_power =
            static_cast<float>(it->second.number_value(ui_.system_map_body_sprite_specular_power));
        ui_.system_map_body_sprite_specular_power = std::clamp(ui_.system_map_body_sprite_specular_power, 1.0f, 128.0f);
      }

      // --- System map procedural contact icons ---
      if (auto it = obj->find("system_map_contact_icons"); it != obj->end()) {
        ui_.system_map_contact_icons = it->second.bool_value(ui_.system_map_contact_icons);
      }
      if (auto it = obj->find("system_map_contact_icon_px"); it != obj->end()) {
        ui_.system_map_contact_icon_px = static_cast<int>(it->second.number_value(ui_.system_map_contact_icon_px));
        ui_.system_map_contact_icon_px = std::clamp(ui_.system_map_contact_icon_px, 16, 256);
      }
      if (auto it = obj->find("system_map_contact_icon_cache"); it != obj->end()) {
        ui_.system_map_contact_icon_cache = static_cast<int>(it->second.number_value(ui_.system_map_contact_icon_cache));
        ui_.system_map_contact_icon_cache = std::clamp(ui_.system_map_contact_icon_cache, 32, 4096);
      }
      if (auto it = obj->find("system_map_ship_icon_size_px"); it != obj->end()) {
        ui_.system_map_ship_icon_size_px = static_cast<float>(it->second.number_value(ui_.system_map_ship_icon_size_px));
        ui_.system_map_ship_icon_size_px = std::clamp(ui_.system_map_ship_icon_size_px, 4.0f, 96.0f);
      }
      if (auto it = obj->find("system_map_ship_icon_thrusters"); it != obj->end()) {
        ui_.system_map_ship_icon_thrusters = it->second.bool_value(ui_.system_map_ship_icon_thrusters);
      }
      if (auto it = obj->find("system_map_ship_icon_thruster_opacity"); it != obj->end()) {
        ui_.system_map_ship_icon_thruster_opacity =
            static_cast<float>(it->second.number_value(ui_.system_map_ship_icon_thruster_opacity));
        ui_.system_map_ship_icon_thruster_opacity = std::clamp(ui_.system_map_ship_icon_thruster_opacity, 0.0f, 1.0f);
      }
      if (auto it = obj->find("system_map_ship_icon_thruster_length_px"); it != obj->end()) {
        ui_.system_map_ship_icon_thruster_length_px =
            static_cast<float>(it->second.number_value(ui_.system_map_ship_icon_thruster_length_px));
        ui_.system_map_ship_icon_thruster_length_px = std::clamp(ui_.system_map_ship_icon_thruster_length_px, 0.0f, 128.0f);
      }
      if (auto it = obj->find("system_map_ship_icon_thruster_width_px"); it != obj->end()) {
        ui_.system_map_ship_icon_thruster_width_px =
            static_cast<float>(it->second.number_value(ui_.system_map_ship_icon_thruster_width_px));
        ui_.system_map_ship_icon_thruster_width_px = std::clamp(ui_.system_map_ship_icon_thruster_width_px, 0.0f, 128.0f);
      }
      if (auto it = obj->find("system_map_missile_icon_size_px"); it != obj->end()) {
        ui_.system_map_missile_icon_size_px =
            static_cast<float>(it->second.number_value(ui_.system_map_missile_icon_size_px));
        ui_.system_map_missile_icon_size_px = std::clamp(ui_.system_map_missile_icon_size_px, 4.0f, 64.0f);
      }
      if (auto it = obj->find("system_map_wreck_icon_size_px"); it != obj->end()) {
        ui_.system_map_wreck_icon_size_px = static_cast<float>(it->second.number_value(ui_.system_map_wreck_icon_size_px));
        ui_.system_map_wreck_icon_size_px = std::clamp(ui_.system_map_wreck_icon_size_px, 4.0f, 96.0f);
      }
      if (auto it = obj->find("system_map_anomaly_icon_size_px"); it != obj->end()) {
        ui_.system_map_anomaly_icon_size_px =
            static_cast<float>(it->second.number_value(ui_.system_map_anomaly_icon_size_px));
        ui_.system_map_anomaly_icon_size_px = std::clamp(ui_.system_map_anomaly_icon_size_px, 4.0f, 96.0f);
      }
      if (auto it = obj->find("system_map_anomaly_icon_pulse"); it != obj->end()) {
        ui_.system_map_anomaly_icon_pulse = it->second.bool_value(ui_.system_map_anomaly_icon_pulse);
      }
      if (auto it = obj->find("system_map_contact_icon_debug_bounds"); it != obj->end()) {
        ui_.system_map_contact_icon_debug_bounds = it->second.bool_value(ui_.system_map_contact_icon_debug_bounds);
      }

      // --- System map jump-point phenomena ---
      if (auto it = obj->find("system_map_jump_phenomena"); it != obj->end()) {
        ui_.system_map_jump_phenomena = it->second.bool_value(ui_.system_map_jump_phenomena);
      }
      if (auto it = obj->find("system_map_jump_phenomena_reveal_unsurveyed"); it != obj->end()) {
        ui_.system_map_jump_phenomena_reveal_unsurveyed =
            it->second.bool_value(ui_.system_map_jump_phenomena_reveal_unsurveyed);
      }
      if (auto it = obj->find("system_map_jump_phenomena_sprite_px"); it != obj->end()) {
        ui_.system_map_jump_phenomena_sprite_px =
            static_cast<int>(it->second.number_value(ui_.system_map_jump_phenomena_sprite_px));
        ui_.system_map_jump_phenomena_sprite_px = std::clamp(ui_.system_map_jump_phenomena_sprite_px, 16, 512);
      }
      if (auto it = obj->find("system_map_jump_phenomena_cache"); it != obj->end()) {
        ui_.system_map_jump_phenomena_cache =
            static_cast<int>(it->second.number_value(ui_.system_map_jump_phenomena_cache));
        ui_.system_map_jump_phenomena_cache = std::clamp(ui_.system_map_jump_phenomena_cache, 16, 4096);
      }
      if (auto it = obj->find("system_map_jump_phenomena_size_mult"); it != obj->end()) {
        ui_.system_map_jump_phenomena_size_mult =
            static_cast<float>(it->second.number_value(ui_.system_map_jump_phenomena_size_mult));
        ui_.system_map_jump_phenomena_size_mult = std::clamp(ui_.system_map_jump_phenomena_size_mult, 1.0f, 16.0f);
      }
      if (auto it = obj->find("system_map_jump_phenomena_opacity"); it != obj->end()) {
        ui_.system_map_jump_phenomena_opacity =
            static_cast<float>(it->second.number_value(ui_.system_map_jump_phenomena_opacity));
        ui_.system_map_jump_phenomena_opacity = std::clamp(ui_.system_map_jump_phenomena_opacity, 0.0f, 1.0f);
      }
      if (auto it = obj->find("system_map_jump_phenomena_animate"); it != obj->end()) {
        ui_.system_map_jump_phenomena_animate = it->second.bool_value(ui_.system_map_jump_phenomena_animate);
      }
      if (auto it = obj->find("system_map_jump_phenomena_anim_speed_cycles_per_day"); it != obj->end()) {
        ui_.system_map_jump_phenomena_anim_speed_cycles_per_day = static_cast<float>(
            it->second.number_value(ui_.system_map_jump_phenomena_anim_speed_cycles_per_day));
        ui_.system_map_jump_phenomena_anim_speed_cycles_per_day =
            std::clamp(ui_.system_map_jump_phenomena_anim_speed_cycles_per_day, 0.0f, 4.0f);
      }
      if (auto it = obj->find("system_map_jump_phenomena_pulse"); it != obj->end()) {
        ui_.system_map_jump_phenomena_pulse = it->second.bool_value(ui_.system_map_jump_phenomena_pulse);
      }
      if (auto it = obj->find("system_map_jump_phenomena_pulse_cycles_per_day"); it != obj->end()) {
        ui_.system_map_jump_phenomena_pulse_cycles_per_day =
            static_cast<float>(it->second.number_value(ui_.system_map_jump_phenomena_pulse_cycles_per_day));
        ui_.system_map_jump_phenomena_pulse_cycles_per_day =
            std::clamp(ui_.system_map_jump_phenomena_pulse_cycles_per_day, 0.0f, 6.0f);
      }
      if (auto it = obj->find("system_map_jump_phenomena_filaments"); it != obj->end()) {
        ui_.system_map_jump_phenomena_filaments = it->second.bool_value(ui_.system_map_jump_phenomena_filaments);
      }
      if (auto it = obj->find("system_map_jump_phenomena_filaments_max"); it != obj->end()) {
        ui_.system_map_jump_phenomena_filaments_max =
            static_cast<int>(it->second.number_value(ui_.system_map_jump_phenomena_filaments_max));
        ui_.system_map_jump_phenomena_filaments_max = std::clamp(ui_.system_map_jump_phenomena_filaments_max, 0, 32);
      }
      if (auto it = obj->find("system_map_jump_phenomena_filament_strength"); it != obj->end()) {
        ui_.system_map_jump_phenomena_filament_strength =
            static_cast<float>(it->second.number_value(ui_.system_map_jump_phenomena_filament_strength));
        ui_.system_map_jump_phenomena_filament_strength =
            std::clamp(ui_.system_map_jump_phenomena_filament_strength, 0.0f, 4.0f);
      }
      if (auto it = obj->find("system_map_jump_phenomena_debug_bounds"); it != obj->end()) {
        ui_.system_map_jump_phenomena_debug_bounds = it->second.bool_value(ui_.system_map_jump_phenomena_debug_bounds);
      }

      // --- System map anomaly phenomena ---
      if (auto it = obj->find("system_map_anomaly_phenomena"); it != obj->end()) {
        ui_.system_map_anomaly_phenomena = it->second.bool_value(ui_.system_map_anomaly_phenomena);
      }
      if (auto it = obj->find("system_map_anomaly_phenomena_sprite_px"); it != obj->end()) {
        ui_.system_map_anomaly_phenomena_sprite_px =
            static_cast<int>(it->second.number_value(ui_.system_map_anomaly_phenomena_sprite_px));
        ui_.system_map_anomaly_phenomena_sprite_px = std::clamp(ui_.system_map_anomaly_phenomena_sprite_px, 16, 512);
      }
      if (auto it = obj->find("system_map_anomaly_phenomena_cache"); it != obj->end()) {
        ui_.system_map_anomaly_phenomena_cache =
            static_cast<int>(it->second.number_value(ui_.system_map_anomaly_phenomena_cache));
        ui_.system_map_anomaly_phenomena_cache = std::clamp(ui_.system_map_anomaly_phenomena_cache, 16, 4096);
      }
      if (auto it = obj->find("system_map_anomaly_phenomena_size_mult"); it != obj->end()) {
        ui_.system_map_anomaly_phenomena_size_mult =
            static_cast<float>(it->second.number_value(ui_.system_map_anomaly_phenomena_size_mult));
        ui_.system_map_anomaly_phenomena_size_mult = std::clamp(ui_.system_map_anomaly_phenomena_size_mult, 1.0f, 16.0f);
      }
      if (auto it = obj->find("system_map_anomaly_phenomena_opacity"); it != obj->end()) {
        ui_.system_map_anomaly_phenomena_opacity =
            static_cast<float>(it->second.number_value(ui_.system_map_anomaly_phenomena_opacity));
        ui_.system_map_anomaly_phenomena_opacity = std::clamp(ui_.system_map_anomaly_phenomena_opacity, 0.0f, 1.0f);
      }
      if (auto it = obj->find("system_map_anomaly_phenomena_animate"); it != obj->end()) {
        ui_.system_map_anomaly_phenomena_animate = it->second.bool_value(ui_.system_map_anomaly_phenomena_animate);
      }
      if (auto it = obj->find("system_map_anomaly_phenomena_anim_speed_cycles_per_day"); it != obj->end()) {
        ui_.system_map_anomaly_phenomena_anim_speed_cycles_per_day = static_cast<float>(
            it->second.number_value(ui_.system_map_anomaly_phenomena_anim_speed_cycles_per_day));
        ui_.system_map_anomaly_phenomena_anim_speed_cycles_per_day =
            std::clamp(ui_.system_map_anomaly_phenomena_anim_speed_cycles_per_day, 0.0f, 4.0f);
      }
      if (auto it = obj->find("system_map_anomaly_phenomena_pulse"); it != obj->end()) {
        ui_.system_map_anomaly_phenomena_pulse = it->second.bool_value(ui_.system_map_anomaly_phenomena_pulse);
      }
      if (auto it = obj->find("system_map_anomaly_phenomena_pulse_cycles_per_day"); it != obj->end()) {
        ui_.system_map_anomaly_phenomena_pulse_cycles_per_day =
            static_cast<float>(it->second.number_value(ui_.system_map_anomaly_phenomena_pulse_cycles_per_day));
        ui_.system_map_anomaly_phenomena_pulse_cycles_per_day =
            std::clamp(ui_.system_map_anomaly_phenomena_pulse_cycles_per_day, 0.0f, 6.0f);
      }
      if (auto it = obj->find("system_map_anomaly_phenomena_filaments"); it != obj->end()) {
        ui_.system_map_anomaly_phenomena_filaments = it->second.bool_value(ui_.system_map_anomaly_phenomena_filaments);
      }
      if (auto it = obj->find("system_map_anomaly_phenomena_filaments_max"); it != obj->end()) {
        ui_.system_map_anomaly_phenomena_filaments_max =
            static_cast<int>(it->second.number_value(ui_.system_map_anomaly_phenomena_filaments_max));
        ui_.system_map_anomaly_phenomena_filaments_max = std::clamp(ui_.system_map_anomaly_phenomena_filaments_max, 0, 64);
      }
      if (auto it = obj->find("system_map_anomaly_phenomena_filament_strength"); it != obj->end()) {
        ui_.system_map_anomaly_phenomena_filament_strength =
            static_cast<float>(it->second.number_value(ui_.system_map_anomaly_phenomena_filament_strength));
        ui_.system_map_anomaly_phenomena_filament_strength =
            std::clamp(ui_.system_map_anomaly_phenomena_filament_strength, 0.0f, 4.0f);
      }
      if (auto it = obj->find("system_map_anomaly_phenomena_glyph_overlay"); it != obj->end()) {
        ui_.system_map_anomaly_phenomena_glyph_overlay =
            it->second.bool_value(ui_.system_map_anomaly_phenomena_glyph_overlay);
      }
      if (auto it = obj->find("system_map_anomaly_phenomena_glyph_strength"); it != obj->end()) {
        ui_.system_map_anomaly_phenomena_glyph_strength =
            static_cast<float>(it->second.number_value(ui_.system_map_anomaly_phenomena_glyph_strength));
        ui_.system_map_anomaly_phenomena_glyph_strength =
            std::clamp(ui_.system_map_anomaly_phenomena_glyph_strength, 0.0f, 1.0f);
      }
      if (auto it = obj->find("system_map_anomaly_phenomena_debug_bounds"); it != obj->end()) {
        ui_.system_map_anomaly_phenomena_debug_bounds =
            it->second.bool_value(ui_.system_map_anomaly_phenomena_debug_bounds);
      }

      // --- System map motion trails (vector FX) ---
      if (auto it = obj->find("system_map_motion_trails"); it != obj->end()) {
        ui_.system_map_motion_trails = it->second.bool_value(ui_.system_map_motion_trails);
      }
      if (auto it = obj->find("system_map_motion_trails_all_ships"); it != obj->end()) {
        ui_.system_map_motion_trails_all_ships = it->second.bool_value(ui_.system_map_motion_trails_all_ships);
      }
      if (auto it = obj->find("system_map_motion_trails_missiles"); it != obj->end()) {
        ui_.system_map_motion_trails_missiles = it->second.bool_value(ui_.system_map_motion_trails_missiles);
      }
      if (auto it = obj->find("system_map_motion_trails_max_age_days"); it != obj->end()) {
        ui_.system_map_motion_trails_max_age_days =
            static_cast<float>(it->second.number_value(ui_.system_map_motion_trails_max_age_days));
        ui_.system_map_motion_trails_max_age_days = std::clamp(ui_.system_map_motion_trails_max_age_days, 0.25f, 60.0f);
      }
      if (auto it = obj->find("system_map_motion_trails_sample_hours"); it != obj->end()) {
        ui_.system_map_motion_trails_sample_hours =
            static_cast<float>(it->second.number_value(ui_.system_map_motion_trails_sample_hours));
        ui_.system_map_motion_trails_sample_hours = std::clamp(ui_.system_map_motion_trails_sample_hours, 0.05f, 72.0f);
      }
      if (auto it = obj->find("system_map_motion_trails_min_seg_px"); it != obj->end()) {
        ui_.system_map_motion_trails_min_seg_px =
            static_cast<float>(it->second.number_value(ui_.system_map_motion_trails_min_seg_px));
        ui_.system_map_motion_trails_min_seg_px = std::clamp(ui_.system_map_motion_trails_min_seg_px, 0.5f, 32.0f);
      }
      if (auto it = obj->find("system_map_motion_trails_thickness_px"); it != obj->end()) {
        ui_.system_map_motion_trails_thickness_px =
            static_cast<float>(it->second.number_value(ui_.system_map_motion_trails_thickness_px));
        ui_.system_map_motion_trails_thickness_px = std::clamp(ui_.system_map_motion_trails_thickness_px, 0.5f, 12.0f);
      }
      if (auto it = obj->find("system_map_motion_trails_alpha"); it != obj->end()) {
        ui_.system_map_motion_trails_alpha =
            static_cast<float>(it->second.number_value(ui_.system_map_motion_trails_alpha));
        ui_.system_map_motion_trails_alpha = std::clamp(ui_.system_map_motion_trails_alpha, 0.0f, 1.0f);
      }
      if (auto it = obj->find("system_map_motion_trails_speed_brighten"); it != obj->end()) {
        ui_.system_map_motion_trails_speed_brighten = it->second.bool_value(ui_.system_map_motion_trails_speed_brighten);
      }

      // --- System map flow field (space weather) ---
      if (auto it = obj->find("system_map_flow_field_overlay"); it != obj->end()) {
        ui_.system_map_flow_field_overlay = it->second.bool_value(ui_.system_map_flow_field_overlay);
      }
      if (auto it = obj->find("system_map_flow_field_animate"); it != obj->end()) {
        ui_.system_map_flow_field_animate = it->second.bool_value(ui_.system_map_flow_field_animate);
      }
      if (auto it = obj->find("system_map_flow_field_mask_nebula"); it != obj->end()) {
        ui_.system_map_flow_field_mask_nebula = it->second.bool_value(ui_.system_map_flow_field_mask_nebula);
      }
      if (auto it = obj->find("system_map_flow_field_mask_storms"); it != obj->end()) {
        ui_.system_map_flow_field_mask_storms = it->second.bool_value(ui_.system_map_flow_field_mask_storms);
      }
      if (auto it = obj->find("system_map_flow_field_debug_tiles"); it != obj->end()) {
        ui_.system_map_flow_field_debug_tiles = it->second.bool_value(ui_.system_map_flow_field_debug_tiles);
      }

      if (auto it = obj->find("system_map_flow_field_opacity"); it != obj->end()) {
        ui_.system_map_flow_field_opacity = static_cast<float>(it->second.number_value(ui_.system_map_flow_field_opacity));
        ui_.system_map_flow_field_opacity = std::clamp(ui_.system_map_flow_field_opacity, 0.0f, 1.0f);
      }
      if (auto it = obj->find("system_map_flow_field_thickness_px"); it != obj->end()) {
        ui_.system_map_flow_field_thickness_px =
            static_cast<float>(it->second.number_value(ui_.system_map_flow_field_thickness_px));
        ui_.system_map_flow_field_thickness_px = std::clamp(ui_.system_map_flow_field_thickness_px, 0.5f, 12.0f);
      }
      if (auto it = obj->find("system_map_flow_field_step_px"); it != obj->end()) {
        ui_.system_map_flow_field_step_px = static_cast<float>(it->second.number_value(ui_.system_map_flow_field_step_px));
        ui_.system_map_flow_field_step_px = std::clamp(ui_.system_map_flow_field_step_px, 1.0f, 64.0f);
      }
      if (auto it = obj->find("system_map_flow_field_highlight_wavelength_px"); it != obj->end()) {
        ui_.system_map_flow_field_highlight_wavelength_px =
            static_cast<float>(it->second.number_value(ui_.system_map_flow_field_highlight_wavelength_px));
        ui_.system_map_flow_field_highlight_wavelength_px =
            std::clamp(ui_.system_map_flow_field_highlight_wavelength_px, 32.0f, 2000.0f);
      }
      if (auto it = obj->find("system_map_flow_field_animate_speed_cycles_per_day"); it != obj->end()) {
        ui_.system_map_flow_field_animate_speed_cycles_per_day =
            static_cast<float>(it->second.number_value(ui_.system_map_flow_field_animate_speed_cycles_per_day));
        ui_.system_map_flow_field_animate_speed_cycles_per_day =
            std::clamp(ui_.system_map_flow_field_animate_speed_cycles_per_day, 0.0f, 4.0f);
      }
      if (auto it = obj->find("system_map_flow_field_nebula_threshold"); it != obj->end()) {
        ui_.system_map_flow_field_nebula_threshold =
            static_cast<float>(it->second.number_value(ui_.system_map_flow_field_nebula_threshold));
        ui_.system_map_flow_field_nebula_threshold = std::clamp(ui_.system_map_flow_field_nebula_threshold, 0.0f, 1.0f);
      }
      if (auto it = obj->find("system_map_flow_field_storm_threshold"); it != obj->end()) {
        ui_.system_map_flow_field_storm_threshold =
            static_cast<float>(it->second.number_value(ui_.system_map_flow_field_storm_threshold));
        ui_.system_map_flow_field_storm_threshold = std::clamp(ui_.system_map_flow_field_storm_threshold, 0.0f, 1.0f);
      }
      if (auto it = obj->find("system_map_flow_field_scale_mkm"); it != obj->end()) {
        ui_.system_map_flow_field_scale_mkm = static_cast<float>(it->second.number_value(ui_.system_map_flow_field_scale_mkm));
        ui_.system_map_flow_field_scale_mkm = std::clamp(ui_.system_map_flow_field_scale_mkm, 250.0f, 500000.0f);
      }

      if (auto it = obj->find("system_map_flow_field_tile_px"); it != obj->end()) {
        ui_.system_map_flow_field_tile_px = static_cast<int>(it->second.number_value(ui_.system_map_flow_field_tile_px));
        ui_.system_map_flow_field_tile_px = std::clamp(ui_.system_map_flow_field_tile_px, 64, 1024);
      }
      if (auto it = obj->find("system_map_flow_field_cache_tiles"); it != obj->end()) {
        ui_.system_map_flow_field_cache_tiles =
            static_cast<int>(it->second.number_value(ui_.system_map_flow_field_cache_tiles));
        ui_.system_map_flow_field_cache_tiles = std::clamp(ui_.system_map_flow_field_cache_tiles, 0, 100000);
      }
      if (auto it = obj->find("system_map_flow_field_lines_per_tile"); it != obj->end()) {
        ui_.system_map_flow_field_lines_per_tile =
            static_cast<int>(it->second.number_value(ui_.system_map_flow_field_lines_per_tile));
        ui_.system_map_flow_field_lines_per_tile = std::clamp(ui_.system_map_flow_field_lines_per_tile, 1, 64);
      }
      if (auto it = obj->find("system_map_flow_field_steps_per_line"); it != obj->end()) {
        ui_.system_map_flow_field_steps_per_line =
            static_cast<int>(it->second.number_value(ui_.system_map_flow_field_steps_per_line));
        ui_.system_map_flow_field_steps_per_line = std::clamp(ui_.system_map_flow_field_steps_per_line, 4, 250);
      }

      // --- System map gravity contours ("gravity wells") ---
      if (auto it = obj->find("system_map_gravity_contours_overlay"); it != obj->end()) {
        ui_.system_map_gravity_contours_overlay =
            it->second.bool_value(ui_.system_map_gravity_contours_overlay);
      }
      if (auto it = obj->find("system_map_gravity_contours_debug_tiles"); it != obj->end()) {
        ui_.system_map_gravity_contours_debug_tiles =
            it->second.bool_value(ui_.system_map_gravity_contours_debug_tiles);
      }

      if (auto it = obj->find("system_map_gravity_contours_opacity"); it != obj->end()) {
        ui_.system_map_gravity_contours_opacity =
            static_cast<float>(it->second.number_value(ui_.system_map_gravity_contours_opacity));
        ui_.system_map_gravity_contours_opacity = std::clamp(ui_.system_map_gravity_contours_opacity, 0.0f, 1.0f);
      }
      if (auto it = obj->find("system_map_gravity_contours_thickness_px"); it != obj->end()) {
        ui_.system_map_gravity_contours_thickness_px =
            static_cast<float>(it->second.number_value(ui_.system_map_gravity_contours_thickness_px));
        ui_.system_map_gravity_contours_thickness_px =
            std::clamp(ui_.system_map_gravity_contours_thickness_px, 0.5f, 12.0f);
      }

      if (auto it = obj->find("system_map_gravity_contours_tile_px"); it != obj->end()) {
        ui_.system_map_gravity_contours_tile_px =
            static_cast<int>(it->second.number_value(ui_.system_map_gravity_contours_tile_px));
        ui_.system_map_gravity_contours_tile_px = std::clamp(ui_.system_map_gravity_contours_tile_px, 64, 2048);
      }
      if (auto it = obj->find("system_map_gravity_contours_cache_tiles"); it != obj->end()) {
        ui_.system_map_gravity_contours_cache_tiles =
            static_cast<int>(it->second.number_value(ui_.system_map_gravity_contours_cache_tiles));
        ui_.system_map_gravity_contours_cache_tiles = std::clamp(ui_.system_map_gravity_contours_cache_tiles, 0, 4096);
      }
      if (auto it = obj->find("system_map_gravity_contours_samples_per_tile"); it != obj->end()) {
        ui_.system_map_gravity_contours_samples_per_tile =
            static_cast<int>(it->second.number_value(ui_.system_map_gravity_contours_samples_per_tile));
        ui_.system_map_gravity_contours_samples_per_tile =
            std::clamp(ui_.system_map_gravity_contours_samples_per_tile, 8, 96);
      }
      if (auto it = obj->find("system_map_gravity_contours_levels"); it != obj->end()) {
        ui_.system_map_gravity_contours_levels =
            static_cast<int>(it->second.number_value(ui_.system_map_gravity_contours_levels));
        ui_.system_map_gravity_contours_levels = std::clamp(ui_.system_map_gravity_contours_levels, 1, 32);
      }
      if (auto it = obj->find("system_map_gravity_contours_level_spacing_decades"); it != obj->end()) {
        ui_.system_map_gravity_contours_level_spacing_decades =
            static_cast<float>(it->second.number_value(ui_.system_map_gravity_contours_level_spacing_decades));
        ui_.system_map_gravity_contours_level_spacing_decades =
            std::clamp(ui_.system_map_gravity_contours_level_spacing_decades, 0.05f, 2.0f);
      }
      if (auto it = obj->find("system_map_gravity_contours_level_offset_decades"); it != obj->end()) {
        ui_.system_map_gravity_contours_level_offset_decades =
            static_cast<float>(it->second.number_value(ui_.system_map_gravity_contours_level_offset_decades));
        ui_.system_map_gravity_contours_level_offset_decades =
            std::clamp(ui_.system_map_gravity_contours_level_offset_decades, -10.0f, 10.0f);
      }

      if (auto it = obj->find("system_map_gravity_contours_softening_min_mkm"); it != obj->end()) {
        ui_.system_map_gravity_contours_softening_min_mkm =
            static_cast<float>(it->second.number_value(ui_.system_map_gravity_contours_softening_min_mkm));
        ui_.system_map_gravity_contours_softening_min_mkm =
            std::clamp(ui_.system_map_gravity_contours_softening_min_mkm, 0.0001f, 10000.0f);
      }
      if (auto it = obj->find("system_map_gravity_contours_softening_radius_mult"); it != obj->end()) {
        ui_.system_map_gravity_contours_softening_radius_mult =
            static_cast<float>(it->second.number_value(ui_.system_map_gravity_contours_softening_radius_mult));
        ui_.system_map_gravity_contours_softening_radius_mult =
            std::clamp(ui_.system_map_gravity_contours_softening_radius_mult, 0.0f, 50.0f);
      }

      // Combat / tactical overlays.
      if (auto it = obj->find("show_selected_weapon_range"); it != obj->end()) {
        ui_.show_selected_weapon_range = it->second.bool_value(ui_.show_selected_weapon_range);
      }
      if (auto it = obj->find("show_fleet_weapon_ranges"); it != obj->end()) {
        ui_.show_fleet_weapon_ranges = it->second.bool_value(ui_.show_fleet_weapon_ranges);
      }
      if (auto it = obj->find("show_hostile_weapon_ranges"); it != obj->end()) {
        ui_.show_hostile_weapon_ranges = it->second.bool_value(ui_.show_hostile_weapon_ranges);
      }

      // Map intel/exploration toggles.
      if (auto it = obj->find("show_selected_sensor_range"); it != obj->end()) {
        ui_.show_selected_sensor_range = it->second.bool_value(ui_.show_selected_sensor_range);
      }
      if (auto it = obj->find("show_faction_sensor_coverage"); it != obj->end()) {
        ui_.show_faction_sensor_coverage = it->second.bool_value(ui_.show_faction_sensor_coverage);
      }
      if (auto it = obj->find("faction_sensor_coverage_fill"); it != obj->end()) {
        ui_.faction_sensor_coverage_fill = it->second.bool_value(ui_.faction_sensor_coverage_fill);
      }
      if (auto it = obj->find("faction_sensor_coverage_signature"); it != obj->end()) {
        ui_.faction_sensor_coverage_signature =
            static_cast<float>(it->second.number_value(ui_.faction_sensor_coverage_signature));
      }
      if (auto it = obj->find("faction_sensor_coverage_max_sources"); it != obj->end()) {
        ui_.faction_sensor_coverage_max_sources =
            static_cast<int>(it->second.number_value(ui_.faction_sensor_coverage_max_sources));
      }
      ui_.faction_sensor_coverage_signature = std::clamp(ui_.faction_sensor_coverage_signature, 0.05f, 100.0f);
      ui_.faction_sensor_coverage_max_sources = std::clamp(ui_.faction_sensor_coverage_max_sources, 1, 4096);
      if (auto it = obj->find("show_contact_markers"); it != obj->end()) {
        ui_.show_contact_markers = it->second.bool_value(ui_.show_contact_markers);
      }
      if (auto it = obj->find("show_contact_labels"); it != obj->end()) {
        ui_.show_contact_labels = it->second.bool_value(ui_.show_contact_labels);
      }
      if (auto it = obj->find("show_contact_uncertainty"); it != obj->end()) {
        ui_.show_contact_uncertainty = it->second.bool_value(ui_.show_contact_uncertainty);
      }
      if (auto it = obj->find("show_minor_bodies"); it != obj->end()) {
        ui_.show_minor_bodies = it->second.bool_value(ui_.show_minor_bodies);
      }
      if (auto it = obj->find("show_minor_body_labels"); it != obj->end()) {
        ui_.show_minor_body_labels = it->second.bool_value(ui_.show_minor_body_labels);
      }
      if (auto it = obj->find("show_galaxy_labels"); it != obj->end()) {
        ui_.show_galaxy_labels = it->second.bool_value(ui_.show_galaxy_labels);
      }
      if (auto it = obj->find("show_galaxy_pins"); it != obj->end()) {
        ui_.show_galaxy_pins = it->second.bool_value(ui_.show_galaxy_pins);
      }
      if (auto it = obj->find("show_galaxy_jump_lines"); it != obj->end()) {
        ui_.show_galaxy_jump_lines = it->second.bool_value(ui_.show_galaxy_jump_lines);
      }
      if (auto it = obj->find("show_galaxy_unknown_exits"); it != obj->end()) {
        ui_.show_galaxy_unknown_exits = it->second.bool_value(ui_.show_galaxy_unknown_exits);
      }
      if (auto it = obj->find("show_galaxy_intel_alerts"); it != obj->end()) {
        ui_.show_galaxy_intel_alerts = it->second.bool_value(ui_.show_galaxy_intel_alerts);
      }
      if (auto it = obj->find("show_galaxy_freight_lanes"); it != obj->end()) {
        ui_.show_galaxy_freight_lanes = it->second.bool_value(ui_.show_galaxy_freight_lanes);
      }
      if (auto it = obj->find("show_galaxy_trade_lanes"); it != obj->end()) {
        ui_.show_galaxy_trade_lanes = it->second.bool_value(ui_.show_galaxy_trade_lanes);
      }
      if (auto it = obj->find("show_galaxy_trade_hubs"); it != obj->end()) {
        ui_.show_galaxy_trade_hubs = it->second.bool_value(ui_.show_galaxy_trade_hubs);
      }

      if (auto it = obj->find("galaxy_trade_good_filter"); it != obj->end()) {
        ui_.galaxy_trade_good_filter = static_cast<int>(it->second.number_value(ui_.galaxy_trade_good_filter));
      }
      // -1 means "all goods".
      ui_.galaxy_trade_good_filter = std::clamp(ui_.galaxy_trade_good_filter, -1, kTradeGoodKindCount - 1);

      if (auto it = obj->find("galaxy_trade_filter_include_secondary"); it != obj->end()) {
        ui_.galaxy_trade_filter_include_secondary = it->second.bool_value(ui_.galaxy_trade_filter_include_secondary);
      }
      if (auto it = obj->find("galaxy_trade_min_lane_volume"); it != obj->end()) {
        ui_.galaxy_trade_min_lane_volume = static_cast<float>(it->second.number_value(ui_.galaxy_trade_min_lane_volume));
      }
      ui_.galaxy_trade_min_lane_volume = std::clamp(ui_.galaxy_trade_min_lane_volume, 0.0f, 1.0e9f);

      if (auto it = obj->find("galaxy_trade_risk_overlay"); it != obj->end()) {
        ui_.galaxy_trade_risk_overlay = it->second.bool_value(ui_.galaxy_trade_risk_overlay);
      }
      if (auto it = obj->find("galaxy_trade_security_panel"); it != obj->end()) {
        ui_.galaxy_trade_security_panel = it->second.bool_value(ui_.galaxy_trade_security_panel);
      }
      if (auto it = obj->find("galaxy_trade_security_top_n"); it != obj->end()) {
        ui_.galaxy_trade_security_top_n = static_cast<int>(it->second.number_value(ui_.galaxy_trade_security_top_n));
      }
      ui_.galaxy_trade_security_top_n = std::clamp(ui_.galaxy_trade_security_top_n, 3, 30);
      if (auto it = obj->find("show_galaxy_fleet_missions"); it != obj->end()) {
        ui_.show_galaxy_fleet_missions = it->second.bool_value(ui_.show_galaxy_fleet_missions);
      }
      if (auto it = obj->find("galaxy_fleet_mission_alpha"); it != obj->end()) {
        ui_.galaxy_fleet_mission_alpha = static_cast<float>(it->second.number_value(ui_.galaxy_fleet_mission_alpha));
      }
      ui_.galaxy_fleet_mission_alpha = std::clamp(ui_.galaxy_fleet_mission_alpha, 0.05f, 1.0f);
      if (auto it = obj->find("show_galaxy_chokepoints"); it != obj->end()) {
        ui_.show_galaxy_chokepoints = it->second.bool_value(ui_.show_galaxy_chokepoints);
      }
      if (auto it = obj->find("show_galaxy_regions"); it != obj->end()) {
        ui_.show_galaxy_regions = it->second.bool_value(ui_.show_galaxy_regions);
      }
      if (auto it = obj->find("show_galaxy_region_labels"); it != obj->end()) {
        ui_.show_galaxy_region_labels = it->second.bool_value(ui_.show_galaxy_region_labels);
      }
      if (auto it = obj->find("show_galaxy_region_boundaries"); it != obj->end()) {
        ui_.show_galaxy_region_boundaries = it->second.bool_value(ui_.show_galaxy_region_boundaries);
      }
      if (auto it = obj->find("galaxy_region_dim_nonselected"); it != obj->end()) {
        ui_.galaxy_region_dim_nonselected = it->second.bool_value(ui_.galaxy_region_dim_nonselected);
      }
      if (auto it = obj->find("galaxy_region_boundary_voronoi"); it != obj->end()) {
        ui_.galaxy_region_boundary_voronoi = it->second.bool_value(ui_.galaxy_region_boundary_voronoi);
      }
      if (auto it = obj->find("show_galaxy_region_centers"); it != obj->end()) {
        ui_.show_galaxy_region_centers = it->second.bool_value(ui_.show_galaxy_region_centers);
      }
      if (auto it = obj->find("show_galaxy_region_border_links"); it != obj->end()) {
        ui_.show_galaxy_region_border_links = it->second.bool_value(ui_.show_galaxy_region_border_links);
      }

      if (auto it = obj->find("galaxy_map_territory_overlay"); it != obj->end()) {
        ui_.galaxy_map_territory_overlay = it->second.bool_value(ui_.galaxy_map_territory_overlay);
      }
      if (auto it = obj->find("galaxy_map_territory_fill"); it != obj->end()) {
        ui_.galaxy_map_territory_fill = it->second.bool_value(ui_.galaxy_map_territory_fill);
      }
      if (auto it = obj->find("galaxy_map_territory_boundaries"); it != obj->end()) {
        ui_.galaxy_map_territory_boundaries = it->second.bool_value(ui_.galaxy_map_territory_boundaries);
      }
      if (auto it = obj->find("galaxy_map_territory_fill_opacity"); it != obj->end()) {
        ui_.galaxy_map_territory_fill_opacity = static_cast<float>(it->second.number_value(ui_.galaxy_map_territory_fill_opacity));
      }
      ui_.galaxy_map_territory_fill_opacity = std::clamp(ui_.galaxy_map_territory_fill_opacity, 0.0f, 1.0f);
      if (auto it = obj->find("galaxy_map_territory_boundary_opacity"); it != obj->end()) {
        ui_.galaxy_map_territory_boundary_opacity = static_cast<float>(it->second.number_value(ui_.galaxy_map_territory_boundary_opacity));
      }
      ui_.galaxy_map_territory_boundary_opacity = std::clamp(ui_.galaxy_map_territory_boundary_opacity, 0.0f, 1.0f);
      if (auto it = obj->find("galaxy_map_territory_boundary_thickness_px"); it != obj->end()) {
        ui_.galaxy_map_territory_boundary_thickness_px = static_cast<float>(it->second.number_value(ui_.galaxy_map_territory_boundary_thickness_px));
      }
      ui_.galaxy_map_territory_boundary_thickness_px = std::clamp(ui_.galaxy_map_territory_boundary_thickness_px, 0.25f, 8.0f);
      if (auto it = obj->find("galaxy_map_territory_tile_px"); it != obj->end()) {
        ui_.galaxy_map_territory_tile_px = static_cast<int>(it->second.number_value(ui_.galaxy_map_territory_tile_px));
      }
      ui_.galaxy_map_territory_tile_px = std::clamp(ui_.galaxy_map_territory_tile_px, 96, 1024);
      if (auto it = obj->find("galaxy_map_territory_cache_tiles"); it != obj->end()) {
        ui_.galaxy_map_territory_cache_tiles = static_cast<int>(it->second.number_value(ui_.galaxy_map_territory_cache_tiles));
      }
      ui_.galaxy_map_territory_cache_tiles = std::clamp(ui_.galaxy_map_territory_cache_tiles, 8, 20000);
      if (auto it = obj->find("galaxy_map_territory_samples_per_tile"); it != obj->end()) {
        ui_.galaxy_map_territory_samples_per_tile = static_cast<int>(it->second.number_value(ui_.galaxy_map_territory_samples_per_tile));
      }
      ui_.galaxy_map_territory_samples_per_tile = std::clamp(ui_.galaxy_map_territory_samples_per_tile, 8, 128);
      if (auto it = obj->find("galaxy_map_territory_influence_base_spacing_mult"); it != obj->end()) {
        ui_.galaxy_map_territory_influence_base_spacing_mult = static_cast<float>(it->second.number_value(ui_.galaxy_map_territory_influence_base_spacing_mult));
      }
      ui_.galaxy_map_territory_influence_base_spacing_mult = std::clamp(ui_.galaxy_map_territory_influence_base_spacing_mult, 0.0f, 12.0f);
      if (auto it = obj->find("galaxy_map_territory_influence_pop_spacing_mult"); it != obj->end()) {
        ui_.galaxy_map_territory_influence_pop_spacing_mult = static_cast<float>(it->second.number_value(ui_.galaxy_map_territory_influence_pop_spacing_mult));
      }
      ui_.galaxy_map_territory_influence_pop_spacing_mult = std::clamp(ui_.galaxy_map_territory_influence_pop_spacing_mult, 0.0f, 12.0f);
      if (auto it = obj->find("galaxy_map_territory_influence_pop_log_bias"); it != obj->end()) {
        ui_.galaxy_map_territory_influence_pop_log_bias = static_cast<float>(it->second.number_value(ui_.galaxy_map_territory_influence_pop_log_bias));
      }
      ui_.galaxy_map_territory_influence_pop_log_bias = std::clamp(ui_.galaxy_map_territory_influence_pop_log_bias, 0.1f, 1000.0f);
      if (auto it = obj->find("galaxy_map_territory_presence_falloff_spacing"); it != obj->end()) {
        ui_.galaxy_map_territory_presence_falloff_spacing = static_cast<float>(it->second.number_value(ui_.galaxy_map_territory_presence_falloff_spacing));
      }
      ui_.galaxy_map_territory_presence_falloff_spacing = std::clamp(ui_.galaxy_map_territory_presence_falloff_spacing, 0.1f, 32.0f);
      if (auto it = obj->find("galaxy_map_territory_dominance_softness_spacing"); it != obj->end()) {
        ui_.galaxy_map_territory_dominance_softness_spacing = static_cast<float>(it->second.number_value(ui_.galaxy_map_territory_dominance_softness_spacing));
      }
      ui_.galaxy_map_territory_dominance_softness_spacing = std::clamp(ui_.galaxy_map_territory_dominance_softness_spacing, 0.05f, 32.0f);
      if (auto it = obj->find("galaxy_map_territory_contested_dither"); it != obj->end()) {
        ui_.galaxy_map_territory_contested_dither = it->second.bool_value(ui_.galaxy_map_territory_contested_dither);
      }
      if (auto it = obj->find("galaxy_map_territory_contested_threshold"); it != obj->end()) {
        ui_.galaxy_map_territory_contested_threshold = static_cast<float>(it->second.number_value(ui_.galaxy_map_territory_contested_threshold));
      }
      ui_.galaxy_map_territory_contested_threshold = std::clamp(ui_.galaxy_map_territory_contested_threshold, 0.0f, 1.0f);
      if (auto it = obj->find("galaxy_map_territory_contested_dither_strength"); it != obj->end()) {
        ui_.galaxy_map_territory_contested_dither_strength = static_cast<float>(it->second.number_value(ui_.galaxy_map_territory_contested_dither_strength));
      }
      ui_.galaxy_map_territory_contested_dither_strength = std::clamp(ui_.galaxy_map_territory_contested_dither_strength, 0.0f, 1.0f);
      if (auto it = obj->find("galaxy_map_territory_debug_tiles"); it != obj->end()) {
        ui_.galaxy_map_territory_debug_tiles = it->second.bool_value(ui_.galaxy_map_territory_debug_tiles);
      }
      if (auto it = obj->find("galaxy_procgen_lens_mode"); it != obj->end()) {
        ui_.galaxy_procgen_lens_mode = static_cast<ProcGenLensMode>(
            static_cast<int>(it->second.number_value(static_cast<double>(ui_.galaxy_procgen_lens_mode))));
      }
      if (auto it = obj->find("galaxy_procgen_lens_alpha"); it != obj->end()) {
        ui_.galaxy_procgen_lens_alpha = static_cast<float>(it->second.number_value(ui_.galaxy_procgen_lens_alpha));
        ui_.galaxy_procgen_lens_alpha = std::clamp(ui_.galaxy_procgen_lens_alpha, 0.0f, 1.0f);
      }
      if (auto it = obj->find("galaxy_procgen_lens_show_legend"); it != obj->end()) {
        ui_.galaxy_procgen_lens_show_legend = it->second.bool_value(ui_.galaxy_procgen_lens_show_legend);
      }
      if (auto it = obj->find("galaxy_procgen_lens_log_scale"); it != obj->end()) {
        ui_.galaxy_procgen_lens_log_scale = it->second.bool_value(ui_.galaxy_procgen_lens_log_scale);
      }
      if (auto it = obj->find("galaxy_procgen_field"); it != obj->end()) {
        ui_.galaxy_procgen_field = it->second.bool_value(ui_.galaxy_procgen_field);
      }
      if (auto it = obj->find("galaxy_procgen_field_alpha"); it != obj->end()) {
        ui_.galaxy_procgen_field_alpha = static_cast<float>(it->second.number_value(ui_.galaxy_procgen_field_alpha));
        ui_.galaxy_procgen_field_alpha = std::clamp(ui_.galaxy_procgen_field_alpha, 0.0f, 1.0f);
      }
      if (auto it = obj->find("galaxy_procgen_field_cell_px"); it != obj->end()) {
        ui_.galaxy_procgen_field_cell_px = static_cast<int>(it->second.number_value(ui_.galaxy_procgen_field_cell_px));
        ui_.galaxy_procgen_field_cell_px = std::clamp(ui_.galaxy_procgen_field_cell_px, 4, 96);
      }
      if (auto it = obj->find("galaxy_procgen_contours"); it != obj->end()) {
        ui_.galaxy_procgen_contours = it->second.bool_value(ui_.galaxy_procgen_contours);
      }
      if (auto it = obj->find("galaxy_procgen_contour_alpha"); it != obj->end()) {
        ui_.galaxy_procgen_contour_alpha = static_cast<float>(it->second.number_value(ui_.galaxy_procgen_contour_alpha));
        ui_.galaxy_procgen_contour_alpha = std::clamp(ui_.galaxy_procgen_contour_alpha, 0.0f, 1.0f);
      }
      if (auto it = obj->find("galaxy_procgen_contour_cell_px"); it != obj->end()) {
        ui_.galaxy_procgen_contour_cell_px = static_cast<int>(it->second.number_value(ui_.galaxy_procgen_contour_cell_px));
        ui_.galaxy_procgen_contour_cell_px = std::clamp(ui_.galaxy_procgen_contour_cell_px, 4, 128);
      }
      if (auto it = obj->find("galaxy_procgen_contour_levels"); it != obj->end()) {
        ui_.galaxy_procgen_contour_levels = static_cast<int>(it->second.number_value(ui_.galaxy_procgen_contour_levels));
        ui_.galaxy_procgen_contour_levels = std::clamp(ui_.galaxy_procgen_contour_levels, 2, 16);
      }
      if (auto it = obj->find("galaxy_procgen_contour_thickness"); it != obj->end()) {
        ui_.galaxy_procgen_contour_thickness = static_cast<float>(it->second.number_value(ui_.galaxy_procgen_contour_thickness));
        ui_.galaxy_procgen_contour_thickness = std::clamp(ui_.galaxy_procgen_contour_thickness, 0.5f, 6.0f);
      }
      if (auto it = obj->find("galaxy_procgen_vectors"); it != obj->end()) {
        ui_.galaxy_procgen_vectors = it->second.bool_value(ui_.galaxy_procgen_vectors);
      }
      if (auto it = obj->find("galaxy_procgen_vector_alpha"); it != obj->end()) {
        ui_.galaxy_procgen_vector_alpha = static_cast<float>(it->second.number_value(ui_.galaxy_procgen_vector_alpha));
        ui_.galaxy_procgen_vector_alpha = std::clamp(ui_.galaxy_procgen_vector_alpha, 0.0f, 1.0f);
      }
      if (auto it = obj->find("galaxy_procgen_vector_cell_px"); it != obj->end()) {
        ui_.galaxy_procgen_vector_cell_px = static_cast<int>(it->second.number_value(ui_.galaxy_procgen_vector_cell_px));
        ui_.galaxy_procgen_vector_cell_px = std::clamp(ui_.galaxy_procgen_vector_cell_px, 6, 192);
      }
      if (auto it = obj->find("galaxy_procgen_vector_scale"); it != obj->end()) {
        ui_.galaxy_procgen_vector_scale = static_cast<float>(it->second.number_value(ui_.galaxy_procgen_vector_scale));
        ui_.galaxy_procgen_vector_scale = std::clamp(ui_.galaxy_procgen_vector_scale, 1.0f, 600.0f);
      }
      if (auto it = obj->find("galaxy_procgen_vector_min_mag"); it != obj->end()) {
        ui_.galaxy_procgen_vector_min_mag = static_cast<float>(it->second.number_value(ui_.galaxy_procgen_vector_min_mag));
        ui_.galaxy_procgen_vector_min_mag = std::clamp(ui_.galaxy_procgen_vector_min_mag, 0.0f, 1.0f);
      }
      if (auto it = obj->find("galaxy_procgen_probe"); it != obj->end()) {
        ui_.galaxy_procgen_probe = it->second.bool_value(ui_.galaxy_procgen_probe);
      }

      // Star Atlas overlay (procedural constellations).
      if (auto it = obj->find("galaxy_star_atlas_constellations"); it != obj->end()) {
        ui_.galaxy_star_atlas_constellations = it->second.bool_value(ui_.galaxy_star_atlas_constellations);
      }
      if (auto it = obj->find("galaxy_star_atlas_labels"); it != obj->end()) {
        ui_.galaxy_star_atlas_labels = it->second.bool_value(ui_.galaxy_star_atlas_labels);
      }
      if (auto it = obj->find("galaxy_star_atlas_alpha"); it != obj->end()) {
        ui_.galaxy_star_atlas_alpha = static_cast<float>(it->second.number_value(ui_.galaxy_star_atlas_alpha));
        ui_.galaxy_star_atlas_alpha = std::clamp(ui_.galaxy_star_atlas_alpha, 0.0f, 1.0f);
      }
      if (auto it = obj->find("galaxy_star_atlas_label_alpha"); it != obj->end()) {
        ui_.galaxy_star_atlas_label_alpha = static_cast<float>(it->second.number_value(ui_.galaxy_star_atlas_label_alpha));
        ui_.galaxy_star_atlas_label_alpha = std::clamp(ui_.galaxy_star_atlas_label_alpha, 0.0f, 1.0f);
      }
      if (auto it = obj->find("galaxy_star_atlas_target_cluster_size"); it != obj->end()) {
        ui_.galaxy_star_atlas_target_cluster_size = static_cast<int>(it->second.number_value(ui_.galaxy_star_atlas_target_cluster_size));
        ui_.galaxy_star_atlas_target_cluster_size = std::clamp(ui_.galaxy_star_atlas_target_cluster_size, 4, 24);
      }
      if (auto it = obj->find("galaxy_star_atlas_max_constellations"); it != obj->end()) {
        ui_.galaxy_star_atlas_max_constellations = static_cast<int>(it->second.number_value(ui_.galaxy_star_atlas_max_constellations));
        ui_.galaxy_star_atlas_max_constellations = std::clamp(ui_.galaxy_star_atlas_max_constellations, 0, 1000);
      }
      if (auto it = obj->find("galaxy_star_atlas_min_zoom"); it != obj->end()) {
        ui_.galaxy_star_atlas_min_zoom = static_cast<float>(it->second.number_value(ui_.galaxy_star_atlas_min_zoom));
        ui_.galaxy_star_atlas_min_zoom = std::clamp(ui_.galaxy_star_atlas_min_zoom, 0.01f, 5.0f);
      }

      if (auto it = obj->find("contact_max_age_days"); it != obj->end()) {
        ui_.contact_max_age_days = static_cast<int>(it->second.number_value(ui_.contact_max_age_days));
        ui_.contact_max_age_days = std::clamp(ui_.contact_max_age_days, 1, 3650);
      }
    }

    // Window layout.
    {
      if (auto it = obj->find("show_controls_window"); it != obj->end()) {
        ui_.show_controls_window = it->second.bool_value(ui_.show_controls_window);
      }
      if (auto it = obj->find("show_map_window"); it != obj->end()) {
        ui_.show_map_window = it->second.bool_value(ui_.show_map_window);
      }
      if (auto it = obj->find("show_details_window"); it != obj->end()) {
        ui_.show_details_window = it->second.bool_value(ui_.show_details_window);
      }
      if (auto it = obj->find("show_directory_window"); it != obj->end()) {
        ui_.show_directory_window = it->second.bool_value(ui_.show_directory_window);
      }
      if (auto it = obj->find("show_production_window"); it != obj->end()) {
        ui_.show_production_window = it->second.bool_value(ui_.show_production_window);
      }
      if (auto it = obj->find("show_economy_window"); it != obj->end()) {
        ui_.show_economy_window = it->second.bool_value(ui_.show_economy_window);
      }
      if (auto it = obj->find("show_planner_window"); it != obj->end()) {
        ui_.show_planner_window = it->second.bool_value(ui_.show_planner_window);
      }
      if (auto it = obj->find("show_regions_window"); it != obj->end()) {
        ui_.show_regions_window = it->second.bool_value(ui_.show_regions_window);
      }
      if (auto it = obj->find("show_freight_window"); it != obj->end()) {
        ui_.show_freight_window = it->second.bool_value(ui_.show_freight_window);
      }
      if (auto it = obj->find("show_mine_window"); it != obj->end()) {
        ui_.show_mine_window = it->second.bool_value(ui_.show_mine_window);
      }
      if (auto it = obj->find("show_fuel_window"); it != obj->end()) {
        ui_.show_fuel_window = it->second.bool_value(ui_.show_fuel_window);
      }
      if (auto it = obj->find("show_sustainment_window"); it != obj->end()) {
        ui_.show_sustainment_window = it->second.bool_value(ui_.show_sustainment_window);
      }
      if (auto it = obj->find("show_repair_planner_window"); it != obj->end()) {
        ui_.show_repair_planner_window = it->second.bool_value(ui_.show_repair_planner_window);
      }
      if (auto it = obj->find("show_maintenance_planner_window"); it != obj->end()) {
        ui_.show_maintenance_planner_window = it->second.bool_value(ui_.show_maintenance_planner_window);
      }
      if (auto it = obj->find("show_fleet_manager_window"); it != obj->end()) {
        ui_.show_fleet_manager_window = it->second.bool_value(ui_.show_fleet_manager_window);
      }
      if (auto it = obj->find("show_troop_window"); it != obj->end()) {
        ui_.show_troop_window = it->second.bool_value(ui_.show_troop_window);
      }
      if (auto it = obj->find("show_colonist_window"); it != obj->end()) {
        ui_.show_colonist_window = it->second.bool_value(ui_.show_colonist_window);
      }
      if (auto it = obj->find("show_terraforming_window"); it != obj->end()) {
        ui_.show_terraforming_window = it->second.bool_value(ui_.show_terraforming_window);
      }
      if (auto it = obj->find("show_time_warp_window"); it != obj->end()) {
        ui_.show_time_warp_window = it->second.bool_value(ui_.show_time_warp_window);
      }
      if (auto it = obj->find("show_timeline_window"); it != obj->end()) {
        ui_.show_timeline_window = it->second.bool_value(ui_.show_timeline_window);
      }
      if (auto it = obj->find("show_notifications_window"); it != obj->end()) {
        ui_.show_notifications_window = it->second.bool_value(ui_.show_notifications_window);
      }
      if (auto it = obj->find("show_design_studio_window"); it != obj->end()) {
        ui_.show_design_studio_window = it->second.bool_value(ui_.show_design_studio_window);
      }
      if (auto it = obj->find("show_balance_lab_window"); it != obj->end()) {
        ui_.show_balance_lab_window = it->second.bool_value(ui_.show_balance_lab_window);
      }
      if (auto it = obj->find("show_battle_forecast_window"); it != obj->end()) {
        ui_.show_battle_forecast_window = it->second.bool_value(ui_.show_battle_forecast_window);
      }
      if (auto it = obj->find("show_procgen_atlas_window"); it != obj->end()) {
        ui_.show_procgen_atlas_window = it->second.bool_value(ui_.show_procgen_atlas_window);
      }
      if (auto it = obj->find("show_star_atlas_window"); it != obj->end()) {
        ui_.show_star_atlas_window = it->second.bool_value(ui_.show_star_atlas_window);
      }
      if (auto it = obj->find("show_intel_window"); it != obj->end()) {
        ui_.show_intel_window = it->second.bool_value(ui_.show_intel_window);
      }
      if (auto it = obj->find("show_intel_notebook_window"); it != obj->end()) {
        ui_.show_intel_notebook_window = it->second.bool_value(ui_.show_intel_notebook_window);
      }
      if (auto it = obj->find("show_diplomacy_window"); it != obj->end()) {
        ui_.show_diplomacy_window = it->second.bool_value(ui_.show_diplomacy_window);
      }
      if (auto it = obj->find("show_victory_window"); it != obj->end()) {
        ui_.show_victory_window = it->second.bool_value(ui_.show_victory_window);
      }
      if (auto it = obj->find("show_settings_window"); it != obj->end()) {
        ui_.show_settings_window = it->second.bool_value(ui_.show_settings_window);
      }
      if (auto it = obj->find("show_save_tools_window"); it != obj->end()) {
        ui_.show_save_tools_window = it->second.bool_value(ui_.show_save_tools_window);
      }
      if (auto it = obj->find("show_time_machine_window"); it != obj->end()) {
        ui_.show_time_machine_window = it->second.bool_value(ui_.show_time_machine_window);
      }
      if (auto it = obj->find("show_compare_window"); it != obj->end()) {
        ui_.show_compare_window = it->second.bool_value(ui_.show_compare_window);
      }
      if (auto it = obj->find("show_omni_search_window"); it != obj->end()) {
        ui_.show_omni_search_window = it->second.bool_value(ui_.show_omni_search_window);
      }
      if (auto it = obj->find("show_json_explorer_window"); it != obj->end()) {
        ui_.show_json_explorer_window = it->second.bool_value(ui_.show_json_explorer_window);
      }
      if (auto it = obj->find("show_content_validation_window"); it != obj->end()) {
        ui_.show_content_validation_window = it->second.bool_value(ui_.show_content_validation_window);
      }

      if (auto it = obj->find("show_state_doctor_window"); it != obj->end()) {
        ui_.show_state_doctor_window = it->second.bool_value(ui_.show_state_doctor_window);
      }
      if (auto it = obj->find("show_trace_viewer_window"); it != obj->end()) {
        ui_.show_trace_viewer_window = it->second.bool_value(ui_.show_trace_viewer_window);
      }
      if (auto it = obj->find("show_entity_inspector_window"); it != obj->end()) {
        ui_.show_entity_inspector_window = it->second.bool_value(ui_.show_entity_inspector_window);
      }
      if (auto it = obj->find("show_reference_graph_window"); it != obj->end()) {
        ui_.show_reference_graph_window = it->second.bool_value(ui_.show_reference_graph_window);
      }
      if (auto it = obj->find("show_layout_profiles_window"); it != obj->end()) {
        ui_.show_layout_profiles_window = it->second.bool_value(ui_.show_layout_profiles_window);
      }

      if (auto it = obj->find("show_watchboard_window"); it != obj->end()) {
        ui_.show_watchboard_window = it->second.bool_value(ui_.show_watchboard_window);
      }
      if (auto it = obj->find("show_data_lenses_window"); it != obj->end()) {
        ui_.show_data_lenses_window = it->second.bool_value(ui_.show_data_lenses_window);
      }
      if (auto it = obj->find("show_dashboards_window"); it != obj->end()) {
        ui_.show_dashboards_window = it->second.bool_value(ui_.show_dashboards_window);
      }
      if (auto it = obj->find("show_pivot_tables_window"); it != obj->end()) {
        ui_.show_pivot_tables_window = it->second.bool_value(ui_.show_pivot_tables_window);
      }
      if (auto it = obj->find("show_ui_forge_window"); it != obj->end()) {
        ui_.show_ui_forge_window = it->second.bool_value(ui_.show_ui_forge_window);
      }
      if (auto it = obj->find("show_status_bar"); it != obj->end()) {
        ui_.show_status_bar = it->second.bool_value(ui_.show_status_bar);
      }

      // Trace Viewer (performance profiler) preferences.
      if (auto it = obj->find("trace_viewer_autostart"); it != obj->end()) {
        ui_.trace_viewer_autostart = it->second.bool_value(ui_.trace_viewer_autostart);
      }
      if (auto it = obj->find("trace_viewer_auto_refresh"); it != obj->end()) {
        ui_.trace_viewer_auto_refresh = it->second.bool_value(ui_.trace_viewer_auto_refresh);
      }
      if (auto it = obj->find("trace_viewer_refresh_sec"); it != obj->end()) {
        ui_.trace_viewer_refresh_sec = static_cast<float>(it->second.number_value(ui_.trace_viewer_refresh_sec));
        ui_.trace_viewer_refresh_sec = std::clamp(ui_.trace_viewer_refresh_sec, 0.05f, 2.0f);
      }
      if (auto it = obj->find("trace_viewer_max_events"); it != obj->end()) {
        ui_.trace_viewer_max_events = static_cast<int>(it->second.number_value(ui_.trace_viewer_max_events));
        ui_.trace_viewer_max_events = std::clamp(ui_.trace_viewer_max_events, 0, 500000);
      }
      if (auto it = obj->find("trace_viewer_follow_tail"); it != obj->end()) {
        ui_.trace_viewer_follow_tail = it->second.bool_value(ui_.trace_viewer_follow_tail);
      }
      if (auto it = obj->find("trace_viewer_window_ms"); it != obj->end()) {
        ui_.trace_viewer_window_ms = static_cast<float>(it->second.number_value(ui_.trace_viewer_window_ms));
        ui_.trace_viewer_window_ms = std::clamp(ui_.trace_viewer_window_ms, 10.0f, 60000.0f);
      }
      if (auto it = obj->find("trace_viewer_export_path"); it != obj->end()) {
        ui_.trace_viewer_export_path = it->second.string_value(ui_.trace_viewer_export_path);
      }
    }

    // Command Console preferences.
    //
    // Stored as stable command ids in ui_prefs.json.
    {
      auto load_string_array = [&](const char* key, std::vector<std::string>* out, int max_items) {
        if (auto it = obj->find(key); it != obj->end()) {
          if (const auto* arr = it->second.as_array()) {
            out->clear();
            out->reserve(std::min<int>(static_cast<int>(arr->size()), max_items));
            for (const auto& e : *arr) {
              if (static_cast<int>(out->size()) >= max_items) break;
              std::string s = e.string_value("");
              if (s.empty()) continue;
              if (s.size() > 128) s.resize(128);
              out->push_back(std::move(s));
            }
          }
        }
      };

      // Optional; introduced in ui_prefs v34.
      load_string_array("command_favorites", &ui_.command_favorites, 64);
      load_string_array("command_recent", &ui_.command_recent, 32);
      if (auto it = obj->find("command_recent_limit"); it != obj->end()) {
        ui_.command_recent_limit = static_cast<int>(it->second.number_value(ui_.command_recent_limit));
        ui_.command_recent_limit = std::clamp(ui_.command_recent_limit, 0, 200);
      }
    }

    // OmniSearch (game JSON global search) preferences.
    {
      if (auto it = obj->find("omni_search_match_keys"); it != obj->end()) {
        ui_.omni_search_match_keys = it->second.bool_value(ui_.omni_search_match_keys);
      }
      if (auto it = obj->find("omni_search_match_values"); it != obj->end()) {
        ui_.omni_search_match_values = it->second.bool_value(ui_.omni_search_match_values);
      }
      if (auto it = obj->find("omni_search_match_entities"); it != obj->end()) {
        ui_.omni_search_match_entities = it->second.bool_value(ui_.omni_search_match_entities);
      }
      if (auto it = obj->find("omni_search_match_docs"); it != obj->end()) {
        ui_.omni_search_match_docs = it->second.bool_value(ui_.omni_search_match_docs);
      }
      if (auto it = obj->find("omni_search_match_windows"); it != obj->end()) {
        ui_.omni_search_match_windows = it->second.bool_value(ui_.omni_search_match_windows);
      }
      if (auto it = obj->find("omni_search_match_layouts"); it != obj->end()) {
        ui_.omni_search_match_layouts = it->second.bool_value(ui_.omni_search_match_layouts);
      }
      if (auto it = obj->find("omni_search_case_sensitive"); it != obj->end()) {
        ui_.omni_search_case_sensitive = it->second.bool_value(ui_.omni_search_case_sensitive);
      }
      if (auto it = obj->find("omni_search_auto_refresh"); it != obj->end()) {
        ui_.omni_search_auto_refresh = it->second.bool_value(ui_.omni_search_auto_refresh);
      }
      if (auto it = obj->find("omni_search_refresh_sec"); it != obj->end()) {
        ui_.omni_search_refresh_sec = static_cast<float>(it->second.number_value(ui_.omni_search_refresh_sec));
      }
      if (auto it = obj->find("omni_search_nodes_per_frame"); it != obj->end()) {
        ui_.omni_search_nodes_per_frame = static_cast<int>(it->second.number_value(ui_.omni_search_nodes_per_frame));
      }
      if (auto it = obj->find("omni_search_max_results"); it != obj->end()) {
        ui_.omni_search_max_results = static_cast<int>(it->second.number_value(ui_.omni_search_max_results));
      }

      if (auto it = obj->find("entity_inspector_id"); it != obj->end()) {
        ui_.entity_inspector_id = static_cast<std::uint64_t>(it->second.number_value(static_cast<double>(ui_.entity_inspector_id)));
      }
      if (auto it = obj->find("entity_inspector_auto_scan"); it != obj->end()) {
        ui_.entity_inspector_auto_scan = it->second.bool_value(ui_.entity_inspector_auto_scan);
      }
      if (auto it = obj->find("entity_inspector_refresh_sec"); it != obj->end()) {
        ui_.entity_inspector_refresh_sec = static_cast<float>(it->second.number_value(ui_.entity_inspector_refresh_sec));
      }
      if (auto it = obj->find("entity_inspector_nodes_per_frame"); it != obj->end()) {
        ui_.entity_inspector_nodes_per_frame = static_cast<int>(it->second.number_value(ui_.entity_inspector_nodes_per_frame));
      }
      if (auto it = obj->find("entity_inspector_max_refs"); it != obj->end()) {
        ui_.entity_inspector_max_refs = static_cast<int>(it->second.number_value(ui_.entity_inspector_max_refs));
      }

      // Reference Graph preferences.
      if (auto it = obj->find("reference_graph_focus_id"); it != obj->end()) {
        ui_.reference_graph_focus_id = static_cast<std::uint64_t>(it->second.number_value(static_cast<double>(ui_.reference_graph_focus_id)));
      }
      if (auto it = obj->find("reference_graph_show_inbound"); it != obj->end()) {
        ui_.reference_graph_show_inbound = it->second.bool_value(ui_.reference_graph_show_inbound);
      }
      if (auto it = obj->find("reference_graph_show_outbound"); it != obj->end()) {
        ui_.reference_graph_show_outbound = it->second.bool_value(ui_.reference_graph_show_outbound);
      }
      if (auto it = obj->find("reference_graph_strict_id_keys"); it != obj->end()) {
        ui_.reference_graph_strict_id_keys = it->second.bool_value(ui_.reference_graph_strict_id_keys);
      }
      if (auto it = obj->find("reference_graph_auto_layout"); it != obj->end()) {
        ui_.reference_graph_auto_layout = it->second.bool_value(ui_.reference_graph_auto_layout);
      }
      if (auto it = obj->find("reference_graph_refresh_sec"); it != obj->end()) {
        ui_.reference_graph_refresh_sec = static_cast<float>(it->second.number_value(ui_.reference_graph_refresh_sec));
      }
      if (auto it = obj->find("reference_graph_nodes_per_frame"); it != obj->end()) {
        ui_.reference_graph_nodes_per_frame = static_cast<int>(it->second.number_value(ui_.reference_graph_nodes_per_frame));
      }
      if (auto it = obj->find("reference_graph_max_nodes"); it != obj->end()) {
        ui_.reference_graph_max_nodes = static_cast<int>(it->second.number_value(ui_.reference_graph_max_nodes));
      }

      if (auto it = obj->find("reference_graph_global_mode"); it != obj->end()) {
        ui_.reference_graph_global_mode = it->second.bool_value(ui_.reference_graph_global_mode);
      }
      if (auto it = obj->find("reference_graph_entities_per_frame"); it != obj->end()) {
        ui_.reference_graph_entities_per_frame = static_cast<int>(it->second.number_value(ui_.reference_graph_entities_per_frame));
      }
      if (auto it = obj->find("reference_graph_scan_nodes_per_entity"); it != obj->end()) {
        ui_.reference_graph_scan_nodes_per_entity = static_cast<int>(it->second.number_value(ui_.reference_graph_scan_nodes_per_entity));
      }
      if (auto it = obj->find("reference_graph_max_edges"); it != obj->end()) {
        ui_.reference_graph_max_edges = static_cast<int>(it->second.number_value(ui_.reference_graph_max_edges));
      }

      // Time Machine preferences.
      if (auto it = obj->find("time_machine_recording"); it != obj->end()) {
        ui_.time_machine_recording = it->second.bool_value(ui_.time_machine_recording);
      }
      if (auto it = obj->find("time_machine_refresh_sec"); it != obj->end()) {
        ui_.time_machine_refresh_sec = static_cast<float>(it->second.number_value(ui_.time_machine_refresh_sec));
      }
      if (auto it = obj->find("time_machine_keep_snapshots"); it != obj->end()) {
        ui_.time_machine_keep_snapshots = static_cast<int>(it->second.number_value(ui_.time_machine_keep_snapshots));
      }
      if (auto it = obj->find("time_machine_max_changes"); it != obj->end()) {
        ui_.time_machine_max_changes = static_cast<int>(it->second.number_value(ui_.time_machine_max_changes));
      }
      if (auto it = obj->find("time_machine_max_value_chars"); it != obj->end()) {
        ui_.time_machine_max_value_chars = static_cast<int>(it->second.number_value(ui_.time_machine_max_value_chars));
      }
      if (auto it = obj->find("time_machine_storage_mode"); it != obj->end()) {
        ui_.time_machine_storage_mode = static_cast<int>(it->second.number_value(ui_.time_machine_storage_mode));
      }
      if (auto it = obj->find("time_machine_checkpoint_stride"); it != obj->end()) {
        ui_.time_machine_checkpoint_stride = static_cast<int>(it->second.number_value(ui_.time_machine_checkpoint_stride));
      }

      // Compare / Diff preferences.
      if (auto it = obj->find("compare_refresh_sec"); it != obj->end()) {
        ui_.compare_refresh_sec = static_cast<float>(it->second.number_value(ui_.compare_refresh_sec));
      }
      if (auto it = obj->find("compare_include_container_sizes"); it != obj->end()) {
        ui_.compare_include_container_sizes = it->second.bool_value(ui_.compare_include_container_sizes);
      }
      if (auto it = obj->find("compare_show_unchanged"); it != obj->end()) {
        ui_.compare_show_unchanged = it->second.bool_value(ui_.compare_show_unchanged);
      }
      if (auto it = obj->find("compare_case_sensitive"); it != obj->end()) {
        ui_.compare_case_sensitive = it->second.bool_value(ui_.compare_case_sensitive);
      }
      if (auto it = obj->find("compare_max_depth"); it != obj->end()) {
        ui_.compare_max_depth = static_cast<int>(it->second.number_value(ui_.compare_max_depth));
      }
      if (auto it = obj->find("compare_max_nodes"); it != obj->end()) {
        ui_.compare_max_nodes = static_cast<int>(it->second.number_value(ui_.compare_max_nodes));
      }
      if (auto it = obj->find("compare_max_value_chars"); it != obj->end()) {
        ui_.compare_max_value_chars = static_cast<int>(it->second.number_value(ui_.compare_max_value_chars));
      }

      // Watchboard query budgets.
      if (auto it = obj->find("watchboard_query_max_matches"); it != obj->end()) {
        ui_.watchboard_query_max_matches = static_cast<int>(it->second.number_value(ui_.watchboard_query_max_matches));
      }
      if (auto it = obj->find("watchboard_query_max_nodes"); it != obj->end()) {
        ui_.watchboard_query_max_nodes = static_cast<int>(it->second.number_value(ui_.watchboard_query_max_nodes));
      }

      ui_.omni_search_refresh_sec = std::clamp(ui_.omni_search_refresh_sec, 0.10f, 30.0f);
      ui_.omni_search_nodes_per_frame = std::clamp(ui_.omni_search_nodes_per_frame, 50, 500000);
      ui_.omni_search_max_results = std::clamp(ui_.omni_search_max_results, 10, 50000);
      ui_.entity_inspector_refresh_sec = std::clamp(ui_.entity_inspector_refresh_sec, 0.0f, 60.0f);
      ui_.entity_inspector_nodes_per_frame = std::clamp(ui_.entity_inspector_nodes_per_frame, 200, 200000);
      ui_.entity_inspector_max_refs = std::clamp(ui_.entity_inspector_max_refs, 10, 500000);

      ui_.reference_graph_refresh_sec = std::clamp(ui_.reference_graph_refresh_sec, 0.0f, 60.0f);
      ui_.reference_graph_nodes_per_frame = std::clamp(ui_.reference_graph_nodes_per_frame, 50, 200000);
      ui_.reference_graph_max_nodes = std::clamp(ui_.reference_graph_max_nodes, 20, 2000);

      ui_.reference_graph_entities_per_frame = std::clamp(ui_.reference_graph_entities_per_frame, 1, 500);
      ui_.reference_graph_scan_nodes_per_entity = std::clamp(ui_.reference_graph_scan_nodes_per_entity, 500, 500000);
      ui_.reference_graph_max_edges = std::clamp(ui_.reference_graph_max_edges, 50, 500000);

      ui_.time_machine_refresh_sec = std::clamp(ui_.time_machine_refresh_sec, 0.05f, 30.0f);
      ui_.time_machine_keep_snapshots = std::clamp(ui_.time_machine_keep_snapshots, 1, 512);
      ui_.time_machine_max_changes = std::clamp(ui_.time_machine_max_changes, 1, 50000);
      ui_.time_machine_max_value_chars = std::clamp(ui_.time_machine_max_value_chars, 16, 2000);
      ui_.time_machine_storage_mode = std::clamp(ui_.time_machine_storage_mode, 0, 2);
      ui_.time_machine_checkpoint_stride = std::clamp(ui_.time_machine_checkpoint_stride, 1, 128);

      ui_.compare_refresh_sec = std::clamp(ui_.compare_refresh_sec, 0.0f, 60.0f);
      ui_.compare_max_depth = std::clamp(ui_.compare_max_depth, 1, 24);
      ui_.compare_max_nodes = std::clamp(ui_.compare_max_nodes, 50, 200000);
      ui_.compare_max_value_chars = std::clamp(ui_.compare_max_value_chars, 0, 5000);

      ui_.watchboard_query_max_matches = std::clamp(ui_.watchboard_query_max_matches, 10, 500000);
      ui_.watchboard_query_max_nodes = std::clamp(ui_.watchboard_query_max_nodes, 100, 5000000);

      if (!ui_.omni_search_match_keys && !ui_.omni_search_match_values) ui_.omni_search_match_keys = true;
    }

    // Watchboard pins (JSON pointers).
    {
      if (auto it = obj->find("json_watch_items"); it != obj->end()) {
        if (const auto* arr = it->second.as_array()) {
          ui_.json_watch_items.clear();
          std::uint64_t max_id = 0;

          for (const auto& e : *arr) {
            const auto* o = e.as_object();
            if (!o) continue;

            JsonWatchConfig cfg;
            if (auto it2 = o->find("id"); it2 != o->end()) {
              cfg.id = static_cast<std::uint64_t>(it2->second.number_value(0.0));
            }
            if (auto it2 = o->find("label"); it2 != o->end()) {
              cfg.label = it2->second.string_value(cfg.label);
            }
            if (auto it2 = o->find("path"); it2 != o->end()) {
              cfg.path = it2->second.string_value(cfg.path);
            }
            if (auto it2 = o->find("track_history"); it2 != o->end()) {
              cfg.track_history = it2->second.bool_value(cfg.track_history);
            }
            if (auto it2 = o->find("show_sparkline"); it2 != o->end()) {
              cfg.show_sparkline = it2->second.bool_value(cfg.show_sparkline);
            }
            if (auto it2 = o->find("history_len"); it2 != o->end()) {
              cfg.history_len = static_cast<int>(it2->second.number_value(cfg.history_len));
              cfg.history_len = std::clamp(cfg.history_len, 2, 4000);
            }

            if (auto it2 = o->find("is_query"); it2 != o->end()) {
              cfg.is_query = it2->second.bool_value(cfg.is_query);
            }
            if (auto it2 = o->find("query_op"); it2 != o->end()) {
              cfg.query_op = static_cast<int>(it2->second.number_value(cfg.query_op));
              cfg.query_op = std::clamp(cfg.query_op, 0, 4);
            }

            // Alert fields (optional; introduced in ui_prefs v33).
            if (auto it2 = o->find("alert_enabled"); it2 != o->end()) {
              cfg.alert_enabled = it2->second.bool_value(cfg.alert_enabled);
            }
            if (auto it2 = o->find("alert_mode"); it2 != o->end()) {
              cfg.alert_mode = static_cast<int>(it2->second.number_value(cfg.alert_mode));
              cfg.alert_mode = std::clamp(cfg.alert_mode, 0, 4);
            }
            if (auto it2 = o->find("alert_threshold"); it2 != o->end()) {
              cfg.alert_threshold = it2->second.number_value(cfg.alert_threshold);
            }
            if (auto it2 = o->find("alert_delta"); it2 != o->end()) {
              cfg.alert_delta = it2->second.number_value(cfg.alert_delta);
            }
            if (auto it2 = o->find("alert_toast_level"); it2 != o->end()) {
              cfg.alert_toast_level = static_cast<int>(it2->second.number_value(cfg.alert_toast_level));
              cfg.alert_toast_level = std::clamp(cfg.alert_toast_level, 0, 2);
            }
            if (auto it2 = o->find("alert_cooldown_sec"); it2 != o->end()) {
              cfg.alert_cooldown_sec = static_cast<float>(it2->second.number_value(cfg.alert_cooldown_sec));
              cfg.alert_cooldown_sec = std::clamp(cfg.alert_cooldown_sec, 0.0f, 120.0f);
            }

            if (cfg.path.empty()) cfg.path = "/";
            if (!cfg.path.empty() && cfg.path[0] != '/') cfg.path = "/" + cfg.path;

            if (cfg.id == 0) {
              cfg.id = ++max_id;
            } else {
              max_id = std::max(max_id, cfg.id);
            }

            if (cfg.label.empty()) cfg.label = cfg.path;
            ui_.json_watch_items.push_back(std::move(cfg));
          }

          ui_.next_json_watch_id = std::max<std::uint64_t>(ui_.next_json_watch_id, max_id + 1);
        }
      }
    }

    // Data Lenses (procedural tables over JSON arrays).
    {
      if (auto it = obj->find("next_json_table_view_id"); it != obj->end()) {
        ui_.next_json_table_view_id = static_cast<std::uint64_t>(it->second.number_value(static_cast<double>(ui_.next_json_table_view_id)));
      }
      if (auto it = obj->find("json_table_views"); it != obj->end()) {
        if (const auto* arr = it->second.as_array()) {
          ui_.json_table_views.clear();
          std::uint64_t max_id = 0;

          for (const auto& e : *arr) {
            const auto* o = e.as_object();
            if (!o) continue;

            JsonTableViewConfig cfg;
            if (auto it2 = o->find("id"); it2 != o->end()) {
              cfg.id = static_cast<std::uint64_t>(it2->second.number_value(0.0));
            }
            if (auto it2 = o->find("name"); it2 != o->end()) {
              cfg.name = it2->second.string_value(cfg.name);
            }
            if (auto it2 = o->find("array_path"); it2 != o->end()) {
              cfg.array_path = it2->second.string_value(cfg.array_path);
            }
            if (auto it2 = o->find("sample_rows"); it2 != o->end()) {
              cfg.sample_rows = static_cast<int>(it2->second.number_value(cfg.sample_rows));
              cfg.sample_rows = std::clamp(cfg.sample_rows, 1, 4096);
            }
            if (auto it2 = o->find("max_depth"); it2 != o->end()) {
              cfg.max_depth = static_cast<int>(it2->second.number_value(cfg.max_depth));
              cfg.max_depth = std::clamp(cfg.max_depth, 0, 6);
            }
            if (auto it2 = o->find("include_container_sizes"); it2 != o->end()) {
              cfg.include_container_sizes = it2->second.bool_value(cfg.include_container_sizes);
            }
            if (auto it2 = o->find("max_infer_columns"); it2 != o->end()) {
              cfg.max_infer_columns = static_cast<int>(it2->second.number_value(cfg.max_infer_columns));
              cfg.max_infer_columns = std::clamp(cfg.max_infer_columns, 4, 512);
            }
            if (auto it2 = o->find("max_rows"); it2 != o->end()) {
              cfg.max_rows = static_cast<int>(it2->second.number_value(cfg.max_rows));
              cfg.max_rows = std::clamp(cfg.max_rows, 50, 500000);
            }
            if (auto it2 = o->find("filter"); it2 != o->end()) {
              cfg.filter = it2->second.string_value(cfg.filter);
            }
            if (auto it2 = o->find("filter_case_sensitive"); it2 != o->end()) {
              cfg.filter_case_sensitive = it2->second.bool_value(cfg.filter_case_sensitive);
            }
            if (auto it2 = o->find("filter_all_fields"); it2 != o->end()) {
              cfg.filter_all_fields = it2->second.bool_value(cfg.filter_all_fields);
            }

            // Columns
            if (auto it2 = o->find("columns"); it2 != o->end()) {
              if (const auto* ca = it2->second.as_array()) {
                cfg.columns.clear();
                cfg.columns.reserve(ca->size());
                for (const auto& ce : *ca) {
                  const auto* co = ce.as_object();
                  if (!co) continue;
                  JsonTableColumnConfig col;
                  if (auto it3 = co->find("label"); it3 != co->end()) {
                    col.label = it3->second.string_value(col.label);
                  }
                  if (auto it3 = co->find("rel_path"); it3 != co->end()) {
                    col.rel_path = it3->second.string_value(col.rel_path);
                  }
                  if (auto it3 = co->find("enabled"); it3 != co->end()) {
                    col.enabled = it3->second.bool_value(col.enabled);
                  }

                  if (col.rel_path.empty()) col.rel_path = "/";
                  if (!col.rel_path.empty() && col.rel_path[0] != '/') col.rel_path = "/" + col.rel_path;
                  cfg.columns.push_back(std::move(col));
                }
              }
            }

            if (cfg.array_path.empty()) cfg.array_path = "/";
            if (!cfg.array_path.empty() && cfg.array_path[0] != '/') cfg.array_path = "/" + cfg.array_path;
            if (cfg.name.empty()) cfg.name = "Lens";

            if (cfg.id == 0) {
              cfg.id = ++max_id;
            }
            max_id = std::max(max_id, cfg.id);
            ui_.json_table_views.push_back(std::move(cfg));
          }

          ui_.next_json_table_view_id = std::max<std::uint64_t>(ui_.next_json_table_view_id, max_id + 1);
        }
      }
    }



    // Dashboards (procedural widgets over Data Lenses).
    {
      if (auto it = obj->find("next_json_dashboard_id"); it != obj->end()) {
        ui_.next_json_dashboard_id = static_cast<std::uint64_t>(it->second.number_value(static_cast<double>(ui_.next_json_dashboard_id)));
      }
      if (auto it = obj->find("json_dashboards"); it != obj->end()) {
        if (const auto* arr = it->second.as_array()) {
          ui_.json_dashboards.clear();
          std::uint64_t max_id = 0;

          for (const auto& e : *arr) {
            const auto* o = e.as_object();
            if (!o) continue;

            JsonDashboardConfig cfg;
            if (auto it2 = o->find("id"); it2 != o->end()) {
              cfg.id = static_cast<std::uint64_t>(it2->second.number_value(0.0));
            }
            if (auto it2 = o->find("name"); it2 != o->end()) {
              cfg.name = it2->second.string_value(cfg.name);
            }
            if (auto it2 = o->find("table_view_id"); it2 != o->end()) {
              cfg.table_view_id = static_cast<std::uint64_t>(it2->second.number_value(static_cast<double>(cfg.table_view_id)));
            }
            if (auto it2 = o->find("scan_rows"); it2 != o->end()) {
              cfg.scan_rows = static_cast<int>(it2->second.number_value(cfg.scan_rows));
              cfg.scan_rows = std::clamp(cfg.scan_rows, 10, 500000);
            }
            if (auto it2 = o->find("rows_per_frame"); it2 != o->end()) {
              cfg.rows_per_frame = static_cast<int>(it2->second.number_value(cfg.rows_per_frame));
              cfg.rows_per_frame = std::clamp(cfg.rows_per_frame, 10, 20000);
            }
            if (auto it2 = o->find("histogram_bins"); it2 != o->end()) {
              cfg.histogram_bins = static_cast<int>(it2->second.number_value(cfg.histogram_bins));
              cfg.histogram_bins = std::clamp(cfg.histogram_bins, 4, 64);
            }
            if (auto it2 = o->find("max_numeric_charts"); it2 != o->end()) {
              cfg.max_numeric_charts = static_cast<int>(it2->second.number_value(cfg.max_numeric_charts));
              cfg.max_numeric_charts = std::clamp(cfg.max_numeric_charts, 0, 32);
            }
            if (auto it2 = o->find("max_category_cards"); it2 != o->end()) {
              cfg.max_category_cards = static_cast<int>(it2->second.number_value(cfg.max_category_cards));
              cfg.max_category_cards = std::clamp(cfg.max_category_cards, 0, 32);
            }
            if (auto it2 = o->find("top_n"); it2 != o->end()) {
              cfg.top_n = static_cast<int>(it2->second.number_value(cfg.top_n));
              cfg.top_n = std::clamp(cfg.top_n, 1, 100);
            }
            if (auto it2 = o->find("link_to_lens_filter"); it2 != o->end()) {
              cfg.link_to_lens_filter = it2->second.bool_value(cfg.link_to_lens_filter);
            }
            if (auto it2 = o->find("use_all_lens_columns"); it2 != o->end()) {
              cfg.use_all_lens_columns = it2->second.bool_value(cfg.use_all_lens_columns);
            }
            if (auto it2 = o->find("top_rows_rel_path"); it2 != o->end()) {
              cfg.top_rows_rel_path = it2->second.string_value(cfg.top_rows_rel_path);
            }

            if (!cfg.top_rows_rel_path.empty() && cfg.top_rows_rel_path[0] != '/') {
              cfg.top_rows_rel_path = "/" + cfg.top_rows_rel_path;
            }

            if (cfg.name.empty()) cfg.name = "Dashboard";

            if (cfg.id == 0) {
              cfg.id = ++max_id;
            }
            max_id = std::max(max_id, cfg.id);

            if (cfg.table_view_id == 0) continue;
            ui_.json_dashboards.push_back(std::move(cfg));
          }

          ui_.next_json_dashboard_id = std::max<std::uint64_t>(ui_.next_json_dashboard_id, max_id + 1);
        }
      }
    }


    // Pivot Tables (procedural group-by aggregations over Data Lenses).
    {
      if (auto it = obj->find("next_json_pivot_id"); it != obj->end()) {
        ui_.next_json_pivot_id = static_cast<std::uint64_t>(it->second.number_value(static_cast<double>(ui_.next_json_pivot_id)));
      }
      if (auto it = obj->find("json_pivots"); it != obj->end()) {
        if (const auto* arr = it->second.as_array()) {
          ui_.json_pivots.clear();
          std::uint64_t max_id = 0;

          for (const auto& e : *arr) {
            const auto* o = e.as_object();
            if (!o) continue;

            JsonPivotConfig cfg;
            if (auto it2 = o->find("id"); it2 != o->end()) {
              cfg.id = static_cast<std::uint64_t>(it2->second.number_value(0.0));
            }
            if (auto it2 = o->find("name"); it2 != o->end()) {
              cfg.name = it2->second.string_value(cfg.name);
            }
            if (auto it2 = o->find("table_view_id"); it2 != o->end()) {
              cfg.table_view_id = static_cast<std::uint64_t>(it2->second.number_value(static_cast<double>(cfg.table_view_id)));
            }
            if (auto it2 = o->find("scan_rows"); it2 != o->end()) {
              cfg.scan_rows = static_cast<int>(it2->second.number_value(cfg.scan_rows));
              cfg.scan_rows = std::clamp(cfg.scan_rows, 10, 500000);
            }
            if (auto it2 = o->find("rows_per_frame"); it2 != o->end()) {
              cfg.rows_per_frame = static_cast<int>(it2->second.number_value(cfg.rows_per_frame));
              cfg.rows_per_frame = std::clamp(cfg.rows_per_frame, 1, 50000);
            }
            if (auto it2 = o->find("link_to_lens_filter"); it2 != o->end()) {
              cfg.link_to_lens_filter = it2->second.bool_value(cfg.link_to_lens_filter);
            }
            if (auto it2 = o->find("use_all_lens_columns"); it2 != o->end()) {
              cfg.use_all_lens_columns = it2->second.bool_value(cfg.use_all_lens_columns);
            }
            if (auto it2 = o->find("group_by_rel_path"); it2 != o->end()) {
              cfg.group_by_rel_path = it2->second.string_value(cfg.group_by_rel_path);
            }
            if (auto it2 = o->find("value_enabled"); it2 != o->end()) {
              cfg.value_enabled = it2->second.bool_value(cfg.value_enabled);
            }
            if (auto it2 = o->find("value_rel_path"); it2 != o->end()) {
              cfg.value_rel_path = it2->second.string_value(cfg.value_rel_path);
            }
            if (auto it2 = o->find("value_op"); it2 != o->end()) {
              cfg.value_op = static_cast<int>(it2->second.number_value(cfg.value_op));
              cfg.value_op = std::clamp(cfg.value_op, 0, 3);
            }
            if (auto it2 = o->find("top_groups"); it2 != o->end()) {
              cfg.top_groups = static_cast<int>(it2->second.number_value(cfg.top_groups));
              cfg.top_groups = std::clamp(cfg.top_groups, 0, 1000000);
            }

            if (cfg.group_by_rel_path.empty()) cfg.group_by_rel_path = "/";
            if (!cfg.group_by_rel_path.empty() && cfg.group_by_rel_path[0] != '/') {
              cfg.group_by_rel_path = "/" + cfg.group_by_rel_path;
            }
            if (!cfg.value_rel_path.empty() && cfg.value_rel_path[0] != '/') {
              cfg.value_rel_path = "/" + cfg.value_rel_path;
            }

            if (cfg.name.empty()) cfg.name = "Pivot";

            if (cfg.id == 0) {
              cfg.id = ++max_id;
            }
            max_id = std::max(max_id, cfg.id);

            if (cfg.table_view_id == 0) continue;
            ui_.json_pivots.push_back(std::move(cfg));
          }

          ui_.next_json_pivot_id = std::max<std::uint64_t>(ui_.next_json_pivot_id, max_id + 1);
        }
      }

      // --- Procedural UI: Context Forge (selection-following UI Forge panel) ---
      if (auto it = obj->find("context_forge_enabled"); it != obj->end()) {
        ui_.context_forge_enabled = it->second.bool_value(ui_.context_forge_enabled);
      }
      if (auto it = obj->find("context_forge_follow_selection"); it != obj->end()) {
        ui_.context_forge_follow_selection = it->second.bool_value(ui_.context_forge_follow_selection);
      }
      if (auto it = obj->find("context_forge_auto_update"); it != obj->end()) {
        ui_.context_forge_auto_update = it->second.bool_value(ui_.context_forge_auto_update);
      }
      if (auto it = obj->find("context_forge_pinned_entity_id"); it != obj->end()) {
        ui_.context_forge_pinned_entity_id = static_cast<std::uint64_t>(it->second.number_value(static_cast<double>(ui_.context_forge_pinned_entity_id)));
      }
      if (auto it = obj->find("context_forge_seed"); it != obj->end()) {
        ui_.context_forge_seed = static_cast<int>(it->second.number_value(ui_.context_forge_seed));
      }
      if (auto it = obj->find("context_forge_max_kpis"); it != obj->end()) {
        ui_.context_forge_max_kpis = std::clamp(static_cast<int>(it->second.number_value(ui_.context_forge_max_kpis)), 0, 80);
      }
      if (auto it = obj->find("context_forge_max_lists"); it != obj->end()) {
        ui_.context_forge_max_lists = std::clamp(static_cast<int>(it->second.number_value(ui_.context_forge_max_lists)), 0, 40);
      }
      if (auto it = obj->find("context_forge_depth"); it != obj->end()) {
        ui_.context_forge_depth = std::clamp(static_cast<int>(it->second.number_value(ui_.context_forge_depth)), 0, 2);
      }
      if (auto it = obj->find("context_forge_max_array_numeric_keys"); it != obj->end()) {
        ui_.context_forge_max_array_numeric_keys = std::clamp(static_cast<int>(it->second.number_value(ui_.context_forge_max_array_numeric_keys)), 0, 10);
      }
      if (auto it = obj->find("context_forge_include_lists"); it != obj->end()) {
        ui_.context_forge_include_lists = it->second.bool_value(ui_.context_forge_include_lists);
      }
      if (auto it = obj->find("context_forge_include_queries"); it != obj->end()) {
        ui_.context_forge_include_queries = it->second.bool_value(ui_.context_forge_include_queries);
      }
      if (auto it = obj->find("context_forge_include_id_fields"); it != obj->end()) {
        ui_.context_forge_include_id_fields = it->second.bool_value(ui_.context_forge_include_id_fields);
      }
      if (auto it = obj->find("context_forge_open_panel_on_generate"); it != obj->end()) {
        ui_.context_forge_open_panel_on_generate = it->second.bool_value(ui_.context_forge_open_panel_on_generate);
      }
      if (auto it = obj->find("context_forge_panel_id"); it != obj->end()) {
        ui_.context_forge_panel_id = static_cast<std::uint64_t>(it->second.number_value(static_cast<double>(ui_.context_forge_panel_id)));
      }

      // --- Procedural UI: UI Forge (custom panels) ---
      if (auto it = obj->find("next_ui_forge_panel_id"); it != obj->end()) {
        ui_.next_ui_forge_panel_id = static_cast<std::uint64_t>(it->second.number_value(static_cast<double>(ui_.next_ui_forge_panel_id)));
      }
      if (auto it = obj->find("next_ui_forge_widget_id"); it != obj->end()) {
        ui_.next_ui_forge_widget_id = static_cast<std::uint64_t>(it->second.number_value(static_cast<double>(ui_.next_ui_forge_widget_id)));
      }
      if (auto it = obj->find("ui_forge_panels"); it != obj->end()) {
        if (it->second.is_array()) {
          ui_.ui_forge_panels.clear();

          std::uint64_t max_pid = 0;
          std::uint64_t max_wid = 0;

          for (const auto& pv : it->second.array_items()) {
            if (!pv.is_object()) continue;
            UiForgePanelConfig p;
            const auto& po = pv.object_items();

            if (auto jt = po.find("id"); jt != po.end()) p.id = static_cast<std::uint64_t>(jt->second.number_value(static_cast<double>(p.id)));
            if (auto jt = po.find("name"); jt != po.end()) p.name = jt->second.string_value(p.name);
            if (auto jt = po.find("open"); jt != po.end()) p.open = jt->second.bool_value(p.open);
            if (auto jt = po.find("root_path"); jt != po.end()) p.root_path = jt->second.string_value(p.root_path);
            if (auto jt = po.find("desired_columns"); jt != po.end()) p.desired_columns = static_cast<int>(jt->second.number_value(p.desired_columns));
            if (auto jt = po.find("card_width_em"); jt != po.end()) p.card_width_em = static_cast<float>(jt->second.number_value(p.card_width_em));

            // Clamp/normalize.
            if (p.root_path.empty()) p.root_path = "/";
            if (!p.root_path.empty() && p.root_path[0] != '/') p.root_path = "/" + p.root_path;
            p.desired_columns = std::clamp(p.desired_columns, 0, 12);
            p.card_width_em = std::clamp(p.card_width_em, 10.0f, 60.0f);

            if (auto jt = po.find("widgets"); jt != po.end() && jt->second.is_array()) {
              for (const auto& wv : jt->second.array_items()) {
                if (!wv.is_object()) continue;
                UiForgeWidgetConfig w;
                const auto& wo = wv.object_items();

                if (auto kt = wo.find("id"); kt != wo.end()) w.id = static_cast<std::uint64_t>(kt->second.number_value(static_cast<double>(w.id)));
                if (auto kt = wo.find("type"); kt != wo.end()) w.type = static_cast<int>(kt->second.number_value(w.type));
                if (auto kt = wo.find("label"); kt != wo.end()) w.label = kt->second.string_value(w.label);
                if (auto kt = wo.find("path"); kt != wo.end()) w.path = kt->second.string_value(w.path);
                if (auto kt = wo.find("text"); kt != wo.end()) w.text = kt->second.string_value(w.text);

                if (auto kt = wo.find("is_query"); kt != wo.end()) w.is_query = kt->second.bool_value(w.is_query);
                if (auto kt = wo.find("query_op"); kt != wo.end()) w.query_op = static_cast<int>(kt->second.number_value(w.query_op));
                if (auto kt = wo.find("track_history"); kt != wo.end()) w.track_history = kt->second.bool_value(w.track_history);
                if (auto kt = wo.find("show_sparkline"); kt != wo.end()) w.show_sparkline = kt->second.bool_value(w.show_sparkline);
                if (auto kt = wo.find("history_len"); kt != wo.end()) w.history_len = static_cast<int>(kt->second.number_value(w.history_len));
                if (auto kt = wo.find("span"); kt != wo.end()) w.span = static_cast<int>(kt->second.number_value(w.span));
                if (auto kt = wo.find("preview_rows"); kt != wo.end()) w.preview_rows = static_cast<int>(kt->second.number_value(w.preview_rows));

                // Normalize/clamp.
                w.type = std::clamp(w.type, 0, 3);
                w.query_op = std::clamp(w.query_op, 0, 4);
                w.history_len = std::clamp(w.history_len, 2, 4000);
                w.span = std::clamp(w.span, 1, 12);
                w.preview_rows = std::clamp(w.preview_rows, 1, 100);
                if (w.path.empty()) w.path = "/";
                if (!w.path.empty() && w.path[0] != '/') w.path = "/" + w.path;

                max_wid = std::max(max_wid, w.id);
                p.widgets.push_back(std::move(w));
              }
            }

            max_pid = std::max(max_pid, p.id);
            ui_.ui_forge_panels.push_back(std::move(p));
          }

          // Sanitize next ids (prefs might be hand-edited).
          ui_.next_ui_forge_panel_id = std::max(ui_.next_ui_forge_panel_id, max_pid + 1);
          ui_.next_ui_forge_widget_id = std::max(ui_.next_ui_forge_widget_id, max_wid + 1);
        }
      }

      // --- Procedural UI: UI Forge (panel preset library) ---
      if (auto it = obj->find("ui_forge_presets"); it != obj->end()) {
        if (it->second.is_array()) {
          ui_.ui_forge_presets.clear();

          constexpr std::size_t kMaxPresets = 200;
          constexpr std::size_t kMaxDnaLen = 64 * 1024;

          for (const auto& pv : it->second.array_items()) {
            if (ui_.ui_forge_presets.size() >= kMaxPresets) break;
            if (!pv.is_object()) continue;
            const auto& po = pv.object_items();

            UiForgePanelPreset pr;
            if (auto jt = po.find("name"); jt != po.end()) pr.name = jt->second.string_value(pr.name);
            if (auto jt = po.find("dna"); jt != po.end()) pr.dna = jt->second.string_value(pr.dna);

            if (pr.dna.empty()) continue;
            if (pr.dna.size() > kMaxDnaLen) pr.dna.resize(kMaxDnaLen);

            if (pr.name.empty()) pr.name = "Preset " + std::to_string(ui_.ui_forge_presets.size() + 1);
            ui_.ui_forge_presets.push_back(std::move(pr));
          }
        }
      }

    }
    return true;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return false;
  }
}

bool App::save_ui_prefs(const char* path, std::string* error) const {
  try {
    if (!path || path[0] == '\0') {
      if (error) *error = "UI prefs path is empty.";
      return false;
    }

    nebula4x::json::Object o;
    o["version"] = 37.0;

    // Theme.
    o["clear_color"] = color_to_json(ui_.clear_color);
    o["system_map_bg"] = color_to_json(ui_.system_map_bg);
    o["galaxy_map_bg"] = color_to_json(ui_.galaxy_map_bg);
    o["override_window_bg"] = ui_.override_window_bg;
    o["window_bg"] = color_to_json(ui_.window_bg);
    o["autosave_ui_prefs"] = ui_.autosave_ui_prefs;

    // Rolling game autosaves.
    o["autosave_game_enabled"] = ui_.autosave_game_enabled;
    o["autosave_game_interval_hours"] = static_cast<double>(ui_.autosave_game_interval_hours);
    o["autosave_game_keep_files"] = static_cast<double>(ui_.autosave_game_keep_files);
    o["autosave_game_dir"] = std::string(ui_.autosave_game_dir);

    // New Game dialog defaults.
    o["new_game_scenario"] = static_cast<double>(ui_.new_game_scenario);
    o["new_game_random_seed"] = static_cast<double>(ui_.new_game_random_seed);
    o["new_game_random_num_systems"] = static_cast<double>(ui_.new_game_random_num_systems);
    o["new_game_random_galaxy_shape"] = static_cast<double>(ui_.new_game_random_galaxy_shape);
    o["new_game_random_placement_style"] = static_cast<double>(ui_.new_game_random_placement_style);
    o["new_game_random_placement_quality"] = static_cast<double>(ui_.new_game_random_placement_quality);
    o["new_game_random_jump_network_style"] = static_cast<double>(ui_.new_game_random_jump_network_style);
    o["new_game_random_jump_density"] = static_cast<double>(ui_.new_game_random_jump_density);
    o["new_game_random_enable_regions"] = ui_.new_game_random_enable_regions;
    o["new_game_random_num_regions"] = static_cast<double>(ui_.new_game_random_num_regions);
    o["new_game_random_ai_empires"] = static_cast<double>(ui_.new_game_random_ai_empires);
    o["new_game_random_enable_pirates"] = ui_.new_game_random_enable_pirates;
    o["new_game_random_pirate_strength"] = static_cast<double>(ui_.new_game_random_pirate_strength);
    o["new_game_random_enable_independents"] = ui_.new_game_random_enable_independents;
    o["new_game_random_num_independent_outposts"] = static_cast<double>(ui_.new_game_random_num_independent_outposts);
    o["new_game_random_ensure_clear_home"] = ui_.new_game_random_ensure_clear_home;

    // Random galaxy preview options.
    o["new_game_preview_show_jumps"] = ui_.new_game_preview_show_jumps;
    o["new_game_preview_show_labels"] = ui_.new_game_preview_show_labels;
    o["new_game_preview_show_regions"] = ui_.new_game_preview_show_regions;
    o["new_game_preview_show_nebula"] = ui_.new_game_preview_show_nebula;
    o["new_game_preview_color_by_component"] = ui_.new_game_preview_color_by_component;
    o["new_game_preview_show_chokepoints"] = ui_.new_game_preview_show_chokepoints;

    // Seed explorer defaults.
    o["new_game_seed_search_objective"] = static_cast<double>(ui_.new_game_seed_search_objective);
    o["new_game_seed_search_tries"] = static_cast<double>(ui_.new_game_seed_search_tries);
    o["new_game_seed_search_steps_per_frame"] = static_cast<double>(ui_.new_game_seed_search_steps_per_frame);

    // Accessibility / HUD.
    o["ui_scale"] = static_cast<double>(ui_.ui_scale);
    o["ui_scale_style"] = ui_.ui_scale_style;
    o["ui_style_preset"] = static_cast<double>(ui_.ui_style_preset);
    o["ui_density"] = static_cast<double>(ui_.ui_density);

    // Hotkeys (keyboard shortcuts).
    o["hotkeys_enabled"] = ui_.hotkeys_enabled;
    {
      nebula4x::json::Object hk;
      for (const auto& d : hotkey_defs()) {
        const auto it = ui_.hotkey_overrides.find(d.id);
        if (it == ui_.hotkey_overrides.end()) continue;

        std::string s = hotkey_to_string(it->second);
        if (s.empty()) s = "Unbound";
        hk[d.id] = std::move(s);
      }
      if (!hk.empty()) o["hotkeys"] = nebula4x::json::object(std::move(hk));
    }

    // Procedural theme (ui_style_preset = 5).
    o["ui_procedural_theme_seed"] = static_cast<double>(ui_.ui_procedural_theme_seed);
    o["ui_procedural_theme_use_seed_hue"] = ui_.ui_procedural_theme_use_seed_hue;
    o["ui_procedural_theme_hue_deg"] = static_cast<double>(ui_.ui_procedural_theme_hue_deg);
    o["ui_procedural_theme_variant"] = static_cast<double>(ui_.ui_procedural_theme_variant);
    o["ui_procedural_theme_saturation"] = static_cast<double>(ui_.ui_procedural_theme_saturation);
    o["ui_procedural_theme_value"] = static_cast<double>(ui_.ui_procedural_theme_value);
    o["ui_procedural_theme_bg_value"] = static_cast<double>(ui_.ui_procedural_theme_bg_value);
    o["ui_procedural_theme_accent_strength"] = static_cast<double>(ui_.ui_procedural_theme_accent_strength);
    o["ui_procedural_theme_animate_hue"] = ui_.ui_procedural_theme_animate_hue;
    o["ui_procedural_theme_animate_speed_deg_per_sec"] = static_cast<double>(ui_.ui_procedural_theme_animate_speed_deg_per_sec);
    o["ui_procedural_theme_sync_backgrounds"] = ui_.ui_procedural_theme_sync_backgrounds;
    o["show_event_toasts"] = ui_.show_event_toasts;
    o["event_toast_duration_sec"] = static_cast<double>(ui_.event_toast_duration_sec);

    // Notification Center defaults.
    o["notifications_capture_sim_events"] = ui_.notifications_capture_sim_events;
    o["notifications_capture_info_events"] = ui_.notifications_capture_info_events;
    o["notifications_capture_watchboard_alerts"] = ui_.notifications_capture_watchboard_alerts;
    o["notifications_collapse_duplicates"] = ui_.notifications_collapse_duplicates;
    o["notifications_auto_open_on_error"] = ui_.notifications_auto_open_on_error;
    o["notifications_max_entries"] = static_cast<double>(ui_.notifications_max_entries);
    o["notifications_keep_days"] = static_cast<double>(ui_.notifications_keep_days);

    // Navigator defaults.
    o["nav_open_windows_on_jump"] = ui_.nav_open_windows_on_jump;
    o["nav_history_max"] = static_cast<double>(ui_.nav_history_max);

    // Screen reader / narration (accessibility).
    o["screen_reader_enabled"] = ui_.screen_reader_enabled;
    o["screen_reader_speak_focus"] = ui_.screen_reader_speak_focus;
    o["screen_reader_speak_hover"] = ui_.screen_reader_speak_hover;
    o["screen_reader_speak_windows"] = ui_.screen_reader_speak_windows;
    o["screen_reader_speak_toasts"] = ui_.screen_reader_speak_toasts;
    o["screen_reader_speak_selection"] = ui_.screen_reader_speak_selection;
    o["screen_reader_rate"] = static_cast<double>(ui_.screen_reader_rate);
    o["screen_reader_volume"] = static_cast<double>(ui_.screen_reader_volume);
    o["screen_reader_hover_delay_sec"] = static_cast<double>(ui_.screen_reader_hover_delay_sec);

    // Timeline view defaults.
    o["timeline_show_minimap"] = ui_.timeline_show_minimap;
    o["timeline_show_grid"] = ui_.timeline_show_grid;
    o["timeline_show_labels"] = ui_.timeline_show_labels;
    o["timeline_compact_rows"] = ui_.timeline_compact_rows;
    o["timeline_lane_height"] = static_cast<double>(ui_.timeline_lane_height);
    o["timeline_marker_size"] = static_cast<double>(ui_.timeline_marker_size);
    o["timeline_follow_now"] = ui_.timeline_follow_now;

    // Design Studio defaults.
    o["design_studio_show_grid"] = ui_.design_studio_show_grid;
    o["design_studio_show_labels"] = ui_.design_studio_show_labels;
    o["design_studio_show_compare"] = ui_.design_studio_show_compare;
    o["design_studio_show_power_overlay"] = ui_.design_studio_show_power_overlay;
    o["design_studio_show_heat_overlay"] = ui_.design_studio_show_heat_overlay;

    // Intel defaults.
    o["intel_radar_scanline"] = ui_.intel_radar_scanline;
    o["intel_radar_grid"] = ui_.intel_radar_grid;
    o["intel_radar_show_sensors"] = ui_.intel_radar_show_sensors;
    o["intel_radar_sensor_heat"] = ui_.intel_radar_sensor_heat;
    o["intel_radar_show_bodies"] = ui_.intel_radar_show_bodies;
    o["intel_radar_show_jump_points"] = ui_.intel_radar_show_jump_points;
    o["intel_radar_show_friendlies"] = ui_.intel_radar_show_friendlies;
    o["intel_radar_show_hostiles"] = ui_.intel_radar_show_hostiles;
    o["intel_radar_show_contacts"] = ui_.intel_radar_show_contacts;
    o["intel_radar_labels"] = ui_.intel_radar_labels;

    // Diplomacy Graph defaults.
    o["diplomacy_graph_starfield"] = ui_.diplomacy_graph_starfield;
    o["diplomacy_graph_grid"] = ui_.diplomacy_graph_grid;
    o["diplomacy_graph_labels"] = ui_.diplomacy_graph_labels;
    o["diplomacy_graph_arrows"] = ui_.diplomacy_graph_arrows;
    o["diplomacy_graph_dim_nonfocus"] = ui_.diplomacy_graph_dim_nonfocus;
    o["diplomacy_graph_show_hostile"] = ui_.diplomacy_graph_show_hostile;
    o["diplomacy_graph_show_neutral"] = ui_.diplomacy_graph_show_neutral;
    o["diplomacy_graph_show_friendly"] = ui_.diplomacy_graph_show_friendly;
    o["diplomacy_graph_layout"] = static_cast<double>(ui_.diplomacy_graph_layout);

    // Docking behavior.
    o["docking_with_shift"] = ui_.docking_with_shift;
    o["docking_always_tab_bar"] = ui_.docking_always_tab_bar;
    o["docking_transparent_payload"] = ui_.docking_transparent_payload;
    o["viewports_enable"] = ui_.viewports_enable;
    o["viewports_no_taskbar_icon"] = ui_.viewports_no_taskbar_icon;
    o["viewports_no_auto_merge"] = ui_.viewports_no_auto_merge;
    o["viewports_no_decoration"] = ui_.viewports_no_decoration;

    // Popup window management.
    o["window_popup_first_mode"] = ui_.window_popup_first_mode;
    o["window_popup_auto_focus"] = ui_.window_popup_auto_focus;
    o["window_popup_cascade_step_px"] = static_cast<double>(ui_.window_popup_cascade_step_px);
    {
      nebula4x::json::Object overrides;
      for (const auto& [id, mode] : ui_.window_launch_overrides) {
        // Mode: 0=docked, 1=popup.
        if (mode != 0 && mode != 1) continue;
        overrides[id] = static_cast<double>(mode);
      }
      o["window_launch_overrides"] = nebula4x::json::object(std::move(overrides));
    }


    // Dock layout profiles (ImGui ini files).
    o["layout_profiles_dir"] = std::string(ui_.layout_profiles_dir);
    o["layout_profile"] = std::string(ui_.layout_profile);

    // Procedural dock layout synthesizer (DockBuilder presets).
    o["ui_procedural_layout_seed"] = static_cast<double>(ui_.ui_procedural_layout_seed);
    o["ui_procedural_layout_mode"] = static_cast<double>(ui_.ui_procedural_layout_mode);
    o["ui_procedural_layout_variation"] = static_cast<double>(ui_.ui_procedural_layout_variation);
    o["ui_procedural_layout_include_tools"] = ui_.ui_procedural_layout_include_tools;
    o["ui_procedural_layout_include_forge_panels"] = ui_.ui_procedural_layout_include_forge_panels;
    o["ui_procedural_layout_max_forge_panels"] = static_cast<double>(ui_.ui_procedural_layout_max_forge_panels);
    o["ui_procedural_layout_auto_open_windows"] = ui_.ui_procedural_layout_auto_open_windows;
    o["ui_procedural_layout_autosave_profile"] = ui_.ui_procedural_layout_autosave_profile;

    // Map rendering chrome.
    o["system_map_starfield"] = ui_.system_map_starfield;
    o["system_map_particle_field"] = ui_.system_map_particle_field;
    o["system_map_grid"] = ui_.system_map_grid;
    o["system_map_order_paths"] = ui_.system_map_order_paths;
    o["system_map_fleet_formation_preview"] = ui_.system_map_fleet_formation_preview;
    o["system_map_missile_salvos"] = ui_.system_map_missile_salvos;
    o["system_map_follow_selected"] = ui_.system_map_follow_selected;
    o["system_map_show_minimap"] = ui_.system_map_show_minimap;
    o["system_map_time_preview"] = ui_.system_map_time_preview;
    o["system_map_time_preview_days"] = static_cast<double>(ui_.system_map_time_preview_days);
    o["system_map_time_preview_vectors"] = ui_.system_map_time_preview_vectors;
    o["system_map_time_preview_all_ships"] = ui_.system_map_time_preview_all_ships;
    o["system_map_time_preview_trails"] = ui_.system_map_time_preview_trails;
    o["system_map_sensor_heatmap"] = ui_.system_map_sensor_heatmap;
    o["system_map_threat_heatmap"] = ui_.system_map_threat_heatmap;
    o["system_map_heatmap_opacity"] = static_cast<double>(ui_.system_map_heatmap_opacity);
    o["system_map_heatmap_resolution"] = static_cast<double>(ui_.system_map_heatmap_resolution);

    // Experimental: LOS ray-traced sensor heatmap (UI-only).
    o["system_map_sensor_heatmap_raytrace"] = ui_.system_map_sensor_heatmap_raytrace;
    o["system_map_sensor_raytrace_max_depth"] = static_cast<double>(ui_.system_map_sensor_raytrace_max_depth);
    o["system_map_sensor_raytrace_error_threshold"] = static_cast<double>(ui_.system_map_sensor_raytrace_error_threshold);
    o["system_map_sensor_raytrace_spp"] = static_cast<double>(ui_.system_map_sensor_raytrace_spp);
    o["system_map_sensor_raytrace_los_samples"] = static_cast<double>(ui_.system_map_sensor_raytrace_los_samples);
    o["system_map_sensor_raytrace_los_strength"] = static_cast<double>(ui_.system_map_sensor_raytrace_los_strength);
    o["system_map_sensor_raytrace_debug"] = ui_.system_map_sensor_raytrace_debug;
    o["system_map_nebula_microfield_overlay"] = ui_.system_map_nebula_microfield_overlay;
    o["system_map_nebula_overlay_opacity"] = static_cast<double>(ui_.system_map_nebula_overlay_opacity);
    o["system_map_nebula_overlay_resolution"] = static_cast<double>(ui_.system_map_nebula_overlay_resolution);
    o["system_map_storm_cell_overlay"] = ui_.system_map_storm_cell_overlay;
    o["system_map_storm_overlay_opacity"] = static_cast<double>(ui_.system_map_storm_overlay_opacity);
    o["system_map_storm_overlay_resolution"] = static_cast<double>(ui_.system_map_storm_overlay_resolution);
    o["galaxy_map_starfield"] = ui_.galaxy_map_starfield;
    o["galaxy_map_particle_field"] = ui_.galaxy_map_particle_field;
    o["galaxy_map_grid"] = ui_.galaxy_map_grid;
    o["galaxy_map_selected_route"] = ui_.galaxy_map_selected_route;
    o["galaxy_map_show_minimap"] = ui_.galaxy_map_show_minimap;
    o["galaxy_map_fuel_range"] = ui_.galaxy_map_fuel_range;
    o["map_starfield_density"] = static_cast<double>(ui_.map_starfield_density);
    o["map_starfield_parallax"] = static_cast<double>(ui_.map_starfield_parallax);
    o["map_grid_opacity"] = static_cast<double>(ui_.map_grid_opacity);
    o["map_route_opacity"] = static_cast<double>(ui_.map_route_opacity);

    // Procedural particle field (dust)
    o["map_particle_tile_px"] = static_cast<double>(ui_.map_particle_tile_px);
    o["map_particle_particles_per_tile"] = static_cast<double>(ui_.map_particle_particles_per_tile);
    o["map_particle_layers"] = static_cast<double>(ui_.map_particle_layers);
    o["map_particle_opacity"] = static_cast<double>(ui_.map_particle_opacity);
    o["map_particle_base_radius_px"] = static_cast<double>(ui_.map_particle_base_radius_px);
    o["map_particle_radius_jitter_px"] = static_cast<double>(ui_.map_particle_radius_jitter_px);
    o["map_particle_twinkle_strength"] = static_cast<double>(ui_.map_particle_twinkle_strength);
    o["map_particle_twinkle_speed"] = static_cast<double>(ui_.map_particle_twinkle_speed);
    o["map_particle_drift"] = ui_.map_particle_drift;
    o["map_particle_drift_px_per_day"] = static_cast<double>(ui_.map_particle_drift_px_per_day);
    o["map_particle_layer0_parallax"] = static_cast<double>(ui_.map_particle_layer0_parallax);
    o["map_particle_layer1_parallax"] = static_cast<double>(ui_.map_particle_layer1_parallax);
    o["map_particle_layer2_parallax"] = static_cast<double>(ui_.map_particle_layer2_parallax);
    o["map_particle_sparkles"] = ui_.map_particle_sparkles;
    o["map_particle_sparkle_chance"] = static_cast<double>(ui_.map_particle_sparkle_chance);
    o["map_particle_sparkle_length_px"] = static_cast<double>(ui_.map_particle_sparkle_length_px);
    o["map_particle_debug_tiles"] = ui_.map_particle_debug_tiles;

    // Ray-marched SDF nebula (experimental).
    o["map_raymarch_nebula"] = ui_.map_raymarch_nebula;
    o["map_raymarch_nebula_alpha"] = static_cast<double>(ui_.map_raymarch_nebula_alpha);
    o["map_raymarch_nebula_parallax"] = static_cast<double>(ui_.map_raymarch_nebula_parallax);
    o["map_raymarch_nebula_max_depth"] = static_cast<double>(ui_.map_raymarch_nebula_max_depth);
    o["map_raymarch_nebula_error_threshold"] = static_cast<double>(ui_.map_raymarch_nebula_error_threshold);
    o["map_raymarch_nebula_spp"] = static_cast<double>(ui_.map_raymarch_nebula_spp);
    o["map_raymarch_nebula_max_steps"] = static_cast<double>(ui_.map_raymarch_nebula_max_steps);
    o["map_raymarch_nebula_animate"] = ui_.map_raymarch_nebula_animate;
    o["map_raymarch_nebula_time_scale"] = static_cast<double>(ui_.map_raymarch_nebula_time_scale);
    o["map_raymarch_nebula_debug"] = ui_.map_raymarch_nebula_debug;

    // Procedural background engine (tile raster).
    o["map_proc_render_engine"] = ui_.map_proc_render_engine;
    o["map_proc_render_tile_px"] = static_cast<double>(ui_.map_proc_render_tile_px);
    o["map_proc_render_cache_tiles"] = static_cast<double>(ui_.map_proc_render_cache_tiles);
    o["map_proc_render_nebula_enable"] = ui_.map_proc_render_nebula_enable;
    o["map_proc_render_nebula_strength"] = static_cast<double>(ui_.map_proc_render_nebula_strength);
    o["map_proc_render_nebula_scale"] = static_cast<double>(ui_.map_proc_render_nebula_scale);
    o["map_proc_render_nebula_warp"] = static_cast<double>(ui_.map_proc_render_nebula_warp);
    o["map_proc_render_debug_tiles"] = ui_.map_proc_render_debug_tiles;

    // Procedural body sprites (system map).
    o["system_map_body_sprites"] = ui_.system_map_body_sprites;
    o["system_map_body_sprite_px"] = static_cast<double>(ui_.system_map_body_sprite_px);
    o["system_map_body_sprite_cache"] = static_cast<double>(ui_.system_map_body_sprite_cache);
    o["system_map_body_sprite_light_steps"] = static_cast<double>(ui_.system_map_body_sprite_light_steps);
    o["system_map_body_sprite_rings"] = ui_.system_map_body_sprite_rings;
    o["system_map_body_sprite_ring_chance"] = static_cast<double>(ui_.system_map_body_sprite_ring_chance);
    o["system_map_body_sprite_ambient"] = static_cast<double>(ui_.system_map_body_sprite_ambient);
    o["system_map_body_sprite_diffuse"] = static_cast<double>(ui_.system_map_body_sprite_diffuse);
    o["system_map_body_sprite_specular"] = static_cast<double>(ui_.system_map_body_sprite_specular);
    o["system_map_body_sprite_specular_power"] = static_cast<double>(ui_.system_map_body_sprite_specular_power);

    // Procedural contact icons (system map).
    o["system_map_contact_icons"] = ui_.system_map_contact_icons;
    o["system_map_contact_icon_px"] = static_cast<double>(ui_.system_map_contact_icon_px);
    o["system_map_contact_icon_cache"] = static_cast<double>(ui_.system_map_contact_icon_cache);
    o["system_map_ship_icon_size_px"] = static_cast<double>(ui_.system_map_ship_icon_size_px);
    o["system_map_ship_icon_thrusters"] = ui_.system_map_ship_icon_thrusters;
    o["system_map_ship_icon_thruster_opacity"] = static_cast<double>(ui_.system_map_ship_icon_thruster_opacity);
    o["system_map_ship_icon_thruster_length_px"] = static_cast<double>(ui_.system_map_ship_icon_thruster_length_px);
    o["system_map_ship_icon_thruster_width_px"] = static_cast<double>(ui_.system_map_ship_icon_thruster_width_px);
    o["system_map_missile_icon_size_px"] = static_cast<double>(ui_.system_map_missile_icon_size_px);
    o["system_map_wreck_icon_size_px"] = static_cast<double>(ui_.system_map_wreck_icon_size_px);
    o["system_map_anomaly_icon_size_px"] = static_cast<double>(ui_.system_map_anomaly_icon_size_px);
    o["system_map_anomaly_icon_pulse"] = ui_.system_map_anomaly_icon_pulse;
    o["system_map_contact_icon_debug_bounds"] = ui_.system_map_contact_icon_debug_bounds;

    // Procedural jump-point phenomena (system map).
    o["system_map_jump_phenomena"] = ui_.system_map_jump_phenomena;
    o["system_map_jump_phenomena_reveal_unsurveyed"] = ui_.system_map_jump_phenomena_reveal_unsurveyed;
    o["system_map_jump_phenomena_sprite_px"] = static_cast<double>(ui_.system_map_jump_phenomena_sprite_px);
    o["system_map_jump_phenomena_cache"] = static_cast<double>(ui_.system_map_jump_phenomena_cache);
    o["system_map_jump_phenomena_size_mult"] = static_cast<double>(ui_.system_map_jump_phenomena_size_mult);
    o["system_map_jump_phenomena_opacity"] = static_cast<double>(ui_.system_map_jump_phenomena_opacity);
    o["system_map_jump_phenomena_animate"] = ui_.system_map_jump_phenomena_animate;
    o["system_map_jump_phenomena_anim_speed_cycles_per_day"] =
        static_cast<double>(ui_.system_map_jump_phenomena_anim_speed_cycles_per_day);
    o["system_map_jump_phenomena_pulse"] = ui_.system_map_jump_phenomena_pulse;
    o["system_map_jump_phenomena_pulse_cycles_per_day"] = static_cast<double>(ui_.system_map_jump_phenomena_pulse_cycles_per_day);
    o["system_map_jump_phenomena_filaments"] = ui_.system_map_jump_phenomena_filaments;
    o["system_map_jump_phenomena_filaments_max"] = static_cast<double>(ui_.system_map_jump_phenomena_filaments_max);
    o["system_map_jump_phenomena_filament_strength"] = static_cast<double>(ui_.system_map_jump_phenomena_filament_strength);
    o["system_map_jump_phenomena_debug_bounds"] = ui_.system_map_jump_phenomena_debug_bounds;

    // Procedural anomaly phenomena (system map).
    o["system_map_anomaly_phenomena"] = ui_.system_map_anomaly_phenomena;
    o["system_map_anomaly_phenomena_sprite_px"] = static_cast<double>(ui_.system_map_anomaly_phenomena_sprite_px);
    o["system_map_anomaly_phenomena_cache"] = static_cast<double>(ui_.system_map_anomaly_phenomena_cache);
    o["system_map_anomaly_phenomena_size_mult"] = static_cast<double>(ui_.system_map_anomaly_phenomena_size_mult);
    o["system_map_anomaly_phenomena_opacity"] = static_cast<double>(ui_.system_map_anomaly_phenomena_opacity);
    o["system_map_anomaly_phenomena_animate"] = ui_.system_map_anomaly_phenomena_animate;
    o["system_map_anomaly_phenomena_anim_speed_cycles_per_day"] =
        static_cast<double>(ui_.system_map_anomaly_phenomena_anim_speed_cycles_per_day);
    o["system_map_anomaly_phenomena_pulse"] = ui_.system_map_anomaly_phenomena_pulse;
    o["system_map_anomaly_phenomena_pulse_cycles_per_day"] =
        static_cast<double>(ui_.system_map_anomaly_phenomena_pulse_cycles_per_day);
    o["system_map_anomaly_phenomena_filaments"] = ui_.system_map_anomaly_phenomena_filaments;
    o["system_map_anomaly_phenomena_filaments_max"] = static_cast<double>(ui_.system_map_anomaly_phenomena_filaments_max);
    o["system_map_anomaly_phenomena_filament_strength"] =
        static_cast<double>(ui_.system_map_anomaly_phenomena_filament_strength);
    o["system_map_anomaly_phenomena_glyph_overlay"] = ui_.system_map_anomaly_phenomena_glyph_overlay;
    o["system_map_anomaly_phenomena_glyph_strength"] = static_cast<double>(ui_.system_map_anomaly_phenomena_glyph_strength);
    o["system_map_anomaly_phenomena_debug_bounds"] = ui_.system_map_anomaly_phenomena_debug_bounds;

    // Procedural motion trails (system map).
    o["system_map_motion_trails"] = ui_.system_map_motion_trails;
    o["system_map_motion_trails_all_ships"] = ui_.system_map_motion_trails_all_ships;
    o["system_map_motion_trails_missiles"] = ui_.system_map_motion_trails_missiles;
    o["system_map_motion_trails_max_age_days"] = static_cast<double>(ui_.system_map_motion_trails_max_age_days);
    o["system_map_motion_trails_sample_hours"] = static_cast<double>(ui_.system_map_motion_trails_sample_hours);
    o["system_map_motion_trails_min_seg_px"] = static_cast<double>(ui_.system_map_motion_trails_min_seg_px);
    o["system_map_motion_trails_thickness_px"] = static_cast<double>(ui_.system_map_motion_trails_thickness_px);
    o["system_map_motion_trails_alpha"] = static_cast<double>(ui_.system_map_motion_trails_alpha);
    o["system_map_motion_trails_speed_brighten"] = ui_.system_map_motion_trails_speed_brighten;

    // Procedural flow field (space weather).
    o["system_map_flow_field_overlay"] = ui_.system_map_flow_field_overlay;
    o["system_map_flow_field_animate"] = ui_.system_map_flow_field_animate;
    o["system_map_flow_field_mask_nebula"] = ui_.system_map_flow_field_mask_nebula;
    o["system_map_flow_field_mask_storms"] = ui_.system_map_flow_field_mask_storms;
    o["system_map_flow_field_debug_tiles"] = ui_.system_map_flow_field_debug_tiles;
    o["system_map_flow_field_opacity"] = static_cast<double>(ui_.system_map_flow_field_opacity);
    o["system_map_flow_field_thickness_px"] = static_cast<double>(ui_.system_map_flow_field_thickness_px);
    o["system_map_flow_field_step_px"] = static_cast<double>(ui_.system_map_flow_field_step_px);
    o["system_map_flow_field_highlight_wavelength_px"] =
        static_cast<double>(ui_.system_map_flow_field_highlight_wavelength_px);
    o["system_map_flow_field_animate_speed_cycles_per_day"] =
        static_cast<double>(ui_.system_map_flow_field_animate_speed_cycles_per_day);
    o["system_map_flow_field_nebula_threshold"] = static_cast<double>(ui_.system_map_flow_field_nebula_threshold);
    o["system_map_flow_field_storm_threshold"] = static_cast<double>(ui_.system_map_flow_field_storm_threshold);
    o["system_map_flow_field_scale_mkm"] = static_cast<double>(ui_.system_map_flow_field_scale_mkm);
    o["system_map_flow_field_tile_px"] = static_cast<double>(ui_.system_map_flow_field_tile_px);
    o["system_map_flow_field_cache_tiles"] = static_cast<double>(ui_.system_map_flow_field_cache_tiles);
    o["system_map_flow_field_lines_per_tile"] = static_cast<double>(ui_.system_map_flow_field_lines_per_tile);
    o["system_map_flow_field_steps_per_line"] = static_cast<double>(ui_.system_map_flow_field_steps_per_line);

    // Procedural gravity contours (system map).
    o["system_map_gravity_contours_overlay"] = ui_.system_map_gravity_contours_overlay;
    o["system_map_gravity_contours_debug_tiles"] = ui_.system_map_gravity_contours_debug_tiles;
    o["system_map_gravity_contours_opacity"] = static_cast<double>(ui_.system_map_gravity_contours_opacity);
    o["system_map_gravity_contours_thickness_px"] = static_cast<double>(ui_.system_map_gravity_contours_thickness_px);
    o["system_map_gravity_contours_tile_px"] = static_cast<double>(ui_.system_map_gravity_contours_tile_px);
    o["system_map_gravity_contours_cache_tiles"] = static_cast<double>(ui_.system_map_gravity_contours_cache_tiles);
    o["system_map_gravity_contours_samples_per_tile"] =
        static_cast<double>(ui_.system_map_gravity_contours_samples_per_tile);
    o["system_map_gravity_contours_levels"] = static_cast<double>(ui_.system_map_gravity_contours_levels);
    o["system_map_gravity_contours_level_spacing_decades"] =
        static_cast<double>(ui_.system_map_gravity_contours_level_spacing_decades);
    o["system_map_gravity_contours_level_offset_decades"] =
        static_cast<double>(ui_.system_map_gravity_contours_level_offset_decades);
    o["system_map_gravity_contours_softening_min_mkm"] =
        static_cast<double>(ui_.system_map_gravity_contours_softening_min_mkm);
    o["system_map_gravity_contours_softening_radius_mult"] =
        static_cast<double>(ui_.system_map_gravity_contours_softening_radius_mult);

    // Combat / tactical overlays.
    o["show_selected_weapon_range"] = ui_.show_selected_weapon_range;
    o["show_fleet_weapon_ranges"] = ui_.show_fleet_weapon_ranges;
    o["show_hostile_weapon_ranges"] = ui_.show_hostile_weapon_ranges;

    // Map intel/exploration toggles.
    o["show_selected_sensor_range"] = ui_.show_selected_sensor_range;
    o["show_faction_sensor_coverage"] = ui_.show_faction_sensor_coverage;
    o["faction_sensor_coverage_fill"] = ui_.faction_sensor_coverage_fill;
    o["faction_sensor_coverage_signature"] = static_cast<double>(ui_.faction_sensor_coverage_signature);
    o["faction_sensor_coverage_max_sources"] = static_cast<double>(ui_.faction_sensor_coverage_max_sources);
    o["show_contact_markers"] = ui_.show_contact_markers;
    o["show_contact_labels"] = ui_.show_contact_labels;
    o["show_contact_uncertainty"] = ui_.show_contact_uncertainty;
    o["show_minor_bodies"] = ui_.show_minor_bodies;
    o["show_minor_body_labels"] = ui_.show_minor_body_labels;
    o["show_galaxy_labels"] = ui_.show_galaxy_labels;
    o["show_galaxy_pins"] = ui_.show_galaxy_pins;
    o["show_galaxy_jump_lines"] = ui_.show_galaxy_jump_lines;
    o["show_galaxy_unknown_exits"] = ui_.show_galaxy_unknown_exits;
    o["show_galaxy_intel_alerts"] = ui_.show_galaxy_intel_alerts;
    o["show_galaxy_freight_lanes"] = ui_.show_galaxy_freight_lanes;
    o["show_galaxy_trade_lanes"] = ui_.show_galaxy_trade_lanes;
    o["show_galaxy_trade_hubs"] = ui_.show_galaxy_trade_hubs;

    o["galaxy_trade_good_filter"] = static_cast<double>(ui_.galaxy_trade_good_filter);
    o["galaxy_trade_filter_include_secondary"] = ui_.galaxy_trade_filter_include_secondary;
    o["galaxy_trade_min_lane_volume"] = static_cast<double>(ui_.galaxy_trade_min_lane_volume);
    o["galaxy_trade_risk_overlay"] = ui_.galaxy_trade_risk_overlay;
    o["galaxy_trade_security_panel"] = ui_.galaxy_trade_security_panel;
    o["galaxy_trade_security_top_n"] = static_cast<double>(ui_.galaxy_trade_security_top_n);
    o["show_galaxy_fleet_missions"] = ui_.show_galaxy_fleet_missions;
    o["galaxy_fleet_mission_alpha"] = static_cast<double>(ui_.galaxy_fleet_mission_alpha);
    o["show_galaxy_chokepoints"] = ui_.show_galaxy_chokepoints;
    o["show_galaxy_regions"] = ui_.show_galaxy_regions;
    o["show_galaxy_region_labels"] = ui_.show_galaxy_region_labels;
    o["show_galaxy_region_boundaries"] = ui_.show_galaxy_region_boundaries;
    o["galaxy_region_dim_nonselected"] = ui_.galaxy_region_dim_nonselected;
    o["galaxy_region_boundary_voronoi"] = ui_.galaxy_region_boundary_voronoi;
    o["show_galaxy_region_centers"] = ui_.show_galaxy_region_centers;
    o["show_galaxy_region_border_links"] = ui_.show_galaxy_region_border_links;
    o["galaxy_map_territory_overlay"] = ui_.galaxy_map_territory_overlay;
    o["galaxy_map_territory_fill"] = ui_.galaxy_map_territory_fill;
    o["galaxy_map_territory_boundaries"] = ui_.galaxy_map_territory_boundaries;
    o["galaxy_map_territory_fill_opacity"] = ui_.galaxy_map_territory_fill_opacity;
    o["galaxy_map_territory_boundary_opacity"] = ui_.galaxy_map_territory_boundary_opacity;
    o["galaxy_map_territory_boundary_thickness_px"] = ui_.galaxy_map_territory_boundary_thickness_px;
    o["galaxy_map_territory_tile_px"] = static_cast<double>(ui_.galaxy_map_territory_tile_px);
    o["galaxy_map_territory_cache_tiles"] = static_cast<double>(ui_.galaxy_map_territory_cache_tiles);
    o["galaxy_map_territory_samples_per_tile"] = static_cast<double>(ui_.galaxy_map_territory_samples_per_tile);
    o["galaxy_map_territory_influence_base_spacing_mult"] = ui_.galaxy_map_territory_influence_base_spacing_mult;
    o["galaxy_map_territory_influence_pop_spacing_mult"] = ui_.galaxy_map_territory_influence_pop_spacing_mult;
    o["galaxy_map_territory_influence_pop_log_bias"] = ui_.galaxy_map_territory_influence_pop_log_bias;
    o["galaxy_map_territory_presence_falloff_spacing"] = ui_.galaxy_map_territory_presence_falloff_spacing;
    o["galaxy_map_territory_dominance_softness_spacing"] = ui_.galaxy_map_territory_dominance_softness_spacing;
    o["galaxy_map_territory_contested_dither"] = ui_.galaxy_map_territory_contested_dither;
    o["galaxy_map_territory_contested_threshold"] = ui_.galaxy_map_territory_contested_threshold;
    o["galaxy_map_territory_contested_dither_strength"] = ui_.galaxy_map_territory_contested_dither_strength;
    o["galaxy_map_territory_debug_tiles"] = ui_.galaxy_map_territory_debug_tiles;
    o["galaxy_procgen_lens_mode"] = static_cast<double>(static_cast<int>(ui_.galaxy_procgen_lens_mode));
    o["galaxy_procgen_lens_alpha"] = static_cast<double>(ui_.galaxy_procgen_lens_alpha);
    o["galaxy_procgen_lens_show_legend"] = ui_.galaxy_procgen_lens_show_legend;
    o["galaxy_procgen_lens_log_scale"] = ui_.galaxy_procgen_lens_log_scale;
    o["galaxy_procgen_field"] = ui_.galaxy_procgen_field;
    o["galaxy_procgen_field_alpha"] = static_cast<double>(ui_.galaxy_procgen_field_alpha);
    o["galaxy_procgen_field_cell_px"] = static_cast<double>(ui_.galaxy_procgen_field_cell_px);
    o["galaxy_procgen_contours"] = ui_.galaxy_procgen_contours;
    o["galaxy_procgen_contour_alpha"] = static_cast<double>(ui_.galaxy_procgen_contour_alpha);
    o["galaxy_procgen_contour_cell_px"] = static_cast<double>(ui_.galaxy_procgen_contour_cell_px);
    o["galaxy_procgen_contour_levels"] = static_cast<double>(ui_.galaxy_procgen_contour_levels);
    o["galaxy_procgen_contour_thickness"] = static_cast<double>(ui_.galaxy_procgen_contour_thickness);
    o["galaxy_procgen_vectors"] = ui_.galaxy_procgen_vectors;
    o["galaxy_procgen_vector_alpha"] = static_cast<double>(ui_.galaxy_procgen_vector_alpha);
    o["galaxy_procgen_vector_cell_px"] = static_cast<double>(ui_.galaxy_procgen_vector_cell_px);
    o["galaxy_procgen_vector_scale"] = static_cast<double>(ui_.galaxy_procgen_vector_scale);
    o["galaxy_procgen_vector_min_mag"] = static_cast<double>(ui_.galaxy_procgen_vector_min_mag);
    o["galaxy_procgen_probe"] = ui_.galaxy_procgen_probe;

    // Star Atlas overlay.
    o["galaxy_star_atlas_constellations"] = ui_.galaxy_star_atlas_constellations;
    o["galaxy_star_atlas_labels"] = ui_.galaxy_star_atlas_labels;
    o["galaxy_star_atlas_alpha"] = static_cast<double>(ui_.galaxy_star_atlas_alpha);
    o["galaxy_star_atlas_label_alpha"] = static_cast<double>(ui_.galaxy_star_atlas_label_alpha);
    o["galaxy_star_atlas_target_cluster_size"] = static_cast<double>(ui_.galaxy_star_atlas_target_cluster_size);
    o["galaxy_star_atlas_max_constellations"] = static_cast<double>(ui_.galaxy_star_atlas_max_constellations);
    o["galaxy_star_atlas_min_zoom"] = static_cast<double>(ui_.galaxy_star_atlas_min_zoom);

    o["contact_max_age_days"] = static_cast<double>(ui_.contact_max_age_days);

    // Layout.
    o["show_controls_window"] = ui_.show_controls_window;
    o["show_map_window"] = ui_.show_map_window;
    o["show_details_window"] = ui_.show_details_window;
    o["show_directory_window"] = ui_.show_directory_window;
    o["show_production_window"] = ui_.show_production_window;
    o["show_economy_window"] = ui_.show_economy_window;
    o["show_planner_window"] = ui_.show_planner_window;
    o["show_regions_window"] = ui_.show_regions_window;
    o["show_freight_window"] = ui_.show_freight_window;
    o["show_mine_window"] = ui_.show_mine_window;
    o["show_fuel_window"] = ui_.show_fuel_window;
    o["show_sustainment_window"] = ui_.show_sustainment_window;
    o["show_repair_planner_window"] = ui_.show_repair_planner_window;
    o["show_maintenance_planner_window"] = ui_.show_maintenance_planner_window;
    o["show_fleet_manager_window"] = ui_.show_fleet_manager_window;
    o["show_troop_window"] = ui_.show_troop_window;
    o["show_colonist_window"] = ui_.show_colonist_window;
    o["show_terraforming_window"] = ui_.show_terraforming_window;
    o["show_time_warp_window"] = ui_.show_time_warp_window;
    o["show_timeline_window"] = ui_.show_timeline_window;
    o["show_notifications_window"] = ui_.show_notifications_window;
    o["show_design_studio_window"] = ui_.show_design_studio_window;
    o["show_balance_lab_window"] = ui_.show_balance_lab_window;
    o["show_battle_forecast_window"] = ui_.show_battle_forecast_window;
    o["show_procgen_atlas_window"] = ui_.show_procgen_atlas_window;
    o["show_star_atlas_window"] = ui_.show_star_atlas_window;
    o["show_intel_window"] = ui_.show_intel_window;
    o["show_intel_notebook_window"] = ui_.show_intel_notebook_window;
    o["show_diplomacy_window"] = ui_.show_diplomacy_window;
    o["show_victory_window"] = ui_.show_victory_window;
    o["show_settings_window"] = ui_.show_settings_window;
    o["show_save_tools_window"] = ui_.show_save_tools_window;
    o["show_time_machine_window"] = ui_.show_time_machine_window;
    o["show_compare_window"] = ui_.show_compare_window;
    o["show_omni_search_window"] = ui_.show_omni_search_window;
    o["show_json_explorer_window"] = ui_.show_json_explorer_window;
	    o["show_content_validation_window"] = ui_.show_content_validation_window;
	    o["show_state_doctor_window"] = ui_.show_state_doctor_window;
	    o["show_trace_viewer_window"] = ui_.show_trace_viewer_window;
    o["show_entity_inspector_window"] = ui_.show_entity_inspector_window;
    o["show_reference_graph_window"] = ui_.show_reference_graph_window;
    o["show_layout_profiles_window"] = ui_.show_layout_profiles_window;
    o["show_watchboard_window"] = ui_.show_watchboard_window;
    o["show_data_lenses_window"] = ui_.show_data_lenses_window;
    o["show_dashboards_window"] = ui_.show_dashboards_window;
    o["show_pivot_tables_window"] = ui_.show_pivot_tables_window;
    o["show_ui_forge_window"] = ui_.show_ui_forge_window;
    o["show_status_bar"] = ui_.show_status_bar;

	    // Trace Viewer (performance profiler) preferences.
	    o["trace_viewer_autostart"] = ui_.trace_viewer_autostart;
	    o["trace_viewer_auto_refresh"] = ui_.trace_viewer_auto_refresh;
	    o["trace_viewer_refresh_sec"] = static_cast<double>(ui_.trace_viewer_refresh_sec);
	    o["trace_viewer_max_events"] = static_cast<double>(ui_.trace_viewer_max_events);
	    o["trace_viewer_follow_tail"] = ui_.trace_viewer_follow_tail;
	    o["trace_viewer_window_ms"] = static_cast<double>(ui_.trace_viewer_window_ms);
	    o["trace_viewer_export_path"] = ui_.trace_viewer_export_path;

    // Command Console preferences.
    // Stored as stable command ids in ui_prefs.json.
    {
      constexpr int kMaxFavorites = 64;
      nebula4x::json::Array a;
      a.reserve(std::min<int>(static_cast<int>(ui_.command_favorites.size()), kMaxFavorites));
      for (const auto& s : ui_.command_favorites) {
        if (static_cast<int>(a.size()) >= kMaxFavorites) break;
        if (s.empty()) continue;
        a.push_back(s.substr(0, 128));
      }
      o["command_favorites"] = nebula4x::json::array(std::move(a));
    }
    {
      constexpr int kMaxRecent = 32;
      nebula4x::json::Array a;
      a.reserve(std::min<int>(static_cast<int>(ui_.command_recent.size()), kMaxRecent));
      for (const auto& s : ui_.command_recent) {
        if (static_cast<int>(a.size()) >= kMaxRecent) break;
        if (s.empty()) continue;
        a.push_back(s.substr(0, 128));
      }
      o["command_recent"] = nebula4x::json::array(std::move(a));
    }

    o["command_recent_limit"] = static_cast<double>(ui_.command_recent_limit);

    // OmniSearch (game JSON global search) preferences.
    o["omni_search_match_keys"] = ui_.omni_search_match_keys;
    o["omni_search_match_values"] = ui_.omni_search_match_values;
    o["omni_search_match_entities"] = ui_.omni_search_match_entities;
    o["omni_search_match_docs"] = ui_.omni_search_match_docs;
    o["omni_search_match_windows"] = ui_.omni_search_match_windows;
    o["omni_search_match_layouts"] = ui_.omni_search_match_layouts;
    o["omni_search_case_sensitive"] = ui_.omni_search_case_sensitive;
    o["omni_search_auto_refresh"] = ui_.omni_search_auto_refresh;
    o["omni_search_refresh_sec"] = static_cast<double>(ui_.omni_search_refresh_sec);
    o["omni_search_nodes_per_frame"] = static_cast<double>(ui_.omni_search_nodes_per_frame);
    o["omni_search_max_results"] = static_cast<double>(ui_.omni_search_max_results);

    // Entity Inspector preferences.
    o["entity_inspector_id"] = static_cast<double>(ui_.entity_inspector_id);
    o["entity_inspector_auto_scan"] = ui_.entity_inspector_auto_scan;
    o["entity_inspector_refresh_sec"] = static_cast<double>(ui_.entity_inspector_refresh_sec);
    o["entity_inspector_nodes_per_frame"] = static_cast<double>(ui_.entity_inspector_nodes_per_frame);
    o["entity_inspector_max_refs"] = static_cast<double>(ui_.entity_inspector_max_refs);

    // Reference Graph preferences.
    o["reference_graph_focus_id"] = static_cast<double>(ui_.reference_graph_focus_id);
    o["reference_graph_show_inbound"] = ui_.reference_graph_show_inbound;
    o["reference_graph_show_outbound"] = ui_.reference_graph_show_outbound;
    o["reference_graph_strict_id_keys"] = ui_.reference_graph_strict_id_keys;
    o["reference_graph_auto_layout"] = ui_.reference_graph_auto_layout;
    o["reference_graph_refresh_sec"] = static_cast<double>(ui_.reference_graph_refresh_sec);
    o["reference_graph_nodes_per_frame"] = static_cast<double>(ui_.reference_graph_nodes_per_frame);
    o["reference_graph_max_nodes"] = static_cast<double>(ui_.reference_graph_max_nodes);
    o["reference_graph_global_mode"] = ui_.reference_graph_global_mode;
    o["reference_graph_entities_per_frame"] = static_cast<double>(ui_.reference_graph_entities_per_frame);
    o["reference_graph_scan_nodes_per_entity"] = static_cast<double>(ui_.reference_graph_scan_nodes_per_entity);
    o["reference_graph_max_edges"] = static_cast<double>(ui_.reference_graph_max_edges);

    // Time Machine preferences.
    o["time_machine_recording"] = ui_.time_machine_recording;
    o["time_machine_refresh_sec"] = static_cast<double>(ui_.time_machine_refresh_sec);
    o["time_machine_keep_snapshots"] = static_cast<double>(ui_.time_machine_keep_snapshots);
    o["time_machine_max_changes"] = static_cast<double>(ui_.time_machine_max_changes);
    o["time_machine_max_value_chars"] = static_cast<double>(ui_.time_machine_max_value_chars);
    o["time_machine_storage_mode"] = static_cast<double>(ui_.time_machine_storage_mode);
    o["time_machine_checkpoint_stride"] = static_cast<double>(ui_.time_machine_checkpoint_stride);

    // Compare / Diff preferences.
    o["compare_refresh_sec"] = static_cast<double>(ui_.compare_refresh_sec);
    o["compare_include_container_sizes"] = ui_.compare_include_container_sizes;
    o["compare_show_unchanged"] = ui_.compare_show_unchanged;
    o["compare_case_sensitive"] = ui_.compare_case_sensitive;
    o["compare_max_depth"] = static_cast<double>(ui_.compare_max_depth);
    o["compare_max_nodes"] = static_cast<double>(ui_.compare_max_nodes);
    o["compare_max_value_chars"] = static_cast<double>(ui_.compare_max_value_chars);

    // Watchboard pins (JSON pointers).
    o["watchboard_query_max_matches"] = static_cast<double>(ui_.watchboard_query_max_matches);
    o["watchboard_query_max_nodes"] = static_cast<double>(ui_.watchboard_query_max_nodes);
    {
      nebula4x::json::Array a;
      a.reserve(ui_.json_watch_items.size());
      for (const auto& w : ui_.json_watch_items) {
        nebula4x::json::Object wo;
        wo["id"] = static_cast<double>(w.id);
        wo["label"] = w.label;
        wo["path"] = w.path;
        wo["track_history"] = w.track_history;
        wo["show_sparkline"] = w.show_sparkline;
        wo["history_len"] = static_cast<double>(w.history_len);
        wo["is_query"] = w.is_query;
        wo["query_op"] = static_cast<double>(w.query_op);
        wo["alert_enabled"] = w.alert_enabled;
        wo["alert_mode"] = static_cast<double>(w.alert_mode);
        wo["alert_threshold"] = w.alert_threshold;
        wo["alert_delta"] = w.alert_delta;
        wo["alert_toast_level"] = static_cast<double>(w.alert_toast_level);
        wo["alert_cooldown_sec"] = static_cast<double>(w.alert_cooldown_sec);
        a.push_back(nebula4x::json::object(std::move(wo)));
      }
      o["json_watch_items"] = nebula4x::json::array(std::move(a));
    }

    // Data Lenses (procedural tables over JSON arrays).
    o["next_json_table_view_id"] = static_cast<double>(ui_.next_json_table_view_id);
    {
      nebula4x::json::Array a;
      a.reserve(ui_.json_table_views.size());
      for (const auto& v : ui_.json_table_views) {
        nebula4x::json::Object vo;
        vo["id"] = static_cast<double>(v.id);
        vo["name"] = v.name;
        vo["array_path"] = v.array_path;
        vo["sample_rows"] = static_cast<double>(v.sample_rows);
        vo["max_depth"] = static_cast<double>(v.max_depth);
        vo["include_container_sizes"] = v.include_container_sizes;
        vo["max_infer_columns"] = static_cast<double>(v.max_infer_columns);
        vo["max_rows"] = static_cast<double>(v.max_rows);
        vo["filter"] = v.filter;
        vo["filter_case_sensitive"] = v.filter_case_sensitive;
        vo["filter_all_fields"] = v.filter_all_fields;

        nebula4x::json::Array ca;
        ca.reserve(v.columns.size());
        for (const auto& c : v.columns) {
          nebula4x::json::Object co;
          co["label"] = c.label;
          co["rel_path"] = c.rel_path;
          co["enabled"] = c.enabled;
          ca.push_back(nebula4x::json::object(std::move(co)));
        }
        vo["columns"] = nebula4x::json::array(std::move(ca));
        a.push_back(nebula4x::json::object(std::move(vo)));
      }
      o["json_table_views"] = nebula4x::json::array(std::move(a));
    }



    // Dashboards (procedural widgets over Data Lenses).
    o["next_json_dashboard_id"] = static_cast<double>(ui_.next_json_dashboard_id);
    {
      nebula4x::json::Array a;
      a.reserve(ui_.json_dashboards.size());
      for (const auto& d : ui_.json_dashboards) {
        nebula4x::json::Object dbo;
        dbo["id"] = static_cast<double>(d.id);
        dbo["name"] = d.name;
        dbo["table_view_id"] = static_cast<double>(d.table_view_id);
        dbo["scan_rows"] = static_cast<double>(d.scan_rows);
        dbo["rows_per_frame"] = static_cast<double>(d.rows_per_frame);
        dbo["histogram_bins"] = static_cast<double>(d.histogram_bins);
        dbo["max_numeric_charts"] = static_cast<double>(d.max_numeric_charts);
        dbo["max_category_cards"] = static_cast<double>(d.max_category_cards);
        dbo["top_n"] = static_cast<double>(d.top_n);
        dbo["link_to_lens_filter"] = d.link_to_lens_filter;
        dbo["use_all_lens_columns"] = d.use_all_lens_columns;
        dbo["top_rows_rel_path"] = d.top_rows_rel_path;
        a.push_back(nebula4x::json::object(std::move(dbo)));
      }
      o["json_dashboards"] = nebula4x::json::array(std::move(a));
    }


    // Pivot Tables (procedural group-by aggregations over Data Lenses).
    o["next_json_pivot_id"] = static_cast<double>(ui_.next_json_pivot_id);
    {
      nebula4x::json::Array a;
      a.reserve(ui_.json_pivots.size());
      for (const auto& p : ui_.json_pivots) {
        nebula4x::json::Object po;
        po["id"] = static_cast<double>(p.id);
        po["name"] = p.name;
        po["table_view_id"] = static_cast<double>(p.table_view_id);
        po["scan_rows"] = static_cast<double>(p.scan_rows);
        po["rows_per_frame"] = static_cast<double>(p.rows_per_frame);
        po["link_to_lens_filter"] = p.link_to_lens_filter;
        po["use_all_lens_columns"] = p.use_all_lens_columns;
        po["group_by_rel_path"] = p.group_by_rel_path;
        po["value_enabled"] = p.value_enabled;
        po["value_rel_path"] = p.value_rel_path;
        po["value_op"] = static_cast<double>(p.value_op);
        po["top_groups"] = static_cast<double>(p.top_groups);
        a.push_back(nebula4x::json::object(std::move(po)));
      }
      o["json_pivots"] = nebula4x::json::array(std::move(a));
    }


    // --- Procedural UI: Context Forge (selection-following UI Forge panel) ---
    o["context_forge_enabled"] = ui_.context_forge_enabled;
    o["context_forge_follow_selection"] = ui_.context_forge_follow_selection;
    o["context_forge_auto_update"] = ui_.context_forge_auto_update;
    o["context_forge_pinned_entity_id"] = static_cast<double>(ui_.context_forge_pinned_entity_id);
    o["context_forge_seed"] = static_cast<double>(ui_.context_forge_seed);
    o["context_forge_max_kpis"] = static_cast<double>(ui_.context_forge_max_kpis);
    o["context_forge_max_lists"] = static_cast<double>(ui_.context_forge_max_lists);
    o["context_forge_depth"] = static_cast<double>(ui_.context_forge_depth);
    o["context_forge_max_array_numeric_keys"] = static_cast<double>(ui_.context_forge_max_array_numeric_keys);
    o["context_forge_include_lists"] = ui_.context_forge_include_lists;
    o["context_forge_include_queries"] = ui_.context_forge_include_queries;
    o["context_forge_include_id_fields"] = ui_.context_forge_include_id_fields;
    o["context_forge_open_panel_on_generate"] = ui_.context_forge_open_panel_on_generate;
    o["context_forge_panel_id"] = static_cast<double>(ui_.context_forge_panel_id);


    // --- Procedural UI: UI Forge (custom panels) ---
    o["next_ui_forge_panel_id"] = static_cast<double>(ui_.next_ui_forge_panel_id);
    o["next_ui_forge_widget_id"] = static_cast<double>(ui_.next_ui_forge_widget_id);
    {
      nebula4x::json::Array pa;
      pa.reserve(ui_.ui_forge_panels.size());
      for (const auto& p : ui_.ui_forge_panels) {
        nebula4x::json::Object po;
        po["id"] = static_cast<double>(p.id);
        po["name"] = p.name;
        po["open"] = p.open;
        po["root_path"] = p.root_path;
        po["desired_columns"] = static_cast<double>(p.desired_columns);
        po["card_width_em"] = static_cast<double>(p.card_width_em);

        nebula4x::json::Array wa;
        wa.reserve(p.widgets.size());
        for (const auto& w : p.widgets) {
          nebula4x::json::Object wo;
          wo["id"] = static_cast<double>(w.id);
          wo["type"] = static_cast<double>(w.type);
          wo["label"] = w.label;
          wo["path"] = w.path;
          wo["text"] = w.text;
          wo["is_query"] = w.is_query;
          wo["query_op"] = static_cast<double>(w.query_op);
          wo["track_history"] = w.track_history;
          wo["show_sparkline"] = w.show_sparkline;
          wo["history_len"] = static_cast<double>(w.history_len);
          wo["span"] = static_cast<double>(w.span);
          wo["preview_rows"] = static_cast<double>(w.preview_rows);
          wa.push_back(nebula4x::json::object(std::move(wo)));
        }
        po["widgets"] = nebula4x::json::array(std::move(wa));

        pa.push_back(nebula4x::json::object(std::move(po)));
      }
      o["ui_forge_panels"] = nebula4x::json::array(std::move(pa));
    }

    // --- Procedural UI: UI Forge (panel preset library) ---
    {
      nebula4x::json::Array a;
      a.reserve(ui_.ui_forge_presets.size());
      for (const auto& p : ui_.ui_forge_presets) {
        nebula4x::json::Object po;
        po["name"] = p.name;
        po["dna"] = p.dna;
        a.push_back(nebula4x::json::object(std::move(po)));
      }
      o["ui_forge_presets"] = nebula4x::json::array(std::move(a));
    }
    const std::string text = nebula4x::json::stringify(nebula4x::json::object(std::move(o)), 2);
    nebula4x::write_text_file(path, text);

    

    return true;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return false;
  }
}

void App::reset_ui_theme_defaults() {
  ui_.clear_color[0] = 0.0f;
  ui_.clear_color[1] = 0.0f;
  ui_.clear_color[2] = 0.0f;
  ui_.clear_color[3] = 1.0f;

  ui_.system_map_bg[0] = 15.0f / 255.0f;
  ui_.system_map_bg[1] = 18.0f / 255.0f;
  ui_.system_map_bg[2] = 22.0f / 255.0f;
  ui_.system_map_bg[3] = 1.0f;

  ui_.galaxy_map_bg[0] = 12.0f / 255.0f;
  ui_.galaxy_map_bg[1] = 14.0f / 255.0f;
  ui_.galaxy_map_bg[2] = 18.0f / 255.0f;
  ui_.galaxy_map_bg[3] = 1.0f;

  ui_.override_window_bg = false;
  ui_.window_bg[0] = 0.10f;
  ui_.window_bg[1] = 0.105f;
  ui_.window_bg[2] = 0.11f;
  ui_.window_bg[3] = 0.94f;

  // Map rendering chrome.
  ui_.system_map_starfield = true;
  ui_.system_map_grid = false;
  ui_.system_map_order_paths = true;
  ui_.system_map_fleet_formation_preview = true;
  ui_.system_map_missile_salvos = false;
  ui_.system_map_follow_selected = false;
  ui_.system_map_show_minimap = true;
  ui_.system_map_time_preview = false;
  ui_.system_map_time_preview_days = 30.0f;
  ui_.system_map_time_preview_vectors = true;
  ui_.system_map_time_preview_all_ships = false;
  ui_.system_map_time_preview_trails = true;
  ui_.system_map_sensor_heatmap = false;
  ui_.system_map_threat_heatmap = false;
  ui_.system_map_heatmap_opacity = 0.35f;
  ui_.system_map_heatmap_resolution = 64;

  // Experimental LOS-shaded sensor heatmap defaults.
  ui_.system_map_sensor_heatmap_raytrace = false;
  ui_.system_map_sensor_raytrace_max_depth = 6;
  ui_.system_map_sensor_raytrace_error_threshold = 0.06f;
  ui_.system_map_sensor_raytrace_spp = 1;
  ui_.system_map_sensor_raytrace_los_samples = 8;
  ui_.system_map_sensor_raytrace_los_strength = 0.85f;
  ui_.system_map_sensor_raytrace_debug = false;
  ui_.system_map_nebula_microfield_overlay = true;
  ui_.system_map_nebula_overlay_opacity = 0.22f;
  ui_.system_map_nebula_overlay_resolution = 84;
  ui_.system_map_storm_cell_overlay = true;
  ui_.system_map_storm_overlay_opacity = 0.18f;
  ui_.system_map_storm_overlay_resolution = 84;
  ui_.system_map_missile_salvos = false;
  ui_.galaxy_map_starfield = true;
  ui_.galaxy_map_grid = false;
  ui_.galaxy_map_selected_route = true;
  ui_.galaxy_map_show_minimap = true;
  ui_.galaxy_map_fuel_range = false;
  ui_.map_starfield_density = 1.0f;
  ui_.map_starfield_parallax = 0.15f;
  ui_.galaxy_map_particle_field = true;
  ui_.system_map_particle_field = true;
  ui_.map_particle_tile_px = 256;
  ui_.map_particle_particles_per_tile = 64;
  ui_.map_particle_layers = 2;
  ui_.map_particle_opacity = 0.22f;
  ui_.map_particle_base_radius_px = 1.0f;
  ui_.map_particle_radius_jitter_px = 1.6f;
  ui_.map_particle_twinkle_strength = 0.55f;
  ui_.map_particle_twinkle_speed = 1.0f;
  ui_.map_particle_drift = true;
  ui_.map_particle_drift_px_per_day = 4.0f;
  ui_.map_particle_layer0_parallax = 0.10f;
  ui_.map_particle_layer1_parallax = 0.28f;
  ui_.map_particle_layer2_parallax = 0.45f;
  ui_.map_particle_sparkles = true;
  ui_.map_particle_sparkle_chance = 0.06f;
  ui_.map_particle_sparkle_length_px = 6.0f;
  ui_.map_particle_debug_tiles = false;
  ui_.map_grid_opacity = 1.0f;
  ui_.map_route_opacity = 1.0f;

  // Ray-marched SDF nebula (experimental).
  ui_.map_raymarch_nebula = false;
  ui_.map_raymarch_nebula_alpha = 0.18f;
  ui_.map_raymarch_nebula_parallax = 0.06f;
  ui_.map_raymarch_nebula_max_depth = 6;
  ui_.map_raymarch_nebula_error_threshold = 0.05f;
  ui_.map_raymarch_nebula_spp = 1;
  ui_.map_raymarch_nebula_max_steps = 48;
  ui_.map_raymarch_nebula_animate = true;
  ui_.map_raymarch_nebula_time_scale = 0.20f;
  ui_.map_raymarch_nebula_debug = false;

  // Procedural background engine (tile raster).
  ui_.map_proc_render_engine = false;
  ui_.map_proc_render_tile_px = 256;
  ui_.map_proc_render_cache_tiles = 96;
  ui_.map_proc_render_nebula_enable = true;
  ui_.map_proc_render_nebula_strength = 0.35f;
  ui_.map_proc_render_nebula_scale = 1.0f;
  ui_.map_proc_render_nebula_warp = 0.70f;
  ui_.map_proc_render_debug_tiles = false;

  // Galaxy procedural territory overlay (political map).
  ui_.galaxy_map_territory_overlay = false;
  ui_.galaxy_map_territory_fill = true;
  ui_.galaxy_map_territory_boundaries = true;
  ui_.galaxy_map_territory_fill_opacity = 0.16f;
  ui_.galaxy_map_territory_boundary_opacity = 0.42f;
  ui_.galaxy_map_territory_boundary_thickness_px = 1.6f;
  ui_.galaxy_map_territory_tile_px = 420;
  ui_.galaxy_map_territory_cache_tiles = 220;
  ui_.galaxy_map_territory_samples_per_tile = 28;
  ui_.galaxy_map_territory_influence_base_spacing_mult = 1.10f;
  ui_.galaxy_map_territory_influence_pop_spacing_mult = 0.28f;
  ui_.galaxy_map_territory_influence_pop_log_bias = 5.0f;
  ui_.galaxy_map_territory_presence_falloff_spacing = 2.0f;
  ui_.galaxy_map_territory_dominance_softness_spacing = 0.65f;
  ui_.galaxy_map_territory_contested_dither = true;
  ui_.galaxy_map_territory_contested_threshold = 0.22f;
  ui_.galaxy_map_territory_contested_dither_strength = 0.55f;
  ui_.galaxy_map_territory_debug_tiles = false;

  // Procedural body sprites (system map).
  ui_.system_map_body_sprites = true;
  ui_.system_map_body_sprite_px = 96;
  ui_.system_map_body_sprite_cache = 384;
  ui_.system_map_body_sprite_light_steps = 32;
  ui_.system_map_body_sprite_rings = true;
  ui_.system_map_body_sprite_ring_chance = 0.25f;
  ui_.system_map_body_sprite_ambient = 0.22f;
  ui_.system_map_body_sprite_diffuse = 1.0f;
  ui_.system_map_body_sprite_specular = 0.35f;
  ui_.system_map_body_sprite_specular_power = 24.0f;

  // Procedural contact icons (system map).
  ui_.system_map_contact_icons = true;
  ui_.system_map_contact_icon_px = 64;
  ui_.system_map_contact_icon_cache = 768;
  ui_.system_map_ship_icon_size_px = 18.0f;
  ui_.system_map_ship_icon_thrusters = true;
  ui_.system_map_ship_icon_thruster_opacity = 0.60f;
  ui_.system_map_ship_icon_thruster_length_px = 14.0f;
  ui_.system_map_ship_icon_thruster_width_px = 7.0f;
  ui_.system_map_missile_icon_size_px = 10.0f;
  ui_.system_map_wreck_icon_size_px = 14.0f;
  ui_.system_map_anomaly_icon_size_px = 16.0f;
  ui_.system_map_anomaly_icon_pulse = true;
  ui_.system_map_contact_icon_debug_bounds = false;

  // Procedural jump-point phenomena (system map).
  ui_.system_map_jump_phenomena = true;
  ui_.system_map_jump_phenomena_reveal_unsurveyed = false;
  ui_.system_map_jump_phenomena_sprite_px = 96;
  ui_.system_map_jump_phenomena_cache = 256;
  ui_.system_map_jump_phenomena_size_mult = 5.6f;
  ui_.system_map_jump_phenomena_opacity = 0.55f;
  ui_.system_map_jump_phenomena_animate = true;
  ui_.system_map_jump_phenomena_anim_speed_cycles_per_day = 0.14f;
  ui_.system_map_jump_phenomena_pulse = true;
  ui_.system_map_jump_phenomena_pulse_cycles_per_day = 0.08f;
  ui_.system_map_jump_phenomena_filaments = true;
  ui_.system_map_jump_phenomena_filaments_max = 6;
  ui_.system_map_jump_phenomena_filament_strength = 1.0f;
  ui_.system_map_jump_phenomena_debug_bounds = false;

  // Procedural motion trails (system map).
  ui_.system_map_motion_trails = false;
  ui_.system_map_motion_trails_all_ships = false;
  ui_.system_map_motion_trails_missiles = false;
  ui_.system_map_motion_trails_max_age_days = 7.0f;
  ui_.system_map_motion_trails_sample_hours = 2.0f;
  ui_.system_map_motion_trails_min_seg_px = 4.0f;
  ui_.system_map_motion_trails_thickness_px = 2.0f;
  ui_.system_map_motion_trails_alpha = 0.55f;
  ui_.system_map_motion_trails_speed_brighten = true;

  // Procedural flow field (space weather).
  ui_.system_map_flow_field_overlay = true;
  ui_.system_map_flow_field_animate = true;
  ui_.system_map_flow_field_mask_nebula = true;
  ui_.system_map_flow_field_mask_storms = false;
  ui_.system_map_flow_field_debug_tiles = false;
  ui_.system_map_flow_field_opacity = 0.35f;
  ui_.system_map_flow_field_thickness_px = 1.25f;
  ui_.system_map_flow_field_step_px = 10.0f;
  ui_.system_map_flow_field_highlight_wavelength_px = 220.0f;
  ui_.system_map_flow_field_animate_speed_cycles_per_day = 0.08f;
  ui_.system_map_flow_field_nebula_threshold = 0.02f;
  ui_.system_map_flow_field_storm_threshold = 0.05f;
  ui_.system_map_flow_field_scale_mkm = 12000.0f;
  ui_.system_map_flow_field_tile_px = 420;
  ui_.system_map_flow_field_cache_tiles = 180;
  ui_.system_map_flow_field_lines_per_tile = 10;
  ui_.system_map_flow_field_steps_per_line = 48;

  // Procedural gravity contours (system map).
  ui_.system_map_gravity_contours_overlay = false;
  ui_.system_map_gravity_contours_debug_tiles = false;
  ui_.system_map_gravity_contours_opacity = 0.22f;
  ui_.system_map_gravity_contours_thickness_px = 1.15f;
  ui_.system_map_gravity_contours_tile_px = 420;
  ui_.system_map_gravity_contours_cache_tiles = 160;
  ui_.system_map_gravity_contours_samples_per_tile = 32;
  ui_.system_map_gravity_contours_levels = 11;
  ui_.system_map_gravity_contours_level_spacing_decades = 0.34f;
  ui_.system_map_gravity_contours_level_offset_decades = 0.0f;
  ui_.system_map_gravity_contours_softening_min_mkm = 0.05f;
  ui_.system_map_gravity_contours_softening_radius_mult = 2.0f;

  ui_.ui_scale = 1.0f;
  ui_.ui_scale_style = true;
  ui_.ui_style_preset = 0;
  ui_.ui_density = 0;

  // Procedural theme defaults.
  ui_.ui_procedural_theme_seed = 1337;
  ui_.ui_procedural_theme_use_seed_hue = true;
  ui_.ui_procedural_theme_hue_deg = 190.0f;
  ui_.ui_procedural_theme_variant = 0;
  ui_.ui_procedural_theme_saturation = 0.72f;
  ui_.ui_procedural_theme_value = 0.90f;
  ui_.ui_procedural_theme_bg_value = 0.11f;
  ui_.ui_procedural_theme_accent_strength = 0.28f;
  ui_.ui_procedural_theme_animate_hue = false;
  ui_.ui_procedural_theme_animate_speed_deg_per_sec = 6.0f;
  ui_.ui_procedural_theme_sync_backgrounds = false;
}

void App::reset_window_layout_defaults() {
  ui_.show_controls_window = true;
  ui_.show_map_window = true;
  ui_.show_details_window = true;
  ui_.show_directory_window = true;
  ui_.show_production_window = false;
  ui_.show_economy_window = false;
  ui_.show_planner_window = false;
  ui_.show_regions_window = false;
  ui_.show_freight_window = false;
  ui_.show_mine_window = false;
  ui_.show_fuel_window = false;
  ui_.show_sustainment_window = false;
  ui_.show_repair_planner_window = false;
  ui_.show_maintenance_planner_window = false;
  ui_.show_fleet_manager_window = false;
  ui_.show_troop_window = false;
  ui_.show_colonist_window = false;
  ui_.show_terraforming_window = false;
  ui_.show_time_warp_window = false;
  ui_.show_timeline_window = false;
  ui_.show_design_studio_window = false;
  ui_.show_balance_lab_window = false;
  ui_.show_battle_forecast_window = false;
  ui_.show_intel_window = false;
  ui_.show_intel_notebook_window = false;
  ui_.show_diplomacy_window = false;
  ui_.show_victory_window = false;
  ui_.show_settings_window = false;
  ui_.show_save_tools_window = false;
  ui_.show_time_machine_window = false;
  ui_.show_compare_window = false;
  ui_.show_omni_search_window = false;
  ui_.show_json_explorer_window = false;
  ui_.show_content_validation_window = false;
  ui_.show_state_doctor_window = false;
	ui_.show_trace_viewer_window = false;
  ui_.show_entity_inspector_window = false;
  ui_.show_reference_graph_window = false;
  ui_.show_layout_profiles_window = false;
  ui_.show_watchboard_window = false;
  ui_.show_data_lenses_window = false;
  ui_.show_dashboards_window = false;
  ui_.show_pivot_tables_window = false;

  ui_.show_ui_forge_window = false;
  for (auto& p : ui_.ui_forge_panels) p.open = false;

  ui_.show_status_bar = true;
  ui_.show_event_toasts = true;
  ui_.event_toast_duration_sec = 6.0f;

  // Docking layout reset: rebuild the default dock layout next frame.
  dock_layout_initialized_ = false;
  // Don't let an existing ini file prevent a reset request from applying.
  dock_layout_checked_ini_ = true;
  dock_layout_has_existing_ini_ = false;

  // Best-effort: clear ImGui's ini settings in memory so window docking/positions
  // don't fight our reset. We guard this in case the function is called when
  // no ImGui context exists (e.g. unit tests / headless).
  if (ImGui::GetCurrentContext() != nullptr) {
    ImGui::LoadIniSettingsFromMemory("");
  }
}

void App::apply_imgui_style_overrides() {
  // This function runs every frame and is responsible for keeping the UI style
  // consistent with the persisted UI preferences (theme preset, density, etc.).
  //
  // Important: Dear ImGui style scaling is multiplicative, so we always rebuild
  // from an unscaled base style to avoid accumulating ScaleAllSizes() calls.
  if (ImGui::GetCurrentContext() == nullptr) return;

  auto clamp_int = [](int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
  };

  // Persisted UI prefs can be edited by hand; keep them within supported ranges.
  ui_.ui_style_preset = clamp_int(ui_.ui_style_preset, 0, 5);
  ui_.ui_density = clamp_int(ui_.ui_density, 0, 2);

  static int last_preset = -1;
  static int last_density = -1;
  static ImGuiStyle base_style;

  auto apply_nebula_overrides = [](ImGuiStyle& s) {
    // Sci-fi friendly "Nebula" preset: starts from ImGui Dark and shifts accent
    // colors toward cyan/teal, with slightly rounder widgets.
    s.WindowRounding = 6.0f;
    s.ChildRounding = 6.0f;
    s.FrameRounding = 4.0f;
    s.PopupRounding = 6.0f;
    s.ScrollbarRounding = 6.0f;
    s.GrabRounding = 4.0f;
    s.TabRounding = 4.0f;

    ImVec4* c = s.Colors;
    const ImVec4 accent(0.00f, 0.78f, 0.90f, 1.00f);

    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = ImVec4(accent.x, accent.y, accent.z, 0.75f);
    c[ImGuiCol_SliderGrabActive] = accent;

    c[ImGuiCol_ButtonHovered] = ImVec4(accent.x, accent.y, accent.z, 0.22f);
    c[ImGuiCol_ButtonActive] = ImVec4(accent.x, accent.y, accent.z, 0.35f);

    c[ImGuiCol_HeaderHovered] = ImVec4(accent.x, accent.y, accent.z, 0.20f);
    c[ImGuiCol_HeaderActive] = ImVec4(accent.x, accent.y, accent.z, 0.30f);

    c[ImGuiCol_SeparatorHovered] = ImVec4(accent.x, accent.y, accent.z, 0.35f);
    c[ImGuiCol_SeparatorActive] = ImVec4(accent.x, accent.y, accent.z, 0.55f);

    c[ImGuiCol_TabHovered] = ImVec4(accent.x, accent.y, accent.z, 0.18f);
    c[ImGuiCol_TabActive] = ImVec4(accent.x, accent.y, accent.z, 0.28f);

    c[ImGuiCol_NavHighlight] = ImVec4(accent.x, accent.y, accent.z, 0.55f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(accent.x, accent.y, accent.z, 0.28f);
    c[ImGuiCol_DockingPreview] = ImVec4(accent.x, accent.y, accent.z, 0.45f);
  };

  auto apply_high_contrast_overrides = [](ImGuiStyle& s) {
    // High-contrast preset: prioritize clarity and selection visibility.
    // This is helpful when streaming/recording or for low-contrast displays.
    s.WindowRounding = 0.0f;
    s.ChildRounding = 0.0f;
    s.FrameRounding = 0.0f;
    s.PopupRounding = 0.0f;
    s.ScrollbarRounding = 0.0f;
    s.GrabRounding = 0.0f;
    s.TabRounding = 0.0f;
    s.FrameBorderSize = 1.0f;
    s.TabBorderSize = 1.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.80f, 0.80f, 0.80f, 1.0f);

    // Bright amber highlight for focus/selection.
    const ImVec4 hi(1.0f, 0.90f, 0.20f, 1.0f);

    c[ImGuiCol_CheckMark] = hi;
    c[ImGuiCol_SliderGrab] = ImVec4(hi.x, hi.y, hi.z, 0.90f);
    c[ImGuiCol_SliderGrabActive] = hi;

    c[ImGuiCol_Header] = ImVec4(hi.x, hi.y, hi.z, 0.25f);
    c[ImGuiCol_HeaderHovered] = ImVec4(hi.x, hi.y, hi.z, 0.35f);
    c[ImGuiCol_HeaderActive] = ImVec4(hi.x, hi.y, hi.z, 0.55f);

    c[ImGuiCol_ButtonHovered] = ImVec4(hi.x, hi.y, hi.z, 0.25f);
    c[ImGuiCol_ButtonActive] = ImVec4(hi.x, hi.y, hi.z, 0.40f);

    c[ImGuiCol_NavHighlight] = ImVec4(hi.x, hi.y, hi.z, 0.75f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(hi.x, hi.y, hi.z, 0.40f);
    c[ImGuiCol_DockingPreview] = ImVec4(hi.x, hi.y, hi.z, 0.55f);

    // Slightly stronger table separators for scanability.
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.18f, 0.18f, 0.18f, 1.0f);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
    c[ImGuiCol_TableBorderLight] = ImVec4(0.35f, 0.35f, 0.35f, 1.0f);
  };

  auto density_scale = [](int density) {
    // 0=Comfortable, 1=Compact, 2=Spacious.
    if (density == 1) return 0.85f;
    if (density == 2) return 1.15f;
    return 1.0f;
  };

  // Rebuild the base style when the preset/density change.
  //
  // For the Procedural preset we rebuild every frame so:
  //  - changes to the procedural knobs are reflected immediately
  //  - optional hue animation can update without additional invalidation plumbing
  const bool force_rebuild = (ui_.ui_style_preset == 5);
  if (force_rebuild || last_preset != ui_.ui_style_preset || last_density != ui_.ui_density) {
    ImGuiStyle s;
    const float time_sec = static_cast<float>(ImGui::GetTime());

    switch (ui_.ui_style_preset) {
      case 0: // Dark (default)
        ImGui::StyleColorsDark(&s);
        break;
      case 1: // Light
        ImGui::StyleColorsLight(&s);
        break;
      case 2: // Classic
        ImGui::StyleColorsClassic(&s);
        break;
      case 3: // Nebula
        ImGui::StyleColorsDark(&s);
        apply_nebula_overrides(s);
        break;
      case 4: // High contrast
        ImGui::StyleColorsDark(&s);
        apply_high_contrast_overrides(s);
        break;
      case 5: { // Procedural
        ImGui::StyleColorsDark(&s);

        ProceduralThemeParams p;
        p.seed = ui_.ui_procedural_theme_seed;
        p.use_seed_hue = ui_.ui_procedural_theme_use_seed_hue;
        p.hue_deg = ui_.ui_procedural_theme_hue_deg;
        p.variant = ui_.ui_procedural_theme_variant;
        p.saturation = ui_.ui_procedural_theme_saturation;
        p.value = ui_.ui_procedural_theme_value;
        p.bg_value = ui_.ui_procedural_theme_bg_value;
        p.accent_strength = ui_.ui_procedural_theme_accent_strength;
        p.animate_hue = ui_.ui_procedural_theme_animate_hue;
        p.animate_speed_deg_per_sec = ui_.ui_procedural_theme_animate_speed_deg_per_sec;
        p.sync_backgrounds = ui_.ui_procedural_theme_sync_backgrounds;

        apply_procedural_theme(s, p, time_sec);

        if (p.sync_backgrounds) {
          const auto pal = compute_procedural_theme_palette(p, time_sec);
          palette_to_float4(pal.clear_color, ui_.clear_color);
          palette_to_float4(pal.system_map_bg, ui_.system_map_bg);
          palette_to_float4(pal.galaxy_map_bg, ui_.galaxy_map_bg);
        }
      } break;
      default:
        ImGui::StyleColorsDark(&s);
        break;
    }

    const float ds = density_scale(ui_.ui_density);
    if (ds != 1.0f) s.ScaleAllSizes(ds);

    base_style = s;
    last_preset = ui_.ui_style_preset;
    last_density = ui_.ui_density;
  }

  // Start from the base style every frame so scaling does not accumulate.
  ImGuiStyle& style = ImGui::GetStyle();
  style = base_style;

  // Optionally scale padding/spacing alongside font scaling.
  if (ui_.ui_scale_style) {
    style.ScaleAllSizes(ui_.ui_scale);
  }

  // Window background override (optional).
  if (ui_.override_window_bg) {
    const ImVec4 c(ui_.window_bg[0], ui_.window_bg[1], ui_.window_bg[2], ui_.window_bg[3]);
    style.Colors[ImGuiCol_WindowBg] = c;
    // Keep child/popup consistent with the override for a cohesive theme.
    style.Colors[ImGuiCol_ChildBg] = c;
    style.Colors[ImGuiCol_PopupBg] = c;
  }

  // When multi-viewports are enabled, detached OS windows can't be rounded the same way
  // as the main viewport. Enforcing a compatible style avoids mismatched corners/alpha.
#ifdef IMGUI_HAS_VIEWPORT
#if NEBULA4X_UI_RENDERER_OPENGL2
  if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
    style.WindowRounding = 0.0f;
    style.Colors[ImGuiCol_WindowBg].w = 1.0f;
  }
#endif
#endif
}

} // namespace nebula4x::ui
