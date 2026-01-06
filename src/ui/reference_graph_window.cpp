#include "ui/reference_graph_window.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "nebula4x/util/json.h"
#include "nebula4x/util/json_pointer.h"

#include "ui/game_entity_index.h"
#include "ui/game_json_cache.h"

namespace nebula4x::ui {
namespace {

using nebula4x::json::Value;

struct EdgeKey {
  std::uint64_t from{0};
  std::uint64_t to{0};

  bool operator==(const EdgeKey& o) const { return from == o.from && to == o.to; }
};

struct EdgeKeyHash {
  std::size_t operator()(const EdgeKey& k) const noexcept {
    // Simple mix (good enough at our scales).
    std::uint64_t x = k.from;
    std::uint64_t y = k.to;
    x ^= (y + 0x9e3779b97f4a7c15ull + (x << 6) + (x >> 2));
    return static_cast<std::size_t>(x);
  }
};

struct GraphEdge {
  std::uint64_t from{0};
  std::uint64_t to{0};
  int count{1};
  std::string sample_ptr;
};

struct GraphNode {
  std::uint64_t id{0};
  std::string kind;
  std::string name;
  std::string path; // JSON pointer to the entity object (best effort)
  ImVec2 pos{0, 0};
  ImVec2 vel{0, 0};
  bool fixed{false};
};

struct ScanFrame {
  const Value* v{nullptr};
  std::string path;
  std::string token;      // current token (for strict id filters)
  std::string field_name; // nearest object field name
};

struct InboundScanState {
  std::uint64_t doc_revision{0};
  std::uint64_t target_id{0};
  bool running{false};
  bool done{false};
  bool capped{false};
  std::uint64_t scanned_nodes{0};
  std::vector<ScanFrame> stack;
};

struct GlobalScanState {
  std::uint64_t doc_revision{0};
  bool running{false};
  bool done{false};
  bool capped{false};

  std::vector<std::uint64_t> entity_ids;
  std::size_t next_idx{0};
  std::uint64_t processed{0};
};

struct PathState {
  std::uint64_t from{0};
  std::uint64_t to{0};
  bool undirected{false};
  bool auto_update{true};

  std::size_t last_node_count{0};
  std::size_t last_edge_count{0};

  bool has_path{false};
  std::string status;

  std::vector<std::uint64_t> nodes;
  std::unordered_set<std::uint64_t> node_set;
  std::unordered_set<EdgeKey, EdgeKeyHash> edge_set;
};

struct ReferenceGraphState {
  std::uint64_t doc_revision{0};
  bool doc_loaded{false};
  std::shared_ptr<const Value> root;

  // Mode + focus.
  bool global_mode{false};
  std::uint64_t focus_id{0};
  std::uint64_t selected_id{0};

  // Graph.
  std::unordered_map<std::uint64_t, GraphNode> nodes;
  std::vector<GraphEdge> edges;
  std::unordered_map<EdgeKey, std::size_t, EdgeKeyHash> edge_map;
  std::unordered_set<std::uint64_t> expanded_out;
  std::unordered_set<std::uint64_t> expanded_in;

  // Hard cap for new edges (0 = unlimited).
  int max_edges{0};

  // View.
  ImVec2 pan{0, 0};
  float zoom{1.0f};
  bool show_grid{true};

  // Force layout.
  float repulsion{4200.0f};
  float spring_k{0.05f};
  float damping{0.90f};

  // UI.
  bool focus_id_input_next{false};
  char name_query[128]{0};
  char node_filter[64]{0}; // display filter

  // Requests consumed by the canvas each frame.
  bool request_fit{false};
  bool request_center_focus{false};
  bool request_center_selection{false};

  // Scan modes.
  InboundScanState inbound_scan;
  GlobalScanState global_scan;

  // Path highlighting.
  PathState path;

