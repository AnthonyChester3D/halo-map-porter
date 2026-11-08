// Halo Map Porter - trimming a built Xbox map's tag data (maptrim.hpp).
#include "maptrim.hpp"
#include "inflate.hpp"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <unordered_set>
#include <vector>

namespace maptrim {
namespace {

struct MlField { int offset, kind, sub; };   // kind: 0 a reference (its path pointer at +4), 1 a block (count +0,
                                             // pointer +4, element struct sub), 2 data (size +0, pointer +12), 3 a pointer
struct MlStruct { unsigned size, first, fields; };
struct MlGroup { unsigned fourcc; int root; };
#include "map_layouts.inc"

const uint32_t kBase = 0x803A6000u;
const size_t kHeader = 0x800;
const uint32_t kFullTable = 19001, kNodeSize = 20, kTableHeader = 56;

uint32_t get32(const std::string &b, size_t o) {
    if (o + 4 > b.size()) return 0;
    const unsigned char *p = (const unsigned char *)b.data() + o;
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}
uint16_t get16(const std::string &b, size_t o) {
    if (o + 2 > b.size()) return 0;
    const unsigned char *p = (const unsigned char *)b.data() + o;
    return (uint16_t)(p[0] | p[1] << 8);
}
void put32(std::string &b, size_t o, uint32_t v) {
    for (int k = 0; k < 4; k++) b[o + (size_t)k] = (char)(v >> (8 * k));
}
void put16(std::string &b, size_t o, uint16_t v) {
    b[o] = (char)v;
    b[o + 1] = (char)(v >> 8);
}

// Memory the map is loaded into: the tag data, or one structure BSP (each is loaded at its own time, so their
// addresses overlap: one is walked at a time).
struct Space {
    uint32_t addr = 0, size = 0;
    size_t file = 0;
    bool has(uint32_t v) const { return v >= addr && v - addr < size; }
};
struct Slot { size_t pos; uint32_t value; bool bsp; };   // a pointer: where it is in the file, what it holds, whose it is

// Every pointer in the tag data and the BSPs.
struct Walk {
    const std::string &d;
    Space tags, bsp;
    bool in_bsp = false;
    std::vector<Slot> slots;
    std::vector<Space> bsps;
    std::unordered_set<uint64_t> seen;
    std::string problem;

