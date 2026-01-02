#include "ui/timeline_window.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "nebula4x/core/date.h"
#include "nebula4x/util/strings.h"
#include "nebula4x/util/time.h"

namespace nebula4x::ui {
namespace {

constexpr int kLaneCount = 9;

struct LaneInfo {
  EventCategory category{EventCategory::General};
  const char* label{""};
};

constexpr std::array<LaneInfo, kLaneCount> kLanes = {{
    {EventCategory::General, "General"},
    {EventCategory::Research, "Research"},
    {EventCategory::Shipyard, "Shipyard"},
    {EventCategory::Construction, "Construction"},
    {EventCategory::Movement, "Movement"},
    {EventCategory::Combat, "Combat"},
    {EventCategory::Intel, "Intel"},
    {EventCategory::Exploration, "Exploration"},
    {EventCategory::Diplomacy, "Diplomacy"},
}};

int lane_index(EventCategory c) {
  for (int i = 0; i < (int)kLanes.size(); ++i) {
    if (kLanes[(std::size_t)i].category == c) return i;
  }
  return 0;
}

const char* level_label(EventLevel l) {
  switch (l) {
    case EventLevel::Info:
      return "INFO";
    case EventLevel::Warn:
      return "WARN";
    case EventLevel::Error:
      return "ERROR";
  }
  return "?";
}

ImVec4 level_color_rgba(EventLevel l) {
  switch (l) {
    case EventLevel::Info:
      return ImVec4(0.72f, 0.80f, 0.95f, 1.0f);
    case EventLevel::Warn:
      return ImVec4(0.98f, 0.72f, 0.22f, 1.0f);
    case EventLevel::Error:
      return ImVec4(1.00f, 0.28f, 0.24f, 1.0f);
  }
  return ImVec4(1, 1, 1, 1);
}

ImU32 level_color_u32(EventLevel l, float alpha_mul = 1.0f) {
  ImVec4 c = level_color_rgba(l);
  c.w = std::clamp(c.w * alpha_mul, 0.0f, 1.0f);
  return ImGui::ColorConvertFloat4ToU32(c);
}

bool matches_search(const SimEvent& ev, const char* q) {
  if (!q || q[0] == '\0') return true;
  const std::string needle = nebula4x::to_lower(std::string(q));
  if (needle.empty()) return true;
  const std::string hay = nebula4x::to_lower(ev.message);
  return hay.find(needle) != std::string::npos;
}

double nice_step(double raw_days) {
  // Choose from a set of pleasant tick steps, including sub-day ticks.
  //
  // This timeline runs in "days" units (where 1.0 == 24 hours). When zoomed
  // in sufficiently, allow major ticks at 12h/6h/1h boundaries to align with
  // sub-day turn ticks.
  const double steps[] = {
      1.0 / 24.0,  // 1 hour
      6.0 / 24.0,  // 6 hours
      12.0 / 24.0, // 12 hours
      1.0,
      2.0,
      5.0,
      10.0,
      20.0,
      50.0,
      100.0,
      200.0,
      500.0,
      1000.0,
  };
  for (double s : steps) {
    if (s >= raw_days) return s;
  }
  return steps[IM_ARRAYSIZE(steps) - 1];
}

struct DayHour {
  std::int64_t day{0};
  int hour{0};
};

DayHour split_day_hour(double t_days) {
  // Convert a continuous day value into (day, hour) rounded to the nearest hour.
  //
  // We round because the timeline uses floating-point values for layout, and
  // the tick steps are exact multiples of 1/24 in intent but not always in
  // binary.
  const double day_floor = std::floor(t_days);
  std::int64_t day = static_cast<std::int64_t>(day_floor);
  const double frac = t_days - day_floor;
  int hour = static_cast<int>(std::llround(frac * 24.0));
  // Handle rounding spillover.
  if (hour >= 24) {
    hour -= 24;
    day += 1;
  } else if (hour < 0) {
    hour += 24;
    day -= 1;
  }
  return {day, clamp_hour(hour)};
}

double event_time_days(const SimEvent& ev) {
  return static_cast<double>(ev.day) + static_cast<double>(clamp_hour(ev.hour)) / 24.0;
}

struct TimelineViewState {
  bool initialized{false};
  double px_per_day{12.0};
  double start_day{0.0};

