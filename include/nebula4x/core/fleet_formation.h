#pragma once

#include <unordered_map>
#include <vector>

#include "nebula4x/core/entities.h"

namespace nebula4x {

// Computes per-ship formation offsets (in million km) for a cohort of ships.
//
// The offsets are intended to be added to the cohort's *raw* target position
// (e.g. MoveToPoint target, or an AttackShip target position) to get the final
// position for each ship.
//
// - "members_sorted_unique" must contain the cohort member ship IDs (including
//   the leader) sorted and de-duplicated.
// - "leader_id" should be the desired leader ship ID. If that ship isn't in
//   the cohort, the first member is treated as leader.
//
// The returned map includes an entry for the leader with offset {0,0}.
std::unordered_map<Id, Vec2> compute_fleet_formation_offsets(FleetFormation formation, double spacing_mkm,
                                                             Id leader_id, const Vec2& leader_pos_mkm,
                                                             const Vec2& raw_target_mkm,
                                                             const std::vector<Id>& members_sorted_unique);

} // namespace nebula4x
