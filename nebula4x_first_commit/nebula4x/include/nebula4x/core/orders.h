#pragma once

#include <string>
#include <variant>
#include <vector>

#include "nebula4x/core/ids.h"
#include "nebula4x/core/vec2.h"

namespace nebula4x {

struct MoveToPoint {
  Vec2 target_mkm;
};

struct MoveToBody {
  Id body_id{kInvalidId};
};

using Order = std::variant<MoveToPoint, MoveToBody>;

struct ShipOrders {
  std::vector<Order> queue;
};

std::string order_to_string(const Order& order);

} // namespace nebula4x
