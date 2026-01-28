#include "ui/proc_icon_sprite_engine.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "nebula4x/util/log.h"

#include "ui/imgui_texture.h"

#if NEBULA4X_UI_RENDERER_OPENGL2
#include <SDL_opengl.h>
#endif

namespace nebula4x::ui {

namespace {

using Clock = std::chrono::high_resolution_clock;

static inline double ms_since(const Clock::time_point& start) {
  const auto end = Clock::now();
  const std::chrono::duration<double, std::milli> dt = end - start;
  return dt.count();
}

// --- Hashing / deterministic RNG ------------------------------------------------

static inline std::uint32_t hash_u32(std::uint32_t x) {
  // A small mix (public domain style) suitable for deterministic content.
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

static inline std::uint64_t hash_combine_u64(std::uint64_t h, std::uint64_t v) {
  return h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2));
}

static inline float rand01(std::uint32_t& state) {
  state = hash_u32(state);
  return static_cast<float>(state & 0x00FFFFFFu) * (1.0f / 16777215.0f);
}

static inline float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

static inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

static inline float smoothstep(float e0, float e1, float x) {
  const float t = clamp01((x - e0) / (e1 - e0));
  return t * t * (3.0f - 2.0f * t);
}

static inline float fade(float t) {
  // Perlin fade curve.
  return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static inline std::uint32_t hash_2d_i32(int x, int y, std::uint32_t seed) {
  std::uint32_t h = seed;
  h ^= hash_u32(static_cast<std::uint32_t>(x) * 0x9E3779B9u);
  h ^= hash_u32(static_cast<std::uint32_t>(y) * 0x85EBCA6Bu);
  return hash_u32(h);
}

static inline float hash_to_01(std::uint32_t h) {
  return static_cast<float>(h & 0x00FFFFFFu) * (1.0f / 16777215.0f);
}

static float value_noise2(float x, float y, std::uint32_t seed) {
  const int xi = static_cast<int>(std::floor(x));
  const int yi = static_cast<int>(std::floor(y));
  const float xf = x - static_cast<float>(xi);
  const float yf = y - static_cast<float>(yi);

  const float u = fade(xf);
  const float v = fade(yf);

  const float n00 = hash_to_01(hash_2d_i32(xi, yi, seed));
  const float n10 = hash_to_01(hash_2d_i32(xi + 1, yi, seed));
  const float n01 = hash_to_01(hash_2d_i32(xi, yi + 1, seed));
  const float n11 = hash_to_01(hash_2d_i32(xi + 1, yi + 1, seed));

  const float x0 = lerp(n00, n10, u);
  const float x1 = lerp(n01, n11, u);
  return lerp(x0, x1, v);
}

static float fbm2(float x, float y, std::uint32_t seed, int octaves, float lacunarity, float gain) {
  float sum = 0.0f;
  float amp = 0.5f;
  float fx = x;
  float fy = y;
  for (int i = 0; i < octaves; ++i) {
    sum += amp * value_noise2(fx, fy, seed + static_cast<std::uint32_t>(i) * 1013u);
    fx *= lacunarity;
    fy *= lacunarity;
    amp *= gain;
  }
  return clamp01(sum);
}

// --- 2D SDF helpers -------------------------------------------------------------

struct V2 {
  float x;
  float y;
};

static inline V2 v2(float x, float y) { return V2{x, y}; }

static inline V2 operator+(V2 a, V2 b) { return V2{a.x + b.x, a.y + b.y}; }
static inline V2 operator-(V2 a, V2 b) { return V2{a.x - b.x, a.y - b.y}; }
static inline V2 operator*(V2 a, float s) { return V2{a.x * s, a.y * s}; }

static inline float dot(V2 a, V2 b) { return a.x * b.x + a.y * b.y; }

static inline float length(V2 a) { return std::sqrt(dot(a, a)); }

static inline V2 abs(V2 a) { return V2{std::fabs(a.x), std::fabs(a.y)}; }

static inline V2 max(V2 a, float b) { return V2{std::max(a.x, b), std::max(a.y, b)}; }

static float sd_circle(V2 p, float r) { return length(p) - r; }

static float sd_box(V2 p, V2 b) {
  // signed distance to axis-aligned box centered at origin with half-extents b.
  const V2 d = abs(p) - b;
  const V2 d0 = max(d, 0.0f);
  return length(d0) + std::min(std::max(d.x, d.y), 0.0f);
}

static float sd_round_box(V2 p, V2 b, float r) {
  return sd_box(p, V2{b.x - r, b.y - r}) - r;
}

static float sd_capsule(V2 p, V2 a, V2 b, float r) {
  // Capsule distance to segment a-b.
  const V2 pa = p - a;
  const V2 ba = b - a;
  const float h = clamp01(dot(pa, ba) / std::max(1e-6f, dot(ba, ba)));
  return length(pa - ba * h) - r;
}

static V2 rotate(V2 p, float c, float s) {
  return V2{p.x * c - p.y * s, p.x * s + p.y * c};
}

static inline std::uint8_t f2b(float v) {
  const int iv = static_cast<int>(std::lround(clamp01(v) * 255.0f));
  return static_cast<std::uint8_t>(std::clamp(iv, 0, 255));
}

} // namespace

// --- ProcIconSpriteEngine -------------------------------------------------------

ProcIconSpriteEngine::~ProcIconSpriteEngine() { shutdown(); }

std::size_t ProcIconSpriteEngine::IconKeyHash::operator()(const IconKey& k) const noexcept {
  std::uint64_t h = 0xcbf29ce484222325ull;
  h = hash_combine_u64(h, static_cast<std::uint64_t>(k.kind));
  h = hash_combine_u64(h, k.id_hash);
  h = hash_combine_u64(h, static_cast<std::uint64_t>(k.seed));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(k.sprite_px));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(k.variant));
  h = hash_combine_u64(h, k.style_hash);
  return static_cast<std::size_t>(h);
}

