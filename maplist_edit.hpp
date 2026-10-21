// Halo Map Porter - the Halo Universal XBE's map list settings, edited on the PC (test builds 20-21).
//
// An Xbox maps folder (or a copy of it) holds the maps, each with its info .txt ("Players:", "Gametypes:" and
// "Genres:" lines at the end), and maps\genres.txt (the genres in order, plus "Genre: map, map" lines that put maps
// in a genre without touching their .txt). The Xbox keeps favorites and hidden maps in E:\TDATA\4d530004\hmp_stats.txt.
// This reads them all into one list, changes many maps at once (or fills in what a scan of the maps finds) and writes
// back only what changed. Portable; the window is in maplist_win.cpp.
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace maplist {
namespace fs = std::filesystem;

struct Row {
    std::string key;            // the Xbox's name for it: "Precipice", "CEX\Abyss", or a stock map ("bloodgulch")
    bool stock = false;
    fs::path folder, txt;       // folder maps: the folder and its info .txt ("" = none yet: written as info.txt)
    fs::path map;               // folder maps: the .map
    std::string name;           // menu name
    std::string players, gametypes;   // as the .txt says ("4-16 (text)": from the description, no Players line)
    bool players_line = false;  // the .txt has a Players line
    std::vector<std::string> txt_genres;    // from its .txt's Genres line
    std::vector<std::string> list_genres;   // from genres.txt ("Genre: map, map")
    int plays = 0;
    bool fav = false, hide = false;
    bool in_stats = false;      // hmp_stats.txt has a line for it
    bool txt_dirty = false, stats_dirty = false;    // (txt_dirty: the Genres line; players_dirty / gametypes_dirty: theirs)
    bool players_dirty = false, gametypes_dirty = false;
    std::vector<std::string> genres() const;   // both, in order, no repeats
};

struct GenreLine { bool genre = false; std::string text, name; std::vector<std::string> maps; };

struct List {
    fs::path maps_dir, stats_file;
    std::vector<Row> rows;
    std::vector<GenreLine> genres_txt;    // genres.txt as it was read (comments kept), with changes
    bool genres_exists = false, genres_dirty = false;
    std::vector<std::string> stats_lines; // hmp_stats.txt as it was read
    bool stats_exists = false;
    std::vector<std::string> genre_names() const;   // genres.txt's (or the Xbox's starting ones), then any others in use
    int changed() const;                            // maps with unsaved changes (+1 for genres.txt)
};

// Reads a maps folder (map folders and packs; the 13 stock maps are listed too) and, if given, an hmp_stats.txt.
bool load(const fs::path &maps_dir, const fs::path &stats_file, List &out, std::string &err);
// The changes, on the selected rows.
void add_genre(List &l, Row &r, const std::string &genre);
void remove_genre(List &l, Row &r, const std::string &genre);
void set_fav(Row &r, bool fav);
void set_hide(Row &r, bool hide);
// What a scan of a map found (porter::analyze_map_traits).
struct Scan { std::string gametypes; int pmin = 0, pmax = 0; std::vector<std::string> genres; std::string why; };
// Fills in what the row doesn't have yet: a Gametypes line; a Players line, unless its description gives a count;
// genres, when it has none. `replace`: the Gametypes and Players lines are set even when there are some. Returns a line
// saying what changed ("" = nothing).
std::string apply_scan(Row &r, const Scan &s, bool replace);
// Writes the changed .txt files, genres.txt and hmp_stats.txt. `done` gets a line for each file written.
bool save(List &l, std::vector<std::string> &done, std::string &err);

// "a, b ,c" -> {"a","b","c"}; and back
std::vector<std::string> split_list(const std::string &s);
std::string join_list(const std::vector<std::string> &v);
bool same_name(const std::string &a, const std::string &b);   // ignoring case
// Sets (or with value "" removes) a "Key: value" line in an info .txt, keeping everything else as it was (UTF-8, ANSI or
// UTF-16 with a BOM). A missing file is created with `first_line` as its name line.
bool set_txt_line(const fs::path &txt, const std::string &key, const std::string &value, const std::string &first_line,
                  std::string &err);
}
