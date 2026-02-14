#include "ui/raytrace_sensor_heatmap.h"

#include "ui/map_render.h"

#include "nebula4x/core/game_state.h"
#include "nebula4x/core/simulation.h"
#include "nebula4x/util/hash_rng.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace nebula4x::ui {
namespace {

constexpr int kImDrawList16BitVertexLimit = (1 << 16) - 1;
constexpr int kImDrawListVertexSafetyReserve = 2048;

inline bool drawlist_can_add_vertices(const ImDrawList* draw, int vtx_count) {
  if (!draw) return false;
  if (vtx_count <= 0) return true;
  if constexpr (sizeof(ImDrawIdx) > 2) return true;
  const int soft_limit = std::max(0, kImDrawList16BitVertexLimit - kImDrawListVertexSafetyReserve);
  if (draw->VtxBuffer.Size >= soft_limit) return false;
  return draw->VtxBuffer.Size <= (soft_limit - vtx_count);
}

using nebula4x::util::splitmix64;
using nebula4x::util::u01_from_u64;

inline Vec2 to_world(const ImVec2& screen_px,
                     const ImVec2& center_px,
                     double scale_px_per_mkm,
                     double zoom,
                     const Vec2& pan_mkm) {
  const double denom = std::max(1e-12, scale_px_per_mkm * zoom);
  const double x = (screen_px.x - center_px.x) / denom - pan_mkm.x;
  const double y = (screen_px.y - center_px.y) / denom - pan_mkm.y;
  return Vec2{x, y};
}

inline std::uint64_t hash_i64_pair(std::int64_t a, std::int64_t b) {
  // Two's-complement reinterpret is fine here; we only need a stable mixing.
  std::uint64_t x = static_cast<std::uint64_t>(a);
  std::uint64_t y = static_cast<std::uint64_t>(b);
  std::uint64_t h = x ^ (y + 0x9e3779b97f4a7c15ULL + (x << 6) + (x >> 2));
  return splitmix64(h);
}

inline std::uint64_t mix_seed(std::uint64_t a, std::uint64_t b) {
  return splitmix64(a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2)));
}

struct EvalContext {
  const Simulation* sim{nullptr};
  Id system_id{kInvalidId};
  const std::vector<RaytraceSensorSource>* sources{nullptr};
  SensorRaytraceHeatmapSettings settings;
  std::uint64_t seed{0};
  bool env_varies{false};
  SensorRaytraceHeatmapStats* stats{nullptr};
};

double los_factor_at(const EvalContext& ctx,
                     const RaytraceSensorSource& src,
                     std::size_t src_index,
                     const Vec2& world_pos,
                     std::uint64_t point_seed) {
  if (!ctx.sim) return 1.0;
  if (!ctx.env_varies) return 1.0;
  const float w = std::clamp(ctx.settings.los_strength, 0.0f, 1.0f);
  if (w <= 1e-6f) return 1.0;
  if (!(src.env_src_multiplier > 1e-9) || !std::isfinite(src.env_src_multiplier)) return 1.0;

  const Vec2 d = world_pos - src.pos_mkm;
  const double dist_mkm = d.length();
  if (!(dist_mkm > 1e-9) || !std::isfinite(dist_mkm)) return 1.0;

  // Adapt sample count to distance: short rays don't need a lot of samples.
  // The hard cap (settings.los_samples) lets the user trade quality for speed.
  constexpr double kStrideMkm = 260.0; // ~1 sample per 260 mkm
  const int cap = std::clamp(ctx.settings.los_samples, 1, 64);
  const int n = std::clamp(static_cast<int>(std::ceil(dist_mkm / kStrideMkm)), 1, cap);

  double sum_env = 0.0;
  for (int i = 0; i < n; ++i) {
    // Stratified jitter in [0,1), avoiding endpoints to reduce double-counting.
    std::uint64_t s = mix_seed(point_seed, mix_seed(static_cast<std::uint64_t>(src_index), static_cast<std::uint64_t>(i)));
    const double u = u01_from_u64(s);
    double t = (static_cast<double>(i) + u) / static_cast<double>(n);
    t = 0.05 + 0.90 * t;

    const Vec2 p = src.pos_mkm + d * t;
    const double env = ctx.sim->system_sensor_environment_multiplier_at(ctx.system_id, p);
    sum_env += std::isfinite(env) ? env : 1.0;
    if (ctx.stats) ctx.stats->los_env_samples += 1;
  }

  const double avg_env = sum_env / std::max(1, n);
  double ratio = avg_env / src.env_src_multiplier;
  if (!std::isfinite(ratio)) ratio = 1.0;

  // This is purely an attenuation term; do not allow the LOS term to increase
  // range beyond the source-local environment-adjusted baseline.
  ratio = std::clamp(ratio, 0.05, 1.0);

  const double out = (1.0 - static_cast<double>(w)) + static_cast<double>(w) * ratio;
  return std::clamp(out, 0.05, 1.0);
}

