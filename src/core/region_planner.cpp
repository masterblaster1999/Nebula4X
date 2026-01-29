#include "nebula4x/core/region_planner.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "nebula4x/core/simulation.h"

namespace nebula4x {
namespace {

constexpr double kEps = 1e-9;

bool finite_vec2(const Vec2& v) {
  return std::isfinite(v.x) && std::isfinite(v.y);
}

double dist2(const Vec2& a, const Vec2& b) {
  const double dx = a.x - b.x;
  const double dy = a.y - b.y;
  return dx * dx + dy * dy;
}

std::string default_region_name(const std::string& prefix, int idx1) {
  const std::string p = prefix.empty() ? std::string("Region") : prefix;
  return p + " " + std::to_string(idx1);
}

Region neutral_region_template() {
  Region r;
  r.id = kInvalidId;
  r.name.clear();
  r.theme.clear();
  r.center = Vec2{0.0, 0.0};
  r.mineral_richness_mult = 1.0;
  r.volatile_richness_mult = 1.0;
  r.salvage_richness_mult = 1.0;
  r.nebula_bias = 0.0;
  r.pirate_risk = 0.0;
  r.pirate_suppression = 0.0;
  r.ruins_density = 0.0;
  return r;
}

Region averaged_template_from_systems(const Simulation& sim, const std::vector<Id>& sys_ids) {
  // Preserve existing flavor: average current region modifiers of the systems.
  // Systems without a region contribute neutral defaults.
  const GameState& st = sim.state();

  Region out = neutral_region_template();
  if (sys_ids.empty()) return out;

  double sum_m = 0.0, sum_v = 0.0, sum_s = 0.0;
  double sum_nb = 0.0, sum_pr = 0.0, sum_ps = 0.0, sum_rd = 0.0;
  int n = 0;

  for (Id sid : sys_ids) {
    const StarSystem* sys = find_ptr(st.systems, sid);
    if (!sys) continue;
    Region r = neutral_region_template();
    if (sys->region_id != kInvalidId) {
      if (const Region* rr = find_ptr(st.regions, sys->region_id)) {
        r = *rr;
      }
    }
    sum_m += std::isfinite(r.mineral_richness_mult) ? r.mineral_richness_mult : 1.0;
    sum_v += std::isfinite(r.volatile_richness_mult) ? r.volatile_richness_mult : 1.0;
    sum_s += std::isfinite(r.salvage_richness_mult) ? r.salvage_richness_mult : 1.0;
    sum_nb += std::isfinite(r.nebula_bias) ? r.nebula_bias : 0.0;
    sum_pr += std::isfinite(r.pirate_risk) ? r.pirate_risk : 0.0;
    sum_ps += std::isfinite(r.pirate_suppression) ? r.pirate_suppression : 0.0;
    sum_rd += std::isfinite(r.ruins_density) ? r.ruins_density : 0.0;
    ++n;
  }

  if (n <= 0) return out;
  out.mineral_richness_mult = sum_m / (double)n;
  out.volatile_richness_mult = sum_v / (double)n;
  out.salvage_richness_mult = sum_s / (double)n;
  out.nebula_bias = sum_nb / (double)n;
  out.pirate_risk = sum_pr / (double)n;
  out.pirate_suppression = sum_ps / (double)n;
  out.ruins_density = sum_rd / (double)n;
  return out;
}

std::string suggested_theme_from_systems(const Simulation& sim, const std::vector<Id>& sys_ids) {
  const GameState& st = sim.state();
  if (sys_ids.empty()) return {};

  double neb = 0.0;
  int n = 0;
  int colonized = 0;

  std::unordered_set<Id> colonized_systems;
  colonized_systems.reserve(st.colonies.size() * 2 + 8);
  for (const auto& [cid, c] : st.colonies) {
    (void)cid;
    const auto* b = find_ptr(st.bodies, c.body_id);
    if (!b) continue;
    if (b->system_id != kInvalidId) colonized_systems.insert(b->system_id);
  }

  for (Id sid : sys_ids) {
    const StarSystem* sys = find_ptr(st.systems, sid);
    if (!sys) continue;
    neb += std::clamp(sys->nebula_density, 0.0, 1.0);
    ++n;

    // Any colony in the system counts as colonized.
    // StarSystem doesn't store colonies directly, so derive from colony->body->system_id.
    if (colonized_systems.find(sid) != colonized_systems.end()) ++colonized;
  }

  if (n <= 0) return {};
  const double neb_avg = neb / (double)n;
  const double colonized_frac = (n > 0) ? ((double)colonized / (double)n) : 0.0;

  if (neb_avg >= 0.65) return "Nebula Expanse";
  if (neb_avg >= 0.35) return "Mist Belt";
  if (colonized_frac >= 0.55) return "Core Worlds";
  if (colonized_frac >= 0.20) return "Frontier";
  return "Outer Reach";
}

int nearest_center_idx(const Vec2& p, const std::vector<Vec2>& centers) {
  int best = -1;
  double best_d2 = std::numeric_limits<double>::infinity();
  for (int i = 0; i < (int)centers.size(); ++i) {
    const double d2 = dist2(p, centers[(size_t)i]);
    if (d2 + 1e-12 < best_d2) {
      best_d2 = d2;
      best = i;
    }
  }
  return best;
}

}  // namespace

RegionPlannerResult compute_region_partition_plan(const Simulation& sim, const RegionPlannerOptions& opt) {
  RegionPlannerResult out;
  out.ok = false;

  const GameState& st = sim.state();

  // Collect eligible systems.
  std::vector<Id> sys_ids;
  sys_ids.reserve(std::min((int)st.systems.size(), std::max(1, opt.max_systems)));

  for (const auto& [sid, sys] : st.systems) {
    if ((int)sys_ids.size() >= std::max(1, opt.max_systems)) break;
    if (sid == kInvalidId) continue;
    if (!finite_vec2(sys.galaxy_pos)) continue;

    if (opt.only_unassigned_systems && sys.region_id != kInvalidId) continue;

    if (opt.restrict_to_discovered && opt.viewer_faction_id != kInvalidId) {
      if (!sim.is_system_discovered_by_faction(opt.viewer_faction_id, sid)) continue;
    }

    sys_ids.push_back(sid);
  }

  std::sort(sys_ids.begin(), sys_ids.end());

  out.eligible_systems = (int)sys_ids.size();
  if (sys_ids.empty()) {
    out.message = "No eligible systems.";
    return out;
  }

  int k = std::clamp(opt.k, 1, (int)sys_ids.size());
  out.used_k = k;

  // Build points array.
  std::vector<Vec2> pts;
  pts.reserve(sys_ids.size());
  for (Id sid : sys_ids) {
    const StarSystem* sys = find_ptr(st.systems, sid);
    pts.push_back(sys ? sys->galaxy_pos : Vec2{0.0, 0.0});
  }

  std::mt19937 rng(opt.seed);

  // --- k-means++ initialization ---
  std::vector<Vec2> centers;
  centers.reserve((size_t)k);

  // Pick first center uniformly.
  {
    std::uniform_int_distribution<int> dist(0, (int)pts.size() - 1);
    centers.push_back(pts[(size_t)dist(rng)]);
  }

  // Next centers by distance^2 weighting.
  while ((int)centers.size() < k) {
    std::vector<double> w;
    w.resize(pts.size(), 0.0);
    double sum = 0.0;

    for (std::size_t i = 0; i < pts.size(); ++i) {
      const double d2 = dist2(pts[i], centers[(size_t)nearest_center_idx(pts[i], centers)]);
      const double ww = std::max(0.0, d2);
      w[i] = ww;
      sum += ww;
    }

    if (!(sum > kEps)) {
      // All points coincident; pick uniformly.
      std::uniform_int_distribution<int> dist(0, (int)pts.size() - 1);
      centers.push_back(pts[(size_t)dist(rng)]);
      continue;
    }

    std::uniform_real_distribution<double> dist(0.0, sum);
    const double pick = dist(rng);
    double acc = 0.0;
    std::size_t idx = 0;
    for (; idx < w.size(); ++idx) {
      acc += w[idx];
      if (acc + 1e-12 >= pick) break;
    }
    if (idx >= pts.size()) idx = pts.size() - 1;
    centers.push_back(pts[idx]);
  }

  // --- Refinement ---
  std::vector<int> assign;
  assign.assign((int)pts.size(), -1);

  int iters = 0;
  for (; iters < std::max(1, opt.max_iters); ++iters) {
    bool any_change = false;

    // Assign points.
    for (int i = 0; i < (int)pts.size(); ++i) {
      const int best = nearest_center_idx(pts[(size_t)i], centers);
      if (best != assign[(size_t)i]) {
        assign[(size_t)i] = best;
        any_change = true;
      }
    }

    // Recompute centers.
    std::vector<Vec2> sum;
    std::vector<int> count;
    sum.assign((size_t)k, Vec2{0.0, 0.0});
    count.assign((size_t)k, 0);

    for (int i = 0; i < (int)pts.size(); ++i) {
      const int cidx = assign[(size_t)i];
      if (cidx < 0 || cidx >= k) continue;
      sum[(size_t)cidx] = sum[(size_t)cidx] + pts[(size_t)i];
      count[(size_t)cidx] += 1;
    }

    // Handle empty clusters by stealing the farthest point.
    for (int c = 0; c < k; ++c) {
      if (count[(size_t)c] > 0) continue;

      // Find farthest point from its current center.
      int far_i = -1;
      double far_d2 = -1.0;
      for (int i = 0; i < (int)pts.size(); ++i) {
        const int cur = assign[(size_t)i];
        if (cur < 0 || cur >= k) continue;
        const double d2 = dist2(pts[(size_t)i], centers[(size_t)cur]);
        if (d2 > far_d2) {
          far_d2 = d2;
          far_i = i;
        }
      }

      if (far_i >= 0) {
        assign[(size_t)far_i] = c;
        centers[(size_t)c] = pts[(size_t)far_i];
        sum[(size_t)c] = pts[(size_t)far_i];
        count[(size_t)c] = 1;
        any_change = true;
      }
    }

    for (int c = 0; c < k; ++c) {
      if (count[(size_t)c] <= 0) continue;
      const double inv = 1.0 / (double)count[(size_t)c];
      centers[(size_t)c] = Vec2{sum[(size_t)c].x * inv, sum[(size_t)c].y * inv};
    }

    if (!any_change) break;
  }

  out.iters_run = iters + 1;

  // Build clusters.
  out.clusters.clear();
  out.clusters.resize((size_t)k);

  for (int c = 0; c < k; ++c) {
    RegionClusterPlan cp;
    cp.region = neutral_region_template();
    cp.region.center = centers[(size_t)c];
    cp.region.name.clear();
    cp.region.theme.clear();
    out.clusters[(size_t)c] = std::move(cp);
  }

  // Membership + inertia.
  for (int i = 0; i < (int)pts.size(); ++i) {
    const int cidx = assign[(size_t)i];
    if (cidx < 0 || cidx >= k) continue;
    const double d2 = dist2(pts[(size_t)i], centers[(size_t)cidx]);
    out.clusters[(size_t)cidx].system_ids.push_back(sys_ids[(size_t)i]);
    out.clusters[(size_t)cidx].inertia += d2;
    out.total_inertia += d2;
  }

  // Deterministic ordering: sort system ids within each cluster.
  for (int c = 0; c < k; ++c) {
    auto& ids = out.clusters[(size_t)c].system_ids;
    std::sort(ids.begin(), ids.end());

    // Suggested modifiers (average of existing regions).
    Region templ = averaged_template_from_systems(sim, ids);
    templ.center = centers[(size_t)c];
    if (templ.name.empty()) templ.name = default_region_name("Region", c + 1);
    if (templ.theme.empty()) templ.theme = suggested_theme_from_systems(sim, ids);
    out.clusters[(size_t)c].region = templ;
  }

  // Deterministic mapping list: sort by system id.
  out.assignment.clear();
  out.assignment.reserve(sys_ids.size());
  for (int i = 0; i < (int)sys_ids.size(); ++i) {
    const int cidx = assign[(size_t)i];
    out.assignment.push_back({sys_ids[(size_t)i], cidx});
  }
  std::sort(out.assignment.begin(), out.assignment.end(), [](const auto& a, const auto& b) {
    if (a.first != b.first) return a.first < b.first;
    return a.second < b.second;
  });

  out.ok = true;
  {
    std::ostringstream ss;
    ss << "Planned " << k << " region(s) for " << sys_ids.size() << " system(s).";
    out.message = ss.str();
  }
  return out;
}

static double clamp01(double v) {
  if (!std::isfinite(v)) return 0.0;
  if (v < 0.0) return 0.0;
  if (v > 1.0) return 1.0;
  return v;
}

static double clamp_pos(double v, double fallback) {
  if (!std::isfinite(v)) return fallback;
  if (v < 0.0) return 0.0;
  return v;
}

static void sanitize_region(Region& r) {
  if (!finite_vec2(r.center)) r.center = Vec2{0.0, 0.0};
  r.mineral_richness_mult = clamp_pos(r.mineral_richness_mult, 1.0);
  r.volatile_richness_mult = clamp_pos(r.volatile_richness_mult, 1.0);
  r.salvage_richness_mult = clamp_pos(r.salvage_richness_mult, 1.0);
  if (!std::isfinite(r.nebula_bias)) r.nebula_bias = 0.0;
  r.nebula_bias = std::clamp(r.nebula_bias, -1.0, 1.0);
  r.pirate_risk = clamp01(r.pirate_risk);
  r.pirate_suppression = clamp01(r.pirate_suppression);
  r.ruins_density = clamp01(r.ruins_density);
}

bool apply_region_partition_plan(GameState& s, const RegionPlannerResult& plan,
                                 const RegionPlannerApplyOptions& opt,
                                 std::string* error) {
  if (!plan.ok) {
    if (error) *error = "Plan is not ok.";
    return false;
  }
  if (plan.clusters.empty()) {
    if (error) *error = "Plan has no clusters.";
    return false;
  }

  // Validate system ids.
  std::unordered_map<Id, int> sys_to_cluster;
  sys_to_cluster.reserve(plan.assignment.size() * 2 + 8);

  for (const auto& [sid, cidx] : plan.assignment) {
    if (sid == kInvalidId) continue;
    if (cidx < 0 || cidx >= (int)plan.clusters.size()) continue;
    if (s.systems.find(sid) == s.systems.end()) continue;
    sys_to_cluster[sid] = cidx;
  }

  if (sys_to_cluster.empty()) {
    if (error) *error = "Plan contains no valid system assignments.";
    return false;
  }

  if (opt.wipe_existing_regions) {
    // Hard reset.
    s.regions.clear();
    for (auto& [sid, sys] : s.systems) {
      (void)sid;
      sys.region_id = kInvalidId;
    }
  } else if (opt.clear_unplanned_system_assignments) {
    for (auto& [sid, sys] : s.systems) {
      (void)sid;
      if (sys_to_cluster.find(sys.id) == sys_to_cluster.end()) {
        sys.region_id = kInvalidId;
      }
    }
  }

  // Create regions for clusters.
  std::vector<Id> new_region_ids;
  new_region_ids.resize(plan.clusters.size(), kInvalidId);

  for (std::size_t i = 0; i < plan.clusters.size(); ++i) {
    Region r = plan.clusters[i].region;
    sanitize_region(r);

    if (r.name.empty()) r.name = default_region_name(opt.name_prefix, (int)i + 1);

    r.id = allocate_id(s);
    new_region_ids[i] = r.id;
    s.regions[r.id] = r;
  }

  // Apply assignments.
  for (const auto& [sid, cidx] : sys_to_cluster) {
    if (sid == kInvalidId) continue;
    if (cidx < 0 || cidx >= (int)new_region_ids.size()) continue;
    auto it = s.systems.find(sid);
    if (it == s.systems.end()) continue;
    it->second.region_id = new_region_ids[(std::size_t)cidx];
  }

  return true;
}

}  // namespace nebula4x
