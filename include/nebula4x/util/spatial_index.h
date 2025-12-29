#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "nebula4x/core/game_state.h"

namespace nebula4x {

// A simple deterministic 2D spatial index (uniform grid / spatial hash).
//
// This is primarily used to accelerate in-system queries like:
//  - "ships within sensor range of a source"
//  - "ships within weapon range of an attacker"
//
// The index is intentionally small and header-only to keep it easy to audit.
// Results from query_radius() are always returned sorted by ship Id to preserve
// deterministic tie-break behavior elsewhere in the simulation.
class SpatialIndex2D {
 public:
  explicit SpatialIndex2D(double cell_size_mkm = 25.0) { set_cell_size(cell_size_mkm); }

  void set_cell_size(double cell_size_mkm) {
    // Avoid division-by-zero and pathological values.
    cell_size_mkm_ = std::max(1e-9, cell_size_mkm);
  }

  double cell_size_mkm() const { return cell_size_mkm_; }

  void clear() {
    ids_.clear();
    positions_.clear();
    cells_.clear();
  }

  // Insert an id/position pair.
  //
  // Note: positions are assumed to be in million-km (mkm), matching the sim.
  void add(Id id, const Vec2& pos_mkm) {
    if (id == kInvalidId) return;
    const std::size_t idx = ids_.size();
    ids_.push_back(id);
    positions_.push_back(pos_mkm);

    const CellKey key{cell_coord(pos_mkm.x), cell_coord(pos_mkm.y)};
    cells_[key].push_back(idx);
  }

  // Convenience builder for the common "ships in a system" case.
  void build_from_ship_ids(const std::vector<Id>& ship_ids, const std::unordered_map<Id, Ship>& ships) {
    clear();
    ids_.reserve(ship_ids.size());
    positions_.reserve(ship_ids.size());

    for (Id sid : ship_ids) {
      const auto it = ships.find(sid);
      if (it == ships.end()) continue;
      add(sid, it->second.position_mkm);
    }
  }

  // Return ids within radius of center (inclusive).
  //
  // The epsilon parameter exists to preserve existing sim behavior in places
  // where a tiny tolerance was used historically (e.g. sensors). For weapons,
  // call with epsilon_mkm = 0.
  std::vector<Id> query_radius(const Vec2& center_mkm, double radius_mkm, double epsilon_mkm = 0.0) const {
    std::vector<Id> out;
    if (cells_.empty()) return out;

    radius_mkm = std::max(0.0, radius_mkm);
    epsilon_mkm = std::max(0.0, epsilon_mkm);
    const double r = radius_mkm + epsilon_mkm;

    const std::int64_t cx0 = cell_coord(center_mkm.x - r);
    const std::int64_t cx1 = cell_coord(center_mkm.x + r);
    const std::int64_t cy0 = cell_coord(center_mkm.y - r);
    const std::int64_t cy1 = cell_coord(center_mkm.y + r);

    // A loose reserve to avoid repeated reallocations. We don't know actual
    // density, so use the full entry count as a safe upper bound.
    out.reserve(std::min<std::size_t>(ids_.size(), 256));

    for (std::int64_t cy = cy0; cy <= cy1; ++cy) {
      for (std::int64_t cx = cx0; cx <= cx1; ++cx) {
        const CellKey key{cx, cy};
        const auto it = cells_.find(key);
        if (it == cells_.end()) continue;

        for (const std::size_t idx : it->second) {
          if (idx >= positions_.size() || idx >= ids_.size()) continue;
          const Vec2& p = positions_[idx];
          const double dx = p.x - center_mkm.x;
          const double dy = p.y - center_mkm.y;
          const double dist = std::sqrt(dx * dx + dy * dy);
          if (dist <= r) out.push_back(ids_[idx]);
        }
      }
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
  }

 private:
  struct CellKey {
    std::int64_t cx{0};
    std::int64_t cy{0};
    bool operator==(const CellKey& o) const { return cx == o.cx && cy == o.cy; }
  };

  struct CellKeyHash {
    std::size_t operator()(const CellKey& k) const {
      // Mix two signed 64-bit coords into a size_t.
      //
      // Note: This isn't cryptographic; it's just a decent distribution for
      // typical in-system coordinates.
      const std::uint64_t x = static_cast<std::uint64_t>(k.cx);
      const std::uint64_t y = static_cast<std::uint64_t>(k.cy);
      const std::uint64_t h = (x * 0x9E3779B185EBCA87ULL) ^ (y + 0xC2B2AE3D27D4EB4FULL);
      return static_cast<std::size_t>(h ^ (h >> 33));
    }
  };

  std::int64_t cell_coord(double x) const {
    return static_cast<std::int64_t>(std::floor(x / cell_size_mkm_));
  }

  double cell_size_mkm_{25.0};

  // Dense entry storage.
  std::vector<Id> ids_;
  std::vector<Vec2> positions_;

  // Sparse cell -> indices into the dense arrays.
  std::unordered_map<CellKey, std::vector<std::size_t>, CellKeyHash> cells_;
};

} // namespace nebula4x
