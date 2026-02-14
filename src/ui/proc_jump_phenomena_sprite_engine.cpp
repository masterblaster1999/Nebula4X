#include "ui/proc_jump_phenomena_sprite_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "nebula4x/core/procgen_jump_phenomena.h"
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

static inline float clamp01(float v) {
  if (v < 0.0f) return 0.0f;
  if (v > 1.0f) return 1.0f;
  return v;
}

static inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

static inline float smoothstep(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

static inline float smoothstep(float edge0, float edge1, float x) {
  if (edge0 == edge1) return (x < edge0) ? 0.0f : 1.0f;
  float t = (x - edge0) / (edge1 - edge0);
  return smoothstep(t);
}

static inline std::uint32_t hash_u32(std::uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

static inline std::uint32_t hash_2d_i32(int x, int y, std::uint32_t seed) {
  std::uint32_t h = seed;
  h ^= hash_u32(static_cast<std::uint32_t>(x) + 0x9e3779b9u);
  h ^= hash_u32(static_cast<std::uint32_t>(y) + 0x85ebca6bu);
  return hash_u32(h);
}

static inline std::uint64_t hash_combine_u64(std::uint64_t h, std::uint64_t v) {
  // 64-bit mix (boost-ish). Good enough for cache keys.
  return h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2));
}

static inline std::uint64_t float_to_u64_quant(float v, float scale) {
  if (!std::isfinite(v)) return 0;
  const double dv = static_cast<double>(v);
  const long long q = static_cast<long long>(std::llround(dv * static_cast<double>(scale)));
  return static_cast<std::uint64_t>(q);
}

// --- Value noise + fBm (cheap CPU) -------------------------------------------

static float value_noise(float x, float y, std::uint32_t seed) {
  const int ix = static_cast<int>(std::floor(x));
  const int iy = static_cast<int>(std::floor(y));
  const float fx = x - static_cast<float>(ix);
  const float fy = y - static_cast<float>(iy);

  const float u = smoothstep(fx);
  const float v = smoothstep(fy);

  const auto h00 = hash_2d_i32(ix, iy, seed);
  const auto h10 = hash_2d_i32(ix + 1, iy, seed);
  const auto h01 = hash_2d_i32(ix, iy + 1, seed);
  const auto h11 = hash_2d_i32(ix + 1, iy + 1, seed);

  const float n00 = (h00 & 0xffffu) / 65535.0f;
  const float n10 = (h10 & 0xffffu) / 65535.0f;
  const float n01 = (h01 & 0xffffu) / 65535.0f;
  const float n11 = (h11 & 0xffffu) / 65535.0f;

  const float nx0 = lerp(n00, n10, u);
  const float nx1 = lerp(n01, n11, u);
  return lerp(nx0, nx1, v);
}

static float fbm(float x, float y, std::uint32_t seed, int octaves, float lacunarity, float gain) {
  float sum = 0.0f;
  float amp = 0.5f;
  float fx = x;
  float fy = y;
  std::uint32_t s = seed;
  for (int i = 0; i < octaves; ++i) {
    sum += amp * value_noise(fx, fy, s);
    fx *= lacunarity;
    fy *= lacunarity;
    amp *= gain;
    s = hash_u32(s + 0x9e3779b9u + static_cast<std::uint32_t>(i));
  }
  return sum;
}

static float ridged(float n) {
  // Ridged multifractal base: 1 - abs(2n-1)
  const float x = 1.0f - std::fabs(n * 2.0f - 1.0f);
  return clamp01(x);
}

static inline std::uint8_t f2b(float v) {
  const int iv = static_cast<int>(std::lround(clamp01(v) * 255.0f));
  return static_cast<std::uint8_t>(std::clamp(iv, 0, 255));
}

