#include "ui/app.h"

#include <imgui.h>
// DockBuilder* API lives in the internal header (docking branch).
#include <imgui_internal.h>

#include <algorithm>
#include <filesystem>

#include "nebula4x/util/json.h"
#include "nebula4x/util/log.h"

#include "nebula4x/core/serialization.h"
#include "nebula4x/util/file_io.h"
#include "ui/panels.h"
#include "ui/economy_window.h"
#include "ui/production_window.h"
#include "ui/galaxy_map.h"
#include "ui/system_map.h"
#include "ui/timeline_window.h"
#include "ui/design_studio_window.h"
#include "ui/intel_window.h"
#include "ui/diplomacy_window.h"

namespace nebula4x::ui {

App::App(Simulation sim) : sim_(std::move(sim)) {
  if (!sim_.state().colonies.empty()) {
    selected_colony_ = sim_.state().colonies.begin()->first;
    if (const auto* c = find_ptr(sim_.state().colonies, selected_colony_)) {
      selected_body_ = c->body_id;
    }
  }

  // Best-effort auto-load of UI preferences (colors/layout).
  std::string err;
  (void)load_ui_prefs(ui_prefs_path_, &err);
}

void App::on_event(const SDL_Event& /*e*/) {
  // Reserved for future (resize, etc.)
}

void App::frame() {
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

  // Handle actions after both the menu and settings window have had a chance
  // to set action flags.
  if (actions.reset_ui_theme) reset_ui_theme_defaults();
  if (actions.reset_window_layout) reset_window_layout_defaults();

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
  if (ui_.show_directory_window) draw_directory_window(sim_, ui_, selected_colony_, selected_body_);
  if (ui_.show_production_window) draw_production_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  if (ui_.show_economy_window) draw_economy_window(sim_, ui_, selected_colony_, selected_body_);
  if (ui_.show_timeline_window) draw_timeline_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  if (ui_.show_design_studio_window) {
    draw_design_studio_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_intel_window) {
    draw_intel_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }
  if (ui_.show_diplomacy_window) {
    draw_diplomacy_window(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
  }

  // Help overlay/window.
  draw_help_window(ui_);

  // HUD chrome (status bar, command palette, event toasts).
  draw_status_bar(sim_, ui_, hud_, selected_ship_, selected_colony_, selected_body_, save_path_, load_path_);
  draw_command_palette(sim_, ui_, hud_, selected_ship_, selected_colony_, selected_body_, save_path_, load_path_);

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
      if (auto it = obj->find("galaxy_map_starfield"); it != obj->end()) {
        ui_.galaxy_map_starfield = it->second.bool_value(ui_.galaxy_map_starfield);
      }
      if (auto it = obj->find("galaxy_map_grid"); it != obj->end()) {
        ui_.galaxy_map_grid = it->second.bool_value(ui_.galaxy_map_grid);
      }
      if (auto it = obj->find("galaxy_map_selected_route"); it != obj->end()) {
        ui_.galaxy_map_selected_route = it->second.bool_value(ui_.galaxy_map_selected_route);
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
      if (auto it = obj->find("show_contact_markers"); it != obj->end()) {
        ui_.show_contact_markers = it->second.bool_value(ui_.show_contact_markers);
      }
      if (auto it = obj->find("show_contact_labels"); it != obj->end()) {
        ui_.show_contact_labels = it->second.bool_value(ui_.show_contact_labels);
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
      if (auto it = obj->find("show_timeline_window"); it != obj->end()) {
        ui_.show_timeline_window = it->second.bool_value(ui_.show_timeline_window);
      }
      if (auto it = obj->find("show_design_studio_window"); it != obj->end()) {
        ui_.show_design_studio_window = it->second.bool_value(ui_.show_design_studio_window);
      }
      if (auto it = obj->find("show_intel_window"); it != obj->end()) {
        ui_.show_intel_window = it->second.bool_value(ui_.show_intel_window);
      }
      if (auto it = obj->find("show_diplomacy_window"); it != obj->end()) {
        ui_.show_diplomacy_window = it->second.bool_value(ui_.show_diplomacy_window);
      }
      if (auto it = obj->find("show_settings_window"); it != obj->end()) {
        ui_.show_settings_window = it->second.bool_value(ui_.show_settings_window);
      }

      if (auto it = obj->find("show_status_bar"); it != obj->end()) {
        ui_.show_status_bar = it->second.bool_value(ui_.show_status_bar);
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
    o["version"] = 10.0;

    // Theme.
    o["clear_color"] = color_to_json(ui_.clear_color);
    o["system_map_bg"] = color_to_json(ui_.system_map_bg);
    o["galaxy_map_bg"] = color_to_json(ui_.galaxy_map_bg);
    o["override_window_bg"] = ui_.override_window_bg;
    o["window_bg"] = color_to_json(ui_.window_bg);
    o["autosave_ui_prefs"] = ui_.autosave_ui_prefs;

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

    // Map rendering chrome.
    o["system_map_starfield"] = ui_.system_map_starfield;
    o["system_map_grid"] = ui_.system_map_grid;
    o["system_map_order_paths"] = ui_.system_map_order_paths;
    o["system_map_fleet_formation_preview"] = ui_.system_map_fleet_formation_preview;
    o["system_map_missile_salvos"] = ui_.system_map_missile_salvos;
    o["system_map_follow_selected"] = ui_.system_map_follow_selected;
    o["galaxy_map_starfield"] = ui_.galaxy_map_starfield;
    o["galaxy_map_grid"] = ui_.galaxy_map_grid;
    o["galaxy_map_selected_route"] = ui_.galaxy_map_selected_route;
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
    o["show_contact_markers"] = ui_.show_contact_markers;
    o["show_contact_labels"] = ui_.show_contact_labels;
    o["show_minor_bodies"] = ui_.show_minor_bodies;
    o["show_minor_body_labels"] = ui_.show_minor_body_labels;
    o["show_galaxy_labels"] = ui_.show_galaxy_labels;
    o["show_galaxy_jump_lines"] = ui_.show_galaxy_jump_lines;
    o["show_galaxy_unknown_exits"] = ui_.show_galaxy_unknown_exits;
    o["show_galaxy_intel_alerts"] = ui_.show_galaxy_intel_alerts;
    o["contact_max_age_days"] = static_cast<double>(ui_.contact_max_age_days);

    // Layout.
    o["show_controls_window"] = ui_.show_controls_window;
    o["show_map_window"] = ui_.show_map_window;
    o["show_details_window"] = ui_.show_details_window;
    o["show_directory_window"] = ui_.show_directory_window;
    o["show_production_window"] = ui_.show_production_window;
    o["show_economy_window"] = ui_.show_economy_window;
    o["show_timeline_window"] = ui_.show_timeline_window;
    o["show_design_studio_window"] = ui_.show_design_studio_window;
    o["show_intel_window"] = ui_.show_intel_window;
    o["show_diplomacy_window"] = ui_.show_diplomacy_window;
    o["show_settings_window"] = ui_.show_settings_window;
    o["show_status_bar"] = ui_.show_status_bar;

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
  ui_.system_map_missile_salvos = false;
  ui_.galaxy_map_starfield = true;
  ui_.galaxy_map_grid = false;
  ui_.galaxy_map_selected_route = true;
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
  ui_.show_timeline_window = false;
  ui_.show_design_studio_window = false;
  ui_.show_intel_window = false;
  ui_.show_diplomacy_window = false;
  ui_.show_settings_window = false;

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
