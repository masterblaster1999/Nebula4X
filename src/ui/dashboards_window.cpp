#include "ui/dashboards_window.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <queue>
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

static std::string scalar_preview(const nebula4x::json::Value& v, int max_len = 64) {
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
    return contains_substring(scalar_preview(v, 128), needle, case_sensitive);
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

struct ColumnProbe {
  std::string label;
  std::string rel_path;

  int numeric_count{0};
  int string_count{0};
  int bool_count{0};
  int null_count{0};

  double min{0.0};
  double max{0.0};
  double sum{0.0};
  bool has_minmax{false};

  std::vector<float> values;

  std::unordered_map<std::string, int> freq;
  bool freq_truncated{false};
  int other_count{0};

  std::vector<std::pair<double, std::size_t>> top; // (value, row_index)
};

static void update_top_k(std::vector<std::pair<double, std::size_t>>& top, double v, std::size_t idx, int k) {
  if (k <= 0) return;
  if ((int)top.size() < k) {
    top.emplace_back(v, idx);
    return;
  }
  // Find current smallest.
  int min_i = 0;
  for (int i = 1; i < (int)top.size(); ++i) {
    if (top[(std::size_t)i].first < top[(std::size_t)min_i].first) min_i = i;
  }
  if (v > top[(std::size_t)min_i].first) {
    top[(std::size_t)min_i] = {v, idx};
  }
}

static void probe_add_value(ColumnProbe& p, const nebula4x::json::Value* v, std::size_t row_index,
                            int top_k, int max_distinct) {
  if (!v) {
    p.null_count++;
    return;
  }

  if (v->is_number()) {
    const double x = v->number_value();
    p.numeric_count++;
    p.sum += x;
    if (!p.has_minmax) {
      p.min = p.max = x;
      p.has_minmax = true;
    } else {
      p.min = std::min(p.min, x);
      p.max = std::max(p.max, x);
    }
    p.values.push_back((float)x);
    update_top_k(p.top, x, row_index, top_k);
    return;
  }

  if (v->is_array()) {
    const auto* a = v->as_array();
    const double x = a ? (double)a->size() : 0.0;
    p.numeric_count++;
    p.sum += x;
    if (!p.has_minmax) {
      p.min = p.max = x;
      p.has_minmax = true;
    } else {
      p.min = std::min(p.min, x);
      p.max = std::max(p.max, x);
    }
    p.values.push_back((float)x);
    update_top_k(p.top, x, row_index, top_k);
    return;
  }

  if (v->is_object()) {
    const auto* o = v->as_object();
    const double x = o ? (double)o->size() : 0.0;
    p.numeric_count++;
    p.sum += x;
    if (!p.has_minmax) {
      p.min = p.max = x;
      p.has_minmax = true;
    } else {
      p.min = std::min(p.min, x);
      p.max = std::max(p.max, x);
    }
    p.values.push_back((float)x);
    update_top_k(p.top, x, row_index, top_k);
    return;
  }

  if (v->is_bool()) {
    p.bool_count++;
    const std::string key = v->bool_value() ? "true" : "false";
    auto it = p.freq.find(key);
    if (it == p.freq.end()) {
      if ((int)p.freq.size() >= max_distinct) {
        p.freq_truncated = true;
        p.other_count++;
      } else {
        p.freq.emplace(key, 1);
      }
    } else {
      it->second++;
    }
    return;
  }

  if (v->is_string()) {
    p.string_count++;
    std::string key = v->string_value();
    if (key.size() > 96) {
      key.resize(96);
      key += "…";
    }
    auto it = p.freq.find(key);
    if (it == p.freq.end()) {
      if ((int)p.freq.size() >= max_distinct) {
        p.freq_truncated = true;
        p.other_count++;
      } else {
        p.freq.emplace(std::move(key), 1);
      }
    } else {
      it->second++;
    }
    return;
  }

  // null/unknown
  p.null_count++;
}

struct NumericWidget {
  std::string label;
  std::string rel_path;
  int count{0};
  double min{0.0};
  double max{0.0};
  double mean{0.0};
  std::vector<float> hist;
  std::vector<std::pair<double, std::size_t>> top;
};

struct CategoryWidget {
  std::string label;
  std::string rel_path;
  int count{0};
  int distinct{0};
  std::vector<std::pair<std::string, int>> top;
  bool truncated{false};
  int other_count{0};
};

struct DashboardRuntime {
  std::string cache_key;

  bool building{false};
  bool ready{false};
  std::string error;

  const nebula4x::json::Array* array{nullptr};
  std::size_t total_rows{0};
  std::size_t scan_max{0};
  std::size_t scan_i{0};
  std::size_t included_rows{0};

  // Filter config snapshot (so our cache_key meaning is stable while building).
  std::string filter;
  bool filter_case_sensitive{false};
  bool filter_all_fields{false};
  std::vector<std::string> filter_rel_paths;

  std::vector<ColumnProbe> probes;
};

struct DashboardsState {
  bool initialized{false};
  bool auto_refresh{true};
  float refresh_sec{1.0f};
  double last_refresh_time{0.0};
  std::uint64_t doc_revision{0};

  bool doc_loaded{false};
  std::shared_ptr<const nebula4x::json::Value> root;
  std::string doc_error;

  std::uint64_t selected_dashboard_id{0};

  // Add new dashboard UI.
  bool add_source_use_lens{true};
  std::uint64_t add_table_view_id{0};
  char add_name[64]{};
  char add_path[256]{"/"};

  // Discovered datasets.
  struct Discovered {
    std::string path;
    std::string label;
    std::size_t size{0};
  };
  std::vector<Discovered> discovered;

  std::unordered_map<std::uint64_t, DashboardRuntime> runtimes;
};

static void refresh_doc(DashboardsState& st, Simulation& sim, bool force) {
  const double now = ImGui::GetTime();
  ensure_game_json_cache(sim, now, st.refresh_sec, force);
  const auto& cache = game_json_cache();
  st.doc_revision = cache.revision;
  st.root = cache.root;
  st.doc_loaded = (bool)st.root;
  st.doc_error = cache.error;
}

static JsonTableViewConfig* find_table_view(UIState& ui, std::uint64_t id) {
  for (auto& v : ui.json_table_views) {
    if (v.id == id) return &v;
  }
  return nullptr;
}

static JsonDashboardConfig* find_dashboard(UIState& ui, std::uint64_t id) {
  for (auto& d : ui.json_dashboards) {
    if (d.id == id) return &d;
  }
  return nullptr;
}

static std::string build_cache_key(const DashboardsState& st, const JsonDashboardConfig& dash,
                                  const JsonTableViewConfig& view) {
  std::string key;
  key.reserve(512);

  key += std::to_string(st.doc_revision);
  key.push_back('|');
  key += std::to_string(dash.table_view_id);
  key.push_back('|');
  key += view.array_path;
  key.push_back('|');
  key += std::to_string(dash.scan_rows);
  key.push_back('|');
  key += dash.use_all_lens_columns ? "all" : "ena";
  key.push_back('|');
  key += dash.link_to_lens_filter ? "lf" : "nf";
  key.push_back('|');
  if (dash.link_to_lens_filter) {
    key += view.filter;
    key.push_back('|');
    key += view.filter_case_sensitive ? "cs" : "ci";
    key.push_back('|');
    key += view.filter_all_fields ? "allf" : "cols";
  } else {
    key += "nofilter";
  }
  key.push_back('|');

  // Columns signature.
  for (const auto& c : view.columns) {
    if (!dash.use_all_lens_columns && !c.enabled) continue;
    key += c.rel_path;
    key.push_back(':');
    key += c.enabled ? '1' : '0';
    key.push_back(';');
  }

  // Filter columns signature (enabled columns only), so changing filter search surface triggers rebuild.
  key.push_back('|');
  for (const auto& c : view.columns) {
    if (!c.enabled) continue;
    key += c.rel_path;
    key.push_back(';');
  }

  return key;
}

static void discover_datasets(const nebula4x::json::Value& root, std::vector<DashboardsState::Discovered>& out) {
  out.clear();
  struct Node {
    const nebula4x::json::Value* v{nullptr};
    std::string path;
    int depth{0};
  };

  std::queue<Node> q;
  q.push({&root, "/", 0});

  const int kMaxDepth = 5;
  const int kMaxResults = 80;

  while (!q.empty() && (int)out.size() < kMaxResults) {
    Node n = std::move(q.front());
    q.pop();
    if (!n.v) continue;
    if (n.depth > kMaxDepth) continue;

    if (const auto* a = n.v->as_array()) {
      bool any_obj = false;
      for (std::size_t i = 0; i < a->size() && i < 8; ++i) {
        if ((*a)[i].is_object()) {
          any_obj = true;
          break;
        }
      }
      if (any_obj) {
        DashboardsState::Discovered d;
        d.path = n.path;
        d.size = a->size();
        d.label = last_pointer_token(n.path);
        if (d.label.empty()) d.label = n.path;
        out.push_back(std::move(d));
      }

      // Traverse into a few elements (depth-limited) to find nested arrays.
      const std::size_t step = std::max<std::size_t>(1, a->size() / 8);
      for (std::size_t i = 0; i < a->size() && i < 64; i += step) {
        std::string child_path = nebula4x::json_pointer_join_index(n.path, i);
        q.push({&(*a)[i], std::move(child_path), n.depth + 1});
      }
    } else if (const auto* o = n.v->as_object()) {
      std::size_t seen = 0;
      for (const auto& kv : *o) {
        std::string child_path = nebula4x::json_pointer_join(n.path, kv.first);
        q.push({&kv.second, std::move(child_path), n.depth + 1});
        if (++seen >= 96) break;
      }
    }
  }

  // Dedup by path (can happen if arrays share nested refs in traversal).
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
  out.erase(std::unique(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.path == b.path; }),
            out.end());
}

