#include "ui/json_explorer_window.h"
#include "ui/watchboard_window.h"
#include "ui/data_lenses_window.h"
#include "ui/dashboards_window.h"
#include "ui/pivot_tables_window.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "nebula4x/util/autosave.h"
#include "nebula4x/util/file_io.h"
#include "nebula4x/util/json.h"
#include "nebula4x/util/json_pointer.h"
#include "nebula4x/util/json_pointer_autocomplete.h"
#include "nebula4x/util/log.h"

#include "ui/game_entity_index.h"
#include "ui/game_json_cache.h"

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

const char* json_type_name(const nebula4x::json::Value& v) {
  if (v.is_object()) return "object";
  if (v.is_array()) return "array";
  if (v.is_string()) return "string";
  if (v.is_number()) return "number";
  if (v.is_bool()) return "bool";
  if (v.is_null()) return "null";
  return "unknown";
}

std::string format_number(double x) {
  char buf[64];
  // Compact but stable-ish representation.
  std::snprintf(buf, sizeof(buf), "%.6g", x);
  return std::string(buf);
}

std::string json_scalar_preview(const nebula4x::json::Value& v, std::size_t max_len) {
  if (v.is_null()) return "null";
  if (v.is_bool()) return v.bool_value() ? "true" : "false";
  if (v.is_number()) return format_number(v.number_value());
  if (v.is_string()) {
    std::string s = v.string_value();
    if (s.size() > max_len) {
      s.resize(max_len);
      s += "...";
    }
    return '"' + s + '"';
  }
  return "";
}

std::string json_node_preview(const nebula4x::json::Value& v, std::size_t max_len = 64) {
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
  return json_scalar_preview(v, max_len);
}

struct FilterConfig {
  std::string needle;
  bool match_keys{true};
  bool match_values{true};
  bool case_sensitive{false};
  int max_nodes{50000};
};

struct FilterScan {
  std::unordered_set<std::string> visible_paths;
  std::unordered_set<std::string> open_paths;
  std::unordered_set<std::string> self_match_paths;
  int scanned{0};
  bool truncated{false};
};

bool node_self_matches(const std::string& key_label, const nebula4x::json::Value& v, const FilterConfig& cfg) {
  if (cfg.needle.empty()) return true;
  bool ok = false;
  if (cfg.match_keys && contains_text(key_label, cfg.needle, cfg.case_sensitive)) ok = true;
  if (!ok && cfg.match_values) {
    if (v.is_string()) {
      ok = contains_text(v.string_value(), cfg.needle, cfg.case_sensitive);
    } else if (v.is_number()) {
      ok = contains_text(format_number(v.number_value()), cfg.needle, cfg.case_sensitive);
    } else if (v.is_bool()) {
      ok = contains_text(v.bool_value() ? "true" : "false", cfg.needle, cfg.case_sensitive);
    } else if (v.is_null()) {
      ok = contains_text("null", cfg.needle, cfg.case_sensitive);
    } else {
      // For containers, also allow matching the type/name.
      ok = contains_text(json_type_name(v), cfg.needle, cfg.case_sensitive);
    }
  }
  return ok;
}

bool scan_filter_tree(const nebula4x::json::Value& v, const std::string& path, const std::string& key_label,
                      const FilterConfig& cfg, FilterScan& out) {
  if (out.truncated) return false;
  if (out.scanned++ > cfg.max_nodes) {
    out.truncated = true;
    return false;
  }

  const bool self_match = node_self_matches(key_label, v, cfg);
  bool any_child_match = false;

  if (v.is_object()) {
    const auto* o = v.as_object();
    if (o) {
      for (const auto& kv : *o) {
        const std::string child_path = nebula4x::json_pointer_join(path, kv.first);
        if (scan_filter_tree(kv.second, child_path, kv.first, cfg, out)) {
          any_child_match = true;
        }
        if (out.truncated) break;
      }
    }
  } else if (v.is_array()) {
    const auto* a = v.as_array();
    if (a) {
      for (std::size_t i = 0; i < a->size(); ++i) {
        const std::string idx_label = "[" + std::to_string(i) + "]";
        const std::string child_path = nebula4x::json_pointer_join_index(path, i);
        if (scan_filter_tree((*a)[i], child_path, idx_label, cfg, out)) {
          any_child_match = true;
        }
        if (out.truncated) break;
      }
    }
  }

  const bool keep = self_match || any_child_match;
  if (keep) {
    out.visible_paths.insert(path);
    if (any_child_match) out.open_paths.insert(path);
    if (self_match) out.self_match_paths.insert(path);
  }
  return keep;
}

