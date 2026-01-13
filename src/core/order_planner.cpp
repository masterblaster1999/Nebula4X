#include "nebula4x/core/order_planner.h"

#include "nebula4x/core/simulation.h"

#include "simulation_sensors.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace nebula4x {

namespace {

constexpr double kTwoPi = 6.28318530717958647692;

double mkm_per_day_from_speed(double speed_km_s, double seconds_per_day) {
  // 1 mkm = 1e6 km.
  if (speed_km_s <= 0.0 || seconds_per_day <= 0.0) return 0.0;
  return (speed_km_s * seconds_per_day) / 1.0e6;
}

Vec2 compute_body_pos_at_time(const GameState& st, Id body_id, double t_days) {
  if (body_id == kInvalidId) return {0.0, 0.0};

  std::unordered_map<Id, Vec2> cache;
  cache.reserve(st.bodies.size() * 2 + 8);

  std::unordered_set<Id> visiting;
  visiting.reserve(st.bodies.size());

  const auto compute_pos = [&](Id id, const auto& self) -> Vec2 {
    if (id == kInvalidId) return {0.0, 0.0};
    if (auto it = cache.find(id); it != cache.end()) return it->second;
    if (visiting.count(id)) return {0.0, 0.0};
    visiting.insert(id);

    const Body* b = find_ptr(st.bodies, id);
    if (!b) {
      visiting.erase(id);
      return {0.0, 0.0};
    }

    const Vec2 center = self(b->parent_body_id, self);

    Vec2 pos = center;

    const double a = std::max(0.0, b->orbit_radius_mkm);
    const double period = std::max(0.0, b->orbit_period_days);
    const double e = std::clamp(b->orbit_eccentricity, 0.0, 0.999999);

    if (a > 1e-12 && period > 1e-12) {
      const double M0 = b->orbit_phase_radians;
      const double M = std::fmod(M0 + kTwoPi * (t_days / period), kTwoPi);

      if (e <= 1e-9) {
        const double ang = M + b->orbit_arg_periapsis_radians;
        pos = center + Vec2{a * std::cos(ang), a * std::sin(ang)};
      } else {
        // Solve Kepler's equation: M = E - e sin E (Newton iterations).
        double E = (e < 0.8) ? M : (kTwoPi * 0.5);
        for (int it = 0; it < 12; ++it) {
          const double sE = std::sin(E);
          const double cE = std::cos(E);
          const double f = (E - e * sE) - M;
          const double fp = 1.0 - e * cE;
          if (std::fabs(fp) < 1e-12) break;
          const double d = f / fp;
          E -= d;
          if (std::fabs(f) < 1e-10) break;
        }

        const double sE = std::sin(E);
        const double cE = std::cos(E);
        const double bsemi = a * std::sqrt(std::max(0.0, 1.0 - e * e));
        const double x = a * (cE - e);
        const double y = bsemi * sE;

        const double w = b->orbit_arg_periapsis_radians;
        const double cw = std::cos(w);
        const double sw = std::sin(w);
        const double rx = x * cw - y * sw;
        const double ry = x * sw + y * cw;

        pos = center + Vec2{rx, ry};
      }
    }

    cache[id] = pos;
    visiting.erase(id);
    return pos;
  };

  return compute_pos(body_id, compute_pos);
}

std::optional<Vec2> body_pos(const GameState& st, Id body_id, double t_days, bool predict_orbits) {
  const Body* b = find_ptr(st.bodies, body_id);
  if (!b) return std::nullopt;
  if (!predict_orbits) return b->position_mkm;
  return compute_body_pos_at_time(st, body_id, t_days);
}

[[maybe_unused]] std::optional<Vec2> colony_body_pos(const GameState& st, Id colony_id, double t_days, bool predict_orbits) {
  const Colony* c = find_ptr(st.colonies, colony_id);
  if (!c) return std::nullopt;
  return body_pos(st, c->body_id, t_days, predict_orbits);
}

// Returns (dt_days, target_pos_at_arrival).
std::pair<double, Vec2> intercept_body_dt(const GameState& st,
                                          Vec2 ship_pos,
                                          Id body_id,
                                          double t_days,
                                          double threshold_mkm,
                                          double mkm_per_day,
                                          bool predict_orbits) {
  if (mkm_per_day <= 0.0) return {std::numeric_limits<double>::infinity(), ship_pos};

  // Fixed-point: target = pos(body, t + dt(target))
  Vec2 target = ship_pos;
  if (auto p = body_pos(st, body_id, t_days, predict_orbits); p) target = *p;

  double dt = 0.0;
  for (int it = 0; it < 8; ++it) {
    const double dist = (target - ship_pos).length();
    const double cover = std::max(0.0, dist - std::max(0.0, threshold_mkm));
    const double dt_new = cover / mkm_per_day;

    const double t_arrive = t_days + dt_new;
    auto p2 = body_pos(st, body_id, t_arrive, predict_orbits);
    if (!p2) break;

    const double move = (*p2 - target).length();
    const double dt_diff = std::fabs(dt_new - dt);
    target = *p2;
    dt = dt_new;

    if (dt_diff < 1e-6 && move < 1e-3) break;
  }

  return {dt, target};
}

} // namespace