static void begin_card(const char* id, const char* title) {
  const ImGuiStyle& style = ImGui::GetStyle();
  ImGui::BeginChild(id, ImVec2(0, 0), true, ImGuiWindowFlags_NoScrollbar);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, style.FramePadding.y));
  ImGui::SeparatorText(title);
  ImGui::PopStyleVar();
}

static void end_card() { ImGui::EndChild(); }

} // namespace

bool add_json_dashboard_for_table_view(UIState& ui, std::uint64_t table_view_id, const std::string& suggested_name) {
  if (table_view_id == 0) return false;

  JsonDashboardConfig cfg;
  cfg.id = ui.next_json_dashboard_id++;
  if (cfg.id == 0) cfg.id = ui.next_json_dashboard_id++;

  cfg.table_view_id = table_view_id;

  if (!suggested_name.empty()) {
    cfg.name = suggested_name;
  } else {
    // Best-effort: derive from the lens name.
    if (auto* v = find_table_view(ui, table_view_id)) {
      cfg.name = v->name.empty() ? "Dashboard" : (v->name + " Dashboard");
    } else {
      cfg.name = "Dashboard";
    }
  }

  ui.json_dashboards.push_back(std::move(cfg));
  ui.request_select_json_dashboard_id = ui.json_dashboards.back().id;
  return true;
}

