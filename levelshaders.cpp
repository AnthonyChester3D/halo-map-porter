// Halo Map Porter - the level's shaders on the Xbox (see levelshaders.hpp)
#include "levelshaders.hpp"
#include "sky.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <set>

#include "bcdec.h"
#include "perf.hpp"
#include "tagfile.hpp"

namespace lshade {

namespace fs = std::filesystem;
using V3 = std::array<double, 3>;

namespace {

// An item spawn point with every game type "none": no game spawns anything there (the bots' hidden waypoints).
bool never_spawns(const tagfile::Elem &e) {
    for (const char *f : {"type 0", "type 1", "type 2", "type 3"})
        if (e.integer(f) != 0) return false;
    return true;
}

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
    bool dxt = fmt == 14 || fmt == 15 || fmt == 16 || fmt == 18;   // (18: MCC's BC7)
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
                    else if (fmt == 18) bcdec_bc7(src, block, 16);
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
                case 1: case 2: q[0] = q[1] = q[2] = p[i]; break;
                case 17: q[0] = q[1] = q[2] = p[i]; q[3] = p[i] >= 249 ? 0 : 255; break;   // (P8: the Xbox palette's last 7 are see-through)
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

// Whether a triangle's cut-out (its material's, Material::cutout) cuts it away at barycentrics (l1, l2) of its 2nd and
// 3rd corners: drawn only where the alpha is over half (the Xbox's alpha reference 0x7F, "greater").
bool cut_away(const Preview &p, const Triangle &t, double l1, double l2) {
    const Material &m = p.materials[(size_t)t.material];
    if (m.cutout < 0 || (size_t)m.cutout >= p.textures.size()) return false;
    double l0 = 1 - l1 - l2;
    double u = t.v[0].uv[0] * l0 + t.v[1].uv[0] * l1 + t.v[2].uv[0] * l2, v = t.v[0].uv[1] * l0 + t.v[1].uv[1] * l1 + t.v[2].uv[1] * l2;
    float c[4];
    sample(p.textures[(size_t)m.cutout], u * m.cutout_scale, v * m.cutout_scale, true, c);
    return c[3] * 255 <= 127;
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

namespace {
// A model's box (the corners of every geometry's vertices), in its own space; false if it has none.
bool model_box(const fs::path &file, float lo[3], float hi[3]) {
    tagfile::Tag t;
    if (!t.load(file)) return false;
    for (int k = 0; k < 3; k++) { lo[k] = 1e30f; hi[k] = -1e30f; }
    tagfile::Block geos = t.root().block("geometries");
    for (size_t g = 0; g < geos.size(); g++) {
        tagfile::Block parts = geos[g].block("parts");
        for (size_t q = 0; q < parts.size(); q++) {
            tagfile::Block vs = parts[q].block("uncompressed vertices");
            if (!vs.size()) vs = parts[q].block("compressed vertices");
            for (size_t v = 0; v < vs.size(); v++)
                for (int k = 0; k < 3; k++) {
                    float x = (float)vs[v].num("position", k);
                    lo[k] = std::min(lo[k], x);
                    hi[k] = std::max(hi[k], x);
                }
        }
    }
    return lo[0] <= hi[0];
}
}   // namespace

// The scenario's CTF flags, power items and placed objects' boxes (Preview::marks, boxes).
static void read_marks(const tagfile::Elem &sc, const Find &find, Preview &p) {
    tagfile::Block flags = sc.block("netgame flags");
    for (size_t i = 0; i < flags.size(); i++) {
        long long type = flags[i].integer("type");
        Mark m;
        for (int k = 0; k < 3; k++) m.p[k] = (float)flags[i].num("position", k);
        m.index = (int32_t)flags[i].integer("usage id");
        if (type == 0) {   // (a CTF flag's spot)
            m.kind = Mark::kFlag;
            snprintf(m.name, sizeof m.name, "%s flag", flags[i].integer("usage id") == 1 ? "blue" : "red");   // (CTF: 0 red, 1 blue)
        } else {
            static const char *const kTypes[] = {"flag", "flag vehicle", "oddball", "race point", "race vehicle", "bank",
                                                 "teleporter", "teleporter exit", "hill"};
            m.kind = Mark::kPlace;
            snprintf(m.name, sizeof m.name, "%s", type >= 0 && type < 9 ? kTypes[type] : "place");
        }
        p.marks.push_back(m);
    }
    tagfile::Block starts = sc.block("player starting locations");
    for (size_t i = 0; i < starts.size(); i++) {
        Mark m;
        for (int k = 0; k < 3; k++) m.p[k] = (float)starts[i].num("position", k);
        m.kind = Mark::kSpawn;
        snprintf(m.name, sizeof m.name, "spawn");
        p.marks.push_back(m);
    }
    {
        tagfile::Block veh = sc.block("vehicles"), vpal = sc.block("vehicle palette");
        for (size_t i = 0; i < veh.size(); i++) {
            long long t = veh[i].integer("type");
            if (t < 0 || (size_t)t >= vpal.size() || veh[i].integer("multiplayer spawn flags") == 0) continue;
            Mark m;
            for (int k = 0; k < 3; k++) m.p[k] = (float)veh[i].num("position", k);
            m.kind = Mark::kVehicle;
            snprintf(m.name, sizeof m.name, "%s", fs::path(vpal[(size_t)t].ref("name")).stem().string().c_str());
            p.marks.push_back(m);
        }
    }
    static const char *const kPower[] = {"rocket", "sniper", "fuel", "flame", "overshield", "over shield", "camo", "invis"};
    tagfile::Block eq = sc.block("netgame equipment");
    for (size_t i = 0; i < eq.size(); i++) {
        if (never_spawns(eq[i])) continue;
        std::string ref = eq[i].ref("item collection"), stem = fs::path(ref).stem().string(), low = stem;
        for (auto &c : low) c = (char)std::tolower((unsigned char)c);
        bool power = false;
        for (const char *w : kPower) power = power || low.find(w) != std::string::npos;
        Mark m;
        for (int k = 0; k < 3; k++) m.p[k] = (float)eq[i].num("position", k);
        m.kind = power ? Mark::kPower : Mark::kItem;
        snprintf(m.name, sizeof m.name, "%s", stem.c_str());
        p.marks.push_back(m);
    }
    static const char *const kinds[][2] = {{"scenery", "scenery palette"}, {"machines", "machine palette"},
                                           {"controls", "control palette"}, {"light fixtures", "light fixture palette"}};
    std::map<std::string, std::array<float, 6>> box_of;   // object -> its model's box (lo, hi; empty: none)
    for (auto &k : kinds) {
        tagfile::Block items = sc.block(k[0]), pal = sc.block(k[1]);
        for (size_t i = 0; i < items.size(); i++) {
            long long t = items[i].integer("type");
            if (t < 0 || (size_t)t >= pal.size()) continue;
            std::string o = pal[(size_t)t].ref("name");
            if (o.empty()) continue;
            auto it = box_of.find(o);
            if (it == box_of.end()) {
                std::array<float, 6> b{1, 1, 1, 0, 0, 0};   // (lo > hi: none)
                fs::path of = find(o);
                tagfile::Tag ot;
                if (!of.empty() && ot.load(of)) {
                    std::string m = ot.root().ref("model");
                    fs::path mf = m.empty() ? fs::path() : find(m);
                    float lo[3], hi[3];
                    if (!mf.empty() && model_box(mf, lo, hi)) b = {lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]};
                }
                it = box_of.emplace(o, b).first;
            }
            const auto &b = it->second;
            if (b[0] > b[3]) continue;
            double yaw = items[i].num("rotation", 0), pitch = items[i].num("rotation", 1), roll = items[i].num("rotation", 2);
            Box x;
            double cx = (b[0] + b[3]) / 2, cy = (b[1] + b[4]) / 2, cz = (b[2] + b[5]) / 2;
            double hx = (b[3] - b[0]) / 2, hy = (b[4] - b[1]) / 2, hz = (b[5] - b[2]) / 2;
            if (std::fabs(pitch) > 0.05 || std::fabs(roll) > 0.05) {   // (tipped over: a cube around it, upright)
                double r = std::sqrt(hx * hx + hy * hy + hz * hz);
                hx = hy = hz = r;
            }
            double c = std::cos(yaw), s = std::sin(yaw);
            x.c[0] = (float)(items[i].num("position", 0) + c * cx - s * cy);
            x.c[1] = (float)(items[i].num("position", 1) + s * cx + c * cy);
            x.c[2] = (float)(items[i].num("position", 2) + cz);
            x.axis[0][0] = (float)c; x.axis[0][1] = (float)s;
            x.axis[1][0] = (float)-s; x.axis[1][1] = (float)c;
            x.half[0] = (float)hx; x.half[1] = (float)hy; x.half[2] = (float)hz;
            p.boxes.push_back(x);
        }
    }
}

const char *object_kind(int kind) {
    static const char *const k[Object::kKinds] = {"scenery", "a machine", "a control", "a light fixture", "a vehicle",
                                                  "a weapon", "equipment", "an item on a spawn point"};
    return kind >= 0 && kind < Object::kKinds ? k[kind] : "an object";
}

void ObjectLight::at(const float n[3], float out[3]) const {
    float f[2];
    for (int i = 0; i < 2; i++)
        f[i] = std::max(0.0f, -(n[0] * direction[i][0] + n[1] * direction[i][1] + n[2] * direction[i][2]));
    for (int k = 0; k < 3; k++) out[k] = ambient[k] + color[0][k] * f[0] + color[1][k] * f[1];
}

namespace {

double or_one(double v) { return v == 0 || !std::isfinite(v) ? 1.0 : v; }

// A model tag (.gbxmodel or .model) as a Model: its most detailed geometry (each region's first permutation's super
// high), its shaders' base maps put in `tex`. False when it has no triangles.
bool read_model(const std::string &tag, const Find &find, Textures &tex, Model &out) {
    out = Model();
    out.tag = tag;
    fs::path mf = find(tag);
    tagfile::Tag t;
    if (mf.empty() || !t.load(mf)) return false;
    tagfile::Elem r = t.root();
    double mu = or_one(r.num("base map u scale")), mv = or_one(r.num("base map v scale"));
    tagfile::Block geos = r.block("geometries"), regions = r.block("regions"), shaders = r.block("shaders");
    std::vector<int> use;
    for (size_t i = 0; i < regions.size(); i++) {
        tagfile::Block perms = regions[i].block("permutations");
        if (!perms.size()) continue;
        long long g = perms[0].integer("super high");
        if (g >= 0 && g < (long long)geos.size() && std::find(use.begin(), use.end(), (int)g) == use.end()) use.push_back((int)g);
    }
    // Each shader: its base map, how it repeats, how it's drawn (rasterizer_xbox_models.c: a shader_model is
    // alpha-tested by its base map's alpha unless it says it isn't, or it's an alpha-blended decal).
    struct Look { int texture = -1, flags = 0, index = -1; double u = 1, v = 1; bool done = false; };
    std::vector<Look> looks(shaders.size());
    auto look = [&](long long si) -> Look {
        if (si < 0 || (size_t)si >= looks.size()) return Look();
        Look &l = looks[(size_t)si];
        if (l.done) return l;
        l.done = true;
        std::string group, ref = shaders[(size_t)si].ref("shader", &group);
        std::string st = with_extension(ref, group, find);
        l.index = (int)out.shaders.size();
        out.shaders.push_back(st.empty() ? ref : st);
        tagfile::Tag sh;
        fs::path sf = st.empty() ? fs::path() : find(st);
        if (sf.empty() || !sh.load(sf)) return l;
        tagfile::Elem e = sh.root();
        std::string ext = fs::path(st).extension().u8string(), base;
        if (ext == ".shader_model") {
            base = bitmap_ref(e, "base map");
            l.u = or_one(e.num("map u scale"));
            l.v = or_one(e.num("map v scale"));
            long long f = (long long)e.num("shader model flags");
            if (f & 2) l.flags |= ModelPart::kTwoSided;
            if (f & 8) l.flags |= ModelPart::kSeeThrough;
            else if (!(f & 4)) l.flags |= ModelPart::kCutout;
        } else if (ext == ".shader_environment") {
            base = bitmap_ref(e, "base map");
        } else {
            l.flags |= ModelPart::kSeeThrough | ModelPart::kTwoSided;
            if (ext == ".shader_transparent_chicago" || ext == ".shader_transparent_chicago_extended" ||
                ext == ".shader_transparent_generic") {
                // (0 alpha blend, 1 multiply, 2 double multiply, 3 add, 4 subtract, 5 min, 6 max, 7 alpha-multiply add)
                long long f = (long long)e.num("framebuffer blend function");
                l.flags |= f == 3 || f == 7 ? ModelPart::kAdd : f == 1 ? ModelPart::kMultiply : f == 2 ? ModelPart::kDoubleMultiply : 0;
            }
            tagfile::Block maps = e.block(ext == ".shader_transparent_chicago_extended" ? "maps 4 stage" : "maps");
            if (!maps.size() && ext == ".shader_transparent_chicago_extended") maps = e.block("maps 2 stage");
            if (maps.size()) {
                base = bitmap_ref(maps[0], "map");
                l.u = or_one(maps[0].num("map u scale"));
                l.v = or_one(maps[0].num("map v scale"));
            }
        }
        l.texture = tex.get(base, 0, 128);
        if ((l.flags & ModelPart::kCutout) && l.texture >= 0) {   // (nothing to cut when no texel is half see-through)
            const std::vector<uint8_t> &px = tex.p.textures[(size_t)l.texture].rgba;
            bool any = false;
            for (size_t i = 3; i < px.size() && !any; i += 4) any = px[i] <= 127;
            if (!any) l.flags &= ~ModelPart::kCutout;
        } else if (l.texture < 0) {
            l.flags &= ~ModelPart::kCutout;
        }
        return l;
    };
    for (int g : use) {
        tagfile::Block parts = geos[(size_t)g].block("parts");
        for (size_t k = 0; k < parts.size(); k++) {
            tagfile::Block verts = parts[k].block("uncompressed vertices"), strip = parts[k].block("triangles");
            if (!verts.size() || !strip.size()) continue;
            Look l = look(parts[k].integer("shader index"));
            uint32_t base = (uint32_t)out.vertices.size();
            for (size_t v = 0; v < verts.size(); v++) {
                ModelVertex x;
                for (int c = 0; c < 3; c++) {
                    x.p[c] = (float)verts[v].num("position", c);
                    x.n[c] = (float)verts[v].num("normal", c);
                }
                x.uv[0] = (float)(verts[v].num("texture coords", 0) * mu * l.u);
                x.uv[1] = (float)(verts[v].num("texture coords", 1) * mv * l.v);
                out.vertices.push_back(x);
            }
            ModelPart part;
            part.first = (int32_t)(out.tris.size() / 3);
            part.texture = l.texture;
            part.flags = l.flags;
            part.shader = l.index;
            std::vector<int> s;
            for (size_t i = 0; i < strip.size(); i++) {
                static const char *f[3] = {"vertex0 index", "vertex1 index", "vertex2 index"};
                for (int c = 0; c < 3; c++) {
                    int x = (int)strip[i].integer(f[c]);
                    if (x != 65535) s.push_back(x);
                }
            }
            const int n = (int)verts.size();
            for (size_t i = 2; i < s.size(); i++) {
                int a = s[i - 2], b = s[i - 1], c = s[i];
                if (a == b || b == c || a == c || a < 0 || b < 0 || c < 0 || a >= n || b >= n || c >= n) continue;
                // (wound counterclockwise seen from the front: the side its corners' normals face)
                const ModelVertex &A = out.vertices[base + a], &B = out.vertices[base + b], &C = out.vertices[base + c];
                V3 e1{B.p[0] - A.p[0], B.p[1] - A.p[1], B.p[2] - A.p[2]}, e2{C.p[0] - A.p[0], C.p[1] - A.p[1], C.p[2] - A.p[2]};
                V3 nn{A.n[0] + B.n[0] + C.n[0], A.n[1] + B.n[1] + C.n[1], A.n[2] + B.n[2] + C.n[2]};
                bool flip = dot(cross(e1, e2), nn) < 0;
                if (dot(nn, nn) < 1e-12) flip = (i - 2) % 2 == 1;
                if (flip) std::swap(b, c);
                out.tris.insert(out.tris.end(), {base + a, base + b, base + c});
            }
            part.count = (int32_t)(out.tris.size() / 3) - part.first;
            if (part.count) out.parts.push_back(part);
        }
    }
    return !out.tris.empty();
}

// The level's triangles in a grid by where they are across (x, y), for what's straight below a point.
struct Below {
    const Preview &p;
    double x0 = 0, y0 = 0, cell = 4;
    int nx = 0, ny = 0;
    std::vector<std::vector<int>> cells;
    explicit Below(const Preview &pv) : p(pv) {
        double lo[2] = {1e30, 1e30}, hi[2] = {-1e30, -1e30};
        for (auto &t : p.triangles)
            if (t.material >= 0)
                for (auto &v : t.v)
                    for (int k = 0; k < 2; k++) { lo[k] = std::min(lo[k], (double)v.p[k]); hi[k] = std::max(hi[k], (double)v.p[k]); }
        if (lo[0] > hi[0]) return;
        cell = std::max({4.0, (hi[0] - lo[0]) / 512, (hi[1] - lo[1]) / 512});
        x0 = lo[0];
        y0 = lo[1];
        nx = (int)((hi[0] - lo[0]) / cell) + 1;
        ny = (int)((hi[1] - lo[1]) / cell) + 1;
        cells.assign((size_t)nx * ny, {});
        for (size_t i = 0; i < p.triangles.size(); i++) {
            const Triangle &t = p.triangles[i];
            if (t.material < 0) continue;
            double a[2] = {1e30, 1e30}, b[2] = {-1e30, -1e30};
            for (auto &v : t.v)
                for (int k = 0; k < 2; k++) { a[k] = std::min(a[k], (double)v.p[k]); b[k] = std::max(b[k], (double)v.p[k]); }
            int cx0 = std::clamp((int)((a[0] - x0) / cell), 0, nx - 1), cx1 = std::clamp((int)((b[0] - x0) / cell), 0, nx - 1);
            int cy0 = std::clamp((int)((a[1] - y0) / cell), 0, ny - 1), cy1 = std::clamp((int)((b[1] - y0) / cell), 0, ny - 1);
            for (int y = cy0; y <= cy1; y++)
                for (int x = cx0; x <= cx1; x++) cells[(size_t)y * nx + x].push_back((int)i);
        }
    }
    // The first level surface a ray from `q` straight down `reach` units meets from its front (as the engine's
    // structure_test_vector: going into the level's solid): its triangle and the barycentrics of its 2nd and 3rd
    // corners there; -1 for none.
    int hit(const V3 &q, double reach, double &l1, double &l2) const {
        if (!nx) return -1;
        int cx = (int)std::floor((q[0] - x0) / cell), cy = (int)std::floor((q[1] - y0) / cell);
        if (cx < 0 || cy < 0 || cx >= nx || cy >= ny) return -1;
        int best = -1;
        double best_z = q[2] - reach;
        for (int i : cells[(size_t)cy * nx + cx]) {
            const Triangle &t = p.triangles[(size_t)i];
            const float *a = t.v[0].p, *b = t.v[1].p, *c = t.v[2].p;
            double e1x = b[0] - a[0], e1y = b[1] - a[1], e2x = c[0] - a[0], e2y = c[1] - a[1];
            double det = e1x * e2y - e2x * e1y;   // (its normal's z, as Halo winds it: negative when it faces up)
            if (det >= -1e-12) continue;
            double px = q[0] - a[0], py = q[1] - a[1];
            double u = (px * e2y - e2x * py) / det, v = (e1x * py - px * e1y) / det;
            if (u < 0 || v < 0 || u + v > 1) continue;
            double z = a[2] + (b[2] - a[2]) * u + (c[2] - a[2]) * v;
            if (z > q[2] || z <= best_z) continue;
            best = i;
            best_z = z;
            l1 = u;
            l2 = v;
        }
        return best;
    }
};

// brighten_real_rgb_color (object_lights.c): its brightest channel `fraction` brighter, up to 1, at least `fraction`.
void brighten(float c[3], float fraction) {
    float m = std::max({c[0], c[1], c[2]});
    if (m <= 0) return;
    float scale = fraction + 1, sm = scale * m;
    if (sm > 1) scale = 1 / m;
    else if (sm < fraction) scale = fraction / m;
    for (int k = 0; k < 3; k++) c[k] *= scale;
}

// The level's lighting for objects (lights_distant_lighting_at_point, build_distant_lights in object_lights.c).
struct Lighting {
    const Preview &p;
    const Below &below;
    const std::vector<std::array<float, 9>> &incident;   // per triangle: its corners' incident radiosity (the lightmap's
                                                         // light: where it comes from, as long as it's that directional)
    ObjectLight fallback;                                // the level's default lighting, else the engine's
    // At a point: the light from the lightmap below it, or false (and the fallback) when the ray meets no lit surface.
    bool at(const V3 &q, bool bright, ObjectLight &out) const {
        out = fallback;
        double l1 = 0, l2 = 0;
        int ti = below.hit(q, 10, l1, l2);
        if (ti < 0) return false;
        const Triangle &t = p.triangles[(size_t)ti];
        const Material &m = p.materials[(size_t)t.material];
        if (m.shader < 0 || m.base < 0 || t.lightmap < 0) return false;   // (a shader_environment with a base map and a lightmap)
        double l0 = 1 - l1 - l2;
        auto lerp = [&](const float *a, const float *b, const float *c, int k) { return a[k] * l0 + b[k] * l1 + c[k] * l2; };
        float lm[4], diffuse[4];
        sample(p.textures[(size_t)t.lightmap], lerp(t.v[0].luv, t.v[1].luv, t.v[2].luv, 0), lerp(t.v[0].luv, t.v[1].luv, t.v[2].luv, 1), false, lm);
        sample(p.textures[(size_t)m.base], lerp(t.v[0].uv, t.v[1].uv, t.v[2].uv, 0), lerp(t.v[0].uv, t.v[1].uv, t.v[2].uv, 1), true, diffuse);
        V3 surface = norm(V3{lerp(t.v[0].n, t.v[1].n, t.v[2].n, 0), lerp(t.v[0].n, t.v[1].n, t.v[2].n, 1), lerp(t.v[0].n, t.v[1].n, t.v[2].n, 2)});
        V3 rn{0, 0, 0};
        double accuracy = 0;
        const auto &inc = incident[(size_t)ti];
        const double w[3] = {l0, l1, l2};
        for (int k = 0; k < 3; k++) {
            V3 d{inc[k * 3], inc[k * 3 + 1], inc[k * 3 + 2]};
            double len = std::sqrt(dot(d, d));
            if (len > 1e-12) d = mul(d, 1 / len);
            rn = {rn[0] + d[0] * w[k], rn[1] + d[1] * w[k], rn[2] + d[2] * w[k]};
            accuracy += len * w[k];
        }
        rn = norm(rn);
        (void)accuracy;   // (the engine uses it for the object's shadow only)
        float brightness = lm[0] * 0.299f + lm[1] * 0.587f + lm[2] * 0.114f;
        for (int k = 0; k < 3; k++) {
            out.ambient[k] = lm[k] * 0.4f + 0.03f;
            out.color[0][k] = lm[k];
            out.direction[0][k] = (float)-rn[k];
            out.color[1][k] = diffuse[k] * brightness;
            out.direction[1][k] = (float)surface[k];
        }
        if (bright) {
            brighten(out.ambient, 0.2f);
            brighten(out.color[0], 0.3f);
            brighten(out.color[1], 0.2f);
        }
        return true;
    }
    // An object's (lights_prepare_for_object_static): from its bounding sphere's centre and four points around it
    // (the radius x 0.707 away in x and y), the ones that meet a lit surface averaged. Its samples in `lit`.
    ObjectLight object(const V3 &center, double radius, bool bright, int &lit) const {
        ObjectLight light, s;
        lit = 0;
        if (at(center, bright, light)) lit = 1;
        else light = ObjectLight{{0, 0, 0}, {{0, 0, 0}, {0, 0, 0}}, {{0, 0, 0}, {0, 0, 0}}};
        for (int corner = 0; corner < 4; corner++) {
            V3 q{center[0] + ((corner & 1) ? 0.70710678 : -0.70710678) * radius,
                 center[1] + ((corner & 2) ? 0.70710678 : -0.70710678) * radius, center[2]};
            if (!at(q, bright, s)) continue;
            lit++;
            for (int k = 0; k < 3; k++) {
                light.ambient[k] += s.ambient[k];
                for (int i = 0; i < 2; i++) {
                    light.color[i][k] += s.color[i][k];
                    light.direction[i][k] += s.direction[i][k];
                }
            }
        }
        if (lit > 1) {
            float scale = 1.0f / lit;
            for (int k = 0; k < 3; k++) {
                light.ambient[k] *= scale;
                for (int i = 0; i < 2; i++) light.color[i][k] *= scale;
            }
            for (int i = 0; i < 2; i++) {
                V3 d = norm(V3{light.direction[i][0], light.direction[i][1], light.direction[i][2]});
                for (int k = 0; k < 3; k++) light.direction[i][k] = (float)d[k];
            }
        } else if (lit == 0) {
            light = s;
        }
        return light;
    }
};

// The objects in the level when a game starts (Preview::objects, models), lit by its lightmap.
void read_objects(const tagfile::Elem &sc, const tagfile::Elem &bsp, const Find &find, Textures &tex,
                  const std::vector<std::array<float, 9>> &incident, Preview &p) {
    Below below(p);
    Lighting lighting{p, below, incident, ObjectLight()};
    if (bsp.num("default ambient color", 0) != 0) {   // (the level's own default lighting, when it has one)
        ObjectLight &f = lighting.fallback;
        for (int k = 0; k < 3; k++) {
            f.ambient[k] = (float)bsp.num("default ambient color", k);
            f.color[0][k] = (float)bsp.num("default distant light 0 color", k);
            f.direction[0][k] = (float)bsp.num("default distant light 0 direction", k);
            f.color[1][k] = (float)bsp.num("default distant light 1 color", k);
            f.direction[1][k] = (float)bsp.num("default distant light 1 direction", k);
        }
    }
    std::map<std::string, int> model_of;   // object tag -> its model (-1: none)
    struct Def { int model = -1; double radius = 0; V3 offset{}; bool bright = false; };
    std::map<std::string, Def> defs;
    auto def = [&](const std::string &o) -> const Def & {
        auto it = defs.find(o);
        if (it != defs.end()) return it->second;
        Def d;
        fs::path of = find(o);
        tagfile::Tag ot;
        if (!of.empty() && ot.load(of)) {
            tagfile::Elem r = ot.root();
            d.radius = r.num("bounding radius");
            for (int k = 0; k < 3; k++) d.offset[k] = r.num("bounding offset", k);
            d.bright = ((long long)r.num("flags") & 4) != 0;   // (artificially bright)
            std::string m = r.ref("model");
            if (!m.empty()) {
                auto mt = model_of.find(m);
                if (mt == model_of.end()) {
                    Model model;
                    int id = -1;
                    if (p.models.size() < 1000 && read_model(m, find, tex, model)) {
                        id = (int)p.models.size();
                        p.models.push_back(std::move(model));
                    }
                    mt = model_of.emplace(m, id).first;
                }
                d.model = mt->second;
            }
        }
        return defs.emplace(o, d).first->second;
    };
    auto place = [&](const std::string &o, int kind, const V3 &pos, double yaw, double pitch, double roll) {
        if (o.empty() || p.objects.size() >= 5000) return;
        const Def &d = def(o);
        if (d.model < 0) return;
        Object x;
        x.model = d.model;
        x.kind = kind;
        double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch), cr = std::cos(roll), sr = std::sin(roll);
        V3 f{cy * cp, sy * cp, sp}, l0{-sy, cy, 0}, u0{-sp * cy, -sp * sy, cp};
        V3 l{l0[0] * cr + u0[0] * sr, l0[1] * cr + u0[1] * sr, l0[2] * cr + u0[2] * sr};
        V3 u{u0[0] * cr - l0[0] * sr, u0[1] * cr - l0[1] * sr, u0[2] * cr - l0[2] * sr};
        const V3 axes[3] = {f, l, u};
        for (int i = 0; i < 3; i++)
            for (int k = 0; k < 3; k++) x.axes[i][k] = (float)axes[i][k];
        for (int k = 0; k < 3; k++) x.p[k] = (float)pos[k];
        V3 center{pos[0] + f[0] * d.offset[0] + l[0] * d.offset[1] + u[0] * d.offset[2],
                  pos[1] + f[1] * d.offset[0] + l[1] * d.offset[1] + u[1] * d.offset[2],
                  pos[2] + f[2] * d.offset[0] + l[2] * d.offset[1] + u[2] * d.offset[2]};
        int lit = 0;
        x.light = lighting.object(center, d.radius, d.bright, lit);
        x.lit = lit;
        snprintf(x.tag, sizeof x.tag, "%s", o.c_str());
        p.objects.push_back(x);
    };
    static const char *const kinds[][2] = {{"scenery", "scenery palette"}, {"machines", "machine palette"},
                                           {"controls", "control palette"}, {"light fixtures", "light fixture palette"},
                                           {"vehicles", "vehicle palette"}, {"weapons", "weapon palette"},
                                           {"equipment", "equipment palette"}};
    for (int k = 0; k < 7; k++) {
        tagfile::Block items = sc.block(kinds[k][0]), pal = sc.block(kinds[k][1]);
        for (size_t i = 0; i < items.size(); i++) {
            long long t = items[i].integer("type");
            if (t < 0 || (size_t)t >= pal.size()) continue;
            if ((long long)items[i].num("not placed") & 1) continue;   // (not placed automatically: only by scripts)
            if (k == Object::kVehicle && items[i].integer("multiplayer spawn flags") == 0) continue;   // (no game type spawns it)
            V3 pos{items[i].num("position", 0), items[i].num("position", 1), items[i].num("position", 2)};
            place(pal[(size_t)t].ref("name"), k, pos, items[i].num("rotation", 0), items[i].num("rotation", 1), items[i].num("rotation", 2));
        }
    }
    // (the multiplayer spawn points' items: each one's collection's first)
    tagfile::Block eq = sc.block("netgame equipment");
    std::map<std::string, std::string> first_item;
    for (size_t i = 0; i < eq.size(); i++) {
        std::string ic = eq[i].ref("item collection");
        if (ic.empty() || never_spawns(eq[i])) continue;
        if (!first_item.count(ic)) {
            std::string &item = first_item[ic];
            fs::path f = find(ic);
            tagfile::Tag t;
            if (!f.empty() && t.load(f)) {
                tagfile::Block perms = t.root().block("permutations");
                for (size_t k = 0; k < perms.size() && item.empty(); k++) item = perms[k].ref("item");
            }
        }
        V3 pos{eq[i].num("position", 0), eq[i].num("position", 1), eq[i].num("position", 2)};
        place(first_item[ic], Object::kItem, pos, eq[i].num("facing"), 0, 0);
    }
}

}   // namespace

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
    std::vector<std::array<float, 9>> incident(surfaces.size(), std::array<float, 9>{});   // (for lighting objects)
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
                if ((flags & 1) && !(flags & 2)) {   // alpha tested (and the bump map isn't a specular mask)
                    m.cutout = tex.get(bitmap_ref(s, "bump map"), 0, 1024);
                    m.cutout_scale = (float)(s.num("bump map scale") > 0 ? s.num("bump map scale") : 1);
                }
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
        p.material_tags.push_back(tag);
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
            auto vertex = [&](size_t i, Vertex &v, float *inc) {
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
                if (have_lm) {   // (a lightmap vertex: its incident radiosity, then its lightmap coordinates)
                    size_t la = lbase + i * lstride;
                    if (compressed) {
                        int16_t u, w;
                        memcpy(&u, data.data() + la + 4, 2);
                        memcpy(&w, data.data() + la + 6, 2);
                        v.luv[0] = u / 32767.0f; v.luv[1] = w / 32767.0f;
                        uint32_t n;
                        memcpy(&n, data.data() + la, 4);
                        unpack(n, inc);
                    } else {
                        v.luv[0] = f32(la + 12); v.luv[1] = f32(la + 16);
                        for (int k = 0; k < 3; k++) inc[k] = f32(la + 4 * k);
                    }
                }
            };
            long long first = m.integer("surfaces"), n = m.integer("surface count");
            for (long long sfc = first; sfc < first + n && sfc >= 0 && sfc < (long long)surfaces.size(); sfc++) {
                size_t i0 = (size_t)surfaces[(size_t)sfc].integer("vertex0 index"), i1 = (size_t)surfaces[(size_t)sfc].integer("vertex1 index"),
                       i2 = (size_t)surfaces[(size_t)sfc].integer("vertex2 index");
                if (i0 >= count || i1 >= count || i2 >= count) continue;
                Triangle &tr = p.triangles[(size_t)sfc];
                float *inc = incident[(size_t)sfc].data();
                vertex(i0, tr.v[0], inc);
                vertex(i1, tr.v[1], inc + 3);
                vertex(i2, tr.v[2], inc + 6);
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
        // (the weather: the heaviest of the systems the level uses)
        tagfile::Block pal = r.block("weather palette");
        std::set<std::string> seen;
        for (size_t i = 0; i < pal.size(); i++) {
            std::string ref = pal[i].ref("particle system");
            if (ref.empty() || !seen.insert(ref).second) continue;
            fs::path file = find(ref);
            std::vector<perf::WeatherType> v = file.empty() ? std::vector<perf::WeatherType>() : perf::weather_types(file);
            if (v.empty()) continue;
            p.weather.systems++;
            float made = (float)perf::weather_drops(v);
            if (made < p.weather.made) continue;
            Weather w;
            w.systems = p.weather.systems;
            w.made = made;
            for (auto &t : v) w.end = std::max(w.end, (float)t.fade_out_end);
            std::vector<perf::WeatherType> l = v, h = v;
            w.lighter = perf::weather_lighten(l, 1) ? (float)perf::weather_drops(l) : made;
            for (auto &t : l) w.lighter_end = std::max(w.lighter_end, (float)t.fade_out_end);
            w.half = perf::weather_lighten(h, 2) ? (float)perf::weather_drops(h) : made;
            p.weather = w;
        }
        // (the scripts' riders, and the small props with very detailed collision)
        tagfile::Tag sc;
        if (!scenario.empty() && sc.load(scenario)) {
            std::string src;
            tagfile::Block files = sc.root().block("source files");
            for (size_t i = 0; i < files.size(); i++) src += files[i].data("source") + "\n";
            p.content.riders = (int32_t)perf::script_riders(src).size();
            tagfile::Block placed = sc.root().block("scenery"), spal = sc.root().block("scenery palette");
            std::map<long long, int> count;
            for (size_t i = 0; i < placed.size(); i++) count[placed[i].integer("type")]++;
            std::string names;
            for (auto &[k, n] : count) {
                if (k < 0 || (size_t)k >= spal.size()) continue;
                std::string ref = spal[(size_t)k].ref("name");
                fs::path of = ref.empty() ? fs::path() : find(ref);
                tagfile::Tag ot;
                if (of.empty() || !ot.load(of)) continue;
                std::string coll = ot.root().ref("collision model");
                if (coll.empty() || ot.root().num("bounding radius") > perf::kDetailedPropRadius) continue;
                fs::path cf = find(coll);
                int surfaces = cf.empty() ? 0 : perf::collision_surfaces(cf);
                if (surfaces < perf::kDetailedPropSurfaces) continue;
                p.content.detailed_props += n;
                names += (names.empty() ? "" : ", ") + fs::path(ref).stem().string() + " (" + std::to_string(surfaces) + " surfaces)";
            }
            snprintf(p.content.prop_names, sizeof p.content.prop_names, "%s", names.c_str());
            // (the dynamic lights on placed objects: attached, or made by an attached effect, which always is dynamic)
            std::map<std::string, int> lights;   // light tag -> objects carrying it
            static const char *const kinds[][2] = {{"scenery", "scenery palette"}, {"machines", "machine palette"},
                                                   {"controls", "control palette"}, {"light fixtures", "light fixture palette"}};
            std::map<std::string, std::vector<std::string>> of_object;
            for (auto &k : kinds) {
                tagfile::Block items = sc.root().block(k[0]), pal = sc.root().block(k[1]);
                for (size_t i = 0; i < items.size(); i++) {
                    long long t = items[i].integer("type");
                    if (t < 0 || (size_t)t >= pal.size()) continue;
                    std::string o = pal[(size_t)t].ref("name");
                    if (o.empty()) continue;
                    if (!of_object.count(o)) {
                        std::vector<std::string> &v = of_object[o];
                        fs::path of = find(o);
                        tagfile::Tag ot;
                        if (of.empty() || !ot.load(of)) continue;
                        tagfile::Block at = ot.root().block("attachments");
                        for (size_t a = 0; a < at.size(); a++) {
                            std::string ref = at[a].ref("type");
                            fs::path lf = ref.empty() ? fs::path() : find(ref);
                            if (lf.empty()) continue;
                            if (ref.size() > 6 && ref.compare(ref.size() - 6, 6, ".light") == 0) {
                                tagfile::Tag lt;
                                if (lt.load(lf) && ((long long)lt.root().num("flags") & 1) && lt.root().num("radius") > 0) v.push_back(ref);
                            } else if (ref.size() > 7 && ref.compare(ref.size() - 7, 7, ".effect") == 0) {
                                tagfile::Tag et;
                                if (!et.load(lf)) continue;
                                tagfile::Block ev = et.root().block("events");
                                for (size_t e = 0; e < ev.size(); e++) {
                                    tagfile::Block parts = ev[e].block("parts");
                                    for (size_t q = 0; q < parts.size(); q++) {
                                        std::string l = parts[q].ref("type");
                                        fs::path f2 = l.empty() ? fs::path() : find(l);
                                        tagfile::Tag lt;
                                        if (l.size() > 6 && l.compare(l.size() - 6, 6, ".light") == 0 && !f2.empty() && lt.load(f2) &&
                                            lt.root().num("radius") > 0 && std::find(v.begin(), v.end(), l) == v.end())
                                            v.push_back(l);
                                    }
                                }
                            }
                        }
                    }
                    for (auto &l : of_object[o]) lights[l]++;
                }
            }
            std::string ln;
            for (auto &[l, n] : lights) {
                p.content.object_lights += n;
                ln += (ln.empty() ? "" : ", ") + fs::path(l).stem().string() + " (" + std::to_string(n) + " placed)";
            }
            snprintf(p.content.light_names, sizeof p.content.light_names, "%s", ln.c_str());
            read_marks(sc.root(), find, p);
            read_objects(sc.root(), r, find, tex, incident, p);
            // (the sky: its model, as an object's)
            tagfile::Block skies = sc.root().block("skies");
            std::string sky = skies.size() ? skies[0].ref("sky") : std::string();
            fs::path skf = sky.empty() ? fs::path() : find(sky);
            // (each layer's maps combined as its chicago or generic shader combines them, baked into one texture;
            // a shader_model, shader_environment or water layer drawn with its base map; any other left out)
            sky::Baked baked = skf.empty() ? sky::Baked() : sky::bake(sky, [&](const std::string &t) { return find(t); }, 256);
            if (baked.ok) {
                Model model;
                model.tag = sky;
                for (auto &bp : baked.parts) {
                    ModelPart part;
                    double su = 1, sv = 1;
                    if (bp.baked) {
                        Texture t;
                        t.w = bp.w;
                        t.h = bp.h;
                        t.rgba = bp.rgba;
                        part.texture = (int32_t)p.textures.size();
                        p.textures.push_back(std::move(t));
                        part.flags = ModelPart::kSeeThrough | ModelPart::kTwoSided |
                                     (bp.blend == 1 ? ModelPart::kAdd : bp.blend == 2 ? ModelPart::kMultiply
                                      : bp.blend == 3 ? ModelPart::kDoubleMultiply : 0);
                    } else {
                        std::string ext = fs::path(bp.shader).extension().u8string();
                        if (ext != ".shader_model" && ext != ".shader_environment" && ext != ".shader_transparent_water") continue;
                        tagfile::Tag sh;
                        fs::path shf = find(bp.shader);
                        if (shf.empty() || !sh.load(shf)) continue;
                        part.texture = tex.get(bitmap_ref(sh.root(), "base map"), 0, 256);
                        if (part.texture < 0) continue;
                        if (ext == ".shader_model") { su = or_one(sh.root().num("map u scale")); sv = or_one(sh.root().num("map v scale")); }
                        part.flags = ModelPart::kTwoSided | (ext == ".shader_transparent_water" ? ModelPart::kSeeThrough : 0);
                    }
                    uint32_t base = (uint32_t)model.vertices.size();
                    for (size_t k = 0; k < bp.p.size(); k++) {
                        ModelVertex x{};
                        for (int c = 0; c < 3; c++) x.p[c] = bp.p[k][c];
                        x.uv[0] = (float)(bp.uv[k][0] * su);
                        x.uv[1] = (float)(bp.uv[k][1] * sv);
                        model.vertices.push_back(x);
                    }
                    part.first = (int32_t)(model.tris.size() / 3);
                    for (uint32_t i : bp.tris) model.tris.push_back(base + i);
                    part.count = (int32_t)(model.tris.size() / 3) - part.first;
                    if (part.count > 0) model.parts.push_back(part);
                }
                if (!model.parts.empty()) {
                    p.sky_model = (int32_t)p.models.size();
                    p.models.push_back(std::move(model));
                }
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

namespace {
// An object's triangle in the world: its corners, texture coordinates and light (the object's light where each corner
// faces), and how it's drawn (ModelPart).
struct WorldTriangle { float q[3][3], uv[3][2], c[3][3]; int texture = -1, flags = 0, object = -1, part = -1; };
void object_triangles(const Preview &p, std::vector<WorldTriangle> &out) {
    out.clear();
    for (size_t oi = 0; oi < p.objects.size(); oi++) {
        const Object &o = p.objects[oi];
        if (o.model < 0 || (size_t)o.model >= p.models.size()) continue;
        const Model &m = p.models[(size_t)o.model];
        for (size_t pi = 0; pi < m.parts.size(); pi++) {
            const ModelPart &part = m.parts[pi];
            for (int32_t t = part.first; t < part.first + part.count && (size_t)t * 3 + 2 < m.tris.size(); t++) {
                WorldTriangle w;
                w.texture = part.texture;
                w.flags = part.flags;
                w.object = (int)oi;
                w.part = (int)pi;
                for (int k = 0; k < 3; k++) {
                    const ModelVertex &v = m.vertices[m.tris[(size_t)t * 3 + k]];
                    float n[3];
                    for (int c = 0; c < 3; c++) {
                        w.q[k][c] = o.p[c] + o.axes[0][c] * v.p[0] + o.axes[1][c] * v.p[1] + o.axes[2][c] * v.p[2];
                        n[c] = o.axes[0][c] * v.n[0] + o.axes[1][c] * v.n[1] + o.axes[2][c] * v.n[2];
                    }
                    float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                    if (l > 1e-12f) for (float &c : n) c /= l;
                    o.light.at(n, w.c[k]);
                    w.uv[k][0] = v.uv[0];
                    w.uv[k][1] = v.uv[1];
                }
                out.push_back(w);
            }
        }
    }
}
// Whether an object's triangle is cut away at barycentrics (l1, l2): alpha-tested, where its base map's alpha is half or
// less (the Xbox's alpha reference 0x7F, "greater").
bool object_cut_away(const Preview &p, const WorldTriangle &t, double l1, double l2) {
    if (!(t.flags & ModelPart::kCutout) || t.texture < 0 || (size_t)t.texture >= p.textures.size()) return false;
    double l0 = 1 - l1 - l2;
    float c[4];
    sample(p.textures[(size_t)t.texture], t.uv[0][0] * l0 + t.uv[1][0] * l1 + t.uv[2][0] * l2,
           t.uv[0][1] * l0 + t.uv[1][1] * l1 + t.uv[2][1] * l2, true, c);
    return c[3] * 255 <= 127;
}
}   // namespace

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
        const bool cut = p.materials[(size_t)t.material].cutout >= 0;
        raster((int)i, [&](int x, int y, float z, float l1, float l2) {
            size_t o = (size_t)y * w + x;
            if (z < zb[o] && !(cut && cut_away(p, t, l1, l2))) { zb[o] = z; tid[o] = (int)i; b1[o] = l1; b2[o] = l2; }
        });
    }
    // The objects (their triangles after the level's in `tid`), see-through ones later with the level's.
    const int T = (int)p.triangles.size();
    std::vector<WorldTriangle> ot;
    std::vector<int> clear_objects;
    if (extras && extras->objects) object_triangles(p, ot);
    for (size_t k = 0; k < ot.size(); k++) {
        const WorldTriangle &t = ot[k];
        if (t.flags & ModelPart::kSeeThrough) { clear_objects.push_back((int)k); continue; }
        if (!(t.flags & ModelPart::kTwoSided)) {   // (wound counterclockwise seen from the front)
            V3 a{t.q[0][0], t.q[0][1], t.q[0][2]}, b{t.q[1][0], t.q[1][1], t.q[1][2]}, c{t.q[2][0], t.q[2][1], t.q[2][2]};
            if (dot(cross(sub(b, a), sub(c, a)), sub(eye, a)) <= 0) continue;
        }
        raster_corners(t.q[0], t.q[1], t.q[2], [&](int x, int y, float z, float l1, float l2) {
            size_t o = (size_t)y * w + x;
            if (z < zb[o] && !object_cut_away(p, t, l1, l2)) { zb[o] = z; tid[o] = T + (int)k; b1[o] = l1; b2[o] = l2; }
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
            } else if (tid[o] >= T) {   // (an object: its base map x its light)
                const WorldTriangle &t = ot[(size_t)(tid[o] - T)];
                double l1 = b1[o], l2 = b2[o], l0 = 1 - l1 - l2;
                float base[4] = {0.6f, 0.6f, 0.6f, 1};
                if (t.texture >= 0)
                    sample(p.textures[(size_t)t.texture], t.uv[0][0] * l0 + t.uv[1][0] * l1 + t.uv[2][0] * l2,
                           t.uv[0][1] * l0 + t.uv[1][1] * l1 + t.uv[2][1] * l2, true, base);
                for (int k = 0; k < 3; k++) col[k] = base[k] * (t.c[0][k] * l0 + t.c[1][k] * l1 + t.c[2][k] * l2);
                if (fogged) {
                    double span = std::max(1e-3, (double)fog.opaque - fog.start);
                    double f = fog.density * std::clamp((zb[o] - fog.start) / span, 0.0, 1.0);
                    for (int k = 0; k < 3; k++) col[k] = std::min(1.0, col[k]) * (1 - f) + fog.color[k] * f;
                }
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
    // (the objects' see-through parts: by their framebuffer blend function, fogged as the engine fogs them: what they
    // add fades with the fog, what they multiply by turns to 1)
    for (int k : clear_objects) {
        const WorldTriangle &t = ot[(size_t)k];
        raster_corners(t.q[0], t.q[1], t.q[2], [&](int x, int y, float z, float l1, float l2) {
            size_t o = (size_t)y * w + x;
            if (z >= zb[o] || z > far_z) return;
            double l0 = 1 - l1 - l2;
            float c[4] = {0.55f, 0.65f, 0.75f, 0.35f};
            if (t.texture >= 0)
                sample(p.textures[(size_t)t.texture], t.uv[0][0] * l0 + t.uv[1][0] * l1 + t.uv[2][0] * l2,
                       t.uv[0][1] * l0 + t.uv[1][1] * l1 + t.uv[2][1] * l2, true, c);
            double f = 0;
            if (fogged) f = fog.density * std::clamp((z - fog.start) / std::max(1e-3, (double)fog.opaque - fog.start), 0.0, 1.0);
            uint32_t q = px[o];
            double d[3] = {((q >> 16) & 255) / 255.0, ((q >> 8) & 255) / 255.0, (q & 255) / 255.0};
            for (int i = 0; i < 3; i++) {
                if (t.flags & ModelPart::kAdd) d[i] += c[i] * (1 - f);
                else if (t.flags & ModelPart::kMultiply) d[i] *= c[i] * (1 - f) + f;
                else if (t.flags & ModelPart::kDoubleMultiply) d[i] *= (2 * c[i]) * (1 - f) + f;
                else d[i] = d[i] * (1 - c[3]) + (c[i] * (1 - f) + fog.color[i] * f) * c[3];
            }
            px[o] = (uint32_t)(std::min(1.0, d[0]) * 255) << 16 | (uint32_t)(std::min(1.0, d[1]) * 255) << 8 | (uint32_t)(std::min(1.0, d[2]) * 255);
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

int pick(const Preview &p, const Camera &cam, int w, int h, double x, double y, std::array<double, 3> *hit, int *object,
         int *part) {
    if (object) *object = -1;
    if (part) *part = -1;
    if (w <= 0 || h <= 0) return -1;
    const double tx = std::tan(35.0 * 3.14159265358979 / 180), ty = tx * h / w;
    V3 eye{cam.eye[0], cam.eye[1], cam.eye[2]};
    V3 fwd{std::cos(cam.yaw) * std::cos(cam.pitch), std::sin(cam.yaw) * std::cos(cam.pitch), std::sin(cam.pitch)};
    V3 right = norm(cross(fwd, V3{0, 0, 1})), up = cross(right, fwd);
    const double sx = ((x + 0.5) / w * 2 - 1) * tx, sy = (1 - (y + 0.5) / h * 2) * ty;
    // (the ray through the pixel, one unit ahead of the camera per unit along it: its distances are depths)
    V3 d{fwd[0] + right[0] * sx + up[0] * sy, fwd[1] + right[1] * sx + up[1] * sy, fwd[2] + right[2] * sx + up[2] * sy};
    int best = -1;
    double best_t = 1e30;
    for (size_t i = 0; i < p.triangles.size(); i++) {
        const Triangle &t = p.triangles[i];
        if (t.material < 0 || (size_t)t.material >= p.materials.size()) continue;
        V3 a{t.v[0].p[0], t.v[0].p[1], t.v[0].p[2]}, b{t.v[1].p[0], t.v[1].p[1], t.v[1].p[2]}, c{t.v[2].p[0], t.v[2].p[1], t.v[2].p[2]};
        V3 e1 = sub(b, a), e2 = sub(c, a);
        // (Halo winds a surface's corners clockwise seen from the front; see-through ones are drawn from both sides)
        if (!p.materials[(size_t)t.material].see_through && dot(cross(e1, e2), sub(eye, a)) >= 0) continue;
        V3 pv = cross(d, e2);
        double det = dot(e1, pv);
        if (std::fabs(det) < 1e-12) continue;
        V3 tv = sub(eye, a);
        double l1 = dot(tv, pv) / det;
        if (l1 < 0 || l1 > 1) continue;
        V3 q = cross(tv, e1);
        double l2 = dot(d, q) / det;
        if (l2 < 0 || l1 + l2 > 1) continue;
        double dist = dot(e2, q) / det;
        if (dist < 0.02 || dist >= best_t || cut_away(p, t, l1, l2)) continue;
        best = (int)i;
        best_t = dist;
    }
    if (object) {   // (the objects, in front of it)
        std::vector<WorldTriangle> ot;
        object_triangles(p, ot);
        for (const WorldTriangle &t : ot) {
            V3 a{t.q[0][0], t.q[0][1], t.q[0][2]}, b{t.q[1][0], t.q[1][1], t.q[1][2]}, c{t.q[2][0], t.q[2][1], t.q[2][2]};
            V3 e1 = sub(b, a), e2 = sub(c, a);
            if (!(t.flags & (ModelPart::kTwoSided | ModelPart::kSeeThrough)) && dot(cross(e1, e2), sub(eye, a)) <= 0) continue;
            V3 pv = cross(d, e2);
            double det = dot(e1, pv);
            if (std::fabs(det) < 1e-12) continue;
            V3 tv = sub(eye, a);
            double l1 = dot(tv, pv) / det;
            if (l1 < 0 || l1 > 1) continue;
            V3 q = cross(tv, e1);
            double l2 = dot(d, q) / det;
            if (l2 < 0 || l1 + l2 > 1) continue;
            double dist = dot(e2, q) / det;
            if (dist < 0.02 || dist >= best_t || object_cut_away(p, t, l1, l2)) continue;
            best = -1;
            best_t = dist;
            *object = t.object;
            if (part) *part = t.part;
        }
        if (*object >= 0 && hit) *hit = {eye[0] + d[0] * best_t, eye[1] + d[1] * best_t, eye[2] + d[2] * best_t};
    }
    if (hit && best >= 0) *hit = {eye[0] + d[0] * best_t, eye[1] + d[1] * best_t, eye[2] + d[2] * best_t};
    return best;
}

const double kSide = 0.77;   // (the cosine of 40 degrees: the picture's side, widescreen's a little past Halo's 35)
std::vector<FogCut> fog_cuts(const Preview &p, double end, size_t most) {
    std::vector<FogCut> out;
    if (end <= 0 || p.views.empty()) return out;
    // Each material's upright triangles: their corners (at most 600, spread) and area.
    std::map<int, std::vector<std::array<float, 2>>> pts;
    std::map<int, double> area;
    for (auto &t : p.triangles) {
        if (t.material < 0) continue;
        float e1[3], e2[3], n[3];
        for (int k = 0; k < 3; k++) { e1[k] = t.v[1].p[k] - t.v[0].p[k]; e2[k] = t.v[2].p[k] - t.v[0].p[k]; }
        n[0] = e1[1] * e2[2] - e1[2] * e2[1]; n[1] = e1[2] * e2[0] - e1[0] * e2[2]; n[2] = e1[0] * e2[1] - e1[1] * e2[0];
        double l = std::sqrt((double)n[0] * n[0] + (double)n[1] * n[1] + (double)n[2] * n[2]);
        if (l <= 1e-9 || std::fabs(n[2]) / l > 0.5) continue;   // (upright: walls, cards, cliffs; not floors)
        area[t.material] += l / 2;
        for (auto &v : t.v) pts[t.material].push_back({v.p[0], v.p[1]});
    }
    size_t step = std::max<size_t>(1, p.views.size() / 150);
    for (auto &[m, ps] : pts) {
        if (area[m] < 20) continue;
        std::vector<std::array<float, 2>> q;
        size_t every = std::max<size_t>(1, ps.size() / 600);
        for (size_t i = 0; i < ps.size(); i += every) q.push_back(ps[i]);
        std::vector<double> nearest, farthest;
        for (size_t v = 0; v < p.views.size(); v += step) {
            double lo = 1e30, hi = 0;
            for (auto &c : q) {
                double d = std::hypot(c[0] - p.views[v].eye[0], c[1] - p.views[v].eye[1]);
                lo = std::min(lo, d);
                hi = std::max(hi, d);
            }
            nearest.push_back(lo);
            farthest.push_back(hi);
        }
        std::sort(nearest.begin(), nearest.end());
        std::sort(farthest.begin(), farthest.end());
        // (the cut is by distance in front of the camera: something nearer than the end draws at the picture's sides
        // as it turns, out to kSide of its distance; and the level's own walls round players are cut wherever the fog
        // ends, the backdrop past them is what shows it)
        double a = nearest[nearest.size() / 2], b = farthest[farthest.size() / 2];
        if (a * kSide < end && b > end && a >= end * 0.5 && (size_t)m < p.material_tags.size())
            out.push_back({p.material_tags[(size_t)m], a, b, area[m]});
    }
    std::sort(out.begin(), out.end(), [](const FogCut &x, const FogCut &y) { return x.area > y.area; });
    if (out.size() > most) out.resize(most);
    return out;
}

std::string fog_cut_text(const std::vector<FogCut> &cuts, double end) {
    if (cuts.empty()) return "";
    auto num = [](double v) { char b[32]; snprintf(b, sizeof b, "%.0f", v); return std::string(b); };
    double lo = 1e30, hi = 0;
    std::string names;
    for (size_t i = 0; i < cuts.size(); i++) {
        lo = std::min(lo, cuts[i].nearest);
        hi = std::max(hi, cuts[i].farthest);
        names += (i ? ", " : "") + fs::path(cuts[i].tag).stem().u8string() + " (" + num(cuts[i].nearest) + " to " +
                 num(cuts[i].farthest) + (i ? ")" : " units from where players stand)");
    }
    bool one = cuts.size() == 1;
    return "the fog's end (" + num(end) + " units) runs through " + names + ": past it the Xbox draws nothing and the sky "
           "shows, so " + (one ? "it's" : "they're") + " cut off along a line that moves as the camera turns. Under " +
           num(std::max(1.0, std::floor(lo * kSide))) + " units " + (one ? "it's" : "they're") + " never drawn; past " +
           num(std::ceil(hi * 1.03)) + ", always whole";
}

// The file: a magic, then each part as counts and raw values (this machine's byte order: the file is a cache).
namespace {
const char kMagic[8] = {'H', 'M', 'P', 'L', 'S', 'H', 'D', 'C'};
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
    o.raw((uint32_t)p.material_tags.size());
    for (auto &t : p.material_tags) o.str(t);
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
    o.raw(p.weather);
    o.raw(p.content);
    o.vec(p.tree);
    o.vec(p.leaf_room);
    o.vec(p.marks);
    o.vec(p.boxes);
    o.raw((uint32_t)p.models.size());
    for (auto &m : p.models) {
        o.str(m.tag);
        o.vec(m.vertices); o.vec(m.tris); o.vec(m.parts);
        o.raw((uint32_t)m.shaders.size());
        for (auto &t : m.shaders) o.str(t);
    }
    o.vec(p.objects);
    o.raw(p.sky_model);
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
    p.material_tags.resize(n);
    for (auto &t : p.material_tags) in.str(t);
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
    in.raw(p.weather);
    in.raw(p.content);
    in.vec(p.tree);
    in.vec(p.leaf_room);
    in.vec(p.marks);
    in.vec(p.boxes);
    in.raw(n);
    if (!in.ok || n > 100000) return false;
    p.models.resize(n);
    for (auto &m : p.models) {
        in.str(m.tag);
        in.vec(m.vertices); in.vec(m.tris); in.vec(m.parts);
        uint32_t k = 0;
        in.raw(k);
        if (!in.ok || k > 100000) return false;
        m.shaders.resize(k);
        for (auto &t : m.shaders) in.str(t);
    }
    in.vec(p.objects);
    in.raw(p.sky_model);
    if (p.sky_model >= (int32_t)p.models.size()) return false;
    // (every reference in range: the pictures index with them)
    for (auto &m : p.models) {
        for (uint32_t v : m.tris) if (v >= m.vertices.size()) return false;
        for (auto &pt : m.parts)
            if (pt.first < 0 || pt.count < 0 || (size_t)(pt.first + pt.count) * 3 > m.tris.size() ||
                pt.texture >= (int32_t)p.textures.size())
                return false;
    }
    for (auto &ob : p.objects) if (ob.model >= (int32_t)p.models.size()) return false;
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
