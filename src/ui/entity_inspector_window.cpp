#include "ui/entity_inspector_window.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
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

#include "ui/data_lenses_window.h"
#include "ui/game_entity_index.h"
#include "ui/game_json_cache.h"
#include "ui/watchboard_window.h"

namespace nebula4x::ui {
namespace {

struct NodeFrame {
  const nebula4x::json::Value* v{nullptr};
  std::string path;
};

struct RefHit {
  std::string path;
  std::string key;
};

struct RefScanState {
  std::uint64_t target_id{0};
  std::uint64_t doc_revision{0};

  bool running{false};
  bool done{false};
  bool capped{false};

  std::uint64_t scanned_nodes{0};

  std::vector<NodeFrame> stack;
  std::vector<RefHit> hits;
};

struct EntityInspectorState {
  // Name search input (transient).
  char name_query[96] = "";

  // Reference filter (transient).
  char ref_filter[128] = "";

  // Scan runtime.
  RefScanState scan{};

  // Transient UI focus helper.
  bool focus_id_input_next{false};
};

EntityInspectorState& state() {
  static EntityInspectorState st;
  return st;
}

char to_lower_ascii(char c) {
  if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
  return c;
}

bool icontains(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) return true;
  if (haystack.size() < needle.size()) return false;
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

bool path_contains_filter(const char* filter, const std::string& path) {
  if (!filter || filter[0] == '\0') return true;
  return icontains(path, filter);
}

void start_ref_scan(EntityInspectorState& st, std::uint64_t target_id, std::uint64_t doc_revision,
                    const std::shared_ptr<const nebula4x::json::Value>& root) {
  st.scan = RefScanState{};
  st.scan.target_id = target_id;
  st.scan.doc_revision = doc_revision;
  st.scan.running = true;
  st.scan.done = false;
  st.scan.capped = false;
  st.scan.scanned_nodes = 0;
  st.scan.stack.clear();
  st.scan.hits.clear();

  if (root) {
    NodeFrame nf;
    nf.v = root.get();
    nf.path = "/";
    st.scan.stack.push_back(std::move(nf));
  } else {
    st.scan.running = false;
    st.scan.done = true;
  }
}

std::string last_path_token(const std::string& path) {
  const auto pos = path.find_last_of('/');
  if (pos == std::string::npos) return path;
  if (pos + 1 >= path.size()) return {};
  return nebula4x::json_pointer_unescape_token(path.substr(pos + 1));
}

bool looks_like_id_key(std::string_view key) {
  if (key == "id") return true;
  if (key.size() >= 3 && key.substr(key.size() - 3) == "_id") return true;
  if (key.size() >= 4 && key.substr(key.size() - 4) == "_ids") return true;
  return false;
}

void scan_step(EntityInspectorState& st, const UIState& ui, const GameEntityIndexEntry* self_entry,
               const std::shared_ptr<const nebula4x::json::Value>& root) {
  if (!st.scan.running || st.scan.done) return;
  if (!root) {
    st.scan.running = false;
    st.scan.done = true;
    return;
  }

  const int budget = std::max(200, ui.entity_inspector_nodes_per_frame);
  const int max_hits = std::max(10, ui.entity_inspector_max_refs);

  for (int i = 0; i < budget && !st.scan.stack.empty(); ++i) {
    NodeFrame fr = std::move(st.scan.stack.back());
    st.scan.stack.pop_back();
    st.scan.scanned_nodes++;

    if (!fr.v) continue;

    // Match: scalar number equals target id.
    std::uint64_t vid = 0;
    if (json_to_u64_id(*fr.v, vid) && vid == st.scan.target_id) {
      // Filter out the entity's own "/.../id" field when we can.
      bool is_self_id = false;
      if (self_entry) {
        if (!self_entry->path.empty() && fr.path == (self_entry->path + "/id")) {
          is_self_id = true;
        }
      }
      if (!is_self_id) {
        RefHit hit;
        hit.path = fr.path;
        hit.key = last_path_token(fr.path);
        st.scan.hits.push_back(std::move(hit));
        if ((int)st.scan.hits.size() >= max_hits) {
          st.scan.capped = true;
          st.scan.running = false;
          st.scan.done = true;
          return;
        }
      }
    }

    // Traverse children.
    if (fr.v->is_object()) {
      const auto* o = fr.v->as_object();
      if (!o) continue;
      for (const auto& kv : *o) {
        NodeFrame child;
        child.v = &kv.second;
        child.path = nebula4x::json_pointer_join(fr.path, kv.first);
        st.scan.stack.push_back(std::move(child));
      }
    } else if (fr.v->is_array()) {
      const auto* a = fr.v->as_array();
      if (!a) continue;
      for (std::size_t idx = 0; idx < a->size(); ++idx) {
        NodeFrame child;
        child.v = &(*a)[idx];
        child.path = nebula4x::json_pointer_join_index(fr.path, idx);
        st.scan.stack.push_back(std::move(child));
      }
    }
  }

  if (st.scan.stack.empty()) {
    st.scan.running = false;
    st.scan.done = true;
  }
}

void open_in_json_explorer(UIState& ui, const std::string& path) {
  ui.show_json_explorer_window = true;
  ui.request_json_explorer_goto_path = path;
}

void open_in_entity_inspector(UIState& ui, std::uint64_t id) {
  ui.show_entity_inspector_window = true;
  ui.entity_inspector_id = id;
}

void open_lens_for_kind(UIState& ui, const std::string& kind) {
  const std::string array_path = nebula4x::json_pointer_join("/", kind);
  ui.show_data_lenses_window = true;
  (void)add_json_table_view(ui, array_path, kind);
}

std::string pretty_entity_label(const GameEntityIndexEntry& e) {
  std::string out;
  out.reserve(e.kind.size() + e.name.size() + 32);
  out += e.kind;
  out += " #";
  out += std::to_string(e.id);
  if (!e.name.empty()) {
    out += "  ";
    out += e.name;
  }
  return out;
}

} // namespace

void draw_entity_inspector_window(Simulation& sim, UIState& ui) {
  if (!ui.show_entity_inspector_window) return;

  EntityInspectorState& st = state();

  ImGui::SetNextWindowSize(ImVec2(760, 560), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Entity Inspector (ID Resolver)", &ui.show_entity_inspector_window)) {
    ImGui::End();
    return;
  }

  // Ensure we have a reasonably fresh game-state JSON snapshot.
  {
    const double now = ImGui::GetTime();
    ensure_game_json_cache(sim, now, ui.entity_inspector_refresh_sec, /*force=*/false);
  }
  const auto& cache = game_json_cache();
  const auto root = cache.root;
  const std::uint64_t doc_revision = cache.revision;

  if (!root) {
    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "No game JSON snapshot available.");
    if (!cache.error.empty()) {
      ImGui::TextDisabled("%s", cache.error.c_str());
    }
    ImGui::End();
    return;
  }