struct JsonExplorerState {
  bool initialized{false};

  // Data source: 0=current state, 1=file, 2=autosave
  int source_tab{0};

  // Live refresh.
  bool auto_refresh{false};
  float refresh_sec{1.0f};
  double last_refresh_time{0.0};

  // Inputs.
  char file_path[256] = "";
  char goto_path[256] = "/";

  // Filter.
  char filter_text[128] = "";
  bool filter_keys{true};
  bool filter_values{true};
  bool filter_case_sensitive{false};
  int filter_max_nodes{50000};

  // Document.
  bool doc_loaded{false};
  std::string doc_source;
  std::string doc_error;
  std::shared_ptr<const nebula4x::json::Value> root;

  // Selection.
  std::string selected_path{"/"};
  bool request_scroll_to_selected{false};
  std::unordered_set<std::string> goto_open_paths;
  bool goto_open_pending{false};

  // Filter cache.
  bool filter_cache_valid{false};
  std::string filter_cache_key;
  FilterScan filter_scan;

  // Table view.
  int table_max_rows{300};
  int table_sample_elems{64};
  int table_max_cols{12};
};

bool load_json_document_from_text(JsonExplorerState& st, std::string source, const std::string& text) {
  try {
    st.root = std::make_shared<nebula4x::json::Value>(nebula4x::json::parse(text));
    st.doc_loaded = (bool)st.root;
    st.doc_source = std::move(source);
    st.doc_error.clear();

    // Reset selection if it no longer resolves.
    std::string err;
    if (!st.root || !nebula4x::resolve_json_pointer(*st.root, st.selected_path, true, &err)) {
      st.selected_path = "/";
    }

    st.filter_cache_valid = false;
    return true;
  } catch (const std::exception& e) {
    st.doc_loaded = false;
    st.doc_source = std::move(source);
    st.doc_error = e.what();
    st.filter_cache_valid = false;
    return false;
  }
}

bool load_from_current_state(JsonExplorerState& st, Simulation& sim, bool force) {
  const double now = ImGui::GetTime();
  ensure_game_json_cache(sim, now, st.refresh_sec, force);
  const auto& cache = game_json_cache();

  st.root = cache.root;
  st.doc_loaded = (bool)st.root;
  st.doc_source = "Current game state";
  st.doc_error = cache.error;

  // Reset selection if it no longer resolves.
  std::string err;
  if (!st.root || !nebula4x::resolve_json_pointer(*st.root, st.selected_path, true, &err)) {
    st.selected_path = "/";
  }

  st.filter_cache_valid = false;
  return st.doc_loaded;
}

bool load_from_file_path(JsonExplorerState& st, const char* path) {
  if (!path || path[0] == '\0') {
    st.doc_loaded = false;
    st.doc_source = "File";
    st.doc_error = "Path is empty.";
    st.filter_cache_valid = false;
    return false;
  }
  try {
    const std::string text = nebula4x::read_text_file(path);
    return load_json_document_from_text(st, std::string("File: ") + path, text);
  } catch (const std::exception& e) {
    st.doc_loaded = false;
    st.doc_source = std::string("File: ") + path;
    st.doc_error = e.what();
    st.filter_cache_valid = false;
    return false;
  }
}

void ensure_filter_cache(JsonExplorerState& st) {
  const std::string needle = st.filter_text;
  const std::string cache_key = needle + "|k" + (st.filter_keys ? "1" : "0") + "|v" + (st.filter_values ? "1" : "0") +
                                "|cs" + (st.filter_case_sensitive ? "1" : "0") + "|mx" +
                                std::to_string(st.filter_max_nodes);

  if (st.filter_cache_valid && st.filter_cache_key == cache_key) return;

  st.filter_cache_valid = true;
  st.filter_cache_key = cache_key;
  st.filter_scan = FilterScan{};

  if (!st.doc_loaded) return;
  if (needle.empty()) return;

  FilterConfig cfg;
  cfg.needle = needle;
  cfg.match_keys = st.filter_keys;
  cfg.match_values = st.filter_values;
  cfg.case_sensitive = st.filter_case_sensitive;
  cfg.max_nodes = st.filter_max_nodes;

  // Always include root.
  st.filter_scan.visible_paths.insert("/");

  scan_filter_tree(*st.root, "/", "(root)", cfg, st.filter_scan);
  st.filter_scan.visible_paths.insert("/");
}

