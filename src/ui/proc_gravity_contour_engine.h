#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <imgui.h>

#include "nebula4x/core/simulation.h"

namespace nebula4x::ui {

// Procedural "gravity well" contour renderer for the system map.
//
// This engine evaluates a simplified Newtonian potential field produced by
// system bodies and renders iso-contour lines (marching squares). The intent
// is to give the player an at-a-glance understanding of where deep gravity
// wells are without shipping bespoke art assets.
//
// Implementation notes:
//  - Contours are generated in world space (mkm) and cached in tiles.
//  - Tiles are keyed by (system id, tile coord, zoom bucket, day bucket).
//  - Contour levels are chosen deterministically from the system's max body
//    mass and the current zoom bucket so adjacent tiles share the same bands.

struct ProcGravityContourConfig {
  bool enabled{false};

  // Approx tile size in screen pixels (converted to world units through
  // quantized zoom buckets so cached tiles remain reusable while zooming).
  int tile_px{420};

  // Maximum number of cached tiles.
  int max_cached_tiles{160};

  // Scalar field resolution inside a tile.
  // N means each tile is sampled on an (N+1)x(N+1) grid and produces up to
  // ~N*N segments per contour level.
  int samples_per_tile{34};

  // Number of contour levels.
  int contour_levels{10};

  // Contour spacing in log10 units ("decades").
  // Larger -> fewer wider rings; smaller -> denser rings.
  float level_spacing_decades{0.33f};

  // Global shift in decades (moves the rings in/out).
  float level_offset_decades{0.0f};

  // Visual style.
  float opacity{0.25f};
  float thickness_px{1.15f};

  // Softening parameters to avoid singularities at r~0.
  // Each body contributes: mass / (distance + softening).
  float softening_min_mkm{0.05f};
  float softening_radius_mult{2.0f};

  // Debug.
  bool debug_tile_bounds{false};
};

struct ProcGravityContourStats {
  int cache_tiles{0};
  int tiles_used_this_frame{0};
  int tiles_generated_this_frame{0};
  int segments_drawn{0};
};

class ProcGravityContourEngine {
 public:
  void begin_frame(double sim_time_days);
  void clear();

  ProcGravityContourStats stats() const { return stats_; }

  // Draw contour lines covering the rectangle [origin, origin+size).
  //
  // Coordinate space matches other map overlays:
  //  - world positions are in mkm.
  //  - screen transform is center + (world + pan)*scale*zoom.
  void draw_contours(ImDrawList* draw,
                     const ImVec2& origin,
                     const ImVec2& size,
                     const ImVec2& center,
                     double scale_px_per_mkm,
                     double zoom,
                     const Vec2& pan_mkm,
                     const Simulation& sim,
                     Id system_id,
                     std::uint32_t seed,
                     const ProcGravityContourConfig& cfg,
                     ImU32 base_color);

 private:
  struct Segment {
    Vec2 a_mkm;
    Vec2 b_mkm;
    std::uint16_t level_idx{0};
  };

  struct TileKey {
    Id system_id{kInvalidId};
    int tx{0};
    int ty{0};
    int scale_bucket{0};
    int tile_px{0};
    std::int64_t day_bucket{0};
    std::uint32_t seed{0};
    std::uint64_t style_hash{0};

    bool operator==(const TileKey& o) const {
      return system_id == o.system_id && tx == o.tx && ty == o.ty && scale_bucket == o.scale_bucket &&
             tile_px == o.tile_px && day_bucket == o.day_bucket && seed == o.seed && style_hash == o.style_hash;
    }
  };

  struct TileKeyHash {
    std::size_t operator()(const TileKey& k) const noexcept;
  };

  struct Tile {
    std::uint64_t last_used_frame{0};
    std::vector<Segment> segments;
  };

  static std::uint64_t compute_style_hash(const ProcGravityContourConfig& cfg);
  static int quantize_scale_bucket(double units_per_px_mkm);
  static double bucket_to_units_per_px_mkm(int bucket);

  Tile& get_or_build_tile(const TileKey& key,
                          double tile_world_mkm,
                          const Simulation& sim,
                          Id system_id,
                          double max_mass_earths,
                          const ProcGravityContourConfig& cfg);

  void trim_cache(int max_tiles);

  std::unordered_map<TileKey, Tile, TileKeyHash> cache_;
  ProcGravityContourStats stats_{};
  std::uint64_t frame_index_{0};
  double time_days_{0.0};
};

} // namespace nebula4x::ui
