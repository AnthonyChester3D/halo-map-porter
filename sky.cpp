// Halo Map Porter - a sky's cost on the Xbox and making it lighter (sky.hpp).
#include "sky.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>

#include "bcdec.h"
#include "meshoptimizer/meshoptimizer.h"
#include "tagfile.hpp"

namespace sky {
namespace {

namespace fs = std::filesystem;
using tagfile::TagNode;
const double kPi = 3.14159265358979323846;

// ---- Textures

struct Image {
    int w = 0, h = 0;
    std::vector<float> p;                 // RGBA, top row first
    float lo[4] = {1, 1, 1, 1}, hi[4] = {0, 0, 0, 0};   // each channel's range over the texture
};

// The first texture of a bitmap tag, its most detailed level, as the Xbox samples it (A8: white with alpha; Y8 and
// AY8 grey). False for what isn't a plain 2D texture.
bool load_image(const fs::path &file, Image &out) {
    tagfile::Tag t;
    if (!t.load(file)) return false;
    tagfile::Elem r = t.root();
    tagfile::Block bd = r.block("bitmap data");
    if (!bd.size()) return false;
    tagfile::Elem e = bd[0];
    int w = (int)e.integer("width"), h = (int)e.integer("height"), fmt = (int)e.integer("format");
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || e.integer("type") != 0 || (e.integer("flags") & 8)) return false;
    std::string pix = r.data("processed pixel data");
    size_t off = (size_t)e.integer("pixel data offset");
    if (off >= pix.size()) return false;
    const unsigned char *p = (const unsigned char *)pix.data() + off;
    size_t avail = pix.size() - off;
    out.w = w;
    out.h = h;
    out.p.assign((size_t)w * h * 4, 1.f);
    if (fmt == 14 || fmt == 15 || fmt == 16) {
        int bw = (w + 3) / 4, bh = (h + 3) / 4, bs = fmt == 14 ? 8 : 16;
        if (avail < (size_t)bw * bh * bs) return false;
        unsigned char block[64];
        for (int by = 0; by < bh; by++)
            for (int bx = 0; bx < bw; bx++) {
                const unsigned char *src = p + ((size_t)by * bw + bx) * bs;
                if (fmt == 14) bcdec_bc1(src, block, 16);
                else if (fmt == 15) bcdec_bc2(src, block, 16);
                else bcdec_bc3(src, block, 16);
                for (int y = 0; y < 4 && by * 4 + y < h; y++)
                    for (int x = 0; x < 4 && bx * 4 + x < w; x++)
                        for (int c = 0; c < 4; c++)
                            out.p[((size_t)(by * 4 + y) * w + bx * 4 + x) * 4 + c] = block[(y * 4 + x) * 4 + c] / 255.f;
            }
    } else {
        int bpp = fmt <= 2 || fmt == 17 ? 1 : (fmt == 3 || fmt == 6 || fmt == 8 || fmt == 9) ? 2 : (fmt == 10 || fmt == 11) ? 4 : 0;
        if (!bpp || fmt == 17 || avail < (size_t)w * h * bpp) return false;
        for (size_t i = 0; i < (size_t)w * h; i++) {
            float *q = &out.p[i * 4];
            unsigned v = bpp == 2 ? p[i * 2] | (p[i * 2 + 1] << 8) : 0;
            switch (fmt) {
            case 0: q[3] = p[i] / 255.f; break;                                            // A8
            case 1: q[0] = q[1] = q[2] = p[i] / 255.f; break;                              // Y8
            case 2: q[0] = q[1] = q[2] = q[3] = p[i] / 255.f; break;                       // AY8
            case 3: q[0] = q[1] = q[2] = p[i * 2] / 255.f; q[3] = p[i * 2 + 1] / 255.f; break;   // A8Y8
            case 6: q[0] = (v >> 11) / 31.f; q[1] = ((v >> 5) & 63) / 63.f; q[2] = (v & 31) / 31.f; break;
            case 8: q[0] = ((v >> 10) & 31) / 31.f; q[1] = ((v >> 5) & 31) / 31.f; q[2] = (v & 31) / 31.f; q[3] = (float)(v >> 15); break;
            case 9: q[0] = ((v >> 8) & 15) / 15.f; q[1] = ((v >> 4) & 15) / 15.f; q[2] = (v & 15) / 15.f; q[3] = (v >> 12) / 15.f; break;
            case 10: q[0] = p[i * 4 + 2] / 255.f; q[1] = p[i * 4 + 1] / 255.f; q[2] = p[i * 4] / 255.f; break;
            case 11: q[0] = p[i * 4 + 2] / 255.f; q[1] = p[i * 4 + 1] / 255.f; q[2] = p[i * 4] / 255.f; q[3] = p[i * 4 + 3] / 255.f; break;
            }
        }
    }
    for (size_t i = 0; i < (size_t)w * h; i++)
        for (int c = 0; c < 4; c++) {
            out.lo[c] = std::min(out.lo[c], out.p[i * 4 + c]);
            out.hi[c] = std::max(out.hi[c], out.p[i * 4 + c]);
        }
    return true;
}

struct V4 { float v[4] = {0, 0, 0, 0}; };

int wrap(int x, int n, bool clamp) { return clamp ? std::min(std::max(x, 0), n - 1) : ((x % n) + n) % n; }

// Bilinear, at texture coordinates (1 = across the texture).
V4 sample(const Image &im, double u, double v, bool cu, bool cv) {
    double x = u * im.w - 0.5, y = v * im.h - 0.5;
    int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    float fx = (float)(x - x0), fy = (float)(y - y0);
    int xa = wrap(x0, im.w, cu), xb = wrap(x0 + 1, im.w, cu), ya = wrap(y0, im.h, cv), yb = wrap(y0 + 1, im.h, cv);
    V4 o;
    for (int c = 0; c < 4; c++) {
        float a = im.p[((size_t)ya * im.w + xa) * 4 + c], b = im.p[((size_t)ya * im.w + xb) * 4 + c];
        float d = im.p[((size_t)yb * im.w + xa) * 4 + c], e = im.p[((size_t)yb * im.w + xb) * 4 + c];
        o.v[c] = (a * (1 - fx) + b * fx) * (1 - fy) + (d * (1 - fx) + e * fx) * fy;
    }
    return o;
}

struct Iv { float lo = 0, hi = 0; };

// The range of each channel over the texels a box of texture coordinates samples (`margin` texels more each way).
void box_range(const Image &im, double u0, double v0, double u1, double v1, int margin, bool cu, bool cv, Iv out[4]) {
    int x0 = (int)std::floor(u0 * im.w - 0.5) - margin, x1 = (int)std::floor(u1 * im.w - 0.5) + 1 + margin;
    int y0 = (int)std::floor(v0 * im.h - 0.5) - margin, y1 = (int)std::floor(v1 * im.h - 0.5) + 1 + margin;
    if ((!cu && x1 - x0 + 1 >= im.w) || (!cv && y1 - y0 + 1 >= im.h) || (long long)(x1 - x0 + 1) * (y1 - y0 + 1) > 1 << 20) {
        for (int c = 0; c < 4; c++) out[c] = {im.lo[c], im.hi[c]};
        if (!((!cu && x1 - x0 + 1 >= im.w) || (!cv && y1 - y0 + 1 >= im.h))) return;
        return;
    }
    for (int c = 0; c < 4; c++) out[c] = {1e9f, -1e9f};
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            const float *q = &im.p[((size_t)wrap(y, im.h, cv) * im.w + wrap(x, im.w, cu)) * 4];
            for (int c = 0; c < 4; c++) {
                out[c].lo = std::min(out[c].lo, q[c]);
                out[c].hi = std::max(out[c].hi, q[c]);
            }
        }
}

// ---- Shaders

// A transparent chicago map as the Xbox draws it (rasterizer_xbox_transparent_geometry.c).
struct MapI {
    std::string bitmap;
    int cf = 0, af = 0, flags = 0;   // flags: 1 unfiltered, 2 alpha replicate (the next map's), 4 u clamped, 8 v clamped
    double us = 1, vs = 1, uo = 0, vo = 0;
    bool moving = false;             // its texture slides, turns, or follows an object function
    const Image *img = nullptr;
};
// A shader_transparent_generic's stage: its combiners' inputs and outputs (as the tag has them).
struct GStage {
    int flags = 0;                   // 1 color mux, 2 alpha mux
    float c0[4] = {0, 0, 0, 0}, c1[4] = {0, 0, 0, 0};   // constant colours 0 and 1 (r, g, b, a)
    int in[4] = {0, 0, 0, 0}, map[4] = {0, 0, 0, 0}, ab = 0, abf = 0, cd = 0, cdf = 0, sum = 0, out_map = 0;
    int ina[4] = {0, 0, 0, 0}, mapa[4] = {0, 0, 0, 0}, aba = 0, cda = 0, suma = 0, out_mapa = 0;
};
struct ShaderI {
    std::string tag;                 // with extension
    bool readable = false, chicago = false, generic = false;
    std::vector<GStage> stages;      // (generic)
    int blend = 0, flags = 0, first_type = 0, fade = 0, layers = 0, passes = 1;
    bool lens_flare = false;
    std::vector<MapI> maps;
    bool two_sided() const { return flags & 4; }
};

struct Context {
    const Find &find;
    std::map<std::string, std::unique_ptr<Image>> images;
    std::map<std::string, ShaderI> shaders;
    explicit Context(const Find &f) : find(f) {}

    const Image *image(const std::string &bitmap) {
        auto it = images.find(bitmap);
        if (it != images.end()) return it->second.get();
        std::unique_ptr<Image> im(new Image);
        fs::path f = bitmap.empty() ? fs::path() : find(bitmap);
        if (f.empty() || !load_image(f, *im)) im.reset();
        return (images[bitmap] = std::move(im)).get();
    }

    // A model's shader reference (any shader kind: the extension tried).
    const ShaderI &shader(const std::string &ref) {
        auto it = shaders.find(ref);
        if (it != shaders.end()) return it->second;
        ShaderI s;
        std::string base = ref, ext = fs::path(ref).extension().u8string();
        fs::path f;
        if (ext.rfind(".shader_", 0) == 0) f = find(ref);
        else {
            if (!ext.empty()) base = ref.substr(0, ref.size() - ext.size());
            for (const char *x : {".shader_transparent_chicago", ".shader_transparent_chicago_extended", ".shader_transparent_generic",
                                  ".shader_model", ".shader_environment", ".shader_transparent_glass", ".shader_transparent_meter",
                                  ".shader_transparent_plasma", ".shader_transparent_water"})
                if (!(f = find(base + x)).empty()) { ext = x; break; }
        }
        s.tag = base + ext;
        tagfile::Tag t;
        if (!f.empty() && t.load(f)) {
            s.readable = true;
            tagfile::Elem r = t.root();
            if (ext == ".shader_transparent_chicago" || ext == ".shader_transparent_chicago_extended") {
                s.chicago = true;
                s.blend = (int)r.integer("framebuffer blend function");
                s.flags = (int)r.integer("shader transparent chicago flags");
                s.first_type = (int)r.integer("first map type");
                s.fade = (int)r.integer("framebuffer fade mode");
                s.layers = (int)r.block("extra layers").size();
                s.lens_flare = !r.ref("lens flare").empty();
                tagfile::Block maps = r.block(ext == ".shader_transparent_chicago" ? "maps" : "maps 4 stage");
                if (!maps.size() && ext != ".shader_transparent_chicago") maps = r.block("maps 2 stage");
                for (size_t i = 0; i < maps.size() && i < 4; i++) s.maps.push_back(read_map(maps[i]));
            } else if (ext == ".shader_transparent_generic") {
                s.generic = true;
                s.blend = (int)r.integer("framebuffer blend function");
                s.flags = (int)r.integer("shader transparent generic flags");
                s.first_type = (int)r.integer("first map type");
                s.layers = (int)r.block("extra layers").size();
                tagfile::Block maps = r.block("maps");
                for (size_t i = 0; i < maps.size() && i < 4; i++) {
                    MapI m = read_map(maps[i]);
                    int f = (int)maps[i].integer("flags");   // (1 unfiltered, 2 u clamped, 4 v clamped)
                    m.flags = (f & 1) | ((f & 2) ? 4 : 0) | ((f & 4) ? 8 : 0);
                    s.maps.push_back(m);
                }
                tagfile::Block st = r.block("stages");
                for (size_t i = 0; i < st.size() && i < 7; i++) {
                    const tagfile::Elem &e = st[i];
                    GStage g;
                    g.flags = (int)e.integer("flags");
                    for (int k = 0; k < 4; k++) {   // (ARGB: alpha first)
                        int c = k == 3 ? 0 : k + 1;
                        g.c0[k] = (float)((e.num("color0 animation lower bound", c) + e.num("color0 animation upper bound", c)) / 2);
                        g.c1[k] = (float)e.num("color1", c);
                    }
                    const char *L[4] = {"a", "b", "c", "d"};
                    for (int k = 0; k < 4; k++) {
                        std::string x = std::string("input ") + L[k];
                        g.in[k] = (int)e.integer(x.c_str());
                        g.map[k] = (int)e.integer((x + " mapping").c_str());
                        g.ina[k] = (int)e.integer((x + " alpha").c_str());
                        g.mapa[k] = (int)e.integer((x + " mapping alpha").c_str());
                    }
                    g.ab = (int)e.integer("output ab");
                    g.abf = (int)e.integer("output ab function");
                    g.cd = (int)e.integer("output bc");   // (Invader's name for the cd output)
                    g.cdf = (int)e.integer("output cd function");
                    g.sum = (int)e.integer("output ab cd mux sum");
                    g.out_map = (int)e.integer("output mapping color");
                    g.aba = (int)e.integer("output ab alpha");
                    g.cda = (int)e.integer("output cd alpha");
                    g.suma = (int)e.integer("output ab cd mux sum alpha");
                    g.out_mapa = (int)e.integer("output mapping alpha");
                    s.stages.push_back(g);
                }
            } else {
                s.blend = -1;
            }
            s.passes = 1 + s.layers;
        }
        return shaders[ref] = s;
    }