// Simple HSV->RGB helper (all in [0,1]).
static ImVec4 hsv(float h, float s, float v, float a) {
  h = std::fmod(h, 1.0f);
  if (h < 0.0f) h += 1.0f;
  s = std::clamp(s, 0.0f, 1.0f);
  v = std::clamp(v, 0.0f, 1.0f);
  const float c = v * s;
  const float x = c * (1.0f - std::fabs(std::fmod(h * 6.0f, 2.0f) - 1.0f));
  const float m = v - c;
  float r = 0, g = 0, b = 0;
  const float hp = h * 6.0f;
  if (hp < 1.0f) {
    r = c;
    g = x;
  } else if (hp < 2.0f) {
    r = x;
    g = c;
  } else if (hp < 3.0f) {
    g = c;
    b = x;
  } else if (hp < 4.0f) {
    g = x;
    b = c;
  } else if (hp < 5.0f) {
    r = x;
    b = c;
  } else {
    r = c;
    b = x;
  }
  return ImVec4(r + m, g + m, b + m, a);
}

static ImU32 pack_u32(const ImVec4& c) {
  const std::uint8_t r = f2b(c.x);
  const std::uint8_t g = f2b(c.y);
  const std::uint8_t b = f2b(c.z);
  const std::uint8_t a = f2b(c.w);
  return IM_COL32(r, g, b, a);
}

[[maybe_unused]] static ImU32 tint_from_phenomena(float stability01, float turbulence01, float shear01, float alpha) {
  // Map stability to hue: stable => cyan/blue, unstable => magenta/red.
  // turb/shear bias saturation/value.
  stability01 = std::clamp(stability01, 0.0f, 1.0f);
  turbulence01 = std::clamp(turbulence01, 0.0f, 1.0f);
  shear01 = std::clamp(shear01, 0.0f, 1.0f);

  const float hue = lerp(0.85f, 0.52f, stability01); // ~magenta -> ~cyan
  const float sat = std::clamp(0.55f + 0.35f * turbulence01 + 0.15f * shear01, 0.25f, 1.0f);
  const float val = std::clamp(0.75f + 0.25f * (1.0f - stability01) + 0.10f * turbulence01, 0.40f, 1.0f);
  return pack_u32(hsv(hue, sat, val, std::clamp(alpha, 0.0f, 1.0f)));
}

} // namespace

// --- ProcJumpPhenomenaSpriteEngine -------------------------------------------

ProcJumpPhenomenaSpriteEngine::~ProcJumpPhenomenaSpriteEngine() { shutdown(); }

std::size_t ProcJumpPhenomenaSpriteEngine::JumpKeyHash::operator()(const JumpKey& k) const noexcept {
  std::uint64_t h = 0xcbf29ce484222325ull;
  h = hash_combine_u64(h, k.id_hash);
  h = hash_combine_u64(h, static_cast<std::uint64_t>(k.seed));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(k.sprite_px));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(k.variant));
  h = hash_combine_u64(h, k.style_hash);
  return static_cast<std::size_t>(h);
}

void ProcJumpPhenomenaSpriteEngine::set_backend(UIRendererBackend backend, SDL_Renderer* sdl_renderer) {
  if (backend != backend_ || sdl_renderer != sdl_renderer_) {
    shutdown();
  }
  backend_ = backend;
  sdl_renderer_ = sdl_renderer;
}

bool ProcJumpPhenomenaSpriteEngine::ready() const {
  if (backend_ == UIRendererBackend::SDLRenderer2) return sdl_renderer_ != nullptr;
#if NEBULA4X_UI_RENDERER_OPENGL2
  if (backend_ == UIRendererBackend::OpenGL2) return true;
#endif
  return false;
}

void ProcJumpPhenomenaSpriteEngine::begin_frame() {
  ++frame_;
  stats_.cache_sprites = static_cast<int>(cache_.size());
  stats_.generated_this_frame = 0;
  stats_.gen_ms_this_frame = 0.0;
  stats_.upload_ms_this_frame = 0.0;
}

