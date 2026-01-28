#pragma once

#include <cstdint>
#include <type_traits>

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
constexpr ImTextureID imgui_null_texture_id() {
  if constexpr (std::is_pointer_v<ImTextureID>) {
    return nullptr;
  } else {
    return static_cast<ImTextureID>(0);
  }
}

// Returns true if id is non-null.
constexpr bool imgui_texture_id_is_valid(ImTextureID id) {
  if constexpr (std::is_pointer_v<ImTextureID>) {
    return id != nullptr;
  } else {
    return id != static_cast<ImTextureID>(0);
  }
}

// --- SDL_Renderer2 backend -------------------------------------------------

inline ImTextureID imgui_texture_id_from_sdl_texture(SDL_Texture* tex) {
  if (!tex) return imgui_null_texture_id();
  if constexpr (std::is_pointer_v<ImTextureID>) {
    return reinterpret_cast<ImTextureID>(tex);
  } else {
    return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(tex));
  }
}

inline SDL_Texture* sdl_texture_from_imgui_texture_id(ImTextureID id) {
  if (!imgui_texture_id_is_valid(id)) return nullptr;
  if constexpr (std::is_pointer_v<ImTextureID>) {
    return reinterpret_cast<SDL_Texture*>(id);
  } else {
    return reinterpret_cast<SDL_Texture*>(static_cast<std::uintptr_t>(id));
  }
}

// --- OpenGL2 backend -------------------------------------------------------
// We avoid including any GL headers here. Callers can use GLuint or any unsigned
// int-like type.

template <typename GLuintLike>
inline ImTextureID imgui_texture_id_from_gl_texture(GLuintLike tex) {
  if (tex == static_cast<GLuintLike>(0)) return imgui_null_texture_id();
  if constexpr (std::is_pointer_v<ImTextureID>) {
    return reinterpret_cast<ImTextureID>(static_cast<std::uintptr_t>(tex));
  } else {
    return static_cast<ImTextureID>(tex);
  }
}

template <typename GLuintLike>
inline GLuintLike gl_texture_from_imgui_texture_id(ImTextureID id) {
  if (!imgui_texture_id_is_valid(id)) return static_cast<GLuintLike>(0);
  if constexpr (std::is_pointer_v<ImTextureID>) {
    return static_cast<GLuintLike>(reinterpret_cast<std::uintptr_t>(id));
  } else {
    return static_cast<GLuintLike>(id);
  }
}

} // namespace nebula4x::ui
