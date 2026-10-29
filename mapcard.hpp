// Halo Map Porter - Xbox map cards (Halo Universal XBE map folders)
//
// A map folder on the Xbox holds the map, info.txt (line 1 = menu name, the rest = description) and
// preview.bin (the menu picture: a 256x128 DXT1 texture whose top-left 140x114 is what Halo shows).
// This part is portable; the editor window is in mapcard_win.cpp.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mapcard {
namespace fs = std::filesystem;

constexpr int PIC_W = 140, PIC_H = 114;   // visible picture, in Halo's 640x480 menu space
constexpr int TEX_W = 256, TEX_H = 128;   // stored texture (Halo's own map pictures use this layout)
constexpr int FOLDER_MAX = 30;            // the XBE skips longer folder names
constexpr int NAME_MAX = 63, DESC_MAX = 254;
constexpr int NAME_BOX_W = 145, NAME_LINES = 2, DESC_BOX_W = 145, DESC_LINES = 6;
// Halo's text boxes don't wrap: they clip. Text starts after the font's leading width (large_ui 4,
// small_ui 3), so a line can be this wide. The Halo Universal XBE breaks lines at these widths, and
// info.txt gets the description's line breaks written in.
constexpr int NAME_LEAD = 4, DESC_LEAD = 3;
constexpr int NAME_TEXT_W = NAME_BOX_W - NAME_LEAD, DESC_TEXT_W = DESC_BOX_W - DESC_LEAD;

struct Card {
    std::string folder, name, desc;   // UTF-8; desc uses '\n' between lines
    std::string image;                // file name of the source picture inside the card's folder ("" = none)
    double zoom = 1, cx = 0.5, cy = 0.5;   // zoom 1 = fill the frame; centre as a fraction of the image
    std::string output;               // the Xbox map these files were last written next to (UTF-8 path)
    // The Halo Universal XBE's map browser (build 20) sorts by these; info.txt gets them as lines after the description
    // ("Players: 4-16", "Gametypes: CTF, Slayer", "Genres: Competitive"), which the Xbox doesn't show.
    std::string players;              // "4-16"; "" = the description's "4-16 players", or not known
    std::string gametypes;            // "CTF, King, Oddball, Race, Slayer"
    std::string genres;               // "Competitive, Custom vehicles"
    std::string edited;               // the ones you set yourself ("players gametypes genres"): a build leaves those alone
    // The map's release ("Version: N" in info.txt; 0: none written): Halo 1 Unlimited's map sharing keeps the newer of
    // two copies. Each export of a map that differs from the last one exported (version_hash) raises it by one; one
    // set by hand (version_set) is used as it is for the next export.
    int version = 0;
    std::string version_hash;
    bool version_set = false;
};
// The map file's fingerprint, for telling a changed map from one written again with only a new card.
std::string map_fingerprint(const fs::path &map_file);
// Before a card's files are written next to `map_file`: its version raised when the map changed (see Card::version).
// True when it changed.
bool next_version(Card &c, const fs::path &map_file);
// The three map browser lines of an info.txt (empty = not there).
struct InfoLines { std::string players, gametypes, genres; int version = 0; };
bool is_edited(const Card &c, const char *field);
void mark_edited(Card &c, const char *field);
// "4 - 16 players" / "4-16" / "8" -> lo, hi (1..16); false when there's no player count at the start of s
bool parse_players(const std::string &s, int &lo, int &hi);
// A "4-16 players" in a description (what the XBE reads when there's no Players line)
bool desc_players(const std::string &desc, int &lo, int &hi);
// What a build found out about the map (porter::analyze_map_traits) goes into the fields you haven't set yourself:
// gametypes; players unless the description already says; genres when the card has none. Returns a line for the log
// ("" = nothing changed).
std::string apply_analysis(Card &c, const std::string &gametypes, int pmin, int pmax, const std::vector<std::string> &genres,
                           const std::string &players_why = "");
