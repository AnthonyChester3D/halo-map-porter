// Halo Map Porter - map packs (portable part). See mappack.hpp.
#include "mappack.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <set>

#include "bcdec.h"        // implementation in core.cpp
#include "inflate.hpp"

namespace mappack {

const char *const kStockSlots[13] = {"beavercreek", "sidewinder", "damnation", "ratrace", "prisoner", "hangemhigh",
                                     "chillout", "carousel", "boardingaction", "bloodgulch", "wizard", "putput", "longest"};
const char *const kExpandedSlots[27] = {"chillout", "hangemhigh", "prisoner", "dammy", "derelict", "downrush", "badcreek",
                                        "decidia", "doubletake", "redshift", "outbnd", "exhibit", "hotbox", "ratrace",
                                        "zerohour", "wizard", "atlas", "bloodgulch", "boardingaction", "imminent",
                                        "sidewinder", "longest", "putput", "madhouse", "calamity", "overflow", "tinker"};

namespace {

uint32_t rd32(const std::string &d, size_t o) {
    if (o + 4 > d.size()) return 0;
    const uint8_t *p = (const uint8_t *)d.data() + o;
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}
uint16_t rd16(const std::string &d, size_t o) {
    if (o + 2 > d.size()) return 0;
    const uint8_t *p = (const uint8_t *)d.data() + o;
    return (uint16_t)(p[0] | p[1] << 8);
}
std::string lower(std::string s) {
    for (auto &c : s) c = (char)tolower((unsigned char)c);
    return s;
}
bool read_file(const fs::path &p, std::string &out, size_t limit = 0) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    if (limit) {
        out.resize(limit);
        f.read(&out[0], (std::streamsize)limit);
        out.resize((size_t)f.gcount());
        return true;
    }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// ---------------------------------------------------------------- an Xbox cache file (ui.map)
constexpr uint32_t kBase = 0x803A6000;   // where the Xbox loads tag data
struct Tag { std::string cls, name; uint32_t id = 0, data = 0; };
struct Cache {
    std::string d;
    uint32_t tdo = 0, tds = 0;
    std::vector<Tag> tags;
    std::map<uint32_t, size_t> by_id;
    size_t off(uint32_t v) const { return v >= kBase && v - kBase < tds ? tdo + (v - kBase) : std::string::npos; }
    bool ok(size_t o, size_t n) const { return o != std::string::npos && o + n <= d.size(); }
    const Tag *tag(uint32_t id) const { auto it = by_id.find(id); return it == by_id.end() ? nullptr : &tags[it->second]; }
};

bool load_cache(const fs::path &p, Cache &c, std::string &err) {
    std::string raw;
    if (!read_file(p, raw)) { err = "Couldn't read " + p.u8string(); return false; }
    if (raw.size() < 0x800 || raw.compare(0, 4, "daeh") != 0) { err = p.filename().u8string() + " isn't a Halo map file"; return false; }
    if (rd32(raw, 4) != 5) { err = p.filename().u8string() + " isn't an Xbox map (version " + std::to_string(rd32(raw, 4)) + ")"; return false; }
    uint32_t size = rd32(raw, 8);
    if (size != raw.size()) {   // Xbox maps are usually zlib-compressed after the 2 KB header
        std::string body;
        if (!inflate::zlib((const uint8_t *)raw.data() + 0x800, raw.size() - 0x800, body, size > 0x800 ? size - 0x800 : 0)) {
            err = "Couldn't decompress " + p.filename().u8string(); return false;
        }
        c.d = raw.substr(0, 0x800) + body;
    } else {
        c.d.swap(raw);
    }
    c.tdo = rd32(c.d, 0x10); c.tds = rd32(c.d, 0x14);
    if ((size_t)c.tdo + c.tds > c.d.size() || c.tds < 0x28) { err = "Damaged map header in " + p.filename().u8string(); return false; }
    uint32_t arr = rd32(c.d, c.tdo), count = rd32(c.d, c.tdo + 12);
    size_t ao = c.off(arr);
    if (!c.ok(ao, (size_t)count * 32) || count > 65535) { err = "Damaged tag index in " + p.filename().u8string(); return false; }
    for (uint32_t i = 0; i < count; i++) {
        size_t e = ao + i * 32;
        Tag t;
        char cls[5] = {c.d[e + 3], c.d[e + 2], c.d[e + 1], c.d[e], 0};
        t.cls = cls;
        t.id = rd32(c.d, e + 12);
        size_t no = c.off(rd32(c.d, e + 16));
        if (no != std::string::npos) { size_t z = c.d.find('\0', no); t.name = c.d.substr(no, std::min(z, no + 256) - no); }
        t.data = rd32(c.d, e + 20);
        c.by_id[t.id] = c.tags.size();
        c.tags.push_back(t);
    }
    return true;
}

// bitmap tag: sequences at 0x54, bitmaps at 0x60 (count, pointer); a bitmap is 0x30 bytes
struct Frame { int w = 0, h = 0, format = 0, flags = 0; uint32_t offset = 0, size = 0; };
std::vector<Frame> frames(const Cache &c, const Tag &t) {
    std::vector<Frame> out;
    size_t o = c.off(t.data);
    if (!c.ok(o, 0x6c)) return out;
    uint32_t nseq = rd32(c.d, o + 0x54), nb = rd32(c.d, o + 0x60);
    size_t so = c.off(rd32(c.d, o + 0x58)), bo = c.off(rd32(c.d, o + 0x64));
    if (!c.ok(bo, (size_t)nb * 0x30) || nb > 4096) return out;
    std::vector<int> order;
    if (nseq && c.ok(so, 64)) {   // sequence 0 decides which bitmap each frame index is
        int first = (int16_t)rd16(c.d, so + 0x20), n = (int16_t)rd16(c.d, so + 0x22);
        for (int k = 0; k < n; k++) if (first + k >= 0 && first + k < (int)nb) order.push_back(first + k);
    }
    if (order.empty()) for (uint32_t k = 0; k < nb; k++) order.push_back((int)k);
    for (int k : order) {
        size_t e = bo + (size_t)k * 0x30;
        Frame f;
        f.w = rd16(c.d, e + 4); f.h = rd16(c.d, e + 6); f.format = rd16(c.d, e + 0xc); f.flags = rd16(c.d, e + 0xe);
        f.offset = rd32(c.d, e + 0x18); f.size = rd32(c.d, e + 0x1c);
        out.push_back(f);
    }
    return out;
}
// unicode string list: (count, pointer) at 0; each string is a 20-byte data reference (size, -, -, pointer, -)
std::vector<std::string> strings(const Cache &c, const Tag &t) {
    std::vector<std::string> out;
    size_t o = c.off(t.data);
    if (!c.ok(o, 12)) return out;
    uint32_t n = rd32(c.d, o);
    size_t so = c.off(rd32(c.d, o + 4));
    if (!c.ok(so, (size_t)n * 20) || n > 4096) return out;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t size = rd32(c.d, so + i * 20);
        size_t p = c.off(rd32(c.d, so + i * 20 + 12));
        std::u32string s;
        if (c.ok(p, size))
            for (uint32_t k = 0; k + 1 < size; k += 2) {
                char32_t ch = rd16(c.d, p + k);
                if (!ch) break;
                s += ch;
            }
        out.push_back(mapcard::u32_to_utf8(s));
    }
    return out;
}
// ui widget: background bitmap at 0x38, text strings at 0xEC (tag references: class, name, -, id), children
// at 0x3E0 (count, pointer; 0x50 bytes each, the widget reference first)
uint32_t ref_id(const Cache &c, const Tag &t, size_t at) { size_t o = c.off(t.data); return c.ok(o, at + 16) ? rd32(c.d, o + at + 12) : 0; }
std::vector<uint32_t> children(const Cache &c, const Tag &t) {
    std::vector<uint32_t> out;
    size_t o = c.off(t.data);
    if (!c.ok(o, 0x3ec)) return out;
    uint32_t n = rd32(c.d, o + 0x3e0);
    size_t co = c.off(rd32(c.d, o + 0x3e4));
    if (!c.ok(co, (size_t)n * 0x50) || n > 64) return out;
    for (uint32_t i = 0; i < n; i++) out.push_back(rd32(c.d, co + i * 0x50 + 12));
    return out;
}
bool is_map_pictures(const Cache &c, const Tag *t) {
    if (!t || t->cls != "bitm") return false;
    auto f = frames(c, *t);
    return f.size() >= 13 && std::all_of(f.begin(), f.begin() + 13, [](const Frame &x) { return x.w >= 64 && x.h >= 32; });
}

// ---------------------------------------------------------------- pictures
uint32_t unswizzle(int x, int y, int w, int h) {   // Xbox "swizzled" (Morton order) textures
    uint32_t o = 0, bit = 0;
    for (int m = 1; m < w || m < h; m <<= 1) {
        if (m < w) o |= (uint32_t)((x & m) ? 1 : 0) << bit++;
        if (m < h) o |= (uint32_t)((y & m) ? 1 : 0) << bit++;
    }
    return o;
}
bool decode_frame(const Cache &c, const Frame &f, std::vector<uint8_t> &rgba) {
    int w = f.w, h = f.h;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || !c.ok(f.offset, 1)) return false;
    const uint8_t *src = (const uint8_t *)c.d.data() + f.offset;
    size_t avail = c.d.size() - f.offset;
    rgba.assign((size_t)w * h * 4, 255);
    auto blocks = [&](int bytes, void (*dec)(const void *, void *, int)) {
        size_t need = (size_t)((w + 3) / 4) * ((h + 3) / 4) * bytes;
        if (avail < need) return false;
        std::vector<uint8_t> tmp((size_t)((w + 3) & ~3) * ((h + 3) & ~3) * 4);
        int tw = (w + 3) & ~3;
        size_t k = 0;
        for (int by = 0; by < h; by += 4)
            for (int bx = 0; bx < w; bx += 4, k += bytes) dec(src + k, &tmp[((size_t)by * tw + bx) * 4], tw * 4);
        for (int y = 0; y < h; y++) memcpy(&rgba[(size_t)y * w * 4], &tmp[(size_t)y * tw * 4], (size_t)w * 4);
        return true;
    };
    bool swz = (f.flags & 8) != 0;
    auto px = [&](int x, int y) { return swz ? unswizzle(x, y, w, h) : (uint32_t)(y * w + x); };
    switch (f.format) {
    case 14: return blocks(8, bcdec_bc1);                     // DXT1
    case 15: return blocks(16, bcdec_bc2);                    // DXT3
    case 16: return blocks(16, bcdec_bc3);                    // DXT5
    case 10: case 11:                                         // X8R8G8B8, A8R8G8B8
        if (avail < (size_t)w * h * 4) return false;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const uint8_t *s = src + px(x, y) * 4; uint8_t *d = &rgba[((size_t)y * w + x) * 4];
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = f.format == 11 ? s[3] : 255;
            }
        return true;
    case 6: case 8: case 9:                                   // R5G6B5, A1R5G5B5, A4R4G4B4
        if (avail < (size_t)w * h * 2) return false;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint32_t v = src[px(x, y) * 2] | src[px(x, y) * 2 + 1] << 8;
                uint8_t *d = &rgba[((size_t)y * w + x) * 4];
                if (f.format == 6) { d[0] = (v >> 11 & 31) * 255 / 31; d[1] = (v >> 5 & 63) * 255 / 63; d[2] = (v & 31) * 255 / 31; }
                else if (f.format == 8) { d[0] = (v >> 10 & 31) * 255 / 31; d[1] = (v >> 5 & 31) * 255 / 31; d[2] = (v & 31) * 255 / 31; d[3] = v & 0x8000 ? 255 : 0; }
                else { d[0] = (v >> 8 & 15) * 17; d[1] = (v >> 4 & 15) * 17; d[2] = (v & 15) * 17; d[3] = (v >> 12 & 15) * 17; }
            }
        return true;
    }
    return false;
}
// Halo shows the top-left 140x114 of a 256x128 picture: the same share of whatever size the pack used
std::string to_preview(const Cache &c, const Frame &f) {
    if (f.w == 256 && f.h == 128 && f.format == 14 && c.ok(f.offset, 16384)) return c.d.substr(f.offset, 16384);
    std::vector<uint8_t> rgba;
    if (!decode_frame(c, f, rgba)) return "";
    double vw = f.w * 140.0 / 256, vh = f.h * 114.0 / 128;
    std::vector<uint8_t> pic((size_t)mapcard::PIC_W * mapcard::PIC_H * 4);
    for (int y = 0; y < mapcard::PIC_H; y++)
        for (int x = 0; x < mapcard::PIC_W; x++) {   // box filter over the source pixels this one covers
            double x0 = x * vw / mapcard::PIC_W, x1 = (x + 1) * vw / mapcard::PIC_W, y0 = y * vh / mapcard::PIC_H, y1 = (y + 1) * vh / mapcard::PIC_H;
            double acc[4] = {0, 0, 0, 0}, wsum = 0;
            for (int sy = (int)y0; sy < (int)std::ceil(y1) && sy < f.h; sy++)
                for (int sx = (int)x0; sx < (int)std::ceil(x1) && sx < f.w; sx++) {
                    double wgt = (std::min(x1, sx + 1.0) - std::max(x0, (double)sx)) * (std::min(y1, sy + 1.0) - std::max(y0, (double)sy));
                    const uint8_t *s = &rgba[((size_t)sy * f.w + sx) * 4];
                    for (int k = 0; k < 4; k++) acc[k] += s[k] * wgt;
                    wsum += wgt;
                }
            uint8_t *d = &pic[((size_t)y * mapcard::PIC_W + x) * 4];
            for (int k = 0; k < 4; k++) d[k] = wsum > 0 ? (uint8_t)std::lround(acc[k] / wsum) : 0;
            d[3] = 255;
        }
    return mapcard::preview_bin(pic);
}

