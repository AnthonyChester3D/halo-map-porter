// Halo Map Porter - Xbox map cards (portable part). See mapcard.hpp.
#include "mapcard.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "stb_dxt.h"   // implementations live in core.cpp
#include "bcdec.h"

namespace mapcard {

// ---------------------------------------------------------------- Halo menu fonts
// ui\large_ui.font (map names) and ui\small_ui.font (descriptions): character widths, codes 32-255,
// then the Windows-1252 extras. -1 = not in the font.
static const signed char kLargeLatin[224] = {6,5,9,13,11,19,12,4,6,6,9,11,4,7,4,9,12,7,13,12,13,12,12,12,13,12,4,4,8,11,8,11,15,14,12,12,12,11,11,13,13,5,12,13,11,15,13,13,12,13,13,12,12,13,13,16,12,15,12,6,9,7,10,10,8,10,10,10,10,10,8,10,10,6,6,10,5,16,11,10,10,10,7,10,8,10,12,15,11,12,9,8,4,8,10,9,-1,9,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,9,-1,9,9,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,9,-1,-1,3,5,9,12,15,13,15,12,9,15,8,12,11,15,15,8,8,15,15,15,8,15,13,4,8,15,8,11,15,15,15,11,13,13,13,13,13,13,19,12,11,11,11,11,6,5,7,7,13,13,13,13,13,13,13,15,13,13,13,13,13,14,12,10,10,10,10,10,10,10,15,9,9,9,9,9,6,4,6,6,10,10,10,10,10,10,10,15,10,10,10,10,10,11,10,11};
static const struct { uint16_t cp; signed char w; } kLargeExtra[] = {{338,20},{339,16},{352,13},{353,10},{376,14},{381,13},{382,11},{402,8},{710,10},{732,10},{8211,9},{8212,12},{8216,5},{8217,5},{8218,5},{8220,10},{8221,10},{8222,10},{8224,11},{8225,11},{8226,9},{8230,16},{8240,27},{8249,7},{8250,7},{8364,10},{8482,16}};
static const signed char kSmallLatin[224] = {4,3,7,10,8,13,9,6,5,5,7,8,3,5,3,6,9,6,9,9,10,9,9,8,9,9,3,3,6,7,6,8,11,10,10,9,10,8,7,10,10,4,9,9,8,11,10,10,9,10,10,9,9,11,9,12,9,10,9,5,6,4,8,7,5,8,8,7,9,7,5,9,9,4,5,7,4,12,8,8,8,9,6,8,6,9,7,11,7,8,7,6,4,5,8,6,-1,6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,6,-1,6,6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,6,-1,-1,4,3,7,8,6,9,6,9,9,4,6,8,6,5,6,5,5,6,6,6,4,6,9,6,5,6,6,8,6,6,6,8,6,7,7,6,6,6,6,6,6,5,5,6,7,5,7,7,6,5,6,6,6,6,6,6,6,5,5,5,5,6,6,6,6,14,6,6,6,6,6,6,6,10,6,7,7,5,7,7,6,11,6,6,6,6,6,6,6,7,5,7,5,6,6,6};
static const struct { uint16_t cp; signed char w; } kSmallExtra[] = {{338,6},{339,6},{352,6},{353,6},{376,6},{381,9},{382,7},{402,6},{710,6},{732,6},{8211,6},{8212,6},{8216,6},{8217,6},{8218,6},{8220,6},{8221,6},{8222,6},{8224,6},{8225,6},{8226,6},{8230,6},{8240,6},{8249,6},{8250,6},{8364,6},{8482,6}};

int glyph_width(Font f, char32_t c) {
    if (c >= 32 && c <= 255) return (f == LARGE ? kLargeLatin : kSmallLatin)[c - 32];
    if (f == LARGE) { for (auto &e : kLargeExtra) if (e.cp == c) return e.w; }
    else { for (auto &e : kSmallExtra) if (e.cp == c) return e.w; }
    return -1;
}
int text_width(Font f, const std::u32string &s) {
    int w = 0;
    for (char32_t c : s) { int g = glyph_width(f, c); if (g > 0) w += g; }
    return w;
}
static bool is_space(char32_t c) { return c == U' ' || c == U'\t'; }

// Word wrap like Halo's text boxes: break at spaces, split a word wider than the box.
std::vector<std::u32string> wrap(const std::u32string &text_in, Font f, int max_w, std::u32string *missing) {
    std::vector<std::u32string> lines;
    std::u32string text = text_in;
    for (auto &c : text) if (c == U'\t') c = U' ';   // the XBE reads a tab as a space
    if (missing)
        for (char32_t c : text)
            if (c != U'\n' && glyph_width(f, c) < 0 && missing->find(c) == std::u32string::npos) missing->push_back(c);
    size_t start = 0;
    while (true) {
        size_t nl = text.find(U'\n', start);
        std::u32string para = text.substr(start, nl == std::u32string::npos ? std::u32string::npos : nl - start);
        std::u32string line;
        int lw = 0;
        size_t i = 0;
        while (i < para.size()) {
            size_t j = i;
            bool sp = is_space(para[i]);
            while (j < para.size() && is_space(para[j]) == sp) j++;
            std::u32string tok = para.substr(i, j - i);
            i = j;
            int tw = text_width(f, tok);
            if (sp) {
                if (i == para.size()) continue;   // trailing spaces never start a line
                if (lw + tw > max_w) { lines.push_back(line); line.clear(); lw = 0; }
                else if (!line.empty()) { line += tok; lw += tw; }
                continue;
            }
            if (lw + tw <= max_w) { line += tok; lw += tw; continue; }
            if (!line.empty()) {
                while (!line.empty() && is_space(line.back())) line.pop_back();
                lines.push_back(line); line.clear(); lw = 0;
            }
            if (tw <= max_w) { line = tok; lw = tw; continue; }
            for (char32_t c : tok) {
                int cw = std::max(0, glyph_width(f, c));
                if (lw + cw > max_w && !line.empty()) { lines.push_back(line); line.clear(); lw = 0; }
                line += c; lw += cw;
            }
        }
        while (!line.empty() && is_space(line.back())) line.pop_back();
        lines.push_back(line);
        if (nl == std::u32string::npos) break;
        start = nl + 1;
    }
    return lines;
}

std::u32string utf8_to_u32(const std::string &s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        char32_t cp;
        int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 6 && i + 1 < s.size()) { cp = ((c & 0x1f) << 6) | (s[i + 1] & 0x3f); n = 2; }
        else if ((c >> 4) == 14 && i + 2 < s.size()) { cp = ((c & 0x0f) << 12) | ((s[i + 1] & 0x3f) << 6) | (s[i + 2] & 0x3f); n = 3; }
        else if ((c >> 3) == 30 && i + 3 < s.size()) {
            cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3f) << 12) | ((s[i + 2] & 0x3f) << 6) | (s[i + 3] & 0x3f); n = 4;
        } else { cp = U'?'; n = 1; }
        if (cp != U'\r') out += cp;
        i += n;
    }
    return out;
}
std::string u32_to_utf8(const std::u32string &s) {
    std::string o;
    for (char32_t c : s) {
        if (c < 0x80) o += (char)c;
        else if (c < 0x800) { o += (char)(0xc0 | (c >> 6)); o += (char)(0x80 | (c & 0x3f)); }
        else if (c < 0x10000) { o += (char)(0xe0 | (c >> 12)); o += (char)(0x80 | ((c >> 6) & 0x3f)); o += (char)(0x80 | (c & 0x3f)); }
        else { o += (char)(0xf0 | (c >> 18)); o += (char)(0x80 | ((c >> 12) & 0x3f)); o += (char)(0x80 | ((c >> 6) & 0x3f)); o += (char)(0x80 | (c & 0x3f)); }
    }
    return o;
}

