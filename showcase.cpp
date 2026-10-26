// Halo Map Porter - showcase flythroughs (showcase.hpp).
#include "showcase.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <set>
#include <sstream>

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

    // In turn: the reveal, then circling, looking down, gliding and sight lines, one of each at a time.
    for (auto &s : reveals) sc.shots.push_back(s);
    for (size_t k = 0; k < 4; k++) {
        if (k < orbits.size()) sc.shots.push_back(orbits[k]);
        if (k < overviews.size()) sc.shots.push_back(overviews[k]);
        if (k < glides.size()) sc.shots.push_back(glides[k]);
        if (k < lines.size()) sc.shots.push_back(lines[k]);
    }
    if (report) {
        char b[240];
        snprintf(b, sizeof b, "%zu shots, %.0f seconds: %zu circling (%d flags, %d power items), %zu looking down, %zu gliding, "
                 "%zu down sight lines, %zu rising",
                 sc.shots.size(), sc.length(), orbits.size(), flags, powers, overviews.size(), glides.size(), lines.size(),
                 reveals.size());
        *report = sc.shots.empty() ? "no shot kept the camera clear of the level's walls" : b;
    }
    return sc;
}

}   // namespace showcase
