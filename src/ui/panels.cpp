#include "ui/panels.h"

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <utility>

#include "nebula4x/core/serialization.h"
#include "nebula4x/util/file_io.h"
#include "nebula4x/util/log.h"
#include "nebula4x/util/strings.h"

namespace nebula4x::ui {
namespace {

bool case_insensitive_contains(const std::string& haystack, const char* needle_cstr) {
  if (!needle_cstr) return true;
  if (needle_cstr[0] == '\0') return true;
  const std::string needle(needle_cstr);
  const auto it = std::search(
      haystack.begin(), haystack.end(), needle.begin(), needle.end(),
      [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
      });
  return it != haystack.end();
}

const char* ship_role_label(ShipRole r) {
  switch (r) {
    case ShipRole::Freighter: return "Freighter";
    case ShipRole::Surveyor: return "Surveyor";
    case ShipRole::Combatant: return "Combatant";
    default: return "Unknown";
  }
}

const char* component_type_label(ComponentType t) {
  switch (t) {
    case ComponentType::Engine: return "Engine";
    case ComponentType::Cargo: return "Cargo";
    case ComponentType::Sensor: return "Sensor";
    case ComponentType::Reactor: return "Reactor";
    case ComponentType::Weapon: return "Weapon";
    case ComponentType::Armor: return "Armor";
    default: return "Unknown";
  }
}

const char* event_level_label(EventLevel l) {
  switch (l) {
    case EventLevel::Info: return "Info";
    case EventLevel::Warn: return "Warn";
    case EventLevel::Error: return "Error";
  }
  return "Info";
}

const char* event_category_label(EventCategory c) {
  switch (c) {
    case EventCategory::General: return "General";
    case EventCategory::Research: return "Research";
    case EventCategory::Shipyard: return "Shipyard";
    case EventCategory::Construction: return "Construction";
    case EventCategory::Movement: return "Movement";
    case EventCategory::Combat: return "Combat";
    case EventCategory::Intel: return "Intel";
    case EventCategory::Exploration: return "Exploration";
  }
  return "General";
}

std::vector<std::string> sorted_all_design_ids(const Simulation& sim) {
  std::vector<std::string> ids;
  ids.reserve(sim.content().designs.size() + sim.state().custom_designs.size());
  for (const auto& [id, _] : sim.content().designs) ids.push_back(id);
  for (const auto& [id, _] : sim.state().custom_designs) ids.push_back(id);
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

std::vector<std::string> sorted_buildable_design_ids(const Simulation& sim, Id faction_id) {
  auto ids = sorted_all_design_ids(sim);
  ids.erase(std::remove_if(ids.begin(), ids.end(), [&](const std::string& id) {
              return !sim.is_design_buildable_for_faction(faction_id, id);
            }),
            ids.end());
  return ids;
}

std::vector<std::pair<Id, std::string>> sorted_factions(const GameState& s) {
  std::vector<std::pair<Id, std::string>> out;
  out.reserve(s.factions.size());
  for (const auto& [id, f] : s.factions) out.push_back({id, f.name});
  std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.second < b.second; });
  return out;
}

bool vec_contains(const std::vector<std::string>& v, const std::string& x) {
  return std::find(v.begin(), v.end(), x) != v.end();
}

bool prereqs_met(const Faction& f, const TechDef& t) {
  for (const auto& p : t.prereqs) {
    if (!vec_contains(f.known_techs, p)) return false;
  }
  return true;
}

ShipDesign derive_preview_design(const ContentDB& c, ShipDesign d) {
  double mass = 0.0;
  double speed = 0.0;
  double cargo = 0.0;
  double sensor = 0.0;
  double weapon_damage = 0.0;
  double weapon_range = 0.0;
  double hp_bonus = 0.0;

  for (const auto& cid : d.components) {
    auto it = c.components.find(cid);
    if (it == c.components.end()) continue;
    const auto& comp = it->second;
    mass += comp.mass_tons;
    speed = std::max(speed, comp.speed_km_s);
    cargo += comp.cargo_tons;
    sensor = std::max(sensor, comp.sensor_range_mkm);
    if (comp.type == ComponentType::Weapon) {
      weapon_damage += comp.weapon_damage;
      weapon_range = std::max(weapon_range, comp.weapon_range_mkm);
    }
    hp_bonus += comp.hp_bonus;
  }

  d.mass_tons = mass;
  d.speed_km_s = speed;
  d.cargo_tons = cargo;
  d.sensor_range_mkm = sensor;
  d.weapon_damage = weapon_damage;
  d.weapon_range_mkm = weapon_range;
  d.max_hp = std::max(1.0, mass * 2.0 + hp_bonus);
  return d;
}

} // namespace

void draw_main_menu(Simulation& sim, char* save_path, char* load_path) {
  if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("Game")) {
      if (ImGui::MenuItem("New Game")) {
        sim.new_game();
      }

      ImGui::Separator();

      ImGui::TextDisabled("Save path");
      ImGui::InputText("##save_path", save_path, 256);
      if (ImGui::MenuItem("Save")) {
        try {
          write_text_file(save_path, serialize_game_to_json(sim.state()));
        } catch (const std::exception& e) {
          nebula4x::log::error(std::string("Save failed: ") + e.what());
        }
      }

      ImGui::Separator();

      ImGui::TextDisabled("Load path");
      ImGui::InputText("##load_path", load_path, 256);
      if (ImGui::MenuItem("Load")) {
        try {
          sim.load_game(deserialize_game_from_json(read_text_file(load_path)));
        } catch (const std::exception& e) {
          nebula4x::log::error(std::string("Load failed: ") + e.what());
        }
      }

      ImGui::EndMenu();
    }

    ImGui::Text("  Date: %s", sim.state().date.to_string().c_str());

    ImGui::EndMainMenuBar();
  }
}

