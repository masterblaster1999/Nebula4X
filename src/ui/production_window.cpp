#include "ui/production_window.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "nebula4x/util/strings.h"

#include "ui/map_render.h"  // nice_number_125 + modulate_alpha

namespace nebula4x::ui {
namespace {
inline ImU32 with_alpha(ImU32 col, float a) {
  return modulate_alpha(col, a);
}

struct TimelineView {
  // How many pixels represent one day.
  double px_per_day{18.0};
  // Day offset at the left edge of the timeline (0 = today).
  double origin_day{0.0};

  bool show_grid{true};
  bool show_dates{true};
};

struct ShipyardScheduleItem {
  int index{0};
  int start_day{0};
  int end_day{0};
  int full_end_day{0};

  bool stalled{false};
  std::string stall_reason;

  bool is_refit{false};
  Id refit_ship_id{kInvalidId};
  std::string design_id;
  std::string label;

  double start_remaining_tons{0.0};
  double built_tons{0.0};
  double original_tons{0.0};
  double build_rate_tpd{0.0};
};

struct ConstructionScheduleItem {
  int index{0};

  std::string installation_id;
  std::string label;
  int total_units{0};
  int completed_units{0};

  int start_day{0};
  int end_day{0};

  bool stalled{false};
  std::string stall_reason;

