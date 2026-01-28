#pragma once

#include <cstddef>
#include <unordered_map>
#include <vector>

#include <cstdint>

#include <imgui.h>

#include "nebula4x/core/simulation.h"

namespace nebula4x::ui {

// A lightweight procedural "space weather" renderer.
//
// This engine generates deterministic divergence-free 2D flow streamlines
// (curl-noise) in world space, caches them in tiles, and draws them as a
// stylized animated overlay.
//
// The intent is to provide a readable, performant visual cue for "terrain" in
// space (nebula microfields / storms) without shipping additional art assets.
//
// Implementation notes:
//  - Streamlines are cached per (system id, tile coord, zoom bucket).
//  - The underlying vector field is static & deterministic; animation is done
//    via a traveling highlight along the polyline so caches remain valid.
struct ProcFlowFieldConfig {
  // Master toggle (caller can simply avoid calling draw if false).
  bool enabled{false};

  // Rough size of a cache tile in screen pixels (converted to world units via
  // quantized zoom buckets).
  int tile_px{360};

  // Cache limit for generated tiles.
  int max_cached_tiles{128};

  // How many streamlines to seed per tile.
  int lines_per_tile{10};

  // Integration steps per line.
  int steps_per_line{42};

  // Integration step size in *screen pixels* (converted to world units through
  // quantized zoom buckets so cached tiles remain reusable).
  float step_px{10.0f};

  // Visual style.
  float thickness_px{1.35f};
  float opacity{0.35f};

  // Animate a moving highlight along streamlines.
  bool animate{true};

  // Highlight travel speed in cycles per day (simulation time units).
  float animate_speed_cycles_per_day{0.10f};

  // Wave length in screen pixels (converted to world units).
  float highlight_wavelength_px{180.0f};

  // Masking: only draw segments in areas with enough nebula/storm intensity.
  bool mask_by_nebula{true};
  bool mask_by_storms{false};
  float nebula_threshold{0.02f};
  float storm_threshold{0.02f};

  // Vector field feature size (bigger = smoother, smaller = more turbulent).
  float field_scale_mkm{4500.0f};

  // Debug: draw tile bounds.
  bool debug_tile_bounds{false};
};

struct ProcFlowFieldStats {
  int cache_tiles{0};
  int tiles_used_this_frame{0};
  int tiles_generated_this_frame{0};
  int lines_drawn{0};
  int segments_drawn{0};
};

class ProcFlowFieldEngine {
 public:
  void begin_frame(double sim_time_days);

  void clear();

  ProcFlowFieldStats stats() const { return stats_; }

  // Draw streamlines covering the rectangle [origin, origin+size).
  //
  // Coordinate space matches other map overlays:
  //  - world positions are in mkm.
  //  - screen transform is center + (world + pan)*scale*zoom.
  void draw_streamlines(ImDrawList* draw,
                        const ImVec2& origin,
                        const ImVec2& size,
                        const ImVec2& center,
                        double scale_px_per_mkm,
                        double zoom,
                        const Vec2& pan_mkm,
                        const Simulation& sim,
                        Id system_id,
                        std::uint32_t seed,
                        const ProcFlowFieldConfig& cfg,
                        ImU32 base_color);

 private:
  struct Streamline {
    std::vector<Vec2> pts_mkm;
    std::vector<float> s_mkm; // cumulative distance along line (mkm)
  };

  struct TileKey {
    Id system_id{kInvalidId};
    int tx{0};
    int ty{0};
    int scale_bucket{0};
    int tile_px{0};
    std::uint32_t seed{0};
    std::uint64_t style_hash{0};

    bool operator==(const TileKey& o) const {
      return system_id == o.system_id && tx == o.tx && ty == o.ty && scale_bucket == o.scale_bucket &&
             tile_px == o.tile_px && seed == o.seed && style_hash == o.style_hash;
    }
  };

  struct TileKeyHash {
    std::size_t operator()(const TileKey& k) const noexcept;
  };

  struct TileEntry {
    std::vector<Streamline> lines;
    std::uint64_t last_used_frame{0};
  };

  static std::uint64_t compute_style_hash(const ProcFlowFieldConfig& cfg);

  static int quantize_scale_bucket(double units_per_px_mkm);
  static double bucket_to_units_per_px_mkm(int bucket);

  TileEntry& get_or_create_tile(const TileKey& key,
                               double tile_world_mkm,
                               double step_world_mkm,
                               const ProcFlowFieldConfig& cfg);

  void trim_cache(int max_tiles);

  // Mutable caches.
  std::unordered_map<TileKey, TileEntry, TileKeyHash> cache_;

  // Per-frame.
  std::uint64_t frame_index_{0};
  double time_days_{0.0};

  // Used to avoid unbounded cache growth when config changes.
  std::uint64_t last_style_hash_{0};
  bool last_style_hash_valid_{false};

  ProcFlowFieldStats stats_{};
};

} // namespace nebula4x::ui
