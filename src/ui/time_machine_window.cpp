#include "ui/time_machine_window.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <list>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nebula4x/core/serialization.h"

#include "nebula4x/util/file_io.h"
#include "nebula4x/util/json.h"
#include "nebula4x/util/json_merge_patch.h"
#include "nebula4x/util/log.h"
#include "nebula4x/util/save_delta.h"
#include "nebula4x/util/save_diff.h"
#include "nebula4x/util/time.h"

#include "ui/game_json_cache.h"
#include "ui/watchboard_window.h"

namespace nebula4x::ui {
namespace {

constexpr int kStorageModeFull = 0;
constexpr int kStorageModeDeltaMergePatch = 1;
constexpr int kStorageModeDeltaJsonPatch = 2;

bool is_delta_storage_mode(int mode) {
  return mode != kStorageModeFull;
}

bool is_merge_patch_delta_mode(int mode) {
  return mode == kStorageModeDeltaMergePatch;
}

bool is_json_patch_delta_mode(int mode) {
  return mode == kStorageModeDeltaJsonPatch;
}

std::string unknown_delta_storage_mode_msg(int mode) {
  return "unknown delta storage mode: " + std::to_string(mode) + " (valid: 0=full, 1=merge-patch, 2=json-patch)";
}

[[noreturn]] void throw_unknown_delta_storage_mode(int mode) {
  throw std::runtime_error("Time Machine: " + unknown_delta_storage_mode_msg(mode));
}

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
  if (v.is_bool()) return v.bool_value(false) ? "true" : "false";
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

  // Storage:
  // - In Full mode: every snapshot stores full save-game JSON in `json_text`.
  // - In Delta modes: `json_text` is stored only for checkpoint snapshots
  //   (including snapshot[0]) and other snapshots store only `delta_patch`.
  std::string json_text;

  // Delta patch that transforms (snapshot[i-1]) -> (snapshot[i]).
  // Present in Delta modes. snapshot[0] has none.
  //
  // Patch encoding depends on TimeMachineRuntime::stored_storage_mode:
  //  - DeltaMergePatch: RFC 7396 JSON Merge Patch
  //  - DeltaJsonPatch:  RFC 6902 JSON Patch (array of op objects)
  bool has_delta_patch{false};
  nebula4x::json::Value delta_patch;
  std::size_t delta_patch_bytes{0};

  // Diff vs previous snapshot (Prev mode). Snapshot[0] has none.
  bool diff_prev_truncated{false};
  std::vector<DiffChange> diff_prev;
};

struct CachedJson {
  int idx{-1};
  std::string json_text;
};

struct TimeMachineRuntime {
  bool initialized{false};
  std::uint64_t last_seen_state_generation{0};
  std::uint64_t next_snapshot_id{1};

  // Storage settings currently applied to the stored history.
  int stored_storage_mode{kStorageModeDeltaMergePatch};
  int stored_checkpoint_stride{8};

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
  char export_merge_patch_path[256] = "saves/time_machine_merge_patch.json";
  char export_delta_save_path[256] = "saves/time_machine_history.delta.json";
  bool export_delta_include_digests{false};

  // Cached computed diff for Baseline mode.
  int cached_a{-1};
  int cached_b{-1};
  int cached_max_changes{0};
  int cached_preview_chars{0};
  DiffView cached_diff;

  // Reconstruction cache (delta mode).
  int json_cache_max_entries{4};
  std::list<CachedJson> json_cache;

  // Last full snapshot JSON (for fast change detection + delta computation).
  std::string last_snapshot_json;

  // Runtime errors (not persisted).
  std::string last_error;

