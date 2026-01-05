#include "ui/data_lenses_window.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "nebula4x/util/json.h"
#include "nebula4x/util/json_pointer.h"
#include "nebula4x/util/json_pointer_autocomplete.h"
#include "nebula4x/util/log.h"
#include "ui/game_json_cache.h"
#include "ui/game_entity_index.h"
#include "ui/watchboard_window.h"
#include "ui/dashboards_window.h"
#include "ui/pivot_tables_window.h"

namespace nebula4x::ui {
namespace {

char to_lower_ascii(char c) {
  return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

bool contains_substring(std::string_view haystack, std::string_view needle, bool case_sensitive) {
  if (needle.empty()) return true;
  if (haystack.empty()) return false;

  if (case_sensitive) {
    return haystack.find(needle) != std::string_view::npos;
  }

  // Naive ASCII case-insensitive scan (good enough for UI search).
  for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
    bool ok = true;
    for (std::size_t j = 0; j < needle.size(); ++j) {
      if (to_lower_ascii(haystack[i + j]) != to_lower_ascii(needle[j])) {
        ok = false;
        break;
      }
    }
    if (ok) return true;
  }
  return false;
}

std::string value_type_name(const nebula4x::json::Value& v) {
  if (v.is_null()) return "null";
  if (v.is_bool()) return "bool";
  if (v.is_number()) return "number";
  if (v.is_string()) return "string";
  if (v.is_array()) return "array";
  if (v.is_object()) return "object";
  return "unknown";
}

// Compact preview string for table cells.
std::string preview_value(const nebula4x::json::Value& v, std::size_t max_len = 120) {
  std::string out;
  if (v.is_null()) out = "null";
  else if (v.is_bool()) out = v.bool_value(false) ? "true" : "false";
  else if (v.is_number()) {
    char buf[64];
    const double n = v.number_value(0.0);
    if (std::isfinite(n)) {
      std::snprintf(buf, sizeof(buf), "%.8g", n);
    } else {
      std::snprintf(buf, sizeof(buf), "%s", std::isnan(n) ? "nan" : (n > 0 ? "+inf" : "-inf"));
    }
    out = buf;
  } else if (v.is_string()) {
    const std::string s = v.string_value("");
    out.reserve(s.size() + 2);
    out.push_back('"');
    out += s;
    out.push_back('"');
  } else if (v.is_array()) {
    const auto* a = v.as_array();
    const std::size_t n = a ? a->size() : 0;
    out = "[" + std::to_string(n) + "]";
  } else if (v.is_object()) {
    const auto* o = v.as_object();
    const std::size_t n = o ? o->size() : 0;
    out = "{" + std::to_string(n) + "}";
  }

  if (out.size() > max_len) {
    out.resize(max_len);
    if (max_len >= 3) {
      out[max_len - 1] = '.';
      out[max_len - 2] = '.';
      out[max_len - 3] = '.';
    }
  }
  return out;
}

bool is_array_of_objects(const nebula4x::json::Value& v, std::size_t probe = 6) {
  const auto* a = v.as_array();
  if (!a || a->empty()) return false;
  const std::size_t n = std::min<std::size_t>(a->size(), probe);
  for (std::size_t i = 0; i < n; ++i) {
    if ((*a)[i].is_object()) return true;
  }
  return false;
}

std::string default_lens_name_from_path(const std::string& path) {
  if (path.empty() || path == "/") return "Lens";
  const auto toks = nebula4x::split_json_pointer(path);
  if (!toks.empty()) {
    const std::string& last = toks.back();
    if (!last.empty()) return last;
  }
  return "Lens";
}

// Column inference: collect scalar leaves and (optionally) container sizes.
struct InferredColumn {
  std::string label;
  std::string rel_path;
};

void collect_columns_from_node(const nebula4x::json::Value& node, const std::string& rel_path,
                               const std::string& label_prefix, int depth, int max_depth,
                               bool include_container_sizes, int max_columns,
                               std::unordered_map<std::string, std::string>& path_to_label) {
  if ((int)path_to_label.size() >= max_columns) return;
  if (depth > max_depth) return;

  const auto add_col = [&](const std::string& p, const std::string& label) {
    if ((int)path_to_label.size() >= max_columns) return;
    if (path_to_label.find(p) != path_to_label.end()) return;
    path_to_label.emplace(p, label.empty() ? p : label);
  };

  const auto mk_label = [&](const std::string& base, const char* suffix) {
    if (base.empty()) return std::string(suffix);
    return base + suffix;
  };

  if (node.is_object()) {
    const auto* o = node.as_object();
    if (!o) return;

    if (include_container_sizes && rel_path != "/") {
      add_col(rel_path, mk_label(label_prefix, ".keys"));
    }

    if (depth == max_depth) return;

    // Stabilize column ordering by iterating keys in sorted order.
    std::vector<std::string> keys;
    keys.reserve(o->size());
    for (const auto& kv : *o) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());

    for (const auto& k : keys) {
      const auto it = o->find(k);
      if (it == o->end()) continue;
      const std::string child_path = nebula4x::json_pointer_join(rel_path, k);
      const std::string child_label = label_prefix.empty() ? k : (label_prefix + "." + k);
      collect_columns_from_node(it->second, child_path, child_label, depth + 1, max_depth, include_container_sizes,
                                max_columns, path_to_label);
      if ((int)path_to_label.size() >= max_columns) break;
    }
    return;
  }

