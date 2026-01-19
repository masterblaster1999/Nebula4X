#include "ui/procedural_layout.h"

// DockBuilder* API lives in Dear ImGui's internal header.
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <utility>

namespace nebula4x::ui {
namespace {

struct XorShift32 {
  std::uint32_t x{1337};

  std::uint32_t next_u32() {
    // xorshift32: simple deterministic RNG good enough for UI layouts.
    std::uint32_t v = x;
    v ^= v << 13;
    v ^= v >> 17;
    v ^= v << 5;
    x = v;
    return v;
  }

  float next01() {
    // Map to [0,1].
    const std::uint32_t v = next_u32();
    return static_cast<float>(v) * (1.0f / 4294967295.0f);
  }

  float range(float lo, float hi) {
    return lo + (hi - lo) * next01();
  }

  bool chance(float p) {
    return next01() < p;
  }
};

float clamp01(float v) {
  return std::clamp(v, 0.0f, 1.0f);
}

float jitter_ratio(XorShift32& rng, float base, float amplitude, float variation, float lo, float hi) {
  const float t = (rng.next01() - 0.5f) * 2.0f; // -1..1
  const float v = base + t * amplitude * clamp01(variation);
  return std::clamp(v, lo, hi);
}

bool parse_bool(const std::string& v, bool fallback) {
  if (v.empty()) return fallback;
  if (v == "1" || v == "true" || v == "True" || v == "TRUE" || v == "yes" || v == "Yes" || v == "YES") return true;
  if (v == "0" || v == "false" || v == "False" || v == "FALSE" || v == "no" || v == "No" || v == "NO") return false;
  return fallback;
}

void dock_window(ImGuiID dock_id, const char* title) {
  if (dock_id == 0 || title == nullptr || title[0] == '\0') return;
  ImGui::DockBuilderDockWindow(title, dock_id);
}

void dock_windows(ImGuiID dock_id, const std::vector<const char*>& titles) {
  for (const char* t : titles) dock_window(dock_id, t);
}

void dock_windows(ImGuiID dock_id, const std::vector<std::string>& titles) {
  for (const auto& t : titles) dock_window(dock_id, t.c_str());
}

} // namespace

std::string encode_layout_dna(const ProceduralLayoutParams& p) {
  std::ostringstream oss;
  oss << "nebula-layout-v1"
      << " seed=" << p.seed
      << " mode=" << p.mode
      << " var=" << p.variation
      << " tools=" << (p.include_tools ? 1 : 0)
      << " forge=" << (p.include_forge_panels ? 1 : 0)
      << " max=" << p.max_forge_panels
      << " open=" << (p.auto_open_windows ? 1 : 0)
      << " save=" << (p.auto_save_profile ? 1 : 0);
  return oss.str();
}

bool decode_layout_dna(const std::string& s, ProceduralLayoutParams* out, std::string* error) {
  if (out == nullptr) return false;

  ProceduralLayoutParams p = *out; // start from existing defaults

  // Normalize separators into whitespace so we can use stream tokenization.
  std::string norm;
  norm.reserve(s.size());
  for (char c : s) {
    if (c == ';' || c == '|' || c == ',' || c == '\n' || c == '\t' || c == '\r') {
      norm.push_back(' ');
    } else {
      norm.push_back(c);
    }
  }

  std::istringstream iss(norm);
  std::string tok;

  bool any = false;
  while (iss >> tok) {
    if (tok == "nebula-layout-v1") continue;

    const auto eq = tok.find('=');
    if (eq == std::string::npos) continue;

    const std::string key = tok.substr(0, eq);
    const std::string val = tok.substr(eq + 1);

    try {
      if (key == "seed") {
        p.seed = static_cast<std::uint32_t>(std::stoul(val));
        any = true;
      } else if (key == "mode") {
        p.mode = std::stoi(val);
        any = true;
      } else if (key == "var") {
        p.variation = std::stof(val);
        any = true;
      } else if (key == "tools") {
        p.include_tools = parse_bool(val, p.include_tools);
        any = true;
      } else if (key == "forge") {
        p.include_forge_panels = parse_bool(val, p.include_forge_panels);
        any = true;
      } else if (key == "max") {
        p.max_forge_panels = std::stoi(val);
        any = true;
      } else if (key == "open") {
        p.auto_open_windows = parse_bool(val, p.auto_open_windows);
        any = true;
      } else if (key == "save") {
        p.auto_save_profile = parse_bool(val, p.auto_save_profile);
        any = true;
      }
    } catch (...) {
      // Ignore parse errors; we validate at the end.
    }
  }

  if (!any) {
    if (error) *error = "No recognized key/value pairs.";
    return false;
  }

  // Clamp.
  p.mode = std::clamp(p.mode, 0, 4);
  p.variation = clamp01(p.variation);
  p.max_forge_panels = std::clamp(p.max_forge_panels, 0, 64);

  *out = p;
  return true;
}

std::vector<std::string> gather_ui_forge_panel_window_titles(const UIState& ui, int max_panels) {
  std::vector<std::string> out;
  int count = 0;
  for (const auto& p : ui.ui_forge_panels) {
    if (!p.open) continue;
    std::string title = (p.name.empty() ? "Custom Panel" : p.name);
    title += "##uiforge_" + std::to_string(p.id);
    out.push_back(std::move(title));

    count++;
    if (max_panels > 0 && count >= max_panels) break;
  }
  return out;
}

void apply_procedural_layout_visibility(UIState& ui, const ProceduralLayoutParams& p) {
  // Core windows.
  ui.show_controls_window = true;
  ui.show_map_window = true;
  ui.show_details_window = true;

  // Layout archetypes.
  switch (std::clamp(p.mode, 0, 4)) {
    case 0: { // Balanced
      ui.show_directory_window = true;
      ui.show_production_window = true;
      ui.show_economy_window = true;
      ui.show_planner_window = true;
      ui.show_timeline_window = true;
      break;
    }
    case 1: { // Command
      ui.show_directory_window = true;
      ui.show_planner_window = true;
      ui.show_time_warp_window = true;
      ui.show_timeline_window = true;
      ui.show_intel_window = true;
      break;
    }
    case 2: { // Data
      ui.show_directory_window = true;
      ui.show_data_lenses_window = true;
      ui.show_dashboards_window = true;
      ui.show_pivot_tables_window = true;
      ui.show_watchboard_window = true;
      ui.show_production_window = true;
      ui.show_economy_window = true;
      ui.show_timeline_window = true;
      break;
    }
    case 3: { // Debug
      ui.show_directory_window = true;
      ui.show_json_explorer_window = true;
      ui.show_state_doctor_window = true;
      ui.show_content_validation_window = true;
      ui.show_entity_inspector_window = true;
      ui.show_reference_graph_window = true;
      ui.show_save_tools_window = true;
      ui.show_time_machine_window = true;
      ui.show_omni_search_window = true;
      break;
    }
    case 4: { // Forge
      ui.show_directory_window = true;
      ui.show_ui_forge_window = true;
      ui.show_watchboard_window = true;
      ui.show_data_lenses_window = true;
      ui.show_dashboards_window = true;
      break;
    }
  }

  if (p.include_tools) {
    ui.show_omni_search_window = true;
    ui.show_json_explorer_window = true;
    ui.show_state_doctor_window = true;
    ui.show_reference_graph_window = true;
  }

  // UI Forge panels.
  if (p.include_forge_panels) {
    int opened = 0;
    for (auto& panel : ui.ui_forge_panels) {
      if (p.max_forge_panels > 0 && opened >= p.max_forge_panels) break;
      panel.open = true;
      opened++;
    }
  }
}

void build_procedural_dock_layout(ImGuiID dockspace_id, const ImVec2& size, const ProceduralLayoutParams& in,
                                  const std::vector<std::string>& extra_windows) {
  if (dockspace_id == 0) return;
  if ((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_DockingEnable) == 0) return;

  ProceduralLayoutParams p = in;
  p.mode = std::clamp(p.mode, 0, 4);
  p.variation = clamp01(p.variation);
  p.max_forge_panels = std::clamp(p.max_forge_panels, 0, 64);

  XorShift32 rng;
  rng.x = (p.seed == 0 ? 1337u : p.seed);

  // Remove any existing layout and create a fresh dockspace node.
  ImGui::DockBuilderRemoveNode(dockspace_id);
  ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(dockspace_id, size);

  ImGuiID dock_main = dockspace_id;
  ImGuiID dock_left = 0;
  ImGuiID dock_right = 0;
  ImGuiID dock_bottom = 0;
  ImGuiID dock_top = 0;

  // These can be used by certain archetypes.
  ImGuiID dock_data = 0;
  ImGuiID dock_panels = 0;
  ImGuiID dock_details = 0;

  switch (p.mode) {
    default:
    case 0: { // Balanced
      const float left_ratio = jitter_ratio(rng, 0.22f, 0.10f, p.variation, 0.12f, 0.35f);
      const float right_ratio = jitter_ratio(rng, 0.26f, 0.12f, p.variation, 0.14f, 0.40f);
      const float bottom_ratio = jitter_ratio(rng, 0.30f, 0.14f, p.variation, 0.18f, 0.46f);
      const bool has_top = rng.chance(0.15f + 0.25f * p.variation);
      const float top_ratio = jitter_ratio(rng, 0.18f, 0.10f, p.variation, 0.12f, 0.32f);

      dock_left = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Left, left_ratio, nullptr, &dock_main);
      dock_right = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Right, right_ratio, nullptr, &dock_main);
      dock_bottom = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Down, bottom_ratio, nullptr, &dock_main);
      if (has_top) {
        dock_top = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Up, top_ratio, nullptr, &dock_main);
      }

      dock_window(dock_left, "Controls");
      dock_window(dock_right, "Details");
      dock_window(dock_main, "Map");

      // Bottom stack: logistics + planning.
      std::vector<const char*> bottom = {"Directory", "Production", "Economy", "Planner", "Timeline"};
      if (p.include_tools) {
        bottom.push_back("OmniSearch");
      }
      dock_windows(dock_bottom, bottom);

      if (dock_top != 0) {
        std::vector<const char*> top = {"Time Warp", "Intel"};
        dock_windows(dock_top, top);
      }

      // Optional: dock extra panels into the bottom stack.
      if (!extra_windows.empty()) {
        dock_windows(dock_bottom, extra_windows);
      }

      break;
    }

    case 1: { // Command
      const float left_ratio = jitter_ratio(rng, 0.25f, 0.10f, p.variation, 0.14f, 0.38f);
      const float right_ratio = jitter_ratio(rng, 0.28f, 0.14f, p.variation, 0.16f, 0.45f);
      const float bottom_ratio = jitter_ratio(rng, 0.33f, 0.16f, p.variation, 0.18f, 0.50f);

      dock_left = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Left, left_ratio, nullptr, &dock_main);
      dock_right = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Right, right_ratio, nullptr, &dock_main);
      dock_bottom = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Down, bottom_ratio, nullptr, &dock_main);

      // Split the right side into Details and Time/Intel controls.
      ImGuiID dock_right_bottom = 0;
      dock_right_bottom = ImGui::DockBuilderSplitNode(dock_right, ImGuiDir_Down,
                                                      jitter_ratio(rng, 0.50f, 0.20f, p.variation, 0.30f, 0.70f),
                                                      nullptr, &dock_right);

      dock_window(dock_left, "Controls");
      dock_windows(dock_left, std::vector<const char*>{"Directory"});

      dock_window(dock_right, "Details");
      dock_windows(dock_right, std::vector<const char*>{"Intel"});

      dock_windows(dock_right_bottom, std::vector<const char*>{"Time Warp", "Planner"});

      dock_window(dock_main, "Map");

      std::vector<const char*> bottom = {"Timeline", "Production", "Economy", "Diplomacy Graph"};
      if (p.include_tools) bottom.push_back("OmniSearch");
      dock_windows(dock_bottom, bottom);

      if (!extra_windows.empty()) {
        dock_windows(dock_bottom, extra_windows);
      }

      break;
    }

    case 2: { // Data
      const float left_ratio = jitter_ratio(rng, 0.22f, 0.12f, p.variation, 0.14f, 0.40f);
      const float right_ratio = jitter_ratio(rng, 0.28f, 0.14f, p.variation, 0.16f, 0.46f);
      const float bottom_ratio = jitter_ratio(rng, 0.28f, 0.18f, p.variation, 0.16f, 0.55f);

      dock_left = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Left, left_ratio, nullptr, &dock_main);
      dock_right = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Right, right_ratio, nullptr, &dock_main);
      dock_bottom = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Down, bottom_ratio, nullptr, &dock_main);

      // Split central area into Map (top) and Data (bottom).
      dock_data = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Down,
                                              jitter_ratio(rng, 0.55f, 0.20f, p.variation, 0.40f, 0.70f),
                                              nullptr, &dock_main);

      dock_windows(dock_left, std::vector<const char*>{"Directory", "Pivot Tables"});
      dock_windows(dock_left, std::vector<const char*>{"Watchboard (JSON Pins)"});

      dock_windows(dock_right, std::vector<const char*>{"Details", "Economy", "Production"});

      dock_window(dock_main, "Map");
      dock_windows(dock_data, std::vector<const char*>{"Data Lenses", "Dashboards"});

      std::vector<const char*> bottom = {"Timeline", "Planner"};
      if (p.include_tools) {
        bottom.push_back("JSON Explorer");
        bottom.push_back("State Doctor");
      }
      dock_windows(dock_bottom, bottom);

      if (!extra_windows.empty()) {
        dock_windows(dock_data, extra_windows);
      }

      break;
    }

    case 3: { // Debug
      const float left_ratio = jitter_ratio(rng, 0.22f, 0.12f, p.variation, 0.14f, 0.40f);
      const float right_ratio = jitter_ratio(rng, 0.36f, 0.16f, p.variation, 0.20f, 0.55f);
      const float bottom_ratio = jitter_ratio(rng, 0.30f, 0.16f, p.variation, 0.18f, 0.55f);

      dock_left = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Left, left_ratio, nullptr, &dock_main);
      dock_right = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Right, right_ratio, nullptr, &dock_main);
      dock_bottom = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Down, bottom_ratio, nullptr, &dock_main);

      // Split center for a dedicated details view.
      dock_details = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Right,
                                                 jitter_ratio(rng, 0.28f, 0.10f, p.variation, 0.18f, 0.40f),
                                                 nullptr, &dock_main);

      dock_windows(dock_left, std::vector<const char*>{"Controls", "Directory", "Content Validation"});

      dock_window(dock_main, "Map");
      dock_window(dock_details, "Details");

      std::vector<const char*> debug = {"JSON Explorer", "State Doctor", "Entity Inspector (ID Resolver)",
                                        "Reference Graph (Entity IDs)"};
      dock_windows(dock_right, debug);

      std::vector<const char*> bottom = {"Save Tools", "Time Machine", "OmniSearch"};
      dock_windows(dock_bottom, bottom);

      if (!extra_windows.empty()) {
        dock_windows(dock_right, extra_windows);
      }

      break;
    }

    case 4: { // Forge
      const float left_ratio = jitter_ratio(rng, 0.28f, 0.12f, p.variation, 0.16f, 0.45f);
      const float right_ratio = jitter_ratio(rng, 0.26f, 0.14f, p.variation, 0.14f, 0.45f);
      const float bottom_ratio = jitter_ratio(rng, 0.30f, 0.16f, p.variation, 0.18f, 0.55f);

      dock_left = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Left, left_ratio, nullptr, &dock_main);
      dock_right = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Right, right_ratio, nullptr, &dock_main);
      dock_bottom = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Down, bottom_ratio, nullptr, &dock_main);

      // Reserve a panel strip next to the map for custom UI Forge panels.
      dock_panels = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Right,
                                                jitter_ratio(rng, 0.34f, 0.18f, p.variation, 0.20f, 0.55f),
                                                nullptr, &dock_main);

      dock_windows(dock_left, std::vector<const char*>{"Controls", "UI Forge (Custom Panels)"});

      dock_window(dock_main, "Map");

      dock_windows(dock_right, std::vector<const char*>{"Details", "Watchboard (JSON Pins)"});

      dock_windows(dock_bottom, std::vector<const char*>{"Data Lenses", "Dashboards", "Pivot Tables"});

      if (!extra_windows.empty()) {
        dock_windows(dock_panels, extra_windows);
      } else {
        // No panels yet? Put the editor there so users discover it.
        dock_window(dock_panels, "UI Forge (Custom Panels)");
      }

      break;
    }
  }

  ImGui::DockBuilderFinish(dockspace_id);
}

} // namespace nebula4x::ui