  std::vector<Snapshot> snapshots;
};

std::size_t total_stored_json_bytes(const std::vector<Snapshot>& snaps) {
  std::size_t sum = 0;
  for (const auto& s : snaps) sum += s.json_text.size();
  return sum;
}

std::size_t total_stored_patch_bytes(const std::vector<Snapshot>& snaps) {
  std::size_t sum = 0;
  for (const auto& s : snaps) sum += s.delta_patch_bytes;
  return sum;
}

void clear_reconstruction_cache(TimeMachineRuntime& rt) {
  rt.json_cache.clear();
}

void clear_history(TimeMachineRuntime& rt) {
  rt.snapshots.clear();
  rt.selected_idx = 0;
  rt.baseline_idx = 0;
  rt.cached_a = -1;
  rt.cached_b = -1;
  rt.cached_diff = DiffView{};
  rt.last_snapshot_json.clear();
  clear_reconstruction_cache(rt);
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

// Return the full snapshot JSON for index `idx`.
//
// In Full mode or for checkpoint snapshots, this returns the stored string.
// In Delta modes for non-checkpoints, this reconstructs via delta patches.
const std::string& snapshot_json(TimeMachineRuntime& rt, int idx) {
  static const std::string kEmpty;

  if (idx < 0 || idx >= static_cast<int>(rt.snapshots.size())) {
    rt.last_error = "Snapshot index out of range.";
    return kEmpty;
  }

  Snapshot& s = rt.snapshots[static_cast<std::size_t>(idx)];
  if (!s.json_text.empty()) return s.json_text;

  if (rt.stored_storage_mode == kStorageModeFull) {
    // Shouldn't happen (full mode stores every JSON), but be defensive.
    return s.json_text;
  }

  // Cache lookup.
  for (auto it = rt.json_cache.begin(); it != rt.json_cache.end(); ++it) {
    if (it->idx == idx) {
      // Touch LRU: move to back.
      rt.json_cache.splice(rt.json_cache.end(), rt.json_cache, it);
      return rt.json_cache.back().json_text;
    }
  }

  // Reconstruct from the nearest prior checkpoint.
  try {
    int start = idx;
    while (start > 0 && rt.snapshots[static_cast<std::size_t>(start)].json_text.empty()) {
      --start;
    }

    const std::string& base_txt = rt.snapshots[static_cast<std::size_t>(start)].json_text;
    if (base_txt.empty()) {
      rt.last_error = "Time Machine: missing base/checkpoint snapshot JSON.";
      return kEmpty;
    }

    nebula4x::json::Value doc = nebula4x::json::parse(base_txt);
    for (int i = start + 1; i <= idx; ++i) {
      Snapshot& step = rt.snapshots[static_cast<std::size_t>(i)];
      if (!step.has_delta_patch) {
        rt.last_error = "Time Machine: missing delta patch for snapshot " + std::to_string(i) + ".";
        return kEmpty;
      }
      if (is_merge_patch_delta_mode(rt.stored_storage_mode)) {
        nebula4x::apply_json_merge_patch(doc, step.delta_patch);
      } else if (is_json_patch_delta_mode(rt.stored_storage_mode)) {
        nebula4x::apply_json_patch(doc, step.delta_patch);
      } else {
        rt.last_error = "Time Machine: " + unknown_delta_storage_mode_msg(rt.stored_storage_mode) + ".";
        return kEmpty;
      }
    }

    std::string out = nebula4x::json::stringify(doc, 2);

    // Insert into reconstruction cache.
    if (rt.json_cache_max_entries < 1) rt.json_cache_max_entries = 1;
    rt.json_cache.push_back(CachedJson{idx, std::move(out)});
    while (static_cast<int>(rt.json_cache.size()) > rt.json_cache_max_entries) {
      rt.json_cache.pop_front();
    }
    return rt.json_cache.back().json_text;
  } catch (const std::exception& e) {
    rt.last_error = std::string("Reconstruct failed: ") + e.what();
    return kEmpty;
  }
}

std::string snapshot_json_copy(TimeMachineRuntime& rt, int idx) {
  const std::string& s = snapshot_json(rt, idx);
  return s;
}

void trim_history(TimeMachineRuntime& rt, int keep) {
  keep = std::clamp(keep, 1, 1000000);
  if (static_cast<int>(rt.snapshots.size()) <= keep) return;

  const int to_remove = static_cast<int>(rt.snapshots.size()) - keep;
  std::string new_base_json;
  if (is_delta_storage_mode(rt.stored_storage_mode)) {
    // Capture the full JSON for what will become the new base snapshot.
    new_base_json = snapshot_json_copy(rt, to_remove);
  }

  rt.snapshots.erase(rt.snapshots.begin(), rt.snapshots.begin() + to_remove);

  // New first snapshot has no previous diff.
  if (!rt.snapshots.empty()) {
    rt.snapshots.front().diff_prev.clear();
    rt.snapshots.front().diff_prev_truncated = false;

    if (is_delta_storage_mode(rt.stored_storage_mode)) {
      // Ensure the new base is a real checkpoint with no incoming patch.
      rt.snapshots.front().json_text = std::move(new_base_json);
      rt.snapshots.front().has_delta_patch = false;
      rt.snapshots.front().delta_patch = is_json_patch_delta_mode(rt.stored_storage_mode) ? nebula4x::json::array({})
                                                                                        : nebula4x::json::object({});
      rt.snapshots.front().delta_patch_bytes = 0;
    }
  }

  rt.selected_idx = std::max(0, rt.selected_idx - to_remove);
  rt.baseline_idx = std::max(0, rt.baseline_idx - to_remove);
  rt.cached_a = -1;
  rt.cached_b = -1;
  clear_reconstruction_cache(rt);
  clamp_indices(rt);

  // Removing from the front does not change the last snapshot; keep last_snapshot_json unless history is empty.
  if (rt.snapshots.empty()) {
    rt.last_snapshot_json.clear();
  }
}

void truncate_newer(TimeMachineRuntime& rt, int keep_up_to_idx) {
  const int n = static_cast<int>(rt.snapshots.size());
  if (n <= 0) return;
  keep_up_to_idx = std::clamp(keep_up_to_idx, 0, n - 1);
  if (keep_up_to_idx == n - 1) return;

  rt.snapshots.erase(rt.snapshots.begin() + (keep_up_to_idx + 1), rt.snapshots.end());
  rt.cached_a = -1;
  rt.cached_b = -1;
  clear_reconstruction_cache(rt);

  rt.selected_idx = std::clamp(rt.selected_idx, 0, static_cast<int>(rt.snapshots.size()) - 1);
  rt.baseline_idx = std::clamp(rt.baseline_idx, 0, static_cast<int>(rt.snapshots.size()) - 1);

  // Update last snapshot JSON.
  if (!rt.snapshots.empty()) {
    rt.last_snapshot_json = snapshot_json_copy(rt, static_cast<int>(rt.snapshots.size()) - 1);
  } else {
    rt.last_snapshot_json.clear();
  }
}

// Convert the stored history into `new_mode` representation.
//
// This preserves snapshot metadata and diffs, but can:
//  - drop stored JSON for non-checkpoints in delta mode
//  - compute missing merge patches when switching from full->delta
void convert_history_storage(TimeMachineRuntime& rt, int new_mode, int new_stride) {
  new_mode = std::clamp(new_mode, 0, 2);
  new_stride = std::clamp(new_stride, 1, 128);

  if (rt.snapshots.empty()) {
    rt.stored_storage_mode = new_mode;
    rt.stored_checkpoint_stride = new_stride;
    clear_reconstruction_cache(rt);
    return;
  }

  const bool new_is_delta = is_delta_storage_mode(new_mode);
  if (rt.stored_storage_mode == new_mode && (!new_is_delta || rt.stored_checkpoint_stride == new_stride)) {
    return;
  }

  // Materialize full JSON for every snapshot in the current history.
  const int n = static_cast<int>(rt.snapshots.size());
  std::vector<std::string> full_json;
  full_json.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    full_json.push_back(snapshot_json_copy(rt, i));
  }

  std::vector<Snapshot> new_snaps = rt.snapshots;

  if (!new_is_delta) {
    for (int i = 0; i < n; ++i) {
      Snapshot& s = new_snaps[static_cast<std::size_t>(i)];
      s.json_text = std::move(full_json[static_cast<std::size_t>(i)]);
      s.has_delta_patch = false;
      s.delta_patch = nebula4x::json::object({});
      s.delta_patch_bytes = 0;
    }
  } else {
    // Delta modes.
    for (int i = 0; i < n; ++i) {
      Snapshot& s = new_snaps[static_cast<std::size_t>(i)];

      const bool is_checkpoint = (i == 0) || (new_stride <= 1) || ((i % new_stride) == 0);
      s.json_text = is_checkpoint ? full_json[static_cast<std::size_t>(i)] : std::string();

      if (i == 0) {
        s.has_delta_patch = false;
        s.delta_patch = is_json_patch_delta_mode(new_mode) ? nebula4x::json::array({}) : nebula4x::json::object({});
        s.delta_patch_bytes = 0;
        continue;
      }

      if (rt.stored_storage_mode == new_mode && rt.snapshots[static_cast<std::size_t>(i)].has_delta_patch) {
        // Reuse existing patches when only changing checkpoint stride.
        s.has_delta_patch = true;
        s.delta_patch = rt.snapshots[static_cast<std::size_t>(i)].delta_patch;
        s.delta_patch_bytes = rt.snapshots[static_cast<std::size_t>(i)].delta_patch_bytes;
      } else {
        try {
          // Compute delta patch from previous -> current.
          const std::string& from_json = full_json[static_cast<std::size_t>(i - 1)];
          const std::string& to_json = full_json[static_cast<std::size_t>(i)];
          s.has_delta_patch = true;

          if (is_merge_patch_delta_mode(new_mode)) {
            const nebula4x::json::Value from = nebula4x::json::parse(from_json);
            const nebula4x::json::Value to = nebula4x::json::parse(to_json);
            s.delta_patch = nebula4x::diff_json_merge_patch(from, to);
            s.delta_patch_bytes = nebula4x::json::stringify(s.delta_patch, 0).size();
          } else if (is_json_patch_delta_mode(new_mode)) {
            nebula4x::JsonPatchOptions jopt;
            jopt.indent = 0;
            const std::string patch_json = nebula4x::diff_saves_to_json_patch(from_json, to_json, jopt);
            s.delta_patch = nebula4x::json::parse(patch_json);
            s.delta_patch_bytes = patch_json.size();
          } else {
            throw_unknown_delta_storage_mode(new_mode);
          }
        } catch (const std::exception& e) {
          s.has_delta_patch = false;
          s.delta_patch = is_json_patch_delta_mode(new_mode) ? nebula4x::json::array({}) : nebula4x::json::object({});
          s.delta_patch_bytes = 0;
          rt.last_error = std::string("Storage conversion: patch compute failed: ") + e.what();
        }
      }
    }

    // Ensure base is a checkpoint.
    if (!new_snaps.empty() && new_snaps.front().json_text.empty()) {
      new_snaps.front().json_text = snapshot_json_copy(rt, 0);
    }
  }

  rt.snapshots = std::move(new_snaps);
  rt.stored_storage_mode = new_mode;
  rt.stored_checkpoint_stride = new_stride;
  rt.cached_a = -1;
  rt.cached_b = -1;
  clear_reconstruction_cache(rt);

  // Refresh last snapshot JSON.
  rt.last_snapshot_json = snapshot_json_copy(rt, static_cast<int>(rt.snapshots.size()) - 1);
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
  if (!rt.snapshots.empty()) {
    if (rt.last_snapshot_json == txt) {
      return false; // No change.
    }
  }

  Snapshot snap;
  snap.id = rt.next_snapshot_id++;
  snap.state_generation = sim.state_generation();
  snap.cache_revision = c.revision;
  snap.day = sim.state().date.days_since_epoch();
  snap.hour = sim.state().hour_of_day;

  const int new_index = static_cast<int>(rt.snapshots.size());
  const bool is_delta = is_delta_storage_mode(rt.stored_storage_mode);

  // Determine whether to store the full JSON for this snapshot.
  const bool is_checkpoint = (!is_delta) || (new_index == 0) || (ui.time_machine_checkpoint_stride <= 1) ||
                             ((new_index % ui.time_machine_checkpoint_stride) == 0);
  if (!is_delta || is_checkpoint) {
    snap.json_text = txt;
  }

  // Compute diff vs previous snapshot (Prev mode) and merge patch (delta mode).
  if (!rt.snapshots.empty()) {
    const std::string& prev_json = rt.last_snapshot_json;

    // Diff preview.
    DiffView dv = compute_diff_view(prev_json, txt, ui.time_machine_max_changes, ui.time_machine_max_value_chars);
    if (dv.valid) {
      snap.diff_prev_truncated = dv.truncated;
      snap.diff_prev = std::move(dv.changes);
    } else {
      // Keep snapshot but record the error for visibility.
      snap.diff_prev_truncated = false;
      snap.diff_prev.clear();
      rt.last_error = "Diff error: " + dv.error;
    }

    if (is_delta) {
      try {
        snap.has_delta_patch = true;
        if (is_merge_patch_delta_mode(rt.stored_storage_mode)) {
          const nebula4x::json::Value from = nebula4x::json::parse(prev_json);
          snap.delta_patch = nebula4x::diff_json_merge_patch(from, *c.root);
          snap.delta_patch_bytes = nebula4x::json::stringify(snap.delta_patch, 0).size();
        } else if (is_json_patch_delta_mode(rt.stored_storage_mode)) {
          nebula4x::JsonPatchOptions jopt;
          jopt.indent = 0;
          const std::string patch_json = nebula4x::diff_saves_to_json_patch(prev_json, txt, jopt);
          snap.delta_patch = nebula4x::json::parse(patch_json);
          snap.delta_patch_bytes = patch_json.size();
        } else {
          throw_unknown_delta_storage_mode(rt.stored_storage_mode);
        }
      } catch (const std::exception& e) {
        snap.has_delta_patch = false;
        snap.delta_patch = is_json_patch_delta_mode(rt.stored_storage_mode) ? nebula4x::json::array({})
                                                                           : nebula4x::json::object({});
        snap.delta_patch_bytes = 0;
        rt.last_error = std::string("Delta patch error: ") + e.what();
      }
    }
  }

  const bool was_at_latest = (rt.selected_idx == static_cast<int>(rt.snapshots.size()) - 1);
  rt.snapshots.push_back(std::move(snap));

  // Keep last snapshot JSON for fast change detection and future patch/diff.
  rt.last_snapshot_json = txt;

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
    rt.stored_storage_mode = std::clamp(ui.time_machine_storage_mode, 0, 2);
    rt.stored_checkpoint_stride = std::clamp(ui.time_machine_checkpoint_stride, 1, 128);
  }

