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
        } else if constexpr (std::is_same_v<T, TravelViaJump>) {
          ss << "TravelViaJump(jump_id=" << o.jump_point_id << ")";
        } else if constexpr (std::is_same_v<T, AttackShip>) {
          ss << "AttackShip(target_id=" << o.target_ship_id << ")";
        }
        return ss.str();
      },
      order);
}

} // namespace nebula4x