float eval_strength_at(const EvalContext& ctx, const Vec2& world_pos, std::uint64_t point_seed) {
  if (!ctx.sources) return 0.0f;
  if (ctx.stats) ctx.stats->point_evals += 1;

  float best = 0.0f;
  for (std::size_t i = 0; i < ctx.sources->size(); ++i) {
    const auto& src = (*ctx.sources)[i];
    if (src.range_mkm <= 1e-9) continue;
    const Vec2 d = world_pos - src.pos_mkm;
    const double dist = d.length();
    if (!std::isfinite(dist)) continue;

    // Apply LOS attenuation as a multiplicative adjustment to the baseline.
    const double los = los_factor_at(ctx, src, i, world_pos, mix_seed(point_seed, i));
    const double eff = src.range_mkm * los;
    if (eff <= 1e-9) continue;
    if (dist >= eff) continue;

    float v = static_cast<float>(1.0 - (dist / eff));
    v *= std::clamp(src.weight, 0.0f, 1.0f);
    if (v > best) best = v;
  }

  if (best <= 0.0f) return 0.0f;
  const float g = std::clamp(ctx.settings.gamma, 0.05f, 4.0f);
  return std::pow(best, g);
}

} // namespace

void draw_raytraced_sensor_heatmap(ImDrawList* draw,
                                  const ImVec2& origin,
                                  const ImVec2& avail,
                                  const ImVec2& center_px,
                                  double scale_px_per_mkm,
                                  double zoom,
                                  const Vec2& pan_mkm,
                                  const Simulation& sim,
                                  Id system_id,
                                  const std::vector<RaytraceSensorSource>& sources,
                                  ImU32 base_col,
                                  float opacity,
                                  std::uint32_t seed,
                                  const SensorRaytraceHeatmapSettings& settings,
                                  SensorRaytraceHeatmapStats* out_stats) {
  if (!draw) return;
  if (opacity <= 0.0f) return;
  if (avail.x <= 1.0f || avail.y <= 1.0f) return;
  if (sources.empty()) return;

  SensorRaytraceHeatmapStats local_stats;
  SensorRaytraceHeatmapStats* stats = out_stats ? out_stats : &local_stats;
  *stats = SensorRaytraceHeatmapStats{};

  // Detect whether the environment field can vary along LOS.
  bool env_varies = false;
  const auto& cfg = sim.cfg();
  const auto& st = sim.state();
  if (const auto* sys = find_ptr(st.systems, system_id)) {
    if (cfg.enable_nebula_microfields && sys->nebula_density > 1e-6) env_varies = true;
    if (cfg.enable_nebula_storms && cfg.enable_nebula_storm_cells && sim.system_has_storm(system_id)) {
      env_varies = true;
    }
  }

  EvalContext ctx;
  ctx.sim = &sim;
  ctx.system_id = system_id;
  ctx.sources = &sources;
  ctx.settings = settings;
  ctx.seed = static_cast<std::uint64_t>(seed);
  ctx.env_varies = env_varies;
  ctx.stats = stats;

  struct Quad {
    float x0{0}, y0{0}, x1{0}, y1{0};
    int depth{0};
  };

  const float x0 = origin.x;
  const float y0 = origin.y;
  const float x1 = origin.x + avail.x;
  const float y1 = origin.y + avail.y;

  const ImVec2 clip_p1(x1, y1);
  ImGui::PushClipRect(origin, clip_p1, true);

  std::vector<Quad> stack;
  stack.reserve(2048);
  stack.push_back(Quad{x0, y0, x1, y1, 0});

  const int max_depth = std::clamp(settings.max_depth, 0, 12);
  const float err_th = std::clamp(settings.error_threshold, 0.0f, 1.0f);
  const int spp = std::clamp(settings.spp, 1, 16);

  while (!stack.empty()) {
    const Quad q = stack.back();
    stack.pop_back();

    stats->quads_tested += 1;

    const float w = q.x1 - q.x0;
    const float h = q.y1 - q.y0;
    const bool tiny = (w <= 2.0f && h <= 2.0f);

    const float mx = (q.x0 + q.x1) * 0.5f;
    const float my = (q.y0 + q.y1) * 0.5f;

    // Evaluate corners + center for an error estimate.
    auto eval_screen = [&](float sx, float sy) -> float {
      const Vec2 wp = to_world(ImVec2(sx, sy), center_px, scale_px_per_mkm, zoom, pan_mkm);
      const std::int64_t qx = static_cast<std::int64_t>(std::floor(wp.x * 4.0));
      const std::int64_t qy = static_cast<std::int64_t>(std::floor(wp.y * 4.0));
      const std::uint64_t ps = mix_seed(ctx.seed, hash_i64_pair(qx, qy));
      return eval_strength_at(ctx, wp, ps);
    };

    const float v00 = eval_screen(q.x0, q.y0);
    const float v10 = eval_screen(q.x1, q.y0);
    const float v01 = eval_screen(q.x0, q.y1);
    const float v11 = eval_screen(q.x1, q.y1);
    const float vc = eval_screen(mx, my);

    const float vmin = std::min({v00, v10, v01, v11, vc});
    const float vmax = std::max({v00, v10, v01, v11, vc});
    const float err = vmax - vmin;

    if (!tiny && q.depth < max_depth && err > err_th) {
      const float hx = mx;
      const float hy = my;
      // Subdivide into 4 children (push in reverse for a nicer traversal).
      stack.push_back(Quad{hx, hy, q.x1, q.y1, q.depth + 1});
      stack.push_back(Quad{q.x0, hy, hx, q.y1, q.depth + 1});
      stack.push_back(Quad{hx, q.y0, q.x1, hy, q.depth + 1});
      stack.push_back(Quad{q.x0, q.y0, hx, hy, q.depth + 1});
      continue;
    }

    // Leaf: average spp stochastic samples within the quad.
    float acc = 0.0f;
    {
      // Use the center sample as the first sample to reduce variance.
      acc += vc;
      std::uint64_t base = mix_seed(ctx.seed, static_cast<std::uint64_t>(q.depth));
      base = mix_seed(base, hash_i64_pair(static_cast<std::int64_t>(std::floor(mx)),
                                          static_cast<std::int64_t>(std::floor(my))));
      for (int i = 1; i < spp; ++i) {
        base = splitmix64(base);
        const double rx = u01_from_u64(base);
        base = splitmix64(base);
        const double ry = u01_from_u64(base);
        const float sx = q.x0 + static_cast<float>(rx) * (q.x1 - q.x0);
        const float sy = q.y0 + static_cast<float>(ry) * (q.y1 - q.y0);
        acc += eval_screen(sx, sy);
      }
    }
    float v = acc / static_cast<float>(spp);

    const float a = std::clamp(opacity * v, 0.0f, 1.0f);
    if (a > 0.001f) {
      if (!drawlist_can_add_vertices(draw, settings.debug ? 12 : 6)) break;
      // Slight overlap avoids cracks from float rounding at high subdivision.
      draw->AddRectFilled(ImVec2(q.x0, q.y0), ImVec2(q.x1 + 0.5f, q.y1 + 0.5f),
                          modulate_alpha(base_col, a));
      if (settings.debug) {
        draw->AddRect(ImVec2(q.x0, q.y0), ImVec2(q.x1, q.y1), modulate_alpha(base_col, 0.12f), 0.0f, 0, 1.0f);
      }
    }

    stats->quads_leaf += 1;
  }

  ImGui::PopClipRect();
}

} // namespace nebula4x::ui
