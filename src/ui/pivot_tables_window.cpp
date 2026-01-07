#include "ui/pivot_tables_window.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nebula4x/util/json.h"
#include "nebula4x/util/json_pointer.h"
#include "nebula4x/util/json_pointer_autocomplete.h"
#include "nebula4x/util/log.h"
#include "nebula4x/util/strings.h"

#include "ui/game_json_cache.h"

#include "ui/data_lenses_window.h"

namespace nebula4x::ui {

namespace {

static std::string normalize_json_pointer_copy(std::string p) {
  if (p.empty()) p = "/";
  if (!p.empty() && p[0] != '/') p = "/" + p;
  return p;
}

static std::string last_pointer_token(const std::string& p) {
  try {
    const auto toks = nebula4x::split_json_pointer(p, /*accept_root_slash=*/true);
    if (!toks.empty()) return toks.back();
  } catch (...) {
    // ignore
  }
  return std::string();
}

static std::string scalar_preview(const nebula4x::json::Value& v, int max_len = 96) {
  char buf[128];
  if (v.is_null()) return "null";
  if (v.is_bool()) return v.bool_value() ? "true" : "false";
  if (v.is_number()) {
    std::snprintf(buf, sizeof(buf), "%.6g", v.number_value());
    return std::string(buf);
  }
  if (v.is_string()) {
    std::string s = v.string_value();
    if ((int)s.size() > max_len) {
      s.resize((std::size_t)max_len);
      s += "…";
    }
    return s;
  }
  if (v.is_array()) {
    const auto* a = v.as_array();
    std::snprintf(buf, sizeof(buf), "[...](%zu)", a ? a->size() : 0);
    return std::string(buf);
  }
  if (v.is_object()) {
    const auto* o = v.as_object();
    std::snprintf(buf, sizeof(buf), "{...}(%zu)", o ? o->size() : 0);
    return std::string(buf);
  }
  return {};
}

static bool contains_substring(const std::string& hay, const std::string& needle, bool case_sensitive) {
  if (needle.empty()) return true;
  if (hay.empty()) return false;

  if (case_sensitive) {
    return hay.find(needle) != std::string::npos;
  }
  return nebula4x::to_lower(hay).find(nebula4x::to_lower(needle)) != std::string::npos;
}

static bool node_contains_text_limited(const nebula4x::json::Value& v, const std::string& needle,
                                      bool case_sensitive, int depth_left, int& node_budget) {
  if (node_budget-- <= 0) return false;
  if (depth_left < 0) return false;

  if (v.is_null() || v.is_bool() || v.is_number() || v.is_string()) {
    return contains_substring(scalar_preview(v, 160), needle, case_sensitive);
  }

  if (const auto* a = v.as_array()) {
    const std::size_t n = std::min<std::size_t>(a->size(), 32);
    for (std::size_t i = 0; i < n; ++i) {
      if (node_contains_text_limited((*a)[i], needle, case_sensitive, depth_left - 1, node_budget)) return true;
    }
  } else if (const auto* o = v.as_object()) {
    std::size_t seen = 0;
    for (const auto& kv : *o) {
      if (contains_substring(kv.first, needle, case_sensitive)) return true;
      if (node_contains_text_limited(kv.second, needle, case_sensitive, depth_left - 1, node_budget)) return true;
      if (++seen >= 48) break;
    }
  }
  return false;
}

static bool value_to_number(const nebula4x::json::Value* v, double* out) {
  if (!out) return false;
  if (!v) return false;

  if (v->is_number()) {
    *out = v->number_value();
    return true;
  }
  if (v->is_bool()) {
    *out = v->bool_value() ? 1.0 : 0.0;
    return true;
  }
  if (v->is_array()) {
    const auto* a = v->as_array();
    *out = a ? (double)a->size() : 0.0;
    return true;
  }
  if (v->is_object()) {
    const auto* o = v->as_object();
    *out = o ? (double)o->size() : 0.0;
    return true;
  }
  return false;
}

static std::string value_to_key_string(const nebula4x::json::Value* v, int max_len = 128) {
  if (!v) return "(missing)";

  if (v->is_string()) {
    std::string s = v->string_value();
    if ((int)s.size() > max_len) {
      s.resize((std::size_t)max_len);
      s += "…";
    }
    if (s.empty()) return "(empty)";
    return s;
  }
  if (v->is_number() || v->is_bool() || v->is_null()) {
    return scalar_preview(*v, max_len);
  }

  // Heuristic: if the group value is an object, try to use common identity-like keys.
  if (v->is_object()) {
    const auto* o = v->as_object();
    if (o) {
      if (auto it = o->find("name"); it != o->end() && it->second.is_string()) {
        std::string s = it->second.string_value();
        if ((int)s.size() > max_len) {
          s.resize((std::size_t)max_len);
          s += "…";
        }
        if (!s.empty()) return s;
      }
      if (auto it = o->find("id"); it != o->end()) {
        return std::string("id=") + scalar_preview(it->second, max_len);
      }
    }
    return scalar_preview(*v, max_len);
  }

  // Array: treat size as a key.
  if (v->is_array()) {
    return scalar_preview(*v, max_len);
  }

  return "(unknown)";
}

[[maybe_unused]] static JsonTableViewConfig* find_table_view(UIState& ui, std::uint64_t id) {
  for (auto& v : ui.json_table_views) {
    if (v.id == id) return &v;
  }
  return nullptr;
}

[[maybe_unused]] static const JsonTableViewConfig* find_table_view(const UIState& ui, std::uint64_t id) {
  for (const auto& v : ui.json_table_views) {
    if (v.id == id) return &v;
  }
  return nullptr;
}

struct GroupAgg {
  std::uint64_t count{0};

