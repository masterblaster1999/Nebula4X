#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "nebula4x/util/json.h"

namespace nebula4x::trace {

// Minimal Chrome/Perfetto-compatible trace event representation.
//
// We primarily emit "X" (complete) events, plus a few metadata ("M") events
// for process/thread naming.
struct TraceEvent {
  std::string name;
  std::string cat;
  char ph{'X'};

  // Microseconds since trace start.
  std::uint64_t ts_us{0};
  // Duration in microseconds (only used for ph == 'X').
  std::uint64_t dur_us{0};

  std::uint32_t pid{0};
  std::uint32_t tid{0};

  json::Object args;
};

// Thread-safe recorder. Tracing is opt-in and should be enabled by calling start().
class TraceRecorder {
 public:
  static TraceRecorder& instance();

  // Limit the number of recorded data events (ph == 'X') retained in memory.
  // Metadata events (ph == 'M') are always retained.
  //
  // A max_events of 0 disables retention of data events.
  void set_max_events(std::size_t max_events);
  std::size_t max_events() const;

  // Counts.
  std::size_t data_event_count() const;
  std::size_t total_event_count() const;

  // Start a new trace session.
  //
  // This resets any previously recorded events.
  void start(std::string_view process_name = "nebula4x");

  // Stop recording. Recorded events remain available for export.
  void stop();

  // Clear all recorded events and thread mappings.
  void clear();

  bool enabled() const;

  // A timestamp in microseconds since start().
  // Returns 0 if tracing is not enabled.
  std::uint64_t now_us() const;

  // Record a complete event ("X") that starts at ts_us and lasts dur_us.
  void record_complete(std::string_view name, std::string_view cat, std::uint64_t ts_us,
                       std::uint64_t dur_us, json::Object args = {});

  // Emit JSON in the Chrome Trace Event format.
  json::Value to_json() const;
  std::string to_json_string(int indent = 2) const;

  // Snapshot events into an output vector.
  //
  // If max_data_events is non-zero, the snapshot includes only the most recent
  // max_data_events data events (plus all metadata).
  void snapshot(std::vector<TraceEvent>* out, std::size_t max_data_events = 0) const;

 private:
  struct Impl;
  TraceRecorder();
  ~TraceRecorder();
  TraceRecorder(const TraceRecorder&) = delete;
  TraceRecorder& operator=(const TraceRecorder&) = delete;

  std::unique_ptr<Impl> impl_;
};

// RAII scope that records a complete ("X") event.
class Scope {
 public:
  Scope(std::string_view name, std::string_view cat = {}, json::Object args = {});
  ~Scope();

  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  bool enabled_{false};
  std::string name_;
  std::string cat_;
  std::uint64_t start_us_{0};
  json::Object args_;
};

// Convenience helper for CLI/tools: records a trace to a file (or stdout if path is "-")
// for the lifetime of this object.
class Session {
 public:
  explicit Session(std::string out_path, std::string_view process_name = "nebula4x", int indent = 2);
  ~Session();

  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

 private:
  std::string out_path_;
  int indent_{2};
  bool active_{false};
};

} // namespace nebula4x::trace

// --- Convenience macros ---

#define NEBULA4X_TRACE_CONCAT2(a, b) a##b
#define NEBULA4X_TRACE_CONCAT(a, b) NEBULA4X_TRACE_CONCAT2(a, b)

// Record a scoped complete event.
#define NEBULA4X_TRACE_SCOPE(name, cat) \
  ::nebula4x::trace::Scope NEBULA4X_TRACE_CONCAT(_n4x_trace_scope_, __LINE__)((name), (cat))

// Record a scoped complete event with JSON args.
#define NEBULA4X_TRACE_SCOPE_ARGS(name, cat, args_obj) \
  ::nebula4x::trace::Scope NEBULA4X_TRACE_CONCAT(_n4x_trace_scope_, __LINE__)((name), (cat), (args_obj))
