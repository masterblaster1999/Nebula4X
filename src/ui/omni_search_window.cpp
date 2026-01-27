#include "ui/omni_search_window.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "nebula4x/util/file_io.h"
#include "nebula4x/util/json.h"
#include "nebula4x/util/json_pointer.h"
#include "nebula4x/util/strings.h"

#include "ui/data_lenses_window.h"
#include "ui/dashboards_window.h"
#include "ui/entity_inspector_window.h"
#include "ui/game_entity_index.h"
#include "ui/game_json_cache.h"
#include "ui/navigation.h"
#include "ui/pivot_tables_window.h"
#include "ui/watchboard_window.h"

#include "ui/layout_profiles.h"
#include "ui/workspace_presets.h"
#include "ui/window_management.h"

namespace nebula4x::ui {

namespace {

// --- Small string helpers ---

static inline char to_lower_ascii(char c) {
  if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
  return c;
}

static inline bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

std::string trim_copy(std::string_view in) {
  std::size_t a = 0;
  std::size_t b = in.size();
  while (a < b && is_space(in[a])) ++a;
  while (b > a && is_space(in[b - 1])) --b;
  return std::string(in.substr(a, b - a));
}

std::string to_lower_copy(std::string_view s) {
  std::string out;
  out.resize(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) out[i] = to_lower_ascii(s[i]);
  return out;
}

bool contains_ci(std::string_view hay, std::string_view needle) {
  if (needle.empty()) return true;
  const std::string h = to_lower_copy(hay);
  const std::string n = to_lower_copy(needle);
  return h.find(n) != std::string::npos;
}

bool contains_cs(std::string_view hay, std::string_view needle) {
  if (needle.empty()) return true;
  return std::string(hay).find(std::string(needle)) != std::string::npos;
}

// --- Fuzzy scorer ---

// Simple "fuzzy subsequence with rewards" scoring.
// Returns -1 if needle does not match haystack.
int fuzzy_score(std::string_view haystack, std::string_view needle, bool case_sensitive) {
  if (needle.empty()) return 0;
  if (haystack.empty()) return -1;

  int score = 0;
  std::size_t h = 0;
  std::size_t n = 0;

  int consecutive = 0;
  int start_bonus = 0;
  bool started = false;

  auto eq = [&](char a, char b) {
    if (!case_sensitive) {
      a = to_lower_ascii(a);
      b = to_lower_ascii(b);
    }
    return a == b;
  };

  while (h < haystack.size() && n < needle.size()) {
    if (eq(haystack[h], needle[n])) {
      if (!started) {
        started = true;
        // Big bonus for early match / word start.
        if (h == 0 || haystack[h - 1] == '/' || haystack[h - 1] == '_' || haystack[h - 1] == '-' ||
            haystack[h - 1] == ' ') {
          start_bonus += 30;
        } else {
          start_bonus += std::max(0, 20 - static_cast<int>(h));
        }
      }

      // Reward consecutive matches.
      consecutive++;
      score += 10 + consecutive * 3;
      n++;
    } else {
      consecutive = 0;
      // Small penalty for skipping characters.
      score -= 1;
    }
    h++;
  }

  if (n != needle.size()) return -1;
  score += start_bonus;

  // Length normalization: prefer shorter haystacks for same match quality.
  score -= static_cast<int>(haystack.size() / 8);
  return score;
}

std::string truncate_middle(const std::string& s, std::size_t max_len) {
  if (s.size() <= max_len) return s;
  if (max_len < 6) return s.substr(0, max_len);

  const std::size_t keep = (max_len - 3) / 2;
  const std::size_t keep2 = max_len - 3 - keep;
  return s.substr(0, keep) + "..." + s.substr(s.size() - keep2);
}

// --- JSON helpers ---

const char* json_type_name(const nebula4x::json::Value& v) {
  if (v.is_null()) return "null";
  if (v.is_bool()) return "bool";
  if (v.is_number()) return "number";
  if (v.is_string()) return "string";
  if (v.is_array()) return "array";
  if (v.is_object()) return "object";
  return "unknown";
}

std::string json_node_preview(const nebula4x::json::Value& v, std::size_t max_chars) {
  std::string out;
  out.reserve(std::min<std::size_t>(max_chars, 256));

  if (v.is_null()) {
    out = "null";
  } else if (v.is_bool()) {
    out = v.bool_value(false) ? "true" : "false";
  } else if (v.is_number()) {
    std::ostringstream oss;
    oss << v.number_value();
    out = oss.str();
  } else if (v.is_string()) {
    out = "\"" + v.string_value() + "\"";
  } else if (v.is_array()) {
    const auto* a = v.as_array();
    out = "array[" + std::to_string(a ? a->size() : 0) + "]";
  } else if (v.is_object()) {
    const auto* o = v.as_object();
    out = "object{" + std::to_string(o ? o->size() : 0) + "}";
  } else {
    out = "(unknown)";
  }

  if (out.size() > max_chars) out.resize(max_chars);
  return out;
}

bool looks_like_array_of_objects(const nebula4x::json::Value& v) {
  const auto* arr = v.as_array();
  if (!arr || arr->empty()) return false;

  // Heuristic: first few elements are objects, and at least one has an "id" key.
  const std::size_t probe = std::min<std::size_t>(arr->size(), 6);
  int obj_count = 0;
  bool has_id = false;
  for (std::size_t i = 0; i < probe; ++i) {
    const auto* o = (*arr)[i].as_object();
    if (!o) continue;
    obj_count++;
    if (o->find("id") != o->end()) has_id = true;
  }
  return obj_count >= (int)probe / 2 && has_id;
}

// --- Docs scanning (Codex markdown) ---

struct DocEntry {
  std::string title;
  std::string ref;          // normalized reference (lowercase, forward slashes)
  std::string display_path; // relative path for UI display
  std::string abs_path;
  bool from_data{false};

  std::vector<std::string> lines;
  std::string raw_all;  // original text (for case-sensitive searches)
  std::string lower_all; // full lowercased text for substring matches
};

std::string strip_trailing_cr(std::string s) {
  if (!s.empty() && s.back() == '\r') s.pop_back();
  return s;
}

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> out;
  out.reserve(256);

  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find('\n', start);
    if (end == std::string::npos) {
      out.push_back(strip_trailing_cr(text.substr(start)));
      break;
    }
    out.push_back(strip_trailing_cr(text.substr(start, end - start)));
    start = end + 1;
  }
  return out;
}

std::string normalize_doc_ref(std::string_view path) {
  std::string p = trim_copy(path);

  // Remove leading ./
  while (p.rfind("./", 0) == 0) p = p.substr(2);

  // Normalize separators.
  for (char& c : p) {
    if (c == '\\') c = '/';
  }

  // Strip leading slash.
  while (!p.empty() && p.front() == '/') p.erase(p.begin());

  // Strip common prefixes.
  const auto lower = nebula4x::to_lower(p);
  if (lower.rfind("data/docs/", 0) == 0) p = p.substr(std::string("data/docs/").size());
  if (lower.rfind("docs/", 0) == 0) p = p.substr(std::string("docs/").size());

  // Lowercase for lookup.
  p = nebula4x::to_lower(p);
  return p;
}

std::string extract_title_from_markdown(const std::vector<std::string>& lines, std::string_view fallback) {
  for (const auto& ln : lines) {
    if (ln.empty()) continue;
    std::string_view s = ln;
    std::size_t i = 0;
    while (i < s.size() && s[i] == '#') ++i;
    if (i == 0) continue;
    if (i < s.size() && s[i] == ' ') {
      const auto t = trim_copy(s.substr(i + 1));
      if (!t.empty()) return t;
    }
  }
  return std::string(fallback);
}

void add_doc(std::vector<DocEntry>& docs, std::unordered_map<std::string, int>& doc_by_ref, const DocEntry& e) {
  if (e.ref.empty()) return;
  if (doc_by_ref.find(e.ref) != doc_by_ref.end()) return;
  const int idx = static_cast<int>(docs.size());
  docs.push_back(e);
  doc_by_ref[e.ref] = idx;
}

void scan_dir_for_docs(std::vector<DocEntry>& docs,
                       std::unordered_map<std::string, int>& doc_by_ref,
                       const std::filesystem::path& base,
                       bool from_data) {
  if (!std::filesystem::exists(base)) return;
  if (!std::filesystem::is_directory(base)) return;

  try {
    for (const auto& it : std::filesystem::recursive_directory_iterator(base)) {
      if (!it.is_regular_file()) continue;
      const auto p = it.path();
      const auto ext = nebula4x::to_lower(p.extension().string());
      if (ext != ".md" && ext != ".markdown") continue;

      const auto rel = std::filesystem::relative(p, base).generic_string();
      const auto ref = normalize_doc_ref(rel);
      if (ref.empty()) continue;

      try {
        const std::string contents = nebula4x::read_text_file(p.string());
        DocEntry e;
        e.from_data = from_data;
        e.display_path = rel;
        e.ref = ref;
        e.abs_path = p.string();
        e.lines = split_lines(contents);
        e.title = extract_title_from_markdown(e.lines, p.stem().string());
        e.raw_all = contents;
        e.lower_all = to_lower_copy(contents);
        add_doc(docs, doc_by_ref, e);
      } catch (...) {
        // Ignore unreadable docs.
      }
    }
  } catch (...) {
    // Ignore iterator errors.
  }
}

