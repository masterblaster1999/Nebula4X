#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "nebula4x/core/game_state.h"

namespace nebula4x {

class Simulation;

// Coarse-grained trade goods used by the procedural trade network.
//
// These are intentionally *categories* rather than specific minerals so the
// system can summarize a star system's trade role even when content mods add
// additional resources.
enum class TradeGoodKind : std::uint8_t {
  RawMetals = 0,
  RawMinerals,
  Volatiles,
  Exotics,
  ProcessedMetals,
  ProcessedMinerals,
  Fuel,
  Munitions,
};

constexpr int kTradeGoodKindCount = 8;

inline int trade_good_index(TradeGoodKind k) {
  return static_cast<int>(k);
}

inline const char* trade_good_kind_label(TradeGoodKind k) {
  switch (k) {
    case TradeGoodKind::RawMetals: return "Raw metals";
    case TradeGoodKind::RawMinerals: return "Raw minerals";
    case TradeGoodKind::Volatiles: return "Volatiles";
    case TradeGoodKind::Exotics: return "Exotics";
    case TradeGoodKind::ProcessedMetals: return "Processed metals";
    case TradeGoodKind::ProcessedMinerals: return "Processed minerals";
    case TradeGoodKind::Fuel: return "Fuel";
    case TradeGoodKind::Munitions: return "Munitions";
    default: return "(unknown)";
  }
}

struct TradeGoodFlow {
  TradeGoodKind good{TradeGoodKind::RawMetals};
  double volume{0.0};
};

// Per-system summary used for UI overlays and economy/debug tooling.
struct TradeNode {
  Id system_id{kInvalidId};

  // Abstract market magnitude (dimensionless). 0 means "no meaningful market".
  double market_size{0.0};

  // 0..1 heuristic "hub" score derived from jump graph degree and galaxy position.
  double hub_score{0.0};

  // Supply/demand are abstract, but roughly "100 tons/day ~= 1 unit" when
  // colony contributions are enabled.
  std::array<double, kTradeGoodKindCount> supply{};
  std::array<double, kTradeGoodKindCount> demand{};
  std::array<double, kTradeGoodKindCount> balance{};  // supply - demand

  TradeGoodKind primary_export{TradeGoodKind::RawMetals};
  TradeGoodKind primary_import{TradeGoodKind::RawMetals};
};

// A directed inter-system trade lane.
struct TradeLane {
  Id from_system_id{kInvalidId};
  Id to_system_id{kInvalidId};

  double total_volume{0.0};
  std::vector<TradeGoodFlow> top_flows;  // top goods (by volume)
};

struct TradeNetwork {
  std::vector<TradeNode> nodes;   // one per star system
  std::vector<TradeLane> lanes;   // top-N lanes after pruning
};

struct TradeNetworkOptions {
  // Safety/perf cap. The renderer is expected to further filter by fog-of-war.
  int max_lanes{180};

  // How many goods to keep per lane for tooltips/legends.
  int max_goods_per_lane{3};

  // Exponent used for distance decay.
  // Larger values favor local trade; smaller values create long-range lanes.
  double distance_exponent{1.35};

  // If false, only systems that contain at least one colony contribute to (and
  // appear in) the market model.
  bool include_uncolonized_markets{true};

  // If true, colony industry recipes (installations) add supply/demand signals.
  bool include_colony_contributions{true};

  // Scale factor for converting "tons/day" into abstract market units.
  double colony_tons_per_unit{100.0};
};

// Compute a procedural interstellar trade network.
//
// The intent is to provide:
//  - A strategic overlay (trade hubs + lanes) for the galaxy map.
//  - A foundation for later gameplay systems (piracy, blockades, trade treaties).
//
// The result is deterministic given the current GameState.
TradeNetwork compute_trade_network(const Simulation& sim, const TradeNetworkOptions& opt = {});

}  // namespace nebula4x
