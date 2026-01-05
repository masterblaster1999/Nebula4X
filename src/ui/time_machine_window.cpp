#include "ui/time_machine_window.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nebula4x/core/serialization.h"

#include "nebula4x/util/file_io.h"
#include "nebula4x/util/json.h"
#include "nebula4x/util/log.h"
#include "nebula4x/util/save_diff.h"
#include "nebula4x/util/time.h"

#include "ui/game_json_cache.h"
#include "ui/watchboard_window.h"

namespace nebula4x::ui {
namespace {

char to_lower_ascii(char c) {
  return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

std::string to_lower_copy(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) out.push_back(to_lower_ascii(c));
  return out;
}

bool contains_text(std::string_view haystack, std::string_view needle, bool case_sensitive) {
  if (needle.empty()) return true;
  if (case_sensitive) {
    return haystack.find(needle) != std::string_view::npos;
  }
  const std::string h = to_lower_copy(haystack);
  const std::string n = to_lower_copy(needle);
  return h.find(n) != std::string::npos;
}

std::string format_number(double x) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.6g", x);
  return std::string(buf);
}

std::string shorten(std::string s, int max_chars) {
  if (max_chars <= 0) return s;
  if (static_cast<int>(s.size()) <= max_chars) return s;
  if (max_chars <= 3) return s.substr(0, static_cast<std::size_t>(max_chars));
  s.resize(static_cast<std::size_t>(max_chars - 3));
  s += "...";
  return s;
}

std::string preview_value(const nebula4x::json::Value& v, int max_chars) {
  // Avoid stringifying large objects/arrays: show an informative summary.
  if (v.is_object()) {
    const auto* o = v.as_object();
    const std::size_t n = o ? o->size() : 0;
    return "{" + std::to_string(n) + "}";
  }
  if (v.is_array()) {
    const auto* a = v.as_array();
    const std::size_t n = a ? a->size() : 0;
    return "[" + std::to_string(n) + "]";
  }
  if (v.is_null()) return "null";
  if (v.is_bool()) return v.bool_value() ? "true" : "false";
  if (v.is_number()) return format_number(v.number_value());
  if (v.is_string()) {
    std::string s = v.string_value();
    // One-line preview.
    for (char& c : s) {
      if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    }
    s = shorten(std::move(s), std::max(0, max_chars - 2));
    return "\"" + s + "\"";
  }
  return "?";
}

struct DiffChange {
  std::string op;
  std::string path;
  std::string before;
  std::string after;
};

struct DiffView {
  bool valid{false};
  bool truncated{false};
  std::vector<DiffChange> changes;
  std::string error;
};

DiffView compute_diff_view(const std::string& a_json, const std::string& b_json, int max_changes, int preview_chars) {
  DiffView out;
  try {
    nebula4x::SaveDiffOptions opt;
    opt.max_changes = std::clamp(max_changes, 1, 1000000);

    const std::string report_text = nebula4x::diff_saves_to_json(a_json, b_json, opt);
    const nebula4x::json::Value report = nebula4x::json::parse(report_text);
    const auto* obj = report.as_object();
    if (!obj) {
      out.error = "Diff report root is not an object.";
      return out;
    }

    out.truncated = false;
    if (auto it = obj->find("truncated"); it != obj->end()) {
      out.truncated = it->second.bool_value(false);
    }

    const auto it_changes = obj->find("changes");
    if (it_changes == obj->end() || !it_changes->second.is_array()) {
      out.error = "Diff report missing 'changes' array.";
      return out;
    }

    const auto& arr = it_changes->second.array();
    out.changes.reserve(arr.size());
    for (const auto& v : arr) {
      const auto* co = v.as_object();
      if (!co) continue;
      DiffChange c;
      if (auto it = co->find("op"); it != co->end()) c.op = it->second.string_value();
      if (auto it = co->find("path"); it != co->end()) c.path = it->second.string_value();
      if (auto it = co->find("before"); it != co->end()) c.before = preview_value(it->second, preview_chars);
      if (auto it = co->find("after"); it != co->end()) c.after = preview_value(it->second, preview_chars);
      if (c.path.empty()) c.path = "/";
      out.changes.push_back(std::move(c));
    }

    out.valid = true;
  } catch (const std::exception& e) {
    out.error = e.what();
  }
  return out;
}

struct Snapshot {
  std::uint64_t id{0};
  std::uint64_t state_generation{0};
  std::uint64_t cache_revision{0};
  std::int64_t day{0};
  int hour{0};

