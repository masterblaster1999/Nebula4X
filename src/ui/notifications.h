#pragma once

#include <cstdint>
#include <string>

#include "nebula4x/core/ids.h"

#include "ui/ui_state.h"

namespace nebula4x {
class Simulation;
}

namespace nebula4x::ui {

// Reset all inbox state (used when a new simulation state is loaded).
void notifications_reset(UIState& ui);

// Ingest newly appended SimEvents into the UI inbox.
//
// This is designed to be called once per frame; it only processes events with
// seq > ui.notifications_last_ingested_event_seq.
void notifications_ingest_sim_events(const nebula4x::Simulation& sim, UIState& ui);

// Push a watchboard alert into the inbox.
//
// This is typically called from watchboard_alerts.cpp when an alert fires.
void notifications_push_watchboard_alert(UIState& ui, std::uint64_t id, std::int64_t day, int hour, int level,
                                         const std::string& message, std::uint64_t watch_id,
                                         const std::string& watch_label, const std::string& watch_path,
                                         const std::string& watch_rep_ptr);

// Convenience helper for status bar badges.
int notifications_unread_count(const UIState& ui);

// Bulk operations.
void notifications_mark_all_read(UIState& ui);
void notifications_clear_read(UIState& ui);
void notifications_clear(UIState& ui, bool keep_pinned);

} // namespace nebula4x::ui
