#include "ui/notifications_window.h"

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nebula4x/core/date.h"
#include "nebula4x/core/entities.h"
#include "nebula4x/util/strings.h"

#include "ui/notifications.h"

namespace nebula4x::ui {

namespace {

struct NotificationsUi {
  bool initialized{false};

  char filter[128]{};
  bool unread_only{false};

  bool show_info{false};
  bool show_warn{true};
  bool show_error{true};

  bool show_sim_events{true};
  bool show_watchboard{true};

  bool newest_first{true};

  std::uint64_t selected_id{0};
  bool request_scroll_to_selected{false};
};

NotificationsUi& st() {
  static NotificationsUi s;
  return s;
}

const char* level_short(int level) {
  switch (static_cast<EventLevel>(level)) {
    case EventLevel::Info:
      return "INFO";
    case EventLevel::Warn:
      return "WARN";
    case EventLevel::Error:
      return "ERROR";
  }
  return "?";
}

ImVec4 level_color(int level) {
  switch (static_cast<EventLevel>(level)) {
    case EventLevel::Info:
      return ImVec4(0.70f, 0.75f, 0.85f, 1.0f);
    case EventLevel::Warn:
      return ImVec4(0.92f, 0.76f, 0.30f, 1.0f);
    case EventLevel::Error:
      return ImVec4(0.95f, 0.35f, 0.35f, 1.0f);
  }
  return ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
}

const char* category_label(int category) {
  switch (static_cast<EventCategory>(category)) {
    case EventCategory::General:
      return "General";
    case EventCategory::Research:
      return "Research";
    case EventCategory::Shipyard:
      return "Shipyard";
    case EventCategory::Construction:
      return "Construction";
    case EventCategory::Movement:
      return "Movement";
    case EventCategory::Combat:
      return "Combat";
    case EventCategory::Intel:
      return "Intel";
    case EventCategory::Exploration:
      return "Exploration";
    case EventCategory::Diplomacy:
      return "Diplomacy";
    case EventCategory::Terraforming:
      return "Terraforming";
  }
  return "?";
}

std::string format_day_hour(std::int64_t day, int hour) {
  const Date d = Date::from_days_since_epoch(day);
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%s %02d:00", d.to_string().c_str(), hour);
  return std::string(buf);
}

bool passes_level_filter(const NotificationsUi& ui, int level) {
  switch (static_cast<EventLevel>(level)) {
    case EventLevel::Info:
      return ui.show_info;
    case EventLevel::Warn:
      return ui.show_warn;
    case EventLevel::Error:
      return ui.show_error;
  }
  return true;
}

bool passes_source_filter(const NotificationsUi& ui, NotificationSource src) {
  if (src == NotificationSource::SimEvent) return ui.show_sim_events;
  if (src == NotificationSource::WatchboardAlert) return ui.show_watchboard;
  return true;
}

bool contains_ci(std::string_view hay, std::string_view needle) {
  if (needle.empty()) return true;
  const std::string h = nebula4x::to_lower(std::string(hay));
  const std::string n = nebula4x::to_lower(std::string(needle));
  return h.find(n) != std::string::npos;
}

std::optional<std::size_t> find_index_by_id(const UIState& ui, std::uint64_t id) {
  if (id == 0) return std::nullopt;
  for (std::size_t i = 0; i < ui.notifications.size(); ++i) {
    if (ui.notifications[i].id == id) return i;
  }
  return std::nullopt;
}

void clear_read(UIState& ui) {
  ui.notifications.erase(
      std::remove_if(ui.notifications.begin(), ui.notifications.end(), [](const NotificationEntry& e) {
        return !e.pinned && !e.unread;
      }),
      ui.notifications.end());
}

void clear_all_unpinned(UIState& ui) {
  ui.notifications.erase(
      std::remove_if(ui.notifications.begin(), ui.notifications.end(), [](const NotificationEntry& e) {
        return !e.pinned;
      }),
      ui.notifications.end());
}

void focus_context(Simulation& sim, UIState& ui, const NotificationEntry& e, Id& selected_ship, Id& selected_colony,
                   Id& selected_body) {
  // Prefer entity selection if present; fall back to system selection.
  if (e.ship_id != kInvalidId) {
    selected_ship = e.ship_id;
    ui.show_details_window = true;
    ui.request_details_tab = DetailsTab::Orders;
    return;
  }
  if (e.colony_id != kInvalidId) {
    selected_colony = e.colony_id;
    ui.show_details_window = true;
    ui.request_details_tab = DetailsTab::Colony;
    // Also update body selection when available.
    if (const auto* c = find_ptr(sim.state().colonies, selected_colony)) {
      selected_body = c->body_id;
    }
    return;
  }
  if (e.system_id != kInvalidId) {
    sim.state().selected_system = e.system_id;
    ui.show_map_window = true;
    ui.request_map_tab = MapTab::System;
  }
}

void open_log_for_event(UIState& ui, const NotificationEntry& e) {
  if (e.source != NotificationSource::SimEvent) return;
  ui.show_details_window = true;
  ui.request_details_tab = DetailsTab::Log;
  ui.request_focus_event_seq = e.id;
  ui.last_seen_event_seq = std::max(ui.last_seen_event_seq, e.id);
}

void open_timeline_for_event(UIState& ui, const NotificationEntry& e) {
  if (e.source != NotificationSource::SimEvent) return;
  ui.show_timeline_window = true;
  ui.request_focus_event_seq = e.id;
}

void open_watchboard_for_alert(UIState& ui, const NotificationEntry& e) {
  if (e.source != NotificationSource::WatchboardAlert) return;
  ui.show_watchboard_window = true;
  if (e.watch_id != 0) ui.request_watchboard_focus_id = e.watch_id;
}

void open_json_explorer_for_alert(UIState& ui, const NotificationEntry& e) {
  if (e.source != NotificationSource::WatchboardAlert) return;
  if (e.watch_rep_ptr.empty()) return;
  ui.show_json_explorer_window = true;
  ui.request_json_explorer_goto_path = e.watch_rep_ptr;
}

void promote_to_journal(Simulation& sim, UIState& ui, const NotificationEntry& e, Id selected_ship,
                       Id selected_colony, Id selected_body) {
  Id target_faction = e.faction_id != kInvalidId ? e.faction_id : ui.viewer_faction_id;

  // If the notification isn't tied to a faction, fall back to selected ship faction (if any).
  if (target_faction == kInvalidId && selected_ship != kInvalidId) {
    if (const auto* sh = find_ptr(sim.state().ships, selected_ship)) target_faction = sh->faction_id;
  }

  if (target_faction == kInvalidId) return;

  JournalEntry je;
  je.category = static_cast<EventCategory>(e.category);

  // Title: first line of the message, truncated.
  je.title = e.message;
  const auto nl = je.title.find_first_of("\r\n");
  if (nl != std::string::npos) je.title = je.title.substr(0, nl);
  if (je.title.size() > 96) je.title.resize(96);

  const std::string original_ts = format_day_hour(e.day, e.hour);

  je.text = "Captured from Notification Center.\n";
  je.text += "Original time: " + original_ts + "\n\n";
  je.text += e.message;

  if (e.source == NotificationSource::WatchboardAlert) {
    if (!e.watch_label.empty()) {
      je.text += "\n\nWatchboard: " + e.watch_label;
    }
    if (!e.watch_path.empty()) {
      je.text += "\nPath: " + e.watch_path;
    }
  }

  // Carry context ids when available.
  je.system_id = e.system_id;
  je.ship_id = e.ship_id;
  je.colony_id = e.colony_id;
  je.body_id = e.body_id;
  je.anomaly_id = e.anomaly_id;
  je.wreck_id = e.wreck_id;

  sim.push_journal_entry(target_faction, je);

  // Surface the result.
  ui.show_intel_notebook_window = true;
}

} // namespace

void draw_notifications_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  if (!ui.show_notifications_window) return;

