#include "ui/proc_render_engine.h"

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

// 32-bit mix (variant of splitmix/wyhash-style avalanche). Fast and good enough
// for procedural textures.
static inline std::uint32_t hash_u32(std::uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

static inline std::uint32_t hash_2d_i32(int x, int y, std::uint32_t seed) {
  // Thomas Wang-ish mix of coordinates.
  std::uint32_t h = seed;
  h ^= hash_u32(static_cast<std::uint32_t>(x) + 0x9e3779b9u);
  h ^= hash_u32(static_cast<std::uint32_t>(y) + 0x85ebca6bu);
  return hash_u32(h);
}

struct Rng {
  std::uint32_t s = 0;
  explicit Rng(std::uint32_t seed) : s(seed ? seed : 0x12345678u) {}
  std::uint32_t next_u32() {
    // Xorshift32.
    std::uint32_t x = s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s = x;
    return x;
  }
  float next_f01() { return (next_u32() & 0x00ffffffu) / 16777216.0f; }
};

static inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

static inline float smoothstep(float t) {
  // Smooth Hermite interpolation.
  t = std::clamp(t, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// --- Value noise + fBm ----------------------------------------------------------

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

// --- Worley / Voronoi noise (F1) ------------------------------------------------

static float worley_f1(float x, float y, std::uint32_t seed) {
  const int ix = static_cast<int>(std::floor(x));
  const int iy = static_cast<int>(std::floor(y));
  const float fx = x - static_cast<float>(ix);
  const float fy = y - static_cast<float>(iy);

  float min_d2 = 1e9f;

  // Check nearest feature points in surrounding 3x3 cells.
  for (int oy = -1; oy <= 1; ++oy) {
    for (int ox = -1; ox <= 1; ++ox) {
      const int cx = ix + ox;
      const int cy = iy + oy;
      const std::uint32_t h = hash_2d_i32(cx, cy, seed);

      // One feature point per cell (good enough for visuals).
      const float px = (h & 0xffffu) / 65535.0f;
      const float py = ((h >> 16) & 0xffffu) / 65535.0f;

      const float dx = static_cast<float>(ox) + px - fx;
      const float dy = static_cast<float>(oy) + py - fy;
      const float d2 = dx * dx + dy * dy;
      min_d2 = std::min(min_d2, d2);
    }
  }
  return std::sqrt(min_d2);
}

// Helper to additively blend a contribution into an RGBA8 buffer.
static inline void add_rgba(std::uint8_t* px, int add_r, int add_g, int add_b, int add_a) {
  px[0] = static_cast<std::uint8_t>(std::min(255, static_cast<int>(px[0]) + add_r));
  px[1] = static_cast<std::uint8_t>(std::min(255, static_cast<int>(px[1]) + add_g));
  px[2] = static_cast<std::uint8_t>(std::min(255, static_cast<int>(px[2]) + add_b));
  px[3] = static_cast<std::uint8_t>(std::min(255, static_cast<int>(px[3]) + add_a));
}

static void stamp_star(std::vector<std::uint8_t>& rgba,
                       int w,
                       int h,
                       float sx,
                       float sy,
                       float radius,
                       float r,
                       float g,
                       float b,
                       float alpha) {
  if (radius <= 0.01f || alpha <= 0.001f) return;
  const float r2 = radius * radius;

  const int x0 = std::max(0, static_cast<int>(std::floor(sx - radius - 1.0f)));
  const int x1 = std::min(w - 1, static_cast<int>(std::ceil(sx + radius + 1.0f)));
  const int y0 = std::max(0, static_cast<int>(std::floor(sy - radius - 1.0f)));
  const int y1 = std::min(h - 1, static_cast<int>(std::ceil(sy + radius + 1.0f)));

  const float a255 = 255.0f * std::clamp(alpha, 0.0f, 1.0f);
  const float r255 = 255.0f * std::clamp(r, 0.0f, 1.0f);
  const float g255 = 255.0f * std::clamp(g, 0.0f, 1.0f);
  const float b255 = 255.0f * std::clamp(b, 0.0f, 1.0f);

  for (int y = y0; y <= y1; ++y) {
    const float dy = (static_cast<float>(y) + 0.5f) - sy;
    for (int x = x0; x <= x1; ++x) {
      const float dx = (static_cast<float>(x) + 0.5f) - sx;
      const float d2 = dx * dx + dy * dy;
      if (d2 > r2) continue;
      // Quadratic falloff, squared again for a softer core.
      const float t = 1.0f - (d2 / r2);
      const float falloff = t * t;
      const float a = a255 * falloff;
      const int add_a = static_cast<int>(a);
      const int add_r = static_cast<int>(r255 * (a / 255.0f));
      const int add_g = static_cast<int>(g255 * (a / 255.0f));
      const int add_b = static_cast<int>(b255 * (a / 255.0f));
      std::uint8_t* px = &rgba[static_cast<std::size_t>((y * w + x) * 4)];
      add_rgba(px, add_r, add_g, add_b, add_a);
    }
  }
}

static std::array<float, 3> hue_to_rgb(float hue01, float sat01, float val01) {
  float r, g, b;
  ImGui::ColorConvertHSVtoRGB(
      std::fmod(hue01, 1.0f), std::clamp(sat01, 0.0f, 1.0f), std::clamp(val01, 0.0f, 1.0f), r, g, b);
  return {r, g, b};
}

struct ScrollTiles {
  int tile_x0 = 0;
  int tile_y0 = 0;
  float frac_x = 0.0f;
  float frac_y = 0.0f;
};

static ScrollTiles compute_scroll_tiles(float offset_x, float offset_y, float parallax, int tile_px) {
  ScrollTiles s;
  const float sx = offset_x * parallax;
  const float sy = offset_y * parallax;
  const float tx = sx / static_cast<float>(tile_px);
  const float ty = sy / static_cast<float>(tile_px);
  s.tile_x0 = static_cast<int>(std::floor(tx));
  s.tile_y0 = static_cast<int>(std::floor(ty));
  s.frac_x = (tx - static_cast<float>(s.tile_x0)) * static_cast<float>(tile_px);
  s.frac_y = (ty - static_cast<float>(s.tile_y0)) * static_cast<float>(tile_px);
  return s;
}

static inline std::uint64_t hash_combine_u64(std::uint64_t h, std::uint64_t v) {
  // From boost::hash_combine.
  return h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2));
}

static inline std::uint64_t float_to_u64_quant(float f, float scale) {
  const double q = std::round(static_cast<double>(f) * static_cast<double>(scale));
  return static_cast<std::uint64_t>(static_cast<std::int64_t>(q));
}

} // namespace

