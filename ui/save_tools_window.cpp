#include "ui/save_tools_window.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nebula4x/core/serialization.h"
#include "nebula4x/util/autosave.h"
#include "nebula4x/util/file_io.h"
#include "nebula4x/util/json.h"
#include "nebula4x/util/log.h"
#include "nebula4x/util/save_diff.h"

namespace nebula4x::ui {
namespace {

std::string trim_copy(std::string_view s) {
  std::size_t a = 0;
  while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  std::size_t b = s.size();
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return std::string(s.substr(a, b - a));
}

inline void copy_cstr_trunc(char* dst, const char* src, std::size_t dst_cap) {
  if (!dst || dst_cap == 0) return;
#if defined(_MSC_VER)
  strncpy_s(dst, dst_cap, src ? src : "", _TRUNCATE);
#else
  std::strncpy(dst, src ? src : "", dst_cap);
  dst[dst_cap - 1] = '\0';
#endif
}

bool icontains(std::string_view hay, std::string_view needle) {
  if (needle.empty()) return true;
  if (hay.size() < needle.size()) return false;

  auto tolow = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };

  for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
    bool ok = true;
    for (std::size_t j = 0; j < needle.size(); ++j) {
      if (tolow(static_cast<unsigned char>(hay[i + j])) != tolow(static_cast<unsigned char>(needle[j]))) {
        ok = false;
        break;
      }
    }
    if (ok) return true;
  }
  return false;
}

std::string one_line(std::string s) {
  for (char& c : s) {
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
  }
  // Collapse long runs of whitespace a bit.
  std::string out;
  out.reserve(s.size());
  bool last_space = false;
  for (char c : s) {
    const bool is_space = (c == ' ');
    if (is_space) {
      if (last_space) continue;
      last_space = true;
      out.push_back(' ');
    } else {
      last_space = false;
      out.push_back(c);
    }
  }
  return out;
}

std::string json_preview(const nebula4x::json::Value& v, int max_chars) {
  max_chars = std::clamp(max_chars, 24, 4096);
  std::string s = nebula4x::json::stringify(v, 0);
  s = one_line(std::move(s));
  if ((int)s.size() <= max_chars) return s;
  if (max_chars <= 4) return s.substr(0, (std::size_t)max_chars);
  return s.substr(0, (std::size_t)(max_chars - 3)) + "...";
}

bool load_side_json(Simulation& sim, bool use_current, const char* path, std::string* out, std::string* err) {
  if (!out) return false;
  try {
    if (use_current) {
      *out = serialize_game_to_json(sim.state());
      return true;
    }

    if (!path || path[0] == '\0') {
      if (err) *err = "Path is empty.";
      return false;
    }

    *out = nebula4x::read_text_file(path);
    return true;
  } catch (const std::exception& e) {
    if (err) *err = e.what();
    return false;
  }
}

struct SaveToolsState {
  bool initialized{false};

  // --- Diff inputs ---
  bool a_is_current{false};
  bool b_is_current{true};
  char a_path[256]{};
  char b_path[256]{};

  int max_changes{200};
  int max_value_chars{240};

  bool auto_recompute{false};
  bool diff_dirty{true};

  // --- Diff results (cached) ---
  bool diff_ok{false};
  std::string diff_error;
  std::string diff_report_json;
  std::string diff_text;
  nebula4x::json::Value diff_report; // parsed diff_report_json

  // --- Diff UI ---
  char filter_path[128]{};
  char filter_text[128]{};
  bool show_add{true};
  bool show_remove{true};
  bool show_replace{true};
  int selected_change_idx{-1};
  std::string selected_op;
  std::string selected_path;
  std::string selected_before;
  std::string selected_after;

  // --- Autosaves ---
  bool autosaves_scanned{false};
  nebula4x::AutosaveScanResult autosaves;
  int autosave_selected_idx{-1};

  // --- Export paths ---
  char export_report_json_path[256]{"saves/save_diff_report.json"};
  char export_report_text_path[256]{"saves/save_diff_report.txt"};
  char export_patch_json_path[256]{"saves/save_patch.json"};
  std::string export_status;

