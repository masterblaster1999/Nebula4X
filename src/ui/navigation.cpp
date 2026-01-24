#include "ui/navigation.h"

#include <algorithm>
#include <cstdint>

namespace nebula4x::ui {

namespace {

std::string kind_prefix(NavTargetKind k) {
  switch (k) {
    case NavTargetKind::System: return "System";
    case NavTargetKind::Ship: return "Ship";
    case NavTargetKind::Colony: return "Colony";
    case NavTargetKind::Body: return "Body";
  }
  return "Unknown";
}

std::string default_bookmark_name(const Simulation& sim, const NavTarget& t) {
  const auto& s = sim.state();
  switch (t.kind) {
    case NavTargetKind::System: {
      if (const auto* sys = find_ptr(s.systems, t.id)) {
        return sys->name.empty() ? ("System #" + std::to_string((unsigned long long)t.id)) : sys->name;
      }
      return "System #" + std::to_string((unsigned long long)t.id);
    }
    case NavTargetKind::Ship: {
      if (const auto* sh = find_ptr(s.ships, t.id)) {
        return sh->name.empty() ? ("Ship #" + std::to_string((unsigned long long)t.id)) : sh->name;
      }
      return "Ship #" + std::to_string((unsigned long long)t.id);
    }
    case NavTargetKind::Colony: {
      if (const auto* c = find_ptr(s.colonies, t.id)) {
        return c->name.empty() ? ("Colony #" + std::to_string((unsigned long long)t.id)) : c->name;
      }
      return "Colony #" + std::to_string((unsigned long long)t.id);
    }
    case NavTargetKind::Body: {
      if (const auto* b = find_ptr(s.bodies, t.id)) {
        return b->name.empty() ? ("Body #" + std::to_string((unsigned long long)t.id)) : b->name;
      }
      return "Body #" + std::to_string((unsigned long long)t.id);
    }
  }
  return "Bookmark";
}

void request_center_on_galaxy_system(UIState& ui, const StarSystem& sys) {
  ui.request_galaxy_map_center = true;
  ui.request_galaxy_map_center_x = sys.galaxy_pos.x;
  ui.request_galaxy_map_center_y = sys.galaxy_pos.y;
  // Don't override zoom by default.
  ui.request_galaxy_map_center_zoom = 0.0;
  ui.request_galaxy_map_fit_half_span = 0.0;
}

void request_center_on_system_pos(UIState& ui, Id system_id, const Vec2& pos_mkm) {
  ui.request_system_map_center = true;
  ui.request_system_map_center_system_id = system_id;
  ui.request_system_map_center_x_mkm = pos_mkm.x;
  ui.request_system_map_center_y_mkm = pos_mkm.y;
  // Leave zoom unchanged unless the user explicitly requested.
  ui.request_system_map_center_zoom = 0.0;
}

} // namespace

NavTarget current_nav_target(const Simulation& sim, Id selected_ship, Id selected_colony, Id selected_body) {
  if (selected_ship != kInvalidId) return {NavTargetKind::Ship, selected_ship};
  if (selected_colony != kInvalidId) return {NavTargetKind::Colony, selected_colony};
  if (selected_body != kInvalidId) return {NavTargetKind::Body, selected_body};
  return {NavTargetKind::System, sim.state().selected_system};
}

bool nav_target_exists(const Simulation& sim, const NavTarget& t) {
  const auto& s = sim.state();
  if (t.id == kInvalidId) return false;
  switch (t.kind) {
    case NavTargetKind::System: return find_ptr(s.systems, t.id) != nullptr;
    case NavTargetKind::Ship: return find_ptr(s.ships, t.id) != nullptr;
    case NavTargetKind::Colony: return find_ptr(s.colonies, t.id) != nullptr;
    case NavTargetKind::Body: return find_ptr(s.bodies, t.id) != nullptr;
  }
  return false;
}

std::string nav_target_label(const Simulation& sim, const NavTarget& t, bool include_kind_prefix) {
  const auto& s = sim.state();

  auto fmt_missing = [&](const char* kind) {
    if (t.id == kInvalidId) return std::string(kind) + ": (none)";
    return std::string(kind) + ": #" + std::to_string((unsigned long long)t.id) + " (missing)";
  };

  std::string base;

  switch (t.kind) {
    case NavTargetKind::System: {
      if (const auto* sys = find_ptr(s.systems, t.id)) {
        base = sys->name.empty() ? ("#" + std::to_string((unsigned long long)sys->id)) : sys->name;
      } else {
        base = (t.id == kInvalidId) ? "(none)" : ("#" + std::to_string((unsigned long long)t.id) + " (missing)");
      }
      break;
    }
    case NavTargetKind::Ship: {
      const auto* sh = find_ptr(s.ships, t.id);
      if (!sh) return include_kind_prefix ? fmt_missing("Ship") : ("#" + std::to_string((unsigned long long)t.id));
      base = sh->name.empty() ? ("#" + std::to_string((unsigned long long)sh->id)) : sh->name;
      if (const auto* sys = find_ptr(s.systems, sh->system_id)) {
        if (!sys->name.empty()) base += " (" + sys->name + ")";
      }
      break;
    }
    case NavTargetKind::Colony: {
      const auto* c = find_ptr(s.colonies, t.id);
      if (!c) return include_kind_prefix ? fmt_missing("Colony") : ("#" + std::to_string((unsigned long long)t.id));
      base = c->name.empty() ? ("#" + std::to_string((unsigned long long)c->id)) : c->name;
      if (const auto* b = find_ptr(s.bodies, c->body_id)) {
        if (const auto* sys = find_ptr(s.systems, b->system_id)) {
          if (!sys->name.empty()) base += " (" + sys->name + ")";
        }
      }
      break;
    }
    case NavTargetKind::Body: {
      const auto* b = find_ptr(s.bodies, t.id);
      if (!b) return include_kind_prefix ? fmt_missing("Body") : ("#" + std::to_string((unsigned long long)t.id));
      base = b->name.empty() ? ("#" + std::to_string((unsigned long long)b->id)) : b->name;
      if (const auto* sys = find_ptr(s.systems, b->system_id)) {
        if (!sys->name.empty()) base += " (" + sys->name + ")";
      }
      break;
    }
  }

  if (!include_kind_prefix) return base;
  return kind_prefix(t.kind) + ": " + base;
}

void apply_nav_target(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body,
                      const NavTarget& t, bool open_windows) {
  auto& s = sim.state();

  // Clear selection by default; cases below re-populate.
  selected_ship = kInvalidId;
  selected_colony = kInvalidId;
  selected_body = kInvalidId;

  switch (t.kind) {
    case NavTargetKind::System: {
      if (t.id != kInvalidId) s.selected_system = t.id;
      if (open_windows) {
        ui.show_map_window = true;
        ui.request_map_tab = MapTab::Galaxy;
        if (const auto* sys = find_ptr(s.systems, s.selected_system)) {
          request_center_on_galaxy_system(ui, *sys);
        }
      }
      break;
    }
    case NavTargetKind::Ship: {
      const auto* sh = find_ptr(s.ships, t.id);
      if (!sh) {
        // Missing target; fall back to system selection.
        if (open_windows) {
          ui.show_map_window = true;
          ui.request_map_tab = MapTab::Galaxy;
        }
        break;
      }

      selected_ship = sh->id;
      s.selected_system = sh->system_id;

      if (open_windows) {
        ui.show_map_window = true;
        ui.request_map_tab = MapTab::System;
        ui.show_details_window = true;
        ui.request_details_tab = DetailsTab::Ship;
        request_center_on_system_pos(ui, sh->system_id, sh->position_mkm);
      }
      break;
    }
    case NavTargetKind::Colony: {
      const auto* c = find_ptr(s.colonies, t.id);
      if (!c) {
        if (open_windows) {
          ui.show_map_window = true;
          ui.request_map_tab = MapTab::Galaxy;
        }
        break;
      }

      selected_colony = c->id;
      selected_body = c->body_id;

      if (const auto* b = find_ptr(s.bodies, c->body_id)) {
        s.selected_system = b->system_id;
        if (open_windows) request_center_on_system_pos(ui, b->system_id, b->position_mkm);
      }

      if (open_windows) {
        ui.show_map_window = true;
        ui.request_map_tab = MapTab::System;
        ui.show_details_window = true;
        ui.request_details_tab = DetailsTab::Colony;
      }
      break;
    }
    case NavTargetKind::Body: {
      const auto* b = find_ptr(s.bodies, t.id);
      if (!b) {
        if (open_windows) {
          ui.show_map_window = true;
          ui.request_map_tab = MapTab::Galaxy;
        }
        break;
      }

      selected_body = b->id;
      s.selected_system = b->system_id;

      if (open_windows) {
        ui.show_map_window = true;
        ui.request_map_tab = MapTab::System;
        ui.show_details_window = true;
        ui.request_details_tab = DetailsTab::Body;
        request_center_on_system_pos(ui, b->system_id, b->position_mkm);
      }
      break;
    }
  }
}

void nav_history_reset(UIState& ui) {
  ui.nav_history.clear();
  ui.nav_history_cursor = -1;
  ui.nav_history_suppress_push = false;
}

void nav_history_push(UIState& ui, const NavTarget& t) {
  if (ui.nav_history_suppress_push) {
    ui.nav_history_suppress_push = false;
    return;
  }
  if (t.id == kInvalidId) return;

  const int max_len = std::clamp(ui.nav_history_max, 16, 1024);

  if (ui.nav_history_cursor >= 0 && ui.nav_history_cursor < (int)ui.nav_history.size()) {
    if (ui.nav_history[ui.nav_history_cursor] == t) return;

    // If we've navigated back, drop forward history before pushing.
    if (ui.nav_history_cursor < (int)ui.nav_history.size() - 1) {
      ui.nav_history.erase(ui.nav_history.begin() + ui.nav_history_cursor + 1, ui.nav_history.end());
    }
  } else {
    // Cursor out of range; treat as reset.
    ui.nav_history_cursor = (int)ui.nav_history.size() - 1;
  }

  ui.nav_history.push_back(t);
  ui.nav_history_cursor = (int)ui.nav_history.size() - 1;

  if ((int)ui.nav_history.size() > max_len) {
    const int overflow = (int)ui.nav_history.size() - max_len;
    ui.nav_history.erase(ui.nav_history.begin(), ui.nav_history.begin() + overflow);
    ui.nav_history_cursor = std::max(0, ui.nav_history_cursor - overflow);
  }
}

bool nav_history_can_back(const UIState& ui) {
  return ui.nav_history_cursor > 0 && ui.nav_history_cursor < (int)ui.nav_history.size();
}

bool nav_history_can_forward(const UIState& ui) {
  return ui.nav_history_cursor >= 0 && ui.nav_history_cursor + 1 < (int)ui.nav_history.size();
}

bool nav_history_back(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body,
                      bool open_windows) {
  if (!nav_history_can_back(ui)) return false;
  ui.nav_history_cursor -= 1;
  ui.nav_history_suppress_push = true;
  apply_nav_target(sim, ui, selected_ship, selected_colony, selected_body, ui.nav_history[ui.nav_history_cursor], open_windows);
  return true;
}

bool nav_history_forward(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body,
                         bool open_windows) {
  if (!nav_history_can_forward(ui)) return false;
  ui.nav_history_cursor += 1;
  ui.nav_history_suppress_push = true;
  apply_nav_target(sim, ui, selected_ship, selected_colony, selected_body, ui.nav_history[ui.nav_history_cursor], open_windows);
  return true;
}

bool nav_is_bookmarked(const UIState& ui, const NavTarget& t) {
  for (const auto& b : ui.nav_bookmarks) {
    if (b.target == t) return true;
  }
  return false;
}

bool nav_bookmark_toggle_current(const Simulation& sim, UIState& ui, Id selected_ship, Id selected_colony,
                                 Id selected_body) {
  const NavTarget cur = current_nav_target(sim, selected_ship, selected_colony, selected_body);
  if (cur.id == kInvalidId) return false;

  for (std::size_t i = 0; i < ui.nav_bookmarks.size(); ++i) {
    if (ui.nav_bookmarks[i].target == cur) {
      ui.nav_bookmarks.erase(ui.nav_bookmarks.begin() + (std::ptrdiff_t)i);
      return false;
    }
  }

  NavBookmark b;
  b.bookmark_id = ui.nav_next_bookmark_id++;
  b.target = cur;
  b.name = default_bookmark_name(sim, cur);
  ui.nav_bookmarks.push_back(std::move(b));

  // Cap to avoid unbounded growth.
  constexpr std::size_t kMaxBookmarks = 128;
  if (ui.nav_bookmarks.size() > kMaxBookmarks) {
    ui.nav_bookmarks.erase(ui.nav_bookmarks.begin(),
                           ui.nav_bookmarks.begin() + (std::ptrdiff_t)(ui.nav_bookmarks.size() - kMaxBookmarks));
  }

  return true;
}

int nav_bookmarks_prune_missing(const Simulation& sim, UIState& ui) {
  const std::size_t before = ui.nav_bookmarks.size();
  ui.nav_bookmarks.erase(std::remove_if(ui.nav_bookmarks.begin(), ui.nav_bookmarks.end(),
                                        [&](const NavBookmark& b) { return !nav_target_exists(sim, b.target); }),
                         ui.nav_bookmarks.end());
  return (int)(before - ui.nav_bookmarks.size());
}

} // namespace nebula4x::ui
