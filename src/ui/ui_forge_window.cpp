#include "ui/ui_forge_window.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "nebula4x/util/json_pointer.h"
#include "nebula4x/util/json_pointer_autocomplete.h"
#include "nebula4x/util/log.h"

#include "ui/dashboards_window.h"
#include "ui/data_lenses_window.h"
#include "ui/game_entity_index.h"
#include "ui/game_json_cache.h"
#include "ui/pivot_tables_window.h"

namespace nebula4x::ui {
namespace {

constexpr int kMaxPreviewChars = 180;
constexpr int kMaxSampleMatches = 20;

constexpr int kMinHistLen = 2;
constexpr int kMaxHistLen = 4000;

constexpr int kMinQueryMaxMatches = 10;
constexpr int kMaxQueryMaxMatches = 500000;
constexpr int kMinQueryMaxNodes = 100;
constexpr int kMaxQueryMaxNodes = 5000000;

std::string query_op_label(int op) {
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

// Draw a vertical separator using only public ImGui API.
void VerticalSeparator(float height = 0.0f) {
  ImGui::SameLine();
  const ImGuiStyle& style = ImGui::GetStyle();
  const float h = (height > 0.0f) ? height : ImGui::GetFrameHeight();

  // Reserve a small rect.
  ImGui::Dummy(ImVec2(style.ItemSpacing.x, h));

  const ImVec2 a = ImGui::GetItemRectMin();
  const ImVec2 b = ImGui::GetItemRectMax();
  const float x = (a.x + b.x) * 0.5f;

  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddLine(ImVec2(x, a.y + style.FramePadding.y), ImVec2(x, b.y - style.FramePadding.y),
              ImGui::GetColorU32(ImGuiCol_Separator));

  ImGui::SameLine();
}

std::string normalize_json_pointer_copy(std::string p) {
  if (p.empty()) return "/";
  // Accept root as either "/" or "".
  if (p == "") return "/";
  if (p[0] != '/') p = "/" + p;
  return p;
}

std::string pretty_token(std::string t) {
  for (char& c : t) {
    if (c == '_') c = ' ';
  }
  if (!t.empty()) {
    // Title-case first letter (ASCII) for readability.
    if (t[0] >= 'a' && t[0] <= 'z') t[0] = static_cast<char>(t[0] - 'a' + 'A');
  }
  return t;
}

std::string label_from_pointer(const std::string& path) {
  const auto toks = nebula4x::split_json_pointer(path, /*accept_root_slash=*/true);
  if (toks.empty()) return "/";
  return pretty_token(toks.back());
}

std::int64_t sim_tick_hours(const GameState& st) {
  const std::int64_t day = st.date.days_since_epoch();
  const int hod = std::clamp(st.hour_of_day, 0, 23);
  return day * 24 + static_cast<std::int64_t>(hod);
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
  if (const auto* a = v.as_array()) {
    out = static_cast<double>(a->size());
    return true;
  }
  if (const auto* o = v.as_object()) {
    out = static_cast<double>(o->size());
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

  // Query metadata (for tooltips / navigation).
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

  if (v.is_number()) {
    const double d = v.number_value();
    r.numeric = true;
    r.value = static_cast<float>(d);
    r.display = format_number(d);
    return r;
  }
  if (v.is_bool()) {
    r.numeric = true;
    r.value = v.bool_value() ? 1.0f : 0.0f;
    r.display = v.bool_value() ? "true" : "false";
    return r;
  }
  if (v.is_string()) {
    r.display = trim_preview(v.string_value());
    return r;
  }
  if (const auto* a = v.as_array()) {
    r.numeric = true;
    r.value = static_cast<float>(a->size());
    r.display = "[" + std::to_string(a->size()) + "]";
    return r;
  }
  if (const auto* o = v.as_object()) {
    r.numeric = true;
    r.value = static_cast<float>(o->size());
    r.display = "{" + std::to_string(o->size()) + "}";
    return r;
  }
  if (v.is_null()) {
    r.display = "null";
    return r;
  }

  r.display = "(unknown)";
  return r;
}

EvalResult eval_pointer(const nebula4x::json::Value& root, const std::string& path_in) {
  EvalResult r;

  const std::string path = normalize_json_pointer_copy(path_in);
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

EvalResult eval_query(const nebula4x::json::Value& root, const std::string& pattern_in, int query_op_in,
                     const UIState& ui) {
  EvalResult r;
  r.is_query = true;
  r.query_op = std::clamp(query_op_in, 0, 4);

  const std::string pattern = normalize_json_pointer_copy(pattern_in);

  nebula4x::JsonPointerQueryStats stats;
  std::string err;

  const int max_matches = std::clamp(ui.watchboard_query_max_matches, kMinQueryMaxMatches, kMaxQueryMaxMatches);
  const int max_nodes = std::clamp(ui.watchboard_query_max_nodes, kMinQueryMaxNodes, kMaxQueryMaxNodes);

  const auto matches = nebula4x::query_json_pointer_glob(root, pattern, /*accept_root_slash=*/true, max_matches,
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
    if (!m.value) continue;

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

EvalResult eval_kpi(const nebula4x::json::Value& root, const UiForgeWidgetConfig& cfg, const UIState& ui) {
  if (cfg.is_query) return eval_query(root, cfg.path, cfg.query_op, ui);
  return eval_pointer(root, cfg.path);
}

void draw_autocomplete_list(const char* id, std::string& buf, const nebula4x::json::Value& root) {
  if (!id) return;

  const std::vector<std::string> sugg = nebula4x::suggest_json_pointer_completions(
      root, buf, 10, /*accept_root_slash=*/true, /*case_sensitive=*/false);
  if (sugg.empty()) return;

  const float h = std::min(140.0f, 18.0f * static_cast<float>(sugg.size()) + 6.0f);
  if (ImGui::BeginListBox(id, ImVec2(-1.0f, h))) {
    for (const auto& s : sugg) {
      if (ImGui::Selectable(s.c_str(), false)) {
        buf = s;
      }
    }
    ImGui::EndListBox();
  }
}

struct WidgetRuntime {
  // History.
  std::int64_t last_sample_tick{-1};
  float last_value{0.0f};
  bool has_last_value{false};
  std::vector<float> history;

  // Cached evaluation (expensive queries should not run every frame).
  std::uint64_t last_eval_revision{0};
  bool has_cached_eval{false};
  EvalResult cached_eval;

  // Detect config changes to reset history + cache.
  std::string last_path;
  bool last_is_query{false};
  int last_query_op{0};
  int last_type{0};

  // UI-only hover state (for delayed tooltips).
  bool tooltip_hovering{false};
  double tooltip_hover_start{0.0};
};

struct ForgeDoc {
  // Cached doc.
  std::shared_ptr<const nebula4x::json::Value> root;
  std::string doc_error;
  bool doc_loaded{false};
  std::uint64_t doc_revision{0};
  double last_refresh_time{0.0};
  float refresh_sec{0.35f};
};

struct ForgeEditorState {
  bool initialized{false};
  std::uint64_t selected_panel_id{0};

  // Generator knobs.
  int gen_depth{2};
  int gen_max_widgets{64};
  bool gen_replace_existing{true};

  // Optional: show live preview.
  bool show_preview{true};
};

static std::unordered_map<std::uint64_t, WidgetRuntime> g_widget_rt;
static ForgeDoc g_doc;
static ForgeEditorState g_ed;

bool ensure_doc(const Simulation& sim, ForgeDoc& st, const bool force = false) {
  const double now = ImGui::GetTime();
  ensure_game_json_cache(sim, now, st.refresh_sec, force);

  const auto& cache = game_json_cache();
  st.doc_loaded = cache.loaded && (cache.root != nullptr);
  st.doc_error = cache.error;
  st.root = cache.root;
  st.doc_revision = cache.revision;
  st.last_refresh_time = cache.last_refresh_time;

  if (st.doc_loaded && st.root) {
    (void)ensure_game_entity_index(*st.root, st.doc_revision);
  }

  return st.doc_loaded;
}

UiForgePanelConfig* find_panel(UIState& ui, std::uint64_t panel_id) {
  for (auto& p : ui.ui_forge_panels) {
    if (p.id == panel_id) return &p;
  }
  return nullptr;
}

const UiForgePanelConfig* find_panel_const(const UIState& ui, std::uint64_t panel_id) {
  for (const auto& p : ui.ui_forge_panels) {
    if (p.id == panel_id) return &p;
  }
  return nullptr;
}

void ensure_editor_initialized(UIState& ui) {
  if (g_ed.initialized) return;
  g_ed.initialized = true;

  if (!ui.ui_forge_panels.empty()) {
    g_ed.selected_panel_id = ui.ui_forge_panels.front().id;
  }
}

UiForgePanelConfig& add_new_panel(UIState& ui, const std::string& name) {
  UiForgePanelConfig p;
  p.id = ui.next_ui_forge_panel_id++;
  p.name = name;
  p.open = true;
  p.root_path = "/";
  p.desired_columns = 0;
  p.card_width_em = 20.0f;

  // Add a small note widget so the panel isn't empty.
  UiForgeWidgetConfig w;
  w.id = ui.next_ui_forge_widget_id++;
  w.type = 1;  // Text
  w.label = "Tip";
  w.text = "Right-click cards for actions (pin, open JSON explorer, create lenses/dashboards).\n"
           "Use the UI Forge window to add KPIs or auto-generate from an entity.";
  w.span = 2;
  p.widgets.push_back(std::move(w));

  ui.ui_forge_panels.push_back(std::move(p));
  return ui.ui_forge_panels.back();
}

UiForgePanelConfig& duplicate_panel(UIState& ui, const UiForgePanelConfig& src) {
  UiForgePanelConfig p = src;
  p.id = ui.next_ui_forge_panel_id++;
  p.name = src.name.empty() ? ("Panel " + std::to_string(p.id)) : (src.name + " (Copy)");
  p.open = true;

  // Re-id widgets.
  for (auto& w : p.widgets) {
    w.id = ui.next_ui_forge_widget_id++;
  }

  ui.ui_forge_panels.push_back(std::move(p));
  return ui.ui_forge_panels.back();
}

void remove_panel(UIState& ui, const std::uint64_t panel_id) {
  ui.ui_forge_panels.erase(
      std::remove_if(ui.ui_forge_panels.begin(), ui.ui_forge_panels.end(),
                     [&](const UiForgePanelConfig& p) { return p.id == panel_id; }),
      ui.ui_forge_panels.end());
}

struct WidgetCandidate {
  int type{0};
  std::string label;
  std::string path;
};

void collect_widget_candidates(const nebula4x::json::Value& v, const std::string& path, int depth,
                               std::vector<WidgetCandidate>& out, const int max_widgets) {
  if ((int)out.size() >= max_widgets) return;

  // Scalars => KPI.
  if (v.is_number() || v.is_bool() || v.is_string() || v.is_null()) {
    WidgetCandidate c;
    c.type = 0;
    c.path = path;
    c.label = label_from_pointer(path);
    out.push_back(std::move(c));
    return;
  }

  // Arrays => List preview (and optionally recurse into first element).
  if (const auto* a = v.as_array()) {
    WidgetCandidate c;
    c.type = 3;
    c.path = path;
    c.label = label_from_pointer(path) + " (List)";
    out.push_back(std::move(c));

    if (depth > 0 && !a->empty()) {
      // Recurse into element 0 to discover common fields without exploding.
      const std::string p0 = nebula4x::json_pointer_join_index(path, 0);
      collect_widget_candidates((*a)[0], p0, depth - 1, out, max_widgets);
    }
    return;
  }

  // Objects => walk keys.
  if (const auto* o = v.as_object()) {
    if (depth <= 0) {
      // If we ran out of depth, at least show the size.
      WidgetCandidate c;
      c.type = 0;
      c.path = path;
      c.label = label_from_pointer(path) + " (Size)";
      out.push_back(std::move(c));
      return;
    }

    // Deterministic order improves repeatability.
    std::vector<std::string> keys;
    keys.reserve(o->size());
    for (const auto& kv : *o) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());

    for (const auto& k : keys) {
      auto it = o->find(k);
      if (it == o->end()) continue;
      const std::string child = nebula4x::json_pointer_join(path, k);
      collect_widget_candidates(it->second, child, depth - 1, out, max_widgets);
      if ((int)out.size() >= max_widgets) return;
    }
  }
}

void generate_panel_widgets_from_root(UIState& ui, UiForgePanelConfig& panel, const nebula4x::json::Value& root) {
  const std::string rp = normalize_json_pointer_copy(panel.root_path);
  std::string err;
  const auto* node = nebula4x::resolve_json_pointer(root, rp, /*accept_root_slash=*/true, &err);
  if (!node) {
    nebula4x::log::warn("UI Forge generator: root path not found: " + rp + " (" + err + ")");
    return;
  }

  std::vector<WidgetCandidate> cand;
  cand.reserve(std::min(256, g_ed.gen_max_widgets));

  collect_widget_candidates(*node, rp, std::clamp(g_ed.gen_depth, 0, 6), cand, std::clamp(g_ed.gen_max_widgets, 1, 500));

  if (g_ed.gen_replace_existing) {
    panel.widgets.clear();
  }

  for (const auto& c : cand) {
    UiForgeWidgetConfig w;
    w.id = ui.next_ui_forge_widget_id++;
    w.type = c.type;
    w.label = c.label;
    w.path = c.path;
    w.span = (c.type == 3) ? 2 : 1;
    w.preview_rows = 8;

    // Heuristic: only track history for numeric-ish values.
    w.track_history = true;
    w.show_sparkline = true;
    w.history_len = 120;

    panel.widgets.push_back(std::move(w));
  }
}

std::string representative_pointer(const UiForgeWidgetConfig& cfg, const EvalResult& ev) {
  // Choose a "representative" strict pointer for navigation actions.
  std::string rep_ptr = normalize_json_pointer_copy(cfg.path);
  if (cfg.is_query) {
    if (!ev.sample_paths.empty()) {
      rep_ptr = ev.sample_paths[0];
    } else {
      rep_ptr = "/";
    }
  }
  return rep_ptr;
}

void draw_kpi_tooltip(const UiForgeWidgetConfig& cfg, const EvalResult& ev) {
  ImGui::BeginTooltip();
  ImGui::TextUnformatted(cfg.label.empty() ? "(KPI)" : cfg.label.c_str());
  ImGui::Separator();
  ImGui::TextDisabled("Path:");
  ImGui::TextWrapped("%s", normalize_json_pointer_copy(cfg.path).c_str());

  if (cfg.is_query) {
    ImGui::Separator();
    ImGui::TextDisabled("Query:");
    ImGui::Text("op=%s, matches=%d, numeric=%d", query_op_label(cfg.query_op).c_str(), ev.match_count, ev.numeric_count);
    ImGui::Text("visited=%d%s%s", ev.nodes_visited, ev.hit_match_limit ? " (match cap)" : "",
                ev.hit_node_limit ? " (node cap)" : "");
    if (!ev.sample_paths.empty()) {
      ImGui::Separator();
      ImGui::TextDisabled("Samples:");
      for (int i = 0; i < (int)ev.sample_paths.size() && i < 6; ++i) {
        ImGui::BulletText("%s = %s", ev.sample_paths[i].c_str(), ev.sample_previews[i].c_str());
      }
    }
  }

  if (!ev.ok && !ev.error.empty()) {
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Error: %s", ev.error.c_str());
  }

  ImGui::EndTooltip();
}

void pin_widget_to_watchboard(UIState& ui, const UiForgeWidgetConfig& cfg, const EvalResult& ev) {
  JsonWatchConfig w;
  w.id = ui.next_json_watch_id++;
  w.label = cfg.label.empty() ? label_from_pointer(cfg.path) : cfg.label;
  w.path = normalize_json_pointer_copy(cfg.path);
  w.is_query = cfg.is_query;
  w.query_op = cfg.query_op;
  w.track_history = cfg.track_history;
  w.show_sparkline = cfg.show_sparkline;
  w.history_len = cfg.history_len;

  ui.json_watch_items.push_back(std::move(w));
  ui.show_watchboard_window = true;

  // Best-effort: focus the JSON Explorer on a representative value.
  ui.request_json_explorer_goto_path = representative_pointer(cfg, ev);
}

void draw_widget_context_menu(UIState& ui, const UiForgeWidgetConfig& cfg, const EvalResult& ev) {
  const std::string rep_ptr = representative_pointer(cfg, ev);

  if (ImGui::MenuItem("Open in JSON Explorer")) {
    ui.show_json_explorer_window = true;
    ui.request_json_explorer_goto_path = rep_ptr;
  }

  if (ImGui::MenuItem("Pin to Watchboard")) {
    pin_widget_to_watchboard(ui, cfg, ev);
  }

  ImGui::Separator();

  if (ImGui::MenuItem("Create Data Lens (table)") ) {
    if (add_json_table_view(ui, rep_ptr, cfg.label.empty() ? label_from_pointer(rep_ptr) : cfg.label)) {
      ui.show_data_lenses_window = true;
    }
  }

  if (ImGui::MenuItem("Create Dashboard (charts)")) {
    if (add_json_dashboard_for_path(ui, rep_ptr, cfg.label.empty() ? label_from_pointer(rep_ptr) : cfg.label)) {
      ui.show_dashboards_window = true;
    }
  }

  if (ImGui::MenuItem("Create Pivot Table (group-by)")) {
    if (add_json_pivot_for_path(ui, rep_ptr, cfg.label.empty() ? label_from_pointer(rep_ptr) : cfg.label)) {
      ui.show_pivot_tables_window = true;
    }
  }

  if (cfg.is_query && !ev.sample_paths.empty()) {
    ImGui::Separator();
    if (ImGui::BeginMenu("Navigate to sample match")) {
      const int n = std::min<int>((int)ev.sample_paths.size(), 12);
      for (int i = 0; i < n; ++i) {
        const std::string lbl = ev.sample_paths[i];
        if (ImGui::MenuItem(lbl.c_str())) {
          ui.show_json_explorer_window = true;
          ui.request_json_explorer_goto_path = lbl;
        }
      }
      ImGui::EndMenu();
    }
  }
}

void draw_kpi_card(UIState& ui, const UiForgeWidgetConfig& cfg, WidgetRuntime& rt, const EvalResult& ev,
                   const std::int64_t tick) {
  (void)ui;

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

  // Header row: label + query badge.
  {
    const char* title = cfg.label.empty() ? label_from_pointer(cfg.path).c_str() : cfg.label.c_str();
    ImGui::TextUnformatted(title);
    if (cfg.is_query) {
      ImGui::SameLine();
      ImGui::TextDisabled("[%s]", query_op_label(cfg.query_op).c_str());
    }
  }

  // Value line.
  {
    ImGui::SetWindowFontScale(1.25f);
    if (ev.ok) {
      ImGui::TextUnformatted(ev.display.c_str());
    } else {
      ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", ev.display.c_str());
    }
    ImGui::SetWindowFontScale(1.0f);
  }

  // Delta.
  if (has_delta && std::abs(delta) > 0.000001f) {
    ImGui::TextDisabled("Δ %s", format_number(delta).c_str());
  }

  // Sparkline.
  if (cfg.show_sparkline && ev.ok && ev.numeric && rt.history.size() >= 2) {
    ImGui::PlotLines("##spark", rt.history.data(), (int)rt.history.size(), 0, nullptr, FLT_MAX, FLT_MAX,
                     ImVec2(-1.0f, 42.0f));
  }

  if (!ev.ok && !ev.error.empty()) {
    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", trim_preview(ev.error, 120).c_str());
  }

  // (Tooltip is handled at the card/window level in draw_panel_contents.)
}

void draw_list_contents(const nebula4x::json::Value& root, const UiForgeWidgetConfig& cfg, const UIState& ui,
                        EvalResult& out_eval, std::vector<std::pair<std::string, std::string>>& rows) {
  // For list widgets, we treat cfg.path as:
  //  - pointer => resolve and then preview array/object/scalar
  //  - query   => preview first N matches as (path,value)

  rows.clear();

  if (cfg.is_query) {
    out_eval = eval_query(root, cfg.path, /*query_op=*/0, ui);
    if (!out_eval.ok) return;

    const int n = std::min<int>((int)out_eval.sample_paths.size(), std::clamp(cfg.preview_rows, 1, 50));
    for (int i = 0; i < n; ++i) {
      rows.emplace_back(out_eval.sample_paths[i], out_eval.sample_previews[i]);
    }
    return;
  }

  // Pointer.
  const std::string p = normalize_json_pointer_copy(cfg.path);
  std::string err;
  const auto* node = nebula4x::resolve_json_pointer(root, p, /*accept_root_slash=*/true, &err);
  if (!node) {
    out_eval.ok = false;
    out_eval.display = "(missing)";
    out_eval.error = err;
    return;
  }

  out_eval = eval_value(*node);

  const int limit = std::clamp(cfg.preview_rows, 1, 50);

  if (const auto* a = node->as_array()) {
    const int n = std::min<int>((int)a->size(), limit);
    for (int i = 0; i < n; ++i) {
      const std::string ip = nebula4x::json_pointer_join_index(p, (std::size_t)i);
      rows.emplace_back(ip, trim_preview(eval_value((*a)[i]).display, 120));
    }
    return;
  }

  if (const auto* o = node->as_object()) {
    std::vector<std::string> keys;
    keys.reserve(o->size());
    for (const auto& kv : *o) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());

    const int n = std::min<int>((int)keys.size(), limit);
    for (int i = 0; i < n; ++i) {
      const auto& k = keys[i];
      const auto it = o->find(k);
      if (it == o->end()) continue;
      const std::string kp = nebula4x::json_pointer_join(p, k);
      rows.emplace_back(kp, trim_preview(eval_value(it->second).display, 120));
    }
    return;
  }

  // Scalar: show itself.
  rows.emplace_back(p, trim_preview(out_eval.display, 120));
}

void draw_list_card(UIState& ui, const UiForgeWidgetConfig& cfg, const UIState& ui_ro, const nebula4x::json::Value& root,
                    EvalResult& ev, std::vector<std::pair<std::string, std::string>>& rows) {
  (void)ui;

  draw_list_contents(root, cfg, ui_ro, ev, rows);

  const char* title = cfg.label.empty() ? label_from_pointer(cfg.path).c_str() : cfg.label.c_str();
  ImGui::TextUnformatted(title);
  if (cfg.is_query) {
    ImGui::SameLine();
    ImGui::TextDisabled("[query] %d", ev.match_count);
  }

  if (!ev.ok) {
    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", ev.display.c_str());
    if (!ev.error.empty()) ImGui::TextWrapped("%s", trim_preview(ev.error, 160).c_str());
    return;
  }

  if (ImGui::BeginTable("##list", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
    ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    for (const auto& r : rows) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(r.first.c_str());
      ImGui::TableSetColumnIndex(1);
      ImGui::TextWrapped("%s", r.second.c_str());
    }
    ImGui::EndTable();
  }

  if (ImGui::IsItemHovered()) {
    ImGui::BeginTooltip();
    ImGui::TextDisabled("Root:");
    ImGui::TextWrapped("%s", normalize_json_pointer_copy(cfg.path).c_str());
    ImGui::EndTooltip();
  }
}

void draw_panel_contents(const Simulation& sim, UIState& ui, const UiForgePanelConfig& panel,
                         const nebula4x::json::Value& root) {
  const ImGuiStyle& style = ImGui::GetStyle();

  const float em = ImGui::GetFontSize();
  const float base_w = std::max(10.0f * em, panel.card_width_em * em);
  const float spacing = style.ItemSpacing.x;

  int cols = panel.desired_columns;
  if (cols < 0) cols = 0;

  const float avail_w = ImGui::GetContentRegionAvail().x;
  if (cols == 0) {
    cols = std::max(1, (int)std::floor((avail_w + spacing) / (base_w + spacing)));
  }

  // Flow layout.
  float line_w = 0.0f;
  bool first_in_line = true;

  // Shared scratch for list widgets.
  static std::vector<std::pair<std::string, std::string>> list_rows;
  static EvalResult list_eval;

  const std::int64_t tick = sim_tick_hours(sim.state());

  for (const auto& cfg : panel.widgets) {
    // Separators take full width and reset the flow.
    if (cfg.type == 2) {
      if (!first_in_line) {
        ImGui::NewLine();
        line_w = 0.0f;
        first_in_line = true;
      }
      ImGui::Separator();
      continue;
    }

    int span = std::clamp(cfg.span, 1, 6);
    if (span > cols) span = cols;

    const float w = base_w * span + spacing * (span - 1);

    if (!first_in_line && (line_w + w) > avail_w) {
      ImGui::NewLine();
      line_w = 0.0f;
      first_in_line = true;
    }

    if (!first_in_line) ImGui::SameLine();

    // Card visuals.
    ImGui::PushID((int)cfg.id);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, style.FrameRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);

    const ImGuiWindowFlags child_flags = ImGuiWindowFlags_AlwaysUseWindowPadding;
    if (ImGui::BeginChild("##card", ImVec2(w, 0.0f), true, child_flags)) {
      // Context menu: right click anywhere in the card.
      if (ImGui::BeginPopupContextWindow("##ctx", ImGuiPopupFlags_MouseButtonRight)) {
        // Evaluate once for context actions.
        EvalResult ev;
        if (g_doc.root) {
          if (cfg.type == 0) {
            ev = eval_kpi(*g_doc.root, cfg, ui);
          } else if (cfg.type == 3) {
            std::vector<std::pair<std::string, std::string>> tmp;
            draw_list_contents(*g_doc.root, cfg, ui, ev, tmp);
          }
        }

        draw_widget_context_menu(ui, cfg, ev);
        ImGui::EndPopup();
      }

      if (cfg.type == 0) {
        // KPI
        auto& rt = g_widget_rt[cfg.id];

        // Detect config changes.
        if (rt.last_type != cfg.type || rt.last_path != cfg.path || rt.last_is_query != cfg.is_query ||
            rt.last_query_op != cfg.query_op) {
          rt = WidgetRuntime{};
          rt.last_type = cfg.type;
          rt.last_path = cfg.path;
          rt.last_is_query = cfg.is_query;
          rt.last_query_op = cfg.query_op;
        }

        // Eval cache.
        if (!rt.has_cached_eval || rt.last_eval_revision != g_doc.doc_revision) {
          rt.cached_eval = eval_kpi(root, cfg, ui);
          rt.last_eval_revision = g_doc.doc_revision;
          rt.has_cached_eval = true;
        }

        const EvalResult& ev = rt.cached_eval;
        draw_kpi_card(ui, cfg, rt, ev, tick);

        // Hover tooltip for KPI cards.
        //
        // We intentionally avoid imgui_internal.h and ImVec2 math operators here to keep
        // the UI Forge building against a wider range of Dear ImGui versions/configs.
        const bool card_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);
        const bool popup_open = ImGui::IsPopupOpen("##ctx");
        const bool interacting = ImGui::IsAnyItemActive() || ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
                                 ImGui::IsMouseDown(ImGuiMouseButton_Right);

        const bool want_tooltip = card_hovered && !popup_open && !interacting;
        const double now = ImGui::GetTime();

        if (want_tooltip) {
          if (!rt.tooltip_hovering) {
            rt.tooltip_hovering = true;
            rt.tooltip_hover_start = now;
          }

          // Hold Shift to show instantly; otherwise show after a short hover delay.
          const bool immediate = ImGui::GetIO().KeyShift;
          if (immediate || (now - rt.tooltip_hover_start) > 0.45) {
            draw_kpi_tooltip(cfg, ev);
          }
        } else {
          rt.tooltip_hovering = false;
        }

      } else if (cfg.type == 1) {
        // Text note.
        const char* title = cfg.label.empty() ? "Note" : cfg.label.c_str();
        ImGui::TextUnformatted(title);
        ImGui::Separator();
        ImGui::TextWrapped("%s", cfg.text.c_str());
      } else if (cfg.type == 3) {
        // List preview.
        // Evaluate on demand (cheap enough); use shared scratch buffers.
        list_rows.clear();
        list_eval = EvalResult{};
        draw_list_card(ui, cfg, ui, root, list_eval, list_rows);
      } else {
        ImGui::TextDisabled("(unknown widget type)");
      }
    }
    ImGui::EndChild();

    ImGui::PopStyleVar(2);
    ImGui::PopID();

    line_w += w + spacing;
    first_in_line = false;
  }
}

bool set_panel_root_from_entity(UIState& ui, UiForgePanelConfig& panel, const std::uint64_t entity_id,
                               const char* kind_label) {
  (void)ui;

  if (entity_id == 0) return false;
  const auto* ent = find_game_entity(entity_id);
  if (!ent) return false;
  panel.root_path = ent->path;
  if (panel.name.empty()) {
    panel.name = ent->name.empty() ? (std::string(kind_label) + " " + std::to_string(entity_id)) : ent->name;
  }
  return true;
}

} // namespace

void draw_ui_forge_panel_windows(Simulation& sim, UIState& ui) {
  // Panels are cheap when no windows are open.
  bool any_open = false;
  for (const auto& p : ui.ui_forge_panels) {
    if (p.open) {
      any_open = true;
      break;
    }
  }
  if (!any_open) return;

  const bool doc_ok = ensure_doc(sim, g_doc, /*force=*/false);
  if (!doc_ok || !g_doc.root) {
    // Still draw windows so the user sees the error and can close them.
    for (auto& p : ui.ui_forge_panels) {
      if (!p.open) continue;
      std::string title = (p.name.empty() ? "Custom Panel" : p.name);
      title += "##uiforge_" + std::to_string(p.id);
      bool open = p.open;
      if (!ImGui::Begin(title.c_str(), &open)) {
        ImGui::End();
        p.open = open;
        continue;
      }
      p.open = open;
      ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Game JSON not available.");
      if (!g_doc.doc_error.empty()) {
        ImGui::TextWrapped("%s", g_doc.doc_error.c_str());
      }
      if (ImGui::Button("Open UI Forge")) {
        ui.show_ui_forge_window = true;
        g_ed.selected_panel_id = p.id;
      }
      ImGui::End();
    }
    return;
  }

  for (auto& p : ui.ui_forge_panels) {
    if (!p.open) continue;

    std::string title = (p.name.empty() ? "Custom Panel" : p.name);
    title += "##uiforge_" + std::to_string(p.id);

    bool open = p.open;
    ImGui::SetNextWindowSize(ImVec2(520, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title.c_str(), &open)) {
      ImGui::End();
      p.open = open;
      continue;
    }
    p.open = open;

    if (ImGui::SmallButton("Edit...")) {
      ui.show_ui_forge_window = true;
      g_ed.selected_panel_id = p.id;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Root: %s", normalize_json_pointer_copy(p.root_path).c_str());

    ImGui::Separator();
    draw_panel_contents(sim, ui, p, *g_doc.root);

    ImGui::End();
  }
}

void draw_ui_forge_window(Simulation& sim, UIState& ui, Id selected_ship, Id selected_colony, Id selected_body) {
  if (!ui.show_ui_forge_window) return;

  ensure_editor_initialized(ui);

  const bool doc_ok = ensure_doc(sim, g_doc, /*force=*/false);

  ImGui::SetNextWindowSize(ImVec2(980, 640), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("UI Forge (Custom Panels)", &ui.show_ui_forge_window)) {
    ImGui::End();
    return;
  }

  ImGui::TextWrapped(
      "Build dockable custom panels procedurally from the live game-state JSON. "
      "Use JSON Pointers (RFC 6901) or wildcard queries (* and **). Right-click a card for actions.");

  if (!doc_ok || !g_doc.root) {
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Game JSON not available.");
    if (!g_doc.doc_error.empty()) ImGui::TextWrapped("%s", g_doc.doc_error.c_str());
    ImGui::End();
    return;
  }

  // Top toolbar.
  {
    if (ImGui::Button("New Panel")) {
      const std::string name = "Panel " + std::to_string(ui.next_ui_forge_panel_id);
      UiForgePanelConfig& p = add_new_panel(ui, name);
      g_ed.selected_panel_id = p.id;
    }

    ImGui::SameLine();
    if (ImGui::Button("Duplicate")) {
      if (const auto* sel = find_panel_const(ui, g_ed.selected_panel_id)) {
        UiForgePanelConfig& p = duplicate_panel(ui, *sel);
        g_ed.selected_panel_id = p.id;
      }
    }

    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
      if (g_ed.selected_panel_id != 0) {
        remove_panel(ui, g_ed.selected_panel_id);
        g_ed.selected_panel_id = ui.ui_forge_panels.empty() ? 0 : ui.ui_forge_panels.front().id;
      }
    }

    ImGui::SameLine();
    VerticalSeparator();
    ImGui::SameLine();

    if (ImGui::Button("Refresh JSON")) {
      (void)ensure_doc(sim, g_doc, /*force=*/true);
    }

    ImGui::SameLine();
    ImGui::TextDisabled("rev %llu", (unsigned long long)g_doc.doc_revision);

    ImGui::SameLine();
    VerticalSeparator();
    ImGui::SameLine();

    ImGui::Checkbox("Show preview", &g_ed.show_preview);

    ImGui::SameLine();
    ImGui::TextDisabled("Query caps: %d matches, %d nodes", ui.watchboard_query_max_matches, ui.watchboard_query_max_nodes);
  }

  ImGui::Separator();

  // Split view: panel list (left) and editor (right).
  const float left_w = 260.0f;
  ImGui::BeginChild("##uiforge_left", ImVec2(left_w, 0.0f), true);
  {
    ImGui::TextDisabled("Panels");

    if (ui.ui_forge_panels.empty()) {
      ImGui::TextWrapped("No custom panels yet.");
      ImGui::Spacing();
      if (ImGui::Button("Create a panel")) {
        UiForgePanelConfig& p = add_new_panel(ui, "Panel " + std::to_string(ui.next_ui_forge_panel_id));
        g_ed.selected_panel_id = p.id;
      }
      ImGui::EndChild();
      ImGui::SameLine();
      ImGui::BeginChild("##uiforge_right", ImVec2(0.0f, 0.0f), true);
      ImGui::TextDisabled("Select a panel to edit.");
      ImGui::EndChild();
      ImGui::End();
      return;
    }

    for (auto& p : ui.ui_forge_panels) {
      const bool sel = (p.id == g_ed.selected_panel_id);
      std::string label = p.name.empty() ? ("Panel " + std::to_string(p.id)) : p.name;
      label += p.open ? "  [open]" : "";

      if (ImGui::Selectable(label.c_str(), sel)) {
        g_ed.selected_panel_id = p.id;
      }

      // Context menu on the list entry.
      if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem(p.open ? "Close panel window" : "Open panel window")) {
          p.open = !p.open;
        }
        if (ImGui::MenuItem("Duplicate")) {
          UiForgePanelConfig& np = duplicate_panel(ui, p);
          g_ed.selected_panel_id = np.id;
        }
        if (ImGui::MenuItem("Delete")) {
          const std::uint64_t id = p.id;
          ImGui::EndPopup();
          remove_panel(ui, id);
          g_ed.selected_panel_id = ui.ui_forge_panels.empty() ? 0 : ui.ui_forge_panels.front().id;
          goto end_left;
        }
        ImGui::EndPopup();
      }
    }

