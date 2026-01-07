#include "nebula4x/core/scenario.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

#include "nebula4x/core/date.h"
#include "nebula4x/util/delaunay.h"

namespace nebula4x {

GameState make_sol_scenario() {
  GameState s;
  // Bump when the save schema changes.
  // v8: adds WaitDays order type.
  // v9: adds ShipOrders repeat fields (repeat + repeat_template).
  // v10: adds persistent GameState::events (simulation event log).
  // v11: adds structured event fields (category + context ids).
  // v12: adds SimEvent::seq + GameState::next_event_seq (monotonic event ids).
  // v22: adds Body::mineral_deposits (finite mining / depletion).
  // v23: adds ship fuel tanks + fuel consumption (logistics).
  // v26: adds hierarchical orbits (Body::parent_body_id) + optional physical metadata.
  // v27: adds eccentric orbits + BodyType::Comet.
  // v43: adds StarSystem::nebula_density (system-level environmental effects).
  // v44: adds procedural Regions + StarSystem::region_id (galaxy sectors).
  // New games should start at the current save schema version.
  s.save_version = GameState{}.save_version;
  s.date = Date::from_ymd(2200, 1, 1);

  // --- Factions ---
  const Id terrans = allocate_id(s);
  {
    Faction f;
    f.id = terrans;
    f.name = "Terran Union";
    f.research_points = 0.0;
    f.known_techs = {"chemistry_1"};
    f.research_queue = {"nuclear_1", "propulsion_1", "colonization_1"};
    s.factions[terrans] = f;
  }

  const Id pirates = allocate_id(s);
  {
    Faction f;
    f.id = pirates;
    f.name = "Pirate Raiders";
    f.control = FactionControl::AI_Pirate;
    f.research_points = 0.0;
    s.factions[pirates] = f;
  }

  // --- Systems ---
  const Id sol = allocate_id(s);
  {
    StarSystem system;
    system.id = sol;
    system.name = "Sol";
    system.galaxy_pos = {0.0, 0.0};
    s.systems[sol] = system;
  }

  const Id centauri = allocate_id(s);
  {
    StarSystem system;
    system.id = centauri;
    system.name = "Alpha Centauri";
    system.galaxy_pos = {4.3, 0.0};
    s.systems[centauri] = system;
  }

  // A third system to make the early exploration loop more interesting.
  const Id barnard = allocate_id(s);
  {
    StarSystem system;
    system.id = barnard;
    system.name = "Barnard's Star";
    system.galaxy_pos = {6.0, -1.4};
    s.systems[barnard] = system;
  }

  s.selected_system = sol;

  auto add_body = [&](Id system_id, const std::string& name, BodyType type, double a_mkm, double period_days,
                      double mean_anomaly, double eccentricity = 0.0, double arg_periapsis = 0.0) {
    const Id id = allocate_id(s);
    Body b;
    b.id = id;
    b.name = name;
    b.type = type;
    b.system_id = system_id;
    b.orbit_radius_mkm = a_mkm;
    b.orbit_period_days = period_days;
    b.orbit_phase_radians = mean_anomaly;
    b.orbit_eccentricity = eccentricity;
    b.orbit_arg_periapsis_radians = arg_periapsis;
    s.bodies[id] = b;
    s.systems[system_id].bodies.push_back(id);
    return id;
  };

  // --- Sol bodies ---
  (void)add_body(sol, "Sun", BodyType::Star, 0.0, 1.0, 0.0);
  const Id earth = add_body(sol, "Earth", BodyType::Planet, 149.6, 365.25, 0.0);
  const Id mars = add_body(sol, "Mars", BodyType::Planet, 227.9, 686.98, 1.0);
  (void)add_body(sol, "Jupiter", BodyType::GasGiant, 778.5, 4332.6, 2.0);

  // Basic environmental defaults + a starter terraforming target for Mars.
  {
    auto& e = s.bodies.at(earth);
    e.surface_temp_k = 288.0;
    e.atmosphere_atm = 1.0;
    e.terraforming_target_temp_k = 288.0;
    e.terraforming_target_atm = 1.0;
    e.terraforming_complete = true;

    auto& m = s.bodies.at(mars);
    m.surface_temp_k = 210.0;
    m.atmosphere_atm = 0.006;
    // Prototype target: "Earthlike".
    m.terraforming_target_temp_k = 288.0;
    m.terraforming_target_atm = 0.8;
    m.terraforming_complete = false;
  }

  // Minor bodies (prototype): a modest asteroid belt and a short-period comet.
  //
  // This keeps the Sol start interesting for mining and demonstrates
  // non-circular (eccentric) orbits without blowing out the map scale.
  auto period_solar = [](double a_mkm) {
    const double a_au = a_mkm / 149.6;
    const double years = std::sqrt(std::max(0.0, a_au * a_au * a_au));
    return std::max(1.0, years * 365.25);
  };

  auto eq_temp_solar = [](double dist_au, double albedo) {
    const double d = std::max(0.05, dist_au);
    const double a = std::clamp(albedo, 0.0, 0.95);
    const double t = 278.0 / std::sqrt(d) * std::pow(1.0 - a, 0.25);
    return std::clamp(t, 30.0, 2000.0);
  };

  std::vector<Id> sol_belt;
  sol_belt.reserve(12);
  for (int i = 0; i < 12; ++i) {
    const double rr = 414.0 + (static_cast<double>(i) - 5.5) * 3.0; // ~2.7 AU belt band
    const double ph = 0.35 * static_cast<double>(i);
    const Id aid = add_body(sol, "Asteroid " + std::to_string(i + 1), BodyType::Asteroid, rr, period_solar(rr), ph);
    sol_belt.push_back(aid);
    auto& b = s.bodies.at(aid);
    b.radius_km = 50.0 + 15.0 * (i % 5);
    b.surface_temp_k = eq_temp_solar(rr / 149.6, 0.08);
  }

  // A short-period comet with a noticeable eccentricity but a manageable aphelion.
  const double encke_a = 332.0; // ~2.22 AU
  const Id encke =
      add_body(sol, "Comet Encke", BodyType::Comet, encke_a, period_solar(encke_a), 0.7, 0.85, 1.2);
  {
    auto& b = s.bodies.at(encke);
    b.radius_km = 6.0;
    // Temperature shown is a rough perihelion equilibrium estimate.
    const double q_au = (encke_a / 149.6) * (1.0 - 0.85);
    b.surface_temp_k = eq_temp_solar(q_au, 0.04);
  }

  // --- Alpha Centauri bodies ---
  (void)add_body(centauri, "Alpha Centauri A", BodyType::Star, 0.0, 1.0, 0.0);
  const Id centauri_prime = add_body(centauri, "Centauri Prime", BodyType::Planet, 110.0, 320.0, 0.4);

  // --- Barnard's Star bodies ---
  (void)add_body(barnard, "Barnard's Star", BodyType::Star, 0.0, 1.0, 0.0);
  const Id barnard_b = add_body(barnard, "Barnard b", BodyType::Planet, 60.0, 233.0, 0.2);

  // Environment for non-Sol colonies (prototype defaults).
  {
    auto& cp = s.bodies.at(centauri_prime);
    cp.surface_temp_k = 285.0;
    cp.atmosphere_atm = 0.95;
    cp.terraforming_target_temp_k = cp.surface_temp_k;
    cp.terraforming_target_atm = cp.atmosphere_atm;
    cp.terraforming_complete = true;

    auto& bb = s.bodies.at(barnard_b);
    bb.surface_temp_k = 240.0;
    bb.atmosphere_atm = 0.20;
    bb.terraforming_target_temp_k = 288.0;
    bb.terraforming_target_atm = 1.0;
    bb.terraforming_complete = false;
  }

  // --- Mineral deposits (finite mining) ---
  //
  // Newer versions of the prototype support finite mineral deposits attached to
  // bodies. Mines extract from these deposits each day.
  auto seed_standard_deposits = [&](Id body_id, double duranium_tons, double sorium_multiplier = 1.0) {
    auto& b = s.bodies.at(body_id);
    auto& d = b.mineral_deposits;

    d["Duranium"] = std::max(0.0, duranium_tons);
    d["Neutronium"] = std::max(0.0, duranium_tons * 0.10);

    // Terra Novan-style mineral roster (prototype). These ratios are tuned for
    // gameplay rather than strict canon realism.
    d["Tritanium"] = std::max(0.0, duranium_tons * 0.08);
    d["Boronide"] = std::max(0.0, duranium_tons * 0.07);
    d["Corbomite"] = std::max(0.0, duranium_tons * 0.05);
    d["Mercassium"] = std::max(0.0, duranium_tons * 0.06);
    d["Vendarite"] = std::max(0.0, duranium_tons * 0.05);
    d["Uridium"] = std::max(0.0, duranium_tons * 0.05);
    d["Corundium"] = std::max(0.0, duranium_tons * 0.05);
    d["Gallicite"] = std::max(0.0, duranium_tons * 0.06);

    // Volatiles: comets / outer bodies trend Sorium-rich.
    d["Sorium"] = std::max(0.0, duranium_tons * 0.12 * sorium_multiplier);
  };

  // Give home bodies deep deposits so early growth isn't immediately blocked.
  seed_standard_deposits(earth, 2.0e6, 1.0);
  seed_standard_deposits(mars, 4.0e5, 0.7);
  // Minor bodies have smaller, but sometimes useful deposits.
  for (Id aid : sol_belt) {
    seed_standard_deposits(aid, 7.5e4, 0.3);
  }
  // Comets are volatile-rich.
  seed_standard_deposits(encke, 2.5e4, 6.0);
  seed_standard_deposits(centauri_prime, 1.2e6, 0.9);
  seed_standard_deposits(barnard_b, 3.0e5, 0.8);

  // --- Jump points (Sol <-> Alpha Centauri) ---
  const Id jp_sol = allocate_id(s);
  const Id jp_cen = allocate_id(s);
  {
    JumpPoint a;
    a.id = jp_sol;
    a.name = "Sol Jump Point";
    a.system_id = sol;
    a.position_mkm = {170.0, 0.0};
    a.linked_jump_id = jp_cen;
    s.jump_points[a.id] = a;
    s.systems[sol].jump_points.push_back(a.id);
  }
  {
    JumpPoint b;
    b.id = jp_cen;
    b.name = "Centauri Jump Point";
    b.system_id = centauri;
    b.position_mkm = {80.0, 0.0};
    b.linked_jump_id = jp_sol;
    s.jump_points[b.id] = b;
    s.systems[centauri].jump_points.push_back(b.id);
  }

  // --- Jump points (Alpha Centauri <-> Barnard's Star) ---
  const Id jp_cen2 = allocate_id(s);
  const Id jp_bar = allocate_id(s);
  {
    JumpPoint a;
    a.id = jp_cen2;
    a.name = "Centauri Outer Jump";
    a.system_id = centauri;
    a.position_mkm = {140.0, -35.0};
    a.linked_jump_id = jp_bar;
    s.jump_points[a.id] = a;
    s.systems[centauri].jump_points.push_back(a.id);
  }
  {
    JumpPoint b;
    b.id = jp_bar;
    b.name = "Barnard Jump Point";
    b.system_id = barnard;
    b.position_mkm = {55.0, 10.0};
    b.linked_jump_id = jp_cen2;
    s.jump_points[b.id] = b;
    s.systems[barnard].jump_points.push_back(b.id);
  }

  // --- Colony ---
  const Id earth_colony = allocate_id(s);
  {
    Colony c;
    c.id = earth_colony;
    c.name = "Earth";
    c.faction_id = terrans;
    c.body_id = earth;
    c.population_millions = 8500.0;
    c.minerals = {
        {"Duranium", 10000.0},
        {"Neutronium", 1500.0},
        {"Fuel", 30000.0},
    };
    c.installations = {
        {"automated_mine", 50},
        {"construction_factory", 5},
        {"fuel_refinery", 10},
        {"shipyard", 1},
        {"research_lab", 20},
        {"sensor_station", 1},
        {"training_facility", 2},
        {"planetary_fortress", 1},
    };
    c.ground_forces = 1200.0;
    s.colonies[c.id] = c;
  }

  const Id mars_colony = allocate_id(s);
  {
    Colony c;
    c.id = mars_colony;
    c.name = "Mars Outpost";
    c.faction_id = terrans;
    c.body_id = mars;
    c.population_millions = 250.0;
    c.minerals = {
        {"Duranium", 200.0},
        {"Neutronium", 20.0},
        {"Fuel", 1000.0},
    };
    c.installations = {
        {"automated_mine", 5},
        {"construction_factory", 1},
        {"fuel_refinery", 1},
        {"research_lab", 2},
        {"infrastructure", 3},
        {"terraforming_plant", 1},
    };
    c.ground_forces = 80.0;
    s.colonies[c.id] = c;
  }

  // --- Pirate base colony (Alpha Centauri) ---
  // Gives the default pirates an economy so they can scale up over time.
  {
    Colony c;
    c.id = allocate_id(s);
    c.name = "Haven";
    c.faction_id = pirates;
    c.body_id = centauri_prime;
    c.population_millions = 200.0;
    c.installations["shipyard"] = 1;
    c.installations["construction_factory"] = 1;
    c.installations["research_lab"] = 5;
    c.installations["sensor_station"] = 1;
    c.installations["planetary_fortress"] = 1;
    c.ground_forces = 400.0;
    c.installations["automated_mine"] = 10;
    c.installations["fuel_refinery"] = 3;
    c.minerals["Duranium"] = 15000.0;
    c.minerals["Neutronium"] = 1500.0;
    c.minerals["Fuel"] = 20000.0;
    s.colonies[c.id] = c;
  }

  auto add_ship = [&](Id faction_id, Id system_id, const Vec2& pos, const std::string& name,
                      const std::string& design_id) {
    const Id id = allocate_id(s);
    Ship ship;
    ship.id = id;
    ship.name = name;
    ship.faction_id = faction_id;
    ship.system_id = system_id;
    ship.design_id = design_id;
    ship.position_mkm = pos;
    s.ships[id] = ship;
    s.ship_orders[id] = ShipOrders{};
    s.systems[system_id].ships.push_back(id);
    return id;
  };

  // --- Starting Terran fleet (Sol) ---
  const Vec2 earth_pos = {149.6, 0.0};
  (void)add_ship(terrans, sol, earth_pos, "Freighter Alpha", "freighter_alpha");
  (void)add_ship(terrans, sol, earth_pos + Vec2{0.0, 0.8}, "Surveyor Beta", "surveyor_beta");
  (void)add_ship(terrans, sol, earth_pos + Vec2{0.0, -0.8}, "Escort Gamma", "escort_gamma");

  // --- Pirate presence (Alpha Centauri) ---
  const Vec2 pirate_pos = {80.0, 0.5};
  (void)add_ship(pirates, centauri, pirate_pos, "Raider I", "pirate_raider");
  (void)add_ship(pirates, centauri, pirate_pos + Vec2{0.7, -0.3}, "Raider II", "pirate_raider");

  return s;
}

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

// Astronomical unit in million km (mkm). This matches the Sol scenario where Earth is ~149.6 mkm.
constexpr double kAu_mkm = 149.6;

// Uniform random in [0, 1).
double rand_unit(std::mt19937& rng) {
  // generate_canonical is deterministic for a given engine sequence and avoids
  // some implementation differences between libstdc++'s distribution types.
  return std::generate_canonical<double, 32>(rng);
}

double rand_real(std::mt19937& rng, double lo, double hi) {
  return lo + (hi - lo) * rand_unit(rng);
}

int rand_int(std::mt19937& rng, int lo, int hi) {
  std::uniform_int_distribution<int> dist(lo, hi);
  return dist(rng);
}

// Simple Box-Muller normal sampler (deterministic given rand_unit()).
double rand_normal(std::mt19937& rng, double mean, double stddev) {
  const double u1 = std::max(1e-12, rand_unit(rng));
  const double u2 = rand_unit(rng);
  const double z0 = std::sqrt(-2.0 * std::log(u1)) * std::cos(kTwoPi * u2);
  return mean + z0 * stddev;
}

// Hash an undirected (a,b) pair into a single integer.
std::uint64_t edge_key(int a, int b) {
  const auto lo = static_cast<std::uint32_t>(std::min(a, b));
  const auto hi = static_cast<std::uint32_t>(std::max(a, b));
  return (static_cast<std::uint64_t>(lo) << 32) | static_cast<std::uint64_t>(hi);
}

char ascii_lower(char c) {
  if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
  return c;
}

char ascii_upper(char c) {
  if (c >= 'a' && c <= 'z') return static_cast<char>(c - 'a' + 'A');
  return c;
}

std::string capitalize(std::string s) {
  if (s.empty()) return s;
  s[0] = ascii_upper(s[0]);
  for (std::size_t i = 1; i < s.size(); ++i) s[i] = ascii_lower(s[i]);
  return s;
}

std::string to_roman(int n) {
  // Enough for our generator (we cap major bodies).
  static const char* kRomans[] = {"",  "I",  "II",  "III",  "IV",  "V",  "VI",  "VII",  "VIII",  "IX",  "X",
                                  "XI", "XII", "XIII", "XIV", "XV", "XVI", "XVII", "XVIII", "XIX", "XX"};
  if (n >= 0 && n <= 20) return kRomans[n];
  return std::to_string(n);
}

double clamp01(double x) { return std::clamp(x, 0.0, 1.0); }

std::uint32_t hash_u32(std::uint32_t x) {
  // Deterministic integer hash (good avalanche) for procedural noise.
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

double hash_to_unit01(std::uint32_t x) {
  // Convert to [0, 1).
  return static_cast<double>(x) / 4294967296.0;
}

double value_noise_2d(std::uint32_t seed, int x, int y) {
  const std::uint32_t ux = static_cast<std::uint32_t>(x);
  const std::uint32_t uy = static_cast<std::uint32_t>(y);
  const std::uint32_t h = hash_u32(seed ^ hash_u32(ux * 73856093u ^ uy * 19349663u));
  return hash_to_unit01(h);
}

double smoothstep(double t) {
  t = clamp01(t);
  return t * t * (3.0 - 2.0 * t);
}

double value_noise_2d_continuous(std::uint32_t seed, double x, double y) {
  const int x0 = static_cast<int>(std::floor(x));
  const int y0 = static_cast<int>(std::floor(y));
  const int x1 = x0 + 1;
  const int y1 = y0 + 1;

  const double tx = smoothstep(x - static_cast<double>(x0));
  const double ty = smoothstep(y - static_cast<double>(y0));

  const double v00 = value_noise_2d(seed, x0, y0);
  const double v10 = value_noise_2d(seed, x1, y0);
  const double v01 = value_noise_2d(seed, x0, y1);
  const double v11 = value_noise_2d(seed, x1, y1);

  const double a = v00 + (v10 - v00) * tx;
  const double b = v01 + (v11 - v01) * tx;
  return a + (b - a) * ty;
}

double fbm_noise_2d(std::uint32_t seed, double x, double y, int octaves = 4) {
  // Fractal Brownian Motion over value noise.
  double sum = 0.0;
  double amp = 1.0;
  double norm = 0.0;
  double freq = 1.0;

  for (int i = 0; i < octaves; ++i) {
    const std::uint32_t s = seed + 0x9E3779B9u * static_cast<std::uint32_t>(i + 1);
    sum += amp * value_noise_2d_continuous(s, x * freq, y * freq);
    norm += amp;
    amp *= 0.5;
    freq *= 2.0;
  }

  if (norm <= 0.0) return 0.0;
  return sum / norm;
}

double compute_nebula_density(std::uint32_t seed, const Vec2& galaxy_pos, double galaxy_radius) {
  // Nebula 'fields' from deterministic fractal noise.
  // Produces spatially coherent patches without a separate placement pass.
  const double scale = std::max(4.0, galaxy_radius * 0.18);
  const double nx = galaxy_pos.x / scale;
  const double ny = galaxy_pos.y / scale;

  const double n = fbm_noise_2d(seed ^ 0xC0FFEEu, nx, ny, 4);

  // Bias toward mostly-clear space with a few dense clouds.
  double d = clamp01((n - 0.58) / 0.28);
  d = d * d;

  // Mild radial shaping: keep the extreme core and rim slightly clearer.
  const double r = galaxy_pos.length();
  const double rn = r / std::max(1e-6, galaxy_radius);
  const double radial = clamp01(1.0 - std::abs(rn - 0.60) / 0.80);
  d *= 0.70 + 0.30 * radial;

  return clamp01(d);
}

double compute_filament_strength(std::uint32_t seed, const Vec2& galaxy_pos, double galaxy_radius) {
  // A "ridged" fractal field (toy model) used to create a filamentary galaxy
  // archetype.
  //
  // The core idea: value noise -> fBm -> transform into ridges by folding the
  // signal around 0.5 (so mid-range values become peaks). This creates
  // connected "veins" rather than blobby islands.
  const double scale = std::max(3.5, galaxy_radius * 0.22);
  const double nx = galaxy_pos.x / scale;
  const double ny = galaxy_pos.y / scale;

  const double n1 = fbm_noise_2d(seed ^ 0xF11A13EDu, nx, ny, 5);
  const double n2 = fbm_noise_2d(seed ^ 0xA17B3E57u, nx * 0.70 + 7.1, ny * 0.70 - 11.3, 3);

  auto ridge = [](double n) {
    double r = 1.0 - std::abs(2.0 * n - 1.0);
    r = std::clamp(r, 0.0, 1.0);
    return r * r;
  };

  double f = 0.65 * ridge(n1) + 0.35 * ridge(n2);

  // Radial shaping: keep the extreme rim a bit quieter, but avoid making the
  // core a complete void so the start system isn't isolated.
  const double r = galaxy_pos.length();
  const double rn = r / std::max(1e-6, galaxy_radius);
  const double radial = clamp01(1.10 - 0.95 * rn * rn);
  f *= 0.55 + 0.45 * radial;

  return clamp01(f);
}
double rand_log_uniform(std::mt19937& rng, double lo, double hi) {
  const double l = std::log(std::max(1e-12, lo));
  const double h = std::log(std::max(1e-12, hi));
  return std::exp(rand_real(rng, l, h));
}

std::string generate_system_name(std::mt19937& rng, std::unordered_set<std::string>& used) {
  // Small syllable-based name generator. The goal is "varied and pronounceable"
  // rather than resembling any specific language.
  static const std::array<const char*, 24> kLead = {
      "al", "an", "ar", "bel", "cor", "da", "el", "en", "fa", "gar", "hal", "ir",
      "ja", "kel", "lor", "mar", "nar", "or", "pra", "sel", "tor", "ul", "vor", "zen",
  };
  static const std::array<const char*, 18> kMid = {
      "a", "e", "i", "o", "u", "ae", "ai", "ia", "io", "oa", "oi", "ou", "ua", "ui", "au", "eo", "uu", "ei",
  };
  static const std::array<const char*, 28> kTail = {
      "dor", "nus", "lia", "reon", "tara", "vex", "mond", "phor", "tis", "dine", "ron", "rix", "vara", "loth",
      "mir", "gai", "dax", "tris", "gorn", "lune", "bor", "kesh", "ther", "vorn", "mere", "veil", "cai", "zen",
  };

  for (int attempt = 0; attempt < 1000; ++attempt) {
    const int parts = rand_int(rng, 2, 3);
    std::string raw;
    raw.reserve(12);
    raw += kLead[static_cast<std::size_t>(rand_int(rng, 0, static_cast<int>(kLead.size()) - 1))];
    raw += kMid[static_cast<std::size_t>(rand_int(rng, 0, static_cast<int>(kMid.size()) - 1))];
    raw += kTail[static_cast<std::size_t>(rand_int(rng, 0, static_cast<int>(kTail.size()) - 1))];
    if (parts == 3) {
      raw += kMid[static_cast<std::size_t>(rand_int(rng, 0, static_cast<int>(kMid.size()) - 1))];
      raw += kTail[static_cast<std::size_t>(rand_int(rng, 0, static_cast<int>(kTail.size()) - 1))];
    }

    std::string name = capitalize(raw);
    if (name.size() < 4 || name.size() > 14) continue;
    if (used.insert(name).second) return name;
  }

  // Fallback: deterministic-ish name.
  std::string fb = "System" + std::to_string(used.size() + 1);
  used.insert(fb);
  return fb;
}

struct StarParams {
  char spectral{'G'};
  double mass_solar{1.0};
  double luminosity_solar{1.0};
  double temp_k{5778.0};
  double radius_km{696340.0};
  double metallicity{1.0};
  double binary_prob{0.15};
};

double main_sequence_luminosity(double mass_solar) {
  // Very rough piecewise approximation.
  if (mass_solar < 0.43) return 0.23 * std::pow(mass_solar, 2.3);
  if (mass_solar < 2.0) return std::pow(mass_solar, 4.0);
  if (mass_solar < 20.0) return 1.5 * std::pow(mass_solar, 3.5);
  return 32000.0 * mass_solar;
}

StarParams sample_star(std::mt19937& rng) {
  struct Band {
    char spectral;
    double weight;
    double m_lo, m_hi;
    double t_lo, t_hi;
    double binary_prob;
  };

  // We bias toward lower-mass stars for interesting-but-playable system sizes.
  static const std::array<Band, 5> kBands = {{
      {'M', 0.52, 0.15, 0.60, 2400.0, 3900.0, 0.10},
      {'K', 0.22, 0.60, 0.85, 3900.0, 5200.0, 0.18},
      {'G', 0.16, 0.85, 1.15, 5200.0, 6000.0, 0.22},
      {'F', 0.08, 1.15, 1.45, 6000.0, 7500.0, 0.28},
      {'A', 0.02, 1.45, 2.10, 7500.0, 10000.0, 0.35},
  }};

  const double u = rand_unit(rng);
  double acc = 0.0;
  Band b = kBands.back();
  for (const auto& band : kBands) {
    acc += band.weight;
    if (u <= acc) {
      b = band;
      break;
    }
  }

  StarParams sp;
  sp.spectral = b.spectral;
  sp.mass_solar = rand_real(rng, b.m_lo, b.m_hi);
  sp.temp_k = rand_real(rng, b.t_lo, b.t_hi);
  sp.luminosity_solar = std::max(0.001, main_sequence_luminosity(sp.mass_solar));
  sp.radius_km = 696340.0 * std::pow(std::max(0.05, sp.mass_solar), 0.8);

  // Metallicity: centered on 1.0 with modest spread.
  sp.metallicity = std::clamp(rand_normal(rng, 1.0, 0.18), 0.55, 1.55);

  sp.binary_prob = b.binary_prob;
  return sp;
}

double kepler_period_days(double a_au, double star_mass_solar) {
  // P(years) = sqrt(a^3 / M)
  const double years = std::sqrt(std::max(0.0, a_au * a_au * a_au) / std::max(0.05, star_mass_solar));
  return std::max(1.0, years * 365.25);
}

double equilibrium_temp_k(double luminosity_solar, double dist_au, double albedo) {
  // Very rough blackbody equilibrium temperature.
  // 278K at 1 AU around 1 Lsun, adjusted by albedo.
  const double l = std::max(0.001, luminosity_solar);
  const double d = std::max(0.05, dist_au);
  const double a = std::clamp(albedo, 0.0, 0.95);
  const double t = 278.0 * std::pow(l, 0.25) / std::sqrt(d) * std::pow(1.0 - a, 0.25);
  return std::clamp(t, 30.0, 2000.0);
}

double sample_albedo(std::mt19937& rng, BodyType type) {
  switch (type) {
    case BodyType::GasGiant:
      return rand_real(rng, 0.30, 0.65);
    case BodyType::Comet:
      // Comets: very dark, dusty ice.
      return rand_real(rng, 0.02, 0.06);
    case BodyType::Asteroid:
      return rand_real(rng, 0.02, 0.18);
    case BodyType::Moon:
      return rand_real(rng, 0.05, 0.28);
    case BodyType::Planet:
    default:
      return rand_real(rng, 0.10, 0.45);
  }
}

double sample_atmosphere_atm(std::mt19937& rng, BodyType type, double orbit_au, double hz_au, double mass_earths) {
  // Prototype atmosphere model:
  // - We mostly care about roughly-Earthlike pressures for habitability.
  // - Non-rocky bodies (gas giants, asteroids, comets) are treated as effectively 0 atm.
  if (type == BodyType::GasGiant || type == BodyType::Asteroid || type == BodyType::Comet) return 0.0;

  const double hz = std::max(0.05, hz_au);
  const double rel = orbit_au / hz;
  const double d = std::abs(rel - 1.0);

  // "Habitable zone likeness" in [0,1].
  const double hz_like = clamp01(1.0 - (d / 0.70));

  // Near HZ: choose a broadly Earthlike range; far from HZ: thin/trace atmospheres.
  double atm = hz_like * rand_real(rng, 0.6, 1.4) + (1.0 - hz_like) * rand_real(rng, 0.0, 0.2);

  // Small bodies struggle to retain atmosphere.
  const double m = std::max(0.01, mass_earths);
  const double retention = std::clamp(std::pow(m, 0.35), 0.15, 1.60);
  atm *= retention;

  // Moons are more likely to have thin atmospheres.
  if (type == BodyType::Moon) atm *= 0.35;

  return std::max(0.0, atm);
}

double rocky_radius_km(double mass_earths) {
  // Mass-radius relation for rocky planets (very rough).
  const double m = std::max(1e-6, mass_earths);
  return 6371.0 * std::pow(m, 0.27);
}

double gas_radius_km(double mass_earths) {
  // Gas giants have a fairly weak mass-radius relation in this regime.
  const double m = std::clamp(mass_earths, 10.0, 400.0);
  const double r_earth = std::clamp(6.0 + 6.0 * std::pow(m / 318.0, 0.15), 5.0, 14.0);
  return 6371.0 * r_earth;
}

} // namespace

GameState make_random_scenario(const RandomScenarioConfig& cfg) {
  GameState s;
  // Bump when the save schema changes.
  // v8: adds WaitDays order type.
  // v9: adds ShipOrders repeat fields (repeat + repeat_template).
  // v10: adds persistent GameState::events (simulation event log).
  // v11: adds structured event fields (category + context ids).
  // v12: adds repeat_count_remaining to ShipOrders for finite repeats.
  // v22: adds Body::mineral_deposits (finite mining / depletion).
  // v23: adds ship fuel tanks + fuel consumption (logistics).
  // v26: adds hierarchical orbits (Body::parent_body_id) + optional physical metadata.
  // v27: adds eccentric orbits + BodyType::Comet.
  // New games should start at the current save schema version.
  s.save_version = GameState{}.save_version;
  s.date = Date::from_ymd(2200, 1, 1);

  std::uint32_t seed = cfg.seed;
  int num_systems = cfg.num_systems;

  const RandomGalaxyShape galaxy_shape = cfg.galaxy_shape;
  const bool pirates_enabled = cfg.enable_pirates;
  const double pirate_strength = std::max(0.0, cfg.pirate_strength);

  if (num_systems < 1) num_systems = 1;
  // Keep things reasonably small so the prototype UI stays responsive.
  if (num_systems > 64) num_systems = 64;

  std::mt19937 rng(seed);

  // --- Factions ---
  const Id terrans = allocate_id(s);
  {
    Faction f;
    f.id = terrans;
    f.name = "Terran Union";
    f.research_points = 0.0;
    f.known_techs = {"chemistry_1"};
    f.research_queue = {"nuclear_1", "propulsion_1", "colonization_1"};
    s.factions[terrans] = f;
  }

  Id pirates = kInvalidId;
  if (pirates_enabled) {
    pirates = allocate_id(s);
    Faction f;
    f.id = pirates;
    f.name = "Pirate Raiders";
    f.control = FactionControl::AI_Pirate;
    f.research_points = 0.0;
    s.factions[pirates] = f;
  }

  // --- Helpers ---
  auto add_body = [&](Id system_id, Id parent_body_id, const std::string& name, BodyType type, double a_mkm,
                      double period_days, double mean_anomaly, double eccentricity = 0.0, double arg_periapsis = 0.0) -> Id {
    const Id id = allocate_id(s);
    Body b;
    b.id = id;
    b.name = name;
    b.type = type;
    b.system_id = system_id;
    b.parent_body_id = parent_body_id;
    b.orbit_radius_mkm = a_mkm;
    b.orbit_period_days = period_days;
    b.orbit_phase_radians = mean_anomaly;
    b.orbit_eccentricity = eccentricity;
    b.orbit_arg_periapsis_radians = arg_periapsis;
    s.bodies[id] = b;
    s.systems[system_id].bodies.push_back(id);
    return id;
  };

  auto seed_basic_deposits = [&](Id body_id, BodyType type, double orbit_au, double metallicity, double richness, double nebula_density) {
    // Baselines tuned for "prototype fun" rather than strict realism.
    double base_dur = 6.0e5;
    double base_neu = 6.0e4;

    switch (type) {
      case BodyType::GasGiant:
        base_dur = 3.5e5;
        base_neu = 4.5e4;
        break;
      case BodyType::Moon:
        base_dur = 2.5e5;
        base_neu = 2.5e4;
        break;
      case BodyType::Asteroid:
        base_dur = 4.5e5;
        base_neu = 4.0e4;
        break;
      case BodyType::Comet:
        // Comets trend volatile-rich but metal-poor (prototype: fewer mineable tons).
        base_dur = 1.6e5;
        base_neu = 1.2e4;
        break;
      case BodyType::Planet:
      default:
        base_dur = 6.0e5;
        base_neu = 6.0e4;
        break;
    }

    // Outer bodies trend slightly richer.
    const double dist_factor = 0.85 + 0.35 * clamp01(orbit_au / 6.0);

    double dur = base_dur * metallicity * dist_factor * rand_log_uniform(rng, 0.55, 1.85) * richness;
    double neu = base_neu * metallicity * dist_factor * rand_log_uniform(rng, 0.55, 1.85) * richness;

    // Occasionally generate "bonanza" asteroids.
    if (type == BodyType::Asteroid && rand_unit(rng) < 0.12) {
      const double boost = rand_real(rng, 3.0, 8.0);
      dur *= boost;
      neu *= boost;
    }

    auto& d = s.bodies.at(body_id).mineral_deposits;

    // Volatiles multiplier trends upward with distance; comets and gas giants are extra Sorium-rich.
    double sorium_mult = 0.6 + 0.6 * clamp01(orbit_au / 6.0);
    // Nebulae skew volatile-rich: boost Sorium in dusty systems.
    sorium_mult *= 1.0 + 1.25 * clamp01(nebula_density);
    if (type == BodyType::GasGiant) sorium_mult *= 2.2;
    if (type == BodyType::Comet) sorium_mult *= 7.0;

    // Duranium / Neutronium are seeded explicitly, the rest are proportional to Duranium with
    // an independent random factor.
    d["Duranium"] = std::max(0.0, dur);
    d["Neutronium"] = std::max(0.0, neu);

    const double base = std::max(0.0, dur);
    auto prop = [&](double fraction, double lo, double hi) {
      return std::max(0.0, base * fraction * rand_log_uniform(rng, lo, hi));
    };

    d["Tritanium"] = prop(0.08, 0.55, 1.85);
    d["Boronide"] = prop(0.07, 0.55, 1.85);
    d["Corbomite"] = prop(0.05, 0.55, 1.85);
    d["Mercassium"] = prop(0.06, 0.55, 1.85);
    d["Vendarite"] = prop(0.05, 0.55, 1.85);
    d["Uridium"] = prop(0.05, 0.55, 1.85);
    d["Corundium"] = prop(0.05, 0.55, 1.85);
    d["Gallicite"] = prop(0.06, 0.55, 1.85);

    d["Sorium"] = std::max(0.0, base * 0.12 * sorium_mult * rand_log_uniform(rng, 0.55, 1.85));
  };

  struct SysInfo {
    Id id{kInvalidId};
    std::string name;
    Vec2 galaxy_pos{0.0, 0.0};


    // Procedural region/sector id (optional).
    Id region_id{kInvalidId};

    // System-level nebula/dust density in [0,1].
    double nebula_density{0.0};

    Id primary_star{kInvalidId};
    Id preferred_colony_body{kInvalidId};
    Id first_planet{kInvalidId};

    double star_mass_solar{1.0};
    double star_luminosity_solar{1.0};
    double metallicity{1.0};

    double max_orbit_extent_mkm{0.0};

    std::vector<Id> planet_bodies;
  };

  std::vector<SysInfo> systems;
  systems.reserve(static_cast<std::size_t>(num_systems));

  // --- Galaxy layout ---
  // We generate a small 2D "galaxy" in abstract distance units (approx. 1 ~= 1 ly).
  // Placement is deterministic for a given seed.
  const double galaxy_radius = std::max(3.0, std::sqrt(static_cast<double>(num_systems)) * 3.6);

  // Minimum separation to keep the map readable and avoid extremely short jump edges.
  const double min_sep = std::max(0.55, galaxy_radius / (std::sqrt(static_cast<double>(num_systems)) * 2.25));

  // Spiral parameters (used for SpiralDisc).
  const int spiral_arms = 4;
  const double arm_tightness = 1.25;
  const double arm_spread = 0.28;
  const double core_fraction = 0.18;
  const double core_radius = galaxy_radius * 0.25;

  // Ring parameters.
  const double ring_inner = galaxy_radius * 0.45;

  // Cluster parameters.
  std::vector<Vec2> cluster_centers;
  double cluster_sigma = galaxy_radius * 0.18;
  if (galaxy_shape == RandomGalaxyShape::Clustered) {
    const int clusters = std::clamp(2 + num_systems / 18, 2, 5);
    cluster_centers.reserve(static_cast<std::size_t>(clusters));

    const double center_radius = galaxy_radius * 0.70;
    const double center_min_sep = galaxy_radius * 0.45;

    for (int c = 0; c < clusters; ++c) {
      Vec2 center{0.0, 0.0};
      bool found = false;
      for (int attempt = 0; attempt < 800; ++attempt) {
        const double r = center_radius * std::sqrt(rand_unit(rng));
        const double a = rand_real(rng, 0.0, kTwoPi);
        center = {r * std::cos(a), r * std::sin(a)};

        // Keep clusters away from the origin so the "home" system isn't always inside a cluster.
        if (center.length() < galaxy_radius * 0.20) continue;

        bool ok = true;
        for (const Vec2& q : cluster_centers) {
          if ((center - q).length() < center_min_sep) {
            ok = false;
            break;
          }
        }
        if (ok) {
          found = true;
          break;
        }
      }
      if (found) cluster_centers.push_back(center);
    }

    if (cluster_centers.empty()) {
      // Worst-case fallback.
      cluster_centers.push_back({galaxy_radius * 0.35, 0.0});
      cluster_centers.push_back({-galaxy_radius * 0.35, 0.0});
    }
  }

  std::vector<Vec2> placed;
  placed.reserve(static_cast<std::size_t>(num_systems));

  std::unordered_set<std::string> used_names;
  used_names.reserve(static_cast<std::size_t>(num_systems) * 2);

  const RandomPlacementStyle placement_style = cfg.placement_style;
  const int placement_quality = std::clamp(cfg.placement_quality, 4, 96);

  // Sample a random candidate point for the current galaxy shape.
  //
  // For Filamentary, we use an importance-sampling style acceptance step
  // driven by a ridged fractal field, which creates voids and "rivers".
  const auto random_point_for_shape = [&]() -> Vec2 {
    for (int attempt = 0; attempt < 2400; ++attempt) {
      Vec2 p{0.0, 0.0};

      switch (galaxy_shape) {
        case RandomGalaxyShape::SpiralDisc: {
          if (rand_unit(rng) < core_fraction) {
            const double r = core_radius * std::sqrt(rand_unit(rng));
            const double a = rand_real(rng, 0.0, kTwoPi);
            p = {r * std::cos(a), r * std::sin(a)};
          } else {
            const double r = galaxy_radius * std::sqrt(rand_unit(rng));
            const int arm = rand_int(rng, 0, spiral_arms - 1);
            const double arm_base = static_cast<double>(arm) * (kTwoPi / static_cast<double>(spiral_arms));
            const double theta = arm_base + arm_tightness * std::log(1.0 + r) + rand_normal(rng, 0.0, arm_spread);

            const double j = (0.35 + 0.65 * (r / galaxy_radius));
            const double rr = std::max(0.0, r + rand_normal(rng, 0.0, arm_spread * 0.55 * j));
            const double tt = theta + rand_normal(rng, 0.0, arm_spread * 0.35 * j);
            p = {rr * std::cos(tt), rr * std::sin(tt)};
          }
          break;
        }

        case RandomGalaxyShape::UniformDisc: {
          const double r = galaxy_radius * std::sqrt(rand_unit(rng));
          const double a = rand_real(rng, 0.0, kTwoPi);
          p = {r * std::cos(a), r * std::sin(a)};
          break;
        }

        case RandomGalaxyShape::Ring: {
          // Uniform-in-area annulus.
          const double u = rand_unit(rng);
          const double r = std::sqrt((ring_inner * ring_inner) * (1.0 - u) + (galaxy_radius * galaxy_radius) * u);
          const double a = rand_real(rng, 0.0, kTwoPi);
          const double rr = std::max(0.0, r + rand_normal(rng, 0.0, galaxy_radius * 0.015));
          p = {rr * std::cos(a), rr * std::sin(a)};
          break;
        }

        case RandomGalaxyShape::Clustered: {
          if (rand_unit(rng) < 0.10) {
            const double r = galaxy_radius * std::sqrt(rand_unit(rng));
            const double a = rand_real(rng, 0.0, kTwoPi);
            p = {r * std::cos(a), r * std::sin(a)};
          } else {
            const Vec2 c = cluster_centers[static_cast<std::size_t>(
                rand_int(rng, 0, static_cast<int>(cluster_centers.size()) - 1))];
            p = {c.x + rand_normal(rng, 0.0, cluster_sigma), c.y + rand_normal(rng, 0.0, cluster_sigma)};
            const double len = p.length();
            if (len > galaxy_radius) {
              p = p * (galaxy_radius / std::max(1e-6, len));
            }
          }
          break;
        }

        case RandomGalaxyShape::Filamentary: {
          const double r = galaxy_radius * std::sqrt(rand_unit(rng));
          const double a = rand_real(rng, 0.0, kTwoPi);
          p = {r * std::cos(a), r * std::sin(a)};

          // Importance-sample toward filaments but keep a small background field
          // so the map doesn't become a set of disconnected strings.
          const double f = compute_filament_strength(seed ^ 0x6D4D4F7Bu, p, galaxy_radius);
          const double accept = 0.10 + 0.90 * f;
          if (rand_unit(rng) > accept) continue;
          break;
        }
      }

      return p;
    }

    // Fallback: random disc.
    const double r = galaxy_radius * std::sqrt(rand_unit(rng));
    const double a = rand_real(rng, 0.0, kTwoPi);
    return {r * std::cos(a), r * std::sin(a)};
  };

  auto min_distance_to_placed = [&](const Vec2& p) {
    if (placed.empty()) return 1e9;
    double best = 1e9;
    for (const Vec2& q : placed) {
      best = std::min(best, (p - q).length());
    }
    return best;
  };

  const auto sample_system_pos = [&](int i) -> Vec2 {
    if (i == 0) return {0.0, 0.0};

    auto classic = [&]() -> Vec2 {
      for (int attempt = 0; attempt < 1200; ++attempt) {
        const Vec2 p = random_point_for_shape();
        bool ok = true;
        for (const Vec2& q : placed) {
          if ((p - q).length() < min_sep) {
            ok = false;
            break;
          }
        }
        if (ok) return p;
      }
      return random_point_for_shape();
    };

    if (placement_style == RandomPlacementStyle::Classic) {
      return classic();
    }

    // Mitchell-style best-candidate sampling: evaluate K candidates and choose
    // the one that maximizes the distance to the nearest existing point.
    //
    // This approximates a Poisson-disc / blue-noise distribution.
    Vec2 best_p = random_point_for_shape();
    double best_score = -1.0;
    double best_min_dist = 0.0;

    const int k = placement_quality;
    for (int c = 0; c < k; ++c) {
      const Vec2 p = random_point_for_shape();
      const double md = min_distance_to_placed(p);
      double score = md;

      // Filamentary: softly bias candidates toward high filament strength,
      // while still keeping spacing as the primary term.
      if (galaxy_shape == RandomGalaxyShape::Filamentary) {
        const double f = compute_filament_strength(seed ^ 0x3C6EF35Fu, p, galaxy_radius);
        score *= (0.35 + 0.65 * f);
      }

      if (score > best_score) {
        best_score = score;
        best_p = p;
        best_min_dist = md;
      }
    }

    // If we found a well-separated candidate, take it.
    if (best_min_dist >= min_sep) return best_p;

    // Otherwise, do a few extra "hard" attempts to satisfy min_sep, then fall
    // back to the best candidate we saw.
    for (int attempt = 0; attempt < 2400; ++attempt) {
      const Vec2 p = random_point_for_shape();
      if (min_distance_to_placed(p) >= min_sep) return p;
    }

    return best_p;
  };

  // --- Systems + bodies ---
  for (int i = 0; i < num_systems; ++i) {
    const Id sys_id = allocate_id(s);

    StarSystem sys;
    sys.id = sys_id;
    sys.name = generate_system_name(rng, used_names);
    sys.galaxy_pos = sample_system_pos(i);
    sys.nebula_density = compute_nebula_density(seed, sys.galaxy_pos, galaxy_radius);
    // Keep the start system readable/playable: avoid extremely dense nebula at game start.
    if (i == 0 && cfg.ensure_clear_home) sys.nebula_density = std::min(sys.nebula_density, 0.25);
    s.systems[sys_id] = sys;
    placed.push_back(sys.galaxy_pos);

    SysInfo info;
    info.id = sys_id;
    info.name = sys.name;
    info.galaxy_pos = sys.galaxy_pos;
    info.nebula_density = sys.nebula_density;

    StarParams sp = sample_star(rng);

    // Mild galactic metallicity gradient: inner systems skew metal-rich, outer systems skew metal-poor.
    // Nebulae (star-forming regions) get a small additional boost in this toy model.
    {
      const double r = sys.galaxy_pos.length();
      const double rn = r / std::max(1e-6, galaxy_radius);
      const double grad = std::clamp(1.25 - 0.55 * rn, 0.55, 1.35);
      const double neb_boost = 1.0 + 0.20 * clamp01(sys.nebula_density);
      sp.metallicity = std::clamp(sp.metallicity * grad * neb_boost, 0.35, 1.80);
    }
    info.star_mass_solar = sp.mass_solar;
    info.star_luminosity_solar = sp.luminosity_solar;
    info.metallicity = sp.metallicity;

    // Star (anchor at origin).
    {
      const std::string star_name = sys.name + " Star (" + std::string(1, sp.spectral) + ")";
      info.primary_star = add_body(sys_id, kInvalidId, star_name, BodyType::Star, 0.0, 1.0, 0.0);
      auto& b = s.bodies.at(info.primary_star);
      b.mass_solar = sp.mass_solar;
      b.luminosity_solar = sp.luminosity_solar;
      b.radius_km = sp.radius_km;
      b.surface_temp_k = sp.temp_k;
    }

    double max_extent_mkm = 0.0;

    // Occasionally generate a secondary star.
    if (rand_unit(rng) < sp.binary_prob) {
      const double mass2 = sp.mass_solar * rand_real(rng, 0.25, 0.90);
      const double lum2 = std::max(0.001, main_sequence_luminosity(mass2));
      const double temp2 = std::clamp(2600.0 + 4200.0 * std::sqrt(std::max(0.05, mass2)), 2400.0, 11000.0);

      // Keep separations playable (roughly 0.3–2.5 AU).
      const double sep_mkm = rand_real(rng, 40.0, 380.0);
      const double sep_au = sep_mkm / kAu_mkm;

      const double period = kepler_period_days(sep_au, sp.mass_solar + mass2);
      const double phase = rand_real(rng, 0.0, kTwoPi);

      const Id sec_id = add_body(sys_id, info.primary_star, sys.name + " B", BodyType::Star, sep_mkm, period, phase);
      auto& b = s.bodies.at(sec_id);
      b.mass_solar = mass2;
      b.luminosity_solar = lum2;
      b.radius_km = 696340.0 * std::pow(std::max(0.05, mass2), 0.8);
      b.surface_temp_k = temp2;

      max_extent_mkm = std::max(max_extent_mkm, sep_mkm);
    }

    // Planetary orbits (in AU).
    const double hz_au = std::sqrt(std::max(0.001, sp.luminosity_solar));
    const double inner_au = std::max(0.15, hz_au * rand_real(rng, 0.25, 0.55));
    const double outer_au = std::clamp(hz_au * rand_real(rng, 6.0, 10.0), 2.0, 8.0);

    std::vector<double> orbits_au;
    orbits_au.reserve(12);

    double a = inner_au;
    const int max_major = rand_int(rng, 4, 9);
    for (int p = 0; p < max_major && a <= outer_au; ++p) {
      orbits_au.push_back(a);
      a *= rand_real(rng, 1.35, 1.85);
    }
    if (orbits_au.empty()) orbits_au.push_back(hz_au);

    const double snow_au = 2.7 * hz_au;

    // Optional asteroid belt: pick an orbit near the snow line.
    int belt_index = -1;
    if (static_cast<int>(orbits_au.size()) >= 4 && rand_unit(rng) < 0.45) {
      const double target = std::clamp(snow_au, orbits_au.front(), orbits_au.back());
      double best = 1e9;
      for (int idx = 0; idx < static_cast<int>(orbits_au.size()); ++idx) {
        const double d = std::abs(orbits_au[idx] - target);
        if (d < best) {
          best = d;
          belt_index = idx;
        }
      }
      // Avoid belts as the innermost orbit (keeps early colonization less cluttered).
      if (belt_index == 0) belt_index = -1;
    }

    // For the home system, force the orbit closest to the habitable zone to be a rocky planet.
    int forced_home_idx = -1;
    if (i == 0) {
      double best = 1e9;
      for (int idx = 0; idx < static_cast<int>(orbits_au.size()); ++idx) {
        const double d = std::abs(orbits_au[idx] - hz_au);
        if (d < best) {
          best = d;
          forced_home_idx = idx;
        }
      }
      if (forced_home_idx == belt_index) belt_index = -1;
    }

    int planet_counter = 0;
    double best_hab_score = std::numeric_limits<double>::infinity();
    Id best_hab_id = kInvalidId;

    for (int idx = 0; idx < static_cast<int>(orbits_au.size()); ++idx) {
      const double orbit_au = orbits_au[idx];
      const double orbit_mkm = orbit_au * kAu_mkm;
      const double period_days = kepler_period_days(orbit_au, sp.mass_solar);

      // Asteroid belt.
      if (idx == belt_index) {
        const int asteroids = rand_int(rng, 24, 64);
        for (int aidx = 0; aidx < asteroids; ++aidx) {
          const double rr = std::max(5.0, orbit_mkm + rand_normal(rng, 0.0, std::max(0.8, orbit_mkm * 0.012)));
          const double ph = rand_real(rng, 0.0, kTwoPi);
          const double pd = kepler_period_days(rr / kAu_mkm, sp.mass_solar);

          const std::string aname =
              sys.name + " Belt " + to_roman(planet_counter + 1) + "-" + std::to_string(aidx + 1);
          const Id aid = add_body(sys_id, kInvalidId, aname, BodyType::Asteroid, rr, pd, ph);

          auto& b = s.bodies.at(aid);
          b.radius_km = rand_real(rng, 5.0, 180.0);
          b.surface_temp_k = equilibrium_temp_k(sp.luminosity_solar, rr / kAu_mkm, sample_albedo(rng, BodyType::Asteroid));
          seed_basic_deposits(aid, BodyType::Asteroid, rr / kAu_mkm, sp.metallicity, 1.0, sys.nebula_density);

          max_extent_mkm = std::max(max_extent_mkm, rr);
        }
        continue;
      }

      ++planet_counter;

      // Gas giant probability ramps up beyond the snow line.
      const double beyond = (orbit_au <= snow_au) ? 0.0 : clamp01((orbit_au - snow_au) / std::max(0.25, outer_au - snow_au));
      double gas_prob = 0.04 + 0.30 * beyond;
      gas_prob *= std::clamp(0.75 + 0.45 * (sp.metallicity - 1.0), 0.45, 1.55);

      bool is_gas = rand_unit(rng) < gas_prob;
      if (idx == forced_home_idx) is_gas = false;

      const BodyType type = is_gas ? BodyType::GasGiant : BodyType::Planet;

      double phase = rand_real(rng, 0.0, kTwoPi);
      if (idx == forced_home_idx) phase = 0.0;

      const std::string pname = sys.name + " " + to_roman(planet_counter);
      const Id pid = add_body(sys_id, kInvalidId, pname, type, orbit_mkm, period_days, phase);
      info.planet_bodies.push_back(pid);
      if (info.first_planet == kInvalidId) info.first_planet = pid;

      auto& pb = s.bodies.at(pid);
      if (type == BodyType::GasGiant) {
        pb.mass_earths = rand_log_uniform(rng, 15.0, 320.0);
        pb.radius_km = gas_radius_km(pb.mass_earths);
      } else {
        pb.mass_earths = rand_log_uniform(rng, 0.20, 5.0);
        pb.radius_km = rocky_radius_km(pb.mass_earths);
      }
      pb.surface_temp_k = equilibrium_temp_k(sp.luminosity_solar, orbit_au, sample_albedo(rng, type));

      // Basic atmosphere model (used by the habitability system).
      pb.atmosphere_atm = sample_atmosphere_atm(rng, type, orbit_au, hz_au, pb.mass_earths);

      // For the forced homeworld orbit, mark terraforming complete so it behaves
      // as fully habitable regardless of small temperature/atmosphere deviations.
      if (idx == forced_home_idx) {
        pb.atmosphere_atm = std::clamp(pb.atmosphere_atm, 0.8, 1.2);
        pb.terraforming_target_temp_k = pb.surface_temp_k;
        pb.terraforming_target_atm = pb.atmosphere_atm;
        pb.terraforming_complete = true;
      }

      // Mineral deposits.
      const double richness = (idx == forced_home_idx) ? 2.1 : 1.0;
      seed_basic_deposits(pid, type, orbit_au, sp.metallicity, richness, sys.nebula_density);

      // Trojan asteroids (L4/L5) around some gas giants.
      // These orbit the primary star at roughly the same semi-major axis as the giant.
      if (type == BodyType::GasGiant && rand_unit(rng) < 0.70) {
        const int tro_each = rand_int(rng, 4, 12);

        auto add_trojans = [&](const char* tag, double offset) {
          for (int ti = 0; ti < tro_each; ++ti) {
            const double rr = orbit_mkm * rand_real(rng, 0.995, 1.005);
            const double e = std::clamp(rand_real(rng, 0.0, 0.06), 0.0, 0.12);
            const double w = rand_real(rng, 0.0, kTwoPi);
            const double ph = phase + offset + rand_normal(rng, 0.0, 0.20);
            const double pd = kepler_period_days(rr / kAu_mkm, sp.mass_solar);

            const std::string tname = pname + " " + tag + "-" + std::to_string(ti + 1);
            const Id tid = add_body(sys_id, kInvalidId, tname, BodyType::Asteroid, rr, pd, ph, e, w);

            auto& tb = s.bodies.at(tid);
            tb.radius_km = rand_real(rng, 2.0, 60.0);
            tb.surface_temp_k =
                equilibrium_temp_k(sp.luminosity_solar, rr / kAu_mkm, sample_albedo(rng, BodyType::Asteroid));
            seed_basic_deposits(tid, BodyType::Asteroid, rr / kAu_mkm, sp.metallicity, 0.8, sys.nebula_density);

            max_extent_mkm = std::max(max_extent_mkm, rr * (1.0 + e));
          }
        };

        add_trojans("L4", kTwoPi / 6.0);   // +60 degrees
        add_trojans("L5", -kTwoPi / 6.0);  // -60 degrees
      }

      // Habitable-ish scoring for colony candidates.
      if (type == BodyType::Planet) {
        const double t = pb.surface_temp_k;
        if (t >= 250.0 && t <= 330.0) {
          const double score = std::abs(t - 288.0) + 8.0 * std::abs(orbit_au - hz_au);
          if (score < best_hab_score) {
            best_hab_score = score;
            best_hab_id = pid;
          }
        }
      }

      max_extent_mkm = std::max(max_extent_mkm, orbit_mkm);

      // Moons.
      int moon_count = 0;
      if (type == BodyType::GasGiant) {
        moon_count = rand_int(rng, 0, 3);
        if (rand_unit(rng) < 0.15) moon_count = std::min(5, moon_count + 2);
      } else if (rand_unit(rng) < 0.18) {
        moon_count = rand_int(rng, 1, 2);
      }

      for (int mi = 0; mi < moon_count; ++mi) {
        const double mr = rand_log_uniform(rng, 0.20, 3.5);
        const double mp = rand_real(rng, 2.0, 60.0);
        const double ph = rand_real(rng, 0.0, kTwoPi);

        const std::string mname = pname + "-" + static_cast<char>('a' + mi);
        const Id mid = add_body(sys_id, pid, mname, BodyType::Moon, mr, mp, ph);

        auto& mb = s.bodies.at(mid);
        // Scale moon masses a bit with the parent.
        const double moon_hi = (type == BodyType::GasGiant) ? 0.25 : 0.08;
        mb.mass_earths = rand_log_uniform(rng, 0.001, moon_hi);
        mb.radius_km = rocky_radius_km(mb.mass_earths);
        mb.surface_temp_k = equilibrium_temp_k(sp.luminosity_solar, orbit_au, sample_albedo(rng, BodyType::Moon));
        mb.atmosphere_atm = sample_atmosphere_atm(rng, BodyType::Moon, orbit_au, hz_au, mb.mass_earths);
        seed_basic_deposits(mid, BodyType::Moon, orbit_au, sp.metallicity, 0.9, sys.nebula_density);

        max_extent_mkm = std::max(max_extent_mkm, orbit_mkm + mr);
      }

      // Captured minor bodies / irregular satellites around some gas giants.
      // Implemented as Asteroid-type bodies orbiting the planet (not the star).
      if (type == BodyType::GasGiant && rand_unit(rng) < 0.35) {
        const int captured = rand_int(rng, 1, 3);
        for (int ci = 0; ci < captured; ++ci) {
          const double mr = rand_log_uniform(rng, 0.05, 1.5);
          const double mp = rand_real(rng, 10.0, 180.0);
          const double ph = rand_real(rng, 0.0, kTwoPi);
          const double e = std::clamp(rand_real(rng, 0.0, 0.35), 0.0, 0.8);
          const double w = rand_real(rng, 0.0, kTwoPi);

          const std::string cname = pname + " Captured-" + std::to_string(ci + 1);
          const Id cid = add_body(sys_id, pid, cname, BodyType::Asteroid, mr, mp, ph, e, w);

          auto& cb = s.bodies.at(cid);
          cb.mass_earths = rand_log_uniform(rng, 0.000001, 0.00005);
          cb.radius_km = rand_real(rng, 1.0, 25.0);
          cb.surface_temp_k =
              equilibrium_temp_k(sp.luminosity_solar, orbit_au, sample_albedo(rng, BodyType::Asteroid));
          seed_basic_deposits(cid, BodyType::Asteroid, orbit_au, sp.metallicity, 0.65, sys.nebula_density);

          max_extent_mkm = std::max(max_extent_mkm, orbit_mkm + mr * (1.0 + e));
        }
      }
    }

    // Comets: eccentric minor bodies with perihelia in the inner system and aphelia near the outer edge.
    // Keep aphelion within a modest multiple of the system's current extent so the map stays readable.
    const double sys_extent = std::max(max_extent_mkm, outer_au * kAu_mkm);
    if (rand_unit(rng) < 0.65) {
      const int comets = rand_int(rng, 1, 3);
      for (int ci = 0; ci < comets; ++ci) {
        const double aphelion = sys_extent * rand_real(rng, 1.05, 1.25);

        // Perihelion biased toward the inner system.
        double perihelion = inner_au * kAu_mkm * rand_real(rng, 0.35, 0.85);
        perihelion = std::max(perihelion, 0.05 * kAu_mkm);
        perihelion = std::min(perihelion, aphelion * 0.70);

        const double e = (aphelion - perihelion) / (aphelion + perihelion);
        const double a_mkm = 0.5 * (aphelion + perihelion);
        const double pd = kepler_period_days(a_mkm / kAu_mkm, sp.mass_solar);
        const double M0 = rand_real(rng, 0.0, kTwoPi);
        const double w = rand_real(rng, 0.0, kTwoPi);

        const std::string cname = sys.name + " Comet " + std::string(1, static_cast<char>('A' + ci));
        const Id cid = add_body(sys_id, kInvalidId, cname, BodyType::Comet, a_mkm, pd, M0, e, w);

        auto& cb = s.bodies.at(cid);
        cb.radius_km = rand_real(rng, 1.0, 20.0);
        // Temperature shown is a perihelion equilibrium estimate (what matters for "active" comets).
        cb.surface_temp_k = equilibrium_temp_k(sp.luminosity_solar, perihelion / kAu_mkm, sample_albedo(rng, BodyType::Comet));
        seed_basic_deposits(cid, BodyType::Comet, a_mkm / kAu_mkm, sp.metallicity, 0.55, sys.nebula_density);

        max_extent_mkm = std::max(max_extent_mkm, aphelion);
      }
    }


    // Colony candidate selection for this system.
    if (best_hab_id != kInvalidId) {
      info.preferred_colony_body = best_hab_id;
    } else if (info.first_planet != kInvalidId) {
      info.preferred_colony_body = info.first_planet;
    } else {
      info.preferred_colony_body = info.primary_star;
    }

    info.max_orbit_extent_mkm = max_extent_mkm;
    systems.push_back(std::move(info));
  }

  // --- Start positions ---
  const Id home_system = systems.front().id;

  // --- Procedural regions (Voronoi sectors) ---
  //
  // We generate region seeds using farthest-point sampling and assign each
  // system to its nearest seed (a Voronoi partition). Regions carry a small
  // set of modifiers that influence nebula density and resource/salvage
  // richness. This adds large-scale "biomes" to the map without needing
  // complex geometry.
  if (cfg.enable_regions && num_systems > 0) {
    int region_count = cfg.num_regions;
    if (region_count < 0) {
      if (num_systems <= 1) {
        region_count = 1;
      } else {
        // sqrt(n) is a decent heuristic for small maps (<=64 systems).
        region_count = std::clamp(static_cast<int>(std::llround(std::sqrt(static_cast<double>(num_systems)))), 2, 10);
      }
    }
    region_count = std::clamp(region_count, 1, num_systems);

    std::mt19937 rrng(seed ^ 0xD0A17E11u);

    struct RegionThemeDef {
      const char* theme;
      double mineral_lo;
      double mineral_hi;
      double volatile_lo;
      double volatile_hi;
      double salvage_lo;
      double salvage_hi;
      double nebula_bias;
      double pirate_lo;
      double pirate_hi;
      double ruins_lo;
      double ruins_hi;
    };

    // Themes are intentionally gamey: they bias "what you find" and "what you fight"
    // rather than trying to model astrophysics.
    static const std::array<RegionThemeDef, 6> kThemes = {{
        {"Core Worlds", 1.10, 1.35, 0.85, 1.10, 0.85, 1.05, -0.10, 0.02, 0.18, 0.05, 0.18},
        {"Frontier", 0.95, 1.20, 0.95, 1.25, 0.95, 1.15, 0.00, 0.10, 0.35, 0.08, 0.30},
        {"Nebula Expanse", 0.85, 1.10, 1.05, 1.55, 0.90, 1.20, 0.22, 0.18, 0.45, 0.10, 0.35},
        {"Shattered Belt", 1.25, 1.75, 0.80, 1.05, 1.10, 1.45, 0.04, 0.20, 0.55, 0.12, 0.40},
        {"Pirate Reach", 0.90, 1.20, 0.90, 1.25, 1.05, 1.35, 0.06, 0.70, 0.95, 0.05, 0.22},
        {"Precursor Graveyard", 1.00, 1.35, 0.90, 1.20, 1.25, 1.70, 0.10, 0.18, 0.55, 0.55, 0.95},
    }};

    static const std::array<const char*, 22> kAdj = {
        "Shattered", "Veiled", "Silent", "Crimson", "Azure", "Golden", "Emerald", "Obsidian", "Radiant", "Forgotten",
        "Broken", "Burning", "Frozen", "Storm", "Iron", "Hollow", "Sundered", "Distant", "Outer", "Inner", "Twilight",
        "Gilded"};
    static const std::array<const char*, 22> kNoun = {
        "Reach", "Expanse", "Marches", "Frontier", "Rift", "Gulf", "Arc", "Belt", "Corridor", "Trench", "Dominion",
        "Enclave", "Crown", "Abyss", "Wastes", "Spiral", "Veil", "Divide", "Shoals", "Provinces", "Wilds", "Run"};

    auto gen_region_name = [&](std::unordered_set<std::string>& used) -> std::string {
      for (int attempt = 0; attempt < 200; ++attempt) {
        const char* adj = kAdj[static_cast<std::size_t>(rand_int(rrng, 0, static_cast<int>(kAdj.size()) - 1))];
        const char* noun = kNoun[static_cast<std::size_t>(rand_int(rrng, 0, static_cast<int>(kNoun.size()) - 1))];
        const double u = rand_unit(rrng);

        std::string name;
        if (u < 0.30) {
          name = std::string("The ") + adj + " " + noun;
        } else if (u < 0.70) {
          name = std::string(adj) + " " + noun;
        } else {
          name = std::string(adj) + " " + noun + " Sector";
        }

        if (used.insert(name).second) return name;
      }

      // Deterministic fallback.
      std::string name = "Unnamed Sector";
      int n = 2;
      while (!used.insert(name).second) {
        name = "Unnamed Sector " + std::to_string(n++);
      }
      return name;
    };

    auto pick_theme_index = [&](const Vec2& center_pos) -> int {
      const double r = center_pos.length();
      const double rn = (galaxy_radius > 1e-6) ? std::clamp(r / galaxy_radius, 0.0, 1.0) : 0.0;
      const double neb = compute_nebula_density(seed ^ 0x51EC7A11u, center_pos, galaxy_radius);

      // A low-frequency "geology" field to create belts/wastes.
      const double geo = fbm_noise_2d(seed ^ 0x9E3779B9u, center_pos.x * 0.18, center_pos.y * 0.18, 4);
      const double u = rand_unit(rrng);

      // Strong nebula => Nebula Expanse.
      if (neb > 0.60) return 2;

      // Rare special themes.
      if (u > 0.965) return 5; // Precursor Graveyard
      if (u > 0.92 && rn > 0.45) return 4; // Pirate Reach (prefer outer regions)

      // Belts / dead zones from geo field.
      if (geo > 0.80) return 3;     // Shattered Belt
      if (geo < 0.14 && rn > 0.35) return 1; // empty-ish Frontier

      // Core tends to be richer/safer.
      if (rn < 0.25 && u < 0.80) return 0;

      // Default to Frontier.
      return 1;
    };

    // Pick region seed systems using farthest-point sampling for even coverage.
    std::vector<int> center_idxs;
    center_idxs.reserve(static_cast<std::size_t>(region_count));
    center_idxs.push_back(0); // ensure the home system has a named region

    for (int r = 1; r < region_count; ++r) {
      int best_i = -1;
      double best_score = -1.0;

      for (int i = 0; i < num_systems; ++i) {
        bool is_center = false;
        for (int c : center_idxs) {
          if (c == i) {
            is_center = true;
            break;
          }
        }
        if (is_center) continue;

        double min_d = std::numeric_limits<double>::infinity();
        for (int c : center_idxs) {
          const Vec2 d = systems[static_cast<std::size_t>(i)].galaxy_pos - systems[static_cast<std::size_t>(c)].galaxy_pos;
          min_d = std::min(min_d, d.length());
        }

        const double score = min_d * (1.0 + 0.02 * rand_unit(rrng));
        if (best_i < 0 || score > best_score) {
          best_i = i;
          best_score = score;
        }
      }

      if (best_i < 0) break;
      center_idxs.push_back(best_i);
    }

    // Choose region themes, then optionally force at least one pirate-heavy region
    // when pirates are enabled (so the "Pirate Reach" concept actually shows up).
    std::vector<int> theme_idxs;
    theme_idxs.reserve(center_idxs.size());
    int pirate_theme_count = 0;
    for (int ci : center_idxs) {
      const int t = pick_theme_index(systems[static_cast<std::size_t>(ci)].galaxy_pos);
      theme_idxs.push_back(t);
      if (t == 4) ++pirate_theme_count;
    }
    if (pirates_enabled && pirate_theme_count == 0 && theme_idxs.size() >= 2) {
      int best = 1;
      double best_d = -1.0;
      for (std::size_t j = 1; j < center_idxs.size(); ++j) {
        const int ci = center_idxs[j];
        const double d = (systems[static_cast<std::size_t>(ci)].galaxy_pos - systems.front().galaxy_pos).length();
        if (d > best_d) {
          best_d = d;
          best = static_cast<int>(j);
        }
      }
      theme_idxs[static_cast<std::size_t>(best)] = 4;
    }

    std::vector<Id> region_ids;
    region_ids.reserve(center_idxs.size());
    std::unordered_set<std::string> used_region_names;
    used_region_names.reserve(center_idxs.size() * 2);

    auto sample_range = [&](double lo, double hi) {
      return rand_real(rrng, lo, hi);
    };

    for (std::size_t j = 0; j < center_idxs.size(); ++j) {
      const int ci = center_idxs[j];
      const RegionThemeDef& def = kThemes[static_cast<std::size_t>(std::clamp(theme_idxs[j], 0, (int)kThemes.size() - 1))];

      Region reg;
      reg.id = allocate_id(s);
      reg.name = gen_region_name(used_region_names);
      reg.center = systems[static_cast<std::size_t>(ci)].galaxy_pos;
      reg.theme = def.theme;

      reg.mineral_richness_mult = std::clamp(sample_range(def.mineral_lo, def.mineral_hi), 0.55, 2.25);
      reg.volatile_richness_mult = std::clamp(sample_range(def.volatile_lo, def.volatile_hi), 0.55, 2.25);
      reg.salvage_richness_mult = std::clamp(sample_range(def.salvage_lo, def.salvage_hi), 0.40, 2.75);

      reg.nebula_bias = std::clamp(def.nebula_bias + sample_range(-0.05, 0.05), -0.35, 0.35);
      reg.pirate_risk = std::clamp(sample_range(def.pirate_lo, def.pirate_hi), 0.0, 1.0);
      reg.ruins_density = std::clamp(sample_range(def.ruins_lo, def.ruins_hi), 0.0, 1.0);

      s.regions[reg.id] = reg;
      region_ids.push_back(reg.id);
    }

    // Assign each system to its nearest region seed (Voronoi).
    for (int i = 0; i < num_systems; ++i) {
      const Vec2 p = systems[static_cast<std::size_t>(i)].galaxy_pos;

      std::size_t best_j = 0;
      double best_d = std::numeric_limits<double>::infinity();
      for (std::size_t j = 0; j < region_ids.size(); ++j) {
        const auto* reg = find_ptr(s.regions, region_ids[j]);
        if (!reg) continue;
        const double d = (p - reg->center).length();
        if (d < best_d) {
          best_d = d;
          best_j = j;
        }
      }

      const Id rid = region_ids.empty() ? kInvalidId : region_ids[best_j];
      if (auto* sys = find_ptr(s.systems, systems[static_cast<std::size_t>(i)].id)) {
        sys->region_id = rid;
      }
      systems[static_cast<std::size_t>(i)].region_id = rid;
    }

    // Apply region modifiers.
    for (int i = 0; i < num_systems; ++i) {
      auto* sys = find_ptr(s.systems, systems[static_cast<std::size_t>(i)].id);
      if (!sys) continue;
      const auto* reg = find_ptr(s.regions, sys->region_id);
      if (!reg) continue;

      sys->nebula_density = std::clamp(sys->nebula_density + reg->nebula_bias, 0.0, 1.0);
      systems[static_cast<std::size_t>(i)].nebula_density = sys->nebula_density;
    }

    // Keep the home system readable if requested.
    if (cfg.ensure_clear_home) {
      if (auto* hs = find_ptr(s.systems, home_system)) {
        hs->nebula_density = std::min(hs->nebula_density, 0.25);
        systems.front().nebula_density = hs->nebula_density;
      }
    }

    // Scale mineral deposits by region richness.
    for (auto& [bid, b] : s.bodies) {
      (void)bid;
      const auto* sys = find_ptr(s.systems, b.system_id);
      if (!sys) continue;
      const auto* reg = find_ptr(s.regions, sys->region_id);
      if (!reg) continue;

      for (auto& kv : b.mineral_deposits) {
        const std::string& key = kv.first;
        const double mult = (key == "Sorium" || key == "Fuel") ? reg->volatile_richness_mult : reg->mineral_richness_mult;
        kv.second = std::max(0.0, kv.second * mult);
      }
    }
  }



  // Optional pirate start system: prefer far systems in pirate-heavy regions.
  int pirate_idx = -1;
  Id pirate_system = kInvalidId;
  if (pirates != kInvalidId) {
    pirate_idx = 0;
    if (num_systems > 1) {
      std::mt19937 prng(seed ^ 0xBADA55E5u);

      double best_score = -1.0;
      for (int i = 1; i < num_systems; ++i) {
        const double dist = (systems[static_cast<std::size_t>(i)].galaxy_pos - systems.front().galaxy_pos).length();

        double risk = 0.25;
        if (const auto* reg = find_ptr(s.regions, systems[static_cast<std::size_t>(i)].region_id)) {
          risk = reg->pirate_risk;
        }

        // Distance dominates, but region risk can tilt the choice.
        const double score = dist * (0.70 + 0.80 * clamp01(risk)) * (1.0 + 0.03 * rand_unit(prng));

        if (score > best_score) {
          best_score = score;
          pirate_idx = i;
        }
      }
    }
    pirate_system = systems[static_cast<std::size_t>(pirate_idx)].id;
  }


  // Additional major AI empires.
  int num_ai_empires = cfg.num_ai_empires;
  if (num_ai_empires < 0) {
    // "Auto" scales gently with galaxy size.
    num_ai_empires = std::clamp((num_systems - 4) / 8, 0, 6);
  }
  {
    const int reserved = 1 + ((pirate_idx >= 0) ? 1 : 0);
    const int max_ai = std::max(0, num_systems - reserved);
    num_ai_empires = std::clamp(num_ai_empires, 0, max_ai);
  }

  // Choose empire home systems using farthest-point sampling (spread starts out).
  std::vector<int> empire_idxs;
  empire_idxs.reserve(static_cast<std::size_t>(num_ai_empires));

  std::vector<int> reserved_idxs;
  reserved_idxs.reserve(static_cast<std::size_t>(2 + num_ai_empires));
  reserved_idxs.push_back(0);
  if (pirate_idx >= 0 && pirate_idx != 0) reserved_idxs.push_back(pirate_idx);

  std::vector<char> reserved_flag(static_cast<std::size_t>(num_systems), 0);
  reserved_flag[0] = 1;
  if (pirate_idx >= 0) reserved_flag[static_cast<std::size_t>(pirate_idx)] = 1;

  for (int e = 0; e < num_ai_empires; ++e) {
    int best_i = -1;
    double best_score = -1.0;

    for (int i = 1; i < num_systems; ++i) {
      if (reserved_flag[static_cast<std::size_t>(i)]) continue;

      double min_d = std::numeric_limits<double>::infinity();
      for (int r : reserved_idxs) {
        const Vec2 d = systems[static_cast<std::size_t>(i)].galaxy_pos - systems[static_cast<std::size_t>(r)].galaxy_pos;
        min_d = std::min(min_d, d.length());
      }

      // Small deterministic jitter so ties don't always pick the lowest index.
      const double score = min_d * (1.0 + 0.02 * rand_unit(rng));
      if (best_i < 0 || score > best_score) {
        best_i = i;
        best_score = score;
      }
    }

    if (best_i < 0) break;
    reserved_flag[static_cast<std::size_t>(best_i)] = 1;
    reserved_idxs.push_back(best_i);
    empire_idxs.push_back(best_i);
  }

  struct EmpireStart {
    Id faction_id{kInvalidId};
    int system_idx{0};
    Id system_id{kInvalidId};
    Id capital_body{kInvalidId};

    // 0..1: higher => more likely to start hostile.
    double aggression{0.0};
  };

  std::vector<EmpireStart> empires;
  empires.reserve(empire_idxs.size());

  // Generate unique empire names.
  std::unordered_set<std::string> used_faction_names;
  used_faction_names.reserve(8 + empire_idxs.size());
  used_faction_names.insert(s.factions.at(terrans).name);
  if (pirates != kInvalidId) used_faction_names.insert(s.factions.at(pirates).name);

  static const std::array<const char*, 10> kGovTypes = {
      "Republic", "Union", "Directorate", "League", "Collective",
      "Confederacy", "Dominion", "Commonwealth", "Hegemony", "Mandate"};
  static const std::array<const char*, 18> kAdjectives = {
      "Orion", "Astra", "Helios", "Cygnus", "Lyra", "Vega", "Nova", "Solar",
      "Stellar", "Crimson", "Azure", "Golden", "Emerald", "Obsidian", "Radiant",
      "Silent", "Frontier", "Arcadian"};

  auto gen_empire_name = [&](const std::string& capital) -> std::string {
    for (int attempt = 0; attempt < 200; ++attempt) {
      const char* gov = kGovTypes[static_cast<std::size_t>(rand_int(rng, 0, static_cast<int>(kGovTypes.size()) - 1))];
      const char* adj = kAdjectives[static_cast<std::size_t>(rand_int(rng, 0, static_cast<int>(kAdjectives.size()) - 1))];
      const double u = rand_unit(rng);

      std::string name;
      if (u < 0.45) {
        name = std::string(gov) + " of " + capital;
      } else if (u < 0.75) {
        name = std::string(adj) + " " + gov;
      } else {
        name = capital + " " + gov;
      }

      if (used_faction_names.insert(name).second) return name;
    }

    // Deterministic fallback.
    std::string name = "Empire of " + capital;
    int n = 2;
    while (!used_faction_names.insert(name).second) {
      name = "Empire of " + capital + " " + std::to_string(n++);
    }
    return name;
  };

  auto doctrine_queue = [&](double aggression) -> std::vector<std::string> {
    // All doctrines start with nuclear_1 (unlocks early propulsion + many other lines).
    //
    // We bias the selection using a single "aggression" scalar:
    //   - low aggression => more industrial / terraforming
    //   - high aggression => more militarist
    const double a = clamp01(aggression);

    const double w_mil = std::clamp(0.15 + 0.45 * a, 0.05, 0.70);
    const double w_ind = std::clamp(0.35 - 0.15 * a, 0.10, 0.45);
    const double w_ter = std::clamp(0.20 - 0.10 * a, 0.05, 0.25);
    const double w_exp = std::max(0.05, 1.0 - (w_mil + w_ind + w_ter));
    const double total = (w_mil + w_ind + w_ter + w_exp);

    double x = rand_unit(rng) * total;
    if (x < w_ind) {
      return {"nuclear_1", "materials_processing_1", "industrial_efficiency_1", "construction_methods_1", "mining_1"};
    }
    x -= w_ind;

    if (x < w_ter) {
      return {"nuclear_1", "propulsion_1", "colonization_1", "terraforming_1", "research_methods_1"};
    }
    x -= w_ter;

    if (x < w_exp) {
      return {"nuclear_1", "propulsion_1", "sensors_1", "colonization_1", "mining_1"};
    }

    // Militarist
    return {"nuclear_1", "propulsion_1", "weapons_1", "armor_1", "sensors_1"};
  };


  // Create the empires.
  for (int idx : empire_idxs) {
    const SysInfo& sys = systems[static_cast<std::size_t>(idx)];
    const Id fid = allocate_id(s);

    // Aggression is a small faction "personality" knob for initial diplomacy.
    const double aggression = clamp01(rand_real(rng, 0.0, 1.0));

    Faction f;
    f.id = fid;
    f.control = FactionControl::AI_Explorer;
    f.name = gen_empire_name(sys.name);
    f.research_points = 0.0;
    f.known_techs = {"chemistry_1"};
    f.research_queue = doctrine_queue(aggression);
    s.factions[fid] = f;

    EmpireStart es;
    es.faction_id = fid;
    es.system_idx = idx;
    es.system_id = sys.id;
    es.capital_body = (sys.preferred_colony_body != kInvalidId) ? sys.preferred_colony_body : sys.primary_star;
    es.aggression = aggression;
    empires.push_back(es);
  }

  s.selected_system = home_system;

  // Seed initial discovery: each faction knows its start system.
  s.factions[terrans].discovered_systems = {home_system};
  if (pirates != kInvalidId) s.factions[pirates].discovered_systems = {pirate_system};
  for (const auto& es : empires) {
    s.factions[es.faction_id].discovered_systems = {es.system_id};
  }

  // Initialize diplomacy stances.
  // Missing relations default to Hostile (legacy behavior), so we seed reasonable defaults here.
  auto set_mutual_relation = [&](Id a, Id b, DiplomacyStatus st) {
    if (a == kInvalidId || b == kInvalidId || a == b) return;
    s.factions[a].relations[b] = st;
    s.factions[b].relations[a] = st;
  };

  // Civilized factions: player + major AI empires.
  std::vector<Id> civilized;
  civilized.reserve(1 + empires.size());
  civilized.push_back(terrans);
  for (const auto& es : empires) civilized.push_back(es.faction_id);

  auto aggression_of = [&](Id fid) -> double {
    if (fid == terrans) return 0.0;
    for (const auto& es : empires) {
      if (es.faction_id == fid) return es.aggression;
    }
    return 0.5;
  };

  for (std::size_t i = 0; i < civilized.size(); ++i) {
    for (std::size_t j = i + 1; j < civilized.size(); ++j) {
      const Id a = civilized[i];
      const Id b = civilized[j];
      const double aa = aggression_of(a);
      const double bb = aggression_of(b);

      // Player diplomacy is a bit friendlier by default.
      const bool player_pair = (a == terrans || b == terrans);
      const double ag = std::max(aa, bb);

      double p_friendly = player_pair ? (0.18 - 0.10 * ag) : (0.08 - 0.04 * (aa + bb));
      double p_hostile = player_pair ? (0.08 + 0.22 * ag) : (0.14 + 0.25 * (aa * bb));
      p_friendly = std::clamp(p_friendly, 0.02, 0.25);
      p_hostile = std::clamp(p_hostile, 0.05, 0.55);

      const double u = rand_unit(rng);
      DiplomacyStatus st = DiplomacyStatus::Neutral;
      if (u < p_friendly) {
        st = DiplomacyStatus::Friendly;
      } else if (u > (1.0 - p_hostile)) {
        st = DiplomacyStatus::Hostile;
      }
      set_mutual_relation(a, b, st);
    }
  }

  // Pirates are hostile to everyone (and vice versa).
  if (pirates != kInvalidId) {
    for (Id fid : civilized) {
      set_mutual_relation(pirates, fid, DiplomacyStatus::Hostile);
    }
  }

  // Ensure the homeworld is stable and generous.
  Id homeworld_body = systems.front().preferred_colony_body;
  if (homeworld_body == kInvalidId) homeworld_body = systems.front().primary_star;

  if (auto* hb = find_ptr(s.bodies, homeworld_body)) {
    hb->orbit_phase_radians = 0.0;

    // Ensure the homeworld is habitable (used by the colony habitability system).
    // The procedural generator already tries to place a rocky planet near the HZ, but
    // we still defensively fill in any missing environment values.
    if (hb->surface_temp_k <= 0.0) hb->surface_temp_k = 288.0;
    if (hb->atmosphere_atm <= 0.0) hb->atmosphere_atm = 1.0;
    hb->terraforming_target_temp_k = hb->surface_temp_k;
    hb->terraforming_target_atm = hb->atmosphere_atm;
    hb->terraforming_complete = true;

    if (auto it = hb->mineral_deposits.find("Duranium"); it != hb->mineral_deposits.end()) it->second *= 1.6;
    if (auto it = hb->mineral_deposits.find("Neutronium"); it != hb->mineral_deposits.end()) it->second *= 1.6;
  }

  // Make empire capitals habitable and slightly above-average so AI can actually play.
  for (auto& es : empires) {
    if (auto* cb = find_ptr(s.bodies, es.capital_body)) {
      cb->orbit_phase_radians = 0.0;
      if (cb->surface_temp_k <= 0.0) cb->surface_temp_k = rand_real(rng, 275.0, 305.0);
      if (cb->atmosphere_atm <= 0.0) cb->atmosphere_atm = rand_real(rng, 0.9, 1.2);
      cb->terraforming_target_temp_k = cb->surface_temp_k;
      cb->terraforming_target_atm = cb->atmosphere_atm;
      cb->terraforming_complete = true;

      // Gentle boost: not as strong as the player homeworld, but avoids immediate mineral starvation.
      for (auto& kv : cb->mineral_deposits) {
        kv.second *= rand_real(rng, 1.05, 1.25);
      }
    }
  }

    // --- Jump points ---
  //
  // Build a connected "hyperlane" graph over galaxy space. The chosen archetype
  // intentionally changes strategic topology (dense webs vs. sparse chokepoints).
  const int jump_style_i = std::clamp(static_cast<int>(cfg.jump_network_style), 0, 5);
  const RandomJumpNetworkStyle jump_style = static_cast<RandomJumpNetworkStyle>(jump_style_i);
  const double jump_density = std::clamp(cfg.jump_density, 0.0, 2.0);

  std::unordered_set<std::uint64_t> edges;
  edges.reserve(static_cast<std::size_t>(num_systems * 8));

  auto system_dist = [&](int a, int b) {
    return (systems[static_cast<std::size_t>(a)].galaxy_pos - systems[static_cast<std::size_t>(b)].galaxy_pos).length();
  };

  auto add_jump_link = [&](int a_idx, int b_idx) -> bool {
    if (a_idx == b_idx) return false;
    const std::uint64_t k = edge_key(a_idx, b_idx);
    if (!edges.insert(k).second) return false;

    // Create two jump points with random positions inside each system's map.
    auto make_pos = [&](const SysInfo& from) -> Vec2 {
      const double r = std::max(30.0, from.max_orbit_extent_mkm * 0.35);
      const double ang = rand_real(rng, 0.0, kTwoPi);
      const double rad = r * (0.65 + 0.4 * rand_unit(rng));
      return {rad * std::cos(ang), rad * std::sin(ang)};
    };

    const SysInfo& sa = systems[static_cast<std::size_t>(a_idx)];
    const SysInfo& sb = systems[static_cast<std::size_t>(b_idx)];

    const Id jp_a = allocate_id(s);
    const Id jp_b = allocate_id(s);

    JumpPoint a;
    a.id = jp_a;
    a.system_id = sa.id;
    a.name = "Jump Point";
    a.position_mkm = make_pos(sa);
    a.linked_jump_id = jp_b;

    JumpPoint b;
    b.id = jp_b;
    b.system_id = sb.id;
    b.name = "Jump Point";
    b.position_mkm = make_pos(sb);
    b.linked_jump_id = jp_a;

    s.jump_points[jp_a] = a;
    s.jump_points[jp_b] = b;
    s.systems[sa.id].jump_points.push_back(jp_a);
    s.systems[sb.id].jump_points.push_back(jp_b);
    return true;
  };

  // Prim MST over a subset of nodes (indices into `systems`). Ensures connectivity.
  auto connect_mst = [&](const std::vector<int>& nodes) {
    const int n = static_cast<int>(nodes.size());
    if (n <= 1) return;

    std::vector<double> best(static_cast<std::size_t>(n), std::numeric_limits<double>::infinity());
    std::vector<int> parent(static_cast<std::size_t>(n), -1);
    std::vector<std::uint8_t> used(static_cast<std::size_t>(n), 0);

    best[0] = 0.0;

    for (int it = 0; it < n; ++it) {
      int u = -1;
      double u_best = std::numeric_limits<double>::infinity();
      for (int i = 0; i < n; ++i) {
        if (used[static_cast<std::size_t>(i)]) continue;
        const double v = best[static_cast<std::size_t>(i)];
        if (v < u_best) {
          u_best = v;
          u = i;
        }
      }
      if (u == -1) break;

      used[static_cast<std::size_t>(u)] = 1;
      if (parent[static_cast<std::size_t>(u)] != -1) {
        add_jump_link(nodes[static_cast<std::size_t>(u)], nodes[static_cast<std::size_t>(parent[static_cast<std::size_t>(u)])]);
      }

      for (int v = 0; v < n; ++v) {
        if (used[static_cast<std::size_t>(v)]) continue;
        if (v == u) continue;
        const double d = system_dist(nodes[static_cast<std::size_t>(u)], nodes[static_cast<std::size_t>(v)]);
        if (d < best[static_cast<std::size_t>(v)]) {
          best[static_cast<std::size_t>(v)] = d;
          parent[static_cast<std::size_t>(v)] = u;
        }
      }
    }
  };

  // Connect each node to k nearest neighbors (within the given node set).
  auto connect_k_nearest = [&](const std::vector<int>& nodes, int k) {
    const int n = static_cast<int>(nodes.size());
    if (n <= 1) return;
    k = std::clamp(k, 1, n - 1);

    struct Cand {
      double d{0.0};
      int other{-1};
    };

    std::vector<Cand> cands;
    cands.reserve(static_cast<std::size_t>(n - 1));

    for (int ii = 0; ii < n; ++ii) {
      cands.clear();
      const int a = nodes[static_cast<std::size_t>(ii)];
      for (int jj = 0; jj < n; ++jj) {
        if (ii == jj) continue;
        const int b = nodes[static_cast<std::size_t>(jj)];
        cands.push_back({system_dist(a, b), b});
      }

      std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.d < y.d; });

      const int take = std::min(k, static_cast<int>(cands.size()));
      for (int t = 0; t < take; ++t) {
        add_jump_link(a, cands[static_cast<std::size_t>(t)].other);
      }
    }
  };

  // Add shortest missing edges until the undirected edge count reaches target_total.
  auto add_shortest_until = [&](int target_total) {
    if (num_systems <= 1) return;
    target_total = std::clamp(target_total, 0, (num_systems * (num_systems - 1)) / 2);
    if (static_cast<int>(edges.size()) >= target_total) return;

    struct Edge {
      double d{0.0};
      int a{-1};
      int b{-1};
    };

    std::vector<Edge> pairs;
    pairs.reserve(static_cast<std::size_t>(num_systems * (num_systems - 1) / 2));
    for (int i = 0; i < num_systems; ++i) {
      for (int j = i + 1; j < num_systems; ++j) {
        pairs.push_back({system_dist(i, j), i, j});
      }
    }
    std::sort(pairs.begin(), pairs.end(), [](const Edge& x, const Edge& y) { return x.d < y.d; });

    for (const auto& e : pairs) {
      if (static_cast<int>(edges.size()) >= target_total) break;
      add_jump_link(e.a, e.b);
    }
  };

  // All systems as indices.
  std::vector<int> all_nodes;
  all_nodes.reserve(static_cast<std::size_t>(num_systems));
  for (int i = 0; i < num_systems; ++i) all_nodes.push_back(i);

  // --- Style selection ---
  switch (jump_style) {
    case RandomJumpNetworkStyle::Balanced: {
      // Baseline: MST + some extra local edges (old behavior, but density-scalable).
      connect_mst(all_nodes);

      const int base_extra = std::max(0, num_systems / 2);
      const int extra_target = std::clamp(static_cast<int>(std::llround(static_cast<double>(base_extra) * jump_density)), 0,
                                          (num_systems * (num_systems - 1)) / 2);
      const int target_total = (num_systems > 0) ? std::min((num_systems - 1) + extra_target, (num_systems * (num_systems - 1)) / 2) : 0;

      if (num_systems >= 3 && extra_target > 0) {
        struct Edge {
          double d{0.0};
          int a{-1};
          int b{-1};
        };

        std::vector<Edge> pairs;
        pairs.reserve(static_cast<std::size_t>(num_systems * (num_systems - 1) / 2));
        for (int i = 0; i < num_systems; ++i) {
          for (int j = i + 1; j < num_systems; ++j) {
            pairs.push_back({system_dist(i, j), i, j});
          }
        }
        std::sort(pairs.begin(), pairs.end(), [](const Edge& x, const Edge& y) { return x.d < y.d; });

        // Prefer adding shorter edges; probability decays with distance.
        double maxd = pairs.empty() ? 1.0 : pairs.back().d;
        const double link_scale = std::max(1e-6, maxd * 0.45);

        for (const auto& e : pairs) {
          if (static_cast<int>(edges.size()) >= target_total) break;
          const double prob = 0.40 * std::exp(-e.d / link_scale);
          if (rand_unit(rng) < prob) add_jump_link(e.a, e.b);
        }

        // If RNG didn't hit the target, deterministically fill with shortest remaining edges.
        add_shortest_until(target_total);
      }
      break;
    }

    case RandomJumpNetworkStyle::DenseWeb: {
      // Dense web: MST + k-nearest neighbor links + a few long-range shortcuts.
      connect_mst(all_nodes);

      const int k = std::clamp(2 + static_cast<int>(std::llround(2.0 * jump_density)), 2, 8);
      connect_k_nearest(all_nodes, k);

      // Long-range shortcuts: pick from the far tail of distance-sorted pairs.
      const int long_target = std::clamp(static_cast<int>(std::llround(static_cast<double>(num_systems) * 0.08 * jump_density)), 0, 24);

      if (num_systems >= 4 && long_target > 0) {
        struct Edge {
          double d{0.0};
          int a{-1};
          int b{-1};
        };

        std::vector<Edge> pairs;
        pairs.reserve(static_cast<std::size_t>(num_systems * (num_systems - 1) / 2));
        for (int i = 0; i < num_systems; ++i) {
          for (int j = i + 1; j < num_systems; ++j) {
            pairs.push_back({system_dist(i, j), i, j});
          }
        }
        std::sort(pairs.begin(), pairs.end(), [](const Edge& x, const Edge& y) { return x.d < y.d; });

        const int start = static_cast<int>(pairs.size() * 0.65);
        if (start < static_cast<int>(pairs.size())) {
          std::uniform_int_distribution<int> pick(start, static_cast<int>(pairs.size()) - 1);
          int added = 0;
          for (int tries = 0; tries < 200 && added < long_target; ++tries) {
            const auto& e = pairs[static_cast<std::size_t>(pick(rng))];
            if (add_jump_link(e.a, e.b)) ++added;
          }
        }
      }
      break;
    }

    case RandomJumpNetworkStyle::SparseLanes: {
      // Sparse lanes: build a backbone path along the principal axis of the galaxy,
      // then (optionally) add a small number of short bypass edges.
      if (num_systems <= 1) break;

      Vec2 mean{0.0, 0.0};
      for (const auto& sys : systems) mean += sys.galaxy_pos;
      mean = mean * (1.0 / static_cast<double>(num_systems));

      double xx = 0.0, yy = 0.0, xy = 0.0;
      for (const auto& sys : systems) {
        const double dx = sys.galaxy_pos.x - mean.x;
        const double dy = sys.galaxy_pos.y - mean.y;
        xx += dx * dx;
        yy += dy * dy;
        xy += dx * dy;
      }

      // Direction of the dominant eigenvector of the 2x2 covariance matrix.
      double theta = 0.0;
      if (std::fabs(xy) > 1e-9 || std::fabs(xx - yy) > 1e-9) {
        theta = 0.5 * std::atan2(2.0 * xy, xx - yy);
      } else {
        // Nearly isotropic: pick a deterministic direction from the seed.
        const double u = static_cast<double>(hash_u32(seed ^ 0xA1B2C3D4u) & 0xFFFFu) / 65535.0;
        theta = u * kTwoPi;
      }

      const Vec2 axis{std::cos(theta), std::sin(theta)};

      std::vector<std::pair<double, int>> order;
      order.reserve(static_cast<std::size_t>(num_systems));
      for (int i = 0; i < num_systems; ++i) {
        const Vec2 p = systems[static_cast<std::size_t>(i)].galaxy_pos - mean;
        const double t = p.x * axis.x + p.y * axis.y;
        order.push_back({t, i});
      }
      std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

      // Backbone.
      for (int i = 0; i + 1 < num_systems; ++i) {
        add_jump_link(order[static_cast<std::size_t>(i)].second, order[static_cast<std::size_t>(i + 1)].second);
      }

      // Bypasses.
      const int extra = std::clamp(static_cast<int>(std::llround(static_cast<double>(std::max(0, num_systems / 8)) * jump_density)), 0,
                                   (num_systems * (num_systems - 1)) / 2);
      const int target_total = std::min((num_systems - 1) + extra, (num_systems * (num_systems - 1)) / 2);
      add_shortest_until(target_total);
      break;
    }

    case RandomJumpNetworkStyle::ClusterBridges: {
      // Cluster bridges: form k clusters, add dense intra-cluster links, then connect clusters with a thin backbone.
      if (num_systems <= 1) break;

      int k = std::clamp(num_systems / 12, 2, 6);
      k = std::min(k, num_systems);

      // Initialize centers via farthest-point sampling (deterministic).
      std::vector<int> centers_idx;
      centers_idx.reserve(static_cast<std::size_t>(k));
      centers_idx.push_back(0);
      while (static_cast<int>(centers_idx.size()) < k) {
        int best_i = -1;
        double best_d = -1.0;
        for (int i = 0; i < num_systems; ++i) {
          bool is_center = false;
          for (int c : centers_idx) {
            if (c == i) {
              is_center = true;
              break;
            }
          }
          if (is_center) continue;

          double dmin = std::numeric_limits<double>::infinity();
          for (int c : centers_idx) dmin = std::min(dmin, system_dist(i, c));
          if (dmin > best_d) {
            best_d = dmin;
            best_i = i;
          }
        }
        if (best_i < 0) break;
        centers_idx.push_back(best_i);
      }

      std::vector<Vec2> centers;
      centers.reserve(static_cast<std::size_t>(k));
      for (int ci = 0; ci < k; ++ci) {
        centers.push_back(systems[static_cast<std::size_t>(centers_idx[static_cast<std::size_t>(ci)])].galaxy_pos);
      }

      std::vector<int> assign(static_cast<std::size_t>(num_systems), 0);

      // A few Lloyd iterations.
      for (int it = 0; it < 4; ++it) {
        // Assign.
        for (int i = 0; i < num_systems; ++i) {
          int best_c = 0;
          double best = std::numeric_limits<double>::infinity();
          for (int ci = 0; ci < k; ++ci) {
            const Vec2 d = systems[static_cast<std::size_t>(i)].galaxy_pos - centers[static_cast<std::size_t>(ci)];
            const double dd = d.x * d.x + d.y * d.y;
            if (dd < best) {
              best = dd;
              best_c = ci;
            }
          }
          assign[static_cast<std::size_t>(i)] = best_c;
        }

        // Recompute centroids.
        std::vector<Vec2> sum(static_cast<std::size_t>(k));
        std::vector<int> cnt(static_cast<std::size_t>(k), 0);
        for (int i = 0; i < num_systems; ++i) {
          const int ci = assign[static_cast<std::size_t>(i)];
          sum[static_cast<std::size_t>(ci)] += systems[static_cast<std::size_t>(i)].galaxy_pos;
          cnt[static_cast<std::size_t>(ci)]++;
        }
        for (int ci = 0; ci < k; ++ci) {
          if (cnt[static_cast<std::size_t>(ci)] > 0) {
            centers[static_cast<std::size_t>(ci)] =
                sum[static_cast<std::size_t>(ci)] * (1.0 / static_cast<double>(cnt[static_cast<std::size_t>(ci)]));
          } else {
            // Empty cluster: re-seed to farthest system from all non-empty centroids.
            int best_i = 0;
            double best_d = -1.0;
            for (int i = 0; i < num_systems; ++i) {
              double dmin = std::numeric_limits<double>::infinity();
              for (int cj = 0; cj < k; ++cj) {
                if (cj == ci) continue;
                const Vec2 d = systems[static_cast<std::size_t>(i)].galaxy_pos - centers[static_cast<std::size_t>(cj)];
                const double dd = d.x * d.x + d.y * d.y;
                dmin = std::min(dmin, dd);
              }
              if (dmin > best_d) {
                best_d = dmin;
                best_i = i;
              }
            }
            centers[static_cast<std::size_t>(ci)] = systems[static_cast<std::size_t>(best_i)].galaxy_pos;
          }
        }
      }

      // Materialize clusters.
      std::vector<std::vector<int>> clusters(static_cast<std::size_t>(k));
      for (int i = 0; i < num_systems; ++i) clusters[static_cast<std::size_t>(assign[static_cast<std::size_t>(i)])].push_back(i);

      // Intra-cluster links.
      const int intra_k = std::clamp(2 + static_cast<int>(std::llround(jump_density)), 2, 6);
      for (auto& cl : clusters) {
        if (cl.size() <= 1) continue;
        connect_mst(cl);
        connect_k_nearest(cl, intra_k);
      }

      // Inter-cluster MST on centroids, bridged by closest system pair between clusters.
      std::unordered_set<std::uint64_t> cluster_edges;
      cluster_edges.reserve(static_cast<std::size_t>(k * 4));

      if (k >= 2) {
        std::vector<double> best(static_cast<std::size_t>(k), std::numeric_limits<double>::infinity());
        std::vector<int> parent(static_cast<std::size_t>(k), -1);
        std::vector<std::uint8_t> used(static_cast<std::size_t>(k), 0);
        best[0] = 0.0;

        for (int it = 0; it < k; ++it) {
          int u = -1;
          double u_best = std::numeric_limits<double>::infinity();
          for (int i = 0; i < k; ++i) {
            if (used[static_cast<std::size_t>(i)]) continue;
            const double v = best[static_cast<std::size_t>(i)];
            if (v < u_best) {
              u_best = v;
              u = i;
            }
          }
          if (u == -1) break;
          used[static_cast<std::size_t>(u)] = 1;

          if (parent[static_cast<std::size_t>(u)] != -1) {
            const int p = parent[static_cast<std::size_t>(u)];
            const std::uint64_t ck = edge_key(u, p);
            cluster_edges.insert(ck);

            // Bridge by closest system pair.
            double best_pair = std::numeric_limits<double>::infinity();
            int best_a = clusters[static_cast<std::size_t>(u)].front();
            int best_b = clusters[static_cast<std::size_t>(p)].front();
            for (int a : clusters[static_cast<std::size_t>(u)]) {
              for (int b : clusters[static_cast<std::size_t>(p)]) {
                const double d = system_dist(a, b);
                if (d < best_pair) {
                  best_pair = d;
                  best_a = a;
                  best_b = b;
                }
              }
            }
            add_jump_link(best_a, best_b);
          }

          for (int v = 0; v < k; ++v) {
            if (used[static_cast<std::size_t>(v)]) continue;
            if (v == u) continue;
            const Vec2 d = centers[static_cast<std::size_t>(u)] - centers[static_cast<std::size_t>(v)];
            const double dd = std::sqrt(d.x * d.x + d.y * d.y);
            if (dd < best[static_cast<std::size_t>(v)]) {
              best[static_cast<std::size_t>(v)] = dd;
              parent[static_cast<std::size_t>(v)] = u;
            }
          }
        }

        // Optional extra bridges when density > 1.0.
        const int extra = std::clamp(static_cast<int>(std::llround(std::max(0.0, jump_density - 1.0) * static_cast<double>(k))), 0, 16);
        if (extra > 0) {
          struct CEdge {
            double d{0.0};
            int a{-1};
            int b{-1};
          };
          std::vector<CEdge> cpairs;
          for (int a = 0; a < k; ++a) {
            for (int b = a + 1; b < k; ++b) {
              const Vec2 d = centers[static_cast<std::size_t>(a)] - centers[static_cast<std::size_t>(b)];
              cpairs.push_back({std::sqrt(d.x * d.x + d.y * d.y), a, b});
            }
          }
          std::sort(cpairs.begin(), cpairs.end(), [](const CEdge& x, const CEdge& y) { return x.d < y.d; });

          int added = 0;
          for (const auto& ce : cpairs) {
            if (added >= extra) break;
            const std::uint64_t ck = edge_key(ce.a, ce.b);
            if (cluster_edges.find(ck) != cluster_edges.end()) continue;
            cluster_edges.insert(ck);

            double best_pair = std::numeric_limits<double>::infinity();
            int best_a = clusters[static_cast<std::size_t>(ce.a)].front();
            int best_b = clusters[static_cast<std::size_t>(ce.b)].front();
            for (int a : clusters[static_cast<std::size_t>(ce.a)]) {
              for (int b : clusters[static_cast<std::size_t>(ce.b)]) {
                const double d = system_dist(a, b);
                if (d < best_pair) {
                  best_pair = d;
                  best_a = a;
                  best_b = b;
                }
              }
            }
            if (add_jump_link(best_a, best_b)) ++added;
          }
        }
      }

      break;
    }

    case RandomJumpNetworkStyle::HubAndSpoke: {
      // Hub & spoke: select a few central hubs, connect hubs with a backbone, then attach all systems to a hub.
      if (num_systems <= 1) break;

      // Center of mass in galaxy space.
      Vec2 mean{0.0, 0.0};
      for (const auto& sys : systems) mean += sys.galaxy_pos;
      mean = mean * (1.0 / static_cast<double>(num_systems));

      int hubs_n = std::clamp(num_systems / 20, 1, 3);
      hubs_n = std::min(hubs_n, num_systems);

      struct HubCand {
        double d{0.0};
        int idx{-1};
      };
      std::vector<HubCand> hub_cands;
      hub_cands.reserve(static_cast<std::size_t>(num_systems));
      for (int i = 0; i < num_systems; ++i) {
        const Vec2 p = systems[static_cast<std::size_t>(i)].galaxy_pos - mean;
        hub_cands.push_back({p.x * p.x + p.y * p.y, i});
      }
      std::sort(hub_cands.begin(), hub_cands.end(), [](const HubCand& a, const HubCand& b) { return a.d < b.d; });

      std::vector<int> hubs;
      hubs.reserve(static_cast<std::size_t>(hubs_n));
      std::unordered_set<int> is_hub;
      is_hub.reserve(static_cast<std::size_t>(hubs_n * 2));
      for (int i = 0; i < hubs_n; ++i) {
        hubs.push_back(hub_cands[static_cast<std::size_t>(i)].idx);
        is_hub.insert(hub_cands[static_cast<std::size_t>(i)].idx);
      }

      connect_mst(hubs);

      // Attach every system to its nearest hub (1 edge each).
      for (int i = 0; i < num_systems; ++i) {
        if (is_hub.find(i) != is_hub.end()) continue;
        int best_h = hubs.front();
        double best_d = std::numeric_limits<double>::infinity();
        for (int h : hubs) {
          const double d = system_dist(i, h);
          if (d < best_d) {
            best_d = d;
            best_h = h;
          }
        }
        add_jump_link(i, best_h);
      }

      // Add a few short side-streets between nearby non-hub systems (density-scaled).
      const int side = std::clamp(static_cast<int>(std::llround(static_cast<double>(num_systems) * 0.05 * jump_density)), 0, 24);
      if (side > 0 && num_systems >= 4) {
        struct Edge {
          double d{0.0};
          int a{-1};
          int b{-1};
        };
        std::vector<Edge> pairs;
        for (int i = 0; i < num_systems; ++i) {
          if (is_hub.find(i) != is_hub.end()) continue;
          for (int j = i + 1; j < num_systems; ++j) {
            if (is_hub.find(j) != is_hub.end()) continue;
            pairs.push_back({system_dist(i, j), i, j});
          }
        }
        std::sort(pairs.begin(), pairs.end(), [](const Edge& x, const Edge& y) { return x.d < y.d; });

        int added = 0;
        for (const auto& e : pairs) {
          if (added >= side) break;
          if (add_jump_link(e.a, e.b)) ++added;
        }
      }

      break;
    }

    case RandomJumpNetworkStyle::PlanarProximity: {
      // Planar proximity: use a Delaunay triangulation as the candidate lane set.
      //
      // Properties we want:
      //  - Connected (via MST) so exploration is always possible.
      //  - Mostly planar (subset of Delaunay edges), dramatically reducing map
      //    clutter from crossings.
      //  - Density scales how much of the Delaunay graph is retained.
      if (num_systems <= 1) break;

      std::vector<Vec2> pts;
      pts.reserve(static_cast<std::size_t>(num_systems));
      for (int i = 0; i < num_systems; ++i) {
        pts.push_back(systems[static_cast<std::size_t>(i)].galaxy_pos);
      }

      const auto del = nebula4x::delaunay_edges(pts);

      struct Edge {
        double d{0.0};
        int a{-1};
        int b{-1};
      };

      std::vector<Edge> cand;
      cand.reserve(del.size());
      for (const auto& e : del) {
        if (e.a < 0 || e.b < 0 || e.a >= num_systems || e.b >= num_systems) continue;
        if (e.a == e.b) continue;
        cand.push_back({system_dist(e.a, e.b), e.a, e.b});
      }

      // Degenerate fallback (extremely unlikely for our galaxy samplers).
      if (static_cast<int>(cand.size()) < (num_systems - 1)) {
        connect_mst(all_nodes);
        // Add some extra short edges (same as Balanced).
        const int base_extra = std::max(0, num_systems / 2);
        const int extra_target = std::clamp(static_cast<int>(std::llround(static_cast<double>(base_extra) * jump_density)), 0,
                                            (num_systems * (num_systems - 1)) / 2);
        const int target_total = std::min((num_systems - 1) + extra_target, (num_systems * (num_systems - 1)) / 2);
        add_shortest_until(target_total);
        break;
      }

      std::sort(cand.begin(), cand.end(), [](const Edge& x, const Edge& y) {
        if (x.d != y.d) return x.d < y.d;
        if (x.a != y.a) return x.a < y.a;
        return x.b < y.b;
      });

      // Kruskal MST over the Delaunay candidate edges.
      struct DSU {
        std::vector<int> p;
        std::vector<int> r;
        explicit DSU(int n) : p(static_cast<std::size_t>(n)), r(static_cast<std::size_t>(n), 0) {
          for (int i = 0; i < n; ++i) p[static_cast<std::size_t>(i)] = i;
        }
        int find(int x) {
          int root = x;
          while (p[static_cast<std::size_t>(root)] != root) root = p[static_cast<std::size_t>(root)];
          // Path compression.
          while (p[static_cast<std::size_t>(x)] != x) {
            const int next = p[static_cast<std::size_t>(x)];
            p[static_cast<std::size_t>(x)] = root;
            x = next;
          }
          return root;
        }
        bool unite(int a, int b) {
          a = find(a);
          b = find(b);
          if (a == b) return false;
          if (r[static_cast<std::size_t>(a)] < r[static_cast<std::size_t>(b)]) std::swap(a, b);
          p[static_cast<std::size_t>(b)] = a;
          if (r[static_cast<std::size_t>(a)] == r[static_cast<std::size_t>(b)]) r[static_cast<std::size_t>(a)]++;
          return true;
        }
      };

      DSU dsu(num_systems);
      int mst_need = num_systems - 1;
      for (const auto& e : cand) {
        if (mst_need <= 0) break;
        if (dsu.unite(e.a, e.b)) {
          if (add_jump_link(e.a, e.b)) {
            --mst_need;
          }
        }
      }

      if (mst_need > 0) {
        // Should not happen for valid Delaunay graphs, but keep the game playable.
        connect_mst(all_nodes);
      }

      // Density scaling: treat jump_density in [0,2] as a fraction of the remaining
      // Delaunay edges to include (0 => MST only, 2 => full Delaunay graph).
      const int base = num_systems - 1;
      const int max_edges = static_cast<int>(cand.size());
      const int remaining = std::max(0, max_edges - base);
      const double frac = std::clamp(jump_density / 2.0, 0.0, 1.0);
      const int extra = static_cast<int>(std::llround(static_cast<double>(remaining) * frac));
      const int target_total = std::clamp(base + extra, base, max_edges);

      // Add a small number of longer "shortcuts" first to reduce diameter,
      // then fill with shorter edges for local redundancy.
      const int extra_needed = std::max(0, target_total - static_cast<int>(edges.size()));
      int long_needed = static_cast<int>(std::llround(static_cast<double>(extra_needed) * 0.25));

      for (int i = static_cast<int>(cand.size()) - 1; i >= 0 && long_needed > 0; --i) {
        if (add_jump_link(cand[static_cast<std::size_t>(i)].a, cand[static_cast<std::size_t>(i)].b)) {
          --long_needed;
        }
      }

      for (const auto& e : cand) {
        if (static_cast<int>(edges.size()) >= target_total) break;
        add_jump_link(e.a, e.b);
      }

      break;
    }

    default: {
      // Fallback.
      connect_mst(all_nodes);
      break;
    }
  }

