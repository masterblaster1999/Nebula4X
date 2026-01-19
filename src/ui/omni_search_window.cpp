#include "ui/omni_search_window.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nebula4x/util/json.h"
#include "nebula4x/util/json_pointer.h"

#include "ui/game_json_cache.h"
#include "ui/game_entity_index.h"

#include "ui/data_lenses_window.h"
#include "ui/dashboards_window.h"
#include "ui/pivot_tables_window.h"
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

// A small fuzzy matcher to rank OmniSearch results.
// Returns -1 if no match. Higher is better.
int fuzzy_score(std::string_view text, std::string_view query, bool case_sensitive) {
  if (query.empty()) return 0;

  std::string t;
  std::string q;

  if (case_sensitive) {
    t.assign(text.begin(), text.end());
    q.assign(query.begin(), query.end());
  } else {
    t = to_lower_copy(text);
    q = to_lower_copy(query);
  }

  // Fast path: substring match.
  const std::size_t sub = t.find(q);
  if (sub != std::string::npos) {
    // Prefer earlier matches and shorter strings.
    const int base = 2200;
    return base - static_cast<int>(sub) * 3 - static_cast<int>(t.size());
  }

  // Subsequence match.
  std::size_t ti = 0;
  int score = 0;
  int streak = 0;

  for (std::size_t qi = 0; qi < q.size(); ++qi) {
    const char qc = q[qi];
    bool found = false;
    while (ti < t.size()) {
      if (t[ti] == qc) {
        found = true;
        score += 40;
        if (streak > 0) score += 25;
        ++streak;
        if (ti == 0 || t[ti - 1] == ' ' || t[ti - 1] == '_' || t[ti - 1] == '-' || t[ti - 1] == '/') {
          score += 15;
        }
        ++ti;
        break;
      }
      streak = 0;
      ++ti;
    }
    if (!found) return -1;
  }

  score -= static_cast<int>(t.size());
  return score;
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
  std::snprintf(buf, sizeof(buf), "%.6g", x);
  return std::string(buf);
}

std::string truncate_middle(std::string_view s, std::size_t max_len) {
  if (s.size() <= max_len) return std::string(s);
  if (max_len < 8) return std::string(s.substr(0, max_len));
  const std::size_t head = (max_len - 1) / 2;
  const std::size_t tail = max_len - head - 1;
  return std::string(s.substr(0, head)) + "…" + std::string(s.substr(s.size() - tail));
}

std::string json_scalar_text(const nebula4x::json::Value& v, std::size_t max_len) {
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
  return json_scalar_text(v, max_len);
}

bool looks_like_array_of_objects(const nebula4x::json::Value& v) {
  if (!v.is_array()) return false;
  const auto* a = v.as_array();
  if (!a || a->empty()) return false;
  int obj = 0;
  int sample = 0;
  for (std::size_t i = 0; i < a->size() && sample < 8; ++i, ++sample) {
    if ((*a)[i].is_object()) ++obj;
  }
  // Heuristic: at least half of first few elements are objects.
  return sample > 0 && obj * 2 >= sample;
}

struct NodeFrame {
  const nebula4x::json::Value* v{nullptr};
  std::string path;
  std::string key_label;
};

enum class ResultKind : int {
  JsonNode = 0,
  Action = 1,
};

enum class OmniActionId : int {
  // Window toggles.
  ToggleFleetManager,
  ToggleAutomationCenter,
  ToggleWatchboard,
  ToggleDataLenses,
  ToggleDashboards,
  TogglePivotTables,
  ToggleUiForge,
  ToggleContextForge,
  ToggleSettings,
  ToggleJsonExplorer,
  ToggleEntityInspector,
  ToggleLayoutProfiles,
  ToggleProcGenAtlas,
  ToggleStarAtlas,

  // Navigation helpers.
  MapTabSystem,
  MapTabGalaxy,

  // UI style presets.
  StyleDark,
  StyleNebula,
  StyleHighContrast,
  StyleProcedural,

  // Procedural UI quick actions.
  ProceduralThemeNewSeed,
  ProceduralLayoutGenerate,
  ProceduralLayoutNewSeedAndGenerate,
};