bool doc_line_contains(const std::string& line, std::string_view q, bool case_sensitive) {
  if (q.empty()) return true;
  if (case_sensitive) return contains_cs(line, q);
  return contains_ci(line, q);
}

std::string doc_find_snippet(const DocEntry& d, std::string_view q, bool case_sensitive) {
  if (q.empty()) return {};
  // Prefer a whole-line hit.
  for (const auto& ln : d.lines) {
    if (ln.empty()) continue;
    if (doc_line_contains(ln, q, case_sensitive)) {
      std::string s = trim_copy(ln);
      if (s.size() > 200) s.resize(200);
      return s;
    }
  }
  return {};
}

// --- Omni actions (commands) ---

enum class OmniActionId : int {
  ToggleCommandPalette = 1,
  ToggleNavigator,
  ToggleNotifications,
  ToggleIntelNotebook,
  ToggleDocs,
  ToggleHotkeys,
  ToggleTimeMachine,
  ToggleCompare,
  ToggleReferenceGraph,
  ToggleEntityInspector,
  ToggleJsonExplorer,
  ToggleWatchboard,
  ToggleDataLenses,
  ToggleDashboards,
  TogglePivotTables,
  ToggleUiForge,
  ToggleWindowManager,
  ToggleLayoutProfiles,
  ToggleFocusMode,
  OpenSettings,
  ResetLayout,
};

struct OmniAction {
  OmniActionId id{OmniActionId::ToggleCommandPalette};
  const char* group{""};
  const char* label{""};
  const char* desc{""};
  const char* shortcut_hint{""}; // Optional human hint (not the actual bound key).
};

static const OmniAction kActions[] = {
    {OmniActionId::ToggleCommandPalette, "Command", "Open Command Palette", "Search actions + run commands.", "Ctrl+P"},
    {OmniActionId::ToggleNavigator, "Navigation", "Open Navigator", "Bookmarks + selection history.", nullptr},
    {OmniActionId::ToggleNotifications, "Tools", "Open Notification Center", "Persistent inbox for events/alerts.", "F3"},
    {OmniActionId::ToggleIntelNotebook, "Intel", "Open Intel Notebook",
     "System notes + curated journal (tags, pins, export).", "Ctrl+Shift+I"},
    {OmniActionId::ToggleDocs, "Help", "Open Codex", "In-game documentation browser.", "F1"},
    {OmniActionId::ToggleHotkeys, "Help", "Open Hotkeys", "View/rebind keyboard shortcuts.", nullptr},
    {OmniActionId::ToggleTimeMachine, "Tools", "Time Machine", "State history + diffs (debug / analysis).", "Ctrl+Shift+D"},
    {OmniActionId::ToggleCompare, "Tools", "Compare / Diff", "Compare two entities + export a merge patch.", "Ctrl+Shift+X"},
    {OmniActionId::ToggleReferenceGraph, "Tools", "Reference Graph", "Entity relationships from live JSON.", nullptr},
    {OmniActionId::ToggleEntityInspector, "Tools", "Entity Inspector", "ID resolver + reference finder.", nullptr},
    {OmniActionId::ToggleJsonExplorer, "Tools", "JSON Explorer", "Inspect live game state JSON.", nullptr},
    {OmniActionId::ToggleWatchboard, "Tools", "Watchboard", "Pin JSON pointers + alerts.", nullptr},
    {OmniActionId::ToggleDataLenses, "Tools", "Data Lenses", "Table view over JSON arrays.", nullptr},
    {OmniActionId::ToggleDashboards, "Tools", "Dashboards", "Charts/widgets over Data Lenses.", nullptr},
    {OmniActionId::TogglePivotTables, "Tools", "Pivot Tables", "Group-by aggregations over arrays.", nullptr},
    {OmniActionId::ToggleUiForge, "Tools", "UI Forge", "Custom panels over live game JSON.", nullptr},
    {OmniActionId::ToggleWindowManager, "Layout", "Open Window Manager",
     "Manage window visibility and pop-out (floating) launch behavior.", "Ctrl+Shift+W"},
    {OmniActionId::ToggleLayoutProfiles, "Layout", "Open Layout Profiles",
     "Save/load docking layouts (ImGui ini profiles).", "Ctrl+Shift+L"},
    {OmniActionId::ToggleFocusMode, "Layout", "Toggle Focus Mode", "Hide non-essential windows (press again to restore).",
     "F10"},
    {OmniActionId::OpenSettings, "System", "Open Settings", "UI preferences and theme toggles.", nullptr},
    {OmniActionId::ResetLayout, "System", "Reset Dock Layout", "Rebuild a default dock layout.", nullptr},
};

void invoke_omni_action(UIState& ui, OmniActionId id) {
  switch (id) {
    case OmniActionId::ToggleCommandPalette: ui.show_command_palette = true; break;
    case OmniActionId::ToggleNavigator: ui.show_navigator_window = true; break;
    case OmniActionId::ToggleNotifications: ui.show_notifications_window = true; break;
    case OmniActionId::ToggleIntelNotebook: ui.show_intel_notebook_window = true; break;
    case OmniActionId::ToggleDocs:
      ui.show_help_window = true;
      ui.request_help_tab = HelpTab::Docs;
      break;
    case OmniActionId::ToggleHotkeys:
      ui.show_help_window = true;
      ui.request_help_tab = HelpTab::Shortcuts;
      break;
    case OmniActionId::ToggleTimeMachine: ui.show_time_machine_window = !ui.show_time_machine_window; break;
    case OmniActionId::ToggleCompare: ui.show_compare_window = !ui.show_compare_window; break;
    case OmniActionId::ToggleReferenceGraph: ui.show_reference_graph_window = true; break;
    case OmniActionId::ToggleEntityInspector: ui.show_entity_inspector_window = true; break;
    case OmniActionId::ToggleJsonExplorer: ui.show_json_explorer_window = true; break;
    case OmniActionId::ToggleWatchboard: ui.show_watchboard_window = true; break;
    case OmniActionId::ToggleDataLenses: ui.show_data_lenses_window = true; break;
    case OmniActionId::ToggleDashboards: ui.show_dashboards_window = true; break;
    case OmniActionId::TogglePivotTables: ui.show_pivot_tables_window = true; break;
    case OmniActionId::ToggleUiForge: ui.show_ui_forge_window = true; break;
    case OmniActionId::ToggleWindowManager: ui.show_window_manager_window = true; break;
    case OmniActionId::ToggleLayoutProfiles: ui.show_layout_profiles_window = true; break;
    case OmniActionId::ToggleFocusMode: toggle_focus_mode(ui); break;
    case OmniActionId::OpenSettings: ui.show_settings_window = true; break;
    case OmniActionId::ResetLayout: ui.request_reset_window_layout = true; break;
  }
}

// --- Search runtime structures ---

enum class ResultKind : int {
  Action = 0,
  Window,
  WorkspacePreset,
  LayoutProfile,
  Entity,
  Doc,
  JsonNode,
};

struct SearchResult {
  int score{0};
  ResultKind kind{ResultKind::JsonNode};

  // Common display fields.
  std::string path;    // Display label (or JSON pointer for JsonNode).
  std::string key;     // For JsonNode pins.
  std::string type;    // Kind label ("ship", "doc", "string", etc).
  std::string preview; // Small preview snippet/value.
  std::string hint;    // Optional additional guidance (e.g., shortcut hint).

  // Action payload.
  int action_id{0};

  // UI payloads.
  std::string window_id;
  std::string layout_profile;
  std::string workspace_preset;

  // JSON node flags.
  bool array_of_objects{false};
  bool is_scalar{false};

  // Entity payload.
  std::uint64_t entity_id{0};
  std::string entity_kind;
  std::string entity_json_path;
  bool nav_valid{false};
  NavTargetKind nav_kind{NavTargetKind::System};
  Id nav_id{kInvalidId};

  // Doc payload.
  std::string doc_ref;
  std::string doc_display_path;
  std::string doc_abs_path;
};

struct ScanFrame {
  const nebula4x::json::Value* v{nullptr};
  std::string path;
  std::string key;
};

struct OmniSearchState {
  // Cached root JSON snapshot from the game.
  std::shared_ptr<const nebula4x::json::Value> root;
  std::uint64_t doc_revision{0};
  bool doc_loaded{false};

  // Docs index (Codex markdown).
  bool docs_scanned{false};
  std::vector<DocEntry> docs;
  std::unordered_map<std::string, int> doc_by_ref;
  std::string docs_error;

  // Entities index (snapshot derived).
  std::uint64_t entity_revision{0};
  std::vector<GameEntityIndexEntry> entities;

  // Query state.
  char query_buf[256]{};
  std::string last_query;
  std::string effective_query;

  bool action_only{false};
  bool entity_only{false};
  bool docs_only{false};

  bool ui_only{false};

  // Results.
  std::vector<SearchResult> results;
  int selected_idx{-1};
  bool results_dirty_sort{false};

