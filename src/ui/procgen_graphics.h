#pragma once

#include <cstdint>
#include <string>

#include <imgui.h>

#include "nebula4x/core/entities.h"

namespace nebula4x::ui::procgen_gfx {

// Parsed interior of a procgen_surface ASCII stamp.
// The generator includes borders; this grid contains only the interior cells.
struct SurfaceStampGrid {
  int w{0};
  int h{0};
  // Row-major, size = w*h.
  std::string cells;

  bool valid() const { return w > 0 && h > 0 && static_cast<int>(cells.size()) == w * h; }
  char at(int x, int y) const {
    return valid() ? cells[static_cast<std::size_t>(y * w + x)] : ' ';
  }
};

SurfaceStampGrid parse_surface_stamp_grid(const std::string& stamp);

// Cached parser for surface stamps keyed by a stable id (usually Body::id).
// Avoids reparsing the ASCII stamp every frame in immediate-mode UI.
const SurfaceStampGrid& cached_surface_stamp_grid(Id stable_id, const std::string& stamp);

// Clears the internal surface stamp cache.
// Useful if you switch scenarios or want to force a refresh during debugging.
void clear_surface_stamp_cache();

struct SurfacePalette {
  ImU32 ocean{0};
  ImU32 land{0};
  ImU32 hills{0};
  ImU32 mountain{0};
  ImU32 ice{0};
  ImU32 desert{0};

  ImU32 bright{0};
  ImU32 mid{0};
  ImU32 dark{0};
  ImU32 storm{0};

  ImU32 rock{0};
  ImU32 regolith{0};
  ImU32 crater{0};

  ImU32 star_hot{0};
  ImU32 star_mid{0};
  ImU32 star_cool{0};

  ImU32 empty{0};
};

SurfacePalette palette_for_body(const Body& b);

void draw_surface_stamp_pixels(ImDrawList* dl, ImVec2 p0, ImVec2 size, const SurfaceStampGrid& g,
                               const SurfacePalette& pal, float alpha = 1.0f, bool draw_border = true);

void draw_star_glyph(ImDrawList* dl, ImVec2 center, float r, std::uint32_t seed, ImU32 col, float alpha = 1.0f);

// Procedural body icon for maps/tables. Draws a stable, deterministic glyph derived from the
// body's properties (type/temp/atm/id). Designed to be readable at small radii.
void draw_body_glyph(ImDrawList* dl, ImVec2 center, float r, const Body& b, float alpha = 1.0f, bool selected = false);

// Procedural jump point icon. Uses a portal-like swirl and rings for readability.
void draw_jump_glyph(ImDrawList* dl, ImVec2 center, float r, std::uint32_t seed, ImU32 col, float alpha = 1.0f, bool surveyed = true);

void draw_system_badge(ImDrawList* dl, ImVec2 p0, float sz, std::uint32_t seed, int jump_degree,
                       double nebula_density, int habitable, float minerals01, bool chokepoint, bool selected);

}  // namespace nebula4x::ui::procgen_gfx
