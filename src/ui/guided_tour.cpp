#include "ui/guided_tour.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "ui/screen_reader.h"

namespace nebula4x::ui {

namespace {

using EnsureVisibleFn = void (*)(UIState&);

struct TourStepDef {
  const char* title;
  const char* body;
  const char* target_window;  // ImGui window name to spotlight (optional).
  EnsureVisibleFn ensure_visible;  // Opens target window (optional).
  const char* doc_ref;  // Codex doc reference (optional, e.g. "ui_tour.md").
};

struct TourDef {
  const char* name;
  const char* blurb;
  const TourStepDef* steps;
  int step_count;
  const char* doc_ref;  // Optional doc describing the tour.
};

// --- Ensure-visible helpers --------------------------------------------------

void open_controls(UIState& ui) { ui.show_controls_window = true; }

void open_map_system(UIState& ui) {
  ui.show_map_window = true;
  ui.request_map_tab = MapTab::System;
}

void open_details(UIState& ui) { ui.show_details_window = true; }

void open_command_console(UIState& ui) { ui.show_command_palette = true; }

void open_notifications(UIState& ui) { ui.show_notifications_window = true; }

void open_help_tours_doc(UIState& ui) {
  ui.show_help_window = true;
  ui.request_help_tab = HelpTab::Docs;
  ui.request_open_doc_ref = "tours.md";
}

void open_watchboard(UIState& ui) { ui.show_watchboard_window = true; }
void open_data_lenses(UIState& ui) { ui.show_data_lenses_window = true; }
void open_dashboards(UIState& ui) { ui.show_dashboards_window = true; }
void open_ui_forge(UIState& ui) { ui.show_ui_forge_window = true; }
void open_context_forge(UIState& ui) { ui.show_context_forge_window = true; }
void open_omnisearch(UIState& ui) { ui.show_omni_search_window = true; }
void open_json_explorer(UIState& ui) { ui.show_json_explorer_window = true; }

void open_layout_profiles(UIState& ui) { ui.show_layout_profiles_window = true; }
void open_navigator(UIState& ui) { ui.show_navigator_window = true; }
void open_settings(UIState& ui) { ui.show_settings_window = true; }

// --- Tours ------------------------------------------------------------------

static const TourStepDef kTourCoreWorkspaceSteps[] = {
    {
        "Welcome",
        "This overlay will spotlight key panels and teach a few everyday workflows.\n\n"
        "Use the buttons below (or Left/Right arrow keys) to move through steps.\n"
        "Press F2 anytime to toggle the tour overlay.",
        nullptr,
        nullptr,
        "getting_started.md",
    },
    {
        "Controls",
        "The Controls window is your primary command surface.\n\n"
        "Tip: You can dock/undock windows by dragging their tabs.",
        "Controls",
        open_controls,
        "ui_tour.md",
    },
    {
        "Map",
        "The Map window provides System and Galaxy views.\n\n"
        "Try switching tabs: System for local orbital context, Galaxy for big-picture navigation.",
        "Map",
        open_map_system,
        "ui_tour.md",
    },
    {
        "Details",
        "The Details window shows context for your current selection (ship/colony/body).\n\n"
        "Many actions will automatically open Details to the relevant tab.",
        "Details",
        open_details,
        "ui_tour.md",
    },
    {
        "Command Console",
        "The Command Console (Ctrl+P) is a fast way to open windows and run UI actions.\n\n"
        "If you're unsure where a feature lives, try searching for it here.",
        "Command Console",
        open_command_console,
        "command_console.md",
    },
    {
        "Notification Center",
        "The Notification Center (F3) is a persistent inbox for events, alerts and watchboard triggers.\n\n"
        "Pin important items and use filters to keep signal over noise.",
        "Notification Center",
        open_notifications,
        "notifications.md",
    },
    {
        "Codex & Tours",
        "Help / Codex (F1) hosts offline documentation and this Tours panel.\n\n"
        "You can start any tour from Help → Tours.",
        "Help / Codex",
        open_help_tours_doc,
        "tours.md",
    },
    {
        "You're set",
        "That's it for the quick orientation.\n\n"
        "Recommended next: open Procedural Tools tour to learn Watchboard + Data Lenses.",
        nullptr,
        nullptr,
        "index.md",
    },
};

static const TourStepDef kTourProceduralToolsSteps[] = {
    {
        "Why these tools",
        "Nebula4X has several UI-only tools to inspect state and build custom views.\n\n"
        "This tour spotlights the procedural stack used for debugging and power-user workflows.",
        nullptr,
        nullptr,
        "automation.md",
    },
    {
        "Watchboard",
        "Watchboard (JSON Pins) lets you pin JSON paths and create alerts that trigger when a value changes.\n\n"
        "Tip: combine Watchboard alerts with Notification Center for a persistent signal.",
        "Watchboard (JSON Pins)",
        open_watchboard,
        "notifications.md",
    },
    {
        "Data Lenses",
        "Data Lenses are lightweight filters/formatters you can apply to game data for exploration.\n\n"
        "They're intended as a bridge between raw JSON and higher-level UI.",
        "Data Lenses",
        open_data_lenses,
        nullptr,
    },
    {
        "Dashboards",
        "Dashboards are curated screens for monitoring a slice of the game.\n\n"
        "As more simulation systems come online, dashboards will evolve into your strategic overview.",
        "Dashboards",
        open_dashboards,
        nullptr,
    },
    {
        "UI Forge",
        "UI Forge lets you build custom panels (tables/cards) from the current simulation snapshot.\n\n"
        "Think of it as a user-level modding layer for UI.",
        "UI Forge (Custom Panels)",
        open_ui_forge,
        nullptr,
    },
    {
        "Context Forge",
        "Context Forge builds procedural panels for the currently selected entity.\n\n"
        "Use it when you're exploring: it keeps views synced with selection changes.",
        "Context Forge (Procedural Panels)",
        open_context_forge,
        nullptr,
    },
    {
        "OmniSearch",
        "OmniSearch (Ctrl+F) searches the live JSON snapshot.\n\n"
        "Use it to answer: 'where is this value stored?' or 'what changed this turn?'.",
        "OmniSearch",
        open_omnisearch,
        nullptr,
    },
    {
        "JSON Explorer",
        "JSON Explorer is a structured view of the simulation snapshot.\n\n"
        "If you ever suspect a UI bug, JSON Explorer is the fastest sanity check.",
        "JSON Explorer",
        open_json_explorer,
        nullptr,
    },
    {
        "Done",
        "That's the procedural stack.\n\n"
        "Next: try Workspaces & Navigation to learn Layout Profiles and selection history.",
        nullptr,
        nullptr,
        "tours.md",
    },
};

static const TourStepDef kTourWorkspacesSteps[] = {
    {
        "Workspaces",
        "Nebula4X uses ImGui docking, so you can treat the UI like a configurable workspace.\n\n"
        "This tour shows how to save layouts and move quickly between contexts.",
        nullptr,
        nullptr,
        "ui_tour.md",
    },
    {
        "Layout Profiles",
        "Layout Profiles save and load window docking layouts.\n\n"
        "Use them to create task-based workspaces (exploration, economy, combat, debugging).",
        "Layout Profiles",
        open_layout_profiles,
        "ui_tour.md",
    },
    {
        "Navigator",
        "Navigator is a selection-focused jump tool.\n\n"
        "Use it to quickly switch between ships, colonies and points of interest.",
        "Navigator",
        open_navigator,
        nullptr,
    },
    {
        "Selection history",
        "Selection changes are recorded into a history stack so you can go back/forward after drilling into details.\n\n"
        "Tip: bookmarks and history are designed to reduce 'where was I?' moments.",
        nullptr,
        nullptr,
        nullptr,
    },
    {
        "Settings",
        "Settings contains UI preferences (scaling, docking behavior, accessibility).\n\n"
        "If something feels uncomfortable, check Settings first.",
        "Settings",
        open_settings,
        "troubleshooting.md",
    },
    {
        "Done",
        "You're ready to explore.\n\n"
        "If you get lost, open the Command Console (Ctrl+P) and search for the window you need.",
        nullptr,
        nullptr,
        "index.md",
    },
};

static const TourDef kTours[] = {
    {
        "Core Workspace",
        "A fast orientation: Controls, Map, Details and the two core utility surfaces (Console + Notifications).",
        kTourCoreWorkspaceSteps,
        (int)(sizeof(kTourCoreWorkspaceSteps) / sizeof(kTourCoreWorkspaceSteps[0])),
        "tours.md",
    },
    {
        "Procedural Tools",
        "Watchboard + Data Lenses + Dashboards + Forge tools used for power workflows and debugging.",
        kTourProceduralToolsSteps,
        (int)(sizeof(kTourProceduralToolsSteps) / sizeof(kTourProceduralToolsSteps[0])),
        "tours.md",
    },
    {
        "Workspaces & Navigation",
        "Layout profiles, selection history and small tools that keep long sessions manageable.",
        kTourWorkspacesSteps,
        (int)(sizeof(kTourWorkspacesSteps) / sizeof(kTourWorkspacesSteps[0])),
        "tours.md",
    },
};

constexpr int tour_count() { return (int)(sizeof(kTours) / sizeof(kTours[0])); }

const TourDef* get_tour(int idx) {
  if (idx < 0 || idx >= tour_count()) return nullptr;
  return &kTours[idx];
}

void clamp_indices(UIState& ui) {
  const int tc = tour_count();
  if (tc <= 0) {
    ui.tour_active = false;
    ui.tour_active_index = 0;
    ui.tour_step_index = 0;
    return;
  }

  ui.tour_active_index = std::clamp(ui.tour_active_index, 0, tc - 1);
  const TourDef& t = kTours[ui.tour_active_index];
  if (t.step_count <= 0) {
    ui.tour_step_index = 0;
  } else {
    ui.tour_step_index = std::clamp(ui.tour_step_index, 0, t.step_count - 1);
  }

  ui.tour_dim_alpha = std::clamp(ui.tour_dim_alpha, 0.05f, 0.95f);
}

struct RuntimeState {
  int last_tour{-1};
  int last_step{-1};
  std::string pending_focus;
  int focus_attempts{0};
};

RuntimeState& rt() {
  static RuntimeState s;
  return s;
}

void announce_step(const TourDef& tour, int step_idx, const TourStepDef& step) {
  ScreenReader& sr = ScreenReader::instance();
  if (!sr.enabled() || !sr.speak_windows()) return;

  const std::string msg = std::string("Tour: ") + tour.name + ". Step " + std::to_string(step_idx + 1) + "/" +
                          std::to_string(tour.step_count) + ": " + (step.title ? step.title : "");
  sr.speak(msg, true);
}

bool find_window_rect(const char* window_name, ImRect* out) {
  if (!window_name || !window_name[0]) return false;
  ImGuiWindow* w = ImGui::FindWindowByName(window_name);
  if (!w || !w->Active) return false;
  *out = ImRect(w->Pos, ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y));
  return true;
}

void draw_mask_window(const char* name, const ImVec2& pos, const ImVec2& size, ImGuiViewport* vp, float alpha,
                      bool block_inputs) {
  if (size.x <= 1.0f || size.y <= 1.0f) return;

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                           ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

  if (!block_inputs) flags |= ImGuiWindowFlags_NoInputs;

  ImGui::SetNextWindowPos(pos);
  ImGui::SetNextWindowSize(size);
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::SetNextWindowBgAlpha(alpha);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
  ImGui::Begin(name, nullptr, flags);
  ImGui::End();
  ImGui::PopStyleColor(1);
  ImGui::PopStyleVar(2);
}

ImVec2 pick_panel_pos(const ImRect& viewport, const ImRect* spot, float w, float h_guess) {
  const float pad = 18.0f;

  auto clamp_pos = [&](ImVec2 p) {
    p.x = std::clamp(p.x, viewport.Min.x + pad, viewport.Max.x - w - pad);
    p.y = std::clamp(p.y, viewport.Min.y + pad, viewport.Max.y - h_guess - pad);
    return p;
  };

  if (!spot) {
    const ImVec2 center((viewport.Min.x + viewport.Max.x - w) * 0.5f, (viewport.Min.y + viewport.Max.y - h_guess) * 0.5f);
    return clamp_pos(center);
  }

  const ImVec2 smin = spot->Min;
  const ImVec2 smax = spot->Max;

  const ImVec2 candidates[] = {
      ImVec2(smax.x + pad, smin.y),                 // right-top
      ImVec2(smax.x + pad, smax.y - h_guess),       // right-bottom
      ImVec2(smin.x - pad - w, smin.y),             // left-top
      ImVec2(smin.x - pad - w, smax.y - h_guess),   // left-bottom
      ImVec2(smin.x, smax.y + pad),                 // bottom-left
      ImVec2(smin.x, smin.y - pad - h_guess),       // top-left
  };

  auto fits = [&](ImVec2 p) {
    return p.x >= viewport.Min.x + pad && p.y >= viewport.Min.y + pad && (p.x + w) <= viewport.Max.x - pad &&
           (p.y + h_guess) <= viewport.Max.y - pad;
  };

  for (const ImVec2 p : candidates) {
    if (fits(p)) return p;
  }

  return clamp_pos(candidates[0]);
}

void start_tour(UIState& ui, int idx, int step, bool hide_help) {
  ui.tour_active = true;
  ui.tour_active_index = idx;
  ui.tour_step_index = step;
  clamp_indices(ui);
  if (hide_help) ui.show_help_window = false;
}

}  // namespace