std::string one_line(std::string s) {   // a name with Halo line breaks in it
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        char ch = s[i];
        if (ch == '\r') continue;
        if (ch == '\n') { if (!o.empty() && o.back() != ' ') o += ' '; continue; }
        o += ch;
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    while (!o.empty() && o[0] == ' ') o.erase(0, 1);
    return o;
}
std::string description(std::string s) {
    std::string o;
    for (char ch : s) if (ch != '\r') o += ch;
    while (!o.empty() && (o.back() == '\n' || o.back() == ' ')) o.pop_back();
    return o;
}
std::string pretty(const std::string &stem) {   // "cex_museum" -> "Cex Museum"
    std::string o;
    bool up = true;
    for (char ch : stem) {
        if (ch == '_' || ch == '-') { o += ' '; up = true; continue; }
        o += up ? (char)toupper((unsigned char)ch) : ch;
        up = false;
    }
    return o;
}
fs::path find_file(const fs::path &dir, const std::string &name) {   // case-insensitive (for Linux)
    std::error_code ec;
    fs::path p = dir / name;
    if (fs::exists(p, ec)) return p;
    for (auto &e : fs::directory_iterator(dir, ec))
        if (lower(e.path().filename().u8string()) == lower(name)) return e.path();
    return {};
}

// ---------------------------------------------------------------- the pack's map list (its default.xbe)
// Halo's multiplayer maps are a table of scenario names in the XBE ("levels\test\beavercreek\beavercreek", ...).
// A map's place in that table is its slot: its picture, name and description in ui.map are entry `slot` of the
// menu's lists, and the map file has the scenario's last name. Stock Halo has 13; Halo 1 NHE (and the packs
// built on it) has 30 (27 maps and 3 spares). Returns the names in table order, or nothing.
bool same_ci(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}
std::vector<std::string> xbe_map_table(const fs::path &file) {
    std::string d;
    std::vector<std::string> best;
    if (!read_file(file, d) || d.size() < 0x200 || d.compare(0, 4, "XBEH") != 0) return best;
    uint32_t base = rd32(d, 0x104), nsec = rd32(d, 0x11c), sh = rd32(d, 0x120) - base;
    struct Sec { uint32_t va, raw, size; };
    std::vector<Sec> secs;
    for (uint32_t i = 0; i < nsec && i < 64 && (size_t)sh + i * 56 + 24 <= d.size(); i++) {
        size_t o = sh + (size_t)i * 56;
        secs.push_back({rd32(d, o + 4), rd32(d, o + 12), rd32(d, o + 16)});
    }
    auto map_name = [&](uint32_t v) -> std::string {   // the string at address v, if it's "levels\test\X\X": "X"
        if (v < base) return "";
        for (auto &sc : secs) {
            if (v < sc.va || v - sc.va >= sc.size) continue;
            size_t f = (size_t)sc.raw + (v - sc.va);
            if (f + 16 > d.size() || !same_ci(d.data() + f, "levels\\test\\", 12)) return "";
            const char *z = (const char *)memchr(d.data() + f, 0, std::min<size_t>(128, d.size() - f));
            if (!z) return "";
            std::string str = lower(std::string((const char *)d.data() + f + 12, z));
            size_t k = str.find('\\');
            if (k == std::string::npos || k == 0) return "";
            std::string n = str.substr(0, k);
            return str.substr(k + 1) == n ? n : "";
        }
        return "";
    };
    std::vector<std::string> run;
    for (size_t o = 0; o + 4 <= d.size(); o += 4) {
        std::string n = map_name(rd32(d, o));
        if (!n.empty()) { run.push_back(n); continue; }
        if (run.size() > best.size()) best = run;
        run.clear();
    }
    if (run.size() > best.size()) best = run;
    if (best.size() < 4) best.clear();
    return best;
}

