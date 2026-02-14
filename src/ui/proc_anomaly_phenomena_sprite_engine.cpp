#include "ui/proc_anomaly_phenomena_sprite_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "nebula4x/core/procgen_obscure.h"
#include "nebula4x/util/hash_rng.h"
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
  h ^= (h << 7) ^ (h >> 9);
  return hash_u32(h);
}

static inline float u01_from_u32(std::uint32_t x) {
  // 24-bit mantissa into [0,1)
  return static_cast<float>(x & 0x00FFFFFFu) * (1.0f / 16777216.0f);
}

static inline float value_noise_2d(float x, float y, std::uint32_t seed) {
  const int ix = static_cast<int>(std::floor(x));
  const int iy = static_cast<int>(std::floor(y));
  const float fx = x - static_cast<float>(ix);
  const float fy = y - static_cast<float>(iy);

  const float a = u01_from_u32(hash_2d_i32(ix, iy, seed));
  const float b = u01_from_u32(hash_2d_i32(ix + 1, iy, seed));
  const float c = u01_from_u32(hash_2d_i32(ix, iy + 1, seed));
  const float d = u01_from_u32(hash_2d_i32(ix + 1, iy + 1, seed));

  const float ux = smoothstep(fx);
  const float uy = smoothstep(fy);
  const float ab = lerp(a, b, ux);
  const float cd = lerp(c, d, ux);
  return lerp(ab, cd, uy);
}

static inline float fbm(float x, float y, std::uint32_t seed, int octaves) {
  float v = 0.0f;
  float amp = 0.5f;
  float freq = 1.0f;
  for (int i = 0; i < octaves; ++i) {
    v += amp * value_noise_2d(x * freq, y * freq, seed + static_cast<std::uint32_t>(i) * 1013u);
    freq *= 2.0f;
    amp *= 0.5f;
  }
  return v;
}

static inline std::uint64_t float_to_u64_quant(float v, float scale) {
  const double q = std::round(static_cast<double>(v) * static_cast<double>(scale));
  const std::int64_t qi = static_cast<std::int64_t>(q);
  return static_cast<std::uint64_t>(qi);
}

static inline std::uint64_t hash_combine_u64(std::uint64_t h, std::uint64_t v) {
  // A small 64-bit mix (inspired by boost::hash_combine).
  v += 0x9e3779b97f4a7c15ULL;
  h ^= v + (h << 6) + (h >> 2);
  // Extra avalanche.
  h ^= (h >> 33);
  h *= 0xff51afd7ed558ccdULL;
  h ^= (h >> 33);
  h *= 0xc4ceb9fe1a85ec53ULL;
  h ^= (h >> 33);
  return h;
}

static inline std::uint16_t base_variant_from_kind(nebula4x::AnomalyKind kind, std::uint32_t seed) {
  switch (kind) {
    case nebula4x::AnomalyKind::Signal:
    case nebula4x::AnomalyKind::Distress:
    case nebula4x::AnomalyKind::Echo:
    case nebula4x::AnomalyKind::CodexEcho:
      return 0; // Radar / rings
    case nebula4x::AnomalyKind::Xenoarchaeology:
    case nebula4x::AnomalyKind::Ruins:
    case nebula4x::AnomalyKind::Artifact:
      return 1; // Geometric / runic
    case nebula4x::AnomalyKind::Distortion:
    case nebula4x::AnomalyKind::Phenomenon:
      return 2; // Swirl
    case nebula4x::AnomalyKind::Cache:
      return 3; // Facets
    default:
      break;
  }
  return static_cast<std::uint16_t>(hash_u32(static_cast<std::uint32_t>(kind) ^ seed) % 4u);
}

static inline ImU32 modulate_alpha(ImU32 col, float a_mul) {
  const std::uint32_t c = static_cast<std::uint32_t>(col);
  const std::uint32_t a = (c >> 24) & 0xFFu;
  const std::uint32_t na = static_cast<std::uint32_t>(std::clamp(a_mul, 0.0f, 1.0f) * static_cast<float>(a));
  return static_cast<ImU32>((c & 0x00FFFFFFu) | (na << 24));
}

