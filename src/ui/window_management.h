#pragma once

#include <imgui.h>

#include <vector>

#include "ui/ui_state.h"

namespace nebula4x::ui {

// How a window should be launched when it is opened.
//
// - Docked: use whatever docking/layout state is stored in the current ImGui ini profile.
// - Popup: force the window to appear as a floating popup (moveable, and detachable with multi-viewport).
enum class WindowLaunchMode : int {
  Docked = 0,
  Popup = 1,
};

struct WindowSpec {
  // Stable id used for preferences.
  const char* id;
  // Exact Dear ImGui window name (matches the string passed to ImGui::Begin).
  const char* title;
  // Friendly label shown in the Window Manager.
  const char* label;
  // Category label shown in the Window Manager.
  const char* category;
  // Suggested popup size (<= 0 means auto).
  ImVec2 popup_size;

  // Which UIState flag controls visibility.
  bool UIState::*open_flag;

  // Core windows are never affected by popup-first mode by default.
  bool core;
  // Some windows do their own special positioning and shouldn't be auto-popped.
  bool supports_popup;

  // Default launch mode when popup-first mode is off and there is no override.
  WindowLaunchMode default_mode;
};

// Registry of all major windows that can be managed by the Window Manager.
const std::vector<WindowSpec>& window_specs();

// Find a window spec by its stable id.
const WindowSpec* find_window_spec(const char* id);

// Resolve the effective launch mode for a window given user preferences and defaults.
WindowLaunchMode effective_launch_mode(const UIState& ui, const WindowSpec& spec);

// Request that a window be popped out (undocked and centered) the next time it is drawn.
// If the window is currently closed, it will be opened.
void request_popout(UIState& ui, const char* id);

// Apply popup/window placement policy for the *next* ImGui::Begin call.
//
// This must be called immediately before drawing the window corresponding to `id`.
// It is safe to call only when that window is going to be drawn, because it uses
// ImGui::SetNextWindow* APIs.
void prepare_window_for_draw(UIState& ui, const char* id);

// Update internal per-window tracking at the end of the frame.
void window_management_end_frame(UIState& ui);

// "Focus Mode": hide non-essential windows to declutter the main view.
void set_focus_mode(UIState& ui, bool enabled);
void toggle_focus_mode(UIState& ui);
bool focus_mode_enabled(const UIState& ui);

}  // namespace nebula4x::ui