void ProcJumpPhenomenaSpriteEngine::destroy_texture(ImTextureID id) {
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

void ProcJumpPhenomenaSpriteEngine::shutdown() {
  for (auto& [k, e] : cache_) {
    destroy_texture(e.sprite.tex_id);
    e.sprite.tex_id = imgui_null_texture_id();
  }
  cache_.clear();
}

void ProcJumpPhenomenaSpriteEngine::clear() { shutdown(); }

void ProcJumpPhenomenaSpriteEngine::trim_cache(std::size_t max_entries) {
  if (max_entries < 1) max_entries = 1;
  if (cache_.size() <= max_entries) return;

  std::vector<std::pair<std::uint64_t, JumpKey>> order;
  order.reserve(cache_.size());
  for (const auto& [k, e] : cache_) {
    order.emplace_back(e.last_used_frame, k);
  }
  std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  const std::size_t to_evict = cache_.size() - max_entries;
  for (std::size_t i = 0; i < to_evict; ++i) {
    const JumpKey& k = order[i].second;
    auto it = cache_.find(k);
    if (it != cache_.end()) {
      destroy_texture(it->second.sprite.tex_id);
      cache_.erase(it);
    }
  }
}

ImTextureID ProcJumpPhenomenaSpriteEngine::upload_rgba(const std::uint8_t* rgba, int w, int h) {
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
      nebula4x::log::warn(std::string("ProcJumpPhenomenaSpriteEngine: SDL_CreateRGBSurfaceFrom failed: ") +
                          SDL_GetError());
      return imgui_null_texture_id();
    }

    SDL_Texture* tex = SDL_CreateTextureFromSurface(sdl_renderer_, surf);
    SDL_FreeSurface(surf);

    if (!tex) {
      nebula4x::log::warn(std::string("ProcJumpPhenomenaSpriteEngine: SDL_CreateTextureFromSurface failed: ") +
                          SDL_GetError());
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

std::uint64_t ProcJumpPhenomenaSpriteEngine::style_hash_from_cfg(const ProcJumpPhenomenaSpriteConfig& cfg) {
  std::uint64_t h = 0;
  h = hash_combine_u64(h, static_cast<std::uint64_t>(cfg.filaments ? 1 : 0));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(cfg.filaments_max));
  h = hash_combine_u64(h, float_to_u64_quant(cfg.filament_strength, 1000.0f));
  // Don't include on-screen scale/opacity/animation: those are draw-time.
  return h;
}

ProcJumpPhenomenaSpriteEngine::SpriteInfo ProcJumpPhenomenaSpriteEngine::get_jump_sprite(
    const nebula4x::JumpPoint& jp, std::uint32_t seed, const ProcJumpPhenomenaSpriteConfig& cfg) {
  // Hash jump properties so IDs reused across saves don't collide.
  std::uint64_t idh = 0;
  idh = hash_combine_u64(idh, static_cast<std::uint64_t>(jp.id));
  idh = hash_combine_u64(idh, static_cast<std::uint64_t>(jp.system_id));
  idh = hash_combine_u64(idh, static_cast<std::uint64_t>(jp.linked_jump_id));
  // Quantize position so tiny floating drift doesn't explode cache.
  idh = hash_combine_u64(idh, float_to_u64_quant(static_cast<float>(jp.position_mkm.x), 10.0f));
  idh = hash_combine_u64(idh, float_to_u64_quant(static_cast<float>(jp.position_mkm.y), 10.0f));

  const std::uint16_t sprite_px = static_cast<std::uint16_t>(std::clamp(cfg.sprite_px, 24, 256));
  const std::uint16_t variant = static_cast<std::uint16_t>(((static_cast<std::uint64_t>(seed) ^ idh) >> 8) & 0xFFu);

  JumpKey key;
  key.id_hash = idh;
  key.seed = seed;
  key.sprite_px = sprite_px;
  key.variant = variant;
  key.style_hash = style_hash_from_cfg(cfg);

  return get_or_create(key, jp, cfg);
}

ProcJumpPhenomenaSpriteEngine::SpriteInfo ProcJumpPhenomenaSpriteEngine::get_or_create(
    const JumpKey& key, const nebula4x::JumpPoint& jp, const ProcJumpPhenomenaSpriteConfig& cfg) {
  auto it = cache_.find(key);
  if (it != cache_.end()) {
    it->second.last_used_frame = frame_;
    return it->second.sprite;
  }

  SpriteInfo out;
  out.w = static_cast<int>(key.sprite_px);
  out.h = static_cast<int>(key.sprite_px);

  std::vector<std::uint8_t> rgba;
  rgba.resize(static_cast<std::size_t>(out.w) * static_cast<std::size_t>(out.h) * 4u);

  const auto t0 = Clock::now();
  raster_jump(rgba, out.w, out.h, key.seed, key.id_hash, key.variant, jp, cfg);
  stats_.gen_ms_this_frame += ms_since(t0);

  const auto t1 = Clock::now();
  out.tex_id = upload_rgba(rgba.data(), out.w, out.h);
  stats_.upload_ms_this_frame += ms_since(t1);

  if (!out.tex_id) {
    // Don't cache failures.
    return out;
  }

  CacheEntry e;
  e.sprite = out;
  e.last_used_frame = frame_;
  cache_.emplace(key, e);
  stats_.generated_this_frame += 1;

  trim_cache(static_cast<std::size_t>(std::clamp(cfg.max_cached_sprites, 8, 4096)));
  return out;
}

void ProcJumpPhenomenaSpriteEngine::raster_jump(std::vector<std::uint8_t>& rgba,
                                               int w,
                                               int h,
                                               std::uint32_t seed,
                                               std::uint64_t id_hash,
                                               std::uint16_t variant,
                                               const nebula4x::JumpPoint& jp,
                                               const ProcJumpPhenomenaSpriteConfig& cfg) {
  // Pull deterministic phenomena for this jump point (simulation-side fields).
  const auto ph = nebula4x::procgen_jump_phenomena::generate(jp);
  const float stability = static_cast<float>(std::clamp(ph.stability01, 0.0, 1.0));
  const float turb = static_cast<float>(std::clamp(ph.turbulence01, 0.0, 1.0));
  const float shear = static_cast<float>(std::clamp(ph.shear01, 0.0, 1.0));

  // Visual knobs derived from phenomena.
  const float user_filament = cfg.filaments ? std::clamp(cfg.filament_strength, 0.0f, 4.0f) : 0.0f;
  const float shear_vis = cfg.filaments ? shear : (shear * 0.25f);
  const float chaos = std::clamp((1.0f - stability) * 0.85f + turb * 0.65f + shear_vis * 0.40f, 0.0f, 1.35f);
  const float fil = std::clamp((shear * 1.15f + turb * 0.35f + (1.0f - stability) * 0.25f) * user_filament, 0.0f, 2.5f);
  const float ring_r = lerp(0.60f, 0.74f, stability);
  const float ring_w = lerp(0.22f, 0.14f, stability);

  // Seed mixing: make style stable per jump.
  std::uint32_t s0 = seed ^ static_cast<std::uint32_t>(id_hash);
  s0 = hash_u32(s0 + static_cast<std::uint32_t>(variant));

  const float inv = 1.0f / static_cast<float>(std::max(1, w));
  const float aspect = (h > 0) ? (static_cast<float>(w) / static_cast<float>(h)) : 1.0f;

  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      // Normalized coords [-1,1].
      const float nx = (static_cast<float>(x) + 0.5f) * inv * 2.0f - 1.0f;
      const float ny = ((static_cast<float>(y) + 0.5f) * inv * 2.0f - 1.0f) / aspect;
      const float r = std::sqrt(nx * nx + ny * ny);

      // Outside the influence radius: transparent.
      if (r > 1.20f) {
        const std::size_t idx = (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                                 static_cast<std::size_t>(x)) *
                                4u;
        rgba[idx + 0] = 0;
        rgba[idx + 1] = 0;
        rgba[idx + 2] = 0;
        rgba[idx + 3] = 0;
        continue;
      }

      const float ang = std::atan2(ny, nx);

      // Domain-warped noise: use low-frequency fbm as a coordinate offset.
      const float wx = fbm(nx * 1.8f, ny * 1.8f, s0 ^ 0x51ed270bu, 3, 2.0f, 0.55f) - 0.5f;
      const float wy = fbm(nx * 1.8f, ny * 1.8f, s0 ^ 0x2f9be6cbu, 3, 2.0f, 0.55f) - 0.5f;

      const float dx = nx + wx * 0.55f * chaos;
      const float dy = ny + wy * 0.55f * chaos;

      const float n1 = fbm(dx * 7.0f, dy * 7.0f, s0 ^ 0x1b873593u, 3, 2.1f, 0.52f);

      // Ring profile: gaussian-ish.
      const float dr = (r - ring_r) / std::max(0.001f, ring_w);
      const float ring = std::exp(-dr * dr);

      // Core glow.
      const float core = std::exp(-(r * r) / 0.11f) * (0.55f + 0.45f * turb);

      // Swirl bands.
      const float swirl_freq = lerp(6.0f, 10.0f, chaos);
      const float swirl = ang * swirl_freq + (1.0f - r) * (5.0f + 10.0f * chaos) + (n1 - 0.5f) * 6.0f;
      const float bands = 0.5f + 0.5f * std::sin(swirl);

      // Filaments: ridged noise modulated in polar coordinates.
      const float rf = r * (2.2f + 2.8f * fil);
      const float af = ang * (3.0f + 5.0f * fil);
      const float px = std::cos(af) * rf + dx * 0.55f;
      const float py = std::sin(af) * rf + dy * 0.55f;
      const float ridge = ridged(fbm(px * 2.2f, py * 2.2f, s0 ^ 0x9e3779b9u, 4, 2.1f, 0.58f));
      const float filament = std::pow(ridge, 3.0f) * (0.25f + 0.75f * fil);

      // Outer falloff + vignetting.
      const float edge = smoothstep(1.20f, 0.95f, r);
      const float vign = 1.0f - 0.12f * (nx * nx + ny * ny);

      float intensity = 0.0f;
      intensity += ring * (0.35f + 0.65f * bands);
      intensity += core;
      intensity += filament * (0.55f + 0.45f * (1.0f - stability));
      intensity *= edge * vign;

      // Add subtle noisy grain (helps avoid banding at low alpha).
      const float g = value_noise(dx * 28.0f, dy * 28.0f, s0 ^ 0x7f4a7c15u);
      intensity += (g - 0.5f) * 0.08f * (0.25f + 0.75f * chaos);

      // Final alpha. Clamp and remap so weak fields still show.
      float a = clamp01(intensity);
      a = std::pow(a, 0.85f);

      const float lum = clamp01(0.20f + 0.85f * intensity);

      const std::size_t idx = (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                               static_cast<std::size_t>(x)) *
                              4u;
      const std::uint8_t b = f2b(lum);
      const std::uint8_t aa = f2b(a);
      rgba[idx + 0] = b;
      rgba[idx + 1] = b;
      rgba[idx + 2] = b;
      rgba[idx + 3] = aa;
    }
  }
}