  // JSON scan runtime.
  bool scanning_json{false};
  bool truncated{false};
  std::uint64_t scanned_nodes{0};
  std::vector<ScanFrame> stack;

  // Entity scan runtime.
  bool scanning_entities{false};
  std::uint64_t scanned_entities{0};
  std::size_t entity_cursor{0};

  // Timing.
  double last_refresh_time{0.0};
  double last_scan_time{0.0};

  std::string status;
  std::string error;
};

OmniSearchState& st() {
  static OmniSearchState s;
  return s;
}

int kind_priority(ResultKind k) {
  // Lower is earlier.
  switch (k) {
    case ResultKind::Action: return 0;
    case ResultKind::WorkspacePreset: return 1;
    case ResultKind::LayoutProfile: return 2;
    case ResultKind::Window: return 3;
    case ResultKind::Entity: return 4;
    case ResultKind::Doc: return 5;
    case ResultKind::JsonNode: return 6;
  }
  return 9;
}

void sort_results(OmniSearchState& s) {
  std::stable_sort(s.results.begin(), s.results.end(), [](const SearchResult& a, const SearchResult& b) {
    if (a.score != b.score) return a.score > b.score;
    const int pa = kind_priority(a.kind);
    const int pb = kind_priority(b.kind);
    if (pa != pb) return pa < pb;
    // Tiebreakers for stability.
    if (a.type != b.type) return a.type < b.type;
    return a.path < b.path;
  });
}

void open_in_json_explorer(UIState& ui, const std::string& path) {
  ui.show_json_explorer_window = true;
  ui.request_json_explorer_goto_path = path;
}

void open_doc_in_codex(UIState& ui, const std::string& ref) {
  ui.show_help_window = true;
  ui.request_help_tab = HelpTab::Docs;
  ui.request_open_doc_ref = ref;
}

bool kind_to_nav_target(std::string_view kind, std::uint64_t id, NavTarget& out) {
  const std::string k = to_lower_copy(kind);
  if (k == "ships" || k == "ship") {
    out.kind = NavTargetKind::Ship;
    out.id = static_cast<Id>(id);
    return true;
  }
  if (k == "colonies" || k == "colony") {
    out.kind = NavTargetKind::Colony;
    out.id = static_cast<Id>(id);
    return true;
  }
  if (k == "bodies" || k == "body") {
    out.kind = NavTargetKind::Body;
    out.id = static_cast<Id>(id);
    return true;
  }
  if (k == "systems" || k == "system" || k == "star_systems") {
    out.kind = NavTargetKind::System;
    out.id = static_cast<Id>(id);
    return true;
  }
  return false;
}

const char* nav_kind_label(NavTargetKind k) {
  switch (k) {
    case NavTargetKind::System: return "System";
    case NavTargetKind::Ship: return "Ship";
    case NavTargetKind::Colony: return "Colony";
    case NavTargetKind::Body: return "Body";
  }
  return "Entity";
}

void ensure_docs_scanned(OmniSearchState& s) {
  if (s.docs_scanned) return;
  s.docs_scanned = true;
  s.docs.clear();
  s.doc_by_ref.clear();
  s.docs_error.clear();

  // Prefer docs shipped with the build.
  scan_dir_for_docs(s.docs, s.doc_by_ref, std::filesystem::path("data") / "docs", /*from_data=*/true);
  // Dev builds: repo docs.
  scan_dir_for_docs(s.docs, s.doc_by_ref, std::filesystem::path("docs"), /*from_data=*/false);

  // Extra single-file docs (dev).
  const std::vector<std::string> extra = {"README.md", "PATCH_NOTES.md", "PATCH_PACK_NOTES.md"};
  for (const auto& p : extra) {
    try {
      const std::filesystem::path fp(p);
      if (!std::filesystem::exists(fp) || !std::filesystem::is_regular_file(fp)) continue;
      const std::string contents = nebula4x::read_text_file(fp.string());
      DocEntry e;
      e.from_data = false;
      e.display_path = fp.filename().generic_string();
      e.ref = normalize_doc_ref(e.display_path);
      e.abs_path = fp.string();
      e.lines = split_lines(contents);
      e.title = extract_title_from_markdown(e.lines, fp.stem().string());
      e.raw_all = contents;
      e.lower_all = to_lower_copy(contents);
      add_doc(s.docs, s.doc_by_ref, e);
    } catch (...) {
      // ignore
    }
  }

  std::stable_sort(s.docs.begin(), s.docs.end(), [](const DocEntry& a, const DocEntry& b) {
    if (a.from_data != b.from_data) return a.from_data > b.from_data;
    if (a.title != b.title) return a.title < b.title;
    return a.display_path < b.display_path;
  });
}

void rebuild_entity_list_if_needed(OmniSearchState& s) {
  if (!s.doc_loaded || !s.root) return;
  if (!ensure_game_entity_index(*s.root, s.doc_revision)) return;
  if (s.entity_revision == s.doc_revision) return;

  s.entity_revision = s.doc_revision;
  s.entities.clear();
  s.entities.reserve(game_entity_index().by_id.size());

  for (const auto& kv : game_entity_index().by_id) {
    s.entities.push_back(kv.second);
  }

  // Stable-ish ordering: kind then name then id.
  std::stable_sort(s.entities.begin(), s.entities.end(), [](const GameEntityIndexEntry& a, const GameEntityIndexEntry& b) {
    if (a.kind != b.kind) return a.kind < b.kind;
    if (a.name != b.name) return a.name < b.name;
    return a.id < b.id;
  });
}

void refresh_doc(OmniSearchState& s, Simulation& sim, const UIState& ui, bool force) {
  const double now = ImGui::GetTime();

  // Ensure we have a reasonably fresh game-state JSON snapshot.
  ensure_game_json_cache(sim, now, ui.omni_search_refresh_sec, force);
  const auto& cache = game_json_cache();
  s.root = cache.root;
  s.doc_revision = cache.revision;
  s.doc_loaded = static_cast<bool>(s.root);
  s.last_refresh_time = now;

  if (s.doc_loaded && s.root) {
    rebuild_entity_list_if_needed(s);
  }
}

void add_action_results(OmniSearchState& s, const UIState& ui) {
  const bool case_sensitive = ui.omni_search_case_sensitive;
  const std::string q = s.effective_query;

  for (const auto& a : kActions) {
    // Candidate strings.
    const std::string label = std::string(a.group) + " / " + a.label;
    int sc = -1;

    if (q.empty()) {
      sc = 0;
    } else {
      sc = std::max(sc, fuzzy_score(label, q, case_sensitive));
      sc = std::max(sc, fuzzy_score(a.label, q, case_sensitive));
      sc = std::max(sc, fuzzy_score(a.group, q, case_sensitive));
      if (a.desc) sc = std::max(sc, fuzzy_score(a.desc, q, case_sensitive));
    }

    if (sc < 0) continue;

    SearchResult r;
    r.kind = ResultKind::Action;
    r.score = sc + 800; // prioritize actions slightly.
    r.type = "action";
    r.path = label;
    r.preview = a.desc ? a.desc : "";
    r.hint = a.shortcut_hint ? a.shortcut_hint : "";
    r.action_id = static_cast<int>(a.id);

    s.results.push_back(std::move(r));
  }
}


std::string window_launch_mode_label(WindowLaunchMode m) {
  switch (m) {
    case WindowLaunchMode::Docked: return "docked";
    case WindowLaunchMode::Popup: return "popup";
  }
  return "docked";
}

void add_workspace_preset_results(OmniSearchState& s, const UIState& ui, bool include_all_when_empty) {
  if (!ui.omni_search_match_layouts && !s.ui_only) return;

  std::size_t n = 0;
  const WorkspacePresetInfo* presets = workspace_preset_infos(&n);
  if (!presets || n == 0) return;

  const bool case_sensitive = ui.omni_search_case_sensitive;
  const std::string q = s.effective_query;

  for (std::size_t i = 0; i < n; ++i) {
    const auto& p = presets[i];
    int sc = -1;

    if (q.empty()) {
      if (!include_all_when_empty) continue;
      sc = 0;
    } else {
      sc = std::max(sc, fuzzy_score(p.name, q, case_sensitive));
      if (p.desc) sc = std::max(sc, fuzzy_score(p.desc, q, case_sensitive));
      sc = std::max(sc, fuzzy_score(std::string("workspace ") + p.name, q, case_sensitive));
    }

    if (sc < 0) continue;

    SearchResult r;
    r.kind = ResultKind::WorkspacePreset;
    r.score = sc + 980;
    r.type = "workspace";
    r.path = p.name ? p.name : "";
    r.preview = p.desc ? p.desc : "";
    r.workspace_preset = p.name ? p.name : "";
    s.results.push_back(std::move(r));
  }
}