// ---------------------------------------------------------------- the pack's logo on its main menu
// Mod packs usually swap the HALO logo on the main menu for their own. The menu is the widget
// ui\shell\main_menu\main_menu (the XBE and Halo open it by that name); its children draw the logo, the
// buttons and the backdrop. Pictures that look like a logo are offered, best first.
constexpr uint64_t kStockLogoHash = 0xa445b303f9e75e59ull;   // FNV-1a of the stock halo_logo's 1024x256 DXT5 pixels
uint64_t fnv1a(const char *p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)p[i]) * 0x100000001b3ull;
    return h;
}
bool has_any(const std::string &s, std::initializer_list<const char *> words) {
    for (auto w : words) if (s.find(w) != std::string::npos) return true;
    return false;
}
struct LogoFinder {
    const Cache &c;
    std::vector<std::pair<int, MenuLogo>> found;   // score, logo
    std::set<uint32_t> seen;
    bool stock = false;                            // the menu still shows the HALO logo
    explicit LogoFinder(const Cache &cc) : c(cc) {}

    // widget: type at 0 (2/3 = lists), bounds at 0x24 (top, left, bottom, right in 640x480)
    void look(const Tag *widget, const Tag &b, bool named_only) {   // widget null = not on the menu itself
        if (b.cls != "bitm" || !seen.insert(b.id).second) return;
        auto fr = frames(c, b);
        if (fr.empty() || !c.ok(fr[0].offset, fr[0].size)) return;
        const Frame &f = fr[0];
        if (fnv1a(c.d.data() + f.offset, f.size) == kStockLogoHash) { stock = true; return; }
        std::string names = lower(b.name + " " + (widget ? widget->name : std::string()));
        bool logo = has_any(names, {"logo", "title", "banner", "emblem"});
        if (named_only && !logo) return;
        if (!logo && has_any(names, {"button", "cursor", "arrow", "highlight", "background", "select", "overscan", "vignette"})) return;
        std::vector<uint8_t> px;
        if (!decode_frame(c, f, px)) return;
        int w = f.w, h = f.h;
        bool opaque = true;
        for (size_t i = 3; i < px.size() && opaque; i += 4) opaque = px[i] >= 250;
        if (opaque)   // no alpha: drawn added onto the menu, so black is see-through
            for (size_t i = 0; i < px.size(); i += 4) {
                int a = std::max(px[i], std::max(px[i + 1], px[i + 2]));
                for (int k = 0; k < 3; k++) px[i + k] = a ? (uint8_t)(px[i + k] * 255 / a) : 0;
                px[i + 3] = (uint8_t)a;
            }
        int x0 = w, y0 = h, x1 = -1, y1 = -1;
        double ink = 0;
        std::vector<int> hist(256, 0);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int a = px[((size_t)y * w + x) * 4 + 3];
                if (a < 16) continue;
                x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
                ink += a / 255.0;
                hist[a]++;
            }
        if (x1 < 0 || ink < 150) return;   // empty, or a few faint specks
        double fw = (x1 - x0 + 1.0) / w, fh = (y1 - y0 + 1.0) / h;
        bool screen = false;
        if (size_t o = widget ? c.off(widget->data) : std::string::npos; c.ok(o, 0x2c)) {
            int top = (int16_t)rd16(c.d, o + 0x24), left = (int16_t)rd16(c.d, o + 0x26);
            int bottom = (int16_t)rd16(c.d, o + 0x28), right = (int16_t)rd16(c.d, o + 0x2a);
            screen = right - left >= 600 && bottom - top >= 440;
        }
        if (!logo && (fw > 0.85 && fh > 0.85)) return;              // a backdrop or a frame
        if (!logo && screen && fw > 0.5 && fh > 0.5) return;        // a full-screen overlay
        int score = (logo ? 1000 : 0) + (screen ? -200 : 100) + (int)std::min(300.0, ink / 50) - (named_only ? 500 : 0);

