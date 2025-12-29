#include "nebula4x/util/save_diff.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <system_error>
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

struct PatchOp {
  std::string op;   // add|remove|replace
  std::string path; // RFC 6901 JSON Pointer
  json::Value value;
  bool has_value{false};
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

std::string unescape_path_token(const std::string& t) {
  // JSON Pointer unescaping (RFC 6901):
  //  ~0 -> ~
  //  ~1 -> /
  std::string out;
  out.reserve(t.size());
  for (std::size_t i = 0; i < t.size(); ++i) {
    const char c = t[i];
    if (c != '~') {
      out.push_back(c);
      continue;
    }
    if (i + 1 >= t.size()) throw std::runtime_error("JSON pointer: dangling '~'");
    const char n = t[i + 1];
    if (n == '0') {
      out.push_back('~');
    } else if (n == '1') {
      out.push_back('/');
    } else {
      throw std::runtime_error("JSON pointer: invalid escape '~" + std::string(1, n) + "'");
    }
    ++i;
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

void push_patch_op(std::vector<PatchOp>& out, bool& truncated, const JsonPatchOptions& opt, PatchOp op) {
  if (opt.max_ops > 0 && static_cast<int>(out.size()) >= opt.max_ops) {
    truncated = true;
    return;
  }
  // RFC 6901 root is the empty string. For convenience, normalize "/" -> "".
  if (op.path == "/") op.path.clear();
  out.push_back(std::move(op));
}

void diff_patch_impl(const json::Value& a, const json::Value& b, const std::string& path,
                     std::vector<PatchOp>& out, bool& truncated, const JsonPatchOptions& opt) {
  if (truncated) return;
  if (values_equal(a, b)) return;

  const bool a_scalar = a.is_null() || a.is_bool() || a.is_number() || a.is_string();
  const bool b_scalar = b.is_null() || b.is_bool() || b.is_number() || b.is_string();

  if (a.index() != b.index() || (a_scalar && b_scalar)) {
    PatchOp op;
    op.op = "replace";
    op.path = path;
    op.value = b;
    op.has_value = true;
    push_patch_op(out, truncated, opt, std::move(op));
    return;
  }

  if (a.is_array() && b.is_array()) {
    const auto& aa = a.array();
    const auto& bb = b.array();
    const std::size_t n = std::min(aa.size(), bb.size());

    for (std::size_t i = 0; i < n; ++i) {
      diff_patch_impl(aa[i], bb[i], join_index(path, i), out, truncated, opt);
      if (truncated) return;
    }

    // Important: for sequentially-applied JSON Patches, array removals must be
    // emitted from the end towards the front to keep indices valid.
    if (aa.size() > bb.size()) {
      for (std::size_t i = aa.size(); i-- > bb.size();) {
        PatchOp op;
        op.op = "remove";
        op.path = join_index(path, i);
        op.has_value = false;
        push_patch_op(out, truncated, opt, std::move(op));
        if (truncated) return;
      }
    } else if (bb.size() > aa.size()) {
      for (std::size_t i = aa.size(); i < bb.size(); ++i) {
        PatchOp op;
        op.op = "add";
        op.path = join_index(path, i);
        op.value = bb[i];
        op.has_value = true;
        push_patch_op(out, truncated, opt, std::move(op));
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
        PatchOp op;
        op.op = "add";
        op.path = join_path(path, k);
        op.value = itb->second;
        op.has_value = true;
        push_patch_op(out, truncated, opt, std::move(op));
      } else if (in_a && !in_b) {
        PatchOp op;
        op.op = "remove";
        op.path = join_path(path, k);
        op.has_value = false;
        push_patch_op(out, truncated, opt, std::move(op));
      } else if (in_a && in_b) {
        diff_patch_impl(ita->second, itb->second, join_path(path, k), out, truncated, opt);
      }

      if (truncated) return;
    }
    return;
  }

  // Fallback: replace (should be rare).
  {
    PatchOp op;
    op.op = "replace";
    op.path = path;
    op.value = b;
    op.has_value = true;
    push_patch_op(out, truncated, opt, std::move(op));
  }
}

std::vector<std::string> split_json_pointer(const std::string& path, bool accept_root_slash) {
  // RFC 6901:
  //   ""     => root
  //   "/a/b" => tokens: ["a", "b"]
  if (path.empty()) return {};
  if (accept_root_slash && path == "/") return {};
  if (path.empty() || path[0] != '/') {
    throw std::runtime_error("JSON pointer must be empty or start with '/': " + path);
  }

  std::vector<std::string> out;
  std::string cur;
  for (std::size_t i = 1; i <= path.size(); ++i) {
    const bool at_end = (i == path.size());
    const char c = at_end ? '/' : path[i];
    if (c == '/') {
      out.push_back(unescape_path_token(cur));
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  return out;
}

bool parse_array_index(const std::string& tok, std::size_t& out) {
  if (tok.empty()) return false;
  if (tok[0] == '-') return false;
  unsigned long long v = 0;
  const char* b = tok.data();
  const char* e = tok.data() + tok.size();
  auto res = std::from_chars(b, e, v);
  if (res.ec != std::errc() || res.ptr != e) return false;
  out = static_cast<std::size_t>(v);
  return true;
}

json::Value* pointer_parent(json::Value& doc, const std::vector<std::string>& tokens) {
  json::Value* cur = &doc;
  if (tokens.size() <= 1) return cur;

  for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
    const std::string& t = tokens[i];

    if (cur->is_object()) {
      auto* o = cur->as_object();
      auto it = o->find(t);
      if (it == o->end()) throw std::runtime_error("JSON pointer path not found: /" + t);
      cur = &it->second;
      continue;
    }

    if (cur->is_array()) {
      auto* a = cur->as_array();
      std::size_t idx = 0;
      if (!parse_array_index(t, idx)) throw std::runtime_error("JSON pointer array index invalid: " + t);
      if (idx >= a->size()) throw std::runtime_error("JSON pointer array index out of range: " + t);
      cur = &(*a)[idx];
      continue;
    }

    throw std::runtime_error("JSON pointer traversed into scalar at token: " + t);
  }
  return cur;
}

void apply_single_op(json::Value& doc, const PatchOp& op, const JsonPatchApplyOptions& opt) {
  const auto tokens = split_json_pointer(op.path, opt.accept_root_slash);

  // Root operations.
  if (tokens.empty()) {
    if (op.op == "remove") {
      throw std::runtime_error("JSON Patch: remove at document root is not supported");
    }
    if (op.op == "add" || op.op == "replace") {
      if (!op.has_value) throw std::runtime_error("JSON Patch: missing value for root op");
      doc = op.value;
      return;
    }
    throw std::runtime_error("JSON Patch: unknown op: " + op.op);
  }

  json::Value* parent = pointer_parent(doc, tokens);
  const std::string& last = tokens.back();

  if (parent->is_object()) {
    auto* o = parent->as_object();

    if (op.op == "add") {
      if (!op.has_value) throw std::runtime_error("JSON Patch: add missing value");
      (*o)[last] = op.value;
      return;
    }
    if (op.op == "replace") {
      if (!op.has_value) throw std::runtime_error("JSON Patch: replace missing value");
      auto it = o->find(last);
      if (it == o->end()) throw std::runtime_error("JSON Patch: replace path not found: " + op.path);
      it->second = op.value;
      return;
    }
    if (op.op == "remove") {
      auto it = o->find(last);
      if (it == o->end()) throw std::runtime_error("JSON Patch: remove path not found: " + op.path);
      o->erase(it);
      return;
    }

    throw std::runtime_error("JSON Patch: unknown op: " + op.op);
  }

  if (parent->is_array()) {
    auto* a = parent->as_array();

    if (op.op == "add") {
      if (!op.has_value) throw std::runtime_error("JSON Patch: add missing value");
      if (last == "-") {
        a->push_back(op.value);
        return;
      }
      std::size_t idx = 0;
      if (!parse_array_index(last, idx)) throw std::runtime_error("JSON Patch: invalid array index: " + last);
      if (idx > a->size()) throw std::runtime_error("JSON Patch: add index out of range: " + last);
      a->insert(a->begin() + static_cast<std::ptrdiff_t>(idx), op.value);
      return;
    }

    std::size_t idx = 0;
    if (!parse_array_index(last, idx)) throw std::runtime_error("JSON Patch: invalid array index: " + last);
    if (idx >= a->size()) throw std::runtime_error("JSON Patch: index out of range: " + last);

    if (op.op == "replace") {
      if (!op.has_value) throw std::runtime_error("JSON Patch: replace missing value");
      (*a)[idx] = op.value;
      return;
    }
    if (op.op == "remove") {
      a->erase(a->begin() + static_cast<std::ptrdiff_t>(idx));
      return;
    }

    throw std::runtime_error("JSON Patch: unknown op: " + op.op);
  }

  throw std::runtime_error("JSON Patch: parent is not container for path: " + op.path);
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

std::string diff_saves_to_json_patch(const std::string& a_json, const std::string& b_json, JsonPatchOptions opt) {
  json::Value a = json::parse(a_json);
  json::Value b = json::parse(b_json);

  std::vector<PatchOp> ops;
  ops.reserve(256);
  bool truncated = false;

  diff_patch_impl(a, b, "", ops, truncated, opt);

  if (truncated) {
    throw std::runtime_error(
        "JSON Patch generation truncated (max_ops reached). Increase JsonPatchOptions::max_ops or set it to 0 for unlimited.");
  }

  json::Array arr;
  arr.reserve(ops.size());
  for (const auto& op : ops) {
    json::Object o;
    o["op"] = op.op;
    o["path"] = op.path;
    if (op.has_value) o["value"] = op.value;
    arr.emplace_back(json::object(std::move(o)));
  }

  std::string s = json::stringify(json::array(std::move(arr)), opt.indent);
  if (s.empty() || s.back() != '\n') s.push_back('\n');
  return s;
}

std::string apply_json_patch(const std::string& doc_json, const std::string& patch_json, JsonPatchApplyOptions opt) {
  json::Value doc = json::parse(doc_json);
  json::Value p = json::parse(patch_json);

  if (!p.is_array()) throw std::runtime_error("JSON Patch must be a JSON array");

  std::vector<PatchOp> ops;
  ops.reserve(p.array().size());

  for (const auto& v : p.array()) {
    if (!v.is_object()) throw std::runtime_error("JSON Patch op must be an object");
    const auto& o = v.object();

    auto it_op = o.find("op");
    auto it_path = o.find("path");
    if (it_op == o.end() || !it_op->second.is_string()) throw std::runtime_error("JSON Patch op missing string 'op'");
    if (it_path == o.end() || !it_path->second.is_string()) throw std::runtime_error("JSON Patch op missing string 'path'");

    PatchOp op;
    op.op = it_op->second.string_value();
    op.path = it_path->second.string_value();

    if (op.op == "add" || op.op == "replace") {
      auto it_val = o.find("value");
      if (it_val == o.end()) throw std::runtime_error("JSON Patch op missing 'value': " + op.op);
      op.value = it_val->second;
      op.has_value = true;
    }

    ops.push_back(std::move(op));
  }

  for (const auto& op : ops) {
    apply_single_op(doc, op, opt);
  }

  std::string s = json::stringify(doc, opt.indent);
  if (s.empty() || s.back() != '\n') s.push_back('\n');
  return s;
}

} // namespace nebula4x