  double cp_per_day{0.0};
};

double shipyard_build_rate_tpd(const Simulation& sim, const Colony& c) {
  const auto it_def = sim.content().installations.find("shipyard");
  if (it_def == sim.content().installations.end()) return 0.0;
  const InstallationDef& def = it_def->second;
  if (def.build_rate_tons_per_day <= 0.0) return 0.0;
  const auto it = c.installations.find("shipyard");
  const int yards = (it != c.installations.end()) ? std::max(0, it->second) : 0;
  if (yards <= 0) return 0.0;
  return def.build_rate_tons_per_day * static_cast<double>(yards);
}

// Returns a deterministic, best-effort shipyard schedule estimate.
//
// Model assumptions (UI-only, not changing the sim):
// - Uses current shipyard tons/day capacity.
// - Consumes minerals from current stockpiles (no resupply).
// - Respects refit stall conditions (ship must be docked).
// - Shipyard processes orders strictly front-to-back.
std::vector<ShipyardScheduleItem> estimate_shipyard_schedule(const Simulation& sim, const Colony& colony) {
  std::vector<ShipyardScheduleItem> out;

  const auto it_def = sim.content().installations.find("shipyard");
  const InstallationDef* shipyard_def = (it_def != sim.content().installations.end()) ? &it_def->second : nullptr;
  const double rate = shipyard_build_rate_tpd(sim, colony);

  std::unordered_map<std::string, double> minerals = colony.minerals;

  int day_cursor = 0;

  for (int i = 0; i < static_cast<int>(colony.shipyard_queue.size()); ++i) {
    const auto& bo = colony.shipyard_queue[static_cast<std::size_t>(i)];
    ShipyardScheduleItem it;
    it.index = i;
    it.start_day = day_cursor;
    it.end_day = day_cursor;
    it.full_end_day = day_cursor;
    it.design_id = bo.design_id;
    it.is_refit = (bo.refit_ship_id != kInvalidId);
    it.refit_ship_id = bo.refit_ship_id;
    it.start_remaining_tons = std::max(0.0, bo.tons_remaining);
    it.build_rate_tpd = rate;

    const Ship* refit_ship = it.is_refit ? nebula4x::find_ptr(sim.state().ships, it.refit_ship_id) : nullptr;
    const auto* d = sim.find_design(it.design_id);
    const std::string design_name = d ? d->name : it.design_id;

    if (!it.is_refit) {
      it.label = design_name;
      it.original_tons = d ? std::max(1.0, d->mass_tons) : std::max(1.0, it.start_remaining_tons);
    } else {
      const std::string ship_name = refit_ship ? refit_ship->name : ("Ship #" + std::to_string(static_cast<int>(it.refit_ship_id)));
      it.label = "REFIT: " + ship_name + " -> " + design_name;
      it.original_tons = sim.estimate_refit_tons(it.refit_ship_id, it.design_id);
      if (it.original_tons <= 0.0) it.original_tons = std::max(1.0, it.start_remaining_tons);
    }

    // Baseline full-duration estimate (ignoring minerals).
    if (rate > 1e-9) {
      const int full_days = static_cast<int>(std::ceil(it.start_remaining_tons / rate));
      it.full_end_day = it.start_day + std::max(0, full_days);
    } else {
      it.full_end_day = it.start_day;
    }

    // Hard stall cases.
    if (rate <= 1e-9) {
      it.stalled = true;
      it.stall_reason = "no shipyard capacity";
      // Shipyard is effectively stalled; later orders won't progress.
      out.push_back(std::move(it));
      break;
    }

    if (it.is_refit) {
      if (!refit_ship) {
        it.stalled = true;
        it.stall_reason = "refit ship missing";
        out.push_back(std::move(it));
        break;
      }
      if (!sim.is_ship_docked_at_colony(refit_ship->id, colony.id)) {
        it.stalled = true;
        it.stall_reason = "ship not docked";
        out.push_back(std::move(it));
        break;
      }
    }

    // Mineral-limited build amount (no resupply assumption).
    double possible_tons = it.start_remaining_tons;
    if (shipyard_def) {
      for (const auto& [mineral, cost_per_ton] : shipyard_def->build_costs_per_ton) {
        if (cost_per_ton <= 0.0) continue;
        const auto mit = minerals.find(mineral);
        const double avail = (mit != minerals.end()) ? std::max(0.0, mit->second) : 0.0;
        possible_tons = std::min(possible_tons, avail / cost_per_ton);
      }
    }

    if (possible_tons <= 1e-9) {
      it.stalled = true;
      it.stall_reason = "insufficient minerals";
      out.push_back(std::move(it));
      break;
    }

    // How many days of work do we have before we complete or stall?
    const double build_tons = std::min(it.start_remaining_tons, possible_tons);
    const int days = std::max(1, static_cast<int>(std::ceil(build_tons / rate)));
    it.built_tons = build_tons;
    it.end_day = it.start_day + days;

    // Consume minerals.
    if (shipyard_def) {
      for (const auto& [mineral, cost_per_ton] : shipyard_def->build_costs_per_ton) {
        if (cost_per_ton <= 0.0) continue;
        minerals[mineral] = std::max(0.0, minerals[mineral] - build_tons * cost_per_ton);
      }
    }

    if (build_tons + 1e-9 < it.start_remaining_tons) {
      it.stalled = true;
      it.stall_reason = "minerals depleted";
      out.push_back(std::move(it));
      break;
    }

    // Completed this order; advance cursor.
    out.push_back(std::move(it));
    day_cursor = out.back().end_day;
  }

  return out;
}

// A simplified construction schedule estimator.
//
// Model assumptions (UI-only):
// - Uses current colony minerals as a fixed pool (no resupply).
// - Uses current CP/day.
// - Re-implements the tick_construction loop logic on copies to forecast which
//   orders can complete and when.
std::vector<ConstructionScheduleItem> estimate_construction_schedule(const Simulation& sim, const Colony& colony) {
  std::vector<ConstructionScheduleItem> out;

  // Build a working copy of the queue with stable UI indices.
  struct Working {
    int ui_index{0};
    InstallationBuildOrder ord;
  };

  std::vector<Working> q;
  q.reserve(colony.construction_queue.size());
  for (int i = 0; i < static_cast<int>(colony.construction_queue.size()); ++i) {
    Working w;
    w.ui_index = i;
    w.ord = colony.construction_queue[static_cast<std::size_t>(i)];
    q.push_back(std::move(w));
  }

  // Per-order stats (keyed by ui_index).
  struct Stats {
    bool started{false};
    int start_day{0};
    int last_day{0};
    int completed_units{0};
    bool stalled{false};
    std::string stall_reason;
  };

  std::unordered_map<int, Stats> stats;
  stats.reserve(q.size() * 2);

  // Seed stats from current queue state.
  for (const auto& w : q) {
    Stats st;
    st.started = w.ord.minerals_paid || (w.ord.cp_remaining > 1e-9);
    st.start_day = 0;
    st.last_day = 0;
    st.completed_units = 0;
    stats[w.ui_index] = std::move(st);
  }

  // Copy minerals (already reflect any previously paid minerals).
  std::unordered_map<std::string, double> minerals = colony.minerals;

  const double cp_per_day = sim.construction_points_per_day(colony);
  const int max_days = 5000;  // UI safety cap.

  auto can_pay = [&](const InstallationDef& def) {
    for (const auto& [mineral, cost] : def.build_costs) {
      if (cost <= 0.0) continue;
      const auto it = minerals.find(mineral);
      const double have = (it != minerals.end()) ? std::max(0.0, it->second) : 0.0;
      if (have + 1e-9 < cost) return false;
    }
    return true;
  };

  auto pay = [&](const InstallationDef& def) {
    for (const auto& [mineral, cost] : def.build_costs) {
      if (cost <= 0.0) continue;
      minerals[mineral] = std::max(0.0, minerals[mineral] - cost);
    }
  };

  int day = 0;
  for (; day < max_days; ++day) {
    if (q.empty()) break;

    double cp_available = cp_per_day;
    if (cp_available <= 1e-9) break;

    bool progressed_any = false;

    // Mirror Simulation::tick_construction's queue scan (on our copies).
    for (std::size_t i = 0; i < q.size() && cp_available > 1e-9;) {
      auto& w = q[i];
      auto& ord = w.ord;
      Stats& st = stats[w.ui_index];

      if (ord.quantity_remaining <= 0) {
        q.erase(q.begin() + static_cast<std::ptrdiff_t>(i));
        progressed_any = true;
        continue;
      }

      auto it_def = sim.content().installations.find(ord.installation_id);
      if (it_def == sim.content().installations.end()) {
        st.stalled = true;
        st.stall_reason = "unknown installation";
        q.erase(q.begin() + static_cast<std::ptrdiff_t>(i));
        progressed_any = true;
        continue;
      }
      const InstallationDef& def = it_def->second;

      auto complete_one = [&]() {
        ord.quantity_remaining -= 1;
        ord.minerals_paid = false;
        ord.cp_remaining = 0.0;

        st.completed_units += 1;
        st.started = true;
        st.last_day = day;
        progressed_any = true;

        if (ord.quantity_remaining <= 0) {
          q.erase(q.begin() + static_cast<std::ptrdiff_t>(i));
          return;
        }
        // keep i so we can immediately attempt next unit in the same order
      };

      if (!ord.minerals_paid) {
        if (!can_pay(def)) {
          // Stalled: skip this order for now.
          ++i;
          continue;
        }

        pay(def);
        ord.minerals_paid = true;
        ord.cp_remaining = std::max(0.0, def.construction_cost);

        if (!st.started) st.start_day = day;
        st.started = true;
        st.last_day = day;
        progressed_any = true;

        if (ord.cp_remaining <= 1e-9) {
          // Instant build.
          complete_one();
          continue;
        }
      } else {
        // Defensive repair like sim.
        if (ord.cp_remaining <= 1e-9 && def.construction_cost > 0.0) {
          ord.cp_remaining = def.construction_cost;
        }
      }

      if (ord.minerals_paid && ord.cp_remaining > 1e-9) {
        const double spend = std::min(cp_available, ord.cp_remaining);
        ord.cp_remaining -= spend;
        cp_available -= spend;

        if (!st.started) st.start_day = day;
        st.started = true;
        st.last_day = day;
        progressed_any = true;

        if (ord.cp_remaining <= 1e-9) {
          complete_one();
          continue;
        }
      }

      ++i;
    }

    if (!progressed_any) {
      // No progress in a full scan => mineral stalls.
      break;
    }
  }

  // Build output rows in UI order.
  out.reserve(colony.construction_queue.size());

  for (int i = 0; i < static_cast<int>(colony.construction_queue.size()); ++i) {
    const auto& ord = colony.construction_queue[static_cast<std::size_t>(i)];
    ConstructionScheduleItem it;
    it.index = i;
    it.installation_id = ord.installation_id;
    it.total_units = std::max(0, ord.quantity_remaining);
    it.completed_units = 0;
    it.cp_per_day = cp_per_day;

    const auto it_def = sim.content().installations.find(ord.installation_id);
    const InstallationDef* def = (it_def != sim.content().installations.end()) ? &it_def->second : nullptr;
    it.label = def ? def->name : ord.installation_id;

    auto st_it = stats.find(i);
    if (st_it != stats.end()) {
      const Stats& st = st_it->second;
      it.completed_units = st.completed_units;

      if (st.started) {
        it.start_day = st.start_day;
        it.end_day = st.last_day + 1;
      } else {
        it.start_day = 0;
        it.end_day = 0;
      }
    }

    // If the order is already in progress in the real queue, show it as started today.
    if (ord.minerals_paid || ord.cp_remaining > 1e-9) {
      it.start_day = 0;
      it.end_day = std::max(it.end_day, 1);
    }

    // Infer stall state.
    if (it.completed_units < it.total_units) {
      // Stalled if we hit the simulation horizon without completing.
      // This is intentionally conservative.
      // Provide a better reason when we can.
      bool can_ever_pay = true;
      if (def && !def->build_costs.empty()) {
        for (const auto& [mineral, cost] : def->build_costs) {
          if (cost <= 0.0) continue;
          const auto mit = colony.minerals.find(mineral);
          const double have = (mit != colony.minerals.end()) ? std::max(0.0, mit->second) : 0.0;
          if (have + 1e-9 < cost && !ord.minerals_paid) {
            it.stalled = true;
            it.stall_reason = "need " + mineral;
            can_ever_pay = false;
            break;
          }
        }
      }
      if (can_ever_pay && cp_per_day <= 1e-9) {
        it.stalled = true;
        it.stall_reason = "no construction capacity";
      }
    }

    out.push_back(std::move(it));
  }

  return out;
}

struct AxisLayout {
  float label_w{260.0f};
  float axis_h{34.0f};
  float row_h{30.0f};
};

// Draw timeline axis aligned to the right column.
void draw_timeline_axis(ImDrawList* dl,
                        const ImVec2& p0,
                        const ImVec2& size,
                        const TimelineView& view,
                        const Date& base_date,
                        bool show_dates) {
  if (!dl) return;

  const ImU32 bg = ImGui::GetColorU32(ImGuiCol_FrameBg);
  const ImU32 line = with_alpha(ImGui::GetColorU32(ImGuiCol_Border), 0.85f);
  const ImU32 text = with_alpha(ImGui::GetColorU32(ImGuiCol_Text), 0.85f);
  const ImU32 grid = with_alpha(ImGui::GetColorU32(ImGuiCol_TextDisabled), 0.18f);

  dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), bg, 4.0f);
  dl->AddRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), line, 4.0f);

  if (size.x <= 1.0f || view.px_per_day <= 1e-6) return;

  const double left_day = view.origin_day;
  const double right_day = view.origin_day + static_cast<double>(size.x) / view.px_per_day;

  const double span = std::max(1e-6, right_day - left_day);
  const double raw_step = span / 8.0;
  const double step = std::max(1e-6, nice_number_125(raw_step));

  const double first = std::floor(left_day / step) * step;
  for (double d = first; d <= right_day + step; d += step) {
    const float x = p0.x + static_cast<float>((d - view.origin_day) * view.px_per_day);
    if (x < p0.x - 1.0f || x > p0.x + size.x + 1.0f) continue;
    dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p0.y + size.y), grid);

    // Label.
    char buf[64];
    if (show_dates) {
      const std::int64_t di = static_cast<std::int64_t>(std::llround(d));
      const std::string s = base_date.add_days(di).to_string();
      std::snprintf(buf, sizeof(buf), "%s", s.c_str());
    } else {
      std::snprintf(buf, sizeof(buf), "D+%.0f", d);
    }
    const ImVec2 ts = ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(x + 3.0f, p0.y + size.y - ts.y - 4.0f), text, buf);
  }

  // Today marker.
  const float x0 = p0.x + static_cast<float>((0.0 - view.origin_day) * view.px_per_day);
  if (x0 >= p0.x - 2.0f && x0 <= p0.x + size.x + 2.0f) {
    const ImU32 today = with_alpha(ImGui::GetColorU32(ImGuiCol_PlotLines), 0.95f);
    dl->AddLine(ImVec2(x0, p0.y), ImVec2(x0, p0.y + size.y), today, 2.0f);
    dl->AddText(ImVec2(x0 + 4.0f, p0.y + 4.0f), today, "Today");
  }
}