struct OmniActionDef {
  OmniActionId id;
  const char* label;
  const char* desc;
  const char* keywords;
};

constexpr OmniActionDef kOmniActions[] = {
    {OmniActionId::ToggleFleetManager, "Open Fleet Manager", "Global fleet list + quick mission tools.",
     "fleet ships missions"},
    {OmniActionId::ToggleAutomationCenter, "Open Automation Center", "Bulk ship automation flags + triage.",
     "automation autopilot"},
    {OmniActionId::ToggleWatchboard, "Open Watchboard", "Pin JSON pointers / queries with history + alerts.",
     "watch pins alerts"},
    {OmniActionId::ToggleDataLenses, "Open Data Lenses", "Create tables over JSON arrays (inspect/sort/filter).",
     "lens table"},
    {OmniActionId::ToggleDashboards, "Open Dashboards", "Procedural KPI cards over JSON arrays.",
     "dashboard kpi"},
    {OmniActionId::TogglePivotTables, "Open Pivot Tables", "Group/summarize JSON arrays into pivots.",
     "pivot group"},
    {OmniActionId::ToggleUiForge, "Open UI Forge", "Build custom panels from JSON pointers/queries.",
     "ui forge panels"},
    {OmniActionId::ToggleContextForge, "Open Context Forge", "Auto-generate a UI Forge panel for current selection.",
     "context forge"},
    {OmniActionId::ToggleJsonExplorer, "Open JSON Explorer", "Browse the live game JSON tree.",
     "json explorer"},
    {OmniActionId::ToggleEntityInspector, "Open Entity Inspector", "Inspect any entity by id and its JSON path.",
     "entity inspector"},
    {OmniActionId::ToggleLayoutProfiles, "Open Layout Profiles", "Save/load ImGui layouts and procedural layouts.",
     "layout dock profiles"},
    {OmniActionId::ToggleProcGenAtlas, "Open ProcGen Atlas", "Explore procedural generation maps + metrics.",
     "procgen atlas"},
    {OmniActionId::ToggleStarAtlas, "Open Star Atlas", "Browse star systems and galaxy navigation.",
     "star atlas"},
    {OmniActionId::ToggleSettings, "Open Settings", "UI configuration (scaling, theme, layout, tools).",
     "settings options"},

    {OmniActionId::MapTabGalaxy, "Go to Galaxy Map", "Switch map tab to Galaxy.", "map galaxy"},
    {OmniActionId::MapTabSystem, "Go to System Map", "Switch map tab to System.", "map system"},

    {OmniActionId::StyleDark, "UI Style: Dark", "Switch UI style preset to Dark.", "theme style dark"},
    {OmniActionId::StyleNebula, "UI Style: Nebula", "Switch UI style preset to Nebula.", "theme style nebula"},
    {OmniActionId::StyleHighContrast, "UI Style: High Contrast", "Switch UI style preset to High Contrast.",
     "theme style contrast"},
    {OmniActionId::StyleProcedural, "UI Style: Procedural", "Switch UI style preset to Procedural.",
     "theme style procedural"},

    {OmniActionId::ProceduralThemeNewSeed, "Procedural Theme: New Seed", "Mutate the procedural theme seed.",
     "theme procedural random"},
    {OmniActionId::ProceduralLayoutGenerate, "Procedural Layout: Generate", "Generate a dock layout from current knobs.",
     "layout procedural generate"},
    {OmniActionId::ProceduralLayoutNewSeedAndGenerate, "Procedural Layout: New Seed + Generate",
     "Mutate the procedural layout seed and regenerate.", "layout procedural random"},
};

std::uint32_t xorshift32(std::uint32_t& s) {
  // Simple deterministic PRNG for UI-only seeds (fast, tiny).
  // Note: not intended for simulation/procgen determinism.
  if (s == 0) s = 0x6D2B79F5u;
  s ^= (s << 13);
  s ^= (s >> 17);
  s ^= (s << 5);
  return s;
}

