#include "ui/context_forge_window.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "nebula4x/util/json_pointer.h"

#include "ui/game_entity_index.h"
#include "ui/game_json_cache.h"
#include "ui/ui_forge_dna.h"

namespace nebula4x::ui {
namespace {

// Small deterministic RNG (LCG) for procedural picking.
struct Rng {
  std::uint64_t state{0x123456789abcdef0ull};
  explicit Rng(std::uint64_t s) : state(s ? s : 0x123456789abcdef0ull) {}

  std::uint32_t next_u32() {
    // Numerical Recipes LCG.
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<std::uint32_t>(state >> 32);
  }

  float next_01() { return (next_u32() / 4294967296.0f); }
};

static std::string to_lower_copy(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) out.push_back(static_cast<char>(std::tolower(c)));
  return out;
}

static bool contains_any(const std::string& hay, const std::initializer_list<const char*>& needles) {
  for (const char* n : needles) {
    if (!n || n[0] == '\0') continue;
    if (hay.find(n) != std::string::npos) return true;
  }
  return false;
}

static int group_rank(const std::string& g) {
  if (g == "Identity") return 0;
  if (g == "Location") return 1;
  if (g == "Economy") return 2;
  if (g == "Combat") return 3;
  if (g == "Collections") return 4;
  if (g == "Derived") return 5;
  if (g == "Other") return 9;
  return 99;
}

static std::string classify_group(const std::string& key_lc, const nebula4x::json::Value& v) {
  if (key_lc == "name" || key_lc == "id" || key_lc == "type" || key_lc == "kind" ||
      key_lc == "class" || key_lc == "designation") {
    return "Identity";
  }

  if (contains_any(key_lc, {"pos", "x", "y", "z", "system", "orbit", "location", "body", "sector", "region"})) {
    return "Location";
  }

  if (contains_any(key_lc, {"fuel", "prop", "cargo", "stock", "mineral", "resource", "wealth", "credit", "income",
                           "expense", "maint", "cost", "pop", "industry", "factory", "mine", "research"})) {
    return "Economy";
  }

  if (contains_any(key_lc, {"hp", "armor", "armour", "shield", "damage", "weapon", "missile", "sensor", "threat",
                           "signature", "combat", "attack", "defense", "defence"})) {
    return "Combat";
  }

  if (v.is_array() || v.is_object()) return "Collections";
  return "Other";
}

static float base_score_for_key(const std::string& key_lc, const nebula4x::json::Value& v,
                                bool include_id_fields) {
  // Hard filters.
  if (!include_id_fields) {
    if (key_lc == "id") return -1000.0f;
    if (key_lc.size() > 3 && key_lc.rfind("_id") == key_lc.size() - 3) return -500.0f;
  }

  float s = 10.0f;
  if (key_lc == "name") s += 1000.0f;
  if (key_lc == "id") s += 700.0f;
  if (key_lc == "type" || key_lc == "kind" || key_lc == "class") s += 250.0f;

  if (v.is_number()) s += 140.0f;
  if (v.is_bool()) s += 70.0f;
  if (v.is_string()) s += 40.0f;
  if (v.is_array() || v.is_object()) s += 110.0f;

  // Keyword boosts.
  if (contains_any(key_lc, {"fuel", "prop"})) s += 180.0f;
  if (contains_any(key_lc, {"pop", "population"})) s += 220.0f;
  if (contains_any(key_lc, {"mass", "ton", "tonnage"})) s += 150.0f;
  if (contains_any(key_lc, {"speed", "thrust", "dv", "delta"})) s += 140.0f;
  if (contains_any(key_lc, {"hp", "armor", "armour", "shield"})) s += 160.0f;
  if (contains_any(key_lc, {"income", "cost", "maint"})) s += 160.0f;

  // Mild penalties.
  if (key_lc.size() > 40) s -= 40.0f;

  return s;
}

struct Candidate {
  int type{0};
  std::string group;
  std::string label;
  std::string path;
  bool is_query{false};
  int query_op{0};
  int span{1};
  int preview_rows{8};
  float score{0.0f};
  std::uint32_t jitter{0};
};

static UiForgePanelConfig* find_panel(UIState& ui, std::uint64_t id) {
  if (id == 0) return nullptr;
  for (auto& p : ui.ui_forge_panels) {
    if (p.id == id) return &p;
  }
  return nullptr;
}

static std::string format_entity_label(const GameEntityIndexEntry& ent, std::uint64_t id) {
  std::string label = ent.kind;
  if (!ent.name.empty()) {
    label += ": " + ent.name;
  } else {
    label += " #" + std::to_string((unsigned long long)id);
  }
  return label;
}

static void push_separator(UIState& ui, UiForgePanelConfig& panel, const std::string& label) {
  UiForgeWidgetConfig w;
  w.id = ui.next_ui_forge_widget_id++;
  w.type = 2;
  w.label = label;
  w.span = 6;
  panel.widgets.push_back(std::move(w));
}

static void push_candidate_widget(UIState& ui, UiForgePanelConfig& panel, const Candidate& c) {
  UiForgeWidgetConfig w;
  w.id = ui.next_ui_forge_widget_id++;
  w.type = c.type;
  w.label = c.label;
  w.path = c.path;
  w.is_query = c.is_query;
  w.query_op = c.query_op;
  w.span = std::clamp(c.span, 1, 6);
  w.preview_rows = std::clamp(c.preview_rows, 1, 30);

  // Sensible defaults for KPI history.
  if (w.type == 0) {
    w.track_history = true;
    w.show_sparkline = true;
    w.history_len = 160;
  }

  panel.widgets.push_back(std::move(w));
}

static void add_object_scalar_candidates(std::vector<Candidate>& out, const nebula4x::json::Value& obj_v,
                                         const std::string& base_ptr, const std::string& base_group,
                                         bool include_id_fields, int depth, Rng& rng,
                                         const std::string& label_prefix = "") {
  const auto* obj = obj_v.as_object();
  if (!obj) return;
  if (depth <= 0) return;

  // Avoid descending into extremely large objects (tends to produce noisy UIs).
  if (obj->size() > 128) return;

  for (const auto& kv : *obj) {
    const std::string& k = kv.first;
    const nebula4x::json::Value& v = kv.second;
    const std::string k_lc = to_lower_copy(k);

    if (v.is_null() || v.is_bool() || v.is_number() || v.is_string()) {
      Candidate c;
      c.type = 0;
      c.group = base_group;
      c.label = label_prefix.empty() ? k : (label_prefix + "." + k);
      c.path = nebula4x::json_pointer_join(base_ptr, k);
      c.score = base_score_for_key(k_lc, v, include_id_fields) * 0.6f;
      c.jitter = rng.next_u32();
      out.push_back(std::move(c));
      continue;
    }

    // Optional descent into nested objects.
    if (v.is_object() && depth > 1) {
      const std::string next_prefix = label_prefix.empty() ? k : (label_prefix + "." + k);
      add_object_scalar_candidates(out, v, nebula4x::json_pointer_join(base_ptr, k), base_group,
                                   include_id_fields, depth - 1, rng, next_prefix);
      continue;
    }
  }
}

static void add_array_derived_candidates(std::vector<Candidate>& out, const nebula4x::json::Value& arr_v,
                                         const std::string& arr_ptr, const std::string& arr_key,
                                         bool include_id_fields, int max_numeric_keys, Rng& rng) {
  const auto* arr = arr_v.as_array();
  if (!arr || arr->empty()) return;

  // Probe a few elements to infer numeric keys.
  std::unordered_set<std::string> numeric_keys;
  const int probe = std::min<int>(6, (int)arr->size());
  for (int i = 0; i < probe; ++i) {
    const auto* obj = (*arr)[i].as_object();
    if (!obj) continue;
    for (const auto& kv : *obj) {
      const std::string& k = kv.first;
      const auto& v = kv.second;
      if (!v.is_number()) continue;
      const std::string k_lc = to_lower_copy(k);
      if (!include_id_fields && (k_lc == "id" || (k_lc.size() > 3 && k_lc.rfind("_id") == k_lc.size() - 3))) {
        continue;
      }
      numeric_keys.insert(k);
    }
  }

  if (numeric_keys.empty()) return;

  // Rank numeric keys by heuristic importance.
  struct KeyScore {
    std::string key;
    float score{0.0f};
    std::uint32_t jitter{0};
  };

  std::vector<KeyScore> keys;
  keys.reserve(numeric_keys.size());
  for (const auto& k : numeric_keys) {
    const std::string k_lc = to_lower_copy(k);
    float s = 20.0f;
    if (contains_any(k_lc, {"amount", "qty", "count", "num", "size"})) s += 120.0f;
    if (contains_any(k_lc, {"mass", "ton", "tonnage"})) s += 140.0f;
    if (contains_any(k_lc, {"cost", "maint", "price"})) s += 140.0f;
    if (contains_any(k_lc, {"hp", "armor", "armour", "shield", "damage"})) s += 150.0f;
    if (contains_any(k_lc, {"fuel", "prop"})) s += 140.0f;
    if (k_lc == "value") s += 70.0f;
    keys.push_back(KeyScore{k, s, rng.next_u32()});
  }

  std::sort(keys.begin(), keys.end(), [](const KeyScore& a, const KeyScore& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.jitter < b.jitter;
  });

  const int take = std::clamp(max_numeric_keys, 0, 12);
  for (int i = 0; i < (int)keys.size() && i < take; ++i) {
    const std::string& nk = keys[i].key;
    const std::string pattern = nebula4x::json_pointer_join(arr_ptr, "*") + "/" + nebula4x::json_pointer_escape_token(nk);

    // Sum.
    Candidate sum;
    sum.type = 0;
    sum.group = "Derived";
    sum.label = arr_key + "." + nk + " Σ";
    sum.path = pattern;
    sum.is_query = true;
    sum.query_op = 1;
    sum.span = 1;
    sum.score = 180.0f + keys[i].score;
    sum.jitter = rng.next_u32();
    out.push_back(std::move(sum));

    // Average.
    Candidate avg = sum;
    avg.label = arr_key + "." + nk + " avg";
    avg.query_op = 2;
    avg.score = 160.0f + keys[i].score * 0.8f;
    avg.jitter = rng.next_u32();
    out.push_back(std::move(avg));
  }
}

static bool regenerate_context_forge_panel(Simulation& sim, UIState& ui, std::uint64_t entity_id,
                                           std::string* out_error) {
  if (entity_id == kInvalidId) {
    if (out_error) *out_error = "No entity selected.";
    return false;
  }

  // Ensure we have fresh JSON + index.
  ensure_game_json_cache(sim, ImGui::GetTime(), /*min_refresh_sec=*/0.0, /*force=*/true);
  auto& cache = game_json_cache();
  if (!cache.loaded || !cache.root) {
    if (out_error) *out_error = cache.error.empty() ? "Game JSON cache is not available." : cache.error;
    return false;
  }
  ensure_game_entity_index(*cache.root, cache.revision);
  const GameEntityIndexEntry* ent = find_game_entity(entity_id);
  if (!ent) {
    if (out_error) *out_error = "Entity not found in JSON index (it may not be serialized yet).";
    return false;
  }

  // Resolve the entity node for schema probing.
  std::string rerr;
  const auto* node = nebula4x::resolve_json_pointer(*cache.root, ent->path, /*accept_root_slash=*/true, &rerr);
  if (!node) {
    if (out_error) *out_error = rerr.empty() ? "Failed to resolve entity JSON pointer." : rerr;
    return false;
  }

  // Find or create the Context Forge panel.
  UiForgePanelConfig* panel = find_panel(ui, ui.context_forge_panel_id);
  if (!panel) {
    UiForgePanelConfig created;
    created.id = ui.next_ui_forge_panel_id++;
    created.name = "Context Forge";
    created.open = ui.context_forge_open_panel_on_generate;
    created.root_path = ent->path;
    created.desired_columns = 0;
    created.card_width_em = 20.0f;
    ui.ui_forge_panels.push_back(std::move(created));
    ui.context_forge_panel_id = ui.ui_forge_panels.back().id;
    panel = &ui.ui_forge_panels.back();
  }

  panel->name = "Context Forge: " + format_entity_label(*ent, entity_id);
  panel->root_path = ent->path;
  if (ui.context_forge_open_panel_on_generate) panel->open = true;

  // Build candidate widgets.
  const std::uint64_t seed = (static_cast<std::uint64_t>(ui.context_forge_seed) * 0x9E3779B97F4A7C15ull) ^
                             (entity_id * 0xD1B54A32D192ED03ull);
  Rng rng(seed);

  std::vector<Candidate> candidates;
  candidates.reserve(128);

  const bool include_id_fields = ui.context_forge_include_id_fields;

  // Always show a context note.
  panel->widgets.clear();
  {
    UiForgeWidgetConfig note;
    note.id = ui.next_ui_forge_widget_id++;
    note.type = 1;
    note.span = 2;
    note.label = "Context";
    note.text = format_entity_label(*ent, entity_id) + "\nID: " + std::to_string((unsigned long long)entity_id) +
                "\nPath: " + ent->path + "\nSeed: " + std::to_string(ui.context_forge_seed) +
                "\nTip: Right-click any card for actions; use UI Forge to edit.";
    panel->widgets.push_back(std::move(note));
  }

  // Root scalar/object/array handling.
  const std::string root_ptr = ent->path;
  if (const auto* obj = node->as_object()) {
    for (const auto& kv : *obj) {
      const std::string& k = kv.first;
      const nebula4x::json::Value& v = kv.second;
      const std::string k_lc = to_lower_copy(k);
      const std::string group = classify_group(k_lc, v);

      // Direct KPI for scalars and container sizes.
      if (v.is_null() || v.is_bool() || v.is_number() || v.is_string() || v.is_array() || v.is_object()) {
        Candidate c;
        c.type = 0;
        c.group = group;
        c.label = k;
        c.path = nebula4x::json_pointer_join(root_ptr, k);
        c.score = base_score_for_key(k_lc, v, include_id_fields);
        c.jitter = rng.next_u32();
        c.span = 1;

        // Prefer wider cards for long strings.
        if (v.is_string() && v.string_value().size() > 26) c.span = 2;
        candidates.push_back(std::move(c));
      }

      // List previews for containers.
      if (ui.context_forge_include_lists && (v.is_array() || v.is_object())) {
        Candidate l;
        l.type = 3;
        l.group = "Collections";
        l.label = k;
        l.path = nebula4x::json_pointer_join(root_ptr, k);
        l.preview_rows = 8;
        l.span = 2;
        l.score = 70.0f + base_score_for_key(k_lc, v, /*include_id_fields=*/true) * 0.25f;
        l.jitter = rng.next_u32();
        candidates.push_back(std::move(l));
      }

      // Shallow object scalar expansion.
      if (ui.context_forge_depth > 0 && v.is_object()) {
        add_object_scalar_candidates(candidates, v, nebula4x::json_pointer_join(root_ptr, k), group,
                                     include_id_fields, ui.context_forge_depth, rng, k);
      }

      // Derived metrics from arrays.
      if (ui.context_forge_include_queries && v.is_array()) {
        add_array_derived_candidates(candidates, v, nebula4x::json_pointer_join(root_ptr, k), k, include_id_fields,
                                     ui.context_forge_max_array_numeric_keys, rng);
      }
    }
  }

  // Split candidates into KPI vs lists.
  std::vector<Candidate> kpis;
  std::vector<Candidate> lists;
  kpis.reserve(candidates.size());
  lists.reserve(candidates.size());
  for (auto& c : candidates) {
    if (c.type == 3) {
      lists.push_back(std::move(c));
    } else {
      kpis.push_back(std::move(c));
    }
  }

  auto sort_cands = [](std::vector<Candidate>& v) {
    std::sort(v.begin(), v.end(), [](const Candidate& a, const Candidate& b) {
      const int ra = group_rank(a.group);
      const int rb = group_rank(b.group);
      if (ra != rb) return ra < rb;
      if (a.score != b.score) return a.score > b.score;
      return a.jitter < b.jitter;
    });
  };
  sort_cands(kpis);
  sort_cands(lists);

  // Pick with uniqueness (avoid duplicate paths/patterns).
  std::unordered_set<std::string> used_paths;
  used_paths.reserve(128);

  const int max_kpis = std::clamp(ui.context_forge_max_kpis, 0, 80);
  const int max_lists = std::clamp(ui.context_forge_max_lists, 0, 24);

  std::vector<Candidate> picked;
  picked.reserve((std::size_t)max_kpis + (std::size_t)max_lists);

  for (const auto& c : kpis) {
    if ((int)picked.size() >= max_kpis) break;
    if (c.score < 0.0f) continue;
    if (used_paths.insert(c.path).second) picked.push_back(c);
  }

  int picked_lists = 0;
  for (const auto& c : lists) {
    if (picked_lists >= max_lists) break;
    if (used_paths.insert(c.path).second) {
      picked.push_back(c);
      picked_lists++;
    }
  }

  // Final ordering by group, then by type (KPI first), then score.
  std::sort(picked.begin(), picked.end(), [](const Candidate& a, const Candidate& b) {
    const int ra = group_rank(a.group);
    const int rb = group_rank(b.group);
    if (ra != rb) return ra < rb;
    if (a.type != b.type) return a.type < b.type;
    if (a.score != b.score) return a.score > b.score;
    return a.jitter < b.jitter;
  });

  // Emit widgets with labeled separators per group.
  std::string last_group;
  for (const auto& c : picked) {
    if (c.group != last_group) {
      push_separator(ui, *panel, c.group);
      last_group = c.group;
    }
    push_candidate_widget(ui, *panel, c);
  }

  ui.context_forge_last_entity_id = entity_id;
  ui.context_forge_last_error.clear();
  ui.context_forge_last_success_time = ImGui::GetTime();
  return true;
}

static std::uint64_t pick_target_entity_id(const UIState& ui, Id selected_ship, Id selected_colony, Id selected_body) {
  if (!ui.context_forge_follow_selection && ui.context_forge_pinned_entity_id != kInvalidId) {
    return ui.context_forge_pinned_entity_id;
  }

  if (selected_ship != kInvalidId) return selected_ship;
  if (selected_colony != kInvalidId) return selected_colony;
  if (selected_body != kInvalidId) return selected_body;
  return kInvalidId;
}

static void duplicate_context_panel(UIState& ui) {
  UiForgePanelConfig* src = find_panel(ui, ui.context_forge_panel_id);
  if (!src) return;

  UiForgePanelConfig dup = *src;
  dup.id = ui.next_ui_forge_panel_id++;
  dup.name = src->name + " (frozen)";
  dup.open = true;

  // Re-id widgets.
  for (auto& w : dup.widgets) {
    w.id = ui.next_ui_forge_widget_id++;
  }

  ui.ui_forge_panels.push_back(std::move(dup));
}

}  // namespace

