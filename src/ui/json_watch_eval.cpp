#include "ui/json_watch_eval.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "nebula4x/util/json_pointer.h"
#include "ui/ui_state.h"

namespace nebula4x::ui {
namespace {

constexpr int kMinQueryMaxMatches = 10;
constexpr int kMaxQueryMaxMatches = 500000;
constexpr int kMinQueryMaxNodes = 100;
constexpr int kMaxQueryMaxNodes = 5000000;

std::string format_number(const double x) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.6g", x);
  return std::string(buf);
}

std::string trim_preview(std::string s, const int max_chars) {
  if (max_chars <= 0) return std::string();
  if ((int)s.size() <= max_chars) return s;
  s.resize(std::max(0, max_chars - 3));
  s += "...";
  return s;
}

// Coerce common JSON types into a numeric value for aggregation.
//
// - number: the number
// - bool: true=1, false=0
// - array: size
// - object: size
// - null/string: not numeric
bool coerce_numeric(const nebula4x::json::Value& v, double& out) {
  if (v.is_number()) {
    out = v.number_value();
    return true;
  }
  if (v.is_bool()) {
    out = v.bool_value() ? 1.0 : 0.0;
    return true;
  }
  if (v.is_array()) {
    const auto* a = v.as_array();
    out = a ? static_cast<double>(a->size()) : 0.0;
    return true;
  }
  if (v.is_object()) {
    const auto* o = v.as_object();
    out = o ? static_cast<double>(o->size()) : 0.0;
    return true;
  }
  return false;
}

JsonWatchEvalResult eval_scalar(const nebula4x::json::Value& v, const int max_preview_chars) {
  JsonWatchEvalResult r;
  r.ok = true;

  if (v.is_null()) {
    r.display = "null";
    return r;
  }

  if (v.is_bool()) {
    const bool b = v.bool_value(false);
    r.numeric = true;
    r.value = b ? 1.0 : 0.0;
    r.display = b ? "true" : "false";
    return r;
  }

  if (v.is_number()) {
    const double d = v.number_value(0.0);
    r.numeric = true;
    r.value = d;
    r.display = format_number(d);
    return r;
  }

  if (v.is_string()) {
    std::string s = v.string_value();
    s = trim_preview(std::move(s), max_preview_chars);
    r.display = '"' + s + '"';
    return r;
  }

  if (v.is_array()) {
    const auto* a = v.as_array();
    const int n = a ? static_cast<int>(a->size()) : 0;
    r.numeric = true;
    r.value = static_cast<double>(n);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "[%d items]", n);
    r.display = buf;
    return r;
  }

  if (v.is_object()) {
    const auto* o = v.as_object();
    const int n = o ? static_cast<int>(o->size()) : 0;
    r.numeric = true;
    r.value = static_cast<double>(n);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "{%d keys}", n);
    r.display = buf;
    return r;
  }

  r.display = "(unknown)";
  return r;
}

JsonWatchEvalResult eval_pointer(const nebula4x::json::Value& root, const std::string& path,
                                const JsonWatchEvalOptions& opts) {
  JsonWatchEvalResult r;
  r.rep_ptr = (path.empty() && opts.accept_root_slash) ? "/" : path;

  std::string err;
  const auto* node = nebula4x::resolve_json_pointer(root, path, opts.accept_root_slash, &err);
  if (!node) {
    r.ok = false;
    r.display = "(missing)";
    r.error = err;
    return r;
  }

  r = eval_scalar(*node, opts.max_preview_chars);
  r.rep_ptr = (path.empty() && opts.accept_root_slash) ? "/" : path;
  return r;
}

