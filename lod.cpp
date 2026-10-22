// Halo Map Porter - lower-detail versions of models (see lod.hpp)
#include "lod.hpp"
#include "meshoptimizer/meshoptimizer.h"
#include "tagfile.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <vector>

namespace fs = std::filesystem;
using tagfile::TagNode;

namespace lod {

// Bump when what's made changes, so the library makes them again instead of reusing old ones.
static const char *kVersion = "hmp-lod-1 meshoptimizer-1.3";
// The versions made: share of the triangles to aim for, and how far (share of the model's size) the shape may move.
struct Level { double ratio, error; };
static const Level kLevels[2] = {{0.5, 0.01}, {0.2, 0.025}};

static float f32(const std::string &b, size_t o) {
    uint32_t u = (uint32_t)(uint8_t)b[o] << 24 | (uint32_t)(uint8_t)b[o + 1] << 16 | (uint32_t)(uint8_t)b[o + 2] << 8 | (uint8_t)b[o + 3];
    float v;
    memcpy(&v, &u, 4);
    return v;
}
static unsigned u16(const std::string &b, size_t o) { return (unsigned)(uint8_t)b[o] << 8 | (uint8_t)b[o + 1]; }
static void put16(std::string &b, size_t o, unsigned v) { b[o] = (char)(v >> 8); b[o + 1] = (char)v; }

// A part's triangle strip as a list of triangles. Halo's strips alternate like Direct3D's: (a b c), (c b d)...
// written here as even (a b c), odd (b a c), which keeps every triangle's facing.
static std::vector<uint32_t> strip_to_list(const std::vector<TagNode> &tris, size_t vertices) {
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

// A list of triangles as one strip (joined with repeated indices), three indices per element, the end padded.
static std::vector<TagNode> list_to_strip(const std::vector<uint32_t> &list, size_t vertices, int sid) {
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

static int list_triangles(const TagNode &geo) {
    int n = 0;
    TagNode g = geo;
    for (auto &p : *tagfile::block(g, "parts"))
        n += (int)strip_to_list(*tagfile::block(p, "triangles"), tagfile::block(p, "uncompressed vertices")->size()).size() / 3;
    return n;
}

using Point = std::array<float, 3>;

// A geometry simplified to `level`: a copy with only the triangles kept and the vertices they use. Its parts (one
// per shader) are simplified together as one mesh, so where two parts meet they change together (no cracks): their
// vertices there are copies at the same positions, which the simplifier keeps paired, like a texture seam.
static TagNode simplify_geometry(const TagNode &geo, const Level &level, double size) {
    TagNode g = geo;
    std::vector<TagNode> &parts = *tagfile::block(g, "parts");
    // All the parts' vertices in one list (each remembering its part), and all their triangles.
    std::vector<size_t> first(parts.size() + 1, 0);
    std::vector<uint32_t> list;
    for (size_t p = 0; p < parts.size(); p++) {
        size_t n = tagfile::block(parts[p], "uncompressed vertices")->size();
        for (uint32_t i : strip_to_list(*tagfile::block(parts[p], "triangles"), n)) list.push_back((uint32_t)(first[p] + i));
        first[p + 1] = first[p] + n;
    }
    size_t n = first.back();
    if (list.size() / 3 < 24 || n == 0) return g;   // too small to be worth it
    std::vector<uint32_t> part_of(n);
    std::vector<float> pos(n * 3), attr(n * 5);
    std::vector<unsigned> bone(n);
    for (size_t p = 0; p < parts.size(); p++) {
        auto &V = *tagfile::block(parts[p], "uncompressed vertices");
        for (size_t k = 0; k < V.size(); k++) {
            size_t i = first[p] + k;
            const std::string &v = V[k].fixed;
            part_of[i] = (uint32_t)p;
            for (int c = 0; c < 3; c++) pos[i * 3 + c] = f32(v, c * 4);
            attr[i * 5 + 0] = f32(v, 48);
            attr[i * 5 + 1] = f32(v, 52);
            for (int c = 0; c < 3; c++) attr[i * 5 + 2 + c] = f32(v, 12 + c * 4);
            unsigned n0 = u16(v, 56), n1 = u16(v, 58);
            bone[i] = f32(v, 60) >= 0.99f ? n0 : (n0 << 16 | n1);
        }
    }
    // Texture coordinates and normals count like changes in shape: 2% of the texture like 1% of the model's size,
    // about 11 degrees of a normal like 1%.
    const float w[5] = {(float)(size * 0.5), (float)(size * 0.5), (float)(size * 0.05), (float)(size * 0.05),
                        (float)(size * 0.05)};
    // Vertices that stay where they are: where pieces moved by different bones meet (a wheel stays apart from the
    // body). Copies at one position (a model's vertices are mostly copies, along hard edges, texture seams and
    // where parts meet) stay paired, except along hard edges: those may smooth over, which doesn't show from a
    // distance.
    std::vector<unsigned char> lock(n, 0);
    for (size_t t = 0; t + 2 < list.size(); t += 3)
        if (bone[list[t]] != bone[list[t + 1]] || bone[list[t]] != bone[list[t + 2]])
            lock[list[t]] = lock[list[t + 1]] = lock[list[t + 2]] = meshopt_SimplifyVertex_Lock;
    std::map<Point, std::vector<size_t>> at;
    for (size_t i = 0; i < n; i++) at[{pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2]}].push_back(i);
    for (auto &[pt, vs] : at) {
        bool keep = false;
        for (size_t k = 1; k < vs.size() && !keep; k++)
            keep = part_of[vs[k]] != part_of[vs[0]] || std::fabs(attr[vs[k] * 5] - attr[vs[0] * 5]) > 1e-5f ||
                   std::fabs(attr[vs[k] * 5 + 1] - attr[vs[0] * 5 + 1]) > 1e-5f;
        if (keep)
            for (size_t i : vs) lock[i] |= meshopt_SimplifyVertex_Protect;
    }

    size_t target = std::max<size_t>(3, (size_t)(list.size() / 3 * level.ratio) * 3);
    std::vector<uint32_t> out(list.size());
    float err = 0;
    out.resize(meshopt_simplifyWithAttributes(out.data(), list.data(), list.size(), pos.data(), n, 12, attr.data(), 20, w, 5,
                                              lock.data(), target, (float)(size * level.error),
                                              meshopt_SimplifyPermissive | meshopt_SimplifyErrorAbsolute, &err));
    // Back into parts: each triangle to its vertices' part (a triangle whose vertices ended up in different parts
    // would be a simplifier mistake: then the geometry stays as it was).
    std::vector<std::vector<uint32_t>> per(parts.size());
    for (size_t t = 0; t + 2 < out.size(); t += 3) {
        uint32_t p = part_of[out[t]];
        if (part_of[out[t + 1]] != p || part_of[out[t + 2]] != p) return geo;
        per[p].insert(per[p].end(), {out[t], out[t + 1], out[t + 2]});
    }
    // (Every part stays, in its place: parts refer to each other by number.)
    std::vector<TagNode> kept;
    for (size_t p = 0; p < parts.size(); p++) {
        if (per[p].empty()) { kept.push_back(parts[p]); continue; }   // simplified away entirely: keep it as it was
        TagNode part = parts[p];
        std::vector<TagNode> &V = *tagfile::block(part, "uncompressed vertices");
        std::vector<TagNode> *C = tagfile::block(part, "compressed vertices");
        size_t pn = V.size();
        // Only the vertices still used, in their original order.
        std::vector<uint32_t> remap(pn, ~0u);
        std::vector<char> used(pn, 0);
        for (uint32_t i : per[p]) used[i - first[p]] = 1;
        std::vector<TagNode> nv, nc;
        for (size_t i = 0; i < pn; i++)
            if (used[i]) {
                remap[i] = (uint32_t)nv.size();
                nv.push_back(V[i]);
                if (C && C->size() == pn) nc.push_back((*C)[i]);
            }
        for (auto &i : per[p]) i = remap[i - first[p]];
        *tagfile::block(part, "triangles") = list_to_strip(per[p], nv.size(), tagfile::block_struct(part, "triangles"));
        V = std::move(nv);
        if (C) *C = std::move(nc);
        kept.push_back(std::move(part));
    }
    parts = std::move(kept);
    return g;
}

static uint64_t fnv(const std::string &s, uint64_t h = 1469598103934665603ull) {
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

static std::string hex(uint64_t v) {
    char b[17];
    snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
    return b;
}

static size_t geometry_xbox_bytes(TagNode g) {
    size_t n = 0;
    for (auto &p : *tagfile::block(g, "parts"))
        n += tagfile::block(p, "uncompressed vertices")->size() * 32 + tagfile::block(p, "triangles")->size() * 6;
    return n;
}

static size_t geometry_bytes(TagNode g) {
    size_t n = 0;
    for (auto &p : *tagfile::block(g, "parts"))
        n += tagfile::block(p, "uncompressed vertices")->size() * 68 + tagfile::block(p, "compressed vertices")->size() * 32 +
             tagfile::block(p, "triangles")->size() * 6;
    return n;
}

static std::string lower(std::string s) {
    for (auto &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool skipped(const Settings &settings, const std::string &model_tag, const std::string &object_name) {
    if (settings.library.empty()) return false;
    std::ifstream in(settings.library / "skip.txt");
    std::string l, tag = lower(model_tag), noext = tag.substr(0, tag.rfind('.')), stem = fs::path(noext).filename().string();
    while (std::getline(in, l)) {
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        size_t a = l.find_first_not_of(' ');
        if (a == std::string::npos || l[a] == '#') continue;
        std::string x = lower(l.substr(a));
        std::replace(x.begin(), x.end(), '\\', '/');
        if (x == tag || x == noext || x == stem || x == lower(object_name)) return true;
    }
    return false;
}

// The versions of one geometry: from the library, or made now (and kept there).
static std::vector<TagNode> versions(const TagNode &geo, uint32_t checksum, double size, const Settings &settings, bool &from_library,
                                     std::string &key) {
    std::string key_text = std::string(kVersion) + "|" + std::to_string(checksum) + "|" + tagfile::write_node(geo);
    key = hex(fnv(key_text));
    fs::path file = settings.library.empty() ? fs::path() : settings.library / (key + ".lod");
    std::vector<TagNode> out;
    if (!file.empty() && fs::exists(file)) {
        std::ifstream in(file, std::ios::binary);
        std::string b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        size_t pos = 0;
        bool ok = b.compare(0, 8, "HMPLOD1\n") == 0;
        pos = 8;
        std::vector<TagNode> got;
        while (ok && pos < b.size()) {
            TagNode n;
            ok = tagfile::read_node(b, pos, n);
            if (ok) got.push_back(std::move(n));
        }
        if (ok && got.size() == 2) {
            from_library = true;
            return got;
        }
    }
    for (const Level &lv : kLevels) out.push_back(simplify_geometry(geo, lv, size));
    if (!file.empty()) {
        std::error_code ec;
        fs::create_directories(settings.library, ec);
        std::string b = "HMPLOD1\n";
        for (auto &g : out) b += tagfile::write_node(g);
        std::ofstream(file, std::ios::binary) << b;
    }
    return out;
}

Result add_lods(const fs::path &model, const double cutoffs[5], const Settings &settings, const std::string &tag) {
    Result r;
    tagfile::EditTag t;
    if (!t.load(model)) { r.why = "couldn't be read"; return r; }
    TagNode &root = t.root();
    std::vector<TagNode> *geos = tagfile::block(root, "geometries"), *regions = tagfile::block(root, "regions");
    if (!geos || !regions) { r.why = "not a model"; return r; }
    static const char *slots[5] = {"super low", "low", "medium", "high", "super high"};

    // Geometries every detail level of some permutation uses (no lower-detail versions yet).
    std::set<int> single;
    for (auto &reg : *regions)
        for (auto &perm : *tagfile::block(reg, "permutations")) {
            int g = (int)tagfile::get_num(perm, "super high");
            bool same = true;
            for (auto s : slots) same = same && (int)tagfile::get_num(perm, s) == g;
            if (same && g >= 0 && g < (int)geos->size()) single.insert(g);
        }
    // The model's triangles: its regions' first permutations, most detailed.
    for (auto &reg : *regions) {
        auto &perms = *tagfile::block(reg, "permutations");
        int g = perms.empty() ? -1 : (int)tagfile::get_num(perms[0], "super high");
        if (g >= 0 && g < (int)geos->size()) r.triangles += list_triangles((*geos)[g]);
    }
    if (single.empty()) { r.why = "already has lower-detail versions"; return r; }
    if (r.triangles < settings.min_triangles) { r.why = "only " + std::to_string(r.triangles) + " triangles"; return r; }

    // Its size: the diagonal of the box around the geometries being simplified.
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (int g : single)
        for (auto &p : *tagfile::block((*geos)[g], "parts"))
            for (auto &v : *tagfile::block(p, "uncompressed vertices"))
                for (int k = 0; k < 3; k++) {
                    float x = f32(v.fixed, k * 4);
                    lo[k] = std::min(lo[k], x);
                    hi[k] = std::max(hi[k], x);
                }
    double size = std::sqrt((double)(hi[0] - lo[0]) * (hi[0] - lo[0]) + (double)(hi[1] - lo[1]) * (hi[1] - lo[1]) +
                            (double)(hi[2] - lo[2]) * (hi[2] - lo[2]));
    if (!(size > 0)) { r.why = "has no size"; return r; }

    uint32_t checksum = (uint32_t)(long long)tagfile::get_num(root, "node list checksum");
    std::map<int, int> medium, low;   // geometry -> the version used for medium / low and super low
    int base_tris = 0, tris[2] = {0, 0};
    bool any_a = false, any_b = false, made = false;
    std::string files;   // the library files (made now), for its index
    for (int g : single) {
        bool lib = false;
        std::string key;
        std::vector<TagNode> vs = versions((*geos)[g], checksum, size, settings, lib, key);
        r.from_library = r.from_library || lib;
        if (!lib) {
            made = true;
            files += (files.empty() ? "" : " ") + key + ".lod";
        }
        int full = list_triangles((*geos)[g]), a = list_triangles(vs[0]), b = list_triangles(vs[1]);
        base_tris += full;
        // A version is worth adding when it has noticeably fewer triangles than the one above it.
        bool use_a = a > 0 && a <= full * 0.8, use_b = b > 0 && b <= (use_a ? a : full) * 0.8;
        int ia = g, ib = g;
        if (use_a) { ia = (int)geos->size(); geos->push_back(vs[0]); r.bytes_added += geometry_bytes(vs[0]); r.xbox_bytes += geometry_xbox_bytes(vs[0]); tris[0] += a; any_a = true; }
        else tris[0] += full;
        if (use_b) { ib = (int)geos->size(); geos->push_back(vs[1]); r.bytes_added += geometry_bytes(vs[1]); r.xbox_bytes += geometry_xbox_bytes(vs[1]); tris[1] += b; any_b = true; }
        else tris[1] += use_a ? a : full;
        if (!use_b) ib = ia;
        medium[g] = ia;
        low[g] = ib;
    }
    if (r.bytes_added == 0) { r.why = "couldn't be simplified much (its shape needs most of its triangles)"; return r; }
    if (geos->size() > 256) { r.why = "would have too many geometries"; return r; }
    for (auto &reg : *regions)
        for (auto &perm : *tagfile::block(reg, "permutations")) {
            int g = (int)tagfile::get_num(perm, "super high");
            if (!medium.count(g) || (int)tagfile::get_num(perm, "super low") != g) continue;
            tagfile::set_num(perm, "medium", medium[g]);
            tagfile::set_num(perm, "low", low[g]);
            tagfile::set_num(perm, "super low", low[g]);
        }
    static const char *cut[5] = {"super high detail cutoff", "high detail cutoff", "medium detail cutoff", "low detail cutoff",
                                 "super low detail cutoff"};
    bool none = true;
    for (auto c : cut) none = none && tagfile::get_num(root, c) == 0;
    if (none)
        for (int i = 0; i < 5; i++) tagfile::set_num(root, cut[i], cutoffs[i]);
    if (!t.save(model)) { r.why = "couldn't be saved"; return r; }
    r.changed = true;
    r.triangles = base_tris;   // (the geometries simplified, most detailed)
    r.level_triangles[0] = any_a ? tris[0] : 0;
    r.level_triangles[1] = any_b ? tris[1] : 0;
    if (!settings.library.empty() && made) {   // what's new in the library
        std::time_t now = std::time(nullptr);
        char date[16];
        std::strftime(date, sizeof date, "%Y-%m-%d", std::localtime(&now));
        bool fresh = !fs::exists(settings.library / "index.txt");
        std::ofstream idx(settings.library / "index.txt", std::ios::app | std::ios::binary);
        if (fresh)
            idx << "# The LOD library: lower-detail versions Halo Map Porter made, one line per model (date, model, triangles\r\n"
                   "# most detailed > medium > low, its files here). Delete a file to have it made again.\r\n";
        std::string tris_text = std::to_string(r.triangles);
        for (int k : r.level_triangles)
            if (k) tris_text += " > " + std::to_string(k);
        idx << date << "\t" << (tag.empty() ? model.filename().u8string() : tag) << "\t" << tris_text << "\t" << files << "\r\n";
    }
    return r;
}

}   // namespace lod
