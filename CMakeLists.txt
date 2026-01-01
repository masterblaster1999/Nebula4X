cmake_minimum_required(VERSION 3.21)

project(Nebula4X VERSION 0.1.0 LANGUAGES CXX)

option(NEBULA4X_BUILD_UI "Build the SDL2/ImGui UI" ON)
option(NEBULA4X_BUILD_TESTS "Build tests" ON)
option(NEBULA4X_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Warnings)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# --- core library ---
add_library(nebula4x_core)
add_library(nebula4x::core ALIAS nebula4x_core)

target_sources(nebula4x_core
  PRIVATE
    src/core/content_validation.cpp
    src/core/date.cpp
    src/core/enum_strings.cpp
    src/core/fleet_formation.cpp
    src/core/game_state.cpp
    src/core/orders.cpp
    src/core/research_planner.cpp
    src/core/scenario.cpp
    src/core/simulation.cpp
    src/core/simulation_state.cpp
    src/core/simulation_orders.cpp
    src/core/simulation_tick_core.cpp
    src/core/simulation_tick_economy.cpp
    src/core/simulation_tick_ai.cpp
    src/core/simulation_sensors.cpp
    src/core/simulation_tick_ships.cpp
    src/core/simulation_tick_combat.cpp
    src/core/simulation_tick_ground_combat.cpp
    src/core/simulation_tick_terraforming.cpp
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
    src/util/digest.cpp
    src/util/timeline_export.cpp
    src/util/tech_export.cpp
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
)

nebula4x_set_project_warnings(nebula4x_core ${NEBULA4X_WARNINGS_AS_ERRORS})

# --- CLI executable ---
add_executable(nebula4x_cli src/cli_main.cpp)
target_link_libraries(nebula4x_cli PRIVATE nebula4x::core)
nebula4x_set_project_warnings(nebula4x_cli ${NEBULA4X_WARNINGS_AS_ERRORS})

# Copy data directory next to the CLI executable for convenience.
add_custom_command(TARGET nebula4x_cli POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/data
    $<TARGET_FILE_DIR:nebula4x_cli>/data
)

# --- UI executable ---
if(NEBULA4X_BUILD_UI)
  # SDL2: try system first, then FetchContent.
  find_package(SDL2 QUIET)
  if(NOT SDL2_FOUND)
    message(STATUS "SDL2 not found via find_package; fetching via FetchContent")
    FetchContent_Declare(
      SDL2
      GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
      GIT_TAG release-2.30.10
    )
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(SDL2)
    # SDL2 CMake exports SDL2::SDL2-static
  endif()

  FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    # Docking and viewport APIs live on the upstream 'docking' branch.
    # Use the matching release tag to keep versioning stable.
    GIT_TAG v1.91.4-docking
  )
  FetchContent_MakeAvailable(imgui)

  add_library(imgui_sdlrenderer2)
  target_sources(imgui_sdlrenderer2 PRIVATE
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer2.cpp
  )
  target_include_directories(imgui_sdlrenderer2 PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
  )
  if(SDL2_FOUND)
    target_link_libraries(imgui_sdlrenderer2 PUBLIC SDL2::SDL2)
  else()
    # when fetched
    target_link_libraries(imgui_sdlrenderer2 PUBLIC SDL2::SDL2-static)
  endif()

  add_executable(nebula4x
    src/main.cpp
    src/ui/app.cpp
    src/ui/hud.cpp
    src/ui/map_render.cpp
    src/ui/panels.cpp
    src/ui/economy_window.cpp
    src/ui/production_window.cpp
    src/ui/galaxy_map.cpp
    src/ui/system_map.cpp
    src/ui/timeline_window.cpp
    src/ui/design_studio_window.cpp
    src/ui/intel_window.cpp
    src/ui/diplomacy_window.cpp
  )
  target_include_directories(nebula4x PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
  )
  target_link_libraries(nebula4x PRIVATE nebula4x::core imgui_sdlrenderer2)

  if(SDL2_FOUND)
    target_link_libraries(nebula4x PRIVATE SDL2::SDL2)
  else()
    target_link_libraries(nebula4x PRIVATE SDL2::SDL2-static)
  endif()

  nebula4x_set_project_warnings(nebula4x ${NEBULA4X_WARNINGS_AS_ERRORS})

  # Copy data directory next to executable for convenience.
  add_custom_command(TARGET nebula4x POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      ${CMAKE_CURRENT_SOURCE_DIR}/data
      $<TARGET_FILE_DIR:nebula4x>/data
  )
endif()

# --- tests ---
if(NEBULA4X_BUILD_TESTS)
  include(CTest)
  add_executable(nebula4x_tests
    tests/test_main.cpp
    tests/test_date.cpp
    tests/test_simulation.cpp
    tests/test_ground_ops.cpp
    tests/test_boarding.cpp
    tests/test_colonization.cpp
    tests/test_serialization.cpp
    tests/test_auto_freight.cpp
    tests/test_industry.cpp
    tests/test_ai_economy.cpp
    tests/test_research_planner.cpp
    tests/test_mineral_deposits.cpp
    tests/test_power_system.cpp
    tests/test_digests.cpp
    tests/test_faction_economy_modifiers.cpp
    tests/test_refit.cpp
    tests/test_diplomacy.cpp
    tests/test_auto_routing.cpp
    tests/test_order_repeat.cpp
    tests/test_determinism.cpp
    tests/test_event_export.cpp
    tests/test_content_validation.cpp
    tests/test_content_overlays.cpp
    tests/test_spatial_index.cpp
    tests/test_random_scenario.cpp
    tests/test_file_io.cpp
    tests/test_json_unicode.cpp
    tests/test_json_bom.cpp
    tests/test_json_errors.cpp
    tests/test_state_validation.cpp
    tests/test_save_diff.cpp
    tests/test_state_export.cpp
    tests/test_fleets.cpp
    tests/test_combat_events.cpp
    tests/test_shields.cpp
    tests/test_population_growth.cpp
  tests/test_population_transport.cpp
    tests/test_ship_repairs.cpp
  )
  target_link_libraries(nebula4x_tests PRIVATE nebula4x::core)
  nebula4x_set_project_warnings(nebula4x_tests ${NEBULA4X_WARNINGS_AS_ERRORS})

  # Tests read data/... in a couple of places, so run from the repo root.
  add_test(NAME nebula4x_tests COMMAND nebula4x_tests)
  set_tests_properties(nebula4x_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()