void add_layout_profile_results(OmniSearchState& s, const UIState& ui, bool include_all_when_empty) {
  if (!ui.omni_search_match_layouts && !s.ui_only) return;

  const bool case_sensitive = ui.omni_search_case_sensitive;
  const std::string q = s.effective_query;

  const std::string dir =
      (ui.layout_profiles_dir[0] ? std::string(ui.layout_profiles_dir) : std::string("ui_layouts"));

  std::vector<std::string> names;
  try {
    names = scan_layout_profile_names(dir.c_str());
  } catch (...) {
    // Ignore filesystem errors; Layout Profiles window will show details.
    return;
  }

  const std::string active = sanitize_layout_profile_name(ui.layout_profile);

  for (const auto& name : names) {
    int sc = -1;

    if (q.empty()) {
      if (!include_all_when_empty) continue;
      sc = 0;
    } else {
      sc = std::max(sc, fuzzy_score(name, q, case_sensitive));
      sc = std::max(sc, fuzzy_score(std::string("layout ") + name, q, case_sensitive));
    }

    if (sc < 0) continue;

    SearchResult r;
    r.kind = ResultKind::LayoutProfile;
    r.score = sc + 920;
    r.type = "layout";
    r.path = name;
    r.layout_profile = name;

    std::string pv;
    if (name == active) pv = "Active";
    else pv = "Layout profile";
    pv += " • ";
    pv += make_layout_profile_ini_path(dir.c_str(), name);
    r.preview = std::move(pv);

    s.results.push_back(std::move(r));
  }
}

void add_window_results(OmniSearchState& s, const UIState& ui, bool include_all_when_empty) {
  if (!ui.omni_search_match_windows && !s.ui_only) return;

  const bool case_sensitive = ui.omni_search_case_sensitive;
  const std::string q = s.effective_query;

  for (const auto& spec : window_specs()) {
    const bool is_open = ui.*(spec.open_flag);

    int sc = -1;
    if (q.empty()) {
      if (!include_all_when_empty) continue;
      sc = 0;
    } else {
      const std::string label = std::string(spec.category) + " / " + spec.label;
      sc = std::max(sc, fuzzy_score(label, q, case_sensitive));
      sc = std::max(sc, fuzzy_score(spec.label, q, case_sensitive));
      sc = std::max(sc, fuzzy_score(spec.category, q, case_sensitive));
      sc = std::max(sc, fuzzy_score(spec.title, q, case_sensitive));
      sc = std::max(sc, fuzzy_score(spec.id, q, case_sensitive));
    }

    if (sc < 0) continue;

    SearchResult r;
    r.kind = ResultKind::Window;
    r.score = sc + 880 + (is_open ? 20 : 0);
    r.type = "window";
    r.path = std::string(spec.category) + " / " + spec.label;
    r.window_id = spec.id;

    std::string pv = is_open ? "Open" : "Closed";

    if (spec.core) {
      pv += " • core";
    }

    if (spec.supports_popup) {
      pv += " • ";
      pv += window_launch_mode_label(effective_launch_mode(ui, spec));
      if (auto it = ui.window_launch_overrides.find(spec.id); it != ui.window_launch_overrides.end()) {
        (void)it;
        pv += " (override)";
      }
    } else {
      pv += " • fixed";
    }

    pv += " • id:";
    pv += spec.id;

    r.preview = std::move(pv);

    s.results.push_back(std::move(r));
  }
}


void add_doc_results(OmniSearchState& s, const UIState& ui, bool include_all_when_empty) {
  if (!ui.omni_search_match_docs) return;

  ensure_docs_scanned(s);
  if (!s.docs_error.empty()) return;

  const bool case_sensitive = ui.omni_search_case_sensitive;
  const std::string q = s.effective_query;

  for (const auto& d : s.docs) {
    int sc = -1;

    if (q.empty()) {
      if (!include_all_when_empty) continue;
      sc = 0;
    } else {
      sc = std::max(sc, fuzzy_score(d.title, q, case_sensitive));
      sc = std::max(sc, fuzzy_score(d.display_path, q, case_sensitive));
      sc = std::max(sc, fuzzy_score(d.ref, q, case_sensitive));
    }

    // Content hit + snippet (case insensitive uses lower_all).
    std::string snippet;
    bool content_hit = false;
    if (!q.empty()) {
      if (case_sensitive) {
        content_hit = d.raw_all.find(q) != std::string::npos;
      } else {
        content_hit = d.lower_all.find(to_lower_copy(q)) != std::string::npos;
      }
      if (content_hit) {
        snippet = doc_find_snippet(d, q, case_sensitive);
        sc = std::max(sc, 1400); // boost for content hit.
      }
    }

    if (sc < 0) continue;

    SearchResult r;
    r.kind = ResultKind::Doc;
    r.score = sc + 350;
    r.type = "doc";
    r.path = d.title;
    r.doc_ref = d.ref;
    r.doc_display_path = d.display_path;
    r.doc_abs_path = d.abs_path;

    if (!snippet.empty()) {
      r.preview = d.display_path + " — " + snippet;
    } else {
      r.preview = d.display_path;
    }

    s.results.push_back(std::move(r));
  }
}

void add_nav_shortcuts_if_applicable(OmniSearchState& s,
                                    const Simulation& sim,
                                    const UIState& ui,
                                    Id selected_ship,
                                    Id selected_colony,
                                    Id selected_body) {
  // Only used when in entity-only mode with an empty query: show current/bookmarks/history.
  if (!s.entity_only) return;
  if (!s.effective_query.empty()) return;

  // Current selection.
  {
    const NavTarget cur = current_nav_target(sim, selected_ship, selected_colony, selected_body);
    if (cur.id != kInvalidId) {
      SearchResult r;
      r.kind = ResultKind::Entity;
      r.score = 6000;
      r.type = nav_kind_label(cur.kind);
      r.path = nav_target_label(sim, cur, /*include_kind_prefix=*/false);
      r.preview = "Current selection";
      r.nav_valid = true;
      r.nav_kind = cur.kind;
      r.nav_id = cur.id;
      r.entity_id = static_cast<std::uint64_t>(cur.id);
      s.results.push_back(std::move(r));
    }
  }

  // Bookmarks.
  for (const auto& b : ui.nav_bookmarks) {
    if (b.target.id == kInvalidId) continue;
    SearchResult r;
    r.kind = ResultKind::Entity;
    r.score = 5200;
    r.type = nav_kind_label(b.target.kind);
    r.path = nav_target_label(sim, b.target, /*include_kind_prefix=*/false);
    r.preview = b.name.empty() ? "Bookmark" : ("Bookmark: " + b.name);
    r.nav_valid = true;
    r.nav_kind = b.target.kind;
    r.nav_id = b.target.id;
    r.entity_id = static_cast<std::uint64_t>(b.target.id);
    s.results.push_back(std::move(r));
  }

  // Recent history (newest first), capped.
  constexpr int kMaxHist = 40;
  int added = 0;
  for (int i = (int)ui.nav_history.size() - 1; i >= 0 && added < kMaxHist; --i) {
    const NavTarget& t = ui.nav_history[i];
    if (t.id == kInvalidId) continue;

    SearchResult r;
    r.kind = ResultKind::Entity;
    r.score = 4800 - added; // stable ordering
    r.type = nav_kind_label(t.kind);
    r.path = nav_target_label(sim, t, /*include_kind_prefix=*/false);
    r.preview = "History";
    r.nav_valid = true;
    r.nav_kind = t.kind;
    r.nav_id = t.id;
    r.entity_id = static_cast<std::uint64_t>(t.id);
    s.results.push_back(std::move(r));
    added++;
  }
}

