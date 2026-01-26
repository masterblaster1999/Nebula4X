#include "ui/navigator_window.h"

#include <algorithm>
#include <cfloat>
#include <string>

#include "imgui.h"
#include <misc/cpp/imgui_stdlib.h>

#include "ui/navigation.h"

namespace nebula4x::ui {

namespace {

bool bookmark_row_drag_drop(UIState& ui, int row_index) {
  // Drag source
  bool moved = false;
  if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
    ImGui::SetDragDropPayload("nebula4x_nav_bm_idx", &row_index, sizeof(int));
    if (row_index >= 0 && row_index < (int)ui.nav_bookmarks.size()) {
      const auto& b = ui.nav_bookmarks[row_index];
      ImGui::TextUnformatted(b.name.empty() ? "(bookmark)" : b.name.c_str());
    }
    ImGui::EndDragDropSource();
  }

  // Drop target
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("nebula4x_nav_bm_idx")) {
      if (payload->DataSize == sizeof(int)) {
        const int src = *(const int*)payload->Data;
        const int dst = row_index;
        if (src >= 0 && src < (int)ui.nav_bookmarks.size() && dst >= 0 && dst < (int)ui.nav_bookmarks.size() &&
            src != dst) {
          NavBookmark tmp = ui.nav_bookmarks[src];
          ui.nav_bookmarks.erase(ui.nav_bookmarks.begin() + src);
          ui.nav_bookmarks.insert(ui.nav_bookmarks.begin() + dst, std::move(tmp));
          moved = true;
        }
      }
    }
    ImGui::EndDragDropTarget();
  }
  return moved;
}

} // namespace