  std::string json_text;

  // Diff vs previous snapshot (Prev mode). Snapshot[0] has none.
  bool diff_prev_truncated{false};
  std::vector<DiffChange> diff_prev;
};

struct TimeMachineRuntime {
  bool initialized{false};
  std::uint64_t last_seen_state_generation{0};
  std::uint64_t next_snapshot_id{1};

  // Selection + compare.
  int selected_idx{0};
  int compare_mode{0};       // 0 = Prev, 1 = Baseline
  int baseline_idx{0};
  bool follow_latest{true};

  // Filters.
  char path_filter[128] = "";
  char value_filter[128] = "";
  bool filter_case_sensitive{false};
  bool show_add{true};
  bool show_remove{true};
  bool show_replace{true};

  // Export paths.
  char export_snapshot_path[256] = "saves/time_machine_snapshot.json";
  char export_diff_path[256] = "saves/time_machine_diff.json";
  char export_patch_path[256] = "saves/time_machine_patch.json";

  // Cached computed diff for Baseline mode.
  int cached_a{-1};
  int cached_b{-1};
  int cached_max_changes{0};
  int cached_preview_chars{0};
  DiffView cached_diff;

  // Runtime errors (not persisted).
  std::string last_error;

  std::vector<Snapshot> snapshots;
};

std::size_t total_snapshot_bytes(const std::vector<Snapshot>& snaps) {
  std::size_t sum = 0;
  for (const auto& s : snaps) sum += s.json_text.size();
  return sum;
}

void clear_history(TimeMachineRuntime& rt) {
  rt.snapshots.clear();
  rt.selected_idx = 0;
  rt.baseline_idx = 0;
  rt.cached_a = -1;
  rt.cached_b = -1;
  rt.cached_diff = DiffView{};
}

void clamp_indices(TimeMachineRuntime& rt) {
  const int n = static_cast<int>(rt.snapshots.size());
  if (n <= 0) {
    rt.selected_idx = 0;
    rt.baseline_idx = 0;
    return;
  }
  rt.selected_idx = std::clamp(rt.selected_idx, 0, n - 1);
  rt.baseline_idx = std::clamp(rt.baseline_idx, 0, n - 1);
}

void trim_history(TimeMachineRuntime& rt, int keep) {
  keep = std::clamp(keep, 1, 1000000);
  if (static_cast<int>(rt.snapshots.size()) <= keep) return;

  const int to_remove = static_cast<int>(rt.snapshots.size()) - keep;
  rt.snapshots.erase(rt.snapshots.begin(), rt.snapshots.begin() + to_remove);

  // New first snapshot has no previous diff.
  if (!rt.snapshots.empty()) {
    rt.snapshots.front().diff_prev.clear();
    rt.snapshots.front().diff_prev_truncated = false;
  }

  rt.selected_idx = std::max(0, rt.selected_idx - to_remove);
  rt.baseline_idx = std::max(0, rt.baseline_idx - to_remove);
  rt.cached_a = -1;
  rt.cached_b = -1;
  clamp_indices(rt);
}

bool capture_snapshot(TimeMachineRuntime& rt, Simulation& sim, UIState& ui, bool force_refresh) {
  rt.last_error.clear();

  const double now_sec = ImGui::GetTime();
  // Force refresh (manual capture) bypasses min-refresh throttling.
  if (!ensure_game_json_cache(sim, now_sec, ui.time_machine_refresh_sec, force_refresh)) {
    const auto& c = game_json_cache();
    rt.last_error = c.error.empty() ? "Failed to refresh game JSON." : c.error;
    return false;
  }

  const auto& c = game_json_cache();
  if (!c.loaded || !c.root) {
    rt.last_error = c.error.empty() ? "Game JSON cache is not loaded." : c.error;
    return false;
  }

  const std::string& txt = c.text;
  if (!rt.snapshots.empty() && rt.snapshots.back().json_text == txt) {
    return false; // No change.
  }

  Snapshot snap;
  snap.id = rt.next_snapshot_id++;
  snap.state_generation = sim.state_generation();
  snap.cache_revision = c.revision;
  snap.day = sim.state().date.days_since_epoch();
  snap.hour = sim.state().hour_of_day;
  snap.json_text = txt;

  // Compute diff vs previous snapshot (Prev mode).
  if (!rt.snapshots.empty()) {
    const Snapshot& prev = rt.snapshots.back();
    DiffView dv = compute_diff_view(prev.json_text, snap.json_text, ui.time_machine_max_changes, ui.time_machine_max_value_chars);
    if (dv.valid) {
      snap.diff_prev_truncated = dv.truncated;
      snap.diff_prev = std::move(dv.changes);
    } else {
      // Keep snapshot but record the error for visibility.
      snap.diff_prev_truncated = false;
      snap.diff_prev.clear();
      rt.last_error = "Diff error: " + dv.error;
    }
  }

  const bool was_at_latest = (rt.selected_idx == static_cast<int>(rt.snapshots.size()) - 1);
  rt.snapshots.push_back(std::move(snap));

  trim_history(rt, ui.time_machine_keep_snapshots);

  if (rt.follow_latest && was_at_latest) {
    rt.selected_idx = static_cast<int>(rt.snapshots.size()) - 1;
  }
  clamp_indices(rt);
  return true;
}

bool op_visible(const DiffChange& c, const TimeMachineRuntime& rt) {
  if (c.op == "add") return rt.show_add;
  if (c.op == "remove") return rt.show_remove;
  // treat everything else as replace
  return rt.show_replace;
}

} // namespace

