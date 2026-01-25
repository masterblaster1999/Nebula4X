#include "ui/compare_window.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nebula4x/util/json.h"
#include "nebula4x/util/json_merge_patch.h"
#include "nebula4x/util/json_pointer.h"
#include "nebula4x/util/strings.h"

#include "ui/game_entity_index.h"
#include "ui/game_json_cache.h"
#include "ui/navigation.h"

namespace nebula4x::ui {
namespace {

struct FlatScalar {
  enum class Kind : int { Null = 0, Bool, Number, String, ContainerSummary };

  Kind kind{Kind::Null};
  std::string repr;
  bool has_number{false};
  double number{0.0};
};

using FlatMap = std::unordered_map<std::string, FlatScalar>;

struct FlattenOptions {
  int max_depth{6};
  int max_nodes{6000};
  bool include_container_sizes{true};
  int max_string_chars{160};
};

struct FlattenStats {
  int nodes{0};
  bool truncated{false};
  int max_depth_hit{0};
};

struct DiffRow {
  enum class Op : int { Same = 0, Added, Removed, Changed };
  Op op{Op::Same};
  std::string path;
  bool a_present{false};
  bool b_present{false};
  FlatScalar a;
  FlatScalar b;
  bool has_delta{false};
  double delta{0.0};
};

std::string format_number(double x) {
  if (!std::isfinite(x)) {
    if (std::isnan(x)) return "NaN";
    return (x < 0.0) ? "-Inf" : "Inf";
  }

  std::ostringstream oss;
  oss.setf(std::ios::fmtflags(0), std::ios::floatfield);
  oss << std::setprecision(12) << x;
  return oss.str();
}

FlatScalar make_scalar(const nebula4x::json::Value& v, int max_string_chars) {
  FlatScalar out;

  if (v.is_null()) {
    out.kind = FlatScalar::Kind::Null;
    out.repr = "null";
    return out;
  }
  if (const auto* b = v.as_bool()) {
    out.kind = FlatScalar::Kind::Bool;
    out.repr = (*b) ? "true" : "false";
    return out;
  }
  if (const auto* n = v.as_number()) {
    out.kind = FlatScalar::Kind::Number;
    out.has_number = true;
    out.number = *n;
    out.repr = format_number(*n);
    return out;
  }
  if (const auto* s = v.as_string()) {
    out.kind = FlatScalar::Kind::String;
    out.repr = *s;
    if (max_string_chars > 0 && static_cast<int>(out.repr.size()) > max_string_chars) {
      out.repr.resize(static_cast<size_t>(max_string_chars));
      out.repr += "…";
    }
    return out;
  }

  // Fallback: shouldn't happen in well-formed JSON.
  out.kind = FlatScalar::Kind::String;
  out.repr = "<unknown>";
  return out;
}

FlatScalar make_container_summary(char open, size_t n, char close) {
  FlatScalar out;
  out.kind = FlatScalar::Kind::ContainerSummary;
  out.repr.reserve(32);
  out.repr.push_back(open);
  out.repr += std::to_string(n);
  out.repr.push_back(close);
  return out;
}

void flatten_json_impl(const nebula4x::json::Value& v,
                      const std::string& path,
                      int depth,
                      const FlattenOptions& opt,
                      FlatMap& out,
                      FlattenStats& st) {
  if (st.truncated) return;
  if (st.nodes >= opt.max_nodes) {
    st.truncated = true;
    return;
  }

  const bool at_max_depth = (depth >= opt.max_depth);

  if (const auto* obj = v.as_object()) {
    if (opt.include_container_sizes && !path.empty()) {
      out[path] = make_container_summary('{', obj->size(), '}');
      ++st.nodes;
      if (st.nodes >= opt.max_nodes) {
        st.truncated = true;
        return;
      }
    }
    if (at_max_depth) {
      st.max_depth_hit = std::max(st.max_depth_hit, depth);
      return;
    }
    for (const auto& kv : *obj) {
      if (st.nodes >= opt.max_nodes) {
        st.truncated = true;
        return;
      }
      const std::string child = json_pointer_join(path, kv.first);
      flatten_json_impl(kv.second, child, depth + 1, opt, out, st);
      if (st.truncated) return;
    }
    return;
  }

  if (const auto* arr = v.as_array()) {
    if (opt.include_container_sizes && !path.empty()) {
      out[path] = make_container_summary('[', arr->size(), ']');
      ++st.nodes;
      if (st.nodes >= opt.max_nodes) {
        st.truncated = true;
        return;
      }
    }
    if (at_max_depth) {
      st.max_depth_hit = std::max(st.max_depth_hit, depth);
      return;
    }
    for (size_t i = 0; i < arr->size(); ++i) {
      if (st.nodes >= opt.max_nodes) {
        st.truncated = true;
        return;
      }
      const std::string child = json_pointer_join_index(path, i);
      flatten_json_impl((*arr)[i], child, depth + 1, opt, out, st);
      if (st.truncated) return;
    }
    return;
  }

  // Scalar.
  {
    const std::string key = path.empty() ? "/" : path;
    out[key] = make_scalar(v, opt.max_string_chars);
    ++st.nodes;
  }
}

void flatten_json(const nebula4x::json::Value& v,
                 const FlattenOptions& opt,
                 FlatMap& out,
                 FlattenStats& st) {
  out.clear();
  st = {};
  flatten_json_impl(v, /*path=*/"", /*depth=*/0, opt, out, st);
}

bool contains_case_insensitive(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) return true;
  if (haystack.empty()) return false;

