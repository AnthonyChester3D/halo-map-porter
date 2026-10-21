// Halo Map Porter - the Halo Universal XBE's map list settings (see maplist_edit.hpp)
#include "maplist_edit.hpp"
#include "mapcard.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace maplist {
namespace {
const char *const kStockKeys[13] = {"beavercreek", "sidewinder", "damnation", "ratrace", "prisoner", "hangemhigh", "chillout",
                                    "carousel", "boardingaction", "bloodgulch", "wizard", "putput", "longest"};
const char *const kStockNames[13] = {"Battle Creek", "Sidewinder", "Damnation", "Rat Race", "Prisoner", "Hang 'Em High",
                                     "Chill Out", "Derelict", "Boarding Action", "Blood Gulch", "Wizard", "Chiron TL34", "Longest"};
const char *const kStockPlayers[13] = {"2-8", "4-16", "4-8", "2-6", "2-8", "4-16", "2-8", "4-8", "4-16", "4-16", "2-8", "2-6", "2-8"};

std::string lower(std::string s) {
    for (auto &c : s) c = (char)tolower((unsigned char)c);
    return s;
}
std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
std::string slashes(std::string s) {
    std::replace(s.begin(), s.end(), '/', '\\');
    return s;
}
bool read_all(const fs::path &p, std::string &out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}
// text lines of a file: UTF-8 (BOM dropped), ANSI as is, or UTF-16 with a BOM (to UTF-8). `utf16` says which.
std::vector<std::string> read_lines(const std::string &raw, bool *utf16) {
    std::string s;
    bool wide = raw.size() >= 2 && (unsigned char)raw[0] == 0xff && (unsigned char)raw[1] == 0xfe;
    if (wide) {
        std::u32string u;
        for (size_t i = 2; i + 1 < raw.size(); i += 2) {
            char32_t c = (unsigned char)raw[i] | ((unsigned char)raw[i + 1] << 8);
            if (c >= 0xd800 && c < 0xdc00 && i + 3 < raw.size()) {
                char32_t lo = (unsigned char)raw[i + 2] | ((unsigned char)raw[i + 3] << 8);
                if (lo >= 0xdc00 && lo < 0xe000) { c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00); i += 2; }
            }
            u += c;
        }
        s = mapcard::u32_to_utf8(u);
    } else {
        s = raw;
        if (s.compare(0, 3, "\xef\xbb\xbf") == 0) s.erase(0, 3);
    }
    if (utf16) *utf16 = wide;
    std::vector<std::string> lines;
    std::string l;
    std::istringstream ss(s);
    while (std::getline(ss, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
    }
    return lines;
}
std::string encode(const std::vector<std::string> &lines, bool utf16) {
    std::string s;
    for (auto &l : lines) s += l + "\r\n";
    if (!utf16) return s;
    std::string o = "\xff\xfe";
    for (char32_t c : mapcard::utf8_to_u32(s)) {
        auto put = [&](unsigned v) { o += (char)(v & 0xff); o += (char)(v >> 8); };
        if (c >= 0x10000) { c -= 0x10000; put(0xd800 + (unsigned)(c >> 10)); put(0xdc00 + (unsigned)(c & 0x3ff)); }
        else put((unsigned)c);
    }
    return o;
}
bool write_file(const fs::path &p, const std::string &data, std::string &err) {
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(data.data(), (std::streamsize)data.size());
        if (!f) { err = "Couldn't write " + p.u8string(); return false; }
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) { fs::remove(tmp, ec); err = "Couldn't replace " + p.u8string(); return false; }
    return true;
}
fs::path map_in(const fs::path &dir) {                    // the folder's .map (the first, as the Xbox takes it)
    std::error_code ec;
    std::vector<fs::path> v;
    for (auto &e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec) && lower(e.path().extension().u8string()) == ".map") v.push_back(e.path());
    std::sort(v.begin(), v.end());
    return v.empty() ? fs::path() : v[0];
}
bool has_map(const fs::path &dir) { return !map_in(dir).empty(); }
std::vector<fs::path> subdirs(const fs::path &dir) {
    std::vector<fs::path> v;
    std::error_code ec;
    for (auto &e : fs::directory_iterator(dir, ec)) {
        std::string n = lower(e.path().filename().u8string());
        if (!e.is_directory(ec) || n.size() > 30) continue;
        if (n.size() > 7 && (n.compare(n.size() - 7, 7, ".hmp_dl") == 0 || n.compare(n.size() - 8, 8, ".hmp_old") == 0)) continue;
        v.push_back(e.path());
    }
    std::sort(v.begin(), v.end(), [](const fs::path &a, const fs::path &b) {
        return lower(a.filename().u8string()) < lower(b.filename().u8string());
    });
    return v;
}
// does a genres.txt map name mean this row? (as the XBE: a stock map's name, the key, or a pack map's own folder)
bool spec_is(const std::string &spec, const Row &r, int stock) {
    if (r.stock) return same_name(spec, kStockKeys[stock]) || same_name(spec, kStockNames[stock]);
    if (same_name(slashes(spec), r.key)) return true;
    size_t sl = r.key.rfind('\\');
    return sl != std::string::npos && same_name(spec, r.key.substr(sl + 1));
}
int stock_index(const Row &r) {
    for (int k = 0; k < 13; k++) if (r.stock && r.key == kStockKeys[k]) return k;
    return -1;
}
void add_unique(std::vector<std::string> &v, const std::string &s) {
    for (auto &x : v) if (same_name(x, s)) return;
    v.push_back(s);
}
bool remove_name(std::vector<std::string> &v, const std::string &s) {
    size_t n = v.size();
    v.erase(std::remove_if(v.begin(), v.end(), [&](const std::string &x) { return same_name(x, s); }), v.end());
    return v.size() != n;
}
// a stats line: the key, then "name=value" fields
bool stats_key(const std::string &line, std::string &key, size_t &fields_at) {
    std::string t = line;
    size_t i = t.find_first_not_of(" \t");
    if (i == std::string::npos || t[i] == '#' || t[i] == ';') return false;
    for (size_t e = i; e < t.size(); e++) {
        if (t[e] != ' ' && t[e] != '\t') continue;
        size_t f = t.find_first_not_of(" \t", e);
        if (f == std::string::npos) return false;
        size_t eq = t.find('=', f), sp = t.find_first_of(" \t", f);
        if (eq != std::string::npos && (sp == std::string::npos || eq < sp)) {
            key = slashes(trim(t.substr(i, e - i)));
            fields_at = f;
            return true;
        }
    }
    return false;
}
std::string field(const std::string &line, size_t at, const std::string &name) {
    std::istringstream ss(line.substr(at));
    std::string tok;
    while (ss >> tok) {
        size_t eq = tok.find('=');
        if (eq != std::string::npos && same_name(tok.substr(0, eq), name)) return tok.substr(eq + 1);
    }
    return "";
}
std::string set_field(const std::string &line, size_t at, const std::string &name, const std::string &value) {
    std::string head = line.substr(0, at), rest = line.substr(at), out;
    std::istringstream ss(rest);
    std::string tok;
    bool done = false;
    while (ss >> tok) {
        size_t eq = tok.find('=');
        if (eq != std::string::npos && same_name(tok.substr(0, eq), name)) { tok = tok.substr(0, eq + 1) + value; done = true; }
        out += (out.empty() ? "" : "  ") + tok;
    }
    if (!done) out += (out.empty() ? "" : "  ") + name + "=" + value;
    return head + out;
}
}   // namespace

