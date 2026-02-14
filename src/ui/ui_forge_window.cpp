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
#include "nebula4x/util/strings.h"

#include "ui/ui_forge_dna.h"

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

ImVec4 lerp_color(const ImVec4& a, const ImVec4& b, float t) {
  const float u = std::clamp(t, 0.0f, 1.0f);
  return ImVec4(a.x + (b.x - a.x) * u,
                a.y + (b.y - a.y) * u,
                a.z + (b.z - a.z) * u,
                a.w + (b.w - a.w) * u);
}

ImVec4 with_alpha(ImVec4 c, float a) {
  c.w = std::clamp(a, 0.0f, 1.0f);
  return c;
}

ImU32 color_u32(const ImVec4& c) {
  return ImGui::ColorConvertFloat4ToU32(c);
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

  // 0 = Exhaustive (walk everything up to Depth)
  // 1 = Curated (seeded, query-aware aggregation + grouping)
  int gen_mode{1};

  // Curated generator knobs.
  int gen_seed{1337};
  int gen_target_widgets{24};
  bool gen_include_lists{true};
  bool gen_include_strings{true};
  bool gen_include_id_fields{false};
  bool gen_group_separators{true};
  bool gen_add_intro_note{true};

  // Clipboard UX.
  std::string dna_status;
  double dna_status_time{0.0};

  // Optional: show live preview.
  bool show_preview{true};
};

// Preset library UI state (not persisted; the presets live in UIState).
struct ForgePresetState {
  int selected_idx{-1};
  char filter[64]{};

  // Rename modal.
  int rename_idx{-1};
  char rename_buf[128]{};
};

static std::unordered_map<std::uint64_t, WidgetRuntime> g_widget_rt;
static ForgeDoc g_doc;
static ForgeEditorState g_ed;
static ForgePresetState g_presets;

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

// --- UI Forge Presets (Panel DNA library) ---

bool preset_name_exists(const UIState& ui, const std::string& name) {
  for (const auto& p : ui.ui_forge_presets) {
    if (p.name == name) return true;
  }
  return false;
}

std::string make_unique_preset_name(const UIState& ui, std::string base) {
  if (base.empty()) base = "Preset";
  std::string name = base;
  int n = 2;
  while (preset_name_exists(ui, name)) {
    name = base + " (" + std::to_string(n++) + ")";
  }
  return name;
}

// Append a preset to UIState with simple safety caps.
void add_preset(UIState& ui, std::string name, std::string dna) {
  constexpr std::size_t kMaxPresets = 200;
  constexpr std::size_t kMaxDnaLen = 64 * 1024;

  if (dna.empty()) return;
  if (dna.size() > kMaxDnaLen) dna.resize(kMaxDnaLen);
  if (ui.ui_forge_presets.size() >= kMaxPresets) {
    // Drop the oldest entry to make room.
    ui.ui_forge_presets.erase(ui.ui_forge_presets.begin());
  }

  UiForgePanelPreset p;
  p.name = make_unique_preset_name(ui, std::move(name));
  p.dna = std::move(dna);
  ui.ui_forge_presets.push_back(std::move(p));
}

bool decode_preset_dna(const std::string& dna, UiForgePanelConfig* out_panel, std::string* err) {
  if (!out_panel) return false;

  UiForgePanelConfig tmp;
  tmp.root_path = "/";
  tmp.desired_columns = 0;
  tmp.card_width_em = 20.0f;

  if (!decode_ui_forge_panel_dna(dna, &tmp, err)) return false;
  *out_panel = std::move(tmp);
  return true;
}

void assign_fresh_widget_ids(UIState& ui, UiForgePanelConfig& panel) {
  for (auto& w : panel.widgets) {
    w.id = ui.next_ui_forge_widget_id++;
  }
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

// --- Curated procedural generator ---
//
// The exhaustive generator is useful for discovery, but it tends to create huge panels.
// The curated generator is intentionally opinionated:
//   - prefers user-facing fields (name, vitals) over ids/internal keys
//   - creates query KPIs for arrays of objects using wildcard pointers (e.g. /items/*/mass)
//   - selects a limited set of widgets with a deterministic seed

struct CuratedGenOptions {
  int depth{2};
  int target_widgets{24};
  bool replace_existing{true};

  bool include_lists{true};
  bool include_strings{true};
  bool include_id_fields{false};
  bool group_separators{true};
  bool add_intro_note{true};

  std::uint32_t seed{1337};
};

struct CuratedCandidate {
  int type{0}; // 0=KPI, 3=List
  std::string label;
  std::string path;

  bool is_query{false};
  int query_op{0};
  bool numeric{false};

  int depth{0};
  std::string key_lc;
  std::string group;
  float score{0.0f};
};

bool ends_with(const std::string& s, const std::string& suffix) {
  if (suffix.size() > s.size()) return false;
  return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin());
}

