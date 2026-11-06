// Halo Map Porter - shared files (Halo 1 Unlimited's docs/shared_maps.md, format version 2, game build v1.94): the
// texture and sound data Xbox maps have in common stored once, in shared files, and left out of each map.
//
// A shared file is a cache file like a map's with no tags (its header names it; its checksum, a CRC32 of everything
// after the header unpacked, is the version the maps are built against). A map draws on up to two: its pack's
// <name>.shared (data two or more of the pack's maps use) and stock.shared (every texture and sound of the 13 stock
// multiplayer maps, laid out the same way everywhere, so every player can build the same file from their own maps).
// In a map, a bitmap's pixels_offset or a sound permutation's samples offset of 0x80000000 | offset is in the pack's
// file, 0xC0000000 | offset in stock.shared; the map's header names both at 0x700, with its identity in map sharing
// (the standalone build's size on disk and header checksum). What moved is left out of the map: it's laid out again
// without it, in whole 512-byte sectors (everything keeps its offset's remainder modulo 512), and every offset to what
// stays (the tag data's, the BSPs', the textures' and sounds') moved down.
#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace sharedfile {
namespace fs = std::filesystem;

struct Result {
    bool ok = false;
    std::string error;
    std::string report;   // for people: each map's size before and after, the shared files', in all
    bool used_stock = false;   // a map draws on stock.shared (written, or already there)
};
// `maps`: finished Xbox maps, rewritten in place (maps already drawing on shared files are read through them, so
// running this again after one is rebuilt works). The pack's file is written to `shared` (its stem is the name the
// maps give it: 1 to 30 characters); only data two or more of the maps use goes in it. `stock_maps`: the Xbox's own
// maps folder; when it has the 13 stock multiplayer maps, data in them is drawn from stock.shared instead, written in
// `stock_folder` (the folder that will be the Xbox's maps folder; empty: beside the pack's file). Every map is read
// back through the new files and compared with what it was before anything is written.
Result make(const fs::path &shared, const std::vector<fs::path> &maps, const fs::path &stock_maps, const fs::path &stock_folder,
            const std::function<void(const std::string &)> &log);

// The 13 stock multiplayer maps in `folder` (empty if any is missing).
std::vector<fs::path> stock_multiplayer_maps(const fs::path &folder);
}   // namespace sharedfile
