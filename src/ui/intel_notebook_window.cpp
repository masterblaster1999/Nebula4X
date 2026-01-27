#include "ui/intel_notebook_window.h"

#include "ui/imgui_includes.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nebula4x/core/date.h"
#include "nebula4x/util/file_io.h"
#include "nebula4x/util/log.h"
#include "nebula4x/util/time.h"

#include "ui/navigation.h"

namespace nebula4x::ui {

namespace {

// ---- small string helpers (ASCII-focused; good enough for tags/search) ----

static inline char to_lower_ascii(char c) {
  return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

static inline std::string lower_copy(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) out.push_back(to_lower_ascii(c));
  return out;
}

static inline bool ascii_iequals(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (to_lower_ascii(a[i]) != to_lower_ascii(b[i])) return false;
  }
  return true;
}

static inline bool ascii_icontains(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) return true;
  const std::string h = lower_copy(haystack);
  const std::string n = lower_copy(needle);
  return h.find(n) != std::string::npos;
}

static inline std::string ascii_trim(std::string s) {
  auto is_ws = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
  while (!s.empty() && is_ws(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
  while (!s.empty() && is_ws(static_cast<unsigned char>(s.back()))) s.pop_back();
  return s;
}

static inline std::string normalize_tag(std::string t) {
  t = ascii_trim(std::move(t));
  if (!t.empty() && t.front() == '#') t.erase(t.begin());
  for (char& ch : t) ch = to_lower_ascii(ch);
  for (char& ch : t) {
    if (ch == ' ' || ch == '\t') ch = '_';
  }
  if (t.size() > 24) t.resize(24);
  while (!t.empty() && t.back() == '_') t.pop_back();
  return t;
}

static inline bool note_has_tag(const SystemIntelNote& n, const std::string& tag_norm) {
  if (tag_norm.empty()) return false;
  for (const std::string& t : n.tags) {
    if (ascii_iequals(t, tag_norm)) return true;
  }
  return false;
}

static inline void dedupe_tags(std::vector<std::string>& tags) {
  // Normalize in-place then dedupe case-insensitively.
  for (std::string& t : tags) t = normalize_tag(t);

  std::vector<std::string> out;
  out.reserve(tags.size());
  for (const std::string& t : tags) {
    if (t.empty()) continue;
    bool exists = false;
    for (const std::string& e : out) {
      if (ascii_iequals(e, t)) {
        exists = true;
        break;
      }
    }
    if (!exists) out.push_back(t);
  }
  tags.swap(out);
}

const char* event_category_label(EventCategory c) {
  switch (c) {
    case EventCategory::General: return "General";
    case EventCategory::Research: return "Research";
    case EventCategory::Shipyard: return "Shipyard";
    case EventCategory::Construction: return "Construction";
    case EventCategory::Movement: return "Movement";
    case EventCategory::Combat: return "Combat";
    case EventCategory::Intel: return "Intel";
    case EventCategory::Exploration: return "Exploration";
    case EventCategory::Diplomacy: return "Diplomacy";
    case EventCategory::Terraforming: return "Terraforming";
  }
  return "General";
}

EventCategory event_category_from_idx(int idx) {
  // Keep in sync with UI list below.
  switch (idx) {
    case 1: return EventCategory::General;
    case 2: return EventCategory::Research;
    case 3: return EventCategory::Shipyard;
    case 4: return EventCategory::Construction;
    case 5: return EventCategory::Movement;
    case 6: return EventCategory::Combat;
    case 7: return EventCategory::Intel;
    case 8: return EventCategory::Exploration;
    case 9: return EventCategory::Diplomacy;
    case 10: return EventCategory::Terraforming;
    default: return EventCategory::General;
  }
}

int event_category_to_idx(EventCategory c) {
  switch (c) {
    case EventCategory::General: return 1;
    case EventCategory::Research: return 2;
    case EventCategory::Shipyard: return 3;
    case EventCategory::Construction: return 4;
    case EventCategory::Movement: return 5;
    case EventCategory::Combat: return 6;
    case EventCategory::Intel: return 7;
    case EventCategory::Exploration: return 8;
    case EventCategory::Diplomacy: return 9;
    case EventCategory::Terraforming: return 10;
  }
  return 1;
}

Id resolve_viewer_faction_id(const Simulation& sim, const UIState& ui, Id selected_ship) {
  const auto& s = sim.state();
  if (selected_ship != kInvalidId) {
    if (const auto* sh = find_ptr(s.ships, selected_ship)) return sh->faction_id;
  }
  return ui.viewer_faction_id;
}

struct TagCount {
  std::string tag;
  int count{0};
};

std::vector<TagCount> build_tag_counts(const std::unordered_map<Id, SystemIntelNote>& notes) {
  std::unordered_map<std::string, int> counts;
  for (const auto& kv : notes) {
    for (const std::string& raw : kv.second.tags) {
      const std::string t = normalize_tag(raw);
      if (t.empty()) continue;
      counts[t] += 1;
    }
  }

  std::vector<TagCount> out;
  out.reserve(counts.size());
  for (const auto& kv : counts) out.push_back(TagCount{kv.first, kv.second});

  std::sort(out.begin(), out.end(), [](const TagCount& a, const TagCount& b) {
    if (a.count != b.count) return a.count > b.count;
    return a.tag < b.tag;
  });
  return out;
}

bool note_is_effectively_empty(const SystemIntelNote& n) {
  return !n.pinned && n.text.empty() && n.tags.empty();
}

std::string build_notes_markdown(const Simulation& sim, Id viewer_faction_id, const Faction& fac, bool fog_of_war,
                                const std::string& search, bool pinned_only, const std::string& tag_filter) {
  const auto& s = sim.state();

  struct Row {
    Id sys_id{kInvalidId};
    std::string sys_name;
    const SystemIntelNote* note{nullptr};
    bool discovered{true};
  };

  std::vector<Row> rows;
  rows.reserve(fac.system_notes.size());

  for (const auto& kv : fac.system_notes) {
    const Id sys_id = kv.first;
    const SystemIntelNote& n = kv.second;

    const StarSystem* sys = find_ptr(s.systems, sys_id);
    const std::string sys_name =
        sys ? sys->name
            : std::string("(missing system #") + std::to_string((unsigned long long)sys_id) + ")";

    bool discovered = true;
    if (fog_of_war && viewer_faction_id != kInvalidId) {
      discovered = sim.is_system_discovered_by_faction(viewer_faction_id, sys_id);
    }

    if (pinned_only && !n.pinned) continue;
    if (!tag_filter.empty() && !note_has_tag(n, tag_filter)) continue;

    // Search across system name, tag list, and text.
    if (!search.empty()) {
      bool ok = false;
      if (ascii_icontains(sys_name, search)) ok = true;
      if (!ok && ascii_icontains(n.text, search)) ok = true;
      if (!ok) {
        for (const std::string& t : n.tags) {
          if (ascii_icontains(t, search)) {
            ok = true;
            break;
          }
        }
      }
      if (!ok) continue;
    }

    rows.push_back(Row{sys_id, sys_name, &n, discovered});
  }

  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.sys_name < b.sys_name; });

  std::string md;
  md += "# Intel Notebook — System Notes\n\n";
  md += "Viewer faction: " + fac.name + "\n\n";
  if (!tag_filter.empty()) md += "Filter: #" + tag_filter + "\n\n";
  if (pinned_only) md += "Filter: pinned only\n\n";
  if (!search.empty()) md += "Search: `" + search + "`\n\n";

  if (rows.empty()) {
    md += "(no matching notes)\n";
    return md;
  }

  for (const Row& r : rows) {
    md += "## " + r.sys_name;
    if (r.note && r.note->pinned) md += "  ⭐";
    if (!r.discovered) md += "  *(undiscovered)*";
    md += "\n\n";

    if (r.note && !r.note->tags.empty()) {
      md += "Tags: ";
      for (size_t i = 0; i < r.note->tags.size(); ++i) {
        md += "#" + normalize_tag(r.note->tags[i]);
        if (i + 1 < r.note->tags.size()) md += ", ";
      }
      md += "\n\n";
    }

    if (r.note && !r.note->text.empty()) {
      md += r.note->text;
      if (!md.empty() && md.back() != '\n') md += "\n";
      md += "\n";
    } else {
      md += "(no text)\n\n";
    }
  }

  return md;
}