  if (node.is_array()) {
    const auto* a = node.as_array();
    if (!a) return;
    if (include_container_sizes) {
      add_col(rel_path, mk_label(label_prefix, ".len"));
    }
    return;
  }

  // Scalar.
  {
    std::string label = label_prefix;
    if (label.empty()) label = "value";
    add_col(rel_path, label);
  }
}

void infer_columns_for_view(const nebula4x::json::Value& root, JsonTableViewConfig& view,
                            std::string* out_err = nullptr) {
  if (out_err) out_err->clear();
  std::string err;
  const auto* node = nebula4x::resolve_json_pointer(root, view.array_path, /*accept_root_slash=*/true, &err);
  if (!node) {
    if (out_err) *out_err = err;
    return;
  }

  const auto* arr = node->as_array();
  if (!arr) {
    if (out_err) *out_err = "Pointer does not resolve to an array.";
    return;
  }

  const std::size_t n = arr->size();
  if (n == 0) {
    view.columns.clear();
    return;
  }

  const std::size_t sample = std::min<std::size_t>(n, static_cast<std::size_t>(std::max(view.sample_rows, 1)));
  const int max_cols = std::clamp(view.max_infer_columns, 4, 512);
  const int max_depth = std::clamp(view.max_depth, 0, 6);

  std::unordered_map<std::string, std::string> p2l;
  p2l.reserve(std::min<int>(max_cols, 256));

  for (std::size_t i = 0; i < sample; ++i) {
    const auto& e = (*arr)[i];
    collect_columns_from_node(e, "/", "", 0, max_depth, view.include_container_sizes, max_cols, p2l);
    if ((int)p2l.size() >= max_cols) break;
  }

  std::vector<InferredColumn> cols;
  cols.reserve(p2l.size());
  for (auto& kv : p2l) {
    InferredColumn c;
    c.rel_path = std::move(kv.first);
    c.label = std::move(kv.second);
    cols.push_back(std::move(c));
  }

  std::sort(cols.begin(), cols.end(), [](const InferredColumn& a, const InferredColumn& b) {
    if (a.label != b.label) return a.label < b.label;
    return a.rel_path < b.rel_path;
  });

  view.columns.clear();
  view.columns.reserve(cols.size());

  const int enable_first = 12;
  for (std::size_t i = 0; i < cols.size(); ++i) {
    JsonTableColumnConfig cc;
    cc.label = cols[i].label;
    cc.rel_path = cols[i].rel_path;
    cc.enabled = ((int)i < enable_first);
    view.columns.push_back(std::move(cc));
  }
}

// Filter helper: scan scalar values recursively, with a hard cap.
bool node_contains_text(const nebula4x::json::Value& node, std::string_view needle, bool case_sensitive,
                        int* visited, int max_visited) {
  if (*visited >= max_visited) return false;
  ++(*visited);

  if (node.is_string() || node.is_number() || node.is_bool() || node.is_null()) {
    const std::string pv = preview_value(node, 512);
    return contains_substring(pv, needle, case_sensitive);
  }

  if (node.is_object()) {
    const auto* o = node.as_object();
    if (!o) return false;
    for (const auto& kv : *o) {
      if (contains_substring(kv.first, needle, case_sensitive)) return true;
      if (node_contains_text(kv.second, needle, case_sensitive, visited, max_visited)) return true;
    }
    return false;
  }

  if (node.is_array()) {
    const auto* a = node.as_array();
    if (!a) return false;
    const std::size_t n = a->size();
    // Cap scanning long arrays.
    const std::size_t scan = std::min<std::size_t>(n, 64);
    for (std::size_t i = 0; i < scan; ++i) {
      if (node_contains_text((*a)[i], needle, case_sensitive, visited, max_visited)) return true;
    }
    return false;
  }

  return false;
}

// Sorting.
enum class SortType { Missing = 0, Number = 1, String = 2, Bool = 3 };

struct SortValue {
  SortType type{SortType::Missing};
  double num{0.0};
  bool b{false};
  std::string s;
};

SortValue make_sort_value(const nebula4x::json::Value* v) {
  SortValue sv;
  if (!v) {
    sv.type = SortType::Missing;
    return sv;
  }
  if (v->is_number()) {
    sv.type = SortType::Number;
    sv.num = v->number_value(0.0);
    return sv;
  }
  if (v->is_bool()) {
    sv.type = SortType::Bool;
    sv.b = v->bool_value(false);
    sv.num = sv.b ? 1.0 : 0.0;
    return sv;
  }
  if (v->is_string()) {
    sv.type = SortType::String;
    sv.s = v->string_value("");
    if (sv.s.size() > 256) {
      sv.s.resize(256);
    }
    return sv;
  }
  if (v->is_array()) {
    sv.type = SortType::Number;
    const auto* a = v->as_array();
    sv.num = a ? static_cast<double>(a->size()) : 0.0;
    return sv;
  }
  if (v->is_object()) {
    sv.type = SortType::Number;
    const auto* o = v->as_object();
    sv.num = o ? static_cast<double>(o->size()) : 0.0;
    return sv;
  }
  sv.type = SortType::Missing;
  return sv;
}

int compare_sort_value(const SortValue& a, const SortValue& b) {
  // Missing always sorts last.
  if (a.type == SortType::Missing && b.type == SortType::Missing) return 0;
  if (a.type == SortType::Missing) return 1;
  if (b.type == SortType::Missing) return -1;

  if (a.type != b.type) {
    return (static_cast<int>(a.type) < static_cast<int>(b.type)) ? -1 : 1;
  }

  switch (a.type) {
    case SortType::Number: {
      if (a.num < b.num) return -1;
      if (a.num > b.num) return 1;
      return 0;
    }
    case SortType::String: {
      if (a.s < b.s) return -1;
      if (a.s > b.s) return 1;
      return 0;
    }
    case SortType::Bool: {
      if (a.b == b.b) return 0;
      return a.b ? 1 : -1;
    }
    case SortType::Missing:
    default:
      return 0;
  }
}

struct DiscoveredDataset {
  std::string path;
  std::size_t size{0};
  int object_samples{0};
  std::vector<std::string> sample_keys;
};

void scan_datasets_rec(const nebula4x::json::Value& node, const std::string& path, int depth, int max_depth,
                       int* visited, int max_visited, std::vector<DiscoveredDataset>& out) {
  if (*visited >= max_visited) return;
  ++(*visited);

  if (depth > max_depth) return;

  if (node.is_array()) {
    const auto* a = node.as_array();
    if (!a) return;
    const std::size_t n = a->size();

    // Check for arrays-of-objects.
    int obj = 0;
    std::vector<std::string> keys;
    const std::size_t probe = std::min<std::size_t>(n, 8);
    for (std::size_t i = 0; i < probe; ++i) {
      if ((*a)[i].is_object()) {
        ++obj;
        if (keys.empty()) {
          const auto* o = (*a)[i].as_object();
          if (o) {
            for (const auto& kv : *o) {
              keys.push_back(kv.first);
              if (keys.size() >= 6) break;
            }
            std::sort(keys.begin(), keys.end());
          }
        }
      }
    }
    if (obj > 0) {
      DiscoveredDataset d;
      d.path = path;
      d.size = n;
      d.object_samples = obj;
      d.sample_keys = std::move(keys);
      out.push_back(std::move(d));
    }

    // Recurse into a few elements (to find nested arrays-of-objects).
    const std::size_t scan = std::min<std::size_t>(n, 6);
    for (std::size_t i = 0; i < scan; ++i) {
      if (*visited >= max_visited) break;
      const std::string child = nebula4x::json_pointer_join_index(path, i);
      scan_datasets_rec((*a)[i], child, depth + 1, max_depth, visited, max_visited, out);
    }
    return;
  }

  if (node.is_object()) {
    const auto* o = node.as_object();
    if (!o) return;
    for (const auto& kv : *o) {
      if (*visited >= max_visited) break;
      const std::string child = nebula4x::json_pointer_join(path, kv.first);
      scan_datasets_rec(kv.second, child, depth + 1, max_depth, visited, max_visited, out);
    }
    return;
  }
}

struct ViewRuntime {
  std::string cache_key;
  std::vector<int> rows;
};

struct DataLensesState {
  bool initialized{false};
  bool auto_refresh{true};
  float refresh_sec{0.75f};
  double last_refresh_time{0.0};
  std::uint64_t doc_revision{0};

