// Halo Map Porter - map packs: turn an Xbox mod pack (maps + its ui.map) into Halo Universal map folders.
//
// A mod pack replaces the 13 stock multiplayer maps and changes ui.map so the map select menu shows its own
// pictures, names and descriptions. Halo looks them up by slot (beavercreek = 0 ... longest = 12) in the
// pictures bitmap and the two string lists that the menu's widgets point at. Some packs' XBEs have a longer
// map table (Halo 1 NHE: 27 maps); the slots then come from the pack's default.xbe. This reads them out and writes
//   <maps>\<Pack>\<Map>\<file>.map, info.txt, preview.bin     one folder per map
//   <maps>\<Pack>\order.txt                                   the pack's own menu order
//   <maps>\<Pack>\series.bin                                  optional: a logo the XBE stamps on every picture
// This part is portable; the window is in gui_win.cpp.
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "mapcard.hpp"

namespace mappack {
namespace fs = std::filesystem;

extern const char *const kStockSlots[13];     // beavercreek ... longest, in the XBE's slot order
extern const char *const kExpandedSlots[27];  // Halo 1 NHE's list (chillout ... tinker), for a pack without its XBE

struct Entry {
    std::string slot;        // stock slot the pack put this map in ("bloodgulch"), "" = a map the menu doesn't show
    fs::path map_file;       // the pack's .map
    std::string name, desc;  // menu text (UTF-8; desc has '\n' between lines)
    std::string folder;      // folder name inside the pack folder
    std::string preview;     // 16384 bytes: 256x128 DXT1 (the visible part is the top-left 140x114); "" = none
    bool include = true;
    std::string note;        // shown next to it ("not in the pack's menu"...)
    std::string lines;       // the map browser's info.txt lines (mapcard::info_lines), "" = none
};

// A picture from the pack's main menu that looks like its logo (packs usually replace the HALO logo there)
struct MenuLogo {
    std::string tag, widget;   // the bitmap and the menu widget that shows it
    mapcard::Image image;      // cut to what's visible; RGBA with the faintest-drawn strokes made solid
    bool faded = false;        // the menu draws it see-through (it may look thin as a small stamp)
};

struct Pack {
    fs::path maps_dir;                 // the pack's maps folder (has ui.map)
    std::string suggested_name;        // from the pack's folder name
    std::vector<Entry> maps;
    std::vector<std::string> info;     // what was found where (for the log)
    std::vector<MenuLogo> menu_logos;  // best first; empty = the menu still has the HALO logo, or nothing found
};

// Read a pack: `folder` is the pack's own folder or its maps folder.
bool read_pack(const fs::path &folder, Pack &out, std::string &err);

// Names the Xbox drive and the XBE accept. Characters FATX can't store are dropped ("CEX+" -> "CEX").
std::string clean_name(const std::string &name, size_t max);
constexpr size_t KEY_MAX = 30;                   // pack + '\' + folder, as the XBE allows
// Give every included map a unique folder name that fits next to the pack's name.
void assign_folders(Pack &p, const std::string &pack_name);
// "" = fine, else what's wrong with this pack name / map folder name
std::string pack_name_problem(const std::string &pack_name, const Pack &p);
std::string folder_problem(const Pack &p, const std::string &pack_name, size_t index);

// ---- series.bin: "HUSB", width, height, x, y (u16), 4 reserved bytes, then BGRA pixels (top row first)
enum Corner { BOTTOM_RIGHT = 0, BOTTOM_LEFT = 1, TOP_RIGHT = 2, TOP_LEFT = 3 };
struct Logo { int w = 0, h = 0; std::vector<uint8_t> rgba; };
// Scaled to fit box x box (never past 64). keep_strokes: thin lines stay as strong as in the original
// instead of fading as they shrink (for logos cut from a menu; a PNG keeps whatever see-through it has).
Logo fit_logo(const mapcard::Image &img, int box, bool keep_strokes = false);
std::string series_bin(const Logo &logo, Corner corner, int margin);   // placed inside the 140x114 that shows
// The picture as the XBE will show it: the same decode / blend / encode the XBE does when it loads it.
std::string stamp_preview(const std::string &preview, const std::string &series);

// Write the pack into `maps_dir` (an Xbox maps folder, or anywhere to copy from later).
bool write_pack(const Pack &p, const std::string &pack_name, const fs::path &maps_dir, const std::string &series,
                const std::function<void(const std::string &)> &log, std::string &err);
}
