#include "ui/content_validation_window.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nebula4x/core/content_validation.h"
#include "nebula4x/util/file_io.h"
#include "nebula4x/util/json.h"
#include "nebula4x/util/log.h"

namespace nebula4x::ui {

namespace {

struct ContentValidationState {
  bool initialized{false};

  // Re-run automatically when content changes.
  bool auto_refresh_on_content_change{true};
  bool auto_run_on_open{true};

  bool show_errors{true};
  bool show_warnings{true};

  char filter[128]{};

  std::uint64_t last_seen_content_generation{0};
  bool has_results{false};
  std::vector<nebula4x::ContentIssue> issues;

  int selected_index{-1};

  // Export.
  char export_text_path[256]{};
  char export_json_path[256]{};
  std::string last_status;
  std::string last_error;
};

ContentValidationState& st() {
  static ContentValidationState s;
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
  // Tiny inputs: cheap lowercase copies are fine.
  const std::string h = to_lower_copy(hay);
  const std::string n = to_lower_copy(needle);
  return h.find(n) != std::string::npos;
}


template <std::size_t N>
void set_cstr(char (&dst)[N], std::string_view src) {
  static_assert(N > 0, "destination buffer must have positive size");
  const std::size_t n = std::min<std::size_t>(src.size(), N - 1);
  if (n > 0) std::memcpy(dst, src.data(), n);
  dst[n] = '\0';
}


std::string issues_to_text(const std::vector<nebula4x::ContentIssue>& issues, bool include_warnings) {
  std::string out;
  out.reserve(issues.size() * 96);

  for (const auto& is : issues) {
    if (is.severity == nebula4x::ContentIssueSeverity::Warning && !include_warnings) continue;

    const char sev = (is.severity == nebula4x::ContentIssueSeverity::Error) ? 'E' : 'W';

    out.push_back('[');
    out.push_back(sev);
    out.push_back(']');
    out.push_back(' ');

    if (!is.subject_kind.empty()) {
      out += is.subject_kind;
      if (!is.subject_id.empty()) {
        out.push_back(':');
        out += is.subject_id;
      }
      out += "  ";
    }

    if (!is.code.empty()) {
      out += is.code;
      out += "  ";
    }

    out += is.message;
    out.push_back('\n');
  }

  return out;
}

nebula4x::json::Value issues_to_json_value(const std::vector<nebula4x::ContentIssue>& issues, bool include_warnings) {
  nebula4x::json::Array arr;
  arr.reserve(issues.size());

  for (const auto& is : issues) {
    if (is.severity == nebula4x::ContentIssueSeverity::Warning && !include_warnings) continue;

    nebula4x::json::Object o;
    o["severity"] = nebula4x::to_string(is.severity);
    o["code"] = is.code;
    o["message"] = is.message;
    o["subject_kind"] = is.subject_kind;
    o["subject_id"] = is.subject_id;
    arr.push_back(nebula4x::json::object(std::move(o)));
  }

  return nebula4x::json::Value(std::move(arr));
}

void run_validation(nebula4x::Simulation& sim, ContentValidationState& s) {
  s.last_error.clear();
  s.last_status.clear();

  try {
    s.issues = nebula4x::validate_content_db_detailed(sim.content());
    s.last_seen_content_generation = sim.content_generation();
    s.has_results = true;
    s.selected_index = -1;

    int err = 0;
    int warn = 0;
    for (const auto& is : s.issues) {
      if (is.severity == nebula4x::ContentIssueSeverity::Error) err++;
      if (is.severity == nebula4x::ContentIssueSeverity::Warning) warn++;
    }
    s.last_status = "Validation complete (" + std::to_string(err) + " errors, " + std::to_string(warn) + " warnings).";
  } catch (const std::exception& e) {
    s.last_error = e.what();
    s.has_results = false;
    s.issues.clear();
  }
}

}  // namespace

void draw_content_validation_window(Simulation& sim, UIState& ui) {
  auto& s = st();
  if (!s.initialized) {
    set_cstr(s.export_text_path, "content_validation_report.txt");
    set_cstr(s.export_json_path, "content_validation_report.json");
    s.initialized = true;
  }

  // Auto-run on first open.
  if (ui.show_content_validation_window && s.auto_run_on_open && !s.has_results) {
    run_validation(sim, s);
  }

  // Auto-refresh when the user hot-reloads content.
  if (ui.show_content_validation_window && s.auto_refresh_on_content_change &&
      s.last_seen_content_generation != sim.content_generation()) {
    run_validation(sim, s);
  }

  ImGui::SetNextWindowSize(ImVec2(980, 680), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Content Validation", &ui.show_content_validation_window)) {
    ImGui::End();
    return;
  }

  // Bundle provenance.
  if (ImGui::CollapsingHeader("Loaded content bundle", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::Text("Content generation: %llu",
                static_cast<unsigned long long>(static_cast<std::uint64_t>(sim.content_generation())));

    const auto& content = sim.content();
    if (!content.content_source_paths.empty()) {
      ImGui::TextUnformatted("Blueprint roots:");
      for (const auto& p : content.content_source_paths) ImGui::BulletText("%s", p.c_str());
    } else {
      ImGui::TextUnformatted("Blueprint roots: (unknown)");
    }

    if (!content.tech_source_paths.empty()) {
      ImGui::TextUnformatted("Tech roots:");
      for (const auto& p : content.tech_source_paths) ImGui::BulletText("%s", p.c_str());
    } else {
      ImGui::TextUnformatted("Tech roots: (unknown)");
    }
  }

  // Controls.
  ImGui::SeparatorText("Run");
  if (ImGui::Button("Run validation")) run_validation(sim, s);
  ImGui::SameLine();
  ImGui::Checkbox("Auto-run on open", &s.auto_run_on_open);
  ImGui::SameLine();
  ImGui::Checkbox("Auto-refresh on content change", &s.auto_refresh_on_content_change);

  ImGui::SeparatorText("Filter");
  ImGui::Checkbox("Errors", &s.show_errors);
  ImGui::SameLine();
  ImGui::Checkbox("Warnings", &s.show_warnings);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(420.0f);
  ImGui::InputText("Search", s.filter, sizeof(s.filter));

  // Summary counts.
  int err_total = 0;
  int warn_total = 0;
  for (const auto& is : s.issues) {
    if (is.severity == nebula4x::ContentIssueSeverity::Error) err_total++;
    if (is.severity == nebula4x::ContentIssueSeverity::Warning) warn_total++;
  }

  ImGui::Text("Issues: %d errors, %d warnings", err_total, warn_total);

  if (!s.last_error.empty()) {
    ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Error: %s", s.last_error.c_str());
  } else if (!s.last_status.empty()) {
    ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1), "%s", s.last_status.c_str());
  }

