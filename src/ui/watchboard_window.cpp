#include "ui/watchboard_window.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "imgui.h"
#include "nebula4x/util/json.h"
#include "nebula4x/util/json_pointer.h"
#include "nebula4x/util/json_pointer_autocomplete.h"
#include "ui/dashboards_window.h"
#include "ui/data_lenses_window.h"
#include "ui/game_entity_index.h"
#include "ui/game_json_cache.h"
#include "ui/pivot_tables_window.h"

namespace nebula4x::ui {

namespace {

constexpr int kMinHistLen = 2;
constexpr int kMaxHistLen = 4000;

constexpr int kMinQueryMaxMatches = 10;
constexpr int kMaxQueryMaxMatches = 500000;
constexpr int kMinQueryMaxNodes = 100;
constexpr int kMaxQueryMaxNodes = 5000000;

constexpr int kMaxSampleMatches = 8;
constexpr int kMaxPreviewChars = 120;

const char* query_op_name(const int op) {
  switch (op) {
    case 0:
      return "Count";
    case 1:
      return "Sum";
    case 2:
      return "Avg";
    case 3:
      return "Min";
    case 4:
      return "Max";
    default:
      return "Count";
  }
}

const char* query_op_func(const int op) {
  switch (op) {
    case 0:
      return "count";
    case 1:
      return "sum";
    case 2:
      return "avg";
    case 3:
      return "min";
    case 4:
      return "max";
    default:
      return "count";
  }
}

std::string trim_preview(std::string s, const int max_chars = kMaxPreviewChars) {
  if ((int)s.size() <= max_chars) return s;
  s.resize(std::max(0, max_chars - 3));
  s += "...";
  return s;
}

std::string format_number(const double x) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.6g", x);
  return std::string(buf);
}

// Coerce common JSON types into a numeric value for aggregation.
//
// - number: the number
// - bool: true=1, false=0
// - array: size
// - object: size
// - null/string: not numeric
bool coerce_numeric(const nebula4x::json::Value& v, double& out) {
  if (v.is_number()) {
    out = v.number_value();
    return true;
  }
  if (v.is_bool()) {
    out = v.bool_value() ? 1.0 : 0.0;
    return true;
  }
  if (v.is_array()) {
    const auto* a = v.as_array();
    out = a ? static_cast<double>(a->size()) : 0.0;
    return true;
  }
  if (v.is_object()) {
    const auto* o = v.as_object();
    out = o ? static_cast<double>(o->size()) : 0.0;
    return true;
  }
  return false;
}

struct EvalResult {
  bool ok{false};
  bool numeric{false};
  float value{0.0f};
  std::string display;
  std::string error;

  // Query-only diagnostics.
  bool is_query{false};
  int query_op{0};
  int match_count{0};
  int numeric_count{0};
  int nodes_visited{0};
  bool hit_match_limit{false};
  bool hit_node_limit{false};
  std::vector<std::string> sample_paths;
  std::vector<std::string> sample_previews;
};

EvalResult eval_value(const nebula4x::json::Value& v) {
  EvalResult r;
  r.ok = true;

  if (v.is_null()) {
    r.display = "null";
    return r;
  }

  if (v.is_bool()) {
    const bool b = v.bool_value(false);
    r.numeric = true;
    r.value = b ? 1.0f : 0.0f;
    r.display = b ? "true" : "false";
    return r;
  }

  if (v.is_number()) {
    const double d = v.number_value(0.0);
    r.numeric = true;
    r.value = static_cast<float>(d);
    r.display = format_number(d);
    return r;
  }

  if (v.is_string()) {
    std::string s = v.string_value();
    s = trim_preview(s, kMaxPreviewChars);
    r.display = '"' + s + '"';
    return r;
  }

  if (v.is_array()) {
    const auto* a = v.as_array();
    const int n = a ? static_cast<int>(a->size()) : 0;
    r.numeric = true;
    r.value = static_cast<float>(n);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "[%d items]", n);
    r.display = buf;
    return r;
  }

  if (v.is_object()) {
    const auto* o = v.as_object();
    const int n = o ? static_cast<int>(o->size()) : 0;
    r.numeric = true;
    r.value = static_cast<float>(n);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "{%d keys}", n);
    r.display = buf;
    return r;
  }

  r.display = "(unknown)";
  return r;
}

