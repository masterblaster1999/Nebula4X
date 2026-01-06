#include <iostream>
#include <string>

#include "nebula4x/util/digest.h"
#include "nebula4x/util/regression_tape.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

namespace {

nebula4x::TimelineSnapshot make_snap(std::int64_t day, std::uint64_t digest, int ships) {
  nebula4x::TimelineSnapshot s;
  s.day = day;
  s.date = "2500-01-01";
  s.state_digest = digest;
  s.content_digest = 0xBEEF;
  s.systems = 1;
  s.bodies = 2;
  s.jump_points = 3;
  s.ships = ships;
  s.colonies = 4;
  s.fleets = 5;
  s.next_event_seq = 42;
  return s;
}

} // namespace

int test_regression_tape() {
  nebula4x::RegressionTape tape;
  tape.created_utc = "2026-01-01T00:00:00Z";
  tape.nebula4x_version = "0.1.0";

  tape.config.scenario = "sol";
  tape.config.seed = 42;
  tape.config.systems = 7;
  tape.config.days = 10;
  tape.config.step_days = 2;
  tape.config.load_path = "saves/test.json";
  tape.config.content_paths = {"data/blueprints/starting_blueprints.json"};
  tape.config.tech_paths = {"data/tech/tech_tree.json"};
  tape.config.timeline_opt.include_minerals = true;
  tape.config.timeline_opt.include_ship_cargo = false;
  tape.config.timeline_opt.mineral_filter = {"Duranium"};
  tape.config.timeline_opt.digest.include_events = false;
  tape.config.timeline_opt.digest.include_ui_state = false;

  tape.snapshots.push_back(make_snap(0, 0x1234, 10));
  tape.snapshots.push_back(make_snap(2, 0x5678, 11));

  // Round-trip JSON.
  const std::string json = nebula4x::regression_tape_to_json(tape, 2);
  const nebula4x::RegressionTape t2 = nebula4x::regression_tape_from_json(json);

  N4X_ASSERT(t2.config.scenario == tape.config.scenario);
  N4X_ASSERT(t2.config.seed == tape.config.seed);
  N4X_ASSERT(t2.config.systems == tape.config.systems);
  N4X_ASSERT(t2.config.days == tape.config.days);
  N4X_ASSERT(t2.config.step_days == tape.config.step_days);
  N4X_ASSERT(t2.config.load_path == tape.config.load_path);
  N4X_ASSERT(t2.config.content_paths.size() == 1);
  N4X_ASSERT(t2.config.tech_paths.size() == 1);
  N4X_ASSERT(t2.config.timeline_opt.mineral_filter.size() == 1);
  N4X_ASSERT(t2.config.timeline_opt.digest.include_events == false);
  N4X_ASSERT(t2.config.timeline_opt.digest.include_ui_state == false);
  N4X_ASSERT(t2.snapshots.size() == 2);
  N4X_ASSERT(t2.snapshots[0].day == 0);
  N4X_ASSERT(t2.snapshots[1].day == 2);
  N4X_ASSERT(t2.snapshots[0].state_digest == 0x1234);
  N4X_ASSERT(t2.snapshots[1].state_digest == 0x5678);

  // Compare tapes.
  {
    const auto rep = nebula4x::compare_regression_tapes(tape, t2, /*compare_metrics=*/true);
    N4X_ASSERT(rep.ok);
  }

  // Digest mismatch.
  {
    nebula4x::RegressionTape bad = t2;
    bad.snapshots[1].state_digest ^= 0x1;

    const auto rep = nebula4x::compare_regression_tapes(tape, bad, /*compare_metrics=*/false);
    N4X_ASSERT(!rep.ok);
    N4X_ASSERT(rep.first_mismatch.index == 1);
    N4X_ASSERT(rep.first_mismatch.day == 2);
    const std::string report_json = nebula4x::regression_verify_report_to_json(rep, 2);
    N4X_ASSERT(!report_json.empty());
  }

  return 0;
}