void ProcIconSpriteEngine::set_backend(UIRendererBackend backend, SDL_Renderer* sdl_renderer) {
  // If backend changes, drop cached textures (they are backend-specific).
  if (backend != backend_ || sdl_renderer != sdl_renderer_) {
    shutdown();
  }
  backend_ = backend;
  sdl_renderer_ = sdl_renderer;
}

bool ProcIconSpriteEngine::ready() const {
  if (backend_ == UIRendererBackend::SDLRenderer2) return sdl_renderer_ != nullptr;
#if NEBULA4X_UI_RENDERER_OPENGL2
  if (backend_ == UIRendererBackend::OpenGL2) return true;
#endif
  return false;
}

void ProcIconSpriteEngine::begin_frame() {
  ++frame_;
  stats_.cache_sprites = static_cast<int>(cache_.size());
  stats_.generated_this_frame = 0;
  stats_.gen_ms_this_frame = 0.0;
  stats_.upload_ms_this_frame = 0.0;
}

void ProcIconSpriteEngine::destroy_texture(ImTextureID id) {
  if (!imgui_texture_id_is_valid(id)) return;
  if (backend_ == UIRendererBackend::SDLRenderer2) {
    SDL_DestroyTexture(sdl_texture_from_imgui_texture_id(id));
    return;
  }
#if NEBULA4X_UI_RENDERER_OPENGL2
  if (backend_ == UIRendererBackend::OpenGL2) {
    const GLuint tex = gl_texture_from_imgui_texture_id<GLuint>(id);
    if (tex != 0) glDeleteTextures(1, &tex);
  }
#endif
}

void ProcIconSpriteEngine::shutdown() {
  for (auto& [k, e] : cache_) {
    destroy_texture(e.sprite.tex_id);
    e.sprite.tex_id = imgui_null_texture_id();
  }
  cache_.clear();
}

void ProcIconSpriteEngine::clear() { shutdown(); }

void ProcIconSpriteEngine::trim_cache(std::size_t max_entries) {
  if (max_entries < 1) max_entries = 1;
  if (cache_.size() <= max_entries) return;

  // Collect keys sorted by LRU.
  std::vector<std::pair<std::uint64_t, IconKey>> order;
  order.reserve(cache_.size());
  for (const auto& [k, e] : cache_) {
    order.emplace_back(e.last_used_frame, k);
  }
  std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  const std::size_t to_evict = cache_.size() - max_entries;
  for (std::size_t i = 0; i < to_evict; ++i) {
    const IconKey& k = order[i].second;
    auto it = cache_.find(k);
    if (it != cache_.end()) {
      destroy_texture(it->second.sprite.tex_id);
      cache_.erase(it);
    }
  }
}

