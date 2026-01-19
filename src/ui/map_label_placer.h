#pragma once

#include <imgui.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace nebula4x::ui {

struct LabelRect {
  ImVec2 min{0.0f, 0.0f};
  ImVec2 max{0.0f, 0.0f};
};

// Lightweight screen-space label decluttering helper.
//
// Uses a coarse uniform grid to accelerate overlap checks.
class LabelPlacer {
 public:
  // preferred_quadrant:
  //  0 = top-right, 1 = bottom-right, 2 = top-left, 3 = bottom-left.
  explicit LabelPlacer(ImVec2 viewport_min = ImVec2(0, 0),
                       ImVec2 viewport_max = ImVec2(0, 0),
                       float cell_px = 96.0f);

  void reset(ImVec2 viewport_min, ImVec2 viewport_max);

  // Place a label near an anchor point.
  //
  // dx/dy specify the desired padding from the anchor to the label bounds.
  // Returns true and writes the chosen top-left position when placed.
  bool place_near(const ImVec2& anchor,
                  float dx,
                  float dy,
                  const ImVec2& text_size,
                  float padding_px,
                  int preferred_quadrant,
                  ImVec2* out_pos);

  // Place a label at a fixed top-left position (no quadrant search).
  bool place_at(const ImVec2& pos, const ImVec2& text_size, float padding_px, ImVec2* out_pos);

 private:
  bool rect_overlaps(const LabelRect& r) const;
  bool rect_in_viewport(const LabelRect& r) const;
  void commit_rect(const LabelRect& r);

  static bool intersects(const LabelRect& a, const LabelRect& b);

  std::uint64_t cell_key(int cx, int cy) const;
  void rect_cells(const LabelRect& r, int* x0, int* y0, int* x1, int* y1) const;

  ImVec2 vmin_{0.0f, 0.0f};
  ImVec2 vmax_{0.0f, 0.0f};
  float cell_px_{96.0f};

  // cell -> indices into rects_
  std::unordered_map<std::uint64_t, std::vector<int>> grid_;
  std::vector<LabelRect> rects_;
};

} // namespace nebula4x::ui