void invoke_omni_action(UIState& ui, OmniActionId id) {
  switch (id) {
    case OmniActionId::ToggleFleetManager:
      ui.show_fleet_manager_window = true;
      break;
    case OmniActionId::ToggleAutomationCenter:
      ui.show_automation_center_window = true;
      break;
    case OmniActionId::ToggleWatchboard:
      ui.show_watchboard_window = true;
      break;
    case OmniActionId::ToggleDataLenses:
      ui.show_data_lenses_window = true;
      break;
    case OmniActionId::ToggleDashboards:
      ui.show_dashboards_window = true;
      break;
    case OmniActionId::TogglePivotTables:
      ui.show_pivot_tables_window = true;
      break;
    case OmniActionId::ToggleUiForge:
      ui.show_ui_forge_window = true;
      break;
    case OmniActionId::ToggleContextForge:
      ui.show_context_forge_window = true;
      break;
    case OmniActionId::ToggleSettings:
      ui.show_settings_window = true;
      break;
    case OmniActionId::ToggleJsonExplorer:
      ui.show_json_explorer_window = true;
      break;
    case OmniActionId::ToggleEntityInspector:
      ui.show_entity_inspector_window = true;
      break;
    case OmniActionId::ToggleLayoutProfiles:
      ui.show_layout_profiles_window = true;
      break;
    case OmniActionId::ToggleProcGenAtlas:
      ui.show_procgen_atlas_window = true;
      break;
    case OmniActionId::ToggleStarAtlas:
      ui.show_star_atlas_window = true;
      break;

    case OmniActionId::MapTabSystem:
      ui.show_map_window = true;
      ui.request_map_tab = MapTab::System;
      break;
    case OmniActionId::MapTabGalaxy:
      ui.show_map_window = true;
      ui.request_map_tab = MapTab::Galaxy;
      break;

    case OmniActionId::StyleDark:
      ui.ui_style_preset = 0;
      break;
    case OmniActionId::StyleNebula:
      ui.ui_style_preset = 3;
      break;
    case OmniActionId::StyleHighContrast:
      ui.ui_style_preset = 4;
      break;
    case OmniActionId::StyleProcedural:
      ui.ui_style_preset = 5;
      break;

    case OmniActionId::ProceduralThemeNewSeed: {
      ui.ui_style_preset = 5;
      std::uint32_t s = static_cast<std::uint32_t>(ui.ui_procedural_theme_seed);
      s ^= static_cast<std::uint32_t>(ImGui::GetTime() * 1000.0);
      xorshift32(s);
      ui.ui_procedural_theme_seed = static_cast<int>(s & 0x7fffffffU);
    } break;

    case OmniActionId::ProceduralLayoutGenerate:
      ui.request_generate_procedural_layout = true;
      break;

    case OmniActionId::ProceduralLayoutNewSeedAndGenerate: {
      std::uint32_t s = static_cast<std::uint32_t>(ui.ui_procedural_layout_seed);
      s ^= static_cast<std::uint32_t>(ImGui::GetTime() * 1000.0);
      xorshift32(s);
      ui.ui_procedural_layout_seed = static_cast<int>(s & 0x7fffffffU);
      ui.request_generate_procedural_layout = true;
    } break;
  }
}

struct SearchResult {
  int score{0};
  ResultKind kind{ResultKind::JsonNode};
  int action_id{-1};

  std::string path;
  std::string key;
  std::string type;
  std::string preview;
  bool array_of_objects{false};
  bool is_scalar{false};
};

struct OmniSearchState {
  bool initialized{false};

  // Live document (current game state).
  bool doc_loaded{false};
  std::uint64_t doc_revision{0};
  std::shared_ptr<const nebula4x::json::Value> root;
  std::string doc_error;
  double last_refresh_time{0.0};

  // Search state.
  char query[256]{};
  std::string last_query;

  // Derived query mode.
  bool action_only{false};
  std::string effective_query;

  bool scanning{false};
  bool truncated{false};
  int scanned_nodes{0};

  std::vector<NodeFrame> stack;
  std::vector<SearchResult> results;
  bool results_dirty_sort{false};

  int selected_idx{-1};
  bool focus_query_next{false};
};

void refresh_doc(OmniSearchState& st, const Simulation& sim, double min_refresh_sec, bool force);
void start_scan(OmniSearchState& st, const UIState& ui);
void scan_step(OmniSearchState& st, const UIState& ui);
void open_in_json_explorer(UIState& ui, const std::string& path);