ImTextureID ProcIconSpriteEngine::upload_rgba(const std::uint8_t* rgba, int w, int h) {
  if (!rgba || w <= 0 || h <= 0) return imgui_null_texture_id();
  if (!ready()) return imgui_null_texture_id();

  if (backend_ == UIRendererBackend::SDLRenderer2) {
    std::uint32_t rmask, gmask, bmask, amask;
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
    rmask = 0xff000000u;
    gmask = 0x00ff0000u;
    bmask = 0x0000ff00u;
    amask = 0x000000ffu;
#else
    rmask = 0x000000ffu;
    gmask = 0x0000ff00u;
    bmask = 0x00ff0000u;
    amask = 0xff000000u;
#endif

    SDL_Surface* surf = SDL_CreateRGBSurfaceFrom(const_cast<std::uint8_t*>(rgba), w, h, 32, w * 4, rmask, gmask,
                                                 bmask, amask);
    if (!surf) {
      nebula4x::log::warn(std::string("ProcIconSpriteEngine: SDL_CreateRGBSurfaceFrom failed: ") + SDL_GetError());
      return imgui_null_texture_id();
    }

    SDL_Texture* tex = SDL_CreateTextureFromSurface(sdl_renderer_, surf);
    SDL_FreeSurface(surf);

    if (!tex) {
      nebula4x::log::warn(std::string("ProcIconSpriteEngine: SDL_CreateTextureFromSurface failed: ") + SDL_GetError());
      return imgui_null_texture_id();
    }

    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    return imgui_texture_id_from_sdl_texture(tex);
  }

#if NEBULA4X_UI_RENDERER_OPENGL2
  if (backend_ == UIRendererBackend::OpenGL2) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindTexture(GL_TEXTURE_2D, 0);
    return imgui_texture_id_from_gl_texture(tex);
  }
#endif

  return imgui_null_texture_id();
}

std::uint64_t ProcIconSpriteEngine::hash_string_fnv1a(const std::string& s) {
  std::uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) {
    h ^= static_cast<std::uint64_t>(c);
    h *= 1099511628211ull;
  }
  return h;
}

std::uint64_t ProcIconSpriteEngine::style_hash_from_cfg(const ProcIconSpriteConfig& cfg) {
  std::uint64_t h = 0;
  h = hash_combine_u64(h, static_cast<std::uint64_t>(cfg.ship_thrusters ? 1 : 0));
  // We intentionally do not include draw sizes here; icon textures are resolution-dependent, not scale-dependent.
  // Quantize a few floats that influence the *shape* (thruster plume drawn via geometry is not part of texture).
  auto qf = [&](float v, float scale) {
    const long long q = static_cast<long long>(std::llround(static_cast<double>(v) * static_cast<double>(scale)));
    h = hash_combine_u64(h, static_cast<std::uint64_t>(q));
  };
  qf(cfg.ship_thruster_opacity, 1000.0f);
  qf(cfg.ship_thruster_length_px, 10.0f);
  qf(cfg.ship_thruster_width_px, 10.0f);
  h = hash_combine_u64(h, static_cast<std::uint64_t>(cfg.anomaly_pulse ? 1 : 0));
  return h;
}

ProcIconSpriteEngine::SpriteInfo ProcIconSpriteEngine::get_ship_icon(const nebula4x::Ship& ship,
                                                                     const nebula4x::ShipDesign* design,
                                                                     std::uint32_t seed,
                                                                     const ProcIconSpriteConfig& cfg) {
  const std::uint64_t id_hash = ship.design_id.empty() ? static_cast<std::uint64_t>(ship.id)
                                                       : hash_string_fnv1a(ship.design_id);

  // Small variant bucket so visually distinct designs remain stable even if a design_id collides.
  const std::uint16_t variant = static_cast<std::uint16_t>((id_hash ^ static_cast<std::uint64_t>(seed)) & 0xFFu);

  IconKey key;
  key.kind = ProcIconKind::Ship;
  key.id_hash = id_hash;
  key.seed = seed;
  key.sprite_px = static_cast<std::uint16_t>(std::clamp(cfg.sprite_px, 16, 256));
  key.variant = variant;
  key.style_hash = style_hash_from_cfg(cfg);

  return get_or_create(key, design, nullptr, nullptr, cfg);
}

ProcIconSpriteEngine::SpriteInfo ProcIconSpriteEngine::get_missile_icon(std::uint32_t seed,
                                                                        const ProcIconSpriteConfig& cfg) {
  IconKey key;
  key.kind = ProcIconKind::Missile;
  key.id_hash = 0xC0FFEEu;
  key.seed = seed;
  key.sprite_px = static_cast<std::uint16_t>(std::clamp(cfg.sprite_px, 16, 256));
  key.variant = static_cast<std::uint16_t>((seed ^ 0xA5A5A5A5u) & 0xFFu);
  key.style_hash = style_hash_from_cfg(cfg);
  return get_or_create(key, nullptr, nullptr, nullptr, cfg);
}

ProcIconSpriteEngine::SpriteInfo ProcIconSpriteEngine::get_wreck_icon(const nebula4x::Wreck& wreck,
                                                                      std::uint32_t seed,
                                                                      const ProcIconSpriteConfig& cfg) {
  IconKey key;
  key.kind = ProcIconKind::Wreck;
  key.id_hash = static_cast<std::uint64_t>(wreck.id);
  key.seed = seed;
  key.sprite_px = static_cast<std::uint16_t>(std::clamp(cfg.sprite_px, 16, 256));
  key.variant = static_cast<std::uint16_t>((static_cast<std::uint64_t>(wreck.id) ^ seed) & 0xFFu);
  key.style_hash = style_hash_from_cfg(cfg);
  return get_or_create(key, nullptr, &wreck, nullptr, cfg);
}

