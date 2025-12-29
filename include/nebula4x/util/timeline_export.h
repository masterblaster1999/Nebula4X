#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "nebula4x/core/game_state.h"
#include "nebula4x/util/digest.h"

namespace nebula4x {

// Options for timeline snapshot/export helpers.
//
// The timeline exporter is intended as a lightweight analytics/telemetry tool
// for balancing and debugging (especially in headless CLI runs).
struct TimelineExportOptions {
  // Digest settings for the per-snapshot state digest.
  DigestOptions digest;

  // Include per-faction colony mineral totals.
  bool include_minerals{true};

  // Include per-faction ship cargo totals.
  bool include_ship_cargo{false};

  // If non-empty, only include these mineral keys in the minerals/cargo maps.
  //
  // This is useful to keep timeline outputs small when a save contains many
  // different minerals.
  std::vector<std::string> mineral_filter;
};

struct TimelineSnapshot {
  std::int64_t day{0};
  std::string date;

  std::uint64_t state_digest{0};
  std::uint64_t content_digest{0};

  std::uint64_t next_event_seq{0};
  std::size_t events_size{0};
  std::uint64_t new_events{0};
  int new_events_retained{0};
  int new_info{0};
  int new_warn{0};
  int new_error{0};

  int systems{0};
  int bodies{0};
  int jump_points{0};
  int ships{0};
  int colonies{0};
  int fleets{0};

  struct FactionSnapshot {
    Id faction_id{kInvalidId};
    std::string name;
    FactionControl control{FactionControl::Player};

    int ships{0};
    int colonies{0};
    int fleets{0};
    double population_millions{0.0};

    double research_points{0.0};
    std::string active_research_id;
    double active_research_progress{0.0};
    int known_techs{0};

    int discovered_systems{0};
    int contacts{0};

    std::unordered_map<std::string, double> minerals;
    std::unordered_map<std::string, double> ship_cargo;
  };

  std::vector<FactionSnapshot> factions;
};

// Compute a snapshot of basic simulation metrics + stable digests.
//
// prev_next_event_seq is used to compute per-day event deltas:
//   new_events = state.next_event_seq - prev_next_event_seq
//
// For the first snapshot of a run, pass prev_next_event_seq = state.next_event_seq
// to make new_events==0.
TimelineSnapshot compute_timeline_snapshot(const GameState& state,
                                          const ContentDB& content,
                                          std::uint64_t content_digest,
                                          std::uint64_t prev_next_event_seq,
                                          const TimelineExportOptions& opt = {});

// Encode a sequence of snapshots as JSONL/NDJSON.
//
// One JSON object per line; output ends with a trailing newline.
std::string timeline_snapshots_to_jsonl(const std::vector<TimelineSnapshot>& snaps);

} // namespace nebula4x
