// Halo Map Porter - shared files (sharedfile.hpp; Halo 1 Unlimited's docs/shared_maps.md, format version 2).
#include "sharedfile.hpp"
#include "inflate.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <thread>
#include <unordered_map>

namespace sharedfile {
namespace {
const size_t kHeader = 0x800, kUnit = 512, kSector = 2048;
const uint32_t kTagBase = 0x803A6000u, kRef = 0x700, kRefSize = 0x60;
const uint32_t kPack = 0x80000000u, kStock = 0xC0000000u, kOffset = 0x3FFFFFFFu, kMaxShared = 0x11600000u;
const uint32_t kBitmap = 0x6269746Du, kSound = 0x736E6421u;
const size_t npos = (size_t)-1;

uint32_t crc32_of(const char *d, size_t n) {   // (zlib's CRC32)
    static const std::vector<uint32_t> table = [] {
        std::vector<uint32_t> t(256);
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ (unsigned char)d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
uint32_t get32(const std::string &b, size_t o) {
    if (o + 4 > b.size()) return 0;
    const unsigned char *p = (const unsigned char *)b.data() + o;
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}
void put32(std::string &b, size_t o, uint32_t v) {
    for (int k = 0; k < 4; k++) b[o + (size_t)k] = (char)(v >> (8 * k));
}
std::string mb(uint64_t n) {
    char t[32];
    snprintf(t, sizeof t, "%.1f MB", n / 1048576.0);
    return t;
}
std::string name_of(const fs::path &p) { return p.filename().u8string(); }
std::string shown(const fs::path &map) { return map.parent_path().filename().u8string() + "/" + name_of(map); }
std::string cstr(const std::string &b, size_t o, size_t n) {
    std::string s = b.substr(o, n);
    return s.substr(0, s.find('\0'));
}

bool read_file(const fs::path &p, std::string &out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamoff n = f.tellg();
    if (n < 0) return false;
    f.seekg(0);
    out.resize((size_t)n);
    return n == 0 || (bool)f.read(&out[0], n);
}
bool write_file(const fs::path &p, const std::string &d) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    return f && f.write(d.data(), (std::streamsize)d.size()) && f.flush();
}

// A cache file (map or shared file) unpacked, header included.
bool unpack(const std::string &disk, std::string &data, bool &compressed, std::string &why) {
    if (disk.size() < kHeader || disk.compare(0, 4, "daeh") != 0 || get32(disk, 4) != 5 || disk.compare(0x7FC, 4, "toof") != 0) {
        why = "not an Xbox map";
        return false;
    }
    const uint32_t length = get32(disk, 8);
    compressed = disk.size() < length || (disk.size() != length && (unsigned char)disk[kHeader] == 0x78);
    if (!compressed) {
        data = disk;
        return true;
    }
    if (length <= kHeader) { why = "its header's length is wrong"; return false; }
    std::string rest;
    if (!inflate::zlib((const uint8_t *)disk.data() + kHeader, disk.size() - kHeader, rest, length - kHeader) ||
        rest.size() != length - kHeader) {
        why = "it doesn't unpack (damaged?)";
        return false;
    }
    data.reserve(length);
    data.assign(disk, 0, kHeader);
    data += rest;
    return true;
}

// Packed as a map is (one zlib stream after the header), padded to whole 4096 bytes as Invader pads them (the padding
// at 0xC), the header's checksum a CRC32 of the rest as Halo Map Porter stamps it (stamp_xbox_checksum).
std::string pack_map(std::string &data, bool compressed) {
    std::string file;
    if (compressed) {
        file = data.substr(0, kHeader) + inflate::zlib_compress((const uint8_t *)data.data() + kHeader, data.size() - kHeader);
        const uint32_t pad = (uint32_t)((4096 - file.size() % 4096) % 4096);
        file.append(pad, '\0');
        put32(file, 0xC, pad);
    } else {
        file = data;
        put32(file, 0xC, 0);
    }
    uint32_t crc = crc32_of(file.data() + kHeader, file.size() - kHeader);
    if (crc == 0xFFFFFFFFu) crc = 0xFFFFFFFEu;
    put32(file, 0x64, crc);
    data.replace(0, kHeader, file, 0, kHeader);   // (the same header, unpacked)
    return file;
}
// A shared file packed: whole 2048-byte sectors (docs/shared_maps.md).
std::string pack_shared(const std::string &data) {
    std::string file = data.substr(0, kHeader) + inflate::zlib_compress((const uint8_t *)data.data() + kHeader, data.size() - kHeader);
    file.append((kSector - file.size() % kSector) % kSector, '\0');
    return file;
}

uint64_t hash_bytes(const char *p, size_t n) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ (uint64_t)n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    uint64_t w = 0;
    memcpy(&w, p + i, n - i);
    h = (h ^ w) * 0xC4CEB9FE1A85EC53ull;
    return h ^ (h >> 29);
}

// A texture's or sound's data: where its offset is in the map, and its bytes (in the map, or in the shared file it
// was in). `bytes` null: left as it is (empty, or not data in the file).
struct Block {
    size_t field = 0;
    uint32_t value = 0, size = 0, rem = 0;
    const char *bytes = nullptr;
    bool own = false;          // in the map itself
    int entry = -1;
};
struct Parsed {
    uint32_t td_off = 0, td_size = 0;
    std::vector<Block> blocks;
    std::vector<size_t> bsp_fields;   // each BSP's file offset (and its size after it)
};
using Resolve = std::function<const char *(uint32_t value, uint32_t size)>;

// The map's textures and sounds (bitmaps' bitmap_datas, sounds' pitch ranges' permutations, in tag index order) and
// BSPs. `resolve`: a block in a shared file's bytes (null: not found).
bool parse(const std::string &d, Parsed &p, const Resolve &resolve, std::string &why) {
    p = Parsed();
    p.td_off = get32(d, 0x10);
    p.td_size = get32(d, 0x14);
    const uint64_t td_end = (uint64_t)p.td_off + p.td_size;
    if (p.td_size < 16 || p.td_off < kHeader || td_end > d.size()) { why = "its tag data isn't in the file"; return false; }
    auto at = [&](uint32_t addr, uint64_t need) -> size_t {
        if (addr < kTagBase || (uint64_t)(addr - kTagBase) + need > p.td_size) return npos;
        return p.td_off + (size_t)(addr - kTagBase);
    };
    const uint32_t count = get32(d, p.td_off + 12), scenario = get32(d, p.td_off + 4) & 0xFFFF;
    const size_t index = count <= 65535 ? at(get32(d, p.td_off), (uint64_t)count * 32) : npos;
    if (index == npos || scenario >= count) { why = "its tag index isn't readable"; return false; }
    auto add = [&](size_t field, uint32_t value, uint32_t size) -> bool {
        Block b;
        b.field = field;
        b.value = value;
        b.size = size;
        if (size && (value & kPack)) {
            b.bytes = resolve(value, size);
            if (!b.bytes) { why = "a texture or sound in it is past its shared file's end"; return false; }
            b.rem = (value & kOffset) % kUnit;
        } else if (size && value >= kHeader && (uint64_t)value + size <= d.size() &&
                   !((uint64_t)value + size > p.td_off && value < td_end)) {
            b.bytes = d.data() + value;
            b.own = true;
            b.rem = value % kUnit;
        }
        p.blocks.push_back(b);
        return true;
    };
    for (uint32_t k = 0; k < count; k++) {
        const size_t e = index + (size_t)k * 32;
        const uint32_t group = get32(d, e), base = get32(d, e + 0x14);
        if (!base || (group != kBitmap && group != kSound)) continue;
        if (group == kBitmap) {
            const size_t b = at(base, 0x68);
            if (b == npos) { why = "a bitmap tag isn't readable"; return false; }
            const uint32_t n = get32(d, b + 0x60);
            const size_t list = n ? at(get32(d, b + 0x64), (uint64_t)n * 0x30) : 0;
            if (list == npos) { why = "a bitmap tag isn't readable"; return false; }
            for (uint32_t m = 0; m < n; m++) {
                const size_t bd = list + (size_t)m * 0x30;
                if (!add(bd + 0x18, get32(d, bd + 0x18), get32(d, bd + 0x1C))) return false;
            }
        } else {
            const size_t s = at(base, 0xA0);
            if (s == npos) { why = "a sound tag isn't readable"; return false; }
            const uint32_t n = get32(d, s + 0x98);
            const size_t ranges = n ? at(get32(d, s + 0x9C), (uint64_t)n * 0x48) : 0;
            if (ranges == npos) { why = "a sound tag isn't readable"; return false; }
            for (uint32_t r = 0; r < n; r++) {
                const size_t pr = ranges + (size_t)r * 0x48;
                const uint32_t q = get32(d, pr + 0x3C);
                const size_t perms = q ? at(get32(d, pr + 0x40), (uint64_t)q * 0x7C) : 0;
                if (perms == npos) { why = "a sound tag isn't readable"; return false; }
                for (uint32_t j = 0; j < q; j++) {
                    const size_t pm = perms + (size_t)j * 0x7C;
                    if (!add(pm + 0x48, get32(d, pm + 0x48), get32(d, pm + 0x40))) return false;
                }
            }
        }
    }
    const size_t s = at(get32(d, index + (size_t)scenario * 32 + 0x14), 0x5AC);
    if (s == npos) { why = "its scenario isn't readable"; return false; }
    const uint32_t n = get32(d, s + 0x5A4);
    const size_t list = n ? at(get32(d, s + 0x5A8), (uint64_t)n * 0x20) : 0;
    if (list == npos || n > 64) { why = "its BSP list isn't readable"; return false; }
    for (uint32_t k = 0; k < n; k++) p.bsp_fields.push_back(list + (size_t)k * 0x20);
    return true;
}

// The blocks found so far, each the same bytes at the same remainder modulo 512 once.
struct Entry {
    const char *bytes;
    uint32_t size, rem;
    int maps = 0, last_map = -1;
    bool in_pack = false;
    uint32_t at = 0;           // its offset in the pack's file (in_pack)
};
struct Table {
    std::vector<Entry> e;
    std::unordered_map<uint64_t, std::vector<int>> by;
    static uint64_t key(const char *b, uint32_t n, uint32_t rem) { return hash_bytes(b, n) ^ ((uint64_t)rem << 52); }
    int find(const char *b, uint32_t n, uint32_t rem) const {
        auto it = by.find(key(b, n, rem));
        if (it == by.end()) return -1;
        for (int i : it->second)
            if (e[(size_t)i].size == n && e[(size_t)i].rem == rem && memcmp(e[(size_t)i].bytes, b, n) == 0) return i;
        return -1;
    }
    int add(const char *b, uint32_t n, uint32_t rem) {
        int i = find(b, n, rem);
        if (i >= 0) return i;
        e.push_back({b, n, rem});
        by[key(b, n, rem)].push_back((int)e.size() - 1);
        return (int)e.size() - 1;
    }
};

// A shared file's header (docs/shared_maps.md), its data laid out after it.
void shared_header(std::string &data, const std::string &name) {
    std::string h(kHeader, '\0');
    h.replace(0, 4, "daeh");
    put32(h, 4, 5);
    put32(h, 8, (uint32_t)data.size());
    h.replace(0x20, name.size(), name);
    h.replace(0x40, 6, "shared");
    put32(h, 0x64, crc32_of(data.data() + kHeader, data.size() - kHeader));
    h.replace(0x7FC, 4, "toof");
    data.replace(0, kHeader, h);
}
// Each block placed after the last, padded with zeros up to its remainder modulo 512.
uint32_t place(std::string &data, const char *bytes, uint32_t size, uint32_t rem) {
    data.append((rem + kUnit - data.size() % kUnit) % kUnit, '\0');
    const uint32_t at = (uint32_t)data.size();
    data.append(bytes, size);
    return at;
}

struct Source {   // a shared file a map drew on
    std::string data;
    uint32_t sum = 0, length = 0;
    uint64_t disk = 0;   // (its size on disk)
};
struct Map {
    fs::path path;
    uint64_t disk_size = 0;
    bool compressed = true, had_ref = false;
    uint32_t ident_size = 0, ident_sum = 0;
    std::string data;   // unpacked
    Parsed p;
    // what becomes of it
    std::string out, file;
    uint64_t to_pack = 0, to_stock = 0;
    bool rewrite = false, pack_ref = false, stock_ref = false;
    std::string error;
};

// Where each block goes: 0 stays, 1 the pack's file, 2 stock.shared.
struct Plan {
    std::vector<int> go;
    std::vector<uint32_t> at;   // its offset in that shared file
};

// The map laid out again: what moved left out in whole 512-byte units, each kept unit moved down; blocks that stay but
// weren't in the map (they were in a shared file) placed after the rest. Returns false (`m.error`) if it can't be.
bool relayout(Map &m, const Plan &plan, const std::string &pack_name, const Source &pack, const Source &stock) {
    const std::string &d = m.data;
    const Parsed &p = m.p;
    const size_t len = d.size(), units = (len - kHeader + kUnit - 1) / kUnit;
    std::vector<uint8_t> drop(units, 0);
    std::vector<std::pair<uint64_t, uint64_t>> moved;
    for (size_t j = 0; j < p.blocks.size(); j++)
        if (p.blocks[j].own && plan.go[j]) moved.push_back({p.blocks[j].value, (uint64_t)p.blocks[j].value + p.blocks[j].size});
    std::sort(moved.begin(), moved.end());
    std::vector<std::pair<uint64_t, uint64_t>> merged;
    for (auto &r : moved) {
        if (!merged.empty() && r.first <= merged.back().second) merged.back().second = std::max(merged.back().second, r.second);
        else merged.push_back(r);
    }
    for (auto &r : merged)
        for (uint64_t u = (r.first - kHeader + kUnit - 1) / kUnit; u < (r.second - kHeader) / kUnit && u < units; u++) drop[u] = 1;
    // what stays keeps its units: the tag data, the BSPs, the blocks that stay
    std::vector<std::pair<uint64_t, uint64_t>> kept;
    kept.push_back({p.td_off, (uint64_t)p.td_off + p.td_size});
    for (size_t f : p.bsp_fields) kept.push_back({get32(d, f), (uint64_t)get32(d, f) + get32(d, f + 4)});
    for (size_t j = 0; j < p.blocks.size(); j++)
        if (p.blocks[j].own && !plan.go[j]) kept.push_back({p.blocks[j].value, (uint64_t)p.blocks[j].value + p.blocks[j].size});
    for (auto &r : kept) {
        uint64_t a = std::max<uint64_t>(r.first, kHeader), b = std::min<uint64_t>(r.second, len);
        if (b > a)
            for (uint64_t u = (a - kHeader) / kUnit; u <= (b - 1 - kHeader) / kUnit; u++) drop[u] = 0;
    }
    std::vector<uint32_t> before(units + 1, 0);
    for (size_t u = 0; u < units; u++) before[u + 1] = before[u] + drop[u];
    bool bad = false;
    auto move = [&](uint64_t o) -> uint32_t {
        if (o < kHeader) return (uint32_t)o;
        const uint64_t u = (o - kHeader) / kUnit;
        if (u >= units) return (uint32_t)(o - (uint64_t)before[units] * kUnit);
        if (drop[u]) bad = true;
        return (uint32_t)(o - (uint64_t)before[u] * kUnit);
    };
    // the bytes: what moved zeroed (it compresses away), what stays put back over it where they share bytes
    std::string w = d;
    for (auto &r : merged) memset(&w[r.first], 0, r.second - r.first);
    for (auto &r : kept) {
        uint64_t a = std::max<uint64_t>(r.first, kHeader), b = std::min<uint64_t>(r.second, len);
        if (b > a) memcpy(&w[a], d.data() + a, b - a);
    }
    std::string &out = m.out;
    out.clear();
    out.reserve(len - (size_t)before[units] * kUnit + 1024);
    out.append(w, 0, kHeader);
    for (size_t u = 0; u < units; u++)
        if (!drop[u]) out.append(w, kHeader + u * kUnit, std::min(kUnit, len - kHeader - u * kUnit));
    std::string().swap(w);
    // blocks that stay in it but weren't in it, after the rest (each once)
    std::map<int, uint32_t> inserted;
    std::vector<uint32_t> value(p.blocks.size());
    for (size_t j = 0; j < p.blocks.size(); j++) {
        const Block &b = p.blocks[j];
        if (!b.bytes) value[j] = b.value;
        else if (plan.go[j] == 1) value[j] = kPack | plan.at[j];
        else if (plan.go[j] == 2) value[j] = kStock | plan.at[j];
        else if (b.own) value[j] = move(b.value);
        else {
            auto it = inserted.find(b.entry);
            if (it == inserted.end()) it = inserted.insert({b.entry, place(out, b.bytes, b.size, b.rem)}).first;
            value[j] = it->second;
        }
    }
    out.append((kUnit - out.size() % kUnit) % kUnit, '\0');
    for (size_t j = 0; j < p.blocks.size(); j++) put32(out, move(p.blocks[j].field), value[j]);
    for (size_t f : p.bsp_fields) put32(out, move(f), move(get32(d, f)));
    put32(out, 0x10, move(p.td_off));
    if (bad) { m.error = "something that stays in it shares a 512-byte sector with what moved"; return false; }
    put32(out, 8, (uint32_t)out.size());
    std::string ref(kRefSize, '\0');
    if (m.pack_ref || m.stock_ref) {
        ref.replace(0, 4, "drhs");
        put32(ref, 4, 2);
        if (m.pack_ref) {
            ref.replace(0x08, pack_name.size(), pack_name);
            put32(ref, 0x28, pack.sum);
            put32(ref, 0x2C, pack.length);
        }
        put32(ref, 0x30, m.ident_size);
        put32(ref, 0x34, m.ident_sum);
        if (m.stock_ref) {
            ref.replace(0x38, 5, "stock");
            put32(ref, 0x58, stock.sum);
            put32(ref, 0x5C, stock.length);
        }
    }
    out.replace(kRef, kRefSize, ref);
    return true;
}

// The new map read back: unpacked again, every texture and sound through the new files the same bytes as before, the
// BSPs and the tag data the same (but for the offsets that changed), the header the same but for what changed.
bool check(Map &m, const Source &pack, const Source &stock) {
    if (m.compressed) {
        std::string data;
        bool c = false;
        std::string why;
        if (!unpack(m.file, data, c, why) || data != m.out) { m.error = "it didn't pack right (unpacked, it isn't what was packed)"; return false; }
    }
    auto from = [&](const Source &s, uint32_t value, uint32_t size) -> const char * {
        const uint64_t o = value & kOffset;
        return o + size <= s.data.size() ? s.data.data() + o : nullptr;
    };
    Parsed q;
    std::string why;
    if (!parse(m.out, q, [&](uint32_t v, uint32_t n) { return (v & kStock) == kStock ? from(stock, v, n) : from(pack, v, n); }, why)) {
        m.error = "read back, " + why;
        return false;
    }
    const Parsed &p = m.p;
    if (q.blocks.size() != p.blocks.size() || q.bsp_fields.size() != p.bsp_fields.size() || q.td_size != p.td_size) {
        m.error = "read back, its tags aren't the same";
        return false;
    }
    for (size_t j = 0; j < p.blocks.size(); j++) {
        const Block &a = p.blocks[j], &b = q.blocks[j];
        if (a.size != b.size || (!a.bytes && a.value != b.value) || (a.bytes && (!b.bytes || memcmp(a.bytes, b.bytes, a.size) != 0))) {
            m.error = "read back, a texture or sound isn't the same";
            return false;
        }
    }
    const std::string &d = m.data, &o = m.out;
    for (size_t k = 0; k < p.bsp_fields.size(); k++) {
        const uint32_t a = get32(d, p.bsp_fields[k]), b = get32(o, q.bsp_fields[k]), n = get32(d, p.bsp_fields[k] + 4);
        if (n != get32(o, q.bsp_fields[k] + 4) || (uint64_t)a + n > d.size() || (uint64_t)b + n > o.size() || memcmp(d.data() + a, o.data() + b, n) != 0) {
            m.error = "read back, a BSP isn't the same";
            return false;
        }
    }
    std::string td = o.substr(q.td_off, q.td_size);
    for (size_t j = 0; j < p.blocks.size(); j++) put32(td, p.blocks[j].field - p.td_off, p.blocks[j].value);
    for (size_t k = 0; k < p.bsp_fields.size(); k++) put32(td, p.bsp_fields[k] - p.td_off, get32(d, p.bsp_fields[k]));
    if (td.compare(0, td.size(), d, p.td_off, p.td_size) != 0) { m.error = "read back, its tag data isn't the same"; return false; }
    std::string h0 = d.substr(0, kHeader), h1 = o.substr(0, kHeader);
    for (size_t f : {8, 0xC, 0x10, 0x64}) { put32(h0, f, 0); put32(h1, f, 0); }
    h0.replace(kRef, kRefSize, kRefSize, '\0');
    h1.replace(kRef, kRefSize, kRefSize, '\0');
    if (h0 != h1) { m.error = "read back, its header isn't the same"; return false; }
    return true;
}

// stock.shared as Halo 1 Unlimited lays it out (docs/shared_maps.md, "stock.shared's layout"): the stock multiplayer
// maps in the order of their file names (lower case), each map's blocks in tag index order, each block's first copy
// placed after the last.
const char *const kStockMaps[] = {"beavercreek", "bloodgulch", "boardingaction", "carousel", "chillout", "damnation", "hangemhigh",
                                  "longest", "prisoner", "putput", "ratrace", "sidewinder", "wizard"};
struct StockLayout {
    Source file;
    Table t;
    std::vector<uint32_t> at;   // each entry's offset
};
bool lay_out_stock(const std::vector<fs::path> &paths, StockLayout &s, std::string &why) {
    std::vector<std::string> datas(paths.size());
    std::vector<std::string> errors(paths.size());
    std::vector<std::thread> th;
    for (size_t i = 0; i < paths.size(); i++)
        th.emplace_back([&, i] {
            std::string disk;
            bool c;
            if (!read_file(paths[i], disk)) errors[i] = "couldn't be read";
            else if (!unpack(disk, datas[i], c, errors[i])) datas[i].clear();
        });
    for (auto &t : th) t.join();
    for (size_t i = 0; i < paths.size(); i++)
        if (!errors[i].empty()) { why = name_of(paths[i]) + ": " + errors[i]; return false; }
    std::string &data = s.file.data;
    data.assign(kHeader, '\0');
    for (size_t i = 0; i < paths.size(); i++) {
        Parsed p;
        if (!parse(datas[i], p, [](uint32_t, uint32_t) { return (const char *)nullptr; }, why)) { why = name_of(paths[i]) + ": " + why; return false; }
        for (auto &b : p.blocks) {
            if (!b.own) continue;
            const size_t before = s.t.e.size();
            const int e = s.t.add(b.bytes, b.size, b.rem);
            if (s.t.e.size() > before) s.at.push_back(place(data, b.bytes, b.size, b.rem));
            (void)e;
        }
    }
    data.append((kSector - data.size() % kSector) % kSector, '\0');
    if (data.size() > kMaxShared) { why = "it would be over 278 MB"; return false; }
    shared_header(data, "stock");
    s.file.sum = get32(data, 0x64);
    s.file.length = (uint32_t)data.size();
    // (the entries' bytes, from the maps, to the file's own copy)
    for (size_t k = 0; k < s.t.e.size(); k++) s.t.e[k].bytes = data.data() + s.at[k];
    return true;
}
}   // namespace

std::vector<fs::path> stock_multiplayer_maps(const fs::path &folder) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (folder.empty() || !fs::is_directory(folder, ec)) return out;
    std::map<std::string, fs::path> found;
    for (auto &e : fs::directory_iterator(folder, ec)) {
        std::string n = e.path().filename().u8string();
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        found[n] = e.path();
    }
    for (const char *n : kStockMaps) {
        auto it = found.find(std::string(n) + ".map");
        if (it == found.end()) return {};
        out.push_back(it->second);
    }
    return out;
}