void draw_left_sidebar(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony) {
  ImGui::Text("Turns");
  if (ImGui::Button("+1 day")) sim.advance_days(1);
  ImGui::SameLine();
  if (ImGui::Button("+5")) sim.advance_days(5);
  ImGui::SameLine();
  if (ImGui::Button("+30")) sim.advance_days(30);

  // --- Auto-run / time warp ---
  {
    static int max_days = 365;
    static bool stop_info = true;
    static bool stop_warn = true;
    static bool stop_error = true;
    static int category_idx = 0; // 0 = Any
    static Id faction_filter = kInvalidId;
    static char message_contains[128] = "";
    static std::string last_status;

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Auto-run (pause on event)", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::InputInt("Max days##autorun", &max_days);
      max_days = std::clamp(max_days, 1, 36500);

      ImGui::Checkbox("Info##autorun", &stop_info);
      ImGui::SameLine();
      ImGui::Checkbox("Warn##autorun", &stop_warn);
      ImGui::SameLine();
      ImGui::Checkbox("Error##autorun", &stop_error);

      // Category filter.
      {
        const char* cats[] = {
            "Any",
            "General",
            "Research",
            "Shipyard",
            "Construction",
            "Movement",
            "Combat",
            "Intel",
            "Exploration",
        };
        ImGui::Combo("Category##autorun", &category_idx, cats, IM_ARRAYSIZE(cats));
      }

      // Faction filter.
      {
        auto& s = sim.state();
        const auto factions = sorted_factions(s);
        const auto* sel = find_ptr(s.factions, faction_filter);
        const char* label = (faction_filter == kInvalidId) ? "Any" : (sel ? sel->name.c_str() : "(missing)");

        if (ImGui::BeginCombo("Faction##autorun", label)) {
          if (ImGui::Selectable("Any", faction_filter == kInvalidId)) faction_filter = kInvalidId;
          for (const auto& [fid, name] : factions) {
            if (ImGui::Selectable(name.c_str(), faction_filter == fid)) faction_filter = fid;
          }
          ImGui::EndCombo();
        }
      }

      ImGui::InputText("Message contains##autorun", message_contains, IM_ARRAYSIZE(message_contains));

      if (ImGui::Button("Run until event##autorun")) {
        EventStopCondition stop;
        stop.stop_on_info = stop_info;
        stop.stop_on_warn = stop_warn;
        stop.stop_on_error = stop_error;
        stop.filter_category = false;
        stop.category = EventCategory::General;
        stop.faction_id = faction_filter;
        stop.message_contains = message_contains;

        if (category_idx > 0) {
          static const EventCategory cat_vals[] = {
              EventCategory::General,
              EventCategory::Research,
              EventCategory::Shipyard,
              EventCategory::Construction,
              EventCategory::Movement,
              EventCategory::Combat,
              EventCategory::Intel,
              EventCategory::Exploration,
          };
          const int idx = category_idx - 1;
          if (idx >= 0 && idx < (int)IM_ARRAYSIZE(cat_vals)) {
            stop.filter_category = true;
            stop.category = cat_vals[idx];
          }
        }

        auto res = sim.advance_until_event(max_days, stop);

        if (res.hit) {
          // Jump UI context to the event payload when possible.
          auto& s = sim.state();
          if (res.event.system_id != kInvalidId) s.selected_system = res.event.system_id;
          if (res.event.colony_id != kInvalidId) selected_colony = res.event.colony_id;
          if (res.event.ship_id != kInvalidId) {
            if (find_ptr(s.ships, res.event.ship_id)) selected_ship = res.event.ship_id;
          }

          last_status = "Paused on event after " + std::to_string(res.days_advanced) + " day(s): " + res.event.message;
        } else {
          last_status = "No matching events in " + std::to_string(res.days_advanced) + " day(s).";
        }
      }

      if (!last_status.empty()) {
        ImGui::TextWrapped("%s", last_status.c_str());
      }
    }
  }

  ImGui::Separator();
  ImGui::Text("Systems");
  const Ship* viewer_ship_for_fow = (selected_ship != kInvalidId) ? find_ptr(sim.state().ships, selected_ship) : nullptr;
  const Id viewer_faction_id_for_fow = viewer_ship_for_fow ? viewer_ship_for_fow->faction_id : ui.viewer_faction_id;

  for (const auto& [id, sys] : sim.state().systems) {
    if (ui.fog_of_war && viewer_faction_id_for_fow != kInvalidId) {
      if (!sim.is_system_discovered_by_faction(viewer_faction_id_for_fow, id)) {
        continue;
      }
    }
    const bool sel = (sim.state().selected_system == id);
    if (ImGui::Selectable(sys.name.c_str(), sel)) {
      sim.state().selected_system = id;
      // If we have a selected ship that isn't in this system, deselect it.
      if (selected_ship != kInvalidId) {
        const auto* sh = find_ptr(sim.state().ships, selected_ship);
        if (!sh || sh->system_id != id) selected_ship = kInvalidId;
      }
    }
  }

  ImGui::Separator();
  ImGui::Text("Ships (in system)");

  const auto* sys = find_ptr(sim.state().systems, sim.state().selected_system);
  if (!sys) {
    ImGui::TextDisabled("No system selected");
    return;
  }

  const Ship* viewer_ship = (selected_ship != kInvalidId) ? find_ptr(sim.state().ships, selected_ship) : nullptr;
  const Id viewer_faction_id = viewer_ship ? viewer_ship->faction_id : ui.viewer_faction_id;

  if (ui.fog_of_war && viewer_faction_id != kInvalidId) {
    if (!sim.is_system_discovered_by_faction(viewer_faction_id, sim.state().selected_system)) {
      ImGui::TextDisabled("System not discovered by viewer faction");
      ImGui::TextDisabled("(Select a ship or faction in the Research tab to change view)");
      return;
    }
  }

  for (Id sid : sys->ships) {
    const auto* sh = find_ptr(sim.state().ships, sid);
    if (!sh) continue;

    // Fog-of-war: only show friendly ships and detected hostiles, based on the selected ship's faction.
    if (ui.fog_of_war && viewer_faction_id != kInvalidId) {
      if (sh->faction_id != viewer_faction_id && !sim.is_ship_detected_by_faction(viewer_faction_id, sid)) {
        continue;
      }
    }

    const auto* fac = find_ptr(sim.state().factions, sh->faction_id);
    const std::string fac_name = fac ? fac->name : std::string("Faction ") + std::to_string(sh->faction_id);

    char label[256];
    std::snprintf(label, sizeof(label), "%s  (HP %.0f)  [%s]##%llu", sh->name.c_str(), sh->hp, fac_name.c_str(),
                  static_cast<unsigned long long>(sh->id));

    if (ImGui::Selectable(label, selected_ship == sid)) {
      selected_ship = sid;
    }
  }

  ImGui::Separator();
  ImGui::Text("Jump Points");
  if (sys->jump_points.empty()) {
    ImGui::TextDisabled("(none)");
  } else {
    for (Id jid : sys->jump_points) {
      const auto* jp = find_ptr(sim.state().jump_points, jid);
      if (!jp) continue;
      const auto* dest = find_ptr(sim.state().jump_points, jp->linked_jump_id);
      const auto* dest_sys = dest ? find_ptr(sim.state().systems, dest->system_id) : nullptr;

      const char* dest_label = "(unknown)";
      if (dest_sys) {
        // Fog-of-war: don't leak destination system names unless discovered.
        if (!ui.fog_of_war || viewer_faction_id_for_fow == kInvalidId ||
            sim.is_system_discovered_by_faction(viewer_faction_id_for_fow, dest_sys->id)) {
          dest_label = dest_sys->name.c_str();
        }
      }

      ImGui::BulletText("%s -> %s", jp->name.c_str(), dest_label);
    }
  }

  ImGui::Separator();
  ImGui::Text("Colonies");
  for (const auto& [cid, c] : sim.state().colonies) {
    std::string label = c.name + "##" + std::to_string(cid);
    if (ImGui::Selectable(label.c_str(), selected_colony == cid)) {
      selected_colony = cid;
    }
  }
}

