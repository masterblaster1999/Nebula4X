#include "ui/new_game_modal.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_set>

#include "nebula4x/core/scenario.h"
#include "nebula4x/util/log.h"

namespace nebula4x::ui {
namespace {

constexpr int kScenarioSol = 0;
constexpr int kScenarioRandom = 1;

std::uint32_t time_seed_u32() {
  using namespace std::chrono;
  const std::uint64_t t = static_cast<std::uint64_t>(
      duration_cast<nanoseconds>(high_resolution_clock::now().time_since_epoch()).count());
  // Mix bits a bit to avoid obvious patterns when only low bits change.
  std::uint64_t x = t;
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return static_cast<std::uint32_t>(x ^ (x >> 32));
}

struct RandomPreviewCache {
  bool valid{false};
  std::uint32_t seed{0};
  int num_systems{0};
  GameState state;
  std::string error;
};

void ensure_preview(RandomPreviewCache& cache, std::uint32_t seed, int num_systems) {
  seed = static_cast<std::uint32_t>(seed);
  num_systems = std::clamp(num_systems, 1, 64);

  if (cache.valid && cache.seed == seed && cache.num_systems == num_systems) return;

  cache.valid = false;
  cache.seed = seed;
  cache.num_systems = num_systems;
  cache.error.clear();

  try {
    cache.state = nebula4x::make_random_scenario(seed, num_systems);
    cache.valid = true;
  } catch (const std::exception& e) {
    cache.error = e.what();
  }
}

void draw_galaxy_preview(const GameState& s) {
  ImVec2 avail = ImGui::GetContentRegionAvail();
  const float h = std::clamp(avail.y, 120.0f, 240.0f);
  const ImVec2 size(avail.x, h);

  if (ImGui::BeginChild("##new_game_galaxy_preview", size, true)) {
    const ImVec2 region = ImGui::GetContentRegionAvail();

    ImGui::InvisibleButton("##galaxy_preview_canvas", region);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 rmin = ImGui::GetItemRectMin();
    const ImVec2 rmax = ImGui::GetItemRectMax();

    // Background.
    const ImU32 bg = ImGui::GetColorU32(ImGuiCol_FrameBg);
    dl->AddRectFilled(rmin, rmax, bg);

    if (s.systems.empty()) {
      ImGui::EndChild();
      return;
    }

    // Bounds.
    double minx = 1e30, maxx = -1e30, miny = 1e30, maxy = -1e30;
    for (const auto& [_, sys] : s.systems) {
      minx = std::min(minx, sys.galaxy_pos.x);
      maxx = std::max(maxx, sys.galaxy_pos.x);
      miny = std::min(miny, sys.galaxy_pos.y);
      maxy = std::max(maxy, sys.galaxy_pos.y);
    }
    const double dx = std::max(1e-6, maxx - minx);
    const double dy = std::max(1e-6, maxy - miny);

    const float pad = 10.0f;
    const float w = std::max(1.0f, region.x - pad * 2.0f);
    const float h2 = std::max(1.0f, region.y - pad * 2.0f);

    const double sx = static_cast<double>(w) / dx;
    const double sy = static_cast<double>(h2) / dy;
    const double scale = std::min(sx, sy);

    const float ox = pad + static_cast<float>((static_cast<double>(w) - dx * scale) * 0.5);
    const float oy = pad + static_cast<float>((static_cast<double>(h2) - dy * scale) * 0.5);

    auto to_screen = [&](const Vec2& gp) -> ImVec2 {
      const float x = rmin.x + ox + static_cast<float>((gp.x - minx) * scale);
      // Flip Y so positive galaxy_pos.y is "up".
      const float y = rmin.y + oy + static_cast<float>((maxy - gp.y) * scale);
      return ImVec2(x, y);
    };

    // Draw jump connections.
    std::unordered_set<std::uint64_t> drawn;
    drawn.reserve(s.jump_points.size() * 2);

    const ImU32 line_col = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    for (const auto& [_, jp] : s.jump_points) {
      const auto* other = nebula4x::find_ptr(s.jump_points, jp.linked_jump_id);
      if (!other) continue;
      const Id a = jp.system_id;
      const Id b = other->system_id;
      if (a == kInvalidId || b == kInvalidId) continue;
      const auto lo = static_cast<std::uint32_t>(std::min(a, b));
      const auto hi = static_cast<std::uint32_t>(std::max(a, b));
      const std::uint64_t key = (static_cast<std::uint64_t>(lo) << 32) | static_cast<std::uint64_t>(hi);
      if (!drawn.insert(key).second) continue;

      const auto* sys_a = nebula4x::find_ptr(s.systems, a);
      const auto* sys_b = nebula4x::find_ptr(s.systems, b);
      if (!sys_a || !sys_b) continue;

      dl->AddLine(to_screen(sys_a->galaxy_pos), to_screen(sys_b->galaxy_pos), line_col, 1.0f);
    }

    // Draw systems.
    const ImU32 star_col = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 home_col = ImGui::GetColorU32(ImGuiCol_PlotHistogram);

    for (const auto& [id, sys] : s.systems) {
      const ImVec2 p = to_screen(sys.galaxy_pos);
      const float r = (id == s.selected_system) ? 6.0f : 4.0f;
      const ImU32 col = (id == s.selected_system) ? home_col : star_col;
      dl->AddCircleFilled(p, r, col);
    }

    // Hover tooltip.
    if (ImGui::IsItemHovered()) {
      const ImVec2 m = ImGui::GetIO().MousePos;
      const float hit_r2 = 8.0f * 8.0f;

      Id best_id = kInvalidId;
      float best_d2 = hit_r2;

      for (const auto& [id, sys] : s.systems) {
        const ImVec2 p = to_screen(sys.galaxy_pos);
        const float dx2 = m.x - p.x;
        const float dy2 = m.y - p.y;
        const float d2 = dx2 * dx2 + dy2 * dy2;
        if (d2 <= best_d2) {
          best_d2 = d2;
          best_id = id;
        }
      }

      if (best_id != kInvalidId) {
        const auto* sys = nebula4x::find_ptr(s.systems, best_id);
        if (sys) {
          ImGui::BeginTooltip();
          ImGui::Text("%s", sys->name.c_str());
          ImGui::Separator();
          ImGui::TextDisabled("Systems: %d", static_cast<int>(s.systems.size()));
          ImGui::TextDisabled("Jump points: %d", static_cast<int>(s.jump_points.size()));
          ImGui::EndTooltip();
        }
      }
    }
  }

  ImGui::EndChild();
}

} // namespace

void draw_new_game_modal(Simulation& sim, UIState& ui) {
  if (!ui.show_new_game_modal) return;

  // Keep the popup open while the flag is set.
  ImGui::OpenPopup("New Game");

  bool open = true;
  if (ImGui::BeginPopupModal("New Game", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
    ui.new_game_scenario = std::clamp(ui.new_game_scenario, kScenarioSol, kScenarioRandom);

    ImGui::Text("Choose scenario");

    if (ImGui::RadioButton("Sol (classic)", ui.new_game_scenario == kScenarioSol)) {
      ui.new_game_scenario = kScenarioSol;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Random galaxy (procedural)", ui.new_game_scenario == kScenarioRandom)) {
      ui.new_game_scenario = kScenarioRandom;
    }

    ImGui::Separator();

    static RandomPreviewCache preview;

    if (ui.new_game_scenario == kScenarioSol) {
      ImGui::TextWrapped(
          "A compact starter scenario in the Sol system. Good for learning the UI and testing early ship designs.");

    } else {
      // --- Random scenario settings ---
      ui.new_game_random_num_systems = std::clamp(ui.new_game_random_num_systems, 1, 64);

      ImGui::Text("Random galaxy settings");

      // Seed.
      {
        std::uint32_t seed = ui.new_game_random_seed;
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputScalar("Seed", ImGuiDataType_U32, &seed);
        ui.new_game_random_seed = seed;

        ImGui::SameLine();
        if (ImGui::Button("Randomize")) {
          ui.new_game_random_seed = time_seed_u32();
          preview.valid = false;
        }
      }

      // System count.
      {
        int n = ui.new_game_random_num_systems;
        ImGui::SetNextItemWidth(180.0f);
        ImGui::SliderInt("Systems", &n, 1, 64);
        ui.new_game_random_num_systems = std::clamp(n, 1, 64);
      }

      ImGui::SameLine();
      const bool manual = ImGui::Button("Generate preview");

      // Auto-preview when the user isn't actively editing inputs.
      const bool auto_trigger = (!preview.valid) && !ImGui::IsAnyItemActive();
      if (manual || auto_trigger || (preview.seed != ui.new_game_random_seed) || (preview.num_systems != ui.new_game_random_num_systems)) {
        // Debounce: only regenerate when inputs aren't active, unless explicitly requested.
        if (manual || !ImGui::IsAnyItemActive()) {
          ensure_preview(preview, ui.new_game_random_seed, ui.new_game_random_num_systems);
        }
      }

      if (!preview.error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "Preview failed: %s", preview.error.c_str());
      }

      if (preview.valid) {
        const GameState& s = preview.state;

        ImGui::Separator();
        ImGui::Text("Preview");
        ImGui::TextDisabled("Systems: %d", static_cast<int>(s.systems.size()));
        ImGui::SameLine();
        ImGui::TextDisabled("Bodies: %d", static_cast<int>(s.bodies.size()));
        ImGui::SameLine();
        ImGui::TextDisabled("Jump points: %d", static_cast<int>(s.jump_points.size()));
        ImGui::TextDisabled("Colonies: %d", static_cast<int>(s.colonies.size()));
        ImGui::SameLine();
        ImGui::TextDisabled("Ships: %d", static_cast<int>(s.ships.size()));

        draw_galaxy_preview(s);
      }
    }

    ImGui::Separator();

    // Buttons.
    const float bw = 140.0f;
    if (ImGui::Button("Start", ImVec2(bw, 0.0f))) {
      if (ui.new_game_scenario == kScenarioSol) {
        sim.new_game();
        ui.request_map_tab = MapTab::System;
        nebula4x::log::info("New game: Sol scenario");
      } else {
        sim.new_game_random(ui.new_game_random_seed, ui.new_game_random_num_systems);
        ui.request_map_tab = MapTab::Galaxy;
        nebula4x::log::info("New game: random galaxy (seed=" + std::to_string(ui.new_game_random_seed) +
                           ", systems=" + std::to_string(ui.new_game_random_num_systems) + ")");
      }

      ui.show_new_game_modal = false;
      ImGui::CloseCurrentPopup();
    }

    ImGui::SameLine();

    if (ImGui::Button("Cancel", ImVec2(bw, 0.0f))) {
      ui.show_new_game_modal = false;
      ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
  }

  if (!open) {
    ui.show_new_game_modal = false;
  }
}

} // namespace nebula4x::ui
