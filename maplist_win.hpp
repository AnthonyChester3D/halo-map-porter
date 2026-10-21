// Halo Map Porter - the Halo Universal XBE's map list settings window (Windows): players, gametypes, genres, favorites
// and hidden maps for many maps at once, and a scan that fills in what maps don't have yet.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <filesystem>
#include <functional>
#include <string>

#include "maplist_edit.hpp"

namespace maplist {
// Reads a built map (true = done; on the scan's thread): porter::analyze_map_traits.
using Analyzer = std::function<bool(const std::filesystem::path &map, Scan &out, std::string &err)>;
// Modal. `maps_dir` / `stats_file` are where it starts, and on return the ones last used.
void edit_map_list(HWND owner, std::wstring &maps_dir, std::wstring &stats_file, Analyzer analyze);
}