bool same_name(const std::string &a, const std::string &b) { return lower(trim(a)) == lower(trim(b)); }
std::vector<std::string> split_list(const std::string &s) {
    std::vector<std::string> v;
    std::string item;
    std::istringstream ss(s);
    while (std::getline(ss, item, ',')) {
        item = trim(item);
        if (!item.empty()) add_unique(v, item);
    }
    return v;
}
std::string join_list(const std::vector<std::string> &v) {
    std::string o;
    for (auto &x : v) o += (o.empty() ? "" : ", ") + x;
    return o;
}
std::vector<std::string> Row::genres() const {
    std::vector<std::string> v = txt_genres;
    for (auto &g : list_genres) add_unique(v, g);
    return v;
}
std::vector<std::string> List::genre_names() const {
    std::vector<std::string> v;
    for (auto &g : genres_txt) if (g.genre) add_unique(v, g.name);
    if (v.empty()) for (auto *g : mapcard::kDefaultGenres) v.push_back(g);
    for (auto &r : rows) for (auto &g : r.genres()) add_unique(v, g);
    return v;
}
int List::changed() const {
    int n = genres_dirty ? 1 : 0;
    for (auto &r : rows) n += r.txt_dirty || r.stats_dirty || r.players_dirty || r.gametypes_dirty;
    return n;
}

