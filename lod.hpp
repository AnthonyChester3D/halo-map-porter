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
// index.
Result add_lods(const std::filesystem::path &model, const double cutoffs[5], const Settings &settings,
                const std::string &tag = {});

// The models never to touch, listed in the library's skip.txt: one per line, a tag path like vehicles/jet/jet.model,
// or just a name like jet (the model's or its object's). The library also keeps index.txt, its record of what it
// holds.
bool skipped(const Settings &settings, const std::string &model_tag, const std::string &object_name);

}   // namespace lod