OrderPlan compute_order_plan(const Simulation& sim, Id ship_id, const OrderPlannerOptions& opts) {
  OrderPlan plan;

  const GameState& st = sim.state();
  const Ship* ship = find_ptr(st.ships, ship_id);
  if (!ship) {
    plan.ok = false;
    plan.truncated = true;
    plan.truncated_reason = "Ship not found";
    return plan;
  }

  const ShipDesign* sd = sim.find_design(ship->design_id);

  double speed_km_s = ship->speed_km_s;
  if (speed_km_s <= 1e-9) speed_km_s = sd ? sd->speed_km_s : 0.0;

  const double seconds_per_day = std::max(1.0, sim.cfg().seconds_per_day);
  const double arrive_eps = std::max(0.0, sim.cfg().arrival_epsilon_mkm);
  const double dock_range = std::max(arrive_eps, std::max(0.0, sim.cfg().docking_range_mkm));
  const double mkm_per_day = mkm_per_day_from_speed(speed_km_s, seconds_per_day);

  const double fuel_cap = sd ? std::max(0.0, sd->fuel_capacity_tons) : 0.0;
  const double fuel_use = sd ? std::max(0.0, sd->fuel_use_per_mkm) : 0.0;
  const bool uses_fuel = (fuel_use > 1e-12 && fuel_cap > 1e-9);

  double fuel = ship->fuel_tons;
  if (uses_fuel) {
    if (fuel < 0.0) fuel = fuel_cap;
    fuel = std::clamp(fuel, 0.0, fuel_cap);
  } else {
    fuel = std::max(0.0, fuel);
  }

  plan.start_fuel_tons = fuel;

  // Fractional day time.
  const double t0 =
      static_cast<double>(st.date.days_since_epoch()) + static_cast<double>(std::clamp(st.hour_of_day, 0, 23)) / 24.0;
  double t = t0;

  Vec2 pos = ship->position_mkm;
  Id sys = ship->system_id;

  // Local fuel availability snapshot for refuel simulation.
  std::unordered_map<Id, double> colony_fuel;
  colony_fuel.reserve(st.colonies.size() * 2 + 8);
  if (opts.simulate_refuel) {
    for (const auto& [cid, c] : st.colonies) {
      const auto it = c.minerals.find("Fuel");
      const double avail = (it != c.minerals.end()) ? std::max(0.0, it->second) : 0.0;
      colony_fuel[cid] = avail;
    }
  }

  auto maybe_refuel = [&](std::string& note_out) {
    if (!opts.simulate_refuel) return;
    if (!uses_fuel) return;
    if (fuel_cap <= 1e-9) return;

    const double need = std::max(0.0, fuel_cap - fuel);
    if (need <= 1e-9) return;

    // Find closest mutually-friendly colony in this system within dock range.
    Id best_cid = kInvalidId;
    double best_dist = 1e100;

    for (const auto& [cid, col] : st.colonies) {
      if (!sim.are_factions_mutual_friendly(ship->faction_id, col.faction_id)) continue;

      const Body* b = find_ptr(st.bodies, col.body_id);
      if (!b) continue;
      if (b->system_id != sys) continue;

      const Vec2 bpos = opts.predict_orbits ? compute_body_pos_at_time(st, b->id, t) : b->position_mkm;
      const double dist = (bpos - pos).length();
      if (dist > dock_range + 1e-9) continue;

      if (dist < best_dist) {
        best_dist = dist;
        best_cid = cid;
      }
    }

    if (best_cid == kInvalidId) return;

    const double avail = colony_fuel[best_cid];
    if (avail <= 1e-9) return;

    const double take = std::min(need, avail);
    if (take <= 1e-9) return;

    colony_fuel[best_cid] = avail - take;
    fuel += take;
    fuel = std::clamp(fuel, 0.0, fuel_cap);

    const Colony* c = find_ptr(st.colonies, best_cid);
    const std::string cname = c ? c->name : std::string("(colony)");
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(1);
    oss << "Refueled +" << take << "t at " << cname;
    note_out = oss.str();
  };

  // Fetch current queue.
  const auto it_orders = st.ship_orders.find(ship_id);
  if (it_orders == st.ship_orders.end()) {
    plan.ok = true;
    plan.end_fuel_tons = fuel;
    plan.total_eta_days = 0.0;
    return plan;
  }

  const ShipOrders& so = it_orders->second;
  const auto& q = so.queue;
  if (q.empty()) {
    plan.ok = true;
    plan.end_fuel_tons = fuel;
    plan.total_eta_days = 0.0;
    return plan;
  }

  plan.ok = true;
  plan.steps.reserve(std::min<std::size_t>(q.size(), static_cast<std::size_t>(std::max(0, opts.max_orders))));

  double eta = 0.0;

  const int max_orders = std::max(0, opts.max_orders);
  for (int i = 0; i < static_cast<int>(q.size()) && i < max_orders; ++i) {
    const Order& ord = q[static_cast<std::size_t>(i)];

    PlannedOrderStep step;
    step.system_id = sys;
    step.position_mkm = pos;

    std::string note;
    maybe_refuel(note);

    // fuel_before reflects any instantaneous refuel available at the current position.
    step.fuel_before_tons = fuel;

    const double eta_before = eta;
    double dt = 0.0;

    auto truncate = [&](const std::string& why, bool feasible) {
      step.feasible = feasible;
      if (!note.empty()) {
        step.note = note + "\n" + why;
      } else {
        step.note = why;
      }
      plan.truncated = true;
      plan.truncated_reason = why;
    };

    // Helper: travel to a fixed point in the current system.
    auto travel_to_point = [&](Vec2 target, double threshold) -> bool {
      if (sys == kInvalidId) {
        truncate("Ship is not in a valid system", false);
        return false;
      }
      if (mkm_per_day <= 0.0) {
        truncate("Ship has no effective speed", false);
        return false;
      }

      const double dist = (target - pos).length();
      const double cover = std::max(0.0, dist - std::max(0.0, threshold));
      dt += cover / mkm_per_day;

      if (uses_fuel) {
        const double burn = cover * fuel_use;
        if (burn > fuel + 1e-9) {
          std::ostringstream oss;
          oss.setf(std::ios::fixed);
          oss.precision(1);
          oss << "Insufficient fuel (" << fuel << "t) for travel burn (" << burn << "t)";
          truncate(oss.str(), false);
          return false;
        }
        fuel = std::max(0.0, fuel - burn);
      }

      // Snap to target for simplicity (simulation uses eps/dock-range anyway).
      pos = target;
      t += (cover / mkm_per_day);
      return true;
    };

    // Helper: travel to a body (possibly moving) and end at its position.
    auto travel_to_body = [&](Id body_id, double threshold) -> bool {
      const Body* b = find_ptr(st.bodies, body_id);
      if (!b) {
        truncate("Invalid body target", false);
        return false;
      }
      if (b->system_id != sys) {
        truncate("Target body is in a different system", false);
        return false;
      }
      if (mkm_per_day <= 0.0) {
        truncate("Ship has no effective speed", false);
        return false;
      }

      const auto [dt_move, target] = intercept_body_dt(st, pos, body_id, t, threshold, mkm_per_day, opts.predict_orbits);
      if (!std::isfinite(dt_move)) {
        truncate("Could not estimate intercept time", false);
        return false;
      }

      // Recompute cover for burn (approx: dt_move*mkm_per_day).
      const double cover = std::max(0.0, dt_move * mkm_per_day);

      dt += dt_move;

      if (uses_fuel) {
        const double burn = cover * fuel_use;
        if (burn > fuel + 1e-9) {
          std::ostringstream oss;
          oss.setf(std::ios::fixed);
          oss.precision(1);
          oss << "Insufficient fuel (" << fuel << "t) for travel burn (" << burn << "t)";
          truncate(oss.str(), false);
          return false;
        }
        fuel = std::max(0.0, fuel - burn);
      }

      pos = target;
      t += dt_move;
      return true;
    };

    // Helper: travel to a colony body.
    auto travel_to_colony = [&](Id colony_id, double threshold) -> bool {
      const Colony* c = find_ptr(st.colonies, colony_id);
      if (!c) {
        truncate("Invalid colony target", false);
        return false;
      }
      return travel_to_body(c->body_id, threshold);
    };

    // Helper: travel to a jump point.
    auto travel_to_jump = [&](Id jump_id, double threshold) -> bool {
      const JumpPoint* jp = find_ptr(st.jump_points, jump_id);
      if (!jp) {
        truncate("Invalid jump point target", false);
        return false;
      }
      if (jp->system_id != sys) {
        truncate("Jump point is in a different system", false);
        return false;
      }
      return travel_to_point(jp->position_mkm, threshold);
    };

    // Helper: travel to a ship (same system).
    auto travel_to_ship = [&](Id target_ship_id, double desired_range) -> bool {
      const Ship* tgt = find_ptr(st.ships, target_ship_id);
      if (!tgt) {
        truncate("Invalid ship target", false);
        return false;
      }
      if (tgt->system_id != sys) {
        truncate("Target ship is in a different system", false);
        return false;
      }
      return travel_to_point(tgt->position_mkm, std::max(0.0, desired_range));
    };

    // --- Order handling ---
    bool ok = true;
    bool terminal = false;
    bool indefinite = false;

    if (std::holds_alternative<WaitDays>(ord)) {
      const auto& o = std::get<WaitDays>(ord);
      const int d = std::max(0, o.days_remaining);
      dt += static_cast<double>(d);
      t += static_cast<double>(d);
    } else if (std::holds_alternative<MoveToPoint>(ord)) {
      const auto& o = std::get<MoveToPoint>(ord);
      ok = travel_to_point(o.target_mkm, arrive_eps);
    } else if (std::holds_alternative<MoveToBody>(ord)) {
      const auto& o = std::get<MoveToBody>(ord);
      ok = travel_to_body(o.body_id, dock_range);
    } else if (std::holds_alternative<TravelViaJump>(ord)) {
      const auto& o = std::get<TravelViaJump>(ord);
      ok = travel_to_jump(o.jump_point_id, dock_range);
      if (ok) {
        const JumpPoint* jp = find_ptr(st.jump_points, o.jump_point_id);
        if (!jp || jp->linked_jump_id == kInvalidId) {
          truncate("Jump point has no linked destination", false);
          ok = false;
        } else {
          const JumpPoint* dest = find_ptr(st.jump_points, jp->linked_jump_id);
          if (!dest) {
            truncate("Linked jump point not found", false);
            ok = false;
          } else {
            sys = dest->system_id;
            pos = dest->position_mkm;
            // No time cost to transit in the current simulation model.
          }
        }
      }
    } else if (std::holds_alternative<SurveyJumpPoint>(ord)) {
      const auto& o = std::get<SurveyJumpPoint>(ord);
      ok = travel_to_jump(o.jump_point_id, dock_range);
      if (ok) {
        const JumpPoint* jp = find_ptr(st.jump_points, o.jump_point_id);
        if (!jp) {
          truncate("SurveyJumpPoint references an invalid jump point", false);
          ok = false;
        } else {
          // Best-effort: estimate survey time from current progress + ship survey rate.
          const double required_points = sim.cfg().jump_survey_points_required;
          if (required_points <= 1e-9 || sim.is_jump_point_surveyed_by_faction(ship->faction_id, jp->id)) {
            // Instant / already surveyed.
          } else {
            const auto* fac = find_ptr(st.factions, ship->faction_id);
            double prog = 0.0;
            if (fac) {
              if (auto itp = fac->jump_survey_progress.find(jp->id); itp != fac->jump_survey_progress.end()) {
                prog = itp->second;
              }
            }
            if (!std::isfinite(prog) || prog < 0.0) prog = 0.0;

            const auto* des = sim.find_design(ship->design_id);
            if (!des) {
              truncate("No design data for survey ETA estimate", true);
            } else {
              const double env_mult = sim.system_sensor_environment_multiplier(jp->system_id);
              double sensor_mkm = sim_sensors::sensor_range_mkm_with_mode(sim, *ship, *des);
              sensor_mkm *= env_mult;
              const double ref_range = std::max(1e-9, sim.cfg().jump_survey_reference_sensor_range_mkm);
              const double role_mult = (des->role == ShipRole::Surveyor) ? sim.cfg().jump_survey_strength_multiplier_surveyor
                                                                         : sim.cfg().jump_survey_strength_multiplier_other;
              double points_per_day = (sensor_mkm / ref_range) * std::max(0.0, role_mult);
              const double cap = std::max(0.0, sim.cfg().jump_survey_points_per_day_cap);
              if (cap > 0.0) points_per_day = std::clamp(points_per_day, 0.0, cap);

              if (points_per_day <= 1e-12) {
                truncate("Survey rate is zero (no sensors?)", true);
                indefinite = true;
              } else {
                const double remaining = std::max(0.0, required_points - prog);
                const int days = static_cast<int>(std::ceil(remaining / points_per_day));
                dt += static_cast<double>(std::max(0, days));
                t += static_cast<double>(std::max(0, days));
              }
            }
          }

          if (o.transit_when_done) {
            if (jp->linked_jump_id == kInvalidId) {
              truncate("SurveyJumpPoint transit_when_done requires a linked jump", false);
              ok = false;
            } else if (const JumpPoint* dest = find_ptr(st.jump_points, jp->linked_jump_id)) {
              sys = dest->system_id;
              pos = dest->position_mkm;
            } else {
              truncate("SurveyJumpPoint has invalid destination", false);
              ok = false;
            }
          } else {
            pos = jp->position_mkm;
          }
        }
      }
    } else if (std::holds_alternative<InvestigateAnomaly>(ord)) {
      const auto& o = std::get<InvestigateAnomaly>(ord);
      const Anomaly* an = find_ptr(st.anomalies, o.anomaly_id);
      if (!an) {
        truncate("Invalid anomaly target", false);
        ok = false;
      } else if (an->resolved) {
        // Simulation will just skip this order, so planning treats it as a zero-cost no-op.
        if (note.empty()) {
          note = "Anomaly already resolved (order will be skipped)";
        } else {
          note += "\nAnomaly already resolved (order will be skipped)";
        }
        ok = true;
      } else {
        const auto* des = sim.find_design(ship->design_id);
        if (!des || des->sensor_range_mkm <= 1e-6) {
          truncate("Cannot investigate anomaly without sensors", false);
          ok = false;
        } else if (an->system_id != sys) {
          // Cross-system travel exists via TravelToSystem + TravelViaJump in simulation,
          // but the planner models only the ship's current queue, not automatic insertion.
          truncate("Anomaly is in a different system", false);
          ok = false;
        } else {
          const double investigation_dist = std::max(dock_range, std::max(0.0, des->sensor_range_mkm * 0.5));
          ok = travel_to_point(an->position_mkm, investigation_dist);
          if (ok) {
            int dur = o.duration_days;
            if (dur == 0) dur = std::max(0, an->investigation_days);
            const double prog = std::max(0.0, o.progress_days);
            const double remain = std::max(0.0, static_cast<double>(dur) - prog);
            dt += remain;
            t += remain;
          }
        }
      }
    } else if (std::holds_alternative<OrbitBody>(ord)) {
      const auto& o = std::get<OrbitBody>(ord);
      ok = travel_to_body(o.body_id, dock_range);
      if (ok) {
        if (o.duration_days < 0) {
          indefinite = true;
          truncate("OrbitBody has infinite duration", true);
        } else if (o.duration_days > 0) {
          dt += static_cast<double>(o.duration_days);
          t += static_cast<double>(o.duration_days);
        }
      }
    } else if (std::holds_alternative<ColonizeBody>(ord)) {
      const auto& o = std::get<ColonizeBody>(ord);
      ok = travel_to_body(o.body_id, dock_range);
      if (ok) {
        terminal = true;
        truncate("ColonizeBody consumes the ship (terminal)", true);
      }
    } else if (std::holds_alternative<ScrapShip>(ord)) {
      const auto& o = std::get<ScrapShip>(ord);
      ok = travel_to_colony(o.colony_id, dock_range);
      if (ok) {
        terminal = true;
        truncate("ScrapShip is terminal (ship removed)", true);
      }
    } else if (std::holds_alternative<LoadMineral>(ord)) {
      const auto& o = std::get<LoadMineral>(ord);
      ok = travel_to_colony(o.colony_id, dock_range);
    } else if (std::holds_alternative<UnloadMineral>(ord)) {
      const auto& o = std::get<UnloadMineral>(ord);
      ok = travel_to_colony(o.colony_id, dock_range);
    } else if (std::holds_alternative<LoadTroops>(ord)) {
      const auto& o = std::get<LoadTroops>(ord);
      ok = travel_to_colony(o.colony_id, dock_range);
    } else if (std::holds_alternative<UnloadTroops>(ord)) {
      const auto& o = std::get<UnloadTroops>(ord);
      ok = travel_to_colony(o.colony_id, dock_range);
    } else if (std::holds_alternative<LoadColonists>(ord)) {
      const auto& o = std::get<LoadColonists>(ord);
      ok = travel_to_colony(o.colony_id, dock_range);
    } else if (std::holds_alternative<UnloadColonists>(ord)) {
      const auto& o = std::get<UnloadColonists>(ord);
      ok = travel_to_colony(o.colony_id, dock_range);
    } else if (std::holds_alternative<InvadeColony>(ord)) {
      const auto& o = std::get<InvadeColony>(ord);
      ok = travel_to_colony(o.colony_id, dock_range);
      if (ok) {
        indefinite = true;
        truncate("InvadeColony outcome depends on combat resolution", true);
      }
    } else if (std::holds_alternative<BombardColony>(ord)) {
      const auto& o = std::get<BombardColony>(ord);
      const Colony* c = find_ptr(st.colonies, o.colony_id);
      if (!c) {
        truncate("Invalid colony target", false);
        ok = false;
      } else {
        const Body* b = find_ptr(st.bodies, c->body_id);
        if (!b || b->system_id != sys) {
          truncate("Target colony is in a different system", false);
          ok = false;
        } else {
          const double frac = std::clamp(sim.cfg().bombard_standoff_range_fraction, 0.0, 1.0);
          const double w_range = sd ? std::max(0.0, sd->weapon_range_mkm) : 0.0;
          const double desired = std::max(0.0, w_range * frac);
          ok = travel_to_body(b->id, desired);

          if (ok) {
            if (o.duration_days < 0) {
              indefinite = true;
              truncate("BombardColony has infinite duration", true);
            } else if (o.duration_days > 0) {
              dt += static_cast<double>(o.duration_days);
              t += static_cast<double>(o.duration_days);
            }
          }
        }
      }
    } else if (std::holds_alternative<SalvageWreck>(ord)) {
      const auto& o = std::get<SalvageWreck>(ord);
      const Wreck* w = find_ptr(st.wrecks, o.wreck_id);
      if (!w) {
        truncate("Invalid wreck target", false);
        ok = false;
      } else if (w->system_id != sys) {
        truncate("Wreck is in a different system", false);
        ok = false;
      } else {
        ok = travel_to_point(w->position_mkm, dock_range);
      }
    } else if (std::holds_alternative<TransferCargoToShip>(ord)) {
      const auto& o = std::get<TransferCargoToShip>(ord);
      ok = travel_to_ship(o.target_ship_id, dock_range);
    } else if (std::holds_alternative<TransferFuelToShip>(ord)) {
      const auto& o = std::get<TransferFuelToShip>(ord);
      ok = travel_to_ship(o.target_ship_id, dock_range);
    } else if (std::holds_alternative<TransferTroopsToShip>(ord)) {
      const auto& o = std::get<TransferTroopsToShip>(ord);
      ok = travel_to_ship(o.target_ship_id, dock_range);
    } else if (std::holds_alternative<AttackShip>(ord)) {
      const auto& o = std::get<AttackShip>(ord);
      const Ship* tgt = find_ptr(st.ships, o.target_ship_id);
      if (tgt && tgt->system_id == sys) {
        const double w_range = sd ? std::max(0.0, sd->weapon_range_mkm) : 0.0;
        const double desired = (w_range > 0.0) ? (w_range * 0.9) : 0.1;
        ok = travel_to_point(tgt->position_mkm, desired);
      } else if (o.has_last_known) {
        ok = travel_to_point(o.last_known_position_mkm, arrive_eps);
      } else {
        truncate("AttackShip has no target contact / last-known position", false);
        ok = false;
      }

      if (ok) {
        indefinite = true;
        truncate("AttackShip outcome depends on combat/contact", true);
      }
    } else if (std::holds_alternative<EscortShip>(ord)) {
      const auto& o = std::get<EscortShip>(ord);
      const Ship* tgt = find_ptr(st.ships, o.target_ship_id);
      if (!tgt) {
        truncate("Invalid escort target ship", false);
        ok = false;
      } else if (tgt->system_id != sys) {
        // Cross-system escort routing exists in simulation, but is complex to preview here.
        truncate("EscortShip cross-system routing is not previewed", true);
        indefinite = true;
        ok = true;
      } else {
        const double desired = std::max(dock_range, std::max(0.0, o.follow_distance_mkm));
        ok = travel_to_point(tgt->position_mkm, desired);
        indefinite = true;
        truncate("EscortShip is a continuous behaviour (no fixed completion)", true);
      }
    } else {
      truncate("Unsupported order type", true);
      ok = true;
      indefinite = true;
    }

    // Commit step.
    step.delta_days = std::max(0.0, dt);
    eta = eta_before + step.delta_days;
    step.eta_days = eta;

    step.fuel_after_tons = fuel;
    step.system_id = sys;
    step.position_mkm = pos;

    if (!step.note.empty() && !note.empty()) {
      // keep existing
    } else if (step.note.empty() && !note.empty()) {
      step.note = note;
    }

    plan.steps.push_back(step);

    if (!ok) {
      // truncate() already set fields
      break;
    }
    if (terminal || indefinite) {
      // truncate() already set summary, but keep going? no.
      break;
    }
  }

  plan.total_eta_days = eta;
  plan.end_fuel_tons = fuel;

  if (static_cast<int>(q.size()) > max_orders) {
    plan.truncated = true;
    plan.truncated_reason = "Order plan limit reached";
  }

  return plan;
}

} // namespace nebula4x
