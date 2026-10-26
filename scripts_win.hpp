// Halo Map Porter - the scripts window (Windows): a map's scripts to read and edit. Its own (as Invader gets them
// back out of the map), as the Xbox gets them (converted), and your edits, kept per map (a folder of .hsc files) and
// compiled instead of the map's own when it's converted or built (Config::user_scripts).
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "core.hpp"

namespace scriptsui {
// The map's own scripts (on a thread of the window's): true, or false with `err`; `notes`: what's worth knowing about
// them (old network tricks: porter::read_map_scripts's).
using Loader = std::function<bool(std::vector<porter::ScriptFile> &files, std::string &err, std::vector<std::string> &notes)>;
// Scripts as the Xbox gets them (converted), and what changed.
using Converter = std::function<std::vector<porter::ScriptFile>(const std::vector<porter::ScriptFile> &files,
                                                                std::vector<std::string> &notes)>;
// Modal. `dir`: where this map's edits are kept (read when it opens, written by Save, removed by "Go back to the
// map's"). Returns whether the map has edits when it closes.
bool edit(HWND owner, const std::wstring &title, const std::filesystem::path &dir, Loader load, Converter convert);
}