std::string trim_copy(std::string_view s) {
  std::size_t b = 0;
  std::size_t e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return std::string(s.substr(b, e - b));
}

void add_action_results(OmniSearchState& st, const UIState& ui) {
  const std::string q = trim_copy(st.effective_query);

  // When query is empty, show a curated default list.
  const bool empty = q.empty();

  for (const auto& a : kOmniActions) {
    int sc = 0;
    if (!empty) {
      sc = -1;
      sc = std::max(sc, fuzzy_score(a.label, q, ui.omni_search_case_sensitive));
      if (a.desc) sc = std::max(sc, fuzzy_score(a.desc, q, ui.omni_search_case_sensitive));
      if (a.keywords) sc = std::max(sc, fuzzy_score(a.keywords, q, ui.omni_search_case_sensitive));
      if (sc < 0) continue;

      // Slight boost so commands remain discoverable alongside JSON results.
      sc += 250;
    }

    SearchResult r;
    r.kind = ResultKind::Action;
    r.action_id = static_cast<int>(a.id);
    r.score = sc;
    r.path = a.label;
    r.type = "action";
    r.preview = a.desc ? a.desc : "";
    r.key.clear();
    r.array_of_objects = false;
    r.is_scalar = true;
    st.results.push_back(std::move(r));
  }

  st.results_dirty_sort = true;
}

void sort_results(OmniSearchState& st) {
  if (!st.results_dirty_sort) return;
  st.results_dirty_sort = false;

  std::stable_sort(st.results.begin(), st.results.end(), [](const SearchResult& a, const SearchResult& b) {
    if (a.score != b.score) return a.score > b.score;
    if (a.kind != b.kind) return a.kind == ResultKind::Action; // prefer actions on ties
    return a.path < b.path;
  });

  if (st.selected_idx >= static_cast<int>(st.results.size())) {
    st.selected_idx = static_cast<int>(st.results.size()) - 1;
  }
}


void refresh_doc(OmniSearchState& st, const Simulation& sim, double min_refresh_sec, bool force) {
  const double now = ImGui::GetTime();
  ensure_game_json_cache(sim, now, min_refresh_sec, force);
  const auto& cache = game_json_cache();
  st.doc_revision = cache.revision;
  st.root = cache.root;
  st.doc_loaded = (bool)st.root;
  st.doc_error = cache.error;
  if (st.doc_loaded && st.root) {
    (void)ensure_game_entity_index(*st.root, st.doc_revision);
  }
}

void start_scan(OmniSearchState& st, const UIState& ui) {
  st.results.clear();
  st.results_dirty_sort = false;
  st.scanned_nodes = 0;
  st.truncated = false;
  st.stack.clear();
  st.selected_idx = -1;

  // Parse query mode:
  //  - ">foo" => action-only mode (command palette)
  //  - "foo"  => JSON + actions
  st.action_only = false;
  st.effective_query = st.last_query;
  if (!st.effective_query.empty() && st.effective_query[0] == '>') {
    st.action_only = true;
    st.effective_query.erase(0, 1);
  }
  st.effective_query = trim_copy(st.effective_query);

  // Always add actions (even when JSON doc isn't available yet).
  add_action_results(st, ui);

  if (!st.doc_loaded) {
    st.scanning = false;
    return;
  }

  if (st.effective_query.empty()) {
    st.scanning = false;
    return;
  }

  if (st.action_only) {
    st.scanning = false;
    return;
  }

  // Root path is '/' for UI consistency.
  NodeFrame root;
  root.v = st.root.get();
  root.path = "/";
  root.key_label = "/";
  st.stack.push_back(std::move(root));
  st.scanning = true;
}