bool set_txt_line(const fs::path &txt, const std::string &key, const std::string &value, const std::string &first_line,
                  std::string &err) {
    std::string raw;
    bool utf16 = false;
    std::vector<std::string> lines;
    if (read_all(txt, raw)) lines = read_lines(raw, &utf16);
    else lines.push_back(first_line);
    if (lines.empty()) lines.push_back(first_line);
    std::string want = lower(key) + ":";
    bool found = false;
    for (size_t i = 1; i < lines.size(); i++) {
        std::string t = lower(trim(lines[i]));
        if (t.compare(0, want.size(), want) != 0) continue;
        if (found || value.empty()) { lines.erase(lines.begin() + (long)i); i--; continue; }   // (a second one goes)
        lines[i] = key + ": " + value;
        found = true;
    }
    if (!found && !value.empty()) {
        while (lines.size() > 1 && trim(lines.back()).empty()) lines.pop_back();
        lines.push_back(key + ": " + value);
    }
    return write_file(txt, encode(lines, utf16), err);
}

bool load(const fs::path &maps_dir, const fs::path &stats_file, List &out, std::string &err) {
    out = List();
    out.maps_dir = maps_dir;
    out.stats_file = stats_file;
    std::error_code ec;
    if (!fs::is_directory(maps_dir, ec)) { err = "That maps folder doesn't exist."; return false; }
    for (int k = 0; k < 13; k++) {
        Row r;
        r.key = kStockKeys[k];
        r.stock = true;
        r.name = kStockNames[k];
        r.players = kStockPlayers[k];
        r.gametypes = "all";
        out.rows.push_back(r);
    }
    auto add_folder = [&](const fs::path &dir, const std::string &key) {
        Row r;
        r.key = key;
        r.folder = dir;
        r.map = map_in(dir);
        r.txt = mapcard::folder_info_txt(dir);
        r.name = dir.filename().u8string();
        if (!r.txt.empty()) {
            std::string n, d;
            mapcard::InfoLines il;
            if (mapcard::read_info(r.txt, n, d, &il)) {
                if (!n.empty()) r.name = n;
                r.players = il.players;
                r.players_line = !il.players.empty();
                int lo, hi;
                if (r.players.empty() && mapcard::desc_players(d, lo, hi))
                    r.players = std::to_string(lo) + (lo == hi ? "" : "-" + std::to_string(hi)) + " (text)";
                r.gametypes = il.gametypes;
                r.txt_genres = split_list(il.genres);
            }
        }
        out.rows.push_back(r);
    };
    for (auto &d : subdirs(maps_dir)) {
        std::string n = d.filename().u8string();
        if (has_map(d)) { add_folder(d, n); continue; }
        for (auto &m : subdirs(d))   // a pack
            if (has_map(m) && n.size() + 1 + m.filename().u8string().size() <= 30) add_folder(m, n + "\\" + m.filename().u8string());
    }

    std::string raw;
    if ((out.genres_exists = read_all(maps_dir / "genres.txt", raw))) {
        for (auto &l : read_lines(raw, nullptr)) {
            GenreLine g;
            g.text = l;
            std::string t = trim(l);
            if (!t.empty() && t[0] != '#' && t[0] != ';') {
                size_t c = t.find(':');
                g.name = trim(t.substr(0, c));
                if (!g.name.empty()) {
                    g.genre = true;
                    if (c != std::string::npos) g.maps = split_list(t.substr(c + 1));
                }
            }
            out.genres_txt.push_back(g);
        }
        for (auto &g : out.genres_txt)
            if (g.genre)
                for (auto &r : out.rows)
                    for (auto &m : g.maps) if (spec_is(m, r, stock_index(r))) add_unique(r.list_genres, g.name);
    }

    if (!stats_file.empty() && (out.stats_exists = read_all(stats_file, raw))) {
        out.stats_lines = read_lines(raw, nullptr);
        for (auto &l : out.stats_lines) {
            std::string key;
            size_t at;
            if (!stats_key(l, key, at) || key[0] == '*') continue;
            for (auto &r : out.rows) {
                if (!same_name(r.key, key) || r.in_stats) continue;
                r.in_stats = true;
                r.plays = atoi(field(l, at, "plays").c_str());
                r.fav = atoi(field(l, at, "fav").c_str()) != 0;
                r.hide = atoi(field(l, at, "hide").c_str()) != 0;
            }
        }
    }
    return true;
}

