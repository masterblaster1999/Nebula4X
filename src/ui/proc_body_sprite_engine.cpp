#include "ui/proc_body_sprite_engine.h"

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
  // Similar to boost::hash_combine.
  return h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2));
}

static inline std::uint64_t float_to_u64_quant(float v, float scale) {
  const double dv = static_cast<double>(v);
  const double s = static_cast<double>(scale);
  const long long q = static_cast<long long>(std::llround(dv * s));
  return static_cast<std::uint64_t>(static_cast<std::uint64_t>(q) ^ 0xC0FFEEu);
}

static inline float rand01(std::uint32_t& state) {
  state = hash_u32(state);
  // 24 bits of mantissa.
  return static_cast<float>(state & 0x00FFFFFFu) * (1.0f / 16777215.0f);
}

// --- Noise ---------------------------------------------------------------------

static inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

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

static inline std::uint32_t hash_3d_i32(int x, int y, int z, std::uint32_t seed) {
  std::uint32_t h = seed;
  h ^= hash_u32(static_cast<std::uint32_t>(x) * 0x9E3779B9u);
  h ^= hash_u32(static_cast<std::uint32_t>(y) * 0x85EBCA6Bu);
  h ^= hash_u32(static_cast<std::uint32_t>(z) * 0xC2B2AE35u);
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

static float value_noise3(float x, float y, float z, std::uint32_t seed) {
  const int xi = static_cast<int>(std::floor(x));
  const int yi = static_cast<int>(std::floor(y));
  const int zi = static_cast<int>(std::floor(z));
  const float xf = x - static_cast<float>(xi);
  const float yf = y - static_cast<float>(yi);
  const float zf = z - static_cast<float>(zi);

  const float u = fade(xf);
  const float v = fade(yf);
  const float w = fade(zf);

  const float n000 = hash_to_01(hash_3d_i32(xi, yi, zi, seed));
  const float n100 = hash_to_01(hash_3d_i32(xi + 1, yi, zi, seed));
  const float n010 = hash_to_01(hash_3d_i32(xi, yi + 1, zi, seed));
  const float n110 = hash_to_01(hash_3d_i32(xi + 1, yi + 1, zi, seed));

  const float n001 = hash_to_01(hash_3d_i32(xi, yi, zi + 1, seed));
  const float n101 = hash_to_01(hash_3d_i32(xi + 1, yi, zi + 1, seed));
  const float n011 = hash_to_01(hash_3d_i32(xi, yi + 1, zi + 1, seed));
  const float n111 = hash_to_01(hash_3d_i32(xi + 1, yi + 1, zi + 1, seed));

  const float x00 = lerp(n000, n100, u);
  const float x10 = lerp(n010, n110, u);
  const float x01 = lerp(n001, n101, u);
  const float x11 = lerp(n011, n111, u);

  const float y0 = lerp(x00, x10, v);
  const float y1 = lerp(x01, x11, v);

  return lerp(y0, y1, w);
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
  return std::clamp(sum, 0.0f, 1.0f);
}

static float fbm3(float x, float y, float z, std::uint32_t seed, int octaves, float lacunarity, float gain) {
  float sum = 0.0f;
  float amp = 0.5f;
  float fx = x;
  float fy = y;
  float fz = z;
  for (int i = 0; i < octaves; ++i) {
    sum += amp * value_noise3(fx, fy, fz, seed + static_cast<std::uint32_t>(i) * 1013u);
    fx *= lacunarity;
    fy *= lacunarity;
    fz *= lacunarity;
    amp *= gain;
  }
  return std::clamp(sum, 0.0f, 1.0f);
}

// Simple Worley F1 (cellular) noise with 1 feature point per integer cell.
static float worley_f1(float x, float y, std::uint32_t seed) {
  const int xi = static_cast<int>(std::floor(x));
  const int yi = static_cast<int>(std::floor(y));

  float best = std::numeric_limits<float>::max();
  for (int oy = -1; oy <= 1; ++oy) {
    for (int ox = -1; ox <= 1; ++ox) {
      const int cx = xi + ox;
      const int cy = yi + oy;
      std::uint32_t h = hash_2d_i32(cx, cy, seed);
      const float fx = static_cast<float>(cx) + rand01(h);
      const float fy = static_cast<float>(cy) + rand01(h);
      const float dx = x - fx;
      const float dy = y - fy;
      const float d2 = dx * dx + dy * dy;
      if (d2 < best) best = d2;
    }
  }
  return std::sqrt(best);
}

// --- Color helpers --------------------------------------------------------------

struct ColorF {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;
};

static inline ColorF lerp(const ColorF& a, const ColorF& b, float t) {
  return ColorF{
      lerp(a.r, b.r, t),
      lerp(a.g, b.g, t),
      lerp(a.b, b.b, t),
      lerp(a.a, b.a, t),
  };
}

static inline ColorF mul(const ColorF& c, float s) {
  return ColorF{c.r * s, c.g * s, c.b * s, c.a};
}

static inline ColorF add(const ColorF& a, const ColorF& b) {
  return ColorF{a.r + b.r, a.g + b.g, a.b + b.b, a.a};
}

static inline ColorF clamp01(const ColorF& c) {
  return ColorF{
      std::clamp(c.r, 0.0f, 1.0f),
      std::clamp(c.g, 0.0f, 1.0f),
      std::clamp(c.b, 0.0f, 1.0f),
      std::clamp(c.a, 0.0f, 1.0f),
  };
}

static inline std::uint8_t f2u8(float v) {
  const float x = std::clamp(v, 0.0f, 1.0f);
  return static_cast<std::uint8_t>(std::lround(x * 255.0f));
}

static ColorF star_color_from_temp(float temp_k, std::uint32_t seed) {
  if (temp_k <= 0.0f) {
    // Deterministic random color (biased to warm whites).
    std::uint32_t s = hash_u32(seed ^ 0xC0DECAFEu);
    const float t = 0.25f + 0.65f * rand01(s);
    const float u = rand01(s);
    // Warm -> cool.
    const ColorF warm{1.0f, 0.72f, 0.48f, 1.0f};
    const ColorF white{1.0f, 0.97f, 0.92f, 1.0f};
    const ColorF cool{0.62f, 0.78f, 1.0f, 1.0f};
    if (u < 0.55f) return lerp(warm, white, t);
    return lerp(white, cool, t);
  }

  // Very rough Kelvin-to-RGB approximation via piecewise gradient.
  // 3000K -> warm, 5800K -> near-white, 12000K -> blue.
  const float k = std::clamp(temp_k, 2500.0f, 14000.0f);
  const float t = (k - 2500.0f) / (14000.0f - 2500.0f);
  const ColorF warm{1.0f, 0.63f, 0.42f, 1.0f};
  const ColorF white{1.0f, 0.98f, 0.95f, 1.0f};
  const ColorF blue{0.62f, 0.80f, 1.0f, 1.0f};

  if (t < 0.55f) {
    return lerp(warm, white, t / 0.55f);
  }
  return lerp(white, blue, (t - 0.55f) / 0.45f);
}

static inline float smoothstep(float e0, float e1, float x) {
  const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

static inline float length2(float x, float y) { return std::sqrt(x * x + y * y); }

static inline void normalize2(float& x, float& y) {
  const float len = std::sqrt(x * x + y * y);
  if (len < 1e-6f) {
    x = 1.0f;
    y = 0.0f;
    return;
  }
  x /= len;
  y /= len;
}

static inline void normalize3(float& x, float& y, float& z) {
  const float len = std::sqrt(x * x + y * y + z * z);
  if (len < 1e-6f) {
    x = 0.0f;
    y = 0.0f;
    z = 1.0f;
    return;
  }
  x /= len;
  y /= len;
  z /= len;
}

static inline float dot3(float ax, float ay, float az, float bx, float by, float bz) {
  return ax * bx + ay * by + az * bz;
}

static inline float remap01(float x, float a, float b) {
  if (std::abs(b - a) < 1e-6f) return 0.0f;
  return std::clamp((x - a) / (b - a), 0.0f, 1.0f);
}

}  // namespace

// --- ProcBodySpriteEngine -------------------------------------------------------

ProcBodySpriteEngine::~ProcBodySpriteEngine() { shutdown(); }

void ProcBodySpriteEngine::set_backend(UIRendererBackend backend, SDL_Renderer* sdl_renderer) {
  if (backend_ != backend || sdl_renderer_ != sdl_renderer) {
    shutdown();
    backend_ = backend;
    sdl_renderer_ = sdl_renderer;
  }
}

bool ProcBodySpriteEngine::ready() const {
  if (backend_ == UIRendererBackend::SDLRenderer2) return sdl_renderer_ != nullptr;
#if NEBULA4X_UI_RENDERER_OPENGL2
  if (backend_ == UIRendererBackend::OpenGL2) return true;
#endif
  return false;
}

void ProcBodySpriteEngine::shutdown() {
  for (auto& kv : cache_) {
    destroy_sprite(kv.second);
  }
  cache_.clear();
  stats_ = {};
}

void ProcBodySpriteEngine::begin_frame() {
  ++frame_index_;
  stats_.generated_this_frame = 0;
  stats_.gen_ms_this_frame = 0.0;
  stats_.upload_ms_this_frame = 0.0;
  stats_.cache_sprites = static_cast<int>(cache_.size());
}

void ProcBodySpriteEngine::clear() { shutdown(); }

std::size_t ProcBodySpriteEngine::SpriteKeyHash::operator()(const SpriteKey& k) const noexcept {
  std::uint64_t h = 0;
  h = hash_combine_u64(h, k.body_id);
  h = hash_combine_u64(h, static_cast<std::uint64_t>(k.seed));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(k.size_px));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(k.light_step));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(k.variant));
  h = hash_combine_u64(h, k.style_hash);
  return static_cast<std::size_t>(h);
}