void scan_step(OmniSearchState& st, const UIState& ui) {
  // Even when no scan is active, we may have action results that need sorting.
  if (!st.scanning) {
    sort_results(st);
    return;
  }
  if (st.effective_query.empty() || st.action_only) {
    st.scanning = false;
    sort_results(st);
    return;
  }

  const int budget = std::max(50, ui.omni_search_nodes_per_frame);
  const int max_results = std::clamp(ui.omni_search_max_results, 10, 50000);

  int steps = 0;
  while (steps < budget && !st.stack.empty()) {
    NodeFrame n = std::move(st.stack.back());
    st.stack.pop_back();
    ++steps;
    ++st.scanned_nodes;

    if (!n.v) continue;

    // Compute match score.
    int sc = -1;
    if (ui.omni_search_match_keys) {
      if (!n.key_label.empty()) {
        sc = std::max(sc, fuzzy_score(n.key_label, st.effective_query, ui.omni_search_case_sensitive));
      }
      // Matching against the full path is very useful for nested structures.
      sc = std::max(sc, fuzzy_score(n.path, st.effective_query, ui.omni_search_case_sensitive));
    }
    if (ui.omni_search_match_values) {
      if (n.v->is_string()) {
        sc = std::max(sc, fuzzy_score(n.v->string_value(), st.effective_query, ui.omni_search_case_sensitive));
      } else if (n.v->is_number()) {
        const std::string t = format_number(n.v->number_value());
        sc = std::max(sc, fuzzy_score(t, st.effective_query, ui.omni_search_case_sensitive));
      } else if (n.v->is_bool()) {
        const std::string t = n.v->bool_value() ? "true" : "false";
        sc = std::max(sc, fuzzy_score(t, st.effective_query, ui.omni_search_case_sensitive));
      } else if (n.v->is_null()) {
        sc = std::max(sc, fuzzy_score("null", st.effective_query, ui.omni_search_case_sensitive));
      }
    }

    if (sc >= 0) {
      SearchResult r;
      r.score = sc;
      r.kind = ResultKind::JsonNode;
      r.path = n.path;
      r.key = n.key_label;
      r.type = json_type_name(*n.v);
      r.preview = json_node_preview(*n.v, 72);
      r.array_of_objects = looks_like_array_of_objects(*n.v);
      r.is_scalar = (!n.v->is_object() && !n.v->is_array());
      st.results.push_back(std::move(r));
      st.results_dirty_sort = true;

      if (static_cast<int>(st.results.size()) >= max_results) {
        st.truncated = true;
        st.scanning = false;
        break;
      }
    }

    // Push children.
    if (n.v->is_object()) {
      const auto* o = n.v->as_object();
      if (o) {
        for (auto it = o->begin(); it != o->end(); ++it) {
          const std::string child_path = nebula4x::json_pointer_join(n.path, it->first);
          NodeFrame c;
          c.v = &it->second;
          c.path = child_path;
          c.key_label = it->first;
          st.stack.push_back(std::move(c));
        }
      }
    } else if (n.v->is_array()) {
      const auto* a = n.v->as_array();
      if (a) {
        for (std::size_t i = 0; i < a->size(); ++i) {
          const std::string child_path = nebula4x::json_pointer_join_index(n.path, i);
          NodeFrame c;
          c.v = &(*a)[i];
          c.path = child_path;
          c.key_label = std::to_string(i);
          st.stack.push_back(std::move(c));
        }
      }
    }
  }

  if (st.stack.empty()) {
    st.scanning = false;
  }

  // Keep best results at the top.
  sort_results(st);
}

void open_in_json_explorer(UIState& ui, const std::string& path) {
  ui.show_json_explorer_window = true;
  ui.request_json_explorer_goto_path = path;
}

} // namespace


