#include <algorithm>
#include <cctype>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "nebula4x/core/content_validation.h"
#include "nebula4x/core/date.h"
#include "nebula4x/core/research_planner.h"
#include "nebula4x/core/serialization.h"
#include "nebula4x/core/scenario.h"
#include "nebula4x/core/simulation.h"
#include "nebula4x/core/state_validation.h"
#include "nebula4x/core/tech.h"
#include "nebula4x/util/file_io.h"
#include "nebula4x/util/event_export.h"
#include "nebula4x/util/digest.h"
#include "nebula4x/util/save_diff.h"
#include "nebula4x/util/state_export.h"
#include "nebula4x/util/timeline_export.h"
#include "nebula4x/util/tech_export.h"
#include "nebula4x/util/json.h"
#include "nebula4x/util/log.h"
#include "nebula4x/util/strings.h"
#include "nebula4x/util/time.h"

namespace {

#ifndef NEBULA4X_VERSION
#define NEBULA4X_VERSION "unknown"
#endif

int get_int_arg(int argc, char** argv, const std::string& key, int def) {
  for (int i = 1; i < argc - 1; ++i) {
    if (argv[i] == key) return std::stoi(argv[i + 1]);
  }
  return def;
}

std::string get_str_arg(int argc, char** argv, const std::string& key, const std::string& def) {
  for (int i = 1; i < argc - 1; ++i) {
    if (argv[i] == key) return argv[i + 1];
  }
  return def;
}

std::vector<std::string> get_multi_str_args(int argc, char** argv, const std::string& key) {
  std::vector<std::string> out;
  for (int i = 1; i < argc - 1; ++i) {
    if (argv[i] == key) out.push_back(argv[i + 1]);
  }
  return out;
}



bool get_two_str_args(int argc, char** argv, const std::string& key, std::string& out_a, std::string& out_b) {
  for (int i = 1; i < argc - 2; ++i) {
    if (argv[i] == key) {
      out_a = argv[i + 1];
      out_b = argv[i + 2];
      return true;
    }
  }
  return false;
}

bool has_kv_arg(int argc, char** argv, const std::string& key) {
  for (int i = 1; i < argc - 1; ++i) {
    if (argv[i] == key) return true;
  }
  return false;
}

bool has_flag(int argc, char** argv, const std::string& flag) {
  for (int i = 1; i < argc; ++i) {
    if (argv[i] == flag) return true;
  }
  return false;
}

const char* event_level_label(nebula4x::EventLevel l) {
  switch (l) {
    case nebula4x::EventLevel::Info: return "INFO";
    case nebula4x::EventLevel::Warn: return "WARN";
    case nebula4x::EventLevel::Error: return "ERROR";
  }
  return "INFO";
}

const char* event_category_label(nebula4x::EventCategory c) {
  switch (c) {
    case nebula4x::EventCategory::General: return "GENERAL";
    case nebula4x::EventCategory::Research: return "RESEARCH";
    case nebula4x::EventCategory::Shipyard: return "SHIPYARD";
    case nebula4x::EventCategory::Construction: return "CONSTRUCTION";
    case nebula4x::EventCategory::Movement: return "MOVEMENT";
    case nebula4x::EventCategory::Combat: return "COMBAT";
    case nebula4x::EventCategory::Intel: return "INTEL";
    case nebula4x::EventCategory::Exploration: return "EXPLORATION";
    case nebula4x::EventCategory::Diplomacy: return "DIPLOMACY";
  }
  return "GENERAL";
}

const char* body_type_label(nebula4x::BodyType t) {
  switch (t) {
    case nebula4x::BodyType::Star: return "star";
    case nebula4x::BodyType::Planet: return "planet";
    case nebula4x::BodyType::Moon: return "moon";
    case nebula4x::BodyType::Asteroid: return "asteroid";
    case nebula4x::BodyType::Comet: return "comet";
    case nebula4x::BodyType::GasGiant: return "gas_giant";
  }
  return "planet";
}

bool is_digits(const std::string& s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}

bool parse_event_category(const std::string& raw, nebula4x::EventCategory& out) {
  const std::string s = nebula4x::to_lower(raw);
  if (s == "general") {
    out = nebula4x::EventCategory::General;
    return true;
  }
  if (s == "research") {
    out = nebula4x::EventCategory::Research;
    return true;
  }
  if (s == "shipyard") {
    out = nebula4x::EventCategory::Shipyard;
    return true;
  }
  if (s == "construction") {
    out = nebula4x::EventCategory::Construction;
    return true;
  }
  if (s == "movement") {
    out = nebula4x::EventCategory::Movement;
    return true;
  }
  if (s == "combat") {
    out = nebula4x::EventCategory::Combat;
    return true;
  }
  if (s == "intel") {
    out = nebula4x::EventCategory::Intel;
    return true;
  }
  if (s == "exploration") {
    out = nebula4x::EventCategory::Exploration;
  } else if (s == "diplomacy") {
    out = nebula4x::EventCategory::Diplomacy;
    return true;
  }
  return false;
}

std::string trim_copy(std::string t) {
  const auto not_space = [](unsigned char c) { return !std::isspace(c); };
  auto b = std::find_if(t.begin(), t.end(), not_space);
  auto e = std::find_if(t.rbegin(), t.rend(), not_space).base();
  if (b >= e) return {};
  return std::string(b, e);
}

bool parse_event_levels(const std::string& raw, bool& allow_info, bool& allow_warn, bool& allow_error) {
  const std::string s = nebula4x::to_lower(raw);
  if (s.empty() || s == "all") {
    allow_info = true;
    allow_warn = true;
    allow_error = true;
    return true;
  }

  allow_info = false;
  allow_warn = false;
  allow_error = false;

  std::size_t pos = 0;
  while (pos < s.size()) {
    const std::size_t comma = s.find(',', pos);
    std::string token = (comma == std::string::npos) ? s.substr(pos) : s.substr(pos, comma - pos);
    token = trim_copy(std::move(token));
    if (!token.empty()) {
      if (token == "info") {
        allow_info = true;
      } else if (token == "warn" || token == "warning") {
        allow_warn = true;
      } else if (token == "error" || token == "err") {
        allow_error = true;
      } else {
        return false;
      }
    }
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }

  return allow_info || allow_warn || allow_error;
}

bool parse_day_or_date(const std::string& raw, std::int64_t& out_day) {
  std::string s = trim_copy(raw);
  if (s.empty()) return false;

  const auto is_signed_digits = [](const std::string& t) {
    if (t.empty()) return false;
    std::size_t i = 0;
    if (t[0] == '-') i = 1;
    if (i >= t.size()) return false;
    for (; i < t.size(); ++i) {
      if (!std::isdigit(static_cast<unsigned char>(t[i]))) return false;
    }
    return true;
  };

  // Accept either a raw day number (days since epoch) or an ISO date (YYYY-MM-DD).
  if (is_signed_digits(s)) {
    out_day = std::stoll(s);
    return true;
  }
  try {
    const auto d = nebula4x::Date::parse_iso_ymd(s);
    out_day = d.days_since_epoch();
    return true;
  } catch (...) {
    return false;
  }
}
template <typename Map>
std::vector<typename Map::key_type> sorted_keys(const Map& m) {
  std::vector<typename Map::key_type> keys;
  keys.reserve(m.size());
  for (const auto& [k, _] : m) keys.push_back(k);
  std::sort(keys.begin(), keys.end());
  return keys;
}

std::string resolve_tech_id(const std::unordered_map<std::string, nebula4x::TechDef>& techs, const std::string& raw) {
  if (raw.empty()) return {};
  if (techs.find(raw) != techs.end()) return raw;

  // Name match (case-insensitive). Prefer deterministic id ordering.
  const std::string want = nebula4x::to_lower(raw);
  for (const auto& id : sorted_keys(techs)) {
    const auto it = techs.find(id);
    if (it == techs.end()) continue;
    if (nebula4x::to_lower(it->second.name) == want) return id;
  }
  return {};
}




nebula4x::Id resolve_faction_id(const nebula4x::GameState& s, const std::string& raw) {
  if (raw.empty()) return nebula4x::kInvalidId;

  // Numeric id.
  if (is_digits(raw)) {
    const auto id = static_cast<nebula4x::Id>(std::stoll(raw));
    if (s.factions.find(id) != s.factions.end()) return id;
    return nebula4x::kInvalidId;
  }

  // Name match (case-insensitive).
  const std::string want = nebula4x::to_lower(raw);
  for (const auto& [id, f] : s.factions) {
    if (nebula4x::to_lower(f.name) == want) return id;
  }
  return nebula4x::kInvalidId;
}

nebula4x::Id resolve_system_id(const nebula4x::GameState& s, const std::string& raw) {
  if (raw.empty()) return nebula4x::kInvalidId;

  // Numeric id.
  if (is_digits(raw)) {
    const auto id = static_cast<nebula4x::Id>(std::stoll(raw));
    if (s.systems.find(id) != s.systems.end()) return id;
    return nebula4x::kInvalidId;
  }

  // Name match (case-insensitive).
  const std::string want = nebula4x::to_lower(raw);
  for (const auto& [id, sys] : s.systems) {
    if (nebula4x::to_lower(sys.name) == want) return id;
  }
  return nebula4x::kInvalidId;
}

nebula4x::Id resolve_ship_id(const nebula4x::GameState& s, const std::string& raw) {
  if (raw.empty()) return nebula4x::kInvalidId;

  // Numeric id.
  if (is_digits(raw)) {
    const auto id = static_cast<nebula4x::Id>(std::stoll(raw));
    if (s.ships.find(id) != s.ships.end()) return id;
    return nebula4x::kInvalidId;
  }

  // Name match (case-insensitive).
  const std::string want = nebula4x::to_lower(raw);
  for (const auto& [id, sh] : s.ships) {
    if (nebula4x::to_lower(sh.name) == want) return id;
  }
  return nebula4x::kInvalidId;
}

nebula4x::Id resolve_colony_id(const nebula4x::GameState& s, const std::string& raw) {
  if (raw.empty()) return nebula4x::kInvalidId;

  // Numeric id.
  if (is_digits(raw)) {
    const auto id = static_cast<nebula4x::Id>(std::stoll(raw));
    if (s.colonies.find(id) != s.colonies.end()) return id;
    return nebula4x::kInvalidId;
  }

  // Name match (case-insensitive).
  const std::string want = nebula4x::to_lower(raw);
  for (const auto& [id, c] : s.colonies) {
    if (nebula4x::to_lower(c.name) == want) return id;
  }
  return nebula4x::kInvalidId;
}

void print_usage(const char* exe) {
  std::cout << "Nebula4X CLI v" << NEBULA4X_VERSION << "\n\n";
  std::cout << "Usage: " << (exe ? exe : "nebula4x_cli") << " [options]\n\n";
  std::cout << "Options:\n";
  std::cout << "  --days N         Advance simulation by N days (default: 30)\n";
  std::cout << "  --until-event N  Advance up to N days, stopping when a new matching event occurs\n";
  std::cout << "                 (uses --events-* filters; defaults to levels warn,error unless --events-level is provided)\n";
  std::cout << "  --scenario NAME  Starting scenario when not loading (sol|random, default: sol)\n";
  std::cout << "  --seed N         RNG seed for random scenario (default: 1)\n";
  std::cout << "  --systems N      Number of systems for random scenario (default: 12)\n";
  std::cout << "  --content PATH   Content blueprints JSON (repeatable; later overrides earlier; default: data/blueprints/starting_blueprints.json)\n";
  std::cout << "                 Files may use top-level include/includes to compose overlays\n";
  std::cout << "  --tech PATH      Tech tree JSON (repeatable; later overrides earlier; default: data/tech/tech_tree.json)\n";
  std::cout << "                 Files may use top-level include/includes to compose overlays\n";
  std::cout << "  --load PATH      Load a save JSON before advancing\n";
  std::cout << "  --save PATH      Save state JSON after advancing\n";
  std::cout << "  --format-save    Load + re-save (canonicalize JSON) without advancing\n";
  std::cout << "  --fix-save       Attempt to repair common save integrity issues (requires --load and --save or --dump)\n";
  std::cout << "  --diff-saves A B Compare two save JSON files and print a structural diff\n";
  std::cout << "  --diff-saves-json PATH  (optional) Also emit a JSON diff report (PATH can be '-' for stdout)\n";
  std::cout << "  --diff-saves-jsonpatch PATH  (optional) Also emit an RFC 6902 JSON Patch (PATH can be '-' for stdout)\n";
  std::cout << "  --apply-save-patch SAVE PATCH  Apply an RFC 6902 JSON Patch to SAVE\n";
  std::cout << "  --apply-save-patch-out PATH   (optional) Output path for the patched save (PATH can be '-' for stdout; default: -)\n";
  std::cout << "  --validate-content  Validate content + tech files and exit\n";
  std::cout << "  --validate-save     Validate loaded/new game state and exit\n";
  std::cout << "  --digest         Print stable content/state digests (useful for bug reports)\n";
  std::cout << "    --digest-no-events  Exclude the persistent SimEvent log from the state digest\n";
  std::cout << "    --digest-no-ui      Exclude UI-only fields (selected system) from the state digest\n";
  std::cout << "  --dump           Print the resulting save JSON to stdout\n";
  std::cout << "  --quiet          Suppress non-essential summary/status output (useful for scripts)\n";
  std::cout << "  --list-factions  Print faction ids and names, then exit\n";
  std::cout << "  --list-systems   Print system ids and names, then exit\n";
  std::cout << "  --list-bodies    Print body ids/names and basic context, then exit\n";
  std::cout << "  --list-jumps     Print jump point ids/names and links, then exit\n";
  std::cout << "  --list-ships     Print ship ids/names and basic context, then exit\n";
  std::cout << "  --list-colonies  Print colony ids/names and basic context, then exit\n";
  std::cout << "  --export-ships-json PATH    Export ships state to JSON (PATH can be '-' for stdout)\n";
  std::cout << "  --export-colonies-json PATH Export colonies state to JSON (PATH can be '-' for stdout)\n";
  std::cout << "  --export-fleets-json PATH   Export fleets state to JSON (PATH can be '-' for stdout)\n";
  std::cout << "  --export-bodies-json PATH   Export bodies state to JSON (PATH can be '-' for stdout)\n";
  std::cout << "  --export-tech-tree-json PATH Export tech tree definitions to JSON (PATH can be '-' for stdout)\n";
  std::cout << "  --export-tech-tree-dot PATH  Export tech tree graph to Graphviz DOT (PATH can be '-' for stdout)\n";
  std::cout << "  --export-timeline-jsonl PATH Export a daily timeline (counts, economy totals, digests) to JSONL/NDJSON (PATH can be '-' for stdout)\n";
  std::cout << "    --timeline-mineral NAME    (repeatable) Limit timeline mineral/cargo maps to NAME\n";
  std::cout << "    --timeline-include-cargo   Include per-faction ship cargo totals in timeline output\n";
  std::cout << "  --plan-research FACTION TECH  Print a prereq-ordered research plan for FACTION -> TECH\n";
  std::cout << "  --plan-research-json PATH     (optional) Export the plan as JSON (PATH can be '-' for stdout)\n";
  std::cout << "  --dump-events    Print the persistent simulation event log to stdout\n";
  std::cout << "  --export-events-csv PATH  Export the persistent simulation event log to CSV (PATH can be '-' for stdout)\n";
  std::cout << "  --export-events-json PATH Export the persistent simulation event log to JSON (PATH can be '-' for stdout)\n";
  std::cout << "  --export-events-jsonl PATH Export the persistent simulation event log to JSONL/NDJSON (PATH can be '-' for stdout)\n";
  std::cout << "    --events-last N         Only print the last N matching events (0 = all)\n";
  std::cout << "    --events-category NAME  Filter by category (general|research|shipyard|construction|movement|combat|intel|exploration)\n";
  std::cout << "    --events-faction X      Filter by faction id or exact name (case-insensitive)\n";
  std::cout << "    --events-system X       Filter by system id or exact name (case-insensitive)\n";
  std::cout << "    --events-ship X         Filter by ship id or exact name (case-insensitive)\n";
  std::cout << "    --events-colony X       Filter by colony id or exact name (case-insensitive)\n";
  std::cout << "    --events-contains TEXT  Filter by message substring (case-insensitive)\n";
  std::cout << "    --events-level LEVELS  Filter by level (all|info|warn|error or comma-separated list)\n";
  std::cout << "    --events-since X        Filter to events on/after X (day number or YYYY-MM-DD)\n";
  std::cout << "    --events-until X        Filter to events on/before X (day number or YYYY-MM-DD)\n";
  std::cout << "    --events-summary        Print a summary of the filtered events (counts by level/category)\n";
  std::cout << "    --events-summary-json PATH  Export a JSON summary of the filtered events (PATH can be '-' for stdout)\n";
  std::cout << "    --events-summary-csv PATH  Export a CSV summary of the filtered events (PATH can be '-' for stdout)\n";
  std::cout << "  -h, --help       Show this help\n";
  std::cout << "  --version        Print version and exit\n";
}

} // namespace

