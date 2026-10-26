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
#include <sstream>
#include <vector>

namespace fs = std::filesystem;
using tagfile::TagNode;

namespace lod {

// Bump when what's made changes, so the library makes them again instead of reusing old ones.
static const char *kVersion = "hmp-lod-1 meshoptimizer-1.3";
static const char *kTrimVersion = "hmp-trim-2 meshoptimizer-1.3";
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
// `how`: kLower (lower versions, seen from a distance): texture seams and where parts meet stay paired, hard edges may
// smooth over. kTop (a most detailed version, seen up close): every texture seam stays a seam (the simplifier may only
// slide along one, never across), and so does where parts meet; every hard edge left stays as hard (see Hard edges);
// two-sided surfaces stay as made.
//
// Bones: every vertex keeps the bones that move it and how much (it isn't moved, only taken out), and a vertex only
// goes into one moved exactly as it is, so the triangles left either all move as one piece or are the model's own
// (where a joint bends, where a wheel meets its axle: those vertices stay). So a character bends as before in every
// pose, and the shape is off by no more in any pose than at rest.
// Hard edges (kTop): copies of a vertex that differ only in their normal (the same part, place, texture coordinates and
// bones: a hard edge, or a model of flat facets) count as one for the simplifier, with their normals averaged, so such
// models can lose triangles too (a crease still costs what moving the shape does). Each corner left then takes the copy
// whose normal is nearest its triangle's facing, so the edges left are as hard as made.
// Layers: a part that repeats another part's triangles exactly, as a layer drawn over it (a character's shield
// shimmer is its whole body again with the shield's shader; a weapon's hologram over its gauge), isn't simplified on
// its own: the part below is, and the layer gets the same triangles on its own vertices.
// Cut-out parts (`keep`: per model shader, 1 for one drawn cut out) stay exactly as made: leaf cards, grates.
// `seen` (kTop, a first-person model): per part, per triangle, how much of the time it's on screen. Triangles never
// on screen go first, and the vertices on the edge between them and the rest stay where they are; then what's seen
// under 15% of the time is simplified first, allowed three times the change, with what's seen more held still, and
// then the whole as usual.
enum How { kLower, kTop };
static TagNode simplify_geometry(const TagNode &geo, const Level &level, double size, How how = kLower,
                                 const std::vector<char> *keep = nullptr, const std::vector<std::vector<float>> *seen = nullptr,
                                 int *unseen_out = nullptr) {
    bool top = how != kLower;
    TagNode g = geo;
    std::vector<TagNode> &parts = *tagfile::block(g, "parts");
    if (unseen_out) *unseen_out = 0;
    if (seen && seen->size() != parts.size()) seen = nullptr;   // (measured on another version of it)
    // All the parts' vertices in one list (each remembering its part), and each part's triangles.
    std::vector<size_t> first(parts.size() + 1, 0);
    std::vector<std::vector<uint32_t>> own(parts.size());
    for (size_t p = 0; p < parts.size(); p++) {
        size_t n = tagfile::block(parts[p], "uncompressed vertices")->size();
        for (uint32_t i : strip_to_list(*tagfile::block(parts[p], "triangles"), n)) own[p].push_back((uint32_t)(first[p] + i));
        first[p + 1] = first[p] + n;
    }
    size_t n = first.back();
    std::vector<uint32_t> part_of(n);
    std::vector<float> pos(n * 3), attr(n * 5);
    std::vector<uint64_t> bone(n);
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
            // What moves it: one bone, or two and how much each (in sixteenths).
            uint64_t n0 = u16(v, 56), n1 = u16(v, 58);
            float w0 = f32(v, 60);
            bone[i] = w0 >= 0.99f || n0 == n1 ? n0 : w0 <= 0.01f ? n1 : 1ull << 63 | n0 << 32 | n1 << 8 | (uint64_t)std::lround(w0 * 16);
        }
    }
    auto at = [&](uint32_t i) { return Point{pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2]}; };
    auto corners = [&](const std::vector<uint32_t> &tris, size_t t) {
        std::array<Point, 3> k = {at(tris[t]), at(tris[t + 1]), at(tris[t + 2])};
        std::sort(k.begin(), k.end());
        return k;
    };

    // Layers: `layer_of[q]` the part q repeats (-1: none), `to_layer[q]` each of that part's vertices -> q's at its
    // place, `flipped[q]` whether q's triangles face the other way.
    std::vector<int> layer_of(parts.size(), -1);
    std::vector<std::map<uint32_t, uint32_t>> to_layer(parts.size());
    std::vector<char> flipped(parts.size(), 0);
    for (size_t q = 1; q < parts.size(); q++)
        for (size_t p = 0; p < q && layer_of[q] < 0; p++) {
            if (layer_of[p] >= 0 || own[p].empty() || own[p].size() != own[q].size()) continue;
            std::map<std::array<Point, 3>, std::vector<size_t>> below;
            for (size_t t = 0; t + 2 < own[p].size(); t += 3) below[corners(own[p], t)].push_back(t);
            std::map<uint32_t, uint32_t> map;
            int facing = 0;   // 1 the same way, 2 the other way, 3 both: not a layer
            bool ok = true;
            for (size_t t = 0; ok && t + 2 < own[q].size(); t += 3) {
                auto it = below.find(corners(own[q], t));
                if (it == below.end() || it->second.empty()) { ok = false; break; }
                size_t b = it->second.back();
                it->second.pop_back();
                int same = 0;
                for (int j = 0; j < 3; j++)
                    for (int k = 0; k < 3; k++)
                        if (at(own[p][b + j]) == at(own[q][t + k])) {
                            map.emplace(own[p][b + j], own[q][t + k]);
                            if (at(own[p][b + (j + 1) % 3]) == at(own[q][t + (k + 1) % 3])) same++;
                        }
                facing |= same ? 1 : 2;
            }
            if (!ok || facing == 3) continue;
            layer_of[q] = (int)p;
            to_layer[q] = std::move(map);
            flipped[q] = facing == 2;
        }
    std::vector<uint32_t> list;
    std::set<Point> unseen_at;   // (where triangles never on screen were: the edge with the rest stays)
    int unseen = 0;
    for (size_t p = 0; p < parts.size(); p++) {
        if (layer_of[p] >= 0) continue;
        if (seen && (*seen)[p].size() == own[p].size() / 3) {
            for (size_t t = 0; t + 2 < own[p].size(); t += 3) {
                if ((*seen)[p][t / 3] > 0) {
                    list.insert(list.end(), {own[p][t], own[p][t + 1], own[p][t + 2]});
                } else {
                    unseen++;
                    for (int j = 0; j < 3; j++) unseen_at.insert(at(own[p][t + j]));
                }
            }
        } else {
            list.insert(list.end(), own[p].begin(), own[p].end());
        }
    }
    if (unseen_out) *unseen_out = unseen;
    if (list.size() / 3 < 24 || n == 0) {   // too small to be worth it
        if (!unseen) return g;
        if (list.empty()) return geo;   // (nothing seen at all: the measurement didn't fit it)
    }
    std::vector<float> normal(n * 3);
    for (size_t i = 0; i < n; i++)
        for (int c = 0; c < 3; c++) normal[i * 3 + c] = attr[i * 5 + 2 + c];
    std::map<uint32_t, std::vector<uint32_t>> copies_of;   // a vertex standing for copies (hard edges) -> them
    if (top) {
        std::map<std::tuple<uint32_t, Point, float, float, uint64_t>, uint32_t> one;
        for (auto &i : list) {
            auto key = std::make_tuple(part_of[i], at(i), attr[i * 5], attr[i * 5 + 1], bone[i]);
            auto [it, fresh] = one.emplace(key, i);
            auto &c = copies_of[it->second];
            if (std::find(c.begin(), c.end(), i) == c.end()) c.push_back(i);
            i = it->second;
        }
        for (auto &[r, c] : copies_of) {
            if (c.size() < 2) continue;
            double a[3] = {0, 0, 0};
            for (uint32_t i : c)
                for (int k = 0; k < 3; k++) a[k] += normal[i * 3 + k];
            double l = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
            if (l > 1e-9)
                for (int k = 0; k < 3; k++) attr[r * 5 + 2 + k] = (float)(a[k] / l);
        }
    }
    // Texture coordinates and normals count like changes in shape. The simplifier weighs a change in them by the
    // size of the triangles it spans (a change of 1 across a triangle d wide counts like moving it d), so for a most
    // detailed version they weigh the same whatever the model's size: otherwise a small model's texture could slide
    // (the Precipice plasma rifle's orange coil, mapped mirrored across its strip, lost its colour at 0.06%).
    // Lower versions keep the weights they were made with (the LOD library's versions stay valid): 2% of the texture
    // like 1% of the model's size, about 11 degrees of a normal like 1%.
    float w[5] = {(float)(size * 0.5), (float)(size * 0.5), (float)(size * 0.05), (float)(size * 0.05),
                  (float)(size * 0.05)};
    if (top) {
        const float weights[5] = {1.0f, 1.0f, 0.15f, 0.15f, 0.15f};
        std::copy(weights, weights + 5, w);
    }
    // Vertices that stay where they are: where pieces moved by different bones meet (a joint, or a wheel and the body:
    // see Bones above). Copies at one position (a model's vertices are mostly copies, along hard edges, texture seams
    // and where parts meet) stay paired, except along hard edges (lower versions): those may smooth over, which
    // doesn't show from a distance.
    std::vector<unsigned char> lock(n, 0);
    for (size_t t = 0; t + 2 < list.size(); t += 3)
        if (bone[list[t]] != bone[list[t + 1]] || bone[list[t]] != bone[list[t + 2]])
            lock[list[t]] = lock[list[t + 1]] = lock[list[t + 2]] = meshopt_SimplifyVertex_Lock;
    if (how != kTop) {
        std::map<Point, std::vector<size_t>> copies;
        for (size_t i = 0; i < n; i++)
            if (layer_of[part_of[i]] < 0) copies[at((uint32_t)i)].push_back(i);
        for (auto &[pt, vs] : copies) {
            bool keep = false;
            for (size_t k = 1; k < vs.size() && !keep; k++)
                keep = (part_of[vs[k]] != part_of[vs[0]] && layer_of[part_of[vs[k]]] < 0 && layer_of[part_of[vs[0]]] < 0) ||
                       std::fabs(attr[vs[k] * 5] - attr[vs[0] * 5]) > 1e-5f ||
                       std::fabs(attr[vs[k] * 5 + 1] - attr[vs[0] * 5 + 1]) > 1e-5f;
            if (keep)
                for (size_t i : vs) lock[i] |= meshopt_SimplifyVertex_Protect;
        }
    }
    if (top) {
        // Two-sided surfaces (a fin, a grille, glass: each triangle twice, back to back, on copies of its vertices)
        // stay as they are: to the simplifier a sheet with no volume, which it could fold away almost for free.
        std::map<std::array<Point, 3>, int> faces;
        for (size_t t = 0; t + 2 < list.size(); t += 3) faces[corners(list, t)]++;
        std::set<Point> sheet;
        for (auto &[k, c] : faces)
            if (c > 1) sheet.insert(k.begin(), k.end());
        if (!sheet.empty())
            for (size_t i = 0; i < n; i++)
                if (sheet.count(at((uint32_t)i))) lock[i] = meshopt_SimplifyVertex_Lock;
    }

    // Cut-out parts: every vertex stays.
    if (keep)
        for (size_t i = 0; i < n; i++) {
            int sh = (int)tagfile::get_num(parts[part_of[i]], "shader index");
            if (sh >= 0 && (size_t)sh < keep->size() && (*keep)[(size_t)sh]) lock[i] = meshopt_SimplifyVertex_Lock;
        }
    // Where what's never seen was taken out: the edge stays.
    if (!unseen_at.empty())
        for (uint32_t i : list)
            if (unseen_at.count(at(i))) lock[i] = meshopt_SimplifyVertex_Lock;

    // (Sparse when some vertices aren't in the list, a layer's or the copies standing behind one: otherwise the
    // simplifier counts them as more copies at their place, and a place with too many it never moves.)
    bool sparse = top || unseen || std::find_if(layer_of.begin(), layer_of.end(), [](int l) { return l >= 0; }) != layer_of.end();
    // (the target: the share of the triangles as made, the unseen ones included)
    size_t made = 0;
    for (size_t p = 0; p < parts.size(); p++)
        if (layer_of[p] < 0) made += own[p].size();
    size_t target = std::max<size_t>(3, (size_t)(made / 3 * level.ratio) * 3);
    const unsigned options = (how == kTop ? 0u : (unsigned)meshopt_SimplifyPermissive) | (sparse ? (unsigned)meshopt_SimplifySparse : 0u) |
                             meshopt_SimplifyErrorAbsolute;
    std::vector<uint32_t> out = list;
    float err = 0;
    if (seen && out.size() > target) {   // (first what's seen least, with what's seen more held still)
        std::vector<unsigned char> held = lock;
        for (size_t p = 0; p < parts.size(); p++) {
            if (layer_of[p] >= 0 || (*seen)[p].size() != own[p].size() / 3) continue;
            for (size_t t = 0; t + 2 < own[p].size(); t += 3)
                if ((*seen)[p][t / 3] >= 0.15f)
                    for (int j = 0; j < 3; j++) held[own[p][t + j]] = meshopt_SimplifyVertex_Lock;
        }
        std::vector<uint32_t> first(out.size());
        first.resize(meshopt_simplifyWithAttributes(first.data(), out.data(), out.size(), pos.data(), n, 12, attr.data(), 20, w, 5,
                                                    held.data(), target, (float)(size * level.error * 3), options, &err));
        out = std::move(first);
    }
    if (out.size() > target) {
        std::vector<uint32_t> next(out.size());
        next.resize(meshopt_simplifyWithAttributes(next.data(), out.data(), out.size(), pos.data(), n, 12, attr.data(), 20, w, 5,
                                                   lock.data(), target, (float)(size * level.error), options, &err));
        out = std::move(next);
    }
    // Hard edges: each corner takes the copy whose normal is nearest its triangle's facing.
    if (top)
        for (size_t t = 0; t + 2 < out.size(); t += 3) {
            const float *p0 = &pos[out[t] * 3], *p1 = &pos[out[t + 1] * 3], *p2 = &pos[out[t + 2] * 3];
            float u[3], v[3], f[3];
            for (int k = 0; k < 3; k++) { u[k] = p1[k] - p0[k]; v[k] = p2[k] - p0[k]; }
            f[0] = u[1] * v[2] - u[2] * v[1];
            f[1] = u[2] * v[0] - u[0] * v[2];
            f[2] = u[0] * v[1] - u[1] * v[0];
            for (int j = 0; j < 3; j++) {
                auto it = copies_of.find(out[t + j]);
                if (it == copies_of.end() || it->second.size() < 2) continue;
                float best = -1e30f;
                for (uint32_t c : it->second) {
                    float d = normal[c * 3] * f[0] + normal[c * 3 + 1] * f[1] + normal[c * 3 + 2] * f[2];
                    if (d > best) { best = d; out[t + j] = c; }
                }
            }
        }
    // Back into parts: each triangle to its vertices' part (a triangle whose vertices ended up in different parts
    // would be a simplifier mistake: then the geometry stays as it was).
    std::vector<std::vector<uint32_t>> per(parts.size());
    for (size_t t = 0; t + 2 < out.size(); t += 3) {
        uint32_t p = part_of[out[t]];
        if (part_of[out[t + 1]] != p || part_of[out[t + 2]] != p) return geo;
        per[p].insert(per[p].end(), {out[t], out[t + 1], out[t + 2]});
    }
    // The layers: their part's triangles again, on their own vertices.
    for (size_t q = 0; q < parts.size(); q++) {
        if (layer_of[q] < 0) continue;
        const std::vector<uint32_t> &below = per[layer_of[q]];
        for (size_t t = 0; t + 2 < below.size(); t += 3) {
            uint32_t v[3];
            for (int j = 0; j < 3; j++) {
                auto it = to_layer[q].find(below[t + j]);
                if (it == to_layer[q].end()) return geo;
                v[j] = it->second;
            }
            if (flipped[q]) std::swap(v[1], v[2]);
            per[q].insert(per[q].end(), {v[0], v[1], v[2]});
        }
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

bool add_skip(const Settings &settings, const std::string &model_tag, const std::string &comment) {
    if (settings.library.empty()) return false;
    std::error_code ec;
    fs::create_directories(settings.library, ec);
    fs::path f = settings.library / "skip.txt";
    bool fresh = !fs::exists(f);
    std::string before;
    if (!fresh) {   // (a line of its own, even when the file doesn't end with one)
        std::ifstream in(f, std::ios::binary);
        before.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    std::ofstream out(f, std::ios::binary | std::ios::app);
    if (fresh)
        out << "# Models Halo Map Porter leaves as they are (not trimmed, no lower-detail versions made, no switch distances\r\n"
               "# given):\r\n"
               "# one per line, a tag path or a name. Lines starting with # are notes.\r\n";
    else if (!before.empty() && before.back() != '\n')
        out << "\r\n";
    if (!comment.empty()) out << "# " << comment << "\r\n";
    out << model_tag << "\r\n";
    return (bool)out;
}

// choices.txt: one line per model, tab separated: its tag path; lower-detail versions ("auto", "none", or "make" and
// the medium and low triangles, 0 automatic); most detailed version ("auto", "as made", or "trim" and the triangles);
// then a note ("# name (map)").
static const char *kChoicesHeader =
    "# Halo Map Porter: what to do with these models (chosen in the LODs window), for every map that uses them.\r\n"
    "# One line per model, tab separated: its tag path; lower-detail versions: auto, none, or make and the medium\r\n"
    "# and low triangles to aim for (0: automatic); its most detailed version: auto, as made, or trim and the\r\n"
    "# triangles. Lines starting with # are notes.\r\n";

static std::vector<std::string> split_tabs(const std::string &l) {
    std::vector<std::string> f;
    for (size_t a = 0;;) {
        size_t b = l.find('\t', a);
        std::string x = l.substr(a, b == std::string::npos ? std::string::npos : b - a);
        while (!x.empty() && (x.back() == ' ' || x.back() == '\r')) x.pop_back();
        while (!x.empty() && x[0] == ' ') x.erase(0, 1);
        f.push_back(x);
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return f;
}

static std::string tag_key(std::string t) {
    t = lower(t);
    std::replace(t.begin(), t.end(), '\\', '/');
    return t;
}

Choice choice(const Settings &settings, const std::string &model_tag, const std::string &object_name) {
    Choice c;
    if (settings.library.empty()) return c;
    c.skip = skipped(settings, model_tag, object_name);
    if (c.skip) c.lods = 'n', c.top = 'k';
    std::ifstream in(settings.library / "choices.txt", std::ios::binary);
    std::string l, key = tag_key(model_tag);
    while (std::getline(in, l)) {
        if (l.empty() || l[0] == '#') continue;
        std::vector<std::string> f = split_tabs(l);
        if (f.size() < 3 || tag_key(f[0]) != key) continue;
        Choice got;
        got.skip = c.skip;
        got.chosen = true;
        std::istringstream a(lower(f[1])), b(lower(f[2]));
        std::string w;
        a >> w;
        if (w == "none") got.lods = 'n';
        else if (w == "make") {
            got.lods = 'm';
            a >> got.medium >> got.low;
            got.medium = std::max(0, got.medium);
            got.low = std::max(0, got.low);
        }
        b >> w;
        if (w == "as") got.top = 'k';
        else if (w == "trim" && (b >> got.top_triangles) && got.top_triangles > 0) got.top = 't';
        return got;
    }
    return c;
}

bool set_choice(const Settings &settings, const std::string &model_tag, const Choice &c, const std::string &note) {
    if (settings.library.empty()) return false;
    std::error_code ec;
    fs::create_directories(settings.library, ec);
    fs::path f = settings.library / "choices.txt";
    std::vector<std::string> lines;
    {
        std::ifstream in(f, std::ios::binary);
        std::string l;
        while (std::getline(in, l)) {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            lines.push_back(l);
        }
    }
    if (lines.empty()) {
        std::string h = kChoicesHeader;
        for (size_t a = 0; a < h.size();) {
            size_t b = h.find("\r\n", a);
            lines.push_back(h.substr(a, b - a));
            a = b + 2;
        }
    }
    std::string key = tag_key(model_tag), line;
    bool automatic = c.lods == 'a' && c.top == 'a';
    if (!automatic || skipped(settings, model_tag, "")) {
        line = model_tag + "\t";
        if (c.lods == 'n') line += "none";
        else if (c.lods == 'm') line += "make " + std::to_string(std::max(0, c.medium)) + " " + std::to_string(std::max(0, c.low));
        else line += "auto";
        line += "\t";
        if (c.top == 'k') line += "as made";
        else if (c.top == 't' && c.top_triangles > 0) line += "trim " + std::to_string(c.top_triangles);
        else line += "auto";
        if (!note.empty()) line += "\t# " + note;
    }
    bool placed = false;
    for (size_t i = 0; i < lines.size(); i++) {
        if (lines[i].empty() || lines[i][0] == '#') continue;
        std::vector<std::string> fl = split_tabs(lines[i]);
        if (tag_key(fl[0]) != key) continue;
        if (!placed && !line.empty()) {
            lines[i] = line;
            placed = true;
        } else {
            lines.erase(lines.begin() + (long)i);
            i--;
        }
    }
    if (!placed && !line.empty()) lines.push_back(line);
    std::string out;
    for (auto &l : lines) out += l + "\r\n";
    std::ofstream o(f, std::ios::binary | std::ios::trunc);
    o << out;
    return (bool)o;
}

// The versions of one geometry: from the library, or made now (and kept there).
// `levels`: what to aim for (other than kLevels: keys of their own).
static std::string keep_text(const std::vector<char> &keep) {   // (for library keys: "" when nothing's kept as made)
    std::string t;
    for (char k : keep) t += k ? '1' : '0';
    return t.find('1') == std::string::npos ? "" : "|cut " + t;
}
static std::vector<TagNode> versions(const TagNode &geo, uint32_t checksum, double size, const Level *levels,
                                     const Settings &settings, bool &from_library, std::string &key,
                                     const std::vector<char> &keep = {}) {
    std::string key_text = std::string(kVersion) + "|" + std::to_string(checksum) + "|" + tagfile::write_node(geo) + keep_text(keep);
    bool usual = true;
    for (int i = 0; i < 2; i++) usual = usual && levels[i].ratio == kLevels[i].ratio && levels[i].error == kLevels[i].error;
    if (!usual) {
        char params[96];
        snprintf(params, sizeof params, "|chosen %.4f %.4f %.4f %.4f", levels[0].ratio, levels[0].error, levels[1].ratio,
                 levels[1].error);
        key_text += params;
    }
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
    for (int i = 0; i < 2; i++) out.push_back(simplify_geometry(geo, levels[i], size, kLower, keep.empty() ? nullptr : &keep));
    if (!file.empty()) {
        std::error_code ec;
        fs::create_directories(settings.library, ec);
        std::string b = "HMPLOD1\n";
        for (auto &g : out) b += tagfile::write_node(g);
        std::ofstream(file, std::ios::binary) << b;
    }
    return out;
}

// A line in the library's index.txt, its record of what it holds.
static void index_line(const Settings &settings, const std::string &model, const std::string &what, const std::string &files) {
    std::time_t now = std::time(nullptr);
    char date[16];
    std::strftime(date, sizeof date, "%Y-%m-%d", std::localtime(&now));
    bool fresh = !fs::exists(settings.library / "index.txt");
    std::ofstream idx(settings.library / "index.txt", std::ios::app | std::ios::binary);
    if (fresh)
        idx << "# The LOD library: what Halo Map Porter made, one line per model (date, model, triangles: most detailed >\r\n"
               "# medium > low, or \"trimmed\" as made > now for a lighter most detailed version; its files here). Delete a\r\n"
               "# file to have it made again.\r\n";
    idx << date << "\t" << model << "\t" << what << "\t" << files << "\r\n";
}

// The diagonal of the box around these geometries.
static double geometries_size(std::vector<TagNode> &geos, const std::set<int> &which) {
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (int g : which)
        for (auto &p : *tagfile::block(geos[g], "parts"))
            for (auto &v : *tagfile::block(p, "uncompressed vertices"))
                for (int k = 0; k < 3; k++) {
                    float x = f32(v.fixed, k * 4);
                    lo[k] = std::min(lo[k], x);
                    hi[k] = std::max(hi[k], x);
                }
    if (lo[0] > hi[0]) return 0;
    return std::sqrt((double)(hi[0] - lo[0]) * (hi[0] - lo[0]) + (double)(hi[1] - lo[1]) * (hi[1] - lo[1]) +
                     (double)(hi[2] - lo[2]) * (hi[2] - lo[2]));
}

static const char *kLevelNames[5] = {"super high", "high", "medium", "low", "super low"};

// Per model shader: 1 when it's drawn cut out (Settings::look), its parts kept as made. Empty without a look-up.
static std::vector<char> cut_out_shaders(const fs::path &model, const Settings &settings) {
    std::vector<char> keep;
    if (!settings.look) return keep;
    tagfile::Tag t;
    if (!t.load(model)) return keep;
    tagfile::Block sh = t.root().block("shaders");
    for (size_t i = 0; i < sh.size(); i++) {
        std::string ref = sh[i].ref("shader");
        keep.push_back(!ref.empty() && settings.look(ref).kind == ShaderLook::kCutOut);
    }
    if (std::find(keep.begin(), keep.end(), 1) == keep.end()) keep.clear();
    return keep;
}
// The triangles of a geometry's parts drawn with those shaders.
static int kept_triangles(TagNode geo, const std::vector<char> &keep) {
    int n = 0;
    for (auto &p : *tagfile::block(geo, "parts")) {
        int sh = (int)tagfile::get_num(p, "shader index");
        if (sh >= 0 && (size_t)sh < keep.size() && keep[(size_t)sh])
            n += (int)strip_to_list(*tagfile::block(p, "triangles"), tagfile::block(p, "uncompressed vertices")->size()).size() / 3;
    }
    return n;
}

// Bungie's stock models (every object in the Xbox tags: each one's most detailed triangles against the diagonal of the
// box around them), fitted as a x size^b on a log scale. Their volume doesn't predict them (a sniper rifle is about
// five times a pistol's length and has less than twice its triangles): Bungie gave each kind a budget that grows
// slowly with size.
//   first-person weapons: 2,415 x size^0.234 (pistol 1,558, assault rifle 1,964, sniper rifle 2,236; all within 25%)
//   weapons and items: 1,105 x size^0.182 (pistol 787, assault rifle 764, sniper rifle 1,380, rocket launcher 1,468;
//     within 40%; the flag and the energy sword left out)
//   characters: no trend with size; the main ones about 2,000 (the Chief 2,016, Elite 1,670, Jackal 2,221, Hunter
//     2,006)
//   vehicles vary 3x at one size (Ghost 1,034, Warthog 2,798): the line through the most detailed, the Warthog and
//     the Pelican (4,469), 2,212 x size^0.27 (a vehicle you drive is big on screen)
//   scenery and devices vary most (a rock 300, the Pelican wreck 4,277): the line 3 in 4 are under, 457 x size^0.30
double stock_triangles(char kind, double size) {
    size = std::max(size, 0.01);
    switch (kind) {
    case 'f': return 2415 * std::pow(size, 0.234);
    case 'w': return 1105 * std::pow(size, 0.182);
    case 'b': return 2000;
    case 'v': return 2212 * std::pow(size, 0.27);
    default: return 457 * std::pow(size, 0.30);
    }
}

// The diagonal of the box around every permutation's most detailed geometry.
static double top_size(std::vector<TagNode> &geos, std::vector<TagNode> &regions, std::set<int> *tops_out = nullptr) {
    std::set<int> tops;
    for (auto &reg : regions)
        for (auto &perm : *tagfile::block(reg, "permutations")) {
            int g = (int)tagfile::get_num(perm, "super high");
            if (g >= 0 && g < (int)geos.size()) tops.insert(g);
        }
    if (tops_out) *tops_out = tops;
    return geometries_size(geos, tops);
}

double model_size(const fs::path &model) {
    tagfile::EditTag t;
    if (!t.load(model)) return 0;
    std::vector<TagNode> *geos = tagfile::block(t.root(), "geometries"), *regions = tagfile::block(t.root(), "regions");
    if (!geos || !regions) return 0;
    return top_size(*geos, *regions);
}

// The most detailed triangles: the regions' first permutations' super high geometries.
static int top_triangles(std::vector<TagNode> &geos, std::vector<TagNode> &regions) {
    int n = 0;
    for (auto &reg : regions) {
        auto &perms = *tagfile::block(reg, "permutations");
        int g = perms.empty() ? -1 : (int)tagfile::get_num(perms[0], "super high");
        if (g >= 0 && g < (int)geos.size()) n += list_triangles(geos[g]);
    }
    return n;
}

// One geometry trimmed: from the library, or made now (and kept there).
static TagNode trimmed_version(const TagNode &geo, uint32_t checksum, double ratio, double size, double error, How how,
                               const Settings &settings, bool &from_library, std::string &key, const std::vector<char> &keep = {},
                               const std::vector<std::vector<float>> *seen = nullptr, int *unseen = nullptr) {
    char params[96];
    snprintf(params, sizeof params, "|%.4f|%.6g|%d|", ratio, size * error, (int)how);
    std::string seen_text;
    if (seen) {   // (what's on screen how often, in steps of 1%: a measurement that changes only a little reuses it)
        seen_text = "|seen";
        for (auto &part : *seen) {
            seen_text += '|';
            for (float v : part) seen_text += v <= 0 ? '0' : (char)('1' + std::min(99, (int)(v * 100)) / 10);
        }
    }
    key = hex(fnv(std::string(kTrimVersion) + "|" + std::to_string(checksum) + params + tagfile::write_node(geo) + keep_text(keep) +
                  seen_text));
    fs::path file = settings.library.empty() ? fs::path() : settings.library / (key + ".top");
    if (!file.empty() && fs::exists(file)) {
        std::ifstream in(file, std::ios::binary);
        std::string b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        size_t pos = 8;
        TagNode n;
        if (b.compare(0, 8, "HMPTOP1\n") == 0 && tagfile::read_node(b, pos, n) && pos == b.size()) {
            from_library = true;
            if (unseen) *unseen = -1;   // (unknown: from the library)
            return n;
        }
    }
    TagNode out = simplify_geometry(geo, Level{ratio, error}, size, how, keep.empty() ? nullptr : &keep, seen, unseen);
    if (!file.empty()) {
        std::error_code ec;
        fs::create_directories(settings.library, ec);
        std::ofstream(file, std::ios::binary) << "HMPTOP1\n" << tagfile::write_node(out);
    }
    return out;
}

Trim trim_top(const fs::path &model, const TrimSettings &ts, const Settings &settings, const std::string &tag) {
    Trim r;
    tagfile::EditTag t;
    if (!t.load(model)) { r.why = "couldn't be read"; return r; }
    TagNode &root = t.root();
    std::vector<TagNode> *geos = tagfile::block(root, "geometries"), *regions = tagfile::block(root, "regions");
    if (!geos || !regions) { r.why = "not a model"; return r; }
    r.before = r.after = top_triangles(*geos, *regions);
    r.budget = ts.budget;
    // (with what's on screen measured, what never is goes even within the budget)
    const bool cull = ts.seen && !ts.seen->geometry.empty();
    if (ts.density <= 0 && r.before <= ts.budget && !cull) { r.why = "within its budget"; return r; }

    // Every permutation's most detailed geometry is trimmed by the same share (variants like "~damaged" too), so the
    // model as a whole comes down to its budget; how far is up to how much each one can lose and still look the same.
    std::set<int> tops;
    double size = top_size(*geos, *regions, &tops);
    if (ts.density > 0) {
        r.budget = std::max(1, (int)std::lround(ts.density * stock_triangles(ts.kind, size)));
        if (r.before <= r.budget * ts.slack && !cull) { r.why = "within its budget"; return r; }
    }
    if (!(size > 0)) { r.why = "has no size"; return r; }
    double ratio = std::min(1.0, (double)r.budget / r.before), error = std::min(ts.max_change, ts.max_units / size);
    if (r.before <= r.budget * ts.slack) ratio = 1;   // (only what's never seen goes)
    const std::vector<char> keep = cut_out_shaders(model, settings);
    uint32_t checksum = (uint32_t)(long long)tagfile::get_num(root, "node list checksum");
    std::vector<int> tris(geos->size(), -1);
    auto count = [&](int g) {
        if (tris[g] < 0) tris[g] = list_triangles((*geos)[g]);
        return tris[g];
    };
    size_t bytes_before = 0, bytes_after = 0;
    std::string files;
    bool any = false;
    for (int g : tops) {
        int full = count(g);
        if (full < 24) continue;
        bool lib = false;
        std::string key;
        const std::vector<std::vector<float>> *seen = nullptr;
        if (cull) {
            auto it = ts.seen->geometry.find(g);
            if (it != ts.seen->geometry.end()) seen = &it->second;
        }
        int unseen = 0;
        TagNode v = trimmed_version((*geos)[g], checksum, ratio, size, error, kTop, settings, lib, key, keep, seen, &unseen);
        int n = list_triangles(v);
        if (n <= 0 || n > full * 0.95) continue;   // (hardly lighter: kept as it was)
        if (seen)
            for (auto &part : *seen)
                for (float x : part) r.unseen += x <= 0;
        if (lib) r.from_library = true;
        else files += (files.empty() ? "" : " ") + key + ".top";
        bytes_before += geometry_xbox_bytes((*geos)[g]);
        bytes_after += geometry_xbox_bytes(v);
        (*geos)[g] = std::move(v);
        tris[g] = n;
        any = true;
    }
    if (!any) {
        r.why = cull && r.before <= r.budget * ts.slack ? "within its budget, and nearly all of it shows in first person"
                                                       : "can't lose triangles without changing its shape or how its textures sit";
        return r;
    }

    // A lower level that's now as detailed as the level above it (made for the old, heavier version) draws that one
    // instead; geometries nothing draws any more are dropped.
    for (auto &reg : *regions)
        for (auto &perm : *tagfile::block(reg, "permutations")) {
            int above = (int)tagfile::get_num(perm, "super high");
            for (int lv = 1; lv < 5; lv++) {
                int g = (int)tagfile::get_num(perm, kLevelNames[lv]);
                if (g < 0 || g >= (int)geos->size() || above < 0 || above >= (int)geos->size()) break;
                if (g != above && !tops.count(g) && count(g) >= count(above) * 0.9) {
                    tagfile::set_num(perm, kLevelNames[lv], above);
                    g = above;
                }
                above = g;
            }
        }
    std::vector<int> used(geos->size(), 0);
    for (auto &reg : *regions)
        for (auto &perm : *tagfile::block(reg, "permutations"))
            for (auto lvn : kLevelNames) {
                int g = (int)tagfile::get_num(perm, lvn);
                if (g >= 0 && g < (int)geos->size()) used[g] = 1;
            }
    if (std::find(used.begin(), used.end(), 0) != used.end()) {
        std::vector<int> remap(geos->size(), -1);
        std::vector<TagNode> kept;
        for (size_t g = 0; g < geos->size(); g++) {
            if (!used[g]) { bytes_before += geometry_xbox_bytes((*geos)[g]); continue; }
            remap[g] = (int)kept.size();
            kept.push_back(std::move((*geos)[g]));
        }
        *geos = std::move(kept);
        for (auto &reg : *regions)
            for (auto &perm : *tagfile::block(reg, "permutations"))
                for (auto lvn : kLevelNames) {
                    int g = (int)tagfile::get_num(perm, lvn);
                    if (g >= 0 && g < (int)remap.size()) tagfile::set_num(perm, lvn, remap[g]);
                }
    }
    r.after = top_triangles(*geos, *regions);
    if (!t.save(model)) { r.why = "couldn't be saved"; r.after = r.before; return r; }
    r.changed = true;
    r.xbox_bytes_saved = bytes_before > bytes_after ? bytes_before - bytes_after : 0;
    if (!settings.library.empty() && !files.empty())
        index_line(settings, tag.empty() ? model.filename().u8string() : tag,
                   "trimmed " + std::to_string(r.before) + " > " + std::to_string(r.after), files);
    return r;
}

Result add_lods(const fs::path &model, const double cutoffs[5], const Settings &settings, const std::string &tag,
                const int targets[2]) {
    Result r;
    tagfile::EditTag t;
    if (!t.load(model)) { r.why = "couldn't be read"; return r; }
    TagNode &root = t.root();
    std::vector<TagNode> *geos = tagfile::block(root, "geometries"), *regions = tagfile::block(root, "regions");
    if (!geos || !regions) { r.why = "not a model"; return r; }
    static const char *slots[5] = {"super low", "low", "medium", "high", "super high"};

    // Geometries to make lower-detail versions of: a permutation's most detailed one when every level draws it (none
    // yet), or when its lower levels draw another permutation's most detailed geometry (a variant, like one of a
    // tree's four shapes, drawing the first shape at a distance): that one gets lower levels of its own, so each
    // variant keeps its shape at every distance.
    std::set<int> single;
    std::set<std::pair<size_t, size_t>> own_levels;   // (region, permutation) whose borrowed levels get their own
    bool mixed = false;   // a permutation with lower-detail versions of its own
    for (size_t ri = 0; ri < regions->size(); ri++) {
        auto &perms = *tagfile::block((*regions)[ri], "permutations");
        std::set<int> tops;
        for (auto &perm : perms) tops.insert((int)tagfile::get_num(perm, "super high"));
        for (size_t pi = 0; pi < perms.size(); pi++) {
            auto &perm = perms[pi];
            int g = (int)tagfile::get_num(perm, "super high");
            bool same = true, borrowed = true;
            for (auto s : slots) {
                int l = (int)tagfile::get_num(perm, s);
                same = same && l == g;
                borrowed = borrowed && tops.count(l);
            }
            std::string name = tagfile::get_str(perm, "name");
            bool special = !name.empty() && name[0] == '~';   // ("~blur": the engine's pick, can switch)
            if (g < 0 || g >= (int)geos->size()) continue;
            if (same) single.insert(g);
            else if (borrowed && !special) {
                single.insert(g);
                own_levels.insert({ri, pi});
            } else if (!special) mixed = true;
        }
    }
    // With no switch distances, the switch distances this sets would also start switching those (a rock's variants
    // drawing another variant's geometry at a distance): leave such a model alone. Permutations the engine picks
    // itself ("~blur", a moving tire) can switch to another's geometry at a distance.
    // (No switch distances: super high to low all 0. Super low is where it stops being drawn, not a switch: a model
    // with only that set still always draws its most detailed version, so it gets the stock-style ones too, keeping
    // its super low.)
    static const char *cutoff_names[5] = {"super high detail cutoff", "high detail cutoff", "medium detail cutoff",
                                          "low detail cutoff", "super low detail cutoff"};
    bool no_cutoffs = true;
    for (int i = 0; i < 4; i++) no_cutoffs = no_cutoffs && tagfile::get_num(root, cutoff_names[i]) == 0;
    if (mixed && no_cutoffs) { r.why = "its other permutations' levels would start switching"; return r; }
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

    // How far each version may move the shape: about 1.5 pixels where it's first drawn (the model that many pixels
    // across: the "high" switch distance for the medium version, "medium" for the low one), never less than 1% and
    // 2.5% of its size (characters, seen from 200 and 80 pixels). Scenery (90 and 30 pixels): 1.7% and 5%; vehicles
    // (100 and 50) 1.5% and 3%; weapons and items (50 and 25) 3% and 6%.
    Level levels[2] = {kLevels[0], kLevels[1]};
    double px[5];
    for (int i = 0; i < 5; i++) px[i] = no_cutoffs ? cutoffs[i] : tagfile::get_num(root, cutoff_names[i]);
    if (px[1] > 0) levels[0].error = std::max(kLevels[0].error, std::min(0.06, 1.5 / px[1]));
    if (px[2] > 0) levels[1].error = std::max(kLevels[1].error, std::min(0.08, 1.5 / px[2]));
    // Chosen targets: every geometry by the same share, allowed to move further (5% and 10% of the model's size) so
    // they get there; a level not chosen as usual.
    for (int i = 0; i < 2 && targets && r.triangles > 0; i++)
        if (targets[i] > 0) levels[i] = Level{std::min(1.0, (double)targets[i] / r.triangles), i == 0 ? 0.05 : 0.1};
    uint32_t checksum = (uint32_t)(long long)tagfile::get_num(root, "node list checksum");
    const std::vector<char> keep = cut_out_shaders(model, settings);
    std::map<int, int> medium, low;   // geometry -> the version used for medium / low and super low
    int base_tris = 0, tris[2] = {0, 0};
    bool any_a = false, any_b = false, made = false;
    std::string files;   // the library files (made now), for its index
    for (int g : single) {
        bool lib = false;
        std::string key;
        std::vector<TagNode> vs = versions((*geos)[g], checksum, size, levels, settings, lib, key, keep);
        r.from_library = r.from_library || lib;
        if (!lib) {
            made = true;
            files += (files.empty() ? "" : " ") + key + ".lod";
        }
        int full = list_triangles((*geos)[g]), a = list_triangles(vs[0]), b = list_triangles(vs[1]);
        base_tris += full;
        // A version is worth adding when it has noticeably fewer triangles (10%) than the one above it.
        bool use_a = a > 0 && a <= full * 0.9, use_b = b > 0 && b <= (use_a ? a : full) * 0.9;
        int ia = g, ib = g;
        if (use_a) { ia = (int)geos->size(); geos->push_back(vs[0]); r.bytes_added += geometry_bytes(vs[0]); r.xbox_bytes += geometry_xbox_bytes(vs[0]); tris[0] += a; any_a = true; }
        else tris[0] += full;
        if (use_b) { ib = (int)geos->size(); geos->push_back(vs[1]); r.bytes_added += geometry_bytes(vs[1]); r.xbox_bytes += geometry_xbox_bytes(vs[1]); tris[1] += b; any_b = true; }
        else tris[1] += use_a ? a : full;
        if (!use_b) ib = ia;
        medium[g] = ia;
        low[g] = ib;
    }
    // And the model as a whole: its lowest version 10% lighter than as made, or none (a tree whose leaf cards stay as
    // made, with only its trunk simplified, isn't worth the memory of another copy).
    if (r.bytes_added > 0 && base_tris > 0 && tris[1] > base_tris * 0.9) {
        r.bytes_added = 0;
        r.xbox_bytes = 0;
    }
    if (r.bytes_added == 0) {
        int cut = 0, all = 0;
        for (int g : single) {
            cut += keep.empty() ? 0 : kept_triangles((*geos)[g], keep);
            all += list_triangles((*geos)[g]);
        }
        r.why = all > 0 && cut * 2 >= all ? "most of it is cut-out cards (leaves, grates), which stay as made"
                                          : "couldn't be simplified much (its shape needs most of its triangles)";
        return r;
    }
    if (geos->size() > 256) { r.why = "would have too many geometries"; return r; }
    for (size_t ri = 0; ri < regions->size(); ri++) {
        auto &perms = *tagfile::block((*regions)[ri], "permutations");
        for (size_t pi = 0; pi < perms.size(); pi++) {
            auto &perm = perms[pi];
            int g = (int)tagfile::get_num(perm, "super high");
            bool own = own_levels.count({ri, pi}) > 0;
            if (!medium.count(g) || (!own && (int)tagfile::get_num(perm, "super low") != g)) continue;
            if (own) tagfile::set_num(perm, "high", g);
            tagfile::set_num(perm, "medium", medium[g]);
            tagfile::set_num(perm, "low", low[g]);
            tagfile::set_num(perm, "super low", low[g]);
        }
    }
    static const char *cut[5] = {"super high detail cutoff", "high detail cutoff", "medium detail cutoff", "low detail cutoff",
                                 "super low detail cutoff"};
    if (no_cutoffs) {
        for (int i = 0; i < 4; i++) tagfile::set_num(root, cut[i], cutoffs[i]);
        tagfile::set_num(root, cut[4], std::max(cutoffs[4], tagfile::get_num(root, cut[4])));
    }
    if (!t.save(model)) { r.why = "couldn't be saved"; return r; }
    r.changed = true;
    // The model's triangles as the game counts them (each region's first permutation), most detailed, medium, low.
    int levels_now[3] = {0, 0, 0};
    for (auto &reg : *regions) {
        auto &perms = *tagfile::block(reg, "permutations");
        if (perms.empty()) continue;
        const char *which[3] = {"super high", "medium", "low"};
        for (int k = 0; k < 3; k++) {
            int g = (int)tagfile::get_num(perms[0], which[k]);
            if (g >= 0 && g < (int)geos->size()) levels_now[k] += list_triangles((*geos)[g]);
        }
    }
    (void)base_tris;
    r.triangles = levels_now[0];
    r.level_triangles[0] = any_a ? levels_now[1] : 0;
    r.level_triangles[1] = any_b ? levels_now[2] : 0;
    if (!settings.library.empty() && made) {   // what's new in the library
        std::string tris_text = std::to_string(r.triangles);
        for (int k : r.level_triangles)
            if (k) tris_text += " > " + std::to_string(k);
        index_line(settings, tag.empty() ? model.filename().u8string() : tag, tris_text, files);
    }
    return r;
}

}   // namespace lod
