#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "nebula4x/util/json.h"

namespace nebula4x {

// Escape / unescape a single token for use in a JSON Pointer (RFC 6901).
//
// Escaping rules:
//   ~ -> ~0
//   / -> ~1
std::string json_pointer_escape_token(std::string_view token);
std::string json_pointer_unescape_token(std::string_view token);

// Join a base pointer with a child token (object key).
std::string json_pointer_join(const std::string& base, std::string_view token);

// Join a base pointer with an array index.
std::string json_pointer_join_index(const std::string& base, std::size_t idx);

// Split a JSON Pointer into unescaped tokens.
//
// Accepts:
// - "" as the document root.
// - "/a/b" as tokens {"a", "b"}.
// - when accept_root_slash == true, also accepts "/" as the document root.
std::vector<std::string> split_json_pointer(const std::string& path, bool accept_root_slash = false);

// Parse an array index token (non-negative integer). Returns false for invalid
// tokens (including "-").
bool parse_json_pointer_index(std::string_view tok, std::size_t& out);

// Resolve a JSON Pointer against a parsed JSON document.
//
// Returns nullptr on error. If error is non-null, it is filled with a
// human-readable message.
const json::Value* resolve_json_pointer(const json::Value& doc, const std::string& path,
                                       bool accept_root_slash = false, std::string* error = nullptr);
json::Value* resolve_json_pointer(json::Value& doc, const std::string& path,
                                  bool accept_root_slash = false, std::string* error = nullptr);


// --- JSON pointer pattern queries (glob-style) ---
//
// Nebula4X uses JSON Pointer strings per RFC 6901, but many procedural tooling
// surfaces benefit from addressing *sets* of values instead of exactly one.
//
// query_json_pointer_glob extends JSON Pointer syntax with two special tokens:
//   *  matches any object key or array index at a single path segment
//   ** matches zero or more path segments (recursive descent)
//
// These wildcards are only interpreted by the query function; resolve_json_pointer
// retains strict RFC 6901 semantics.
struct JsonPointerQueryMatch {
  // JSON Pointer to the matched value.
  std::string path;
  // Pointer into the input document. Valid as long as the document lives.
  const json::Value* value{nullptr};
};

struct JsonPointerQueryStats {
  int nodes_visited{0};
  int matches{0};
  bool hit_match_limit{false};
  bool hit_node_limit{false};
};

// Query a document with a wildcard pointer pattern. Returns the first max_matches matches.
//
// max_nodes is a safety cap (approximate traversal budget) for recursive patterns like '**'.
std::vector<JsonPointerQueryMatch> query_json_pointer_glob(const json::Value& doc,
                                                          const std::string& pattern,
                                                          bool accept_root_slash = false,
                                                          int max_matches = 1000,
                                                          int max_nodes = 200000,
                                                          JsonPointerQueryStats* stats = nullptr,
                                                          std::string* error = nullptr);

} // namespace nebula4x
