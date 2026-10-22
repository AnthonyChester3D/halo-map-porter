// Halo Map Porter - level geometry and model statistics (see perf.hpp)
#include "perf.hpp"
#include "tagfile.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace perf {

BspStats bsp_stats(const std::filesystem::path &bsp) {
    BspStats s;
    tagfile::Tag t;
    if (!t.load(bsp)) return s;
    tagfile::Elem r = t.root();
    tagfile::Block clusters = r.block("clusters");
    size_t n = clusters.size();
    s.clusters = (int)n;
    s.portals = (int)r.block("cluster portals").size();
    s.triangles = (int)r.block("surfaces").size();
    // Each cluster's surfaces, from its subclusters (a surface across two of them is in both).
    std::vector<std::vector<int>> surfaces(n);
    for (size_t i = 0; i < n; i++) {
        tagfile::Block sub = clusters[i].block("subclusters");
        for (size_t k = 0; k < sub.size(); k++) {
            tagfile::Block idx = sub[k].block("surface indices");
            for (size_t j = 0; j < idx.size(); j++) surfaces[i].push_back((int)idx[j].integer("index"));
        }
    }
    // The visibility data: one row of bits per cluster, a bit per cluster it can see. Tool writes the words in
    // the PC's byte order; read the other order if that's what makes every cluster see itself.
    std::string pvs = r.data("cluster data");
    size_t row = (n + 31) / 32 * 4;
    auto bit = [&](size_t from, size_t to, bool little) {
        size_t o = from * row + to / 32 * 4;
        if (o + 4 > pvs.size()) return true;   // no data: everything can be seen
        const unsigned char *p = (const unsigned char *)pvs.data() + o;
        uint32_t w = little ? (p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24)
                            : ((uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]);
        return ((w >> (to % 32)) & 1) != 0;
    };
    size_t self_le = 0, self_be = 0;
    for (size_t i = 0; i < n; i++) { self_le += bit(i, i, true); self_be += bit(i, i, false); }
    bool little = self_le >= self_be;
    std::vector<int> visible;
    std::vector<char> seen(std::max(s.triangles, 1), 0);
    for (size_t i = 0; i < n; i++) {
        std::fill(seen.begin(), seen.end(), 0);
        int count = 0;
        for (size_t j = 0; j < n; j++) {
            if (!bit(i, j, little)) continue;
            for (int sfc : surfaces[j])
                if (sfc >= 0 && sfc < s.triangles && !seen[sfc]) { seen[sfc] = 1; count++; }
        }
        visible.push_back(count);
    }
    if (visible.empty()) visible.push_back(s.triangles);   // no clusters: all of it
    std::sort(visible.begin(), visible.end());
    s.median_visible = visible[visible.size() / 2];
    s.worst_visible = visible.back();
    s.ok = true;
    return s;
}

// Triangles in a geometry: its parts' triangle strips (three indices per element, -1 pads the end), without the
// degenerate ones a strip uses to turn.
static int geometry_triangles(const tagfile::Elem &g) {
    int total = 0;
    tagfile::Block parts = g.block("parts");
    for (size_t p = 0; p < parts.size(); p++) {
        tagfile::Block tris = parts[p].block("triangles");
        std::vector<int> strip;
        strip.reserve(tris.size() * 3);
        for (size_t k = 0; k < tris.size(); k++)
            for (const char *v : {"vertex0 index", "vertex1 index", "vertex2 index"}) {
                int x = (int)tris[k].integer(v);
                if (x != 0xFFFF) strip.push_back(x);
            }
        for (size_t k = 2; k < strip.size(); k++)
            if (strip[k] != strip[k - 1] && strip[k] != strip[k - 2] && strip[k - 1] != strip[k - 2]) total++;
    }
    return total;
}