void guided_tour_preframe(UIState& ui) {
  // Keep indices valid even if user toggled the overlay from hotkeys.
  clamp_indices(ui);

  RuntimeState& s = rt();
  if (!ui.tour_active) {
    s.last_tour = -1;
    s.last_step = -1;
    s.pending_focus.clear();
    s.focus_attempts = 0;
    return;
  }

  const TourDef& tour = kTours[ui.tour_active_index];
  const TourStepDef& step = tour.steps[ui.tour_step_index];

  const bool changed = (ui.tour_active_index != s.last_tour) || (ui.tour_step_index != s.last_step);
  if (changed) {
    s.last_tour = ui.tour_active_index;
    s.last_step = ui.tour_step_index;

    if (step.ensure_visible) {
      step.ensure_visible(ui);
    }

    // When we change steps, try to focus the target window for a few frames.
    s.pending_focus = step.target_window ? step.target_window : "";
    s.focus_attempts = 8;

    announce_step(tour, ui.tour_step_index, step);
  }

  if (s.focus_attempts > 0 && !s.pending_focus.empty()) {
    // Public API: safe even if the window doesn't exist yet.
    ImGui::SetWindowFocus(s.pending_focus.c_str());
    s.focus_attempts--;
  }
}

void draw_guided_tour_overlay(UIState& ui) {
  if (!ui.tour_active) return;
  if (ImGui::GetCurrentContext() == nullptr) return;

  clamp_indices(ui);

  const TourDef& tour = kTours[ui.tour_active_index];
  const TourStepDef& step = tour.steps[ui.tour_step_index];

  // --- Keyboard shortcuts (overlay only) ---
  {
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput) {
      if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ui.tour_active = false;
        return;
      }
      if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        if (ui.tour_step_index > 0) ui.tour_step_index--;
      }
      if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        if (ui.tour_step_index + 1 < tour.step_count) {
          ui.tour_step_index++;
        } else {
          // Right arrow on the last step: finish.
          ui.tour_active = false;
          return;
        }
      }
      if (ImGui::IsKeyPressed(ImGuiKey_Home)) {
        ui.tour_step_index = 0;
      }
    }
  }

  // --- Spotlight target ---
  ImGuiViewport* vp = ImGui::GetMainViewport();
  const ImRect vp_rect(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y));

  ImRect target_rect;
  bool has_target = find_window_rect(step.target_window, &target_rect);

  const float spotlight_pad = 8.0f;
  if (has_target) {
    target_rect.Min.x -= spotlight_pad;
    target_rect.Min.y -= spotlight_pad;
    target_rect.Max.x += spotlight_pad;
    target_rect.Max.y += spotlight_pad;

    // Clamp to viewport.
    target_rect.Min.x = std::clamp(target_rect.Min.x, vp_rect.Min.x, vp_rect.Max.x);
    target_rect.Min.y = std::clamp(target_rect.Min.y, vp_rect.Min.y, vp_rect.Max.y);
    target_rect.Max.x = std::clamp(target_rect.Max.x, vp_rect.Min.x, vp_rect.Max.x);
    target_rect.Max.y = std::clamp(target_rect.Max.y, vp_rect.Min.y, vp_rect.Max.y);
  }

  // --- Dim mask windows (optional) ---
  const bool any_mask = ui.tour_dim_background || ui.tour_block_outside_spotlight;
  if (any_mask) {
    const float alpha = ui.tour_dim_background ? ui.tour_dim_alpha : 0.0f;
    const bool block = ui.tour_block_outside_spotlight;

    if (!has_target) {
      draw_mask_window("##tour_mask_full", vp_rect.Min, vp->Size, vp, alpha, block);
    } else {
      // Top
      draw_mask_window("##tour_mask_top", vp_rect.Min,
                       ImVec2(vp_rect.Max.x - vp_rect.Min.x, target_rect.Min.y - vp_rect.Min.y), vp, alpha, block);
      // Bottom
      draw_mask_window("##tour_mask_bottom", ImVec2(vp_rect.Min.x, target_rect.Max.y),
                       ImVec2(vp_rect.Max.x - vp_rect.Min.x, vp_rect.Max.y - target_rect.Max.y), vp, alpha, block);
      // Left
      draw_mask_window("##tour_mask_left", ImVec2(vp_rect.Min.x, target_rect.Min.y),
                       ImVec2(target_rect.Min.x - vp_rect.Min.x, target_rect.Max.y - target_rect.Min.y), vp, alpha,
                       block);
      // Right
      draw_mask_window("##tour_mask_right", ImVec2(target_rect.Max.x, target_rect.Min.y),
                       ImVec2(vp_rect.Max.x - target_rect.Max.x, target_rect.Max.y - target_rect.Min.y), vp, alpha,
                       block);
    }
  }

  // --- Spotlight outline ---
  if (has_target) {
    ImDrawList* fg = ImGui::GetForegroundDrawList(vp);

    ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
    accent.w = 0.85f;

    const float t = (float)ImGui::GetTime();
    const float pulse = 0.5f + 0.5f * std::sin(t * 2.7f);
    const float thickness = 2.0f + 1.5f * pulse;

    const float rounding = 8.0f;
    fg->AddRect(target_rect.Min, target_rect.Max, ImGui::GetColorU32(accent), rounding, 0, thickness);
  }

  // --- Step panel (always) ---
  const float panel_w = 420.0f;
  const float panel_h_guess = 280.0f;

  const ImVec2 panel_pos = pick_panel_pos(vp_rect, has_target ? &target_rect : nullptr, panel_w, panel_h_guess);

  ImGui::SetNextWindowPos(panel_pos, ImGuiCond_Always);
  ImGui::SetNextWindowSizeConstraints(ImVec2(panel_w, 0.0f), ImVec2(panel_w, vp->Size.y - 20.0f));
  ImGui::SetNextWindowBgAlpha(0.97f);

  ImGuiWindowFlags panel_flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize;

  ImVec2 panel_min, panel_max;

  if (ImGui::Begin("##guided_tour_panel", nullptr, panel_flags)) {
    panel_min = ImGui::GetWindowPos();
    const ImVec2 panel_size = ImGui::GetWindowSize();
    panel_max = ImVec2(panel_min.x + panel_size.x, panel_min.y + panel_size.y);

    ImGui::TextDisabled("Guided Tour");
    ImGui::SameLine();
    ImGui::TextUnformatted(tour.name);
    ImGui::SameLine();
    ImGui::TextDisabled("(%d/%d)", ui.tour_step_index + 1, tour.step_count);

    ImGui::SeparatorText(step.title ? step.title : "Step");
    if (step.body && step.body[0]) {
      ImGui::TextWrapped("%s", step.body);
    }

    if (step.target_window && step.target_window[0]) {
      ImGui::Separator();
      if (!has_target) {
        ImGui::TextDisabled("Target: %s (not visible)", step.target_window);
        if (step.ensure_visible) {
          if (ImGui::Button("Open target")) {
            step.ensure_visible(ui);
          }
          ImGui::SameLine();
        }
        if (ImGui::Button("Focus")) {
          ImGui::SetWindowFocus(step.target_window);
        }
      } else {
        ImGui::TextDisabled("Target: %s", step.target_window);
        if (ImGui::SmallButton("Focus")) {
          ImGui::SetWindowFocus(step.target_window);
        }
      }
    }

    if (step.doc_ref && step.doc_ref[0]) {
      ImGui::SameLine();
      if (ImGui::SmallButton("Open doc")) {
        ui.show_help_window = true;
        ui.request_help_tab = HelpTab::Docs;
        ui.request_open_doc_ref = step.doc_ref;
      }
    }

    // Options (so the user doesn't have to open Help while touring).
    if (ImGui::CollapsingHeader("Overlay options")) {
      ImGui::Checkbox("Dim background", &ui.tour_dim_background);
      ImGui::SliderFloat("Dim alpha", &ui.tour_dim_alpha, 0.10f, 0.95f, "%.2f");
      ImGui::Checkbox("Block outside spotlight", &ui.tour_block_outside_spotlight);
      ImGui::Checkbox("Pause toast popups", &ui.tour_pause_toasts);
    }

    ImGui::Separator();

    // Progress bar.
    const float frac = (tour.step_count <= 1) ? 1.0f : (float)(ui.tour_step_index + 1) / (float)tour.step_count;
    ImGui::ProgressBar(frac, ImVec2(-1, 0), nullptr);

    // Navigation buttons.
    const bool can_back = ui.tour_step_index > 0;
    const bool can_next = ui.tour_step_index + 1 < tour.step_count;

    if (!can_back) ImGui::BeginDisabled();
    if (ImGui::Button("Back")) {
      if (ui.tour_step_index > 0) ui.tour_step_index--;
    }
    if (!can_back) ImGui::EndDisabled();

    ImGui::SameLine();

    if (ImGui::Button(can_next ? "Next" : "Finish")) {
      if (can_next) {
        ui.tour_step_index++;
      } else {
        ui.tour_active = false;
      }
    }

    ImGui::SameLine();
    if (ImGui::Button("Exit")) {
      ui.tour_active = false;
    }

    ImGui::SameLine();
    if (ImGui::SmallButton("Tours...")) {
      ui.show_help_window = true;
      ui.request_help_tab = HelpTab::Tours;
    }

    ImGui::SameLine();
    if (ImGui::SmallButton("Docs")) {
      ui.show_help_window = true;
      ui.request_help_tab = HelpTab::Docs;
      ui.request_open_doc_ref = (tour.doc_ref && tour.doc_ref[0]) ? tour.doc_ref : "index.md";
    }

    ImGui::TextDisabled("Keys: Left/Right, Esc, F2");
  }
  ImGui::End();

  // --- Pointer line from panel to spotlight ---
  if (has_target) {
    ImDrawList* fg = ImGui::GetForegroundDrawList(vp);

    const ImRect panel_rect(panel_min, panel_max);
    const ImVec2 target_c((target_rect.Min.x + target_rect.Max.x) * 0.5f, (target_rect.Min.y + target_rect.Max.y) * 0.5f);

    // Choose a point on the panel edge closest to the target center.
    ImVec2 panel_p = target_c;
    panel_p.x = std::clamp(panel_p.x, panel_rect.Min.x, panel_rect.Max.x);
    panel_p.y = std::clamp(panel_p.y, panel_rect.Min.y, panel_rect.Max.y);

    // If the clamped point is inside the panel, snap to the nearest edge.
    if (panel_p.x > panel_rect.Min.x && panel_p.x < panel_rect.Max.x && panel_p.y > panel_rect.Min.y &&
        panel_p.y < panel_rect.Max.y) {
      const float dl = std::abs(panel_p.x - panel_rect.Min.x);
      const float dr = std::abs(panel_rect.Max.x - panel_p.x);
      const float dt = std::abs(panel_p.y - panel_rect.Min.y);
      const float db = std::abs(panel_rect.Max.y - panel_p.y);

      const float m = std::min(std::min(dl, dr), std::min(dt, db));
      if (m == dl) panel_p.x = panel_rect.Min.x;
      else if (m == dr) panel_p.x = panel_rect.Max.x;
      else if (m == dt) panel_p.y = panel_rect.Min.y;
      else panel_p.y = panel_rect.Max.y;
    }

    ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
    accent.w = 0.75f;
    const ImU32 col = ImGui::GetColorU32(accent);

    fg->AddLine(panel_p, target_c, col, 2.0f);

    // Arrow head.
    const ImVec2 dir = ImVec2(target_c.x - panel_p.x, target_c.y - panel_p.y);
    const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
    if (len > 1.0f) {
      const ImVec2 n = ImVec2(dir.x / len, dir.y / len);
      const ImVec2 perp = ImVec2(-n.y, n.x);

      const float ah = 10.0f;
      const ImVec2 tip = target_c;
      const ImVec2 b1 = ImVec2(tip.x - n.x * ah + perp.x * (ah * 0.55f), tip.y - n.y * ah + perp.y * (ah * 0.55f));
      const ImVec2 b2 = ImVec2(tip.x - n.x * ah - perp.x * (ah * 0.55f), tip.y - n.y * ah - perp.y * (ah * 0.55f));

      fg->AddTriangleFilled(tip, b1, b2, col);
    }
  }
}

