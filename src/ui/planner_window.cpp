#include "ui/planner_window.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "imgui.h"

#include "ui/time_warp_window.h"

#include "nebula4x/core/game_state.h"
#include "nebula4x/core/planner_events.h"
#include "nebula4x/util/strings.h"
#include "nebula4x/util/time.h"

namespace nebula4x::ui {

namespace {

const char* level_label(EventLevel lvl) {
  switch (lvl) {
    case EventLevel::Info: return "Info";
    case EventLevel::Warn: return "Warn";
    case EventLevel::Error: return "Error";
  }
  return "";
}

const char* category_label(EventCategory cat) {
  switch (cat) {
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
  return "";
}

// Compact D+ style label. Examples:
//   0.0  -> D+0h
//   0.5  -> D+12h
//   1.0  -> D+1d
//   2.25 -> D+2d 6h
std::string eta_label(double eta_days) {
  if (!std::isfinite(eta_days) || eta_days < 0.0) eta_days = 0.0;
  int d = static_cast<int>(std::floor(eta_days + 1e-9));
  int h = static_cast<int>(std::round((eta_days - static_cast<double>(d)) * 24.0));
  if (h >= 24) {
    d += 1;
    h = 0;
  }
  if (h < 0) h = 0;

  std::string out;
  if (d <= 0) {
    out = "D+" + std::to_string(std::max(0, h)) + "h";
  } else {
    out = "D+" + std::to_string(d) + "d";
    if (h > 0) out += " " + std::to_string(h) + "h";
  }
  return out;
}


int hours_until(const Simulation& sim, std::int64_t target_day, int target_hour) {
  const auto& st = sim.state();
  const std::int64_t now_day = st.date.days_since_epoch();
  const int now_hour = nebula4x::clamp_hour(st.hour_of_day);
  const int th = nebula4x::clamp_hour(target_hour);

  const std::int64_t dh =
      (target_day - now_day) * 24 + static_cast<std::int64_t>(th) - static_cast<std::int64_t>(now_hour);

  if (dh <= 0) return 0;
  if (dh > static_cast<std::int64_t>(std::numeric_limits<int>::max())) return std::numeric_limits<int>::max();
  return static_cast<int>(dh);
}

int pick_step_hours(int total_hours) {
  // Pick a step size that keeps the UI responsive but doesn't take forever.
  if (total_hours <= 48) return 1;
  if (total_hours <= 24 * 14) return 6;
  if (total_hours <= 24 * 90) return 12;
  return 24;
}

std::string extract_research_term(const PlannerEvent& ev) {
  const std::string prefix = "Research complete: ";
  if (ev.title.rfind(prefix, 0) == 0) return ev.title.substr(prefix.size());
  return ev.title;
}

void start_warp_to_time(const PlannerEvent& ev, Simulation& sim, UIState& ui) {
  const int h = hours_until(sim, ev.day, ev.hour);
  if (h <= 0) return;

  TimeWarpQuickStart req;
  req.total_hours = h;
  req.step_hours = pick_step_hours(h);
  req.chunk_hours_per_frame = 24;

  // Safe default: interrupt on any WARN/ERROR, but don't stop on INFO.
  req.stop.stop_on_info = false;
  req.stop.stop_on_warn = true;
  req.stop.stop_on_error = true;
  req.stop.filter_category = false;
  req.stop.category = EventCategory::General;
  req.stop.faction_id = kInvalidId;
  req.stop.system_id = kInvalidId;
  req.stop.ship_id = kInvalidId;
  req.stop.colony_id = kInvalidId;
  req.stop.message_contains.clear();

  req.stop_at_time_limit_is_success = true;
  req.open_timeline_on_hit = true;
  req.focus_context_on_hit = true;

  req.target_label = ev.title;
  req.target_day = ev.day;
  req.target_hour = ev.hour;
  req.has_target_time = true;

  time_warp_quick_start(req, ui);
}

void start_warp_until_event(const PlannerEvent& ev, Simulation& sim, UIState& ui) {
  const int h = hours_until(sim, ev.day, ev.hour);
  const int budget = std::max(24, h + 24); // 1 day grace past the forecast

  TimeWarpQuickStart req;
  req.total_hours = budget;
  req.step_hours = pick_step_hours(budget);
  req.chunk_hours_per_frame = 24;

  // More precise: stop on an INFO that matches this event's category/context.
  // NOTE: Filters apply to WARN/ERROR too, so this may ignore unrelated problems elsewhere.
  req.stop.stop_on_info = true;
  req.stop.stop_on_warn = true;
  req.stop.stop_on_error = true;
  req.stop.filter_category = true;
  req.stop.category = ev.category;
  req.stop.faction_id = ev.faction_id;
  req.stop.system_id = ev.system_id;
  req.stop.ship_id = ev.ship_id;
  req.stop.colony_id = ev.colony_id;

  if (ev.category == EventCategory::Research) {
    req.stop.message_contains = extract_research_term(ev);
  } else {
    req.stop.message_contains.clear();
  }

  req.stop_at_time_limit_is_success = false;
  req.open_timeline_on_hit = true;
  req.focus_context_on_hit = true;

  req.target_label = ev.title;
  req.target_day = ev.day;
  req.target_hour = ev.hour;
  req.has_target_time = true;

  time_warp_quick_start(req, ui);
}


void jump_to_planner_event(const PlannerEvent& ev,
                           Simulation& sim,
                           UIState& ui,
                           Id& selected_ship,
                           Id& selected_colony,
                           Id& selected_body) {
  // Map focus.
  if (ev.system_id != kInvalidId) {
    sim.state_mut().selected_system = ev.system_id;
    ui.request_map_tab = MapTab::System;

    // Best-effort map centering on the referenced entity.
    if (ev.ship_id != kInvalidId) {
      if (const auto* ship = find_ptr(sim.state().ships, ev.ship_id)) {
        ui.request_system_map_center = true;
        ui.request_system_map_center_system_id = ship->system_id;
        ui.request_system_map_center_x_mkm = ship->x_mkm;
        ui.request_system_map_center_y_mkm = ship->y_mkm;
      }
    } else if (ev.colony_id != kInvalidId) {
      if (const auto* colony = find_ptr(sim.state().colonies, ev.colony_id)) {
        if (const auto* body = find_ptr(sim.state().bodies, colony->body_id)) {
          ui.request_system_map_center = true;
          ui.request_system_map_center_system_id = body->system_id;
          ui.request_system_map_center_x_mkm = body->x_mkm;
          ui.request_system_map_center_y_mkm = body->y_mkm;
        }
      }
    }
  }

  // Details focus.
  if (ev.ship_id != kInvalidId) {
    selected_ship = ev.ship_id;
    selected_colony = kInvalidId;
    selected_body = kInvalidId;

    if (const auto* ship = find_ptr(sim.state().ships, ev.ship_id)) {
      ui.selected_fleet_id = ship->fleet_id;
    }

    ui.show_details_window = true;
    ui.request_details_tab = DetailsTab::Ship;
    return;
  }

  if (ev.colony_id != kInvalidId) {
    selected_colony = ev.colony_id;
    selected_ship = kInvalidId;

    if (const auto* colony = find_ptr(sim.state().colonies, ev.colony_id)) {
      selected_body = colony->body_id;
    }

    ui.show_details_window = true;
    ui.request_details_tab = DetailsTab::Colony;
    return;
  }

  if (ev.category == EventCategory::Research) {
    ui.show_details_window = true;
    ui.request_details_tab = DetailsTab::Research;
    ui.request_focus_faction_id = ev.faction_id;
    return;
  }
}

bool same_options(const PlannerEventsOptions& a, const PlannerEventsOptions& b) {
  return a.max_days == b.max_days && a.max_items == b.max_items && a.include_research == b.include_research &&
         a.include_colonies == b.include_colonies && a.include_ground_battles == b.include_ground_battles &&
         a.include_ships == b.include_ships &&
         a.include_ship_next_step == b.include_ship_next_step &&
         a.include_ship_queue_complete == b.include_ship_queue_complete && a.max_ships == b.max_ships &&
         a.max_orders_per_ship == b.max_orders_per_ship;
}

} // namespace

void draw_planner_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  if (!ui.show_planner_window) return;