void start_scan(OmniSearchState& s,
                Simulation& sim,
                const UIState& ui,
                Id selected_ship,
                Id selected_colony,
                Id selected_body) {
  s.results.clear();
  s.selected_idx = -1;
  s.error.clear();
  s.status.clear();

  s.scanning_json = false;
  s.scanning_entities = false;
  s.truncated = false;

  s.scanned_nodes = 0;
  s.stack.clear();

  s.scanned_entities = 0;
  s.entity_cursor = 0;

  // Parse query + prefix mode.
  s.action_only = false;
  s.entity_only = false;
  s.docs_only = false;
  s.ui_only = false;

  s.effective_query = trim_copy(s.last_query);
  if (!s.effective_query.empty()) {
    const char c0 = s.effective_query[0];
    if (c0 == '>') {
      s.action_only = true;
      s.effective_query.erase(s.effective_query.begin());
    } else if (c0 == '@') {
      s.entity_only = true;
      s.effective_query.erase(s.effective_query.begin());
    } else if (c0 == '?') {
      s.docs_only = true;
      s.effective_query.erase(s.effective_query.begin());
    } else if (c0 == '#') {
      s.ui_only = true;
      s.effective_query.erase(s.effective_query.begin());
    }
  }
  s.effective_query = trim_copy(s.effective_query);

  // Ensure indices are ready (UI-only mode intentionally skips heavy indices).
  if (!s.ui_only) {
    if (ui.omni_search_match_docs || s.docs_only) ensure_docs_scanned(s);
    if (ui.omni_search_match_entities) rebuild_entity_list_if_needed(s);
  }

  // Add nav shortcuts for @ with empty query.
  if (s.entity_only && s.effective_query.empty()) {
    add_nav_shortcuts_if_applicable(s, sim, ui, selected_ship, selected_colony, selected_body);
    // Also show a hint action to open the Navigator.
    {
      SearchResult r;
      r.kind = ResultKind::Action;
      r.score = 5600;
      r.type = "action";
      r.path = "Navigation / Open Navigator";
      r.preview = "Bookmarks + selection history.";
      r.action_id = static_cast<int>(OmniActionId::ToggleNavigator);
      s.results.push_back(std::move(r));
    }
    s.results_dirty_sort = true;
    sort_results(s);
    if (!s.results.empty()) s.selected_idx = 0;
    s.last_scan_time = ImGui::GetTime();
    return;
  }

  // Actions (unless docs-only mode).
  if (!s.docs_only) {
    // In @ mode, don't list the entire action catalog on an empty query.
    if (!s.entity_only || !s.effective_query.empty()) {
      // In # mode, the primary UI surface is windows/layouts. Only include actions when searching.
      if (!s.ui_only || !s.effective_query.empty()) {
        add_action_results(s, ui);
      }
    }
  }

  // UI surfaces: built-in workspaces, layout profiles, and window launchers.
  if (!s.action_only && !s.docs_only && !s.entity_only) {
    const bool include_all_when_empty = s.ui_only;
    if (s.ui_only || !s.effective_query.empty()) {
      add_workspace_preset_results(s, ui, include_all_when_empty);
      add_layout_profile_results(s, ui, include_all_when_empty);
      add_window_results(s, ui, include_all_when_empty);
    }
  }

  // Docs (skipped in UI-only mode).
  if (!s.ui_only) {
    if (s.docs_only) {
      add_doc_results(s, ui, /*include_all_when_empty=*/true);
    } else if (ui.omni_search_match_docs && !s.action_only) {
      add_doc_results(s, ui, /*include_all_when_empty=*/false);
    }
  }

  // Entities.
  if (ui.omni_search_match_entities && !s.action_only && !s.docs_only && !s.ui_only && s.doc_loaded && !s.entities.empty()) {
    // Default mode: only scan entities when query isn't empty.
    if (s.entity_only || !s.effective_query.empty()) {
      s.scanning_entities = true;
    }
  }

  // JSON scan (only when not action-only/docs-only, and when query isn't empty OR when explicitly allowed by match flags).
  if (!s.action_only && !s.docs_only && !s.ui_only && s.doc_loaded && s.root) {
    // If query empty, JSON scan is usually noisy; keep it off unless entity-only is false and user explicitly wants (keys/values).
    if (!s.effective_query.empty()) {
      s.scanning_json = true;
      ScanFrame f;
      f.v = s.root.get();
      f.path = "/";
      f.key.clear();
      s.stack.push_back(std::move(f));
    }
  }

  s.results_dirty_sort = true;
  sort_results(s);
  if (!s.results.empty()) s.selected_idx = 0;

  s.last_scan_time = ImGui::GetTime();
}

// Emit a JSON node result.
void emit_json_result(OmniSearchState& s,
                      const UIState& ui,
                      const std::string& path,
                      const std::string& key,
                      const nebula4x::json::Value& v,
                      int score) {
  const int max_results = std::max(200, ui.omni_search_max_results);
  if ((int)s.results.size() >= max_results) {
    s.truncated = true;
    s.scanning_json = false;
    s.stack.clear();
    return;
  }

  SearchResult r;
  r.kind = ResultKind::JsonNode;
  r.score = score;
  r.path = path;
  r.key = key;
  r.type = json_type_name(v);
  r.preview = json_node_preview(v, 140);
  r.array_of_objects = looks_like_array_of_objects(v);
  r.is_scalar = v.is_null() || v.is_bool() || v.is_number() || v.is_string();
  s.results.push_back(std::move(r));
  s.results_dirty_sort = true;
}

void emit_entity_result(OmniSearchState& s,
                        const UIState& ui,
                        const Simulation& sim,
                        const GameEntityIndexEntry& e,
                        int score) {
  const int max_results = std::max(200, ui.omni_search_max_results);
  if ((int)s.results.size() >= max_results) {
    s.truncated = true;
    s.scanning_entities = false;
    return;
  }

  SearchResult r;
  r.kind = ResultKind::Entity;
  r.score = score;
  r.entity_id = e.id;
  r.entity_kind = e.kind;
  r.entity_json_path = e.path;

  NavTarget t;
  if (kind_to_nav_target(e.kind, e.id, t)) {
    r.nav_valid = true;
    r.nav_kind = t.kind;
    r.nav_id = t.id;
    r.type = nav_kind_label(t.kind);
    r.path = nav_target_label(sim, t, /*include_kind_prefix=*/false);
  } else {
    r.nav_valid = false;
    r.type = e.kind;
    if (!e.name.empty()) {
      r.path = "#" + std::to_string((unsigned long long)e.id) + "  " + e.name;
    } else {
      r.path = "#" + std::to_string((unsigned long long)e.id);
    }
  }

  if (!e.name.empty() && r.preview.empty()) {
    r.preview = truncate_middle(e.path, 90);
  } else {
    r.preview = truncate_middle(e.path, 90);
  }

  s.results.push_back(std::move(r));
  s.results_dirty_sort = true;
}

void scan_step(OmniSearchState& s, Simulation& sim, const UIState& ui) {
  const int total_budget = std::max(50, ui.omni_search_nodes_per_frame);
  int entity_budget = 0;
  int json_budget = 0;

  if (s.scanning_entities && s.scanning_json) {
    entity_budget = std::clamp(total_budget / 3, 50, 25000);
    json_budget = std::max(50, total_budget - entity_budget);
  } else if (s.scanning_entities) {
    entity_budget = total_budget;
    json_budget = 0;
  } else if (s.scanning_json) {
    entity_budget = 0;
    json_budget = total_budget;
  }

  // --- Entity scan ---
  if (s.scanning_entities) {
    const bool case_sensitive = ui.omni_search_case_sensitive;
    const std::string q = s.effective_query;
    // Guard: if query empty and not entity-only mode, don't scan entities.
    if (q.empty() && !s.entity_only) {
      s.scanning_entities = false;
    } else {
      for (int i = 0; i < entity_budget && s.entity_cursor < s.entities.size(); ++i, ++s.entity_cursor) {
        const auto& e = s.entities[s.entity_cursor];
        s.scanned_entities++;

        int sc = -1;
        if (q.empty()) {
          sc = 0;
        } else {
          // Match on combined label.
          std::string id_s = std::to_string((unsigned long long)e.id);
          std::string combo = e.kind + " " + id_s + " " + e.name;
          sc = std::max(sc, fuzzy_score(combo, q, case_sensitive));
          sc = std::max(sc, fuzzy_score(e.name, q, case_sensitive));
          sc = std::max(sc, fuzzy_score(e.kind, q, case_sensitive));
          sc = std::max(sc, fuzzy_score(id_s, q, /*case_sensitive=*/true));
          sc = std::max(sc, fuzzy_score(e.path, q, case_sensitive));
        }

        if (sc < 0) continue;

        // Slight boost for exact id match.
        if (!q.empty()) {
          const std::string id_s = std::to_string((unsigned long long)e.id);
          if ((case_sensitive ? id_s == q : to_lower_copy(id_s) == to_lower_copy(q))) sc += 300;
        }

        emit_entity_result(s, ui, sim, e, sc + 500);
        if (!s.scanning_entities) break; // truncated
      }

      if (s.entity_cursor >= s.entities.size()) s.scanning_entities = false;
    }
  }

  // --- JSON scan ---
  if (s.scanning_json && s.root) {
    const bool case_sensitive = ui.omni_search_case_sensitive;
    const std::string q = s.effective_query;
    const bool match_keys = ui.omni_search_match_keys;
    const bool match_values = ui.omni_search_match_values;

    if (!match_keys && !match_values) {
      // Shouldn't happen (prefs clamp), but avoid busy loops.
      s.scanning_json = false;
      s.stack.clear();
    } else {
      for (int i = 0; i < json_budget && !s.stack.empty(); ++i) {
        ScanFrame fr = std::move(s.stack.back());
        s.stack.pop_back();
        s.scanned_nodes++;

        if (!fr.v) continue;
        const auto& v = *fr.v;

        // Score candidates.
        int best_sc = -1;

        if (!q.empty()) {
          if (match_keys && !fr.key.empty()) {
            best_sc = std::max(best_sc, fuzzy_score(fr.key, q, case_sensitive));
          }

          if (match_values && (v.is_string() || v.is_number() || v.is_bool() || v.is_null())) {
            const std::string pv = json_node_preview(v, 240);
            best_sc = std::max(best_sc, fuzzy_score(pv, q, case_sensitive));
          }

          // Also score the full path.
          best_sc = std::max(best_sc, fuzzy_score(fr.path, q, case_sensitive));
        } else {
          // We don't scan JSON when query is empty (see start_scan).
          best_sc = -1;
        }

        if (best_sc >= 0) {
          emit_json_result(s, ui, fr.path, fr.key, v, best_sc);
          if (!s.scanning_json) break;
        }

        // Traverse children.
        if (v.is_object()) {
          const auto* o = v.as_object();
          if (!o) continue;
          for (const auto& kv : *o) {
            ScanFrame child;
            child.v = &kv.second;
            child.key = kv.first;
            child.path = nebula4x::json_pointer_join(fr.path, kv.first);
            s.stack.push_back(std::move(child));
          }
        } else if (v.is_array()) {
          const auto* a = v.as_array();
          if (!a) continue;
          for (std::size_t idx = 0; idx < a->size(); ++idx) {
            ScanFrame child;
            child.v = &(*a)[idx];
            child.key = std::to_string(idx);
            child.path = nebula4x::json_pointer_join_index(fr.path, idx);
            s.stack.push_back(std::move(child));
          }
        }
      }

      if (s.stack.empty()) s.scanning_json = false;
    }
  }

  if (s.results_dirty_sort) {
    sort_results(s);
    s.results_dirty_sort = false;
    // Keep selection stable as much as possible.
    if (s.selected_idx < 0 && !s.results.empty()) s.selected_idx = 0;
    if (s.selected_idx >= (int)s.results.size()) s.selected_idx = (int)s.results.size() - 1;
  }
}