    MapI read_map(const tagfile::Elem &m) {
        MapI o;
        o.bitmap = m.ref("map");
        o.cf = (int)m.integer("color function");
        o.af = (int)m.integer("alpha function");
        o.flags = (int)m.integer("flags");
        // Zeros are filled in when the map is built (Invader: shader.cpp default_maps, the tag definitions' defaults):
        // a scale of 0 takes the other one's (both 0: 1); animation periods and scales 1, the rotation's scale 360.
        double us = m.num("map u scale"), vs = m.num("map v scale");
        if (us == 0 && vs == 0) us = vs = 1;
        else if (us == 0) us = vs;
        else if (vs == 0) vs = us;
        o.us = us;
        o.vs = vs;
        o.uo = m.num("map u offset");
        o.vo = m.num("map v offset");
        // An animation moves the texture unless it's constant: function "zero" (none), or "one" (a fixed offset, its
        // scale: so by default 1 in u and v, a whole turn) reading no object function (periodic functions: one, zero,
        // cosine, ...).
        const char *axes[3] = {"u", "v", "rotation"};
        double fixed[3] = {0, 0, 0};
        for (int a = 0; a < 3; a++) {
            std::string ax = axes[a];
            double scale = m.num((ax + " animation scale").c_str());
            if (scale == 0) scale = a == 2 ? 360 : 1;
            int fn = (int)m.integer((ax + " animation function").c_str());
            if (fn == 1) continue;
            if (m.integer((ax + " animation source").c_str()) != 0 || fn != 0) o.moving = true;
            else fixed[a] = scale;
        }
        o.uo += fixed[0];
        o.vo += fixed[1];
        // (Turned: taken as moving, not combined. A whole turn is none.)
        double turn = std::fmod(m.num("map rotation") + fixed[2], 360.0);
        if (std::fabs(turn) > 1e-4 && std::fabs(std::fabs(turn) - 360) > 1e-4) o.moving = true;
        o.img = image(o.bitmap);
        return o;
    }
};

// ---- The chicago combiners (shader_transparent_chicago_preprocessor.c): stage 0 loads map 0; stage s (map s-1's
// functions) combines the result so far (R0) with map s (T[s]); the result is R0's colour and alpha.

const uint32_t kRgb[2][13] = {
    {0x0c200000, 0x08200000, 0x080c0000, 0x080c080c, 0x08200c20, 0x08204c20, 0x0c204820, 0x08200c40, 0x0c200840, 0x081c0c3c,
     0x0c1c083c, 0x08180c38, 0x0c180838},
    {0x0c200000, 0x18200000, 0x180c0000, 0x180c180c, 0x18200c20, 0x18204c20, 0x0c205820, 0x18200c40, 0x0c201840, 0x181c0c3c,
     0x0c1c183c, 0x18180c38, 0x0c181838}};
const uint32_t kAlpha[13] = {0x1c200000, 0x18200000, 0x181c0000, 0x181c181c, 0x18201c20, 0x18205c20, 0x1c205820,
                             0x18201c40, 0x1c201840, 0x181c1c3c, 0x1c1c183c, 0x18181c38, 0x1c181838};
const uint32_t kInc[13] = {0x00000000, 0x01000000, 0x01000000, 0x01000100, 0x01000000, 0x01000000, 0x00000100,
                           0x01000000, 0x00000100, 0x01000000, 0x00000100, 0x01010001, 0x00010101};

bool functions_ok(const ShaderI &s) {
    for (size_t i = 0; i + 1 < s.maps.size(); i++)
        if (s.maps[i].cf < 0 || s.maps[i].cf > 12 || s.maps[i].af < 0 || s.maps[i].af > 12) return false;
    return true;
}

void stage_words(const std::vector<MapI> &maps, size_t s, uint32_t &rgb, uint32_t &alpha) {
    if (s == 0) { rgb = 0x08200000; alpha = 0x18200000; return; }
    const MapI &m = maps[s - 1];
    rgb = kRgb[(m.flags & 2) ? 1 : 0][m.cf] + kInc[m.cf] * (uint32_t)s;
    alpha = kAlpha[m.af] + kInc[m.af] * (uint32_t)s;
}

float mapping(float x, int map) {
    switch (map) {
    case 0: return std::max(0.f, x);                        // unsigned identity
    case 1: return 1 - std::min(1.f, std::max(0.f, x));     // unsigned invert
    case 2: return 2 * std::max(0.f, x) - 1;                // expand normal
    case 3: return 1 - 2 * std::max(0.f, x);                // expand negate
    case 4: return std::max(0.f, x) - 0.5f;                 // half bias normal
    case 5: return 0.5f - std::max(0.f, x);                 // half bias negate
    case 6: return x;                                       // signed identity
    default: return -x;                                     // signed negate
    }
}

// One input of a stage: register (zero, T0..T3, R0), channel, mapping. `c`: 0..2 a colour component, 3 the alpha
// portion (whose channel bit off means blue).
float input(uint8_t code, int c, const V4 &R0, const V4 *T) {
    int reg = code & 15;
    bool alpha = code & 0x10;
    const V4 *src = reg >= 8 && reg <= 11 ? &T[reg - 8] : reg == 12 ? &R0 : nullptr;
    float x = 0;
    if (src) x = alpha ? src->v[3] : src->v[c == 3 ? 2 : c];
    return mapping(x, code >> 5);
}

V4 chicago_eval(const std::vector<MapI> &maps, const V4 *T) {
    V4 R0;
    for (size_t s = 0; s < maps.size(); s++) {
        uint32_t rw, aw;
        stage_words(maps, s, rw, aw);
        V4 n;
        for (int c = 0; c < 4; c++) {
            uint32_t w = c == 3 ? aw : rw;
            float v = input((uint8_t)(w >> 24), c, R0, T) * input((uint8_t)(w >> 16), c, R0, T) +
                      input((uint8_t)(w >> 8), c, R0, T) * input((uint8_t)w, c, R0, T);
            n.v[c] = std::min(1.f, std::max(-1.f, v));
        }
        R0 = n;
    }
    for (int c = 0; c < 4; c++) R0.v[c] = std::min(1.f, std::max(0.f, R0.v[c]));   // the final combiner
    return R0;
}

// A shader_transparent_generic's colour (shader_transparent_generic: register combiners). Registers: 0 discard,
// 1 scratch 0 (the result), 2 scratch 1, 3 and 4 vertex colours 0 and 1 (white: a sky is unlit), 5 to 8 maps 0 to 3.
// No stages: map 0 as it is.
V4 generic_eval(const ShaderI &s, const V4 *T) {
    V4 R[9];
    for (int c = 0; c < 4; c++) { R[3].v[c] = R[4].v[c] = 1; }
    for (size_t k = 0; k < s.maps.size() && k < 4; k++) R[5 + k] = T[k];
    if (s.stages.empty()) return s.maps.empty() ? R[3] : R[5];
    R[1].v[3] = R[5].v[3];   // (scratch alpha 0 starts as map 0's alpha)
    auto clamp1 = [](float x) { return std::min(1.f, std::max(-1.f, x)); };
    for (const GStage &g : s.stages) {
        // colour inputs: 0 zero, 1 one, 2 half, 3 -1, 4 -half, 5-8 map colour, 9-10 vertex colour, 11-12 scratch colour,
        // 13-14 constant colour, 15-18 map alpha, 19-20 vertex alpha, 21-22 scratch alpha, 23-24 constant alpha
        auto reg_c = [&](int i, int c) -> float {
            const float consts[5] = {0, 1, 0.5f, -1, -0.5f};
            if (i < 5) return consts[i];
            bool alpha = i >= 15;
            int j = alpha ? i - 10 : i;   // (to the colour inputs' numbering)
            int ch = alpha ? 3 : c;
            if (j <= 8) return R[j].v[ch];
            if (j <= 10) return R[j - 6].v[ch];
            if (j <= 12) return R[j - 10].v[ch];
            return (j == 13 ? g.c0 : g.c1)[ch];
        };
        // alpha inputs: 0-4 constants, 5-8 map alpha, 9-10 vertex alpha, 11-12 scratch alpha, 13-14 constant alpha,
        // 15-24 the same, blue
        auto reg_a = [&](int i) -> float {
            const float consts[5] = {0, 1, 0.5f, -1, -0.5f};
            if (i < 5) return consts[i];
            bool blue = i >= 15;
            int j = blue ? i - 10 : i, ch = blue ? 2 : 3;
            if (j <= 8) return R[j].v[ch];
            if (j <= 10) return R[j - 6].v[ch];
            if (j <= 12) return R[j - 10].v[ch];
            return (j == 13 ? g.c0 : g.c1)[ch];
        };
        auto out_map = [](float x, int m) {   // identity, x 1/2, x 2, x 4, bias -1/2, expand (bias -1/2, x 2)
            switch (m) {
            case 1: return x * 0.5f;
            case 2: return x * 2;
            case 3: return x * 4;
            case 4: return x - 0.5f;
            case 5: return (x - 0.5f) * 2;
            default: return x;
            }
        };
        float A[3][4], B[3][4], C[3][4], D[3][4];
        for (int c = 0; c < 3; c++) {
            A[c][0] = mapping(reg_c(g.in[0], c), g.map[0]); B[c][0] = mapping(reg_c(g.in[1], c), g.map[1]);
            C[c][0] = mapping(reg_c(g.in[2], c), g.map[2]); D[c][0] = mapping(reg_c(g.in[3], c), g.map[3]);
        }
        float ab[3], cd[3], sum[3];
        float dab = A[0][0] * B[0][0] + A[1][0] * B[1][0] + A[2][0] * B[2][0], dcd = C[0][0] * D[0][0] + C[1][0] * D[1][0] + C[2][0] * D[2][0];
        bool mux = (g.flags & 1) && R[1].v[3] >= 0.5f;
        for (int c = 0; c < 3; c++) {
            ab[c] = out_map(g.abf ? dab : A[c][0] * B[c][0], g.out_map);
            cd[c] = out_map(g.cdf ? dcd : C[c][0] * D[c][0], g.out_map);
            float a = g.abf ? dab : A[c][0] * B[c][0], b = g.cdf ? dcd : C[c][0] * D[c][0];
            sum[c] = out_map((g.flags & 1) ? (mux ? b : a) : a + b, g.out_map);
        }
        float aa = mapping(reg_a(g.ina[0]), g.mapa[0]) * mapping(reg_a(g.ina[1]), g.mapa[1]);
        float ac = mapping(reg_a(g.ina[2]), g.mapa[2]) * mapping(reg_a(g.ina[3]), g.mapa[3]);
        bool muxa = (g.flags & 2) && R[1].v[3] >= 0.5f;
        float sa = (g.flags & 2) ? (muxa ? ac : aa) : aa + ac;
        // (written after all are read: registers 1-8)
        V4 N[9];
        for (int k = 0; k < 9; k++) N[k] = R[k];
        for (int c = 0; c < 3; c++) {
            if (g.ab > 0) N[g.ab].v[c] = clamp1(ab[c]);
            if (g.cd > 0) N[g.cd].v[c] = clamp1(cd[c]);
            if (g.sum > 0) N[g.sum].v[c] = clamp1(sum[c]);
        }
        if (g.aba > 0) N[g.aba].v[3] = clamp1(out_map(aa, g.out_mapa));
        if (g.cda > 0) N[g.cda].v[3] = clamp1(out_map(ac, g.out_mapa));
        if (g.suma > 0) N[g.suma].v[3] = clamp1(out_map(sa, g.out_mapa));
        for (int k = 0; k < 9; k++) R[k] = N[k];
    }
    V4 o = R[1];
    for (int c = 0; c < 4; c++) o.v[c] = std::min(1.f, std::max(0.f, o.v[c]));
    return o;
}

