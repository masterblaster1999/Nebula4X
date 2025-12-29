#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "nebula4x/core/tech.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";            \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

static bool approx(double a, double b, double eps = 1e-9) {
  return std::abs(a - b) <= eps;
}

int test_content_overlays() {
  // --- Multi-root overlay loading ---
  {
    std::vector<std::string> paths = {"tests/data/content_base.json", "tests/data/content_mod.json"};
    auto content = nebula4x::load_content_db_from_files(paths);

    N4X_ASSERT(content.components.find("engine_test") != content.components.end());
    N4X_ASSERT(approx(content.components.at("engine_test").speed_km_s, 9.0));

    N4X_ASSERT(content.designs.find("ship_test") != content.designs.end());
    const auto& d = content.designs.at("ship_test");

    bool has_engine = false;
    bool has_sensor = false;
    bool has_cargo = false;
    for (const auto& cid : d.components) {
      if (cid == "engine_test") has_engine = true;
      if (cid == "sensor_test") has_sensor = true;
      if (cid == "cargo_test") has_cargo = true;
    }
    N4X_ASSERT(has_engine);
    N4X_ASSERT(has_sensor);
    N4X_ASSERT(!has_cargo);

    // Speed comes from the (patched) engine.
    N4X_ASSERT(approx(d.speed_km_s, 9.0));
    // Cargo should be removed (cargo component removed from design).
    N4X_ASSERT(approx(d.cargo_tons, 0.0));
    // Sensor range should be provided by the added sensor.
    N4X_ASSERT(approx(d.sensor_range_mkm, 123.0));
  }

  // --- include/includes directive ---
  {
    auto content = nebula4x::load_content_db_from_file("tests/data/content_root_include.json");

    N4X_ASSERT(content.components.find("engine_test") != content.components.end());
    N4X_ASSERT(approx(content.components.at("engine_test").speed_km_s, 9.0));

    N4X_ASSERT(content.designs.find("ship_test") != content.designs.end());
    const auto& d = content.designs.at("ship_test");

    bool has_engine = false;
    bool has_sensor = false;
    bool has_cargo = false;
    for (const auto& cid : d.components) {
      if (cid == "engine_test") has_engine = true;
      if (cid == "sensor_test") has_sensor = true;
      if (cid == "cargo_test") has_cargo = true;
    }
    N4X_ASSERT(has_engine);
    N4X_ASSERT(has_sensor);
    N4X_ASSERT(!has_cargo);

    N4X_ASSERT(approx(d.speed_km_s, 9.0));
    N4X_ASSERT(approx(d.cargo_tons, 0.0));
    N4X_ASSERT(approx(d.sensor_range_mkm, 123.0));
  }

  // --- Tech overlays ---
  {
    std::vector<std::string> paths = {"tests/data/tech_base.json", "tests/data/tech_mod.json"};
    auto techs = nebula4x::load_tech_db_from_files(paths);

    N4X_ASSERT(techs.find("t0") != techs.end());
    N4X_ASSERT(techs.find("t2") != techs.end());

    const auto& t2 = techs.at("t2");
    N4X_ASSERT(approx(t2.cost, 250.0));

    bool has_t1 = false;
    bool has_t0 = false;
    for (const auto& p : t2.prereqs) {
      if (p == "t1") has_t1 = true;
      if (p == "t0") has_t0 = true;
    }
    N4X_ASSERT(has_t1);
    N4X_ASSERT(has_t0);
  }

  // --- Tech include/includes directive ---
  {
    auto techs = nebula4x::load_tech_db_from_file("tests/data/tech_root_include.json");

    N4X_ASSERT(techs.find("t0") != techs.end());
    N4X_ASSERT(techs.find("t2") != techs.end());

    const auto& t2 = techs.at("t2");
    N4X_ASSERT(approx(t2.cost, 250.0));

    bool has_t1 = false;
    bool has_t0 = false;
    for (const auto& p : t2.prereqs) {
      if (p == "t1") has_t1 = true;
      if (p == "t0") has_t0 = true;
    }
    N4X_ASSERT(has_t1);
    N4X_ASSERT(has_t0);
  }

  return 0;
}
