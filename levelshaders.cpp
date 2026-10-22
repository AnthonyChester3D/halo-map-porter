// Halo Map Porter - the level's shaders on the Xbox (see levelshaders.hpp)
#include "levelshaders.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <set>

#include "bcdec.h"
#include "tagfile.hpp"

namespace lshade {

namespace fs = std::filesystem;
using V3 = std::array<double, 3>;

namespace {

V3 sub(V3 a, V3 b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 mul(V3 a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(V3 a, V3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 cross(V3 a, V3 b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
V3 norm(V3 a) {
    double l = std::sqrt(dot(a, a));
    return l > 1e-12 ? mul(a, 1 / l) : V3{0, 0, 1};
}

// One bitmap of a bitmap tag as RGBA (top row first), at most `max_side` pixels on a side; a cube map's six faces
// side by side (+x -x +y -y +z -z, each at most `max_side`).
bool decode(const fs::path &file, int index, int max_side, Texture &out) {
    tagfile::Tag t;
    if (!t.load(file)) return false;
    tagfile::Elem r = t.root();
    tagfile::Block bd = r.block("bitmap data");
    if (index < 0 || (size_t)index >= bd.size()) return false;
    tagfile::Elem e = bd[(size_t)index];
    int w = (int)e.integer("width"), h = (int)e.integer("height"), fmt = (int)e.integer("format");
    int type = (int)e.integer("type");
    long long flags = e.integer("flags");
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || (flags & 8)) return false;   // (8: swizzled)
    int faces = type == 2 ? 6 : 1;                                                 // (2: cube map)
    std::string pix = r.data("processed pixel data");
    size_t off = (size_t)e.integer("pixel data offset");
    bool dxt = fmt == 14 || fmt == 15 || fmt == 16;
    int bpp = fmt <= 2 || fmt == 17 ? 1 : (fmt == 3 || fmt == 6 || fmt == 8 || fmt == 9) ? 2 : (fmt == 10 || fmt == 11) ? 4 : 0;
    if (!dxt && !bpp) return false;
    size_t face_bytes = dxt ? (size_t)((w + 3) / 4) * ((h + 3) / 4) * (fmt == 14 ? 8 : 16) : (size_t)w * h * bpp;
    if (off + face_bytes * faces > pix.size()) return false;
    int f = 1;
    while (std::max(w, h) / f > max_side) f *= 2;
    int nw = std::max(1, w / f), nh = std::max(1, h / f);
    out.w = nw * faces;
    out.h = nh;
    out.rgba.assign((size_t)out.w * nh * 4, 0);
    std::vector<uint8_t> rgba((size_t)w * h * 4);
    for (int face = 0; face < faces; face++) {
        const unsigned char *p = (const unsigned char *)pix.data() + off + face_bytes * face;
        std::fill(rgba.begin(), rgba.end(), 255);
        if (dxt) {
            int bw = (w + 3) / 4, bh = (h + 3) / 4, bs = fmt == 14 ? 8 : 16;
            uint8_t block[64];
            for (int by = 0; by < bh; by++)
                for (int bx = 0; bx < bw; bx++) {
                    const unsigned char *src = p + ((size_t)by * bw + bx) * bs;
                    if (fmt == 14) bcdec_bc1(src, block, 16);
                    else if (fmt == 15) bcdec_bc2(src, block, 16);
                    else bcdec_bc3(src, block, 16);
                    for (int y = 0; y < 4 && by * 4 + y < h; y++)
                        for (int x = 0; x < 4 && bx * 4 + x < w; x++)
                            memcpy(&rgba[((size_t)(by * 4 + y) * w + bx * 4 + x) * 4], &block[(y * 4 + x) * 4], 4);
                }
        } else {
            for (size_t i = 0; i < (size_t)w * h; i++) {
                uint8_t *q = &rgba[i * 4];
                unsigned v = bpp == 2 ? p[i * 2] | (p[i * 2 + 1] << 8) : 0;
                switch (fmt) {
                case 0: q[0] = q[1] = q[2] = 255; q[3] = p[i]; break;
                case 1: case 2: case 17: q[0] = q[1] = q[2] = p[i]; break;
                case 3: q[0] = q[1] = q[2] = p[i * 2]; q[3] = p[i * 2 + 1]; break;
                case 6: q[0] = (v >> 11) * 255 / 31; q[1] = ((v >> 5) & 63) * 255 / 63; q[2] = (v & 31) * 255 / 31; break;
                case 8: q[0] = ((v >> 10) & 31) * 255 / 31; q[1] = ((v >> 5) & 31) * 255 / 31; q[2] = (v & 31) * 255 / 31; q[3] = v & 0x8000 ? 255 : 0; break;
                case 9: q[0] = ((v >> 8) & 15) * 17; q[1] = ((v >> 4) & 15) * 17; q[2] = (v & 15) * 17; q[3] = (v >> 12) * 17; break;
                case 10: q[0] = p[i * 4 + 2]; q[1] = p[i * 4 + 1]; q[2] = p[i * 4]; break;
                default: q[0] = p[i * 4 + 2]; q[1] = p[i * 4 + 1]; q[2] = p[i * 4]; q[3] = p[i * 4 + 3]; break;
                }
            }
        }
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++)
                for (int c = 0; c < 4; c++) {
                    unsigned sum = 0, n = 0;
                    for (int dy = 0; dy < f && y * f + dy < h; dy++)
                        for (int dx = 0; dx < f && x * f + dx < w; dx++, n++) sum += rgba[((size_t)(y * f + dy) * w + x * f + dx) * 4 + c];
                    out.rgba[((size_t)y * out.w + face * nw + x) * 4 + c] = (uint8_t)(n ? sum / n : 0);
                }
    }
    return true;
}

// A texture's colour at (u, v), each channel 0..1 (bilinear; `wrap` or clamp to the edge). `face`/`faces`: a cube
// map's face (its part of the strip).
inline void sample(const Texture &t, double u, double v, bool wrap, float out[4], int face = 0, int faces = 1) {
    if (t.w <= 0 || t.h <= 0) { out[0] = out[1] = out[2] = out[3] = 1; return; }
    int fw = t.w / faces, h = t.h;
    double x = u * fw - 0.5, y = v * h - 0.5;
    int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    double fx = x - x0, fy = y - y0;
    auto at = [&](int xx, int yy) -> const uint8_t * {
        if (wrap) { xx = ((xx % fw) + fw) % fw; yy = ((yy % h) + h) % h; }
        else { xx = std::clamp(xx, 0, fw - 1); yy = std::clamp(yy, 0, h - 1); }
        return &t.rgba[((size_t)yy * t.w + (size_t)face * fw + xx) * 4];
    };
    const uint8_t *a = at(x0, y0), *b = at(x0 + 1, y0), *c = at(x0, y0 + 1), *d = at(x0 + 1, y0 + 1);
    for (int k = 0; k < 4; k++)
        out[k] = (float)(((a[k] * (1 - fx) + b[k] * fx) * (1 - fy) + (c[k] * (1 - fx) + d[k] * fx) * fy) / 255.0);
}

// A cube map's colour in direction `d` (the Xbox's cube faces: +x -x +y -y +z -z).
inline void sample_cube(const Texture &t, const V3 &d, float out[4]) {
    double ax = std::fabs(d[0]), ay = std::fabs(d[1]), az = std::fabs(d[2]);
    int face;
    double sc, tc, ma;
    if (ax >= ay && ax >= az) { ma = ax; face = d[0] > 0 ? 0 : 1; sc = d[0] > 0 ? -d[2] : d[2]; tc = -d[1]; }
    else if (ay >= az) { ma = ay; face = d[1] > 0 ? 2 : 3; sc = d[0]; tc = d[1] > 0 ? d[2] : -d[2]; }
    else { ma = az; face = d[2] > 0 ? 4 : 5; sc = d[2] > 0 ? d[0] : -d[0]; tc = -d[1]; }
    sample(t, (sc / ma + 1) / 2, (tc / ma + 1) / 2, false, out, face, 6);
}

const char *kShaderKinds[] = {".shader_environment", ".shader_model", ".shader_transparent_chicago",
                              ".shader_transparent_chicago_extended", ".shader_transparent_generic",
                              ".shader_transparent_glass", ".shader_transparent_water", ".shader_transparent_meter",
                              ".shader_transparent_plasma"};

std::string shader_extension(const std::string &group) {
    static const std::map<std::string, std::string> m{
        {"senv", ".shader_environment"}, {"soso", ".shader_model"}, {"schi", ".shader_transparent_chicago"},
        {"scex", ".shader_transparent_chicago_extended"}, {"sotr", ".shader_transparent_generic"},
        {"sgla", ".shader_transparent_glass"}, {"swat", ".shader_transparent_water"},
        {"smet", ".shader_transparent_meter"}, {"spla", ".shader_transparent_plasma"}};
    auto it = m.find(group);
    return it == m.end() ? "" : it->second;
}

// A tag reference's path with its extension ("" for none).
std::string with_extension(const std::string &path, const std::string &group, const Find &find) {
    if (path.empty()) return "";
    if (fs::path(path).has_extension() && !find(path).empty()) return path;
    std::string ext = shader_extension(group);
    if (!ext.empty() && !find(path + ext).empty()) return path + ext;
    for (const char *k : kShaderKinds)
        if (!find(path + k).empty()) return path + k;
    return "";
}

// The texture of a bitmap tag's first bitmap (cached by path), or -1.
// A colour of its own for each room (cluster): hues a golden-ratio step apart.
void room_colour(int room, double out[3]) {
    double h = std::fmod(room * 0.618033988749895, 1.0) * 6, s = 0.75, v = 0.95;
    int i = (int)h;
    double f = h - i, p_ = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    double c[6][3] = {{v, t, p_}, {q, v, p_}, {p_, v, t}, {p_, q, v}, {t, p_, v}, {v, p_, q}};
    for (int k = 0; k < 3; k++) out[k] = c[i % 6][k];
}

struct Textures {
    Preview &p;
    const Find &find;
    std::map<std::pair<std::string, int>, int> done;
    int get(const std::string &bitmap, int index, int max_side) {
        if (bitmap.empty()) return -1;
        auto key = std::make_pair(bitmap, index);
        auto it = done.find(key);
        if (it != done.end()) return it->second;
        int id = -1;
        fs::path file = find(bitmap);
        Texture t;
        if (!file.empty() && decode(file, index, max_side, t)) {
            id = (int)p.textures.size();
            p.textures.push_back(std::move(t));
        }
        done[key] = id;
        return id;
    }
};

std::string bitmap_ref(const tagfile::Elem &e, const char *field) {
    std::string r = e.ref(field);
    if (r.empty()) return r;
    return fs::path(r).extension() == ".bitmap" ? r : r + ".bitmap";
}

}   // namespace

Cost cost(const Analysis &a, const std::vector<char> &dropped) {
    Cost c;
    size_t S = a.shaders.size();
    if (a.views <= 0) return c;
    std::vector<double> per((size_t)a.views);
    for (int v = 0; v < a.views; v++) {
        double sum = a.other.empty() ? 0 : a.other[(size_t)v];
        for (size_t s = 0; s < S; s++) {
            const Shader &sh = a.shaders[s];
            int passes = 2 + sh.extra_passes - (s < dropped.size() && dropped[s] ? sh.reflection_passes : 0);
            sum += (double)a.drawn[(size_t)v * S + s] * passes;
        }
        per[(size_t)v] = sum;
    }
    std::sort(per.begin(), per.end());
    c.typical = per[per.size() / 2];
    c.busy = per[per.size() * 9 / 10];
    return c;
}

Cost stock_cost() {
    // Sidewinder's level, counted the same way: among the busiest of the stock Xbox multiplayer maps (typical / busy:
    // Blood Gulch 1,338 / 4,984, Damnation 1,830 / 8,183, Sidewinder 3,348 / 8,260, Boarding Action 2,132 / 9,156,
    // Prisoner 3,620 / 9,522, the most), which the Xbox draws at its frame rate.
    return Cost{3350, 8260};
}

bool analyze(const fs::path &bsp_file, const fs::path &scenario, const Find &find, Analysis &a, Preview &p, std::string *why) {
    auto fail = [&](const char *w) { if (why) *why = w; return false; };
    a = Analysis();
    p = Preview();
    tagfile::Tag t;
    if (!t.load(bsp_file)) return fail("couldn't be read");
    tagfile::Elem r = t.root();
    tagfile::Block surfaces = r.block("surfaces"), lightmaps = r.block("lightmaps");
    p.triangles.assign(surfaces.size(), Triangle());
    Textures tex{p, find, {}};
    std::string lightmaps_bitmap = bitmap_ref(r, "lightmaps bitmap");
    std::map<std::string, int> material_of, shader_of;
    std::vector<int> material_triangles;

    auto material_for = [&](const std::string &tag) -> int {
        auto it = material_of.find(tag);
        if (it != material_of.end()) return it->second;
        Material m;
        fs::path file = tag.empty() ? fs::path() : find(tag);
        tagfile::Tag st;
        if (!file.empty() && st.load(file)) {
            tagfile::Elem s = st.root();
            if (fs::path(tag).extension() == ".shader_environment") {
                Shader sh;
                sh.tag = tag;
                sh.perpendicular = (float)s.num("perpendicular brightness");
                sh.parallel = (float)s.num("parallel brightness");
                std::string cube = bitmap_ref(s, "reflection cube map");
                sh.cube_map = cube;
                double scale = s.num("lightmap brightness scale");
                bool reflects = sh.perpendicular > 0 || sh.parallel > 0;
                int refl = reflects && !cube.empty() ? 1 : 0, mask = reflects && scale < 1.0 ? 1 : 0;
                int spec = s.num("brightness") > 0 && ((long long)s.num("specular flags") & 4) ? 1 : 0;
                sh.reflection_passes = refl + mask;
                sh.extra_passes = refl + mask + spec;
                m.shader = (int)a.shaders.size();
                shader_of[tag] = m.shader;
                a.shaders.push_back(sh);
                m.base = tex.get(bitmap_ref(s, "base map"), 0, 256);
                m.detail = tex.get(bitmap_ref(s, "primary detail map"), 0, 64);
                m.detail_scale = (float)(s.num("primary detail map scale") > 0 ? s.num("primary detail map scale") : 1);
                long long flags = (long long)s.num("shader environment flags");
                m.mask_by_base_alpha = s.num("shader environment type") == 0 && !(flags & 2);
                if (refl) {
                    m.cube = tex.get(cube, 0, 32);
                    for (int k = 0; k < 3; k++) {
                        m.perpendicular[k] = (float)(s.num("perpendicular color", k) * sh.perpendicular);
                        m.parallel[k] = (float)(s.num("parallel color", k) * sh.parallel);
                    }
                }
                m.lightmap_scale = mask ? (float)std::max(0.0, scale) : 1.0f;
            } else {
                m.see_through = true;
                std::string ext = fs::path(tag).extension().u8string();
                if (ext == ".shader_transparent_chicago" || ext == ".shader_transparent_chicago_extended" ||
                    ext == ".shader_transparent_generic") {
                    tagfile::Block maps = s.block(ext == ".shader_transparent_chicago_extended" ? "maps 4 stage" : "maps");
                    if (maps.size() == 0 && ext == ".shader_transparent_chicago_extended") maps = s.block("maps 2 stage");
                    if (maps.size()) m.base = tex.get(bitmap_ref(maps[0], "map"), 0, 128);
                }
            }
        } else {
            m.see_through = !tag.empty() && fs::path(tag).extension() != ".shader_environment";
        }
        int id = (int)p.materials.size();
        p.materials.push_back(m);
        material_of[tag] = id;
        return id;
    };

    for (size_t l = 0; l < lightmaps.size(); l++) {
        long long page = lightmaps[l].integer("bitmap");
        int lm = page >= 0 && page < 65535 ? tex.get(lightmaps_bitmap, (int)page, 256) : -1;
        tagfile::Block mats = lightmaps[l].block("materials");
        for (size_t mi = 0; mi < mats.size(); mi++) {
            tagfile::Elem m = mats[mi];
            std::string group, sref = m.ref("shader", &group);
            int material = material_for(with_extension(sref, group, find));
            size_t count = (size_t)m.integer("rendered vertices count"), lcount = (size_t)m.integer("lightmap vertices count");
            std::string data = m.data("uncompressed vertices");
            bool compressed = false;
            if (data.size() < count * 56) {
                data = m.data("compressed vertices");
                compressed = true;
                if (data.size() < count * 32) continue;
            }
            size_t stride = compressed ? 32 : 56, lstride = compressed ? 8 : 20, lbase = count * stride;
            bool have_lm = lcount == count && data.size() >= lbase + lcount * lstride;
            auto f32 = [&](size_t at) { float f; memcpy(&f, data.data() + at, 4); return f; };
            auto unpack = [](uint32_t v, float out[3]) {
                int x = (int)(v & 0x7FF), y = (int)((v >> 11) & 0x7FF), z = (int)(v >> 22);
                if (x & 0x400) x -= 0x800;
                if (y & 0x400) y -= 0x800;
                if (z & 0x200) z -= 0x400;
                out[0] = x / 1023.0f; out[1] = y / 1023.0f; out[2] = z / 511.0f;
            };
            auto vertex = [&](size_t i, Vertex &v) {
                size_t at = i * stride;
                for (int k = 0; k < 3; k++) v.p[k] = f32(at + 4 * k);
                if (compressed) {
                    uint32_t n;
                    memcpy(&n, data.data() + at + 12, 4);
                    unpack(n, v.n);
                    v.uv[0] = f32(at + 24); v.uv[1] = f32(at + 28);
                } else {
                    for (int k = 0; k < 3; k++) v.n[k] = f32(at + 12 + 4 * k);
                    v.uv[0] = f32(at + 48); v.uv[1] = f32(at + 52);
                }
                v.luv[0] = v.luv[1] = 0.5f;
                if (have_lm) {
                    size_t la = lbase + i * lstride;
                    if (compressed) {
                        int16_t u, w;
                        memcpy(&u, data.data() + la + 4, 2);
                        memcpy(&w, data.data() + la + 6, 2);
                        v.luv[0] = u / 32767.0f; v.luv[1] = w / 32767.0f;
                    } else {
                        v.luv[0] = f32(la + 12); v.luv[1] = f32(la + 16);
                    }
                }
            };
            long long first = m.integer("surfaces"), n = m.integer("surface count");
            for (long long sfc = first; sfc < first + n && sfc >= 0 && sfc < (long long)surfaces.size(); sfc++) {
                size_t i0 = (size_t)surfaces[(size_t)sfc].integer("vertex0 index"), i1 = (size_t)surfaces[(size_t)sfc].integer("vertex1 index"),
                       i2 = (size_t)surfaces[(size_t)sfc].integer("vertex2 index");
                if (i0 >= count || i1 >= count || i2 >= count) continue;
                Triangle &tr = p.triangles[(size_t)sfc];
                vertex(i0, tr.v[0]);
                vertex(i1, tr.v[1]);
                vertex(i2, tr.v[2]);
                tr.material = material;
                tr.lightmap = lm;
                int sh = p.materials[(size_t)material].shader;
                if (sh >= 0) a.shaders[(size_t)sh].triangles++;
            }
        }
    }
    if (a.shaders.empty() && p.materials.empty()) return fail("has no surfaces");

    // The rooms (clusters: each triangle's first) and the portals between them.
    tagfile::Block clusters = r.block("clusters"), cportals = r.block("cluster portals");
    p.rooms = (int)clusters.size();
    p.room.assign(p.triangles.size(), -1);
    for (size_t c = 0; c < clusters.size(); c++) {
        tagfile::Block sub = clusters[c].block("subclusters");
        for (size_t k = 0; k < sub.size(); k++) {
            tagfile::Block idx = sub[k].block("surface indices");
            for (size_t j = 0; j < idx.size(); j++) {
                long long sfc = idx[j].integer("index");
                if (sfc >= 0 && sfc < (long long)p.room.size() && p.room[(size_t)sfc] < 0) p.room[(size_t)sfc] = (int16_t)c;
            }
        }
    }
    for (size_t i = 0; i < cportals.size(); i++) {
        Portal po;
        po.front = (int)cportals[i].integer("front cluster");
        po.back = (int)cportals[i].integer("back cluster");
        tagfile::Block vs = cportals[i].block("vertices");
        for (size_t k = 0; k < vs.size(); k++)
            po.points.push_back({(float)vs[k].num("point", 0), (float)vs[k].num("point", 1), (float)vs[k].num("point", 2)});
        if (po.points.size() >= 3) p.portals.push_back(std::move(po));
    }
    // The fog: the scenario's skies, each room's sky, and the tree that says which room a point is in.
    {
        auto fog_of = [](const tagfile::Elem &sky, const char *kind, Fog &f) {
            std::string k = kind;
            for (int c = 0; c < 3; c++) f.color[c] = (float)sky.num((k + " fog color").c_str(), c);
            f.density = (float)sky.num((k + " fog maximum density").c_str());
            f.start = (float)sky.num((k + " fog start distance").c_str());
            f.opaque = (float)sky.num((k + " fog opaque distance").c_str());
        };
        tagfile::Tag st;
        if (!scenario.empty() && st.load(scenario)) {
            tagfile::Block skies = st.root().block("skies");
            for (size_t i = 0; i < skies.size(); i++) {
                Fog f;
                std::string ref = skies[i].ref("sky");
                fs::path file = ref.empty() ? fs::path() : find(ref);
                tagfile::Tag sky;
                if (!file.empty() && sky.load(file)) {
                    fog_of(sky.root(), "outdoor", f);
                    if (i == 0) fog_of(sky.root(), "indoor", p.indoor_fog);
                }
                p.sky_fog.push_back(f);
            }
        }
        for (size_t c = 0; c < clusters.size(); c++) {
            long long sky = clusters[c].integer("sky");
            p.room_sky.push_back((int16_t)(sky >= 0 && sky < 65535 ? sky : -1));
        }
        tagfile::Block cbs = r.block("collision bsp"), leaves = r.block("leaves");
        if (cbs.size()) {
            tagfile::Block bn = cbs[0].block("bsp3d nodes"), bp = cbs[0].block("planes");
            auto child = [](long long c) -> int32_t {
                uint32_t u = (uint32_t)c;
                if (u == 0xFFFFFFFFu) return -1;
                if (u & 0x80000000u) return ~(int32_t)(u & 0x7FFFFFFFu) - 1;   // (leaf k: -2 - k)
                return (int32_t)u;
            };
            for (size_t i = 0; i < bn.size(); i++) {
                Preview::Node n;
                size_t pl = (size_t)((uint32_t)bn[i].integer("plane") & 0x7FFFFFFFu);
                if (pl < bp.size()) {
                    double nx = bp[pl].num("plane", 0), ny = bp[pl].num("plane", 1), nz = bp[pl].num("plane", 2), d = bp[pl].num("plane", 3);
                    double l = std::sqrt(nx * nx + ny * ny + nz * nz);
                    if (l > 0) { nx /= l; ny /= l; nz /= l; d /= l; }
                    n.n[0] = (float)nx; n.n[1] = (float)ny; n.n[2] = (float)nz; n.d = (float)d;
                }
                n.front = child(bn[i].integer("front child"));
                n.back = child(bn[i].integer("back child"));
                p.tree.push_back(n);
            }
            for (size_t i = 0; i < leaves.size(); i++) {
                long long c = leaves[i].integer("cluster");
                p.leaf_room.push_back((int16_t)(c >= 0 && c < (long long)clusters.size() ? c : -1));
            }
        }
    }

    // What the engine draws from where players play.
    portals::PlayArea area = scenario.empty() ? portals::PlayArea() : portals::play_area(scenario);
    std::vector<portals::SampleView> views;
    if (!portals::sample_views(bsp_file, area, 600, views)) return fail("has nowhere for players to stand");
    size_t S = a.shaders.size();
    a.views = (int)views.size();
    a.drawn.assign((size_t)a.views * S, 0);
    a.other.assign((size_t)a.views, 0);
    for (size_t v = 0; v < views.size(); v++) {
        for (int sfc : views[v].drawn) {
            if (sfc < 0 || (size_t)sfc >= p.triangles.size()) continue;
            int m = p.triangles[(size_t)sfc].material;
            int sh = m >= 0 ? p.materials[(size_t)m].shader : -1;
            if (sh >= 0) a.drawn[v * S + (size_t)sh]++;
            else a.other[v]++;
        }
        views[v].drawn.clear();
        views[v].drawn.shrink_to_fit();
    }
    p.views = views;
    for (size_t s = 0; s < S; s++) {
        double sum = 0;
        for (int v = 0; v < a.views; v++) sum += a.drawn[(size_t)v * S + s];
        a.shaders[s].drawn = sum / std::max(1, a.views);
    }
    // How much each reflection shows: the views drawn small, every reflection on.
    a.rendered = std::min(a.views, 96);
    a.shows.assign((size_t)a.rendered * S, 0);
    std::vector<char> all(S, 1);
    std::vector<uint32_t> px;
    std::vector<Shown> shown;
    std::vector<double> refl(S, 0), bright(S, 0), pixels(S, 0);
    double picture = 0;
    const int W = 192, H = 144;
    for (int v = 0; v < a.rendered; v++) {
        Camera c;
        c.eye = views[(size_t)v].eye;
        c.yaw = views[(size_t)v].yaw;
        double total = 0;
        draw(p, c, all, -1, W, H, px, &shown, &total);
        picture += total;
        for (size_t s = 0; s < S; s++) {
            a.shows[(size_t)v * S + s] = total > 0 ? (float)(shown[s].reflection / total) : 0;
            refl[s] += shown[s].reflection;
            bright[s] += shown[s].brightness;
            pixels[s] += shown[s].pixels;
        }
    }
    for (size_t s = 0; s < S; s++) {
        a.shaders[s].shows = picture > 0 ? refl[s] / picture : 0;
        a.shaders[s].strength = bright[s] > 0 ? refl[s] / bright[s] : 0;
        a.shaders[s].coverage = pixels[s] / ((double)W * H * std::max(1, a.rendered));
    }
    return true;
}

int Preview::room_at(const std::array<double, 3> &q) const {
    if (tree.empty()) return rooms == 1 ? 0 : -1;
    int32_t id = 0;
    for (size_t steps = 0; id >= 0 && steps <= tree.size(); steps++) {
        const Node &n = tree[(size_t)id];
        id = n.n[0] * q[0] + n.n[1] * q[1] + n.n[2] * q[2] - n.d >= 0 ? n.front : n.back;
        if (id >= (int32_t)tree.size()) return -1;
    }
    if (id >= -1) return -1;   // (solid, or a loop in a broken tree)
    size_t leaf = (size_t)(-2 - id);
    return leaf < leaf_room.size() ? leaf_room[leaf] : -1;
}

Fog Preview::fog_at(const std::array<double, 3> &eye, bool *outdoor) const {
    int r = room_at(eye);
    int sky = r >= 0 && (size_t)r < room_sky.size() ? room_sky[(size_t)r] : (room_sky.empty() ? -1 : room_sky[0]);
    if (outdoor) *outdoor = sky >= 0;
    if (sky >= 0 && (size_t)sky < sky_fog.size()) return sky_fog[(size_t)sky];
    return indoor_fog;
}

void suggest(Analysis &a) {
    const double kInvisible = 0.001;   // under 0.1% of what you see, all views together: nobody would miss it
    size_t S = a.shaders.size();
    std::vector<char> dropped(S, 0);
    for (size_t s = 0; s < S; s++) {
        a.shaders[s].suggested = false;
        if (a.shaders[s].reflection_passes > 0 && a.shaders[s].shows < kInvisible) dropped[s] = 1;
    }
    // Then, while the level costs more than stock ones, the ones that show least for what they cost.
    Cost target = stock_cost();
    std::vector<size_t> order;
    for (size_t s = 0; s < S; s++)
        if (!dropped[s] && a.shaders[s].reflection_passes > 0 && a.shaders[s].drawn > 0) order.push_back(s);
    auto value = [&](size_t s) { return a.shaders[s].shows / (a.shaders[s].drawn * a.shaders[s].reflection_passes); };
    std::sort(order.begin(), order.end(), [&](size_t x, size_t y) { return value(x) < value(y); });
    for (size_t s : order) {
        Cost c = cost(a, dropped);
        if (c.busy <= target.busy && c.typical <= target.typical) break;
        dropped[s] = 1;
    }
    for (size_t s = 0; s < S; s++) a.shaders[s].suggested = dropped[s] != 0;
}

void draw(const Preview &p, const Camera &cam, const std::vector<char> &reflections, int highlight, int w, int h,
          std::vector<uint32_t> &px, std::vector<Shown> *shown, double *total_out, const Extras *extras) {
    const bool show_portals = extras && extras->portals;
    const bool fogged = extras && extras->fog_on && extras->fog.any();
    const Fog fog = fogged ? extras->fog : Fog();
    const float far_z = fogged && fog.cuts() ? fog.opaque : 1e30f;
    px.assign((size_t)w * h, 0);
    size_t S = 0;
    for (auto &m : p.materials) S = std::max(S, (size_t)(m.shader + 1));
    if (shown) shown->assign(std::max(S, reflections.size()), Shown());
    const double tx = std::tan(35.0 * 3.14159265358979 / 180), ty = tx * h / w;
    V3 eye{cam.eye[0], cam.eye[1], cam.eye[2]};
    V3 fwd{std::cos(cam.yaw) * std::cos(cam.pitch), std::sin(cam.yaw) * std::cos(cam.pitch), std::sin(cam.pitch)};
    V3 right = norm(cross(fwd, V3{0, 0, 1})), up = cross(right, fwd);
    std::vector<float> zb((size_t)w * h, 1e30f);
    std::vector<int> tid((size_t)w * h, -1);
    std::vector<float> b1((size_t)w * h), b2((size_t)w * h);
    struct SV { double x, y, z; };   // view space
    auto to_view = [&](const float *q) {
        V3 d = sub(V3{q[0], q[1], q[2]}, eye);
        return SV{dot(d, right), dot(d, up), dot(d, fwd)};
    };
    // Rasterizes a triangle (in view space, clipped at the near plane): `pixel(x, y, z, b1, b2)` for each pixel it
    // covers, with the triangle's own barycentrics (perspective-correct) of its 2nd and 3rd corners.
    auto raster_corners = [&](const float *q0, const float *q1, const float *q2, const std::function<void(int, int, float, float, float)> &pixel) {
        SV c[3] = {to_view(q0), to_view(q1), to_view(q2)};
        // Corners as (view position, barycentric of the original corners).
        struct C { SV v; double l1, l2; };
        std::vector<C> poly{{c[0], 0, 0}, {c[1], 1, 0}, {c[2], 0, 1}}, out;
        const double near = 0.02;
        for (size_t k = 0; k < poly.size(); k++) {
            const C &A = poly[k], &B = poly[(k + 1) % poly.size()];
            bool ain = A.v.z >= near, bin = B.v.z >= near;
            if (ain) out.push_back(A);
            if (ain != bin) {
                double s = (near - A.v.z) / (B.v.z - A.v.z);
                out.push_back({{A.v.x + (B.v.x - A.v.x) * s, A.v.y + (B.v.y - A.v.y) * s, near}, A.l1 + (B.l1 - A.l1) * s, A.l2 + (B.l2 - A.l2) * s});
            }
        }
        if (out.size() < 3) return;
        struct P { double x, y, iz, l1z, l2z; };
        std::vector<P> sp;
        for (auto &q : out)
            sp.push_back({(q.v.x / q.v.z / tx * 0.5 + 0.5) * w, (0.5 - q.v.y / q.v.z / ty * 0.5) * h, 1 / q.v.z, q.l1 / q.v.z, q.l2 / q.v.z});
        for (size_t k = 1; k + 1 < sp.size(); k++) {
            const P &A = sp[0], &B = sp[k], &D = sp[k + 1];
            double area = (B.x - A.x) * (D.y - A.y) - (D.x - A.x) * (B.y - A.y);
            if (std::fabs(area) < 1e-9) continue;
            int x0 = std::max(0, (int)std::floor(std::min({A.x, B.x, D.x}))), x1 = std::min(w - 1, (int)std::ceil(std::max({A.x, B.x, D.x})));
            int y0 = std::max(0, (int)std::floor(std::min({A.y, B.y, D.y}))), y1 = std::min(h - 1, (int)std::ceil(std::max({A.y, B.y, D.y})));
            for (int y = y0; y <= y1; y++)
                for (int x = x0; x <= x1; x++) {
                    double px_ = x + 0.5, py = y + 0.5;
                    double w0 = ((B.x - px_) * (D.y - py) - (D.x - px_) * (B.y - py)) / area;
                    double w1 = ((D.x - px_) * (A.y - py) - (A.x - px_) * (D.y - py)) / area;
                    double w2 = 1 - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                    double iz = w0 * A.iz + w1 * B.iz + w2 * D.iz;
                    if (iz <= 0) continue;
                    double z = 1 / iz;
                    pixel(x, y, (float)z, (float)((w0 * A.l1z + w1 * B.l1z + w2 * D.l1z) * z), (float)((w0 * A.l2z + w1 * B.l2z + w2 * D.l2z) * z));
                }
        }
    };
    auto raster = [&](int i, const std::function<void(int, int, float, float, float)> &pixel) {
        const Triangle &t = p.triangles[(size_t)i];
        raster_corners(t.v[0].p, t.v[1].p, t.v[2].p, pixel);
    };
    auto front = [&](const Triangle &t) {   // (Halo winds a surface's corners clockwise seen from the front)
        V3 a{t.v[0].p[0], t.v[0].p[1], t.v[0].p[2]}, b{t.v[1].p[0], t.v[1].p[1], t.v[1].p[2]}, c{t.v[2].p[0], t.v[2].p[1], t.v[2].p[2]};
        return dot(cross(sub(b, a), sub(c, a)), sub(eye, a)) < 0;
    };
    std::vector<int> clear;
    for (size_t i = 0; i < p.triangles.size(); i++) {
        const Triangle &t = p.triangles[i];
        if (t.material < 0) continue;
        if (p.materials[(size_t)t.material].see_through) { clear.push_back((int)i); continue; }
        if (!front(t)) continue;
        raster((int)i, [&](int x, int y, float z, float l1, float l2) {
            size_t o = (size_t)y * w + x;
            if (z < zb[o]) { zb[o] = z; tid[o] = (int)i; b1[o] = l1; b2[o] = l2; }
        });
    }
    // Shading.
    double total = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            size_t o = (size_t)y * w + x;
            double col[3];
            if (tid[o] < 0 || zb[o] > far_z) {   // (the sky; past opaque fog the engine draws nothing either)
                double g = (double)y / h;
                col[0] = 0.30 + 0.15 * g; col[1] = 0.36 + 0.14 * g; col[2] = 0.46 + 0.10 * g;
            } else {
                const Triangle &t = p.triangles[(size_t)tid[o]];
                const Material &m = p.materials[(size_t)t.material];
                double l1 = b1[o], l2 = b2[o], l0 = 1 - l1 - l2;
                auto lerp2 = [&](const float *a, const float *b, const float *c, int k) { return a[k] * l0 + b[k] * l1 + c[k] * l2; };
                double u = lerp2(t.v[0].uv, t.v[1].uv, t.v[2].uv, 0), v = lerp2(t.v[0].uv, t.v[1].uv, t.v[2].uv, 1);
                double lu = lerp2(t.v[0].luv, t.v[1].luv, t.v[2].luv, 0), lv = lerp2(t.v[0].luv, t.v[1].luv, t.v[2].luv, 1);
                float base[4] = {0.6f, 0.6f, 0.6f, 1}, det[4] = {0.5f, 0.5f, 0.5f, 1}, lm[4] = {0.5f, 0.5f, 0.5f, 1};
                if (m.base >= 0) sample(p.textures[(size_t)m.base], u, v, true, base);
                if (m.detail >= 0) sample(p.textures[(size_t)m.detail], u * m.detail_scale, v * m.detail_scale, true, det);
                if (t.lightmap >= 0) sample(p.textures[(size_t)t.lightmap], lu, lv, false, lm);
                bool refl = m.cube >= 0 && m.shader >= 0 && ((size_t)m.shader >= reflections.size() || reflections[(size_t)m.shader]);
                for (int k = 0; k < 3; k++) col[k] = std::min(1.0, (double)base[k] * 2 * det[k]) * lm[k] * 2;
                if (refl) {
                    V3 n = norm(V3{lerp2(t.v[0].n, t.v[1].n, t.v[2].n, 0), lerp2(t.v[0].n, t.v[1].n, t.v[2].n, 1),
                                   lerp2(t.v[0].n, t.v[1].n, t.v[2].n, 2)});
                    V3 pos{lerp2(t.v[0].p, t.v[1].p, t.v[2].p, 0), lerp2(t.v[0].p, t.v[1].p, t.v[2].p, 1), lerp2(t.v[0].p, t.v[1].p, t.v[2].p, 2)};
                    V3 d = norm(sub(pos, eye));
                    double facing = std::fabs(dot(n, d));
                    V3 rd = sub(d, mul(n, 2 * dot(d, n)));
                    float cube[4];
                    sample_cube(p.textures[(size_t)m.cube], rd, cube);
                    // (the reflection goes where the specular mask lets it: the base map's alpha; with a lightmap
                    // brightness scale under 1, it's dimmer where the lightmap is dark)
                    double mask = m.mask_by_base_alpha ? base[3] : 1.0, lum = 0;
                    if (m.lightmap_scale < 1) mask *= m.lightmap_scale + (1 - m.lightmap_scale) * std::min(1.0, (lm[0] * 0.3 + lm[1] * 0.59 + lm[2] * 0.11) * 2.0);
                    for (int k = 0; k < 3; k++) {
                        double r = cube[k] * (m.parallel[k] + (m.perpendicular[k] - m.parallel[k]) * facing) * mask;
                        col[k] += r;
                        lum += r * (k == 0 ? 0.299 : k == 1 ? 0.587 : 0.114);
                    }
                    if (shown && (size_t)m.shader < shown->size()) (*shown)[(size_t)m.shader].reflection += lum;
                }
                if (shown && m.shader >= 0 && (size_t)m.shader < shown->size()) {
                    Shown &sh = (*shown)[(size_t)m.shader];
                    sh.pixels++;
                    sh.brightness += std::min(1.0, col[0]) * 0.299 + std::min(1.0, col[1]) * 0.587 + std::min(1.0, col[2]) * 0.114;
                }
                if (fogged) {   // (by distance in front of the camera, from start to opaque)
                    double span = std::max(1e-3, (double)fog.opaque - fog.start);
                    double f = fog.density * std::clamp((zb[o] - fog.start) / span, 0.0, 1.0);
                    for (int k = 0; k < 3; k++) col[k] = std::min(1.0, col[k]) * (1 - f) + fog.color[k] * f;
                }
                if (highlight >= 0 && m.shader == highlight) {
                    col[0] = col[0] * 0.5 + 0.5; col[1] *= 0.5; col[2] = col[2] * 0.5 + 0.45;
                }
                if (show_portals && (size_t)tid[o] < p.room.size() && p.room[(size_t)tid[o]] >= 0) {
                    double rc[3];
                    room_colour(p.room[(size_t)tid[o]], rc);
                    for (int k = 0; k < 3; k++) col[k] = std::min(1.0, col[k]) * 0.6 + rc[k] * 0.4;
                }
            }
            for (int k = 0; k < 3; k++) col[k] = std::min(1.0, col[k]);
            total += col[0] * 0.299 + col[1] * 0.587 + col[2] * 0.114;
            px[o] = (uint32_t)(col[0] * 255) << 16 | (uint32_t)(col[1] * 255) << 8 | (uint32_t)(col[2] * 255);
        }
    // See-through surfaces over the rest (in no particular order, half blended).
    for (int i : clear) {
        const Triangle &t = p.triangles[(size_t)i];
        const Material &m = p.materials[(size_t)t.material];
        raster(i, [&](int x, int y, float z, float l1, float l2) {
            size_t o = (size_t)y * w + x;
            if (z >= zb[o] || z > far_z) return;
            double l0 = 1 - l1 - l2;
            float c[4] = {0.55f, 0.65f, 0.75f, 0.35f};
            if (m.base >= 0) {
                double u = t.v[0].uv[0] * l0 + t.v[1].uv[0] * l1 + t.v[2].uv[0] * l2, v = t.v[0].uv[1] * l0 + t.v[1].uv[1] * l1 + t.v[2].uv[1] * l2;
                sample(p.textures[(size_t)m.base], u, v, true, c);
            }
            double alpha = 0.35;
            uint32_t q = px[o];
            double r = ((q >> 16) & 255) / 255.0 * (1 - alpha) + c[0] * alpha, g = ((q >> 8) & 255) / 255.0 * (1 - alpha) + c[1] * alpha,
                   b = (q & 255) / 255.0 * (1 - alpha) + c[2] * alpha;
            px[o] = (uint32_t)(std::min(1.0, r) * 255) << 16 | (uint32_t)(std::min(1.0, g) * 255) << 8 | (uint32_t)(std::min(1.0, b) * 255);
        });
    }
    // The portals: a glow where you see through one (nothing in front of it), and its outline.
    if (show_portals) {
        auto blend = [&](size_t o, const double c[3], double a) {
            uint32_t q = px[o];
            double r = ((q >> 16) & 255) / 255.0 * (1 - a) + c[0] * a, g = ((q >> 8) & 255) / 255.0 * (1 - a) + c[1] * a,
                   b = (q & 255) / 255.0 * (1 - a) + c[2] * a;
            px[o] = (uint32_t)(std::min(1.0, r) * 255) << 16 | (uint32_t)(std::min(1.0, g) * 255) << 8 | (uint32_t)(std::min(1.0, b) * 255);
        };
        const double glow[3] = {1.0, 0.62, 0.1}, edge[3] = {1.0, 0.85, 0.2};
        const double near = 0.02;
        std::vector<int> lit((size_t)w * h, -1);   // (the portal last glowing there: a pixel glows once per portal)
        for (size_t pi = 0; pi < p.portals.size(); pi++) {
            const Portal &po = p.portals[pi];
            for (size_t k = 1; k + 1 < po.points.size(); k++)
                raster_corners(po.points[0].data(), po.points[k].data(), po.points[k + 1].data(), [&](int x, int y, float z, float, float) {
                    size_t o = (size_t)y * w + x;
                    if (lit[o] == (int)pi || z >= zb[o] * 0.999f + 0.01f || z > far_z) return;
                    lit[o] = (int)pi;
                    blend(o, glow, 0.28);
                });
            // Outline: each edge, where nothing is in front of it.
            int thick = w >= 400 ? 1 : 0;
            for (size_t k = 0; k < po.points.size(); k++) {
                SV a = to_view(po.points[k].data()), b = to_view(po.points[(k + 1) % po.points.size()].data());
                if (a.z < near && b.z < near) continue;
                if (a.z < near) { double t = (near - a.z) / (b.z - a.z); a = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, near}; }
                if (b.z < near) { double t = (near - b.z) / (a.z - b.z); b = {b.x + (a.x - b.x) * t, b.y + (a.y - b.y) * t, near}; }
                double ax = (a.x / a.z / tx * 0.5 + 0.5) * w, ay = (0.5 - a.y / a.z / ty * 0.5) * h;
                double bx = (b.x / b.z / tx * 0.5 + 0.5) * w, by = (0.5 - b.y / b.z / ty * 0.5) * h;
                int steps = (int)std::min(4000.0, std::max(std::fabs(bx - ax), std::fabs(by - ay))) + 1;
                for (int i = 0; i <= steps; i++) {
                    double t = (double)i / steps;
                    double iz = (1 / a.z) * (1 - t) + (1 / b.z) * t;
                    float z = (float)(1 / iz);
                    int x = (int)(ax + (bx - ax) * t), y = (int)(ay + (by - ay) * t);
                    for (int dy = 0; dy <= thick; dy++)
                        for (int dx = 0; dx <= thick; dx++) {
                            int xx = x + dx, yy = y + dy;
                            if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                            size_t o = (size_t)yy * w + xx;
                            if (z > zb[o] * 1.002f + 0.03f) continue;
                            blend(o, edge, 0.9);
                        }
                }
            }
        }
    }
    if (total_out) *total_out = total;
}

// The file: a magic, then each part as counts and raw values (this machine's byte order: the file is a cache).
namespace {
const char kMagic[8] = {'H', 'M', 'P', 'L', 'S', 'H', 'D', '4'};
struct Out {
    std::ofstream f;
    template <class T> void raw(const T &v) { f.write((const char *)&v, sizeof v); }
    void str(const std::string &s) { raw((uint32_t)s.size()); f.write(s.data(), (std::streamsize)s.size()); }
    template <class T> void vec(const std::vector<T> &v) { raw((uint32_t)v.size()); if (!v.empty()) f.write((const char *)v.data(), (std::streamsize)(v.size() * sizeof(T))); }
};
struct In {
    std::ifstream f;
    bool ok = true;
    template <class T> void raw(T &v) { if (!f.read((char *)&v, sizeof v)) ok = false; }
    void str(std::string &s) {
        uint32_t n = 0;
        raw(n);
        if (!ok || n > (1u << 20)) { ok = false; return; }
        s.resize(n);
        if (n && !f.read(&s[0], n)) ok = false;
    }
    template <class T> void vec(std::vector<T> &v) {
        uint32_t n = 0;
        raw(n);
        if (!ok || n > (1u << 28) / sizeof(T)) { ok = false; return; }
        v.resize(n);
        if (n && !f.read((char *)v.data(), (std::streamsize)(n * sizeof(T)))) ok = false;
    }
};
}   // namespace

bool save(const Analysis &a, const Preview &p, const fs::path &file) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    Out o;
    o.f.open(file, std::ios::binary | std::ios::trunc);
    if (!o.f) return false;
    o.f.write(kMagic, 8);
    o.str(a.map_name);
    o.raw((uint32_t)a.shaders.size());
    for (auto &s : a.shaders) {
        o.str(s.tag);
        o.str(s.cube_map);
        o.raw(s.triangles); o.raw(s.extra_passes); o.raw(s.reflection_passes);
        o.raw(s.perpendicular); o.raw(s.parallel); o.raw(s.drawn); o.raw(s.shows); o.raw(s.strength); o.raw(s.coverage);
        o.raw((uint8_t)s.suggested);
    }
    o.raw(a.views); o.raw(a.rendered);
    o.vec(a.drawn); o.vec(a.other); o.vec(a.shows);
    o.vec(p.triangles);
    o.vec(p.materials);
    o.raw((uint32_t)p.textures.size());
    for (auto &t : p.textures) { o.raw(t.w); o.raw(t.h); o.vec(t.rgba); }
    o.raw((uint32_t)p.views.size());
    for (auto &v : p.views) { o.raw(v.eye); o.raw(v.yaw); }
    o.raw(p.rooms);
    o.vec(p.room);
    o.raw((uint32_t)p.portals.size());
    for (auto &po : p.portals) { o.raw(po.front); o.raw(po.back); o.vec(po.points); }
    o.vec(p.sky_fog);
    o.raw(p.indoor_fog);
    o.vec(p.room_sky);
    o.vec(p.tree);
    o.vec(p.leaf_room);
    return (bool)o.f;
}

