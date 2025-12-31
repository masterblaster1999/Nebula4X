#include "nebula4x/core/simulation.h"

#include "nebula4x/core/fleet_formation.h"

#include "simulation_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "nebula4x/util/log.h"

namespace nebula4x {
namespace {
using sim_internal::mkm_per_day_from_speed;
using sim_internal::push_unique;
using sim_internal::sorted_keys;
using sim_internal::compute_power_allocation;
} // namespace

void Simulation::tick_ships() {
  auto cargo_used_tons = [](const Ship& s) {
    double used = 0.0;
    for (const auto& [_, tons] : s.cargo) used += std::max(0.0, tons);
    return used;
  };

  const double arrive_eps = std::max(0.0, cfg_.arrival_epsilon_mkm);
  const double dock_range = std::max(arrive_eps, cfg_.docking_range_mkm);

  const auto ship_ids = sorted_keys(state_.ships);

  // --- Fleet cohesion prepass ---
  //
  // Fleets are intentionally lightweight in the data model, so we do a small
  // amount of per-tick work here to make fleet-issued orders behave more like
  // a coordinated group.
  //
  // 1) Speed matching: ships in the same fleet executing the same current
  //    movement order will match the slowest ship.
  // 2) Coordinated jump transits: ships in the same fleet attempting to transit
  //    the same jump point in the same system will wait until all have arrived.
  // 3) Formations: fleets may optionally offset per-ship targets for some
  //    cohorts so that ships travel/attack in a loose formation instead of
  //    piling onto the exact same coordinates.

  std::unordered_map<Id, Id> ship_to_fleet;
  ship_to_fleet.reserve(state_.ships.size() * 2);

  if (!state_.fleets.empty()) {
    const auto fleet_ids = sorted_keys(state_.fleets);
    for (Id fid : fleet_ids) {
      const auto* fl = find_ptr(state_.fleets, fid);
      if (!fl) continue;
      for (Id sid : fl->ship_ids) {
        if (sid == kInvalidId) continue;
        ship_to_fleet[sid] = fid;
      }
    }
  }

  enum class CohortKind : std::uint8_t {
    MovePoint,
    MoveBody,
    OrbitBody,
    Jump,
    Attack,
    Load,
    Unload,
    Transfer,
    Scrap,
  };

  struct CohortKey {
    Id fleet_id{kInvalidId};
    Id system_id{kInvalidId};
    CohortKind kind{CohortKind::MovePoint};
    Id target_id{kInvalidId};
    std::uint64_t x_bits{0};
    std::uint64_t y_bits{0};

    bool operator==(const CohortKey& o) const {
      return fleet_id == o.fleet_id && system_id == o.system_id && kind == o.kind && target_id == o.target_id &&
             x_bits == o.x_bits && y_bits == o.y_bits;
    }
  };

  struct CohortKeyHash {
    size_t operator()(const CohortKey& k) const {
      // FNV-1a style mixing.
      std::uint64_t h = 1469598103934665603ull;
      auto mix = [&](std::uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;
      };
      mix(k.fleet_id);
      mix(k.system_id);
      mix(static_cast<std::uint64_t>(k.kind));
      mix(k.target_id);
      mix(k.x_bits);
      mix(k.y_bits);
      return static_cast<size_t>(h);
    }
  };

  auto double_bits = [](double v) -> std::uint64_t {
    std::uint64_t out = 0;
    std::memcpy(&out, &v, sizeof(out));
    return out;
  };

  auto make_cohort_key = [&](Id fleet_id, Id system_id, const Order& ord) -> std::optional<CohortKey> {
    if (fleet_id == kInvalidId) return std::nullopt;

    CohortKey k;
    k.fleet_id = fleet_id;
    k.system_id = system_id;

    if (std::holds_alternative<MoveToPoint>(ord)) {
      k.kind = CohortKind::MovePoint;
      const auto& o = std::get<MoveToPoint>(ord);
      k.x_bits = double_bits(o.target_mkm.x);
      k.y_bits = double_bits(o.target_mkm.y);
      return k;
    }
    if (std::holds_alternative<MoveToBody>(ord)) {
      k.kind = CohortKind::MoveBody;
      k.target_id = std::get<MoveToBody>(ord).body_id;
      return k;
    }
    if (std::holds_alternative<ColonizeBody>(ord)) {
      k.kind = CohortKind::MoveBody;
      k.target_id = std::get<ColonizeBody>(ord).body_id;
      return k;
    }
    if (std::holds_alternative<OrbitBody>(ord)) {
      k.kind = CohortKind::OrbitBody;
      k.target_id = std::get<OrbitBody>(ord).body_id;
      return k;
    }
    if (std::holds_alternative<TravelViaJump>(ord)) {
      k.kind = CohortKind::Jump;
      k.target_id = std::get<TravelViaJump>(ord).jump_point_id;
      return k;
    }
    if (std::holds_alternative<AttackShip>(ord)) {
      k.kind = CohortKind::Attack;
      k.target_id = std::get<AttackShip>(ord).target_ship_id;
      return k;
    }
    if (std::holds_alternative<LoadMineral>(ord)) {
      k.kind = CohortKind::Load;
      k.target_id = std::get<LoadMineral>(ord).colony_id;
      return k;
    }
    if (std::holds_alternative<UnloadMineral>(ord)) {
      k.kind = CohortKind::Unload;
      k.target_id = std::get<UnloadMineral>(ord).colony_id;
      return k;
    }
    if (std::holds_alternative<TransferCargoToShip>(ord)) {
      k.kind = CohortKind::Transfer;
      k.target_id = std::get<TransferCargoToShip>(ord).target_ship_id;
      return k;
    }
    if (std::holds_alternative<ScrapShip>(ord)) {
      k.kind = CohortKind::Scrap;
      k.target_id = std::get<ScrapShip>(ord).colony_id;
      return k;
    }

    return std::nullopt;
  };

  std::unordered_map<CohortKey, double, CohortKeyHash> cohort_min_speed_km_s;

  if (cfg_.fleet_speed_matching && !ship_to_fleet.empty()) {
    cohort_min_speed_km_s.reserve(state_.ships.size() * 2);

    for (Id ship_id : ship_ids) {
      const auto* sh = find_ptr(state_.ships, ship_id);
      if (!sh) continue;

      const auto it_fleet = ship_to_fleet.find(ship_id);
      if (it_fleet == ship_to_fleet.end()) continue;

      const auto it_orders = state_.ship_orders.find(ship_id);
      if (it_orders == state_.ship_orders.end()) continue;

      const ShipOrders& so = it_orders->second;
      const Order* ord_ptr = nullptr;
      if (!so.queue.empty()) {
        ord_ptr = &so.queue.front();
      } else if (so.repeat && !so.repeat_template.empty()) {
        // Mirror the main tick loop behaviour where empty queues are refilled
        // from the repeat template.
        ord_ptr = &so.repeat_template.front();
      } else {
        continue;
      }

      const Order& ord = *ord_ptr;
      if (std::holds_alternative<WaitDays>(ord)) continue;

      const auto key_opt = make_cohort_key(it_fleet->second, sh->system_id, ord);
      if (!key_opt) continue;

      // Power gating for fleet speed matching: if a ship cannot power its
      // engines, treat its speed as 0 for cohesion purposes.
      double base_speed_km_s = sh->speed_km_s;
      if (const auto* sd = find_design(sh->design_id)) {
        const auto p = compute_power_allocation(*sd);
        if (!p.engines_online && sd->power_use_engines > 1e-9) {
          base_speed_km_s = 0.0;
        }
      }

      const CohortKey key = *key_opt;
      auto it_min = cohort_min_speed_km_s.find(key);
      if (it_min == cohort_min_speed_km_s.end()) {
        cohort_min_speed_km_s.emplace(key, base_speed_km_s);
      } else {
        it_min->second = std::min(it_min->second, base_speed_km_s);
      }
    }
  }

  struct JumpGroupKey {
    Id fleet_id{kInvalidId};
    Id jump_id{kInvalidId};
    Id system_id{kInvalidId};

    bool operator==(const JumpGroupKey& o) const {
      return fleet_id == o.fleet_id && jump_id == o.jump_id && system_id == o.system_id;
    }
  };

  struct JumpGroupKeyHash {
    size_t operator()(const JumpGroupKey& k) const {
      std::uint64_t h = 1469598103934665603ull;
      auto mix = [&](std::uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;
      };
      mix(k.fleet_id);
      mix(k.jump_id);
      mix(k.system_id);
      return static_cast<size_t>(h);
    }
  };

  struct JumpGroupState {
    int count{0};
    bool valid{false};
    bool ready{false};
    Vec2 jp_pos{0.0, 0.0};
  };

  std::unordered_map<JumpGroupKey, JumpGroupState, JumpGroupKeyHash> jump_group_state;

  if (cfg_.fleet_coordinated_jumps && !ship_to_fleet.empty()) {
    std::unordered_map<JumpGroupKey, std::vector<Id>, JumpGroupKeyHash> group_members;
    group_members.reserve(state_.fleets.size() * 2);

    for (Id ship_id : ship_ids) {
      const auto* sh = find_ptr(state_.ships, ship_id);
      if (!sh) continue;

      const auto it_fleet = ship_to_fleet.find(ship_id);
      if (it_fleet == ship_to_fleet.end()) continue;

      const auto it_orders = state_.ship_orders.find(ship_id);
      if (it_orders == state_.ship_orders.end()) continue;

      const ShipOrders& so = it_orders->second;
      const Order* ord_ptr = nullptr;
      if (!so.queue.empty()) {
        ord_ptr = &so.queue.front();
      } else if (so.repeat && !so.repeat_template.empty()) {
        ord_ptr = &so.repeat_template.front();
      } else {
        continue;
      }

      const Order& ord = *ord_ptr;
      if (!std::holds_alternative<TravelViaJump>(ord)) continue;

      const Id jump_id = std::get<TravelViaJump>(ord).jump_point_id;
      if (jump_id == kInvalidId) continue;

      JumpGroupKey key;
      key.fleet_id = it_fleet->second;
      key.jump_id = jump_id;
      key.system_id = sh->system_id;
      group_members[key].push_back(ship_id);
    }

    jump_group_state.reserve(group_members.size() * 2);

    for (auto& [key, members] : group_members) {
      JumpGroupState st;
      st.count = static_cast<int>(members.size());

      const auto* jp = find_ptr(state_.jump_points, key.jump_id);
      if (jp && jp->system_id == key.system_id) {
        st.valid = true;
        st.jp_pos = jp->position_mkm;
        if (st.count > 1) {
          bool ready = true;
          for (Id sid : members) {
            const auto* s2 = find_ptr(state_.ships, sid);
            if (!s2) {
              ready = false;
              break;
            }
            const double dist = (s2->position_mkm - st.jp_pos).length();
            if (dist > dock_range + 1e-9) {
              ready = false;
              break;
            }
          }
          st.ready = ready;
        }
      }

      jump_group_state.emplace(key, st);
    }
  }

  // Fleet formation offsets (optional).
  //
  // This is intentionally lightweight: we only compute offsets for cohorts
  // where a formation makes sense (currently: move-to-point and attack).
  std::unordered_map<Id, Vec2> formation_offset_mkm;
  if (cfg_.fleet_formations && !ship_to_fleet.empty()) {
    std::unordered_map<CohortKey, std::vector<Id>, CohortKeyHash> cohorts;
    cohorts.reserve(state_.fleets.size() * 2);

    for (Id ship_id : ship_ids) {
      const auto* sh = find_ptr(state_.ships, ship_id);
      if (!sh) continue;

      const auto it_fleet = ship_to_fleet.find(ship_id);
      if (it_fleet == ship_to_fleet.end()) continue;

      const auto* fl = find_ptr(state_.fleets, it_fleet->second);
      if (!fl) continue;
      if (fl->formation == FleetFormation::None) continue;
      if (fl->formation_spacing_mkm <= 0.0) continue;

      const auto it_orders = state_.ship_orders.find(ship_id);
      if (it_orders == state_.ship_orders.end()) continue;

      const ShipOrders& so = it_orders->second;
      const Order* ord_ptr = nullptr;
      if (!so.queue.empty()) {
        ord_ptr = &so.queue.front();
      } else if (so.repeat && !so.repeat_template.empty()) {
        ord_ptr = &so.repeat_template.front();
      } else {
        continue;
      }

      const Order& ord = *ord_ptr;
      if (std::holds_alternative<WaitDays>(ord)) continue;

      const auto key_opt = make_cohort_key(it_fleet->second, sh->system_id, ord);
      if (!key_opt) continue;
      const CohortKey key = *key_opt;
      if (key.kind != CohortKind::MovePoint && key.kind != CohortKind::Attack) continue;

      cohorts[key].push_back(ship_id);
    }

    formation_offset_mkm.reserve(state_.ships.size() * 2);

    auto bits_to_double = [](std::uint64_t bits) -> double {
      double out = 0.0;
      std::memcpy(&out, &bits, sizeof(out));
      return out;
    };

    for (auto& [key, members] : cohorts) {
      if (members.size() < 2) continue;
      std::sort(members.begin(), members.end());
      members.erase(std::unique(members.begin(), members.end()), members.end());
      if (members.size() < 2) continue;

      const auto* fl = find_ptr(state_.fleets, key.fleet_id);
      if (!fl) continue;
      if (fl->formation == FleetFormation::None) continue;

      const double spacing = std::max(0.0, fl->formation_spacing_mkm);
      if (spacing <= 0.0) continue;

      Id leader_id = fl->leader_ship_id;
      if (leader_id == kInvalidId || std::find(members.begin(), members.end(), leader_id) == members.end()) {
        leader_id = members.front();
      }

      const auto* leader = find_ptr(state_.ships, leader_id);
      if (!leader) continue;
      const Vec2 leader_pos = leader->position_mkm;

      Vec2 raw_target = leader_pos + Vec2{1.0, 0.0};
      if (key.kind == CohortKind::MovePoint) {
        raw_target = Vec2{bits_to_double(key.x_bits), bits_to_double(key.y_bits)};
      } else if (key.kind == CohortKind::Attack) {
        const Id target_ship_id = key.target_id;
        const bool detected = is_ship_detected_by_faction(leader->faction_id, target_ship_id);
        if (detected) {
          if (const auto* tgt = find_ptr(state_.ships, target_ship_id)) raw_target = tgt->position_mkm;
        } else {
          const Order* lord_ptr = nullptr;
          if (auto itso = state_.ship_orders.find(leader_id); itso != state_.ship_orders.end()) {
            const ShipOrders& so = itso->second;
            if (!so.queue.empty()) {
              lord_ptr = &so.queue.front();
            } else if (so.repeat && !so.repeat_template.empty()) {
              lord_ptr = &so.repeat_template.front();
            }
          }
          if (lord_ptr && std::holds_alternative<AttackShip>(*lord_ptr)) {
            const auto& ao = std::get<AttackShip>(*lord_ptr);
            if (ao.has_last_known) raw_target = ao.last_known_position_mkm;
          }
        }
      }

      // Shared formation solver (used by UI previews as well).
      const auto offsets = compute_fleet_formation_offsets(fl->formation, spacing, leader_id, leader_pos, raw_target, members);
      for (const auto& [sid, off] : offsets) {
        formation_offset_mkm[sid] = off;
      }
    }
  }

  for (Id ship_id : ship_ids) {
    auto it_ship = state_.ships.find(ship_id);
    if (it_ship == state_.ships.end()) continue;
    auto& ship = it_ship->second;

    const Id fleet_id = [&]() -> Id {
      const auto it_fleet = ship_to_fleet.find(ship_id);
      return (it_fleet != ship_to_fleet.end()) ? it_fleet->second : kInvalidId;
    }();

    auto it = state_.ship_orders.find(ship_id);
    if (it == state_.ship_orders.end()) continue;
    auto& so = it->second;

    if (so.queue.empty() && so.repeat && !so.repeat_template.empty()) {
      so.queue = so.repeat_template;
    }

    auto& q = so.queue;
    if (q.empty()) continue;

    if (std::holds_alternative<WaitDays>(q.front())) {
      auto& ord = std::get<WaitDays>(q.front());
      if (ord.days_remaining <= 0) {
        q.erase(q.begin());
        continue;
      }
      ord.days_remaining -= 1;
      if (ord.days_remaining <= 0) q.erase(q.begin());
      continue;
    }

    Vec2 target = ship.position_mkm;
    double desired_range = 0.0; 
    bool attack_has_contact = false;

    // Cargo vars
    bool is_cargo_op = false;
    // 0=Load, 1=Unload, 2=TransferToShip
    int cargo_mode = 0;

    // Pointers to active orders for updating state
    LoadMineral* load_ord = nullptr;
    UnloadMineral* unload_ord = nullptr;
    TransferCargoToShip* transfer_ord = nullptr;

    Id cargo_colony_id = kInvalidId;
    Id cargo_target_ship_id = kInvalidId;
    std::string cargo_mineral;
    double cargo_tons = 0.0;

    // Troop ops
    bool is_troop_op = false;
    // 0=LoadTroops, 1=UnloadTroops, 2=Invade
    int troop_mode = 0;
    LoadTroops* load_troops_ord = nullptr;
    UnloadTroops* unload_troops_ord = nullptr;
    Id troop_colony_id = kInvalidId;
    double troop_strength = 0.0;

    if (std::holds_alternative<MoveToPoint>(q.front())) {
      target = std::get<MoveToPoint>(q.front()).target_mkm;
    } else if (std::holds_alternative<MoveToBody>(q.front())) {
      const Id body_id = std::get<MoveToBody>(q.front()).body_id;
      const auto* body = find_ptr(state_.bodies, body_id);
      if (!body) {
        q.erase(q.begin());
        continue;
      }
      if (body->system_id != ship.system_id) {
        q.erase(q.begin());
        continue;
      }
      target = body->position_mkm;
    } else if (std::holds_alternative<ColonizeBody>(q.front())) {
      const auto& ord = std::get<ColonizeBody>(q.front());
      const Id body_id = ord.body_id;
      const auto* body = find_ptr(state_.bodies, body_id);
      if (!body) {
        q.erase(q.begin());
        continue;
      }
      if (body->system_id != ship.system_id) {
        q.erase(q.begin());
        continue;
      }
      target = body->position_mkm;
    } else if (std::holds_alternative<OrbitBody>(q.front())) {
      auto& ord = std::get<OrbitBody>(q.front());
      const Id body_id = ord.body_id;
      const auto* body = find_ptr(state_.bodies, body_id);
      if (!body || body->system_id != ship.system_id) {
        q.erase(q.begin());
        continue;
      }
      target = body->position_mkm;
    } else if (std::holds_alternative<TravelViaJump>(q.front())) {
      const Id jump_id = std::get<TravelViaJump>(q.front()).jump_point_id;
      const auto* jp = find_ptr(state_.jump_points, jump_id);
      if (!jp || jp->system_id != ship.system_id) {
        q.erase(q.begin());
        continue;
      }
      target = jp->position_mkm;
    } else if (std::holds_alternative<AttackShip>(q.front())) {
      auto& ord = std::get<AttackShip>(q.front());
      const Id target_id = ord.target_ship_id;
      const auto* tgt = find_ptr(state_.ships, target_id);
      if (!tgt || tgt->system_id != ship.system_id) {
        q.erase(q.begin());
        continue;
      }

      if (tgt->faction_id == ship.faction_id) {
        // Target changed hands (captured) or is otherwise no longer hostile.
        q.erase(q.begin());
        continue;
      }

      attack_has_contact = is_ship_detected_by_faction(ship.faction_id, target_id);
      // An explicit AttackShip order acts as a de-facto declaration of hostilities if needed.
      if (attack_has_contact && !are_factions_hostile(ship.faction_id, tgt->faction_id)) {
        set_diplomatic_status(ship.faction_id, tgt->faction_id, DiplomacyStatus::Hostile, /*reciprocal=*/true,
                             /*push_event_on_change=*/true);
      }


      if (attack_has_contact) {
        target = tgt->position_mkm;
        ord.last_known_position_mkm = target;
        ord.has_last_known = true;
        const auto* d = find_design(ship.design_id);
        const double w_range = d ? d->weapon_range_mkm : 0.0;
        desired_range = (w_range > 0.0) ? (w_range * 0.9) : 0.1;
        // If the target is disabled and we have troops, close to boarding range.
        if (cfg_.enable_boarding && ship.troops >= cfg_.boarding_min_attacker_troops) {
          const auto* td = find_design(tgt->design_id);
          const double tmax_hp = (td && td->max_hp > 1e-9) ? td->max_hp : std::max(1.0, tgt->hp);
          const double hp_frac = (tmax_hp > 1e-9) ? (tgt->hp / tmax_hp) : 1.0;
          const bool shields_ok = (!cfg_.boarding_require_shields_down) || (tgt->shields <= 1e-9);
          if (shields_ok && hp_frac <= cfg_.boarding_target_hp_fraction) {
            desired_range = std::min(desired_range, std::max(0.0, cfg_.boarding_range_mkm));
          }
        }
      } else {
        if (!ord.has_last_known) {
          q.erase(q.begin());
          continue;
        }
        target = ord.last_known_position_mkm;
        desired_range = 0.0;
      }
    } else if (std::holds_alternative<LoadMineral>(q.front())) {
      auto& ord = std::get<LoadMineral>(q.front());
      is_cargo_op = true;
      cargo_mode = 0;
      load_ord = &ord;
      cargo_colony_id = ord.colony_id;
      cargo_mineral = ord.mineral;
      cargo_tons = ord.tons;
      const auto* colony = find_ptr(state_.colonies, cargo_colony_id);
      if (!colony || colony->faction_id != ship.faction_id) { q.erase(q.begin()); continue; }
      const auto* body = find_ptr(state_.bodies, colony->body_id);
      if (!body || body->system_id != ship.system_id) { q.erase(q.begin()); continue; }
      target = body->position_mkm;
    } else if (std::holds_alternative<UnloadMineral>(q.front())) {
      auto& ord = std::get<UnloadMineral>(q.front());
      is_cargo_op = true;
      cargo_mode = 1;
      unload_ord = &ord;
      cargo_colony_id = ord.colony_id;
      cargo_mineral = ord.mineral;
      cargo_tons = ord.tons;
      const auto* colony = find_ptr(state_.colonies, cargo_colony_id);
      if (!colony || colony->faction_id != ship.faction_id) { q.erase(q.begin()); continue; }
      const auto* body = find_ptr(state_.bodies, colony->body_id);
      if (!body || body->system_id != ship.system_id) { q.erase(q.begin()); continue; }
      target = body->position_mkm;
    } else if (std::holds_alternative<LoadTroops>(q.front())) {
      auto& ord = std::get<LoadTroops>(q.front());
      is_troop_op = true;
      troop_mode = 0;
      load_troops_ord = &ord;
      troop_colony_id = ord.colony_id;
      troop_strength = ord.strength;
      const auto* colony = find_ptr(state_.colonies, troop_colony_id);
      if (!colony || colony->faction_id != ship.faction_id) { q.erase(q.begin()); continue; }
      const auto* body = find_ptr(state_.bodies, colony->body_id);
      if (!body || body->system_id != ship.system_id) { q.erase(q.begin()); continue; }
      target = body->position_mkm;
    } else if (std::holds_alternative<UnloadTroops>(q.front())) {
      auto& ord = std::get<UnloadTroops>(q.front());
      is_troop_op = true;
      troop_mode = 1;
      unload_troops_ord = &ord;
      troop_colony_id = ord.colony_id;
      troop_strength = ord.strength;
      const auto* colony = find_ptr(state_.colonies, troop_colony_id);
      if (!colony || colony->faction_id != ship.faction_id) { q.erase(q.begin()); continue; }
      const auto* body = find_ptr(state_.bodies, colony->body_id);
      if (!body || body->system_id != ship.system_id) { q.erase(q.begin()); continue; }
      target = body->position_mkm;
    } else if (std::holds_alternative<InvadeColony>(q.front())) {
      auto& ord = std::get<InvadeColony>(q.front());
      is_troop_op = true;
      troop_mode = 2;
      troop_colony_id = ord.colony_id;
      const auto* colony = find_ptr(state_.colonies, troop_colony_id);
      if (!colony) { q.erase(q.begin()); continue; }
      if (colony->faction_id == ship.faction_id) { q.erase(q.begin()); continue; }
      const auto* body = find_ptr(state_.bodies, colony->body_id);
      if (!body || body->system_id != ship.system_id) { q.erase(q.begin()); continue; }

      // An explicit invasion is an act of hostility.
      if (!are_factions_hostile(ship.faction_id, colony->faction_id)) {
        set_diplomatic_status(ship.faction_id, colony->faction_id, DiplomacyStatus::Hostile, /*reciprocal=*/true,
                             /*push_event_on_change=*/true);
      }
      target = body->position_mkm;
    } else if (std::holds_alternative<TransferCargoToShip>(q.front())) {
      auto& ord = std::get<TransferCargoToShip>(q.front());
      is_cargo_op = true;
      cargo_mode = 2;
      transfer_ord = &ord;
      cargo_target_ship_id = ord.target_ship_id;
      cargo_mineral = ord.mineral;
      cargo_tons = ord.tons;
      const auto* tgt = find_ptr(state_.ships, cargo_target_ship_id);
      // Valid target check: exists, same system, same faction
      if (!tgt || tgt->system_id != ship.system_id || tgt->faction_id != ship.faction_id) {
        q.erase(q.begin());
        continue;
      }
      target = tgt->position_mkm;
    } else if (std::holds_alternative<ScrapShip>(q.front())) {
      auto& ord = std::get<ScrapShip>(q.front());
      const auto* colony = find_ptr(state_.colonies, ord.colony_id);
      if (!colony || colony->faction_id != ship.faction_id) { q.erase(q.begin()); continue; }
      const auto* body = find_ptr(state_.bodies, colony->body_id);
      if (!body || body->system_id != ship.system_id) { q.erase(q.begin()); continue; }
      target = body->position_mkm;
    }

    // Fleet formation: optionally offset the movement/attack target.
    if (cfg_.fleet_formations && fleet_id != kInvalidId && !formation_offset_mkm.empty()) {
      const bool can_offset = std::holds_alternative<MoveToPoint>(q.front()) ||
                              std::holds_alternative<AttackShip>(q.front());
      if (can_offset) {
        if (auto itoff = formation_offset_mkm.find(ship_id); itoff != formation_offset_mkm.end()) {
          target = target + itoff->second;
        }
      }
    }

    const Vec2 delta = target - ship.position_mkm;
    const double dist = delta.length();

    const bool is_attack = std::holds_alternative<AttackShip>(q.front());
    const bool is_jump = std::holds_alternative<TravelViaJump>(q.front());
    const bool is_move_body = std::holds_alternative<MoveToBody>(q.front());
    const bool is_colonize = std::holds_alternative<ColonizeBody>(q.front());
    const bool is_body = is_move_body || is_colonize;
    const bool is_orbit = std::holds_alternative<OrbitBody>(q.front());
    const bool is_scrap = std::holds_alternative<ScrapShip>(q.front());

    // Fleet jump coordination: if multiple ships in the same fleet are trying to
    // transit the same jump point in the same system, we can optionally hold the
    // transit until all of them have arrived.
    bool is_coordinated_jump_group = false;
    bool allow_jump_transit = true;
    if (is_jump && cfg_.fleet_coordinated_jumps && fleet_id != kInvalidId && !jump_group_state.empty()) {
      const Id jump_id = std::get<TravelViaJump>(q.front()).jump_point_id;
      JumpGroupKey key;
      key.fleet_id = fleet_id;
      key.jump_id = jump_id;
      key.system_id = ship.system_id;

      const auto itjg = jump_group_state.find(key);
      if (itjg != jump_group_state.end() && itjg->second.valid && itjg->second.count > 1) {
        is_coordinated_jump_group = true;
        allow_jump_transit = itjg->second.ready;
      }
    }

    auto do_cargo_transfer = [&]() -> double {
      // mode 0=load from col, 1=unload to col, 2=transfer to ship
      
      std::unordered_map<std::string, double>* source_minerals = nullptr;
      std::unordered_map<std::string, double>* dest_minerals = nullptr;
      double dest_capacity_free = 1e300;

      if (cargo_mode == 0) { // Load from colony
        auto* col = find_ptr(state_.colonies, cargo_colony_id);
        if (!col) return 0.0;
        source_minerals = &col->minerals;
        dest_minerals = &ship.cargo;
        
        const auto* d = find_design(ship.design_id);
        const double cap = d ? d->cargo_tons : 0.0;
        dest_capacity_free = std::max(0.0, cap - cargo_used_tons(ship));
      } else if (cargo_mode == 1) { // Unload to colony
        auto* col = find_ptr(state_.colonies, cargo_colony_id);
        if (!col) return 0.0;
        source_minerals = &ship.cargo;
        dest_minerals = &col->minerals;
        // Colony has infinite capacity
      } else if (cargo_mode == 2) { // Transfer to ship
        auto* tgt = find_ptr(state_.ships, cargo_target_ship_id);
        if (!tgt) return 0.0;
        source_minerals = &ship.cargo;
        dest_minerals = &tgt->cargo;
        
        const auto* d = find_design(tgt->design_id);
        const double cap = d ? d->cargo_tons : 0.0;
        dest_capacity_free = std::max(0.0, cap - cargo_used_tons(*tgt));
      }

      if (!source_minerals || !dest_minerals) return 0.0;
      if (dest_capacity_free <= 1e-9) return 0.0;

      double moved_total = 0.0;
      double remaining_request = (cargo_tons > 0.0) ? cargo_tons : 1e300;
      remaining_request = std::min(remaining_request, dest_capacity_free);

      auto transfer_one = [&](const std::string& min_type, double amount_limit) {
        if (amount_limit <= 1e-9) return 0.0;
        auto it_src = source_minerals->find(min_type);
        const double have = (it_src != source_minerals->end()) ? std::max(0.0, it_src->second) : 0.0;
        const double take = std::min(have, amount_limit);
        if (take > 1e-9) {
          (*dest_minerals)[min_type] += take;
          if (it_src != source_minerals->end()) {
            it_src->second = std::max(0.0, it_src->second - take);
            if (it_src->second <= 1e-9) source_minerals->erase(it_src);
          }
          moved_total += take;
        }
        return take;
      };

      if (!cargo_mineral.empty()) {
        transfer_one(cargo_mineral, remaining_request);
        return moved_total;
      }

      std::vector<std::string> keys;
      keys.reserve(source_minerals->size());
      for (const auto& [k, v] : *source_minerals) {
        if (v > 1e-9) keys.push_back(k);
      }
      std::sort(keys.begin(), keys.end());

      for (const auto& k : keys) {
        if (remaining_request <= 1e-9) break;
        const double moved = transfer_one(k, remaining_request);
        remaining_request -= moved;
      }
      return moved_total;
    };

    auto cargo_order_complete = [&](double moved_this_tick) {
      if (cargo_tons <= 0.0) return true; // "As much as possible" -> done after one attempt? No, standard logic usually implies until full/empty.
                                          // But for simplicity, we'll stick to: if we requested unlimited, we try until we can't move anymore.
      
      // Update remaining tons in the order struct
      if (cargo_mode == 0 && load_ord) {
        load_ord->tons = std::max(0.0, load_ord->tons - moved_this_tick);
        cargo_tons = load_ord->tons;
      } else if (cargo_mode == 1 && unload_ord) {
        unload_ord->tons = std::max(0.0, unload_ord->tons - moved_this_tick);
        cargo_tons = unload_ord->tons;
      } else if (cargo_mode == 2 && transfer_ord) {
        transfer_ord->tons = std::max(0.0, transfer_ord->tons - moved_this_tick);
        cargo_tons = transfer_ord->tons;
      }

      if (cargo_tons <= 1e-9) return true;

      // If we couldn't move anything this tick, check if we are blocked (full/empty).
      if (moved_this_tick <= 1e-9) {
          // Simplistic check: if we moved nothing, we are likely done or blocked.
          return true;
      }
      return false;
    };

    // --- Docking / Arrival Checks ---

    if (is_cargo_op && dist <= dock_range) {
      ship.position_mkm = target;
      const double moved = do_cargo_transfer();
      if (cargo_order_complete(moved)) q.erase(q.begin());
      continue;
    }
    if (is_troop_op && dist <= dock_range) {
      ship.position_mkm = target;

      const auto* design = find_design(ship.design_id);
      const double cap = design ? std::max(0.0, design->troop_capacity) : 0.0;

      auto* col = find_ptr(state_.colonies, troop_colony_id);
      if (!col) {
        q.erase(q.begin());
        continue;
      }

      auto transfer_amount = [&](double want, double available, double free_cap) -> double {
        double take = (want <= 0.0) ? 1e300 : want;
        take = std::min(take, available);
        take = std::min(take, free_cap);
        if (take < 0.0) take = 0.0;
        return take;
      };

      if (troop_mode == 0) {
        // Load from colony garrison.
        const double free_cap = std::max(0.0, cap - ship.troops);
        const double moved = transfer_amount(load_troops_ord ? load_troops_ord->strength : troop_strength,
                                             std::max(0.0, col->ground_forces), free_cap);
        if (moved > 1e-9) {
          ship.troops += moved;
          col->ground_forces = std::max(0.0, col->ground_forces - moved);
        }
      } else if (troop_mode == 1) {
        // Unload into colony garrison.
        const double moved = transfer_amount(unload_troops_ord ? unload_troops_ord->strength : troop_strength,
                                             std::max(0.0, ship.troops), 1e300);
        if (moved > 1e-9) {
          ship.troops = std::max(0.0, ship.troops - moved);
          col->ground_forces += moved;
        }
      } else if (troop_mode == 2) {
        // Invade (disembark all or requested troops into attacker strength).
        if (ship.troops <= 1e-9) {
          q.erase(q.begin());
          continue;
        }
        const double moved = ship.troops;
        ship.troops = 0.0;

        GroundBattle& b = state_.ground_battles[col->id];
        if (b.colony_id == kInvalidId) {
          b.colony_id = col->id;
          b.system_id = ship.system_id;
          b.attacker_faction_id = ship.faction_id;
          b.defender_faction_id = col->faction_id;
          b.attacker_strength = 0.0;
          b.defender_strength = std::max(0.0, col->ground_forces);
          b.days_fought = 0;
        }
        // Reinforcement: if attacker changes, treat as a new battle by replacing.
        if (b.attacker_faction_id != ship.faction_id) {
          b.attacker_faction_id = ship.faction_id;
          b.defender_faction_id = col->faction_id;
          b.attacker_strength = 0.0;
          b.defender_strength = std::max(0.0, col->ground_forces);
          b.days_fought = 0;
        }
        b.attacker_strength += moved;
      }

      q.erase(q.begin());
      continue;
    }
    if (is_scrap && dist <= dock_range) {
      // Decommission the ship at a friendly colony.
      // - Return carried cargo minerals to the colony stockpile.
      // - Refund a fraction of shipyard mineral costs (estimated by design mass * build_costs_per_ton).
      ship.position_mkm = target;

      const ScrapShip ord = std::get<ScrapShip>(q.front()); // copy (we may erase the ship)
      q.erase(q.begin());

      auto* col = find_ptr(state_.colonies, ord.colony_id);
      if (!col || col->faction_id != ship.faction_id) {
        continue;
      }

      // Snapshot before erasing from state_.
      const Ship ship_snapshot = ship;

      // Return cargo to colony.
      for (const auto& [mineral, tons] : ship_snapshot.cargo) {
        if (tons > 1e-9) col->minerals[mineral] += tons;
      }

      // Return remaining fuel (if any).
      if (ship_snapshot.fuel_tons > 1e-9) col->minerals["Fuel"] += ship_snapshot.fuel_tons;

      // Refund a fraction of shipyard build costs (if configured/content available).
      std::unordered_map<std::string, double> refunded;
      const double refund_frac = std::clamp(cfg_.scrap_refund_fraction, 0.0, 1.0);

      if (refund_frac > 1e-9) {
        const auto it_yard = content_.installations.find("shipyard");
        const auto* design = find_design(ship_snapshot.design_id);
        if (it_yard != content_.installations.end() && design) {
          const double mass_tons = std::max(0.0, design->mass_tons);
          for (const auto& [mineral, per_ton] : it_yard->second.build_costs_per_ton) {
            if (per_ton <= 0.0) continue;
            const double amt = mass_tons * per_ton * refund_frac;
            if (amt > 1e-9) {
              refunded[mineral] += amt;
              col->minerals[mineral] += amt;
            }
          }
        }
      }

      // Remove ship from the system list.
      if (auto* sys = find_ptr(state_.systems, ship_snapshot.system_id)) {
        sys->ships.erase(std::remove(sys->ships.begin(), sys->ships.end(), ship_id), sys->ships.end());
      }

      // Remove ship orders, contacts, and the ship itself.
      state_.ship_orders.erase(ship_id);
      state_.ships.erase(ship_id);

      // Keep fleet membership consistent.
      remove_ship_from_fleets(ship_id);

      for (auto& [_, fac] : state_.factions) {
        fac.ship_contacts.erase(ship_id);
      }

      // Record event.
      {
        std::string msg = "Ship scrapped at " + col->name + ": " + ship_snapshot.name;
        if (!refunded.empty()) {
          std::vector<std::string> keys;
          keys.reserve(refunded.size());
          for (const auto& [k, _] : refunded) keys.push_back(k);
          std::sort(keys.begin(), keys.end());

          msg += " (refund:";
          for (const auto& k : keys) {
            const double v = refunded[k];
            // Print near-integers cleanly.
            if (std::fabs(v - std::round(v)) < 1e-6) {
              msg += " " + k + " " + std::to_string(static_cast<long long>(std::llround(v)));
            } else {
              // Use a compact representation for fractional refunds.
              std::ostringstream ss;
              ss.setf(std::ios::fixed);
              ss.precision(2);
              ss << v;
              msg += " " + k + " " + ss.str();
            }
          }
          msg += ")";
        }

        EventContext ctx;
        ctx.faction_id = col->faction_id;
        ctx.system_id = ship_snapshot.system_id;
        ctx.ship_id = ship_snapshot.id;
        ctx.colony_id = col->id;
        push_event(EventLevel::Info, EventCategory::Shipyard, msg, ctx);
      }

      continue;
    }

    if (is_colonize && dist <= dock_range) {
      ship.position_mkm = target;

      const ColonizeBody ord = std::get<ColonizeBody>(q.front()); // copy (we may erase the ship)
      q.erase(q.begin());

      const auto* body = find_ptr(state_.bodies, ord.body_id);
      if (!body || body->system_id != ship.system_id) {
        continue;
      }

      const bool colonizable = (body->type == BodyType::Planet || body->type == BodyType::Moon ||
                                body->type == BodyType::Asteroid);
      if (!colonizable) {
        EventContext ctx;
        ctx.faction_id = ship.faction_id;
        ctx.system_id = ship.system_id;
        ctx.ship_id = ship_id;
        push_event(EventLevel::Warn, EventCategory::Exploration,
                  "Colonization failed: target body is not colonizable: " + body->name, ctx);
        continue;
      }

      // Ensure the body is not already colonized.
      Id existing_colony_id = kInvalidId;
      std::string existing_colony_name;
      for (const auto& [cid, col] : state_.colonies) {
        if (col.body_id == body->id) {
          existing_colony_id = cid;
          existing_colony_name = col.name;
          break;
        }
      }
      if (existing_colony_id != kInvalidId) {
        EventContext ctx;
        ctx.faction_id = ship.faction_id;
        ctx.system_id = ship.system_id;
        ctx.ship_id = ship_id;
        ctx.colony_id = existing_colony_id;
        push_event(EventLevel::Info, EventCategory::Exploration,
                  "Colonization aborted: " + body->name + " already has a colony (" + existing_colony_name + ")", ctx);
        continue;
      }

      {
        const Ship ship_snapshot = ship;
        const ShipDesign* d = find_design(ship_snapshot.design_id);
        const double cap = d ? d->colony_capacity_millions : 0.0;
        if (cap <= 1e-9) {
          EventContext ctx;
          ctx.faction_id = ship_snapshot.faction_id;
          ctx.system_id = ship_snapshot.system_id;
          ctx.ship_id = ship_snapshot.id;
          push_event(EventLevel::Warn, EventCategory::Exploration,
                    "Colonization failed: ship has no colony module capacity: " + ship_snapshot.name, ctx);
          continue;
        }

        // Choose a unique colony name.
        auto name_exists = [&](const std::string& n) {
          for (const auto& [_, c] : state_.colonies) {
            if (c.name == n) return true;
          }
          return false;
        };
        const std::string base_name = !ord.colony_name.empty() ? ord.colony_name : (body->name + " Colony");
        std::string final_name = base_name;
        for (int suffix = 2; name_exists(final_name); ++suffix) {
          final_name = base_name + " (" + std::to_string(suffix) + ")";
        }

        Colony new_col;
        new_col.id = allocate_id(state_);
        new_col.name = final_name;
        new_col.faction_id = ship_snapshot.faction_id;
        new_col.body_id = body->id;
        new_col.population_millions = cap;

        // Transfer all carried cargo minerals to the new colony.
        for (const auto& [mineral, tons] : ship_snapshot.cargo) {
          if (tons > 1e-9) new_col.minerals[mineral] += tons;
        }

        state_.colonies[new_col.id] = new_col;

        // Ensure the faction has this system discovered (also invalidates route caches if newly discovered).
        discover_system_for_faction(ship_snapshot.faction_id, body->system_id);

        // Remove the ship from the system list.
        if (auto* sys = find_ptr(state_.systems, ship_snapshot.system_id)) {
          sys->ships.erase(std::remove(sys->ships.begin(), sys->ships.end(), ship_id), sys->ships.end());
        }

        // Remove ship orders, contacts, and the ship itself.
        state_.ship_orders.erase(ship_id);
        state_.ships.erase(ship_id);

        // Keep fleet membership consistent.
        remove_ship_from_fleets(ship_id);

        for (auto& [_, fac] : state_.factions) {
          fac.ship_contacts.erase(ship_id);
        }

        // Record event.
        {
          std::ostringstream ss;
          ss.setf(std::ios::fixed);
          ss.precision(0);
          ss << cap;
          const std::string msg = "Colony established: " + final_name + " on " + body->name +
                                  " (population " + ss.str() + "M)";
          EventContext ctx;
          ctx.faction_id = new_col.faction_id;
          ctx.system_id = ship_snapshot.system_id;
          ctx.ship_id = ship_snapshot.id;
          ctx.colony_id = new_col.id;
          push_event(EventLevel::Info, EventCategory::Exploration, msg, ctx);
        }
      }

      continue;
    }

    if (is_move_body && dist <= dock_range) {
      ship.position_mkm = target;
      q.erase(q.begin());
      continue;
    }

    if (is_orbit && dist <= dock_range) {
      ship.position_mkm = target; // snap to body
      auto& ord = std::get<OrbitBody>(q.front());
      if (ord.duration_days > 0) {
        ord.duration_days--;
      }
      if (ord.duration_days == 0) {
        q.erase(q.begin());
      }
      // If -1, we stay here forever (until order cancelled).
      continue;
    }

    if (!is_attack && !is_jump && !is_cargo_op && !is_body && !is_orbit && !is_scrap && dist <= arrive_eps) {
      q.erase(q.begin());
      continue;
    }

    auto transit_jump = [&]() {
      const Id jump_id = std::get<TravelViaJump>(q.front()).jump_point_id;
      const auto* jp = find_ptr(state_.jump_points, jump_id);
      if (!jp || jp->system_id != ship.system_id || jp->linked_jump_id == kInvalidId) return;

      const auto* dest = find_ptr(state_.jump_points, jp->linked_jump_id);
      if (!dest) return;

      const Id old_sys = ship.system_id;
      const Id new_sys = dest->system_id;

      if (auto* sys_old = find_ptr(state_.systems, old_sys)) {
        sys_old->ships.erase(std::remove(sys_old->ships.begin(), sys_old->ships.end(), ship_id), sys_old->ships.end());
      }

      ship.system_id = new_sys;
      ship.position_mkm = dest->position_mkm;

      if (auto* sys_new = find_ptr(state_.systems, new_sys)) {
        sys_new->ships.push_back(ship_id);
      }

      discover_system_for_faction(ship.faction_id, new_sys);

      {
        const auto* sys_new = find_ptr(state_.systems, new_sys);
        const std::string dest_name = sys_new ? sys_new->name : std::string("(unknown)");
        const std::string msg = "Ship " + ship.name + " transited jump point " + jp->name + " -> " + dest_name;
        nebula4x::log::info(msg);
        EventContext ctx;
        ctx.faction_id = ship.faction_id;
        ctx.system_id = new_sys;
        ctx.ship_id = ship_id;
        push_event(EventLevel::Info, EventCategory::Movement, msg, ctx);
      }
    };

    if (is_jump && dist <= dock_range) {
      ship.position_mkm = target;
      if (!is_coordinated_jump_group || allow_jump_transit) {
        transit_jump();
        q.erase(q.begin());
      }
      continue;
    }

    if (is_attack) {
      if (attack_has_contact) {
        if (dist <= desired_range) {
          continue;
        }
      } else {
        if (dist <= arrive_eps) {
          q.erase(q.begin());
          continue;
        }
      }
    }

    const auto* sd = find_design(ship.design_id);

    // Power gating: if engines draw power and the ship can't allocate it, it
    // cannot move this tick.
    double effective_speed_km_s = ship.speed_km_s;
    if (sd) {
      const auto p = compute_power_allocation(*sd);
      if (!p.engines_online && sd->power_use_engines > 1e-9) {
        effective_speed_km_s = 0.0;
      }
    }

    // Fleet speed matching: for ships in the same fleet with the same current
    // movement order, cap speed to the slowest ship in that cohort.
    if (cfg_.fleet_speed_matching && fleet_id != kInvalidId && !cohort_min_speed_km_s.empty()) {
      const auto key_opt = make_cohort_key(fleet_id, ship.system_id, q.front());
      if (key_opt) {
        const auto it_min = cohort_min_speed_km_s.find(*key_opt);
        if (it_min != cohort_min_speed_km_s.end()) {
          effective_speed_km_s = std::min(effective_speed_km_s, it_min->second);
        }
      }
    }

    const double max_step = mkm_per_day_from_speed(effective_speed_km_s, cfg_.seconds_per_day);
    if (max_step <= 0.0) continue;

    double step = max_step;
    if (is_attack) {
      step = std::min(step, std::max(0.0, dist - desired_range));
      if (step <= 0.0) continue;
    }

    const double fuel_cap = sd ? std::max(0.0, sd->fuel_capacity_tons) : 0.0;
    const double fuel_use = sd ? std::max(0.0, sd->fuel_use_per_mkm) : 0.0;
    const bool uses_fuel = (fuel_use > 0.0);
    if (uses_fuel) {
      // Be defensive for older saves/custom content that may not have been initialized yet.
      if (ship.fuel_tons < 0.0) ship.fuel_tons = fuel_cap;
      ship.fuel_tons = std::clamp(ship.fuel_tons, 0.0, fuel_cap);

      const double max_by_fuel = ship.fuel_tons / fuel_use;
      step = std::min(step, max_by_fuel);
      if (step <= 1e-12) continue;
    }

    auto burn_fuel = [&](double moved_mkm) {
      if (!uses_fuel || moved_mkm <= 0.0) return;
      const double before = ship.fuel_tons;
      const double burn = moved_mkm * fuel_use;
      ship.fuel_tons = std::max(0.0, ship.fuel_tons - burn);
      if (before > 1e-9 && ship.fuel_tons <= 1e-9) {
        const auto* sys = find_ptr(state_.systems, ship.system_id);
        const std::string sys_name = sys ? sys->name : std::string("(unknown)");
        const std::string msg = "Ship " + ship.name + " has run out of Fuel in " + sys_name;
        EventContext ctx;
        ctx.faction_id = ship.faction_id;
        ctx.system_id = ship.system_id;
        ctx.ship_id = ship_id;
        push_event(EventLevel::Warn, EventCategory::Movement, msg, ctx);
      }
    };

    if (dist <= step) {
      ship.position_mkm = target;

      burn_fuel(dist);

      if (is_jump) {
        if (!is_coordinated_jump_group || allow_jump_transit) {
          transit_jump();
          q.erase(q.begin());
        }
      } else if (is_attack) {
        if (!attack_has_contact) q.erase(q.begin());
      } else if (is_cargo_op) {
        const double moved = do_cargo_transfer();
        if (cargo_order_complete(moved)) q.erase(q.begin());
      } else if (is_scrap) {
          // Re-check scrap logic in case we arrived exactly on this frame
          // For now, simpler to wait for next tick's "in range" check which is cleaner
      } else if (is_orbit) {
          // Arrived at orbit body.
          // Don't pop; handled by duration logic next tick.
      } else {
        q.erase(q.begin());
      }
      continue;
    }

    const Vec2 dir = delta.normalized();
    ship.position_mkm += dir * step;
    burn_fuel(step);
  }
}



} // namespace nebula4x
