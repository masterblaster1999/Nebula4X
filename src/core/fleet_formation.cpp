#include "nebula4x/core/fleet_formation.h"

#include <algorithm>
#include <cmath>

namespace nebula4x {
namespace {
constexpr double kTwoPi = 6.283185307179586;
}

std::unordered_map<Id, Vec2> compute_fleet_formation_offsets(FleetFormation formation, double spacing_mkm,
                                                             Id leader_id, const Vec2& leader_pos_mkm,
                                                             const Vec2& raw_target_mkm,
                                                             const std::vector<Id>& members_sorted_unique) {
  std::unordered_map<Id, Vec2> out;
  if (formation == FleetFormation::None) return out;
  if (spacing_mkm <= 0.0) return out;
  if (members_sorted_unique.empty()) return out;

  // Choose a deterministic leader.
  if (leader_id == kInvalidId || std::find(members_sorted_unique.begin(), members_sorted_unique.end(), leader_id) ==
                                    members_sorted_unique.end()) {
    leader_id = members_sorted_unique.front();
  }

  out.reserve(members_sorted_unique.size() * 2);
  out[leader_id] = Vec2{0.0, 0.0};

  // Build the follower list in deterministic order.
  std::vector<Id> followers;
  followers.reserve(members_sorted_unique.size());
  for (Id sid : members_sorted_unique) {
    if (sid != leader_id) followers.push_back(sid);
  }
  const std::size_t m = followers.size();
  if (m == 0) return out;

  // Orthonormal basis derived from leader position and raw target.
  Vec2 forward = raw_target_mkm - leader_pos_mkm;
  const double flen = forward.length();
  if (flen < 1e-9) {
    forward = Vec2{1.0, 0.0};
  } else {
    forward = forward * (1.0 / flen);
  }
  const Vec2 right{-forward.y, forward.x};

  auto world_from_local = [&](double x_right, double y_forward) -> Vec2 {
    return right * x_right + forward * y_forward;
  };

  for (std::size_t i = 0; i < m; ++i) {
    const Id sid = followers[i];
    Vec2 off{0.0, 0.0};

    switch (formation) {
      case FleetFormation::LineAbreast: {
        const int ring = static_cast<int>(i / 2) + 1;
        const int sign = ((i % 2) == 0) ? 1 : -1;
        off = world_from_local(static_cast<double>(sign * ring) * spacing_mkm, 0.0);
        break;
      }
      case FleetFormation::Column: {
        off = world_from_local(0.0, -static_cast<double>(i + 1) * spacing_mkm);
        break;
      }
      case FleetFormation::Wedge: {
        const int layer = static_cast<int>(i / 2) + 1;
        const int sign = ((i % 2) == 0) ? 1 : -1;
        off = world_from_local(static_cast<double>(sign * layer) * spacing_mkm,
                               -static_cast<double>(layer) * spacing_mkm);
        break;
      }
      case FleetFormation::Ring: {
        const double angle = kTwoPi * (static_cast<double>(i) / static_cast<double>(m));
        const double radius = std::max(spacing_mkm, (static_cast<double>(m) * spacing_mkm) / kTwoPi);
        off = world_from_local(std::cos(angle) * radius, std::sin(angle) * radius);
        break;
      }
      case FleetFormation::None:
      default: {
        off = Vec2{0.0, 0.0};
        break;
      }
    }

    out[sid] = off;
  }

  return out;
}

} // namespace nebula4x
