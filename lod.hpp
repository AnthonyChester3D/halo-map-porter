// Halo Map Porter - lower-detail versions (LODs) for models that have none, and a library that keeps them so the
// same model in another map reuses them instead of making them again.
//
// Each part of the model (one shader's triangles) is simplified with meshoptimizer: it removes vertices where that
// changes the shape least, keeping every remaining vertex exactly as it was (position, texture coordinates, normal,
// bones), the part's open edges where it meets other parts (no cracks), and the edges between pieces moved by
// different bones (a wheel stays separate from the body). The result is stored as Halo's triangle strips.
#pragma once
#include <filesystem>
#include <string>

namespace lod {

struct Settings {
    std::filesystem::path library;   // the LOD library folder (empty: make every time, keep nothing)
    int min_triangles = 1000;        // models with fewer (most detailed version) are left alone
};

struct Result {
    bool changed = false;
    std::string why;                     // nothing changed: why
    int triangles = 0;                   // the model's most detailed version (changed: of the geometries simplified)
    int level_triangles[2] = {0, 0};     // the versions added (medium, low; 0 = not added)
    size_t bytes_added = 0;              // vertex and triangle data added to the tag
    size_t xbox_bytes = 0;               // what that takes in an Xbox map: tag memory and file (32-byte vertices, strips)
    bool from_library = false;           // reused from the library
};

// Adds lower-detail versions to the model tag `model` (.model or .gbxmodel), saved in place: every permutation
// whose detail levels all use one geometry gets a medium version (about half its triangles, used from the "high"
// switch distance down) and a low one (about a fifth, from "medium" down). A model with no switch distances gets
// `cutoffs` (tag order: super high, high, medium, low, super low; pixels). `tag`: its tag path, for the library's
// index. `targets`: the model's triangles to aim for, medium and low, instead (0: as above; chosen in the LODs
// window), allowed to move the shape further to get there.
Result add_lods(const std::filesystem::path &model, const double cutoffs[5], const Settings &settings,
                const std::string &tag = {}, const int targets[2] = nullptr);

// A heavy model's most detailed version made lighter ("trimmed"), for the Xbox: every permutation's most detailed
// geometry is simplified by the same share, aiming at `budget` triangles for the model (its regions' first
// permutations, as the game counts them), but never moving its shape further than `max_change` of the model's size
// or `max_units` world units, whichever is less, so a model whose triangles all show keeps them. Texture seams stay
// exactly as made (the simplifier only slides along them), hard edges left stay as hard, every vertex keeps its bones
// (a character bends as before), and layers drawn over a part (a shield shimmer) follow it. Lower levels made for the
// old version that are now as detailed as the trimmed one draw it instead, and geometries nothing draws any more are
// dropped. Saved in place; the trimmed geometries are kept in the library (.top files) and reused.
// `density` > 0: the budget is that many times Bungie's for the model's kind and size instead (stock_triangles,
// `kind`), and a model is only trimmed when it's over its budget by more than `slack` times.
struct TrimSettings {
    int budget = 4000;
    double max_change = 0.004;
    double max_units = 0.01;
    double density = 0;
    char kind = 's';
    double slack = 1.0;
};
struct Trim {
    bool changed = false;
    std::string why;                 // nothing changed: why
    int before = 0, after = 0;       // most detailed triangles, as made and now
    int budget = 0;                  // what it aimed at
    size_t xbox_bytes_saved = 0;     // vertex and triangle data the Xbox map no longer holds (32-byte vertices, strips)
    bool from_library = false;
};
Trim trim_top(const std::filesystem::path &model, const TrimSettings &ts, const Settings &settings, const std::string &tag = {});

// What Bungie's stock models of a kind have, for their size: the triangles of the most detailed version (the
// regions' first permutations), `size` the diagonal of the box around them (world units). Fitted to all of Bungie's
// models (Xbox tags; lod.cpp has the numbers). `kind`: f first-person weapon, w weapon or item, b character, v vehicle,
// s scenery and devices.
double stock_triangles(char kind, double size);
// A model's size as stock_triangles takes it: the diagonal of the box around its most detailed geometries (every
// permutation's). 0 if it can't be read.
double model_size(const std::filesystem::path &model);

// What to do with a model, chosen in the LODs window: kept in the library's choices.txt, one line per model, for
// every map that uses it. A model in skip.txt (below) with no line there is left as made: not trimmed, no
// lower-detail versions, no switch distances.
struct Choice {
    char lods = 'a';            // lower-detail versions: a as the Xbox tab says, m make them, n don't (nor switch distances)
    int medium = 0, low = 0;    // m: the model's triangles to aim for (0: automatic, about half and a fifth)
    char top = 'a';             // most detailed version: a as "Heavy models" says, k keep it as made, t trim it
    int top_triangles = 0;      // t: to this many
    bool skip = false;          // from skip.txt (lods n, top k)
    bool chosen = false;        // a line in choices.txt
};
Choice choice(const Settings &settings, const std::string &model_tag, const std::string &object_name);
// Writes `c` for the model (its tag path) in choices.txt, replacing its line; `note` (e.g. its name and map) ends the
// line. A choice that's all automatic takes the line out (unless the model is in skip.txt: the line overrides that).
// False if it can't.
bool set_choice(const Settings &settings, const std::string &model_tag, const Choice &c, const std::string &note);

// The models never to touch, listed in the library's skip.txt: one per line, a tag path like vehicles/jet/jet.model,
// or just a name like jet (the model's or its object's). The library also keeps index.txt, its record of what it
// holds.
bool skipped(const Settings &settings, const std::string &model_tag, const std::string &object_name);
// Adds a model (its tag path) to skip.txt, with a comment line before it (e.g. its name and map). False if it can't.
bool add_skip(const Settings &settings, const std::string &model_tag, const std::string &comment);

}   // namespace lod