  // For value aggregations.
  std::uint64_t value_n{0};
  double value_sum{0.0};
  double value_min{0.0};
  double value_max{0.0};
  bool has_minmax{false};

  std::size_t example_row_index{0};
  bool has_example{false};
};

struct GroupRow {
  std::string key;
  std::uint64_t count{0};
  double value{0.0};
  bool has_value{false};

  std::size_t example_row_index{0};
  bool has_example{false};
};

struct PivotRuntime {
  std::uint64_t built_doc_revision{0};
  std::string built_cache_key;

  bool building{false};
  std::size_t next_row{0};
  std::size_t scan_cap{0};

  std::unordered_map<std::string, GroupAgg> groups;
  std::vector<GroupRow> rows;

  std::uint64_t total_count{0};
  double total_value_for_pct{0.0};

  std::string group_filter;
  std::string visible_cache_key;
  std::vector<int> visible;
};

struct PivotTablesState {
  bool initialized{false};

  bool auto_refresh{true};
  float refresh_sec{1.0f};
  double last_refresh_time{0.0};

  std::uint64_t doc_revision{0};
  bool doc_loaded{false};
  std::string doc_error;
  std::shared_ptr<const nebula4x::json::Value> root;

  std::uint64_t selected_pivot_id{0};

  // Add pivot helpers.
  char add_name[64] = {0};
  int add_view_idx{0};

  char add_path[256] = {0};

