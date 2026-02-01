#pragma once

#include <string>
#include <vector>

#include "nebula4x/core/ids.h"
#include "nebula4x/core/orders.h"
#include "nebula4x/core/serialization.h"

namespace nebula4x {
class Simulation;
}

namespace nebula4x::ui {

// Options for exporting a "portable" order template to JSON.
//
// Portable templates embed human-readable references (system/body/colony names, etc.)
// alongside (or instead of) raw numeric IDs so that templates can be shared between
// different saves where IDs differ.
struct PortableOrderTemplateOptions {
  // Fog-of-war context for export.
  // When fog_of_war is true, portable references are only emitted for entities
  // that are visible to viewer_faction_id (to avoid leaking hidden info).
  Id viewer_faction_id{kInvalidId};
  bool fog_of_war{false};

  // When true, TravelViaJump orders are removed from the exported template.
  // (Useful if you plan to use Smart apply on import.)
  bool strip_travel_via_jump{false};

  // When true, the original numeric IDs are retained under `source_*_id` keys.
  bool include_source_ids{true};
};

// Serialize an order template into a portable JSON format.
//
// The produced JSON is still accepted by the import helper below.
// It is *not* guaranteed to be compatible with older builds that only support
// ID-based template JSON.
std::string serialize_order_template_to_json_portable(const Simulation& sim, const std::string& name,
                                                     const std::vector<Order>& orders,
                                                     const PortableOrderTemplateOptions& opts,
                                                     int indent = 2);

// Parse an order template from JSON, resolving portable reference fields when present.
//
// - Supports legacy v1 templates (ID-based).
// - Supports portable v2 templates emitted by serialize_order_template_to_json_portable.
//
// When fog_of_war is true, reference resolution is restricted to entities that are
// visible to viewer_faction_id (discovered systems, surveyed jump points, detected ships, etc).
bool deserialize_order_template_from_json_portable(const Simulation& sim, Id viewer_faction_id, bool fog_of_war,
                                                  const std::string& json_text, ParsedOrderTemplate* out,
                                                  std::string* error = nullptr);

// --- Interactive/diagnostic import (portable v2) ---

// A single candidate that can satisfy a portable reference (e.g. a matching body).
struct PortableTemplateRefCandidate {
  Id id{kInvalidId};
  std::string label;
};

// An unresolved or ambiguous portable reference within a specific order.
//
// These are produced by start_portable_template_import_session() when the clipboard
// JSON contains *_ref fields that cannot be deterministically resolved.
struct PortableTemplateImportIssue {
  // 0-based index into the template's order list.
  int order_index{-1};
  std::string order_type;

  // The JSON key for the numeric id that must be supplied (e.g. "body_id").
  std::string id_key;
  // The JSON key for the portable reference object (e.g. "body_ref").
  std::string ref_key;

  // Human-readable summary of the reference request.
  std::string ref_summary;

  // Diagnostic message (why resolution failed / what was ambiguous).
  std::string message;

  // Candidate entities in the current save that could satisfy this reference.
  std::vector<PortableTemplateRefCandidate> candidates;

  // UI-controlled selection (index into candidates). -1 means unresolved.
  int selected_candidate{-1};
};

// Holds a parsed template JSON plus any unresolved reference issues.
//
// The JSON is normalized into an object containing an "orders" array.
// Some ids may already be auto-resolved during parsing.
struct PortableTemplateImportSession {
  std::string template_name;
  nebula4x::json::Value root;
  int total_orders{0};
  std::vector<PortableTemplateImportIssue> issues;
};

// Parse and partially resolve a portable template JSON into an interactive session.
//
// This does *not* require that all references be resolvable. Instead, any unresolved
// or ambiguous references are reported via `out_session->issues`.
bool start_portable_template_import_session(const Simulation& sim, Id viewer_faction_id, bool fog_of_war,
                                            const std::string& json_text, PortableTemplateImportSession* out_session,
                                            std::string* error = nullptr);

// Finalize a previously-started portable import session.
//
// All issues must have a selected_candidate (and candidates must be non-empty),
// otherwise this returns false.
bool finalize_portable_template_import_session(const Simulation& sim, PortableTemplateImportSession* session,
                                               ParsedOrderTemplate* out, std::string* error = nullptr);

}  // namespace nebula4x::ui