Result make(const fs::path &shared, const std::vector<fs::path> &paths, const fs::path &stock_maps,
            const std::function<void(const std::string &)> &log) {
    Result r;
    try {
        const std::string name = shared.stem().u8string();
        bool name_ok = !name.empty() && name.size() <= 30 && name != "stock";
        for (unsigned char c : name)
            if (c < 32 || c > 126 || strchr("\\/:*?\"<>|", c)) name_ok = false;
        if (!name_ok) {
            r.error = "The shared file's name (" + name + ") must be 1 to 30 letters, digits and the like (none of \\ / : * ? \" < > |), and not \"stock\".";
            return r;
        }
        // ---- the maps, and the shared files they already draw on
        log("Reading the maps...");
        std::vector<std::unique_ptr<Map>> maps;
        for (auto &p : paths) {
            maps.push_back(std::make_unique<Map>());
            maps.back()->path = p;
        }
        {
            std::vector<std::thread> th;
            for (auto &mp : maps)
                th.emplace_back([&m = *mp] {
                    std::string disk;
                    if (!read_file(m.path, disk)) { m.error = "couldn't be read"; return; }
                    m.disk_size = disk.size();
                    m.ident_size = (uint32_t)disk.size();
                    m.ident_sum = get32(disk, 0x64);
                    if (!unpack(disk, m.data, m.compressed, m.error)) m.data.clear();
                });
            for (auto &t : th) t.join();
        }
        for (auto &m : maps)
            if (!m->error.empty()) { r.error = shown(m->path) + ": " + m->error; return r; }
        std::map<fs::path, std::unique_ptr<Source>> sources;
        auto source = [&](const Map &m, const std::string &n, uint32_t sum, uint32_t length, const Source *&out) -> bool {
            std::vector<fs::path> where{m.path.parent_path() / (n + ".shared"), m.path.parent_path().parent_path() / (n + ".shared")};
            if (n == "stock" && !stock_maps.empty()) where.push_back(stock_maps / "stock.shared");
            for (auto &w : where) {
                std::error_code ec;
                if (!fs::exists(w, ec)) continue;
                auto &s = sources[w];
                if (!s) {
                    s = std::make_unique<Source>();
                    std::string disk, why;
                    bool c;
                    if (!read_file(w, disk) || !unpack(disk, s->data, c, why)) s->data.clear();
                    s->disk = disk.size();
                    s->sum = get32(s->data, 0x64);
                    s->length = get32(s->data, 8);
                }
                if (s->sum == sum && s->length == length && s->data.size() == length) { out = s.get(); return true; }
            }
            r.error = shown(m.path) + " draws on " + n + ".shared, and the one it was built with isn't beside it or in the folder above "
                      "(convert it again, or put that file back).";
            return false;
        };
        for (auto &mp : maps) {
            Map &m = *mp;
            const Source *pack_src = nullptr, *stock_src = nullptr;
            if (m.data.compare(kRef, 4, "drhs") == 0) {
                const uint32_t version = get32(m.data, kRef + 4);
                if (version != 1 && version != 2) { r.error = shown(m.path) + " draws on shared files of a later format."; return r; }
                m.had_ref = true;
                m.ident_size = get32(m.data, kRef + 0x30);
                m.ident_sum = get32(m.data, kRef + 0x34);
                const std::string pn = cstr(m.data, kRef + 8, 32), sn = version == 2 ? cstr(m.data, kRef + 0x38, 32) : "";
                if (!pn.empty() && !source(m, pn, get32(m.data, kRef + 0x28), get32(m.data, kRef + 0x2C), pack_src)) return r;
                if (!sn.empty() && !source(m, sn, get32(m.data, kRef + 0x58), get32(m.data, kRef + 0x5C), stock_src)) return r;
            }
            std::string why;
            auto resolve = [&](uint32_t v, uint32_t n) -> const char * {
                const Source *s = (v & kStock) == kStock ? stock_src : pack_src;
                const uint64_t o = v & kOffset;
                return s && o + n <= s->data.size() ? s->data.data() + o : nullptr;
            };
            if (!parse(m.data, m.p, resolve, why)) { r.error = shown(m.path) + ": " + why; return r; }
        }
        // ---- stock.shared
        StockLayout stock;
        bool use_stock = false;
        const std::vector<fs::path> stock_paths = stock_multiplayer_maps(stock_maps);
        if (!stock_paths.empty()) {
            log("Laying out stock.shared from the Xbox's 13 stock multiplayer maps...");
            std::string why;
            if (!lay_out_stock(stock_paths, stock, why)) log("  stock.shared isn't used: " + why);
            else use_stock = true;
        } else if (!stock_maps.empty()) {
            log("  stock.shared isn't used: the Xbox maps folder doesn't have the 13 stock multiplayer maps.");
        }
        // ---- what goes where
        Table t;
        for (size_t i = 0; i < maps.size(); i++)
            for (auto &b : maps[i]->p.blocks) {
                if (!b.bytes) continue;
                b.entry = t.add(b.bytes, b.size, b.rem);
                Entry &e = t.e[(size_t)b.entry];
                if (e.last_map != (int)i) { e.last_map = (int)i; e.maps++; }
            }
        Source pack;
        pack.data.assign(kHeader, '\0');
        std::vector<Plan> plans(maps.size());
        uint64_t stock_used = 0;
        for (size_t i = 0; i < maps.size(); i++) {
            Map &m = *maps[i];
            Plan &plan = plans[i];
            plan.go.assign(m.p.blocks.size(), 0);
            plan.at.assign(m.p.blocks.size(), 0);
            for (size_t j = 0; j < m.p.blocks.size(); j++) {
                const Block &b = m.p.blocks[j];
                if (!b.bytes) continue;
                const int s = use_stock ? stock.t.find(b.bytes, b.size, b.rem) : -1;
                Entry &e = t.e[(size_t)b.entry];
                if (s >= 0) {
                    plan.go[j] = 2;
                    plan.at[j] = stock.at[(size_t)s];
                    m.stock_ref = true;
                    m.to_stock += b.size;
                } else if (e.maps >= 2) {
                    if (!e.in_pack) {
                        e.at = place(pack.data, b.bytes, b.size, b.rem);
                        e.in_pack = true;
                    }
                    plan.go[j] = 1;
                    plan.at[j] = e.at;
                    m.pack_ref = true;
                    m.to_pack += b.size;
                }
            }
            m.rewrite = m.pack_ref || m.stock_ref || m.had_ref;
            if (m.stock_ref) stock_used++;
        }
        const bool has_pack = pack.data.size() > kHeader;
        if (has_pack) {
            pack.data.append((kSector - pack.data.size() % kSector) % kSector, '\0');
            if (pack.data.size() > kMaxShared) {
                r.error = name + ".shared would be " + mb(pack.data.size()) + " unpacked: a shared file can be at most 278 MB.";
                return r;
            }
            shared_header(pack.data, name);
            pack.sum = get32(pack.data, 0x64);
            pack.length = (uint32_t)pack.data.size();
        }
        bool any = false;
        for (auto &m : maps) any = any || m->rewrite;
        if (!any) {
            r.ok = true;
            r.report = "Nothing in these maps is in two of them" + std::string(use_stock ? " or in stock.shared" : "") + ": they're left as they are.\n";
            return r;
        }
        // ---- each map laid out again, packed and read back (at once)
        log("Laying the maps out again without what moved, packing and checking them...");
        std::string pack_file, stock_file;
        {
            std::vector<std::thread> th;
            for (size_t i = 0; i < maps.size(); i++) {
                if (!maps[i]->rewrite) continue;
                th.emplace_back([&, i] {
                    Map &m = *maps[i];
                    if (!relayout(m, plans[i], name, pack, stock.file)) return;
                    m.file = pack_map(m.out, m.compressed);
                    check(m, pack, stock.file);
                });
            }
            if (has_pack) th.emplace_back([&] { pack_file = pack_shared(pack.data); });
            if (stock_used) th.emplace_back([&] { stock_file = pack_shared(stock.file.data); });
            for (auto &x : th) x.join();
        }
        for (auto &m : maps)
            if (!m->error.empty()) { r.error = shown(m->path) + ": " + m->error + ". Nothing was written."; return r; }
        auto check_shared = [&](const std::string &file, const std::string &data) {
            std::string back, why;
            bool c;
            return unpack(file, back, c, why) && back == data;
        };
        if ((has_pack && !check_shared(pack_file, pack.data)) || (stock_used && !check_shared(stock_file, stock.file.data))) {
            r.error = "A shared file didn't pack right (unpacked, it isn't what was packed). Nothing was written.";
            return r;
        }
        // ---- written: each file beside the one it replaces, then put in its place
        log("Writing...");
        struct Out { fs::path path; const std::string *file; };
        std::vector<Out> outs;
        const fs::path folder = shared.parent_path();
        auto same = [&](const fs::path &p, const std::string &file) {   // (an identical shared file already there)
            std::ifstream f(p, std::ios::binary);
            std::string h(kHeader, '\0');
            return f && f.read(&h[0], (std::streamsize)kHeader) && h.compare(0, 4, "daeh") == 0 &&
                   get32(h, 0x64) == get32(file, 0x64) && get32(h, 8) == get32(file, 8);
        };
        auto temp = [](const fs::path &p) { fs::path t = p; t += ".new"; return t; };
        if (has_pack && !same(shared, pack_file)) outs.push_back({shared, &pack_file});
        if (stock_used && !same(folder / "stock.shared", stock_file)) outs.push_back({folder / "stock.shared", &stock_file});
        for (auto &m : maps)
            if (m->rewrite) outs.push_back({m->path, &m->file});
        for (auto &o : outs)
            if (!write_file(temp(o.path), *o.file)) {
                std::error_code ec;
                for (auto &x : outs) fs::remove(temp(x.path), ec);
                r.error = "Couldn't write " + temp(o.path).u8string() + " (is the disk full?). Nothing was changed.";
                return r;
            }
        for (auto &o : outs) {
            std::error_code ec;
            fs::remove(o.path, ec);
            fs::rename(temp(o.path), o.path, ec);
            if (ec) { r.error = "Couldn't put " + o.path.u8string() + " in place (" + ec.message() + "): its new copy is " + temp(o.path).u8string() + "."; return r; }
        }
        // ---- the report
        uint64_t before = 0, after = 0;
        for (auto &s : sources)
            if (s.first.filename() != "stock.shared") before += s.second->disk;   // (the pack's file they drew on before)
        std::string &rep = r.report;
        for (auto &m : maps) {
            before += m->disk_size;
            if (!m->rewrite) {
                after += m->disk_size;
                rep += shown(m->path) + ": " + mb(m->disk_size) + ", left as it is (nothing in it is in another map" +
                       (use_stock ? " or in stock.shared" : "") + ")\n";
                continue;
            }
            after += m->file.size();
            rep += shown(m->path) + ": " + mb(m->disk_size) + " -> " + mb(m->file.size()) + " (" + mb(m->out.size()) + " unpacked";
            if (m->to_pack) rep += "; " + mb(m->to_pack) + " of its textures and sounds in " + name + ".shared";
            if (m->to_stock) rep += "; " + mb(m->to_stock) + " in stock.shared";
            rep += ")\n";
        }
        if (has_pack) {
            after += pack_file.size();
            rep += name + ".shared: " + mb(pack_file.size()) + " (" + mb(pack.data.size()) + " unpacked), beside the maps' folders\n";
        }
        if (stock_used)
            rep += "stock.shared: " + mb(stock_file.size()) + " (" + mb(stock.file.data.size()) + " unpacked), beside the maps' folders: every "
                   "player has it once Halo 1 Unlimited's installer builds it, so it isn't counted\n";
        rep += "In all: " + mb(before) + " -> " + mb(after) + ". Every texture, sound, BSP and the tag data were read back through the new files and match.\n";
        r.ok = true;
    } catch (const std::exception &e) {
        r.error = std::string("Making the shared files failed: ") + e.what() + " (not enough memory?). Nothing was written.";
    }
    return r;
}
}   // namespace sharedfile
