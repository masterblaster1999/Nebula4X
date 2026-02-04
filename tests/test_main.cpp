#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <vector>

int test_date();
int test_simulation();
int test_ground_ops();
int test_ground_battle_forecast();
int test_fleet_battle_forecast();
int test_boarding();
int test_serialization();
int test_auto_freight();
int test_freight_planner();
int test_freight_planner_partial_cargo();
int test_fuel_planner();
int test_repair_planner();
int test_trade_network();
int test_civilian_trade_activity_prosperity();
int test_security_planner();
int test_invasion_planner();
int test_industry();
int test_refit();
int test_diplomacy();
int test_piracy_suppression();
int test_auto_routing();
int test_jump_route_env_cost();
int test_auto_explore();
int test_order_repeat();
int test_order_planner();
int test_determinism();
int test_event_export();
int test_content_validation();
int test_resource_catalog();
int test_materials_processing();
int test_content_overlays();
int test_content_hot_reload();
int test_spatial_index();
int test_random_scenario();
int test_file_io();
int test_autosave();
int test_json_unicode();
int test_json_bom();
int test_json_errors();
int test_json_merge_patch();
int test_json_pointer();
int test_json_pointer_autocomplete();
int test_json_pointer_glob();
int test_trace_events();
int test_state_validation();
int test_save_diff();
int test_save_merge();
int test_save_delta();
int test_regression_tape();
int test_state_export();
int test_population_growth();
int test_combat_events();
int test_planetary_point_defense();
int test_shields();
int test_population_transport();
int test_colonization();
int test_auto_colonize();
int test_auto_salvage();
int test_reverse_engineering();
int test_anomalies();
int test_anomaly_discovery();
int test_missile_components();
int test_auto_tanker();
int test_auto_refuel();
int test_ship_repairs();
int test_crew_experience();
int test_electronic_warfare();
int test_procgen_surface();
int test_design_forge_constraints();
int test_nebula_microfields();
int test_nebula_storm_cells();
int test_jump_transit_hazards();
int test_dynamic_poi_spawns();
int test_fleets();
int test_ai_economy();
int test_ai_research_plan();
namespace nebula4x {
int test_ai_empire_fleet_missions();
int test_victory();
}  // namespace nebula4x
int test_research_planner();
int test_research_schedule();
int test_colony_schedule();
int test_colony_profiles();
int test_planner_events();
int test_time_warp();
int test_contact_prediction();
int test_sensor_coverage();
int test_swept_contacts();
int test_body_occlusion();
int test_mineral_deposits();
int test_mobile_mining();
int test_auto_mine();
int test_power_system();
int test_digests();
int test_faction_economy_modifiers();
int test_turn_ticks();
int test_intercept();
int test_duel_simulator();
int test_duel_tournament();
int test_duel_swiss_tournament();
int test_attack_lead_pursuit();
int test_lost_contact_search();
int test_combat_doctrine();
int test_advisor();

namespace {

struct TestCase {
  const char* name;
  int (*fn)();
};

struct TestRunResult {
  std::string name;
  int rc{0};
  double time_s{0.0};
  std::string captured_out;
  std::string captured_err;
};

std::string get_env_str(const char* key) {
#if defined(_MSC_VER)
  // MSVC warns that getenv() is unsafe. _dupenv_s allocates a copy that we own.
  char* buf = nullptr;
  size_t len = 0;
  if (_dupenv_s(&buf, &len, key) != 0 || !buf) return {};
  std::string out(buf);
  std::free(buf);
  return out;
#else
  const char* v = std::getenv(key);
  return v ? std::string(v) : std::string{};
#endif
}

bool env_flag(const char* key) {
  const std::string v = get_env_str(key);
  if (v.empty()) return false;
  if (v == "0" || v == "false" || v == "FALSE" || v == "off" || v == "OFF") return false;
  return true;
}

int env_int(const char* key, int def) {
  const std::string v = get_env_str(key);
  if (v.empty()) return def;
  try {
    return std::stoi(v);
  } catch (...) {
    return def;
  }
}

bool parse_int_strict(const std::string& s, int* out) {
  try {
    std::size_t idx = 0;
    const long long v = std::stoll(s, &idx, 10);
    if (idx != s.size()) return false;
    if (v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max()) return false;
    *out = static_cast<int>(v);
    return true;
  } catch (...) {
    return false;
  }
}

bool parse_uint_strict(const std::string& s, unsigned* out) {
  try {
    std::size_t idx = 0;
    const unsigned long long v = std::stoull(s, &idx, 10);
    if (idx != s.size()) return false;
    if (v > std::numeric_limits<unsigned>::max()) return false;
    *out = static_cast<unsigned>(v);
    return true;
  } catch (...) {
    return false;
  }
}

unsigned env_uint(const char* key, unsigned def) {
  const std::string v = get_env_str(key);
  if (v.empty()) return def;
  try {
    const long long x = std::stoll(v);
    return x <= 0 ? def : static_cast<unsigned>(x);
  } catch (...) {
    return def;
  }
}

bool contains_substr(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) return true;
  return haystack.find(needle) != std::string::npos;
}