ProcIconSpriteEngine::SpriteInfo ProcIconSpriteEngine::get_anomaly_icon(const nebula4x::Anomaly& anomaly,
                                                                        std::uint32_t seed,
                                                                        const ProcIconSpriteConfig& cfg) {
  const std::uint64_t kind_hash = anomaly.kind.empty() ? hash_string_fnv1a(anomaly.name) : hash_string_fnv1a(anomaly.kind);
  IconKey key;
  key.kind = ProcIconKind::Anomaly;
  key.id_hash = static_cast<std::uint64_t>(anomaly.id) ^ (kind_hash << 1);
  key.seed = seed;
  key.sprite_px = static_cast<std::uint16_t>(std::clamp(cfg.sprite_px, 16, 256));
  key.variant = static_cast<std::uint16_t>((kind_hash ^ static_cast<std::uint64_t>(seed)) & 0xFFu);
  key.style_hash = style_hash_from_cfg(cfg);
  return get_or_create(key, nullptr, nullptr, &anomaly, cfg);
}

ProcIconSpriteEngine::SpriteInfo ProcIconSpriteEngine::get_or_create(const IconKey& key,
                                                                     const nebula4x::ShipDesign* ship_design,
                                                                     const nebula4x::Wreck* wreck,
                                                                     const nebula4x::Anomaly* anomaly,
                                                                     const ProcIconSpriteConfig& cfg) {
  if (!ready()) return {};

  auto it = cache_.find(key);
  if (it != cache_.end()) {
    it->second.last_used_frame = frame_;
    return it->second.sprite;
  }

  const int w = std::clamp(static_cast<int>(key.sprite_px), 16, 256);
  const int h = w;

  const auto t0 = Clock::now();
  std::vector<std::uint8_t> rgba;
  rgba.resize(static_cast<std::size_t>(w * h * 4), 0);

  const std::uint32_t s = key.seed ^ static_cast<std::uint32_t>(key.id_hash) ^ (static_cast<std::uint32_t>(key.variant) << 16);
  switch (key.kind) {
    case ProcIconKind::Ship:
      raster_ship(rgba, w, h, s, key.id_hash, key.variant, ship_design);
      break;
    case ProcIconKind::Missile:
      raster_missile(rgba, w, h, s, key.variant);
      break;
    case ProcIconKind::Wreck:
      raster_wreck(rgba, w, h, s, key.id_hash, key.variant, wreck);
      break;
    case ProcIconKind::Anomaly:
      raster_anomaly(rgba, w, h, s, key.id_hash, key.variant, anomaly);
      break;
  }
  stats_.gen_ms_this_frame += ms_since(t0);

  const auto t1 = Clock::now();
  ImTextureID tex_id = upload_rgba(rgba.data(), w, h);
  stats_.upload_ms_this_frame += ms_since(t1);

  if (!tex_id) return {};

  CacheEntry e;
  e.sprite.tex_id = tex_id;
  e.sprite.w = w;
  e.sprite.h = h;
  e.last_used_frame = frame_;
  cache_.emplace(key, e);

  stats_.generated_this_frame += 1;
  stats_.cache_sprites = static_cast<int>(cache_.size());

  trim_cache(static_cast<std::size_t>(std::max(16, cfg.max_cached_sprites)));

  return e.sprite;
}

void ProcIconSpriteEngine::draw_icon_rotated(ImDrawList* draw,
                                            ImTextureID tex,
                                            const ImVec2& center,
                                            float size_px,
                                            float angle_rad,
                                            ImU32 tint) {
  if (!draw || !tex) return;
  if (size_px <= 0.0f) return;

  const float h = 0.5f * size_px;
  const float c = std::cos(angle_rad);
  const float s = std::sin(angle_rad);

  // Local quad corners around the origin.
  const ImVec2 q0(-h, -h);
  const ImVec2 q1(+h, -h);
  const ImVec2 q2(+h, +h);
  const ImVec2 q3(-h, +h);

  auto rot = [&](ImVec2 p) -> ImVec2 {
    return ImVec2(center.x + (p.x * c - p.y * s), center.y + (p.x * s + p.y * c));
  };

  const ImVec2 p0 = rot(q0);
  const ImVec2 p1 = rot(q1);
  const ImVec2 p2 = rot(q2);
  const ImVec2 p3 = rot(q3);

  draw->AddImageQuad(tex, p0, p1, p2, p3, ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1), tint);
}