  // Quick path: ASCII fold to lower (sufficient for UI strings in this project).
  std::string h(haystack);
  std::string n(needle);
  for (auto& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (auto& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return h.find(n) != std::string::npos;
}

bool contains_case_sensitive(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) return true;
  if (haystack.empty()) return false;
  return haystack.find(needle) != std::string::npos;
}

struct CompareRuntime {
  // Cache keys.
  std::uint64_t last_root_rev{0};
  Id last_a_id{kInvalidId};
  Id last_b_id{kInvalidId};
  bool last_a_snapshot{false};
  bool last_b_snapshot{false};
  std::size_t last_a_snap_hash{0};
  std::size_t last_b_snap_hash{0};
  FlattenOptions last_opt;

  // Output.
  bool valid{false};
  std::string error;
  FlatMap a_flat;
  FlatMap b_flat;
  FlattenStats a_stats;
  FlattenStats b_stats;
  std::vector<DiffRow> rows;
  int count_same{0};
  int count_added{0};
  int count_removed{0};
  int count_changed{0};

  // Entity list cache for pickers.
  std::uint64_t entity_index_rev{0};
  std::vector<const GameEntityIndexEntry*> entity_entries;
  std::string picker_filter_a;
  std::string picker_filter_b;

  // Snapshot parse cache.
  std::size_t snap_a_hash{0};
  std::size_t snap_b_hash{0};
  std::shared_ptr<nebula4x::json::Value> snap_a_value;
  std::shared_ptr<nebula4x::json::Value> snap_b_value;

