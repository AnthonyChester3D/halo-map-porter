// Halo Map Porter - a small zlib/deflate decoder. See inflate.hpp.
// Canonical Huffman decoding the way RFC 1951 describes it (counts per code length, symbols in code order).
#include "inflate.hpp"
#include <mutex>
#include <algorithm>
#include <vector>

#include <cstring>

namespace inflate {
namespace {

struct Bits {
    const uint8_t *p, *end;
    uint32_t buf = 0;
    int cnt = 0;
    bool bad = false;
    int need(int n) {                       // the next n bits, LSB first
        while (cnt < n) {
            if (p == end) { bad = true; return 0; }
            buf |= (uint32_t)*p++ << cnt;
            cnt += 8;
        }
        int v = (int)(buf & ((1u << n) - 1));
        buf >>= n;
        cnt -= n;
        return v;
    }
    void align() { buf = 0; cnt = 0; }      // stored blocks start on a byte boundary
};

struct Huff {
    short count[16];                        // codes of each length
    short symbol[320];                      // symbols ordered by code
};

// build from code lengths; false = over-subscribed
bool build(Huff &h, const short *len, int n) {
    short offs[16];
    memset(h.count, 0, sizeof h.count);
    for (int s = 0; s < n; s++) h.count[len[s]]++;
    if (h.count[0] == n) return true;       // no codes
    int left = 1;
    for (int l = 1; l < 16; l++) {
        left <<= 1;
        left -= h.count[l];
        if (left < 0) return false;
    }
    offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = offs[l] + h.count[l];
    for (int s = 0; s < n; s++)
        if (len[s]) h.symbol[offs[len[s]]++] = (short)s;
    return true;
}

int decode(Bits &b, const Huff &h) {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
        code |= b.need(1);
        if (b.bad) return -1;
        int count = h.count[l];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

const short kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const short kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const short kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const short kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool codes(Bits &b, std::string &out, const Huff &lit, const Huff &dist) {
    for (;;) {
        int s = decode(b, lit);
        if (s < 0) return false;
        if (s < 256) { out.push_back((char)s); continue; }
        if (s == 256) return true;
        s -= 257;
        if (s >= 29) return false;
        int len = kLenBase[s] + b.need(kLenExtra[s]);
        int d = decode(b, dist);
        if (d < 0 || d >= 30) return false;
        size_t back = (size_t)(kDistBase[d] + b.need(kDistExtra[d]));
        if (b.bad || back > out.size()) return false;
        size_t from = out.size() - back;
        for (int k = 0; k < len; k++) out.push_back(out[from + k]);   // (may overlap what it writes)
    }
}

bool fixed(Bits &b, std::string &out) {
    static Huff lit, dist;
    static std::once_flag ready;
    std::call_once(ready, [] {
        short len[288];
        int s = 0;
        for (; s < 144; s++) len[s] = 8;
        for (; s < 256; s++) len[s] = 9;
        for (; s < 280; s++) len[s] = 7;
        for (; s < 288; s++) len[s] = 8;
        build(lit, len, 288);
        for (s = 0; s < 30; s++) len[s] = 5;
        build(dist, len, 30);
    });
    return codes(b, out, lit, dist);
}

bool dynamic(Bits &b, std::string &out) {
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    short len[320];
    int nlen = b.need(5) + 257, ndist = b.need(5) + 1, ncode = b.need(4) + 4;
    if (b.bad || nlen > 286 || ndist > 30) return false;
    for (int i = 0; i < 19; i++) len[order[i]] = i < ncode ? (short)b.need(3) : 0;
    Huff lencode, lit, dist;
    if (!build(lencode, len, 19)) return false;
    int i = 0;
    while (i < nlen + ndist) {
        int s = decode(b, lencode);
        if (s < 0) return false;
        if (s < 16) { len[i++] = (short)s; continue; }
        short v = 0;
        int rep;
        if (s == 16) { if (!i) return false; v = len[i - 1]; rep = 3 + b.need(2); }
        else if (s == 17) rep = 3 + b.need(3);
        else rep = 11 + b.need(7);
        if (b.bad || i + rep > nlen + ndist) return false;
        while (rep--) len[i++] = v;
    }
    if (len[256] == 0) return false;        // no end-of-block code
    if (!build(lit, len, nlen) || !build(dist, len + nlen, ndist)) return false;
    return codes(b, out, lit, dist);
}

} // namespace

bool zlib(const uint8_t *in, size_t n, std::string &out, size_t expect) {
    out.clear();
    if (n < 2 || (in[0] & 0x0f) != 8 || ((in[0] << 8) | in[1]) % 31 || (in[1] & 0x20)) return false;
    if (expect) out.reserve(expect);
    Bits b{in + 2, in + n};
    for (;;) {
        int last = b.need(1), type = b.need(2);
        if (b.bad) return false;
        if (type == 0) {
            b.align();
            if (b.end - b.p < 4) return false;
            unsigned len = b.p[0] | b.p[1] << 8, nlen = b.p[2] | b.p[3] << 8;
            if ((len ^ 0xffff) != nlen) return false;
            b.p += 4;
            if ((size_t)(b.end - b.p) < len) return false;
            out.append((const char *)b.p, len);
            b.p += len;
        } else if (type == 1) {
            if (!fixed(b, out)) return false;
        } else if (type == 2) {
            if (!dynamic(b, out)) return false;
        } else {
            return false;
        }
        if (last) return true;
    }
}

namespace {
struct BitOut {
    std::string &o;
    uint64_t acc = 0;
    int nbits = 0;
    explicit BitOut(std::string &s) : o(s) {}
    void put(uint32_t v, int n) {   // (least significant bit first)
        acc |= (uint64_t)v << nbits;
        nbits += n;
        while (nbits >= 8) { o += (char)(acc & 255); acc >>= 8; nbits -= 8; }
    }
    void huff(uint32_t code, int len) {   // (Huffman codes go most significant bit first)
        uint32_t r = 0;
        for (int i = 0; i < len; i++) r |= ((code >> i) & 1) << (len - 1 - i);
        put(r, len);
    }
    void flush() { if (nbits) { o += (char)(acc & 255); acc = 0; nbits = 0; } }
};
}   // namespace

namespace {
// Code lengths for these frequencies, none longer than `limit` (0 for an unused symbol; at least two symbols get a
// code, as inflaters want).
std::vector<int> code_lengths(std::vector<uint64_t> f, int limit) {
    size_t n = f.size();
    std::vector<int> len(n, 0);
    int used = 0;
    for (auto v : f) used += v > 0;
    if (used == 0) { f[0] = 1; f[1 % n] = 1; }
    else if (used == 1) { for (size_t k = 0; k < n; k++) if (!f[k]) { f[k] = 1; break; } }
    for (;;) {
        struct Nd { uint64_t w; int l, r; };
        std::vector<Nd> nodes;
        std::vector<std::pair<uint64_t, int>> heap;
        for (size_t k = 0; k < n; k++)
            if (f[k]) { nodes.push_back({f[k], -1, (int)k}); heap.push_back({f[k], (int)nodes.size() - 1}); }
        auto cmp = [](const std::pair<uint64_t, int> &a, const std::pair<uint64_t, int> &b) { return a.first > b.first; };
        std::make_heap(heap.begin(), heap.end(), cmp);
        while (heap.size() > 1) {
            std::pop_heap(heap.begin(), heap.end(), cmp); auto a = heap.back(); heap.pop_back();
            std::pop_heap(heap.begin(), heap.end(), cmp); auto b = heap.back(); heap.pop_back();
            nodes.push_back({a.first + b.first, a.second, b.second});
            heap.push_back({a.first + b.first, (int)nodes.size() - 1});
            std::push_heap(heap.begin(), heap.end(), cmp);
        }
        std::fill(len.begin(), len.end(), 0);
        int maxl = 0;
        std::vector<std::pair<int, int>> st{{heap[0].second, 0}};
        while (!st.empty()) {
            auto [k, d] = st.back();
            st.pop_back();
            if (nodes[(size_t)k].l < 0) { len[(size_t)nodes[(size_t)k].r] = std::max(d, 1); maxl = std::max(maxl, d); }
            else { st.push_back({nodes[(size_t)k].l, d + 1}); st.push_back({nodes[(size_t)k].r, d + 1}); }
        }
        if (maxl <= limit) return len;
        for (auto &v : f) if (v) v = (v >> 1) | 1;   // (flatter, until it fits)
    }
}
std::vector<uint32_t> canonical(const std::vector<int> &len) {
    int bl[16] = {0};
    for (int l : len) if (l) bl[l]++;
    uint32_t next[16] = {0}, code = 0;
    for (int b = 1; b < 16; b++) { code = (code + (uint32_t)bl[b - 1]) << 1; next[b] = code; }
    std::vector<uint32_t> c(len.size(), 0);
    for (size_t k = 0; k < len.size(); k++) if (len[k]) c[k] = next[len[k]]++;
    return c;
}
struct Sym { uint16_t lit; uint16_t dist; uint16_t lx, dx; };   // lit < 286; dist 0xFFFF none; extra bits' values
}   // namespace

std::string zlib_compress(const uint8_t *in, size_t n) {
    std::string out;
    out.reserve(n / 3 + 64);
    out += (char)0x78;
    out += (char)0x01;   // (0x7801: a valid header, 32 KB window)
    BitOut b(out);
    const size_t kWindow = 32768, kHash = 1 << 16;
    const int kChain = 48, kMax = 258;
    std::vector<int64_t> head(kHash, -1), prev(kWindow, -1);
    auto hash = [&](size_t i) { return (uint32_t)((in[i] << 10) ^ (in[i + 1] << 5) ^ in[i + 2]) & (kHash - 1); };
    auto insert = [&](size_t i) {
        if (i + 2 >= n) return;
        uint32_t h = hash(i);
        prev[i % kWindow] = head[h];
        head[h] = (int64_t)i;
    };
    const size_t kBlockSyms = 1 << 16;   // (a block, with its own codes, every 64K symbols)
    std::vector<Sym> syms;
    syms.reserve(kBlockSyms + 1);
    auto flush_block = [&](bool last) {
        syms.push_back({256, 0xFFFF, 0, 0});
        std::vector<uint64_t> lf(286, 0), df(30, 0);
        for (auto &s : syms) { lf[s.lit]++; if (s.dist != 0xFFFF) df[s.dist]++; }
        std::vector<int> ll = code_lengths(lf, 15), dl = code_lengths(df, 15);
        int hlit = 286, hdist = 30;
        while (hlit > 257 && !ll[(size_t)hlit - 1]) hlit--;
        while (hdist > 1 && !dl[(size_t)hdist - 1]) hdist--;
        std::vector<int> all(ll.begin(), ll.begin() + hlit);
        all.insert(all.end(), dl.begin(), dl.begin() + hdist);
        // The code lengths, run-length coded (16 repeat previous, 17/18 zeros).
        struct CL { int sym, extra, bits; };
        std::vector<CL> cl;
        for (size_t k = 0; k < all.size();) {
            size_t r = 1;
            while (k + r < all.size() && all[k + r] == all[k]) r++;
            if (all[k] == 0 && r >= 3) {
                size_t m = std::min<size_t>(r, 138);
                if (m >= 11) cl.push_back({18, (int)m - 11, 7}); else cl.push_back({17, (int)m - 3, 3});
                k += m;
            } else if (all[k] != 0 && r >= 4) {
                cl.push_back({all[k], 0, 0});
                size_t m = std::min<size_t>(r - 1, 6);
                cl.push_back({16, (int)m - 3, 2});
                k += 1 + m;
            } else {
                cl.push_back({all[k], 0, 0});
                k++;
            }
        }
        std::vector<uint64_t> cf(19, 0);
        for (auto &c : cl) cf[(size_t)c.sym]++;
        std::vector<int> clen = code_lengths(cf, 7);
        static const int order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        int hclen = 19;
        while (hclen > 4 && !clen[(size_t)order[hclen - 1]]) hclen--;
        std::vector<uint32_t> lc = canonical(ll), dc = canonical(dl), cc = canonical(clen);
        b.put(last ? 1 : 0, 1);
        b.put(2, 2);   // dynamic Huffman codes
        b.put((uint32_t)hlit - 257, 5);
        b.put((uint32_t)hdist - 1, 5);
        b.put((uint32_t)hclen - 4, 4);
        for (int k = 0; k < hclen; k++) b.put((uint32_t)clen[(size_t)order[k]], 3);
        for (auto &c : cl) {
            b.huff(cc[(size_t)c.sym], clen[(size_t)c.sym]);
            if (c.bits) b.put((uint32_t)c.extra, c.bits);
        }
        for (auto &s : syms) {
            b.huff(lc[s.lit], ll[s.lit]);
            if (s.lit > 256) {
                int li = s.lit - 257;
                if (kLenExtra[li]) b.put(s.lx, kLenExtra[li]);
                b.huff(dc[s.dist], dl[s.dist]);
                if (kDistExtra[s.dist]) b.put(s.dx, kDistExtra[s.dist]);
            }
        }
        syms.clear();
    };
    size_t i = 0;
    while (i < n) {
        int best = 0;
        size_t best_d = 0;
        if (i + 2 < n) {
            int64_t c = head[hash(i)];
            int chain = kChain;
            size_t lim = std::min<size_t>(kMax, n - i);
            while (c >= 0 && i - (size_t)c <= kWindow - 1 && chain-- > 0) {
                const uint8_t *a = in + c, *p = in + i;
                if (a[best] == p[best]) {
                    int l = 0;
                    while ((size_t)l < lim && a[l] == p[l]) l++;
                    if (l > best) { best = l; best_d = i - (size_t)c; if ((size_t)l == lim) break; }
                }
                int64_t nx = prev[(size_t)c % kWindow];
                if (nx >= c) break;
                c = nx;
            }
        }
        if (best >= 3) {
            int li = 28;
            while (kLenBase[li] > best) li--;
            int di = 29;
            while ((size_t)kDistBase[di] > best_d) di--;
            syms.push_back({(uint16_t)(257 + li), (uint16_t)di, (uint16_t)(best - kLenBase[li]), (uint16_t)(best_d - (size_t)kDistBase[di])});
            for (int k = 0; k < best; k++) insert(i + (size_t)k);
            i += (size_t)best;
        } else {
            syms.push_back({in[i], 0xFFFF, 0, 0});
            insert(i);
            i++;
        }
        if (syms.size() >= kBlockSyms) flush_block(i >= n);
    }
    if (!syms.empty() || n == 0 || out.size() == 2) flush_block(true);
    b.flush();
    uint32_t s1 = 1, s2 = 0;   // Adler-32
    for (size_t k = 0; k < n;) {
        size_t m = std::min<size_t>(n - k, 5552);
        for (size_t j = 0; j < m; j++) { s1 += in[k + j]; s2 += s1; }
        s1 %= 65521;
        s2 %= 65521;
        k += m;
    }
    uint32_t ad = s2 << 16 | s1;
    for (int k = 3; k >= 0; k--) out += (char)(ad >> (8 * k) & 255);
    return out;
}
} // namespace inflate