std::string build_journal_markdown(const Simulation& sim, const Faction& fac, const std::string& search, int category_idx,
                                  int max_show) {
  (void)sim;

  std::vector<const JournalEntry*> rows;
  rows.reserve(fac.journal.size());

  const bool have_search = !search.empty();
  const bool have_cat = (category_idx != 0);
  const EventCategory cat = have_cat ? event_category_from_idx(category_idx) : EventCategory::General;

  for (const JournalEntry& je : fac.journal) {
    if (have_cat && je.category != cat) continue;
    if (have_search) {
      if (!ascii_icontains(je.title, search) && !ascii_icontains(je.text, search)) continue;
    }
    rows.push_back(&je);
  }

  // Newest first.
  std::sort(rows.begin(), rows.end(), [](const JournalEntry* a, const JournalEntry* b) {
    if (a->day != b->day) return a->day > b->day;
    if (a->hour != b->hour) return a->hour > b->hour;
    return a->seq > b->seq;
  });

  if (max_show > 0 && (int)rows.size() > max_show) rows.resize((size_t)max_show);

  std::string md;
  md += "# Intel Notebook — Journal\n\n";
  md += "Faction: " + fac.name + "\n\n";
  if (have_cat) md += std::string("Category: ") + event_category_label(cat) + "\n\n";
  if (have_search) md += "Search: `" + search + "`\n\n";

  if (rows.empty()) {
    md += "(no matching entries)\n";
    return md;
  }

  for (const JournalEntry* je : rows) {
    const nebula4x::Date d(je->day);
    const std::string dt = format_datetime(d, je->hour);

    md += "## [" + dt + "] " + std::string(event_category_label(je->category)) + " — " + je->title + "\n\n";
    if (!je->text.empty()) {
      md += je->text;
      if (!md.empty() && md.back() != '\n') md += "\n";
      md += "\n";
    } else {
      md += "(no text)\n\n";
    }
  }

  return md;
}

} // namespace

