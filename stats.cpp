// Halo Map Porter - map stats (stats.hpp).
#include "stats.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <vector>

namespace stats {
namespace {

struct Kind {
    const char *ext, *title, *one;
};
// (the kinds counted: weather is the level's rain and snow)
const Kind kKinds[] = {{"weapon", "Weapons", "weapon"},
                       {"vehicle", "Vehicles", "vehicle"},
                       {"biped", "Bipeds (characters)", "biped"},
                       {"sky", "Skies", "sky"},
                       {"weather_particle_system", "Weather (rain, snow...)", "weather"}};

uint64_t fnv(const std::string &s, uint64_t h = 1469598103934665603ull) {
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}
std::string hex(uint64_t v) {
    char b[17];
    snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
    return b;
}
std::string lower(std::string s) {
    for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
std::string read(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
std::string esc(const std::string &s) {
    std::string o;
    for (char c : s) o += c == '<' ? "&lt;" : c == '>' ? "&gt;" : c == '&' ? "&amp;" : c == '"' ? "&quot;" : std::string(1, c);
    return o;
}
std::mutex g_lock;

}   // namespace

void record(const fs::path &dir, const fs::path &input, const fs::path &tags, const fs::path &stock_tags) {
    std::error_code ec;
    if (!fs::is_directory(tags, ec)) return;
    // (the map, by its file's name and size: the same file from two folders is one map)
    const std::string name = input.stem().u8string();
    const uint64_t key = fnv(lower(input.filename().u8string()) + "|" + std::to_string((unsigned long long)fs::file_size(input, ec)));
    std::ostringstream out;
    out << "map\t" << name << "\t" << input.u8string() << "\t" << (long long)time(nullptr) << "\n";
    std::vector<std::string> rows;
    for (auto it = fs::recursive_directory_iterator(tags, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string ext = it->path().extension().u8string();
        if (ext.size() < 2) continue;
        ext = lower(ext.substr(1));
        const Kind *k = nullptr;
        for (auto &kk : kKinds)
            if (ext == kk.ext) k = &kk;
        if (!k) continue;
        fs::path rel = fs::relative(it->path(), tags, ec);
        if (ec) { ec.clear(); continue; }
        std::string path = lower(rel.generic_u8string());
        path.resize(path.size() - ext.size() - 1);
        const std::string bytes = read(it->path());
        // Bungie's as made (the same content as the stock maps'), edited (their path, other content) or the map's own
        std::string kind = "own";
        if (!stock_tags.empty()) {
            fs::path s = stock_tags / rel;
            if (fs::exists(s, ec)) kind = read(s) == bytes ? "stock" : "edited";
        }
        rows.push_back(std::string(k->ext) + "\t" + path + "\t" + kind + "\t" + hex(fnv(bytes)));
    }
    std::sort(rows.begin(), rows.end());
    for (auto &r : rows) out << r << "\n";
    std::lock_guard<std::mutex> g(g_lock);
    fs::create_directories(dir, ec);
    std::ofstream(dir / (hex(key) + ".tsv"), std::ios::binary | std::ios::trunc) << out.str();
}

std::string report_html(const fs::path &dir, int top, int &maps) {
    maps = 0;
    // per kind, per path: the maps using it, its versions, how it's marked
    struct Use {
        std::set<std::string> maps, versions;
        std::map<std::string, int> kinds;   // own / edited / stock: how many maps
    };
    std::map<std::string, std::map<std::string, Use>> by;
    std::error_code ec;
    for (auto &e : fs::directory_iterator(dir, ec)) {
        if (e.path().extension() != ".tsv") continue;
        std::istringstream f(read(e.path()));
        std::string line, map;
        while (std::getline(f, line)) {
            std::vector<std::string> c;
            std::istringstream ls(line);
            for (std::string x; std::getline(ls, x, '\t');) c.push_back(x);
            if (c.size() >= 2 && c[0] == "map") { map = c[1]; maps++; continue; }
            if (c.size() < 4 || map.empty()) continue;
            Use &u = by[c[0]][c[1]];
            u.maps.insert(map);
            u.versions.insert(c[3]);
            u.kinds[c[2]]++;
        }
    }
    std::ostringstream h;
    h << "<!doctype html><html><head><meta charset=\"utf-8\"><title>Map stats</title><style>"
         ":root{color-scheme:light dark;--bg:#fafafa;--fg:#1d1d1f;--mute:#6e6e73;--line:#d9d9de;--bar:#4a7bd0;--own:#2f8f5b;--ed:#b07a1c}"
         "@media (prefers-color-scheme:dark){:root{--bg:#1c1c1e;--fg:#f2f2f7;--mute:#a1a1a6;--line:#3a3a3c;--bar:#6c9cf0;--own:#53c08a;--ed:#e0a840}}"
         "body{background:var(--bg);color:var(--fg);font:14px/1.45 'Segoe UI',system-ui,sans-serif;margin:24px auto;max-width:980px;padding:0 16px}"
         "h1{font-size:22px;margin:0 0 4px}h2{font-size:17px;margin:28px 0 8px}p.m{color:var(--mute);margin:0 0 8px}"
         "table{border-collapse:collapse;width:100%}td,th{padding:5px 8px;border-bottom:1px solid var(--line);text-align:left;vertical-align:top}"
         "th{font-weight:600;color:var(--mute);font-size:12px}td.n{text-align:right;white-space:nowrap;width:1%}"
         ".bar{height:6px;background:var(--bar);border-radius:3px;margin-top:4px}.path{font-family:Consolas,monospace;font-size:13px}"
         ".k{font-size:12px;padding:1px 6px;border-radius:8px;border:1px solid currentColor;white-space:nowrap}"
         ".own{color:var(--own)}.edited{color:var(--ed)}.stock{color:var(--mute)}.maps{color:var(--mute);font-size:12px}"
         "</style></head><body><h1>Map stats</h1>";
    h << "<p class=\"m\">" << maps << " maps, from what the app took apart converting them (each map counted once, "
         "whatever folders it came from). <span class=\"k own\">custom</span> the maps' own; <span class=\"k edited\">"
         "edited</span> Bungie's path with other content; <span class=\"k stock\">stock</span> Bungie's as made. "
         "Versions: different contents under the same path.</p>";
    for (auto &k : kKinds) {
        auto it = by.find(k.ext);
        h << "<h2>" << k.title << "</h2>";
        if (it == by.end() || it->second.empty()) {
            h << "<p class=\"m\">None recorded.</p>";
            continue;
        }
        auto section = [&](const char *what, bool custom) {
            std::vector<std::pair<std::string, const Use *>> list;
            for (auto &[p, u] : it->second) {
                const bool stock_only = u.kinds.size() == 1 && u.kinds.count("stock");
                if (custom != !stock_only) continue;
                list.push_back({p, &u});
            }
            std::sort(list.begin(), list.end(), [](auto &a, auto &b) {
                return a.second->maps.size() != b.second->maps.size() ? a.second->maps.size() > b.second->maps.size() : a.first < b.first;
            });
            if (list.empty()) return;
            const size_t most = list.front().second->maps.size();
            h << "<p class=\"m\">" << what << " (" << list.size() << " in all)</p><table><tr><th>#</th><th>" << k.one
              << "</th><th>maps</th><th>versions</th><th></th></tr>";
            for (size_t i = 0; i < list.size() && i < (size_t)top; i++) {
                const Use &u = *list[i].second;
                std::string kinds;
                for (auto &[kd, n] : u.kinds) {
                    const char *label = kd == "own" ? "custom" : kd.c_str();
                    kinds += "<span class=\"k " + kd + "\">" + label + (u.kinds.size() > 1 ? " " + std::to_string(n) : "") + "</span> ";
                }
                std::string names;
                int shown = 0;
                for (auto &m : u.maps) {
                    if (shown++ == 8) { names += ", and " + std::to_string(u.maps.size() - 8) + " more"; break; }
                    names += (shown > 1 ? ", " : "") + esc(m);
                }
                h << "<tr><td class=\"n\">" << i + 1 << "</td><td><span class=\"path\">" << esc(list[i].first)
                  << "</span><div class=\"bar\" style=\"width:" << (int)(100.0 * u.maps.size() / most) << "%\"></div>"
                  << "<div class=\"maps\">" << names << "</div></td><td class=\"n\">" << u.maps.size() << "</td><td class=\"n\">"
                  << u.versions.size() << "</td><td>" << kinds << "</td></tr>";
            }
            h << "</table>";
        };
        section("Custom and edited: the most used", true);
        section("Bungie's, as made: the most used", false);
    }
    h << "</body></html>";
    return h.str();
}
}   // namespace stats