  // --- Apply patch ---
  char apply_doc_path[256]{};
  char apply_patch_path[256]{};
  char apply_output_path[256]{"saves/patched_save.json"};
  int apply_indent{2};
  bool apply_accept_root_slash{true};
  bool apply_ok{false};
  std::string apply_status;
  std::string apply_output_json;
};

SaveToolsState& st() {
  static SaveToolsState s;
  return s;
}

void scan_autosaves_if_needed(const UIState& ui) {
  auto& s = st();
  if (s.autosaves_scanned) return;

  nebula4x::AutosaveConfig cfg;
  cfg.enabled = ui.autosave_game_enabled;
  cfg.interval_hours = ui.autosave_game_interval_hours;
  cfg.keep_files = ui.autosave_game_keep_files;
  cfg.dir = ui.autosave_game_dir;
  cfg.prefix = "autosave_";
  cfg.extension = ".json";

  s.autosaves = nebula4x::scan_autosaves(cfg, 64);
  s.autosaves_scanned = true;
  s.autosave_selected_idx = s.autosaves.files.empty() ? -1 : 0;
}

void rescan_autosaves(const UIState& ui) {
  auto& s = st();
  s.autosaves_scanned = false;
  scan_autosaves_if_needed(ui);
}

void set_selected_change_details() {
  auto& s = st();
  s.selected_op.clear();
  s.selected_path.clear();
  s.selected_before.clear();
  s.selected_after.clear();

  if (!s.diff_ok) return;
  const auto* obj = s.diff_report.as_object();
  if (!obj) return;
  auto it = obj->find("changes");
  if (it == obj->end()) return;
  const auto* arr = it->second.as_array();
  if (!arr) return;
  if (s.selected_change_idx < 0 || s.selected_change_idx >= (int)arr->size()) return;

  const auto& chv = (*arr)[static_cast<std::size_t>(s.selected_change_idx)];
  const auto* cho = chv.as_object();
  if (!cho) return;

  s.selected_op = cho->find("op") != cho->end() ? cho->at("op").string_value() : "";
  s.selected_path = cho->find("path") != cho->end() ? cho->at("path").string_value() : "";

  const auto itb = cho->find("before");
  const auto ita = cho->find("after");

  if (itb != cho->end()) {
    s.selected_before = nebula4x::json::stringify(itb->second, 2);
  }
  if (ita != cho->end()) {
    s.selected_after = nebula4x::json::stringify(ita->second, 2);
  }

  // Ensure trailing newline (helps readability when copy/pasting).
  if (!s.selected_before.empty() && s.selected_before.back() != '\n') s.selected_before.push_back('\n');
  if (!s.selected_after.empty() && s.selected_after.back() != '\n') s.selected_after.push_back('\n');
}

bool compute_diff(Simulation& sim, UIState& ui) {
  auto& s = st();
  s.diff_ok = false;
  s.diff_error.clear();
  s.diff_report_json.clear();
  s.diff_text.clear();
  s.diff_report = nebula4x::json::Value();
  s.selected_change_idx = -1;
  s.selected_op.clear();
  s.selected_path.clear();
  s.selected_before.clear();
  s.selected_after.clear();

  std::string a_json;
  std::string b_json;
  std::string err;

  if (!load_side_json(sim, s.a_is_current, s.a_path, &a_json, &err)) {
    s.diff_error = std::string("Failed to load A: ") + err;
    return false;
  }
  if (!load_side_json(sim, s.b_is_current, s.b_path, &b_json, &err)) {
    s.diff_error = std::string("Failed to load B: ") + err;
    return false;
  }

  nebula4x::SaveDiffOptions opt;
  opt.max_changes = std::clamp(s.max_changes, 1, 1000000);
  opt.max_value_chars = std::clamp(s.max_value_chars, 24, 20000);

  try {
    s.diff_report_json = nebula4x::diff_saves_to_json(a_json, b_json, opt);
    s.diff_text = nebula4x::diff_saves_to_text(a_json, b_json, opt);
    s.diff_report = nebula4x::json::parse(s.diff_report_json);
    s.diff_ok = true;
    s.diff_dirty = false;
    s.export_status.clear();

    // Keep autosave scan warm (this window is often used with autosaves).
    scan_autosaves_if_needed(ui);

    return true;
  } catch (const std::exception& e) {
    s.diff_error = e.what();
    return false;
  }
}