// The same with each value a range: the output's range over all inputs in theirs.
struct I4 { Iv v[4]; };
Iv input_iv(uint8_t code, int c, const I4 &R0, const I4 *T) {
    int reg = code & 15;
    bool alpha = code & 0x10;
    const I4 *src = reg >= 8 && reg <= 11 ? &T[reg - 8] : reg == 12 ? &R0 : nullptr;
    Iv x{0, 0};
    if (src) x = alpha ? src->v[3] : src->v[c == 3 ? 2 : c];
    float a = mapping(x.lo, code >> 5), b = mapping(x.hi, code >> 5);   // (each mapping is monotonic)
    return {std::min(a, b), std::max(a, b)};
}
Iv mul(Iv a, Iv b) {
    float p[4] = {a.lo * b.lo, a.lo * b.hi, a.hi * b.lo, a.hi * b.hi};
    return {*std::min_element(p, p + 4), *std::max_element(p, p + 4)};
}
I4 chicago_range(const std::vector<MapI> &maps, const I4 *T) {
    I4 R0;
    for (size_t s = 0; s < maps.size(); s++) {
        uint32_t rw, aw;
        stage_words(maps, s, rw, aw);
        I4 n;
        for (int c = 0; c < 4; c++) {
            uint32_t w = c == 3 ? aw : rw;
            Iv ab = mul(input_iv((uint8_t)(w >> 24), c, R0, T), input_iv((uint8_t)(w >> 16), c, R0, T));
            Iv cd = mul(input_iv((uint8_t)(w >> 8), c, R0, T), input_iv((uint8_t)w, c, R0, T));
            n.v[c] = {std::min(1.f, std::max(-1.f, ab.lo + cd.lo)), std::min(1.f, std::max(-1.f, ab.hi + cd.hi))};
        }
        R0 = n;
    }
    for (int c = 0; c < 4; c++) R0.v[c] = {std::min(1.f, std::max(0.f, R0.v[c].lo)), std::min(1.f, std::max(0.f, R0.v[c].hi))};
    return R0;
}

// Whether a layer whose output lies in `o` can change the framebuffer by a visible step, by its blend function
// (rasterizer_xbox.c: alpha blend, multiply, double multiply, add, subtract, min, max, alpha-multiply add).
bool can_show(const I4 &o, int blend) {
    const float step = 1.f / 255;
    float rgb_hi = std::max({o.v[0].hi, o.v[1].hi, o.v[2].hi}), rgb_lo = std::min({o.v[0].lo, o.v[1].lo, o.v[2].lo});
    switch (blend) {
    case 0: return o.v[3].hi >= step;
    case 1: return rgb_lo <= 1 - step;
    case 2: return rgb_lo <= 0.5f - step / 2 || rgb_hi >= 0.5f + step / 2;
    case 3: case 4: case 6: return rgb_hi >= step;
    case 7: return rgb_hi >= step || o.v[3].hi >= step;
    default: return true;
    }
}

// ---- The sky's model

struct Vtx { double p[3] = {0, 0, 0}, uv[2] = {0, 0}, n[3] = {0, 0, 0}; int src = -1; };   // src: the tag vertex it is (-1: new)
struct Part {
    int region = 0, geometry = 0, part = 0, shader = -1;
    bool one_node = true;               // all its vertices on the same node (what can be cut up and rewritten)
    std::vector<Vtx> v;                 // uv: as the tag has them (the model's base map scale not applied)
    std::vector<uint32_t> tris;         // a list, in Halo's winding
};

double get(const TagNode &n, const char *f, int k = 0) { return tagfile::get_num(n, f, k); }

unsigned u16(const std::string &b, size_t o) { return (unsigned)(uint8_t)b[o] << 8 | (uint8_t)b[o + 1]; }
void put16(std::string &b, size_t o, unsigned v) { b[o] = (char)(v >> 8); b[o + 1] = (char)v; }

// A part's strip as a list (even: a b c; odd: b a c, as Direct3D's strips keep facing).
std::vector<uint32_t> strip_to_list(const std::vector<TagNode> &tris, size_t vertices) {
    std::vector<uint32_t> s, out;
    for (auto &t : tris)
        for (int k = 0; k < 3; k++) {
            unsigned x = u16(t.fixed, k * 2);
            if (x != 0xFFFF) s.push_back(x);
        }
    for (size_t i = 2; i < s.size(); i++) {
        uint32_t a = s[i - 2], b = s[i - 1], c = s[i];
        if (a == b || b == c || a == c || a >= vertices || b >= vertices || c >= vertices) continue;
        if ((i - 2) % 2 == 0) out.insert(out.end(), {a, b, c});
        else out.insert(out.end(), {b, a, c});
    }
    return out;
}
std::vector<TagNode> list_to_strip(const std::vector<uint32_t> &list, size_t vertices, int sid) {
    std::vector<uint32_t> strip(meshopt_stripifyBound(list.size()));
    strip.resize(meshopt_stripify(strip.data(), list.data(), list.size(), vertices, 0));
    while (strip.size() % 3) strip.push_back(0xFFFF);
    std::vector<TagNode> out;
    for (size_t i = 0; i < strip.size(); i += 3) {
        TagNode t = tagfile::new_node(sid);
        for (int k = 0; k < 3; k++) put16(t.fixed, k * 2, strip[i + k]);
        out.push_back(std::move(t));
    }
    return out;
}

// The geometry each region draws for the sky: its first permutation, at the detail the engine picks for a model 0
// pixels across (models.c), which for a sky (all distances 0) is its most detailed.
int sky_level(const TagNode &root) {
    static const char *cut[5] = {"super high detail cutoff", "high detail cutoff", "medium detail cutoff", "low detail cutoff",
                                 "super low detail cutoff"};
    int k = 4;
    while (k > 0 && 0 < get(root, cut[k])) k--;
    return k;
}
const char *kLevelField[5] = {"super low", "low", "medium", "high", "super high"};

struct Model {
    tagfile::EditTag tag;
    double bu = 1, bv = 1;              // base map scale
    std::vector<Part> parts;            // in the order they're drawn
    std::vector<std::string> shader_refs;
    bool ok = false;
};

// The parts and shader references, from the model tag as it is (read again after it's edited).
bool parse_model(Model &m) {
    m.parts.clear();
    m.shader_refs.clear();
    m.ok = false;
    TagNode &r = m.tag.root();
    m.bu = get(r, "base map u scale");
    m.bv = get(r, "base map v scale");
    if (m.bu == 0) m.bu = 1;
    if (m.bv == 0) m.bv = 1;
    auto *shaders = tagfile::block(r, "shaders");
    auto *regions = tagfile::block(r, "regions");
    auto *geos = tagfile::block(r, "geometries");
    if (!shaders || !regions || !geos) return false;
    for (auto &s : *shaders) {
        std::string *p = tagfile::ref_path(s, "shader");
        std::string path = p ? *p : "";
        std::replace(path.begin(), path.end(), '\\', '/');
        m.shader_refs.push_back(path);
    }
    int level = sky_level(r);
    for (size_t ri = 0; ri < regions->size(); ri++) {
        auto *perms = tagfile::block((*regions)[ri], "permutations");
        if (!perms || perms->empty()) continue;
        int g = (int)(int16_t)get((*perms)[0], kLevelField[level]);
        if (g < 0 || g >= (int)geos->size()) continue;
        auto *parts = tagfile::block((*geos)[(size_t)g], "parts");
        if (!parts) continue;
        for (size_t pi = 0; pi < parts->size(); pi++) {
            TagNode &pn = (*parts)[pi];
            Part p;
            p.region = (int)ri;
            p.geometry = g;
            p.part = (int)pi;
            p.shader = (int)(int16_t)get(pn, "shader index");
            auto *V = tagfile::block(pn, "uncompressed vertices");
            auto *T = tagfile::block(pn, "triangles");
            if (!V || !T) continue;
            for (auto &vn : *V) {
                Vtx v;
                for (int k = 0; k < 3; k++) { v.p[k] = get(vn, "position", k); v.n[k] = get(vn, "normal", k); }
                v.uv[0] = get(vn, "texture coords", 0);
                v.uv[1] = get(vn, "texture coords", 1);
                v.src = (int)p.v.size();
                p.one_node = p.one_node && get(vn, "node0 index") == get((*V)[0], "node0 index") &&
                             get(vn, "node1 index") == get((*V)[0], "node1 index") && get(vn, "node0 weight") == get((*V)[0], "node0 weight");
                p.v.push_back(v);
            }
            p.tris = strip_to_list(*T, p.v.size());
            m.parts.push_back(std::move(p));
        }
    }
    m.ok = true;
    return true;
}
bool read_model(const fs::path &file, Model &m) { return m.tag.load(file) && parse_model(m); }

// Which winding faces the sky's middle (where the camera is): the one most of the triangles' area has.
bool inward_winding(const std::vector<Part> &parts) {
    double in = 0, out = 0;
    for (auto &p : parts)
        for (size_t t = 0; t + 2 < p.tris.size(); t += 3) {
            const double *A = p.v[p.tris[t]].p, *B = p.v[p.tris[t + 1]].p, *C = p.v[p.tris[t + 2]].p;
            double e1[3] = {B[0] - A[0], B[1] - A[1], B[2] - A[2]}, e2[3] = {C[0] - A[0], C[1] - A[1], C[2] - A[2]};
            double n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            double d = n[0] * A[0] + n[1] * A[1] + n[2] * A[2];
            (d < 0 ? in : out) += std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        }
    return in >= out;
}
bool faces_in(const double *A, const double *B, const double *C) {
    double e1[3] = {B[0] - A[0], B[1] - A[1], B[2] - A[2]}, e2[3] = {C[0] - A[0], C[1] - A[1], C[2] - A[2]};
    double n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    return n[0] * A[0] + n[1] * A[1] + n[2] * A[2] < 0;
}

// ---- Fill: layers drawn per pixel from the sky's middle, in 320x240 views 70 degrees across (the Xbox's), yaw every 30
// degrees at pitch -15, 0, 15 and 35.