// --- Derelict wrecks / salvage sites (procedural) ---
  // Placed as stationary salvageable objects to create early exploration incentives.
  {
    std::mt19937 wrng(seed ^ 0x7A11A93Du);

    auto add_wreck = [&](Id system_id, const std::string& name, const Vec2& pos_mkm,
                         std::unordered_map<std::string, double> minerals) -> Id {
      const Id wid = allocate_id(s);
      Wreck w;
      w.id = wid;
      w.name = name;
      w.system_id = system_id;
      w.position_mkm = pos_mkm;
      w.minerals = std::move(minerals);
      w.created_day = s.date.days_since_epoch();
      s.wrecks[wid] = std::move(w);
      return wid;
    };

    auto random_offset = [&](double r_lo, double r_hi) {
      const double r = rand_real(wrng, r_lo, r_hi);
      const double a = rand_real(wrng, 0.0, kTwoPi);
      return Vec2{r * std::cos(a), r * std::sin(a)};
    };

    auto salvage_package = [&](double scale, double nebula_density, const Region* reg) {
      std::unordered_map<std::string, double> m;
      const double neb = clamp01(nebula_density);

      const double pirate = reg ? clamp01(reg->pirate_risk) : 0.0;
      const double ruins = reg ? clamp01(reg->ruins_density) : 0.0;

      // Region "salvage richness" is a separate knob from minerals so we can have
      // places that are poor to mine but rewarding to explore (and vice versa).
      const double salv_mult = reg ? std::clamp(reg->salvage_richness_mult, 0.25, 3.5) : 1.0;
      scale *= salv_mult;

      auto add = [&](const char* key, double lo, double hi, double p) {
        if (rand_unit(wrng) < p) m[key] = rand_log_uniform(wrng, lo, hi) * scale;
      };

      // Always some baseline salvage.
      m["Duranium"] = rand_log_uniform(wrng, 60.0, 700.0) * scale;
      add("Neutronium", 20.0, 220.0, 0.70);
      add("Tritanium", 15.0, 260.0, 0.55);
      add("Boronide", 15.0, 240.0, 0.50);
      add("Corbomite", 10.0, 180.0, std::clamp(0.45 + 0.25 * ruins, 0.0, 1.0));
      add("Mercassium", 12.0, 200.0, 0.48);
      add("Vendarite", 12.0, 220.0, 0.42);
      add("Uridium", 12.0, 220.0, 0.40);
      add("Corundium", 12.0, 220.0, std::clamp(0.40 + 0.30 * ruins, 0.0, 1.0));
      add("Gallicite", 12.0, 260.0, std::clamp(0.50 + 0.25 * ruins, 0.0, 1.0));

      // Volatiles: more common in nebulae (and a bit more common in pirate space).
      const double vol_mult = reg ? reg->volatile_richness_mult : 1.0;
      const double sor_p = std::clamp(0.35 + 0.55 * neb + 0.10 * (vol_mult - 1.0), 0.0, 1.0);
      const double fuel_p = std::clamp(0.30 + 0.45 * neb + 0.12 * pirate, 0.0, 1.0);
      add("Sorium", 20.0, 500.0, sor_p);
      add("Fuel", 250.0, 4000.0, fuel_p);

      // Trim near-zero / invalid entries.
      for (auto it = m.begin(); it != m.end();) {
        if (!std::isfinite(it->second) || it->second <= 0.01) it = m.erase(it);
        else ++it;
      }
      return m;
    };

    auto ruins_package = [&](double scale, double nebula_density, const Region* reg) {
      auto m = salvage_package(scale * 1.35, nebula_density, reg);

      // Guarantee at least one exotic payload.
      static const std::array<const char*, 4> kExotics = {"Corundium", "Gallicite", "Vendarite", "Uridium"};
      const char* ex = kExotics[static_cast<std::size_t>(rand_int(wrng, 0, (int)kExotics.size() - 1))];

      const double ruins = reg ? clamp01(reg->ruins_density) : 0.0;
      m[ex] += rand_log_uniform(wrng, 80.0, 520.0) * scale * (1.0 + 0.75 * ruins);
      return m;
    };

    // Home system: a small 'tutorial' wreck to make salvage mechanics discoverable immediately.
    {
      const auto* hs = find_ptr(s.systems, home_system);
      const double neb = hs ? hs->nebula_density : 0.0;
      const Region* hreg = hs ? find_ptr(s.regions, hs->region_id) : nullptr;
      const Vec2 pos = random_offset(35.0, 85.0);
      add_wreck(home_system, "Derelict Survey Probe", pos, salvage_package(0.30, neb, hreg));
    }

    // Pirate system: stash/caches are more common in dusty regions.
    if (pirates != kInvalidId && pirate_system != kInvalidId) {
      const auto* psys = find_ptr(s.systems, pirate_system);
      const double neb = psys ? psys->nebula_density : 0.0;
      const Region* preg = psys ? find_ptr(s.regions, psys->region_id) : nullptr;
      Vec2 pos = random_offset(120.0, 220.0);

      if (psys && !psys->jump_points.empty()) {
        const Id jpid = psys->jump_points[static_cast<std::size_t>(rand_int(wrng, 0, (int)psys->jump_points.size() - 1))];
        if (const auto* jp = find_ptr(s.jump_points, jpid)) {
          pos = jp->position_mkm + random_offset(8.0, 28.0);
        }
      }

      add_wreck(pirate_system, "Pirate Cache", pos, salvage_package(0.55, std::max(0.35, neb), preg));
    }


    // Additional sites: a mix of near and far systems.
    int extra_sites = std::clamp(num_systems / 3, 2, 14);
    if (num_systems <= 2) extra_sites = 0;

    struct SysPick {
      Id id{kInvalidId};
      Id region_id{kInvalidId};
      double dist{0.0};
      double nebula{0.0};
      double extent{0.0};
      std::string name;
    };

    std::vector<SysPick> picks;
    picks.reserve(systems.size());
    for (const auto& si : systems) {
      SysPick p;
      p.id = si.id;
      p.region_id = si.region_id;
      p.dist = (si.galaxy_pos - systems.front().galaxy_pos).length();
      p.nebula = si.nebula_density;
      p.extent = si.max_orbit_extent_mkm;
      p.name = si.name;
      picks.push_back(std::move(p));
    }
    std::sort(picks.begin(), picks.end(), [](const SysPick& a, const SysPick& b) { return a.dist < b.dist; });

    const int pool = (int)picks.size();
    const int near_pool = std::min(pool, 8);
    const int far_pool = std::min(pool, 8);

    std::unordered_set<Id> used_sys;
    used_sys.insert(home_system);
    if (pirate_system != kInvalidId) used_sys.insert(pirate_system);
    for (const auto& es : empires) used_sys.insert(es.system_id);

    auto pick_from_range = [&](int lo, int hi) -> SysPick* {
      if (lo >= hi) return nullptr;

      double total = 0.0;
      for (int i = lo; i < hi; ++i) {
        if (used_sys.count(picks[static_cast<std::size_t>(i)].id)) continue;
        double w = 1.0 + 2.0 * clamp01(picks[static_cast<std::size_t>(i)].nebula);
        if (const auto* reg = find_ptr(s.regions, picks[static_cast<std::size_t>(i)].region_id)) {
          w *= 1.0 + 0.60 * clamp01(reg->ruins_density);
        }
        total += w;
      }
      if (total <= 0.0) return nullptr;

      double t = rand_real(wrng, 0.0, total);
      for (int i = lo; i < hi; ++i) {
        if (used_sys.count(picks[static_cast<std::size_t>(i)].id)) continue;
        double w = 1.0 + 2.0 * clamp01(picks[static_cast<std::size_t>(i)].nebula);
        if (const auto* reg = find_ptr(s.regions, picks[static_cast<std::size_t>(i)].region_id)) {
          w *= 1.0 + 0.60 * clamp01(reg->ruins_density);
        }
        t -= w;
        if (t <= 0.0) return &picks[static_cast<std::size_t>(i)];
      }
      return nullptr;
    };

    for (int k = 0; k < extra_sites; ++k) {
      const bool want_far = (k >= extra_sites / 2);
      SysPick* pick = nullptr;
      if (want_far) {
        pick = pick_from_range(std::max(0, pool - far_pool), pool);
      } else {
        pick = pick_from_range(1, near_pool); // skip index 0 (home)
      }
      if (!pick) break;

      used_sys.insert(pick->id);

      const int count = want_far ? rand_int(wrng, 1, 3) : rand_int(wrng, 1, 2);
      for (int wi = 0; wi < count; ++wi) {
        Vec2 pos = random_offset(60.0, std::max(140.0, pick->extent + 60.0));

        if (const auto* ss = find_ptr(s.systems, pick->id); ss && !ss->jump_points.empty() && rand_unit(wrng) < 0.50) {
          const Id jpid = ss->jump_points[static_cast<std::size_t>(rand_int(wrng, 0, (int)ss->jump_points.size() - 1))];
          if (const auto* jp = find_ptr(s.jump_points, jpid)) {
            pos = jp->position_mkm + random_offset(6.0, 28.0);
          }
        }

        const char* kind = "Derelict Freighter";
        if (rand_unit(wrng) < 0.22) kind = "Wrecked Escort";
        if (rand_unit(wrng) < 0.18) kind = "Abandoned Station";
        if (rand_unit(wrng) < 0.14) kind = "Lost Survey Ship";

        std::string nm = std::string(kind) + " (" + pick->name + ")";
        const double base_scale = want_far ? 1.0 : 0.55;
        const double neb_scale = 0.85 + 0.55 * clamp01(pick->nebula);
        const Region* rreg = find_ptr(s.regions, pick->region_id);
        add_wreck(pick->id, nm, pos, salvage_package(base_scale * neb_scale, pick->nebula, rreg));
      }
    }

    // Ancient ruins / precursor sites: rare high-value salvage that is biased by
    // region.ruins_density and distance from the player.
    if (!s.regions.empty() && num_systems > 3) {
      int ruins_sites = std::clamp(num_systems / 8, 1, 10);

      struct RuinsCand {
        SysPick* pick{nullptr};
        double w{0.0};
      };

      std::vector<RuinsCand> cands;
      cands.reserve(picks.size());

      const double max_dist = std::max(1e-6, picks.back().dist);

      for (auto& p : picks) {
        if (used_sys.count(p.id)) continue;

        const auto* reg = find_ptr(s.regions, p.region_id);
        const double ruins = reg ? clamp01(reg->ruins_density) : 0.0;
        if (ruins < 0.08) continue;

        const double dn = std::clamp(p.dist / max_dist, 0.0, 1.0);
        double w = (0.10 + 2.40 * ruins) * (0.55 + 0.75 * dn) * (0.85 + 0.35 * clamp01(p.nebula));
        if (w <= 0.0) continue;

        cands.push_back({&p, w});
      }

      if (!cands.empty()) {
        static const std::array<const char*, 6> kRuinsNames = {
            "Precursor Vault",
            "Ancient Ruins",
            "Derelict Megastructure",
            "Forgotten Listening Post",
            "Xeno Archive",
            "Sunken Temple",
        };

        for (int r = 0; r < ruins_sites && !cands.empty(); ++r) {
          double total = 0.0;
          for (const auto& c : cands) total += c.w;
          if (total <= 0.0) break;

          double t = rand_real(wrng, 0.0, total);
          std::size_t sel = 0;
          for (std::size_t i = 0; i < cands.size(); ++i) {
            t -= cands[i].w;
            if (t <= 0.0) {
              sel = i;
              break;
            }
          }

          SysPick* sp = cands[sel].pick;
          const Region* reg = sp ? find_ptr(s.regions, sp->region_id) : nullptr;

          // Prefer placing ruins near jump points in more connected systems.
          Vec2 pos = random_offset(80.0, std::max(180.0, sp ? (sp->extent + 90.0) : 180.0));
          if (sp) {
            if (const auto* ss = find_ptr(s.systems, sp->id); ss && !ss->jump_points.empty() && rand_unit(wrng) < 0.60) {
              const Id jpid = ss->jump_points[static_cast<std::size_t>(rand_int(wrng, 0, (int)ss->jump_points.size() - 1))];
              if (const auto* jp = find_ptr(s.jump_points, jpid)) {
                pos = jp->position_mkm + random_offset(10.0, 36.0);
              }
            }
          }

          const char* kind = kRuinsNames[static_cast<std::size_t>(rand_int(wrng, 0, (int)kRuinsNames.size() - 1))];
          std::string nm = std::string(kind) + " (" + (sp ? sp->name : std::string("Unknown")) + ")";

          if (sp) {
            add_wreck(sp->id, nm, pos, ruins_package(0.95, sp->nebula, reg));
            used_sys.insert(sp->id);
          }

          // Remove selected candidate (and any duplicates by id).
          if (sp) {
            const Id picked_id = sp->id;
            cands.erase(cands.begin() + static_cast<std::vector<RuinsCand>::difference_type>(sel));
            for (auto it = cands.begin(); it != cands.end();) {
              if (it->pick && it->pick->id == picked_id) it = cands.erase(it);
              else ++it;
            }
          } else {
            cands.erase(cands.begin() + static_cast<std::vector<RuinsCand>::difference_type>(sel));
          }
        }
      }
    }

  }

  // --- Colonies ---
  const Id home_colony = allocate_id(s);
  {
    Colony c;
    c.id = home_colony;
    c.name = "Homeworld";
    c.faction_id = terrans;
    c.body_id = homeworld_body;
    c.population_millions = 8500.0;
    c.minerals = {
        {"Duranium", 10000.0},
        {"Neutronium", 1500.0},
        {"Fuel", 30000.0},
    };
    c.installations = {
        {"automated_mine", 50},
        {"construction_factory", 5},
        {"fuel_refinery", 10},
        {"shipyard", 1},
        {"research_lab", 20},
        {"sensor_station", 1},
    };
    s.colonies[c.id] = c;
  }

  // --- Pirate base colony ---
  if (pirates != kInvalidId && pirate_idx >= 0 && pirate_system != kInvalidId) {
    const SysInfo& ps = systems[static_cast<std::size_t>(pirate_idx)];
    Id base_body = ps.preferred_colony_body;
    if (base_body == kInvalidId) base_body = ps.primary_star;

    // If pirates start in the home system, try to avoid putting them on the homeworld.
    if (pirate_system == home_system && base_body == homeworld_body) {
      for (Id pid : ps.planet_bodies) {
        if (pid != homeworld_body) {
          base_body = pid;
          break;
        }
      }
    }

    // Ensure the pirate base world is livable so the pirate AI can scale up over time.
    if (auto* bb = find_ptr(s.bodies, base_body)) {
      if (bb->surface_temp_k <= 0.0) bb->surface_temp_k = 288.0;
      if (bb->atmosphere_atm <= 0.0) bb->atmosphere_atm = 1.0;
      bb->terraforming_target_temp_k = bb->surface_temp_k;
      bb->terraforming_target_atm = bb->atmosphere_atm;
      bb->terraforming_complete = true;
    }

    Colony c;
    c.id = allocate_id(s);
    c.name = "Pirate Haven";
    c.faction_id = pirates;
    c.body_id = base_body;
    c.population_millions = 200.0;
    c.installations = {
        {"shipyard", 1},
        {"construction_factory", 1},
        {"fuel_refinery", 3},
        {"research_lab", 5},
        {"sensor_station", 1},
        {"automated_mine", 10},
    };
    c.minerals = {
        {"Duranium", 15000.0},
        {"Neutronium", 1500.0},
        {"Fuel", 20000.0},
    };
    s.colonies[c.id] = c;
  }


  // --- AI empire starting colonies ---
  for (const auto& es : empires) {
    Colony c;
    c.id = allocate_id(s);
    c.name = systems[static_cast<std::size_t>(es.system_idx)].name + " Prime";
    c.faction_id = es.faction_id;
    c.body_id = es.capital_body;
    c.population_millions = rand_real(rng, 2200.0, 5500.0);
    c.minerals = {
        {"Duranium", rand_real(rng, 6000.0, 14000.0)},
        {"Neutronium", rand_real(rng, 500.0, 1600.0)},
        {"Fuel", rand_real(rng, 12000.0, 26000.0)},
    };

    const int mines = rand_int(rng, 15, 35);
    const int labs = rand_int(rng, 6, 14);
    const int refineries = rand_int(rng, 4, 9);
    const int factories = rand_int(rng, 2, 5);

    c.installations = {
        {"automated_mine", mines},
        {"construction_factory", factories},
        {"fuel_refinery", refineries},
        {"shipyard", 1},
        {"research_lab", labs},
        {"sensor_station", 1},
    };
    s.colonies[c.id] = c;
  }

  auto add_ship = [&](Id faction_id, Id system_id, const Vec2& pos, const std::string& name,
                      const std::string& design_id) {
    const Id id = allocate_id(s);
    Ship ship;
    ship.id = id;
    ship.name = name;
    ship.faction_id = faction_id;
    ship.system_id = system_id;
    ship.design_id = design_id;
    ship.position_mkm = pos;
    s.ships[id] = ship;
    s.ship_orders[id] = ShipOrders{};
    s.systems[system_id].ships.push_back(id);
    return id;
  };

  // Approximate body positions at epoch (t=0) so ships spawn near the right place even for eccentric orbits.
  std::unordered_map<Id, Vec2> epoch_pos_cache;

  auto orbit_offset_epoch = [&](const Body& b) -> Vec2 {
    if (b.orbit_radius_mkm <= 1e-9) return {0.0, 0.0};
    const double a = std::max(0.0, b.orbit_radius_mkm);
    const double e = std::clamp(std::abs(b.orbit_eccentricity), 0.0, 0.999999);
    const double M = std::fmod(b.orbit_phase_radians, kTwoPi);

    // Solve Kepler's equation for eccentric anomaly E (t=0).
    double E = (e < 0.8) ? M : (kTwoPi * 0.5);
    for (int it = 0; it < 12; ++it) {
      const double sE = std::sin(E);
      const double cE = std::cos(E);
      const double f = (E - e * sE) - M;
      const double fp = 1.0 - e * cE;
      if (std::fabs(fp) < 1e-12) break;
      E -= f / fp;
      if (std::fabs(f) < 1e-10) break;
    }

    const double sE = std::sin(E);
    const double cE = std::cos(E);
    const double bsemi = a * std::sqrt(std::max(0.0, 1.0 - e * e));
    const double x = a * (cE - e);
    const double y = bsemi * sE;

    const double w = b.orbit_arg_periapsis_radians;
    const double cw = std::cos(w);
    const double sw = std::sin(w);
    return {x * cw - y * sw, x * sw + y * cw};
  };

  auto body_pos_epoch = [&](Id bid, auto&& self) -> Vec2 {
    if (auto it = epoch_pos_cache.find(bid); it != epoch_pos_cache.end()) return it->second;
    const auto* b = find_ptr(s.bodies, bid);
    if (!b) return {0.0, 0.0};

    Vec2 parent_pos{0.0, 0.0};
    if (b->parent_body_id != kInvalidId) {
      parent_pos = self(b->parent_body_id, self);
    }

    const Vec2 pos = parent_pos + orbit_offset_epoch(*b);
    epoch_pos_cache[bid] = pos;
    return pos;
  };

  Vec2 home_pos{0.0, 0.0};
  home_pos = body_pos_epoch(homeworld_body, body_pos_epoch);

  // --- Starting Terran fleet ---
  (void)add_ship(terrans, home_system, home_pos, "Freighter Alpha", "freighter_alpha");
  (void)add_ship(terrans, home_system, home_pos + Vec2{0.0, 0.8}, "Surveyor Beta", "surveyor_beta");
  (void)add_ship(terrans, home_system, home_pos + Vec2{0.0, -0.8}, "Escort Gamma", "escort_gamma");


  // --- Starting AI empire fleets ---
  // These give major AI empires the ability to explore/expand from day 1.
  if (!empires.empty()) {
    static const std::array<const char*, 10> greek = {
        "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X"};

    for (std::size_t ei = 0; ei < empires.size(); ++ei) {
      const auto& es = empires[ei];
      const std::string cap = systems[static_cast<std::size_t>(es.system_idx)].name;
      const Vec2 cap_pos = body_pos_epoch(es.capital_body, body_pos_epoch);
      const char* suf = greek[std::min<std::size_t>(ei, greek.size() - 1)];

      {
        const Id sid = add_ship(es.faction_id, es.system_id, cap_pos, "Freighter " + cap + " " + suf, "freighter_alpha");
        auto& sh = s.ships[sid];
        sh.auto_refuel = true;
        sh.auto_repair = true;
        sh.auto_freight = true;
      }

      {
        const Id sid = add_ship(es.faction_id, es.system_id, cap_pos + Vec2{0.0, 1.0}, "Surveyor " + cap + " " + suf,
                                "surveyor_beta");
        auto& sh = s.ships[sid];
        sh.auto_refuel = true;
        sh.auto_repair = true;
        sh.auto_explore = true;
      }

      {
        const Id sid = add_ship(es.faction_id, es.system_id, cap_pos + Vec2{0.0, -1.0}, "Escort " + cap + " " + suf,
                                "escort_gamma");
        auto& sh = s.ships[sid];
        sh.auto_refuel = true;
        sh.auto_repair = true;
      }

      // A single colony ship is enough for early expansion; the AI economy can build more later.
      {
        const Id sid = add_ship(es.faction_id, es.system_id, cap_pos + Vec2{1.2, 0.0}, "Colony Ship " + cap + " " + suf,
                                "colony_ship_mk1");
        auto& sh = s.ships[sid];
        sh.auto_refuel = true;
        sh.auto_repair = true;
        sh.auto_colonize = true;
      }
    }
  }

  // --- Pirate presence ---
  if (pirates != kInvalidId && pirate_system != kInvalidId) {
    const double strength = pirate_strength;
    const int base_count = (pirate_system != home_system) ? 2 : 1;
    int count = (int)std::llround((double)base_count * strength);
    count = std::clamp(count, 1, 10);

    if (pirate_system != home_system) {
      // Spawn near the pirate base body.
      Vec2 ppos{80.0, 0.5};
      const SysInfo& ps = systems[static_cast<std::size_t>(pirate_idx)];
      ppos = body_pos_epoch(ps.preferred_colony_body, body_pos_epoch);

      for (int i = 0; i < count; ++i) {
        const Vec2 off{0.7 * (double)(i % 3), -0.35 * (double)(i / 3)};
        const std::string nm = "Raider " + std::to_string(i + 1);
        const Id sid = add_ship(pirates, pirate_system, ppos + off, nm, "pirate_raider");
        auto& sh = s.ships[sid];
        sh.auto_refuel = true;
        sh.auto_repair = true;
      }
    } else {
      // If only one system exists, keep pirates offset from the home fleet.
      for (int i = 0; i < count; ++i) {
        const Vec2 off{12.0 + 1.2 * (double)(i % 4), 12.0 - 0.8 * (double)(i / 4)};
        const std::string nm = "Raider " + std::to_string(i + 1);
        const Id sid = add_ship(pirates, pirate_system, home_pos + off, nm, "pirate_raider");
        auto& sh = s.ships[sid];
        sh.auto_refuel = true;
        sh.auto_repair = true;
      }
    }
  }

  return s;
}

GameState make_random_scenario(std::uint32_t seed, int num_systems) {
  RandomScenarioConfig cfg;
  cfg.seed = seed;
  cfg.num_systems = num_systems;
  return make_random_scenario(cfg);
}

} // namespace nebula4x
