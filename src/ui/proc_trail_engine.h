#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "nebula4x/core/simulation.h"
#include "nebula4x/core/vec2.h"

namespace nebula4x::ui {

// A small UI-first "procedural" motion-trail engine.
//
// The system map previously rendered only the current position of moving entities.
// This engine records recent world-space positions and provides fast access to
// those samples so the UI can render "motion trails".
//
// Design goals:
// - Deterministic and backend-agnostic (pure CPU data, rendered via ImDrawList)
// - Safe with fog-of-war (the UI decides which entities to feed into the engine)
// - Cheap: built-in pruning and simple collinearity compression

struct ProcTrailStats {
  int systems = 0;
  int ship_tracks = 0;
  int missile_tracks = 0;
  int points = 0;
  int points_pruned_this_frame = 0;
  int tracks_pruned_this_frame = 0;
};

class ProcTrailEngine {
 public:
  struct TrailPoint {
    Vec2 pos_mkm{0.0, 0.0};
    double t_days = 0.0;
  };

  struct TrailTrack {
    std::vector<TrailPoint> points;
    double last_sample_t_days = 0.0;
    double last_seen_t_days = 0.0;
  };

  ProcTrailEngine() = default;
  ~ProcTrailEngine() = default;

  ProcTrailEngine(const ProcTrailEngine&) = delete;
  ProcTrailEngine& operator=(const ProcTrailEngine&) = delete;

  // Per-frame bookkeeping (pruning + stats reset). `now_days` should be
  // sim_time_days(state).
  void begin_frame(double now_days);

  // Recompute aggregate counts (systems/tracks/points) without touching the
  // per-frame prune counters. Useful after sampling.
  void rebuild_stats();

  // Drop *all* cached trails.
  void clear_all();
  // Drop trails for a specific system.
  void clear_system(Id system_id);

  // Record a ship sample. The caller decides visibility (fog-of-war).
  void sample_ship(Id system_id,
                   Id ship_id,
                   const Vec2& pos_mkm,
                   double now_days,
                   double sample_interval_days,
                   double min_dist_mkm,
                   double max_age_days);

  // Record a missile sample (optional). The caller decides visibility (fog-of-war).
  void sample_missile(Id system_id,
                      Id missile_id,
                      const Vec2& pos_mkm,
                      double now_days,
                      double sample_interval_days,
                      double min_dist_mkm,
                      double max_age_days);

  const TrailTrack* ship_track(Id system_id, Id ship_id) const;
  const TrailTrack* missile_track(Id system_id, Id missile_id) const;

  const ProcTrailStats& stats() const { return stats_; }

 private:
  struct SystemTrails {
    std::unordered_map<Id, TrailTrack> ships;
    std::unordered_map<Id, TrailTrack> missiles;

    double last_used_t_days = 0.0;
    double max_age_days = 7.0;
  };

  static void prune_track(TrailTrack& tr, double now_days, double max_age_days, int* pruned_points);
  static void compress_tail(TrailTrack& tr, double epsilon_mkm);

  static TrailTrack* get_track(std::unordered_map<Id, TrailTrack>& map, Id id);
  static const TrailTrack* find_track(const std::unordered_map<Id, TrailTrack>& map, Id id);

  std::unordered_map<Id, SystemTrails> systems_;
  ProcTrailStats stats_{};
  double last_begin_frame_days_ = 0.0;
};

} // namespace nebula4x::ui
