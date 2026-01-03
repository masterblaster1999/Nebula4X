#include "ui/time_warp_window.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nebula4x/util/log.h"
#include "nebula4x/util/time.h"

namespace nebula4x::ui {
namespace {

const char* event_level_label(EventLevel l) {
  switch (l) {
    case EventLevel::Info: return "Info";
    case EventLevel::Warn: return "Warn";
    case EventLevel::Error: return "Error";
  }
  return "Info";
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
  }
  return "General";
}

constexpr EventCategory kAllCategories[] = {
    EventCategory::General,      EventCategory::Research,     EventCategory::Shipyard,
    EventCategory::Construction, EventCategory::Movement,     EventCategory::Combat,
    EventCategory::Intel,        EventCategory::Exploration,  EventCategory::Diplomacy,
};

constexpr int kStepChoicesHours[] = {1, 6, 12, 24};
constexpr const char* kStepChoicesLabel[] = {"1h", "6h", "12h", "24h"};

static_assert(std::size(kStepChoicesHours) == std::size(kStepChoicesLabel));

std::string trim_copy(std::string_view s) {
  std::size_t a = 0;
  while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  std::size_t b = s.size();
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return std::string(s.substr(a, b - a));
}

struct TimeWarpJob {
  // Configuration
  int max_days{180};
  int step_idx{3};              // 24h
  int chunk_hours_per_frame{24};

  bool stop_on_info{false};
  bool stop_on_warn{true};
  bool stop_on_error{true};

  bool filter_category{false};
  int category_idx{0};

  bool filter_faction{false};
  Id faction_id{kInvalidId};

  bool filter_system{false};
  Id system_id{kInvalidId};

  bool filter_ship{false};
  Id ship_id{kInvalidId};

  bool filter_colony{false};
  Id colony_id{kInvalidId};

  char message_contains[128]{};

  bool open_timeline_on_hit{true};
  bool focus_context_on_hit{true};

  // Optional: treat reaching the time limit as a successful completion.
  // (Used by planner-driven "warp to time" actions.)
  bool stop_at_time_limit_is_success{false};

  // Optional: display-only target time/label (for quick-started warps).
  bool has_target_time{false};
  std::int64_t target_day{0};
  int target_hour{0};
  std::string target_label;


  // Runtime
  bool active{false};
  int remaining_hours{0};
  int total_hours{0};
  int advanced_hours{0};
  std::string status;

