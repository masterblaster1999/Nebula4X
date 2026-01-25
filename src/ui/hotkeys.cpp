#include "ui/hotkeys.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_map>

#include "nebula4x/util/strings.h"

namespace nebula4x::ui {

namespace {

HotkeyChord chord(bool ctrl, bool shift, bool alt, bool super, ImGuiKey key) {
  HotkeyChord c;
  c.ctrl = ctrl;
  c.shift = shift;
  c.alt = alt;
  c.super = super;
  c.key = static_cast<int>(key);
  return c;
}

bool is_modifier_key(ImGuiKey key) {
  switch (key) {
    case ImGuiKey_LeftCtrl:
    case ImGuiKey_RightCtrl:
    case ImGuiKey_LeftShift:
    case ImGuiKey_RightShift:
    case ImGuiKey_LeftAlt:
    case ImGuiKey_RightAlt:
    case ImGuiKey_LeftSuper:
    case ImGuiKey_RightSuper:
      return true;
    default:
      return false;
  }
}

std::string trim_ascii(std::string_view s) {
  std::size_t a = 0;
  std::size_t b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return std::string(s.substr(a, b - a));
}

bool ieq(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  }
  return true;
}

ImGuiKey find_key_by_name(std::string_view name) {
  const std::string n = nebula4x::to_lower(trim_ascii(name));
  if (n.empty()) return ImGuiKey_None;

  // Common aliases.
  struct Alias {
    const char* name;
    ImGuiKey key;
  };
  static const Alias kAliases[] = {
      {"esc", ImGuiKey_Escape},
      {"escape", ImGuiKey_Escape},
      {"return", ImGuiKey_Enter},
      {"enter", ImGuiKey_Enter},
      {"space", ImGuiKey_Space},
      {"tab", ImGuiKey_Tab},
      {"backspace", ImGuiKey_Backspace},
      {"del", ImGuiKey_Delete},
      {"delete", ImGuiKey_Delete},
      {"ins", ImGuiKey_Insert},
      {"insert", ImGuiKey_Insert},
      {"home", ImGuiKey_Home},
      {"end", ImGuiKey_End},
      {"pgup", ImGuiKey_PageUp},
      {"pageup", ImGuiKey_PageUp},
      {"pgdn", ImGuiKey_PageDown},
      {"pagedown", ImGuiKey_PageDown},
      {"left", ImGuiKey_LeftArrow},
      {"leftarrow", ImGuiKey_LeftArrow},
      {"right", ImGuiKey_RightArrow},
      {"rightarrow", ImGuiKey_RightArrow},
      {"up", ImGuiKey_UpArrow},
      {"uparrow", ImGuiKey_UpArrow},
      {"down", ImGuiKey_DownArrow},
      {"downarrow", ImGuiKey_DownArrow},
      {"comma", ImGuiKey_Comma},
      {"period", ImGuiKey_Period},
      {"dot", ImGuiKey_Period},
  };
  for (const auto& a : kAliases) {
    if (n == a.name) return a.key;
  }

  // Exact match against ImGui key names.
  for (int k = static_cast<int>(ImGuiKey_NamedKey_BEGIN); k < static_cast<int>(ImGuiKey_NamedKey_END); ++k) {
    const char* nm = ImGui::GetKeyName(static_cast<ImGuiKey>(k));
    if (!nm || nm[0] == '\0') continue;
    const std::string low = nebula4x::to_lower(std::string(nm));
    if (low == n) return static_cast<ImGuiKey>(k);
  }

  return ImGuiKey_None;
}

std::vector<std::string> split_plus(std::string_view s) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= s.size()) {
    const std::size_t p = s.find('+', start);
    if (p == std::string_view::npos) {
      out.push_back(trim_ascii(s.substr(start)));
      break;
    }
    out.push_back(trim_ascii(s.substr(start, p - start)));
    start = p + 1;
  }
  // Drop empties.
  out.erase(std::remove_if(out.begin(), out.end(), [](const std::string& t) { return t.empty(); }), out.end());
  return out;
}

bool modifiers_match_exact(const HotkeyChord& c, const ImGuiIO& io) {
  return io.KeyCtrl == c.ctrl && io.KeyShift == c.shift && io.KeyAlt == c.alt && io.KeySuper == c.super;
}

}  // namespace

