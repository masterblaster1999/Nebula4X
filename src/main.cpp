// On Windows, SDL can redefine `main` to `SDL_main` via a macro in SDL_main.h.
// If we don't opt out, MSVC will look for a real `main` symbol and the link
// step fails with LNK2019 (unresolved external symbol main).
//
// Defining SDL_MAIN_HANDLED prevents SDL from rewriting our entry point.
// We then call SDL_SetMainReady() before SDL_Init() to satisfy SDL on
// platforms where SDL expects its own entrypoint wrapper.
#define SDL_MAIN_HANDLED

#include <SDL.h>

// If the build system forces `-Dmain=SDL_main` (some SDL2 build setups do),
// make absolutely sure we still export a real `main` symbol.
#ifdef main
#undef main
#endif

#include <string>
#include <vector>
#include <cstdlib>

#include <algorithm>
#include <cctype>

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>

#include "nebula4x/util/log.h"

#include "nebula4x/core/simulation.h"
#include "nebula4x/core/tech.h"
#include "ui/app.h"

namespace {

std::string trim_copy(const std::string& s) {
  size_t a = 0;
  while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  size_t b = s.size();
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

std::string get_env_string(const char* name) {
#ifdef _MSC_VER
  // MSVC warns on getenv(); use _dupenv_s instead.
  char* buf = nullptr;
  size_t sz = 0;
  if (_dupenv_s(&buf, &sz, name) != 0 || buf == nullptr) return {};
  std::string out(buf);
  std::free(buf);
  return out;
#else
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string{};
#endif
}


std::vector<std::string> split_env_paths(const char* env_value) {
  std::vector<std::string> out;
  if (!env_value) return out;

  std::string cur;
  for (const char ch : std::string(env_value)) {
    if (ch == ';' || ch == ',') {
      const std::string t = trim_copy(cur);
      if (!t.empty()) out.push_back(t);
      cur.clear();
    } else {
      cur.push_back(ch);
    }
  }
  const std::string t = trim_copy(cur);
  if (!t.empty()) out.push_back(t);
  return out;
}

} // namespace

int main(int /*argc*/, char** /*argv*/) {
  try {
    nebula4x::log::set_level(nebula4x::log::Level::Info);

    const std::string content_env = get_env_string("NEBULA4X_CONTENT");
    std::vector<std::string> content_paths = split_env_paths(content_env.empty() ? nullptr : content_env.c_str());
    if (content_paths.empty()) content_paths.push_back("data/blueprints/starting_blueprints.json");
    const std::string tech_env = get_env_string("NEBULA4X_TECH");
    std::vector<std::string> tech_paths = split_env_paths(tech_env.empty() ? nullptr : tech_env.c_str());
    if (tech_paths.empty()) tech_paths.push_back("data/tech/tech_tree.json");

    auto content = nebula4x::load_content_db_from_files(content_paths);
    content.techs = nebula4x::load_tech_db_from_files(tech_paths);

    nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});
    nebula4x::ui::App app(std::move(sim));

    // When SDL doesn't wrap the entry point (SDL_MAIN_HANDLED), some platforms
    // require this before SDL_Init(). It's harmless on platforms that don't.
    SDL_SetMainReady();

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
    // Docking dramatically improves the usability of a multi-window UI.
    //
    // Note: we intentionally do NOT enable multi-viewports here because the
    // SDL_Renderer backend is not a great fit for multi-viewport rendering.
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Keep ImGui's layout/settings in a project-specific file.
    // Derive the ini filename from the UI's persisted layout profile.
    io.IniFilename = app.imgui_ini_filename();

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
      // Give the app a chance to reload docking/ini state before NewFrame.
      app.pre_frame();
      ImGui::NewFrame();

      app.frame();

      ImGui::Render();
      // Clear color is user-configurable via UI settings.
      const float* cc = app.clear_color_rgba();
      auto to_u8 = [](float v) -> Uint8 {
        v = std::clamp(v, 0.0f, 1.0f);
        return static_cast<Uint8>(v * 255.0f + 0.5f);
      };
      SDL_SetRenderDrawColor(renderer, to_u8(cc[0]), to_u8(cc[1]), to_u8(cc[2]), to_u8(cc[3]));
      SDL_RenderClear(renderer);
      // Dear ImGui's SDL_Renderer2 backend requires the renderer parameter (v1.91+).
      ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
      SDL_RenderPresent(renderer);
    }

    // Best-effort auto-save of UI prefs (theme/layout) on exit.
    if (app.autosave_ui_prefs_enabled()) {
      std::string err;
      if (!app.save_ui_prefs(app.ui_prefs_path(), &err)) {
        nebula4x::log::warn(std::string("UI prefs auto-save failed: ") + err);
      }
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