  std::uint64_t selected_seq{0};
  std::uint64_t context_seq{0};

  char search[128]{};

  bool show_info{true};
  bool show_warn{true};
  bool show_error{true};

  bool cat_enabled[kLaneCount];

  TimelineViewState() {
    std::fill(std::begin(cat_enabled), std::end(cat_enabled), true);
    std::memset(search, 0, sizeof(search));
  }
};

static TimelineViewState tl;

const SimEvent* find_event_by_seq(const std::vector<SimEvent>& events, std::uint64_t seq) {
  if (seq == 0) return nullptr;
  for (const auto& ev : events) {
    if (ev.seq == seq) return &ev;
  }
  return nullptr;
}

void jump_to_event(const SimEvent& ev, Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony,
                   Id& selected_body) {
  auto& s = sim.state();

  if (ev.system_id != kInvalidId) {
    s.selected_system = ev.system_id;
    ui.show_map_window = true;
    ui.request_map_tab = MapTab::System;
  }

  if (ev.colony_id != kInvalidId) {
    selected_colony = ev.colony_id;
    if (const auto* c = find_ptr(s.colonies, ev.colony_id)) {
      selected_body = c->body_id;
      if (const auto* b = (c->body_id != kInvalidId) ? find_ptr(s.bodies, c->body_id) : nullptr) {
        s.selected_system = b->system_id;
      }
    }
    ui.show_details_window = true;
    ui.request_details_tab = DetailsTab::Colony;
  }

  if (ev.ship_id != kInvalidId) {
    selected_ship = ev.ship_id;
    ui.selected_fleet_id = sim.fleet_for_ship(ev.ship_id);
    if (const auto* sh = find_ptr(s.ships, ev.ship_id)) {
      s.selected_system = sh->system_id;
    }
    ui.show_details_window = true;
    ui.request_details_tab = DetailsTab::Ship;
  }
}

void draw_marker(ImDrawList* dl, const ImVec2& p, float r, EventLevel level, bool selected) {
  const ImU32 fill = level_color_u32(level, 0.95f);
  const ImU32 outline = IM_COL32(0, 0, 0, 190);
  const ImU32 shadow = IM_COL32(0, 0, 0, 110);

  // Soft shadow + glow.
  dl->AddCircleFilled(ImVec2(p.x + 1.2f, p.y + 1.2f), r + 1.4f, shadow, 0);
  dl->AddCircleFilled(p, r * 2.8f, level_color_u32(level, 0.10f), 0);
  dl->AddCircleFilled(p, r * 1.9f, level_color_u32(level, 0.18f), 0);

  // Shape by severity.
  if (level == EventLevel::Warn) {
    const ImVec2 a(p.x, p.y - r);
    const ImVec2 b(p.x + r, p.y);
    const ImVec2 c(p.x, p.y + r);
    const ImVec2 d(p.x - r, p.y);
    dl->AddQuadFilled(a, b, c, d, fill);
    dl->AddQuad(a, b, c, d, outline, 1.0f);
  } else if (level == EventLevel::Error) {
    const ImVec2 a(p.x, p.y - r);
    const ImVec2 b(p.x + r, p.y + r);
    const ImVec2 c(p.x - r, p.y + r);
    dl->AddTriangleFilled(a, b, c, fill);
    dl->AddTriangle(a, b, c, outline, 1.0f);
  } else {
    dl->AddCircleFilled(p, r, fill, 0);
    dl->AddCircle(p, r, outline, 0, 1.0f);
  }

  if (selected) {
    dl->AddCircle(p, r + 3.5f, level_color_u32(level, 0.75f), 0, 2.0f);
    dl->AddCircle(p, r + 6.0f, level_color_u32(level, 0.25f), 0, 2.0f);
  }
}

} // namespace

void draw_timeline_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  if (!ui.show_timeline_window) return;

  ImGui::SetNextWindowSize(ImVec2(980, 620), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Timeline", &ui.show_timeline_window)) {
    ImGui::End();
    return;
  }

  auto& s = sim.state();
  const auto& events = s.events;

  if (events.empty()) {
    ImGui::TextDisabled("No events yet.");
    ImGui::TextDisabled("Advance time or perform actions to generate SimEvents.");
    ImGui::End();
    return;
  }

  // Compute event time bounds.
  //
  // The canvas uses a continuous "days" unit where 1.0 == 24 hours.
  double min_time = event_time_days(events.front());
  double max_time = min_time;

