// Halo Map Porter - the "LODs" window (Windows): the models whose levels of detail an Xbox conversion changed, each
// level drawn (modelview_win.hpp), where the game switches to it, and a way to have the app leave a model alone.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <filesystem>

namespace lodui {

// Modal. `file`: the conversion's LodView (core.hpp, Result::lods); `library`: the LOD library folder (its skip.txt).
void show(HWND owner, const std::filesystem::path &file, const std::filesystem::path &library);

}   // namespace lodui