int main(int argc, char** argv) {
  try {
    if (has_flag(argc, argv, "--version")) {
      std::cout << NEBULA4X_VERSION << "\n";
      return 0;
    }
    if (has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
      print_usage(argv[0]);
      return 0;
    }

    const bool quiet = has_flag(argc, argv, "--quiet");

    // Save diff utility:
    //   --diff-saves A B
    //   --diff-saves A B --diff-saves-json OUT.json
    //   --diff-saves A B --diff-saves-json -   (JSON to stdout; human diff to stderr unless --quiet)
    //   --diff-saves A B --diff-saves-jsonpatch OUT.patch.json
    //   --diff-saves A B --diff-saves-jsonpatch -   (patch to stdout; human diff to stderr unless --quiet)
    std::string diff_a;
    std::string diff_b;
    const bool diff_saves = get_two_str_args(argc, argv, "--diff-saves", diff_a, diff_b);
    const bool diff_flag = has_flag(argc, argv, "--diff-saves");
    const std::string diff_json_path = get_str_arg(argc, argv, "--diff-saves-json", "");
    const std::string diff_patch_path = get_str_arg(argc, argv, "--diff-saves-jsonpatch", "");

    if (diff_flag && !diff_saves) {
      std::cerr << "--diff-saves requires two paths: --diff-saves A B\n\n";
      print_usage(argv[0]);
      return 2;
    }

    if (diff_saves) {
      const bool json_to_stdout = (!diff_json_path.empty() && diff_json_path == "-");
      const bool patch_to_stdout = (!diff_patch_path.empty() && diff_patch_path == "-");
      if (json_to_stdout && patch_to_stdout) {
        std::cerr << "--diff-saves-json and --diff-saves-jsonpatch cannot both write to stdout ('-')\n";
        return 2;
      }

      const auto a_state = nebula4x::deserialize_game_from_json(nebula4x::read_text_file(diff_a));
      const auto b_state = nebula4x::deserialize_game_from_json(nebula4x::read_text_file(diff_b));
      const std::string a_canon = nebula4x::serialize_game_to_json(a_state);
      const std::string b_canon = nebula4x::serialize_game_to_json(b_state);

      if (!diff_json_path.empty()) {
        const std::string report = nebula4x::diff_saves_to_json(a_canon, b_canon);
        if (diff_json_path == "-") {
          std::cout << report;
        } else {
          nebula4x::write_text_file(diff_json_path, report);
          if (!quiet) {
            std::cout << "JSON diff written to " << diff_json_path << "\n";
          }
        }
      }

      if (!diff_patch_path.empty()) {
        const std::string patch = nebula4x::diff_saves_to_json_patch(a_canon, b_canon);
        if (diff_patch_path == "-") {
          std::cout << patch;
        } else {
          nebula4x::write_text_file(diff_patch_path, patch);
          if (!quiet) {
            std::cout << "JSON Patch written to " << diff_patch_path << "\n";
          }
        }
      }

      if (!quiet) {
        const bool machine_to_stdout = json_to_stdout || patch_to_stdout;
        std::ostream& out = machine_to_stdout ? std::cerr : std::cout;
        out << nebula4x::diff_saves_to_text(a_canon, b_canon);
      }
      return 0;
    }

    // Save patch apply utility:
    //   --apply-save-patch SAVE.json PATCH.json
    //   --apply-save-patch SAVE.json PATCH.json --apply-save-patch-out OUT.json
    //   --apply-save-patch SAVE.json PATCH.json --apply-save-patch-out -  (save to stdout; info to stderr unless --quiet)
    std::string apply_save_path;
    std::string apply_patch_path;
    const bool apply_save_patch = get_two_str_args(argc, argv, "--apply-save-patch", apply_save_path, apply_patch_path);
    const bool apply_save_patch_flag = has_flag(argc, argv, "--apply-save-patch");
    const std::string apply_out_path = get_str_arg(argc, argv, "--apply-save-patch-out", "-");

    if (apply_save_patch_flag && !apply_save_patch) {
      std::cerr << "--apply-save-patch requires two paths: --apply-save-patch SAVE PATCH\n\n";
      print_usage(argv[0]);
      return 2;
    }

    if (apply_save_patch) {
      const bool out_to_stdout = (apply_out_path == "-");
      const auto base_state = nebula4x::deserialize_game_from_json(nebula4x::read_text_file(apply_save_path));
      const std::string base_canon = nebula4x::serialize_game_to_json(base_state);

      const std::string patch_json = nebula4x::read_text_file(apply_patch_path);
      const std::string patched_json = nebula4x::apply_json_patch(base_canon, patch_json);

      // Validate the patched document is still a valid Nebula4X save.
      const auto patched_state = nebula4x::deserialize_game_from_json(patched_json);
      const std::string patched_canon = nebula4x::serialize_game_to_json(patched_state);

      if (out_to_stdout) {
        std::cout << patched_canon;
      } else {
        nebula4x::write_text_file(apply_out_path, patched_canon);
        if (!quiet) {
          std::cout << "Patched save written to " << apply_out_path << "\n";
        }
      }

      if (!quiet && out_to_stdout) {
        std::cerr << "Patched save written to stdout\n";
      }
      return 0;
    }

    const int days = get_int_arg(argc, argv, "--days", 30);
    const int until_event_days = get_int_arg(argc, argv, "--until-event", -1);
    const bool until_event = (until_event_days != -1);
    const std::string scenario = get_str_arg(argc, argv, "--scenario", "sol");
    const int seed = get_int_arg(argc, argv, "--seed", 1);
    const int systems = get_int_arg(argc, argv, "--systems", 12);
    std::vector<std::string> content_paths = get_multi_str_args(argc, argv, "--content");
    if (content_paths.empty()) content_paths.push_back("data/blueprints/starting_blueprints.json");
    std::vector<std::string> tech_paths = get_multi_str_args(argc, argv, "--tech");
    if (tech_paths.empty()) tech_paths.push_back("data/tech/tech_tree.json");
    const std::string load_path = get_str_arg(argc, argv, "--load", "");
    const std::string save_path = get_str_arg(argc, argv, "--save", "");
    const std::string export_events_csv_path = get_str_arg(argc, argv, "--export-events-csv", "");
    const std::string export_events_json_path = get_str_arg(argc, argv, "--export-events-json", "");
    const std::string export_events_jsonl_path = get_str_arg(argc, argv, "--export-events-jsonl", "");
    const std::string events_summary_json_path = get_str_arg(argc, argv, "--events-summary-json", "");
    const std::string events_summary_csv_path = get_str_arg(argc, argv, "--events-summary-csv", "");
    const std::string export_ships_json_path = get_str_arg(argc, argv, "--export-ships-json", "");
    const std::string export_colonies_json_path = get_str_arg(argc, argv, "--export-colonies-json", "");
    const std::string export_fleets_json_path = get_str_arg(argc, argv, "--export-fleets-json", "");
    const std::string export_bodies_json_path = get_str_arg(argc, argv, "--export-bodies-json", "");
    const std::string export_tech_tree_json_path = get_str_arg(argc, argv, "--export-tech-tree-json", "");
    const std::string export_tech_tree_dot_path = get_str_arg(argc, argv, "--export-tech-tree-dot", "");
    const std::string export_timeline_jsonl_path = get_str_arg(argc, argv, "--export-timeline-jsonl", "");

    const bool print_digests = has_flag(argc, argv, "--digest");
    const bool digest_no_events = has_flag(argc, argv, "--digest-no-events");
    const bool digest_no_ui = has_flag(argc, argv, "--digest-no-ui");

    nebula4x::TimelineExportOptions timeline_opt;
    timeline_opt.include_minerals = true;
    timeline_opt.include_ship_cargo = has_flag(argc, argv, "--timeline-include-cargo");
    timeline_opt.mineral_filter = get_multi_str_args(argc, argv, "--timeline-mineral");
    timeline_opt.digest.include_events = !digest_no_events;
    timeline_opt.digest.include_ui_state = !digest_no_ui;

    std::string plan_faction_raw;
    std::string plan_tech_raw;
    const bool plan_research = get_two_str_args(argc, argv, "--plan-research", plan_faction_raw, plan_tech_raw);
    const bool plan_research_flag = has_flag(argc, argv, "--plan-research");
    const std::string plan_research_json_path = get_str_arg(argc, argv, "--plan-research-json", "");

    if (plan_research_flag && !plan_research) {
      std::cerr << "--plan-research requires two args: --plan-research FACTION TECH\n\n";
      print_usage(argv[0]);
      return 2;
    }

    if (!plan_research && !plan_research_json_path.empty()) {
      std::cerr << "--plan-research-json requires --plan-research\n\n";
      print_usage(argv[0]);
      return 2;
    }

    const bool script_stdout =
        (!export_events_csv_path.empty() && export_events_csv_path == "-") ||
        (!export_events_json_path.empty() && export_events_json_path == "-") ||
        (!export_events_jsonl_path.empty() && export_events_jsonl_path == "-") ||
        (!events_summary_json_path.empty() && events_summary_json_path == "-") ||
        (!events_summary_csv_path.empty() && events_summary_csv_path == "-") ||
        (!export_ships_json_path.empty() && export_ships_json_path == "-") ||
        (!export_colonies_json_path.empty() && export_colonies_json_path == "-") ||
        (!export_fleets_json_path.empty() && export_fleets_json_path == "-") ||
        (!export_bodies_json_path.empty() && export_bodies_json_path == "-") ||
        (!export_tech_tree_json_path.empty() && export_tech_tree_json_path == "-") ||
        (!export_tech_tree_dot_path.empty() && export_tech_tree_dot_path == "-") ||
        (!export_timeline_jsonl_path.empty() && export_timeline_jsonl_path == "-") ||
        (!plan_research_json_path.empty() && plan_research_json_path == "-");

    const bool list_factions = has_flag(argc, argv, "--list-factions");
    const bool list_systems = has_flag(argc, argv, "--list-systems");
    const bool list_bodies = has_flag(argc, argv, "--list-bodies");
    const bool list_jumps = has_flag(argc, argv, "--list-jumps");
    const bool list_ships = has_flag(argc, argv, "--list-ships");
    const bool list_colonies = has_flag(argc, argv, "--list-colonies");

    const bool format_save = has_flag(argc, argv, "--format-save");
    const bool fix_save = has_flag(argc, argv, "--fix-save");
    const bool validate_content = has_flag(argc, argv, "--validate-content");
    const bool validate_save = has_flag(argc, argv, "--validate-save");

    if (format_save) {
      if (load_path.empty() || save_path.empty()) {
        std::cerr << "--format-save requires both --load and --save\n\n";
        print_usage(argv[0]);
        return 2;
      }

      const auto loaded = nebula4x::deserialize_game_from_json(nebula4x::read_text_file(load_path));
      nebula4x::write_text_file(save_path, nebula4x::serialize_game_to_json(loaded));
      if (!quiet) {
        std::ostream& info = script_stdout ? std::cerr : std::cout;
        info << "Formatted save written to " << save_path << "\n";
      }
      return 0;
    }

    if (fix_save) {
      if (load_path.empty() || (save_path.empty() && !has_flag(argc, argv, "--dump"))) {
        std::cerr << "--fix-save requires --load and either --save or --dump\n\n";
        print_usage(argv[0]);
        return 2;
      }
    }

    auto content = nebula4x::load_content_db_from_files(content_paths);
    content.techs = nebula4x::load_tech_db_from_files(tech_paths);

    if (validate_content) {
      const auto errors = nebula4x::validate_content_db(content);
      if (!errors.empty()) {
        std::cerr << "Content validation failed:\n";
        for (const auto& e : errors) std::cerr << "  - " << e << "\n";
        return 1;
      }
      if (!quiet) {
        std::ostream& info = script_stdout ? std::cerr : std::cout;
        info << "Content OK\n";
      }
      return 0;
    }

    nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});

    if (!load_path.empty()) {
      sim.load_game(nebula4x::deserialize_game_from_json(nebula4x::read_text_file(load_path)));
    } else {
      if (scenario == "random") {
        sim.load_game(nebula4x::make_random_scenario(static_cast<std::uint32_t>(seed), systems));
      } else if (scenario == "sol") {
        // The Simulation ctor already starts a new Sol game.
      } else {
        std::cerr << "Unknown --scenario: '" << scenario << "'\n\n";
        print_usage(argv[0]);
        return 2;
      }
    }

    if (fix_save) {
      const bool dump_json = has_flag(argc, argv, "--dump");
      std::ostream& info = dump_json ? std::cerr : (script_stdout ? std::cerr : std::cout);

      const auto report = nebula4x::fix_game_state(sim.state(), &sim.content());
      const auto errors = nebula4x::validate_game_state(sim.state(), &sim.content());

      if (!quiet) {
        info << "Applied state fixer: " << report.changes << " change(s)";
        if (!errors.empty()) {
          info << " (validation still failing)";
        }
        info << "\n";

        const std::size_t max_lines = 100;
        const std::size_t n = std::min<std::size_t>(max_lines, report.actions.size());
        for (std::size_t i = 0; i < n; ++i) {
          info << "  - " << report.actions[i] << "\n";
        }
        if (report.actions.size() > max_lines) {
          info << "  ... (" << (report.actions.size() - max_lines) << " more)\n";
        }

        if (!errors.empty()) {
          info << "\nState validation failed after fix (" << errors.size() << " error(s)):\n";
          const std::size_t max_err = 50;
          const std::size_t ecount = std::min<std::size_t>(max_err, errors.size());
          for (std::size_t i = 0; i < ecount; ++i) {
            info << "  - " << errors[i] << "\n";
          }
          if (errors.size() > max_err) {
            info << "  ... (" << (errors.size() - max_err) << " more)\n";
          }
        }
      }

      if (!save_path.empty()) {
        nebula4x::write_text_file(save_path, nebula4x::serialize_game_to_json(sim.state()));
        if (!quiet) {
          info << "\nWrote fixed save to " << save_path << "\n";
        }
      }

      if (dump_json) {
        std::cout << "\n--- JSON ---\n" << nebula4x::serialize_game_to_json(sim.state()) << "\n";
      }

      return errors.empty() ? 0 : 1;
    }


    if (validate_save) {
      const auto errors = nebula4x::validate_game_state(sim.state(), &sim.content());
      if (!errors.empty()) {
        std::cerr << "State validation failed:\n";
        for (const auto& e : errors) std::cerr << "  - " << e << "\n";
        return 1;
      }
      if (!quiet) {
        std::ostream& info = script_stdout ? std::cerr : std::cout;
        info << "State OK\n";
      }
      return 0;
    }

    // Convenience helpers for scripting: list ids/names and exit.
    if (list_factions || list_systems || list_bodies || list_jumps || list_ships || list_colonies) {
      const auto& st = sim.state();

      auto faction_name = [&](nebula4x::Id id) -> std::string {
        if (id == nebula4x::kInvalidId) return {};
        const auto it = st.factions.find(id);
        return (it != st.factions.end()) ? it->second.name : std::string{};
      };
      auto system_name = [&](nebula4x::Id id) -> std::string {
        if (id == nebula4x::kInvalidId) return {};
        const auto it = st.systems.find(id);
        return (it != st.systems.end()) ? it->second.name : std::string{};
      };

      bool printed_any = false;

      if (list_factions) {
        printed_any = true;
        std::cout << "Factions: " << st.factions.size() << "\n";
        for (const auto id : sorted_keys(st.factions)) {
          const auto it = st.factions.find(id);
          if (it == st.factions.end()) continue;
          std::cout << "  " << static_cast<unsigned long long>(id) << "\t" << it->second.name << "\n";
        }
      }

      if (list_systems) {
        if (printed_any) std::cout << "\n";
        printed_any = true;
        std::cout << "Systems: " << st.systems.size() << "\n";
        for (const auto id : sorted_keys(st.systems)) {
          const auto it = st.systems.find(id);
          if (it == st.systems.end()) continue;
          const auto& sys = it->second;
          std::cout << "  " << static_cast<unsigned long long>(id) << "\t" << sys.name
                    << "\t(bodies=" << sys.bodies.size() << ", ships=" << sys.ships.size()
                    << ", jumps=" << sys.jump_points.size() << ")\n";
        }
      }

      if (list_bodies) {
        if (printed_any) std::cout << "\n";
        printed_any = true;
        std::cout << "Bodies: " << st.bodies.size() << "\n";
        for (const auto id : sorted_keys(st.bodies)) {
          const auto it = st.bodies.find(id);
          if (it == st.bodies.end()) continue;
          const auto& b = it->second;
          double dep_total = 0.0;
          for (const auto& [_, tons] : b.mineral_deposits) {
            if (tons > 0.0) dep_total += tons;
          }
          std::cout << "  " << static_cast<unsigned long long>(id) << "\t" << b.name
                    << "\t" << body_type_label(b.type)
                    << "\t" << system_name(b.system_id)
                    << "\torbit_r=" << b.orbit_radius_mkm
                    << "\torbit_d=" << b.orbit_period_days
                    << "\tpos=(" << b.position_mkm.x << "," << b.position_mkm.y << ")";
          if (!b.mineral_deposits.empty()) {
            std::cout << "\tdeposits_tons=" << dep_total;
          }
          std::cout << "\n";
        }
      }

      if (list_jumps) {
        if (printed_any) std::cout << "\n";
        printed_any = true;
        std::cout << "Jump Points: " << st.jump_points.size() << "\n";
        for (const auto id : sorted_keys(st.jump_points)) {
          const auto it = st.jump_points.find(id);
          if (it == st.jump_points.end()) continue;
          const auto& jp = it->second;

          const auto* linked = nebula4x::find_ptr(st.jump_points, jp.linked_jump_id);
          const auto other_sys_id = linked ? linked->system_id : nebula4x::kInvalidId;

          std::cout << "  " << static_cast<unsigned long long>(id) << "\t" << jp.name
                    << "\t" << system_name(jp.system_id)
                    << "\tpos=(" << jp.position_mkm.x << "," << jp.position_mkm.y << ")"
                    << "\tlinked=" << static_cast<unsigned long long>(jp.linked_jump_id)
                    << "\tto=" << system_name(other_sys_id)
                    << "\n";
        }
      }

      if (list_ships) {
        if (printed_any) std::cout << "\n";
        printed_any = true;
        std::cout << "Ships: " << st.ships.size() << "\n";
        for (const auto id : sorted_keys(st.ships)) {
          const auto it = st.ships.find(id);
          if (it == st.ships.end()) continue;
          const auto& sh = it->second;

          std::size_t qn = 0;
          bool repeat = false;
          if (const auto* so = nebula4x::find_ptr(st.ship_orders, id)) {
            qn = so->queue.size();
            repeat = so->repeat;
          }

          double cargo_tons = 0.0;
          for (const auto& [_, v] : sh.cargo) cargo_tons += v;

          std::cout << "  " << static_cast<unsigned long long>(id) << "\t" << sh.name
                    << "\t" << faction_name(sh.faction_id) << "\t" << system_name(sh.system_id)
                    << "\t" << sh.design_id
                    << "\thp=" << sh.hp
                    << "\tcargo=" << cargo_tons
                    << "\torders=" << qn
                    << (repeat ? "\trepeat=1" : "")
                    << "\n";
        }
      }

      if (list_colonies) {
        if (printed_any) std::cout << "\n";
        printed_any = true;
        std::cout << "Colonies: " << st.colonies.size() << "\n";
        for (const auto id : sorted_keys(st.colonies)) {
          const auto it = st.colonies.find(id);
          if (it == st.colonies.end()) continue;
          const auto& c = it->second;

          const auto* b = nebula4x::find_ptr(st.bodies, c.body_id);
          const auto sys_id = b ? b->system_id : nebula4x::kInvalidId;

          std::cout << "  " << static_cast<unsigned long long>(id) << "\t" << c.name
                    << "\t" << faction_name(c.faction_id)
                    << "\t" << system_name(sys_id)
                    << "\tbody=" << (b ? b->name : std::string{})
                    << "\tpop_m=" << c.population_millions
                    << "\tinst=" << c.installations.size()
                    << "\tshipyard_q=" << c.shipyard_queue.size()
                    << "\tbuild_q=" << c.construction_queue.size()
                    << "\n";
        }
      }

      return 0;
    }

    const bool export_timeline_jsonl = !export_timeline_jsonl_path.empty();

    std::uint64_t content_digest = 0;
    if (export_timeline_jsonl || print_digests) {
      content_digest = nebula4x::digest_content_db64(sim.content());
    }

    std::vector<nebula4x::TimelineSnapshot> timeline;
    std::uint64_t prev_next_event_seq = sim.state().next_event_seq;
    if (export_timeline_jsonl) {
      int reserve_days = until_event ? until_event_days : days;
      if (reserve_days < 0) reserve_days = 0;
      timeline.reserve(static_cast<std::size_t>(reserve_days) + 1);

      // Initial snapshot: new_events == 0
      timeline.push_back(nebula4x::compute_timeline_snapshot(
          sim.state(), sim.content(), content_digest, sim.state().next_event_seq, timeline_opt));
      prev_next_event_seq = sim.state().next_event_seq;
    }

    nebula4x::AdvanceUntilEventResult until_res{};
    if (until_event) {
      if (until_event_days <= 0) {
        std::cerr << "--until-event requires N > 0\n\n";
        print_usage(argv[0]);
        return 2;
      }

      // Build stop condition from the same --events-* flags.
      // Default to warn/error unless --events-level is explicitly provided.
      nebula4x::EventStopCondition stop{};
      const std::string levels_raw = has_kv_arg(argc, argv, "--events-level")
                                      ? get_str_arg(argc, argv, "--events-level", "all")
                                      : std::string("warn,error");
      bool allow_info = true;
      bool allow_warn = true;
      bool allow_error = true;
      if (!parse_event_levels(levels_raw, allow_info, allow_warn, allow_error)) {
        std::cerr << "Unknown --events-level: " << levels_raw << "\n";
        return 2;
      }
      stop.stop_on_info = allow_info;
      stop.stop_on_warn = allow_warn;
      stop.stop_on_error = allow_error;

      const std::string cat_raw = get_str_arg(argc, argv, "--events-category", "");
      if (!cat_raw.empty()) {
        stop.filter_category = true;
        if (!parse_event_category(cat_raw, stop.category)) {
          std::cerr << "Unknown --events-category: " << cat_raw << "\n";
          return 2;
        }
      }

      const std::string fac_raw = get_str_arg(argc, argv, "--events-faction", "");
      if (!fac_raw.empty()) {
        const auto& st = sim.state();
        stop.faction_id = resolve_faction_id(st, fac_raw);
        if (stop.faction_id == nebula4x::kInvalidId) {
          std::cerr << "Unknown --events-faction: " << fac_raw << "\n";
          return 2;
        }
      }

      const std::string sys_raw = get_str_arg(argc, argv, "--events-system", "");
      if (!sys_raw.empty()) {
        const auto& st = sim.state();
        stop.system_id = resolve_system_id(st, sys_raw);
        if (stop.system_id == nebula4x::kInvalidId) {
          std::cerr << "Unknown --events-system: " << sys_raw << "\n";
          return 2;
        }
      }

      const std::string ship_raw = get_str_arg(argc, argv, "--events-ship", "");
      if (!ship_raw.empty()) {
        const auto& st = sim.state();
        stop.ship_id = resolve_ship_id(st, ship_raw);
        if (stop.ship_id == nebula4x::kInvalidId) {
          std::cerr << "Unknown --events-ship: " << ship_raw << "\n";
          return 2;
        }
      }

      const std::string col_raw = get_str_arg(argc, argv, "--events-colony", "");
      if (!col_raw.empty()) {
        const auto& st = sim.state();
        stop.colony_id = resolve_colony_id(st, col_raw);
        if (stop.colony_id == nebula4x::kInvalidId) {
          std::cerr << "Unknown --events-colony: " << col_raw << "\n";
          return 2;
        }
      }

      stop.message_contains = get_str_arg(argc, argv, "--events-contains", "");

      if (export_timeline_jsonl) {
        // Step day-by-day so we can emit a snapshot per day.
        for (int i = 0; i < until_event_days; ++i) {
          const auto day_res = sim.advance_until_event(1, stop);
          until_res.days_advanced += day_res.days_advanced;
          if (day_res.hit) {
            until_res.hit = true;
            until_res.event = day_res.event;
          }

          timeline.push_back(nebula4x::compute_timeline_snapshot(
              sim.state(), sim.content(), content_digest, prev_next_event_seq, timeline_opt));
          prev_next_event_seq = sim.state().next_event_seq;

          if (day_res.hit) break;
        }
      } else {
        until_res = sim.advance_until_event(until_event_days, stop);
      }
    } else {
      if (export_timeline_jsonl) {
        for (int i = 0; i < days; ++i) {
          sim.advance_days(1);
          timeline.push_back(nebula4x::compute_timeline_snapshot(
              sim.state(), sim.content(), content_digest, prev_next_event_seq, timeline_opt));
          prev_next_event_seq = sim.state().next_event_seq;
        }
      } else {
        sim.advance_days(days);
      }
    }

    const auto& s = sim.state();
    if (!quiet) {
      // When producing machine-readable output on stdout (PATH='-'), keep human
      // status output on stderr so scripts can safely parse stdout.
      std::ostream& info = script_stdout ? std::cerr : std::cout;
      info << "Date: " << s.date.to_string();
      info << " " << std::setw(2) << std::setfill('0') << std::clamp(s.hour_of_day, 0, 23) << ":00\n";
      info << "Systems: " << s.systems.size() << ", Bodies: " << s.bodies.size() << ", Jump Points: "
                << s.jump_points.size() << ", Ships: " << s.ships.size() << ", Colonies: " << s.colonies.size()
                << "\n";

      for (const auto& [_, c] : s.colonies) {
        info << "\nColony " << c.name << " minerals:\n";
        for (const auto& [k, v] : c.minerals) {
          info << "  " << k << ": " << v << "\n";
        }
      }
    }

    if (until_event) {
      std::ostream& status = (quiet || script_stdout) ? std::cerr : std::cout;
      if (!quiet) status << "\n";
      if (until_res.hit) {
        const nebula4x::Date d(until_res.event.day);
        status << "Until-event: hit after " << until_res.days_advanced << " days (" << until_res.hours_advanced
               << " hours) -> [" << format_datetime(d, until_res.event.hour) << "] #"
               << static_cast<unsigned long long>(until_res.event.seq) << " ["
               << event_category_label(until_res.event.category) << "] "
               << event_level_label(until_res.event.level) << ": " << until_res.event.message << "\n";
      } else {
        status << "Until-event: no matching event within " << until_event_days << " days (advanced "
               << until_res.days_advanced << " days / " << until_res.hours_advanced << " hours, date now "
               << format_datetime(s.date, s.hour_of_day) << ")\n";
      }
    }

    if (print_digests) {
      std::ostream& out = script_stdout ? std::cerr : std::cout;
      out << "content_digest " << nebula4x::digest64_to_hex(content_digest) << "\n";
      out << "state_digest "
          << nebula4x::digest64_to_hex(nebula4x::digest_game_state64(sim.state(), timeline_opt.digest))
          << "\n";
    }

    const bool dump_events = has_flag(argc, argv, "--dump-events");
    const bool export_events_csv = !export_events_csv_path.empty();
    const bool export_events_json = !export_events_json_path.empty();
    const bool export_events_jsonl = !export_events_jsonl_path.empty();
    const bool events_summary = has_flag(argc, argv, "--events-summary");
    const bool events_summary_json = !events_summary_json_path.empty();
    const bool events_summary_csv = !events_summary_csv_path.empty();
    const bool export_ships_json = !export_ships_json_path.empty();
    const bool export_colonies_json = !export_colonies_json_path.empty();
    const bool export_fleets_json = !export_fleets_json_path.empty();
    const bool export_bodies_json = !export_bodies_json_path.empty();
    const bool export_tech_tree_json = !export_tech_tree_json_path.empty();
    const bool export_tech_tree_dot = !export_tech_tree_dot_path.empty();
    const bool export_plan_json = !plan_research_json_path.empty();

    if (dump_events || export_events_csv || export_events_json || export_events_jsonl || events_summary ||
        events_summary_json || events_summary_csv || export_ships_json || export_colonies_json || export_fleets_json ||
        export_bodies_json ||
        export_tech_tree_json || export_tech_tree_dot || export_timeline_jsonl || plan_research || export_plan_json) {
      // Prevent ambiguous script output.
      {
        int stdout_exports = 0;
        if (export_events_csv && export_events_csv_path == "-") ++stdout_exports;
        if (export_events_json && export_events_json_path == "-") ++stdout_exports;
        if (export_events_jsonl && export_events_jsonl_path == "-") ++stdout_exports;
        if (events_summary_json && events_summary_json_path == "-") ++stdout_exports;
        if (events_summary_csv && events_summary_csv_path == "-") ++stdout_exports;
        if (export_ships_json && export_ships_json_path == "-") ++stdout_exports;
        if (export_colonies_json && export_colonies_json_path == "-") ++stdout_exports;
        if (export_fleets_json && export_fleets_json_path == "-") ++stdout_exports;
        if (export_bodies_json && export_bodies_json_path == "-") ++stdout_exports;
        if (export_tech_tree_json && export_tech_tree_json_path == "-") ++stdout_exports;
        if (export_tech_tree_dot && export_tech_tree_dot_path == "-") ++stdout_exports;
        if (export_timeline_jsonl && export_timeline_jsonl_path == "-") ++stdout_exports;
        if (export_plan_json && plan_research_json_path == "-") ++stdout_exports;
        if (stdout_exports > 1) {
          std::cerr << "Multiple machine-readable outputs set to '-' (stdout). Choose at most one.\n";
          return 2;
        }
        if (stdout_exports == 1) {
          if (dump_events || events_summary || has_flag(argc, argv, "--dump")) {
            std::cerr << "Cannot combine --dump-events/--events-summary/--dump with stdout export (PATH='-').\n";
            std::cerr << "Write those outputs to a file instead, or remove them for script-friendly stdout.\n";
            return 2;
          }
        }
      }

      // --- Tech tree exports (content-level) ---
      if (export_tech_tree_json) {
        const std::string blob = nebula4x::tech_tree_to_json(sim.content().techs);
        if (export_tech_tree_json_path == "-") {
          std::cout << blob;
        } else {
          nebula4x::write_text_file(export_tech_tree_json_path, blob);
          if (!quiet) {
            std::ostream& info = script_stdout ? std::cerr : std::cout;
            info << "Tech tree JSON written to " << export_tech_tree_json_path << "\n";
          }
        }
      }

      if (export_tech_tree_dot) {
        const std::string blob = nebula4x::tech_tree_to_dot(sim.content().techs);
        if (export_tech_tree_dot_path == "-") {
          std::cout << blob;
        } else {
          nebula4x::write_text_file(export_tech_tree_dot_path, blob);
          if (!quiet) {
            std::ostream& info = script_stdout ? std::cerr : std::cout;
            info << "Tech tree DOT written to " << export_tech_tree_dot_path << "\n";
          }
        }
      }

      // --- Research planner ---
      if (plan_research) {
        const nebula4x::Id fid = resolve_faction_id(s, plan_faction_raw);
        if (fid == nebula4x::kInvalidId) {
          std::cerr << "Unknown --plan-research faction: " << plan_faction_raw << "\n";
          return 2;
        }
        const auto* fac = nebula4x::find_ptr(s.factions, fid);
        if (!fac) {
          std::cerr << "Faction not found: " << plan_faction_raw << "\n";
          return 2;
        }

        const std::string tech_id = resolve_tech_id(sim.content().techs, plan_tech_raw);
        if (tech_id.empty()) {
          std::cerr << "Unknown --plan-research tech: " << plan_tech_raw << "\n";
          return 2;
        }

        const auto it_tech = sim.content().techs.find(tech_id);
        const std::string tech_name = (it_tech == sim.content().techs.end()) ? tech_id : it_tech->second.name;

        const auto plan = nebula4x::compute_research_plan(sim.content(), *fac, tech_id);

        if (export_plan_json) {
          nebula4x::json::Object root;
          root["ok"] = plan.ok();
          root["faction_id"] = static_cast<double>(fid);
          root["faction"] = fac->name;
          root["target_tech_id"] = tech_id;
          root["target_tech"] = tech_name;
          root["total_cost"] = plan.plan.total_cost;

          nebula4x::json::Array errors;
          for (const auto& e : plan.errors) errors.push_back(e);
          root["errors"] = nebula4x::json::array(std::move(errors));

          nebula4x::json::Array techs;
          techs.reserve(plan.plan.tech_ids.size());
          for (const auto& tid : plan.plan.tech_ids) {
            nebula4x::json::Object to;
            to["id"] = tid;
            const auto it = sim.content().techs.find(tid);
            if (it != sim.content().techs.end()) {
              to["name"] = it->second.name;
              to["cost"] = it->second.cost;
            }
            techs.push_back(nebula4x::json::object(std::move(to)));
          }
          root["plan"] = nebula4x::json::array(std::move(techs));

          const std::string blob = nebula4x::json::stringify(nebula4x::json::object(std::move(root))) + "\n";
          if (plan_research_json_path == "-") {
            std::cout << blob;
          } else {
            nebula4x::write_text_file(plan_research_json_path, blob);
            if (!quiet) {
              std::ostream& info = script_stdout ? std::cerr : std::cout;
              info << "Research plan JSON written to " << plan_research_json_path << "\n";
            }
          }
        } else {
          // Human-readable plan.
          std::ostream& out = script_stdout ? std::cerr : std::cout;
          out << "Research plan for " << fac->name << " -> " << tech_name << " (" << tech_id << ")\n";
          if (!plan.ok()) {
            out << "Errors:\n";
            for (const auto& e : plan.errors) out << "  - " << e << "\n";
          }
          out << "Steps: " << plan.plan.tech_ids.size() << ", Total cost: " << plan.plan.total_cost << "\n";
          for (std::size_t i = 0; i < plan.plan.tech_ids.size(); ++i) {
            const auto& tid = plan.plan.tech_ids[i];
            const auto it = sim.content().techs.find(tid);
            const std::string nm = (it == sim.content().techs.end()) ? tid : it->second.name;
            const double cost = (it == sim.content().techs.end()) ? 0.0 : it->second.cost;
            out << "  " << (i + 1) << ". " << nm << " (" << tid << ")";
            if (cost > 0.0) out << "  cost=" << cost;
            out << "\n";
          }
        }
      }

      const int events_last = std::max(0, get_int_arg(argc, argv, "--events-last", 0));
      const std::string cat_raw = get_str_arg(argc, argv, "--events-category", "");
      const std::string fac_raw = get_str_arg(argc, argv, "--events-faction", "");
      const std::string sys_raw = get_str_arg(argc, argv, "--events-system", "");
      const std::string ship_raw = get_str_arg(argc, argv, "--events-ship", "");
      const std::string col_raw = get_str_arg(argc, argv, "--events-colony", "");
      const std::string contains_raw = get_str_arg(argc, argv, "--events-contains", "");
      const std::string contains_filter = nebula4x::to_lower(contains_raw);
      const std::string levels_raw = get_str_arg(argc, argv, "--events-level", "all");
      const std::string since_raw = get_str_arg(argc, argv, "--events-since", "");
      const std::string until_raw = get_str_arg(argc, argv, "--events-until", "");
      bool allow_info = true;
      bool allow_warn = true;
      bool allow_error = true;
      if (!parse_event_levels(levels_raw, allow_info, allow_warn, allow_error)) {
        std::cerr << "Unknown --events-level: " << levels_raw << "\n";
        return 2;
      }


      bool has_since = false;
      std::int64_t since_day = 0;
      if (!since_raw.empty()) {
        if (!parse_day_or_date(since_raw, since_day)) {
          std::cerr << "Unknown --events-since (expected day number or YYYY-MM-DD): " << since_raw << "\n";
          return 2;
        }
        has_since = true;
      }

      bool has_until = false;
      std::int64_t until_day = 0;
      if (!until_raw.empty()) {
        if (!parse_day_or_date(until_raw, until_day)) {
          std::cerr << "Unknown --events-until (expected day number or YYYY-MM-DD): " << until_raw << "\n";
          return 2;
        }
        has_until = true;
      }

      if (has_since && has_until && since_day > until_day) {
        std::cerr << "Invalid event range: --events-since is after --events-until\n";
        return 2;
      }


      bool has_cat = !cat_raw.empty();
      nebula4x::EventCategory cat_filter = nebula4x::EventCategory::General;
      if (has_cat && !parse_event_category(cat_raw, cat_filter)) {
        std::cerr << "Unknown --events-category: " << cat_raw << "\n";
        return 2;
      }

      const nebula4x::Id fac_filter = resolve_faction_id(s, fac_raw);
      if (!fac_raw.empty() && fac_filter == nebula4x::kInvalidId) {
        std::cerr << "Unknown --events-faction: " << fac_raw << "\n";
        return 2;
      }

      const nebula4x::Id sys_filter = resolve_system_id(s, sys_raw);
      if (!sys_raw.empty() && sys_filter == nebula4x::kInvalidId) {
        std::cerr << "Unknown --events-system: " << sys_raw << "\n";
        return 2;
      }

      const nebula4x::Id ship_filter = resolve_ship_id(s, ship_raw);
      if (!ship_raw.empty() && ship_filter == nebula4x::kInvalidId) {
        std::cerr << "Unknown --events-ship: " << ship_raw << "\n";
        return 2;
      }

      const nebula4x::Id col_filter = resolve_colony_id(s, col_raw);
      if (!col_raw.empty() && col_filter == nebula4x::kInvalidId) {
        std::cerr << "Unknown --events-colony: " << col_raw << "\n";
        return 2;
      }

      std::vector<const nebula4x::SimEvent*> filtered;
      filtered.reserve(s.events.size());
      for (const auto& ev : s.events) {
        if (has_since && ev.day < since_day) continue;
        if (has_until && ev.day > until_day) continue;
        if (ev.level == nebula4x::EventLevel::Info && !allow_info) continue;
        if (ev.level == nebula4x::EventLevel::Warn && !allow_warn) continue;
        if (ev.level == nebula4x::EventLevel::Error && !allow_error) continue;
        if (has_cat && ev.category != cat_filter) continue;
        if (fac_filter != nebula4x::kInvalidId && ev.faction_id != fac_filter && ev.faction_id2 != fac_filter) continue;
        if (sys_filter != nebula4x::kInvalidId && ev.system_id != sys_filter) continue;
        if (ship_filter != nebula4x::kInvalidId && ev.ship_id != ship_filter) continue;
        if (col_filter != nebula4x::kInvalidId && ev.colony_id != col_filter) continue;
        if (!contains_filter.empty()) {
          if (nebula4x::to_lower(ev.message).find(contains_filter) == std::string::npos) continue;
        }
        filtered.push_back(&ev);
      }

      if (events_last > 0 && (int)filtered.size() > events_last) {
        filtered.erase(filtered.begin(), filtered.end() - events_last);
      }

      if (events_summary) {
        if (!quiet) std::cout << "\n";
        std::cout << "Event summary: " << filtered.size();
        if (!(allow_info && allow_warn && allow_error)) std::cout << " (levels=" << levels_raw << ")";
        if (has_cat) std::cout << " (category=" << event_category_label(cat_filter) << ")";
        if (has_since) std::cout << " (since=" << since_raw << ")";
        if (has_until) std::cout << " (until=" << until_raw << ")";
        if (fac_filter != nebula4x::kInvalidId) {
          const auto itf = s.factions.find(fac_filter);
          const std::string name = (itf != s.factions.end()) ? itf->second.name : std::string("(missing)");
          std::cout << " (faction=" << name << ")";
        }
        if (sys_filter != nebula4x::kInvalidId) {
          const auto its = s.systems.find(sys_filter);
          const std::string name = (its != s.systems.end()) ? its->second.name : std::string("(missing)");
          std::cout << " (system=" << name << ")";
        }
        if (ship_filter != nebula4x::kInvalidId) {
          const auto itsh = s.ships.find(ship_filter);
          const std::string name = (itsh != s.ships.end()) ? itsh->second.name : std::string("(missing)");
          std::cout << " (ship=" << name << ")";
        }
        if (col_filter != nebula4x::kInvalidId) {
          const auto itc = s.colonies.find(col_filter);
          const std::string name = (itc != s.colonies.end()) ? itc->second.name : std::string("(missing)");
          std::cout << " (colony=" << name << ")";
        }
        if (!contains_filter.empty()) std::cout << " (contains='" << contains_raw << "')";
        if (events_last > 0) std::cout << " (tail=" << events_last << ")";
        std::cout << "\n";

        if (filtered.empty()) {
          std::cout << "  (none)\n";
        } else {
          std::size_t info_count = 0;
          std::size_t warn_count = 0;
          std::size_t error_count = 0;
          std::vector<std::size_t> by_cat(9, 0);

          std::int64_t min_day = filtered.front()->day;
          std::int64_t max_day = filtered.front()->day;

          for (const auto* ev : filtered) {
            min_day = std::min(min_day, ev->day);
            max_day = std::max(max_day, ev->day);

            if (ev->level == nebula4x::EventLevel::Info) ++info_count;
            if (ev->level == nebula4x::EventLevel::Warn) ++warn_count;
            if (ev->level == nebula4x::EventLevel::Error) ++error_count;

            const int idx = static_cast<int>(ev->category);
            if (idx >= 0 && idx < (int)by_cat.size()) ++by_cat[static_cast<std::size_t>(idx)];
          }

          const nebula4x::Date d0(min_day);
          const nebula4x::Date d1(max_day);
          std::cout << "  Range: [" << d0.to_string() << " .. " << d1.to_string() << "]\n";
          std::cout << "  Levels: INFO=" << info_count << "  WARN=" << warn_count << "  ERROR=" << error_count << "\n";

          std::cout << "  Categories:\n";
          const nebula4x::EventCategory cats[] = {
              nebula4x::EventCategory::General,
              nebula4x::EventCategory::Research,
              nebula4x::EventCategory::Shipyard,
              nebula4x::EventCategory::Construction,
              nebula4x::EventCategory::Movement,
              nebula4x::EventCategory::Combat,
              nebula4x::EventCategory::Intel,
              nebula4x::EventCategory::Exploration,
              nebula4x::EventCategory::Diplomacy,
          };
          for (auto c : cats) {
            const int idx = static_cast<int>(c);
            const std::size_t n = (idx >= 0 && idx < (int)by_cat.size()) ? by_cat[static_cast<std::size_t>(idx)] : 0;
            if (n == 0) continue;
            std::cout << "    " << event_category_label(c) << ": " << n << "\n";
          }
        }
      }



if (events_summary_json) {
  try {
    const std::string summary_json_text = nebula4x::events_summary_to_json(filtered);
    if (events_summary_json_path == "-") {
      // Explicit stdout export for scripting.
      std::cout << summary_json_text;
    } else {
      nebula4x::write_text_file(events_summary_json_path, summary_json_text);
      if (!quiet) {
        std::ostream& info = script_stdout ? std::cerr : std::cout;
        info << "\nWrote events summary JSON to " << events_summary_json_path << "\n";
      }
    }
  } catch (const std::exception& e) {
    std::cerr << "Failed to export events summary JSON: " << e.what() << "\n";
    return 1;
  }
}

if (events_summary_csv) {
  try {
    const std::string summary_csv_text = nebula4x::events_summary_to_csv(filtered);
    if (events_summary_csv_path == "-") {
      // Explicit stdout export for scripting.
      std::cout << summary_csv_text;
    } else {
      nebula4x::write_text_file(events_summary_csv_path, summary_csv_text);
      if (!quiet) {
        std::ostream& info = script_stdout ? std::cerr : std::cout;
        info << "\nWrote events summary CSV to " << events_summary_csv_path << "\n";
      }
    }
  } catch (const std::exception& e) {
    std::cerr << "Failed to export events summary CSV: " << e.what() << "\n";
    return 1;
  }
}

      if (dump_events) {
        if (!quiet) std::cout << "\n";
        std::cout << "Events: " << filtered.size();
        if (!(allow_info && allow_warn && allow_error)) std::cout << " (levels=" << levels_raw << ")";
        if (has_cat) std::cout << " (category=" << event_category_label(cat_filter) << ")";
        if (has_since) std::cout << " (since=" << since_raw << ")";
        if (has_until) std::cout << " (until=" << until_raw << ")";
        if (fac_filter != nebula4x::kInvalidId) {
          const auto itf = s.factions.find(fac_filter);
          const std::string name = (itf != s.factions.end()) ? itf->second.name : std::string("(missing)");
          std::cout << " (faction=" << name << ")";
        }
        if (sys_filter != nebula4x::kInvalidId) {
          const auto its = s.systems.find(sys_filter);
          const std::string name = (its != s.systems.end()) ? its->second.name : std::string("(missing)");
          std::cout << " (system=" << name << ")";
        }
        if (ship_filter != nebula4x::kInvalidId) {
          const auto itsh = s.ships.find(ship_filter);
          const std::string name = (itsh != s.ships.end()) ? itsh->second.name : std::string("(missing)");
          std::cout << " (ship=" << name << ")";
        }
        if (col_filter != nebula4x::kInvalidId) {
          const auto itc = s.colonies.find(col_filter);
          const std::string name = (itc != s.colonies.end()) ? itc->second.name : std::string("(missing)");
          std::cout << " (colony=" << name << ")";
        }
        if (!contains_filter.empty()) std::cout << " (contains='" << contains_raw << "')";
        if (events_last > 0) std::cout << " (tail=" << events_last << ")";
        std::cout << "\n";

        if (filtered.empty()) {
          std::cout << "  (none)\n";
        } else {
          for (const auto* ev : filtered) {
            const nebula4x::Date d(ev->day);
            std::cout << "  [" << d.to_string() << "] #" << static_cast<unsigned long long>(ev->seq) << " ["
                      << event_category_label(ev->category) << "] "
                      << event_level_label(ev->level) << ": " << ev->message << "\n";
          }
        }
      }

      if (export_events_csv) {
        try {
          const std::string csv = nebula4x::events_to_csv(s, filtered);
          if (export_events_csv_path == "-") {
            // Explicit stdout export for scripting.
            std::cout << csv;
          } else {
            nebula4x::write_text_file(export_events_csv_path, csv);
            if (!quiet) {
              std::ostream& info = script_stdout ? std::cerr : std::cout;
              info << "\nWrote events CSV to " << export_events_csv_path << "\n";
            }
          }
        } catch (const std::exception& e) {
          std::cerr << "Failed to export events CSV: " << e.what() << "\n";
          return 1;
        }
      }

      if (export_events_json) {
        try {
          const std::string json_text = nebula4x::events_to_json(s, filtered);
          if (export_events_json_path == "-") {
            // Explicit stdout export for scripting.
            std::cout << json_text;
          } else {
            nebula4x::write_text_file(export_events_json_path, json_text);
            if (!quiet) {
              std::ostream& info = script_stdout ? std::cerr : std::cout;
              info << "\nWrote events JSON to " << export_events_json_path << "\n";
            }
          }
        } catch (const std::exception& e) {
          std::cerr << "Failed to export events JSON: " << e.what() << "\n";
          return 1;
        }
      }

      if (export_events_jsonl) {
        try {
          const std::string jsonl_text = nebula4x::events_to_jsonl(s, filtered);
          if (export_events_jsonl_path == "-") {
            // Explicit stdout export for scripting.
            std::cout << jsonl_text;
          } else {
            nebula4x::write_text_file(export_events_jsonl_path, jsonl_text);
            if (!quiet) {
              std::ostream& info = script_stdout ? std::cerr : std::cout;
              info << "\nWrote events JSONL to " << export_events_jsonl_path << "\n";
            }
          }
        } catch (const std::exception& e) {
          std::cerr << "Failed to export events JSONL: " << e.what() << "\n";
          return 1;
        }
      }
    }

    if (export_timeline_jsonl) {
      try {
        const std::string jsonl_text = nebula4x::timeline_snapshots_to_jsonl(timeline);
        if (export_timeline_jsonl_path == "-") {
          // Explicit stdout export for scripting.
          std::cout << jsonl_text;
        } else {
          nebula4x::write_text_file(export_timeline_jsonl_path, jsonl_text);
          if (!quiet) {
            std::ostream& info = script_stdout ? std::cerr : std::cout;
            info << "\nWrote timeline JSONL to " << export_timeline_jsonl_path << "\n";
          }
        }
      } catch (const std::exception& e) {
        std::cerr << "Failed to export timeline JSONL: " << e.what() << "\n";
        return 1;
      }
    }

    if (export_ships_json) {
      try {
        const std::string json_text = nebula4x::ships_to_json(s, &sim.content());
        if (export_ships_json_path == "-") {
          // Explicit stdout export for scripting.
          std::cout << json_text;
        } else {
          nebula4x::write_text_file(export_ships_json_path, json_text);
          if (!quiet) {
            std::ostream& info = script_stdout ? std::cerr : std::cout;
            info << "\nWrote ships JSON to " << export_ships_json_path << "\n";
          }
        }
      } catch (const std::exception& e) {
        std::cerr << "Failed to export ships JSON: " << e.what() << "\n";
        return 1;
      }
    }

    if (export_colonies_json) {
      try {
        const std::string json_text = nebula4x::colonies_to_json(s, &sim.content());
        if (export_colonies_json_path == "-") {
          // Explicit stdout export for scripting.
          std::cout << json_text;
        } else {
          nebula4x::write_text_file(export_colonies_json_path, json_text);
          if (!quiet) {
            std::ostream& info = script_stdout ? std::cerr : std::cout;
            info << "\nWrote colonies JSON to " << export_colonies_json_path << "\n";
          }
        }
      } catch (const std::exception& e) {
        std::cerr << "Failed to export colonies JSON: " << e.what() << "\n";
        return 1;
      }
    }

    if (export_fleets_json) {
      try {
        const std::string json_text = nebula4x::fleets_to_json(s);
        if (export_fleets_json_path == "-") {
          // Explicit stdout export for scripting.
          std::cout << json_text;
        } else {
          nebula4x::write_text_file(export_fleets_json_path, json_text);
          if (!quiet) {
            std::ostream& info = script_stdout ? std::cerr : std::cout;
            info << "\nWrote fleets JSON to " << export_fleets_json_path << "\n";
          }
        }
      } catch (const std::exception& e) {
        std::cerr << "Failed to export fleets JSON: " << e.what() << "\n";
        return 1;
      }
    }

    if (export_bodies_json) {
      try {
        const std::string json_text = nebula4x::bodies_to_json(s);
        if (export_bodies_json_path == "-") {
          // Explicit stdout export for scripting.
          std::cout << json_text;
        } else {
          nebula4x::write_text_file(export_bodies_json_path, json_text);
          if (!quiet) {
            std::ostream& info = script_stdout ? std::cerr : std::cout;
            info << "\nWrote bodies JSON to " << export_bodies_json_path << "\n";
          }
        }
      } catch (const std::exception& e) {
        std::cerr << "Failed to export bodies JSON: " << e.what() << "\n";
        return 1;
      }

      // --- Timeline exports (state/time-series) ---
      if (export_timeline_jsonl) {
        const std::string out = nebula4x::timeline_snapshots_to_jsonl(timeline);
        if (export_timeline_jsonl_path == "-") {
          std::cout << out;
        } else {
          nebula4x::write_text_file(export_timeline_jsonl_path, out);
          if (!quiet) {
            std::ostream& info = script_stdout ? std::cerr : std::cout;
            info << "Wrote timeline JSONL to " << export_timeline_jsonl_path << "\n";
          }
        }
      }
    }


    if (!save_path.empty()) {
      nebula4x::write_text_file(save_path, nebula4x::serialize_game_to_json(s));
      if (!quiet) {
        std::ostream& info = script_stdout ? std::cerr : std::cout;
        info << "\nSaved to " << save_path << "\n";
      }
    }

    if (has_flag(argc, argv, "--dump")) {
      std::cout << "\n--- JSON ---\n" << nebula4x::serialize_game_to_json(s) << "\n";
    }

    return 0;
  } catch (const std::exception& e) {
    nebula4x::log::error(std::string("Fatal: ") + e.what());
    return 1;
  }
}
