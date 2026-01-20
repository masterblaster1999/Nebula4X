#include "ui/raymarch_nebula.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace nebula4x::ui {
namespace {

static float clampf(float v, float a, float b) {
  return std::max(a, std::min(b, v));
}

static float lerpf(float a, float b, float t) {
  return a + (b - a) * t;
}

static float smoothstep(float t) {
  // Smooth cubic Hermite interpolation.
  t = clampf(t, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

static std::uint32_t hash_u32(std::uint32_t x) {
  // Murmur3-ish finalizer.
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

static std::uint32_t hash_combine(std::uint32_t a, std::uint32_t b) {
  return hash_u32(a ^ (b + 0x9e3779b9U + (a << 6) + (a >> 2)));
}

static std::uint32_t hash3_i32(int x, int y, int z, std::uint32_t seed) {
  // Stable 3D integer hash.
  std::uint32_t h = seed;
  h = hash_combine(h, static_cast<std::uint32_t>(x) * 0x8da6b343U);
  h = hash_combine(h, static_cast<std::uint32_t>(y) * 0xd8163841U);
  h = hash_combine(h, static_cast<std::uint32_t>(z) * 0xcb1ab31fU);
  return h;
}

static float rand01(std::uint32_t h) {
  // 24-bit mantissa-ish.
  return static_cast<float>((h >> 8) & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
}

struct Vec3 {
  float x{0.0f};
  float y{0.0f};
  float z{0.0f};
};

static Vec3 operator+(Vec3 a, Vec3 b) {
  return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
}
static Vec3 operator-(Vec3 a, Vec3 b) {
  return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}
static Vec3 operator*(Vec3 a, float s) {
  return Vec3{a.x * s, a.y * s, a.z * s};
}
static Vec3 operator*(float s, Vec3 a) {
  return a * s;
}

static float dot(Vec3 a, Vec3 b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

static float length(Vec3 a) {
  return std::sqrt(std::max(0.0f, dot(a, a)));
}

static Vec3 normalize(Vec3 a) {
  const float len = length(a);
  if (len <= 1e-12f) return Vec3{0.0f, 0.0f, 0.0f};
  const float inv = 1.0f / len;
  return a * inv;
}

static Vec3 reflect(Vec3 i, Vec3 n) {
  // Reflect i around n.
  // Use the (float * Vec3) overload so MSVC doesn't warn about an unused operator.
  return i - (2.0f * dot(n, i)) * n;
}

struct Color4 {
  float r{0.0f};
  float g{0.0f};
  float b{0.0f};
  float a{0.0f};
};

static Color4 operator+(Color4 a, Color4 b) {
  return Color4{a.r + b.r, a.g + b.g, a.b + b.b, a.a + b.a};
}
static Color4 operator*(Color4 a, float s) {
  return Color4{a.r * s, a.g * s, a.b * s, a.a * s};
}

static Color4 clamp01(Color4 c) {
  c.r = clampf(c.r, 0.0f, 1.0f);
  c.g = clampf(c.g, 0.0f, 1.0f);
  c.b = clampf(c.b, 0.0f, 1.0f);
  c.a = clampf(c.a, 0.0f, 1.0f);
  return c;
}

static float luma(Color4 c) {
  return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}

// --- 3D value noise (procedural) ---

static float value_noise_3d(Vec3 p, std::uint32_t seed) {
  const int xi = static_cast<int>(std::floor(p.x));
  const int yi = static_cast<int>(std::floor(p.y));
  const int zi = static_cast<int>(std::floor(p.z));
  const float xf = p.x - static_cast<float>(xi);
  const float yf = p.y - static_cast<float>(yi);
  const float zf = p.z - static_cast<float>(zi);

  const float u = smoothstep(xf);
  const float v = smoothstep(yf);
  const float w = smoothstep(zf);

  auto n = [&](int x, int y, int z) {
    return rand01(hash3_i32(x, y, z, seed)) * 2.0f - 1.0f;
  };

  const float n000 = n(xi, yi, zi);
  const float n100 = n(xi + 1, yi, zi);
  const float n010 = n(xi, yi + 1, zi);
  const float n110 = n(xi + 1, yi + 1, zi);
  const float n001 = n(xi, yi, zi + 1);
  const float n101 = n(xi + 1, yi, zi + 1);
  const float n011 = n(xi, yi + 1, zi + 1);
  const float n111 = n(xi + 1, yi + 1, zi + 1);

  const float nx00 = lerpf(n000, n100, u);
  const float nx10 = lerpf(n010, n110, u);
  const float nx01 = lerpf(n001, n101, u);
  const float nx11 = lerpf(n011, n111, u);
  const float nxy0 = lerpf(nx00, nx10, v);
  const float nxy1 = lerpf(nx01, nx11, v);
  return lerpf(nxy0, nxy1, w);
}

static float fbm_3d(Vec3 p, std::uint32_t seed) {
  float sum = 0.0f;
  float amp = 0.5f;
  float freq = 1.0f;
  for (int i = 0; i < 4; ++i) {
    sum += amp * value_noise_3d(p * freq, seed ^ (0x9E3779B9u * static_cast<std::uint32_t>(i + 1)));
    freq *= 2.02f;
    amp *= 0.5f;
  }
  return sum;  // roughly [-1, 1]
}

static Vec3 fbm_vec3(Vec3 p, std::uint32_t seed) {
  return Vec3{
      fbm_3d(p + Vec3{11.3f, 7.1f, 3.7f}, seed ^ 0xA531u),
      fbm_3d(p + Vec3{2.9f, 19.2f, 5.4f}, seed ^ 0xC0FFu),
      fbm_3d(p + Vec3{13.7f, 3.3f, 17.0f}, seed ^ 0xBEEFu),
  };
}

// --- SDF primitives + smooth ops ---

static float sdf_sphere(Vec3 p, Vec3 c, float r) {
  return length(p - c) - r;
}

static float sdf_torus(Vec3 p, float major, float minor) {
  const float qx = std::sqrt(p.x * p.x + p.z * p.z) - major;
  const float qy = p.y;
  return std::sqrt(qx * qx + qy * qy) - minor;
}

// Smooth minimum (a.k.a. smooth union) as popularized by Inigo Quilez.
static float smooth_min(float a, float b, float k) {
  const float h = clampf(0.5f + 0.5f * (b - a) / std::max(1e-6f, k), 0.0f, 1.0f);
  return lerpf(b, a, h) - k * h * (1.0f - h);
}

// Scene distance field: "wispy" shells built from a smoothed union of spheres,
// torus swirls, and domain-warped noise.
static float scene_sdf(Vec3 p, std::uint32_t seed, float t_anim) {
  // Domain warp: avoids obvious sphere boundaries.
  const float warp_t = t_anim * 0.25f;
  Vec3 q = p;
  const Vec3 wv = fbm_vec3(q * 0.55f + Vec3{0.0f, 0.0f, warp_t}, seed ^ 0x1234ABCDu);
  q = q + wv * 0.35f;

  // A handful of seed-driven blobs.
  float d = 1e9f;
  std::uint32_t h = hash_u32(seed ^ 0xDEADC0DEu);
  const int blobs = 11;
  for (int i = 0; i < blobs; ++i) {
    h = hash_u32(h ^ (0x9E3779B9u * static_cast<std::uint32_t>(i + 1)));
    const float cx = (rand01(h ^ 0xA1u) * 2.0f - 1.0f) * 1.55f;
    const float cy = (rand01(h ^ 0xB2u) * 2.0f - 1.0f) * 1.20f;
    const float cz = 0.35f + rand01(h ^ 0xC3u) * 3.25f;
    const float r = 0.55f + rand01(h ^ 0xD4u) * 1.05f;
    const float di = sdf_sphere(q, Vec3{cx, cy, cz}, r);
    // Soft union.
    d = (i == 0) ? di : smooth_min(d, di, 0.55f);
  }

  // Add a subtle torus swirl layer.
  {
    const float spin = 0.25f * std::sin(t_anim * 0.65f);
    const float cs = std::cos(spin);
    const float sn = std::sin(spin);
    Vec3 qt = q;
    // Rotate in XZ plane.
    const float x = qt.x * cs - qt.z * sn;
    const float z = qt.x * sn + qt.z * cs;
    qt.x = x;
    qt.z = z;
    const float dt = sdf_torus(qt - Vec3{0.0f, 0.15f, 1.65f}, 1.35f, 0.22f);
    d = smooth_min(d, dt, 0.35f);
  }

  // Turn the union into a shell (wisps around the surface).
  d = std::fabs(d) - 0.085f;

  // Surface roughness.
  const float n = fbm_3d(q * 2.15f + Vec3{0.0f, 0.0f, warp_t * 1.4f}, seed ^ 0x77AA55CCu);
  d += n * 0.12f;
  return d;
}

struct Ray {
  Vec3 o;
  Vec3 d;
};

struct RayResult {
  bool hit{false};
  float t{0.0f};
  float min_abs_d{1e9f};
  Vec3 pos{};
  Vec3 min_pos{};
  int steps{0};
};

static RayResult raymarch(const Ray& r, std::uint32_t seed, float t_anim, const RaymarchNebulaStyle& style) {
  RayResult out;
  const int max_steps = std::clamp(style.max_steps, 8, 160);
  const float step_scale = 0.92f;
  const float eps = 0.0045f;
  const float max_dist = 7.0f;

  float t = 0.0f;
  float min_abs = 1e9f;
  Vec3 min_pos = r.o;
  for (int i = 0; i < max_steps; ++i) {
    const Vec3 p = r.o + r.d * t;
    const float d = scene_sdf(p, seed, t_anim);
    const float ad = std::fabs(d);
    if (ad < min_abs) {
      min_abs = ad;
      min_pos = p;
    }

    if (d < eps) {
      out.hit = true;
      out.t = t;
      out.pos = p;
      out.min_abs_d = min_abs;
      out.min_pos = min_pos;
      out.steps = i + 1;
      return out;
    }

    // Sphere tracing. Clamp step for stability.
    const float step = clampf(d * step_scale, 0.012f, 0.38f);
    t += step;
    if (t > max_dist) break;
  }

  out.hit = false;
  out.t = t;
  out.pos = r.o + r.d * t;
  out.min_abs_d = min_abs;
  out.min_pos = min_pos;
  out.steps = max_steps;
  return out;
}

static Vec3 estimate_normal(Vec3 p, std::uint32_t seed, float t_anim) {
  const float e = 0.0035f;
  const float dx = scene_sdf(p + Vec3{e, 0.0f, 0.0f}, seed, t_anim) - scene_sdf(p - Vec3{e, 0.0f, 0.0f}, seed, t_anim);
  const float dy = scene_sdf(p + Vec3{0.0f, e, 0.0f}, seed, t_anim) - scene_sdf(p - Vec3{0.0f, e, 0.0f}, seed, t_anim);
  const float dz = scene_sdf(p + Vec3{0.0f, 0.0f, e}, seed, t_anim) - scene_sdf(p - Vec3{0.0f, 0.0f, e}, seed, t_anim);
  return normalize(Vec3{dx, dy, dz});
}

static float ambient_occlusion(Vec3 p, Vec3 n, std::uint32_t seed, float t_anim) {
  // A tiny AO probe ...
  float occ = 0.0f;
  float sca = 1.0f;
  for (int i = 1; i <= 4; ++i) {
    const float h = 0.07f * static_cast<float>(i);
    const float d = scene_sdf(p + n * h, seed, t_anim);
    occ += (h - d) * sca;
    sca *= 0.72f;
  }
  return clampf(1.0f - occ, 0.0f, 1.0f);
}

static float soft_shadow(Vec3 p, Vec3 ldir, std::uint32_t seed, float t_anim) {
  float res = 1.0f;
  float t = 0.035f;
  for (int i = 0; i < 18; ++i) {
    const float h = scene_sdf(p + ldir * t, seed, t_anim);
    // Penumbra approximation.
    res = std::min(res, 12.0f * h / std::max(1e-4f, t));
    t += clampf(h, 0.02f, 0.25f);
    if (res < 0.02f || t > 2.8f) break;
  }
  return clampf(res, 0.0f, 1.0f);
}

static Color4 shade(const ImVec2& origin,
                    const ImVec2& size,
                    float px,
                    float py,
                    ImVec4 tint,
                    float offset_px_x,
                    float offset_px_y,
                    std::uint32_t seed,
                    const RaymarchNebulaStyle& style,
                    RaymarchNebulaStats* stats) {
  if (stats) {
    stats->shade_calls += 1;
  }

  const float sx = std::max(1.0f, size.x);
  const float sy = std::max(1.0f, size.y);
  const float aspect = sx / sy;

  const float cx = origin.x + sx * 0.5f;
  const float cy = origin.y + sy * 0.5f;

  // NDC in [-1, 1].
  float nx = (px - cx) / (sx * 0.5f);
  float ny = (py - cy) / (sy * 0.5f);
  nx *= aspect;

  // Camera + parallax.
  const float par = clampf(style.parallax, 0.0f, 1.0f);
  Vec3 ro{0.0f, 0.0f, -3.10f};
  ro.x += offset_px_x * par * 0.0024f;
  ro.y += offset_px_y * par * 0.0024f;

  const float t_anim = style.animate ? static_cast<float>(ImGui::GetTime()) * clampf(style.time_scale, 0.0f, 3.0f)
                                     : 0.0f;
  ro.z += std::sin(t_anim * 0.35f) * 0.12f;

  Vec3 rd = normalize(Vec3{nx, ny, 1.55f});

  if (stats) {
    stats->rays_cast += 1;
  }
  RayResult rr = raymarch(Ray{ro, rd}, seed, t_anim, style);
  if (stats) {
    stats->steps_total += rr.steps;
  }

  // Color theme: cool hues with seed-based drift.
  const float h0 = 0.55f + 0.20f * rand01(hash_u32(seed ^ 0x3141592u));
  const float n = fbm_3d(rr.min_pos * 0.80f + Vec3{0.0f, 0.0f, t_anim * 0.18f}, seed ^ 0xCAFEBABEu);
  const float hue = std::fmod(h0 + 0.06f * n + 1.0f, 1.0f);

  float br = 0.0f, bg = 0.0f, bb = 0.0f;
  ImGui::ColorConvertHSVtoRGB(hue, 0.55f, 0.92f, br, bg, bb);

  // Tint with the map background so it "belongs".
  br = lerpf(br, tint.x, 0.18f);
  bg = lerpf(bg, tint.y, 0.18f);
  bb = lerpf(bb, tint.z, 0.18f);

  // Base atmospheric glow around nearest approach.
  const float glow = std::exp(-rr.min_abs_d * 9.0f);
  const float fog = std::exp(-rr.t * 0.35f);

  float a = 0.0f;
  float r = br;
  float g = bg;
  float b = bb;

  if (rr.hit) {
    const Vec3 nrm = estimate_normal(rr.pos, seed, t_anim);
    const Vec3 ldir = normalize(Vec3{0.55f, 0.32f, -0.78f});
    const float diff = std::max(0.0f, dot(nrm, ldir));

    const float ao = ambient_occlusion(rr.pos, nrm, seed, t_anim);
    const float sh = soft_shadow(rr.pos, ldir, seed, t_anim);

    // Fresnel-ish rim for "nebula shell" highlights.
    const float rim = std::pow(1.0f - std::max(0.0f, dot(nrm, Vec3{-rd.x, -rd.y, -rd.z})), 3.0f);
    const float light = (0.22f + 0.85f * diff * sh) * ao + 0.12f * rim;
    r *= light;
    g *= light;
    b *= light;

    // Specular sparkle.
    const Vec3 refl = reflect(Vec3{-ldir.x, -ldir.y, -ldir.z}, nrm);
    const float spec = std::pow(std::max(0.0f, dot(refl, Vec3{-rd.x, -rd.y, -rd.z})), 18.0f);
    r += spec * 0.55f;
    g += spec * 0.60f;
    b += spec * 0.75f;

    // Depth fog back toward background.
    r = lerpf(tint.x, r, fog);
    g = lerpf(tint.y, g, fog);
    b = lerpf(tint.z, b, fog);

    a = style.alpha * clampf(0.18f + 0.70f * glow, 0.0f, 1.0f) * clampf(0.35f + 0.65f * fog, 0.0f, 1.0f);
  } else {
    // No surface hit: keep only a soft halo at nearest approach.
    a = style.alpha * glow * 0.42f;
    const float lift = 0.55f + 0.45f * glow;
    r *= lift;
    g *= lift;
    b *= lift;
    r = lerpf(tint.x, r, 0.55f * fog);
    g = lerpf(tint.y, g, 0.55f * fog);
    b = lerpf(tint.z, b, 0.55f * fog);
  }

  // Guard rails: keep the effect subtle.
  a = clampf(a, 0.0f, 0.75f);
  return clamp01(Color4{r, g, b, a});
}

struct QuadNode {
  float x0{0.0f};
  float y0{0.0f};
  float x1{0.0f};
  float y1{0.0f};
  int depth{0};
};

static void format_stats(std::string& out, const RaymarchNebulaStats& s) {
  out.clear();
  out.reserve(128);
  out += "Raymarch Nebula";
  out += "\nquads: " + std::to_string(s.quads_drawn);
  out += "  split: " + std::to_string(s.nodes_split);
  out += "\nshade: " + std::to_string(s.shade_calls);
  out += "  rays: " + std::to_string(s.rays_cast);
  out += "  steps: " + std::to_string(s.steps_total);
  out += "\nmax depth: " + std::to_string(s.max_depth_reached);
}

}  // namespace

void draw_raymarched_nebula(ImDrawList* draw,
                            const ImVec2& origin,
                            const ImVec2& size,
                            ImU32 bg_tint,
                            float offset_px_x,
                            float offset_px_y,
                            std::uint32_t seed,
                            const RaymarchNebulaStyle& style,
                            RaymarchNebulaStats* out_stats) {
  if (!draw) return;
  if (!style.enabled) return;
  if (size.x <= 2.0f || size.y <= 2.0f) return;
  if (style.alpha <= 0.0f) return;

  RaymarchNebulaStats stats_local;
  RaymarchNebulaStats* stats = out_stats ? out_stats : &stats_local;
  *stats = RaymarchNebulaStats{};

  const ImVec2 clip_p1(origin.x + size.x, origin.y + size.y);
  ImGui::PushClipRect(origin, clip_p1, true);

  const ImVec4 tint = ImGui::ColorConvertU32ToFloat4(bg_tint);

  const int max_depth = std::clamp(style.max_depth, 0, 10);
  const float err_th = clampf(style.error_threshold, 0.0f, 0.50f);
  const int spp = std::clamp(style.spp, 1, 8);

  std::vector<QuadNode> stack;
  stack.reserve(4096);
  stack.push_back(QuadNode{origin.x, origin.y, origin.x + size.x, origin.y + size.y, 0});

  while (!stack.empty()) {
    QuadNode n = stack.back();
    stack.pop_back();
    stats->max_depth_reached = std::max(stats->max_depth_reached, n.depth);

    const float w = n.x1 - n.x0;
    const float h = n.y1 - n.y0;
    if (w <= 0.5f || h <= 0.5f) continue;

    // Evaluate canonical samples.
    // Canonical 5 samples + up to (spp-1) stochastic samples.
    std::array<Color4, 16> samples;
    int ns = 0;
    const float inset = 0.35f;
    const float x0 = n.x0 + inset;
    const float y0 = n.y0 + inset;
    const float x1 = n.x1 - inset;
    const float y1 = n.y1 - inset;
    const float xc = (n.x0 + n.x1) * 0.5f;
    const float yc = (n.y0 + n.y1) * 0.5f;

    auto push_sample = [&](float px, float py, std::uint32_t hseed) {
      // Deterministic micro-jitter (stochastic AA without temporal noise).
      const float jx = (rand01(hash_u32(hseed ^ 0xA1B2C3D4u)) - 0.5f) * 0.8f;
      const float jy = (rand01(hash_u32(hseed ^ 0xB2C3D4E5u)) - 0.5f) * 0.8f;
      samples[ns++] = shade(origin, size, px + jx, py + jy, tint, offset_px_x, offset_px_y, seed, style, stats);
    };

    const std::uint32_t ih = hash_u32(seed ^ (static_cast<std::uint32_t>(static_cast<int>(n.x0)) * 0x27d4eb2du) ^
                                      (static_cast<std::uint32_t>(static_cast<int>(n.y0)) * 0x165667b1u) ^
                                      (static_cast<std::uint32_t>(n.depth) * 0x9e3779b9u));

    push_sample(x0, y0, ih ^ 0x01u);
    push_sample(x1, y0, ih ^ 0x02u);
    push_sample(x0, y1, ih ^ 0x03u);
    push_sample(x1, y1, ih ^ 0x04u);
    push_sample(xc, yc, ih ^ 0x05u);

    // Additional stochastic samples inside the node.
    for (int i = 1; i < spp; ++i) {
      const std::uint32_t hh = hash_u32(ih ^ (0x9e3779b9u * static_cast<std::uint32_t>(i + 7)));
      const float rx = rand01(hh ^ 0xAAAA1111u);
      const float ry = rand01(hh ^ 0x2222BBBBu);
      const float px = lerpf(n.x0, n.x1, rx);
      const float py = lerpf(n.y0, n.y1, ry);
      push_sample(px, py, hh);
    }

    // Average.
    Color4 avg;
    for (int i = 0; i < ns; ++i) avg = avg + samples[i];
    avg = avg * (1.0f / static_cast<float>(std::max(1, ns)));
    avg = clamp01(avg);

    // Error estimate: max deviation in luma/alpha.
    float err = 0.0f;
    const float l0 = luma(avg);
    for (int i = 0; i < ns; ++i) {
      const float dl = std::fabs(luma(samples[i]) - l0);
      const float da = std::fabs(samples[i].a - avg.a);
      err = std::max(err, dl + 0.65f * da);
    }

    const bool small = (w <= 6.0f || h <= 6.0f);
    const bool leaf = (n.depth >= max_depth) || small || (err <= err_th);

    if (!leaf) {
      // Subdivide.
      stats->nodes_split += 1;
      const float mx = (n.x0 + n.x1) * 0.5f;
      const float my = (n.y0 + n.y1) * 0.5f;
      const int d2 = n.depth + 1;
      stack.push_back(QuadNode{n.x0, n.y0, mx, my, d2});
      stack.push_back(QuadNode{mx, n.y0, n.x1, my, d2});
      stack.push_back(QuadNode{n.x0, my, mx, n.y1, d2});
      stack.push_back(QuadNode{mx, my, n.x1, n.y1, d2});
      continue;
    }

    if (avg.a <= 0.0025f) continue;

    const ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(avg.r, avg.g, avg.b, avg.a));
    draw->AddRectFilled(ImVec2(n.x0, n.y0), ImVec2(n.x1, n.y1), col);
    stats->quads_drawn += 1;
  }

  if (style.debug_overlay && out_stats) {
    std::string s;
    format_stats(s, *out_stats);
    const ImU32 txt = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    draw->AddText(ImVec2(origin.x + 6.0f, origin.y + 6.0f), txt, s.c_str());
  }

  ImGui::PopClipRect();
}

}  // namespace nebula4x::ui