  // Build/update the entity index for this snapshot.
  (void)ensure_game_entity_index(*root, doc_revision);

  // --- ID input / name search ---
  ImGui::TextDisabled("Resolve by ID");
  ImGui::SameLine();
  ImGui::TextDisabled("(Ctrl+G toggles this window)");

  ImGui::Separator();

  if (st.focus_id_input_next) {
    ImGui::SetKeyboardFocusHere();
    st.focus_id_input_next = false;
  }

  ImGui::PushItemWidth(220.0f);
  ImGui::InputScalar("Entity ID", ImGuiDataType_U64, &ui.entity_inspector_id);
  ImGui::PopItemWidth();

  ImGui::SameLine();
  if (ImGui::SmallButton("Scan refs")) {
    st.focus_id_input_next = false;
    // Trigger a fresh scan below.
    start_ref_scan(st, ui.entity_inspector_id, doc_revision, root);
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Refresh JSON")) {
    const double now = ImGui::GetTime();
    ensure_game_json_cache(sim, now, /*min_refresh_sec=*/0.0, /*force=*/true);
    invalidate_game_entity_index();
  }
  ImGui::SameLine();
  ImGui::Checkbox("Auto-scan", &ui.entity_inspector_auto_scan);

  ImGui::Separator();

  ImGui::TextDisabled("Find by name");
  ImGui::SameLine();
  ImGui::PushItemWidth(300.0f);
  ImGui::InputTextWithHint("##name_query", "type part of an entity name (ships, systems, ...)", st.name_query,
                           sizeof(st.name_query));
  ImGui::PopItemWidth();