void ProcIconSpriteEngine::draw_thruster_plume(ImDrawList* draw,
                                              const ImVec2& center,
                                              float heading_rad,
                                              float speed01,
                                              ImU32 base_col,
                                              const ProcIconSpriteConfig& cfg) {
  if (!draw) return;
  if (!cfg.ship_thrusters) return;
  speed01 = clamp01(speed01);
  if (speed01 <= 0.02f) return;

  // In icon-space, "forward" is +X. Thruster is behind the ship: -X.
  // Compute a unit vector pointing backwards in screen space.
  const float c = std::cos(heading_rad);
  const float s = std::sin(heading_rad);
  const ImVec2 back(-c, -s);
  const ImVec2 up(-s, +c); // 90 degrees.

  const float len = std::max(2.0f, cfg.ship_thruster_length_px * (0.4f + 0.9f * speed01));
  const float w = std::max(1.5f, cfg.ship_thruster_width_px * (0.55f + 0.6f * speed01));

  const ImVec2 tip(center.x + back.x * len, center.y + back.y * len);
  const ImVec2 a(center.x + up.x * w * 0.5f, center.y + up.y * w * 0.5f);
  const ImVec2 b(center.x - up.x * w * 0.5f, center.y - up.y * w * 0.5f);

  const float alpha = clamp01(cfg.ship_thruster_opacity) * (0.45f + 0.55f * speed01);

  // Bright core (bluish) + faint halo.
  const ImU32 core = IM_COL32(120, 200, 255, static_cast<int>(alpha * 255.0f));
  const ImU32 halo = IM_COL32(80, 160, 255, static_cast<int>(alpha * 120.0f));

  draw->AddTriangleFilled(a, b, tip, halo);
  // Slightly shorter core triangle.
  const ImVec2 tip2(center.x + back.x * (len * 0.72f), center.y + back.y * (len * 0.72f));
  const ImVec2 a2(center.x + up.x * w * 0.32f, center.y + up.y * w * 0.32f);
  const ImVec2 b2(center.x - up.x * w * 0.32f, center.y - up.y * w * 0.32f);
  draw->AddTriangleFilled(a2, b2, tip2, core);

  // Tiny tint hint of the ship's faction color near the exhaust.
  const int a_ship = (base_col >> IM_COL32_A_SHIFT) & 0xFF;
  if (a_ship > 0) {
    const float ah = alpha * 0.18f;
    const ImU32 ship_glow = (base_col & ~IM_COL32_A_MASK) | (static_cast<ImU32>(static_cast<int>(ah * 255.0f)) << IM_COL32_A_SHIFT);
    const ImVec2 tip3(center.x + back.x * (len * 0.45f), center.y + back.y * (len * 0.45f));
    const ImVec2 a3(center.x + up.x * w * 0.22f, center.y + up.y * w * 0.22f);
    const ImVec2 b3(center.x - up.x * w * 0.22f, center.y - up.y * w * 0.22f);
    draw->AddTriangleFilled(a3, b3, tip3, ship_glow);
  }
}

// --- Rasterizers ----------------------------------------------------------------

static inline float role_bias(const nebula4x::ShipDesign* d, nebula4x::ShipRole role) {
  if (!d) return 0.0f;
  return (d->role == role) ? 1.0f : 0.0f;
}