void add_genre(List &l, Row &r, const std::string &genre) {
    for (auto &g : r.genres()) if (same_name(g, genre)) return;
    if (!r.stock) {
        add_unique(r.txt_genres, genre);
        r.txt_dirty = true;
        return;
    }
    // a stock map: in genres.txt (made with the Xbox's starting genres if there isn't one)
    if (!l.genres_exists && l.genres_txt.empty()) {
        GenreLine c;
        c.text = "# The map list's Genres sort, in this order (Halo Map Porter made this file). \"Genre: map, map\" puts maps in it.";
        l.genres_txt.push_back(c);
        for (auto *n : mapcard::kDefaultGenres) {
            GenreLine g;
            g.genre = true;
            g.name = n;
            l.genres_txt.push_back(g);
        }
    }
    GenreLine *line = nullptr;
    for (auto &g : l.genres_txt) if (g.genre && same_name(g.name, genre)) { line = &g; break; }
    if (!line) {
        GenreLine g;
        g.genre = true;
        g.name = genre;
        l.genres_txt.push_back(g);
        line = &l.genres_txt.back();
    }
    add_unique(line->maps, r.key);
    add_unique(r.list_genres, line->name);
    l.genres_dirty = true;
}
void remove_genre(List &l, Row &r, const std::string &genre) {
    if (remove_name(r.txt_genres, genre)) r.txt_dirty = true;
    if (!remove_name(r.list_genres, genre)) return;
    int st = stock_index(r);
    for (auto &g : l.genres_txt) {
        if (!g.genre || !same_name(g.name, genre)) continue;
        size_t n = g.maps.size();
        g.maps.erase(std::remove_if(g.maps.begin(), g.maps.end(), [&](const std::string &m) { return spec_is(m, r, st); }),
                     g.maps.end());
        if (g.maps.size() != n) l.genres_dirty = true;
    }
}
std::string apply_scan(Row &r, const Scan &sc, bool replace) {
    if (r.stock) return "";
    std::vector<std::string> said;
    if (!sc.gametypes.empty() && (r.gametypes.empty() || replace) && r.gametypes != sc.gametypes) {
        r.gametypes = sc.gametypes;
        r.gametypes_dirty = true;
        said.push_back("Gametypes: " + sc.gametypes);
    }
    bool from_text = !r.players_line && !r.players.empty();   // ("4-16 (text)")
    if (sc.pmax > 0 && !from_text && (r.players.empty() || replace)) {
        std::string p = sc.pmin == sc.pmax ? std::to_string(sc.pmax) : std::to_string(sc.pmin) + "-" + std::to_string(sc.pmax);
        if (p != r.players) {
            r.players = p;
            r.players_line = true;
            r.players_dirty = true;
            said.push_back("Players: " + p);
        }
    }
    if (r.genres().empty() && !sc.genres.empty()) {
        for (auto &g : sc.genres) add_unique(r.txt_genres, g);
        r.txt_dirty = true;
        said.push_back("Genres: " + join_list(r.txt_genres));
    }
    std::string o;
    for (auto &x : said) o += (o.empty() ? "" : "; ") + x;
    return o;
}
void set_fav(Row &r, bool fav) { if (r.fav != fav) { r.fav = fav; r.stats_dirty = true; } }
void set_hide(Row &r, bool hide) { if (r.hide != hide) { r.hide = hide; r.stats_dirty = true; } }

