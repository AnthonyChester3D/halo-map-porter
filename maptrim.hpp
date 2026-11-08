// Halo Map Porter - trimming a built Xbox map's tag data.
//
// Invader writes the scenario's script node table at the engine's full size, 19,001 nodes of 20 bytes (371 KB),
// however few the scripts use; Halo 1 Unlimited reads a table sized to the scripts. This trims it to the nodes in use
// (and some to spare) and moves the tag data after it down: every pointer in the tag data and the structure BSPs is
// found by walking each tag through its group's structs (map_layouts.inc, from Invader's definitions), moved, and the
// result walked again and compared with the map as it was before it's written.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace maptrim {
namespace fs = std::filesystem;

const uint32_t kXboxTagSpace = 0x1600000;   // 22 MiB of tag memory: the tag data from the start, each BSP at the top
const uint32_t kMaxSaving = 0x5D000;        // (more than a trim can take out: the whole table but its header)

struct Result {
    bool ok = false;               // read, and (if anything changed) checked and written
    std::string error;
    uint32_t nodes = 0, kept = 0;  // script nodes in use; the table's size after
    uint32_t saved = 0;            // bytes of tag data taken out
    uint32_t tag_data = 0, largest_bsp = 0;   // after, in bytes: together they're the tag memory the map needs
};
// `map`: an Xbox map Invader just built (compressed or not), rewritten in place. `tag_space`: the tag space Invader was
// told the map has (its -T) when more than the Xbox's 22 MiB: Invader put the BSPs at the top of that, so they're
// moved to the top of the Xbox's (0: built for the Xbox's). `spare`: nodes kept free past those in use.
Result trim_scripts(const fs::path &map, uint64_t tag_space = 0, uint32_t spare = 256);
}   // namespace maptrim