// ---------------------------------------------------------------- picture
Crop crop_geometry(int w, int h, double zoom, double fx, double fy) {
    const double A = (double)PIC_W / PIC_H;
    double cw0, ch0;
    if ((double)w / h > A) { ch0 = h; cw0 = h * A; } else { cw0 = w; ch0 = w / A; }
    Crop c;
    c.zmin = std::min(cw0 / w, ch0 / h);
    c.z = std::clamp(zoom, c.zmin, 4.0);
    c.cw = cw0 / c.z;
    c.ch = ch0 / c.z;
    c.cx = c.cw >= w ? w / 2.0 : std::clamp(fx * w, c.cw / 2, w - c.cw / 2);
    c.cy = c.ch >= h ? h / 2.0 : std::clamp(fy * h, c.ch / 2, h - c.ch / 2);
    return c;
}

// Area-average resample of the source window [x0, x0+sw) x [y0, y0+sh) (which may stick out of the
// image: outside counts as black) into dw x dh.
static std::vector<float> resample_axis(int n_src, double a0, double len, int n_dst, std::vector<int> &first) {
    // weights[d * span + k] for source index first[d] + k
    double k = len / n_dst;
    int span = (int)std::ceil(k) + 2;
    std::vector<float> wts((size_t)n_dst * span, 0.f);
    first.assign(n_dst, 0);
    for (int d = 0; d < n_dst; d++) {
        double s0 = a0 + d * k, s1 = s0 + k;
        int i0 = (int)std::floor(s0);
        first[d] = i0;
        for (int t = 0; t < span; t++) {
            int i = i0 + t;
            double ov = std::min(s1, (double)i + 1) - std::max(s0, (double)i);
            if (ov <= 0) continue;
            if (i < 0 || i >= n_src) continue;   // outside the image: black
            wts[(size_t)d * span + t] = (float)(ov / k);
        }
    }
    return wts;
}

std::vector<uint8_t> render_picture(const Image &img, const Card &card) {
    std::vector<uint8_t> out((size_t)PIC_W * PIC_H * 4, 0);
    for (size_t i = 3; i < out.size(); i += 4) out[i] = 255;
    if (img.w <= 0 || img.h <= 0) return out;
    Crop c = crop_geometry(img.w, img.h, card.zoom, card.cx, card.cy);
    std::vector<int> fx, fy;
    auto wx = resample_axis(img.w, c.cx - c.cw / 2, c.cw, PIC_W, fx);
    auto wy = resample_axis(img.h, c.cy - c.ch / 2, c.ch, PIC_H, fy);
    int spx = (int)(wx.size() / PIC_W), spy = (int)(wy.size() / PIC_H);
    // horizontal pass for every source row the vertical pass touches
    std::vector<float> rows((size_t)img.h * PIC_W * 3, 0.f);
    std::vector<char> done(img.h, 0);
    for (int dy = 0; dy < PIC_H; dy++) {
        float acc[3 * PIC_W];
        std::fill(acc, acc + 3 * PIC_W, 0.f);
        for (int t = 0; t < spy; t++) {
            float wv = wy[(size_t)dy * spy + t];
            if (wv == 0.f) continue;
            int sy = fy[dy] + t;
            float *row = &rows[(size_t)sy * PIC_W * 3];
            if (!done[sy]) {
                done[sy] = 1;
                const uint8_t *src = &img.rgba[(size_t)sy * img.w * 4];
                for (int dx = 0; dx < PIC_W; dx++) {
                    float r = 0, g = 0, b = 0;
                    for (int u = 0; u < spx; u++) {
                        float wh = wx[(size_t)dx * spx + u];
                        if (wh == 0.f) continue;
                        const uint8_t *p = src + (size_t)(fx[dx] + u) * 4;
                        r += wh * p[0]; g += wh * p[1]; b += wh * p[2];
                    }
                    row[dx * 3] = r; row[dx * 3 + 1] = g; row[dx * 3 + 2] = b;
                }
            }
            for (int i = 0; i < 3 * PIC_W; i++) acc[i] += wv * row[i];
        }
        for (int dx = 0; dx < PIC_W; dx++)
            for (int ch = 0; ch < 3; ch++)
                out[((size_t)dy * PIC_W + dx) * 4 + ch] = (uint8_t)std::clamp((int)std::lround(acc[dx * 3 + ch]), 0, 255);
    }
    return out;
}