void draw_help_tours_tab(UIState& ui) {
  clamp_indices(ui);

  static int selected = 0;
  selected = std::clamp(selected, 0, tour_count() - 1);

  ImGui::TextWrapped(
      "Guided Tours are a UI-only onboarding overlay. They can spotlight panels, open required windows and provide "
      "short instructions.");
  ImGui::TextDisabled("Tip: press F2 to toggle the tour overlay.");

  if (ImGui::Button("Open tours.md")) {
    ui.request_help_tab = HelpTab::Docs;
    ui.request_open_doc_ref = "tours.md";
  }

  ImGui::Separator();

  // Global options.
  ImGui::Checkbox("Dim background", &ui.tour_dim_background);
  ImGui::SameLine();
  ImGui::SliderFloat("Alpha", &ui.tour_dim_alpha, 0.10f, 0.95f, "%.2f");
  ImGui::Checkbox("Block outside spotlight", &ui.tour_block_outside_spotlight);
  ImGui::Checkbox("Pause toast popups", &ui.tour_pause_toasts);

  ImGui::Separator();

  const float list_w = 240.0f;
  ImGui::BeginChild("tour_list", ImVec2(list_w, 0.0f), true);
  for (int i = 0; i < tour_count(); ++i) {
    const TourDef& t = kTours[i];
    std::string label = t.name;
    if (ui.tour_active && ui.tour_active_index == i) {
      label += "  [active]";
    }
    if (ImGui::Selectable(label.c_str(), selected == i)) {
      selected = i;
    }
  }
  ImGui::EndChild();

  ImGui::SameLine();

  ImGui::BeginChild("tour_details", ImVec2(0.0f, 0.0f), true);
  if (const TourDef* t = get_tour(selected)) {
    ImGui::Text("%s", t->name);
    ImGui::Separator();
    ImGui::TextWrapped("%s", t->blurb);

    if (t->doc_ref && t->doc_ref[0]) {
      if (ImGui::SmallButton("Open tour doc")) {
        ui.request_help_tab = HelpTab::Docs;
        ui.request_open_doc_ref = t->doc_ref;
      }
    }

    ImGui::Spacing();

    const bool is_active = ui.tour_active && ui.tour_active_index == selected;

    if (ImGui::Button(is_active ? "Resume" : "Start")) {
      start_tour(ui, selected, 0, true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Start (keep Help open)")) {
      start_tour(ui, selected, 0, false);
    }

    if (is_active) {
      ImGui::SameLine();
      if (ImGui::Button("Stop")) {
        ui.tour_active = false;
      }

      ImGui::TextDisabled("Progress: step %d/%d", ui.tour_step_index + 1, t->step_count);
    }

    ImGui::SeparatorText("Steps");

    for (int sidx = 0; sidx < t->step_count; ++sidx) {
      const TourStepDef& s = t->steps[sidx];
      ImGui::PushID(sidx);
      const bool cur = is_active && ui.tour_step_index == sidx;
      if (ImGui::Selectable(s.title ? s.title : "(unnamed)", cur)) {
        // Jump to step. Keep help open so the user can preview step content.
        start_tour(ui, selected, sidx, false);
      }
      ImGui::PopID();
    }
  }
  ImGui::EndChild();
}

}  // namespace nebula4x::ui