  end_left:;

    ImGui::Separator();
    ImGui::TextDisabled("Quick: new from selection");

    const auto try_add_from_entity = [&](const char* btn, const std::uint64_t id, const char* kind) {
      if (id == 0) {
        ImGui::BeginDisabled();
        ImGui::Button(btn);
        ImGui::EndDisabled();
        return;
      }
      if (ImGui::Button(btn)) {
        UiForgePanelConfig p;
        p.id = ui.next_ui_forge_panel_id++;
        p.open = true;
        p.root_path = "/";
        p.card_width_em = 20.0f;

        ui.ui_forge_panels.push_back(std::move(p));
        UiForgePanelConfig& created = ui.ui_forge_panels.back();

        if (set_panel_root_from_entity(ui, created, id, kind)) {
          generate_panel_widgets_from_root(ui, created, *g_doc.root);
        }
        g_ed.selected_panel_id = created.id;
      }
    };

    try_add_from_entity("New from selected ship", selected_ship, "Ship");
    try_add_from_entity("New from selected colony", selected_colony, "Colony");
    try_add_from_entity("New from selected body", selected_body, "Body");
  }
  ImGui::EndChild();

  ImGui::SameLine();

  ImGui::BeginChild("##uiforge_right", ImVec2(0.0f, 0.0f), true);
  {
    UiForgePanelConfig* panel = find_panel(ui, g_ed.selected_panel_id);
    if (!panel) {
      ImGui::TextDisabled("Select a panel.");
      ImGui::EndChild();
      ImGui::End();
      return;
    }

    ImGui::TextDisabled("Panel");
    ImGui::InputText("Name", &panel->name);

    ImGui::Checkbox("Open as window", &panel->open);

    ImGui::Separator();
    ImGui::TextDisabled("Generator");

    ImGui::InputText("Root path", &panel->root_path);
    panel->root_path = normalize_json_pointer_copy(panel->root_path);

    // Helper buttons: set root from current selection.
    {
      if (ImGui::SmallButton("Set to selected ship")) {
        (void)set_panel_root_from_entity(ui, *panel, selected_ship, "Ship");
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Set to selected colony")) {
        (void)set_panel_root_from_entity(ui, *panel, selected_colony, "Colony");
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Set to selected body")) {
        (void)set_panel_root_from_entity(ui, *panel, selected_body, "Body");
      }
    }

    ImGui::SliderInt("Depth", &g_ed.gen_depth, 0, 6);
    ImGui::SliderInt("Max widgets", &g_ed.gen_max_widgets, 8, 300);
    ImGui::Checkbox("Replace existing widgets", &g_ed.gen_replace_existing);

    if (ImGui::Button("Auto-generate widgets")) {
      generate_panel_widgets_from_root(ui, *panel, *g_doc.root);
    }

    ImGui::SameLine();
    if (ImGui::Button("Add KPI")) {
      UiForgeWidgetConfig w;
      w.id = ui.next_ui_forge_widget_id++;
      w.type = 0;
      w.label = "KPI";
      w.path = panel->root_path;
      w.span = 1;
      panel->widgets.push_back(std::move(w));
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Note")) {
      UiForgeWidgetConfig w;
      w.id = ui.next_ui_forge_widget_id++;
      w.type = 1;
      w.label = "Note";
      w.text = "";
      w.span = 2;
      panel->widgets.push_back(std::move(w));
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Separator")) {
      UiForgeWidgetConfig w;
      w.id = ui.next_ui_forge_widget_id++;
      w.type = 2;
      w.span = 2;
      panel->widgets.push_back(std::move(w));
    }
    ImGui::SameLine();
    if (ImGui::Button("Add List")) {
      UiForgeWidgetConfig w;
      w.id = ui.next_ui_forge_widget_id++;
      w.type = 3;
      w.label = "List";
      w.path = panel->root_path;
      w.span = 2;
      panel->widgets.push_back(std::move(w));
    }

    ImGui::Separator();
    ImGui::TextDisabled("Layout");

    ImGui::SliderInt("Columns (0=auto)", &panel->desired_columns, 0, 6);
    ImGui::SliderFloat("Card width (em)", &panel->card_width_em, 12.0f, 36.0f, "%.1f");

    ImGui::Separator();
    ImGui::TextDisabled("Widgets");

    // Widget list editor.
    int dnd_src = -1;
    int dnd_dst = -1;
    for (int i = 0; i < (int)panel->widgets.size(); ++i) {
      UiForgeWidgetConfig& w = panel->widgets[i];
      ImGui::PushID((int)w.id);
      ImGui::Separator();

      // Drag-drop target: drop a widget here to move it above this widget.
      if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("UIFORGE_WIDGET_REORDER")) {
          dnd_src = *static_cast<const int*>(payload->Data);
          dnd_dst = i;
        }
        ImGui::EndDragDropTarget();
      }