const std::vector<HotkeyDef>& hotkey_defs() {
  static const std::vector<HotkeyDef> kDefs = {
      // Command/navigation
      {"ui.command_console", "Command", "Open Command Console", "Search and run commands (UI and sim actions).",
       chord(true, false, false, false, ImGuiKey_P)},
      {"ui.toggle.omnisearch", "Command", "Toggle OmniSearch", "Universal search across commands, entities, docs, and live JSON.",
       chord(true, false, false, false, ImGuiKey_F)},
      {"ui.toggle.settings", "Command", "Toggle Settings", "Open the Settings window.",
       chord(true, false, false, false, ImGuiKey_Comma)},
      {"ui.toggle.help", "Command", "Toggle Help / Shortcuts", "Open the in-game help overlay.",
       chord(false, false, false, false, ImGuiKey_F1)},
      {"ui.toggle.tours", "Command", "Toggle Guided Tour", "Start/stop the guided tours overlay.",
       chord(false, false, false, false, ImGuiKey_F2)},
      {"ui.toggle.notifications", "Command", "Toggle Notification Center", "Open the notifications inbox.",
       chord(false, false, false, false, ImGuiKey_F3)},

      // Navigator
      {"nav.back", "Navigation", "Back (Selection History)", "Navigate to the previous selection.",
       chord(false, false, true, false, ImGuiKey_LeftArrow)},
      {"nav.forward", "Navigation", "Forward (Selection History)", "Navigate to the next selection.",
       chord(false, false, true, false, ImGuiKey_RightArrow)},

      // Window toggles
      {"ui.toggle.controls", "Windows", "Toggle Controls window", nullptr, chord(true, false, false, false, ImGuiKey_1)},
      {"ui.toggle.map", "Windows", "Toggle Map window", nullptr, chord(true, false, false, false, ImGuiKey_2)},
      {"ui.toggle.details", "Windows", "Toggle Details window", nullptr, chord(true, false, false, false, ImGuiKey_3)},
      {"ui.toggle.directory", "Windows", "Toggle Directory window", nullptr, chord(true, false, false, false, ImGuiKey_4)},
      {"ui.toggle.economy", "Windows", "Toggle Economy window", nullptr, chord(true, false, false, false, ImGuiKey_5)},
      {"ui.toggle.production", "Windows", "Toggle Production window", nullptr, chord(true, false, false, false, ImGuiKey_6)},
      {"ui.toggle.timeline", "Windows", "Toggle Timeline window", nullptr, chord(true, false, false, false, ImGuiKey_7)},
      {"ui.toggle.design_studio", "Windows", "Toggle Design Studio window", nullptr,
       chord(true, false, false, false, ImGuiKey_8)},
      {"ui.toggle.intel", "Windows", "Toggle Intel window", nullptr, chord(true, false, false, false, ImGuiKey_9)},
      {"ui.toggle.intel_notebook", "Windows", "Toggle Intel Notebook",
       "Unified knowledge-base: system intel notes + curated journal (tagging, pinning, export).",
       chord(true, true, false, false, ImGuiKey_I)},
      {"ui.toggle.diplomacy", "Windows", "Toggle Diplomacy Graph window", nullptr,
       chord(true, false, false, false, ImGuiKey_0)},

      {"ui.toggle.fleet_manager", "Windows", "Toggle Fleet Manager", nullptr,
       chord(true, true, false, false, ImGuiKey_F)},
      {"ui.toggle.regions", "Windows", "Toggle Regions window", nullptr, chord(true, true, false, false, ImGuiKey_R)},
      {"ui.toggle.advisor", "Windows", "Toggle Advisor (Issues)", nullptr, chord(true, true, false, false, ImGuiKey_A)},
      {"ui.toggle.colony_profiles", "Windows", "Toggle Colony Profiles", nullptr,
       chord(true, true, false, false, ImGuiKey_B)},
      {"ui.toggle.ship_profiles", "Windows", "Toggle Ship Profiles", nullptr,
       chord(true, true, false, false, ImGuiKey_M)},
      {"ui.toggle.shipyard_targets", "Windows", "Toggle Shipyard Targets", nullptr,
       chord(true, true, false, false, ImGuiKey_Y)},
      {"ui.toggle.survey_network", "Windows", "Toggle Survey Network", nullptr,
       chord(true, true, false, false, ImGuiKey_J)},

      // Tools / debug
      {"ui.toggle.entity_inspector", "Tools", "Toggle Entity Inspector", nullptr,
       chord(true, false, false, false, ImGuiKey_G)},
      {"ui.toggle.reference_graph", "Tools", "Toggle Reference Graph", nullptr,
       chord(true, true, false, false, ImGuiKey_G)},
      {"ui.toggle.time_machine", "Tools", "Toggle Time Machine", nullptr,
       chord(true, true, false, false, ImGuiKey_D)},
      {"ui.toggle.compare", "Tools", "Toggle Compare / Diff", 
       "Compare two entities and view/export a structured diff.",
       chord(true, true, false, false, ImGuiKey_X)},
      {"ui.toggle.navigator", "Tools", "Toggle Navigator window", nullptr,
       chord(true, true, false, false, ImGuiKey_N)},
      {"ui.toggle.layout_profiles", "Tools", "Toggle Layout Profiles", nullptr,
       chord(true, true, false, false, ImGuiKey_L)},
      {"ui.toggle.window_manager", "Tools", "Toggle Window Manager", 
       "Manage window visibility and pop-out (floating) launch behavior.",
       chord(true, true, false, false, ImGuiKey_W)},
      {"ui.toggle.focus_mode", "Windows", "Toggle Focus Mode (Map only)",
       "Temporarily hides all windows except the Map; toggling again restores the previous set.",
       chord(false, false, false, false, ImGuiKey_F10)},
      {"ui.toggle.ui_forge", "Tools", "Toggle UI Forge", nullptr, chord(true, true, false, false, ImGuiKey_U)},
      {"ui.toggle.context_forge", "Tools", "Toggle Context Forge", nullptr,
       chord(true, true, false, false, ImGuiKey_C)},
      {"ui.toggle.content_validation", "Tools", "Toggle Content Validation", nullptr,
       chord(true, true, false, false, ImGuiKey_V)},
      {"ui.toggle.state_doctor", "Tools", "Toggle State Doctor", nullptr,
       chord(true, true, false, false, ImGuiKey_K)},

      // Game
      {"game.save", "Game", "Save game", "Save to the current save path.", chord(true, false, false, false, ImGuiKey_S)},
      {"game.load", "Game", "Load game", "Load from the current load path.", chord(true, false, false, false, ImGuiKey_O)},

      // Time advance (separate actions so players can rebind easily).
      {"time.advance_1", "Time", "Advance 1 day", "Advance the simulation by 1 day.",
       chord(false, false, false, false, ImGuiKey_Space)},
      {"time.advance_5", "Time", "Advance 5 days", "Advance the simulation by 5 days.",
       chord(false, true, false, false, ImGuiKey_Space)},
      {"time.advance_30", "Time", "Advance 30 days", "Advance the simulation by 30 days.",
       chord(true, false, false, false, ImGuiKey_Space)},

      // Accessibility
      {"accessibility.toggle_screen_reader", "Accessibility", "Toggle narration (screen reader)", nullptr,
       chord(true, false, true, false, ImGuiKey_R)},
      {"accessibility.repeat_last", "Accessibility", "Repeat last narration", nullptr,
       chord(true, false, true, false, ImGuiKey_Period)},
  };
  return kDefs;
}

