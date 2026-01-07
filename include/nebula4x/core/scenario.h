#pragma once

#include <cstdint>

#include "nebula4x/core/game_state.h"

namespace nebula4x {

// High-level archetype for procedural galaxy placement.
//
// The goal is variety and interesting navigation graphs, not strict
// astrophysical fidelity.
enum class RandomGalaxyShape : std::uint8_t {
  // Classic 4X-ish "spiral arms" disc. This is the default.
  SpiralDisc = 0,

  // A smooth disc with no explicit arms (uniform-in-area distribution).
  UniformDisc = 1,

  // Annular distribution (low density core, denser rim).
  Ring = 2,

  // A small number of clustered star groups with sparse interstitial space.
  Clustered = 3,

  // Filamentary distribution (voids + "rivers" of stars).
  //
  // This is a gamey approximation of large-scale structure: it produces
  // contiguous corridors of higher density separated by empty voids.
  Filamentary = 4,
};

// High-level sampling style used to place star systems in the galaxy map.
//
// "Classic" uses simple rejection sampling with a minimum separation.
// "BlueNoise" uses Mitchell's best-candidate strategy to approximate a
// Poisson-disc / blue-noise distribution (more even spacing, fewer clumps).
enum class RandomPlacementStyle : std::uint8_t {
  Classic = 0,
  BlueNoise = 1,
};

// High-level archetype for how systems are connected via jump points.
//
// These are intentionally "gamey" navigation topologies designed to create
// different strategic constraints and exploration pacing.
enum class RandomJumpNetworkStyle : std::uint8_t {
  // Baseline: connected MST + a moderate number of extra short links.
  Balanced = 0,

  // Many local links (high redundancy, lots of alternate routes).
  DenseWeb = 1,

  // Very sparse "hyperlane" style network (chokepoints / long corridors).
  SparseLanes = 2,

  // Dense intra-cluster links with a small number of inter-cluster bridges.
  ClusterBridges = 3,

  // A small number of hubs with spokes; hubs are connected by a thin backbone.
  HubAndSpoke = 4,

  // Planar proximity graph: build lanes from a Delaunay triangulation of the
  // galaxy points.
  //
  // This produces visually readable, near-planar jump maps (few/no crossings)
  // while still allowing density-scaled redundancy.
  PlanarProximity = 5,
};


// Tunables for make_random_scenario().
//
// This is intentionally compact so it can be wired into UI/CLI without needing
// to expose the entire internal generator.
struct RandomScenarioConfig {
  std::uint32_t seed{12345u};
  int num_systems{12};

  RandomGalaxyShape galaxy_shape{RandomGalaxyShape::SpiralDisc};

  // System placement sampling style.
  RandomPlacementStyle placement_style{RandomPlacementStyle::Classic};

  // Quality / cost knob for BlueNoise placement.
  //
  // This is the number of random candidates evaluated per point when using
  // Mitchell-style best-candidate sampling. Higher values produce more even
  // spacing at higher cost.
  int placement_quality{24};

  // Jump network topology archetype.
  RandomJumpNetworkStyle jump_network_style{RandomJumpNetworkStyle::Balanced};

  // Extra-link density scaler for the jump network.
  //
  // 0.0 => minimum connectivity only (tree/backbone)
  // 1.0 => default for the chosen topology
  // 2.0 => very dense (many alternate routes)
  double jump_density{1.0};


  // If true, generate Voronoi-like galaxy regions ("sectors") and assign each
  // system to a region. Regions carry environment / content modifiers (minerals,
  // volatiles, salvage, nebula bias, piracy risk, ruins density).
  bool enable_regions{true};

  // How many regions to generate.
  //
  // -1 => auto scale with num_systems (recommended).
  int num_regions{-1};



  // Number of non-player AI empires (excluding pirates).
  //
  // -1 => auto scale with num_systems.
  int num_ai_empires{-1};

  // Pirates are a dedicated hostile AI faction.
  bool enable_pirates{true};

  // Multiplier for pirate starting ships (1.0 = default).
  //
  // Values > 1.0 spawn more starting pirate raiders.
  double pirate_strength{1.0};

  // If true, clamp the player's home system nebula density low so early UI
  // readability and sensor ranges aren't immediately crushed.
  bool ensure_clear_home{true};
};

// Creates a small Sol scenario with Earth colony + two ships.
GameState make_sol_scenario();

// Creates a small procedurally-generated scenario.
//
// The output is deterministic for a given config.
//
// This is intentionally lightweight and content-agnostic: it uses a small set
// of well-known starting design ids ("freighter_alpha", "surveyor_beta",
// "escort_gamma", "colony_ship_mk1", "pirate_raider"). If a given content pack
// does not provide these designs, the simulation will still run (ships will fall
// back to minimal stats), but the scenario will be less interesting.
GameState make_random_scenario(const RandomScenarioConfig& cfg);

// Backwards-compatible wrapper: deterministic for a given (seed, num_systems).
GameState make_random_scenario(std::uint32_t seed, int num_systems = 12);

} // namespace nebula4x