static inline float hazard01(const nebula4x::Anomaly& a) {
  if (!(a.hazard_chance > 1e-9) || !(a.hazard_damage > 1e-9)) return 0.0f;
  // hazard_chance is 0..1, hazard_damage is game-defined. Map damage roughly into 0..1.
  const float dmg = static_cast<float>(std::clamp(a.hazard_damage / 20.0, 0.0, 1.0));
  return clamp01(static_cast<float>(a.hazard_chance) * dmg);
}

static inline float reward01(const nebula4x::Anomaly& a) {
  float r = 0.0f;
  if (a.research_reward > 1e-9) r += static_cast<float>(std::clamp(a.research_reward / 200.0, 0.0, 1.0));
  double tot = 0.0;
  for (const auto& [_, t] : a.mineral_reward) tot += std::max(0.0, t);
  if (tot > 1e-6) r += static_cast<float>(std::clamp(tot / 20000.0, 0.0, 1.0));
  if (!a.unlock_component_id.empty()) r += 0.25f;
  return clamp01(r);
}

} // namespace

// --- Lifetime / backend ---------------------------------------------------

ProcAnomalyPhenomenaSpriteEngine::~ProcAnomalyPhenomenaSpriteEngine() { shutdown(); }

void ProcAnomalyPhenomenaSpriteEngine::set_backend(UIRendererBackend backend, SDL_Renderer* sdl_renderer) {
  // Rebind if backend changed (or if SDL renderer changed).
  if (backend_ != backend || sdl_renderer_ != sdl_renderer) {
    shutdown();
  }
  backend_ = backend;
  sdl_renderer_ = sdl_renderer;
}

bool ProcAnomalyPhenomenaSpriteEngine::ready() const {
  if (backend_ == UIRendererBackend::SDLRenderer2) return sdl_renderer_ != nullptr;
#if NEBULA4X_UI_RENDERER_OPENGL2
  if (backend_ == UIRendererBackend::OpenGL2) return true;
#endif
  return false;
}

void ProcAnomalyPhenomenaSpriteEngine::begin_frame() {
  frame_++;
  stats_.generated_this_frame = 0;
  stats_.gen_ms_this_frame = 0.0;
  stats_.upload_ms_this_frame = 0.0;
  stats_.cache_sprites = static_cast<int>(cache_.size());
}

void ProcAnomalyPhenomenaSpriteEngine::clear() {
  for (auto& [_, e] : cache_) {
    if (e.sprite.tex_id) destroy_texture(e.sprite.tex_id);
  }
  cache_.clear();
  stats_.cache_sprites = 0;
}

void ProcAnomalyPhenomenaSpriteEngine::shutdown() {
  clear();
  backend_ = UIRendererBackend::Unknown;
  sdl_renderer_ = nullptr;
}

// --- Cache key / hash -----------------------------------------------------

std::size_t ProcAnomalyPhenomenaSpriteEngine::AnomalyKeyHash::operator()(const AnomalyKey& k) const noexcept {
  std::uint64_t h = 0;
  h = hash_combine_u64(h, k.id_hash);
  h = hash_combine_u64(h, static_cast<std::uint64_t>(k.seed));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(k.sprite_px));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(k.variant));
  h = hash_combine_u64(h, k.style_hash);
  return static_cast<std::size_t>(h);
}

std::uint64_t ProcAnomalyPhenomenaSpriteEngine::style_hash_from_cfg(const ProcAnomalyPhenomenaSpriteConfig& cfg) {
  std::uint64_t h = 0;
  h = hash_combine_u64(h, static_cast<std::uint64_t>(cfg.glyph_overlay ? 1 : 0));
  h = hash_combine_u64(h, float_to_u64_quant(cfg.glyph_strength, 1000.0f));
  // Do NOT include on-screen scale/opacity/animation: those are draw-time.
  return h;
}