  NotificationsUi& s = st();

  static bool was_open = false;
  const bool just_opened = ui.show_notifications_window && !was_open;
  was_open = ui.show_notifications_window;

  if (just_opened && !s.initialized) {
    s.initialized = true;
    std::memset(s.filter, 0, sizeof(s.filter));
    s.unread_only = false;
    s.show_info = false;
    s.show_warn = true;
    s.show_error = true;
    s.show_sim_events = true;
    s.show_watchboard = true;
    s.newest_first = true;
    s.selected_id = 0;
    s.request_scroll_to_selected = false;
  }

  // External focus request (e.g. when a new notification arrives).
  if (ui.notifications_request_focus_id != 0) {
    s.selected_id = ui.notifications_request_focus_id;
    ui.notifications_request_focus_id = 0;
    s.request_scroll_to_selected = true;
  }

  ImGui::SetNextWindowSize(ImVec2(1080, 720), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Notification Center", &ui.show_notifications_window)) {
    ImGui::End();
    return;
  }

  // --- Toolbar ---
  {
    const int unread = notifications_unread_count(ui);
    ImGui::TextUnformatted("Inbox");
    ImGui::SameLine();
    ImGui::TextDisabled("(unread: %d / total: %d)", unread, (int)ui.notifications.size());

    ImGui::SameLine();
    if (ImGui::Button("Mark all read")) {
      notifications_mark_all_read(ui);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear read")) {
      clear_read(ui);
      if (!find_index_by_id(ui, s.selected_id)) s.selected_id = 0;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear all (unpinned)")) {
      clear_all_unpinned(ui);
      if (!find_index_by_id(ui, s.selected_id)) s.selected_id = 0;
    }
    ImGui::SameLine();
    if (ImGui::Button("Settings")) {
      ui.show_settings_window = true;
    }

    ImGui::Separator();

    ImGui::SetNextItemWidth(280.0f);
    ImGui::InputTextWithHint("##notif_filter", "Filter (text)", s.filter, sizeof(s.filter));
    ImGui::SameLine();
    ImGui::Checkbox("Unread only", &s.unread_only);
    ImGui::SameLine();
    ImGui::Checkbox("Info", &s.show_info);
    ImGui::SameLine();
    ImGui::Checkbox("Warn", &s.show_warn);
    ImGui::SameLine();
    ImGui::Checkbox("Error", &s.show_error);
    ImGui::SameLine();
    ImGui::Checkbox("Events", &s.show_sim_events);
    ImGui::SameLine();
    ImGui::Checkbox("Watchboard", &s.show_watchboard);
    ImGui::SameLine();
    ImGui::Checkbox("Newest first", &s.newest_first);
  }

