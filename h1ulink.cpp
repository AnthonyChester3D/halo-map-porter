// Halo Map Porter - the link file Halo 1 Unlimited reads for a linked Custom Edition / Xbox pair (h1ulink.hpp).
#include "h1ulink.hpp"
#include "inflate.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>

namespace h1ulink {
namespace fs = std::filesystem;

// ---- BLAKE2b (RFC 7693), 32-byte digest, no key

namespace {
const uint64_t kIV[8] = {0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
                         0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};
const uint8_t kSigma[12][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4}, {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13}, {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11}, {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5}, {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3}};

uint64_t rotr(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }
uint64_t le64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
    return v;
}

void compress(uint64_t h[8], const uint8_t block[128], uint64_t t, bool last) {
    uint64_t v[16], m[16];
    for (int i = 0; i < 16; i++) m[i] = le64(block + 8 * i);
    for (int i = 0; i < 8; i++) { v[i] = h[i]; v[i + 8] = kIV[i]; }
    v[12] ^= t;   // (the counter's high word stays 0: no map is 2^64 bytes)
    if (last) v[14] = ~v[14];
    auto G = [&](int a, int b, int c, int d, uint64_t x, uint64_t y) {
        v[a] = v[a] + v[b] + x; v[d] = rotr(v[d] ^ v[a], 32);
        v[c] = v[c] + v[d];     v[b] = rotr(v[b] ^ v[c], 24);
        v[a] = v[a] + v[b] + y; v[d] = rotr(v[d] ^ v[a], 16);
        v[c] = v[c] + v[d];     v[b] = rotr(v[b] ^ v[c], 63);
    };
    for (int r = 0; r < 12; r++) {
        const uint8_t *s = kSigma[r];
        G(0, 4, 8, 12, m[s[0]], m[s[1]]);
        G(1, 5, 9, 13, m[s[2]], m[s[3]]);
        G(2, 6, 10, 14, m[s[4]], m[s[5]]);
        G(3, 7, 11, 15, m[s[6]], m[s[7]]);
        G(0, 5, 10, 15, m[s[8]], m[s[9]]);
        G(1, 6, 11, 12, m[s[10]], m[s[11]]);
        G(2, 7, 8, 13, m[s[12]], m[s[13]]);
        G(3, 4, 9, 14, m[s[14]], m[s[15]]);
    }
    for (int i = 0; i < 8; i++) h[i] ^= v[i] ^ v[i + 8];
}

// (a gbxmodel is a model on the Xbox, and a shader may be another shader class there: matched as one group)
std::string key_group(const std::string &g) {
    static const char *shaders[] = {"shdr", "senv", "soso", "sotr", "schi", "scex", "swat", "sgla", "smet", "spla"};
    for (auto sh : shaders)
        if (g == sh) return "shdr";
    return g == "mod2" ? std::string("mode") : g;
}

std::string hex(const uint8_t *d, size_t n) {
    static const char *x = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) { s += x[d[i] >> 4]; s += x[d[i] & 15]; }
    return s;
}
}   // namespace

void blake2b_256(const void *data, size_t size, uint8_t out[32]) {
    uint64_t h[8];
    for (int i = 0; i < 8; i++) h[i] = kIV[i];
    h[0] ^= 0x01010000ULL ^ 32;   // (digest length 32, no key, fanout and depth 1)
    const uint8_t *p = (const uint8_t *)data;
    uint8_t block[128];
    uint64_t t = 0;
    while (size > 128) {
        t += 128;
        compress(h, p, t, false);
        p += 128;
        size -= 128;
    }
    memset(block, 0, sizeof block);
    if (size) memcpy(block, p, size);
    t += size;
    compress(h, block, t, true);
    for (int i = 0; i < 32; i++) out[i] = (uint8_t)(h[i / 8] >> (8 * (i % 8)));
}

std::string map_hash(const fs::path &map, uint64_t *size_out) {
    std::ifstream f(map, std::ios::binary);
    if (!f) return "";
    std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (size_out) *size_out = all.size();
    const size_t piece = 262144;
    std::string top = "OpenCE map 1";
    uint32_t n = (uint32_t)all.size();
    for (int i = 0; i < 4; i++) top += (char)(n >> (8 * i) & 255);
    uint8_t d[32];
    for (size_t at = 0; at < all.size(); at += piece) {
        blake2b_256(all.data() + at, std::min(piece, all.size() - at), d);
        top.append((const char *)d, 32);
    }
    blake2b_256(top.data(), top.size(), d);
    return hex(d, 32);
}

