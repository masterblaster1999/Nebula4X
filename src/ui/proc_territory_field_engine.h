#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <imgui.h>

#include "nebula4x/core/ids.h"
#include "nebula4x/core/vec2.h"

namespace nebula4x::ui {

// Galaxy-map procedural territory overlay.
//
// This is a UI-only visualization layer. Call sites gather and pass influence
// sources (typically colonies) so fog-of-war rules can be applied without
// leaking information.

struct ProcTerritorySource {
  Vec2 pos{0.0, 0.0};
  Id faction_id{kInvalidId};
  float population_millions{0.0f};
};

struct ProcTerritoryFieldConfig {
  bool enabled{false};

  // Approx tile size in screen pixels.
  int tile_px{420};

  // Maximum number of cached tiles.
  int max_cached_tiles{220};

  // Low-res evaluation grid per tile (N x N).
  int samples_per_tile{28};

  bool draw_fill{true};
  bool draw_boundaries{true};

  float fill_opacity{0.16f};
  float boundary_opacity{0.42f};
  float boundary_thickness_px{1.6f};

  // Influence radius model, expressed in multiples of an estimated average
  // system spacing (world units).
  float influence_base_spacing_mult{1.10f};
  float influence_pop_spacing_mult{0.28f};
  float influence_pop_log_bias{5.0f};

  float presence_falloff_spacing{2.0f};
  float dominance_softness_spacing{0.65f};

  int max_sources{512};

  bool contested_dither{true};
  float contested_threshold{0.22f};
  float contested_dither_strength{0.55f};

  bool debug_tile_bounds{false};
};

struct ProcTerritoryFieldStats {
  int cache_tiles{0};
  int tiles_used_this_frame{0};
  int tiles_generated_this_frame{0};
  int cells_drawn{0};
  double gen_ms_this_frame{0.0};
};

class ProcTerritoryFieldEngine {
 public:
  void begin_frame();
  void clear();

  ProcTerritoryFieldStats stats() const { return stats_; }

  void draw_territories(ImDrawList* draw,
                        const ImVec2& origin,
                        const ImVec2& size,
                        const ImVec2& center_px,
                        double scale_px_per_unit,
                        double zoom,
                        const Vec2& pan,
                        const std::vector<ProcTerritorySource>& sources,
                        double system_spacing_units,
                        std::uint32_t seed,
                        const ProcTerritoryFieldConfig& cfg);

 private:
  struct EvalSource {
    Vec2 pos{0.0, 0.0};
    float radius{0.0f};
    int faction_index{-1};
    Id faction_id{kInvalidId};
  };

  struct TileKey {
    int tx{0};
    int ty{0};
    int scale_bucket{0};
    int tile_px{0};
    int samples{0};
    std::uint32_t seed{0};
    std::uint64_t sources_hash{0};
    std::uint64_t style_hash{0};

    bool operator==(const TileKey& o) const {
      return tx == o.tx && ty == o.ty && scale_bucket == o.scale_bucket && tile_px == o.tile_px && samples == o.samples &&
             seed == o.seed && sources_hash == o.sources_hash && style_hash == o.style_hash;
    }
  };

  struct TileKeyHash {
    std::size_t operator()(const TileKey& k) const noexcept;
  };

  struct Cell {
    int faction_index{-1};
    float alpha{0.0f};
    float dominance{0.0f};
    bool boundary{false};
  };

  struct Tile {
    std::uint64_t last_used_frame{0};
    int grid{0};
    std::vector<Cell> cells;
  };

  static std::uint64_t compute_style_hash(const ProcTerritoryFieldConfig& cfg);
  static int quantize_scale_bucket(double units_per_px);
  static double bucket_to_units_per_px(int bucket);

  static std::uint64_t compute_sources_hash(const std::vector<EvalSource>& eval_sources,
                                            double spacing_units,
                                            const ProcTerritoryFieldConfig& cfg);

  Tile& get_or_build_tile(const TileKey& key,
                          double tile_world_units,
                          const std::vector<EvalSource>& eval_sources,
                          int faction_count,
                          double spacing_units,
                          const ProcTerritoryFieldConfig& cfg);

  void trim_cache(int max_tiles);

  std::unordered_map<TileKey, Tile, TileKeyHash> cache_;
  ProcTerritoryFieldStats stats_{};
  std::uint64_t frame_index_{0};
};

} // namespace nebula4x::ui
