#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "nebula4x/util/json.h"

namespace nebula4x {

// Suggest JSON Pointer completions for a partially typed pointer.
//
// This is intended for UI autocompletion ("go to path", watch lists, etc).
// The function inspects the current JSON document and returns *full pointer*
// strings that would be valid children of the typed prefix.
//
// Examples:
//   input "/systems/0/n" -> "/systems/0/name"
//   input "/" -> top-level keys ("/ships", "/systems", ...)
//
// Notes:
// - If input does not start with '/', it is treated as if it did.
// - Keys are properly escaped per RFC 6901 ("~" -> "~0", "/" -> "~1").
// - Array indices are suggested as plain numeric tokens.
// - Suggestions are capped by max_suggestions and sorted for determinism.
std::vector<std::string> suggest_json_pointer_completions(const json::Value& doc, std::string_view input,
                                                          int max_suggestions = 32, bool accept_root_slash = true,
                                                          bool case_sensitive = false);

} // namespace nebula4x
