#pragma once

#include <string>
#include <cstdint>

#include <SDL.h>

#include "nebula4x/core/simulation.h"
#include "nebula4x/util/autosave.h"

#include "ui/hud.h"
#include "ui/ui_state.h"

namespace nebula4x::ui {

class App {
 public:
  App(Simulation sim);

  // Called once per frame BEFORE ImGui::NewFrame().
  //
  // This is used for operations that Dear ImGui expects to happen prior to
  // NewFrame (e.g. reloading docking layouts from an ini file).
  void pre_frame();

  // Called once per frame.
  void frame();

  // SDL resize events etc.
  void on_event(const SDL_Event& e);

  // Renderer background (RGBA floats in 0..1).
  const float* clear_color_rgba() const { return ui_.clear_color; }

  // The ini file Dear ImGui uses to persist window positions/docking.
  // This is derived from ui_.layout_profiles_dir + ui_.layout_profile.
  const char* imgui_ini_filename() const;

  // UI preferences (separate from save-games).
  // Returns true on success; on failure, returns false and optionally writes an error.
  bool load_ui_prefs(const char* path, std::string* error = nullptr);
  bool save_ui_prefs(const char* path, std::string* error = nullptr) const;

  // Reset only theme/layout preferences (does not touch fog-of-war, selection, etc.).
  void reset_ui_theme_defaults();
  void reset_window_layout_defaults();

  const char* ui_prefs_path() const { return ui_prefs_path_; }
  char* ui_prefs_path_buf() { return ui_prefs_path_; }

  bool autosave_ui_prefs_enabled() const { return ui_.autosave_ui_prefs; }

 private:
  void update_imgui_ini_path_from_ui();
  void apply_imgui_style_overrides();
  void draw_dockspace();
  void build_default_dock_layout(unsigned int dockspace_id);

  Simulation sim_;

  // Selection state
  Id selected_ship_{kInvalidId};
  Id selected_colony_{kInvalidId};
  Id selected_body_{kInvalidId};

  // File dialogs (simple text inputs)
  char save_path_[256] = "saves/save.json";
  char load_path_[256] = "saves/save.json";

  // UI prefs file (colors/layout). Separate from save-games.
  char ui_prefs_path_[256] = "ui_prefs.json";

  // Dear ImGui ini file used for docking/window position persistence.
  std::string imgui_ini_path_ = "ui_layouts/default.ini";
  std::string last_imgui_ini_path_applied_;

  // Map view state
  double map_zoom_{1.0};
  Vec2 map_pan_{0.0, 0.0};

  // Galaxy map view state
  double galaxy_zoom_{1.0};
  Vec2 galaxy_pan_{0.0, 0.0};

  // Shared UI toggles (fog-of-war etc.)
  UIState ui_{};

  // Rolling autosave (separate from UI prefs autosave-on-exit).
  nebula4x::AutosaveManager autosave_mgr_{};
  std::uint64_t last_seen_state_generation_{0};

  // HUD transient state (command palette query, toast queue, etc.).
  HUDState hud_{};

  // Docking: when enabled, we create a fullscreen dockspace and build a
  // sensible default layout the first time (or when the user resets layout).
  bool dock_layout_initialized_{false};
  bool dock_layout_checked_ini_{false};
  bool dock_layout_has_existing_ini_{false};
};

} // namespace nebula4x::ui