void draw_intel_notebook_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  ImGui::SetNextWindowSize(ImVec2(1120, 760), ImGuiCond_FirstUseEver);

  if (!ImGui::Begin("Intel Notebook", &ui.show_intel_notebook_window)) {
    ImGui::End();
    return;
  }

  auto& s = sim.state();

  // Resolve viewer faction.
  const Id viewer_faction_id = resolve_viewer_faction_id(sim, ui, selected_ship);
  Faction* fac = (viewer_faction_id != kInvalidId) ? find_ptr(s.factions, viewer_faction_id) : nullptr;

  if (!fac) {
    ImGui::TextWrapped("Select a viewer faction (Controls/Research) or select a ship to open the Intel Notebook.");
    ImGui::Separator();
    ImGui::TextDisabled("This window edits faction-persisted data: system intel notes + the curated journal.");
    ImGui::End();
    return;
  }

  // Notebook persistent UI state.
  struct NotebookState {
    int tab_idx{0};

    // Notes filters
    char notes_search[128]{};
    bool notes_pinned_only{false};
    bool notes_hide_undiscovered{true};
    std::string notes_tag_filter;
    Id notes_selected_system_id{kInvalidId};

    // Notes editor
    char notes_new_tag[32]{};
    std::string notes_edit_text;
    Id notes_edit_system_id{kInvalidId};

    // Tag bulk edit
    std::string tag_context;
    char tag_rename_to[32]{};

    // Export
    char export_path[256]{"intel_notebook_export.md"};
    std::string last_export_status;

    // Journal filters
    char journal_search[128]{};
    int journal_category_idx{0};  // 0 = all
    int journal_max_show{250};
    std::uint64_t journal_selected_seq{0};

    // Journal composer
    bool journal_compose_open{true};
    int compose_category_idx{1};
    std::string compose_title;
    std::string compose_text;
    bool compose_attach_system{true};
    bool compose_attach_ship{false};
    bool compose_attach_colony{false};
    bool compose_attach_body{false};
    bool compose_attach_anomaly{false};
    bool compose_attach_wreck{false};
    Id compose_anomaly_id{kInvalidId};
    Id compose_wreck_id{kInvalidId};

    // Journal editor
    bool journal_edit_mode{false};
    std::string journal_edit_title;
    std::string journal_edit_text;

    // Faction tracking (reset selection when viewer changes).
    Id last_viewer_faction{kInvalidId};
  };

  static NotebookState st;

  if (st.last_viewer_faction != viewer_faction_id) {
    st.last_viewer_faction = viewer_faction_id;
    st.notes_selected_system_id = kInvalidId;
    st.notes_edit_system_id = kInvalidId;
    st.notes_edit_text.clear();
    st.notes_tag_filter.clear();
    st.journal_selected_seq = 0;
    st.journal_edit_mode = false;
  }

  ImGui::TextDisabled("Faction: %s", fac->name.c_str());
  ImGui::SameLine();
  ImGui::TextDisabled("| Notes: %d | Journal: %d", (int)fac->system_notes.size(), (int)fac->journal.size());

  ImGui::Separator();

  if (ImGui::BeginTabBar("##intel_notebook_tabs")) {
    // ----------------------------- System Notes tab -----------------------------
    if (ImGui::BeginTabItem("System Notes")) {
      // Prune invalid notes (missing system ids) in the background; keep stable UX.
      {
        std::vector<Id> to_erase;
        for (const auto& kv : fac->system_notes) {
          if (kv.first == kInvalidId) to_erase.push_back(kv.first);
          if (find_ptr(s.systems, kv.first) == nullptr) to_erase.push_back(kv.first);
        }
        if (!to_erase.empty()) {
          for (Id id : to_erase) fac->system_notes.erase(id);
          if (st.notes_selected_system_id != kInvalidId && find_ptr(s.systems, st.notes_selected_system_id) == nullptr) {
            st.notes_selected_system_id = kInvalidId;
          }
        }
      }

      ImGui::BeginChild("##notes_left", ImVec2(260.0f, 0.0f), true);

      ImGui::SeparatorText("Filters");

      ImGui::TextDisabled("Search");
      ImGui::SetNextItemWidth(-1.0f);
      ImGui::InputTextWithHint("##notes_search", "system, tag, or text...", st.notes_search, sizeof(st.notes_search));

      ImGui::Checkbox("Pinned only", &st.notes_pinned_only);
      if (ui.fog_of_war) {
        ImGui::Checkbox("Hide undiscovered", &st.notes_hide_undiscovered);
      } else {
        st.notes_hide_undiscovered = false;
        ImGui::TextDisabled("Hide undiscovered (FoW off)");
      }

      ImGui::SeparatorText("Tags");

      const auto tags = build_tag_counts(fac->system_notes);

      if (ImGui::Selectable("All tags", st.notes_tag_filter.empty())) {
        st.notes_tag_filter.clear();
      }

      for (const TagCount& tc : tags) {
        std::string label = "#" + tc.tag + " (" + std::to_string(tc.count) + ")";
        const bool sel = ascii_iequals(st.notes_tag_filter, tc.tag);

        if (ImGui::Selectable((label + "##tag_" + tc.tag).c_str(), sel)) {
          st.notes_tag_filter = tc.tag;
        }

        // Context menu for bulk ops.
        if (ImGui::BeginPopupContextItem(("tag_ctx_" + tc.tag).c_str())) {
          st.tag_context = tc.tag;

          if (ImGui::MenuItem("Copy #tag")) {
            const std::string t = "#" + tc.tag;
            ImGui::SetClipboardText(t.c_str());
          }
          if (ImGui::MenuItem("Rename tag...")) {
            std::snprintf(st.tag_rename_to, sizeof(st.tag_rename_to), "%s", tc.tag.c_str());
            ImGui::OpenPopup("RenameTagModal");
          }
          if (ImGui::MenuItem("Remove tag from all notes")) {
            // Bulk remove
            for (auto it = fac->system_notes.begin(); it != fac->system_notes.end();) {
              auto& note = it->second;
              note.tags.erase(std::remove_if(note.tags.begin(), note.tags.end(),
                                             [&](const std::string& t) { return ascii_iequals(t, tc.tag); }),
                              note.tags.end());
              dedupe_tags(note.tags);
              if (note_is_effectively_empty(note)) {
                if (st.notes_selected_system_id == it->first) st.notes_selected_system_id = kInvalidId;
                it = fac->system_notes.erase(it);
              } else {
                ++it;
              }
            }
          }

          ImGui::EndPopup();
        }

        if (ImGui::BeginPopupModal("RenameTagModal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
          ImGui::Text("Rename tag #%s", st.tag_context.c_str());
          ImGui::Separator();
          ImGui::TextDisabled("New name");
          ImGui::SetNextItemWidth(240.0f);
          ImGui::InputText("##tag_rename_to", st.tag_rename_to, sizeof(st.tag_rename_to));

          const std::string new_name = normalize_tag(std::string(st.tag_rename_to));

          if (ImGui::Button("Apply")) {
            if (!st.tag_context.empty() && !new_name.empty()) {
              for (auto it = fac->system_notes.begin(); it != fac->system_notes.end();) {
                auto& note = it->second;
                for (std::string& t : note.tags) {
                  if (ascii_iequals(t, st.tag_context)) t = new_name;
                }
                dedupe_tags(note.tags);
                if (note_is_effectively_empty(note)) {
                  if (st.notes_selected_system_id == it->first) st.notes_selected_system_id = kInvalidId;
                  it = fac->system_notes.erase(it);
                } else {
                  ++it;
                }
              }
              if (ascii_iequals(st.notes_tag_filter, st.tag_context)) st.notes_tag_filter = new_name;
            }
            ImGui::CloseCurrentPopup();
          }
          ImGui::SameLine();
          if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
          }
          ImGui::EndPopup();
        }
      }

      ImGui::Separator();
      if (ImGui::SmallButton("Open on Galaxy Map")) {
        ui.show_map_window = true;
        ui.request_map_tab = MapTab::Galaxy;
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Opens the Galaxy Map; system notes are visible as pins/overlays there too.");
      }

      ImGui::EndChild();

      ImGui::SameLine();

      // Right side: list + editor.
      ImGui::BeginChild("##notes_right", ImVec2(0.0f, 0.0f), false);

      // Build filtered list of notes.
      struct NoteRow {
        Id sys_id{kInvalidId};
        std::string sys_name;
        bool discovered{true};
        bool pinned{false};
        SystemIntelNote* note{nullptr};
      };

      const std::string search = ascii_trim(std::string(st.notes_search));
      const bool have_search = !search.empty();

      std::vector<NoteRow> rows;
      rows.reserve(fac->system_notes.size());

      for (auto& kv : fac->system_notes) {
        const Id sys_id = kv.first;
        SystemIntelNote& note = kv.second;

        const StarSystem* sys = find_ptr(s.systems, sys_id);
        if (!sys) continue;

        bool discovered = true;
        if (ui.fog_of_war && viewer_faction_id != kInvalidId) {
          discovered = sim.is_system_discovered_by_faction(viewer_faction_id, sys_id);
        }

        if (st.notes_hide_undiscovered && ui.fog_of_war && !discovered) continue;
        if (st.notes_pinned_only && !note.pinned) continue;
        if (!st.notes_tag_filter.empty() && !note_has_tag(note, st.notes_tag_filter)) continue;

        if (have_search) {
          bool ok = false;
          if (ascii_icontains(sys->name, search)) ok = true;
          if (!ok && ascii_icontains(note.text, search)) ok = true;
          if (!ok) {
            for (const std::string& t : note.tags) {
              if (ascii_icontains(t, search)) {
                ok = true;
                break;
              }
            }
          }
          if (!ok) continue;
        }

        rows.push_back(NoteRow{sys_id, sys->name, discovered, note.pinned, &note});
      }

      std::sort(rows.begin(), rows.end(), [](const NoteRow& a, const NoteRow& b) { return a.sys_name < b.sys_name; });

      // Top bar
      ImGui::SeparatorText("Notes");

      if (ImGui::SmallButton("Copy Markdown")) {
        const std::string md = build_notes_markdown(sim, viewer_faction_id, *fac, ui.fog_of_war, search,
                                                   st.notes_pinned_only, st.notes_tag_filter);
        ImGui::SetClipboardText(md.c_str());
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Export Markdown...")) {
        ImGui::OpenPopup("Export Notes");
      }

      if (ImGui::BeginPopupModal("Export Notes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Export visible system notes to Markdown");
        ImGui::Separator();
        ImGui::TextDisabled("Path");
        ImGui::SetNextItemWidth(420.0f);
        ImGui::InputText("##export_path_notes", st.export_path, sizeof(st.export_path));

        if (ImGui::Button("Write file")) {
          try {
            const std::string md = build_notes_markdown(sim, viewer_faction_id, *fac, ui.fog_of_war, search,
                                                       st.notes_pinned_only, st.notes_tag_filter);
            nebula4x::write_text_file(st.export_path, md);
            st.last_export_status = std::string("Wrote ") + st.export_path;
          } catch (const std::exception& e) {
            st.last_export_status = std::string("Export failed: ") + e.what();
            nebula4x::log::warn(st.last_export_status);
          }
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
          ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
      }

      if (!st.last_export_status.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", st.last_export_status.c_str());
      }

      const float avail_w = ImGui::GetContentRegionAvail().x;
      const float left_w = std::max(420.0f, avail_w * 0.56f);
      const float right_w = std::max(300.0f, avail_w - left_w - 10.0f);

      ImGui::BeginChild("##notes_list", ImVec2(left_w, 0.0f), true);

      if (rows.empty()) {
        ImGui::TextDisabled("(no matching notes)");
      } else {
        const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                   ImGuiTableFlags_Resizable;
        if (ImGui::BeginTable("##notes_tbl", 4, tf)) {
          ImGui::TableSetupColumn("⭐", ImGuiTableColumnFlags_WidthFixed, 24.0f);
          ImGui::TableSetupColumn("System", ImGuiTableColumnFlags_WidthStretch, 0.50f);
          ImGui::TableSetupColumn("Tags", ImGuiTableColumnFlags_WidthStretch, 0.25f);
          ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthStretch, 0.75f);
          ImGui::TableHeadersRow();

          for (const NoteRow& r : rows) {
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(r.pinned ? "★" : "");

            ImGui::TableSetColumnIndex(1);
            const bool sel = (st.notes_selected_system_id == r.sys_id);
            if (ImGui::Selectable((r.sys_name + "##sys_" + std::to_string((unsigned long long)r.sys_id)).c_str(), sel,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
              st.notes_selected_system_id = r.sys_id;
              // Reset edit buffer if switching.
              if (st.notes_edit_system_id != r.sys_id) {
                st.notes_edit_system_id = r.sys_id;
                st.notes_edit_text = r.note ? r.note->text : std::string();
              }
            }
            if (!r.discovered) {
              ImGui::SameLine();
              ImGui::TextDisabled(" (undiscovered)");
            }

            ImGui::TableSetColumnIndex(2);
            if (r.note && !r.note->tags.empty()) {
              std::string t;
              for (size_t i = 0; i < r.note->tags.size(); ++i) {
                const std::string n = normalize_tag(r.note->tags[i]);
                if (n.empty()) continue;
                t += "#" + n;
                if (i + 1 < r.note->tags.size()) t += " ";
              }
              ImGui::TextUnformatted(t.c_str());
            } else {
              ImGui::TextDisabled("-");
            }

            ImGui::TableSetColumnIndex(3);
            if (r.note && !r.note->text.empty()) {
              std::string preview = r.note->text;
              // Single-line preview
              for (char& c : preview) {
                if (c == '\n' || c == '\r') c = ' ';
              }
              if (preview.size() > 90) preview.resize(90);
              ImGui::TextUnformatted(preview.c_str());
            } else {
              ImGui::TextDisabled("(empty)");
            }
          }

          ImGui::EndTable();
        }
      }

      ImGui::EndChild();

      ImGui::SameLine();

      ImGui::BeginChild("##notes_editor", ImVec2(right_w, 0.0f), true);

      ImGui::SeparatorText("Editor");

      SystemIntelNote* edit_note = nullptr;
      StarSystem* edit_sys = nullptr;

      if (st.notes_selected_system_id != kInvalidId) {
        edit_note = &fac->system_notes[st.notes_selected_system_id]; // creates if missing
        edit_sys = find_ptr(s.systems, st.notes_selected_system_id);
        if (!edit_sys) {
          st.notes_selected_system_id = kInvalidId;
          edit_note = nullptr;
        }
      }

      if (!edit_note || !edit_sys) {
        ImGui::TextDisabled("Select a note from the list.\n\nTip: you can create a note by selecting a system then editing its text/tags.");
        ImGui::EndChild();
        ImGui::EndChild();
        ImGui::EndTabItem();
        ImGui::EndTabBar();
        ImGui::End();
        return;
      }

      // Header controls
      ImGui::Text("%s", edit_sys->name.c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton("Jump")) {
        apply_nav_target(sim, ui, selected_ship, selected_colony, selected_body, NavTarget{NavTargetKind::System, edit_sys->id},
                         true);
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Open Galaxy Map and center on this system.");
      }

      bool pinned = edit_note->pinned;
      if (ImGui::Checkbox("Pinned", &pinned)) {
        edit_note->pinned = pinned;
      }

      // Tags
      ImGui::SeparatorText("Tags");

      dedupe_tags(edit_note->tags);

      if (!edit_note->tags.empty()) {
        for (size_t i = 0; i < edit_note->tags.size(); ++i) {
          const std::string t = normalize_tag(edit_note->tags[i]);
          if (t.empty()) continue;

          ImGui::PushID((int)i);
          if (ImGui::SmallButton(("#" + t).c_str())) {
            st.notes_tag_filter = t;
          }
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Filter by this tag (click)");
          }
          ImGui::SameLine();
          if (ImGui::SmallButton("x")) {
            edit_note->tags.erase(edit_note->tags.begin() + (std::ptrdiff_t)i);
            --i;
          }
          ImGui::PopID();
        }
      } else {
        ImGui::TextDisabled("(no tags)");
      }

      ImGui::Spacing();
      ImGui::TextDisabled("Add tag");
      ImGui::SetNextItemWidth(-1.0f);
      ImGui::InputTextWithHint("##new_tag", "#tag", st.notes_new_tag, sizeof(st.notes_new_tag));
      if (ImGui::IsItemDeactivatedAfterEdit() || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        const std::string nt = normalize_tag(std::string(st.notes_new_tag));
        if (!nt.empty()) {
          edit_note->tags.push_back(nt);
          dedupe_tags(edit_note->tags);
        }
        st.notes_new_tag[0] = '\0';
      }

      // Text
      ImGui::SeparatorText("Note");

      if (st.notes_edit_system_id != edit_sys->id) {
        st.notes_edit_system_id = edit_sys->id;
        st.notes_edit_text = edit_note->text;
      }

      ImGui::SetNextItemWidth(-1.0f);
      ImGui::InputTextMultiline("##note_text", &st.notes_edit_text, ImVec2(0.0f, 260.0f),
                                ImGuiInputTextFlags_AllowTabInput);

      if (ImGui::Button("Save note")) {
        edit_note->text = st.notes_edit_text;
        if (note_is_effectively_empty(*edit_note)) {
          fac->system_notes.erase(edit_sys->id);
          st.notes_selected_system_id = kInvalidId;
        }
      }
      ImGui::SameLine();
      if (ImGui::Button("Revert")) {
        st.notes_edit_text = edit_note->text;
      }
      ImGui::SameLine();
      if (ImGui::Button("Delete note")) {
        ImGui::OpenPopup("DeleteNoteModal");
      }

      if (ImGui::BeginPopupModal("DeleteNoteModal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Delete this note for system '%s'?", edit_sys->name.c_str());
        ImGui::Separator();
        if (ImGui::Button("Delete")) {
          fac->system_notes.erase(edit_sys->id);
          st.notes_selected_system_id = kInvalidId;
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
      }

      ImGui::EndChild(); // editor
      ImGui::EndChild(); // notes right
      ImGui::EndTabItem();
    }

    // ----------------------------- Journal tab -----------------------------
    if (ImGui::BeginTabItem("Journal")) {
      const float avail_w = ImGui::GetContentRegionAvail().x;
      const float left_w = std::max(520.0f, avail_w * 0.58f);
      const float right_w = std::max(320.0f, avail_w - left_w - 10.0f);

      // Left side: composer + list
      ImGui::BeginChild("##journal_left", ImVec2(left_w, 0.0f), false);

      if (ImGui::CollapsingHeader("New entry", st.journal_compose_open ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        st.journal_compose_open = true;

        const char* cats[] = {
            "(choose)",
            "General",
            "Research",
            "Shipyard",
            "Construction",
            "Movement",
            "Combat",
            "Intel",
            "Exploration",
            "Diplomacy",
            "Terraforming",
        };

        ImGui::Combo("Category", &st.compose_category_idx, cats, IM_ARRAYSIZE(cats));

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##compose_title", "Title", &st.compose_title);

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextMultiline("##compose_text", &st.compose_text, ImVec2(0.0f, 120.0f),
                                  ImGuiInputTextFlags_AllowTabInput);

        ImGui::SeparatorText("Attach context");
        // By default, attach whatever is selected/active.
        ImGui::Checkbox("System", &st.compose_attach_system);
        ImGui::SameLine();
        ImGui::Checkbox("Ship", &st.compose_attach_ship);
        ImGui::SameLine();
        ImGui::Checkbox("Colony", &st.compose_attach_colony);
        ImGui::SameLine();
        ImGui::Checkbox("Body", &st.compose_attach_body);

        ImGui::Checkbox("Anomaly", &st.compose_attach_anomaly);
        ImGui::SameLine();
        ImGui::Checkbox("Wreck", &st.compose_attach_wreck);

        if (st.compose_attach_anomaly) {
          ImGui::Spacing();
          ImGui::TextDisabled("Anomaly (in selected system)");
          ImGui::SetNextItemWidth(-1.0f);

          // Build a stable list of anomalies in the currently selected system.
          std::vector<const Anomaly*> anomalies;
          anomalies.reserve(s.anomalies.size());
          for (const auto& kv : s.anomalies) {
            const Anomaly& a = kv.second;
            if (s.selected_system != kInvalidId && a.system_id == s.selected_system) {
              anomalies.push_back(&a);
            }
          }
          std::sort(anomalies.begin(), anomalies.end(),
                    [](const Anomaly* a, const Anomaly* b) { return a->name < b->name; });

          const Anomaly* current = (st.compose_anomaly_id != kInvalidId) ? find_ptr(s.anomalies, st.compose_anomaly_id) : nullptr;
          const char* preview = current ? current->name.c_str() : "(none)";

          if (ImGui::BeginCombo("##compose_anomaly", preview)) {
            if (ImGui::Selectable("(none)", st.compose_anomaly_id == kInvalidId)) {
              st.compose_anomaly_id = kInvalidId;
            }
            for (const Anomaly* a : anomalies) {
              const bool sel = (st.compose_anomaly_id == a->id);
              if (ImGui::Selectable(a->name.c_str(), sel)) {
                st.compose_anomaly_id = a->id;
              }
            }
            ImGui::EndCombo();
          }
          if (anomalies.empty()) {
            ImGui::TextDisabled("(no anomalies in selected system)");
          }
        }

        if (st.compose_attach_wreck) {
          ImGui::Spacing();
          ImGui::TextDisabled("Wreck (in selected system)");
          ImGui::SetNextItemWidth(-1.0f);

          std::vector<const Wreck*> wrecks;
          wrecks.reserve(s.wrecks.size());
          for (const auto& kv : s.wrecks) {
            const Wreck& w = kv.second;
            if (s.selected_system != kInvalidId && w.system_id == s.selected_system) {
              wrecks.push_back(&w);
            }
          }
          std::sort(wrecks.begin(), wrecks.end(), [](const Wreck* a, const Wreck* b) { return a->name < b->name; });

          const Wreck* current = (st.compose_wreck_id != kInvalidId) ? find_ptr(s.wrecks, st.compose_wreck_id) : nullptr;
          const char* preview = current ? current->name.c_str() : "(none)";

          if (ImGui::BeginCombo("##compose_wreck", preview)) {
            if (ImGui::Selectable("(none)", st.compose_wreck_id == kInvalidId)) {
              st.compose_wreck_id = kInvalidId;
            }
            for (const Wreck* w : wrecks) {
              const bool sel = (st.compose_wreck_id == w->id);
              if (ImGui::Selectable(w->name.c_str(), sel)) {
                st.compose_wreck_id = w->id;
              }
            }
            ImGui::EndCombo();
          }
          if (wrecks.empty()) {
            ImGui::TextDisabled("(no wrecks in selected system)");
          }
        }

        ImGui::Separator();

        const std::string title = ascii_trim(st.compose_title);
        const std::string text = st.compose_text;

        if (ImGui::Button("Add to journal")) {
          JournalEntry je;
          je.category = event_category_from_idx(st.compose_category_idx);
          je.title = title.empty() ? std::string("Note") : title;
          je.text = text;

          // Attach requested context using current selection.
          if (st.compose_attach_system) je.system_id = s.selected_system;
          if (st.compose_attach_ship && selected_ship != kInvalidId) je.ship_id = selected_ship;
          if (st.compose_attach_colony && selected_colony != kInvalidId) je.colony_id = selected_colony;
          if (st.compose_attach_body && selected_body != kInvalidId) je.body_id = selected_body;

          if (st.compose_attach_anomaly && st.compose_anomaly_id != kInvalidId) {
            je.anomaly_id = st.compose_anomaly_id;
            if (!st.compose_attach_system) je.system_id = s.selected_system;
          }
          if (st.compose_attach_wreck && st.compose_wreck_id != kInvalidId) {
            je.wreck_id = st.compose_wreck_id;
            if (!st.compose_attach_system) je.system_id = s.selected_system;
          }

          sim.add_journal_entry(viewer_faction_id, je);

          // Reset compose buffer but keep category.
          st.compose_title.clear();
          st.compose_text.clear();
        }

        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("Creates a persistent Journal entry for this faction (saved with the game).");
        }
      } else {
        st.journal_compose_open = false;
      }

      ImGui::SeparatorText("Filters");

      ImGui::TextDisabled("Search");
      ImGui::SetNextItemWidth(-1.0f);
      ImGui::InputTextWithHint("##journal_search", "title or text...", st.journal_search, sizeof(st.journal_search));

      const char* cats[] = {
          "All categories",
          "General",
          "Research",
          "Shipyard",
          "Construction",
          "Movement",
          "Combat",
          "Intel",
          "Exploration",
          "Diplomacy",
          "Terraforming",
      };
      ImGui::Combo("Category", &st.journal_category_idx, cats, IM_ARRAYSIZE(cats));

      ImGui::SliderInt("Max shown", &st.journal_max_show, 50, 2000);
      st.journal_max_show = std::clamp(st.journal_max_show, 50, 5000);

      // Build filtered list.
      const std::string jsearch = ascii_trim(std::string(st.journal_search));
      const bool have_search = !jsearch.empty();
      const bool have_cat = (st.journal_category_idx != 0);
      const EventCategory cat = have_cat ? event_category_from_idx(st.journal_category_idx) : EventCategory::General;

      std::vector<const JournalEntry*> entries;
      entries.reserve(fac->journal.size());
      for (const JournalEntry& je : fac->journal) {
        if (have_cat && je.category != cat) continue;
        if (have_search && !ascii_icontains(je.title, jsearch) && !ascii_icontains(je.text, jsearch)) continue;
        entries.push_back(&je);
      }

      std::sort(entries.begin(), entries.end(), [](const JournalEntry* a, const JournalEntry* b) {
        if (a->day != b->day) return a->day > b->day;
        if (a->hour != b->hour) return a->hour > b->hour;
        return a->seq > b->seq;
      });

      if (st.journal_max_show > 0 && (int)entries.size() > st.journal_max_show) entries.resize((size_t)st.journal_max_show);

      ImGui::TextDisabled("Entries: %d (filtered)", (int)entries.size());

      if (ImGui::SmallButton("Copy Markdown")) {
        const std::string md = build_journal_markdown(sim, *fac, jsearch, st.journal_category_idx, st.journal_max_show);
        ImGui::SetClipboardText(md.c_str());
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Export Markdown...")) {
        ImGui::OpenPopup("Export Journal");
      }

      if (ImGui::BeginPopupModal("Export Journal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Export visible journal entries to Markdown");
        ImGui::Separator();
        ImGui::TextDisabled("Path");
        ImGui::SetNextItemWidth(420.0f);
        ImGui::InputText("##export_path_journal", st.export_path, sizeof(st.export_path));

        if (ImGui::Button("Write file")) {
          try {
            const std::string md =
                build_journal_markdown(sim, *fac, jsearch, st.journal_category_idx, st.journal_max_show);
            nebula4x::write_text_file(st.export_path, md);
            st.last_export_status = std::string("Wrote ") + st.export_path;
          } catch (const std::exception& e) {
            st.last_export_status = std::string("Export failed: ") + e.what();
            nebula4x::log::warn(st.last_export_status);
          }
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
          ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
      }

      if (!st.last_export_status.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", st.last_export_status.c_str());
      }

      ImGui::Separator();

      // List
      ImGui::BeginChild("##journal_list", ImVec2(0.0f, 0.0f), true);
      if (entries.empty()) {
        ImGui::TextDisabled("(no matching entries)");
      } else {
        for (const JournalEntry* je : entries) {
          const nebula4x::Date d(je->day);
          const std::string dt = format_datetime(d, je->hour);

          std::string header = "[" + dt + "] " + std::string(event_category_label(je->category)) + ": " + je->title;
          const bool sel = (st.journal_selected_seq == je->seq);

          if (ImGui::Selectable((header + "##je_" + std::to_string((unsigned long long)je->seq)).c_str(), sel)) {
            st.journal_selected_seq = je->seq;
            st.journal_edit_mode = false;
          }
        }
      }
      ImGui::EndChild();

      ImGui::EndChild();

      ImGui::SameLine();

      // Right side: selected entry details/editor.
      ImGui::BeginChild("##journal_right", ImVec2(right_w, 0.0f), true);

      ImGui::SeparatorText("Details");

      // Find selected entry.
      JournalEntry* selected = nullptr;
      if (st.journal_selected_seq != 0) {
        for (JournalEntry& je : fac->journal) {
          if (je.seq == st.journal_selected_seq) {
            selected = &je;
            break;
          }
        }
      }

      if (!selected) {
        ImGui::TextDisabled("Select a journal entry from the list.");
        ImGui::EndChild();
        ImGui::EndTabItem();
        ImGui::EndTabBar();
        ImGui::End();
        return;
      }

      const nebula4x::Date d(selected->day);
      const std::string dt = format_datetime(d, selected->hour);

      ImGui::Text("%s", dt.c_str());
      ImGui::TextDisabled("#%llu", (unsigned long long)selected->seq);
      ImGui::Separator();

      ImGui::Text("Category: %s", event_category_label(selected->category));

      // Jump shortcuts.
      if (selected->system_id != kInvalidId) {
        if (ImGui::SmallButton("View system")) {
          apply_nav_target(sim, ui, selected_ship, selected_colony, selected_body,
                           NavTarget{NavTargetKind::System, selected->system_id}, true);
        }
      }
      if (selected->ship_id != kInvalidId) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Select ship")) {
          apply_nav_target(sim, ui, selected_ship, selected_colony, selected_body,
                           NavTarget{NavTargetKind::Ship, selected->ship_id}, true);
        }
      }
      if (selected->colony_id != kInvalidId) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Select colony")) {
          apply_nav_target(sim, ui, selected_ship, selected_colony, selected_body,
                           NavTarget{NavTargetKind::Colony, selected->colony_id}, true);
        }
      }
      if (selected->body_id != kInvalidId) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Select body")) {
          apply_nav_target(sim, ui, selected_ship, selected_colony, selected_body,
                           NavTarget{NavTargetKind::Body, selected->body_id}, true);
        }
      }

      if (selected->anomaly_id != kInvalidId) {
        ImGui::Spacing();
        if (ImGui::SmallButton("Center anomaly")) {
          if (const auto* a = find_ptr(s.anomalies, selected->anomaly_id)) {
            s.selected_system = a->system_id;
            ui.show_map_window = true;
            ui.request_map_tab = MapTab::System;
            ui.request_system_map_center = true;
            ui.request_system_map_center_system_id = a->system_id;
            ui.request_system_map_center_x_mkm = a->position_mkm.x;
            ui.request_system_map_center_y_mkm = a->position_mkm.y;
            ui.request_system_map_center_zoom = 0.0;
          }
        }
      }
      if (selected->wreck_id != kInvalidId) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Center wreck")) {
          if (const auto* w = find_ptr(s.wrecks, selected->wreck_id)) {
            s.selected_system = w->system_id;
            ui.show_map_window = true;
            ui.request_map_tab = MapTab::System;
            ui.request_system_map_center = true;
            ui.request_system_map_center_system_id = w->system_id;
            ui.request_system_map_center_x_mkm = w->position_mkm.x;
            ui.request_system_map_center_y_mkm = w->position_mkm.y;
            ui.request_system_map_center_zoom = 0.0;
          }
        }
      }

      ImGui::Separator();

      if (!st.journal_edit_mode) {
        ImGui::TextWrapped("%s", selected->title.c_str());
        if (!selected->text.empty()) {
          ImGui::Spacing();
          ImGui::TextWrapped("%s", selected->text.c_str());
        } else {
          ImGui::Spacing();
          ImGui::TextDisabled("(no text)");
        }

        ImGui::Separator();
        if (ImGui::SmallButton("Edit")) {
          st.journal_edit_mode = true;
          st.journal_edit_title = selected->title;
          st.journal_edit_text = selected->text;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete")) {
          ImGui::OpenPopup("Delete journal entry");
        }

        if (ImGui::BeginPopupModal("Delete journal entry", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
          ImGui::TextWrapped("Delete this journal entry? This cannot be undone.");
          ImGui::Separator();
          if (ImGui::Button("Delete")) {
            const std::uint64_t seq = selected->seq;
            fac->journal.erase(std::remove_if(fac->journal.begin(), fac->journal.end(),
                                             [&](const JournalEntry& je) { return je.seq == seq; }),
                               fac->journal.end());
            st.journal_selected_seq = 0;
            st.journal_edit_mode = false;
            ImGui::CloseCurrentPopup();
          }
          ImGui::SameLine();
          if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
          ImGui::EndPopup();
        }
      } else {
        ImGui::TextDisabled("Edit mode");

        const char* cats2[] = {
            "(choose)",
            "General",
            "Research",
            "Shipyard",
            "Construction",
            "Movement",
            "Combat",
            "Intel",
            "Exploration",
            "Diplomacy",
            "Terraforming",
        };
        int cat_idx = event_category_to_idx(selected->category);
        ImGui::Combo("Category", &cat_idx, cats2, IM_ARRAYSIZE(cats2));

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##edit_title", "Title", &st.journal_edit_title);

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextMultiline("##edit_text", &st.journal_edit_text, ImVec2(0.0f, 220.0f),
                                  ImGuiInputTextFlags_AllowTabInput);

        if (ImGui::Button("Save")) {
          selected->category = event_category_from_idx(cat_idx);
          selected->title = ascii_trim(st.journal_edit_title);
          selected->text = st.journal_edit_text;
          st.journal_edit_mode = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
          st.journal_edit_mode = false;
        }
      }

      ImGui::EndChild();
      ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
  }

  ImGui::End();
}

} // namespace nebula4x::ui
