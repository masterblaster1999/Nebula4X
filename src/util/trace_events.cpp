#include "nebula4x/util/trace_events.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#include "nebula4x/util/file_io.h"
#include "nebula4x/util/json.h"

namespace nebula4x::trace {
namespace {

using Clock = std::chrono::steady_clock;

std::int64_t to_ns(Clock::time_point tp) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
}

std::string ph_to_string(char ph) { return std::string(1, ph); }

} // namespace

struct TraceRecorder::Impl {
  std::mutex mu;
  // Metadata events (process/thread naming) are retained indefinitely.
  std::vector<TraceEvent> meta;
  // Data events (ph == 'X') are retained up to max_events.
  std::deque<TraceEvent> events;

  std::size_t max_events{20000};

  std::unordered_map<std::thread::id, std::uint32_t> thread_ids;
  std::uint32_t next_tid{0};

  std::atomic<bool> enabled{false};
  std::atomic<std::int64_t> start_ns{0};

  void reset_unlocked(std::string_view process_name) {
    meta.clear();
    events.clear();
    thread_ids.clear();
    next_tid = 0;

    // Stable main thread id.
    thread_ids[std::this_thread::get_id()] = 0;
    next_tid = 1;

    // Process name metadata.
    TraceEvent proc;
    proc.name = "process_name";
    proc.ph = 'M';
    proc.pid = 0;
    proc.tid = 0;
    proc.ts_us = 0;
    proc.args["name"] = std::string(process_name);
    meta.push_back(std::move(proc));

    // Main thread name metadata.
    TraceEvent thr;
    thr.name = "thread_name";
    thr.ph = 'M';
    thr.pid = 0;
    thr.tid = 0;
    thr.ts_us = 0;
    thr.args["name"] = std::string("main");
    meta.push_back(std::move(thr));
  }

  std::uint32_t thread_id_unlocked() {
    const auto key = std::this_thread::get_id();
    auto it = thread_ids.find(key);
    if (it != thread_ids.end()) return it->second;

    const std::uint32_t tid = next_tid++;
    thread_ids[key] = tid;

    TraceEvent thr;
    thr.name = "thread_name";
    thr.ph = 'M';
    thr.pid = 0;
    thr.tid = tid;
    thr.ts_us = 0;
    thr.args["name"] = std::string("thread_") + std::to_string(tid);
    meta.push_back(std::move(thr));

    return tid;
  }
};

TraceRecorder& TraceRecorder::instance() {
  static TraceRecorder rec;
  return rec;
}

TraceRecorder::TraceRecorder() : impl_(std::make_unique<Impl>()) {}

TraceRecorder::~TraceRecorder() = default;

void TraceRecorder::set_max_events(std::size_t max_events) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  impl_->max_events = max_events;
  while (impl_->events.size() > impl_->max_events) {
    impl_->events.pop_front();
  }
}

std::size_t TraceRecorder::max_events() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->max_events;
}

std::size_t TraceRecorder::data_event_count() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->events.size();
}

std::size_t TraceRecorder::total_event_count() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->meta.size() + impl_->events.size();
}

void TraceRecorder::start(std::string_view process_name) {
  const std::int64_t ns = to_ns(Clock::now());
  impl_->start_ns.store(ns, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->reset_unlocked(process_name);
  }
  impl_->enabled.store(true, std::memory_order_release);
}

void TraceRecorder::stop() {
  impl_->enabled.store(false, std::memory_order_release);
}

void TraceRecorder::clear() {
  std::lock_guard<std::mutex> lock(impl_->mu);
  impl_->meta.clear();
  impl_->events.clear();
  impl_->thread_ids.clear();
  impl_->next_tid = 0;
}

bool TraceRecorder::enabled() const {
  return impl_->enabled.load(std::memory_order_acquire);
}

std::uint64_t TraceRecorder::now_us() const {
  if (!enabled()) return 0;
  const std::int64_t ns = to_ns(Clock::now());
  const std::int64_t start = impl_->start_ns.load(std::memory_order_relaxed);
  const std::int64_t delta = ns - start;
  if (delta <= 0) return 0;
  return static_cast<std::uint64_t>(delta / 1000);
}