std::string xml_escape(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (const char c : in) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '\"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

struct StreamCapture {
  bool enabled{false};
  std::streambuf* old_out{nullptr};
  std::streambuf* old_err{nullptr};
  std::ostringstream out_buf;
  std::ostringstream err_buf;

  explicit StreamCapture(bool enable) : enabled(enable) {
    if (!enabled) return;
    old_out = std::cout.rdbuf(out_buf.rdbuf());
    old_err = std::cerr.rdbuf(err_buf.rdbuf());
  }

  ~StreamCapture() {
    if (!enabled) return;
    std::cout.rdbuf(old_out);
    std::cerr.rdbuf(old_err);
  }
};

void print_usage(const char* argv0) {
  std::cout << "Nebula4X test runner\n\n";
  std::cout << "Usage: " << argv0 << " [options]\n\n";
  std::cout << "Options:\n";
  std::cout << "  -l, --list                 List all tests\n";
  std::cout << "  -f, --filter <substr>      Run only tests whose name contains <substr>\n";
  std::cout << "  -s, --shuffle              Shuffle test order\n";
  std::cout << "      --seed <n>             RNG seed for shuffle (0 = random)\n";
  std::cout << "  -r, --repeat <n>           Repeat selected tests n times (for flake hunting)\n";
  std::cout << "      --shard-count <n>      Split tests into N shards and run only one shard\n";
  std::cout << "      --shard-index <i>      Shard index in [0, N-1] (used with --shard-count)\n";
  std::cout << "      --junit <path>         Write JUnit XML report to <path>\n";
  std::cout << "  -x, --fail-fast            Stop on first failing test\n";
  std::cout << "  -v, --verbose              Print per-test timing and PASS/FAIL\n";
  std::cout << "  -h, --help                 Show this help\n\n";
  std::cout << "Env vars (defaults):\n";
  std::cout << "  N4X_TEST_FILTER, N4X_TEST_SHUFFLE, N4X_TEST_SEED, N4X_TEST_REPEAT,\n";
  std::cout << "  N4X_TEST_FAIL_FAST, N4X_TEST_VERBOSE, N4X_TEST_SHARD_COUNT, N4X_TEST_SHARD_INDEX,\n";
  std::cout << "  N4X_TEST_JUNIT\n";
}

} // namespace

