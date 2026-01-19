#include "ui/map_label_placer.h"

#include <algorithm>
#include <cmath>

namespace nebula4x::ui {

LabelPlacer::LabelPlacer(ImVec2 viewport_min, ImVec2 viewport_max, float cell_px)
    : vmin_(viewport_min), vmax_(viewport_max), cell_px_(std::max(8.0f, cell_px)) {}

void LabelPlacer::reset(ImVec2 viewport_min, ImVec2 viewport_max) {
  vmin_ = viewport_min;
  vmax_ = viewport_max;
  grid_.clear();
  rects_.clear();
}

bool LabelPlacer::intersects(const LabelRect& a, const LabelRect& b) {
  return a.min.x < b.max.x && a.max.x > b.min.x && a.min.y < b.max.y && a.max.y > b.min.y;
}

std::uint64_t LabelPlacer::cell_key(int cx, int cy) const {
  // Pack signed int32 into uint64.
  const std::uint64_t ux = static_cast<std::uint32_t>(cx);
  const std::uint64_t uy = static_cast<std::uint32_t>(cy);
  return (ux << 32) | uy;
}

void LabelPlacer::rect_cells(const LabelRect& r, int* x0, int* y0, int* x1, int* y1) const {
  const float inv = 1.0f / std::max(1.0f, cell_px_);
  *x0 = static_cast<int>(std::floor(r.min.x * inv));
  *y0 = static_cast<int>(std::floor(r.min.y * inv));
  *x1 = static_cast<int>(std::floor(r.max.x * inv));
  *y1 = static_cast<int>(std::floor(r.max.y * inv));
}

bool LabelPlacer::rect_in_viewport(const LabelRect& r) const {
  // Require the padded rect to fully fit in viewport.
  if (r.min.x < vmin_.x || r.min.y < vmin_.y) return false;
  if (r.max.x > vmax_.x || r.max.y > vmax_.y) return false;
  return true;
}

bool LabelPlacer::rect_overlaps(const LabelRect& r) const {
  int x0, y0, x1, y1;
  rect_cells(r, &x0, &y0, &x1, &y1);
  for (int cy = y0; cy <= y1; ++cy) {
    for (int cx = x0; cx <= x1; ++cx) {
      auto it = grid_.find(cell_key(cx, cy));
      if (it == grid_.end()) continue;
      for (int idx : it->second) {
        if (idx < 0 || idx >= static_cast<int>(rects_.size())) continue;
        if (intersects(r, rects_[idx])) return true;
      }
    }
  }
  return false;
}

void LabelPlacer::commit_rect(const LabelRect& r) {
  const int idx = static_cast<int>(rects_.size());
  rects_.push_back(r);

  int x0, y0, x1, y1;
  rect_cells(r, &x0, &y0, &x1, &y1);
  for (int cy = y0; cy <= y1; ++cy) {
    for (int cx = x0; cx <= x1; ++cx) {
      grid_[cell_key(cx, cy)].push_back(idx);
    }
  }
}

bool LabelPlacer::place_at(const ImVec2& pos, const ImVec2& text_size, float padding_px, ImVec2* out_pos) {
  const float pad = std::max(0.0f, padding_px);
  LabelRect r;
  r.min = ImVec2(pos.x - pad, pos.y - pad);
  r.max = ImVec2(pos.x + text_size.x + pad, pos.y + text_size.y + pad);

  if (!rect_in_viewport(r)) return false;
  if (rect_overlaps(r)) return false;

  if (out_pos) *out_pos = pos;
  commit_rect(r);
  return true;
}

bool LabelPlacer::place_near(const ImVec2& anchor,
                            float dx,
                            float dy,
                            const ImVec2& text_size,
                            float padding_px,
                            int preferred_quadrant,
                            ImVec2* out_pos) {
  const float px = std::max(0.0f, dx);
  const float py = std::max(0.0f, dy);

  // Candidate top-left positions.
  const ImVec2 tr(anchor.x + px, anchor.y - py - text_size.y);
  const ImVec2 br(anchor.x + px, anchor.y + py);
  const ImVec2 tl(anchor.x - px - text_size.x, anchor.y - py - text_size.y);
  const ImVec2 bl(anchor.x - px - text_size.x, anchor.y + py);

  const ImVec2 candidates[4] = {tr, br, tl, bl};
  int order[4] = {0, 1, 2, 3};

  // Rotate order so the preferred quadrant is tested first.
  const int pq = std::clamp(preferred_quadrant, 0, 3);
  order[0] = pq;
  int w = 1;
  for (int i = 0; i < 4; ++i) {
    if (i == pq) continue;
    order[w++] = i;
  }

  for (int i = 0; i < 4; ++i) {
    const ImVec2 pos = candidates[order[i]];
    if (place_at(pos, text_size, padding_px, out_pos)) {
      return true;
    }
  }

  return false;
}

} // namespace nebula4x::ui