  std::unordered_map<std::uint64_t, PivotRuntime> runtimes;
};

static void refresh_doc(PivotTablesState& st, Simulation& sim, bool force) {
  const double now = ImGui::GetTime();
  ensure_game_json_cache(sim, now, st.refresh_sec, force);
  const auto& cache = game_json_cache();
  st.doc_revision = cache.revision;
  st.root = cache.root;
  st.doc_loaded = (bool)st.root;
  st.doc_error = cache.error;
}

static std::string make_cache_key(const JsonPivotConfig& p, const JsonTableViewConfig* view) {
  std::string k;
  k.reserve(256);

  k += "tv=" + std::to_string(static_cast<unsigned long long>(p.table_view_id));
  k += "|gr=" + p.group_by_rel_path;
  k += "|sr=" + std::to_string(p.scan_rows);
  k += "|rpf=" + std::to_string(p.rows_per_frame);
  k += "|lnk=" + std::string(p.link_to_lens_filter ? "1" : "0");
  k += "|all=" + std::string(p.use_all_lens_columns ? "1" : "0");
  k += "|ve=" + std::string(p.value_enabled ? "1" : "0");
  k += "|vp=" + p.value_rel_path;
  k += "|vo=" + std::to_string(p.value_op);
  k += "|top=" + std::to_string(p.top_groups);

  if (view && p.link_to_lens_filter) {
    k += "|f=" + view->filter;
    k += "|fcs=" + std::string(view->filter_case_sensitive ? "1" : "0");
    k += "|fall=" + std::string(view->filter_all_fields ? "1" : "0");

    // If filtering uses columns, include the enabled column set in the cache key.
    if (!view->filter_all_fields) {
      for (const auto& c : view->columns) {
        if (!p.use_all_lens_columns && !c.enabled) continue;
        k += "|c:" + c.rel_path;
      }
    }
  }

  return k;
}

static bool row_passes_lens_filter(const JsonPivotConfig& p, const JsonTableViewConfig& view,
                                  const nebula4x::json::Value& row) {
  if (!p.link_to_lens_filter) return true;
  if (view.filter.empty()) return true;

  const bool cs = view.filter_case_sensitive;

  if (view.filter_all_fields) {
    int budget = 2500;
    return node_contains_text_limited(row, view.filter, cs, /*depth_left=*/4, budget);
  }

  // Column-scoped filtering.
  for (const auto& col : view.columns) {
    if (!p.use_all_lens_columns && !col.enabled) continue;
    std::string err;
    const auto* v = nebula4x::resolve_json_pointer(row, col.rel_path, /*accept_root_slash=*/true, &err);
    if (!v) continue;
    if (contains_substring(scalar_preview(*v, 160), view.filter, cs)) return true;
  }

  return false;
}

static void begin_pivot_build(PivotRuntime& rt, const JsonPivotConfig& p,
                             const JsonTableViewConfig& view, const nebula4x::json::Value& root) {
  rt.groups.clear();
  rt.rows.clear();
  rt.visible.clear();
  rt.visible_cache_key.clear();
  rt.total_count = 0;
  rt.total_value_for_pct = 0.0;

  rt.building = true;
  rt.next_row = 0;

  // Resolve the dataset array.
  std::string err;
  const auto* node = nebula4x::resolve_json_pointer(root, view.array_path, /*accept_root_slash=*/true, &err);
  const auto* arr = node ? node->as_array() : nullptr;

  if (!arr) {
    rt.building = false;
    rt.scan_cap = 0;
    return;
  }

  const std::size_t cap = (std::size_t)std::max(0, p.scan_rows);
  rt.scan_cap = std::min<std::size_t>(arr->size(), cap > 0 ? cap : arr->size());
}

static void finalize_pivot_build(PivotRuntime& rt, const JsonPivotConfig& p) {
  rt.rows.clear();
  rt.rows.reserve(rt.groups.size());

  // Compute the per-row value based on the selected op.
  for (const auto& kv : rt.groups) {
    const std::string& key = kv.first;
    const GroupAgg& g = kv.second;

    GroupRow r;
    r.key = key;
    r.count = g.count;
    r.example_row_index = g.example_row_index;
    r.has_example = g.has_example;

    if (p.value_enabled) {
      r.has_value = (g.value_n > 0) || g.has_minmax;
      switch (p.value_op) {
        case 1: // avg
          r.value = (g.value_n > 0) ? (g.value_sum / (double)g.value_n) : 0.0;
          break;
        case 2: // min
          r.value = g.has_minmax ? g.value_min : 0.0;
          break;
        case 3: // max
          r.value = g.has_minmax ? g.value_max : 0.0;
          break;
        case 0: // sum
        default:
          r.value = g.value_sum;
          break;
      }
    }

    rt.rows.push_back(std::move(r));
  }

  // Totals for percent columns.
  rt.total_count = 0;
  rt.total_value_for_pct = 0.0;
  for (const auto& r : rt.rows) {
    rt.total_count += r.count;
    if (p.value_enabled && p.value_op == 0) {
      rt.total_value_for_pct += r.value;
    }
  }

  rt.building = false;
}

static void step_pivot_build(PivotRuntime& rt, const JsonPivotConfig& p,
                            const JsonTableViewConfig& view, const nebula4x::json::Value& root) {
  if (!rt.building) return;

  std::string err;
  const auto* node = nebula4x::resolve_json_pointer(root, view.array_path, /*accept_root_slash=*/true, &err);
  const auto* arr = node ? node->as_array() : nullptr;
  if (!arr) {
    rt.building = false;
    return;
  }

  const int rows_pf = std::clamp(p.rows_per_frame, 10, 20000);
  const std::size_t end = std::min<std::size_t>(rt.scan_cap, rt.next_row + (std::size_t)rows_pf);

  const std::string group_path = p.group_by_rel_path.empty() ? "/" : p.group_by_rel_path;
  const std::string value_path = p.value_rel_path;

  for (std::size_t i = rt.next_row; i < end; ++i) {
    const auto& row = (*arr)[i];

    if (!row_passes_lens_filter(p, view, row)) {
      continue;
    }

    std::string gerr;
    const auto* gv = nebula4x::resolve_json_pointer(row, group_path, /*accept_root_slash=*/true, &gerr);
    const std::string gkey = value_to_key_string(gv);

    GroupAgg& g = rt.groups[gkey];
    g.count++;
    if (!g.has_example) {
      g.example_row_index = i;
      g.has_example = true;
    }

    if (p.value_enabled && !value_path.empty()) {
      std::string verr;
      const auto* vv = nebula4x::resolve_json_pointer(row, value_path, /*accept_root_slash=*/true, &verr);
      double x = 0.0;
      if (value_to_number(vv, &x)) {
        g.value_n++;
        g.value_sum += x;
        if (!g.has_minmax) {
          g.value_min = g.value_max = x;
          g.has_minmax = true;
        } else {
          g.value_min = std::min(g.value_min, x);
          g.value_max = std::max(g.value_max, x);
        }
      }
    }
  }

  rt.next_row = end;

  if (rt.next_row >= rt.scan_cap) {
    finalize_pivot_build(rt, p);
  }
}

static const char* value_op_label(int op) {
  switch (op) {
    case 1: return "Avg";
    case 2: return "Min";
    case 3: return "Max";
    case 0:
    default: return "Sum";
  }
}

static std::string display_for_column_choice(const JsonTableViewConfig& view, const std::string& rel_path) {
  if (rel_path.empty()) return "(none)";
  for (const auto& c : view.columns) {
    if (normalize_json_pointer_copy(c.rel_path) == normalize_json_pointer_copy(rel_path)) {
      if (!c.label.empty()) return c.label + "  (" + c.rel_path + ")";
      return c.rel_path;
    }
  }
  return rel_path;
}

static void ensure_visible_rows(PivotRuntime& rt, const JsonPivotConfig& p) {
  const std::string key = rt.group_filter + "|top=" + std::to_string(p.top_groups) + "|rows=" +
                          std::to_string(static_cast<unsigned long long>(rt.rows.size()));
  if (rt.visible_cache_key == key) return;

  rt.visible_cache_key = key;
  rt.visible.clear();
  rt.visible.reserve(rt.rows.size());

  const bool has_filter = !rt.group_filter.empty();
  const std::string needle = has_filter ? rt.group_filter : std::string();

  for (int i = 0; i < (int)rt.rows.size(); ++i) {
    if (!has_filter || contains_substring(rt.rows[(std::size_t)i].key, needle, /*case_sensitive=*/false)) {
      rt.visible.push_back(i);
    }
  }

  // Apply top_groups after filtering.
  if (p.top_groups > 0 && (int)rt.visible.size() > p.top_groups) {
    rt.visible.resize((std::size_t)p.top_groups);
  }
}

} // namespace

bool add_json_pivot_for_table_view(UIState& ui, std::uint64_t table_view_id, const std::string& suggested_name) {
  if (table_view_id == 0) return false;

  JsonPivotConfig cfg;
  cfg.id = ui.next_json_pivot_id++;
  if (cfg.id == 0) cfg.id = ui.next_json_pivot_id++;

  cfg.table_view_id = table_view_id;

  const auto* view = find_table_view(ui, table_view_id);

  if (!suggested_name.empty()) {
    cfg.name = suggested_name;
  } else if (view && !view->name.empty()) {
    cfg.name = view->name + " Pivot";
  } else {
    cfg.name = "Pivot";
  }

  // Heuristic defaults: group-by tries to use a name-like column.
  if (view) {
    auto pick_group = [&]() -> std::string {
      for (const auto& c : view->columns) {
        if (!c.enabled) continue;
        const std::string lbl = nebula4x::to_lower(c.label);
        const std::string rp = nebula4x::to_lower(c.rel_path);
        if (lbl.find("name") != std::string::npos || rp.find("/name") != std::string::npos) {
          return normalize_json_pointer_copy(c.rel_path);
        }
      }
      for (const auto& c : view->columns) {
        if (!c.enabled) continue;
        return normalize_json_pointer_copy(c.rel_path);
      }
      return std::string("/");
    };

    cfg.group_by_rel_path = pick_group();

    // Optional value column: pick the first enabled column not used as group.
    for (const auto& c : view->columns) {
      if (!c.enabled) continue;
      const std::string rp = normalize_json_pointer_copy(c.rel_path);
      if (rp == cfg.group_by_rel_path) continue;
      cfg.value_rel_path = rp;
      cfg.value_enabled = false; // start off; user can enable.
      break;
    }
  }

  if (cfg.group_by_rel_path.empty()) cfg.group_by_rel_path = "/";
  if (!cfg.group_by_rel_path.empty() && cfg.group_by_rel_path[0] != '/') cfg.group_by_rel_path = "/" + cfg.group_by_rel_path;
  if (!cfg.value_rel_path.empty() && cfg.value_rel_path[0] != '/') cfg.value_rel_path = "/" + cfg.value_rel_path;

  ui.json_pivots.push_back(std::move(cfg));
  ui.request_select_json_pivot_id = ui.json_pivots.back().id;
  return true;
}

bool add_json_pivot_for_path(UIState& ui, const std::string& array_path, const std::string& suggested_name) {
  const std::string norm = normalize_json_pointer_copy(array_path);

  // Reuse an existing lens for the same path if possible.
  std::uint64_t view_id = 0;
  for (const auto& v : ui.json_table_views) {
    if (normalize_json_pointer_copy(v.array_path) == norm) {
      view_id = v.id;
      break;
    }
  }

  if (view_id == 0) {
    // Create a new lens.
    const std::string lens_name = suggested_name.empty() ? (last_pointer_token(norm).empty() ? "Lens" : last_pointer_token(norm))
                                                         : suggested_name;
    if (!add_json_table_view(ui, norm, lens_name)) {
      return false;
    }
    if (!ui.json_table_views.empty()) {
      view_id = ui.json_table_views.back().id;
    }
  }

  const std::string pivot_name = suggested_name.empty()
                                    ? (last_pointer_token(norm).empty() ? "Pivot" : (last_pointer_token(norm) + " Pivot"))
                                    : (suggested_name + " Pivot");
  return add_json_pivot_for_table_view(ui, view_id, pivot_name);
}

void draw_pivot_tables_window(Simulation& sim, UIState& ui) {
  if (!ui.show_pivot_tables_window) return;

  static PivotTablesState st;
  const double now = ImGui::GetTime();

  if (!st.initialized) {
    st.initialized = true;
    st.auto_refresh = true;
    st.refresh_sec = 1.0f;
    st.last_refresh_time = 0.0;
    std::snprintf(st.add_name, sizeof(st.add_name), "%s", "Pivot");
    std::snprintf(st.add_path, sizeof(st.add_path), "%s", "/");
    st.add_view_idx = 0;
    refresh_doc(st, sim, /*force=*/true);
  }

  // Consume selection request.
  if (ui.request_select_json_pivot_id != 0) {
    st.selected_pivot_id = ui.request_select_json_pivot_id;
    ui.request_select_json_pivot_id = 0;
  }

  // Default selection.
  if (st.selected_pivot_id == 0 && !ui.json_pivots.empty()) {
    st.selected_pivot_id = ui.json_pivots.front().id;
  }

  // Auto-refresh the document.
  if (st.auto_refresh) {
    if ((now - st.last_refresh_time) >= st.refresh_sec) {
      st.last_refresh_time = now;
      refresh_doc(st, sim, /*force=*/false);
    }
  }

  ImGui::SetNextWindowSize(ImVec2(1040, 740), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Pivot Tables", &ui.show_pivot_tables_window)) {
    ImGui::End();
    return;
  }

  // Top bar.
  {
    if (ImGui::Button("Refresh##piv")) {
      refresh_doc(st, sim, /*force=*/true);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto##piv", &st.auto_refresh);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::SliderFloat("##piv_refresh_sec", &st.refresh_sec, 0.10f, 5.0f, "%.2fs");
    st.refresh_sec = std::clamp(st.refresh_sec, 0.05f, 60.0f);

    ImGui::SameLine();
    ImGui::TextDisabled("Doc rev: %llu", (unsigned long long)st.doc_revision);

    if (!st.doc_error.empty()) {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "Parse error: %s", st.doc_error.c_str());
    }
  }

  ImGui::Separator();

  const float left_w = 300.0f;
  ImGui::BeginChild("##piv_left", ImVec2(left_w, 0), true);
  {
    ImGui::Text("Pivots");
    ImGui::Separator();

    if (ui.json_pivots.empty()) {
      ImGui::TextDisabled("No pivots yet.");
      ImGui::TextDisabled("Create one below or from Data Lenses / JSON Explorer.");
    }

    for (auto& p : ui.json_pivots) {
      const bool sel = (p.id == st.selected_pivot_id);
      if (ImGui::Selectable(p.name.c_str(), sel)) {
        st.selected_pivot_id = p.id;
      }
      if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Duplicate")) {
          JsonPivotConfig copy = p;
          copy.id = ui.next_json_pivot_id++;
          if (copy.id == 0) copy.id = ui.next_json_pivot_id++;
          copy.name = p.name + " (copy)";
          ui.json_pivots.push_back(std::move(copy));
          st.selected_pivot_id = ui.json_pivots.back().id;
        }
        if (ImGui::MenuItem("Delete")) {
          const std::uint64_t del_id = p.id;
          ui.json_pivots.erase(
              std::remove_if(ui.json_pivots.begin(), ui.json_pivots.end(), [&](const JsonPivotConfig& x) { return x.id == del_id; }),
              ui.json_pivots.end());
          st.runtimes.erase(del_id);
          if (st.selected_pivot_id == del_id) {
            st.selected_pivot_id = ui.json_pivots.empty() ? 0 : ui.json_pivots.front().id;
          }
          ImGui::EndPopup();
          break;
        }
        ImGui::EndPopup();
      }
    }

