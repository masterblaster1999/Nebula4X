#include "nebula4x/util/save_diff.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <vector>

#include "nebula4x/util/json.h"

namespace nebula4x {
namespace {

struct Change {
  std::string op;   // add|remove|replace
  std::string path; // JSON-pointer-like
  json::Value before;
  json::Value after;
};

std::string escape_path_token(const std::string& t) {
  // JSON Pointer escaping (RFC 6901):
  //  ~ -> ~0
  //  / -> ~1
  std::string out;
  out.reserve(t.size());
  for (char c : t) {
    if (c == '~') {
      out += "~0";
    } else if (c == '/') {
      out += "~1";
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::string join_path(const std::string& base, const std::string& token) {
  const std::string esc = escape_path_token(token);
  if (base.empty() || base == "/") return "/" + esc;
  return base + "/" + esc;
}

std::string join_index(const std::string& base, std::size_t idx) {
  const std::string token = std::to_string(idx);
  if (base.empty() || base == "/") return "/" + token;
  return base + "/" + token;
}

bool nearly_equal(double a, double b) {
  return std::fabs(a - b) < 1e-9;
}

bool values_equal(const json::Value& a, const json::Value& b) {
  if (a.index() != b.index()) return false;
  if (a.is_null()) return true;
  if (a.is_bool()) return *a.as_bool() == *b.as_bool();
  if (a.is_number()) return nearly_equal(*a.as_number(), *b.as_number());
  if (a.is_string()) return *a.as_string() == *b.as_string();
  if (a.is_array()) {
    const auto& aa = a.array();
    const auto& bb = b.array();
    if (aa.size() != bb.size()) return false;
    for (std::size_t i = 0; i < aa.size(); ++i) {
      if (!values_equal(aa[i], bb[i])) return false;
    }
    return true;
  }
  // object
  const auto& ao = a.object();
  const auto& bo = b.object();
  if (ao.size() != bo.size()) return false;
  // Deterministic compare: sort keys.
  std::vector<std::string> keys;
  keys.reserve(ao.size());
  for (const auto& [k, _] : ao) keys.push_back(k);
  std::sort(keys.begin(), keys.end());
  for (const auto& k : keys) {
    const auto itb = bo.find(k);
    if (itb == bo.end()) return false;
    const auto ita = ao.find(k);
    if (ita == ao.end()) return false;
    if (!values_equal(ita->second, itb->second)) return false;
  }
  return true;
}

void push_change(std::vector<Change>& out, bool& truncated, const SaveDiffOptions& opt, Change c) {
  if (static_cast<int>(out.size()) >= opt.max_changes) {
    truncated = true;
    return;
  }
  // Normalize root path.
  if (c.path.empty()) c.path = "/";
  out.push_back(std::move(c));
}

void diff_impl(const json::Value& a, const json::Value& b, const std::string& path, std::vector<Change>& out,
               bool& truncated, const SaveDiffOptions& opt) {
  if (truncated) return;

  if (values_equal(a, b)) return;

  // Type mismatch or scalar replacement.
  const bool a_scalar = a.is_null() || a.is_bool() || a.is_number() || a.is_string();
  const bool b_scalar = b.is_null() || b.is_bool() || b.is_number() || b.is_string();

  if (a.index() != b.index() || (a_scalar && b_scalar)) {
    push_change(out, truncated, opt, Change{"replace", path, a, b});
    return;
  }

  if (a.is_array() && b.is_array()) {
    const auto& aa = a.array();
    const auto& bb = b.array();
    const std::size_t n = std::min(aa.size(), bb.size());

    for (std::size_t i = 0; i < n; ++i) {
      diff_impl(aa[i], bb[i], join_index(path, i), out, truncated, opt);
      if (truncated) return;
    }

    if (aa.size() > bb.size()) {
      for (std::size_t i = bb.size(); i < aa.size(); ++i) {
        push_change(out, truncated, opt, Change{"remove", join_index(path, i), aa[i], nullptr});
        if (truncated) return;
      }
    } else if (bb.size() > aa.size()) {
      for (std::size_t i = aa.size(); i < bb.size(); ++i) {
        push_change(out, truncated, opt, Change{"add", join_index(path, i), nullptr, bb[i]});
        if (truncated) return;
      }
    }
    return;
  }

  if (a.is_object() && b.is_object()) {
    const auto& ao = a.object();
    const auto& bo = b.object();

    std::vector<std::string> keys;
    keys.reserve(ao.size() + bo.size());
    for (const auto& [k, _] : ao) keys.push_back(k);
    for (const auto& [k, _] : bo) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

    for (const auto& k : keys) {
      const auto ita = ao.find(k);
      const auto itb = bo.find(k);
      const bool in_a = (ita != ao.end());
      const bool in_b = (itb != bo.end());

      if (!in_a && in_b) {
        push_change(out, truncated, opt, Change{"add", join_path(path, k), nullptr, itb->second});
      } else if (in_a && !in_b) {
        push_change(out, truncated, opt, Change{"remove", join_path(path, k), ita->second, nullptr});
      } else if (in_a && in_b) {
        diff_impl(ita->second, itb->second, join_path(path, k), out, truncated, opt);
      }

      if (truncated) return;
    }
    return;
  }

  // Fallback: replace (should be rare).
  push_change(out, truncated, opt, Change{"replace", path, a, b});
}

std::string shorten_value(const std::string& s, int max_chars) {
  if (max_chars <= 0) return s;
  if (static_cast<int>(s.size()) <= max_chars) return s;
  if (max_chars <= 3) return s.substr(0, static_cast<std::size_t>(max_chars));
  return s.substr(0, static_cast<std::size_t>(max_chars - 3)) + "...";
}

std::string value_one_line(const json::Value& v, int max_chars) {
  std::string s = json::stringify(v, 0);
  // Compact spaces a bit (indent==0 already avoids newlines, but keeps ":" without space).
  return shorten_value(s, max_chars);
}

} // namespace

std::string diff_saves_to_text(const std::string& a_json, const std::string& b_json, SaveDiffOptions opt) {
  json::Value a = json::parse(a_json);
  json::Value b = json::parse(b_json);

  std::vector<Change> changes;
  changes.reserve(static_cast<std::size_t>(std::max(0, opt.max_changes)));
  bool truncated = false;

  diff_impl(a, b, "", changes, truncated, opt);

  std::ostringstream out;
  out << "Save diff: " << changes.size() << " change" << (changes.size() == 1 ? "" : "s");
  if (truncated) out << " (truncated to " << opt.max_changes << ")";
  out << "\n";

  for (const auto& c : changes) {
    if (c.op == "add") {
      out << "+ " << c.path << " = " << value_one_line(c.after, opt.max_value_chars) << "\n";
    } else if (c.op == "remove") {
      out << "- " << c.path << " = " << value_one_line(c.before, opt.max_value_chars) << "\n";
    } else {
      out << "~ " << c.path << ": " << value_one_line(c.before, opt.max_value_chars)
          << " -> " << value_one_line(c.after, opt.max_value_chars) << "\n";
    }
  }

  return out.str();
}

std::string diff_saves_to_json(const std::string& a_json, const std::string& b_json, SaveDiffOptions opt) {
  json::Value a = json::parse(a_json);
  json::Value b = json::parse(b_json);

  std::vector<Change> changes;
  changes.reserve(static_cast<std::size_t>(std::max(0, opt.max_changes)));
  bool truncated = false;

  diff_impl(a, b, "", changes, truncated, opt);

  json::Object report;
  report["changes_total"] = static_cast<double>(changes.size());
  report["changes_shown"] = static_cast<double>(changes.size());
  report["truncated"] = truncated;

  json::Array arr;
  arr.reserve(changes.size());
  for (const auto& c : changes) {
    json::Object o;
    o["op"] = c.op;
    o["path"] = c.path.empty() ? std::string("/") : c.path;
    o["before"] = c.before;
    o["after"] = c.after;
    arr.emplace_back(json::object(std::move(o)));
  }
  report["changes"] = json::array(std::move(arr));

  std::string s = json::stringify(json::object(std::move(report)), 2);
  if (s.empty() || s.back() != '\n') s.push_back('\n');
  return s;
}

} // namespace nebula4x
