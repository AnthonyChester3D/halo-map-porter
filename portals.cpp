// Halo Map Porter - portals for a level that has few or none (see portals.hpp)
#include "portals.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <functional>
#include <map>
#include <tuple>
#include <unordered_map>

#include "tagfile.hpp"

namespace portals {

namespace fs = std::filesystem;

namespace {

V3 operator+(V3 a, V3 b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 operator-(V3 a, V3 b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 operator*(V3 a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(V3 a, V3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 cross(V3 a, V3 b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double len(V3 a) { return std::sqrt(dot(a, a)); }

const double kEps = 0.0005;   // world units (a unit is 10 feet)

struct Plane { V3 n; double d; };   // n.p - d: positive in front

using Poly = std::vector<V3>;

// The part of `w` in front of the plane (keep_front) or behind it. Empty: none.
Poly clip(const Poly &w, const Plane &p, bool keep_front) {
    if (w.size() < 3) return {};
    std::vector<double> s(w.size());
    bool all_in = true, all_out = true;
    for (size_t i = 0; i < w.size(); i++) {
        s[i] = (dot(p.n, w[i]) - p.d) * (keep_front ? 1 : -1);
        if (s[i] < -kEps) all_in = false;
        if (s[i] > kEps) all_out = false;
    }
    if (all_in) return w;
    if (all_out) return {};
    Poly out;
    for (size_t i = 0; i < w.size(); i++) {
        size_t j = (i + 1) % w.size();
        if (s[i] >= -kEps) out.push_back(w[i]);
        if ((s[i] > kEps && s[j] < -kEps) || (s[i] < -kEps && s[j] > kEps)) {
            double t = s[i] / (s[i] - s[j]);
            out.push_back(w[i] + (w[j] - w[i]) * t);
        }
    }
    if (out.size() < 3) return {};
    return out;
}

double area(const Poly &w) {
    V3 a{0, 0, 0};
    for (size_t i = 1; i + 1 < w.size(); i++) a = a + cross(w[i] - w[0], w[i + 1] - w[0]);
    return 0.5 * len(a);
}

// A square on the plane, `size` from its middle, wound counterclockwise seen from the front.
Poly base_winding(const Plane &p, double size) {
    int ax = 0;
    for (int k = 1; k < 3; k++)
        if (std::fabs(p.n[k]) > std::fabs(p.n[ax])) ax = k;
    V3 up = ax == 2 ? V3{1, 0, 0} : V3{0, 0, 1};
    V3 v = up - p.n * dot(up, p.n);
    v = v * (1.0 / len(v));
    V3 u = cross(v, p.n);
    V3 o = p.n * p.d;
    return {o - u * size - v * size, o + u * size - v * size, o + u * size + v * size, o - u * size + v * size};
}

// Tree node ids: an internal node's index (>= 0), a leaf (kLeaf - index), solid, or the outside of the world box.
const int kSolid = -1, kOutside = -2, kLeaf = -3;
int leaf_id(int k) { return kLeaf - k; }
bool is_leaf(int id) { return id <= kLeaf; }
int leaf_of(int id) { return kLeaf - id; }

struct TreePortal {
    Poly w;
    Plane plane;
    int plane_index = -1;
    int nodes[2] = {0, 0};   // the node in front of the plane, behind it
    bool alive = true;
};

// The collision BSP: its planes and tree (enough to find the leaf a point is in, and the leaves' portals).
struct Bsp {
    std::vector<Plane> planes;
    struct Node { int plane, front, back; };
    std::vector<Node> nodes;
    int leaves = 0;
    V3 lo{}, hi{};   // world bounds
    // The leaf a point is in (-1: solid).
    int leaf_at(const V3 &p) const {
        int id = 0;
        for (size_t steps = 0; id >= 0 && steps <= nodes.size(); steps++) {
            const Node &n = nodes[(size_t)id];
            const Plane &pl = planes[(size_t)n.plane];
            id = dot(pl.n, p) - pl.d >= 0 ? n.front : n.back;
        }
        return is_leaf(id) ? leaf_of(id) : -1;   // (a loop in a broken tree: solid)
    }
};

bool load_bsp(const tagfile::Elem &r, Bsp &b, std::string *why) {
    auto fail = [&](const char *w) { if (why) *why = w; return false; };
    tagfile::Block cbs = r.block("collision bsp");
    if (cbs.size() == 0) return fail("has no collision BSP");
    tagfile::Elem cb = cbs[0];
    tagfile::Block bn = cb.block("bsp3d nodes"), bp = cb.block("planes"), bl = cb.block("leaves");
    if (bn.size() == 0 || bl.size() == 0) return fail("has no BSP tree");
    if (r.block("leaves").size() != bl.size()) return fail("has a leaf list that doesn't match its BSP tree");
    b.planes.resize(bp.size());
    for (size_t i = 0; i < bp.size(); i++) {
        V3 n{bp[i].num("plane", 0), bp[i].num("plane", 1), bp[i].num("plane", 2)};
        double l = len(n), d = bp[i].num("plane", 3);
        if (l > 0) { n = n * (1 / l); d /= l; }
        b.planes[i] = {n, d};
    }
    b.nodes.resize(bn.size());
    b.leaves = (int)bl.size();
    auto child = [&](uint32_t c) -> int {
        if (c == 0xFFFFFFFFu) return kSolid;
        if (c & 0x80000000u) return leaf_id((int)(c & 0x7FFFFFFFu));
        return (int)c;
    };
    for (size_t i = 0; i < bn.size(); i++) {
        uint32_t pl = (uint32_t)bn[i].integer("plane") & 0x7FFFFFFFu;
        if (pl >= b.planes.size()) return fail("has a BSP node with no plane");
        b.nodes[i] = {(int)pl, child((uint32_t)bn[i].integer("front child")), child((uint32_t)bn[i].integer("back child"))};
        for (int c : {b.nodes[i].front, b.nodes[i].back})
            if ((c >= 0 && c >= (int)b.nodes.size()) || (is_leaf(c) && leaf_of(c) >= b.leaves))
                return fail("has a broken BSP tree");
    }
    // A tree: every node has one parent (the root none), so following children always ends.
    std::vector<int> parents(b.nodes.size(), 0);
    for (auto &n : b.nodes)
        for (int c : {n.front, n.back})
            if (c >= 0) parents[(size_t)c]++;
    if (parents[0] != 0) return fail("has a broken BSP tree");
    for (int k : parents)
        if (k > 1) return fail("has a broken BSP tree");
    b.lo = {r.num("world bounds x", 0), r.num("world bounds y", 0), r.num("world bounds z", 0)};
    b.hi = {r.num("world bounds x", 1), r.num("world bounds y", 1), r.num("world bounds z", 1)};
    return true;
}

// The portals between the tree's leaves, and between leaves and solid (walls).
void portalize(const Bsp &b, std::vector<LeafPortal> &result) {
    const std::vector<Plane> &planes = b.planes;
    const std::vector<Bsp::Node> &nodes = b.nodes;
    // Portals between the tree's nodes (Quake's qbsp way): the world box's six sides first, between the root and
    // the outside; then at each node, its plane cut down to the node's space (by the portals around it) is a new
    // portal between its two children, and the portals around it are split between its children.
    std::vector<TreePortal> ps;
    std::vector<std::vector<int>> on;   // portals touching each node
    auto slot = [&](int id) -> std::vector<int> & {
        if (id >= 0) return on[(size_t)id];
        if (id == kSolid) return on[nodes.size()];
        if (id == kOutside) return on[nodes.size() + 1];
        return on[nodes.size() + 2 + (size_t)leaf_of(id)];
    };
    on.resize(nodes.size() + 2 + (size_t)b.leaves);
    auto add = [&](TreePortal p) {
        int k = (int)ps.size();
        ps.push_back(std::move(p));
        slot(ps[k].nodes[0]).push_back(k);
        slot(ps[k].nodes[1]).push_back(k);
    };
    auto unlink = [&](int k, int id) {
        auto &v = slot(id);
        v.erase(std::remove(v.begin(), v.end(), k), v.end());
    };
    V3 lo = b.lo - V3{16, 16, 16}, hi = b.hi + V3{16, 16, 16};
    double size = 4 * std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], 100.0});
    for (int ax = 0; ax < 3; ax++)
        for (int side = 0; side < 2; side++) {
            // The box's side: its plane faces out (the world is behind it).
            Plane p;
            p.n = {0, 0, 0};
            p.n[ax] = side == 0 ? 1 : -1;
            p.d = side == 0 ? hi[ax] : -lo[ax];
            Poly w = base_winding(p, size);
            for (int ax2 = 0; ax2 < 3; ax2++)
                for (int s2 = 0; s2 < 2 && ax2 != ax; s2++) {
                    Plane q;
                    q.n = {0, 0, 0};
                    q.n[ax2] = s2 == 0 ? 1 : -1;
                    q.d = s2 == 0 ? hi[ax2] : -lo[ax2];
                    w = clip(w, q, false);
                }
            TreePortal tp;
            tp.w = w;
            tp.plane = p;
            tp.nodes[0] = kOutside;
            tp.nodes[1] = 0;
            add(tp);
        }
    // Each node's parent and which side of it the node is on, for its space: the box, cut by its ancestors' planes.
    std::vector<int> parent(nodes.size(), -1);
    std::vector<char> in_front(nodes.size(), 0);
    for (size_t i = 0; i < nodes.size(); i++)
        for (int side = 0; side < 2; side++) {
            int c = side == 0 ? nodes[i].front : nodes[i].back;
            if (c >= 0) { parent[(size_t)c] = (int)i; in_front[(size_t)c] = side == 0; }
        }
    std::vector<Plane> box;
    for (int ax = 0; ax < 3; ax++)
        for (int side = 0; side < 2; side++) {
            Plane q;
            q.n = {0, 0, 0};
            q.n[ax] = side == 0 ? 1 : -1;
            q.d = side == 0 ? hi[ax] : -lo[ax];
            box.push_back(q);
        }
    // Depth first, without recursion (a BSP can be deep).
    std::vector<int> todo{0};
    while (!todo.empty()) {
        int i = todo.back();
        todo.pop_back();
        const Bsp::Node &nd = nodes[(size_t)i];
        const Plane &pl = planes[(size_t)nd.plane];
        // The node's own portal: its plane, within its space.
        Poly w = base_winding(pl, size);
        for (const Plane &q : box) w = clip(w, q, false);
        for (int c = i; parent[(size_t)c] >= 0 && !w.empty(); c = parent[(size_t)c])
            w = clip(w, planes[(size_t)nodes[(size_t)parent[(size_t)c]].plane], in_front[(size_t)c] != 0);
        // The portals around it, split between its children.
        std::vector<int> around = slot(i);
        for (int k : around) {
            int side = ps[(size_t)k].nodes[0] == i ? 0 : 1;
            int other = ps[(size_t)k].nodes[1 - side];
            unlink(k, i);
            unlink(k, other);
            ps[(size_t)k].alive = false;
            Poly f, b;
            bool on_plane = true;
            for (const V3 &x : ps[(size_t)k].w) on_plane = on_plane && std::fabs(dot(pl.n, x) - pl.d) <= kEps;
            if (on_plane) {   // on this node's plane: it goes to the child on the node's side of it
                bool front = (side == 0) == (dot(ps[(size_t)k].plane.n, pl.n) > 0);
                (front ? f : b) = ps[(size_t)k].w;
            } else {
                f = clip(ps[(size_t)k].w, pl, true);
                b = clip(ps[(size_t)k].w, pl, false);
            }
            for (int pass = 0; pass < 2; pass++) {
                Poly &part = pass == 0 ? f : b;
                if (part.empty()) continue;
                TreePortal q;
                q.w = std::move(part);
                q.plane = ps[(size_t)k].plane;
                q.plane_index = ps[(size_t)k].plane_index;
                q.nodes[side] = pass == 0 ? nd.front : nd.back;
                q.nodes[1 - side] = other;
                add(q);
            }
        }
        slot(i).clear();
        if (!w.empty() && area(w) > 1e-6) {
            TreePortal q;
            q.w = std::move(w);
            q.plane = pl;
            q.plane_index = nd.plane;
            q.nodes[0] = nd.front;
            q.nodes[1] = nd.back;
            add(q);
        }
        if (nd.back >= 0) todo.push_back(nd.back);
        if (nd.front >= 0) todo.push_back(nd.front);
    }
    // What's left: portals between leaves (and solid, and the outside).
    for (auto &p : ps) {
        if (!p.alive || p.w.size() < 3 || p.plane_index < 0) continue;
        bool fl = is_leaf(p.nodes[0]), bl2 = is_leaf(p.nodes[1]);
        if (!fl && !bl2) continue;
        if ((!fl && p.nodes[0] != kSolid) || (!bl2 && p.nodes[1] != kSolid)) continue;   // (the outside)
        if (area(p.w) < 1e-6) continue;
        LeafPortal lp;
        lp.front = fl ? leaf_of(p.nodes[0]) : -1;
        lp.back = bl2 ? leaf_of(p.nodes[1]) : -1;
        lp.plane = p.plane_index;
        lp.points = p.w;
        result.push_back(std::move(lp));
    }
}

bool load_triangles(const tagfile::Elem &r, std::vector<std::array<V3, 3>> &tri);
void hidden_openings(const Bsp &b, const tagfile::Elem &r, const SeeThrough &see_through, std::vector<LeafPortal> &portals);

}   // namespace

bool leaf_portals(const fs::path &bsp_file, Leaves &out, std::string *why, const SeeThrough &see_through) {
    tagfile::Tag t;
    if (!t.load(bsp_file)) {
        if (why) *why = "couldn't be read";
        return false;
    }
    Bsp b;
    if (!load_bsp(t.root(), b, why)) return false;
    out = Leaves();
    out.count = b.leaves;
    tagfile::Block sl = t.root().block("leaves");
    out.cluster.resize((size_t)out.count);
    for (int i = 0; i < out.count; i++) out.cluster[(size_t)i] = (int)sl[(size_t)i].integer("cluster");
    out.lo = b.lo;
    out.hi = b.hi;
    portalize(b, out.portals);
    hidden_openings(b, t.root(), see_through, out.portals);
    return true;
}

namespace {

// Squared distance transform along one line (Felzenszwalb and Huttenlocher): d[i] = min over j of f[j] + (i - j)^2.
void edt_line(const float *f, int n, float *d, std::vector<int> &v, std::vector<float> &z) {
    v.resize((size_t)n);
    z.resize((size_t)n + 1);
    int k = 0;
    v[0] = 0;
    z[0] = -1e30f;
    z[1] = 1e30f;
    for (int q = 1; q < n; q++) {
        if (f[q] >= 1e29f) continue;   // (no wall here: skipped)
        if (f[v[(size_t)k]] >= 1e29f) { v[(size_t)k] = q; continue; }
        float s;
        while (true) {
            int p = v[(size_t)k];
            s = ((f[q] + (float)q * q) - (f[p] + (float)p * p)) / (2.0f * (q - p));
            if (s <= z[(size_t)k] && k > 0) k--;
            else break;
        }
        k++;
        v[(size_t)k] = q;
        z[(size_t)k] = s;
        z[(size_t)k + 1] = 1e30f;
    }
    if (f[v[0]] >= 1e29f) {   // no wall on the line at all
        for (int q = 0; q < n; q++) d[q] = f[q];
        return;
    }
    k = 0;
    for (int q = 0; q < n; q++) {
        while (z[(size_t)k + 1] < (float)q) k++;
        int p = v[(size_t)k];
        d[q] = (float)(q - p) * (q - p) + f[p];
    }
}

// Maximum flow (Dinic), for the smallest opening between two rooms.
struct Flow {
    struct E { int to; double cap; };
    std::vector<E> e;
    std::vector<std::vector<int>> g;
    std::vector<int> level, it;
    explicit Flow(int n) : g((size_t)n), level((size_t)n), it((size_t)n) {}
    void add(int a, int b, double c) {   // both ways (an opening is passable both ways)
        g[(size_t)a].push_back((int)e.size());
        e.push_back({b, c});
        g[(size_t)b].push_back((int)e.size());
        e.push_back({a, c});
    }
    bool bfs(int s, int t) {
        std::fill(level.begin(), level.end(), -1);
        std::vector<int> q{s};
        level[(size_t)s] = 0;
        for (size_t i = 0; i < q.size(); i++)
            for (int k : g[(size_t)q[i]])
                if (e[(size_t)k].cap > 1e-12 && level[(size_t)e[(size_t)k].to] < 0) {
                    level[(size_t)e[(size_t)k].to] = level[(size_t)q[i]] + 1;
                    q.push_back(e[(size_t)k].to);
                }
        return level[(size_t)t] >= 0;
    }
    double dfs(int v, int t, double f) {
        if (v == t) return f;
        for (int &i = it[(size_t)v]; i < (int)g[(size_t)v].size(); i++) {
            E &x = e[(size_t)g[(size_t)v][(size_t)i]];
            if (x.cap <= 1e-12 || level[(size_t)x.to] != level[(size_t)v] + 1) continue;
            double d = dfs(x.to, t, std::min(f, x.cap));
            if (d > 0) {
                x.cap -= d;
                e[(size_t)g[(size_t)v][(size_t)i] ^ 1].cap += d;
                return d;
            }
        }
        return 0;
    }
    void run(int s, int t) {
        for (int round = 0; round < 1000 && bfs(s, t); round++) {
            std::fill(it.begin(), it.end(), 0);
            while (dfs(s, t, 1e30) > 0) {}
        }
    }
    std::vector<char> source_side(int s) {   // after run(): what's still reachable from s
        std::vector<char> in(g.size(), 0);
        std::vector<int> q{s};
        in[(size_t)s] = 1;
        for (size_t i = 0; i < q.size(); i++)
            for (int k : g[(size_t)q[i]])
                if (e[(size_t)k].cap > 1e-12 && !in[(size_t)e[(size_t)k].to]) {
                    in[(size_t)e[(size_t)k].to] = 1;
                    q.push_back(e[(size_t)k].to);
                }
        return in;
    }
};

struct UnionFind {
    std::vector<int> up;
    int add() { up.push_back((int)up.size()); return (int)up.size() - 1; }
    int find(int a) {
        while (up[(size_t)a] != a) a = up[(size_t)a] = up[(size_t)up[(size_t)a]];
        return a;
    }
};

}   // namespace

namespace {

// A plane's 2D basis: u, v with u x v = n (so counterclockwise in (u, v) is counterclockwise around n).
void basis(const V3 &n, V3 &u, V3 &v) {
    int ax = 0;
    for (int k = 1; k < 3; k++)
        if (std::fabs(n[k]) > std::fabs(n[ax])) ax = k;
    V3 up = ax == 2 ? V3{1, 0, 0} : V3{0, 0, 1};
    v = up - n * dot(up, n);
    v = v * (1.0 / len(v));
    u = cross(v, n);
}

// The convex hull of points on a plane (counterclockwise around its normal).
Poly hull_on_plane(const std::vector<V3> &pts, const Plane &pl) {
    V3 u, v;
    basis(pl.n, u, v);
    V3 o = pl.n * pl.d;
    std::vector<std::pair<double, double>> q;
    for (auto &p : pts) q.push_back({dot(p - o, u), dot(p - o, v)});
    std::sort(q.begin(), q.end());
    q.erase(std::unique(q.begin(), q.end(), [](auto &a, auto &b) {
                return std::fabs(a.first - b.first) < 1e-7 && std::fabs(a.second - b.second) < 1e-7;
            }), q.end());
    if (q.size() < 3) return {};
    auto turn = [](std::pair<double, double> a, std::pair<double, double> b, std::pair<double, double> c) {
        return (b.first - a.first) * (c.second - a.second) - (b.second - a.second) * (c.first - a.first);
    };
    std::vector<std::pair<double, double>> h(2 * q.size());
    size_t k = 0;
    for (size_t i = 0; i < q.size(); i++) {
        while (k >= 2 && turn(h[k - 2], h[k - 1], q[i]) <= 1e-12) k--;
        h[k++] = q[i];
    }
    for (size_t i = q.size() - 1, t = k + 1; i-- > 0;) {
        while (k >= t && turn(h[k - 2], h[k - 1], q[i]) <= 1e-12) k--;
        h[k++] = q[i];
    }
    h.resize(k - 1);
    Poly out;
    for (auto &[x, y] : h) out.push_back(o + u * x + v * y);
    return out.size() >= 3 ? out : Poly();
}

}   // namespace

void cluster_portals(const Leaves &L, const std::vector<int> &cl, std::vector<ClusterPortal> &out, const std::vector<V3> *planes_n,
                     const std::vector<double> *planes_d) {
    out.clear();
    // The leaf portals between clusters, by cluster pair, plane and side: Halo's portal has the "front" cluster
    // behind its plane.
    std::map<std::tuple<int, int, int>, std::vector<const LeafPortal *>> groups;
    for (auto &p : L.portals) {
        if (p.front < 0 || p.back < 0) continue;
        int f = cl[(size_t)p.front], b = cl[(size_t)p.back];
        if (f == b || f < 0 || b < 0) continue;
        groups[{b, f, p.plane}].push_back(&p);   // (back leaf's cluster is behind the plane)
    }
    for (auto &[key, list] : groups) {
        auto [front, back, plane] = key;
        // Coplanar pieces merge while their hull is about their own area (not across walls between them).
        std::vector<Poly> polys;
        std::vector<double> areas;
        for (auto *p : list) { polys.push_back(p->points); areas.push_back(area(p->points)); }
        Plane pl;
        V3 nrm = cross(polys[0][1] - polys[0][0], polys[0][2] - polys[0][0]);
        pl.n = nrm * (1 / len(nrm));
        pl.d = dot(pl.n, polys[0][0]);
        if (planes_n && planes_d && plane >= 0 && plane < (int)planes_n->size()) { pl.n = (*planes_n)[(size_t)plane]; pl.d = (*planes_d)[(size_t)plane]; }
        for (bool merged = true; merged && polys.size() > 1;) {
            merged = false;
            for (size_t i = 0; i < polys.size() && !merged; i++)
                for (size_t j = i + 1; j < polys.size() && !merged; j++) {
                    std::vector<V3> pts = polys[i];
                    pts.insert(pts.end(), polys[j].begin(), polys[j].end());
                    Poly h = hull_on_plane(pts, pl);
                    if (h.empty()) continue;
                    double ha = area(h);
                    if (ha > (areas[i] + areas[j]) * 1.15 + 1e-4) continue;
                    polys[i] = h;
                    areas[i] = ha;
                    polys.erase(polys.begin() + (long)j);
                    areas.erase(areas.begin() + (long)j);
                    merged = true;
                }
        }
        for (size_t i = 0; i < polys.size(); i++) {
            if (areas[i] < 1e-6) continue;
            ClusterPortal c;
            c.front = front;
            c.back = back;
            c.plane = plane;
            c.points = polys[i];
            if (c.points.size() > 128) c.points = hull_on_plane(c.points, pl);   // (at most 128 corners)
            out.push_back(std::move(c));
        }
    }
}

bool plan(const fs::path &bsp_file, const Options &o, Leaves &L, std::vector<int> &cluster_of_leaf, std::string *report) {
    tagfile::Tag t;
    if (!t.load(bsp_file)) {
        if (report) *report = "couldn't be read";
        return false;
    }
    Bsp b;
    if (!load_bsp(t.root(), b, report)) return false;
    L = Leaves();
    L.count = b.leaves;
    tagfile::Block sl = t.root().block("leaves");
    L.cluster.resize((size_t)L.count);
    for (int i = 0; i < L.count; i++) L.cluster[(size_t)i] = (int)sl[(size_t)i].integer("cluster");
    L.lo = b.lo;
    L.hi = b.hi;
    portalize(b, L.portals);

    // 1. The open space on a grid: the box around the leaves' portals (open space and its walls).
    V3 lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
    for (auto &p : L.portals)
        for (auto &x : p.points)
            for (int k = 0; k < 3; k++) { lo[k] = std::min(lo[k], x[k]); hi[k] = std::max(hi[k], x[k]); }
    for (int k = 0; k < 3; k++) { lo[k] = std::max(lo[k], b.lo[k]); hi[k] = std::min(hi[k], b.hi[k]); }
    if (!(lo[0] < hi[0] && lo[1] < hi[1] && lo[2] < hi[2])) {
        if (report) *report = "has no open space";
        return false;
    }
    double volume = (hi[0] - lo[0]) * (hi[1] - lo[1]) * (hi[2] - lo[2]);
    double h = std::max(o.min_cell, std::cbrt(volume / o.max_cells));
    if (h > o.max_cell && o.area.set) {
        // Too big for a fine enough grid: only where players play (a backdrop's open space far around it needs no
        // rooms; its leaves go with the nearest room).
        for (size_t k = 0; k < 3; k++) { lo[k] = std::max(lo[k], o.area.lo[k] - 5); hi[k] = std::min(hi[k], o.area.hi[k] + 5); }
        if (!(lo[0] < hi[0] && lo[1] < hi[1] && lo[2] < hi[2])) {
            if (report) *report = "has no open space where players play";
            return false;
        }
        volume = (hi[0] - lo[0]) * (hi[1] - lo[1]) * (hi[2] - lo[2]);
        h = std::max(o.min_cell, std::cbrt(volume / o.max_cells));
    }
    if (h > o.max_cell) {   // (a doorway would be a cell or two)
        if (report) *report = "is too big for its rooms to be found";
        return false;
    }
    hidden_openings(b, t.root(), o.see_through, L.portals);
    int n[3];
    for (int k = 0; k < 3; k++) n[k] = std::max(1, (int)std::ceil((hi[k] - lo[k]) / h));
    size_t cells = (size_t)n[0] * n[1] * n[2];
    auto at = [&](int x, int y, int z) { return ((size_t)z * n[1] + y) * n[0] + x; };
    std::vector<int> leaf(cells);
    for (int z = 0; z < n[2]; z++)
        for (int y = 0; y < n[1]; y++)
            for (int x = 0; x < n[0]; x++)
                leaf[at(x, y, z)] = b.leaf_at({lo[0] + (x + 0.5) * h, lo[1] + (y + 0.5) * h, lo[2] + (z + 0.5) * h});

    // 2. How far each open cell is from the nearest wall (cells; beyond the grid counts as wall).
    std::vector<float> dist(cells);
    for (size_t i = 0; i < cells; i++) dist[i] = leaf[i] < 0 ? 0.0f : 1e30f;
    {
        int m = std::max({n[0], n[1], n[2]}) + 2;
        std::vector<float> f((size_t)m), d((size_t)m), z;
        std::vector<int> v;
        for (int axis = 0; axis < 3; axis++) {
            int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3, len_ = n[axis];
            for (int i = 0; i < n[a1]; i++)
                for (int j = 0; j < n[a2]; j++) {
                    auto idx = [&](int q) {
                        int c[3];
                        c[axis] = q;
                        c[a1] = i;
                        c[a2] = j;
                        return at(c[0], c[1], c[2]);
                    };
                    // with a wall just outside each end
                    f[0] = 0;
                    for (int q = 0; q < len_; q++) f[(size_t)q + 1] = dist[idx(q)];
                    f[(size_t)len_ + 1] = 0;
                    edt_line(f.data(), len_ + 2, d.data(), v, z);
                    for (int q = 0; q < len_; q++) dist[idx(q)] = d[(size_t)q + 1];
                }
        }
    }
    for (auto &x : dist) x = std::sqrt(x);

    // 3. Rooms: basins of that distance, around its peaks (the widest spots), flooded from the widest down. Where
    // two basins meet, they're one room unless the opening is at most `ratio` of the narrower one's peak.
    std::vector<int> order;
    order.reserve(cells);
    for (size_t i = 0; i < cells; i++)
        if (leaf[i] >= 0) order.push_back((int)i);
    std::sort(order.begin(), order.end(), [&](int a, int c) { return dist[(size_t)a] > dist[(size_t)c]; });
    std::vector<int> label(cells, -1);
    UnionFind uf;
    std::vector<float> peak;
    for (int c : order) {
        int z = c / (n[0] * n[1]), y = (c / n[0]) % n[1], x = c % n[0];
        int roots[6], nr = 0;
        auto look = [&](int xx, int yy, int zz) {
            if (xx < 0 || yy < 0 || zz < 0 || xx >= n[0] || yy >= n[1] || zz >= n[2]) return;
            int l = label[at(xx, yy, zz)];
            if (l < 0) return;
            l = uf.find(l);
            for (int k = 0; k < nr; k++)
                if (roots[k] == l) return;
            roots[nr++] = l;
        };
        look(x - 1, y, z); look(x + 1, y, z); look(x, y - 1, z); look(x, y + 1, z); look(x, y, z - 1); look(x, y, z + 1);
        if (nr == 0) {
            label[(size_t)c] = uf.add();
            peak.push_back(dist[(size_t)c]);
            continue;
        }
        int best = roots[0];
        for (int k = 1; k < nr; k++)
            if (peak[(size_t)roots[k]] > peak[(size_t)best]) best = roots[k];
        label[(size_t)c] = best;
        float s = dist[(size_t)c];
        for (int k = 0; k < nr; k++) {
            int a = uf.find(best), q = uf.find(roots[k]);
            if (a == q) continue;
            float ref = o.use_max ? std::max(peak[(size_t)a], peak[(size_t)q]) : std::min(peak[(size_t)a], peak[(size_t)q]);
            if (s > o.ratio * ref) {   // not a narrow opening: one room
                uf.up[(size_t)q] = a;
                peak[(size_t)a] = std::max(peak[(size_t)a], peak[(size_t)q]);
            }
        }
    }

    // 4. Each leaf goes to the room most of its cells are in; leaves too small for a cell, to the neighbour they
    // share the most opening with.
    std::vector<std::vector<std::pair<int, int>>> votes((size_t)L.count);
    for (size_t i = 0; i < cells; i++) {
        if (leaf[i] < 0) continue;
        int r = uf.find(label[i]);
        auto &v = votes[(size_t)leaf[i]];
        bool found = false;
        for (auto &[room, k] : v)
            if (room == r) { k++; found = true; break; }
        if (!found) v.push_back({r, 1});
    }
    std::vector<int> room((size_t)L.count, -1);
    std::vector<double> room_cells(peak.size(), 0);
    for (int i = 0; i < L.count; i++) {
        int best = -1, most = 0;
        for (auto &[r, k] : votes[(size_t)i]) {
            room_cells[(size_t)r] += k;
            if (k > most) { most = k; best = r; }
        }
        room[(size_t)i] = best;
    }
    std::vector<std::vector<std::pair<int, double>>> nb((size_t)L.count);   // leaf neighbours and shared area
    for (auto &p : L.portals)
        if (p.front >= 0 && p.back >= 0 && p.front != p.back) {
            double a = area(p.points);
            nb[(size_t)p.front].push_back({p.back, a});
            nb[(size_t)p.back].push_back({p.front, a});
        }
    for (bool changed = true; changed;) {
        changed = false;
        for (int i = 0; i < L.count; i++) {
            if (room[(size_t)i] >= 0) continue;
            int best = -1;
            double most = -1;
            for (auto &[j, a] : nb[(size_t)i])
                if (room[(size_t)j] >= 0 && a > most && (!o.keep_clusters || L.cluster[(size_t)j] == L.cluster[(size_t)i])) {
                    most = a;
                    best = room[(size_t)j];
                }
            if (best >= 0) { room[(size_t)i] = best; changed = true; }
        }
    }

    // 4b. Where two rooms meet, the boundary goes onto one plane if it can: a doorway's opening is usually on one of
    // the BSP's planes (the wall's), so the leaves near the boundary split cleanly along it, and one flat portal
    // covers the opening. Each plane the openings near the boundary are on is tried: cut along it (near the
    // boundary only), does it part the rooms? The one with the smallest opening wins. Else the boundary stays.
    std::vector<std::vector<std::pair<int, int>>> edges((size_t)L.count);   // leaf -> (neighbour, leaf portal)
    for (size_t k = 0; k < L.portals.size(); k++) {
        const LeafPortal &p = L.portals[k];
        if (p.front < 0 || p.back < 0 || p.front == p.back) continue;
        edges[(size_t)p.front].push_back({p.back, (int)k});
        edges[(size_t)p.back].push_back({p.front, (int)k});
    }
    int planar = 0, ragged_left = 0, two_planes = 0;
    if (o.refine) {
        auto started = std::chrono::steady_clock::now();
        int rooms_n = 0;
        for (int r : room) rooms_n = std::max(rooms_n, r + 1);
        std::vector<std::vector<int>> members((size_t)rooms_n);   // each room's leaves
        for (int i = 0; i < L.count; i++)
            if (room[(size_t)i] >= 0) members[(size_t)room[(size_t)i]].push_back(i);
        std::vector<int> best_mark((size_t)L.count, -1);
        std::map<std::pair<int, int>, char> pairs;
        for (int i = 0; i < L.count; i++)
            for (auto &[j, k] : edges[(size_t)i])
                if (room[(size_t)i] != room[(size_t)j] && (!o.keep_clusters || L.cluster[(size_t)i] == L.cluster[(size_t)j]))
                    pairs[{std::min(room[(size_t)i], room[(size_t)j]), std::max(room[(size_t)i], room[(size_t)j])}] = 1;
        const int reach = o.reach;   // how many leaves from the boundary the cut may be
        std::vector<int> hops((size_t)L.count), mark((size_t)L.count, -1);
        int stamp = 0;
        for (auto &[pr, _] : pairs) {
            // (a level with very many rooms: after a while the rest of the boundaries stay as they are)
            if (std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() > o.refine_seconds) break;
            int A = pr.first, B = pr.second;
            auto in = [&](int i) {
                return (room[(size_t)i] == A || room[(size_t)i] == B);
            };
            std::vector<int> both = members[(size_t)A];
            both.insert(both.end(), members[(size_t)B].begin(), members[(size_t)B].end());
            // Steps from the boundary (within each room).
            for (int i : both) hops[(size_t)i] = -1;
            std::vector<int> q;
            for (int i : both) {
                for (auto &[j, k] : edges[(size_t)i])
                    if (in(j) && room[(size_t)j] != room[(size_t)i] && (!o.keep_clusters || L.cluster[(size_t)i] == L.cluster[(size_t)j])) {
                        hops[(size_t)i] = 0;
                        q.push_back(i);
                        break;
                    }
            }
            if (q.empty()) continue;
            for (size_t h2 = 0; h2 < q.size(); h2++)
                for (auto &[j, k] : edges[(size_t)q[h2]])
                    if (in(j) && hops[(size_t)j] < 0 && room[(size_t)j] == room[(size_t)q[h2]]) {
                        hops[(size_t)j] = hops[(size_t)q[h2]] + 1;
                        q.push_back(j);
                    }
            // The planes of the openings near the boundary, and each side's leaves away from it.
            std::map<int, double> planes_near;
            std::vector<int> core_a, core_b;
            int far_a = 0, far_b = 0;
            for (int i : q) (room[(size_t)i] == A ? far_a : far_b) = std::max(room[(size_t)i] == A ? far_a : far_b, hops[(size_t)i]);
            int ca = std::min(reach + 1, far_a), cb = std::min(reach + 1, far_b);
            for (int i : q) {
                if (room[(size_t)i] == A && hops[(size_t)i] >= ca) core_a.push_back(i);
                if (room[(size_t)i] == B && hops[(size_t)i] >= cb) core_b.push_back(i);
                if (hops[(size_t)i] > reach) continue;
                for (auto &[j, k] : edges[(size_t)i])
                    if (in(j) && hops[(size_t)j] >= 0 && hops[(size_t)j] <= reach) planes_near[L.portals[(size_t)k].plane] += 1;
            }
            if (core_a.empty() || core_b.empty()) continue;
            // (leaves of A and B not reached from the boundary keep their room)
            int best_plane = -1;
            double best_area = 1e30;
            int best_stamp = -1;
            // Try cutting along `cut` (one plane, or two): true when it parts the rooms; keeps the smallest.
            auto attempt = [&](int p1, int p2) {
                const int plane = p1;
                stamp++;
                std::vector<int> st = core_a;
                for (int i : st) mark[(size_t)i] = stamp;
                bool reached_b = false;
                for (size_t h2 = 0; h2 < st.size() && !reached_b; h2++) {
                    int a = st[h2];
                    for (auto &[j, k] : edges[(size_t)a]) {
                        if (!in(j) || mark[(size_t)j] == stamp) continue;
                        if (o.keep_clusters && L.cluster[(size_t)j] != L.cluster[(size_t)a]) continue;
                        bool near = hops[(size_t)a] >= 0 && hops[(size_t)a] <= reach && hops[(size_t)j] >= 0 && hops[(size_t)j] <= reach;
                        if (near && (L.portals[(size_t)k].plane == plane || L.portals[(size_t)k].plane == p2)) continue;   // cut here
                        if (room[(size_t)j] == B && hops[(size_t)j] >= cb) { reached_b = true; break; }
                        mark[(size_t)j] = stamp;
                        st.push_back(j);
                    }
                }
                if (reached_b) return false;
                // It parts them: the opening's area (what crosses from A's side to the rest).
                double cut = 0;
                bool core_b_reached = false;
                for (int b2 : core_b) core_b_reached = core_b_reached || mark[(size_t)b2] == stamp;
                if (core_b_reached) return false;
                for (int a : st)
                    for (auto &[j, k] : edges[(size_t)a])
                        if (in(j) && mark[(size_t)j] != stamp) cut += area(L.portals[(size_t)k].points);
                if (cut < best_area) {
                    best_area = cut;
                    best_plane = plane;
                    best_stamp = stamp;
                    for (int a : st) best_mark[(size_t)a] = stamp;
                }
                return true;
            };
            for (auto &[plane, count] : planes_near) attempt(plane, -1);
            if (best_plane < 0 && planes_near.size() <= 80) {   // two planes (an opening across two of them)
                std::vector<int> pl;
                for (auto &[plane, count] : planes_near) pl.push_back(plane);
                for (size_t x = 0; x < pl.size(); x++)
                    for (size_t y = x + 1; y < pl.size(); y++) attempt(pl[x], pl[y]);
                if (best_plane >= 0) two_planes++;
            }
            if (best_plane < 0) { ragged_left++; continue; }
            planar++;
            for (int i : q) room[(size_t)i] = best_mark[(size_t)i] == best_stamp ? A : B;
            members[(size_t)A].clear();
            members[(size_t)B].clear();
            for (int i : both) members[(size_t)room[(size_t)i]].push_back(i);
        }
    }

    // Leaves with no open neighbours (closed off: the camera can't be in them) go with the nearest leaf that has.
    {
        std::vector<V3> mid((size_t)L.count, V3{0, 0, 0});
        std::vector<int> pts((size_t)L.count, 0);
        for (auto &p : L.portals)
            for (int leaf_ : {p.front, p.back})
                if (leaf_ >= 0)
                    for (auto &x : p.points) { mid[(size_t)leaf_] = mid[(size_t)leaf_] + x; pts[(size_t)leaf_]++; }
        std::vector<int> open_leaves;
        for (int i = 0; i < L.count; i++) {
            if (pts[(size_t)i]) mid[(size_t)i] = mid[(size_t)i] * (1.0 / pts[(size_t)i]);
            if (!nb[(size_t)i].empty() && room[(size_t)i] >= 0) open_leaves.push_back(i);
        }
        for (int i = 0; i < L.count; i++) {
            if (!nb[(size_t)i].empty() && room[(size_t)i] >= 0) continue;
            int best = -1;
            double bd = 1e30;
            for (int j : open_leaves) {
                if (o.keep_clusters && L.cluster[(size_t)j] != L.cluster[(size_t)i]) continue;
                V3 d = mid[(size_t)j] - mid[(size_t)i];
                if (dot(d, d) < bd) { bd = dot(d, d); best = j; }
            }
            if (best >= 0) { room[(size_t)i] = room[(size_t)best]; nb[(size_t)i].push_back({best, 0.0}); nb[(size_t)best].push_back({i, 0.0}); }
            else if (room[(size_t)i] < 0) room[(size_t)i] = 0;
        }
    }

    // 5. Clusters: connected leaves of one room (and one of the map's own clusters, when keeping them).
    std::vector<int> cl((size_t)L.count, -1);
    int clusters = 0;
    for (int i = 0; i < L.count; i++) {
        if (cl[(size_t)i] >= 0) continue;
        int id = clusters++;
        std::vector<int> stack{i};
        cl[(size_t)i] = id;
        while (!stack.empty()) {
            int a = stack.back();
            stack.pop_back();
            for (auto &[j, ar] : nb[(size_t)a]) {
                if (cl[(size_t)j] >= 0 || room[(size_t)j] != room[(size_t)a]) continue;
                if (o.keep_clusters && L.cluster[(size_t)j] != L.cluster[(size_t)a]) continue;
                cl[(size_t)j] = id;
                stack.push_back(j);
            }
        }
    }
    // Small ones join the neighbour they share the most opening with.
    double cell_volume = h * h * h, min_room = o.min_room > 0 ? o.min_room : 4.0;
    for (int pass = 0; pass < 64; pass++) {
        std::vector<double> vol((size_t)clusters, 0);
        for (int i = 0; i < L.count; i++)
            for (auto &[r, k] : votes[(size_t)i]) vol[(size_t)cl[(size_t)i]] += k * cell_volume;
        std::vector<std::map<int, double>> shared((size_t)clusters);
        for (int i = 0; i < L.count; i++)
            for (auto &[j, a] : nb[(size_t)i])
                if (cl[(size_t)j] != cl[(size_t)i] && (!o.keep_clusters || L.cluster[(size_t)i] == L.cluster[(size_t)j]))
                    shared[(size_t)cl[(size_t)i]][cl[(size_t)j]] += a;
        int merged = 0;
        std::vector<int> to((size_t)clusters);
        for (int c = 0; c < clusters; c++) to[(size_t)c] = c;
        std::vector<int> by_size((size_t)clusters);
        for (int c = 0; c < clusters; c++) by_size[(size_t)c] = c;
        std::sort(by_size.begin(), by_size.end(), [&](int a, int c) { return vol[(size_t)a] < vol[(size_t)c]; });
        std::vector<char> touched((size_t)clusters, 0);
        for (int c : by_size) {
            if (vol[(size_t)c] >= min_room || touched[(size_t)c]) continue;
            int best = -1;
            double most = 0;
            for (auto &[d, a] : shared[(size_t)c])
                if (a > most && !touched[(size_t)d]) { most = a; best = d; }
            if (best < 0) continue;
            to[(size_t)c] = best;
            touched[(size_t)c] = touched[(size_t)best] = 1;
            merged++;
        }
        if (!merged) break;
        std::vector<int> renum((size_t)clusters, -1);
        int k = 0;
        for (int c = 0; c < clusters; c++)
            if (to[(size_t)c] == c) renum[(size_t)c] = k++;
        for (int i = 0; i < L.count; i++) cl[(size_t)i] = renum[(size_t)to[(size_t)cl[(size_t)i]]];
        clusters = k;
    }
    // 6. Openings that take many portals to cover (a ragged boundary through open space, not a doorway's flat
    // opening) aren't worth it: those clusters join. Then the engine's limits: 512 portals in all, 128 per cluster,
    // and 128 clusters drawn at once (more clusters than that in all could leave some of the level undrawn).
    int ragged = 0, limited = 0;
    for (int round = 0; round < 1000; round++) {
        std::vector<ClusterPortal> ps;
        cluster_portals(L, cl, ps);
        std::map<std::pair<int, int>, int> pieces;
        std::vector<int> per((size_t)clusters, 0);
        for (auto &p : ps) {
            pieces[{std::min(p.front, p.back), std::max(p.front, p.back)}]++;
            per[(size_t)p.front]++;
            per[(size_t)p.back]++;
        }
        auto mergeable = [&](int a, int c) {
            if (!o.keep_clusters) return true;
            // (both within one of the map's own clusters)
            for (int i = 0; i < L.count; i++)
                if (cl[(size_t)i] == a || cl[(size_t)i] == c)
                    for (int j = 0; j < L.count; j++)
                        if ((cl[(size_t)j] == a || cl[(size_t)j] == c) && L.cluster[(size_t)i] != L.cluster[(size_t)j]) return false;
            return true;
        };
        std::vector<int> to((size_t)clusters);
        for (int c = 0; c < clusters; c++) to[(size_t)c] = c;
        std::function<int(int)> root = [&](int c) { return to[(size_t)c] == c ? c : to[(size_t)c] = root(to[(size_t)c]); };
        int merged = 0;
        for (auto &[k, n] : pieces)
            if (n > o.max_pieces && root(k.first) != root(k.second) && mergeable(k.first, k.second)) {
                to[(size_t)root(k.second)] = root(k.first);
                merged++;
                ragged++;
            }
        if (!merged) {
            int most = 0;
            for (int c = 0; c < clusters; c++) most = std::max(most, per[(size_t)c]);
            if (ps.size() <= 480 && most <= 120 && (o.keep_clusters || clusters <= 120)) break;
            // Over a limit: the two clusters with the most portals between them join.
            std::pair<int, int> best{-1, -1};
            int bn = 0;
            for (auto &[k, n] : pieces)
                if (n > bn && mergeable(k.first, k.second)) { bn = n; best = k; }
            if (best.first < 0) break;
            to[(size_t)best.second] = best.first;
            merged++;
            limited++;
        }
        std::vector<int> renum((size_t)clusters, -1);
        int k = 0;
        for (int c = 0; c < clusters; c++)
            if (root(c) == c) renum[(size_t)c] = k++;
        for (int i = 0; i < L.count; i++) cl[(size_t)i] = renum[(size_t)root(cl[(size_t)i])];
        clusters = k;
    }
    cluster_of_leaf = cl;
    if (report) {
        char buf[256];
        snprintf(buf, sizeof buf, "grid %dx%dx%d (%.2f units), %zu rooms before merging, %d clusters (%d openings cut flat, %d "
                 "of them on two planes, %d not; %d joins for ragged openings, %d for limits)", n[0], n[1], n[2], h, peak.size(), clusters,
                 planar, two_planes, ragged_left, ragged, limited);
        *report = buf;
    }
    return true;
}


namespace {

// The level's triangles (structure surfaces), from the lightmaps' vertex buffers.
bool load_triangles(const tagfile::Elem &r, std::vector<std::array<V3, 3>> &tri) {
    tagfile::Block surfaces = r.block("surfaces"), lightmaps = r.block("lightmaps");
    tri.assign(surfaces.size(), {V3{0, 0, 0}, V3{0, 0, 0}, V3{0, 0, 0}});
    bool any = false;
    for (size_t l = 0; l < lightmaps.size(); l++) {
        tagfile::Block mats = lightmaps[l].block("materials");
        for (size_t m = 0; m < mats.size(); m++) {
            size_t count = (size_t)mats[m].integer("rendered vertices count");
            std::string data = mats[m].data("uncompressed vertices");
            size_t stride = 56;
            if (data.size() < count * stride) { data = mats[m].data("compressed vertices"); stride = 32; }
            if (data.size() < count * stride) continue;
            auto pos = [&](size_t i) {
                float f[3];
                memcpy(f, data.data() + i * stride, 12);   // (little endian, as the GPU reads it)
                return V3{f[0], f[1], f[2]};
            };
            long long first = mats[m].integer("surfaces"), n = mats[m].integer("surface count");
            for (long long sfc = first; sfc < first + n && sfc >= 0 && sfc < (long long)surfaces.size(); sfc++) {
                size_t a = (size_t)surfaces[(size_t)sfc].integer("vertex0 index"), b = (size_t)surfaces[(size_t)sfc].integer("vertex1 index"),
                       c = (size_t)surfaces[(size_t)sfc].integer("vertex2 index");
                if (a >= count || b >= count || c >= count) continue;
                tri[(size_t)sfc] = {pos(a), pos(b), pos(c)};
                any = true;
            }
        }
    }
    return any;
}

// Openings the collision BSP closes but the eye sees through: a seam of solid thinner than a hair between two
// pieces of the level, an invisible wall (collision only), glass, a grate. From each leaf's wall faces, probes go
// straight into the solid; one that comes out into another leaf within kSeeDepth with no solid-looking level triangle
// on the way is an opening, and the face becomes a portal between the two leaves as well. (A face bigger than its
// opening only means a bigger portal: more drawn, never less.)
const double kSeeDepth = 0.5;   // world units (5 feet)

// The leaves a segment passes through, near to far: `visit(leaf or kSolid, t where it enters)` (true: stop).
bool trace(const Bsp &b, int id, const V3 &a, const V3 &c, double ta, double tc, const std::function<bool(int, double)> &visit, int depth = 0) {
    if (id == kSolid || is_leaf(id) || depth > 4096) return visit(id == kSolid ? kSolid : leaf_of(id), ta);
    if (id < 0) return visit(kSolid, ta);
    const Bsp::Node &n = b.nodes[(size_t)id];
    const Plane &pl = b.planes[(size_t)n.plane];
    double da = dot(pl.n, a) - pl.d, dc = dot(pl.n, c) - pl.d;
    if (da >= 0 && dc >= 0) return trace(b, n.front, a, c, ta, tc, visit, depth + 1);
    if (da < 0 && dc < 0) return trace(b, n.back, a, c, ta, tc, visit, depth + 1);
    double t = da / (da - dc);
    V3 m = a + (c - a) * t;
    double tm = ta + (tc - ta) * t;
    int near = da >= 0 ? n.front : n.back, far = da >= 0 ? n.back : n.front;
    if (trace(b, near, a, m, ta, tm, visit, depth + 1)) return true;
    return trace(b, far, m, c, tm, tc, visit, depth + 1);
}

// Each level triangle: can it be seen through (its shader)?
std::vector<char> see_through_triangles(const tagfile::Elem &r, size_t count, const SeeThrough &see_through) {
    std::vector<char> st(count, 0);
    tagfile::Block lightmaps = r.block("lightmaps");
    std::map<std::pair<std::string, std::string>, bool> known;
    for (size_t l = 0; l < lightmaps.size(); l++) {
        tagfile::Block mats = lightmaps[l].block("materials");
        for (size_t m = 0; m < mats.size(); m++) {
            std::string group, shader = mats[m].ref("shader", &group);
            auto key = std::make_pair(shader, group);
            auto it = known.find(key);
            if (it == known.end())
                it = known.emplace(key, see_through ? see_through(shader, group) : group != "senv" && group != "soso").first;
            if (!it->second) continue;
            long long first = mats[m].integer("surfaces"), n = mats[m].integer("surface count");
            for (long long sfc = first; sfc < first + n && sfc >= 0 && sfc < (long long)count; sfc++) st[(size_t)sfc] = 1;
        }
    }
    return st;
}

// Does the segment a-c cross the triangle?
bool crosses(const V3 &a, const V3 &c, const std::array<V3, 3> &t) {
    V3 d = c - a, e1 = t[1] - t[0], e2 = t[2] - t[0];
    V3 p = cross(d, e2);
    double det = dot(e1, p);
    if (std::fabs(det) < 1e-12) return false;
    double inv = 1 / det;
    V3 s = a - t[0];
    double u = dot(s, p) * inv;
    if (u < 0 || u > 1) return false;
    V3 q = cross(s, e1);
    double v = dot(d, q) * inv;
    if (v < 0 || u + v > 1) return false;
    double k = dot(e2, q) * inv;
    return k >= 0 && k <= 1;
}

void hidden_openings(const Bsp &b, const tagfile::Elem &r, const SeeThrough &see_through, std::vector<LeafPortal> &portals) {
    std::vector<std::array<V3, 3>> tri;
    if (!load_triangles(r, tri)) return;
    std::vector<char> clear = see_through_triangles(r, tri.size(), see_through);
    // The solid-looking triangles in a tree of boxes (each box around its triangles; four triangles a leaf).
    struct Box { V3 lo, hi; int first = 0, count = 0, left = -1, right = -1; };
    std::vector<int> order;
    std::vector<V3> lo_of, hi_of;
    for (size_t i = 0; i < tri.size(); i++) {
        if (clear[i]) continue;
        V3 lo = tri[i][0], hi = tri[i][0];
        for (auto &p : tri[i])
            for (size_t k = 0; k < 3; k++) { lo[k] = std::min(lo[k], p[k]); hi[k] = std::max(hi[k], p[k]); }
        order.push_back((int)i);
        lo_of.push_back(lo);
        hi_of.push_back(hi);
    }
    std::vector<int> slot(tri.size(), -1);
    for (size_t k = 0; k < order.size(); k++) slot[(size_t)order[k]] = (int)k;
    std::vector<Box> boxes;
    std::function<int(int, int)> build = [&](int first, int count) {
        Box bx;
        bx.lo = {1e30, 1e30, 1e30};
        bx.hi = {-1e30, -1e30, -1e30};
        for (int k = first; k < first + count; k++) {
            int j = slot[(size_t)order[(size_t)k]];
            for (size_t a = 0; a < 3; a++) { bx.lo[a] = std::min(bx.lo[a], lo_of[(size_t)j][a]); bx.hi[a] = std::max(bx.hi[a], hi_of[(size_t)j][a]); }
        }
        bx.first = first;
        bx.count = count;
        int id = (int)boxes.size();
        boxes.push_back(bx);
        if (count <= 4) return id;
        size_t ax = 0;
        for (size_t a = 1; a < 3; a++)
            if (bx.hi[a] - bx.lo[a] > bx.hi[ax] - bx.lo[ax]) ax = a;
        auto mid = [&](int t) { int j = slot[(size_t)t]; return lo_of[(size_t)j][ax] + hi_of[(size_t)j][ax]; };
        int half = count / 2;
        std::nth_element(order.begin() + first, order.begin() + first + half, order.begin() + first + count,
                         [&](int x, int y) { return mid(x) < mid(y); });
        int l = build(first, half), r2 = build(first + half, count - half);
        boxes[(size_t)id].left = l;
        boxes[(size_t)id].right = r2;
        return id;
    };
    if (!order.empty()) build(0, (int)order.size());
    auto blocked = [&](const V3 &a, const V3 &c) {
        if (boxes.empty()) return false;
        V3 d = c - a;
        std::vector<int> todo{0};
        while (!todo.empty()) {
            const Box &bx = boxes[(size_t)todo.back()];
            todo.pop_back();
            double t0 = 0, t1 = 1;
            bool hit = true;
            for (size_t k = 0; k < 3 && hit; k++) {
                if (std::fabs(d[k]) < 1e-12) {
                    if (a[k] < bx.lo[k] - 1e-6 || a[k] > bx.hi[k] + 1e-6) hit = false;
                    continue;
                }
                double u = (bx.lo[k] - 1e-6 - a[k]) / d[k], v = (bx.hi[k] + 1e-6 - a[k]) / d[k];
                if (u > v) std::swap(u, v);
                t0 = std::max(t0, u);
                t1 = std::min(t1, v);
                if (t0 > t1) hit = false;
            }
            if (!hit) continue;
            if (bx.left < 0) {
                for (int k = bx.first; k < bx.first + bx.count; k++)
                    if (crosses(a, c, tri[(size_t)order[(size_t)k]])) return true;
                continue;
            }
            todo.push_back(bx.left);
            todo.push_back(bx.right);
        }
        return false;
    };
    std::vector<LeafPortal> found;
    size_t n = portals.size();
    for (size_t k = 0; k < n; k++) {
        const LeafPortal &lp = portals[k];
        if ((lp.front >= 0) == (lp.back >= 0) || lp.plane < 0 || lp.points.size() < 3) continue;   // a wall face only
        int leaf = lp.front >= 0 ? lp.front : lp.back;
        const Plane &pl = b.planes[(size_t)lp.plane];
        V3 into = lp.front >= 0 ? pl.n * -1.0 : pl.n;   // into the solid
        // Probe points: the face's middle and a grid over it (inset from its edges).
        V3 mid{0, 0, 0};
        for (auto &q : lp.points) mid = mid + q;
        mid = mid * (1.0 / lp.points.size());
        double a = area(lp.points);
        std::vector<V3> probes{mid};
        if (a > 0.01) {
            V3 u = lp.points[1] - lp.points[0];
            u = u * (1 / std::max(len(u), 1e-9));
            V3 v = cross(pl.n, u);
            double ulo = 1e30, uhi = -1e30, vlo = 1e30, vhi = -1e30;
            for (auto &q : lp.points) {
                ulo = std::min(ulo, dot(q - mid, u)); uhi = std::max(uhi, dot(q - mid, u));
                vlo = std::min(vlo, dot(q - mid, v)); vhi = std::max(vhi, dot(q - mid, v));
            }
            double step = std::max(0.1, std::sqrt(a / 16));
            for (double x = ulo + step / 2; x < uhi; x += step)
                for (double y = vlo + step / 2; y < vhi; y += step) {
                    V3 q = mid + u * x + v * y;
                    q = mid + (q - mid) * 0.97;   // (off the edges)
                    // inside the (convex) face?
                    bool in = true;
                    for (size_t i = 0; i < lp.points.size() && in; i++) {
                        const V3 &p0 = lp.points[i], &p1 = lp.points[(i + 1) % lp.points.size()];
                        double s1 = dot(cross(p1 - p0, q - p0), pl.n), s2 = dot(cross(p1 - p0, mid - p0), pl.n);
                        if ((s1 < 0) != (s2 < 0) && std::fabs(s1) > 1e-9) in = false;
                    }
                    if (in) probes.push_back(q);
                    if (probes.size() >= 24) break;
                }
        }
        std::vector<int> to;
        for (const V3 &s : probes) {
            V3 a0 = s + into * 0.001, c0 = s + into * kSeeDepth;
            bool solid = false;   // (passed through some)
            int other = -1;
            double at = 0;
            // (Open space right behind the face is another leaf the tree's numbers put next to this one: an opening
            // the portals between the leaves missed.)
            trace(b, 0, a0, c0, 0, 1, [&](int id, double t) {
                if (id == kSolid) { solid = true; return false; }
                if (id == leaf) return false;
                other = id;
                at = t;
                return true;
            });
            if (other < 0 || std::find(to.begin(), to.end(), other) != to.end()) continue;
            V3 e = a0 + (c0 - a0) * at;
            if (blocked(s - into * 0.02, e + into * 0.02)) continue;
            to.push_back(other);
        }
        for (int o : to) {
            LeafPortal x = lp;
            (x.front >= 0 ? x.back : x.front) = o;
            found.push_back(std::move(x));
        }
    }
    for (auto &x : found) portals.push_back(std::move(x));
}

using P2 = std::pair<double, double>;
using Hull = std::vector<P2>;

double signed_area(const Hull &h) {
    double a = 0;
    for (size_t i = 0; i < h.size(); i++) {
        const P2 &p = h[i], &q = h[(i + 1) % h.size()];
        a += p.first * q.second - q.first * p.second;
    }
    return a / 2;
}

// `subject` clipped by the convex, counterclockwise `clip`.
Hull clip_hull(Hull subject, const Hull &clip) {
    for (size_t i = 0; i < clip.size() && !subject.empty(); i++) {
        P2 a = clip[i], b = clip[(i + 1) % clip.size()];
        auto side = [&](const P2 &p) { return (b.first - a.first) * (p.second - a.second) - (b.second - a.second) * (p.first - a.first); };
        Hull out;
        for (size_t k = 0; k < subject.size(); k++) {
            const P2 &p = subject[k], &q = subject[(k + 1) % subject.size()];
            double sp = side(p), sq = side(q);
            if (sp >= 0) out.push_back(p);
            if ((sp >= 0) != (sq >= 0)) {
                double t = sp / (sp - sq);
                out.push_back({p.first + t * (q.first - p.first), p.second + t * (q.second - p.second)});
            }
        }
        subject = std::move(out);
        if (subject.size() < 3) subject.clear();
    }
    return subject;
}

struct View { V3 eye, right, up, fwd; };

// The level's floors: triangles facing up (Halo winds a surface's corners clockwise seen from the front) with open
// space just above them; `cum`: their areas added up, for picking one at random by area.
void floor_triangles(const Bsp &b, const std::vector<std::array<V3, 3>> &tri, std::vector<int> &floors, std::vector<double> &cum,
                     const PlayArea &area = PlayArea()) {
    for (double facing : {-1.0, 1.0}) {   // (the other winding only if none face up: a level built some other way)
        floors.clear();
        cum.clear();
        double total = 0;
        for (size_t i = 0; i < tri.size(); i++) {
            V3 n = cross(tri[i][1] - tri[i][0], tri[i][2] - tri[i][0]);
            double a = len(n);
            if (a <= 1e-9 || facing * n[2] / a < 0.7) continue;
            V3 c = (tri[i][0] + tri[i][1] + tri[i][2]) * (1.0 / 3);
            if (area.set && (c[0] < area.lo[0] || c[0] > area.hi[0] || c[1] < area.lo[1] || c[1] > area.hi[1] || c[2] < area.lo[2] ||
                             c[2] > area.hi[2]))
                continue;
            if (area.killed(c + V3{0, 0, 0.3})) continue;   // (standing there kills you)
            if (b.leaf_at(c + V3{0, 0, 0.05}) < 0) continue;   // (solid above: not somewhere to stand)
            total += a;
            floors.push_back((int)i);
            cum.push_back(total);
        }
        if (!floors.empty()) return;
    }
}

// How the engine sees the level from one spot: from the camera's cluster on through the portals it can see, the
// view narrowed to each portal (structure_visibility_traverse_cluster). Returns the triangles drawn.
int drawn_from(const View &view, int start, const std::vector<std::vector<int>> &portals_of, const std::vector<ClusterPortal> &portals,
               const std::vector<std::vector<int>> &surfaces_of, const std::vector<std::array<V3, 3>> &tri, std::vector<int> &stamp,
               int frame, const std::vector<std::vector<char>> *pvs = nullptr) {
    const double tx = std::tan(35.0 * 3.14159265358979 / 180), ty = tx * 0.75;
    size_t nc = surfaces_of.size();
    std::vector<char> on_path(nc, 0), seen(nc, 0);
    std::vector<std::array<double, 4>> rect(nc, {1e9, 1e9, -1e9, -1e9});
    int steps = 0;
    auto to_view = [&](const V3 &p) {
        V3 d = p - view.eye;
        return V3{dot(d, view.right), dot(d, view.up), dot(d, view.fwd)};
    };
    std::function<void(int, const Hull &)> visit = [&](int c, const Hull &h) {
        if (++steps > 20000) return;
        on_path[(size_t)c] = 1;
        seen[(size_t)c] = 1;
        for (auto &p : h) {
            rect[(size_t)c][0] = std::min(rect[(size_t)c][0], p.first);
            rect[(size_t)c][1] = std::min(rect[(size_t)c][1], p.second);
            rect[(size_t)c][2] = std::max(rect[(size_t)c][2], p.first);
            rect[(size_t)c][3] = std::max(rect[(size_t)c][3], p.second);
        }
        for (int pi : portals_of[(size_t)c]) {
            const ClusterPortal &pt = portals[(size_t)pi];
            int other = pt.front == c ? pt.back : pt.front;
            if (other < 0 || on_path[(size_t)other]) continue;
            if (pvs && !(*pvs)[(size_t)start][(size_t)other]) continue;   // (the visibility data says it can't be seen)
            // (its normal from all its corners: three of them can be in a line)
            V3 nrm{0, 0, 0}, mid{0, 0, 0};
            for (size_t i = 0; i < pt.points.size(); i++) {
                const V3 &p = pt.points[i], &q = pt.points[(i + 1) % pt.points.size()];
                nrm = nrm + V3{(p[1] - q[1]) * (p[2] + q[2]), (p[2] - q[2]) * (p[0] + q[0]), (p[0] - q[0]) * (p[1] + q[1])};
                mid = mid + p;
            }
            if (len(nrm) < 1e-12) continue;
            nrm = nrm * (1 / len(nrm));
            mid = mid * (1.0 / pt.points.size());
            double dist = dot(nrm, view.eye - mid);
            double facing = pt.front == c ? -dist : dist;   // (the front cluster is behind the plane)
            if (std::fabs(facing) < 0.1) { visit(other, h); continue; }
            if (facing < 0) continue;
            // The portal on screen: in view space, cut at the near plane, projected.
            Poly vp;
            for (auto &q : pt.points) vp.push_back(to_view(q));
            vp = clip(vp, Plane{{0, 0, 1}, 0.01}, true);
            if (vp.size() < 3) continue;
            Hull ph;
            for (auto &q : vp) ph.push_back({q[0] / q[2], q[1] / q[2]});
            if (signed_area(ph) < 0) std::reverse(ph.begin(), ph.end());
            Hull inter = clip_hull(ph, h);
            if (inter.size() >= 3 && std::fabs(signed_area(inter)) > 1e-9) visit(other, inter);
        }
        on_path[(size_t)c] = 0;
    };
    Hull screen{{-tx, -ty}, {tx, -ty}, {tx, ty}, {-tx, ty}};
    visit(start, screen);
    int count = 0;
    for (size_t c = 0; c < nc; c++) {
        if (!seen[c]) continue;
        auto &R = rect[c];
        for (int sfc : surfaces_of[c]) {
            if (stamp[(size_t)sfc] == frame) continue;
            double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
            bool behind = false, front = false;
            for (auto &p : tri[(size_t)sfc]) {
                V3 q = to_view(p);
                if (q[2] <= 0.01) { behind = true; continue; }
                front = true;
                x0 = std::min(x0, q[0] / q[2]); x1 = std::max(x1, q[0] / q[2]);
                y0 = std::min(y0, q[1] / q[2]); y1 = std::max(y1, q[1] / q[2]);
            }
            if (!front) continue;
            if (behind) { x0 = y0 = -1e9; x1 = y1 = 1e9; }   // (crossing the camera's plane: could be anywhere)
            if (x1 < std::max(R[0], -tx) || x0 > std::min(R[2], tx) || y1 < std::max(R[1], -ty) || y0 > std::min(R[3], ty)) continue;
            stamp[(size_t)sfc] = frame;
            count++;
        }
    }
    return count;
}

}   // namespace

bool LevelTriangles::load(const fs::path &bsp) {
    centre.clear();
    radius.clear();
    tagfile::Tag t;
    std::vector<std::array<V3, 3>> tri;
    if (!t.load(bsp) || !load_triangles(t.root(), tri)) return false;
    for (auto &x : tri) {
        V3 c = (x[0] + x[1] + x[2]) * (1.0 / 3);
        centre.push_back(c);
        radius.push_back(std::max({len(x[0] - c), len(x[1] - c), len(x[2] - c)}));
    }
    return true;
}

int LevelTriangles::within(const V3 &p, double r) const {
    int n = 0;
    for (size_t i = 0; i < centre.size(); i++)
        if (len(centre[i] - p) - radius[i] <= r) n++;
    return n;
}

bool LevelChunks::load(const fs::path &bsp) {
    chunks.clear();
    if (!triangles.load(bsp)) return false;
    surface_count = (int)triangles.centre.size();
    tagfile::Tag t;
    if (!t.load(bsp)) return false;
    tagfile::Block clusters = t.root().block("clusters");
    for (size_t i = 0; i < clusters.size(); i++) {
        tagfile::Block sub = clusters[i].block("subclusters");
        for (size_t k = 0; k < sub.size(); k++) {
            Chunk c;
            const char *axes[3] = {"world bounds x", "world bounds y", "world bounds z"};
            for (int a = 0; a < 3; a++) {
                c.lo[(size_t)a] = sub[k].num(axes[a], 0);
                c.hi[(size_t)a] = sub[k].num(axes[a], 1);
            }
            tagfile::Block idx = sub[k].block("surface indices");
            for (size_t j = 0; j < idx.size(); j++) c.surfaces.push_back((int)idx[j].integer("index"));
            chunks.push_back(std::move(c));
        }
    }
    return true;
}

int LevelChunks::gathered(const V3 &p, double r) const {
    if (r < 2.0) return triangles.within(p, r);   // (structure_sphere_threshold: the BSP's leaves, near the light)
    std::vector<char> seen((size_t)surface_count, 0);
    int n = 0;
    for (auto &c : chunks) {
        bool touches = true;
        for (size_t a = 0; a < 3; a++) touches = touches && c.lo[a] <= p[a] + r && c.hi[a] >= p[a] - r;
        if (!touches) continue;
        for (int s : c.surfaces)
            if (s >= 0 && s < surface_count && !seen[(size_t)s]) { seen[(size_t)s] = 1; n++; }
    }
    return n;
}

bool PlayArea::killed(const V3 &p) const {
    for (auto &b : kill) {
        V3 d = p - b.corner;
        double x = dot(d, b.forward), y = dot(d, b.left), z = dot(d, b.up);
        if (x > 0 && y > 0 && z > 0 && x < b.extents[0] && y < b.extents[1] && z < b.extents[2]) return true;
    }
    return false;
}

PlayArea play_area(const fs::path &scenario) {
    PlayArea a;
    tagfile::Tag t;
    if (!t.load(scenario)) return a;
    tagfile::Elem r = t.root();
    tagfile::Block tv = r.block("trigger volumes");
    for (size_t i = 0; i < tv.size(); i++) {
        std::string name = tv[i].str("name");
        for (auto &ch : name) ch = (char)std::tolower((unsigned char)ch);
        if (name.find("kill") == std::string::npos && name.find("death") == std::string::npos) continue;
        PlayArea::Box b;
        for (size_t k = 0; k < 3; k++) {
            b.forward[k] = tv[i].num("rotation vector forward", (int)k);
            b.up[k] = tv[i].num("rotation vector up", (int)k);
            b.corner[k] = tv[i].num("starting corner", (int)k);
            b.extents[k] = tv[i].num("ending corner offset", (int)k);
        }
        if (len(b.forward) < 1e-6 || len(b.up) < 1e-6) { b.forward = {1, 0, 0}; b.up = {0, 0, 1}; }
        b.forward = b.forward * (1 / len(b.forward));
        b.up = b.up * (1 / len(b.up));
        b.left = cross(b.up, b.forward);
        a.kill.push_back(b);
    }
    V3 lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
    int n = 0;
    for (const char *block : {"player starting locations", "netgame flags", "netgame equipment", "vehicles"}) {
        tagfile::Block bl = r.block(block);
        for (size_t i = 0; i < bl.size(); i++) {
            V3 p{bl[i].num("position", 0), bl[i].num("position", 1), bl[i].num("position", 2)};
            for (size_t k = 0; k < 3; k++) { lo[k] = std::min(lo[k], p[k]); hi[k] = std::max(hi[k], p[k]); }
            n++;
        }
    }
    if (n < 2) return a;
    // Room around them: players go a little past the outermost spawn, and up and down from it.
    for (size_t k = 0; k < 2; k++) {
        double m = std::max(5.0, (hi[k] - lo[k]) * 0.15);
        lo[k] -= m;
        hi[k] += m;
    }
    lo[2] -= 3;
    hi[2] += 5;
    a.set = true;
    a.lo = lo;
    a.hi = hi;
    return a;
}

namespace {

// Views from where players play: `each(view, triangles drawn, stamp, frame)` for each (the surfaces drawn are the
// ones whose stamp is `frame`). `max_views`: how many (6 directions from each spot). `triangles`: the level's.
bool sample(const fs::path &bsp_file, Arrangement which, const std::vector<int> *cl_in, const std::vector<ClusterPortal> *portals_in,
            const PlayArea &area, int max_views, int *triangles,
            const std::function<void(const View &, int, const std::vector<int> &, int)> &each) {
    tagfile::Tag t;
    if (!t.load(bsp_file)) return false;
    tagfile::Elem r = t.root();
    Bsp b;
    if (!load_bsp(r, b, nullptr)) return false;
    std::vector<std::array<V3, 3>> tri;
    if (!load_triangles(r, tri)) return false;
    tagfile::Block sl = r.block("leaves"), ls = r.block("leaf surfaces");
    std::vector<int> cl((size_t)b.leaves, 0);
    std::vector<ClusterPortal> portals;
    int nc = 1;
    if (which == Arrangement::Own) {
        tagfile::Block cp = r.block("cluster portals");
        nc = (int)r.block("clusters").size();
        for (int i = 0; i < b.leaves; i++) cl[(size_t)i] = (int)sl[(size_t)i].integer("cluster");
        for (size_t i = 0; i < cp.size(); i++) {
            ClusterPortal p;
            p.front = (int)cp[i].integer("front cluster");
            p.back = (int)cp[i].integer("back cluster");
            p.plane = (int)cp[i].integer("plane index");
            tagfile::Block vs = cp[i].block("vertices");
            for (size_t k = 0; k < vs.size(); k++) p.points.push_back({vs[k].num("point", 0), vs[k].num("point", 1), vs[k].num("point", 2)});
            if (p.points.size() >= 3 && p.front >= 0 && p.back >= 0 && p.front < nc && p.back < nc) portals.push_back(p);
        }
    } else if (which == Arrangement::Given && cl_in && portals_in) {
        cl = *cl_in;
        portals = *portals_in;
        for (int c : cl) nc = std::max(nc, c + 1);
    }
    std::vector<std::vector<int>> surfaces_of((size_t)nc), portals_of((size_t)nc);
    {
        std::vector<std::vector<char>> in((size_t)nc);
        for (int i = 0; i < b.leaves; i++) {
            int c = cl[(size_t)i];
            if (c < 0 || c >= nc) continue;
            if (in[(size_t)c].empty()) in[(size_t)c].assign(tri.size(), 0);
            long long first = sl[(size_t)i].integer("surface references"), n = sl[(size_t)i].integer("surface reference count");
            for (long long k = first; k < first + n && k >= 0 && k < (long long)ls.size(); k++) {
                long long sfc = ls[(size_t)k].integer("surface");
                if (sfc < 0 || sfc >= (long long)tri.size() || in[(size_t)c][(size_t)sfc]) continue;
                in[(size_t)c][(size_t)sfc] = 1;
                surfaces_of[(size_t)c].push_back((int)sfc);
            }
        }
    }
    for (size_t i = 0; i < portals.size(); i++) {
        portals_of[(size_t)portals[i].front].push_back((int)i);
        portals_of[(size_t)portals[i].back].push_back((int)i);
    }
    // The level's own visibility data: which clusters each one can see at all (the engine checks it too).
    std::vector<std::vector<char>> pvs;
    if (which == Arrangement::Own) {
        std::string d = r.data("cluster data");
        size_t row = ((size_t)nc + 31) / 32 * 4;
        if (d.size() >= row * (size_t)nc) {
            pvs.assign((size_t)nc, std::vector<char>((size_t)nc, 0));
            for (int a = 0; a < nc; a++)
                for (int c = 0; c < nc; c++) {
                    const unsigned char *p = (const unsigned char *)d.data() + (size_t)a * row + (size_t)c / 32 * 4;
                    uint32_t w = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;   // (little-endian words)
                    pvs[(size_t)a][(size_t)c] = (char)((w >> (c % 32)) & 1);
                }
        }
    }
    // Where a player's eyes can be: above floors (weighted by area).
    std::vector<int> floors;
    std::vector<double> cum;
    floor_triangles(b, tri, floors, cum, area);
    if (floors.empty() && area.set) floor_triangles(b, tri, floors, cum);   // (no floors where the spawns are: all of them)
    if (floors.empty()) return false;
    double total = cum.back();
    uint64_t seed = 0x9E3779B97F4A7C15ull;
    auto rnd = [&]() {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        return (double)(seed >> 11) / 9007199254740992.0;
    };
    if (triangles) *triangles = (int)tri.size();
    std::vector<int> stamp(tri.size(), -1);
    int frame = 0;
    for (int tries = 0; tries < max_views * 5 / 3 && frame < max_views; tries++) {
        size_t k = (size_t)(std::lower_bound(cum.begin(), cum.end(), rnd() * total) - cum.begin());
        const auto &T = tri[(size_t)floors[std::min(k, floors.size() - 1)]];
        double u = rnd(), v = rnd();
        if (u + v > 1) { u = 1 - u; v = 1 - v; }
        V3 eye = T[0] + (T[1] - T[0]) * u + (T[2] - T[0]) * v + V3{0, 0, 0.62};
        int leaf = b.leaf_at(eye);
        if (leaf < 0 || cl[(size_t)leaf] < 0 || cl[(size_t)leaf] >= nc) continue;
        for (int dir = 0; dir < 6; dir++) {
            double yaw = dir * 3.14159265358979 / 3 + rnd() * 0.5;
            View w;
            w.eye = eye;
            w.fwd = {std::cos(yaw), std::sin(yaw), 0};
            w.up = {0, 0, 1};
            w.right = cross(w.fwd, w.up);
            int n = drawn_from(w, cl[(size_t)leaf], portals_of, portals, surfaces_of, tri, stamp, frame, pvs.empty() ? nullptr : &pvs);
            each(w, n, stamp, frame);
            frame++;
        }
    }
    return frame > 0;
}

}   // namespace

bool estimate(const fs::path &bsp_file, Arrangement which, const std::vector<int> *cl_in, const std::vector<ClusterPortal> *portals_in,
              Drawn &out, const PlayArea &area) {
    out = Drawn();
    std::vector<int> counts;
    int triangles = 0;
    if (!sample(bsp_file, which, cl_in, portals_in, area, 2400, &triangles,
                [&](const View &, int n, const std::vector<int> &, int) { counts.push_back(n); }))
        return false;
    std::sort(counts.begin(), counts.end());
    out.samples = (int)counts.size();
    out.median = counts[counts.size() / 2];
    out.high = counts[counts.size() * 9 / 10];
    double sum = 0;
    for (int c : counts) sum += c;
    out.mean = sum / counts.size();
    out.triangles = triangles;
    return true;
}

bool sample_views(const fs::path &bsp, const PlayArea &area, int count, std::vector<SampleView> &out) {
    out.clear();
    return sample(bsp, Arrangement::Own, nullptr, nullptr, area, count, nullptr,
                  [&](const View &w, int, const std::vector<int> &stamp, int frame) {
                      SampleView v;
                      v.eye = w.eye;
                      v.yaw = std::atan2(w.fwd[1], w.fwd[0]);
                      for (size_t i = 0; i < stamp.size(); i++)
                          if (stamp[i] == frame) v.drawn.push_back((int)i);
                      out.push_back(std::move(v));
                  });
}

Result add_portals(const fs::path &file, const Options &options) {
    using tagfile::TagNode;
    Result r;
    // Only a level with one cluster: the map's own clusters (their sky, fog, sound and weather, and the portals its
    // maker placed) are left as they are.
    tagfile::EditTag et;
    if (!et.load(file)) { r.why = "couldn't be read"; return r; }
    TagNode &root = et.root();
    std::vector<TagNode> *clusters = tagfile::block(root, "clusters"), *cportals = tagfile::block(root, "cluster portals"),
                         *leaves = tagfile::block(root, "leaves"), *leaf_surfaces = tagfile::block(root, "leaf surfaces"),
                         *markers = tagfile::block(root, "lens flare markers");
    if (!clusters || !cportals || !leaves || !leaf_surfaces || !markers) { r.why = "couldn't be read"; return r; }
    r.clusters_before = (int)clusters->size();
    r.portals_before = (int)cportals->size();
    if (clusters->size() != 1) { r.why = clusters->empty() ? "has no clusters" : "already has portals"; return r; }
    Options o = options;
    o.keep_clusters = false;
    Leaves L;
    std::vector<int> cl;
    std::string rep;
    if (!plan(file, o, L, cl, &rep)) { r.why = rep; return r; }
    int nc = 0;
    for (int c : cl) nc = std::max(nc, c + 1);
    if (nc < 2) { r.why = "has no rooms worth keeping apart"; return r; }

    tagfile::Tag rt;
    Bsp b;
    std::vector<std::array<V3, 3>> tri;
    if (!rt.load(file) || !load_bsp(rt.root(), b, nullptr) || !load_triangles(rt.root(), tri)) { r.why = "couldn't be read"; return r; }
    std::vector<V3> pn;
    std::vector<double> pd;
    for (auto &p : b.planes) { pn.push_back(p.n); pd.push_back(p.d); }
    std::vector<ClusterPortal> ps;
    cluster_portals(L, cl, ps, &pn, &pd);
    if (ps.empty()) { r.why = "has no rooms worth keeping apart"; return r; }
    // Each portal's sides as the tree has them just off its middle (a sliver of a portal can come out turned around).
    for (auto &p : ps) {
        if (p.plane < 0 || p.plane >= (int)b.planes.size() || p.points.empty()) continue;
        V3 c{0, 0, 0};
        for (auto &q : p.points) c = c + q;
        c = c * (1.0 / p.points.size());
        const V3 &n = b.planes[(size_t)p.plane].n;
        for (double e : {0.01, 0.03, 0.1}) {
            int lm = b.leaf_at(c - n * e), lp = b.leaf_at(c + n * e);
            int cm = lm >= 0 && lm < (int)cl.size() ? cl[(size_t)lm] : -1, cp = lp >= 0 && lp < (int)cl.size() ? cl[(size_t)lp] : -1;
            if (cm == p.front && cp == p.back) break;
            if (cm == p.back && cp == p.front) { std::swap(p.front, p.back); break; }
        }
    }

    // Worth it? What the engine would draw from where players are, without and with.
    Drawn before, after;
    if (!estimate(file, Arrangement::None, nullptr, nullptr, before, options.area) ||
        !estimate(file, Arrangement::Given, &cl, &ps, after, options.area)) {
        r.why = "couldn't be measured";
        return r;
    }
    r.drawn_before = before.median;
    r.drawn_after = after.median;
    r.drawn_high_before = before.high;
    r.drawn_high_after = after.high;
    if (after.median > before.median * 0.85 && after.mean > before.mean * 0.85) {
        r.why = "is open enough that portals wouldn't save much";
        return r;
    }

    // Each cluster's surfaces: those its leaves list.
    std::vector<std::vector<int>> surf((size_t)nc);
    {
        std::vector<std::vector<char>> in((size_t)nc, std::vector<char>(tri.size(), 0));
        for (size_t i = 0; i < leaves->size() && i < cl.size(); i++) {
            int c = cl[i];
            long long first = (long long)tagfile::get_num((*leaves)[i], "surface references"),
                      n = (long long)tagfile::get_num((*leaves)[i], "surface reference count");
            for (long long k = first; k < first + n && k >= 0 && k < (long long)leaf_surfaces->size(); k++) {
                long long sfc = (long long)tagfile::get_num((*leaf_surfaces)[(size_t)k], "surface");
                if (sfc < 0 || sfc >= (long long)tri.size() || in[(size_t)c][(size_t)sfc]) continue;
                in[(size_t)c][(size_t)sfc] = 1;
                surf[(size_t)c].push_back((int)sfc);
            }
        }
    }
    // Lens flare markers, by the cluster they're in (each cluster's are one run).
    // (A point on a wall may be just inside it: then a little way out, toward where it faces, or any way.)
    auto cluster_at = [&](V3 p, V3 dir) {
        double dl = len(dir);
        if (dl > 1e-9) dir = dir * (1.0 / dl);
        std::vector<V3> tries{p, p + dir * 0.05, p + dir * 0.25};
        for (double e : {0.05, 0.25})
            for (int k = 0; k < 3; k++)
                for (double sg : {1.0, -1.0}) {
                    V3 q = p;
                    q[(size_t)k] += e * sg;
                    tries.push_back(q);
                }
        for (auto &q : tries) {
            int leaf = b.leaf_at(q);
            if (leaf >= 0 && leaf < (int)cl.size()) return cl[(size_t)leaf];
        }
        return 0;
    };
    std::vector<int> marker_cluster(markers->size(), 0);
    for (size_t i = 0; i < markers->size(); i++) {
        TagNode &m = (*markers)[i];
        V3 p{tagfile::get_num(m, "position", 0), tagfile::get_num(m, "position", 1), tagfile::get_num(m, "position", 2)};
        V3 d{tagfile::get_num(m, "direction i component"), tagfile::get_num(m, "direction j component"),
             tagfile::get_num(m, "direction k component")};
        marker_cluster[i] = cluster_at(p, d);
    }
    std::vector<size_t> marker_order(markers->size());
    for (size_t i = 0; i < marker_order.size(); i++) marker_order[i] = i;
    std::stable_sort(marker_order.begin(), marker_order.end(), [&](size_t x, size_t y) { return marker_cluster[x] < marker_cluster[y]; });
    std::vector<TagNode> new_markers;
    for (size_t i : marker_order) new_markers.push_back((*markers)[i]);

    // The clusters: the one cluster's settings (sky, fog, sound, weather...) for each.
    const TagNode tmpl = (*clusters)[0];
    int sub_sid = tagfile::block_struct(tmpl, "subclusters");
    std::vector<TagNode> new_clusters;
    for (int c = 0; c < nc; c++) {
        TagNode cn = tmpl;
        std::vector<TagNode> *subs = tagfile::block(cn, "subclusters");
        if (!subs) { r.why = "couldn't be read"; return r; }
        subs->clear();
        // Subclusters: the cluster's surfaces in groups of up to 64 near each other (the engine checks each group's
        // box against the view).
        std::function<void(std::vector<int>)> split = [&](std::vector<int> idx) {
            if (idx.empty()) return;
            V3 lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
            for (int sfc : idx)
                for (auto &p : tri[(size_t)sfc])
                    for (int k = 0; k < 3; k++) { lo[k] = std::min(lo[k], p[k]); hi[k] = std::max(hi[k], p[k]); }
            if (idx.size() <= 64) {
                TagNode sn = tagfile::new_node(sub_sid);
                tagfile::set_num(sn, "world bounds x", lo[0], 0);
                tagfile::set_num(sn, "world bounds x", hi[0], 1);
                tagfile::set_num(sn, "world bounds y", lo[1], 0);
                tagfile::set_num(sn, "world bounds y", hi[1], 1);
                tagfile::set_num(sn, "world bounds z", lo[2], 0);
                tagfile::set_num(sn, "world bounds z", hi[2], 1);
                std::vector<TagNode> *si = tagfile::block(sn, "surface indices");
                int isid = tagfile::block_struct(sn, "surface indices");
                for (int sfc : idx) {
                    TagNode x = tagfile::new_node(isid);
                    tagfile::set_num(x, "index", sfc);
                    si->push_back(std::move(x));
                }
                subs->push_back(std::move(sn));
                return;
            }
            int ax = 0;
            for (int k = 1; k < 3; k++)
                if (hi[k] - lo[k] > hi[ax] - lo[ax]) ax = k;
            auto mid = [&](int sfc) { return tri[(size_t)sfc][0][ax] + tri[(size_t)sfc][1][ax] + tri[(size_t)sfc][2][ax]; };
            size_t h = idx.size() / 2;
            std::nth_element(idx.begin(), idx.begin() + (long)h, idx.end(), [&](int x, int y) { return mid(x) < mid(y); });
            split(std::vector<int>(idx.begin(), idx.begin() + (long)h));
            split(std::vector<int>(idx.begin() + (long)h, idx.end()));
        };
        std::vector<int> mine = surf[(size_t)c];
        std::sort(mine.begin(), mine.end());
        split(mine);
        if (std::vector<TagNode> *csi = tagfile::block(cn, "surface indices")) csi->clear();
        // Mirrors: each in the cluster it's in.
        if (std::vector<TagNode> *mir = tagfile::block(cn, "mirrors")) {
            std::vector<TagNode> keep;
            for (TagNode &m : *mir) {
                std::vector<TagNode> *mv = tagfile::block(m, "vertices");
                V3 mc{0, 0, 0};
                int k = 0;
                if (mv)
                    for (TagNode &v : *mv) {
                        for (int j = 0; j < 3; j++) mc[(size_t)j] += tagfile::get_num(v, "point", j);
                        k++;
                    }
                if (k) mc = mc * (1.0 / k);
                V3 mn{tagfile::get_num(m, "plane", 0), tagfile::get_num(m, "plane", 1), tagfile::get_num(m, "plane", 2)};
                if (cluster_at(mc, mn) == c) keep.push_back(m);
            }
            *mir = std::move(keep);
        }
        std::vector<TagNode> *cpi = tagfile::block(cn, "portals");
        int pisid = tagfile::block_struct(cn, "portals");
        if (!cpi) { r.why = "couldn't be read"; return r; }
        cpi->clear();
        for (size_t k = 0; k < ps.size(); k++)
            if (ps[k].front == c || ps[k].back == c) {
                TagNode x = tagfile::new_node(pisid);
                tagfile::set_num(x, "portal", (double)k);
                cpi->push_back(std::move(x));
            }
        int first = -1, count = 0;
        for (size_t i = 0; i < marker_order.size(); i++)
            if (marker_cluster[marker_order[i]] == c) { if (first < 0) first = (int)i; count++; }
        tagfile::set_num(cn, "first lens flare marker index", first < 0 ? 0 : first);
        tagfile::set_num(cn, "lens flare marker count", count);
        new_clusters.push_back(std::move(cn));
    }

    // The portals.
    int cp_sid = tagfile::block_struct(root, "cluster portals");
    std::vector<TagNode> new_portals;
    for (auto &p : ps) {
        TagNode x = tagfile::new_node(cp_sid);
        tagfile::set_num(x, "front cluster", p.front);
        tagfile::set_num(x, "back cluster", p.back);
        tagfile::set_num(x, "plane index", p.plane);
        V3 c{0, 0, 0};
        for (auto &q : p.points) c = c + q;
        c = c * (1.0 / p.points.size());
        double rad = 0;
        for (auto &q : p.points) rad = std::max(rad, len(q - c));
        for (int k = 0; k < 3; k++) tagfile::set_num(x, "centroid", c[(size_t)k], k);
        tagfile::set_num(x, "bounding radius", rad);
        std::vector<TagNode> *vs = tagfile::block(x, "vertices");
        int vsid = tagfile::block_struct(x, "vertices");
        for (auto &q : p.points) {
            TagNode v = tagfile::new_node(vsid);
            for (int k = 0; k < 3; k++) tagfile::set_num(v, "point", q[(size_t)k], k);
            vs->push_back(std::move(v));
        }
        new_portals.push_back(std::move(x));
    }

    // Leaves, visibility (every cluster may see every other: the portals do the work), and sound: how far apart
    // two clusters are through the portals (a byte each pair: 1/127 of 256 units a step; 0xFF can't be reached).
    for (size_t i = 0; i < leaves->size() && i < cl.size(); i++) tagfile::set_num((*leaves)[i], "cluster", cl[i]);
    size_t row = ((size_t)nc + 31) / 32 * 4;
    std::string pvs((size_t)nc * row, '\0');
    for (int a = 0; a < nc; a++)
        for (int c = 0; c < nc; c++) pvs[(size_t)a * row + (size_t)c / 8] |= (char)(1 << (c % 8));   // (little-endian words)
    std::vector<std::vector<int>> of((size_t)nc);
    for (size_t k = 0; k < ps.size(); k++) { of[(size_t)ps[k].front].push_back((int)k); of[(size_t)ps[k].back].push_back((int)k); }
    std::vector<V3> pc;
    for (auto &p : ps) {
        V3 c{0, 0, 0};
        for (auto &q : p.points) c = c + q;
        pc.push_back(c * (1.0 / p.points.size()));
    }
    std::string pas((size_t)nc * (nc - 1) / 2, '\0');
    for (int a = 0; a < nc; a++) {
        std::vector<double> d(ps.size(), 1e30);
        std::vector<std::pair<double, int>> heap;
        for (int k : of[(size_t)a]) { d[(size_t)k] = 0; heap.push_back({0.0, k}); }
        std::make_heap(heap.begin(), heap.end(), std::greater<>());
        while (!heap.empty()) {
            std::pop_heap(heap.begin(), heap.end(), std::greater<>());
            auto [dd, k] = heap.back();
            heap.pop_back();
            if (dd > d[(size_t)k]) continue;
            for (int c2 : {ps[(size_t)k].front, ps[(size_t)k].back})
                for (int j : of[(size_t)c2]) {
                    double nd = dd + len(pc[(size_t)k] - pc[(size_t)j]);
                    if (nd < d[(size_t)j]) { d[(size_t)j] = nd; heap.push_back({nd, j}); std::push_heap(heap.begin(), heap.end(), std::greater<>()); }
                }
        }
        for (int c = a + 1; c < nc; c++) {
            double best = 1e30;
            for (int k : of[(size_t)c]) best = std::min(best, d[(size_t)k]);
            unsigned char e = best >= 1e29 ? 0xFF : (unsigned char)std::min(127.0, std::round(best * 127.0 / 256.0));
            pas[(size_t)a * (nc - 1) - (size_t)a * (a + 1) / 2 + (size_t)c - 1] = (char)e;
        }
    }
    std::string *cd = tagfile::data(root, "cluster data"), *sd = tagfile::data(root, "sound pas data");
    if (!cd || !sd) { r.why = "couldn't be read"; return r; }
    *cd = pvs;
    *sd = pas;
    *clusters = std::move(new_clusters);
    *cportals = std::move(new_portals);
    *markers = std::move(new_markers);
    if (!et.save(file)) { r.why = "couldn't be saved"; return r; }
    r.changed = true;
    r.clusters_after = nc;
    r.portals_after = (int)ps.size();
    return r;
}

}   // namespace portals