void draw_navigator_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  if (!ui.show_navigator_window) return;

  ImGui::SetNextWindowSize(ImVec2(640, 460), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Navigator", &ui.show_navigator_window, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return;
  }

  // Current selection
  const NavTarget cur = current_nav_target(sim, selected_ship, selected_colony, selected_body);
  const std::string cur_label = nav_target_label(sim, cur, /*include_kind_prefix=*/true);
  const bool cur_bookmarked = nav_is_bookmarked(ui, cur);

  ImGui::TextDisabled("Current:");
  ImGui::SameLine();
  ImGui::TextUnformatted(cur_label.c_str());

  ImGui::Spacing();

  if (ImGui::BeginTabBar("##navigator_tabs")) {
    if (ImGui::BeginTabItem("Bookmarks")) {
      // Toolbar
      const char* pin_label = cur_bookmarked ? "Unpin current" : "Pin current";
      if (ImGui::Button(pin_label)) {
        nav_bookmark_toggle_current(sim, ui, selected_ship, selected_colony, selected_body);
      }
      ImGui::SameLine();
      if (ImGui::Button("Prune missing")) {
        const int removed = nav_bookmarks_prune_missing(sim, ui);
        if (removed > 0) {
          // No toast system here; status is visible in the list.
        }
      }
      ImGui::SameLine();
      if (ImGui::Button("Clear all")) {
        ui.nav_bookmarks.clear();
      }

      ImGui::SameLine();
      ImGui::Checkbox("Auto-open on jump", &ui.nav_open_windows_on_jump);
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("When jumping to a bookmark/history entry, open Map+Details and focus the relevant tabs.");
      }

      ImGui::Separator();

      if (ui.nav_bookmarks.empty()) {
        ImGui::TextDisabled("(no pinned bookmarks yet)");
        ImGui::TextDisabled("Tip: pin the current selection, then use this window (or the command console) to jump.");
      } else {
        const ImGuiTableFlags flags =
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;

        // Reserve space for the table; leave room for the window bottom.
        const float footer_h = ImGui::GetFrameHeightWithSpacing() * 0.5f;
        const ImVec2 table_size(0.0f, std::max(120.0f, ImGui::GetContentRegionAvail().y - footer_h));

        if (ImGui::BeginTable("##nav_bookmarks", 4, flags, table_size)) {
          ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.45f);
          ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthStretch, 0.45f);
          ImGui::TableSetupColumn("Go", ImGuiTableColumnFlags_WidthFixed, 44.0f);
          ImGui::TableSetupColumn("Del", ImGuiTableColumnFlags_WidthFixed, 44.0f);
          ImGui::TableHeadersRow();

          // Note: erase while iterating is tricky; defer deletions.
          int delete_idx = -1;

          for (int i = 0; i < (int)ui.nav_bookmarks.size(); ++i) {
            NavBookmark& b = ui.nav_bookmarks[i];
            const bool exists = nav_target_exists(sim, b.target);
            const std::string label = nav_target_label(sim, b.target, /*include_kind_prefix=*/true);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID((int)b.bookmark_id);

            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputText("##bm_name", &b.name);

            // Drag/drop reorder by dragging the name cell.
            bookmark_row_drag_drop(ui, i);

            ImGui::TableSetColumnIndex(1);
            if (!exists) {
              ImGui::TextDisabled("%s", label.c_str());
            } else {
              // Clickable label (double-click to jump).
              ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_SpanAllColumns);
              if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                apply_nav_target(sim, ui, selected_ship, selected_colony, selected_body, b.target, ui.nav_open_windows_on_jump);
              }
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::BeginDisabled(!exists);
            if (ImGui::SmallButton("Go")) {
              apply_nav_target(sim, ui, selected_ship, selected_colony, selected_body, b.target, ui.nav_open_windows_on_jump);
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered()) {
              ImGui::SetTooltip("Jump to this bookmark");
            }

            ImGui::TableSetColumnIndex(3);
            if (ImGui::SmallButton("Del")) {
              delete_idx = i;
            }

            ImGui::PopID();
          }

          if (delete_idx >= 0 && delete_idx < (int)ui.nav_bookmarks.size()) {
            ui.nav_bookmarks.erase(ui.nav_bookmarks.begin() + delete_idx);
          }

          ImGui::EndTable();
        }
      }

      ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("History")) {
      // Toolbar
      const bool can_back = nav_history_can_back(ui);
      const bool can_fwd = nav_history_can_forward(ui);

      ImGui::BeginDisabled(!can_back);
      if (ImGui::Button("Back")) {
        nav_history_back(sim, ui, selected_ship, selected_colony, selected_body, ui.nav_open_windows_on_jump);
      }
      ImGui::EndDisabled();

      ImGui::SameLine();
      ImGui::BeginDisabled(!can_fwd);
      if (ImGui::Button("Forward")) {
        nav_history_forward(sim, ui, selected_ship, selected_colony, selected_body, ui.nav_open_windows_on_jump);
      }
      ImGui::EndDisabled();

      ImGui::SameLine();
      if (ImGui::Button("Clear history")) {
        nav_history_reset(ui);
      }

      ImGui::SameLine();
      ImGui::Checkbox("Auto-open on jump", &ui.nav_open_windows_on_jump);

      ImGui::SameLine();
      ImGui::SetNextItemWidth(140.0f);
      ImGui::SliderInt("Max##nav_hist_max", &ui.nav_history_max, 32, 512);
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Max number of history entries to keep (oldest entries are dropped).");
      }

      ImGui::Separator();

      ImGui::TextDisabled("Tip: Alt+Left / Alt+Right navigates history without opening this window.");

      ImGui::Spacing();

      if (ui.nav_history.empty()) {
        ImGui::TextDisabled("(history is empty)");
        ImGui::TextDisabled("Interact with the map/directory, then come back here to jump between recent selections.");
      } else {
        const float list_h = std::max(140.0f, ImGui::GetContentRegionAvail().y);
        if (ImGui::BeginChild("##nav_history_list", ImVec2(0.0f, list_h), true)) {
          // Show newest first.
          for (int i = (int)ui.nav_history.size() - 1; i >= 0; --i) {
            const NavTarget& t = ui.nav_history[i];
            const bool exists = nav_target_exists(sim, t);
            std::string label = nav_target_label(sim, t, /*include_kind_prefix=*/true);
            if (!exists) label += " (missing)";

            // Highlight current cursor.
            const bool selected = (i == ui.nav_history_cursor);
            ImGui::PushID(i);
            if (!exists) {
              ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            }
            if (ImGui::Selectable(label.c_str(), selected)) {
              ui.nav_history_cursor = i;
              ui.nav_history_suppress_push = true;
              apply_nav_target(sim, ui, selected_ship, selected_colony, selected_body, t, ui.nav_open_windows_on_jump);
            }
            if (!exists) {
              ImGui::PopStyleColor();
            }
            ImGui::PopID();
          }
          ImGui::EndChild();
        }
      }

      ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
  }

  ImGui::End();
}

} // namespace nebula4x::ui
