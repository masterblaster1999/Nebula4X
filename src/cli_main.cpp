#include <iostream>
#include <string>

#include "nebula4x/core/serialization.h"
#include "nebula4x/core/simulation.h"
#include "nebula4x/core/tech.h"
#include "nebula4x/util/file_io.h"
#include "nebula4x/util/log.h"

namespace {

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

bool has_flag(int argc, char** argv, const std::string& flag) {
  for (int i = 1; i < argc; ++i) {
    if (argv[i] == flag) return true;
  }
  return false;
}

} // namespace

int main(int argc, char** argv) {
  try {
    const int days = get_int_arg(argc, argv, "--days", 30);
    const std::string content_path = get_str_arg(argc, argv, "--content", "data/blueprints/starting_blueprints.json");
    const std::string load_path = get_str_arg(argc, argv, "--load", "");
    const std::string save_path = get_str_arg(argc, argv, "--save", "");

    auto content = nebula4x::load_content_db_from_file(content_path);
    nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});

    if (!load_path.empty()) {
      sim.load_game(nebula4x::deserialize_game_from_json(nebula4x::read_text_file(load_path)));
    }

    sim.advance_days(days);

    const auto& s = sim.state();
    std::cout << "Date: " << s.date.to_string() << "\n";
    std::cout << "Systems: " << s.systems.size() << ", Bodies: " << s.bodies.size() << ", Ships: " << s.ships.size()
              << ", Colonies: " << s.colonies.size() << "\n";

    for (const auto& [_, c] : s.colonies) {
      std::cout << "\nColony " << c.name << " minerals:\n";
      for (const auto& [k, v] : c.minerals) {
        std::cout << "  " << k << ": " << v << "\n";
      }
    }

    if (!save_path.empty()) {
      nebula4x::write_text_file(save_path, nebula4x::serialize_game_to_json(s));
      std::cout << "\nSaved to " << save_path << "\n";
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