void ProcIconSpriteEngine::raster_ship(std::vector<std::uint8_t>& rgba,
                                      int w,
                                      int h,
                                      std::uint32_t seed,
                                      std::uint64_t design_hash,
                                      std::uint16_t /*variant*/,
                                      const nebula4x::ShipDesign* design) {
  rgba.assign(static_cast<std::size_t>(w * h * 4), 0);
  std::uint32_t s = hash_u32(seed ^ static_cast<std::uint32_t>(design_hash) ^ 0xBADC0DEu);

  // Derived / normalized scale from mass.
  const double mass = design ? std::max(1.0, design->mass_tons) : 2500.0;
  const float mass_n = clamp01(static_cast<float>(std::log10(mass + 1.0) / 6.0));

  // Base dimensions.
  float len = 0.55f + 0.35f * mass_n + rand01(s) * 0.10f;
  float wid = 0.16f + 0.18f * (1.0f - mass_n) + rand01(s) * 0.08f;

  // Role-driven bias.
  const float is_freighter = role_bias(design, nebula4x::ShipRole::Freighter);
  const float is_survey = role_bias(design, nebula4x::ShipRole::Surveyor);
  const float is_warship = role_bias(design, nebula4x::ShipRole::Combatant);

  const bool has_cargo = design && design->cargo_tons > 1.0;
  const bool has_mining = design && design->mining_tons_per_day > 0.0;
  const bool has_sensors = design && design->sensor_range_mkm > 0.0;
  const bool has_weapons =
      design && (design->weapon_damage > 0.0 || design->missile_damage > 0.0 || design->point_defense_damage > 0.0);

  const bool has_colony = design && design->colony_capacity_millions > 0.0;
  const float colony_bias = has_colony ? 1.0f : 0.0f;

  wid *= 1.0f + 0.35f * is_freighter + 0.25f * colony_bias;
  len *= 1.0f + 0.15f * is_warship - 0.08f * is_freighter;

  // Quantize to avoid extremely thin ships.
  len = std::clamp(len, 0.45f, 0.95f);
  wid = std::clamp(wid, 0.12f, 0.55f);

  // Sub-shapes for hull.
  const float nose_sharp = 0.05f + 0.10f * rand01(s) + 0.10f * is_warship;
  const float tail_cut = 0.05f + 0.10f * rand01(s);

  // Small detailing noise seed.
  const std::uint32_t nseed = hash_u32(s ^ 0x1234ABCDu);

  const float inv = 1.0f / static_cast<float>(w);
  const float aa = 2.0f * inv;

  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      // Normalized coordinates in [-1,1].
      const float nx = (static_cast<float>(x) + 0.5f) / static_cast<float>(w) * 2.0f - 1.0f;
      const float ny = (static_cast<float>(y) + 0.5f) / static_cast<float>(h) * 2.0f - 1.0f;
      V2 p = v2(nx, ny);

      // Base capsule along X (ship forward = +X).
      float d = sd_capsule(p, v2(-len, 0.0f), v2(len, 0.0f), wid);

      // Sharpen the nose a little by intersecting with a tapered round-box.
      {
        const float taper = 0.65f + 0.25f * rand01(s);
        const float bx = len * 0.95f;
        const float by = wid * taper;
        const float dn = sd_round_box(p - v2(len * 0.15f, 0.0f), v2(bx, by), nose_sharp);
        d = std::max(d, dn);
      }

      // Cut the tail slightly (engine block).
      {
        const float dx = sd_box(p - v2(-len - tail_cut * 0.35f, 0.0f), v2(tail_cut, wid * 1.2f));
        d = std::max(d, -dx); // subtract tail box.
      }

      // Weapons => wings (mid-ship) for more readable silhouette.
      if (has_weapons) {
        const float wx = 0.22f + 0.10f * rand01(s);
        const float wy = 0.55f + 0.25f * rand01(s);
        const float dw = sd_round_box(p - v2(0.05f * rand01(s), 0.0f), v2(wx, wid * wy), 0.05f);
        d = std::min(d, dw);
      }

      // Cargo => side pods.
      if (has_cargo) {
        const float pod_x = -0.10f + 0.15f * rand01(s);
        const float pod_y = wid * (1.05f + 0.55f * rand01(s));
        const float pod_r = wid * (0.35f + 0.25f * rand01(s));
        float dp = sd_circle(p - v2(pod_x, pod_y), pod_r);
        dp = std::min(dp, sd_circle(p - v2(pod_x, -pod_y), pod_r));
        d = std::min(d, dp);
      }

      // Mining => front "claws".
      if (has_mining) {
        const float cx = len * 0.65f;
        const float cy = wid * (0.75f + 0.25f * rand01(s));
        const float cr = wid * 0.22f;
        float dc = sd_capsule(p, v2(cx, cy), v2(len * 0.98f, cy * 1.25f), cr);
        dc = std::min(dc, sd_capsule(p, v2(cx, -cy), v2(len * 0.98f, -cy * 1.25f), cr));
        d = std::min(d, dc);
      }

      // Sensors => a small dorsal dish.
      if (has_sensors || is_survey > 0.5f) {
        const float dish_x = 0.05f + 0.15f * rand01(s);
        const float dish_y = 0.0f;
        const float dish_r = wid * (0.22f + 0.18f * rand01(s));
        const float dd = sd_circle(p - v2(dish_x, dish_y), dish_r);
        d = std::min(d, dd);
      }

      // Anti-alias edge.
      const float alpha = smoothstep(aa, 0.0f, d);
      if (alpha <= 0.0f) {
        continue;
      }

      // Interior distance for shading.
      const float inside = clamp01(-d / (wid * 1.75f));

      // Directional highlight towards the nose.
      const float front = clamp01((p.x / std::max(0.2f, len)) * 0.5f + 0.5f);

      // Panel noise.
      const float pn = fbm2((p.x + 2.3f) * 6.0f, (p.y + 2.1f) * 6.0f, nseed, 3, 2.2f, 0.52f);

      float shade = 0.42f + 0.58f * inside;
      shade += 0.12f * (front - 0.5f);
      shade *= 0.88f + 0.18f * pn;

      // Outline band.
      const float outline = 1.0f - smoothstep(0.0f, aa * 2.2f, std::fabs(d));
      shade *= 1.0f - 0.35f * outline;

      // Add a canopy/glass highlight for some roles.
      if (is_warship > 0.5f || is_survey > 0.5f) {
        const float cd = sd_circle(p - v2(len * 0.20f, 0.0f), wid * 0.25f);
        const float ca = smoothstep(aa * 2.0f, 0.0f, cd);
        shade = lerp(shade, 0.95f, ca * 0.35f);
      }

      shade = clamp01(shade);

      const std::size_t idx = static_cast<std::size_t>((y * w + x) * 4);
      rgba[idx + 0] = f2b(shade);
      rgba[idx + 1] = f2b(shade);
      rgba[idx + 2] = f2b(shade);
      rgba[idx + 3] = f2b(alpha);
    }
  }
}