bool contains_substr(const std::string& s, const char* needle) {
  return s.find(needle) != std::string::npos;
}

bool is_noise_key(const std::string& k) {
  // Always-filter keys that are almost never meaningful in dashboards.
  return (k == "_" || k == "__" || k == "hash" || k == "guid" || k == "uuid" || k == "checksum" || k == "version" ||
          k == "revision" || k == "rev" || k == "debug" || k == "internal" || k == "last_updated" || k == "last_update" ||
          k == "timestamp" || k == "time_stamp");
}

bool is_id_like_key(const std::string& k) {
  if (k == "id" || k == "uid" || k == "guid") return true;
  if (ends_with(k, "_id")) return true;
  if (ends_with(k, "_ids")) return true;
  if (ends_with(k, "_idx") || ends_with(k, "_index")) return true;
  if (contains_substr(k, "id_") && k.size() <= 8) return true;
  return false;
}

std::string classify_group(const CuratedCandidate& c) {
  if (c.type == 3) return "Collections";
  const std::string& k = c.key_lc;

  if (k == "name" || contains_substr(k, "name") || contains_substr(k, "title") || contains_substr(k, "class") ||
      contains_substr(k, "designation") || contains_substr(k, "hull") || contains_substr(k, "model") ||
      contains_substr(k, "type")) {
    return "Identity";
  }

  if (contains_substr(k, "system") || contains_substr(k, "orbit") || contains_substr(k, "pos") || contains_substr(k, "location") ||
      k == "x" || k == "y" || k == "z" || contains_substr(k, "coord") || contains_substr(k, "sector") ||
      contains_substr(k, "region")) {
    return "Location";
  }

  if (contains_substr(k, "pop") || contains_substr(k, "industry") || contains_substr(k, "econ") || contains_substr(k, "wealth") ||
      contains_substr(k, "credit") || contains_substr(k, "cost") || contains_substr(k, "income") || contains_substr(k, "output") ||
      contains_substr(k, "prod") || contains_substr(k, "cargo") || contains_substr(k, "fuel") || contains_substr(k, "stock") ||
      contains_substr(k, "inventory") || contains_substr(k, "mineral") || contains_substr(k, "ore") || contains_substr(k, "supply")) {
    return "Economy";
  }

  if (contains_substr(k, "hp") || contains_substr(k, "armor") || contains_substr(k, "shield") || contains_substr(k, "weapon") ||
      contains_substr(k, "missile") || contains_substr(k, "damage") || contains_substr(k, "range") || contains_substr(k, "combat") ||
      contains_substr(k, "ton") || contains_substr(k, "mass") || contains_substr(k, "speed") || contains_substr(k, "thrust")) {
    return "Combat";
  }

  if (contains_substr(k, "research") || contains_substr(k, "tech") || contains_substr(k, "lab") || contains_substr(k, "science")) {
    return "Research";
  }

  if (contains_substr(k, "queue") || contains_substr(k, "order") || contains_substr(k, "plan") || contains_substr(k, "task") ||
      contains_substr(k, "eta") || contains_substr(k, "time")) {
    return "Plans";
  }

  return "General";
}

int guess_query_op_for_key(const std::string& k, const bool is_bool) {
  // 0=count, 1=sum, 2=avg, 3=min, 4=max
  if (is_bool) return 1; // sum bools => count(true)
  if (contains_substr(k, "min")) return 3;
  if (contains_substr(k, "max")) return 4;
  if (contains_substr(k, "pct") || contains_substr(k, "ratio") || contains_substr(k, "fraction") || contains_substr(k, "chance") ||
      contains_substr(k, "prob") || contains_substr(k, "mean") || contains_substr(k, "avg")) {
    return 2;
  }
  return 1; // sum by default
}

std::uint32_t fnv1a_32(const std::string& s) {
  std::uint32_t h = 2166136261u;
  for (unsigned char c : s) {
    h ^= (std::uint32_t)c;
    h *= 16777619u;
  }
  return h;
}

float jitter01(std::uint32_t seed, const std::string& path) {
  std::uint32_t h = fnv1a_32(path);
  h ^= seed + 0x9e3779b9u + (h << 6) + (h >> 2);
  // 0..1
  return (float)(h & 0xFFFFu) / 65535.0f;
}

int pointer_depth(const std::string& path) {
  try {
    return (int)nebula4x::split_json_pointer(path, /*accept_root_slash=*/true).size();
  } catch (...) {
    return 0;
  }
}

std::string last_token_lc(const std::string& path) {
  try {
    const auto toks = nebula4x::split_json_pointer(path, /*accept_root_slash=*/true);
    if (toks.empty()) return "";
    return nebula4x::to_lower(toks.back());
  } catch (...) {
    return "";
  }
}