void update_context_forge(Simulation& sim, UIState& ui, Id selected_ship, Id selected_colony, Id selected_body) {
  if (!ui.context_forge_enabled) return;

  const std::uint64_t target = pick_target_entity_id(ui, selected_ship, selected_colony, selected_body);

  bool want_regen = false;
  if (ui.context_forge_request_regenerate) {
    ui.context_forge_request_regenerate = false;
    want_regen = true;
  }

  if (ui.context_forge_auto_update && target != kInvalidId && target != ui.context_forge_last_entity_id) {
    want_regen = true;
  }

  if (!want_regen) return;
  if (target == kInvalidId) return;

  std::string err;
  if (!regenerate_context_forge_panel(sim, ui, target, &err)) {
    ui.context_forge_last_error = err;
  }
}

void draw_context_forge_window(Simulation& sim, UIState& ui, Id selected_ship, Id selected_colony, Id selected_body) {
  if (!ui.show_context_forge_window) return;

  // (currently unused in the UI draw pass, but kept in the signature to match other windows)
  (void)sim;

  ImGui::SetNextWindowSize(ImVec2(520, 520), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Context Forge (Procedural Panels)", &ui.show_context_forge_window)) {
    ImGui::End();
    return;
  }

  ImGui::Checkbox("Enable", &ui.context_forge_enabled);
  ImGui::SameLine();
  ImGui::TextDisabled("(Generates a live UI Forge panel for your current selection)");

  const std::uint64_t target = pick_target_entity_id(ui, selected_ship, selected_colony, selected_body);

  {
    ImGui::Separator();
    ImGui::TextUnformatted("Target");

    ImGui::Checkbox("Follow selection", &ui.context_forge_follow_selection);
    ImGui::SameLine();
    ImGui::Checkbox("Auto-update", &ui.context_forge_auto_update);

    if (!ui.context_forge_follow_selection) {
      ImGui::TextDisabled("Pinned entity ID:");
      ImGui::SameLine();
      unsigned long long pid = (unsigned long long)ui.context_forge_pinned_entity_id;
      if (ImGui::InputScalar("##pinned", ImGuiDataType_U64, &pid)) {
        ui.context_forge_pinned_entity_id = (std::uint64_t)pid;
        ui.context_forge_request_regenerate = true;
      }
      if (ImGui::Button("Pin current")) {
        if (target != kInvalidId) {
          ui.context_forge_pinned_entity_id = target;
          ui.context_forge_request_regenerate = true;
        }
      }
      ImGui::SameLine();
      if (ImGui::Button("Unpin")) {
        ui.context_forge_pinned_entity_id = kInvalidId;
        ui.context_forge_follow_selection = true;
      }
    } else {
      ImGui::TextDisabled("Following selection: ship > colony > body");
    }

    ImGui::Text("Selected: ship=%llu  colony=%llu  body=%llu",
                (unsigned long long)selected_ship,
                (unsigned long long)selected_colony,
                (unsigned long long)selected_body);
    ImGui::Text("Target: %s", (target == kInvalidId) ? "(none)" : ("#" + std::to_string((unsigned long long)target)).c_str());
  }

  {
    ImGui::Separator();
    ImGui::TextUnformatted("Generator");
    ImGui::InputInt("Seed", &ui.context_forge_seed);
    ImGui::SliderInt("Max KPIs", &ui.context_forge_max_kpis, 0, 40);
    ImGui::SliderInt("Max Lists", &ui.context_forge_max_lists, 0, 12);
    ImGui::SliderInt("Depth", &ui.context_forge_depth, 0, 2);
    ImGui::SliderInt("Array numeric keys", &ui.context_forge_max_array_numeric_keys, 0, 6);
    ImGui::Checkbox("Include list widgets", &ui.context_forge_include_lists);
    ImGui::Checkbox("Include derived (query) KPIs", &ui.context_forge_include_queries);
    ImGui::Checkbox("Include *_id fields", &ui.context_forge_include_id_fields);
    ImGui::Checkbox("Open panel on generate", &ui.context_forge_open_panel_on_generate);
  }

  {
    ImGui::Separator();
    ImGui::TextUnformatted("Actions");

    const bool can_gen = ui.context_forge_enabled && target != kInvalidId;
    if (!can_gen) ImGui::BeginDisabled();
    if (ImGui::Button("Generate / Refresh")) {
      ui.context_forge_request_regenerate = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Duplicate (freeze)")) {
      duplicate_context_panel(ui);
    }
    if (!can_gen) ImGui::EndDisabled();

    UiForgePanelConfig* panel = find_panel(ui, ui.context_forge_panel_id);
    if (panel) {
      ImGui::SameLine();
      if (ImGui::Button(panel->open ? "Hide panel" : "Show panel")) {
        panel->open = !panel->open;
      }
      ImGui::SameLine();
      if (ImGui::Button("Copy Panel DNA")) {
        const std::string dna = encode_ui_forge_panel_dna(*panel);
        ImGui::SetClipboardText(dna.c_str());
      }
      ImGui::SameLine();
      if (ImGui::Button("Paste Panel DNA")) {
        const char* clip = ImGui::GetClipboardText();
        if (clip && clip[0]) {
          UiForgePanelDNA dna;
          std::string derr;
          if (decode_ui_forge_panel_dna(std::string(clip), dna, &derr)) {
            // Replace existing panel widgets.
            panel->widgets.clear();
            panel->name = dna.name.empty() ? panel->name : dna.name;
            panel->root_path = dna.root_path.empty() ? panel->root_path : dna.root_path;
            for (const auto& w : dna.widgets) {
              UiForgeWidgetConfig cfg;
              cfg.id = ui.next_ui_forge_widget_id++;
              cfg.type = w.type;
              cfg.label = w.label;
              cfg.path = w.path;
              cfg.text = w.text;
              cfg.is_query = w.is_query;
              cfg.query_op = w.query_op;
              cfg.track_history = w.track_history;
              cfg.show_sparkline = w.show_sparkline;
              cfg.history_len = w.history_len;
              cfg.span = w.span;
              cfg.preview_rows = w.preview_rows;
              panel->widgets.push_back(std::move(cfg));
            }
            ui.context_forge_last_error.clear();
            ui.context_forge_last_success_time = ImGui::GetTime();
          } else {
            ui.context_forge_last_error = derr;
          }
        }
      }
    } else {
      ImGui::TextDisabled("No context panel created yet.");
    }
  }

  if (!ui.context_forge_last_error.empty()) {
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", ui.context_forge_last_error.c_str());
  } else if (ui.context_forge_last_success_time > 0.0) {
    const double dt = ImGui::GetTime() - ui.context_forge_last_success_time;
    ImGui::Separator();
    ImGui::TextDisabled("Last generated %.2fs ago", dt);
  }

  ImGui::End();
}

}  // namespace nebula4x::ui