void draw_autosave_picker(UIState& ui) {
  auto& s = st();

  if (!ImGui::CollapsingHeader("Autosaves", ImGuiTreeNodeFlags_DefaultOpen)) return;

  scan_autosaves_if_needed(ui);

  ImGui::TextDisabled("Directory: %s", ui.autosave_game_dir);

  if (ImGui::SmallButton("Rescan")) {
    rescan_autosaves(ui);
  }

  if (!s.autosaves.ok) {
    ImGui::SameLine();
    ImGui::TextDisabled("(scan failed)");
    if (!s.autosaves.error.empty()) {
      ImGui::TextWrapped("%s", s.autosaves.error.c_str());
    }
    return;
  }

  if (s.autosaves.files.empty()) {
    ImGui::TextDisabled("(no autosaves found)");
    return;
  }

  // Convenience button: newest autosave vs current.
  if (ImGui::SmallButton("A = newest autosave, B = current")) {
    const auto& f0 = s.autosaves.files.front();
    copy_cstr_trunc(s.a_path, f0.path.c_str(), sizeof(s.a_path));
    s.a_path[sizeof(s.a_path) - 1] = '\0';
    s.a_is_current = false;
    s.b_is_current = true;
    s.diff_dirty = true;
  }

  ImGui::Spacing();

  const float list_h = std::min(240.0f, ImGui::GetTextLineHeightWithSpacing() * 10.0f + 10.0f);
  if (ImGui::BeginListBox("##autosave_list", ImVec2(-FLT_MIN, list_h))) {
    for (int i = 0; i < (int)s.autosaves.files.size(); ++i) {
      const auto& f = s.autosaves.files[(std::size_t)i];
      const bool sel = (i == s.autosave_selected_idx);
      std::string label = f.filename;
      if (f.size_bytes > 0) {
        label += "  (" + std::to_string((int)(f.size_bytes / 1024)) + " KiB)";
      }
      if (ImGui::Selectable(label.c_str(), sel)) {
        s.autosave_selected_idx = i;
      }
    }
    ImGui::EndListBox();
  }

  const auto* chosen =
      (s.autosave_selected_idx >= 0 && s.autosave_selected_idx < (int)s.autosaves.files.size())
          ? &s.autosaves.files[(std::size_t)s.autosave_selected_idx]
          : nullptr;

  if (chosen) {
    ImGui::TextDisabled("Selected: %s", chosen->path.c_str());

    if (ImGui::Button("Set as A")) {
      copy_cstr_trunc(s.a_path, chosen->path.c_str(), sizeof(s.a_path));
      s.a_path[sizeof(s.a_path) - 1] = '\0';
      s.a_is_current = false;
      s.diff_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Set as B")) {
      copy_cstr_trunc(s.b_path, chosen->path.c_str(), sizeof(s.b_path));
      s.b_path[sizeof(s.b_path) - 1] = '\0';
      s.b_is_current = false;
      s.diff_dirty = true;
    }
  }
}