        // cut it out (2 pixels spare) and make the strongest strokes solid: menus often draw logos faint
        // and brighten them with render effects, which a small stamp doesn't have
        int total = 0, p99 = 255;
        for (int a = 16; a < 256; a++) total += hist[a];
        for (int a = 255, run = 0; a >= 16; a--) { run += hist[a]; if (run * 100 >= total) { p99 = a; break; } }
        x0 = std::max(0, x0 - 2); y0 = std::max(0, y0 - 2); x1 = std::min(w - 1, x1 + 2); y1 = std::min(h - 1, y1 + 2);
        MenuLogo m;
        m.tag = b.name;
        m.widget = widget ? widget->name : "";
        m.faded = p99 < 200;
        m.image.w = x1 - x0 + 1; m.image.h = y1 - y0 + 1;
        m.image.rgba.resize((size_t)m.image.w * m.image.h * 4);
        for (int y = 0; y < m.image.h; y++)
            for (int x = 0; x < m.image.w; x++) {
                const uint8_t *s = &px[((size_t)(y + y0) * w + x + x0) * 4];
                uint8_t *d = &m.image.rgba[((size_t)y * m.image.w + x) * 4];
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
                d[3] = (uint8_t)std::min(255, s[3] * 255 / std::max(1, p99));
            }
        found.push_back({score, m});
    }
    void walk(const Tag &t, int depth) {
        if (t.cls != "DeLa" || depth > 4 || !seen.insert(t.id).second) return;
        size_t o = c.off(t.data);
        if (!c.ok(o, 0x3ec)) return;
        if (const Tag *b = c.tag(ref_id(c, t, 0x38))) look(&t, *b, false);
        int type = (int16_t)rd16(c.d, o);
        if (type == 2 || type == 3) return;   // a list: its items are the menu's buttons
        for (uint32_t id : children(c, t))
            if (const Tag *k = c.tag(id)) walk(*k, depth + 1);
    }
};
void find_menu_logos(const Cache &c, Pack &out) {
    LogoFinder lf(c);
    const Tag *root = nullptr;   // ui\shell\main_menu\main_menu, else any widget called main_menu
    for (auto &t : c.tags) {
        if (t.cls != "DeLa") continue;
        std::string n = lower(t.name);
        if (n.size() >= 19 && n.compare(n.size() - 19, 19, "main_menu\\main_menu") == 0) { root = &t; break; }
        if (!root && n.compare(n.rfind('\\') + 1, std::string::npos, "main_menu") == 0) root = &t;
    }
    if (root) lf.walk(*root, 0);
    for (auto &t : c.tags)   // logos the menu doesn't point at directly (in a widget it opens later, say)
        if (t.cls == "bitm" && lower(t.name).find("logo") != std::string::npos) lf.look(nullptr, t, true);
    std::stable_sort(lf.found.begin(), lf.found.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    for (auto &f : lf.found) {
        if (out.menu_logos.size() >= 6) break;
        out.menu_logos.push_back(f.second);
    }
    if (!out.menu_logos.empty())
        out.info.push_back("Menu logo: " + out.menu_logos[0].tag + (out.menu_logos.size() > 1 ? " (+" + std::to_string(out.menu_logos.size() - 1) + " more)" : ""));
    else
        out.info.push_back(lf.stock ? "Menu logo: the main menu still has the HALO logo" : "Menu logo: none found");
}
} // namespace