  // Merge patch cache.
  std::uint64_t patch_root_rev{0};
  Id patch_a_id{kInvalidId};
  Id patch_b_id{kInvalidId};
  bool patch_a_snapshot{false};
  bool patch_b_snapshot{false};
  std::size_t patch_a_snap_hash{0};
  std::size_t patch_b_snap_hash{0};
  std::string merge_patch_text;
  std::string merge_patch_error;
};

CompareRuntime& runtime() {
  static CompareRuntime rt;
  return rt;
}

std::size_t stable_hash(const std::string& s) {
  return std::hash<std::string>{}(s);
}

// Resolve an entity from the game JSON cache by Id.
// Returns nullptr on failure and fills error.
const nebula4x::json::Value* resolve_live_entity(const nebula4x::json::Value& root,
                                                Id id,
                                                const GameEntityIndexEntry** out_entry,
                                                std::string* err) {
  if (out_entry) *out_entry = nullptr;
  if (err) err->clear();

  if (id == kInvalidId) {
    if (err) *err = "No entity id selected.";
    return nullptr;
  }

  const auto* e = find_game_entity(id);
  if (!e) {
    if (err) *err = "Unknown entity id (not indexed in live JSON).";
    return nullptr;
  }
  if (out_entry) *out_entry = e;

  std::string perr;
  const nebula4x::json::Value* v = resolve_json_pointer(root, e->path, /*create_missing=*/false, &perr);
  if (!v) {
    if (err) *err = perr.empty() ? "Failed to resolve entity JSON pointer." : perr;
    return nullptr;
  }
  return v;
}

std::shared_ptr<nebula4x::json::Value> parse_json_value_cached(std::shared_ptr<nebula4x::json::Value>& cache,
                                                               std::size_t& cache_hash,
                                                               const std::string& text,
                                                               std::string* err) {
  if (err) err->clear();
  const std::size_t h = stable_hash(text);
  if (cache && cache_hash == h) return cache;

  try {
    cache = std::make_shared<nebula4x::json::Value>(nebula4x::json::parse(text));
    cache_hash = h;
    return cache;
  } catch (const std::exception& e) {
    if (err) *err = e.what();
    cache.reset();
    cache_hash = 0;
    return nullptr;
  }
}

bool kind_to_nav_target(const std::string& kind, Id id, NavTarget* out) {
  if (!out) return false;
  if (kind == "ships") {
    out->kind = NavTargetKind::Ship;
    out->id = id;
    return true;
  }
  if (kind == "colonies") {
    out->kind = NavTargetKind::Colony;
    out->id = id;
    return true;
  }
  if (kind == "bodies") {
    out->kind = NavTargetKind::Body;
    out->id = id;
    return true;
  }
  if (kind == "systems") {
    out->kind = NavTargetKind::System;
    out->id = id;
    return true;
  }
  return false;
}

struct SlotView {
  const char* label;
  Id* id;
  bool* use_snapshot;
  std::string* snapshot_label;
  std::string* snapshot_json;
  std::string* picker_filter;
};

void draw_entity_picker_popup(const char* popup_id,
                              SlotView slot,
                              CompareRuntime& rt,
                              bool* cleared_snapshot) {
  if (cleared_snapshot) *cleared_snapshot = false;

  if (!ImGui::BeginPopup(popup_id)) return;

  std::string& filter = slot.picker_filter ? *slot.picker_filter : rt.picker_filter_a;
  ImGui::TextUnformatted("Search entities (by kind/name/id)");
  ImGui::InputTextWithHint("##entity_picker_filter", "e.g. ship, sol, 42", &filter);
  ImGui::Separator();

  const auto& idx = game_entity_index();
  if (!idx.built) {
    ImGui::TextUnformatted("Entity index not built yet.");
    ImGui::EndPopup();
    return;
  }

  // Build cache if needed.
  if (rt.entity_index_rev != idx.revision) {
    rt.entity_index_rev = idx.revision;
    rt.entity_entries.clear();
    rt.entity_entries.reserve(idx.by_id.size());
    for (const auto& kv : idx.by_id) {
      rt.entity_entries.push_back(&kv.second);
    }
    std::sort(rt.entity_entries.begin(), rt.entity_entries.end(), [](const auto* a, const auto* b) {
      if (a->kind != b->kind) return a->kind < b->kind;
      if (a->name != b->name) return a->name < b->name;
      return a->id < b->id;
    });
  }

  constexpr int kMaxRows = 400;
  int shown = 0;

  if (ImGui::BeginTable("##entity_picker_table", 3,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY)) {
    ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 110.0f);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableHeadersRow();

    for (const auto* e : rt.entity_entries) {
      if (shown >= kMaxRows) break;

      // Filter.
      if (!filter.empty()) {
        const std::string id_s = std::to_string(e->id);
        if (!contains_case_insensitive(e->kind, filter) &&
            !contains_case_insensitive(e->name, filter) &&
            !contains_case_insensitive(id_s, filter)) {
          continue;
        }
      }

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(e->kind.c_str());
      ImGui::TableSetColumnIndex(1);

      // Selectable name.
      const std::string row_label = e->name.empty() ? ("(id " + std::to_string(e->id) + ")") : e->name;
      if (ImGui::Selectable(row_label.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
        *slot.id = e->id;
        if (slot.use_snapshot) *slot.use_snapshot = false;
        if (slot.snapshot_label) slot.snapshot_label->clear();
        if (slot.snapshot_json) slot.snapshot_json->clear();
        if (cleared_snapshot) *cleared_snapshot = true;
        ImGui::CloseCurrentPopup();
      }

      ImGui::TableSetColumnIndex(2);
      ImGui::Text("%llu", static_cast<unsigned long long>(e->id));
      ++shown;
    }

    ImGui::EndTable();
  }

  if (shown >= kMaxRows) {
    ImGui::Text("Showing first %d matches…", kMaxRows);
  } else {
    ImGui::Text("Matches: %d", shown);
  }

  if (ImGui::Button("Close")) {
    ImGui::CloseCurrentPopup();
  }

  ImGui::EndPopup();
}

void draw_slot_controls(Simulation& sim,
                        UIState& ui,
                        SlotView slot,
                        const nebula4x::json::Value* live_value,
                        const GameEntityIndexEntry* live_entry,
                        bool live_ok,
                        const std::string& live_err,
                        Id& selected_ship,
                        Id& selected_colony,
                        Id& selected_body) {
  ImGui::PushID(slot.label);

  ImGui::Text("%s", slot.label);
  ImGui::SameLine();
  if (ImGui::SmallButton("Use Selected")) {
    const NavTarget cur = current_nav_target(sim, selected_ship, selected_colony, selected_body);
    if (cur.kind != NavTargetKind::None && cur.id != kInvalidId) {
      *slot.id = cur.id;
      if (slot.use_snapshot) *slot.use_snapshot = false;
      if (slot.snapshot_label) slot.snapshot_label->clear();
      if (slot.snapshot_json) slot.snapshot_json->clear();
    }
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Pick…")) {
    ImGui::OpenPopup("entity_picker");
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Clear")) {
    *slot.id = kInvalidId;
    if (slot.use_snapshot) *slot.use_snapshot = false;
    if (slot.snapshot_label) slot.snapshot_label->clear();
    if (slot.snapshot_json) slot.snapshot_json->clear();
  }

  // ID input.
  unsigned long long id_u = static_cast<unsigned long long>(*slot.id);
  if (ImGui::InputScalar("ID", ImGuiDataType_U64, &id_u)) {
    *slot.id = static_cast<Id>(id_u);
    if (slot.use_snapshot) *slot.use_snapshot = false;
  }

  // Snapshot controls.
  {
    const bool has_snapshot = slot.use_snapshot && *slot.use_snapshot && slot.snapshot_json && !slot.snapshot_json->empty();
    if (!has_snapshot) {
      if (ImGui::SmallButton("Snapshot")) {
        if (live_ok && live_value) {
          // Capture.
          if (slot.snapshot_json) {
            *slot.snapshot_json = nebula4x::json::stringify(*live_value, /*indent=*/2);
          }
          if (slot.snapshot_label) {
            const std::string ts = sim.state().date.to_string() + " " +
                                   (sim.state().hour_of_day < 10 ? "0" : "") +
                                   std::to_string(sim.state().hour_of_day) + ":00";
            *slot.snapshot_label = ts;
          }
          if (slot.use_snapshot) *slot.use_snapshot = true;
        }
      }
      ImGui::SameLine();
      ImGui::BeginDisabled(!(slot.snapshot_json && !slot.snapshot_json->empty()));
      if (ImGui::SmallButton("Use Snapshot")) {
        if (slot.use_snapshot) *slot.use_snapshot = true;
      }
      ImGui::EndDisabled();
    } else {
      if (ImGui::SmallButton("Use Live")) {
        if (slot.use_snapshot) *slot.use_snapshot = false;
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Re-snapshot")) {
        if (live_ok && live_value) {
          if (slot.snapshot_json) {
            *slot.snapshot_json = nebula4x::json::stringify(*live_value, /*indent=*/2);
          }
          if (slot.snapshot_label) {
            const std::string ts = sim.state().date.to_string() + " " +
                                   (sim.state().hour_of_day < 10 ? "0" : "") +
                                   std::to_string(sim.state().hour_of_day) + ":00";
            *slot.snapshot_label = ts;
          }
        }
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Copy Snapshot JSON")) {
        if (slot.snapshot_json) {
          ImGui::SetClipboardText(slot.snapshot_json->c_str());
        }
      }
    }
  }

  // Label line.
  {
    std::string label;
    if (slot.use_snapshot && *slot.use_snapshot && slot.snapshot_label && !slot.snapshot_label->empty()) {
      label = std::string("Snapshot @ ") + *slot.snapshot_label;
    } else if (live_ok && live_entry) {
      label = live_entry->kind + ": " + live_entry->name + "  (#" + std::to_string(live_entry->id) + ")";
    } else {
      label = live_err.empty() ? "(no selection)" : ("(" + live_err + ")");
    }
    ImGui::TextWrapped("%s", label.c_str());
  }

  // Jump buttons (only for known nav kinds).
  {
    bool can_jump = false;
    NavTarget t;
    if (live_ok && live_entry) {
      can_jump = kind_to_nav_target(live_entry->kind, live_entry->id, &t);
    }
    ImGui::BeginDisabled(!can_jump);
    if (ImGui::SmallButton("Jump")) {
      if (can_jump) {
        apply_nav_target(sim, ui, selected_ship, selected_colony, selected_body, t, /*open_windows=*/true);
      }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered() && can_jump) {
      ImGui::SetTooltip("Focus this entity in Map/Details");
    }
  }

  // Picker popup.
  bool cleared_snapshot = false;
  draw_entity_picker_popup("entity_picker", slot, runtime(), &cleared_snapshot);
  (void)cleared_snapshot;

  ImGui::PopID();
}

bool should_recompute(const UIState& ui, const CompareRuntime& rt, std::uint64_t root_rev) {
  const std::size_t a_h = stable_hash(ui.compare_a_snapshot_json);
  const std::size_t b_h = stable_hash(ui.compare_b_snapshot_json);

  if (!rt.valid) return true;
  if (rt.last_root_rev != root_rev) return true;
  if (rt.last_a_id != ui.compare_a_id || rt.last_b_id != ui.compare_b_id) return true;
  if (rt.last_a_snapshot != ui.compare_a_use_snapshot || rt.last_b_snapshot != ui.compare_b_use_snapshot) return true;
  if (rt.last_a_snap_hash != a_h || rt.last_b_snap_hash != b_h) return true;

  FlattenOptions cur;
  cur.max_depth = ui.compare_max_depth;
  cur.max_nodes = ui.compare_max_nodes;
  cur.include_container_sizes = ui.compare_include_container_sizes;
  cur.max_string_chars = ui.compare_max_value_chars;

  if (cur.max_depth != rt.last_opt.max_depth) return true;
  if (cur.max_nodes != rt.last_opt.max_nodes) return true;
  if (cur.include_container_sizes != rt.last_opt.include_container_sizes) return true;
  if (cur.max_string_chars != rt.last_opt.max_string_chars) return true;

  return false;
}

void recompute_diff(UIState& ui,
                   const nebula4x::json::Value& root,
                   std::uint64_t root_rev) {
  (void)sim;
  auto& rt = runtime();
  rt.valid = false;
  rt.error.clear();
  rt.rows.clear();
  rt.count_same = rt.count_added = rt.count_removed = rt.count_changed = 0;

  // Update cache keys.
  rt.last_root_rev = root_rev;
  rt.last_a_id = ui.compare_a_id;
  rt.last_b_id = ui.compare_b_id;
  rt.last_a_snapshot = ui.compare_a_use_snapshot;
  rt.last_b_snapshot = ui.compare_b_use_snapshot;
  rt.last_a_snap_hash = stable_hash(ui.compare_a_snapshot_json);
  rt.last_b_snap_hash = stable_hash(ui.compare_b_snapshot_json);

  rt.last_opt.max_depth = ui.compare_max_depth;
  rt.last_opt.max_nodes = ui.compare_max_nodes;
  rt.last_opt.include_container_sizes = ui.compare_include_container_sizes;
  rt.last_opt.max_string_chars = ui.compare_max_value_chars;

  const GameEntityIndexEntry* a_entry = nullptr;
  const GameEntityIndexEntry* b_entry = nullptr;
  std::string err_a;
  std::string err_b;
  const nebula4x::json::Value* a_live = resolve_live_entity(root, ui.compare_a_id, &a_entry, &err_a);
  const nebula4x::json::Value* b_live = resolve_live_entity(root, ui.compare_b_id, &b_entry, &err_b);

  // Decide which values to compare.
  const nebula4x::json::Value* a_val = nullptr;
  const nebula4x::json::Value* b_val = nullptr;

  // Snapshot side A.
  if (ui.compare_a_use_snapshot && !ui.compare_a_snapshot_json.empty()) {
    std::string perr;
    rt.snap_a_value = parse_json_value_cached(rt.snap_a_value, rt.snap_a_hash, ui.compare_a_snapshot_json, &perr);
    if (!rt.snap_a_value) {
      rt.error = std::string("Slot A snapshot parse error: ") + perr;
      return;
    }
    a_val = rt.snap_a_value.get();
  } else {
    a_val = a_live;
  }

  // Snapshot side B.
  if (ui.compare_b_use_snapshot && !ui.compare_b_snapshot_json.empty()) {
    std::string perr;
    rt.snap_b_value = parse_json_value_cached(rt.snap_b_value, rt.snap_b_hash, ui.compare_b_snapshot_json, &perr);
    if (!rt.snap_b_value) {
      rt.error = std::string("Slot B snapshot parse error: ") + perr;
      return;
    }
    b_val = rt.snap_b_value.get();
  } else {
    b_val = b_live;
  }

  if (!a_val || !b_val) {
    // Produce a helpful error if one side is missing.
    if (!a_val && !b_val) {
      rt.error = "Select two entities (or snapshots) to compare.";
    } else if (!a_val) {
      rt.error = err_a.empty() ? "Slot A is empty." : ("Slot A: " + err_a);
    } else {
      rt.error = err_b.empty() ? "Slot B is empty." : ("Slot B: " + err_b);
    }
    return;
  }

  // Flatten both.
  flatten_json(*a_val, rt.last_opt, rt.a_flat, rt.a_stats);
  flatten_json(*b_val, rt.last_opt, rt.b_flat, rt.b_stats);

  // Union of keys.
  std::vector<std::string> keys;
  keys.reserve(rt.a_flat.size() + rt.b_flat.size());
  for (const auto& kv : rt.a_flat) keys.push_back(kv.first);
  for (const auto& kv : rt.b_flat) {
    if (rt.a_flat.find(kv.first) == rt.a_flat.end()) keys.push_back(kv.first);
  }
  std::sort(keys.begin(), keys.end());

  rt.rows.reserve(keys.size());
  for (const auto& k : keys) {
    auto ia = rt.a_flat.find(k);
    auto ib = rt.b_flat.find(k);

    DiffRow row;
    row.path = k;
    row.a_present = (ia != rt.a_flat.end());
    row.b_present = (ib != rt.b_flat.end());
    if (row.a_present) row.a = ia->second;
    if (row.b_present) row.b = ib->second;

    if (!row.a_present && row.b_present) {
      row.op = DiffRow::Op::Added;
      ++rt.count_added;
    } else if (row.a_present && !row.b_present) {
      row.op = DiffRow::Op::Removed;
      ++rt.count_removed;
    } else {
      // Both present.
      const bool same = (row.a.kind == row.b.kind) && (row.a.repr == row.b.repr);
      if (same) {
        row.op = DiffRow::Op::Same;
        ++rt.count_same;
      } else {
        row.op = DiffRow::Op::Changed;
        ++rt.count_changed;
      }

      if (row.a.has_number && row.b.has_number) {
        row.has_delta = true;
        row.delta = row.b.number - row.a.number;
      }
    }

    rt.rows.push_back(std::move(row));
  }

  rt.valid = true;
}

void invalidate_merge_patch_cache(CompareRuntime& rt) {
  rt.patch_root_rev = 0;
  rt.patch_a_id = kInvalidId;
  rt.patch_b_id = kInvalidId;
  rt.patch_a_snapshot = false;
  rt.patch_b_snapshot = false;
  rt.patch_a_snap_hash = 0;
  rt.patch_b_snap_hash = 0;
  rt.merge_patch_text.clear();
  rt.merge_patch_error.clear();
}

void ensure_merge_patch_cached(UIState& ui,
                              CompareRuntime& rt,
                              const nebula4x::json::Value& root,
                              std::uint64_t root_rev) {
  const std::size_t a_h = stable_hash(ui.compare_a_snapshot_json);
  const std::size_t b_h = stable_hash(ui.compare_b_snapshot_json);

  if (!rt.merge_patch_text.empty() || !rt.merge_patch_error.empty()) {
    // Might still be valid.
    if (rt.patch_root_rev == root_rev &&
        rt.patch_a_id == ui.compare_a_id &&
        rt.patch_b_id == ui.compare_b_id &&
        rt.patch_a_snapshot == ui.compare_a_use_snapshot &&
        rt.patch_b_snapshot == ui.compare_b_use_snapshot &&
        rt.patch_a_snap_hash == a_h &&
        rt.patch_b_snap_hash == b_h) {
      return;
    }
  }

  // Recompute.
  rt.patch_root_rev = root_rev;
  rt.patch_a_id = ui.compare_a_id;
  rt.patch_b_id = ui.compare_b_id;
  rt.patch_a_snapshot = ui.compare_a_use_snapshot;
  rt.patch_b_snapshot = ui.compare_b_use_snapshot;
  rt.patch_a_snap_hash = a_h;
  rt.patch_b_snap_hash = b_h;
  rt.merge_patch_text.clear();
  rt.merge_patch_error.clear();

  const GameEntityIndexEntry* a_entry = nullptr;
  const GameEntityIndexEntry* b_entry = nullptr;
  std::string err_a;
  std::string err_b;
  const nebula4x::json::Value* a_live = resolve_live_entity(root, ui.compare_a_id, &a_entry, &err_a);
  const nebula4x::json::Value* b_live = resolve_live_entity(root, ui.compare_b_id, &b_entry, &err_b);

  const nebula4x::json::Value* a_val = nullptr;
  const nebula4x::json::Value* b_val = nullptr;

  if (ui.compare_a_use_snapshot && !ui.compare_a_snapshot_json.empty()) {
    std::string perr;
    rt.snap_a_value = parse_json_value_cached(rt.snap_a_value, rt.snap_a_hash, ui.compare_a_snapshot_json, &perr);
    if (!rt.snap_a_value) {
      rt.merge_patch_error = std::string("Slot A snapshot parse error: ") + perr;
      return;
    }
    a_val = rt.snap_a_value.get();
  } else {
    a_val = a_live;
  }

  if (ui.compare_b_use_snapshot && !ui.compare_b_snapshot_json.empty()) {
    std::string perr;
    rt.snap_b_value = parse_json_value_cached(rt.snap_b_value, rt.snap_b_hash, ui.compare_b_snapshot_json, &perr);
    if (!rt.snap_b_value) {
      rt.merge_patch_error = std::string("Slot B snapshot parse error: ") + perr;
      return;
    }
    b_val = rt.snap_b_value.get();
  } else {
    b_val = b_live;
  }

  if (!a_val || !b_val) {
    rt.merge_patch_error = "Select two entities (or snapshots) to export a merge patch.";
    return;
  }

  try {
    nebula4x::json::Value patch = nebula4x::diff_json_merge_patch(*a_val, *b_val);
    rt.merge_patch_text = nebula4x::json::stringify(patch, /*indent=*/2);
  } catch (const std::exception& e) {
    rt.merge_patch_error = e.what();
  }
}

}  // namespace

void draw_compare_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  if (!ui.show_compare_window) return;

