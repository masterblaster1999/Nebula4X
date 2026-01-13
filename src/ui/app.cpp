#include "ui/app.h"

#include <imgui.h>
// DockBuilder* API lives in the internal header (docking branch).
#include <imgui_internal.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "nebula4x/util/json.h"
#include "nebula4x/util/log.h"

#include "nebula4x/core/serialization.h"
#include "nebula4x/util/file_io.h"
#include "ui/panels.h"
#include "ui/new_game_modal.h"
#include "ui/economy_window.h"
#include "ui/planner_window.h"
#include "ui/regions_window.h"
#include "ui/freight_window.h"
#include "ui/fuel_window.h"
#include "ui/salvage_window.h"
#include "ui/sustainment_window.h"
#include "ui/advisor_window.h"
#include "ui/colony_profiles_window.h"
#include "ui/ship_profiles_window.h"
#include "ui/shipyard_targets_window.h"
#include "ui/survey_network_window.h"
#include "ui/time_warp_window.h"
#include "ui/production_window.h"
#include "ui/galaxy_map.h"
#include "ui/system_map.h"
#include "ui/timeline_window.h"
#include "ui/design_studio_window.h"
#include "ui/balance_lab_window.h"
#include "ui/intel_window.h"
#include "ui/victory_window.h"
#include "ui/diplomacy_window.h"
#include "ui/save_tools_window.h"
#include "ui/time_machine_window.h"
#include "ui/omni_search_window.h"
#include "ui/json_explorer_window.h"
#include "ui/content_validation_window.h"
#include "ui/state_doctor_window.h"
#include "ui/entity_inspector_window.h"
#include "ui/reference_graph_window.h"
#include "ui/watchboard_window.h"
#include "ui/data_lenses_window.h"
#include "ui/dashboards_window.h"
#include "ui/pivot_tables_window.h"
#include "ui/layout_profiles.h"
#include "ui/layout_profiles_window.h"

