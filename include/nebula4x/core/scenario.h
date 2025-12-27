#pragma once

#include <cstdint>

#include "nebula4x/core/game_state.h"

namespace nebula4x {

// Creates a small Sol scenario with Earth colony + two ships.
GameState make_sol_scenario();

// Creates a small procedurally-generated scenario.
//
// The output is deterministic for a given (seed, num_systems) pair.
//
// This is intentionally lightweight and content-agnostic: it uses a small set
// of well-known starting design ids ("freighter_alpha", "surveyor_beta",
// "escort_gamma", "pirate_raider"). If a given content pack does not provide
// these designs, the simulation will still run (ships will fall back to minimal
// stats), but the scenario will be less interesting.
GameState make_random_scenario(std::uint32_t seed, int num_systems = 12);

} // namespace nebula4x