bool read_tags(const fs::path &map, std::vector<TagEntry> &tags, std::string &scenario) {
    std::ifstream f(map, std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto u32 = [](const std::string &b, size_t o) -> uint32_t {
        if (o + 4 > b.size()) return 0;
        const unsigned char *p = (const unsigned char *)b.data() + o;
        return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
    };
    if (raw.size() < 0x800 || raw.compare(0, 4, "daeh") != 0) return false;
    uint32_t version = u32(raw, 4), file_size = u32(raw, 8), td_off = u32(raw, 0x10), td_size = u32(raw, 0x14);
    std::string body;
    if (version == 5 && raw.size() < file_size) {   // (an Xbox map, compressed)
        std::string rest;
        if (!inflate::zlib((const uint8_t *)raw.data() + 0x800, raw.size() - 0x800, rest, file_size - 0x800)) return false;
        body = raw.substr(0, 0x800) + rest;
    } else {
        body.swap(raw);
    }
    const uint32_t base = version == 5 ? 0x803A6000u : version == 13 ? 0x50000000u : 0x40440000u;   // (Xbox, MCC, Custom Edition)
    if (td_size < 16 || (size_t)td_off + td_size > body.size()) return false;
    std::string td = body.substr(td_off, td_size);
    auto at = [&](uint32_t addr) -> size_t { return addr >= base && addr < base + td_size ? addr - base : (size_t)-1; };
    size_t index = at(u32(td, 0));
    uint32_t scenario_id = u32(td, 4), count = u32(td, 12);
    if (index == (size_t)-1 || count > 65535) return false;
    for (uint32_t i = 0; i < count; i++) {
        size_t e = index + (size_t)i * 32;
        if (e + 32 > td.size()) return false;
        TagEntry t;
        for (int k = 3; k >= 0; k--) t.group += td[e + (size_t)k];
        t.id = u32(td, e + 12);
        size_t p = at(u32(td, e + 16));
        if (p != (size_t)-1) {
            size_t z = td.find('\0', p);
            t.path = td.substr(p, z == std::string::npos ? std::string::npos : z - p);
        }
        if (t.id == scenario_id) scenario = t.path;
        tags.push_back(t);
    }
    return true;
}

std::string link_text(const Link &l, std::string &why) {
    std::vector<TagEntry> ce, xb;
    std::string ce_scen, xb_scen;
    if (!read_tags(l.ce_map, ce, ce_scen)) { why = l.mcc ? "couldn't read the MCC map's tags" : "couldn't read the Custom Edition map's tags"; return ""; }
    if (!read_tags(l.xbox_map, xb, xb_scen)) { why = "couldn't read the Xbox map's tags"; return ""; }
    const fs::path &pc = l.pc_file.empty() ? l.ce_map : l.pc_file;
    uint64_t ce_size = 0, xb_size = 0;
    std::string ce_hash = l.mcc ? "-" : map_hash(pc, &ce_size), xb_hash = map_hash(l.xbox_map, &xb_size);
    if (ce_hash.empty() || xb_hash.empty()) { why = "couldn't read a map to hash it"; return ""; }
    std::string sha, sum;
    if (l.mcc && !mcc_ids(pc, l.resources, sha, sum, why)) return "";
    const char *pc_kind = l.mcc ? "mcc " : "ce  ";
    std::string s = "h1u-link 1\ntool " + l.tool + "\n";
    if (l.mcc) {
        s += "# the two files: the name each game knows the map by; the MCC map's content SHA-256 (with the resource files it\n"
             "# reads), metadata SHA-256 (-) and checksum; the Xbox map's size in bytes and hash\n";
        s += "mcc  " + l.ce_name + "   " + sha + "   -   " + sum + "\n";
    } else {
        s += "# the two files: the name each game knows the map by, its size in bytes and its hash\n";
        s += "ce   " + l.ce_name + "   " + std::to_string(ce_size) + "   " + ce_hash + "\n";
    }
    s += "xbox " + l.xbox_name + "   " + std::to_string(xb_size) + "   " + xb_hash + "\n";
    s += "# the scenario tag's path in each\nlevel " + std::string(pc_kind) + " " + ce_scen + "\nlevel xbox " + xb_scen + "\n";
    s += "# placed objects, per palette type: the count (the same in both)\n";
    for (auto &[k, n] : l.placed) s += "placed " + k + " " + std::to_string(n) + "\n";
    s += "# every tag in either map: group, full tag ID in the " + std::string(l.mcc ? "MCC" : "CE") + " map, in the Xbox map, path\n"
         "# (a tag missing from one side: - there)\n";
    // In the CE map's order, then the Xbox's own (matched by group and path).
    std::map<std::pair<std::string, std::string>, uint32_t> xb_ids;
    for (auto &t : xb) xb_ids[{key_group(t.group), t.path}] = t.id;
    std::map<std::pair<std::string, std::string>, bool> listed;
    char b[32];
    for (auto &t : ce) {
        std::string xg = key_group(t.group);
        auto it = xb_ids.find({xg, t.path});
        snprintf(b, sizeof b, "%08x", t.id);
        s += "tag " + t.group + " " + b + " ";
        if (it != xb_ids.end()) {
            snprintf(b, sizeof b, "%08x", it->second);
            s += b;
            listed[{xg, t.path}] = true;
        } else {
            s += "-";
        }
        s += " " + t.path + "\n";
    }
    for (auto &t : xb) {
        if (listed.count({key_group(t.group), t.path})) continue;
        snprintf(b, sizeof b, "%08x", t.id);
        s += "tag " + t.group + " - " + b + " " + t.path + "\n";
    }
    s += "# the checks that passed (one line each; free text after the name)\n";
    for (auto &c : l.checks) s += "check " + c + "\n";
    return s;
}

// ---- SHA-256 (FIPS 180-4) and crc32 (zlib's), streamed

namespace {
struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf[64];
    size_t used = 0;
    uint64_t total = 0;
    static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const uint8_t *p) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
            0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
            0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void add(const void *data, size_t n) {
        const uint8_t *p = (const uint8_t *)data;
        total += n;
        while (n) {
            if (used == 0 && n >= 64) { block(p); p += 64; n -= 64; continue; }
            size_t k = std::min(n, 64 - used);
            memcpy(buf + used, p, k);
            used += k; p += k; n -= k;
            if (used == 64) { block(buf); used = 0; }
        }
    }
    std::string done() {
        const uint64_t bits = total * 8;
        uint8_t pad = 0x80;
        add(&pad, 1);
        pad = 0;
        while (used != 56) add(&pad, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
        add(len, 8);
        uint8_t out[32];
        for (int i = 0; i < 32; i++) out[i] = (uint8_t)(h[i / 4] >> (24 - 8 * (i % 4)));
        return hex(out, 32);
    }
};

// zlib's crc32(crc, data): `crc` the value so far (0 to start)
uint32_t crc32_add(uint32_t crc, const void *data, size_t n) {
    static uint32_t table[256];
    static bool init = [] {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        return true;
    }();
    (void)init;
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = ~crc;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

// Each piece of a file, in turn (false if it can't be read); `size` its size.
template <class F> bool each_piece(const fs::path &file, uint64_t &size, F f) {
    std::error_code ec;
    size = fs::file_size(file, ec);
    if (ec) return false;
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::vector<char> b(1 << 20);
    uint64_t read = 0;
    while (read < size) {
        in.read(b.data(), (std::streamsize)std::min<uint64_t>(b.size(), size - read));
        const std::streamsize got = in.gcount();
        if (got <= 0) return false;
        f(b.data(), (size_t)got);
        read += (uint64_t)got;
    }
    return true;
}
}   // namespace

std::string sha256_hex(const std::string &bytes) {
    Sha256 s;
    s.add(bytes.data(), bytes.size());
    return s.done();
}

uint32_t map_version(const fs::path &map) {
    std::ifstream f(map, std::ios::binary);
    unsigned char h[8] = {};
    if (!f.read((char *)h, 8) || memcmp(h, "daeh", 4) != 0) return 0;
    return h[4] | h[5] << 8 | h[6] << 16 | (uint32_t)h[7] << 24;
}

bool mcc_ids(const fs::path &map, const std::vector<Resource> &resources, std::string &sha, std::string &checksum, std::string &why) {
    uint32_t crc = 0;
    {
        std::ifstream f(map, std::ios::binary);
        unsigned char h[0x68] = {};
        if (!f.read((char *)h, sizeof h)) { why = "couldn't read the MCC map's header"; return false; }
        crc = h[0x64] | h[0x65] << 8 | h[0x66] << 16 | (uint32_t)h[0x67] << 24;
    }
    Sha256 s;
    s.add("GulchCE MCC content v1", 22);
    std::vector<Resource> files{{0, map}};
    for (auto &r : resources) files.push_back(r);
    for (auto &r : files) {
        std::error_code ec;
        const uint64_t size = fs::file_size(r.file, ec);
        if (ec) { why = "couldn't read " + r.file.u8string(); return false; }
        uint8_t frame[9] = {r.type};
        for (int i = 0; i < 8; i++) frame[1 + i] = (uint8_t)(size >> (8 * i));
        s.add(frame, 9);
        if (r.type) crc = crc32_add(crc, &r.type, 1);
        uint64_t got = 0;
        const bool resource = r.type != 0;
        if (!each_piece(r.file, got, [&](const char *d, size_t n) {
                s.add(d, n);
                if (resource) crc = crc32_add(crc, d, n);
            }) || got != size) {
            why = "couldn't read " + r.file.u8string();
            return false;
        }
        if (resource) {
            const uint8_t end = 0xFF;
            crc = crc32_add(crc, &end, 1);
        }
    }
    sha = s.done();
    char b[16];
    snprintf(b, sizeof b, "%08x", crc ? crc : 1u);
    checksum = b;
    return true;
}

int refresh_xbox(const fs::path &map, std::string &why) {
    std::error_code ec;
    const fs::path dir = map.parent_path();
    auto lower = [](std::string x) { for (auto &c : x) c = (char)tolower((unsigned char)c); return x; };
    const std::string file = lower(map.filename().u8string());
    std::vector<TagEntry> xb;
    std::string xb_scen, xb_hash;
    uint64_t xb_size = 0;
    bool read = false;
    int n = 0;
    for (auto &e : fs::directory_iterator(dir, ec)) {
        if (lower(e.path().extension().u8string()) != ".h1ulink" || !e.is_regular_file(ec)) continue;
        std::ifstream in(e.path(), std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        std::vector<std::string> lines;
        {
            std::istringstream ls(text);
            for (std::string l; std::getline(ls, l);) {
                if (!l.empty() && l.back() == '\r') l.pop_back();
                lines.push_back(l);
            }
        }
        // (the link for this map: its xbox line names this file, in its folder or not)
        bool ours = false;
        for (auto &l : lines)
            if (l.rfind("xbox ", 0) == 0) {
                std::istringstream ws(l.substr(5));
                std::string name;
                ws >> name;
                const size_t slash = name.find_last_of("\\/");
                ours = lower(slash == std::string::npos ? name : name.substr(slash + 1)) == file;
            }
        if (!ours) continue;
        if (!read) {
            if (!read_tags(map, xb, xb_scen) || (xb_hash = map_hash(map, &xb_size)).empty()) {
                why = "couldn't read " + map.u8string();
                return n;
            }
            read = true;
        }
        std::map<std::pair<std::string, std::string>, uint32_t> ids;
        for (auto &t : xb) ids[{key_group(t.group), t.path}] = t.id;
        std::map<std::pair<std::string, std::string>, bool> listed;
        std::string out;
        char b[16];
        // (the Xbox's own tags go where they were, else before the checks)
        size_t xb_only_at = std::string::npos, checks_at = std::string::npos;
        for (auto &l : lines) {
            if (l.rfind("xbox ", 0) == 0) {
                std::istringstream ws(l.substr(5));
                std::string name;
                ws >> name;
                out += "xbox " + name + "   " + std::to_string(xb_size) + "   " + xb_hash + "\n";
            } else if (l.rfind("level xbox ", 0) == 0) {
                out += "level xbox " + xb_scen + "\n";
            } else if (l.rfind("tag ", 0) == 0) {
                // tag <group: 4 characters, "sky " too> <pc id|-> <xbox id|-> <path, spaces and all>
                size_t p1 = l.size() > 9 && l[8] == ' ' ? 8 : std::string::npos, p2 = p1 == std::string::npos ? p1 : l.find(' ', p1 + 1),
                       p3 = p2 == std::string::npos ? p2 : l.find(' ', p2 + 1);
                if (p3 == std::string::npos) { out += l + "\n"; continue; }
                const std::string g = l.substr(4, 4), pc = l.substr(p1 + 1, p2 - p1 - 1), path = l.substr(p3 + 1);
                if (pc == "-") {   // (the Xbox's own: all of them written again below)
                    if (xb_only_at == std::string::npos) xb_only_at = out.size();
                    continue;
                }
                auto it = ids.find({key_group(g), path});
                std::string x = "-";
                if (it != ids.end()) {
                    snprintf(b, sizeof b, "%08x", it->second);
                    x = b;
                    listed[it->first] = true;
                }
                out += "tag " + g + " " + pc + " " + x + " " + path + "\n";
            } else {
                if (l.rfind("# the checks", 0) == 0 && checks_at == std::string::npos) checks_at = out.size();
                out += l + "\n";
            }
        }
        std::string own;
        for (auto &t : xb) {
            if (listed.count({key_group(t.group), t.path})) continue;
            snprintf(b, sizeof b, "%08x", t.id);
            own += "tag " + t.group + " - " + b + " " + t.path + "\n";
        }
        if (xb_only_at == std::string::npos) xb_only_at = checks_at == std::string::npos ? out.size() : checks_at;
        out.insert(xb_only_at, own);
        if (out == text) continue;
        const fs::path tmp = e.path().u8string() + ".new";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!(f << out)) { why = "couldn't write " + tmp.u8string(); continue; }
        }
        fs::remove(e.path(), ec);
        fs::rename(tmp, e.path(), ec);
        if (ec) { why = "couldn't put " + e.path().u8string() + " in place"; continue; }
        n++;
    }
    return n;
}

}   // namespace h1ulink
