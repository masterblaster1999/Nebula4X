#include <SDL.h>

#include <string>

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>

#include "nebula4x/util/log.h"

#include "nebula4x/core/simulation.h"
#include "nebula4x/core/tech.h"
#include "ui/app.h"

int main(int /*argc*/, char** /*argv*/) {
  try {
    nebula4x::log::set_level(nebula4x::log::Level::Info);

    auto content = nebula4x::load_content_db_from_file("data/blueprints/starting_blueprints.json");
    content.techs = nebula4x::load_tech_db_from_file("data/tech/tech_tree.json");

    nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});
    nebula4x::ui::App app(std::move(sim));

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
      nebula4x::log::error(std::string("SDL_Init failed: ") + SDL_GetError());
      return 1;
    }

    SDL_Window* window = SDL_CreateWindow("Nebula4X", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
                                          SDL_WINDOW_RESIZABLE);
    if (!window) {
      nebula4x::log::error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
      return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
      nebula4x::log::error(std::string("SDL_CreateRenderer failed: ") + SDL_GetError());
      return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    bool running = true;
    while (running) {
      SDL_Event e;
      while (SDL_PollEvent(&e)) {
        ImGui_ImplSDL2_ProcessEvent(&e);
        if (e.type == SDL_QUIT) running = false;
        if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE &&
            e.window.windowID == SDL_GetWindowID(window))
          running = false;
        app.on_event(e);
      }

      ImGui_ImplSDLRenderer2_NewFrame();
      ImGui_ImplSDL2_NewFrame();
      ImGui::NewFrame();

      app.frame();

      ImGui::Render();
      SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
      SDL_RenderClear(renderer);
      // Dear ImGui's SDL_Renderer2 backend requires the renderer parameter (v1.91+).
      ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
      SDL_RenderPresent(renderer);
    }

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
  } catch (const std::exception& e) {
    nebula4x::log::error(std::string("Fatal: ") + e.what());
    return 1;
  }
}
