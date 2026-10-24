// Halo Map Porter - invader-edit and invader-dependency in process (tagedit.hpp).
#include "tagedit.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace tagedit {
namespace {
namespace fs = std::filesystem;

struct TdField {
    const char *name;
    unsigned offset, size;
    int kind, vt, flags, endian, sub, count;
    unsigned mask;
    int classes, nclasses, excluded, nexcluded;
    unsigned rmin, rmax;
};
struct TdStruct { const char *name; unsigned size, first, fields; };
struct TdNames { unsigned first, count; };
struct TdBits { unsigned first, count, width; };
struct TdClass { const char *ext; unsigned fourcc; int root, version; };
#include "tag_defs.inc"

enum Kind { K_VALUE, K_DEP, K_REFLEXIVE, K_DATA, K_STRING, K_ENUM, K_BITFIELD, K_RAW, K_TAGID };
enum Flag { F_READ_ONLY = 1, F_LISTED = 2, F_UNREAD = 4, F_ZERO_ON_SAVE = 8, F_BOUNDS = 16 };
enum VT {   // Invader's ParserStructValue::ValueType order
    INT8, UINT8, INT16, UINT16, INDEX, INT32, UINT32, COLORARGBINT, POINT2DINT, RECTANGLE2D, FLOAT, FRACTION, ANGLE,
    COLORARGB, COLORRGB, VECTOR2D, VECTOR3D, EULER2D, EULER3D, PLANE2D, PLANE3D, POINT2D, POINT3D, QUATERNION, MATRIX,
    REFLEXIVE, DEPENDENCY, TAGID, TAGSTRING, TAGDATAOFFSET, ENUM, BITMASK
};
const double kHaloPi = 3.1415926535897932384626433832795028841971693993751058209749445923078164062862089986280348253421170679;
const size_t kNone = (size_t)-1;

std::atomic<long> g_handled{0}, g_fallback{0};

struct Fail {};   // the tool would do something else here (an error, an option not handled): run it instead

const TdField &field(int sid, unsigned i) { return kTdFields[kTdStructs[sid].first + i]; }
unsigned nfields(int sid) { return kTdStructs[sid].fields; }

uint64_t get(const std::string &b, size_t off, unsigned bytes, bool little) {
    if (off + bytes > b.size()) throw Fail();
    uint64_t v = 0;
    for (unsigned i = 0; i < bytes; i++) {
        unsigned char c = (unsigned char)b[off + (little ? bytes - 1 - i : i)];
        v = v << 8 | c;
    }
    return v;
}
void put(std::string &b, size_t off, unsigned bytes, bool little, uint64_t v) {
    for (unsigned i = 0; i < bytes; i++) {
        unsigned char c = (unsigned char)(v >> (8 * (bytes - 1 - i)));
        b[off + (little ? bytes - 1 - i : i)] = (char)c;
    }
}
uint32_t be32(const std::string &b, size_t off) { return (uint32_t)get(b, off, 4, false); }

// ---- tag classes and paths

const TdClass *class_of(uint32_t fourcc) {
    for (auto &c : kTdTagClasses)
        if (c.fourcc == fourcc) return &c;
    return nullptr;
}
const char *extension_of(uint32_t fourcc) {
    if (fourcc == 0xFFFFFFFFu) return "none";
    const TdClass *c = class_of(fourcc);
    return c ? c->ext : "unknown";
}
uint32_t fourcc_of(const std::string &ext) {
    for (auto &c : kTdTagClasses)
        if (ext == c.ext) return c.fourcc;
    return 0;
}

#ifdef _WIN32
const char kSystemSep = '\\';
#else
const char kSystemSep = '/';
#endif
std::string preferred(std::string p) {
    for (auto &c : p)
        if (c == '\\' || c == '/') c = kSystemSep;
    return p;
}
std::string halo_path(std::string p) {
    for (auto &c : p)
        if (c == '\\' || c == '/') c = '\\';
    return p;
}
// (Invader's remove_duplicate_slashes: one of each run of separators goes per position)
std::string remove_duplicate_slashes(const std::string &path) {
    std::vector<char> v(path.begin(), path.end());
    v.push_back(0);
    auto sep = [](char c) { return c == '\\' || c == '/' || c == kSystemSep; };
    for (char *i = v.data(); *i; i++)
        if (sep(i[0]) && sep(i[1]))
            for (char *j = i + 1; *j; j++) j[0] = j[1];
    return v.data();
}

// ---- a tag as Invader's parser holds it

struct Node;
struct Extra {
    std::string path;      // a reference's path (Halo's '\\' separators)
    uint32_t fourcc = 0;
    std::vector<Node> elems;
    std::string data;
};
struct Node {
    int sid = -1;
    std::string b;              // the struct's bytes, as in the tag file
    std::vector<Extra> x;       // per field: its reference, block elements or data
};

void parse_trailing(const std::string &f, size_t &pos, Node &n);

// The struct's own bytes as Invader's parser keeps them: fields it doesn't read zeroed, flags it doesn't know cleared,
// tag IDs set to none.
void parse_fixed(Node &n) {
    for (unsigned i = 0; i < nfields(n.sid); i++) {
        const TdField &fd = field(n.sid, i);
        if (fd.flags & F_UNREAD) {
            std::fill(n.b.begin() + fd.offset, n.b.begin() + fd.offset + fd.size, '\0');
            continue;
        }
        bool little = fd.endian != 0;
        if (fd.kind == K_BITFIELD) {
            unsigned w = kTdBitfields[fd.sub].width / 8;
            put(n.b, fd.offset, w, little, get(n.b, fd.offset, w, little) & fd.mask);
        } else if (fd.kind == K_TAGID) {
            put(n.b, fd.offset, 4, little, 0xFFFFFFFFu);
        }
    }
}

void parse_trailing(const std::string &f, size_t &pos, Node &n) {
    n.x.assign(nfields(n.sid), Extra());
    for (unsigned i = 0; i < nfields(n.sid); i++) {
        const TdField &fd = field(n.sid, i);
        bool unread = fd.flags & F_UNREAD;
        if (fd.kind == K_DEP) {
            if (fd.size != 16) throw Fail();
            size_t len = be32(n.b, fd.offset + 8);
            uint32_t cc = be32(n.b, fd.offset);
            std::string path;
            if (len > 0) {
                if (len + 1 > f.size() - pos) throw Fail();
                for (size_t k = 0; k < len; k++)
                    if (f[pos + k] == 0) throw Fail();
                if (f[pos + len] != 0) throw Fail();
                path = remove_duplicate_slashes(f.substr(pos, len));
                pos += len + 1;
            } else if (cc == 0 && fd.nclasses > 0) {
                cc = kTdClasses[fd.classes];
            }
            if (!unread) { n.x[i].path = path; n.x[i].fourcc = cc; }
        } else if (fd.kind == K_REFLEXIVE) {
            if (fd.size != 12) throw Fail();
            size_t count = be32(n.b, fd.offset), ss = kTdStructs[fd.sub].size;
            if (count > (f.size() - pos) / (ss ? ss : 1)) throw Fail();
            std::vector<Node> elems(count);
            for (size_t k = 0; k < count; k++) {
                elems[k].sid = fd.sub;
                elems[k].b = f.substr(pos + k * ss, ss);
            }
            pos += count * ss;
            for (auto &e : elems) {
                parse_trailing(f, pos, e);
                parse_fixed(e);
            }
            if (!unread) n.x[i].elems = std::move(elems);
        } else if (fd.kind == K_DATA) {
            if (fd.size != 20) throw Fail();
            size_t size = be32(n.b, fd.offset);
            if (size > f.size() - pos) throw Fail();
            if (!unread) n.x[i].data = f.substr(pos, size);
            pos += size;
        }
    }
}

struct Tag {
    uint32_t fourcc = 0;
    Node root;
};

void load(const fs::path &file, Tag &t) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw Fail();
    std::string f((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (f.size() < 64 || be32(f, 60) != 0x626C616Du) throw Fail();   // "blam"
    t.fourcc = be32(f, 36);
    const TdClass *c = class_of(t.fourcc);
    if (!c || c->root < 0 || get(f, 56, 2, false) != (uint64_t)c->version) throw Fail();
    t.root.sid = c->root;
    size_t ss = kTdStructs[c->root].size;
    if (f.size() - 64 < ss) throw Fail();
    t.root.b = f.substr(64, ss);
    size_t pos = 64 + ss;
    parse_trailing(f, pos, t.root);
    parse_fixed(t.root);
    if (pos != f.size()) throw Fail();
}

// The struct and what follows it, as Invader writes them.
std::string serialize(const Node &n) {
    const TdStruct &st = kTdStructs[n.sid];
    std::string b(st.size, '\0'), tail;
    for (unsigned i = 0; i < st.fields; i++) {
        const TdField &fd = field(n.sid, i);
        if (fd.flags & (F_UNREAD | F_ZERO_ON_SAVE)) continue;
        const Extra &x = n.x[i];
        if (fd.kind == K_DEP) {
            uint32_t cc = x.fourcc;
            if (x.path.empty() && cc == 0 && fd.nclasses > 0) cc = kTdClasses[fd.classes];
            put(b, fd.offset, 4, false, cc);
            put(b, fd.offset + 12, 4, false, 0xFFFFFFFFu);
            if (!x.path.empty()) {
                put(b, fd.offset + 8, 4, false, x.path.size());
                tail += x.path;
                tail.push_back('\0');
            }
        } else if (fd.kind == K_REFLEXIVE) {
            if (x.elems.empty()) continue;
            put(b, fd.offset, 4, false, x.elems.size());
            size_t ss = kTdStructs[fd.sub].size;
            std::string fixed, more;
            for (auto &e : x.elems) {
                std::string s = serialize(e);
                fixed.append(s, 0, ss);
                more.append(s, ss, std::string::npos);
            }
            tail += fixed;
            tail += more;
        } else if (fd.kind == K_DATA) {
            put(b, fd.offset, 4, false, x.data.size());
            tail += x.data;
        } else if (fd.kind == K_BITFIELD) {
            unsigned w = kTdBitfields[fd.sub].width / 8;
            bool little = fd.endian != 0;
            put(b, fd.offset, w, little, get(n.b, fd.offset, w, little) & fd.mask);
        } else {
            std::copy(n.b.begin() + fd.offset, n.b.begin() + fd.offset + fd.size, b.begin() + fd.offset);
        }
    }
    return b + tail;
}

uint32_t crc32(const char *d, size_t n) {
    static const auto table = [] {
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

std::string file_bytes(const Tag &t) {
    std::string out(64, '\0');
    put(out, 36, 4, false, t.fourcc);
    put(out, 44, 4, false, 64);
    put(out, 56, 2, false, (uint64_t)class_of(t.fourcc)->version);
    put(out, 58, 2, false, 255);
    put(out, 60, 4, false, 0x626C616Du);
    out += serialize(t.root);
    put(out, 40, 4, false, ~crc32(out.data() + 64, out.size() - 64));
    return out;
}

// A new element of a block, as Invader makes one: zero, indices none.
Node new_node(int sid) {
    Node n;
    n.sid = sid;
    n.b.assign(kTdStructs[sid].size, '\0');
    n.x.assign(nfields(sid), Extra());
    for (unsigned i = 0; i < nfields(sid); i++) {
        const TdField &fd = field(sid, i);
        if (fd.kind == K_VALUE && fd.vt == INDEX) put(n.b, fd.offset, 2, false, 0xFFFF);
    }
    return n;
}

// ---- values

struct Val { Node *n; unsigned i; };
const TdField &fd_of(const Val &v) { return field(v.n->sid, v.i); }

unsigned components(int vt) {
    switch (vt) {
    case POINT2DINT: case VECTOR2D: case EULER2D: case POINT2D: return 2;
    case COLORRGB: case VECTOR3D: case EULER3D: case PLANE2D: case POINT3D: return 3;
    case RECTANGLE2D: case COLORARGBINT: case COLORARGB: case PLANE3D: case QUATERNION: return 4;
    case MATRIX: return 9;
    default: return 1;
    }
}
unsigned component_bytes(int vt) {
    switch (vt) {
    case INT8: case UINT8: return 1;
    case INT16: case UINT16: case INDEX: case POINT2DINT: case RECTANGLE2D: return 2;
    default: return 4;
    }
}
bool is_int(int vt) { return vt < FLOAT; }
bool is_number(int vt) { return vt < REFLEXIVE; }

struct Num { bool integer; int64_t i; double d; };

std::vector<Num> get_numbers(const Val &v) {
    const TdField &fd = fd_of(v);
    bool little = fd.endian != 0;
    std::vector<Num> out;
    unsigned cb = component_bytes(fd.vt), comps = components(fd.vt);
    size_t off = fd.offset;
    for (int e = 0; e < fd.count; e++) {
        if (fd.vt == COLORARGBINT) {
            uint32_t c = (uint32_t)get(v.n->b, off, 4, little);
            for (int k = 0; k < 4; k++) out.push_back({true, (int64_t)((c >> (24 - 8 * k)) & 0xFF), 0});
            off += 4;
            continue;
        }
        for (unsigned k = 0; k < comps; k++, off += cb) {
            uint64_t r = get(v.n->b, off, cb, little);
            Num nm{is_int(fd.vt), 0, 0};
            switch (fd.vt) {
            case INT8: nm.i = (int8_t)r; break;
            case UINT8: nm.i = (uint8_t)r; break;
            case INT16: case POINT2DINT: case RECTANGLE2D: nm.i = (int16_t)r; break;
            case UINT16: case INDEX: nm.i = (uint16_t)r; break;
            case INT32: nm.i = (int32_t)r; break;
            case UINT32: nm.i = (uint32_t)r; break;
            default: {
                uint32_t u = (uint32_t)r;
                float fl;
                std::memcpy(&fl, &u, 4);
                nm.d = fl;
            }
            }
            out.push_back(nm);
        }
    }
    return out;
}

void set_numbers(const Val &v, const std::vector<Num> &vals) {
    const TdField &fd = fd_of(v);
    bool little = fd.endian != 0;
    unsigned cb = component_bytes(fd.vt), comps = components(fd.vt);
    size_t off = fd.offset, k = 0;
    for (int e = 0; e < fd.count; e++) {
        if (fd.vt == COLORARGBINT) {
            uint32_t c = 0;
            for (int q = 0; q < 4; q++) c |= (uint32_t)(uint8_t)vals[k++].i << (24 - 8 * q);
            put(v.n->b, off, 4, little, c);
            off += 4;
            continue;
        }
        for (unsigned q = 0; q < comps; q++, off += cb) {
            const Num &nm = vals[k++];
            uint64_t r;
            switch (fd.vt) {
            case INT8: case UINT8: r = (uint8_t)nm.i; break;
            case INT16: case UINT16: case INDEX: case POINT2DINT: case RECTANGLE2D: r = (uint16_t)nm.i; break;
            case INT32: case UINT32: r = (uint32_t)nm.i; break;
            default: {
                float fl = (float)nm.d;
                uint32_t u;
                std::memcpy(&u, &fl, 4);
                r = u;
            }
            }
            put(v.n->b, off, cb, little, r);
        }
    }
}

std::string fmt_double(double d) {
    char b[512];
    std::snprintf(b, sizeof b, "%f", d);
    return b;
}

std::string number_text(const Val &v) {
    const TdField &fd = fd_of(v);
    std::string s;
    double mult = (fd.vt == ANGLE || fd.vt == EULER2D || fd.vt == EULER3D) ? 180.0 / kHaloPi : 1.0;
    for (auto &n : get_numbers(v)) {
        if (!s.empty()) s += ",";
        s += n.integer ? std::to_string(n.i) : fmt_double(n.d * mult);
    }
    return s;
}

const char *enum_name(const TdField &fd, unsigned value) {
    const TdNames &e = kTdEnums[fd.sub];
    if (value >= e.count) throw Fail();   // (Invader throws reading it)
    return kTdEnumNames[e.first + value];
}
unsigned enum_value(const Val &v) {
    const TdField &fd = fd_of(v);
    return (unsigned)get(v.n->b, fd.offset, 2, fd.endian != 0);
}
std::vector<unsigned> listed_options(const TdField &fd) {
    std::vector<unsigned> out;
    for (unsigned k = 0; k < kTdEnums[fd.sub].count; k++) {
        bool ex = false;
        for (int q = 0; q < fd.nexcluded; q++)
            if (kTdExcluded[fd.excluded + q] == k) ex = true;
        if (!ex) out.push_back(k);
    }
    return out;
}
int bit_index(const TdField &fd, const std::string &name) {
    const TdBits &b = kTdBitfields[fd.sub];
    for (unsigned k = 0; k < b.count; k++)
        if (name == kTdBitNames[b.first + k]) return (int)k;
    return -1;
}
bool read_bit(const Val &v, int bit) {
    const TdField &fd = fd_of(v);
    unsigned w = kTdBitfields[fd.sub].width / 8;
    return (get(v.n->b, fd.offset, w, fd.endian != 0) >> bit) & 1;
}
void write_bit(const Val &v, int bit, bool on) {
    const TdField &fd = fd_of(v);
    unsigned w = kTdBitfields[fd.sub].width / 8;
    bool little = fd.endian != 0;
    uint64_t x = get(v.n->b, fd.offset, w, little);
    x = on ? x | (1ull << bit) : x & ~(1ull << bit);
    put(v.n->b, fd.offset, w, little, x);
}
std::string string_of(const Val &v) {
    const TdField &fd = fd_of(v);
    size_t end = v.n->b.find('\0', fd.offset);
    if (end == std::string::npos || end >= fd.offset + 32) throw Fail();   // (not terminated)
    return v.n->b.substr(fd.offset, end - fd.offset);
}

std::string get_value(const Val &v, const std::string &bit) {
    const TdField &fd = fd_of(v);
    if (is_number(fd.vt)) return number_text(v);
    switch (fd.vt) {
    case TAGSTRING: return string_of(v);
    case DEPENDENCY: {
        const Extra &x = v.n->x[v.i];
        return preferred(x.path) + "." + extension_of(x.fourcc);
    }
    case ENUM: return enum_name(fd, enum_value(v));
    case BITMASK: {
        int b = bit_index(fd, bit);
        if (b < 0) throw Fail();
        return read_bit(v, b) ? "1" : "0";
    }
    default: throw Fail();
    }
}

// A value as invader-edit's expressions read a plain number (the only kind the app passes): digits, an optional
// fraction, an optional leading minus. Anything else (operators, exponents, spaces) is left to the tool.
bool plain_number(const std::string &s, bool integer, Num &out) {
    size_t i = 0;
    bool neg = false;
    if (i < s.size() && s[i] == '-') { neg = true; i++; }
    size_t digits = 0, frac = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') { i++; digits++; }
    if (i < s.size() && s[i] == '.') {
        i++;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { i++; frac++; }
    }
    if (i != s.size() || digits + frac == 0) return false;
    std::string body = s.substr(neg ? 1 : 0);
    if (integer) {
        if (digits == 0 || digits > 18) return false;   // (std::stoll of the leading digits)
        int64_t v = std::stoll(body);
        out = {true, neg ? -1 * v : v, 0};
    } else {
        double v = std::strtod(body.c_str(), nullptr);
        out = {false, 0, neg ? -1.0 * v : v};
    }
    return true;
}

void set_value(const Val &v, const std::string &value, const std::string &bit) {
    const TdField &fd = fd_of(v);
    if (is_number(fd.vt)) {
        std::vector<std::string> parts;
        size_t start = 0;
        for (size_t k = 0; k <= value.size(); k++)
            if (k == value.size() || value[k] == ',') { parts.push_back(value.substr(start, k - start)); start = k + 1; }
        std::vector<Num> vals = get_numbers(v);
        if (parts.size() != vals.size()) throw Fail();
        for (size_t k = 0; k < parts.size(); k++) {
            Num n;
            if (!plain_number(parts[k], vals[k].integer, n)) throw Fail();
            vals[k] = n;
        }
        set_numbers(v, vals);
        return;
    }
    switch (fd.vt) {
    case TAGSTRING: {
        if (value.size() > 31) throw Fail();
        std::fill(v.n->b.begin() + fd.offset, v.n->b.begin() + fd.offset + 32, '\0');
        std::copy(value.begin(), value.end(), v.n->b.begin() + fd.offset);
        return;
    }
    case DEPENDENCY: {
        Extra &x = v.n->x[v.i];
        if (value.empty()) { x.path.clear(); return; }
        std::string h = halo_path(value);
        size_t dot = h.rfind('.');
        if (dot == std::string::npos) throw Fail();
        uint32_t cc = fourcc_of(h.substr(dot + 1));
        if (cc == 0) throw Fail();
        if (fd.nclasses > 0) {
            bool ok = false;
            for (int k = 0; k < fd.nclasses; k++)
                if (kTdClasses[fd.classes + k] == cc) ok = true;
            if (!ok) throw Fail();
        }
        x.path = h.substr(0, dot);
        x.fourcc = cc;
        return;
    }
    case ENUM: {
        const TdNames &e = kTdEnums[fd.sub];
        for (unsigned k = 0; k < e.count; k++)
            if (value == kTdEnumNames[e.first + k]) { put(v.n->b, fd.offset, 2, fd.endian != 0, k); return; }
        throw Fail();
    }
    case BITMASK: {
        int b = bit_index(fd, bit);
        if (b < 0 || (value != "0" && value != "1")) throw Fail();
        write_bit(v, b, value == "1");
        return;
    }
    default: throw Fail();
    }
}

// ---- keys (invader-edit's: member[range].member...bit)

std::pair<size_t, size_t> get_range(std::string &key) {
    if (key.empty() || key[0] != '[') throw Fail();
    size_t close = key.find(']');
    if (close == std::string::npos) throw Fail();
    std::string r = key.substr(1, close - 1);
    key = key.substr(close + 1);
    if (r == "*") return {0, kNone};
    if (r == "end") return {kNone, kNone};
    size_t hyphens = std::count(r.begin(), r.end(), '-');
    auto digits = [](const std::string &s) {
        return !s.empty() && s.size() < 10 && s.find_first_not_of("0123456789") == std::string::npos;
    };
    if (hyphens == 0) {
        if (!digits(r)) throw Fail();   // (strtoul would read what it could)
        size_t v = std::stoul(r);
        return {v, v};
    }
    if (hyphens > 1) throw Fail();
    size_t h = r.find('-');
    std::string a = r.substr(0, h), b = r.substr(h + 1);
    size_t lo, hi;
    if (a == "end") lo = kNone;
    else if (digits(a)) lo = std::stoul(a);
    else throw Fail();
    if (b == "end") hi = kNone;
    else if (digits(b)) hi = std::stoul(b);
    else throw Fail();
    if (lo > hi) throw Fail();
    return {lo, hi};
}

void build_array(Node &n, std::string key, std::vector<Val> &out, std::string *bit, std::pair<size_t, size_t> *range) {
    if (key.empty() || key[0] != '.') throw Fail();
    key = key.substr(1);
    size_t end = key.find_first_of(".[]");
    std::string member = key.substr(0, end);
    key = end == std::string::npos ? std::string() : key.substr(end);
    if (member.empty()) throw Fail();
    for (unsigned i = 0; i < nfields(n.sid); i++) {
        const TdField &fd = field(n.sid, i);
        if (!(fd.flags & F_LISTED) || member != fd.name) continue;
        if (fd.kind == K_REFLEXIVE) {
            if (key.empty()) { out.push_back({&n, i}); return; }
            auto &elems = n.x[i].elems;
            size_t count = elems.size();
            auto r = get_range(key);
            if (count == 0) {
                if (r.first == 0 && r.second == kNone) return;
                throw Fail();
            }
            if (r.first == kNone) r.first = count - 1;
            if (r.second == kNone) r.second = count - 1;
            if (count < r.first || count <= r.second) throw Fail();
            if (key.empty() && range) { *range = r; out.push_back({&n, i}); return; }
            for (size_t k = r.first; k <= r.second; k++) build_array(elems[k], key, out, bit, range);
            return;
        }
        out.push_back({&n, i});
        if (fd.kind == K_BITFIELD) {
            if (key.empty() || key[0] != '.' || !bit) throw Fail();
            *bit = key.substr(1);
        } else if (!key.empty()) {
            throw Fail();
        }
        return;
    }
    throw Fail();
}

std::vector<Val> values_for(Node &root, const std::string &key, bool writable, std::string *bit,
                            std::pair<size_t, size_t> *range) {
    if (key.empty()) throw Fail();
    std::vector<Val> out;
    build_array(root, "." + key, out, bit, range);
    if (writable)
        for (auto &v : out)
            if (fd_of(v).flags & F_READ_ONLY) throw Fail();
    return out;
}

// ---- invader-edit -L

const char *type_name(int vt) {
    switch (vt) {
    case REFLEXIVE: return "array";
    case INT8: return "int8";
    case INT16: return "int16";
    case INT32: return "int32";
    case UINT8: return "uint8";
    case UINT16: return "uint16";
    case UINT32: return "uint32";
    case FLOAT: case FRACTION: return "float32";
    case ANGLE: return "angle (float32)";
    case INDEX: return "index";
    case COLORARGB: return "color (ARGB float32)";
    case COLORRGB: return "color (RGB float32)";
    case COLORARGBINT: return "color (A8R8G8B8)";
    case POINT2DINT: return "point2d (integer)";
    case POINT2D: return "point2d (float32)";
    case POINT3D: return "point3d (float32)";
    case PLANE2D: return "plane2d (float32)";
    case PLANE3D: return "plane3d (float32)";
    case QUATERNION: return "quaternion (float32)";
    case DEPENDENCY: return "dependency";
    case TAGID: return "bullshit";
    case TAGSTRING: return "string";
    case TAGDATAOFFSET: return "data";
    case ENUM: return "enum";
    case BITMASK: return "bitmask";
    case MATRIX: return "matrix (float32)";
    case VECTOR2D: return "vector2d (float)";
    case VECTOR3D: return "vector3d (float)";
    case EULER2D: return "euler2d (float)";
    case EULER3D: return "euler3d (float)";
    case RECTANGLE2D: return "rectangle2d (float)";
    }
    return "";
}

struct Line { size_t level; std::string name, type; };

void list_everything(Node &n, std::vector<Line> &out, size_t level) {
    for (unsigned i = 0; i < nfields(n.sid); i++) {
        const TdField &fd = field(n.sid, i);
        if (!(fd.flags & F_LISTED)) continue;
        std::string type = type_name(fd.vt), name = fd.name;
        if (fd.flags & F_BOUNDS) type += " (bounds)";
        Val v{&n, i};
        if (fd.kind == K_REFLEXIVE) name += "[" + std::to_string(n.x[i].elems.size()) + "]";
        else if (is_number(fd.vt)) name += " (" + number_text(v) + ")";
        else if (fd.kind == K_DEP) {
            if (!n.x[i].path.empty()) name += " (" + preferred(n.x[i].path) + "." + extension_of(n.x[i].fourcc) + ")";
        } else if (fd.kind == K_STRING) {
            name += " (" + string_of(v) + ")";
        }
        out.push_back({level, name, type});
        if (fd.kind == K_REFLEXIVE) {
            auto &elems = n.x[i].elems;
            for (size_t m = 0; m < elems.size(); m++) {
                out.push_back({level + 1, std::string(fd.name) + "[" + std::to_string(m) + "]", "struct"});
                list_everything(elems[m], out, level + 2);
            }
        } else if (fd.kind == K_BITFIELD) {
            const TdBits &b = kTdBitfields[fd.sub];
            for (unsigned k = 0; k < b.count; k++)
                if (fd.mask >> k & 1)
                    out.push_back({level + 1, std::string(kTdBitNames[b.first + k]) + (read_bit(v, (int)k) ? " (1)" : " (0)"),
                                   "bitfield"});
        } else if (fd.kind == K_ENUM) {
            const char *cur = enum_name(fd, enum_value(v));
            for (unsigned k : listed_options(fd)) {
                std::string o = kTdEnumNames[kTdEnums[fd.sub].first + k];
                if (o == cur) o += " (*)";
                out.push_back({level + 1, o, "enum value"});
            }
        }
    }
}

void list_lines(Node &root, std::vector<std::string> &out) {
    std::vector<Line> a;
    list_everything(root, a, 0);
    const size_t width = 80;
#ifdef _WIN32
    const size_t subtract = 1;
#else
    const size_t subtract = 0;
#endif
    for (size_t ar = 0; ar < a.size(); ar++) {
        std::string indent;
        for (size_t e = 0; e < a[ar].level; e++) {
            bool next = false;
            for (size_t ar2 = ar + 1; ar2 < a.size(); ar2++) {
                if (a[ar2].level == e + 1) { next = true; break; }
                if (a[ar2].level <= e) break;
            }
            if (next) indent += e + 1 == a[ar].level ? " \xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80" : " \xE2\x94\x82  ";
            else indent += e + 1 == a[ar].level ? " \xE2\x94\x94\xE2\x94\x80\xE2\x94\x80" : "    ";
        }
        std::string s = indent + a[ar].name;
        size_t len = 0;
        for (unsigned char c : s) len += (c & 0xC0) != 0x80;
        const std::string &type = a[ar].type;
        if (len + 1 + subtract + type.size() < width) {
            for (size_t q = len; q < width - type.size() - subtract; q++) s += ' ';
            s += type;
        }
        out.push_back(s);
    }
}

// ---- command lines

struct Action { char op; std::string key, value; size_t count = 0, pos = 0; };

bool edit(const std::vector<std::string> &args, std::string &output, int &code) {
    std::string tags = "tags";
    std::vector<std::string> positional;
    std::vector<Action> actions;
    bool check_read_only = true;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string &a = args[i];
        auto need = [&](size_t n) { if (i + n >= args.size()) throw Fail(); };
        if (a == "-t") { need(1); tags = args[++i]; }
        else if (a == "-n") check_read_only = false;
        else if (a == "-G" || a == "-C") { need(1); actions.push_back({a[1], args[++i]}); }
        else if (a == "-S") { need(2); actions.push_back({'S', args[i + 1], args[i + 2]}); i += 2; }
        else if (a == "-L") actions.push_back({'L'});
        else if (a == "-E") { need(1); actions.push_back({'E', args[++i]}); }
        else if (a == "-I") {
            need(3);
            Action ac{'I', args[i + 1]};
            const std::string &c = args[i + 2], &p = args[i + 3];
            if (c.empty() || c.size() > 9 || c.find_first_not_of("0123456789") != std::string::npos) throw Fail();
            ac.count = std::stoul(c);
            if (p == "end") ac.pos = kNone;
            else if (!p.empty() && p.size() < 10 && p.find_first_not_of("0123456789") == std::string::npos) ac.pos = std::stoul(p);
            else throw Fail();
            actions.push_back(ac);
            i += 3;
        } else if (!a.empty() && a[0] == '-') throw Fail();   // (an option the app doesn't use)
        else positional.push_back(a);
    }
    if (positional.size() != 1) throw Fail();
    std::string tag_path = preferred(positional[0]);
    fs::path file = fs::u8path(tags) / fs::u8path(tag_path);
    Tag t;
    load(file, t);

    std::vector<std::string> lines;
    bool save = false;
    for (auto &ac : actions) {
        switch (ac.op) {
        case 'L': list_lines(t.root, lines); break;
        case 'G': {
            std::string bit;
            for (auto &v : values_for(t.root, ac.key, false, &bit, nullptr)) lines.push_back(get_value(v, bit));
            break;
        }
        case 'S': {
            std::string bit;
            save = true;
            for (auto &v : values_for(t.root, ac.key, check_read_only, &bit, nullptr)) set_value(v, ac.value, bit);
            break;
        }
        case 'C':
            for (auto &v : values_for(t.root, ac.key, false, nullptr, nullptr)) {
                if (fd_of(v).kind != K_REFLEXIVE) throw Fail();
                lines.push_back(std::to_string(v.n->x[v.i].elems.size()));
            }
            break;
        case 'I':
            save = true;
            for (auto &v : values_for(t.root, ac.key, check_read_only, nullptr, nullptr)) {
                const TdField &fd = fd_of(v);
                if (fd.kind != K_REFLEXIVE) throw Fail();
                auto &elems = v.n->x[v.i].elems;
                if (ac.count + elems.size() > fd.rmax) throw Fail();
                size_t at = ac.pos == kNone ? elems.size() : ac.pos;
                if (at > elems.size()) throw Fail();
                elems.insert(elems.begin() + at, ac.count, new_node(fd.sub));
            }
            break;
        case 'E': {
            save = true;
            std::pair<size_t, size_t> range{0, 0};
            for (auto &v : values_for(t.root, ac.key, check_read_only, nullptr, &range)) {
                const TdField &fd = fd_of(v);
                if (fd.kind != K_REFLEXIVE) throw Fail();
                auto &elems = v.n->x[v.i].elems;
                size_t n = range.second - range.first + 1;
                if (range.first >= elems.size() || n > elems.size() - range.first) throw Fail();
                if (elems.size() - n < fd.rmin) throw Fail();
                elems.erase(elems.begin() + range.first, elems.begin() + range.first + n);
            }
            break;
        }
        default: throw Fail();
        }
    }
    if (save) {
        // (saved only as the tag kind its extension names)
        std::string f = file.u8string();
        size_t dot = f.rfind('.');
        if (dot == std::string::npos || fourcc_of(f.substr(dot + 1)) != t.fourcc) throw Fail();
        std::string bytes = file_bytes(t);
        std::ofstream o(file, std::ios::binary | std::ios::trunc);
        if (!o.write(bytes.data(), (std::streamsize)bytes.size())) throw Fail();
    }
    output.clear();
    for (auto &l : lines) output += l + "\n";
    code = 0;
    return true;
}

// ---- reading only the references (invader-dependency, invader-refactor): the same walk through the file and the same
// checks as load(), without keeping anything else.

struct Ref { std::string path; uint32_t fourcc; bool listed, unread; size_t at; };   // at: its struct field's offset

std::string read_file(const fs::path &file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw Fail();
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void scan_struct(const std::string &f, size_t at, int sid, size_t &pos, bool listed, bool unread, std::vector<Ref> &out) {
    for (unsigned i = 0; i < nfields(sid); i++) {
        const TdField &fd = field(sid, i);
        bool l = listed && (fd.flags & F_LISTED), u = unread || (fd.flags & F_UNREAD);
        size_t o = at + fd.offset;
        if (fd.kind == K_DEP) {
            if (fd.size != 16) throw Fail();
            size_t len = be32(f, o + 8);
            if (len > 0) {
                if (len + 1 > f.size() - pos) throw Fail();
                for (size_t k = 0; k < len; k++)
                    if (f[pos + k] == 0) throw Fail();
                if (f[pos + len] != 0) throw Fail();
                out.push_back({remove_duplicate_slashes(f.substr(pos, len)), be32(f, o), l, u, o});
                pos += len + 1;
            }
        } else if (fd.kind == K_REFLEXIVE) {
            if (fd.size != 12) throw Fail();
            size_t count = be32(f, o), ss = kTdStructs[fd.sub].size;
            if (count > (f.size() - pos) / (ss ? ss : 1)) throw Fail();
            size_t first = pos;
            pos += count * ss;
            for (size_t k = 0; k < count; k++) scan_struct(f, first + k * ss, fd.sub, pos, l, u, out);
        } else if (fd.kind == K_DATA) {
            if (fd.size != 20) throw Fail();
            size_t size = be32(f, o);
            if (size > f.size() - pos) throw Fail();
            pos += size;
        }
    }
}

// The tag's references (throws Fail where load() would).
std::vector<Ref> scan(const std::string &f, uint32_t *fourcc = nullptr) {
    if (f.size() < 64 || be32(f, 60) != 0x626C616Du) throw Fail();
    uint32_t cc = be32(f, 36);
    const TdClass *c = class_of(cc);
    if (!c || c->root < 0 || get(f, 56, 2, false) != (uint64_t)c->version) throw Fail();
    size_t ss = kTdStructs[c->root].size;
    if (f.size() - 64 < ss) throw Fail();
    size_t pos = 64 + ss;
    std::vector<Ref> out;
    scan_struct(f, 64, c->root, pos, true, false, out);
    if (pos != f.size()) throw Fail();
    if (fourcc) *fourcc = cc;
    return out;
}

// ---- invader-dependency

struct Found { std::string path; uint32_t fourcc; bool broken; };

bool dependency(const std::vector<std::string> &args, std::string &output, int &code) {
    std::vector<fs::path> dirs;
    std::vector<std::string> positional;
    bool recursive = false;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string &a = args[i];
        if (a == "-t") { if (i + 1 >= args.size()) throw Fail(); dirs.push_back(fs::u8path(args[++i])); }
        else if (a == "-r") recursive = true;
        else if (!a.empty() && a[0] == '-') throw Fail();
        else positional.push_back(a);
    }
    if (positional.size() != 1) throw Fail();
    if (dirs.empty()) dirs.push_back("tags");
    std::string p = preferred(positional[0]);
    size_t dot = p.rfind('.');
    if (dot == std::string::npos) throw Fail();
    uint32_t cc = fourcc_of(p.substr(dot + 1));
    if (cc == 0) throw Fail();
    std::vector<Found> found;
    auto find_in = [&](const std::string &path, uint32_t fourcc, auto &self) -> void {
        std::string rel = preferred(path) + "." + extension_of(fourcc);
        for (auto &d : dirs) {
            fs::path file = d / fs::u8path(rel);
            std::error_code ec;
            if (!fs::exists(file, ec)) continue;
            for (auto &r : scan(read_file(file))) {
                if (!r.listed || r.unread) continue;
                std::string dp = preferred(r.path);
                bool dupe = false;
                for (auto &f : found)
                    if (f.path == dp && f.fourcc == r.fourcc) { dupe = true; break; }
                if (dupe) continue;
                std::string join = dp + "." + extension_of(r.fourcc);
                bool ok = false;
                for (auto &d2 : dirs) {
                    std::error_code e2;
                    if (fs::is_regular_file(d2 / fs::u8path(join), e2)) { ok = true; break; }
                }
                found.push_back({dp, r.fourcc, !ok});
                if (ok && recursive) self(dp, r.fourcc, self);
            }
            return;
        }
        throw Fail();   // (the tool reports the tag missing)
    };
    find_in(p.substr(0, dot), cc, find_in);
    output.clear();
    for (auto &f : found) output += preferred(f.path) + "." + extension_of(f.fourcc) + (f.broken ? " [BROKEN]" : "") + "\n";
    code = 0;
    return true;
}

// ---- invader-refactor -M no-move (references renamed in place; no tag moved)

struct TagPath { std::string path; uint32_t fourcc; };   // Halo separators
bool same(const TagPath &a, const TagPath &b) { return a.path == b.path && a.fourcc == b.fourcc; }

TagPath split_path(const std::string &arg) {
    std::string h = halo_path(arg);
    size_t dot = h.rfind('.');
    if (dot == std::string::npos) throw Fail();
    uint32_t cc = fourcc_of(h.substr(dot + 1));
    if (cc == 0) throw Fail();
    return {h.substr(0, dot), cc};
}

// Applies the replacements in order (as Invader: one can rename what an earlier one made) to every reference the
// tag reads. Invader goes through the tag once per replacement; a reference only ever meets them in that order, so
// going through it once and trying each reference against them in turn renames the same. Returns how many were
// replaced; throws Fail where Invader refuses (a class the reference can't take).
size_t refactor_node(Node &n, const std::vector<std::pair<TagPath, TagPath>> &reps) {
    size_t count = 0;
    for (unsigned i = 0; i < nfields(n.sid); i++) {
        const TdField &fd = field(n.sid, i);
        if (fd.flags & F_UNREAD) continue;
        Extra &x = n.x[i];
        if (fd.kind == K_DEP) {
            for (auto &[from, to] : reps) {
                if (x.fourcc != from.fourcc || x.path != from.path) continue;
                if (fd.nclasses > 0) {
                    bool ok = false;
                    for (int k = 0; k < fd.nclasses; k++)
                        if (kTdClasses[fd.classes + k] == to.fourcc) ok = true;
                    if (!ok) throw Fail();
                }
                x.fourcc = to.fourcc;
                x.path = to.path;
                count++;
            }
        } else if (fd.kind == K_REFLEXIVE) {
            for (auto &e : x.elems) count += refactor_node(e, reps);
        }
    }
    return count;
}

bool refactor(const std::vector<std::string> &args, std::string &output, int &code) {
    std::vector<fs::path> dirs;
    std::vector<std::pair<TagPath, TagPath>> reps;
    std::vector<std::pair<uint32_t, uint32_t>> groups;
    std::string single;
    bool no_move = false, unsafe = false;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string &a = args[i];
        auto need = [&](size_t n) { if (i + n >= args.size()) throw Fail(); };
        if (a == "-t") { need(1); dirs.push_back(fs::u8path(args[++i])); }
        else if (a == "-M") { need(1); if (args[++i] != "no-move") throw Fail(); no_move = true; }
        else if (a == "-U") unsafe = true;
        else if (a == "-T") { need(2); reps.push_back({split_path(args[i + 1]), split_path(args[i + 2])}); i += 2; }
        else if (a == "-g") {
            need(2);
            uint32_t f = fourcc_of(args[i + 1]), t = fourcc_of(args[i + 2]);
            if (!f || !t) throw Fail();
            groups.push_back({f, t});
            i += 2;
        } else if (a == "-s") { need(1); single = args[++i]; }
        else throw Fail();   // (an option or argument the app doesn't use)
    }
    if (!no_move || dirs.size() != 1) throw Fail();
    const fs::path &dir = dirs[0];
    std::error_code ec;
    if (!unsafe)
        for (auto &[from, to] : reps)
            if (!fs::exists(dir / fs::u8path(preferred(to.path) + "." + extension_of(to.fourcc)), ec)) throw Fail();
    // Every tag in the folder (Invader reads them all to look for the references)
    struct File { fs::path full; std::string rel; uint32_t fourcc; };
    std::vector<File> all;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string ext = it->path().extension().u8string();
        uint32_t cc = ext.size() > 1 ? fourcc_of(ext.substr(1)) : 0;
        if (!cc) continue;
        all.push_back({it->path(), fs::relative(it->path(), dir, ec).u8string(), cc});
    }
    if (ec) throw Fail();
    for (auto &[gf, gt] : groups)
        for (auto &t : all)
            if (t.fourcc == gt) {
                TagPath p = split_path(t.rel);
                reps.push_back({{p.path, gf}, p});
            }
    for (size_t i = 0; i < reps.size();) {
        if (same(reps[i].first, reps[i].second)) reps.erase(reps.begin() + (long)i);
        else i++;
    }
    std::vector<File> todo;
    if (!single.empty()) {
        TagPath s = split_path(preferred(single));
        fs::path f = dir / fs::u8path(preferred(s.path) + "." + extension_of(s.fourcc));
        if (!fs::exists(f, ec)) throw Fail();
        todo.push_back({f, "", s.fourcc});
    } else {
        todo = all;
    }
    std::set<std::string> from_keys;   // (what the replacements rename: a tag with none of them is left alone)
    for (auto &[from, to] : reps) from_keys.insert(std::to_string(from.fourcc) + ":" + from.path);
    size_t total = 0, tags = 0;
    std::string lines;
    for (auto &t : todo) {
        switch (t.fourcc) {
        case 0x6269746Du: case 0x70687973u: case 0x73747223u: case 0x75737472u: case 0x686D7420u:   // bitm phys str# ustr hmt
            continue;
        }
        std::string f = read_file(t.full);
        bool hit = false;
        for (auto &r : scan(f))
            if (!r.unread && from_keys.count(std::to_string(r.fourcc) + ":" + r.path)) hit = true;
        if (!hit) continue;   // (none of its references are renamed; the first one replaced may lead to others)
        Tag tag;
        load(t.full, tag);
        size_t n = refactor_node(tag.root, reps);
        if (!n) continue;
        std::string bytes = file_bytes(tag);
        std::ofstream o(t.full, std::ios::binary | std::ios::trunc);
        if (!o.write(bytes.data(), (std::streamsize)bytes.size())) throw Fail();
        total += n;
        tags++;
        lines += "Replaced " + std::to_string(n) + " reference" + (n == 1 ? "" : "s") + " in " + t.full.u8string() + "\n";
    }
    output = lines + "Replaced " + std::to_string(total) + " reference" + (total == 1 ? "" : "s") + " in " +
             std::to_string(tags) + " tag" + (tags == 1 ? "" : "s") + "\n";
    code = 0;
    return true;
}

}   // namespace