const int kW = 320, kH = 240;
struct View { double fw[3], lf[3], up[3]; };
const std::vector<View> &views() {
    static std::vector<View> v;
    if (!v.empty()) return v;
    const int pitches[4] = {-15, 0, 15, 35};
    for (int pi = 0; pi < 4; pi++)
        for (int yi = 0; yi < 12; yi++) {
            double yaw = yi * 30 * kPi / 180, pitch = pitches[pi] * kPi / 180;
            View w;
            double fw[3] = {std::cos(yaw) * std::cos(pitch), std::sin(yaw) * std::cos(pitch), std::sin(pitch)};
            double lf[3] = {-std::sin(yaw), std::cos(yaw), 0};
            std::copy(fw, fw + 3, w.fw);
            std::copy(lf, lf + 3, w.lf);
            w.up[0] = fw[1] * lf[2] - fw[2] * lf[1];
            w.up[1] = fw[2] * lf[0] - fw[0] * lf[2];
            w.up[2] = fw[0] * lf[1] - fw[1] * lf[0];
            v.push_back(w);
        }
    return v;
}

// A part's triangles in a view: `passes` added to each pixel they cover.
void draw_layers(const std::vector<Vtx> &v, const std::vector<uint32_t> &tris, float passes, bool two_sided, bool inward, const View &w,
                 std::vector<float> &layers) {
    const double tx = std::tan(35.0 * kPi / 180), ty = tx * kH / kW;
    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
        const double *P3[3] = {v[tris[t]].p, v[tris[t + 1]].p, v[tris[t + 2]].p};
        if (!two_sided && faces_in(P3[0], P3[1], P3[2]) != inward) continue;
        std::vector<std::array<double, 3>> poly, cp;
        double scale = 0;
        for (int k = 0; k < 3; k++) {
            const double *q = P3[k];
            poly.push_back({-(q[0] * w.lf[0] + q[1] * w.lf[1] + q[2] * w.lf[2]), q[0] * w.up[0] + q[1] * w.up[1] + q[2] * w.up[2],
                            q[0] * w.fw[0] + q[1] * w.fw[1] + q[2] * w.fw[2]});
            scale = std::max(scale, std::fabs(q[0]) + std::fabs(q[1]) + std::fabs(q[2]));
        }
        double nz = scale * 1e-5;
        for (size_t i = 0; i < 3; i++) {
            auto &a = poly[i], &b = poly[(i + 1) % 3];
            bool ia = a[2] > nz, ib = b[2] > nz;
            if (ia) cp.push_back(a);
            if (ia != ib) {
                double k = (nz - a[2]) / (b[2] - a[2]);
                cp.push_back({a[0] + k * (b[0] - a[0]), a[1] + k * (b[1] - a[1]), nz});
            }
        }
        if (cp.size() < 3) continue;
        std::vector<std::array<double, 2>> sp;
        for (auto &c : cp) sp.push_back({(c[0] / c[2] / tx * 0.5 + 0.5) * kW, (0.5 - c[1] / c[2] / ty * 0.5) * kH});
        for (size_t k = 1; k + 1 < sp.size(); k++) {
            auto &A = sp[0], &B = sp[k], &C = sp[k + 1];
            double area = (B[0] - A[0]) * (C[1] - A[1]) - (B[1] - A[1]) * (C[0] - A[0]);
            if (std::fabs(area) < 1e-12) continue;
            int x0 = std::max(0, (int)std::floor(std::min({A[0], B[0], C[0]})));
            int x1 = std::min(kW - 1, (int)std::ceil(std::max({A[0], B[0], C[0]})));
            int y0 = std::max(0, (int)std::floor(std::min({A[1], B[1], C[1]})));
            int y1 = std::min(kH - 1, (int)std::ceil(std::max({A[1], B[1], C[1]})));
            for (int y = y0; y <= y1; y++)
                for (int x = x0; x <= x1; x++) {
                    double px = x + 0.5, py = y + 0.5;
                    double w0 = (B[0] - px) * (C[1] - py) - (B[1] - py) * (C[0] - px);
                    double w1 = (C[0] - px) * (A[1] - py) - (C[1] - py) * (A[0] - px);
                    double w2 = (A[0] - px) * (B[1] - py) - (A[1] - py) * (B[0] - px);
                    bool in = area > 0 ? (w0 >= 0 && w1 >= 0 && w2 >= 0) : (w0 <= 0 && w1 <= 0 && w2 <= 0);
                    if (in) layers[(size_t)y * kW + x] += passes;
                }
        }
    }
}

Fill fill_of(const Model &m, Context &cx) {
    Fill f;
    std::vector<float> layers((size_t)kW * kH);
    bool inward = inward_winding(m.parts);
    double sum = 0;
    for (auto &p : m.parts) {
        f.parts++;
        f.triangles += (int)(p.tris.size() / 3);
    }
    for (const View &w : views()) {
        std::fill(layers.begin(), layers.end(), 0.f);
        for (auto &p : m.parts) {
            if (p.shader < 0 || p.shader >= (int)m.shader_refs.size()) continue;
            const ShaderI &s = cx.shader(m.shader_refs[(size_t)p.shader]);
            draw_layers(p.v, p.tris, (float)s.passes, s.two_sided(), inward, w, layers);
        }
        double v = 0;
        for (float l : layers) v += l;
        v /= (double)kW * kH;
        sum += v;
        f.worst = std::max(f.worst, v);
    }
    f.layers = sum / (double)views().size();
    f.ok = true;
    return f;
}

// What one part's triangles add to the layers per pixel (averaged over the views).
double part_fill(const std::vector<Vtx> &v, const std::vector<uint32_t> &tris, bool two_sided, bool inward) {
    std::vector<float> layers((size_t)kW * kH);
    double sum = 0;
    for (const View &w : views()) {
        std::fill(layers.begin(), layers.end(), 0.f);
        draw_layers(v, tris, 1, two_sided, inward, w, layers);
        for (float l : layers) sum += l;
    }
    return sum / ((double)kW * kH * (double)views().size());
}

// ---- Layers drawn in one pass

// Whether two parts are the same triangles seen from the sky's middle: the same texture coordinates in the same
// triangles, each vertex in the same direction (within a tenth of a degree: under a pixel of the Xbox's views; e.g.
// Precipice's two cloud layers, one a copy of the other made 0.9 the size).
bool same_dome(const Part &a, const Part &b) {
    if (a.v.size() != b.v.size() || a.tris != b.tris || a.v.empty()) return false;
    const double most = std::cos(0.1 * kPi / 180);
    for (size_t i = 0; i < a.v.size(); i++) {
        double d = 0, la = 0, lb = 0;
        for (int k = 0; k < 3; k++) {
            d += a.v[i].p[k] * b.v[i].p[k];
            la += a.v[i].p[k] * a.v[i].p[k];
            lb += b.v[i].p[k] * b.v[i].p[k];
        }
        if (la <= 0 || lb <= 0 || d / std::sqrt(la * lb) < most) return false;
        if (std::fabs(a.v[i].uv[0] - b.v[i].uv[0]) > 1e-6 || std::fabs(a.v[i].uv[1] - b.v[i].uv[1]) > 1e-6) return false;
    }
    return true;
}

// A layer that can be drawn in another's pass: transparent chicago, alpha blended, not cut out, no extra layers, a
// flat first map, no fading, no lens flare, nothing drawn in screen space or scaled by distance or numeric.
bool plain_layer(const ShaderI &s) {
    return s.readable && s.chicago && s.blend == 0 && !(s.flags & (1 | 2 | 8 | 64 | 128)) && s.first_type == 0 && s.fade == 0 &&
           s.layers == 0 && !s.lens_flare && !s.maps.empty() && functions_ok(s);
}

bool still(const MapI &m) { return !m.moving && m.img; }

// A new A8R8G8B8 bitmap tag (all its mip levels), made from another bitmap tag (`like`): `rgba` top row first.
bool write_bitmap(const fs::path &like, const fs::path &out, int w, int h, const std::vector<float> &rgba) {
    tagfile::EditTag t;
    if (!t.load(like)) return false;
    TagNode &r = t.root();
    std::string pix;
    std::vector<float> lv = rgba;
    int cw = w, ch = h, mips = 0;
    for (;;) {
        for (size_t i = 0; i < (size_t)cw * ch; i++) {
            auto q = [&](int c) { return (char)(uint8_t)std::lround(std::min(1.f, std::max(0.f, lv[i * 4 + c])) * 255); };
            pix += q(2);
            pix += q(1);
            pix += q(0);
            pix += q(3);
        }
        if (cw == 1 && ch == 1) break;
        int nw = std::max(1, cw / 2), nh = std::max(1, ch / 2);
        std::vector<float> n((size_t)nw * nh * 4);
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++)
                for (int c = 0; c < 4; c++) {
                    float s = 0;
                    int k = 0;
                    for (int dy = 0; dy < 2; dy++)
                        for (int dx = 0; dx < 2; dx++, k++)
                            s += lv[((size_t)std::min(ch - 1, y * 2 + dy) * cw + std::min(cw - 1, x * 2 + dx)) * 4 + c];
                    n[((size_t)y * nw + x) * 4 + c] = s / k;
                }
        lv.swap(n);
        cw = nw;
        ch = nh;
        mips++;
    }
    tagfile::set_num(r, "type", 0);
    tagfile::set_num(r, "encoding format", 4);   // 32-bit colour (the porter compresses it like the map's others)
    tagfile::set_num(r, "color plate width", 0);
    tagfile::set_num(r, "color plate height", 0);
    if (std::string *cp = tagfile::data(r, "compressed color plate data")) cp->clear();
    std::string *pp = tagfile::data(r, "processed pixel data");
    auto *bd = tagfile::block(r, "bitmap data");
    auto *seq = tagfile::block(r, "bitmap group sequence");
    if (!pp || !bd || bd->empty()) return false;
    *pp = pix;
    bd->resize(1);
    TagNode &e = (*bd)[0];
    tagfile::set_num(e, "width", w);
    tagfile::set_num(e, "height", h);
    tagfile::set_num(e, "depth", 1);
    tagfile::set_num(e, "type", 0);
    tagfile::set_num(e, "format", 11);
    tagfile::set_num(e, "flags", 1);   // power-of-two sides
    tagfile::set_num(e, "registration point", w / 2, 0);
    tagfile::set_num(e, "registration point", h / 2, 1);
    tagfile::set_num(e, "mipmap count", mips);
    tagfile::set_num(e, "pixel data offset", 0);
    tagfile::set_num(e, "pixel data size", (double)pix.size());
    if (seq && seq->size() != 1) seq->clear();
    if (seq && seq->size() == 1) {
        tagfile::set_num((*seq)[0], "first bitmap index", 0);
        tagfile::set_num((*seq)[0], "bitmap count", 1);
        if (auto *sprites = tagfile::block((*seq)[0], "sprites")) sprites->clear();
    }
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    return t.save(out);
}

int pow2_at_least(double v) {
    int p = 8;
    while (p < v && p < 4096) p *= 2;
    return p;
}

std::string stem(const std::string &tag) { return fs::path(tag).stem().u8string(); }
std::string dir_of(const std::string &tag) {
    size_t s = tag.rfind('/');
    return s == std::string::npos ? "" : tag.substr(0, s + 1);
}
std::string no_ext(const std::string &tag) {
    std::string e = fs::path(tag).extension().u8string();
    return e.empty() ? tag : tag.substr(0, tag.size() - e.size());
}

double angle_between(const double *a, const double *b) {
    double la = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), lb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    double d = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / std::max(1e-30, la * lb);
    return std::acos(std::min(1.0, std::max(-1.0, d)));
}

// ---- A part cut into an n x n grid of triangles on each of its own (on the same planes: the same picture), the new
// vertices along an edge shared by the triangles on both sides.

struct Grid {
    int n = 1;
    std::vector<Vtx> v;                         // the part's vertices first, then the new ones
    std::vector<std::array<uint32_t, 3>> sub;   // each triangle's n * n, in the part's order (grid_cell's numbering)
};

