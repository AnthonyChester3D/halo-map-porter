// Halo Map Porter - the Xbox tab's "Level shaders" window (Windows): the level's shaders with what each one's
// reflection costs on the Xbox and how much it shows, which reflections a conversion drops (the suggested ones, your
// own picks, or none), and the level drawn from where players stand, before and after (levelshaders.hpp).
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <filesystem>
#include <functional>
#include <set>
#include <string>

namespace shadersui {

// Measures the map's level (in a worker thread; `log` may be called from it): the level shaders file, or false with
// `error`.
using Measure = std::function<bool(std::filesystem::path &file, std::string &error, const std::function<void(const std::string &)> &log)>;

// What a conversion does with the level's reflections: 0 the suggested ones go, 1 exactly `drops` go (shader tags),
// 2 they all stay (the default).
struct Picks {
    int mode = 2;
    std::set<std::string> drops;
    // The level's fog and tiny scenery (Config::xbox_fog, xbox_fog_scale, xbox_fog_distance, xbox_scenery_pixels).
    int fog = 0;
    double fog_scale = 1.0;
    double fog_distance = 0;
    double tiny_px = 0;
    int weather = 0;   // (Config::xbox_weather)
    int riders = 0;    // 1: no AI for units riding other objects (Config::xbox_rider_ai_off)
    int props = 0;     // 1: no collision for small props with very detailed collision (Config::xbox_detailed_props_off)
    int sky = 0;       // 0: the map's own skies drawn in fewer layers, looking the same; 1 as made (Config::xbox_sky)
    int effects = 0;   // 0: the map's own effects no busier than Bungie's; 1 as made; 2 lighter (Config::xbox_effects)
    int lights = 0;    // dynamic lights on the map's objects: 0 as made, 1 no specular, 2 off (Config::xbox_object_lights)
    // The map's own showcase flythrough (showcase.txt's text, showcase.hpp), made in the window; empty: the
    // conversion makes one from the level (Config::xbox_showcase_text).
    std::string showcase;
};

// Modal. `key` identifies the map and what changes its level (a measurement is reused while it's the same).
// Returns true on OK with `picks` updated.
bool edit(HWND owner, const std::wstring &title, const std::string &key, Measure measure, Picks &picks);

// Where measurements are kept between runs of the app.
void set_cache_folder(const std::filesystem::path &dir);

// A conversion measured the map's level as well (Result::shaders): keep a copy for the window, under `key`.
void remember(const std::string &key, const std::filesystem::path &file);

}   // namespace shadersui
