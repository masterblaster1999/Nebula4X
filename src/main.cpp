// On Windows, SDL can redefine `main` to `SDL_main` via a macro in SDL_main.h.
// If we don't opt out, MSVC will look for a real `main` symbol and the link
// step fails with LNK2019 (unresolved external symbol WinMain).
#define SDL_MAIN_HANDLED

#include <SDL.h>

#include <imgui.h>
#include <imgui_impl_sdl2.h>

// Renderer backend selection:
// - OpenGL2 enables ImGui Multi-Viewports (detachable OS windows).
// - SDL_Renderer2 is a fallback for environments without OpenGL dev libs.
#if NEBULA4X_UI_RENDERER_OPENGL2
#include <SDL_opengl.h>
#include <imgui_impl_opengl2.h>
#else
#include <imgui_impl_sdlrenderer2.h>
#endif

#include <cstdio>
#include <exception>
#include <filesystem>
#include <vector>
#include <string>

#include "nebula4x/core/simulation.h"
#include "nebula4x/core/tech.h"
#include "ui/app.h"

namespace {
std::string find_data_file(const std::string& rel_path) {
  namespace fs = std::filesystem;
  // Try common working directories (repo root, build dirs).
  const std::vector<std::string> prefixes = {
      "", "../", "../../", "../../../", "../../../../",
  };
  for (const auto& p : prefixes) {
    fs::path candidate = fs::path(p) / rel_path;
    std::error_code ec;
    if (fs::exists(candidate, ec) && !ec) {
      return candidate.lexically_normal().string();
    }
  }
  return rel_path;
}
} // namespace

int main(int /*argc*/, char** /*argv*/) {
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0) {
    std::fprintf(stderr, "Error: %s\n", SDL_GetError());
    return 1;
  }

  // Enable IME. Note: SDL_HINT_IME_SHOW_UI is optional and platform-specific.
  SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");

#if NEBULA4X_UI_RENDERER_OPENGL2
  // Configure an OpenGL 2.x context for the ImGui OpenGL2 backend.
  // (OpenGL 2.x avoids extra GL loader dependencies and still supports Viewports.)
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
#endif

  const Uint32 window_flags =
      SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
#if NEBULA4X_UI_RENDERER_OPENGL2
      | SDL_WINDOW_OPENGL