      // Row controls.
      {
        ImGui::TextDisabled("#%d", i + 1);

        ImGui::SameLine();
        ImGui::SmallButton("Drag");
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("Drag to reorder");
        }
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
          ImGui::SetDragDropPayload("UIFORGE_WIDGET_REORDER", &i, sizeof(i));
          const char* name = w.label.empty() ? "(widget)" : w.label.c_str();
          ImGui::Text("Move #%d: %s", i + 1, name);
          ImGui::EndDragDropSource();
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("Up") && i > 0) {
          std::swap(panel->widgets[i], panel->widgets[i - 1]);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Down") && i + 1 < (int)panel->widgets.size()) {
          std::swap(panel->widgets[i], panel->widgets[i + 1]);
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("Dup")) {
          UiForgeWidgetConfig cp = w;
          cp.id = ui.next_ui_forge_widget_id++;
          if (!cp.label.empty()) cp.label += " (copy)";
          panel->widgets.insert(panel->widgets.begin() + (i + 1), std::move(cp));
          ImGui::PopID();
          ++i;
          continue;
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
          panel->widgets.erase(panel->widgets.begin() + i);
          ImGui::PopID();
          --i;
          continue;
        }
      }

      ImGui::InputText("Label", &w.label);

      const char* type_items = "KPI\0Text\0Separator\0List\0";
      ImGui::Combo("Type", &w.type, type_items);

      if (w.type == 0) {
        ImGui::InputText("Path", &w.path);
        w.path = normalize_json_pointer_copy(w.path);
        draw_autocomplete_list("##ac", w.path, *g_doc.root);

        ImGui::Checkbox("Query mode", &w.is_query);
        if (w.is_query) {
          const char* ops = "count\0sum\0avg\0min\0max\0";
          ImGui::Combo("Op", &w.query_op, ops);
        }

        ImGui::SliderInt("Span", &w.span, 1, 6);
        ImGui::Checkbox("Track history", &w.track_history);
        if (w.track_history) {
          ImGui::Checkbox("Show sparkline", &w.show_sparkline);
          ImGui::SliderInt("History len", &w.history_len, 10, 2000);
        }

      } else if (w.type == 1) {
        ImGui::SliderInt("Span", &w.span, 1, 6);
        ImGui::InputTextMultiline("Text", &w.text, ImVec2(-1.0f, 90.0f));
      } else if (w.type == 3) {
        ImGui::InputText("Path", &w.path);
        w.path = normalize_json_pointer_copy(w.path);
        draw_autocomplete_list("##ac", w.path, *g_doc.root);

        ImGui::Checkbox("Query mode", &w.is_query);
        if (w.is_query) {
          // List queries always show matches; query_op is unused.
          ImGui::TextDisabled("(List queries show first N matches)");
        }

        ImGui::SliderInt("Span", &w.span, 1, 6);
        ImGui::SliderInt("Preview rows", &w.preview_rows, 1, 30);
      } else if (w.type == 2) {
        ImGui::TextDisabled("Separator has no settings.");
      }

      ImGui::PopID();
    }

    // Drag-drop target: drop here to move a widget to the end.
    if (!panel->widgets.empty()) {
      ImGui::Separator();
      ImGui::TextDisabled("Tip: drag widgets to reorder (drop here to move to end)");
      if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("UIFORGE_WIDGET_REORDER")) {
          dnd_src = *static_cast<const int*>(payload->Data);
          dnd_dst = (int)panel->widgets.size();
        }
        ImGui::EndDragDropTarget();
      }
    }

    // Apply any pending re-order after rendering the editor list (safe with ImGui).
    if (dnd_src != -1 && dnd_dst != -1 && dnd_src != dnd_dst) {
      const int n = (int)panel->widgets.size();
      if (dnd_src >= 0 && dnd_src < n && dnd_dst >= 0 && dnd_dst <= n) {
        UiForgeWidgetConfig moving = std::move(panel->widgets[dnd_src]);
        panel->widgets.erase(panel->widgets.begin() + dnd_src);

        int insert_at = dnd_dst;
        if (dnd_src < dnd_dst) insert_at -= 1;
        insert_at = std::clamp(insert_at, 0, (int)panel->widgets.size());

        panel->widgets.insert(panel->widgets.begin() + insert_at, std::move(moving));
      }
    }

    if (g_ed.show_preview) {
      ImGui::Separator();
      ImGui::TextDisabled("Live Preview");
      ImGui::BeginChild("##uiforge_preview", ImVec2(0.0f, 240.0f), true);
      draw_panel_contents(sim, ui, *panel, *g_doc.root);
      ImGui::EndChild();
    }
  }
  ImGui::EndChild();

  ImGui::End();
}

} // namespace nebula4x::ui