std::uint64_t ProcBodySpriteEngine::compute_style_hash(const ProcBodySpriteConfig& cfg) {
  std::uint64_t h = 0;
  h = hash_combine_u64(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(cfg.sprite_px)));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(cfg.max_cached_sprites)));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(std::clamp(cfg.light_steps, 4, 128))));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(cfg.enable_rings ? 1 : 0));
  h = hash_combine_u64(h, float_to_u64_quant(cfg.ring_probability, 1000.0f));
  h = hash_combine_u64(h, float_to_u64_quant(cfg.ambient, 1000.0f));
  h = hash_combine_u64(h, float_to_u64_quant(cfg.diffuse_strength, 1000.0f));
  h = hash_combine_u64(h, float_to_u64_quant(cfg.specular_strength, 1000.0f));
  h = hash_combine_u64(h, float_to_u64_quant(cfg.specular_power, 100.0f));
  return h;
}

std::uint32_t ProcBodySpriteEngine::quantize_light_step(const Vec2& light_dir, int steps) {
  if (steps < 4) steps = 4;
  if (steps > 128) steps = 128;

  float lx = static_cast<float>(light_dir.x);
  float ly = static_cast<float>(light_dir.y);
  if (std::abs(lx) < 1e-6f && std::abs(ly) < 1e-6f) {
    lx = 1.0f;
    ly = -0.25f;
  }
  normalize2(lx, ly);
  float a = std::atan2(ly, lx);
  if (a < 0.0f) a += 2.0f * 3.14159265358979323846f;
  const float t = a / (2.0f * 3.14159265358979323846f);
  int s = static_cast<int>(std::lround(t * static_cast<float>(steps))) % steps;
  if (s < 0) s += steps;
  return static_cast<std::uint32_t>(s);
}