  // Keep integer day bounds for coarse minimap/grid ranges.
  std::int64_t min_day = events.front().day;
  std::int64_t max_day = events.front().day;

  for (const auto& ev : events) {
    min_day = std::min(min_day, ev.day);
    max_day = std::max(max_day, ev.day);
    const double t = event_time_days(ev);
    min_time = std::min(min_time, t);
    max_time = std::max(max_time, t);
  }

  const std::int64_t now_day = s.date.days_since_epoch();
  const double now_time = static_cast<double>(now_day) + static_cast<double>(clamp_hour(s.hour_of_day)) / 24.0;

  // --- Toolbar / filters ---
  ImGui::TextDisabled("Visualize and navigate the persistent event log (SimEvents).");
  ImGui::SameLine();
  ImGui::TextDisabled("Tip: Right-click an event marker for quick actions.");

  ImGui::Separator();

  ImGui::InputTextWithHint("##timeline_search", "Search message text…", tl.search, IM_ARRAYSIZE(tl.search));

  ImGui::SameLine();
  ImGui::Checkbox("Info", &tl.show_info);
  ImGui::SameLine();
  ImGui::Checkbox("Warn", &tl.show_warn);
  ImGui::SameLine();
  ImGui::Checkbox("Error", &tl.show_error);

  ImGui::SameLine();
  ImGui::TextDisabled("|");

  ImGui::SameLine();
  ImGui::Checkbox("Follow now", &ui.timeline_follow_now);

  ImGui::SameLine();
  if (ImGui::SmallButton("Now")) {
    ui.timeline_follow_now = true;
  }

  ImGui::SameLine();
  if (ImGui::SmallButton("Reset view")) {
    tl.initialized = false;
  }

  // Category filters in a compact popup.
  ImGui::SameLine();
  if (ImGui::SmallButton("Categories…")) {
    ImGui::OpenPopup("##timeline_categories");
  }
  if (ImGui::BeginPopup("##timeline_categories")) {
    if (ImGui::SmallButton("All")) {
      std::fill(std::begin(tl.cat_enabled), std::end(tl.cat_enabled), true);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("None")) {
      std::fill(std::begin(tl.cat_enabled), std::end(tl.cat_enabled), false);
    }
    ImGui::Separator();
    for (int i = 0; i < kLaneCount; ++i) {
      ImGui::Checkbox(kLanes[(std::size_t)i].label, &tl.cat_enabled[i]);
    }
    ImGui::EndPopup();
  }

  // Visual options.
  ImGui::SameLine();
  if (ImGui::SmallButton("Options…")) {
    ImGui::OpenPopup("##timeline_options");
  }
  if (ImGui::BeginPopup("##timeline_options")) {
    ImGui::Checkbox("Minimap", &ui.timeline_show_minimap);
    ImGui::Checkbox("Grid", &ui.timeline_show_grid);
    ImGui::Checkbox("Lane labels", &ui.timeline_show_labels);
    ImGui::Checkbox("Compact rows", &ui.timeline_compact_rows);

    ImGui::Separator();
    ImGui::SliderFloat("Lane height", &ui.timeline_lane_height, 18.0f, 56.0f, "%.0f px");
    ImGui::SliderFloat("Marker size", &ui.timeline_marker_size, 2.5f, 7.0f, "%.1f px");
    ImGui::EndPopup();
  }

  // Legend.
  {
    ImGui::SameLine();
    ImGui::TextDisabled("Legend:");
    ImGui::SameLine();
    ImGui::TextColored(level_color_rgba(EventLevel::Info), "● Info");
    ImGui::SameLine();
    ImGui::TextColored(level_color_rgba(EventLevel::Warn), "◆ Warn");
    ImGui::SameLine();
    ImGui::TextColored(level_color_rgba(EventLevel::Error), "▲ Error");
  }

  ImGui::Separator();

  // Layout.
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const float minimap_h = ui.timeline_show_minimap ? 68.0f : 0.0f;
  const float details_h = 170.0f;
  const float spacing = ImGui::GetStyle().ItemSpacing.y;
  const float canvas_h = std::max(220.0f, avail.y - details_h - minimap_h - (ui.timeline_show_minimap ? spacing : 0.0f));

