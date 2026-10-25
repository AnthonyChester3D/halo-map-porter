// Halo Map Porter - animations stored as keyframes, the Xbox's compressed format (see animcomp.hpp).
#include "animcomp.hpp"
#include "tagfile.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <set>

namespace animcomp {

namespace {

const double kPi = 3.14159265358979323846;

int16_t be16s(const std::string &b, size_t o) { return (int16_t)((uint8_t)b[o] << 8 | (uint8_t)b[o + 1]); }
double be_float(const std::string &b, size_t o) {
    uint32_t u = (uint32_t)(uint8_t)b[o] << 24 | (uint32_t)(uint8_t)b[o + 1] << 16 | (uint32_t)(uint8_t)b[o + 2] << 8 | (uint8_t)b[o + 3];
    float f;
    memcpy(&f, &u, 4);
    return f;
}
uint16_t le16(const std::string &b, size_t o) { return (uint16_t)((uint8_t)b[o] | (uint8_t)b[o + 1] << 8); }
uint32_t le32(const std::string &b, size_t o) { return (uint32_t)le16(b, o) | (uint32_t)le16(b, o + 2) << 16; }
double le_float(const std::string &b, size_t o) {
    uint32_t u = le32(b, o);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
void put16(std::string &b, uint16_t v) { b.push_back((char)(v & 255)); b.push_back((char)(v >> 8)); }
void put32(std::string &b, uint32_t v) { put16(b, (uint16_t)(v & 0xFFFF)); put16(b, (uint16_t)(v >> 16)); }
void put_float(std::string &b, double v) {
    float f = (float)v;
    uint32_t u;
    memcpy(&u, &f, 4);
    put32(b, u);
}

void normalize(double q[4]) {
    double l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (l > 0)
        for (int k = 0; k < 4; k++) q[k] /= l;
}

// quaternion_decompress_6byte (model_animations.c): four 12-bit components in three words, each widened back to 16
// bits with its top bits repeated below.
void unpack6(const uint16_t w[3], double q[4]) {
    int16_t i = (int16_t)((w[0] >> 12) | (w[0] & 0xFFF0));
    int16_t j = (int16_t)(((w[1] >> 4) & 0x0FF0) | (w[0] & 0x000F) | (uint16_t)(w[0] << 12));
    int16_t k = (int16_t)(((((w[2] >> 4) & 0x0F00) | (w[1] & 0x00F0)) >> 4) | (uint16_t)(w[1] << 8));
    int16_t ww = (int16_t)(((w[2] >> 8) & 0x000F) | (uint16_t)(w[2] << 4));
    q[0] = i / 32767.0;
    q[1] = j / 32767.0;
    q[2] = k / 32767.0;
    q[3] = ww / 32767.0;
}
// quaternion_compress_6byte, from four 16-bit components whose low 4 bits are dropped.
void pack6(const int16_t c[4], uint16_t w[3]) {
    uint16_t i = (uint16_t)c[0], j = (uint16_t)c[1], k = (uint16_t)c[2], ww = (uint16_t)c[3];
    w[0] = (uint16_t)((i & 0xFFF0) | (j >> 12));
    w[1] = (uint16_t)(((j & 0xFFF0) << 4) | ((k >> 8) & 0x00FF));
    w[2] = (uint16_t)(((k & 0x00F0) << 8) | (ww >> 4));
}
// The 6-byte form of a unit quaternion that decodes nearest it (each component's 12-bit value and its neighbours).
void quantize(const double q[4], uint16_t w[3], double out[4]) {
    int16_t c[4];
    for (int a = 0; a < 4; a++) {
        double best = 1e9;
        int base = (int)std::lround(q[a] * 32767.0 / 16.0);
        c[a] = 0;
        for (int d = -1; d <= 1; d++) {
            int v = std::clamp(base + d, -2048, 2047);
            uint16_t s = (uint16_t)(v << 4);
            int16_t dec = (int16_t)((s & 0xFFF0) | (s >> 12));   // (the fill is the top 4 bits for every component)
            double err = std::fabs(dec / 32767.0 - q[a]);
            if (err < best) { best = err; c[a] = (int16_t)s; }
        }
    }
    pack6(c, w);
    unpack6(w, out);
}

double quat_angle(const double a[4], const double b[4]) {
    double na[4] = {a[0], a[1], a[2], a[3]}, nb[4] = {b[0], b[1], b[2], b[3]};
    normalize(na);
    normalize(nb);
    double d = std::fabs(na[0] * nb[0] + na[1] * nb[1] + na[2] * nb[2] + na[3] * nb[3]);
    return 2 * std::acos(std::min(1.0, d));
}
// quaternions_interpolate_and_normalize: a straight blend (through the nearer of q1 and -q1), normalized.
void nlerp(const double a[4], const double b[4], double t, double out[4]) {
    double tt = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0 ? -t : t, v = 1 - t;
    for (int k = 0; k < 4; k++) out[k] = a[k] * v + b[k] * tt;
    normalize(out);
}

// Keyframes for one track (its value at frame 0 is the default): the frames after 0 to keep, each segment as long as
// every frame inside it blends within `tol` of its value (at most 512 frames, which keeps a long, steady track quick);
// none if the whole track stays within `tol` of frame 0. `still(f)`: how far frame f is from frame 0; `err(f, a, b,
// t)`: how far frame f is from the blend of frames a and b (as stored) at t.
std::vector<int> pick_keys(int frames, double tol, const std::function<double(int)> &still,
                           const std::function<double(int, int, int, double)> &err) {
    std::vector<int> keys;
    bool moves = false;
    for (int f = 1; f < frames && !moves; f++) moves = still(f) > tol;
    if (!moves) return keys;
    int cur = 0;
    while (cur < frames - 1) {
        int best = cur + 1;
        for (int nx = cur + 2; nx < frames && nx - cur <= 512; nx++) {
            bool ok = true;
            for (int f = cur + 1; f < nx && ok; f++) ok = err(f, cur, nx, double(f - cur) / (nx - cur)) <= tol;
            if (!ok) break;
            best = nx;
        }
        keys.push_back(best);
        cur = best;
    }
    return keys;
}

}   // namespace

bool decode_tag_frames(const std::string &default_data, const std::string &frame_data, Clip &c) {
    size_t fs = 0, ds = 0;
    for (int n = 0; n < c.nodes; n++) {
        (flag(c.rot, n) ? fs : ds) += 8;
        (flag(c.tr, n) ? fs : ds) += 12;
        (flag(c.sc, n) ? fs : ds) += 4;
    }
    if (frame_data.size() < fs * c.frames || default_data.size() != ds) return false;
    c.p.assign((size_t)c.frames * c.nodes, Pose());
    for (int f = 0; f < c.frames; f++) {
        size_t o = fs * f, d = 0;
        for (int n = 0; n < c.nodes; n++) {
            Pose &p = c.at(f, n);
            auto rot = [&](const std::string &b, size_t &x) {
                for (int k = 0; k < 4; k++) p.q[k] = be16s(b, x + 2 * k) / 32767.0;
                normalize(p.q);
                x += 8;
            };
            auto tr = [&](const std::string &b, size_t &x) {
                for (int k = 0; k < 3; k++) p.t[k] = be_float(b, x + 4 * k);
                x += 12;
            };
            auto sc = [&](const std::string &b, size_t &x) {
                p.s = be_float(b, x);
                x += 4;
            };
            flag(c.rot, n) ? rot(frame_data, o) : rot(default_data, d);
            flag(c.tr, n) ? tr(frame_data, o) : tr(default_data, d);
            flag(c.sc, n) ? sc(frame_data, o) : sc(default_data, d);
        }
    }
    return true;
}

bool decode_compressed(const std::string &b, Clip &c) {
    if (b.size() < 44) return false;
    uint32_t off[11];
    for (int k = 0; k < 11; k++) off[k] = le32(b, 4 * k);
    const uint32_t rot_idx = off[0], rot_def = off[1], rot_key = off[2], tr_head = off[3], tr_idx = off[4], tr_def = off[5],
                   tr_key = off[6], sc_head = off[7], sc_idx = off[8], sc_def = off[9], sc_key = off[10];
    auto fits = [&](size_t o, size_t n) { return o + n <= b.size(); };
    c.p.assign((size_t)c.frames * c.nodes, Pose());
    int ri = 0, ti = 0, si = 0;
    // which segment frame f falls in, the engine's way: (this frame, its value index or -1 for the default, next ...)
    struct Seg { int f0, k0, f1, k1; };
    auto segment = [&](uint32_t idx_off, uint32_t first, uint32_t count, int f) -> Seg {
        auto kf = [&](uint32_t k) { return (int)le16(b, idx_off + 2 * (first + k)); };
        int last = kf(count - 1);
        if (f > last) return {f, (int)count - 1, f, (int)count - 1};
        if (f < kf(0)) return {0, -1, kf(0), 0};
        if (f == last) return {last, (int)count - 1, last + 1, -1};
        uint32_t k = 0;
        while (k + 1 < count && kf(k + 1) <= f) k++;
        return {kf(k), (int)k, kf(k + 1), (int)k + 1};
    };
    for (int n = 0; n < c.nodes; n++) {
        // rotation
        if (!fits(rot_def + 6 * (size_t)n, 6)) return false;
        auto q6 = [&](size_t o, double q[4]) { uint16_t w[3] = {le16(b, o), le16(b, o + 2), le16(b, o + 4)}; unpack6(w, q); };
        double dq[4];
        q6(rot_def + 6 * (size_t)n, dq);
        if (flag(c.rot, n)) {
            size_t h = 44 + 4 * (size_t)ri++;
            if (!fits(h, 4)) return false;
            uint32_t hd = le32(b, h), first = hd >> 12, count = hd & 4095;
            if (count && (!fits(rot_key + 6 * (size_t)(first + count - 1), 6) || !fits(rot_idx + 2 * (size_t)(first + count - 1), 2)))
                return false;
            for (int f = 0; f < c.frames; f++) {
                Pose &p = c.at(f, n);
                if (!count) { memcpy(p.q, dq, sizeof dq); normalize(p.q); continue; }
                Seg s = segment(rot_idx, first, count, f);
                double a[4], z[4];
                if (s.k0 < 0) memcpy(a, dq, sizeof a); else q6(rot_key + 6 * (size_t)(first + s.k0), a);
                if (f == s.f0) { memcpy(p.q, a, sizeof a); normalize(p.q); continue; }
                if (s.k1 < 0) memcpy(z, dq, sizeof z); else q6(rot_key + 6 * (size_t)(first + s.k1), z);
                nlerp(a, z, double(f - s.f0) / (s.f1 - s.f0), p.q);
            }
        } else {
            for (int f = 0; f < c.frames; f++) { memcpy(c.at(f, n).q, dq, sizeof dq); normalize(c.at(f, n).q); }
        }
        // translation
        if (!fits(tr_def + 12 * (size_t)n, 12)) return false;
        double dt[3] = {le_float(b, tr_def + 12 * n), le_float(b, tr_def + 12 * n + 4), le_float(b, tr_def + 12 * n + 8)};
        if (flag(c.tr, n)) {
            size_t h = tr_head + 4 * (size_t)ti++;
            if (!fits(h, 4)) return false;
            uint32_t hd = le32(b, h), first = hd >> 12, count = hd & 4095;
            if (count && (!fits(tr_key + 12 * (size_t)(first + count - 1), 12) || !fits(tr_idx + 2 * (size_t)(first + count - 1), 2)))
                return false;
            auto key = [&](int k, double v[3]) {
                if (k < 0) { memcpy(v, dt, sizeof dt); return; }
                size_t o = tr_key + 12 * (size_t)(first + k);
                for (int a = 0; a < 3; a++) v[a] = le_float(b, o + 4 * a);
            };
            for (int f = 0; f < c.frames; f++) {
                Pose &p = c.at(f, n);
                if (!count) { memcpy(p.t, dt, sizeof dt); continue; }
                Seg s = segment(tr_idx, first, count, f);
                double a[3], z[3];
                key(s.k0, a);
                if (f == s.f0) { memcpy(p.t, a, sizeof a); continue; }
                key(s.k1, z);
                double t = double(f - s.f0) / (s.f1 - s.f0);
                for (int k = 0; k < 3; k++) p.t[k] = a[k] * (1 - t) + z[k] * t;
            }
        } else {
            for (int f = 0; f < c.frames; f++) memcpy(c.at(f, n).t, dt, sizeof dt);
        }
        // scale
        if (flag(c.sc, n)) {
            size_t h = sc_head + 4 * (size_t)si;
            if (!fits(h, 4) || !fits(sc_def + 4 * (size_t)si, 4)) return false;
            double ds = le_float(b, sc_def + 4 * (size_t)si);
            si++;
            uint32_t hd = le32(b, h), first = hd >> 12, count = hd & 4095;
            if (count && (!fits(sc_key + 4 * (size_t)(first + count - 1), 4) || !fits(sc_idx + 2 * (size_t)(first + count - 1), 2)))
                return false;
            auto key = [&](int k) { return k < 0 ? ds : le_float(b, sc_key + 4 * (size_t)(first + k)); };
            for (int f = 0; f < c.frames; f++) {
                Pose &p = c.at(f, n);
                if (!count) { p.s = ds; continue; }
                Seg s = segment(sc_idx, first, count, f);
                double a = key(s.k0);
                if (f == s.f0) { p.s = a; continue; }
                double t = double(f - s.f0) / (s.f1 - s.f0);
                p.s = a * (1 - t) + key(s.k1) * t;
            }
        } else {
            for (int f = 0; f < c.frames; f++) c.at(f, n).s = 1;
        }
    }
    return true;
}

std::string encode(const Clip &c, double angle, double units, double scale) {
    const int N = c.nodes, F = c.frames;
    if (F < 1 || N < 1) return "";
    for (int n = 0; n < N; n++)   // a still node's scale is 1 in this format
        if (!flag(c.sc, n) && std::fabs(c.at(0, n).s - 1) > 1e-4) return "";

    // Every frame's rotations as stored (quantized), to choose keyframes by what the engine will blend.
    std::vector<std::array<uint16_t, 3>> q6((size_t)F * N);
    std::vector<std::array<double, 4>> qd((size_t)F * N);
    for (int f = 0; f < F; f++)
        for (int n = 0; n < N; n++) {
            uint16_t w[3];
            double o[4];
            quantize(c.at(f, n).q, w, o);
            q6[(size_t)f * N + n] = {w[0], w[1], w[2]};
            qd[(size_t)f * N + n] = {o[0], o[1], o[2], o[3]};
        }

    std::vector<uint32_t> rot_head, tr_head, sc_head;
    std::string rot_idx, rot_keys, tr_idx, tr_keys, sc_idx, sc_def, sc_keys;
    uint32_t rk = 0, tk = 0, skn = 0;
    for (int n = 0; n < N; n++) {
        if (flag(c.rot, n)) {
            auto keys = pick_keys(F, angle, [&](int f) { return quat_angle(c.at(f, n).q, qd[n].data()); },
                                  [&](int f, int a, int z, double t) {
                                      double m[4];
                                      nlerp(qd[(size_t)a * N + n].data(), qd[(size_t)z * N + n].data(), t, m);
                                      return quat_angle(m, c.at(f, n).q);
                                  });
            if (keys.size() > 4095) return "";
            rot_head.push_back(rk << 12 | (uint32_t)keys.size());
            for (int k : keys) {
                put16(rot_idx, (uint16_t)k);
                for (int a = 0; a < 3; a++) put16(rot_keys, q6[(size_t)k * N + n][a]);
            }
            rk += (uint32_t)keys.size();
        }
        if (flag(c.tr, n)) {
            auto dist = [&](const double a[3], const double b[3]) {
                return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
            };
            auto keys = pick_keys(F, units, [&](int f) { return dist(c.at(f, n).t, c.at(0, n).t); },
                                  [&](int f, int a, int z, double t) {
                                      double m[3];
                                      for (int k = 0; k < 3; k++) m[k] = c.at(a, n).t[k] * (1 - t) + c.at(z, n).t[k] * t;
                                      return dist(m, c.at(f, n).t);
                                  });
            if (keys.size() > 4095) return "";
            tr_head.push_back(tk << 12 | (uint32_t)keys.size());
            for (int k : keys) {
                put16(tr_idx, (uint16_t)k);
                for (int a = 0; a < 3; a++) put_float(tr_keys, c.at(k, n).t[a]);
            }
            tk += (uint32_t)keys.size();
        }
        if (flag(c.sc, n)) {
            auto keys = pick_keys(F, scale, [&](int f) { return std::fabs(c.at(f, n).s - c.at(0, n).s); },
                                  [&](int f, int a, int z, double t) { return std::fabs(c.at(a, n).s * (1 - t) + c.at(z, n).s * t - c.at(f, n).s); });
            if (keys.size() > 4095) return "";
            sc_head.push_back(skn << 12 | (uint32_t)keys.size());
            put_float(sc_def, c.at(0, n).s);
            for (int k : keys) {
                put16(sc_idx, (uint16_t)k);
                put_float(sc_keys, c.at(k, n).s);
            }
            skn += (uint32_t)keys.size();
        }
    }
    if (rk > 32767 || tk > 32767 || skn > 32767) return "";   // (the engine reads the first keyframe's index as a short)

    // Laid out as Bungie's tools do (and the CMT tagset's H1A-compressed animations are): the 11 offsets, rotation node
    // headers, then each kind's frame indices, defaults (every node's; scale: the scaled nodes') and keyframes.
    std::string rot_def, tr_def;
    for (int n = 0; n < N; n++) {
        const auto &w = q6[n];   // frame 0
        for (int a = 0; a < 3; a++) put16(rot_def, w[a]);
        for (int a = 0; a < 3; a++) put_float(tr_def, c.at(0, n).t[a]);
    }
    std::string body;
    for (auto h : rot_head) put32(body, h);
    uint32_t o[11];
    size_t base = 44;
    auto at = [&](const std::string &part) {
        uint32_t x = (uint32_t)(base + body.size());
        body += part;
        return x;
    };
    o[0] = at(rot_idx);
    o[1] = at(rot_def);
    o[2] = at(rot_keys);
    std::string th;
    for (auto h : tr_head) put32(th, h);
    o[3] = at(th);
    o[4] = at(tr_idx);
    o[5] = at(tr_def);
    o[6] = at(tr_keys);
    std::string sh;
    for (auto h : sc_head) put32(sh, h);
    o[7] = at(sh);
    o[8] = at(sc_idx);
    o[9] = at(sc_def);
    o[10] = at(sc_keys);
    std::string out;
    for (auto x : o) put32(out, x);
    return out + body;
}

double world_error(const Clip &a, const Clip &b, const std::vector<int> &parent, const std::vector<double> &lever) {
    const int N = a.nodes;
    struct W { double q[4], p[3], s; };
    auto rotate = [](const double q[4], const double v[3], double r[3]) {   // quaternion_transform_point
        double w2 = 2 * q[3] * q[3] - 1, vd = 2 * (q[0] * v[0] + q[1] * v[1] + q[2] * v[2]), tw = 2 * q[3];
        double cx = q[1] * v[2] - q[2] * v[1], cy = q[2] * v[0] - q[0] * v[2], cz = q[0] * v[1] - q[1] * v[0];
        r[0] = cx * tw + vd * q[0] + w2 * v[0];
        r[1] = cy * tw + vd * q[1] + w2 * v[1];
        r[2] = cz * tw + vd * q[2] + w2 * v[2];
    };
    auto mul = [](const double x[4], const double y[4], double r[4]) {   // x * y (Hamilton; i,j,k,w)
        r[3] = x[3] * y[3] - x[0] * y[0] - x[1] * y[1] - x[2] * y[2];
        r[0] = x[3] * y[0] + x[0] * y[3] + x[1] * y[2] - x[2] * y[1];
        r[1] = x[3] * y[1] - x[0] * y[2] + x[1] * y[3] + x[2] * y[0];
        r[2] = x[3] * y[2] + x[0] * y[1] - x[1] * y[0] + x[2] * y[3];
    };
    auto pose = [&](const Clip &c, int f, std::vector<W> &w) {
        w.assign(N, W());
        std::vector<char> done(N, 0);
        std::function<void(int)> go = [&](int n) {
            if (done[n]) return;
            const Pose &p = c.at(f, n);
            int pa = n < (int)parent.size() ? parent[n] : -1;
            if (pa < 0 || pa >= N || pa == n) {
                memcpy(w[n].q, p.q, sizeof p.q);
                memcpy(w[n].p, p.t, sizeof p.t);
                w[n].s = p.s;
            } else {
                done[n] = 1;   // (a loop in the parents stops here)
                go(pa);
                double r[3];
                rotate(w[pa].q, p.t, r);
                for (int k = 0; k < 3; k++) w[n].p[k] = w[pa].p[k] + w[pa].s * r[k];
                mul(w[pa].q, p.q, w[n].q);
                normalize(w[n].q);
                w[n].s = w[pa].s * p.s;
            }
            done[n] = 1;
        };
        for (int n = 0; n < N; n++) go(n);
    };
    double worst = 0;
    std::vector<W> wa, wb;
    for (int f = 0; f < a.frames; f++) {
        pose(a, f, wa);
        pose(b, f, wb);
        for (int n = 0; n < N; n++) {
            double L = n < (int)lever.size() ? lever[n] : 0;
            for (int ax = -1; ax < 3; ax++) {
                double v[3] = {0, 0, 0}, ra[3] = {0, 0, 0}, rb[3] = {0, 0, 0};
                if (ax >= 0) {
                    v[ax] = L;
                    rotate(wa[n].q, v, ra);
                    rotate(wb[n].q, v, rb);
                }
                double d = 0;
                for (int k = 0; k < 3; k++) {
                    double x = (wa[n].p[k] + wa[n].s * ra[k]) - (wb[n].p[k] + wb[n].s * rb[k]);
                    d += x * x;
                }
                worst = std::max(worst, std::sqrt(d));
            }
        }
    }
    return worst;
}

std::vector<double> levers(const Clip &c, const std::vector<int> &parent, double minimum) {
    const int N = c.nodes;
    std::vector<double> len(N, 0), own(N, 0);
    for (int n = 0; n < N; n++) {
        const Pose &p = c.at(0, n);
        own[n] = std::sqrt(p.t[0] * p.t[0] + p.t[1] * p.t[1] + p.t[2] * p.t[2]);
        int pa = n < (int)parent.size() ? parent[n] : -1;
        if (pa >= 0 && pa < N && pa != n) len[pa] = std::max(len[pa], own[n]);
    }
    for (int n = 0; n < N; n++)
        if (len[n] == 0) len[n] = own[n];
    for (auto &x : len) x = std::max(x, minimum);
    return len;
}

GraphResult compress_graph(tagfile::TagNode &root, int level) {
    using namespace tagfile;
    GraphResult r;
    std::vector<TagNode> *anims = block(root, "animations"), *nodes = block(root, "nodes");
    if (!anims) return r;
    std::vector<int> parent;
    if (nodes)
        for (auto &n : *nodes) parent.push_back((int)(int16_t)(uint16_t)get_num(n, "parent node index"));
    const double angle = (level >= 2 ? 0.2 : 0.05) * kPi / 180, units = level >= 2 ? 0.0004 : 0.0001,
                 scale = level >= 2 ? 0.002 : 0.0005, slack = level >= 2 ? 0.002 : 0.0003;
    // Animations played by a value whatever their type: a device's position and power (devices.c: a wheel spinning as
    // a machine's position loops), an object's animations driven by its functions, a vehicle's (steering, velocity,
    // suspension). The engine plays them as it plays an overlay by a value, so they are kept as overlays are.
    std::set<int> by_value;
    auto collect = [&](const char *outer, std::initializer_list<const char *> inner) {
        std::vector<TagNode> *b = block(root, outer);
        if (!b) return;
        for (auto &x : *b) {
            if (inner.size() == 0) by_value.insert((int)(int16_t)(uint16_t)get_num(x, "animation"));
            for (auto in : inner)
                if (std::vector<TagNode> *c = block(x, in))
                    for (auto &y : *c) by_value.insert((int)(int16_t)(uint16_t)get_num(y, "animation"));
        }
    };
    collect("objects", {});
    collect("devices", {"animations"});
    collect("vehicles", {"animations", "suspension animations"});
    for (size_t ai = 0; ai < anims->size(); ai++) {
        TagNode &e = (*anims)[ai];
        int flags = (int)get_num(e, "flags");
        if (get_num(e, "type") == 1 || by_value.count((int)ai)) {
            // An overlay stays (or goes back to) its frames: the engine blends a compressed overlay's rotations only at
            // whole frames when it plays one by a value (overlay_animation_apply_continuous: a vehicle's velocity,
            // steering and suspension, a rider's acceleration, a device's position), so they snap from one frame to
            // the next as the value moves (a hovering Ghost jitters between its leaning poses; Deathtrap's wheels
            // turned a frame of their 60 every 4 of the game's). A compressed one whose frames are still in the tag
            // (MCC's) has them back; one without them is left as it is.
            std::string *fd = data(e, "frame data"), *def = data(e, "default data");
            size_t off = (size_t)get_num(e, "offset to compressed data");
            if ((flags & 1) && fd && def && off > 0 && off <= fd->size()) {
                Clip c;
                c.frames = (int)get_num(e, "frame count");
                c.nodes = (int)get_num(e, "node count");
                for (int k = 0; k < 2; k++) {
                    c.rot[k] = (uint32_t)get_num(e, "node rotation flag data", k);
                    c.tr[k] = (uint32_t)get_num(e, "node transform flag data", k);
                    c.sc[k] = (uint32_t)get_num(e, "node scale flag data", k);
                }
                if (c.nodes <= 64 && decode_tag_frames(*def, fd->substr(0, off), c)) {
                    fd->resize(off);
                    set_num(e, "flags", flags & ~1);
                    set_num(e, "offset to compressed data", 0);
                    r.overlays_restored++;
                }
            }
            continue;
        }
        if ((flags & 1) || level < 1) continue;
        Clip c;
        c.frames = (int)get_num(e, "frame count");
        c.nodes = (int)get_num(e, "node count");
        for (int k = 0; k < 2; k++) {
            c.rot[k] = (uint32_t)get_num(e, "node rotation flag data", k);
            c.tr[k] = (uint32_t)get_num(e, "node transform flag data", k);
            c.sc[k] = (uint32_t)get_num(e, "node scale flag data", k);
        }
        std::string *fd = data(e, "frame data"), *def = data(e, "default data"), *info = data(e, "frame info");
        if (!fd || !def || !info || c.nodes > 64 || !decode_tag_frames(*def, *fd, c)) continue;
        r.animations++;
        size_t was = info->size() + def->size() + (size_t)get_num(e, "frame size") * c.frames;
        r.before += was;
        // the rounding alone (every frame a keyframe), then the tolerances, tighter until within `slack` of it
        auto lev = levers(c, parent, 0.01);
        Clip d = c;
        std::string blob = encode(c, 0, 0, 0);
        if (blob.empty() || !decode_compressed(blob, d)) { r.after += was; continue; }
        double floor = world_error(c, d, parent, lev), err = 0;
        std::string best;
        double s = 1;
        for (int tries = 0; tries < 4 && best.empty(); tries++, s /= 2) {
            std::string b = encode(c, angle * s, units * s, scale * s);
            if (b.empty() || !decode_compressed(b, d)) break;
            err = world_error(c, d, parent, lev);
            if (err <= floor + slack) best = b;
        }
        if (best.empty() || info->size() + best.size() >= was) { r.after += was; continue; }
        set_num(e, "flags", flags | 1);
        set_num(e, "offset to compressed data", (double)fd->size());
        *fd += best;
        r.after += info->size() + best.size();
        r.compressed++;
        r.worst = std::max(r.worst, err);
    }
    return r;
}

}   // namespace animcomp