    explicit Walk(const std::string &data) : d(data) {
        tags.addr = kBase;
        tags.file = get32(d, 0x10);
        tags.size = get32(d, 0x14);
        if (tags.file < kHeader || tags.file + tags.size > d.size()) { problem = "the tag data isn't in the file"; return; }
        walk();
    }
    bool has(uint32_t v) const { return tags.has(v) || (in_bsp && bsp.has(v)); }
    size_t pos(uint32_t v) const { return tags.has(v) ? tags.file + (v - tags.addr) : bsp.file + (v - bsp.addr); }
    uint32_t slot(size_t p) {
        const uint32_t v = get32(d, p);
        if (has(v)) slots.push_back({p, v, in_bsp});
        return v;
    }
    // A table of `n` 12-byte entries, each with a pointer at +4 (the Xbox's vertex and index buffers).
    void table(uint32_t a, uint32_t n) {
        if (!n || !has(a)) return;
        if (!has(a + n * 12 - 1)) { problem = "a vertex table runs past its memory"; return; }
        for (uint32_t i = 0; i < n; i++) slot(pos(a) + i * 12 + 4);
    }
    void walk_struct(uint32_t addr, int s) {
        if (!problem.empty()) return;
        const MlStruct &st = kMlStructs[s];
        if (!has(addr) || !has(addr + st.size - 1)) {
            char t[96];
            snprintf(t, sizeof t, "a struct (#%d) at 0x%08X isn't in its memory%s", s, addr, in_bsp ? " (a BSP's)" : "");
            problem = t;
            return;
        }
        if (!seen.insert((uint64_t)addr << 12 | (uint64_t)s).second) return;
        const size_t p0 = pos(addr);
        for (unsigned i = 0; i < st.fields; i++) {
            const MlField &f = kMlFields[st.first + i];
            const size_t p = p0 + (size_t)f.offset;
            if (f.kind == 0) slot(p + 4);
            else if (f.kind == 1) {
                const uint32_t n = get32(d, p), v = get32(d, p + 4);
                if (!n) continue;
                const MlStruct &e = kMlStructs[f.sub];
                if (!has(v) || (uint64_t)n * e.size > (uint64_t)(tags.has(v) ? tags.addr + tags.size : bsp.addr + bsp.size) - v) {
                    problem = "a block isn't in its memory";
                    return;
                }
                slots.push_back({p + 4, v, in_bsp});
                for (uint32_t k = 0; k < n && problem.empty(); k++) walk_struct(v + k * e.size, f.sub);
            } else if (f.kind == 2) {
                if (get32(d, p)) slot(p + 12);
            } else slot(p);
        }
    }
    void walk() {
        const size_t to = tags.file;
        const uint32_t tag_array = get32(d, to), count = get32(d, to + 12), parts = get32(d, to + 16);
        for (size_t o : {0, 0x14, 0x1C}) slot(to + o);
        table(get32(d, to + 0x14), parts);   // (each model part's vertices, then indices)
        table(get32(d, to + 0x1C), parts);
        if (!has(tag_array) || !has(tag_array + count * 0x20 - 1)) { problem = "the tag index isn't in the tag data"; return; }
        const size_t ia = pos(tag_array);
        for (uint32_t k = 0; k < count && problem.empty(); k++) {
            const size_t e = ia + (size_t)k * 0x20;
            const uint32_t group = get32(d, e), data = get32(d, e + 0x14);
            slot(e + 0x10);
            slot(e + 0x14);
            if (!has(data)) continue;   // (a BSP's: it's walked below; or a tag that's elsewhere)
            const MlGroup *g = std::find_if(std::begin(kMlGroups), std::end(kMlGroups), [&](const MlGroup &x) { return x.fourcc == group; });
            if (g == std::end(kMlGroups)) { problem = "a tag of a group the app doesn't know"; return; }
            walk_struct(data, g->root);
        }
        if (!problem.empty()) return;
        const size_t sd = scenario();
        if (!sd) { problem = "the map has no scenario"; return; }
        const uint32_t n = get32(d, sd + kMlScenarioBsps), a = get32(d, sd + kMlScenarioBsps + 4);
        for (uint32_t i = 0; n && has(a) && i < n; i++) {
            const size_t e = pos(a) + (size_t)i * 32;
            Space s;
            s.file = get32(d, e);
            s.size = get32(d, e + 4);
            s.addr = get32(d, e + 8);
            if (!s.size) continue;
            if (s.file < kHeader || s.file + s.size > tags.file) { problem = "a BSP isn't before the tag data"; return; }
            if (std::none_of(bsps.begin(), bsps.end(), [&](const Space &b) { return b.file == s.file; })) bsps.push_back(s);
        }
        for (const Space &b : bsps) {
            bsp = b;
            in_bsp = true;
            seen.clear();
            walk_struct(b.addr, kMlBspHeader);
            table(get32(d, b.file + 8), get32(d, b.file + 4));     // (each lightmap material's rendered vertices,
            table(get32(d, b.file + 16), get32(d, b.file + 12));   // then its lightmap vertices)
            walk_struct(get32(d, b.file), kMlBsp);
            if (!problem.empty()) return;
        }
        in_bsp = false;
    }
    size_t scenario() const {   // where the scenario tag's struct is (0: nowhere)
        const uint32_t index = get32(d, tags.file), k = get32(d, tags.file + 4) & 0xFFFF;
        if (!tags.has(index) || k >= get32(d, tags.file + 12)) return 0;
        const uint32_t a = get32(d, tags.file + (index - kBase) + (size_t)k * 0x20 + 0x14);
        return tags.has(a) && tags.has(a + kMlScenarioBsps + 11) ? tags.file + (a - kBase) : 0;
    }
};

bool read_file(const fs::path &p, std::string &s) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    s.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}

}   // namespace