void ProcIconSpriteEngine::raster_missile(std::vector<std::uint8_t>& rgba,
                                         int w,
                                         int h,
                                         std::uint32_t seed,
                                         std::uint16_t /*variant*/) {
  rgba.assign(static_cast<std::size_t>(w * h * 4), 0);
  std::uint32_t s = hash_u32(seed ^ 0xDEADBEEFu);

  const float inv = 1.0f / static_cast<float>(w);
  const float aa = 2.0f * inv;

  // Missile: small dart (forward = +X).
  const float len = 0.85f;
  const float wid = 0.18f + 0.05f * rand01(s);

  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const float nx = (static_cast<float>(x) + 0.5f) / static_cast<float>(w) * 2.0f - 1.0f;
      const float ny = (static_cast<float>(y) + 0.5f) / static_cast<float>(h) * 2.0f - 1.0f;
      V2 p = v2(nx, ny);

      float d = sd_capsule(p, v2(-len, 0.0f), v2(len, 0.0f), wid);

      // Fins.
      {
        const float fx = -0.10f;
        const float fy = wid * 2.0f;
        const float df = sd_round_box(p - v2(fx, 0.0f), v2(0.16f, fy), 0.04f);
        d = std::min(d, df);
      }

      // Pointier nose.
      {
        const float dn = sd_round_box(p - v2(len * 0.20f, 0.0f), v2(len * 0.85f, wid * 0.75f), 0.02f);
        d = std::max(d, dn);
      }

      const float alpha = smoothstep(aa, 0.0f, d);
      if (alpha <= 0.0f) continue;

      const float inside = clamp01(-d / (wid * 1.8f));
      float shade = 0.55f + 0.45f * inside;
      // Hot tip.
      shade = lerp(shade, 0.95f, clamp01((p.x - 0.25f) * 2.0f) * 0.25f);

      const std::size_t idx = static_cast<std::size_t>((y * w + x) * 4);
      rgba[idx + 0] = f2b(shade);
      rgba[idx + 1] = f2b(shade);
      rgba[idx + 2] = f2b(shade);
      rgba[idx + 3] = f2b(alpha);
    }
  }
}

void ProcIconSpriteEngine::raster_wreck(std::vector<std::uint8_t>& rgba,
                                       int w,
                                       int h,
                                       std::uint32_t seed,
                                       std::uint64_t id_hash,
                                       std::uint16_t /*variant*/,
                                       const nebula4x::Wreck* wreck) {
  rgba.assign(static_cast<std::size_t>(w * h * 4), 0);
  std::uint32_t s = hash_u32(seed ^ static_cast<std::uint32_t>(id_hash) ^ 0xA11CEu);
  const float inv = 1.0f / static_cast<float>(w);
  const float aa = 2.0f * inv;

  // Wreck kind influences the "chunkiness".
  float chunk = 0.85f;
  if (wreck) {
    if (wreck->kind == nebula4x::WreckKind::Cache) {
      chunk = 0.65f;
    } else if (wreck->kind == nebula4x::WreckKind::Ship) {
      chunk = 1.0f;
    }
  }

  const float t = 0.15f + 0.08f * rand01(s);
  const float r = 0.60f + 0.12f * rand01(s);
  const float ang = (0.25f + 0.75f * rand01(s)) * 3.1415926f;
  const float c = std::cos(ang);
  const float si = std::sin(ang);

  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const float nx = (static_cast<float>(x) + 0.5f) / static_cast<float>(w) * 2.0f - 1.0f;
      const float ny = (static_cast<float>(y) + 0.5f) / static_cast<float>(h) * 2.0f - 1.0f;
      V2 p = v2(nx, ny);

      // Rotated X made from two rounded boxes.
      V2 pr = rotate(p, c, si);
      float d1 = sd_round_box(pr, v2(r, t), 0.05f);
      V2 pr2 = rotate(p, c, -si);
      float d2 = sd_round_box(pr2, v2(r, t), 0.05f);
      float d = std::min(d1, d2);

      // Erode with noise to look like debris.
      const float n = fbm2((p.x + 2.0f) * 5.0f, (p.y + 2.0f) * 5.0f, s, 3, 2.1f, 0.55f);
      d += (n - 0.55f) * 0.10f * chunk;

      const float alpha = smoothstep(aa, 0.0f, d);
      if (alpha <= 0.0f) continue;

      float shade = 0.50f + 0.35f * clamp01(-d / 0.35f);
      // darker "burn" spots.
      shade *= 0.88f + 0.12f * n;
      shade = clamp01(shade);

      const std::size_t idx = static_cast<std::size_t>((y * w + x) * 4);
      rgba[idx + 0] = f2b(shade);
      rgba[idx + 1] = f2b(shade);
      rgba[idx + 2] = f2b(shade);
      rgba[idx + 3] = f2b(alpha);
    }
  }
}