  // --- Timeline canvas ---
  double view_days = 1.0;
  if (ImGui::BeginChild("##timeline_canvas", ImVec2(0, canvas_h), true,
                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 cpos = ImGui::GetCursorScreenPos();
    const ImVec2 csize = ImGui::GetContentRegionAvail();

    ImGui::InvisibleButton("##timeline_canvas_btn", csize,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();

    ImGuiIO& io = ImGui::GetIO();

    // Style-scaled geometry.
    float lane_h = std::max(18.0f, ui.timeline_lane_height * (ui.timeline_compact_rows ? 0.76f : 1.0f));
    float marker_r = std::max(2.0f, ui.timeline_marker_size * (ui.timeline_compact_rows ? 0.86f : 1.0f));
    const float axis_h = 24.0f;
    const float label_w = ui.timeline_show_labels ? 122.0f : 10.0f;

    // Fit lanes into the available height if needed.
    const float max_lane_h = std::max(10.0f, (csize.y - axis_h) / (float)kLaneCount);
    if (lane_h > max_lane_h) lane_h = max_lane_h;

    const ImVec2 lanes_pos(cpos.x + label_w, cpos.y + axis_h);
    const ImVec2 lanes_size(std::max(1.0f, csize.x - label_w), std::max(1.0f, csize.y - axis_h));

    // Clamp zoom (allow sub-day exploration at higher zoom levels).
    tl.px_per_day = std::clamp(tl.px_per_day, 2.0, 720.0);

    view_days = std::max(1.0, (double)lanes_size.x / tl.px_per_day);

    // First-time initialization: show the most recent history.
    if (!tl.initialized) {
      tl.initialized = true;
      tl.px_per_day = 12.0;
      view_days = std::max(1.0, (double)lanes_size.x / tl.px_per_day);
      tl.start_day = std::max(min_time, now_time - view_days);
    }

    // Apply programmatic focus request (from toast/log buttons).
    if (ui.request_focus_event_seq != 0) {
      if (const SimEvent* ev = find_event_by_seq(events, ui.request_focus_event_seq)) {
        tl.selected_seq = ev->seq;
        ui.timeline_follow_now = false;
        tl.start_day = event_time_days(*ev) - view_days * 0.5;
      }
      ui.request_focus_event_seq = 0;
    }

    // Follow now: keep the right edge close to the current sim date.
    if (ui.timeline_follow_now) {
      tl.start_day = now_time - view_days;
    }

    // Input: zoom/pan (only over the marker region, not the label column).
    const bool mouse_over_lanes = hovered && (io.MousePos.x >= lanes_pos.x) && (io.MousePos.x <= lanes_pos.x + lanes_size.x) &&
                                 (io.MousePos.y >= lanes_pos.y) && (io.MousePos.y <= lanes_pos.y + lanes_size.y);

    if (mouse_over_lanes && !io.WantTextInput) {
      if (io.MouseWheel != 0.0f) {
        const double x = (double)(io.MousePos.x - lanes_pos.x);
        const double day_at_cursor = tl.start_day + x / tl.px_per_day;

        const double factor = std::pow(1.10, (double)io.MouseWheel);
        double new_ppd = tl.px_per_day * factor;
        new_ppd = std::clamp(new_ppd, 2.0, 720.0);

        tl.px_per_day = new_ppd;
        view_days = std::max(1.0, (double)lanes_size.x / tl.px_per_day);
        tl.start_day = day_at_cursor - x / tl.px_per_day;

        ui.timeline_follow_now = false;
      }

      if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        const ImVec2 d = io.MouseDelta;
        tl.start_day -= (double)d.x / tl.px_per_day;
        ui.timeline_follow_now = false;
      }
    }

    // Clamp view to a reasonable range (allow a bit of overscroll for aesthetics).
    {
      const double overscroll = view_days * 0.15;
      const double data_min = std::min(min_time, now_time);
      const double data_max = std::max(max_time, now_time);
      const double min_start = data_min - overscroll;
      const double max_start = data_max - view_days + overscroll;
      if (min_start < max_start) {
        tl.start_day = std::clamp(tl.start_day, min_start, max_start);
      } else {
        tl.start_day = min_start;
      }
    }

    const double end_day = tl.start_day + view_days;

    // --- Draw background / chrome ---
    {
      const ImU32 bg_top = ImGui::GetColorU32(ImVec4(0.07f, 0.075f, 0.085f, 1.0f));
      const ImU32 bg_bot = ImGui::GetColorU32(ImVec4(0.04f, 0.045f, 0.052f, 1.0f));
      dl->AddRectFilledMultiColor(cpos, ImVec2(cpos.x + csize.x, cpos.y + csize.y), bg_top, bg_top, bg_bot, bg_bot);

      // Axis strip.
      const ImU32 axis_col = ImGui::GetColorU32(ImVec4(0.10f, 0.105f, 0.115f, 1.0f));
      dl->AddRectFilled(cpos, ImVec2(cpos.x + csize.x, cpos.y + axis_h), axis_col);
      dl->AddLine(ImVec2(cpos.x, cpos.y + axis_h), ImVec2(cpos.x + csize.x, cpos.y + axis_h), IM_COL32(0, 0, 0, 140), 1.0f);
    }

    // Lanes background + labels.
    const float lanes_bottom = lanes_pos.y + lane_h * (float)kLaneCount;
    for (int i = 0; i < kLaneCount; ++i) {
      const float y0 = lanes_pos.y + (float)i * lane_h;
      const float y1 = y0 + lane_h;
      const bool alt = ((i % 2) == 1);

      const ImU32 lane_bg = ImGui::GetColorU32(alt ? ImVec4(0.06f, 0.065f, 0.075f, 0.55f) : ImVec4(0.05f, 0.055f, 0.065f, 0.45f));
      dl->AddRectFilled(ImVec2(lanes_pos.x, y0), ImVec2(lanes_pos.x + lanes_size.x, y1), lane_bg);

      dl->AddLine(ImVec2(lanes_pos.x, y1), ImVec2(lanes_pos.x + lanes_size.x, y1), IM_COL32(0, 0, 0, 75), 1.0f);

      if (ui.timeline_show_labels) {
        const ImU32 lab_col = tl.cat_enabled[i] ? IM_COL32(210, 210, 220, 255) : IM_COL32(120, 120, 130, 255);
        dl->AddText(ImVec2(cpos.x + 8.0f, y0 + 4.0f), lab_col, kLanes[(std::size_t)i].label);
      }
    }

    // Grid (vertical time ticks).
    if (ui.timeline_show_grid) {
      const double desired_major_px = 140.0;
      const double major_step = nice_step(desired_major_px / tl.px_per_day);
      const double minor_step = std::max(1.0 / 24.0, major_step / 6.0);

      // Minor lines (optional when zoomed-in enough).
      if (tl.px_per_day * minor_step >= 12.0) {
        const double first_minor = std::floor(tl.start_day / minor_step) * minor_step;
        for (double d = first_minor; d <= end_day + minor_step; d += minor_step) {
          const float x = lanes_pos.x + (float)((d - tl.start_day) * tl.px_per_day);
          if (x < lanes_pos.x - 2.0f || x > lanes_pos.x + lanes_size.x + 2.0f) continue;
          dl->AddLine(ImVec2(x, lanes_pos.y), ImVec2(x, lanes_bottom), IM_COL32(255, 255, 255, 18), 1.0f);
        }
      }

      // Major lines + labels.
      const double first_major = std::floor(tl.start_day / major_step) * major_step;
      for (double d = first_major; d <= end_day + major_step; d += major_step) {
        const float x = lanes_pos.x + (float)((d - tl.start_day) * tl.px_per_day);
        if (x < lanes_pos.x - 2.0f || x > lanes_pos.x + lanes_size.x + 2.0f) continue;

        dl->AddLine(ImVec2(x, lanes_pos.y), ImVec2(x, lanes_bottom), IM_COL32(255, 255, 255, 40), 1.5f);

        // Axis labels (only when there's room).
        if (tl.px_per_day * major_step >= 64.0) {
          std::string label;
          if (major_step >= 1.0) {
            const nebula4x::Date date((std::int64_t)std::llround(d));
            label = date.to_string();
          } else {
            const DayHour dh = split_day_hour(d);
            label = format_datetime(nebula4x::Date(dh.day), dh.hour);
          }
          dl->AddText(ImVec2(x + 3.0f, cpos.y + 4.0f), IM_COL32(220, 220, 220, 200), label.c_str());
        }
      }
    }

    // "Now" indicator.
    {
      const float x_now = lanes_pos.x + (float)((now_time - tl.start_day) * tl.px_per_day);
      if (x_now >= lanes_pos.x && x_now <= lanes_pos.x + lanes_size.x) {
        dl->AddLine(ImVec2(x_now, lanes_pos.y), ImVec2(x_now, lanes_bottom), IM_COL32(80, 220, 170, 165), 2.0f);
        dl->AddText(ImVec2(x_now + 4.0f, lanes_pos.y - 18.0f), IM_COL32(80, 220, 170, 220), "NOW");
      }
    }

    // Draw events.
    std::unordered_map<std::int64_t, int> stacks[kLaneCount];
    for (int i = 0; i < kLaneCount; ++i) stacks[i].reserve(64);

    const SimEvent* hovered_ev = nullptr;
    float hovered_d2 = 1e30f;

    for (const auto& ev : events) {
      const bool ok_level = (ev.level == EventLevel::Info && tl.show_info) || (ev.level == EventLevel::Warn && tl.show_warn) ||
                            (ev.level == EventLevel::Error && tl.show_error);
      if (!ok_level) continue;

      const int lane = lane_index(ev.category);
      if (lane < 0 || lane >= kLaneCount) continue;
      if (!tl.cat_enabled[lane]) continue;

      if (!matches_search(ev, tl.search)) continue;

      const double t = event_time_days(ev);
      if (t < tl.start_day - 1.0 || t > end_day + 1.0) continue;

      const float x = lanes_pos.x + (float)((t - tl.start_day) * tl.px_per_day);
      if (x < lanes_pos.x - 8.0f || x > lanes_pos.x + lanes_size.x + 8.0f) continue;

      const float y_center = lanes_pos.y + (float)lane * lane_h + lane_h * 0.5f;

      // Stack multiple events that share the same lane + hour-bucket.
      const std::int64_t bucket = ev.day * 24 + static_cast<std::int64_t>(clamp_hour(ev.hour));
      int n = stacks[lane][bucket]++;
      float dy = 0.0f;
      if (n > 0) {
        const int band = (n + 1) / 2;
        const float sign = (n % 2 == 1) ? -1.0f : 1.0f;
        dy = sign * (float)band * (marker_r * 1.55f);
      }

      float y = y_center + dy;
      const float y0 = lanes_pos.y + (float)lane * lane_h;
      const float y1 = y0 + lane_h;
      y = std::clamp(y, y0 + marker_r + 2.0f, y1 - marker_r - 2.0f);

      const bool is_selected = (tl.selected_seq != 0 && ev.seq == tl.selected_seq);
      draw_marker(dl, ImVec2(x, y), marker_r, ev.level, is_selected);

      if (mouse_over_lanes) {
        const float dx = io.MousePos.x - x;
        const float dy2 = io.MousePos.y - y;
        const float d2 = dx * dx + dy2 * dy2;
        const float rr = (marker_r + 5.0f) * (marker_r + 5.0f);
        if (d2 <= rr && d2 < hovered_d2) {
          hovered_d2 = d2;
          hovered_ev = &ev;
        }
      }
    }

    // Hover tooltip + interaction.
    if (hovered_ev && mouse_over_lanes) {
      ImGui::BeginTooltip();
      const std::string dt = format_datetime(nebula4x::Date(hovered_ev->day), hovered_ev->hour);
      ImGui::Text("%s", dt.c_str());
      ImGui::TextDisabled("#%llu  %s  %s", (unsigned long long)hovered_ev->seq, level_label(hovered_ev->level),
                          kLanes[(std::size_t)lane_index(hovered_ev->category)].label);
      ImGui::Separator();
      ImGui::TextWrapped("%s", hovered_ev->message.c_str());
      ImGui::Separator();
      ImGui::TextDisabled("Left-click: select   Double-click: center   Right-click: actions");
      ImGui::EndTooltip();

      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        tl.selected_seq = hovered_ev->seq;
      }
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        ui.timeline_follow_now = false;
        tl.start_day = event_time_days(*hovered_ev) - view_days * 0.5;
      }
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        tl.context_seq = hovered_ev->seq;
        ImGui::OpenPopup("##timeline_event_ctx");
      }
    }

    // Context menu for a specific event.
    if (ImGui::BeginPopup("##timeline_event_ctx")) {
      const SimEvent* ev = find_event_by_seq(events, tl.context_seq);
      if (!ev) {
        ImGui::TextDisabled("(event missing)");
        ImGui::EndPopup();
      } else {
        const std::string dt = format_datetime(nebula4x::Date(ev->day), ev->hour);
        ImGui::Text("%s", dt.c_str());
        ImGui::TextDisabled("#%llu  %s  %s", (unsigned long long)ev->seq, level_label(ev->level),
                            kLanes[(std::size_t)lane_index(ev->category)].label);
        ImGui::Separator();
        ImGui::TextWrapped("%s", ev->message.c_str());
        ImGui::Separator();

        if (ImGui::MenuItem("Center on timeline")) {
          ui.timeline_follow_now = false;
          tl.start_day = event_time_days(*ev) - view_days * 0.5;
        }
        if (ImGui::MenuItem("Open Event Log")) {
          ui.show_details_window = true;
          ui.request_details_tab = DetailsTab::Log;
        }
        if (ImGui::MenuItem("Jump to context")) {
          jump_to_event(*ev, sim, ui, selected_ship, selected_colony, selected_body);
        }
        ImGui::EndPopup();
      }
    }

    // Footer: show visible range.
    {
      const DayHour t0 = split_day_hour(tl.start_day);
      const DayHour t1 = split_day_hour(end_day);
      const std::string rng = format_datetime(nebula4x::Date(t0.day), t0.hour) + "  →  " +
                              format_datetime(nebula4x::Date(t1.day), t1.hour);
      dl->AddText(ImVec2(cpos.x + 8.0f, cpos.y + csize.y - 18.0f), IM_COL32(200, 200, 200, 160), rng.c_str());
    }
  }
  ImGui::EndChild();

  // --- Minimap ---
  if (ui.timeline_show_minimap) {
    if (ImGui::BeginChild("##timeline_minimap", ImVec2(0, minimap_h), true,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImVec2 p0 = ImGui::GetCursorScreenPos();
      const ImVec2 sz = ImGui::GetContentRegionAvail();

      ImGui::InvisibleButton("##timeline_minimap_btn", sz,
                             ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
      const bool hovered = ImGui::IsItemHovered();
      ImGuiIO& io = ImGui::GetIO();

      const ImU32 bg = ImGui::GetColorU32(ImVec4(0.06f, 0.065f, 0.075f, 1.0f));
      dl->AddRectFilled(p0, ImVec2(p0.x + sz.x, p0.y + sz.y), bg);
      dl->AddRect(p0, ImVec2(p0.x + sz.x, p0.y + sz.y), IM_COL32(0, 0, 0, 180));

      const double full_range = std::max(1.0, (double)(std::max(max_day, now_day) - min_day + 1));
      auto day_to_x = [&](std::int64_t day) -> float {
        const double t = ((double)(day - min_day)) / full_range;
        return p0.x + (float)(t * (double)sz.x);
      };

      // Event density tick marks (filtered).
      for (const auto& ev : events) {
        const bool ok_level = (ev.level == EventLevel::Info && tl.show_info) || (ev.level == EventLevel::Warn && tl.show_warn) ||
                              (ev.level == EventLevel::Error && tl.show_error);
        if (!ok_level) continue;

        const int lane = lane_index(ev.category);
        if (!tl.cat_enabled[lane]) continue;
        if (!matches_search(ev, tl.search)) continue;

        const float x = day_to_x(ev.day);
        dl->AddLine(ImVec2(x, p0.y + 6.0f), ImVec2(x, p0.y + sz.y - 6.0f), level_color_u32(ev.level, 0.18f), 1.0f);
      }

      // Viewport rectangle.
      const float vx0 = p0.x + (float)(((tl.start_day - (double)min_day) / full_range) * (double)sz.x);
      const float vx1 = p0.x + (float)(((tl.start_day + view_days - (double)min_day) / full_range) * (double)sz.x);
      const ImU32 vcol = IM_COL32(255, 255, 255, 110);
      dl->AddRectFilled(ImVec2(vx0, p0.y + 3.0f), ImVec2(vx1, p0.y + sz.y - 3.0f), IM_COL32(255, 255, 255, 25));
      dl->AddRect(ImVec2(vx0, p0.y + 3.0f), ImVec2(vx1, p0.y + sz.y - 3.0f), vcol, 0.0f, 0, 1.5f);

      // Click-to-pan.
      if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const float rel = (io.MousePos.x - p0.x) / std::max(1.0f, sz.x);
        const double center = (double)min_day + (double)rel * full_range;
        tl.start_day = center - view_days * 0.5;
        ui.timeline_follow_now = false;
      }

      // Draw "now" tick.
      {
        const float x = day_to_x(now_day);
        dl->AddLine(ImVec2(x, p0.y + 2.0f), ImVec2(x, p0.y + sz.y - 2.0f), IM_COL32(80, 220, 170, 180), 2.0f);
      }

      // Selected event tick.
      if (const SimEvent* sel = find_event_by_seq(events, tl.selected_seq)) {
        const float x = day_to_x(sel->day);
        dl->AddCircleFilled(ImVec2(x, p0.y + sz.y * 0.5f), 3.5f, level_color_u32(sel->level, 0.85f), 0);
      }
    }
    ImGui::EndChild();
  }

  // --- Details panel ---
  if (ImGui::BeginChild("##timeline_details", ImVec2(0, 0), true)) {
    const SimEvent* sel = find_event_by_seq(events, tl.selected_seq);

    if (!sel) {
      ImGui::TextDisabled("No event selected.");
      ImGui::TextDisabled("Hover markers for a tooltip, left-click to select.");
    } else {
      const std::string dt = format_datetime(nebula4x::Date(sel->day), sel->hour);
      ImGui::Text("%s", dt.c_str());
      ImGui::SameLine();
      ImGui::TextDisabled("#%llu", (unsigned long long)sel->seq);

      ImGui::SameLine();
      ImGui::TextColored(level_color_rgba(sel->level), "%s", level_label(sel->level));
      ImGui::SameLine();
      ImGui::TextDisabled("[%s]", kLanes[(std::size_t)lane_index(sel->category)].label);

      if (ImGui::SmallButton("Clear selection")) {
        tl.selected_seq = 0;
      }

      ImGui::SameLine();
      if (ImGui::SmallButton("Center")) {
        ui.timeline_follow_now = false;
        tl.start_day = event_time_days(*sel) - view_days * 0.5;
      }

      ImGui::SameLine();
      if (ImGui::SmallButton("Open log")) {
        ui.show_details_window = true;
        ui.request_details_tab = DetailsTab::Log;
      }

      ImGui::Separator();
      ImGui::TextWrapped("%s", sel->message.c_str());

      ImGui::Separator();

      // Quick context navigation (linked elements).
      if (sel->system_id != kInvalidId) {
        if (ImGui::Button("View system")) {
          s.selected_system = sel->system_id;
          ui.show_map_window = true;
          ui.request_map_tab = MapTab::System;
        }
        if (const auto* sys = find_ptr(s.systems, sel->system_id)) {
          ImGui::SameLine();
          ImGui::TextDisabled("%s", sys->name.c_str());
        }
      }

      if (sel->colony_id != kInvalidId) {
        if (ImGui::Button("Select colony")) {
          selected_colony = sel->colony_id;
          if (const auto* c = find_ptr(s.colonies, sel->colony_id)) {
            selected_body = c->body_id;
            if (const auto* b = (c->body_id != kInvalidId) ? find_ptr(s.bodies, c->body_id) : nullptr) {
              s.selected_system = b->system_id;
            }
          }
          ui.show_details_window = true;
          ui.request_details_tab = DetailsTab::Colony;
        }
        if (const auto* c = find_ptr(s.colonies, sel->colony_id)) {
          ImGui::SameLine();
          ImGui::TextDisabled("%s", c->name.c_str());
        }
      }

      if (sel->ship_id != kInvalidId) {
        if (ImGui::Button("Select ship")) {
          selected_ship = sel->ship_id;
          ui.selected_fleet_id = sim.fleet_for_ship(sel->ship_id);
          if (const auto* sh = find_ptr(s.ships, sel->ship_id)) {
            s.selected_system = sh->system_id;
          }
          ui.show_details_window = true;
          ui.request_details_tab = DetailsTab::Ship;
        }
        if (const auto* sh = find_ptr(s.ships, sel->ship_id)) {
          ImGui::SameLine();
          ImGui::TextDisabled("%s", sh->name.c_str());
        }
      }
    }
  }
  ImGui::EndChild();

  ImGui::End();
}

} // namespace nebula4x::ui
