#pragma once

#include <cstdint>

#include <SDL.h>

#include "imgui.h"

#include "nebula4x/core/entities.h"

#include "ui/proc_render_engine.h" // UIRendererBackend

namespace nebula4x::ui {

// Procedural jump-point phenomena sprite engine.
//
// This adds an optional *visual* layer for jump points in the system map.
// The simulation already generates deterministic jump phenomena values
// (stability / turbulence / shear). This engine renders them as CPU-generated
// grayscale sprites (cached + uploaded as textures) that can be tinted and
// rotated at draw time.

struct ProcJumpPhenomenaSpriteConfig {
  // Raster resolution for the generated sprite textures.
  int sprite_px{96};

  // Cache cap (across all jump points).
  int max_cached_sprites{256};

  // On-screen radius multiplier relative to the base jump glyph radius.
  float size_mult{5.6f};

  // Global opacity multiplier for the phenomena halo.
  float opacity{0.55f};

  // Rotate the sprite over time to fake motion.
  bool animate{true};

  // Rotation speed in cycles/day (0.0 => no rotation).
  float animate_speed_cycles_per_day{0.14f};

  // Pulse alpha with a slow sine. (Purely cosmetic.)
  bool pulse{true};

  // Pulse speed in cycles/day.
  float pulse_speed_cycles_per_day{0.08f};

  // Extra filament overlays (shear-driven) rendered as vector geometry.
  bool filaments{true};

  // Filament intensity multiplier.
  float filament_strength{1.0f};

  // Maximum number of filaments drawn per jump point.
  int filaments_max{6};

  // Debug: draw sprite bounds.
  bool debug_bounds{false};
};

struct ProcJumpPhenomenaSpriteStats {
  int cache_sprites{0};
  int generated_this_frame{0};
  double gen_ms_this_frame{0.0};
  double upload_ms_this_frame{0.0};
};

class ProcJumpPhenomenaSpriteEngine {
 public:
  struct SpriteInfo {
    ImTextureID tex_id{nullptr};
    int w{0};
    int h{0};
  };

  ProcJumpPhenomenaSpriteEngine() = default;
  ~ProcJumpPhenomenaSpriteEngine();

  ProcJumpPhenomenaSpriteEngine(const ProcJumpPhenomenaSpriteEngine&) = delete;
  ProcJumpPhenomenaSpriteEngine& operator=(const ProcJumpPhenomenaSpriteEngine&) = delete;

  void set_backend(UIRendererBackend backend, SDL_Renderer* sdl_renderer);
  bool ready() const;

  void begin_frame();
  void clear();
  void shutdown();

  const ProcJumpPhenomenaSpriteStats& stats() const { return stats_; }

  // Returns a cached sprite for this jump point.
  // seed should be stable (e.g. system seed / map seed) to allow stylistic variation.
  SpriteInfo get_jump_sprite(const nebula4x::JumpPoint& jp, std::uint32_t seed, const ProcJumpPhenomenaSpriteConfig& cfg);

  // Draw helper: draw a square sprite rotated about its center.
  // size_px is the on-screen size (width==height).
  static void draw_sprite_rotated(ImDrawList* draw,
                                  ImTextureID tex,
                                  const ImVec2& center,
                                  float size_px,
                                  float angle_rad,
                                  ImU32 tint);

  // Draw helper: draw shear filaments as noisy arcs/rays.
  // This is separate from the cached sprite so it can animate cheaply.
  static void draw_filaments(ImDrawList* draw,
                             const ImVec2& center,
                             float radius_px,
                             std::uint32_t seed,
                             float shear01,
                             float turbulence01,
                             double time_days,
                             ImU32 tint,
                             const ProcJumpPhenomenaSpriteConfig& cfg);

 private:
  struct JumpKey {
    std::uint64_t id_hash{0};
    std::uint32_t seed{0};
    std::uint16_t sprite_px{0};
    std::uint16_t variant{0};
    std::uint64_t style_hash{0};

    bool operator==(const JumpKey& o) const {
      return id_hash == o.id_hash && seed == o.seed && sprite_px == o.sprite_px && variant == o.variant &&
             style_hash == o.style_hash;
    }
  };

  struct JumpKeyHash {
    std::size_t operator()(const JumpKey& k) const noexcept;
  };

  struct CacheEntry {
    SpriteInfo sprite;
    std::uint64_t last_used_frame{0};
  };

  static std::uint64_t style_hash_from_cfg(const ProcJumpPhenomenaSpriteConfig& cfg);

  SpriteInfo get_or_create(const JumpKey& key, const nebula4x::JumpPoint& jp, const ProcJumpPhenomenaSpriteConfig& cfg);

  ImTextureID upload_rgba(const std::uint8_t* rgba, int w, int h);
  void destroy_texture(ImTextureID id);
  void trim_cache(std::size_t max_entries);

  static void raster_jump(std::vector<std::uint8_t>& rgba,
                          int w,
                          int h,
                          std::uint32_t seed,
                          std::uint64_t id_hash,
                          std::uint16_t variant,
                          const nebula4x::JumpPoint& jp,
                          const ProcJumpPhenomenaSpriteConfig& cfg);

  UIRendererBackend backend_{UIRendererBackend::Unknown};
  SDL_Renderer* sdl_renderer_{nullptr};

  std::uint64_t frame_{0};
  ProcJumpPhenomenaSpriteStats stats_{};

  std::unordered_map<JumpKey, CacheEntry, JumpKeyHash> cache_{};
};

} // namespace nebula4x::ui