Grid subdivide(const Part &p, int n) {
    Grid g;
    g.n = n;
    g.v = p.v;
    std::map<std::array<uint32_t, 3>, uint32_t> edge;   // (low vertex, high vertex, steps from low) -> vertex
    auto lerp = [](const Vtx &a, const Vtx &b, double f) {
        Vtx o;
        for (int k = 0; k < 3; k++) { o.p[k] = a.p[k] + (b.p[k] - a.p[k]) * f; o.n[k] = a.n[k] + (b.n[k] - a.n[k]) * f; }
        for (int k = 0; k < 2; k++) o.uv[k] = a.uv[k] + (b.uv[k] - a.uv[k]) * f;
        return o;
    };
    std::vector<uint32_t> grid((size_t)(n + 1) * (n + 1), 0);
    for (size_t q = 0; q + 2 < p.tris.size(); q += 3) {
        uint32_t c[3] = {p.tris[q], p.tris[q + 1], p.tris[q + 2]};
        // Grid point (i steps towards corner 1, j towards corner 2).
        for (int i = 0; i <= n; i++)
            for (int j = 0; j + i <= n; j++) {
                int l = n - i - j;
                uint32_t id;
                if (l == n) id = c[0];
                else if (i == n) id = c[1];
                else if (j == n) id = c[2];
                else if (j == 0 || i == 0 || l == 0) {
                    uint32_t a, b;
                    int steps;
                    if (j == 0) { a = c[0]; b = c[1]; steps = i; }
                    else if (i == 0) { a = c[0]; b = c[2]; steps = j; }
                    else { a = c[1]; b = c[2]; steps = j; }
                    if (a > b) { std::swap(a, b); steps = n - steps; }
                    auto key = std::array<uint32_t, 3>{a, b, (uint32_t)steps};
                    auto it = edge.find(key);
                    if (it != edge.end()) id = it->second;
                    else {
                        id = (uint32_t)g.v.size();
                        g.v.push_back(lerp(p.v[a], p.v[b], (double)steps / n));
                        edge[key] = id;
                    }
                } else {
                    Vtx o;
                    double fa = (double)l / n, fb = (double)i / n, fc = (double)j / n;
                    for (int k = 0; k < 3; k++) {
                        o.p[k] = p.v[c[0]].p[k] * fa + p.v[c[1]].p[k] * fb + p.v[c[2]].p[k] * fc;
                        o.n[k] = p.v[c[0]].n[k] * fa + p.v[c[1]].n[k] * fb + p.v[c[2]].n[k] * fc;
                    }
                    for (int k = 0; k < 2; k++) o.uv[k] = p.v[c[0]].uv[k] * fa + p.v[c[1]].uv[k] * fb + p.v[c[2]].uv[k] * fc;
                    id = (uint32_t)g.v.size();
                    g.v.push_back(o);
                }
                grid[(size_t)i * (n + 1) + j] = id;
            }
        auto G = [&](int i, int j) { return grid[(size_t)i * (n + 1) + j]; };
        for (int i = 0; i < n; i++)
            for (int j = 0; j + i < n; j++) {
                g.sub.push_back({G(i, j), G(i + 1, j), G(i, j + 1)});
                if (i + j + 1 < n) g.sub.push_back({G(i + 1, j), G(i + 1, j + 1), G(i, j + 1)});
            }
    }
    return g;
}

// A ray from the sky's middle along d, against triangle ABC: where (t along d) and the weights of B and C there.
bool ray_hit(const double *d, const double *A, const double *B, const double *C, double &t, double &b1, double &b2) {
    double e1[3] = {B[0] - A[0], B[1] - A[1], B[2] - A[2]}, e2[3] = {C[0] - A[0], C[1] - A[1], C[2] - A[2]};
    double p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
    double det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
    if (std::fabs(det) < 1e-30) return false;
    double inv = 1 / det, s[3] = {-A[0], -A[1], -A[2]};
    b1 = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
    if (b1 < -1e-9 || b1 > 1 + 1e-9) return false;
    double q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
    b2 = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
    if (b2 < -1e-9 || b1 + b2 > 1 + 1e-9) return false;
    t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
    return t > 0;
}

// The triangles of a part the Xbox draws (one-sided ones only facing the sky's middle).
std::vector<size_t> drawn_triangles(const Part &p, bool two_sided, bool inward) {
    std::vector<size_t> out;
    for (size_t t = 0; t + 2 < p.tris.size(); t += 3)
        if (two_sided || faces_in(p.v[p.tris[t]].p, p.v[p.tris[t + 1]].p, p.v[p.tris[t + 2]].p) == inward) out.push_back(t);
    return out;
}

// Whether direction d meets one of a part's drawn triangles; which (its first index), and the weights there.
bool meets(const Part &p, const std::vector<size_t> &drawn, const double *d, size_t &tri, double &b1, double &b2) {
    double best = 1e300;
    bool hit = false;
    for (size_t t : drawn) {
        double tt, x, y;
        if (ray_hit(d, p.v[p.tris[t]].p, p.v[p.tris[t + 1]].p, p.v[p.tris[t + 2]].p, tt, x, y) && tt < best) {
            best = tt;
            tri = t;
            b1 = x;
            b2 = y;
            hit = true;
        }
    }
    return hit;
}

// ---- Folding layers into one pass
//
// A pass U (an alpha-blended layer whose transparency doesn't move) takes in:
//  - the layer drawn just before it, L, when that's still and on the same dome (the same triangles seen from the
//    same direction, the same texture coordinates);
//  - the still alpha-blended layers drawn just after it (O: a haze band, sun rays), where U's dome is in front of
//    them: those triangles of theirs are cut away and what they drew is drawn by U.
// U's maps: a still prefix (0..k-1) and moving ones D (k.., at most two). The pass's maps:
//   P (U's still prefix's colour; alpha: the total), D as they were, Lb (L's colour; alpha w), Ob (O's colour; alpha v),
// the last D blending to Lb by w and Lb to Ob by v ("blend next map alpha inverse": so far * a + next * (1 - a)):
//   L then U:  colour U w + L (1 - w),  alpha A = Ua + La (1 - Ua),  w = Ua / A
//   then O:    colour so far v + O (1 - v),  alpha At = Oa + A (1 - Oa),  v = A (1 - Oa) / At
// which is what drawing them one after another gives, alpha blended (and the sky's planar fog stage treats it the
// same way). All still (no D): one map, the whole stack baked.

struct Fold {
    size_t upper = 0;
    int lower = -1;                 // part, or -1
    std::vector<size_t> over;       // parts
    std::vector<Part> below;        // each over part's pieces below U's rim (still drawn as they were)
    int k = 0;
};

// U's rim: its open edges, which must make one loop around the direction U is centred on (a dome's lower edge), U a
// cap inside it. Then the directions a part of a layer over U is in front of, or not, are those on each side of the
// planes through the sky's middle and each rim edge, within the wedge from the centre through that edge.
struct Rim {
    double pole[3] = {0, 0, 0};
    std::vector<std::array<double, 3>> a, b;   // each rim edge, as U's triangles go round it
    double sign = 1;
    bool ok = false;
};
double det3(const double *a, const double *b, const double *c) {
    return a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0]);
}
void cross3(const double *a, const double *b, double *o) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}
Rim rim_of(const Part &U, const std::vector<size_t> &drawn) {
    Rim r;
    std::map<std::pair<uint32_t, uint32_t>, int> count;
    for (size_t t : drawn)
        for (int k = 0; k < 3; k++) {
            uint32_t a = U.tris[t + k], b = U.tris[t + (k + 1) % 3];
            count[{std::min(a, b), std::max(a, b)}]++;
        }
    for (auto &v : U.v) {
        double l = std::sqrt(v.p[0] * v.p[0] + v.p[1] * v.p[1] + v.p[2] * v.p[2]);
        if (l > 0)
            for (int k = 0; k < 3; k++) r.pole[k] += v.p[k] / l;
    }
    double pl = std::sqrt(r.pole[0] * r.pole[0] + r.pole[1] * r.pole[1] + r.pole[2] * r.pole[2]);
    if (pl <= 0) return r;
    for (double &x : r.pole) x /= pl;
    double turn = 0;
    int signs = 0;
    for (size_t t : drawn)
        for (int k = 0; k < 3; k++) {
            uint32_t a = U.tris[t + k], b = U.tris[t + (k + 1) % 3];
            if (count[{std::min(a, b), std::max(a, b)}] != 1) continue;
            const double *A = U.v[a].p, *B = U.v[b].p;
            double d = det3(r.pole, A, B);
            if (d == 0) return r;
            signs += d > 0 ? 1 : -1;
            // the edge's turn round the pole
            double pa[3], pb[3];
            double da = A[0] * r.pole[0] + A[1] * r.pole[1] + A[2] * r.pole[2], db = B[0] * r.pole[0] + B[1] * r.pole[1] + B[2] * r.pole[2];
            for (int q = 0; q < 3; q++) { pa[q] = A[q] - da * r.pole[q]; pb[q] = B[q] - db * r.pole[q]; }
            turn += angle_between(pa, pb);
            r.a.push_back({A[0], A[1], A[2]});
            r.b.push_back({B[0], B[1], B[2]});
        }
    if (r.a.empty() || std::abs(signs) != (int)r.a.size() || std::fabs(turn - 2 * kPi) > 1e-3) return r;   // (not one loop round it)
    r.sign = signs > 0 ? 1 : -1;
    r.ok = true;
    return r;
}

// A convex polygon (on one plane) cut by a plane through the sky's middle: the side n . x >= 0.
std::vector<Vtx> clip(const std::vector<Vtx> &poly, const double *n) {
    std::vector<Vtx> out;
    for (size_t i = 0; i < poly.size(); i++) {
        const Vtx &a = poly[i], &b = poly[(i + 1) % poly.size()];
        double da = n[0] * a.p[0] + n[1] * a.p[1] + n[2] * a.p[2], db = n[0] * b.p[0] + n[1] * b.p[1] + n[2] * b.p[2];
        if (da >= 0) out.push_back(a);
        if ((da >= 0) != (db >= 0)) {
            double f = da / (da - db);
            Vtx o;
            for (int k = 0; k < 3; k++) { o.p[k] = a.p[k] + (b.p[k] - a.p[k]) * f; o.n[k] = a.n[k] + (b.n[k] - a.n[k]) * f; }
            for (int k = 0; k < 2; k++) o.uv[k] = a.uv[k] + (b.uv[k] - a.uv[k]) * f;
            out.push_back(o);
        }
    }
    return out;
}

// What of a layer's triangles is below U's rim (not in front of U), as triangles (vertices close together merged).
// `above`: how much of it (solid angle) is in front.
Part below_rim(const Part &O, const std::vector<size_t> &drawn, const Rim &rim, double &above, double &all);

