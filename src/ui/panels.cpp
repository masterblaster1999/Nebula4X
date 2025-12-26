#include "ui/panels.h"

#include <imgui.h>

#include <algorithm>
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

std::vector<std::string> sorted_design_ids(const ContentDB& c) {
  std::vector<std::string> ids;
  ids.reserve(c.designs.size());
  for (const auto& [id, _] : c.designs) ids.push_back(id);
  std::sort(ids.begin(), ids.end());
  return ids;
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

void draw_left_sidebar(Simulation& sim, Id& selected_ship, Id& selected_colony) {
  ImGui::Text("Turns");
  if (ImGui::Button("+1 day")) sim.advance_days(1);
  ImGui::SameLine();
  if (ImGui::Button("+5")) sim.advance_days(5);
  ImGui::SameLine();
  if (ImGui::Button("+30")) sim.advance_days(30);

  ImGui::Separator();
  ImGui::Text("Systems");
  for (const auto& [id, sys] : sim.state().systems) {
    const bool sel = (sim.state().selected_system == id);
    if (ImGui::Selectable(sys.name.c_str(), sel)) {
      sim.state().selected_system = id;
    }
  }

  ImGui::Separator();
  ImGui::Text("Ships");

  const auto* sys = find_ptr(sim.state().systems, sim.state().selected_system);
  if (!sys) {
    ImGui::TextDisabled("No system selected");
    return;
  }

  for (Id sid : sys->ships) {
    const auto* sh = find_ptr(sim.state().ships, sid);
    if (!sh) continue;
    std::string label = sh->name + "##" + std::to_string(sh->id);
    if (ImGui::Selectable(label.c_str(), selected_ship == sid)) {
      selected_ship = sid;
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

void draw_right_sidebar(Simulation& sim, Id selected_ship, Id& selected_colony) {
  const auto& s = sim.state();

  if (selected_ship != kInvalidId) {
    if (const auto* sh = find_ptr(s.ships, selected_ship)) {
      ImGui::Text("Ship");
      ImGui::Separator();
      ImGui::Text("%s", sh->name.c_str());
      ImGui::Text("Speed: %.1f km/s", sh->speed_km_s);
      ImGui::Text("Pos: (%.1f, %.1f) mkm", sh->position_mkm.x, sh->position_mkm.y);

      if (const auto it = sim.content().designs.find(sh->design_id); it != sim.content().designs.end()) {
        const auto& d = it->second;
        ImGui::Text("Design: %s", d.name.c_str());
        ImGui::Text("Role: %s", ship_role_label(d.role));
        ImGui::Text("Cargo: %.0f t", d.cargo_tons);
        ImGui::Text("Sensor: %.0f mkm", d.sensor_range_mkm);
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
        // Find Earth in the selected system.
        const auto* sys = find_ptr(s.systems, s.selected_system);
        if (sys) {
          for (Id bid : sys->bodies) {
            const auto* b = find_ptr(s.bodies, bid);
            if (b && b->name == "Earth") {
              sim.issue_move_to_body(selected_ship, b->id);
              break;
            }
          }
        }
      }
      ImGui::TextDisabled("Tip: click on the map to set a move-to-point order.");
    }
  }

  ImGui::Spacing();
  ImGui::Spacing();

  if (selected_colony != kInvalidId) {
    if (auto* colony = find_ptr(sim.state().colonies, selected_colony)) {
      ImGui::Text("Colony");
      ImGui::Separator();
      ImGui::Text("%s", colony->name.c_str());
      ImGui::Text("Population: %.0f M", colony->population_millions);

      ImGui::Separator();
      ImGui::Text("Minerals");
      for (const auto& [k, v] : colony->minerals) {
        ImGui::BulletText("%s: %.1f", k.c_str(), v);
      }

      ImGui::Separator();
      ImGui::Text("Installations");
      for (const auto& [k, v] : colony->installations) {
        ImGui::BulletText("%s: %d", k.c_str(), v);
      }

      ImGui::Separator();
      ImGui::Text("Shipyard");

      const bool has_yard = colony->installations["shipyard"] > 0;
      if (!has_yard) {
        ImGui::TextDisabled("No shipyard present");
        return;
      }

      if (colony->shipyard_queue.empty()) {
        ImGui::TextDisabled("Queue empty");
      } else {
        for (const auto& bo : colony->shipyard_queue) {
          ImGui::BulletText("%s (%.1f tons remaining)", bo.design_id.c_str(), bo.tons_remaining);
        }
      }

      static int selected_design_idx = 0;
      const auto ids = sorted_design_ids(sim.content());
      if (!ids.empty()) {
        std::vector<const char*> labels;
        labels.reserve(ids.size());
        for (const auto& id : ids) labels.push_back(id.c_str());
        ImGui::Combo("Design", &selected_design_idx, labels.data(), static_cast<int>(labels.size()));
        if (ImGui::Button("Enqueue build")) {
          selected_design_idx = std::clamp(selected_design_idx, 0, static_cast<int>(ids.size()) - 1);
          sim.enqueue_build(colony->id, ids[selected_design_idx]);
        }
      }
    }
  }
}

} // namespace nebula4x::ui