  ImGui::Separator();

  // --- Split layout (list + details) ---
  if (ImGui::BeginTable("##notif_split", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
    ImGui::TableSetupColumn("List", ImGuiTableColumnFlags_WidthStretch, 0.58f);
    ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch, 0.42f);
    ImGui::TableNextRow();

    // --- Left: list ---
    ImGui::TableSetColumnIndex(0);
    {
      ImGui::BeginChild("##notif_list", ImVec2(0, 0), false);

      const std::string_view filter_sv = s.filter;

      // Build the list of visible indices in display order.
      std::vector<int> visible;
      visible.reserve(ui.notifications.size());
      const int n = static_cast<int>(ui.notifications.size());
      int selected_row = -1;
      auto consider = [&](int idx) {
        if (idx < 0 || idx >= n) return;
        const NotificationEntry& e = ui.notifications[static_cast<std::size_t>(idx)];
        if (s.unread_only && !e.unread) return;
        if (!passes_level_filter(s, e.level)) return;
        if (!passes_source_filter(s, e.source)) return;
        if (!contains_ci(e.message, filter_sv) && !contains_ci(e.watch_label, filter_sv) &&
            !contains_ci(e.watch_path, filter_sv)) {
          return;
        }
        if (e.id == s.selected_id) selected_row = static_cast<int>(visible.size());
        visible.push_back(idx);
      };
      if (s.newest_first) {
        for (int i = n - 1; i >= 0; --i) consider(i);
      } else {
        for (int i = 0; i < n; ++i) consider(i);
      }

      std::optional<std::size_t> request_delete_idx;

      // If an external focus request wants us to scroll to the selected row,
      // do it up front so the clipper renders the right region.
      if (s.request_scroll_to_selected && selected_row >= 0) {
        const float row_h = ImGui::GetTextLineHeightWithSpacing();
        const float child_h = ImGui::GetWindowHeight();
        const float target_y = row_h * static_cast<float>(selected_row);
        const float scroll_y = std::max(0.0f, target_y - child_h * 0.35f);
        ImGui::SetScrollY(scroll_y);
        s.request_scroll_to_selected = false;
      }

      ImGuiListClipper clipper;
      clipper.Begin(static_cast<int>(visible.size()));

      while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
          if (row < 0 || row >= (int)visible.size()) continue;
          const int idx = visible[static_cast<std::size_t>(row)];
          NotificationEntry& e = ui.notifications[static_cast<std::size_t>(idx)];

          ImGui::PushID((void*)(uintptr_t)e.id);

          // Row layout: [unread] [LEVEL] [time] [message] [xN] [pin]
          if (e.unread) {
            ImGui::TextUnformatted("●");
          } else {
            ImGui::TextUnformatted(" ");
          }
          ImGui::SameLine();

          ImGui::TextColored(level_color(e.level), "%s", level_short(e.level));
          ImGui::SameLine();

          const std::string ts = format_day_hour(e.day, e.hour);
          ImGui::TextDisabled("%s", ts.c_str());
          ImGui::SameLine();

          const bool selected = (s.selected_id == e.id);
          ImGuiSelectableFlags sel_flags = ImGuiSelectableFlags_SpanAvailWidth;
          if (ImGui::Selectable(e.message.c_str(), selected, sel_flags)) {
            s.selected_id = e.id;
            e.unread = false;
            s.request_scroll_to_selected = false;
          }

          // Context menu.
          if (ImGui::BeginPopupContextItem("##notif_ctx")) {
            if (ImGui::MenuItem(e.unread ? "Mark read" : "Mark unread")) {
              e.unread = !e.unread;
            }
            if (ImGui::MenuItem(e.pinned ? "Unpin" : "Pin")) {
              e.pinned = !e.pinned;
            }
            if (ImGui::MenuItem("Copy message")) {
              ImGui::SetClipboardText(e.message.c_str());
            }
            if (ImGui::MenuItem("Promote to Journal")) {
              promote_to_journal(sim, ui, e, selected_ship, selected_colony, selected_body);
              e.unread = false;
            }
            if (ImGui::MenuItem("Delete")) {
              request_delete_idx = static_cast<std::size_t>(idx);
            }
            ImGui::EndPopup();
          }

          // Badges.
          if (e.count > 1) {
            ImGui::SameLine();
            ImGui::TextDisabled("×%d", e.count);
          }
          if (e.pinned) {
            ImGui::SameLine();
            ImGui::TextDisabled("★");
          }

          ImGui::PopID();
        }
      }

