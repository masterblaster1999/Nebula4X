#pragma once

#include <imgui.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ui/ui_state.h"

namespace nebula4x::ui {

// Parameters controlling procedural docking layout generation.
//
// This builds a dockspace layout using Dear ImGui's DockBuilder API.
// The resulting dock layout is deterministic given the same seed+params and
// can be saved into a layout profile (ImGui ini file).
struct ProceduralLayoutParams {
  std::uint32_t seed{1337};

  // 0=Balanced, 1=Command, 2=Data, 3=Debug, 4=Forge
  int mode{0};

  // 0..1: how much randomness to inject into split ratios / window assignment.
  float variation{0.45f};

  bool include_tools{false};
  bool include_forge_panels{true};

  // Limit how many UI Forge panel windows are auto-docked.
  // 0 = all (be careful: can create a huge tab stack).
  int max_forge_panels{4};

  // When true, the generator also toggles expected windows open.
  bool auto_open_windows{true};

  // When enabled, generation also saves to the active ImGui ini file.
  bool auto_save_profile{false};
};

// Encode params into a compact, shareable string.
std::string encode_layout_dna(const ProceduralLayoutParams& p);

// Decode params from a previously encoded DNA string.
// Returns true on success; on failure returns false and optionally fills error.
bool decode_layout_dna(const std::string& s, ProceduralLayoutParams* out, std::string* error = nullptr);

// Convert UI Forge panel configs into the exact ImGui window titles used when
// drawing those panel windows.
std::vector<std::string> gather_ui_forge_panel_window_titles(const UIState& ui, int max_panels);

// Optionally open a set of windows that the chosen layout mode expects.
//
// This is intentionally conservative: it will only set a subset of windows to
// open (it does not close other windows).
void apply_procedural_layout_visibility(UIState& ui, const ProceduralLayoutParams& p);

// Build an ImGui dock layout under the given dockspace id.
//
// extra_windows are additional window titles to dock (e.g. UI Forge panel windows).
void build_procedural_dock_layout(ImGuiID dockspace_id, const ImVec2& size, const ProceduralLayoutParams& p,
                                  const std::vector<std::string>& extra_windows);

} // namespace nebula4x::ui