// The genres the Xbox starts with when maps\genres.txt doesn't list its own.
extern const char *const kDefaultGenres[7];
struct Image { int w = 0, h = 0; std::vector<uint8_t> rgba; };
struct Crop { double cw, ch, cx, cy, z, zmin; };   // crop window in image pixels

Crop crop_geometry(int w, int h, double zoom, double fx, double fy);
std::vector<uint8_t> render_picture(const Image &img, const Card &c);   // PIC_W x PIC_H RGBA
std::string preview_bin(const std::vector<uint8_t> &pic);               // TEX_W x TEX_H DXT1, 16384 bytes
std::vector<uint8_t> decode_preview(const std::string &bin);            // TEX_W x TEX_H RGBA

// Halo's menu fonts (letter widths from the stock ui tags), for wrapping exactly like the Xbox.
enum Font { LARGE, SMALL };
int glyph_width(Font f, char32_t c);   // -1: Halo's font doesn't have it
int text_width(Font f, const std::u32string &s);
std::vector<std::u32string> wrap(const std::u32string &text, Font f, int max_w, std::u32string *missing = nullptr);
std::u32string utf8_to_u32(const std::string &s);
std::string u32_to_utf8(const std::u32string &s);

std::string folder_problem(const std::string &folder);   // "" when the name is fine
std::string sanitize_folder(const std::string &name);
fs::path card_dir(const fs::path &cards_root, const fs::path &source);   // one card per source map / scenario
// The card whose files were last written next to `map_file` (so a built or copied map opens with the card
// it was made with); empty when there's none. The newest one wins.
fs::path find_card_by_output(const fs::path &cards_root, const fs::path &map_file);
Card load_card(const fs::path &dir, const std::string &default_name);
bool save_card(const fs::path &dir, const Card &c);
// The map browser's lines for info.txt ("Players: 4-16\r\nGametypes: ...", no line break at the end; "" = none)
std::string info_lines(const Card &c);
// Write just info.txt into `folder` (the picture stays as it is).
bool write_info_txt(const Card &c, const fs::path &folder, std::string &err);
// Write info.txt and preview.bin (or remove a stale preview.bin when there's no picture) into `folder`.
bool write_folder_files(const Card &c, const Image *img, const fs::path &folder, std::string &err);

// A .txt read the way the XBE reads it: line 1 = name, the rest = description ('\n' between lines).
// UTF-8 (with or without BOM), UTF-16 with BOM, or ANSI.
// The map browser's lines ("Genres:", "Gametypes:", "Players:") aren't part of the description; `lines` gets them.
bool read_info(const fs::path &txt, std::string &name, std::string &desc, InfoLines *lines = nullptr);
// The .txt the XBE would use for a map folder: info.txt, else the folder's only .txt ("" = none).
fs::path folder_info_txt(const fs::path &folder);
// A map that already sits in its own map folder (someone else's port, or an earlier build): start its
// card from the folder's info.txt and preview.bin, with the folder's name. Only when the card doesn't
// exist yet and the folder holds just this one .map plus a card file (or `in_maps_dir`: the caller knows
// the folder sits in an Xbox maps folder). Returns true when it made the card.
bool import_card(const fs::path &card_dir, const fs::path &map_file, bool in_maps_dir);
// A map from an MCC mod: a Steam Workshop item (steamapps\workshop\content\976730\<id>\) or any mod folder laid
// out the same way, <mod>\maps\<map>.map with <mod>\multiplayer\<map>.json (or campaign\). Start its card from the
// mod's own info: the map's title and description, and its thumbnail as the picture. The .json is the one whose
// ScenarioFile names this map; its Images say which picture is the thumbnail. Only when the card doesn't exist
// yet or still holds nothing but `default_name`. `what` gets a line for the log. True when it made the card.
bool import_mcc_mod_card(const fs::path &card_dir, const fs::path &map_file, const std::string &default_name,
                         std::string &what);
}
