#include "ui/hotkeys_window.h"

#include <imgui.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include "nebula4x/util/strings.h"

#include "ui/hotkeys.h"
#include "ui/panels.h"

namespace nebula4x::ui {

namespace {

bool contains_ci(std::string_view hay, std::string_view needle) {
  if (needle.empty()) return true;
  return nebula4x::to_lower(std::string(hay)).find(nebula4x::to_lower(std::string(needle))) !=
         std::string::npos;
}

}  // namespace

void draw_hotkeys_settings_tab(UIState& ui, UIPrefActions& actions) {
  static char filter[128] = {0};
  static bool show_only_overrides = false;
  static bool show_only_conflicts = false;
  static std::string status;
  static std::string last_error;

  // Clear capture state if we are not waiting for a chord anymore.
  ui.hotkeys_capture_active = !ui.hotkeys_capture_id.empty();

  ImGui::SeparatorText("Global hotkeys");
  ImGui::Checkbox("Enable global hotkeys", &ui.hotkeys_enabled);
  ImGui::SameLine();
  ImGui::TextDisabled("(%zu overrides)", ui.hotkey_overrides.size());

  ImGui::Spacing();

  if (ImGui::Button("Reset all to defaults")) {
    hotkeys_reset_all(ui);
    status = "Hotkeys reset to defaults.";
    last_error.clear();
  }
  ImGui::SameLine();
  if (ImGui::Button("Copy hotkeys")) {
    const std::string txt = export_hotkeys_text(ui);
    ImGui::SetClipboardText(txt.c_str());
    status = "Copied hotkeys to clipboard.";
    last_error.clear();
  }
  ImGui::SameLine();
  if (ImGui::Button("Paste hotkeys")) {
    const char* clip = ImGui::GetClipboardText();
    if (!clip) {
      last_error = "Clipboard is empty.";
      status.clear();
    } else {
      std::string err;
      if (!import_hotkeys_text(ui, clip, &err)) {
        last_error = err.empty() ? "Failed to import hotkeys." : err;
        status.clear();
      } else {
        status = "Imported hotkeys from clipboard.";
        last_error.clear();
      }
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Save UI prefs")) {
    actions.save_ui_prefs = true;
    status = "Queued UI prefs save.";
    last_error.clear();
  }

  if (!status.empty()) {
    ImGui::Spacing();
    ImGui::TextDisabled("%s", status.c_str());
  }
  if (!last_error.empty()) {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1), "%s", last_error.c_str());
  }

  ImGui::Spacing();
  ImGui::SeparatorText("Filter");
  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputText("##hotkey_filter", filter, sizeof(filter));
  const std::string q = nebula4x::to_lower(std::string(filter));

  ImGui::Checkbox("Show only overrides", &show_only_overrides);
  ImGui::SameLine();
  ImGui::Checkbox("Show only conflicts", &show_only_conflicts);

  // Conflict detection (computed from effective chords).
  std::unordered_map<std::string, std::vector<std::string>> chord_to_ids;
  chord_to_ids.reserve(hotkey_defs().size());
  for (const auto& d : hotkey_defs()) {
    const std::string s = hotkey_to_string(hotkey_get(ui, d.id));
    if (s.empty()) continue;
    chord_to_ids[s].push_back(d.id);
  }

  if (!ui.hotkeys_capture_id.empty()) {
    ImGui::Spacing();
    ImGui::SeparatorText("Capturing");
    ImGui::TextWrapped("Press the new chord for: %s", ui.hotkeys_capture_id.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Cancel")) {
      ui.hotkeys_capture_id.clear();
      ui.hotkeys_capture_active = false;
      status = "Cancelled capture.";
      last_error.clear();
    }

    HotkeyChord captured;
    bool cancelled = false;
    if (capture_hotkey_chord(&captured, &cancelled)) {
      hotkey_set(ui, ui.hotkeys_capture_id, captured);
      status = std::string("Bound ") + ui.hotkeys_capture_id + " to " +
               (hotkey_to_string(captured).empty() ? std::string("Unbound") : hotkey_to_string(captured));
      last_error.clear();
      ui.hotkeys_capture_id.clear();
      ui.hotkeys_capture_active = false;
    } else if (cancelled) {
      ui.hotkeys_capture_id.clear();
      ui.hotkeys_capture_active = false;
      status = "Cancelled capture.";
      last_error.clear();
    }
  }

  ImGui::Spacing();
  ImGui::SeparatorText("Bindings");

  const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                             ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
  if (ImGui::BeginTable("##hotkeys_table", 6, tf, ImVec2(0, 0))) {
    ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 140.0f);
    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Binding", ImGuiTableColumnFlags_WidthFixed, 140.0f);
    ImGui::TableSetupColumn("Default", ImGuiTableColumnFlags_WidthFixed, 140.0f);
    ImGui::TableSetupColumn("Conflict", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("Edit", ImGuiTableColumnFlags_WidthFixed, 220.0f);
    ImGui::TableHeadersRow();

    for (const auto& d : hotkey_defs()) {
      const HotkeyChord cur = hotkey_get(ui, d.id);
      const HotkeyChord def = d.default_chord;

      const bool is_override = (ui.hotkey_overrides.find(d.id) != ui.hotkey_overrides.end());
      if (show_only_overrides && !is_override) continue;

      const std::string cur_s = hotkey_to_string(cur);
      const std::string def_s = hotkey_to_string(def);
      const bool conflict = !cur_s.empty() && chord_to_ids[cur_s].size() > 1;
      if (show_only_conflicts && !conflict) continue;

      const std::string hay = std::string(d.category) + " " + d.label + " " + cur_s + " " + def_s;
      if (!q.empty() && !contains_ci(hay, q)) continue;

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(d.category);

      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(d.label);
      if (d.description && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", d.description);
      }

      ImGui::TableSetColumnIndex(2);
      if (cur_s.empty()) {
        ImGui::TextDisabled("Unbound");
      } else if (is_override) {
        ImGui::Text("%s", cur_s.c_str());
      } else {
        ImGui::TextDisabled("%s", cur_s.c_str());
      }

      ImGui::TableSetColumnIndex(3);
      if (def_s.empty()) ImGui::TextDisabled("Unbound");
      else ImGui::TextDisabled("%s", def_s.c_str());

      ImGui::TableSetColumnIndex(4);
      if (conflict) {
        ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "Yes");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
          const auto& ids = chord_to_ids[cur_s];
          std::string tip = "Conflicts with:\n";
          for (const auto& id : ids) {
            if (id == d.id) continue;
            tip += "  - " + id + "\n";
          }
          ImGui::SetTooltip("%s", tip.c_str());
        }
      } else {
        ImGui::TextDisabled("-");
      }

      ImGui::TableSetColumnIndex(5);
      ImGui::PushID(d.id);

      const bool capturing_this = (ui.hotkeys_capture_id == d.id);
      if (capturing_this) {
        ImGui::BeginDisabled();
      }
      if (ImGui::SmallButton("Rebind")) {
        ui.hotkeys_capture_id = d.id;
        ui.hotkeys_capture_active = true;
        status = std::string("Capturing: ") + d.id;
        last_error.clear();
      }
      if (capturing_this) {
        ImGui::EndDisabled();
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Clear")) {
        HotkeyChord none;
        hotkey_set(ui, d.id, none);
        status = std::string("Cleared: ") + d.id;
        last_error.clear();
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Reset")) {
        hotkey_reset(ui, d.id);
        status = std::string("Reset: ") + d.id;
        last_error.clear();
      }

      ImGui::PopID();
    }

    ImGui::EndTable();
  }
}

}  // namespace nebula4x::ui