  // Compute visible rows.
  std::vector<int> visible;
  visible.reserve(s.issues.size());
  for (int i = 0; i < static_cast<int>(s.issues.size()); ++i) {
    const auto& is = s.issues[i];

    if (is.severity == nebula4x::ContentIssueSeverity::Error && !s.show_errors) continue;
    if (is.severity == nebula4x::ContentIssueSeverity::Warning && !s.show_warnings) continue;

    const std::string_view needle{s.filter};
    if (!needle.empty()) {
      bool hit = false;
      hit = hit || contains_case_insensitive(is.message, needle);
      hit = hit || contains_case_insensitive(is.code, needle);
      hit = hit || contains_case_insensitive(is.subject_kind, needle);
      hit = hit || contains_case_insensitive(is.subject_id, needle);
      if (!hit) continue;
    }

    visible.push_back(i);
  }

  // Export / clipboard.
  ImGui::SeparatorText("Export");
  if (ImGui::Button("Copy text (visible)")) {
    std::vector<nebula4x::ContentIssue> tmp;
    tmp.reserve(visible.size());
    for (int idx : visible) tmp.push_back(s.issues[idx]);
    const std::string text = issues_to_text(tmp, true);
    ImGui::SetClipboardText(text.c_str());
    s.last_status = "Copied text report to clipboard.";
    s.last_error.clear();
  }
  ImGui::SameLine();
  if (ImGui::Button("Copy JSON (visible)")) {
    std::vector<nebula4x::ContentIssue> tmp;
    tmp.reserve(visible.size());
    for (int idx : visible) tmp.push_back(s.issues[idx]);
    const std::string js = nebula4x::json::stringify(issues_to_json_value(tmp, true), 2);
    ImGui::SetClipboardText(js.c_str());
    s.last_status = "Copied JSON report to clipboard.";
    s.last_error.clear();
  }

