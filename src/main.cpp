#include "core/content_db.h"
#include "core/serialization.h"
#include "nebula4x/util/file_io.h"
#include "ui/app.h"

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>

#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#if NEBULA4X_UI_RENDERER_OPENGL2
#include <SDL_opengl.h>
#include <imgui_impl_opengl2.h>
#endif

namespace {

enum class RendererRequest {
  Auto,
  OpenGL2,
  SDLRenderer2,
};

static bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    char ca = a[i];
    char cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
    if (ca != cb) return false;
  }
  return true;
}

static void apply_renderer_request(RendererRequest& request, std::string_view value) {
  if (iequals(value, "auto")) {
    request = RendererRequest::Auto;
  } else if (iequals(value, "sdl") || iequals(value, "sdlrenderer") ||
             iequals(value, "sdlrenderer2") || iequals(value, "software")) {
    request = RendererRequest::SDLRenderer2;
  } else if (iequals(value, "opengl") || iequals(value, "gl") || iequals(value, "opengl2")) {
    request = RendererRequest::OpenGL2;
  }
}

static RendererRequest parse_renderer_request(int argc, char** argv) {
  RendererRequest request = RendererRequest::Auto;

  // Environment override (command line wins).
  if (const char* env = SDL_getenv("NEBULA4X_RENDERER"); env && env[0]) {
    apply_renderer_request(request, std::string_view(env));
  }

  for (int i = 1; i < argc; ++i) {
    const char* raw = argv[i] ? argv[i] : "";
    std::string_view arg(raw);

    if (arg == "--renderer" && i + 1 < argc) {
      apply_renderer_request(request, std::string_view(argv[++i]));
      continue;
    }

    constexpr std::string_view kPrefix = "--renderer=";
    if (arg.rfind(kPrefix, 0) == 0) {
      apply_renderer_request(request, arg.substr(kPrefix.size()));
      continue;
    }
  }

  return request;
}

struct RendererInitResult {
  nebula4x::ui::UIRendererBackend backend{nebula4x::ui::UIRendererBackend::SDLRenderer2};
  SDL_Window* window{nullptr};
  SDL_GLContext gl_context{nullptr};
  SDL_Renderer* sdl_renderer{nullptr};

  bool used_fallback{false};
  std::string fallback_reason{};

  // Only filled for OpenGL2.
  std::string gl_vendor{};
  std::string gl_renderer{};
  std::string gl_version{};
  std::string glsl_version{};
};

static bool create_sdlrenderer_window(RendererInitResult& out, Uint32 base_window_flags, int width,
                                      int height, std::string* error) {
  SDL_Window* window =
      SDL_CreateWindow("Nebula4X", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height,
                       base_window_flags);
  if (!window) {
    if (error) *error = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
    return false;
  }

  // Prefer accelerated+vsync, but fall back to software if needed.
  SDL_Renderer* renderer = SDL_CreateRenderer(window, -1,
                                              SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer) {
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  }
  if (!renderer) {
    if (error) *error = std::string("SDL_CreateRenderer failed: ") + SDL_GetError();
    SDL_DestroyWindow(window);
    return false;
  }

  out.backend = nebula4x::ui::UIRendererBackend::SDLRenderer2;
  out.window = window;
  out.sdl_renderer = renderer;
  out.gl_context = nullptr;
  return true;
}

#if NEBULA4X_UI_RENDERER_OPENGL2
struct GLAttempt {
  int major{0};
  int minor{0};
  int depth{24};
  int stencil{8};
  const char* label{"(unknown)"};
};

static bool create_opengl2_window(RendererInitResult& out, Uint32 base_window_flags, int width, int height,
                                  const GLAttempt& attempt, std::string* error) {
  SDL_GL_ResetAttributes();

  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, attempt.depth);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, attempt.stencil);

  // Request a specific GL version if provided, otherwise let SDL choose.
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, attempt.major);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, attempt.minor);

  // For legacy OpenGL, let SDL pick the profile. Setting 0 means "don't care".
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, 0);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);

  SDL_Window* window =
      SDL_CreateWindow("Nebula4X", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height,
                       base_window_flags | SDL_WINDOW_OPENGL);
  if (!window) {
    if (error) {
      *error = std::string("SDL_CreateWindow(OpenGL) failed (") + attempt.label + "): " + SDL_GetError();
    }
    return false;
  }

  SDL_GLContext gl_context = SDL_GL_CreateContext(window);
  if (!gl_context) {
    if (error) {
      *error = std::string("SDL_GL_CreateContext failed (") + attempt.label + "): " + SDL_GetError();
    }
    SDL_DestroyWindow(window);
    return false;
  }

  SDL_GL_MakeCurrent(window, gl_context);
  SDL_GL_SetSwapInterval(1);

  out.backend = nebula4x::ui::UIRendererBackend::OpenGL2;
  out.window = window;
  out.gl_context = gl_context;
  out.sdl_renderer = nullptr;

  // Capture OpenGL driver strings for diagnostics (best effort).
  auto to_string = [](const GLubyte* s) -> std::string {
    return s ? std::string(reinterpret_cast<const char*>(s)) : std::string();
  };
  out.gl_vendor = to_string(glGetString(GL_VENDOR));
  out.gl_renderer = to_string(glGetString(GL_RENDERER));
  out.gl_version = to_string(glGetString(GL_VERSION));
