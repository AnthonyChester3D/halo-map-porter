// Halo Map Porter - a model's preview for the weapons & vehicles window (see preview.hpp)
#include "preview.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

#include "bcdec.h"
#include "tagfile.hpp"

namespace preview {

using porter::ModelPreview;

namespace {

double or_one(double v) { return v == 0 || !std::isfinite(v) ? 1.0 : v; }

// The first texture of a bitmap tag as RGBA (top row first), made at most `max_side` pixels on a side.
bool bitmap_texture(const std::filesystem::path &file, int max_side, ModelPreview::Texture &out) {
    tagfile::Tag t;
    if (!t.load(file)) return false;
    tagfile::Elem r = t.root();
    tagfile::Block bd = r.block("bitmap data");
    if (bd.size() == 0) return false;
    tagfile::Elem e = bd[0];
    int w = (int)e.integer("width"), h = (int)e.integer("height"), fmt = (int)e.integer("format");
    long long flags = e.integer("flags");
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || (flags & 8)) return false;   // (8: swizzled)
    std::string pix = r.data("processed pixel data");
    size_t off = (size_t)e.integer("pixel data offset");
    if (off >= pix.size()) return false;
    const unsigned char *p = (const unsigned char *)pix.data() + off;
    size_t avail = pix.size() - off;
    std::vector<uint8_t> rgba((size_t)w * h * 4, 255);
    if (fmt == 14 || fmt == 15 || fmt == 16) {                       // DXT1 / DXT3 / DXT5
        int bw = (w + 3) / 4, bh = (h + 3) / 4, bs = fmt == 14 ? 8 : 16;
        if (avail < (size_t)bw * bh * bs) return false;
        std::vector<uint8_t> block(64);
        for (int by = 0; by < bh; by++)
            for (int bx = 0; bx < bw; bx++) {
                const unsigned char *src = p + ((size_t)by * bw + bx) * bs;
                if (fmt == 14) bcdec_bc1(src, block.data(), 16);
                else if (fmt == 15) bcdec_bc2(src, block.data(), 16);
                else bcdec_bc3(src, block.data(), 16);
                for (int y = 0; y < 4 && by * 4 + y < h; y++)
                    for (int x = 0; x < 4 && bx * 4 + x < w; x++)
                        memcpy(&rgba[((size_t)(by * 4 + y) * w + bx * 4 + x) * 4], &block[(y * 4 + x) * 4], 4);
            }
    } else {
        int bpp = fmt <= 2 || fmt == 17 ? 1 : (fmt == 3 || fmt == 6 || fmt == 8 || fmt == 9) ? 2 : (fmt == 10 || fmt == 11) ? 4 : 0;
        if (!bpp || avail < (size_t)w * h * bpp) return false;
        for (size_t i = 0; i < (size_t)w * h; i++) {
            uint8_t *q = &rgba[i * 4];
            unsigned v = bpp == 2 ? p[i * 2] | (p[i * 2 + 1] << 8) : 0;
            switch (fmt) {
            case 0: q[0] = q[1] = q[2] = 255; q[3] = p[i]; break;                  // A8
            case 1: case 2: case 17: q[0] = q[1] = q[2] = p[i]; break;             // Y8 / AY8 / P8 (shown grey)
            case 3: q[0] = q[1] = q[2] = p[i * 2]; q[3] = p[i * 2 + 1]; break;     // A8Y8
            case 6: q[0] = (v >> 11) * 255 / 31; q[1] = ((v >> 5) & 63) * 255 / 63; q[2] = (v & 31) * 255 / 31; break;
            case 8: q[0] = ((v >> 10) & 31) * 255 / 31; q[1] = ((v >> 5) & 31) * 255 / 31; q[2] = (v & 31) * 255 / 31; break;
            case 9: q[0] = ((v >> 8) & 15) * 17; q[1] = ((v >> 4) & 15) * 17; q[2] = (v & 15) * 17; break;
            default: q[0] = p[i * 4 + 2]; q[1] = p[i * 4 + 1]; q[2] = p[i * 4]; q[3] = p[i * 4 + 3]; break;   // BGRA bytes
            }
        }
    }
    // Smaller: average blocks of pixels.
    int f = 1;
    while (std::max(w, h) / f > max_side) f *= 2;
    int nw = std::max(1, w / f), nh = std::max(1, h / f);
    out.w = nw;
    out.h = nh;
    out.rgba.assign((size_t)nw * nh * 4, 0);
    for (int y = 0; y < nh; y++)
        for (int x = 0; x < nw; x++)
            for (int c = 0; c < 4; c++) {
                unsigned sum = 0, n = 0;
                for (int dy = 0; dy < f && y * f + dy < h; dy++)
                    for (int dx = 0; dx < f && x * f + dx < w; dx++, n++) sum += rgba[((size_t)(y * f + dy) * w + x * f + dx) * 4 + c];
                out.rgba[((size_t)y * nw + x) * 4 + c] = (uint8_t)(n ? sum / n : 0);
            }
    return true;
}

// A shader's base texture and how many times it repeats across the model's texture coordinates.
struct ShaderMap { std::string bitmap; double u = 1, v = 1; };
ShaderMap shader_map(const std::filesystem::path &file) {
    ShaderMap m;
    tagfile::Tag t;
    if (!t.load(file)) return m;
    tagfile::Elem r = t.root();
    std::string ext = file.extension().u8string();
    if (ext == ".shader_model") {
        m.bitmap = r.ref("base map");
        m.u = or_one(r.num("map u scale"));
        m.v = or_one(r.num("map v scale"));
    } else if (ext == ".shader_environment") {
        m.bitmap = r.ref("base map");
    } else {
        tagfile::Block maps = r.block(ext == ".shader_transparent_chicago_extended" ? "maps 4 stage" : "maps");
        if (maps.size() == 0 && ext == ".shader_transparent_chicago_extended") maps = r.block("maps 2 stage");
        if (maps.size()) {
            m.bitmap = maps[0].ref("map");
            m.u = or_one(maps[0].num("map u scale"));
            m.v = or_one(maps[0].num("map v scale"));
        }
    }
    return m;
}

int strip_triangles(const tagfile::Elem &part) {
    tagfile::Block t = part.block("triangles");
    int n = 0, run = 0;
    for (size_t i = 0; i < t.size(); i++)
        for (int k = 0; k < 3; k++) {
            static const char *f[3] = {"vertex0 index", "vertex1 index", "vertex2 index"};
            if ((int)t[i].integer(f[k]) == 65535) continue;
            if (++run >= 3) n++;
        }
    return n;
}

}   // namespace

