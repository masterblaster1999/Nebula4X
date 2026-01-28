#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <SDL.h>

#include "imgui.h"

#include "nebula4x/core/entities.h"

#include "ui/proc_render_engine.h" // UIRendererBackend

namespace nebula4x::ui {

// Procedural anomaly-phenomena sprite engine.
//
// This adds an optional *visual* layer for anomalies in the system map.
// The simulation already provides deterministic anomaly metadata (kind, hazard,
// rewards). This engine renders CPU-generated grayscale sprites (cached +
// uploaded as textures) that are tinted and optionally animated at draw time.
//
// Design goals:
//  - Deterministic visuals per anomaly id/kind (stable across loads).
//  - Works on both OpenGL2 and SDL_Renderer2 ImGui backends (no custom shaders).
//  - Fast: LRU cache + cheap vector overlays for motion.

struct ProcAnomalyPhenomenaSpriteConfig {
  // Raster resolution for generated sprite textures.
  int sprite_px{96};

  // Cache cap (across all anomalies).
  int max_cached_sprites{256};

  // On-screen radius multiplier relative to the base anomaly icon size.
  float size_mult{6.0f};

  // Global opacity multiplier for the phenomena halo.
  float opacity{0.55f};

  // Rotate the sprite over time to fake motion.
  bool animate{true};

  // Rotation speed in cycles/day (0.0 => no rotation).
  float animate_speed_cycles_per_day{0.12f};

  // Pulse alpha with a slow sine (purely cosmetic).
  bool pulse{true};

  // Pulse speed in cycles/day.
  float pulse_speed_cycles_per_day{0.07f};

  // Extra filament overlays rendered as vector geometry.
  bool filaments{true};

  // Filament intensity multiplier.
  float filament_strength{1.0f};

  // Maximum number of filaments drawn per anomaly.
  int filaments_max{7};

  // Overlay the anomaly's deterministic 8x8 signature glyph into the sprite.
  bool glyph_overlay{true};

  // Glyph alpha multiplier.
  float glyph_strength{0.65f};

  // Debug: draw sprite bounds.
  bool debug_bounds{false};
};

struct ProcAnomalyPhenomenaSpriteStats {
  int cache_sprites{0};
  int generated_this_frame{0};
  double gen_ms_this_frame{0.0};
  double upload_ms_this_frame{0.0};
};

class ProcAnomalyPhenomenaSpriteEngine {
 public:
  struct SpriteInfo {
    ImTextureID tex_id{nullptr};
    int w{0};
    int h{0};
  };

  ProcAnomalyPhenomenaSpriteEngine() = default;
  ~ProcAnomalyPhenomenaSpriteEngine();

  ProcAnomalyPhenomenaSpriteEngine(const ProcAnomalyPhenomenaSpriteEngine&) = delete;
  ProcAnomalyPhenomenaSpriteEngine& operator=(const ProcAnomalyPhenomenaSpriteEngine&) = delete;

  void set_backend(UIRendererBackend backend, SDL_Renderer* sdl_renderer);
  bool ready() const;

  void begin_frame();
  void clear();
  void shutdown();

  const ProcAnomalyPhenomenaSpriteStats& stats() const { return stats_; }

  // Returns a cached sprite for this anomaly.
  // seed should be stable (e.g. system seed / map seed) to allow stylistic variation.
  SpriteInfo get_anomaly_sprite(const nebula4x::Anomaly& a, std::uint32_t seed, const ProcAnomalyPhenomenaSpriteConfig& cfg);

  // Draw helper: draw a square sprite rotated about its center.
  static void draw_sprite_rotated(ImDrawList* draw,
                                  ImTextureID tex,
                                  const ImVec2& center,
                                  float size_px,
                                  float angle_rad,
                                  ImU32 tint);

  // Draw helper: draw anomaly filaments as noisy arcs/rays.
  // Separate from the cached sprite so it can animate cheaply.
  static void draw_filaments(ImDrawList* draw,
                             const ImVec2& center,
                             float radius_px,
                             const nebula4x::Anomaly& a,
                             std::uint32_t seed,
                             double time_days,
                             ImU32 tint,
                             const ProcAnomalyPhenomenaSpriteConfig& cfg);

 private:
  struct AnomalyKey {
    std::uint64_t id_hash{0};
    std::uint32_t seed{0};
    std::uint16_t sprite_px{0};
    std::uint16_t variant{0};
    std::uint64_t style_hash{0};

    bool operator==(const AnomalyKey& o) const {
      return id_hash == o.id_hash && seed == o.seed && sprite_px == o.sprite_px && variant == o.variant &&
             style_hash == o.style_hash;
    }
  };

  struct AnomalyKeyHash {
    std::size_t operator()(const AnomalyKey& k) const noexcept;
  };

  struct CacheEntry {
    SpriteInfo sprite;
    std::uint64_t last_used_frame{0};
  };

  static std::uint64_t style_hash_from_cfg(const ProcAnomalyPhenomenaSpriteConfig& cfg);

  SpriteInfo get_or_create(const AnomalyKey& key, const nebula4x::Anomaly& a, const ProcAnomalyPhenomenaSpriteConfig& cfg);

  ImTextureID upload_rgba(const std::uint8_t* rgba, int w, int h);
  void destroy_texture(ImTextureID id);
  void trim_cache(std::size_t max_entries);

  static void raster_anomaly(std::vector<std::uint8_t>& rgba,
                             int w,
                             int h,
                             std::uint32_t seed,
                             std::uint64_t id_hash,
                             std::uint16_t variant,
                             const nebula4x::Anomaly& a,
                             const ProcAnomalyPhenomenaSpriteConfig& cfg);

  UIRendererBackend backend_{UIRendererBackend::Unknown};
  SDL_Renderer* sdl_renderer_{nullptr};

  std::uint64_t frame_{0};
  ProcAnomalyPhenomenaSpriteStats stats_{};

  std::unordered_map<AnomalyKey, CacheEntry, AnomalyKeyHash> cache_{};
};

} // namespace nebula4x::ui