    ImGui::Separator();
    ImGui::Text("New Pivot (from Data Lens)");

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##piv_add_name", "Name", st.add_name, IM_ARRAYSIZE(st.add_name));

    if (!ui.json_table_views.empty()) {
      std::vector<const char*> lens_names;
      lens_names.reserve(ui.json_table_views.size());
      for (const auto& v : ui.json_table_views) lens_names.push_back(v.name.c_str());

      st.add_view_idx = std::clamp(st.add_view_idx, 0, (int)lens_names.size() - 1);
      ImGui::SetNextItemWidth(-1);
      ImGui::Combo("##piv_add_lens", &st.add_view_idx, lens_names.data(), (int)lens_names.size());

      if (ImGui::Button("Add Pivot##from_lens")) {
        const std::uint64_t view_id = ui.json_table_views[(std::size_t)st.add_view_idx].id;
        (void)add_json_pivot_for_table_view(ui, view_id, st.add_name);
        ui.show_pivot_tables_window = true;
      }
    } else {
      ImGui::TextDisabled("(no Data Lenses yet)");
    }

    ImGui::Separator();
    ImGui::Text("New Pivot (from JSON array path)");

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##piv_add_path", "/ships", st.add_path, IM_ARRAYSIZE(st.add_path));

    // Autocomplete the dataset pointer.
    if (st.root) {
      const std::vector<std::string> sugg = nebula4x::suggest_json_pointer_completions(
          *st.root, st.add_path, 10, /*accept_root_slash=*/true, /*case_sensitive=*/false);
      if (!sugg.empty() && ImGui::BeginListBox("##piv_add_path_sugg", ImVec2(-1, 90))) {
        for (const auto& s : sugg) {
          if (ImGui::Selectable(s.c_str(), false)) {
            std::snprintf(st.add_path, sizeof(st.add_path), "%s", s.c_str());
          }
        }
        ImGui::EndListBox();
      }
    }