int seats(const std::filesystem::path &unit, std::string *roles) {
    tagfile::Tag t;
    if (!t.load(unit)) return -1;
    tagfile::Block s = t.root().block("seats");
    int drivers = 0, gunners = 0, others = 0;
    for (size_t i = 0; i < s.size(); i++) {
        long long f = s[i].integer("flags");
        if (f & 4) drivers++;            // driver
        else if (f & 8) gunners++;       // gunner
        else others++;
    }
    if (roles) {
        std::vector<std::string> v;
        auto add = [&](int n, const char *one, const char *many) {
            if (n == 1) v.push_back(one);
            else if (n > 1) v.push_back(std::to_string(n) + " " + many);
        };
        add(drivers, "driver", "drivers");
        add(gunners, "gunner", "gunners");
        add(others, (drivers || gunners) ? "passenger" : "1 passenger", "passengers");
        roles->clear();
        for (size_t i = 0; i < v.size(); i++) *roles += (i ? ", " : "") + v[i];
    }
    return (int)s.size();
}

bool build(const std::string &model, const Find &find, ModelPreview &out, int max_texture, int level,
           std::vector<int> *geometries) {
    out = ModelPreview();
    std::filesystem::path mf = find(model);
    if (mf.empty()) return false;
    tagfile::Tag t;
    if (!t.load(mf)) return false;
    tagfile::Elem r = t.root();
    double mu = or_one(r.num("base map u scale")), mv = or_one(r.num("base map v scale"));
    tagfile::Block geos = r.block("geometries"), regions = r.block("regions"), shaders = r.block("shaders");

    // Each region's first permutation, most detailed (a lower-detail version when that's a lot to draw), or the
    // level asked for.
    static const char *levels[5] = {"super high", "high", "medium", "low", "super low"};
    std::vector<int> use;
    for (int lv = level >= 0 && level < 5 ? level : 0; lv < 5; lv++) {
        std::set<long long> seen;
        use.clear();
        int tris = 0;
        for (size_t i = 0; i < regions.size(); i++) {
            tagfile::Block perms = regions[i].block("permutations");
            if (perms.size() == 0) continue;
            long long g = perms[0].integer(levels[lv]);
            if (g < 0 || g >= (long long)geos.size() || !seen.insert(g).second) continue;
            use.push_back((int)g);
            tagfile::Block parts = geos[(size_t)g].block("parts");
            for (size_t k = 0; k < parts.size(); k++) tris += strip_triangles(parts[k]);
        }
        if (tris <= 40000 || (level >= 0 && level < 5)) break;
    }
    if (geometries) *geometries = use;
    if (use.empty()) return false;

    std::map<long long, int> texture_of_shader;
    std::map<std::string, int> texture_of_bitmap;
    std::map<long long, std::pair<double, double>> scale_of_shader;
    auto texture_for = [&](long long si, double &su, double &sv) {
        su = sv = 1;
        if (si < 0 || si >= (long long)shaders.size()) return -1;
        auto it = texture_of_shader.find(si);
        if (it != texture_of_shader.end()) {
            su = scale_of_shader[si].first;
            sv = scale_of_shader[si].second;
            return it->second;
        }
        int tex = -1;
        // A model's shader references say only "a shader": try each kind.
        std::string sh = shaders[(size_t)si].ref("shader");
        std::filesystem::path sf;
        if (!sh.empty() && std::filesystem::path(sh).extension().u8string().rfind(".shader", 0) == 0) sf = find(sh);
        else if (!sh.empty())
            for (const char *x : {".shader_model", ".shader_environment", ".shader_transparent_chicago",
                                  ".shader_transparent_chicago_extended", ".shader_transparent_generic"})
                if (!(sf = find(sh + x)).empty()) break;
        ShaderMap m = sf.empty() ? ShaderMap() : shader_map(sf);
        if (!m.bitmap.empty()) {
            auto bt = texture_of_bitmap.find(m.bitmap);
            if (bt != texture_of_bitmap.end()) tex = bt->second;
            else {
                std::filesystem::path bf = find(m.bitmap);
                ModelPreview::Texture tx;
                if (!bf.empty() && bitmap_texture(bf, max_texture, tx)) {
                    tex = (int)out.textures.size();
                    out.textures.push_back(std::move(tx));
                }
                texture_of_bitmap[m.bitmap] = tex;
            }
        }
        texture_of_shader[si] = tex;
        scale_of_shader[si] = {m.u, m.v};
        su = m.u;
        sv = m.v;
        return tex;
    };

    for (int g : use) {
        tagfile::Block parts = geos[(size_t)g].block("parts");
        for (size_t k = 0; k < parts.size(); k++) {
            tagfile::Elem part = parts[k];
            tagfile::Block verts = part.block("uncompressed vertices"), strip = part.block("triangles");
            if (verts.size() == 0 || strip.size() == 0) continue;
            double su, sv;
            int tex = texture_for(part.integer("shader index"), su, sv);
            uint32_t base = (uint32_t)(out.pos.size() / 3);
            for (size_t v = 0; v < verts.size(); v++) {
                for (int c = 0; c < 3; c++) out.pos.push_back((float)verts[v].num("position", c));
                out.uv.push_back((float)(verts[v].num("texture coords", 0) * mu * su));
                out.uv.push_back((float)(verts[v].num("texture coords", 1) * mv * sv));
            }
            ModelPreview::Part p;
            p.first = (uint32_t)(out.tris.size() / 3);
            p.texture = tex;
            std::vector<int> s;
            for (size_t i = 0; i < strip.size(); i++) {
                static const char *f[3] = {"vertex0 index", "vertex1 index", "vertex2 index"};
                for (int c = 0; c < 3; c++) {
                    int x = (int)strip[i].integer(f[c]);
                    if (x != 65535) s.push_back(x);
                }
            }
            for (size_t i = 2; i < s.size(); i++) {
                int a = s[i - 2], b = s[i - 1], c = s[i];
                if (a == b || b == c || a == c) continue;
                if (a >= (int)verts.size() || b >= (int)verts.size() || c >= (int)verts.size() || a < 0 || b < 0 || c < 0) continue;
                if ((i - 2) % 2) std::swap(a, b);
                out.tris.insert(out.tris.end(), {base + a, base + b, base + c});
            }
            p.count = (uint32_t)(out.tris.size() / 3) - p.first;
            if (p.count) out.parts.push_back(p);
        }
    }
    return !out.tris.empty();
}

}   // namespace preview
