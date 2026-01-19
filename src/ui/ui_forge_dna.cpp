#include "ui/ui_forge_dna.h"

#include <algorithm>
#include <cctype>
#include <string>

#include "nebula4x/util/json.h"

namespace nebula4x::ui {
namespace {

constexpr const char* kPrefix = "nebula-uiforge-panel-v1";

std::string ltrim_copy(std::string s) {
  std::size_t i = 0;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  if (i > 0) s.erase(0, i);
  return s;
}

std::string normalize_json_pointer_copy(std::string p) {
  if (p.empty()) return "/";
  // Accept root as either "/" or "".
  if (p == "") return "/";
  if (!p.empty() && p[0] != '/') p = "/" + p;
  return p;
}

double get_num(const nebula4x::json::Object& o, const char* key, double def) {
  const auto it = o.find(key);
  if (it == o.end()) return def;
  return it->second.number_value(def);
}

bool get_bool(const nebula4x::json::Object& o, const char* key, bool def) {
  const auto it = o.find(key);
  if (it == o.end()) return def;
  return it->second.bool_value(def);
}

std::string get_str(const nebula4x::json::Object& o, const char* key, const std::string& def) {
  const auto it = o.find(key);
  if (it == o.end()) return def;
  return it->second.string_value(def);
}

} // namespace

std::string encode_ui_forge_panel_dna(const UiForgePanelConfig& panel) {
  nebula4x::json::Object o;
  o["v"] = 1.0;
  o["name"] = panel.name;
  o["root"] = normalize_json_pointer_copy(panel.root_path);
  o["open"] = panel.open;
  o["cols"] = static_cast<double>(panel.desired_columns);
  o["w_em"] = static_cast<double>(panel.card_width_em);

  nebula4x::json::Array wa;
  wa.reserve(panel.widgets.size());

  for (const auto& w : panel.widgets) {
    nebula4x::json::Object wo;
    wo["type"] = static_cast<double>(w.type);
    if (!w.label.empty()) wo["label"] = w.label;

    // Per-type fields.
    if (w.type == 0 || w.type == 3) {
      wo["path"] = normalize_json_pointer_copy(w.path);
      wo["is_query"] = w.is_query;
    }

    if (w.type == 0) {
      wo["query_op"] = static_cast<double>(w.query_op);
      wo["track_history"] = w.track_history;
      wo["show_sparkline"] = w.show_sparkline;
      wo["history_len"] = static_cast<double>(w.history_len);
    } else if (w.type == 1) {
      wo["text"] = w.text;
    } else if (w.type == 3) {
      wo["preview_rows"] = static_cast<double>(w.preview_rows);
    }

    wo["span"] = static_cast<double>(w.span);

    wa.push_back(nebula4x::json::object(std::move(wo)));
  }

  o["widgets"] = nebula4x::json::array(std::move(wa));

  const std::string json = nebula4x::json::stringify(nebula4x::json::object(std::move(o)), 0);
  return std::string(kPrefix) + " " + json;
}

bool decode_ui_forge_panel_dna(const std::string& text_in, UiForgePanelConfig* out_panel, std::string* error) {
  if (out_panel == nullptr) return false;

  try {
    std::string text = ltrim_copy(text_in);
    if (text.empty()) {
      if (error) *error = "Empty clipboard.";
      return false;
    }

    // Accept either the prefixed DNA or raw JSON.
    std::string json_text;
    if (text.rfind(kPrefix, 0) == 0) {
      const std::size_t brace = text.find('{');
      if (brace == std::string::npos) {
        if (error) *error = "Panel DNA missing JSON object.";
        return false;
      }
      json_text = text.substr(brace);
    } else {
      // If it's not JSON, try to locate the first object.
      const std::size_t brace = text.find('{');
      if (brace != std::string::npos) {
        json_text = text.substr(brace);
      } else {
        json_text = text;
      }
    }

    nebula4x::json::Value v = nebula4x::json::parse(json_text);
    if (!v.is_object()) {
      if (error) *error = "Panel DNA JSON must be an object.";
      return false;
    }

    const auto& o = v.object_items();

    // Version is currently informational (v1). Unknown versions are tolerated.
    (void)static_cast<int>(get_num(o, "v", 0.0));

    UiForgePanelConfig p = *out_panel; // start from caller defaults
    p.name = get_str(o, "name", p.name);
    p.root_path = normalize_json_pointer_copy(get_str(o, "root", p.root_path));
    p.open = get_bool(o, "open", p.open);
    p.desired_columns = static_cast<int>(get_num(o, "cols", p.desired_columns));
    p.card_width_em = static_cast<float>(get_num(o, "w_em", p.card_width_em));

    // Clamp.
    p.desired_columns = std::clamp(p.desired_columns, 0, 12);
    p.card_width_em = std::clamp(p.card_width_em, 10.0f, 60.0f);

    p.widgets.clear();

    const auto it = o.find("widgets");
    if (it != o.end() && it->second.is_array()) {
      for (const auto& wv : it->second.array_items()) {
        if (!wv.is_object()) continue;
        const auto& wo = wv.object_items();

        UiForgeWidgetConfig w;
        w.type = static_cast<int>(get_num(wo, "type", w.type));
        w.label = get_str(wo, "label", w.label);
        w.span = static_cast<int>(get_num(wo, "span", w.span));

        // Normalize/clamp common values.
        w.type = std::clamp(w.type, 0, 3);
        w.span = std::clamp(w.span, 1, 12);

        if (w.type == 0 || w.type == 3) {
          w.path = normalize_json_pointer_copy(get_str(wo, "path", w.path));
          w.is_query = get_bool(wo, "is_query", w.is_query);
        }

        if (w.type == 0) {
          w.query_op = static_cast<int>(get_num(wo, "query_op", w.query_op));
          w.query_op = std::clamp(w.query_op, 0, 4);
          w.track_history = get_bool(wo, "track_history", w.track_history);
          w.show_sparkline = get_bool(wo, "show_sparkline", w.show_sparkline);
          w.history_len = static_cast<int>(get_num(wo, "history_len", w.history_len));
          w.history_len = std::clamp(w.history_len, 2, 4000);
        } else if (w.type == 1) {
          w.text = get_str(wo, "text", w.text);
        } else if (w.type == 3) {
          w.preview_rows = static_cast<int>(get_num(wo, "preview_rows", w.preview_rows));
          w.preview_rows = std::clamp(w.preview_rows, 1, 100);
        }

        p.widgets.push_back(std::move(w));
      }
    }

    if (p.root_path.empty()) p.root_path = "/";

    *out_panel = std::move(p);
    return true;

  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return false;
  }
}

} // namespace nebula4x::ui