ModelStats model_stats(const std::filesystem::path &model) {
    ModelStats s;
    tagfile::Tag t;
    if (!t.load(model)) return s;
    tagfile::Elem r = t.root();
    const char *cut[5] = {"super high detail cutoff", "high detail cutoff", "medium detail cutoff", "low detail cutoff",
                          "super low detail cutoff"};
    for (int i = 0; i < 5; i++) s.cutoff[i] = r.num(cut[i]);
    tagfile::Block geos = r.block("geometries"), regions = r.block("regions");
    std::vector<int> tri(geos.size(), -1);
    auto count = [&](long long g) {
        if (g < 0 || g >= (long long)geos.size()) return 0;
        if (tri[g] < 0) tri[g] = geometry_triangles(geos[g]);
        return tri[g];
    };
    for (size_t i = 0; i < regions.size(); i++) {
        tagfile::Block perms = regions[i].block("permutations");
        std::vector<long long> tops;   // each permutation's most detailed geometry
        for (size_t k = 0; k < perms.size(); k++) tops.push_back(perms[k].integer("super high"));
        for (size_t k = 0; k < perms.size(); k++) {
            long long lv[5];
            const char *names[5] = {"super low", "low", "medium", "high", "super high"};
            for (int j = 0; j < 5; j++) lv[j] = perms[k].integer(names[j]);
            // A lower level with its own geometry is a lower-detail version; one that's another permutation's most
            // detailed geometry is that variant (one of a rock's 5 shapes), not a simpler version. A permutation the
            // engine picks itself ("~blur": a moving tire) can fall back to another's at a distance.
            std::string pname = perms[k].str("name");
            bool special = !pname.empty() && pname[0] == '~';
            for (int j = 0; j < 4; j++) {
                if (lv[j] == lv[4]) continue;
                if (std::find(tops.begin(), tops.end(), lv[j]) == tops.end()) s.lods = true;
                else if (!special) s.borrowed = true;
            }
            if (k == 0) {
                s.triangles += count(lv[4]);
                s.lowest_triangles += count(lv[0]);
            }
        }
    }
    s.ok = true;
    return s;
}

std::string object_model(const std::filesystem::path &object) {
    tagfile::Tag t;
    if (!t.load(object)) return "";
    return t.root().ref("model");
}

std::string weapon_first_person_model(const std::filesystem::path &weapon) {
    tagfile::Tag t;
    if (!t.load(weapon)) return "";
    return t.root().ref("first person model");
}

std::vector<WeatherType> weather_types(const std::filesystem::path &weather) {
    std::vector<WeatherType> v;
    tagfile::Tag t;
    if (!t.load(weather)) return v;
    tagfile::Block types = t.root().block("particle types");
    for (size_t i = 0; i < types.size(); i++) {
        WeatherType w;
        w.count_lo = types[i].num("particle count", 0);
        w.count_hi = types[i].num("particle count", 1);
        w.fade_in_start = types[i].num("fade in start distance");
        w.fade_in_end = types[i].num("fade in end distance");
        w.fade_out_start = types[i].num("fade out start distance");
        w.fade_out_end = types[i].num("fade out end distance");
        v.push_back(w);
    }
    return v;
}

double weather_wanted(const std::vector<WeatherType> &types) {
    double n = 0;
    for (auto &w : types) n += std::max(0.0, (w.count_lo + w.count_hi) / 2) * std::pow(std::max(0.0, w.fade_out_end), 3);
    return n;
}

double weather_drops(const std::vector<WeatherType> &types) { return std::min(weather_wanted(types), kMaxWeatherDrops); }

bool weather_lighten(std::vector<WeatherType> &types, int mode) {
    double wanted = weather_wanted(types);
    if (wanted <= 0) return false;
    if (mode == 3) {
        for (auto &w : types) w.count_lo = w.count_hi = 0;
        return true;
    }
    double s = std::min(1.0, kMaxWeatherDrops / wanted);   // (how much of what it asks for the engine keeps)
    bool changed = s < 0.999;
    for (auto &w : types) {
        w.count_lo *= s;
        w.count_hi *= s;
        if (mode == 2) {
            w.count_lo *= 0.5;
            w.count_hi *= 0.5;
            changed = true;
        } else if (mode == 1 && w.fade_out_end > kLighterWeatherEnd) {
            double k = kLighterWeatherEnd / w.fade_out_end;
            w.fade_out_start *= k;
            w.fade_out_end = kLighterWeatherEnd;
            w.fade_in_end = std::min(w.fade_in_end, w.fade_out_start);
            w.fade_in_start = std::min(w.fade_in_start, w.fade_in_end);
            changed = true;
        }
    }
    return changed && mode >= 1 && mode <= 2;
}

}   // namespace perf