void push_candidate(std::vector<CuratedCandidate>& out, CuratedCandidate c, const CuratedGenOptions& opt) {
  c.depth = pointer_depth(c.path);
  c.key_lc = last_token_lc(c.path);

  if (c.key_lc.empty()) return;
  if (is_noise_key(c.key_lc)) return;
  if (!opt.include_id_fields && is_id_like_key(c.key_lc) && c.key_lc != "name") return;

  c.group = classify_group(c);

  // Score heuristics.
  float s = 0.0f;

  // Type bias.
  s += (c.type == 3) ? 12.0f : 20.0f;
  if (c.is_query) s += 18.0f;
  if (c.numeric) s += 16.0f;

  // Prefer shallower paths.
  s += std::max(0.0f, 80.0f - (float)c.depth * 10.0f);

  // Keyword boosts.
  const std::string& k = c.key_lc;
  if (k == "name") s += 260.0f;
  if (contains_substr(k, "pop")) s += 120.0f;
  if (contains_substr(k, "fuel")) s += 110.0f;
  if (contains_substr(k, "speed") || contains_substr(k, "vel")) s += 90.0f;
  if (contains_substr(k, "mass") || contains_substr(k, "ton")) s += 80.0f;
  if (contains_substr(k, "hp") || contains_substr(k, "armor") || contains_substr(k, "shield")) s += 95.0f;
  if (contains_substr(k, "income") || contains_substr(k, "output") || contains_substr(k, "prod")) s += 75.0f;
  if (contains_substr(k, "mineral") || contains_substr(k, "ore")) s += 65.0f;
  if (contains_substr(k, "research") || contains_substr(k, "tech")) s += 65.0f;

  // Penalties.
  if (is_id_like_key(k)) s -= 140.0f;

  // Deterministic jitter so two fields with similar scores can be varied via seed.
  s += jitter01(opt.seed, c.path) * 10.0f;

  c.score = s;
  out.push_back(std::move(c));
}

void collect_curated_candidates(const nebula4x::json::Value& v, const std::string& path, int depth, const CuratedGenOptions& opt,
                               std::vector<CuratedCandidate>& out, const int max_total) {
  if ((int)out.size() >= max_total) return;

  // Scalars => KPI.
  if (v.is_null() || v.is_bool() || v.is_number() || v.is_string()) {
    if (v.is_string() && !opt.include_strings) {
      // Always keep 'name' even when strings are off.
      const std::string k = last_token_lc(path);
      if (k != "name") return;
    }

    CuratedCandidate c;
    c.type = 0;
    c.path = path;
    c.label = label_from_pointer(path);
    c.numeric = v.is_number() || v.is_bool();
    push_candidate(out, std::move(c), opt);
    return;
  }

  // Arrays.
  if (const auto* a = v.as_array()) {
    // Always include a size KPI (arrays evaluate to their length in eval_value).
    {
      CuratedCandidate c;
      c.type = 0;
      c.path = path;
      c.label = label_from_pointer(path);
      c.numeric = true;
      push_candidate(out, std::move(c), opt);
    }

    if (opt.include_lists) {
      CuratedCandidate c;
      c.type = 3;
      c.path = path;
      c.label = label_from_pointer(path);
      c.numeric = false;
      push_candidate(out, std::move(c), opt);
    }

    if (depth > 0 && !a->empty()) {
      const auto& e0 = (*a)[0];
      const std::string wildcard = nebula4x::json_pointer_join(path, "*");

      // Arrays of objects => query KPIs over numeric fields.
      if (const auto* o = e0.as_object()) {
        // Deterministic iteration improves repeatability.
        std::vector<std::string> keys;
        keys.reserve(o->size());
        for (const auto& kv : *o) keys.push_back(kv.first);
        std::sort(keys.begin(), keys.end());

        for (const auto& k : keys) {
          auto it = o->find(k);
          if (it == o->end()) continue;

          const auto& vv = it->second;
          const bool numeric = vv.is_number() || vv.is_bool();
          if (!numeric) continue;

          CuratedCandidate q;
          q.type = 0;
          q.path = nebula4x::json_pointer_join(wildcard, k);
          q.label = label_from_pointer(q.path);
          q.is_query = true;
          q.numeric = true;
          q.query_op = guess_query_op_for_key(nebula4x::to_lower(k), vv.is_bool());
          push_candidate(out, std::move(q), opt);

          if ((int)out.size() >= max_total) return;
        }
      } else if (e0.is_number() || e0.is_bool()) {
        // Arrays of scalars => query KPI directly.
        CuratedCandidate q;
        q.type = 0;
        q.path = wildcard;
        q.label = label_from_pointer(path);
        q.is_query = true;
        q.numeric = true;
        q.query_op = guess_query_op_for_key(last_token_lc(path), e0.is_bool());
        push_candidate(out, std::move(q), opt);
      }
    }

    return;
  }

  // Objects.
  if (const auto* o = v.as_object()) {
    if (depth <= 0) {
      // Size KPI at max depth.
      CuratedCandidate c;
      c.type = 0;
      c.path = path;
      c.label = label_from_pointer(path);
      c.numeric = true;
      push_candidate(out, std::move(c), opt);
      return;
    }

    std::vector<std::string> keys;
    keys.reserve(o->size());
    for (const auto& kv : *o) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());

    for (const auto& k : keys) {
      auto it = o->find(k);
      if (it == o->end()) continue;
      const std::string child = nebula4x::json_pointer_join(path, k);
      collect_curated_candidates(it->second, child, depth - 1, opt, out, max_total);
      if ((int)out.size() >= max_total) return;
    }
  }
}