JsonWatchEvalResult eval_query(const nebula4x::json::Value& root, const JsonWatchConfig& cfg,
                              const UIState& ui, const JsonWatchEvalOptions& opts) {
  JsonWatchEvalResult r;
  r.is_query = true;
  r.query_op = std::clamp(cfg.query_op, 0, 4);

  nebula4x::JsonPointerQueryStats stats;
  std::string err;

  const int max_matches = std::clamp(ui.watchboard_query_max_matches, kMinQueryMaxMatches, kMaxQueryMaxMatches);
  const int max_nodes = std::clamp(ui.watchboard_query_max_nodes, kMinQueryMaxNodes, kMaxQueryMaxNodes);

  const auto matches = nebula4x::query_json_pointer_glob(root, cfg.path, opts.accept_root_slash, max_matches,
                                                        max_nodes, &stats, &err);

  r.match_count = stats.matches;
  r.nodes_visited = stats.nodes_visited;
  r.hit_match_limit = stats.hit_match_limit;
  r.hit_node_limit = stats.hit_node_limit;

  if (!err.empty()) {
    r.ok = false;
    r.display = "(error)";
    r.error = err;
    r.rep_ptr = "/";
    return r;
  }

  r.ok = true;
  r.rep_ptr = (!matches.empty()) ? matches.front().path : std::string("/");

  // Optional sample list (tooltips/navigation).
  if (opts.collect_samples && opts.max_sample_matches > 0) {
    const int lim = std::min<int>(opts.max_sample_matches, (int)matches.size());
    r.sample_paths.reserve(lim);
    r.sample_previews.reserve(lim);
    for (int i = 0; i < lim; ++i) {
      const auto& m = matches[i];
      r.sample_paths.push_back(m.path);
      if (m.value) {
        std::string prev = eval_scalar(*m.value, opts.max_preview_chars).display;
        prev = trim_preview(std::move(prev), opts.max_preview_chars);
        r.sample_previews.push_back(std::move(prev));
      } else {
        r.sample_previews.push_back("(null)");
      }
    }
  }

  // Numeric scan for aggregates and diagnostics.
  int num_count = 0;
  double sum = 0.0;
  double min_v = std::numeric_limits<double>::infinity();
  double max_v = -std::numeric_limits<double>::infinity();

  for (const auto& m : matches) {
    if (!m.value) continue;
    double x = 0.0;
    if (coerce_numeric(*m.value, x)) {
      ++num_count;
      sum += x;
      min_v = std::min(min_v, x);
      max_v = std::max(max_v, x);
    }
  }

  r.numeric_count = num_count;

  // Aggregate.
  switch (r.query_op) {
    case 0: {  // count
      r.numeric = true;
      r.value = static_cast<double>(r.match_count);
      r.display = std::to_string(r.match_count);
      if (r.hit_match_limit || r.hit_node_limit) r.display += "+";
      return r;
    }
    case 1: {  // sum
      r.numeric = true;
      r.value = sum;
      r.display = format_number(sum);
      return r;
    }
    case 2: {  // avg
      if (num_count <= 0) {
        r.ok = false;
        r.display = "(no numeric)";
        return r;
      }
      const double avg = sum / static_cast<double>(num_count);
      r.numeric = true;
      r.value = avg;
      r.display = format_number(avg);
      return r;
    }
    case 3: {  // min
      if (num_count <= 0) {
        r.ok = false;
        r.display = "(no numeric)";
        return r;
      }
      r.numeric = true;
      r.value = min_v;
      r.display = format_number(min_v);
      return r;
    }
    case 4: {  // max
      if (num_count <= 0) {
        r.ok = false;
        r.display = "(no numeric)";
        return r;
      }
      r.numeric = true;
      r.value = max_v;
      r.display = format_number(max_v);
      return r;
    }
    default:
      break;
  }

  // Fallback.
  r.numeric = true;
  r.value = static_cast<double>(r.match_count);
  r.display = std::to_string(r.match_count);
  if (r.hit_match_limit || r.hit_node_limit) r.display += "+";
  return r;
}

} // namespace

JsonWatchEvalResult eval_json_watch(const nebula4x::json::Value& root, const JsonWatchConfig& cfg,
                                   const UIState& ui, const JsonWatchEvalOptions& opts) {
  if (cfg.is_query) return eval_query(root, cfg, ui, opts);
  return eval_pointer(root, cfg.path, opts);
}

} // namespace nebula4x::ui
