#include "ui/notifications.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

#include <imgui.h>

#include "nebula4x/core/entities.h"
#include "nebula4x/core/simulation.h"

namespace nebula4x::ui {

namespace {

// Compare for duplicate-collapse...
bool same_key(const NotificationEntry& a, const NotificationEntry& b) {
  return a.source == b.source && a.level == b.level && a.category == b.category && a.system_id == b.system_id &&
         a.ship_id == b.ship_id && a.colony_id == b.colony_id && a.faction_id == b.faction_id &&
         a.faction_id2 == b.faction_id2 && a.watch_id == b.watch_id && a.message == b.message;
}

void clamp_retention(UIState& ui) {
  ui.notifications_max_entries = std::clamp(ui.notifications_max_entries, 0, 5000);
  ui.notifications_keep_days = std::clamp(ui.notifications_keep_days, 0, 36500);
}

void prune_by_size(UIState& ui) {
  clamp_retention(ui);
  const std::size_t max_n = static_cast<std::size_t>(ui.notifications_max_entries);
  if (max_n == 0) {
    // If the user explicitly sets max to 0, we keep only pinned items.
    ui.notifications.erase(
        std::remove_if(ui.notifications.begin(), ui.notifications.end(), [](const NotificationEntry& e) {
          return !e.pinned;
        }),
        ui.notifications.end());
    return;
  }

  // Remove oldest non-pinned entries until we're under the cap.
  while (ui.notifications.size() > max_n) {
    auto it = std::find_if(ui.notifications.begin(), ui.notifications.end(), [](const NotificationEntry& e) {
      return !e.pinned;
    });
    if (it == ui.notifications.end()) break;
    ui.notifications.erase(it);
  }
}

void prune_by_age(const Simulation& sim, UIState& ui) {
  clamp_retention(ui);
  if (ui.notifications_keep_days <= 0) return;
  const auto& st = sim.state();
  const std::int64_t now_day = st.date.days_since_epoch();
  const std::int64_t min_day = now_day - static_cast<std::int64_t>(ui.notifications_keep_days);
  ui.notifications.erase(
      std::remove_if(ui.notifications.begin(), ui.notifications.end(), [&](const NotificationEntry& e) {
        return !e.pinned && e.day < min_day;
      }),
      ui.notifications.end());
}

void prune_all(const Simulation& sim, UIState& ui) {
  prune_by_age(sim, ui);
  prune_by_size(ui);
}

void push_or_collapse(UIState& ui, NotificationEntry&& e) {
  const double now = ImGui::GetTime();
  e.created_time_s = now;
  e.updated_time_s = now;

  if (ui.notifications_collapse_duplicates && !ui.notifications.empty()) {
    // Scan a small window from the back to collapse spammy repeats without
    // turning this into an O(N) per notification operation.
    const int kScan = 24;
    const int n = static_cast<int>(ui.notifications.size());
    for (int i = n - 1, scanned = 0; i >= 0 && scanned < kScan; --i, ++scanned) {
      NotificationEntry& prev = ui.notifications[static_cast<std::size_t>(i)];
      if (!prev.pinned && same_key(prev, e)) {
        prev.count += std::max(1, e.count);
        prev.unread = true;
        prev.day = e.day;
        prev.hour = e.hour;
        prev.updated_time_s = now;
        ui.notifications_request_focus_id = prev.id;
        return;
      }
    }
  }

  ui.notifications.push_back(std::move(e));
  ui.notifications_request_focus_id = ui.notifications.back().id;
}

} // namespace

void notifications_reset(UIState& ui) {
  ui.notifications.clear();
  ui.notifications_last_ingested_event_seq = 0;
  ui.notifications_request_focus_id = 0;
}

void notifications_ingest_sim_events(const Simulation& sim, UIState& ui) {
  if (!ui.notifications_capture_sim_events) {
    // Even if capture is off, keep the ingestion cursor up to date to avoid
    // importing a huge backlog if the user toggles capture later.
    const auto& events = sim.state().events;
    if (!events.empty()) {
      ui.notifications_last_ingested_event_seq = std::max(ui.notifications_last_ingested_event_seq, events.back().seq);
    }
    return;
  }

  const auto& st = sim.state();
  const auto& events = st.events;
  if (events.empty()) {
    ui.notifications_last_ingested_event_seq = 0;
    return;
  }

  // Defensive: if the sim was reloaded and the event sequence moved backwards
  // (shouldn't happen, but saves could prune), resync.
  const std::uint64_t newest_seq = events.back().seq;
  if (ui.notifications_last_ingested_event_seq > newest_seq) {
    ui.notifications_last_ingested_event_seq = 0;
  }

  // If this is the first time ingesting after a state load, start from the
  // newest event (no backfill). The inbox is primarily for *new* events.
  if (ui.notifications_last_ingested_event_seq == 0) {
    ui.notifications_last_ingested_event_seq = newest_seq;
    prune_all(sim, ui);
    return;
  }

  // Collect new events (seq > cursor) preserving chronological order.
  // We iterate forward because event vectors are already in order.
  bool saw_error = false;
  for (const auto& ev : events) {
    if (ev.seq <= ui.notifications_last_ingested_event_seq) continue;

    const int lvl = static_cast<int>(ev.level);
    if (lvl == static_cast<int>(EventLevel::Info) && !ui.notifications_capture_info_events) {
      continue;
    }

    NotificationEntry e;
    e.id = ev.seq;
    e.source = NotificationSource::SimEvent;
    e.unread = true;
    e.pinned = false;
    e.count = 1;
    e.day = ev.day;
    e.hour = ev.hour;
    e.level = lvl;
    e.category = static_cast<int>(ev.category);
    e.system_id = ev.system_id;
    e.ship_id = ev.ship_id;
    e.colony_id = ev.colony_id;
    e.faction_id = ev.faction_id;
    e.faction_id2 = ev.faction_id2;
    e.message = ev.message;

    push_or_collapse(ui, std::move(e));
    if (lvl == static_cast<int>(EventLevel::Error)) saw_error = true;
  }

  ui.notifications_last_ingested_event_seq = newest_seq;

  if (saw_error && ui.notifications_auto_open_on_error) {
    ui.show_notifications_window = true;
  }

  prune_all(sim, ui);
}

void notifications_push_watchboard_alert(UIState& ui, std::uint64_t id, std::int64_t day, int hour, int level,
                                         const std::string& message, std::uint64_t watch_id,
                                         const std::string& watch_label, const std::string& watch_path,
                                         const std::string& watch_rep_ptr) {
  if (!ui.notifications_capture_watchboard_alerts) return;

  NotificationEntry e;
  e.id = id;
  e.source = NotificationSource::WatchboardAlert;
  e.unread = true;
  e.pinned = false;
  e.count = 1;
  e.day = day;
  e.hour = hour;
  e.level = level;
  e.category = static_cast<int>(EventCategory::General);

  e.watch_id = watch_id;
  e.watch_label = watch_label;
  e.watch_path = watch_path;
  e.watch_rep_ptr = watch_rep_ptr;
  e.message = message;

  push_or_collapse(ui, std::move(e));

  // Mirror the "stop and look" behavior for error-level alerts.
  if (ui.notifications_auto_open_on_error && level >= static_cast<int>(EventLevel::Error)) {
    ui.show_notifications_window = true;
    ui.notifications_request_focus_id = id;
  }
}

int notifications_unread_count(const UIState& ui) {
  int n = 0;
  for (const auto& e : ui.notifications) {
    if (e.unread) ++n;
  }
  return n;
}

void notifications_mark_all_read(UIState& ui) {
  for (auto& e : ui.notifications) {
    e.unread = false;
  }
}

void notifications_clear_read(UIState& ui) {
  ui.notifications.erase(
      std::remove_if(ui.notifications.begin(), ui.notifications.end(), [](const NotificationEntry& e) {
        return !e.unread && !e.pinned;
      }),
      ui.notifications.end());
}

void notifications_clear(UIState& ui, bool keep_pinned) {
  if (!keep_pinned) {
    ui.notifications.clear();
    return;
  }

  ui.notifications.erase(
      std::remove_if(ui.notifications.begin(), ui.notifications.end(), [](const NotificationEntry& e) {
        return !e.pinned;
      }),
      ui.notifications.end());
}

} // namespace nebula4x::ui