void build_goto_open_paths(JsonExplorerState& st, const std::string& path) {
  st.goto_open_paths.clear();
  st.goto_open_pending = false;

  try {
    const auto tokens = nebula4x::split_json_pointer(path, true);
    std::string cur = "/";
    st.goto_open_paths.insert(cur);
    for (const auto& t : tokens) {
      // Tokens are already unescaped.
      if (!t.empty() && t.front() == '[' && t.back() == ']') {
        // UI label form; not a real token. Skip.
        continue;
      }
      // Detect index-like tokens.
      std::size_t idx = 0;
      if (nebula4x::parse_json_pointer_index(t, idx)) {
        cur = nebula4x::json_pointer_join_index(cur, idx);
      } else {
        cur = nebula4x::json_pointer_join(cur, t);
      }
      st.goto_open_paths.insert(cur);
    }
    st.goto_open_pending = true;
  } catch (...) {
    // ignore
  }
}

struct TreeDrawContext {
  JsonExplorerState& st;
  UIState& ui;
  bool filter_active{false};
};

bool path_visible(const TreeDrawContext& ctx, const std::string& path) {
  if (!ctx.filter_active) return true;
  return ctx.st.filter_scan.visible_paths.find(path) != ctx.st.filter_scan.visible_paths.end();
}

bool path_self_match(const TreeDrawContext& ctx, const std::string& path) {
  if (!ctx.filter_active) return false;
  return ctx.st.filter_scan.self_match_paths.find(path) != ctx.st.filter_scan.self_match_paths.end();
}

bool path_force_open(const TreeDrawContext& ctx, const std::string& path) {
  if (ctx.st.goto_open_pending && ctx.st.goto_open_paths.find(path) != ctx.st.goto_open_paths.end()) return true;
  if (ctx.filter_active) {
    return ctx.st.filter_scan.open_paths.find(path) != ctx.st.filter_scan.open_paths.end();
  }
  return false;
}

void draw_json_tree_node(const nebula4x::json::Value& v, const std::string& path, const std::string& label,
                         TreeDrawContext& ctx);

void draw_object_children(const nebula4x::json::Object& o, const std::string& path, TreeDrawContext& ctx) {
  // Sort keys for determinism.
  std::vector<std::string> keys;
  keys.reserve(o.size());
  for (const auto& kv : o) keys.push_back(kv.first);
  std::sort(keys.begin(), keys.end());

  if (keys.size() > 250) {
    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(keys.size()));
    while (clip.Step()) {
      for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
        const std::string& k = keys[(std::size_t)i];
        const auto it = o.find(k);
        if (it == o.end()) continue;
        const std::string child_path = nebula4x::json_pointer_join(path, k);
        draw_json_tree_node(it->second, child_path, k, ctx);
      }
    }
  } else {
    for (const auto& k : keys) {
      const auto it = o.find(k);
      if (it == o.end()) continue;
      const std::string child_path = nebula4x::json_pointer_join(path, k);
      draw_json_tree_node(it->second, child_path, k, ctx);
    }
  }
}

void draw_array_children(const nebula4x::json::Array& a, const std::string& path, TreeDrawContext& ctx) {
  const int n = static_cast<int>(a.size());
  if (n > 400) {
    ImGuiListClipper clip;
    clip.Begin(n);
    while (clip.Step()) {
      for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
        const std::string idx_label = "[" + std::to_string(i) + "]";
        const std::string child_path = nebula4x::json_pointer_join_index(path, (std::size_t)i);
        draw_json_tree_node(a[(std::size_t)i], child_path, idx_label, ctx);
      }
    }
  } else {
    for (int i = 0; i < n; ++i) {
      const std::string idx_label = "[" + std::to_string(i) + "]";
      const std::string child_path = nebula4x::json_pointer_join_index(path, (std::size_t)i);
      draw_json_tree_node(a[(std::size_t)i], child_path, idx_label, ctx);
    }
  }
}