void ProcBodySpriteEngine::trim_cache(int max_sprites) {
  if (max_sprites < 16) max_sprites = 16;
  if (static_cast<int>(cache_.size()) <= max_sprites) return;

  while (static_cast<int>(cache_.size()) > max_sprites) {
    auto oldest = cache_.begin();
    for (auto it = cache_.begin(); it != cache_.end(); ++it) {
      if (it->second.last_used_frame < oldest->second.last_used_frame) oldest = it;
    }
    destroy_sprite(oldest->second);
    cache_.erase(oldest);
  }

  stats_.cache_sprites = static_cast<int>(cache_.size());
}

void ProcBodySpriteEngine::destroy_sprite(SpriteEntry& entry) {
  if (!imgui_texture_id_is_valid(entry.tex_id)) return;

  if (backend_ == UIRendererBackend::SDLRenderer2) {
    SDL_Texture* tex = sdl_texture_from_imgui_texture_id(entry.tex_id);
    SDL_DestroyTexture(tex);
    entry.tex_id = imgui_null_texture_id();
    return;
  }

#if NEBULA4X_UI_RENDERER_OPENGL2
  if (backend_ == UIRendererBackend::OpenGL2) {
    const GLuint tex = gl_texture_from_imgui_texture_id<GLuint>(entry.tex_id);
    if (tex != 0) {
      glDeleteTextures(1, &tex);
    }
    entry.tex_id = imgui_null_texture_id();
  }
#else
  (void)entry;
#endif
}