  // Position restore across snapshot refreshes.
  std::unordered_map<std::uint64_t, ImVec2> restore_pos;
  std::unordered_map<std::uint64_t, bool> restore_fixed;
};

ReferenceGraphState& st() {
  static ReferenceGraphState s;
  return s;
}

bool is_digits(std::string_view s) {
  if (s.empty()) return false;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

bool ends_with(std::string_view s, std::string_view suf) {
  if (s.size() < suf.size()) return false;
  return s.substr(s.size() - suf.size()) == suf;
}

std::string to_lower_copy(std::string_view s) {
  std::string out;
  out.resize(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
  return out;
}

bool icontains(std::string_view hay, std::string_view needle) {
  if (needle.empty()) return true;
  const std::string h = to_lower_copy(hay);
  const std::string n = to_lower_copy(needle);
  return h.find(n) != std::string::npos;
}

// Deterministic seed position based on id.
ImVec2 seeded_pos(std::uint64_t id) {
  const float a = static_cast<float>((id % 360) * (3.14159 / 180.0));
  const float r = 120.0f + static_cast<float>((id % 97) * 2);
  return ImVec2(std::cos(a) * r, std::sin(a) * r);
}

bool node_matches_filter(const GraphNode& n, std::string_view q) {
  if (q.empty()) return true;
  if (icontains(n.kind, q)) return true;
  if (icontains(n.name, q)) return true;
  // allow searching by id substring
  const std::string id = std::to_string(static_cast<unsigned long long>(n.id));
  if (icontains(id, q)) return true;
  return false;
}

GraphNode* ensure_node(ReferenceGraphState& s, std::uint64_t id) {
  auto it = s.nodes.find(id);
  if (it != s.nodes.end()) return &it->second;

  GraphNode n;
  n.id = id;

  if (const auto* ent = find_game_entity(id)) {
    n.kind = ent->kind;
    n.name = ent->name;
    n.path = ent->path;
  }

  // Restore position if available.
  if (auto itp = s.restore_pos.find(id); itp != s.restore_pos.end()) {
    n.pos = itp->second;
    s.restore_pos.erase(itp);
  } else {
    n.pos = seeded_pos(id);
  }
  if (auto itf = s.restore_fixed.find(id); itf != s.restore_fixed.end()) {
    n.fixed = itf->second;
    s.restore_fixed.erase(itf);
  }

  auto [it2, ok] = s.nodes.emplace(id, std::move(n));
  (void)ok;
  return &it2->second;
}

bool add_edge(ReferenceGraphState& s, std::uint64_t from, std::uint64_t to, std::string_view sample_ptr) {
  if (from == 0 || to == 0) return false;
  if (from == to) return false;

  const EdgeKey k{from, to};
  if (auto it = s.edge_map.find(k); it != s.edge_map.end()) {
    GraphEdge& e = s.edges[it->second];
    e.count += 1;
    if (e.sample_ptr.empty() && !sample_ptr.empty()) e.sample_ptr = std::string(sample_ptr);
    return true;
  }

  if (s.max_edges > 0 && static_cast<int>(s.edges.size()) >= s.max_edges) {
    return false;
  }

  GraphEdge e;
  e.from = from;
  e.to = to;
  e.count = 1;
  if (!sample_ptr.empty()) e.sample_ptr = std::string(sample_ptr);

  s.edge_map.emplace(k, s.edges.size());
  s.edges.push_back(std::move(e));
  return true;
}

void clear_graph(ReferenceGraphState& s) {
  s.nodes.clear();
  s.edges.clear();
  s.edge_map.clear();
  s.expanded_out.clear();
  s.expanded_in.clear();
  s.selected_id = 0;

  s.inbound_scan = InboundScanState{};
  s.global_scan = GlobalScanState{};

  // Path cache invalid (endpoints kept).
  s.path.last_node_count = 0;
  s.path.last_edge_count = 0;
  s.path.nodes.clear();
  s.path.node_set.clear();
  s.path.edge_set.clear();
  s.path.has_path = false;
  s.path.status.clear();
}

// Heuristic: accept ids only when the surrounding context looks like an entity id reference.
// This is best-effort because game JSON is arbitrary.
bool accept_id_by_context(std::string_view token, std::string_view field_name, bool strict_id_keys) {
  if (!strict_id_keys) return true;

  // If token is an array index, rely on the nearest field name.
  const bool token_is_index = is_digits(token);

  auto good_key = [](std::string_view k) -> bool {
    // common patterns: "id", "...Id", "...Ids"
    if (k == "id") return true;
    if (k.size() >= 2 && (ends_with(k, "Id") || ends_with(k, "ID"))) return true;
    if (k.size() >= 3 && (ends_with(k, "Ids") || ends_with(k, "IDs"))) return true;
    if (k.find("_id") != std::string_view::npos) return true;
    if (k.find("_ids") != std::string_view::npos) return true;
    return false;
  };

  if (!token_is_index && good_key(token)) return true;
  if (good_key(field_name)) return true;

  return false;
}

void scan_outbound_from_entity(ReferenceGraphState& s,
                              const Value& root,
                              std::uint64_t from_id,
                              bool strict_id_keys,
                              int max_additional_nodes,
                              int max_scan_nodes) {
  const auto* ent = find_game_entity(from_id);
  if (!ent) return;

  const Value* entity_v = nebula4x::resolve_json_pointer(root, ent->path, /*accept_root_slash=*/true);
  if (!entity_v) return;

  ensure_node(s, from_id);

  std::vector<ScanFrame> stack;
  stack.reserve(2048);
  stack.push_back(ScanFrame{entity_v, ent->path, /*token*/"", /*field_name*/""});

  int scanned = 0;

  while (!stack.empty()) {
    if (max_scan_nodes > 0 && scanned++ >= max_scan_nodes) break;
    if (max_additional_nodes > 0 && static_cast<int>(s.nodes.size()) >= max_additional_nodes) break;
    if (s.max_edges > 0 && static_cast<int>(s.edges.size()) >= s.max_edges) break;

    ScanFrame f = std::move(stack.back());
    stack.pop_back();
    if (!f.v) continue;

    // Look for numbers that correspond to known entity ids.
    if (f.v->is_number()) {
      std::uint64_t to_id = 0;
      if (json_to_u64_id(*f.v, to_id) && to_id != 0 && to_id != from_id) {
        // Only keep edges to entity ids that exist in the entity index.
        if (find_game_entity(to_id) && accept_id_by_context(f.token, f.field_name, strict_id_keys)) {
          ensure_node(s, to_id);
          add_edge(s, from_id, to_id, f.path);
        }
      }
      continue;
    }

    if (const auto* obj = f.v->as_object()) {
      for (const auto& kv : *obj) {
        ScanFrame c;
        c.v = &kv.second;
        c.token = kv.first;
        c.field_name = kv.first;
        c.path = nebula4x::json_pointer_join(f.path, kv.first);
        stack.push_back(std::move(c));
      }
      continue;
    }

    if (const auto* arr = f.v->as_array()) {
      for (std::size_t i = 0; i < arr->size(); ++i) {
        ScanFrame c;
        c.v = &(*arr)[i];
        c.token = std::to_string(i);
        c.field_name = f.field_name;
        c.path = nebula4x::json_pointer_join_index(f.path, i);
        stack.push_back(std::move(c));
      }
      continue;
    }
  }
}

void start_inbound_scan(ReferenceGraphState& s, std::uint64_t target_id, std::uint64_t doc_rev, const Value* root) {
  s.inbound_scan = InboundScanState{};
  s.inbound_scan.doc_revision = doc_rev;
  s.inbound_scan.target_id = target_id;
  s.inbound_scan.running = true;
  s.inbound_scan.done = false;
  s.inbound_scan.capped = false;
  s.inbound_scan.scanned_nodes = 0;
  s.inbound_scan.stack.reserve(4096);
  s.inbound_scan.stack.push_back(ScanFrame{root, "/", "", ""});
}

void rebuild_focus_graph(ReferenceGraphState& s, const Value& root, std::uint64_t focus_id, const UIState& ui) {
  clear_graph(s);
  s.focus_id = focus_id;

  if (focus_id == 0) return;

  ensure_node(s, focus_id);

  if (ui.reference_graph_show_outbound) {
    scan_outbound_from_entity(s, root, focus_id, ui.reference_graph_strict_id_keys, ui.reference_graph_max_nodes,
                             /*max_scan_nodes=*/200000);
    s.expanded_out.insert(focus_id);
  }

  if (ui.reference_graph_show_inbound) {
    start_inbound_scan(s, focus_id, s.doc_revision, &root);
    s.expanded_in.insert(focus_id);
  }
}

void start_global_graph(ReferenceGraphState& s, const UIState& ui) {
  clear_graph(s);

  s.global_scan = GlobalScanState{};
  s.global_scan.doc_revision = s.doc_revision;
  s.global_scan.running = true;
  s.global_scan.done = false;
  s.global_scan.capped = false;

  const auto& idx = game_entity_index();
  s.global_scan.entity_ids.reserve(idx.by_id.size());
  for (const auto& kv : idx.by_id) {
    s.global_scan.entity_ids.push_back(kv.first);
  }
  std::sort(s.global_scan.entity_ids.begin(), s.global_scan.entity_ids.end());

  // Prefer focusing/visiting the focus id early.
  if (ui.reference_graph_focus_id != 0) {
    auto it = std::lower_bound(s.global_scan.entity_ids.begin(), s.global_scan.entity_ids.end(), ui.reference_graph_focus_id);
    if (it != s.global_scan.entity_ids.end() && *it == ui.reference_graph_focus_id) {
      std::rotate(s.global_scan.entity_ids.begin(), it, it + 1);
    }
    ensure_node(s, ui.reference_graph_focus_id);
    s.focus_id = ui.reference_graph_focus_id;
  }
}

void step_global_graph(ReferenceGraphState& s, const Value& root, const UIState& ui) {
  if (!s.global_scan.running || s.global_scan.done) return;
  if (s.global_scan.doc_revision != s.doc_revision) {
    // Snapshot changed while we were scanning.
    s.global_scan.running = false;
    s.global_scan.done = true;
    return;
  }

  const int entities_per_frame = std::clamp(ui.reference_graph_entities_per_frame, 1, 500);
  for (int i = 0; i < entities_per_frame; ++i) {
    if (s.global_scan.next_idx >= s.global_scan.entity_ids.size()) {
      s.global_scan.running = false;
      s.global_scan.done = true;
      break;
    }

    const std::uint64_t from_id = s.global_scan.entity_ids[s.global_scan.next_idx++];
    s.global_scan.processed++;

    if (ui.reference_graph_max_nodes > 0 && static_cast<int>(s.nodes.size()) >= ui.reference_graph_max_nodes) {
      s.global_scan.capped = true;
      s.global_scan.running = false;
      s.global_scan.done = true;
      break;
    }
    if (s.max_edges > 0 && static_cast<int>(s.edges.size()) >= s.max_edges) {
      s.global_scan.capped = true;
      s.global_scan.running = false;
      s.global_scan.done = true;
      break;
    }

    ensure_node(s, from_id);
    scan_outbound_from_entity(s, root, from_id, ui.reference_graph_strict_id_keys, ui.reference_graph_max_nodes,
                             ui.reference_graph_scan_nodes_per_entity);
    s.expanded_out.insert(from_id);

    if (ui.reference_graph_max_nodes > 0 && static_cast<int>(s.nodes.size()) >= ui.reference_graph_max_nodes) {
      s.global_scan.capped = true;
      s.global_scan.running = false;
      s.global_scan.done = true;
      break;
    }
    if (s.max_edges > 0 && static_cast<int>(s.edges.size()) >= s.max_edges) {
      s.global_scan.capped = true;
      s.global_scan.running = false;
      s.global_scan.done = true;
      break;
    }
  }
}

bool compute_shortest_path(const ReferenceGraphState& s,
                           std::uint64_t from,
                           std::uint64_t to,
                           bool undirected,
                           std::vector<std::uint64_t>& out_nodes,
                           std::string& out_status) {
  out_nodes.clear();
  out_status.clear();

  if (from == 0 || to == 0) {
    out_status = "Set both endpoints.";
    return false;
  }
  if (from == to) {
    out_nodes.push_back(from);
    out_status = "Same node.";
    return true;
  }
  if (s.nodes.find(from) == s.nodes.end() || s.nodes.find(to) == s.nodes.end()) {
    out_status = "Endpoints not present in the current graph.";
    return false;
  }

  // Build adjacency.
  std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> adj;
  adj.reserve(s.nodes.size());

  for (const auto& e : s.edges) {
    adj[e.from].push_back(e.to);
    if (undirected) {
      adj[e.to].push_back(e.from);
    }
  }

  std::unordered_map<std::uint64_t, std::uint64_t> parent;
  parent.reserve(s.nodes.size() * 2);

  std::deque<std::uint64_t> q;
  q.push_back(from);
  parent[from] = 0;

  bool found = false;
  while (!q.empty()) {
    const std::uint64_t cur = q.front();
    q.pop_front();

    if (cur == to) {
      found = true;
      break;
    }

    auto it = adj.find(cur);
    if (it == adj.end()) continue;

    for (const std::uint64_t nxt : it->second) {
      if (parent.find(nxt) != parent.end()) continue;
      parent[nxt] = cur;
      q.push_back(nxt);
      if (nxt == to) {
        found = true;
        q.clear();
        break;
      }
    }
  }

  if (!found) {
    out_status = "No path found (within current graph).";
    return false;
  }

  // Reconstruct path.
  std::vector<std::uint64_t> rev;
  for (std::uint64_t v = to; v != 0; v = parent[v]) {
    rev.push_back(v);
    if (v == from) break;
  }
  if (rev.empty() || rev.back() != from) {
    out_status = "No path found (broken reconstruction).";
    return false;
  }

  out_nodes.assign(rev.rbegin(), rev.rend());
  out_status = "OK";
  return true;
}

void update_path_cache(ReferenceGraphState& s) {
  PathState& p = s.path;

  const bool endpoints_set = (p.from != 0 && p.to != 0);
  if (!endpoints_set) {
    p.nodes.clear();
    p.node_set.clear();
    p.edge_set.clear();
    p.has_path = false;
    p.status.clear();
    p.last_edge_count = s.edges.size();
    p.last_node_count = s.nodes.size();
    return;
  }

  const bool graph_changed = (p.last_edge_count != s.edges.size()) || (p.last_node_count != s.nodes.size());
  if (!p.auto_update && !p.nodes.empty() && !graph_changed) return;
  if (!p.auto_update && !p.nodes.empty() && graph_changed) {
    // keep cached but mark status
    p.status = "Graph changed; click Find Path to refresh.";
    p.last_edge_count = s.edges.size();
    p.last_node_count = s.nodes.size();
    return;
  }

  std::vector<std::uint64_t> nodes;
  std::string status;
  const bool ok = compute_shortest_path(s, p.from, p.to, p.undirected, nodes, status);

  p.nodes = std::move(nodes);
  p.node_set.clear();
  p.edge_set.clear();
  p.has_path = ok;

  if (ok) {
    for (std::uint64_t id : p.nodes) p.node_set.insert(id);
    for (std::size_t i = 1; i < p.nodes.size(); ++i) {
      const std::uint64_t a = p.nodes[i - 1];
      const std::uint64_t b = p.nodes[i];
      p.edge_set.insert(EdgeKey{a, b});
      if (p.undirected) p.edge_set.insert(EdgeKey{b, a});
    }
  }

  p.status = status;
  p.last_edge_count = s.edges.size();
  p.last_node_count = s.nodes.size();
}

std::string node_label(const GraphNode& n) {
  std::string out = "#" + std::to_string(static_cast<unsigned long long>(n.id));
  if (!n.kind.empty()) out = n.kind + " " + out;
  if (!n.name.empty()) out += "  " + n.name;
  return out;
}

ImVec2 to_screen(const ImVec2& origin, const ImVec2& world, const ImVec2& pan, float zoom) {
  return ImVec2(origin.x + (world.x + pan.x) * zoom, origin.y + (world.y + pan.y) * zoom);
}

ImVec2 to_world(const ImVec2& origin, const ImVec2& screen, const ImVec2& pan, float zoom) {
  return ImVec2((screen.x - origin.x) / zoom - pan.x, (screen.y - origin.y) / zoom - pan.y);
}

void draw_grid(ImDrawList* dl, const ImVec2& origin, const ImVec2& size, const ImVec2& pan, float zoom) {
  const float grid = 80.0f;
  const ImU32 col = IM_COL32(255, 255, 255, 14);

  const float step = grid * zoom;
  if (step < 12.0f) return;

  const float ox = origin.x + pan.x * zoom;
  const float oy = origin.y + pan.y * zoom;

  for (float x = std::fmod(ox, step); x < size.x; x += step) {
    dl->AddLine(ImVec2(origin.x + x, origin.y), ImVec2(origin.x + x, origin.y + size.y), col);
  }
  for (float y = std::fmod(oy, step); y < size.y; y += step) {
    dl->AddLine(ImVec2(origin.x, origin.y + y), ImVec2(origin.x + size.x, origin.y + y), col);
  }
}

void center_on(ReferenceGraphState& s, const ImVec2& canvas_sz, const ImVec2& world_pos) {
  const ImVec2 canvas_center(canvas_sz.x * 0.5f, canvas_sz.y * 0.5f);
  s.pan = ImVec2(canvas_center.x / s.zoom - world_pos.x, canvas_center.y / s.zoom - world_pos.y);
}

void fit_view(ReferenceGraphState& s, const ImVec2& canvas_sz) {
  if (s.nodes.empty()) return;

  float minx = std::numeric_limits<float>::infinity();
  float maxx = -std::numeric_limits<float>::infinity();
  float miny = std::numeric_limits<float>::infinity();
  float maxy = -std::numeric_limits<float>::infinity();

  for (const auto& kv : s.nodes) {
    const ImVec2 p = kv.second.pos;
    minx = std::min(minx, p.x);
    maxx = std::max(maxx, p.x);
    miny = std::min(miny, p.y);
    maxy = std::max(maxy, p.y);
  }

  const float w = std::max(1.0f, maxx - minx);
  const float h = std::max(1.0f, maxy - miny);
  const float margin = 120.0f;

  const float zx = canvas_sz.x / (w + margin * 2.0f);
  const float zy = canvas_sz.y / (h + margin * 2.0f);
  const float z = std::clamp(std::min(zx, zy), 0.15f, 3.5f);

  s.zoom = z;

  const ImVec2 world_center((minx + maxx) * 0.5f, (miny + maxy) * 0.5f);
  center_on(s, canvas_sz, world_center);
}

void apply_force_layout(ReferenceGraphState& s, float dt, bool enable) {
  if (!enable) return;
  if (s.nodes.size() < 2) return;

  dt = std::clamp(dt, 0.001f, 0.050f);

  // Build a compact pointer array for deterministic iteration.
  std::vector<GraphNode*> ns;
  ns.reserve(s.nodes.size());
  for (auto& kv : s.nodes) ns.push_back(&kv.second);

  // --- Repulsion ---
  //
  // For small graphs, do O(n^2).
  // For larger graphs, approximate repulsion using a spatial hash grid.
  const std::size_t n = ns.size();
  const bool use_spatial_hash = (n > 450);

  if (!use_spatial_hash) {
    for (std::size_t i = 0; i < n; ++i) {
      GraphNode& a = *ns[i];
      if (a.fixed) continue;

      for (std::size_t j = i + 1; j < n; ++j) {
        GraphNode& b = *ns[j];

        ImVec2 d = ImVec2(a.pos.x - b.pos.x, a.pos.y - b.pos.y);
        float dist2 = d.x * d.x + d.y * d.y + 1.0f;
        float inv = 1.0f / std::sqrt(dist2);

        const float f = s.repulsion / dist2;
        d.x *= inv * f;
        d.y *= inv * f;

        a.vel.x += d.x;
        a.vel.y += d.y;
        if (!b.fixed) {
          b.vel.x -= d.x;
          b.vel.y -= d.y;
        }
      }
    }
  } else {
    const float cell_size = 260.0f; // world units

    auto cell_key = [&](const ImVec2& p) -> std::int64_t {
      const int cx = static_cast<int>(std::floor(p.x / cell_size));
      const int cy = static_cast<int>(std::floor(p.y / cell_size));
      return (static_cast<std::int64_t>(cx) << 32) | static_cast<std::uint32_t>(cy);
    };

    std::unordered_map<std::int64_t, std::vector<GraphNode*>> grid;
    grid.reserve(n * 2);

    for (GraphNode* node : ns) {
      grid[cell_key(node->pos)].push_back(node);
    }

    auto decode = [](std::int64_t key, int& cx, int& cy) {
      cx = static_cast<int>(key >> 32);
      cy = static_cast<int>(static_cast<std::int32_t>(key & 0xffffffff));
    };

    const int neigh[5][2] = {{0, 0}, {0, 1}, {1, -1}, {1, 0}, {1, 1}};

    for (const auto& kv : grid) {
      int cx = 0, cy = 0;
      decode(kv.first, cx, cy);
      const auto& cellA = kv.second;

      for (int oi = 0; oi < 5; ++oi) {
        const int nx = cx + neigh[oi][0];
        const int ny = cy + neigh[oi][1];
        const std::int64_t nk = (static_cast<std::int64_t>(nx) << 32) | static_cast<std::uint32_t>(ny);

        auto itB = grid.find(nk);
        if (itB == grid.end()) continue;

        const auto& cellB = itB->second;

        if (nk == kv.first) {
          // Within same cell (pairwise, no duplicates).
          for (std::size_t i = 0; i < cellA.size(); ++i) {
            GraphNode& a = *cellA[i];
            if (a.fixed) continue;
            for (std::size_t j = i + 1; j < cellA.size(); ++j) {
              GraphNode& b = *cellA[j];

              ImVec2 d = ImVec2(a.pos.x - b.pos.x, a.pos.y - b.pos.y);
              float dist2 = d.x * d.x + d.y * d.y + 1.0f;
              float inv = 1.0f / std::sqrt(dist2);

              const float f = s.repulsion / dist2;
              d.x *= inv * f;
              d.y *= inv * f;

              a.vel.x += d.x;
              a.vel.y += d.y;
              if (!b.fixed) {
                b.vel.x -= d.x;
                b.vel.y -= d.y;
              }
            }
          }
        } else {
          // Across neighboring cells.
          for (GraphNode* pa : cellA) {
            GraphNode& a = *pa;
            if (a.fixed) continue;

            for (GraphNode* pb : cellB) {
              GraphNode& b = *pb;

              ImVec2 d = ImVec2(a.pos.x - b.pos.x, a.pos.y - b.pos.y);
              float dist2 = d.x * d.x + d.y * d.y + 1.0f;
              float inv = 1.0f / std::sqrt(dist2);

              const float f = s.repulsion / dist2;
              d.x *= inv * f;
              d.y *= inv * f;

              a.vel.x += d.x;
              a.vel.y += d.y;
              if (!b.fixed) {
                b.vel.x -= d.x;
                b.vel.y -= d.y;
              }
            }
          }
        }
      }
    }
  }

  // --- Springs on edges ---
  const float ideal_len = 220.0f;
  for (const GraphEdge& e : s.edges) {
    auto it_a = s.nodes.find(e.from);
    auto it_b = s.nodes.find(e.to);
    if (it_a == s.nodes.end() || it_b == s.nodes.end()) continue;

    GraphNode& a = it_a->second;
    GraphNode& b = it_b->second;

    ImVec2 d = ImVec2(b.pos.x - a.pos.x, b.pos.y - a.pos.y);
    float len = std::sqrt(d.x * d.x + d.y * d.y) + 1e-3f;
    float diff = (len - ideal_len) / len;

    const float k = s.spring_k * std::clamp(1.0f + 0.08f * static_cast<float>(e.count), 1.0f, 3.0f);
    const ImVec2 f = ImVec2(d.x * (diff * k), d.y * (diff * k));

    if (!a.fixed) {
      a.vel.x += f.x;
      a.vel.y += f.y;
    }
    if (!b.fixed) {
      b.vel.x -= f.x;
      b.vel.y -= f.y;
    }
  }

  // --- Integrate ---
  for (GraphNode* n0 : ns) {
    GraphNode& node = *n0;
    if (node.fixed) {
      node.vel = ImVec2(0, 0);
      continue;
    }

    node.vel.x *= s.damping;
    node.vel.y *= s.damping;
    node.pos.x += node.vel.x * dt;
    node.pos.y += node.vel.y * dt;
  }
}

void step_inbound_scan(ReferenceGraphState& s, const Value& root, const UIState& ui) {
  if (!s.inbound_scan.running) return;
  if (s.inbound_scan.doc_revision != s.doc_revision) {
    s.inbound_scan.running = false;
    s.inbound_scan.done = true;
    return;
  }

  const int budget = std::clamp(ui.reference_graph_nodes_per_frame, 50, 200000);
  const int max_nodes = ui.reference_graph_max_nodes;
  const bool strict = ui.reference_graph_strict_id_keys;

  // NOTE: in focus graph mode we only care about edges that point INTO target_id.
  const std::uint64_t target_id = s.inbound_scan.target_id;

  int steps = 0;
  while (!s.inbound_scan.stack.empty() && steps++ < budget) {
    if (max_nodes > 0 && static_cast<int>(s.nodes.size()) >= max_nodes) {
      s.inbound_scan.capped = true;
      s.inbound_scan.running = false;
      s.inbound_scan.done = true;
      break;
    }
    if (s.max_edges > 0 && static_cast<int>(s.edges.size()) >= s.max_edges) {
      s.inbound_scan.capped = true;
      s.inbound_scan.running = false;
      s.inbound_scan.done = true;
      break;
    }

    ScanFrame f = std::move(s.inbound_scan.stack.back());
    s.inbound_scan.stack.pop_back();
    if (!f.v) continue;

    s.inbound_scan.scanned_nodes++;

    // Identify the containing entity (best-effort) from path prefix /<kind>/<idx>/...
    auto containing_entity_id = [&](std::string_view ptr) -> std::uint64_t {
      // Path split on '/'; tokens are already escaped in pointers.
      std::uint64_t id = 0;
      if (ptr.size() < 2 || ptr[0] != '/') return 0;

      // Quick parse for "/kind/index/..."
      std::size_t p1 = ptr.find('/', 1);
      if (p1 == std::string_view::npos) return 0;
      std::size_t p2 = ptr.find('/', p1 + 1);
      if (p2 == std::string_view::npos) return 0;

      const std::string kind(ptr.substr(1, p1 - 1));
      const std::string idxs(ptr.substr(p1 + 1, p2 - (p1 + 1)));
      if (!is_digits(idxs)) return 0;

      // Resolve /kind/index/id
      std::string base = "/";
      base = nebula4x::json_pointer_join(base, kind);
      base = nebula4x::json_pointer_join(base, idxs);
      std::string pid = nebula4x::json_pointer_join(base, "id");

      const Value* v = nebula4x::resolve_json_pointer(root, pid, /*accept_root_slash=*/true);
      if (!v) return 0;
      if (!json_to_u64_id(*v, id)) return 0;
      return id;
    };

    if (f.v->is_number()) {
      std::uint64_t maybe = 0;
      if (json_to_u64_id(*f.v, maybe) && maybe == target_id && accept_id_by_context(f.token, f.field_name, strict)) {
        const std::uint64_t from_id = containing_entity_id(f.path);
        if (from_id != 0 && from_id != target_id && find_game_entity(from_id)) {
          ensure_node(s, from_id);
          ensure_node(s, target_id);
          add_edge(s, from_id, target_id, f.path);
        }
      }
      continue;
    }

    if (const auto* obj = f.v->as_object()) {
      for (const auto& kv : *obj) {
        ScanFrame c;
        c.v = &kv.second;
        c.token = kv.first;
        c.field_name = kv.first;
        c.path = nebula4x::json_pointer_join(f.path, kv.first);
        s.inbound_scan.stack.push_back(std::move(c));
      }
      continue;
    }

    if (const auto* arr = f.v->as_array()) {
      for (std::size_t i = 0; i < arr->size(); ++i) {
        ScanFrame c;
        c.v = &(*arr)[i];
        c.token = std::to_string(i);
        c.field_name = f.field_name;
        c.path = nebula4x::json_pointer_join_index(f.path, i);
        s.inbound_scan.stack.push_back(std::move(c));
      }
      continue;
    }
  }

  if (s.inbound_scan.stack.empty()) {
    s.inbound_scan.running = false;
    s.inbound_scan.done = true;
  }
}

std::string export_dot(const ReferenceGraphState& s) {
  std::string out;
  out.reserve(4096);

  out += "digraph Nebula4X_Refs {\n";
  out += "  rankdir=LR;\n";
  out += "  node [shape=box, fontname=\"Arial\"];\n";

  for (const auto& kv : s.nodes) {
    const GraphNode& n = kv.second;
    std::string label = node_label(n);
    for (char& c : label) {
      if (c == '"') c = '\'';
    }
    out += "  n" + std::to_string(static_cast<unsigned long long>(n.id)) + " [label=\"" + label + "\"];\n";
  }

  for (const GraphEdge& e : s.edges) {
    out += "  n" + std::to_string(static_cast<unsigned long long>(e.from)) + " -> n" +
           std::to_string(static_cast<unsigned long long>(e.to));
    if (e.count > 1) out += " [label=\"" + std::to_string(e.count) + "\"]";
    out += ";\n";
  }

  out += "}\n";
  return out;
}

} // namespace

void draw_reference_graph_window(Simulation& sim, UIState& ui) {
  if (!ui.show_reference_graph_window) return;

  ImGui::SetNextWindowSize(ImVec2(980, 740), ImGuiCond_FirstUseEver);

  if (!ImGui::Begin("Reference Graph (Entity IDs)", &ui.show_reference_graph_window)) {
    ImGui::End();
    return;
  }

  ReferenceGraphState& s = st();

  // Refresh live JSON snapshot (shared cache).
  const double now = ImGui::GetTime();
  const float refresh_sec = std::clamp(ui.reference_graph_refresh_sec, 0.0f, 60.0f);
  const bool force_refresh = (refresh_sec <= 0.0f);
  (void)ensure_game_json_cache(sim, now, refresh_sec, force_refresh);

  const auto& cache = game_json_cache();
  const std::uint64_t new_rev = cache.revision;

  s.doc_loaded = cache.loaded;
  s.root = cache.root;

  if (!s.doc_loaded || !s.root) {
    ImGui::TextDisabled("No live game JSON snapshot available.");
    if (!cache.error.empty()) {
      ImGui::Separator();
      ImGui::TextDisabled("Error: %s", cache.error.c_str());
    }
    ImGui::End();
    return;
  }

  // Ensure entity index is up-to-date for this snapshot.
  (void)ensure_game_entity_index(*s.root, new_rev);

  // Keep UI prefs sane.
  ui.reference_graph_refresh_sec = std::clamp(ui.reference_graph_refresh_sec, 0.0f, 60.0f);
  ui.reference_graph_nodes_per_frame = std::clamp(ui.reference_graph_nodes_per_frame, 50, 200000);
  ui.reference_graph_max_nodes = std::clamp(ui.reference_graph_max_nodes, 20, 2000);
  ui.reference_graph_entities_per_frame = std::clamp(ui.reference_graph_entities_per_frame, 1, 500);
  ui.reference_graph_scan_nodes_per_entity = std::clamp(ui.reference_graph_scan_nodes_per_entity, 500, 500000);
  ui.reference_graph_max_edges = std::clamp(ui.reference_graph_max_edges, 50, 500000);

  s.max_edges = ui.reference_graph_max_edges;

  const bool doc_changed = (s.doc_revision != new_rev);
  const bool focus_changed = (s.focus_id != ui.reference_graph_focus_id);
  const bool mode_changed = (s.global_mode != ui.reference_graph_global_mode);

  if (doc_changed && !mode_changed && !focus_changed) {
    // Preserve positions when the snapshot refreshes but the user's focus/mode stays stable.
    s.restore_pos.clear();
    s.restore_fixed.clear();
    s.restore_pos.reserve(s.nodes.size());
    s.restore_fixed.reserve(s.nodes.size());
    for (const auto& kv : s.nodes) {
      s.restore_pos.emplace(kv.first, kv.second.pos);
      s.restore_fixed.emplace(kv.first, kv.second.fixed);
    }
  } else if (doc_changed || mode_changed || focus_changed) {
    s.restore_pos.clear();
    s.restore_fixed.clear();
  }

  if (doc_changed || focus_changed || mode_changed) {
    s.doc_revision = new_rev;
    s.global_mode = ui.reference_graph_global_mode;
    s.focus_id = ui.reference_graph_focus_id;

    if (s.global_mode) {
      start_global_graph(s, ui);
    } else {
      rebuild_focus_graph(s, *s.root, ui.reference_graph_focus_id, ui);
    }
  } else {
    s.doc_revision = new_rev;
  }

  // Keep path highlight in sync with the currently displayed graph.
  update_path_cache(s);

  if (ImGui::BeginTable("##refgraph_layout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
    // --- Left panel ---
    ImGui::TableNextColumn();
    ImGui::BeginChild("##refgraph_left", ImVec2(0, 0), false);

    ImGui::SeparatorText("Mode");

    if (ImGui::Checkbox("Global graph (scan all entities)", &ui.reference_graph_global_mode)) {
      // rebuild handled next frame by mode_changed
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("When enabled, the tool incrementally scans ALL entities in the snapshot and\n"
                        "builds a reference graph (entity -> referenced entity ids).\n"
                        "This can be heavier but enables paths and richer connectivity.\n\n"
                        "Tip: tune Entities/frame and Scan nodes/entity.");
    }

    ImGui::SeparatorText("Focus");

    if (s.focus_id_input_next) {
      ImGui::SetKeyboardFocusHere();
      s.focus_id_input_next = false;
    }

    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputScalar("##focus_id", ImGuiDataType_U64, &ui.reference_graph_focus_id)) {
      // handled by rebuild on next frame
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Entity id to focus.\nTip: right-click ids in other tools to open here.");
    }

    ImGui::Spacing();

    if (!ui.reference_graph_global_mode) {
      ImGui::Checkbox("Outbound edges", &ui.reference_graph_show_outbound);
      ImGui::Checkbox("Inbound edges (scan)", &ui.reference_graph_show_inbound);
    } else {
      ImGui::BeginDisabled();
      bool dummy = true;
      ImGui::Checkbox("Outbound edges", &dummy);
      ImGui::Checkbox("Inbound edges (scan)", &dummy);
      ImGui::EndDisabled();
    }

    ImGui::Checkbox("Strict id keys", &ui.reference_graph_strict_id_keys);
    ImGui::Checkbox("Auto layout", &ui.reference_graph_auto_layout);
    ImGui::Checkbox("Grid", &s.show_grid);

    ImGui::Spacing();

    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("Refresh (s)", &ui.reference_graph_refresh_sec, 0.0f, 5.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);

    if (!ui.reference_graph_global_mode) {
      ImGui::SetNextItemWidth(-1);
      ImGui::SliderInt("Nodes/frame", &ui.reference_graph_nodes_per_frame, 50, 20000, "%d", ImGuiSliderFlags_Logarithmic);
    } else {
      ImGui::SetNextItemWidth(-1);
      ImGui::SliderInt("Entities/frame", &ui.reference_graph_entities_per_frame, 1, 120, "%d", ImGuiSliderFlags_Logarithmic);
      ImGui::SetNextItemWidth(-1);
      ImGui::SliderInt("Scan nodes/entity", &ui.reference_graph_scan_nodes_per_entity, 500, 200000, "%d",
                       ImGuiSliderFlags_Logarithmic);
    }

    ImGui::SetNextItemWidth(-1);
    ImGui::SliderInt("Max nodes", &ui.reference_graph_max_nodes, 20, 1000);
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderInt("Max edges", &ui.reference_graph_max_edges, 50, 40000, "%d", ImGuiSliderFlags_Logarithmic);

    if (ImGui::Button(ui.reference_graph_global_mode ? "Restart scan" : "Rebuild graph")) {
      if (ui.reference_graph_global_mode) {
        start_global_graph(s, ui);
      } else {
        rebuild_focus_graph(s, *s.root, ui.reference_graph_focus_id, ui);
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear pins")) {
      for (auto& kv : s.nodes) kv.second.fixed = false;
    }

    if (ui.reference_graph_global_mode) {
      ImGui::SameLine();
      if (s.global_scan.running) {
        if (ImGui::Button("Pause")) s.global_scan.running = false;
      } else if (!s.global_scan.done && !s.global_scan.entity_ids.empty()) {
        if (ImGui::Button("Resume")) s.global_scan.running = true;
      }
    }

    ImGui::Spacing();
    if (ImGui::Button("Copy Graphviz DOT")) {
      const std::string dot = export_dot(s);
      ImGui::SetClipboardText(dot.c_str());
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Copies a DOT graph to the clipboard.\nPaste into Graphviz or an online DOT viewer.");
    }

    ImGui::SeparatorText("View");

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##node_filter", "Display filter (kind/name/id)", s.node_filter, IM_ARRAYSIZE(s.node_filter));

    if (ImGui::Button("Center focus")) s.request_center_focus = true;
    ImGui::SameLine();
    if (ImGui::Button("Center selection")) s.request_center_selection = true;
    ImGui::SameLine();
    if (ImGui::Button("Fit")) s.request_fit = true;

    ImGui::TextDisabled("Zoom: %.2fx  Nodes: %zu  Edges: %zu", s.zoom, s.nodes.size(), s.edges.size());

    if (ui.reference_graph_global_mode) {
      const auto total = static_cast<unsigned long long>(s.global_scan.entity_ids.size());
      const auto done = static_cast<unsigned long long>(s.global_scan.processed);
      if (total > 0) {
        const float frac = static_cast<float>(done) / static_cast<float>(total);
        ImGui::ProgressBar(frac, ImVec2(-1, 0),
                           (std::to_string(done) + "/" + std::to_string(total) + " entities").c_str());
      }
      if (s.global_scan.done) {
        ImGui::TextDisabled("Global scan: done%s", s.global_scan.capped ? " (capped)" : "");
      } else if (s.global_scan.running) {
        ImGui::TextDisabled("Global scan: running...");
      } else if (!s.global_scan.entity_ids.empty()) {
        ImGui::TextDisabled("Global scan: paused");
      }
    } else {
      if (s.inbound_scan.running && s.inbound_scan.target_id == s.focus_id) {
        ImGui::TextDisabled("Inbound scan: %llu nodes scanned...",
                            static_cast<unsigned long long>(s.inbound_scan.scanned_nodes));
      } else if (s.inbound_scan.done && s.inbound_scan.target_id == s.focus_id) {
        ImGui::TextDisabled("Inbound scan: done (%llu scanned)%s",
                            static_cast<unsigned long long>(s.inbound_scan.scanned_nodes),
                            s.inbound_scan.capped ? " (capped)" : "");
      }
    }

    ImGui::SeparatorText("Search");

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##name_query", "Search by kind/name/id (click to focus)", s.name_query,
                             IM_ARRAYSIZE(s.name_query));
    const std::string q = s.name_query;

    if (!q.empty()) {
      std::vector<const GameEntityIndexEntry*> hits;
      hits.reserve(128);

      const auto& idx = game_entity_index();
      for (const auto& kv : idx.by_id) {
        const GameEntityIndexEntry& e = kv.second;
        std::string label = e.kind + " #" + std::to_string(e.id);
        if (!e.name.empty()) label += " " + e.name;
        if (icontains(label, q)) {
          hits.push_back(&e);
          if (hits.size() >= 120) break;
        }
      }

      if (!hits.empty()) {
        ImGui::BeginChild("##hits", ImVec2(0, 140), true);
        for (std::size_t i = 0; i < hits.size(); ++i) {
          const GameEntityIndexEntry& e = *hits[i];
          std::string label = e.kind + " #" + std::to_string(e.id);
          if (!e.name.empty()) label += "  " + e.name;
          if (ImGui::Selectable(label.c_str(), false)) {
            ui.reference_graph_focus_id = e.id;
            s.focus_id_input_next = true;
            if (ui.reference_graph_global_mode) {
              // In global mode we don't rebuild on focus changes, so ensure the node exists now.
              ensure_node(s, e.id);
            }
          }
        }
        ImGui::EndChild();
      } else {
        ImGui::TextDisabled("No matches.");
      }
    } else {
      ImGui::TextDisabled("Type to search the live entity index.");
    }

    ImGui::SeparatorText("Selection");

    const std::uint64_t sel = (s.selected_id != 0) ? s.selected_id : s.focus_id;
    if (sel != 0) {
      const GraphNode* n = nullptr;
      if (auto it = s.nodes.find(sel); it != s.nodes.end()) n = &it->second;

      if (n) {
        const std::string lbl = node_label(*n);
        ImGui::TextWrapped("%s", lbl.c_str());

        if (!n->path.empty()) {
          ImGui::TextDisabled("Path: %s", n->path.c_str());
        }

        if (ImGui::Button("Open in JSON Explorer") && !n->path.empty()) {
          ui.show_json_explorer_window = true;
          ui.request_json_explorer_goto_path = n->path;
        }
        ImGui::SameLine();
        if (ImGui::Button("Open in Entity Inspector")) {
          ui.show_entity_inspector_window = true;
          ui.entity_inspector_id = n->id;
        }
        ImGui::SameLine();
        if (ImGui::Button("Focus")) {
          ui.reference_graph_focus_id = n->id;
          s.focus_id_input_next = true;
        }

        // Neighbor lists.
        std::vector<const GraphEdge*> out_edges;
        std::vector<const GraphEdge*> in_edges;
        out_edges.reserve(32);
        in_edges.reserve(32);
        for (const auto& e : s.edges) {
          if (e.from == sel) out_edges.push_back(&e);
          if (e.to == sel) in_edges.push_back(&e);
        }

        if (ImGui::CollapsingHeader("Outbound neighbors", ImGuiTreeNodeFlags_DefaultOpen)) {
          if (out_edges.empty()) {
            ImGui::TextDisabled("None.");
          } else {
            ImGui::BeginChild("##out_nei", ImVec2(0, 120), true);
            for (const GraphEdge* ep : out_edges) {
              auto itn = s.nodes.find(ep->to);
              std::string name = (itn != s.nodes.end()) ? node_label(itn->second)
                                                           : ("#" + std::to_string(static_cast<unsigned long long>(ep->to)));
              std::string line = name;
              if (ep->count > 1) line += "  (" + std::to_string(ep->count) + ")";
              if (ImGui::Selectable(line.c_str(), false)) {
                s.selected_id = ep->to;
              }
            }
            ImGui::EndChild();
          }
        }

        if (ImGui::CollapsingHeader("Inbound neighbors", ImGuiTreeNodeFlags_DefaultOpen)) {
          if (in_edges.empty()) {
            ImGui::TextDisabled("None.");
          } else {
            ImGui::BeginChild("##in_nei", ImVec2(0, 120), true);
            for (const GraphEdge* ep : in_edges) {
              auto itn = s.nodes.find(ep->from);
              std::string name = (itn != s.nodes.end()) ? node_label(itn->second)
                                                           : ("#" + std::to_string(static_cast<unsigned long long>(ep->from)));
              std::string line = name;
              if (ep->count > 1) line += "  (" + std::to_string(ep->count) + ")";
              if (ImGui::Selectable(line.c_str(), false)) {
                s.selected_id = ep->from;
              }
            }
            ImGui::EndChild();
          }
        }
      } else {
        ImGui::TextDisabled("Select a node in the graph.");
      }
    } else {
      ImGui::TextDisabled("No focus id set.");
    }

    ImGui::SeparatorText("Path finder");

    ImGui::Checkbox("Undirected", &s.path.undirected);
    ImGui::SameLine();
    ImGui::Checkbox("Auto update", &s.path.auto_update);

    ImGui::SetNextItemWidth(-1);
    ImGui::InputScalar("##path_from", ImGuiDataType_U64, &s.path.from);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputScalar("##path_to", ImGuiDataType_U64, &s.path.to);

    if (ImGui::Button("From = focus")) s.path.from = ui.reference_graph_focus_id;
    ImGui::SameLine();
    if (ImGui::Button("To = selection")) s.path.to = (s.selected_id != 0) ? s.selected_id : ui.reference_graph_focus_id;
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
      s.path.from = 0;
      s.path.to = 0;
      s.path.nodes.clear();
      s.path.node_set.clear();
      s.path.edge_set.clear();
      s.path.has_path = false;
      s.path.status.clear();
      s.path.last_edge_count = 0;
      s.path.last_node_count = 0;
    }

    if (ImGui::Button("Find path now")) {
      // Force recompute by invalidating counters; update_path_cache will run next frame.
      s.path.last_edge_count = 0;
      s.path.last_node_count = 0;
      update_path_cache(s);
    }

    if (!s.path.status.empty()) {
      if (s.path.has_path) {
        ImGui::TextDisabled("Path: %zu steps", (s.path.nodes.size() > 0) ? (s.path.nodes.size() - 1) : 0);
      } else {
        ImGui::TextDisabled("%s", s.path.status.c_str());
      }
    }

    if (s.path.has_path && !s.path.nodes.empty()) {
      ImGui::BeginChild("##path_list", ImVec2(0, 140), true);
      for (std::size_t i = 0; i < s.path.nodes.size(); ++i) {
        const std::uint64_t id = s.path.nodes[i];
        auto itn = s.nodes.find(id);
        const std::string label = (itn != s.nodes.end()) ? node_label(itn->second) : ("#" + std::to_string(id));
        if (ImGui::Selectable(label.c_str(), false)) {
          s.selected_id = id;
          s.request_center_selection = true;
        }
      }
      ImGui::EndChild();
    }

    ImGui::EndChild();

    // --- Right panel (canvas) ---
    ImGui::TableNextColumn();
    ImGui::BeginChild("##refgraph_canvas", ImVec2(0, 0), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const ImVec2 canvas_p0 = ImGui::GetCursorScreenPos();
    const ImVec2 canvas_sz = ImGui::GetContentRegionAvail();
    const ImVec2 canvas_p1 = ImVec2(canvas_p0.x + canvas_sz.x, canvas_p0.y + canvas_sz.y);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(canvas_p0, canvas_p1, IM_COL32(18, 18, 22, 255));
    dl->AddRect(canvas_p0, canvas_p1, IM_COL32(255, 255, 255, 30));

    if (s.show_grid) draw_grid(dl, canvas_p0, canvas_sz, s.pan, s.zoom);

    ImGui::InvisibleButton("##canvas_btn", canvas_sz,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);

    const bool hovered = ImGui::IsItemHovered();
    ImGuiIO& io = ImGui::GetIO();

    // Zoom on wheel.
    if (hovered && io.MouseWheel != 0.0f) {
      const float z = s.zoom;
      const float z2 = std::clamp(z * (1.0f + io.MouseWheel * 0.10f), 0.15f, 3.5f);

      const ImVec2 mouse = io.MousePos;
      const ImVec2 before = to_world(canvas_p0, mouse, s.pan, z);
      s.zoom = z2;
      const ImVec2 after = to_world(canvas_p0, mouse, s.pan, z2);
      s.pan = ImVec2(s.pan.x + (before.x - after.x), s.pan.y + (before.y - after.y));
    }

    // Pan with middle mouse drag.
    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
      s.pan = ImVec2(s.pan.x + io.MouseDelta.x / s.zoom, s.pan.y + io.MouseDelta.y / s.zoom);
    }

    // Keyboard shortcuts on canvas.
    if (hovered && ImGui::IsKeyPressed(ImGuiKey_F, false)) s.request_fit = true;
    if (hovered && ImGui::IsKeyPressed(ImGuiKey_C, false)) s.request_center_selection = true;
    if (hovered && ImGui::IsKeyPressed(ImGuiKey_G, false)) s.request_center_focus = true;

    // Apply view requests.
    if (s.request_fit) {
      fit_view(s, canvas_sz);
      s.request_fit = false;
    }
    if (s.request_center_focus) {
      if (auto itn = s.nodes.find(ui.reference_graph_focus_id); itn != s.nodes.end()) {
        center_on(s, canvas_sz, itn->second.pos);
      }
      s.request_center_focus = false;
    }
    if (s.request_center_selection) {
      const std::uint64_t id = (s.selected_id != 0) ? s.selected_id : ui.reference_graph_focus_id;
      if (auto itn = s.nodes.find(id); itn != s.nodes.end()) {
        center_on(s, canvas_sz, itn->second.pos);
      }
      s.request_center_selection = false;
    }

    // Force layout step.
    apply_force_layout(s, io.DeltaTime, ui.reference_graph_auto_layout);

    const std::string filter = s.node_filter;

    // Hit-testing: find node under cursor.
    std::uint64_t hovered_id = 0;
    const ImVec2 mouse_world = to_world(canvas_p0, io.MousePos, s.pan, s.zoom);
    const float node_r = 18.0f;
    for (const auto& kv : s.nodes) {
      const GraphNode& n = kv.second;

      const bool vis = node_matches_filter(n, filter) ||
                       (n.id == ui.reference_graph_focus_id) ||
                       (n.id == s.selected_id) ||
                       (s.path.node_set.find(n.id) != s.path.node_set.end());
      if (!vis) continue;

      const float dx = (mouse_world.x - n.pos.x);
      const float dy = (mouse_world.y - n.pos.y);
      if (dx * dx + dy * dy <= node_r * node_r) {
        hovered_id = n.id;
        break;
      }
    }

    // Right-click context.
    static std::uint64_t ctx_id = 0;
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      ctx_id = hovered_id;
      if (ctx_id != 0) {
        ImGui::OpenPopup("##node_ctx");
      }
    }

    // Drag node with left mouse.
    static std::uint64_t drag_id = 0;
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      if (hovered_id != 0) {
        if (io.KeyShift) {
          s.path.from = hovered_id;
          s.path.last_edge_count = 0;
          s.path.last_node_count = 0;
          update_path_cache(s);
        } else if (io.KeyCtrl) {
          s.path.to = hovered_id;
          s.path.last_edge_count = 0;
          s.path.last_node_count = 0;
          update_path_cache(s);
        } else {
          s.selected_id = hovered_id;
          drag_id = hovered_id;
        }
      } else {
        drag_id = 0;
      }
    }
    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f) && drag_id != 0) {
      auto it = s.nodes.find(drag_id);
      if (it != s.nodes.end()) {
        GraphNode& n = it->second;
        n.pos = ImVec2(n.pos.x + io.MouseDelta.x / s.zoom, n.pos.y + io.MouseDelta.y / s.zoom);
        n.fixed = true;
      }
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
      drag_id = 0;
    }

    // Draw edges.
    for (const GraphEdge& e : s.edges) {
      auto it_a = s.nodes.find(e.from);
      auto it_b = s.nodes.find(e.to);
      if (it_a == s.nodes.end() || it_b == s.nodes.end()) continue;

      const GraphNode& na = it_a->second;
      const GraphNode& nb = it_b->second;

      const bool va = node_matches_filter(na, filter) || (na.id == ui.reference_graph_focus_id) || (na.id == s.selected_id) ||
                      (s.path.node_set.find(na.id) != s.path.node_set.end());
      const bool vb = node_matches_filter(nb, filter) || (nb.id == ui.reference_graph_focus_id) || (nb.id == s.selected_id) ||
                      (s.path.node_set.find(nb.id) != s.path.node_set.end());
      if (!va || !vb) continue;

      const bool in_path = (s.path.edge_set.find(EdgeKey{e.from, e.to}) != s.path.edge_set.end());
      const bool touches_sel = (s.selected_id != 0 && (e.from == s.selected_id || e.to == s.selected_id));

      const ImU32 col = in_path ? IM_COL32(255, 210, 120, 200)
                                : (touches_sel ? IM_COL32(220, 220, 240, 110) : IM_COL32(200, 200, 220, 70));
      const float thickness = in_path ? 3.0f : 1.6f;

      const ImVec2 a = to_screen(canvas_p0, na.pos, s.pan, s.zoom);
      const ImVec2 b = to_screen(canvas_p0, nb.pos, s.pan, s.zoom);
      dl->AddLine(a, b, col, thickness);

      // Tiny arrow head.
      ImVec2 d = ImVec2(b.x - a.x, b.y - a.y);
      float len = std::sqrt(d.x * d.x + d.y * d.y);
      if (len > 1.0f) {
        d.x /= len;
        d.y /= len;
        const ImVec2 tip = ImVec2(b.x - d.x * node_r * 0.8f, b.y - d.y * node_r * 0.8f);
        const ImVec2 left = ImVec2(tip.x - d.y * 6.0f - d.x * 10.0f, tip.y + d.x * 6.0f - d.y * 10.0f);
        const ImVec2 right = ImVec2(tip.x + d.y * 6.0f - d.x * 10.0f, tip.y - d.x * 6.0f - d.y * 10.0f);
        dl->AddTriangleFilled(tip, left, right, col);
      }
    }

    // Draw nodes.
    for (const auto& kv : s.nodes) {
      const GraphNode& n = kv.second;

      const bool vis = node_matches_filter(n, filter) ||
                       (n.id == ui.reference_graph_focus_id) ||
                       (n.id == s.selected_id) ||
                       (s.path.node_set.find(n.id) != s.path.node_set.end());
      if (!vis) continue;

      const ImVec2 c = to_screen(canvas_p0, n.pos, s.pan, s.zoom);

      const bool is_focus = (n.id == ui.reference_graph_focus_id);
      const bool is_sel = (n.id == s.selected_id);
      const bool is_hov = (n.id == hovered_id);
      const bool is_path = (s.path.node_set.find(n.id) != s.path.node_set.end());

      ImU32 col_fill = IM_COL32(90, 110, 140, 220);
      if (is_path) col_fill = IM_COL32(160, 130, 200, 240);
      if (is_focus) col_fill = IM_COL32(120, 150, 220, 240);
      if (is_sel) col_fill = IM_COL32(220, 180, 95, 245);
      if (is_hov) col_fill = IM_COL32(235, 235, 235, 245);

      const float r = node_r * std::clamp(s.zoom, 0.6f, 1.15f);
      dl->AddCircleFilled(c, r, col_fill, 24);
      dl->AddCircle(c, r, IM_COL32(0, 0, 0, 140), 24, 1.0f);

      std::string t = !n.name.empty() ? n.name : (!n.kind.empty() ? (n.kind + " #" + std::to_string(n.id)) : ("#" + std::to_string(n.id)));
      if (t.size() > 22) t = t.substr(0, 19) + "...";
      const ImVec2 tsz = ImGui::CalcTextSize(t.c_str());
      dl->AddText(ImVec2(c.x - tsz.x * 0.5f, c.y - tsz.y * 0.5f), IM_COL32(10, 10, 10, 255), t.c_str());
    }

    // Tooltip for hovered node.
    if (hovered && hovered_id != 0) {
      auto it = s.nodes.find(hovered_id);
      if (it != s.nodes.end()) {
        const std::string lbl = node_label(it->second);
        ImGui::SetTooltip("%s\n\nLeft: select/drag\nShift+Click: set path start\nCtrl+Click: set path end\n"
                          "Right: actions\nDouble-click: focus",
                          lbl.c_str());

        if (ImGui::IsMouseDoubleClicked(0)) {
          ui.reference_graph_focus_id = hovered_id;
          s.focus_id_input_next = true;
        }
      }
    }

    // Node context menu.
    if (ImGui::BeginPopup("##node_ctx")) {
      if (ctx_id != 0) {
        auto it = s.nodes.find(ctx_id);
        if (it != s.nodes.end()) {
          const GraphNode& n = it->second;
          ImGui::TextDisabled("%s", node_label(n).c_str());
          ImGui::Separator();

          if (ImGui::MenuItem("Focus here")) {
            ui.reference_graph_focus_id = ctx_id;
            s.focus_id_input_next = true;
          }
          if (ImGui::MenuItem("Select")) {
            s.selected_id = ctx_id;
          }

          ImGui::Separator();

          if (ImGui::MenuItem("Set as Path start")) {
            s.path.from = ctx_id;
            s.path.last_edge_count = 0;
            s.path.last_node_count = 0;
            update_path_cache(s);
          }
          if (ImGui::MenuItem("Set as Path end")) {
            s.path.to = ctx_id;
            s.path.last_edge_count = 0;
            s.path.last_node_count = 0;
            update_path_cache(s);
          }

          ImGui::Separator();

          if (!ui.reference_graph_global_mode) {
            if (ImGui::MenuItem("Expand outbound")) {
              scan_outbound_from_entity(s, *s.root, ctx_id, ui.reference_graph_strict_id_keys, ui.reference_graph_max_nodes,
                                       /*max_scan_nodes=*/200000);
              s.expanded_out.insert(ctx_id);
            }
            if (ImGui::MenuItem("Scan inbound (whole state)")) {
              start_inbound_scan(s, ctx_id, s.doc_revision, s.root.get());
              s.expanded_in.insert(ctx_id);
            }
            ImGui::Separator();
          } else {
            if (ImGui::MenuItem("Scan outbound (this entity)")) {
              scan_outbound_from_entity(s, *s.root, ctx_id, ui.reference_graph_strict_id_keys, ui.reference_graph_max_nodes,
                                       ui.reference_graph_scan_nodes_per_entity);
              s.expanded_out.insert(ctx_id);
            }
            ImGui::Separator();
          }

          if (!n.path.empty() && ImGui::MenuItem("Open in JSON Explorer")) {
            ui.show_json_explorer_window = true;
            ui.request_json_explorer_goto_path = n.path;
          }
          if (ImGui::MenuItem("Open in Entity Inspector")) {
            ui.show_entity_inspector_window = true;
            ui.entity_inspector_id = n.id;
          }

          ImGui::Separator();
          if (ImGui::MenuItem("Center here")) {
            s.request_center_selection = true;
            s.selected_id = ctx_id;
          }
          if (ImGui::MenuItem("Copy ID")) {
            const std::string idstr = std::to_string(static_cast<unsigned long long>(n.id));
            ImGui::SetClipboardText(idstr.c_str());
          }
          if (!n.path.empty() && ImGui::MenuItem("Copy entity JSON Pointer")) {
            ImGui::SetClipboardText(n.path.c_str());
          }
        }
      }
      ImGui::EndPopup();
    }

    ImGui::EndChild();

    ImGui::EndTable();
  }

  // Incremental scanning after UI so the window stays responsive.
  if (!ui.reference_graph_global_mode) {
    step_inbound_scan(s, *s.root, ui);
  } else {
    step_global_graph(s, *s.root, ui);
  }

  ImGui::End();
}

} // namespace nebula4x::ui
