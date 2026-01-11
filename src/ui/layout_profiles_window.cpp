#include "ui/layout_profiles_window.h"

#include <cstdio>
#include <filesystem>

#include <imgui.h>

#include "nebula4x/util/log.h"

#include "ui/layout_profiles.h"

namespace nebula4x::ui {
namespace {

// Small helper: copy std::string into a fixed-size C buffer.
template <std::size_t N>
void copy_into(char (&dst)[N], const std::string& s) {
  std::snprintf(dst, N, "%s", s.c_str());
  dst[N - 1] = '\0';
}

// Applies a window-visibility "workspace" preset.
void apply_workspace_preset(const char* preset, UIState& ui) {
  const std::string p = preset ? std::string(preset) : std::string();

  auto hide_all_major = [&]() {
    ui.show_controls_window = false;
    ui.show_map_window = false;
    ui.show_details_window = false;
    ui.show_directory_window = false;
    ui.show_production_window = false;
    ui.show_economy_window = false;
    ui.show_planner_window = false;
    ui.show_freight_window = false;
    ui.show_fuel_window = false;
    ui.show_sustainment_window = false;
    ui.show_time_warp_window = false;
    ui.show_timeline_window = false;
    ui.show_design_studio_window = false;
    ui.show_balance_lab_window = false;
    ui.show_intel_window = false;
    ui.show_diplomacy_window = false;
    ui.show_save_tools_window = false;
  };

  if (p == "Default") {
    hide_all_major();
    ui.show_controls_window = true;
    ui.show_map_window = true;
    ui.show_details_window = true;
    ui.show_directory_window = true;
    ui.show_status_bar = true;
    return;
  }

  if (p == "Minimal") {
    hide_all_major();
    ui.show_map_window = true;
    ui.show_details_window = true;
    ui.show_status_bar = true;
    return;
  }

  if (p == "Economy") {
    hide_all_major();
    ui.show_map_window = true;
    ui.show_details_window = true;
    ui.show_directory_window = true;
    ui.show_production_window = true;
    ui.show_economy_window = true;
    ui.show_planner_window = true;
    ui.show_timeline_window = true;
    ui.show_status_bar = true;
    return;
  }

  if (p == "Design") {
    hide_all_major();
    ui.show_map_window = true;
    ui.show_details_window = true;
    ui.show_design_studio_window = true;
    ui.show_balance_lab_window = true;
    ui.show_status_bar = true;
    return;
  }

  if (p == "Intel") {
    hide_all_major();
    ui.show_map_window = true;
    ui.show_details_window = true;
    ui.show_intel_window = true;
    ui.show_diplomacy_window = true;
    ui.show_timeline_window = true;
    ui.show_status_bar = true;
    return;
  }
}

}  // namespace

void draw_layout_profiles_window(UIState& ui) {
  if (!ui.show_layout_profiles_window) return;

  ImGui::SetNextWindowSize(ImVec2(620, 520), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Layout Profiles", &ui.show_layout_profiles_window)) {
    ImGui::End();
    return;
  }

  const std::string active_name = sanitize_layout_profile_name(ui.layout_profile);
  if (active_name != ui.layout_profile) {
    copy_into(ui.layout_profile, active_name);
  }
  const std::string dir = (ui.layout_profiles_dir[0] ? std::string(ui.layout_profiles_dir) : std::string("ui_layouts"));
  if (dir != ui.layout_profiles_dir) {
    copy_into(ui.layout_profiles_dir, dir);
  }
  const std::string active_path = make_layout_profile_ini_path(ui.layout_profiles_dir, ui.layout_profile);

  // Ensure profile dir exists so ImGui can save its ini file.
  {
    std::string err;
    if (!ensure_layout_profile_dir(ui.layout_profiles_dir, &err)) {
      ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1), "Layout directory error: %s", err.c_str());
    }
  }

  ImGui::SeparatorText("Active");
  ImGui::Text("Profile: %s", ui.layout_profile);
  ImGui::TextDisabled("Ini file: %s", active_path.c_str());
  {
    const char* ini = ImGui::GetIO().IniFilename;
    ImGui::TextDisabled("ImGui IO.IniFilename: %s", (ini && ini[0]) ? ini : "(none)");
  }

  if (!ui.layout_profile_status.empty()) {
    ImGui::Spacing();
    ImGui::TextWrapped("%s", ui.layout_profile_status.c_str());
  }

  ImGui::Spacing();
  if (ImGui::Button("Save current layout to active profile")) {
    try {
      ImGui::SaveIniSettingsToDisk(active_path.c_str());
      ui.layout_profile_status = std::string("Saved layout to ") + active_path;
    } catch (const std::exception& e) {
      ui.layout_profile_status = std::string("Save failed: ") + e.what();
      nebula4x::log::warn(ui.layout_profile_status);
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Reload active profile")) {
    // Defer actual load to App::pre_frame (before NewFrame) for best docking results.
    ui.request_reload_layout_profile = true;
    ui.layout_profile_status = "Requested reload (will apply next frame).";
  }
  ImGui::SameLine();
  if (ImGui::Button("Reset active layout")) {
    // App::reset_window_layout_defaults() also clears ini in memory and forces
    // rebuilding the default dock layout.
    ui.request_reset_window_layout = true;
    ui.layout_profile_status = "Requested layout reset (default dock layout will rebuild next frame).";
  }

  ImGui::SeparatorText("Switch / Manage");
  ImGui::TextDisabled("Profiles directory");
  ImGui::InputText("##layout_dir", ui.layout_profiles_dir, IM_ARRAYSIZE(ui.layout_profiles_dir));
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    // The directory changed; reload next frame.
    ui.request_reload_layout_profile = true;
    ui.layout_profile_status = "Layout directory updated. Reload requested.";
  }

  const std::vector<std::string> profiles = scan_layout_profile_names(ui.layout_profiles_dir);
  if (profiles.empty()) {
    ImGui::TextDisabled("No saved profiles yet. Save one to create %s", active_path.c_str());
  }

  static int selected_idx = 0;
  // Keep selection stable and try to snap to active profile.
  if (!profiles.empty()) {
    selected_idx = std::clamp(selected_idx, 0, (int)profiles.size() - 1);
    for (int i = 0; i < (int)profiles.size(); ++i) {
      if (profiles[i] == ui.layout_profile) {
        selected_idx = i;
        break;
      }
    }
  }

  // Build labels for combo.
  std::vector<const char*> labels;
  labels.reserve(profiles.size());
  for (const auto& p : profiles) labels.push_back(p.c_str());

  if (!labels.empty()) {
    ImGui::SetNextItemWidth(240.0f);
    ImGui::Combo("Saved profiles", &selected_idx, labels.data(), (int)labels.size());
  }

  const std::string selected_name = (!profiles.empty() && selected_idx >= 0 && selected_idx < (int)profiles.size())
                                        ? profiles[(std::size_t)selected_idx]
                                        : active_name;
  const std::string selected_path = make_layout_profile_ini_path(ui.layout_profiles_dir, selected_name);
  ImGui::TextDisabled("Selected: %s", selected_path.c_str());

  if (ImGui::Button("Activate selected")) {
    copy_into(ui.layout_profile, selected_name);
    ui.request_reload_layout_profile = true;
    ui.layout_profile_status = std::string("Switched to profile '") + ui.layout_profile + "' (will apply next frame).";
  }
  ImGui::SameLine();
  if (ImGui::Button("Duplicate selected -> active")) {
    try {
      // Load selected into memory next frame by switching, but first ensure it exists.
      if (std::filesystem::exists(selected_path)) {
        // Just copy the file bytes to active path.
        std::error_code ec;
        std::filesystem::copy_file(selected_path, active_path, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
          ui.layout_profile_status = std::string("Duplicate failed: ") + ec.message();
        } else {
          ui.request_reload_layout_profile = true;
          ui.layout_profile_status = std::string("Duplicated '") + selected_name + "' -> active profile.";
        }
      } else {
        ui.layout_profile_status = "Selected profile file does not exist.";
      }
    } catch (const std::exception& e) {
      ui.layout_profile_status = std::string("Duplicate failed: ") + e.what();
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Delete selected")) {
    if (selected_name == active_name) {
      ui.layout_profile_status = "Refusing to delete the active profile. Switch to a different profile first.";
    } else {
      std::error_code ec;
      const bool ok = std::filesystem::remove(selected_path, ec);
      if (!ok || ec) {
        ui.layout_profile_status = std::string("Delete failed: ") + (ec ? ec.message() : "(unknown)");
      } else {
        ui.layout_profile_status = std::string("Deleted '") + selected_name + "'.";
      }
    }
  }

  ImGui::Spacing();
  ImGui::SeparatorText("Save As");
  static char new_name_buf[64] = "";
  ImGui::InputTextWithHint("##new_profile", "new profile name (e.g. Economy)", new_name_buf, IM_ARRAYSIZE(new_name_buf));
  if (ImGui::Button("Save As (and activate)")) {
    const std::string new_name = sanitize_layout_profile_name(new_name_buf);
    const std::string new_path = make_layout_profile_ini_path(ui.layout_profiles_dir, new_name);
    if (new_name.empty()) {
      ui.layout_profile_status = "Invalid profile name.";
    } else {
      try {
        ImGui::SaveIniSettingsToDisk(new_path.c_str());
        copy_into(ui.layout_profile, new_name);
        ui.request_reload_layout_profile = true;
        ui.layout_profile_status = std::string("Saved and activated '") + new_name + "'.";
        new_name_buf[0] = '\0';
      } catch (const std::exception& e) {
        ui.layout_profile_status = std::string("Save As failed: ") + e.what();
      }
    }
  }

  ImGui::SeparatorText("Workspace presets");
  ImGui::TextWrapped(
      "These presets toggle which windows are visible (they do NOT change the docking layout). Combine them with a saved layout profile for fast workflow switching.");

  if (ImGui::Button("Default")) apply_workspace_preset("Default", ui);
  ImGui::SameLine();
  if (ImGui::Button("Minimal")) apply_workspace_preset("Minimal", ui);
  ImGui::SameLine();
  if (ImGui::Button("Economy")) apply_workspace_preset("Economy", ui);
  ImGui::SameLine();
  if (ImGui::Button("Design")) apply_workspace_preset("Design", ui);
  ImGui::SameLine();
  if (ImGui::Button("Intel")) apply_workspace_preset("Intel", ui);

  ImGui::Spacing();
  ImGui::SeparatorText("Tips");
  ImGui::BulletText("Ctrl+Shift+L: open this window");
  ImGui::BulletText("If docking looks odd after switching, hit 'Reload active profile'.");

  ImGui::End();
}

}  // namespace nebula4x::ui