std::string preview_bin(const std::vector<uint8_t> &pic) {
    std::vector<uint8_t> tex((size_t)TEX_W * TEX_H * 4, 0);
    for (size_t i = 3; i < tex.size(); i += 4) tex[i] = 255;
    for (int y = 0; y < PIC_H && pic.size() >= (size_t)PIC_W * PIC_H * 4; y++)
        memcpy(&tex[(size_t)y * TEX_W * 4], &pic[(size_t)y * PIC_W * 4], PIC_W * 4);
    std::string out;
    out.reserve(TEX_W * TEX_H / 2);
    unsigned char block[64], enc[8];
    for (int by = 0; by < TEX_H; by += 4)
        for (int bx = 0; bx < TEX_W; bx += 4) {
            for (int j = 0; j < 4; j++) memcpy(block + j * 16, &tex[((size_t)(by + j) * TEX_W + bx) * 4], 16);
            stb_compress_dxt_block(enc, block, 0, STB_DXT_HIGHQUAL);
            out.append((const char *)enc, 8);
        }
    return out;
}

std::vector<uint8_t> decode_preview(const std::string &bin) {
    std::vector<uint8_t> out((size_t)TEX_W * TEX_H * 4, 0);
    if (bin.size() < (size_t)TEX_W * TEX_H / 2) return out;
    size_t k = 0;
    for (int by = 0; by < TEX_H; by += 4)
        for (int bx = 0; bx < TEX_W; bx += 4, k += 8)
            bcdec_bc1(bin.data() + k, &out[((size_t)by * TEX_W + bx) * 4], TEX_W * 4);
    return out;
}

// ---------------------------------------------------------------- folder names
// Characters the Xbox's FATX drive accepts in names.
static bool fatx_char(unsigned char c) {
    if (c >= 'a' && c <= 'z') return true;
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= '0' && c <= '9') return true;
    return strchr(" !#$%&'()-.@[]^_`{}~", c) != nullptr && c != 0;
}
std::string folder_problem(const std::string &f) {
    if (f.find_first_not_of(' ') == std::string::npos) return "Needs a folder name";
    for (unsigned char c : f)
        if (!fatx_char(c)) return "The Xbox drive can't store some of these characters";
    if (f.back() == ' ' || f.back() == '.') return "Can't end with a space or a dot";
    if ((int)f.size() > FOLDER_MAX) return "Too long: the XBE skips names over 30 characters";
    return "";
}
std::string sanitize_folder(const std::string &name) {
    std::string o;
    for (unsigned char c : name) o += fatx_char(c) ? (char)c : '_';
    if ((int)o.size() > FOLDER_MAX) o.resize(FOLDER_MAX);
    while (!o.empty() && (o.back() == ' ' || o.back() == '.')) o.pop_back();
    if (o.empty()) o = "map";
    return o;
}

