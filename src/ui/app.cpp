#include "ui/app.h"

#include <imgui.h>

#include <algorithm>
#include <filesystem>

#include "nebula4x/util/json.h"
#include "nebula4x/util/log.h"

#include "nebula4x/core/serialization.h"
#include "nebula4x/util/file_io.h"
#include "ui/panels.h"
#include "ui/economy_window.h"
#include "ui/galaxy_map.h"
#include "ui/system_map.h"

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

  // Optional floating windows.
  if (ui_.show_directory_window) draw_directory_window(sim_, ui_, selected_colony_, selected_body_);
  if (ui_.show_economy_window) draw_economy_window(sim_, ui_, selected_colony_, selected_body_);

  // Help overlay/window.
  draw_help_window(ui_);

  // Layout: left sidebar / center map / right sidebar
  // The layout adapts when panels are hidden.
  const ImVec2 ds = ImGui::GetIO().DisplaySize;
  constexpr float kMargin = 10.0f;
  const float kTop = ImGui::GetFrameHeight() + kMargin;
  const float status_h = ui_.show_status_bar ? (ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2.0f) : 0.0f;
  const float bottom_reserve = ui_.show_status_bar ? (status_h + kMargin) : kMargin;
  const float full_h = std::max(100.0f, ds.y - (kTop + bottom_reserve));
  constexpr float kSideW = 300.0f;

  float x_left = kMargin;
  float x_map = kMargin;
  float x_right = ds.x - (kSideW + kMargin);

  if (ui_.show_controls_window) {
    ImGui::SetNextWindowPos(ImVec2(x_left, kTop), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kSideW, full_h), ImGuiCond_Always);
    ImGui::Begin("Controls", &ui_.show_controls_window, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
    draw_left_sidebar(sim_, ui_, selected_ship_, selected_colony_);
    ImGui::End();
    x_map = x_left + kSideW + kMargin;
  }

  if (ui_.show_details_window) {
    ImGui::SetNextWindowPos(ImVec2(x_right, kTop), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kSideW, full_h), ImGuiCond_Always);
    ImGui::Begin("Details", &ui_.show_details_window, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
    draw_right_sidebar(sim_, ui_, selected_ship_, selected_colony_, selected_body_);
    ImGui::End();
  }

  if (ui_.show_map_window) {
    const float right_gutter = ui_.show_details_window ? (kSideW + kMargin) : kMargin;
    const float map_w = std::max(200.0f, ds.x - x_map - right_gutter);

    ImGui::SetNextWindowPos(ImVec2(x_map, kTop), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(map_w, full_h), ImGuiCond_Always);
    ImGui::Begin("Map", &ui_.show_map_window, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
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
    ImGui::End();
  }

  // HUD chrome (status bar, command palette, event toasts).
  draw_status_bar(sim_, ui_, hud_, selected_ship_, selected_colony_, selected_body_, save_path_, load_path_);
  draw_command_palette(sim_, ui_, hud_, selected_ship_, selected_colony_, selected_body_, save_path_, load_path_);

  update_event_toasts(sim_, ui_, hud_);
  draw_event_toasts(sim_, ui_, hud_, selected_ship_, selected_colony_, selected_body_);
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
      if (auto it = obj->find("show_economy_window"); it != obj->end()) {
        ui_.show_economy_window = it->second.bool_value(ui_.show_economy_window);
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
    o["version"] = 2.0;

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

    // Layout.
    o["show_controls_window"] = ui_.show_controls_window;
    o["show_map_window"] = ui_.show_map_window;
    o["show_details_window"] = ui_.show_details_window;
    o["show_directory_window"] = ui_.show_directory_window;
    o["show_economy_window"] = ui_.show_economy_window;
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

  ui_.ui_scale = 1.0f;
}

void App::reset_window_layout_defaults() {
  ui_.show_controls_window = true;
  ui_.show_map_window = true;
  ui_.show_details_window = true;
  ui_.show_directory_window = true;
  ui_.show_economy_window = false;
  ui_.show_settings_window = false;

  ui_.show_status_bar = true;
  ui_.show_event_toasts = true;
  ui_.event_toast_duration_sec = 6.0f;
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