// ---------------------------------------------------------------- reading a pack
bool read_pack(const fs::path &folder, Pack &out, std::string &err) {
    out = Pack();
    std::error_code ec;
    fs::path maps = folder;
    if (find_file(maps, "ui.map").empty()) {
        fs::path m = find_file(folder, "maps");
        if (!m.empty() && !find_file(m, "ui.map").empty()) maps = m;
    }
    fs::path ui = find_file(maps, "ui.map");
    if (ui.empty()) { err = "No ui.map in " + folder.u8string() + " or its maps folder"; return false; }
    out.maps_dir = maps;
    fs::path named = lower(maps.filename().u8string()) == "maps" ? maps.parent_path() : maps;
    out.suggested_name = clean_name(named.filename().u8string(), 12);
    Cache c;
    if (!load_cache(ui, c, err)) return false;

    // The map select list's items each have 3 child widgets: name (text), picture (bitmap), description (text).
    // Find one whose picture is a bitmap with a frame per stock map.
    const Tag *pics = nullptr, *names = nullptr, *descs = nullptr;
    for (auto &t : c.tags) {
        if (t.cls != "DeLa") continue;
        auto ch = children(c, t);
        for (size_t k = 1; k + 1 < ch.size() && !pics; k++) {
            const Tag *w = c.tag(ch[k]), *wn = c.tag(ch[k - 1]), *wd = c.tag(ch[k + 1]);
            if (!w || !wn || !wd || w->cls != "DeLa" || wn->cls != "DeLa" || wd->cls != "DeLa") continue;
            const Tag *b = c.tag(ref_id(c, *w, 0x38)), *n = c.tag(ref_id(c, *wn, 0xec)), *d = c.tag(ref_id(c, *wd, 0xec));
            if (!is_map_pictures(c, b) || !n || !d || n->cls != "ustr" || d->cls != "ustr") continue;
            pics = b; names = n; descs = d;
            out.info.push_back("Menu: " + t.name);
        }
        if (pics) break;
    }
    if (!pics) {   // no such widget: go by shape (a 13+ picture bitmap, a 13+ name list, a 13+ description list)
        for (auto &t : c.tags) if (!pics && is_map_pictures(c, &t) && lower(t.name).find("sp_") == std::string::npos) pics = &t;
        int best_n = -1, best_d = -1;
        for (auto &t : c.tags) {
            if (t.cls != "ustr") continue;
            auto s = strings(c, t);
            if (s.size() < 13) continue;
            int breaks = 0, maps_word = lower(t.name).find("map") != std::string::npos;
            for (size_t k = 0; k < 13; k++) breaks += (int)std::count(s[k].begin(), s[k].end(), '\n');
            int ns = maps_word * 100 - breaks, ds = maps_word * 100 + breaks;
            if (breaks < 13 && ns > best_n) { best_n = ns; names = &t; }
            if (breaks >= 13 && ds > best_d) { best_d = ds; descs = &t; }
        }
        out.info.push_back("No map select menu found in ui.map; going by what the tags look like");
    }
    if (!pics) { err = "ui.map has no map pictures (a bitmap with a picture for each of the 13 stock maps)"; return false; }
    out.info.push_back("Pictures: " + pics->name);
    if (names) out.info.push_back("Names: " + names->name);
    if (descs) out.info.push_back("Descriptions: " + descs->name);
    auto fr = frames(c, *pics);
    auto nm = names ? strings(c, *names) : std::vector<std::string>();
    auto ds = descs ? strings(c, *descs) : std::vector<std::string>();

    // Which map each menu entry is: the pack's own XBE says (its map table); without one, stock Halo's 13
    // or NHE's 27, whichever the maps here and the size of the menu's lists fit
    std::vector<std::string> slots;
    for (const fs::path &dir : {maps.parent_path(), maps}) {
        fs::path x = find_file(dir, "default.xbe");
        if (x.empty() || !slots.empty()) continue;
        slots = xbe_map_table(x);
        if (!slots.empty())
            out.info.push_back("Map list: " + std::to_string(slots.size()) + " slots in " +
                               (dir == maps ? x.filename() : x.parent_path().filename() / x.filename()).u8string());
    }
    if (slots.empty()) {
        auto present = [&](const char *const *list, int n) {
            int k = 0;
            for (int i = 0; i < n; i++) k += !find_file(maps, std::string(list[i]) + ".map").empty();
            return k;
        };
        bool expanded = std::min(nm.size(), fr.size()) >= 27 && present(kExpandedSlots, 27) > present(kStockSlots, 13);
        if (expanded) slots.assign(kExpandedSlots, kExpandedSlots + 27);
        else slots.assign(kStockSlots, kStockSlots + 13);
        out.info.push_back(expanded ? "Map list: no default.xbe here; the menu and maps fit the 27-map list (Halo 1 NHE)"
                                    : "Map list: no default.xbe here; using stock Halo's 13");
    }
    if (slots.size() > 13 && std::min(nm.size(), fr.size()) < 14)
        out.info.push_back("The XBE lists " + std::to_string(slots.size()) + " maps but ui.map only has pictures and names for " +
                           std::to_string(std::min(nm.size(), fr.size())));

    std::set<std::string> used;
    for (int i = 0; i < (int)slots.size(); i++) {
        fs::path f = find_file(maps, slots[i] + ".map");
        if (f.empty() || used.count(lower(f.filename().u8string()))) continue;   // a spare slot, or no map for it
        Entry e;
        e.slot = slots[i];
        e.map_file = f;
        e.name = i < (int)nm.size() ? one_line(nm[i]) : "";
        if (e.name.empty()) e.name = pretty(e.slot);
        e.desc = i < (int)ds.size() ? description(ds[i]) : "";
        e.preview = i < (int)fr.size() ? to_preview(c, fr[i]) : "";
        if (e.preview.empty()) e.note = "no picture";
        used.insert(lower(f.filename().u8string()));
        out.maps.push_back(e);
    }
    std::vector<fs::path> extra;
    for (auto &de : fs::directory_iterator(maps, ec)) {
        std::string n = lower(de.path().filename().u8string());
        if (de.is_regular_file(ec) && de.path().extension() != "" && lower(de.path().extension().u8string()) == ".map" &&
            n != "ui.map" && !used.count(n))
            extra.push_back(de.path());
    }
    std::sort(extra.begin(), extra.end());
    for (auto &f : extra) {
        Entry e;
        e.map_file = f;
        e.name = pretty(f.stem().u8string());
        std::string hdr;
        read_file(f, hdr, 0x800);
        uint32_t type = rd32(hdr, 0x60);
        bool halo = hdr.size() >= 0x800 && hdr.compare(0, 4, "daeh") == 0 && rd32(hdr, 4) == 5;
        e.include = halo && type == 1;
        e.note = !halo ? "not an Xbox Halo map" : type == 0 ? "a campaign map (not for the multiplayer list)"
               : type == 2 ? "a menu map" : "not in the pack's menu: no picture or description";
        out.maps.push_back(e);
    }
    if (out.maps.empty()) { err = "No maps next to ui.map in " + maps.u8string(); return false; }
    find_menu_logos(c, out);
    return true;
}

