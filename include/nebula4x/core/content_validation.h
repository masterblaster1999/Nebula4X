#pragma once

#include <string>
#include <vector>

#include "nebula4x/core/game_state.h"

namespace nebula4x {

// Richer validation output for content bundle tooling.
enum class ContentIssueSeverity { Error, Warning };

inline const char* to_string(ContentIssueSeverity s) {
  switch (s) {
    case ContentIssueSeverity::Error:
      return "error";
    case ContentIssueSeverity::Warning:
      return "warning";
  }
  return "unknown";
}

struct ContentIssue {
  ContentIssueSeverity severity{ContentIssueSeverity::Error};

  // Optional short, tool-friendly identifier (e.g. "tech.unknown_prereq").
  // May be empty for legacy / free-form messages.
  std::string code;

  // Human-readable message.
  std::string message;

  // Optional "subject" metadata to help UIs/tools group issues.
  // Examples: subject_kind="tech", subject_id="propulsion_1"
  std::string subject_kind;
  std::string subject_id;
};

// Validate a ContentDB for internal consistency.
//
// Returns a list of issues (errors + warnings). An empty list means "valid".
//
// This is meant for:
//  - quick sanity checks in CI/tests,
//  - CLI validation tooling,
//  - UI modding workflows (grouping/filtering).
std::vector<ContentIssue> validate_content_db_detailed(const ContentDB& db);

// Backwards-compatible validator: returns human-readable **errors** only.
// Warnings from validate_content_db_detailed() are intentionally omitted.
std::vector<std::string> validate_content_db(const ContentDB& db);

} // namespace nebula4x