  // If the underlying state was replaced externally (new game, load), clear history.
  // When the user uses the Time Machine's own "Load snapshot" button, we update
  // last_seen_state_generation immediately so we preserve history.
  const std::uint64_t gen = sim.state_generation();
  if (rt.last_seen_state_generation != 0 && gen != rt.last_seen_state_generation) {
    clear_history(rt);
    rt.last_seen_state_generation = gen;
  }

  // Clamp UI knobs.
  ui.time_machine_refresh_sec = std::clamp(ui.time_machine_refresh_sec, 0.05f, 30.0f);
  ui.time_machine_keep_snapshots = std::clamp(ui.time_machine_keep_snapshots, 1, 512);
  ui.time_machine_max_changes = std::clamp(ui.time_machine_max_changes, 1, 50000);
  ui.time_machine_max_value_chars = std::clamp(ui.time_machine_max_value_chars, 16, 2000);
  ui.time_machine_storage_mode = std::clamp(ui.time_machine_storage_mode, 0, 2);
  ui.time_machine_checkpoint_stride = std::clamp(ui.time_machine_checkpoint_stride, 1, 128);

  // Apply storage mode/stride changes.
  if (rt.stored_storage_mode != ui.time_machine_storage_mode ||
      (is_delta_storage_mode(ui.time_machine_storage_mode) && rt.stored_checkpoint_stride != ui.time_machine_checkpoint_stride)) {
    convert_history_storage(rt, ui.time_machine_storage_mode, ui.time_machine_checkpoint_stride);
  }

