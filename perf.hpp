// Halo Map Porter - what a map asks of the renderer: its level geometry (how much of it each spot can see) and
// its models (how detailed, whether they have levels of detail and when they switch). Read from the tags with
// tagfile.hpp. Used for the report's "Performance" section and for filling in missing LOD switch distances.
#pragma once
#include <filesystem>
#include <string>

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
    bool lods = false;           // a permutation with more than one level of detail
    double cutoff[5] = {};
    int triangles = 0, lowest_triangles = 0;
    bool no_cutoffs() const { return !cutoff[0] && !cutoff[1] && !cutoff[2] && !cutoff[3] && !cutoff[4]; }
};
ModelStats model_stats(const std::filesystem::path &model);

// The model an object tag (.vehicle, .scenery, .weapon...) draws, as a tag path with extension; empty if none.
std::string object_model(const std::filesystem::path &object);
// A weapon's first-person model (drawn in the player's view; the engine always asks it for its most detailed level).
std::string weapon_first_person_model(const std::filesystem::path &weapon);

}   // namespace perf
