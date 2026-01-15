#include "ui/galaxy_constellations.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nebula4x/core/procgen_obscure.h"

namespace nebula4x::ui {
namespace {

using nebula4x::procgen_obscure::HashRng;
using nebula4x::procgen_obscure::hex_n;
using nebula4x::procgen_obscure::splitmix64;
using nebula4x::procgen_obscure::glyph8_from_seed;

struct SysNode {
  Id id{kInvalidId};
  Id region_id{kInvalidId};
  Vec2 p{0.0, 0.0};
};

std::uint64_t hash_u64(std::uint64_t h, std::uint64_t x) {
  // A tiny streaming hash based on splitmix.
  return splitmix64(h ^ splitmix64(x + 0x9e3779b97f4a7c15ULL));
}

std::uint64_t compute_cache_key(const std::vector<Id>& ids, const GalaxyConstellationParams& params) {
  std::uint64_t h = 0xB4D0E4F9A1C2D3E5ULL;
  h = hash_u64(h, static_cast<std::uint64_t>(params.target_cluster_size));
  h = hash_u64(h, static_cast<std::uint64_t>(params.max_constellations));
  h = hash_u64(h, static_cast<std::uint64_t>(ids.size()));
  // Order-independent hash: xor in mixed ids.
  std::uint64_t acc = 0;
  for (Id id : ids) {
    acc ^= splitmix64(static_cast<std::uint64_t>(id) * 0xA24BAED4963EE407ULL);
  }
  h = hash_u64(h, acc);
  return h;
}

std::string constellation_name_from_seed(std::uint64_t seed) {
  HashRng rng(splitmix64(seed ^ 0x7B1D3A2C9E8F6D01ULL));

  static constexpr std::array<const char*, 20> kAdj = {
      "Sable", "Cinder", "Pale", "Gilded", "Hollow", "Vanta", "Cobalt", "Iron", "Glass", "Quiet",
      "Crimson", "Drowned", "Ashen", "Lunar", "Ivory", "Obsidian", "Silver", "Rust", "Eld", "Cipher",
  };
  static constexpr std::array<const char*, 22> kNoun = {
      "Crown", "Compass", "Gate", "Choir", "Harbor", "Spiral", "Index", "Lantern", "Wound", "Cathedral",
      "Orchard", "Meridian", "Reliquary", "Lattice", "Vault", "Helix", "Anchor", "Mirror", "Thorn", "Keel",
      "Monastery", "Archive",
  };
  static constexpr std::array<const char*, 12> kSuffix = {
      "of Dust", "of Echoes", "of Salt", "of Knots", "of Glass", "of Thunder", "of Silence", "of Cinders",
      "of Drift", "of Night", "of Lanterns", "of Needles",
  };

  const char* a = kAdj[static_cast<std::size_t>(rng.range_int(0, static_cast<int>(kAdj.size()) - 1))];
  const char* n = kNoun[static_cast<std::size_t>(rng.range_int(0, static_cast<int>(kNoun.size()) - 1))];
  std::string out = std::string(a) + " " + n;
  if (rng.next_u01() < 0.55) {
    const char* s = kSuffix[static_cast<std::size_t>(rng.range_int(0, static_cast<int>(kSuffix.size()) - 1))];
    out += " ";
    out += s;
  }
  return out;
}

Vec2 centroid_of(const std::vector<SysNode>& nodes, const std::vector<Id>& member_ids) {
  if (member_ids.empty()) return Vec2{0.0, 0.0};
  double sx = 0.0;
  double sy = 0.0;
  int n = 0;
  for (Id id : member_ids) {
    for (const auto& nd : nodes) {
      if (nd.id == id) {
        sx += nd.p.x;
        sy += nd.p.y;
        ++n;
        break;
      }
    }
  }
  if (n <= 0) return Vec2{0.0, 0.0};
  return Vec2{sx / static_cast<double>(n), sy / static_cast<double>(n)};
}

double dist2(const Vec2& a, const Vec2& b) {
  const double dx = a.x - b.x;
  const double dy = a.y - b.y;
  return dx * dx + dy * dy;
}

std::vector<GalaxyConstellationEdge> mst_edges(const std::vector<SysNode>& nodes, const std::vector<Id>& member_ids) {
  std::vector<GalaxyConstellationEdge> edges;
  const int n = static_cast<int>(member_ids.size());
  if (n <= 1) return edges;

  // Map member index to node position.
  std::vector<Vec2> pts;
  pts.reserve(member_ids.size());
  for (Id id : member_ids) {
    for (const auto& nd : nodes) {
      if (nd.id == id) {
        pts.push_back(nd.p);
        break;
      }
    }
  }
  if ((int)pts.size() != n) return edges;

  // Prim's algorithm in O(n^2) for small n.
  std::vector<double> best(n, std::numeric_limits<double>::infinity());
  std::vector<int> parent(n, -1);
  std::vector<bool> used(n, false);
  best[0] = 0.0;

  for (int it = 0; it < n; ++it) {
    int v = -1;
    double bv = std::numeric_limits<double>::infinity();
    for (int i = 0; i < n; ++i) {
      if (!used[i] && best[i] < bv) {
        bv = best[i];
        v = i;
      }
    }
    if (v < 0) break;
    used[v] = true;
    if (parent[v] >= 0) {
      edges.push_back(GalaxyConstellationEdge{member_ids[static_cast<std::size_t>(parent[v])], member_ids[static_cast<std::size_t>(v)]});
    }
    for (int u = 0; u < n; ++u) {
      if (used[u]) continue;
      const double w = dist2(pts[v], pts[u]);
      if (w < best[u]) {
        best[u] = w;
        parent[u] = v;
      }
    }
  }

  return edges;
}

} // namespace

std::vector<GalaxyConstellation> build_galaxy_constellations(
    const GameState& st,
    const std::vector<Id>& visible_system_ids,
    const GalaxyConstellationParams& params) {
  GalaxyConstellationParams p = params;
  p.target_cluster_size = std::clamp(p.target_cluster_size, 4, 24);
  p.max_constellations = std::clamp(p.max_constellations, 0, 1000);

  // Collect visible nodes.
  std::vector<SysNode> nodes;
  nodes.reserve(visible_system_ids.size());
  for (Id sid : visible_system_ids) {
    const auto* sys = find_ptr(st.systems, sid);
    if (!sys) continue;
    nodes.push_back(SysNode{sid, sys->region_id, sys->galaxy_pos});
  }
  if (nodes.size() < 3 || p.max_constellations <= 0) return {};

  // Group by region for coherent clusters.
  std::unordered_map<Id, std::vector<Id>> by_region;
  by_region.reserve(nodes.size());
  for (const auto& nd : nodes) {
    by_region[nd.region_id].push_back(nd.id);
  }

  // Stable, deterministic ordering per region.
  for (auto& [rid, ids] : by_region) {
    const std::uint64_t rs = splitmix64(static_cast<std::uint64_t>(rid) ^ 0x5B2C1F0E9D8A7C63ULL);
    std::sort(ids.begin(), ids.end(), [&](Id a, Id b) {
      const std::uint64_t ha = splitmix64(static_cast<std::uint64_t>(a) ^ rs);
      const std::uint64_t hb = splitmix64(static_cast<std::uint64_t>(b) ^ rs);
      if (ha != hb) return ha < hb;
      return a < b;
    });
  }

  const std::uint64_t cache_key = compute_cache_key(visible_system_ids, p);
  (void)cache_key;

  std::vector<GalaxyConstellation> out;
  out.reserve(std::min<std::size_t>(128, nodes.size() / 4));

  int emitted = 0;
  for (auto& [rid, ids] : by_region) {
    if (emitted >= p.max_constellations) break;
    if (ids.size() < 4) continue;

    const std::uint64_t region_seed = splitmix64(static_cast<std::uint64_t>(rid) ^ 0xC0FFEE1234ABCDEFULL);
    HashRng rng(region_seed);

    std::vector<Id> unassigned = ids;

    int cluster_idx = 0;
    while (!unassigned.empty() && emitted < p.max_constellations) {
      // Seed star: deterministic but pseudo-random order.
      const Id seed_id = unassigned.front();
      unassigned.erase(unassigned.begin());

      const int jitter = rng.range_int(-2, 2);
      const int desired = std::clamp(p.target_cluster_size + jitter, 4, 24);

      std::vector<Id> members;
      members.reserve(static_cast<std::size_t>(desired));
      members.push_back(seed_id);

      // Build a compact cluster by repeatedly pulling the nearest unassigned
      // system toward the current cluster centroid.
      while ((int)members.size() < desired && !unassigned.empty()) {
        const Vec2 c = centroid_of(nodes, members);
        int best_i = -1;
        double best_d = std::numeric_limits<double>::infinity();

        for (int i = 0; i < (int)unassigned.size(); ++i) {
          const Id cand = unassigned[(std::size_t)i];
          const auto* sys = find_ptr(st.systems, cand);
          if (!sys) continue;
          const double d2 = dist2(sys->galaxy_pos, c);
          if (d2 < best_d) {
            best_d = d2;
            best_i = i;
          }
        }

        if (best_i < 0) break;
        members.push_back(unassigned[(std::size_t)best_i]);
        unassigned.erase(unassigned.begin() + best_i);
      }

      // Tiny regions can end up with a small tail; avoid generating a
      // constellation that's just a pair.
      if (members.size() < 3) continue;

      const std::uint64_t cid_seed = splitmix64(region_seed ^ (static_cast<std::uint64_t>(cluster_idx) * 0x9e3779b97f4a7c15ULL));
      const std::string code8 = hex_n(static_cast<std::uint64_t>((cid_seed >> 32) ^ (cid_seed & 0xffffffffu)), 8);
      const std::string code = code8.substr(0, 4) + "-" + code8.substr(4, 4);
      const std::string glyph = glyph8_from_seed(cid_seed);

      GalaxyConstellation c;
      c.id = cid_seed;
      c.region_id = rid;
      c.name = constellation_name_from_seed(cid_seed);
      c.code = code;
      c.glyph = glyph;
      c.systems = members;
      c.centroid = centroid_of(nodes, members);
      c.edges = mst_edges(nodes, members);

      out.push_back(std::move(c));
      ++cluster_idx;
      ++emitted;
    }
  }

  // Stable overall ordering (for UI lists): region id then name.
  std::sort(out.begin(), out.end(), [](const GalaxyConstellation& a, const GalaxyConstellation& b) {
    if (a.region_id != b.region_id) return a.region_id < b.region_id;
    if (a.name != b.name) return a.name < b.name;
    return a.id < b.id;
  });

  return out;
}

}  // namespace nebula4x::ui
