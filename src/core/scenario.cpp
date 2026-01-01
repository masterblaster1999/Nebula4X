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
  auto seed_deposits = [&](Id body_id, double duranium_tons, double neutronium_tons) {
    auto& b = s.bodies.at(body_id);
    b.mineral_deposits["Duranium"] = std::max(0.0, duranium_tons);
    b.mineral_deposits["Neutronium"] = std::max(0.0, neutronium_tons);
  };

  // Give home bodies deep deposits so early growth isn't immediately blocked.
  seed_deposits(earth, 2.0e6, 2.0e5);
  seed_deposits(mars, 4.0e5, 4.0e4);
  // Minor bodies have smaller, but sometimes useful deposits.
  for (Id aid : sol_belt) {
    seed_deposits(aid, 7.5e4, 6.0e3);
  }
  seed_deposits(encke, 2.5e4, 1.5e3);
  seed_deposits(centauri_prime, 1.2e6, 1.2e5);
  seed_deposits(barnard_b, 3.0e5, 3.0e4);

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

GameState make_random_scenario(std::uint32_t seed, int num_systems) {
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

  const Id pirates = allocate_id(s);
  {
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

  auto seed_deposits = [&](Id body_id, double duranium_tons, double neutronium_tons) {
    auto& b = s.bodies.at(body_id);
    b.mineral_deposits["Duranium"] = std::max(0.0, duranium_tons);
    b.mineral_deposits["Neutronium"] = std::max(0.0, neutronium_tons);
  };

  auto seed_basic_deposits = [&](Id body_id, BodyType type, double orbit_au, double metallicity, double richness) {
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

    seed_deposits(body_id, dur, neu);
  };

  struct SysInfo {
    Id id{kInvalidId};
    std::string name;
    Vec2 galaxy_pos{0.0, 0.0};

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
  // Spiral-ish disc with a mild core concentration and minimum separation to avoid overlaps.
  const double galaxy_radius = std::max(3.0, std::sqrt(static_cast<double>(num_systems)) * 3.6);
  const int spiral_arms = 4;
  const double arm_tightness = 1.25;
  const double arm_spread = 0.28;
  const double core_fraction = 0.18;
  const double core_radius = galaxy_radius * 0.25;
  const double min_sep = std::max(0.55, galaxy_radius / (std::sqrt(static_cast<double>(num_systems)) * 2.25));

  std::vector<Vec2> placed;
  placed.reserve(static_cast<std::size_t>(num_systems));

  std::unordered_set<std::string> used_names;
  used_names.reserve(static_cast<std::size_t>(num_systems) * 2);

  const auto sample_system_pos = [&](int i) -> Vec2 {
    if (i == 0) return {0.0, 0.0};

    for (int attempt = 0; attempt < 600; ++attempt) {
      Vec2 p{0.0, 0.0};

      if (rand_unit(rng) < core_fraction) {
        // Dense core.
        const double r = core_radius * std::sqrt(rand_unit(rng));
        const double a = rand_real(rng, 0.0, kTwoPi);
        p = {r * std::cos(a), r * std::sin(a)};
      } else {
        // Spiral arms: choose a radius, then "pull" angle toward an arm with some noise.
        const double r = galaxy_radius * std::sqrt(rand_unit(rng));
        const int arm = rand_int(rng, 0, spiral_arms - 1);
        const double arm_base = static_cast<double>(arm) * (kTwoPi / static_cast<double>(spiral_arms));
        const double theta = arm_base + arm_tightness * std::log(1.0 + r) + rand_normal(rng, 0.0, arm_spread);

        // Perpendicular jitter grows slightly with radius.
        const double j = (0.35 + 0.65 * (r / galaxy_radius));
        const double rr = std::max(0.0, r + rand_normal(rng, 0.0, arm_spread * 0.55 * j));
        const double tt = theta + rand_normal(rng, 0.0, arm_spread * 0.35 * j);
        p = {rr * std::cos(tt), rr * std::sin(tt)};
      }

      bool ok = true;
      for (const Vec2& q : placed) {
        if ((p - q).length() < min_sep) {
          ok = false;
          break;
        }
      }
      if (ok) return p;
    }

    // Fallback: random disc.
    const double r = galaxy_radius * std::sqrt(rand_unit(rng));
    const double a = rand_real(rng, 0.0, kTwoPi);
    return {r * std::cos(a), r * std::sin(a)};
  };

  // --- Systems + bodies ---
  for (int i = 0; i < num_systems; ++i) {
    const Id sys_id = allocate_id(s);

    StarSystem sys;
    sys.id = sys_id;
    sys.name = generate_system_name(rng, used_names);
    sys.galaxy_pos = sample_system_pos(i);
    s.systems[sys_id] = sys;
    placed.push_back(sys.galaxy_pos);

    SysInfo info;
    info.id = sys_id;
    info.name = sys.name;
    info.galaxy_pos = sys.galaxy_pos;

    const StarParams sp = sample_star(rng);
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
          seed_basic_deposits(aid, BodyType::Asteroid, rr / kAu_mkm, sp.metallicity, 1.0);

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
      seed_basic_deposits(pid, type, orbit_au, sp.metallicity, richness);

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
            seed_basic_deposits(tid, BodyType::Asteroid, rr / kAu_mkm, sp.metallicity, 0.8);

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
        seed_basic_deposits(mid, BodyType::Moon, orbit_au, sp.metallicity, 0.9);

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
          seed_basic_deposits(cid, BodyType::Asteroid, orbit_au, sp.metallicity, 0.65);

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
        seed_basic_deposits(cid, BodyType::Comet, a_mkm / kAu_mkm, sp.metallicity, 0.55);

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

  // Pick a pirate system farthest from the home system (if possible).
  int pirate_idx = 0;
  if (num_systems > 1) {
    double best = -1.0;
    for (int i = 1; i < num_systems; ++i) {
      const double d = (systems[static_cast<std::size_t>(i)].galaxy_pos - systems.front().galaxy_pos).length();
      if (d > best) {
        best = d;
        pirate_idx = i;
      }
    }
  }
  const Id pirate_system = systems[static_cast<std::size_t>(pirate_idx)].id;

  s.selected_system = home_system;

  // Seed initial discovery: each faction knows its start system.
  s.factions[terrans].discovered_systems = {home_system};
  s.factions[pirates].discovered_systems = {pirate_system};

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

  // --- Jump points ---
  // Build a connected graph using a minimum spanning tree over galaxy distances, then add extra "local" edges.
  std::unordered_set<std::uint64_t> edges;
  edges.reserve(static_cast<std::size_t>(num_systems) * 3);

  const auto system_dist = [&](int ai, int bi) {
    const Vec2 d = systems[static_cast<std::size_t>(ai)].galaxy_pos - systems[static_cast<std::size_t>(bi)].galaxy_pos;
    return d.length();
  };

  auto add_jump_link = [&](int ai, int bi) {
    if (ai == bi) return;
    const std::uint64_t k = edge_key(ai, bi);
    if (edges.find(k) != edges.end()) return;
    edges.insert(k);

    const auto& a = systems[static_cast<std::size_t>(ai)];
    const auto& b = systems[static_cast<std::size_t>(bi)];

    const Id jp_a_id = allocate_id(s);
    const Id jp_b_id = allocate_id(s);

    auto make_pos = [&](const SysInfo& from) {
      const double base = std::max(120.0, from.max_orbit_extent_mkm + 40.0);
      const double r = base + rand_real(rng, 10.0, 90.0);
      const double ang = rand_real(rng, 0.0, kTwoPi);
      return Vec2{r * std::cos(ang), r * std::sin(ang)};
    };

    {
      JumpPoint jp;
      jp.id = jp_a_id;
      jp.name = "JP to " + b.name;
      jp.system_id = a.id;
      jp.position_mkm = make_pos(a);
      jp.linked_jump_id = jp_b_id;
      s.jump_points[jp.id] = jp;
      s.systems[a.id].jump_points.push_back(jp.id);
    }

    {
      JumpPoint jp;
      jp.id = jp_b_id;
      jp.name = "JP to " + a.name;
      jp.system_id = b.id;
      jp.position_mkm = make_pos(b);
      jp.linked_jump_id = jp_a_id;
      s.jump_points[jp.id] = jp;
      s.systems[b.id].jump_points.push_back(jp.id);
    }
  };

  // Minimum spanning tree (Prim).
  if (num_systems > 1) {
    std::vector<double> best(static_cast<std::size_t>(num_systems), std::numeric_limits<double>::infinity());
    std::vector<int> parent(static_cast<std::size_t>(num_systems), -1);
    std::vector<bool> used(static_cast<std::size_t>(num_systems), false);

    best[0] = 0.0;

    for (int it = 0; it < num_systems; ++it) {
      int u = -1;
      double u_best = std::numeric_limits<double>::infinity();
      for (int i = 0; i < num_systems; ++i) {
        if (!used[static_cast<std::size_t>(i)] && best[static_cast<std::size_t>(i)] < u_best) {
          u_best = best[static_cast<std::size_t>(i)];
          u = i;
        }
      }
      if (u == -1) break;

      used[static_cast<std::size_t>(u)] = true;
      if (parent[static_cast<std::size_t>(u)] != -1) {
        add_jump_link(u, parent[static_cast<std::size_t>(u)]);
      }

      for (int v = 0; v < num_systems; ++v) {
        if (used[static_cast<std::size_t>(v)] || v == u) continue;
        const double d = system_dist(u, v);
        if (d < best[static_cast<std::size_t>(v)]) {
          best[static_cast<std::size_t>(v)] = d;
          parent[static_cast<std::size_t>(v)] = u;
        }
      }
    }
  }

  // Extra edges: prefer shorter links, but allow occasional longer ones.
  if (num_systems > 2) {
    struct Pair {
      double d{0.0};
      int a{0};
      int b{0};
    };
    std::vector<Pair> pairs;
    pairs.reserve(static_cast<std::size_t>(num_systems) * static_cast<std::size_t>(num_systems - 1) / 2);

    for (int i = 0; i < num_systems; ++i) {
      for (int j = i + 1; j < num_systems; ++j) {
        pairs.push_back({system_dist(i, j), i, j});
      }
    }
    std::sort(pairs.begin(), pairs.end(), [](const Pair& lhs, const Pair& rhs) { return lhs.d < rhs.d; });

    const int extra_target = std::max(1, num_systems / 2);
    int added = 0;
    const double link_scale = galaxy_radius * 0.55;

    for (const Pair& p : pairs) {
      if (added >= extra_target) break;

      // Probability decays with distance.
      const double prob = 0.40 * std::exp(-p.d / std::max(0.1, link_scale));
      if (rand_unit(rng) < prob) {
        const std::size_t before = edges.size();
        add_jump_link(p.a, p.b);
        if (edges.size() != before) ++added;
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
  {
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

  // --- Pirate presence ---
  if (pirate_system != home_system) {
    // Spawn near the pirate base body.
    Vec2 ppos{80.0, 0.5};
    const SysInfo& ps = systems[static_cast<std::size_t>(pirate_idx)];
    ppos = body_pos_epoch(ps.preferred_colony_body, body_pos_epoch);
    (void)add_ship(pirates, pirate_system, ppos, "Raider I", "pirate_raider");
    (void)add_ship(pirates, pirate_system, ppos + Vec2{0.7, -0.3}, "Raider II", "pirate_raider");
  } else {
    // If only one system exists, keep pirates offset from the home fleet.
    (void)add_ship(pirates, pirate_system, home_pos + Vec2{12.0, 12.0}, "Raider I", "pirate_raider");
  }

  return s;
}

} // namespace nebula4x