  ImGui::SetNextWindowSize(ImVec2(1040, 720), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Compare / Diff", &ui.show_compare_window)) {
    ImGui::End();
    return;
  }

  const double now = ImGui::GetTime();
  (void)ensure_game_json_cache(sim, now, ui.compare_refresh_sec, /*force=*/false);
  const auto& cache = game_json_cache();

  if (!cache.loaded || !cache.root) {
    ImGui::TextUnformatted("Live JSON snapshot unavailable.");
    if (!cache.error.empty()) {
      ImGui::TextWrapped("Error: %s", cache.error.c_str());
    }
    ImGui::End();
    return;
  }

  // Ensure we have an entity index for picking/labels.
  ensure_game_entity_index(*cache.root, cache.revision);

  // Top: slot selection.
  const GameEntityIndexEntry* a_entry = nullptr;
  const GameEntityIndexEntry* b_entry = nullptr;
  std::string err_a;
  std::string err_b;
  const nebula4x::json::Value* a_live = resolve_live_entity(*cache.root, ui.compare_a_id, &a_entry, &err_a);
  const nebula4x::json::Value* b_live = resolve_live_entity(*cache.root, ui.compare_b_id, &b_entry, &err_b);
  const bool a_ok = (a_live != nullptr);
  const bool b_ok = (b_live != nullptr);