// ---------------------------------------------------------------- card storage
static std::string unesc(const std::string &s);
static std::string path_key(const fs::path &p) {
    std::error_code ec;
    std::string s = fs::absolute(p, ec).u8string();
    for (auto &c : s) { c = (char)tolower((unsigned char)c); if (c == '/') c = '\\'; }
    return s;
}
fs::path find_card_by_output(const fs::path &cards_root, const fs::path &map_file) {
    std::error_code ec;
    std::string want = path_key(map_file);
    fs::path best;
    fs::file_time_type best_t{};
    for (auto &e : fs::directory_iterator(cards_root, ec)) {
        fs::path txt = e.path() / "card.txt";
        std::ifstream f(txt, std::ios::binary);
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.compare(0, 7, "output=") != 0) continue;
            if (path_key(fs::u8path(unesc(line.substr(7)))) == want) {
                auto t = fs::last_write_time(txt, ec);
                if (best.empty() || t > best_t) { best = e.path(); best_t = t; }
            }
            break;
        }
    }
    return best;
}
fs::path card_dir(const fs::path &cards_root, const fs::path &source) {
    std::string s = path_key(source);
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    char buf[20];
    snprintf(buf, sizeof buf, "%016llx", (unsigned long long)h);
    return cards_root / buf;
}
static std::string esc(const std::string &s) {
    std::string o;
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c != '\r') o += c;
    }
    return o;
}
static std::string unesc(const std::string &s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) { o += s[i + 1] == 'n' ? '\n' : s[i + 1]; i++; }
        else o += s[i];
    }
    return o;
}
Card load_card(const fs::path &dir, const std::string &default_name) {
    Card c;
    c.folder = sanitize_folder(default_name);
    c.name = default_name.substr(0, NAME_MAX);
    std::ifstream f(dir / "card.txt", std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = unesc(line.substr(eq + 1));
        if (k == "folder") c.folder = v;
        else if (k == "name") c.name = v;
        else if (k == "desc") c.desc = v;
        else if (k == "image") c.image = v;
        else if (k == "zoom") c.zoom = atof(v.c_str());
        else if (k == "cx") c.cx = atof(v.c_str());
        else if (k == "cy") c.cy = atof(v.c_str());
        else if (k == "output") c.output = v;
        else if (k == "players") c.players = v;
        else if (k == "gametypes") c.gametypes = v;
        else if (k == "genres") c.genres = v;
        else if (k == "edited") c.edited = v;
        else if (k == "version") c.version = std::max(0, atoi(v.c_str()));
        else if (k == "version_hash") c.version_hash = v;
        else if (k == "version_set") c.version_set = v == "1";
    }
    if (!c.image.empty() && !fs::exists(dir / fs::u8path(c.image))) c.image.clear();
    return c;
}
bool save_card(const fs::path &dir, const Card &c) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream f(dir / "card.txt", std::ios::binary | std::ios::trunc);
    char num[96];
    snprintf(num, sizeof num, "zoom=%.6f\ncx=%.6f\ncy=%.6f\n", c.zoom, c.cx, c.cy);
    f << "folder=" << esc(c.folder) << "\nname=" << esc(c.name) << "\ndesc=" << esc(c.desc) << "\nimage=" << esc(c.image)
      << "\n" << num;
    if (!c.output.empty()) f << "output=" << esc(c.output) << "\n";
    if (!c.players.empty()) f << "players=" << esc(c.players) << "\n";
    if (!c.gametypes.empty()) f << "gametypes=" << esc(c.gametypes) << "\n";
    if (!c.genres.empty()) f << "genres=" << esc(c.genres) << "\n";
    if (!c.edited.empty()) f << "edited=" << esc(c.edited) << "\n";
    if (c.version > 0) f << "version=" << c.version << "\n";
    if (!c.version_hash.empty()) f << "version_hash=" << c.version_hash << "\n";
    if (c.version_set) f << "version_set=1\n";
    return (bool)f;
}

// ---------------------------------------------------------------- the map browser's lines (Halo Universal XBE build 20)
const char *const kDefaultGenres[7] = {"Competitive", "Casual", "Memes", "Artistic", "Custom vehicles", "Custom weapons",
                                       "Total conversions"};
bool is_edited(const Card &c, const char *field) {
    std::istringstream ss(c.edited);
    std::string w;
    while (ss >> w) if (w == field) return true;
    return false;
}
void mark_edited(Card &c, const char *field) {
    if (!is_edited(c, field)) c.edited += (c.edited.empty() ? "" : " ") + std::string(field);
}
bool parse_players(const std::string &s, int &lo, int &hi) {   // the same as the XBE's players_in()
    size_t i = 0;
    auto num = [&](int &v) {
        size_t k = 0;
        v = 0;
        while (i < s.size() && k < 3 && isdigit((unsigned char)s[i])) { v = v * 10 + (s[i] - '0'); i++; k++; }
        return k > 0;
    };
    auto spaces = [&] { while (i < s.size() && s[i] == ' ') i++; };
    int a, b;
    spaces();
    if (!num(a)) return false;
    spaces();
    if (i < s.size() && (s[i] == '-' || s[i] == 'x' || s.compare(i, 2, "to") == 0)) {
        i += s[i] == 't' ? 2 : 1;
        spaces();
        if (!num(b)) return false;
    } else b = a;
    if (a < 1 || b < a || b > 16) return false;
    lo = a; hi = b;
    return true;
}
bool desc_players(const std::string &desc, int &lo, int &hi) {
    std::string d = desc;
    for (auto &c : d) c = (char)tolower((unsigned char)c);
    for (size_t p = d.find("player"); p != std::string::npos; p = d.find("player", p + 1)) {
        size_t n = p;
        while (n > 0 && d[n - 1] == ' ') n--;
        while (n > 0 && (isdigit((unsigned char)d[n - 1]) || d[n - 1] == '-' || d[n - 1] == ' ')) n--;
        size_t e = d.find('\n', n);
        if (n < p && parse_players(d.substr(n, (e == std::string::npos ? d.size() : e) - n), lo, hi)) return true;
    }
    return false;
}
std::string apply_analysis(Card &c, const std::string &gametypes, int pmin, int pmax, const std::vector<std::string> &genres,
                           const std::string &players_why) {
    std::vector<std::string> said;
    if (!is_edited(c, "gametypes") && !gametypes.empty() && c.gametypes != gametypes) {
        c.gametypes = gametypes;
        said.push_back("Gametypes: " + gametypes);
    }
    int lo, hi;
    if (!is_edited(c, "players")) {
        std::string p = pmax > 0 && !desc_players(c.desc, lo, hi)
                            ? (pmin == pmax ? std::to_string(pmax) : std::to_string(pmin) + "-" + std::to_string(pmax))
                            : std::string();
        if (p != c.players) {
            c.players = p;
            if (!p.empty()) said.push_back("Players: " + p + " (a guess: " + (players_why.empty() ? "from the spawn points" : players_why) + ")");
        }
    }
    if (!is_edited(c, "genres") && c.genres.empty() && !genres.empty()) {
        for (auto &g : genres) c.genres += (c.genres.empty() ? "" : ", ") + g;
        said.push_back("suggested Genres: " + c.genres);
    }
    std::string o;
    for (auto &x : said) o += (o.empty() ? "" : "; ") + x;
    return o;
}