void draw_json_tree_node(const nebula4x::json::Value& v, const std::string& path, const std::string& label,
                         TreeDrawContext& ctx) {
  if (!path_visible(ctx, path)) return;

  const bool is_selected = (ctx.st.selected_path == path);
  const bool self_match = path_self_match(ctx, path);

  std::string disp = label;
  if (self_match && ctx.filter_active) disp += "  *";

  const std::string preview = json_node_preview(v);
  const std::string line = disp + "  " + preview + "##" + path;

  ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_OpenOnArrow |
                             ImGuiTreeNodeFlags_OpenOnDoubleClick;
  if (is_selected) flags |= ImGuiTreeNodeFlags_Selected;

  const bool has_children = v.is_object() || v.is_array();
  if (!has_children) {
    flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
  }

  if (path_force_open(ctx, path)) {
    // While filtering, keep matching ancestors open.
    const ImGuiCond cond = ctx.filter_active ? ImGuiCond_Always : ImGuiCond_Once;
    ImGui::SetNextItemOpen(true, cond);
  }

  const bool open = ImGui::TreeNodeEx(line.c_str(), flags);

  // If a caller requested that we scroll a selection into view, do it when we draw the selected node.
  if (is_selected && ctx.st.request_scroll_to_selected) {
    ImGui::SetScrollHereY(0.10f);
    ctx.st.request_scroll_to_selected = false;
  }

  if (ImGui::IsItemClicked()) {
    ctx.st.selected_path = path;
    ctx.st.request_scroll_to_selected = true;
  }

  // Context menu: copy/pin.
  if (ImGui::BeginPopupContextItem()) {
    if (ImGui::MenuItem("Copy JSON Pointer")) {
      ImGui::SetClipboardText(path.c_str());
    }
    if (ImGui::MenuItem("Pin to Watchboard (JSON Pins)")) {
      ctx.ui.show_watchboard_window = true;
      (void)add_watch_item(ctx.ui, path, label);
    }
    if (ctx.st.doc_source == "Current game state") {
      std::uint64_t ent_id = 0;
      if (json_to_u64_id(v, ent_id)) {
        if (const auto* ent = find_game_entity(ent_id)) {
          ImGui::Separator();
          std::string elabel = ent->kind + " #" + std::to_string(ent->id);
          if (!ent->name.empty()) elabel += "  " + ent->name;
          ImGui::TextDisabled("Entity: %s", elabel.c_str());
          if (ImGui::MenuItem("Go to referenced entity")) {
            ctx.ui.show_json_explorer_window = true;
            ctx.ui.request_json_explorer_goto_path = ent->path;
          }
          if (ImGui::MenuItem("Open in Entity Inspector")) {
            ctx.ui.show_entity_inspector_window = true;
            ctx.ui.entity_inspector_id = ent->id;
          }
          if (ImGui::MenuItem("Open in Reference Graph")) {
            ctx.ui.show_reference_graph_window = true;
            ctx.ui.reference_graph_focus_id = ent->id;
          }
          if (ImGui::MenuItem("Copy referenced entity path")) {
            ImGui::SetClipboardText(ent->path.c_str());
          }
        }
      }
    }
    if (v.is_array()) {
      if (ImGui::MenuItem("Create Data Lens (Procedural Table)")) {
        ctx.ui.show_data_lenses_window = true;
        (void)add_json_table_view(ctx.ui, path, label);
      }
      if (ImGui::MenuItem("Create Dashboard (Procedural Charts)")) {
        ctx.ui.show_dashboards_window = true;
        (void)add_json_dashboard_for_path(ctx.ui, path, label + " Dashboard");
      }
      if (ImGui::MenuItem("Create Pivot Table (Procedural Aggregations)")) {
        ctx.ui.show_pivot_tables_window = true;
        (void)add_json_pivot_for_path(ctx.ui, path, label + " Pivot");
      }
    }
    if (ImGui::MenuItem("Copy preview")) {
      ImGui::SetClipboardText(preview.c_str());
    }
    ImGui::EndPopup();
  }

  if (!has_children) {
    return;
  }

  if (open) {
    if (v.is_object()) {
      const auto* o = v.as_object();
      if (o) draw_object_children(*o, path, ctx);
    } else if (v.is_array()) {
      const auto* a = v.as_array();
      if (a) draw_array_children(*a, path, ctx);
    }
    ImGui::TreePop();
  }
}