  // Auto-recording.
  if (ui.time_machine_recording) {
    (void)capture_snapshot(rt, sim, ui, /*force_refresh=*/false);
  }

  ImGui::SetNextWindowSize(ImVec2(1020, 760), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Time Machine", &ui.show_time_machine_window)) {
    ImGui::End();
    return;
  }

  // --- Controls ---
  {
    ImGui::Checkbox("Recording##tm", &ui.time_machine_recording);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragFloat("Refresh (s)##tm", &ui.time_machine_refresh_sec, 0.05f, 0.05f, 30.0f, "%.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragInt("Keep##tm", &ui.time_machine_keep_snapshots, 1.0f, 1, 512);
    if (ImGui::IsItemHovered()) {
      if (ui.time_machine_storage_mode == kStorageModeFull) {
        ImGui::SetTooltip(
            "In Full mode, snapshots are stored as full save-game JSON text.\n"
            "Reduce this value if memory usage is high.");
      } else if (is_merge_patch_delta_mode(ui.time_machine_storage_mode)) {
        ImGui::SetTooltip(
            "In Delta (Merge Patch) mode, the Time Machine stores RFC 7396 JSON Merge Patches\n"
            "between snapshots and keeps periodic full checkpoints for fast random access.\n"
            "Arrays replace wholesale.\n"
            "You can usually increase Keep substantially compared to Full mode.");
      } else {
        ImGui::SetTooltip(
            "In Delta (JSON Patch) mode, the Time Machine stores RFC 6902 JSON Patch operations\n"
            "between snapshots and keeps periodic full checkpoints for fast random access.\n"
            "This tends to be more space-efficient for small changes inside large arrays.\n"
            "You can usually increase Keep substantially compared to Full mode.");
      }
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

    ImGui::SameLine();
    const char* storage_items[] = {"Full JSON", "Delta (Merge Patch)", "Delta (JSON Patch)"};
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::Combo("Storage##tm", &ui.time_machine_storage_mode, storage_items, IM_ARRAYSIZE(storage_items))) {
      convert_history_storage(rt, ui.time_machine_storage_mode, ui.time_machine_checkpoint_stride);
    }
    if (is_delta_storage_mode(ui.time_machine_storage_mode)) {
      ImGui::SameLine();
      ImGui::SetNextItemWidth(120.0f);
      if (ImGui::DragInt("Checkpoint##tm", &ui.time_machine_checkpoint_stride, 1.0f, 1, 128)) {
        convert_history_storage(rt, ui.time_machine_storage_mode, ui.time_machine_checkpoint_stride);
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Checkpoint stride for Delta modes.\n"
            "A full JSON checkpoint is stored every N snapshots; other snapshots store only a delta patch.\n"
            "Lower values increase memory usage but make random access faster.");
      }
    }
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

  const float left_w = 350.0f;
  ImGui::BeginChild("##tm_left", ImVec2(left_w, 0), true);
  {
    const int n = static_cast<int>(rt.snapshots.size());
    ImGui::TextDisabled("Snapshots: %d", n);

    const std::size_t json_bytes = total_stored_json_bytes(rt.snapshots);
    const std::size_t patch_bytes = total_stored_patch_bytes(rt.snapshots);
    const std::size_t total_bytes = json_bytes + patch_bytes;

    ImGui::SameLine();
    if (is_delta_storage_mode(ui.time_machine_storage_mode)) {
      ImGui::TextDisabled(" | JSON: %.1f MB | Patches: %.1f MB | Total: %.1f MB", static_cast<double>(json_bytes) / (1024.0 * 1024.0),
                           static_cast<double>(patch_bytes) / (1024.0 * 1024.0), static_cast<double>(total_bytes) / (1024.0 * 1024.0));
    } else {
      ImGui::TextDisabled(" | Memory: %.1f MB", static_cast<double>(json_bytes) / (1024.0 * 1024.0));
    }

    if (ImGui::Button("Pin JSON Explorer to baseline##tm")) {
      if (rt.baseline_idx >= 0 && rt.baseline_idx < n) {
        ui.show_json_explorer_window = true;
        ui.request_json_explorer_goto_path = "/";
      }
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Opens the JSON Explorer (Explorer always shows the live state).\n"
          "Use diff rows to jump to specific paths.");
    }

    ImGui::Separator();

    if (n == 0) {
      ImGui::TextWrapped(
          "No snapshots recorded yet.\n\n"
          "- Enable 'Recording' to auto-capture while you advance turns.\n"
          "- Or click 'Capture now' to grab a snapshot immediately.");
    } else {
      // Keep indices valid.
      clamp_indices(rt);

      ImGuiListClipper clipper;
      clipper.Begin(n);
      while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
          const Snapshot& s = rt.snapshots[static_cast<std::size_t>(i)];
          const std::string dt = nebula4x::format_datetime(s.day, s.hour);
          const int delta = (i == 0) ? 0 : static_cast<int>(s.diff_prev.size());
          const bool trunc = (i == 0) ? false : s.diff_prev_truncated;

          const bool is_ckpt = is_delta_storage_mode(rt.stored_storage_mode) && !s.json_text.empty();

          char label[320];
          if (i == rt.baseline_idx) {
            std::snprintf(label, sizeof(label), "[%d] %s  (BASE)%s", i, dt.c_str(), is_ckpt ? " [C]" : "");
          } else if (i == 0) {
            std::snprintf(label, sizeof(label), "[%d] %s%s", i, dt.c_str(), is_ckpt ? " [C]" : "");
          } else {
            if (trunc) {
              std::snprintf(label, sizeof(label), "[%d] %s  (Δ %d+)%s", i, dt.c_str(), delta, is_ckpt ? " [C]" : "");
            } else {
              std::snprintf(label, sizeof(label), "[%d] %s  (Δ %d)%s", i, dt.c_str(), delta, is_ckpt ? " [C]" : "");
            }
          }

          if (ImGui::Selectable(label, rt.selected_idx == i)) {
            rt.selected_idx = i;
          }

          if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("Snapshot %d", i);
            if (rt.stored_storage_mode == kStorageModeFull) {
              ImGui::Text("Stored JSON: %.1f KB", static_cast<double>(s.json_text.size()) / 1024.0);
            } else {
              if (!s.json_text.empty()) {
                ImGui::Text("Checkpoint JSON: %.1f KB", static_cast<double>(s.json_text.size()) / 1024.0);
              } else {
                ImGui::Text("Checkpoint JSON: (none)");
              }
              if (i > 0 && s.has_delta_patch) {
                if (is_json_patch_delta_mode(rt.stored_storage_mode)) {
                  ImGui::Text("JSON patch: %.1f KB", static_cast<double>(s.delta_patch_bytes) / 1024.0);
                } else {
                  ImGui::Text("Merge patch: %.1f KB", static_cast<double>(s.delta_patch_bytes) / 1024.0);
                }
              }
            }
            ImGui::EndTooltip();
          }

          if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Set baseline here")) {
              rt.baseline_idx = i;
            }
            if (ImGui::MenuItem("Branch here (truncate newer)")) {
              truncate_newer(rt, i);
              rt.last_error.clear();
            }
            if (ImGui::MenuItem("Load this snapshot")) {
              try {
                const std::string json_txt = snapshot_json_copy(rt, i);
                sim.load_game(deserialize_game_from_json(json_txt));
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
              const std::string json_txt = snapshot_json_copy(rt, i);
              ImGui::SetClipboardText(json_txt.c_str());
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
    const Snapshot& cur = rt.snapshots[static_cast<std::size_t>(rt.selected_idx)];

    const std::string dt = nebula4x::format_datetime(cur.day, cur.hour);
    const std::string cur_json = snapshot_json_copy(rt, rt.selected_idx);

    ImGui::Text("Selected: [%d] %s", rt.selected_idx, dt.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled(" | JSON: %.1f KB", static_cast<double>(cur_json.size()) / 1024.0);

    if (is_delta_storage_mode(rt.stored_storage_mode)) {
      ImGui::SameLine();
      if (!cur.json_text.empty()) {
        ImGui::TextDisabled(" | Stored: checkpoint");
      } else {
        ImGui::TextDisabled(" | Stored: patch-only");
      }
    }

    // --- Actions ---
    if (ImGui::Button("Load snapshot##tm")) {
      try {
        sim.load_game(deserialize_game_from_json(cur_json));
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
    if (ImGui::Button("Branch here##tm")) {
      truncate_newer(rt, rt.selected_idx);
      rt.last_error.clear();
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Truncate newer snapshots to continue recording from the selected state.");
    }

    ImGui::SameLine();
    if (ImGui::Button("Copy JSON##tm")) {
      ImGui::SetClipboardText(cur_json.c_str());
    }

    ImGui::SameLine();
    if (ImGui::Button("Export JSON##tm")) {
      try {
        nebula4x::write_text_file(rt.export_snapshot_path, cur_json);
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

      const bool cache_ok =
          (rt.cached_a == a_idx && rt.cached_b == b_idx && rt.cached_max_changes == ui.time_machine_max_changes && rt.cached_preview_chars == ui.time_machine_max_value_chars);
      if (!cache_ok) {
        rt.cached_a = a_idx;
        rt.cached_b = b_idx;
        rt.cached_max_changes = ui.time_machine_max_changes;
        rt.cached_preview_chars = ui.time_machine_max_value_chars;
        rt.cached_diff = compute_diff_view(snapshot_json(rt, a_idx), snapshot_json(rt, b_idx), ui.time_machine_max_changes, ui.time_machine_max_value_chars);
      }
      dv_ptr = &rt.cached_diff;
      if (!rt.cached_diff.valid && !rt.cached_diff.error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "Diff error: %s", rt.cached_diff.error.c_str());
      }
    }

    // Export/copy diff + patches for the current comparison.
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
            const std::string diff_json = nebula4x::diff_saves_to_json(snapshot_json(rt, a_idx), snapshot_json(rt, b_idx), opt);
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
            const std::string diff_text = nebula4x::diff_saves_to_text(snapshot_json(rt, a_idx), snapshot_json(rt, b_idx), opt);
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
            nebula4x::JsonPatchOptions jopt;
            jopt.max_ops = 0;
            jopt.indent = 2;
            const std::string patch = nebula4x::diff_saves_to_json_patch(snapshot_json(rt, a_idx), snapshot_json(rt, b_idx), jopt);
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
            nebula4x::JsonPatchOptions jopt;
            jopt.max_ops = 0;
            jopt.indent = 2;
            const std::string patch = nebula4x::diff_saves_to_json_patch(snapshot_json(rt, a_idx), snapshot_json(rt, b_idx), jopt);
            ImGui::SetClipboardText(patch.c_str());
          } catch (...) {
            // ignore
          }
        }
      }
    }

    {
      ImGui::SetNextItemWidth(260.0f);
      ImGui::InputText("Merge patch path##tm", rt.export_merge_patch_path, IM_ARRAYSIZE(rt.export_merge_patch_path));
      ImGui::SameLine();
      if (ImGui::Button("Export merge patch (RFC7396)##tm")) {
        if (a_idx < 0 || b_idx < 0 || a_idx >= n || b_idx >= n) {
          rt.last_error = "Export merge patch: invalid snapshot indices.";
        } else {
          try {
            const std::string patch = nebula4x::diff_json_merge_patch(snapshot_json(rt, a_idx), snapshot_json(rt, b_idx), /*indent=*/2);
            nebula4x::write_text_file(rt.export_merge_patch_path, patch);
            rt.last_error.clear();
          } catch (const std::exception& e) {
            rt.last_error = std::string("Export merge patch failed: ") + e.what();
          }
        }
      }
      ImGui::SameLine();
      if (ImGui::Button("Copy merge patch##tm")) {
        if (a_idx < 0 || b_idx < 0 || a_idx >= n || b_idx >= n) {
          // no-op
        } else {
          try {
            const std::string patch = nebula4x::diff_json_merge_patch(snapshot_json(rt, a_idx), snapshot_json(rt, b_idx), /*indent=*/2);
            ImGui::SetClipboardText(patch.c_str());
          } catch (...) {
            // ignore
          }
        }
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("RFC 7396 JSON Merge Patch (compact structural delta).");
      }
    }

    if (ImGui::CollapsingHeader("Export history (delta-save)", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::Checkbox("Include digests##tm_delta", &rt.export_delta_include_digests);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(280.0f);
      ImGui::InputText("Delta-save path##tm", rt.export_delta_save_path, IM_ARRAYSIZE(rt.export_delta_save_path));
      if (ImGui::IsItemHovered()) {
        if (is_json_patch_delta_mode(rt.stored_storage_mode)) {
          ImGui::SetTooltip(
              "Exports a delta-save file: { base, patches[] } where patches are RFC 6902 JSON Patch arrays.\n"
              "This can be more space-efficient when large arrays change slightly.\n"
              "Compatible with the CLI delta-save tooling.");
        } else {
          ImGui::SetTooltip(
              "Exports a delta-save file: { base, patches[] } where patches are RFC 7396 JSON Merge Patches.\n"
              "Compatible with the CLI delta-save tooling.");
        }
      }

      const auto export_delta_range = [&](int start_idx, int end_idx) {
        if (start_idx < 0 || end_idx < 0 || start_idx >= n || end_idx >= n || start_idx > end_idx) {
          rt.last_error = "Export delta-save: invalid snapshot range.";
          return;
        }
        try {
          nebula4x::DeltaSaveFile f;
          f.patch_kind = is_json_patch_delta_mode(rt.stored_storage_mode) ? nebula4x::DeltaSavePatchKind::JsonPatch
                                                                        : nebula4x::DeltaSavePatchKind::MergePatch;
          f.format = (f.patch_kind == nebula4x::DeltaSavePatchKind::JsonPatch) ? nebula4x::kDeltaSaveFormatV2
                                                                              : nebula4x::kDeltaSaveFormatV1;
          f.base = nebula4x::json::parse(snapshot_json(rt, start_idx));

          f.patches.clear();
          f.patches.reserve(static_cast<std::size_t>(end_idx - start_idx));

          for (int i = start_idx + 1; i <= end_idx; ++i) {
            nebula4x::DeltaSavePatch p;
            const Snapshot& step = rt.snapshots[static_cast<std::size_t>(i)];
            const bool reuse_ok = is_delta_storage_mode(rt.stored_storage_mode) && step.has_delta_patch &&
                                  ((f.patch_kind == nebula4x::DeltaSavePatchKind::MergePatch &&
                                    is_merge_patch_delta_mode(rt.stored_storage_mode)) ||
                                   (f.patch_kind == nebula4x::DeltaSavePatchKind::JsonPatch &&
                                    is_json_patch_delta_mode(rt.stored_storage_mode)));

            if (reuse_ok) {
              // Stored patch is from (i-1)->i; valid for any contiguous range.
              p.patch = step.delta_patch;
            } else {
              // Compute patch (full-mode history or missing patch).
              const std::string from_txt = snapshot_json(rt, i - 1);
              const std::string to_txt = snapshot_json(rt, i);

              if (f.patch_kind == nebula4x::DeltaSavePatchKind::JsonPatch) {
                nebula4x::JsonPatchOptions jopt;
                jopt.max_ops = 0;
                jopt.indent = 0;
                const std::string patch_json = nebula4x::diff_saves_to_json_patch(from_txt, to_txt, jopt);
                p.patch = nebula4x::json::parse(patch_json);
              } else {
                const nebula4x::json::Value from = nebula4x::json::parse(from_txt);
                const nebula4x::json::Value to = nebula4x::json::parse(to_txt);
                p.patch = nebula4x::diff_json_merge_patch(from, to);
              }
            }
            f.patches.push_back(std::move(p));
          }

          if (rt.export_delta_include_digests) {
            // This can be slow for large histories; compute and attach digests for verification.
            nebula4x::compute_delta_save_digests(f);
          }

          const std::string delta_txt = nebula4x::stringify_delta_save_file(f, 2);
          nebula4x::write_text_file(rt.export_delta_save_path, delta_txt);
          rt.last_error.clear();
        } catch (const std::exception& e) {
          rt.last_error = std::string("Export delta-save failed: ") + e.what();
        }
      };

      if (ImGui::Button("Export: all (0..latest)##tm_delta")) {
        export_delta_range(0, n - 1);
      }
      ImGui::SameLine();
      if (ImGui::Button("Export: baseline..selected##tm_delta")) {
        export_delta_range(std::clamp(rt.baseline_idx, 0, n - 1), std::clamp(rt.selected_idx, 0, n - 1));
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
      const DiffChange& c = (*changes)[static_cast<std::size_t>(i)];
      if (!op_visible(c, rt)) continue;
      if (!contains_text(c.path, path_need, rt.filter_case_sensitive)) continue;
      if (!val_need.empty()) {
        if (!contains_text(c.before, val_need, rt.filter_case_sensitive) && !contains_text(c.after, val_need, rt.filter_case_sensitive)) continue;
      }
      visible.push_back(i);
    }

    ImGui::Separator();
    ImGui::TextDisabled("Changes: %d%s", static_cast<int>(visible.size()), truncated ? "+" : "");

    ImGui::BeginChild("##tm_changes", ImVec2(0, 0), true);
    {
      ImGuiListClipper clip;
      clip.Begin(static_cast<int>(visible.size()));
      while (clip.Step()) {
        for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row) {
          const int idx = visible[static_cast<std::size_t>(row)];
          const DiffChange& ch = (*changes)[static_cast<std::size_t>(idx)];

          // Row layout: op | path | before | after
          ImGui::PushID(row);

          ImGui::TextDisabled("%s", ch.op.c_str());
          ImGui::SameLine(90.0f);

          // Path as a clickable link to open JSON explorer.
          if (ImGui::SmallButton(ch.path.c_str())) {
            ui.show_json_explorer_window = true;
            ui.request_json_explorer_goto_path = ch.path;
          }
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Jump to this path in the JSON Explorer (live state).\nPath: %s", ch.path.c_str());
          }

          ImGui::SameLine(420.0f);
          ImGui::TextWrapped("%s", ch.before.c_str());
          ImGui::SameLine(700.0f);
          ImGui::TextWrapped("%s", ch.after.c_str());

          ImGui::PopID();
          ImGui::Separator();
        }
      }
    }
    ImGui::EndChild();
  }
  ImGui::EndChild();

  ImGui::End();
}

} // namespace nebula4x::ui