  // --- Resolve the selected id ---
  const GameEntityIndexEntry* entry = nullptr;
  if (ui.entity_inspector_id != 0) {
    entry = find_game_entity(ui.entity_inspector_id);
  }

  // Right side split: entity + references.
  ImGui::BeginChild("##entity_inspector_split", ImVec2(0, 0), false);

  // Top: entity panel.
  ImGui::BeginChild("##entity_panel", ImVec2(0, 220), true);
  {
    if (entry) {
      ImGui::TextUnformatted(pretty_entity_label(*entry).c_str());
      ImGui::TextDisabled("Path: %s", entry->path.c_str());

      ImGui::Separator();

      if (ImGui::SmallButton("Open in JSON Explorer")) {
        open_in_json_explorer(ui, entry->path);
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Open in Reference Graph")) {
        ui.show_reference_graph_window = true;
        ui.reference_graph_focus_id = entry->id;
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Open collection in Data Lenses")) {
        open_lens_for_kind(ui, entry->kind);
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Pin entity to Watchboard")) {
        ui.show_watchboard_window = true;
        (void)add_watch_item(ui, entry->path, pretty_entity_label(*entry), /*track_history=*/true, /*show_sparkline=*/false);
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Copy ID")) {
        ImGui::SetClipboardText(std::to_string(entry->id).c_str());
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Copy path")) {
        ImGui::SetClipboardText(entry->path.c_str());
      }

      std::string rerr;
      const auto* v = nebula4x::resolve_json_pointer(*root, entry->path, /*accept_root_slash=*/true, &rerr);
      if (!v) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Resolve error: %s", rerr.c_str());
      } else {
        ImGui::Separator();
        ImGui::TextDisabled("Preview (JSON)");
        const std::string pretty = nebula4x::json::stringify(*v, 2);
        const std::size_t max_chars = 9000;
        if (pretty.size() <= max_chars) {
          ImGui::BeginChild("##entity_json_preview", ImVec2(0, 0), false);
          ImGui::TextUnformatted(pretty.c_str());
          ImGui::EndChild();
        } else {
          ImGui::BeginChild("##entity_json_preview", ImVec2(0, 0), false);
          ImGui::TextUnformatted(pretty.substr(0, max_chars).c_str());
          ImGui::TextDisabled("... (truncated, %zu chars total)", pretty.size());
          ImGui::EndChild();
        }
      }
    } else {
      ImGui::TextDisabled("No entity resolved for ID: %llu", (unsigned long long)ui.entity_inspector_id);
      ImGui::TextDisabled("Tip: use OmniSearch (Ctrl+F) to find ids/paths, then right-click → Open in Entity Inspector.");
    }