HotkeyChord hotkey_default(std::string_view id) {
  for (const auto& d : hotkey_defs()) {
    if (id == d.id) return d.default_chord;
  }
  return HotkeyChord{};
}

HotkeyChord hotkey_get(const UIState& ui, std::string_view id) {
  const auto it = ui.hotkey_overrides.find(std::string(id));
  if (it != ui.hotkey_overrides.end()) return it->second;
  return hotkey_default(id);
}

bool hotkey_set(UIState& ui, std::string_view id, const HotkeyChord& chord_in) {
  const HotkeyChord def = hotkey_default(id);

  // Unknown id: still allow set so imported configs don't hard-fail.
  // We keep it as an override until the definition exists.
  const bool known = (def.key != 0) || std::any_of(hotkey_defs().begin(), hotkey_defs().end(),
                                                  [&](const HotkeyDef& d) { return id == d.id; });

  const HotkeyChord chord = chord_in;

  const std::string key = std::string(id);
  const auto it = ui.hotkey_overrides.find(key);

  if (known && chord == def) {
    if (it == ui.hotkey_overrides.end()) return false;
    ui.hotkey_overrides.erase(it);
    return true;
  }

  if (it != ui.hotkey_overrides.end() && it->second == chord) return false;
  ui.hotkey_overrides[key] = chord;
  return true;
}

bool hotkey_reset(UIState& ui, std::string_view id) {
  const auto it = ui.hotkey_overrides.find(std::string(id));
  if (it == ui.hotkey_overrides.end()) return false;
  ui.hotkey_overrides.erase(it);
  return true;
}

void hotkeys_reset_all(UIState& ui) {
  ui.hotkey_overrides.clear();
}

std::string hotkey_to_string(const HotkeyChord& chord) {
  if (chord.key == 0) return std::string();

  std::string out;
  if (chord.ctrl) out += "Ctrl+";
  if (chord.shift) out += "Shift+";
  if (chord.alt) out += "Alt+";
  if (chord.super) out += "Super+";

  const char* name = ImGui::GetKeyName(static_cast<ImGuiKey>(chord.key));
  if (name && name[0] != '\0') out += name;
  else out += "?";
  return out;
}