int group_rank(const std::string& g) {
  // Lower is earlier.
  if (g == "Identity") return 0;
  if (g == "Location") return 1;
  if (g == "Economy") return 2;
  if (g == "Combat") return 3;
  if (g == "Research") return 4;
  if (g == "Plans") return 5;
  if (g == "Collections") return 6;
  return 7;
}

void generate_panel_widgets_curated(UIState& ui, UiForgePanelConfig& panel, const nebula4x::json::Value& root,
                                   const CuratedGenOptions& opt_in) {
  CuratedGenOptions opt = opt_in;
  opt.depth = std::clamp(opt.depth, 0, 6);
  opt.target_widgets = std::clamp(opt.target_widgets, 1, 200);

  const std::string rp = normalize_json_pointer_copy(panel.root_path);
  std::string err;
  const auto* node = nebula4x::resolve_json_pointer(root, rp, /*accept_root_slash=*/true, &err);
  if (!node) {
    nebula4x::log::warn("UI Forge curated generator: root path not found: " + rp + " (" + err + ")");
    return;
  }

  std::vector<CuratedCandidate> cand;
  cand.reserve(256);

  // Budget is a bit higher than target so we can score/select.
  const int budget = std::clamp(opt.target_widgets * 6, 32, 900);
  collect_curated_candidates(*node, rp, opt.depth, opt, cand, budget);

  // Select.
  std::sort(cand.begin(), cand.end(), [](const CuratedCandidate& a, const CuratedCandidate& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.path < b.path;
  });

  std::vector<CuratedCandidate> picked;
  picked.reserve(opt.target_widgets);

  // Soft cap per group to avoid all-economy or all-ids.
  const int max_per_group = std::max(3, opt.target_widgets / 3);
  std::unordered_map<std::string, int> group_counts;
  std::unordered_map<std::string, bool> used_paths;

  for (const auto& c : cand) {
    if ((int)picked.size() >= opt.target_widgets) break;
    if (used_paths[c.path]) continue;

    const int gc = group_counts[c.group];
    if (gc >= max_per_group) continue;

    picked.push_back(c);
    used_paths[c.path] = true;
    group_counts[c.group] = gc + 1;
  }

  // Group ordering.
  std::sort(picked.begin(), picked.end(), [](const CuratedCandidate& a, const CuratedCandidate& b) {
    const int ra = group_rank(a.group);
    const int rb = group_rank(b.group);
    if (ra != rb) return ra < rb;
    if (a.score != b.score) return a.score > b.score;
    return a.path < b.path;
  });

  if (opt.replace_existing) {
    panel.widgets.clear();
  }

  const auto push_sep = [&](const std::string& label) {
    UiForgeWidgetConfig w;
    w.id = ui.next_ui_forge_widget_id++;
    w.type = 2;
    w.span = 2;
    w.label = label;
    panel.widgets.push_back(std::move(w));
  };

  if (opt.add_intro_note && opt.replace_existing) {
    UiForgeWidgetConfig note;
    note.id = ui.next_ui_forge_widget_id++;
    note.type = 1;
    note.span = 2;
    note.label = "Generated";
    note.text = "Curated panel (seed=" + std::to_string(opt.seed) + ")\nRoot: " + rp +
                "\nTip: Ctrl+P opens Command Palette. Use Copy/Paste Panel DNA to share.";
    panel.widgets.push_back(std::move(note));
    push_sep("Overview");
  }

  std::string last_group;
  for (const auto& c : picked) {
    if (opt.group_separators && c.group != last_group) {
      push_sep(c.group);
    }
    last_group = c.group;

    UiForgeWidgetConfig w;
    w.id = ui.next_ui_forge_widget_id++;
    w.type = c.type;
    w.label = c.label;
    w.path = c.path;
    w.span = (c.type == 3) ? 2 : 1;

    if (c.type == 0) {
      w.is_query = c.is_query;
      w.query_op = c.query_op;

      w.track_history = c.numeric;
      w.show_sparkline = true;
      w.history_len = c.is_query ? 180 : 120;

      const std::string key = last_token_lc(c.path);
      const bool key_is_name_like =
          (key == "name" || contains_substr(key, "name") || contains_substr(key, "title"));
      const bool key_is_total_like =
          contains_substr(key, "total") || contains_substr(key, "sum") || contains_substr(key, "count");
      if (key_is_name_like || key_is_total_like || (c.is_query && c.query_op != 3 && c.query_op != 4)) {
        w.span = 2;
      }
    } else if (c.type == 3) {
      w.is_query = c.is_query;
      w.preview_rows = 10;
    }

    panel.widgets.push_back(std::move(w));
  }
}