    // Name search results (inline list).
    if (st.name_query[0] != '\0') {
      ImGui::Separator();
      ImGui::TextDisabled("Name matches");
      int shown = 0;
      const int cap = 20;
      for (const auto& kv : game_entity_index().by_id) {
        const auto& e = kv.second;
        if (e.name.empty()) continue;
        if (!icontains(e.name, st.name_query)) continue;
        if (shown >= cap) break;

        ImGui::PushID((int)e.id);
        std::string label = pretty_entity_label(e);
        if (ImGui::Selectable(label.c_str(), false)) {
          ui.entity_inspector_id = e.id;
          st.focus_id_input_next = true;
        }
        ImGui::PopID();
        shown++;
      }
      if (shown == 0) {
        ImGui::TextDisabled("(no matches)");
      } else if (shown >= cap) {
        ImGui::TextDisabled("(showing first %d)", cap);
      }
    }
  }
  ImGui::EndChild();

  // Bottom: inbound reference scan results.
  ImGui::BeginChild("##refs_panel", ImVec2(0, 0), true);
  {
    ImGui::TextDisabled("Inbound references (where this id appears in the live game JSON)");
    ImGui::Separator();

    // Auto-start scan if requested.
    const bool id_changed = (st.scan.target_id != ui.entity_inspector_id) || (st.scan.doc_revision != doc_revision);
    if (ui.entity_inspector_auto_scan && ui.entity_inspector_id != 0 && id_changed) {
      start_ref_scan(st, ui.entity_inspector_id, doc_revision, root);
    }

    // Step scan incrementally.
    scan_step(st, ui, entry, root);

    ImGui::Text("Scan: %llu nodes, %zu hits%s%s", (unsigned long long)st.scan.scanned_nodes, st.scan.hits.size(),
                st.scan.running ? " (running)" : "",
                st.scan.capped ? " (capped)" : "");

    ImGui::SameLine();
    ImGui::PushItemWidth(220.0f);
    ImGui::InputTextWithHint("##ref_filter", "filter paths (substring)", st.ref_filter, sizeof(st.ref_filter));
    ImGui::PopItemWidth();

    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
      st.ref_filter[0] = '\0';
    }

    ImGui::Separator();

    if (st.scan.target_id == 0) {
      ImGui::TextDisabled("(enter an entity id to scan references)");
    } else if (st.scan.running && st.scan.hits.empty()) {
      ImGui::TextDisabled("Scanning...");
    } else if (!st.scan.running && st.scan.hits.empty()) {
      ImGui::TextDisabled("(no references found)");
    }

    // Show hits.
    if (!st.scan.hits.empty()) {
      const ImGuiTableFlags tflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY;
      if (ImGui::BeginTable("##refs_table", 2, tflags, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableHeadersRow();

        // Build a filtered list of indices (cheap).
        std::vector<int> idxs;
        idxs.reserve(st.scan.hits.size());
        for (int i = 0; i < (int)st.scan.hits.size(); ++i) {
          if (!path_contains_filter(st.ref_filter, st.scan.hits[(std::size_t)i].path)) continue;
          idxs.push_back(i);
        }

        ImGuiListClipper clip;
        clip.Begin((int)idxs.size());
        while (clip.Step()) {
          for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row) {
            const RefHit& hit = st.scan.hits[(std::size_t)idxs[(std::size_t)row]];
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(row);
            if (ImGui::Selectable(hit.path.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
              if (ImGui::IsMouseDoubleClicked(0)) {
                open_in_json_explorer(ui, hit.path);
              }
            }
            if (ImGui::IsItemHovered()) {
              ImGui::BeginTooltip();
              ImGui::TextDisabled("%s", hit.path.c_str());
              ImGui::TextDisabled("double-click to open in JSON Explorer");
              ImGui::EndTooltip();
            }

            if (ImGui::BeginPopupContextItem("##ref_ctx")) {
              if (ImGui::MenuItem("Open in JSON Explorer")) {
                open_in_json_explorer(ui, hit.path);
              }
              if (ImGui::MenuItem("Copy path")) {
                ImGui::SetClipboardText(hit.path.c_str());
              }
              if (ImGui::MenuItem("Pin reference to Watchboard")) {
                ui.show_watchboard_window = true;
                (void)add_watch_item(ui, hit.path, "ref " + hit.key);
              }
              // If this is an id-like key, offer quick navigation to the referenced entity (itself).
              if (looks_like_id_key(hit.key) && entry) {
                if (ImGui::MenuItem("Open Entity Inspector (this id)")) {
                  open_in_entity_inspector(ui, entry->id);
                }
                if (ImGui::MenuItem("Open Reference Graph (this id)")) {
                  ui.show_reference_graph_window = true;
                  ui.reference_graph_focus_id = entry->id;
                }
              }
              ImGui::EndPopup();
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(hit.key.c_str());
            ImGui::PopID();
          }
        }

        ImGui::EndTable();
      }
    }

    ImGui::Separator();
    ImGui::TextDisabled("Settings");
    ImGui::SliderInt("Nodes per frame", &ui.entity_inspector_nodes_per_frame, 200, 20000);
    ImGui::SliderInt("Max refs", &ui.entity_inspector_max_refs, 50, 5000);
    ui.entity_inspector_max_refs = std::clamp(ui.entity_inspector_max_refs, 10, 500000);
  }
  ImGui::EndChild();

  ImGui::EndChild();

  ImGui::End();
}

} // namespace nebula4x::ui
