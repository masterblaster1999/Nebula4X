#pragma once

#include "nebula4x/core/simulation.h"

namespace nebula4x {

// Runs a deterministic, light-weight "economic" planning pass for AI-controlled
// factions.
//
// This is intentionally a prototype: the emphasis is on making the sandbox
// world progress without human input (build mines, queue research, build ships),
// not on perfectly optimal play.
void tick_ai_economy(Simulation& sim);

}  // namespace nebula4x
