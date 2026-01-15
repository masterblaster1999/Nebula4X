#pragma once

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include "nebula4x/core/game_state.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

inline const char* procgen_lens_mode_label(ProcGenLensMode m) {
  switch (m) {
    case ProcGenLensMode::Off: return "Off";
    case ProcGenLensMode::NebulaDensity: return "Nebula density";
    case ProcGenLensMode::StarTemperature: return "Star temperature";
    case ProcGenLensMode::StarMass: return "Star mass";
    case ProcGenLensMode::StarLuminosity: return "Star luminosity";
    case ProcGenLensMode::BodyCount: return "Body count";
    case ProcGenLensMode::HabitableCandidates: return "Habitable candidates";
    case ProcGenLensMode::MineralWealth: return "Mineral wealth";
    case ProcGenLensMode::JumpDegree: return "Jump degree";
  }
  return "Off";
}

inline const char* procgen_lens_mode_combo_items() {
  // NOLINTNEXTLINE(bugprone-return-const-ref-from-parameter)
  return "Off\0Nebula density\0Star temperature\0Star mass\0Star luminosity\0Body count\0Habitable candidates\0Mineral wealth\0Jump degree\0";
}

// Returns the system's primary star body if present.
inline const Body* find_primary_star(const GameState& s, const StarSystem& sys) {
  const Body* fallback = nullptr;
  for (Id bid : sys.bodies) {
    const Body* b = find_ptr(s.bodies, bid);
    if (!b) continue;
    if (b->type != BodyType::Star) continue;
    if (!fallback) fallback = b;
    // Prefer the star at the system origin (common "primary").
    if (b->parent_body_id == kInvalidId && std::abs(b->orbit_radius_mkm) < 1e-6) {
      return b;
    }
  }
  return fallback;
}

inline int count_bodies(const GameState& s, const StarSystem& sys, BodyType t) {
  int n = 0;
  for (Id bid : sys.bodies) {
    const Body* b = find_ptr(s.bodies, bid);
    if (!b) continue;
    if (b->type == t) ++n;
  }
  return n;
}

inline int count_planet_like_bodies(const GameState& s, const StarSystem& sys) {
  int n = 0;
  for (Id bid : sys.bodies) {
    const Body* b = find_ptr(s.bodies, bid);
    if (!b) continue;
    if (b->type == BodyType::Planet || b->type == BodyType::Moon) ++n;
  }
  return n;
}

inline double sum_mineral_deposits_tons(const GameState& s, const StarSystem& sys) {
  double total = 0.0;
  for (Id bid : sys.bodies) {
    const Body* b = find_ptr(s.bodies, bid);
    if (!b) continue;
    for (const auto& [k, v] : b->mineral_deposits) {
      (void)k;
      if (!std::isfinite(v) || v <= 0.0) continue;
      total += v;
    }
  }
  return total;
}

// A very rough "candidate" heuristic meant for UI exploration, not gameplay.
// We intentionally keep it conservative and rely only on fields that exist in
// the current save schema.
inline int count_habitable_candidates(const GameState& s, const StarSystem& sys) {
  int n = 0;
  for (Id bid : sys.bodies) {
    const Body* b = find_ptr(s.bodies, bid);
    if (!b) continue;
    if (b->type != BodyType::Planet && b->type != BodyType::Moon) continue;

    const double t = b->surface_temp_k;
    const double atm = b->atmosphere_atm;
    if (!std::isfinite(t) || !std::isfinite(atm)) continue;
    if (t <= 0.0 || atm <= 0.0) continue;

    // Broad "liquid water" band + survivable pressure band.
    if (t >= 245.0 && t <= 330.0 && atm >= 0.4 && atm <= 4.5) {
      ++n;
    }
  }
  return n;
}

inline double procgen_lens_value(const GameState& s, const StarSystem& sys, ProcGenLensMode mode) {
  switch (mode) {
    case ProcGenLensMode::Off:
      return 0.0;
    case ProcGenLensMode::NebulaDensity:
      return std::clamp(sys.nebula_density, 0.0, 1.0);
    case ProcGenLensMode::StarTemperature: {
      const Body* star = find_primary_star(s, sys);
      return star ? star->surface_temp_k : 0.0;
    }
    case ProcGenLensMode::StarMass: {
      const Body* star = find_primary_star(s, sys);
      return star ? star->mass_solar : 0.0;
    }
    case ProcGenLensMode::StarLuminosity: {
      const Body* star = find_primary_star(s, sys);
      return star ? star->luminosity_solar : 0.0;
    }
    case ProcGenLensMode::BodyCount:
      return static_cast<double>(sys.bodies.size());
    case ProcGenLensMode::HabitableCandidates:
      return static_cast<double>(count_habitable_candidates(s, sys));
    case ProcGenLensMode::MineralWealth:
      return sum_mineral_deposits_tons(s, sys);
    case ProcGenLensMode::JumpDegree:
      return static_cast<double>(sys.jump_points.size());
  }
  return 0.0;
}

inline const char* procgen_lens_value_unit(ProcGenLensMode mode) {
  switch (mode) {
    case ProcGenLensMode::Off: return "";
    case ProcGenLensMode::NebulaDensity: return "%";
    case ProcGenLensMode::StarTemperature: return "K";
    case ProcGenLensMode::StarMass: return "M☉";
    case ProcGenLensMode::StarLuminosity: return "L☉";
    case ProcGenLensMode::BodyCount: return "bodies";
    case ProcGenLensMode::HabitableCandidates: return "candidates";
    case ProcGenLensMode::MineralWealth: return "tons";
    case ProcGenLensMode::JumpDegree: return "links";
  }
  return "";
}

// Simple blue->red perceptual-ish gradient for lens visualizations.
// t should be in [0,1].
inline ImU32 procgen_lens_gradient_color(float t, float alpha = 1.0f) {
  t = std::clamp(t, 0.0f, 1.0f);
  alpha = std::clamp(alpha, 0.0f, 1.0f);
  // Hue 0.67 ~ blue, 0.0 ~ red.
  const float hue = (1.0f - t) * 0.67f;
  ImVec4 c = ImColor::HSV(hue, 0.80f, 0.92f, alpha);
  return ImGui::ColorConvertFloat4ToU32(c);
}

} // namespace nebula4x::ui