  static Id faction_id = kInvalidId;
  static PlannerEventsOptions opt;
  static PlannerEventsOptions last_opt;
  static PlannerEventsResult last;
  static bool have_last = false;
  static Id last_faction_id = kInvalidId;
  static std::int64_t last_compute_day = -1;
  static int last_compute_hour = -1;
  static bool auto_refresh = true;

  static char search_buf[128] = "";
  static int category_filter = 0; // 0=All
  static int level_filter = 0;    // 0=All, 1=Info, 2=Warn, 3=Error

  // Choose a reasonable default faction the first time this window opens.
  if (faction_id == kInvalidId) {
    faction_id = ui.viewer_faction_id;
    if (selected_ship != kInvalidId) {
      if (const auto* ship = find_ptr(sim.state().ships, selected_ship)) {
        faction_id = ship->faction_id;
      }
    } else if (!sim.state().factions.empty()) {
      // Fallback: first faction id.
      Id best = sim.state().factions.begin()->first;
      for (const auto& [fid, _] : sim.state().factions) best = std::min(best, fid);
      faction_id = best;
    }
  }

  ImGui::SetNextWindowSize(ImVec2(920, 620), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Planner", &ui.show_planner_window)) {
    ImGui::End();
    return;
  }

  ImGui::TextUnformatted("Forecast dashboard (best-effort). Merges research + colony production + optional ship order ETAs.");
  ImGui::TextUnformatted("Use this to spot upcoming completions and stalls. Estimates can be wrong when conditions change.");

