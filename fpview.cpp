// Halo Map Porter - how a first-person weapon's model is seen (see fpview.hpp)
#include "fpview.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <atomic>
#include <thread>

#include "animcomp.hpp"
#include "tagfile.hpp"

namespace fpview {

namespace fs = std::filesystem;

namespace {

// A real_matrix4x3 as the engine keeps one (matrix_math.c): a scale, the forward, left and up vectors, a position.
struct M {
    double s = 1;
    double f[3] = {1, 0, 0}, l[3] = {0, 1, 0}, u[3] = {0, 0, 1}, p[3] = {0, 0, 0};
};
// matrix4x3_from_orientation (matrix4x3_rotation_from_quaternion: the engine's quaternions turn the other way from
// the usual reading of them, so this is that matrix's transpose).
M from_orientation(const double q[4], const double t[3], double scale) {
    double n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3], k = n != 0 ? 2 / n : 0;
    double x = k * q[0], y = k * q[1], z = k * q[2];
    double wx = q[3] * x, wy = q[3] * y, wz = q[3] * z, xx = q[0] * x, xy = q[0] * y, xz = q[0] * z, yy = q[1] * y, yz = q[1] * z,
           zz = q[2] * z;
    M m;
    m.s = scale;
    m.f[0] = 1 - (yy + zz); m.f[1] = xy - wz; m.f[2] = xz + wy;
    m.l[0] = xy + wz; m.l[1] = 1 - (xx + zz); m.l[2] = yz - wx;
    m.u[0] = xz - wy; m.u[1] = yz + wx; m.u[2] = 1 - (xx + yy);
    for (int i = 0; i < 3; i++) m.p[i] = t[i];
    return m;
}
void turn(const M &m, const double v[3], double r[3]) {   // (the rotation only)
    for (int i = 0; i < 3; i++) r[i] = m.f[i] * v[0] + m.l[i] * v[1] + m.u[i] * v[2];
}
void point(const M &m, const double v[3], double r[3]) {   // matrix4x3_transform_point
    double sv[3] = {v[0] * m.s, v[1] * m.s, v[2] * m.s};
    turn(m, sv, r);
    for (int i = 0; i < 3; i++) r[i] += m.p[i];
}
M multiply(const M &a, const M &b) {   // matrix4x3_multiply: b, then a
    M r;
    r.s = a.s * b.s;
    turn(a, b.f, r.f);
    turn(a, b.l, r.l);
    turn(a, b.u, r.u);
    point(a, b.p, r.p);
    return r;
}
M inverse(const M &a) {
    M r;
    r.s = a.s != 0 ? 1 / a.s : 0;
    for (int i = 0; i < 3; i++) {   // (the transpose)
        r.f[i] = i == 0 ? a.f[0] : i == 1 ? a.l[0] : a.u[0];
        r.l[i] = i == 0 ? a.f[1] : i == 1 ? a.l[1] : a.u[1];
        r.u[i] = i == 0 ? a.f[2] : i == 1 ? a.l[2] : a.u[2];
    }
    double t[3];
    turn(r, a.p, t);
    for (int i = 0; i < 3; i++) r.p[i] = -t[i] * r.s;
    return r;
}
// quaternions_multiply: q0 x q1.
void qmul(const double a[4], const double b[4], double r[4]) {
    double x = a[1] * b[2] + a[0] * b[3] + a[3] * b[0] - a[2] * b[1];
    double y = a[3] * b[1] + a[1] * b[3] + a[2] * b[0] - a[0] * b[2];
    double z = a[2] * b[3] + a[0] * b[1] + a[3] * b[2] - a[1] * b[0];
    double w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    r[0] = x; r[1] = y; r[2] = z; r[3] = w;
}

std::string lower(std::string s) {
    for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// A part's triangle strip read as triangles, as lod.cpp reads it (even (a b c), odd (b a c); degenerate ones left out).
std::vector<uint32_t> strip_triangles(const tagfile::Block &strip, size_t vertices) {
    std::vector<uint32_t> s, out;
    static const char *f[3] = {"vertex0 index", "vertex1 index", "vertex2 index"};
    for (size_t i = 0; i < strip.size(); i++)
        for (int k = 0; k < 3; k++) {
            long long x = strip[i].integer(f[k]);
            if (x != 65535) s.push_back((uint32_t)x);
        }
    for (size_t i = 2; i < s.size(); i++) {
        uint32_t a = s[i - 2], b = s[i - 1], c = s[i];
        if (a == b || b == c || a == c || a >= vertices || b >= vertices || c >= vertices) continue;
        if ((i - 2) % 2 == 0) out.insert(out.end(), {a, b, c});
        else out.insert(out.end(), {b, a, c});
    }
    return out;
}

struct Vertex { double p[3], n[3]; int node[2]; double weight; };
struct Part {
    int geometry = 0, index = 0;   // (its geometry, and its place there)
    std::vector<Vertex> v;
    std::vector<uint32_t> tris;
    lod::ShaderLook look;
};

}   // namespace

bool measure(const std::string &model_tag, const std::string &anim_tag, const Find &find, const lod::LookUp &look, lod::Seen &out,
             std::string *why, std::vector<std::vector<uint32_t>> *pictures, int pw, int ph) {
    auto fail = [&](const std::string &w) {
        if (why) *why = w;
        return false;
    };
    out = lod::Seen();
    tagfile::Tag mt, at;
    fs::path mf = find(model_tag), af = find(anim_tag);
    if (mf.empty() || !mt.load(mf)) return fail("its model couldn't be read");
    if (af.empty() || !at.load(af)) return fail("its first-person animations couldn't be read");
    tagfile::Elem mr = mt.root(), ar = at.root();

    // The model's bones at rest, and the inverse of each one's place (models.c: runtime_default_inverse_matrix).
    tagfile::Block mnodes = mr.block("nodes");
    const int MN = (int)mnodes.size();
    if (MN == 0) return fail("its model has no bones");
    std::vector<M> rest(MN), rest_inverse(MN);
    std::vector<int> mparent(MN);
    for (int n = 0; n < MN; n++) {
        long long pa = mnodes[n].integer("parent node index");
        mparent[n] = pa >= 0 && pa < n ? (int)pa : -1;   // (a parent comes before its children)
        double q[4], t[3];
        for (int k = 0; k < 4; k++) q[k] = mnodes[n].num("default rotation", k);
        for (int k = 0; k < 3; k++) t[k] = mnodes[n].num("default translation", k);
        M local = from_orientation(q, t, 1);
        rest[n] = mparent[n] >= 0 ? multiply(rest[mparent[n]], local) : local;
        rest_inverse[n] = inverse(rest[n]);
    }
    // The animation graph's bones, and each model bone's (by name, as the engine matches them).
    tagfile::Block gnodes = ar.block("nodes");
    const int GN = (int)gnodes.size();
    if (GN == 0 || GN > 64) return fail("its first-person animations have no bones it can read");
    std::vector<int> gparent(GN);
    std::map<std::string, int> gindex;
    for (int n = 0; n < GN; n++) {
        long long pa = gnodes[n].integer("parent node index");
        gparent[n] = pa >= 0 && pa < GN && pa != n ? (int)pa : -1;
        gindex[lower(gnodes[n].str("name"))] = n;
    }
    std::vector<int> to_graph(MN);
    for (int n = 0; n < MN; n++) {
        auto it = gindex.find(lower(mnodes[n].str("name")));
        if (it == gindex.end()) return fail("its first-person animations don't move its bone \"" + mnodes[n].str("name") + "\"");
        to_graph[n] = it->second;
    }

    // The shaders, and the parts the game draws: each region's first permutation, most detailed.
    tagfile::Block shaders = mr.block("shaders");
    std::vector<lod::ShaderLook> looks;
    for (size_t i = 0; i < shaders.size(); i++) {
        std::string ref = shaders[i].ref("shader");
        lod::ShaderLook l = ref.empty() || !look ? lod::ShaderLook() : look(ref);
        if (!l.known) return fail("its shader " + (ref.empty() ? std::string("(none)") : ref) + " couldn't be read");
        looks.push_back(l);
    }
    tagfile::Block geos = mr.block("geometries"), regions = mr.block("regions");
    std::set<int> drawn;
    for (size_t r = 0; r < regions.size(); r++) {
        tagfile::Block perms = regions[r].block("permutations");
        if (!perms.size()) continue;
        long long g = perms[0].integer("super high");
        if (g >= 0 && g < (long long)geos.size()) drawn.insert((int)g);
    }
    std::vector<Part> parts;
    for (int g : drawn) {
        tagfile::Block gp = geos[(size_t)g].block("parts");
        for (size_t k = 0; k < gp.size(); k++) {
            Part p;
            p.geometry = g;
            p.index = (int)k;
            tagfile::Block vs = gp[k].block("uncompressed vertices");
            long long sh = gp[k].integer("shader index");
            p.look = sh >= 0 && (size_t)sh < looks.size() ? looks[(size_t)sh] : lod::ShaderLook();
            for (size_t i = 0; i < vs.size(); i++) {
                Vertex v;
                for (int c = 0; c < 3; c++) {
                    v.p[c] = vs[i].num("position", c);
                    v.n[c] = vs[i].num("normal", c);
                }
                long long n0 = vs[i].integer("node0 index"), n1 = vs[i].integer("node1 index");
                v.node[0] = n0 >= 0 && n0 < MN ? (int)n0 : 0;
                v.node[1] = n1 >= 0 && n1 < MN ? (int)n1 : -1;
                v.weight = std::clamp(vs[i].num("node0 weight"), 0.0, 1.0);
                if (v.node[1] < 0) v.weight = 1;
                p.v.push_back(v);
            }
            p.tris = strip_triangles(gp[k].block("triangles"), vs.size());
            parts.push_back(std::move(p));
        }
    }
    if (parts.empty()) return fail("its model has nothing to draw");

    // The poses: every base animation's frames (every other one past 64 frames), weighed by how long the weapon is in
    // them; then the idle pose with each overlay frame applied in full.
    tagfile::Block anims = ar.block("animations");
    struct Frame { std::vector<animcomp::Pose> pose; double weight = 0; };
    std::vector<Frame> frames;
    std::vector<animcomp::Pose> idle;
    bool idle_named = false;
    auto decode = [&](size_t a, animcomp::Clip &c) {
        tagfile::Elem e = anims[a];
        c.frames = (int)e.integer("frame count");
        c.nodes = (int)e.integer("node count");
        if (c.frames <= 0 || c.nodes != GN) return false;
        for (int k = 0; k < 2; k++) {
            c.rot[k] = (uint32_t)e.integer("node rotation flag data", k);
            c.tr[k] = (uint32_t)e.integer("node transform flag data", k);
            c.sc[k] = (uint32_t)e.integer("node scale flag data", k);
        }
        std::string fd = e.data("frame data"), def = e.data("default data");
        size_t off = (size_t)e.integer("offset to compressed data");
        if ((long long)e.integer("flags") & 1) {   // (compressed: its frames may still be in front of the compressed data)
            if (off > 0 && off <= fd.size() && animcomp::decode_tag_frames(def, fd.substr(0, off), c)) return true;
            return off <= fd.size() && animcomp::decode_compressed(fd.substr(off), c);
        }
        return animcomp::decode_tag_frames(def, fd, c);
    };
    std::vector<std::pair<size_t, animcomp::Clip>> overlays;
    for (size_t a = 0; a < anims.size(); a++) {
        long long type = anims[a].integer("type");
        animcomp::Clip c;
        if ((type != 0 && type != 1) || !decode(a, c)) continue;
        if (type == 1) {
            overlays.push_back({a, std::move(c)});
            continue;
        }
        std::string name = lower(anims[a].str("name"));
        double w = name.find("idle") != std::string::npos ? 10 : name.find("posing") != std::string::npos ? 4
                   : name.find("fire") != std::string::npos                                         ? 3
                   : name.find("put away") != std::string::npos                                     ? 0.5
                                                                                                     : 1;
        int step = std::max(1, (c.frames + 63) / 64), n = 0;
        for (int f = 0; f < c.frames; f += step) n++;
        for (int f = 0; f < c.frames; f += step) {
            Frame fr;
            fr.pose.assign(c.p.begin() + (long)f * GN, c.p.begin() + (long)(f + 1) * GN);
            fr.weight = w / n;
            frames.push_back(std::move(fr));
        }
        bool is_idle = name.find("idle") != std::string::npos;   // (the idle pose: the first idle animation's first frame)
        if (idle.empty() || (is_idle && !idle_named)) {
            idle.assign(c.p.begin(), c.p.begin() + GN);
            idle_named = is_idle;
        }
    }
    if (frames.empty()) return fail("its first-person animations couldn't be read");
    size_t base_frames = frames.size();
    for (auto &[a, c] : overlays)
        for (int f = 0; f < c.frames; f++) {
            Frame fr;
            fr.pose = idle;
            for (int n = 0; n < GN; n++) {
                const animcomp::Pose &o = c.at(f, n);
                animcomp::Pose &p = fr.pose[(size_t)n];
                if (animcomp::flag(c.rot, n)) qmul(o.q, p.q, p.q);
                if (animcomp::flag(c.tr, n))
                    for (int k = 0; k < 3; k++) p.t[k] += o.t[k];
                if (animcomp::flag(c.sc, n)) p.s *= o.s;
            }
            fr.weight = 1.0 / std::max<size_t>(1, overlays.size()) / c.frames;
            frames.push_back(std::move(fr));
        }

    // The view: 110 x 80 degrees, the camera at the root looking along +x (left +y, up +z).
    const double TX = std::tan(55 * 3.14159265358979 / 180), TY = std::tan(40 * 3.14159265358979 / 180), NEAR = 0.002;
    const int W = 720, H = (int)std::lround(W * TY / TX);
    // (each part's triangles, counted through: per frame whether seen, all frames' weight seen)
    std::vector<size_t> first(parts.size() + 1, 0);
    for (size_t p = 0; p < parts.size(); p++) first[p + 1] = first[p] + parts[p].tris.size() / 3;
    const size_t T = first.back();

    // Rasterizes a triangle (camera space, clipped at the near plane): pixel(x, y, depth) for each pixel it covers,
    // until it returns true.
    auto raster = [&](const std::array<double, 3> *q, const std::function<bool(int, int, float)> &pixel) {
        struct C { double x, y, z; };
        std::vector<C> poly{{q[0][0], q[0][1], q[0][2]}, {q[1][0], q[1][1], q[1][2]}, {q[2][0], q[2][1], q[2][2]}}, cl;
        for (size_t k = 0; k < poly.size(); k++) {
            const C &a = poly[k], &b = poly[(k + 1) % poly.size()];
            bool ain = a.x >= NEAR, bin = b.x >= NEAR;
            if (ain) cl.push_back(a);
            if (ain != bin) {
                double t = (NEAR - a.x) / (b.x - a.x);
                cl.push_back({NEAR, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t});
            }
        }
        if (cl.size() < 3) return;
        struct S { double x, y, iz; };
        std::vector<S> sp;
        for (auto &c : cl) sp.push_back({(-c.y / c.x / TX * 0.5 + 0.5) * W, (0.5 - c.z / c.x / TY * 0.5) * H, 1 / c.x});
        for (size_t k = 1; k + 1 < sp.size(); k++) {
            const S &A = sp[0], &B = sp[k], &D = sp[k + 1];
            double area = (B.x - A.x) * (D.y - A.y) - (D.x - A.x) * (B.y - A.y);
            if (std::fabs(area) < 1e-12) continue;
            int x0 = std::max(0, (int)std::floor(std::min({A.x, B.x, D.x}))), x1 = std::min(W - 1, (int)std::ceil(std::max({A.x, B.x, D.x})));
            int y0 = std::max(0, (int)std::floor(std::min({A.y, B.y, D.y}))), y1 = std::min(H - 1, (int)std::ceil(std::max({A.y, B.y, D.y})));
            for (int y = y0; y <= y1; y++)
                for (int x = x0; x <= x1; x++) {
                    double px = x + 0.5, py = y + 0.5;
                    double w0 = ((B.x - px) * (D.y - py) - (D.x - px) * (B.y - py)) / area;
                    double w1 = ((D.x - px) * (A.y - py) - (A.x - px) * (D.y - py)) / area;
                    double w2 = 1 - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                    double iz = w0 * A.iz + w1 * B.iz + w2 * D.iz;
                    if (iz <= 0) continue;
                    if (pixel(x, y, (float)(1 / iz))) return;
                }
        }
    };
    // One thread's work: its frames' depth and what each saw.
    struct Work {
        std::vector<float> zb;
        std::vector<char> now, ever;
        std::vector<double> seen_weight;
        double total_weight = 0;
        std::vector<std::vector<std::array<double, 3>>> posed, normals;   // (per part's vertex, posed: camera space)
    };
    auto run_frame = [&](Work &wk, const Frame &fr) {
        auto &posed = wk.posed;
        auto &normals = wk.normals;
        // Whether a triangle's front faces the camera (its front: the side its corners' normals face).
        auto facing = [&](size_t p, size_t t) {
            const auto &P = posed[p], &N = normals[p];
            uint32_t a = parts[p].tris[t], b = parts[p].tris[t + 1], c = parts[p].tris[t + 2];
            double e1[3], e2[3], fn[3], nn[3];
            for (int k = 0; k < 3; k++) {
                e1[k] = P[b][k] - P[a][k];
                e2[k] = P[c][k] - P[a][k];
                nn[k] = N[a][k] + N[b][k] + N[c][k];
            }
            fn[0] = e1[1] * e2[2] - e1[2] * e2[1];
            fn[1] = e1[2] * e2[0] - e1[0] * e2[2];
            fn[2] = e1[0] * e2[1] - e1[1] * e2[0];
            if (fn[0] * nn[0] + fn[1] * nn[1] + fn[2] * nn[2] < 0)
                for (double &x : fn) x = -x;
            return -(fn[0] * P[a][0] + fn[1] * P[a][1] + fn[2] * P[a][2]) > 0;   // (towards the camera, at the origin)
        };
        // The graph's bones posed down from the root (animation_graph_node_matrices_from_orientations), then each
        // model bone's matrix with its rest pose taken out.
        std::vector<M> gm(GN);
        std::vector<char> done(GN, 0);
        std::function<void(int)> pose_node = [&](int n) {
            if (done[n]) return;
            done[n] = 1;
            const animcomp::Pose &o = fr.pose[(size_t)n];
            M local = from_orientation(o.q, o.t, o.s);
            if (gparent[n] >= 0) {
                pose_node(gparent[n]);
                gm[n] = multiply(gm[gparent[n]], local);
            } else {
                gm[n] = local;   // (the root: the camera's frame is the identity here)
            }
        };
        for (int n = 0; n < GN; n++) pose_node(n);
        std::vector<M> rel(MN);
        for (int n = 0; n < MN; n++) rel[n] = multiply(gm[to_graph[n]], rest_inverse[n]);
        for (size_t p = 0; p < parts.size(); p++) {
            auto &P = posed[p];
            auto &N = normals[p];
            P.resize(parts[p].v.size());
            N.resize(parts[p].v.size());
            for (size_t i = 0; i < parts[p].v.size(); i++) {
                const Vertex &v = parts[p].v[i];
                double a[3], an[3], b[3] = {0, 0, 0}, bn[3] = {0, 0, 0};
                point(rel[v.node[0]], v.p, a);
                turn(rel[v.node[0]], v.n, an);
                if (v.node[1] >= 0 && v.weight < 1) {
                    point(rel[v.node[1]], v.p, b);
                    turn(rel[v.node[1]], v.n, bn);
                }
                for (int k = 0; k < 3; k++) {
                    P[i][k] = a[k] * v.weight + b[k] * (1 - v.weight);
                    N[i][k] = an[k] * v.weight + bn[k] * (1 - v.weight);
                }
            }
        }
        // What hides what: the opaque parts' faces towards the camera (both sides of a two-sided one's).
        auto &zb = wk.zb;
        std::fill(zb.begin(), zb.end(), 1e30f);
        for (size_t p = 0; p < parts.size(); p++) {
            if (parts[p].look.kind != lod::ShaderLook::kOpaque) continue;
            for (size_t t = 0; t + 2 < parts[p].tris.size(); t += 3) {
                if (!parts[p].look.two_sided && !facing(p, t)) continue;
                std::array<double, 3> q[3] = {posed[p][parts[p].tris[t]], posed[p][parts[p].tris[t + 1]], posed[p][parts[p].tris[t + 2]]};
                raster(q, [&](int x, int y, float z) {
                    float &d = zb[(size_t)y * W + x];
                    if (z < d) d = z;
                    return false;
                });
            }
        }
        // What's seen: a pixel of it in front of (or at) what hides things there.
        auto &now = wk.now;
        std::fill(now.begin(), now.end(), 0);
        for (size_t p = 0; p < parts.size(); p++)
            for (size_t t = 0; t + 2 < parts[p].tris.size(); t += 3) {
                if (!parts[p].look.two_sided && parts[p].look.kind != lod::ShaderLook::kBlended && !facing(p, t)) continue;
                std::array<double, 3> q[3] = {posed[p][parts[p].tris[t]], posed[p][parts[p].tris[t + 1]], posed[p][parts[p].tris[t + 2]]};
                size_t id = first[p] + t / 3;
                raster(q, [&](int x, int y, float z) {
                    if (z <= zb[(size_t)y * W + x] * 1.002f + 0.0005f) now[id] = 1;
                    return now[id] != 0;
                });
                // (a sliver no pixel's middle falls in: the pixels its corners are in)
                for (int j = 0; j < 3 && !now[id]; j++) {
                    if (q[j][0] < NEAR) continue;
                    double sx = (-q[j][1] / q[j][0] / TX * 0.5 + 0.5) * W, sy = (0.5 - q[j][2] / q[j][0] / TY * 0.5) * H;
                    int x = (int)std::floor(sx), y = (int)std::floor(sy);
                    if (x < 0 || y < 0 || x >= W || y >= H) continue;
                    if ((float)q[j][0] <= zb[(size_t)y * W + x] * 1.002f + 0.0005f) now[id] = 1;
                }
            }
        wk.total_weight += fr.weight;
        for (size_t i = 0; i < T; i++)
            if (now[i]) {
                wk.seen_weight[i] += fr.weight;
                wk.ever[i] = 1;
            }
    };
    // The frames shared out among threads, then added up.
    unsigned threads = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    threads = (unsigned)std::min<size_t>(threads, frames.size());
    std::vector<Work> work(threads);
    std::atomic<size_t> next{0};
    auto worker = [&](Work &wk) {
        wk.zb.assign((size_t)W * H, 1e30f);
        wk.now.assign(T, 0);
        wk.ever.assign(T, 0);
        wk.seen_weight.assign(T, 0);
        wk.posed.resize(parts.size());
        wk.normals.resize(parts.size());
        for (size_t fi; (fi = next++) < frames.size();) run_frame(wk, frames[fi]);
    };
    {
        std::vector<std::thread> pool;
        for (unsigned k = 1; k < threads; k++) pool.emplace_back(worker, std::ref(work[k]));
        worker(work[0]);
        for (auto &t : pool) t.join();
    }
    std::vector<double> seen_weight(T, 0);
    std::vector<char> ever(T, 0);
    double total_weight = 0;
    for (auto &wk : work) {
        total_weight += wk.total_weight;
        for (size_t i = 0; i < T && i < wk.ever.size(); i++) {
            seen_weight[i] += wk.seen_weight[i];
            ever[i] |= wk.ever[i];
        }
    }

    // Next to what's seen counts as seen (any triangle with a corner where a seen one has one).
    {
        std::set<std::array<double, 3>> at;
        auto key = [&](size_t p, uint32_t i) {
            std::array<double, 3> k;
            for (int c = 0; c < 3; c++) k[c] = std::round(parts[p].v[i].p[c] * 1e5) / 1e5;
            return k;
        };
        for (size_t p = 0; p < parts.size(); p++)
            for (size_t t = 0; t + 2 < parts[p].tris.size(); t += 3)
                if (ever[first[p] + t / 3])
                    for (int j = 0; j < 3; j++) at.insert(key(p, parts[p].tris[t + j]));
        std::vector<char> grown = ever;
        for (size_t p = 0; p < parts.size(); p++)
            for (size_t t = 0; t + 2 < parts[p].tris.size(); t += 3) {
                size_t id = first[p] + t / 3;
                if (ever[id]) continue;
                for (int j = 0; j < 3 && !grown[id]; j++) grown[id] = at.count(key(p, parts[p].tris[t + j])) > 0;
            }
        ever = grown;
    }

    // Per geometry, per part, per triangle: the share of the time it's seen (just over 0 for one only next to it).
    for (size_t p = 0; p < parts.size(); p++) {
        auto &g = out.geometry[parts[p].geometry];
        if (g.size() <= (size_t)parts[p].index) g.resize((size_t)parts[p].index + 1);
        auto &v = g[(size_t)parts[p].index];
        v.assign(parts[p].tris.size() / 3, 0);
        for (size_t t = 0; t < v.size(); t++) {
            size_t id = first[p] + t;
            v[t] = !ever[id] ? 0.0f : std::max(1e-4f, (float)(total_weight > 0 ? seen_weight[id] / total_weight : 0));
        }
    }
    size_t never = 0;
    for (char e : ever) never += !e;
    char about[160];
    snprintf(about, sizeof about, "%zu frames of %zu animations (%zu overlay frames); %zu of %zu triangles never on screen",
             frames.size(), anims.size(), frames.size() - base_frames, never, T);
    out.about = about;

    // Pictures (tests): the first frame of up to 8 base animations, small, flat shaded; never seen red.
    if (pictures) {
        pictures->clear();
        std::vector<size_t> picks;
        for (size_t i = 0; i < base_frames && picks.size() < 8; i += std::max<size_t>(1, base_frames / 8)) picks.push_back(i);
        for (size_t fi : picks) {
            const Frame &fr = frames[fi];
            std::vector<M> gm(GN);
            std::vector<char> done(GN, 0);
            std::function<void(int)> pose_node = [&](int n) {
                if (done[n]) return;
                done[n] = 1;
                const animcomp::Pose &o = fr.pose[(size_t)n];
                M local = from_orientation(o.q, o.t, o.s);
                if (gparent[n] >= 0) { pose_node(gparent[n]); gm[n] = multiply(gm[gparent[n]], local); }
                else gm[n] = local;
            };
            for (int n = 0; n < GN; n++) pose_node(n);
            std::vector<uint32_t> px((size_t)pw * ph, 0x303840);
            std::vector<float> z((size_t)pw * ph, 1e30f);
            for (size_t p = 0; p < parts.size(); p++)
                for (size_t t = 0; t + 2 < parts[p].tris.size(); t += 3) {
                    double c[3][3];
                    for (int j = 0; j < 3; j++) {
                        const Vertex &v = parts[p].v[parts[p].tris[t + j]];
                        double a[3], b[3] = {0, 0, 0};
                        point(multiply(gm[to_graph[v.node[0]]], rest_inverse[v.node[0]]), v.p, a);
                        if (v.node[1] >= 0 && v.weight < 1) point(multiply(gm[to_graph[v.node[1]]], rest_inverse[v.node[1]]), v.p, b);
                        for (int k = 0; k < 3; k++) c[j][k] = a[k] * v.weight + b[k] * (1 - v.weight);
                    }
                    if (c[0][0] < NEAR || c[1][0] < NEAR || c[2][0] < NEAR) continue;
                    double sx[3], sy[3];
                    for (int j = 0; j < 3; j++) {
                        sx[j] = (-c[j][1] / c[j][0] / TX * 0.5 + 0.5) * pw;
                        sy[j] = (0.5 - c[j][2] / c[j][0] / TY * 0.5) * ph;
                    }
                    double e1[3], e2[3], fn[3];
                    for (int k = 0; k < 3; k++) { e1[k] = c[1][k] - c[0][k]; e2[k] = c[2][k] - c[0][k]; }
                    fn[0] = e1[1] * e2[2] - e1[2] * e2[1]; fn[1] = e1[2] * e2[0] - e1[0] * e2[2]; fn[2] = e1[0] * e2[1] - e1[1] * e2[0];
                    double fl = std::sqrt(fn[0] * fn[0] + fn[1] * fn[1] + fn[2] * fn[2]);
                    double shade = fl > 0 ? 0.35 + 0.65 * std::fabs(fn[0] + 0.3 * fn[2]) / fl / 1.05 : 0.5;
                    bool never_seen = !ever[first[p] + t / 3];
                    uint32_t col;
                    {
                        int base = never_seen ? 0xD02020 : parts[p].look.kind == lod::ShaderLook::kOpaque ? 0xB0B8C0 : 0x60A0E0;
                        int r = (int)(((base >> 16) & 255) * shade), g = (int)(((base >> 8) & 255) * shade), b = (int)((base & 255) * shade);
                        col = (uint32_t)(r << 16 | g << 8 | b);
                    }
                    double area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
                    if (std::fabs(area) < 1e-12) continue;
                    int x0 = std::max(0, (int)std::min({sx[0], sx[1], sx[2]})), x1 = std::min(pw - 1, (int)std::max({sx[0], sx[1], sx[2]}) + 1);
                    int y0 = std::max(0, (int)std::min({sy[0], sy[1], sy[2]})), y1 = std::min(ph - 1, (int)std::max({sy[0], sy[1], sy[2]}) + 1);
                    for (int y = y0; y <= y1; y++)
                        for (int x = x0; x <= x1; x++) {
                            double qx = x + 0.5, qy = y + 0.5;
                            double w0 = ((sx[1] - qx) * (sy[2] - qy) - (sx[2] - qx) * (sy[1] - qy)) / area;
                            double w1 = ((sx[2] - qx) * (sy[0] - qy) - (sx[0] - qx) * (sy[2] - qy)) / area;
                            double w2 = 1 - w0 - w1;
                            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                            float d = (float)(w0 * c[0][0] + w1 * c[1][0] + w2 * c[2][0]);
                            size_t o = (size_t)y * pw + x;
                            if (d < z[o]) { z[o] = d; px[o] = col; }
                        }
                }
            pictures->push_back(std::move(px));
        }
    }
    return true;
}

}   // namespace fpview