void ProcJumpPhenomenaSpriteEngine::draw_sprite_rotated(ImDrawList* draw,
                                                        ImTextureID tex,
                                                        const ImVec2& center,
                                                        float size_px,
                                                        float angle_rad,
                                                        ImU32 tint) {
  if (!draw || !tex) return;
  if (size_px <= 1.0f) return;

  const float half = size_px * 0.5f;
  const float c = std::cos(angle_rad);
  const float s = std::sin(angle_rad);

  auto rot = [&](float x, float y) {
    return ImVec2(center.x + x * c - y * s, center.y + x * s + y * c);
  };

  const ImVec2 p0 = rot(-half, -half);
  const ImVec2 p1 = rot(+half, -half);
  const ImVec2 p2 = rot(+half, +half);
  const ImVec2 p3 = rot(-half, +half);
  draw->AddImageQuad(tex, p0, p1, p2, p3, ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1), tint);
}

void ProcJumpPhenomenaSpriteEngine::draw_filaments(ImDrawList* draw,
                                                   const ImVec2& center,
                                                   float radius_px,
                                                   std::uint32_t seed,
                                                   float shear01,
                                                   float turbulence01,
                                                   double time_days,
                                                   ImU32 tint,
                                                   const ProcJumpPhenomenaSpriteConfig& cfg) {
  if (!draw) return;
  if (!cfg.filaments) return;
  if (radius_px <= 4.0f) return;

  shear01 = std::clamp(shear01, 0.0f, 1.0f);
  turbulence01 = std::clamp(turbulence01, 0.0f, 1.0f);

  const int max_fil = std::clamp(cfg.filaments_max, 0, 24);
  if (max_fil <= 0) return;

  // Filaments count grows with shear.
  int count = static_cast<int>(std::lround(lerp(1.0f, static_cast<float>(max_fil), shear01)));
  count = std::clamp(count, 0, max_fil);
  if (count <= 0) return;

  const float t = static_cast<float>(time_days);
  const float anim = 0.25f + 0.75f * turbulence01;

  // Deterministic angles based on seed.
  std::uint32_t s0 = hash_u32(seed ^ 0xB5297A4Du);

  for (int i = 0; i < count; ++i) {
    s0 = hash_u32(s0 + 0x9e3779b9u + static_cast<std::uint32_t>(i));
    const float a0 = ((s0 & 0xffffu) / 65535.0f) * 2.0f * 3.14159265f;
    const float span = lerp(0.35f, 1.25f, turbulence01) * (0.35f + 0.65f * shear01);
    const float a1 = a0 + span;

    const float rr0 = radius_px * lerp(0.48f, 0.66f, ((s0 >> 16) & 0xffffu) / 65535.0f);
    const float rr1 = radius_px * lerp(0.74f, 1.04f, ((hash_u32(s0) >> 16) & 0xffffu) / 65535.0f);

    const int seg = std::clamp(8 + static_cast<int>(std::lround(span * 18.0f)), 8, 48);
    ImVector<ImVec2> pts;
    pts.resize(seg + 1);

    for (int k = 0; k <= seg; ++k) {
      const float u = static_cast<float>(k) / static_cast<float>(seg);
      const float a = lerp(a0, a1, u);
      const float rr = lerp(rr0, rr1, u);

      // Wobble radius/angle using cheap value noise.
      const float nx = std::cos(a) * 1.1f + t * 0.35f * anim;
      const float ny = std::sin(a) * 1.1f - t * 0.22f * anim;
      const float n = value_noise(nx * 3.5f, ny * 3.5f, s0 ^ 0x68bc21ebu);
      const float wob = (n - 0.5f) * (0.22f + 0.55f * shear01) * cfg.filament_strength;

      const float aa = a + wob * 0.35f;
      const float r2 = rr * (1.0f + wob * 0.25f);
      pts[k] = ImVec2(center.x + std::cos(aa) * r2, center.y + std::sin(aa) * r2);
    }

    const float thick = std::clamp(0.75f + 1.15f * shear01, 0.6f, 2.8f);
    draw->AddPolyline(pts.Data, pts.Size, tint, false, thick);
  }
}

} // namespace nebula4x::ui