EvalResult eval_pointer(const nebula4x::json::Value& root, const std::string& path) {
  EvalResult r;
  std::string err;
  const auto* node = nebula4x::resolve_json_pointer(root, path, /*accept_root_slash=*/true, &err);
  if (!node) {
    r.ok = false;
    r.display = "(missing)";
    r.error = err;
    return r;
  }
  return eval_value(*node);
}

EvalResult eval_query(const nebula4x::json::Value& root, const JsonWatchConfig& cfg, const UIState& ui) {
  EvalResult r;
  r.is_query = true;
  r.query_op = std::clamp(cfg.query_op, 0, 4);

  nebula4x::JsonPointerQueryStats stats;
  std::string err;

  const int max_matches = std::clamp(ui.watchboard_query_max_matches, kMinQueryMaxMatches, kMaxQueryMaxMatches);
  const int max_nodes = std::clamp(ui.watchboard_query_max_nodes, kMinQueryMaxNodes, kMaxQueryMaxNodes);

  const auto matches = nebula4x::query_json_pointer_glob(root, cfg.path, /*accept_root_slash=*/true, max_matches,
                                                        max_nodes, &stats, &err);

  r.match_count = stats.matches;
  r.nodes_visited = stats.nodes_visited;
  r.hit_match_limit = stats.hit_match_limit;
  r.hit_node_limit = stats.hit_node_limit;

  if (!err.empty()) {
    r.ok = false;
    r.display = "(error)";
    r.error = err;
    return r;
  }

  r.ok = true;

  // Sample list for tooltips / navigation.
  r.sample_paths.reserve(std::min<int>(kMaxSampleMatches, (int)matches.size()));
  r.sample_previews.reserve(std::min<int>(kMaxSampleMatches, (int)matches.size()));

  int num_count = 0;
  double sum = 0.0;
  double min_v = std::numeric_limits<double>::infinity();
  double max_v = -std::numeric_limits<double>::infinity();

  for (int i = 0; i < (int)matches.size(); ++i) {
    const auto& m = matches[i];
    if (m.value) {
      double x = 0.0;
      if (coerce_numeric(*m.value, x)) {
        num_count++;
        sum += x;
        min_v = std::min(min_v, x);
        max_v = std::max(max_v, x);
      }

      if ((int)r.sample_paths.size() < kMaxSampleMatches) {
        r.sample_paths.push_back(m.path);
        r.sample_previews.push_back(trim_preview(eval_value(*m.value).display, kMaxPreviewChars));
      }
    }
  }

  r.numeric_count = num_count;

  // Aggregate.
  switch (r.query_op) {
    case 0: {  // count
      r.numeric = true;
      r.value = static_cast<float>(r.match_count);
      r.display = std::to_string(r.match_count);
      if (r.hit_match_limit || r.hit_node_limit) r.display += "+";
      return r;
    }
    case 1: {  // sum
      r.numeric = true;
      r.value = static_cast<float>(sum);
      r.display = format_number(sum);
      return r;
    }
    case 2: {  // avg
      if (num_count <= 0) {
        r.ok = false;
        r.display = "(no numeric)";
        return r;
      }
      const double avg = sum / static_cast<double>(num_count);
      r.numeric = true;
      r.value = static_cast<float>(avg);
      r.display = format_number(avg);
      return r;
    }
    case 3: {  // min
      if (num_count <= 0) {
        r.ok = false;
        r.display = "(no numeric)";
        return r;
      }
      r.numeric = true;
      r.value = static_cast<float>(min_v);
      r.display = format_number(min_v);
      return r;
    }
    case 4: {  // max
      if (num_count <= 0) {
        r.ok = false;
        r.display = "(no numeric)";
        return r;
      }
      r.numeric = true;
      r.value = static_cast<float>(max_v);
      r.display = format_number(max_v);
      return r;
    }
    default:
      break;
  }

  r.numeric = true;
  r.value = static_cast<float>(r.match_count);
  r.display = std::to_string(r.match_count);
  return r;
}

EvalResult eval_watch(const nebula4x::json::Value& root, const JsonWatchConfig& cfg, const UIState& ui) {
  if (cfg.is_query) return eval_query(root, cfg, ui);
  return eval_pointer(root, cfg.path);
}

struct WatchRuntime {
  // History.
  std::int64_t last_sample_tick{-1};
  float last_value{0.0f};
  bool has_last_value{false};
  std::vector<float> history;