bool plan_fold(const Model &m, Context &cx, size_t ui, Fold &f) {
    const Part &U = m.parts[ui];
    if (U.shader < 0 || U.shader >= (int)m.shader_refs.size() || !U.one_node) return false;
    const ShaderI &us = cx.shader(m.shader_refs[(size_t)U.shader]);
    if (!plain_layer(us)) return false;
    for (auto &mp : us.maps)
        if (!mp.img) return false;
    f = Fold();
    f.upper = ui;
    int k = 0;
    while (k < (int)us.maps.size() && still(us.maps[(size_t)k])) k++;
    int moving = (int)us.maps.size() - k;
    if (moving > 2) return false;
    if (moving) {
        // U's transparency mustn't move: a moving map's alpha unused, or opaque.
        if (k == 0 && us.maps[0].img->lo[3] < 254.5f / 255) return false;
        for (size_t s = (size_t)std::max(k, 1); s < us.maps.size(); s++) {
            if (us.maps[s - 1].af != 0 && us.maps[s].img->lo[3] < 254.5f / 255) return false;
            int cf = us.maps[s - 1].cf;
            if (cf == 9 || cf == 10) return false;   // (blends by the alpha so far, which becomes the total)
        }
    }
    f.k = k;
    int slots = moving ? 1 + moving : 1;   // (all still: one map)
    // L
    if (ui > 0) {
        const Part &L = m.parts[ui - 1];
        if (L.shader >= 0 && L.shader < (int)m.shader_refs.size() && L.geometry != U.geometry && same_dome(L, U)) {
            const ShaderI &ls = cx.shader(m.shader_refs[(size_t)L.shader]);
            bool ok = plain_layer(ls) && (ls.flags & 4) == (us.flags & 4) && (!moving || slots + 1 <= 4);
            for (auto &mp : ls.maps) ok = ok && still(mp);
            if (ok) {
                f.lower = (int)(ui - 1);
                if (moving) slots++;
            }
        }
    }
    // O: the layers after U, as far as there are ones partly in front of U (what's below U's rim stays as it was).
    if (!moving || slots + 1 <= 4) {
        bool inward = inward_winding(m.parts);
        Rim rim = rim_of(U, drawn_triangles(U, us.two_sided(), inward));
        for (size_t oi = ui + 1; oi < m.parts.size() && rim.ok; oi++) {
            const Part &O = m.parts[oi];
            if (O.shader < 0 || O.shader >= (int)m.shader_refs.size() || !O.one_node) break;
            const ShaderI &os = cx.shader(m.shader_refs[(size_t)O.shader]);
            bool ok = plain_layer(os);
            for (auto &mp : os.maps) ok = ok && still(mp);
            if (!ok) break;
            double above = 0, all = 0;
            Part kept = below_rim(O, drawn_triangles(O, os.two_sided(), inward), rim, above, all);
            if (above < 0.02 * all) break;
            f.over.push_back(oi);
            f.below.push_back(std::move(kept));
        }
        if (!f.over.empty() && moving) slots++;
    }
    return f.lower >= 0 || !f.over.empty();
}

// Parts with no triangles left are taken out of their geometry (the parts after them renumbered in the links between
// parts); a geometry may be left with none. Shaders no part draws are dropped.
void tidy_model(TagNode &r) {
    auto *geos = tagfile::block(r, "geometries");
    auto *shaders = tagfile::block(r, "shaders");
    for (auto &g : *geos) {
        auto *parts = tagfile::block(g, "parts");
        for (size_t pi = parts->size(); pi-- > 0;) {
            auto *T = tagfile::block((*parts)[pi], "triangles");
            if (T && !T->empty()) continue;
            parts->erase(parts->begin() + (long)pi);
            for (auto &p : *parts)
                for (const char *f : {"prev filthy part index", "next filthy part index"}) {
                    int x = (int)get(p, f);
                    if (x == 255) continue;
                    if (x == (int)pi) tagfile::set_num(p, f, 255);
                    else if (x > (int)pi) tagfile::set_num(p, f, x - 1);
                }
        }
    }
    std::vector<int> used(shaders->size(), 0);
    for (auto &g : *geos)
        for (auto &p : *tagfile::block(g, "parts")) {
            int s = (int)(int16_t)get(p, "shader index");
            if (s >= 0 && s < (int)used.size()) used[(size_t)s] = 1;
        }
    std::vector<int> remap(shaders->size(), -1);
    std::vector<TagNode> kept;
    for (size_t i = 0; i < shaders->size(); i++)
        if (used[i]) { remap[i] = (int)kept.size(); kept.push_back((*shaders)[i]); }
    *shaders = kept;
    for (auto &g : *geos)
        for (auto &p : *tagfile::block(g, "parts")) {
            int s = (int)(int16_t)get(p, "shader index");
            if (s >= 0 && s < (int)remap.size()) tagfile::set_num(p, "shader index", remap[(size_t)s]);
        }
}

// A part as its grid triangles `keep` says (the rest cut away).
struct Trim {
    std::vector<Vtx> v;
    std::vector<uint32_t> tris;
    double kept = 0, all = 0;   // solid angle
};
Trim keep_cells(const Grid &g, const std::vector<char> &keep) {
    Trim t;
    std::vector<uint32_t> remap(g.v.size(), ~0u);
    for (size_t si = 0; si < g.sub.size(); si++) {
        if (!keep[si]) continue;
        for (uint32_t x : g.sub[si]) {
            if (remap[x] == ~0u) { remap[x] = (uint32_t)t.v.size(); t.v.push_back(g.v[x]); }
            t.tris.push_back(remap[x]);
        }
    }
    return t;
}
void write_part(TagNode &part, const Trim &t);

bool do_fold(Model &m, Context &cx, const Fold &f, const Find &find, const Edit &edit, std::vector<std::string> &written, std::string &what) {
    const Part &U = m.parts[f.upper];
    const ShaderI us = cx.shader(m.shader_refs[(size_t)U.shader]);
    const Part *L = f.lower >= 0 ? &m.parts[(size_t)f.lower] : nullptr;
    ShaderI ls;
    if (L) ls = cx.shader(m.shader_refs[(size_t)L->shader]);
    std::vector<ShaderI> os;
    for (size_t oi : f.over) os.push_back(cx.shader(m.shader_refs[(size_t)m.parts[oi].shader]));
    int k = f.k;
    size_t n = us.maps.size();
    bool all_still = k == (int)n;
    bool inward = inward_winding(m.parts);

    // The baked textures cover U's texture coordinates (with the base map scale): as fine as its still maps and L's
    // have them there; with layers drawn over it, at least 512 (the most a sky texture keeps on the Xbox).
    double u0 = 1e30, u1 = -1e30, v0 = 1e30, v1 = -1e30;
    for (auto &v : U.v) {
        u0 = std::min(u0, v.uv[0] * m.bu); u1 = std::max(u1, v.uv[0] * m.bu);
        v0 = std::min(v0, v.uv[1] * m.bv); v1 = std::max(v1, v.uv[1] * m.bv);
    }
    if (!(u1 > u0) || !(v1 > v0)) return false;
    double du = f.over.empty() ? 0 : 512, dv = du;
    auto density = [&](const MapI &mp) {
        du = std::max(du, mp.img->w * std::fabs(mp.us) * (u1 - u0));
        dv = std::max(dv, mp.img->h * std::fabs(mp.vs) * (v1 - v0));
    };
    if (L)
        for (auto &mp : ls.maps) density(mp);
    for (int i = 0; i < k; i++) density(us.maps[(size_t)i]);
    if (du > 1024 * 1.01 || dv > 1024 * 1.01) return false;   // (a tiled still map: too fine to bake)
    int W = pow2_at_least(du * 0.99), H = pow2_at_least(dv * 0.99);

    // Each texel's direction, through U's triangles in texture space (one place for each texel: U's texture mustn't
    // overlap itself), for the layers over U.
    struct UvTri { double a[2], b[2], c[2]; size_t t; };
    std::vector<UvTri> uvt;
    for (size_t t : drawn_triangles(U, us.two_sided(), inward)) {
        const Vtx &A = U.v[U.tris[t]], &B = U.v[U.tris[t + 1]], &C = U.v[U.tris[t + 2]];
        uvt.push_back({{A.uv[0] * m.bu, A.uv[1] * m.bv}, {B.uv[0] * m.bu, B.uv[1] * m.bv}, {C.uv[0] * m.bu, C.uv[1] * m.bv}, t});
    }
    std::vector<std::vector<size_t>> odrawn;
    for (size_t e = 0; e < f.over.size(); e++) odrawn.push_back(drawn_triangles(m.parts[f.over[e]], os[e].two_sided(), inward));

    std::vector<float> P((size_t)W * H * 4), LB((size_t)W * H * 4), OB((size_t)W * H * 4);
    const V4 opaque = [] { V4 o; for (float &x : o.v) x = 1; return o; }();
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            double u = u0 + (x + 0.5) / W * (u1 - u0), v = v0 + (y + 0.5) / H * (v1 - v0);
            auto at = [&](const MapI &mp, double uu, double vv) {
                return sample(*mp.img, uu * mp.us + mp.uo, vv * mp.vs + mp.vo, (mp.flags & 4) != 0, (mp.flags & 8) != 0);
            };
            // Where on U this texel is (off U's texture: the nearest point of it, for the texels around its edge).
            double dir[3] = {0, 0, 0}, best = 1e300, first[3] = {0, 0, 0};
            int inside = 0;
            for (auto &q : uvt) {
                if (f.over.empty()) break;
                double e1[2] = {q.b[0] - q.a[0], q.b[1] - q.a[1]}, e2[2] = {q.c[0] - q.a[0], q.c[1] - q.a[1]};
                double det = e1[0] * e2[1] - e1[1] * e2[0];
                if (std::fabs(det) < 1e-18) continue;
                double px = u - q.a[0], py = v - q.a[1];
                double b1 = (px * e2[1] - py * e2[0]) / det, b2 = (e1[0] * py - e1[1] * px) / det;
                double out = std::max({0.0, -b1, -b2, b1 + b2 - 1});
                double c1 = std::min(1.0, std::max(0.0, b1)), c2 = std::min(1.0 - c1, std::max(0.0, b2));
                const double *A3 = U.v[U.tris[q.t]].p, *B3 = U.v[U.tris[q.t + 1]].p, *C3 = U.v[U.tris[q.t + 2]].p;
                double pt[3];
                for (int kk = 0; kk < 3; kk++) pt[kk] = A3[kk] + (B3[kk] - A3[kk]) * c1 + (C3[kk] - A3[kk]) * c2;
                if (out <= 1e-9) {
                    if (inside++ == 0) std::copy(pt, pt + 3, first);
                    else if (angle_between(first, pt) > 0.2 * kPi / 180) return false;   // (texture over itself)
                }
                if (out < best) { best = out; std::copy(pt, pt + 3, dir); }
            }
            V4 TU[4];
            for (size_t i = 0; i < n; i++) TU[i] = (int)i < k ? at(us.maps[i], u, v) : opaque;
            V4 Uo = chicago_eval(us.maps, TU);
            V4 prefix = k ? chicago_eval(std::vector<MapI>(us.maps.begin(), us.maps.begin() + k), TU) : opaque;
            float Ua = Uo.v[3], A = Ua, w = 1;
            V4 Lo;
            if (L) {
                V4 TL[4];
                for (size_t i = 0; i < ls.maps.size(); i++) TL[i] = at(ls.maps[i], u, v);
                Lo = chicago_eval(ls.maps, TL);
                A = Ua + Lo.v[3] * (1 - Ua);
                w = A > 0 ? Ua / A : 1;
            }
            // The layers over U here, in the order drawn.
            float Oc[3] = {0, 0, 0}, Oa = 0;
            for (size_t e = 0; e < f.over.size(); e++) {
                const Part &O = m.parts[f.over[e]];
                size_t t;
                double b1, b2;
                if (!meets(O, odrawn[e], dir, t, b1, b2)) continue;
                const Vtx &A3 = O.v[O.tris[t]], &B3 = O.v[O.tris[t + 1]], &C3 = O.v[O.tris[t + 2]];
                double ou = (A3.uv[0] + (B3.uv[0] - A3.uv[0]) * b1 + (C3.uv[0] - A3.uv[0]) * b2) * m.bu;
                double ov = (A3.uv[1] + (B3.uv[1] - A3.uv[1]) * b1 + (C3.uv[1] - A3.uv[1]) * b2) * m.bv;
                V4 TO[4];
                for (size_t i = 0; i < os[e].maps.size(); i++) TO[i] = at(os[e].maps[i], ou, ov);
                V4 o = chicago_eval(os[e].maps, TO);
                for (int c = 0; c < 3; c++) Oc[c] = o.v[c] * o.v[3] + Oc[c] * (1 - o.v[3]);   // (premultiplied)
                Oa = o.v[3] + Oa * (1 - o.v[3]);
            }
            if (Oa > 0)
                for (float &c : Oc) c /= Oa;
            float At = Oa + A * (1 - Oa);
            float vw = At > 0 ? A * (1 - Oa) / At : 1;
            float *p = &P[((size_t)y * W + x) * 4], *lb = &LB[((size_t)y * W + x) * 4], *ob = &OB[((size_t)y * W + x) * 4];
            if (all_still) {
                for (int c = 0; c < 3; c++) {
                    float so = L ? Uo.v[c] * w + Lo.v[c] * (1 - w) : Uo.v[c];
                    p[c] = so * vw + Oc[c] * (1 - vw);
                }
                p[3] = At;
            } else {
                for (int c = 0; c < 3; c++) {
                    p[c] = prefix.v[c];
                    lb[c] = L ? Lo.v[c] : 0;
                    ob[c] = Oc[c];
                }
                p[3] = At;
                lb[3] = w;
                ob[3] = vw;
            }
        }

    std::string name = stem(us.tag);
    if (L) name += " " + stem(ls.tag);
    for (auto &o : os) name += " " + stem(o.tag);
    std::string bdir = dir_of(us.maps[0].bitmap.empty() ? us.tag : (L ? ls.maps[0].bitmap : us.maps[0].bitmap));
    std::string pa = bdir + name + (all_still ? "" : " a") + ".bitmap";
    std::string pl = bdir + name + " b.bitmap", po = bdir + name + " c.bitmap";
    fs::path like = find(L ? ls.maps[0].bitmap : us.maps[0].bitmap);
    if (like.empty()) return false;
    auto put = [&](const std::string &tag, const std::vector<float> &px) {
        fs::path out = edit(tag);
        if (out.empty() || !write_bitmap(like, out, W, H, px)) return false;
        written.push_back(tag);
        return true;
    };
    if (!put(pa, P)) return false;
    if (!all_still && L && !put(pl, LB)) return false;
    if (!all_still && !f.over.empty() && !put(po, OB)) return false;

    // The pass's shader: U's, its maps made over.
    std::string shader_tag = dir_of(us.tag) + name + ".shader_transparent_chicago";
    fs::path uf = find(us.tag), sf = edit(shader_tag);
    if (uf.empty() || sf.empty()) return false;
    tagfile::EditTag st;
    if (!st.load(uf)) return false;
    auto *maps = tagfile::block(st.root(), "maps");
    if (!maps || maps->size() < n) return false;
    std::vector<TagNode> old = *maps, made;
    auto baked = [&](const std::string &bitmap, int cf, int replicate) {
        TagNode t = old[0];
        tagfile::set_num(t, "flags", 4 | 8 | replicate);   // clamped (the bake covers U's coordinates)
        tagfile::set_num(t, "color function", cf);
        tagfile::set_num(t, "alpha function", 0);
        tagfile::set_num(t, "map u scale", 1 / (u1 - u0));   // (the engine multiplies in the base map scale)
        tagfile::set_num(t, "map v scale", 1 / (v1 - v0));
        tagfile::set_num(t, "map u offset", -u0 / (u1 - u0));
        tagfile::set_num(t, "map v offset", -v0 / (v1 - v0));
        tagfile::set_num(t, "map rotation", 0);
        tagfile::set_num(t, "mipmap bias", 0);
        // No animation: function "zero" (a scale left at 0 becomes 1 when the map's built, and "one" times that would
        // move it a whole texture over).
        for (const char *a : {"u", "v", "rotation"}) {
            std::string ax = a;
            tagfile::set_num(t, (ax + " animation source").c_str(), 0);
            tagfile::set_num(t, (ax + " animation function").c_str(), 1);
            tagfile::set_num(t, (ax + " animation period").c_str(), 1);
            tagfile::set_num(t, (ax + " animation phase").c_str(), 0);
            tagfile::set_num(t, (ax + " animation scale").c_str(), ax == "rotation" ? 360 : 1);
        }
        tagfile::set_num(t, "rotation animation center", 0, 0);
        tagfile::set_num(t, "rotation animation center", 0, 1);
        if (std::string *r = tagfile::ref_path(t, "map")) {
            *r = no_ext(bitmap);
            std::replace(r->begin(), r->end(), '/', '\\');
        }
        return t;
    };
    const int kBlendNextAlphaInverse = 12;   // so far * next's alpha + next * (1 - next's alpha)
    if (all_still) {
        made.push_back(baked(pa, 0, 0));
    } else {
        made.push_back(baked(pa, k ? us.maps[(size_t)k - 1].cf : 1, k ? (us.maps[(size_t)k - 1].flags & 2) : 0));
        for (size_t i = (size_t)k; i < n; i++) {
            TagNode t = old[i];
            tagfile::set_num(t, "alpha function", 0);
            if (i + 1 == n) {
                tagfile::set_num(t, "color function", kBlendNextAlphaInverse);
                tagfile::set_num(t, "flags", (int)get(t, "flags") & ~2);
            }
            made.push_back(t);
        }
        if (L) made.push_back(baked(pl, f.over.empty() ? 0 : kBlendNextAlphaInverse, 0));
        if (!f.over.empty()) made.push_back(baked(po, 0, 0));
    }
    *maps = made;
    if (auto *layers = tagfile::block(st.root(), "extra layers")) layers->clear();
    std::error_code ec;
    fs::create_directories(sf.parent_path(), ec);
    if (!st.save(sf)) return false;
    written.push_back(shader_tag);

    // The model: U's part draws the pass; L's part is gone, and the parts of O's the pass draws.
    TagNode &r = m.tag.root();
    auto *shaders = tagfile::block(r, "shaders");
    auto *geos = tagfile::block(r, "geometries");
    TagNode ref = (*shaders)[(size_t)U.shader];
    if (std::string *p = tagfile::ref_path(ref, "shader")) {
        *p = no_ext(shader_tag);
        std::replace(p->begin(), p->end(), '/', '\\');
    }
    shaders->push_back(ref);
    tagfile::set_num((*tagfile::block((*geos)[(size_t)U.geometry], "parts"))[(size_t)U.part], "shader index", (double)(shaders->size() - 1));
    if (L) tagfile::block((*tagfile::block((*geos)[(size_t)L->geometry], "parts"))[(size_t)L->part], "triangles")->clear();
    for (size_t e = 0; e < f.over.size(); e++) {
        const Part &O = m.parts[f.over[e]];
        TagNode &pn = (*tagfile::block((*geos)[(size_t)O.geometry], "parts"))[(size_t)O.part];
        if (f.below[e].tris.empty()) tagfile::block(pn, "triangles")->clear();
        else {
            Trim t;
            t.v = f.below[e].v;
            t.tris = f.below[e].tris;
            write_part(pn, t);
        }
    }
    tidy_model(r);
    what = (L ? stem(ls.tag) + " and " : std::string()) + stem(us.tag);
    for (auto &o : os) what += ", " + stem(o.tag);
    return true;
}

