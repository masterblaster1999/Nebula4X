#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <vector>

int test_date();
int test_simulation();
int test_ground_ops();
int test_ground_battle_forecast();
int test_boarding();
int test_serialization();
int test_auto_freight();
int test_freight_planner();
int test_fuel_planner();
int test_industry();
int test_refit();
int test_diplomacy();
int test_auto_routing();
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
int test_save_delta();
int test_regression_tape();
int test_state_export();
int test_population_growth();
int test_combat_events();
int test_shields();
int test_population_transport();
int test_colonization();
int test_auto_colonize();
int test_auto_salvage();
int test_reverse_engineering();
int test_anomalies();
int test_missile_components();
int test_auto_tanker();
int test_ship_repairs();
int test_crew_experience();
int test_electronic_warfare();
int test_fleets();
int test_ai_economy();
int test_research_planner();
int test_research_schedule();
int test_colony_schedule();
int test_colony_profiles();
int test_planner_events();
int test_time_warp();
int test_contact_prediction();
int test_sensor_coverage();
int test_swept_contacts();
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
int test_combat_doctrine();
int test_advisor();

namespace {

struct TestCase {
  const char* name;
  int (*fn)();
};

std::string get_env_str(const char* key) {
  const char* v = std::getenv(key);
  return v ? std::string(v) : std::string{};
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

void print_usage(const char* argv0) {
  std::cout << "Nebula4X test runner\n\n";
  std::cout << "Usage: " << argv0 << " [options]\n\n";
  std::cout << "Options:\n";
  std::cout << "  -l, --list                 List all tests\n";
  std::cout << "  -f, --filter <substr>      Run only tests whose name contains <substr>\n";
  std::cout << "  -s, --shuffle              Shuffle test order\n";
  std::cout << "      --seed <n>             RNG seed for shuffle (0 = random)\n";
  std::cout << "  -r, --repeat <n>           Repeat selected tests n times (for flake hunting)\n";
  std::cout << "  -x, --fail-fast            Stop on first failing test\n";
  std::cout << "  -v, --verbose              Print per-test timing and PASS/FAIL\n";
  std::cout << "  -h, --help                 Show this help\n\n";
  std::cout << "Env vars (defaults):\n";
  std::cout << "  N4X_TEST_FILTER, N4X_TEST_SHUFFLE, N4X_TEST_SEED, N4X_TEST_REPEAT,\n";
  std::cout << "  N4X_TEST_FAIL_FAST, N4X_TEST_VERBOSE\n";
}

} // namespace

int main(int argc, char** argv) {
  // Keep the canonical list in one place.
  const std::vector<TestCase> all = {
      {"date", test_date},
      {"simulation", test_simulation},
      {"ground_ops", test_ground_ops},
      {"ground_battle_forecast", test_ground_battle_forecast},
      {"boarding", test_boarding},
      {"serialization", test_serialization},
      {"auto_freight", test_auto_freight},
      {"freight_planner", test_freight_planner},
      {"fuel_planner", test_fuel_planner},
      {"industry", test_industry},
      {"refit", test_refit},
      {"diplomacy", test_diplomacy},
      {"auto_routing", test_auto_routing},
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
      {"research_planner", test_research_planner},
      {"research_schedule", test_research_schedule},
      {"colony_schedule", test_colony_schedule},
      {"colony_profiles", test_colony_profiles},
      {"planner_events", test_planner_events},
      {"time_warp", test_time_warp},
      {"contact_prediction", test_contact_prediction},
      {"sensor_coverage", test_sensor_coverage},
      {"swept_contacts", test_swept_contacts},
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
      {"missile_components", test_missile_components},
      {"auto_tanker", test_auto_tanker},
      {"combat_events", test_combat_events},
      {"shields", test_shields},
      {"turn_ticks", test_turn_ticks},
      {"intercept", test_intercept},
      {"duel_simulator", test_duel_simulator},
      {"duel_tournament", test_duel_tournament},
      {"duel_swiss_tournament", test_duel_swiss_tournament},
      {"attack_lead_pursuit", test_attack_lead_pursuit},
      {"combat_doctrine", test_combat_doctrine},
      {"ship_repairs", test_ship_repairs},
      {"crew_experience", test_crew_experience},
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
      seed = static_cast<unsigned>(std::stoul(argv[++i]));
      continue;
    }
    if (a.rfind("--seed=", 0) == 0) {
      seed = static_cast<unsigned>(std::stoul(a.substr(std::string("--seed=").size())));
      continue;
    }
    if (a == "-r" || a == "--repeat") {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for --repeat\n";
        return 2;
      }
      repeat = std::max(1, std::stoi(argv[++i]));
      continue;
    }
    if (a.rfind("--repeat=", 0) == 0) {
      repeat = std::max(1, std::stoi(a.substr(std::string("--repeat=").size())));
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

  // Shuffle order if requested.
  unsigned actual_seed = seed;
  if (shuffle) {
    if (actual_seed == 0) {
      actual_seed = std::random_device{}();
    }
    std::mt19937 rng(actual_seed);
    std::shuffle(selected.begin(), selected.end(), rng);
  }

  if (verbose) {
    std::cout << "Running " << selected.size() << " test(s)";
    if (!filter.empty()) std::cout << " (filter='" << filter << "')";
    if (repeat > 1) std::cout << " x" << repeat;
    if (shuffle) std::cout << " (shuffled, seed=" << actual_seed << ")";
    std::cout << "\n";
  }

  int fails = 0;
  const auto t0 = std::chrono::steady_clock::now();

  bool stop = false;
  for (int rep = 1; rep <= repeat && !stop; ++rep) {
    if (verbose && repeat > 1) {
      std::cout << "--- Repeat " << rep << "/" << repeat << " ---\n";
    }

    for (const auto& t : selected) {
      const auto start = std::chrono::steady_clock::now();
      const int r = t.fn();
      const auto end = std::chrono::steady_clock::now();
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

      if (verbose) {
        std::cout << (r == 0 ? "PASS" : "FAIL") << "  " << t.name << "  (" << ms << " ms)\n";
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
