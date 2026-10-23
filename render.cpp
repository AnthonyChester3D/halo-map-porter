// Halo Map Porter - software drawing of a model preview (see render.hpp)
#include "render.hpp"

#include <algorithm>
#include <cmath>

namespace render {

namespace {
struct V3 { float x, y, z; };
V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 norm(V3 a) {
    float l = std::sqrt(dot(a, a));
    return l > 0 ? V3{a.x / l, a.y / l, a.z / l} : V3{0, 0, 0};
}
}   // namespace

void draw(const porter::ModelPreview &m, const View &v, int w, int h, std::vector<uint32_t> &px) {
    px.assign((size_t)std::max(w, 0) * std::max(h, 0), 0);
    if (w <= 0 || h <= 0) return;
    const int W = w * 2, H = h * 2;   // drawn at twice the size, then halved
    std::vector<uint32_t> big((size_t)W * H);
    std::vector<float> depth((size_t)W * H, 0.0f);   // 1 / distance (0: nothing drawn)
    for (int y = 0; y < H; y++) {                    // background: a dark blue-grey, lighter at the top
        float t = (float)y / H;
        uint32_t r = (uint32_t)(64 - 34 * t), g = (uint32_t)(70 - 36 * t), b = (uint32_t)(82 - 40 * t);
        std::fill(big.begin() + (size_t)y * W, big.begin() + (size_t)(y + 1) * W, r << 16 | g << 8 | b);
    }
    size_t nv = m.pos.size() / 3;
    if (nv && !m.tris.empty()) {
        // The model's middle and size.
        V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
        for (size_t i = 0; i < nv; i++) {
            const float *p = &m.pos[i * 3];
            lo = {std::min(lo.x, p[0]), std::min(lo.y, p[1]), std::min(lo.z, p[2])};
            hi = {std::max(hi.x, p[0]), std::max(hi.y, p[1]), std::max(hi.z, p[2])};
        }
        V3 c{(lo.x + hi.x) / 2, (lo.y + hi.y) / 2, (lo.z + hi.z) / 2};
        float radius = 0;
        for (size_t i = 0; i < nv; i++) {
            V3 d = sub(V3{m.pos[i * 3], m.pos[i * 3 + 1], m.pos[i * 3 + 2]}, c);
            radius = std::max(radius, dot(d, d));
        }
        radius = std::sqrt(radius);
        if (radius <= 0) radius = 1;
        // View: x right, y away from the viewer, z up.
        float cy = std::cos(v.yaw), sy = std::sin(v.yaw), cp = std::cos(v.pitch), sp = -std::sin(v.pitch);   // (from above)
        // Size: the model fills the view from every side at this height (the widest and tallest it gets as it turns),
        // so a long, thin rifle isn't drawn as small as a ball would be, and turning it doesn't change its size.
        float ex = 1e-6f, ez = 1e-6f;
        for (int k = 0; k < 16; k++) {
            float a = k * 3.14159265f / 8, ca = std::cos(a), sa = std::sin(a);
            for (size_t i = 0; i < nv; i++) {
                float x = m.pos[i * 3] - c.x, y = m.pos[i * 3 + 1] - c.y, z = m.pos[i * 3 + 2] - c.z;
                ex = std::max(ex, std::fabs(ca * x + sa * y));
                ez = std::max(ez, std::fabs(sp * (sa * x - ca * y) + cp * z));
            }
        }
        float dist = radius * 4.0f;   // (seen from a little way off: some perspective, not much)
        float focal = std::min(W * 0.45f / ex, H * 0.45f / ez) * (dist - radius * 0.5f) * std::max(0.2f, v.zoom);
        std::vector<V3> view(nv);
        std::vector<float> sx(nv), syy(nv), iz(nv);
        for (size_t i = 0; i < nv; i++) {
            V3 p = sub(V3{m.pos[i * 3], m.pos[i * 3 + 1], m.pos[i * 3 + 2]}, c);
            V3 q{cy * p.x + sy * p.y, -sy * cp * p.x + cy * cp * p.y + sp * p.z, sy * sp * p.x - cy * sp * p.y + cp * p.z};
            view[i] = q;
            float d = dist + q.y;
            iz[i] = d > radius * 0.05f ? 1.0f / d : 0.0f;
            sx[i] = W * 0.5f + focal * q.x * iz[i];
            syy[i] = H * 0.5f - focal * q.z * iz[i];
        }
        const V3 light = norm(V3{-0.45f, -0.75f, 0.5f});   // from the viewer's upper left
        for (auto &part : m.parts) {
            const porter::ModelPreview::Texture *tex =
                part.texture >= 0 && part.texture < (int)m.textures.size() ? &m.textures[part.texture] : nullptr;
            for (uint32_t t = part.first; t < part.first + part.count && (size_t)t * 3 + 2 < m.tris.size(); t++) {
                uint32_t a = m.tris[t * 3], b = m.tris[t * 3 + 1], cc = m.tris[t * 3 + 2];
                if (a >= nv || b >= nv || cc >= nv || !iz[a] || !iz[b] || !iz[cc]) continue;
                // Halo's winding: (b - a) x (c - a) points into the model. Both sides are drawn.
                V3 n = norm(cross(sub(view[b], view[a]), sub(view[cc], view[a])));
                n = {-n.x, -n.y, -n.z};
                V3 to_viewer = norm(V3{-view[a].x, -(dist + view[a].y), -view[a].z});
                bool back = dot(n, to_viewer) < 0;
                if (back) n = {-n.x, -n.y, -n.z};
                float shade = 0.55f + 0.7f * std::max(0.0f, dot(n, light));   // (bright: many models' textures are dark)
                if (back) shade *= 0.8f;
                float x0 = sx[a], y0 = syy[a], x1 = sx[b], y1 = syy[b], x2 = sx[cc], y2 = syy[cc];
                float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
                if (std::fabs(area) < 1e-6f) continue;
                int minx = std::max(0, (int)std::floor(std::min({x0, x1, x2}))), maxx = std::min(W - 1, (int)std::ceil(std::max({x0, x1, x2})));
                int miny = std::max(0, (int)std::floor(std::min({y0, y1, y2}))), maxy = std::min(H - 1, (int)std::ceil(std::max({y0, y1, y2})));
                if (minx > maxx || miny > maxy) continue;
                float ua = m.uv[a * 2] * iz[a], va = m.uv[a * 2 + 1] * iz[a], ub = m.uv[b * 2] * iz[b], vb = m.uv[b * 2 + 1] * iz[b],
                      uc = m.uv[cc * 2] * iz[cc], vc = m.uv[cc * 2 + 1] * iz[cc];
                float inv = 1.0f / area;
                // For the triangles' edges: each one's length (the one across from each corner).
                float len0 = std::hypot(x2 - x1, y2 - y1), len1 = std::hypot(x0 - x2, y0 - y2), len2 = std::hypot(x1 - x0, y1 - y0);
                float twice = std::fabs(area);
                for (int y = miny; y <= maxy; y++) {
                    float py = y + 0.5f;
                    for (int x = minx; x <= maxx; x++) {
                        float pxf = x + 0.5f;
                        float w0 = ((x1 - pxf) * (y2 - py) - (x2 - pxf) * (y1 - py)) * inv;
                        float w1 = ((x2 - pxf) * (y0 - py) - (x0 - pxf) * (y2 - py)) * inv;
                        float w2 = 1.0f - w0 - w1;
                        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                        float z = w0 * iz[a] + w1 * iz[b] + w2 * iz[cc];
                        size_t o = (size_t)y * W + x;
                        if (z <= depth[o]) continue;
                        depth[o] = z;
                        int r = 178, g = 178, bl = 172;
                        if (tex) {
                            float u = (w0 * ua + w1 * ub + w2 * uc) / z, vv = (w0 * va + w1 * vb + w2 * vc) / z;
                            u -= std::floor(u);
                            vv -= std::floor(vv);
                            int tx = std::min(tex->w - 1, (int)(u * tex->w)), ty = std::min(tex->h - 1, (int)(vv * tex->h));
                            const uint8_t *s = &tex->rgba[((size_t)ty * tex->w + tx) * 4];
                            r = s[0];
                            g = s[1];
                            bl = s[2];
                        }
                        float fr = r * shade, fg = g * shade, fb = bl * shade;
                        if (t < m.edges.size() && m.edges[t]) {   // a polygon's outline: dark
                            uint8_t e = m.edges[t];
                            float d = 1e9f;
                            if (e & 1) d = std::min(d, w2 * twice / std::max(len2, 1e-6f));   // first to second corner
                            if (e & 2) d = std::min(d, w0 * twice / std::max(len0, 1e-6f));   // second to third
                            if (e & 4) d = std::min(d, w1 * twice / std::max(len1, 1e-6f));   // third to first
                            if (d < 1.6f) {
                                fr *= 0.25f;
                                fg *= 0.25f;
                                fb *= 0.25f;
                            }
                        }
                        if (v.wire) {   // near an edge (within a pixel and a bit, at twice the size): a light blue line
                            float d = std::min({w0 * twice / std::max(len0, 1e-6f), w1 * twice / std::max(len1, 1e-6f),
                                                w2 * twice / std::max(len2, 1e-6f)});
                            if (d < 1.3f) {
                                fr = fr * 0.3f + 120 * 0.7f;
                                fg = fg * 0.3f + 210 * 0.7f;
                                fb = fb * 0.3f + 255 * 0.7f;
                            }
                        }
                        uint32_t R = (uint32_t)std::min(255.0f, fr), G = (uint32_t)std::min(255.0f, fg), B = (uint32_t)std::min(255.0f, fb);
                        big[o] = R << 16 | G << 8 | B;
                    }
                }
            }
        }
    }
    // Halve: the average of each 2 x 2.
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t r = 0, g = 0, b = 0;
            for (int k = 0; k < 4; k++) {
                uint32_t p = big[(size_t)(y * 2 + k / 2) * W + x * 2 + k % 2];
                r += p >> 16 & 255;
                g += p >> 8 & 255;
                b += p & 255;
            }
            px[(size_t)y * w + x] = (r / 4) << 16 | (g / 4) << 8 | (b / 4);
        }
}

}   // namespace render