// ---- Trimming: where a part's texture can't show (clear), its triangles are cut away, on an n x n grid of them.

double solid_angle(const double *a, const double *b, const double *c) {
    double la = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), lb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]),
           lc = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    double bc[3] = {b[1] * c[2] - b[2] * c[1], b[2] * c[0] - b[0] * c[2], b[0] * c[1] - b[1] * c[0]};
    double num = a[0] * bc[0] + a[1] * bc[1] + a[2] * bc[2];
    double den = la * lb * lc + (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) * lc + (a[0] * c[0] + a[1] * c[1] + a[2] * c[2]) * lb +
                 (b[0] * c[0] + b[1] * c[1] + b[2] * c[2]) * la;
    return std::fabs(2 * std::atan2(num, den));
}

Part below_rim(const Part &O, const std::vector<size_t> &drawn, const Rim &rim, double &above, double &all) {
    Part out = O;
    out.v.clear();
    out.tris.clear();
    above = all = 0;
    std::map<std::array<long long, 5>, uint32_t> weld;   // (position, texture coordinates)
    auto add = [&](const Vtx &v) {
        double l = std::sqrt(v.p[0] * v.p[0] + v.p[1] * v.p[1] + v.p[2] * v.p[2]);
        double q = std::max(l, 1e-30) * 1e-7;
        std::array<long long, 5> key{(long long)std::llround(v.p[0] / q), (long long)std::llround(v.p[1] / q), (long long)std::llround(v.p[2] / q),
                                     (long long)std::llround(v.uv[0] * 1e6), (long long)std::llround(v.uv[1] * 1e6)};
        auto it = weld.find(key);
        if (it != weld.end()) return it->second;
        uint32_t id = (uint32_t)out.v.size();
        out.v.push_back(v);
        weld[key] = id;
        return id;
    };
    std::set<size_t> drawn_set(drawn.begin(), drawn.end());
    for (size_t t = 0; t + 2 < O.tris.size(); t += 3) {
        std::vector<Vtx> tri = {O.v[O.tris[t]], O.v[O.tris[t + 1]], O.v[O.tris[t + 2]]};
        double whole = solid_angle(tri[0].p, tri[1].p, tri[2].p);
        if (!drawn_set.count(t)) {   // (not drawn: kept as it is)
            uint32_t a = add(tri[0]), b = add(tri[1]), c = add(tri[2]);
            out.tris.insert(out.tris.end(), {a, b, c});
            continue;
        }
        all += whole;
        double kept = 0;
        for (size_t e = 0; e < rim.a.size(); e++) {
            const double *A = rim.a[e].data(), *B = rim.b[e].data();
            double n1[3], n2[3], n3[3];
            cross3(rim.pole, A, n1);
            cross3(B, rim.pole, n2);
            cross3(A, B, n3);
            for (int k = 0; k < 3; k++) { n1[k] *= rim.sign; n2[k] *= rim.sign; n3[k] *= -rim.sign; }
            std::vector<Vtx> poly = clip(clip(clip(tri, n1), n2), n3);
            if (poly.size() < 3) continue;
            for (size_t i = 1; i + 1 < poly.size(); i++) {
                kept += solid_angle(poly[0].p, poly[i].p, poly[i + 1].p);
                uint32_t a = add(poly[0]), b = add(poly[i]), c = add(poly[i + 1]);
                if (a != b && b != c && a != c) out.tris.insert(out.tris.end(), {a, b, c});
            }
        }
        above += std::max(0.0, whole - kept);
    }
    return out;
}

// A texture may be drawn at a quarter of its size or less by the time the map is built (the porter's texture budget),
// each of its texels then spread over 4 or more of these: the margin taken around where a triangle reads.
const int kShrink = 4;

Trim trim(const Part &p, const ShaderI &s, double bu, double bv, int n) {
    Grid g = subdivide(p, n);
    std::vector<char> keep(g.sub.size(), 0);
    double all = 0, kept = 0;
    const double px_per_rad = 640 / (70 * kPi / 180);   // the Xbox's views: 640 pixels across 70 degrees
    for (size_t si = 0; si < g.sub.size(); si++) {
        const Vtx &a = g.v[g.sub[si][0]], &b = g.v[g.sub[si][1]], &c = g.v[g.sub[si][2]];
        double omega = solid_angle(a.p, b.p, c.p);
        all += omega;
        double ang = std::max({angle_between(a.p, b.p), angle_between(b.p, c.p), angle_between(a.p, c.p)});
        double pixels = std::max(1.0, ang * px_per_rad);
        I4 T[4];
        for (size_t mi = 0; mi < s.maps.size(); mi++) {
            const MapI &mp = s.maps[mi];
            if (!mp.img) { for (auto &x : T[mi].v) x = {0, 1}; continue; }
            if (mp.moving) {
                for (int ch = 0; ch < 4; ch++) T[mi].v[ch] = {mp.img->lo[ch], mp.img->hi[ch]};
                continue;
            }
            double uu[3], vv[3];
            const Vtx *V3[3] = {&a, &b, &c};
            for (int k = 0; k < 3; k++) {
                uu[k] = V3[k]->uv[0] * bu * mp.us + mp.uo;
                vv[k] = V3[k]->uv[1] * bv * mp.vs + mp.vo;
            }
            double umin = std::min({uu[0], uu[1], uu[2]}), umax = std::max({uu[0], uu[1], uu[2]});
            double vmin = std::min({vv[0], vv[1], vv[2]}), vmax = std::max({vv[0], vv[1], vv[2]});
            double texels = std::max((umax - umin) * mp.img->w, (vmax - vmin) * mp.img->h);
            double per_pixel = texels / pixels;   // (the mip level read: each texel there spans this many)
            int margin = 2 * kShrink + (per_pixel > 1 ? (int)std::ceil(per_pixel) * 2 : 0);
            Iv r4[4];
            box_range(*mp.img, umin, vmin, umax, vmax, margin, (mp.flags & 4) != 0, (mp.flags & 8) != 0, r4);
            for (int ch = 0; ch < 4; ch++) T[mi].v[ch] = r4[ch];
        }
        if (can_show(chicago_range(s.maps, T), s.blend)) {
            keep[si] = 1;
            kept += omega;
        }
    }
    Trim t = keep_cells(g, keep);
    t.all = all;
    t.kept = kept;
    return t;
}

