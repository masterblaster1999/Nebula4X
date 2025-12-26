#include "ui/panels.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "nebula4x/core/serialization.h"
#include "nebula4x/util/file_io.h"
#include "nebula4x/util/log.h"

namespace nebula4x::ui {
namespace {

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

void draw_right_sidebar(Simulation& sim, UIState& ui, Id selected_ship, Id& selected_colony) {
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

        if (d) {
          ImGui::Text("Design: %s (%s)", d->name.c_str(), ship_role_label(d->role));
          ImGui::Text("Mass: %.0f t", d->mass_tons);
          ImGui::Text("HP: %.0f / %.0f", sh->hp, d->max_hp);
          ImGui::Text("Cargo: %.0f t", d->cargo_tons);
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

        ImGui::Separator();
        ImGui::Text("Quick orders");
        if (ImGui::Button("Move to (0,0)")) {
          sim.issue_move_to_point(selected_ship, {0.0, 0.0});
        }
        if (ImGui::Button("Move to Earth")) {
          const auto* sys2 = find_ptr(s.systems, sh->system_id);
          if (sys2) {
            for (Id bid : sys2->bodies) {
              const auto* b = find_ptr(s.bodies, bid);
              if (b && b->name == "Earth") {
                sim.issue_move_to_body(selected_ship, b->id);
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
                  sim.issue_attack_ship(sh->id, hid);
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

        const bool has_yard = colony->installations["shipyard"] > 0;
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
            ImGui::Text("Cargo: %.0f t", d->cargo_tons);
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
                  if (!detected_now) {
                    ImGui::BeginDisabled();
                  }
                  if (ImGui::SmallButton(("Attack##" + std::to_string(r.c.ship_id)).c_str())) {
                    sim.issue_attack_ship(selected_ship, r.c.ship_id);
                  }
                  if (!detected_now) {
                    ImGui::EndDisabled();
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

    ImGui::EndTabBar();
  }
}

} // namespace nebula4x::ui
