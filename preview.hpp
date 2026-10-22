// Halo Map Porter - a model's preview for the weapons & vehicles window (core.hpp ModelPreview): its most detailed
// version, read straight from the model tag, with each part's base texture (from its shader's bitmap) made small.
#pragma once
#include <filesystem>
#include <functional>
#include <string>

#include "core.hpp"

namespace preview {

// Where a tag's file is ("" when it isn't anywhere), by tag path ("vehicles/warthog/warthog.model").
using Find = std::function<std::filesystem::path(const std::string &tag)>;

// The model tag `model` (.model or .gbxmodel) as a preview; textures at most `max_texture` pixels on a side.
// False when it can't be read or has no triangles. Very detailed models use a lower-detail version (most 40,000
// triangles).
bool build(const std::string &model, const Find &find, porter::ModelPreview &out, int max_texture = 128);

// A unit's seats, from its tag (.vehicle): how many, and their roles ("driver, gunner, 2 passengers").
int seats(const std::filesystem::path &unit, std::string *roles);

}   // namespace preview
