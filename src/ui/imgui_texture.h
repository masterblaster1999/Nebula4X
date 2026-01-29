#pragma once

#include <cstdint>

#include <imgui.h>

// NOTE:
// Dear ImGui's ImTextureID type is backend-defined.
// - Historically it was `void*`.
// - Newer ImGui versions default to an integer type (e.g. ImU64) unless overridden.
//
// This project supports multiple renderer backends (SDL_Renderer2 and OpenGL2) and
// needs to safely store/convert backend texture handles into ImTextureID.
//
// The helper functions below centralize those conversions so the rest of the UI
// code can be agnostic to the concrete ImTextureID definition.

// Forward declaration to avoid pulling SDL headers into every translation unit.
struct SDL_Texture;

namespace nebula4x::ui {

// Returns the canonical "null" texture id.
//
// We intentionally avoid comparing against nullptr directly because modern Dear
// ImGui defaults ImTextureID to an opaque 64-bit integer type (ImU64). In that
// configuration, comparing ImTextureID to nullptr is ill-formed on MSVC.
//
// Value-initialization works for both pointer and integer ImTextureID.
constexpr ImTextureID imgui_null_texture_id() { return ImTextureID{}; }

// Returns true if id is non-null.
constexpr bool imgui_texture_id_is_valid(ImTextureID id) { return id != ImTextureID{}; }

// --- SDL_Renderer2 backend -------------------------------------------------

inline ImTextureID imgui_texture_id_from_sdl_texture(SDL_Texture* tex) {
  // Cast through an integer that can hold a pointer.
  // This is compatible with both pointer-based and integer-based ImTextureID.
  return tex ? (ImTextureID)(std::uintptr_t)tex : imgui_null_texture_id();
}

inline SDL_Texture* sdl_texture_from_imgui_texture_id(ImTextureID id) {
  if (!imgui_texture_id_is_valid(id)) return nullptr;
  return (SDL_Texture*)(std::uintptr_t)id;
}

// --- OpenGL2 backend -------------------------------------------------------
// We avoid including any GL headers here. Callers can use GLuint or any unsigned
// int-like type.

template <typename GLuintLike>
inline ImTextureID imgui_texture_id_from_gl_texture(GLuintLike tex) {
  if (tex == static_cast<GLuintLike>(0)) return imgui_null_texture_id();
  return (ImTextureID)(std::uintptr_t)tex;
}

template <typename GLuintLike>
inline GLuintLike gl_texture_from_imgui_texture_id(ImTextureID id) {
  if (!imgui_texture_id_is_valid(id)) return static_cast<GLuintLike>(0);
  return (GLuintLike)(std::uintptr_t)id;
}

} // namespace nebula4x::ui