// Draw a single bar inside [cell_p0, cell_p0+cell_size].
void draw_bar(ImDrawList* dl,
              const ImVec2& cell_p0,
              const ImVec2& cell_size,
              const TimelineView& view,
              int start_day,
              int end_day,
              int full_end_day,
              ImU32 col_fill,
              ImU32 col_border,
              bool stalled,
              bool draw_full_outline) {
  if (!dl) return;
  if (cell_size.x <= 1.0f || cell_size.y <= 1.0f) return;

  // Map day -> x in this cell.
  auto x_for = [&](double day) {
    return cell_p0.x + static_cast<float>((day - view.origin_day) * view.px_per_day);
  };

  const float y0 = cell_p0.y + 6.0f;
  const float y1 = cell_p0.y + cell_size.y - 6.0f;

  // Main segment (clamped).
  float x0 = x_for(start_day);
  float x1 = x_for(end_day);
  if (x1 < x0) std::swap(x0, x1);
  x1 = std::max(x1, x0 + 2.0f);

  const float clip_l = cell_p0.x;
  const float clip_r = cell_p0.x + cell_size.x;
  const bool visible = !(x1 < clip_l || x0 > clip_r);

  if (visible) {
    const float rx0 = std::max(clip_l, x0);
    const float rx1 = std::min(clip_r, x1);
    const float rounding = 4.0f;

    // Subtle gradient for a more "rendered" look.
    const ImU32 c0 = with_alpha(col_fill, stalled ? 0.70f : 0.92f);
    const ImU32 c1 = with_alpha(col_fill, stalled ? 0.45f : 0.75f);
    dl->AddRectFilledMultiColor(ImVec2(rx0, y0), ImVec2(rx1, y1), c0, c0, c1, c1);
    dl->AddRect(ImVec2(rx0, y0), ImVec2(rx1, y1), col_border, rounding, 0, 1.0f);
  }

  if (draw_full_outline && full_end_day > end_day) {
    // Outline the "ideal" completion span (useful when minerals stall).
    float fx0 = x_for(start_day);
    float fx1 = x_for(full_end_day);
    if (fx1 < fx0) std::swap(fx0, fx1);
    fx1 = std::max(fx1, fx0 + 2.0f);
    if (!(fx1 < clip_l || fx0 > clip_r)) {
      const float rx0 = std::max(clip_l, fx0);
      const float rx1 = std::min(clip_r, fx1);

      const ImU32 outline = with_alpha(ImGui::GetColorU32(ImGuiCol_TextDisabled), 0.35f);
      dl->AddRect(ImVec2(rx0, y0 - 2.0f), ImVec2(rx1, y1 + 2.0f), outline, 4.0f, 0, 1.0f);
    }
  }
}

