#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// A lightweight, transient UI notification representing a new SimEvent.
//
// These are not persisted; they are generated from the event log while running.
struct EventToast {
  std::uint64_t seq{0};
  std::int64_t day{0};
  EventLevel level{EventLevel::Info};
  EventCategory category{EventCategory::General};

  // Optional quick navigation context.
  Id faction_id{kInvalidId};
  Id faction_id2{kInvalidId};
  Id system_id{kInvalidId};
  Id ship_id{kInvalidId};
  Id colony_id{kInvalidId};

  std::string message;
  double created_time_s{0.0};
};

// UI-only state for HUD features (command palette query, toast queue, etc.).
struct HUDState {
  char palette_query[128]{};
  int palette_selected_idx{0};

  std::uint64_t last_toast_seq{0};
  std::vector<EventToast> toasts;
};

void draw_status_bar(Simulation& sim, UIState& ui, HUDState& hud, Id& selected_ship, Id& selected_colony,
                     Id& selected_body, char* save_path, char* load_path);

void draw_help_window(UIState& ui);

void draw_command_palette(Simulation& sim, UIState& ui, HUDState& hud, Id& selected_ship, Id& selected_colony,
                          Id& selected_body, char* save_path, char* load_path);

void update_event_toasts(const Simulation& sim, UIState& ui, HUDState& hud);
void draw_event_toasts(Simulation& sim, UIState& ui, HUDState& hud, Id& selected_ship, Id& selected_colony,
                       Id& selected_body);

} // namespace nebula4x::ui
