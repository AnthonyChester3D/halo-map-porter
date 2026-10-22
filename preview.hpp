// Halo Map Porter - a model's preview for the weapons & vehicles and LODs windows (core.hpp ModelPreview): one of its
// levels of detail, read straight from the model tag, with each part's base texture (from its shader's bitmap) made
// small.
#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "core.hpp"

namespace preview {

// Where a tag's file is ("" when it isn't anywhere), by tag path ("vehicles/warthog/warthog.model").
using Find = std::function<std::filesystem::path(const std::string &tag)>;

// The model tag `model` (.model or .gbxmodel) as a preview; textures at most `max_texture` pixels on a side.
// False when it can't be read or has no triangles. `level`: the detail level to show (0 super high, 1 high,
// 2 medium, 3 low, 4 super low); -1 the most detailed one, or a lower one for a very detailed model (most 40,000
// triangles). `geometries`: the model's geometries that level draws.
bool build(const std::string &model, const Find &find, porter::ModelPreview &out, int max_texture = 128, int level = -1,
           std::vector<int> *geometries = nullptr);

// A unit's seats, from its tag (.vehicle): how many, and their roles ("driver, gunner, 2 passengers").
int seats(const std::filesystem::path &unit, std::string *roles);

}   // namespace preview
