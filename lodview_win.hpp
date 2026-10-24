// Halo Map Porter - the "LODs" window (Windows): an Xbox conversion's models (the map's own, changed or not, and others
// whose levels of detail or collision it changed), each level drawn (modelview_win.hpp), where the game switches to
// it, its collision as converted and as made, and what conversions do with each model from now on (lod.hpp Choice:
// lower-detail versions made or not and their triangles, its most detailed version trimmed or kept as made).
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <filesystem>
#include <functional>

#include "core.hpp"

namespace lodui {

// Modal. `file`: the conversion's LodView (core.hpp, Result::lods); `library`: the LOD library folder (its choices.txt
// and skip.txt); `update`: "Update the map" (core.hpp update_xbox_models, run on a thread of the window's); `redo`: "Make
// it again" for one model (core.hpp redo_model_lods, the same); `built`: the map has been built (else the button says
// "Build the map": the conversion only prepared it). True when the map has to be converted again instead (its prepared
// tags are gone, and you said yes).
bool show(HWND owner, const std::filesystem::path &file, const std::filesystem::path &library,
          const std::function<porter::Result()> &update,
          const std::function<porter::Result(const std::string &)> &redo = nullptr, bool built = true);

}   // namespace lodui
