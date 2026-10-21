// Halo Map Porter - the Xbox tab's "Weapons & vehicles" window (Windows).
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <functional>
#include <map>
#include <memory>
#include <string>

#include "core.hpp"

namespace contentsui {
// Measures the map (runs in a worker thread; `log` may be called from it).
using Analyze = std::function<bool(porter::ContentAnalysis &out, const porter::Log &log)>;
// Modal window listing the map's weapons and vehicles with what each costs, a "Replace with" choice for each
// (keep, none, another of the map's, or an Xbox one) and the savings needed. `key` identifies the map and the
// options that change its contents: a measurement is reused while the key is the same. Returns true on OK, with
// `picks` (weapon/vehicle tag -> replacement tag, "" = none) updated.
bool edit_picks(HWND owner, const std::wstring &title, const std::string &key, Analyze analyze,
                std::map<std::string, std::string> &picks);
// Where measurements are kept between runs of the app (one file per map).
void set_cache_folder(const std::filesystem::path &dir);
// A measurement made elsewhere (a convert that didn't fit): the window opens with it. Any thread.
void remember(const std::string &key, std::shared_ptr<porter::ContentAnalysis> a);
}