  ImGui::InputText("Text path", s.export_text_path, sizeof(s.export_text_path));
  if (ImGui::Button("Save text (visible)")) {
    try {
      std::vector<nebula4x::ContentIssue> tmp;
      tmp.reserve(visible.size());
      for (int idx : visible) tmp.push_back(s.issues[idx]);
      nebula4x::write_text_file(s.export_text_path, issues_to_text(tmp, true));
      s.last_status = std::string("Wrote ") + s.export_text_path;
      s.last_error.clear();
    } catch (const std::exception& e) {
      s.last_error = e.what();
      s.last_status.clear();
    }
  }

  ImGui::InputText("JSON path", s.export_json_path, sizeof(s.export_json_path));
  if (ImGui::Button("Save JSON (visible)")) {
    try {
      std::vector<nebula4x::ContentIssue> tmp;
      tmp.reserve(visible.size());
      for (int idx : visible) tmp.push_back(s.issues[idx]);
      nebula4x::write_text_file(s.export_json_path, nebula4x::json::stringify(issues_to_json_value(tmp, true), 2));
      s.last_status = std::string("Wrote ") + s.export_json_path;
      s.last_error.clear();
    } catch (const std::exception& e) {
      s.last_error = e.what();
      s.last_status.clear();
    }
  }

  // Issue table.
  ImGui::SeparatorText("Issues");
  if (!s.has_results) {
    ImGui::TextUnformatted("No validation results yet. Click \"Run validation\".");
    ImGui::End();
    return;
  }

  const float table_height = std::max(200.0f, ImGui::GetContentRegionAvail().y * 0.55f);
  const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
                                ImGuiTableFlags_Reorderable | ImGuiTableFlags_ScrollY;
  if (ImGui::BeginTable("content_validation_table", 5, flags, ImVec2(0, table_height))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Sev", ImGuiTableColumnFlags_WidthFixed, 40.0f);
    ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 110.0f);
    ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthFixed, 220.0f);
    ImGui::TableSetupColumn("Code", ImGuiTableColumnFlags_WidthFixed, 230.0f);
    ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    for (int idx : visible) {
      const auto& is = s.issues[idx];

      ImGui::TableNextRow();

      ImGui::PushID(idx);
      // Row selection spanning all columns.
      ImGui::TableSetColumnIndex(0);
      const bool selected = (s.selected_index == idx);
      if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
        s.selected_index = idx;
      }

      ImGui::SameLine();
      const char* sev_txt = (is.severity == nebula4x::ContentIssueSeverity::Error) ? "E" : "W";
      ImGui::TextUnformatted(sev_txt);

      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(is.subject_kind.empty() ? "-" : is.subject_kind.c_str());

      ImGui::TableSetColumnIndex(2);
      ImGui::TextUnformatted(is.subject_id.empty() ? "-" : is.subject_id.c_str());

      ImGui::TableSetColumnIndex(3);
      ImGui::TextUnformatted(is.code.empty() ? "-" : is.code.c_str());

      ImGui::TableSetColumnIndex(4);
      ImGui::TextWrapped("%s", is.message.c_str());

      ImGui::PopID();
    }

    ImGui::EndTable();
  }

  // Selection details.
  ImGui::SeparatorText("Selected");
  if (s.selected_index >= 0 && s.selected_index < static_cast<int>(s.issues.size())) {
    const auto& is = s.issues[s.selected_index];
    ImGui::Text("Severity: %s", nebula4x::to_string(is.severity));
    ImGui::Text("Subject: %s:%s", is.subject_kind.empty() ? "-" : is.subject_kind.c_str(),
                is.subject_id.empty() ? "-" : is.subject_id.c_str());
    ImGui::Text("Code: %s", is.code.empty() ? "-" : is.code.c_str());
    ImGui::TextWrapped("%s", is.message.c_str());

    if (ImGui::Button("Copy selected message")) {
      ImGui::SetClipboardText(is.message.c_str());
      s.last_status = "Copied selected message.";
      s.last_error.clear();
    }
  } else {
    ImGui::TextUnformatted("(none)");
  }

  ImGui::End();
}

}  // namespace nebula4x::ui