  // --- Faction selector ---
  {
    std::vector<Id> fids;
    fids.reserve(sim.state().factions.size());
    for (const auto& [fid, _] : sim.state().factions) fids.push_back(fid);
    std::sort(fids.begin(), fids.end());

    std::string cur_label = "(none)";
    if (const auto* fac = find_ptr(sim.state().factions, faction_id)) {
      cur_label = fac->name.empty() ? ("Faction " + std::to_string(faction_id)) : fac->name;
      cur_label += " (#" + std::to_string(faction_id) + ")";
    }

    if (ImGui::BeginCombo("Faction", cur_label.c_str())) {
      for (Id fid : fids) {
        const auto* fac = find_ptr(sim.state().factions, fid);
        std::string label = fac ? (fac->name.empty() ? ("Faction " + std::to_string(fid)) : fac->name) : std::to_string(fid);
        label += " (#" + std::to_string(fid) + ")";
        const bool selected = (fid == faction_id);
        if (ImGui::Selectable(label.c_str(), selected)) {
          faction_id = fid;
        }
        if (selected) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
  }

  // --- Options ---
  ImGui::Separator();

  ImGui::Checkbox("Auto-refresh on time advance", &auto_refresh);
  ImGui::SameLine();
  if (ImGui::Button("Refresh now")) {
    last = compute_planner_events(sim, faction_id, opt);
    have_last = true;
    last_opt = opt;
    last_faction_id = faction_id;
    last_compute_day = sim.state().date.days_since_epoch();
    last_compute_hour = sim.state().hour_of_day;
  }

  ImGui::SameLine();
  ImGui::TextDisabled("(Horizon+limits are safety guards; ship ETAs can be expensive.)");

  ImGui::PushItemWidth(120);
  ImGui::InputInt("Max days", &opt.max_days);
  ImGui::SameLine();
  ImGui::InputInt("Max items", &opt.max_items);
  ImGui::PopItemWidth();

  ImGui::Checkbox("Include research", &opt.include_research);
  ImGui::SameLine();
  ImGui::Checkbox("Include colonies", &opt.include_colonies);
  ImGui::SameLine();
  ImGui::Checkbox("Include ground battles", &opt.include_ground_battles);
  ImGui::SameLine();
  ImGui::Checkbox("Include ships (expensive)", &opt.include_ships);

  if (opt.include_ships) {
    ImGui::Indent();
    ImGui::Checkbox("Ship: next step", &opt.include_ship_next_step);
    ImGui::SameLine();
    ImGui::Checkbox("Ship: queue complete", &opt.include_ship_queue_complete);
    ImGui::PushItemWidth(120);
    ImGui::InputInt("Max ships", &opt.max_ships);
    ImGui::SameLine();
    ImGui::InputInt("Max orders/ship", &opt.max_orders_per_ship);
    ImGui::PopItemWidth();
    ImGui::Unindent();
  }

  // Auto refresh when the sim time advances (and the user opted in).
  if (auto_refresh) {
    const bool opts_changed = !have_last || !same_options(opt, last_opt);
    const bool faction_changed = !have_last || faction_id != last_faction_id;
    const bool time_changed = !have_last || sim.state().date.days_since_epoch() != last_compute_day ||
                              sim.state().hour_of_day != last_compute_hour;
    if (opts_changed || time_changed || faction_changed) {
      last = compute_planner_events(sim, faction_id, opt);
      have_last = true;
      last_opt = opt;
      last_faction_id = faction_id;
      last_compute_day = sim.state().date.days_since_epoch();
      last_compute_hour = sim.state().hour_of_day;
    }
  }

  // --- Filters ---
  ImGui::Separator();

  ImGui::InputTextWithHint("Search", "title/detail contains...", search_buf, sizeof(search_buf));
  ImGui::SameLine();

  const char* cat_items[] = {
      "All", "General", "Research", "Shipyard", "Construction", "Movement", "Combat", "Intel", "Exploration", "Diplomacy",
  };
  ImGui::SetNextItemWidth(160);
  ImGui::Combo("Category", &category_filter, cat_items, IM_ARRAYSIZE(cat_items));
  ImGui::SameLine();

  const char* lvl_items[] = {"All", "Info", "Warn", "Error"};
  ImGui::SetNextItemWidth(120);
  ImGui::Combo("Level", &level_filter, lvl_items, IM_ARRAYSIZE(lvl_items));

  std::string search_lower = nebula4x::to_lower(std::string(search_buf));

  const EventCategory cat_filter_val =
      (category_filter <= 0) ? EventCategory::General : static_cast<EventCategory>(category_filter - 1);

  if (!have_last) {
    ImGui::TextDisabled("No forecast yet. Click 'Refresh now'.");
    ImGui::End();
    return;
  }

  if (!last.ok) {
    ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Forecast unavailable (unknown faction id?)");
    ImGui::End();
    return;
  }

  if (last.truncated) {
    ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "Truncated: %s", last.truncated_reason.c_str());
  }

  ImGui::TextDisabled("Items: %d", static_cast<int>(last.items.size()));


// Quick action: warp to the soonest visible item (according to current filters).
const PlannerEvent* next_ev = nullptr;
for (const PlannerEvent& ev : last.items) {
  if (category_filter > 0 && ev.category != cat_filter_val) continue;
  if (level_filter > 0) {
    const EventLevel want =
        (level_filter == 1) ? EventLevel::Info : (level_filter == 2 ? EventLevel::Warn : EventLevel::Error);
    if (ev.level != want) continue;
  }

  if (!search_lower.empty()) {
    const std::string t = nebula4x::to_lower(ev.title);
    const std::string d = nebula4x::to_lower(ev.detail);
    if (t.find(search_lower) == std::string::npos && d.find(search_lower) == std::string::npos) continue;
  }

  if (hours_until(sim, ev.day, ev.hour) <= 0) continue;
  if (!next_ev || ev.eta_days < next_ev->eta_days) next_ev = &ev;
}

if (next_ev) {
  ImGui::SameLine();
  if (ImGui::SmallButton("Warp to next")) {
    start_warp_to_time(*next_ev, sim, ui);
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
    ImGui::SetTooltip("Warp to the soonest item currently visible in the table.");
  }

  ImGui::SameLine();
  const std::string next_eta = eta_label(next_ev->eta_days);
  ImGui::TextDisabled("Next: %s (%s)", next_ev->title.c_str(), next_eta.c_str());
}

// --- Table ---
  ImGui::Separator();

  const ImGuiTableFlags flags = ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable;

  if (ImGui::BeginTable("planner_table", 7, flags, ImVec2(0, 0))) {
    ImGui::TableSetupColumn("In", ImGuiTableColumnFlags_WidthFixed, 72);
    ImGui::TableSetupColumn("When", ImGuiTableColumnFlags_WidthFixed, 150);
    ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, 62);
    ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 98);
    ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch, 260);
    ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 128);
    ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch, 420);
    ImGui::TableHeadersRow();

    // Sort handling: allow ImGui sorting UI but always keep a stable deterministic sort
    // as the fallback.
    std::vector<int> order;
    order.reserve(last.items.size());
    for (int i = 0; i < static_cast<int>(last.items.size()); ++i) order.push_back(i);

    if (const ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs()) {
      if (sort && sort->SpecsCount > 0) {
        const ImGuiTableColumnSortSpecs& sp = sort->Specs[0];

        auto cmp_i64 = [](std::int64_t a, std::int64_t b) -> int { return (a < b) ? -1 : (a > b ? 1 : 0); };
        auto cmp_i = [](int a, int b) -> int { return (a < b) ? -1 : (a > b ? 1 : 0); };
        auto cmp_d = [](double a, double b) -> int { return (a < b) ? -1 : (a > b ? 1 : 0); };

        std::stable_sort(order.begin(), order.end(), [&](int ia, int ib) {
          const PlannerEvent& a = last.items[ia];
          const PlannerEvent& b = last.items[ib];

          int c = 0;
          switch (sp.ColumnIndex) {
            case 0: c = cmp_d(a.eta_days, b.eta_days); break;
            case 1:
              c = cmp_i64(a.day, b.day);
              if (c == 0) c = cmp_i(a.hour, b.hour);
              break;
            case 2: c = cmp_i(static_cast<int>(a.level), static_cast<int>(b.level)); break;
            case 3: c = cmp_i(static_cast<int>(a.category), static_cast<int>(b.category)); break;
            case 4: c = nebula4x::to_lower(a.title).compare(nebula4x::to_lower(b.title)); break;
            case 6: c = nebula4x::to_lower(a.detail).compare(nebula4x::to_lower(b.detail)); break;
            default: c = cmp_d(a.eta_days, b.eta_days); break;
          }

          if (c == 0) c = (ia < ib) ? -1 : 1;

          if (sp.SortDirection == ImGuiSortDirection_Ascending) return c < 0;
          return c > 0;
        });
      }
    }

    for (int idx : order) {
      const PlannerEvent& ev = last.items[idx];

      if (category_filter > 0 && ev.category != cat_filter_val) continue;
      if (level_filter > 0) {
        const EventLevel want = (level_filter == 1) ? EventLevel::Info : (level_filter == 2 ? EventLevel::Warn : EventLevel::Error);
        if (ev.level != want) continue;
      }

      if (!search_lower.empty()) {
        std::string hay = nebula4x::to_lower(ev.title + " " + ev.detail);
        if (hay.find(search_lower) == std::string::npos) continue;
      }

      ImGui::TableNextRow();
      ImGui::PushID(idx);

      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(eta_label(ev.eta_days).c_str());

      ImGui::TableSetColumnIndex(1);
      const std::string when = nebula4x::format_datetime(ev.day, ev.hour);
      ImGui::TextUnformatted(when.c_str());

      ImGui::TableSetColumnIndex(2);
      if (ev.level == EventLevel::Warn) {
        ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "%s", level_label(ev.level));
      } else if (ev.level == EventLevel::Error) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", level_label(ev.level));
      } else {
        ImGui::TextUnformatted(level_label(ev.level));
      }

      ImGui::TableSetColumnIndex(3);
      ImGui::TextUnformatted(category_label(ev.category));

      ImGui::TableSetColumnIndex(4);
      // Make the item clickable to jump.
      if (ImGui::Selectable(ev.title.c_str(), false)) {
        jump_to_planner_event(ev, sim, ui, selected_ship, selected_colony, selected_body);
      }

      ImGui::TableSetColumnIndex(5);
      const int h_to_target = hours_until(sim, ev.day, ev.hour);
      const bool can_warp = h_to_target > 0;

      if (!can_warp) ImGui::BeginDisabled();
      if (ImGui::SmallButton("Warp")) {
        start_warp_to_time(ev, sim, ui);
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Warp to the forecast time.\nInterrupts on any WARN/ERROR event.");
      }
      if (!can_warp) ImGui::EndDisabled();

      ImGui::SameLine();

      const bool until_supported = (ev.category != EventCategory::General && ev.category != EventCategory::Movement);
      const bool has_context =
          (ev.faction_id != kInvalidId) || (ev.system_id != kInvalidId) || (ev.ship_id != kInvalidId) || (ev.colony_id != kInvalidId);
      const bool can_until = can_warp && until_supported && has_context;

      if (!can_until) ImGui::BeginDisabled();
      if (ImGui::SmallButton("Until")) {
        start_warp_until_event(ev, sim, ui);
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip(
            "Warp until a matching INFO event happens (category/context-scoped).\n"
            "Note: filters apply to WARN/ERROR too, so unrelated problems elsewhere may be ignored.");
      }
      if (!can_until) ImGui::EndDisabled();

      ImGui::TableSetColumnIndex(6);
      ImGui::TextWrapped("%s", ev.detail.c_str());

      ImGui::PopID();
    }

    ImGui::EndTable();
  }

  ImGui::End();
}

} // namespace nebula4x::ui
