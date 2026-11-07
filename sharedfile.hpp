// Halo Map Porter - shared files (Halo 1 Unlimited's docs/shared_maps.md, format version 2, game build v1.97): the
// texture and sound data Xbox maps have in common stored once, in shared files, and left out of each map.
//
// A shared file is a cache file like a map's with no tags (its header names it; its checksum, a CRC32 of everything
// after the header unpacked, is the version the maps are built against). A map draws on up to two: its pack's
// <name>-<version>.shared (data two or more of the pack's maps use) and stock.shared (every texture and sound of the 13
// stock multiplayer maps and what three or more campaign levels use, laid out the same way everywhere: the installer
// builds it on every Xbox).
// In a map, a bitmap's pixels_offset or a sound permutation's samples offset of 0x80000000 | offset is in the pack's
// file, 0xC0000000 | offset in stock.shared; the map's header names both at 0x700, with its identity in map sharing
// (the standalone build's size on disk and header checksum). What moved is left out of the map: it's laid out again
// without it, in whole 2048-byte sectors (a sector something that stays needs is kept), and every offset to what stays
// (the tag data's, the BSPs', the textures' and sounds') moved down.
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
    std::string report;        // for people: each map's size before and after, the shared files', in all
    bool used_stock = false;   // a map draws on stock.shared (written, or already there)
    std::string pack_file;     // the pack's file written (<pack>-<8 hex of its checksum>.shared), if any
};
// `maps`: finished Xbox maps, rewritten in place (one already drawing on shared files is read through them, so running
// this again after one is rebuilt works; one that can't be read is left as it is). Data two or more of them use goes in
// the pack's file in `folder`, named for its version: <pack_name>-<8 hex of its checksum>.shared (30 characters at most);
// stored unpacked when a map drawing on it is over 47 MB unpacked. `stock_maps`: the Xbox's own maps folder; when it has
// the 13 stock multiplayer maps and the 10 campaign levels, and stock.shared laid out from them is the installer's,
// data in it is drawn from there instead, written unpacked in `stock_folder` (the folder that will be the Xbox's maps
// folder; empty: `folder`). The pack's earlier files in `folder` that no map draws on any more are removed. Every map is
// read back through the new files and compared with what it was before anything is written.
Result make(const fs::path &folder, const std::string &pack_name, const std::vector<fs::path> &maps, const fs::path &stock_maps,
            const fs::path &stock_folder, const std::function<void(const std::string &)> &log);

// The 13 stock multiplayer maps then the 10 campaign levels in `folder` (empty if any is missing).
std::vector<fs::path> stock_maps_in(const fs::path &folder);
}   // namespace sharedfile
