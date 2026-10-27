// Halo Map Porter - showcase flythroughs (showcase.hpp).
#include "showcase.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <tuple>

namespace showcase {

namespace {

constexpr double kPi = 3.14159265358979;
V3 add(V3 a, V3 b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 sub(V3 a, V3 b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 mul(V3 a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(V3 a, V3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 cross(V3 a, V3 b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double len(V3 a) { return std::sqrt(dot(a, a)); }
double len2d(V3 a) { return std::sqrt(a[0] * a[0] + a[1] * a[1]); }
V3 lerp(V3 a, V3 b, double u) { return add(a, mul(sub(b, a), u)); }
double deg(double r) { return r * 180 / kPi; }
double rad(double d) { return d * kPi / 180; }
V3 dir_of(double yaw_deg, double pitch_deg) {
    double y = rad(yaw_deg), p = rad(pitch_deg);
    return {std::cos(y) * std::cos(p), std::sin(y) * std::cos(p), std::sin(p)};
}
// A soft start and end: 60% of the way at an even pace, the rest eased in and out (zero speed never: shots cut).
double ease(double x) {
    x = std::clamp(x, 0.0, 1.0);
    return 0.6 * x + 0.4 * (1 - std::cos(kPi * x)) / 2;
}

constexpr double kEye = 0.62;   // a player's eyes above the ground (as the measured views have them)

// A point a tenth of a unit from `to` towards `from`: what's looked at can be on a surface (picked in the picture), and
// seeing it is seeing up to it.
V3 short_of(V3 from, V3 to) {
    V3 d = sub(from, to);
    double l = len(d);
    return l > 0.2 ? add(to, mul(d, 0.1 / l)) : to;
}

// ---- the level as the camera sees it: a grid of its triangles, its rooms, and the placed objects' boxes
struct World {
    const lshade::Preview &p;
    const double cell = 3.0;
    V3 lo{}, hi{};
    int nx = 1, ny = 1, nz = 1;
    std::vector<std::vector<int>> cells;
    std::vector<char> opaque;                // per triangle: hides what's behind it (not see-through)
    mutable std::vector<int> stamp;
    mutable int stamp_now = 0;

    explicit World(const lshade::Preview &pv) : p(pv) {
        lo = {1e30, 1e30, 1e30};
        hi = {-1e30, -1e30, -1e30};
        for (auto &t : p.triangles)
            for (auto &v : t.v)
                for (int k = 0; k < 3; k++) { lo[k] = std::min(lo[k], (double)v.p[k]); hi[k] = std::max(hi[k], (double)v.p[k]); }
        if (lo[0] > hi[0]) { lo = hi = {0, 0, 0}; }
        for (int k = 0; k < 3; k++) { lo[k] -= 1; hi[k] += 1; }
        nx = std::max(1, (int)std::ceil((hi[0] - lo[0]) / cell));
        ny = std::max(1, (int)std::ceil((hi[1] - lo[1]) / cell));
        nz = std::max(1, (int)std::ceil((hi[2] - lo[2]) / cell));
        while ((long long)nx * ny * nz > 4000000) { nx = (nx + 1) / 2; ny = (ny + 1) / 2; nz = (nz + 1) / 2; }   // (a huge level)
        cells.assign((size_t)nx * ny * nz, {});
        opaque.assign(p.triangles.size(), 1);
        for (size_t i = 0; i < p.triangles.size(); i++) {
            const auto &t = p.triangles[i];
            if (t.material < 0 || (size_t)t.material >= p.materials.size()) { opaque[i] = 0; continue; }
            const auto &m = p.materials[(size_t)t.material];
            if (m.see_through) opaque[i] = 0;
            double a[3] = {1e30, 1e30, 1e30}, b[3] = {-1e30, -1e30, -1e30};
            for (auto &v : t.v)
                for (int k = 0; k < 3; k++) { a[k] = std::min(a[k], (double)v.p[k]); b[k] = std::max(b[k], (double)v.p[k]); }
            int x0, y0, z0, x1, y1, z1;
            cell_of({a[0], a[1], a[2]}, x0, y0, z0);
            cell_of({b[0], b[1], b[2]}, x1, y1, z1);
            for (int z = z0; z <= z1; z++)
                for (int y = y0; y <= y1; y++)
                    for (int x = x0; x <= x1; x++) cells[((size_t)z * ny + y) * nx + x].push_back((int)i);
        }
        stamp.assign(p.triangles.size(), 0);
    }
    void cell_of(V3 q, int &x, int &y, int &z) const {
        x = std::clamp((int)((q[0] - lo[0]) / (hi[0] - lo[0]) * nx), 0, nx - 1);
        y = std::clamp((int)((q[1] - lo[1]) / (hi[1] - lo[1]) * ny), 0, ny - 1);
        z = std::clamp((int)((q[2] - lo[2]) / (hi[2] - lo[2]) * nz), 0, nz - 1);
    }
    // Each triangle in the cells around the box from `a` to `b` grown by `r`, once.
    template <class F> void near(V3 a, V3 b, double r, F &&f) const {
        if (++stamp_now == 0x7fffffff) { std::fill(stamp.begin(), stamp.end(), 0); stamp_now = 1; }
        int x0, y0, z0, x1, y1, z1;
        cell_of({std::min(a[0], b[0]) - r, std::min(a[1], b[1]) - r, std::min(a[2], b[2]) - r}, x0, y0, z0);
        cell_of({std::max(a[0], b[0]) + r, std::max(a[1], b[1]) + r, std::max(a[2], b[2]) + r}, x1, y1, z1);
        for (int z = z0; z <= z1; z++)
            for (int y = y0; y <= y1; y++)
                for (int x = x0; x <= x1; x++)
                    for (int i : cells[((size_t)z * ny + y) * nx + x]) {
                        if (stamp[(size_t)i] == stamp_now) continue;
                        stamp[(size_t)i] = stamp_now;
                        f(i);
                    }
    }
    V3 corner(int i, int k) const { const auto &v = p.triangles[(size_t)i].v[k].p; return {v[0], v[1], v[2]}; }
    // Where segment a -> b first crosses triangle i (0..1), or -1.
    double cross_at(int i, V3 a, V3 b) const {
        V3 v0 = corner(i, 0), e1 = sub(corner(i, 1), v0), e2 = sub(corner(i, 2), v0), d = sub(b, a);
        V3 pv = cross(d, e2);
        double det = dot(e1, pv);
        if (std::fabs(det) < 1e-12) return -1;
        V3 tv = sub(a, v0);
        double u = dot(tv, pv) / det;
        if (u < 0 || u > 1) return -1;
        V3 q = cross(tv, e1);
        double v = dot(d, q) / det;
        if (v < 0 || u + v > 1) return -1;
        double t = dot(e2, q) / det;
        return t >= 0 && t <= 1 ? t : -1;
    }
    // The distance from q to triangle i.
    double distance(int i, V3 q) const {
        V3 a = corner(i, 0), b = corner(i, 1), c = corner(i, 2);
        V3 ab = sub(b, a), ac = sub(c, a), ap = sub(q, a);
        double d1 = dot(ab, ap), d2 = dot(ac, ap);
        V3 cp;
        if (d1 <= 0 && d2 <= 0) cp = a;
        else {
            V3 bp = sub(q, b);
            double d3 = dot(ab, bp), d4 = dot(ac, bp);
            if (d3 >= 0 && d4 <= d3) cp = b;
            else {
                double vc = d1 * d4 - d3 * d2;
                if (vc <= 0 && d1 >= 0 && d3 <= 0) cp = add(a, mul(ab, d1 / (d1 - d3)));
                else {
                    V3 cq = sub(q, c);
                    double d5 = dot(ab, cq), d6 = dot(ac, cq);
                    if (d6 >= 0 && d5 <= d6) cp = c;
                    else {
                        double vb = d5 * d2 - d1 * d6;
                        if (vb <= 0 && d2 >= 0 && d6 <= 0) cp = add(a, mul(ac, d2 / (d2 - d6)));
                        else {
                            double va = d3 * d6 - d5 * d4;
                            if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0)
                                cp = add(b, mul(sub(c, b), (d4 - d3) / ((d4 - d3) + (d5 - d6))));
                            else {
                                double den = 1 / (va + vb + vc);
                                cp = add(a, add(mul(ab, vb * den), mul(ac, vc * den)));
                            }
                        }
                    }
                }
            }
        }
        return len(sub(q, cp));
    }
    // A placed object's box: q inside it grown by r; the segment's first entry into it (0..1, -1 none).
    static void local(const lshade::Box &b, V3 q, double out[3]) {
        double dx = q[0] - b.c[0], dy = q[1] - b.c[1];
        out[0] = dx * b.axis[0][0] + dy * b.axis[0][1];
        out[1] = dx * b.axis[1][0] + dy * b.axis[1][1];
        out[2] = q[2] - b.c[2];
    }
    static bool in_box(const lshade::Box &b, V3 q, double r) {
        double l[3];
        local(b, q, l);
        for (int k = 0; k < 3; k++)
            if (std::fabs(l[k]) > b.half[k] + r) return false;
        return true;
    }
    static double box_hit(const lshade::Box &b, V3 a, V3 e) {
        double la[3], le[3];
        local(b, a, la);
        local(b, e, le);
        double t0 = 0, t1 = 1;
        for (int k = 0; k < 3; k++) {
            double d = le[k] - la[k];
            if (std::fabs(d) < 1e-12) {
                if (std::fabs(la[k]) > b.half[k]) return -1;
                continue;
            }
            double u = (-b.half[k] - la[k]) / d, v = (b.half[k] - la[k]) / d;
            if (u > v) std::swap(u, v);
            t0 = std::max(t0, u);
            t1 = std::min(t1, v);
            if (t0 > t1) return -1;
        }
        return t0;
    }

    // Somewhere the camera can be: inside the level (in a room, not in a wall), `margin` from any surface or object.
    bool open(V3 q, double margin = 0.6) const {
        if (p.room_at(q) < 0) return false;
        for (auto &b : p.boxes)
            if (in_box(b, q, margin)) return false;
        bool ok = true;
        near(q, q, margin, [&](int i) { if (ok && distance(i, q) < margin) ok = false; });
        return ok;
    }
    // Nothing between a and b: no surface (`all`: see-through ones too) and no object's box.
    bool sees(V3 a, V3 b, bool all = false) const {
        V3 d = sub(b, a);
        double L = len(d);
        int steps = std::max(1, (int)std::ceil(L / cell));
        for (auto &bx : p.boxes)
            if (box_hit(bx, a, b) >= 0) return false;
        bool hit = false;
        for (int s = 0; s < steps && !hit; s++) {
            V3 s0 = add(a, mul(d, (double)s / steps)), s1 = add(a, mul(d, (double)(s + 1) / steps));
            near(s0, s1, 0.01, [&](int i) {
                if (!hit && (all || opaque[(size_t)i]) && cross_at(i, a, b) >= 0) hit = true;
            });
        }
        return !hit;
    }
    // How far a ray goes from `a` along `dir` (unit) before a surface or an object, up to `most`.
    double reach(V3 a, V3 dir, double most) const {
        V3 b = add(a, mul(dir, most));
        double best = 1;
        for (auto &bx : p.boxes) {
            double t = box_hit(bx, a, b);
            if (t >= 0) best = std::min(best, t);
        }
        int steps = std::max(1, (int)std::ceil(most / cell));
        for (int s = 0; s < steps; s++) {
            double f0 = (double)s / steps, f1 = (double)(s + 1) / steps;
            if (best <= f0) break;
            near(add(a, mul(sub(b, a), f0)), add(a, mul(sub(b, a), f1)), 0.01, [&](int i) {
                double t = cross_at(i, a, b);
                if (t >= 0 && t < best) best = t;
            });
            if (best <= f1) break;
        }
        return best * most;
    }
};

// A shot made from a function of its progress (0..1): the camera's position and, without a look point, direction.
// Keys every `every` seconds (and at the end).
Shot keyed(const std::string &label, double seconds, double every, const std::function<Key(double)> &at_u) {
    Shot s;
    s.label = label.substr(0, 31);
    s.seconds = seconds;
    int n = std::max(2, (int)std::ceil(seconds / every) + 1);
    for (int i = 0; i < n; i++) {
        double t = seconds * i / (n - 1);
        Key k = at_u(t / seconds);
        k.t = t;
        s.keys.push_back(k);
    }
    return s;
}

// The shot is flyable: every quarter second the camera is somewhere open and saw no wall on its way there; with a look
// point, it sees it at the start, at the end and at least `seen` of the time. Returns the share of the time it sees
// it (1 without one), or -1.
// (`inside`: where the camera may be, else anywhere open. What it looks at it sees through nothing, not even glass.)
double flyable(const World &w, const Shot &s, double seen = 0.9, const std::function<bool(const V3 &)> &inside = nullptr,
               double margin = 0.6) {
    V3 last{};
    int n = 0, sees = 0;
    bool first = true, now = true;
    for (double t = 0; t <= s.seconds + 1e-9; t += 0.25) {
        View v = at(s, t);
        if (!w.open(v.eye, margin) || (inside && !inside(v.eye))) return -1;
        if (n && !w.sees(last, v.eye, true)) return -1;
        now = !s.look || w.sees(v.eye, short_of(v.eye, s.at), true);
        if (n == 0) first = now;
        if (now) sees++;
        last = v.eye;
        n++;
    }
    if (!s.look) return 1;
    double share = n ? (double)sees / n : 0;
    return first && now && share >= seen ? share : -1;
}

double yaw_to(V3 from, V3 to) { return deg(std::atan2(to[1] - from[1], to[0] - from[0])); }

// The shot looks out at something, not into a wall: at its start, middle and end, what's ahead is at least `most`
// units away.
bool sees_out(const World &w, const Shot &s, double most = 6) {
    for (double u : {0.0, 0.5, 1.0}) {
        View v = at(s, s.seconds * u);
        if (w.reach(v.eye, dir_of(v.yaw, v.pitch), most) < most - 1e-6) return false;
    }
    return true;
}


// ---- routes: the camera through open air from one place to another (flag to flag, hill to hill, oddball), round
// the walls in its way. Its stops: over every floor of the box where players play, on a grid (straight down from the
// top of the box, every surface met, each floor's air a couple of units up where the camera can be); two stops next to
// each other join when nothing is between them and the camera stays clear of every surface on the way.
struct Router {
    const World &w;
    std::function<bool(const V3 &)> inside;
    std::vector<V3> nodes;
    std::vector<std::vector<std::pair<int, double>>> near_nodes;   // (each stop's neighbours, by distance)
    std::map<std::pair<int, int>, bool> known;
    double step = 3, margin = 0.4;
    int checks = 0;

    Router(const World &wd, V3 lo, V3 hi, std::function<bool(const V3 &)> in, double across) : w(wd), inside(std::move(in)) {
        step = std::clamp(across / 70, 1.8, 4.0);
        const int nx = std::max(1, (int)std::ceil((hi[0] - lo[0]) / step)), ny = std::max(1, (int)std::ceil((hi[1] - lo[1]) / step));
        if ((long long)nx * ny > 60000) step *= std::sqrt((double)nx * ny / 60000.0);   // (a huge box: a coarser grid)
        std::map<std::pair<int, int>, std::vector<int>> column;
        for (int ix = 0; lo[0] + ix * step <= hi[0]; ix++)
            for (int iy = 0; lo[1] + iy * step <= hi[1]; iy++) {
                double x = lo[0] + ix * step, y = lo[1] + iy * step, z = hi[2];
                for (int floors = 0; floors < 8 && z > lo[2]; floors++) {
                    double most = z - lo[2], d = w.reach({x, y, z}, {0, 0, -1}, most);
                    if (d >= most - 1e-6) break;
                    double floor_z = z - d;
                    for (double h : {1.5, 2.8}) {
                        V3 n{x, y, floor_z + h};
                        if ((inside && !inside(n)) || !w.open(n, 0.45)) continue;
                        column[{ix, iy}].push_back((int)nodes.size());
                        nodes.push_back(n);
                    }
                    z = floor_z - 0.05;
                }
            }
        near_nodes.assign(nodes.size(), {});
        const double reach = step * 1.5;
        for (auto &[c, ids] : column)
            for (int dx = -1; dx <= 1; dx++)
                for (int dy = -1; dy <= 1; dy++) {
                    auto it = column.find({c.first + dx, c.second + dy});
                    if (it == column.end()) continue;
                    for (int i : ids)
                        for (int j : it->second) {
                            if (j <= i) continue;
                            V3 d = sub(nodes[(size_t)j], nodes[(size_t)i]);
                            double l = len(d);
                            if (l > reach || std::fabs(d[2]) > 3.2 || l < 0.3) continue;
                            near_nodes[(size_t)i].push_back({j, l});
                            near_nodes[(size_t)j].push_back({i, l});
                        }
                }
    }
    // The camera can go straight from a to b: no surface between (glass neither) and clear of them all the way.
    bool clear(V3 a, V3 b) const {
        if (!w.sees(a, b, true)) return false;
        double L = len(sub(b, a));
        int n = std::max(1, (int)std::ceil(L / 0.8));
        for (int k = 1; k < n; k++) {
            V3 q = lerp(a, b, (double)k / n);
            if (!w.open(q, margin) || (inside && !inside(q))) return false;
        }
        return true;
    }
    bool edge(int i, int j) {
        auto key = std::make_pair(std::min(i, j), std::max(i, j));
        auto it = known.find(key);
        if (it != known.end()) return it->second;
        checks++;
        return known[key] = clear(nodes[(size_t)i], nodes[(size_t)j]);
    }
    // The stops nearest q that it reaches in a straight line (the route's ends join the network there).
    std::vector<int> joins(V3 q) const {
        std::vector<std::pair<double, int>> by;
        for (size_t i = 0; i < nodes.size(); i++) {
            double d = len(sub(nodes[i], q));
            if (d <= step * 2.5) by.push_back({d, (int)i});
        }
        std::sort(by.begin(), by.end());
        std::vector<int> out;
        for (auto &[d, i] : by) {
            if (out.size() >= 4) break;
            if (clear(q, nodes[(size_t)i])) out.push_back(i);
        }
        return out;
    }
    // A route from a to b (both where the camera can be), as corners; empty when there's none. A* over the stops, each
    // join checked only when the search takes it (lazily), then straightened (each corner to the farthest it sees).
    std::vector<V3> route(V3 a, V3 b) {
        if (clear(a, b)) return {a, b};
        std::vector<int> from = joins(a), to = joins(b);
        if (from.empty() || to.empty()) return {};
        std::set<int> goal(to.begin(), to.end());
        const int N = (int)nodes.size();
        std::vector<double> g(N, 1e30);
        std::vector<int> parent(N, -2);   // (-1: joined from a)
        std::vector<char> closed(N, 0);
        using Entry = std::tuple<double, double, int, int>;   // f, g, node, from
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
        auto h = [&](int i) { return len(sub(b, nodes[(size_t)i])); };
        for (int i : from) open.push({len(sub(nodes[(size_t)i], a)) + h(i), len(sub(nodes[(size_t)i], a)), i, -1});
        int end = -1, pops = 0;
        while (!open.empty() && pops < 60000 && checks < 30000) {
            auto [f, gi, i, par] = open.top();
            open.pop();
            if (closed[(size_t)i]) continue;
            pops++;
            if (par >= 0 && !edge(par, i)) continue;   // (lazily: this way in is blocked; another may not be)
            closed[(size_t)i] = 1;
            g[(size_t)i] = gi;
            parent[(size_t)i] = par;
            if (goal.count(i)) { end = i; break; }
            for (auto &[j, d] : near_nodes[(size_t)i]) {
                if (closed[(size_t)j]) continue;
                double gj = gi + d;
                open.push({gj + h(j), gj, j, i});
            }
        }
        if (end < 0) return {};
        std::vector<V3> pts{b};
        for (int i = end; i >= 0; i = parent[(size_t)i]) pts.push_back(nodes[(size_t)i]);
        pts.push_back(a);
        std::reverse(pts.begin(), pts.end());
        std::vector<V3> out{pts[0]};
        for (size_t i = 0; i + 1 < pts.size();) {
            size_t j = pts.size() - 1;
            while (j > i + 1 && !clear(pts[i], pts[j])) j--;
            out.push_back(pts[j]);
            i = j;
        }
        return out;
    }
};

// The length along corners, and the point `s` units along them (past the end: on along the last leg).
double route_length(const std::vector<V3> &c) {
    double L = 0;
    for (size_t i = 0; i + 1 < c.size(); i++) L += len(sub(c[i + 1], c[i]));
    return L;
}
V3 along(const std::vector<V3> &c, double s) {
    for (size_t i = 0; i + 1 < c.size(); i++) {
        double l = len(sub(c[i + 1], c[i]));
        if (s <= l || i + 2 == c.size()) return l > 1e-9 ? lerp(c[i], c[i + 1], s / l) : c[i];
        s -= l;
    }
    return c.empty() ? V3{} : c.back();
}

// A shot along corners at about `speed` units a second (at an even pace: Pace::even), looking ahead along the way and
// a little down, then, over its last stretch, at `goal` (where the route goes: a flag, a hill), stopping a few units
// short of the end so it's in view. Its path (Shot::via): at every corner, half a unit either side of it, and every 4
// units between (for where it looks), so the curve keeps to the straight legs and only rounds the corners a little;
// its keys are timed along that (retime).
Shot route_shot(const World &w, std::vector<V3> c, double speed, double down, V3 goal, const std::string &label) {
    const double full = route_length(c), ahead = std::clamp(speed * 1.6, 5.0, 14.0);
    double stop = std::min(5.0, full * 0.2);
    for (double d : {5.0, 7.0, 9.0, 3.5, 12.0})   // (where the goal's in view, when it is from somewhere near the end)
        if (d <= full * 0.35 && w.sees(along(c, full - d), short_of(along(c, full - d), goal), true)) { stop = d; break; }
    {   // (the corners up to the stop)
        std::vector<V3> cut{c[0]};
        double want = full - stop, sofar = 0;
        for (size_t i = 1; i < c.size() && sofar < want - 1e-9; i++) {
            double l = len(sub(c[i], c[i - 1]));
            cut.push_back(sofar + l <= want ? c[i] : lerp(c[i - 1], c[i], (want - sofar) / l));
            sofar += l;
        }
        c = cut;
    }
    const double L = route_length(c), seconds = std::clamp(L / speed, 6.0, 40.0);
    const V3 end = c.back();
    std::vector<double> at_s{0};
    double sofar = 0;
    for (size_t i = 0; i + 1 < c.size(); i++) {
        double l = len(sub(c[i + 1], c[i]));
        double a = sofar + std::min(0.5, l / 3), b = sofar + l - std::min(0.5, l / 3);
        at_s.push_back(a);
        int n = std::max(1, (int)std::ceil((b - a) / 2.0));
        for (int k = 1; k <= n; k++) at_s.push_back(a + (b - a) * k / n);
        sofar += l;
        at_s.push_back(sofar);
    }
    std::sort(at_s.begin(), at_s.end());
    at_s.erase(std::unique(at_s.begin(), at_s.end(), [](double x, double y) { return y - x < 0.05; }), at_s.end());
    if (at_s.back() < L - 0.05) at_s.push_back(L);
    Shot sh;
    sh.label = label.substr(0, 31);
    sh.seconds = seconds;
    for (double s : at_s) {
        Key k;
        k.p = along(c, s);
        // (ahead along the way as far as it's in view: round a corner, at the way out of it, not at the wall)
        double look = ahead;
        while (look > 1.5 && !w.sees(k.p, along(c, s + look), false)) look -= 1.0;
        V3 f = add(along(c, s + look), {0, 0, -down * look / ahead});
        if (len2d(sub(f, k.p)) < 0.5) f = add(k.p, sub(end, along(c, std::max(0.0, L - 1))));
        double x = std::clamp((s - (L - ahead)) / std::max(1.0, ahead), 0.0, 1.0), g = x * x * (3 - 2 * x);   // (to the goal)
        V3 to_goal = sub(goal, k.p);
        if (len2d(to_goal) > 0.5 && g > 0) {
            V3 fa = mul(sub(f, k.p), 1 / std::max(1e-6, len(sub(f, k.p)))), fg = mul(to_goal, 1 / len(to_goal));
            f = add(k.p, lerp(fa, fg, g));
        }
        k.yaw = yaw_to(k.p, f);
        k.pitch = std::clamp(deg(std::atan2(f[2] - k.p[2], std::max(1e-6, len2d(sub(f, k.p))))), -35.0, 8.0);
        sh.via.push_back(k);
    }
    sh.pace = Pace::even;
    retime(sh);
    return sh;
}

// Where the camera can start or end a route at a place on the ground: over it, or beside it when that's too tight (a
// flag in a doorway, under a low roof), somewhere open that sees it; the nearest first, at most 12.
std::vector<V3> camera_spots(const World &w, V3 ground, const std::function<bool(const V3 &)> &inside) {
    std::vector<V3> out;
    V3 look = add(ground, {0, 0, 0.3});
    for (double r : {0.0, 0.8, 1.6, 2.4})
        for (double h : {2.2, 1.4, 3.2, 0.9})
            for (int a = 0; a < (r > 0 ? 8 : 1) && out.size() < 12; a++) {
                V3 q = add(ground, {r * std::cos(a * kPi / 4), r * std::sin(a * kPi / 4), h});
                if (w.open(q, 0.4) && (!inside || inside(q)) && w.sees(q, look, true)) out.push_back(q);
            }
    return out;
}

// Looking straight down: every part of the picture (a grid of directions over a widescreen or 4:3 view, 70 degrees
// across) meets the level within `most` units, so none of it is the sky or the void past the level's edge.
bool all_level_below(const World &w, const View &v, double most) {
    V3 f = dir_of(v.yaw, v.pitch), r = cross(f, {0, 0, 1});
    if (len(r) < 1e-6) r = {1, 0, 0};
    r = mul(r, 1 / len(r));
    V3 u = cross(r, f);
    const double tx = std::tan(rad(v.fov / 2)), ty = tx * 0.75;
    for (int i = 0; i <= 8; i++)
        for (int j = 0; j <= 6; j++) {
            V3 d = add(f, add(mul(r, tx * (i / 4.0 - 1)), mul(u, ty * (j / 3.0 - 1))));
            d = mul(d, 1 / len(d));
            if (w.reach(v.eye, d, most) >= most - 1e-6) return false;
        }
    return true;
}
}   // namespace

Shot orbit_shot(const V3 &at, double radius, double height, double start, double sweep, double seconds,
                const std::string &label) {
    radius = std::clamp(radius, 0.5, 200.0);
    seconds = std::clamp(seconds, 0.5, 600.0);
    // (a key at least every 1.5 seconds and every 15 degrees: the curve between them stays on the circle)
    double every = std::min(1.5, seconds / std::max(2.0, std::ceil(std::fabs(sweep) / 15)));
    Shot s = keyed(label, seconds, every, [&](double u) {
        double a = rad(start + sweep * ease(u));
        Key k;
        k.p = {at[0] + radius * std::cos(a), at[1] + radius * std::sin(a), at[2] + height};
        k.yaw = yaw_to(k.p, at);
        k.pitch = deg(std::atan2(at[2] - k.p[2], len2d(sub(at, k.p))));
        return k;
    });
    s.look = true;
    s.at = at;
    s.orbit = {true, radius, height, start, sweep};
    return s;
}

std::string trouble(const lshade::Preview &p, const Shot &s) {
    if (s.keys.empty() || p.triangles.empty()) return "";
    World w(p);
    V3 last{};
    int n = 0, sees = 0;
    double wall = -1, inside = -1;
    for (double t = 0; t <= s.seconds + 1e-9; t += 0.25) {
        View v = at(s, t);
        if (inside < 0 && p.room_at(v.eye) < 0) inside = t;
        if (wall < 0 && n && !w.sees(last, v.eye, true)) wall = t;
        if (!s.look || w.sees(v.eye, short_of(v.eye, s.at), true)) sees++;
        last = v.eye;
        n++;
    }
    char b[200];
    if (inside >= 0) {
        snprintf(b, sizeof b, "the camera is outside the level or inside a wall %.1f seconds in", inside);
        return b;
    }
    if (wall >= 0) {
        snprintf(b, sizeof b, "the camera goes through a surface or an object's box %.1f seconds in", wall);
        return b;
    }
    if (s.look && n && sees < n * 0.85) {
        snprintf(b, sizeof b, "something is between the camera and what it looks at for %.0f%% of the shot", 100.0 * (n - sees) / n);
        return b;
    }
    return "";
}

double Showcase::length() const {
    double t = 0;
    for (auto &s : shots) t += s.seconds;
    return t;
}

// ---- the file
std::string write(const Showcase &s, const std::string &map, const std::string &made) {
    std::string out = "# Halo 1 Unlimited showcase v1 - " + map + ". Made by Halo Map Porter, " + made +
                      " (it writes this file again with each build: to keep your own changes, take \"Halo Map Porter\" "
                      "out of this line).\r\n"
                      "# Shots play in order, then start again. World units; degrees (yaw 0 = +x, counterclockwise; "
                      "pitch up positive).\r\n"
                      "# shot <seconds> <label> / look <x y z> (optional) / key <seconds into the shot> <x y z> <yaw> "
                      "<pitch> [field of view]\r\n";
    char b[200];
    snprintf(b, sizeof b, "fade %.2f\r\n", s.fade);
    out += b;
    for (auto &sh : s.shots) {
        snprintf(b, sizeof b, "\r\nshot %.2f %s\r\n", sh.seconds, clean_label(sh.label).c_str());
        out += b;
        if (sh.look) {
            snprintf(b, sizeof b, "look %.2f %.2f %.2f\r\n", sh.at[0], sh.at[1], sh.at[2]);
            out += b;
        }
        if (sh.look && sh.orbit.on) {   // (for the Level window: an orbit it can change; a comment to Halo 1 Unlimited)
            snprintf(b, sizeof b, "# orbit %.2f %.2f %.1f %.1f\r\n", sh.orbit.radius, sh.orbit.height, sh.orbit.start, sh.orbit.sweep);
            out += b;
        }
        if (sh.pace != Pace::keyed && !sh.via.empty()) {   // (the path the keys are timed along: retime)
            out += std::string("# pace ") + pace_name(sh.pace) + "\r\n";
            for (auto &k : sh.via) {
                double y = std::fmod(k.yaw, 360.0);
                if (y < 0) y += 360;
                if (std::fabs(k.fov - kFov) > 0.05)
                    snprintf(b, sizeof b, "# via %.2f %.2f %.2f %.1f %.1f %.1f\r\n", k.p[0], k.p[1], k.p[2], y, k.pitch, k.fov);
                else
                    snprintf(b, sizeof b, "# via %.2f %.2f %.2f %.1f %.1f\r\n", k.p[0], k.p[1], k.p[2], y, k.pitch);
                out += b;
            }
        }
        for (auto &k : sh.keys) {
            double y = std::fmod(k.yaw, 360.0);
            if (y < 0) y += 360;
            if (std::fabs(k.fov - kFov) > 0.05)
                snprintf(b, sizeof b, "key %.2f %.2f %.2f %.2f %.1f %.1f %.1f\r\n", k.t, k.p[0], k.p[1], k.p[2], y, k.pitch, k.fov);
            else
                snprintf(b, sizeof b, "key %.2f %.2f %.2f %.2f %.1f %.1f\r\n", k.t, k.p[0], k.p[1], k.p[2], y, k.pitch);
            out += b;
        }
    }
    return out;
}

std::string clean_label(const std::string &label) {
    std::string out;
    for (char c : label) {
        if (c == '#' || ((unsigned char)c < 32 && c != '\t') || (unsigned char)c >= 127) continue;   // (plain ASCII: what Halo's font has)
        out += c == '\t' || c == ',' || c == ';' ? ' ' : c;
    }
    size_t a = out.find_first_not_of(' ');
    out = a == std::string::npos ? "" : out.substr(a, 31);
    size_t b = out.find_last_not_of(' ');
    return b == std::string::npos ? "" : out.substr(0, b + 1);
}

bool ours(const std::string &text) {
    size_t e = text.find('\n');
    std::string first = text.substr(0, e);
    return first.rfind("# Halo 1 Unlimited showcase", 0) == 0 && first.find("Halo Map Porter") != std::string::npos;
}

bool read(const std::string &text, Showcase &out, std::string *error) {
    out = Showcase();
    std::istringstream in(text);
    std::string line;
    int n = 0;
    auto fail = [&](const std::string &why) {
        if (error) *error = "line " + std::to_string(n) + ": " + why;
        return false;
    };
    while (std::getline(in, line)) {
        n++;
        {   // (the Level window's own comment: the shot before it is an orbit, radius height start sweep)
            std::istringstream cs(line);
            std::string h, w;
            Shot::Orbit o;
            if (cs >> h >> w && h == "#" && w == "orbit" && !out.shots.empty() && (cs >> o.radius >> o.height >> o.start >> o.sweep)) {
                o.on = true;
                out.shots.back().orbit = o;
            }
            // (its pace, and the path its keys are timed along: retime)
            std::string pn;
            if (h == "#" && w == "pace" && !out.shots.empty() && cs >> pn) pace_from(pn, out.shots.back().pace);
            Key k;
            if (h == "#" && w == "via" && !out.shots.empty() && (cs >> k.p[0] >> k.p[1] >> k.p[2])) {
                if (!(cs >> k.yaw)) k.yaw = 0;
                if (!(cs >> k.pitch)) k.pitch = 0;
                if (!(cs >> k.fov) || k.fov < 10 || k.fov > 150) k.fov = kFov;
                out.shots.back().via.push_back(k);
            }
        }
        size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        for (auto &c : line)
            if (c == ',' || c == ';' || c == '\t' || c == '\r') c = ' ';
        std::istringstream ls(line);
        std::string word;
        if (!(ls >> word)) continue;
        if (word == "fade") {
            if (!(ls >> out.fade) || out.fade < 0 || out.fade > 5) return fail("fade needs seconds (0 to 5)");
        } else if (word == "shot") {
            Shot s;
            if (!(ls >> s.seconds) || s.seconds <= 0 || s.seconds > 600) return fail("shot needs its seconds (up to 600)");
            std::string rest;
            std::getline(ls, rest);
            size_t a = rest.find_first_not_of(' '), b = rest.find_last_not_of(' ');
            s.label = a == std::string::npos ? "" : rest.substr(a, b - a + 1).substr(0, 31);
            out.shots.push_back(s);
        } else if (word == "look") {
            if (out.shots.empty()) return fail("look before any shot");
            Shot &s = out.shots.back();
            if (!(ls >> s.at[0] >> s.at[1] >> s.at[2])) return fail("look needs x y z");
            s.look = true;
        } else if (word == "key") {
            if (out.shots.empty()) return fail("key before any shot");
            Key k;
            if (!(ls >> k.t >> k.p[0] >> k.p[1] >> k.p[2])) return fail("key needs its time and x y z");
            if (!(ls >> k.yaw)) k.yaw = 0;
            if (!(ls >> k.pitch)) k.pitch = 0;
            if (!(ls >> k.fov) || k.fov < 10 || k.fov > 150) k.fov = kFov;
            Shot &s = out.shots.back();
            if (!s.keys.empty() && k.t < s.keys.back().t) return fail("keys go in time order");
            s.keys.push_back(k);
        } else {
            return fail("\"" + word + "\" isn't fade, shot, look or key");
        }
    }
    for (auto &s : out.shots) {
        if (s.keys.empty()) return fail("shot \"" + s.label + "\" has no keys");
        if (!s.look) s.orbit.on = false;
        if (s.via.empty()) s.pace = Pace::keyed;
        if (s.pace == Pace::keyed) s.via.clear();
        if (s.pace != Pace::keyed) {   // (when it gets to each via key: as retime worked it out)
            Shot t = s;
            retime(t);
            for (size_t i = 0; i < s.via.size(); i++) s.via[i].t = t.via[i].t;
        }
    }
    return true;
}

// ---- playback
View at(const Shot &s, double t) {
    View v;
    const auto &K = s.keys;
    if (K.empty()) return v;
    size_t n = K.size();
    // (yaw unwrapped: each key within half a turn of the one before)
    std::vector<double> yaw(n);
    yaw[0] = K[0].yaw;
    for (size_t i = 1; i < n; i++) {
        double y = K[i].yaw;
        while (y - yaw[i - 1] > 180) y -= 360;
        while (y - yaw[i - 1] < -180) y += 360;
        yaw[i] = y;
    }
    auto value = [&](size_t i, int c) -> double {
        return c < 3 ? K[i].p[(size_t)c] : c == 3 ? yaw[i] : c == 4 ? K[i].pitch : K[i].fov;
    };
    if (n == 1 || t <= K[0].t) {
        for (int c = 0; c < 3; c++) v.eye[(size_t)c] = value(0, c);
        v.yaw = yaw[0], v.pitch = K[0].pitch, v.fov = K[0].fov;
    } else if (t >= K[n - 1].t) {
        for (int c = 0; c < 3; c++) v.eye[(size_t)c] = value(n - 1, c);
        v.yaw = yaw[n - 1], v.pitch = K[n - 1].pitch, v.fov = K[n - 1].fov;
    } else {
        size_t i = 0;
        while (i + 2 < n && t > K[i + 1].t) i++;
        double t0 = K[i].t, t1 = K[i + 1].t, d = std::max(1e-6, t1 - t0), u = std::clamp((t - t0) / d, 0.0, 1.0);
        auto tangent = [&](size_t j, int c) {
            if (j == 0) return (value(1, c) - value(0, c)) / std::max(1e-6, K[1].t - K[0].t);
            if (j == n - 1) return (value(n - 1, c) - value(n - 2, c)) / std::max(1e-6, K[n - 1].t - K[n - 2].t);
            return (value(j + 1, c) - value(j - 1, c)) / std::max(1e-6, K[j + 1].t - K[j - 1].t);
        };
        double h00 = 2 * u * u * u - 3 * u * u + 1, h10 = u * u * u - 2 * u * u + u, h01 = -2 * u * u * u + 3 * u * u,
               h11 = u * u * u - u * u;
        double r[6];
        for (int c = 0; c < 6; c++)
            r[c] = h00 * value(i, c) + h10 * d * tangent(i, c) + h01 * value(i + 1, c) + h11 * d * tangent(i + 1, c);
        v.eye = {r[0], r[1], r[2]};
        v.yaw = r[3], v.pitch = r[4], v.fov = r[5];
    }
    if (s.look) {
        V3 d = sub(s.at, v.eye);
        if (len(d) > 1e-6) {
            v.yaw = deg(std::atan2(d[1], d[0]));
            v.pitch = deg(std::atan2(d[2], len2d(d)));
        }
    }
    v.yaw = std::fmod(v.yaw, 360.0);
    if (v.yaw < 0) v.yaw += 360;
    return v;
}

// ---- pace: a shot's keys timed along its path
const char *pace_name(Pace p) {
    switch (p) {
    case Pace::even: return "even";
    case Pace::in: return "in";
    case Pace::out: return "out";
    case Pace::both: return "both";
    default: return "keyed";
    }
}

bool pace_from(const std::string &name, Pace &p) {
    for (Pace q : {Pace::keyed, Pace::even, Pace::in, Pace::out, Pace::both})
        if (name == pace_name(q)) { p = q; return true; }
    return false;
}

namespace {
// How far apart two views are along a path: the distance the camera moves; where it only turns (or zooms) standing
// still, a unit for each 20 degrees, so turning on the spot still takes its time
double step_len(const Key &a, const Key &b, bool look) {
    double d = len(sub(b.p, a.p));
    if (look || d > 0.05) return std::max(d, 1e-3);
    double turn = std::hypot(b.yaw - a.yaw, b.pitch - a.pitch) / 20, zoom = std::fabs(b.fov - a.fov) / 20;
    return std::max({d, turn, zoom, 1e-3});
}

// The share of the way gone at `x` of the time, at an even speed between a speed-up over the first `a` of the time and
// a slow-down over the last `b` (each from or to still, along half a cosine)
double paced(double x, double a, double b) {
    x = std::clamp(x, 0.0, 1.0);
    double v = 1 / (1 - (a + b) / 2);
    auto ramp = [](double t, double r) { return r > 0 ? (t - r / kPi * std::sin(kPi * t / r)) / 2 : 0.0; };
    if (x < a) return v * ramp(x, a);
    if (x <= 1 - b) return v * (a / 2 + (x - a));
    return 1 - v * ramp(1 - x, b);
}
}   // namespace

void retime(Shot &s) {
    if (s.pace == Pace::keyed || s.via.empty()) return;
    const size_t n = s.via.size();
    const double T = std::max(0.5, s.seconds);
    if (n == 1) {
        s.via[0].t = 0;
        s.keys = {s.via[0]};
        return;
    }
    std::vector<Key> P = s.via;   // (yaw unwrapped: each within half a turn of the one before)
    for (size_t i = 1; i < n; i++) {
        while (P[i].yaw - P[i - 1].yaw > 180) P[i].yaw -= 360;
        while (P[i].yaw - P[i - 1].yaw < -180) P[i].yaw += 360;
    }
    // The curve through them: Hermite between each two, Catmull-Rom tangents over the distances between them (as the
    // playback rule does over times: knots this far apart give the even pace a smooth curve)
    std::vector<double> kn(n, 0);
    for (size_t i = 1; i < n; i++) kn[i] = kn[i - 1] + step_len(P[i - 1], P[i], s.look);
    auto val = [&](size_t i, int c) -> double {
        return c < 3 ? P[i].p[(size_t)c] : c == 3 ? P[i].yaw : c == 4 ? P[i].pitch : P[i].fov;
    };
    auto tangent = [&](size_t j, int c) {
        if (j == 0) return (val(1, c) - val(0, c)) / (kn[1] - kn[0]);
        if (j == n - 1) return (val(n - 1, c) - val(n - 2, c)) / (kn[n - 1] - kn[n - 2]);
        return (val(j + 1, c) - val(j - 1, c)) / (kn[j + 1] - kn[j - 1]);
    };
    auto eval = [&](double sigma) {   // (sigma: key i at i)
        sigma = std::clamp(sigma, 0.0, (double)(n - 1));
        size_t i = std::min((size_t)sigma, n - 2);
        double u = sigma - (double)i, d = kn[i + 1] - kn[i];
        double h00 = 2 * u * u * u - 3 * u * u + 1, h10 = u * u * u - 2 * u * u + u, h01 = -2 * u * u * u + 3 * u * u,
               h11 = u * u * u - u * u;
        double r[6];
        for (int c = 0; c < 6; c++) r[c] = h00 * val(i, c) + h10 * d * tangent(i, c) + h01 * val(i + 1, c) + h11 * d * tangent(i + 1, c);
        Key k;
        k.p = {r[0], r[1], r[2]};
        k.yaw = r[3], k.pitch = r[4], k.fov = std::clamp(r[5], 10.0, 150.0);
        return k;
    };
    // How far along it each point is
    const int per = 40;
    std::vector<double> sig, dist;
    Key prev = eval(0);
    double S = 0;
    for (size_t i = 0; i + 1 < n; i++)
        for (int k = i ? 1 : 0; k <= per; k++) {
            double sg = (double)i + (double)k / per;
            Key q = eval(sg);
            if (!sig.empty()) S += step_len(prev, q, s.look);
            sig.push_back(sg);
            dist.push_back(S);
            prev = q;
        }
    auto sigma_at = [&](double want) {
        size_t j = (size_t)(std::lower_bound(dist.begin(), dist.end(), want) - dist.begin());
        if (j == 0) return sig.front();
        if (j >= dist.size()) return sig.back();
        double f = (want - dist[j - 1]) / std::max(1e-12, dist[j] - dist[j - 1]);
        return sig[j - 1] + (sig[j] - sig[j - 1]) * f;
    };
    // The slow start and end: up to 2 seconds each, at most 45% of the shot alone, 35% each with both
    bool in = s.pace == Pace::in || s.pace == Pace::both, out = s.pace == Pace::out || s.pace == Pace::both;
    double share = in && out ? 0.35 : 0.45;
    double a = in ? std::min(2.0, share * T) / T : 0, b = out ? std::min(2.0, share * T) / T : 0;
    // When it gets to each via key
    for (size_t i = 0; i < n; i++) {
        double want = S > 0 ? dist[i * per] / S : 0, lo = 0, hi = 1;
        for (int k = 0; k < 50; k++) {
            double mid = (lo + hi) / 2;
            (paced(mid, a, b) < want ? lo : hi) = mid;
        }
        s.via[i].t = T * (lo + hi) / 2;
    }
    // Keys: a key every half second, or closer at speed (about a unit apart), and one at each via key (where it turns
    // quickly, round a corner). All on the one curve at its pace, so the playback's curve through them keeps to it.
    double top = S / T / (1 - (a + b) / 2);
    double dt = std::clamp(1.0 / std::max(top, 1e-6), 0.2, 0.5);
    int N = std::max(2, (int)std::ceil(T / dt - 1e-9));
    std::vector<double> times;
    for (int j = 0; j <= N; j++) times.push_back(T * j / N);
    for (auto &v : s.via) times.push_back(v.t);
    std::sort(times.begin(), times.end());
    std::vector<double> kept;
    for (double t : times)
        if (kept.empty() || t - kept.back() >= 0.06 || t >= T - 1e-9) {
            if (!kept.empty() && t >= T - 1e-9 && T - kept.back() < 0.06 && kept.size() > 1) kept.back() = T;
            else kept.push_back(std::min(t, T));
        }
    kept.erase(std::unique(kept.begin(), kept.end()), kept.end());
    s.keys.clear();
    for (double t : kept) {
        Key k = eval(sigma_at(paced(t / T, a, b) * S));
        k.t = t;
        k.yaw = std::fmod(k.yaw, 360.0);
        if (k.yaw < 0) k.yaw += 360;
        s.keys.push_back(k);
    }
}

std::vector<V3> path(const Shot &s, double step) {
    std::vector<V3> out;
    for (double t = 0; t <= s.seconds + 1e-9; t += step) out.push_back(at(s, t).eye);
    return out;
}

// ---- making them
Showcase generate(const lshade::Preview &p, std::string *report) {
    Showcase sc;
    if (p.triangles.empty() || p.views.empty()) {
        if (report) *report = "nothing to go on (no level, or nowhere players stand)";
        return sc;
    }
    World w(p);
    // Where players are: the scenario's spawns, item spawn points, flags, game type places and vehicles, at eye height.
    std::vector<V3> play;
    for (auto &m : p.marks) {
        V3 q{m.p[0], m.p[1], m.p[2] + kEye};
        if (w.open(q, 0.3)) play.push_back(q);
    }
    // The measured views (from floors in the box around those), a spread-out few hundred.
    std::vector<V3> sampled;
    std::vector<double> sampled_yaw;
    size_t every = std::max<size_t>(1, p.views.size() / 240);
    for (size_t i = 0; i < p.views.size(); i += every) {
        sampled.push_back({p.views[i].eye[0], p.views[i].eye[1], p.views[i].eye[2]});
        sampled_yaw.push_back(deg(p.views[i].yaw));
    }
    V3 middle{0, 0, 0}, slo{1e30, 1e30, 1e30}, shi{-1e30, -1e30, -1e30};
    const std::vector<V3> &extent = play.size() >= 2 ? play : sampled;
    for (auto &q : extent) {
        middle = add(middle, q);
        for (int k = 0; k < 3; k++) { slo[(size_t)k] = std::min(slo[(size_t)k], q[(size_t)k]); shi[(size_t)k] = std::max(shi[(size_t)k], q[(size_t)k]); }
    }
    middle = mul(middle, 1.0 / extent.size());
    // (how far apart shots go, and how far a glide goes: by the size of where players are, so a small map gets as
    // many shots as a big one)
    const double across = len2d(sub(shi, slo)), apart = std::clamp(across * 0.2, 4.0, 12.0);
    const double glide_lo = std::clamp(across * 0.2, 6.0, 14.0), glide_hi = std::clamp(across * 0.9, glide_lo + 4, 32.0);
    // Where players stand: those views near a place players are (within `apart` across, and at most half a unit above it,
    // so not on the roofs and high ledges the views' floors take in that no one gets to), and those places
    // themselves. All the views when the scenario has too few places.
    std::vector<V3> spots;
    std::vector<double> spot_yaw;
    const double reach_across = std::max(6.0, apart);
    for (size_t i = 0; i < sampled.size(); i++) {
        bool near_play = false;
        for (auto &q : play) {
            double up = sampled[i][2] - q[2];
            if (up <= 0.5 && up >= -2.5 && len2d(sub(sampled[i], q)) <= reach_across) { near_play = true; break; }
        }
        if (near_play) { spots.push_back(sampled[i]); spot_yaw.push_back(sampled_yaw[i]); }
    }
    for (auto &q : play) {
        spots.push_back(q);
        spot_yaw.push_back(len2d(sub(middle, q)) > 1 ? yaw_to(q, middle) : 0);
    }
    if (spots.size() < 6) { spots = sampled; spot_yaw = sampled_yaw; }
    // Where the camera goes for the shots among the players (circling, gliding, sight lines): in the box around where
    // players are (a few units past it across, from just under their feet to a few units over their heads). Only the
    // shots looking down on them go outside it.
    const V3 plo = slo, phi = shi;
    auto in_play = [&](const V3 &q) {
        if (play.size() < 2) return true;
        return q[0] >= plo[0] - 3 && q[0] <= phi[0] + 3 && q[1] >= plo[1] - 3 && q[1] <= phi[1] + 3 &&
               q[2] >= plo[2] - kEye - 0.5 && q[2] <= phi[2] + 3.5;
    };
    std::vector<V3> used;   // where shots already are (the next ones go elsewhere)
    auto far_from_used = [&](V3 q, double d) {
        for (auto &u : used)
            if (len(sub(u, q)) < d) return false;
        return true;
    };
    std::vector<Shot> orbits, lines, glides, reveals, overviews;

    // Circling a point: a 100-degree arc round it in 12 seconds, a few units off and above, looking at it: of the
    // clear ones, the one that sees it most (nearest radius and height first: close is more interesting).
    auto orbit = [&](V3 c, const std::string &label, bool clockwise) -> bool {
        V3 at = add(c, {0, 0, 0.4});
        Shot best;
        double best_seen = -1;
        for (double r : {4.5, 3.5, 6.0, 7.5, 3.0, 2.5})
            for (double h : {1.2, 2.0, 0.6, 3.0})
                for (int a0 = 0; a0 < 360 && best_seen < 0.999; a0 += 20) {
                    Shot s = orbit_shot(at, r, h, a0, clockwise ? -100 : 100, 12, "orbit: " + label);
                    double seen = flyable(w, s, 0.85, in_play);
                    if (seen > best_seen) { best_seen = seen; best = s; }
                }
        if (best_seen < 0) return false;
        orbits.push_back(best);
        used.push_back(c);
        return true;
    };
    int flags = 0, powers = 0;
    for (auto &m : p.marks) {
        V3 c{m.p[0], m.p[1], m.p[2]};
        if (!far_from_used(c, apart * 0.6)) continue;
        if (m.kind == lshade::Mark::kFlag && flags < 2 && orbit(c, m.name, flags % 2 == 1)) flags++;
    }
    for (auto &m : p.marks) {
        V3 c{m.p[0], m.p[1], m.p[2]};
        if (!far_from_used(c, apart * 0.6)) continue;
        if (m.kind == lshade::Mark::kPower && powers < 2 && orbit(c, m.name, powers % 2 == 0)) powers++;
    }
    if (orbits.empty()) {   // (nothing to circle: the middle of where players stand)
        size_t best = 0;
        for (size_t i = 0; i < spots.size(); i++)
            if (len(sub(spots[i], middle)) < len(sub(spots[best], middle))) best = i;
        orbit(add(spots[best], {0, 0, -0.62}), "the middle", false);
    }

    // Down the longest sight lines players have: from a place players are, looking at another far off that it sees,
    // the camera drifting towards it.
    struct Line { double d; V3 a, b; };
    std::vector<Line> found;
    const double line_lo = std::max(10.0, across * 0.25);
    {
        std::vector<V3> ends;
        for (auto &q : play) {
            V3 e = add(q, {0, 0, 0.3});
            if (w.open(e)) ends.push_back(e);
        }
        if (ends.size() < 2)
            for (auto &q : spots) ends.push_back(add(q, {0, 0, 0.3}));
        size_t checks = 0;
        std::vector<Line> far;
        for (size_t i = 0; i < ends.size(); i++)
            for (size_t j = i + 1; j < ends.size(); j++) {
                double d = len(sub(ends[j], ends[i]));
                if (d >= line_lo) far.push_back({d, ends[i], ends[j]});
            }
        std::sort(far.begin(), far.end(), [](const Line &x, const Line &y) { return x.d > y.d; });
        for (auto &l : far) {
            if (found.size() >= 40 || checks++ > 20000) break;
            if (w.sees(l.a, l.b)) found.push_back(l);
        }
    }
    for (auto &l : found) {
        if (lines.size() >= 2) break;
        for (int side = 0; side < 2 && lines.size() < 2; side++) {   // (from either end)
            V3 a = side ? l.b : l.a, b = side ? l.a : l.b;
            if (!far_from_used(a, apart)) continue;
            V3 d = sub(b, a);
            double go = std::min(7.0, l.d * 0.15), drift = lines.size() % 2 ? -4 : 4, heading = yaw_to(a, b);
            V3 fwd = mul(d, 1 / l.d);
            Shot s = keyed("sight line", 10, 2, [&](double u) {
                Key k;
                k.p = add(a, mul(fwd, go * ease(u) - 0.5));
                k.yaw = heading - drift / 2 + drift * ease(u);
                k.pitch = deg(std::atan2(b[2] - k.p[2], len2d(sub(b, k.p))));
                return k;
            });
            if (flyable(w, s, 0.9, in_play, 0.35) < 0) continue;
            lines.push_back(s);
            used.push_back(a);
            break;
        }
    }

    // Gliding between two places players stand that see each other (14 to 32 units apart on a big map, less on a
    // small one), a little above head height, looking ahead.
    struct Pair { double d; size_t a, b; };
    std::vector<Pair> pairs;
    std::vector<char> clear_above(spots.size());   // (room for the camera half a unit over their eyes)
    for (size_t i = 0; i < spots.size(); i++) clear_above[i] = w.open(add(spots[i], {0, 0, 0.5}), 0.35);
    for (size_t i = 0; i < spots.size(); i++)
        for (size_t j = i + 1; j < spots.size(); j++) {
            if (!clear_above[i] || !clear_above[j]) continue;
            V3 d = sub(spots[j], spots[i]);
            double h = len2d(d);
            if (h < glide_lo || h > glide_hi || std::fabs(d[2]) > 3) continue;
            pairs.push_back({h, i, j});
        }
    std::sort(pairs.begin(), pairs.end(), [](const Pair &x, const Pair &y) { return x.d > y.d; });
    size_t tried = 0;
    for (auto &pr : pairs) {
        if (glides.size() >= 2 || tried > 4000) break;
        V3 a = add(spots[pr.a], {0, 0, 0.5}), b = add(spots[pr.b], {0, 0, 0.5});
        V3 mid = lerp(a, b, 0.5);
        if (!far_from_used(mid, apart)) continue;
        tried++;
        if (!w.sees(a, b, true)) continue;
        double seconds = std::clamp(pr.d / 2.2, 8.0, 14.0), heading = yaw_to(a, b), sway = glides.size() % 2 ? -10 : 10;
        Shot s = keyed("glide", seconds, 2, [&](double u) {
            Key k;
            k.p = lerp(a, b, ease(u));
            k.yaw = heading + sway * std::sin(kPi * u);
            k.pitch = -3;
            return k;
        });
        if (flyable(w, s, 0.9, in_play, 0.35) < 0 || !sees_out(w, s)) continue;
        glides.push_back(s);
        used.push_back(mid);
    }

    // Looking down on where players play: from above and off to the side of a flag, a power item or the middle of
    // where players are, the camera moving round it a little, looking at it (from outside where players can go, too).
    {
        std::vector<std::pair<V3, std::string>> targets;
        for (auto &m : p.marks)
            if (m.kind == lshade::Mark::kFlag || m.kind == lshade::Mark::kPower) targets.push_back({{m.p[0], m.p[1], m.p[2] + 0.4}, m.name});
        if (!play.empty()) {
            size_t best = 0;
            for (size_t i = 0; i < play.size(); i++)
                if (len(sub(play[i], middle)) < len(sub(play[best], middle))) best = i;
            targets.push_back({add(play[best], {0, 0, -0.2}), "the middle"});
        }
        std::vector<V3> done;
        for (auto &tg : targets) {
            if (overviews.size() >= 2) break;
            const V3 c = tg.first;
            bool spaced = true;
            for (auto &q : done)
                if (len(sub(q, c)) < apart * 1.5) spaced = false;
            if (!spaced) continue;
            bool made = false;
            for (double h : {7.0, 10.0, 5.0})
                for (double r : {10.0, 14.0, 7.0, 18.0})
                    for (int a0 = 0; a0 < 360 && !made; a0 += 30) {
                        Shot s = orbit_shot(c, r, h, a0, overviews.size() % 2 ? -40 : 40, 10, "looking down on " + tg.second);
                        s.orbit.on = false;   // (keys of its own: not an orbit to change)
                        if (flyable(w, s, 0.85) < 0) continue;
                        overviews.push_back(s);
                        done.push_back(c);
                        made = true;
                    }
        }
    }

    // Rising over open ground: from where players stand with open sky above, nearest the middle first, the camera
    // climbs 6 units, turning a little and tilting down to take in the level.
    {
        std::vector<std::pair<double, size_t>> outdoor;
        for (size_t i = 0; i < spots.size(); i++)
            if (w.reach(add(spots[i], {0, 0, 0.3}), {0, 0, 1}, 8) >= 7.5) outdoor.push_back({len(sub(spots[i], middle)), i});
        std::sort(outdoor.begin(), outdoor.end());
        for (size_t k = 0; k < outdoor.size() && reveals.empty(); k++) {
            V3 base = add(spots[outdoor[k].second], {0, 0, 0.3});
            if (!far_from_used(base, apart * 0.8)) continue;
            double y0 = len2d(sub(middle, base)) > 4 ? yaw_to(base, middle) : spot_yaw[outdoor[k].second];
            Shot s = keyed("rising over the level", 12, 2, [&](double u) {
                Key q;
                q.p = add(base, {0, 0, 6 * ease(u)});
                q.yaw = y0 + 35 * ease(u);
                q.pitch = -18 * ease(u);
                return q;
            });
            if (flyable(w, s) < 0 || !sees_out(w, s, 8)) continue;
            reveals.push_back(s);
            used.push_back(base);
        }
    }


    // Along the routes a game type gives players: from one CTF flag to the other, one hill to the next, one oddball
    // spawn to another. Through open air, round the walls in the way, a couple of
    // units over the ground, looking ahead.
    std::vector<Shot> runs;
    std::vector<std::string> run_kinds;
    {
        Router router(w, {plo[0] - 3, plo[1] - 3, plo[2] - kEye - 0.5}, {phi[0] + 3, phi[1] + 3, phi[2] + 3.5}, in_play, across);
        auto run = [&](const std::vector<V3> &grounds, double speed, double cap, bool partial, const std::string &label) -> bool {
            std::vector<V3> ends;
            for (auto &gq : grounds) {
                bool found = false;
                for (auto &q : camera_spots(w, gq, in_play))   // (one the routes reach)
                    if (!router.joins(q).empty()) { ends.push_back(q); found = true; break; }
                if (!found) {
                    if (partial && ends.size() >= 2) break;
                    return false;
                }
            }
            std::vector<V3> corners;
            std::vector<std::pair<double, size_t>> legs;   // (each leg's end: the length so far, and which place)
            for (size_t i = 0; i + 1 < ends.size(); i++) {
                std::vector<V3> leg = router.route(ends[i], ends[i + 1]);
                if (leg.empty()) {
                    if (partial && corners.size() >= 2) break;
                    return false;
                }
                if (!corners.empty()) leg.erase(leg.begin());
                corners.insert(corners.end(), leg.begin(), leg.end());
                legs.push_back({route_length(corners), i + 1});
                if (route_length(corners) / speed >= cap) break;
            }
            if (corners.size() < 2 || route_length(corners) < 6) return false;
            if (route_length(corners) > cap * speed) {   // (a long one: the first part of it)
                std::vector<V3> cut{corners[0]};
                double want = cap * speed, sofar = 0;
                for (size_t i = 1; i < corners.size() && sofar < want - 1e-9; i++) {
                    double l = len(sub(corners[i], corners[i - 1]));
                    cut.push_back(sofar + l <= want ? corners[i] : lerp(corners[i - 1], corners[i], (want - sofar) / l));
                    sofar += l;
                }
                corners = cut;
            }
            // (looking, at the end, at the place the leg it ends on goes to)
            size_t goal = legs.back().second;
            for (auto &[l, g] : legs)
                if (l >= route_length(corners) - 1e-6) { goal = g; break; }
            Shot s = route_shot(w, corners, speed, 1.2, add(grounds[goal], {0, 0, 0.5}), label);
            if (flyable(w, s, 0.9, in_play, 0.3) < 0) return false;
            // (looking out ahead, not at a wall it's passing: at least 3 units clear in front nearly all the way, and
            // never a wall filling the picture)
            int n = 0, clear_ahead = 0;
            for (double t = 0; t <= s.seconds + 1e-9; t += 0.5, n++) {
                View v = at(s, t);
                double r = w.reach(v.eye, dir_of(v.yaw, v.pitch), 6);
                if (r < 1.2) { n = -1; break; }
                if (r >= 3) clear_ahead++;
            }
            if (n < 0 || clear_ahead < n * 0.85) return false;
            runs.push_back(s);
            return true;
        };
        auto ground = [](const lshade::Mark &m) { return V3{m.p[0], m.p[1], m.p[2]}; };
        const lshade::Mark *red = nullptr, *blue = nullptr;
        for (auto &m : p.marks)
            if (m.kind == lshade::Mark::kFlag) {
                if (m.index == 1) { if (!blue) blue = &m; }
                else if (!red) red = &m;
            }
        if (red && blue && run({ground(*red), ground(*blue)}, 7, 30, false, "red flag to blue flag")) run_kinds.push_back("flag to flag");
        // (a hill: the middle of the flags with its number; oddball spawns by their numbers)
        std::map<int, std::pair<V3, int>> hills;
        std::vector<std::pair<int, V3>> balls;
        for (auto &m : p.marks) {
            if (m.kind != lshade::Mark::kPlace) continue;
            std::string n = m.name;
            if (n == "hill") {
                auto &hl = hills[m.index];
                hl.first = add(hl.first, ground(m));
                hl.second++;
            } else if (n == "oddball") {
                balls.push_back({m.index, ground(m)});
            }
        }
        // (a hill is three flags or more round it; one or two with a number are something else)
        std::vector<V3> hill_at;
        for (auto &[k, hl] : hills)
            if (hl.second >= 3) hill_at.push_back(mul(hl.first, 1.0 / hl.second));
        // Two of them: the farthest apart first (at least 8 units), the next pair when the camera can't get between them.
        auto pair_run = [&](const std::vector<V3> &at, const std::string &label) {
            std::vector<std::tuple<double, size_t, size_t>> pairs;
            for (size_t i = 0; i < at.size(); i++)
                for (size_t j = i + 1; j < at.size(); j++) {
                    double d = len(sub(at[j], at[i]));
                    if (d >= 8) pairs.push_back({d, i, j});
                }
            std::sort(pairs.begin(), pairs.end(), [](const auto &a, const auto &b) { return std::get<0>(a) > std::get<0>(b); });
            for (size_t k = 0; k < pairs.size() && k < 6; k++)
                if (run({at[std::get<1>(pairs[k])], at[std::get<2>(pairs[k])]}, 7, 30, false, label)) return true;
            return false;
        };
        if (pair_run(hill_at, "hill to hill")) run_kinds.push_back("hill to hill");
        {
            std::vector<V3> ball_at;
            for (auto &b : balls) ball_at.push_back(b.second);
            if (pair_run(ball_at, "oddball to oddball")) run_kinds.push_back("oddball");
        }
        // (No race shot: round every race point in a lap is too much turning for a flythrough; it was jarring on
        // every map tried.)
    }

    // The vehicles a game type spawns, where several stand together: along them, when they stand in a line (the
    // camera beside the line, moving along it, looking at them), else circling them.
    std::vector<Shot> rides;
    {
        std::vector<const lshade::Mark *> veh;
        for (auto &m : p.marks)
            if (m.kind == lshade::Mark::kVehicle) veh.push_back(&m);
        std::vector<int> group(veh.size());
        for (size_t i = 0; i < veh.size(); i++) group[i] = (int)i;
        std::function<int(int)> top = [&](int i) { return group[(size_t)i] == i ? i : group[(size_t)i] = top(group[(size_t)i]); };
        for (size_t i = 0; i < veh.size(); i++)
            for (size_t j = i + 1; j < veh.size(); j++) {
                V3 a{veh[i]->p[0], veh[i]->p[1], veh[i]->p[2]}, b{veh[j]->p[0], veh[j]->p[1], veh[j]->p[2]};
                if (len(sub(a, b)) <= 7 && std::fabs(a[2] - b[2]) <= 3) group[(size_t)top((int)i)] = top((int)j);
            }
        std::map<int, std::vector<size_t>> clusters;
        for (size_t i = 0; i < veh.size(); i++) clusters[top((int)i)].push_back(i);
        std::vector<std::vector<size_t>> order;
        for (auto &[k, v] : clusters)
            if (v.size() >= 2) order.push_back(v);
        std::stable_sort(order.begin(), order.end(), [](const std::vector<size_t> &a, const std::vector<size_t> &b) { return a.size() > b.size(); });
        for (auto &cl : order) {
            if (rides.size() >= 2) break;
            V3 c{0, 0, 0};
            std::map<std::string, int> names;
            for (size_t i : cl) {
                c = add(c, {veh[i]->p[0], veh[i]->p[1], veh[i]->p[2]});
                names[veh[i]->name]++;
            }
            c = mul(c, 1.0 / cl.size());
            if (!far_from_used(c, apart * 0.5)) continue;
            std::string what;
            int most = 0;
            for (auto &[n, k] : names)
                if (k > most) { most = k; what = n; }
            std::string label = "vehicles: " + what + (cl.size() > 1 ? " x" + std::to_string(cl.size()) : "");
            // (the line through them: their spread's long axis)
            double sxx = 0, syy = 0, sxy = 0, spread = 0;
            for (size_t i : cl) {
                double dx = veh[i]->p[0] - c[0], dy = veh[i]->p[1] - c[1];
                sxx += dx * dx; syy += dy * dy; sxy += dx * dy;
                spread = std::max(spread, std::hypot(dx, dy));
            }
            double ang = 0.5 * std::atan2(2 * sxy, sxx - syy);
            V3 ax{std::cos(ang), std::sin(ang), 0}, pr{-std::sin(ang), std::cos(ang), 0};
            double alo = 1e30, ahi = -1e30, plo = 1e30, phi = -1e30;
            for (size_t i : cl) {
                V3 d = sub({veh[i]->p[0], veh[i]->p[1], veh[i]->p[2]}, c);
                alo = std::min(alo, dot(d, ax)); ahi = std::max(ahi, dot(d, ax));
                plo = std::min(plo, dot(d, pr)); phi = std::max(phi, dot(d, pr));
            }
            bool made = false;
            if (ahi - alo >= 6 && ahi - alo >= 2.5 * (phi - plo)) {   // (in a line: along it)
                for (double d : {6.0, 8.0, 4.5, 10.0})
                    for (double hgt : {1.8, 2.8})
                        for (int side : {1, -1})
                            for (int way : {1, -1}) {
                                if (made) break;
                                V3 base = add(c, mul(pr, side * (d + (side > 0 ? phi : -plo))));
                                double from = way > 0 ? alo - 2 : ahi + 2, to = way > 0 ? ahi + 2 : alo - 2;
                                double seconds = std::clamp(std::fabs(to - from) / 2.5, 8.0, 14.0), yaw = yaw_to(base, c);
                                Shot s = keyed(label, seconds, 2, [&](double u) {
                                    Key k;
                                    k.p = add(add(base, mul(ax, from + (to - from) * ease(u))), {0, 0, hgt});
                                    k.yaw = yaw;
                                    k.pitch = deg(std::atan2(c[2] + 0.6 - k.p[2], d + 0.01));
                                    return k;
                                });
                                if (flyable(w, s, 0.9, in_play) < 0) continue;
                                bool seen = true;   // (what's straight ahead, on the line, in view at its start, middle and end)
                                for (double u : {0.0, 0.5, 1.0}) {
                                    V3 eye = at(s, s.seconds * u).eye, foot = add(add(c, mul(ax, dot(sub(eye, c), ax))), {0, 0, 0.6});
                                    if (!w.sees(eye, short_of(eye, foot), true)) seen = false;
                                }
                                if (!seen) continue;
                                rides.push_back(s);
                                made = true;
                            }
            }
            if (!made) {   // (circling them)
                Shot best;
                double best_seen = -1;
                for (double r : {spread + 4.0, spread + 5.5, spread + 3.0, spread + 7.0})
                    for (double hgt : {1.8, 3.0, 1.0})
                        for (int a0 = 0; a0 < 360 && best_seen < 0.999; a0 += 20) {
                            Shot s = orbit_shot(add(c, {0, 0, 0.6}), r, hgt, a0, rides.size() % 2 ? -100 : 100, 12, label);
                            double seen = flyable(w, s, 0.85, in_play);
                            if (seen > best_seen) { best_seen = seen; best = s; }
                        }
                if (best_seen >= 0) { rides.push_back(best); made = true; }
            }
            if (made) used.push_back(c);
        }
    }

    // Flying over the level, looking straight down, the camera moving sideways across where players play (along its
    // long side): as high as the level's air goes (up to 36 units over them), and only where the whole picture is the
    // level (no sky, nothing past its edge).
    std::vector<Shot> flyovers;
    {
        double sxx = 0, syy = 0, sxy = 0;
        for (auto &q : spots) {
            double dx = q[0] - middle[0], dy = q[1] - middle[1];
            sxx += dx * dx; syy += dy * dy; sxy += dx * dy;
        }
        double ang = 0.5 * std::atan2(2 * sxy, sxx - syy);
        V3 ax{std::cos(ang), std::sin(ang), 0}, pr{-std::sin(ang), std::cos(ang), 0};
        const double run_len = std::clamp(across * 0.45, 10.0, 40.0);
        // (first where the ground it looks down on is about as far below as it's high, not the bottom of a pit; then
        // anywhere)
        for (int pass = 0; pass < 2 && flyovers.empty(); pass++)
            for (double H : {36.0, 28.0, 22.0, 17.0, 13.0, 10.0})
                for (double off : {0.0, -0.18, 0.18, -0.32, 0.32})
                    for (int way : {1, -1}) {
                        if (!flyovers.empty()) break;
                        V3 c = add(middle, add(mul(pr, off * across), {0, 0, H}));
                        double yaw = deg(ang) + (way > 0 ? 90 : -90);   // (moving to the picture's right)
                        Shot s = keyed("flying over the level", 12, 2, [&](double u) {
                            Key k;
                            k.p = add(c, mul(ax, way * (ease(u) - 0.5) * run_len));
                            k.yaw = yaw;
                            k.pitch = -88;   // (straight down: the engine's camera needs a direction it can turn its up from)
                            return k;
                        });
                        if (flyable(w, s, 0.9, nullptr, 1.0) < 0) continue;
                        bool all = true;   // (out in the open, nothing over it: above the level, not inside a hall)
                        double below = 0;
                        int n = 0;
                        for (double t = 0; t <= s.seconds + 1e-9 && all; t += 1.0, n++) {
                            View v = at(s, t);
                            all = w.reach(v.eye, {0, 0, 1}, 60) >= 60 - 1e-6 && all_level_below(w, v, H * 3 + 60);
                            below += w.reach(v.eye, {0, 0, -1}, H * 3 + 60);
                        }
                        if (all && (pass == 1 || below / std::max(1, n) <= H * 1.6)) flyovers.push_back(s);
                    }
    }

    // In turn: the reveal, then circling, a route, looking down, vehicles, gliding, flying over and sight lines, one
    // of each at a time.
    for (auto &s : reveals) sc.shots.push_back(s);
    for (size_t k = 0; k < 4; k++) {
        if (k < orbits.size()) sc.shots.push_back(orbits[k]);
        if (k < runs.size()) sc.shots.push_back(runs[k]);
        if (k < overviews.size()) sc.shots.push_back(overviews[k]);
        if (k < rides.size()) sc.shots.push_back(rides[k]);
        if (k < glides.size()) sc.shots.push_back(glides[k]);
        if (k < flyovers.size()) sc.shots.push_back(flyovers[k]);
        if (k < lines.size()) sc.shots.push_back(lines[k]);
    }
    if (report) {
        std::string kinds;
        for (auto &k : run_kinds) kinds += (kinds.empty() ? "" : ", ") + k;
        char b[400];
        snprintf(b, sizeof b, "%zu shots, %.0f seconds: %zu circling (%d flags, %d power items), %zu along routes%s%s%s, %zu at "
                 "vehicles, %zu looking down, %zu gliding, %zu flying over, %zu down sight lines, %zu rising",
                 sc.shots.size(), sc.length(), orbits.size(), flags, powers, runs.size(), kinds.empty() ? "" : " (",
                 kinds.c_str(), kinds.empty() ? "" : ")", rides.size(), overviews.size(), glides.size(), flyovers.size(),
                 lines.size(), reveals.size());
        *report = sc.shots.empty() ? "no shot kept the camera clear of the level's walls" : b;
    }
    return sc;
}

}   // namespace showcase