  // Cached evaluation (expensive queries should not run every frame).
  std::uint64_t last_eval_revision{0};
  bool has_cached_eval{false};
  EvalResult cached_eval;

  // Detect config changes (path/mode/op) to reset history + cache.
  std::string last_path;
  bool last_is_query{false};
  int last_query_op{0};

  // Path editing buffer for the config popup.
  bool edit_path_init{false};
  char edit_path[256]{};
};

struct WatchboardState {
  bool initialized{false};

  // Cached doc.
  std::string doc_text;
  std::shared_ptr<nebula4x::json::Value> root;
  std::string doc_error;
  bool doc_loaded{false};
  std::uint64_t doc_revision{0};

  // Refresh controls.
  bool auto_refresh{true};
  float refresh_sec{0.35f};
  double last_refresh_time{0.0};

  // Add-pin UI.
  char add_path[256]{};
  char add_label[128]{};
  bool add_track_history{true};
  bool add_show_sparkline{true};
  int add_history_len{120};
  bool add_is_query{false};
  int add_query_op{0};

  // Runtime per pin.
  std::unordered_map<std::uint64_t, WatchRuntime> rt;
};

void refresh_doc(WatchboardState& st, Simulation& sim, const bool force) {
  std::string err;
  if (!ensure_game_json_cache(sim, /*refresh_interval=*/0.0f, force, &err)) {
    st.doc_error = err;
    st.doc_loaded = false;
    return;
  }

  const auto* cache = get_game_json_cache();
  if (!cache) {
    st.doc_error = "Game JSON cache unavailable";
    st.doc_loaded = false;
    return;
  }

  if (!force && cache->revision == st.doc_revision) {
    // nothing changed
    st.doc_loaded = true;
    return;
  }

  st.doc_revision = cache->revision;
  st.doc_text = cache->text;
  st.doc_error = cache->error;
  st.doc_loaded = cache->ok;
  st.root = cache->root;
}

std::string default_label_from_path(const std::string& path) {
  std::vector<std::string> tokens;
  try {
    tokens = nebula4x::split_json_pointer(path, /*accept_root_slash=*/true);
  } catch (...) {
    return "pin";
  }
  if (tokens.empty()) return "root";
  const std::string& last = tokens.back();
  return last.empty() ? "pin" : last;
}

} // namespace

bool add_watch_item(UIState& ui, const std::string& path, const std::string& label, const bool track_history,
                    const bool show_sparkline, const int history_len) {
  if (path.empty()) return false;

  for (const auto& w : ui.json_watch_items) {
    if (!w.is_query && w.path == path) return false;
  }

  JsonWatchConfig cfg;
  cfg.id = ui.next_json_watch_id++;
  cfg.path = path;
  cfg.label = label.empty() ? default_label_from_path(path) : label;
  cfg.track_history = track_history;
  cfg.show_sparkline = show_sparkline;
  cfg.history_len = std::clamp(history_len, kMinHistLen, kMaxHistLen);
  cfg.is_query = false;
  cfg.query_op = 0;

  ui.json_watch_items.push_back(std::move(cfg));
  return true;
}

bool add_watch_query_item(UIState& ui, const std::string& pattern, const int query_op, const std::string& label,
                          const bool track_history, const bool show_sparkline, const int history_len) {
  if (pattern.empty()) return false;

  const int op = std::clamp(query_op, 0, 4);

  for (const auto& w : ui.json_watch_items) {
    if (w.is_query && w.path == pattern && w.query_op == op) return false;
  }

  JsonWatchConfig cfg;
  cfg.id = ui.next_json_watch_id++;
  cfg.path = pattern;

  if (!label.empty()) {
    cfg.label = label;
  } else {
    const std::string base = default_label_from_path(pattern);
    cfg.label = std::string(query_op_func(op)) + "(" + base + ")";
  }

  cfg.track_history = track_history;
  cfg.show_sparkline = show_sparkline;
  cfg.history_len = std::clamp(history_len, kMinHistLen, kMaxHistLen);
  cfg.is_query = true;
  cfg.query_op = op;

  ui.json_watch_items.push_back(std::move(cfg));
  return true;
}

