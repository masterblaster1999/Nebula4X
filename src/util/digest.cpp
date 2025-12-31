#include "nebula4x/util/digest.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nebula4x/core/entities.h"

namespace nebula4x {
namespace {

// FNV-1a 64-bit.
class Digest64 {
 public:
  Digest64() = default;

  void add_u8(std::uint8_t b) {
    h_ ^= static_cast<std::uint64_t>(b);
    h_ *= kPrime;
  }

  void add_u64(std::uint64_t v) {
    // Feed little-endian bytes to avoid host endianness differences.
    for (int i = 0; i < 8; ++i) {
      add_u8(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFu));
    }
  }

  void add_i64(std::int64_t v) { add_u64(static_cast<std::uint64_t>(v)); }

  void add_size(std::size_t n) { add_u64(static_cast<std::uint64_t>(n)); }

  void add_bool(bool b) { add_u8(static_cast<std::uint8_t>(b ? 1 : 0)); }

  template <typename E, typename = std::enable_if_t<std::is_enum_v<E>>>
  void add_enum(E e) {
    using U = std::underlying_type_t<E>;
    add_u64(static_cast<std::uint64_t>(static_cast<U>(e)));
  }

  void add_string(const std::string& s) {
    add_size(s.size());
    for (unsigned char c : s) add_u8(static_cast<std::uint8_t>(c));
  }

  void add_double(double v) {
    std::uint64_t u = 0;
    static_assert(sizeof(u) == sizeof(v));
    std::memcpy(&u, &v, sizeof(u));

    // Normalize -0.0 to +0.0.
    if ((u << 1) == 0) u = 0;

    // Canonicalize NaNs (if any) so different payloads don't change the digest.
    const std::uint64_t exp = u & 0x7ff0000000000000ULL;
    const std::uint64_t mant = u & 0x000fffffffffffffULL;
    if (exp == 0x7ff0000000000000ULL && mant != 0) {
      u = 0x7ff8000000000000ULL;
    }

    add_u64(u);
  }

  std::uint64_t value() const { return h_; }

 private:
  static constexpr std::uint64_t kOffset = 1469598103934665603ull;
  static constexpr std::uint64_t kPrime = 1099511628211ull;

