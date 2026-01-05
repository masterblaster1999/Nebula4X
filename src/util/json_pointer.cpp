#include "nebula4x/util/json_pointer.h"

#include <algorithm>
#include <charconv>
#include <functional>
#include <stdexcept>

namespace nebula4x {

std::string json_pointer_escape_token(std::string_view token) {
  std::string out;
  out.reserve(token.size());
  for (char c : token) {
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

std::string json_pointer_unescape_token(std::string_view token) {
  std::string out;
  out.reserve(token.size());
  for (std::size_t i = 0; i < token.size(); ++i) {
    const char c = token[i];
    if (c != '~') {
      out.push_back(c);
      continue;
    }
    if (i + 1 >= token.size()) throw std::runtime_error("JSON pointer: dangling '~'");
    const char n = token[i + 1];
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

std::string json_pointer_join(const std::string& base, std::string_view token) {
  const std::string esc = json_pointer_escape_token(token);
  if (base.empty() || base == "/") return "/" + esc;
  return base + "/" + esc;
}

std::string json_pointer_join_index(const std::string& base, std::size_t idx) {
  const std::string token = std::to_string(idx);
  if (base.empty() || base == "/") return "/" + token;
  return base + "/" + token;
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
      out.push_back(json_pointer_unescape_token(cur));
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  return out;
}

bool parse_json_pointer_index(std::string_view tok, std::size_t& out) {
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

const json::Value* resolve_json_pointer(const json::Value& doc, const std::string& path, bool accept_root_slash,
                                       std::string* error) {
  try {
    const auto tokens = split_json_pointer(path, accept_root_slash);
    const json::Value* cur = &doc;

    for (const std::string& t : tokens) {
      if (cur->is_object()) {
        const auto* o = cur->as_object();
        if (!o) throw std::runtime_error("JSON pointer: expected object at token: " + t);
        auto it = o->find(t);
        if (it == o->end()) throw std::runtime_error("JSON pointer: key not found: " + t);
        cur = &it->second;
        continue;
      }

      if (cur->is_array()) {
        const auto* a = cur->as_array();
        if (!a) throw std::runtime_error("JSON pointer: expected array at token: " + t);
        std::size_t idx = 0;
        if (!parse_json_pointer_index(t, idx)) throw std::runtime_error("JSON pointer: invalid array index: " + t);
        if (idx >= a->size()) throw std::runtime_error("JSON pointer: array index out of range: " + t);
        cur = &(*a)[idx];
        continue;
      }

      throw std::runtime_error("JSON pointer: traversed into scalar at token: " + t);
    }

    return cur;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return nullptr;
  }
}

json::Value* resolve_json_pointer(json::Value& doc, const std::string& path, bool accept_root_slash,
                                  std::string* error) {
  try {
    const auto tokens = split_json_pointer(path, accept_root_slash);
    json::Value* cur = &doc;

    for (const std::string& t : tokens) {
      if (cur->is_object()) {
        auto* o = cur->as_object();
        if (!o) throw std::runtime_error("JSON pointer: expected object at token: " + t);
        auto it = o->find(t);
        if (it == o->end()) throw std::runtime_error("JSON pointer: key not found: " + t);
        cur = &it->second;
        continue;
      }

      if (cur->is_array()) {
        auto* a = cur->as_array();
        if (!a) throw std::runtime_error("JSON pointer: expected array at token: " + t);
        std::size_t idx = 0;
        if (!parse_json_pointer_index(t, idx)) throw std::runtime_error("JSON pointer: invalid array index: " + t);
        if (idx >= a->size()) throw std::runtime_error("JSON pointer: array index out of range: " + t);
        cur = &(*a)[idx];
        continue;
      }

      throw std::runtime_error("JSON pointer: traversed into scalar at token: " + t);
    }

    return cur;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return nullptr;
  }
}

std::vector<JsonPointerQueryMatch> query_json_pointer_glob(const json::Value& doc, const std::string& pattern,
                                                          bool accept_root_slash, int max_matches, int max_nodes,
                                                          JsonPointerQueryStats* stats, std::string* error) {
  std::vector<JsonPointerQueryMatch> out;
  if (stats) *stats = JsonPointerQueryStats{};
  if (error) error->clear();

  struct Ctx {
    int max_matches = 0;
    int max_nodes = 0;
    int nodes_visited = 0;
    bool hit_match_limit = false;
    bool hit_node_limit = false;
    std::string error;
  } ctx;

  ctx.max_matches = std::max(0, max_matches);
  ctx.max_nodes = std::max(0, max_nodes);

  std::vector<std::string> tokens;
  try {
    tokens = split_json_pointer(pattern, accept_root_slash);
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return out;
  }

  std::string cur_path;
  cur_path.reserve(pattern.size() + 32);

  const auto push_match = [&](const json::Value* v) {
    if (!v) return;
    if (ctx.max_matches > 0 && (int)out.size() >= ctx.max_matches) {
      ctx.hit_match_limit = true;
      return;
    }
    JsonPointerQueryMatch m;
    if (cur_path.empty() && accept_root_slash) {
      m.path = "/";
    } else {
      m.path = cur_path;
    }
    m.value = v;
    out.push_back(std::move(m));
  };

  const auto bump_node_budget = [&]() -> bool {
    // Returns false if the node budget is exhausted.
    if (ctx.max_nodes > 0 && ctx.nodes_visited >= ctx.max_nodes) {
      ctx.hit_node_limit = true;
      return false;
    }
    ctx.nodes_visited++;
    return true;
  };

  std::function<void(const json::Value&, std::size_t)> rec;
  rec = [&](const json::Value& cur, std::size_t ti) {
    if (ctx.max_matches > 0 && (int)out.size() >= ctx.max_matches) {
      ctx.hit_match_limit = true;
      return;
    }
    if (!bump_node_budget()) return;

    if (ti >= tokens.size()) {
      push_match(&cur);
      return;
    }

    const std::string& t = tokens[ti];

    // Recursive descent (globstar).
    if (t == "**") {
      // Option 1: match zero segments.
      rec(cur, ti + 1);
      if (ctx.hit_match_limit || ctx.hit_node_limit) return;

      // Option 2: consume one segment and keep '**' active.
      if (cur.is_object()) {
        const auto* o = cur.as_object();
        if (!o) return;
        for (const auto& [k, v] : *o) {
          const std::size_t old_sz = cur_path.size();
          cur_path.push_back('/');
          cur_path += json_pointer_escape_token(k);
          rec(v, ti);
          cur_path.resize(old_sz);
          if (ctx.hit_match_limit || ctx.hit_node_limit) return;
        }
      } else if (cur.is_array()) {
        const auto* a = cur.as_array();
        if (!a) return;
        for (std::size_t i = 0; i < a->size(); ++i) {
          const std::size_t old_sz = cur_path.size();
          cur_path.push_back('/');
          cur_path += std::to_string(i);
          rec((*a)[i], ti);
          cur_path.resize(old_sz);
          if (ctx.hit_match_limit || ctx.hit_node_limit) return;
        }
      }
      return;
    }

    // Single-segment wildcard.
    if (t == "*") {
      if (cur.is_object()) {
        const auto* o = cur.as_object();
        if (!o) return;
        for (const auto& [k, v] : *o) {
          const std::size_t old_sz = cur_path.size();
          cur_path.push_back('/');
          cur_path += json_pointer_escape_token(k);
          rec(v, ti + 1);
          cur_path.resize(old_sz);
          if (ctx.hit_match_limit || ctx.hit_node_limit) return;
        }
        return;
      }

      if (cur.is_array()) {
        const auto* a = cur.as_array();
        if (!a) return;
        for (std::size_t i = 0; i < a->size(); ++i) {
          const std::size_t old_sz = cur_path.size();
          cur_path.push_back('/');
          cur_path += std::to_string(i);
          rec((*a)[i], ti + 1);
          cur_path.resize(old_sz);
          if (ctx.hit_match_limit || ctx.hit_node_limit) return;
        }
        return;
      }

      // Scalar: no matches.
      return;
    }

    // Regular token.
    if (cur.is_object()) {
      const auto* o = cur.as_object();
      if (!o) return;
      auto it = o->find(t);
      if (it == o->end()) return; // no match

      const std::size_t old_sz = cur_path.size();
      cur_path.push_back('/');
      cur_path += json_pointer_escape_token(t);
      rec(it->second, ti + 1);
      cur_path.resize(old_sz);
      return;
    }

    if (cur.is_array()) {
      const auto* a = cur.as_array();
      if (!a) return;

      std::size_t idx = 0;
      if (!parse_json_pointer_index(t, idx)) {
        ctx.error = "JSON pointer glob: invalid array index token: " + t;
        return;
      }
      if (idx >= a->size()) return; // no match

      const std::size_t old_sz = cur_path.size();
      cur_path.push_back('/');
      cur_path += std::to_string(idx);
      rec((*a)[idx], ti + 1);
      cur_path.resize(old_sz);
      return;
    }

    // Scalar: no match.
  };

  rec(doc, 0);

  if (stats) {
    stats->nodes_visited = ctx.nodes_visited;
    stats->matches = static_cast<int>(out.size());
    stats->hit_match_limit = ctx.hit_match_limit;
    stats->hit_node_limit = ctx.hit_node_limit;
  }
  if (error) {
    *error = ctx.error;
  }
  return out;
}

} // namespace nebula4x