ProcAnomalyPhenomenaSpriteEngine::SpriteInfo ProcAnomalyPhenomenaSpriteEngine::get_anomaly_sprite(
    const nebula4x::Anomaly& a, std::uint32_t seed, const ProcAnomalyPhenomenaSpriteConfig& cfg) {
  // Hash anomaly properties so IDs reused across saves don't collide.
  std::uint64_t idh = 0;
  idh = hash_combine_u64(idh, static_cast<std::uint64_t>(a.id));
  idh = hash_combine_u64(idh, static_cast<std::uint64_t>(a.system_id));
  idh = hash_combine_u64(idh, static_cast<std::uint64_t>(a.origin_anomaly_id));
  idh = hash_combine_u64(idh, static_cast<std::uint64_t>(a.kind));
  // Quantize position so tiny floating drift doesn't explode cache.
  idh = hash_combine_u64(idh, float_to_u64_quant(static_cast<float>(a.position_mkm.x), 10.0f));
  idh = hash_combine_u64(idh, float_to_u64_quant(static_cast<float>(a.position_mkm.y), 10.0f));
  idh = hash_combine_u64(idh, float_to_u64_quant(static_cast<float>(a.hazard_chance), 10000.0f));
  idh = hash_combine_u64(idh, float_to_u64_quant(static_cast<float>(a.hazard_damage), 100.0f));
  idh = hash_combine_u64(idh, float_to_u64_quant(static_cast<float>(a.research_reward), 100.0f));

  const std::uint16_t sprite_px = static_cast<std::uint16_t>(std::clamp(cfg.sprite_px, 24, 256));

  const std::uint16_t base_kind_variant = base_variant_from_kind(a.kind, seed);
  const std::uint16_t sub = static_cast<std::uint16_t>(((static_cast<std::uint64_t>(seed) ^ idh) >> 12) & 0xFFu);
  const std::uint16_t variant = static_cast<std::uint16_t>((base_kind_variant << 8) | sub);

  AnomalyKey key;
  key.id_hash = idh;
  key.seed = seed;
  key.sprite_px = sprite_px;
  key.variant = variant;
  key.style_hash = style_hash_from_cfg(cfg);

  return get_or_create(key, a, cfg);
}

ProcAnomalyPhenomenaSpriteEngine::SpriteInfo ProcAnomalyPhenomenaSpriteEngine::get_or_create(
    const AnomalyKey& key, const nebula4x::Anomaly& a, const ProcAnomalyPhenomenaSpriteConfig& cfg) {
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
  raster_anomaly(rgba, out.w, out.h, key.seed, key.id_hash, key.variant, a, cfg);
  stats_.gen_ms_this_frame += ms_since(t0);

  const auto t1 = Clock::now();
  out.tex_id = upload_rgba(rgba.data(), out.w, out.h);
  stats_.upload_ms_this_frame += ms_since(t1);

  if (!out.tex_id) return out;

  CacheEntry e;
  e.sprite = out;
  e.last_used_frame = frame_;
  cache_.emplace(key, e);
  stats_.generated_this_frame += 1;
  stats_.cache_sprites = static_cast<int>(cache_.size());

  trim_cache(static_cast<std::size_t>(std::max(8, cfg.max_cached_sprites)));

  return out;
}

// --- Upload / destroy -----------------------------------------------------

