#include "nebula4x/util/json_pointer_autocomplete.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include "nebula4x/util/json_pointer.h"

namespace nebula4x {
namespace {

char to_lower_ascii(char c) {
  return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

bool starts_with(std::string_view s, std::string_view prefix) {
  if (prefix.size() > s.size()) return false;
  return s.substr(0, prefix.size()) == prefix;
}

bool starts_with_ci(std::string_view s, std::string_view prefix) {
  if (prefix.size() > s.size()) return false;
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (to_lower_ascii(s[i]) != to_lower_ascii(prefix[i])) return false;
  }
  return true;
}

// Like json_pointer_unescape_token, but tolerant of incomplete escape sequences
// (useful while the user is typing).
std::string safe_unescape_partial(std::string_view tok) {
  std::string out;
  out.reserve(tok.size());
  for (std::size_t i = 0; i < tok.size(); ++i) {
    const char c = tok[i];
    if (c == '~' && (i + 1) < tok.size()) {
      const char n = tok[i + 1];
      if (n == '0') {
        out.push_back('~');
        ++i;
        continue;
      }
      if (n == '1') {
        out.push_back('/');
        ++i;
        continue;
      }
    }
    out.push_back(c);
  }
  return out;
}

} // namespace

std::vector<std::string> suggest_json_pointer_completions(const json::Value& doc, std::string_view input,
                                                          int max_suggestions, bool accept_root_slash,
                                                          bool case_sensitive) {
  std::vector<std::string> out;
  if (max_suggestions <= 0) return out;

  std::string path(input);
  if (path.empty()) path = "/";

  // Users sometimes omit the leading '/'.
  if (!path.empty() && path[0] != '/') path = "/" + path;

  // Determine prefix pointer and partial token.
  std::string prefix = "/";
  std::string partial_raw;

  const bool is_root = (path == "/");
  const bool ends_with_slash = (!is_root && !path.empty() && path.back() == '/');

  if (ends_with_slash) {
    prefix = path.substr(0, path.size() - 1);
    if (prefix.empty()) prefix = "/";
    partial_raw.clear();
  } else {
    const std::size_t last = path.find_last_of('/');
    if (last == std::string::npos) {
      prefix = "/";
      partial_raw = path;
    } else {
      prefix = (last == 0) ? "/" : path.substr(0, last);
      partial_raw = path.substr(last + 1);
    }
  }

  const std::string partial = safe_unescape_partial(partial_raw);

  std::string err;
  const json::Value* node = resolve_json_pointer(doc, prefix, accept_root_slash, &err);
  if (!node) return out;

  if (node->is_object()) {
    const auto* o = node->as_object();
    if (!o) return out;

    std::vector<std::string> keys;
    keys.reserve(o->size());
    for (const auto& kv : *o) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());

    for (const auto& k : keys) {
      const bool match = partial.empty() ? true
                                         : (case_sensitive ? starts_with(k, partial) : starts_with_ci(k, partial));
      if (!match) continue;

      out.push_back(json_pointer_join(prefix, k));
      if ((int)out.size() >= max_suggestions) break;
    }
    return out;
  }

  if (node->is_array()) {
    const auto* a = node->as_array();
    if (!a) return out;

    const std::size_t n = a->size();
    // Cap scanning effort for very large arrays.
    const std::size_t scan_cap = std::min<std::size_t>(n, static_cast<std::size_t>(max_suggestions) * 200ull);

    for (std::size_t i = 0; i < scan_cap; ++i) {
      const std::string idx = std::to_string(i);
      if (!partial.empty()) {
        const bool match = case_sensitive ? starts_with(idx, partial) : starts_with_ci(idx, partial);
        if (!match) continue;
      }
      out.push_back(json_pointer_join_index(prefix, i));
      if ((int)out.size() >= max_suggestions) break;
    }
    return out;
  }

  // Scalar nodes have no children.
  return out;
}

} // namespace nebula4x