      if (request_delete_idx && *request_delete_idx < ui.notifications.size()) {
        const std::uint64_t del_id = ui.notifications[*request_delete_idx].id;
        ui.notifications.erase(ui.notifications.begin() + static_cast<long>(*request_delete_idx));
        if (s.selected_id == del_id) s.selected_id = 0;
      }

      ImGui::EndChild();
    }

    // --- Right: details ---
    ImGui::TableSetColumnIndex(1);
    {
      ImGui::BeginChild("##notif_details", ImVec2(0, 0), false);

      const auto idx_opt = find_index_by_id(ui, s.selected_id);
      if (!idx_opt) {
        ImGui::TextDisabled("Select a notification on the left.");
        ImGui::EndChild();
        ImGui::TableNextRow();
        ImGui::EndTable();
        ImGui::End();
        return;
      }

      NotificationEntry& e = ui.notifications[*idx_opt];

      ImGui::TextColored(level_color(e.level), "%s", level_short(e.level));
      ImGui::SameLine();
      ImGui::TextDisabled("%s", category_label(e.category));

      const std::string ts = format_day_hour(e.day, e.hour);
      ImGui::TextDisabled("%s", ts.c_str());

      if (e.source == NotificationSource::SimEvent) {
        ImGui::TextDisabled("Source: Simulation event (#%llu)", (unsigned long long)e.id);
      } else {
        ImGui::TextDisabled("Source: Watchboard alert (#%llu)", (unsigned long long)e.id);
      }

      ImGui::Separator();

      ImGui::TextWrapped("%s", e.message.c_str());

      if (e.count > 1) {
        ImGui::TextDisabled("Repeated %d times (collapsed).", e.count);
      }

      ImGui::Separator();

      if (ImGui::Button(e.unread ? "Mark read" : "Mark unread")) {
        e.unread = !e.unread;
      }
      ImGui::SameLine();
      if (ImGui::Button(e.pinned ? "Unpin" : "Pin")) {
        e.pinned = !e.pinned;
      }
      ImGui::SameLine();
      if (ImGui::Button("Copy")) {
        ImGui::SetClipboardText(e.message.c_str());
      }
      ImGui::SameLine();
      if (ImGui::Button("Delete")) {
        const std::uint64_t del_id = e.id;
        ui.notifications.erase(ui.notifications.begin() + static_cast<long>(*idx_opt));
        if (s.selected_id == del_id) s.selected_id = 0;
        ImGui::EndChild();
        ImGui::EndTable();
        ImGui::End();
        return;
      }

      ImGui::SeparatorText("Actions");

      if (ImGui::Button("Promote to Journal")) {
        promote_to_journal(sim, ui, e, selected_ship, selected_colony, selected_body);
        e.unread = false;
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Creates a curated Journal entry from this notification and opens Intel Notebook.");
      }

      if (e.source == NotificationSource::SimEvent) {
        if (ImGui::Button("Open Log")) {
          open_log_for_event(ui, e);
        }
        ImGui::SameLine();
        if (ImGui::Button("Open Timeline")) {
          open_timeline_for_event(ui, e);
        }
        if (ImGui::Button("Focus context")) {
          focus_context(sim, ui, e, selected_ship, selected_colony, selected_body);
        }
      } else {
        if (ImGui::Button("Open Watchboard")) {
          open_watchboard_for_alert(ui, e);
        }
        ImGui::SameLine();
        if (ImGui::Button("Inspect JSON")) {
          open_json_explorer_for_alert(ui, e);
        }
      }

      if (e.source == NotificationSource::WatchboardAlert) {
        ImGui::SeparatorText("Watchboard");
        if (!e.watch_label.empty()) {
          ImGui::TextDisabled("Label: %s", e.watch_label.c_str());
        }
        if (!e.watch_path.empty()) {
          ImGui::TextDisabled("Path: %s", e.watch_path.c_str());
        }
        if (!e.watch_rep_ptr.empty()) {
          ImGui::TextDisabled("Resolved: %s", e.watch_rep_ptr.c_str());
          if (ImGui::Button("Copy resolved pointer")) {
            ImGui::SetClipboardText(e.watch_rep_ptr.c_str());
          }
        }
      }

      if (e.source == NotificationSource::SimEvent) {
        ImGui::SeparatorText("Context");
        if (e.system_id != kInvalidId) {
          ImGui::TextDisabled("System id: %llu", (unsigned long long)e.system_id);
        }
        if (e.ship_id != kInvalidId) {
          ImGui::TextDisabled("Ship id: %llu", (unsigned long long)e.ship_id);
        }
        if (e.colony_id != kInvalidId) {
          ImGui::TextDisabled("Colony id: %llu", (unsigned long long)e.colony_id);
        }
        if (e.faction_id != kInvalidId) {
          ImGui::TextDisabled("Faction id: %llu", (unsigned long long)e.faction_id);
        }
        if (e.faction_id2 != kInvalidId) {
          ImGui::TextDisabled("Faction2 id: %llu", (unsigned long long)e.faction_id2);
        }
      }

      ImGui::EndChild();
    }

    ImGui::EndTable();
  }

  ImGui::End();
}

} // namespace nebula4x::ui