  bool hit{false};
  SimEvent hit_event;
};

TimeWarpJob& tw_state() {
  static TimeWarpJob s;
  return s;
}

EventCategory category_from_idx(int idx) {
  idx = std::clamp(idx, 0, static_cast<int>(std::size(kAllCategories)) - 1);
  return kAllCategories[idx];
}

int idx_for_category(EventCategory c) {
  for (int i = 0; i < static_cast<int>(std::size(kAllCategories)); ++i) {
    if (kAllCategories[i] == c) return i;
  }
  return 0;
}

int step_hours_from_idx(int idx) {
  idx = std::clamp(idx, 0, static_cast<int>(std::size(kStepChoicesHours)) - 1);
  return kStepChoicesHours[idx];
}

int idx_for_step_hours(int step_hours) {
  // Pick the nearest available step size.
  int best = 0;
  int best_abs = std::abs(step_hours - kStepChoicesHours[0]);
  for (int i = 1; i < static_cast<int>(std::size(kStepChoicesHours)); ++i) {
    const int d = std::abs(step_hours - kStepChoicesHours[i]);
    if (d < best_abs) {
      best_abs = d;
      best = i;
    }
  }
  return best;
}


std::string fmt_days_hours(int hours) {
  hours = std::max(0, hours);
  const int d = hours / 24;
  const int h = hours % 24;
  if (d > 0) {
    return std::to_string(d) + "d " + std::to_string(h) + "h";
  }
  return std::to_string(h) + "h";
}

void apply_preset(const char* name, TimeWarpJob& tw) {
  const std::string n(name ? name : "");

  // Defaults.
  tw.stop_on_info = false;
  tw.stop_on_warn = true;
  tw.stop_on_error = true;
  tw.filter_category = false;
  tw.category_idx = idx_for_category(EventCategory::General);
  tw.message_contains[0] = '\0';

  if (n == "Research") {
    tw.stop_on_info = true;
    tw.stop_on_warn = true;
    tw.stop_on_error = true;
    tw.filter_category = true;
    tw.category_idx = idx_for_category(EventCategory::Research);
  } else if (n == "Shipyard") {
    tw.stop_on_info = true;
    tw.stop_on_warn = true;
    tw.stop_on_error = true;
    tw.filter_category = true;
    tw.category_idx = idx_for_category(EventCategory::Shipyard);
  } else if (n == "Construction") {
    tw.stop_on_info = true;
    tw.stop_on_warn = true;
    tw.stop_on_error = true;
    tw.filter_category = true;
    tw.category_idx = idx_for_category(EventCategory::Construction);
  } else if (n == "Movement") {
    tw.stop_on_info = true;
    tw.stop_on_warn = true;
    tw.stop_on_error = true;
    tw.filter_category = true;
    tw.category_idx = idx_for_category(EventCategory::Movement);
  } else if (n == "Combat") {
    tw.stop_on_info = true;
    tw.stop_on_warn = true;
    tw.stop_on_error = true;
    tw.filter_category = true;
    tw.category_idx = idx_for_category(EventCategory::Combat);
  } else if (n == "Exploration") {
    tw.stop_on_info = true;
    tw.stop_on_warn = true;
    tw.stop_on_error = true;
    tw.filter_category = true;
    tw.category_idx = idx_for_category(EventCategory::Exploration);
  } else if (n == "Intel") {
    tw.stop_on_info = true;
    tw.stop_on_warn = true;
    tw.stop_on_error = true;
    tw.filter_category = true;
    tw.category_idx = idx_for_category(EventCategory::Intel);
  } else if (n == "Diplomacy") {
    tw.stop_on_info = true;
    tw.stop_on_warn = true;
    tw.stop_on_error = true;
    tw.filter_category = true;
    tw.category_idx = idx_for_category(EventCategory::Diplomacy);
  }
}

void focus_context_from_event(const SimEvent& ev, Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony,
                              Id& selected_body) {
  auto& s = sim.state();

  // Faction focus (details tabs can choose to consume this).
  if (ev.faction_id != kInvalidId) ui.request_focus_faction_id = ev.faction_id;

  // Prefer ship context when available.
  if (ev.ship_id != kInvalidId) {
    selected_ship = ev.ship_id;
    selected_colony = kInvalidId;
    selected_body = kInvalidId;

    if (const auto* sh = find_ptr(s.ships, ev.ship_id)) {
      s.selected_system = sh->system_id;
      ui.show_map_window = true;
      ui.request_map_tab = MapTab::System;
      ui.show_details_window = true;
      ui.request_details_tab = DetailsTab::Ship;
    }
    return;
  }

  if (ev.colony_id != kInvalidId) {
    selected_colony = ev.colony_id;
    selected_ship = kInvalidId;

    if (const auto* c = find_ptr(s.colonies, ev.colony_id)) {
      selected_body = c->body_id;
      if (const auto* b = find_ptr(s.bodies, c->body_id)) {
        s.selected_system = b->system_id;
      }

      ui.show_details_window = true;
      ui.request_details_tab = DetailsTab::Colony;
      ui.show_map_window = true;
      ui.request_map_tab = MapTab::System;
    }
    return;
  }

  if (ev.system_id != kInvalidId) {
    s.selected_system = ev.system_id;
    ui.show_map_window = true;
    ui.request_map_tab = MapTab::System;
  }
}

} // namespace


