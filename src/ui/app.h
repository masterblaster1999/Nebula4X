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

 private:
  Simulation sim_;

  // Selection state
  Id selected_ship_{kInvalidId};
  Id selected_colony_{kInvalidId};

  // File dialogs (simple text inputs)
  char save_path_[256] = "saves/save.json";
  char load_path_[256] = "saves/save.json";

  // Map view state
  double map_zoom_{1.0};
  Vec2 map_pan_{0.0, 0.0};

  // Shared UI toggles (fog-of-war etc.)
  UIState ui_{};
};

} // namespace nebula4x::ui