bool parse_hotkey(std::string_view text, HotkeyChord* out, std::string* error) {
  if (!out) return false;
  HotkeyChord c;

  const std::string t = nebula4x::to_lower(trim_ascii(text));
  if (t.empty() || t == "unbound" || t == "none" || t == "null" || t == "-" || t == "disabled") {
    *out = HotkeyChord{};
    return true;
  }

  const auto toks = split_plus(text);
  if (toks.empty()) {
    if (error) *error = "Empty hotkey.";
    return false;
  }

  std::string key_token;
  for (const auto& raw : toks) {
    const std::string tok = nebula4x::to_lower(trim_ascii(raw));
    if (tok.empty()) continue;

    if (tok == "ctrl" || tok == "control") {
      c.ctrl = true;
      continue;
    }
    if (tok == "shift") {
      c.shift = true;
      continue;
    }
    if (tok == "alt" || tok == "option") {
      c.alt = true;
      continue;
    }
    if (tok == "super" || tok == "cmd" || tok == "command" || tok == "win" || tok == "meta") {
      c.super = true;
      continue;
    }

    // Key token.
    if (!key_token.empty()) {
      if (error) *error = "Hotkey has multiple key tokens.";
      return false;
    }
    key_token = trim_ascii(raw);
  }

  if (key_token.empty()) {
    if (error) *error = "Hotkey is missing a key.";
    return false;
  }

  const ImGuiKey k = find_key_by_name(key_token);
  if (k == ImGuiKey_None) {
    if (error) *error = std::string("Unknown key name: '") + std::string(key_token) + "'";
    return false;
  }
  c.key = static_cast<int>(k);
  *out = c;
  return true;
}

bool hotkey_pressed(const HotkeyChord& chord, const ImGuiIO& io, bool repeat) {
  if (chord.key == 0) return false;
  if (!modifiers_match_exact(chord, io)) return false;
  return ImGui::IsKeyPressed(static_cast<ImGuiKey>(chord.key), repeat);
}

bool hotkey_pressed(const UIState& ui, std::string_view id, const ImGuiIO& io, bool repeat) {
  return hotkey_pressed(hotkey_get(ui, id), io, repeat);
}

bool capture_hotkey_chord(HotkeyChord* out, bool* capture_cancelled) {
  if (capture_cancelled) *capture_cancelled = false;
  if (!out) return false;

  const ImGuiIO& io = ImGui::GetIO();

  // Escape cancels capture (unless used with modifiers).
  if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt && !io.KeySuper) {
    if (capture_cancelled) *capture_cancelled = true;
    return false;
  }

  for (int k = static_cast<int>(ImGuiKey_NamedKey_BEGIN); k < static_cast<int>(ImGuiKey_NamedKey_END); ++k) {
    const ImGuiKey key = static_cast<ImGuiKey>(k);
    if (is_modifier_key(key)) continue;
    if (!ImGui::IsKeyPressed(key)) continue;
    HotkeyChord c;
    c.ctrl = io.KeyCtrl;
    c.shift = io.KeyShift;
    c.alt = io.KeyAlt;
    c.super = io.KeySuper;
    c.key = k;
    *out = c;
    return true;
  }

  return false;
}

std::string export_hotkeys_text(const UIState& ui) {
  std::ostringstream oss;
  oss << "nebula-hotkeys-v1\n";
  for (const auto& d : hotkey_defs()) {
    const HotkeyChord c = hotkey_get(ui, d.id);
    std::string s = hotkey_to_string(c);
    if (s.empty()) s = "Unbound";
    oss << d.id << "=" << s << "\n";
  }
  return oss.str();
}

bool import_hotkeys_text(UIState& ui, std::string_view text, std::string* error) {
  if (error) error->clear();

  std::istringstream iss(std::string(text));
  std::string line;
  int line_no = 0;
  int applied = 0;
  int failed = 0;

  while (std::getline(iss, line)) {
    ++line_no;
    // Trim.
    line = trim_ascii(line);
    if (line.empty()) continue;
    if (line[0] == '#') continue;
    if (line_no == 1 && nebula4x::to_lower(line) == "nebula-hotkeys-v1") continue;

    const auto eq = line.find('=');
    if (eq == std::string::npos) {
      ++failed;
      if (error && error->empty()) *error = "Invalid line (missing '=') at line " + std::to_string(line_no);
      continue;
    }

    const std::string id = trim_ascii(std::string_view(line).substr(0, eq));
    const std::string rhs = trim_ascii(std::string_view(line).substr(eq + 1));
    if (id.empty()) {
      ++failed;
      if (error && error->empty()) *error = "Empty hotkey id at line " + std::to_string(line_no);
      continue;
    }

    HotkeyChord c;
    std::string perr;
    if (!parse_hotkey(rhs, &c, &perr)) {
      ++failed;
      if (error && error->empty()) {
        *error = "Failed to parse hotkey for '" + id + "' at line " + std::to_string(line_no) + ": " + perr;
      }
      continue;
    }
    hotkey_set(ui, id, c);
    ++applied;
  }

  if (failed > 0) return false;
  (void)applied;
  return true;
}

} // namespace nebula4x::ui
