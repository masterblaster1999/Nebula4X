#include "nebula4x/core/orders.h"

#include <sstream>
#include <type_traits>

namespace nebula4x {

std::string order_to_string(const Order& order) {
  return std::visit(
      [](const auto& o) -> std::string {
        using T = std::decay_t<decltype(o)>;
        std::ostringstream ss;

        if constexpr (std::is_same_v<T, MoveToPoint>) {
          ss << "MoveToPoint(" << o.target_mkm.x << ", " << o.target_mkm.y << ")";
        } else if constexpr (std::is_same_v<T, MoveToBody>) {
          ss << "MoveToBody(id=" << o.body_id << ")";
        } else if constexpr (std::is_same_v<T, ColonizeBody>) {
          ss << "ColonizeBody(body_id=" << o.body_id;
          if (!o.colony_name.empty()) ss << ", name=\"" << o.colony_name << "\"";
          ss << ")";
        } else if constexpr (std::is_same_v<T, OrbitBody>) {
          ss << "OrbitBody(id=" << o.body_id << ", days=" << o.duration_days << ")";
        } else if constexpr (std::is_same_v<T, TravelViaJump>) {
          ss << "TravelViaJump(jump_id=" << o.jump_point_id << ")";
        } else if constexpr (std::is_same_v<T, SurveyJumpPoint>) {
          ss << "SurveyJumpPoint(jump_id=" << o.jump_point_id;
          if (o.transit_when_done) ss << ", transit_when_done=true";
          ss << ")";
        } else if constexpr (std::is_same_v<T, AttackShip>) {
          ss << "AttackShip(target_id=" << o.target_ship_id;
          if (o.has_last_known) {
            ss << ", last=(" << o.last_known_position_mkm.x << ", " << o.last_known_position_mkm.y << ")";
          }
          ss << ")";
        } else if constexpr (std::is_same_v<T, EscortShip>) {
          ss << "EscortShip(target_id=" << o.target_ship_id << ", follow_mkm=" << o.follow_distance_mkm;
          if (o.restrict_to_discovered) ss << ", restrict_to_discovered=true";
          ss << ")";
        } else if constexpr (std::is_same_v<T, WaitDays>) {
          ss << "WaitDays(" << o.days_remaining << ")";
        } else if constexpr (std::is_same_v<T, LoadMineral>) {
          ss << "LoadMineral(colony_id=" << o.colony_id;
          if (!o.mineral.empty()) ss << ", mineral=" << o.mineral;
          if (o.tons > 0.0) ss << ", tons=" << o.tons;
          ss << ")";
        } else if constexpr (std::is_same_v<T, UnloadMineral>) {
          ss << "UnloadMineral(colony_id=" << o.colony_id;
          if (!o.mineral.empty()) ss << ", mineral=" << o.mineral;
          if (o.tons > 0.0) ss << ", tons=" << o.tons;
          ss << ")";
        } else if constexpr (std::is_same_v<T, MineBody>) {
          ss << "MineBody(body_id=" << o.body_id;
          if (!o.mineral.empty()) ss << ", mineral=" << o.mineral;
          if (o.stop_when_cargo_full) ss << ", stop_when_full=true";
          ss << ")";
        } else if constexpr (std::is_same_v<T, LoadTroops>) {
          ss << "LoadTroops(colony_id=" << o.colony_id;
          if (o.strength > 0.0) ss << ", strength=" << o.strength;
          ss << ")";
        } else if constexpr (std::is_same_v<T, UnloadTroops>) {
          ss << "UnloadTroops(colony_id=" << o.colony_id;
          if (o.strength > 0.0) ss << ", strength=" << o.strength;
          ss << ")";
        } else if constexpr (std::is_same_v<T, LoadColonists>) {
          ss << "LoadColonists(colony_id=" << o.colony_id;
          if (o.millions > 0.0) ss << ", millions=" << o.millions;
          ss << ")";
        } else if constexpr (std::is_same_v<T, UnloadColonists>) {
          ss << "UnloadColonists(colony_id=" << o.colony_id;
          if (o.millions > 0.0) ss << ", millions=" << o.millions;
          ss << ")";
        } else if constexpr (std::is_same_v<T, InvadeColony>) {
          ss << "InvadeColony(colony_id=" << o.colony_id << ")";
        } else if constexpr (std::is_same_v<T, BombardColony>) {
          ss << "BombardColony(colony_id=" << o.colony_id << ", days=" << o.duration_days << ")";
        } else if constexpr (std::is_same_v<T, SalvageWreck>) {
          ss << "SalvageWreck(wreck_id=" << o.wreck_id;
          if (!o.mineral.empty()) ss << ", mineral=" << o.mineral;
          if (o.tons > 0.0) ss << ", tons=" << o.tons;
          ss << ")";
        } else if constexpr (std::is_same_v<T, TransferCargoToShip>) {
          ss << "TransferCargoToShip(target_ship_id=" << o.target_ship_id;
          if (!o.mineral.empty()) ss << ", mineral=" << o.mineral;
          if (o.tons > 0.0) ss << ", tons=" << o.tons;
          ss << ")";
        } else if constexpr (std::is_same_v<T, TransferFuelToShip>) {
          ss << "TransferFuelToShip(target_ship_id=" << o.target_ship_id;
          if (o.tons > 0.0) ss << ", tons=" << o.tons;
          ss << ")";
        } else if constexpr (std::is_same_v<T, TransferTroopsToShip>) {
          ss << "TransferTroopsToShip(target_ship_id=" << o.target_ship_id;
          if (o.strength > 0.0) ss << ", strength=" << o.strength;
          ss << ")";
        } else if constexpr (std::is_same_v<T, ScrapShip>) {
          ss << "ScrapShip(colony_id=" << o.colony_id << ")";
        }

        return ss.str();
      },
      order);
}

} // namespace nebula4x