bool save(List &l, std::vector<std::string> &done, std::string &err) {
    bool stats = false;
    for (auto &r : l.rows) stats |= r.stats_dirty;
    if (stats && l.stats_file.empty()) { err = "Pick the hmp_stats.txt to save favorites and hidden maps in."; return false; }
    for (auto &r : l.rows) {
        if (!r.txt_dirty && !r.players_dirty && !r.gametypes_dirty) continue;
        fs::path t = r.txt.empty() ? r.folder / "info.txt" : r.txt;
        std::string what;
        if (r.players_dirty) {
            if (!set_txt_line(t, "Players", r.players, r.name, err)) return false;
            what += "  Players: " + r.players;
        }
        if (r.gametypes_dirty) {
            if (!set_txt_line(t, "Gametypes", r.gametypes, r.name, err)) return false;
            what += "  Gametypes: " + r.gametypes;
        }
        if (r.txt_dirty) {
            if (!set_txt_line(t, "Genres", join_list(r.txt_genres), r.name, err)) return false;
            what += "  Genres: " + (r.txt_genres.empty() ? std::string("(none)") : join_list(r.txt_genres));
        }
        r.txt = t;
        r.txt_dirty = r.players_dirty = r.gametypes_dirty = false;
        done.push_back(t.u8string() + ":" + what);
    }
    if (l.genres_dirty) {
        std::vector<std::string> lines;
        for (auto &g : l.genres_txt)
            lines.push_back(!g.genre ? g.text : g.name + (g.maps.empty() ? "" : ": " + join_list(g.maps)));
        if (!write_file(l.maps_dir / "genres.txt", encode(lines, false), err)) return false;
        l.genres_exists = true;
        l.genres_dirty = false;
        done.push_back((l.maps_dir / "genres.txt").u8string());
    }
    if (stats) {
        std::vector<std::string> lines = l.stats_lines;
        if (!l.stats_exists)
            lines.insert(lines.begin(), "# Halo Universal play stats (one line per map): plays = matches started on this Xbox, last = "
                                        "last played, first = first seen, fav = favorite (Y), hide = hidden (hold Black), order = "
                                        "when last played (higher = more recent)");
        std::vector<bool> placed(l.rows.size());
        for (auto &line : lines) {
            std::string key;
            size_t at;
            if (!stats_key(line, key, at) || key[0] == '*') continue;
            for (size_t i = 0; i < l.rows.size(); i++) {
                Row &r = l.rows[i];
                if (placed[i] || !same_name(r.key, key)) continue;
                placed[i] = true;
                if (!r.stats_dirty) break;
                line = set_field(line, at, "fav", r.fav ? "1" : "0");
                line = set_field(line, at, "hide", r.hide ? "1" : "0");
                break;
            }
        }
        while (!lines.empty() && trim(lines.back()).empty()) lines.pop_back();
        for (size_t i = 0; i < l.rows.size(); i++) {
            Row &r = l.rows[i];
            if (placed[i] || !r.stats_dirty) continue;
            lines.push_back(r.key + "  plays=0  last=-  first=-  fav=" + (r.fav ? "1" : "0") + "  hide=" + (r.hide ? "1" : "0"));
            r.in_stats = true;
        }
        if (!write_file(l.stats_file, encode(lines, false), err)) return false;
        l.stats_lines = lines;
        l.stats_exists = true;
        for (auto &r : l.rows) r.stats_dirty = false;
        done.push_back(l.stats_file.u8string());
    }
    return true;
}
}   // namespace maplist
