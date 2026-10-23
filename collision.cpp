// Halo Map Porter - object collision BSPs (see collision.hpp).
#include "collision.hpp"
#include "tagfile.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <set>
#include <queue>
#include <iterator>
#include <climits>
#include <cstdint>

namespace coll {

namespace {

const int kProjection[3][2][2] = {{{2, 1}, {1, 2}}, {{0, 2}, {2, 0}}, {{1, 0}, {0, 1}}};   // global_projection3d_mappings

int projection_of(const double n[3]) {
    double i = std::fabs(n[0]), j = std::fabs(n[1]), k = std::fabs(n[2]);
    if (k >= j && k >= i) return 2;
    return j >= i ? 1 : 0;
}
void project(const double p[3], int axis, bool sign, double out[2]) {
    out[0] = p[kProjection[axis][sign][0]];
    out[1] = p[kProjection[axis][sign][1]];
}
double dist(const Plane &pl, const double p[3]) { return pl.n[0] * p[0] + pl.n[1] * p[1] + pl.n[2] * p[2] - pl.d; }

}   // namespace

size_t Bsp::bytes() const {
    return nodes.size() * 12 + planes.size() * 16 + leaves.size() * 8 + refs.size() * 8 + nodes2.size() * 20 + surfaces.size() * 12 +
           edges.size() * 24 + vertices.size() * 16;
}

bool read_bsp(const tagfile::Elem &e, Bsp &b) {
    using tagfile::Block;
    auto u = [](double x) { return (uint32_t)(int64_t)x; };
    Block n3 = e.block("bsp3d nodes"), pl = e.block("planes"), lv = e.block("leaves"), rf = e.block("bsp2d references"),
          n2 = e.block("bsp2d nodes"), sf = e.block("surfaces"), ed = e.block("edges"), vx = e.block("vertices");
    b = Bsp();
    for (size_t i = 0; i < n3.size(); i++) b.nodes.push_back({u(n3[i].num("plane")), {u(n3[i].num("back child")), u(n3[i].num("front child"))}});
    for (size_t i = 0; i < pl.size(); i++) b.planes.push_back({{pl[i].num("plane", 0), pl[i].num("plane", 1), pl[i].num("plane", 2)}, pl[i].num("plane", 3)});
    for (size_t i = 0; i < lv.size(); i++)
        b.leaves.push_back({(uint16_t)lv[i].num("flags"), (uint16_t)lv[i].num("bsp2d reference count"), u(lv[i].num("first bsp2d reference"))});
    for (size_t i = 0; i < rf.size(); i++) b.refs.push_back({u(rf[i].num("plane")), u(rf[i].num("bsp2d node"))});
    for (size_t i = 0; i < n2.size(); i++)
        b.nodes2.push_back({{n2[i].num("plane", 0), n2[i].num("plane", 1)}, n2[i].num("plane", 2), {u(n2[i].num("left child")), u(n2[i].num("right child"))}});
    for (size_t i = 0; i < sf.size(); i++)
        b.surfaces.push_back({u(sf[i].num("plane")), u(sf[i].num("first edge")), (uint8_t)sf[i].num("flags"), (int8_t)sf[i].num("breakable surface"),
                              (uint16_t)sf[i].num("material")});
    for (size_t i = 0; i < ed.size(); i++)
        b.edges.push_back({{u(ed[i].num("start vertex")), u(ed[i].num("end vertex"))}, {u(ed[i].num("forward edge")), u(ed[i].num("reverse edge"))},
                           {u(ed[i].num("left surface")), u(ed[i].num("right surface"))}});
    for (size_t i = 0; i < vx.size(); i++) b.vertices.push_back({{vx[i].num("point", 0), vx[i].num("point", 1), vx[i].num("point", 2)}, u(vx[i].num("first edge"))});
    return true;
}

void write_bsp(tagfile::TagNode &e, const Bsp &b) {
    using namespace tagfile;
    auto fill = [&](const char *name, size_t n, const std::function<void(TagNode &, size_t)> &set) {
        std::vector<TagNode> *blk = block(e, name);
        if (!blk) return;
        int sid = block_struct(e, name);
        blk->clear();
        for (size_t i = 0; i < n; i++) {
            blk->push_back(new_node(sid));
            set(blk->back(), i);
        }
    };
    auto I = [](uint32_t v) { return (double)v; };
    fill("bsp3d nodes", b.nodes.size(), [&](TagNode &x, size_t i) {
        set_num(x, "plane", I(b.nodes[i].plane));
        set_num(x, "back child", I(b.nodes[i].child[0]));
        set_num(x, "front child", I(b.nodes[i].child[1]));
    });
    fill("planes", b.planes.size(), [&](TagNode &x, size_t i) {
        for (int k = 0; k < 3; k++) set_num(x, "plane", b.planes[i].n[k], k);
        set_num(x, "plane", b.planes[i].d, 3);
    });
    fill("leaves", b.leaves.size(), [&](TagNode &x, size_t i) {
        set_num(x, "flags", b.leaves[i].flags);
        set_num(x, "bsp2d reference count", b.leaves[i].refs);
        set_num(x, "first bsp2d reference", I(b.leaves[i].first));
    });
    fill("bsp2d references", b.refs.size(), [&](TagNode &x, size_t i) {
        set_num(x, "plane", I(b.refs[i].plane));
        set_num(x, "bsp2d node", I(b.refs[i].root));
    });
    fill("bsp2d nodes", b.nodes2.size(), [&](TagNode &x, size_t i) {
        set_num(x, "plane", b.nodes2[i].n[0], 0);
        set_num(x, "plane", b.nodes2[i].n[1], 1);
        set_num(x, "plane", b.nodes2[i].d, 2);
        set_num(x, "left child", I(b.nodes2[i].child[0]));
        set_num(x, "right child", I(b.nodes2[i].child[1]));
    });
    fill("surfaces", b.surfaces.size(), [&](TagNode &x, size_t i) {
        const Surface &f = b.surfaces[i];
        set_num(x, "plane", I(f.plane));
        set_num(x, "first edge", I(f.first_edge));
        set_num(x, "flags", f.flags);
        set_num(x, "breakable surface", f.breakable);
        set_num(x, "material", f.material);
    });
    fill("edges", b.edges.size(), [&](TagNode &x, size_t i) {
        const Edge &d = b.edges[i];
        set_num(x, "start vertex", I(d.v[0]));
        set_num(x, "end vertex", I(d.v[1]));
        set_num(x, "forward edge", I(d.next[0]));
        set_num(x, "reverse edge", I(d.next[1]));
        set_num(x, "left surface", I(d.s[0]));
        set_num(x, "right surface", I(d.s[1]));
    });
    fill("vertices", b.vertices.size(), [&](TagNode &x, size_t i) {
        for (int k = 0; k < 3; k++) set_num(x, "point", b.vertices[i].p[k], k);
        set_num(x, "first edge", I(b.vertices[i].first_edge));
    });
}

std::vector<uint32_t> surface_ring(const Bsp &b, uint32_t s) {
    std::vector<uint32_t> ring;
    if (s >= b.surfaces.size()) return ring;
    uint32_t e = b.surfaces[s].first_edge;
    for (int k = 0; k < 64; k++) {
        if (e >= b.edges.size()) return {};
        const Edge &ed = b.edges[e];
        bool rev = ed.s[1] == s;
        ring.push_back(ed.v[rev]);
        e = ed.next[rev];
        if (e == b.surfaces[s].first_edge) return ring;
    }
    return {};
}

// ---- collision_bsp_test_vector
namespace {
enum Contents { kUnknown, kSolid, kEmpty, kSemiEmpty };
struct VectorTest {
    const Bsp &b;
    double p[3], v[3];
    bool back;
    Hit *hit;
    uint32_t last_leaf = kNone, last_plane = kNone;
    Contents last = kUnknown;
    long nodes_left;