    if (ImGui::Button("Add Pivot##from_path")) {
      (void)add_json_pivot_for_path(ui, st.add_path, st.add_name);
      ui.show_pivot_tables_window = true;
    }
  }
  ImGui::EndChild();

  ImGui::SameLine();

  ImGui::BeginChild("##piv_right", ImVec2(0, 0), false);
  {
    JsonPivotConfig* sel = nullptr;
    for (auto& p : ui.json_pivots) {
      if (p.id == st.selected_pivot_id) {
        sel = &p;
        break;
      }
    }

    if (!sel) {
      ImGui::TextDisabled("Select a pivot on the left.");
      ImGui::EndChild();
      ImGui::End();
      return;
    }

    // Validate lens.
    JsonTableViewConfig* view = find_table_view(ui, sel->table_view_id);
    if (!view) {
      ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "Pivot references missing Data Lens id=%llu", (unsigned long long)sel->table_view_id);
      ImGui::TextDisabled("Fix: select a new Data Lens in the pivot config.");
    }

    // --- Config panel ---
    ImGui::Text("Config");
    ImGui::Separator();

    ImGui::SetNextItemWidth(360.0f);
    {
      char buf[128];
      std::snprintf(buf, sizeof(buf), "%s", sel->name.c_str());
      if (ImGui::InputText("Name##piv", buf, IM_ARRAYSIZE(buf))) {
        sel->name = buf;
      }
    }

    // Lens selection.
    if (!ui.json_table_views.empty()) {
      int cur_idx = 0;
      for (int i = 0; i < (int)ui.json_table_views.size(); ++i) {
        if (ui.json_table_views[(std::size_t)i].id == sel->table_view_id) {
          cur_idx = i;
          break;
        }
      }

      std::vector<const char*> lens_names;
      lens_names.reserve(ui.json_table_views.size());
      for (const auto& v : ui.json_table_views) lens_names.push_back(v.name.c_str());

      ImGui::SetNextItemWidth(360.0f);
      if (ImGui::Combo("Data Lens##piv", &cur_idx, lens_names.data(), (int)lens_names.size())) {
        sel->table_view_id = ui.json_table_views[(std::size_t)cur_idx].id;
        view = find_table_view(ui, sel->table_view_id);
      }
    }

    // Link/filter knobs.
    ImGui::Checkbox("Link to lens filter##piv", &sel->link_to_lens_filter);
    ImGui::SameLine();
    ImGui::Checkbox("Use all columns##piv", &sel->use_all_lens_columns);

    sel->scan_rows = std::clamp(sel->scan_rows, 10, 500000);
    sel->rows_per_frame = std::clamp(sel->rows_per_frame, 10, 20000);

    ImGui::SetNextItemWidth(150);
    ImGui::InputInt("Scan rows##piv", &sel->scan_rows, 100, 1000);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    ImGui::InputInt("Rows/frame##piv", &sel->rows_per_frame, 50, 250);

    ImGui::SetNextItemWidth(150);
    ImGui::InputInt("Top groups##piv", &sel->top_groups, 1, 5);
    sel->top_groups = std::clamp(sel->top_groups, 0, 50000);

    // Group-by selection.
    {
      if (sel->group_by_rel_path.empty()) sel->group_by_rel_path = "/";
      if (sel->group_by_rel_path[0] != '/') sel->group_by_rel_path = "/" + sel->group_by_rel_path;

      if (view) {
        const std::string cur_disp = display_for_column_choice(*view, sel->group_by_rel_path);
        ImGui::SetNextItemWidth(520.0f);
        if (ImGui::BeginCombo("Group by##piv", cur_disp.c_str())) {
          if (ImGui::Selectable("(row)  (/)##piv_group")) {
            sel->group_by_rel_path = "/";
          }
          for (const auto& c : view->columns) {
            if (!sel->use_all_lens_columns && !c.enabled) continue;
            const std::string rp = normalize_json_pointer_copy(c.rel_path);
            const std::string label = (c.label.empty() ? rp : (c.label + "  (" + rp + ")"));
            const bool is_sel = (normalize_json_pointer_copy(sel->group_by_rel_path) == rp);
            if (ImGui::Selectable(label.c_str(), is_sel)) {
              sel->group_by_rel_path = rp;
            }
          }
          ImGui::EndCombo();
        }
      }

      char gbuf[256];
      std::snprintf(gbuf, sizeof(gbuf), "%s", sel->group_by_rel_path.c_str());
      ImGui::SetNextItemWidth(520.0f);
      if (ImGui::InputTextWithHint("Group path (rel)##piv", "/name", gbuf, IM_ARRAYSIZE(gbuf))) {
        sel->group_by_rel_path = normalize_json_pointer_copy(gbuf);
      }
    }

    // Value aggregation.
    {
      ImGui::Separator();
      ImGui::Checkbox("Aggregate value column##piv", &sel->value_enabled);

      if (sel->value_enabled) {
        if (view) {
          const std::string cur_disp = display_for_column_choice(*view, sel->value_rel_path);
          ImGui::SetNextItemWidth(520.0f);
          if (ImGui::BeginCombo("Value column##piv", cur_disp.c_str())) {
            if (ImGui::Selectable("(none)##piv_val_none", sel->value_rel_path.empty())) {
              sel->value_rel_path.clear();
            }
            for (const auto& c : view->columns) {
              if (!sel->use_all_lens_columns && !c.enabled) continue;
              const std::string rp = normalize_json_pointer_copy(c.rel_path);
              const std::string label = (c.label.empty() ? rp : (c.label + "  (" + rp + ")"));
              const bool is_sel = (normalize_json_pointer_copy(sel->value_rel_path) == rp);
              if (ImGui::Selectable(label.c_str(), is_sel)) {
                sel->value_rel_path = rp;
              }
            }
            ImGui::EndCombo();
          }
        }

        char vbuf[256];
        std::snprintf(vbuf, sizeof(vbuf), "%s", sel->value_rel_path.c_str());
        ImGui::SetNextItemWidth(520.0f);
        if (ImGui::InputTextWithHint("Value path (rel)##piv", "/fuel_tons", vbuf, IM_ARRAYSIZE(vbuf))) {
          sel->value_rel_path = normalize_json_pointer_copy(vbuf);
        }

        const char* ops[] = {"Sum", "Avg", "Min", "Max"};
        int op = std::clamp(sel->value_op, 0, 3);
        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::Combo("Op##piv", &op, ops, IM_ARRAYSIZE(ops))) {
          sel->value_op = op;
        }
      }
    }

    // Build / results.
    ImGui::Separator();

    if (!st.doc_loaded) {
      ImGui::TextDisabled("Document not loaded; pivots unavailable.");
      ImGui::EndChild();
      ImGui::End();
      return;
    }

    if (!view) {
      ImGui::TextDisabled("Select a Data Lens to build the pivot.");
      ImGui::EndChild();
      ImGui::End();
      return;
    }

    PivotRuntime& rt = st.runtimes[sel->id];

    const std::string cache_key = make_cache_key(*sel, view);
    if (rt.built_doc_revision != st.doc_revision || rt.built_cache_key != cache_key) {
      rt.built_doc_revision = st.doc_revision;
      rt.built_cache_key = cache_key;
      begin_pivot_build(rt, *sel, *view, *st.root);
    }

    if (rt.building) {
      step_pivot_build(rt, *sel, *view, *st.root);
    }

    // Rebuild button.
    if (ImGui::Button("Rebuild now##piv")) {
      rt.built_doc_revision = 0;
      rt.built_cache_key.clear();
      begin_pivot_build(rt, *sel, *view, *st.root);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Groups: %zu", rt.rows.size());

    // Status / progress.
    if (rt.building) {
      const float frac = (rt.scan_cap > 0) ? (float)((double)rt.next_row / (double)rt.scan_cap) : 0.0f;
      ImGui::ProgressBar(frac, ImVec2(0, 0), "Building...");
      ImGui::TextDisabled("Processed %zu / %zu", rt.next_row, rt.scan_cap);
    }

    ImGui::Separator();

    // Group filter.
    {
      char fbuf[128];
      std::snprintf(fbuf, sizeof(fbuf), "%s", rt.group_filter.c_str());
      ImGui::SetNextItemWidth(320.0f);
      if (ImGui::InputTextWithHint("Filter groups##piv", "type to filter", fbuf, IM_ARRAYSIZE(fbuf))) {
        rt.group_filter = fbuf;
        rt.visible_cache_key.clear();
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Clear##piv_filter")) {
        rt.group_filter.clear();
        rt.visible_cache_key.clear();
      }

      ImGui::SameLine();
      if (ImGui::SmallButton("Copy CSV##piv")) {
        std::string csv;
        csv.reserve(rt.rows.size() * 48);
        csv += "group,count";
        if (sel->value_enabled) {
          csv += ",";
          csv += value_op_label(sel->value_op);
        }
        csv += "\n";
        for (const auto& r : rt.rows) {
          csv += '"';
          // Basic CSV escaping for quotes.
          for (char ch : r.key) {
            if (ch == '"') csv += '"';
            csv += ch;
          }
          csv += '"';
          csv += "," + std::to_string((unsigned long long)r.count);
          if (sel->value_enabled) {
            char nbuf[64];
            std::snprintf(nbuf, sizeof(nbuf), "%.6g", r.value);
            csv += ",";
            csv += nbuf;
          }
          csv += "\n";
        }
        ImGui::SetClipboardText(csv.c_str());
      }
    }

    // Results table.
    {
      const bool show_value = sel->value_enabled;
      const bool show_value_pct = show_value && sel->value_op == 0 && rt.total_value_for_pct > 0.0;

      ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_Sortable | ImGuiTableFlags_Resizable;

      int cols = 3; // group, count, %count
      if (show_value) cols += 1;
      if (show_value_pct) cols += 1;

      if (ImGui::BeginTable("##piv_table", cols, flags, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        int col_idx = 0;
        ImGui::TableSetupColumn("Group", ImGuiTableColumnFlags_WidthStretch, 0.0f, col_idx++);
        ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 90.0f, col_idx++);
        if (show_value) {
          std::string vcol = std::string(value_op_label(sel->value_op)) + " value";
          ImGui::TableSetupColumn(vcol.c_str(), ImGuiTableColumnFlags_WidthFixed, 120.0f, col_idx++);
        }
        ImGui::TableSetupColumn("%Count", ImGuiTableColumnFlags_WidthFixed, 90.0f, col_idx++);
        if (show_value_pct) {
          ImGui::TableSetupColumn("%Value", ImGuiTableColumnFlags_WidthFixed, 90.0f, col_idx++);
        }

        ImGui::TableHeadersRow();

        // Sort if requested.
        if (ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs()) {
          if (sort->SpecsDirty && sort->SpecsCount > 0) {
            const ImGuiTableColumnSortSpecs& s = sort->Specs[0];
            const int sort_col = (int)s.ColumnUserID;
            const bool desc = (s.SortDirection == ImGuiSortDirection_Descending);

            auto key_cmp = [&](const GroupRow& a, const GroupRow& b) {
              if (a.key == b.key) return a.count < b.count;
              return a.key < b.key;
            };

            auto cmp = [&](const GroupRow& a, const GroupRow& b) {
              if (sort_col == 1) {
                if (a.count == b.count) return key_cmp(a, b);
                return desc ? (a.count > b.count) : (a.count < b.count);
              }
              // Value column index depends on whether show_value is enabled.
              const int value_col_user_id = show_value ? 2 : -1;
              const int pct_count_col_user_id = show_value ? 3 : 2;
              const int pct_value_col_user_id = show_value_pct ? (pct_count_col_user_id + 1) : -1;

              if (show_value && sort_col == value_col_user_id) {
                if (a.value == b.value) return key_cmp(a, b);
                return desc ? (a.value > b.value) : (a.value < b.value);
              }

              if (sort_col == pct_count_col_user_id) {
                const double pa = (rt.total_count > 0) ? ((double)a.count / (double)rt.total_count) : 0.0;
                const double pb = (rt.total_count > 0) ? ((double)b.count / (double)rt.total_count) : 0.0;
                if (pa == pb) return key_cmp(a, b);
                return desc ? (pa > pb) : (pa < pb);
              }

              if (show_value_pct && sort_col == pct_value_col_user_id) {
                const double pa = (rt.total_value_for_pct > 0.0) ? (a.value / rt.total_value_for_pct) : 0.0;
                const double pb = (rt.total_value_for_pct > 0.0) ? (b.value / rt.total_value_for_pct) : 0.0;
                if (pa == pb) return key_cmp(a, b);
                return desc ? (pa > pb) : (pa < pb);
              }

              // Default: sort by group string.
              if (a.key == b.key) return key_cmp(a, b);
              return desc ? (a.key > b.key) : (a.key < b.key);
            };

            std::stable_sort(rt.rows.begin(), rt.rows.end(), cmp);

            // Filter cache invalid because row order changed.
            rt.visible_cache_key.clear();

            sort->SpecsDirty = false;
          }
        }

        ensure_visible_rows(rt, *sel);

        ImGuiListClipper clip;
        clip.Begin((int)rt.visible.size());
        while (clip.Step()) {
          for (int vi = clip.DisplayStart; vi < clip.DisplayEnd; ++vi) {
            const int row_idx = rt.visible[(std::size_t)vi];
            if (row_idx < 0 || row_idx >= (int)rt.rows.size()) continue;

            const GroupRow& r = rt.rows[(std::size_t)row_idx];

            ImGui::TableNextRow();
            int ccol = 0;

            // Group.
            ImGui::TableSetColumnIndex(ccol++);
            ImGui::TextUnformatted(r.key.c_str());
            if (ImGui::BeginPopupContextItem("##piv_row_ctx")) {
              if (ImGui::MenuItem("Drill down in Data Lenses")) {
                ui.show_data_lenses_window = true;
                ui.request_select_json_table_view_id = sel->table_view_id;

                // Best-effort: set the lens filter to the group key.
                if (auto* v = find_table_view(ui, sel->table_view_id)) {
                  v->filter = r.key;
                  v->filter_all_fields = true;
                }
              }

              if (r.has_example) {
                if (ImGui::MenuItem("Go to example row in JSON Explorer")) {
                  ui.show_json_explorer_window = true;
                  ui.request_json_explorer_goto_path = nebula4x::json_pointer_join_index(normalize_json_pointer_copy(view->array_path), r.example_row_index);
                }
              }
              if (ImGui::MenuItem("Copy group")) {
                ImGui::SetClipboardText(r.key.c_str());
              }
              ImGui::EndPopup();
            }

            // Count.
            ImGui::TableSetColumnIndex(ccol++);
            ImGui::Text("%llu", (unsigned long long)r.count);

            // Value.
            if (show_value) {
              ImGui::TableSetColumnIndex(ccol++);
              char nbuf[64];
              std::snprintf(nbuf, sizeof(nbuf), "%.6g", r.value);
              ImGui::TextUnformatted(nbuf);
            }

            // %count.
            ImGui::TableSetColumnIndex(ccol++);
            const double pc = (rt.total_count > 0) ? (100.0 * (double)r.count / (double)rt.total_count) : 0.0;
            char pbuf[32];
            std::snprintf(pbuf, sizeof(pbuf), "%.2f%%", pc);
            ImGui::TextUnformatted(pbuf);

            // %value.
            if (show_value_pct) {
              ImGui::TableSetColumnIndex(ccol++);
              const double pv = (rt.total_value_for_pct > 0.0) ? (100.0 * (double)r.value / rt.total_value_for_pct) : 0.0;
              char pvbuf[32];
              std::snprintf(pvbuf, sizeof(pvbuf), "%.2f%%", pv);
              ImGui::TextUnformatted(pvbuf);
            }
          }
        }

        ImGui::EndTable();
      }
    }
  }
  ImGui::EndChild();

  ImGui::End();
}

} // namespace nebula4x::ui
