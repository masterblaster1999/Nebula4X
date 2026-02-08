#include <iostream>
#include <string>
#include <vector>

#include "nebula4x/core/entities.h"
#include "nebula4x/core/game_state.h"
#include "nebula4x/util/event_export.h"
#include "nebula4x/util/json.h"

#define N4X_ASSERT(expr) \
  do { \
    if (!(expr)) { \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
      return 1; \
    } \
  } while (0)

int test_event_export() {
  nebula4x::GameState s;

  // Minimal entity tables for name resolution in exported rows.
  {
    nebula4x::Faction f;
    f.id = 1;
    f.name = "Terrans";
    s.factions[f.id] = f;
  }
  {
    nebula4x::StarSystem sys;
    sys.id = 10;
    sys.name = "Sol";
    s.systems[sys.id] = sys;
  }
  {
    nebula4x::Ship sh;
    sh.id = 42;
    sh.name = "SC-1";
    sh.faction_id = 1;
    sh.system_id = 10;
    s.ships[sh.id] = sh;
  }
  {
    nebula4x::Colony c;
    c.id = 7;
    c.name = "Earth";
    c.faction_id = 1;
    s.colonies[c.id] = c;
  }

  // Two events with known values.
  {
    nebula4x::SimEvent ev;
    ev.seq = 5;
    ev.day = 10; // 2200-01-11
    ev.hour = 6;
    ev.level = nebula4x::EventLevel::Warn;
    ev.category = nebula4x::EventCategory::Movement;
    ev.faction_id = 1;
    ev.system_id = 10;
    ev.ship_id = 42;
    ev.colony_id = 7;
    ev.message = "Test,comma";
    s.events.push_back(ev);
  }
  {
    nebula4x::SimEvent ev;
    ev.seq = 6;
    ev.day = 11; // 2200-01-12
    ev.hour = 18;
    ev.level = nebula4x::EventLevel::Info;
    ev.category = nebula4x::EventCategory::Research;
    ev.faction_id = 1;
    ev.message = "He said \"ok\"";
    s.events.push_back(ev);
  }
  {
    nebula4x::SimEvent ev;
    ev.seq = 7;
    ev.day = 11; // 2200-01-12
    ev.hour = 19;
    ev.level = nebula4x::EventLevel::Info;
    ev.category = nebula4x::EventCategory::Terraforming;
    ev.faction_id = 1;
    ev.colony_id = 7;
    ev.message = "CO2 scrubbers online";
    s.events.push_back(ev);
  }

  std::vector<const nebula4x::SimEvent*> events;
  events.reserve(s.events.size());
  for (const auto& ev : s.events) events.push_back(&ev);

  // CSV: header + date conversion + CSV escaping.
  const std::string csv = nebula4x::events_to_csv(s, events);
  N4X_ASSERT(csv.find("day,date,seq,level,category") != std::string::npos);
  N4X_ASSERT(csv.find("message,hour,time,datetime") != std::string::npos);
  N4X_ASSERT(csv.find("2200-01-11") != std::string::npos);
  N4X_ASSERT(csv.find("2200-01-11 06:00") != std::string::npos);
  N4X_ASSERT(csv.find("\"Test,comma\"") != std::string::npos);
  N4X_ASSERT(csv.find(R"("He said ""ok""")") != std::string::npos);
  N4X_ASSERT(csv.find("TERRAFORMING") != std::string::npos);
  N4X_ASSERT(csv.find("CO2 scrubbers online") != std::string::npos);

  // JSON: parseable and has expected fields.
  const std::string json_text = nebula4x::events_to_json(s, events);
  N4X_ASSERT(!json_text.empty() && json_text.back() == '\n');
  const auto root = nebula4x::json::parse(json_text);
  const auto* arr = root.as_array();
  N4X_ASSERT(arr != nullptr);
  N4X_ASSERT(arr->size() == 3);

  const auto* o0 = (*arr)[0].as_object();
  N4X_ASSERT(o0 != nullptr);
  N4X_ASSERT(o0->at("day").int_value() == 10);
  N4X_ASSERT(o0->at("date").string_value() == "2200-01-11");
  N4X_ASSERT(o0->at("hour").int_value() == 6);
  N4X_ASSERT(o0->at("time").string_value() == "06:00");
  N4X_ASSERT(o0->at("datetime").string_value() == "2200-01-11 06:00");
  N4X_ASSERT(o0->at("seq").int_value() == 5);
  N4X_ASSERT(o0->at("level").string_value() == "warn");
  N4X_ASSERT(o0->at("category").string_value() == "movement");
  N4X_ASSERT(o0->at("faction").string_value() == "Terrans");
  N4X_ASSERT(o0->at("system").string_value() == "Sol");
  N4X_ASSERT(o0->at("ship").string_value() == "SC-1");
  N4X_ASSERT(o0->at("colony").string_value() == "Earth");
  N4X_ASSERT(o0->at("message").string_value() == "Test,comma");

  const auto* o1 = (*arr)[1].as_object();
  N4X_ASSERT(o1 != nullptr);
  N4X_ASSERT(o1->at("hour").int_value() == 18);
  N4X_ASSERT(o1->at("datetime").string_value() == "2200-01-12 18:00");
  N4X_ASSERT(o1->at("message").string_value() == "He said \"ok\"");

  const auto* o2 = (*arr)[2].as_object();
  N4X_ASSERT(o2 != nullptr);
  N4X_ASSERT(o2->at("seq").int_value() == 7);
  N4X_ASSERT(o2->at("category").string_value() == "terraforming");
  N4X_ASSERT(o2->at("colony").string_value() == "Earth");
  N4X_ASSERT(o2->at("datetime").string_value() == "2200-01-12 19:00");
  N4X_ASSERT(o2->at("message").string_value() == "CO2 scrubbers online");

  // JSONL: one object per line, parseable per-line.
  const std::string jsonl_text = nebula4x::events_to_jsonl(s, events);
  N4X_ASSERT(!jsonl_text.empty() && jsonl_text.back() == '\n');
  std::vector<std::string> lines;
  {
    std::size_t start = 0;
    while (true) {
      const std::size_t nl = jsonl_text.find('\n', start);
      if (nl == std::string::npos) break;
      lines.push_back(jsonl_text.substr(start, nl - start));
      start = nl + 1;
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
  }
  N4X_ASSERT(lines.size() == 3);
  {
    const auto v = nebula4x::json::parse(lines[0]);
    const auto* o = v.as_object();
    N4X_ASSERT(o != nullptr);
    N4X_ASSERT(o->at("seq").int_value() == 5);
    N4X_ASSERT(o->at("message").string_value() == "Test,comma");
  }
  {
    const auto v = nebula4x::json::parse(lines[1]);
    const auto* o = v.as_object();
    N4X_ASSERT(o != nullptr);
    N4X_ASSERT(o->at("seq").int_value() == 6);
    N4X_ASSERT(o->at("message").string_value() == "He said \"ok\"");
  }
  {
    const auto v = nebula4x::json::parse(lines[2]);
    const auto* o = v.as_object();
    N4X_ASSERT(o != nullptr);
    N4X_ASSERT(o->at("seq").int_value() == 7);
    N4X_ASSERT(o->at("category").string_value() == "terraforming");
    N4X_ASSERT(o->at("message").string_value() == "CO2 scrubbers online");
  }

  // Summary JSON: parseable and has expected counts/range.
  const std::string summary_text = nebula4x::events_summary_to_json(events);
  N4X_ASSERT(!summary_text.empty() && summary_text.back() == '\n');
  const auto sum_root = nebula4x::json::parse(summary_text);
  const auto* sum = sum_root.as_object();
  N4X_ASSERT(sum != nullptr);
  N4X_ASSERT(sum->at("count").int_value() == 3);

  const auto* range = sum->at("range").as_object();
  N4X_ASSERT(range != nullptr);
  N4X_ASSERT(range->at("day_min").int_value() == 10);
  N4X_ASSERT(range->at("date_min").string_value() == "2200-01-11");
  N4X_ASSERT(range->at("day_max").int_value() == 11);
  N4X_ASSERT(range->at("date_max").string_value() == "2200-01-12");
  N4X_ASSERT(range->at("hour_min").int_value() == 6);
  N4X_ASSERT(range->at("hour_max").int_value() == 19);
  N4X_ASSERT(range->at("datetime_min").string_value() == "2200-01-11 06:00");
  N4X_ASSERT(range->at("datetime_max").string_value() == "2200-01-12 19:00");

  const auto* levels = sum->at("levels").as_object();
  N4X_ASSERT(levels != nullptr);
  N4X_ASSERT(levels->at("info").int_value() == 2);
  N4X_ASSERT(levels->at("warn").int_value() == 1);
  N4X_ASSERT(levels->at("error").int_value() == 0);

  const auto* cats = sum->at("categories").as_object();
  N4X_ASSERT(cats != nullptr);
  N4X_ASSERT(cats->at("movement").int_value() == 1);
  N4X_ASSERT(cats->at("research").int_value() == 1);
  N4X_ASSERT(cats->at("terraforming").int_value() == 1);

  // Summary CSV: header + expected counts/range.
  const std::string summary_csv = nebula4x::events_summary_to_csv(events);
  N4X_ASSERT(summary_csv.find("count,day_min,day_max,date_min,date_max,hour_min,hour_max,time_min,time_max,datetime_min,datetime_max") != std::string::npos);
  N4X_ASSERT(summary_csv.find(",terraforming") != std::string::npos);
  N4X_ASSERT(summary_csv.find("2200-01-11") != std::string::npos);
  N4X_ASSERT(summary_csv.find("2200-01-12") != std::string::npos);
  // count=3, info=2, warn=1, error=0
  N4X_ASSERT(summary_csv.find("3,10,11") != std::string::npos);
  N4X_ASSERT(summary_csv.find(",2,1,0,") != std::string::npos);

  return 0;
}