#ifdef GL_SHADING_LANGUAGE_VERSION
  out.glsl_version = to_string(glGetString(GL_SHADING_LANGUAGE_VERSION));
#endif

  return true;
}
#endif

}  // namespace

int main(int argc, char** argv) {
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0) {
    std::fprintf(stderr, "Error: %s\n", SDL_GetError());
    return 1;
  }

  const RendererRequest request = parse_renderer_request(argc, argv);

  const Uint32 base_window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
  const int window_width = 1280;
  const int window_height = 720;

  RendererInitResult renderer{};
  std::string opengl_error_log;

  // --- Try OpenGL2 first (if available and requested) ---
  bool opengl_attempted = false;
  bool opengl_ok = false;

#if NEBULA4X_UI_RENDERER_OPENGL2
  if (request != RendererRequest::SDLRenderer2) {
    opengl_attempted = true;

    const std::vector<GLAttempt> attempts = {
        // Legacy contexts: request 2.2/2.1 first, then fall back to "let SDL decide".
        {2, 2, 24, 8, "OpenGL 2.2 (24D/8S)"},
        {2, 1, 24, 8, "OpenGL 2.1 (24D/8S)"},
        {2, 0, 24, 8, "OpenGL 2.0 (24D/8S)"},
        {0, 0, 24, 8, "Default OpenGL (24D/8S)"},
        {0, 0, 16, 0, "Default OpenGL (16D/0S)"},
    };

    std::string last_error;
    for (const auto& attempt : attempts) {
      if (create_opengl2_window(renderer, base_window_flags, window_width, window_height, attempt,
                                &last_error)) {
        opengl_ok = true;
        break;
      }
      opengl_error_log += last_error + "\n";
    }
  }
#else
  if (request == RendererRequest::OpenGL2) {
    renderer.used_fallback = true;
    renderer.fallback_reason =
        "This build was compiled without OpenGL2 support. Falling back to SDL_Renderer2.\n"
        "Reconfigure with -DNEBULA4X_UI_USE_OPENGL2=ON.";
  }
#endif

  // --- Fallback to SDL_Renderer2 ---
  if (!opengl_ok) {
    if (request == RendererRequest::OpenGL2) {
      renderer.used_fallback = true;
      renderer.fallback_reason = "OpenGL renderer was requested but OpenGL context creation failed.\n" +
                                 opengl_error_log;
    } else if (request == RendererRequest::Auto && opengl_attempted) {
      renderer.used_fallback = true;
      renderer.fallback_reason =
          "OpenGL context creation failed; Nebula4X started in SDL_Renderer2 safe mode.\n" +
          opengl_error_log;
    }

    std::string sdl_renderer_error;
    if (!create_sdlrenderer_window(renderer, base_window_flags, window_width, window_height,
                                   &sdl_renderer_error)) {
      std::fprintf(stderr, "Error: %s\n", sdl_renderer_error.c_str());
      SDL_Quit();
      return 1;
    }

    if (renderer.used_fallback) {
      std::fprintf(stderr, "[ui] %s\n", renderer.fallback_reason.c_str());
    }
  }

  // Setup Dear ImGui context.
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();

  // Enable Keyboard Controls
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  // Enable Docking
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

#ifdef IMGUI_HAS_VIEWPORT
  // Viewports require a backend that can create/render platform windows.
  if (renderer.backend == nebula4x::ui::UIRendererBackend::OpenGL2) {
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
  }
#endif

  // Setup Dear ImGui style
  ImGui::StyleColorsDark();

#ifdef IMGUI_HAS_VIEWPORT
  if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.Colors[ImGuiCol_WindowBg].w = 1.0f;
  }