template <unsigned bits> uint32_t pack_float(double f) {
    const uint32_t sign = 1u << (bits - 1), mask = sign - 1;
    f = std::max(-1.0, std::min(1.0, f));
    if (f >= 0) return (uint32_t)(f * mask + 0.5);
    return (uint32_t)((1.0 + f) * mask + 0.5) | sign;
}
uint32_t pack_vector(double i, double j, double k) { return pack_float<11>(i) | (pack_float<11>(j) << 11) | (pack_float<10>(k) << 22); }

// A part written back with new triangles: the vertices it had as they were, new ones (both kinds) made like its first.
void write_part(TagNode &part, const Trim &t) {
    auto *V = tagfile::block(part, "uncompressed vertices");
    auto *C = tagfile::block(part, "compressed vertices");
    auto *T = tagfile::block(part, "triangles");
    if (!V || V->empty() || !T) return;
    bool compressed = C && C->size() == V->size() && !C->empty();
    std::vector<TagNode> nv, nc;
    for (auto &v : t.v) {
        bool own = v.src >= 0 && v.src < (int)V->size();
        if (own) {   // a vertex it had: as it was
            nv.push_back((*V)[(size_t)v.src]);
            if (compressed) nc.push_back((*C)[(size_t)v.src]);
            continue;
        }
        TagNode a = (*V)[0];
        const TagNode &tc = compressed ? (*C)[0] : a;
        double len = std::sqrt(v.n[0] * v.n[0] + v.n[1] * v.n[1] + v.n[2] * v.n[2]);
        double nn[3] = {0, 0, 1};
        if (len > 1e-12)
            for (int k = 0; k < 3; k++) nn[k] = v.n[k] / len;
        for (int k = 0; k < 3; k++) {
            tagfile::set_num(a, "position", (float)v.p[k], k);
            tagfile::set_num(a, "normal", nn[k], k);
        }
        tagfile::set_num(a, "texture coords", (float)v.uv[0], 0);
        tagfile::set_num(a, "texture coords", (float)v.uv[1], 1);
        nv.push_back(a);
        if (compressed) {
            TagNode b = tc;
            for (int k = 0; k < 3; k++) tagfile::set_num(b, "position", (float)v.p[k], k);
            tagfile::set_num(b, "normal", pack_vector(nn[0], nn[1], nn[2]));
            tagfile::set_num(b, "texture coordinate u", (double)(int16_t)(uint16_t)pack_float<16>(v.uv[0]));
            tagfile::set_num(b, "texture coordinate v", (double)(int16_t)(uint16_t)pack_float<16>(v.uv[1]));
            nc.push_back(b);
        }
    }
    *T = list_to_strip(t.tris, t.v.size(), tagfile::block_struct(part, "triangles"));
    *V = std::move(nv);
    if (compressed) *C = std::move(nc);
}

const int kMaxNewVertices = 1500;   // over the whole sky (each costs the Xbox 32 bytes)

}   // namespace

Fill measure(const std::string &sky_tag, const Find &find) {
    Fill f;
    tagfile::Tag st;
    fs::path sf = find(sky_tag);
    if (sf.empty() || !st.load(sf)) return f;
    std::string model = st.root().ref("model");
    fs::path mf = model.empty() ? fs::path() : find(model);
    Model m;
    if (mf.empty() || !read_model(mf, m)) return f;
    Context cx(find);
    return fill_of(m, cx);
}

Lightened lighten(const std::string &sky_tag, const Find &find, const Edit &edit) {
    Lightened out;
    tagfile::Tag st;
    fs::path sf = find(sky_tag);
    if (sf.empty() || !st.load(sf)) return out;
    std::string model = st.root().ref("model");
    fs::path mf = model.empty() ? fs::path() : find(model);
    Model m;
    if (mf.empty() || !read_model(mf, m)) return out;
    {
        Context cx(find);
        out.before = fill_of(m, cx);
    }
    bool changed = false;

    // Folds: layers drawn in one pass where they can be.
    size_t verts_before = 0;
    for (auto &p : m.parts) verts_before += p.v.size();
    for (int round = 0; round < 8; round++) {
        Context cx(find);
        bool did = false;
        for (size_t i = 0; i < m.parts.size() && !did; i++) {
            Fold f;
            if (!plan_fold(m, cx, i, f)) continue;
            std::string what;
            if (!do_fold(m, cx, f, find, edit, out.written, what)) continue;
            out.merged.push_back(what);
            did = changed = true;
        }
        if (!did || !parse_model(m)) break;
    }
    size_t verts_folded = 0;
    for (auto &p : m.parts) verts_folded += p.v.size();

    // Trimming: each part, at the grid that saves the most for the vertices it adds (none past the sky's budget).
    {
        Context cx(find);
        int budget = kMaxNewVertices - std::max(0, (int)verts_folded - (int)verts_before);
        TagNode &r = m.tag.root();
        auto *geos = tagfile::block(r, "geometries");
        struct Choice { size_t part; Trim t; double saved; };
        std::vector<Choice> choices;
        for (size_t pi = 0; pi < m.parts.size(); pi++) {
            const Part &p = m.parts[pi];
            if (p.shader < 0 || p.shader >= (int)m.shader_refs.size()) continue;
            const ShaderI &s = cx.shader(m.shader_refs[(size_t)p.shader]);
            if (!s.readable || !s.chicago || s.layers || s.fade != 0 || s.first_type != 0 || (s.flags & (8 | 64 | 128)) ||
                !functions_ok(s) || s.lens_flare)
                continue;
            // The grid that saves the most fill for the vertices it adds: each 100 must save a hundredth of a layer.
            const double per_vertex = 1e-4;
            bool inward = inward_winding(m.parts);
            double whole = part_fill(p.v, p.tris, s.two_sided(), inward);
            Trim best;
            double best_saved = 0, best_value = 0.005;
            bool have = false;
            for (int n : {1, 2, 3, 4, 6, 8}) {
                Trim t = trim(p, s, m.bu, m.bv, n);
                if (t.kept >= t.all * 0.98) continue;
                double saved = whole - part_fill(t.v, t.tris, s.two_sided(), inward);
                double value = saved - per_vertex * std::max(0.0, (double)t.v.size() - (double)p.v.size());
                if (value > best_value) { best_value = value; best_saved = saved; best = std::move(t); have = true; }
            }
            if (!have) continue;
            choices.push_back({pi, std::move(best), best_saved});
        }
        std::sort(choices.begin(), choices.end(), [](const Choice &a, const Choice &b) {
            return a.saved / std::max<size_t>(1, a.t.v.size()) > b.saved / std::max<size_t>(1, b.t.v.size());
        });
        for (auto &c : choices) {
            const Part &p = m.parts[c.part];
            int added = (int)c.t.v.size() - (int)p.v.size();
            if (added > budget) continue;
            budget -= std::max(0, added);
            auto *parts = tagfile::block((*geos)[(size_t)p.geometry], "parts");
            if (c.t.tris.empty()) tagfile::block((*parts)[(size_t)p.part], "triangles")->clear();
            else write_part((*parts)[(size_t)p.part], c.t);
            const ShaderI &s = cx.shader(m.shader_refs[(size_t)p.shader]);
            int pct = (int)std::lround(100 * (1 - c.t.kept / std::max(1e-12, c.t.all)));
            out.trimmed.push_back(stem(s.tag) + " (" + std::to_string(pct) + "%)");
            changed = true;
        }
    }
    if (!changed) {
        out.after = out.before;
        return out;
    }
    tidy_model(m.tag.root());
    fs::path mo = edit(model);
    if (mo.empty() || !m.tag.save(mo)) return out;
    out.written.push_back(model);
    out.changed = true;
    out.after = measure(sky_tag, find);
    return out;
}


Baked bake(const std::string &sky_tag, const Find &find, int size) {
    Baked out;
    tagfile::Tag st;
    fs::path sf = find(sky_tag);
    if (sf.empty() || !st.load(sf)) return out;
    std::string model = st.root().ref("model");
    fs::path mf = model.empty() ? fs::path() : find(model);
    Model m;
    if (mf.empty() || !read_model(mf, m)) return out;
    Context cx(find);
    for (auto &p : m.parts) {
        Baked::Part b;
        for (auto &v : p.v) b.p.push_back({(float)v.p[0], (float)v.p[1], (float)v.p[2]});
        b.tris = p.tris;
        const ShaderI *s = p.shader >= 0 && p.shader < (int)m.shader_refs.size() ? &cx.shader(m.shader_refs[(size_t)p.shader]) : nullptr;
        b.two_sided = s && s->two_sided();
        bool any = false;
        if (s && (s->chicago || s->generic))
            for (auto &mp : s->maps) any = any || mp.img;
        if (s) b.shader = s->tag;
        if (!any || p.v.empty()) {   // (not a chicago shader with a texture read: left for the caller to draw its way)
            for (auto &v : p.v) b.uv.push_back({(float)(v.uv[0] * m.bu), (float)(v.uv[1] * m.bv)});
            out.parts.push_back(std::move(b));
            continue;
        }
        // Its maps combined as the chicago combiners do, over the part's base texture coordinates (a texel each).
        double u0 = 1e300, v0 = 1e300, u1 = -1e300, v1 = -1e300;
        for (auto &v : p.v) {
            u0 = std::min(u0, v.uv[0] * m.bu); u1 = std::max(u1, v.uv[0] * m.bu);
            v0 = std::min(v0, v.uv[1] * m.bv); v1 = std::max(v1, v.uv[1] * m.bv);
        }
        if (u1 - u0 < 1e-6) u1 = u0 + 1e-6;
        if (v1 - v0 < 1e-6) v1 = v0 + 1e-6;
        for (auto &v : p.v) b.uv.push_back({(float)((v.uv[0] * m.bu - u0) / (u1 - u0)), (float)((v.uv[1] * m.bv - v0) / (v1 - v0))});
        b.w = b.h = size;
        b.rgba.resize((size_t)size * size * 4);
        const V4 white = [] { V4 o; for (float &x : o.v) x = 1; return o; }();
        std::vector<V4> T(s->maps.size());
        // Its framebuffer blend function: 0 alpha blend, 1 multiply, 2 double multiply, 3 add, 7 alpha-multiply add
        // (added, weighted by its alpha: baked in); the rest drawn as alpha blended.
        const int blend = s->blend;
        b.blend = blend == 1 ? 2 : blend == 2 ? 3 : (blend == 3 || blend == 7) ? 1 : 0;
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++) {
                double u = u0 + (x + 0.5) / size * (u1 - u0), v = v0 + (y + 0.5) / size * (v1 - v0);
                for (size_t k = 0; k < s->maps.size(); k++) {
                    const MapI &mp = s->maps[k];
                    T[k] = mp.img ? sample(*mp.img, u * mp.us + mp.uo, v * mp.vs + mp.vo, (mp.flags & 4) != 0, (mp.flags & 8) != 0) : white;
                }
                V4 c = s->generic ? generic_eval(*s, T.data()) : chicago_eval(s->maps, T.data());
                if (blend == 7) for (int k = 0; k < 3; k++) c.v[k] *= c.v[3];
                uint8_t *o = &b.rgba[((size_t)y * size + x) * 4];
                for (int k = 0; k < 4; k++) o[k] = (uint8_t)std::lround(std::min(1.f, std::max(0.f, c.v[k])) * 255);
            }
        b.baked = true;
        out.parts.push_back(std::move(b));
    }
    out.ok = true;
    return out;
}

}   // namespace sky