// ---------------------------------------------------------------- existing map folders
static std::string lower8(std::string s) {
    for (auto &c : s) c = (char)tolower((unsigned char)c);
    return s;
}
static bool valid_utf8(const std::string &s) {
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!n || i + n > s.size()) return false;
        for (int k = 1; k < n; k++) if (((unsigned char)s[i + k] >> 6) != 2) return false;
        i += n;
    }
    return true;
}
// "Genres: x" -> 0 / 1 / 2 (players, gametypes, genres) with the value; -1 for any other line
static int info_line(const std::u32string &l, std::string &value) {
    static const char *const keys[4] = {"players:", "gametypes:", "genres:", "version:"};
    size_t i = 0;
    while (i < l.size() && l[i] == U' ') i++;
    for (int k = 0; k < 4; k++) {
        size_t n = 0;
        while (keys[k][n] && i + n < l.size() && l[i + n] < 128 && tolower((int)l[i + n]) == keys[k][n]) n++;
        if (keys[k][n]) continue;
        std::u32string v = l.substr(i + n);
        while (!v.empty() && v.front() == U' ') v.erase(0, 1);
        while (!v.empty() && v.back() == U' ') v.pop_back();
        value = u32_to_utf8(v);
        return k;
    }
    return -1;
}
bool read_info(const fs::path &txt, std::string &name, std::string &desc, InfoLines *lines) {
    std::ifstream f(txt, std::ios::binary);
    if (!f) return false;
    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::u32string s;
    if (raw.size() >= 2 && (unsigned char)raw[0] == 0xff && (unsigned char)raw[1] == 0xfe) {
        for (size_t i = 2; i + 1 < raw.size(); i += 2) {
            char32_t c = (unsigned char)raw[i] | ((unsigned char)raw[i + 1] << 8);
            if (c >= 0xd800 && c < 0xdc00 && i + 3 < raw.size()) {
                char32_t lo = (unsigned char)raw[i + 2] | ((unsigned char)raw[i + 3] << 8);
                if (lo >= 0xdc00 && lo < 0xe000) { c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00); i += 2; }
            }
            s += c;
        }
    } else {
        if (raw.size() >= 3 && raw.compare(0, 3, "\xef\xbb\xbf") == 0) raw.erase(0, 3);
        if (valid_utf8(raw)) s = utf8_to_u32(raw);
        else for (unsigned char c : raw) s += (char32_t)c;   // ANSI (Windows-1252's Latin-1 part)
    }
    std::u32string n, d, l;
    bool first = true;
    InfoLines got;
    auto end_line = [&] {   // a description line, unless it's one of the map browser's
        std::string v;
        int k = info_line(l, v);
        if (k == 0) got.players = v;
        else if (k == 1) got.gametypes = v;
        else if (k == 2) got.genres = v;
        else if (k == 3) got.version = std::max(0, atoi(v.c_str()));
        else if (!d.empty() || !l.empty()) d += (d.empty() ? U"" : U"\n") + l;   // the XBE skips blank lines before it
        l.clear();
    };
    for (char32_t c : s) {
        if (c == U'\r') continue;
        if (c == U'\t') c = U' ';
        if (c == U'\n') {
            if (first) first = false;
            else end_line();
            continue;
        }
        (first ? n : l) += c;
    }
    if (!first) end_line();
    if (lines) *lines = got;
    while (!n.empty() && n.back() == U' ') n.pop_back();
    while (!d.empty() && (d.back() == U' ' || d.back() == U'\n')) d.pop_back();
    if (n.size() > (size_t)NAME_MAX) n.resize(NAME_MAX);
    if (d.size() > (size_t)DESC_MAX) d.resize(DESC_MAX);
    name = u32_to_utf8(n);
    desc = u32_to_utf8(d);
    return true;
}
fs::path folder_info_txt(const fs::path &folder) {
    std::error_code ec;
    fs::path other;
    int others = 0;
    for (auto &e : fs::directory_iterator(folder, ec)) {
        if (!e.is_regular_file(ec)) continue;
        std::string n = lower8(e.path().filename().u8string());
        if (n == "info.txt") return e.path();
        if (lower8(e.path().extension().u8string()) == ".txt") { other = e.path(); others++; }
    }
    return others == 1 ? other : fs::path();
}
static bool write_bmp(const fs::path &p, const std::vector<uint8_t> &rgba, int w, int h) {
    int row = (w * 3 + 3) & ~3;
    uint32_t size = 54 + row * h;
    std::string b(54, '\0');
    auto put = [&](int at, uint32_t v, int n) { for (int k = 0; k < n; k++) b[at + k] = (char)(v >> (8 * k)); };
    b[0] = 'B'; b[1] = 'M';
    put(2, size, 4); put(10, 54, 4); put(14, 40, 4); put(18, w, 4); put(22, h, 4); put(26, 1, 2); put(28, 24, 2);
    put(34, row * h, 4);
    for (int y = h - 1; y >= 0; y--) {
        std::string line(row, '\0');
        for (int x = 0; x < w; x++) {
            const uint8_t *px = &rgba[((size_t)y * w + x) * 4];
            line[x * 3] = (char)px[2]; line[x * 3 + 1] = (char)px[1]; line[x * 3 + 2] = (char)px[0];
        }
        b += line;
    }
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(b.data(), (std::streamsize)b.size());
    return (bool)f;
}
bool import_card(const fs::path &dir, const fs::path &map_file, bool in_maps_dir) {
    std::error_code ec;
    if (fs::exists(dir / "card.txt", ec)) return false;
    fs::path folder = map_file.parent_path();
    int maps = 0;
    fs::path pb;
    for (auto &e : fs::directory_iterator(folder, ec)) {
        if (!e.is_regular_file(ec)) continue;
        std::string n = lower8(e.path().filename().u8string());
        if (lower8(e.path().extension().u8string()) == ".map") maps++;
        else if (n == "preview.bin" && e.file_size(ec) == (uintmax_t)TEX_W * TEX_H / 2) pb = e.path();
    }
    fs::path txt = folder_info_txt(folder);
    if (maps != 1 || !(in_maps_dir || !pb.empty() || (!txt.empty() && lower8(txt.filename().u8string()) == "info.txt")))
        return false;
    Card c;
    std::string fname = folder.filename().u8string();
    c.folder = folder_problem(fname).empty() ? fname : sanitize_folder(fname);
    c.name = fname.substr(0, NAME_MAX);   // what the XBE shows without a .txt
    if (!txt.empty()) {
        std::string n, d;
        InfoLines il;
        if (read_info(txt, n, d, &il)) {
            if (!n.empty()) c.name = n;
            c.desc = d;
            if (!il.players.empty()) { c.players = il.players; mark_edited(c, "players"); }   // someone chose these
            if (!il.gametypes.empty()) { c.gametypes = il.gametypes; mark_edited(c, "gametypes"); }
            if (!il.genres.empty()) { c.genres = il.genres; mark_edited(c, "genres"); }
            if (il.version > 0) {   // (its release as it is: the same map exported again keeps the number)
                c.version = il.version;
                c.version_hash = map_fingerprint(map_file);
            }
        }
    }
    if (!pb.empty()) {
        std::ifstream f(pb, std::ios::binary);
        std::string bin((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::vector<uint8_t> tex = decode_preview(bin), pic((size_t)PIC_W * PIC_H * 4);
        for (int y = 0; y < PIC_H; y++) memcpy(&pic[(size_t)y * PIC_W * 4], &tex[(size_t)y * TEX_W * 4], PIC_W * 4);
        fs::create_directories(dir, ec);
        if (write_bmp(dir / "preview.bmp", pic, PIC_W, PIC_H)) { c.image = "preview.bmp"; c.zoom = 1; c.cx = c.cy = 0.5; }
    }
    return save_card(dir, c);
}

// ---------------------------------------------------------------- MCC mods (Steam Workshop items)
namespace {
// Just enough JSON for MCC's mod files (ModInfo.json, multiplayer\<map>.json).
struct JVal {
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
    std::string s;
    std::vector<JVal> a;
    std::vector<std::pair<std::string, JVal>> o;
    const JVal *get(const char *k) const {
        if (t == OBJ)
            for (auto &p : o) if (p.first == k) return &p.second;
        return nullptr;
    }
};
struct JParser {
    const std::string &s;
    size_t i = 0;
    int depth = 0;
    void ws() { while (i < s.size() && strchr(" \t\r\n", s[i]) && s[i]) i++; }
    bool hex4(uint32_t &v) {
        if (i + 4 > s.size()) return false;
        v = 0;
        for (int k = 0; k < 4; k++) {
            char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return false;
        }
        return true;
    }
    bool str(std::string &out) {
        if (i >= s.size() || s[i] != '"') return false;
        i++;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) return false;
            char e = s[i++];
            if (e == 'n') out += '\n';
            else if (e == 't') out += '\t';
            else if (e == 'r') out += '\r';
            else if (e == 'b' || e == 'f') out += ' ';
            else if (e == 'u') {
                uint32_t cp;
                if (!hex4(cp)) return false;
                if (cp >= 0xd800 && cp < 0xdc00 && s.compare(i, 2, "\\u") == 0) {
                    i += 2;
                    uint32_t lo;
                    if (!hex4(lo)) return false;
                    if (lo >= 0xdc00 && lo < 0xe000) cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                }
                out += u32_to_utf8(std::u32string(1, (char32_t)cp));
            } else out += e;   // \" \\ \/
        }
        return false;
    }
    bool val(JVal &v) {
        ws();
        if (i >= s.size() || depth > 64) return false;
        depth++;
        bool ok = false;
        char c = s[i];
        if (c == '{' || c == '[') {
            bool obj = c == '{';
            char close = obj ? '}' : ']';
            v.t = obj ? JVal::OBJ : JVal::ARR;
            i++;
            for (;;) {
                ws();
                if (i < s.size() && s[i] == close) { i++; ok = true; break; }   // (also allows a trailing comma)
                std::string k;
                if (obj) {
                    if (!str(k)) break;
                    ws();
                    if (i >= s.size() || s[i] != ':') break;
                    i++;
                }
                JVal x;
                if (!val(x)) break;
                if (obj) v.o.emplace_back(std::move(k), std::move(x));
                else v.a.push_back(std::move(x));
                ws();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == close) { i++; ok = true; }
                break;
            }
        } else if (c == '"') {
            v.t = JVal::STR;
            ok = str(v.s);
        } else if (s.compare(i, 4, "true") == 0 || s.compare(i, 4, "null") == 0) {
            v.t = s[i] == 't' ? JVal::BOOL : JVal::NUL;
            i += 4;
            ok = true;
        } else if (s.compare(i, 5, "false") == 0) {
            v.t = JVal::BOOL;
            i += 5;
            ok = true;
        } else {
            size_t j = i;
            while (j < s.size() && strchr("+-.eE0123456789", s[j]) && s[j]) j++;
            if (j > i) { v.t = JVal::NUM; v.s = s.substr(i, j - i); i = j; ok = true; }
        }
        depth--;
        return ok;
    }
};
bool read_json(const fs::path &p, JVal &out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (raw.size() > (8u << 20)) return false;
    if (raw.compare(0, 3, "\xef\xbb\xbf") == 0) raw.erase(0, 3);
    JParser jp{raw};
    return jp.val(out) && out.t == JVal::OBJ;
}
// MCC's text fields hold one string per language ("Neutral", "English", ...).
std::string mod_text(const JVal *v) {
    auto usable = [](const JVal *x) { return x && x->t == JVal::STR && x->s.find_first_not_of(" \t\r\n") != std::string::npos; };
    if (!v) return "";
    if (v->t == JVal::STR) return v->s;
    for (const char *k : {"English", "en", "en-US", "Neutral"})
        if (usable(v->get(k))) return v->get(k)->s;
    for (auto &p : v->o)
        if (usable(&p.second)) return p.second.s;
    return "";
}
// A mod's text as card text: lines trimmed, blank lines dropped. MCC wraps its text itself, so authors push words
// onto the next line with a run of spaces; three or more spaces count as a line break.
std::string card_text(const std::string &in, bool one_line, size_t max_chars) {
    std::u32string s = utf8_to_u32(in), line;
    std::vector<std::u32string> lines;
    int spaces = 0;
    auto end_line = [&] {
        while (!line.empty() && line.back() == U' ') line.pop_back();
        if (!line.empty()) lines.push_back(line);
        line.clear();
    };
    for (char32_t c : s) {
        if (c == U'\t') c = U' ';
        if (c == U'\n') { end_line(); spaces = 0; continue; }
        if (c < 32) continue;
        if (c == U' ') {
            if (++spaces == 3) end_line();
            if (spaces < 3 && !line.empty()) line += c;
            continue;
        }
        spaces = 0;
        line += c;
    }
    end_line();
    std::u32string o;
    for (auto &l : lines) o += (o.empty() ? U"" : one_line ? U" " : U"\n") + l;
    if (o.size() > max_chars) o.resize(max_chars);
    while (!o.empty() && (o.back() == U' ' || o.back() == U'\n')) o.pop_back();
    return u32_to_utf8(o);
}
// "levels\multi\resuscitation\resuscitation" or "chiron.scenario" -> "resuscitation" / "chiron"
std::string scenario_stem(std::string sf) {
    std::replace(sf.begin(), sf.end(), '\\', '/');
    size_t sl = sf.rfind('/');
    if (sl != std::string::npos) sf = sf.substr(sl + 1);
    sf = lower8(sf);
    for (std::string ext : {".scenario", ".map"})
        if (sf.size() > ext.size() && sf.compare(sf.size() - ext.size(), ext.size(), ext) == 0) sf.resize(sf.size() - ext.size());
    return sf;
}
fs::path mod_file(const fs::path &root, std::string rel) {
    std::replace(rel.begin(), rel.end(), '\\', '/');
    while (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
    if (rel.empty()) return {};
    std::error_code ec;
    fs::path p = root / fs::u8path(rel);
    return fs::is_regular_file(p, ec) ? p : fs::path();
}
}   // namespace