void draw_breadcrumbs(JsonExplorerState& st) {
  ImGui::TextDisabled("Breadcrumbs:");
  ImGui::SameLine();

  // Root
  if (ImGui::SmallButton("/##crumb_root")) {
    st.selected_path = "/";
    st.request_scroll_to_selected = true;
    std::snprintf(st.goto_path, sizeof(st.goto_path), "%s", "/");
  }

  try {
    const auto tokens = nebula4x::split_json_pointer(st.selected_path, true);
    std::string cur = "/";
    for (const auto& t : tokens) {
      ImGui::SameLine();
      ImGui::TextDisabled("/");
      ImGui::SameLine();

      std::size_t idx = 0;
      const bool is_index = nebula4x::parse_json_pointer_index(t, idx);
      cur = is_index ? nebula4x::json_pointer_join_index(cur, idx) : nebula4x::json_pointer_join(cur, t);

      std::string lbl = is_index ? ("[" + t + "]") : t;
      const std::string id = "##crumb_" + cur;
      lbl += id;
      if (ImGui::SmallButton(lbl.c_str())) {
        st.selected_path = cur;
        st.request_scroll_to_selected = true;
        std::snprintf(st.goto_path, sizeof(st.goto_path), "%s", cur.c_str());
      }
    }
  } catch (...) {
    // ignore
  }
}

