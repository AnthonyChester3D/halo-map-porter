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

// A built map's tag array (Custom Edition, or an Xbox map, compressed or not): every tag's group, full ID and path,
// and the scenario's path. False if it can't be read.
bool read_tags(const std::filesystem::path &map, std::vector<TagEntry> &tags, std::string &scenario);

struct Link {
    std::string tool;                   // "Halo Map Porter <version> perfect-conversion"
    std::filesystem::path ce_map, xbox_map;
    std::string ce_name, xbox_name;     // "custom_maps\timberland", "Timberland\timberland.map"
    std::vector<std::pair<std::string, int>> placed;   // ("scenery", 214)...
    std::vector<std::string> checks;    // "gameplay-tags 812 identical"...
};

// The link file's text, or "" (and `why`) when a map can't be read.
std::string link_text(const Link &l, std::string &why);

}   // namespace h1ulink