bool run_edit(const std::vector<std::string> &args, std::string &output, int &code) {
    try {
        if (edit(args, output, code)) { g_handled++; return true; }
    } catch (const Fail &) {
    } catch (const std::exception &) {
    }
    g_fallback++;
    return false;
}

bool run_dependency(const std::vector<std::string> &args, std::string &output, int &code) {
    try {
        if (dependency(args, output, code)) { g_handled++; return true; }
    } catch (const Fail &) {
    } catch (const std::exception &) {
    }
    g_fallback++;
    return false;
}

bool run_refactor(const std::vector<std::string> &args, std::string &output, int &code) {
    try {
        if (refactor(args, output, code)) { g_handled++; return true; }
    } catch (const Fail &) {
    } catch (const std::exception &) {
    }
    g_fallback++;
    return false;
}

bool resave(const std::string &in, const std::string &out) {
    try {
        Tag t;
        load(fs::u8path(in), t);
        std::string bytes = file_bytes(t);
        std::ofstream o(fs::u8path(out), std::ios::binary | std::ios::trunc);
        return (bool)o.write(bytes.data(), (std::streamsize)bytes.size());
    } catch (...) {
        return false;
    }
}

Stats stats() { return {g_handled.load(), g_fallback.load()}; }

}   // namespace tagedit