void draw_selected_details(JsonExplorerState& st, UIState& ui) {
  if (!st.doc_loaded) return;

  std::string err;
  const auto* v = st.root ? nebula4x::resolve_json_pointer(*st.root, st.selected_path, true, &err) : nullptr;
  if (!v) {
    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Selection error: %s", err.c_str());
    return;
  }

  ImGui::Text("Path: %s", st.selected_path.c_str());
  ImGui::SameLine();
  if (ImGui::SmallButton("Copy path")) {
    ImGui::SetClipboardText(st.selected_path.c_str());
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Pin")) {
    ui.show_watchboard_window = true;
    (void)add_watch_item(ui, st.selected_path, "");
  }
  ImGui::SameLine();
  if (v->is_array()) {
    if (ImGui::SmallButton("Lens")) {
      ui.show_data_lenses_window = true;
      (void)add_json_table_view(ui, st.selected_path, "");
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Create a procedural table view from this array");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Dash")) {
      ui.show_dashboards_window = true;
      (void)add_json_dashboard_for_path(ui, st.selected_path, "");
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Create procedural charts/widgets from this array");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Pivot")) {
      ui.show_pivot_tables_window = true;
      (void)add_json_pivot_for_path(ui, st.selected_path, "");
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Create a pivot table (group-by aggregations) from this array");
    }
  }

  ImGui::Text("Type: %s", json_type_name(*v));

  if (v->is_object()) {
    const auto* o = v->as_object();
    ImGui::Text("Members: %zu", o ? o->size() : 0);
  } else if (v->is_array()) {
    const auto* a = v->as_array();
    ImGui::Text("Elements: %zu", a ? a->size() : 0);
  } else if (v->is_string()) {
    ImGui::Text("Length: %zu", v->string_value().size());
  }

  const std::string preview = json_scalar_preview(*v, 200);
  if (!preview.empty()) {
    ImGui::TextWrapped("Value: %s", preview.c_str());
  }

  // If this scalar looks like an entity id (live game state), offer navigation.
  if (st.doc_source == "Current game state") {
    std::uint64_t ent_id = 0;
    if (v && json_to_u64_id(*v, ent_id)) {
      if (const auto* ent = find_game_entity(ent_id)) {
        ImGui::Separator();
        std::string elabel = ent->kind + " #" + std::to_string(ent->id);
        if (!ent->name.empty()) elabel += "  " + ent->name;
        ImGui::TextDisabled("Referenced entity");
        ImGui::TextUnformatted(elabel.c_str());
        if (ImGui::SmallButton("Go to entity")) {
          ui.request_json_explorer_goto_path = ent->path;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Open Entity Inspector")) {
          ui.show_entity_inspector_window = true;
          ui.entity_inspector_id = ent->id;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Open Reference Graph")) {
          ui.show_reference_graph_window = true;
          ui.reference_graph_focus_id = ent->id;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy entity path")) {
          ImGui::SetClipboardText(ent->path.c_str());
        }
      }
    }
  }

  if (ImGui::SmallButton("Copy value (JSON)")) {
    const std::string txt = nebula4x::json::stringify(*v, 2);
    ImGui::SetClipboardText(txt.c_str());
  }

  ImGui::Separator();

  // Pretty JSON view.
  ImGui::TextDisabled("JSON");
  const std::string pretty = nebula4x::json::stringify(*v, 2);
  ImGui::BeginChild("##json_value", ImVec2(0, 140), true);
  ImGui::TextUnformatted(pretty.c_str());
  ImGui::EndChild();

  // Procedural table view for arrays of objects.
  if (v->is_array()) {
    const auto* a = v->as_array();
    if (a && !a->empty()) {
      bool any_object = false;
      for (std::size_t i = 0; i < a->size() && i < 8; ++i) {
        if ((*a)[i].is_object()) {
          any_object = true;
          break;
        }
      }

      if (any_object) {
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Array Table (procedural)", ImGuiTreeNodeFlags_DefaultOpen)) {
          ImGui::SliderInt("Max rows", &st.table_max_rows, 10, 2000);
          st.table_max_rows = std::clamp(st.table_max_rows, 1, 500000);
          ImGui::SliderInt("Sample elems", &st.table_sample_elems, 1, 512);
          st.table_sample_elems = std::clamp(st.table_sample_elems, 1, 8192);
          ImGui::SliderInt("Max columns", &st.table_max_cols, 3, 64);
          st.table_max_cols = std::clamp(st.table_max_cols, 1, 512);

          // Infer schema.
          std::unordered_set<std::string> key_set;
          const int sample = std::min<int>(static_cast<int>(a->size()), st.table_sample_elems);
          for (int i = 0; i < sample; ++i) {
            if (!(*a)[(std::size_t)i].is_object()) continue;
            const auto* o = (*a)[(std::size_t)i].as_object();
            if (!o) continue;
            for (const auto& kv : *o) key_set.insert(kv.first);
          }
          std::vector<std::string> keys;
          keys.reserve(key_set.size());
          for (auto& k : key_set) keys.push_back(std::move(k));
          std::sort(keys.begin(), keys.end());
          if ((int)keys.size() > st.table_max_cols) keys.resize((std::size_t)st.table_max_cols);

          if (keys.empty()) {
            ImGui::TextDisabled("(no object keys detected)");
            return;
          }

          const int rows = std::min<int>(static_cast<int>(a->size()), st.table_max_rows);

          ImGuiTableFlags tf = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                               ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY;

          const float table_h = std::min(260.0f, ImGui::GetContentRegionAvail().y);
          if (ImGui::BeginTable("##json_array_table", static_cast<int>(keys.size()) + 1, tf, ImVec2(0, table_h))) {
            ImGui::TableSetupScrollFreeze(1, 1);
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 48.0f);
            for (const auto& k : keys) {
              ImGui::TableSetupColumn(k.c_str());
            }
            ImGui::TableHeadersRow();

            ImGuiListClipper clip;
            clip.Begin(rows);
            while (clip.Step()) {
              for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                ImGui::TableNextRow();

                // Index column.
                ImGui::TableSetColumnIndex(0);
                char idx_buf[32];
                std::snprintf(idx_buf, sizeof(idx_buf), "%d", r);
                const std::string row_id = std::string("##row_") + std::to_string(r);
                if (ImGui::Selectable((idx_buf + row_id).c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                  // Jump to that element.
                  const std::string el_path = nebula4x::json_pointer_join_index(st.selected_path, (std::size_t)r);
                  st.selected_path = el_path;
                  st.request_scroll_to_selected = true;
                  std::snprintf(st.goto_path, sizeof(st.goto_path), "%s", el_path.c_str());
                  build_goto_open_paths(st, el_path);
                }

                const auto& elem = (*a)[(std::size_t)r];
                const auto* o = elem.as_object();

                for (int c = 0; c < (int)keys.size(); ++c) {
                  ImGui::TableSetColumnIndex(c + 1);
                  if (!o) {
                    ImGui::TextDisabled("-");
                    continue;
                  }
                  auto it = o->find(keys[(std::size_t)c]);
                  if (it == o->end()) {
                    ImGui::TextDisabled("-");
                    continue;
                  }

                  const auto& cell = it->second;
                  const std::string cell_txt = json_node_preview(cell, 32);
                  ImGui::TextUnformatted(cell_txt.c_str());
                }
              }
            }

            ImGui::EndTable();
          }
        }
      }
    }
  }
}

} // namespace