bool add_json_dashboard_for_path(UIState& ui, const std::string& array_path, const std::string& suggested_name) {
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

  const std::string dash_name = suggested_name.empty()
                                   ? (last_pointer_token(norm).empty() ? "Dashboard" : (last_pointer_token(norm) + " Dashboard"))
                                   : (suggested_name + " Dashboard");
  return add_json_dashboard_for_table_view(ui, view_id, dash_name);
}

void draw_dashboards_window(Simulation& sim, UIState& ui) {
  if (!ui.show_dashboards_window) return;

  static DashboardsState st;
  const double now = ImGui::GetTime();

  if (!st.initialized) {
    st.initialized = true;
    st.last_refresh_time = 0.0;
    st.auto_refresh = true;
    st.refresh_sec = 1.0f;
    std::snprintf(st.add_name, sizeof(st.add_name), "%s", "Dashboard");
    std::snprintf(st.add_path, sizeof(st.add_path), "%s", "/");
  }

  // Consume selection request.
  if (ui.request_select_json_dashboard_id != 0) {
    st.selected_dashboard_id = ui.request_select_json_dashboard_id;
    ui.request_select_json_dashboard_id = 0;
  }

  // Auto refresh.
  if (st.auto_refresh && (now - st.last_refresh_time) >= (double)st.refresh_sec) {
    st.last_refresh_time = now;
    refresh_doc(st, sim, /*force=*/false);
  }

  if (!ImGui::Begin("Dashboards", &ui.show_dashboards_window)) {
    ImGui::End();
    return;
  }

  // Top bar.
  {
    if (ImGui::Button("Refresh")) {
      st.last_refresh_time = now;
      refresh_doc(st, sim, /*force=*/true);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto", &st.auto_refresh);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::SliderFloat("##dash_refresh", &st.refresh_sec, 0.1f, 5.0f, "%.2fs");
    ImGui::SameLine();
    ImGui::TextDisabled("rev %llu", (unsigned long long)st.doc_revision);

    if (!st.doc_error.empty()) {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "doc err: %s", st.doc_error.c_str());
    }
  }

  ImGui::Separator();

  // Two-pane layout.
  ImGui::BeginChild("##dash_left", ImVec2(260, 0), true);
  {
    ImGui::SeparatorText("Dashboards");

    // List.
    int remove_idx = -1;
    for (int i = 0; i < (int)ui.json_dashboards.size(); ++i) {
      auto& d = ui.json_dashboards[(std::size_t)i];
      ImGui::PushID((int)d.id);

      const bool selected = (st.selected_dashboard_id == d.id);
      if (ImGui::Selectable(d.name.c_str(), selected)) {
        st.selected_dashboard_id = d.id;
      }

      if (ImGui::BeginPopupContextItem("##dash_ctx")) {
        if (ImGui::MenuItem("Duplicate")) {
          JsonDashboardConfig c = d;
          c.id = ui.next_json_dashboard_id++;
          c.name += " (copy)";
          ui.json_dashboards.push_back(std::move(c));
          st.selected_dashboard_id = ui.json_dashboards.back().id;
        }
        if (ImGui::MenuItem("Delete")) {
          remove_idx = i;
        }
        if (ImGui::MenuItem("Open source lens")) {
          ui.show_data_lenses_window = true;
          ui.request_select_json_table_view_id = d.table_view_id;
        }
        ImGui::EndPopup();
      }

      ImGui::PopID();
    }

    if (remove_idx >= 0) {
      const std::uint64_t rid = ui.json_dashboards[(std::size_t)remove_idx].id;
      ui.json_dashboards.erase(ui.json_dashboards.begin() + remove_idx);
      st.runtimes.erase(rid);
      if (st.selected_dashboard_id == rid) st.selected_dashboard_id = 0;
    }

    ImGui::Separator();

    ImGui::SeparatorText("Create");

    if (ImGui::RadioButton("From Lens", st.add_source_use_lens)) st.add_source_use_lens = true;
    ImGui::SameLine();
    if (ImGui::RadioButton("From Path", !st.add_source_use_lens)) st.add_source_use_lens = false;

    ImGui::InputText("Name", st.add_name, IM_ARRAYSIZE(st.add_name));

    if (st.add_source_use_lens) {
      // Lens dropdown.
      if (st.add_table_view_id == 0 && !ui.json_table_views.empty()) {
        st.add_table_view_id = ui.json_table_views.front().id;
      }

      const char* preview = "(none)";
      if (auto* v = find_table_view(ui, st.add_table_view_id)) {
        preview = v->name.c_str();
      }

      if (ImGui::BeginCombo("Lens", preview)) {
        for (const auto& v : ui.json_table_views) {
          const bool sel = (v.id == st.add_table_view_id);
          if (ImGui::Selectable(v.name.c_str(), sel)) {
            st.add_table_view_id = v.id;
          }
          if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }

      if (ui.json_table_views.empty()) {
        ImGui::TextDisabled("(no Data Lenses yet — create one from JSON Explorer / Data Lenses)");
      }

      if (ImGui::Button("Create Dashboard") && st.add_table_view_id != 0) {
        const std::string nm = st.add_name;
        if (add_json_dashboard_for_table_view(ui, st.add_table_view_id, nm)) {
          ui.show_dashboards_window = true;
          // Request select handled by add_json_dashboard.
        }
      }
    } else {
      // Path input with autocomplete.
      std::string current = st.add_path;
      std::vector<std::string> sugg;
      if (st.root) {
        sugg = nebula4x::suggest_json_pointer_completions(*st.root, current, 8);
      }

      if (ImGui::InputText("Array path", st.add_path, IM_ARRAYSIZE(st.add_path))) {
        // keep
      }
      if (!sugg.empty()) {
        ImGui::TextDisabled("Suggestions:");
        for (const auto& s : sugg) {
          if (ImGui::SmallButton(s.c_str())) {
            std::snprintf(st.add_path, sizeof(st.add_path), "%s", s.c_str());
          }
          ImGui::SameLine();
        }
        ImGui::NewLine();
      }

      if (ImGui::Button("Create (Lens + Dashboard)")) {
        const std::string nm = st.add_name;
        const std::string path = st.add_path;
        if (add_json_dashboard_for_path(ui, path, nm)) {
          ui.show_dashboards_window = true;
          ui.show_data_lenses_window = true;
        }
      }
    }

    ImGui::Separator();
    ImGui::SeparatorText("Discover");

    if (!st.root) {
      ImGui::TextDisabled("(load the JSON doc to discover datasets)");
    } else {
      if (ImGui::Button("Scan for arrays-of-objects")) {
        discover_datasets(*st.root, st.discovered);
      }

      if (!st.discovered.empty()) {
        ImGui::TextDisabled("Found %d", (int)st.discovered.size());
        for (const auto& d : st.discovered) {
          ImGui::PushID(d.path.c_str());
          ImGui::TextUnformatted(d.label.c_str());
          ImGui::SameLine();
          ImGui::TextDisabled("(%zu)", d.size);

          if (ImGui::SmallButton("+Dash")) {
            const std::string nm = d.label.empty() ? "Dashboard" : d.label;
            (void)add_json_dashboard_for_path(ui, d.path, nm);
            ui.show_dashboards_window = true;
            ui.show_data_lenses_window = true;
          }
          ImGui::SameLine();
          if (ImGui::SmallButton("+Lens")) {
            ui.show_data_lenses_window = true;
            (void)add_json_table_view(ui, d.path, d.label);
          }
          ImGui::PopID();
        }
      }
    }
  }
  ImGui::EndChild();

  ImGui::SameLine();

  ImGui::BeginChild("##dash_right", ImVec2(0, 0), true);
  {
    auto* dash = find_dashboard(ui, st.selected_dashboard_id);
    if (!dash) {
      ImGui::TextDisabled("Select or create a dashboard.");
      ImGui::EndChild();
      ImGui::End();
      return;
    }

    auto* view = find_table_view(ui, dash->table_view_id);
    if (!view) {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Source lens not found (id=%llu)",
                         (unsigned long long)dash->table_view_id);
      ImGui::EndChild();
      ImGui::End();
      return;
    }

    // Header/editor.
    ImGui::SeparatorText(dash->name.c_str());

    {
      char name_buf[128];
      std::snprintf(name_buf, sizeof(name_buf), "%s", dash->name.c_str());
      if (ImGui::InputText("Name##dash_name", name_buf, IM_ARRAYSIZE(name_buf))) {
        dash->name = name_buf;
      }

      // Lens selector.
      const char* lens_preview = view->name.c_str();
      if (ImGui::BeginCombo("Lens##dash_lens", lens_preview)) {
        for (const auto& v : ui.json_table_views) {
          const bool sel = (v.id == dash->table_view_id);
          if (ImGui::Selectable(v.name.c_str(), sel)) {
            dash->table_view_id = v.id;
            // Switching lenses: also clear selection in runtime.
            st.runtimes.erase(dash->id);
          }
          if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }

      ImGui::SameLine();
      if (ImGui::Button("Open Lens")) {
        ui.show_data_lenses_window = true;
        ui.request_select_json_table_view_id = dash->table_view_id;
      }

      ImGui::SameLine();
      if (ImGui::Button("Go to JSON")) {
        ui.show_json_explorer_window = true;
        ui.request_json_explorer_goto_path = view->array_path;
      }

      ImGui::Separator();

      ImGui::SetNextItemWidth(160.0f);
      ImGui::SliderInt("Scan rows", &dash->scan_rows, 50, 500000);
      dash->scan_rows = std::clamp(dash->scan_rows, 10, 500000);

      ImGui::SetNextItemWidth(160.0f);
      ImGui::SliderInt("Rows/frame", &dash->rows_per_frame, 10, 5000);
      dash->rows_per_frame = std::clamp(dash->rows_per_frame, 1, 50000);

      ImGui::Checkbox("Link to lens filter", &dash->link_to_lens_filter);
      ImGui::SameLine();
      ImGui::Checkbox("Use disabled lens columns", &dash->use_all_lens_columns);

      ImGui::Separator();

      ImGui::SetNextItemWidth(160.0f);
      ImGui::SliderInt("Histogram bins", &dash->histogram_bins, 4, 64);
      dash->histogram_bins = std::clamp(dash->histogram_bins, 2, 128);

      ImGui::SetNextItemWidth(160.0f);
      ImGui::SliderInt("Numeric charts", &dash->max_numeric_charts, 0, 12);
      dash->max_numeric_charts = std::clamp(dash->max_numeric_charts, 0, 64);

      ImGui::SetNextItemWidth(160.0f);
      ImGui::SliderInt("Category cards", &dash->max_category_cards, 0, 12);
      dash->max_category_cards = std::clamp(dash->max_category_cards, 0, 64);

      ImGui::SetNextItemWidth(160.0f);
      ImGui::SliderInt("Top N", &dash->top_n, 3, 32);
      dash->top_n = std::clamp(dash->top_n, 1, 1000);

      if (ImGui::Button("Rebuild stats")) {
        st.runtimes.erase(dash->id);
      }
    }

    ImGui::Separator();

    if (!st.doc_loaded) {
      ImGui::TextDisabled("(waiting for JSON doc)");
      ImGui::EndChild();
      ImGui::End();
      return;
    }

    // Runtime build.
    DashboardRuntime& rt = st.runtimes[dash->id];
    const std::string key = build_cache_key(st, *dash, *view);
    if (rt.cache_key != key) {
      rt = DashboardRuntime{};
      rt.cache_key = key;
      rt.building = true;
      rt.ready = false;
      rt.error.clear();
      rt.array = nullptr;
      rt.total_rows = 0;
      rt.scan_i = 0;
      rt.included_rows = 0;

      // Snapshot filter config.
      if (dash->link_to_lens_filter) {
        rt.filter = view->filter;
        rt.filter_case_sensitive = view->filter_case_sensitive;
        rt.filter_all_fields = view->filter_all_fields;
      } else {
        rt.filter.clear();
        rt.filter_case_sensitive = false;
        rt.filter_all_fields = false;
      }

      // Filter columns = enabled lens columns.
      rt.filter_rel_paths.clear();
      for (const auto& c : view->columns) {
        if (c.enabled) rt.filter_rel_paths.push_back(c.rel_path);
      }

      // Probes = either enabled columns or all (if requested). If the lens has no columns, we fall back to
      // a single probe for the whole row.
      rt.probes.clear();
      if (!view->columns.empty()) {
        for (const auto& c : view->columns) {
          if (!dash->use_all_lens_columns && !c.enabled) continue;
          ColumnProbe p;
          p.label = c.label.empty() ? c.rel_path : c.label;
          p.rel_path = c.rel_path.empty() ? "/" : c.rel_path;
          if (!p.rel_path.empty() && p.rel_path[0] != '/') p.rel_path = "/" + p.rel_path;
          rt.probes.push_back(std::move(p));
        }
      }
      if (rt.probes.empty()) {
        ColumnProbe p;
        p.label = "Row";
        p.rel_path = "/";
        rt.probes.push_back(std::move(p));
      }

      // Resolve array pointer.
      std::string aerr;
      const auto* node = st.root
                             ? nebula4x::resolve_json_pointer(*st.root, view->array_path, true, &aerr)
                             : nullptr;
      if (!node || !node->is_array()) {
        rt.building = false;
        rt.error = aerr.empty() ? "Path is not an array" : aerr;
      } else {
        rt.array = node->as_array();
        rt.total_rows = rt.array ? rt.array->size() : 0;
        const std::size_t cap = (std::size_t)std::max(1, dash->scan_rows);
        rt.scan_max = std::min(rt.total_rows, cap);
      }
    }

    if (!rt.error.empty()) {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Build error: %s", rt.error.c_str());
      ImGui::EndChild();
      ImGui::End();
      return;
    }

    // Incremental scan step.
    if (rt.building && rt.array) {
      const int top_k = std::max(3, dash->top_n);
      const int max_distinct = 256;
      const int steps = std::clamp(dash->rows_per_frame, 1, 100000);

      for (int step = 0; step < steps; ++step) {
        if (rt.scan_i >= rt.scan_max) {
          rt.building = false;
          rt.ready = true;
          break;
        }

        const std::size_t row_i = rt.scan_i;
        rt.scan_i++;

        const auto& row = (*rt.array)[row_i];

        // Apply filter.
        bool keep = true;
        if (!rt.filter.empty()) {
          if (rt.filter_all_fields) {
            int budget = 512;
            keep = node_contains_text_limited(row, rt.filter, rt.filter_case_sensitive, 4, budget);
          } else {
            keep = false;
            for (const auto& rp : rt.filter_rel_paths) {
              std::string ferr;
              const auto* fv = nebula4x::resolve_json_pointer(row, rp, true, &ferr);
              if (fv && contains_substring(scalar_preview(*fv, 128), rt.filter, rt.filter_case_sensitive)) {
                keep = true;
                break;
              }
            }
          }
        }

        if (!keep) continue;

        rt.included_rows++;

        // Update probes.
        for (auto& p : rt.probes) {
          std::string perr;
          const auto* pv = nebula4x::resolve_json_pointer(row, p.rel_path, true, &perr);
          probe_add_value(p, pv, row_i, top_k, max_distinct);
        }
      }
    }

    // Progress.
    if (rt.building) {
      const float frac = (rt.scan_max > 0) ? (float)((double)rt.scan_i / (double)rt.scan_max) : 0.0f;
      ImGui::TextDisabled("Scanning %zu / %zu rows (matched %zu)", rt.scan_i, rt.scan_max, rt.included_rows);
      ImGui::ProgressBar(frac, ImVec2(-1, 0), nullptr);
      ImGui::Separator();
    } else {
      ImGui::TextDisabled("Rows: %zu (scanned %zu, matched %zu)", rt.total_rows, rt.scan_max, rt.included_rows);
      if (dash->link_to_lens_filter && !view->filter.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("| filter: %s", view->filter.c_str());
      }
      ImGui::Separator();
    }

    if (!rt.ready) {
      ImGui::TextDisabled("(building dashboard stats...)");
      ImGui::EndChild();
      ImGui::End();
      return;
    }

    // Build widgets from probes (cheap, based on current dash settings).
    std::vector<NumericWidget> numeric;
    std::vector<CategoryWidget> cats;

    // Numeric candidates.
    struct NumCand {
      double range{0.0};
      ColumnProbe* p{nullptr};
    };
    std::vector<NumCand> num_cands;
    num_cands.reserve(rt.probes.size());
    for (auto& p : rt.probes) {
      if (!p.has_minmax) continue;
      if (p.numeric_count < 2) continue;
      // Prefer columns that are predominantly numeric.
      if (p.numeric_count < (p.string_count + p.bool_count)) continue;
      num_cands.push_back({p.max - p.min, &p});
    }
    std::sort(num_cands.begin(), num_cands.end(), [](const auto& a, const auto& b) { return a.range > b.range; });

    const int want_num = std::min<int>(dash->max_numeric_charts, (int)num_cands.size());
    numeric.reserve((std::size_t)want_num);

    for (int i = 0; i < want_num; ++i) {
      ColumnProbe& p = *num_cands[(std::size_t)i].p;
      NumericWidget w;
      w.label = p.label;
      w.rel_path = p.rel_path;
      w.count = p.numeric_count;
      w.min = p.min;
      w.max = p.max;
      w.mean = (p.numeric_count > 0) ? (p.sum / (double)p.numeric_count) : 0.0;

      // Histogram.
      const int bins = std::max(2, dash->histogram_bins);
      w.hist.assign((std::size_t)bins, 0.0f);
      if (p.has_minmax && p.max > p.min && !p.values.empty()) {
        for (float vf : p.values) {
          const double v = (double)vf;
          double t = (v - p.min) / (p.max - p.min);
          if (t < 0.0) t = 0.0;
          if (t > 1.0) t = 1.0;
          int bi = (int)std::floor(t * (double)bins);
          if (bi >= bins) bi = bins - 1;
          if (bi < 0) bi = 0;
          w.hist[(std::size_t)bi] += 1.0f;
        }
      } else if (!p.values.empty()) {
        // Degenerate range: put everything in the first bin.
        w.hist[0] = (float)p.values.size();
      }

      w.top = p.top;
      std::sort(w.top.begin(), w.top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
      if ((int)w.top.size() > dash->top_n) w.top.resize((std::size_t)dash->top_n);
      numeric.push_back(std::move(w));
    }

    // Categorical candidates.
    struct CatCand {
      int distinct{0};
      int count{0};
      ColumnProbe* p{nullptr};
    };
    std::vector<CatCand> cat_cands;
    cat_cands.reserve(rt.probes.size());
    for (auto& p : rt.probes) {
      const int cc = p.string_count + p.bool_count;
      if (cc < 2) continue;
      if (cc < p.numeric_count) continue;
      const int distinct = (int)p.freq.size() + (p.freq_truncated ? 1 : 0);
      if (distinct <= 1) continue;
      cat_cands.push_back({distinct, cc, &p});
    }
    std::sort(cat_cands.begin(), cat_cands.end(), [](const auto& a, const auto& b) {
      if (a.distinct != b.distinct) return a.distinct < b.distinct;
      return a.count > b.count;
    });

    const int want_cat = std::min<int>(dash->max_category_cards, (int)cat_cands.size());
    cats.reserve((std::size_t)want_cat);
    for (int i = 0; i < want_cat; ++i) {
      ColumnProbe& p = *cat_cands[(std::size_t)i].p;
      CategoryWidget w;
      w.label = p.label;
      w.rel_path = p.rel_path;
      w.count = p.string_count + p.bool_count;
      w.distinct = (int)p.freq.size();
      w.truncated = p.freq_truncated;
      w.other_count = p.other_count;

      std::vector<std::pair<std::string, int>> vec;
      vec.reserve(p.freq.size());
      for (const auto& kv : p.freq) vec.push_back(kv);
      std::sort(vec.begin(), vec.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
      });
      if ((int)vec.size() > dash->top_n) vec.resize((std::size_t)dash->top_n);
      w.top = std::move(vec);
      cats.push_back(std::move(w));
    }

    // Procedural widget grid layout.
    const float avail_w = ImGui::GetContentRegionAvail().x;
    int cols = 1;
    if (avail_w > 740.0f) cols = 2;
    if (avail_w > 1120.0f) cols = 3;
    cols = std::clamp(cols, 1, 4);

    const ImGuiTableFlags grid_flags = ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX;

    if (ImGui::BeginTable("##dash_grid", cols, grid_flags)) {
      int cell = 0;

      auto next_cell = [&]() {
        ImGui::TableNextColumn();
        ++cell;
      };

      // Summary card.
      next_cell();
      begin_card("##card_summary", "Summary");
      {
        ImGui::Text("Lens: %s", view->name.c_str());
        ImGui::Text("Path: %s", view->array_path.c_str());
        ImGui::Text("Rows: %zu", rt.total_rows);
        if (dash->link_to_lens_filter && !view->filter.empty()) {
          ImGui::Text("Filter: %s", view->filter.c_str());
        }
        ImGui::Separator();
        ImGui::TextDisabled("Columns scanned: %zu", rt.probes.size());
      }
      end_card();

      // Top rows card (drill-down).
      if (!numeric.empty()) {
        next_cell();
        begin_card("##card_toprows", "Top rows");
        {
          // Choose metric.
          const char* metric_preview = numeric.front().label.c_str();
          int metric_idx = 0;
          for (int i = 0; i < (int)numeric.size(); ++i) {
            if (!dash->top_rows_rel_path.empty() && numeric[(std::size_t)i].rel_path == dash->top_rows_rel_path) {
              metric_idx = i;
              metric_preview = numeric[(std::size_t)i].label.c_str();
              break;
            }
          }

          if (ImGui::BeginCombo("Metric", metric_preview)) {
            for (int i = 0; i < (int)numeric.size(); ++i) {
              const bool sel = (i == metric_idx);
              if (ImGui::Selectable(numeric[(std::size_t)i].label.c_str(), sel)) {
                dash->top_rows_rel_path = numeric[(std::size_t)i].rel_path;
              }
              if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
          }

          const auto& m = numeric[(std::size_t)metric_idx];
          if (m.top.empty()) {
            ImGui::TextDisabled("(no data)");
          } else {
            ImGui::TextDisabled("Click a row to jump in JSON Explorer");
            ImGui::BeginTable("##top_table", 2, ImGuiTableFlags_SizingStretchProp);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Row", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (const auto& [val, idx] : m.top) {
              ImGui::TableNextRow();
              ImGui::TableSetColumnIndex(0);
              ImGui::Text("%.6g", val);
              ImGui::TableSetColumnIndex(1);
              char row_lbl[64];
              std::snprintf(row_lbl, sizeof(row_lbl), "[%zu]", idx);
              if (ImGui::Selectable(row_lbl, false, ImGuiSelectableFlags_SpanAllColumns)) {
                ui.show_json_explorer_window = true;
                ui.request_json_explorer_goto_path = nebula4x::json_pointer_join_index(view->array_path, idx);
              }
            }
            ImGui::EndTable();
          }
        }
        end_card();
      }

      // Numeric charts.
      for (int i = 0; i < (int)numeric.size(); ++i) {
        next_cell();
        const std::string id = "##card_num_" + std::to_string(i);
        begin_card(id.c_str(), numeric[(std::size_t)i].label.c_str());
        {
          const auto& w = numeric[(std::size_t)i];
          ImGui::TextDisabled("n=%d", w.count);
          ImGui::SameLine();
          ImGui::TextDisabled("min=%.6g  max=%.6g  mean=%.6g", w.min, w.max, w.mean);

          float max_h = 0.0f;
          for (float x : w.hist) max_h = std::max(max_h, x);
          ImGui::PlotHistogram("##hist", w.hist.data(), (int)w.hist.size(), 0, nullptr, 0.0f,
                               max_h > 0.0f ? max_h : 1.0f, ImVec2(-1, 70.0f));
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s\nmin %.6g\nmax %.6g", w.label.c_str(), w.min, w.max);
          }
        }
        end_card();
      }

      // Category cards.
      for (int i = 0; i < (int)cats.size(); ++i) {
        next_cell();
        const std::string id = "##card_cat_" + std::to_string(i);
        begin_card(id.c_str(), cats[(std::size_t)i].label.c_str());
        {
          const auto& w = cats[(std::size_t)i];
          ImGui::TextDisabled("n=%d  distinct=%d%s", w.count, w.distinct, w.truncated ? "+" : "");

          if (w.top.empty()) {
            ImGui::TextDisabled("(no categories)");
          } else {
            ImGui::BeginTable("##cat_table", 3,
                             ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableHeadersRow();

            for (const auto& kv : w.top) {
              const auto& name = kv.first;
              const int count = kv.second;
              const float frac = (rt.included_rows > 0) ? (float)((double)count / (double)rt.included_rows) : 0.0f;

              ImGui::TableNextRow();
              ImGui::TableSetColumnIndex(0);

              const std::string disp = name.empty() ? std::string("(empty)") : name;
              if (ImGui::Selectable(disp.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                // Drill-down: set lens filter to the clicked value and open the lens.
                if (dash->link_to_lens_filter) {
                  view->filter = disp;
                }
                ui.show_data_lenses_window = true;
                ui.request_select_json_table_view_id = dash->table_view_id;
              }

              ImGui::TableSetColumnIndex(1);
              ImGui::Text("%d", count);
              ImGui::TableSetColumnIndex(2);
              ImGui::Text("%.1f%%", frac * 100.0f);
            }
            if (w.truncated && w.other_count > 0) {
              const float frac = (rt.included_rows > 0) ? (float)((double)w.other_count / (double)rt.included_rows) : 0.0f;
              ImGui::TableNextRow();
              ImGui::TableSetColumnIndex(0);
              ImGui::TextDisabled("(other)");
              ImGui::TableSetColumnIndex(1);
              ImGui::TextDisabled("%d", w.other_count);
              ImGui::TableSetColumnIndex(2);
              ImGui::TextDisabled("%.1f%%", frac * 100.0f);
            }
            ImGui::EndTable();

            ImGui::TextDisabled("(click a value to filter the source lens)");
          }
        }
        end_card();
      }

      ImGui::EndTable();
    }
  }
  ImGui::EndChild();

  ImGui::End();
}

} // namespace nebula4x::ui
