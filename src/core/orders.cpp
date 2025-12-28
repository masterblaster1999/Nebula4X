#include "nebula4x/core/orders.h"

#include <sstream>
#include <type_traits>

namespace nebula4x {

std::string order_to_string(const Order& order) {
  return std::visit(
      [](const auto& o) {
        using T = std::decay_t<decltype(o)>;
        std::ostringstream ss;
        if constexpr (std::is_same_v<T, MoveToPoint>) {
          ss << "MoveToPoint(" << o.target_mkm.x << ", " << o.target_mkm.y << ")";
        } else if constexpr (std::is_same_v<T, MoveToBody>) {
          ss << "MoveToBody(id=" << o.body_id << ")";
        } else if constexpr (std::is_same_v<T, OrbitBody>) {
          ss << "OrbitBody(id=" << o.body_id;
          ss << ", days=" << o.duration_days;
          ss << ")";
        } else if constexpr (std::is_same_v<T, TravelViaJump>) {
          ss << "TravelViaJump(jump_id=" << o.jump_point_id << ")";
        } else if constexpr (std::is_same_v<T, AttackShip>) {
          ss << "AttackShip(target_id=" << o.target_ship_id;
          if (o.has_last_known) {
            ss << ", last=(" << o.last_known_position_mkm.x << ", " << o.last_known_position_mkm.y << ")";
          }
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
        } else if constexpr (std::is_same_v<T, TransferCargoToShip>) {
          ss << "TransferCargoToShip(target_ship_id=" << o.target_ship_id;
          if (!o.mineral.empty()) ss << ", mineral=" << o.mineral;
          if (o.tons > 0.0) ss << ", tons=" << o.tons;
          ss << ")";
        } else if constexpr (std::is_same_v<T, ScrapShip>) {
          ss << "ScrapShip(colony_id=" << o.colony_id << ")";
        }
        return ss.str();
      },
      order);
}

} // namespace nebula4x