void draw_json_explorer_window(Simulation& sim, UIState& ui) {
  if (!ui.show_json_explorer_window) return;

  static JsonExplorerState st;
  static bool was_open = false;
  const bool just_opened = !was_open;
  was_open = ui.show_json_explorer_window;

  if (just_opened && !st.initialized) {
    st.initialized = true;
    st.source_tab = 0;
    st.selected_path = "/";
    std::snprintf(st.goto_path, sizeof(st.goto_path), "%s", "/");
    (void)load_from_current_state(st, sim, /*force=*/true);
  }

  // One-shot external navigation request (e.g. from Watchboard).
  if (!ui.request_json_explorer_goto_path.empty()) {
    const std::string req = ui.request_json_explorer_goto_path;
    ui.request_json_explorer_goto_path.clear();

    // Ensure the document matches the current state when navigating from other tools.
    st.source_tab = 0;
    (void)load_from_current_state(st, sim, /*force=*/true);

    st.selected_path = req;
    std::snprintf(st.goto_path, sizeof(st.goto_path), "%s", req.c_str());
    build_goto_open_paths(st, req);
    st.request_scroll_to_selected = true;
  }

  ImGui::SetNextWindowSize(ImVec2(980, 720), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("JSON Explorer", &ui.show_json_explorer_window)) {
    ImGui::End();
    return;
  }

  // --- Source controls ---
  if (ImGui::BeginTabBar("##json_source_tabs")) {
    if (ImGui::BeginTabItem("Current State")) {
      st.source_tab = 0;

      if (ImGui::Button("Refresh")) {
        st.last_refresh_time = ImGui::GetTime();
        (void)load_from_current_state(st, sim, /*force=*/true);
      }
      ImGui::SameLine();
      ImGui::Checkbox("Auto refresh", &st.auto_refresh);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(120.0f);
      ImGui::SliderFloat("Interval (sec)", &st.refresh_sec, 0.25f, 10.0f, "%.2f");
      st.refresh_sec = std::clamp(st.refresh_sec, 0.05f, 60.0f);

      // Auto-refresh ticker.
      if (st.auto_refresh) {
        const double now = ImGui::GetTime();
        if (now - st.last_refresh_time >= st.refresh_sec) {
          st.last_refresh_time = now;
          (void)load_from_current_state(st, sim, /*force=*/false);
        }
      }

      ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("File")) {
      st.source_tab = 1;
      ImGui::InputText("Path", st.file_path, IM_ARRAYSIZE(st.file_path));
      if (ImGui::Button("Load")) {
        (void)load_from_file_path(st, st.file_path);
      }
      ImGui::SameLine();
      if (ImGui::Button("Use load path")) {
        // Convenience: copy the app's load path when available via UI state.
        // This is a soft dependency: the main menu exposes the same path box.
        // The default load path used by the app is data/save.json.
        std::snprintf(st.file_path, sizeof(st.file_path), "%s", "data/save.json");
      }
      ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Autosaves")) {
      st.source_tab = 2;

      nebula4x::AutosaveConfig cfg;
      cfg.enabled = ui.autosave_game_enabled;
      cfg.interval_hours = ui.autosave_game_interval_hours;
      cfg.keep_files = ui.autosave_game_keep_files;
      cfg.dir = ui.autosave_game_dir;
      cfg.prefix = "autosave_";
      cfg.extension = ".json";

      ImGui::TextDisabled("Directory");
      ImGui::SameLine();
      ImGui::TextUnformatted(ui.autosave_game_dir);

      const auto scan = nebula4x::scan_autosaves(cfg, 48);
      if (!scan.ok) {
        ImGui::TextDisabled("(scan failed)");
      } else if (scan.files.empty()) {
        ImGui::TextDisabled("(none found)");
      } else {
        ImGui::TextDisabled("Click an autosave to load it into the explorer");
        ImGui::BeginChild("##autosave_list", ImVec2(0, 160), true);
        for (const auto& f : scan.files) {
          if (ImGui::Selectable(f.filename.c_str())) {
            (void)load_from_file_path(st, f.path.c_str());
          }
        }
        ImGui::EndChild();
      }

      ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
  }

  ImGui::Separator();

  const bool live_doc = (st.doc_source == "Current game state");
  if (live_doc && st.doc_loaded && st.root) {
    (void)ensure_game_entity_index(*st.root, game_json_cache().revision);
  }

  // --- Filter + goto ---
  ImGui::InputTextWithHint("Filter", "Match keys/values (supports large docs; scan is capped)", st.filter_text,
                          IM_ARRAYSIZE(st.filter_text));
  ImGui::SameLine();
  ImGui::Checkbox("Keys", &st.filter_keys);
  ImGui::SameLine();
  ImGui::Checkbox("Values", &st.filter_values);
  ImGui::SameLine();
  ImGui::Checkbox("Case", &st.filter_case_sensitive);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(110.0f);
  ImGui::InputInt("Max nodes", &st.filter_max_nodes);
  st.filter_max_nodes = std::clamp(st.filter_max_nodes, 1000, 500000);

  const bool goto_enter = ImGui::InputTextWithHint(
      "Go to", "JSON Pointer, e.g. /systems/0/name", st.goto_path, IM_ARRAYSIZE(st.goto_path),
      ImGuiInputTextFlags_EnterReturnsTrue);
  const bool goto_active = ImGui::IsItemActive();
  const ImVec2 goto_pos = ImGui::GetItemRectMin();
  const ImVec2 goto_size = ImGui::GetItemRectSize();

  // Autocomplete popup (procedurally generated from the current JSON document).
  if (st.doc_loaded && goto_active) {
    ImGui::SetNextWindowPos(ImVec2(goto_pos.x, goto_pos.y + goto_size.y));
    ImGui::SetNextWindowSize(ImVec2(goto_size.x, 0.0f));
    ImGui::OpenPopup("##goto_autocomplete");
  }
  if (ImGui::BeginPopup("##goto_autocomplete")) {
    const auto sug = st.root ? nebula4x::suggest_json_pointer_completions(*st.root, st.goto_path, 18, true, st.filter_case_sensitive)
                             : std::vector<std::string>{};
    if (sug.empty()) {
      ImGui::TextDisabled("(no suggestions)");
    } else {
      for (const auto& s : sug) {
        if (ImGui::Selectable(s.c_str())) {
#if defined(_MSC_VER)
          strncpy_s(st.goto_path, IM_ARRAYSIZE(st.goto_path), s.c_str(), _TRUNCATE);
#else
          std::strncpy(st.goto_path, s.c_str(), IM_ARRAYSIZE(st.goto_path));
          st.goto_path[IM_ARRAYSIZE(st.goto_path) - 1] = '\0';
#endif
          ImGui::CloseCurrentPopup();
        }
      }
    }
    ImGui::EndPopup();
  }

  ImGui::SameLine();
  const bool goto_go = ImGui::Button("Go");
  if (goto_go || goto_enter) {
    std::string err;
    const auto* v = st.root ? nebula4x::resolve_json_pointer(*st.root, st.goto_path, true, &err) : nullptr;
    if (v) {
      st.selected_path = st.goto_path;
      st.request_scroll_to_selected = true;
      build_goto_open_paths(st, st.selected_path);
    } else {
      nebula4x::log::warn(std::string("JSON Explorer: go-to failed: ") + err);
    }
  }

  // Update filter cache if needed.
  ensure_filter_cache(st);
  const bool filter_active = st.doc_loaded && st.filter_text[0] != '\0';

  if (filter_active) {
    ImGui::TextDisabled("Filter scan: %d nodes%s", st.filter_scan.scanned,
                        st.filter_scan.truncated ? " (TRUNCATED)" : "");
  }

  if (!st.doc_loaded) {
    ImGui::Separator();
    ImGui::TextDisabled("No document loaded.");
    if (!st.doc_error.empty()) {
      ImGui::TextWrapped("%s", st.doc_error.c_str());
    }
    ImGui::End();
    return;
  }

  if (!st.doc_error.empty()) {
    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "Load/parse error: %s", st.doc_error.c_str());
  }

  ImGui::TextDisabled("Source: %s", st.doc_source.c_str());

  // --- Main split: tree (left) + details (right) ---
  const float avail_w = ImGui::GetContentRegionAvail().x;
  const float left_w = std::max(240.0f, avail_w * 0.47f);

  ImGui::BeginChild("##json_tree", ImVec2(left_w, 0), true);
  {
    TreeDrawContext ctx{st, ui, filter_active};
    draw_json_tree_node(*st.root, "/", "(root)", ctx);

    if (st.goto_open_pending) {
      // Only force-open once unless filtering is active.
      if (!filter_active) {
        st.goto_open_pending = false;
      }
    }
  }
  ImGui::EndChild();

  ImGui::SameLine();

  ImGui::BeginChild("##json_details", ImVec2(0, 0), true);
  {
    draw_breadcrumbs(st);
    ImGui::Separator();
    draw_selected_details(st, ui);
  }
  ImGui::EndChild();

  ImGui::End();
}

} // namespace nebula4x::ui