void draw_right_sidebar(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony) {
  auto& s = sim.state();

  static int faction_combo_idx = 0;
  const auto factions = sorted_factions(s);
  if (!factions.empty()) {
    faction_combo_idx = std::clamp(faction_combo_idx, 0, static_cast<int>(factions.size()) - 1);
  }
  const Id selected_faction_id = factions.empty() ? kInvalidId : factions[faction_combo_idx].first;
  Faction* selected_faction = factions.empty() ? nullptr : find_ptr(s.factions, selected_faction_id);

  // Share the currently selected faction with other panels for fog-of-war/exploration view.
  ui.viewer_faction_id = selected_faction_id;

  if (ImGui::BeginTabBar("details_tabs")) {
    // --- Ship tab ---
    if (ImGui::BeginTabItem("Ship")) {
      if (selected_ship == kInvalidId) {
        ImGui::TextDisabled("No ship selected");
        ImGui::EndTabItem();
      } else if (auto* sh = find_ptr(s.ships, selected_ship)) {
        const auto* sys = find_ptr(s.systems, sh->system_id);
        const auto* fac = find_ptr(s.factions, sh->faction_id);
        const auto* d = sim.find_design(sh->design_id);

        ImGui::Text("%s", sh->name.c_str());
        ImGui::Separator();
        ImGui::Text("Faction: %s", fac ? fac->name.c_str() : "(unknown)");
        ImGui::Text("System: %s", sys ? sys->name.c_str() : "(unknown)");
        ImGui::Text("Pos: (%.2f, %.2f) mkm", sh->position_mkm.x, sh->position_mkm.y);
        ImGui::Text("Speed: %.1f km/s", sh->speed_km_s);

        double cargo_used_tons = 0.0;
        for (const auto& [_, t] : sh->cargo) cargo_used_tons += std::max(0.0, t);

        if (d) {
          ImGui::Text("Design: %s (%s)", d->name.c_str(), ship_role_label(d->role));
          ImGui::Text("Mass: %.0f t", d->mass_tons);
          ImGui::Text("HP: %.0f / %.0f", sh->hp, d->max_hp);
          ImGui::Text("Cargo: %.0f / %.0f t", cargo_used_tons, d->cargo_tons);
          ImGui::Text("Sensor: %.0f mkm", d->sensor_range_mkm);
          if (d->weapon_damage > 0.0) {
            ImGui::Text("Weapons: %.1f dmg/day  (Range %.1f mkm)", d->weapon_damage, d->weapon_range_mkm);
          } else {
            ImGui::TextDisabled("Weapons: (none)");
          }
        } else {
          ImGui::TextDisabled("Design definition missing: %s", sh->design_id.c_str());
        }

        ImGui::Separator();
        ImGui::Text("Orders");
        auto oit = s.ship_orders.find(selected_ship);
        if (oit == s.ship_orders.end() || oit->second.queue.empty()) {
          ImGui::TextDisabled("(none)");
        } else {
          int idx = 0;
          for (const auto& o : oit->second.queue) {
            ImGui::BulletText("%d) %s", idx++, order_to_string(o).c_str());
          }
        }

        const bool repeat_on = (oit != s.ship_orders.end()) ? oit->second.repeat : false;
        const int repeat_len = (oit != s.ship_orders.end()) ? static_cast<int>(oit->second.repeat_template.size()) : 0;
        if (repeat_on) {
          ImGui::Text("Repeat: ON  (template %d orders)", repeat_len);
        } else {
          ImGui::Text("Repeat: OFF");
        }

        ImGui::Spacing();
        if (!repeat_on) {
          if (ImGui::SmallButton("Enable repeat")) {
            if (!sim.enable_order_repeat(selected_ship)) {
              nebula4x::log::warn("Couldn't enable repeat (queue empty?).");
            }
          }
        } else {
          if (ImGui::SmallButton("Update repeat template")) {
            if (!sim.update_order_repeat_template(selected_ship)) {
              nebula4x::log::warn("Couldn't update repeat template (queue empty?).");
            }
          }
          ImGui::SameLine();
          if (ImGui::SmallButton("Disable repeat")) {
            sim.disable_order_repeat(selected_ship);
          }
        }

        ImGui::Spacing();
        if (ImGui::SmallButton("Cancel current")) {
          sim.cancel_current_order(selected_ship);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear orders")) {
          sim.clear_orders(selected_ship);
        }

        ImGui::Separator();
        ImGui::Text("Cargo detail");
        if (d) {
          ImGui::Text("Used: %.0f / %.0f t", cargo_used_tons, d->cargo_tons);
        } else {
          ImGui::Text("Used: %.0f t", cargo_used_tons);
        }

        if (sh->cargo.empty()) {
          ImGui::TextDisabled("(empty)");
        } else {
          // Show carried minerals (sorted for readability).
          std::vector<std::pair<std::string, double>> cargo_list;
          cargo_list.reserve(sh->cargo.size());
          for (const auto& [k, v] : sh->cargo) cargo_list.emplace_back(k, v);
          std::sort(cargo_list.begin(), cargo_list.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

          for (const auto& [k, v] : cargo_list) {
            ImGui::BulletText("%s: %.1f t", k.c_str(), v);
          }
        }

        ImGui::Spacing();
        ImGui::Text("Transfer with selected colony");
        ImGui::TextDisabled("Load/unload is an order: the ship will move to the colony body, then transfer in one day.");

        const Colony* sel_col = (selected_colony != kInvalidId) ? find_ptr(s.colonies, selected_colony) : nullptr;
        const Body* sel_col_body = sel_col ? find_ptr(s.bodies, sel_col->body_id) : nullptr;

        if (!sel_col) {
          ImGui::TextDisabled("No colony selected.");
        } else if (!sel_col_body) {
          ImGui::TextDisabled("Selected colony body missing.");
        } else if (sel_col->faction_id != sh->faction_id) {
          ImGui::TextDisabled("Selected colony is not friendly.");
        } else {
          ImGui::Text("Colony: %s", sel_col->name.c_str());

          if (sel_col_body->system_id != sh->system_id) {
            std::string dest_label = "(unknown)";
            const auto* dest_sys = find_ptr(s.systems, sel_col_body->system_id);
            if (dest_sys && (!ui.fog_of_war || sim.is_system_discovered_by_faction(sh->faction_id, dest_sys->id))) {
              dest_label = dest_sys->name;
            }
            ImGui::TextDisabled("Colony is in a different system %s. Order will auto-route via jump points.",
                                dest_label.c_str());
          }

          // Build a stable mineral list (union of colony minerals + ship cargo).
          std::vector<std::string> minerals;
          minerals.reserve(sel_col->minerals.size() + sh->cargo.size());
          for (const auto& [k, _] : sel_col->minerals) minerals.push_back(k);
          for (const auto& [k, _] : sh->cargo) minerals.push_back(k);
          std::sort(minerals.begin(), minerals.end());
          minerals.erase(std::unique(minerals.begin(), minerals.end()), minerals.end());

          static int mineral_idx = 0;
          static double transfer_tons = 0.0;

          // Clamp idx when list changes.
          const int max_idx = static_cast<int>(minerals.size()); // + 1 for "All"
          mineral_idx = std::max(0, std::min(mineral_idx, max_idx));

          const std::string current_label = (mineral_idx == 0) ? std::string("All minerals") : minerals[mineral_idx - 1];

          if (ImGui::BeginCombo("Mineral", current_label.c_str())) {
            if (ImGui::Selectable("All minerals", mineral_idx == 0)) mineral_idx = 0;
            for (int i = 0; i < static_cast<int>(minerals.size()); ++i) {
              const bool selected = (mineral_idx == i + 1);
              if (ImGui::Selectable(minerals[i].c_str(), selected)) mineral_idx = i + 1;
            }
            ImGui::EndCombo();
          }

          ImGui::InputDouble("Tons (0 = as much as possible)", &transfer_tons, 10.0, 100.0, "%.1f");

          const std::string mineral_id = (mineral_idx == 0) ? std::string() : minerals[mineral_idx - 1];

          if (ImGui::Button("Load##cargo")) {
            if (!sim.issue_load_mineral(selected_ship, selected_colony, mineral_id, transfer_tons, ui.fog_of_war)) {
              nebula4x::log::warn("Couldn't queue load order (no known route?).");
            }
          }
          ImGui::SameLine();
          if (ImGui::Button("Unload##cargo")) {
            if (!sim.issue_unload_mineral(selected_ship, selected_colony, mineral_id, transfer_tons, ui.fog_of_war)) {
              nebula4x::log::warn("Couldn't queue unload order (no known route?).");
            }
          }
        }


        ImGui::Separator();
        ImGui::Text("Quick orders");

        // Simple scheduling primitive.
        static int wait_days = 1;
        wait_days = std::clamp(wait_days, 1, 365000); // ~1000 years, just a safety cap.
        ImGui::InputInt("Wait (days)", &wait_days);
        if (ImGui::Button("Queue wait")) {
          sim.issue_wait_days(selected_ship, wait_days);
        }

        if (ImGui::Button("Move to (0,0)")) {
          sim.issue_move_to_point(selected_ship, {0.0, 0.0});
        }
        if (ImGui::Button("Move to Earth")) {
          const auto* sys2 = find_ptr(s.systems, sh->system_id);
          if (sys2) {
            for (Id bid : sys2->bodies) {
              const auto* b = find_ptr(s.bodies, bid);
              if (b && b->name == "Earth") {
                if (!sim.issue_move_to_body(selected_ship, b->id, ui.fog_of_war)) {
                  nebula4x::log::warn("Couldn't issue move-to-body order.");
                }
                break;
              }
            }
          }
        }

        // Jump point travel
        const auto* sh_sys = find_ptr(s.systems, sh->system_id);
        if (sh_sys && !sh_sys->jump_points.empty()) {
          ImGui::Spacing();
          ImGui::Text("Jump travel");
          for (Id jid : sh_sys->jump_points) {
            const auto* jp = find_ptr(s.jump_points, jid);
            if (!jp) continue;
            const auto* dest = find_ptr(s.jump_points, jp->linked_jump_id);
            const auto* dest_sys = dest ? find_ptr(s.systems, dest->system_id) : nullptr;

            std::string btn = "Travel via " + jp->name;
            if (dest_sys) {
              // Fog-of-war: hide destination names until the system is discovered by this ship's faction.
              if (!ui.fog_of_war || sim.is_system_discovered_by_faction(sh->faction_id, dest_sys->id)) {
                btn += " -> " + dest_sys->name;
              } else {
                btn += " -> (unknown)";
              }
            }

            if (ImGui::Button((btn + "##" + std::to_string(jid)).c_str())) {
              sim.issue_travel_via_jump(selected_ship, jid);
            }
          }
        }

        // Combat: list hostiles in this system
        if (sh_sys) {
          std::vector<Id> hostiles =
              sim.detected_hostile_ships_in_system(sh->faction_id, sh->system_id);

          ImGui::Spacing();
          ImGui::Text("Combat");
          if (hostiles.empty()) {
            ImGui::TextDisabled("No detected hostiles in system");
          } else {
            ImGui::TextDisabled("Ships with weapons auto-fire once/day if in range.");
            for (Id hid : hostiles) {
              const auto* other = find_ptr(s.ships, hid);
              if (!other) continue;
              const auto* od = sim.find_design(other->design_id);
              const double range = d ? d->weapon_range_mkm : 0.0;
              const double dist = (other->position_mkm - sh->position_mkm).length();

              std::string label = other->name;
              label += " (HP " + std::to_string(static_cast<int>(other->hp)) + ")";
              if (od && od->weapon_damage > 0.0) label += " [armed]";

              ImGui::BulletText("%s  dist %.2f mkm", label.c_str(), dist);
              if (range > 0.0) {
                ImGui::SameLine();
                if (ImGui::SmallButton(("Attack##" + std::to_string(hid)).c_str())) {
                  sim.issue_attack_ship(sh->id, hid, ui.fog_of_war);
                }
              }
            }
          }
        }

        ImGui::EndTabItem();
      } else {
        ImGui::TextDisabled("Selected ship no longer exists");
        ImGui::EndTabItem();
      }
    }

    // --- Colony tab ---
    if (ImGui::BeginTabItem("Colony")) {
      if (selected_colony == kInvalidId) {
        ImGui::TextDisabled("No colony selected");
        ImGui::EndTabItem();
      } else if (auto* colony = find_ptr(s.colonies, selected_colony)) {
        ImGui::Text("%s", colony->name.c_str());
        ImGui::Separator();
        ImGui::Text("Population: %.0f M", colony->population_millions);

        ImGui::Separator();
        ImGui::Text("Minerals");
        for (const auto& [k, v] : colony->minerals) {
          ImGui::BulletText("%s: %.1f", k.c_str(), v);
        }

        ImGui::Separator();
        ImGui::Text("Installations");
        for (const auto& [k, v] : colony->installations) {
          const auto it = sim.content().installations.find(k);
          const std::string nm = (it == sim.content().installations.end()) ? k : it->second.name;
          if (it != sim.content().installations.end() && it->second.sensor_range_mkm > 0.0) {
            ImGui::BulletText("%s: %d  (Sensor %.0f mkm)", nm.c_str(), v, it->second.sensor_range_mkm);
          } else {
            ImGui::BulletText("%s: %d", nm.c_str(), v);
          }
        }

        ImGui::Separator();
        ImGui::Text("Construction");
        const double cp_per_day = sim.construction_points_per_day(*colony);
        ImGui::Text("Construction Points/day: %.1f", cp_per_day);

        if (colony->construction_queue.empty()) {
          ImGui::TextDisabled("Queue empty");
        } else {
          for (const auto& ord : colony->construction_queue) {
            const auto it = sim.content().installations.find(ord.installation_id);
            const InstallationDef* def = (it == sim.content().installations.end()) ? nullptr : &it->second;
            const std::string nm = def ? def->name : ord.installation_id;

            ImGui::BulletText("%s x%d", nm.c_str(), ord.quantity_remaining);

            // Status line / progress bar
            if (ord.minerals_paid && def && def->construction_cost > 0.0) {
              const double done = def->construction_cost - ord.cp_remaining;
              const float frac = static_cast<float>(std::clamp(done / def->construction_cost, 0.0, 1.0));
              ImGui::Indent();
              ImGui::ProgressBar(frac, ImVec2(-1, 0),
                                 (std::to_string(static_cast<int>(done)) + " / " +
                                  std::to_string(static_cast<int>(def->construction_cost)) + " CP")
                                     .c_str());
              ImGui::Unindent();
            } else if (!ord.minerals_paid && def && !def->build_costs.empty()) {
              // Hint if we can't currently start due to minerals.
              std::string missing;
              for (const auto& [mineral, cost] : def->build_costs) {
                if (cost <= 0.0) continue;
                const auto it2 = colony->minerals.find(mineral);
                const double have = (it2 == colony->minerals.end()) ? 0.0 : it2->second;
                if (have + 1e-9 < cost) {
                  missing = mineral;
                  break;
                }
              }
              if (!missing.empty()) {
                ImGui::Indent();
                ImGui::TextDisabled("Status: STALLED (need %s)", missing.c_str());
                ImGui::Unindent();
              }
            }
          }
        }

        // Enqueue new construction
        const auto* fac_for_colony = find_ptr(s.factions, colony->faction_id);
        static int inst_sel = 0;
        static int inst_qty = 1;
        static std::string inst_status;

        std::vector<std::string> buildable_installations;
        if (fac_for_colony) {
          for (const auto& id : fac_for_colony->unlocked_installations) {
            if (!sim.is_installation_buildable_for_faction(fac_for_colony->id, id)) continue;
            buildable_installations.push_back(id);
          }
        } else {
          for (const auto& [id, _] : sim.content().installations) buildable_installations.push_back(id);
        }
        std::sort(buildable_installations.begin(), buildable_installations.end());

        if (buildable_installations.empty()) {
          ImGui::TextDisabled("No buildable installations unlocked");
        } else {
          inst_sel = std::clamp(inst_sel, 0, static_cast<int>(buildable_installations.size()) - 1);

          // Build labels
          std::vector<std::string> label_storage;
          std::vector<const char*> labels;
          label_storage.reserve(buildable_installations.size());
          labels.reserve(buildable_installations.size());

          for (const auto& id : buildable_installations) {
            const auto it = sim.content().installations.find(id);
            const std::string nm = (it == sim.content().installations.end()) ? id : it->second.name;
            label_storage.push_back(nm + "##" + id);
          }
          for (const auto& s2 : label_storage) labels.push_back(s2.c_str());

          ImGui::Combo("Installation", &inst_sel, labels.data(), static_cast<int>(labels.size()));
          ImGui::InputInt("Qty", &inst_qty);
          inst_qty = std::clamp(inst_qty, 1, 100);

          const std::string chosen_id = buildable_installations[inst_sel];
          if (const auto it = sim.content().installations.find(chosen_id); it != sim.content().installations.end()) {
            const InstallationDef& def = it->second;
            ImGui::Text("Cost: %.0f CP", def.construction_cost);
            if (!def.build_costs.empty()) {
              ImGui::Text("Mineral costs:");
              for (const auto& [mineral, cost] : def.build_costs) {
                ImGui::BulletText("%s: %.0f", mineral.c_str(), cost);
              }
            }
          }

          if (ImGui::Button("Enqueue construction")) {
            if (sim.enqueue_installation_build(colony->id, chosen_id, inst_qty)) {
              inst_status = "Enqueued: " + chosen_id + " x" + std::to_string(inst_qty);
            } else {
              inst_status = "Failed to enqueue (locked or invalid)";
            }
          }
          if (!inst_status.empty()) {
            ImGui::TextDisabled("%s", inst_status.c_str());
          }
        }

        ImGui::Separator();
        ImGui::Text("Shipyard");

        const InstallationDef* shipyard_def = nullptr;
        if (const auto it = sim.content().installations.find("shipyard"); it != sim.content().installations.end()) {
          shipyard_def = &it->second;
        }

        const auto it_yard = colony->installations.find("shipyard");
        const bool has_yard = (it_yard != colony->installations.end() && it_yard->second > 0);
        if (!has_yard) {
          ImGui::TextDisabled("No shipyard present");
          ImGui::EndTabItem();
        } else {
          if (shipyard_def && !shipyard_def->build_costs_per_ton.empty()) {
            ImGui::Text("Build costs (per ton)");
            for (const auto& [mineral, cost_per_ton] : shipyard_def->build_costs_per_ton) {
              ImGui::BulletText("%s: %.2f", mineral.c_str(), cost_per_ton);
            }
            ImGui::Spacing();
          } else {
            ImGui::TextDisabled("Build costs: (free / not configured)");
          }

          if (colony->shipyard_queue.empty()) {
            ImGui::TextDisabled("Queue empty");
          } else {
            for (const auto& bo : colony->shipyard_queue) {
              const auto* d = sim.find_design(bo.design_id);
              const std::string nm = d ? d->name : bo.design_id;
              ImGui::BulletText("%s (%.1f tons remaining)", nm.c_str(), bo.tons_remaining);

              if (shipyard_def && !shipyard_def->build_costs_per_ton.empty()) {
                // Remaining mineral costs for this order.
                std::string cost_line;
                for (const auto& [mineral, cost_per_ton] : shipyard_def->build_costs_per_ton) {
                  if (cost_per_ton <= 0.0) continue;
                  const double remaining = bo.tons_remaining * cost_per_ton;
                  if (!cost_line.empty()) cost_line += ", ";
                  char buf[64];
                  std::snprintf(buf, sizeof(buf), "%.1f", remaining);
                  cost_line += mineral + " " + buf;
                }

                if (!cost_line.empty()) {
                  ImGui::Indent();
                  ImGui::TextDisabled("Remaining cost: %s", cost_line.c_str());

                  // Simple stall hint: if any required mineral is at 0, the build cannot progress.
                  std::string missing;
                  for (const auto& [mineral, cost_per_ton] : shipyard_def->build_costs_per_ton) {
                    if (cost_per_ton <= 0.0) continue;
                    const auto it = colony->minerals.find(mineral);
                    const double have = (it == colony->minerals.end()) ? 0.0 : it->second;
                    if (have <= 1e-9) {
                      missing = mineral;
                      break;
                    }
                  }
                  if (!missing.empty()) {
                    ImGui::TextDisabled("Status: STALLED (need %s)", missing.c_str());
                  }
                  ImGui::Unindent();
                }
              }
            }
          }

          static int selected_design_idx = 0;
          const auto ids = sorted_buildable_design_ids(sim, colony->faction_id);
          if (!ids.empty()) {
            selected_design_idx = std::clamp(selected_design_idx, 0, static_cast<int>(ids.size()) - 1);
            std::vector<const char*> labels;
            labels.reserve(ids.size());
            for (const auto& id : ids) labels.push_back(id.c_str());
            ImGui::Combo("Design", &selected_design_idx, labels.data(), static_cast<int>(labels.size()));
            if (ImGui::Button("Enqueue build")) {
              sim.enqueue_build(colony->id, ids[selected_design_idx]);
            }
          }

          ImGui::EndTabItem();
        }
      } else {
        ImGui::TextDisabled("Selected colony no longer exists");
        ImGui::EndTabItem();
      }
    }

    // --- Research tab ---
    if (ImGui::BeginTabItem("Research")) {
      if (factions.empty() || !selected_faction) {
        ImGui::TextDisabled("No factions available");
        ImGui::EndTabItem();
      } else {
        ImGui::Text("Faction");
        std::vector<const char*> fac_labels;
        fac_labels.reserve(factions.size());
        for (const auto& p : factions) fac_labels.push_back(p.second.c_str());
        ImGui::Combo("##faction", &faction_combo_idx, fac_labels.data(), static_cast<int>(fac_labels.size()));

        ImGui::Separator();
        ImGui::Text("Research Points (bank): %.1f", selected_faction->research_points);

        // Active
        if (!selected_faction->active_research_id.empty()) {
          const auto it = sim.content().techs.find(selected_faction->active_research_id);
          const TechDef* tech = (it == sim.content().techs.end()) ? nullptr : &it->second;
          const double cost = tech ? tech->cost : 0.0;
          ImGui::Text("Active: %s", tech ? tech->name.c_str() : selected_faction->active_research_id.c_str());
          if (cost > 0.0) {
            const float frac = static_cast<float>(std::clamp(selected_faction->active_research_progress / cost, 0.0, 1.0));
            ImGui::ProgressBar(frac, ImVec2(-1, 0), (std::to_string(static_cast<int>(selected_faction->active_research_progress)) +
                                                    " / " + std::to_string(static_cast<int>(cost)))
                                                       .c_str());
          }
        } else {
          ImGui::TextDisabled("Active: (none)");
        }

        ImGui::Separator();
        ImGui::Text("Queue");
        if (selected_faction->research_queue.empty()) {
          ImGui::TextDisabled("(empty)");
        } else {
          for (size_t i = 0; i < selected_faction->research_queue.size(); ++i) {
            const auto& id = selected_faction->research_queue[i];
            const auto it = sim.content().techs.find(id);
            const char* nm = (it == sim.content().techs.end()) ? id.c_str() : it->second.name.c_str();
            ImGui::BulletText("%s", nm);
          }
        }

        ImGui::Separator();
        ImGui::Text("Available techs");

        // Compute available list.
        std::vector<std::string> available;
        for (const auto& [tid, tech] : sim.content().techs) {
          if (vec_contains(selected_faction->known_techs, tid)) continue;
          if (!prereqs_met(*selected_faction, tech)) continue;
          available.push_back(tid);
        }
        std::sort(available.begin(), available.end());

        static int tech_sel = 0;
        if (!available.empty()) tech_sel = std::clamp(tech_sel, 0, static_cast<int>(available.size()) - 1);

        if (available.empty()) {
          ImGui::TextDisabled("(none)");
        } else {
          // List box
          if (ImGui::BeginListBox("##techs", ImVec2(-1, 180))) {
            for (int i = 0; i < static_cast<int>(available.size()); ++i) {
              const bool sel = (tech_sel == i);
              const auto it = sim.content().techs.find(available[i]);
              const std::string label = (it == sim.content().techs.end()) ? available[i] : (it->second.name + "##" + available[i]);
              if (ImGui::Selectable(label.c_str(), sel)) tech_sel = i;
            }
            ImGui::EndListBox();
          }

          const std::string chosen_id = available[tech_sel];
          const auto it = sim.content().techs.find(chosen_id);
          const TechDef* chosen = (it == sim.content().techs.end()) ? nullptr : &it->second;

          if (chosen) {
            ImGui::Text("Cost: %.0f", chosen->cost);
            if (!chosen->effects.empty()) {
              ImGui::Text("Effects:");
              for (const auto& eff : chosen->effects) {
                ImGui::BulletText("%s: %s", eff.type.c_str(), eff.value.c_str());
              }
            }
          }

          if (ImGui::Button("Set Active")) {
            selected_faction->active_research_id = chosen_id;
            selected_faction->active_research_progress = 0.0;
          }
          ImGui::SameLine();
          if (ImGui::Button("Add to Queue")) {
            selected_faction->research_queue.push_back(chosen_id);
          }
        }

        ImGui::EndTabItem();
      }
    }

    // --- Ship design tab ---
    if (ImGui::BeginTabItem("Design")) {
      if (factions.empty() || !selected_faction) {
        ImGui::TextDisabled("No factions available");
        ImGui::EndTabItem();
      } else {
        ImGui::Text("Design for faction");
        std::vector<const char*> fac_labels;
        fac_labels.reserve(factions.size());
        for (const auto& p : factions) fac_labels.push_back(p.second.c_str());
        ImGui::Combo("##faction_design", &faction_combo_idx, fac_labels.data(), static_cast<int>(fac_labels.size()));

        ImGui::Separator();
        ImGui::Text("Existing designs");
        const auto all_ids = sorted_all_design_ids(sim);

        static int design_sel = 0;
        if (!all_ids.empty()) design_sel = std::clamp(design_sel, 0, static_cast<int>(all_ids.size()) - 1);

        if (ImGui::BeginListBox("##designs", ImVec2(-1, 160))) {
          for (int i = 0; i < static_cast<int>(all_ids.size()); ++i) {
            const bool sel = (design_sel == i);
            const auto* d = sim.find_design(all_ids[i]);
            const std::string label = d ? (d->name + "##" + all_ids[i]) : all_ids[i];
            if (ImGui::Selectable(label.c_str(), sel)) design_sel = i;
          }
          ImGui::EndListBox();
        }

        if (!all_ids.empty()) {
          const auto* d = sim.find_design(all_ids[design_sel]);
          if (d) {
            ImGui::Text("ID: %s", d->id.c_str());
            ImGui::Text("Role: %s", ship_role_label(d->role));
            ImGui::Text("Mass: %.0f t", d->mass_tons);
            ImGui::Text("Speed: %.1f km/s", d->speed_km_s);
            ImGui::Text("HP: %.0f", d->max_hp);
            ImGui::Text("Cargo: %.0f / %.0f t", cargo_used_tons, d->cargo_tons);
            ImGui::Text("Sensor: %.0f mkm", d->sensor_range_mkm);
            if (d->weapon_damage > 0.0) ImGui::Text("Weapons: %.1f (range %.1f)", d->weapon_damage, d->weapon_range_mkm);
          }
        }

        ImGui::Separator();
        ImGui::Text("Create / edit custom design");

        static char new_id[64] = "";
        static char new_name[64] = "";
        static int role_idx = 0;
        static std::vector<std::string> comp_list;
        static std::string status;

        const char* roles[] = {"Freighter", "Surveyor", "Combatant"};
        role_idx = std::clamp(role_idx, 0, 2);

        ImGui::InputText("Design ID", new_id, sizeof(new_id));
        ImGui::InputText("Name", new_name, sizeof(new_name));
        ImGui::Combo("Role", &role_idx, roles, IM_ARRAYSIZE(roles));

        ImGui::Spacing();
        ImGui::Text("Components");

        // Show current components with remove buttons.
        for (size_t i = 0; i < comp_list.size(); ++i) {
          const auto& cid = comp_list[i];
          const auto it = sim.content().components.find(cid);
          const char* cname = (it == sim.content().components.end()) ? cid.c_str() : it->second.name.c_str();
          ImGui::BulletText("%s", cname);
          ImGui::SameLine();
          if (ImGui::SmallButton(("Remove##" + std::to_string(i)).c_str())) {
            comp_list.erase(comp_list.begin() + static_cast<long>(i));
            --i;
            continue;
          }
        }

        // Available components (unlocked)
        ImGui::Spacing();
        ImGui::Text("Add component");

        static int comp_filter = 0; // 0=All
        const char* filters[] = {"All", "Engine", "Cargo", "Sensor", "Reactor", "Weapon", "Armor"};
        ImGui::Combo("Filter", &comp_filter, filters, IM_ARRAYSIZE(filters));

        std::vector<std::string> avail_components;
        for (const auto& [cid, cdef] : sim.content().components) {
          // Only show unlocked for this faction (unless it's already in the design).
          const bool unlocked = vec_contains(selected_faction->unlocked_components, cid);
          if (!unlocked) continue;

          if (comp_filter != 0) {
            const ComponentType desired =
                (comp_filter == 1) ? ComponentType::Engine
                : (comp_filter == 2) ? ComponentType::Cargo
                : (comp_filter == 3) ? ComponentType::Sensor
                : (comp_filter == 4) ? ComponentType::Reactor
                : (comp_filter == 5) ? ComponentType::Weapon
                : (comp_filter == 6) ? ComponentType::Armor
                                    : ComponentType::Unknown;
            if (cdef.type != desired) continue;
          }
          avail_components.push_back(cid);
        }
        std::sort(avail_components.begin(), avail_components.end());

        static int add_comp_idx = 0;
        if (!avail_components.empty()) add_comp_idx = std::clamp(add_comp_idx, 0, static_cast<int>(avail_components.size()) - 1);

        if (avail_components.empty()) {
          ImGui::TextDisabled("No unlocked components match filter");
        } else {
          std::vector<const char*> comp_labels;
          comp_labels.reserve(avail_components.size());
          std::vector<std::string> comp_label_storage;
          comp_label_storage.reserve(avail_components.size());

          for (const auto& cid : avail_components) {
            const auto it = sim.content().components.find(cid);
            const auto& cdef = it->second;
            std::string lbl = cdef.name + " (" + component_type_label(cdef.type) + ")##" + cid;
            comp_label_storage.push_back(std::move(lbl));
          }
          for (const auto& s2 : comp_label_storage) comp_labels.push_back(s2.c_str());

          ImGui::Combo("Component", &add_comp_idx, comp_labels.data(), static_cast<int>(comp_labels.size()));

          if (ImGui::Button("Add")) {
            comp_list.push_back(avail_components[add_comp_idx]);
          }
        }

        // Preview stats
        ShipDesign preview;
        preview.id = new_id;
        preview.name = new_name;
        preview.role = (role_idx == 0) ? ShipRole::Freighter : (role_idx == 1) ? ShipRole::Surveyor : ShipRole::Combatant;
        preview.components = comp_list;
        preview = derive_preview_design(sim.content(), preview);

        ImGui::Separator();
        ImGui::Text("Preview");
        ImGui::Text("Mass: %.0f t", preview.mass_tons);
        ImGui::Text("Speed: %.1f km/s", preview.speed_km_s);
        ImGui::Text("HP: %.0f", preview.max_hp);
        ImGui::Text("Cargo: %.0f t", preview.cargo_tons);
        ImGui::Text("Sensor: %.0f mkm", preview.sensor_range_mkm);
        if (preview.weapon_damage > 0.0) ImGui::Text("Weapons: %.1f (range %.1f)", preview.weapon_damage, preview.weapon_range_mkm);

        if (ImGui::Button("Save custom design")) {
          std::string err;
          if (sim.upsert_custom_design(preview, &err)) {
            status = "Saved custom design: " + preview.id;
          } else {
            status = "Error: " + err;
          }
        }
        if (!status.empty()) {
          ImGui::Spacing();
          ImGui::TextWrapped("%s", status.c_str());
        }

        ImGui::EndTabItem();
      }
    }

    // --- Contacts / intel tab ---
    if (ImGui::BeginTabItem("Contacts")) {
      // Default viewer faction: use selected ship's faction if available, otherwise use the faction combo.
      Id viewer_faction_id = selected_faction_id;
      if (selected_ship != kInvalidId) {
        if (const auto* sh = find_ptr(s.ships, selected_ship)) viewer_faction_id = sh->faction_id;
      }

      Faction* viewer = (viewer_faction_id == kInvalidId) ? nullptr : find_ptr(s.factions, viewer_faction_id);

      if (!viewer) {
        ImGui::TextDisabled("Select a faction (Research tab) or select a ship to view contacts");
        ImGui::EndTabItem();
      } else {
        const auto* sys = find_ptr(s.systems, s.selected_system);
        const char* sys_name = sys ? sys->name.c_str() : "(none)";

        ImGui::Text("Viewer: %s", viewer->name.c_str());
        ImGui::TextDisabled("Contacts are last-known snapshots from sensors; they may be stale.");

        ImGui::Separator();
        ImGui::Checkbox("Fog of war", &ui.fog_of_war);
        ImGui::SameLine();
        ImGui::Checkbox("Show contact markers", &ui.show_contact_markers);

        ImGui::InputInt("Show <= days old", &ui.contact_max_age_days);
        ui.contact_max_age_days = std::clamp(ui.contact_max_age_days, 1, 365);

        static bool only_current_system = true;
        ImGui::Checkbox("Only selected system", &only_current_system);
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", sys_name);

        const int now = static_cast<int>(s.date.days_since_epoch());

        struct Row {
          Contact c;
          int age{0};
        };
        std::vector<Row> rows;
        rows.reserve(viewer->ship_contacts.size());

        for (const auto& [_, c] : viewer->ship_contacts) {
          if (only_current_system && c.system_id != s.selected_system) continue;
          const int age = now - c.last_seen_day;
          if (age < 0) continue;
          if (age > ui.contact_max_age_days) continue;
          rows.push_back(Row{c, age});
        }

        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
          if (a.age != b.age) return a.age < b.age; // younger first
          return a.c.ship_id < b.c.ship_id;
        });

        ImGui::Separator();
        ImGui::Text("Contacts: %d", (int)rows.size());

        if (rows.empty()) {
          ImGui::TextDisabled("(none)");
        } else {
          for (const auto& r : rows) {
            const auto* sys2 = find_ptr(s.systems, r.c.system_id);
            const char* sys2_name = sys2 ? sys2->name.c_str() : "(unknown system)";

            std::string title = r.c.last_seen_name.empty() ? ("Contact #" + std::to_string(r.c.ship_id)) : r.c.last_seen_name;
            title += "##contact_" + std::to_string(r.c.ship_id);

            if (ImGui::TreeNode(title.c_str())) {
              ImGui::Text("System: %s", sys2_name);
              ImGui::Text("Age: %d day(s)", r.age);
              ImGui::Text("Last known pos: (%.2f, %.2f) mkm", r.c.last_seen_position_mkm.x, r.c.last_seen_position_mkm.y);
              if (!r.c.last_seen_design_id.empty()) ImGui::Text("Last seen design: %s", r.c.last_seen_design_id.c_str());

              const bool detected_now = sim.is_ship_detected_by_faction(viewer->id, r.c.ship_id);
              ImGui::Text("Currently detected: %s", detected_now ? "yes" : "no");

              if (ImGui::SmallButton(("View system##" + std::to_string(r.c.ship_id)).c_str())) {
                s.selected_system = r.c.system_id;
              }

              // If the player has a ship selected in the same system, offer quick actions.
              if (selected_ship != kInvalidId) {
                const auto* my_ship = find_ptr(s.ships, selected_ship);
                if (my_ship && my_ship->faction_id == viewer->id && my_ship->system_id == r.c.system_id) {
                  ImGui::SameLine();
                  if (ImGui::SmallButton(("Investigate##" + std::to_string(r.c.ship_id)).c_str())) {
                    sim.issue_move_to_point(selected_ship, r.c.last_seen_position_mkm);
                  }

                  ImGui::SameLine();
                  const char* btn = detected_now ? "Attack" : "Intercept";
                  if (ImGui::SmallButton((std::string(btn) + "##" + std::to_string(r.c.ship_id)).c_str())) {
                    // If not currently detected, this will issue an intercept based on the stored contact snapshot.
                    sim.issue_attack_ship(selected_ship, r.c.ship_id, ui.fog_of_war);
                  }
                }
              }

              ImGui::TreePop();
            }
          }
        }

        ImGui::EndTabItem();
      }
    }

    // --- Event log tab ---
    {
      const std::uint64_t newest_seq = (s.next_event_seq > 0) ? (s.next_event_seq - 1) : 0;
      // UIState isn't persisted; it can be out of sync after New Game / Load.
      if (ui.last_seen_event_seq > newest_seq) ui.last_seen_event_seq = 0;

      int unread = 0;
      for (const auto& ev : s.events) {
        if (ev.seq > ui.last_seen_event_seq) ++unread;
      }

      std::string log_label;
      if (unread > 0) {
        log_label = "Log (" + std::to_string(unread) + ")###log_tab";
      } else {
        log_label = "Log###log_tab";
      }

      if (ImGui::BeginTabItem(log_label.c_str())) {
        // Mark everything up to the newest event as "seen" while the tab is open.
        if (newest_seq > ui.last_seen_event_seq) ui.last_seen_event_seq = newest_seq;

        ImGui::Text("Event log (saved with game)");
        ImGui::TextDisabled("Entries: %d   (unread when opened: %d)", (int)s.events.size(), unread);

        static bool show_info = true;
        static bool show_warn = true;
        static bool show_error = true;
        static int category_idx = 0; // 0=All
        static Id faction_filter = kInvalidId;
        static int max_show = 200;
        static char search_buf[128] = "";

        ImGui::Checkbox("Info", &show_info);
        ImGui::SameLine();
        ImGui::Checkbox("Warn", &show_warn);
        ImGui::SameLine();
        ImGui::Checkbox("Error", &show_error);

        // Category filter.
        {
          const char* cats[] = {
              "All",
              "General",
              "Research",
              "Shipyard",
              "Construction",
              "Movement",
              "Combat",
              "Intel",
              "Exploration",
          };
          ImGui::Combo("Category", &category_idx, cats, IM_ARRAYSIZE(cats));
        }

        // Faction filter.
        {
          const auto factions = sorted_factions(s);
          const auto* sel = find_ptr(s.factions, faction_filter);
          const char* label = (faction_filter == kInvalidId) ? "All" : (sel ? sel->name.c_str() : "(missing)");

          if (ImGui::BeginCombo("Faction", label)) {
            if (ImGui::Selectable("All", faction_filter == kInvalidId)) faction_filter = kInvalidId;
            for (const auto& [fid, name] : factions) {
              if (ImGui::Selectable(name.c_str(), faction_filter == fid)) faction_filter = fid;
            }
            ImGui::EndCombo();
          }
        }

        ImGui::InputText("Search", search_buf, IM_ARRAYSIZE(search_buf));

        ImGui::InputInt("Show last N", &max_show);
        max_show = std::clamp(max_show, 10, 5000);

        static char export_path[256] = "events.csv";
        static std::string export_status;

        ImGui::SameLine();
        if (ImGui::SmallButton("Clear log")) {
          s.events.clear();
          export_status = "Event log cleared.";
        }

        // Collect visible indices (newest-first) based on filters + limit.
        std::vector<int> rows;
        rows.reserve(std::min(max_show, static_cast<int>(s.events.size())));
        for (int i = static_cast<int>(s.events.size()) - 1; i >= 0 && (int)rows.size() < max_show; --i) {
          const auto& ev = s.events[static_cast<std::size_t>(i)];
          const bool ok = (ev.level == EventLevel::Info && show_info) || (ev.level == EventLevel::Warn && show_warn) ||
                          (ev.level == EventLevel::Error && show_error);
          if (!ok) continue;

          if (!case_insensitive_contains(ev.message, search_buf)) continue;

          // Category filter.
          if (category_idx > 0) {
            static const EventCategory cat_vals[] = {
                EventCategory::General,
                EventCategory::Research,
                EventCategory::Shipyard,
                EventCategory::Construction,
                EventCategory::Movement,
                EventCategory::Combat,
                EventCategory::Intel,
                EventCategory::Exploration,
            };
            const int idx = category_idx - 1;
            if (idx < 0 || idx >= (int)IM_ARRAYSIZE(cat_vals)) continue;
            if (ev.category != cat_vals[idx]) continue;
          }

          // Faction filter (match either primary or secondary).
          if (faction_filter != kInvalidId) {
            if (ev.faction_id != faction_filter && ev.faction_id2 != faction_filter) continue;
          }

          rows.push_back(i);
        }

        ImGui::InputText("Export path", export_path, IM_ARRAYSIZE(export_path));

        if (ImGui::SmallButton("Copy visible")) {
          std::string out;
          out.reserve(rows.size() * 96);
          for (int idx : rows) {
            const auto& ev = s.events[static_cast<std::size_t>(idx)];
            const nebula4x::Date d(ev.day);
            out += std::string("[") + d.to_string() + "] #" + std::to_string(static_cast<unsigned long long>(ev.seq)) +
                   " [" + event_category_label(ev.category) + "] " + event_level_label(ev.level) + ": " + ev.message;
            out.push_back('\n');
          }
          ImGui::SetClipboardText(out.c_str());
          export_status = "Copied " + std::to_string(rows.size()) + " event(s) to clipboard.";
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("Export CSV")) {
          try {
            if (export_path[0] == '\0') {
              export_status = "Export failed: export path is empty.";
            } else {
              auto faction_name = [&](Id id) -> std::string {
                if (id == kInvalidId) return {};
                const auto* f = find_ptr(s.factions, id);
                return f ? f->name : std::string{};
              };
              auto system_name = [&](Id id) -> std::string {
                if (id == kInvalidId) return {};
                const auto* sys = find_ptr(s.systems, id);
                return sys ? sys->name : std::string{};
              };
              auto ship_name = [&](Id id) -> std::string {
                if (id == kInvalidId) return {};
                const auto* sh = find_ptr(s.ships, id);
                return sh ? sh->name : std::string{};
              };
              auto colony_name = [&](Id id) -> std::string {
                if (id == kInvalidId) return {};
                const auto* c = find_ptr(s.colonies, id);
                return c ? c->name : std::string{};
              };

              std::string csv;
              csv += "day,date,seq,level,category,"
                     "faction_id,faction,"
                     "faction_id2,faction2,"
                     "system_id,system,"
                     "ship_id,ship,"
                     "colony_id,colony,"
                     "message\n";

              // Export in chronological order (oldest to newest within the visible set).
              for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
                const auto& ev = s.events[static_cast<std::size_t>(*it)];
                const nebula4x::Date d(ev.day);

                csv += std::to_string(static_cast<long long>(ev.day));
                csv += ",";
                csv += nebula4x::csv_escape(d.to_string());
                csv += ",";
                csv += std::to_string(static_cast<unsigned long long>(ev.seq));
                csv += ",";
                csv += nebula4x::csv_escape(std::string(event_level_label(ev.level)));
                csv += ",";
                csv += nebula4x::csv_escape(std::string(event_category_label(ev.category)));
                csv += ",";
                csv += std::to_string(static_cast<unsigned long long>(ev.faction_id));
                csv += ",";
                csv += nebula4x::csv_escape(faction_name(ev.faction_id));
                csv += ",";
                csv += std::to_string(static_cast<unsigned long long>(ev.faction_id2));
                csv += ",";
                csv += nebula4x::csv_escape(faction_name(ev.faction_id2));
                csv += ",";
                csv += std::to_string(static_cast<unsigned long long>(ev.system_id));
                csv += ",";
                csv += nebula4x::csv_escape(system_name(ev.system_id));
                csv += ",";
                csv += std::to_string(static_cast<unsigned long long>(ev.ship_id));
                csv += ",";
                csv += nebula4x::csv_escape(ship_name(ev.ship_id));
                csv += ",";
                csv += std::to_string(static_cast<unsigned long long>(ev.colony_id));
                csv += ",";
                csv += nebula4x::csv_escape(colony_name(ev.colony_id));
                csv += ",";
                csv += nebula4x::csv_escape(ev.message);
                csv += "\n";
              }

              write_text_file(export_path, csv);
              export_status = "Exported " + std::to_string(rows.size()) + " event(s) to " + std::string(export_path);
            }
          } catch (const std::exception& e) {
            export_status = std::string("Export failed: ") + e.what();
            nebula4x::log::error(export_status);
          }
        }

        if (!export_status.empty()) {
          ImGui::TextWrapped("%s", export_status.c_str());
        }

        ImGui::Separator();

        int shown = 0;
        for (int i : rows) {
          const auto& ev = s.events[static_cast<std::size_t>(i)];
          const nebula4x::Date d(ev.day);
          ImGui::BulletText("[%s] #%llu [%s] %s: %s", d.to_string().c_str(),
                            static_cast<unsigned long long>(ev.seq), event_category_label(ev.category),
                            event_level_label(ev.level), ev.message.c_str());

          ImGui::PushID(i);
          ImGui::SameLine();
          if (ImGui::SmallButton("Copy")) {
            std::string line = std::string("[") + d.to_string() + "] #" +
                              std::to_string(static_cast<unsigned long long>(ev.seq)) + " [" +
                              event_category_label(ev.category) + "] " + event_level_label(ev.level) + ": " +
                              ev.message;
            ImGui::SetClipboardText(line.c_str());
          }
          if (ev.system_id != kInvalidId) {
            ImGui::SameLine();
            if (ImGui::SmallButton("View system")) {
              s.selected_system = ev.system_id;
            }
          }
          if (ev.colony_id != kInvalidId) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Select colony")) {
              selected_colony = ev.colony_id;
            }
          }
          if (ev.ship_id != kInvalidId) {
            if (const auto* sh = find_ptr(s.ships, ev.ship_id)) {
              ImGui::SameLine();
              if (ImGui::SmallButton("Select ship")) {
                selected_ship = ev.ship_id;
                s.selected_system = sh->system_id;
              }
            }
          }
          ImGui::PopID();
          ++shown;
        }

        if (shown == 0) {
          ImGui::TextDisabled("(none)");
        }

        ImGui::EndTabItem();
      }
    }

    ImGui::EndTabBar();
  }
}

} // namespace nebula4x::ui