void draw_diff_results_table() {
  auto& s = st();

  if (!s.diff_ok) {
    if (!s.diff_error.empty()) {
      ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.42f, 1.0f), "Diff failed: %s", s.diff_error.c_str());
    } else {
      ImGui::TextDisabled("No diff computed yet.");
    }
    return;
  }

  const auto* root = s.diff_report.as_object();
  if (!root) {
    ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.42f, 1.0f), "Diff report parse error (root not object).");
    return;
  }

  const auto it_changes = root->find("changes");
  const auto* changes = (it_changes != root->end()) ? it_changes->second.as_array() : nullptr;

  const int shown = changes ? (int)changes->size() : 0;
  const bool truncated = root->find("truncated") != root->end() ? root->at("truncated").bool_value(false) : false;

  ImGui::Text("Changes shown: %d%s", shown, truncated ? " (truncated)" : "");

  // Filters.
  {
    bool dirty = false;
    dirty |= ImGui::InputTextWithHint("Filter path", "substring", s.filter_path, IM_ARRAYSIZE(s.filter_path));
    dirty |= ImGui::InputTextWithHint("Filter text", "search in before/after", s.filter_text,
                                      IM_ARRAYSIZE(s.filter_text));

    ImGui::Checkbox("Add", &s.show_add);
    ImGui::SameLine();
    ImGui::Checkbox("Remove", &s.show_remove);
    ImGui::SameLine();
    ImGui::Checkbox("Replace", &s.show_replace);

    (void)dirty;
  }

  if (!changes) {
    ImGui::TextDisabled("(no changes array)");
    return;
  }

  const std::string f_path = trim_copy(s.filter_path);
  const std::string f_txt = trim_copy(s.filter_text);

  const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                ImGuiTableFlags_ScrollY;

  const float table_h = std::max(220.0f, ImGui::GetTextLineHeightWithSpacing() * 10.0f);
  if (ImGui::BeginTable("##save_diff_table", 4, flags, ImVec2(0, table_h))) {
    ImGui::TableSetupColumn("Op", ImGuiTableColumnFlags_WidthFixed, 58.0f);
    ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch, 0.52f);
    ImGui::TableSetupColumn("Before", ImGuiTableColumnFlags_WidthStretch, 0.24f);
    ImGui::TableSetupColumn("After", ImGuiTableColumnFlags_WidthStretch, 0.24f);
    ImGui::TableHeadersRow();

    for (int i = 0; i < (int)changes->size(); ++i) {
      const auto& chv = (*changes)[(std::size_t)i];
      const auto* o = chv.as_object();
      if (!o) continue;

      const std::string op = o->find("op") != o->end() ? o->at("op").string_value() : "";
      const std::string path = o->find("path") != o->end() ? o->at("path").string_value() : "";

      if (op == "add" && !s.show_add) continue;
      if (op == "remove" && !s.show_remove) continue;
      if (op == "replace" && !s.show_replace) continue;

      if (!f_path.empty() && !icontains(path, f_path)) continue;

      const auto itb = o->find("before");
      const auto ita = o->find("after");

      const nebula4x::json::Value before = (itb != o->end()) ? itb->second : nebula4x::json::Value();
      const nebula4x::json::Value after = (ita != o->end()) ? ita->second : nebula4x::json::Value();

      const std::string before_p = json_preview(before, std::min(260, s.max_value_chars));
      const std::string after_p = json_preview(after, std::min(260, s.max_value_chars));

      if (!f_txt.empty()) {
        if (!icontains(before_p, f_txt) && !icontains(after_p, f_txt)) continue;
      }


      ImGui::TableNextRow();

      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(op.c_str());

      ImGui::TableSetColumnIndex(1);
      ImGui::PushID(i);
      const bool selected = (s.selected_change_idx == i);
      if (ImGui::Selectable(path.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
        s.selected_change_idx = i;
        set_selected_change_details();
      }
      if (ImGui::BeginPopupContextItem("##diff_ctx")) {
        if (ImGui::MenuItem("Copy path")) {
          ImGui::SetClipboardText(path.c_str());
        }
        if (ImGui::MenuItem("Copy row (text)")) {
          std::string row;
          row.reserve(path.size() + before_p.size() + after_p.size() + 16);
          row += op;
          row += " ";
          row += path;
          row += " | ";
          row += before_p;
          row += " -> ";
          row += after_p;
          ImGui::SetClipboardText(row.c_str());
        }
        ImGui::EndPopup();
      }
      ImGui::PopID();

      ImGui::TableSetColumnIndex(2);
      ImGui::TextUnformatted(before_p.c_str());

      ImGui::TableSetColumnIndex(3);
      ImGui::TextUnformatted(after_p.c_str());
    }

    ImGui::EndTable();
  }

  // Selected change details.
  if (s.selected_change_idx >= 0) {
    ImGui::Separator();
    ImGui::Text("Selected: %s %s", s.selected_op.c_str(), s.selected_path.c_str());

    if (ImGui::SmallButton("Copy selected path")) {
      ImGui::SetClipboardText(s.selected_path.c_str());
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy before")) {
      ImGui::SetClipboardText(s.selected_before.c_str());
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy after")) {
      ImGui::SetClipboardText(s.selected_after.c_str());
    }

    if (ImGui::BeginTable("##save_diff_detail", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)) {
      ImGui::TableSetupColumn("Before", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("After", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableHeadersRow();

      const float h = std::max(160.0f, ImGui::GetTextLineHeightWithSpacing() * 8.0f);

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::BeginChild("##before_child", ImVec2(0, h), false);
      ImGui::TextUnformatted(s.selected_before.c_str());
      ImGui::EndChild();

      ImGui::TableSetColumnIndex(1);
      ImGui::BeginChild("##after_child", ImVec2(0, h), false);
      ImGui::TextUnformatted(s.selected_after.c_str());
      ImGui::EndChild();

      ImGui::EndTable();
    }
  }
}

} // namespace