void generate_panel_widgets_auto(UIState& ui, UiForgePanelConfig& panel, const nebula4x::json::Value& root) {
  if (g_ed.gen_mode == 1) {
    CuratedGenOptions opt;
    opt.depth = g_ed.gen_depth;
    opt.target_widgets = g_ed.gen_target_widgets;
    opt.replace_existing = g_ed.gen_replace_existing;
    opt.include_lists = g_ed.gen_include_lists;
    opt.include_strings = g_ed.gen_include_strings;
    opt.include_id_fields = g_ed.gen_include_id_fields;
    opt.group_separators = g_ed.gen_group_separators;
    opt.add_intro_note = g_ed.gen_add_intro_note;
    opt.seed = static_cast<std::uint32_t>(g_ed.gen_seed);
    generate_panel_widgets_curated(ui, panel, root, opt);
  } else {
    generate_panel_widgets_from_root(ui, panel, root);
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

  if (cfg.is_query) {
    ImGui::TextDisabled("matches %d | numeric %d", ev.match_count, ev.numeric_count);
  }

  // Delta.
  if (has_delta && std::abs(delta) > 0.000001f) {
    const bool positive = delta >= 0.0f;
    const ImVec4 pos_col = lerp_color(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), ImVec4(0.35f, 0.92f, 0.55f, 1.0f), 0.45f);
    const ImVec4 neg_col = ImVec4(0.96f, 0.43f, 0.38f, 1.0f);
    ImGui::TextColored(positive ? pos_col : neg_col, "delta %s%s", positive ? "+" : "", format_number(delta).c_str());
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
  ImGui::TextDisabled("rows %d", static_cast<int>(rows.size()));

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
      if (!cfg.label.empty()) {
        ImGui::SeparatorText(cfg.label.c_str());
      } else {
        ImGui::Separator();
      }
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
    ImVec4 card_bg = style.Colors[ImGuiCol_ChildBg];
    ImVec4 card_border = style.Colors[ImGuiCol_Border];
    ImVec4 card_accent = style.Colors[ImGuiCol_CheckMark];
    switch (cfg.type) {
      case 0: // KPI
        card_bg = with_alpha(lerp_color(card_bg, style.Colors[ImGuiCol_FrameBg], 0.30f), std::max(card_bg.w, 0.94f));
        card_border = with_alpha(lerp_color(card_border, card_accent, 0.28f), std::max(card_border.w, 0.55f));
        break;
      case 1: // note
        card_bg = with_alpha(lerp_color(card_bg, style.Colors[ImGuiCol_PlotHistogram], 0.16f), std::max(card_bg.w, 0.94f));
        card_border = with_alpha(lerp_color(card_border, style.Colors[ImGuiCol_PlotHistogram], 0.24f), std::max(card_border.w, 0.52f));
        card_accent = style.Colors[ImGuiCol_PlotHistogram];
        break;
      case 3: // list
        card_bg = with_alpha(lerp_color(card_bg, style.Colors[ImGuiCol_Header], 0.22f), std::max(card_bg.w, 0.94f));
        card_border = with_alpha(lerp_color(card_border, style.Colors[ImGuiCol_HeaderHovered], 0.26f), std::max(card_border.w, 0.52f));
        card_accent = style.Colors[ImGuiCol_HeaderHovered];
        break;
      default:
        break;
    }

    ImGui::PushID((int)cfg.id);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, card_bg);
    ImGui::PushStyleColor(ImGuiCol_Border, card_border);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, style.FrameRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);

    const ImGuiWindowFlags child_flags = ImGuiWindowFlags_AlwaysUseWindowPadding;
    if (ImGui::BeginChild("##card", ImVec2(w, 0.0f), true, child_flags)) {
      const ImVec2 card_min = ImGui::GetWindowPos();
      const float strip_h = std::max(2.0f, std::round(ImGui::GetFontSize() * 0.16f));
      ImGui::GetWindowDrawList()->AddRectFilled(
          card_min,
          ImVec2(card_min.x + ImGui::GetWindowWidth(), card_min.y + strip_h),
          color_u32(with_alpha(card_accent, 0.70f)));

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
    ImGui::PopStyleColor(2);
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

bool ensure_ui_forge_base_panels(Simulation& sim, UIState& ui) {
  if (!ui.ui_forge_panels.empty()) return true;

  const bool doc_ok = ensure_doc(sim, g_doc, /*force=*/false);
  if (!doc_ok || !g_doc.root) return false;

  UiForgePanelConfig panel;
  panel.id = ui.next_ui_forge_panel_id++;
  panel.name = "Procedural Command Deck";
  panel.open = true;
  panel.root_path = "/";
  panel.desired_columns = 0;
  panel.card_width_em = 19.0f;
  ui.ui_forge_panels.push_back(std::move(panel));

  CuratedGenOptions opt;
  opt.depth = 2;
  opt.target_widgets = 18;
  opt.replace_existing = true;
  opt.include_lists = true;
  opt.include_strings = true;
  opt.include_id_fields = false;
  opt.group_separators = true;
  opt.add_intro_note = true;
  opt.seed = static_cast<std::uint32_t>(
      static_cast<std::uint32_t>(ui.ui_procedural_theme_seed) ^
      (static_cast<std::uint32_t>(ui.ui_procedural_layout_seed) * 1664525u) ^
      0x9e3779b9u);

  generate_panel_widgets_curated(ui, ui.ui_forge_panels.back(), *g_doc.root, opt);
  if (ui.ui_forge_panels.back().widgets.empty()) {
    UiForgeWidgetConfig w;
    w.id = ui.next_ui_forge_widget_id++;
    w.type = 1;
    w.span = 2;
    w.label = "Procedural Command Deck";
    w.text = "Starter panel created. Open UI Forge to customize widgets.";
    ui.ui_forge_panels.back().widgets.push_back(std::move(w));
  }

  return true;
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

    const bool have_sel_panel = (find_panel_const(ui, g_ed.selected_panel_id) != nullptr);
    if (!have_sel_panel) ImGui::BeginDisabled();
    if (ImGui::Button("Copy Panel DNA")) {
      if (const auto* sel = find_panel_const(ui, g_ed.selected_panel_id)) {
        const std::string dna = encode_ui_forge_panel_dna(*sel);
        ImGui::SetClipboardText(dna.c_str());
        g_ed.dna_status = "Copied panel DNA to clipboard.";
        g_ed.dna_status_time = ImGui::GetTime();
      }
    }
    if (!have_sel_panel) ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Paste Panel DNA")) {
      const char* clip = ImGui::GetClipboardText();
      std::string err;
      UiForgePanelConfig imported;
      imported.root_path = "/";
      imported.desired_columns = 0;
      imported.card_width_em = 20.0f;

      if (clip && decode_ui_forge_panel_dna(clip, &imported, &err)) {
        const bool replace = have_sel_panel && ImGui::GetIO().KeyShift;
        if (replace) {
          UiForgePanelConfig* tgt = find_panel(ui, g_ed.selected_panel_id);
          if (tgt) {
            const std::uint64_t keep_id = tgt->id;
            const bool keep_open = tgt->open;
            *tgt = imported;
            tgt->id = keep_id;
            tgt->open = keep_open;
            for (auto& w : tgt->widgets) w.id = ui.next_ui_forge_widget_id++;
            g_ed.dna_status = "Replaced selected panel from clipboard.";
            g_ed.dna_status_time = ImGui::GetTime();
          }
        } else {
          imported.id = ui.next_ui_forge_panel_id++;
          if (imported.name.empty()) imported.name = "Imported Panel";
          for (auto& w : imported.widgets) w.id = ui.next_ui_forge_widget_id++;
          ui.ui_forge_panels.push_back(std::move(imported));
          g_ed.selected_panel_id = ui.ui_forge_panels.back().id;
          g_ed.dna_status = "Imported new panel from clipboard.";
          g_ed.dna_status_time = ImGui::GetTime();
        }
      } else {
        g_ed.dna_status = err.empty() ? "Clipboard does not contain panel DNA." : ("Panel DNA error: " + err);
        g_ed.dna_status_time = ImGui::GetTime();
      }
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Paste a panel from clipboard. Hold Shift to replace the selected panel.");
    }

    if (!g_ed.dna_status.empty() && (ImGui::GetTime() - g_ed.dna_status_time) < 3.5) {
      ImGui::SameLine();
      ImGui::TextDisabled("%s", g_ed.dna_status.c_str());
    }

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
        ImGui::Separator();
        if (ImGui::MenuItem("Copy DNA to clipboard")) {
          const std::string dna = encode_ui_forge_panel_dna(p);
          ImGui::SetClipboardText(dna.c_str());
          g_ed.dna_status = "Copied panel DNA to clipboard.";
          g_ed.dna_status_time = ImGui::GetTime();
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
          generate_panel_widgets_auto(ui, created, *g_doc.root);
        }
        g_ed.selected_panel_id = created.id;
      }
    };

    try_add_from_entity("New from selected ship", selected_ship, "Ship");
    try_add_from_entity("New from selected colony", selected_colony, "Colony");
    try_add_from_entity("New from selected body", selected_body, "Body");

    ImGui::Separator();

    // --- Preset Library ---
    ImGui::TextDisabled("Presets (Panel DNA Library)");

    const UiForgePanelConfig* sel_panel = find_panel_const(ui, g_ed.selected_panel_id);
    if (!sel_panel) ImGui::BeginDisabled();
    if (ImGui::Button("Save selected panel as preset")) {
      const std::string dna = encode_ui_forge_panel_dna(*sel_panel);
      const std::string base = sel_panel->name.empty() ? ("Panel " + std::to_string(sel_panel->id)) : sel_panel->name;
      add_preset(ui, base, dna);
      g_ed.dna_status = "Saved selected panel to preset library.";
      g_ed.dna_status_time = ImGui::GetTime();
    }
    if (!sel_panel) ImGui::EndDisabled();

    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Stores the current panel as a reusable preset in ui_prefs.json.");
    }

    if (ImGui::Button("Import preset from clipboard")) {
      const char* clip = ImGui::GetClipboardText();
      if (clip && clip[0]) {
        UiForgePanelConfig imported;
        std::string err;
        if (decode_preset_dna(std::string(clip), &imported, &err)) {
          const std::string name = imported.name.empty() ? "Imported Preset" : imported.name;
          // Normalize to canonical encoding so presets remain stable even if pasted JSON was formatted oddly.
          add_preset(ui, name, encode_ui_forge_panel_dna(imported));
          g_ed.dna_status = "Imported preset from clipboard.";
        } else {
          g_ed.dna_status = err.empty() ? "Clipboard does not contain panel DNA." : ("Panel DNA error: " + err);
        }
        g_ed.dna_status_time = ImGui::GetTime();
      }
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Parses panel DNA from clipboard and stores it as a named preset.");
    }

    ImGui::InputTextWithHint("##uiforge_preset_filter", "Filter presets...", g_presets.filter,
                             IM_ARRAYSIZE(g_presets.filter));

    // Preset list.
    {
      ImGui::BeginChild("##uiforge_preset_list", ImVec2(0, 150), true);

      const std::string needle = nebula4x::to_lower(std::string(g_presets.filter));
      int visible_count = 0;

      for (int i = 0; i < (int)ui.ui_forge_presets.size(); ++i) {
        const auto& pr = ui.ui_forge_presets[(std::size_t)i];
        const std::string hay = nebula4x::to_lower(pr.name);
        if (!needle.empty() && hay.find(needle) == std::string::npos) continue;

        const bool sel = (i == g_presets.selected_idx);
        std::string label = pr.name.empty() ? ("Preset " + std::to_string(i + 1)) : pr.name;
        label += "##uiforge_preset_" + std::to_string(i);

        if (ImGui::Selectable(label.c_str(), sel)) {
          g_presets.selected_idx = i;
        }

        // Context menu.
        if (ImGui::BeginPopupContextItem()) {
          if (ImGui::MenuItem("Copy preset DNA to clipboard")) {
            ImGui::SetClipboardText(pr.dna.c_str());
            g_ed.dna_status = "Copied preset DNA to clipboard.";
            g_ed.dna_status_time = ImGui::GetTime();
          }
          if (ImGui::MenuItem("Rename...")) {
            g_presets.rename_idx = i;
            std::snprintf(g_presets.rename_buf, sizeof(g_presets.rename_buf), "%s", pr.name.c_str());
            ImGui::OpenPopup("Rename preset##uiforge");
          }
          if (ImGui::MenuItem("Delete")) {
            ui.ui_forge_presets.erase(ui.ui_forge_presets.begin() + i);
            if (g_presets.selected_idx >= (int)ui.ui_forge_presets.size()) {
              g_presets.selected_idx = (int)ui.ui_forge_presets.size() - 1;
            }
            ImGui::EndPopup();
            break;
          }
          ImGui::EndPopup();
        }

        ++visible_count;
      }

      if (visible_count == 0) {
        ImGui::TextDisabled(ui.ui_forge_presets.empty() ? "No presets yet." : "No matching presets.");
      }

      ImGui::EndChild();
    }

    // Rename modal.
    if (ImGui::BeginPopupModal("Rename preset##uiforge", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextUnformatted("Rename preset");
      ImGui::Spacing();
      ImGui::InputText("Name", g_presets.rename_buf, IM_ARRAYSIZE(g_presets.rename_buf));

      const bool can_apply = (g_presets.rename_idx >= 0) && (g_presets.rename_idx < (int)ui.ui_forge_presets.size());
      if (!can_apply) ImGui::BeginDisabled();
      if (ImGui::Button("Apply")) {
        UiForgePanelPreset& p = ui.ui_forge_presets[(std::size_t)g_presets.rename_idx];
        const std::string wanted = std::string(g_presets.rename_buf);
        p.name = make_unique_preset_name(ui, wanted);
        g_ed.dna_status = "Renamed preset.";
        g_ed.dna_status_time = ImGui::GetTime();
        ImGui::CloseCurrentPopup();
      }
      if (!can_apply) ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }

    // Selected preset actions.
    const bool have_sel_preset = (g_presets.selected_idx >= 0) && (g_presets.selected_idx < (int)ui.ui_forge_presets.size());
    if (!have_sel_preset) {
      ImGui::TextDisabled("Select a preset to use it.");
    } else {
      const UiForgePanelPreset& pr = ui.ui_forge_presets[(std::size_t)g_presets.selected_idx];

      if (ImGui::Button("Create panel from preset")) {
        UiForgePanelConfig imported;
        std::string err;
        if (decode_preset_dna(pr.dna, &imported, &err)) {
          imported.id = ui.next_ui_forge_panel_id++;
          if (imported.name.empty()) imported.name = pr.name;
          imported.open = true;
          assign_fresh_widget_ids(ui, imported);
          ui.ui_forge_panels.push_back(std::move(imported));
          g_ed.selected_panel_id = ui.ui_forge_panels.back().id;
          g_ed.dna_status = "Created a new panel from preset.";
        } else {
          g_ed.dna_status = err.empty() ? "Preset DNA is invalid." : ("Preset DNA error: " + err);
        }
        g_ed.dna_status_time = ImGui::GetTime();
      }

      if (!sel_panel) ImGui::BeginDisabled();
      if (ImGui::Button("Replace selected panel from preset")) {
        UiForgePanelConfig* tgt = find_panel(ui, g_ed.selected_panel_id);
        if (tgt) {
          UiForgePanelConfig imported = *tgt;
          std::string err;
          if (decode_ui_forge_panel_dna(pr.dna, &imported, &err)) {
            const std::uint64_t keep_id = tgt->id;
            const bool keep_open = tgt->open;
            *tgt = std::move(imported);
            tgt->id = keep_id;
            tgt->open = keep_open;
            assign_fresh_widget_ids(ui, *tgt);
            g_ed.dna_status = "Replaced selected panel from preset.";
          } else {
            g_ed.dna_status = err.empty() ? "Preset DNA is invalid." : ("Preset DNA error: " + err);
          }
          g_ed.dna_status_time = ImGui::GetTime();
        }
      }
      if (!sel_panel) ImGui::EndDisabled();

      if (ImGui::Button("Copy preset DNA")) {
        ImGui::SetClipboardText(pr.dna.c_str());
        g_ed.dna_status = "Copied preset DNA to clipboard.";
        g_ed.dna_status_time = ImGui::GetTime();
      }
    }
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

    const char* modes = "Exhaustive\0Curated\0";
    ImGui::Combo("Mode", &g_ed.gen_mode, modes);

    ImGui::SliderInt("Depth", &g_ed.gen_depth, 0, 6);

    if (g_ed.gen_mode == 0) {
      ImGui::SliderInt("Max widgets", &g_ed.gen_max_widgets, 8, 300);
    } else {
      ImGui::InputInt("Seed", &g_ed.gen_seed);
      ImGui::SameLine();
      if (ImGui::SmallButton("Mutate seed")) {
        std::uint32_t x = static_cast<std::uint32_t>(g_ed.gen_seed);
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        g_ed.gen_seed = static_cast<int>(x);
      }

      ImGui::SliderInt("Target widgets", &g_ed.gen_target_widgets, 6, 80);

      ImGui::Checkbox("Include lists", &g_ed.gen_include_lists);
      ImGui::SameLine();
      ImGui::Checkbox("Include strings", &g_ed.gen_include_strings);
      ImGui::SameLine();
      ImGui::Checkbox("Include id fields", &g_ed.gen_include_id_fields);

      ImGui::Checkbox("Group separators", &g_ed.gen_group_separators);
      ImGui::SameLine();
      ImGui::Checkbox("Intro note", &g_ed.gen_add_intro_note);

      ImGui::TextDisabled("Curated mode will also create wildcard query KPIs for arrays (e.g. /items/*/mass). ");
    }

    ImGui::Checkbox("Replace existing widgets", &g_ed.gen_replace_existing);

    if (ImGui::Button("Generate widgets")) {
      generate_panel_widgets_auto(ui, *panel, *g_doc.root);
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
        ImGui::InputText("Label", &w.label);
        ImGui::TextDisabled("Leave empty for a plain separator.");
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

