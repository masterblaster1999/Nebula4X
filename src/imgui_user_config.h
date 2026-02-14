#pragma once

// Nebula4X Dear ImGui user config.
//
// This file is injected via IMGUI_USER_CONFIG (see CMakeLists.txt) so every
// translation unit compiling Dear ImGui uses the same data layout/settings.
//
// Draw-index mode selection:
//   -DNEBULA4X_IMGUI_DRAW_INDEX_BITS=16  -> ImDrawIdx = uint16
//   -DNEBULA4X_IMGUI_DRAW_INDEX_BITS=32  -> ImDrawIdx = uint32 (recommended)
//   -DNEBULA4X_IMGUI_DRAW_INDEX_BITS=64  -> virtual-64 mode:
//      backends remain on 32-bit physical indices for GPU/API compatibility.
//      (OpenGL2/SDL_Renderer2 paths used here don't support 64-bit index types.)

#ifndef NEBULA4X_IMGUI_DRAW_INDEX_BITS
#define NEBULA4X_IMGUI_DRAW_INDEX_BITS 32
#endif

#ifndef ImDrawIdx
#if NEBULA4X_IMGUI_DRAW_INDEX_BITS == 16
#define ImDrawIdx unsigned short
#elif NEBULA4X_IMGUI_DRAW_INDEX_BITS == 32
#define ImDrawIdx unsigned int
#elif NEBULA4X_IMGUI_DRAW_INDEX_BITS == 64
#define ImDrawIdx unsigned int
#ifndef NEBULA4X_IMGUI_DRAW_INDEX_VIRTUAL_64
#define NEBULA4X_IMGUI_DRAW_INDEX_VIRTUAL_64 1
#endif
#else
#error "NEBULA4X_IMGUI_DRAW_INDEX_BITS must be one of: 16, 32, 64."
#endif
#endif

// Enable stb_sprintf-backed formatting in ImGui.
// Nebula4X provides a local stb_sprintf.h compatibility shim in src/.
#ifndef IMGUI_USE_STB_SPRINTF
#define IMGUI_USE_STB_SPRINTF
#endif

// Promote ImWchar to 32-bit code points for broader Unicode coverage in UI
// labels and localization pipelines.
#ifndef IMGUI_USE_WCHAR32
#define IMGUI_USE_WCHAR32
#endif

// NOTE:
// We intentionally do not force IMGUI_DISABLE_OBSOLETE_FUNCTIONS here.
// Nebula4X pulls in many UI translation units and backends; keeping obsolete
// shims enabled avoids surprise compile breaks when updating ImGui or local UI
// code that still references legacy API spellings.