// ---------------------------------------------------------------- names
static bool fatx_char(unsigned char c) {
    if (isalnum(c)) return true;
    return c && strchr(" !#$%&'()-.@[]^_`{}~", c) != nullptr;
}
std::string clean_name(const std::string &name, size_t max) {
    // Latin-1 letters lose their accents; anything else the Xbox drive can't store is dropped
    static const char *fold = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    std::u32string u = mapcard::utf8_to_u32(name);
    std::string o;
    for (char32_t ch : u) {
        if (ch >= 0xC0 && ch <= 0xFF) ch = (unsigned char)fold[ch - 0xC0];
        if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
        if (ch < 128 && fatx_char((unsigned char)ch)) {
            if (ch == ' ' && (o.empty() || o.back() == ' ')) continue;
            if (ch == '.' && !o.empty() && o.back() == '.') continue;   // ".." isn't allowed
            o += (char)ch;
        }
    }
    size_t h;
    while ((h = lower(o).find(".hmp_")) != std::string::npos) o.erase(h, 1);   // the XBE skips those folders
    if (o.size() > max) o.resize(max);
    while (!o.empty() && (o.back() == ' ' || o.back() == '.')) o.pop_back();
    while (!o.empty() && (o[0] == ' ' || o[0] == '.')) o.erase(0, 1);
    return o;
}
void assign_folders(Pack &p, const std::string &pack_name) {
    size_t max = KEY_MAX > pack_name.size() + 1 ? KEY_MAX - pack_name.size() - 1 : 1;
    std::set<std::string> taken;
    for (auto &e : p.maps) {
        if (!e.include) continue;
        std::string base = clean_name(e.name, max);
        if (base.empty()) base = clean_name(e.slot.empty() ? e.map_file.stem().u8string() : e.slot, max);
        if (base.empty()) base = "Map";
        std::string f = base;
        for (int n = 2; taken.count(lower(f)); n++) {
            std::string suffix = " " + std::to_string(n);
            f = clean_name(base, max > suffix.size() ? max - suffix.size() : 1) + suffix;
        }
        taken.insert(lower(f));
        e.folder = f;
    }
}
std::string pack_name_problem(const std::string &pack_name, const Pack &p) {
    if (pack_name.empty()) return "Needs a name";
    if (clean_name(pack_name, KEY_MAX) != pack_name) return "The Xbox drive can't store some of these characters";
    if (pack_name.size() > 20) return "Too long: keep it to 20 characters so the map names fit";
    for (int i = 0; i < 13; i++) if (lower(pack_name) == kStockSlots[i]) return "That's a stock map's name";
    (void)p;
    return "";
}
std::string folder_problem(const Pack &p, const std::string &pack_name, size_t index) {
    const Entry &e = p.maps[index];
    if (!e.include) return "";
    size_t max = KEY_MAX > pack_name.size() + 1 ? KEY_MAX - pack_name.size() - 1 : 0;
    if (e.folder.empty()) return "Needs a folder name";
    if (clean_name(e.folder, KEY_MAX) != e.folder) return "The Xbox drive can't store some of these characters";
    if (e.folder.size() > max) return "Too long: the pack's name and this together can be 29 characters";
    for (size_t k = 0; k < p.maps.size(); k++)
        if (k != index && p.maps[k].include && lower(p.maps[k].folder) == lower(e.folder)) return "Two maps have this folder name";
    return "";
}