Result trim_scripts(const fs::path &map, uint64_t tag_space, uint32_t spare) {
    Result r;
    std::string raw, d;
    if (!read_file(map, raw) || raw.size() < kHeader || raw.compare(0, 4, "daeh") != 0 || raw.compare(0x7FC, 4, "toof") != 0) {
        r.error = "couldn't read the map";
        return r;
    }
    const uint32_t length = get32(raw, 8);
    const bool packed = raw.size() < length;
    if (packed) {
        d = raw.substr(0, kHeader);
        std::string body;
        if (!inflate::zlib((const uint8_t *)raw.data() + kHeader, raw.size() - kHeader, body, length - kHeader) ||
            body.size() != length - kHeader) {
            r.error = "couldn't unpack the map";
            return r;
        }
        d += body;
    } else d = std::move(raw);
    raw.clear();
    raw.shrink_to_fit();

    Walk w(d);
    if (!w.problem.empty()) { r.error = w.problem; return r; }
    const Space tags = w.tags;
    // The BSPs' new addresses: at the top of the Xbox's tag space instead of the one Invader was told.
    const uint32_t lift = tag_space > kXboxTagSpace ? (uint32_t)(tag_space - kXboxTagSpace) : 0;
    for (const Space &b : w.bsps) {
        if (lift && b.addr != kBase + tag_space - b.size) { r.error = "a BSP isn't at the top of the tag space"; return r; }
        r.largest_bsp = std::max(r.largest_bsp, b.size);
    }

    // The table: a data array (its name, then maximum +32, element size +34, ..., in use +44 to +48), then the nodes.
    const size_t sd = w.scenario();
    const uint32_t size = get32(d, sd + kMlScenarioSyntax), table = get32(d, sd + kMlScenarioSyntax + 12);
    const size_t h = tags.has(table) ? w.pos(table) : 0;
    if (!h || size < kTableHeader || !tags.has(table + size - 1) || d.compare(h, 11, "script node") != 0) {
        r.error = "the scenario has no script node table";
        return r;
    }
    const uint32_t max = get16(d, h + 32);
    r.nodes = std::max({get16(d, h + 44), get16(d, h + 46), get16(d, h + 48)});
    r.kept = std::min(kFullTable, r.nodes + spare);
    while ((kFullTable - r.kept) % 8) r.kept++;   // (moves what's after it by a multiple of 32 bytes: it stays aligned)
    uint32_t cut = 0;
    if (max == kFullTable && get16(d, h + 34) == kNodeSize && size == kTableHeader + kNodeSize * kFullTable)
        cut = kNodeSize * (kFullTable - r.kept);
    const uint32_t cut1 = table + size, cut0 = cut1 - cut;   // (the nodes taken out: [cut0, cut1))
    for (size_t i = w.pos(cut0); cut && i < w.pos(cut0) + cut; i++)
        if (d[i]) { r.error = "the script node table's unused nodes aren't empty"; return r; }

    if (!cut && !lift) {
        r.ok = true;
        r.tag_data = tags.size;
        return r;
    }
    // What's checked afterwards: the tag data and the BSPs as they were.
    std::string before = d.substr(tags.file, tags.size);
    std::vector<std::string> bsp_before;
    for (const Space &b : w.bsps) bsp_before.push_back(lift ? d.substr(b.file, b.size) : std::string());

    auto moved = [&](const Slot &s) -> uint32_t {
        if (tags.has(s.value)) {
            if (s.value >= cut0 && s.value < cut1) return 0;   // (into what's taken out: not expected)
            return s.value >= cut1 ? s.value - cut : s.value;
        }
        return s.value - lift;   // (in its BSP)
    };
    // (the scenario's BSP block, read before its pointer moves)
    const uint32_t bsp_entries = get32(d, sd + kMlScenarioBsps), bsp_block = get32(d, sd + kMlScenarioBsps + 4);
    std::vector<Slot> want;   // every pointer as it should be once the tag data is cut: where it is then, what it holds
    for (const Slot &s : w.slots) {
        const uint32_t v = moved(s);
        if (!v) { r.error = "a pointer into the unused script nodes"; return r; }
        put32(d, s.pos, v);
        const bool after = s.pos >= tags.file && s.pos >= w.pos(cut1);
        want.push_back({after ? s.pos - cut : s.pos, v, s.bsp});
    }
    // The scenario's BSPs' addresses (in its block, entries a BSP is used by)
    if (lift) {
        for (uint32_t i = 0; i < bsp_entries; i++) {
            const size_t e = w.pos(bsp_block) + (size_t)i * 32;
            if (get32(d, e + 4)) put32(d, e + 8, get32(d, e + 8) - lift);
        }
    }
    r.tag_data = tags.size - cut;
    if (lift && r.tag_data + (uint64_t)r.largest_bsp > kXboxTagSpace) {
        r.error = "it doesn't fit the Xbox's tag memory even trimmed";
        return r;
    }
    put32(d, sd + kMlScenarioSyntax, size - cut);
    put16(d, h + 32, (uint16_t)r.kept);
    d.erase(w.pos(cut0), cut);
    put32(d, 0x14, tags.size - cut);
    const size_t end = tags.file + tags.size - cut;
    d.resize(end + (2048 - end % 2048) % 2048, '\0');   // (the file is in whole sectors)
    put32(d, 8, (uint32_t)d.size());
    r.saved = cut;

    // Check: walked again, every pointer is where it should be and holds what it should, and every other byte is as it
    // was (but the table's size and maximum, and the BSPs' addresses in the scenario).
    Walk again(d);
    if (!again.problem.empty()) { r.error = "the trimmed map: " + again.problem; return r; }
    auto key = [](const Slot &s) { return std::make_pair(s.pos, s.value); };
    auto sorted = [&](std::vector<Slot> v) {
        std::vector<std::pair<size_t, uint32_t>> k;
        for (auto &s : v) k.push_back(key(s));
        std::sort(k.begin(), k.end());
        k.erase(std::unique(k.begin(), k.end()), k.end());
        return k;
    };
    if (sorted(want) != sorted(again.slots)) { r.error = "the trimmed map's pointers aren't all where they should be"; return r; }
    std::string expect = before;
    expect.erase(cut0 - kBase, cut);
    std::string got = d.substr(tags.file, r.tag_data);
    for (const Slot &s : want)
        if (s.pos >= tags.file) put32(expect, s.pos - tags.file, get32(got, s.pos - tags.file));
    size_t differ = 0;
    for (size_t i = 0; i < expect.size(); i++) differ += expect[i] != got[i];
    const size_t allowed = 6 + (lift ? 4 * (size_t)get32(d, sd + kMlScenarioBsps) : 0);
    if (expect.size() != got.size() || differ > allowed) { r.error = "the trimmed map's tag data isn't the same"; return r; }
    for (size_t i = 0; i < w.bsps.size() && lift; i++) {
        const Space &b = w.bsps[i];
        std::string x = bsp_before[i];
        for (const Slot &s : want)
            if (s.pos >= b.file && s.pos < b.file + b.size) put32(x, s.pos - b.file, get32(d, s.pos));
        if (d.compare(b.file, b.size, x) != 0) { r.error = "a trimmed map's BSP isn't the same"; return r; }
    }

    std::string file = packed ? d.substr(0, kHeader) + inflate::zlib_compress((const uint8_t *)d.data() + kHeader, d.size() - kHeader)
                              : std::move(d);
    const fs::path tmp = map.string() + ".trim";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        o.write(file.data(), (std::streamsize)file.size());
        if (!o) { r.error = "couldn't write the map"; return r; }
    }
    std::error_code ec;
    fs::rename(tmp, map, ec);
    if (ec) { fs::remove(tmp, ec); r.error = "couldn't replace the map"; return r; }
    r.ok = true;
    return r;
}
}   // namespace maptrim
