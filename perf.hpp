// Halo Map Porter - what a map asks of the renderer: its level geometry (how much of it each spot can see) and
// its models (how detailed, whether they have levels of detail and when they switch). Read from the tags with
// tagfile.hpp. Used for the report's "Performance" section and for filling in missing LOD switch distances.
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace perf {

// A structure BSP. Triangles: its render surfaces. Visible: the triangles the visibility data lets the engine
// draw from a spot in each cluster (each counted once), before the view's own culling.
struct BspStats {
    bool ok = false;
    int clusters = 0, portals = 0, triangles = 0;
    int median_visible = 0, worst_visible = 0;
};
BspStats bsp_stats(const std::filesystem::path &bsp);

// A model (.model or .gbxmodel). Cutoffs in tag order: super high, high, medium, low, super low (pixels across
// the screen; the engine draws a level once the model is at least that big, and nothing below super low).
// Triangles: its most detailed level, the first permutation of each region; lowest: its least detailed.
struct ModelStats {
    bool ok = false;
    bool lods = false;           // a permutation with lower-detail versions (geometries of their own)
    bool borrowed = false;       // a permutation whose lower levels draw another permutation (a variant: with switch
                                 // distances, it would turn into that one at a distance)
    double cutoff[5] = {};
    int triangles = 0, lowest_triangles = 0;
    bool no_cutoffs() const { return !cutoff[0] && !cutoff[1] && !cutoff[2] && !cutoff[3] && !cutoff[4]; }
};
ModelStats model_stats(const std::filesystem::path &model);

// The model an object tag (.vehicle, .scenery, .weapon...) draws, as a tag path with extension; empty if none.
std::string object_model(const std::filesystem::path &object);
// A weapon's first-person model (drawn in the player's view; the engine always asks it for its most detailed level).
std::string weapon_first_person_model(const std::filesystem::path &weapon);

// A level's rain or snow (a .weather_particle_system): each type keeps its particle count (per cubic world unit) x
// its fade-out end distance cubed drops in a box around the camera, and the engine keeps at most 512 drops in all
// (weather_particle_systems.c), so a heavy system is thinner than its tag says. Every drop is moved, clipped and
// drawn every frame, indoors too: Deathtrap's rain (asking for ~3,600) cost an Xbox 2.5 ms a frame.
struct WeatherType {
    double count_lo = 0, count_hi = 0;
    double fade_in_start = 0, fade_in_end = 0, fade_out_start = 0, fade_out_end = 0;
};
constexpr double kMaxWeatherDrops = 512;
constexpr double kLighterWeatherEnd = 5;   // (Bungie's rain on 343 Guilty Spark ends there)
std::vector<WeatherType> weather_types(const std::filesystem::path &weather);
double weather_wanted(const std::vector<WeatherType> &types);   // the drops the tag asks for
double weather_drops(const std::vector<WeatherType> &types);    // the drops the engine keeps
// The level window's choice applied to a system's types: 1 lighter (just as dense up close, as the engine draws it,
// but ending at kLighterWeatherEnd units: fewer drops), 2 half the drops, 3 none. False if it changes nothing.
bool weather_lighten(std::vector<WeatherType> &types, int mode);

// A collision model's surfaces (all its nodes' BSPs): what a player's movement is tested against near the object.
// Deathtrap's pilot chairs (416 for an object 1 unit across; a warthog's chassis has 47) cost an Xbox 5-6 ms a tick
// whenever a player stood next to one.
int collision_surfaces(const std::filesystem::path &collision_model);
// A small prop whose collision is that detailed: scenery at most kDetailedPropRadius units across its bounding
// sphere's radius with kDetailedPropSurfaces surfaces or more (Bungie's most detailed small prop, a10's oxygen tank,
// has 163).
constexpr double kDetailedPropRadius = 1.0;
constexpr int kDetailedPropSurfaces = 300;

// The units a map's scripts give AI (ai_attach) and also attach to another object (objects_attach, as the child):
// riders, like Deathtrap's marines sitting in its moving warthogs, which can't go anywhere but still think every
// tick (9 of them: 1.5 ms of each Xbox tick). Their names, lowercase, from the scripts' source text.
std::vector<std::string> script_riders(const std::string &hsc);

}   // namespace perf
