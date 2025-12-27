#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>
#include <vector>

#include "nebula4x/core/content_validation.h"
#include "nebula4x/core/serialization.h"
#include "nebula4x/core/scenario.h"
#include "nebula4x/core/simulation.h"
#include "nebula4x/core/tech.h"
#include "nebula4x/util/file_io.h"
#include "nebula4x/util/log.h"
#include "nebula4x/util/strings.h"

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
  }
  return "GENERAL";
}

std::string to_lower(std::string s) {
  for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

bool is_digits(const std::string& s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}

bool parse_event_category(const std::string& raw, nebula4x::EventCategory& out) {
  const std::string s = to_lower(raw);
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
  const std::string s = to_lower(raw);
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


nebula4x::Id resolve_faction_id(const nebula4x::GameState& s, const std::string& raw) {
  if (raw.empty()) return nebula4x::kInvalidId;

  // Numeric id.
  if (is_digits(raw)) {
    const auto id = static_cast<nebula4x::Id>(std::stoll(raw));
    if (s.factions.find(id) != s.factions.end()) return id;
    return nebula4x::kInvalidId;
  }

  // Name match (case-insensitive).
  const std::string want = to_lower(raw);
  for (const auto& [id, f] : s.factions) {
    if (to_lower(f.name) == want) return id;
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
  std::cout << "  --content PATH   Content blueprints JSON (default: data/blueprints/starting_blueprints.json)\n";
  std::cout << "  --tech PATH      Tech tree JSON (default: data/tech/tech_tree.json)\n";
  std::cout << "  --load PATH      Load a save JSON before advancing\n";
  std::cout << "  --save PATH      Save state JSON after advancing\n";
  std::cout << "  --format-save    Load + re-save (canonicalize JSON) without advancing\n";
  std::cout << "  --validate-content  Validate content + tech files and exit\n";
  std::cout << "  --dump           Print the resulting save JSON to stdout\n";
  std::cout << "  --quiet          Suppress non-essential summary/status output (useful for scripts)\n";
  std::cout << "  --dump-events    Print the persistent simulation event log to stdout\n";
  std::cout << "  --export-events-csv PATH  Export the persistent simulation event log to CSV\n";
  std::cout << "    --events-last N         Only print the last N matching events (0 = all)\n";
  std::cout << "    --events-category NAME  Filter by category (general|research|shipyard|construction|movement|combat|intel|exploration)\n";
  std::cout << "    --events-faction X      Filter by faction id or exact name (case-insensitive)\n";
  std::cout << "    --events-contains TEXT  Filter by message substring (case-insensitive)\n";
  std::cout << "    --events-level LEVELS  Filter by level (all|info|warn|error or comma-separated list)\n";
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

    const int days = get_int_arg(argc, argv, "--days", 30);
    const int until_event_days = get_int_arg(argc, argv, "--until-event", -1);
    const bool until_event = (until_event_days != -1);
    const std::string scenario = get_str_arg(argc, argv, "--scenario", "sol");
    const int seed = get_int_arg(argc, argv, "--seed", 1);
    const int systems = get_int_arg(argc, argv, "--systems", 12);
    const std::string content_path = get_str_arg(argc, argv, "--content", "data/blueprints/starting_blueprints.json");
    const std::string tech_path = get_str_arg(argc, argv, "--tech", "data/tech/tech_tree.json");
    const std::string load_path = get_str_arg(argc, argv, "--load", "");
    const std::string save_path = get_str_arg(argc, argv, "--save", "");
    const std::string export_events_csv_path = get_str_arg(argc, argv, "--export-events-csv", "");

    const bool quiet = has_flag(argc, argv, "--quiet");

    const bool format_save = has_flag(argc, argv, "--format-save");
    const bool validate_content = has_flag(argc, argv, "--validate-content");

    if (format_save) {
      if (load_path.empty() || save_path.empty()) {
        std::cerr << "--format-save requires both --load and --save\n\n";
        print_usage(argv[0]);
        return 2;
      }

      const auto loaded = nebula4x::deserialize_game_from_json(nebula4x::read_text_file(load_path));
      nebula4x::write_text_file(save_path, nebula4x::serialize_game_to_json(loaded));
      if (!quiet) std::cout << "Formatted save written to " << save_path << "\n";
      return 0;
    }

    auto content = nebula4x::load_content_db_from_file(content_path);
    content.techs = nebula4x::load_tech_db_from_file(tech_path);

    if (validate_content) {
      const auto errors = nebula4x::validate_content_db(content);
      if (!errors.empty()) {
        std::cerr << "Content validation failed:\n";
        for (const auto& e : errors) std::cerr << "  - " << e << "\n";
        return 1;
      }
      if (!quiet) std::cout << "Content OK\n";
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

      stop.message_contains = get_str_arg(argc, argv, "--events-contains", "");

      until_res = sim.advance_until_event(until_event_days, stop);
    } else {
      sim.advance_days(days);
    }

    const auto& s = sim.state();
    if (!quiet) {
    std::cout << "Date: " << s.date.to_string() << "\n";
    std::cout << "Systems: " << s.systems.size() << ", Bodies: " << s.bodies.size() << ", Jump Points: "
              << s.jump_points.size() << ", Ships: " << s.ships.size() << ", Colonies: " << s.colonies.size() << "\n";

    for (const auto& [_, c] : s.colonies) {
      std::cout << "\nColony " << c.name << " minerals:\n";
      for (const auto& [k, v] : c.minerals) {
        std::cout << "  " << k << ": " << v << "\n";
      }
    }

    }

    if (until_event) {
      if (until_res.hit) {
        const nebula4x::Date d(until_res.event.day);
        std::cout << "\nUntil-event: hit after " << until_res.days_advanced << " days -> [" << d.to_string()
                  << "] #" << static_cast<unsigned long long>(until_res.event.seq) << " ["
                  << event_category_label(until_res.event.category) << "] "
                  << event_level_label(until_res.event.level) << ": " << until_res.event.message << "\n";
      } else {
        std::cout << "\nUntil-event: no matching event within " << until_event_days << " days (advanced "
                  << until_res.days_advanced << ", date now " << s.date.to_string() << ")\n";
      }
    }

    const bool dump_events = has_flag(argc, argv, "--dump-events");
    const bool export_events_csv = !export_events_csv_path.empty();

    if (dump_events || export_events_csv) {
      const int events_last = std::max(0, get_int_arg(argc, argv, "--events-last", 0));
      const std::string cat_raw = get_str_arg(argc, argv, "--events-category", "");
      const std::string fac_raw = get_str_arg(argc, argv, "--events-faction", "");
      const std::string contains_raw = get_str_arg(argc, argv, "--events-contains", "");
      const std::string contains_filter = to_lower(contains_raw);
      const std::string levels_raw = get_str_arg(argc, argv, "--events-level", "all");
      bool allow_info = true;
      bool allow_warn = true;
      bool allow_error = true;
      if (!parse_event_levels(levels_raw, allow_info, allow_warn, allow_error)) {
        std::cerr << "Unknown --events-level: " << levels_raw << "\n";
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

      std::vector<const nebula4x::SimEvent*> filtered;
      filtered.reserve(s.events.size());
      for (const auto& ev : s.events) {
        if (ev.level == nebula4x::EventLevel::Info && !allow_info) continue;
        if (ev.level == nebula4x::EventLevel::Warn && !allow_warn) continue;
        if (ev.level == nebula4x::EventLevel::Error && !allow_error) continue;
        if (has_cat && ev.category != cat_filter) continue;
        if (fac_filter != nebula4x::kInvalidId && ev.faction_id != fac_filter && ev.faction_id2 != fac_filter) continue;
        if (!contains_filter.empty()) {
          if (to_lower(ev.message).find(contains_filter) == std::string::npos) continue;
        }
        filtered.push_back(&ev);
      }

      if (events_last > 0 && (int)filtered.size() > events_last) {
        filtered.erase(filtered.begin(), filtered.end() - events_last);
      }

      if (dump_events) {
        std::cout << "\nEvents: " << filtered.size();
        if (!(allow_info && allow_warn && allow_error)) std::cout << " (levels=" << levels_raw << ")";
        if (has_cat) std::cout << " (category=" << event_category_label(cat_filter) << ")";
        if (fac_filter != nebula4x::kInvalidId) {
          const auto itf = s.factions.find(fac_filter);
          const std::string name = (itf != s.factions.end()) ? itf->second.name : std::string("(missing)");
          std::cout << " (faction=" << name << ")";
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
          auto faction_name = [&](nebula4x::Id id) -> std::string {
            if (id == nebula4x::kInvalidId) return {};
            const auto it = s.factions.find(id);
            return (it != s.factions.end()) ? it->second.name : std::string{};
          };
          auto system_name = [&](nebula4x::Id id) -> std::string {
            if (id == nebula4x::kInvalidId) return {};
            const auto it = s.systems.find(id);
            return (it != s.systems.end()) ? it->second.name : std::string{};
          };
          auto ship_name = [&](nebula4x::Id id) -> std::string {
            if (id == nebula4x::kInvalidId) return {};
            const auto it = s.ships.find(id);
            return (it != s.ships.end()) ? it->second.name : std::string{};
          };
          auto colony_name = [&](nebula4x::Id id) -> std::string {
            if (id == nebula4x::kInvalidId) return {};
            const auto it = s.colonies.find(id);
            return (it != s.colonies.end()) ? it->second.name : std::string{};
          };

          std::string csv;
          csv += "day,date,seq,level,category,"
                 "faction_id,faction,"
                 "faction_id2,faction2,"
                 "system_id,system,"
                 "ship_id,ship,"
                 "colony_id,colony,"
                 "message\n";

          for (const auto* ev : filtered) {
            const nebula4x::Date d(ev->day);

            csv += std::to_string(static_cast<long long>(ev->day));
            csv += ",";
            csv += nebula4x::csv_escape(d.to_string());
            csv += ",";
            csv += std::to_string(static_cast<unsigned long long>(ev->seq));
            csv += ",";
            csv += nebula4x::csv_escape(std::string(event_level_label(ev->level)));
            csv += ",";
            csv += nebula4x::csv_escape(std::string(event_category_label(ev->category)));
            csv += ",";
            csv += std::to_string(static_cast<unsigned long long>(ev->faction_id));
            csv += ",";
            csv += nebula4x::csv_escape(faction_name(ev->faction_id));
            csv += ",";
            csv += std::to_string(static_cast<unsigned long long>(ev->faction_id2));
            csv += ",";
            csv += nebula4x::csv_escape(faction_name(ev->faction_id2));
            csv += ",";
            csv += std::to_string(static_cast<unsigned long long>(ev->system_id));
            csv += ",";
            csv += nebula4x::csv_escape(system_name(ev->system_id));
            csv += ",";
            csv += std::to_string(static_cast<unsigned long long>(ev->ship_id));
            csv += ",";
            csv += nebula4x::csv_escape(ship_name(ev->ship_id));
            csv += ",";
            csv += std::to_string(static_cast<unsigned long long>(ev->colony_id));
            csv += ",";
            csv += nebula4x::csv_escape(colony_name(ev->colony_id));
            csv += ",";
            csv += nebula4x::csv_escape(ev->message);
            csv += "\n";
          }

          nebula4x::write_text_file(export_events_csv_path, csv);
          if (!quiet) std::cout << "\nWrote events CSV to " << export_events_csv_path << "\n";
        } catch (const std::exception& e) {
          std::cerr << "Failed to export events CSV: " << e.what() << "\n";
          return 1;
        }
      }
    }


    if (!save_path.empty()) {
      nebula4x::write_text_file(save_path, nebula4x::serialize_game_to_json(s));
      if (!quiet) std::cout << "\nSaved to " << save_path << "\n";
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