int main(int argc, char** argv) {
  // Keep the canonical list in one place.
  const std::vector<TestCase> all = {
      {"date", test_date},
      {"simulation", test_simulation},
      {"ground_ops", test_ground_ops},
      {"ground_battle_forecast", test_ground_battle_forecast},
      {"fleet_battle_forecast", test_fleet_battle_forecast},
      {"boarding", test_boarding},
      {"serialization", test_serialization},
      {"auto_freight", test_auto_freight},
      {"freight_planner", test_freight_planner},
      {"freight_planner_partial_cargo", test_freight_planner_partial_cargo},
      {"trade_network", test_trade_network},
      {"civilian_trade_activity_prosperity", test_civilian_trade_activity_prosperity},
      {"security_planner", test_security_planner},
      {"invasion_planner", test_invasion_planner},
      {"fuel_planner", test_fuel_planner},
      {"repair_planner", test_repair_planner},
      {"industry", test_industry},
      {"refit", test_refit},
      {"diplomacy", test_diplomacy},
      {"piracy_suppression", test_piracy_suppression},
      {"auto_routing", test_auto_routing},
      {"jump_route_env_cost", test_jump_route_env_cost},
      {"auto_explore", test_auto_explore},
      {"order_repeat", test_order_repeat},
      {"order_planner", test_order_planner},
      {"determinism", test_determinism},
      {"event_export", test_event_export},
      {"content_validation", test_content_validation},
      {"resource_catalog", test_resource_catalog},
      {"materials_processing", test_materials_processing},
      {"content_overlays", test_content_overlays},
      {"content_hot_reload", test_content_hot_reload},
      {"spatial_index", test_spatial_index},
      {"random_scenario", test_random_scenario},
      {"ai_economy", test_ai_economy},
      {"ai_research_plan", test_ai_research_plan},
      {"ai_empire_fleet_missions", nebula4x::test_ai_empire_fleet_missions},
      {"victory", nebula4x::test_victory},
      {"research_planner", test_research_planner},
      {"research_schedule", test_research_schedule},
      {"colony_schedule", test_colony_schedule},
      {"colony_profiles", test_colony_profiles},
      {"planner_events", test_planner_events},
      {"time_warp", test_time_warp},
      {"contact_prediction", test_contact_prediction},
      {"sensor_coverage", test_sensor_coverage},
      {"swept_contacts", test_swept_contacts},
      {"body_occlusion", test_body_occlusion},
      {"mineral_deposits", test_mineral_deposits},
      {"mobile_mining", test_mobile_mining},
      {"auto_mine", test_auto_mine},
      {"power_system", test_power_system},
      {"digests", test_digests},
      {"autosave", test_autosave},
      {"file_io", test_file_io},
      {"json_unicode", test_json_unicode},
      {"json_bom", test_json_bom},
      {"json_errors", test_json_errors},
      {"json_merge_patch", test_json_merge_patch},
      {"json_pointer", test_json_pointer},
      {"json_pointer_autocomplete", test_json_pointer_autocomplete},
      {"json_pointer_glob", test_json_pointer_glob},
      {"trace_events", test_trace_events},
      {"state_validation", test_state_validation},
      {"save_diff", test_save_diff},
      {"save_merge", test_save_merge},
      {"save_delta", test_save_delta},
      {"regression_tape", test_regression_tape},
      {"state_export", test_state_export},
      {"fleets", test_fleets},
      {"population_growth", test_population_growth},
      {"population_transport", test_population_transport},
      {"colonization", test_colonization},
      {"auto_colonize", test_auto_colonize},
      {"auto_salvage", test_auto_salvage},
      {"reverse_engineering", test_reverse_engineering},
      {"anomalies", test_anomalies},
      {"anomaly_discovery", test_anomaly_discovery},
      {"procgen_surface", test_procgen_surface},
      {"design_forge_constraints", test_design_forge_constraints},
      {"nebula_microfields", test_nebula_microfields},
      {"nebula_storm_cells", test_nebula_storm_cells},
      {"jump_transit_hazards", test_jump_transit_hazards},
      {"dynamic_poi_spawns", test_dynamic_poi_spawns},
      {"missile_components", test_missile_components},
      {"auto_tanker", test_auto_tanker},
    {"auto_refuel", test_auto_refuel},
      {"combat_events", test_combat_events},
      {"planetary_point_defense", test_planetary_point_defense},
      {"shields", test_shields},
      {"turn_ticks", test_turn_ticks},
      {"intercept", test_intercept},
      {"duel_simulator", test_duel_simulator},
      {"duel_tournament", test_duel_tournament},
      {"duel_swiss_tournament", test_duel_swiss_tournament},
      {"attack_lead_pursuit", test_attack_lead_pursuit},
      {"lost_contact_search", test_lost_contact_search},
      {"combat_doctrine", test_combat_doctrine},
      {"ship_repairs", test_ship_repairs},
      {"crew_experience", test_crew_experience},
      {"electronic_warfare", test_electronic_warfare},
      {"faction_economy_modifiers", test_faction_economy_modifiers},
      {"advisor", test_advisor},
  };

  // Defaults from environment (convenient for CI/flakiness repro).
  bool list = false;
  bool shuffle = env_flag("N4X_TEST_SHUFFLE");
  bool fail_fast = env_flag("N4X_TEST_FAIL_FAST");
  bool verbose = env_flag("N4X_TEST_VERBOSE");
  unsigned seed = env_uint("N4X_TEST_SEED", 0);
  int repeat = std::max(1, env_int("N4X_TEST_REPEAT", 1));
  std::string filter = get_env_str("N4X_TEST_FILTER");
  const int shard_count_default = std::max(1, env_int("N4X_TEST_SHARD_COUNT", 1));
  const int shard_index_default = std::max(0, env_int("N4X_TEST_SHARD_INDEX", 0));
  int shard_count = shard_count_default;
  int shard_index = shard_index_default;
  std::string junit_path = get_env_str("N4X_TEST_JUNIT");

  // Parse arguments (override env defaults).
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      print_usage(argv[0]);
      return 0;
    }
    if (a == "-l" || a == "--list") {
      list = true;
      continue;
    }
    if (a == "-s" || a == "--shuffle") {
      shuffle = true;
      continue;
    }
    if (a == "-x" || a == "--fail-fast") {
      fail_fast = true;
      continue;
    }
    if (a == "-v" || a == "--verbose") {
      verbose = true;
      continue;
    }
    if (a == "--shard-count") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for --shard-count\n";
        return 2;
      }
      {
        int v = 1;
        if (!parse_int_strict(argv[++i], &v)) {
          std::cerr << "Invalid value for --shard-count\n";
          return 2;
        }
        shard_count = std::max(1, v);
      }
      continue;
    }
    if (a.rfind("--shard-count=", 0) == 0) {
      {
        int v = 1;
        if (!parse_int_strict(a.substr(std::string("--shard-count=").size()), &v)) {
          std::cerr << "Invalid value for --shard-count\n";
          return 2;
        }
        shard_count = std::max(1, v);
      }
      continue;
    }
    if (a == "--shard-index") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for --shard-index\n";
        return 2;
      }
      {
        int v = 0;
        if (!parse_int_strict(argv[++i], &v)) {
          std::cerr << "Invalid value for --shard-index\n";
          return 2;
        }
        shard_index = std::max(0, v);
      }
      continue;
    }
    if (a.rfind("--shard-index=", 0) == 0) {
      {
        int v = 0;
        if (!parse_int_strict(a.substr(std::string("--shard-index=").size()), &v)) {
          std::cerr << "Invalid value for --shard-index\n";
          return 2;
        }
        shard_index = std::max(0, v);
      }
      continue;
    }
    if (a == "--junit") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for --junit\n";
        return 2;
      }
      junit_path = argv[++i];
      continue;
    }
    if (a.rfind("--junit=", 0) == 0) {
      junit_path = a.substr(std::string("--junit=").size());
      continue;
    }
    if (a == "-f" || a == "--filter") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for --filter\n";
        return 2;
      }
      filter = argv[++i];
      continue;
    }
    if (a.rfind("--filter=", 0) == 0) {
      filter = a.substr(std::string("--filter=").size());
      continue;
    }
    if (a == "--seed") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for --seed\n";
        return 2;
      }
      {
        unsigned v = 0;
        if (!parse_uint_strict(argv[++i], &v)) {
          std::cerr << "Invalid value for --seed\n";
          return 2;
        }
        seed = v;
      }
      continue;
    }
    if (a.rfind("--seed=", 0) == 0) {
      {
        unsigned v = 0;
        if (!parse_uint_strict(a.substr(std::string("--seed=").size()), &v)) {
          std::cerr << "Invalid value for --seed\n";
          return 2;
        }
        seed = v;
      }
      continue;
    }
    if (a == "-r" || a == "--repeat") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for --repeat\n";
        return 2;
      }
      {
        int v = 1;
        if (!parse_int_strict(argv[++i], &v)) {
          std::cerr << "Invalid value for --repeat\n";
          return 2;
        }
        repeat = std::max(1, v);
      }
      continue;
    }
    if (a.rfind("--repeat=", 0) == 0) {
      {
        int v = 1;
        if (!parse_int_strict(a.substr(std::string("--repeat=").size()), &v)) {
          std::cerr << "Invalid value for --repeat\n";
          return 2;
        }
        repeat = std::max(1, v);
      }
      continue;
    }

    // Convenience: treat a single bare argument as a filter.
    if (filter.empty()) {
      filter = a;
    } else {
      std::cerr << "Unrecognized arg: " << a << "\n";
      std::cerr << "(Tip: use --help for options.)\n";
      return 2;
    }
  }

  if (list) {
    for (const auto& t : all) {
      std::cout << t.name << "\n";
    }
    return 0;
  }

  // Select tests.
  std::vector<TestCase> selected;
  selected.reserve(all.size());
  for (const auto& t : all) {
    if (contains_substr(t.name, filter)) selected.push_back(t);
  }

  if (selected.empty()) {
    std::cerr << "No tests matched filter: '" << filter << "'\n";
    return 1;
  }

  if (shard_count < 1) shard_count = 1;
  if (shard_index < 0) shard_index = 0;
  if (shard_index >= shard_count) {
    std::cerr << "Invalid shard index " << shard_index << " for shard count " << shard_count << "\n";
    return 2;
  }

  // Shuffle order if requested.
  unsigned actual_seed = seed;
  if (shuffle) {
    if (actual_seed == 0) {
      actual_seed = std::random_device{}();
    }
    std::mt19937 rng(actual_seed);
    std::shuffle(selected.begin(), selected.end(), rng);
  }

  // Apply sharding after selection and optional shuffle for predictable subsets.
  if (shard_count > 1) {
    std::vector<TestCase> sharded;
    sharded.reserve(selected.size());
    for (std::size_t i = 0; i < selected.size(); ++i) {
      if (static_cast<int>(i % static_cast<std::size_t>(shard_count)) == shard_index) {
        sharded.push_back(selected[i]);
      }
    }
    selected.swap(sharded);
  }

  if (verbose) {
    std::cout << "Running " << selected.size() << " test(s)";
    if (!filter.empty()) std::cout << " (filter='" << filter << "')";
    if (repeat > 1) std::cout << " x" << repeat;
    if (shuffle) std::cout << " (shuffled, seed=" << actual_seed << ")";
    if (shard_count > 1) std::cout << " (shard " << shard_index << "/" << shard_count << ")";
    if (!junit_path.empty()) std::cout << " (junit='" << junit_path << "')";
    std::cout << "\n";
  }

  if (selected.empty()) {
    // It's valid for a shard to be empty when the number of tests is not a multiple of shard_count.
    if (verbose && shard_count > 1) {
      std::cout << "No tests assigned to this shard (index=" << shard_index << ", count=" << shard_count << ")\n";
    }
    if (!junit_path.empty()) {
      try {
        std::filesystem::path p(junit_path);
        if (p.has_parent_path()) {
          std::filesystem::create_directories(p.parent_path());
        }
      } catch (...) {
        // Ignore: we'll still attempt to open the file and let it fail loudly.
      }

      std::ofstream ofs(junit_path, std::ios::binary);
      if (ofs) {
        ofs << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        ofs << "<testsuite name=\"nebula4x_tests\" tests=\"0\" failures=\"0\" errors=\"0\" time=\"0\">\n";
        ofs << "</testsuite>\n";
      }
    }
    return 0;
  }

  const bool capture = !junit_path.empty();
  std::vector<TestRunResult> results;
  if (capture) results.reserve(static_cast<std::size_t>(repeat) * selected.size());

  int fails = 0;
  const auto t0 = std::chrono::steady_clock::now();

  bool stop = false;
  for (int rep = 1; rep <= repeat && !stop; ++rep) {
    if (verbose && repeat > 1) {
      std::cout << "--- Repeat " << rep << "/" << repeat << " ---\n";
    }

    for (const auto& t : selected) {
      const auto start = std::chrono::steady_clock::now();
      int r = 0;
      std::string captured_out;
      std::string captured_err;
      {
        StreamCapture cap(capture);
        try {
          r = t.fn();
        } catch (const std::exception& e) {
          std::cerr << "Unhandled exception: " << e.what() << "\n";
          r = 1;
        } catch (...) {
          std::cerr << "Unhandled non-standard exception\n";
          r = 1;
        }
        if (capture) {
          captured_out = cap.out_buf.str();
          captured_err = cap.err_buf.str();
        }
      }
      const auto end = std::chrono::steady_clock::now();
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

      if (capture) {
        TestRunResult tr;
        tr.name = (repeat > 1) ? (std::string(t.name) + "#" + std::to_string(rep)) : std::string(t.name);
        tr.rc = r;
        tr.time_s = static_cast<double>(ms) / 1000.0;
        tr.captured_out = captured_out;
        tr.captured_err = captured_err;
        results.push_back(std::move(tr));
      }

      if (verbose) {
        std::cout << (r == 0 ? "PASS" : "FAIL") << "  " << t.name << "  (" << ms << " ms)\n";
      }

      if (capture && r != 0) {
        // Make failures actionable even when output is being captured for JUnit.
        if (!captured_err.empty()) std::cerr << captured_err;
        if (!captured_out.empty()) std::cerr << captured_out;
      }

      if (r != 0) {
        fails += r;
        if (fail_fast) {
          stop = true;
          break;
        }
      }
    }
  }

  const auto t1 = std::chrono::steady_clock::now();
  const auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

  if (!junit_path.empty()) {
    try {
      std::filesystem::path p(junit_path);
      if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path());
      }

      int failures = 0;
      for (const auto& r : results) {
        if (r.rc != 0) failures++;
      }

      std::ofstream ofs(junit_path, std::ios::binary);
      if (!ofs) {
        std::cerr << "Failed to open JUnit output file: " << junit_path << "\n";
      } else {
        const double total_s = static_cast<double>(total_ms) / 1000.0;
        ofs << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        ofs << "<testsuite name=\"nebula4x_tests\" tests=\"" << results.size() << "\"";
        ofs << " failures=\"" << failures << "\"";
        ofs << " errors=\"0\"";
        ofs << " time=\"" << total_s << "\">\n";

        for (const auto& r : results) {
          ofs << "  <testcase classname=\"nebula4x_tests\" name=\"" << xml_escape(r.name) << "\" time=\"" << r.time_s
              << "\">\n";
          if (r.rc != 0) {
            ofs << "    <failure message=\"rc=" << r.rc << "\">";
            const std::string combined = r.captured_err + r.captured_out;
            ofs << xml_escape(combined);
            ofs << "</failure>\n";
          }
          if (!r.captured_out.empty()) {
            ofs << "    <system-out>" << xml_escape(r.captured_out) << "</system-out>\n";
          }
          if (!r.captured_err.empty()) {
            ofs << "    <system-err>" << xml_escape(r.captured_err) << "</system-err>\n";
          }
          ofs << "  </testcase>\n";
        }

        ofs << "</testsuite>\n";
      }
    } catch (const std::exception& e) {
      std::cerr << "Failed to write JUnit report: " << e.what() << "\n";
    } catch (...) {
      std::cerr << "Failed to write JUnit report (unknown error)\n";
    }
  }

  if (fails == 0) {
    std::cout << "All tests passed (" << selected.size() << " test(s), " << total_ms << " ms)\n";
    return 0;
  }

  std::cerr << fails << " test(s) failed (" << total_ms << " ms)\n";
  if (shuffle) {
    std::cerr << "Repro: --shuffle --seed " << actual_seed;
    if (!filter.empty()) std::cerr << " --filter " << filter;
    if (repeat > 1) std::cerr << " --repeat " << repeat;
    std::cerr << "\n";
  }
  return 1;
}