bool activate_result(const SearchResult& r,
                     Simulation& sim,
                     UIState& ui,
                     Id& selected_ship,
                     Id& selected_colony,
                     Id& selected_body) {
  switch (r.kind) {
    case ResultKind::Action: {
      invoke_omni_action(ui, static_cast<OmniActionId>(r.action_id));
      return true;
    }
    case ResultKind::Window: {
      if (r.window_id.empty()) return false;
      const WindowSpec* spec = find_window_spec(r.window_id.c_str());
      if (!spec) return false;

      // Holding Shift forces an immediate pop-out (floating) if supported.
      const ImGuiIO& io = ImGui::GetIO();
      if (spec->supports_popup && io.KeyShift) {
        request_popout(ui, spec->id);
        return true;
      }

      // Default behavior: toggle open/close.
      const bool is_open = ui.*(spec->open_flag);
      ui.*(spec->open_flag) = !is_open;
      return true;
    }
    case ResultKind::WorkspacePreset: {
      if (!r.workspace_preset.empty()) {
        apply_workspace_preset(r.workspace_preset.c_str(), ui);
        ui.layout_profile_status = std::string("Applied workspace preset: ") + r.workspace_preset;
        ui.layout_profile_status_time = ImGui::GetTime();
        return true;
      }
      return false;
    }
    case ResultKind::LayoutProfile: {
      if (!r.layout_profile.empty()) {
        const std::string sanitized = sanitize_layout_profile_name(r.layout_profile);
        std::snprintf(ui.layout_profile, sizeof(ui.layout_profile), "%s", sanitized.c_str());
        ui.request_reload_layout_profile = true;
        ui.layout_profile_status = std::string("Switched to layout profile: ") + sanitized;
        ui.layout_profile_status_time = ImGui::GetTime();
        return true;
      }
      return false;
    }
    case ResultKind::Doc: {
      if (!r.doc_ref.empty()) {
        open_doc_in_codex(ui, r.doc_ref);
        return true;
      }
      return false;
    }
    case ResultKind::Entity: {
      if (r.nav_valid && r.nav_id != kInvalidId) {
        const NavTarget t{r.nav_kind, r.nav_id};
        if (nav_target_exists(sim, t)) {
          apply_nav_target(sim, ui, selected_ship, selected_colony, selected_body, t, ui.nav_open_windows_on_jump);
          return true;
        }
      }
      // Fallback: open inspector / JSON.
      if (r.entity_id != 0) {
        ui.show_entity_inspector_window = true;
        ui.entity_inspector_id = r.entity_id;
        return true;
      }
      if (!r.entity_json_path.empty()) {
        open_in_json_explorer(ui, r.entity_json_path);
        return true;
      }
      return false;
    }
    case ResultKind::JsonNode: {
      open_in_json_explorer(ui, r.path);
      return true;
    }
  }
  return false;
}

} // namespace

