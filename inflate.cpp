// Halo Map Porter - a small zlib/deflate decoder. See inflate.hpp.
// Canonical Huffman decoding the way RFC 1951 describes it (counts per code length, symbols in code order).
#include "inflate.hpp"

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
    static bool ready = false;
    if (!ready) {
        short len[288];
        int s = 0;
        for (; s < 144; s++) len[s] = 8;
        for (; s < 256; s++) len[s] = 9;
        for (; s < 280; s++) len[s] = 7;
        for (; s < 288; s++) len[s] = 8;
        build(lit, len, 288);
        for (s = 0; s < 30; s++) len[s] = 5;
        build(dist, len, 30);
        ready = true;
    }
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
} // namespace inflate
