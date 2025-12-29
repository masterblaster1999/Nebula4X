#include "nebula4x/util/timeline_export.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nebula4x/util/json.h"

namespace nebula4x {
namespace {

std::string faction_control_label(FactionControl c) {
  switch (c) {
    case FactionControl::Player:
      return "player";
    case FactionControl::AI_Passive:
      return "ai_passive";
    case FactionControl::AI_Explorer:
      return "ai_explorer";
    case FactionControl::AI_Pirate:
      return "ai_pirate";
  }
  return "unknown";
}

template <typename Map>
std::vector<typename Map::key_type> sorted_keys(const Map& m) {
  std::vector<typename Map::key_type> keys;
  keys.reserve(m.size());
  for (const auto& [k, _] : m) keys.push_back(k);
  std::sort(keys.begin(), keys.end());
  return keys;
}

bool mineral_key_allowed(const TimelineExportOptions& opt, const std::string& key) {
  if (opt.mineral_filter.empty()) return true;
  return std::find(opt.mineral_filter.begin(), opt.mineral_filter.end(), key) != opt.mineral_filter.end();
}

void add_minerals(std::unordered_map<std::string, double>& out,
                  const std::unordered_map<std::string, double>& in,
                  const TimelineExportOptions& opt) {
  for (const auto& [k, v] : in) {
    if (!mineral_key_allowed(opt, k)) continue;
    out[k] += v;
  }
}

json::Value map_string_double_to_json(const std::unordered_map<std::string, double>& m) {
  json::Object o;
  for (const auto& k : sorted_keys(m)) {
    auto it = m.find(k);
    if (it == m.end()) continue;
    o[k] = it->second;
  }
  return json::object(std::move(o));
}

json::Value snapshot_to_json(const TimelineSnapshot& s) {
  json::Object root;
  root["day"] = static_cast<double>(s.day);
  root["date"] = s.date;
  root["state_digest"] = digest64_to_hex(s.state_digest);
  root["content_digest"] = digest64_to_hex(s.content_digest);
  root["next_event_seq"] = std::to_string(static_cast<unsigned long long>(s.next_event_seq));
  root["events_size"] = static_cast<double>(s.events_size);
  root["new_events"] = static_cast<double>(s.new_events);
  root["new_events_retained"] = static_cast<double>(s.new_events_retained);
  root["new_info"] = static_cast<double>(s.new_info);
  root["new_warn"] = static_cast<double>(s.new_warn);
  root["new_error"] = static_cast<double>(s.new_error);

  json::Object counts;
  counts["systems"] = static_cast<double>(s.systems);
  counts["bodies"] = static_cast<double>(s.bodies);
  counts["jump_points"] = static_cast<double>(s.jump_points);
  counts["ships"] = static_cast<double>(s.ships);
  counts["colonies"] = static_cast<double>(s.colonies);
  counts["fleets"] = static_cast<double>(s.fleets);
  root["counts"] = json::object(std::move(counts));

  json::Array factions;
  factions.reserve(s.factions.size());
  for (const auto& f : s.factions) {
    json::Object fo;
    fo["id"] = static_cast<double>(f.faction_id);
    fo["name"] = f.name;
    fo["control"] = faction_control_label(f.control);
    fo["ships"] = static_cast<double>(f.ships);
    fo["colonies"] = static_cast<double>(f.colonies);
    fo["fleets"] = static_cast<double>(f.fleets);
    fo["population_millions"] = f.population_millions;
    fo["research_points"] = f.research_points;
    fo["active_research_id"] = f.active_research_id;
    fo["active_research_progress"] = f.active_research_progress;
    fo["known_techs"] = static_cast<double>(f.known_techs);
    fo["discovered_systems"] = static_cast<double>(f.discovered_systems);
    fo["contacts"] = static_cast<double>(f.contacts);
    if (!f.minerals.empty()) fo["minerals"] = map_string_double_to_json(f.minerals);
    if (!f.ship_cargo.empty()) fo["ship_cargo"] = map_string_double_to_json(f.ship_cargo);
    factions.push_back(json::object(std::move(fo)));
  }
  root["factions"] = json::array(std::move(factions));

  return json::object(std::move(root));
}

} // namespace

TimelineSnapshot compute_timeline_snapshot(const GameState& state,
                                          const ContentDB& content,
                                          std::uint64_t content_digest,
                                          std::uint64_t prev_next_event_seq,
                                          const TimelineExportOptions& opt) {
  TimelineSnapshot snap;
  snap.day = state.date.days_since_epoch();
  snap.date = state.date.to_string();
  snap.content_digest = content_digest;
  snap.state_digest = digest_game_state64(state, opt.digest);

  snap.systems = static_cast<int>(state.systems.size());
  snap.bodies = static_cast<int>(state.bodies.size());
  snap.jump_points = static_cast<int>(state.jump_points.size());
  snap.ships = static_cast<int>(state.ships.size());
  snap.colonies = static_cast<int>(state.colonies.size());
  snap.fleets = static_cast<int>(state.fleets.size());

  snap.next_event_seq = state.next_event_seq;
  snap.events_size = state.events.size();
  if (state.next_event_seq >= prev_next_event_seq) {
    snap.new_events = state.next_event_seq - prev_next_event_seq;
  }
  for (const auto& e : state.events) {
    if (e.seq < prev_next_event_seq) continue;
    ++snap.new_events_retained;
    switch (e.level) {
      case EventLevel::Info:
        ++snap.new_info;
        break;
      case EventLevel::Warn:
        ++snap.new_warn;
        break;
      case EventLevel::Error:
        ++snap.new_error;
        break;
    }
  }

  // Build per-faction rows.
  std::unordered_map<Id, std::size_t> index;
  snap.factions.reserve(state.factions.size());
  for (Id fid : sorted_keys(state.factions)) {
    const auto& f = state.factions.at(fid);
    TimelineSnapshot::FactionSnapshot fs;
    fs.faction_id = fid;
    fs.name = f.name;
    fs.control = f.control;
    fs.research_points = f.research_points;
    fs.active_research_id = f.active_research_id;
    fs.active_research_progress = f.active_research_progress;
    fs.known_techs = static_cast<int>(f.known_techs.size());
    fs.discovered_systems = static_cast<int>(f.discovered_systems.size());
    fs.contacts = static_cast<int>(f.ship_contacts.size());
    index[fid] = snap.factions.size();
    snap.factions.push_back(std::move(fs));
  }

  // Count fleets.
  for (const auto& [_, fl] : state.fleets) {
    auto it = index.find(fl.faction_id);
    if (it == index.end()) continue;
    snap.factions[it->second].fleets += 1;
  }

  // Count ships and optionally cargo totals.
  for (const auto& [_, sh] : state.ships) {
    auto it = index.find(sh.faction_id);
    if (it == index.end()) continue;
    auto& fs = snap.factions[it->second];
    fs.ships += 1;
    if (opt.include_ship_cargo) {
      add_minerals(fs.ship_cargo, sh.cargo, opt);
    }
  }

  // Count colonies, population, minerals.
  for (const auto& [_, col] : state.colonies) {
    auto it = index.find(col.faction_id);
    if (it == index.end()) continue;
    auto& fs = snap.factions[it->second];
    fs.colonies += 1;
    fs.population_millions += col.population_millions;
    if (opt.include_minerals) {
      add_minerals(fs.minerals, col.minerals, opt);
    }
  }

  // Avoid unused parameter warning for future extensions.
  (void)content;

  return snap;
}

std::string timeline_snapshots_to_jsonl(const std::vector<TimelineSnapshot>& snaps) {
  std::string out;
  out.reserve(snaps.size() * 256);
  for (const auto& s : snaps) {
    out += json::stringify(snapshot_to_json(s), 0);
    out.push_back('\n');
  }
  return out;
}

} // namespace nebula4x
