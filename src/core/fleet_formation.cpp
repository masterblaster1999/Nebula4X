#include "nebula4x/core/fleet_formation.h"

#include <algorithm>
#include <cstdint>
#include <cmath>

#include "nebula4x/util/trace_events.h"

namespace nebula4x {
namespace {
constexpr double kTwoPi = 6.283185307179586;

struct PairCost {
  double d2;
  Id ship_id;
  std::uint32_t ship_index;
  std::uint32_t slot_index;
};

static double dist2(const Vec2& a, const Vec2& b) {
  const double dx = a.x - b.x;
  const double dy = a.y - b.y;
  return dx * dx + dy * dy;
}
}

std::unordered_map<Id, Vec2> compute_fleet_formation_offsets(FleetFormation formation, double spacing_mkm,
                                                             Id leader_id, const Vec2& leader_pos_mkm,
                                                             const Vec2& raw_target_mkm,
                                                             const std::vector<Id>& members_sorted_unique,
                                                             const std::unordered_map<Id, Vec2>* member_positions_mkm) {
  NEBULA4X_TRACE_SCOPE("compute_fleet_formation_offsets", "sim.formation");
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

  // Generate ideal formation slots (world-space offsets from raw_target_mkm).
  //
  // NOTE: Slot ordering is deterministic. If member_positions_mkm is provided,
  // we will assign ships to slots to minimize repositioning; otherwise we map
  // slots to ships in follower-id order.
  std::vector<Vec2> slots;
  slots.reserve(m);

  switch (formation) {
    case FleetFormation::LineAbreast: {
      for (std::size_t i = 0; i < m; ++i) {
        const int ring = static_cast<int>(i / 2) + 1;
        const int sign = ((i % 2) == 0) ? 1 : -1;
        slots.push_back(world_from_local(static_cast<double>(sign * ring) * spacing_mkm, 0.0));
      }
      break;
    }
    case FleetFormation::Column: {
      for (std::size_t i = 0; i < m; ++i) {
        slots.push_back(world_from_local(0.0, -static_cast<double>(i + 1) * spacing_mkm));
      }
      break;
    }
    case FleetFormation::Wedge: {
      for (std::size_t i = 0; i < m; ++i) {
        const int layer = static_cast<int>(i / 2) + 1;
        const int sign = ((i % 2) == 0) ? 1 : -1;
        slots.push_back(world_from_local(static_cast<double>(sign * layer) * spacing_mkm,
                                         -static_cast<double>(layer) * spacing_mkm));
      }
      break;
    }
    case FleetFormation::Ring: {
      // Multi-ring screen: for large cohorts a single ring becomes too wide and
      // produces huge travel distances. We fill concentric rings with roughly
      // one ship per "spacing" arc length.
      std::size_t remaining = m;
      int ring_index = 1;
      // Stagger each ring's phase slightly to avoid ships lining up radially.
      double phase = 0.0;
      while (remaining > 0) {
        const double radius = std::max(spacing_mkm, static_cast<double>(ring_index) * spacing_mkm);
        // Target roughly spacing arc length, with a minimum of 6 slots to keep
        // a ring-ish shape.
        int capacity = static_cast<int>(std::floor((kTwoPi * radius) / std::max(1e-9, spacing_mkm) + 0.5));
        capacity = std::max(6, capacity);
        const int take = static_cast<int>(std::min<std::size_t>(remaining, static_cast<std::size_t>(capacity)));

        for (int i = 0; i < take; ++i) {
          const double a = phase + kTwoPi * (static_cast<double>(i) / static_cast<double>(take));
          slots.push_back(world_from_local(std::cos(a) * radius, std::sin(a) * radius));
        }

        remaining -= static_cast<std::size_t>(take);
        ring_index += 1;
        phase += (take > 0) ? (kTwoPi / static_cast<double>(take)) * 0.5 : 0.0;
      }
      break;
    }
    case FleetFormation::None:
    default: {
      break;
    }
  }

  if (slots.size() != m) {
    // Defensive: malformed formation generation. Fall back to no offsets.
    for (Id sid : followers) out[sid] = Vec2{0.0, 0.0};
    return out;
  }

  // Optionally assign ships to formation slots by "best fit" from their current
  // positions to reduce crossing and slot-swapping.
  bool have_positions = (member_positions_mkm != nullptr);
  if (have_positions) {
    for (Id sid : followers) {
      if (member_positions_mkm->find(sid) == member_positions_mkm->end()) {
        have_positions = false;
        break;
      }
    }
  }

  if (!have_positions || m <= 1) {
    for (std::size_t i = 0; i < m; ++i) out[followers[i]] = slots[i];
    return out;
  }

  // Greedy minimum-weight bipartite matching (deterministic): consider all
  // (ship, slot) pairs sorted by distance and assign the closest available.
  // This is not globally optimal like Hungarian, but is fast, stable, and a
  // large improvement over fixed slot mapping.
  std::vector<PairCost> pairs;
  pairs.reserve(m * m);
  for (std::uint32_t si = 0; si < static_cast<std::uint32_t>(m); ++si) {
    const Id sid = followers[si];
    const Vec2 ship_pos = member_positions_mkm->at(sid);
    for (std::uint32_t sl = 0; sl < static_cast<std::uint32_t>(m); ++sl) {
      const Vec2 desired = raw_target_mkm + slots[sl];
      const double d2 = dist2(ship_pos, desired);
      pairs.push_back(PairCost{d2, sid, si, sl});
    }
  }

  std::sort(pairs.begin(), pairs.end(), [](const PairCost& a, const PairCost& b) {
    if (a.d2 < b.d2) return true;
    if (a.d2 > b.d2) return false;
    if (a.ship_id < b.ship_id) return true;
    if (a.ship_id > b.ship_id) return false;
    return a.slot_index < b.slot_index;
  });

  std::vector<int> ship_to_slot(m, -1);
  std::vector<char> slot_taken(m, 0);

  for (const auto& p : pairs) {
    const std::size_t si = static_cast<std::size_t>(p.ship_index);
    const std::size_t sl = static_cast<std::size_t>(p.slot_index);
    if (si >= m || sl >= m) continue;
    if (ship_to_slot[si] >= 0) continue;
    if (slot_taken[sl]) continue;
    ship_to_slot[si] = static_cast<int>(sl);
    slot_taken[sl] = 1;
  }

  // Fill any holes (should be rare; mainly if NaN distances slipped through).
  std::size_t next_free = 0;
  for (std::size_t si = 0; si < m; ++si) {
    if (ship_to_slot[si] >= 0) continue;
    while (next_free < m && slot_taken[next_free]) ++next_free;
    if (next_free >= m) break;
    ship_to_slot[si] = static_cast<int>(next_free);
    slot_taken[next_free] = 1;
    ++next_free;
  }

  for (std::size_t si = 0; si < m; ++si) {
    const int sl = (si < ship_to_slot.size()) ? ship_to_slot[si] : -1;
    const std::size_t slot_idx = (sl >= 0) ? static_cast<std::size_t>(sl) : si;
    const std::size_t slot_safe = (slot_idx < slots.size()) ? slot_idx : (slots.size() - 1);
    out[followers[si]] = slots[slot_safe];
  }

  return out;
}

} // namespace nebula4x