  std::uint64_t h_{kOffset};
};

template <typename Map>
std::vector<typename Map::key_type> sorted_keys(const Map& m) {
  std::vector<typename Map::key_type> keys;
  keys.reserve(m.size());
  for (const auto& [k, _] : m) keys.push_back(k);
  std::sort(keys.begin(), keys.end());
  return keys;
}

template <typename T>
std::vector<T> sorted_unique_copy(std::vector<T> v) {
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
  return v;
}

static void hash_vec2(Digest64& d, const Vec2& v) {
  d.add_double(v.x);
  d.add_double(v.y);
}

static void hash_order(Digest64& d, const Order& ord) {
  std::visit(
      [&](const auto& o) {
        using T = std::decay_t<decltype(o)>;
        if constexpr (std::is_same_v<T, MoveToPoint>) {
          d.add_u64(1);
          hash_vec2(d, o.target_mkm);
        } else if constexpr (std::is_same_v<T, MoveToBody>) {
          d.add_u64(2);
          d.add_u64(o.body_id);
        } else if constexpr (std::is_same_v<T, ColonizeBody>) {
          d.add_u64(3);
          d.add_u64(o.body_id);
          d.add_string(o.colony_name);
        } else if constexpr (std::is_same_v<T, OrbitBody>) {
          d.add_u64(4);
          d.add_u64(o.body_id);
          d.add_i64(o.duration_days);
        } else if constexpr (std::is_same_v<T, TravelViaJump>) {
          d.add_u64(5);
          d.add_u64(o.jump_point_id);
        } else if constexpr (std::is_same_v<T, AttackShip>) {
          d.add_u64(6);
          d.add_u64(o.target_ship_id);
          d.add_bool(o.has_last_known);
          hash_vec2(d, o.last_known_position_mkm);
        } else if constexpr (std::is_same_v<T, WaitDays>) {
          d.add_u64(7);
          d.add_i64(o.days_remaining);
        } else if constexpr (std::is_same_v<T, LoadMineral>) {
          d.add_u64(8);
          d.add_u64(o.colony_id);
          d.add_string(o.mineral);
          d.add_double(o.tons);
        } else if constexpr (std::is_same_v<T, UnloadMineral>) {
          d.add_u64(9);
          d.add_u64(o.colony_id);
          d.add_string(o.mineral);
          d.add_double(o.tons);
        } else if constexpr (std::is_same_v<T, LoadTroops>) {
          d.add_u64(12);
          d.add_u64(o.colony_id);
          d.add_double(o.strength);
        } else if constexpr (std::is_same_v<T, UnloadTroops>) {
          d.add_u64(13);
          d.add_u64(o.colony_id);
          d.add_double(o.strength);
        } else if constexpr (std::is_same_v<T, InvadeColony>) {
          d.add_u64(14);
          d.add_u64(o.colony_id);
        } else if constexpr (std::is_same_v<T, TransferCargoToShip>) {
          d.add_u64(10);
          d.add_u64(o.target_ship_id);
          d.add_string(o.mineral);
          d.add_double(o.tons);
        } else if constexpr (std::is_same_v<T, ScrapShip>) {
          d.add_u64(11);
          d.add_u64(o.colony_id);
        } else {
          // If we add new order types, force a compile-time error until hashing is updated.
          static_assert(!std::is_same_v<T, T>, "Unhandled Order variant in hash_order()");
        }
      },
      ord);
}

static void hash_string_double_map(Digest64& d, const std::unordered_map<std::string, double>& m) {
  d.add_size(m.size());
  auto keys = sorted_keys(m);
  for (const auto& k : keys) {
    d.add_string(k);
    auto it = m.find(k);
    d.add_double(it == m.end() ? 0.0 : it->second);
  }
}

static void hash_string_int_map(Digest64& d, const std::unordered_map<std::string, int>& m) {
  d.add_size(m.size());
  auto keys = sorted_keys(m);
  for (const auto& k : keys) {
    d.add_string(k);
    auto it = m.find(k);
    d.add_i64(it == m.end() ? 0 : it->second);
  }
}

static void hash_relations_map(Digest64& d, const std::unordered_map<Id, DiplomacyStatus>& m) {
  d.add_size(m.size());
  auto keys = sorted_keys(m);
  for (Id k : keys) {
    d.add_u64(k);
    auto it = m.find(k);
    d.add_enum(it == m.end() ? DiplomacyStatus::Hostile : it->second);
  }
}

static void hash_ship_design(Digest64& d, const ShipDesign& sd) {
  d.add_string(sd.id);
  d.add_string(sd.name);
  d.add_enum(sd.role);

  // Components behave like a multiset (order doesn't matter, duplicates do).
  auto comps = sd.components;
  std::sort(comps.begin(), comps.end());
  d.add_size(comps.size());
  for (const auto& c : comps) d.add_string(c);

  // Derived stats.
  d.add_double(sd.mass_tons);
  d.add_double(sd.speed_km_s);
  d.add_double(sd.fuel_capacity_tons);
  d.add_double(sd.fuel_use_per_mkm);
  d.add_double(sd.cargo_tons);
  d.add_double(sd.sensor_range_mkm);
  d.add_double(sd.colony_capacity_millions);
  d.add_double(sd.troop_capacity);
  d.add_double(sd.power_generation);
  d.add_double(sd.power_use_total);
  d.add_double(sd.power_use_engines);
  d.add_double(sd.power_use_sensors);
  d.add_double(sd.power_use_weapons);
  d.add_double(sd.power_use_shields);
  d.add_double(sd.max_hp);
  d.add_double(sd.max_shields);
  d.add_double(sd.shield_regen_per_day);
  d.add_double(sd.weapon_damage);
  d.add_double(sd.weapon_range_mkm);
}

static void hash_component_def(Digest64& d, const ComponentDef& c) {
  d.add_string(c.id);
  d.add_string(c.name);
  d.add_enum(c.type);
  d.add_double(c.mass_tons);
  d.add_double(c.speed_km_s);
  d.add_double(c.fuel_use_per_mkm);
  d.add_double(c.fuel_capacity_tons);
  d.add_double(c.cargo_tons);
  d.add_double(c.sensor_range_mkm);
  d.add_double(c.colony_capacity_millions);
  d.add_double(c.troop_capacity);
  d.add_double(c.power_output);
  d.add_double(c.power_use);
  d.add_double(c.weapon_damage);
  d.add_double(c.weapon_range_mkm);
  d.add_double(c.hp_bonus);
  d.add_double(c.shield_hp);
  d.add_double(c.shield_regen_per_day);
}

static void hash_installation_def(Digest64& d, const InstallationDef& def) {
  d.add_string(def.id);
  d.add_string(def.name);
  d.add_bool(def.mining);
  hash_string_double_map(d, def.produces_per_day);
  hash_string_double_map(d, def.consumes_per_day);
  d.add_double(def.construction_points_per_day);
  d.add_double(def.construction_cost);
  hash_string_double_map(d, def.build_costs);
  d.add_double(def.build_rate_tons_per_day);
  hash_string_double_map(d, def.build_costs_per_ton);
  d.add_double(def.sensor_range_mkm);
  d.add_double(def.research_points_per_day);
  d.add_double(def.terraforming_points_per_day);
  d.add_double(def.troop_training_points_per_day);
  d.add_double(def.fortification_points);
}

static void hash_tech_def(Digest64& d, const TechDef& t) {
  d.add_string(t.id);
  d.add_string(t.name);
  d.add_double(t.cost);

  // Prereqs behave like a set.
  auto prereqs = t.prereqs;
  prereqs = sorted_unique_copy(std::move(prereqs));
  d.add_size(prereqs.size());
  for (const auto& p : prereqs) d.add_string(p);

  // Effects are order-insensitive in the content schema.
  auto eff = t.effects;
  std::sort(eff.begin(), eff.end(), [](const TechEffect& a, const TechEffect& b) {
    if (a.type != b.type) return a.type < b.type;
    if (a.value != b.value) return a.value < b.value;
    return a.amount < b.amount;
  });
  d.add_size(eff.size());
  for (const auto& e : eff) {
    d.add_string(e.type);
    d.add_string(e.value);
    d.add_double(e.amount);
  }
}

static void hash_content_db(Digest64& d, const ContentDB& c) {
  d.add_string("ContentDigestV1");

  d.add_size(c.components.size());
  for (const auto& key : sorted_keys(c.components)) {
    d.add_string(key);
    hash_component_def(d, c.components.at(key));
  }

  d.add_size(c.designs.size());
  for (const auto& key : sorted_keys(c.designs)) {
    d.add_string(key);
    hash_ship_design(d, c.designs.at(key));
  }

  d.add_size(c.installations.size());
  for (const auto& key : sorted_keys(c.installations)) {
    d.add_string(key);
    hash_installation_def(d, c.installations.at(key));
  }

  d.add_size(c.techs.size());
  for (const auto& key : sorted_keys(c.techs)) {
    d.add_string(key);
    hash_tech_def(d, c.techs.at(key));
  }
}

static void hash_game_state(Digest64& d, const GameState& s, const DigestOptions& opt) {
  d.add_string("GameStateDigestV1");

  d.add_i64(s.save_version);
  d.add_i64(s.date.days_since_epoch());
  d.add_u64(s.next_id);
  d.add_u64(s.next_event_seq);

  if (opt.include_ui_state) d.add_u64(s.selected_system);

  // Systems
  d.add_size(s.systems.size());
  for (Id sid : sorted_keys(s.systems)) {
    const auto& sys = s.systems.at(sid);
    d.add_u64(sid);
    d.add_string(sys.name);
    hash_vec2(d, sys.galaxy_pos);

    auto bodies = sys.bodies;
    bodies = sorted_unique_copy(std::move(bodies));
    d.add_size(bodies.size());
    for (Id bid : bodies) d.add_u64(bid);

    auto ships = sys.ships;
    ships = sorted_unique_copy(std::move(ships));
    d.add_size(ships.size());
    for (Id shid : ships) d.add_u64(shid);

    auto jumps = sys.jump_points;
    jumps = sorted_unique_copy(std::move(jumps));
    d.add_size(jumps.size());
    for (Id jid : jumps) d.add_u64(jid);
  }

  // Bodies
  d.add_size(s.bodies.size());
  for (Id bid : sorted_keys(s.bodies)) {
    const auto& b = s.bodies.at(bid);
    d.add_u64(bid);
    d.add_string(b.name);
    d.add_enum(b.type);
    d.add_u64(b.system_id);
    d.add_u64(b.parent_body_id);
    d.add_double(b.orbit_radius_mkm);
    d.add_double(b.orbit_period_days);
    d.add_double(b.orbit_phase_radians);
    d.add_double(b.orbit_eccentricity);
    d.add_double(b.orbit_arg_periapsis_radians);
    d.add_double(b.mass_solar);
    d.add_double(b.luminosity_solar);
    d.add_double(b.mass_earths);
    d.add_double(b.radius_km);
    d.add_double(b.surface_temp_k);
    d.add_double(b.atmosphere_atm);
    d.add_double(b.terraforming_target_temp_k);
    d.add_double(b.terraforming_target_atm);
    d.add_bool(b.terraforming_complete);
    hash_vec2(d, b.position_mkm);
    hash_string_double_map(d, b.mineral_deposits);
  }

  // Jump points
  d.add_size(s.jump_points.size());
  for (Id jid : sorted_keys(s.jump_points)) {
    const auto& jp = s.jump_points.at(jid);
    d.add_u64(jid);
    d.add_string(jp.name);
    d.add_u64(jp.system_id);
    hash_vec2(d, jp.position_mkm);
    d.add_u64(jp.linked_jump_id);
  }

  // Ships
  d.add_size(s.ships.size());
  for (Id shid : sorted_keys(s.ships)) {
    const auto& sh = s.ships.at(shid);
    d.add_u64(shid);
    d.add_string(sh.name);
    d.add_u64(sh.faction_id);
    d.add_u64(sh.system_id);
    hash_vec2(d, sh.position_mkm);
    d.add_string(sh.design_id);
    d.add_double(sh.speed_km_s);
    hash_string_double_map(d, sh.cargo);
    d.add_bool(sh.auto_explore);
    d.add_bool(sh.auto_freight);
    d.add_double(sh.hp);
    d.add_double(sh.fuel_tons);
    d.add_double(sh.shields);
    d.add_double(sh.troops);
  }

  // Colonies
  d.add_size(s.colonies.size());
  for (Id cid : sorted_keys(s.colonies)) {
    const auto& c = s.colonies.at(cid);
    d.add_u64(cid);
    d.add_string(c.name);
    d.add_u64(c.faction_id);
    d.add_u64(c.body_id);
    d.add_double(c.population_millions);
    hash_string_double_map(d, c.minerals);
    hash_string_double_map(d, c.mineral_reserves);
    hash_string_int_map(d, c.installations);
    d.add_double(c.ground_forces);
    d.add_double(c.troop_training_queue);

    d.add_size(c.shipyard_queue.size());
    for (const auto& bo : c.shipyard_queue) {
      d.add_string(bo.design_id);
      d.add_double(bo.tons_remaining);
      d.add_u64(bo.refit_ship_id);
    }

    d.add_size(c.construction_queue.size());
    for (const auto& io : c.construction_queue) {
      d.add_string(io.installation_id);
      d.add_i64(io.quantity_remaining);
      d.add_bool(io.minerals_paid);
      d.add_double(io.cp_remaining);
    }
  }

  // Factions
  d.add_size(s.factions.size());
  for (Id fid : sorted_keys(s.factions)) {
    const auto& f = s.factions.at(fid);
    d.add_u64(fid);
    d.add_string(f.name);
    d.add_enum(f.control);
    hash_relations_map(d, f.relations);
    d.add_double(f.research_points);
    d.add_string(f.active_research_id);
    d.add_double(f.active_research_progress);

    // Research queue is order-sensitive.
    d.add_size(f.research_queue.size());
    for (const auto& tid : f.research_queue) d.add_string(tid);

    // Set-like lists.
    auto known = sorted_unique_copy(f.known_techs);
    d.add_size(known.size());
    for (const auto& tid : known) d.add_string(tid);

    auto uc = sorted_unique_copy(f.unlocked_components);
    d.add_size(uc.size());
    for (const auto& cid : uc) d.add_string(cid);

    auto ui = sorted_unique_copy(f.unlocked_installations);
    d.add_size(ui.size());
    for (const auto& iid : ui) d.add_string(iid);

    auto disc = sorted_unique_copy(f.discovered_systems);
    d.add_size(disc.size());
    for (Id sid : disc) d.add_u64(sid);

    d.add_size(f.ship_contacts.size());
    for (Id sid : sorted_keys(f.ship_contacts)) {
      const auto& c = f.ship_contacts.at(sid);
      d.add_u64(sid);
      d.add_u64(c.ship_id);
      d.add_u64(c.system_id);
      d.add_i64(c.last_seen_day);
      hash_vec2(d, c.last_seen_position_mkm);
      d.add_string(c.last_seen_name);
      d.add_string(c.last_seen_design_id);
      d.add_u64(c.last_seen_faction_id);
    }
  }

  // Fleets
  d.add_size(s.fleets.size());
  for (Id flid : sorted_keys(s.fleets)) {
    const auto& fl = s.fleets.at(flid);
    d.add_u64(flid);
    d.add_string(fl.name);
    d.add_u64(fl.faction_id);
    d.add_u64(fl.leader_ship_id);
    auto ship_ids = sorted_unique_copy(fl.ship_ids);
    d.add_size(ship_ids.size());
    for (Id sid : ship_ids) d.add_u64(sid);
    d.add_enum(fl.formation);
    d.add_double(fl.formation_spacing_mkm);
  }

  // Custom designs
  d.add_size(s.custom_designs.size());
  for (const auto& key : sorted_keys(s.custom_designs)) {
    d.add_string(key);
    hash_ship_design(d, s.custom_designs.at(key));
  }

  // Order templates
  d.add_size(s.order_templates.size());
  for (const auto& key : sorted_keys(s.order_templates)) {
    d.add_string(key);
    const auto& orders = s.order_templates.at(key);
    d.add_size(orders.size());
    for (const auto& o : orders) hash_order(d, o);
  }

  // Ship orders
  d.add_size(s.ship_orders.size());
  for (Id sid : sorted_keys(s.ship_orders)) {
    const auto& so = s.ship_orders.at(sid);
    d.add_u64(sid);
    d.add_size(so.queue.size());
    for (const auto& o : so.queue) hash_order(d, o);
    d.add_bool(so.repeat);
    d.add_i64(static_cast<std::int64_t>(so.repeat_count_remaining));
    d.add_size(so.repeat_template.size());
    for (const auto& o : so.repeat_template) hash_order(d, o);
  }

  // Ground battles
  d.add_size(s.ground_battles.size());
  for (Id cid : sorted_keys(s.ground_battles)) {
    const auto& b = s.ground_battles.at(cid);
    d.add_u64(cid);
    d.add_u64(b.colony_id);
    d.add_u64(b.system_id);
    d.add_u64(b.attacker_faction_id);
    d.add_u64(b.defender_faction_id);
    d.add_double(b.attacker_strength);
    d.add_double(b.defender_strength);
    d.add_i64(b.days_fought);
  }

  // Persistent events
  if (opt.include_events) {
    d.add_size(s.events.size());
    std::vector<const SimEvent*> ev;
    ev.reserve(s.events.size());
    for (const auto& e : s.events) ev.push_back(&e);
    std::sort(ev.begin(), ev.end(), [](const SimEvent* a, const SimEvent* b) {
      return a->seq < b->seq;
    });
    for (const auto* e : ev) {
      d.add_u64(e->seq);
      d.add_i64(e->day);
      d.add_enum(e->level);
      d.add_enum(e->category);
      d.add_u64(e->faction_id);
      d.add_u64(e->faction_id2);
      d.add_u64(e->system_id);
      d.add_u64(e->ship_id);
      d.add_u64(e->colony_id);
      d.add_string(e->message);
    }
  }
}

} // namespace

std::uint64_t digest_game_state64(const GameState& state, const DigestOptions& opt) {
  Digest64 d;
  hash_game_state(d, state, opt);
  return d.value();
}

std::uint64_t digest_content_db64(const ContentDB& content) {
  Digest64 d;
  hash_content_db(d, content);
  return d.value();
}

std::string digest64_to_hex(std::uint64_t v) {
  std::ostringstream out;
  out << std::hex;
  out.width(16);
  out.fill('0');
  out << v;
  return out.str();
}

} // namespace nebula4x
