// Halo Map Porter - route points for Halo 1 Unlimited's multiplayer bots (botnodes.hpp).
#include "botnodes.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <queue>
#include <unordered_map>

#include "collision.hpp"
#include "tagfile.hpp"

namespace botnodes {

namespace fs = std::filesystem;

namespace {

double dist(const V3 &a, const V3 &b) {
    double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}
double dist2d(const V3 &a, const V3 &b) {
    double x = a[0] - b[0], y = a[1] - b[1];
    return std::sqrt(x * x + y * y);
}
// bots_weighted_distance: a floor up or down counts extra
double weighted(const V3 &a, const V3 &b) { return dist2d(a, b) + 2 * std::fabs(b[2] - a[2]); }

const uint32_t kNone = coll::kNone;

// The level as the game's path search sees it: the collision surfaces, each one's edges (to the surface across each,
// and whether that one is walkable).
struct Level {
    coll::Bsp b;
    std::vector<uint8_t> pf;   // each surface's pathfinding flags (bit 6: walkable)
    struct Edge { V3 base, vec; uint32_t adjacent; };
    std::vector<std::vector<Edge>> edges;   // each surface's (build_path_edges_for_surface)
    std::vector<std::vector<V3>> ring;      // its corners
    std::vector<V3> normal;
    std::vector<int> comp;                  // each walkable surface's piece (walkable surfaces joined by edges)
    struct Jump { uint32_t to; V3 from, land; };
    std::vector<std::vector<Jump>> jumps;   // each walkable surface's ways onto another piece (find_jumps)
    bool walkable(uint32_t s) const { return s < pf.size() && (pf[s] & 64); }
};

bool load_level(const tagfile::Elem &r, Level &L, std::string &why) {
    tagfile::Block cbs = r.block("collision bsp");
    if (cbs.size() == 0 || !coll::read_bsp(cbs[0], L.b) || L.b.surfaces.empty()) { why = "has no collision BSP"; return false; }
    tagfile::Block pfs = r.block("pathfinding surfaces");
    if (pfs.size() == 0) { why = "has no pathfinding data (the AI can't walk it)"; return false; }
    L.pf.resize(pfs.size());
    for (size_t i = 0; i < pfs.size(); i++) L.pf[i] = (uint8_t)pfs[i].integer("data");
    const size_t n = L.b.surfaces.size();
    L.edges.resize(n);
    L.ring.resize(n);
    L.normal.resize(n);
    for (uint32_t s = 0; s < n; s++) {
        uint32_t e = L.b.surfaces[s].first_edge;
        for (int k = 0; k < 64 && e < L.b.edges.size(); k++) {
            const coll::Edge &ed = L.b.edges[e];
            bool right = ed.s[1] == s;
            if (ed.v[0] >= L.b.vertices.size() || ed.v[1] >= L.b.vertices.size()) break;
            const double *p0 = L.b.vertices[ed.v[0]].p, *p1 = L.b.vertices[ed.v[1]].p;
            Level::Edge pe;
            pe.base = {p0[0], p0[1], p0[2]};
            pe.vec = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
            pe.adjacent = ed.s[!right];
            L.edges[s].push_back(pe);
            const double *c = L.b.vertices[ed.v[right]].p;
            L.ring[s].push_back({c[0], c[1], c[2]});
            e = ed.next[right];
            if (e == L.b.surfaces[s].first_edge) break;
        }
        V3 nn{0, 0, 0};
        auto &q = L.ring[s];
        for (size_t k = 0; k < q.size(); k++) {
            auto &a = q[k], &z = q[(k + 1) % q.size()];
            nn[0] += (a[1] - z[1]) * (a[2] + z[2]);
            nn[1] += (a[2] - z[2]) * (a[0] + z[0]);
            nn[2] += (a[0] - z[0]) * (a[1] + z[1]);
        }
        double l = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
        L.normal[s] = l > 0 ? V3{nn[0] / l, nn[1] / l, nn[2] / l} : V3{0, 0, 1};
    }
    return true;
}

// bots_ground: the ground under a point (a ray from half a unit above it, 3.5 down, front faces), and its surface.
uint32_t ground(const Level &L, const V3 &p, V3 &out) {
    double o[3] = {p[0], p[1], p[2] + 0.5}, v[3] = {0, 0, -3.5};
    coll::Hit h = coll::test_vector(L.b, o, v, 1, false);
    if (h.surface == kNone) return kNone;
    out = {o[0], o[1], o[2] + v[2] * h.t};
    return h.surface;
}

V3 entry(const Level::Edge &e, const V3 &dest, double radius);

// The pieces of walkable floor (surfaces joined by shared edges: what the game's search can cross), and where a bot
// gets from one onto another anyway: a step or ledge up it can crouch-jump (bots_search finishes a hop straight there,
// and its look-ahead jumps), or a drop it survives. From each edge of a walkable surface with no walkable one across
// it, a probe half a unit and a unit past it: the ground there (a ray down from above the jump's reach), when it's
// another piece's, in reach, and nothing in the way at the ledge's top.
void find_jumps(Level &L, double jump, double drop) {
    const size_t n = L.b.surfaces.size();
    L.comp.assign(n, -1);
    int pieces = 0;
    for (uint32_t s = 0; s < n; s++) {
        if (!L.walkable(s) || L.comp[s] >= 0) continue;
        std::vector<uint32_t> q{s};
        L.comp[s] = pieces;
        for (size_t k = 0; k < q.size(); k++)
            for (auto &e : L.edges[q[k]])
                if (e.adjacent != kNone && L.walkable(e.adjacent) && L.comp[e.adjacent] < 0) {
                    L.comp[e.adjacent] = pieces;
                    q.push_back(e.adjacent);
                }
        pieces++;
    }
    L.jumps.assign(n, {});
    for (uint32_t s = 0; s < n; s++) {
        if (!L.walkable(s) || L.ring[s].size() < 3) continue;
        V3 mid{0, 0, 0};
        for (auto &p : L.ring[s]) for (int k = 0; k < 3; k++) mid[k] += p[k] / L.ring[s].size();
        for (auto &e : L.edges[s]) {
            if (e.adjacent != kNone && L.walkable(e.adjacent)) continue;
            double len = std::sqrt(e.vec[0] * e.vec[0] + e.vec[1] * e.vec[1]);
            if (len < 0.3) continue;
            V3 out{e.vec[1] / len, -e.vec[0] / len, 0};
            int samples = std::min(6, std::max(1, (int)(len / 1.5)));
            for (int i = 0; i < samples; i++) {
                double f = (i + 0.5) / samples;
                V3 m{e.base[0] + e.vec[0] * f, e.base[1] + e.vec[1] * f, e.base[2] + e.vec[2] * f};
                V3 o = out;
                if ((m[0] - mid[0]) * o[0] + (m[1] - mid[1]) * o[1] < 0) o = {-o[0], -o[1], 0};
                for (double reach : {0.5, 1.0}) {
                    double top = m[2] + jump + 0.3;
                    double org[3] = {m[0] + o[0] * reach, m[1] + o[1] * reach, top}, dn[3] = {0, 0, -(jump + 0.3 + drop + 0.2)};
                    coll::Hit h = coll::test_vector(L.b, org, dn, 1, false);
                    if (h.surface == kNone || !L.walkable(h.surface) || L.comp[h.surface] == L.comp[s]) continue;
                    double z = top + dn[2] * h.t, rise = z - m[2];
                    if (!(rise <= jump && rise >= -drop)) continue;
                    // (room over the ledge's top, at the waist, from the edge to the landing)
                    double over[3] = {m[0] - o[0] * 0.3, m[1] - o[1] * 0.3, m[2] + std::max(rise, 0.0) + 0.45};
                    double across[3] = {o[0] * (reach + 0.3), o[1] * (reach + 0.3), 0};
                    if (coll::test_vector(L.b, over, across, 1, true).surface != kNone) continue;
                    L.jumps[s].push_back({h.surface, m, {org[0], org[1], z}});
                    break;
                }
            }
        }
    }
}

struct Step { V3 at; uint32_t surface; };

// The way from a to b over the walkable floor and those jumps, no limits (empty: none). Each step: where it goes
// onto a surface (an edge's entry, a jump's takeoff on its own surface, its landing).
std::vector<Step> way(const Level &L, const V3 &a, uint32_t sa, const V3 &b, uint32_t sb, double radius) {
    struct Node { uint32_t s; V3 at; double g; int parent; };
    std::vector<Node> nodes;
    std::unordered_map<uint32_t, int> best;
    using H = std::pair<double, int>;
    std::priority_queue<H, std::vector<H>, std::greater<H>> heap;
    nodes.push_back({sa, a, 0, -1});
    best[sa] = 0;
    heap.push({dist(a, b), 0});
    int end = -1;
    std::vector<char> done;
    while (!heap.empty()) {
        auto [f, i] = heap.top();
        heap.pop();
        const Node cur = nodes[(size_t)i];
        if (best[cur.s] != i) continue;
        if (cur.s == sb) { end = i; break; }
        if (nodes.size() > 400000) break;
        auto relax = [&](uint32_t s, const V3 &at, double g, int parent) {
            auto it = best.find(s);
            if (it != best.end() && nodes[(size_t)it->second].g <= g) return;
            nodes.push_back({s, at, g, parent});
            best[s] = (int)nodes.size() - 1;
            heap.push({g + dist(at, b), (int)nodes.size() - 1});
        };
        for (auto &e : L.edges[cur.s]) {
            if (e.adjacent == kNone || !L.walkable(e.adjacent)) continue;
            V3 at = entry(e, b, radius);
            relax(e.adjacent, at, cur.g + dist(cur.at, at), i);
        }
        for (auto &j : L.jumps[cur.s]) {
            double g = cur.g + dist(cur.at, j.from) + dist(j.from, j.land) + 1.0;
            nodes.push_back({cur.s, j.from, cur.g + dist(cur.at, j.from), i});   // (the takeoff: no relaxing)
            relax(j.to, j.land, g, (int)nodes.size() - 1);
        }
    }
    std::vector<Step> out;
    if (end < 0) return out;
    for (int k = end; k >= 0; k = nodes[(size_t)k].parent) out.push_back({nodes[(size_t)k].at, nodes[(size_t)k].s});
    std::reverse(out.begin(), out.end());
    out.back().at = b;
    return out;
}

// The closest point of a surface to a point (closest_available_point_on_surface, near enough).
V3 closest_on(const Level &L, uint32_t s, const V3 &p) {
    const auto &q = L.ring[s];
    if (q.size() < 3) return p;
    const V3 &n = L.normal[s];
    double d = (p[0] - q[0][0]) * n[0] + (p[1] - q[0][1]) * n[1] + (p[2] - q[0][2]) * n[2];
    V3 pp{p[0] - n[0] * d, p[1] - n[1] * d, p[2] - n[2] * d};
    bool inside = true;
    for (size_t k = 0; k < q.size() && inside; k++) {
        auto &a = q[k], &c = q[(k + 1) % q.size()];
        V3 e{c[0] - a[0], c[1] - a[1], c[2] - a[2]}, w{pp[0] - a[0], pp[1] - a[1], pp[2] - a[2]};
        V3 x{e[1] * w[2] - e[2] * w[1], e[2] * w[0] - e[0] * w[2], e[0] * w[1] - e[1] * w[0]};
        inside = x[0] * n[0] + x[1] * n[1] + x[2] * n[2] >= -1e-9;
    }
    if (inside) return pp;
    V3 best = q[0];
    double bd = 1e30;
    for (size_t k = 0; k < q.size(); k++) {
        auto &a = q[k], &c = q[(k + 1) % q.size()];
        V3 e{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        double ee = e[0] * e[0] + e[1] * e[1] + e[2] * e[2];
        double t = ee > 0 ? ((p[0] - a[0]) * e[0] + (p[1] - a[1]) * e[1] + (p[2] - a[2]) * e[2]) / ee : 0;
        t = std::clamp(t, 0.0, 1.0);
        V3 x{a[0] + e[0] * t, a[1] + e[1] * t, a[2] + e[2] * t};
        double dd = dist(x, p);
        if (dd < bd) { bd = dd; best = x; }
    }
    return best;
}

// Where a search goes into the next surface across an edge (path_state_traverse): its middle, or on a long edge the
// point nearest the destination (kept the radius from its ends).
V3 entry(const Level::Edge &e, const V3 &dest, double radius) {
    double ll = e.vec[0] * e.vec[0] + e.vec[1] * e.vec[1] + e.vec[2] * e.vec[2];
    double t = 0.5;
    if (ll > 16.0 && ll > (2 * radius) * (2 * radius)) {
        double l = std::sqrt(ll);
        t = ((dest[0] - e.base[0]) * e.vec[0] + (dest[1] - e.base[1]) * e.vec[1] + (dest[2] - e.base[2]) * e.vec[2]) / ll;
        t = std::clamp(t, radius / l, 1 - radius / l);
    }
    return {e.base[0] + e.vec[0] * t, e.base[1] + e.vec[1] * t, e.base[2] + e.vec[2] * t};
}

struct Found {
    bool reached = false;      // popped the destination's surface
    int opened = 0;
    V3 closest{};              // the closest spot it found
    double closest_distance = 1e30;
    std::vector<uint32_t> surfaces;   // the way (no limits only)
    std::vector<V3> points;
};

// path_state_traverse, as the bots run it (bots_search): `limit` surfaces opened at most, the way no longer than
// `bound` (0: none), and it stops when what's left can't get closer. `limit` 0: no limits at all (the way the level's
// walkable surfaces join, if any).
Found search(const Level &L, const V3 &start, uint32_t s0, const V3 &dest, uint32_t s1, double radius, int limit, double bound) {
    Found f;
    radius = std::max(0.2, radius);
    struct Node { uint32_t surface, parent_surface; int parent; V3 at; double from_origin, cost, estimate; long q; bool open; };
    std::vector<Node> nodes;
    std::unordered_map<uint32_t, int> by_surface;
    using H = std::pair<long, int>;
    std::priority_queue<H, std::vector<H>, std::greater<H>> heap;
    const bool unlimited = limit <= 0;
    double d0 = dist(start, dest);
    nodes.push_back({s0, kNone, -1, start, 0, 0, d0, (long)(d0 / 0.1), true});
    by_surface[s0] = 0;
    heap.push({nodes[0].q, 0});
    f.closest = start;
    f.closest_distance = d0;
    double closest_cost = d0;
    int end = -1;
    while (!heap.empty()) {
        auto [q, i] = heap.top();
        heap.pop();
        if (!nodes[(size_t)i].open || q != nodes[(size_t)i].q) continue;
        nodes[(size_t)i].open = false;
        const Node cur = nodes[(size_t)i];
        if (cur.surface == s1) { f.reached = true; f.closest = dest; f.closest_distance = 0; end = i; break; }
        if (!unlimited && cur.estimate > std::max(5.0, f.closest_distance) * 10 + closest_cost) break;
        if (cur.surface >= L.edges.size()) continue;
        for (const auto &e : L.edges[cur.surface]) {
            uint32_t adj = e.adjacent;
            if (adj == cur.parent_surface || adj == kNone || !L.walkable(adj)) continue;
            V3 at = entry(e, dest, radius);
            double lin = dist(cur.at, at);
            double from_origin = cur.from_origin + lin, cost = cur.cost + lin;
            double to_dest = dist(at, dest), estimate = cost + to_dest;
            long qe = (long)(estimate / 0.1);
            if (!unlimited && qe >= 32767) continue;
            if (!unlimited && bound > 0 && from_origin > bound) continue;
            int k;
            auto it = by_surface.find(adj);
            if (it != by_surface.end()) {
                k = it->second;
                if (!(qe < nodes[(size_t)k].q) || !nodes[(size_t)k].open) continue;
            } else {
                if (!unlimited && (int)nodes.size() >= limit) continue;
                k = (int)nodes.size();
                nodes.push_back({});
                by_surface[adj] = k;
            }
            nodes[(size_t)k] = {adj, cur.surface, i, at, from_origin, cost, estimate, qe, true};
            heap.push({qe, k});
            double cd = to_dest;
            V3 cp = at;
            if (cd < 4) { cp = closest_on(L, adj, dest); cd = dist(cp, dest); }
            if (cd < f.closest_distance) { f.closest_distance = cd; f.closest = cp; closest_cost = estimate; }
        }
    }
    f.opened = (int)nodes.size();
    if (unlimited && end >= 0) {
        for (int k = end; k >= 0; k = nodes[(size_t)k].parent) {
            f.surfaces.push_back(nodes[(size_t)k].surface);
            f.points.push_back(nodes[(size_t)k].at);
        }
        std::reverse(f.surfaces.begin(), f.surfaces.end());
        std::reverse(f.points.begin(), f.points.end());
        f.points.back() = dest;
    }
    return f;
}

struct Way { V3 at; uint32_t surface; };

// A hop as a bot walks it: the search finishes it, or ends within 4.5 units of the waypoint at the foot of a ledge
// it can jump onto, or the edge of a drop it survives (bots_search's straight finish).
bool hop_ok(const Level &L, const Way &a, const Way &b, const Options &o, int limit, int *opened = nullptr) {
    double bound = std::min(2 * dist(a.at, b.at) + 10, 60.0);
    Found f = search(L, a.at, a.surface, b.at, b.surface, o.radius, limit, bound);
    if (opened) *opened = f.opened;
    if (f.reached) return true;
    if (dist2d(f.closest, b.at) < 4.5) {
        double rise = b.at[2] - f.closest[2];
        if ((rise > 0.2 && rise <= o.jump) || (rise < -0.5 && -rise <= o.drop) || std::fabs(rise) <= 0.2) return true;
    }
    return false;
}

// bots_route_build's links: each waypoint to its nearest (weighted), up to 8, those past 40 units only for its
// first 2; both ways, 8 a waypoint at most. A teleporter's way in links only to its way out (not searched).
std::vector<std::vector<int>> make_links(const std::vector<Way> &w, const std::vector<int> &tele_to) {
    const int n = (int)w.size();
    std::vector<std::vector<int>> links((size_t)n);
    auto has = [&](int a, int b) { return std::find(links[(size_t)a].begin(), links[(size_t)a].end(), b) != links[(size_t)a].end(); };
    auto source_only = [&](int a) { return tele_to[(size_t)a] >= 0; };   // (a way in that's no way out)
    auto add = [&](int a, int b) {
        if (a != b && !source_only(a) && links[(size_t)a].size() < 8 && !has(a, b)) links[(size_t)a].push_back(b);
    };
    for (int a = 0; a < n; a++)
        if (tele_to[(size_t)a] >= 0) links[(size_t)a].push_back(tele_to[(size_t)a]);
    for (int a = 0; a < n; a++)
        for (int picked = 0; picked < 8; picked++) {
            int best = -1;
            double bd = 1e30;
            for (int b = 0; b < n; b++) {
                if (b == a || has(a, b)) continue;
                double d = weighted(w[(size_t)a].at, w[(size_t)b].at);
                if (d < bd) { bd = d; best = b; }
            }
            if (best < 0 || (picked >= 2 && bd > 40)) break;
            add(a, best);
            add(best, a);
        }
    return links;
}

}   // namespace

Result plan(const fs::path &bsp, const std::vector<V3> &points, int teleporters, const Options &o) {
    Result res;
    tagfile::Tag t;
    if (!t.load(bsp)) { res.why = "its level can't be read"; return res; }
    Level L;
    if (!load_level(t.root(), L, res.why)) return res;
    find_jumps(L, o.jump, o.drop);
    res.surfaces = (int)L.b.surfaces.size();
    for (size_t s = 0; s < L.b.surfaces.size(); s++) res.walkable += L.walkable((uint32_t)s);

    // The waypoints (bots_route_add): on the ground, one for points within 1 unit, 192 at most.
    std::vector<Way> w;
    std::vector<int> tele_to;
    std::vector<V3> past;   // (on the ground, past the budget: how many more there'd be)
    auto add_point = [&](const V3 &p, size_t index) -> int {
        V3 g;
        uint32_t s = ground(L, p, g);
        if (s == kNone) { res.no_ground++; return -1; }
        for (size_t i = 0; i < w.size(); i++)
            if (dist(w[i].at, g) < 1.0) { res.merged++; return (int)i; }
        if ((int)w.size() >= o.budget) {
            for (auto &q : past)
                if (dist(q, g) < 1.0) { res.merged++; return -1; }
            past.push_back(g);
            res.left_out.push_back((int)index);
            return -1;
        }
        w.push_back({g, s});
        tele_to.push_back(-1);
        return (int)w.size() - 1;
    };
    for (size_t i = 0; i < points.size(); i++) {
        int k = add_point(points[i], i);
        res.point_waypoint.push_back(k);
        if (k < 0) res.dropped++;
        if ((int)i < teleporters && i % 2 == 1) {   // (a way in, then its way out)
            int src = -1;
            V3 g;
            uint32_t s = ground(L, points[i - 1], g);
            if (s != kNone)
                for (size_t j = 0; j < w.size(); j++)
                    if (dist(w[j].at, g) < 1.0) src = (int)j;
            if (src >= 0 && k >= 0 && src != k) tele_to[(size_t)src] = k;
        }
    }
    res.given = (int)w.size();

    // Each hop, both ways, as the game searches it.
    auto evaluate = [&](std::vector<std::vector<int>> &links, std::vector<Hop> &hops) {
        hops.clear();
        int failing = 0;
        for (int a = 0; a < (int)w.size(); a++)
            for (int b : links[(size_t)a]) {
                if (tele_to[(size_t)a] == b) continue;   // (through the teleporter)
                Hop h;
                h.a = a;
                h.b = b;
                h.straight = dist(w[(size_t)a].at, w[(size_t)b].at);
                h.ok = hop_ok(L, w[(size_t)a], w[(size_t)b], o, o.node_limit, &h.opened);
                if (!h.ok) failing++;
                hops.push_back(h);
            }
        return failing;
    };
    // Whether the links that work (and the teleporters) go from a to b another way.
    auto around = [&](const std::vector<Hop> &hs, int a, int b) {
        std::vector<std::vector<int>> good(w.size());
        for (auto &h : hs)
            if (h.ok && !(h.a == a && h.b == b)) good[(size_t)h.a].push_back(h.b);
        for (size_t i = 0; i < w.size(); i++)
            if (tele_to[i] >= 0) good[i].push_back(tele_to[i]);
        std::vector<char> seen(w.size(), 0);
        std::vector<int> q{a};
        seen[(size_t)a] = 1;
        for (size_t k = 0; k < q.size(); k++)
            for (int n : good[(size_t)q[k]])
                if (!seen[(size_t)n]) { seen[(size_t)n] = 1; q.push_back(n); }
        return seen[(size_t)b] != 0;
    };
    std::vector<std::vector<int>> links = make_links(w, tele_to);
    std::vector<Hop> hops;
    res.failing_before = evaluate(links, hops);
    for (auto &h : hops)
        if (!h.ok && !around(hops, h.a, h.b)) res.stuck_before++;
    const int room = std::min(o.max_add, std::max(0, o.budget - o.spare - res.given));

    // How well the links that work join the map's own waypoints: of every one, how many of the others it reaches.
    auto reach = [&](const std::vector<Hop> &hs) {
        std::vector<std::vector<int>> good(w.size());
        for (auto &h : hs)
            if (h.ok) good[(size_t)h.a].push_back(h.b);
        for (size_t i = 0; i < w.size(); i++)
            if (tele_to[i] >= 0) good[i].push_back(tele_to[i]);
        long total = 0;
        for (int a = 0; a < res.given; a++) {
            std::vector<char> seen(w.size(), 0);
            std::vector<int> q{a};
            seen[(size_t)a] = 1;
            for (size_t k = 0; k < q.size(); k++)
                for (int n : good[(size_t)q[k]])
                    if (!seen[(size_t)n]) { seen[(size_t)n] = 1; q.push_back(n); }
            for (int b = 0; b < res.given; b++) total += seen[(size_t)b] && b != a;
        }
        return total;
    };
    res.reach_before = reach(hops);
    long reached = res.reach_before;
    for (int round = 0; round < o.rounds && (int)res.added.size() < room; round++) {
        // (a round whose nodes don't join more of the map's waypoints, 10 more pairs for each, is taken back)
        const auto w0 = w;
        const auto tele0 = tele_to;
        const auto added0 = res.added;
        const auto walks0 = res.walks;
        const auto hops0 = hops;
        // The failing hops the level's walkable surfaces join and the links that work don't go around (a bot drops a
        // link that fails twice, and goes round by the others), the ones that open most first.
        std::vector<Hop *> todo;
        for (auto &h : hops)
            if (!h.ok && !around(hops, h.a, h.b)) todo.push_back(&h);
        std::stable_sort(todo.begin(), todo.end(), [](const Hop *x, const Hop *y) { return x->opened > y->opened; });
        bool added_any = false;
        for (Hop *h : todo) {
            if ((int)res.added.size() >= room) break;
            const Way A = w[(size_t)h->a], B = w[(size_t)h->b];
            if (hop_ok(L, A, B, o, o.node_limit)) continue;   // (a node added for another hop finished it)
            std::vector<Step> path = way(L, A.at, A.surface, B.at, B.surface, o.radius);
            if (path.empty()) continue;
            h->joined = true;
            // Along the way: from where the bot is, the furthest step a search still finishes (opening no more than
            // plan_limit, or ending where a jump does), a waypoint there, and on from it, until the rest finishes.
            // (as the game searches it, and with room to spare: a search with fewer surfaces can end elsewhere)
            auto safe = [&](const Way &x, const Way &y) {
                return hop_ok(L, x, y, o, o.node_limit) && hop_ok(L, x, y, o, o.plan_limit);
            };
            // (whether the bots would link two waypoints: each is among the other's 8 nearest, those past 40 units
            // only as its first 2; `extra` a waypoint not added yet)
            auto would_link = [&](const Way &x, const Way &y, const Way *extra) {
                auto among = [&](const Way &p, const Way &q) {
                    double d = weighted(p.at, q.at);
                    int nearer = 0;
                    for (auto &v : w)
                        if (dist(v.at, p.at) > 1e-6 && dist(v.at, q.at) > 1e-6 && weighted(p.at, v.at) < d) nearer++;
                    if (extra && dist(extra->at, p.at) > 1e-6 && dist(extra->at, q.at) > 1e-6 && weighted(p.at, extra->at) < d) nearer++;
                    return nearer < 8 && (d <= 40 || nearer < 2);
                };
                return among(x, y) || among(y, x);
            };
            Way cur = A;
            size_t at = 0;
            const size_t added_before = res.added.size();
            std::vector<V3> walk;
            for (auto &st : path) walk.push_back(st.at);
            bool stuck = false;
            auto way_at = [&](size_t k) {
                // (a little into its surface from where the way goes onto it: the ground ray finds it there)
                V3 p = path[k].at;
                if (k + 1 < path.size() && path[k + 1].surface == path[k].surface) return Way{p, path[k].surface};
                if (k + 1 < path.size()) {
                    const V3 &nx = path[k + 1].at;
                    double d = dist2d(p, nx);
                    if (d > 0.01) {
                        double s = std::min(0.3, d / 2) / d;
                        p = {p[0] + (nx[0] - p[0]) * s, p[1] + (nx[1] - p[1]) * s, p[2] + (nx[2] - p[2]) * s};
                    }
                }
                return Way{p, path[k].surface};
            };
            for (int guard = 0; guard < 40 && !(safe(cur, B) && would_link(cur, B, nullptr)); guard++) {
                // The furthest step a waypoint (on the ground there, or one already within a unit of it: the bots
                // merge those) finishes a search to.
                size_t pick = at;
                Way next{};
                int existing = -1;
                for (size_t k = path.size() - 1; k > at; k--) {
                    Way m = way_at(k);
                    V3 g;
                    uint32_t sf = ground(L, {m.at[0], m.at[1], m.at[2] + 0.25}, g);
                    if (sf == kNone || !L.walkable(sf)) continue;
                    Way c{g, sf};
                    int ex = -1;
                    for (size_t i = 0; i < w.size() && ex < 0; i++)
                        if (dist(w[i].at, g) < 1.0) ex = (int)i;
                    if (ex >= 0) c = w[(size_t)ex];
                    if (dist(c.at, cur.at) < 0.5 || !safe(cur, c) || !would_link(cur, c, ex >= 0 ? nullptr : &c)) continue;
                    pick = k;
                    next = c;
                    existing = ex;
                    break;
                }
                if (pick == at) {
                    stuck = true;
                    break;
                }
                if (existing >= 0) {
                    cur = w[(size_t)existing];
                } else {
                    if ((int)res.added.size() >= room) { stuck = true; break; }
                    w.push_back(next);
                    tele_to.push_back(-1);
                    res.added.push_back({next.at[0], next.at[1], next.at[2] + 0.25});
                    added_any = true;
                    cur = next;
                }
                at = pick;
            }
            (void)stuck;
            if (res.added.size() > added_before) res.walks.push_back(walk);   // (the ways nodes went along)
        }
        if (!added_any) break;
        links = make_links(w, tele_to);
        evaluate(links, hops);
        long now = reach(hops);
        if (now - reached < 10L * (long)(res.added.size() - added0.size())) {   // (worth its nodes: 10 pairs each)
            w = w0;
            tele_to = tele0;
            res.added = added0;
            res.walks = walks0;
            hops = hops0;
            break;
        }
        reached = now;
    }
    res.reach_after = reached;
    res.failing_after = 0;
    for (auto &h : hops)
        if (!h.ok) {
            res.failing_after++;
            if (around(hops, h.a, h.b)) { h.around = true; continue; }
            res.stuck_after++;
            if (!h.joined) h.joined = !way(L, w[(size_t)h.a].at, w[(size_t)h.a].surface, w[(size_t)h.b].at, w[(size_t)h.b].surface, o.radius).empty();
            if (!h.joined) res.unjoined++;
        }
    res.hops = hops;
    for (auto &x : w) res.waypoints.push_back(x.at);
    res.ok = true;
    return res;
}

bool picture(const fs::path &bsp, const Result &r, const fs::path &file) {
    tagfile::Tag t;
    Level L;
    std::string why;
    if (!t.load(bsp) || !load_level(t.root(), L, why)) return false;

    // Around the waypoints (a level's floor reaches out to its backdrop), about 1,024 pixels across; heights from
    // the floors there.
    V3 lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
    for (auto &p : r.waypoints)
        for (int k = 0; k < 3; k++) { lo[k] = std::min(lo[k], p[k]); hi[k] = std::max(hi[k], p[k]); }
    if (lo[0] > hi[0]) return false;
    for (int k = 0; k < 2; k++) {
        double m = std::max(8.0, (hi[k] - lo[k]) * 0.12);
        lo[k] -= m;
        hi[k] += m;
    }
    lo[2] -= 3;
    hi[2] += 3;
    double span = std::max(hi[0] - lo[0], hi[1] - lo[1]) + 2;
    const int size = 1024;
    double px_per = size / span;
    int W = std::max(1, (int)((hi[0] - lo[0] + 2) * px_per)), H = std::max(1, (int)((hi[1] - lo[1] + 2) * px_per));
    auto X = [&](double x) { return (x - lo[0] + 1) * px_per; };
    auto Y = [&](double y) { return (y - lo[1] + 1) * px_per; };
    std::vector<std::array<uint8_t, 3>> px((size_t)W * H, {24, 24, 28});
    std::vector<float> top((size_t)W * H, -1e30f);
    // Each walkable surface filled (seen from above, its highest over a pixel).
    for (size_t s = 0; s < L.ring.size(); s++) {
        if (!L.walkable((uint32_t)s) || L.ring[s].size() < 3 || std::fabs(L.normal[s][2]) < 0.05) continue;
        const auto &q = L.ring[s];
        double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
        for (auto &p : q) { x0 = std::min(x0, X(p[0])); x1 = std::max(x1, X(p[0])); y0 = std::min(y0, Y(p[1])); y1 = std::max(y1, Y(p[1])); }
        const V3 &n = L.normal[s];
        double d = n[0] * q[0][0] + n[1] * q[0][1] + n[2] * q[0][2];
        for (int y = std::max(0, (int)y0); y <= std::min(H - 1, (int)y1); y++)
            for (int x = std::max(0, (int)x0); x <= std::min(W - 1, (int)x1); x++) {
                double wx = lo[0] - 1 + (x + 0.5) / px_per, wy = lo[1] - 1 + (y + 0.5) / px_per;
                bool in = true;
                for (size_t k = 0; k < q.size() && in; k++) {
                    auto &a = q[k], &c = q[(k + 1) % q.size()];
                    double cr = (c[0] - a[0]) * (wy - a[1]) - (c[1] - a[1]) * (wx - a[0]);
                    in = n[2] > 0 ? cr >= -1e-9 : cr <= 1e-9;
                }
                if (!in) continue;
                float z = (float)((d - n[0] * wx - n[1] * wy) / n[2]);
                if (z > top[(size_t)y * W + x]) top[(size_t)y * W + x] = z;
            }
    }
    for (size_t i = 0; i < top.size(); i++)
        if (top[i] > -1e29f) {
            uint8_t g = (uint8_t)(70 + 150 * std::clamp((top[i] - lo[2]) / std::max(0.01, hi[2] - lo[2]), 0.0, 1.0));
            px[i] = {g, g, g};
        }
    auto plot = [&](double x, double y, std::array<uint8_t, 3> c, int rad) {
        int cx = (int)X(x), cy = (int)Y(y);
        for (int dy = -rad; dy <= rad; dy++)
            for (int dx = -rad; dx <= rad; dx++) {
                int a = cx + dx, b = cy + dy;
                if (a >= 0 && b >= 0 && a < W && b < H) px[(size_t)b * W + a] = c;
            }
    };
    auto line = [&](const V3 &a, const V3 &b, std::array<uint8_t, 3> c) {
        double len = dist2d(a, b) * px_per;
        int n = std::max(1, (int)std::ceil(len));
        for (int i = 0; i <= n; i++) {
            double f = (double)i / n;
            plot(a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, c, 0);
        }
    };
    for (auto &wk : r.walks)
        for (size_t k = 1; k < wk.size(); k++) line(wk[k - 1], wk[k], {235, 200, 40});
    for (auto &h : r.hops)
        if (!h.ok && !h.around && h.a < (int)r.waypoints.size() && h.b < (int)r.waypoints.size())
            line(r.waypoints[(size_t)h.a], r.waypoints[(size_t)h.b], {220, 50, 50});
    for (int i = 0; i < r.given && i < (int)r.waypoints.size(); i++) plot(r.waypoints[(size_t)i][0], r.waypoints[(size_t)i][1], {60, 140, 255}, 3);
    for (auto &p : r.added) plot(p[0], p[1], {255, 140, 0}, 4);
    // A .bmp, bottom-up rows (the map's +y up).
    int stride = (W * 3 + 3) & ~3;
    std::string b(54, '\0');
    auto u32 = [&](size_t at, uint32_t v) { for (int k = 0; k < 4; k++) b[at + k] = (char)((v >> (8 * k)) & 255); };
    b[0] = 'B';
    b[1] = 'M';
    u32(2, (uint32_t)(54 + stride * H));
    u32(10, 54);
    u32(14, 40);
    u32(18, (uint32_t)W);
    u32(22, (uint32_t)H);
    b[26] = 1;
    b[28] = 24;
    u32(34, (uint32_t)(stride * H));
    for (int y = 0; y < H; y++) {
        std::string row((size_t)stride, '\0');
        for (int x = 0; x < W; x++) {
            auto &c = px[(size_t)y * W + x];
            row[(size_t)x * 3] = (char)c[2];
            row[(size_t)x * 3 + 1] = (char)c[1];
            row[(size_t)x * 3 + 2] = (char)c[0];
        }
        b += row;
    }
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    f << b;
    return (bool)f;
}

}   // namespace botnodes