void time_warp_quick_start(const TimeWarpQuickStart& req, UIState& ui) {
  auto& tw = tw_state();

  // Cancel/clear any existing job and apply the requested configuration.
  tw.active = false;
  tw.hit = false;
  tw.hit_event = SimEvent{};
  tw.status.clear();
  tw.advanced_hours = 0;

  // Stop criteria.
  tw.stop_on_info = req.stop.stop_on_info;
  tw.stop_on_warn = req.stop.stop_on_warn;
  tw.stop_on_error = req.stop.stop_on_error;

  tw.filter_category = req.stop.filter_category;
  tw.category_idx = idx_for_category(req.stop.category);

  tw.filter_faction = (req.stop.faction_id != kInvalidId);
  tw.faction_id = req.stop.faction_id;

  tw.filter_system = (req.stop.system_id != kInvalidId);
  tw.system_id = req.stop.system_id;

  tw.filter_ship = (req.stop.ship_id != kInvalidId);
  tw.ship_id = req.stop.ship_id;

  tw.filter_colony = (req.stop.colony_id != kInvalidId);
  tw.colony_id = req.stop.colony_id;

  {
    std::string mc = req.stop.message_contains;
    if (mc.size() >= sizeof(tw.message_contains)) mc.resize(sizeof(tw.message_contains) - 1);
    std::memset(tw.message_contains, 0, sizeof(tw.message_contains));
    std::memcpy(tw.message_contains, mc.c_str(), mc.size());
  }

  // Run parameters.
  tw.step_idx = idx_for_step_hours(req.step_hours);
  tw.chunk_hours_per_frame = std::clamp(req.chunk_hours_per_frame, 1, 24 * 30);

  tw.open_timeline_on_hit = req.open_timeline_on_hit;
  tw.focus_context_on_hit = req.focus_context_on_hit;

  // UI/goal info.
  tw.stop_at_time_limit_is_success = req.stop_at_time_limit_is_success;
  tw.has_target_time = req.has_target_time;
  tw.target_day = req.target_day;
  tw.target_hour = req.target_hour;
  tw.target_label = req.target_label;

  // Convert the requested budget into the job runtime fields.
  tw.total_hours = std::max(0, req.total_hours);
  tw.remaining_hours = tw.total_hours;

  // Update the UI-facing max_days input to something sensible.
  tw.max_days = std::clamp((tw.total_hours + 23) / 24, 1, 36500);

  // Always open the window so the user can see/cancel the job.
  ui.show_time_warp_window = true;

  if (tw.total_hours <= 0) {
    tw.active = false;
    tw.status = "Nothing to do (0h budget).";
    return;
  }

  tw.status = "Warping...";
  tw.active = true;
}

void draw_time_warp_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  if (!ui.show_time_warp_window) return;

  auto& tw = tw_state();

  ImGui::SetNextWindowSize(ImVec2(520, 420), ImGuiCond_FirstUseEver);

  bool open = ui.show_time_warp_window;
  if (!ImGui::Begin("Time Warp", &open)) {
    ImGui::End();
    ui.show_time_warp_window = open;
    return;
  }
  ui.show_time_warp_window = open;