  bool doc_loaded{false};
  std::shared_ptr<const nebula4x::json::Value> root;
  std::string doc_error;

  std::uint64_t selected_view_id{0};

  char add_name[64]{};
  char add_path[256]{"/"};

  std::vector<DiscoveredDataset> discovered;

  std::unordered_map<std::uint64_t, ViewRuntime> runtimes;
};

void refresh_doc(DataLensesState& st, Simulation& sim, bool force) {
  const double now = ImGui::GetTime();
  ensure_game_json_cache(sim, now, st.refresh_sec, force);
  const auto& cache = game_json_cache();
  st.doc_revision = cache.revision;
  st.root = cache.root;
  st.doc_loaded = (bool)st.root;
  st.doc_error = cache.error;
}

JsonTableViewConfig* find_view(UIState& ui, std::uint64_t id) {
  for (auto& v : ui.json_table_views) {
    if (v.id == id) return &v;
  }
  return nullptr;
}

std::string build_view_cache_key(const DataLensesState& st, const JsonTableViewConfig& view,
                                 const std::string& sort_rel_path, bool sort_asc) {
  std::string key;
  key.reserve(512);
  key += std::to_string(st.doc_revision);
  key.push_back('|');
  key += view.array_path;
  key.push_back('|');
  key += view.filter;
  key.push_back('|');
  key += view.filter_case_sensitive ? "cs" : "ci";
  key.push_back('|');
  key += view.filter_all_fields ? "all" : "cols";
  key.push_back('|');
  key += std::to_string(view.max_rows);
  key.push_back('|');
  key += sort_rel_path;
  key.push_back('|');
  key += sort_asc ? "asc" : "desc";
  key.push_back('|');
  // Enabled columns affect filtering and sorting choices.
  for (const auto& c : view.columns) {
    if (!c.enabled) continue;
    key += c.rel_path;
    key.push_back(';');
  }
  return key;
}

} // namespace

