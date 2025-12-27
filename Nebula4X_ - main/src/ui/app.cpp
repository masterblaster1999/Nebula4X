#include "ui/app.h"

#include <imgui.h>

#include "nebula4x/core/serialization.h"
#include "nebula4x/util/file_io.h"
#include "ui/panels.h"
#include "ui/galaxy_map.h"
#include "ui/system_map.h"

namespace nebula4x::ui {

App::App(Simulation sim) : sim_(std::move(sim)) {
  if (!sim_.state().colonies.empty()) {
    selected_colony_ = sim_.state().colonies.begin()->first;
  }
}

void App::on_event(const SDL_Event& /*e*/) {
  // Reserved for future (resize, etc.)
}

void App::frame() {
  draw_main_menu(sim_, save_path_, load_path_);

  // Layout: left sidebar / center map / right sidebar
  ImGui::SetNextWindowPos(ImVec2(10, 30), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(300, ImGui::GetIO().DisplaySize.y - 40), ImGuiCond_Always);
  ImGui::Begin("Controls", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
  draw_left_sidebar(sim_, ui_, selected_ship_, selected_colony_);
  ImGui::End();

  ImGui::SetNextWindowPos(ImVec2(320, 30), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x - 640, ImGui::GetIO().DisplaySize.y - 40),
                           ImGuiCond_Always);
  ImGui::Begin("Map", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
  if (ImGui::BeginTabBar("map_tabs")) {
    if (ImGui::BeginTabItem("System")) {
      draw_system_map(sim_, ui_, selected_ship_, map_zoom_, map_pan_);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Galaxy")) {
      draw_galaxy_map(sim_, ui_, selected_ship_, galaxy_zoom_, galaxy_pan_);
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  ImGui::End();

  ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 310, 30), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(300, ImGui::GetIO().DisplaySize.y - 40), ImGuiCond_Always);
  ImGui::Begin("Details", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
  draw_right_sidebar(sim_, ui_, selected_ship_, selected_colony_);
  ImGui::End();
}

} // namespace nebula4x::ui