namespace nebula4x::ui {

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

void App::pre_frame() {
  // If there is no ImGui context yet, do nothing.
  if (ImGui::GetCurrentContext() == nullptr) return;

  update_imgui_ini_path_from_ui();

  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = imgui_ini_filename();

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

void App::frame() {
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

    // Reset autosave cadence when the underlying state is replaced.
    autosave_mgr_.reset();
    ui_.last_autosave_game_path.clear();
    ui_.last_autosave_game_error.clear();
  };

  // Apply UI scaling early so every window in this frame uses it.
  {
    ImGuiIO& io = ImGui::GetIO();
    ui_.ui_scale = std::clamp(ui_.ui_scale, 0.65f, 2.5f);
    io.FontGlobalScale = ui_.ui_scale;

    // Docking behavior (persisted via ui_prefs.json).
    io.ConfigDockingWithShift = ui_.docking_with_shift;
    io.ConfigDockingAlwaysTabBar = ui_.docking_always_tab_bar;
    io.ConfigDockingTransparentPayload = ui_.docking_transparent_payload;
  }

  // Apply last-frame style overrides so the menu/settings windows reflect them.
  apply_imgui_style_overrides();

  // --- Global keyboard shortcuts (UI focus) ---
  {
    const ImGuiIO& io = ImGui::GetIO();
    // Avoid stealing shortcuts when the user is typing in an input field.
    if (!io.WantTextInput) {
      // Command palette / help.
      if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_P)) ui_.show_command_palette = true;
      if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F)) ui_.show_omni_search_window = !ui_.show_omni_search_window;
      if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_G)) ui_.show_entity_inspector_window = !ui_.show_entity_inspector_window;
      if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_G)) ui_.show_reference_graph_window = !ui_.show_reference_graph_window;
      if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_D)) ui_.show_time_machine_window = !ui_.show_time_machine_window;
      if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_A)) ui_.show_advisor_window = !ui_.show_advisor_window;
      if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_B)) ui_.show_colony_profiles_window = !ui_.show_colony_profiles_window;
      if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_M)) ui_.show_ship_profiles_window = !ui_.show_ship_profiles_window;
      if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Y)) ui_.show_shipyard_targets_window = !ui_.show_shipyard_targets_window;
      if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_J)) ui_.show_survey_network_window = !ui_.show_survey_network_window;
      if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_R)) ui_.show_regions_window = !ui_.show_regions_window;
      if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_V)) ui_.show_content_validation_window = !ui_.show_content_validation_window;
      if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_K)) ui_.show_state_doctor_window = !ui_.show_state_doctor_window;
      if (ImGui::IsKeyPressed(ImGuiKey_F1)) ui_.show_help_window = !ui_.show_help_window;

      // Quick window toggles.
      if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_1)) ui_.show_controls_window = !ui_.show_controls_window;
      if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_2)) ui_.show_map_window = !ui_.show_map_window;
      if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_3)) ui_.show_details_window = !ui_.show_details_window;
      if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_4)) ui_.show_directory_window = !ui_.show_directory_window;
      if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_5)) ui_.show_economy_window = !ui_.show_economy_window;
      if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_6)) ui_.show_production_window = !ui_.show_production_window;
      if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_7)) ui_.show_timeline_window = !ui_.show_timeline_window;
      if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_8)) ui_.show_design_studio_window = !ui_.show_design_studio_window;
      if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_9)) ui_.show_intel_window = !ui_.show_intel_window;
      if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_0)) ui_.show_diplomacy_window = !ui_.show_diplomacy_window;
      if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Comma)) ui_.show_settings_window = !ui_.show_settings_window;
      if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_L)) {
        ui_.show_layout_profiles_window = !ui_.show_layout_profiles_window;
      }

      // Save/load.
      if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) {
        try {
          nebula4x::write_text_file(save_path_, serialize_game_to_json(sim_.state()));
          nebula4x::log::info("Saved game.");
        } catch (const std::exception& e) {
          nebula4x::log::error(std::string("Save failed: ") + e.what());
        }
      }
      if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O)) {
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
      if (ImGui::IsKeyPressed(ImGuiKey_Space)) {
        const int days = io.KeyCtrl ? 30 : (io.KeyShift ? 5 : 1);
        sim_.advance_days(days);
      }
    }
  }

  UIPrefActions actions;
  draw_main_menu(sim_, ui_, save_path_, load_path_, ui_prefs_path_, actions);
  if (ui_.show_settings_window) {
    draw_settings_window(ui_, ui_prefs_path_, actions);
  }

  // New Game (scenario picker) modal.
  draw_new_game_modal(sim_, ui_);

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

  // Primary workspace windows (dockable).
  if (ui_.show_controls_window) {
    ImGui::SetNextWindowSize(ImVec2(320, 720), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Controls", &ui_.show_controls_window)) {
      draw_left_sidebar(sim_, ui_, selected_ship_, selected_colony_);
    }
    ImGui::End();
  }

  if (ui_.show_map_window) {
    ImGui::SetNextWindowSize(ImVec2(900, 720), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Map", &ui_.show_map_window)) {
      if (ImGui::BeginTabBar("map_tabs")) {
        const MapTab req = ui_.request_map_tab;

        ImGuiTabItemFlags sys_flags = 0;
        ImGuiTabItemFlags gal_flags = 0;
        if (req == MapTab::System) sys_flags |= ImGuiTabItemFlags_SetSelected;
        if (req == MapTab::Galaxy) gal_flags |= ImGuiTabItemFlags_SetSelected;

        if (ImGui::BeginTabItem("System", nullptr, sys_flags)) {
          draw_system_map(sim_, ui_, selected_ship_, selected_colony_, selected_body_, map_zoom_, map_pan_);
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Galaxy", nullptr, gal_flags)) {
          draw_galaxy_map(sim_, ui_, selected_ship_, galaxy_zoom_, galaxy_pan_);
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
    if (ImGui::Begin("Details", &ui_.show_details_window)) {
      draw_right_sidebar(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
    }
    ImGui::End();
  }

  // Optional secondary windows (also dockable).
  if (ui_.show_directory_window) draw_directory_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  if (ui_.show_production_window) draw_production_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  if (ui_.show_economy_window) draw_economy_window(sim_, ui_, selected_colony_, selected_body_);
  if (ui_.show_planner_window) draw_planner_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  if (ui_.show_regions_window) draw_regions_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  if (ui_.show_freight_window) draw_freight_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  if (ui_.show_fuel_window) draw_fuel_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  if (ui_.show_salvage_window) draw_salvage_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  if (ui_.show_sustainment_window)
    draw_sustainment_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  if (ui_.show_advisor_window) draw_advisor_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  if (ui_.show_colony_profiles_window) draw_colony_profiles_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  if (ui_.show_ship_profiles_window) draw_ship_profiles_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  if (ui_.show_shipyard_targets_window) draw_shipyard_targets_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  if (ui_.show_survey_network_window) draw_survey_network_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  if (ui_.show_time_warp_window) draw_time_warp_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  if (ui_.show_timeline_window) draw_timeline_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  if (ui_.show_design_studio_window) {
    draw_design_studio_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_balance_lab_window) {
    draw_balance_lab_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_intel_window) {
    draw_intel_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_diplomacy_window) {
    draw_diplomacy_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_victory_window) {
    draw_victory_window(sim_, ui_);
  }

  if (ui_.show_save_tools_window) draw_save_tools_window(sim_, ui_, save_path_, load_path_);
  if (ui_.show_time_machine_window) {
    draw_time_machine_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_omni_search_window) draw_omni_search_window(sim_, ui_);
  if (ui_.show_json_explorer_window) draw_json_explorer_window(sim_, ui_);
  if (ui_.show_content_validation_window) draw_content_validation_window(sim_, ui_);
  if (ui_.show_state_doctor_window) draw_state_doctor_window(sim_, ui_);
  if (ui_.show_watchboard_window) draw_watchboard_window(sim_, ui_);
  if (ui_.show_data_lenses_window) draw_data_lenses_window(sim_, ui_);
  if (ui_.show_dashboards_window) draw_dashboards_window(sim_, ui_);
  if (ui_.show_pivot_tables_window) draw_pivot_tables_window(sim_, ui_);
  if (ui_.show_entity_inspector_window) draw_entity_inspector_window(sim_, ui_);
  if (ui_.show_reference_graph_window) draw_reference_graph_window(sim_, ui_);
  if (ui_.show_layout_profiles_window) draw_layout_profiles_window(ui_);

  // Help overlay/window.
  draw_help_window(ui_);

  // HUD chrome (status bar, command palette, event toasts).
  draw_status_bar(sim_, ui_, hud_, selected_ship_, selected_colony_, selected_body_, save_path_, load_path_);
  draw_command_palette(sim_, ui_, hud_, selected_ship_, selected_colony_, selected_body_, save_path_, load_path_);

  // Load/new-game can also be triggered via the status bar or command palette.
  // Ensure we react in the same frame (avoids dereferencing stale selections).
  sync_on_state_generation_change();

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
  draw_event_toasts(sim_, ui_, hud_, selected_ship_, selected_colony_, selected_body_);
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

      // UI scale (accessibility). This is a UI preference (not a save-game setting).
      if (auto it = obj->find("ui_scale"); it != obj->end()) {
        ui_.ui_scale = static_cast<float>(it->second.number_value(ui_.ui_scale));
        ui_.ui_scale = std::clamp(ui_.ui_scale, 0.65f, 2.5f);
      }

      // Toast defaults.
      if (auto it = obj->find("show_event_toasts"); it != obj->end()) {
        ui_.show_event_toasts = it->second.bool_value(ui_.show_event_toasts);
      }
      if (auto it = obj->find("event_toast_duration_sec"); it != obj->end()) {
        ui_.event_toast_duration_sec = static_cast<float>(it->second.number_value(ui_.event_toast_duration_sec));
        ui_.event_toast_duration_sec = std::clamp(ui_.event_toast_duration_sec, 0.5f, 60.0f);
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
      if (auto it = obj->find("show_fuel_window"); it != obj->end()) {
        ui_.show_fuel_window = it->second.bool_value(ui_.show_fuel_window);
      }
      if (auto it = obj->find("show_sustainment_window"); it != obj->end()) {
        ui_.show_sustainment_window = it->second.bool_value(ui_.show_sustainment_window);
      }
      if (auto it = obj->find("show_time_warp_window"); it != obj->end()) {
        ui_.show_time_warp_window = it->second.bool_value(ui_.show_time_warp_window);
      }
      if (auto it = obj->find("show_timeline_window"); it != obj->end()) {
        ui_.show_timeline_window = it->second.bool_value(ui_.show_timeline_window);
      }
      if (auto it = obj->find("show_design_studio_window"); it != obj->end()) {
        ui_.show_design_studio_window = it->second.bool_value(ui_.show_design_studio_window);
      }
      if (auto it = obj->find("show_balance_lab_window"); it != obj->end()) {
        ui_.show_balance_lab_window = it->second.bool_value(ui_.show_balance_lab_window);
      }
      if (auto it = obj->find("show_intel_window"); it != obj->end()) {
        ui_.show_intel_window = it->second.bool_value(ui_.show_intel_window);
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
      if (auto it = obj->find("show_status_bar"); it != obj->end()) {
        ui_.show_status_bar = it->second.bool_value(ui_.show_status_bar);
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
      ui_.time_machine_storage_mode = std::clamp(ui_.time_machine_storage_mode, 0, 1);
      ui_.time_machine_checkpoint_stride = std::clamp(ui_.time_machine_checkpoint_stride, 1, 128);

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
    o["version"] = 27.0;

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

    // Accessibility / HUD.
    o["ui_scale"] = static_cast<double>(ui_.ui_scale);
    o["show_event_toasts"] = ui_.show_event_toasts;
    o["event_toast_duration_sec"] = static_cast<double>(ui_.event_toast_duration_sec);

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

    // Dock layout profiles (ImGui ini files).
    o["layout_profiles_dir"] = std::string(ui_.layout_profiles_dir);
    o["layout_profile"] = std::string(ui_.layout_profile);

    // Map rendering chrome.
    o["system_map_starfield"] = ui_.system_map_starfield;
    o["system_map_grid"] = ui_.system_map_grid;
    o["system_map_order_paths"] = ui_.system_map_order_paths;
    o["system_map_fleet_formation_preview"] = ui_.system_map_fleet_formation_preview;
    o["system_map_missile_salvos"] = ui_.system_map_missile_salvos;
    o["system_map_follow_selected"] = ui_.system_map_follow_selected;
    o["system_map_show_minimap"] = ui_.system_map_show_minimap;
    o["galaxy_map_starfield"] = ui_.galaxy_map_starfield;
    o["galaxy_map_grid"] = ui_.galaxy_map_grid;
    o["galaxy_map_selected_route"] = ui_.galaxy_map_selected_route;
    o["galaxy_map_show_minimap"] = ui_.galaxy_map_show_minimap;
    o["map_starfield_density"] = static_cast<double>(ui_.map_starfield_density);
    o["map_starfield_parallax"] = static_cast<double>(ui_.map_starfield_parallax);
    o["map_grid_opacity"] = static_cast<double>(ui_.map_grid_opacity);
    o["map_route_opacity"] = static_cast<double>(ui_.map_route_opacity);

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
    o["show_galaxy_jump_lines"] = ui_.show_galaxy_jump_lines;
    o["show_galaxy_unknown_exits"] = ui_.show_galaxy_unknown_exits;
    o["show_galaxy_intel_alerts"] = ui_.show_galaxy_intel_alerts;
    o["show_galaxy_freight_lanes"] = ui_.show_galaxy_freight_lanes;
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
    o["show_fuel_window"] = ui_.show_fuel_window;
    o["show_sustainment_window"] = ui_.show_sustainment_window;
    o["show_time_warp_window"] = ui_.show_time_warp_window;
    o["show_timeline_window"] = ui_.show_timeline_window;
    o["show_design_studio_window"] = ui_.show_design_studio_window;
    o["show_balance_lab_window"] = ui_.show_balance_lab_window;
    o["show_intel_window"] = ui_.show_intel_window;
    o["show_diplomacy_window"] = ui_.show_diplomacy_window;
    o["show_victory_window"] = ui_.show_victory_window;
    o["show_settings_window"] = ui_.show_settings_window;
    o["show_save_tools_window"] = ui_.show_save_tools_window;
    o["show_time_machine_window"] = ui_.show_time_machine_window;
    o["show_omni_search_window"] = ui_.show_omni_search_window;
    o["show_json_explorer_window"] = ui_.show_json_explorer_window;
    o["show_content_validation_window"] = ui_.show_content_validation_window;
    o["show_state_doctor_window"] = ui_.show_state_doctor_window;
    o["show_entity_inspector_window"] = ui_.show_entity_inspector_window;
    o["show_reference_graph_window"] = ui_.show_reference_graph_window;
    o["show_layout_profiles_window"] = ui_.show_layout_profiles_window;
    o["show_watchboard_window"] = ui_.show_watchboard_window;
    o["show_data_lenses_window"] = ui_.show_data_lenses_window;
    o["show_dashboards_window"] = ui_.show_dashboards_window;
    o["show_pivot_tables_window"] = ui_.show_pivot_tables_window;
    o["show_status_bar"] = ui_.show_status_bar;

    // OmniSearch (game JSON global search) preferences.
    o["omni_search_match_keys"] = ui_.omni_search_match_keys;
    o["omni_search_match_values"] = ui_.omni_search_match_values;
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
  ui_.system_map_missile_salvos = false;
  ui_.galaxy_map_starfield = true;
  ui_.galaxy_map_grid = false;
  ui_.galaxy_map_selected_route = true;
  ui_.galaxy_map_show_minimap = true;
  ui_.map_starfield_density = 1.0f;
  ui_.map_starfield_parallax = 0.15f;
  ui_.map_grid_opacity = 1.0f;
  ui_.map_route_opacity = 1.0f;

  ui_.ui_scale = 1.0f;
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
  ui_.show_fuel_window = false;
  ui_.show_sustainment_window = false;
  ui_.show_time_warp_window = false;
  ui_.show_timeline_window = false;
  ui_.show_design_studio_window = false;
  ui_.show_balance_lab_window = false;
  ui_.show_intel_window = false;
  ui_.show_diplomacy_window = false;
  ui_.show_victory_window = false;
  ui_.show_settings_window = false;
  ui_.show_save_tools_window = false;
  ui_.show_time_machine_window = false;
  ui_.show_omni_search_window = false;
  ui_.show_json_explorer_window = false;
  ui_.show_content_validation_window = false;
  ui_.show_state_doctor_window = false;
  ui_.show_entity_inspector_window = false;
  ui_.show_reference_graph_window = false;
  ui_.show_layout_profiles_window = false;
  ui_.show_watchboard_window = false;
  ui_.show_data_lenses_window = false;
  ui_.show_dashboards_window = false;
  ui_.show_pivot_tables_window = false;

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
  static bool captured = false;
  static ImVec4 base_window_bg;
  static ImVec4 base_child_bg;
  static ImVec4 base_popup_bg;

  ImGuiStyle& style = ImGui::GetStyle();
  if (!captured) {
    base_window_bg = style.Colors[ImGuiCol_WindowBg];
    base_child_bg = style.Colors[ImGuiCol_ChildBg];
    base_popup_bg = style.Colors[ImGuiCol_PopupBg];
    captured = true;
  }

  if (ui_.override_window_bg) {
    const ImVec4 c(ui_.window_bg[0], ui_.window_bg[1], ui_.window_bg[2], ui_.window_bg[3]);
    style.Colors[ImGuiCol_WindowBg] = c;
    // Keep child/popup consistent with the override for a cohesive theme.
    style.Colors[ImGuiCol_ChildBg] = c;
    style.Colors[ImGuiCol_PopupBg] = c;
  } else {
    style.Colors[ImGuiCol_WindowBg] = base_window_bg;
    style.Colors[ImGuiCol_ChildBg] = base_child_bg;
    style.Colors[ImGuiCol_PopupBg] = base_popup_bg;
  }
}

} // namespace nebula4x::ui
