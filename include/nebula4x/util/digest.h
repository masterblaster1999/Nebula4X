#pragma once

#include <cstdint>
#include <string>

#include "nebula4x/core/game_state.h"

namespace nebula4x {

// Options controlling which parts of a GameState are included in a digest.
//
// The intent is to support both:
//  - "full" digests that match what most people consider part of a save, and
//  - "gameplay" digests that ignore UI-only or log-like fields.
struct DigestOptions {
  // Include the persistent SimEvent log.
  bool include_events{true};

  // Include UI-only fields (currently: GameState::selected_system).
  bool include_ui_state{true};
};

// Compute a stable 64-bit digest of the current in-memory game state.
//
// Properties:
//  - Deterministic across runs/platforms (no dependence on unordered_map iteration order).
//  - Insensitive to ordering of set-like vectors (e.g. discovered_systems, system ship lists).
//  - Sensitive to ordering of queue-like vectors (e.g. ship order queues, build queues).
std::uint64_t digest_game_state64(const GameState& state, const DigestOptions& opt = {});

// Compute a stable 64-bit digest of the loaded content database.
//
// This can be used to identify "mod sets" in bug reports or for regression tests.
std::uint64_t digest_content_db64(const ContentDB& content);

// Format a 64-bit digest as a fixed-width lowercase hex string.
std::string digest64_to_hex(std::uint64_t v);

} // namespace nebula4x