void draw_omni_search_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  if (!ui.show_omni_search_window) return;

  OmniSearchState& s = st();

  ImGui::SetNextWindowSize(ImVec2(980, 620), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("OmniSearch", &ui.show_omni_search_window, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return;
  }

  // Refresh JSON snapshot on open, and periodically if enabled.
  const double now = ImGui::GetTime();
  if (!s.doc_loaded) {
    refresh_doc(s, sim, ui, /*force=*/true);
  } else if (ui.omni_search_auto_refresh && (now - s.last_refresh_time) >= ui.omni_search_refresh_sec) {
    refresh_doc(s, sim, ui, /*force=*/false);
  }

  // Top controls row.
  {
    if (ImGui::Button("Refresh JSON")) {
      refresh_doc(s, sim, ui, /*force=*/true);
      start_scan(s, sim, ui, selected_ship, selected_colony, selected_body);
    }
    ImGui::SameLine();
    if (ImGui::Button("Rescan Docs")) {
      s.docs_scanned = false;
      s.docs.clear();
      s.doc_by_ref.clear();
      ensure_docs_scanned(s);
      start_scan(s, sim, ui, selected_ship, selected_colony, selected_body);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto-refresh", &ui.omni_search_auto_refresh);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Periodically refresh the cached JSON snapshot used for search.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    ImGui::SliderFloat("##refresh_sec", &ui.omni_search_refresh_sec, 0.10f, 3.0f, "%.2fs");
    ImGui::SameLine();
    ImGui::TextDisabled("Docs: %d", (int)s.docs.size());
    ImGui::SameLine();
    ImGui::TextDisabled("Entities: %d", (int)s.entities.size());
  }

  // Scope toggles.
  {
    ImGui::Checkbox("Keys", &ui.omni_search_match_keys);
    ImGui::SameLine();
    ImGui::Checkbox("Values", &ui.omni_search_match_values);
    ImGui::SameLine();
    ImGui::Checkbox("Entities", &ui.omni_search_match_entities);
    ImGui::SameLine();
    ImGui::Checkbox("Docs", &ui.omni_search_match_docs);
    ImGui::SameLine();
    ImGui::Checkbox("Windows", &ui.omni_search_match_windows);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Include window launchers in search results.");
    ImGui::SameLine();
    ImGui::Checkbox("Layouts", &ui.omni_search_match_layouts);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Include layout profiles + workspace presets in search results.");
    ImGui::SameLine();
    ImGui::Checkbox("Case", &ui.omni_search_case_sensitive);

    if (!ui.omni_search_match_keys && !ui.omni_search_match_values) ui.omni_search_match_keys = true;
  }

  // Query input.
  bool query_enter = false;
  {
    // Focus the query field when the window is opened.
    if (ImGui::IsWindowAppearing()) {
      ImGui::SetKeyboardFocusHere();
    }

    ImGui::SetNextItemWidth(-FLT_MIN);
    query_enter = ImGui::InputTextWithHint("##omni_query",
                                          "Search…  (prefix: '>' commands, '@' entities, '?' docs, '#' UI)",
                                          s.query_buf,
                                          sizeof(s.query_buf),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
  }

  // Detect query changes.
  const std::string q_now = std::string(s.query_buf);
  const bool query_changed = (q_now != s.last_query);

  if (query_changed) {
    s.last_query = q_now;
    start_scan(s, sim, ui, selected_ship, selected_colony, selected_body);
  } else if (ImGui::IsWindowAppearing()) {
    // First open: populate initial results for the current query.
    start_scan(s, sim, ui, selected_ship, selected_colony, selected_body);
  }

  // If scanning, advance.
  if (s.scanning_entities || s.scanning_json) {
    scan_step(s, sim, ui);
  }

  // Status line.
  {
    std::string status;
    status.reserve(256);

    if (!s.doc_loaded) {
      status = "JSON: (not loaded)";
    } else {
      status = "JSON rev " + std::to_string((unsigned long long)s.doc_revision);
    }

    if (s.scanning_entities || s.scanning_json) {
      status += "  | scanning";
      if (s.scanning_entities) status += " entities";
      if (s.scanning_entities && s.scanning_json) status += "+";
      if (s.scanning_json) status += " json";
    }

    status += "  | results " + std::to_string((unsigned long long)s.results.size());
    if (s.truncated) status += " (capped)";
    status += "  | scanned: " + std::to_string((unsigned long long)s.scanned_entities) + " ent, " +
              std::to_string((unsigned long long)s.scanned_nodes) + " nodes";

    ImGui::TextDisabled("%s", status.c_str());
  }

  ImGui::Separator();

  // Keyboard navigation (while typing): Ctrl+Up/Down/… moves selection.
  {
    const ImGuiIO& io = ImGui::GetIO();
    const int n = (int)s.results.size();
    if (n > 0) {
      auto clamp = [&](int idx) {
        if (idx < 0) return 0;
        if (idx >= n) return n - 1;
        return idx;
      };

      const int cur = (s.selected_idx >= 0) ? s.selected_idx : 0;

      if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_UpArrow)) s.selected_idx = clamp(cur - 1);
      if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_DownArrow)) s.selected_idx = clamp(cur + 1);
      if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_PageUp)) s.selected_idx = clamp(cur - 10);
      if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_PageDown)) s.selected_idx = clamp(cur + 10);
      if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Home)) s.selected_idx = 0;
      if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_End)) s.selected_idx = n - 1;
    } else {
      s.selected_idx = -1;
    }
  }

  // Split view: results list (left) + details panel (right).
  const float left_w = ImGui::GetContentRegionAvail().x * 0.58f;
  ImGui::BeginChild("##omni_left", ImVec2(left_w, 0.0f), true);
  {
    if (s.results.empty()) {
      ImGui::TextDisabled("No results. Try:");
      ImGui::BulletText("> map");
      ImGui::BulletText("@ terra");
      ImGui::BulletText("? hotkeys");
      ImGui::BulletText("# layout");
      ImGui::BulletText("research labs");
    } else {
      const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV |
                                    ImGuiTableFlags_Resizable;

      if (ImGui::BeginTable("##omni_table", 4, flags)) {
        ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Path / Title", ImGuiTableColumnFlags_WidthStretch, 0.55f);
        ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin((int)s.results.size());
        while (clipper.Step()) {
          for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const SearchResult& r = s.results[(std::size_t)i];
            ImGui::PushID(i);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);

            const bool selected = (i == s.selected_idx);
            const bool row_clicked =
                ImGui::Selectable("##row", selected,
                                 ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap);
            if (row_clicked) s.selected_idx = i;

            // Double-click to activate.
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
              if (activate_result(r, sim, ui, selected_ship, selected_colony, selected_body)) {
                ui.show_omni_search_window = false;
              }
            }

            // Context menu: quick actions.
            if (ImGui::BeginPopupContextItem("##omni_ctx")) {
              bool close = false;

              if (ImGui::MenuItem("Activate")) {
                if (activate_result(r, sim, ui, selected_ship, selected_colony, selected_body)) close = true;
              }

              if (r.kind == ResultKind::Window) {
                const WindowSpec* spec = find_window_spec(r.window_id.c_str());
                if (spec) {
                  const bool is_open = ui.*(spec->open_flag);
                  if (ImGui::MenuItem(is_open ? "Close" : "Open")) ui.*(spec->open_flag) = !is_open;
                  if (spec->supports_popup && ImGui::MenuItem("Pop out (floating)")) {
                    ui.*(spec->open_flag) = true;
                    request_popout(ui, spec->id);
                  }
                }
              } else if (r.kind == ResultKind::JsonNode) {
                if (ImGui::MenuItem("Open in JSON Explorer")) {
                  open_in_json_explorer(ui, r.path);
                  close = true;
                }
                if (ImGui::MenuItem("Pin to Watchboard")) {
                  (void)add_watch_item(ui, r.path);
                  ui.show_watchboard_window = true;
                }

                // Power shortcuts for arrays (spawn tooling around a JSON array).
                if (s.root) {
                  std::string err;
                  const auto* v = nebula4x::resolve_json_pointer(*s.root, r.path,
                                                               /*accept_root_slash=*/true,
                                                               &err);
                  if (v && v->is_array()) {
                    ImGui::Separator();
                    if (ImGui::MenuItem("Create Data Lens")) {
                      (void)add_json_table_view(ui, r.path);
                      ui.show_data_lenses_window = true;
                    }
                    if (ImGui::MenuItem("Create Dashboard")) {
                      (void)add_json_dashboard_for_path(ui, r.path);
                      ui.show_dashboards_window = true;
                    }
                    if (ImGui::MenuItem("Create Pivot")) {
                      (void)add_json_pivot_for_path(ui, r.path);
                      ui.show_pivot_tables_window = true;
                    }
                  } else {
                    (void)err;
                  }
                }
              } else if (r.kind == ResultKind::Entity) {
                if (ImGui::MenuItem("Inspect")) {
                  ui.show_entity_inspector_window = true;
                  ui.entity_inspector_id = r.entity_id;
                  close = true;
                }
                if (!r.entity_json_path.empty() && ImGui::MenuItem("Open JSON")) {
                  open_in_json_explorer(ui, r.entity_json_path);
                  close = true;
                }
              } else if (r.kind == ResultKind::Doc) {
                if (ImGui::MenuItem("Open in Codex")) {
                  open_doc_in_codex(ui, r.doc_ref);
                  close = true;
                }
              }

              if (!r.path.empty() && ImGui::MenuItem("Copy path/title")) {
                ImGui::SetClipboardText(r.path.c_str());
              }

              if (close) ui.show_omni_search_window = false;
              ImGui::EndPopup();
            }

            // Row contents.
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", r.score);

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(r.type.c_str());

            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(r.path.c_str());

            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(r.preview.c_str());
            if (r.kind == ResultKind::Action && !r.hint.empty()) {
              ImGui::SameLine();
              ImGui::TextDisabled("(%s)", r.hint.c_str());
            }

            ImGui::PopID();
          }
        }

        ImGui::EndTable();
      }
    }
  }
  ImGui::EndChild();

  ImGui::SameLine();
  ImGui::BeginChild("##omni_right", ImVec2(0.0f, 0.0f), true);
  {
    const SearchResult* sel = nullptr;
    if (s.selected_idx >= 0 && s.selected_idx < (int)s.results.size()) {
      sel = &s.results[(std::size_t)s.selected_idx];
    }

    bool request_close = false;

    if (!sel) {
      ImGui::TextDisabled("No selection.");
    } else {
      ImGui::TextDisabled("Selected");
      ImGui::TextWrapped("%s", sel->path.c_str());
      ImGui::TextDisabled("Type: %s", sel->type.c_str());
      ImGui::Separator();

      switch (sel->kind) {
        case ResultKind::Action: {
          if (!sel->preview.empty()) {
            ImGui::TextDisabled("Description:");
            ImGui::TextWrapped("%s", sel->preview.c_str());
          }
          if (!sel->hint.empty()) {
            ImGui::TextDisabled("Hint:");
            ImGui::TextWrapped("%s", sel->hint.c_str());
          }
          ImGui::Separator();
          if (ImGui::Button("Run")) {
            activate_result(*sel, sim, ui, selected_ship, selected_colony, selected_body);
            request_close = true;
          }
          ImGui::SameLine();
          if (ImGui::Button("Copy name")) {
            ImGui::SetClipboardText(sel->path.c_str());
          }
        } break;

        case ResultKind::Window: {
          const WindowSpec* spec = find_window_spec(sel->window_id.c_str());
          if (!spec) {
            ImGui::TextDisabled("Window spec not found.");
          } else {
            const bool is_open = ui.*(spec->open_flag);
            ImGui::TextDisabled("Window:");
            ImGui::TextWrapped("%s", spec->label);
            ImGui::TextDisabled("Category:");
            ImGui::TextWrapped("%s", spec->category);
            ImGui::TextDisabled("ID:");
            ImGui::TextWrapped("%s", spec->id);
            ImGui::TextDisabled("State:");
            ImGui::TextUnformatted(is_open ? "Open" : "Closed");

            if (spec->supports_popup) {
              const WindowLaunchMode eff = effective_launch_mode(ui, *spec);
              ImGui::SameLine();
              ImGui::TextDisabled("  Launch: %s", window_launch_mode_label(eff).c_str());
            } else {
              ImGui::SameLine();
              ImGui::TextDisabled("  Launch: fixed");
            }

            if (spec->core) {
              ImGui::TextDisabled("Core window (not hidden by Focus Mode).");
            }

            ImGui::Separator();
            if (ImGui::Button(is_open ? "Close" : "Open")) {
              ui.*(spec->open_flag) = !is_open;
            }
            if (spec->supports_popup) {
              ImGui::SameLine();
              if (ImGui::Button("Pop out now")) {
                ui.*(spec->open_flag) = true;
                request_popout(ui, spec->id);
                request_close = true;
              }
            }
            ImGui::SameLine();
            if (ImGui::Button("Window Manager")) {
              ui.show_window_manager_window = true;
            }

            if (spec->supports_popup) {
              ImGui::SeparatorText("Launch override");
              const bool has_override =
                  (ui.window_launch_overrides.find(spec->id) != ui.window_launch_overrides.end());
              if (ImGui::Button("Docked")) {
                ui.window_launch_overrides[spec->id] = static_cast<int>(WindowLaunchMode::Docked);
              }
              ImGui::SameLine();
              if (ImGui::Button("Pop out (floating)")) {
                ui.window_launch_overrides[spec->id] = static_cast<int>(WindowLaunchMode::Popup);
              }
              ImGui::SameLine();
              if (ImGui::Button("Clear")) {
                ui.window_launch_overrides.erase(spec->id);
              }

              if (!has_override) {
                ImGui::TextDisabled("Tip: hold Shift while activating a window result to pop it out.");
              }
            }
          }
        } break;

        case ResultKind::WorkspacePreset: {
          ImGui::TextDisabled("Workspace preset:");
          ImGui::TextWrapped("%s", sel->workspace_preset.c_str());
          if (!sel->preview.empty()) {
            ImGui::TextDisabled("Description:");
            ImGui::TextWrapped("%s", sel->preview.c_str());
          }
          if (focus_mode_enabled(ui)) {
            ImGui::TextDisabled("Note: applying a workspace exits Focus Mode.");
          }
          ImGui::Separator();
          if (ImGui::Button("Apply preset")) {
            activate_result(*sel, sim, ui, selected_ship, selected_colony, selected_body);
            request_close = true;
          }
          ImGui::SameLine();
          if (ImGui::Button("Copy name")) {
            ImGui::SetClipboardText(sel->workspace_preset.c_str());
          }
        } break;

        case ResultKind::LayoutProfile: {
          const std::string dir =
              (ui.layout_profiles_dir[0] ? std::string(ui.layout_profiles_dir) : std::string("ui_layouts"));
          const std::string name = sanitize_layout_profile_name(sel->layout_profile);
          const std::string active = sanitize_layout_profile_name(ui.layout_profile);

          ImGui::TextDisabled("Layout profile:");
          ImGui::TextWrapped("%s", name.c_str());

          ImGui::TextDisabled("Status:");
          ImGui::TextUnformatted((name == active) ? "Active" : "Inactive");

          ImGui::TextDisabled("Ini path:");
          const std::string ini_path = make_layout_profile_ini_path(dir.c_str(), name);
          ImGui::TextWrapped("%s", ini_path.c_str());

          ImGui::Separator();
          if (ImGui::Button("Activate")) {
            activate_result(*sel, sim, ui, selected_ship, selected_colony, selected_body);
            request_close = true;
          }
          ImGui::SameLine();
          if (ImGui::Button("Manage Profiles")) {
            ui.show_layout_profiles_window = true;
          }
          ImGui::SameLine();
          if (ImGui::Button("Copy name")) {
            ImGui::SetClipboardText(name.c_str());
          }
        } break;

        case ResultKind::Doc: {
          ImGui::TextDisabled("Doc:");
          ImGui::TextWrapped("%s", sel->path.c_str());
          ImGui::TextDisabled("Ref:");
          ImGui::TextWrapped("%s", sel->doc_ref.c_str());
          if (!sel->preview.empty()) {
            ImGui::SeparatorText("Preview");
            ImGui::TextWrapped("%s", sel->preview.c_str());
          }
          ImGui::Separator();
          if (ImGui::Button("Open")) {
            activate_result(*sel, sim, ui, selected_ship, selected_colony, selected_body);
            request_close = true;
          }
          ImGui::SameLine();
          if (ImGui::Button("Copy ref")) {
            ImGui::SetClipboardText(sel->doc_ref.c_str());
          }
        } break;

        case ResultKind::Entity: {
          ImGui::TextDisabled("Entity id:");
          ImGui::Text("%llu", (unsigned long long)sel->entity_id);
          if (!sel->entity_kind.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("  kind: %s", sel->entity_kind.c_str());
          }

          if (!sel->preview.empty()) {
            ImGui::SeparatorText("Details");
            ImGui::TextWrapped("%s", sel->preview.c_str());
          }
          if (sel->nav_valid) {
            ImGui::Separator();
            if (ImGui::Button("Jump to")) {
              activate_result(*sel, sim, ui, selected_ship, selected_colony, selected_body);
              request_close = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Jump + open windows")) {
              const NavTarget t{sel->nav_kind, sel->nav_id};
              if (nav_target_exists(sim, t)) {
                apply_nav_target(sim, ui, selected_ship, selected_colony, selected_body, t, /*open_windows=*/true);
                request_close = true;
              }
            }
          }
          ImGui::Separator();
          if (ImGui::Button("Inspect")) {
            ui.show_entity_inspector_window = true;
            ui.entity_inspector_id = sel->entity_id;
            request_close = true;
          }
          ImGui::SameLine();
          if (ImGui::Button("Open JSON")) {
            if (!sel->entity_json_path.empty()) {
              open_in_json_explorer(ui, sel->entity_json_path);
              request_close = true;
            }
          }
          ImGui::SameLine();
          if (ImGui::Button("Copy id")) {
            const std::string txt = std::to_string((unsigned long long)sel->entity_id);
            ImGui::SetClipboardText(txt.c_str());
          }
        } break;

        case ResultKind::JsonNode: {
          if (!sel->preview.empty()) {
            ImGui::TextDisabled("Preview:");
            ImGui::TextWrapped("%s", sel->preview.c_str());
          }
          ImGui::Separator();
          if (ImGui::Button("Open in JSON Explorer")) {
            open_in_json_explorer(ui, sel->path);
            request_close = true;
          }
          ImGui::SameLine();
          if (ImGui::Button("Pin")) {
            (void)add_watch_item(ui, sel->path);
            ui.show_watchboard_window = true;
          }
          ImGui::SameLine();
          if (ImGui::Button("Copy")) {
            ImGui::SetClipboardText(sel->path.c_str());
          }

          // Resolve node for richer shortcuts.
          if (s.root) {
            std::string err;
            const nebula4x::json::Value* v = nebula4x::resolve_json_pointer(*s.root, sel->path,
                                                                           /*accept_root_slash=*/true,
                                                                           &err);
            if (v) {
              ImGui::SeparatorText("Resolved");
              const std::string t = json_type_name(*v);
              ImGui::TextDisabled("Type:");
              ImGui::SameLine();
              ImGui::TextUnformatted(t.c_str());

              if (v->is_array()) {
                const auto* a = v->as_array();
                const std::size_t n = a ? a->size() : 0;
                ImGui::SameLine();
                ImGui::TextDisabled("  len: %zu", n);

                // Tooling shortcuts for arrays.
                ImGui::Separator();
                if (ImGui::Button("Create Data Lens")) {
                  (void)add_json_table_view(ui, sel->path);
                  ui.show_data_lenses_window = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Create Dashboard")) {
                  (void)add_json_dashboard_for_path(ui, sel->path);
                  ui.show_dashboards_window = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Create Pivot")) {
                  (void)add_json_pivot_for_path(ui, sel->path);
                  ui.show_pivot_tables_window = true;
                }

                // Lightweight inline peek: first few elements.
                if (a && !a->empty()) {
                  ImGui::SeparatorText("Sample elements");
                  const std::size_t show_n = std::min<std::size_t>(a->size(), 12);
                  for (std::size_t idx = 0; idx < show_n; ++idx) {
                    ImGui::PushID((int)idx);
                    const std::string child_ptr = nebula4x::json_pointer_join_index(sel->path, idx);
                    if (ImGui::SmallButton("Open")) {
                      open_in_json_explorer(ui, child_ptr);
                      request_close = true;
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("[%zu]", idx);
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", json_type_name((*a)[idx]));
                    ImGui::SameLine();
                    const std::string pv = json_node_preview((*a)[idx], 80);
                    ImGui::TextUnformatted(pv.c_str());
                    ImGui::PopID();
                  }
                  if (a->size() > show_n) {
                    ImGui::TextDisabled("… (%zu more)", a->size() - show_n);
                  }
                }
              } else if (v->is_object()) {
                const auto* o = v->as_object();
                const std::size_t n = o ? o->size() : 0;
                ImGui::SameLine();
                ImGui::TextDisabled("  keys: %zu", n);

                if (o && !o->empty()) {
                  ImGui::SeparatorText("Sample keys");
                  std::size_t shown = 0;
                  for (const auto& kv : *o) {
                    if (shown++ >= 18) break;
                    const std::string child_ptr = nebula4x::json_pointer_join(sel->path, kv.first);
                    ImGui::PushID((int)shown);
                    if (ImGui::SmallButton("Open")) {
                      open_in_json_explorer(ui, child_ptr);
                      request_close = true;
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", kv.first.c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", json_type_name(kv.second));
                    ImGui::SameLine();
                    const std::string pv = json_node_preview(kv.second, 80);
                    ImGui::TextUnformatted(pv.c_str());
                    ImGui::PopID();
                  }
                  if (o->size() > 18) {
                    ImGui::TextDisabled("… (%zu more)", o->size() - 18);
                  }
                }
              }
            } else if (!err.empty()) {
              ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "Resolve error: %s", err.c_str());
            }
          }
        } break;
      }

      if (request_close) ui.show_omni_search_window = false;
    }

    ImGui::Separator();
    ImGui::TextDisabled("Tips");
    ImGui::BulletText("Enter or double-click activates the selected result.");
    ImGui::BulletText("Ctrl+Up/Down navigates results while typing (also Ctrl+PageUp/Down).");
    ImGui::BulletText("Prefixes: '>' commands, '@' entities, '?' docs, '#' UI.");
    ImGui::BulletText("Window results: hold Shift while activating to pop out (floating).");
    ImGui::BulletText("Right-click a result for quick actions.");
    ImGui::BulletText("For JSON arrays: create Data Lenses / Dashboards / Pivot Tables.");
  }
  ImGui::EndChild();

  // Enter: activate best match.
  if (query_enter && !s.results.empty()) {
    const int idx = (s.selected_idx >= 0 && s.selected_idx < (int)s.results.size()) ? s.selected_idx : 0;
    const auto& r = s.results[(std::size_t)idx];
    if (activate_result(r, sim, ui, selected_ship, selected_colony, selected_body)) {
      ui.show_omni_search_window = false;
    }
  }

  ImGui::End();
}

} // namespace nebula4x::ui