void draw_omni_search_window(Simulation& sim, UIState& ui) {
  if (!ui.show_omni_search_window) return;

  static OmniSearchState st;

  if (!st.initialized) {
    st.initialized = true;
    std::snprintf(st.query, sizeof(st.query), "%s", "");
    st.last_query.clear();
    st.focus_query_next = true;
    refresh_doc(st, sim, /*min_refresh_sec=*/0.0, /*force=*/true);
  }

  ImGui::SetNextWindowSize(ImVec2(980, 620), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("OmniSearch", &ui.show_omni_search_window)) {
    ImGui::End();
    return;
  }

  if (ImGui::IsWindowAppearing()) {
    st.focus_query_next = true;
  }

  if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    ui.show_omni_search_window = false;
    ImGui::End();
    return;
  }

  // Auto-refresh document (optional).
  {
    const double now = ImGui::GetTime();
    ui.omni_search_refresh_sec = std::clamp(ui.omni_search_refresh_sec, 0.10f, 30.0f);
    if (ui.omni_search_auto_refresh && (now - st.last_refresh_time) >= ui.omni_search_refresh_sec) {
      st.last_refresh_time = now;
      refresh_doc(st, sim, ui.omni_search_refresh_sec, /*force=*/false);
      // Restart scan with the current query.
      st.last_query = std::string(st.query);
      start_scan(st, ui);
    }
  }

  bool options_changed = false;

  // Top bar: Refresh + options.
  {
    if (ImGui::Button("Refresh##omni")) {
      st.last_refresh_time = ImGui::GetTime();
      refresh_doc(st, sim, /*min_refresh_sec=*/0.0, /*force=*/true);
      // Re-run scan with current query.
      st.last_query = std::string(st.query);
      start_scan(st, ui);
    }

    ImGui::SameLine();
    options_changed |= ImGui::Checkbox("Auto##omni", &ui.omni_search_auto_refresh);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    options_changed |= ImGui::SliderFloat("##omni_refresh_sec", &ui.omni_search_refresh_sec, 0.10f, 10.0f, "%.2fs");

    ImGui::SameLine();
    options_changed |= ImGui::Checkbox("Keys", &ui.omni_search_match_keys);
    ImGui::SameLine();
    options_changed |= ImGui::Checkbox("Values", &ui.omni_search_match_values);
    ImGui::SameLine();
    options_changed |= ImGui::Checkbox("Case", &ui.omni_search_case_sensitive);

    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    options_changed |= ImGui::InputInt("Nodes/frame", &ui.omni_search_nodes_per_frame);
    ui.omni_search_nodes_per_frame = std::clamp(ui.omni_search_nodes_per_frame, 50, 500000);

    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    options_changed |= ImGui::InputInt("Max results", &ui.omni_search_max_results);
    ui.omni_search_max_results = std::clamp(ui.omni_search_max_results, 10, 50000);
  }

  if (!st.doc_error.empty()) {
    ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "Parse error: %s", st.doc_error.c_str());
  }

  // Search input.
  bool query_enter = false;
  {
    ImGuiInputTextFlags tf = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll;
    if (st.focus_query_next) {
      ImGui::SetKeyboardFocusHere();
      st.focus_query_next = false;
    }

    query_enter = ImGui::InputTextWithHint("##omni_query", "Search JSON and commands (prefix '>' for commands)", st.query,
                                          IM_ARRAYSIZE(st.query), tf);

    const std::string q = std::string(st.query);
    if (q != st.last_query) {
      st.last_query = q;
      start_scan(st, ui);
    }
  }

  // If the user disables both keys and values, force keys back on.
  if (!ui.omni_search_match_keys && !ui.omni_search_match_values) {
    ui.omni_search_match_keys = true;
    options_changed = true;
  }

  if (options_changed) {
    // Re-run scan with the current query.
    start_scan(st, ui);
  }

  // Step the scan (incremental).
  scan_step(st, ui);

  // Status line.
  {
    ImGui::TextDisabled("Results: %zu%s | Scanned: %d | Pending: %zu%s", st.results.size(), st.truncated ? "+" : "",
                        st.scanned_nodes, st.stack.size(), st.scanning ? " (scanning...)" : "");

    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Scanning is incremental to keep the UI responsive.\n"
          "Increase Nodes/frame to scan faster.\n\n"
          "Right-click a result for actions.");
    }

    if (st.scanning) {
      ImGui::SameLine();
      if (ImGui::SmallButton("Stop##omni")) {
        st.scanning = false;
      }
    }
  }

  ImGui::Separator();

  // Layout: left results + right details.
  const float left_w = 620.0f;
  ImGui::BeginChild("##omni_left", ImVec2(left_w, 0), true);
  {
    ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                         ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable;

    if (ImGui::BeginTable("##omni_table", 4, tf)) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_WidthFixed, 56.0f);
      ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 64.0f);
      ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch, 280.0f);
      ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthStretch, 180.0f);
      ImGui::TableHeadersRow();

      ImGuiListClipper clip;
      clip.Begin(static_cast<int>(st.results.size()));
      while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
          const auto& r = st.results[static_cast<std::size_t>(i)];

          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);

          ImGui::PushID(i);
          const bool selected = (st.selected_idx == i);
          const bool row_clicked = ImGui::Selectable(
              "##row", selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap);
          if (row_clicked) st.selected_idx = i;

          // Double-click to jump.
          if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            if (r.kind == ResultKind::Action) {
              invoke_omni_action(ui, static_cast<OmniActionId>(r.action_id));
              ui.show_omni_search_window = false;
            } else {
              open_in_json_explorer(ui, r.path);
            }
          }

          // Context menu.
          if (ImGui::BeginPopupContextItem("##omni_ctx")) {
            if (r.kind == ResultKind::Action) {
              if (ImGui::MenuItem("Run")) {
                invoke_omni_action(ui, static_cast<OmniActionId>(r.action_id));
                ui.show_omni_search_window = false;
              }
              if (ImGui::MenuItem("Copy command name")) {
                ImGui::SetClipboardText(r.path.c_str());
              }
            } else {
              if (ImGui::MenuItem("Copy JSON Pointer")) {
                ImGui::SetClipboardText(r.path.c_str());
              }
              if (ImGui::MenuItem("Go to in JSON Explorer")) {
                open_in_json_explorer(ui, r.path);
              }
              if (ImGui::MenuItem("Pin to Watchboard")) {
                add_watch_item(ui, r.path, r.key.empty() ? r.path : r.key);
                ui.show_watchboard_window = true;
              }

              // Resolve the value for type-specific actions.
              std::string err;
              const nebula4x::json::Value* v = st.root
                                                  ? resolve_json_pointer(*st.root, r.path,
                                                                         /*accept_root_slash=*/true, &err)
                                                  : nullptr;
              if (v) {
                std::uint64_t ent_id = 0;
                if (json_to_u64_id(*v, ent_id)) {
                  if (const auto* ent = find_game_entity(ent_id)) {
                    ImGui::Separator();
                    std::string elabel = ent->kind + " #" + std::to_string(ent->id);
                    if (!ent->name.empty()) elabel += "  " + ent->name;
                    ImGui::TextDisabled("Referenced entity");
                    ImGui::TextUnformatted(elabel.c_str());
                    if (ImGui::MenuItem("Go to referenced entity")) {
                      open_in_json_explorer(ui, ent->path);
                    }
                    if (ImGui::MenuItem("Open in Entity Inspector")) {
                      ui.show_entity_inspector_window = true;
                      ui.entity_inspector_id = ent->id;
                    }
                    if (ImGui::MenuItem("Open in Reference Graph")) {
                      ui.show_reference_graph_window = true;
                      ui.reference_graph_focus_id = ent->id;
                    }
                  }
                }
              }

              if (v && v->is_array()) {
                ImGui::Separator();
                if (ImGui::MenuItem("Create Data Lens")) {
                  add_json_table_view(ui, r.path);
                  ui.show_data_lenses_window = true;
                }
                if (ImGui::MenuItem("Create Dashboard")) {
                  add_json_dashboard_for_path(ui, r.path);
                  ui.show_dashboards_window = true;
                }
                if (ImGui::MenuItem("Create Pivot Table")) {
                  add_json_pivot_for_path(ui, r.path);
                  ui.show_pivot_tables_window = true;
                }
              }

              if (!err.empty()) {
                ImGui::Separator();
                ImGui::TextDisabled("Resolve error: %s", err.c_str());
              }
            }

            ImGui::EndPopup();
          }

          // Render columns.
          ImGui::TableSetColumnIndex(0);
          ImGui::Text("%d", r.score);

          ImGui::TableSetColumnIndex(1);
          if (r.array_of_objects) {
            ImGui::TextUnformatted("array*");
            if (ImGui::IsItemHovered()) {
              ImGui::SetTooltip(
                  "Looks like an array of objects.\n"
                  "Good candidate for Data Lenses / Dashboards / Pivot Tables.");
            }
          } else {
            ImGui::TextUnformatted(r.type.c_str());
          }

          ImGui::TableSetColumnIndex(2);
          const std::string path_short = truncate_middle(r.path, 110);
          ImGui::TextUnformatted(path_short.c_str());
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.path.c_str());

          ImGui::TableSetColumnIndex(3);
          const std::string pv = truncate_middle(r.preview, 90);
          ImGui::TextUnformatted(pv.c_str());
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.preview.c_str());

          ImGui::PopID();
        }
      }

      ImGui::EndTable();
    }
  }
  ImGui::EndChild();

  ImGui::SameLine();

  ImGui::BeginChild("##omni_right", ImVec2(0, 0), true);
  {
    ImGui::Text("Selection");
    ImGui::Separator();

    if (st.selected_idx < 0 || st.selected_idx >= static_cast<int>(st.results.size())) {
      ImGui::TextDisabled("Select a result on the left.");
    } else {
      const auto& r = st.results[static_cast<std::size_t>(st.selected_idx)];

      if (r.kind == ResultKind::Action) {
        ImGui::TextDisabled("Command:");
        ImGui::TextWrapped("%s", r.path.c_str());

        if (!r.preview.empty()) {
          ImGui::TextDisabled("Description:");
          ImGui::TextWrapped("%s", r.preview.c_str());
        }

        ImGui::Separator();
        if (ImGui::Button("Run")) {
          invoke_omni_action(ui, static_cast<OmniActionId>(r.action_id));
          ui.show_omni_search_window = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Copy name")) {
          ImGui::SetClipboardText(r.path.c_str());
        }
      } else {
        ImGui::TextDisabled("Path:");
        ImGui::TextWrapped("%s", r.path.c_str());

        ImGui::TextDisabled("Type:");
        ImGui::Text("%s", r.type.c_str());

        std::string err;
        const nebula4x::json::Value* v = st.root ? resolve_json_pointer(*st.root, r.path,
                                                                       /*accept_root_slash=*/true, &err)
                                                 : nullptr;

        if (v) {
          ImGui::TextDisabled("Preview:");
          const std::string pv = json_node_preview(*v, 240);
          ImGui::TextWrapped("%s", pv.c_str());

          ImGui::Separator();
          if (ImGui::Button("Go to JSON Explorer")) {
            open_in_json_explorer(ui, r.path);
          }
          ImGui::SameLine();
          if (ImGui::Button("Copy Pointer")) {
            ImGui::SetClipboardText(r.path.c_str());
          }
          ImGui::SameLine();
          if (ImGui::Button("Pin")) {
            add_watch_item(ui, r.path, r.key.empty() ? r.path : r.key);
            ui.show_watchboard_window = true;
          }

          if (v->is_array()) {
            ImGui::SeparatorText("Array actions");
            if (ImGui::Button("Create Data Lens")) {
              add_json_table_view(ui, r.path);
              ui.show_data_lenses_window = true;
            }
            if (ImGui::Button("Create Dashboard")) {
              add_json_dashboard_for_path(ui, r.path);
              ui.show_dashboards_window = true;
            }
            if (ImGui::Button("Create Pivot")) {
              add_json_pivot_for_path(ui, r.path);
              ui.show_pivot_tables_window = true;
            }
          }
        } else {
          ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "Resolve error: %s", err.c_str());
        }
      }
    }

    ImGui::Separator();
    ImGui::TextDisabled("Tips");
    ImGui::BulletText("Ctrl+F toggles OmniSearch. (Ctrl+P opens the Command Palette.)");
    ImGui::BulletText("Prefix query with '>' to search commands only.");
    ImGui::BulletText("Enter runs the selected command (or jumps to JSON Explorer for JSON results).");
    ImGui::BulletText("Double-click a JSON result to jump to JSON Explorer; double-click a command to run it.");
    ImGui::BulletText("Right-click a JSON result for actions (Pin/Lens/Dashboard/Pivot).");
  }
  ImGui::EndChild();

  // Enter: jump to best match.
  if (query_enter && !st.results.empty()) {
    const int idx = (st.selected_idx >= 0 && st.selected_idx < static_cast<int>(st.results.size())) ? st.selected_idx : 0;
    const auto& r = st.results[static_cast<std::size_t>(idx)];
    if (r.kind == ResultKind::Action) {
      invoke_omni_action(ui, static_cast<OmniActionId>(r.action_id));
      ui.show_omni_search_window = false;
    } else {
      open_in_json_explorer(ui, r.path);
    }
  }

  ImGui::End();
}

} // namespace nebula4x::ui