#endif
      ;

  SDL_Window* window = SDL_CreateWindow("Nebula4X", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280,
                                        720, window_flags);
  if (!window) {
    std::fprintf(stderr, "Error: SDL_CreateWindow: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }

#if NEBULA4X_UI_RENDERER_OPENGL2
  SDL_GLContext gl_context = SDL_GL_CreateContext(window);
  if (!gl_context) {
    std::fprintf(stderr, "Error: SDL_GL_CreateContext: %s\n", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  SDL_GL_MakeCurrent(window, gl_context);
  SDL_GL_SetSwapInterval(1); // Enable vsync
#else
  SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer) {
    std::fprintf(stderr, "Error: SDL_CreateRenderer: %s\n", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  if (SDL_RenderSetVSync(renderer, 1) != 0) {
    std::fprintf(stderr, "Warning: SDL_RenderSetVSync failed: %s\n", SDL_GetError());
  }
#endif

  // Setup Dear ImGui context
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

#if NEBULA4X_UI_RENDERER_OPENGL2
  // The App can still toggle this at runtime via UI prefs, but enabling here
  // ensures the backend is initialized with viewport support enabled by default.
  io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
#endif

  // Setup Dear ImGui style
  ImGui::StyleColorsDark();

  // Setup Platform/Renderer backends
#if NEBULA4X_UI_RENDERER_OPENGL2
  ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
  ImGui_ImplOpenGL2_Init();
#else
  ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer2_Init(renderer);
#endif

  // Simulation + UI application
  // Load content/tech from the bundled JSON files.
  nebula4x::ContentDB content;
  {
    const std::string resources = find_data_file("data/blueprints/resources.json");
    const std::string blueprints = find_data_file("data/blueprints/starting_blueprints.json");
    try {
      content = nebula4x::load_content_db_from_files({resources, blueprints});
    } catch (const std::exception& e) {
      std::fprintf(stderr, "Warning: failed to load content DB (%s): %s\n", blueprints.c_str(), e.what());
      content = nebula4x::ContentDB{};
    }

    const std::string tech_path = find_data_file("data/tech/tech_tree.json");
    try {
      content.techs = nebula4x::load_tech_db_from_file(tech_path);
      content.tech_source_paths = {tech_path};
    } catch (const std::exception& e) {
      std::fprintf(stderr, "Warning: failed to load tech DB (%s): %s\n", tech_path.c_str(), e.what());
    }
  }

  nebula4x::SimConfig cfg;
  nebula4x::Simulation sim(std::move(content), cfg);
  nebula4x::ui::App app(std::move(sim));

  // Load UI preferences (theme/layout + window visibility).
  // This also controls whether Multi-Viewports are enabled.
  app.load_ui_prefs(app.ui_prefs_path());

  bool done = false;
  Uint64 prev_counter = SDL_GetPerformanceCounter();
  const double freq = static_cast<double>(SDL_GetPerformanceFrequency());

  while (!done) {
    // Frametime in ms.
    const Uint64 counter = SDL_GetPerformanceCounter();
    [[maybe_unused]] const double delta_ms = (static_cast<double>(counter - prev_counter) * 1000.0) / freq;
    prev_counter = counter;

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL2_ProcessEvent(&event);
      app.on_event(event);
      if (event.type == SDL_QUIT) done = true;
      if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE &&
          event.window.windowID == SDL_GetWindowID(window)) {
        done = true;
      }
    }

    // If the main window is minimized, don't waste time rendering.
    if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
      SDL_Delay(10);
      continue;
    }

    // App pre-frame work (ini reload, config updates) must happen before NewFrame.
    app.pre_frame();

    // Start the Dear ImGui frame
#if NEBULA4X_UI_RENDERER_OPENGL2
    ImGui_ImplOpenGL2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
#else
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
#endif
    ImGui::NewFrame();

    // UI/game frame
    app.frame();

    // Rendering
    ImGui::Render();

#if NEBULA4X_UI_RENDERER_OPENGL2
    int display_w = 0, display_h = 0;
    SDL_GL_GetDrawableSize(window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);

    const float* clear_color = app.clear_color_rgba();
    glClearColor(clear_color[0] * clear_color[3], clear_color[1] * clear_color[3], clear_color[2] * clear_color[3], clear_color[3]);
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());

    // Multi-Viewport support: render additional platform windows.
#ifdef IMGUI_HAS_VIEWPORT
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
      SDL_Window* backup_current_window = SDL_GL_GetCurrentWindow();
      SDL_GLContext backup_current_context = SDL_GL_GetCurrentContext();
      ImGui::UpdatePlatformWindows();
      ImGui::RenderPlatformWindowsDefault();
      SDL_GL_MakeCurrent(backup_current_window, backup_current_context);
    }
#endif

    SDL_GL_SwapWindow(window);
#else
    // SDL_Renderer backend
    SDL_RenderSetScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColor(renderer, static_cast<Uint8>(app.clear_color_rgba()[0] * 255.0f),
                           static_cast<Uint8>(app.clear_color_rgba()[1] * 255.0f),
                           static_cast<Uint8>(app.clear_color_rgba()[2] * 255.0f),
                           static_cast<Uint8>(app.clear_color_rgba()[3] * 255.0f));
    SDL_RenderClear(renderer);

    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
    SDL_RenderPresent(renderer);
#endif
  }

  // Save UI prefs on exit if the user has enabled it.
  if (app.autosave_ui_prefs_enabled()) {
    app.save_ui_prefs(app.ui_prefs_path());
  }

  // Cleanup
#if NEBULA4X_UI_RENDERER_OPENGL2
  ImGui_ImplOpenGL2_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();

  SDL_GL_DeleteContext(gl_context);
  SDL_DestroyWindow(window);
#else
  ImGui_ImplSDLRenderer2_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();

  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
#endif

  SDL_Quit();
  return 0;
}