ImTextureID ProcBodySpriteEngine::upload_rgba(const std::uint8_t* rgba, int w, int h) {
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
      nebula4x::log::warn(std::string("ProcBodySpriteEngine: SDL_CreateRGBSurfaceFrom failed: ") + SDL_GetError());
      return imgui_null_texture_id();
    }

    SDL_Texture* tex = SDL_CreateTextureFromSurface(sdl_renderer_, surf);
    SDL_FreeSurface(surf);

    if (!tex) {
      nebula4x::log::warn(std::string("ProcBodySpriteEngine: SDL_CreateTextureFromSurface failed: ") + SDL_GetError());
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

float ProcBodySpriteEngine::compute_sphere_radius_norm(const nebula4x::Body& body,
                                                      std::uint32_t seed,
                                                      const ProcBodySpriteConfig& cfg,
                                                      std::uint16_t* out_variant) {
  std::uint32_t s = hash_u32(seed ^ static_cast<std::uint32_t>(body.id) ^ 0x6D2B79F5u);

  // 0..255 style index.
  std::uint16_t style = 0;

  // Derive a coarse style from body type + temperature.
  if (body.type == BodyType::Planet) {
    const float t = static_cast<float>(body.surface_temp_k);
    if (t > 0.0f) {
      if (t < 220.0f)
        style = 0;  // icy
      else if (t < 330.0f)
        style = 1;  // temperate
      else if (t < 800.0f)
        style = 2;  // desert
      else
        style = 3;  // lava
    } else {
      style = static_cast<std::uint16_t>(rand01(s) * 4.0f) & 3u;
    }
  } else if (body.type == BodyType::GasGiant) {
    style = static_cast<std::uint16_t>(rand01(s) * 4.0f) & 3u;
  } else if (body.type == BodyType::Star) {
    style = static_cast<std::uint16_t>(rand01(s) * 6.0f) & 7u;
  } else if (body.type == BodyType::Moon) {
    style = static_cast<std::uint16_t>(rand01(s) * 3.0f) & 3u;
  } else if (body.type == BodyType::Asteroid) {
    style = static_cast<std::uint16_t>(rand01(s) * 3.0f) & 3u;
  } else {
    style = static_cast<std::uint16_t>(rand01(s) * 4.0f) & 7u;
  }

  bool ringed = false;
  if (cfg.enable_rings && body.type == BodyType::GasGiant) {
    const float p = std::clamp(cfg.ring_probability, 0.0f, 1.0f);
    const float r = rand01(s);
    ringed = (r < p);
  }

  std::uint16_t variant = style;
  if (ringed) variant |= 0x0100u;

  if (out_variant) *out_variant = variant;

  if (body.type == BodyType::Star) {
    // Leave room for corona.
    return 0.82f;
  }
  if (body.type == BodyType::Asteroid) {
    // Irregular silhouettes want the full texture extents.
    return 1.0f;
  }
  if (ringed) {
    // Make the planet smaller inside the texture so rings can fit.
    return 0.70f;
  }

  // Default: allow a small atmosphere/glow margin.
  return 0.92f;
}

ProcBodySpriteEngine::SpriteInfo ProcBodySpriteEngine::get_body_sprite(const nebula4x::Body& body,
                                                                      std::uint32_t seed,
                                                                      const Vec2& light_dir,
                                                                      const ProcBodySpriteConfig& cfg) {
  SpriteInfo out;
  if (!ready()) return out;

  const int px = std::clamp(cfg.sprite_px, 24, 512);

  std::uint16_t variant = 0;
  const float sphere_r = compute_sphere_radius_norm(body, seed, cfg, &variant);

  const std::uint64_t style_hash = compute_style_hash(cfg);
  const int steps = std::clamp(cfg.light_steps, 4, 128);
  const std::uint32_t step = quantize_light_step(light_dir, steps);

  SpriteKey key;
  key.body_id = static_cast<std::uint64_t>(body.id);
  key.seed = seed;
  key.size_px = static_cast<std::uint32_t>(px);
  key.light_step = static_cast<std::uint16_t>(step);
  key.variant = variant;
  key.style_hash = style_hash;

  auto it = cache_.find(key);
  if (it != cache_.end()) {
    it->second.last_used_frame = frame_index_;
    out.tex_id = it->second.tex_id;
    out.w = it->second.w;
    out.h = it->second.h;
    out.sphere_radius_norm = it->second.sphere_radius_norm;
    return out;
  }

  // Generate + upload.
  const int w = px;
  const int h = px;

  const float angle = (static_cast<float>(step) / static_cast<float>(steps)) * (2.0f * 3.14159265358979323846f);

  std::vector<std::uint8_t> rgba;
  rgba.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);

  const auto t0 = Clock::now();
  generate_sprite_rgba(rgba, w, h, sphere_r, body, seed, angle, cfg, variant);
  stats_.gen_ms_this_frame += ms_since(t0);

  const auto t1 = Clock::now();
  ImTextureID tex_id = upload_rgba(rgba.data(), w, h);
  stats_.upload_ms_this_frame += ms_since(t1);

  if (!tex_id) return out;

  SpriteEntry e;
  e.tex_id = tex_id;
  e.w = w;
  e.h = h;
  e.sphere_radius_norm = sphere_r;
  e.last_used_frame = frame_index_;

  cache_.emplace(key, e);
  stats_.generated_this_frame += 1;
  stats_.cache_sprites = static_cast<int>(cache_.size());

  trim_cache(cfg.max_cached_sprites);

  out.tex_id = tex_id;
  out.w = w;
  out.h = h;
  out.sphere_radius_norm = sphere_r;
  return out;
}

