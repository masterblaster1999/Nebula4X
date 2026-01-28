#include "ui/proc_trail_engine.h"

#include <algorithm>
#include <cmath>

namespace nebula4x::ui {
namespace {

constexpr double kSystemPruneIdleDays = 90.0; // UI-only; keep plenty of history across navigation.

double dist_point_to_segment_mkm(const Vec2& p, const Vec2& a, const Vec2& b) {
  const Vec2 ab = b - a;
  const double ab2 = ab.x * ab.x + ab.y * ab.y;
  if (ab2 < 1e-18) {
    return (p - a).length();
  }
  const Vec2 ap = p - a;
  const double t = std::clamp((ap.x * ab.x + ap.y * ab.y) / ab2, 0.0, 1.0);
  const Vec2 proj = a + ab * t;
  return (p - proj).length();
}

} // namespace

void ProcTrailEngine::begin_frame(double now_days) {
  stats_ = ProcTrailStats{};
  last_begin_frame_days_ = now_days;

  // Prune idle systems and expired tracks/points.
  for (auto it = systems_.begin(); it != systems_.end();) {
    SystemTrails& st = it->second;
    stats_.systems += 1;

    const double max_age = std::max(0.01, st.max_age_days);

    auto prune_map = [&](std::unordered_map<Id, TrailTrack>& map) {
      for (auto it2 = map.begin(); it2 != map.end();) {
        TrailTrack& tr = it2->second;
        prune_track(tr, now_days, max_age, &stats_.points_pruned_this_frame);
        const bool expired = (tr.points.empty() || (now_days - tr.last_seen_t_days) > (max_age * 1.25));
        if (expired) {
          stats_.tracks_pruned_this_frame += 1;
          it2 = map.erase(it2);
        } else {
          ++it2;
        }
      }
    };

    prune_map(st.ships);
    prune_map(st.missiles);

    // Drop entire system cache if we haven't touched it in a long while.
    if ((now_days - st.last_used_t_days) > kSystemPruneIdleDays) {
      it = systems_.erase(it);
      continue;
    }
    ++it;
  }

  // Recompute aggregate counts after pruning.
  rebuild_stats();
}

void ProcTrailEngine::rebuild_stats() {
  const int pruned_points = stats_.points_pruned_this_frame;
  const int pruned_tracks = stats_.tracks_pruned_this_frame;

  stats_ = ProcTrailStats{};
  stats_.points_pruned_this_frame = pruned_points;
  stats_.tracks_pruned_this_frame = pruned_tracks;

  for (const auto& [sid, st] : systems_) {
    (void)sid;
    stats_.systems += 1;
    stats_.ship_tracks += static_cast<int>(st.ships.size());
    stats_.missile_tracks += static_cast<int>(st.missiles.size());
    for (const auto& [id, tr] : st.ships) {
      (void)id;
      stats_.points += static_cast<int>(tr.points.size());
    }
    for (const auto& [id, tr] : st.missiles) {
      (void)id;
      stats_.points += static_cast<int>(tr.points.size());
    }
  }
}

void ProcTrailEngine::clear_all() {
  systems_.clear();
  stats_ = ProcTrailStats{};
}

void ProcTrailEngine::clear_system(Id system_id) {
  systems_.erase(system_id);
}

void ProcTrailEngine::prune_track(TrailTrack& tr, double now_days, double max_age_days, int* pruned_points) {
  if (tr.points.empty()) return;
  const double cutoff = now_days - std::max(0.0, max_age_days);
  // Fast path: if the newest point is already too old, drop everything.
  if (tr.points.back().t_days < cutoff) {
    if (pruned_points) *pruned_points += static_cast<int>(tr.points.size());
    tr.points.clear();
    return;
  }
  // Find first index to keep.
  std::size_t keep = 0;
  while (keep < tr.points.size() && tr.points[keep].t_days < cutoff) {
    ++keep;
  }
  if (keep > 0) {
    if (pruned_points) *pruned_points += static_cast<int>(keep);
    tr.points.erase(tr.points.begin(), tr.points.begin() + static_cast<std::ptrdiff_t>(keep));
  }
}

void ProcTrailEngine::compress_tail(TrailTrack& tr, double epsilon_mkm) {
  // Online simplification: if the last 3 points are nearly collinear, drop the middle.
  if (tr.points.size() < 3) return;
  const auto n = tr.points.size();
  const Vec2 a = tr.points[n - 3].pos_mkm;
  const Vec2 b = tr.points[n - 2].pos_mkm;
  const Vec2 c = tr.points[n - 1].pos_mkm;

  const double d = dist_point_to_segment_mkm(b, a, c);
  if (d <= epsilon_mkm) {
    tr.points.erase(tr.points.begin() + static_cast<std::ptrdiff_t>(n - 2));
  }
}

ProcTrailEngine::TrailTrack* ProcTrailEngine::get_track(std::unordered_map<Id, TrailTrack>& map, Id id) {
  auto it = map.find(id);
  if (it == map.end()) {
    it = map.emplace(id, TrailTrack{}).first;
  }
  return &it->second;
}

const ProcTrailEngine::TrailTrack* ProcTrailEngine::find_track(const std::unordered_map<Id, TrailTrack>& map, Id id) {
  auto it = map.find(id);
  if (it == map.end()) return nullptr;
  return &it->second;
}

void ProcTrailEngine::sample_ship(Id system_id,
                                  Id ship_id,
                                  const Vec2& pos_mkm,
                                  double now_days,
                                  double sample_interval_days,
                                  double min_dist_mkm,
                                  double max_age_days) {
  SystemTrails& st = systems_[system_id];
  st.last_used_t_days = now_days;
  st.max_age_days = std::max(0.25, max_age_days);

  TrailTrack& tr = *get_track(st.ships, ship_id);
  tr.last_seen_t_days = now_days;

  if (tr.points.empty()) {
    tr.points.push_back({pos_mkm, now_days});
    tr.last_sample_t_days = now_days;
    return;
  }

  const double dt = now_days - tr.last_sample_t_days;
  if (dt < -1e-9) {
    // Time-travel (loading a save / time machine). Reset the track so it doesn't smear.
    tr.points.clear();
    tr.points.push_back({pos_mkm, now_days});
    tr.last_sample_t_days = now_days;
    return;
  }

  const Vec2 last = tr.points.back().pos_mkm;
  const double dist = (pos_mkm - last).length();

  const double min_dt = std::max(0.0, sample_interval_days);
  const double min_dist = std::max(0.0, min_dist_mkm);

  if (dt < min_dt && dist < min_dist) {
    // Too soon and not far enough.
    prune_track(tr, now_days, st.max_age_days, nullptr);
    return;
  }

  tr.points.push_back({pos_mkm, now_days});
  tr.last_sample_t_days = now_days;

  // Small online compression to reduce redundant points when ships coast in a straight line.
  compress_tail(tr, std::max(1e-9, min_dist * 0.25));

  prune_track(tr, now_days, st.max_age_days, nullptr);
}

void ProcTrailEngine::sample_missile(Id system_id,
                                     Id missile_id,
                                     const Vec2& pos_mkm,
                                     double now_days,
                                     double sample_interval_days,
                                     double min_dist_mkm,
                                     double max_age_days) {
  SystemTrails& st = systems_[system_id];
  st.last_used_t_days = now_days;
  st.max_age_days = std::max(0.25, max_age_days);

  TrailTrack& tr = *get_track(st.missiles, missile_id);
  tr.last_seen_t_days = now_days;

  if (tr.points.empty()) {
    tr.points.push_back({pos_mkm, now_days});
    tr.last_sample_t_days = now_days;
    return;
  }

  const double dt = now_days - tr.last_sample_t_days;
  if (dt < -1e-9) {
    tr.points.clear();
    tr.points.push_back({pos_mkm, now_days});
    tr.last_sample_t_days = now_days;
    return;
  }

  const Vec2 last = tr.points.back().pos_mkm;
  const double dist = (pos_mkm - last).length();

  const double min_dt = std::max(0.0, sample_interval_days);
  const double min_dist = std::max(0.0, min_dist_mkm);

  if (dt < min_dt && dist < min_dist) {
    prune_track(tr, now_days, st.max_age_days, nullptr);
    return;
  }

  tr.points.push_back({pos_mkm, now_days});
  tr.last_sample_t_days = now_days;
  compress_tail(tr, std::max(1e-9, min_dist * 0.25));
  prune_track(tr, now_days, st.max_age_days, nullptr);
}

const ProcTrailEngine::TrailTrack* ProcTrailEngine::ship_track(Id system_id, Id ship_id) const {
  const auto it = systems_.find(system_id);
  if (it == systems_.end()) return nullptr;
  return find_track(it->second.ships, ship_id);
}

const ProcTrailEngine::TrailTrack* ProcTrailEngine::missile_track(Id system_id, Id missile_id) const {
  const auto it = systems_.find(system_id);
  if (it == systems_.end()) return nullptr;
  return find_track(it->second.missiles, missile_id);
}

} // namespace nebula4x::ui
