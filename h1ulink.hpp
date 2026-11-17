// Halo Map Porter - the link file (<map>.h1ulink) that tells Halo 1 Unlimited a Custom Edition map and its Xbox port
// are the same game: the two files' hashes, every tag's ID in each, the placed object counts and the checks passed.
#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace h1ulink {

// BLAKE2b with a 32-byte digest, no key.
void blake2b_256(const void *data, size_t size, uint8_t out[32]);

// The map hash Halo 1 Unlimited uses for map versions: each 256 KiB piece hashed, then "OpenCE map 1", the file size
// (4 bytes, little-endian) and the piece hashes hashed. 64 lowercase hex characters ("" if the file can't be read).
std::string map_hash(const std::filesystem::path &map, uint64_t *size = nullptr);

struct TagEntry { std::string group; uint32_t id = 0; std::string path; };   // group: its 4 characters ("weap")

// A built map's tag array (Custom Edition, MCC, or an Xbox map, compressed or not): every tag's group, full ID and path,
// and the scenario's path. False if it can't be read.
bool read_tags(const std::filesystem::path &map, std::vector<TagEntry> &tags, std::string &scenario);

// A resource file an MCC map reads: type 1 its bitmaps.map, 2 its sounds.map.
struct Resource { uint8_t type = 0; std::filesystem::path file; };

struct Link {
    std::string tool;                   // "Halo Map Porter <version> perfect-conversion"
    std::filesystem::path ce_map, xbox_map;
    std::string ce_name, xbox_name;     // "custom_maps\timberland", "Timberland\timberland.map"
    // An MCC map (version 13): an mcc line instead of the ce line, ce_name its "mcc_maps\<package>\<level>", hashed
    // with the resource files it reads (mcc_ids). ce_map is read for its tags; pc_file (when given) is the file the
    // player has, hashed (ce_map can be a copy of it with plainer tag names).
    bool mcc = false;
    std::vector<Resource> resources;
    std::filesystem::path pc_file;
    std::vector<std::pair<std::string, int>> placed;   // ("scenery", 214)...
    std::vector<std::string> checks;    // "gameplay-tags 812 identical"...
};

// The link file's text, or "" (and `why`) when a map can't be read.
std::string link_text(const Link &l, std::string &why);

// A map file's cache version (the u32 at 4): 13 an MCC map, 609 a Custom Edition map, 5 an Xbox map; 0 if not a Halo 1 map.
// (MCC and Custom Edition maps are told apart by this alone, as OpenCE does: never by their folder.)
uint32_t map_version(const std::filesystem::path &map);

// An MCC map's two IDs for its link's mcc line: `sha` (64 hex) the SHA-256 of "GulchCE MCC content v1" and each file,
// framed by its type and size (8 bytes, little-endian): the map (type 0), then the bitmaps.map (1) and sounds.map (2)
// it reads, those given only when it has textures or sounds in them; `checksum` (8 hex) the map header's checksum (the
// u32 at 0x64) with each resource file's type byte, the file and 0xFF added to it by crc32, in that order (0 is 1).
bool mcc_ids(const std::filesystem::path &map, const std::vector<Resource> &resources, std::string &sha,
             std::string &checksum, std::string &why);

// SHA-256 of a byte string, 64 hex.
std::string sha256_hex(const std::string &bytes);

// The Xbox map `map` rewritten after its link files were written (drawing on stock.shared or a pack's shared file):
// every link file beside it naming it gets its xbox line (size and hash), Xbox scenario and Xbox tag IDs read again
// from the map as it is now. How many were rewritten; `why` names any that couldn't be.
int refresh_xbox(const std::filesystem::path &map, std::string &why);

}   // namespace h1ulink