bool load(Analysis &a, Preview &p, const fs::path &file) {
    a = Analysis();
    p = Preview();
    In in;
    in.f.open(file, std::ios::binary);
    char magic[8];
    if (!in.f || !in.f.read(magic, 8) || memcmp(magic, kMagic, 8)) return false;
    in.str(a.map_name);
    uint32_t n = 0;
    in.raw(n);
    if (!in.ok || n > 100000) return false;
    a.shaders.resize(n);
    for (auto &s : a.shaders) {
        in.str(s.tag);
        in.str(s.cube_map);
        in.raw(s.triangles); in.raw(s.extra_passes); in.raw(s.reflection_passes);
        in.raw(s.perpendicular); in.raw(s.parallel); in.raw(s.drawn); in.raw(s.shows); in.raw(s.strength); in.raw(s.coverage);
        uint8_t sg = 0;
        in.raw(sg);
        s.suggested = sg != 0;
    }
    in.raw(a.views); in.raw(a.rendered);
    in.vec(a.drawn); in.vec(a.other); in.vec(a.shows);
    in.vec(p.triangles);
    in.vec(p.materials);
    in.raw(n);
    if (!in.ok || n > 100000) return false;
    p.textures.resize(n);
    for (auto &t : p.textures) { in.raw(t.w); in.raw(t.h); in.vec(t.rgba); }
    in.raw(n);
    if (!in.ok || n > 100000) return false;
    p.views.resize(n);
    for (auto &v : p.views) { in.raw(v.eye); in.raw(v.yaw); }
    in.raw(p.rooms);
    in.vec(p.room);
    in.raw(n);
    if (!in.ok || n > 100000) return false;
    p.portals.resize(n);
    for (auto &po : p.portals) { in.raw(po.front); in.raw(po.back); in.vec(po.points); }
    in.vec(p.sky_fog);
    in.raw(p.indoor_fog);
    in.vec(p.room_sky);
    in.vec(p.tree);
    in.vec(p.leaf_room);
    return in.ok && a.drawn.size() == (size_t)a.views * a.shaders.size();
}

bool drop_reflection(const fs::path &shader) {
    tagfile::EditTag et;
    if (!et.load(shader)) return false;
    tagfile::TagNode &r = et.root();
    if (!tagfile::set_num(r, "perpendicular brightness", 0) || !tagfile::set_num(r, "parallel brightness", 0)) return false;
    if (std::string *cube = tagfile::ref_path(r, "reflection cube map")) cube->clear();
    return et.save(shader);
}

}   // namespace lshade
