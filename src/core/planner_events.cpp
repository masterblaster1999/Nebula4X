#include "nebula4x/core/planner_events.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "nebula4x/core/colony_schedule.h"
#include "nebula4x/core/date.h"
#include "nebula4x/core/game_state.h"
#include "nebula4x/core/order_planner.h"
#include "nebula4x/core/ground_battle_forecast.h"
#include "nebula4x/core/research_schedule.h"
#include "nebula4x/core/simulation.h"

namespace nebula4x {

namespace {

struct DayHour {
  std::int64_t day{0};
  int hour{0};
};

DayHour split_day_hour(double abs_days) {
  if (!std::isfinite(abs_days)) return {};

  double fday = std::floor(abs_days);
  double frac = abs_days - fday;
  if (frac < 0.0) frac = 0.0;

  std::int64_t day = static_cast<std::int64_t>(fday);
  int hour = static_cast<int>(std::floor(frac * 24.0 + 1e-9));
  hour = std::clamp(hour, 0, 23);
  return {day, hour};
}

// Convert a relative ETA into an absolute (day,hour) using the simulation's
// current time-of-day.
DayHour eta_to_day_hour(const Simulation& sim, double eta_days) {
  const auto& st = sim.state();
  const double now = static_cast<double>(st.date.days_since_epoch()) + static_cast<double>(std::clamp(st.hour_of_day, 0, 23)) / 24.0;
  return split_day_hour(now + std::max(0.0, eta_days));
}

double day_hour_to_abs(std::int64_t day, int hour) {
  return static_cast<double>(day) + static_cast<double>(std::clamp(hour, 0, 23)) / 24.0;
}

std::string tech_display_name(const Simulation& sim, const std::string& tech_id) {
  auto it = sim.content().techs.find(tech_id);
  if (it == sim.content().techs.end()) return tech_id;
  if (it->second.name.empty()) return tech_id;
  return it->second.name;
}

EventCategory categorize_order(const Order& o) {
  // Coarse mapping used only for planner grouping.
  return std::visit(
      [](const auto& ord) -> EventCategory {
        using T = std::decay_t<decltype(ord)>;
        if constexpr (std::is_same_v<T, AttackShip> || std::is_same_v<T, BombardColony> || std::is_same_v<T, InvadeColony>) {
          return EventCategory::Combat;
        } else if constexpr (std::is_same_v<T, MoveToPoint> || std::is_same_v<T, MoveToBody> || std::is_same_v<T, OrbitBody> ||
                             std::is_same_v<T, TravelViaJump> || std::is_same_v<T, EscortShip> || std::is_same_v<T, WaitDays>) {
          return EventCategory::Movement;
        } else {
          return EventCategory::General;
        }
      },
      o);
}

bool push_bounded(std::vector<PlannerEvent>& out,
                  const PlannerEventsOptions& opt,
                  PlannerEvent ev,
                  bool& truncated_out,
                  std::string& trunc_reason_out) {
  // Filter by horizon.
  if (ev.eta_days < -1e-9) return true;
  if (ev.eta_days > static_cast<double>(std::max(0, opt.max_days)) + 1e-9) return true;

  if (static_cast<int>(out.size()) >= std::max(0, opt.max_items)) {
    truncated_out = true;
    if (trunc_reason_out.empty()) trunc_reason_out = "Exceeded max_items";
    return false;
  }

  out.push_back(std::move(ev));
  return true;
}

} // namespace

PlannerEventsResult compute_planner_events(const Simulation& sim, Id faction_id, const PlannerEventsOptions& opt) {
  PlannerEventsResult res;
  res.ok = false;

  const auto* fac = find_ptr(sim.state().factions, faction_id);
  if (!fac) return res;

  std::vector<PlannerEvent> items;
  bool truncated = false;
  std::string trunc_reason;

  // --- Research ---
  if (opt.include_research) {
    ResearchScheduleOptions ro;
    ro.max_days = std::max(0, opt.max_days);
    ro.max_items = std::max(0, opt.max_items);

    const ResearchSchedule sched = estimate_research_schedule(sim, faction_id, ro);
    if (sched.ok) {
      for (const auto& it : sched.items) {
        PlannerEvent ev;
        ev.eta_days = static_cast<double>(std::max(0, it.end_day));
        const auto dh = eta_to_day_hour(sim, ev.eta_days);
        ev.day = dh.day;
        ev.hour = dh.hour;
        ev.level = EventLevel::Info;
        ev.category = EventCategory::Research;
        ev.faction_id = faction_id;

        const std::string name = tech_display_name(sim, it.tech_id);
        ev.title = "Research complete: " + name;
        ev.detail = "tech_id=" + it.tech_id;
        if (it.was_active_at_start) ev.detail += " [A]";

        if (!push_bounded(items, opt, std::move(ev), truncated, trunc_reason)) break;
      }

      if (sched.stalled) {
        PlannerEvent ev;
        ev.eta_days = 0.0;
        const auto dh = eta_to_day_hour(sim, 0.0);
        ev.day = dh.day;
        ev.hour = dh.hour;
        ev.level = EventLevel::Warn;
        ev.category = EventCategory::Research;
        ev.faction_id = faction_id;
        ev.title = "Research forecast stalled";
        ev.detail = sched.stall_reason;
        (void)push_bounded(items, opt, std::move(ev), truncated, trunc_reason);
      }
      if (sched.truncated) {
        PlannerEvent ev;
        ev.eta_days = 0.0;
        const auto dh = eta_to_day_hour(sim, 0.0);
        ev.day = dh.day;
        ev.hour = dh.hour;
        ev.level = EventLevel::Warn;
        ev.category = EventCategory::Research;
        ev.faction_id = faction_id;
        ev.title = "Research forecast truncated";
        ev.detail = sched.truncated_reason;
        (void)push_bounded(items, opt, std::move(ev), truncated, trunc_reason);
      }
    } else {
      PlannerEvent ev;
      ev.eta_days = 0.0;
      const auto dh = eta_to_day_hour(sim, 0.0);
      ev.day = dh.day;
      ev.hour = dh.hour;
      ev.level = EventLevel::Warn;
      ev.category = EventCategory::Research;
      ev.faction_id = faction_id;
      ev.title = "Research forecast unavailable";
      ev.detail = "estimate_research_schedule returned ok=false";
      (void)push_bounded(items, opt, std::move(ev), truncated, trunc_reason);
    }
  }

  // --- Colony production ---
  if (opt.include_colonies) {
    // Deterministic iteration order.
    std::vector<Id> colony_ids;
    colony_ids.reserve(sim.state().colonies.size());
    for (const auto& [cid, c] : sim.state().colonies) {
      if (c.faction_id == faction_id) colony_ids.push_back(cid);
    }
    std::sort(colony_ids.begin(), colony_ids.end());

    ColonyScheduleOptions co;
    co.max_days = std::max(0, opt.max_days);
    co.max_events = std::max(0, opt.max_items);

    for (Id cid : colony_ids) {
      const auto* colony = find_ptr(sim.state().colonies, cid);
      if (!colony) continue;

      const ColonySchedule sched = estimate_colony_schedule(sim, cid, co);
      if (sched.ok) {
        for (const auto& ev0 : sched.events) {
          if (ev0.day < 0) continue;

          PlannerEvent ev;
          ev.eta_days = static_cast<double>(std::max(0, ev0.day));
          const auto dh = eta_to_day_hour(sim, ev.eta_days);
          ev.day = dh.day;
          ev.hour = dh.hour;
          ev.level = EventLevel::Info;
          ev.faction_id = faction_id;
          ev.colony_id = cid;
          if (colony->body_id != kInvalidId) {
            if (const auto* body = find_ptr(sim.state().bodies, colony->body_id)) {
              ev.system_id = body->system_id;
            }
          }

          switch (ev0.kind) {
            case ColonyScheduleEventKind::ShipyardComplete:
              ev.category = EventCategory::Shipyard;
              break;
            case ColonyScheduleEventKind::ConstructionComplete:
              ev.category = EventCategory::Construction;
              break;
            default:
              ev.category = EventCategory::General;
              break;
          }

          ev.title = colony->name.empty() ? "Colony" : colony->name;
          ev.title += ": " + ev0.title;
          ev.detail = ev0.detail;
          if (ev0.auto_queued) {
            if (!ev.detail.empty()) ev.detail += " ";
            ev.detail += "(auto)";
          }

          if (!push_bounded(items, opt, std::move(ev), truncated, trunc_reason)) break;
        }

        if (sched.stalled) {
          PlannerEvent ev;
          ev.eta_days = 0.0;
          const auto dh = eta_to_day_hour(sim, 0.0);
          ev.day = dh.day;
          ev.hour = dh.hour;
          ev.level = EventLevel::Warn;
          ev.category = EventCategory::Construction;
          ev.faction_id = faction_id;
          ev.colony_id = cid;
          ev.title = (colony->name.empty() ? "Colony" : colony->name) + ": forecast stalled";
          ev.detail = sched.stall_reason;
          (void)push_bounded(items, opt, std::move(ev), truncated, trunc_reason);
        }
        if (sched.truncated) {
          PlannerEvent ev;
          ev.eta_days = 0.0;
          const auto dh = eta_to_day_hour(sim, 0.0);
          ev.day = dh.day;
          ev.hour = dh.hour;
          ev.level = EventLevel::Warn;
          ev.category = EventCategory::Construction;
          ev.faction_id = faction_id;
          ev.colony_id = cid;
          ev.title = (colony->name.empty() ? "Colony" : colony->name) + ": forecast truncated";
          ev.detail = sched.truncated_reason;
          (void)push_bounded(items, opt, std::move(ev), truncated, trunc_reason);
        }
      }

      if (truncated) break;
    }
  }

  // --- Ship orders (optional; can be expensive) ---
  if (opt.include_ships) {
    std::vector<Id> ship_ids;
    ship_ids.reserve(sim.state().ships.size());
    for (const auto& [sid, s] : sim.state().ships) {
      if (s.faction_id == faction_id) ship_ids.push_back(sid);
    }
    std::sort(ship_ids.begin(), ship_ids.end());
    if (static_cast<int>(ship_ids.size()) > std::max(0, opt.max_ships)) {
      ship_ids.resize(std::max(0, opt.max_ships));
      truncated = true;
      if (trunc_reason.empty()) trunc_reason = "Exceeded max_ships";
    }

    OrderPlannerOptions po;
    po.max_orders = std::max(0, opt.max_orders_per_ship);

    for (Id sid : ship_ids) {
      const auto* ship = find_ptr(sim.state().ships, sid);
      if (!ship) continue;

      const auto* ords = find_ptr(sim.state().ship_orders, sid);
      if (!ords || ords->queue.empty()) continue;

      const OrderPlan plan = compute_order_plan(sim, sid, po);
      if (!plan.ok) continue;
      if (plan.steps.empty()) continue;

      const Order& first_order = ords->queue.front();
      if (opt.include_ship_next_step) {
        PlannerEvent ev;
        ev.eta_days = std::max(0.0, plan.steps.front().eta_days);
        const auto dh = eta_to_day_hour(sim, ev.eta_days);
        ev.day = dh.day;
        ev.hour = dh.hour;
        ev.level = EventLevel::Info;
        ev.category = categorize_order(first_order);
        ev.faction_id = faction_id;
        // Use the simulated end-of-step system for easier "jump to" navigation.
        ev.system_id = plan.steps.front().system_id;
        ev.ship_id = sid;
        ev.title = ship->name.empty() ? "Ship" : ship->name;
        ev.title += ": next";
        ev.detail = order_to_string(first_order);
        if (!plan.steps.front().note.empty()) {
          ev.detail += " | " + plan.steps.front().note;
        }
        if (!push_bounded(items, opt, std::move(ev), truncated, trunc_reason)) break;
      }

      if (opt.include_ship_queue_complete) {
        PlannerEvent ev;
        ev.eta_days = std::max(0.0, plan.total_eta_days);
        const auto dh = eta_to_day_hour(sim, ev.eta_days);
        ev.day = dh.day;
        ev.hour = dh.hour;
        ev.level = plan.truncated ? EventLevel::Warn : EventLevel::Info;
        ev.category = EventCategory::Movement;
        ev.faction_id = faction_id;
        ev.system_id = plan.steps.back().system_id;
        ev.ship_id = sid;
        ev.title = ship->name.empty() ? "Ship" : ship->name;
        if (plan.truncated) {
          ev.title += ": plan truncated";
          ev.detail = plan.truncated_reason;
        } else {
          ev.title += ": orders complete";
          ev.detail = "end_fuel=" + std::to_string(plan.end_fuel_tons);
        }
        if (!push_bounded(items, opt, std::move(ev), truncated, trunc_reason)) break;
      }

      if (truncated) break;
    }
  }

  // --- Ground battles (optional) ---
  if (opt.include_ground_battles) {
    std::vector<Id> battle_colony_ids;
    battle_colony_ids.reserve(sim.state().ground_battles.size());
    for (const auto& [cid, b] : sim.state().ground_battles) {
      const Colony* col = find_ptr(sim.state().colonies, cid);
      if (!col) continue;

      // Only show battles relevant to this faction: either we are the attacker,
      // or the battle is happening on one of our colonies.
      if (b.attacker_faction_id != faction_id && col->faction_id != faction_id) continue;
      battle_colony_ids.push_back(cid);
    }
    std::sort(battle_colony_ids.begin(), battle_colony_ids.end());

    for (Id cid : battle_colony_ids) {
      const Colony* col = find_ptr(sim.state().colonies, cid);
      const GroundBattle* b = find_ptr(sim.state().ground_battles, cid);
      if (!col || !b) continue;

      PlannerEvent ev;
      ev.category = EventCategory::Combat;
      ev.faction_id = faction_id;
      ev.colony_id = cid;
      ev.system_id = b->system_id;

      const bool you_attacker = (b->attacker_faction_id == faction_id);
      const bool you_defender = (col->faction_id == faction_id);

      const std::string colony_name = col->name.empty() ? ("Colony #" + std::to_string(cid)) : col->name;
      ev.title = you_attacker ? ("Invasion: " + colony_name) : ("Defense: " + colony_name);

      const double forts = sim.fortification_points(*col);
      const GroundBattleForecast fc = forecast_ground_battle(sim.cfg(), b->attacker_strength, b->defender_strength, forts);

      if (!fc.ok) {
        ev.level = EventLevel::Warn;
        ev.detail = "forecast unavailable";
        ev.eta_days = 0.0;
        const auto dh = eta_to_day_hour(sim, ev.eta_days);
        ev.day = dh.day;
        ev.hour = dh.hour;
        if (!push_bounded(items, opt, std::move(ev), truncated, trunc_reason)) break;
        if (truncated) break;
        continue;
      }

      ev.eta_days = std::max(0.0, static_cast<double>(fc.days_to_resolve));
      const auto dh = eta_to_day_hour(sim, ev.eta_days);
      ev.day = dh.day;
      ev.hour = dh.hour;

      // Elevate severity when the forecast is bad for the viewer.
      ev.level = EventLevel::Info;
      if (you_attacker && fc.winner == GroundBattleWinner::Defender) ev.level = EventLevel::Warn;
      if (you_defender && fc.winner == GroundBattleWinner::Attacker) ev.level = EventLevel::Error;

      if (fc.truncated) {
        ev.level = EventLevel::Warn;
        ev.title += ": forecast truncated";
        ev.detail = fc.truncated_reason;
      } else {
        const std::string winner = (fc.winner == GroundBattleWinner::Attacker) ? "attacker" : "defender";
        ev.detail = "Forecast: " + winner + " wins in ~" + std::to_string(fc.days_to_resolve) + "d";
        ev.detail += " | att=" + std::to_string(fc.attacker_end);
        ev.detail += " def=" + std::to_string(fc.defender_end);
        ev.detail += " | forts=" + std::to_string(fc.fort_points);
        ev.detail += " bonus=" + std::to_string(fc.defense_bonus);
      }

      if (!push_bounded(items, opt, std::move(ev), truncated, trunc_reason)) break;
      if (truncated) break;
    }
  }

  // Sort by absolute time, then by category, then stable tiebreakers.
  std::sort(items.begin(), items.end(), [](const PlannerEvent& a, const PlannerEvent& b) {
    const double ta = day_hour_to_abs(a.day, a.hour);
    const double tb = day_hour_to_abs(b.day, b.hour);
    if (ta != tb) return ta < tb;
    if (a.category != b.category) return static_cast<int>(a.category) < static_cast<int>(b.category);
    if (a.level != b.level) return static_cast<int>(a.level) > static_cast<int>(b.level); // error/warn first
    if (a.title != b.title) return a.title < b.title;
    if (a.ship_id != b.ship_id) return a.ship_id < b.ship_id;
    if (a.colony_id != b.colony_id) return a.colony_id < b.colony_id;
    return a.detail < b.detail;
  });

  // Enforce global max_items post-sort (keep earliest items).
  if (static_cast<int>(items.size()) > std::max(0, opt.max_items)) {
    items.resize(std::max(0, opt.max_items));
    truncated = true;
    if (trunc_reason.empty()) trunc_reason = "Exceeded max_items";
  }

  res.ok = true;
  res.truncated = truncated;
  res.truncated_reason = trunc_reason;
  res.items = std::move(items);
  return res;
}

} // namespace nebula4x