void ProcIconSpriteEngine::raster_anomaly(std::vector<std::uint8_t>& rgba,
                                         int w,
                                         int h,
                                         std::uint32_t seed,
                                         std::uint64_t kind_hash,
                                         std::uint16_t /*variant*/,
                                         const nebula4x::Anomaly* anomaly) {
  rgba.assign(static_cast<std::size_t>(w * h * 4), 0);
  std::uint32_t s = hash_u32(seed ^ static_cast<std::uint32_t>(kind_hash) ^ 0xB00B135u);
  const float inv = 1.0f / static_cast<float>(w);
  const float aa = 2.0f * inv;

  const float ring_r = 0.55f + 0.06f * rand01(s);
  const float ring_t = 0.08f + 0.03f * rand01(s);
  const int spikes = 5 + static_cast<int>(rand01(s) * 4.0f);
  const float swirl = 0.5f + 0.8f * rand01(s);

  const std::uint32_t nseed = hash_u32(s ^ 0xCAFEBABEu);

  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const float nx = (static_cast<float>(x) + 0.5f) / static_cast<float>(w) * 2.0f - 1.0f;
      const float ny = (static_cast<float>(y) + 0.5f) / static_cast<float>(h) * 2.0f - 1.0f;
      V2 p = v2(nx, ny);

      const float r = length(p);
      float d = std::fabs(r - ring_r) - ring_t;

      // Add a few spikes / petals.
      if (r > 1e-6f) {
        const float ang = std::atan2(p.y, p.x);
        const float pet = std::sin(ang * static_cast<float>(spikes) + swirl * r * 6.0f);
        d -= 0.05f * pet;
      }

      // Inner dot.
      d = std::min(d, sd_circle(p, 0.10f));

      // Slight noisy halo.
      const float n = fbm2((p.x + 3.0f) * 4.0f, (p.y + 3.0f) * 4.0f, nseed, 3, 2.1f, 0.55f);
      d += (n - 0.55f) * 0.04f;

      const float alpha = smoothstep(aa, 0.0f, d);
      if (alpha <= 0.0f) continue;

      float shade = 0.65f + 0.35f * clamp01(1.0f - r);
      shade *= 0.90f + 0.18f * n;
      shade = clamp01(shade);

      const std::size_t idx = static_cast<std::size_t>((y * w + x) * 4);
      rgba[idx + 0] = f2b(shade);
      rgba[idx + 1] = f2b(shade);
      rgba[idx + 2] = f2b(shade);
      rgba[idx + 3] = f2b(alpha);
    }
  }

  // Optional: encode a tiny hint of kind by cutting a notch.
  if (anomaly && !anomaly->kind.empty()) {
    const float notch_ang = (static_cast<float>(hash_u32(static_cast<std::uint32_t>(kind_hash)) & 0xFFFFu) / 65535.0f) *
                            2.0f * 3.1415926f;
    const float c = std::cos(notch_ang);
    const float si = std::sin(notch_ang);
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        const float nx = (static_cast<float>(x) + 0.5f) / static_cast<float>(w) * 2.0f - 1.0f;
        const float ny = (static_cast<float>(y) + 0.5f) / static_cast<float>(h) * 2.0f - 1.0f;
        const float proj = nx * c + ny * si;
        if (proj > ring_r * 0.86f) {
          const std::size_t idx = static_cast<std::size_t>((y * w + x) * 4);
          rgba[idx + 3] = static_cast<std::uint8_t>(rgba[idx + 3] * 0.45f);
        }
      }
    }
  }
}

} // namespace nebula4x::ui
