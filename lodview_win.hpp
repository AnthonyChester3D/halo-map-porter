// Halo Map Porter - the "LODs" window (Windows): an Xbox conversion's models (the map's own, changed or not, and others
// whose levels of detail or collision it changed), each level drawn (modelview_win.hpp), where the game switches to
// it, its collision as converted and as made, and what conversions do with each model from now on (lod.hpp Choice:
// lower-detail versions made or not and their triangles, its most detailed version trimmed or kept as made).
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <filesystem>

namespace lodui {

// Modal. `file`: the conversion's LodView (core.hpp, Result::lods); `library`: the LOD library folder (its choices.txt
// and skip.txt). True when closed with "Convert again".
bool show(HWND owner, const std::filesystem::path &file, const std::filesystem::path &library);

}   // namespace lodui
