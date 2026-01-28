#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL.h>

#include "imgui.h"

#include "nebula4x/core/entities.h"

#include "ui/proc_render_engine.h" // for UIRendererBackend

namespace nebula4x::ui {

// A lightweight CPU-rasterized icon engine for the system map.
//
// Motivation:
// - The prototype map used simple geometric primitives (circles, Xs, text glyphs)
//   for ships, missiles, wrecks, and anomalies.
// - Those work, but at typical zoom levels they are visually ambiguous and don't
//   convey motion/orientation.
//
// This engine procedurally generates small grayscale sprite textures on the CPU
// (one-time per unique key) and uploads them to the active UI renderer backend
// (OpenGL2 or SDL_Renderer2). The map then draws these sprites with per-entity
// tinting + rotation using ImDrawList::AddImageQuad.

enum class ProcIconKind : std::uint8_t {
  Ship = 0,
  Missile = 1,
  Wreck = 2,
  Anomaly = 3,
};

struct ProcIconSpriteConfig {
  // Sprite raster resolution. Larger => crisper silhouettes at the cost of CPU
  // generation and GPU memory.
  int sprite_px{64};

  // Maximum cached sprites across all icon kinds.
  int max_cached_sprites{768};

  // --- Ship icon draw options (system map) ---
  float ship_icon_size_px{18.0f};
  bool ship_thrusters{true};
  float ship_thruster_opacity{0.60f};
  float ship_thruster_length_px{14.0f};
  float ship_thruster_width_px{7.0f};

  // --- Other icons ---
  float missile_icon_size_px{10.0f};
  float wreck_icon_size_px{14.0f};
  float anomaly_icon_size_px{16.0f};
  bool anomaly_pulse{true};

  // Debug option: draw icon bounds when rendering.
  bool debug_bounds{false};
};

struct ProcIconSpriteStats {
  int cache_sprites{0};
  int generated_this_frame{0};
  double gen_ms_this_frame{0.0};
  double upload_ms_this_frame{0.0};
};

class ProcIconSpriteEngine {
 public:
  struct SpriteInfo {
    ImTextureID tex_id{};
    int w{0};
    int h{0};
  };

  ProcIconSpriteEngine() = default;
  ~ProcIconSpriteEngine();

  ProcIconSpriteEngine(const ProcIconSpriteEngine&) = delete;
  ProcIconSpriteEngine& operator=(const ProcIconSpriteEngine&) = delete;

  void set_backend(UIRendererBackend backend, SDL_Renderer* sdl_renderer);
  bool ready() const;

  void begin_frame();
  void clear();
  void shutdown();

  const ProcIconSpriteStats& stats() const { return stats_; }

  // Icon retrieval (cached + lazily generated).
  SpriteInfo get_ship_icon(const nebula4x::Ship& ship,
                           const nebula4x::ShipDesign* design,
                           std::uint32_t seed,
                           const ProcIconSpriteConfig& cfg);
  SpriteInfo get_missile_icon(std::uint32_t seed, const ProcIconSpriteConfig& cfg);
  SpriteInfo get_wreck_icon(const nebula4x::Wreck& wreck, std::uint32_t seed, const ProcIconSpriteConfig& cfg);
  SpriteInfo get_anomaly_icon(const nebula4x::Anomaly& anomaly, std::uint32_t seed, const ProcIconSpriteConfig& cfg);

  // Draw helper: draw a square icon rotated about its center.
  // size_px is the on-screen size (width==height).
  static void draw_icon_rotated(ImDrawList* draw,
                                ImTextureID tex,
                                const ImVec2& center,
                                float size_px,
                                float angle_rad,
                                ImU32 tint);

  // Draw helper: ship thruster plume behind the ship.
  // heading_rad should match the icon rotation.
  static void draw_thruster_plume(ImDrawList* draw,
                                  const ImVec2& center,
                                  float heading_rad,
                                  float speed01,
                                  ImU32 base_col,
                                  const ProcIconSpriteConfig& cfg);

 private:
  struct IconKey {
    ProcIconKind kind{ProcIconKind::Ship};
    std::uint64_t id_hash{0};
    std::uint32_t seed{0};
    std::uint16_t sprite_px{0};
    std::uint16_t variant{0};
    std::uint64_t style_hash{0};

    bool operator==(const IconKey& o) const {
      return kind == o.kind && id_hash == o.id_hash && seed == o.seed && sprite_px == o.sprite_px &&
             variant == o.variant && style_hash == o.style_hash;
    }
  };

  struct IconKeyHash {
    std::size_t operator()(const IconKey& k) const noexcept;
  };

  struct CacheEntry {
    SpriteInfo sprite;
    std::uint64_t last_used_frame{0};
  };

  SpriteInfo get_or_create(const IconKey& key,
                           const nebula4x::ShipDesign* ship_design,
                           const nebula4x::Wreck* wreck,
                           const nebula4x::Anomaly* anomaly,
                           const ProcIconSpriteConfig& cfg);

  ImTextureID upload_rgba(const std::uint8_t* rgba, int w, int h);
  void destroy_texture(ImTextureID id);
  void trim_cache(std::size_t max_entries);

  // Icon rasterizers.
  static void raster_ship(std::vector<std::uint8_t>& rgba,
                          int w,
                          int h,
                          std::uint32_t seed,
                          std::uint64_t design_hash,
                          std::uint16_t variant,
                          const nebula4x::ShipDesign* design);
  static void raster_missile(std::vector<std::uint8_t>& rgba, int w, int h, std::uint32_t seed, std::uint16_t variant);
  static void raster_wreck(std::vector<std::uint8_t>& rgba,
                           int w,
                           int h,
                           std::uint32_t seed,
                           std::uint64_t id_hash,
                           std::uint16_t variant,
                           const nebula4x::Wreck* wreck);
  static void raster_anomaly(std::vector<std::uint8_t>& rgba,
                             int w,
                             int h,
                             std::uint32_t seed,
                             std::uint64_t kind_hash,
                             std::uint16_t variant,
                             const nebula4x::Anomaly* anomaly);

  static std::uint64_t hash_string_fnv1a(const std::string& s);
  static std::uint64_t style_hash_from_cfg(const ProcIconSpriteConfig& cfg);

  UIRendererBackend backend_{UIRendererBackend::Unknown};
  SDL_Renderer* sdl_renderer_{nullptr};

  std::uint64_t frame_{0};
  ProcIconSpriteStats stats_{};

  std::unordered_map<IconKey, CacheEntry, IconKeyHash> cache_{};
};

} // namespace nebula4x::ui
