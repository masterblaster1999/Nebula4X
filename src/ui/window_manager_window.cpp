#include "ui/window_manager_window.h"

#include <imgui.h>

#include <string>

#include "ui/window_management.h"

namespace nebula4x::ui {

void draw_window_manager_window(UIState& ui) {
  if (!ui.show_window_manager_window) return;

  ImGui::SetNextWindowSize(ImVec2(980, 720), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Window Manager", &ui.show_window_manager_window, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return;
  }

  ImGui::TextUnformatted("Declutter the main view by popping panels out into moveable windows.");
  ImGui::TextDisabled(
      "Tip: With multi-viewport enabled, drag a popup outside the main window to detach it into its own OS window.");

  // Quick actions.
  {
    const bool focus = focus_mode_enabled(ui);
    const char* focus_label = focus ? "Exit Focus Mode (Restore Windows)" : "Enter Focus Mode (Map Only)";
    if (ImGui::Button(focus_label)) {
      toggle_focus_mode(ui);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset Window Layout")) {
      ui.request_reset_window_layout = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Open Layout Profiles")) {
      ui.show_layout_profiles_window = true;
    }
  }

  ImGui::SeparatorText("Popup behavior");
  ImGui::Checkbox("Popup-first mode (new windows open floating)", &ui.window_popup_first_mode);
  ImGui::SameLine();
  ImGui::Checkbox("Auto-focus new popups", &ui.window_popup_auto_focus);
  ImGui::SliderFloat("Cascade step (px)", &ui.window_popup_cascade_step_px, 0.0f, 64.0f, "%.0f");

  if (ImGui::Button("Reset Per-Window Overrides")) {
    ui.window_launch_overrides.clear();
  }

  ImGui::SeparatorText("Windows");
  static ImGuiTextFilter filter;
  filter.Draw("Filter", 240);
  ImGui::SameLine();
  if (ImGui::SmallButton("Clear")) filter.Clear();

  const auto& specs = window_specs();

  const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                             ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;

  if (ImGui::BeginTable("##window_manager_table", 5, tf, ImVec2(0, 0))) {
    ImGui::TableSetupColumn("Open", ImGuiTableColumnFlags_WidthFixed, 48.0f);
    ImGui::TableSetupColumn("Window");
    ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 120.0f);
    ImGui::TableSetupColumn("Launch", ImGuiTableColumnFlags_WidthFixed, 160.0f);
    ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 170.0f);
    ImGui::TableHeadersRow();

    for (const auto& s : specs) {
      // Filter by either label or category.
      if (filter.IsActive()) {
        const bool hit = filter.PassFilter(s.label) || filter.PassFilter(s.category) || filter.PassFilter(s.id);
        if (!hit) continue;
      }

      ImGui::TableNextRow();

      bool open = ui.*(s.open_flag);
      ImGui::PushID(s.id);

      // Open checkbox.
      ImGui::TableSetColumnIndex(0);
      if (ImGui::Checkbox("##open", &open)) {
        ui.*(s.open_flag) = open;
      }

      // Label.
      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(s.label);

      // Category.
      ImGui::TableSetColumnIndex(2);
      ImGui::TextDisabled("%s", s.category);

      // Launch mode.
      ImGui::TableSetColumnIndex(3);
      if (!s.supports_popup) {
        ImGui::TextDisabled("Fixed");
      } else {
        const int override_val = [&]() -> int {
          auto it = ui.window_launch_overrides.find(s.id);
          return it == ui.window_launch_overrides.end() ? -1 : it->second;
        }();

        const WindowLaunchMode effective = effective_launch_mode(ui, s);
        std::string preview;
        if (override_val < 0) {
          preview = std::string("Default (") + (effective == WindowLaunchMode::Popup ? "Popup" : "Docked") + ")";
        } else {
          preview = (override_val != 0) ? "Popup" : "Docked";
        }

        if (ImGui::BeginCombo("##launch", preview.c_str())) {
          const std::string default_label =
              std::string("Default (") + (s.default_mode == WindowLaunchMode::Popup ? "Popup" : "Docked") + ")";

          if (ImGui::Selectable(default_label.c_str(), override_val < 0)) {
            ui.window_launch_overrides.erase(s.id);
          }
          if (ImGui::Selectable("Docked", override_val == 0)) {
            ui.window_launch_overrides[s.id] = 0;
          }
          if (ImGui::Selectable("Popup", override_val != 0 && override_val >= 0)) {
            ui.window_launch_overrides[s.id] = 1;
          }
          ImGui::EndCombo();
        }
      }

      // Actions.
      ImGui::TableSetColumnIndex(4);
      {
        ImGui::BeginDisabled(!open);
        if (ImGui::SmallButton("Focus")) {
          ImGui::SetWindowFocus(s.title);
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::SmallButton("Pop out")) {
          request_popout(ui, s.id);
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(!open);
        if (ImGui::SmallButton("Close")) {
          ui.*(s.open_flag) = false;
        }
        ImGui::EndDisabled();
      }

      ImGui::PopID();
    }

    ImGui::EndTable();
  }

  ImGui::End();
}

}  // namespace nebula4x::ui