bool import_mcc_mod_card(const fs::path &dir, const fs::path &map_file, const std::string &default_name, std::string &what) {
    std::error_code ec;
    if (lower8(map_file.parent_path().filename().u8string()) != "maps") return false;
    fs::path root = map_file.parent_path().parent_path();
    if (!fs::exists(root / "ModInfo.json", ec) && !fs::is_directory(root / "multiplayer", ec) && !fs::is_directory(root / "campaign", ec))
        return false;
    Card c;
    if (fs::exists(dir / "card.txt", ec)) {   // keep a card the user has filled in
        c = load_card(dir, default_name);
        if (!c.desc.empty() || !c.image.empty() || lower8(c.name) != lower8(default_name.substr(0, NAME_MAX))) return false;
    }
    const std::string stem = lower8(map_file.stem().u8string());

    // The map's .json: listed by ModInfo.json / CampaignInfo.json, or anywhere in multiplayer\ and campaign\.
    JVal mod, campaign;
    read_json(root / "ModInfo.json", mod);
    read_json(root / "CampaignInfo.json", campaign);
    std::vector<std::pair<fs::path, bool>> jsons;   // file, listed by the mod
    auto add = [&](const fs::path &p, bool listed) {
        for (auto &j : jsons) if (lower8(j.first.u8string()) == lower8(p.u8string())) { j.second |= listed; return; }
        jsons.push_back({p, listed});
    };
    const JVal *gm = mod.get("GameModContents");
    for (const JVal *list : {gm ? gm->get("MultiplayerMaps") : nullptr, campaign.get("CampaignMaps")})
        if (list && list->t == JVal::ARR)
            for (auto &e : list->a) {
                fs::path p = e.t == JVal::STR ? mod_file(root, e.s) : fs::path();
                if (!p.empty()) add(p, true);
            }
    for (const char *sub : {"multiplayer", "campaign"})
        for (auto &e : fs::directory_iterator(root / sub, ec))
            if (e.is_regular_file(ec) && lower8(e.path().extension().u8string()) == ".json") add(e.path(), false);

    int best_score = 0;
    JVal best;
    fs::path best_json, best_pic;
    for (auto &[p, listed] : jsons) {
        JVal j;
        if (!read_json(p, j)) continue;
        const JVal *sf = j.get("ScenarioFile");
        bool own_name = lower8(p.stem().u8string()) == stem;
        int score = sf && sf->t == JVal::STR && scenario_stem(sf->s) == stem ? 10 : own_name ? 5 : 0;
        if (!score) continue;
        fs::path pic;
        if (const JVal *im = j.get("Images"))
            for (const char *k : {"Thumbnail", "Large", "LoadingScreen"})
                if (pic.empty() && im->get(k) && im->get(k)->t == JVal::STR) pic = mod_file(root, im->get(k)->s);
        score += (pic.empty() ? 0 : 2) + (mod_text(j.get("Title")).empty() ? 0 : 1) + (own_name ? 1 : 0) + (listed ? 1 : 0);
        if (score > best_score) { best_score = score; best = std::move(j); best_json = p; best_pic = pic; }
    }
    if (best_json.empty()) return false;

    // A one-map mod's own title and description stand in for missing ones.
    const JVal *mp = gm ? gm->get("MultiplayerMaps") : nullptr;
    bool one_map = !mp || mp->t != JVal::ARR || mp->a.size() <= 1;
    std::string title = card_text(mod_text(best.get("Title")), true, NAME_MAX);
    std::string desc = card_text(mod_text(best.get("Description")), false, DESC_MAX);
    if (title.empty() && one_map) title = card_text(mod_text(mod.get("Title")), true, NAME_MAX);
    if (desc.empty() && one_map) desc = card_text(mod_text(mod.get("Description")), false, DESC_MAX);
    if (title.empty() && desc.empty() && best_pic.empty()) return false;
    if (!title.empty()) {
        c.name = title;
        c.folder = sanitize_folder(title);
    } else {
        c.name = default_name.substr(0, NAME_MAX);
        c.folder = sanitize_folder(default_name);
    }
    c.desc = desc;
    c.image.clear();
    fs::create_directories(dir, ec);
    std::string ext = lower8(best_pic.extension().u8string());
    if (!best_pic.empty() && (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".gif" ||
                              ext == ".tif" || ext == ".tiff")) {
        for (auto &e : fs::directory_iterator(dir, ec))   // the editor keeps its picture as source.<ext>
            if (e.path().stem() == "source") fs::remove(e.path(), ec);
        fs::copy_file(best_pic, dir / ("source" + ext), fs::copy_options::overwrite_existing, ec);
        if (!ec) { c.image = "source" + ext; c.zoom = 1; c.cx = c.cy = 0.5; }
    }
    if (!save_card(dir, c)) return false;
    std::string rel = best_json.lexically_relative(root).make_preferred().u8string();
    what = "Map card filled in from the MCC mod's " + rel + ": \"" + c.name + "\"" +
           (c.image.empty() ? ", no picture" : ", picture " + best_pic.filename().u8string()) + ".";
    return true;
}

