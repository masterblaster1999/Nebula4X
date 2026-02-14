cmake_minimum_required(VERSION 3.21)

project(Nebula4X VERSION 0.1.0 LANGUAGES CXX)

# Use CMAKE_MSVC_DEBUG_INFORMATION_FORMAT on supported CMake versions.
if(POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
endif()

option(NEBULA4X_BUILD_UI "Build the SDL2/ImGui UI" ON)
option(NEBULA4X_UI_USE_OPENGL2 "Use SDL2 + OpenGL2 renderer backend for ImGui (enables detachable tool windows via multi-viewports)" ON)
option(NEBULA4X_BUILD_TESTS "Build tests" ON)
option(NEBULA4X_REQUIRE_UI_DEPS "Fail configure when UI is requested but SDL2/ImGui dependencies are unavailable" OFF)
option(NEBULA4X_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(NEBULA4X_MSVC_PROGRESS_OUTPUT "Enable MSVC per-file timing output for long builds" ON)
option(NEBULA4X_SUPPRESS_CMAKE_REGEN "Disable automatic CMake regeneration during normal builds (faster iterative builds)" ON)
set(NEBULA4X_IMGUI_DRAW_INDEX_BITS "32" CACHE STRING
    "Dear ImGui draw index mode: 16, 32, or 64 (64 uses a virtual mode and keeps physical GPU indices at 32-bit)")
set_property(CACHE NEBULA4X_IMGUI_DRAW_INDEX_BITS PROPERTY STRINGS 16 32 64)
if(NOT NEBULA4X_IMGUI_DRAW_INDEX_BITS MATCHES "^(16|32|64)$")
  message(FATAL_ERROR
    "Invalid NEBULA4X_IMGUI_DRAW_INDEX_BITS='${NEBULA4X_IMGUI_DRAW_INDEX_BITS}'. Expected one of: 16, 32, 64.")
endif()

# 16-bit ImDrawIdx can overflow in dense UI scenes (e.g. large random galaxies)
# when the renderer backend cannot use VtxOffset across all draw paths.
# Force 32-bit for UI builds to avoid runtime assertions in imgui_draw.cpp.
if(NEBULA4X_BUILD_UI AND NEBULA4X_IMGUI_DRAW_INDEX_BITS STREQUAL "16")
  message(WARNING
    "NEBULA4X_IMGUI_DRAW_INDEX_BITS=16 is not safe for Nebula4X UI workloads; forcing 32.")
  set(NEBULA4X_IMGUI_DRAW_INDEX_BITS "32" CACHE STRING
      "Dear ImGui draw index mode: 16, 32, or 64 (64 uses a virtual mode and keeps physical GPU indices at 32-bit)"
      FORCE)
endif()

if(NEBULA4X_SUPPRESS_CMAKE_REGEN)
  # Avoid expensive "Re-running CMake..." steps on each build invocation.
  # Reconfigure manually when changing CMake files/options.
  set(CMAKE_SUPPRESS_REGENERATION ON)
endif()

# FetchContent is convenient for first-time builds, but it makes offline/headless
# builds brittle (CMake will fail at configure time if it cannot reach GitHub).
option(NEBULA4X_FETCH_DEPS "Allow FetchContent to download third-party dependencies (SDL2/ImGui)" OFF)
set(NEBULA4X_FETCH_PREFLIGHT_TIMEOUT_SECONDS "12" CACHE STRING
    "Timeout in seconds for git preflight checks before FetchContent dependency downloads")

option(NEBULA4X_ENABLE_ASAN "Enable AddressSanitizer (bug hunting)" OFF)
option(NEBULA4X_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer (bug hunting)" OFF)
option(NEBULA4X_ENABLE_TSAN "Enable ThreadSanitizer (bug hunting)" OFF)
option(NEBULA4X_ENABLE_LSAN "Enable LeakSanitizer (bug hunting)" OFF)
option(NEBULA4X_ENABLE_GLIBCXX_ASSERTIONS "Enable libstdc++ runtime assertions (_GLIBCXX_ASSERTIONS)" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  # Reduce lockups in Ninja/Makefile builds by avoiding shared PDB server writes
  # in Debug/RelWithDebInfo (embed debug info in .obj instead of /Zi PDB flow).
  if(CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
        "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
  endif()
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Warnings)

include(Sanitizers)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL
    "Avoid remote update checks for already-populated FetchContent dependencies" FORCE)

function(nebula4x_preseed_fetchcontent_source dep_name marker_relpath)
  # Reuse already-downloaded dependency sources from any existing build dir
  # (e.g. out/build/ui-vs, out/build/ui-runtime-*) to avoid slow/stuck
  # re-clone paths and Windows directory lock edge cases.
  string(TOLOWER "${dep_name}" _dep_lc)
  string(TOUPPER "${dep_name}" _dep_uc)

  set(_cands "${CMAKE_BINARY_DIR}/_deps/${_dep_lc}-src")
  if(EXISTS "${CMAKE_SOURCE_DIR}/out/build")
    file(GLOB _all_build_dirs LIST_DIRECTORIES TRUE "${CMAKE_SOURCE_DIR}/out/build/*")
    foreach(_bd IN LISTS _all_build_dirs)
      if(IS_DIRECTORY "${_bd}/_deps/${_dep_lc}-src")
        list(APPEND _cands "${_bd}/_deps/${_dep_lc}-src")
      endif()
    endforeach()
  endif()

  foreach(_cand IN LISTS _cands)
    if(EXISTS "${_cand}/${marker_relpath}")
      set("FETCHCONTENT_SOURCE_DIR_${_dep_uc}" "${_cand}" CACHE PATH
          "Preseeded local source directory for ${dep_name}" FORCE)
      message(STATUS "Preseed ${dep_name}: using local source at ${_cand}")
      return()
    endif()
  endforeach()
endfunction()

function(nebula4x_verify_fetch_remote dep_name dep_url)
  if(NOT NEBULA4X_FETCH_DEPS)
    return()
  endif()

  # If FetchContent already populated this dependency in the build tree, skip
  # network preflight checks. This keeps iterative reconfigure/build cycles fast
  # and avoids "stalled" rebuilds in offline or rate-limited environments.
  string(TOLOWER "${dep_name}" _dep_lc)
  string(TOUPPER "${dep_name}" _dep_uc)
  set(_local_dep_src "${CMAKE_BINARY_DIR}/_deps/${_dep_lc}-src")
  if(DEFINED FETCHCONTENT_SOURCE_DIR_${_dep_uc} AND
     IS_DIRECTORY "${FETCHCONTENT_SOURCE_DIR_${_dep_uc}}")
    set(_local_dep_src "${FETCHCONTENT_SOURCE_DIR_${_dep_uc}}")
  endif()
  set(_local_dep_present FALSE)
  if(EXISTS "${_local_dep_src}/CMakeLists.txt")
    set(_local_dep_present TRUE)
  elseif(dep_name STREQUAL "ImGui" AND EXISTS "${_local_dep_src}/imgui.h")
    # Dear ImGui upstream repository doesn't provide a top-level CMakeLists.txt.
    set(_local_dep_present TRUE)
  elseif(dep_name STREQUAL "SDL2" AND EXISTS "${_local_dep_src}/include/SDL.h")
    set(_local_dep_present TRUE)
  elseif(EXISTS "${_local_dep_src}/.git")
    # Fallback for other future deps populated by git clone.
    set(_local_dep_present TRUE)
  endif()

  if(_local_dep_present)
    message(STATUS "Dependency preflight skipped for ${dep_name}: using local source at ${_local_dep_src}")
    return()
  endif()

  if(NEBULA4X_FETCH_PREFLIGHT_TIMEOUT_SECONDS MATCHES "^[0-9]+$")
    set(_timeout "${NEBULA4X_FETCH_PREFLIGHT_TIMEOUT_SECONDS}")
  else()
    set(_timeout "12")
    message(WARNING "NEBULA4X_FETCH_PREFLIGHT_TIMEOUT_SECONDS='${NEBULA4X_FETCH_PREFLIGHT_TIMEOUT_SECONDS}' is invalid; using 12.")
  endif()

  find_package(Git QUIET)
  if(NOT GIT_FOUND)
    message(FATAL_ERROR
      "Git was not found, but NEBULA4X_FETCH_DEPS=ON requires git to download ${dep_name} (${dep_url}).")
  endif()

  execute_process(
    COMMAND "${GIT_EXECUTABLE}"
            -c credential.interactive=never
            -c core.askPass=
            ls-remote "${dep_url}" HEAD
    RESULT_VARIABLE _git_rc
    OUTPUT_QUIET
    ERROR_VARIABLE _git_err
    TIMEOUT "${_timeout}"
  )

  if(NOT _git_rc EQUAL 0)
    string(REPLACE "\n" " " _git_err_one_line "${_git_err}")
    message(FATAL_ERROR
      "Dependency preflight failed for ${dep_name} (${dep_url}) within timeout ${_timeout}s (git rc=${_git_rc}).\n"
      "Details: ${_git_err_one_line}\n"
      "Use a network-enabled environment, configure git access, or use locally installed UI dependencies.")
  endif()
endfunction()

# --- core library ---
add_library(nebula4x_core)
add_library(nebula4x::core ALIAS nebula4x_core)

target_sources(nebula4x_core
  PRIVATE
    src/core/content_validation.cpp
    src/core/date.cpp
    src/core/design_stats.cpp
    src/core/enum_strings.cpp
    src/core/fleet_formation.cpp
    src/core/game_state.cpp
    src/core/orders.cpp
    src/core/order_planner.cpp
    src/core/freight_planner.cpp
    src/core/fuel_planner.cpp
    src/core/maintenance_planner.cpp
    src/core/repair_planner.cpp
    src/core/trade_network.cpp
    src/core/security_planner.cpp
    src/core/invasion_planner.cpp
    src/core/salvage_planner.cpp
    src/core/mine_planner.cpp
    src/core/contract_planner.cpp
    src/core/troop_planner.cpp
    src/core/colonist_planner.cpp
    src/core/research_schedule.cpp
    src/core/terraforming_schedule.cpp
    src/core/colony_schedule.cpp
    src/core/colony_profiles.cpp
    src/core/region_planner.cpp
    src/core/ship_profiles.cpp
    src/core/planner_events.cpp
    src/core/ground_battle_forecast.cpp
    src/core/fleet_battle_forecast.cpp
    src/core/advisor.cpp
    src/core/research_planner.cpp
    src/core/scenario.cpp
    src/core/simulation.cpp
    src/core/simulation_state.cpp
    src/core/simulation_orders.cpp
    src/core/simulation_tick_contracts.cpp
    src/core/simulation_tick_procgen.cpp
    src/core/simulation_tick_core.cpp
    src/core/simulation_tick_economy.cpp
    src/core/simulation_tick_ai.cpp
    src/core/simulation_sensors.cpp
    src/core/simulation_subsystems.cpp
    src/core/simulation_tick_heat.cpp
    src/core/simulation_tick_ships.cpp
    src/core/simulation_tick_combat.cpp
    src/core/simulation_tick_ground_combat.cpp
    src/core/simulation_tick_terraforming.cpp
    src/core/procgen_design_forge.cpp
    src/core/ai_economy.cpp
    src/core/serialization.cpp
    src/core/tech.cpp
    src/core/state_validation.cpp
    src/util/file_io.cpp
    src/util/event_export.cpp
    src/util/json.cpp
    src/util/log.cpp
    src/util/state_export.cpp
    src/util/strings.cpp
    src/util/save_diff.cpp
    src/util/save_merge.cpp
    src/util/save_delta.cpp
    src/util/json_merge_patch.cpp
    src/util/json_pointer.cpp
    src/util/json_pointer_autocomplete.cpp
    src/util/digest.cpp
    src/util/timeline_export.cpp
    src/util/tech_export.cpp
    src/util/duel_simulator.cpp
    src/util/duel_tournament.cpp
    src/util/duel_swiss_tournament.cpp
    src/util/trace_events.cpp
    src/util/autosave.cpp
    src/util/regression_tape.cpp
)

target_include_directories(nebula4x_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(nebula4x_core
  PUBLIC
)

target_compile_definitions(nebula4x_core
  PUBLIC
    NEBULA4X_VERSION="${PROJECT_VERSION}"
    NEBULA4X_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

nebula4x_set_project_warnings(nebula4x_core ${NEBULA4X_WARNINGS_AS_ERRORS})
nebula4x_enable_sanitizers(nebula4x_core)

# --- CLI executable ---
add_executable(nebula4x_cli src/cli_main.cpp)
target_link_libraries(nebula4x_cli PRIVATE nebula4x::core)
nebula4x_set_project_warnings(nebula4x_cli ${NEBULA4X_WARNINGS_AS_ERRORS})
nebula4x_enable_sanitizers(nebula4x_cli)

if(MSVC)
  # Run/debug CLI from repo root so relative data paths are stable.
  set_property(TARGET nebula4x_cli PROPERTY
    VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# Copy data directory next to the CLI executable for convenience.
add_custom_command(TARGET nebula4x_cli POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/data
    $<TARGET_FILE_DIR:nebula4x_cli>/data
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/data
    ${CMAKE_BINARY_DIR}/data
)

# --- UI executable ---
if(NEBULA4X_BUILD_UI)
  # UI deps are typically downloaded via FetchContent, but that may be
  # undesired (or impossible) for offline/headless builds.
  set(NEBULA4X_UI_ENABLED TRUE)
  set(NEBULA4X_UI_DISABLE_REASON "")

  # SDL2: try system first, then (optionally) FetchContent.
  find_package(SDL2 QUIET)
  if(NOT SDL2_FOUND)
    if(NEBULA4X_FETCH_DEPS)
      message(STATUS "SDL2 not found via find_package; fetching via FetchContent")
      nebula4x_preseed_fetchcontent_source("SDL2" "include/SDL.h")
      nebula4x_verify_fetch_remote("SDL2" "https://github.com/libsdl-org/SDL.git")
      FetchContent_Declare(
        SDL2
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-2.30.10
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
        UPDATE_DISCONNECTED ${FETCHCONTENT_UPDATES_DISCONNECTED}
      )
      set(SDL_SHARED OFF CACHE BOOL "" FORCE)
      set(SDL_STATIC ON CACHE BOOL "" FORCE)
      FetchContent_MakeAvailable(SDL2)
      # SDL2 CMake exports SDL2::SDL2-static
    else()
      if(NEBULA4X_REQUIRE_UI_DEPS)
        message(FATAL_ERROR "SDL2 not found and NEBULA4X_FETCH_DEPS=OFF while NEBULA4X_REQUIRE_UI_DEPS=ON.\n"
                            "  - Install SDL2 dev packages (or use a toolchain like vcpkg)\n"
                            "  - Or set NEBULA4X_FETCH_DEPS=ON (requires network access)")
      else()
        message(WARNING "SDL2 not found and NEBULA4X_FETCH_DEPS=OFF; disabling UI target.\n"
                        "  - To build headless: -DNEBULA4X_BUILD_UI=OFF\n"
                        "  - To build UI: install SDL2 dev packages or set -DNEBULA4X_FETCH_DEPS=ON")
        set(NEBULA4X_UI_ENABLED FALSE)
        set(NEBULA4X_UI_DISABLE_REASON "SDL2 dependency is unavailable and NEBULA4X_FETCH_DEPS=OFF.")
      endif()
    endif()
  endif()

  if(NEBULA4X_UI_ENABLED)
    if(NEBULA4X_FETCH_DEPS)
      nebula4x_preseed_fetchcontent_source("imgui" "imgui.h")
      # Optional: use SDL2+OpenGL2 backend for ImGui (enables Multi-Viewport detachable windows).
if(NEBULA4X_UI_ENABLED AND NEBULA4X_UI_USE_OPENGL2)
  find_package(OpenGL QUIET)
  if(NOT OpenGL_FOUND)
    message(WARNING "OpenGL not found; falling back to SDL_Renderer2 backend for ImGui. Detachable viewports will be disabled. (Install OpenGL dev libraries or configure -DNEBULA4X_UI_USE_OPENGL2=OFF)")
    set(NEBULA4X_UI_USE_OPENGL2 OFF)
  endif()
endif()

FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        # Docking and viewport APIs live on the upstream 'docking' branch.
        # Use the matching release tag to keep versioning stable.
        GIT_TAG v1.91.4-docking
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
        UPDATE_DISCONNECTED ${FETCHCONTENT_UPDATES_DISCONNECTED}
      )
      nebula4x_verify_fetch_remote("ImGui" "https://github.com/ocornut/imgui.git")
      FetchContent_MakeAvailable(imgui)
    else()
      if(NEBULA4X_REQUIRE_UI_DEPS)
        message(FATAL_ERROR "ImGui dependency is unavailable and NEBULA4X_FETCH_DEPS=OFF while NEBULA4X_REQUIRE_UI_DEPS=ON.\n"
                            "  - Set NEBULA4X_FETCH_DEPS=ON to fetch ImGui (requires network access)")
      else()
        message(WARNING "ImGui is fetched via FetchContent but NEBULA4X_FETCH_DEPS=OFF; disabling UI target.")
        set(NEBULA4X_UI_ENABLED FALSE)
        set(NEBULA4X_UI_DISABLE_REASON "ImGui dependency is unavailable and NEBULA4X_FETCH_DEPS=OFF.")
      endif()
    endif()
  endif()

  if(NEBULA4X_UI_ENABLED)

    add_library(imgui_sdl)
  target_sources(imgui_sdl PRIVATE
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    # Always compile the SDL_Renderer2 backend so the app can fall back at
    # runtime if an OpenGL context cannot be created on the user's machine.
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer2.cpp
  )
  if(NEBULA4X_UI_USE_OPENGL2)
    target_sources(imgui_sdl PRIVATE
      ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl2.cpp
    )
    target_compile_definitions(imgui_sdl PUBLIC NEBULA4X_UI_RENDERER_OPENGL2=1)
    target_link_libraries(imgui_sdl PUBLIC OpenGL::GL)
  else()
    target_compile_definitions(imgui_sdl PUBLIC NEBULA4X_UI_RENDERER_OPENGL2=0)
  endif()
  target_compile_definitions(imgui_sdl PUBLIC
    NEBULA4X_IMGUI_DRAW_INDEX_BITS=${NEBULA4X_IMGUI_DRAW_INDEX_BITS})
  if(NEBULA4X_IMGUI_DRAW_INDEX_BITS STREQUAL "64")
    # Rendering backends used by Nebula4X support up to 32-bit hardware index types.
    # "64" mode enables a virtual wide-index path in project code while preserving
    # backend-compatible 32-bit physical indices.
    target_compile_definitions(imgui_sdl PUBLIC NEBULA4X_IMGUI_DRAW_INDEX_VIRTUAL_64=1)
  endif()
  # Route Dear ImGui compile-time configuration through a project-owned header.
  # This keeps third-party sources unmodified while allowing upgraded index width,
  # quality, and performance tuning in one place.
  target_compile_definitions(imgui_sdl PUBLIC IMGUI_USER_CONFIG=\"imgui_user_config.h\")
  target_include_directories(imgui_sdl PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  if(SDL2_FOUND)
    set(NEBULA4X_SDL_TARGET SDL2::SDL2)
  else()
    # when fetched
    set(NEBULA4X_SDL_TARGET SDL2::SDL2-static)
  endif()
  # imgui_sdl is a static library: it only needs SDL headers to compile.
  # Keeping SDL as INTERFACE avoids pulling large SDL rebuilds when users build
  # only the imgui_sdl target for iteration.
  target_include_directories(imgui_sdl PRIVATE
    $<TARGET_PROPERTY:${NEBULA4X_SDL_TARGET},INTERFACE_INCLUDE_DIRECTORIES>
  )
  target_link_libraries(imgui_sdl INTERFACE ${NEBULA4X_SDL_TARGET})

  nebula4x_set_project_warnings(imgui_sdl ${NEBULA4X_WARNINGS_AS_ERRORS})
  nebula4x_enable_sanitizers(imgui_sdl)

    add_executable(nebula4x
    src/main.cpp
    src/ui/app.cpp
    src/ui/procedural_theme.cpp
    src/ui/procedural_layout.cpp
    src/ui/new_game_modal.cpp
    src/ui/hud.cpp
    src/ui/ui_commands.cpp
    src/ui/order_ui.cpp
    src/ui/order_template_portable.cpp
    src/ui/order_plan_ui.cpp
    src/ui/fleet_plan_ui.cpp
    src/ui/navigation.cpp
    src/ui/navigator_window.cpp
    src/ui/map_render.cpp
    src/ui/proc_render_engine.cpp
    src/ui/proc_body_sprite_engine.cpp
    src/ui/proc_icon_sprite_engine.cpp
    src/ui/proc_jump_phenomena_sprite_engine.cpp
    src/ui/proc_anomaly_phenomena_sprite_engine.cpp
    src/ui/proc_trail_engine.cpp
  src/ui/proc_flow_field_engine.cpp
  src/ui/proc_gravity_contour_engine.cpp
  src/ui/proc_particle_field_engine.cpp
  src/ui/proc_territory_field_engine.cpp
    src/ui/raymarch_nebula.cpp
    src/ui/raytrace_sensor_heatmap.cpp
    src/ui/map_label_placer.cpp
    src/ui/panels.cpp
    src/ui/economy_window.cpp
    src/ui/research_roadmap_window.cpp
    src/ui/planner_window.cpp
    src/ui/regions_window.cpp
    src/ui/security_planner_window.cpp
    src/ui/freight_window.cpp
	    src/ui/mine_window.cpp
    src/ui/fuel_window.cpp
    src/ui/salvage_window.cpp
    src/ui/contracts_window.cpp
    src/ui/sustainment_window.cpp
    src/ui/repair_planner_window.cpp
    src/ui/maintenance_planner_window.cpp
    src/ui/fleet_manager_window.cpp
    src/ui/troop_window.cpp
    src/ui/colonist_window.cpp
    src/ui/terraforming_window.cpp
    src/ui/advisor_window.cpp
    src/ui/colony_profiles_window.cpp
  src/ui/ship_profiles_window.cpp
  src/ui/automation_center_window.cpp
  src/ui/shipyard_targets_window.cpp
  src/ui/survey_network_window.cpp
    src/ui/time_warp_window.cpp
    src/ui/save_tools_window.cpp
    src/ui/time_machine_window.cpp
    src/ui/compare_window.cpp
    src/ui/game_json_cache.cpp
  src/ui/game_entity_index.cpp
  src/ui/json_watch_eval.cpp
    src/ui/omni_search_window.cpp
    src/ui/json_explorer_window.cpp
    src/ui/content_validation_window.cpp
    src/ui/state_doctor_window.cpp
	    src/ui/trace_viewer_window.cpp
  src/ui/entity_inspector_window.cpp
  src/ui/reference_graph_window.cpp
    src/ui/watchboard_window.cpp
  src/ui/watchboard_alerts.cpp
    src/ui/data_lenses_window.cpp
    src/ui/dashboards_window.cpp
    src/ui/docs_browser.cpp
    src/ui/hotkeys.cpp
    src/ui/hotkeys_window.cpp
    src/ui/notifications.cpp
    src/ui/notifications_window.cpp
    src/ui/guided_tour.cpp
    src/ui/pivot_tables_window.cpp
    src/ui/procgen_atlas_window.cpp
    src/ui/procgen_graphics.cpp
    src/ui/ui_forge_window.cpp
    src/ui/context_forge_window.cpp
    src/ui/window_management.cpp
    src/ui/window_manager_window.cpp
    src/ui/ui_forge_dna.cpp
    src/ui/screen_reader.cpp
    src/ui/layout_profiles.cpp
    src/ui/layout_profiles_window.cpp
    src/ui/workspace_presets.cpp
    src/ui/production_window.cpp
    src/ui/galaxy_map.cpp
    src/ui/galaxy_constellations.cpp
    src/ui/star_atlas_window.cpp
    src/ui/system_map.cpp
    src/ui/timeline_window.cpp
    src/ui/design_studio_window.cpp
    src/ui/balance_lab_window.cpp
    src/ui/battle_forecast_window.cpp
    src/ui/intel_window.cpp
    src/ui/intel_notebook_window.cpp
    src/ui/diplomacy_window.cpp
    src/ui/victory_window.cpp
  )
  target_include_directories(nebula4x PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
  )
  target_link_libraries(nebula4x PRIVATE nebula4x::core imgui_sdl)

  if(WIN32)
    # Keep a native main() entry point and avoid SDL_main redirection on Windows.
    target_compile_definitions(nebula4x PRIVATE SDL_MAIN_HANDLED)
  endif()

  if(WIN32)
    # Screen reader (SAPI COM) uses CoInitializeEx / CLSIDFromProgID.
    target_link_libraries(nebula4x PRIVATE ole32)
  endif()

  if(SDL2_FOUND)
    target_link_libraries(nebula4x PRIVATE SDL2::SDL2)
  else()
    target_link_libraries(nebula4x PRIVATE SDL2::SDL2-static)
  endif()

  nebula4x_set_project_warnings(nebula4x ${NEBULA4X_WARNINGS_AS_ERRORS})
  nebula4x_enable_sanitizers(nebula4x)

  if(MSVC)
    # Run/debug UI from repo root so relative data/save paths are stable.
    set_property(TARGET nebula4x PROPERTY
      VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  endif()

  # Copy data directory next to executable for convenience.
  add_custom_command(TARGET nebula4x POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      ${CMAKE_CURRENT_SOURCE_DIR}/data
      $<TARGET_FILE_DIR:nebula4x>/data
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      ${CMAKE_CURRENT_SOURCE_DIR}/data
      ${CMAKE_BINARY_DIR}/data
  )
  if(WIN32)
    # Stage a top-level launcher binary for quick local starts.
    add_custom_command(TARGET nebula4x POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:nebula4x>
        ${CMAKE_CURRENT_SOURCE_DIR}/nebula4x.exe
    )
  endif()
  else()
    if(NOT NEBULA4X_UI_DISABLE_REASON)
      set(NEBULA4X_UI_DISABLE_REASON "UI dependencies are unavailable in this configuration.")
    endif()

    message(STATUS "Building fallback 'nebula4x' launcher (UI disabled): ${NEBULA4X_UI_DISABLE_REASON}")

    add_executable(nebula4x src/ui_unavailable_main.cpp)
    target_compile_definitions(nebula4x PRIVATE
      NEBULA4X_VERSION="${PROJECT_VERSION}"
      NEBULA4X_UI_UNAVAILABLE_REASON="${NEBULA4X_UI_DISABLE_REASON}"
    )
    nebula4x_set_project_warnings(nebula4x ${NEBULA4X_WARNINGS_AS_ERRORS})
    nebula4x_enable_sanitizers(nebula4x)

    if(MSVC)
      set_property(TARGET nebula4x PROPERTY
        VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endif()

    add_custom_command(TARGET nebula4x POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/data
        $<TARGET_FILE_DIR:nebula4x>/data
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/data
        ${CMAKE_BINARY_DIR}/data
    )
    if(WIN32)
      add_custom_command(TARGET nebula4x POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:nebula4x>
          ${CMAKE_CURRENT_SOURCE_DIR}/nebula4x.exe
      )
    endif()
  endif() # NEBULA4X_UI_ENABLED
endif()

if(MSVC)
  # In Visual Studio, default F5 to the interactive game UI when available.
  if(NEBULA4X_BUILD_UI AND TARGET nebula4x)
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      PROPERTY VS_STARTUP_PROJECT nebula4x)
  else()
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      PROPERTY VS_STARTUP_PROJECT nebula4x_cli)
  endif()
endif()

# --- tests ---
if(NEBULA4X_BUILD_TESTS)
  include(CTest)
  add_executable(nebula4x_tests
    tests/test_main.cpp
    tests/test_date.cpp
    tests/test_simulation.cpp
    tests/test_ground_ops.cpp
    tests/test_ground_battle_forecast.cpp
        tests/test_fleet_battle_forecast.cpp
    tests/test_boarding.cpp
    tests/test_colonization.cpp
    tests/test_serialization.cpp
    tests/test_auto_freight.cpp
    tests/test_industry.cpp
    tests/test_ai_economy.cpp
    tests/test_ai_research_plan.cpp
    tests/test_ai_empire_fleet_missions.cpp
    tests/test_victory.cpp
    tests/test_research_planner.cpp
    tests/test_research_schedule.cpp
    tests/test_colony_schedule.cpp
    tests/test_colony_profiles.cpp
    tests/test_planner_events.cpp
    tests/test_time_warp.cpp
    tests/test_contact_prediction.cpp
    tests/test_intercept.cpp
    tests/test_duel_simulator.cpp
    tests/test_duel_tournament.cpp
    tests/test_duel_swiss_tournament.cpp
    tests/test_attack_lead_pursuit.cpp
    tests/test_lost_contact_search.cpp
    tests/test_combat_doctrine.cpp
    tests/test_sensor_coverage.cpp
    tests/test_swept_contacts.cpp
    tests/test_freight_planner.cpp
	    tests/test_freight_planner_partial_cargo.cpp
    tests/test_trade_network.cpp
    tests/test_civilian_trade_activity_prosperity.cpp
    tests/test_security_planner.cpp
    tests/test_invasion_planner.cpp
    tests/test_repair_planner.cpp
    tests/test_maintenance_planner.cpp
    tests/test_fuel_planner.cpp
    tests/test_mineral_deposits.cpp
    tests/test_mining_scarcity_allocation.cpp
    tests/test_mobile_mining.cpp
    tests/test_auto_mine.cpp
    tests/test_power_system.cpp
    tests/test_digests.cpp
    tests/test_faction_economy_modifiers.cpp
    tests/test_refit.cpp
    tests/test_diplomacy.cpp
    tests/test_piracy_suppression.cpp
    tests/test_auto_routing.cpp
    tests/test_jump_route_env_cost.cpp
    tests/test_auto_explore.cpp
    tests/test_order_repeat.cpp
    tests/test_order_planner.cpp
    tests/test_determinism.cpp
    tests/test_event_export.cpp
    tests/test_content_validation.cpp
    tests/test_resource_catalog.cpp
    tests/test_materials_processing.cpp
    tests/test_content_overlays.cpp
    tests/test_content_hot_reload.cpp
    tests/test_spatial_index.cpp
    tests/test_random_scenario.cpp
    tests/test_autosave.cpp
    tests/test_file_io.cpp
    tests/test_json_unicode.cpp
    tests/test_json_bom.cpp
    tests/test_json_errors.cpp
    tests/test_json_merge_patch.cpp
    tests/test_json_pointer.cpp
    tests/test_json_pointer_autocomplete.cpp
    tests/test_json_pointer_glob.cpp
    tests/test_trace_events.cpp
    tests/test_state_validation.cpp
    tests/test_save_diff.cpp
    tests/test_save_merge.cpp
    tests/test_save_delta.cpp
    tests/test_regression_tape.cpp
    tests/test_state_export.cpp
    tests/test_fleets.cpp
    tests/test_combat_events.cpp
    tests/test_planetary_point_defense.cpp
    tests/test_shields.cpp
    tests/test_turn_ticks.cpp
    tests/test_population_growth.cpp
    tests/test_population_transport.cpp
    tests/test_auto_colonize.cpp
    tests/test_auto_salvage.cpp
    tests/test_reverse_engineering.cpp
    tests/test_anomalies.cpp
    tests/test_anomaly_discovery.cpp
    tests/test_procgen_surface.cpp
    tests/test_design_forge_constraints.cpp
    tests/test_nebula_microfields.cpp
    tests/test_nebula_storm_cells.cpp
    tests/test_jump_transit_hazards.cpp
    tests/test_dynamic_poi_spawns.cpp
    tests/test_missile_components.cpp
    tests/test_auto_tanker.cpp
  tests/test_auto_refuel.cpp
  tests/test_body_occlusion.cpp
    tests/test_ship_repairs.cpp
  tests/test_crew_experience.cpp
  tests/test_electronic_warfare.cpp
    tests/test_advisor.cpp
    tests/test_blockade_economy.cpp
  )
  target_link_libraries(nebula4x_tests PRIVATE nebula4x::core)
  target_include_directories(nebula4x_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  nebula4x_set_project_warnings(nebula4x_tests ${NEBULA4X_WARNINGS_AS_ERRORS})
  nebula4x_enable_sanitizers(nebula4x_tests)
  set_target_properties(nebula4x_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}"
  )

  # Allow running tests directly from the build dir (IDE/manual) by staging
  # both runtime content and test fixture content next to the test executable.
  add_custom_command(TARGET nebula4x_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      ${CMAKE_CURRENT_SOURCE_DIR}/data
      $<TARGET_FILE_DIR:nebula4x_tests>/data
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/data
      $<TARGET_FILE_DIR:nebula4x_tests>/tests/data
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      ${CMAKE_CURRENT_SOURCE_DIR}/data
      ${CMAKE_BINARY_DIR}/data
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/data
      ${CMAKE_BINARY_DIR}/tests/data
  )
  if(WIN32)
    add_custom_command(TARGET nebula4x_tests POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:nebula4x_tests>
        ${CMAKE_CURRENT_SOURCE_DIR}/nebula4x_tests.exe
    )
  endif()

  if(MSVC)
    # Make "Run"/debug from Visual Studio resolve relative fixture paths.
    set_property(TARGET nebula4x_tests PROPERTY
      VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  endif()

  # Tests read data/... in a couple of places, so run from the repo root.
  add_test(NAME nebula4x_tests COMMAND nebula4x_tests)
  set_tests_properties(nebula4x_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

  add_custom_target(nebula4x_tests_smoke
    COMMAND $<TARGET_FILE:nebula4x_tests> --exact simulation --verbose
    COMMAND $<TARGET_FILE:nebula4x_tests> --exact random_scenario --verbose
    COMMAND $<TARGET_FILE:nebula4x_tests> --exact sensor_coverage --verbose
    DEPENDS nebula4x_tests
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Run focused smoke tests for combat/procgen/sensors"
  )
endif()

# --- aggregate build target for main runtime dependency wiring ---
# Builds the main executable and, when tests are enabled in the current configure,
# also builds nebula4x_tests. This gives a single target for compile-time
# integration checks.
if(TARGET nebula4x)
  add_custom_target(nebula4x_main_all
    COMMENT "Build nebula4x and all configured compile-time checks")
  add_dependencies(nebula4x_main_all nebula4x)
  if(TARGET nebula4x_cli)
    add_dependencies(nebula4x_main_all nebula4x_cli)
  endif()
  if(TARGET nebula4x_tests)
    add_dependencies(nebula4x_main_all nebula4x_tests)
  endif()
endif()