void draw_watchboard_window(Simulation& sim, UIState& ui) {
  static WatchboardState st;

  if (!st.initialized) {
    st.initialized = true;
    st.auto_refresh = true;
    st.refresh_sec = 0.35f;
    st.last_refresh_time = ImGui::GetTime();

    std::snprintf(st.add_path, sizeof(st.add_path), "%s", "/");
    st.add_history_len = 120;

    // Conservative query limits.
    ui.watchboard_query_max_matches = std::clamp(ui.watchboard_query_max_matches, kMinQueryMaxMatches, kMaxQueryMaxMatches);
    ui.watchboard_query_max_nodes = std::clamp(ui.watchboard_query_max_nodes, kMinQueryMaxNodes, kMaxQueryMaxNodes);

    refresh_doc(st, sim, /*force=*/true);
  }

  ImGui::SetNextWindowSize(ImVec2(980, 560), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Watchboard (JSON Pins)", &ui.show_watchboard_window)) {
    ImGui::End();
    return;
  }

  ImGui::TextDisabled("Procedural pins rendered from the current game state's JSON.");
  ImGui::SameLine();
  ImGui::TextDisabled("(Tip: right-click nodes in JSON Explorer to pin.)");

  // --- refresh controls ---
  {
    if (ImGui::Button("Refresh")) {
      refresh_doc(st, sim, /*force=*/true);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto refresh", &st.auto_refresh);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    ImGui::SliderFloat("Interval (sec)", &st.refresh_sec, 0.10f, 5.0f, "%.2f");
    st.refresh_sec = std::clamp(st.refresh_sec, 0.05f, 60.0f);

    if (st.auto_refresh) {
      const double now = ImGui::GetTime();
      if (now - st.last_refresh_time >= st.refresh_sec) {
        st.last_refresh_time = now;
        refresh_doc(st, sim, /*force=*/false);
      }
    }

    if (!st.doc_error.empty()) {
      ImGui::Separator();
      ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "JSON error: %s", st.doc_error.c_str());
    }
  }

  ImGui::Separator();

  // --- add pin ---
  if (ImGui::CollapsingHeader("Add pin", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::InputTextWithHint("Label", "(optional)", st.add_label, IM_ARRAYSIZE(st.add_label));

    const char* path_hint = st.add_is_query ? "/colonies/*/population" : "/systems/0/name";
    ImGui::InputTextWithHint("Path / Pattern", path_hint, st.add_path, IM_ARRAYSIZE(st.add_path));

    if (st.doc_loaded && st.root && !st.add_is_query) {
      // Autocomplete only for strict pointers (query patterns are free-form).
      draw_autocomplete_list("##watch_add_autocomplete", st.add_path, IM_ARRAYSIZE(st.add_path), *st.root);
    }

    ImGui::Checkbox("Track history", &st.add_track_history);
    ImGui::SameLine();
    ImGui::Checkbox("Sparkline", &st.add_show_sparkline);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt("History len", &st.add_history_len, 10, 100);
    st.add_history_len = std::clamp(st.add_history_len, kMinHistLen, kMaxHistLen);

    ImGui::Separator();

    ImGui::Checkbox("Aggregate query (wildcards)", &st.add_is_query);
    if (st.add_is_query) {
      ImGui::SameLine();
      ImGui::SetNextItemWidth(140.0f);
      const char* ops = "Count matches\0Sum\0Average\0Min\0Max\0";
      ImGui::Combo("Op", &st.add_query_op, ops);
      st.add_query_op = std::clamp(st.add_query_op, 0, 4);

      if (ImGui::TreeNode("Query settings")) {
        ImGui::TextDisabled("Wildcards: * (one segment), ** (recursive)");
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputInt("Max matches", &ui.watchboard_query_max_matches, 100, 1000);
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputInt("Max nodes", &ui.watchboard_query_max_nodes, 1000, 50000);
        ui.watchboard_query_max_matches =
            std::clamp(ui.watchboard_query_max_matches, kMinQueryMaxMatches, kMaxQueryMaxMatches);
        ui.watchboard_query_max_nodes = std::clamp(ui.watchboard_query_max_nodes, kMinQueryMaxNodes, kMaxQueryMaxNodes);
        ImGui::TextDisabled("Example: /colonies/*/population (sum total population)");
        ImGui::TreePop();
      }
    }

    if (ImGui::Button("Add")) {
      const std::string label = st.add_label;
      const std::string path = st.add_path;
      bool ok = false;
      if (st.add_is_query) {
        ok = add_watch_query_item(ui, path, st.add_query_op, label, st.add_track_history, st.add_show_sparkline,
                                  st.add_history_len);
      } else {
        ok = add_watch_item(ui, path, label, st.add_track_history, st.add_show_sparkline, st.add_history_len);
      }
      if (ok) {
        st.add_label[0] = '\0';
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Paste path")) {
      const char* clip = ImGui::GetClipboardText();
      if (clip && clip[0] != '\0') {
#if defined(_MSC_VER)
        strncpy_s(st.add_path, IM_ARRAYSIZE(st.add_path), clip, _TRUNCATE);
#else
        std::strncpy(st.add_path, clip, IM_ARRAYSIZE(st.add_path));
        st.add_path[IM_ARRAYSIZE(st.add_path) - 1] = '\0';
#endif
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy help")) {
      ImGui::SetClipboardText(
          "Watchboard quick ref:\n"
          "  JSON Pointer: /a/b/0  (object keys + array indices)\n"
          "  Escape: ~ -> ~0, / -> ~1\n"
          "  Query wildcards: * (one segment), ** (recursive)\n"
          "Examples:\n"
          "  /systems\n"
          "  /ships/123/name\n"
          "  /colonies/*/population\n"
          "  /**/name\n");
    }
  }

  ImGui::Separator();

  if (!st.doc_loaded) {
    ImGui::TextDisabled("(No JSON loaded)");
    ImGui::End();
    return;
  }

  // Build set of ids so we can prune runtime for removed pins.
  {
    std::vector<std::uint64_t> ids;
    ids.reserve(ui.json_watch_items.size());
    for (const auto& w : ui.json_watch_items) ids.push_back(w.id);

    for (auto it = st.rt.begin(); it != st.rt.end();) {
      if (std::find(ids.begin(), ids.end(), it->first) == ids.end()) {
        it = st.rt.erase(it);
      } else {
        ++it;
      }
    }
  }

  const std::int64_t tick = sim_tick_hours(sim.state());

  ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter |
                       ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                       ImGuiTableFlags_Reorderable;

  const float table_h = std::max(160.0f, ImGui::GetContentRegionAvail().y);

  if (ImGui::BeginTable("##watch_table", 5, tf, ImVec2(0.0f, table_h))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 180.0f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 160.0f);
    ImGui::TableSetupColumn("Plot", ImGuiTableColumnFlags_WidthFixed, 200.0f);
    ImGui::TableSetupColumn("Path / Pattern", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 160.0f);
    ImGui::TableHeadersRow();

    int remove_idx = -1;

    for (int i = 0; i < (int)ui.json_watch_items.size(); ++i) {
      JsonWatchConfig& cfg = ui.json_watch_items[i];
      WatchRuntime& rt = st.rt[cfg.id];

      // Detect config changes affecting the evaluated signal.
      if (rt.last_path != cfg.path || rt.last_is_query != cfg.is_query || rt.last_query_op != cfg.query_op) {
        rt.last_path = cfg.path;
        rt.last_is_query = cfg.is_query;
        rt.last_query_op = cfg.query_op;

        rt.history.clear();
        rt.has_last_value = false;
        rt.last_sample_tick = -1;

        rt.has_cached_eval = false;
        rt.last_eval_revision = 0;

        rt.edit_path_init = false;
      }

      // Evaluate (cached per doc revision).
      if (st.root) {
        if (!rt.has_cached_eval || rt.last_eval_revision != st.doc_revision) {
          rt.cached_eval = eval_watch(*st.root, cfg, ui);
          rt.last_eval_revision = st.doc_revision;
          rt.has_cached_eval = true;
        }
      } else {
        rt.cached_eval = EvalResult{};
        rt.has_cached_eval = true;
        rt.last_eval_revision = st.doc_revision;
      }

      const EvalResult& ev = rt.cached_eval;

      // History sampling (once per sim tick).
      float delta = 0.0f;
      bool has_delta = false;

      if (cfg.track_history && ev.ok && ev.numeric) {
        if (rt.last_sample_tick != tick) {
          rt.last_sample_tick = tick;

          if (rt.has_last_value) {
            delta = ev.value - rt.last_value;
            has_delta = true;
          }

          rt.last_value = ev.value;
          rt.has_last_value = true;

          rt.history.push_back(ev.value);
          const int keep = std::clamp(cfg.history_len, kMinHistLen, kMaxHistLen);
          if ((int)rt.history.size() > keep) {
            const int extra = (int)rt.history.size() - keep;
            rt.history.erase(rt.history.begin(), rt.history.begin() + extra);
          }
        } else if (rt.has_last_value) {
          delta = ev.value - rt.last_value;
          has_delta = true;
        }
      }

      // Choose a "representative" strict pointer for navigation actions.
      std::string rep_ptr = cfg.path;
      if (cfg.is_query) {
        if (!ev.sample_paths.empty()) {
          rep_ptr = ev.sample_paths[0];
        } else {
          rep_ptr = "/";
        }
      }

      ImGui::PushID((int)cfg.id);
      ImGui::TableNextRow();

      // Label
      ImGui::TableSetColumnIndex(0);
      {
        char buf[128];
#if defined(_MSC_VER)
        strncpy_s(buf, sizeof(buf), cfg.label.c_str(), _TRUNCATE);
#else
        std::strncpy(buf, cfg.label.c_str(), sizeof(buf));
        buf[sizeof(buf) - 1] = '\0';
#endif
        ImGuiInputTextFlags lf = ImGuiInputTextFlags_AutoSelectAll;
        if (ImGui::InputText("##label", buf, IM_ARRAYSIZE(buf), lf)) {
          cfg.label = buf;
        }
        if (cfg.is_query) {
          ImGui::SameLine();
          ImGui::TextDisabled("[%s]", query_op_name(std::clamp(cfg.query_op, 0, 4)));
        }
      }

      // Value
      ImGui::TableSetColumnIndex(1);
      {
        if (!ev.ok) {
          ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", ev.display.c_str());
          if (ImGui::IsItemHovered() && !ev.error.empty()) {
            ImGui::SetTooltip("%s", ev.error.c_str());
          }
        } else {
          ImGui::TextUnformatted(ev.display.c_str());
        }

        if (has_delta && std::fabs(delta) > 0.00001f) {
          ImGui::SameLine();
          char dbuf[64];
          std::snprintf(dbuf, sizeof(dbuf), "(%+.3g)", (double)delta);
          ImGui::TextDisabled("%s", dbuf);
        }

        if (ImGui::IsItemHovered() && ev.is_query) {
          ImGui::BeginTooltip();
          ImGui::TextDisabled("Query pin: %s(...)", query_op_func(ev.query_op));
          ImGui::Separator();
          ImGui::Text("Pattern: %s", cfg.path.c_str());
          ImGui::Text("Matches: %d%s", ev.match_count,
                      (ev.hit_match_limit || ev.hit_node_limit) ? " (clipped)" : "");
          ImGui::Text("Numeric: %d", ev.numeric_count);
          ImGui::Text("Nodes visited: %d", ev.nodes_visited);
          if (ev.hit_match_limit) ImGui::TextDisabled("Hit match cap (%d)", ui.watchboard_query_max_matches);
          if (ev.hit_node_limit) ImGui::TextDisabled("Hit node cap (%d)", ui.watchboard_query_max_nodes);
          if (!ev.sample_paths.empty()) {
            ImGui::Separator();
            ImGui::TextDisabled("Sample matches:");
            for (int s = 0; s < (int)ev.sample_paths.size(); ++s) {
              ImGui::BulletText("%s = %s", ev.sample_paths[s].c_str(),
                               s < (int)ev.sample_previews.size() ? ev.sample_previews[s].c_str() : "");
            }
          }
          ImGui::EndTooltip();
        }
      }

      // Plot
      ImGui::TableSetColumnIndex(2);
      {
        if (cfg.track_history && cfg.show_sparkline && rt.history.size() >= 2) {
          ImGui::PlotLines("##plot", rt.history.data(), (int)rt.history.size(), 0, nullptr, FLT_MAX, FLT_MAX,
                           ImVec2(190.0f, 34.0f));
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("samples: %d\nlast: %.6g", (int)rt.history.size(),
                              rt.has_last_value ? (double)rt.last_value : 0.0);
          }
        } else {
          ImGui::TextDisabled(cfg.track_history ? "(no data)" : "(history off)");
        }
      }

      // Path / Pattern
      ImGui::TableSetColumnIndex(3);
      {
        ImGui::TextUnformatted(cfg.path.c_str());
        if (cfg.is_query) {
          ImGui::SameLine();
          ImGui::TextDisabled("(%s)", query_op_name(std::clamp(cfg.query_op, 0, 4)));
        }

        if (ImGui::IsItemHovered()) {
          if (cfg.is_query) {
            ImGui::SetTooltip("JSON Pointer pattern (wildcards: * and **)\n%s", cfg.path.c_str());
          } else {
            ImGui::SetTooltip("JSON Pointer\n%s", cfg.path.c_str());
          }
        }

        if (ImGui::BeginPopupContextItem("##path_ctx")) {
          if (ImGui::MenuItem("Copy path/pattern")) {
            ImGui::SetClipboardText(cfg.path.c_str());
          }

          if (cfg.is_query && !ev.sample_paths.empty()) {
            if (ImGui::MenuItem("Copy first match pointer")) {
              ImGui::SetClipboardText(ev.sample_paths[0].c_str());
            }
          }

          if (!cfg.is_query) {
            if (ImGui::MenuItem("Go to in JSON Explorer")) {
              ui.show_json_explorer_window = true;
              ui.request_json_explorer_goto_path = cfg.path;
            }
          } else {
            if (ImGui::MenuItem("Go to first match in JSON Explorer", nullptr, false, !ev.sample_paths.empty())) {
              ui.show_json_explorer_window = true;
              ui.request_json_explorer_goto_path = rep_ptr;
            }
          }

          // Resolve representative pointer for entity-based actions.
          if (st.doc_loaded && st.root) {
            std::string derr;
            const auto* node = nebula4x::resolve_json_pointer(*st.root, rep_ptr, /*accept_root_slash=*/true, &derr);
            if (node) {
              std::uint64_t ent_id = 0;
              if (json_to_u64_id(*node, ent_id)) {
                if (const auto* ent = find_game_entity(ent_id)) {
                  ImGui::Separator();
                  std::string elabel = ent->kind + " #" + std::to_string(ent->id);
                  if (!ent->name.empty()) elabel += "  " + ent->name;
                  ImGui::TextDisabled("Referenced entity");
                  ImGui::TextUnformatted(elabel.c_str());
                  if (ImGui::MenuItem("Go to referenced entity")) {
                    ui.show_json_explorer_window = true;
                    ui.request_json_explorer_goto_path = ent->path;
                  }
                  if (ImGui::MenuItem("Open in Entity Inspector")) {
                    ui.show_entity_inspector_window = true;
                    ui.entity_inspector_id = ent->id;
                  }
                  if (ImGui::MenuItem("Open in Reference Graph")) {
                    ui.show_reference_graph_window = true;
                    ui.reference_graph_focus_id = ent->id;
                  }
                  if (ImGui::MenuItem("Copy referenced entity path")) {
                    ImGui::SetClipboardText(ent->path.c_str());
                  }
                }
              }

              if (node->is_array()) {
                ImGui::Separator();
                if (ImGui::MenuItem("Create Data Lens from this array")) {
                  ui.show_data_lenses_window = true;
                  (void)add_json_table_view(ui, rep_ptr, cfg.label.empty() ? "" : cfg.label);
                }
                if (ImGui::MenuItem("Create Dashboard (Procedural Charts)")) {
                  ui.show_dashboards_window = true;
                  const std::string nm = (cfg.label.empty() ? "" : cfg.label + " Dashboard");
                  (void)add_json_dashboard_for_path(ui, rep_ptr, nm);
                }
                if (ImGui::MenuItem("Create Pivot Table (Procedural Aggregations)")) {
                  ui.show_pivot_tables_window = true;
                  const std::string nm = (cfg.label.empty() ? "" : cfg.label + " Pivot");
                  (void)add_json_pivot_for_path(ui, rep_ptr, nm);
                }
              }
            }
          }

          ImGui::EndPopup();
        }
      }

      // Actions
      ImGui::TableSetColumnIndex(4);
      {
        const bool can_go = !rep_ptr.empty();
        if (ImGui::SmallButton("Go") && can_go) {
          ui.show_json_explorer_window = true;
          ui.request_json_explorer_goto_path = rep_ptr;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Config")) {
          ImGui::OpenPopup("##cfg_popup");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
          remove_idx = i;
        }

        if (ImGui::BeginPopup("##cfg_popup")) {
          // Path / pattern editing
          if (!rt.edit_path_init) {
#if defined(_MSC_VER)
            strncpy_s(rt.edit_path, sizeof(rt.edit_path), cfg.path.c_str(), _TRUNCATE);
#else
            std::strncpy(rt.edit_path, cfg.path.c_str(), sizeof(rt.edit_path));
            rt.edit_path[sizeof(rt.edit_path) - 1] = '\0';
#endif
            rt.edit_path_init = true;
          }

          ImGui::TextDisabled("Path / Pattern");
          ImGuiInputTextFlags pf = ImGuiInputTextFlags_EnterReturnsTrue;
          bool committed = ImGui::InputTextWithHint("##edit_path", "/colonies/*/population", rt.edit_path,
                                                    IM_ARRAYSIZE(rt.edit_path), pf);
          const bool deactivated = ImGui::IsItemDeactivatedAfterEdit();
          if ((committed || deactivated) && std::strlen(rt.edit_path) > 0) {
            cfg.path = rt.edit_path;
            // Reset happens automatically via config change detection.
          }

          if (st.doc_loaded && st.root) {
            draw_autocomplete_list("##watch_cfg_autocomplete", rt.edit_path, IM_ARRAYSIZE(rt.edit_path), *st.root);
          }

          ImGui::Separator();

          // Query mode
          ImGui::Checkbox("Aggregate query", &cfg.is_query);
          if (cfg.is_query) {
            const char* ops = "Count matches\0Sum\0Average\0Min\0Max\0";
            ImGui::Combo("Op", &cfg.query_op, ops);
            cfg.query_op = std::clamp(cfg.query_op, 0, 4);

            if (ImGui::TreeNode("Query budgets")) {
              ImGui::SetNextItemWidth(160.0f);
              ImGui::InputInt("Max matches##cfg", &ui.watchboard_query_max_matches, 100, 1000);
              ImGui::SetNextItemWidth(160.0f);
              ImGui::InputInt("Max nodes##cfg", &ui.watchboard_query_max_nodes, 1000, 50000);
              ui.watchboard_query_max_matches =
                  std::clamp(ui.watchboard_query_max_matches, kMinQueryMaxMatches, kMaxQueryMaxMatches);
              ui.watchboard_query_max_nodes = std::clamp(ui.watchboard_query_max_nodes, kMinQueryMaxNodes,
                                                         kMaxQueryMaxNodes);
              ImGui::TextDisabled("Wildcards: * (one segment), ** (recursive)");
              ImGui::TreePop();
            }

            ImGui::Separator();
            ImGui::TextDisabled("Last eval:");
            ImGui::BulletText("Matches: %d%s", ev.match_count,
                              (ev.hit_match_limit || ev.hit_node_limit) ? " (clipped)" : "");
            ImGui::BulletText("Numeric: %d", ev.numeric_count);
            ImGui::BulletText("Nodes visited: %d", ev.nodes_visited);
            if (!ev.error.empty()) {
              ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", ev.error.c_str());
            }
          }

          ImGui::Separator();

          // History controls
          ImGui::Checkbox("Track history", &cfg.track_history);
          ImGui::Checkbox("Sparkline", &cfg.show_sparkline);
          ImGui::InputInt("History len", &cfg.history_len, 10, 100);
          cfg.history_len = std::clamp(cfg.history_len, kMinHistLen, kMaxHistLen);

          if (ImGui::Button("Clear history")) {
            rt.history.clear();
            rt.has_last_value = false;
            rt.last_sample_tick = -1;
          }

          ImGui::Separator();
          if (ImGui::Button("Copy path/pattern")) {
            ImGui::SetClipboardText(cfg.path.c_str());
          }
          if (ImGui::Button("Copy label")) {
            ImGui::SetClipboardText(cfg.label.c_str());
          }

          ImGui::EndPopup();
        }
      }

      ImGui::PopID();
    }

    if (remove_idx >= 0 && remove_idx < (int)ui.json_watch_items.size()) {
      const std::uint64_t id = ui.json_watch_items[remove_idx].id;
      ui.json_watch_items.erase(ui.json_watch_items.begin() + remove_idx);
      st.rt.erase(id);
    }

    ImGui::EndTable();
  }

  ImGui::End();
}

} // namespace nebula4x::ui
