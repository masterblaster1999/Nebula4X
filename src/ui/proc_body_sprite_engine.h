#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <SDL.h>

#include <imgui.h>

#include "nebula4x/core/entities.h"
#include "nebula4x/core/vec2.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

struct ProcBodySpriteConfig {
  // Base sprite resolution (pixels). Higher values look slightly better when zoomed
  // in but cost more CPU time on cache misses.
  int sprite_px = 96;

  // Maximum number of cached sprites (LRU). Each 96x96 RGBA sprite is ~36 KiB.
  int max_cached_sprites = 256;

  // Quantization steps for lighting direction (reduces churn when bodies move).
  int light_steps = 32;

  // Optional rings for gas giants.
  bool enable_rings = true;
  float ring_probability = 0.25f;

  // Shading controls.
  float ambient = 0.22f;          // 0..1
  float diffuse_strength = 0.88f; // 0..2
  float specular_strength = 0.28f;
  float specular_power = 24.0f;

  bool debug_bounds = false;
};

struct ProcBodySpriteStats {
  int cache_sprites = 0;
  int generated_this_frame = 0;
  double gen_ms_this_frame = 0.0;
  double upload_ms_this_frame = 0.0;
};

// CPU procedural body-sprite renderer + GPU uploader.
//
// Generates small RGBA textures for celestial bodies (planets/gas giants/stars/etc)
// and caches them. Works in both the OpenGL2 and SDL_Renderer2 ImGui backends.
class ProcBodySpriteEngine {
 public:
  struct SpriteInfo {
    ImTextureID tex_id{};
    int w = 0;
    int h = 0;

    // Radius of the main sphere in normalized texture space (0..1 relative to
    // half-size). If this is <1 (e.g. rings), the caller can scale the drawn quad
    // so the sphere itself stays the desired size.
    float sphere_radius_norm = 1.0f;
  };

  ProcBodySpriteEngine() = default;
  ~ProcBodySpriteEngine();

  ProcBodySpriteEngine(const ProcBodySpriteEngine&) = delete;
  ProcBodySpriteEngine& operator=(const ProcBodySpriteEngine&) = delete;

  void set_backend(UIRendererBackend backend, SDL_Renderer* sdl_renderer);
  bool ready() const;

  void begin_frame();
  void shutdown();
  void clear();

  const ProcBodySpriteStats& stats() const { return stats_; }

  // Get or generate a sprite for a body.
  //
  // seed: an additional deterministic seed (e.g. system ID hash) to allow
  // per-save/per-map variation without touching the simulation state.
  // light_dir: direction from the body towards the primary light source
  // (in world coordinates). If zero, a default direction is used.
  SpriteInfo get_body_sprite(const nebula4x::Body& body,
                             std::uint32_t seed,
                             const Vec2& light_dir,
                             const ProcBodySpriteConfig& cfg);

 private:
  struct SpriteKey {
    std::uint64_t body_id = 0;
    std::uint32_t seed = 0;
    std::uint32_t size_px = 0;
    std::uint16_t light_step = 0;
    std::uint16_t variant = 0;
    std::uint64_t style_hash = 0;

    bool operator==(const SpriteKey& o) const noexcept {
      return body_id == o.body_id && seed == o.seed && size_px == o.size_px &&
             light_step == o.light_step && variant == o.variant && style_hash == o.style_hash;
    }
  };

  struct SpriteKeyHash {
    std::size_t operator()(const SpriteKey& k) const noexcept;
  };

  struct SpriteEntry {
    ImTextureID tex_id{};
    int w = 0;
    int h = 0;
    float sphere_radius_norm = 1.0f;
    std::uint64_t last_used_frame = 0;
  };

  using Cache = std::unordered_map<SpriteKey, SpriteEntry, SpriteKeyHash>;

  static std::uint64_t compute_style_hash(const ProcBodySpriteConfig& cfg);

  static std::uint32_t quantize_light_step(const Vec2& light_dir, int steps);

  void trim_cache(int max_sprites);
  void destroy_sprite(SpriteEntry& entry);

  // Backend upload.
  ImTextureID upload_rgba(const std::uint8_t* rgba, int w, int h);

  // Procedural generation.
  static void generate_sprite_rgba(std::vector<std::uint8_t>& out,
                                   int w,
                                   int h,
                                   float sphere_radius_norm,
                                   const nebula4x::Body& body,
                                   std::uint32_t seed,
                                   float light_angle_rad,
                                   const ProcBodySpriteConfig& cfg,
                                   std::uint16_t variant);

  static float compute_sphere_radius_norm(const nebula4x::Body& body,
                                          std::uint32_t seed,
                                          const ProcBodySpriteConfig& cfg,
                                          std::uint16_t* out_variant);

  UIRendererBackend backend_ = UIRendererBackend::SDLRenderer2;
  SDL_Renderer* sdl_renderer_ = nullptr;

  std::uint64_t frame_index_ = 0;
  ProcBodySpriteStats stats_ = {};
  Cache cache_;
};

}  // namespace nebula4x::ui