#endif

  // Setup Platform/Renderer backends
  if (renderer.backend == nebula4x::ui::UIRendererBackend::OpenGL2) {
#if NEBULA4X_UI_RENDERER_OPENGL2
    ImGui_ImplSDL2_InitForOpenGL(renderer.window, renderer.gl_context);
    ImGui_ImplOpenGL2_Init();
#else
    // Should be unreachable: compiled without OpenGL2 support.
    ImGui_ImplSDL2_InitForSDLRenderer(renderer.window, renderer.sdl_renderer);
    ImGui_ImplSDLRenderer2_Init(renderer.sdl_renderer);
#endif
  } else {
    ImGui_ImplSDL2_InitForSDLRenderer(renderer.window, renderer.sdl_renderer);
    ImGui_ImplSDLRenderer2_Init(renderer.sdl_renderer);
  }

  // Load content and initial save.
  nebula4x::ContentDB db = nebula4x::ContentDB::load("content");

  nebula4x::State st;
  if (nebula4x::read_text_file("saves/save.json", &st.serialized_state)) {
    try {
      st = nebula4x::load_state_from_json(st.serialized_state, db);
    } catch (const std::exception&) {
      // ignore load failures; fall back to a fresh start.
    }
  }

  // Create simulation and UI app.
  nebula4x::Simulation sim(db, std::move(st));
  sim.initialize_star_system();
  nebula4x::ui::App app(std::move(sim));

  // Load UI prefs if present.
  app.load_ui_prefs("ui_prefs.json");

  // Propagate runtime renderer diagnostics into UIState (used for settings + startup popup).
  {
    auto& ui = app.ui_state();
    ui.runtime_renderer_backend = renderer.backend;
    ui.runtime_renderer_supports_viewports =
        (renderer.backend == nebula4x::ui::UIRendererBackend::OpenGL2);
    ui.runtime_renderer_used_fallback = renderer.used_fallback;
    ui.runtime_renderer_fallback_reason = renderer.fallback_reason;

    ui.runtime_opengl_vendor = renderer.gl_vendor;
    ui.runtime_opengl_renderer = renderer.gl_renderer;
    ui.runtime_opengl_version = renderer.gl_version;
    ui.runtime_opengl_glsl_version = renderer.glsl_version;

    if (!ui.runtime_renderer_supports_viewports) {
      ui.viewports_enable = false;
    }
    if (ui.runtime_renderer_used_fallback) {
      ui.show_graphics_safe_mode_popup = true;
      ui.graphics_safe_mode_popup_opened = false;
    }
  }

  // Allow UI subsystems to create renderer-owned resources.
  // (E.g. the procedural background engine uploads tiles as textures.)
  app.set_renderer_context(renderer.backend, renderer.sdl_renderer);

  // Set Dear ImGui ini (layout) filename for this session (derived from UI prefs).
  io.IniFilename = app.imgui_ini_filename();

  // Main loop.
  bool done = false;
  while (!done) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL2_ProcessEvent(&event);
      if (event.type == SDL_QUIT) done = true;
      if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE &&
          event.window.windowID == SDL_GetWindowID(renderer.window)) {
        done = true;
      }
      app.on_event(event);
    }

    if (renderer.backend == nebula4x::ui::UIRendererBackend::OpenGL2) {
#if NEBULA4X_UI_RENDERER_OPENGL2
      ImGui_ImplOpenGL2_NewFrame();
#endif
    } else {
      ImGui_ImplSDLRenderer2_NewFrame();
    }
    ImGui_ImplSDL2_NewFrame();

    app.pre_frame();
    ImGui::NewFrame();

    app.frame();

    ImGui::Render();

    const float* cc = app.clear_color_rgba();

    if (renderer.backend == nebula4x::ui::UIRendererBackend::OpenGL2) {
#if NEBULA4X_UI_RENDERER_OPENGL2
      int display_w = 0, display_h = 0;
      SDL_GL_GetDrawableSize(renderer.window, &display_w, &display_h);
      glViewport(0, 0, display_w, display_h);
      glClearColor(cc[0], cc[1], cc[2], cc[3]);
      glClear(GL_COLOR_BUFFER_BIT);

      ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());

#ifdef IMGUI_HAS_VIEWPORT
      if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        SDL_Window* backup_current_window = SDL_GL_GetCurrentWindow();
        SDL_GLContext backup_current_context = SDL_GL_GetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        SDL_GL_MakeCurrent(backup_current_window, backup_current_context);
      }
#endif

      SDL_GL_SwapWindow(renderer.window);
#endif
    } else {
      // SDL_Renderer backend
      SDL_RenderSetScale(renderer.sdl_renderer, io.DisplayFramebufferScale.x,
                         io.DisplayFramebufferScale.y);
      const auto to_u8 = [](float v) -> Uint8 {
        v = std::clamp(v, 0.0f, 1.0f);
        return static_cast<Uint8>(v * 255.0f);
      };
      SDL_SetRenderDrawColor(renderer.sdl_renderer, to_u8(cc[0]), to_u8(cc[1]), to_u8(cc[2]),
                             to_u8(cc[3]));
      SDL_RenderClear(renderer.sdl_renderer);
      ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData());
      SDL_RenderPresent(renderer.sdl_renderer);
    }
  }

  // Shutdown
  // Ensure renderer-owned resources are released while their contexts are still alive.
  app.shutdown_renderer_resources();
  if (renderer.backend == nebula4x::ui::UIRendererBackend::OpenGL2) {
#if NEBULA4X_UI_RENDERER_OPENGL2
    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DeleteContext(renderer.gl_context);
    SDL_DestroyWindow(renderer.window);
#else
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer.sdl_renderer);
    SDL_DestroyWindow(renderer.window);
#endif
  } else {
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer.sdl_renderer);
    SDL_DestroyWindow(renderer.window);
  }

  SDL_Quit();
  return 0;
}