#include "nebula4x/util/regression_tape.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <stdexcept>
#include <string>
#include <string_view>

#include "nebula4x/util/json.h"

namespace nebula4x {
namespace {

std::string utc_now_iso8601() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const std::time_t t = system_clock::to_time_t(now);

  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif

  // Avoid fixed-size snprintf warnings by relying on strftime, which reports
  // failure instead of truncating.
  std::array<char, 64> buf{};
  if (std::strftime(buf.data(), buf.size(), "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) {
    // Extremely unlikely (would require a very large year), but keep it safe.
    std::array<char, 128> big{};
    if (std::strftime(big.data(), big.size(), "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) {
      return std::string("1970-01-01T00:00:00Z");
    }
    return std::string(big.data());
  }
  return std::string(buf.data());
}

bool is_hex_digit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

std::uint64_t parse_hex64(std::string_view s) {
  // Accept optional 0x prefix.
  if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s.remove_prefix(2);
  if (s.empty()) return 0;

  std::uint64_t v = 0;
  for (char c : s) {
    if (!is_hex_digit(c)) throw std::runtime_error("invalid hex digest");
    v <<= 4;
    if (c >= '0' && c <= '9') v |= static_cast<std::uint64_t>(c - '0');
    else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint64_t>(10 + (c - 'a'));
    else v |= static_cast<std::uint64_t>(10 + (c - 'A'));
  }
  return v;
}

json::Value digest_options_to_json(const DigestOptions& opt) {
  json::Object o;
  o["include_events"] = opt.include_events;
  o["include_ui_state"] = opt.include_ui_state;
  return json::object(std::move(o));
}

DigestOptions digest_options_from_json(const json::Value& v) {
  DigestOptions out;
  if (const auto* o = v.as_object()) {
    if (auto it = o->find("include_events"); it != o->end()) out.include_events = it->second.bool_value(true);
    if (auto it = o->find("include_ui_state"); it != o->end()) out.include_ui_state = it->second.bool_value(true);
  }
  return out;
}

json::Value timeline_opt_to_json(const TimelineExportOptions& opt) {
  json::Object o;
  o["include_minerals"] = opt.include_minerals;
  o["include_ship_cargo"] = opt.include_ship_cargo;

  if (!opt.mineral_filter.empty()) {
    json::Array arr;
    arr.reserve(opt.mineral_filter.size());
    for (const auto& k : opt.mineral_filter) arr.push_back(k);
    o["mineral_filter"] = json::array(std::move(arr));
  }

  o["digest"] = digest_options_to_json(opt.digest);
  return json::object(std::move(o));
}

TimelineExportOptions timeline_opt_from_json(const json::Value& v) {
  TimelineExportOptions out;
  if (const auto* o = v.as_object()) {
    if (auto it = o->find("include_minerals"); it != o->end()) out.include_minerals = it->second.bool_value(true);
    if (auto it = o->find("include_ship_cargo"); it != o->end()) out.include_ship_cargo = it->second.bool_value(false);
    if (auto it = o->find("mineral_filter"); it != o->end() && it->second.is_array()) {
      out.mineral_filter.clear();
      for (const auto& kv : it->second.array()) {
        if (kv.is_string()) out.mineral_filter.push_back(kv.string_value());
      }
    }
    if (auto it = o->find("digest"); it != o->end()) {
      out.digest = digest_options_from_json(it->second);
    }
  }
  return out;
}

json::Value snapshot_to_json(const TimelineSnapshot& s) {
  json::Object root;
  root["day"] = static_cast<double>(s.day);
  root["date"] = s.date;
  root["state_digest"] = digest64_to_hex(s.state_digest);
  root["content_digest"] = digest64_to_hex(s.content_digest);
  root["next_event_seq"] = std::to_string(static_cast<unsigned long long>(s.next_event_seq));
  root["events_size"] = static_cast<double>(s.events_size);
  root["new_events"] = static_cast<double>(s.new_events);
  root["new_events_retained"] = static_cast<double>(s.new_events_retained);
  root["new_info"] = static_cast<double>(s.new_info);
  root["new_warn"] = static_cast<double>(s.new_warn);
  root["new_error"] = static_cast<double>(s.new_error);

  json::Object counts;
  counts["systems"] = static_cast<double>(s.systems);
  counts["bodies"] = static_cast<double>(s.bodies);
  counts["jump_points"] = static_cast<double>(s.jump_points);
  counts["ships"] = static_cast<double>(s.ships);
  counts["colonies"] = static_cast<double>(s.colonies);
  counts["fleets"] = static_cast<double>(s.fleets);
  root["counts"] = json::object(std::move(counts));

  // We intentionally omit per-faction rows here to keep tapes compact.
  return json::object(std::move(root));
}

TimelineSnapshot snapshot_from_json(const json::Value& v) {
  TimelineSnapshot s;
  const auto* o = v.as_object();
  if (!o) throw std::runtime_error("snapshot is not an object");

  if (auto it = o->find("day"); it != o->end()) s.day = it->second.int_value(0);
  if (auto it = o->find("date"); it != o->end()) s.date = it->second.string_value();
  if (auto it = o->find("state_digest"); it != o->end()) {
    s.state_digest = parse_hex64(it->second.string_value());
  }
  if (auto it = o->find("content_digest"); it != o->end()) {
    s.content_digest = parse_hex64(it->second.string_value());
  }
  if (auto it = o->find("next_event_seq"); it != o->end()) {
    // Stored as string to avoid double rounding; accept numbers too.
    if (it->second.is_string()) {
      try {
        s.next_event_seq = static_cast<std::uint64_t>(std::stoull(it->second.string_value()));
      } catch (...) {
        s.next_event_seq = 0;
      }
    } else {
      s.next_event_seq = static_cast<std::uint64_t>(it->second.number_value(0));
    }
  }
  if (auto it = o->find("events_size"); it != o->end()) s.events_size = static_cast<std::size_t>(it->second.number_value(0));
  if (auto it = o->find("new_events"); it != o->end()) s.new_events = static_cast<std::uint64_t>(it->second.number_value(0));
  if (auto it = o->find("new_events_retained"); it != o->end()) s.new_events_retained = static_cast<int>(it->second.number_value(0));
  if (auto it = o->find("new_info"); it != o->end()) s.new_info = static_cast<int>(it->second.number_value(0));
  if (auto it = o->find("new_warn"); it != o->end()) s.new_warn = static_cast<int>(it->second.number_value(0));
  if (auto it = o->find("new_error"); it != o->end()) s.new_error = static_cast<int>(it->second.number_value(0));

  if (auto it = o->find("counts"); it != o->end() && it->second.is_object()) {
    const auto& c = it->second.object();
    if (auto jt = c.find("systems"); jt != c.end()) s.systems = static_cast<int>(jt->second.number_value(0));
    if (auto jt = c.find("bodies"); jt != c.end()) s.bodies = static_cast<int>(jt->second.number_value(0));
    if (auto jt = c.find("jump_points"); jt != c.end()) s.jump_points = static_cast<int>(jt->second.number_value(0));
    if (auto jt = c.find("ships"); jt != c.end()) s.ships = static_cast<int>(jt->second.number_value(0));
    if (auto jt = c.find("colonies"); jt != c.end()) s.colonies = static_cast<int>(jt->second.number_value(0));
    if (auto jt = c.find("fleets"); jt != c.end()) s.fleets = static_cast<int>(jt->second.number_value(0));
  }
  return s;
}

json::Value config_to_json(const RegressionTapeConfig& c) {
  json::Object o;
  o["scenario"] = c.scenario;
  o["seed"] = static_cast<double>(c.seed);
  o["systems"] = static_cast<double>(c.systems);
  o["days"] = static_cast<double>(c.days);
  o["step_days"] = static_cast<double>(c.step_days);
  if (!c.load_path.empty()) o["load"] = c.load_path;

  if (!c.content_paths.empty()) {
    json::Array arr;
    arr.reserve(c.content_paths.size());
    for (const auto& p : c.content_paths) arr.push_back(p);
    o["content"] = json::array(std::move(arr));
  }
  if (!c.tech_paths.empty()) {
    json::Array arr;
    arr.reserve(c.tech_paths.size());
    for (const auto& p : c.tech_paths) arr.push_back(p);
    o["tech"] = json::array(std::move(arr));
  }

  o["timeline"] = timeline_opt_to_json(c.timeline_opt);
  return json::object(std::move(o));
}

RegressionTapeConfig config_from_json(const json::Value& v) {
  RegressionTapeConfig c;
  const auto* o = v.as_object();
  if (!o) throw std::runtime_error("config is not an object");
  if (auto it = o->find("scenario"); it != o->end()) c.scenario = it->second.string_value("sol");
  if (auto it = o->find("seed"); it != o->end()) c.seed = static_cast<std::uint32_t>(it->second.number_value(1));
  if (auto it = o->find("systems"); it != o->end()) c.systems = static_cast<int>(it->second.number_value(12));
  if (auto it = o->find("days"); it != o->end()) c.days = static_cast<int>(it->second.number_value(30));
  if (auto it = o->find("step_days"); it != o->end()) c.step_days = static_cast<int>(it->second.number_value(1));
  if (auto it = o->find("load"); it != o->end()) c.load_path = it->second.string_value();

  if (auto it = o->find("content"); it != o->end() && it->second.is_array()) {
    c.content_paths.clear();
    for (const auto& pv : it->second.array()) {
      if (pv.is_string()) c.content_paths.push_back(pv.string_value());
    }
  }
  if (auto it = o->find("tech"); it != o->end() && it->second.is_array()) {
    c.tech_paths.clear();
    for (const auto& pv : it->second.array()) {
      if (pv.is_string()) c.tech_paths.push_back(pv.string_value());
    }
  }
  if (auto it = o->find("timeline"); it != o->end()) {
    c.timeline_opt = timeline_opt_from_json(it->second);
  }

  return c;
}

} // namespace

std::string regression_tape_to_json(const RegressionTape& tape, int indent) {
  json::Object root;
  root["format"] = "nebula4x.regression_tape.v1";
  root["created_utc"] = tape.created_utc.empty() ? utc_now_iso8601() : tape.created_utc;
  if (!tape.nebula4x_version.empty()) root["nebula4x_version"] = tape.nebula4x_version;
  root["config"] = config_to_json(tape.config);

  json::Array snaps;
  snaps.reserve(tape.snapshots.size());
  for (const auto& s : tape.snapshots) snaps.push_back(snapshot_to_json(s));
  root["snapshots"] = json::array(std::move(snaps));

  return json::stringify(json::object(std::move(root)), indent);
}

RegressionTape regression_tape_from_json(const std::string& json_text) {
  const json::Value root_v = json::parse(json_text);
  const auto* root = root_v.as_object();
  if (!root) throw std::runtime_error("tape root is not an object");

  const std::string fmt = root_v.at("format").string_value();
  if (fmt != "nebula4x.regression_tape.v1") {
    throw std::runtime_error("unsupported tape format: " + fmt);
  }

  RegressionTape out;
  if (auto it = root->find("created_utc"); it != root->end()) out.created_utc = it->second.string_value();
  if (auto it = root->find("nebula4x_version"); it != root->end()) out.nebula4x_version = it->second.string_value();
  out.config = config_from_json(root_v.at("config"));

  const auto& snaps_v = root_v.at("snapshots");
  if (!snaps_v.is_array()) throw std::runtime_error("tape 'snapshots' is not an array");
  out.snapshots.clear();
  out.snapshots.reserve(snaps_v.array().size());
  for (const auto& sv : snaps_v.array()) {
    out.snapshots.push_back(snapshot_from_json(sv));
  }

  return out;
}

bool regression_snapshots_equal(const TimelineSnapshot& a, const TimelineSnapshot& b, bool compare_metrics) {
  if (a.day != b.day) return false;
  if (a.state_digest != b.state_digest) return false;
  if (!compare_metrics) return true;

  if (a.content_digest != b.content_digest) return false;

  // Compare a small set of debugging metrics that should be stable if the digest matches.
  if (a.systems != b.systems) return false;
  if (a.bodies != b.bodies) return false;
  if (a.jump_points != b.jump_points) return false;
  if (a.ships != b.ships) return false;
  if (a.colonies != b.colonies) return false;
  if (a.fleets != b.fleets) return false;
  if (a.next_event_seq != b.next_event_seq) return false;
  return true;
}

RegressionTapeVerifyReport compare_regression_tapes(const RegressionTape& expected,
                                                    const RegressionTape& actual,
                                                    bool compare_metrics) {
  RegressionTapeVerifyReport out;

  if (expected.snapshots.size() != actual.snapshots.size()) {
    out.ok = false;
    out.message = "snapshot count mismatch";
    out.first_mismatch.index = -1;
    out.first_mismatch.message = "expected " + std::to_string(expected.snapshots.size()) +
                                 ", got " + std::to_string(actual.snapshots.size());
    return out;
  }

  for (std::size_t i = 0; i < expected.snapshots.size(); ++i) {
    const auto& e = expected.snapshots[i];
    const auto& g = actual.snapshots[i];
    if (!regression_snapshots_equal(e, g, compare_metrics)) {
      out.ok = false;
      out.message = "mismatch";
      out.first_mismatch.index = static_cast<int>(i);
      out.first_mismatch.day = e.day;
      out.first_mismatch.date = e.date;
      out.first_mismatch.expected_state_digest = digest64_to_hex(e.state_digest);
      out.first_mismatch.actual_state_digest = digest64_to_hex(g.state_digest);

      std::string msg = "digest mismatch";
      if (e.day != g.day) msg = "day mismatch";
      else if (e.state_digest == g.state_digest && compare_metrics) msg = "metrics mismatch";
      out.first_mismatch.message = msg;
      return out;
    }
  }

  out.ok = true;
  out.message = "ok";
  return out;
}

std::string regression_verify_report_to_json(const RegressionTapeVerifyReport& r, int indent) {
  json::Object o;
  o["ok"] = r.ok;
  o["message"] = r.message;

  if (!r.ok) {
    json::Object mm;
    mm["index"] = static_cast<double>(r.first_mismatch.index);
    mm["day"] = static_cast<double>(r.first_mismatch.day);
    mm["date"] = r.first_mismatch.date;
    mm["expected_state_digest"] = r.first_mismatch.expected_state_digest;
    mm["actual_state_digest"] = r.first_mismatch.actual_state_digest;
    mm["detail"] = r.first_mismatch.message;
    o["first_mismatch"] = json::object(std::move(mm));
  }

  return json::stringify(json::object(std::move(o)), indent);
}

} // namespace nebula4x
