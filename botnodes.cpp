// Halo Map Porter - route points for Halo 1 Unlimited's multiplayer bots (botnodes.hpp).
#include "botnodes.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <queue>
#include <map>
#include <set>
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
    std::vector<std::vector<V3>> portals;   // the cluster portals' corners (where the level's maker put doorways)
};

bool load_level(const tagfile::Elem &r, Level &L, std::string &why) {
    tagfile::Block cbs = r.block("collision bsp");
    if (cbs.size() == 0 || !coll::read_bsp(cbs[0], L.b) || L.b.surfaces.empty()) { why = "has no collision BSP"; return false; }
    tagfile::Block pfs = r.block("pathfinding surfaces");
    if (pfs.size() == 0) { why = "has no pathfinding data (the AI can't walk it)"; return false; }
    {
        tagfile::Block cp = r.block("cluster portals");
        for (size_t i = 0; i < cp.size(); i++) {
            std::vector<V3> pts;
            tagfile::Block vs = cp[i].block("vertices");
            for (size_t k = 0; k < vs.size(); k++) pts.push_back({vs[k].num("point", 0), vs[k].num("point", 1), vs[k].num("point", 2)});
            if (pts.size() >= 3) L.portals.push_back(std::move(pts));
        }
    }
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

// ---- the level's ramps and narrow ways (doorways, tunnels, bridges), for waypoints bots follow along them
// The walkable floor on a grid (each cell's floors where a biped stands, with room above), joined where a biped walks
// from one to the next (a step up or a slope, nothing in the way at the knee).
struct Grid {
    double h = 0.5;
    double x0 = 0, y0 = 0;
    int nx = 0, ny = 0;
    std::vector<int> first;          // cell -> its first floor (size cells+1)
    std::vector<V3> at;
    std::vector<float> nz;           // each floor's normal's up part (1 flat)
    std::vector<std::array<float, 2>> nxy;   // and its sideways part (which way the slope faces)
    std::vector<std::array<int, 8>> nb;   // its neighbour in each of 8 directions (-1: none)
};
const int kDir[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};

void build_grid(const Level &L, const V3 &lo, const V3 &hi, Grid &g) {
    double area = std::max(1.0, (hi[0] - lo[0]) * (hi[1] - lo[1]));
    g.h = std::max(0.5, std::sqrt(area / 600000.0));
    g.x0 = lo[0];
    g.y0 = lo[1];
    g.nx = std::max(1, (int)std::ceil((hi[0] - lo[0]) / g.h));
    g.ny = std::max(1, (int)std::ceil((hi[1] - lo[1]) / g.h));
    struct Spot { float z, nz, nx, ny; };
    std::vector<std::vector<Spot>> cells((size_t)g.nx * g.ny);
    for (uint32_t s = 0; s < L.ring.size(); s++) {
        if (!L.walkable(s) || L.ring[s].size() < 3 || L.normal[s][2] < 0.3) continue;
        const auto &q = L.ring[s];
        const V3 &n = L.normal[s];
        double d = n[0] * q[0][0] + n[1] * q[0][1] + n[2] * q[0][2];
        double a0 = 1e30, b0 = 1e30, a1 = -1e30, b1 = -1e30;
        for (auto &p : q) { a0 = std::min(a0, p[0]); a1 = std::max(a1, p[0]); b0 = std::min(b0, p[1]); b1 = std::max(b1, p[1]); }
        int i0 = std::max(0, (int)std::ceil((a0 - g.x0) / g.h - 0.5)), i1 = std::min(g.nx - 1, (int)std::floor((a1 - g.x0) / g.h - 0.5));
        int j0 = std::max(0, (int)std::ceil((b0 - g.y0) / g.h - 0.5)), j1 = std::min(g.ny - 1, (int)std::floor((b1 - g.y0) / g.h - 0.5));
        for (int j = j0; j <= j1; j++)
            for (int i = i0; i <= i1; i++) {
                double x = g.x0 + (i + 0.5) * g.h, y = g.y0 + (j + 0.5) * g.h;
                bool in = true;
                for (size_t k = 0; k < q.size() && in; k++) {
                    auto &a = q[k], &c = q[(k + 1) % q.size()];
                    in = (c[0] - a[0]) * (y - a[1]) - (c[1] - a[1]) * (x - a[0]) >= -1e-9;
                }
                if (in) cells[(size_t)j * g.nx + i].push_back({(float)((d - n[0] * x - n[1] * y) / n[2]), (float)n[2], (float)n[0], (float)n[1]});
            }
    }
    g.first.assign(cells.size() + 1, 0);
    for (size_t c = 0; c < cells.size(); c++) {
        g.first[c] = (int)g.at.size();
        auto &v = cells[c];
        std::sort(v.begin(), v.end(), [](const Spot &a, const Spot &b) { return a.z < b.z; });
        double x = g.x0 + (double)(c % g.nx + 0.5) * g.h, y = g.y0 + (double)(c / g.nx + 0.5) * g.h;
        for (size_t k = 0; k < v.size(); k++) {
            if (k + 1 < v.size() && v[k + 1].z - v[k].z < 0.15) continue;
            double p[3] = {x, y, v[k].z + 0.05}, up[3] = {0, 0, 0.6};
            if (coll::test_vector(L.b, p, up, 1, true).surface != kNone) continue;
            g.at.push_back({x, y, v[k].z});
            g.nz.push_back(v[k].nz);
            g.nxy.push_back({v[k].nx, v[k].ny});
        }
    }
    g.first[cells.size()] = (int)g.at.size();
    g.nb.assign(g.at.size(), {-1, -1, -1, -1, -1, -1, -1, -1});
    for (int y = 0; y < g.ny; y++)
        for (int x = 0; x < g.nx; x++) {
            int c = y * g.nx + x;
            for (int f = g.first[(size_t)c]; f < g.first[(size_t)c + 1]; f++)
                for (int d = 0; d < 4; d++) {   // (each pair once: the other way round set with it)
                    int x2 = x + kDir[d][0], y2 = y + kDir[d][1];
                    if (x2 < 0 || y2 < 0 || x2 >= g.nx || y2 >= g.ny) continue;
                    int c2 = y2 * g.nx + x2;
                    double run = g.h * ((kDir[d][0] && kDir[d][1]) ? 1.4142 : 1.0);
                    int best = -1;
                    double bdz = 1e30;
                    for (int k = g.first[(size_t)c2]; k < g.first[(size_t)c2 + 1]; k++) {
                        double dz = std::fabs(g.at[(size_t)k][2] - g.at[(size_t)f][2]);
                        double tan_a = std::sqrt(std::max(0.0, 1.0 - g.nz[(size_t)f] * g.nz[(size_t)f])) / std::max(0.3f, g.nz[(size_t)f]);
                        double tan_b = std::sqrt(std::max(0.0, 1.0 - g.nz[(size_t)k] * g.nz[(size_t)k])) / std::max(0.3f, g.nz[(size_t)k]);
                        if (dz <= 0.3 + run * std::max(tan_a, tan_b) * 1.1 && dz < bdz) { bdz = dz; best = k; }
                    }
                    if (best < 0) continue;
                    const V3 &a = g.at[(size_t)f], &b = g.at[(size_t)best];
                    double p[3] = {a[0], a[1], a[2] + 0.35}, v[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
                    if (coll::test_vector(L.b, p, v, 1, true).surface != kNone) continue;
                    g.nb[(size_t)f][(size_t)d] = best;
                    g.nb[(size_t)best][(size_t)d + 4] = f;
                }
        }
}

// Waypoints along the level's ramps (at the foot, the middle, each turn, the top, and every 4 units between) and through its
// narrow ways (doorways, tunnels, bridges: a waypoint in the middle and either side of a doorway, and along a long one
// every few units), on the floor players can walk to from their spawns. `kind`: 1 ramp, 2 narrow way.
struct Feature { V3 p; int kind; };
// `spawns`: every waypoint; `walk`: the ones bots walk on from (the floor they get to is walked from these).
std::vector<Feature> find_features(const Level &L, const std::vector<V3> &spawns, const std::vector<V3> &walk_from) {
    std::vector<Feature> out;
    if (spawns.empty()) return out;
    V3 lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
    for (auto &p : spawns)
        for (int k = 0; k < 3; k++) { lo[k] = std::min(lo[k], p[k]); hi[k] = std::max(hi[k], p[k]); }
    for (int k = 0; k < 2; k++) { lo[k] -= 12; hi[k] += 12; }
    Grid g;
    build_grid(L, lo, hi, g);
    const size_t n = g.at.size();
    if (n == 0) return out;
    // The floor players get to: walked from each waypoint's floor.
    std::vector<char> reach(n, 0);
    std::vector<int> q;
    for (auto &p : walk_from) {
        int ci = (int)std::floor((p[0] - g.x0) / g.h), cj = (int)std::floor((p[1] - g.y0) / g.h);
        if (ci < 0 || cj < 0 || ci >= g.nx || cj >= g.ny) continue;
        int c = cj * g.nx + ci, best = -1;
        for (int f = g.first[(size_t)c]; f < g.first[(size_t)c + 1]; f++)
            if (g.at[(size_t)f][2] <= p[2] + 0.5 && (best < 0 || g.at[(size_t)f][2] > g.at[(size_t)best][2])) best = f;
        if (best >= 0 && !reach[(size_t)best]) { reach[(size_t)best] = 1; q.push_back(best); }
    }
    // (walked on slopes of 45 degrees at most: the AI calls steeper rock walkable, which would take the bots down a pit's
    // walls to its bottom, or up a cliff face)
    for (size_t k = 0; k < q.size(); k++)
        for (int m : g.nb[(size_t)q[k]])
            if (m >= 0 && !reach[(size_t)m] && g.nz[(size_t)m] >= 0.707f) { reach[(size_t)m] = 1; q.push_back(m); }
    // Waypoints go only on floor a bot walks on gladly: 30 degrees at most (not a rock slope it scrambles over), and no
    // lower than 1.5 units under the map's lowest point or 3 over its highest (not a pit's floor, not a rooftop).
    {
        double zlo = 1e30, zhi = -1e30;
        for (auto &p : spawns) { zlo = std::min(zlo, p[2]); zhi = std::max(zhi, p[2]); }
        for (size_t f = 0; f < n; f++)
            if (reach[f] && (g.nz[f] < 0.866f || g.at[f][2] < zlo - 1.5 || g.at[f][2] > zhi + 3.0)) reach[f] = 0;
    }
    // Each floor's width across the narrowest way (both ways along a line of the grid, to a wall or an edge), and how
    // far it goes the other way (along the passage).
    const int kMax = (int)std::ceil(12.0 / g.h);
    auto run = [&](int f, int d) {
        int steps = 0;
        for (int c = f; steps < kMax; steps++) {
            int m = g.nb[(size_t)c][(size_t)d];
            if (m < 0) break;
            c = m;
        }
        return steps * g.h * ((d & 1) ? 1.4142 : 1.0);
    };
    const double kNarrow = 4.0;   // world units across at most (a doorway, a corridor, a bridge)
    std::vector<char> narrow(n, 0);
    std::vector<V3> axis(n, V3{0, 0, 0});
    for (size_t f = 0; f < n; f++) {
        if (!reach[f]) continue;
        double wmin = 1e30, along = 0;
        int dmin = 0;
        for (int d = 0; d < 4; d++) {
            double w = run((int)f, d) + run((int)f, d + 4) + g.h;
            if (w < wmin) { wmin = w; dmin = d; }
        }
        int perp = (dmin + 2) % 4;
        along = run((int)f, perp) + run((int)f, perp + 4) + g.h;
        if (wmin <= kNarrow && along >= std::max(2.0 * wmin, 3.0)) {
            narrow[f] = 1;
            double l = std::hypot((double)kDir[perp][0], (double)kDir[perp][1]);
            axis[f] = {kDir[perp][0] / l, kDir[perp][1] / l, 0};
        }
    }
    auto slope = [&](size_t f) { return g.nz[f] < 0.993f; };   // (steeper than about 7 degrees)
    // A part of the floor (narrow ways, or ramps): its pieces, and through each the way from end to end.
    auto pieces = [&](const std::vector<char> &in, std::vector<std::vector<int>> &comps, bool one_slope = false) {
        std::vector<int> id(n, -1);
        for (size_t f = 0; f < n; f++) {
            if (!in[f] || id[f] >= 0) continue;
            comps.push_back({});
            std::vector<int> st{(int)f};
            id[f] = (int)comps.size() - 1;
            while (!st.empty()) {
                int c = st.back();
                st.pop_back();
                comps.back().push_back(c);
                for (int m : g.nb[(size_t)c]) {
                    if (m < 0 || !in[(size_t)m] || id[(size_t)m] >= 0) continue;
                    if (one_slope) {   // (a ramp is one slope: its neighbour on another isn't part of it)
                        const auto &a = g.nxy[(size_t)c], &b = g.nxy[(size_t)m];
                        // (within about 8 degrees: a flight's triangles tilt a little apart, as a spiral stair's do)
                        if (a[0] * b[0] + a[1] * b[1] + g.nz[(size_t)c] * g.nz[(size_t)m] < 0.99) continue;
                    }
                    id[(size_t)m] = id[f];
                    st.push_back(m);
                }
            }
        }
    };
    auto walk = [&](const std::vector<int> &comp, int from, std::vector<double> &dist_out, std::vector<int> &par) {
        std::map<int, size_t> idx;
        for (size_t k = 0; k < comp.size(); k++) idx[comp[k]] = k;
        dist_out.assign(comp.size(), 1e30);
        par.assign(comp.size(), -1);
        using H = std::pair<double, int>;
        std::priority_queue<H, std::vector<H>, std::greater<H>> pq;
        dist_out[idx[from]] = 0;
        pq.push({0, from});
        while (!pq.empty()) {
            auto [d, c] = pq.top();
            pq.pop();
            if (d > dist_out[idx[c]]) continue;
            for (int m : g.nb[(size_t)c]) {
                auto it = m >= 0 ? idx.find(m) : idx.end();
                if (it == idx.end()) continue;
                double nd = d + dist(g.at[(size_t)c], g.at[(size_t)m]);
                if (nd < dist_out[it->second]) { dist_out[it->second] = nd; par[it->second] = c; pq.push({nd, m}); }
            }
        }
        return idx;
    };
    // Along a way (cells, end to end): a waypoint at each end, at each turn, and every `spacing` between.
    auto along_way = [&](const std::vector<int> &way, int kind, double spacing, bool middle) {
        if (way.empty()) return;
        std::vector<double> sl(way.size(), 0);
        for (size_t k = 1; k < way.size(); k++) sl[k] = sl[k - 1] + dist(g.at[(size_t)way[k - 1]], g.at[(size_t)way[k]]);
        auto at_len = [&](double x) {
            size_t k = (size_t)(std::lower_bound(sl.begin(), sl.end(), x) - sl.begin());
            return g.at[(size_t)way[std::min(k, way.size() - 1)]];
        };
        const double total = sl.back();
        std::vector<double> marks{0, total};
        if (middle && total > 1.5) marks.push_back(total / 2);
        for (double x = 1.5; x + 1.5 < total; x += 0.5) {   // (turns: the way's heading changes by 40 degrees or more)
            V3 a = at_len(x - 1.5), b = at_len(x), c = at_len(x + 1.5);
            double ux = b[0] - a[0], uy = b[1] - a[1], vx = c[0] - b[0], vy = c[1] - b[1];
            double lu = std::hypot(ux, uy), lv = std::hypot(vx, vy);
            if (lu < 0.5 || lv < 0.5) continue;
            if ((ux * vx + uy * vy) / (lu * lv) < 0.766) { marks.push_back(x); x += 2; }
        }
        std::sort(marks.begin(), marks.end());
        std::vector<double> all = marks;
        for (size_t k = 0; k + 1 < marks.size(); k++) {
            double gap = marks[k + 1] - marks[k];
            int extra = (int)std::ceil(gap / spacing) - 1;
            for (int i = 1; i <= extra; i++) all.push_back(marks[k] + gap * i / (extra + 1));
        }
        for (double x : all) out.push_back({at_len(x), kind});
    };
    // Past a way's end, out of it: `len` further on, the way it was going.
    auto beyond = [&](int from, int toward_dir_cell, double len) {
        V3 a = g.at[(size_t)toward_dir_cell], b = g.at[(size_t)from];
        double dx = b[0] - a[0], dy = b[1] - a[1], l = std::hypot(dx, dy);
        int c = from;
        if (l < 1e-6) return c;
        for (double went = 0; went < len;) {
            int best = -1;
            double bd = -1e30;
            for (int d = 0; d < 8; d++) {
                int m = g.nb[(size_t)c][(size_t)d];
                if (m < 0) continue;
                double s = (kDir[d][0] * dx + kDir[d][1] * dy) / (l * std::hypot((double)kDir[d][0], (double)kDir[d][1]));
                if (s > 0.7 && s > bd) { bd = s; best = m; }
            }
            if (best < 0) break;
            went += dist(g.at[(size_t)c], g.at[(size_t)best]);
            c = best;
        }
        return c;
    };
    // Doorways: where a door-sized cluster portal (upright, 8 units across and high at most) stands on floor bots get
    // to, a waypoint on its threshold, so the way through it is plain to the bots (first: the ones near it give way).
    for (auto &pp : L.portals) {
        double zlo = 1e30, zhi = -1e30, wide = 0;
        for (auto &p : pp) { zlo = std::min(zlo, p[2]); zhi = std::max(zhi, p[2]); }
        for (size_t i = 0; i < pp.size(); i++)
            for (size_t j = i + 1; j < pp.size(); j++) wide = std::max(wide, std::hypot(pp[i][0] - pp[j][0], pp[i][1] - pp[j][1]));
        double ux = pp[1][0] - pp[0][0], uy = pp[1][1] - pp[0][1], uz = pp[1][2] - pp[0][2];
        double vx = pp[2][0] - pp[0][0], vy = pp[2][1] - pp[0][1], vz = pp[2][2] - pp[0][2];
        double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx, nl = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (nl < 1e-9 || std::fabs(nz / nl) > 0.3) continue;   // (a floor or ceiling portal)
        if (wide < 0.8 || wide > 8.0 || zhi - zlo > 8.0 || zhi - zlo < 1.0) continue;
        // (its foot: the middle of its lowest corners)
        double mx = 0, my = 0;
        int k = 0;
        for (auto &p : pp)
            if (p[2] < zlo + 0.3) { mx += p[0]; my += p[1]; k++; }
        mx /= k;
        my /= k;
        int best = -1;
        double bd = 1.0;
        int ci0 = (int)std::floor((mx - g.x0) / g.h), cj0 = (int)std::floor((my - g.y0) / g.h), r = (int)std::ceil(1.0 / g.h);
        for (int cj = cj0 - r; cj <= cj0 + r; cj++)
            for (int ci = ci0 - r; ci <= ci0 + r; ci++) {
                if (ci < 0 || cj < 0 || ci >= g.nx || cj >= g.ny) continue;
                int c = cj * g.nx + ci;
                for (int f = g.first[(size_t)c]; f < g.first[(size_t)c + 1]; f++) {
                    const V3 &a = g.at[(size_t)f];
                    if (!reach[(size_t)f] || a[2] < zlo - 0.6 || a[2] > zlo + 2.0) continue;
                    double d = std::hypot(a[0] - mx, a[1] - my);
                    if (d < bd) { bd = d; best = f; }
                }
            }
        if (best >= 0) out.push_back({g.at[(size_t)best], 4});   // (4: a doorway's, counted with the narrow ways')
    }
    // Ramps: the sloped floor that climbs a unit or more; from its lowest to its highest along it.
    {
        std::vector<char> in(n, 0);
        for (size_t f = 0; f < n; f++) in[f] = reach[f] && slope(f);
        std::vector<std::vector<int>> comps;
        pieces(in, comps, true);
        for (auto &comp : comps) {
            int lo_c = comp[0], hi_c = comp[0];
            for (int c : comp) {
                if (g.at[(size_t)c][2] < g.at[(size_t)lo_c][2]) lo_c = c;
                if (g.at[(size_t)c][2] > g.at[(size_t)hi_c][2]) hi_c = c;
            }
            // (one that climbs a third of a unit or more: a short leg of an L-shaped ramp too; and a ramp, not rolling
            // ground: most of it one slope, facing one way)
            if (g.at[(size_t)hi_c][2] - g.at[(size_t)lo_c][2] < 0.35 || comp.size() * g.h * g.h < 1.0) continue;
            {
                std::map<std::array<int, 3>, int> slopes;   // (each slope, rounded)
                for (int c : comp) {
                    const auto &v = g.nxy[(size_t)c];
                    slopes[{(int)std::lround(v[0] * 25), (int)std::lround(v[1] * 25), (int)std::lround(g.nz[(size_t)c] * 50)}]++;
                }
                int most = 0;
                for (auto &[k, c] : slopes) most = std::max(most, c);
                if (most < 0.6 * comp.size()) continue;
                // (and edged: much of its side a wall or a drop, as a ramp's sides are; a slope of open ground meets
                // more ground all round)
                std::set<int> inside(comp.begin(), comp.end());
                int hard = 0, soft = 0;
                for (int c : comp)
                    for (int d = 0; d < 8; d += 2) {
                        int m = g.nb[(size_t)c][(size_t)d];
                        if (m < 0) hard++;
                        else if (!inside.count(m)) soft++;
                    }
                if (hard < 0.35 * (hard + soft)) continue;
            }
            std::vector<double> dd;
            std::vector<int> par;
            auto idx = walk(comp, lo_c, dd, par);
            if (dd[idx[hi_c]] >= 1e29) continue;
            std::vector<int> way;
            for (int c = hi_c; c >= 0; c = par[idx[c]]) way.push_back(c);
            std::reverse(way.begin(), way.end());
            // (its middle line: the shortest way from its lowest corner to its highest runs along one side, where a
            // bot walks at the drop's edge; each step moved across the slope to the middle of the ramp there)
            {
                std::set<int> inside(comp.begin(), comp.end());
                for (auto &c : way) {
                    const auto &v = g.nxy[(size_t)c];
                    double lv = std::hypot(v[0], v[1]);
                    if (lv < 1e-4) continue;
                    int dacross = -1;
                    double best = 1e30;
                    for (int d = 0; d < 4; d++) {   // (the grid's way most nearly level across the slope)
                        double dot = std::fabs((kDir[d][0] * v[0] + kDir[d][1] * v[1]) / (lv * std::hypot(kDir[d][0], kDir[d][1])));
                        if (dot < best) { best = dot; dacross = d; }
                    }
                    auto steps = [&](int d) {
                        int k = 0;
                        for (int t = c; k < kMax; k++) {
                            int m = g.nb[(size_t)t][(size_t)d];
                            if (m < 0 || !inside.count(m)) break;
                            t = m;
                        }
                        return k;
                    };
                    int left = steps(dacross), right = steps(dacross + 4), shift = (left - right) / 2, t = c;
                    for (int k = 0; k < std::abs(shift); k++) {
                        int m = g.nb[(size_t)t][(size_t)(shift > 0 ? dacross : dacross + 4)];
                        if (m < 0) break;
                        t = m;
                    }
                    c = t;
                }
            }
            // (from the floor before its foot to the floor past its top)
            if (way.size() >= 2) {
                way.insert(way.begin(), beyond(way.front(), way[std::min<size_t>(3, way.size() - 1)], 1.0));
                way.push_back(beyond(way.back(), way[way.size() >= 5 ? way.size() - 5 : 0], 1.0));
            }
            along_way(way, 1, 4.0, true);
        }
    }
    // Landings: a small flat floor with sloped floor going up from it and sloped floor going down (the corners of a
    // stair that turns, a spiral's): a waypoint in its middle, so the bots' way turns there instead of cutting across.
    {
        std::vector<char> flat(n, 0);
        for (size_t f = 0; f < n; f++) flat[f] = reach[f] && !slope(f);
        std::vector<std::vector<int>> comps;
        pieces(flat, comps);
        for (auto &comp : comps) {
            if (comp.size() * g.h * g.h > 16.0) continue;   // (a floor, not a landing)
            double z = 0;
            for (int c : comp) z += g.at[(size_t)c][2];
            z /= (double)comp.size();
            bool up = false, down = false;
            std::set<int> inside(comp.begin(), comp.end());
            int edge = 0;
            std::vector<int> rim;
            for (int c : comp)
                for (int m : g.nb[(size_t)c]) {
                    if (m >= 0 && inside.count(m)) continue;
                    rim.push_back(c);
                    if (m < 0 || !reach[(size_t)m] || !slope((size_t)m)) continue;
                    if (g.at[(size_t)m][2] > z + 0.1) up = true;
                    if (g.at[(size_t)m][2] < z - 0.1) down = true;
                    edge++;
                }
            if (!up || !down) continue;
            // (its middle: the cell furthest from its rim)
            int best = comp[0];
            double bd = -1;
            for (int c : comp) {
                double d = 1e30;
                for (int e : rim) d = std::min(d, dist(g.at[(size_t)c], g.at[(size_t)e]));
                if (d > bd) { bd = d; best = c; }
            }
            out.push_back({g.at[(size_t)best], 1});
        }
    }
    // The grid cell next to a floor's, in direction d: no floor there as high as it (less half a unit), or none at
    // all: a drop (a floor there that high, it's a wall or a step in the way).
    auto drop_beyond = [&](int f, int d) {
        const V3 &a = g.at[(size_t)f];
        int ci = (int)std::floor((a[0] - g.x0) / g.h) + kDir[d][0], cj = (int)std::floor((a[1] - g.y0) / g.h) + kDir[d][1];
        if (ci < 0 || cj < 0 || ci >= g.nx || cj >= g.ny) return true;
        int c = cj * g.nx + ci;
        for (int k = g.first[(size_t)c]; k < g.first[(size_t)c + 1]; k++)
            if (g.at[(size_t)k][2] >= a[2] - 0.5) return false;
        return true;
    };
    // Open floor (before the narrow ways: the bots' 512 waypoints go to filling the floor first): wherever it's more
    // than kOpen units' walk from every waypoint (the map's, yours, the ones above), one
    // more there, the farthest first, until none is that far. (A bot picks a waypoint to go to: floor far from all of
    // them is floor bots never cross.)
    {
        const double kOpen = 3.5;   // (your Snow Space nodes on the open ice: 3 to 4 units from the nearest waypoint)
        const float kFar = 1e30f;
        std::vector<float> far_(n, kFar);
        auto floor_at = [&](const V3 &p) {
            int ci = (int)std::floor((p[0] - g.x0) / g.h), cj = (int)std::floor((p[1] - g.y0) / g.h);
            if (ci < 0 || cj < 0 || ci >= g.nx || cj >= g.ny) return -1;
            int c = cj * g.nx + ci, best = -1;
            for (int f = g.first[(size_t)c]; f < g.first[(size_t)c + 1]; f++)
                if (g.at[(size_t)f][2] <= p[2] + 0.5 && (best < 0 || g.at[(size_t)f][2] > g.at[(size_t)best][2])) best = f;
            return best;
        };
        using QE = std::pair<float, int>;
        auto spread = [&](int from) {   // (walking distances from a new waypoint, where they're shorter than before)
            if (from < 0 || far_[(size_t)from] == 0) return;
            std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;
            far_[(size_t)from] = 0;
            pq.push({0, from});
            while (!pq.empty()) {
                auto [d, c] = pq.top();
                pq.pop();
                if (d > far_[(size_t)c] || d > 40) continue;
                for (int m : g.nb[(size_t)c]) {
                    if (m < 0) continue;
                    float nd = d + (float)dist(g.at[(size_t)c], g.at[(size_t)m]);
                    if (nd < far_[(size_t)m]) { far_[(size_t)m] = nd; pq.push({nd, m}); }
                }
            }
        };
        for (auto &p : spawns) spread(floor_at(p));
        for (size_t k = 0; k < out.size(); k++) spread(floor_at(out[k].p));
        // (only floor as high as the map's own points go, give or take 1.5 units: below them is the bottom of a pit or a
        // drop players die in, not a place to send bots)
        double zlo = 1e30, zhi = -1e30;
        for (auto &p : spawns) { zlo = std::min(zlo, p[2]); zhi = std::max(zhi, p[2]); }
        for (int added = 0; added < 400; added++) {
            int best = -1;
            for (size_t f = 0; f < n; f++)
                if (reach[f] && far_[f] > kOpen && g.at[f][2] >= zlo - 1.5 && g.at[f][2] <= zhi + 1.5 &&
                    (best < 0 || far_[f] > far_[(size_t)best]))
                    best = (int)f;
            if (best < 0) break;
            out.push_back({g.at[(size_t)best], 3});
            spread(best);
        }
    }
    // Narrow ways: from end to end (its two ends furthest apart), and a little past each into the open.
    {
        std::vector<std::vector<int>> comps;
        pieces(narrow, comps);
        for (auto &comp : comps) {
            if (comp.size() * g.h * g.h < 1.0) continue;
            std::vector<double> dd;
            std::vector<int> par;
            auto idx = walk(comp, comp[0], dd, par);
            int a = comp[0];
            for (int c : comp)
                if (dd[idx[c]] < 1e29 && dd[idx[c]] > dd[idx[a]]) a = c;
            idx = walk(comp, a, dd, par);
            int b = a;
            for (int c : comp)
                if (dd[idx[c]] < 1e29 && dd[idx[c]] > dd[idx[b]]) b = c;
            std::vector<int> way;
            for (int c = b; c >= 0; c = par[idx[c]]) way.push_back(c);
            std::reverse(way.begin(), way.end());
            if (way.size() < 2) continue;
            const std::vector<int> main_line = way;   // (before it's moved to the middle: the branches start from it)
            // (its middle line: each step moved across to the middle of the narrow floor there)
            for (auto &c : way) {
                const V3 &ax = axis[(size_t)c];
                int dacross = -1;
                for (int d = 0; d < 4; d++)
                    if (std::fabs(kDir[d][0] * ax[0] + kDir[d][1] * ax[1]) < 0.3) { dacross = d; break; }
                if (dacross < 0) continue;
                int left = 0, right = 0;
                for (int t = c; left < kMax && g.nb[(size_t)t][(size_t)dacross] >= 0; left++) t = g.nb[(size_t)t][(size_t)dacross];
                for (int t = c; right < kMax && g.nb[(size_t)t][(size_t)dacross + 4] >= 0; right++) t = g.nb[(size_t)t][(size_t)dacross + 4];
                int shift = (left - right) / 2, t = c;
                for (int k = 0; k < std::abs(shift); k++) {
                    int m = g.nb[(size_t)t][(size_t)(shift > 0 ? dacross : dacross + 4)];
                    if (m < 0) break;
                    t = m;
                }
                c = t;
            }
            // (a ledge: a walkway with a drop at its side for much of it, where a bot that strays from the line falls;
            // a doorway, tunnel or corridor between walls: its ends and middle are enough)
            size_t drops = 0;
            for (int c : way) {
                const V3 &ax = axis[(size_t)c];
                int dacross = -1;
                for (int d = 0; d < 4; d++)
                    if (std::fabs(kDir[d][0] * ax[0] + kDir[d][1] * ax[1]) < 0.3) { dacross = d; break; }
                if (dacross < 0) continue;
                bool drop = false;
                for (int side = 0; side < 2 && !drop; side++) {
                    int d = dacross + 4 * side, t = c;
                    for (int k = 0; k < kMax && g.nb[(size_t)t][(size_t)d] >= 0; k++) t = g.nb[(size_t)t][(size_t)d];
                    drop = drop_beyond(t, d);
                }
                drops += drop;
            }
            const bool ledge = drops * 2 >= way.size();
            way.insert(way.begin(), beyond(way.front(), way[std::min<size_t>(3, way.size() - 1)], 1.5));
            way.push_back(beyond(way.back(), way[way.size() >= 5 ? way.size() - 5 : 0], 1.5));
            along_way(way, 2, ledge ? 2.0 : 6.0, !ledge);
            // Its branches: a narrow way that forks (a doorway off a walkway, a side tunnel) is followed from end to end
            // only along its longest line. Each part more than 4 units' walk from the lines followed so far gets a line
            // of its own, from where it leaves them to its far end and on into the open there: its doorway, and each
            // turn on the way.
            {
                std::set<int> inside(comp.begin(), comp.end());
                std::vector<int> covered = main_line;
                for (int br = 0; br < 8; br++) {
                    std::map<int, double> far_of;
                    std::map<int, int> from;
                    using QE = std::pair<double, int>;
                    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;
                    for (int c : covered) { far_of[c] = 0; from[c] = -1; pq.push({0, c}); }
                    while (!pq.empty()) {
                        auto [d, c] = pq.top();
                        pq.pop();
                        if (d > far_of[c]) continue;
                        for (int m : g.nb[(size_t)c]) {
                            if (m < 0 || !inside.count(m)) continue;
                            double nd = d + dist(g.at[(size_t)c], g.at[(size_t)m]);
                            auto it = far_of.find(m);
                            if (it == far_of.end() || nd < it->second) { far_of[m] = nd; from[m] = c; pq.push({nd, m}); }
                        }
                    }
                    int tip = -1;
                    double td = 4.0;   // (a side way, not a nook)
                    for (auto &[c, d] : far_of)
                        if (d > td) { td = d; tip = c; }
                    if (tip < 0) break;
                    std::vector<int> branch;
                    for (int c = tip; c >= 0; c = from[c]) branch.push_back(c);
                    std::reverse(branch.begin(), branch.end());   // (from the line it leaves to its far end)
                    covered.insert(covered.end(), branch.begin(), branch.end());
                    branch.push_back(beyond(branch.back(), branch[branch.size() >= 5 ? branch.size() - 5 : 0], 1.5));
                    along_way(branch, 2, 6.0, false);
                }
            }
        }
    }
    // Away from walls and drops: a waypoint within a unit of one has bots rubbing the wall (crouching under it) or
    // walking at the edge. Each is moved, up to a unit, to the floor nearby furthest from either.
    {
        const double kWant = 1.0;
        std::vector<float> clear(n, 1e30f);
        std::vector<int> q;
        for (size_t f = 0; f < n; f++)
            for (int d = 0; d < 8; d += 2)
                if (g.nb[f][(size_t)d] < 0) { clear[f] = 0; q.push_back((int)f); break; }
        for (size_t k = 0; k < q.size(); k++) {   // (breadth first: a grid step at a time)
            int c = q[k];
            if (clear[(size_t)c] >= kWant) continue;
            for (int d = 0; d < 8; d++) {
                int m = g.nb[(size_t)c][(size_t)d];
                float nd = clear[(size_t)c] + (float)(g.h * ((d & 1) ? 1.4142 : 1.0));
                if (m >= 0 && nd < clear[(size_t)m]) { clear[(size_t)m] = nd; q.push_back(m); }
            }
        }
        for (auto &o : out) {
            if (o.kind == 4) continue;   // (a doorway's: on its threshold, between its sides already)
            int ci = (int)std::floor((o.p[0] - g.x0) / g.h), cj = (int)std::floor((o.p[1] - g.y0) / g.h);
            if (ci < 0 || cj < 0 || ci >= g.nx || cj >= g.ny) continue;
            int c = cj * g.nx + ci, f0 = -1;
            for (int f = g.first[(size_t)c]; f < g.first[(size_t)c + 1]; f++)
                if (std::fabs(g.at[(size_t)f][2] - o.p[2]) < 0.6 && (f0 < 0 || std::fabs(g.at[(size_t)f][2] - o.p[2]) < std::fabs(g.at[(size_t)f0][2] - o.p[2]))) f0 = f;
            if (f0 < 0 || clear[(size_t)f0] >= kWant) continue;
            // (the floor within a unit's walk of it, on the same kind of floor: a ramp's stays on the ramp)
            int best = f0;
            std::map<int, double> seen{{f0, 0.0}};
            std::vector<int> fr{f0};
            for (size_t k = 0; k < fr.size(); k++) {
                int c2 = fr[k];
                double dc = seen[c2];
                for (int d = 0; d < 8; d++) {
                    int m = g.nb[(size_t)c2][(size_t)d];
                    if (m < 0 || !reach[(size_t)m] || seen.count(m)) continue;
                    double nd = dc + g.h * ((d & 1) ? 1.4142 : 1.0);
                    if (nd > kWant) continue;
                    if (o.kind == 1 && !slope((size_t)m)) continue;
                    seen[m] = nd;
                    fr.push_back(m);
                    if (clear[(size_t)m] > clear[(size_t)best] + 0.01) best = m;
                }
            }
            if (best != f0) o.p = g.at[(size_t)best];
        }
    }
    return out;
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

    // The waypoints (bots_route_add): on the ground, one for points within 1 unit, 512 at most.
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
    int room = std::min(o.max_add, std::max(0, o.budget - o.spare - res.given));

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
    // The level's ramps and narrow ways: waypoints along each, so the way along them is plain to the bots.
    if (o.features) {
        std::vector<V3> from;
        for (int i = 0; i < res.given; i++) from.push_back(w[(size_t)i].at);
        std::vector<V3> walk;
        if (o.walk_from.size() == points.size()) {
            for (size_t i = 0; i < points.size() && i < res.point_waypoint.size(); i++)
                if (o.walk_from[i] && res.point_waypoint[i] >= 0) walk.push_back(w[(size_t)res.point_waypoint[i]].at);
        } else walk = from;
        walk.insert(walk.end(), o.ladder_tops.begin(), o.ladder_tops.end());
        // (the bots' waypoints left, less some kept for finishing the links the search can't: doorways and ramps
        // first, then the rest shared, 60% to filling open floor and 40% to narrow ways, either one's unused share to
        // the other)
        const int kForLinks = 64;
        const int cap = std::min(o.max_features, std::max(0, o.budget - o.spare - (int)w.size() - kForLinks)) +
                        (int)res.added.size();
        std::vector<Feature> all = find_features(L, from, walk);
        auto place = [&](const Feature &f) {   // (false: not placed, no ground or a waypoint near)
            V3 gp;
            uint32_t sf = ground(L, {f.p[0], f.p[1], f.p[2] + 0.25}, gp);
            if (sf == kNone) return false;
            bool near = false;
            for (auto &x : w) near = near || dist(x.at, gp) < 1.6;
            for (auto &x : o.removed) near = near || dist(x, gp) < kRemovedRadius;
            if (near) return false;
            w.push_back({gp, sf});
            tele_to.push_back(-1);
            res.added.push_back({gp[0], gp[1], gp[2] + 0.25});
            res.added_kind.push_back(f.kind);
            (f.kind == 1 ? res.on_ramps : f.kind == 2 || f.kind == 4 ? res.in_narrow : res.on_open)++;
            return true;
        };
        auto room_left = [&] { return cap - (int)res.added.size(); };
        for (auto &f : all)   // doorways and ramps
            if ((f.kind == 4 || f.kind == 1) && room_left() > 0) place(f);
        const int rest = std::max(0, room_left());
        // (open floor up to its share; the narrow ways; then open floor again with what they left)
        int open_n = rest * 6 / 10;
        size_t open_at = 0;
        for (; open_at < all.size() && open_n > 0 && room_left() > 0; open_at++)
            if (all[open_at].kind == 3 && place(all[open_at])) open_n--;
        for (auto &f : all)
            if (f.kind == 2 && room_left() > 0) place(f);
        for (; open_at < all.size() && room_left() > 0; open_at++)
            if (all[open_at].kind == 3) place(all[open_at]);
        if (!res.added.empty()) {
            links = make_links(w, tele_to);
            evaluate(links, hops);
        }
    }
    room = std::min(o.max_add, std::max(0, o.budget - o.spare - (int)w.size()));
    long reached = reach(hops);
    const size_t feat_n = res.added.size();   // (the ones the search's hops get: room more)
    for (int round = 0; round < o.rounds && (int)(res.added.size() - feat_n) < room; round++) {
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
            if ((int)(res.added.size() - feat_n) >= room) break;
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
                    if (L.normal[sf][2] < 0.707) continue;   // (not on a slope steeper than 45 degrees: a rock face)
                    Way c{g, sf};
                    int ex = -1;
                    for (size_t i = 0; i < w.size() && ex < 0; i++)
                        if (dist(w[i].at, g) < 1.0) ex = (int)i;
                    if (ex >= 0) c = w[(size_t)ex];
                    else {   // (not where you took one out)
                        bool out = false;
                        for (auto &x : o.removed) out = out || dist(x, g) < kRemovedRadius;
                        if (out) continue;
                    }
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
                    if ((int)(res.added.size() - feat_n) >= room) { stuck = true; break; }
                    w.push_back(next);
                    tele_to.push_back(-1);
                    res.added.push_back({next.at[0], next.at[1], next.at[2] + 0.25});
                    res.added_kind.push_back(0);
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
            res.added_kind.resize(added0.size());
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

std::vector<Ladder> find_ladders(const fs::path &bsp) {
    std::vector<Ladder> out;
    tagfile::Tag t;
    if (!t.load(bsp)) return out;
    tagfile::Block cbs = t.root().block("collision bsp");
    coll::Bsp b;
    if (cbs.size() == 0 || !coll::read_bsp(cbs[0], b)) return out;
    std::vector<coll::Poly> polys = coll::polygons(b);
    std::vector<size_t> cl;
    for (size_t i = 0; i < polys.size(); i++)
        if ((polys[i].flags & coll::kClimbable) && polys[i].pts.size() >= 3) cl.push_back(i);
    // (one ladder: climbable surfaces sharing a corner; its front and back are two of them)
    std::vector<int> group(cl.size(), -1);
    int groups = 0;
    auto touch = [&](size_t a, size_t c) {
        for (auto &p : polys[cl[a]].pts)
            for (auto &q : polys[cl[c]].pts)
                if (std::fabs(p[0] - q[0]) + std::fabs(p[1] - q[1]) + std::fabs(p[2] - q[2]) < 0.05) return true;
        return false;
    };
    for (size_t i = 0; i < cl.size(); i++) {
        if (group[i] >= 0) continue;
        std::vector<size_t> st{i};
        group[i] = groups;
        while (!st.empty()) {
            size_t a = st.back();
            st.pop_back();
            for (size_t j = 0; j < cl.size(); j++)
                if (group[j] < 0 && touch(a, j)) { group[j] = groups; st.push_back(j); }
        }
        groups++;
    }
    // The ground under a point: from `up` above it, down to `down` below it (front faces).
    auto floor_at = [&](double x, double y, double z, double up, double down, double &gz) {
        double o[3] = {x, y, z + up}, v[3] = {0, 0, -(up + down)};
        coll::Hit h = coll::test_vector(b, o, v, 1, false);
        if (h.surface == kNone) return false;
        gz = o[2] + v[2] * h.t;
        return true;
    };
    for (int k = 0; k < groups; k++) {
        double lo[3] = {1e30, 1e30, 1e30}, hi[3] = {-1e30, -1e30, -1e30}, n[2] = {0, 0};
        for (size_t i = 0; i < cl.size(); i++) {
            if (group[i] != k) continue;
            const auto &q = polys[cl[i]].pts;
            for (auto &p : q)
                for (int a = 0; a < 3; a++) { lo[a] = std::min(lo[a], p[a]); hi[a] = std::max(hi[a], p[a]); }
            for (size_t m = 0; m < q.size(); m++) {   // (its normal's sideways part, area-weighted)
                auto &a = q[m], &z = q[(m + 1) % q.size()];
                n[0] += (a[1] - z[1]) * (a[2] + z[2]);
                n[1] += (a[2] - z[2]) * (a[0] + z[0]);
            }
        }
        if (hi[2] - lo[2] < 1.0) continue;   // (not something to climb)
        double l = std::hypot(n[0], n[1]);
        if (l > 1e-6 && l > 0.05 * std::hypot(hi[0] - lo[0], hi[1] - lo[1]) * (hi[2] - lo[2])) { n[0] /= l; n[1] /= l; }
        else if (hi[0] - lo[0] < hi[1] - lo[1]) { n[0] = 1; n[1] = 0; }   // (front and back cancel: across its thin side)
        else { n[0] = 0; n[1] = 1; }
        const double cx = (lo[0] + hi[0]) / 2, cy = (lo[1] + hi[1]) / 2;
        // Its front: the side with floor at its foot.
        for (int side : {1, -1}) {
            double fx = n[0] * side, fy = n[1] * side, gz;
            Ladder L;
            if (!floor_at(cx + fx * 0.75, cy + fy * 0.75, lo[2], 0.6, 0.8, gz)) continue;
            L.foot = {cx + fx * 0.75, cy + fy * 0.75, gz};
            // Its top end: behind the top edge, else out in front, else beside it; on a floor at least a unit above
            // the foot and no higher than the ladder's top.
            bool found = false;
            auto try_at = [&](double x, double y) {
                double tz;
                if (found || !floor_at(x, y, hi[2], 0.3, 1.8, tz) || tz < gz + 1.0 || tz > hi[2] + 0.3) return;
                L.top = {x, y, tz};
                found = true;
            };
            for (double d = 0.75; d <= 1.25 && !found; d += 0.25) try_at(cx - fx * d, cy - fy * d);
            for (double d : {1.6, 1.4, 1.8, 2.0, 2.4}) try_at(cx + fx * d, cy + fy * d);
            for (double d = 0.75; d <= 1.5 && !found; d += 0.25)
                for (int s2 : {1, -1}) try_at(cx - fy * d * s2, cy + fx * d * s2);
            if (!found) continue;
            L.dx = fx == 0 ? 0.0 : -fx;   // (no "-0")
            L.dy = fy == 0 ? 0.0 : -fy;
            out.push_back(L);
            break;
        }
    }
    return out;
}

std::vector<std::pair<int, int>> stacked_hops(const Result &r) {
    std::vector<std::pair<int, int>> out;
    std::set<std::pair<int, int>> seen;
    for (auto &h : r.hops) {
        if (h.ok || h.joined || h.a < 0 || h.b < 0 || h.a >= (int)r.waypoints.size() || h.b >= (int)r.waypoints.size()) continue;
        const V3 &a = r.waypoints[(size_t)h.a], &c = r.waypoints[(size_t)h.b];
        if (dist2d(a, c) >= 2.0 || std::fabs(a[2] - c[2]) < 1.0) continue;
        std::pair<int, int> p = a[2] < c[2] ? std::make_pair(h.a, h.b) : std::make_pair(h.b, h.a);
        if (seen.insert(p).second) out.push_back(p);
    }
    return out;
}

std::string bot_file(const std::string &map, const std::vector<V3> &added, const std::vector<V3> &mine,
                     const std::vector<Ladder> &ladders, std::string *note) {
    auto f2 = [](double v) {
        char b[32];
        snprintf(b, sizeof b, "%.2f", std::fabs(v) < 0.005 ? 0.0 : v);
        return std::string(b);
    };
    auto p3 = [&](const V3 &p) { return f2(p[0]) + " " + f2(p[1]) + " " + f2(p[2]); };
    const size_t n = std::min<size_t>(ladders.size(), 16);
    std::string s = "# Halo 1 Unlimited bot waypoints for " + map + ", made by Halo Map Porter.\n"
                    "# The game reads it from maps\\bots\\" + map + ".txt (a stock map) or bots.txt in a folder map's folder.\n"
                    "# " + std::to_string(added.size()) + " waypoints the porter adds, " + std::to_string(mine.size()) +
                    " placed by hand, " + std::to_string(n) + " ladder" + (n == 1 ? "" : "s") + ".\n";
    if (!added.empty()) s += "\n# Waypoints the porter adds (ramps, narrow ways, open floor, where the search needs them)\n";
    for (auto &p : added) s += "node " + p3(p) + "\n";
    if (!mine.empty()) s += "\n# Waypoints placed by hand in the Level window\n";
    for (auto &p : mine) s += "node " + p3(p) + "\n";
    if (n) s += "\n# Ladders: foot and top nodes, then the climb (and the way down)\n";
    for (size_t i = 0; i < n; i++) s += "node " + p3(ladders[i].foot) + "\nnode " + p3(ladders[i].top) + "\n";
    for (size_t i = 0; i < n; i++) {
        const Ladder &L = ladders[i];
        s += "ladder " + p3(L.foot) + " " + p3(L.top) + " " + f2(L.dx) + " " + f2(L.dy) + "\n";
        if (L.down) s += "link " + p3(L.top) + " " + p3(L.foot) + " c\n";
    }
    if (note) {
        note->clear();
        if (ladders.size() > 16) *note = std::to_string(ladders.size() - 16) + " ladders past the game's 16 left out";
        if (s.size() > 65536) *note += std::string(note->empty() ? "" : "; ") + "the file is over the game's 64 KB";
    }
    return s;
}

bool picture(const fs::path &bsp, const Result &r, const fs::path &file, const std::vector<Ladder> *ladders) {
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
    if (ladders)
        for (auto &L : *ladders) {
            line(L.foot, L.top, {60, 220, 90});
            plot(L.foot[0], L.foot[1], {60, 220, 90}, 3);
            plot(L.top[0], L.top[1], {150, 255, 160}, 3);
        }
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