ImTextureID ProcAnomalyPhenomenaSpriteEngine::upload_rgba(const std::uint8_t* rgba, int w, int h) {
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
      nebula4x::log::warn(std::string("ProcAnomalyPhenomenaSpriteEngine: SDL_CreateRGBSurfaceFrom failed: ") +
                          SDL_GetError());
      return imgui_null_texture_id();
    }

    SDL_Texture* tex = SDL_CreateTextureFromSurface(sdl_renderer_, surf);
    SDL_FreeSurface(surf);

    if (!tex) {
      nebula4x::log::warn(std::string("ProcAnomalyPhenomenaSpriteEngine: SDL_CreateTextureFromSurface failed: ") +
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

void ProcAnomalyPhenomenaSpriteEngine::destroy_texture(ImTextureID id) {
  if (!imgui_texture_id_is_valid(id)) return;

  if (backend_ == UIRendererBackend::SDLRenderer2) {
    SDL_Texture* tex = sdl_texture_from_imgui_texture_id(id);
    SDL_DestroyTexture(tex);
    return;
  }

#if NEBULA4X_UI_RENDERER_OPENGL2
  if (backend_ == UIRendererBackend::OpenGL2) {
    const GLuint tex = gl_texture_from_imgui_texture_id<GLuint>(id);
    glDeleteTextures(1, &tex);
    return;
  }
#endif
}

void ProcAnomalyPhenomenaSpriteEngine::trim_cache(std::size_t max_entries) {
  if (cache_.size() <= max_entries) return;

  // Evict least recently used.
  while (cache_.size() > max_entries) {
    auto best_it = cache_.end();
    std::uint64_t best_frame = std::numeric_limits<std::uint64_t>::max();
    for (auto it = cache_.begin(); it != cache_.end(); ++it) {
      if (it->second.last_used_frame < best_frame) {
        best_frame = it->second.last_used_frame;
        best_it = it;
      }
    }
    if (best_it == cache_.end()) break;
    if (best_it->second.sprite.tex_id) destroy_texture(best_it->second.sprite.tex_id);
    cache_.erase(best_it);
  }
  stats_.cache_sprites = static_cast<int>(cache_.size());
}

// --- Draw helpers ---------------------------------------------------------

void ProcAnomalyPhenomenaSpriteEngine::draw_sprite_rotated(ImDrawList* draw,
                                                           ImTextureID tex,
                                                           const ImVec2& center,
                                                           float size_px,
                                                           float angle_rad,
                                                           ImU32 tint) {
  if (!draw || !tex || size_px <= 0.0f) return;

  const float c = std::cos(angle_rad);
  const float s = std::sin(angle_rad);
  const float h = 0.5f * size_px;

  // Four corners of a centered square.
  ImVec2 p0{-h, -h};
  ImVec2 p1{+h, -h};
  ImVec2 p2{+h, +h};
  ImVec2 p3{-h, +h};

  auto rot = [&](const ImVec2& p) -> ImVec2 {
    return ImVec2(center.x + p.x * c - p.y * s, center.y + p.x * s + p.y * c);
  };

  const ImVec2 q0 = rot(p0);
  const ImVec2 q1 = rot(p1);
  const ImVec2 q2 = rot(p2);
  const ImVec2 q3 = rot(p3);

  draw->AddImageQuad(tex, q0, q1, q2, q3, ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1), tint);
}

void ProcAnomalyPhenomenaSpriteEngine::draw_filaments(ImDrawList* draw,
                                                      const ImVec2& center,
                                                      float radius_px,
                                                      const nebula4x::Anomaly& a,
                                                      std::uint32_t seed,
                                                      double time_days,
                                                      ImU32 tint,
                                                      const ProcAnomalyPhenomenaSpriteConfig& cfg) {
  if (!draw) return;
  if (!cfg.filaments || cfg.filaments_max <= 0) return;
  if (!(radius_px > 1.0f)) return;

  const float hz = hazard01(a);
  const float rw = reward01(a);

  const std::uint64_t s0 = nebula4x::procgen_obscure::anomaly_seed(a) ^ (static_cast<std::uint64_t>(seed) << 1);
  nebula4x::util::HashRng rng(s0);

  const int nmax = std::clamp(cfg.filaments_max, 0, 128);
  int n = std::clamp(2 + static_cast<int>(std::round(hz * 5.0f + rw * 2.0f)), 1, nmax);
  n = std::min(n, nmax);
  if (n <= 0) return;

  // Reduce filament alpha, then modulate by hazard/reward.
  float base_a = 0.20f + 0.35f * hz + 0.10f * rw;
  base_a *= std::clamp(cfg.filament_strength, 0.0f, 4.0f);
  base_a = std::clamp(base_a, 0.02f, 0.9f);

  const float t = static_cast<float>(time_days);
  const float tw = 0.65f + 0.35f * hz;

  for (int i = 0; i < n; ++i) {
    const float a0 = static_cast<float>(rng.range(0.0, 6.283185307179586));
    const float span = static_cast<float>(rng.range(0.40, 1.35));
    const float r0 = radius_px * static_cast<float>(rng.range(0.60, 1.05));
    const float r1 = radius_px * static_cast<float>(rng.range(0.70, 1.25));
    const float wob = static_cast<float>(rng.range(2.0, 7.0));
    const float phase = static_cast<float>(rng.range(0.0, 6.283185307179586));

    const int segs = rng.range_int(20, 29);
    std::vector<ImVec2> pts;
    pts.reserve(static_cast<std::size_t>(segs) + 1u);

    for (int j = 0; j <= segs; ++j) {
      const float u = static_cast<float>(j) / static_cast<float>(segs);
      const float ang = a0 + (u - 0.5f) * span;
      const float rr = lerp(r0, r1, u);
      const float n0 = std::sinf((u * wob + t * 0.9f) * tw + phase);
      const float n1 = std::sinf((u * (wob * 0.7f) - t * 0.55f) * tw + phase * 1.7f);
      const float jitter = 1.0f + 0.08f * n0 + 0.045f * n1;
      const float rad = rr * jitter;

      pts.emplace_back(center.x + std::cos(ang) * rad, center.y + std::sin(ang) * rad);
    }

    const float a_mul = base_a * (0.70f + 0.30f * std::sinf(t * 0.8f + phase));
    const float thickness = std::clamp(0.9f + 1.0f * hz + 0.25f * rw, 0.75f, 3.25f);
    draw->AddPolyline(pts.data(), static_cast<int>(pts.size()), modulate_alpha(tint, a_mul), false, thickness);
  }

  // A few subtle radial rays (reads well at high zoom-out).
  if (hz > 0.15f || rw > 0.55f) {
    const int rays = std::clamp(2 + static_cast<int>(std::round(hz * 6.0f + rw * 3.0f)), 0, 18);
    for (int i = 0; i < rays; ++i) {
      const float ang = static_cast<float>(rng.range(0.0, 6.283185307179586));
      const float len = radius_px * static_cast<float>(rng.range(0.55, 1.10));
      const float wob = 0.04f + 0.03f * std::sinf(t * 1.2f + ang * 3.0f);
      const ImVec2 p1{center.x + std::cos(ang) * len, center.y + std::sin(ang) * len};
      const ImVec2 p2{center.x + std::cos(ang + wob) * (len * 1.12f), center.y + std::sin(ang + wob) * (len * 1.12f)};
      draw->AddLine(p1, p2, modulate_alpha(tint, base_a * 0.55f), 1.0f);
    }
  }
}

// --- Raster ---------------------------------------------------------------

void ProcAnomalyPhenomenaSpriteEngine::raster_anomaly(std::vector<std::uint8_t>& rgba,
                                                      int w,
                                                      int h,
                                                      std::uint32_t seed,
                                                      std::uint64_t id_hash,
                                                      std::uint16_t variant,
                                                      const nebula4x::Anomaly& a,
                                                      const ProcAnomalyPhenomenaSpriteConfig& cfg) {
  if (w <= 0 || h <= 0) return;
  if (rgba.size() < static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u) return;

  const std::uint16_t kind_variant = static_cast<std::uint16_t>(variant >> 8);
  const std::uint16_t sub_variant = static_cast<std::uint16_t>(variant & 0xFFu);

  const float hz = hazard01(a);
  const float rw = reward01(a);

  // Stable per-anomaly seed.
  const std::uint64_t aseed = nebula4x::procgen_obscure::anomaly_seed(a) ^ (static_cast<std::uint64_t>(seed) << 32) ^
                              (static_cast<std::uint64_t>(id_hash) >> 1);
  nebula4x::util::HashRng rng(aseed);

  // Basic style parameters.
  const float ring_freq = static_cast<float>(rng.range(7.0, 15.0));
  const float ray_freq = static_cast<float>(rng.range(6.0, 13.0));
  const float swirl = static_cast<float>(rng.range(1.0, 3.5)) * (0.5f + 0.7f * hz);
  const float grit = static_cast<float>(rng.range(0.25, 0.65));

  const std::uint32_t nseed0 = hash_u32(seed ^ static_cast<std::uint32_t>(sub_variant) ^ 0xA11A5EEDu);
  const std::uint32_t nseed1 = hash_u32(seed ^ static_cast<std::uint32_t>(sub_variant) ^ 0x51A7F1E1u);

  // Optional glyph overlay.
  std::uint8_t glyph_rows[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  if (cfg.glyph_overlay) {
    const std::string g = nebula4x::procgen_obscure::anomaly_signature_glyph(a);
    int row = 0;
    int col = 0;
    std::uint8_t cur = 0;
    for (char ch : g) {
      if (ch == '\n') {
        // Ignore "extra" newlines after full rows.
        if (col == 0) continue;
        if (row >= 8) break;
        if (col < 8) {
          cur = static_cast<std::uint8_t>(cur << static_cast<std::uint8_t>(8 - col));
        }
        glyph_rows[row] = cur;
        row++;
        col = 0;
        cur = 0;
        continue;
      }
      if (ch != '#' && ch != '.') continue;
      if (row >= 8) break;
      cur = static_cast<std::uint8_t>(cur << 1);
      if (ch == '#') cur |= 1u;
      col++;
      if (col >= 8) {
        glyph_rows[row] = cur;
        row++;
        col = 0;
        cur = 0;
      }
    }
    // If no trailing newline, commit last row if partial.
    if (row < 8 && col > 0) {
      cur = static_cast<std::uint8_t>(cur << static_cast<std::uint8_t>(8 - col));
      glyph_rows[row] = cur;
    }
  }

  const float inv_w = 1.0f / static_cast<float>(w);
  const float inv_h = 1.0f / static_cast<float>(h);

  // Glyph placement: centered square occupying ~35% of sprite width.
  const float glyph_extent = 0.36f; // in normalized [-1,1]
  const float gx0 = -glyph_extent;
  const float gx1 = +glyph_extent;
  const float gy0 = -glyph_extent;
  const float gy1 = +glyph_extent;

  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const float u = (static_cast<float>(x) + 0.5f) * inv_w;
      const float v = (static_cast<float>(y) + 0.5f) * inv_h;

      // Normalized coords in [-1,1].
      const float px = (u * 2.0f - 1.0f);
      const float py = (v * 2.0f - 1.0f);

      const float r = std::sqrt(px * px + py * py);
      const float ang = std::atan2(py, px);

      // Soft outer envelope.
      float env = 1.0f - smoothstep(0.82f, 1.02f, r);
      env *= env;
      if (env <= 0.0005f) {
        const std::size_t idx = (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                                 static_cast<std::size_t>(x)) *
                                4u;
        rgba[idx + 0] = 255;
        rgba[idx + 1] = 255;
        rgba[idx + 2] = 255;
        rgba[idx + 3] = 0;
        continue;
      }

      // Slight warp so shapes don't look perfectly symmetric.
      const float n = fbm(px * 3.5f + 11.7f, py * 3.5f - 5.2f, nseed0, 4);
      const float n2 = fbm(px * 7.0f - 2.3f, py * 7.0f + 9.1f, nseed1, 3);
      const float warp = (n - 0.5f) * 0.10f + (n2 - 0.5f) * 0.055f;

      const float rr = r * (1.0f + warp);
      const float aa = ang + warp * 1.5f;

      float pat = 0.0f;

      if (kind_variant == 0) {
        // --- SIGNAL: concentric rings + rays ---
        const float rings = 0.5f + 0.5f * std::sinf(rr * ring_freq * 6.2831853f + 1.1f + warp * 3.0f);
        const float ring_lines = std::pow(std::abs(rings), 9.0f);

        const float rays = 0.5f + 0.5f * std::sinf(aa * ray_freq + 0.7f + warp * 2.0f);
        const float ray_lines = std::pow(std::abs(rays), 10.0f);

        const float core = std::exp(-rr * rr * (28.0f + 22.0f * hz));
        pat = 0.55f * ring_lines + 0.30f * ray_lines * (1.0f - rr) + 0.20f * core;
        pat += grit * 0.15f * (n2 - 0.5f);
      } else if (kind_variant == 1) {
        // --- RUINS: square distance + runic lattice ---
        const float sx = std::abs(px);
        const float sy = std::abs(py);
        const float sq = std::max(sx, sy);

        const float sq_rings = 0.5f + 0.5f * std::sinf(sq * (ring_freq * 0.85f) * 6.2831853f + 0.2f + warp);
        const float sq_lines = std::pow(std::abs(sq_rings), 10.0f);

        const float diag0 = std::abs(std::sinf((px + py) * (ray_freq * 2.0f) + warp * 4.0f));
        const float diag1 = std::abs(std::sinf((px - py) * (ray_freq * 1.6f) - warp * 3.5f));
        const float lattice = std::pow(std::max(diag0, diag1), 8.0f);

        const float core = std::exp(-rr * rr * 22.0f);
        pat = 0.55f * sq_lines + 0.35f * lattice * (1.0f - sq) + 0.10f * core;
        pat += grit * 0.18f * (n - 0.5f);
      } else if (kind_variant == 2) {
        // --- VORTEX: spiral arms ---
        const float arms = 3.0f + static_cast<float>((sub_variant % 4u));
        const float spiral_ang = aa + swirl * (1.0f - rr) * (1.0f - rr);
        const float spiral = 0.5f + 0.5f * std::sinf(spiral_ang * arms + rr * ring_freq * 6.2831853f + warp * 5.0f);
        const float spiral_lines = std::pow(std::abs(spiral), 9.0f);
        const float rim = std::exp(-std::pow((rr - 0.55f), 2.0f) * (28.0f + 18.0f * hz));
        const float core = std::exp(-rr * rr * (18.0f + 18.0f * hz));
        pat = 0.55f * spiral_lines + 0.25f * rim + 0.15f * core;
        pat += grit * 0.14f * (n2 - 0.5f);
      } else {
        // --- CRYSTAL: simple Worley / facets ---
        const float scale = 4.0f + static_cast<float>(sub_variant % 4u);
        const float gx = (px * 0.85f + 0.12f) * scale;
        const float gy = (py * 0.85f - 0.08f) * scale;
        const int ix = static_cast<int>(std::floor(gx));
        const int iy = static_cast<int>(std::floor(gy));

        float best = 1e9f;
        for (int oy = -1; oy <= 1; ++oy) {
          for (int ox = -1; ox <= 1; ++ox) {
            const int cx = ix + ox;
            const int cy = iy + oy;
            const std::uint32_t h0 = hash_2d_i32(cx, cy, nseed0);
            const float rx = (static_cast<float>(h0 & 0xFFFFu) / 65535.0f);
            const float ry = (static_cast<float>((h0 >> 16) & 0xFFFFu) / 65535.0f);
            const float px2 = static_cast<float>(cx) + rx;
            const float py2 = static_cast<float>(cy) + ry;
            const float dx = gx - px2;
            const float dy = gy - py2;
            const float d2 = dx * dx + dy * dy;
            if (d2 < best) best = d2;
          }
        }

        const float d = std::sqrt(std::max(0.0f, best));
        const float cell = 1.0f - smoothstep(0.18f, 0.55f, d);
        const float facet = std::pow(std::abs(std::sinf((aa + warp) * (ray_freq * 0.65f))), 6.0f);
        const float rim = std::exp(-std::pow((rr - 0.60f), 2.0f) * 22.0f);
        pat = 0.55f * cell + 0.25f * facet * (1.0f - rr) + 0.15f * rim;
        pat += grit * 0.12f * (n - 0.5f);
      }

      // Inner attenuation so the very center isn't a solid blob.
      pat *= (0.35f + 0.65f * smoothstep(0.08f, 0.32f, rr));

      // Reward slightly brightens the core.
      if (rw > 0.01f) {
        const float core = std::exp(-rr * rr * 20.0f);
        pat += rw * 0.18f * core;
      }

      // Hazard adds sharper spikes.
      if (hz > 0.01f) {
        const float sp = 0.5f + 0.5f * std::sinf(aa * (ray_freq * 1.9f) + warp * 6.0f);
        pat += hz * 0.18f * std::pow(std::abs(sp), 14.0f) * (1.0f - rr);
      }

      // Glyph overlay.
      if (cfg.glyph_overlay && cfg.glyph_strength > 0.001f) {
        if (px >= gx0 && px <= gx1 && py >= gy0 && py <= gy1) {
          const float gu = (px - gx0) / (gx1 - gx0);
          const float gv = (py - gy0) / (gy1 - gy0);
          const int gx_i = std::clamp(static_cast<int>(gu * 8.0f), 0, 7);
          const int gy_i = std::clamp(static_cast<int>(gv * 8.0f), 0, 7);
          const std::uint8_t row = glyph_rows[gy_i];
          const int bit = (row >> (7 - gx_i)) & 1;
          if (bit) {
            const float edge = std::min({gu, 1.0f - gu, gv, 1.0f - gv});
            const float fade = smoothstep(0.00f, 0.06f, edge);
            pat += std::clamp(cfg.glyph_strength, 0.0f, 1.0f) * 0.65f * fade;
          }
        }
      }

      // Final alpha.
      float alpha = clamp01(pat) * env;
      // Slight gamma so faint structures survive.
      alpha = std::pow(std::clamp(alpha, 0.0f, 1.0f), 0.85f);

      const std::size_t idx = (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                               static_cast<std::size_t>(x)) *
                              4u;
      rgba[idx + 0] = 255;
      rgba[idx + 1] = 255;
      rgba[idx + 2] = 255;
      rgba[idx + 3] = static_cast<std::uint8_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
    }
  }
}

} // namespace nebula4x::ui
