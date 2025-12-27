#include "nebula4x/util/event_export.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

#include "nebula4x/core/date.h"
#include "nebula4x/util/json.h"
#include "nebula4x/util/strings.h"

namespace nebula4x {
namespace {

const char* event_level_csv_label(EventLevel l) {
  switch (l) {
    case EventLevel::Info: return "INFO";
    case EventLevel::Warn: return "WARN";
    case EventLevel::Error: return "ERROR";
  }
  return "INFO";
}

const char* event_category_csv_label(EventCategory c) {
  switch (c) {
    case EventCategory::General: return "GENERAL";
    case EventCategory::Research: return "RESEARCH";
    case EventCategory::Shipyard: return "SHIPYARD";
    case EventCategory::Construction: return "CONSTRUCTION";
    case EventCategory::Movement: return "MOVEMENT";
    case EventCategory::Combat: return "COMBAT";
    case EventCategory::Intel: return "INTEL";
    case EventCategory::Exploration: return "EXPLORATION";
  }
  return "GENERAL";
}

const char* event_level_json_label(EventLevel l) {
  switch (l) {
    case EventLevel::Info: return "info";
    case EventLevel::Warn: return "warn";
    case EventLevel::Error: return "error";
  }
  return "info";
}

const char* event_category_json_label(EventCategory c) {
  switch (c) {
    case EventCategory::General: return "general";
    case EventCategory::Research: return "research";
    case EventCategory::Shipyard: return "shipyard";
    case EventCategory::Construction: return "construction";
    case EventCategory::Movement: return "movement";
    case EventCategory::Combat: return "combat";
    case EventCategory::Intel: return "intel";
    case EventCategory::Exploration: return "exploration";
  }
  return "general";
}

std::string faction_name(const GameState& s, Id id) {
  if (id == kInvalidId) return {};
  const auto it = s.factions.find(id);
  return (it != s.factions.end()) ? it->second.name : std::string{};
}

std::string system_name(const GameState& s, Id id) {
  if (id == kInvalidId) return {};
  const auto it = s.systems.find(id);
  return (it != s.systems.end()) ? it->second.name : std::string{};
}

std::string ship_name(const GameState& s, Id id) {
  if (id == kInvalidId) return {};
  const auto it = s.ships.find(id);
  return (it != s.ships.end()) ? it->second.name : std::string{};
}

std::string colony_name(const GameState& s, Id id) {
  if (id == kInvalidId) return {};
  const auto it = s.colonies.find(id);
  return (it != s.colonies.end()) ? it->second.name : std::string{};
}

json::Object event_to_object(const GameState& state, const SimEvent& ev) {
  const Date d(ev.day);

  json::Object obj;
  obj["day"] = static_cast<double>(ev.day);
  obj["date"] = d.to_string();
  obj["seq"] = static_cast<double>(ev.seq);
  obj["level"] = std::string(event_level_json_label(ev.level));
  obj["category"] = std::string(event_category_json_label(ev.category));
  obj["faction_id"] = static_cast<double>(ev.faction_id);
  obj["faction"] = faction_name(state, ev.faction_id);
  obj["faction_id2"] = static_cast<double>(ev.faction_id2);
  obj["faction2"] = faction_name(state, ev.faction_id2);
  obj["system_id"] = static_cast<double>(ev.system_id);
  obj["system"] = system_name(state, ev.system_id);
  obj["ship_id"] = static_cast<double>(ev.ship_id);
  obj["ship"] = ship_name(state, ev.ship_id);
  obj["colony_id"] = static_cast<double>(ev.colony_id);
  obj["colony"] = colony_name(state, ev.colony_id);
  obj["message"] = ev.message;
  return obj;
}

} // namespace

std::string events_to_csv(const GameState& state, const std::vector<const SimEvent*>& events) {
  std::string csv;
  csv += "day,date,seq,level,category,"
         "faction_id,faction,"
         "faction_id2,faction2,"
         "system_id,system,"
         "ship_id,ship,"
         "colony_id,colony,"
         "message\n";

  // Reserve a small amount to avoid lots of reallocations for modest logs.
  csv.reserve(csv.size() + events.size() * 96);

  for (const auto* ev : events) {
    if (!ev) continue;
    const Date d(ev->day);

    csv += std::to_string(static_cast<long long>(ev->day));
    csv += ",";
    csv += csv_escape(d.to_string());
    csv += ",";
    csv += std::to_string(static_cast<unsigned long long>(ev->seq));
    csv += ",";
    csv += csv_escape(std::string(event_level_csv_label(ev->level)));
    csv += ",";
    csv += csv_escape(std::string(event_category_csv_label(ev->category)));
    csv += ",";
    csv += std::to_string(static_cast<unsigned long long>(ev->faction_id));
    csv += ",";
    csv += csv_escape(faction_name(state, ev->faction_id));
    csv += ",";
    csv += std::to_string(static_cast<unsigned long long>(ev->faction_id2));
    csv += ",";
    csv += csv_escape(faction_name(state, ev->faction_id2));
    csv += ",";
    csv += std::to_string(static_cast<unsigned long long>(ev->system_id));
    csv += ",";
    csv += csv_escape(system_name(state, ev->system_id));
    csv += ",";
    csv += std::to_string(static_cast<unsigned long long>(ev->ship_id));
    csv += ",";
    csv += csv_escape(ship_name(state, ev->ship_id));
    csv += ",";
    csv += std::to_string(static_cast<unsigned long long>(ev->colony_id));
    csv += ",";
    csv += csv_escape(colony_name(state, ev->colony_id));
    csv += ",";
    csv += csv_escape(ev->message);
    csv += "\n";
  }

  return csv;
}

std::string events_to_json(const GameState& state, const std::vector<const SimEvent*>& events) {
  json::Array out;
  out.reserve(events.size());

  for (const auto* ev : events) {
    if (!ev) continue;
    out.emplace_back(event_to_object(state, *ev));
  }

  std::string json_text = json::stringify(json::array(std::move(out)), 2);
  json_text += "\n";
  return json_text;
}

std::string events_to_jsonl(const GameState& state, const std::vector<const SimEvent*>& events) {
  std::string out;
  out.reserve(events.size() * 160);

  for (const auto* ev : events) {
    if (!ev) continue;
    const auto obj = json::object(event_to_object(state, *ev));
    out += json::stringify(obj, 0);
    out.push_back('\n');
  }

  if (out.empty()) out.push_back('\n');
  return out;
}



std::string events_summary_to_json(const std::vector<const SimEvent*>& events) {
  std::size_t count = 0;
  std::int64_t min_day = 0;
  std::int64_t max_day = 0;

  std::size_t info_count = 0;
  std::size_t warn_count = 0;
  std::size_t error_count = 0;

  std::array<std::size_t, 8> by_cat{};

  for (const auto* ev : events) {
    if (!ev) continue;
    ++count;

    if (count == 1) {
      min_day = ev->day;
      max_day = ev->day;
    } else {
      min_day = std::min(min_day, ev->day);
      max_day = std::max(max_day, ev->day);
    }

    if (ev->level == EventLevel::Info) ++info_count;
    if (ev->level == EventLevel::Warn) ++warn_count;
    if (ev->level == EventLevel::Error) ++error_count;

    const int idx = static_cast<int>(ev->category);
    if (idx >= 0 && idx < static_cast<int>(by_cat.size())) {
      ++by_cat[static_cast<std::size_t>(idx)];
    }
  }

  json::Object out;
  out["count"] = static_cast<double>(count);

  // Range info: include both day numbers and ISO dates for convenience.
  json::Object range;
  if (count == 0) {
    range["day_min"] = nullptr;
    range["day_max"] = nullptr;
    range["date_min"] = nullptr;
    range["date_max"] = nullptr;
  } else {
    range["day_min"] = static_cast<double>(min_day);
    range["day_max"] = static_cast<double>(max_day);
    range["date_min"] = Date(min_day).to_string();
    range["date_max"] = Date(max_day).to_string();
  }
  out["range"] = std::move(range);

  json::Object levels;
  levels["info"] = static_cast<double>(info_count);
  levels["warn"] = static_cast<double>(warn_count);
  levels["error"] = static_cast<double>(error_count);
  out["levels"] = std::move(levels);

  json::Object cats;
  cats["general"] = static_cast<double>(by_cat[static_cast<std::size_t>(EventCategory::General)]);
  cats["research"] = static_cast<double>(by_cat[static_cast<std::size_t>(EventCategory::Research)]);
  cats["shipyard"] = static_cast<double>(by_cat[static_cast<std::size_t>(EventCategory::Shipyard)]);
  cats["construction"] = static_cast<double>(by_cat[static_cast<std::size_t>(EventCategory::Construction)]);
  cats["movement"] = static_cast<double>(by_cat[static_cast<std::size_t>(EventCategory::Movement)]);
  cats["combat"] = static_cast<double>(by_cat[static_cast<std::size_t>(EventCategory::Combat)]);
  cats["intel"] = static_cast<double>(by_cat[static_cast<std::size_t>(EventCategory::Intel)]);
  cats["exploration"] = static_cast<double>(by_cat[static_cast<std::size_t>(EventCategory::Exploration)]);
  out["categories"] = std::move(cats);

  std::string json_text = json::stringify(json::object(std::move(out)), 2);
  json_text += "\n";
  return json_text;
}

} // namespace nebula4x
