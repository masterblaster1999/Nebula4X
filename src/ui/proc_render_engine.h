#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <SDL.h>

#include <imgui.h>

#include "ui/ui_state.h"

namespace nebula4x::ui {

// A lightweight, fully deterministic procedural background *engine*.
//
// - Generates tile textures on-demand (CPU rasterization into RGBA8).
// - Uploads tiles to the active UI renderer backend (OpenGL2 or SDL_Renderer2).
// - Maintains an LRU cache of textures so panning/zooming stays smooth.
//
// This engine is UI-first (ImDrawList/ImTextureID) and intentionally avoids any
// custom GPU shaders so it works in both OpenGL and the SDL_Renderer fallback.
//
// Notes on ImGui texture handles:
// - When using Dear ImGui's SDL_Renderer backend, ImTextureID is expected to be an SDL_Texture*.
//   (See imgui_impl_sdlrenderer2.* in the ImGui repository.)

struct ProcRenderConfig {
  // Tile resolution in pixels. Lower values reduce generation cost but show more tiling.
  int tile_px = 256;

  // Maximum number of cached tiles across all layers.
  int max_cached_tiles = 96;

  // Controls star density; this typically maps to UIState::map_starfield_density.
  float star_density = 1.0f;

  // Base parallax factor; maps to UIState::map_starfield_parallax.
  float parallax = 0.15f;

  // Nebula layer controls.
  bool nebula_enable = true;
  float nebula_strength = 0.35f; // 0..1
  float nebula_scale = 1.0f;     // >0
  float nebula_warp = 0.70f;     // 0..2

  // Debug visuals.
  bool debug_show_tile_bounds = false;
};

struct ProcRenderStats {
  int cache_tiles = 0;
  int generated_this_frame = 0;
  double gen_ms_this_frame = 0.0;
  double upload_ms_this_frame = 0.0;
};

class ProcRenderEngine {
 public:
  ProcRenderEngine();
  ~ProcRenderEngine();

  ProcRenderEngine(const ProcRenderEngine&) = delete;
  ProcRenderEngine& operator=(const ProcRenderEngine&) = delete;

  void set_backend(UIRendererBackend backend, SDL_Renderer* sdl_renderer);

  // Must be called before destroying the graphics backend (GL context / SDL_Renderer).
  void shutdown();

  // Start-of-frame bookkeeping (LRU / per-frame stats).
  void begin_frame();

  // Drop cached tiles (forces regeneration/upload on next draw).
  void clear();

  // Draw a multi-layer procedural background into an ImDrawList.
  //
  // `offset_px_*` should be the map pan in pixels (world -> screen), so the
  // procedural field "sticks" to the galaxy as you pan/zoom.
  void draw_background(ImDrawList* draw,
                       const ImVec2& origin,
                       const ImVec2& size,
                       ImU32 tint,
                       float offset_px_x,
                       float offset_px_y,
                       std::uint32_t seed,
                       const ProcRenderConfig& cfg);

  const ProcRenderStats& stats() const { return stats_; }
  UIRendererBackend backend() const { return backend_; }
  bool ready() const;

 private:
  struct TileKey {
    int layer = 0;
    int tx = 0;
    int ty = 0;
    int tile_px = 256;
    std::uint32_t seed = 0;
    std::uint64_t style_hash = 0;

    bool operator==(const TileKey& o) const {
      return layer == o.layer && tx == o.tx && ty == o.ty && tile_px == o.tile_px && seed == o.seed &&
             style_hash == o.style_hash;
    }
  };

  struct TileKeyHash {
    std::size_t operator()(const TileKey& k) const noexcept;
  };

  struct TileEntry {
    // Backend handle is stored in the union-ish way so we can destroy correctly.
    // (ImTextureID may be a pointer or an integer depending on the ImGui version).
    ImTextureID tex_id{};
    std::uint64_t last_used_frame = 0;
    int w = 0;
    int h = 0;
  };

  ImTextureID get_or_create_tile(const TileKey& key, const ProcRenderConfig& cfg);
  void destroy_tile(TileEntry& entry);
  void trim_cache(int max_tiles);

  // --- Backend upload ---
  ImTextureID upload_rgba_tile(const std::uint8_t* rgba, int w, int h);

  // --- Generation ---
  static void generate_tile_rgba(std::vector<std::uint8_t>& out_rgba,
                                 int w,
                                 int h,
                                 int layer,
                                 int tile_x,
                                 int tile_y,
                                 std::uint32_t seed,
                                 const ProcRenderConfig& cfg);

  static std::uint64_t compute_style_hash(const ProcRenderConfig& cfg);

  UIRendererBackend backend_{UIRendererBackend::OpenGL2};
  SDL_Renderer* sdl_renderer_{nullptr};

  std::unordered_map<TileKey, TileEntry, TileKeyHash> cache_;
  std::uint64_t frame_index_{0};
  ProcRenderStats stats_{};
};

} // namespace nebula4x::ui