void clamp_timeline_view(TimelineView& v) {
  v.px_per_day = std::clamp(v.px_per_day, 4.0, 120.0);
  // origin_day can be negative (look back), but keep it sane.
  v.origin_day = std::clamp(v.origin_day, -5000.0, 500000.0);
}

void handle_timeline_interactions(TimelineView& v, float timeline_width_px) {
  ImGuiIO& io = ImGui::GetIO();
  if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) return;

  // Ctrl + wheel = zoom.
  if (io.KeyCtrl && std::fabs(io.MouseWheel) > 1e-6f) {
    const double span_px = std::max(1.0f, timeline_width_px);
    const double center_day = v.origin_day + (span_px * 0.5) / v.px_per_day;
    const double factor = std::pow(1.12, static_cast<double>(io.MouseWheel));
    v.px_per_day *= factor;
    clamp_timeline_view(v);
    v.origin_day = center_day - (span_px * 0.5) / v.px_per_day;
    clamp_timeline_view(v);
  }

  // Middle-mouse drag = pan.
  if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
    const ImVec2 d = io.MouseDelta;
    if (std::fabs(d.x) > 1e-6f) {
      v.origin_day -= static_cast<double>(d.x) / v.px_per_day;
      clamp_timeline_view(v);
    }
  }
}