// ---------------------------------------------------------------- series.bin
Logo fit_logo(const mapcard::Image &img, int box, bool keep_strokes) {
    Logo l;
    if (img.w <= 0 || img.h <= 0) return l;
    box = std::max(4, std::min(64, box));
    double s = std::min((double)box / img.w, (double)box / img.h);
    l.w = std::max(1, (int)std::lround(img.w * s)); l.h = std::max(1, (int)std::lround(img.h * s));
    l.w = std::min(l.w, 64); l.h = std::min(l.h, 64);
    l.rgba.assign((size_t)l.w * l.h * 4, 0);
    for (int y = 0; y < l.h; y++)
        for (int x = 0; x < l.w; x++) {   // area average (enlarging: nearest), colours weighted by alpha
            double x0 = x * (double)img.w / l.w, x1 = (x + 1) * (double)img.w / l.w;
            double y0 = y * (double)img.h / l.h, y1 = (y + 1) * (double)img.h / l.h;
            double acc[4] = {0, 0, 0, 0}, wsum = 0;
            for (int sy = (int)y0; sy < std::max((int)y0 + 1, (int)std::ceil(y1)) && sy < img.h; sy++)
                for (int sx = (int)x0; sx < std::max((int)x0 + 1, (int)std::ceil(x1)) && sx < img.w; sx++) {
                    double wgt = std::max(1e-6, (std::min(x1, sx + 1.0) - std::max(x0, (double)sx))) *
                                 std::max(1e-6, (std::min(y1, sy + 1.0) - std::max(y0, (double)sy)));
                    const uint8_t *q = &img.rgba[((size_t)sy * img.w + sx) * 4];
                    double a = q[3] / 255.0;
                    for (int k = 0; k < 3; k++) acc[k] += q[k] * a * wgt;
                    acc[3] += a * wgt;
                    wsum += wgt;
                }
            uint8_t *d = &l.rgba[((size_t)y * l.w + x) * 4];
            double a = wsum > 0 ? acc[3] / wsum : 0;
            for (int k = 0; k < 3; k++) d[k] = acc[3] > 0 ? (uint8_t)std::lround(std::min(255.0, acc[k] / acc[3])) : 0;
            d[3] = (uint8_t)std::lround(a * 255);
        }
    if (keep_strokes) {   // lines thinner than a pixel once shrunk fade out: give them back their strength
        auto p99 = [](const std::vector<uint8_t> &px) {
            std::vector<int> hist(256, 0);
            int total = 0;
            for (size_t i = 3; i < px.size(); i += 4) if (px[i] >= 16) { hist[px[i]]++; total++; }
            for (int a = 255, run = 0; a >= 16; a--) if ((run += hist[a]) * 100 >= total) return a;
            return 0;
        };
        int want = p99(img.rgba), got = p99(l.rgba);
        if (got > 0 && want > got) {
            double k = std::min(4.0, (double)want / got);
            for (size_t i = 3; i < l.rgba.size(); i += 4) l.rgba[i] = (uint8_t)std::min(255.0, std::round(l.rgba[i] * k));
        }
    }
    return l;
}
std::string series_bin(const Logo &l, Corner corner, int margin) {
    if (l.w <= 0 || l.h <= 0) return "";
    int x = (corner == BOTTOM_RIGHT || corner == TOP_RIGHT) ? mapcard::PIC_W - margin - l.w : margin;
    int y = (corner == BOTTOM_RIGHT || corner == BOTTOM_LEFT) ? mapcard::PIC_H - margin - l.h : margin;
    x = std::max(0, std::min(x, 256 - l.w)); y = std::max(0, std::min(y, 128 - l.h));
    std::string s = "HUSB";
    auto u16 = [&](int v) { s += (char)(v & 255); s += (char)(v >> 8 & 255); };
    u16(l.w); u16(l.h); u16(x); u16(y);
    s.append(4, '\0');
    for (size_t i = 0; i < l.rgba.size(); i += 4) {
        s += (char)l.rgba[i + 2]; s += (char)l.rgba[i + 1]; s += (char)l.rgba[i]; s += (char)l.rgba[i + 3];
    }
    return s;
}