bool add_json_table_view(UIState& ui, const std::string& array_path, const std::string& suggested_name) {
  std::string p = array_path;
  if (p.empty()) return false;
  if (!p.empty() && p[0] != '/') p = "/" + p;

  JsonTableViewConfig cfg;
  cfg.id = ui.next_json_table_view_id++;
  if (cfg.id == 0) cfg.id = ui.next_json_table_view_id++;

  cfg.array_path = p;
  cfg.name = suggested_name.empty() ? default_lens_name_from_path(p) : suggested_name;
  if (cfg.name.empty()) cfg.name = "Lens";

  ui.json_table_views.push_back(std::move(cfg));
  ui.request_select_json_table_view_id = ui.json_table_views.back().id;
  return true;
}

void draw_data_lenses_window(Simulation& sim, UIState& ui) {
  if (!ui.show_data_lenses_window) return;

  static DataLensesState st;

  if (!st.initialized) {
    st.initialized = true;
    std::snprintf(st.add_name, sizeof(st.add_name), "%s", "New Lens");
    std::snprintf(st.add_path, sizeof(st.add_path), "%s", "/");
    refresh_doc(st, sim, /*force=*/true);
  }

  // One-shot selection request (from JSON Explorer context menus, etc.).
  if (ui.request_select_json_table_view_id != 0) {
    st.selected_view_id = ui.request_select_json_table_view_id;
    ui.request_select_json_table_view_id = 0;
  }

  ImGui::SetNextWindowSize(ImVec2(980, 640), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Data Lenses", &ui.show_data_lenses_window)) {
    ImGui::End();
    return;
  }

  // Auto-refresh the document.
  {
    const double now = ImGui::GetTime();
    if (st.auto_refresh && (now - st.last_refresh_time) >= st.refresh_sec) {
      st.last_refresh_time = now;
      refresh_doc(st, sim, /*force=*/false);
    }
  }

  if (st.doc_loaded && st.root) {
    (void)ensure_game_entity_index(*st.root, st.doc_revision);
  }

  // Top bar.
  {
    if (ImGui::Button("Refresh##lenses")) {
      refresh_doc(st, sim, /*force=*/true);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto", &st.auto_refresh);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::SliderFloat("##refresh_sec", &st.refresh_sec, 0.10f, 5.0f, "%.2fs");
    st.refresh_sec = std::clamp(st.refresh_sec, 0.05f, 60.0f);

    ImGui::SameLine();
    ImGui::TextDisabled("Doc rev: %llu", static_cast<unsigned long long>(st.doc_revision));

    if (!st.doc_error.empty()) {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "Parse error: %s", st.doc_error.c_str());
    }
  }

  ImGui::Separator();

  // Layout: left list + right editor/table.
  const float left_w = 260.0f;
  ImGui::BeginChild("##lenses_left", ImVec2(left_w, 0), true);
  {
    ImGui::Text("Lenses");
    ImGui::Separator();

    if (ui.json_table_views.empty()) {
      ImGui::TextDisabled("No lenses yet.");
      ImGui::TextDisabled("Add one below or create one from JSON Explorer.");
    }

    for (auto& v : ui.json_table_views) {
      const bool sel = (v.id == st.selected_view_id);
      if (ImGui::Selectable(v.name.c_str(), sel)) {
        st.selected_view_id = v.id;
      }
      if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Create Dashboard (Procedural Charts)")) {
          if (add_json_dashboard_for_table_view(ui, v.id, v.name + " Dashboard")) {
            ui.show_dashboards_window = true;
          }
        }
        if (ImGui::MenuItem("Create Pivot Table (Procedural Aggregations)")) {
          if (add_json_pivot_for_table_view(ui, v.id, v.name + " Pivot")) {
            ui.show_pivot_tables_window = true;
          }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate")) {
          JsonTableViewConfig copy = v;
          copy.id = ui.next_json_table_view_id++;
          if (copy.id == 0) copy.id = ui.next_json_table_view_id++;
          copy.name = v.name + " (copy)";
          ui.json_table_views.push_back(std::move(copy));
          st.selected_view_id = ui.json_table_views.back().id;
        }
        if (ImGui::MenuItem("Delete")) {
          const std::uint64_t del_id = v.id;
          ui.json_table_views.erase(
              std::remove_if(ui.json_table_views.begin(), ui.json_table_views.end(),
                             [&](const JsonTableViewConfig& x) { return x.id == del_id; }),
              ui.json_table_views.end());
          if (st.selected_view_id == del_id) st.selected_view_id = ui.json_table_views.empty() ? 0 : ui.json_table_views.front().id;
          st.runtimes.erase(del_id);
          ImGui::EndPopup();
          break;
        }
        ImGui::EndPopup();
      }
    }

    ImGui::Separator();
    ImGui::Text("New Lens");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##lens_name", "Name", st.add_name, IM_ARRAYSIZE(st.add_name));
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##lens_path", "/ships", st.add_path, IM_ARRAYSIZE(st.add_path));

    // Autocomplete for the dataset pointer.
    if (st.root) {
      const std::vector<std::string> sugg = nebula4x::suggest_json_pointer_completions(
          *st.root, st.add_path, 10, /*accept_root_slash=*/true, /*case_sensitive=*/false);
      if (!sugg.empty() && ImGui::BeginListBox("##lens_path_sugg", ImVec2(-1, 90))) {
        for (const auto& s : sugg) {
          if (ImGui::Selectable(s.c_str(), false)) {
            std::snprintf(st.add_path, sizeof(st.add_path), "%s", s.c_str());
          }
        }
        ImGui::EndListBox();
      }
    }

    if (ImGui::Button("Add Lens")) {
      ui.show_data_lenses_window = true;
      add_json_table_view(ui, st.add_path, st.add_name);
      if (ui.request_select_json_table_view_id != 0) {
        st.selected_view_id = ui.request_select_json_table_view_id;
        ui.request_select_json_table_view_id = 0;
      }
    }
  }
  ImGui::EndChild();

  ImGui::SameLine();

  ImGui::BeginChild("##lenses_right", ImVec2(0, 0), false);
  {
    // --- Dataset discovery (procedural UI) ---
    if (ImGui::CollapsingHeader("Discover datasets (arrays of objects)", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::TextDisabled("Scans the current serialized game state and finds arrays that look like tables.");
      if (ImGui::Button("Scan now")) {
        st.discovered.clear();
        if (st.root) {
          int visited = 0;
          scan_datasets_rec(*st.root, "/", 0, 6, &visited, 90000, st.discovered);
          std::sort(st.discovered.begin(), st.discovered.end(), [](const DiscoveredDataset& a, const DiscoveredDataset& b) {
            if (a.size != b.size) return a.size > b.size;
            return a.path < b.path;
          });
        }
      }
      ImGui::SameLine();
      ImGui::TextDisabled("Found: %d", (int)st.discovered.size());

      if (!st.discovered.empty()) {
        const ImGuiTableFlags tflags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY;
        if (ImGui::BeginTable("##discover_table", 4, tflags, ImVec2(0, 180))) {
          ImGui::TableSetupScrollFreeze(0, 1);
          ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 70.0f);
          ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, 160.0f);
          ImGui::TableHeadersRow();

          ImGuiListClipper clip;
          clip.Begin((int)st.discovered.size());
          while (clip.Step()) {
            for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row) {
              const auto& d = st.discovered[(std::size_t)row];
              ImGui::TableNextRow();
              ImGui::TableSetColumnIndex(0);
              ImGui::TextUnformatted(d.path.c_str());
              ImGui::TableSetColumnIndex(1);
              ImGui::Text("%zu", d.size);
              ImGui::TableSetColumnIndex(2);
              if (d.sample_keys.empty()) {
                ImGui::TextDisabled("(keys unavailable)");
              } else {
                std::string k;
                for (std::size_t i = 0; i < d.sample_keys.size(); ++i) {
                  if (i) k += ", ";
                  k += d.sample_keys[i];
                }
                ImGui::TextUnformatted(k.c_str());
              }
              ImGui::TableSetColumnIndex(3);
              ImGui::PushID(row);
              if (ImGui::SmallButton("Create lens")) {
                ui.show_data_lenses_window = true;
                add_json_table_view(ui, d.path, default_lens_name_from_path(d.path));
                if (ui.request_select_json_table_view_id != 0) {
                  st.selected_view_id = ui.request_select_json_table_view_id;
                  ui.request_select_json_table_view_id = 0;
                }
              }
              ImGui::SameLine();
              if (ImGui::SmallButton("Go (JSON)")) {
                ui.show_json_explorer_window = true;
                ui.request_json_explorer_goto_path = d.path;
              }
              ImGui::PopID();
            }
          }

          ImGui::EndTable();
        }
      }
    }

    ImGui::Separator();

    JsonTableViewConfig* view = nullptr;
    if (st.selected_view_id != 0) view = find_view(ui, st.selected_view_id);
    if (!view && !ui.json_table_views.empty()) {
      st.selected_view_id = ui.json_table_views.front().id;
      view = &ui.json_table_views.front();
    }

    if (!view) {
      ImGui::TextDisabled("Select a lens on the left.");
      ImGui::EndChild();
      ImGui::End();
      return;
    }

    // --- Lens config ---
    {
      ImGui::Text("Lens Settings");
      ImGui::Separator();

      char name_buf[128];
      std::snprintf(name_buf, sizeof(name_buf), "%s", view->name.c_str());
      if (ImGui::InputText("Name##lens", name_buf, IM_ARRAYSIZE(name_buf))) {
        view->name = name_buf;
      }

      char path_buf[256];
      std::snprintf(path_buf, sizeof(path_buf), "%s", view->array_path.c_str());
      if (ImGui::InputTextWithHint("Dataset (JSON Pointer)", "/ships", path_buf, IM_ARRAYSIZE(path_buf))) {
        view->array_path = path_buf;
        if (!view->array_path.empty() && view->array_path[0] != '/') view->array_path = "/" + view->array_path;
        st.runtimes[view->id].cache_key.clear();
      }

      if (st.root) {
        const std::vector<std::string> sugg = nebula4x::suggest_json_pointer_completions(
            *st.root, view->array_path, 10, /*accept_root_slash=*/true, /*case_sensitive=*/false);
        if (!sugg.empty() && ImGui::BeginListBox("##dataset_sugg", ImVec2(-1, 80))) {
          for (const auto& s : sugg) {
            if (ImGui::Selectable(s.c_str(), false)) {
              view->array_path = s;
              st.runtimes[view->id].cache_key.clear();
            }
          }
          ImGui::EndListBox();
        }
      }

      ImGui::Spacing();
      ImGui::SeparatorText("Columns");

      ImGui::SetNextItemWidth(120);
      ImGui::InputInt("Sample rows", &view->sample_rows);
      view->sample_rows = std::clamp(view->sample_rows, 1, 4096);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(120);
      ImGui::InputInt("Max depth", &view->max_depth);
      view->max_depth = std::clamp(view->max_depth, 0, 6);
      ImGui::SameLine();
      ImGui::Checkbox("Include container sizes", &view->include_container_sizes);

      ImGui::SetNextItemWidth(120);
      ImGui::InputInt("Max inferred cols", &view->max_infer_columns);
      view->max_infer_columns = std::clamp(view->max_infer_columns, 4, 512);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(120);
      ImGui::InputInt("Max rows", &view->max_rows);
      view->max_rows = std::clamp(view->max_rows, 50, 500000);

      if (ImGui::Button("Infer columns")) {
        std::string err;
        if (st.root) {
          infer_columns_for_view(*st.root, *view, &err);
          st.runtimes[view->id].cache_key.clear();
        }
        if (!err.empty()) {
          nebula4x::log::warn(std::string("Data Lenses: infer failed: ") + err);
        }
      }
      ImGui::SameLine();
      if (ImGui::Button("Enable all")) {
        for (auto& c : view->columns) c.enabled = true;
        st.runtimes[view->id].cache_key.clear();
      }
      ImGui::SameLine();
      if (ImGui::Button("Disable all")) {
        for (auto& c : view->columns) c.enabled = false;
        st.runtimes[view->id].cache_key.clear();
      }

      ImGui::SameLine();
      ImGui::TextDisabled("(%d columns)", (int)view->columns.size());

      // Column toggles.
      if (ImGui::BeginChild("##cols_child", ImVec2(0, 160), true)) {
        for (std::size_t i = 0; i < view->columns.size(); ++i) {
          auto& c = view->columns[i];
          ImGui::PushID((int)i);
          bool en = c.enabled;
          if (ImGui::Checkbox("##en", &en)) {
            c.enabled = en;
            st.runtimes[view->id].cache_key.clear();
          }
          ImGui::SameLine();
          ImGui::TextUnformatted(c.label.c_str());
          ImGui::SameLine();
          ImGui::TextDisabled("%s", c.rel_path.c_str());

          if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Move up", nullptr, false, i > 0)) {
              std::swap(view->columns[i], view->columns[i - 1]);
              st.runtimes[view->id].cache_key.clear();
            }
            if (ImGui::MenuItem("Move down", nullptr, false, i + 1 < view->columns.size())) {
              std::swap(view->columns[i], view->columns[i + 1]);
              st.runtimes[view->id].cache_key.clear();
            }
            if (ImGui::MenuItem("Remove")) {
              view->columns.erase(view->columns.begin() + (std::ptrdiff_t)i);
              st.runtimes[view->id].cache_key.clear();
              ImGui::EndPopup();
              ImGui::PopID();
              break;
            }
            ImGui::EndPopup();
          }
          ImGui::PopID();
        }
      }
      ImGui::EndChild();

      ImGui::SeparatorText("Filter");
      char filter_buf[256];
      std::snprintf(filter_buf, sizeof(filter_buf), "%s", view->filter.c_str());
      if (ImGui::InputTextWithHint("##filter", "substring filter", filter_buf, IM_ARRAYSIZE(filter_buf))) {
        view->filter = filter_buf;
        st.runtimes[view->id].cache_key.clear();
      }
      ImGui::SameLine();
      if (ImGui::Checkbox("Case sensitive", &view->filter_case_sensitive)) {
        st.runtimes[view->id].cache_key.clear();
      }
      ImGui::SameLine();
      if (ImGui::Checkbox("Scan all fields", &view->filter_all_fields)) {
        st.runtimes[view->id].cache_key.clear();
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("When enabled, scans each row recursively (slower but finds nested fields).\n"
                          "When disabled, searches only the currently enabled columns.");
      }
    }

    ImGui::Separator();

    // --- Data table ---
    if (!st.doc_loaded) {
      ImGui::TextDisabled("Document not loaded.");
      ImGui::EndChild();
      ImGui::End();
      return;
    }

    std::string err;
    const auto* dataset_node = st.root
                                 ? nebula4x::resolve_json_pointer(*st.root, view->array_path,
                                                                /*accept_root_slash=*/true, &err)
                                 : nullptr;
    if (!dataset_node) {
      ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "Resolve error: %s", err.c_str());
      ImGui::EndChild();
      ImGui::End();
      return;
    }

    const auto* arr = dataset_node->as_array();
    if (!arr) {
      ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "Dataset is a %s (expected array).", value_type_name(*dataset_node).c_str());
      ImGui::TextDisabled("Tip: right-click an array in JSON Explorer and choose 'Create Data Lens'.");
      ImGui::EndChild();
      ImGui::End();
      return;
    }

    // Auto-infer columns once for empty configs (nice UX when created procedurally).
    if (view->columns.empty()) {
      std::string e2;
      if (st.root) infer_columns_for_view(*st.root, *view, &e2);
    }

    // Compute list of enabled columns.
    std::vector<const JsonTableColumnConfig*> enabled_cols;
    enabled_cols.reserve(view->columns.size());
    for (const auto& c : view->columns) {
      if (c.enabled) enabled_cols.push_back(&c);
    }

    // Table flags.
    const ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingFixedFit;

    const int cols_total = 1 + (int)enabled_cols.size();
    const float table_h = ImGui::GetContentRegionAvail().y;

    // Sorting state (per-view, not persisted).
    static std::unordered_map<std::uint64_t, std::string> sort_rel;
    static std::unordered_map<std::uint64_t, bool> sort_asc;
    if (sort_rel.find(view->id) == sort_rel.end()) {
      sort_rel[view->id] = "";
      sort_asc[view->id] = true;
    }

    if (ImGui::BeginTable("##lens_table", cols_total, flags, ImVec2(0, table_h))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_DefaultSort, 58.0f, 0);
      for (int i = 0; i < (int)enabled_cols.size(); ++i) {
        ImGui::TableSetupColumn(enabled_cols[(std::size_t)i]->label.c_str(), ImGuiTableColumnFlags_None, 160.0f, (ImGuiID)(i + 1));
      }
      ImGui::TableHeadersRow();

      // React to ImGui sort changes.
      if (ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs()) {
        if (ss->SpecsDirty && ss->SpecsCount > 0) {
          const ImGuiTableColumnSortSpecs& s0 = ss->Specs[0];
          const int col_idx = s0.ColumnIndex;
          if (col_idx == 0) {
            sort_rel[view->id].clear();
          } else {
            const int enabled_idx = col_idx - 1;
            if (enabled_idx >= 0 && enabled_idx < (int)enabled_cols.size()) {
              sort_rel[view->id] = enabled_cols[(std::size_t)enabled_idx]->rel_path;
            }
          }
          sort_asc[view->id] = (s0.SortDirection == ImGuiSortDirection_Ascending);
          st.runtimes[view->id].cache_key.clear();
          ss->SpecsDirty = false;
        }
      }

      ViewRuntime& rt = st.runtimes[view->id];
      const std::string cache_key = build_view_cache_key(st, *view, sort_rel[view->id], sort_asc[view->id]);
      if (rt.cache_key != cache_key) {
        rt.cache_key = cache_key;
        rt.rows.clear();

        const std::size_t n = arr->size();
        const std::size_t limit = std::min<std::size_t>(n, static_cast<std::size_t>(view->max_rows));
        rt.rows.reserve(limit);

        const std::string filter = view->filter;
        for (std::size_t i = 0; i < limit; ++i) {
          const auto& rowv = (*arr)[i];
          if (!filter.empty()) {
            bool match = false;
            if (view->filter_all_fields) {
              int visited = 0;
              match = node_contains_text(rowv, filter, view->filter_case_sensitive, &visited, 2200);
            } else {
              for (const auto* c : enabled_cols) {
                std::string rerr;
                const auto* cell = nebula4x::resolve_json_pointer(rowv, c->rel_path, true, &rerr);
                if (cell) {
                  const std::string pv = preview_value(*cell, 512);
                  if (contains_substring(pv, filter, view->filter_case_sensitive)) {
                    match = true;
                    break;
                  }
                }
              }
            }
            if (!match) continue;
          }
          rt.rows.push_back((int)i);
        }

        // Apply sort if requested.
        if (!sort_rel[view->id].empty()) {
          std::vector<std::pair<int, SortValue>> tmp;
          tmp.reserve(rt.rows.size());
          for (int idx : rt.rows) {
            const auto& rowv = (*arr)[(std::size_t)idx];
            std::string rerr;
            const auto* cell = nebula4x::resolve_json_pointer(rowv, sort_rel[view->id], true, &rerr);
            tmp.emplace_back(idx, make_sort_value(cell));
          }
          std::stable_sort(tmp.begin(), tmp.end(), [&](const auto& a, const auto& b) {
            const int c = compare_sort_value(a.second, b.second);
            return sort_asc[view->id] ? (c < 0) : (c > 0);
          });
          rt.rows.clear();
          rt.rows.reserve(tmp.size());
          for (auto& p : tmp) rt.rows.push_back(p.first);
        }
      }

      // Draw rows.
      ImGuiListClipper clip;
      clip.Begin((int)rt.rows.size());
      while (clip.Step()) {
        for (int ridx = clip.DisplayStart; ridx < clip.DisplayEnd; ++ridx) {
          const int row_index = rt.rows[(std::size_t)ridx];
          const auto& rowv = (*arr)[(std::size_t)row_index];
          const std::string element_path = nebula4x::json_pointer_join_index(view->array_path, (std::size_t)row_index);

          ImGui::TableNextRow();
          ImGui::PushID(row_index);

          // Row index column with row-level context.
          ImGui::TableSetColumnIndex(0);
          char idx_buf[32];
          std::snprintf(idx_buf, sizeof(idx_buf), "%d", row_index);
          ImGuiSelectableFlags sflags = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick;
          const bool clicked = ImGui::Selectable(idx_buf, false, sflags);
          if (clicked && ImGui::IsMouseDoubleClicked(0)) {
            ui.show_json_explorer_window = true;
            ui.request_json_explorer_goto_path = element_path;
          }
          if (ImGui::BeginPopupContextItem("##row_ctx")) {
            ImGui::TextDisabled("%s", element_path.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Go to in JSON Explorer")) {
              ui.show_json_explorer_window = true;
              ui.request_json_explorer_goto_path = element_path;
            }
            if (ImGui::MenuItem("Copy pointer")) {
              ImGui::SetClipboardText(element_path.c_str());
            }
            if (ImGui::MenuItem("Copy JSON")) {
              const std::string js = nebula4x::json::stringify(rowv, 2);
              ImGui::SetClipboardText(js.c_str());
            }
            if (ImGui::MenuItem("Pin row (size/value)")) {
              const bool track = rowv.is_number() || rowv.is_bool() || rowv.is_array() || rowv.is_object();
              add_watch_item(ui, element_path, view->name + "[" + std::to_string(row_index) + "]", track, true);
              ui.show_watchboard_window = true;
            }
            ImGui::EndPopup();
          }

          // Data cells.
          for (int ci = 0; ci < (int)enabled_cols.size(); ++ci) {
            const auto* col = enabled_cols[(std::size_t)ci];
            ImGui::TableSetColumnIndex(ci + 1);
            ImGui::PushID(ci);
            std::string rerr;
            const auto* cell = nebula4x::resolve_json_pointer(rowv, col->rel_path, true, &rerr);
            const std::string pv = cell ? preview_value(*cell) : "(missing)";

            // If this looks like an entity id, render as a clickable link with a resolved name.
            const GameEntityIndexEntry* ent = nullptr;
            std::uint64_t ent_id = 0;
            if (cell && json_to_u64_id(*cell, ent_id)) {
              // Heuristic: only "linkify" id-ish columns to avoid accidental collisions.
              std::string last_tok;
              {
                const auto toks = nebula4x::split_json_pointer(col->rel_path, true);
                if (!toks.empty()) last_tok = nebula4x::json_pointer_unescape_token(toks.back());
              }
              const bool idish = (!last_tok.empty() && (last_tok == "id" ||
                                                       (last_tok.size() >= 3 && last_tok.substr(last_tok.size() - 3) == "_id") ||
                                                       (last_tok.size() >= 4 && last_tok.substr(last_tok.size() - 4) == "_ids"))) ||
                                 (!col->label.empty() && (col->label == "id" ||
                                                        (col->label.size() >= 3 && col->label.substr(col->label.size() - 3) == "_id") ||
                                                        (col->label.size() >= 4 && col->label.substr(col->label.size() - 4) == "_ids")));
              if (idish) {
                ent = find_game_entity(ent_id);
              }
            }

            if (ent) {
              const std::string disp = ent->name.empty() ? pv : ent->name;
              ImGui::Selectable(disp.c_str(), false, ImGuiSelectableFlags_SpanAllColumns);
              if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextDisabled("%s", ("id=" + std::to_string(ent->id)).c_str());
                ImGui::TextDisabled("%s", ("kind=" + ent->kind).c_str());
                ImGui::Separator();
                ImGui::TextDisabled("%s", ent->path.c_str());
                ImGui::TextUnformatted("Click to open referenced entity in JSON Explorer");
                ImGui::EndTooltip();
              }
              if (ImGui::IsItemClicked()) {
                ui.show_json_explorer_window = true;
                ui.request_json_explorer_goto_path = ent->path;
              }
            } else {
              ImGui::TextUnformatted(pv.c_str());
              if (ImGui::IsItemHovered() && cell) {
                ImGui::BeginTooltip();
                ImGui::TextDisabled("%s", (element_path + (col->rel_path == "/" ? "" : col->rel_path)).c_str());
                ImGui::Separator();
                ImGui::TextUnformatted(preview_value(*cell, 2048).c_str());
                ImGui::EndTooltip();
              }
            }

            if (ImGui::BeginPopupContextItem("##cell_ctx")) {
              const std::string cell_path = element_path + ((col->rel_path == "/" || col->rel_path.empty()) ? "" : col->rel_path);

              ImGui::TextDisabled("%s", cell_path.c_str());
              ImGui::Separator();
              if (ImGui::MenuItem("Go to in JSON Explorer")) {
                ui.show_json_explorer_window = true;
                ui.request_json_explorer_goto_path = cell_path;
              }
              if (ent) {
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
                ImGui::Separator();
              }
              if (ImGui::MenuItem("Copy pointer")) {
                ImGui::SetClipboardText(cell_path.c_str());
              }
              if (ImGui::MenuItem("Copy value")) {
                ImGui::SetClipboardText(pv.c_str());
              }
              if (cell && ImGui::MenuItem("Copy JSON")) {
                const std::string js = nebula4x::json::stringify(*cell, 2);
                ImGui::SetClipboardText(js.c_str());
              }
              if (ImGui::MenuItem("Pin to Watchboard")) {
                const bool track = cell && (cell->is_number() || cell->is_bool() || cell->is_array() || cell->is_object());
                add_watch_item(ui, cell_path, view->name + "." + col->label, track, true);
                ui.show_watchboard_window = true;
              }
              if (cell && cell->is_array() && is_array_of_objects(*cell) && ImGui::MenuItem("Create lens from this array")) {
                add_json_table_view(ui, cell_path, default_lens_name_from_path(cell_path));
                ui.show_data_lenses_window = true;
                if (ui.request_select_json_table_view_id != 0) {
                  st.selected_view_id = ui.request_select_json_table_view_id;
                  ui.request_select_json_table_view_id = 0;
                }
              }
              ImGui::EndPopup();
            }
            ImGui::PopID();
          }

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