// --- ProcRenderEngine ------------------------------------------------------------

ProcRenderEngine::ProcRenderEngine() = default;

ProcRenderEngine::~ProcRenderEngine() {
  // Best-effort cleanup. We expect the app to call shutdown() before the backend
  // is torn down.
  shutdown();
}

void ProcRenderEngine::set_backend(UIRendererBackend backend, SDL_Renderer* sdl_renderer) {
  if (backend_ != backend || sdl_renderer_ != sdl_renderer) {
    // Backend changed: drop cached tiles to avoid mixing handle types.
    shutdown();
    backend_ = backend;
    sdl_renderer_ = sdl_renderer;
  }
}

bool ProcRenderEngine::ready() const {
  if (backend_ == UIRendererBackend::SDLRenderer2) return sdl_renderer_ != nullptr;
  // OpenGL needs no extra pointers here; the active GL context must be current.
#if NEBULA4X_UI_RENDERER_OPENGL2
  return true;
#else
  return false;
#endif
}

void ProcRenderEngine::shutdown() {
  for (auto& kv : cache_) {
    destroy_tile(kv.second);
  }
  cache_.clear();
  stats_ = {};
}

void ProcRenderEngine::begin_frame() {
  ++frame_index_;
  stats_.generated_this_frame = 0;
  stats_.gen_ms_this_frame = 0.0;
  stats_.upload_ms_this_frame = 0.0;
  stats_.cache_tiles = static_cast<int>(cache_.size());
}

void ProcRenderEngine::clear() {
  shutdown();
}

std::size_t ProcRenderEngine::TileKeyHash::operator()(const TileKey& k) const noexcept {
  std::uint64_t h = 0;
  h = hash_combine_u64(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.layer)));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.tx)));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.ty)));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.tile_px)));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(k.seed));
  h = hash_combine_u64(h, k.style_hash);
  return static_cast<std::size_t>(h);
}

