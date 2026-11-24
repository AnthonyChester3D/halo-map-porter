// Halo Map Porter - shared files (sharedfile.hpp; Halo 1 Unlimited's docs/shared_maps.md, format version 3).
#include "sharedfile.hpp"
#include "h1ulink.hpp"
#include "inflate.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <unordered_map>

namespace sharedfile {
namespace {
const size_t kHeader = 0x800, kUnit = 512, kSector = 2048;
// A map's moved data goes in whole 2048-byte sectors, as Halo 1 Unlimited's shared_pass.py drops it (what stays keeps its
// place in its sector).
const size_t kDrop = 2048;
// stock.shared as every Halo 1 Unlimited installer builds it from the retail maps (v1.97): its version.
const uint32_t kStockSum = 0xA7FE2F70u, kStockLength = 238745600u;
// The one before (v1.94-1.96: the multiplayer maps alone), whose blocks are where they are in this one.
const uint32_t kOldStockSum = 0xC028F8BFu, kOldStockLength = 78260224u;
const uint64_t kMultiplayerCache = 47ull * 1048576;   // (a map drawing on a packed shared file must fit one unpacked)
const uint32_t kTagBase = 0x803A6000u, kRef = 0x700, kRefSize = 0x68, kRefSize3 = 0x80;   // (version 2; 4 (and 3 before it), with a recipe)
const size_t kEdge = 65536;   // (a map's identity: its size and the CRC32s of this much at its start and its end)
const uint32_t kPack = 0x80000000u, kStock = 0xC0000000u, kOffset = 0x3FFFFFFFu, kMaxShared = 0x11600000u;
const uint32_t kBitmap = 0x6269746Du, kSound = 0x736E6421u;
// Format 4 (shared tag data): frames, runs, the tag cache, and the canonical stock tag pool (from the 23 NTSC retail maps
// by the pool's rule: its identity in a map's reference).
const size_t kFrameWords = 4096;   // (a 16 KB frame of the pool, or slice of a pack's tag part)
const size_t kMinRun = 16;         // (a run is 16 words or more, for the pool and for packs)
const uint64_t kTagCache = 22ull * 1048576, kScratch = 65536;
const uint32_t kPoolCrc = 0xC52152D9u, kPoolSize = 52854036u;
std::atomic<bool> g_recipes{false};
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
// SHA-256 (FIPS 180-4), for the pack's list.
void sha256(const char *data, size_t n, unsigned char out[32]) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    auto rot = [](uint32_t x, int r) { return (x >> r) | (x << (32 - r)); };
    auto block = [&](const unsigned char *b) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)b[4 * i] << 24 | b[4 * i + 1] << 16 | b[4 * i + 2] << 8 | b[4 * i + 3];
        for (int i = 16; i < 64; i++)
            w[i] = w[i - 16] + (rot(w[i - 15], 7) ^ rot(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7] +
                   (rot(w[i - 2], 17) ^ rot(w[i - 2], 19) ^ (w[i - 2] >> 10));
        uint32_t a = h[0], b2 = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            const uint32_t t1 = hh + (rot(e, 6) ^ rot(e, 11) ^ rot(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (rot(a, 2) ^ rot(a, 13) ^ rot(a, 22)) + ((a & b2) ^ (a & c) ^ (b2 & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b2; b2 = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b2; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    };
    size_t i = 0;
    for (; i + 64 <= n; i += 64) block((const unsigned char *)data + i);
    unsigned char last[128] = {0};
    const size_t r = n - i;
    memcpy(last, data + i, r);
    last[r] = 0x80;
    const size_t blocks = r + 9 > 64 ? 2 : 1;
    const uint64_t bits = (uint64_t)n * 8;
    for (int j = 0; j < 8; j++) last[blocks * 64 - 1 - j] = (unsigned char)(bits >> (8 * j));
    for (size_t j = 0; j < blocks; j++) block(last + 64 * j);
    for (int j = 0; j < 8; j++)
        for (int q = 0; q < 4; q++) out[4 * j + q] = (unsigned char)(h[j] >> (24 - 8 * q));
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
    if (!inflate::zlib((const uint8_t *)disk.data() + kHeader, disk.size() - kHeader, rest, length - kHeader)) {
        why = "it doesn't unpack (damaged?)";
        return false;
    }
    // (some older tools' packed maps stop where the tag data ends, leaving out the zeros that pad the map to its
    // header's length: those zeros are put back, as the Xbox's own unpacking leaves them)
    const uint64_t ends = (uint64_t)get32(disk, 0x10) + get32(disk, 0x14);
    if (rest.size() < length - kHeader && length - kHeader - rest.size() < 2048 && kHeader + rest.size() >= ends)
        rest.resize(length - kHeader, '\0');
    if (rest.size() != length - kHeader) {
        why = "it doesn't unpack (damaged?)";
        return false;
    }
    data.reserve(length);
    data.assign(disk, 0, kHeader);
    data += rest;
    return true;
}

// Packed as a map is (one zlib stream after the header), in whole 2048-byte sectors; its header as it is (as the
// installer's pass makes them: the same maps make the same small maps, byte for byte unpacked).
std::string pack_map(const std::string &data, bool compressed) {
    if (!compressed) return data;
    std::string file = data.substr(0, kHeader) + inflate::zlib_compress((const uint8_t *)data.data() + kHeader, data.size() - kHeader);
    file.append((kSector - file.size() % kSector) % kSector, '\0');
    return file;
}
// A shared file packed: whole 2048-byte sectors (docs/shared_maps.md).
std::string pack_shared(const std::string &data) {
    std::string file = data.substr(0, kHeader) + inflate::zlib_compress((const uint8_t *)data.data() + kHeader, data.size() - kHeader);
    file.append((kSector - file.size() % kSector) % kSector, '\0');
    return file;
}

uint64_t hash_bytes(const char *p, size_t n, uint64_t seed = 0x9E3779B97F4A7C15ull) {
    uint64_t h = seed ^ (uint64_t)n;
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

// ---- Format 4 (docs/shared_maps.md, "Shared tag data"): a map's tag data rebuilt as it loads, by a recipe in the map,
// from the stock tag pool (stocktags.shared, which every Xbox's installer builds from its own retail maps) and from its
// pack's tag part (the words two or more of the pack's maps have that the pool doesn't, in its .shared file).

// Each 8-word window of a word sequence, at its earliest position.
struct Windows {
    const std::vector<uint32_t> *w = nullptr;
    std::vector<uint64_t> slot;   // (the hash's top half | the position)
    size_t mask = 0, used = 0;
    void init(const std::vector<uint32_t> *src, int bits) { w = src; slot.assign((size_t)1 << bits, 0); mask = slot.size() - 1; used = 0; }
    static uint64_t hash(const uint32_t *p) {
        uint64_t h = 0x9E3779B97F4A7C15ull;
        for (int i = 0; i < 8; i++) { h ^= p[i]; h *= 0xFF51AFD7ED558CCDull; h ^= h >> 29; }
        return h | (1ull << 63);
    }
    void grow() {
        std::vector<uint64_t> old;
        old.swap(slot);
        slot.assign(old.size() * 2, 0);
        mask = slot.size() - 1;
        for (uint64_t s : old)
            if (s) {
                size_t i = (s >> 32) & mask;
                while (slot[i]) i = (i + 1) & mask;
                slot[i] = s;
            }
    }
    void add(size_t pos) {
        if ((used + 1) * 2 > slot.size()) grow();
        const uint64_t h = hash(&(*w)[pos]);
        size_t i = (h >> 32) & mask;
        while (slot[i]) {
            if ((slot[i] >> 32) == (h >> 32) && memcmp(&(*w)[(uint32_t)slot[i]], &(*w)[pos], 32) == 0) return;   // (the earlier one stays)
            i = (i + 1) & mask;
        }
        slot[i] = (h & 0xFFFFFFFF00000000ull) | (uint32_t)pos;
        used++;
    }
    long find(const uint32_t *p) const {
        const uint64_t h = hash(p);
        size_t i = (h >> 32) & mask;
        while (slot[i]) {
            if ((slot[i] >> 32) == (h >> 32) && memcmp(&(*w)[(uint32_t)slot[i]], p, 32) == 0) return (long)(uint32_t)slot[i];
            i = (i + 1) & mask;
        }
        return -1;
    }
};
struct Pool {
    std::vector<uint32_t> w;
    Windows idx;
};
// The pool by its rule: the 23 maps in kStockMaps' order, each one's tag data as words walked against the pool so far
// (this map's own new words too): a word's 8-word window at its earliest place in the pool, extended forward; 16 words or
// more is a copy, else that word is new (appended).
bool build_pool(const std::vector<fs::path> &paths, std::vector<uint32_t> &pool, std::string &why) {
    pool.clear();
    Windows idx;
    idx.init(&pool, 24);
    for (auto &p : paths) {
        std::vector<uint32_t> w;
        {
            std::string disk, data;
            bool c;
            if (!read_file(p, disk)) { why = name_of(p) + " couldn't be read"; return false; }
            if (!unpack(disk, data, c, why)) { why = name_of(p) + ": " + why; return false; }
            const uint32_t to = get32(data, 0x10), ts = get32(data, 0x14);
            if (ts % 4 || (uint64_t)to + ts > data.size()) { why = name_of(p) + "'s tag data isn't whole words"; return false; }
            w.resize(ts / 4);
            memcpy(w.data(), data.data() + to, ts);
        }
        for (size_t i = 0; i < w.size();) {
            size_t k = 0;
            if (i + 8 <= w.size()) {
                const long q = idx.find(&w[i]);
                if (q >= 0) while (i + k < w.size() && (size_t)q + k < pool.size() && w[i + k] == pool[(size_t)q + k]) k++;
            }
            if (k >= kMinRun) { i += k; continue; }
            pool.push_back(w[i++]);
            if (pool.size() >= 8) idx.add(pool.size() - 8);
        }
    }
    return true;
}
// The pool (made once from the Xbox's retail maps, kept in `cache` as stocktags.pool), with its windows. Null (`why`)
// when it can't be had, or the maps make another one.
const Pool *stock_pool(const fs::path &cache, const fs::path &stock_maps, const std::function<void(const std::string &)> &log, std::string &why) {
    static std::mutex mu;
    static std::unique_ptr<Pool> have;
    std::lock_guard<std::mutex> lock(mu);
    if (have) return have.get();
    const fs::path file = cache / "stocktags.pool";
    std::string raw;
    if (!read_file(file, raw) || raw.size() != kPoolSize || crc32_of(raw.data(), raw.size()) != kPoolCrc) {
        const std::vector<fs::path> paths = stock_maps_in(stock_maps);
        if (paths.empty()) { why = "the Xbox maps folder needs the 23 retail maps (the stock tag pool is made from them)"; return nullptr; }
        log("Making the stock tag pool from the Xbox's 23 retail maps (once: it's kept)...");
        std::vector<uint32_t> w;
        if (!build_pool(paths, w, why)) return nullptr;
        raw.assign((const char *)w.data(), w.size() * 4);
        const uint32_t crc = crc32_of(raw.data(), raw.size());
        if (raw.size() != kPoolSize || crc != kPoolCrc) {
            char t[200];
            snprintf(t, sizeof t, "from these maps the stock tag pool comes out %08x (%zu bytes), not the canonical %08x (%u bytes): the Xbox maps "
                     "folder's maps aren't the NTSC retail ones", crc, raw.size(), kPoolCrc, kPoolSize);
            why = t;
            return nullptr;
        }
        std::error_code ec;
        fs::create_directories(cache, ec);
        write_file(file, raw);
    }
    auto p = std::make_unique<Pool>();
    p->w.resize(raw.size() / 4);
    memcpy(p->w.data(), raw.data(), raw.size());
    p->idx.init(&p->w, 25);
    for (size_t i = 0; i + 8 <= p->w.size(); i++) p->idx.add(i);
    have = std::move(p);
    return have.get();
}
void leb(std::string &o, uint64_t v) {
    do {
        uint8_t b = v & 0x7F;
        v >>= 7;
        if (v) b |= 0x80;
        o.push_back((char)b);
    } while (v);
}
bool unleb(const std::string &r, size_t &at, uint64_t &v) {
    v = 0;
    for (int s = 0; s < 64; s += 7) {
        if (at >= r.size()) return false;
        const uint8_t b = (uint8_t)r[at++];
        v |= (uint64_t)(b & 0x7F) << s;
        if (!(b & 0x80)) return true;
    }
    return false;
}
// A map's tag data rebuilt from its recipe (a reference of version 3 or 4 with one), in place: false (`why`) if the
// recipe or what it copies from is wrong. Version 4: the file stops at its tag data, the recipe after it (its own words
// too); rebuilt, the map is its part before the tag data and the tag data. `check`: a map read back, held to every rule
// (version 3: the words it writes are zeros in the map; version 4: its recipe where it must be, after zeros, and the
// map's other parts before its tag data).
bool apply_recipe(std::string &d, const Pool *pool, const std::string *pack, bool check, std::string &why) {
    if (d.size() < kHeader || d.compare(kRef, 4, "drhs") != 0) return true;
    const uint32_t version = get32(d, kRef + 4);
    if (version != 3 && version != 4) return true;
    const uint32_t ro = get32(d, kRef + 0x68), rs = get32(d, kRef + 0x6C), to = get32(d, 0x10), ts = get32(d, 0x14);
    if (!rs) return true;
    if ((uint64_t)ro + rs > d.size()) { why = "its recipe isn't in the file"; return false; }
    if (version == 3 && (uint64_t)to + ts > d.size()) { why = "its tag data isn't in the file"; return false; }
    if (version == 4) {
        if (ts % 4) { why = "its tag data isn't whole words"; return false; }
        if (get32(d, kRef + 0x7C)) { why = "its reference has a flag this doesn't know"; return false; }
        const uint64_t at = ((uint64_t)to + kSector - 1) / kSector * kSector;
        if (ro != at || to < kHeader) { why = "its recipe isn't where it must be (its tag data's offset rounded up to a sector)"; return false; }
        if (check)
            for (uint64_t x = to; x < ro; x++)
                if (d[x]) { why = "what's between its tag data's offset and its recipe isn't zeros"; return false; }
    }
    const std::string rec = d.substr(ro, rs);
    const size_t words = ts / 4;
    std::string tag;   // (version 4: rebuilt here, then put in place)
    if (version == 4) tag.assign(ts, '\0');
    char *dst = version == 4 ? &tag[0] : &d[to];
    std::vector<uint8_t> written(words, 0);
    const uint32_t *tp = nullptr;
    size_t tp_words = 0;
    if (pack) {
        const uint32_t po = get32(*pack, 0x68), ps = get32(*pack, 0x6C);
        if (ps && (uint64_t)po + ps <= pack->size() && ps % 4 == 0) { tp = (const uint32_t *)(pack->data() + po); tp_words = ps / 4; }
    }
    // a kind's groups (version 3's parts, version 4's sections 1 and 2), from `at` up to `end`
    auto groups = [&](size_t &at, size_t end, int kind, bool exact) -> bool {
        const uint32_t *src = kind == 0 ? (pool ? pool->w.data() : nullptr) : tp;
        const size_t src_words = kind == 0 ? (pool ? pool->w.size() : 0) : tp_words;
        const std::string part = rec.substr(0, end);
        uint64_t count, frame = 0;
        if (!unleb(part, at, count)) { why = "its recipe is cut short"; return false; }
        if (count && !src) {
            why = kind == 0 ? "its recipe copies from a stock tag pool that isn't here" : "its recipe copies from its pack's tag part, and its pack's file has none";
            return false;
        }
        if (kind == 0 && count && (get32(d, kRef + 0x70) != kPoolCrc || get32(d, kRef + 0x74) != kPoolSize)) {
            why = "it was made against another stock tag pool";
            return false;
        }
        for (uint64_t g = 0; g < count; g++) {
            uint64_t f, runs;
            if (!unleb(part, at, f) || !unleb(part, at, runs)) { why = "its recipe is cut short"; return false; }
            if (g && !f) { why = "its recipe's frames aren't in order"; return false; }
            frame = g ? frame + f : f;
            const uint64_t first = frame * kFrameWords;
            if (first >= src_words || !runs) { why = "its recipe names a frame that isn't there"; return false; }
            const uint64_t fwords = std::min<uint64_t>(kFrameWords, src_words - first);
            uint64_t end_w = 0, last_off = UINT64_MAX;
            for (uint64_t k = 0; k < runs; k++) {
                uint64_t dd, off, len;
                if (!unleb(part, at, dd) || !unleb(part, at, off) || !unleb(part, at, len)) { why = "its recipe is cut short"; return false; }
                const uint64_t dest = end_w + dd;
                if (!len || off + len > fwords || dest + len > words) { why = "its recipe has a run outside its tag data or a frame"; return false; }
                if (check && k && !dd && off == last_off) { why = "its recipe's runs aren't maximal"; return false; }
                for (uint64_t x = dest; x < dest + len; x++) {
                    if (written[x]) { why = "its recipe writes a word twice"; return false; }
                    written[x] = 1;
                }
                if (check && version == 3)
                    for (uint64_t x = dest; x < dest + len; x++)
                        if (get32(d, to + x * 4) != 0) { why = "a word its recipe writes isn't zero in the map"; return false; }
                memcpy(dst + dest * 4, src + first + off, len * 4);
                end_w = dest + len;
                last_off = off + len;
            }
        }
        if (exact && at != end) { why = "a section of its recipe has more in it than its runs"; return false; }
        return true;
    };
    size_t at = 0;
    if (version == 3) {
        if (!groups(at, rec.size(), 0, false) || !groups(at, rec.size(), 1, true)) return false;
    } else {
        uint64_t sections, last_type = 0;
        bool seen[4] = {false, false, false, false};
        if (!unleb(rec, at, sections)) { why = "its recipe is cut short"; return false; }
        for (uint64_t k = 0; k < sections; k++) {
            uint64_t type, size;
            if (!unleb(rec, at, type) || !unleb(rec, at, size) || at + size > rec.size()) { why = "its recipe is cut short"; return false; }
            if (k && type <= last_type) { why = "its recipe's sections aren't in order"; return false; }
            last_type = type;
            const size_t end = at + (size_t)size;
            if (type == 1 || type == 2) {
                if (!groups(at, end, (int)type - 1, true)) return false;
            } else if (type == 3) {   // (the own words)
                const std::string part = rec.substr(0, end);
                uint64_t runs, end_w = 0;
                if (!unleb(part, at, runs)) { why = "its recipe is cut short"; return false; }
                for (uint64_t r = 0; r < runs; r++) {
                    uint64_t dd, len;
                    if (!unleb(part, at, dd) || !unleb(part, at, len)) { why = "its recipe is cut short"; return false; }
                    const uint64_t dest = end_w + dd;
                    if (!len || dest + len > words || at + len * 4 > end) { why = "its recipe's own words run outside its tag data"; return false; }
                    if (r && !dd) { why = "its recipe's own runs aren't maximal"; return false; }
                    for (uint64_t x = dest; x < dest + len; x++) {
                        if (written[x]) { why = "its recipe writes a word twice"; return false; }
                        written[x] = 1;
                    }
                    memcpy(dst + dest * 4, rec.data() + at, len * 4);
                    at += (size_t)len * 4;
                    end_w = dest + len;
                }
                if (at != end) { why = "its recipe's own words section has more in it than its runs"; return false; }
            } else if (type < 64) { why = "its recipe has a section this doesn't know"; return false; }
            else at = end;   // (64 and up: skipped)
            if (type <= 3) seen[type] = true;
        }
        if (!seen[1] || !seen[2] || !seen[3]) { why = "its recipe is missing a section"; return false; }
        for (size_t x = 0; x < words; x++)
            if (!written[x]) { why = "its recipe leaves a word of its tag data unwritten"; return false; }
    }
    if (at != rec.size()) { why = "its recipe has more in it than its sections"; return false; }
    if (version == 4) {
        d.resize(to);
        d += tag;
    }
    if (crc32_of(d.data() + to, ts) != get32(d, kRef + 0x78)) { why = "its tag data rebuilt isn't what it was made from (CRC32)"; return false; }
    return true;
}
// A run of a recipe: `len` words written at `dest` from `src` (in the pool, or the pack's tag part).
struct Run {
    uint32_t dest, src, len;
    bool pack;
};
// A recipe's stock and pack groups for these runs (format 4): split at frames, maximal, each kind's groups by frame
// (version 3's two parts; version 4's sections 1 and 2). `runs` comes back as they're written.
void encode_groups(std::vector<Run> &runs, std::string part[2], std::set<uint32_t> &frames) {
    std::vector<Run> split;
    for (auto &r : runs)
        for (uint32_t s = r.src, d = r.dest, left = r.len; left;) {
            const uint32_t n = std::min<uint32_t>(left, (uint32_t)(kFrameWords - s % kFrameWords));
            split.push_back({d, s, n, r.pack});
            s += n, d += n, left -= n;
        }
    std::sort(split.begin(), split.end(), [](const Run &a, const Run &b) {
        const uint32_t fa = a.src / kFrameWords, fb = b.src / kFrameWords;
        return std::tie(a.pack, fa, a.dest) < std::tie(b.pack, fb, b.dest);
    });
    std::vector<Run> m;   // (maximal: one that continues the last in its tag data and its frame joins it)
    for (auto &r : split) {
        if (!m.empty() && m.back().pack == r.pack && m.back().src / kFrameWords == r.src / kFrameWords && m.back().dest + m.back().len == r.dest &&
            m.back().src + m.back().len == r.src)
            m.back().len += r.len;
        else m.push_back(r);
    }
    for (int kind = 0; kind < 2; kind++) {
        std::string &out = part[kind];
        out.clear();
        std::vector<std::pair<size_t, size_t>> groups;   // (first, end) in m
        for (size_t i = 0; i < m.size();) {
            if (m[i].pack != (kind == 1)) { i++; continue; }
            size_t j = i;
            while (j < m.size() && m[j].pack == m[i].pack && m[j].src / kFrameWords == m[i].src / kFrameWords) j++;
            groups.push_back({i, j});
            i = j;
        }
        leb(out, groups.size());
        uint32_t last = 0;
        for (size_t g = 0; g < groups.size(); g++) {
            const uint32_t f = m[groups[g].first].src / kFrameWords;
            if (kind == 0) frames.insert(f);
            leb(out, g ? f - last : f);
            last = f;
            leb(out, groups[g].second - groups[g].first);
            uint32_t end = 0;
            for (size_t k = groups[g].first; k < groups[g].second; k++) {
                leb(out, m[k].dest - end);
                leb(out, m[k].src % kFrameWords);
                leb(out, m[k].len);
                end = m[k].dest + m[k].len;
            }
        }
    }
    runs.swap(m);
}
// Version 4's recipe: its sections (count; each one's type, size and bytes): 1 the stock groups, 2 the pack's, 3 the own
// words (run count; each maximal run of words no stock or pack run writes: dest delta from the end of the last own run,
// length, then the words raw).
std::string encode_v4(std::vector<Run> runs, const uint32_t *tag, size_t words, size_t &count, std::set<uint32_t> &frames) {
    std::string part[2];
    encode_groups(runs, part, frames);
    std::vector<uint8_t> by(words, 0);
    for (auto &r : runs) memset(&by[r.dest], 1, r.len);
    std::string own;
    std::vector<std::pair<size_t, size_t>> oruns;
    for (size_t i = 0; i < words;) {
        if (by[i]) { i++; continue; }
        size_t j = i;
        while (j < words && !by[j]) j++;
        oruns.push_back({i, j - i});
        i = j;
    }
    leb(own, oruns.size());
    size_t end = 0;
    for (auto &o : oruns) {
        leb(own, o.first - end);
        leb(own, o.second);
        own.append((const char *)(tag + o.first), o.second * 4);
        end = o.first + o.second;
    }
    count = runs.size() + oruns.size();
    std::string out;
    leb(out, 3);
    const std::string *sec[3] = {&part[0], &part[1], &own};
    for (int t = 0; t < 3; t++) {
        leb(out, (uint64_t)t + 1);
        leb(out, sec[t]->size());
        out += *sec[t];
    }
    return out;
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
        return i >= 0 ? i : add_new(b, n, rem);
    }
    int add_new(const char *b, uint32_t n, uint32_t rem) {   // (known not to be there)
        e.push_back({b, n, rem});
        by[key(b, n, rem)].push_back((int)e.size() - 1);
        return (int)e.size() - 1;
    }
};

// A shared file's header (docs/shared_maps.md), its data laid out after it (a pack's after its list, `list` bytes: the
// checksum leaves the list out, as the list names each small map's CRC and each small map names this checksum).
void shared_header(std::string &data, const std::string &name, uint32_t list = 0, uint32_t tag_off = 0, uint32_t tag_size = 0) {
    std::string h(kHeader, '\0');
    h.replace(0, 4, "daeh");
    put32(h, 4, 5);
    put32(h, 8, (uint32_t)data.size());
    h.replace(0x20, name.size(), name);
    put32(h, 0x18, list);
    h.replace(0x40, 6, "shared");
    put32(h, 0x68, tag_off);   // (its tag part: format 4)
    put32(h, 0x6C, tag_size);
    put32(h, 0x64, crc32_of(data.data() + kHeader + list, data.size() - kHeader - list));
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
    uint32_t ident_size = 0, ident_sum = 0;   // the original's size on disk and its header's checksum
    uint32_t ident_head = 0, ident_tail = 0;  // the CRC32s of its first and last 64 KB (its identity, with its size)
    std::string data;   // unpacked
    Parsed p;
    // what becomes of it
    std::string out, file;
    uint64_t to_pack = 0, to_stock = 0;
    bool rewrite = false, pack_ref = false, stock_ref = false;
    std::string error;
    std::string skipped;   // why it's left as it is (it can't be read)
    std::string ref_name;  // the pack's file it drew on
    // format 4: its recipe (tag data rebuilt from the stock tag pool and the pack's tag part)
    std::string recipe;
    uint32_t tag_crc = 0;
    size_t recipe_runs = 0, from_pool = 0, from_pack = 0, own_words = 0;   // (runs; words)
    std::set<uint32_t> pool_frames;
    uint32_t recipe_cut = 0;   // (read: where its old recipe was)
};

// Where each block goes: 0 stays, 1 the pack's file, 2 stock.shared.
struct Plan {
    std::vector<int> go;
    std::vector<uint32_t> at;   // its offset in that shared file
};

// The map's reference (its header's 0x700): version 2, or 3 with its recipe (at `recipe_at` in it).
void write_ref(Map &m, const std::string &pack_name, const Source &pack, const Source &stock, uint32_t recipe_at = 0) {
    std::string ref(kRefSize3, '\0');
    const bool recipe = !m.recipe.empty();
    if (m.pack_ref || m.stock_ref || recipe) {
        ref.replace(0, 4, "drhs");
        put32(ref, 4, recipe ? 4 : 2);
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
        put32(ref, 0x60, m.ident_head);
        put32(ref, 0x64, m.ident_tail);
        if (recipe) {
            put32(ref, 0x68, recipe_at);
            put32(ref, 0x6C, (uint32_t)m.recipe.size());
            put32(ref, 0x70, kPoolCrc);
            put32(ref, 0x74, kPoolSize);
            put32(ref, 0x78, m.tag_crc);
            put32(ref, 0x7C, 0);   // (flags: none yet)
        }
    }
    m.out.replace(kRef, kRefSize3, ref);
}

// The map laid out again: what moved left out in whole 512-byte units, each kept unit moved down; blocks that stay but
// weren't in the map (they were in a shared file) placed after the rest. Returns false (`m.error`) if it can't be.
bool relayout(Map &m, const Plan &plan, const std::string &pack_name, const Source &pack, const Source &stock) {
    const std::string &d = m.data;
    const Parsed &p = m.p;
    const size_t len = d.size(), units = (len - kHeader + kDrop - 1) / kDrop;
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
        for (uint64_t u = (r.first - kHeader + kDrop - 1) / kDrop; u < (r.second - kHeader) / kDrop && u < units; u++) drop[u] = 1;
    // what stays keeps its units: the tag data, the BSPs, the blocks that stay
    std::vector<std::pair<uint64_t, uint64_t>> kept;
    kept.push_back({p.td_off, (uint64_t)p.td_off + p.td_size});
    for (size_t f : p.bsp_fields) kept.push_back({get32(d, f), (uint64_t)get32(d, f) + std::max<uint32_t>(get32(d, f + 4), 1)});
    for (size_t j = 0; j < p.blocks.size(); j++)
        if (p.blocks[j].own && !plan.go[j]) kept.push_back({p.blocks[j].value, (uint64_t)p.blocks[j].value + p.blocks[j].size});
    for (auto &r : kept) {
        uint64_t a = std::max<uint64_t>(r.first, kHeader), b = std::min<uint64_t>(r.second, len);
        if (b > a)
            for (uint64_t u = (a - kHeader) / kDrop; u <= (b - 1 - kHeader) / kDrop; u++) drop[u] = 0;
    }
    std::vector<uint32_t> before(units + 1, 0);
    for (size_t u = 0; u < units; u++) before[u + 1] = before[u] + drop[u];
    bool bad = false;
    auto move = [&](uint64_t o) -> uint32_t {
        if (o < kHeader) return (uint32_t)o;
        const uint64_t u = (o - kHeader) / kDrop;
        if (u >= units) return (uint32_t)(o - (uint64_t)before[units] * kDrop);
        if (drop[u]) bad = true;
        return (uint32_t)(o - (uint64_t)before[u] * kDrop);
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
    out.reserve(len - (size_t)before[units] * kDrop + 1024);
    out.append(w, 0, kHeader);
    for (size_t u = 0; u < units; u++)
        if (!drop[u]) out.append(w, kHeader + u * kDrop, std::min(kDrop, len - kHeader - u * kDrop));
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
    for (size_t j = 0; j < p.blocks.size(); j++) put32(out, move(p.blocks[j].field), value[j]);
    for (size_t f : p.bsp_fields) put32(out, move(f), move(get32(d, f)));
    put32(out, 0x10, move(p.td_off));
    if (bad) { m.error = "something that stays in it lies in a sector of what moved"; return false; }
    out.append((kSector - out.size() % kSector) % kSector, '\0');   // (whole sectors: the game copies a map to its cache in them)
    put32(out, 8, (uint32_t)out.size());
    write_ref(m, pack_name, pack, stock);
    return true;
}

// The pack pass (format 4): each map's tag data, in the maps' order, matched against the stock tag pool and the pack's
// candidates (the words earlier maps kept as their own, in the order they came; a map's join once it's done): at a word,
// its 8-word window's earliest place in each, extended forward; the longer wins (a tie: the pool); 16 words or more is a
// run, else that word is the map's own. The pack's tag part is every candidate word a later map's run uses, in candidate
// order; a map's own words in it are runs from it in its recipe too. Each map's recipe made (version 4: the rest of its
// words, its own, carried in the recipe).
// A map whose tag data leaves no room to rebuild it in (64 KB below the 22 MB tag cache's top) has none.
void tag_pass(const std::vector<Map *> &ms, const Pool &pool, std::vector<uint32_t> &tag_part) {
    std::vector<uint32_t> cand;
    Windows cx;
    cx.init(&cand, 18);
    std::vector<uint8_t> used;
    std::vector<size_t> cand_first(ms.size(), 0);
    std::vector<std::vector<uint32_t>> own(ms.size());   // (each map's own words' places: its candidates, in order)
    std::vector<std::vector<Run>> runs(ms.size());
    std::vector<uint8_t> room(ms.size(), 0);
    const std::vector<uint32_t> &P = pool.w;
    for (size_t k = 0; k < ms.size(); k++) {
        Map &m = *ms[k];
        const uint32_t to = get32(m.out, 0x10), ts = get32(m.out, 0x14);
        if ((uint64_t)ts + kScratch > kTagCache || (uint64_t)to + ts > m.out.size() || ts % 4) continue;   // (and whole words)
        room[k] = 1;
        const size_t n = ts / 4;
        std::vector<uint32_t> T(n);
        memcpy(T.data(), m.out.data() + to, n * 4);
        for (size_t i = 0; i < n;) {
            size_t lp = 0, lc = 0;
            long p = -1, c = -1;
            if (i + 8 <= n) {
                p = pool.idx.find(&T[i]);
                if (p >= 0) while (i + lp < n && (size_t)p + lp < P.size() && T[i + lp] == P[(size_t)p + lp]) lp++;
                if (!cand.empty()) c = cx.find(&T[i]);
                if (c >= 0) while (i + lc < n && (size_t)c + lc < cand.size() && T[i + lc] == cand[(size_t)c + lc]) lc++;
            }
            const size_t len = std::max(lp, lc);
            if (len >= kMinRun) {
                if (lp >= lc) runs[k].push_back({(uint32_t)i, (uint32_t)p, (uint32_t)lp, false});
                else {
                    runs[k].push_back({(uint32_t)i, (uint32_t)c, (uint32_t)lc, true});
                    std::fill(used.begin() + c, used.begin() + c + (long)lc, 1);
                }
                i += len;
            } else own[k].push_back((uint32_t)i++);
        }
        const size_t old = cand.size();   // (its own words: the next maps' candidates)
        cand_first[k] = old;
        for (uint32_t d : own[k]) cand.push_back(T[d]);
        used.resize(cand.size(), 0);
        for (size_t q = old >= 7 ? old - 7 : 0; q + 8 <= cand.size(); q++) cx.add(q);
    }
    std::vector<uint32_t> tpos(cand.size(), 0);
    tag_part.clear();
    for (size_t q = 0; q < cand.size(); q++)
        if (used[q]) { tpos[q] = (uint32_t)tag_part.size(); tag_part.push_back(cand[q]); }
    for (size_t k = 0; k < ms.size(); k++) {
        if (!room[k]) continue;
        Map &m = *ms[k];
        std::vector<Run> rr;
        for (auto &r : runs[k]) {
            if (!r.pack) { rr.push_back(r); m.from_pool += r.len; }
            else { rr.push_back({r.dest, tpos[r.src], r.len, true}); m.from_pack += r.len; }
        }
        for (size_t j = 0; j < own[k].size(); j++) {   // (its own words in the tag part)
            const size_t q = cand_first[k] + j;
            if (!used[q]) continue;
            rr.push_back({own[k][j], tpos[q], 1, true});
            m.from_pack++;
        }
        if (rr.empty()) continue;   // (nothing in it from elsewhere: no recipe)
        const uint32_t to = get32(m.out, 0x10), ts = get32(m.out, 0x14);
        m.tag_crc = crc32_of(m.out.data() + to, ts);
        m.own_words = ts / 4 - m.from_pool - m.from_pack;
        m.recipe = encode_v4(rr, (const uint32_t *)(m.out.data() + to), ts / 4, m.recipe_runs, m.pool_frames);
    }
}

// Version 4: the tag data the map's last part (nothing else ends after it begins). What a re-share put after it (blocks
// brought back into the map) or another tool left there is placed again before it, each at its remainder modulo 512,
// the tag data after them at its own; what points at them pointed again.
bool tag_data_last(std::string &out, std::string &why) {
    static const char none = 0;
    Parsed q;
    if (!parse(out, q, [](uint32_t, uint32_t) { return &none; }, why)) return false;
    const uint32_t to = q.td_off, ts = q.td_size;
    bool any = false;
    for (auto &b : q.blocks) any = any || (b.own && (uint64_t)b.value + b.size > to);
    for (size_t f : q.bsp_fields) any = any || (uint64_t)get32(out, f) + get32(out, f + 4) > to;
    if (!any) return true;
    std::string n = out.substr(0, to);
    std::map<uint64_t, uint32_t> placed;   // ((offset, size): where it is now)
    std::vector<std::pair<size_t, uint32_t>> fields;
    auto again = [&](size_t field, uint32_t o, uint32_t size) {
        const uint64_t key = (uint64_t)o << 32 | size;
        auto it = placed.find(key);
        if (it == placed.end()) it = placed.insert({key, place(n, out.data() + o, size, o % kUnit)}).first;
        fields.push_back({field, it->second});
    };
    for (auto &b : q.blocks)
        if (b.own && (uint64_t)b.value + b.size > to) again(b.field, b.value, b.size);
    for (size_t f : q.bsp_fields)
        if ((uint64_t)get32(out, f) + get32(out, f + 4) > to) again(f, get32(out, f), get32(out, f + 4));
    const uint32_t nt = place(n, out.data() + to, ts, to % kUnit);
    for (auto &f : fields) put32(n, f.first - to + nt, f.second);
    put32(n, 0x10, nt);
    n.append((kSector - n.size() % kSector) % kSector, '\0');
    put32(n, 8, (uint32_t)n.size());
    out.swap(n);
    return true;
}

// The new map read back: unpacked again, every texture and sound through the new files the same bytes as before, the
// BSPs and the tag data the same (but for the offsets that changed), the header the same but for what changed.
bool check(Map &m, const Source &pack, const Source &stock, const Pool *pool = nullptr) {
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
    // (a recipe map: its tag data rebuilt first, as the game does, its CRC32 checked)
    std::string rebuilt;
    if (!m.recipe.empty()) {
        rebuilt = m.out;
        if (!apply_recipe(rebuilt, pool, &pack.data, true, why)) { m.error = "read back, " + why; return false; }
    }
    const std::string &o = m.recipe.empty() ? m.out : rebuilt;
    if (!parse(o, q, [&](uint32_t v, uint32_t n) { return (v & kStock) == kStock ? from(stock, v, n) : from(pack, v, n); }, why)) {
        m.error = "read back, " + why;
        return false;
    }
    if (!m.recipe.empty()) {   // (version 4: nothing else ends after its tag data begins)
        for (auto &b : q.blocks)
            if (b.own && (uint64_t)b.value + b.size > q.td_off) { m.error = "read back, a texture or sound lies after its tag data"; return false; }
        for (size_t f : q.bsp_fields)
            if ((uint64_t)get32(o, f) + get32(o, f + 4) > q.td_off) { m.error = "read back, a BSP lies after its tag data"; return false; }
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
    const std::string &d = m.data;
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
    h0.replace(kRef, kRefSize3, kRefSize3, '\0');
    h1.replace(kRef, kRefSize3, kRefSize3, '\0');
    if (h0 != h1) { m.error = "read back, its header isn't the same"; return false; }
    return true;
}

// stock.shared as Halo 1 Unlimited's installer lays it out (docs/shared_maps.md, "stock.shared's layout", v1.97): the 13
// stock multiplayer maps in the order of their names, every block; then the 10 campaign levels in theirs, only the
// blocks three levels or more use (counted once a level); each map's blocks in tag index order, each block's first copy
// (the same bytes at the same remainder modulo 512) placed after the last. BSPs and ui.map aren't in it.
const char *const kStockMaps[] = {"beavercreek", "bloodgulch", "boardingaction", "carousel", "chillout", "damnation", "hangemhigh",
                                  "longest", "prisoner", "putput", "ratrace", "sidewinder", "wizard",
                                  "a10", "a30", "a50", "b30", "b40", "c10", "c20", "c40", "d20", "d40"};
const size_t kStockMultiplayer = 13, kStockLevelsBy = 3;
struct StockLayout {
    Source file;
    Table t;              // its blocks (their bytes in `file`)
    std::vector<uint32_t> at;   // each entry's offset
};
bool load_map(const fs::path &p, std::string &data, Parsed &parsed, std::string &why) {
    std::string disk;
    bool c;
    if (!read_file(p, disk)) { why = "couldn't be read"; return false; }
    if (!unpack(disk, data, c, why)) return false;
    return parse(data, parsed, [](uint32_t, uint32_t) { return (const char *)nullptr; }, why);
}
bool lay_out_stock(const std::vector<fs::path> &paths, StockLayout &s, std::string &why) {
    std::string &data = s.file.data;
    data.reserve(kMaxShared + kSector);   // (the entries point into it: it never moves)
    data.assign(kHeader, '\0');
    auto put = [&](const Block &b) -> bool {
        if (s.t.find(b.bytes, b.size, b.rem) >= 0) return true;
        if (data.size() + kUnit + b.size > kMaxShared) { why = "it would be over 278 MB"; return false; }
        const uint32_t at = place(data, b.bytes, b.size, b.rem);
        s.t.add_new(data.data() + at, b.size, b.rem);
        s.at.push_back(at);
        return true;
    };
    // the multiplayer maps: every block
    {
        std::vector<std::string> datas(kStockMultiplayer), errors(kStockMultiplayer);
        std::vector<Parsed> parsed(kStockMultiplayer);
        std::vector<std::thread> th;
        for (size_t i = 0; i < kStockMultiplayer; i++)
            th.emplace_back([&, i] { load_map(paths[i], datas[i], parsed[i], errors[i]); });
        for (auto &t : th) t.join();
        for (size_t i = 0; i < kStockMultiplayer; i++) {
            if (!errors[i].empty()) { why = name_of(paths[i]) + ": " + errors[i]; return false; }
            for (auto &b : parsed[i].blocks)
                if (b.own && !put(b)) return false;
        }
    }
    // the levels: which blocks three levels or more use (each level's blocks counted once, by two independent hashes,
    // so the levels needn't all be in memory at once), then those placed, a level at a time
    struct Key {
        uint64_t a, b;
        bool operator==(const Key &o) const { return a == o.a && b == o.b; }
    };
    struct KeyHash { size_t operator()(const Key &k) const { return (size_t)(k.a ^ (k.b * 0x9E3779B97F4A7C15ull)); } };
    auto key_of = [](const Block &b) {
        return Key{hash_bytes(b.bytes, b.size) ^ ((uint64_t)b.rem << 52), hash_bytes(b.bytes, b.size, 0xD6E8FEB86659FD93ull) ^ b.size};
    };
    const size_t levels = paths.size() - kStockMultiplayer;
    std::vector<std::vector<Key>> keys(levels);
    std::vector<std::string> errors(levels);
    {
        std::atomic<size_t> next{0};
        std::vector<std::thread> th;
        const size_t workers = std::min<size_t>(levels, std::max(2u, std::min(4u, std::thread::hardware_concurrency())));
        for (size_t w = 0; w < workers; w++)
            th.emplace_back([&] {
                for (size_t i; (i = next++) < levels;) {
                    std::string d;
                    Parsed p;
                    if (!load_map(paths[kStockMultiplayer + i], d, p, errors[i])) continue;
                    for (auto &b : p.blocks)
                        if (b.own) keys[i].push_back(key_of(b));
                    std::sort(keys[i].begin(), keys[i].end(), [](const Key &x, const Key &y) { return x.a != y.a ? x.a < y.a : x.b < y.b; });
                    keys[i].erase(std::unique(keys[i].begin(), keys[i].end()), keys[i].end());
                }
            });
        for (auto &t : th) t.join();
    }
    std::unordered_map<Key, uint32_t, KeyHash> used;
    for (size_t i = 0; i < levels; i++) {
        if (!errors[i].empty()) { why = name_of(paths[kStockMultiplayer + i]) + ": " + errors[i]; return false; }
        for (auto &k : keys[i]) used[k]++;
        std::vector<Key>().swap(keys[i]);
    }
    for (size_t i = 0; i < levels; i++) {
        std::string d;
        Parsed p;
        if (!load_map(paths[kStockMultiplayer + i], d, p, why)) { why = name_of(paths[kStockMultiplayer + i]) + ": " + why; return false; }
        for (auto &b : p.blocks) {
            if (!b.own) continue;
            auto it = used.find(key_of(b));
            if (it != used.end() && it->second >= kStockLevelsBy && !put(b)) return false;
        }
    }
    data.append((kSector - data.size() % kSector) % kSector, '\0');
    shared_header(data, "stock");
    s.file.sum = get32(data, 0x64);
    s.file.length = (uint32_t)data.size();
    return true;
}
// stock.shared kept once laid out (its layout is fixed): <cache>\stock.shared as written for the Xbox, and beside it
// stock.shared.blocks, each block's offset and size, so the next run reads it back (a second or two) instead of laying it
// out from the 23 maps again (a minute, 3 GB read).
bool read_cached_stock(const fs::path &cache, StockLayout &s) {
    if (cache.empty()) return false;
    std::string blocks;
    std::string &data = s.file.data;
    data.reserve(kMaxShared + kSector);   // (the entries point into it: it never moves)
    if (!read_file(cache / "stock.shared.blocks", blocks) || !read_file(cache / "stock.shared", data)) { data.clear(); return false; }
    if (data.size() != kStockLength || data.compare(0, 4, "daeh") != 0 || get32(data, 8) != kStockLength || get32(data, 0x64) != kStockSum ||
        blocks.size() < 8 || blocks.compare(0, 4, "HMPS") != 0 || get32(blocks, 4) != (blocks.size() - 8) / 8 ||
        crc32_of(data.data() + kHeader, data.size() - kHeader) != kStockSum) {
        data.clear();
        return false;
    }
    const size_t n = (blocks.size() - 8) / 8;
    s.at.reserve(n);
    s.t.e.reserve(n);
    for (size_t i = 0; i < n; i++) {
        const uint32_t at = get32(blocks, 8 + i * 8), size = get32(blocks, 12 + i * 8);
        if (at < kHeader || (uint64_t)at + size > data.size() || !size) { data.clear(); s = StockLayout(); return false; }
        s.at.push_back(at);
        s.t.add_new(data.data() + at, size, at % kUnit);
    }
    s.file.sum = kStockSum;
    s.file.length = kStockLength;
    return true;
}
void write_cached_stock(const fs::path &cache, const StockLayout &s) {
    if (cache.empty()) return;
    std::error_code ec;
    fs::create_directories(cache, ec);
    std::string blocks = "HMPS";
    blocks.append(4, '\0');
    put32(blocks, 4, (uint32_t)s.at.size());
    for (size_t i = 0; i < s.at.size(); i++) {
        blocks.append(8, '\0');
        put32(blocks, 8 + i * 8, s.at[i]);
        put32(blocks, 12 + i * 8, s.t.e[i].size);
    }
    fs::path tmp = cache / "stock.shared.new";
    if (write_file(tmp, s.file.data) && write_file(cache / "stock.shared.blocks", blocks)) {
        fs::remove(cache / "stock.shared", ec);
        fs::rename(tmp, cache / "stock.shared", ec);
    }
    if (ec) fs::remove(cache / "stock.shared.blocks", ec);
}
}   // namespace

void set_tag_recipes(bool on) { g_recipes = on; }
bool tag_recipes() { return g_recipes; }

std::vector<fs::path> stock_maps_in(const fs::path &folder) {
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

Result make(const fs::path &folder, const std::string &base_name, const std::vector<fs::path> &paths, const fs::path &stock_maps,
            const fs::path &stock_folder, const fs::path &cache, const std::function<void(const std::string &)> &log) {
    Result r;
    const fs::path stock_dir = stock_folder.empty() ? folder : stock_folder;
    try {
        // the pack's file is named for its version: <pack>-<8 hex of its checksum>, 30 characters at most
        std::string base;
        for (unsigned char c : base_name) base += (c < 32 || c > 126 || strchr("\\/:*?\"<>|", c)) ? '_' : (char)c;
        while (!base.empty() && (base.back() == ' ' || base.back() == '.')) base.pop_back();
        if (base.size() > 21) base.resize(21);
        if (base.empty()) base = "pack";
        // ---- stock.shared, laid out first (maps built against the one before draw on its blocks where they still are)
        StockLayout stock;
        bool use_stock = false;
        if (!stock_maps.empty()) {
            std::error_code ec;
            if (fs::exists(cache / "stock.shared.blocks", ec)) log("Reading stock.shared laid out before...");
            if (read_cached_stock(cache, stock)) use_stock = true;
            else {
                const std::vector<fs::path> stock_paths = stock_maps_in(stock_maps);
                std::string why;
                if (stock_paths.empty())
                    log("  stock.shared isn't used: the Xbox maps folder needs the 13 stock multiplayer maps and the 10 campaign levels.");
                else {
                    log("Laying out stock.shared from the Xbox's 13 multiplayer maps and 10 campaign levels (once: it's kept)...");
                    if (!lay_out_stock(stock_paths, stock, why)) log("  stock.shared isn't used: " + why);
                    else if (stock.file.sum != kStockSum || stock.file.length != kStockLength) {
                        char t[256];
                        snprintf(t, sizeof t, "  stock.shared isn't used: from these maps it comes out %08x (%u bytes), not the installer's %08x (%u bytes): "
                                 "the Xbox maps folder's maps aren't the retail ones.", stock.file.sum, stock.file.length, kStockSum, kStockLength);
                        log(t);
                    } else {
                        use_stock = true;
                        write_cached_stock(cache, stock);
                    }
                }
            }
            if (!use_stock) stock = StockLayout();
        }
        // ---- a map's new copy an earlier run wrote but couldn't put in place (Windows held the map open), <map>.new:
        // put in place now when it's the newer one and the shared files it draws on are still there; else taken away
        // (only the maps themselves belong in the folder: a map library zips it as it is)
        std::vector<fs::path> promoted;
        for (auto &p : paths) {
            fs::path t = p;
            t += ".new";
            std::error_code ec;
            if (!fs::is_regular_file(t, ec)) continue;
            std::string head(kHeader, '\0');
            {
                std::ifstream f(t, std::ios::binary);
                if (!f || !f.read(&head[0], (std::streamsize)kHeader)) head.clear();
            }
            bool ok = head.size() == kHeader && head.compare(0, 4, "daeh") == 0 && head.compare(0x7FC, 4, "toof") == 0;
            if (ok && head.compare(kRef, 4, "drhs") == 0) {
                auto there = [&](const std::string &n) {
                    std::error_code e2;
                    return n.empty() || fs::exists(p.parent_path() / (n + ".shared"), e2) || fs::exists(p.parent_path().parent_path() / (n + ".shared"), e2) ||
                           (n == "stock" && (fs::exists(stock_dir / "stock.shared", e2) || (!stock_maps.empty() && fs::exists(stock_maps / "stock.shared", e2))));
                };
                ok = there(cstr(head, kRef + 8, 32)) && (get32(head, kRef + 4) < 2 || there(cstr(head, kRef + 0x38, 32)));
            }
            std::error_code e1, e2;
            const bool newer = !fs::exists(p, e1) || fs::last_write_time(t, e1) >= fs::last_write_time(p, e2);
            bool placed = false;
            if (ok && newer)
                for (int attempt = 0; attempt < 20 && !placed; attempt++) {
                    ec.clear();
                    fs::rename(t, p, ec);   // (over the map: Windows replaces it)
                    if (!ec) placed = true;
                    else std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
            if (placed) {
                promoted.push_back(p);
                log("  " + name_of(p) + ": put in place its new copy an earlier run left beside it (" + name_of(t) + ")");
            } else {
                fs::remove(t, ec);
                log("  " + name_of(p) + ": removed " + name_of(t) + ", an earlier run's copy " +
                    (!ok ? "that draws on shared files no longer here" : newer ? "that still couldn't be put in place" : "older than the map"));
            }
        }
        // ---- the maps, and the shared files they already draw on (one that can't be read is left as it is)
        log("Reading the maps...");
        std::vector<std::unique_ptr<Map>> maps;
        for (auto &p : paths) {
            maps.push_back(std::make_unique<Map>());
            maps.back()->path = p;
        }
        {
            // (a few at a time: each holds its map packed and unpacked while it reads, a level's 100 MB and 250 MB)
            std::atomic<size_t> next{0};
            auto read_one = [&](Map &m) {
                    std::string disk;
                    if (!read_file(m.path, disk)) { m.skipped = "it couldn't be read"; return; }
                    m.disk_size = disk.size();
                    m.ident_size = (uint32_t)disk.size();
                    m.ident_sum = get32(disk, 0x64);
                    const size_t edge = std::min(kEdge, disk.size());
                    m.ident_head = crc32_of(disk.data(), edge);
                    m.ident_tail = crc32_of(disk.data() + disk.size() - edge, edge);
                    std::string why;
                    if (!unpack(disk, m.data, m.compressed, why)) { m.data.clear(); m.skipped = why; }
            };
            const size_t at_once = std::clamp<size_t>(std::thread::hardware_concurrency(), 1, 4);
            std::vector<std::thread> th;
            for (size_t t = 0; t < std::min(at_once, maps.size()); t++)
                th.emplace_back([&] {
                    for (size_t i; (i = next++) < maps.size();) read_one(*maps[i]);
                });
            for (auto &t : th) t.join();
        }
        std::map<fs::path, std::unique_ptr<Source>> sources;
        auto source = [&](const Map &m, const std::string &n, uint32_t sum, uint32_t length, const Source *&out) -> bool {
            std::vector<fs::path> where{m.path.parent_path() / (n + ".shared"), m.path.parent_path().parent_path() / (n + ".shared")};
            if (n == "stock") {
                // (built against the stock.shared before this one: its blocks are where they were in this one)
                if (use_stock && ((sum == stock.file.sum && length == stock.file.length) || (sum == kOldStockSum && length == kOldStockLength))) {
                    out = &stock.file;
                    return true;
                }
                where.push_back(stock_dir / "stock.shared");
                if (!stock_maps.empty()) where.push_back(stock_maps / "stock.shared");
            }
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
            return false;
        };
        for (auto &mp : maps) {
            Map &m = *mp;
            if (!m.skipped.empty()) continue;
            const Source *pack_src = nullptr, *stock_src = nullptr;
            if (m.data.compare(kRef, 4, "drhs") == 0) {
                const uint32_t version = get32(m.data, kRef + 4);
                if (version < 1 || version > 4) { m.skipped = "it draws on shared files of a later format"; continue; }
                m.had_ref = true;
                m.ident_size = get32(m.data, kRef + 0x30);
                m.ident_sum = get32(m.data, kRef + 0x34);
                m.ident_head = get32(m.data, kRef + 0x60);   // (0 from an earlier format: not known)
                m.ident_tail = get32(m.data, kRef + 0x64);
                m.ref_name = cstr(m.data, kRef + 8, 32);
                const std::string sn = version >= 2 ? cstr(m.data, kRef + 0x38, 32) : "";
                if (!m.ref_name.empty() && !source(m, m.ref_name, get32(m.data, kRef + 0x28), get32(m.data, kRef + 0x2C), pack_src)) {
                    m.skipped = "it draws on " + m.ref_name + ".shared, and the one it was built with isn't beside it or in the folder above";
                    continue;
                }
                if (!sn.empty() && !source(m, sn, get32(m.data, kRef + 0x58), get32(m.data, kRef + 0x5C), stock_src)) {
                    m.skipped = "it draws on a stock.shared that isn't here";
                    continue;
                }
                // (format 4: its tag data rebuilt from its recipe first; the recipe's bytes zeroed, and cut off when
                // they're the file's end, as the map is made again)
                if ((version == 3 || version == 4) && get32(m.data, kRef + 0x6C)) {
                    std::string pwhy, why;
                    const Pool *pl = stock_pool(cache, stock_maps, log, pwhy);
                    if (!apply_recipe(m.data, pl, pack_src ? &pack_src->data : nullptr, false, why)) {
                        m.skipped = "its tag data can't be rebuilt from its recipe: " + why + (pl ? "" : " (" + pwhy + ")");
                        continue;
                    }
                    if (version == 3) {   // (version 4's is gone already: the map is its parts and its tag data)
                        const uint32_t ro = get32(m.data, kRef + 0x68), rs = get32(m.data, kRef + 0x6C);
                        memset(&m.data[ro], 0, rs);
                        m.recipe_cut = ro;
                    }
                }
            }
            auto resolve = [&](uint32_t v, uint32_t n) -> const char * {
                const Source *s = (v & kStock) == kStock ? stock_src : pack_src;
                const uint64_t o = v & kOffset;
                return s && o + n <= s->data.size() ? s->data.data() + o : nullptr;
            };
            std::string why;
            if (!parse(m.data, m.p, resolve, why)) m.skipped = why;
            else if (m.recipe_cut) {   // (nothing of it after its old recipe: the file ends there)
                uint64_t end = (uint64_t)m.p.td_off + m.p.td_size;
                for (size_t f : m.p.bsp_fields) end = std::max<uint64_t>(end, (uint64_t)get32(m.data, f) + get32(m.data, f + 4));
                for (auto &b : m.p.blocks)
                    if (b.own) end = std::max<uint64_t>(end, (uint64_t)b.value + b.size);
                if (m.recipe_cut >= end && m.recipe_cut < m.data.size()) {
                    m.data.resize(m.recipe_cut);
                    if (!parse(m.data, m.p, resolve, why)) m.skipped = why;
                }
            }
        }
        for (auto &m : maps)
            if (!m->skipped.empty()) {
                log("  " + shown(m->path) + " is left as it is: " + m->skipped + ".");
                r.left.push_back(shown(m->path) + ": " + m->skipped);
                std::string().swap(m->data);
            }
        // ---- what goes where: stock.shared's data there, data two or more maps use in the pack's file, the rest stays.
        // The maps in the order of their identities (size, then the CRC32s of their first and last 64 KB), whatever
        // their names: the same maps make the same files, whoever makes them.
        std::vector<size_t> order;
        for (size_t i = 0; i < maps.size(); i++)
            if (maps[i]->skipped.empty()) order.push_back(i);
        std::sort(order.begin(), order.end(), [&](size_t x, size_t y) {
            const Map &a = *maps[x], &b = *maps[y];
            return std::tie(a.ident_size, a.ident_head, a.ident_tail, x) < std::tie(b.ident_size, b.ident_head, b.ident_tail, y);
        });
        Table t;
        for (size_t i : order)
            for (auto &b : maps[i]->p.blocks) {
                if (!b.bytes) continue;
                b.entry = t.add(b.bytes, b.size, b.rem);
                Entry &e = t.e[(size_t)b.entry];
                if (e.last_map != (int)i) { e.last_map = (int)i; e.maps++; }
            }
        // The pack's file: its header, its list (16 bytes, 20 a map, 40 a block, to a whole sector), then its blocks.
        size_t blocks = 0;
        for (const Entry &e : t.e)
            if (e.maps >= 2 && !(use_stock && stock.t.find(e.bytes, e.size, e.rem) >= 0)) blocks++;
        bool has_pack = blocks > 0;
        const size_t list_bytes = 16 + 20 * order.size() + 40 * blocks;
        uint32_t list_room = has_pack ? (uint32_t)((list_bytes + kSector - 1) / kSector * kSector) : 0;
        Source pack;
        pack.data.assign(kHeader + list_room, '\0');
        std::vector<Plan> plans(maps.size());
        std::vector<uint32_t> pack_blocks;   // (each of the pack's blocks' entry, in the order they're in its file)
        bool moves = false;
        for (size_t i : order) {
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
                    m.to_stock += b.size;
                } else if (e.maps >= 2) {
                    if (!e.in_pack) {
                        e.at = place(pack.data, b.bytes, b.size, b.rem);
                        e.in_pack = true;
                        pack_blocks.push_back((uint32_t)b.entry);
                    }
                    plan.go[j] = 1;
                    plan.at[j] = e.at;
                    m.to_pack += b.size;
                }
            }
            moves = moves || m.to_pack || m.to_stock || m.had_ref;
        }
        // ---- format 4: tag data stored once (the stock tag pool, and the pack's tag part), when asked for
        const Pool *pool = nullptr;
        if (g_recipes && !order.empty()) {
            std::string why;
            pool = stock_pool(cache, stock_maps, log, why);
            if (!pool) log("  Tag data isn't stored once: " + why + ".");
            else moves = true;
        }
        // Once anything moves, every map is made again, naming the pack's file and stock.shared (as the installer's
        // pass makes them): each one is in the pack's list.
        bool stock_used = false;
        for (size_t i : order) {
            Map &m = *maps[i];
            m.rewrite = moves;
            m.pack_ref = moves && has_pack;
            m.stock_ref = moves && use_stock;
            stock_used = stock_used || m.stock_ref;
        }
        std::string name;
        uint32_t tag_off = 0, tag_size = 0;   // (the pack's tag part)
        auto finish_pack = [&]() -> bool {   // (its header: its name is its version)
            pack.data.append((kSector - pack.data.size() % kSector) % kSector, '\0');
            if (pack.data.size() > kMaxShared) {
                r.error = "The pack's shared file would be " + mb(pack.data.size()) + " unpacked: a shared file can be at most 278 MB. Nothing was written.";
                return false;
            }
            shared_header(pack.data, "", list_room, tag_off, tag_size);
            char hex[16];
            snprintf(hex, sizeof hex, "-%08x", get32(pack.data, 0x64));
            name = base + hex;
            shared_header(pack.data, name, list_room, tag_off, tag_size);   // (the name isn't in the checksum: the header isn't)
            pack.sum = get32(pack.data, 0x64);
            pack.length = (uint32_t)pack.data.size();
            return true;
        };
        if (!pool && has_pack && !finish_pack()) return r;
        bool any = false;
        for (auto &m : maps) any = any || m->rewrite;
        // ---- each map laid out again, packed and read back (at once)
        if (any) log("Laying the maps out again without what moved, packing and checking them...");
        auto each = [&](const std::function<void(Map &, size_t)> &f) {
            std::vector<std::thread> th;
            for (size_t i = 0; i < maps.size(); i++)
                if (maps[i]->rewrite) th.emplace_back([&, i] { f(*maps[i], i); });
            for (auto &x : th) x.join();
        };
        if (!pool)
            each([&](Map &m, size_t i) {
                if (!relayout(m, plans[i], name, pack, stock.file)) return;
                m.file = pack_map(m.out, m.compressed);
                check(m, pack, stock.file);
            });
        else {
            // (the pack's file is finished once its tag part is known: each map's reference written again then)
            each([&](Map &m, size_t i) {
                std::string why;
                if (relayout(m, plans[i], "", pack, stock.file) && !tag_data_last(m.out, why)) m.error = "laid out again, " + why;
            });
            for (auto &m : maps)
                if (!m->error.empty()) { r.error = shown(m->path) + ": " + m->error + ". Nothing was written."; return r; }
            log("Storing the tag data once: each map's against the stock tag pool and the pack's other maps...");
            std::vector<Map *> ms;
            for (size_t i : order)
                if (maps[i]->rewrite) ms.push_back(maps[i].get());
            std::vector<uint32_t> tag_part;
            tag_pass(ms, *pool, tag_part);
            if (!tag_part.empty()) {   // (after the pack's last block, at a sector)
                if (!has_pack) {
                    list_room = (uint32_t)((16 + 20 * order.size() + kSector - 1) / kSector * kSector);
                    pack.data.assign(kHeader + list_room, '\0');
                    has_pack = true;
                }
                pack.data.append((kSector - pack.data.size() % kSector) % kSector, '\0');
                tag_off = (uint32_t)pack.data.size();
                tag_size = (uint32_t)(tag_part.size() * 4);
                pack.data.append((const char *)tag_part.data(), tag_size);
            }
            if (has_pack && !finish_pack()) return r;
            for (size_t i : order) {
                Map &m = *maps[i];
                if (!m.rewrite) continue;
                m.pack_ref = has_pack;
                uint32_t at = 0;
                if (!m.recipe.empty()) {   // (version 4: the file stops at its tag data, its recipe at the next sector)
                    m.out.resize(get32(m.out, 0x10));
                    m.out.append((kSector - m.out.size() % kSector) % kSector, '\0');
                    at = (uint32_t)m.out.size();
                    m.out += m.recipe;
                    m.out.append((kSector - m.out.size() % kSector) % kSector, '\0');
                    put32(m.out, 8, (uint32_t)m.out.size());
                }
                write_ref(m, name, pack, stock.file, at);
            }
            each([&](Map &m, size_t) {
                m.file = pack_map(m.out, m.compressed);
                check(m, pack, stock.file, pool);
            });
        }
        for (auto &m : maps)
            if (!m->error.empty()) { r.error = shown(m->path) + ": " + m->error + ". Nothing was written."; return r; }
        // The pack's list: "OXPL", version 1, its maps, its blocks; then each map (in their order): its identity, its small
        // map's length and CRC32 unpacked; then each block (in file order): its SHA-256, offset and size.
        if (has_pack) {
            std::string list;
            auto add32 = [&](uint32_t v) { list.append(4, '\0'); put32(list, list.size() - 4, v); };
            list = "OXPL";
            add32(1);
            add32((uint32_t)order.size());
            add32((uint32_t)pack_blocks.size());
            for (size_t i : order) {
                const Map &m = *maps[i];
                add32(m.ident_size);
                add32(m.ident_head);
                add32(m.ident_tail);
                add32((uint32_t)m.out.size());
                add32(crc32_of(m.out.data(), m.out.size()));
            }
            for (uint32_t k : pack_blocks) {
                const Entry &e = t.e[k];
                unsigned char h[32];
                sha256(pack.data.data() + e.at, e.size, h);
                list.append((const char *)h, 32);
                add32(e.at);
                add32(e.size);
            }
            if (list.size() > list_room) { r.error = "The pack's list didn't fit its room (a bug). Nothing was written."; return r; }
            pack.data.replace(kHeader, list.size(), list);
        }
        // The pack's file stored unpacked (read where it is) when a map drawing on it is over 47 MB unpacked: a packed one
        // takes one of the two big cache files, and a level change needs both. stock.shared always is.
        bool pack_unpacked = false;
        for (auto &m : maps)
            if (m->pack_ref && m->out.size() > kMultiplayerCache) pack_unpacked = true;
        std::string pack_file;
        if (has_pack) {
            pack_file = pack_unpacked ? pack.data : pack_shared(pack.data);
            std::string back, why;
            bool c;
            if (!unpack(pack_file, back, c, why) || back != pack.data) {
                r.error = "The pack's shared file didn't pack right (unpacked, it isn't what was packed). Nothing was written.";
                return r;
            }
        }
        // ---- written: each file beside the one it replaces, then put in its place
        struct Out { fs::path path; const std::string *file; };
        std::vector<Out> outs;
        auto same = [&](const fs::path &p, const std::string &file) {   // (that very file already there, every byte: the list
            std::error_code ec;                                            // is outside the checksum, so the name can match alone)
            if (fs::file_size(p, ec) != file.size() || ec) return false;
            std::ifstream f(p, std::ios::binary);
            std::string h(1 << 20, '\0');
            for (size_t at = 0; at < file.size();) {
                const size_t n = std::min(h.size(), file.size() - at);
                if (!f.read(&h[0], (std::streamsize)n) || h.compare(0, n, file, at, n) != 0) return false;
                at += n;
            }
            return true;
        };
        auto temp = [](const fs::path &p) { fs::path t = p; t += ".new"; return t; };
        const fs::path pack_path = folder / (name + ".shared"), stock_path = stock_dir / "stock.shared";
        if (has_pack && !same(pack_path, pack_file)) outs.push_back({pack_path, &pack_file});
        if (stock_used && !same(stock_path, stock.file.data)) outs.push_back({stock_path, &stock.file.data});
        for (auto &m : maps)
            if (m->rewrite) outs.push_back({m->path, &m->file});
        if (!outs.empty()) log("Writing...");
        for (auto &o : outs)
            if (!write_file(temp(o.path), *o.file)) {
                std::error_code ec;
                for (auto &x : outs) fs::remove(temp(x.path), ec);
                r.error = "Couldn't write " + temp(o.path).u8string() + " (is the disk full?). Nothing was changed.";
                return r;
            }
        // (a file Windows holds open for a moment, a virus scan's or the search indexer's, is tried again for a few
        // seconds; one still held keeps its old version, and the others go on: each map as it was or as it is now,
        // never one drawing on a shared file that moved under it)
        std::set<fs::path> stuck;
        for (auto &o : outs) {
            std::error_code ec;
            for (int attempt = 0; attempt < 20; attempt++) {
                ec.clear();
                fs::remove(o.path, ec);
                ec.clear();
                fs::rename(temp(o.path), o.path, ec);
                if (!ec) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            if (ec) {
                stuck.insert(o.path);
                r.error += (r.error.empty() ? "" : " ") + std::string("Couldn't put ") + o.path.u8string() + " in place (" + ec.message() +
                           ", tried for 10 seconds: is it open in another program?): it's kept as it was.";
                std::error_code e2;
                fs::remove(temp(o.path), e2);   // (only the maps belong in the folder: no copy left beside one)
            }
        }
        if (!r.error.empty() && (stuck.count(pack_path) || stuck.count(stock_path))) return r;   // (the maps need them)
        // ---- a map left as it is that isn't whole 512-byte sectors (made before the app padded them): padded with
        // zeros after its zlib stream, as Halo 1 Unlimited copies a folder map into the Xbox's cache by sectors
        std::set<fs::path> padded;
        for (auto &m : maps) {
            if (m->rewrite && !stuck.count(m->path)) continue;
            std::error_code ec;
            const uintmax_t size = fs::file_size(m->path, ec);
            if (ec || size % 512 == 0 || size < kHeader) continue;
            std::string head(kHeader, '\0');
            {
                std::ifstream f(m->path, std::ios::binary);
                if (!f || !f.read(&head[0], (std::streamsize)kHeader) || head.compare(0, 4, "daeh") != 0) continue;
            }
            std::ofstream f(m->path, std::ios::binary | std::ios::app);
            if (f && f.write(std::string(512 - size % 512, '\0').data(), (std::streamsize)(512 - size % 512)) && f.flush()) {
                padded.insert(m->path);
                m->disk_size = (size_t)(size + 512 - size % 512);
            }
        }
        if (!padded.empty()) {
            std::string list;
            for (auto &p : padded) list += (list.empty() ? "" : ", ") + name_of(p);
            log("  padded to whole 512-byte sectors (the Xbox reads a folder map by them): " + list);
        }
        // ---- the link files beside a map rewritten (or put in place, or padded): its xbox line (size and hash) and
        // Xbox tag IDs are for the file as it is now
        std::string relinked;
        for (auto &m : maps) {
            const bool changed = (m->rewrite && !stuck.count(m->path)) || padded.count(m->path) ||
                                 std::find(promoted.begin(), promoted.end(), m->path) != promoted.end();
            if (!changed) continue;
            std::string why;
            const int n = h1ulink::refresh_xbox(m->path, why);
            if (n) relinked += (relinked.empty() ? "" : ", ") + name_of(m->path);
            if (!why.empty()) log("Link file not updated for " + name_of(m->path) + ": " + why);
        }
        // ---- shared files in its folder that no map draws on now: the pack's earlier versions (<pack>-<8 hex>.shared, or
        // <pack>.shared), and ones these maps drew on before (another pack's, when they've come into this one). What
        // every map in the folder and its folders draws on stays (one that isn't one of these too).
        std::string removed;
        {
            std::set<std::string> needed;
            for (auto &m : maps)
                if ((!m->skipped.empty() || stuck.count(m->path)) && !m->ref_name.empty()) needed.insert(m->ref_name + ".shared");
            if (has_pack) needed.insert(name + ".shared");
            {
                std::error_code e3;
                auto look = [&](const fs::path &p) {
                    std::string head(kHeader, '\0');
                    std::ifstream f(p, std::ios::binary);
                    if (!f || !f.read(&head[0], (std::streamsize)kHeader) || head.compare(kRef, 4, "drhs") != 0) return;
                    const std::string n = cstr(head, kRef + 8, 32);
                    if (!n.empty()) needed.insert(n + ".shared");
                };
                for (auto &e : fs::directory_iterator(folder, e3)) {
                    if (e.is_regular_file(e3) && e.path().extension() == ".map") look(e.path());
                    else if (e.is_directory(e3))
                        for (auto &f : fs::directory_iterator(e.path(), e3))
                            if (f.is_regular_file(e3) && f.path().extension() == ".map") look(f.path());
                }
            }
            std::set<std::string> former;   // (the pack files in this folder these maps drew on before)
            for (auto &src : sources) {
                std::error_code e4;
                const std::string f = src.first.filename().u8string();
                const fs::path dir = src.first.parent_path().empty() ? fs::path(".") : src.first.parent_path();
                if (f != "stock.shared" && fs::equivalent(dir, folder.empty() ? fs::path(".") : folder, e4)) former.insert(f);
            }
            auto earlier = [&](const std::string &f) {
                if (f == base + ".shared") return true;
                if (f.size() != base.size() + 16 || f.compare(0, base.size() + 1, base + "-") != 0 || f.compare(base.size() + 9, 7, ".shared") != 0) return false;
                for (size_t k = base.size() + 1; k < base.size() + 9; k++)
                    if (!isxdigit((unsigned char)f[k]) || isupper((unsigned char)f[k])) return false;
                return true;
            };
            std::error_code ec;
            std::vector<fs::path> gone;
            for (auto &e : fs::directory_iterator(folder, ec)) {
                const std::string f = e.path().filename().u8string();
                if ((earlier(f) || former.count(f)) && !needed.count(f) && e.is_regular_file(ec)) gone.push_back(e.path());
            }
            for (auto &g : gone) {
                fs::remove(g, ec);
                if (!ec) removed += (removed.empty() ? "" : ", ") + name_of(g);
            }
        }
        // ---- the report
        uint64_t before = 0, after = 0;
        for (auto &s : sources)
            if (s.first.filename() != "stock.shared") before += s.second->disk;   // (the pack's file they drew on before)
        std::string &rep = r.report;
        auto too_big = [](const Map &m, size_t unpacked) {   // (a multiplayer map goes in a multiplayer cache file)
            return (get32(m.data, 0x60) & 0xFFFF) == 1 && unpacked > kMultiplayerCache
                       ? "; still over the 47 MB a multiplayer cache file holds" : "";
        };
        for (auto &m : maps) {
            before += m->disk_size;
            if (!m->skipped.empty()) {
                after += m->disk_size;
                rep += shown(m->path) + ": left as it is: " + m->skipped + "\n";
            } else if (!m->rewrite) {
                after += m->disk_size;
                rep += shown(m->path) + ": " + mb(m->disk_size) + ", left as it is (nothing in it is in another map" +
                       (use_stock ? " or in stock.shared" : "") + too_big(*m, m->data.size()) + ")\n";
            } else {
                after += m->file.size();
                rep += shown(m->path) + ": " + mb(m->disk_size) + " -> " + mb(m->file.size()) + " (" + mb(m->out.size()) + " unpacked";
                if (m->to_pack) rep += "; " + mb(m->to_pack) + " of its textures and sounds in " + name + ".shared";
                if (m->to_stock) rep += "; " + mb(m->to_stock) + " in stock.shared";
                if (!m->recipe.empty())
                    rep += "; of its tag data's " + mb(get32(m->out, 0x14)) + ", " + mb(m->from_pool * 4) + " from the stock tag pool" +
                           (m->from_pack ? " and " + mb(m->from_pack * 4) + " from the pack's tag part" : "") + ", " + mb(m->own_words * 4) +
                           " its own (a recipe of " + std::to_string(m->recipe_runs) + " runs, " + mb(m->recipe.size()) + ", in place of its tag data)";
                rep += std::string(too_big(*m, m->out.size())) + ")\n";
            }
        }
        if (has_pack) {
            after += pack_file.size();
            rep += name + ".shared: " + mb(pack_file.size()) + (pack_unpacked ? " stored unpacked, so the game reads it where it is (a map "
                   "drawing on it is over 47 MB unpacked)" : " (" + mb(pack.data.size()) + " unpacked)") +
                   (tag_size ? "; its tag part (tag data two or more of the maps have, not in the stock tag pool) " + mb(tag_size) : "") +
                   ", in " + folder.u8string() + "\n";
            r.pack_file = name + ".shared";
        }
        if (stock_used)
            rep += "stock.shared: " + mb(stock.file.data.size()) + ", in " + stock_dir.u8string() + ": the same file Halo 1 Unlimited's "
                   "installer builds on every Xbox (checksum a7fe2f70), so it isn't counted or sent\n";
        if (!removed.empty()) rep += "Removed the shared file" + std::string(removed.find(',') != std::string::npos ? "s" : "") +
                                     " (no map draws on " + (removed.find(',') != std::string::npos ? "them" : "it") + " now): " + removed + "\n";
        for (auto &p : promoted) rep += shown(p) + ": its new copy an earlier run left beside it (" + name_of(p) + ".new) put in place\n";
        for (auto &p : padded) rep += shown(p) + ": padded to whole 512-byte sectors (the Xbox reads a folder map by them)\n";
        if (!relinked.empty()) rep += "Link files updated for the maps written (their Xbox size and hash): " + relinked + "\n";
        r.used_stock = stock_used;
        if (!stuck.empty()) rep += r.error + " Those stay as they were (whole maps, not drawing on the shared files), with their link files.\n";
        if (!any) rep += "Nothing in these maps is in two of them" + std::string(use_stock ? " or in stock.shared" : "") + ".\n";
        else rep += "In all: " + mb(before) + " -> " + mb(after) + ". Every texture, sound, BSP and the tag data were read back through the new files" +
                    std::string(pool ? " (the tag data rebuilt from each map's recipe, as the game does)" : "") + " and match.\n";
        if (pool) rep += "The maps need stocktags.shared on the Xbox (Halo 1 Unlimited's installer builds it from the retail maps; format 4).\n";
        r.ok = true;
    } catch (const std::exception &e) {
        r.error = std::string("Making the shared files failed: ") + e.what() + " (not enough memory?). Nothing was written.";
    }
    return r;
}
}   // namespace sharedfile