// the XBE's stamp (maplist.c: series_stamp), line for line, so the preview here is what the Xbox shows
namespace {
void rgb565(uint32_t c, int *o) { o[0] = (c >> 11 & 31) * 255 / 31; o[1] = (c >> 5 & 63) * 255 / 63; o[2] = (c & 31) * 255 / 31; }
uint32_t to565(const int *c) { return (uint32_t)((c[0] * 31 + 127) / 255) << 11 | (uint32_t)((c[1] * 63 + 127) / 255) << 5 | (uint32_t)((c[2] * 31 + 127) / 255); }
void palette(uint32_t c0, uint32_t c1, int pal[4][3]) {
    rgb565(c0, pal[0]); rgb565(c1, pal[1]);
    for (int c = 0; c < 3; c++) {
        if (c0 > c1) { pal[2][c] = (2 * pal[0][c] + pal[1][c]) / 3; pal[3][c] = (pal[0][c] + 2 * pal[1][c]) / 3; }
        else { pal[2][c] = (pal[0][c] + pal[1][c]) / 2; pal[3][c] = 0; }
    }
}
void dxt1_decode(const uint8_t *b, int px[16][3]) {
    uint32_t bits = b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24;
    int pal[4][3];
    palette(b[0] | b[1] << 8, b[2] | b[3] << 8, pal);
    for (int k = 0; k < 16; k++) for (int c = 0; c < 3; c++) px[k][c] = pal[bits >> (2 * k) & 3][c];
}
void dxt1_encode(int px[16][3], uint8_t *b) {
    int lo[3], hi[3], mean[3], pal[4][3], t, cov[3] = {0, 0, 0};
    uint32_t c0, c1, bits = 0;
    for (int c = 0; c < 3; c++) {
        lo[c] = 255; hi[c] = 0; mean[c] = 0;
        for (int k = 0; k < 16; k++) { t = px[k][c]; if (t < lo[c]) lo[c] = t; if (t > hi[c]) hi[c] = t; mean[c] += t; }
        mean[c] >>= 4;
    }
    for (int k = 0; k < 16; k++) for (int c = 1; c < 3; c++) cov[c] += (px[k][0] - mean[0]) * (px[k][c] - mean[c]);
    for (int c = 1; c < 3; c++) if (cov[c] < 0) { t = lo[c]; lo[c] = hi[c]; hi[c] = t; }
    for (int c = 0; c < 3; c++) { t = (hi[c] - lo[c]) / 16; hi[c] -= t; lo[c] += t; }
    c0 = to565(hi); c1 = to565(lo);
    if (c0 < c1) std::swap(c0, c1);
    if (c0 != c1) {
        palette(c0, c1, pal);
        for (int k = 0; k < 16; k++) {
            int best = 0, bd = 0x7fffffff;
            for (int s = 0; s < 4; s++) {
                int d = 0;
                for (int c = 0; c < 3; c++) { t = px[k][c] - pal[s][c]; d += t * t; }
                if (d < bd) { bd = d; best = s; }
            }
            bits |= (uint32_t)best << (2 * k);
        }
    }
    b[0] = (uint8_t)c0; b[1] = (uint8_t)(c0 >> 8); b[2] = (uint8_t)c1; b[3] = (uint8_t)(c1 >> 8);
    b[4] = (uint8_t)bits; b[5] = (uint8_t)(bits >> 8); b[6] = (uint8_t)(bits >> 16); b[7] = (uint8_t)(bits >> 24);
}
}
std::string stamp_preview(const std::string &preview, const std::string &ser) {
    std::string tex = preview;
    if (tex.size() != 16384 || ser.size() < 16 || ser.compare(0, 4, "HUSB") != 0) return tex;
    const uint8_t *s = (const uint8_t *)ser.data();
    int w = s[4] | s[5] << 8, hh = s[6] | s[7] << 8, x0 = s[8] | s[9] << 8, y0 = s[10] | s[11] << 8;
    if (w < 1 || w > 64 || hh < 1 || hh > 64 || x0 + w > 256 || y0 + hh > 128 || ser.size() != (size_t)(16 + w * hh * 4)) return tex;
    for (int by = y0 >> 2; by <= (y0 + hh - 1) >> 2; by++)
        for (int bx = x0 >> 2; bx <= (x0 + w - 1) >> 2; bx++) {
            uint8_t *blk = (uint8_t *)&tex[(size_t)(by * 64 + bx) * 8];
            int px[16][3];
            dxt1_decode(blk, px);
            for (int k = 0; k < 16; k++) {
                int X = bx * 4 + (k & 3) - x0, Y = by * 4 + (k >> 2) - y0;
                if (X < 0 || X >= w || Y < 0 || Y >= hh) continue;
                const uint8_t *q = s + 16 + (Y * w + X) * 4;
                int a = q[3];
                for (int c = 0; c < 3; c++) px[k][c] = (px[k][c] * (255 - a) + q[2 - c] * a + 127) / 255;
            }
            dxt1_encode(px, blk);
        }
    return tex;
}

// ---------------------------------------------------------------- writing
static bool write_file(const fs::path &p, const std::string &data, std::string &err) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(data.data(), (std::streamsize)data.size());
    if (!f) { err = "Couldn't write " + p.u8string(); return false; }
    return true;
}
bool write_pack(const Pack &p, const std::string &pack_name, const fs::path &maps_dir, const std::string &series,
                const std::function<void(const std::string &)> &log, std::string &err) {
    std::error_code ec;
    if (std::string why = pack_name_problem(pack_name, p); !why.empty()) { err = "Pack name: " + why; return false; }
    for (size_t i = 0; i < p.maps.size(); i++)
        if (std::string why = folder_problem(p, pack_name, i); !why.empty()) { err = p.maps[i].name + ": " + why; return false; }
    fs::path root = maps_dir / fs::u8path(pack_name);
    fs::create_directories(root, ec);
    if (ec) { err = "Couldn't create " + root.u8string() + ": " + ec.message(); return false; }
    int total = 0, n = 0;
    for (auto &e : p.maps) total += e.include;
    std::string order = "# " + pack_name + " - the order its maps appear in the menu (Halo Map Porter wrote this)\r\n";
    for (auto &e : p.maps) {
        if (!e.include) continue;
        n++;
        fs::path dir = root / fs::u8path(e.folder);
        uintmax_t mb = fs::file_size(e.map_file, ec) >> 20;
        if (log) log("Map " + std::to_string(n) + " of " + std::to_string(total) + ": " + e.name + "  ->  " + pack_name + "\\" + e.folder +
                     " (" + std::to_string(mb) + " MB)");
        fs::create_directories(dir, ec);
        if (ec) { err = "Couldn't create " + dir.u8string() + ": " + ec.message(); return false; }
        fs::path dst = dir / e.map_file.filename();
        for (auto &old : fs::directory_iterator(dir, ec))   // the XBE uses the first .map it finds: no strays
            if (old.is_regular_file() && lower(old.path().extension().u8string()) == ".map" && old.path().filename() != dst.filename())
                fs::remove(old.path(), ec);
        if (!fs::equivalent(e.map_file, dst, ec)) {
            fs::copy_file(e.map_file, dst, fs::copy_options::overwrite_existing, ec);
            if (ec) { err = "Couldn't copy " + e.map_file.u8string() + ": " + ec.message(); return false; }
        }
        std::string txt = e.name;   // line 1 = name, then the description, broken the way the Xbox shows it
        if (!e.desc.empty())
            for (auto &line : mapcard::wrap(mapcard::utf8_to_u32(e.desc), mapcard::SMALL, mapcard::DESC_TEXT_W))
                txt += "\r\n" + mapcard::u32_to_utf8(line);
        if (!e.lines.empty()) txt += "\r\n" + e.lines;
        txt += "\r\n";
        if (!write_file(dir / "info.txt", txt, err)) return false;
        if (e.preview.size() == 16384) { if (!write_file(dir / "preview.bin", e.preview, err)) return false; }
        else fs::remove(dir / "preview.bin", ec);
        order += e.folder + "\r\n";
    }
    if (!write_file(root / "order.txt", order, err)) return false;
    if (!series.empty()) {
        if (!write_file(root / "series.bin", series, err)) return false;
        if (log) log("Wrote " + pack_name + "\\series.bin (the logo on every picture)");
    }
    if (log) log("Done: " + std::to_string(total) + " maps in " + root.u8string());
    return true;
}
} // namespace mappack