void TraceRecorder::record_complete(std::string_view name, std::string_view cat, std::uint64_t ts_us,
                                   std::uint64_t dur_us, json::Object args) {
  if (!enabled()) return;

  TraceEvent ev;
  ev.name = std::string(name);
  ev.cat = std::string(cat);
  ev.ph = 'X';
  ev.ts_us = ts_us;
  ev.dur_us = dur_us;
  ev.pid = 0;
  ev.args = std::move(args);

  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    ev.tid = impl_->thread_id_unlocked();
    if (impl_->max_events > 0) {
      impl_->events.push_back(std::move(ev));
      while (impl_->events.size() > impl_->max_events) {
        impl_->events.pop_front();
      }
    }
  }
}

json::Value TraceRecorder::to_json() const {
  json::Array arr;
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    arr.reserve(impl_->meta.size() + impl_->events.size());
    for (const auto& e : impl_->meta) {
      json::Object o;
      o["name"] = e.name;
      if (!e.cat.empty()) o["cat"] = e.cat;
      o["ph"] = ph_to_string(e.ph);
      o["ts"] = static_cast<double>(e.ts_us);
      o["pid"] = static_cast<double>(e.pid);
      o["tid"] = static_cast<double>(e.tid);
      if (e.ph == 'X') o["dur"] = static_cast<double>(e.dur_us);
      if (!e.args.empty()) o["args"] = json::object(e.args);
      arr.push_back(json::object(std::move(o)));
    }

    for (const auto& e : impl_->events) {
      json::Object o;
      o["name"] = e.name;
      if (!e.cat.empty()) o["cat"] = e.cat;
      o["ph"] = ph_to_string(e.ph);
      o["ts"] = static_cast<double>(e.ts_us);
      o["pid"] = static_cast<double>(e.pid);
      o["tid"] = static_cast<double>(e.tid);
      if (e.ph == 'X') o["dur"] = static_cast<double>(e.dur_us);
      if (!e.args.empty()) o["args"] = json::object(e.args);
      arr.push_back(json::object(std::move(o)));
    }
  }
  return json::array(std::move(arr));
}

std::string TraceRecorder::to_json_string(int indent) const {
  return json::stringify(to_json(), indent);
}

void TraceRecorder::snapshot(std::vector<TraceEvent>* out, std::size_t max_data_events) const {
  if (!out) return;
  std::lock_guard<std::mutex> lock(impl_->mu);
  out->clear();

  const std::size_t want_data =
      (max_data_events == 0) ? impl_->events.size() : std::min<std::size_t>(max_data_events, impl_->events.size());
  out->reserve(impl_->meta.size() + want_data);

  // Metadata first (process/thread names).
  for (const auto& e : impl_->meta) out->push_back(e);

  if (want_data == impl_->events.size()) {
    for (const auto& e : impl_->events) out->push_back(e);
  } else {
    // Copy the most recent want_data entries.
    const std::size_t skip = impl_->events.size() - want_data;
    std::size_t i = 0;
    for (const auto& e : impl_->events) {
      if (i++ < skip) continue;
      out->push_back(e);
    }
  }
}

Scope::Scope(std::string_view name, std::string_view cat, json::Object args)
    : enabled_(TraceRecorder::instance().enabled()), name_(name), cat_(cat), args_(std::move(args)) {
  if (!enabled_) return;
  start_us_ = TraceRecorder::instance().now_us();
}

Scope::~Scope() {
  if (!enabled_) return;
  try {
    const std::uint64_t end_us = TraceRecorder::instance().now_us();
    const std::uint64_t dur = (end_us >= start_us_) ? (end_us - start_us_) : 0;
    TraceRecorder::instance().record_complete(name_, cat_, start_us_, dur, std::move(args_));
  } catch (...) {
    // Never throw from destructors.
  }
}

Session::Session(std::string out_path, std::string_view process_name, int indent)
    : out_path_(std::move(out_path)), indent_(indent) {
  if (out_path_.empty()) return;
  active_ = true;
  TraceRecorder::instance().start(process_name);
}

Session::~Session() {
  if (!active_) return;
  try {
    TraceRecorder::instance().stop();
    const std::string payload = TraceRecorder::instance().to_json_string(indent_);
    if (out_path_ == "-") {
      if (!payload.empty()) {
        std::fwrite(payload.data(), 1, payload.size(), stdout);
        if (payload.back() != '\n') std::fputc('\n', stdout);
      }
    } else {
      write_text_file(out_path_, payload);
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "Trace write failed: %s\n", e.what());
  } catch (...) {
    std::fprintf(stderr, "Trace write failed: unknown error\n");
  }
}

} // namespace nebula4x::trace