void draw_time_machine_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  static TimeMachineRuntime rt;
  if (!rt.initialized) {
    rt.initialized = true;
    rt.last_seen_state_generation = sim.state_generation();
  }

  // If the underlying state was replaced externally (new game, load), clear history.
  // When the user uses the Time Machine's own "Load snapshot" button, we update
  // last_seen_state_generation immediately so we preserve history.
  const std::uint64_t gen = sim.state_generation();
  if (rt.last_seen_state_generation != 0 && gen != rt.last_seen_state_generation) {
    clear_history(rt);
    rt.last_seen_state_generation = gen;
  }

  // Auto-recording.
  if (ui.time_machine_recording) {
    (void)capture_snapshot(rt, sim, ui, /*force_refresh=*/false);
  }

  ImGui::SetNextWindowSize(ImVec2(980, 720), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Time Machine", &ui.show_time_machine_window)) {
    ImGui::End();
    return;
  }

  // --- Controls ---
  {
    ui.time_machine_refresh_sec = std::clamp(ui.time_machine_refresh_sec, 0.05f, 30.0f);
    ui.time_machine_keep_snapshots = std::clamp(ui.time_machine_keep_snapshots, 1, 512);
    ui.time_machine_max_changes = std::clamp(ui.time_machine_max_changes, 1, 50000);
    ui.time_machine_max_value_chars = std::clamp(ui.time_machine_max_value_chars, 16, 2000);

    ImGui::Checkbox("Recording##tm", &ui.time_machine_recording);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragFloat("Refresh (s)##tm", &ui.time_machine_refresh_sec, 0.05f, 0.05f, 30.0f, "%.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragInt("Keep##tm", &ui.time_machine_keep_snapshots, 1.0f, 1, 512);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Snapshots are stored in-memory as full save-game JSON text.\nReduce this value if memory usage is high.");
    }

    ImGui::SameLine();
    if (ImGui::Button("Capture now##tm")) {
      (void)capture_snapshot(rt, sim, ui, /*force_refresh=*/true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear##tm")) {
      clear_history(rt);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Follow latest##tm", &rt.follow_latest);
  }

  // Advanced knobs.
  if (ImGui::CollapsingHeader("Diff settings", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragInt("Max changes (preview)##tm", &ui.time_machine_max_changes, 5.0f, 1, 50000);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragInt("Preview chars##tm", &ui.time_machine_max_value_chars, 5.0f, 16, 2000);

    ImGui::Checkbox("Case sensitive filters##tm", &rt.filter_case_sensitive);
    ImGui::SameLine();
    ImGui::Checkbox("Show add##tm", &rt.show_add);
    ImGui::SameLine();
    ImGui::Checkbox("Show remove##tm", &rt.show_remove);
    ImGui::SameLine();
    ImGui::Checkbox("Show replace##tm", &rt.show_replace);

    ImGui::Separator();
    ImGui::RadioButton("Compare: Prev##tm", &rt.compare_mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Compare: Baseline##tm", &rt.compare_mode, 1);
    ImGui::SameLine();
    if (ImGui::Button("Set baseline = selected##tm")) {
      rt.baseline_idx = rt.selected_idx;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Baseline: %d", rt.baseline_idx);
  }

  if (!rt.last_error.empty()) {
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", rt.last_error.c_str());
  }

  // --- Layout: snapshot list (left) + details/diff (right) ---
  ImGui::Separator();

  const float left_w = 320.0f;
  ImGui::BeginChild("##tm_left", ImVec2(left_w, 0), true);
  {
    const int n = static_cast<int>(rt.snapshots.size());
    ImGui::TextDisabled("Snapshots: %d", n);
    const std::size_t bytes = total_snapshot_bytes(rt.snapshots);
    ImGui::SameLine();
    ImGui::TextDisabled(" | Memory: %.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));

    if (ImGui::Button("Pin JSON Explorer to baseline##tm")) {
      if (rt.baseline_idx >= 0 && rt.baseline_idx < n) {
        ui.show_json_explorer_window = true;
        ui.request_json_explorer_goto_path = "/";
      }
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Opens the JSON Explorer (baseline is for comparison, but Explorer always shows the live state).\nUse diff rows to jump to specific paths.");
    }

    ImGui::Separator();

    if (n == 0) {
      ImGui::TextWrapped("No snapshots recorded yet.\n\n- Enable 'Recording' to auto-capture while you advance turns.\n- Or click 'Capture now' to grab a snapshot immediately.");
    } else {
      // Keep indices valid.
      clamp_indices(rt);

      ImGuiListClipper clipper;
      clipper.Begin(n);
      while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
          const Snapshot& s = rt.snapshots[i];
          const std::string dt = nebula4x::format_datetime(s.day, s.hour);
          const int delta = (i == 0) ? 0 : static_cast<int>(s.diff_prev.size());
          const bool trunc = (i == 0) ? false : s.diff_prev_truncated;

          char label[256];
          if (i == rt.baseline_idx) {
            std::snprintf(label, sizeof(label), "[%d] %s  (BASE)", i, dt.c_str());
          } else if (i == 0) {
            std::snprintf(label, sizeof(label), "[%d] %s", i, dt.c_str());
          } else {
            if (trunc) {
              std::snprintf(label, sizeof(label), "[%d] %s  (Δ %d+)", i, dt.c_str(), delta);
            } else {
              std::snprintf(label, sizeof(label), "[%d] %s  (Δ %d)", i, dt.c_str(), delta);
            }
          }

          if (ImGui::Selectable(label, rt.selected_idx == i)) {
            rt.selected_idx = i;
          }

          if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Set baseline here")) {
              rt.baseline_idx = i;
            }
            if (ImGui::MenuItem("Load this snapshot")) {
              try {
                sim.load_game(deserialize_game_from_json(rt.snapshots[i].json_text));
                selected_ship = kInvalidId;
                selected_colony = kInvalidId;
                selected_body = kInvalidId;
                invalidate_game_json_cache();
                rt.last_seen_state_generation = sim.state_generation();
                rt.last_error.clear();
              } catch (const std::exception& e) {
                rt.last_error = std::string("Load failed: ") + e.what();
              }
            }
            if (ImGui::MenuItem("Copy snapshot JSON")) {
              ImGui::SetClipboardText(rt.snapshots[i].json_text.c_str());
            }
            ImGui::EndPopup();
          }
        }
      }
    }
  }
  ImGui::EndChild();

  ImGui::SameLine();

  ImGui::BeginChild("##tm_right", ImVec2(0, 0), false);
  {
    const int n = static_cast<int>(rt.snapshots.size());
    if (n == 0) {
      ImGui::TextDisabled("Capture a snapshot to begin.");
      ImGui::EndChild();
      ImGui::End();
      return;
    }

    clamp_indices(rt);
    const Snapshot& cur = rt.snapshots[rt.selected_idx];

    const std::string dt = nebula4x::format_datetime(cur.day, cur.hour);
    ImGui::Text("Selected: [%d] %s", rt.selected_idx, dt.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled(" | JSON: %.1f KB", static_cast<double>(cur.json_text.size()) / 1024.0);

    // --- Actions ---
    if (ImGui::Button("Load snapshot##tm")) {
      try {
        sim.load_game(deserialize_game_from_json(cur.json_text));
        selected_ship = kInvalidId;
        selected_colony = kInvalidId;
        selected_body = kInvalidId;
        invalidate_game_json_cache();
        rt.last_seen_state_generation = sim.state_generation();
        rt.last_error.clear();
      } catch (const std::exception& e) {
        rt.last_error = std::string("Load failed: ") + e.what();
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy JSON##tm")) {
      ImGui::SetClipboardText(cur.json_text.c_str());
    }

    ImGui::SameLine();
    if (ImGui::Button("Export JSON##tm")) {
      try {
        nebula4x::write_text_file(rt.export_snapshot_path, cur.json_text);
        rt.last_error.clear();
      } catch (const std::exception& e) {
        rt.last_error = std::string("Export failed: ") + e.what();
      }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(320.0f);
    ImGui::InputText("##tm_export_snapshot", rt.export_snapshot_path, IM_ARRAYSIZE(rt.export_snapshot_path));
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Snapshot export path");
    }

    ImGui::Separator();

    // --- Diff view source selection ---
    int a_idx = -1;
    int b_idx = rt.selected_idx;
    const DiffView* dv_ptr = nullptr;

    if (rt.compare_mode == 0) {
      // Prev
      if (rt.selected_idx <= 0) {
        ImGui::TextDisabled("Prev diff: (none for the first snapshot)");
      } else {
        a_idx = rt.selected_idx - 1;
        dv_ptr = nullptr; // use stored diff
        const int shown = static_cast<int>(cur.diff_prev.size());
        ImGui::TextDisabled("Prev diff: [%d] -> [%d]  (%d%s)", a_idx, b_idx, shown, cur.diff_prev_truncated ? "+" : "");
      }
    } else {
      // Baseline
      a_idx = std::clamp(rt.baseline_idx, 0, n - 1);
      ImGui::TextDisabled("Baseline diff: [%d] -> [%d]", a_idx, b_idx);

      const bool cache_ok = (rt.cached_a == a_idx && rt.cached_b == b_idx && rt.cached_max_changes == ui.time_machine_max_changes &&
                             rt.cached_preview_chars == ui.time_machine_max_value_chars);
      if (!cache_ok) {
        rt.cached_a = a_idx;
        rt.cached_b = b_idx;
        rt.cached_max_changes = ui.time_machine_max_changes;
        rt.cached_preview_chars = ui.time_machine_max_value_chars;
        rt.cached_diff = compute_diff_view(rt.snapshots[a_idx].json_text, rt.snapshots[b_idx].json_text, ui.time_machine_max_changes,
                                           ui.time_machine_max_value_chars);
      }
      dv_ptr = &rt.cached_diff;
      if (!rt.cached_diff.valid && !rt.cached_diff.error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "Diff error: %s", rt.cached_diff.error.c_str());
      }
    }

    // Export/copy diff + patch for the current comparison.
    ImGui::Separator();
    {
      ImGui::SetNextItemWidth(260.0f);
      ImGui::InputText("Diff path##tm", rt.export_diff_path, IM_ARRAYSIZE(rt.export_diff_path));
      ImGui::SameLine();
      if (ImGui::Button("Export diff (JSON)##tm")) {
        if (a_idx < 0 || b_idx < 0 || a_idx >= n || b_idx >= n) {
          rt.last_error = "Export diff: invalid snapshot indices.";
        } else {
          try {
            nebula4x::SaveDiffOptions opt;
            opt.max_changes = ui.time_machine_max_changes;
            const std::string diff_json = nebula4x::diff_saves_to_json(rt.snapshots[a_idx].json_text, rt.snapshots[b_idx].json_text, opt);
            nebula4x::write_text_file(rt.export_diff_path, diff_json);
            rt.last_error.clear();
          } catch (const std::exception& e) {
            rt.last_error = std::string("Export diff failed: ") + e.what();
          }
        }
      }

      ImGui::SameLine();
      if (ImGui::Button("Copy diff (text)##tm")) {
        if (a_idx < 0 || b_idx < 0 || a_idx >= n || b_idx >= n) {
          // no-op
        } else {
          try {
            nebula4x::SaveDiffOptions opt;
            opt.max_changes = ui.time_machine_max_changes;
            opt.max_value_chars = ui.time_machine_max_value_chars;
            const std::string diff_text = nebula4x::diff_saves_to_text(rt.snapshots[a_idx].json_text, rt.snapshots[b_idx].json_text, opt);
            ImGui::SetClipboardText(diff_text.c_str());
          } catch (...) {
            // ignore
          }
        }
      }
    }

    {
      ImGui::SetNextItemWidth(260.0f);
      ImGui::InputText("Patch path##tm", rt.export_patch_path, IM_ARRAYSIZE(rt.export_patch_path));
      ImGui::SameLine();
      if (ImGui::Button("Export patch (RFC6902)##tm")) {
        if (a_idx < 0 || b_idx < 0 || a_idx >= n || b_idx >= n) {
          rt.last_error = "Export patch: invalid snapshot indices.";
        } else {
          try {
            const std::string patch = nebula4x::diff_saves_to_json_patch(rt.snapshots[a_idx].json_text, rt.snapshots[b_idx].json_text,
                                                                         nebula4x::JsonPatchOptions{.max_ops = 0, .indent = 2});
            nebula4x::write_text_file(rt.export_patch_path, patch);
            rt.last_error.clear();
          } catch (const std::exception& e) {
            rt.last_error = std::string("Export patch failed: ") + e.what();
          }
        }
      }
      ImGui::SameLine();
      if (ImGui::Button("Copy patch##tm")) {
        if (a_idx < 0 || b_idx < 0 || a_idx >= n || b_idx >= n) {
          // no-op
        } else {
          try {
            const std::string patch = nebula4x::diff_saves_to_json_patch(rt.snapshots[a_idx].json_text, rt.snapshots[b_idx].json_text,
                                                                         nebula4x::JsonPatchOptions{.max_ops = 0, .indent = 2});
            ImGui::SetClipboardText(patch.c_str());
          } catch (...) {
            // ignore
          }
        }
      }
    }

    ImGui::Separator();

    // --- Filters ---
    ImGui::SetNextItemWidth(280.0f);
    ImGui::InputTextWithHint("Path filter##tm", "e.g. /systems", rt.path_filter, IM_ARRAYSIZE(rt.path_filter));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(280.0f);
    ImGui::InputTextWithHint("Value filter##tm", "text in before/after", rt.value_filter, IM_ARRAYSIZE(rt.value_filter));

    const std::string path_need = rt.path_filter;
    const std::string val_need = rt.value_filter;

    // --- Build filtered change list pointer ---
    const std::vector<DiffChange>* changes = nullptr;
    bool truncated = false;

    if (rt.compare_mode == 0) {
      if (rt.selected_idx > 0) {
        changes = &cur.diff_prev;
        truncated = cur.diff_prev_truncated;
      }
    } else {
      if (dv_ptr && dv_ptr->valid) {
        changes = &dv_ptr->changes;
        truncated = dv_ptr->truncated;
      }
    }

    if (!changes) {
      ImGui::TextDisabled("No diff to display.");
      ImGui::EndChild();
      ImGui::End();
      return;
    }

    // Build a filtered index list. ImGuiListClipper expects the loop body to
    // submit a consistent number of rows; filtering inside the clipper loop can
    // lead to empty space or incorrect clipping.
    std::vector<int> visible;
    visible.reserve(changes->size());
    for (int i = 0; i < static_cast<int>(changes->size()); ++i) {
      const DiffChange& c = (*changes)[i];
      if (!op_visible(c, rt)) continue;
      if (!contains_text(c.path, path_need, rt.filter_case_sensitive)) continue;
      if (!val_need.empty()) {
        const bool ok = contains_text(c.before, val_need, rt.filter_case_sensitive) ||
                        contains_text(c.after, val_need, rt.filter_case_sensitive);
        if (!ok) continue;
      }
      visible.push_back(i);
    }

    // Table of changes.
    ImGui::TextDisabled("Changes shown: %d%s | Visible: %d", static_cast<int>(changes->size()), truncated ? "+" : "",
                        static_cast<int>(visible.size()));

    ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
                         ImGuiTableFlags_ScrollY | ImGuiTableFlags_Hideable;

    const float table_h = std::max(180.0f, ImGui::GetContentRegionAvail().y);
    if (ImGui::BeginTable("##tm_changes", 4, tf, ImVec2(0, table_h))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Op", ImGuiTableColumnFlags_WidthFixed, 55.0f);
      ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch, 0.0f);
      ImGui::TableSetupColumn("Before", ImGuiTableColumnFlags_WidthStretch, 0.0f);
      ImGui::TableSetupColumn("After", ImGuiTableColumnFlags_WidthStretch, 0.0f);
      ImGui::TableHeadersRow();

      // We expect a few hundred rows at most, but clip anyway.
      ImGuiListClipper clipper;
      clipper.Begin(static_cast<int>(visible.size()));
      while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
          const int idx = visible[row];
          const DiffChange& c = (*changes)[idx];

          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          ImGui::TextUnformatted(c.op.c_str());

          ImGui::TableSetColumnIndex(1);
          ImGui::PushID(idx);
          const bool clicked = ImGui::Selectable(c.path.c_str(), false, ImGuiSelectableFlags_SpanAllColumns);
          if (clicked) {
            // Single-click: focus JSON Explorer on this path.
            ui.show_json_explorer_window = true;
            ui.request_json_explorer_goto_path = c.path;
          }

          if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Go to in JSON Explorer")) {
              ui.show_json_explorer_window = true;
              ui.request_json_explorer_goto_path = c.path;
            }
            if (ImGui::MenuItem("Pin to Watchboard")) {
              ui.show_watchboard_window = true;
              (void)add_watch_item(ui, c.path, c.path, true, true, 120);
            }
            if (ImGui::MenuItem("Copy path")) {
              ImGui::SetClipboardText(c.path.c_str());
            }
            ImGui::EndPopup();
          }

          ImGui::TableSetColumnIndex(2);
          ImGui::TextUnformatted(c.before.c_str());
          ImGui::TableSetColumnIndex(3);
          ImGui::TextUnformatted(c.after.c_str());
          ImGui::PopID();
        }
      }

      ImGui::EndTable();
    }
  }
  ImGui::EndChild();

  ImGui::End();
}

} // namespace nebula4x::ui