void draw_save_tools_window(Simulation& sim, UIState& ui, const char* save_path, const char* load_path) {
  if (!ui.show_save_tools_window) return;

  auto& s = st();

  if (!s.initialized) {
    // Seed paths from the main menu defaults.
    if (save_path && save_path[0] != '\0') {
      copy_cstr_trunc(s.a_path, save_path, sizeof(s.a_path));
      s.a_path[sizeof(s.a_path) - 1] = '\0';
    } else {
      copy_cstr_trunc(s.a_path, "saves/save.json", sizeof(s.a_path));
      s.a_path[sizeof(s.a_path) - 1] = '\0';
    }

    if (load_path && load_path[0] != '\0') {
      copy_cstr_trunc(s.b_path, load_path, sizeof(s.b_path));
      s.b_path[sizeof(s.b_path) - 1] = '\0';
    } else {
      copy_cstr_trunc(s.b_path, "saves/load.json", sizeof(s.b_path));
      s.b_path[sizeof(s.b_path) - 1] = '\0';
    }

    // Apply tab defaults.
    if (save_path && save_path[0] != '\0') {
      copy_cstr_trunc(s.apply_doc_path, save_path, sizeof(s.apply_doc_path));
      s.apply_doc_path[sizeof(s.apply_doc_path) - 1] = '\0';
    } else {
      copy_cstr_trunc(s.apply_doc_path, "saves/save.json", sizeof(s.apply_doc_path));
      s.apply_doc_path[sizeof(s.apply_doc_path) - 1] = '\0';
    }
    copy_cstr_trunc(s.apply_patch_path, s.export_patch_json_path, sizeof(s.apply_patch_path));
    s.apply_patch_path[sizeof(s.apply_patch_path) - 1] = '\0';

    s.initialized = true;
  }

  ImGui::SetNextWindowSize(ImVec2(960, 720), ImGuiCond_Appearing);

  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
  if (!ImGui::Begin("Save Tools (Diff / Patch)", &ui.show_save_tools_window, flags)) {
    ImGui::End();
    return;
  }

  ImGui::TextWrapped(
      "Experimental tooling for save-game debugging: diff two save JSON documents, export an RFC 6902 patch, or apply a patch.\n"
      "Tip: Use the Autosaves section to quickly compare the newest autosave against the current game state.");

  if (ImGui::BeginTabBar("##save_tools_tabs")) {
    if (ImGui::BeginTabItem("Diff")) {
      bool dirty = false;

      ImGui::SeparatorText("Inputs");

      // A
      {
        ImGui::Text("A (base)");
        ImGui::SameLine();
        dirty |= ImGui::Checkbox("Use current##a", &s.a_is_current);
        if (s.a_is_current) {
          ImGui::BeginDisabled();
        }
        dirty |= ImGui::InputText("Path##a", s.a_path, IM_ARRAYSIZE(s.a_path));
        if (s.a_is_current) {
          ImGui::EndDisabled();
        }

        if (ImGui::SmallButton("A <- Save path")) {
          if (save_path && save_path[0] != '\0') {
            copy_cstr_trunc(s.a_path, save_path, sizeof(s.a_path));
            s.a_path[sizeof(s.a_path) - 1] = '\0';
            s.a_is_current = false;
            dirty = true;
          }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("A <- Load path")) {
          if (load_path && load_path[0] != '\0') {
            copy_cstr_trunc(s.a_path, load_path, sizeof(s.a_path));
            s.a_path[sizeof(s.a_path) - 1] = '\0';
            s.a_is_current = false;
            dirty = true;
          }
        }
      }

      // B
      {
        ImGui::Spacing();
        ImGui::Text("B (compare)");
        ImGui::SameLine();
        dirty |= ImGui::Checkbox("Use current##b", &s.b_is_current);
        if (s.b_is_current) {
          ImGui::BeginDisabled();
        }
        dirty |= ImGui::InputText("Path##b", s.b_path, IM_ARRAYSIZE(s.b_path));
        if (s.b_is_current) {
          ImGui::EndDisabled();
        }

        if (ImGui::SmallButton("B <- Save path")) {
          if (save_path && save_path[0] != '\0') {
            copy_cstr_trunc(s.b_path, save_path, sizeof(s.b_path));
            s.b_path[sizeof(s.b_path) - 1] = '\0';
            s.b_is_current = false;
            dirty = true;
          }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("B <- Load path")) {
          if (load_path && load_path[0] != '\0') {
            copy_cstr_trunc(s.b_path, load_path, sizeof(s.b_path));
            s.b_path[sizeof(s.b_path) - 1] = '\0';
            s.b_is_current = false;
            dirty = true;
          }
        }
      }

      ImGui::Spacing();
      ImGui::SeparatorText("Options");

      dirty |= ImGui::SliderInt("Max changes", &s.max_changes, 10, 2000);
      dirty |= ImGui::SliderInt("Preview chars", &s.max_value_chars, 60, 800);
      ImGui::Checkbox("Auto recompute", &s.auto_recompute);

      if (dirty) {
        s.diff_dirty = true;
      }

      if (ImGui::Button("Compute diff")) {
        compute_diff(sim, ui);
      }
      ImGui::SameLine();
      if (ImGui::Button("Copy text report")) {
        if (s.diff_ok && !s.diff_text.empty()) {
          ImGui::SetClipboardText(s.diff_text.c_str());
        }
      }

      if (s.auto_recompute && s.diff_dirty) {
        // Best-effort: recompute immediately.
        compute_diff(sim, ui);
      }

      ImGui::Separator();
      draw_autosave_picker(ui);

      ImGui::SeparatorText("Results");
      draw_diff_results_table();

      ImGui::SeparatorText("Export");

      ImGui::InputText("Report JSON path", s.export_report_json_path, IM_ARRAYSIZE(s.export_report_json_path));
      ImGui::InputText("Report text path", s.export_report_text_path, IM_ARRAYSIZE(s.export_report_text_path));
      ImGui::InputText("Patch JSON path", s.export_patch_json_path, IM_ARRAYSIZE(s.export_patch_json_path));

      if (ImGui::Button("Write report (JSON)")) {
        s.export_status.clear();
        if (!s.diff_ok) {
          if (!compute_diff(sim, ui)) {
            s.export_status = "Cannot export: diff failed.";
          }
        }
        if (s.diff_ok) {
          try {
            nebula4x::write_text_file(s.export_report_json_path, s.diff_report_json);
            s.export_status = std::string("Wrote ") + s.export_report_json_path;
          } catch (const std::exception& e) {
            s.export_status = std::string("Export failed: ") + e.what();
          }
        }
      }
      ImGui::SameLine();
      if (ImGui::Button("Write report (text)")) {
        s.export_status.clear();
        if (!s.diff_ok) {
          if (!compute_diff(sim, ui)) {
            s.export_status = "Cannot export: diff failed.";
          }
        }
        if (s.diff_ok) {
          try {
            nebula4x::write_text_file(s.export_report_text_path, s.diff_text);
            s.export_status = std::string("Wrote ") + s.export_report_text_path;
          } catch (const std::exception& e) {
            s.export_status = std::string("Export failed: ") + e.what();
          }
        }
      }
      ImGui::SameLine();
      if (ImGui::Button("Generate + write JSON Patch")) {
        s.export_status.clear();
        std::string a_json;
        std::string b_json;
        std::string err;
        if (!load_side_json(sim, s.a_is_current, s.a_path, &a_json, &err)) {
          s.export_status = std::string("Failed to load A: ") + err;
        } else if (!load_side_json(sim, s.b_is_current, s.b_path, &b_json, &err)) {
          s.export_status = std::string("Failed to load B: ") + err;
        } else {
          try {
            const std::string patch = nebula4x::diff_saves_to_json_patch(a_json, b_json, nebula4x::JsonPatchOptions{.max_ops = 0, .indent = 2});
            nebula4x::write_text_file(s.export_patch_json_path, patch);
            s.export_status = std::string("Wrote ") + s.export_patch_json_path;

            // Keep apply tab seeded.
            copy_cstr_trunc(s.apply_patch_path, s.export_patch_json_path, sizeof(s.apply_patch_path));
            s.apply_patch_path[sizeof(s.apply_patch_path) - 1] = '\0';
          } catch (const std::exception& e) {
            s.export_status = std::string("Patch generation failed: ") + e.what();
          }
        }
      }

      if (!s.export_status.empty()) {
        ImGui::TextWrapped("%s", s.export_status.c_str());
      }

      ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Apply Patch")) {
      ImGui::SeparatorText("Inputs");

      ImGui::InputText("Document (JSON)", s.apply_doc_path, IM_ARRAYSIZE(s.apply_doc_path));
      ImGui::InputText("Patch (RFC 6902)", s.apply_patch_path, IM_ARRAYSIZE(s.apply_patch_path));
      ImGui::InputText("Output path", s.apply_output_path, IM_ARRAYSIZE(s.apply_output_path));

      ImGui::SetNextItemWidth(120.0f);
      ImGui::Combo("Indent", &s.apply_indent, "0\0" "2\0" "4\0" "8\0");
      const int indent_choices[] = {0, 2, 4, 8};
      const int indent = indent_choices[std::clamp(s.apply_indent, 0, 3)];

      ImGui::Checkbox("Accept '/' as root (compat)##apply", &s.apply_accept_root_slash);

      if (ImGui::Button("Apply patch")) {
        s.apply_ok = false;
        s.apply_status.clear();
        s.apply_output_json.clear();

        try {
          const std::string doc = nebula4x::read_text_file(s.apply_doc_path);
          const std::string patch = nebula4x::read_text_file(s.apply_patch_path);

          nebula4x::JsonPatchApplyOptions opt;
          opt.indent = indent;
          opt.accept_root_slash = s.apply_accept_root_slash;

          s.apply_output_json = nebula4x::apply_json_patch(doc, patch, opt);
          nebula4x::write_text_file(s.apply_output_path, s.apply_output_json);
          s.apply_ok = true;
          s.apply_status = std::string("Wrote patched document: ") + s.apply_output_path;
        } catch (const std::exception& e) {
          s.apply_ok = false;
          s.apply_status = std::string("Apply failed: ") + e.what();
        }
      }

      if (s.apply_ok) {
        ImGui::SameLine();
        if (ImGui::Button("Load patched output into game")) {
          try {
            sim.load_game(deserialize_game_from_json(s.apply_output_json));
            s.apply_status = "Loaded patched output into the current game.";
          } catch (const std::exception& e) {
            s.apply_status = std::string("Load failed: ") + e.what();
          }
        }
      }

      if (!s.apply_status.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", s.apply_status.c_str());
      }

      if (s.apply_ok && !s.apply_output_json.empty()) {
        ImGui::SeparatorText("Preview (first ~8KB)");
        const std::size_t n = std::min<std::size_t>(8192, s.apply_output_json.size());
        const std::string preview = s.apply_output_json.substr(0, n);
        ImGui::BeginChild("##patched_preview", ImVec2(0, 220), true);
        ImGui::TextUnformatted(preview.c_str());
        ImGui::EndChild();
      }

      ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
  }

  ImGui::End();
}

} // namespace nebula4x::ui
