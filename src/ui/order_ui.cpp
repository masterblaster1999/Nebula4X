#include "ui/order_ui.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <sstream>
#include <string>
#include <type_traits>

#include "nebula4x/core/game_state.h"
#include "nebula4x/core/simulation.h"

namespace nebula4x::ui {
namespace {

std::string id_fallback(const char* kind, Id id) {
  std::ostringstream ss;
  ss << kind << " #" << static_cast<unsigned long long>(id);
  return ss.str();
}

bool can_show_system_name(const Simulation& sim, Id viewer_faction_id, bool fog_of_war, Id system_id) {
  if (system_id == kInvalidId) return false;
  if (!fog_of_war) return true;
  if (viewer_faction_id == kInvalidId) return true;
  return sim.is_system_discovered_by_faction(viewer_faction_id, system_id);
}

bool can_show_ship_name(const Simulation& sim, Id viewer_faction_id, bool fog_of_war, Id ship_id) {
  if (ship_id == kInvalidId) return false;
  if (!fog_of_war) return true;
  if (viewer_faction_id == kInvalidId) return true;
  return sim.is_ship_detected_by_faction(viewer_faction_id, ship_id);
}

std::string system_label(const Simulation& sim, Id system_id, Id viewer_faction_id, bool fog_of_war) {
  const auto& s = sim.state();
  const auto* sys = find_ptr(s.systems, system_id);
  if (!sys) return id_fallback("System", system_id);
  if (!can_show_system_name(sim, viewer_faction_id, fog_of_war, system_id)) {
    return id_fallback("System", system_id);
  }
  if (!sys->name.empty()) return sys->name;
  return id_fallback("System", system_id);
}

std::string body_label(const Simulation& sim, Id body_id, Id viewer_faction_id, bool fog_of_war) {
  const auto& s = sim.state();
  const auto* b = find_ptr(s.bodies, body_id);
  if (!b) return id_fallback("Body", body_id);

  // If the body is in an undiscovered system, suppress its name.
  if (b->system_id != kInvalidId && !can_show_system_name(sim, viewer_faction_id, fog_of_war, b->system_id)) {
    return id_fallback("Body", body_id);
  }

  std::string nm = b->name.empty() ? id_fallback("Body", body_id) : b->name;
  if (b->system_id != kInvalidId && can_show_system_name(sim, viewer_faction_id, fog_of_war, b->system_id)) {
    nm += " (" + system_label(sim, b->system_id, viewer_faction_id, fog_of_war) + ")";
  }
  return nm;
}

std::string colony_label(const Simulation& sim, Id colony_id, Id viewer_faction_id, bool fog_of_war) {
  const auto& s = sim.state();
  const auto* c = find_ptr(s.colonies, colony_id);
  if (!c) return id_fallback("Colony", colony_id);

  // Gate colony names based on the owning body's system discovery.
  Id sys_id = kInvalidId;
  if (c->body_id != kInvalidId) {
    const auto* b = find_ptr(s.bodies, c->body_id);
    if (b) sys_id = b->system_id;
  }
  if (sys_id != kInvalidId && !can_show_system_name(sim, viewer_faction_id, fog_of_war, sys_id)) {
    return id_fallback("Colony", colony_id);
  }

  std::string nm = c->name.empty() ? id_fallback("Colony", colony_id) : c->name;
  if (sys_id != kInvalidId && can_show_system_name(sim, viewer_faction_id, fog_of_war, sys_id)) {
    nm += " (" + system_label(sim, sys_id, viewer_faction_id, fog_of_war) + ")";
  }
  return nm;
}

std::string ship_label(const Simulation& sim, Id ship_id, Id viewer_faction_id, bool fog_of_war) {
  const auto& s = sim.state();
  const auto* sh = find_ptr(s.ships, ship_id);
  if (!sh) return id_fallback("Ship", ship_id);
  if (!can_show_ship_name(sim, viewer_faction_id, fog_of_war, ship_id)) {
    return id_fallback("Ship", ship_id);
  }
  if (!sh->name.empty()) return sh->name;
  return id_fallback("Ship", ship_id);
}

std::string jump_point_label(const Simulation& sim, Id jump_id, Id viewer_faction_id, bool fog_of_war) {
  const auto& s = sim.state();
  const auto* jp = find_ptr(s.jump_points, jump_id);
  if (!jp) return id_fallback("Jump", jump_id);

  if (jp->system_id != kInvalidId && !can_show_system_name(sim, viewer_faction_id, fog_of_war, jp->system_id)) {
    return id_fallback("Jump", jump_id);
  }

  std::string nm = jp->name.empty() ? id_fallback("Jump", jump_id) : jp->name;

  // Try to display destination system name when visible.
  Id dst_sys = kInvalidId;
  if (jp->linked_jump_id != kInvalidId) {
    const auto* other = find_ptr(s.jump_points, jp->linked_jump_id);
    if (other) dst_sys = other->system_id;
  }

  if (dst_sys != kInvalidId && can_show_system_name(sim, viewer_faction_id, fog_of_war, dst_sys)) {
    nm += " -> " + system_label(sim, dst_sys, viewer_faction_id, fog_of_war);
  }

  return nm;
}

std::string wreck_label(const Simulation& sim, Id wreck_id, Id viewer_faction_id, bool fog_of_war) {
  const auto& s = sim.state();
  const auto* w = find_ptr(s.wrecks, wreck_id);
  if (!w) return id_fallback("Wreck", wreck_id);

  if (w->system_id != kInvalidId && !can_show_system_name(sim, viewer_faction_id, fog_of_war, w->system_id)) {
    return id_fallback("Wreck", wreck_id);
  }
  if (!w->name.empty()) return w->name;
  return id_fallback("Wreck", wreck_id);
}

std::string anomaly_label(const Simulation& sim, Id anomaly_id, Id viewer_faction_id, bool fog_of_war) {
  const auto& s = sim.state();
  const auto* a = find_ptr(s.anomalies, anomaly_id);
  if (!a) return id_fallback("Anomaly", anomaly_id);

  if (a->system_id != kInvalidId && !can_show_system_name(sim, viewer_faction_id, fog_of_war, a->system_id)) {
    return id_fallback("Anomaly", anomaly_id);
  }

  std::string nm = a->name.empty() ? id_fallback("Anomaly", anomaly_id) : a->name;
  if (a->system_id != kInvalidId && can_show_system_name(sim, viewer_faction_id, fog_of_war, a->system_id)) {
    nm += " (" + system_label(sim, a->system_id, viewer_faction_id, fog_of_war) + ")";
  }
  return nm;
}

std::string repeat_prefix(int repeat_count_remaining) {
  std::string prefix = "Repeat";
  if (repeat_count_remaining < 0) {
    prefix += "(∞)";
  } else {
    prefix += "(" + std::to_string(repeat_count_remaining) + ")";
  }
  prefix += ": ";
  return prefix;
}

}  // namespace

std::string order_to_ui_string(const Simulation& sim, const Order& order, Id viewer_faction_id, bool fog_of_war) {
  return std::visit(
      [&](const auto& o) -> std::string {
        using T = std::decay_t<decltype(o)>;
        std::ostringstream ss;
        ss.setf(std::ios::fixed);

        if constexpr (std::is_same_v<T, MoveToPoint>) {
          ss << "Move to (" << std::setprecision(1) << o.target_mkm.x << ", " << o.target_mkm.y << ")";
        } else if constexpr (std::is_same_v<T, MoveToBody>) {
          ss << "Move to " << body_label(sim, o.body_id, viewer_faction_id, fog_of_war);
        } else if constexpr (std::is_same_v<T, ColonizeBody>) {
          ss << "Colonize " << body_label(sim, o.body_id, viewer_faction_id, fog_of_war);
          if (!o.colony_name.empty()) ss << " as \"" << o.colony_name << "\"";
        } else if constexpr (std::is_same_v<T, OrbitBody>) {
          ss << "Orbit " << body_label(sim, o.body_id, viewer_faction_id, fog_of_war);
          if (o.duration_days == 0) {
            ss << " (instant)";
          } else if (o.duration_days > 0) {
            ss << " (" << o.duration_days << " d)";
          }
        } else if constexpr (std::is_same_v<T, TravelViaJump>) {
          ss << "Travel via " << jump_point_label(sim, o.jump_point_id, viewer_faction_id, fog_of_war);
        } else if constexpr (std::is_same_v<T, SurveyJumpPoint>) {
          ss << "Survey " << jump_point_label(sim, o.jump_point_id, viewer_faction_id, fog_of_war);
          if (o.transit_when_done) ss << " (transit)";
        } else if constexpr (std::is_same_v<T, AttackShip>) {
          ss << "Attack " << ship_label(sim, o.target_ship_id, viewer_faction_id, fog_of_war);
        } else if constexpr (std::is_same_v<T, EscortShip>) {
          ss << "Escort " << ship_label(sim, o.target_ship_id, viewer_faction_id, fog_of_war);
          if (o.follow_distance_mkm > 0.0) {
            ss << " (" << std::setprecision(1) << o.follow_distance_mkm << " mkm)";
          }
          if (o.restrict_to_discovered) ss << " [disc]";
          if (o.allow_neutral) ss << " [neutral]";
        } else if constexpr (std::is_same_v<T, WaitDays>) {
          ss << "Wait " << o.days_remaining << " d";
        } else if constexpr (std::is_same_v<T, LoadMineral>) {
          ss << "Load ";
          if (!o.mineral.empty()) {
            ss << o.mineral;
          } else {
            ss << "minerals";
          }
          ss << " @ " << colony_label(sim, o.colony_id, viewer_faction_id, fog_of_war);
          if (o.tons > 0.0) ss << " (" << std::setprecision(1) << o.tons << " t)";
        } else if constexpr (std::is_same_v<T, UnloadMineral>) {
          ss << "Unload ";
          if (!o.mineral.empty()) {
            ss << o.mineral;
          } else {
            ss << "minerals";
          }
          ss << " @ " << colony_label(sim, o.colony_id, viewer_faction_id, fog_of_war);
          if (o.tons > 0.0) ss << " (" << std::setprecision(1) << o.tons << " t)";
        } else if constexpr (std::is_same_v<T, MineBody>) {
          ss << "Mine ";
          if (!o.mineral.empty()) {
            ss << o.mineral << " @ ";
          }
          ss << body_label(sim, o.body_id, viewer_faction_id, fog_of_war);
          if (o.stop_when_cargo_full) ss << " (until full)";
        } else if constexpr (std::is_same_v<T, LoadTroops>) {
          ss << "Load troops @ " << colony_label(sim, o.colony_id, viewer_faction_id, fog_of_war);
          if (o.strength > 0.0) ss << " (" << std::setprecision(1) << o.strength << ")";
        } else if constexpr (std::is_same_v<T, UnloadTroops>) {
          ss << "Unload troops @ " << colony_label(sim, o.colony_id, viewer_faction_id, fog_of_war);
          if (o.strength > 0.0) ss << " (" << std::setprecision(1) << o.strength << ")";
        } else if constexpr (std::is_same_v<T, LoadColonists>) {
          ss << "Load colonists @ " << colony_label(sim, o.colony_id, viewer_faction_id, fog_of_war);
          if (o.millions > 0.0) ss << " (" << std::setprecision(2) << o.millions << " M)";
        } else if constexpr (std::is_same_v<T, UnloadColonists>) {
          ss << "Unload colonists @ " << colony_label(sim, o.colony_id, viewer_faction_id, fog_of_war);
          if (o.millions > 0.0) ss << " (" << std::setprecision(2) << o.millions << " M)";
        } else if constexpr (std::is_same_v<T, InvadeColony>) {
          ss << "Invade " << colony_label(sim, o.colony_id, viewer_faction_id, fog_of_war);
        } else if constexpr (std::is_same_v<T, BombardColony>) {
          ss << "Bombard " << colony_label(sim, o.colony_id, viewer_faction_id, fog_of_war);
          if (o.duration_days == 0) {
            ss << " (instant)";
          } else if (o.duration_days > 0) {
            ss << " (" << o.duration_days << " d)";
          }
        } else if constexpr (std::is_same_v<T, SalvageWreck>) {
          ss << "Salvage " << wreck_label(sim, o.wreck_id, viewer_faction_id, fog_of_war);
          if (!o.mineral.empty()) ss << " (" << o.mineral << ")";
          if (o.tons > 0.0) ss << " (" << std::setprecision(1) << o.tons << " t)";
        } else if constexpr (std::is_same_v<T, SalvageWreckLoop>) {
          ss << "Salvage loop " << wreck_label(sim, o.wreck_id, viewer_faction_id, fog_of_war);
          if (o.dropoff_colony_id != kInvalidId) {
            ss << " -> " << colony_label(sim, o.dropoff_colony_id, viewer_faction_id, fog_of_war);
          }
          if (o.restrict_to_discovered) ss << " [disc]";
        } else if constexpr (std::is_same_v<T, InvestigateAnomaly>) {
          ss << "Investigate " << anomaly_label(sim, o.anomaly_id, viewer_faction_id, fog_of_war);
          if (o.duration_days > 0) ss << " (" << o.duration_days << " d)";
        } else if constexpr (std::is_same_v<T, TransferCargoToShip>) {
          ss << "Transfer cargo to " << ship_label(sim, o.target_ship_id, viewer_faction_id, fog_of_war);
          if (!o.mineral.empty()) ss << " (" << o.mineral << ")";
          if (o.tons > 0.0) ss << " (" << std::setprecision(1) << o.tons << " t)";
        } else if constexpr (std::is_same_v<T, TransferFuelToShip>) {
          ss << "Transfer fuel to " << ship_label(sim, o.target_ship_id, viewer_faction_id, fog_of_war);
          if (o.tons > 0.0) ss << " (" << std::setprecision(1) << o.tons << " t)";
        } else if constexpr (std::is_same_v<T, TransferTroopsToShip>) {
          ss << "Transfer troops to " << ship_label(sim, o.target_ship_id, viewer_faction_id, fog_of_war);
          if (o.strength > 0.0) ss << " (" << std::setprecision(1) << o.strength << ")";
        } else if constexpr (std::is_same_v<T, TransferColonistsToShip>) {
          ss << "Transfer colonists to " << ship_label(sim, o.target_ship_id, viewer_faction_id, fog_of_war);
          if (o.millions > 0.0) ss << " (" << std::setprecision(2) << o.millions << " M)";
        } else if constexpr (std::is_same_v<T, ScrapShip>) {
          ss << "Scrap ship @ " << colony_label(sim, o.colony_id, viewer_faction_id, fog_of_war);
        } else {
          // Fall back to the core debug string for any future variants.
          return nebula4x::order_to_string(order);
        }

        return ss.str();
      },
      order);
}

std::string ship_orders_first_action_label(const Simulation& sim, const ShipOrders* so,
                                           Id viewer_faction_id, bool fog_of_war) {
  if (!so) return std::string();

  if (so->suspended) {
    if (!so->queue.empty()) {
      return std::string("[Suspended] ") +
             order_to_ui_string(sim, so->queue.front(), viewer_faction_id, fog_of_war);
    }
    return std::string("[Suspended]");
  }

  if (!so->queue.empty()) {
    return order_to_ui_string(sim, so->queue.front(), viewer_faction_id, fog_of_war);
  }

  if (so->repeat && !so->repeat_template.empty() && so->repeat_count_remaining != 0) {
    return repeat_prefix(so->repeat_count_remaining) +
           order_to_ui_string(sim, so->repeat_template.front(), viewer_faction_id, fog_of_war);
  }

  return std::string();
}

void draw_ship_orders_tooltip(const Simulation& sim, const ShipOrders* so, Id viewer_faction_id,
                              bool fog_of_war, int max_lines) {
  ImGui::BeginTooltip();

  if (!so) {
    ImGui::TextDisabled("(no orders)");
    ImGui::EndTooltip();
    return;
  }

  const bool idle = ship_orders_is_idle_for_automation(*so);
  const bool repeating_active = (so->repeat && !so->repeat_template.empty() && so->repeat_count_remaining != 0);

  if (so->suspended) {
    ImGui::TextUnformatted("Status: Suspended");
    ImGui::TextDisabled("(auto-retreat emergency plan is active)");
  } else if (idle) {
    ImGui::TextUnformatted("Status: Idle");
  } else {
    ImGui::TextUnformatted("Status: Busy");
  }

  auto draw_list = [&](const char* title, const std::vector<Order>& q) {
    ImGui::Separator();
    ImGui::TextUnformatted(title);
    if (q.empty()) {
      ImGui::TextDisabled("(empty)");
      return;
    }
    const int cap = std::max(1, max_lines);
    const int n = static_cast<int>(q.size());
    const int shown = std::min(n, cap);
    for (int i = 0; i < shown; ++i) {
      const std::string s = order_to_ui_string(sim, q[static_cast<std::size_t>(i)], viewer_faction_id, fog_of_war);
      ImGui::BulletText("%d. %s", i + 1, s.c_str());
    }
    if (shown < n) {
      ImGui::TextDisabled("... (%d more)", n - shown);
    }
  };

  // Show active plan (queue or active repeat template).
  if (so->suspended) {
    draw_list("Emergency queue", so->queue);
    if (!so->suspended_queue.empty()) {
      draw_list("Suspended original queue", so->suspended_queue);
    } else if (so->suspended_repeat && !so->suspended_repeat_template.empty()) {
      draw_list("Suspended original repeat template", so->suspended_repeat_template);
    }
  } else if (!so->queue.empty()) {
    draw_list("Queue", so->queue);
    if (repeating_active) {
      // Also show the repeat template so the player can tell what will happen next cycle.
      std::string hdr = "Repeat template ";
      if (so->repeat_count_remaining < 0) {
        hdr += "(∞)";
      } else {
        hdr += "(" + std::to_string(so->repeat_count_remaining) + ")";
      }
      draw_list(hdr.c_str(), so->repeat_template);
    }
  } else if (repeating_active) {
    std::string hdr = "Repeat template ";
    if (so->repeat_count_remaining < 0) {
      hdr += "(∞)";
    } else {
      hdr += "(" + std::to_string(so->repeat_count_remaining) + ")";
    }
    draw_list(hdr.c_str(), so->repeat_template);
  } else {
    ImGui::Separator();
    ImGui::TextDisabled("(no queued orders)");
  }

  ImGui::EndTooltip();
}

}  // namespace nebula4x::ui