// --- Run job (incrementally) ---
if (tw.active) {
  const auto reached_time_limit_status = [&]() -> std::string {
    std::string msg = "Reached time limit.";
    if (tw.has_target_time) {
      msg = "Reached " + nebula4x::format_datetime(tw.target_day, tw.target_hour) + ".";
      if (!tw.target_label.empty()) {
        msg = "Reached " + nebula4x::format_datetime(tw.target_day, tw.target_hour) + " (" + tw.target_label + ").";
      }
    } else if (!tw.target_label.empty()) {
      msg = "Reached target (" + tw.target_label + ").";
    }
    return msg;
  };

  const int budget = std::min(std::max(0, tw.chunk_hours_per_frame), std::max(0, tw.remaining_hours));
  if (budget > 0) {
    EventStopCondition stop;
    stop.stop_on_info = tw.stop_on_info;
    stop.stop_on_warn = tw.stop_on_warn;
    stop.stop_on_error = tw.stop_on_error;

    stop.filter_category = tw.filter_category;
    stop.category = category_from_idx(tw.category_idx);

    if (tw.filter_faction) stop.faction_id = tw.faction_id;
    if (tw.filter_system) stop.system_id = tw.system_id;
    if (tw.filter_ship) stop.ship_id = tw.ship_id;
    if (tw.filter_colony) stop.colony_id = tw.colony_id;

    stop.message_contains = trim_copy(tw.message_contains);

    const int step_hours = step_hours_from_idx(tw.step_idx);

    const AdvanceUntilEventResult res = sim.advance_until_event_hours(budget, stop, step_hours);

    tw.advanced_hours += std::max(0, res.hours_advanced);
    tw.remaining_hours -= std::max(0, res.hours_advanced);

    if (!res.hit && res.hours_advanced <= 0) {
      tw.active = false;
      tw.status = "Time warp stalled (no progress).";
    } else if (res.hit) {
      tw.active = false;
      tw.hit = true;
      tw.hit_event = res.event;
      tw.status = std::string("Hit ") + event_level_label(res.event.level) + "/" +
                  event_category_label(res.event.category) + " event.";

      if (tw.open_timeline_on_hit) {
        ui.show_timeline_window = true;
        ui.request_focus_event_seq = res.event.seq;
      }

      if (tw.focus_context_on_hit) {
        focus_context_from_event(res.event, sim, ui, selected_ship, selected_colony, selected_body);
      }
    } else if (tw.remaining_hours <= 0) {
      tw.active = false;
      if (tw.stop_at_time_limit_is_success) {
        tw.status = reached_time_limit_status();
      } else {
        tw.status = "No matching event within the time limit.";
      }
    }
  } else {
    // No budget remaining.
    tw.active = false;
    if (tw.stop_at_time_limit_is_success) {
      tw.status = reached_time_limit_status();
    } else {
      tw.status = "No matching event within the time limit.";
    }
  }
}

  // --- UI ---

  if (tw.active && (tw.has_target_time || !tw.target_label.empty())) {
    std::string tgt = "Target: ";
    if (!tw.target_label.empty()) tgt += tw.target_label;
    if (tw.has_target_time) {
      if (!tw.target_label.empty()) tgt += " @ ";
      tgt += nebula4x::format_datetime(tw.target_day, tw.target_hour);
    }
    ImGui::TextDisabled("%s", tgt.c_str());
  }

  const bool disable_controls = tw.active;

  if (disable_controls) ImGui::BeginDisabled();

  // Presets row.
  ImGui::TextDisabled("Presets:");
  ImGui::SameLine();
  if (ImGui::SmallButton("WARN/ERROR")) apply_preset("WarnError", tw);
  ImGui::SameLine();
  if (ImGui::SmallButton("Research")) apply_preset("Research", tw);
  ImGui::SameLine();
  if (ImGui::SmallButton("Shipyard")) apply_preset("Shipyard", tw);
  ImGui::SameLine();
  if (ImGui::SmallButton("Construction")) apply_preset("Construction", tw);
  ImGui::SameLine();
  if (ImGui::SmallButton("Movement")) apply_preset("Movement", tw);
  ImGui::SameLine();
  if (ImGui::SmallButton("Combat")) apply_preset("Combat", tw);

  if (disable_controls) ImGui::EndDisabled();

  ImGui::Separator();

  // Stop condition.
  if (disable_controls) ImGui::BeginDisabled();

  ImGui::TextDisabled("Stop when an event matches:");

  ImGui::Checkbox("Info", &tw.stop_on_info);
  ImGui::SameLine();
  ImGui::Checkbox("Warn", &tw.stop_on_warn);
  ImGui::SameLine();
  ImGui::Checkbox("Error", &tw.stop_on_error);

  ImGui::Spacing();

  ImGui::Checkbox("Filter category", &tw.filter_category);
  if (tw.filter_category) {
    ImGui::SameLine();
    const char* preview = event_category_label(category_from_idx(tw.category_idx));
    if (ImGui::BeginCombo("##tw_category", preview)) {
      for (int i = 0; i < static_cast<int>(std::size(kAllCategories)); ++i) {
        const bool sel = (i == tw.category_idx);
        if (ImGui::Selectable(event_category_label(kAllCategories[i]), sel)) tw.category_idx = i;
        if (sel) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
  }

  ImGui::InputTextWithHint("Message contains", "(optional substring)", tw.message_contains,
                           static_cast<int>(std::size(tw.message_contains)));

  ImGui::Spacing();
  ImGui::TextDisabled("Scope filters:");

  // Faction filter: default to viewer faction if set.
  ImGui::Checkbox("Only this faction", &tw.filter_faction);
  if (tw.filter_faction) {
    // Choose a reasonable default.
    Id default_fid = ui.viewer_faction_id;
    if (selected_ship != kInvalidId) {
      if (const auto* sh = find_ptr(sim.state().ships, selected_ship)) default_fid = sh->faction_id;
    }
    if (!tw.active && tw.faction_id == kInvalidId) tw.faction_id = default_fid;

    ImGui::SameLine();
    // Faction chooser (small; number of factions is usually low).
    std::vector<std::pair<Id, std::string>> facs;
    facs.reserve(sim.state().factions.size());
    for (const auto& [id, f] : sim.state().factions) facs.push_back({id, f.name});
    std::sort(facs.begin(), facs.end(), [](const auto& a, const auto& b) { return a.second < b.second; });

    const char* preview = "(none)";
    for (const auto& p : facs) {
      if (p.first == tw.faction_id) {
        preview = p.second.c_str();
        break;
      }
    }

    if (ImGui::BeginCombo("##tw_faction", preview)) {
      for (const auto& p : facs) {
        const bool sel = (p.first == tw.faction_id);
        if (ImGui::Selectable(p.second.c_str(), sel)) tw.faction_id = p.first;
        if (sel) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
  }

  // System filter follows the currently selected system.
  ImGui::Checkbox("Only selected system", &tw.filter_system);
  if (tw.filter_system) {
    if (!tw.active) tw.system_id = sim.state().selected_system;
    if (const auto* sys = find_ptr(sim.state().systems, tw.system_id)) {
      ImGui::SameLine();
      ImGui::TextDisabled("(%s)", sys->name.c_str());
    }
  } else {
    if (!tw.active) tw.system_id = kInvalidId;
  }

  // Ship/colony filters follow the current selection.
  {
    if (selected_ship == kInvalidId) ImGui::BeginDisabled();
    ImGui::Checkbox("Only selected ship", &tw.filter_ship);
    if (selected_ship == kInvalidId) {
      ImGui::EndDisabled();
      if (tw.filter_ship) tw.filter_ship = false;
    }
    if (tw.filter_ship) {
      if (!tw.active) tw.ship_id = selected_ship;
      if (const auto* sh = find_ptr(sim.state().ships, tw.ship_id)) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", sh->name.c_str());
      }
    } else {
      if (!tw.active) tw.ship_id = kInvalidId;
    }
  }

  {
    if (selected_colony == kInvalidId) ImGui::BeginDisabled();
    ImGui::Checkbox("Only selected colony", &tw.filter_colony);
    if (selected_colony == kInvalidId) {
      ImGui::EndDisabled();
      if (tw.filter_colony) tw.filter_colony = false;
    }
    if (tw.filter_colony) {
      if (!tw.active) tw.colony_id = selected_colony;
      if (const auto* c = find_ptr(sim.state().colonies, tw.colony_id)) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", c->name.c_str());
      }
    } else {
      if (!tw.active) tw.colony_id = kInvalidId;
    }
  }

  ImGui::Spacing();
  ImGui::TextDisabled("Run settings:");
  ImGui::InputInt("Max days", &tw.max_days);
  tw.max_days = std::clamp(tw.max_days, 1, 36500);

  // Step size.
  {
    tw.step_idx = std::clamp(tw.step_idx, 0, static_cast<int>(std::size(kStepChoicesHours)) - 1);
    if (ImGui::BeginCombo("Step", kStepChoicesLabel[tw.step_idx])) {
      for (int i = 0; i < static_cast<int>(std::size(kStepChoicesHours)); ++i) {
        const bool sel = (i == tw.step_idx);
        if (ImGui::Selectable(kStepChoicesLabel[i], sel)) tw.step_idx = i;
        if (sel) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
  }

  ImGui::InputInt("Chunk hours/frame", &tw.chunk_hours_per_frame);
  tw.chunk_hours_per_frame = std::clamp(tw.chunk_hours_per_frame, 1, 24 * 30);

  ImGui::Checkbox("Open Timeline on hit", &tw.open_timeline_on_hit);
  ImGui::SameLine();
  ImGui::Checkbox("Focus context on hit", &tw.focus_context_on_hit);

  if (disable_controls) ImGui::EndDisabled();

  ImGui::Separator();

  // Start/cancel/progress.
  const bool any_level = tw.stop_on_info || tw.stop_on_warn || tw.stop_on_error;

  bool can_start = !tw.active && any_level;
  if (tw.filter_faction && tw.faction_id == kInvalidId) can_start = false;
  if (tw.filter_system && sim.state().selected_system == kInvalidId) can_start = false;
  if (tw.filter_ship && selected_ship == kInvalidId) can_start = false;
  if (tw.filter_colony && selected_colony == kInvalidId) can_start = false;

  if (!can_start) ImGui::BeginDisabled();
  if (ImGui::Button("Start Warp")) {
    tw.hit = false;
    tw.status.clear();
    tw.advanced_hours = 0;

    // Manual starts behave like the classic "until event" warp.
    tw.stop_at_time_limit_is_success = false;
    tw.has_target_time = false;
    tw.target_day = 0;
    tw.target_hour = 0;
    tw.target_label.clear();

    // Freeze scope filters at the moment the warp starts.
    if (tw.filter_system) tw.system_id = sim.state().selected_system;
    if (tw.filter_ship) tw.ship_id = selected_ship;
    if (tw.filter_colony) tw.colony_id = selected_colony;
    if (tw.filter_faction && tw.faction_id == kInvalidId) {
      Id default_fid = ui.viewer_faction_id;
      if (selected_ship != kInvalidId) {
        if (const auto* sh = find_ptr(sim.state().ships, selected_ship)) default_fid = sh->faction_id;
      }
      tw.faction_id = default_fid;
    }

    tw.total_hours = std::max(1, tw.max_days) * 24;
    tw.remaining_hours = tw.total_hours;
    tw.active = true;
  }
  if (!can_start) ImGui::EndDisabled();

  ImGui::SameLine();
  if (tw.active) {
    if (ImGui::Button("Cancel")) {
      tw.active = false;
      tw.status = "Canceled.";
    }
  } else {
    if (ImGui::Button("Reset")) {
      tw = TimeWarpJob{};
    }
  }

  ImGui::Spacing();

  if (tw.active) {
    const float frac = (tw.total_hours > 0) ? (float)tw.advanced_hours / (float)tw.total_hours : 0.0f;
    ImGui::ProgressBar(std::clamp(frac, 0.0f, 1.0f), ImVec2(-1, 0),
                       ("Advanced " + fmt_days_hours(tw.advanced_hours) + " / " + fmt_days_hours(tw.total_hours))
                           .c_str());
    ImGui::TextDisabled("Remaining: %s", fmt_days_hours(tw.remaining_hours).c_str());
  } else {
    if (!tw.status.empty()) {
      ImGui::TextWrapped("%s", tw.status.c_str());
    } else {
      ImGui::TextDisabled("Ready.");
    }

    if (tw.hit) {
      const auto& ev = tw.hit_event;
      ImGui::Separator();
      ImGui::TextDisabled("Last hit:");
      ImGui::BulletText("Seq %llu", (unsigned long long)ev.seq);
      ImGui::BulletText("%s / %s", event_level_label(ev.level), event_category_label(ev.category));
      ImGui::BulletText("%s", ev.message.c_str());

      if (ImGui::SmallButton("Focus Timeline")) {
        ui.show_timeline_window = true;
        ui.request_focus_event_seq = ev.seq;
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Focus Context")) {
        focus_context_from_event(ev, sim, ui, selected_ship, selected_colony, selected_body);
      }
    }
  }

  ImGui::End();
}

} // namespace nebula4x::ui
