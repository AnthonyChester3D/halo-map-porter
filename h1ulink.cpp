// Halo Map Porter - the link file Halo 1 Unlimited reads for a linked Custom Edition / Xbox pair (h1ulink.hpp).
#include "h1ulink.hpp"
#include "inflate.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

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
    const uint32_t base = version == 5 ? 0x803A6000u : 0x40440000u;
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
    if (!read_tags(l.ce_map, ce, ce_scen)) { why = "couldn't read the Custom Edition map's tags"; return ""; }
    if (!read_tags(l.xbox_map, xb, xb_scen)) { why = "couldn't read the Xbox map's tags"; return ""; }
    uint64_t ce_size = 0, xb_size = 0;
    std::string ce_hash = map_hash(l.ce_map, &ce_size), xb_hash = map_hash(l.xbox_map, &xb_size);
    if (ce_hash.empty() || xb_hash.empty()) { why = "couldn't read a map to hash it"; return ""; }
    std::string s = "h1u-link 1\ntool " + l.tool + "\n";
    s += "# the two files: the name each game knows the map by, its size in bytes and its hash\n";
    s += "ce   " + l.ce_name + "   " + std::to_string(ce_size) + "   " + ce_hash + "\n";
    s += "xbox " + l.xbox_name + "   " + std::to_string(xb_size) + "   " + xb_hash + "\n";
    s += "# the scenario tag's path in each\nlevel ce   " + ce_scen + "\nlevel xbox " + xb_scen + "\n";
    s += "# placed objects, per palette type: the count (the same in both)\n";
    for (auto &[k, n] : l.placed) s += "placed " + k + " " + std::to_string(n) + "\n";
    s += "# every tag in either map: group, full tag ID in the CE map, in the Xbox map, path\n"
         "# (a tag missing from one side: - there)\n";
    // In the CE map's order, then the Xbox's own (matched by group and path).
    // (a gbxmodel is a model on the Xbox, and a shader may be another shader class there: matched as one group)
    auto key_group = [](const std::string &g) {
        static const char *shaders[] = {"shdr", "senv", "soso", "sotr", "schi", "scex", "swat", "sgla", "smet", "spla"};
        for (auto sh : shaders)
            if (g == sh) return std::string("shdr");
        return g == "mod2" ? std::string("mode") : g;
    };
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

}   // namespace h1ulink
