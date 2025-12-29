#pragma once

#include <string>

#include <SDL.h>

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

class App {
 public:
  App(Simulation sim);

  // Called once per frame.
  void frame();

  // SDL resize events etc.
  void on_event(const SDL_Event& e);

  // Renderer background (RGBA floats in 0..1).
  const float* clear_color_rgba() const { return ui_.clear_color; }

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
  void apply_imgui_style_overrides();

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

  // Map view state
  double map_zoom_{1.0};
  Vec2 map_pan_{0.0, 0.0};

  // Galaxy map view state
  double galaxy_zoom_{1.0};
  Vec2 galaxy_pan_{0.0, 0.0};

  // Shared UI toggles (fog-of-war etc.)
  UIState ui_{};
};

} // namespace nebula4x::ui
