#include "ui/state_doctor_window.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nebula4x/core/serialization.h"
#include "nebula4x/core/state_validation.h"
#include "nebula4x/util/digest.h"
#include "nebula4x/util/file_io.h"
#include "nebula4x/util/json_merge_patch.h"
#include "nebula4x/util/log.h"

namespace nebula4x::ui {

namespace {

struct StateDoctorState {
  bool initialized{false};

  // Re-run automatically when the loaded state is replaced (new game / load / apply fix).
  bool auto_refresh_on_state_change{true};
  bool auto_run_on_open{true};

  char filter[128]{};

  std::uint64_t last_seen_state_generation{0};
  bool has_results{false};
  std::vector<std::string> errors;
  int selected_index{-1};

  // Fix preview (does not mutate the live game state).
  bool has_preview{false};
  nebula4x::FixReport preview_report;
  std::vector<std::string> preview_errors_after;

  std::string preview_fixed_json;
  std::string preview_merge_patch_json;

  std::string preview_before_digest_hex;
  // Digest breakdown (debug helper).
  bool has_digest_report{false};
  std::uint64_t digest_report_state_generation{0};
  nebula4x::GameStateDigestReport64 digest_report;

  std::string preview_after_digest_hex;

  // Export.
  char export_fixed_path[256]{};
  char export_patch_path[256]{};
  std::string last_status;
  std::string last_error;
};

StateDoctorState& st() {
  static StateDoctorState s;
  return s;
}

std::string to_lower_copy(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return out;
}

bool contains_case_insensitive(std::string_view hay, std::string_view needle) {
  if (needle.empty()) return true;
  const std::string h = to_lower_copy(hay);
  const std::string n = to_lower_copy(needle);
  return h.find(n) != std::string::npos;
}

std::string digest_hex_for_state(const nebula4x::GameState& gs) {
  nebula4x::DigestOptions opt;
  opt.include_events = false;
  opt.include_ui_state = false;
  return nebula4x::digest64_to_hex(nebula4x::digest_game_state64(gs, opt));
}

nebula4x::GameStateDigestReport64 digest_report_for_state(const nebula4x::GameState& gs) {
  nebula4x::DigestOptions opt;
  opt.include_events = false;
  opt.include_ui_state = false;
  return nebula4x::digest_game_state64_report(gs, opt);
}


void clear_preview(StateDoctorState& s) {
  s.has_preview = false;
  s.preview_report = {};
  s.preview_errors_after.clear();
  s.preview_fixed_json.clear();
  s.preview_merge_patch_json.clear();
  s.preview_before_digest_hex.clear();
  s.preview_after_digest_hex.clear();
}

void clear_digest_report(StateDoctorState& s) {
  s.has_digest_report = false;
  s.digest_report_state_generation = 0;
  s.digest_report = {};
}


void run_validation(nebula4x::Simulation& sim, StateDoctorState& s) {
  s.last_error.clear();
  s.last_status.clear();
  clear_preview(s);

  try {
    s.errors = nebula4x::validate_game_state(sim.state(), &sim.content());
    s.last_seen_state_generation = sim.state_generation();
    s.has_results = true;
    s.selected_index = -1;

    s.last_status =
        "Validation complete (" + std::to_string(s.errors.size()) + (s.errors.size() == 1 ? " error)." : " errors).");
  } catch (const std::exception& e) {
    s.last_error = e.what();
    s.has_results = false;
    s.errors.clear();
  }
}

void run_digest_breakdown(nebula4x::Simulation& sim, StateDoctorState& s) {
  s.last_error.clear();
  s.last_status.clear();

  try {
    s.digest_report = digest_report_for_state(sim.state());
    s.digest_report_state_generation = sim.state_generation();
    s.has_digest_report = true;

    s.last_status = "Digest breakdown computed (" + std::to_string(s.digest_report.parts.size()) + " sections).";
  } catch (const std::exception& e) {
    s.last_error = e.what();
    s.last_status.clear();
    s.has_digest_report = false;
    s.digest_report = {};
  }
}


void run_preview_fix(nebula4x::Simulation& sim, StateDoctorState& s) {
  s.last_error.clear();
  s.last_status.clear();
  clear_preview(s);

  try {
    const std::string before_json = nebula4x::serialize_game_to_json(sim.state());
    s.preview_before_digest_hex = digest_hex_for_state(sim.state());

    nebula4x::GameState fixed = sim.state(); // copy for safety
    s.preview_report = nebula4x::fix_game_state(fixed, &sim.content());
    s.preview_errors_after = nebula4x::validate_game_state(fixed, &sim.content());
    s.preview_after_digest_hex = digest_hex_for_state(fixed);

    s.preview_fixed_json = nebula4x::serialize_game_to_json(fixed);
    s.preview_merge_patch_json = nebula4x::diff_json_merge_patch(before_json, s.preview_fixed_json, /*indent=*/2);

    s.has_preview = true;

    s.last_status = "Fix preview ready (" + std::to_string(s.preview_report.changes) + " change" +
                    (s.preview_report.changes == 1 ? "" : "s") + ", " +
                    std::to_string(s.preview_errors_after.size()) + " error" +
                    (s.preview_errors_after.size() == 1 ? "" : "s") + " after fix).";
  } catch (const std::exception& e) {
    s.last_error = e.what();
  }
}

void apply_fix(nebula4x::Simulation& sim, StateDoctorState& s) {
  s.last_error.clear();
  s.last_status.clear();

  try {
    nebula4x::GameState fixed = sim.state(); // copy
    const auto report = nebula4x::fix_game_state(fixed, &sim.content());

    // Replace the live state so the Simulation rebuilds derived caches and the UI
    // clears stale selections (via state_generation).
    sim.load_game(std::move(fixed));
    clear_digest_report(s);

    // Re-validate the now-loaded state.
    run_validation(sim, s);

    s.last_status =
        "Applied fixer (" + std::to_string(report.changes) + " change" + (report.changes == 1 ? "" : "s") + ").";
    if (!report.actions.empty()) {
      nebula4x::log::info("StateDoctor: applied fixer actions=" + std::to_string(report.actions.size()));
    }
  } catch (const std::exception& e) {
    s.last_error = e.what();
  }
}

} // namespace

void draw_state_doctor_window(Simulation& sim, UIState& ui) {
  auto& s = st();
  if (!s.initialized) {
    std::strncpy(s.export_fixed_path, "fixed_save.json", sizeof(s.export_fixed_path) - 1);
    std::strncpy(s.export_patch_path, "fix_merge_patch.json", sizeof(s.export_patch_path) - 1);
    s.initialized = true;
  }

  // Auto-run on first open.
  if (ui.show_state_doctor_window && s.auto_run_on_open && !s.has_results) {
    run_validation(sim, s);
  }

  // Auto-refresh when the simulation replaces the state (load/new-game/etc).
  if (ui.show_state_doctor_window && s.auto_refresh_on_state_change &&
      s.last_seen_state_generation != sim.state_generation()) {
    clear_digest_report(s);
    run_validation(sim, s);
  }

  ImGui::SetNextWindowSize(ImVec2(980, 720), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("State Doctor", &ui.show_state_doctor_window)) {
    ImGui::End();
    return;
  }

  ImGui::Text("State generation: %llu",
              static_cast<unsigned long long>(static_cast<std::uint64_t>(sim.state_generation())));
  ImGui::SameLine();
  const std::string digest_hex = digest_hex_for_state(sim.state());
  ImGui::Text("Gameplay digest: %s", digest_hex.c_str());

  if (!s.last_error.empty()) {
    ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Error: %s", s.last_error.c_str());
  } else if (!s.last_status.empty()) {
    ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1), "%s", s.last_status.c_str());
  }

  ImGui::SeparatorText("Digest breakdown");
  const bool digest_stale = s.has_digest_report && (s.digest_report_state_generation != sim.state_generation());
  if (digest_stale) {
    ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "Breakdown is stale (state changed). Recompute.");
  }

  if (ImGui::Button("Compute breakdown")) run_digest_breakdown(sim, s);
  ImGui::SameLine();
  ImGui::TextUnformatted("Splits the state digest by subsystem (helps localize mismatches).");

  if (s.has_digest_report && !digest_stale) {
    const std::string overall_hex = nebula4x::digest64_to_hex(s.digest_report.overall);
    ImGui::Text("Report overall: %s", overall_hex.c_str());

    const float table_height = std::max(140.0f, ImGui::GetContentRegionAvail().y * 0.18f);
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("digest_breakdown_table", 3, flags, ImVec2(0, table_height))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Part", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 70.0f);
      ImGui::TableSetupColumn("Digest", ImGuiTableColumnFlags_WidthFixed, 160.0f);
      ImGui::TableHeadersRow();

      for (std::size_t i = 0; i < s.digest_report.parts.size(); ++i) {
        const auto& p = s.digest_report.parts[i];
        const std::string hex = nebula4x::digest64_to_hex(p.digest);

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(p.label.c_str());

        ImGui::TableNextColumn();
        ImGui::Text("%llu", static_cast<unsigned long long>(p.element_count));

        ImGui::TableNextColumn();
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable(hex.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
          ImGui::SetClipboardText(hex.c_str());
          s.last_status = std::string("Copied digest for ") + p.label + " to clipboard.";
          s.last_error.clear();
        }
        ImGui::PopID();
      }

      ImGui::EndTable();
    }
  } else {
    ImGui::TextUnformatted("No breakdown computed yet.");
  }

  ImGui::SeparatorText("Validate");
  if (ImGui::Button("Run validation")) run_validation(sim, s);
  ImGui::SameLine();
  ImGui::Checkbox("Auto-run on open", &s.auto_run_on_open);
  ImGui::SameLine();
  ImGui::Checkbox("Auto-refresh on state change", &s.auto_refresh_on_state_change);

  ImGui::SeparatorText("Filter");
  ImGui::SetNextItemWidth(520.0f);
  ImGui::InputText("Search", s.filter, sizeof(s.filter));

  // Visible rows.
  std::vector<int> visible;
  visible.reserve(s.errors.size());
  for (int i = 0; i < static_cast<int>(s.errors.size()); ++i) {
    const std::string_view needle{s.filter};
    if (!needle.empty() && !contains_case_insensitive(s.errors[i], needle)) continue;
    visible.push_back(i);
  }

  ImGui::Text("Errors: %d (visible %d)", static_cast<int>(s.errors.size()), static_cast<int>(visible.size()));

  ImGui::SeparatorText("Issues");
  if (!s.has_results) {
    ImGui::TextUnformatted("No validation results yet. Click \"Run validation\".");
  } else if (s.errors.empty()) {
    ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1), "No state validation errors detected.");
  } else {
    const float table_height = std::max(220.0f, ImGui::GetContentRegionAvail().y * 0.35f);
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_Reorderable | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("state_doctor_table", 2, flags, ImVec2(0, table_height))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 50.0f);
      ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableHeadersRow();

      for (int idx : visible) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%d", idx);

        ImGui::TableNextColumn();
        ImGui::PushID(idx);
        const bool selected = (s.selected_index == idx);
        if (ImGui::Selectable(s.errors[idx].c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
          s.selected_index = idx;
        }
        ImGui::PopID();
      }

      ImGui::EndTable();
    }

    if (s.selected_index >= 0 && s.selected_index < static_cast<int>(s.errors.size())) {
      ImGui::SeparatorText("Selected");
      ImGui::TextWrapped("%s", s.errors[s.selected_index].c_str());
      if (ImGui::Button("Copy selected")) {
        ImGui::SetClipboardText(s.errors[s.selected_index].c_str());
        s.last_status = "Copied selected error to clipboard.";
        s.last_error.clear();
      }
    }
  }

  ImGui::SeparatorText("Fix / Repair");
  ImGui::TextWrapped(
      "The fixer is conservative and aims to repair common save integrity issues (missing ids, invalid references, "
      "unsorted lists). Use Preview first to inspect what will change.");

  if (ImGui::Button("Preview fixer (safe)")) run_preview_fix(sim, s);
  ImGui::SameLine();
  if (ImGui::Button("Apply fixer to game state")) apply_fix(sim, s);

  if (s.has_preview) {
    ImGui::SeparatorText("Preview summary");
    ImGui::Text("Fixer changes: %d", s.preview_report.changes);
    ImGui::SameLine();
    ImGui::Text("Errors after fix: %d", static_cast<int>(s.preview_errors_after.size()));

    if (!s.preview_before_digest_hex.empty() || !s.preview_after_digest_hex.empty()) {
      ImGui::Text("Digest before: %s", s.preview_before_digest_hex.c_str());
      ImGui::Text("Digest after:  %s", s.preview_after_digest_hex.c_str());
    }

    if (ImGui::CollapsingHeader("Fix actions", ImGuiTreeNodeFlags_DefaultOpen)) {
      const float h = std::min(220.0f, ImGui::GetContentRegionAvail().y * 0.30f);
      if (ImGui::BeginChild("state_doctor_actions", ImVec2(0, h), true)) {
        if (s.preview_report.actions.empty()) {
          ImGui::TextUnformatted("(No actions recorded.)");
        } else {
          for (std::size_t i = 0; i < s.preview_report.actions.size(); ++i) {
            ImGui::BulletText("%s", s.preview_report.actions[i].c_str());
          }
        }
      }
      ImGui::EndChild();
    }

    if (ImGui::CollapsingHeader("Errors after fix")) {
      if (s.preview_errors_after.empty()) {
        ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1), "No validation errors after fix.");
      } else {
        const float h = std::min(200.0f, ImGui::GetContentRegionAvail().y * 0.22f);
        if (ImGui::BeginChild("state_doctor_after_errors", ImVec2(0, h), true)) {
          for (const auto& e : s.preview_errors_after) ImGui::BulletText("%s", e.c_str());
        }
        ImGui::EndChild();
      }
    }

    if (ImGui::CollapsingHeader("Merge patch (RFC 7386)")) {
      if (ImGui::Button("Copy merge patch")) {
        ImGui::SetClipboardText(s.preview_merge_patch_json.c_str());
        s.last_status = "Copied merge patch to clipboard.";
        s.last_error.clear();
      }
      ImGui::SameLine();
      if (ImGui::Button("Copy fixed save JSON")) {
        ImGui::SetClipboardText(s.preview_fixed_json.c_str());
        s.last_status = "Copied fixed save JSON to clipboard.";
        s.last_error.clear();
      }

      ImGui::InputText("Patch path", s.export_patch_path, sizeof(s.export_patch_path));
      if (ImGui::Button("Save merge patch")) {
        try {
          nebula4x::write_text_file(s.export_patch_path, s.preview_merge_patch_json);
          s.last_status = std::string("Wrote ") + s.export_patch_path;
          s.last_error.clear();
        } catch (const std::exception& e) {
          s.last_error = e.what();
          s.last_status.clear();
        }
      }

      ImGui::InputText("Fixed save path", s.export_fixed_path, sizeof(s.export_fixed_path));
      if (ImGui::Button("Save fixed save (preview)")) {
        try {
          nebula4x::write_text_file(s.export_fixed_path, s.preview_fixed_json);
          s.last_status = std::string("Wrote ") + s.export_fixed_path;
          s.last_error.clear();
        } catch (const std::exception& e) {
          s.last_error = e.what();
          s.last_status.clear();
        }
      }

      const float h = std::max(160.0f, ImGui::GetContentRegionAvail().y * 0.40f);
      if (ImGui::BeginChild("state_doctor_patch_view", ImVec2(0, h), true, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::TextUnformatted(s.preview_merge_patch_json.c_str());
      }
      ImGui::EndChild();
    }
  }

  ImGui::End();
}

} // namespace nebula4x::ui