    bool surface_has(uint32_t s, int axis, bool sign, const double pt[2]) {   // collision_surface_test_point
        auto ring = surface_ring(b, s);
        if (ring.empty()) return false;
        for (size_t k = 0; k < ring.size(); k++) {
            double a[2], c[2];
            project(b.vertices[ring[k]].p, axis, sign, a);
            project(b.vertices[ring[(k + 1) % ring.size()]].p, axis, sign, c);
            double v0[2] = {pt[0] - a[0], pt[1] - a[1]}, v1[2] = {c[0] - a[0], c[1] - a[1]};
            if (v0[0] * v1[1] - v0[1] * v1[0] > 0) return false;
        }
        return true;
    }
    uint32_t leaf_surface(uint32_t leaf, uint32_t plane, double t, bool test_surface) {   // collision_leaf_test_vector
        const Leaf &l = b.leaves[leaf];
        for (uint32_t r = l.first; r < l.first + l.refs && r < b.refs.size(); r++) {
            const Ref &ref = b.refs[r];
            if ((ref.plane & ~kLeaf) != plane) continue;
            const Plane &pl = b.planes[plane];
            int axis = projection_of(pl.n);
            bool sign = (pl.n[axis] > 0) != ((ref.plane & kLeaf) != 0);
            double hp[3] = {v[0] * t + p[0], v[1] * t + p[1], v[2] * t + p[2]}, pt[2];
            project(hp, axis, sign, pt);
            uint32_t node = ref.root;
            for (int depth = 0; !(node & kLeaf) && depth < 128; depth++) {
                if (node >= b.nodes2.size()) { node = kNone; break; }
                const Node2 &n = b.nodes2[node];
                node = n.child[n.n[0] * pt[0] + n.n[1] * pt[1] - n.d >= 0];
            }
            if (node == kNone || !(node & kLeaf)) continue;
            uint32_t s = node & ~kLeaf;
            if (s < b.surfaces.size() && (!test_surface || surface_has(s, axis, sign, pt))) return s;
        }
        return kNone;
    }
    bool go(uint32_t node, double t0, double t1, int depth) {
        if (!(node & kLeaf)) {
            if (node >= b.nodes.size() || depth >= 128 || --nodes_left < 0) return false;
            const Node3 &n = b.nodes[node];
            const Plane &pl = b.planes[n.plane];
            double d = dist(pl, p), dot = v[0] * pl.n[0] + v[1] * pl.n[1] + v[2] * pl.n[2];
            double d0 = dot * t0 + d, d1 = dot * t1 + d;
            bool reaches_back = d0 < 0 || d1 < 0, reaches_front = d0 >= 0 || d1 >= 0;
            if (reaches_back && reaches_front) {
                bool front = dot > 0;
                double t = -(d / dot);
                if (go(n.child[!front], t0, t, depth + 1)) return true;
                if (hit->t <= t) return false;
                last_plane = n.plane;
                return go(n.child[front], t, t1, depth + 1);
            }
            return go(n.child[reaches_front], t0, t1, depth + 1);
        }
        uint32_t leaf = kNone;
        Contents c = kSolid;
        if (node != kNone && (node & ~kLeaf) < b.leaves.size()) {
            leaf = node & ~kLeaf;
            c = (b.leaves[leaf].flags & 1) ? kSemiEmpty : kEmpty;
        }
        uint32_t test = kNone;
        bool test_surface = false;
        if (!back && (last == kEmpty || last == kSemiEmpty) && c == kSolid) test = last_leaf;
        else if (back && last == kSolid && (c == kEmpty || c == kSemiEmpty)) test = leaf;
        else if (last == kSemiEmpty && c == kSemiEmpty) { test = back ? leaf : last_leaf; test_surface = true; }
        if (test != kNone && last_plane != kNone) {
            uint32_t s = leaf_surface(test, last_plane, t0, test_surface);
            if (s != kNone) {
                hit->t = t0;
                hit->surface = s;
                return true;
            }
        }
        last_leaf = leaf;
        last = c;
        return false;
    }
};
}   // namespace

Hit test_vector(const Bsp &b, const double p[3], const double v[3], double max_t, bool back_faces) {
    Hit h;
    h.t = max_t < 0 ? 0 : max_t;
    if (b.nodes.empty()) return h;
    VectorTest vt{b, {p[0], p[1], p[2]}, {v[0], v[1], v[2]}, back_faces, &h};
    vt.nodes_left = (long)b.nodes.size();
    vt.go(0, 0, std::min(1.0, std::max(0.0, max_t)), 0);
    return h;
}

// ---- collision_bsp_test_sphere
Features test_sphere(const Bsp &b, const double c[3], double r) {
    Features f;
    std::vector<uint32_t> fs, fe, fv;
    long ops = 0;
    auto add = [&](std::vector<uint32_t> &v, uint32_t x) {
        ops += (long)v.size();
        if (std::find(v.begin(), v.end(), x) == v.end() && v.size() < 256) v.push_back(x);
    };
    std::vector<uint32_t> stack;
    double c2[2] = {0, 0};
    int axis = 2;
    bool sign = true;
    auto surface_test = [&](uint32_t s) {   // collision_surface_test_sphere
        auto ring = surface_ring(b, s);
        if (ring.empty()) return;
        ops += 3 * (long)ring.size();
        bool hitf = false;
        for (uint32_t vi : ring) {
            const double *q = b.vertices[vi].p;
            double dd = (q[0] - c[0]) * (q[0] - c[0]) + (q[1] - c[1]) * (q[1] - c[1]) + (q[2] - c[2]) * (q[2] - c[2]);
            if (dd <= r * r) { add(fv, vi); hitf = true; }
        }
        // edges: the segment within r of the centre (fast_vector_intersects_sphere)
        uint32_t e = b.surfaces[s].first_edge;
        for (size_t k = 0; k < ring.size(); k++) {
            const Edge &ed = b.edges[e];
            bool rev = ed.s[1] == s;
            const double *a = b.vertices[ed.v[rev]].p, *z = b.vertices[ed.v[!rev]].p;
            double d[3] = {z[0] - a[0], z[1] - a[1], z[2] - a[2]}, w[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
            double dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2], t = dd > 0 ? (w[0] * d[0] + w[1] * d[1] + w[2] * d[2]) / dd : 0;
            t = std::min(1.0, std::max(0.0, t));
            double x[3] = {a[0] + d[0] * t - c[0], a[1] + d[1] * t - c[1], a[2] + d[2] * t - c[2]};
            if (x[0] * x[0] + x[1] * x[1] + x[2] * x[2] <= r * r) { add(fe, e); hitf = true; }
            e = ed.next[rev];
        }
        if (!hitf) {
            for (size_t k = 0; k < ring.size(); k++) {
                double a[2], z[2];
                project(b.vertices[ring[k]].p, axis, sign, a);
                project(b.vertices[ring[(k + 1) % ring.size()]].p, axis, sign, z);
                double v0[2] = {a[0] - c2[0], a[1] - c2[1]}, v1[2] = {z[0] - c2[0], z[1] - c2[1]};
                if (v0[0] * v1[1] - v0[1] * v1[0] < 0) return;
            }
        }
        add(fs, s);
    };
    std::function<void(uint32_t, int)> go2 = [&](uint32_t node, int depth) {
        while (!(node & kLeaf)) {
            if (node >= b.nodes2.size() || depth++ >= 128) return;
            ops++;
            const Node2 &n = b.nodes2[node];
            double d = n.n[0] * c2[0] + n.n[1] * c2[1] - n.d;
            if (d <= r) go2(n.child[0], depth);
            if (!(d >= -r)) return;
            node = n.child[1];
        }
        if (node != kNone) surface_test(node & ~kLeaf);
    };
    std::function<void(uint32_t, int)> go = [&](uint32_t node, int depth) {
        while (!(node & kLeaf)) {
            if (node >= b.nodes.size() || depth++ >= 128) return;
            ops++;
            const Node3 &n = b.nodes[node];
            double d = dist(b.planes[n.plane], c);
            bool second = d < r, idx = d > -r;
            if (idx && second) {
                stack.push_back(n.plane | kLeaf);
                go(n.child[0], depth);
                stack.back() = n.plane & ~kLeaf;
                go(n.child[1], depth);
                stack.pop_back();
                return;
            }
            node = n.child[idx];
        }
        if (node == kNone || (node & ~kLeaf) >= b.leaves.size()) return;
        const Leaf &l = b.leaves[node & ~kLeaf];
        f.leaves++;
        for (uint32_t ri = l.first; ri < l.first + l.refs && ri < b.refs.size(); ri++) {
            const Ref &ref = b.refs[ri];
            ops += 1 + (long)stack.size();
            if (std::find(stack.begin(), stack.end(), ref.plane) == stack.end()) continue;
            const Plane &pl = b.planes[ref.plane & ~kLeaf];
            double pd = -dist(pl, c), cp[3] = {pl.n[0] * pd + c[0], pl.n[1] * pd + c[1], pl.n[2] * pd + c[2]};
            axis = projection_of(pl.n);
            sign = (pl.n[axis] > 0) != ((ref.plane & kLeaf) != 0);
            project(cp, axis, sign, c2);
            go2(ref.root, 0);
        }
    };
    if (!b.nodes.empty()) go(0, 0);
    f.surfaces = (int)fs.size();
    f.edges = (int)fe.size();
    f.vertices = (int)fv.size();
    f.ops = ops;
    return f;
}

std::vector<Poly> polygons(const Bsp &b) {
    std::vector<Poly> out;
    for (uint32_t s = 0; s < b.surfaces.size(); s++) {
        auto ring = surface_ring(b, s);
        if (ring.size() < 3) continue;
        Poly p;
        for (auto v : ring) p.pts.push_back({b.vertices[v].p[0], b.vertices[v].p[1], b.vertices[v].p[2]});
        p.material = b.surfaces[s].material;
        p.flags = b.surfaces[s].flags;
        p.breakable = b.surfaces[s].breakable;
        out.push_back(p);
    }
    return out;
}

Hit first_face(const std::vector<Poly> &polys, const double p[3], const double v[3]) {
    Hit best;
    best.t = 2;
    for (uint32_t s = 0; s < polys.size(); s++) {
        const Poly &q = polys[s];
        if (q.pts.size() < 3) continue;
        double n[3] = {0, 0, 0};
        for (size_t k = 0; k < q.pts.size(); k++) {
            auto &a = q.pts[k], &z = q.pts[(k + 1) % q.pts.size()];
            n[0] += (a[1] - z[1]) * (a[2] + z[2]);
            n[1] += (a[2] - z[2]) * (a[0] + z[0]);
            n[2] += (a[0] - z[0]) * (a[1] + z[1]);
        }
        double dn = v[0] * n[0] + v[1] * n[1] + v[2] * n[2];
        if (dn >= 0) continue;   // a back face, or edge on
        double d = n[0] * q.pts[0][0] + n[1] * q.pts[0][1] + n[2] * q.pts[0][2];
        double t = (d - (n[0] * p[0] + n[1] * p[1] + n[2] * p[2])) / dn;
        if (t < 0 || t > 1 || t >= best.t) continue;
        double h[3] = {p[0] + v[0] * t, p[1] + v[1] * t, p[2] + v[2] * t};
        bool in = true;
        for (size_t k = 0; k < q.pts.size() && in; k++) {
            auto &a = q.pts[k], &z = q.pts[(k + 1) % q.pts.size()];
            double e[3] = {z[0] - a[0], z[1] - a[1], z[2] - a[2]}, w[3] = {h[0] - a[0], h[1] - a[1], h[2] - a[2]};
            double x[3] = {e[1] * w[2] - e[2] * w[1], e[2] * w[0] - e[0] * w[2], e[0] * w[1] - e[1] * w[0]};
            in = x[0] * n[0] + x[1] * n[1] + x[2] * n[2] >= 0;
        }
        if (in) { best.t = t; best.surface = s; }
    }
    if (best.surface == kNone) best.t = 1;
    return best;
}

double self_consistency(const Bsp &b, int rays, unsigned seed) {
    auto polys = polygons(b);
    if (polys.empty()) return 1;
    double lo[3] = {1e9, 1e9, 1e9}, hi[3] = {-1e9, -1e9, -1e9};
    for (auto &v : b.vertices)
        for (int k = 0; k < 3; k++) { lo[k] = std::min(lo[k], v.p[k]); hi[k] = std::max(hi[k], v.p[k]); }
    double c[3], R = 0;
    for (int k = 0; k < 3; k++) { c[k] = (lo[k] + hi[k]) / 2; R += (hi[k] - lo[k]) * (hi[k] - lo[k]); }
    R = std::sqrt(R) + 0.1;
    uint32_t x = seed * 2654435761u + 1;
    auto rnd = [&]() { x = x * 1664525u + 1013904223u; return (x >> 8) / 8388608.0 - 1; };   // -1..1
    int agree = 0;
    for (int k = 0; k < rays; k++) {
        double d[3] = {rnd(), rnd(), rnd()}, l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 1e-12;
        for (auto &e : d) e /= l;
        double tgt[3] = {c[0] + rnd() * (hi[0] - lo[0]) * 0.6, c[1] + rnd() * (hi[1] - lo[1]) * 0.6, c[2] + rnd() * (hi[2] - lo[2]) * 0.6};
        double p[3] = {tgt[0] + d[0] * R, tgt[1] + d[1] * R, tgt[2] + d[2] * R}, v[3] = {-d[0] * 2 * R, -d[1] * 2 * R, -d[2] * 2 * R};
        Hit h = test_vector(b, p, v), g = first_face(polys, p, v);
        bool same = (h.surface == kNone) == (g.surface == kNone) && (h.surface == kNone || std::fabs(h.t - g.t) * 2 * R < 1e-3);
        agree += same;
    }
    return agree / (double)rays;
}

// ---- building
namespace {

const double kEps = 1e-4;   // a point this close to a plane is on it

struct V3 { double x, y, z; };
V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
double dot3(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

struct Frag { std::vector<V3> pts; uint32_t surface; };   // a piece of a surface (all of it, or what's left inside a node)
struct Plane3 { V3 n; double d; };

struct Builder {
    const std::vector<Poly> &polys;
    Bsp &b;
    std::vector<uint32_t> surface_plane;   // each surface's plane designator
    std::vector<Plane3> planes;
    std::string why;
    struct Step { uint32_t plane; bool back; const std::vector<Frag> *facing; };   // an ancestor: its plane, the side taken, its surfaces facing that side
    std::map<std::pair<uint32_t, std::vector<uint32_t>>, uint32_t> tree_cache;   // (plane | back, surfaces) -> bsp2d root
    std::vector<Step> path;
    int max_depth = 0;

    uint32_t plane_index(V3 n, double d, bool &flipped) {
        for (uint32_t i = 0; i < planes.size(); i++) {
            const Plane3 &p = planes[i];
            if (dot3(p.n, n) > 1 - 1e-9 && std::fabs(p.d - d) < kEps) { flipped = false; return i; }
            if (dot3(p.n, n) < -1 + 1e-9 && std::fabs(p.d + d) < kEps) { flipped = true; return i; }
        }
        planes.push_back({n, d});
        flipped = false;
        return (uint32_t)planes.size() - 1;
    }
    // side of a fragment: 1 front, -1 back, 0 on the plane, 2 both. Only a fragment of a surface on the plane is on
    // it: a sliver of another one that happens to lie within kEps of it goes to the side its middle is on.
    int classify(const Frag &f, const Plane3 &p) {
        bool front = false, back = false;
        double mid = 0;
        for (auto &q : f.pts) {
            double d = dot3(p.n, q) - p.d;
            mid += d;
            if (d > kEps) front = true;
            else if (d < -kEps) back = true;
        }
        if (front || back) return front && back ? 2 : front ? 1 : -1;
        // (a surface nearly in the plane, its corners all within kEps, is in it: one leaning ever so slightly beside
        // another would otherwise make slivers of leaves between their planes)
        const Plane3 &own = planes[surface_plane[f.surface] & ~kLeaf];
        if (std::fabs(dot3(own.n, p.n)) > 0.99) return 0;
        return mid >= 0 ? 1 : -1;
    }
    void split(const Frag &f, const Plane3 &p, Frag &fr, Frag &bk) {
        fr.surface = bk.surface = f.surface;
        size_t n = f.pts.size();
        for (size_t i = 0; i < n; i++) {
            V3 a = f.pts[i], c = f.pts[(i + 1) % n];
            double da = dot3(p.n, a) - p.d, dc = dot3(p.n, c) - p.d;
            if (da >= -kEps) fr.pts.push_back(a);
            if (da <= kEps) bk.pts.push_back(a);
            if ((da > kEps && dc < -kEps) || (da < -kEps && dc > kEps)) {
                double t = da / (da - dc);
                V3 m{a.x + (c.x - a.x) * t, a.y + (c.y - a.y) * t, a.z + (c.z - a.z) * t};
                fr.pts.push_back(m);
                bk.pts.push_back(m);
            }
        }
    }

    // ---- bsp2d: the surfaces on one plane, facing one way, seen projected
    struct F2 { std::vector<std::array<double, 2>> pts; uint32_t surface; };
    // A 2D BSP over fragments that don't overlap: every leaf region inside one fragment (its surface) or outside all
    // (none). `used`: the lines already split by on the way here (a ring's edges once used bound the region).
    uint32_t build2(const std::vector<F2> &fs, std::vector<std::array<double, 3>> &used, int depth) {
        if (fs.empty()) return kNone;
        // one fragment: the engine only looks a point up where the leaf's face is, which surfaces cover
        if (fs.size() == 1) return fs[0].surface | kLeaf;
        if (depth > 120) { why = "bsp2d too deep"; return kNone; }
        auto line_of = [](const std::array<double, 2> &a, const std::array<double, 2> &c, std::array<double, 3> &L) {
            double nx = c[1] - a[1], ny = a[0] - c[0], l = std::sqrt(nx * nx + ny * ny);
            if (l < 1e-9) return false;
            L = {nx / l, ny / l, (nx * a[0] + ny * a[1]) / l};
            return true;
        };
        auto is_used = [&](const std::array<double, 3> &L) {
            for (auto &u : used)
                if (std::fabs(u[0] * L[0] + u[1] * L[1]) > 1 - 1e-9 && std::fabs(std::fabs(u[2]) - std::fabs(L[2])) < kEps &&
                    ((u[0] * L[0] + u[1] * L[1] > 0) ? std::fabs(u[2] - L[2]) < kEps : std::fabs(u[2] + L[2]) < kEps))
                    return true;
            return false;
        };
        std::array<double, 3> best{};
        bool have = false;
        long best_score = LONG_MAX;
        for (size_t i = 0; i < fs.size(); i++) {
            auto &p = fs[i].pts;
            for (size_t e = 0; e < p.size(); e++) {
                std::array<double, 3> L;
                if (!line_of(p[e], p[(e + 1) % p.size()], L) || is_used(L)) continue;
                long pos = 0, neg = 0, both = 0;
                for (auto &g : fs) {
                    bool P = false, N = false;
                    for (auto &q : g.pts) {
                        double sd = L[0] * q[0] + L[1] * q[1] - L[2];
                        if (sd > kEps) P = true; else if (sd < -kEps) N = true;
                    }
                    if (P && N) both++; else if (P) pos++; else if (N) neg++;
                }
                // a line with some on each side
                long score = (pos || both) && (neg || both) ? both * 4 + std::labs(pos - neg) : LONG_MAX;
                if (score < best_score) { best_score = score; best = L; have = true; }
            }
        }
        if (!have || best_score == LONG_MAX) { why = "overlapping surfaces"; return kNone; }
        std::vector<F2> pos, neg;
        auto area = [](const F2 &h) {
            double sa = 0;
            for (size_t i = 0; i < h.pts.size(); i++) {
                auto &u = h.pts[i], &w = h.pts[(i + 1) % h.pts.size()];
                sa += u[0] * w[1] - w[0] * u[1];
            }
            return std::fabs(sa) / 2;
        };
        for (auto &g : fs) {
            F2 gp{{}, g.surface}, gn{{}, g.surface};
            size_t n = g.pts.size();
            for (size_t i = 0; i < n; i++) {
                auto q = g.pts[i], r = g.pts[(i + 1) % n];
                double sq = best[0] * q[0] + best[1] * q[1] - best[2], sr = best[0] * r[0] + best[1] * r[1] - best[2];
                if (sq >= -kEps) gp.pts.push_back(q);
                if (sq <= kEps) gn.pts.push_back(q);
                if ((sq > kEps && sr < -kEps) || (sq < -kEps && sr > kEps)) {
                    double t = sq / (sq - sr);
                    std::array<double, 2> m{q[0] + (r[0] - q[0]) * t, q[1] + (r[1] - q[1]) * t};
                    gp.pts.push_back(m);
                    gn.pts.push_back(m);
                }
            }
            if (gp.pts.size() >= 3 && area(gp) > 1e-10) pos.push_back(gp);
            if (gn.pts.size() >= 3 && area(gn) > 1e-10) neg.push_back(gn);
        }
        uint32_t idx = (uint32_t)b.nodes2.size();
        Node2 node;
        node.n[0] = best[0]; node.n[1] = best[1]; node.d = best[2];
        b.nodes2.push_back(node);
        used.push_back(best);
        uint32_t cp = build2(pos, used, depth + 1), cn = build2(neg, used, depth + 1);
        used.pop_back();
        b.nodes2[idx].child[1] = cp;
        b.nodes2[idx].child[0] = cn;
        return idx;
    }
    uint32_t tree2(const std::vector<Frag> &fs, uint32_t plane, bool back) {
        if (fs.empty()) return kNone;
        const Plane3 &p = planes[plane];
        double n[3] = {p.n.x, p.n.y, p.n.z};
        int axis = projection_of(n);
        bool sign = (n[axis] > 0) != back;
        std::vector<F2> f2;
        for (auto &f : fs) {
            F2 g{{}, f.surface};
            for (auto &q : f.pts) {
                double v[3] = {q.x, q.y, q.z}, o[2];
                project(v, axis, sign, o);
                g.pts.push_back({o[0], o[1]});
            }
            f2.push_back(g);
        }
        std::vector<std::array<double, 3>> used;
        return build2(f2, used, 0);
    }

    // How many times the surfaces wind around a point (solid angles over 4 pi): about 1 inside a closed shape, 0
    // outside, and so also right where shapes overlap (2) or a sheet has both faces (0).
    double winding(V3 p) {
        double w = 0;
        for (auto &q : polys) {
            size_t n = q.pts.size();
            V3 a{q.pts[0][0] - p.x, q.pts[0][1] - p.y, q.pts[0][2] - p.z};
            for (size_t k = 1; k + 1 < n; k++) {
                V3 b{q.pts[k][0] - p.x, q.pts[k][1] - p.y, q.pts[k][2] - p.z}, c{q.pts[k + 1][0] - p.x, q.pts[k + 1][1] - p.y, q.pts[k + 1][2] - p.z};
                double la = std::sqrt(dot3(a, a)), lb = std::sqrt(dot3(b, b)), lc = std::sqrt(dot3(c, c));
                double det = a.x * (b.y * c.z - b.z * c.y) - a.y * (b.x * c.z - b.z * c.x) + a.z * (b.x * c.y - b.y * c.x);
                double den = la * lb * lc + dot3(a, b) * lc + dot3(a, c) * lb + dot3(b, c) * la;
                w += 2 * std::atan2(det, den);
            }
        }
        return w / (4 * 3.14159265358979323846);
    }

    // The leaf's face on an ancestor's plane (where it borders that plane): a big square on the plane cut down by every
    // other ancestor's side. Empty if the leaf only touches the plane along an edge or a corner, or not at all.
    std::vector<V3> face_on(size_t step) {
        const Plane3 &P = planes[path[step].plane];
        V3 n = P.n, a = std::fabs(n.x) < 0.9 ? V3{1, 0, 0} : V3{0, 1, 0};
        V3 u{n.y * a.z - n.z * a.y, n.z * a.x - n.x * a.z, n.x * a.y - n.y * a.x};
        double lu = std::sqrt(dot3(u, u));
        u = {u.x / lu, u.y / lu, u.z / lu};
        V3 v{n.y * u.z - n.z * u.y, n.z * u.x - n.x * u.z, n.x * u.y - n.y * u.x};
        V3 o{n.x * P.d, n.y * P.d, n.z * P.d};
        const double R = 1e4;
        std::vector<V3> poly;
        for (int k = 0; k < 4; k++) {
            double su = (k == 0 || k == 3) ? -R : R, sv = k < 2 ? -R : R;
            poly.push_back({o.x + u.x * su + v.x * sv, o.y + u.y * su + v.y * sv, o.z + u.z * su + v.z * sv});
        }
        for (size_t i = 0; i < path.size() && poly.size() >= 3; i++) {
            if (i == step || path[i].plane == path[step].plane) continue;
            const Plane3 &Q = planes[path[i].plane];
            double sgn = path[i].back ? -1 : 1;
            std::vector<V3> out;
            for (size_t k = 0; k < poly.size(); k++) {
                V3 p0 = poly[k], p1 = poly[(k + 1) % poly.size()];
                double d0 = sgn * (dot3(Q.n, p0) - Q.d), d1 = sgn * (dot3(Q.n, p1) - Q.d);
                if (d0 >= 0) out.push_back(p0);
                if ((d0 > 0 && d1 < 0) || (d0 < 0 && d1 > 0)) {
                    double t = d0 / (d0 - d1);
                    out.push_back({p0.x + (p1.x - p0.x) * t, p0.y + (p1.y - p0.y) * t, p0.z + (p1.z - p0.z) * t});
                }
            }
            poly.swap(out);
        }
        if (poly.size() < 3) return {};
        V3 cr{0, 0, 0};
        for (size_t k = 1; k + 1 < poly.size(); k++) {
            V3 e1 = sub(poly[k], poly[0]), e2 = sub(poly[k + 1], poly[0]);
            cr.x += e1.y * e2.z - e1.z * e2.y; cr.y += e1.z * e2.x - e1.x * e2.z; cr.z += e1.x * e2.y - e1.y * e2.x;
        }
        if (std::sqrt(dot3(cr, cr)) / 2 < 1e-10) return {};
        return poly;
    }
    // Do two convex polygons on one plane overlap (more than touching)? Separating axes in the plane.
    static bool overlap(const std::vector<V3> &A, const std::vector<V3> &B, V3 n) {
        for (auto *P : {&A, &B})
            for (size_t k = 0; k < P->size(); k++) {
                V3 e = sub((*P)[(k + 1) % P->size()], (*P)[k]);
                V3 ax{n.y * e.z - n.z * e.y, n.z * e.x - n.x * e.z, n.x * e.y - n.y * e.x};
                double l = std::sqrt(dot3(ax, ax));
                if (l < 1e-12) continue;
                ax = {ax.x / l, ax.y / l, ax.z / l};
                double a0 = 1e30, a1 = -1e30, b0 = 1e30, b1 = -1e30;
                for (auto &q : A) { double d = dot3(ax, q); a0 = std::min(a0, d); a1 = std::max(a1, d); }
                for (auto &q : B) { double d = dot3(ax, q); b0 = std::min(b0, d); b1 = std::max(b1, d); }
                if (a1 < b0 + 1e-6 || b1 < a0 + 1e-6) return false;
            }
        return true;
    }

    // A leaf: for each plane it has a face on where surfaces face it, those surfaces that cover some of that face (as
    // Bungie's tool: only what a point crossing out of the leaf there could land on).
    uint32_t leaf() {
        Leaf l;
        l.first = (uint32_t)b.refs.size();
        for (size_t i = 0; i < path.size(); i++) {
            const Step &s = path[i];
            if (!s.facing || s.facing->empty()) continue;
            auto face = face_on(i);
            if (face.empty()) continue;
            std::vector<Frag> some;
            for (auto &f : *s.facing)
                if (overlap(face, f.pts, planes[s.plane].n)) some.push_back(f);
            if (some.empty()) continue;
            std::vector<uint32_t> key;
            for (auto &f : some) key.push_back(f.surface);
            std::sort(key.begin(), key.end());
            auto ck = std::make_pair(s.plane | (s.back ? kLeaf : 0u), key);
            auto it = tree_cache.find(ck);
            uint32_t root = it != tree_cache.end() ? it->second : (tree_cache[ck] = tree2(some, s.plane, s.back));
            if (root != kNone) b.refs.push_back({s.plane | (s.back ? kLeaf : 0u), root});
        }
        l.refs = (uint16_t)(b.refs.size() - l.first);
        if (!l.refs) l.first = kNone;
        b.leaves.push_back(l);
        return (uint32_t)(b.leaves.size() - 1) | kLeaf;
    }

    uint32_t build3(std::vector<Frag> fs, int depth) {
        max_depth = std::max(max_depth, depth);
        if (depth > 120) { why = "bsp too deep"; return kNone; }
        // splitter: the plane of a fragment, or an axis-aligned plane through the middle of them (a rounded shape's
        // own planes each have everything behind them: alone they'd make a BSP as deep as it has surfaces), whichever
        // splits the fewest and divides them most evenly
        uint32_t best = kNone;
        long best_score = LONG_MAX;
        Plane3 best_axis{};
        bool axis = false;
        std::set<uint32_t> tried;
        auto score_of = [&](const Plane3 &pl, bool need_both) {
            long front = 0, back = 0, spans = 0, on = 0;
            for (auto &g : fs) {
                int c = classify(g, pl);
                if (c == 2) spans++; else if (c == 1) front++; else if (c == -1) back++; else on++;
            }
            if (need_both && (front + spans == 0 || back + spans == 0)) return LONG_MAX;
            return spans * 8 + std::labs(front - back);
        };
        size_t stride = std::max<size_t>(1, fs.size() / 48);
        for (size_t i = 0; i < fs.size(); i += stride) {
            uint32_t pl = surface_plane[fs[i].surface] & ~kLeaf;
            if (!tried.insert(pl).second) continue;
            long sc = score_of(planes[pl], false);
            if (sc < best_score) { best_score = sc; best = pl; axis = false; }
        }
        if (fs.size() > 4) {
            std::vector<double> cs[3];
            for (auto &g : fs) {
                V3 m{0, 0, 0};
                for (auto &q : g.pts) { m.x += q.x / g.pts.size(); m.y += q.y / g.pts.size(); m.z += q.z / g.pts.size(); }
                cs[0].push_back(m.x); cs[1].push_back(m.y); cs[2].push_back(m.z);
            }
            for (int k = 0; k < 3; k++) {
                std::nth_element(cs[k].begin(), cs[k].begin() + cs[k].size() / 2, cs[k].end());
                Plane3 pl{{k == 0 ? 1.0 : 0.0, k == 1 ? 1.0 : 0.0, k == 2 ? 1.0 : 0.0}, cs[k][cs[k].size() / 2]};
                long sc = score_of(pl, true);
                if (sc < best_score) { best_score = sc; best_axis = pl; axis = true; }
            }
        }
        if (axis) {
            bool flipped;
            best = plane_index(best_axis.n, best_axis.d, flipped);
        }
        const Plane3 P = planes[best];
        std::vector<Frag> front, back, on_front, on_back;
        for (auto &g : fs) {
            int c = classify(g, P);
            if (c == 0) {
                bool flipped = (surface_plane[g.surface] & ~kLeaf) != best || (surface_plane[g.surface] & kLeaf);
                if ((surface_plane[g.surface] & ~kLeaf) == best) flipped = (surface_plane[g.surface] & kLeaf) != 0;
                else {   // coplanar with a different plane index (within kEps): face by normals
                    const Plane3 &q = planes[surface_plane[g.surface] & ~kLeaf];
                    flipped = (dot3(q.n, P.n) < 0) != ((surface_plane[g.surface] & kLeaf) != 0);
                }
                (flipped ? on_back : on_front).push_back(g);
            } else if (c == 1) front.push_back(g);
            else if (c == -1) back.push_back(g);
            else {
                Frag fr, bk;
                split(g, P, fr, bk);
                if (fr.pts.size() >= 3) front.push_back(fr);
                if (bk.pts.size() >= 3) back.push_back(bk);
            }
        }
        uint32_t idx = (uint32_t)b.nodes.size();
        b.nodes.push_back({best, {kNone, kNone}});
        for (int side = 1; side >= 0; side--) {
            auto &list = side ? front : back;
            path.push_back({best, side == 0, side ? &on_front : &on_back});
            uint32_t child;
            if (!list.empty()) child = build3(list, depth + 1);
            else {
                // nothing more on this side: one region, solid or empty throughout. Usually empty if the surfaces
                // here face it and solid if they face away; with surfaces facing both ways (shapes that overlap, or a
                // sheet), whatever the surfaces wind around a point just off the biggest of them on this side.
                bool facing = side ? !on_front.empty() : !on_back.empty(), away = side ? !on_back.empty() : !on_front.empty();
                bool empty_side = facing;
                if (facing && away) {
                    const Frag *big = nullptr;
                    double big_area = -1;
                    for (auto *lst : {&on_front, &on_back})
                        for (auto &g : *lst) {
                            V3 cr{0, 0, 0};
                            for (size_t k = 1; k + 1 < g.pts.size(); k++) {
                                V3 u = sub(g.pts[k], g.pts[0]), w = sub(g.pts[k + 1], g.pts[0]);
                                cr.x += u.y * w.z - u.z * w.y; cr.y += u.z * w.x - u.x * w.z; cr.z += u.x * w.y - u.y * w.x;
                            }
                            double ar = std::sqrt(dot3(cr, cr));
                            if (ar > big_area) { big_area = ar; big = &g; }
                        }
                    V3 c{0, 0, 0};
                    for (auto &q : big->pts) { c.x += q.x / big->pts.size(); c.y += q.y / big->pts.size(); c.z += q.z / big->pts.size(); }
                    double off = side ? 1e-3 : -1e-3;
                    empty_side = winding({c.x + P.n.x * off, c.y + P.n.y * off, c.z + P.n.z * off}) < 0.5;
                }
                child = empty_side ? leaf() : kNone;
            }
            path.pop_back();
            b.nodes[idx].child[side] = child;
        }
        return idx;
    }
};

}   // namespace

bool build(const std::vector<Poly> &polys, Bsp &out, std::string *why) {
    out = Bsp();
    if (polys.empty()) { if (why) *why = "no surfaces"; return false; }
    Builder bd{polys, out};
    // vertices (shared where equal), surfaces, their planes
    std::map<std::array<double, 3>, uint32_t> vid;
    std::vector<std::vector<uint32_t>> rings;
    std::vector<Frag> frags;
    for (uint32_t s = 0; s < polys.size(); s++) {
        const Poly &p = polys[s];
        if (p.pts.size() < 3 || p.pts.size() > 8) { if (why) *why = "a surface with " + std::to_string(p.pts.size()) + " corners"; return false; }
        // facing: the ring's Newell normal (Bungie's rings run counterclockwise seen from the front)
        V3 n{0, 0, 0}, c{0, 0, 0};
        std::vector<uint32_t> ring;
        Frag f;
        f.surface = s;
        for (size_t k = 0; k < p.pts.size(); k++) {
            auto &a = p.pts[k], &z = p.pts[(k + 1) % p.pts.size()];
            n.x += (a[1] - z[1]) * (a[2] + z[2]);
            n.y += (a[2] - z[2]) * (a[0] + z[0]);
            n.z += (a[0] - z[0]) * (a[1] + z[1]);
            c.x += a[0] / p.pts.size(); c.y += a[1] / p.pts.size(); c.z += a[2] / p.pts.size();
            auto it = vid.find(a);
            if (it == vid.end()) {
                it = vid.emplace(a, (uint32_t)out.vertices.size()).first;
                Vertex v;
                v.p[0] = a[0]; v.p[1] = a[1]; v.p[2] = a[2];
                out.vertices.push_back(v);
            }
            ring.push_back(it->second);
            f.pts.push_back({a[0], a[1], a[2]});
        }
        double l = std::sqrt(dot3(n, n));
        if (l < 1e-12) { if (why) *why = "a surface with no area"; return false; }
        n = {n.x / l, n.y / l, n.z / l};
        bool flipped = false;
        uint32_t pl = bd.plane_index(n, dot3(n, c), flipped);
        bd.surface_plane.push_back(pl | (flipped ? kLeaf : 0u));
        Surface sf;
        sf.plane = bd.surface_plane.back();
        sf.flags = p.flags;
        sf.breakable = p.breakable;
        sf.material = p.material;
        out.surfaces.push_back(sf);
        rings.push_back(ring);
        frags.push_back(f);
    }
    // winged edges: each pair of corners once; the surface it was first met in is its left. (Where two solids touch
    // along an edge, four surfaces meet there: each pairs with one of its own group.)
    std::map<std::tuple<int, uint32_t, uint32_t>, uint32_t> directed;   // (group, a, b) -> edge whose left runs a -> b
    std::vector<std::vector<uint32_t>> ring_edges(polys.size());
    for (uint32_t s = 0; s < rings.size(); s++) {
        auto &r = rings[s];
        int g = polys[s].group;
        for (size_t k = 0; k < r.size(); k++) {
            uint32_t a = r[k], z = r[(k + 1) % r.size()];
            auto rev = directed.find({g, z, a});
            if (rev != directed.end() && out.edges[rev->second].s[1] == kNone) {
                out.edges[rev->second].s[1] = s;
                ring_edges[s].push_back(rev->second);
            } else {
                Edge e;
                e.v[0] = a; e.v[1] = z;
                e.s[0] = s; e.s[1] = kNone;
                directed[{g, a, z}] = (uint32_t)out.edges.size();
                ring_edges[s].push_back((uint32_t)out.edges.size());
                out.edges.push_back(e);
            }
        }
    }
    for (uint32_t s = 0; s < rings.size(); s++) {
        auto &re = ring_edges[s];
        for (size_t k = 0; k < re.size(); k++) {
            Edge &e = out.edges[re[k]];
            e.next[e.s[1] == s ? 1 : 0] = re[(k + 1) % re.size()];
        }
        out.surfaces[s].first_edge = re[0];
    }
    for (uint32_t e = 0; e < out.edges.size(); e++) {
        if (out.edges[e].s[1] == kNone) { if (why) *why = "the surfaces don't close (an open edge)"; return false; }
        for (int k = 0; k < 2; k++) out.vertices[out.edges[e].v[k]].first_edge = e;
    }
    for (auto &p : bd.planes) out.planes.push_back({{p.n.x, p.n.y, p.n.z}, p.d});
    bd.build3(frags, 0);
    if (!bd.why.empty()) { if (why) *why = bd.why; return false; }
    out.planes.clear();
    for (auto &p : bd.planes) out.planes.push_back({{p.n.x, p.n.y, p.n.z}, p.d});
    // as a tag keeps them (floats), so what's tested is what the game gets
    for (auto &p : out.planes) {
        for (auto &x : p.n) x = (float)x;
        p.d = (float)p.d;
    }
    for (auto &n : out.nodes2) { n.n[0] = (float)n.n[0]; n.n[1] = (float)n.n[1]; n.d = (float)n.d; }
    for (auto &v : out.vertices)
        for (auto &x : v.p) x = (float)x;
    return true;
}

int worst_features(const Bsp &b, double radius, long *worst_ops) {
    int worst = 0;
    if (worst_ops) *worst_ops = 0;
    std::set<std::array<long, 3>> seen;   // (a sphere where another just as near already was gathers the same)
    for (uint32_t s = 0; s < b.surfaces.size(); s++) {
        auto ring = surface_ring(b, s);
        if (ring.empty()) continue;
        double m[3] = {0, 0, 0};
        for (auto v : ring)
            for (int k = 0; k < 3; k++) m[k] += b.vertices[v].p[k] / ring.size();
        const Plane &pl = b.planes[b.surfaces[s].plane & ~kLeaf];
        double f = (b.surfaces[s].plane & kLeaf) ? -1 : 1, off = radius * 0.6;
        double c[3] = {m[0] + pl.n[0] * f * off, m[1] + pl.n[1] * f * off, m[2] + pl.n[2] * f * off};
        const double q = radius * 0.15;
        if (!seen.insert({std::lround(c[0] / q), std::lround(c[1] / q), std::lround(c[2] / q)}).second) continue;
        Features ft = test_sphere(b, c, radius);
        worst = std::max(worst, ft.surfaces + ft.edges + ft.vertices);
        if (worst_ops) *worst_ops = std::max(*worst_ops, ft.ops);
    }
    return worst;
}

namespace {
// distance from p to a convex polygon (squared)
double poly_dist2(const Poly &q, V3 p) {
    size_t n = q.pts.size();
    V3 a{q.pts[0][0], q.pts[0][1], q.pts[0][2]};
    V3 nn{0, 0, 0};
    for (size_t k = 0; k < n; k++) {
        auto &u = q.pts[k], &w = q.pts[(k + 1) % n];
        nn.x += (u[1] - w[1]) * (u[2] + w[2]);
        nn.y += (u[2] - w[2]) * (u[0] + w[0]);
        nn.z += (u[0] - w[0]) * (u[1] + w[1]);
    }
    double l = std::sqrt(dot3(nn, nn));
    if (l > 0) nn = {nn.x / l, nn.y / l, nn.z / l};
    double dp = dot3(nn, sub(p, a));
    V3 proj{p.x - nn.x * dp, p.y - nn.y * dp, p.z - nn.z * dp};
    bool inside = true;
    for (size_t k = 0; k < n && inside; k++) {
        V3 u{q.pts[k][0], q.pts[k][1], q.pts[k][2]}, w{q.pts[(k + 1) % n][0], q.pts[(k + 1) % n][1], q.pts[(k + 1) % n][2]};
        V3 e = sub(w, u), r = sub(proj, u);
        V3 cr{e.y * r.z - e.z * r.y, e.z * r.x - e.x * r.z, e.x * r.y - e.y * r.x};
        inside = dot3(cr, nn) >= 0;
    }
    if (inside) return dp * dp;
    double best = 1e30;
    for (size_t k = 0; k < n; k++) {
        V3 u{q.pts[k][0], q.pts[k][1], q.pts[k][2]}, w{q.pts[(k + 1) % n][0], q.pts[(k + 1) % n][1], q.pts[(k + 1) % n][2]};
        V3 e = sub(w, u), r = sub(p, u);
        double ee = dot3(e, e), t = ee > 0 ? std::min(1.0, std::max(0.0, dot3(r, e) / ee)) : 0;
        V3 c{u.x + e.x * t - p.x, u.y + e.y * t - p.y, u.z + e.z * t - p.z};
        best = std::min(best, dot3(c, c));
    }
    return best;
}
bool solid_at(const Bsp &b, V3 p) {   // bsp3d_test_point: NONE is solid
    uint32_t n = 0;
    for (int d = 0; d < 256 && !(n & kLeaf); d++) {
        if (n >= b.nodes.size()) return false;
        const Node3 &x = b.nodes[n];
        const Plane &pl = b.planes[x.plane];
        n = x.child[pl.n[0] * p.x + pl.n[1] * p.y + pl.n[2] * p.z - pl.d >= 0];
    }
    return n == kNone;
}

// Squared distance transform (Felzenszwalb & Huttenlocher) of a grid, in cells: 0 at the cells of the set, else the
// squared distance to the nearest one.
std::vector<float> distance_transform(const std::vector<char> &set, int nx, int ny, int nz) {
    const double kFar = 1e18;
    std::vector<float> d(set.size());
    for (size_t i = 0; i < set.size(); i++) d[i] = set[i] ? 0.f : (float)kFar;
    int n = std::max({nx, ny, nz});
    std::vector<double> f(n), out(n), z(n + 1);
    std::vector<int> v(n);
    auto pass = [&](int len) {
        int k = 0;
        v[0] = 0;
        z[0] = -1e30;
        z[1] = 1e30;
        for (int q = 1; q < len; q++) {
            double s = ((f[q] + (double)q * q) - (f[v[k]] + (double)v[k] * v[k])) / (2.0 * q - 2.0 * v[k]);
            while (s <= z[k]) {
                k--;
                s = ((f[q] + (double)q * q) - (f[v[k]] + (double)v[k] * v[k])) / (2.0 * q - 2.0 * v[k]);
            }
            k++;
            v[k] = q;
            z[k] = s;
            z[k + 1] = 1e30;
        }
        k = 0;
        for (int q = 0; q < len; q++) {
            while (z[k + 1] < q) k++;
            out[q] = (double)(q - v[k]) * (q - v[k]) + f[v[k]];
        }
    };
    for (int axis = 0; axis < 3; axis++) {
        int len = axis == 0 ? nx : axis == 1 ? ny : nz;
        size_t stride = axis == 0 ? 1 : axis == 1 ? (size_t)nx : (size_t)nx * ny;
        int a = axis == 0 ? ny : nx, bcount = axis == 2 ? ny : nz;
        for (int p = 0; p < a; p++)
            for (int q2 = 0; q2 < bcount; q2++) {
                size_t base = axis == 0 ? ((size_t)q2 * ny + p) * nx : axis == 1 ? ((size_t)q2 * ny) * nx + p : (size_t)q2 * nx + p;
                for (int i = 0; i < len; i++) f[i] = std::min((double)d[base + i * stride], kFar);
                pass(len);
                for (int i = 0; i < len; i++) d[base + i * stride] = (float)std::min(out[i], kFar);
            }
    }
    return d;
}

// distances from points spread over a's surfaces to b's (sorted); about `samples` points, by area
std::vector<double> spread_distances(const std::vector<Poly> &a, const std::vector<Poly> &b, int samples) {
    std::vector<double> areas;
    double total = 0;
    for (auto &q : a) {
        double ar = 0;
        for (size_t k = 1; k + 1 < q.pts.size(); k++) {
            V3 u{q.pts[k][0] - q.pts[0][0], q.pts[k][1] - q.pts[0][1], q.pts[k][2] - q.pts[0][2]};
            V3 w{q.pts[k + 1][0] - q.pts[0][0], q.pts[k + 1][1] - q.pts[0][1], q.pts[k + 1][2] - q.pts[0][2]};
            V3 cr{u.y * w.z - u.z * w.y, u.z * w.x - u.x * w.z, u.x * w.y - u.y * w.x};
            ar += std::sqrt(dot3(cr, cr)) / 2;
        }
        areas.push_back(ar);
        total += ar;
    }
    std::vector<std::array<double, 6>> box;
    for (auto &q : b) {
        std::array<double, 6> bb{1e30, 1e30, 1e30, -1e30, -1e30, -1e30};
        for (auto &p : q.pts)
            for (int c = 0; c < 3; c++) { bb[c] = std::min(bb[c], p[c]); bb[c + 3] = std::max(bb[c + 3], p[c]); }
        box.push_back(bb);
    }
    uint32_t x = 777;
    auto rnd = [&]() { x = x * 1664525u + 1013904223u; return (x >> 8) / 16777216.0; };
    std::vector<double> out;
    for (size_t i = 0; i < a.size() && total > 0; i++) {
        int n = std::max(1, (int)(samples * areas[i] / total));
        auto &q = a[i];
        for (int s = 0; s < n; s++) {
            size_t k = 1 + std::min(q.pts.size() - 3, (size_t)(rnd() * (q.pts.size() - 2)));
            double r1 = rnd(), r2 = rnd();
            if (r1 + r2 > 1) { r1 = 1 - r1; r2 = 1 - r2; }
            V3 p{q.pts[0][0] + (q.pts[k][0] - q.pts[0][0]) * r1 + (q.pts[k + 1][0] - q.pts[0][0]) * r2,
                 q.pts[0][1] + (q.pts[k][1] - q.pts[0][1]) * r1 + (q.pts[k + 1][1] - q.pts[0][1]) * r2,
                 q.pts[0][2] + (q.pts[k][2] - q.pts[0][2]) * r1 + (q.pts[k + 1][2] - q.pts[0][2]) * r2};
            double best = 1e30;
            for (size_t o = 0; o < b.size(); o++) {
                auto &bb = box[o];
                double dx = std::max({bb[0] - p.x, 0.0, p.x - bb[3]}), dy = std::max({bb[1] - p.y, 0.0, p.y - bb[4]}),
                       dz = std::max({bb[2] - p.z, 0.0, p.z - bb[5]});
                if (dx * dx + dy * dy + dz * dz >= best) continue;
                best = std::min(best, poly_dist2(b[o], p));
            }
            out.push_back(std::sqrt(best));
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// How thin a shape is: from points spread over its surfaces, the distance straight in to the surface on the other side.
std::vector<double> thicknesses(const Bsp &b, int samples) {
    auto polys = polygons(b);
    std::vector<double> out;
    double total = 0;
    std::vector<double> areas;
    for (auto &q : polys) {
        double ar = 0;
        for (size_t k = 1; k + 1 < q.pts.size(); k++) {
            V3 u{q.pts[k][0] - q.pts[0][0], q.pts[k][1] - q.pts[0][1], q.pts[k][2] - q.pts[0][2]};
            V3 w{q.pts[k + 1][0] - q.pts[0][0], q.pts[k + 1][1] - q.pts[0][1], q.pts[k + 1][2] - q.pts[0][2]};
            V3 cr{u.y * w.z - u.z * w.y, u.z * w.x - u.x * w.z, u.x * w.y - u.y * w.x};
            ar += std::sqrt(dot3(cr, cr)) / 2;
        }
        areas.push_back(ar);
        total += ar;
    }
    uint32_t x = 99;
    auto rnd = [&]() { x = x * 1664525u + 1013904223u; return (x >> 8) / 16777216.0; };
    for (size_t i = 0; i < polys.size() && total > 0; i++) {
        auto &q = polys[i];
        V3 nn{0, 0, 0};
        for (size_t k = 0; k < q.pts.size(); k++) {
            auto &u = q.pts[k], &w = q.pts[(k + 1) % q.pts.size()];
            nn.x += (u[1] - w[1]) * (u[2] + w[2]);
            nn.y += (u[2] - w[2]) * (u[0] + w[0]);
            nn.z += (u[0] - w[0]) * (u[1] + w[1]);
        }
        double l = std::sqrt(dot3(nn, nn));
        if (l <= 0) continue;
        nn = {nn.x / l, nn.y / l, nn.z / l};
        int n = std::max(1, (int)(samples * areas[i] / total));
        for (int s = 0; s < n; s++) {
            size_t k = 1 + std::min(q.pts.size() - 3, (size_t)(rnd() * (q.pts.size() - 2)));
            double r1 = rnd(), r2 = rnd();
            if (r1 + r2 > 1) { r1 = 1 - r1; r2 = 1 - r2; }
            double p[3], v[3];
            for (int c = 0; c < 3; c++) {
                double nc = c == 0 ? nn.x : c == 1 ? nn.y : nn.z;
                p[c] = q.pts[0][c] + (q.pts[k][c] - q.pts[0][c]) * r1 + (q.pts[k + 1][c] - q.pts[0][c]) * r2 - nc * 1e-4;
                v[c] = -nc * 2;
            }
            // the first back face going in (the far side, seen from inside)
            Hit h = test_vector(b, p, v, 1, true);
            if (h.surface != kNone) out.push_back(h.t * 2 + 1e-4);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}
// ---- convex pieces: a dense shape made again the way you'd model collision by hand

V3 cross3(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 unit3(V3 a) {
    double l = std::sqrt(dot3(a, a));
    return l > 0 ? V3{a.x / l, a.y / l, a.z / l} : a;
}
double coord(V3 p, int axis) { return axis == 0 ? p.x : axis == 1 ? p.y : p.z; }
void set_coord(V3 &p, int axis, double v) { (axis == 0 ? p.x : axis == 1 ? p.y : p.z) = v; }

// Positions and plane offsets snapped to a fine grid (a power of two, so exactly), and normals likewise, so pieces
// that meet along the same face of the model meet exactly, not a hair apart (which leaves slivers).
const double kSnap = 1.0 / 2048;   // (0.15 mm)
double snap_down(double x) { return std::floor(x / kSnap) * kSnap; }
double snap_up(double x) { return std::ceil(x / kSnap) * kSnap; }
V3 snap_normal(V3 n) {
    const double q = 1.0 / (1 << 20);
    V3 m{std::round(n.x / q) * q, std::round(n.y / q) * q, std::round(n.z / q) * q};
    return unit3(m);
}


struct Hull {
    std::vector<V3> pts;                    // its corners
    std::vector<std::array<int, 3>> tris;   // counterclockwise seen from outside
    double volume() const {
        double v = 0;
        for (auto &t : tris) v += dot3(pts[t[0]], cross3(pts[t[1]], pts[t[2]]));
        return v / 6;
    }
    double area() const {
        double a = 0;
        for (auto &t : tris) a += std::sqrt(dot3(cross3(sub(pts[t[1]], pts[t[0]]), sub(pts[t[2]], pts[t[0]])), cross3(sub(pts[t[1]], pts[t[0]]), sub(pts[t[2]], pts[t[0]])))) / 2;
        return a;
    }
};

// The convex hull of points (incremental). False if they're flat (or fewer than four apart): then *flat, if given, is
// the normal of the plane they're on when three are apart (else zero).
bool convex_hull(const std::vector<V3> &in, double eps, Hull &h, V3 *flat = nullptr) {
    h = Hull();
    if (flat) *flat = {0, 0, 0};
    std::vector<V3> P;
    {
        std::map<std::array<long long, 3>, int> seen;
        for (auto &p : in)
            if (seen.emplace(std::array<long long, 3>{std::llround(p.x / eps), std::llround(p.y / eps), std::llround(p.z / eps)}, 0).second) P.push_back(p);
    }
    if (P.size() < 3) return false;
    int i0 = 0, i1 = 0;
    double ext = -1;
    for (int ax = 0; ax < 3; ax++) {
        int a = 0, b = 0;
        for (int i = 0; i < (int)P.size(); i++) {
            if (coord(P[i], ax) < coord(P[a], ax)) a = i;
            if (coord(P[i], ax) > coord(P[b], ax)) b = i;
        }
        if (coord(P[b], ax) - coord(P[a], ax) > ext) { ext = coord(P[b], ax) - coord(P[a], ax); i0 = a; i1 = b; }
    }
    if (ext < eps) return false;
    V3 d = unit3(sub(P[i1], P[i0]));
    int i2 = -1;
    double bd = eps;
    for (int i = 0; i < (int)P.size(); i++) {
        V3 c = cross3(sub(P[i], P[i0]), d);
        double l = std::sqrt(dot3(c, c));
        if (l > bd) { bd = l; i2 = i; }
    }
    if (i2 < 0) return false;
    V3 n = unit3(cross3(sub(P[i1], P[i0]), sub(P[i2], P[i0])));
    if (flat) *flat = n;
    int i3 = -1;
    bd = eps;
    for (int i = 0; i < (int)P.size(); i++) {
        double l = std::fabs(dot3(n, sub(P[i], P[i0])));
        if (l > bd) { bd = l; i3 = i; }
    }
    if (i3 < 0) return false;
    struct Face { int v[3]; V3 n; double d; bool alive; std::vector<int> out; };
    std::vector<Face> F;
    auto make = [&](int a, int b, int c) {
        Face f;
        f.v[0] = a; f.v[1] = b; f.v[2] = c;
        f.n = unit3(cross3(sub(P[b], P[a]), sub(P[c], P[a])));
        f.d = dot3(f.n, P[a]);
        f.alive = true;
        return f;
    };
    V3 C{(P[i0].x + P[i1].x + P[i2].x + P[i3].x) / 4, (P[i0].y + P[i1].y + P[i2].y + P[i3].y) / 4, (P[i0].z + P[i1].z + P[i2].z + P[i3].z) / 4};
    int tv[4][3] = {{i0, i1, i2}, {i0, i3, i1}, {i1, i3, i2}, {i2, i3, i0}};
    for (auto &t : tv) {
        Face f = make(t[0], t[1], t[2]);
        if (dot3(f.n, C) - f.d > 0) f = make(t[0], t[2], t[1]);
        F.push_back(f);
    }
    for (int i = 0; i < (int)P.size(); i++) {
        if (i == i0 || i == i1 || i == i2 || i == i3) continue;
        for (auto &f : F)
            if (dot3(f.n, P[i]) - f.d > eps) { f.out.push_back(i); break; }
    }
    for (int guard = 0; guard < 100000; guard++) {
        int fi = -1;
        for (int i = 0; i < (int)F.size() && fi < 0; i++)
            if (F[i].alive && !F[i].out.empty()) fi = i;
        if (fi < 0) break;
        int p = F[fi].out[0];
        double far = -1;
        for (int q : F[fi].out) {
            double dd = dot3(F[fi].n, P[q]) - F[fi].d;
            if (dd > far) { far = dd; p = q; }
        }
        std::vector<int> vis;
        for (int i = 0; i < (int)F.size(); i++)
            if (F[i].alive && dot3(F[i].n, P[p]) - F[i].d > eps) vis.push_back(i);
        std::set<std::pair<int, int>> edges;
        for (int i : vis)
            for (int k = 0; k < 3; k++) edges.insert({F[i].v[k], F[i].v[(k + 1) % 3]});
        std::vector<int> orphans;
        for (int i : vis) {
            F[i].alive = false;
            for (int q : F[i].out)
                if (q != p) orphans.push_back(q);
            F[i].out.clear();
        }
        size_t first_new = F.size();
        for (auto &e : edges)
            if (!edges.count({e.second, e.first})) F.push_back(make(e.first, e.second, p));
        for (int q : orphans)
            for (size_t i = first_new; i < F.size(); i++)
                if (dot3(F[i].n, P[q]) - F[i].d > eps) { F[i].out.push_back(q); break; }
    }
    std::vector<int> remap(P.size(), -1);
    for (auto &f : F) {
        if (!f.alive) continue;
        std::array<int, 3> t;
        for (int k = 0; k < 3; k++) {
            if (remap[f.v[k]] < 0) { remap[f.v[k]] = (int)h.pts.size(); h.pts.push_back(P[f.v[k]]); }
            t[k] = remap[f.v[k]];
        }
        h.tris.push_back(t);
    }
    return h.tris.size() >= 4;
}

// how wide a hull is across, at its narrowest (over its faces' normals), and which way
double hull_width(const Hull &h, V3 &dir) {
    double best = 1e30;
    for (auto &t : h.tris) {
        V3 n = unit3(cross3(sub(h.pts[t[1]], h.pts[t[0]]), sub(h.pts[t[2]], h.pts[t[0]])));
        double lo = 1e30, hi = -1e30;
        for (auto &p : h.pts) { double d = dot3(n, p); lo = std::min(lo, d); hi = std::max(hi, d); }
        if (hi - lo < best) { best = hi - lo; dir = n; }
    }
    return best;
}

// A polygon clipped to one side of an axis plane (keep: coordinate <= at if !above, >= at if above); points made on
// the plane get exactly that coordinate.
std::vector<V3> clip_axis(const std::vector<V3> &poly, int axis, double at, bool above) {
    std::vector<V3> out;
    size_t n = poly.size();
    for (size_t i = 0; i < n; i++) {
        V3 a = poly[i], b = poly[(i + 1) % n];
        double da = above ? coord(a, axis) - at : at - coord(a, axis), db = above ? coord(b, axis) - at : at - coord(b, axis);
        if (da >= 0) out.push_back(a);
        if ((da > 0 && db < 0) || (da < 0 && db > 0)) {
            // (the same point whichever side is kept: computed from the edge's ends in a fixed order)
            V3 p = a, q = b;
            if (coord(q, axis) < coord(p, axis) || (coord(q, axis) == coord(p, axis) && (q.x < p.x || (q.x == p.x && q.y < p.y)))) std::swap(p, q);
            double t = (at - coord(p, axis)) / (coord(q, axis) - coord(p, axis));
            V3 m{p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t, p.z + (q.z - p.z) * t};
            set_coord(m, axis, at);
            out.push_back(m);
        }
    }
    return out.size() >= 3 ? out : std::vector<V3>{};
}

struct Box { V3 lo, hi; };

std::vector<std::vector<V3>> clip_polys(const std::vector<std::vector<V3>> &polys, int axis, double at, bool above) {
    std::vector<std::vector<V3>> out;
    for (auto &p : polys) {
        auto c = clip_axis(p, axis, at, above);
        if (!c.empty()) out.push_back(std::move(c));
    }
    return out;
}

std::vector<V3> points_of(const std::vector<std::vector<V3>> &polys) {
    std::vector<V3> out;
    for (auto &p : polys) out.insert(out.end(), p.begin(), p.end());
    return out;
}

// The solid's volume in a box, from a grid of samples (prefix sums).
struct SolidGrid {
    V3 lo{0, 0, 0};
    double h = 1;
    int nx = 0, ny = 0, nz = 0;
    std::vector<int> sum;   // (nx+1)(ny+1)(nz+1)
    std::vector<float> gap; // each cell's distance to the nearest solid one (0 in the solid)
    int S(int i, int j, int k) const { return sum[((size_t)k * (ny + 1) + j) * (nx + 1) + i]; }
    SolidGrid(const Bsp &b, V3 a, V3 z, double cell) : lo(a), h(cell) {
        nx = std::max(1, (int)std::ceil((z.x - a.x) / h));
        ny = std::max(1, (int)std::ceil((z.y - a.y) / h));
        nz = std::max(1, (int)std::ceil((z.z - a.z) / h));
        sum.assign((size_t)(nx + 1) * (ny + 1) * (nz + 1), 0);
        for (int k = 0; k < nz; k++)
            for (int j = 0; j < ny; j++)
                for (int i = 0; i < nx; i++) {
                    int s = solid_at(b, {lo.x + (i + 0.5) * h, lo.y + (j + 0.5) * h, lo.z + (k + 0.5) * h});
                    sum[((size_t)(k + 1) * (ny + 1) + j + 1) * (nx + 1) + i + 1] =
                        s + S(i, j + 1, k + 1) + S(i + 1, j, k + 1) + S(i + 1, j + 1, k) - S(i, j, k + 1) - S(i, j + 1, k) - S(i + 1, j, k) + S(i, j, k);
                }
        std::vector<char> solid((size_t)nx * ny * nz);
        for (int k = 0; k < nz; k++)
            for (int j = 0; j < ny; j++)
                for (int i = 0; i < nx; i++) {
                    int c = S(i + 1, j + 1, k + 1) - S(i, j + 1, k + 1) - S(i + 1, j, k + 1) - S(i + 1, j + 1, k) + S(i, j, k + 1) + S(i, j + 1, k) + S(i + 1, j, k) - S(i, j, k);
                    solid[((size_t)k * ny + j) * nx + i] = c > 0;
                }
        auto d = distance_transform(solid, nx, ny, nz);
        gap.resize(d.size());
        for (size_t i = 0; i < d.size(); i++) gap[i] = (float)(std::sqrt(d[i]) * h);
    }
    // The widest gap in a hull (within a box): the farthest any point inside it is from the solid. (The hull is given
    // by its planes.)
    double widest_gap(const std::vector<std::pair<V3, double>> &planes, const Box &b) const {
        int i0 = std::max(0, (int)std::floor((b.lo.x - lo.x) / h)), i1 = std::min(nx - 1, (int)std::ceil((b.hi.x - lo.x) / h));
        int j0 = std::max(0, (int)std::floor((b.lo.y - lo.y) / h)), j1 = std::min(ny - 1, (int)std::ceil((b.hi.y - lo.y) / h));
        int k0 = std::max(0, (int)std::floor((b.lo.z - lo.z) / h)), k1 = std::min(nz - 1, (int)std::ceil((b.hi.z - lo.z) / h));
        double widest = 0;
        for (int k = k0; k <= k1; k++)
            for (int j = j0; j <= j1; j++)
                for (int i = i0; i <= i1; i++) {
                    float g = gap[((size_t)k * ny + j) * nx + i];
                    if (g <= widest) continue;
                    V3 p{lo.x + (i + 0.5) * h, lo.y + (j + 0.5) * h, lo.z + (k + 0.5) * h};
                    if (p.x < b.lo.x || p.x > b.hi.x || p.y < b.lo.y || p.y > b.hi.y || p.z < b.lo.z || p.z > b.hi.z) continue;
                    bool in = true;
                    for (auto &pl : planes)
                        if (dot3(pl.first, p) > pl.second) { in = false; break; }
                    if (in) widest = g;
                }
        return widest;
    }
    double volume(const Box &b) const {
        auto rng = [&](double a, double z, double o, int n, int &i0, int &i1) {
            i0 = std::max(0, (int)std::ceil((a - o) / h - 0.5));
            i1 = std::min(n, (int)std::floor((z - o) / h - 0.5) + 1);
        };
        int i0, i1, j0, j1, k0, k1;
        rng(b.lo.x, b.hi.x, lo.x, nx, i0, i1);
        rng(b.lo.y, b.hi.y, lo.y, ny, j0, j1);
        rng(b.lo.z, b.hi.z, lo.z, nz, k0, k1);
        if (i1 <= i0 || j1 <= j0 || k1 <= k0) return 0;
        long c = (long)S(i1, j1, k1) - S(i0, j1, k1) - S(i1, j0, k1) - S(i1, j1, k0) + S(i0, j0, k1) + S(i0, j1, k0) + S(i1, j0, k0) - S(i0, j0, k0);
        return c * h * h * h;
    }
};

struct PieceParams {
    double layer = 0.005;   // a piece is convex enough when its hull fills no gap wider than this
    double shrink = 0.003;  // its simple shape's corners no more than this outside its hull
    double thick = 0.015;   // nothing thinner (half on each side)
    int max_pieces = 24;
    int max_faces = 14;     // a piece's faces, at most
};

struct Pieces {
    std::vector<Box> regions;
    std::vector<std::pair<int, double>> cuts;   // the planes they were cut apart on
};

// The shape cut into convex enough pieces, the worst first: a region whose hull fills the widest gap (the farthest
// any point inside the hull is from the model's solid) is cut in two along whichever axis plane makes the two hulls
// smallest, until no hull fills a gap wider than pp.layer or there are pp.max_pieces.
void split_regions(const std::vector<std::vector<V3>> &polys, const Box &all, const SolidGrid &grid, const PieceParams &pp, double eps, Pieces &out) {
    struct Region { std::vector<std::vector<V3>> polys; Box box; double gap; int depth; };
    auto gap_of = [&](const std::vector<std::vector<V3>> &ps) {
        Hull H;
        if (!convex_hull(points_of(ps), eps, H)) return 0.0;
        std::vector<std::pair<V3, double>> planes;
        V3 lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
        for (auto &t : H.tris) {
            V3 n = unit3(cross3(sub(H.pts[t[1]], H.pts[t[0]]), sub(H.pts[t[2]], H.pts[t[0]])));
            planes.push_back({n, dot3(n, H.pts[t[0]]) + 1e-9});
        }
        for (auto &p : H.pts) {
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
        return grid.widest_gap(planes, {lo, hi});
    };
    std::vector<Region> done, open;
    open.push_back({polys, all, gap_of(polys), 0});
    while (!open.empty()) {
        // the worst
        size_t w = 0;
        for (size_t i = 1; i < open.size(); i++)
            if (open[i].gap > open[w].gap) w = i;
        Region r = std::move(open[w]);
        open.erase(open.begin() + w);
        if (r.gap <= pp.layer || r.depth >= 14 || (int)(done.size() + open.size() + 2) > pp.max_pieces) {
            done.push_back(std::move(r));
            continue;
        }
        auto pts = points_of(r.polys);
        Hull H;
        double hv = convex_hull(pts, eps, H) ? H.volume() : 0;
        V3 lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
        for (auto &p : pts) {
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
        int best_axis = -1;
        double best_at = 0, best_score = hv * 0.97;
        for (int ax = 0; ax < 3; ax++) {
            double a = coord(lo, ax), z = coord(hi, ax);
            if (z - a < 2 * pp.thick) continue;
            for (int k = 1; k < 16; k++) {
                double at = std::round((a + (z - a) * k / 16) / kSnap) * kSnap;
                if (at <= a || at >= z) continue;
                Hull L, R;
                double score = 0;
                auto lp = points_of(clip_polys(r.polys, ax, at, false)), rp = points_of(clip_polys(r.polys, ax, at, true));
                if (convex_hull(lp, eps, L)) score += L.volume();
                if (convex_hull(rp, eps, R)) score += R.volume();
                if (score < best_score) { best_score = score; best_axis = ax; best_at = at; }
            }
        }
        if (best_axis < 0) {
            done.push_back(std::move(r));
            continue;
        }
        out.cuts.push_back({best_axis, best_at});
        Box lb = r.box, rb = r.box;
        set_coord(lb.hi, best_axis, best_at);
        set_coord(rb.lo, best_axis, best_at);
        auto lpol = clip_polys(r.polys, best_axis, best_at, false), rpol = clip_polys(r.polys, best_axis, best_at, true);
        if (!lpol.empty()) open.push_back({lpol, lb, gap_of(lpol), r.depth + 1});
        if (!rpol.empty()) open.push_back({rpol, rb, gap_of(rpol), r.depth + 1});
    }
    for (auto &r : done) {
        out.regions.push_back(r.box);
    }
}

// A convex polytope as its faces (counterclockwise from outside), clipped plane by plane. Corners within kOn of a plane
// count as on it, and are shared exactly by the faces that meet there.
using Polytope = std::vector<std::vector<V3>>;
const double kOn = 1e-7;
Polytope box_polytope(const Box &b) {
    V3 c[8];
    for (int i = 0; i < 8; i++) c[i] = {i & 1 ? b.hi.x : b.lo.x, i & 2 ? b.hi.y : b.lo.y, i & 4 ? b.hi.z : b.lo.z};
    return {{c[0], c[2], c[3], c[1]}, {c[4], c[5], c[7], c[6]}, {c[0], c[1], c[5], c[4]}, {c[2], c[6], c[7], c[3]}, {c[0], c[4], c[6], c[2]}, {c[1], c[3], c[7], c[5]}};
}
double face_area(const std::vector<V3> &f) {
    V3 a{0, 0, 0};
    for (size_t k = 1; k + 1 < f.size(); k++) {
        V3 c = cross3(sub(f[k], f[0]), sub(f[k + 1], f[0]));
        a.x += c.x; a.y += c.y; a.z += c.z;
    }
    return std::sqrt(dot3(a, a)) / 2;
}
// keep n.x <= d; false if nothing (or no volume) is left
bool clip_polytope(Polytope &P, V3 n, double d) {
    Polytope out;
    std::vector<V3> cap;
    for (auto &f : P) {
        std::vector<V3> g;
        size_t m = f.size();
        for (size_t i = 0; i < m; i++) {
            V3 a = f[i], b = f[(i + 1) % m];
            double da = dot3(n, a) - d, db = dot3(n, b) - d;
            if (std::fabs(da) <= kOn) da = 0;
            if (std::fabs(db) <= kOn) db = 0;
            if (da <= 0) g.push_back(a);
            if (da == 0) cap.push_back(a);
            if ((da < 0 && db > 0) || (da > 0 && db < 0)) {
                // (the same point from either face: from the edge's ends in a fixed order)
                V3 p = a, q = b;
                double dp = da, dq = db;
                if (std::make_tuple(q.x, q.y, q.z) < std::make_tuple(p.x, p.y, p.z)) { std::swap(p, q); std::swap(dp, dq); }
                double t = dp / (dp - dq);
                V3 x{p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t, p.z + (q.z - p.z) * t};
                g.push_back(x);
                cap.push_back(x);
            }
        }
        // (corners repeated by the cut dropped)
        std::vector<V3> h;
        for (auto &v : g)
            if (h.empty() || dot3(sub(h.back(), v), sub(h.back(), v)) > 0) h.push_back(v);
        while (h.size() > 1 && dot3(sub(h.front(), h.back()), sub(h.front(), h.back())) == 0) h.pop_back();
        if (h.size() >= 3 && face_area(h) > 1e-12) out.push_back(h);
    }
    if (cap.size() >= 3) {
        V3 c{0, 0, 0};
        for (auto &q : cap) { c.x += q.x / cap.size(); c.y += q.y / cap.size(); c.z += q.z / cap.size(); }
        V3 u = std::fabs(n.x) < 0.9 ? unit3(cross3(n, {1, 0, 0})) : unit3(cross3(n, {0, 1, 0})), w = cross3(n, u);
        std::vector<std::pair<double, V3>> ring;
        for (auto &q : cap) ring.push_back({std::atan2(dot3(sub(q, c), w), dot3(sub(q, c), u)), q});
        std::sort(ring.begin(), ring.end(), [](auto &a, auto &b) { return a.first < b.first; });
        std::vector<V3> g;
        for (auto &r : ring) {
            bool dup = false;
            for (auto &x : g) dup |= dot3(sub(x, r.second), sub(x, r.second)) == 0;
            if (!dup) g.push_back(r.second);
        }
        if (g.size() >= 3 && face_area(g) > 1e-12) out.push_back(g);
    }
    P.swap(out);
    return P.size() >= 4;
}
double polytope_volume(const Polytope &P) {
    double v = 0;
    for (auto &f : P)
        for (size_t k = 1; k + 1 < f.size(); k++) v += dot3(f[0], cross3(f[k], f[k + 1]));
    return v / 6;
}

// A region's piece, as you'd model collision by hand: the hull of what's in it (thickened where it's thinner than
// pp.thick), then a box around it (its bounds within the region), cut by whichever of the hull's own face planes cuts
// most away, one by one, until no corner is more than pp.shrink outside the hull or it has pp.max_faces faces. Its faces
// lie on the model's own (or its bounds) and it only sits outside it, as Bungie's collision mostly does.
bool make_piece(const std::vector<V3> &pts_in, const Box &region, const PieceParams &pp, double eps, Polytope &out) {
    std::vector<V3> pts = pts_in;
    Hull h;
    V3 flat;
    if (!convex_hull(pts, eps, h, &flat)) {
        if (dot3(flat, flat) == 0) return false;   // a line or a point: nothing to stand on
        std::vector<V3> two;
        for (auto &p : pts) {
            two.push_back({p.x + flat.x * pp.thick / 2, p.y + flat.y * pp.thick / 2, p.z + flat.z * pp.thick / 2});
            two.push_back({p.x - flat.x * pp.thick / 2, p.y - flat.y * pp.thick / 2, p.z - flat.z * pp.thick / 2});
        }
        if (!convex_hull(two, eps, h)) return false;
    }
    for (int it = 0; it < 3; it++) {
        V3 dir{0, 0, 1};
        double w = hull_width(h, dir);
        if (w >= pp.thick * 0.98) break;
        double lo = 1e30, hi = -1e30;
        for (auto &p : h.pts) { lo = std::min(lo, dot3(dir, p)); hi = std::max(hi, dot3(dir, p)); }
        double mid = (lo + hi) / 2, s = pp.thick / std::max(w, 1e-9);
        std::vector<V3> q;
        for (auto &p : h.pts) {
            double o = dot3(dir, p) - mid;
            q.push_back({p.x + dir.x * o * (s - 1), p.y + dir.y * o * (s - 1), p.z + dir.z * o * (s - 1)});
        }
        Hull h2;
        if (!convex_hull(q, eps, h2)) break;
        h = h2;
    }
    struct Pl { V3 n; double d; };
    std::vector<Pl> cand;
    for (auto &t : h.tris) {
        V3 n = snap_normal(unit3(cross3(sub(h.pts[t[1]], h.pts[t[0]]), sub(h.pts[t[2]], h.pts[t[0]]))));
        double d = -1e30;
        for (auto &p : h.pts) d = std::max(d, dot3(n, p));   // (the plane around all of it, the normal snapped)
        d = snap_up(d);
        bool dup = false;
        for (auto &q : cand)
            if (dot3(q.n, n) > 1 - 1e-6 && std::fabs(q.d - d) < 1e-5) dup = true;
        // (axis-aligned ones are the box's)
        if (!dup && std::max({std::fabs(n.x), std::fabs(n.y), std::fabs(n.z)}) < 1 - 1e-9) cand.push_back({n, d});
    }
    V3 lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
    for (auto &p : h.pts) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    Box b{{std::max(snap_down(lo.x), region.lo.x), std::max(snap_down(lo.y), region.lo.y), std::max(snap_down(lo.z), region.lo.z)},
          {std::min(snap_up(hi.x), region.hi.x), std::min(snap_up(hi.y), region.hi.y), std::min(snap_up(hi.z), region.hi.z)}};
    if (b.hi.x - b.lo.x < 1e-6 || b.hi.y - b.lo.y < 1e-6 || b.hi.z - b.lo.z < 1e-6) return false;
    Polytope P = box_polytope(b);
    auto outside = [&](const Polytope &Q) {   // how far its farthest corner is outside the hull
        double far = 0;
        for (auto &f : Q)
            for (auto &v : f) {
                double o = -1e30;
                for (auto &t : h.tris) {
                    V3 n = unit3(cross3(sub(h.pts[t[1]], h.pts[t[0]]), sub(h.pts[t[2]], h.pts[t[0]])));
                    o = std::max(o, dot3(n, v) - dot3(n, h.pts[t[0]]));
                }
                far = std::max(far, o);
            }
        return far;
    };
    std::vector<char> used(cand.size(), 0);
    int faces = 6;
    while (faces < pp.max_faces && outside(P) > pp.shrink) {
        double v = polytope_volume(P), best_cut = 0;
        size_t best = SIZE_MAX;
        Polytope best_P;
        for (size_t i = 0; i < cand.size(); i++) {
            if (used[i]) continue;
            Polytope Q = P;
            if (!clip_polytope(Q, cand[i].n, cand[i].d)) continue;
            double cut = v - polytope_volume(Q);
            if (cut > best_cut) { best_cut = cut; best = i; best_P = std::move(Q); }
        }
        if (best == SIZE_MAX || best_cut < 1e-9) break;
        used[best] = 1;
        P = std::move(best_P);
        faces = (int)P.size();
    }
    out = P;
    return true;
}

// The whole: pieces, their surface, materials from the nearest original surface, a BSP.
bool convex_pieces(const std::vector<Poly> &polys, const SolidGrid &grid, const PieceParams &pp, Bsp &out, std::string *why) {
    auto fail = [&](const char *w) { if (why) *why = w; return false; };
    std::vector<std::vector<V3>> ps;
    V3 lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
    for (auto &q : polys) {
        std::vector<V3> p;
        for (auto &a : q.pts) {
            p.push_back({a[0], a[1], a[2]});
            lo = {std::min(lo.x, a[0]), std::min(lo.y, a[1]), std::min(lo.z, a[2])};
            hi = {std::max(hi.x, a[0]), std::max(hi.y, a[1]), std::max(hi.z, a[2])};
        }
        ps.push_back(p);
    }
    const double eps = 1e-5;
    Box all{{snap_down(lo.x - pp.thick), snap_down(lo.y - pp.thick), snap_down(lo.z - pp.thick)}, {snap_up(hi.x + pp.thick), snap_up(hi.y + pp.thick), snap_up(hi.z + pp.thick)}};
    Pieces pc;
    split_regions(ps, all, grid, pp, eps, pc);
    std::vector<Polytope> pieces;
    for (size_t i = 0; i < pc.regions.size(); i++) {
        const Box &r = pc.regions[i];
        auto clipped = ps;
        for (int ax = 0; ax < 3; ax++) {
            clipped = clip_polys(clipped, ax, coord(r.lo, ax), true);
            clipped = clip_polys(clipped, ax, coord(r.hi, ax), false);
        }
        Polytope P;
        if (make_piece(points_of(clipped), r, pp, eps, P)) pieces.push_back(P);
    }
    if (pieces.empty()) return fail("no pieces");
    // each piece a closed shape of its own (where pieces meet on a cut, their faces there face each other, solid both
    // sides: the BSP has no empty space there to look them up from); faces of more than 8 corners split
    std::vector<std::vector<V3>> faces;
    std::vector<int> groups;
    for (size_t pi = 0; pi < pieces.size(); pi++)
        for (auto &f : pieces[pi]) {
            if (f.size() <= 8) { faces.push_back(f); groups.push_back((int)pi); continue; }
            for (size_t k = 1; k + 1 < f.size(); k += 6) {
                std::vector<V3> part = {f[0]};
                for (size_t m = k; m <= std::min(k + 6, f.size() - 1); m++) part.push_back(f[m]);
                faces.push_back(part);
                groups.push_back((int)pi);
            }
        }
    std::vector<Poly> np;
    for (size_t fi = 0; fi < faces.size(); fi++) {
        auto &f = faces[fi];
        Poly p;
        p.group = groups[fi];
        V3 c{0, 0, 0};
        for (auto &v : f) {
            p.pts.push_back({v.x, v.y, v.z});
            c.x += v.x / f.size(); c.y += v.y / f.size(); c.z += v.z / f.size();
        }
        double bd = 1e30;
        for (auto &q : polys) {
            double d2 = poly_dist2(q, c);
            if (d2 < bd) { bd = d2; p.material = q.material; p.flags = q.flags & ~(kTwoSided | kBreakable); }
        }
        np.push_back(p);
    }
    return build(np, out, why);
}
}   // namespace

// ---- Bungie's own collision where it fits

std::vector<NodeBsps> read_nodes(const tagfile::Elem &c) {
    std::vector<NodeBsps> out;
    tagfile::Block nodes = c.block("nodes");
    for (size_t i = 0; i < nodes.size(); i++) {
        NodeBsps n;
        n.name = nodes[i].str("name");
        tagfile::Block b = nodes[i].block("bsps");
        for (size_t j = 0; j < b.size(); j++) {
            Bsp x;
            read_bsp(b[j], x);
            n.bsps.push_back(x);
        }
        out.push_back(n);
    }
    return out;
}

namespace {
using Mat = std::array<double, 12>;   // rotation rows, then translation
Mat mat_mul(const Mat &a, const Mat &b) {
    Mat o{};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++)
            for (int k = 0; k < 3; k++) o[i * 3 + j] += a[i * 3 + k] * b[k * 3 + j];
        o[9 + i] = a[9 + i];
        for (int k = 0; k < 3; k++) o[9 + i] += a[i * 3 + k] * b[9 + k];
    }
    return o;
}
std::array<double, 3> mat_apply(const Mat &m, const std::array<double, 3> &p) {
    std::array<double, 3> o;
    for (int i = 0; i < 3; i++) o[i] = m[9 + i] + m[i * 3] * p[0] + m[i * 3 + 1] * p[1] + m[i * 3 + 2] * p[2];
    return o;
}

// distance from p to a triangle (Ericson's closest point)
double tri_dist2(const double *A, const double *B, const double *C, const std::array<double, 3> &p) {
    double ab[3], ac[3], ap[3], bp[3], cq[3], cp[3];
    for (int c = 0; c < 3; c++) { ab[c] = B[c] - A[c]; ac[c] = C[c] - A[c]; ap[c] = p[c] - A[c]; bp[c] = p[c] - B[c]; cq[c] = p[c] - C[c]; }
    auto dot = [](const double *u, const double *v) { return u[0] * v[0] + u[1] * v[1] + u[2] * v[2]; };
    double d1 = dot(ab, ap), d2 = dot(ac, ap), d3 = dot(ab, bp), d4 = dot(ac, bp), d5 = dot(ab, cq), d6 = dot(ac, cq);
    double va = d3 * d6 - d5 * d4, vb = d5 * d2 - d1 * d6, vc = d1 * d4 - d3 * d2;
    if (d1 <= 0 && d2 <= 0) for (int c = 0; c < 3; c++) cp[c] = A[c];
    else if (d3 >= 0 && d4 <= d3) for (int c = 0; c < 3; c++) cp[c] = B[c];
    else if (vc <= 0 && d1 >= 0 && d3 <= 0) { double v = d1 / (d1 - d3); for (int c = 0; c < 3; c++) cp[c] = A[c] + ab[c] * v; }
    else if (d6 >= 0 && d5 <= d6) for (int c = 0; c < 3; c++) cp[c] = C[c];
    else if (vb <= 0 && d2 >= 0 && d6 <= 0) { double w = d2 / (d2 - d6); for (int c = 0; c < 3; c++) cp[c] = A[c] + ac[c] * w; }
    else if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) { double w = (d4 - d3) / ((d4 - d3) + (d5 - d6)); for (int c = 0; c < 3; c++) cp[c] = B[c] + (C[c] - B[c]) * w; }
    else { double den = 1 / (va + vb + vc), v = vb * den, w = vc * den; for (int c = 0; c < 3; c++) cp[c] = A[c] + ab[c] * v + ac[c] * w; }
    double d = 0;
    for (int c = 0; c < 3; c++) d += (p[c] - cp[c]) * (p[c] - cp[c]);
    return d;
}

using Tri9 = std::array<double, 9>;
// points spread over triangles by area (about n)
std::vector<std::array<double, 3>> spread_points(const std::vector<Tri9> &ts, int n) {
    std::vector<double> ar;
    double total = 0;
    for (auto &t : ts) {
        double u[3] = {t[3] - t[0], t[4] - t[1], t[5] - t[2]}, w[3] = {t[6] - t[0], t[7] - t[1], t[8] - t[2]};
        double cr[3] = {u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
        ar.push_back(std::sqrt(cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]) / 2);
        total += ar.back();
    }
    std::vector<std::array<double, 3>> out;
    if (total <= 0) return out;
    uint32_t x = 5;
    auto rnd = [&]() { x = x * 1664525u + 1013904223u; return (x >> 8) / 16777216.0; };
    for (size_t i = 0; i < ts.size(); i++) {
        int m = (int)(n * ar[i] / total + rnd());
        for (int s = 0; s < m; s++) {
            double r1 = rnd(), r2 = rnd();
            if (r1 + r2 > 1) { r1 = 1 - r1; r2 = 1 - r2; }
            auto &t = ts[i];
            out.push_back({t[0] + (t[3] - t[0]) * r1 + (t[6] - t[0]) * r2, t[1] + (t[4] - t[1]) * r1 + (t[7] - t[1]) * r2,
                           t[2] + (t[5] - t[2]) * r1 + (t[8] - t[2]) * r2});
        }
    }
    return out;
}
// distances from points to the nearest of some triangles (sorted)
std::vector<double> distances_to(const std::vector<std::array<double, 3>> &pts, const std::vector<Tri9> &ts) {
    std::vector<std::array<double, 6>> box;
    for (auto &t : ts)
        box.push_back({std::min({t[0], t[3], t[6]}), std::min({t[1], t[4], t[7]}), std::min({t[2], t[5], t[8]}), std::max({t[0], t[3], t[6]}),
                       std::max({t[1], t[4], t[7]}), std::max({t[2], t[5], t[8]})});
    std::vector<double> out;
    for (auto &p : pts) {
        double best = 1e30;
        for (size_t i = 0; i < ts.size(); i++) {
            auto &b = box[i];
            double dx = std::max({b[0] - p[0], 0.0, p[0] - b[3]}), dy = std::max({b[1] - p[1], 0.0, p[1] - b[4]}), dz = std::max({b[2] - p[2], 0.0, p[2] - b[5]});
            if (dx * dx + dy * dy + dz * dz >= best) continue;
            best = std::min(best, tri_dist2(&ts[i][0], &ts[i][3], &ts[i][6], p));
        }
        out.push_back(std::sqrt(best));
    }
    std::sort(out.begin(), out.end());
    return out;
}
// a BSP's surfaces as triangles, placed
std::vector<Tri9> placed_tris(const Bsp &b, const Mat &m) {
    std::vector<Tri9> out;
    for (auto &q : polygons(b)) {
        std::vector<std::array<double, 3>> w;
        for (auto &p : q.pts) w.push_back(mat_apply(m, p));
        for (size_t k = 1; k + 1 < w.size(); k++) out.push_back({w[0][0], w[0][1], w[0][2], w[k][0], w[k][1], w[k][2], w[k + 1][0], w[k + 1][1], w[k + 1][2]});
    }
    return out;
}
// how a collision sits on what's drawn: its distance to it, and its distance from it (50% and 90% of each)
struct Fit { double c50, c90, v50, v90; };
Fit fit_of(const std::vector<Tri9> &col, const std::vector<Tri9> &vis) {
    auto q = [](const std::vector<double> &v, double f) { return v.empty() ? 1e30 : v[std::min(v.size() - 1, (size_t)(v.size() * f))]; };
    auto a = distances_to(spread_points(col, 800), vis), b = distances_to(spread_points(vis, 800), col);
    return {q(a, 0.5), q(a, 0.9), q(b, 0.5), q(b, 0.9)};
}
}   // namespace

bool read_rig(const tagfile::Elem &model, Rig &out) {
    out = Rig();
    tagfile::Block nodes = model.block("nodes");
    if (!nodes.size()) return false;
    for (size_t i = 0; i < nodes.size(); i++) {
        double x = nodes[i].num("default rotation", 0), y = nodes[i].num("default rotation", 1), z = nodes[i].num("default rotation", 2),
               w = nodes[i].num("default rotation", 3);
        double n = std::sqrt(x * x + y * y + z * z + w * w);
        if (n > 0) { x /= n; y /= n; z /= n; w /= n; }
        Mat l = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), nodes[i].num("default translation", 0),
                 nodes[i].num("default translation", 1), nodes[i].num("default translation", 2)};
        int par = (int)nodes[i].num("parent node index");
        out.world.push_back(par >= 0 && par < (int)i ? mat_mul(out.world[par], l) : l);
        out.names.push_back(nodes[i].str("name"));
    }
    out.tris.assign(nodes.size(), {});
    tagfile::Block regions = model.block("regions"), geos = model.block("geometries");
    for (size_t r = 0; r < regions.size(); r++) {
        tagfile::Block perms = regions[r].block("permutations");
        if (!perms.size()) continue;
        size_t pick = 0;
        for (size_t p = 0; p < perms.size(); p++)
            if (perms[p].str("name") == "base") pick = p;
        int g = (int)perms[pick].num("super high");
        if (g < 0 || g >= (int)geos.size()) continue;
        tagfile::Block parts = geos[g].block("parts");
        for (size_t p = 0; p < parts.size(); p++) {
            tagfile::Block V = parts[p].block("uncompressed vertices"), T = parts[p].block("triangles");
            std::vector<std::array<double, 3>> P;
            std::vector<int> node;
            for (size_t i = 0; i < V.size(); i++) {
                P.push_back({V[i].num("position", 0), V[i].num("position", 1), V[i].num("position", 2)});
                node.push_back((int)V[i].num("node0 index"));
            }
            std::vector<int> s;
            const char *f[3] = {"vertex0 index", "vertex1 index", "vertex2 index"};
            for (size_t i = 0; i < T.size(); i++)
                for (int k = 0; k < 3; k++) {
                    int x = (int)T[i].num(f[k]);
                    if (x != 0xFFFF && x >= 0) s.push_back(x);
                }
            for (size_t i = 2; i < s.size(); i++) {
                int a = s[i - 2], b = s[i - 1], c = s[i];
                if (a == b || b == c || a == c || a >= (int)P.size() || b >= (int)P.size() || c >= (int)P.size()) continue;
                int nd = node[a];
                if (nd < 0 || nd >= (int)nodes.size()) continue;
                out.tris[nd].push_back({P[a][0], P[a][1], P[a][2], P[b][0], P[b][1], P[b][2], P[c][0], P[c][1], P[c][2]});
            }
        }
    }
    return true;
}

StockFit fit_stock(const std::vector<NodeBsps> &own, const Rig &own_rig, const std::vector<NodeBsps> &stock, const Rig &stock_rig) {
    StockFit r;
    r.bsps.assign(own.size(), {});
    auto rig_index = [](const Rig &g, const std::string &n) {
        for (size_t i = 0; i < g.names.size(); i++)
            if (g.names[i] == n) return (int)i;
        return -1;
    };
    const double abs_tol = 0.0066;   // 2 cm
    const size_t kFewest = 12;       // triangles a node must move, here and in the stock model, to judge it by
    for (size_t i = 0; i < own.size(); i++) {
        if (own[i].bsps.empty()) continue;
        const NodeBsps *st = nullptr;
        for (auto &s : stock)
            if (s.name == own[i].name && !s.bsps.empty()) st = &s;
        int oi = rig_index(own_rig, own[i].name), si = rig_index(stock_rig, own[i].name);
        if (!st || oi < 0 || si < 0 || own_rig.tris[oi].size() < kFewest || stock_rig.tris[si].size() < kFewest) continue;
        bool breakable = false;
        for (auto &b : own[i].bsps)
            for (auto &sf : b.surfaces) breakable |= (sf.flags & kBreakable) != 0;
        if (breakable) continue;   // (breakable surfaces are the map's own to keep)
        Fit base = fit_of(placed_tris(st->bsps[0], stock_rig.world[si]), stock_rig.tris[si]);
        Fit here = fit_of(placed_tris(st->bsps[0], own_rig.world[oi]), own_rig.tris[oi]);
        auto ok = [&](double h, double b) { return h <= std::max(b * 1.25, b + abs_tol); };
        if (!(ok(here.c50, base.c50) && ok(here.c90, base.c90) && ok(here.v50, base.v50) && ok(here.v90, base.v90))) continue;
        // the stock BSPs, each surface given the material of the nearest of the node's own surfaces
        std::vector<Poly> own_polys;
        for (auto &b : own[i].bsps)
            for (auto &p : polygons(b)) own_polys.push_back(p);
        std::vector<Bsp> nb;
        for (size_t j = 0; j < own[i].bsps.size(); j++) {
            Bsp b = st->bsps[std::min(j, st->bsps.size() - 1)];
            for (uint32_t s = 0; s < b.surfaces.size(); s++) {
                auto ring = surface_ring(b, s);
                V3 c{0, 0, 0};
                for (auto v : ring) { c.x += b.vertices[v].p[0] / ring.size(); c.y += b.vertices[v].p[1] / ring.size(); c.z += b.vertices[v].p[2] / ring.size(); }
                double bd = 1e30;
                for (auto &q : own_polys) {
                    double d2 = poly_dist2(q, c);
                    if (d2 < bd) { bd = d2; b.surfaces[s].material = q.material; }
                }
                b.surfaces[s].flags &= ~kBreakable;
                b.surfaces[s].breakable = -1;
            }
            nb.push_back(b);
        }
        for (auto &b : own[i].bsps) r.surfaces_before += b.surfaces.size();
        for (auto &b : nb) r.surfaces_after += b.surfaces.size();
        r.bsps[i] = nb;
        r.taken.push_back(own[i].name);
    }
    return r;
}

const double kBipedReach = 0.49;   // a biped's feature sphere: half its pill's height, its radius, 1/16 (cyborg: 0.15 + 0.2 + 0.0625), and a little movement

long sphere_work(const Bsp &b) {
    long ops = 0;
    worst_features(b, kBipedReach, &ops);
    return ops;
}

Lightened lighten(const Bsp &in, long budget, Bsp &out) {
    Lightened r;
    r.surfaces_before = r.surfaces_after = in.surfaces.size();
    r.work_before = r.work_after = sphere_work(in);
    if (r.work_before <= budget) { r.why = "light enough"; return r; }
    for (auto &sf : in.surfaces)
        if (sf.flags & (kTwoSided | kBreakable)) { r.why = "it has two-sided or breakable surfaces"; return r; }
    auto accept = [&](const Bsp &b) { return self_consistency(b, 1500) >= 0.995; };
    // the same surfaces in a leaner BSP may be enough
    {
        Bsp b;
        if (build(polygons(in), b) && accept(b)) {
            long w = sphere_work(b);
            if (w <= budget) {
                out = b;
                r.changed = true;
                r.work_after = w;
                return r;
            }
        }
    }
    // else made again as Bungie made collision for their vehicles: convex pieces with big flat faces, hollows and small
    // details filled over, nothing thin; looser in steps until it's light enough
    auto polys = polygons(in);
    V3 lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
    for (auto &q : polys)
        for (auto &a : q.pts) {
            lo = {std::min(lo.x, a[0]), std::min(lo.y, a[1]), std::min(lo.z, a[2])};
            hi = {std::max(hi.x, a[0]), std::max(hi.y, a[1]), std::max(hi.z, a[2])};
        }
    double volume = (hi.x - lo.x + 0.02) * (hi.y - lo.y + 0.02) * (hi.z - lo.z + 0.02);
    double cell = std::max(0.003, std::cbrt(volume / 2e6));
    SolidGrid grid(in, {lo.x - 0.01, lo.y - 0.01, lo.z - 0.01}, {hi.x + 0.01, hi.y + 0.01, hi.z + 0.01}, cell);
    struct Try { bool ok = false; long work = 0; Bsp b; PieceParams pp; };
    Try best, lightest;
    // (world units: 0.01 is 3 cm) hollows and gaps up to about 4 cm filled; nothing thinner than about 4.5 cm; the
    // most pieces that are light enough
    for (int n : {24, 18, 14, 12, 10, 9, 8, 7, 6}) {
        Try t;
        t.pp = {0.007, n >= 14 ? 0.006 : n >= 10 ? 0.008 : 0.011, 0.015, n, n >= 12 ? 12 : 10};
        t.ok = convex_pieces(polys, grid, t.pp, t.b, nullptr) && accept(t.b);
        if (!t.ok) continue;
        t.work = sphere_work(t.b);
        if (t.work <= budget) { best = std::move(t); break; }
        if (!lightest.ok || t.work < lightest.work) lightest = std::move(t);
    }
    if (!best.ok) {
        // not light enough at the most it may change: the lightest, if much lighter
        if (!lightest.ok) { r.why = "it couldn't be made again as a closed shape"; return r; }
        if (lightest.work * 2 > r.work_before) { r.why = "it couldn't be made much lighter without changing its shape too much"; return r; }
        best = std::move(lightest);
    }
    out = std::move(best.b);
    r.changed = true;
    r.work_after = best.work;
    r.surfaces_after = out.surfaces.size();
    r.pieces = best.pp.max_pieces;
    auto pb = polygons(out);
    auto added = spread_distances(pb, polys, 4000), lost = spread_distances(polys, pb, 4000);
    auto q = [](const std::vector<double> &v, double f) { return v.empty() ? 0.0 : v[std::min(v.size() - 1, (size_t)(v.size() * f))]; };
    r.moved = q(added, 0.99);
    r.moved_most = added.empty() ? 0 : added.back();
    r.lost = q(lost, 0.99);
    return r;
}

std::vector<double> thickness_profile(const Bsp &b, int samples) { return thicknesses(b, samples); }

}   // namespace coll
