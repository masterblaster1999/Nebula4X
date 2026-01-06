#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "nebula4x/util/digest.h"
#include "nebula4x/util/timeline_export.h"

namespace nebula4x {

// A "regression tape" is a compact, portable record of expected simulation digests
// and a small set of summary metrics captured at specific in-game dates.
//
// This is intended for:
//  - deterministic regression testing ("golden master" style),
//  - performance/balance benchmarking across changes,
//  - quickly bisecting when a change caused a simulation divergence.
//
// Tapes are produced and verified via nebula4x_cli:
//   --make-regression-tape OUT.json
//   --verify-regression-tape TAPE.json
//
// Notes:
//  - Digests are computed with DigestOptions (see digest.h). For determinism tests,
//    you may want to exclude event logs/UI state.
//  - Metrics are best-effort and intended for debugging; digest mismatch is the
//    authoritative failure signal.

struct RegressionTapeConfig {
  std::string scenario{"sol"};
  std::uint32_t seed{1};
  int systems{12};

  // Total simulated days advanced when generating the tape.
  int days{30};

  // Snapshot cadence for tape generation.
  //
  // The generator will always include the initial snapshot (day 0), then advance
  // by step_days between subsequent snapshots.
  int step_days{1};

  // Optional save file to start from instead of a scenario.
  std::string load_path;

  // Content and tech inputs used to create the simulation.
  std::vector<std::string> content_paths;
  std::vector<std::string> tech_paths;

  // Options controlling digest + which metrics are captured.
  TimelineExportOptions timeline_opt;
};

// A regression tape is essentially a config + the expected timeline snapshots.
struct RegressionTape {
  RegressionTapeConfig config;

  // Metadata.
  std::string created_utc;
  std::string nebula4x_version;

  // Expected snapshots (typically at fixed day intervals).
  std::vector<TimelineSnapshot> snapshots;
};

// Serialize a RegressionTape to JSON.
std::string regression_tape_to_json(const RegressionTape& tape, int indent = 2);

// Parse a RegressionTape from JSON.
// Throws std::runtime_error on invalid format.
RegressionTape regression_tape_from_json(const std::string& json_text);

// Compare two timeline snapshots.
//
// By default we compare both digest and a small set of sanity metrics.
// If compare_metrics=false, only digests are compared.
bool regression_snapshots_equal(const TimelineSnapshot& a, const TimelineSnapshot& b, bool compare_metrics = true);

// A compact structured diff used for machine-readable failure reports.
struct RegressionTapeMismatch {
  int index{-1};
  std::int64_t day{0};
  std::string date;
  std::string expected_state_digest;
  std::string actual_state_digest;
  std::string message;
};

struct RegressionTapeVerifyReport {
  bool ok{true};
  std::string message;
  RegressionTapeMismatch first_mismatch;
};

// Compare expected vs actual tapes.
RegressionTapeVerifyReport compare_regression_tapes(const RegressionTape& expected,
                                                    const RegressionTape& actual,
                                                    bool compare_metrics = true);

// Serialize a verification report to JSON (for CI/bots).
std::string regression_verify_report_to_json(const RegressionTapeVerifyReport& r, int indent = 2);

} // namespace nebula4x