std::uint64_t ProcRenderEngine::compute_style_hash(const ProcRenderConfig& cfg) {
  std::uint64_t h = 0;
  h = hash_combine_u64(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(cfg.tile_px)));
  h = hash_combine_u64(h, float_to_u64_quant(cfg.star_density, 1000.0f));
  h = hash_combine_u64(h, static_cast<std::uint64_t>(cfg.nebula_enable ? 1 : 0));
  h = hash_combine_u64(h, float_to_u64_quant(cfg.nebula_strength, 1000.0f));
  h = hash_combine_u64(h, float_to_u64_quant(cfg.nebula_scale, 1000.0f));
  h = hash_combine_u64(h, float_to_u64_quant(cfg.nebula_warp, 1000.0f));
  return h;
}

void ProcRenderEngine::trim_cache(int max_tiles) {
  if (max_tiles < 4) max_tiles = 4;
  if (static_cast<int>(cache_.size()) <= max_tiles) return;

  // Naive LRU eviction: find oldest tiles until we're under budget.
  // For our expected cache sizes (<~200) this is fine.
  while (static_cast<int>(cache_.size()) > max_tiles) {
    auto oldest = cache_.begin();
    for (auto it = cache_.begin(); it != cache_.end(); ++it) {
      if (it->second.last_used_frame < oldest->second.last_used_frame) oldest = it;
    }
    destroy_tile(oldest->second);
    cache_.erase(oldest);
  }

  stats_.cache_tiles = static_cast<int>(cache_.size());
}