void ProcBodySpriteEngine::generate_sprite_rgba(std::vector<std::uint8_t>& out,
                                               int w,
                                               int h,
                                               float sphere_radius_norm,
                                               const nebula4x::Body& body,
                                               std::uint32_t seed,
                                               float light_angle_rad,
                                               const ProcBodySpriteConfig& cfg,
                                               std::uint16_t variant) {
  if (w <= 0 || h <= 0) return;

  const float aa = 2.0f / static_cast<float>(std::min(w, h));

  // Light direction in sprite space.
  float lx = std::cos(light_angle_rad);
  float ly = std::sin(light_angle_rad);
  float lz = 0.75f;
  normalize3(lx, ly, lz);

  // Specular half-vector (view is +Z).
  float hx = lx;
  float hy = ly;
  float hz = lz + 1.0f;
  normalize3(hx, hy, hz);

  const std::uint16_t style = static_cast<std::uint16_t>(variant & 0x00FFu);
  const bool ringed = (variant & 0x0100u) != 0;

  // Palette.
  const ColorF ocean_deep{0.04f, 0.14f, 0.38f, 1.0f};
  const ColorF ocean_shallow{0.08f, 0.32f, 0.58f, 1.0f};
  const ColorF land_green{0.12f, 0.62f, 0.24f, 1.0f};
  const ColorF land_brown{0.40f, 0.34f, 0.22f, 1.0f};
  const ColorF land_sand{0.78f, 0.68f, 0.42f, 1.0f};
  const ColorF land_red{0.62f, 0.33f, 0.23f, 1.0f};
  const ColorF ice{0.88f, 0.94f, 1.00f, 1.0f};
  const ColorF ice_dark{0.55f, 0.74f, 0.92f, 1.0f};
  const ColorF lava{1.0f, 0.36f, 0.10f, 1.0f};
  const ColorF basalt{0.20f, 0.18f, 0.17f, 1.0f};

  const std::array<std::pair<ColorF, ColorF>, 4> gas_pal = {
      std::pair<ColorF, ColorF>{ColorF{0.30f, 0.52f, 1.00f, 1.0f}, ColorF{0.16f, 0.28f, 0.70f, 1.0f}},
      std::pair<ColorF, ColorF>{ColorF{1.00f, 0.64f, 0.26f, 1.0f}, ColorF{0.82f, 0.36f, 0.12f, 1.0f}},
      std::pair<ColorF, ColorF>{ColorF{0.36f, 0.92f, 0.60f, 1.0f}, ColorF{0.12f, 0.58f, 0.32f, 1.0f}},
      std::pair<ColorF, ColorF>{ColorF{0.78f, 0.48f, 0.98f, 1.0f}, ColorF{0.35f, 0.22f, 0.62f, 1.0f}},
  };

  // Ring parameters (deterministic).
  float ring_tilt = 0.0f;
  float ring_inner = 0.78f;
  float ring_outer = 1.0f;
  ColorF ring_col{0.86f, 0.82f, 0.68f, 1.0f};
  if (ringed) {
    std::uint32_t rs = hash_u32(seed ^ static_cast<std::uint32_t>(body.id) ^ 0xA53A9A1Du);
    ring_tilt = (rand01(rs) * 2.0f - 1.0f) * 0.55f;  // -0.55..0.55 rad
    ring_inner = 0.72f + 0.10f * rand01(rs);
    ring_outer = 0.98f;

    // Ring tint: from pale to dusty.
    const float t = rand01(rs);
    ring_col = lerp(ColorF{0.90f, 0.88f, 0.82f, 1.0f}, ColorF{0.70f, 0.62f, 0.48f, 1.0f}, t);
  }

  auto shade = [&](float nx, float ny, float nz, const ColorF& base) {
    const float ndotl = std::max(0.0f, dot3(nx, ny, nz, lx, ly, lz));
    const float ndoth = std::max(0.0f, dot3(nx, ny, nz, hx, hy, hz));

    const float amb = std::clamp(cfg.ambient, 0.0f, 1.0f);
    const float diff = std::clamp(cfg.diffuse_strength, 0.0f, 2.0f) * ndotl;
    const float spec_pow = std::clamp(cfg.specular_power, 1.0f, 128.0f);
    const float spec = std::clamp(cfg.specular_strength, 0.0f, 2.0f) * std::pow(ndoth, spec_pow);

    ColorF c = mul(base, amb + diff);
    c = add(c, mul(ColorF{1.0f, 1.0f, 1.0f, 1.0f}, spec));
    return clamp01(c);
  };

  // Write pixels.
  out.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, std::uint8_t{0});

  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const float fx = (static_cast<float>(x) + 0.5f) / static_cast<float>(w);
      const float fy = (static_cast<float>(y) + 0.5f) / static_cast<float>(h);

      // Texture-space coords in [-1,1].
      const float tx = fx * 2.0f - 1.0f;
      const float ty = fy * 2.0f - 1.0f;

      // Sphere coords (scaled by sphere_radius_norm).
      const float sx = tx / std::max(1e-4f, sphere_radius_norm);
      const float sy = ty / std::max(1e-4f, sphere_radius_norm);
      const float r = length2(sx, sy);

      ColorF col{0, 0, 0, 0};
      float alpha = 0.0f;

      // Optional ring outside the sphere.
      if (ringed) {
        // Rotate ring plane.
        const float cs = std::cos(ring_tilt);
        const float sn = std::sin(ring_tilt);
        const float rx = tx * cs - ty * sn;
        const float ry = tx * sn + ty * cs;

        const float rr = std::sqrt(rx * rx + (ry * 1.65f) * (ry * 1.65f));
        const float ring_alpha =
            (1.0f - smoothstep(ring_outer - aa * 3.0f, ring_outer + aa * 3.0f, rr)) *
            smoothstep(ring_inner - aa * 3.0f, ring_inner + aa * 3.0f, rr);

        if (ring_alpha > 0.001f) {
          // Subtle ring striations.
          const float str = fbm2(rx * 18.0f + 7.1f, ry * 18.0f - 3.3f, seed ^ 0x55AA77CCu, 3, 2.0f, 0.5f);
          const float t = 0.40f + 0.60f * str;
          col = mul(ring_col, t);
          alpha = std::max(alpha, ring_alpha * 0.82f);
        }
      }

      if (r <= 1.0f + aa) {
        // Edge AA.
        const float edge = 1.0f - smoothstep(1.0f - aa, 1.0f + aa, r);

        if (r <= 1.0f) {
          const float z = std::sqrt(std::max(0.0f, 1.0f - r * r));
          float nx = sx;
          float ny = sy;
          float nz = z;
          normalize3(nx, ny, nz);

          // Base albedo.
          ColorF base{0.5f, 0.5f, 0.5f, 1.0f};

          // Seamless-ish patterns: sample noise in 3D using the surface normal.
          const float n1 = fbm3(nx * 2.2f + 5.0f, ny * 2.2f - 2.0f, nz * 2.2f + 1.0f, seed ^ 0xA341316Cu, 5, 2.0f,
                                0.5f);
          const float n2 = fbm3(nx * 5.2f - 1.7f, ny * 5.2f + 3.3f, nz * 5.2f + 0.9f, seed ^ 0xC8013EA4u, 4, 2.0f,
                                0.5f);

          if (body.type == BodyType::Planet) {
            // Height field.
            const float height = 0.65f * n1 + 0.35f * n2;
            const float ridged = 1.0f - std::abs(2.0f * n2 - 1.0f);

            const float lat = std::abs(ny);
            const float ice_cap = smoothstep(0.65f, 0.92f, lat);

            if (style == 0) {
              // Icy world.
              const float t = 0.55f * height + 0.45f * ridged;
              base = lerp(ice_dark, ice, t);
              // Brighten poles.
              base = lerp(base, ice, ice_cap * 0.85f);
            } else if (style == 3) {
              // Lava world.
              const float cracks = fbm3(nx * 9.0f, ny * 9.0f, nz * 9.0f, seed ^ 0xF00DFACEu, 3, 2.2f, 0.5f);
              const float flow = smoothstep(0.72f, 0.90f, height + cracks * 0.25f);
              base = lerp(basalt, lava, flow);
              // Darken highlands.
              base = lerp(base, basalt, smoothstep(0.60f, 1.0f, ridged));
            } else {
              // Temperate / desert.
              float sea_level = (style == 2) ? 0.62f : 0.52f;
              sea_level += (hash_to_01(hash_u32(seed ^ static_cast<std::uint32_t>(body.id))) - 0.5f) * 0.05f;

              if (height < sea_level) {
                const float t = remap01(height, 0.0f, sea_level);
                base = lerp(ocean_deep, ocean_shallow, t);
              } else {
                const float t = remap01(height, sea_level, 1.0f);
                if (style == 2) {
                  // Desert.
                  base = lerp(land_sand, land_red, t * 0.65f);
                } else {
                  // Temperate.
                  base = lerp(land_green, land_brown, smoothstep(0.35f, 0.85f, t));
                }

                // Mountains.
                base = lerp(base, land_brown, smoothstep(0.70f, 0.96f, ridged));
              }

              // Polar ice caps.
              base = lerp(base, ice, ice_cap * 0.55f);

              // Subtle atmosphere rim (blue) if we left margin.
              const float rim = smoothstep(0.92f, 1.0f, r);
              if (rim > 0.0f) {
                base = lerp(base, ColorF{0.34f, 0.55f, 1.0f, 1.0f}, rim * 0.25f);
              }
            }

            base = shade(nx, ny, nz, base);
          } else if (body.type == BodyType::GasGiant) {
            const auto& pal = gas_pal[style & 3u];
            const float band_noise = fbm3(nx * 3.0f, ny * 3.0f, nz * 3.0f, seed ^ 0xB16B00B5u, 4, 2.0f, 0.55f);
            const float bands = ny * 6.5f + (band_noise - 0.5f) * 1.8f;
            const float t = 0.5f + 0.5f * std::sin(bands * 3.14159265f);
            base = lerp(pal.first, pal.second, t);

            // Storms / spots.
            const float storms = fbm3(nx * 10.0f + 1.2f, ny * 10.0f - 2.6f, nz * 10.0f + 0.7f, seed ^ 0x1234567u, 3,
                                      2.0f, 0.5f);
            const float storm_mask = smoothstep(0.78f, 0.94f, storms);
            base = lerp(base, ColorF{1.0f, 1.0f, 1.0f, 1.0f}, storm_mask * 0.10f);

            // Slight limb darkening.
            base = mul(base, 0.85f + 0.15f * nz);

            base = shade(nx, ny, nz, base);
          } else if (body.type == BodyType::Star) {
            // Star disc color is mostly emissive. Ignore external lighting.
            const float temp = static_cast<float>(body.surface_temp_k);
            const ColorF sc = star_color_from_temp(temp, seed ^ static_cast<std::uint32_t>(body.id));

            // Spots.
            const float spots = fbm3(nx * 7.0f, ny * 7.0f, nz * 7.0f, seed ^ 0xDEADBEEFu, 4, 2.1f, 0.55f);
            const float spot_mask = smoothstep(0.65f, 0.92f, spots);

            // Core/edge intensity.
            const float core = smoothstep(1.0f, 0.0f, r);
            base = mul(sc, 0.65f + 0.55f * core);
            base = lerp(base, mul(sc, 0.75f), spot_mask * 0.45f);

            // Bloom towards edge.
            const float bloom = smoothstep(0.70f, 1.0f, r);
            base = add(base, mul(sc, bloom * 0.30f));
            base = clamp01(base);
          } else if (body.type == BodyType::Moon) {
            const float rock = 0.55f * n1 + 0.45f * n2;
            base = lerp(ColorF{0.34f, 0.34f, 0.35f, 1.0f}, ColorF{0.72f, 0.72f, 0.74f, 1.0f}, rock);

            // Craters (cell noise in screen-space over the disc).
            const float d = worley_f1(sx * 3.8f + 12.3f, sy * 3.8f - 8.7f, seed ^ 0xBADC0DEu);
            const float crater = smoothstep(0.30f, 0.08f, d);
            const float rim = smoothstep(0.16f, 0.10f, d) - smoothstep(0.10f, 0.06f, d);
            base = lerp(base, mul(base, 0.78f), crater * 0.65f);
            base = lerp(base, ColorF{0.92f, 0.92f, 0.94f, 1.0f}, std::clamp(rim, 0.0f, 1.0f) * 0.55f);

            base = shade(nx, ny, nz, base);
          } else if (body.type == BodyType::Asteroid) {
            // Deformed silhouette: rmax depends on angle.
            const float ang = std::atan2(ty, tx);
            const float ax = std::cos(ang) * 1.8f + 2.0f;
            const float ay = std::sin(ang) * 1.8f - 1.0f;
            const float n = fbm2(ax * 2.2f, ay * 2.2f, seed ^ 0xABCDEF12u, 4, 2.0f, 0.5f);
            const float rmax = 0.80f + 0.25f * n;
            const float rr = length2(tx, ty);
            if (rr > rmax + aa) {
              // Outside asteroid.
              continue;
            }

            const float edge2 = 1.0f - smoothstep(rmax - aa * 2.0f, rmax + aa * 2.0f, rr);

            // Approximate normal as a squished sphere.
            const float asx = tx / std::max(1e-4f, rmax);
            const float asy = ty / std::max(1e-4f, rmax);
            const float ar = length2(asx, asy);
            const float az = std::sqrt(std::max(0.0f, 1.0f - ar * ar));
            float anx = asx;
            float any = asy;
            float anz = az;
            normalize3(anx, any, anz);

            const float rock = fbm3(anx * 5.0f + 0.2f, any * 5.0f - 1.1f, anz * 5.0f + 2.7f, seed ^ 0x33445566u, 4,
                                    2.0f, 0.55f);
            base = lerp(ColorF{0.30f, 0.28f, 0.26f, 1.0f}, ColorF{0.58f, 0.54f, 0.50f, 1.0f}, rock);
            base = shade(anx, any, anz, base);
            alpha = std::max(alpha, edge2);

            col = base;

            const std::size_t idx = (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                                     static_cast<std::size_t>(x)) *
                                    4;
            out[idx + 0] = f2u8(col.r);
            out[idx + 1] = f2u8(col.g);
            out[idx + 2] = f2u8(col.b);
            out[idx + 3] = f2u8(alpha);
            continue;
          } else {
            // Fallback: shaded grey sphere.
            const float rock = 0.65f * n1 + 0.35f * n2;
            base = lerp(ColorF{0.32f, 0.32f, 0.35f, 1.0f}, ColorF{0.74f, 0.74f, 0.78f, 1.0f}, rock);
            base = shade(nx, ny, nz, base);
          }

          col = base;
          alpha = std::max(alpha, edge);
        } else {
          // Only AA edge.
          alpha = std::max(alpha, edge);
        }
      }

      // Star corona glow (outside sphere radius norm).
      if (body.type == BodyType::Star) {
        const float rr = length2(tx, ty);
        const float inner = sphere_radius_norm;
        if (rr > inner && rr <= 1.0f) {
          const float glow = (1.0f - smoothstep(inner, 1.0f, rr));
          const float temp = static_cast<float>(body.surface_temp_k);
          const ColorF sc = star_color_from_temp(temp, seed ^ static_cast<std::uint32_t>(body.id));
          const float haze = fbm2(tx * 8.0f + 1.0f, ty * 8.0f - 2.0f, seed ^ 0xCAFED00Du, 3, 2.0f, 0.5f);
          const float g = glow * (0.35f + 0.65f * haze);
          const ColorF gc = mul(sc, 0.55f + 0.45f * g);
          col = add(col, mul(gc, g));
          alpha = std::max(alpha, g * 0.85f);
          col = clamp01(col);
        }
      }

      if (alpha <= 0.0001f) continue;

      const std::size_t idx = (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4;
      out[idx + 0] = f2u8(col.r);
      out[idx + 1] = f2u8(col.g);
      out[idx + 2] = f2u8(col.b);
      out[idx + 3] = f2u8(alpha);
    }
  }
}

}  // namespace nebula4x::ui