  if (ImGui::BeginTable("##compare_slots", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
    ImGui::TableSetupColumn("A", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("##mid", ImGuiTableColumnFlags_WidthFixed, 64.0f);
    ImGui::TableSetupColumn("B", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableNextRow();

    // A.
    ImGui::TableSetColumnIndex(0);
    SlotView slot_a{"Slot A", &ui.compare_a_id, &ui.compare_a_use_snapshot, &ui.compare_a_snapshot_label,
                    &ui.compare_a_snapshot_json};
    draw_slot_controls(sim, ui, slot_a, a_live, a_entry, a_ok, err_a, selected_ship, selected_colony, selected_body);

    // Middle controls.
    ImGui::TableSetColumnIndex(1);
    ImGui::BeginChild("##compare_mid", ImVec2(0, 0), false);
    ImGui::Dummy(ImVec2(0, 20));
    if (ImGui::Button("Swap")) {
      std::swap(ui.compare_a_id, ui.compare_b_id);
      std::swap(ui.compare_a_use_snapshot, ui.compare_b_use_snapshot);
      std::swap(ui.compare_a_snapshot_label, ui.compare_b_snapshot_label);
      std::swap(ui.compare_a_snapshot_json, ui.compare_b_snapshot_json);

      // Snapshot parse caches now invalid.
      auto& rt = runtime();
      rt.snap_a_value.reset();
      rt.snap_b_value.reset();
      rt.snap_a_hash = 0;
      rt.snap_b_hash = 0;
      rt.valid = false;
      invalidate_merge_patch_cache(rt);
    }
    ImGui::Dummy(ImVec2(0, 8));
    if (ImGui::Button("Clear##both")) {
      ui.compare_a_id = kInvalidId;
      ui.compare_b_id = kInvalidId;
      ui.compare_a_use_snapshot = false;
      ui.compare_b_use_snapshot = false;
      ui.compare_a_snapshot_label.clear();
      ui.compare_b_snapshot_label.clear();
      ui.compare_a_snapshot_json.clear();
      ui.compare_b_snapshot_json.clear();

      auto& rt = runtime();
      rt.valid = false;
      rt.snap_a_value.reset();
      rt.snap_b_value.reset();
      rt.snap_a_hash = 0;
      rt.snap_b_hash = 0;
      invalidate_merge_patch_cache(rt);
    }
    ImGui::EndChild();

    // B.
    ImGui::TableSetColumnIndex(2);
    SlotView slot_b{"Slot B", &ui.compare_b_id, &ui.compare_b_use_snapshot, &ui.compare_b_snapshot_label,
                    &ui.compare_b_snapshot_json};
    draw_slot_controls(sim, ui, slot_b, b_live, b_entry, b_ok, err_b, selected_ship, selected_colony, selected_body);

    ImGui::EndTable();
  }

  ImGui::Separator();

  // Options.
  {
    ImGui::TextUnformatted("Diff Options");
    ImGui::SameLine();
    ImGui::TextDisabled("(Flattened scalar diff; containers shown as sizes when enabled)");

    ImGui::Checkbox("Include container sizes", &ui.compare_include_container_sizes);
    ImGui::SameLine();
    ImGui::Checkbox("Show unchanged", &ui.compare_show_unchanged);
    ImGui::SameLine();
    ImGui::Checkbox("Case-sensitive filter", &ui.compare_case_sensitive);

    ImGui::SliderInt("Max depth", &ui.compare_max_depth, 1, 12);
    ImGui::SliderInt("Max nodes", &ui.compare_max_nodes, 250, 50000);
    ImGui::SliderInt("Max value chars", &ui.compare_max_value_chars, 32, 600);
    ImGui::SliderFloat("Auto-refresh (sec)", &ui.compare_refresh_sec, 0.05f, 5.0f, "%.2f");

    ImGui::InputTextWithHint("Filter", "match path or value…", &ui.compare_filter);
  }

  // Clamp budgets.
  ui.compare_max_depth = std::clamp(ui.compare_max_depth, 1, 24);
  ui.compare_max_nodes = std::clamp(ui.compare_max_nodes, 50, 200000);
  ui.compare_max_value_chars = std::clamp(ui.compare_max_value_chars, 0, 5000);
  ui.compare_refresh_sec = std::clamp(ui.compare_refresh_sec, 0.0f, 60.0f);

  // Recompute diff if needed.
  auto& rt = runtime();
  if (should_recompute(ui, rt, cache.revision)) {
    recompute_diff(ui, *cache.root, cache.revision);
    invalidate_merge_patch_cache(rt);
  }

  if (!rt.valid) {
    if (!rt.error.empty()) {
      ImGui::TextWrapped("%s", rt.error.c_str());
    }
    ImGui::End();
    return;
  }

  // Summary.
  {
    ImGui::Text("Changes: %d  Added: %d  Removed: %d  Unchanged: %d", rt.count_changed, rt.count_added,
                rt.count_removed, rt.count_same);
    if (rt.a_stats.truncated || rt.b_stats.truncated) {
      ImGui::SameLine();
      ImGui::TextDisabled("(truncated)");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Flattening hit the node budget; increase Max nodes or reduce Max depth.");
      }
    }
  }

  // Diff table.
  const ImGuiTableFlags diff_flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                     ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
  if (ImGui::BeginTable("##diff_table", 5, diff_flags, ImVec2(0, 0))) {
    ImGui::TableSetupColumn("Op", ImGuiTableColumnFlags_WidthFixed, 34.0f);
    ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch, 0.44f);
    ImGui::TableSetupColumn("A", ImGuiTableColumnFlags_WidthStretch, 0.20f);
    ImGui::TableSetupColumn("B", ImGuiTableColumnFlags_WidthStretch, 0.20f);
    ImGui::TableSetupColumn("Δ", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableHeadersRow();

    const std::string filter = ui.compare_filter;
    const bool cs = ui.compare_case_sensitive;
    auto match = [&](std::string_view s) {
      return cs ? contains_case_sensitive(s, filter) : contains_case_insensitive(s, filter);
    };

    for (const auto& row : rt.rows) {
      if (!ui.compare_show_unchanged && row.op == DiffRow::Op::Same) continue;

      if (!filter.empty()) {
        const bool ok = match(row.path) ||
                        (row.a_present && match(row.a.repr)) ||
                        (row.b_present && match(row.b.repr));
        if (!ok) continue;
      }

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);

      const char* op_s = "=";
      ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
      switch (row.op) {
        case DiffRow::Op::Same:
          op_s = "=";
          col = ImGui::GetColorU32(ImGuiCol_TextDisabled);
          break;
        case DiffRow::Op::Added:
          op_s = "+";
          col = ImGui::GetColorU32(ImVec4(0.25f, 0.85f, 0.35f, 1.0f));
          break;
        case DiffRow::Op::Removed:
          op_s = "-";
          col = ImGui::GetColorU32(ImVec4(0.95f, 0.35f, 0.35f, 1.0f));
          break;
        case DiffRow::Op::Changed:
          op_s = "≠";
          col = ImGui::GetColorU32(ImVec4(0.95f, 0.82f, 0.25f, 1.0f));
          break;
      }
      ImGui::PushStyleColor(ImGuiCol_Text, col);
      ImGui::TextUnformatted(op_s);
      ImGui::PopStyleColor();

      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(row.path.c_str());

      ImGui::TableSetColumnIndex(2);
      if (row.a_present) {
        ImGui::TextUnformatted(row.a.repr.c_str());
      } else {
        ImGui::TextDisabled("(missing)");
      }

      ImGui::TableSetColumnIndex(3);
      if (row.b_present) {
        ImGui::TextUnformatted(row.b.repr.c_str());
      } else {
        ImGui::TextDisabled("(missing)");
      }

      ImGui::TableSetColumnIndex(4);
      if (row.has_delta) {
        ImGui::TextUnformatted(format_number(row.delta).c_str());
      } else {
        ImGui::TextDisabled("-");
      }
    }

    ImGui::EndTable();
  }

  // Export tools.
  ImGui::Separator();
  ImGui::TextUnformatted("Export");
  ImGui::SameLine();
  ImGui::TextDisabled("(debug / save editing)");

  if (ImGui::Button("Copy diff summary")) {
    std::ostringstream oss;
    oss << "Compare/Diff summary\n";
    oss << "Changed: " << rt.count_changed << "\n";
    oss << "Added: " << rt.count_added << "\n";
    oss << "Removed: " << rt.count_removed << "\n";
    oss << "Unchanged: " << rt.count_same << "\n";
    ImGui::SetClipboardText(oss.str().c_str());
  }
  ImGui::SameLine();
  if (ImGui::Button("Copy merge patch (A→B)")) {
    ensure_merge_patch_cached(ui, rt, *cache.root, cache.revision);
    if (!rt.merge_patch_text.empty()) {
      ImGui::SetClipboardText(rt.merge_patch_text.c_str());
    }
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Copies an RFC 7386 JSON Merge Patch that transforms A into B (object-recursive; arrays replace).\nUseful with CLI save tooling.");
  }

  if (ImGui::CollapsingHeader("Merge Patch (RFC 7386)", ImGuiTreeNodeFlags_DefaultOpen)) {
    ensure_merge_patch_cached(ui, rt, *cache.root, cache.revision);
    if (!rt.merge_patch_error.empty()) {
      ImGui::TextWrapped("%s", rt.merge_patch_error.c_str());
    } else if (!rt.merge_patch_text.empty()) {
      ImGui::InputTextMultiline("##merge_patch", &rt.merge_patch_text, ImVec2(-1, 180),
                                ImGuiInputTextFlags_ReadOnly);
    } else {
      ImGui::TextUnformatted("(No merge patch computed.)");
    }
  }

  ImGui::End();
}

}  // namespace nebula4x::ui