void ProcRenderEngine::destroy_tile(TileEntry& entry) {
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

ImTextureID ProcRenderEngine::upload_rgba_tile(const std::uint8_t* rgba, int w, int h) {
  if (!rgba || w <= 0 || h <= 0) return imgui_null_texture_id();
  if (!ready()) return imgui_null_texture_id();

  if (backend_ == UIRendererBackend::SDLRenderer2) {
    // Create a surface wrapping RGBA bytes with explicit masks, then let SDL
    // convert it into an SDL_Texture.
    //
    // Masks are chosen so the input byte layout is RGBA on both endian variants.
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

    SDL_Surface* surf = SDL_CreateRGBSurfaceFrom(
        const_cast<std::uint8_t*>(rgba), w, h, 32, w * 4, rmask, gmask, bmask, amask);
    if (!surf) {
      nebula4x::log::warn(std::string("ProcRenderEngine: SDL_CreateRGBSurfaceFrom failed: ") + SDL_GetError());
      return imgui_null_texture_id();
    }

    SDL_Texture* tex = SDL_CreateTextureFromSurface(sdl_renderer_, surf);
    SDL_FreeSurface(surf);

    if (!tex) {
      nebula4x::log::warn(std::string("ProcRenderEngine: SDL_CreateTextureFromSurface failed: ") + SDL_GetError());
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

ImTextureID ProcRenderEngine::get_or_create_tile(const TileKey& key, const ProcRenderConfig& cfg) {
  auto it = cache_.find(key);
  if (it != cache_.end()) {
    it->second.last_used_frame = frame_index_;
    return it->second.tex_id;
  }

  const int max_new_tiles = std::max(0, cfg.max_new_tiles_per_frame);
  if (max_new_tiles > 0 && stats_.generated_this_frame >= max_new_tiles) {
    return imgui_null_texture_id();
  }

  const double max_new_tile_ms = static_cast<double>(cfg.max_new_tile_ms_per_frame);
  if (max_new_tile_ms > 0.0) {
    const double frame_tile_ms = stats_.gen_ms_this_frame + stats_.upload_ms_this_frame;
    if (frame_tile_ms >= max_new_tile_ms) {
      return imgui_null_texture_id();
    }
  }

  // Generate + upload.
  const auto t0 = Clock::now();
  const std::size_t rgba_bytes =
      static_cast<std::size_t>(key.tile_px) * static_cast<std::size_t>(key.tile_px) * 4;
  if (scratch_rgba_.size() < rgba_bytes) {
    scratch_rgba_.resize(rgba_bytes);
  }
  generate_tile_rgba(scratch_rgba_, key.tile_px, key.tile_px, key.layer, key.tx, key.ty, key.seed, cfg);
  stats_.gen_ms_this_frame += ms_since(t0);

  const auto t1 = Clock::now();
  ImTextureID tex_id = upload_rgba_tile(scratch_rgba_.data(), key.tile_px, key.tile_px);
  stats_.upload_ms_this_frame += ms_since(t1);

  if (!imgui_texture_id_is_valid(tex_id)) return imgui_null_texture_id();

  TileEntry e;
  e.tex_id = tex_id;
  e.last_used_frame = frame_index_;
  e.w = key.tile_px;
  e.h = key.tile_px;
  cache_.emplace(key, e);
  stats_.generated_this_frame += 1;
  stats_.cache_tiles = static_cast<int>(cache_.size());

  // Respect cache budget.
  trim_cache(cfg.max_cached_tiles);
  return tex_id;
}

void ProcRenderEngine::draw_background(ImDrawList* draw,
                                      const ImVec2& origin,
                                      const ImVec2& size,
                                      ImU32 tint,
                                      float offset_px_x,
                                      float offset_px_y,
                                      std::uint32_t seed,
                                      const ProcRenderConfig& cfg) {
  if (!draw) return;
  if (cfg.tile_px <= 0) return;
  if (!ready()) return;

  const int tile_px = std::clamp(cfg.tile_px, 64, 1024);
  const std::uint64_t style_hash = compute_style_hash(cfg);

  // Determine how many tiles we need to cover the viewport.
  const int tiles_x = static_cast<int>(std::ceil(size.x / static_cast<float>(tile_px))) + 1;
  const int tiles_y = static_cast<int>(std::ceil(size.y / static_cast<float>(tile_px))) + 1;

  struct TileCoord {
    int i = 0;
    int j = 0;
    float dist2 = 0.0f;
  };
  std::vector<TileCoord> draw_order;
  draw_order.reserve(static_cast<std::size_t>(tiles_x) * static_cast<std::size_t>(tiles_y));
  const float center_i = static_cast<float>(tiles_x - 1) * 0.5f;
  const float center_j = static_cast<float>(tiles_y - 1) * 0.5f;
  for (int j = 0; j < tiles_y; ++j) {
    for (int i = 0; i < tiles_x; ++i) {
      const float dx = static_cast<float>(i) - center_i;
      const float dy = static_cast<float>(j) - center_j;
      draw_order.push_back(TileCoord{i, j, dx * dx + dy * dy});
    }
  }
  std::sort(draw_order.begin(), draw_order.end(), [](const TileCoord& a, const TileCoord& b) {
    if (a.dist2 != b.dist2) return a.dist2 < b.dist2;
    if (a.j != b.j) return a.j < b.j;
    return a.i < b.i;
  });

  // The background is a stack of layers to create depth.
  struct LayerDesc {
    int layer;
    float parallax;
    float alpha;
    bool enabled;
  };

  const float base_parallax = std::clamp(cfg.parallax, 0.0f, 1.0f);
  const std::array<LayerDesc, 3> layers = {
      LayerDesc{0, std::clamp(base_parallax * 0.10f, 0.0f, 1.0f), std::clamp(cfg.nebula_strength, 0.0f, 1.0f), cfg.nebula_enable},
      LayerDesc{1, std::clamp(base_parallax * 0.55f, 0.0f, 1.0f), 1.0f, true},
      LayerDesc{2, std::clamp(base_parallax * 1.00f, 0.0f, 1.0f), 1.0f, true},
  };

  const ImVec4 tint_f = ImGui::ColorConvertU32ToFloat4(tint);
  struct ActiveLayer {
    int layer = 0;
    ScrollTiles scroll;
    ImU32 tint_u32 = 0;
    ImU32 fallback_u32 = 0;
  };
  std::array<ActiveLayer, 3> active_layers{};
  std::size_t active_count = 0;
  for (const auto& layer : layers) {
    if (!layer.enabled) continue;
    if (active_count >= active_layers.size()) break;

    ImVec4 layer_tint = tint_f;
    layer_tint.w *= std::clamp(layer.alpha, 0.0f, 1.0f);

    ActiveLayer& dst = active_layers[active_count++];
    dst.layer = layer.layer;
    dst.scroll = compute_scroll_tiles(offset_px_x, offset_px_y, layer.parallax, tile_px);
    dst.tint_u32 = ImGui::ColorConvertFloat4ToU32(layer_tint);

    ImVec4 fallback_tint = tint_f;
    fallback_tint.w *= std::clamp(layer.alpha * 0.08f, 0.0f, 1.0f);
    dst.fallback_u32 = ImGui::ColorConvertFloat4ToU32(fallback_tint);
  }

  const float uv_inset = 0.5f / static_cast<float>(tile_px);
  const ImVec2 uv0(uv_inset, uv_inset);
  const ImVec2 uv1(1.0f - uv_inset, 1.0f - uv_inset);
  const float tile_overlap_px = 0.60f;

  for (const TileCoord& tc : draw_order) {
    const int i = tc.i;
    const int j = tc.j;
    for (std::size_t li = 0; li < active_count; ++li) {
      const ActiveLayer& layer = active_layers[li];
      const int tx = layer.scroll.tile_x0 + i;
      const int ty = layer.scroll.tile_y0 + j;

      TileKey key;
      key.layer = layer.layer;
      key.tx = tx;
      key.ty = ty;
      key.tile_px = tile_px;
      key.seed = seed;
      key.style_hash = style_hash;

      const ImVec2 p0(origin.x + static_cast<float>(i * tile_px) - layer.scroll.frac_x,
                      origin.y + static_cast<float>(j * tile_px) - layer.scroll.frac_y);
      const ImVec2 p1(p0.x + static_cast<float>(tile_px), p0.y + static_cast<float>(tile_px));
      ImTextureID tile_id = get_or_create_tile(key, cfg);
      if (imgui_texture_id_is_valid(tile_id)) {
        draw->AddImage(tile_id,
                       ImVec2(p0.x - tile_overlap_px, p0.y - tile_overlap_px),
                       ImVec2(p1.x + tile_overlap_px, p1.y + tile_overlap_px),
                       uv0,
                       uv1,
                       layer.tint_u32);
      } else if ((layer.fallback_u32 >> 24) != 0u) {
        draw->AddRectFilled(p0, p1, layer.fallback_u32);
      }

      if (cfg.debug_show_tile_bounds) {
        draw->AddRect(p0, p1, IM_COL32(255, 0, 255, 120));
      }
    }
  }

  stats_.cache_tiles = static_cast<int>(cache_.size());
}

void ProcRenderEngine::generate_tile_rgba(std::vector<std::uint8_t>& out_rgba,
                                         int w,
                                         int h,
                                         int layer,
                                         int tile_x,
                                         int tile_y,
                                         std::uint32_t seed,
                                         const ProcRenderConfig& cfg) {
  if (w <= 0 || h <= 0) return;
  if (static_cast<int>(out_rgba.size()) < w * h * 4) return;

  // Clear to transparent.
  std::fill(out_rgba.begin(), out_rgba.end(), std::uint8_t{0});

  const float base_x = static_cast<float>(tile_x * w);
  const float base_y = static_cast<float>(tile_y * h);

  if (layer == 0) {
    // Nebula haze layer: fBm + Worley ridges with domain warping.
    if (!cfg.nebula_enable || cfg.nebula_strength <= 0.001f) return;

    const float strength = std::clamp(cfg.nebula_strength, 0.0f, 1.0f);
    const float scale = std::max(0.05f, cfg.nebula_scale);
    const float warp = std::clamp(cfg.nebula_warp, 0.0f, 2.0f);

    const float freq = 0.0022f / scale;
    const float worley_freq = 0.010f / scale;

    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        const float gx = (base_x + static_cast<float>(x)) * freq;
        const float gy = (base_y + static_cast<float>(y)) * freq;

        // Domain warp field.
        const float wx = fbm(gx * 1.9f, gy * 1.9f, seed ^ 0x51ed270bu, 3, 2.1f, 0.55f) - 0.5f;
        const float wy = fbm(gx * 1.9f, gy * 1.9f, seed ^ 0x2f9be6cbu, 3, 2.1f, 0.55f) - 0.5f;

        const float nx = gx + wx * warp;
        const float ny = gy + wy * warp;

        const float f = fbm(nx, ny, seed ^ 0xa341316cu, 5, 2.05f, 0.55f);

        const float w1 = worley_f1((base_x + static_cast<float>(x)) * worley_freq,
                                   (base_y + static_cast<float>(y)) * worley_freq,
                                   seed ^ 0x9e3779b9u);

        // Convert Worley distance (0..~1.4) into a ridge field.
        const float ridge = 1.0f - std::clamp(w1, 0.0f, 1.0f);
        const float ridge2 = ridge * ridge;

        // Cloud density: bias + ridges.
        float d = (f - 0.52f) * 1.9f;
        d += ridge2 * 0.65f;
        d = std::clamp(d, 0.0f, 1.0f);

        // Feather to avoid hard edges.
        d = d * d;

        const float alpha = strength * d;
        if (alpha <= 0.002f) continue;

        const float hue = fbm(nx * 0.6f, ny * 0.6f, seed ^ 0x7f4a7c15u, 2, 2.0f, 0.5f);
        const float sat = std::clamp(0.35f + 0.45f * d, 0.0f, 1.0f);
        const float val = std::clamp(0.45f + 0.55f * d, 0.0f, 1.0f);
        const auto rgb = hue_to_rgb(hue, sat, val);

        const int add_a = static_cast<int>(alpha * 255.0f);
        const int add_r = static_cast<int>(rgb[0] * alpha * 255.0f);
        const int add_g = static_cast<int>(rgb[1] * alpha * 255.0f);
        const int add_b = static_cast<int>(rgb[2] * alpha * 255.0f);

        std::uint8_t* px = &out_rgba[static_cast<std::size_t>((y * w + x) * 4)];
        add_rgba(px, add_r, add_g, add_b, add_a);
      }
    }
    return;
  }

  // Star layers.
  const float density = std::clamp(cfg.star_density, 0.0f, 4.0f);
  if (density <= 0.001f) return;

  const bool near_layer = (layer == 2);
  const float cell_base = near_layer ? 46.0f : 18.0f;
  const float cell = std::clamp(cell_base / std::sqrt(std::max(0.15f, density)), 8.0f, 128.0f);
  const float prob = std::clamp((near_layer ? 0.16f : 0.55f) * density, 0.0f, 1.0f);

  const float world_x0 = base_x;
  const float world_y0 = base_y;
  const float world_x1 = world_x0 + static_cast<float>(w);
  const float world_y1 = world_y0 + static_cast<float>(h);

  // Include neighboring cells so stars crossing tile edges remain seamless.
  const float max_star_radius = near_layer ? 8.0f : 4.0f;
  const int gcx0 = static_cast<int>(std::floor((world_x0 - max_star_radius) / cell));
  const int gcy0 = static_cast<int>(std::floor((world_y0 - max_star_radius) / cell));
  const int gcx1 = static_cast<int>(std::floor((world_x1 + max_star_radius) / cell));
  const int gcy1 = static_cast<int>(std::floor((world_y1 + max_star_radius) / cell));

  for (int gcy = gcy0; gcy <= gcy1; ++gcy) {
    for (int gcx = gcx0; gcx <= gcx1; ++gcx) {
      const std::uint32_t h0 = hash_2d_i32(gcx, gcy, seed ^ (near_layer ? 0x02u : 0x01u));
      Rng rng(h0);
      if (rng.next_f01() > prob) continue;

      const float px_world = (static_cast<float>(gcx) + rng.next_f01()) * cell;
      const float py_world = (static_cast<float>(gcy) + rng.next_f01()) * cell;
      const float px = px_world - world_x0;
      const float py = py_world - world_y0;
      if (px < -max_star_radius || py < -max_star_radius || px > static_cast<float>(w) + max_star_radius ||
          py > static_cast<float>(h) + max_star_radius) {
        continue;
      }

      // Brightness distribution: many dim stars, few bright.
      const float m = rng.next_f01();
      float brightness = near_layer ? std::pow(m, 1.6f) : std::pow(m, 3.2f);
      brightness = std::clamp(brightness, 0.02f, 1.0f);

      // Subtle color temperature variation.
      const float t = rng.next_f01();
      float hue = 0.0f;
      float sat = 0.0f;
      if (t < 0.55f) {
        // Cool whites.
        hue = lerp(0.55f, 0.70f, rng.next_f01());
        sat = lerp(0.05f, 0.18f, rng.next_f01());
      } else {
        // Warm whites.
        hue = lerp(0.02f, 0.10f, rng.next_f01());
        sat = lerp(0.10f, 0.28f, rng.next_f01());
      }

      float sr, sg, sb;
      ImGui::ColorConvertHSVtoRGB(hue, sat, 1.0f, sr, sg, sb);

      const float radius =
          near_layer ? (1.0f + 2.6f * std::sqrt(brightness)) : (0.55f + 1.25f * std::sqrt(brightness));
      const float alpha = near_layer ? (0.55f + 0.40f * brightness) : (0.30f + 0.30f * brightness);
      stamp_star(out_rgba, w, h, px, py, radius, sr, sg, sb, alpha);

      // Bright stars get a tiny extra bloom.
      if (near_layer && brightness > 0.82f) {
        stamp_star(out_rgba, w, h, px, py, radius * 2.0f, sr, sg, sb, alpha * 0.10f);
      }
    }
  }
}

} // namespace nebula4x::ui