int schedule_end_day(const std::vector<ShipyardScheduleItem>& v) {
  int end = 1;
  for (const auto& it : v) end = std::max(end, std::max(it.end_day, it.full_end_day));
  return end;
}

int schedule_end_day(const std::vector<ConstructionScheduleItem>& v) {
  int end = 1;
  for (const auto& it : v) end = std::max(end, it.end_day);
  return end;
}

} // namespace

void draw_production_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  ImGui::SetNextWindowSize(ImVec2(980, 640), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Production", &ui.show_production_window)) {
    ImGui::End();
    return;
  }

  const GameState& s = sim.state();

  // Left: colony picker
  static char colony_filter[64] = {};

  ImGui::BeginChild("prod_left", ImVec2(300, 0), true);
  ImGui::TextDisabled("Colonies");
  ImGui::InputTextWithHint("##col_filter", "Filter...", colony_filter, IM_ARRAYSIZE(colony_filter));

  const std::string filter = nebula4x::to_lower(std::string(colony_filter));

  // Sorted colony ids.
  std::vector<Id> colonies;
  colonies.reserve(s.colonies.size());
  for (const auto& [cid, _] : s.colonies) colonies.push_back(cid);
  std::sort(colonies.begin(), colonies.end(), [&](Id a, Id b) {
    const Colony* ca = nebula4x::find_ptr(s.colonies, a);
    const Colony* cb = nebula4x::find_ptr(s.colonies, b);
    const std::string na = ca ? nebula4x::to_lower(ca->name) : "";
    const std::string nb = cb ? nebula4x::to_lower(cb->name) : "";
    if (na != nb) return na < nb;
    return a < b;
  });

  // Quick stats for shipyard capacity.
  const auto it_shipyard_def = sim.content().installations.find("shipyard");
  const InstallationDef* shipyard_def = (it_shipyard_def != sim.content().installations.end()) ? &it_shipyard_def->second : nullptr;

  for (Id cid : colonies) {
    const Colony* c = nebula4x::find_ptr(s.colonies, cid);
    if (!c) continue;
    const std::string nm_l = nebula4x::to_lower(c->name);
    if (!filter.empty() && nm_l.find(filter) == std::string::npos) continue;

    const bool sel = (selected_colony == cid);

    // One-line summary.
    const double cp = sim.construction_points_per_day(*c);
    const double tpd = shipyard_build_rate_tpd(sim, *c);
    const int qy = static_cast<int>(c->shipyard_queue.size());
    const int qc = static_cast<int>(c->construction_queue.size());

    std::string label = c->name;
    label += "##col_" + std::to_string(static_cast<unsigned long long>(cid));

    if (ImGui::Selectable(label.c_str(), sel)) {
      selected_colony = cid;
      selected_body = c->body_id;
      ui.show_details_window = true;
      ui.request_details_tab = DetailsTab::Colony;
    }
    if (ImGui::IsItemHovered()) {
      ImGui::BeginTooltip();
      ImGui::Text("%s", c->name.c_str());
      ImGui::Separator();
      ImGui::Text("Construction: %.0f CP/day", cp);
      if (tpd > 1e-9) {
        ImGui::Text("Shipyard: %.1f tons/day", tpd);
        if (shipyard_def && !shipyard_def->build_costs_per_ton.empty()) {
          ImGui::TextDisabled("(Mineral-limited build rate possible)");
        }
      } else {
        ImGui::TextDisabled("Shipyard: none");
      }
      ImGui::Text("Queues: shipyard %d, construction %d", qy, qc);
      ImGui::EndTooltip();
    }
  }

  ImGui::EndChild();
  ImGui::SameLine();

  // Right: timeline for selected colony
  ImGui::BeginChild("prod_right", ImVec2(0, 0), false);

  if (selected_colony == kInvalidId || nebula4x::find_ptr(s.colonies, selected_colony) == nullptr) {
    ImGui::TextDisabled("Select a colony to view its production schedule.");
    ImGui::EndChild();
    ImGui::End();
    return;
  }

  const Colony& colony = s.colonies.at(selected_colony);

  ImGui::Text("%s", colony.name.c_str());
  ImGui::SameLine();
  ImGui::TextDisabled("(%s)", s.date.to_string().c_str());

  ImGui::Separator();

  static TimelineView shipyard_view;
  static TimelineView construction_view;
  static AxisLayout layout;

  if (ImGui::BeginTabBar("prod_tabs")) {
    // ---- Shipyard tab ----
    if (ImGui::BeginTabItem("Shipyard")) {
      const double rate = shipyard_build_rate_tpd(sim, colony);
      if (rate <= 1e-9) {
        ImGui::TextDisabled("No shipyard capacity at this colony.");
        ImGui::EndTabItem();
      } else {
        const auto sched = estimate_shipyard_schedule(sim, colony);
        const int end_day = schedule_end_day(sched);

        const float spacing_x = ImGui::GetStyle().ItemSpacing.x;
        const float full_w = ImGui::GetContentRegionAvail().x;
        const float timeline_w = std::max(1.0f, full_w - layout.label_w - spacing_x);

        // Controls
        ImGui::TextDisabled("Ctrl+Wheel to zoom, Middle-drag to pan");
        ImGui::SameLine();
        if (ImGui::SmallButton("Fit##shipyard_fit")) {
          shipyard_view.origin_day = 0.0;
          shipyard_view.px_per_day = (end_day > 0) ? (std::max(1.0f, timeline_w) / static_cast<double>(end_day)) : 18.0;
          clamp_timeline_view(shipyard_view);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##shipyard_reset")) {
          shipyard_view = TimelineView{};
        }
        ImGui::SameLine();
        ImGui::Checkbox("Dates##shipyard_dates", &shipyard_view.show_dates);
        ImGui::SameLine();
        ImGui::Checkbox("Grid##shipyard_grid", &shipyard_view.show_grid);

        ImGui::Separator();

        // Axis row (label spacer + axis).
        ImGui::Dummy(ImVec2(layout.label_w, layout.axis_h));
        ImGui::SameLine();
        const ImVec2 axis_p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##shipyard_axis", ImVec2(timeline_w, layout.axis_h));
        draw_timeline_axis(ImGui::GetWindowDrawList(), axis_p0, ImVec2(timeline_w, layout.axis_h), shipyard_view, s.date, shipyard_view.show_dates);

        // Handle interactions on the right panel window (pan/zoom).
        handle_timeline_interactions(shipyard_view, timeline_w);

        // Timeline table.
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;

        const float table_h = std::max(120.0f, ImGui::GetContentRegionAvail().y);
        if (ImGui::BeginTable("shipyard_timeline", 2, flags, ImVec2(0, table_h))) {
          ImGui::TableSetupColumn("Order", ImGuiTableColumnFlags_WidthFixed, layout.label_w);
          ImGui::TableSetupColumn("Timeline", ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableHeadersRow();

          int delete_idx = -1;
          int move_from = -1;
          int move_to = -1;

          for (const auto& it : sched) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            {
              std::string row_label = it.label + "##sy_row_" + std::to_string(it.index);
              ImGui::Selectable(row_label.c_str(), false, ImGuiSelectableFlags_SpanAllColumns);

              // Context menu: move/delete.
              if (ImGui::BeginPopupContextItem()) {
                const int n = static_cast<int>(colony.shipyard_queue.size());
                if (ImGui::MenuItem("Focus")) {
                  ui.show_details_window = true;
                  ui.request_details_tab = it.is_refit ? DetailsTab::Ship : DetailsTab::Design;
                  if (!it.is_refit) ui.request_focus_design_id = it.design_id;
                  if (it.is_refit) selected_ship = it.refit_ship_id;
                }
                if (!it.design_id.empty()) {
                  if (ImGui::MenuItem("Open in Design Studio")) {
                    ui.show_design_studio_window = true;
                    ui.request_focus_design_studio_id = it.design_id;
                  }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Move up", nullptr, false, it.index > 0)) {
                  move_from = it.index;
                  move_to = it.index - 1;
                }
                if (ImGui::MenuItem("Move down", nullptr, false, it.index + 1 < n)) {
                  move_from = it.index;
                  move_to = it.index + 1;
                }
                if (ImGui::MenuItem("Move to top", nullptr, false, it.index > 0)) {
                  move_from = it.index;
                  move_to = 0;
                }
                if (ImGui::MenuItem("Move to bottom", nullptr, false, it.index + 1 < n)) {
                  move_from = it.index;
                  move_to = n;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Delete")) {
                  delete_idx = it.index;
                }
                ImGui::EndPopup();
              }
            }

            ImGui::TableSetColumnIndex(1);
            {
              const ImVec2 cell_p0 = ImGui::GetCursorScreenPos();
              const float cell_w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
              const ImVec2 cell_size(cell_w, layout.row_h);
              ImGui::InvisibleButton(("##sy_bar_" + std::to_string(it.index)).c_str(), cell_size);
              const bool hovered = ImGui::IsItemHovered();

              ImDrawList* dl = ImGui::GetWindowDrawList();
              const ImU32 fill = it.is_refit ? ImGui::GetColorU32(ImGuiCol_PlotHistogram) : ImGui::GetColorU32(ImGuiCol_PlotLines);
              const ImU32 border = with_alpha(ImGui::GetColorU32(ImGuiCol_Border), 0.9f);

              const int start = it.start_day;
              const int end = std::max(it.end_day, it.start_day + 1);
              const int full_end = std::max(it.full_end_day, it.start_day + 1);
              draw_bar(dl, cell_p0, cell_size, shipyard_view, start, end, full_end, fill, border, it.stalled, it.stalled);

              if (shipyard_view.show_grid) {
                // Minor vertical grid every N days (derived from px scale).
                const double span_days = static_cast<double>(cell_size.x) / shipyard_view.px_per_day;
                const double step = std::max(1.0, nice_number_125(span_days / 10.0));
                const double left_day = shipyard_view.origin_day;
                const double right_day = shipyard_view.origin_day + span_days;
                const double first = std::floor(left_day / step) * step;
                const ImU32 grid = with_alpha(ImGui::GetColorU32(ImGuiCol_TextDisabled), 0.10f);
                for (double d = first; d <= right_day + step; d += step) {
                  const float x = cell_p0.x + static_cast<float>((d - shipyard_view.origin_day) * shipyard_view.px_per_day);
                  if (x < cell_p0.x || x > cell_p0.x + cell_size.x) continue;
                  dl->AddLine(ImVec2(x, cell_p0.y), ImVec2(x, cell_p0.y + cell_size.y), grid);
                }
              }

              if (hovered) {
                ImGui::BeginTooltip();
                ImGui::Text("%s", it.label.c_str());
                ImGui::Separator();
                ImGui::Text("Remaining: %.1f tons", it.start_remaining_tons);
                if (it.build_rate_tpd > 1e-9) {
                  ImGui::Text("Capacity: %.1f tons/day", it.build_rate_tpd);
                  ImGui::Text("Ideal ETA: %d days", std::max(0, it.full_end_day - it.start_day));
                }
                const Date done = s.date.add_days(it.full_end_day);
                ImGui::Text("Ideal completion: %s", done.to_string().c_str());
                if (it.stalled) {
                  ImGui::Separator();
                  ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "STALLED: %s", it.stall_reason.c_str());
                }
                ImGui::EndTooltip();
              }

              // Click to focus.
              if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                ui.show_details_window = true;
                if (it.is_refit) {
                  selected_ship = it.refit_ship_id;
                  ui.request_details_tab = DetailsTab::Ship;
                } else {
                  ui.request_details_tab = DetailsTab::Design;
                  ui.request_focus_design_id = it.design_id;
                }
              }
            }
          }

          if (delete_idx >= 0) {
            sim.delete_shipyard_order(colony.id, delete_idx);
          }
          if (move_from >= 0 && move_to >= 0) {
            sim.move_shipyard_order(colony.id, move_from, move_to);
          }

          ImGui::EndTable();
        }

        ImGui::EndTabItem();
      }
    }

    // ---- Construction tab ----
    if (ImGui::BeginTabItem("Construction")) {
      const double cp = sim.construction_points_per_day(colony);
      if (cp <= 1e-9) {
        ImGui::TextDisabled("No construction capacity at this colony.");
        ImGui::EndTabItem();
      } else {
        const auto sched = estimate_construction_schedule(sim, colony);
        const int end_day = schedule_end_day(sched);

        const float spacing_x = ImGui::GetStyle().ItemSpacing.x;
        const float full_w = ImGui::GetContentRegionAvail().x;
        const float timeline_w = std::max(1.0f, full_w - layout.label_w - spacing_x);

        ImGui::TextDisabled("Ctrl+Wheel to zoom, Middle-drag to pan");
        ImGui::SameLine();
        if (ImGui::SmallButton("Fit##con_fit")) {
          construction_view.origin_day = 0.0;
          construction_view.px_per_day = (end_day > 0) ? (std::max(1.0f, timeline_w) / static_cast<double>(end_day)) : 18.0;
          clamp_timeline_view(construction_view);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##con_reset")) {
          construction_view = TimelineView{};
        }
        ImGui::SameLine();
        ImGui::Checkbox("Dates##con_dates", &construction_view.show_dates);
        ImGui::SameLine();
        ImGui::Checkbox("Grid##con_grid", &construction_view.show_grid);

        ImGui::Separator();

        // Axis row.
        ImGui::Dummy(ImVec2(layout.label_w, layout.axis_h));
        ImGui::SameLine();
        const ImVec2 axis_p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##con_axis", ImVec2(timeline_w, layout.axis_h));
        draw_timeline_axis(ImGui::GetWindowDrawList(), axis_p0, ImVec2(timeline_w, layout.axis_h), construction_view, s.date, construction_view.show_dates);

        handle_timeline_interactions(construction_view, timeline_w);

        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;

        const float table_h = std::max(120.0f, ImGui::GetContentRegionAvail().y);
        if (ImGui::BeginTable("construction_timeline", 2, flags, ImVec2(0, table_h))) {
          ImGui::TableSetupColumn("Order", ImGuiTableColumnFlags_WidthFixed, layout.label_w);
          ImGui::TableSetupColumn("Timeline", ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableHeadersRow();

          int delete_idx = -1;
          int move_from = -1;
          int move_to = -1;

          for (const auto& it : sched) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            {
              std::string row_label = it.label + " x" + std::to_string(it.total_units) + "##con_row_" + std::to_string(it.index);
              ImGui::Selectable(row_label.c_str(), false, ImGuiSelectableFlags_SpanAllColumns);

              if (ImGui::BeginPopupContextItem()) {
                const int n = static_cast<int>(colony.construction_queue.size());
                if (ImGui::MenuItem("Focus colony")) {
                  ui.show_details_window = true;
                  ui.request_details_tab = DetailsTab::Colony;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Move up", nullptr, false, it.index > 0)) {
                  move_from = it.index;
                  move_to = it.index - 1;
                }
                if (ImGui::MenuItem("Move down", nullptr, false, it.index + 1 < n)) {
                  move_from = it.index;
                  move_to = it.index + 1;
                }
                if (ImGui::MenuItem("Move to top", nullptr, false, it.index > 0)) {
                  move_from = it.index;
                  move_to = 0;
                }
                if (ImGui::MenuItem("Move to bottom", nullptr, false, it.index + 1 < n)) {
                  move_from = it.index;
                  move_to = n;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Delete (refund minerals if paid)")) {
                  delete_idx = it.index;
                }
                ImGui::EndPopup();
              }
            }

            ImGui::TableSetColumnIndex(1);
            {
              const ImVec2 cell_p0 = ImGui::GetCursorScreenPos();
              const float cell_w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
              const ImVec2 cell_size(cell_w, layout.row_h);
              ImGui::InvisibleButton(("##con_bar_" + std::to_string(it.index)).c_str(), cell_size);
              const bool hovered = ImGui::IsItemHovered();

              ImDrawList* dl = ImGui::GetWindowDrawList();
              const ImU32 fill = ImGui::GetColorU32(ImGuiCol_PlotHistogram);
              const ImU32 border = with_alpha(ImGui::GetColorU32(ImGuiCol_Border), 0.9f);

              const int start = it.start_day;
              const int end = std::max(it.end_day, it.start_day + 1);
              draw_bar(dl, cell_p0, cell_size, construction_view, start, end, end, fill, border, it.stalled, false);

              if (construction_view.show_grid) {
                const double span_days = static_cast<double>(cell_size.x) / construction_view.px_per_day;
                const double step = std::max(1.0, nice_number_125(span_days / 10.0));
                const double left_day = construction_view.origin_day;
                const double right_day = construction_view.origin_day + span_days;
                const double first = std::floor(left_day / step) * step;
                const ImU32 grid = with_alpha(ImGui::GetColorU32(ImGuiCol_TextDisabled), 0.10f);
                for (double d = first; d <= right_day + step; d += step) {
                  const float x = cell_p0.x + static_cast<float>((d - construction_view.origin_day) * construction_view.px_per_day);
                  if (x < cell_p0.x || x > cell_p0.x + cell_size.x) continue;
                  dl->AddLine(ImVec2(x, cell_p0.y), ImVec2(x, cell_p0.y + cell_size.y), grid);
                }
              }

              if (hovered) {
                ImGui::BeginTooltip();
                ImGui::Text("%s", it.label.c_str());
                ImGui::Separator();
                ImGui::Text("Construction: %.0f CP/day", it.cp_per_day);
                ImGui::Text("Completed: %d / %d", it.completed_units, it.total_units);
                if (it.end_day > it.start_day) {
                  ImGui::Text("Work span: %d days", it.end_day - it.start_day);
                  const Date done = s.date.add_days(it.end_day);
                  ImGui::Text("Estimated last completion: %s", done.to_string().c_str());
                }
                if (it.stalled) {
                  ImGui::Separator();
                  ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "STALLED: %s", it.stall_reason.c_str());
                }
                ImGui::EndTooltip();
              }
            }
          }

          if (delete_idx >= 0) {
            sim.delete_construction_order(colony.id, delete_idx, true);
          }
          if (move_from >= 0 && move_to >= 0) {
            sim.move_construction_order(colony.id, move_from, move_to);
          }

          ImGui::EndTable();
        }

        ImGui::EndTabItem();
      }
    }

    // ---- Summary tab ----
    if (ImGui::BeginTabItem("Summary")) {
      ImGui::TextDisabled("At-a-glance production stats (current colony)");
      ImGui::Separator();

      const double cp = sim.construction_points_per_day(colony);
      const double tpd = shipyard_build_rate_tpd(sim, colony);

      ImGui::Text("Construction: %.0f CP/day", cp);
      if (tpd > 1e-9) {
        ImGui::Text("Shipyard: %.1f tons/day", tpd);
      } else {
        ImGui::TextDisabled("Shipyard: none");
      }
      ImGui::Text("Queues: shipyard %d, construction %d",
                  static_cast<int>(colony.shipyard_queue.size()),
                  static_cast<int>(colony.construction_queue.size()));

      ImGui::Separator();

      if (ImGui::Button("Open Colony Details")) {
        ui.show_details_window = true;
        ui.request_details_tab = DetailsTab::Colony;
      }
      ImGui::SameLine();
      if (ImGui::Button("Open Event Log")) {
        ui.show_details_window = true;
        ui.request_details_tab = DetailsTab::Log;
      }

      ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
  }

  ImGui::EndChild();
  ImGui::End();
}

} // namespace nebula4x::ui