std::string info_lines(const Card &c) {
    std::string o;
    auto add = [&](const char *k, const std::string &v) {
        size_t a = v.find_first_not_of(' '), e = v.find_last_not_of(' ');
        if (a != std::string::npos) o += (o.empty() ? "" : "\r\n") + std::string(k) + v.substr(a, e - a + 1);
    };
    add("Players: ", c.players);
    add("Gametypes: ", c.gametypes);
    add("Genres: ", c.genres);
    if (c.version > 0) add("Version: ", std::to_string(c.version));
    return o;
}

std::string map_fingerprint(const fs::path &map_file) {
    std::ifstream f(map_file, std::ios::binary);
    if (!f) return "";
    uint64_t h = 1469598103934665603ull, n = 0;
    char buf[1 << 16];
    while (f) {
        f.read(buf, sizeof buf);
        std::streamsize got = f.gcount();
        for (std::streamsize i = 0; i < got; i++) { h ^= (unsigned char)buf[i]; h *= 1099511628211ull; }
        n += (uint64_t)got;
    }
    char hex[48];
    snprintf(hex, sizeof hex, "%016llx-%llu", (unsigned long long)h, (unsigned long long)n);
    return hex;
}
bool next_version(Card &c, const fs::path &map_file) {
    std::string fp = map_fingerprint(map_file);
    if (fp.empty()) return false;
    const int before = c.version;
    if (c.version_set) c.version = std::max(1, c.version);          // (yours, as you set it)
    else if (fp != c.version_hash) c.version = std::max(0, c.version) + 1;   // (a changed map: the next release)
    c.version_set = false;
    c.version_hash = fp;
    return c.version != before;
}
bool write_info_txt(const Card &c, const fs::path &folder, std::string &err) {
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec) { err = "Couldn't create " + folder.u8string() + ": " + ec.message(); return false; }
    std::string txt = c.name;
    std::string d = c.desc;
    while (!d.empty() && (d.back() == '\n' || d.back() == ' ')) d.pop_back();
    if (!d.empty())   // the description with its line breaks, the way the Xbox menu shows it
        for (auto &line : wrap(utf8_to_u32(d), SMALL, DESC_TEXT_W)) txt += "\r\n" + u32_to_utf8(line);
    // the map browser's lines go last: the Xbox doesn't show them, and Halo's description buffer can't cut them short
    if (std::string l = info_lines(c); !l.empty()) txt += "\r\n" + l;
    txt += "\r\n";
    std::ofstream f(folder / "info.txt", std::ios::binary | std::ios::trunc);
    f << txt;
    if (!f) { err = "Couldn't write " + (folder / "info.txt").u8string(); return false; }
    return true;
}
bool write_folder_files(const Card &c, const Image *img, const fs::path &folder, std::string &err) {
    std::error_code ec;
    if (!write_info_txt(c, folder, err)) return false;
    fs::path pb = folder / "preview.bin";
    if (img && img->w > 0) {
        std::string bin = preview_bin(render_picture(*img, c));
        std::ofstream f(pb, std::ios::binary | std::ios::trunc);
        f.write(bin.data(), (std::streamsize)bin.size());
        if (!f) { err = "Couldn't write " + pb.u8string(); return false; }
    } else {
        fs::remove(pb, ec);
    }
    return true;
}
}
