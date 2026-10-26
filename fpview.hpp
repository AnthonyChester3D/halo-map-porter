// Halo Map Porter - how a first-person weapon's model is seen: drawn as the game draws it in first person, posed by
// every frame of its first-person animations (first_person_weapons.c: the animation graph's node matrices from the
// camera, each model node's times its default pose's inverse, models.c), to find which of its triangles are ever on
// screen and how much of the time. Used to trim it (lod.hpp TrimSettings::seen): what's never on screen goes, and
// what's seen least is simplified first.
//
// The view: the camera at the graph's root, as the game puts it, through a frustum wider than any field of view a
// player picks (110 x 80 degrees, so nothing at the edge of a widescreen view is missed), the model's front faces
// (or both sides of a two-sided shader's), opaque parts hiding what's behind them; cut-out and blended parts hide
// nothing. Every base animation's frames (the idle ones counted most: the weapon spends most of its time there,
// firing next), and the idle pose with each overlay's frames applied in full (turning, moving, firing push back).
// A triangle next to one that's seen counts as seen (a pixel's error, or a pose in between frames, never opens a hole).
// The arms aren't drawn: what they cover counts as seen.
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "lod.hpp"

namespace fpview {

using Find = std::function<std::filesystem::path(const std::string &tag)>;

// `model` and `animations`: tag paths (with extensions), found with `find`; `look`: how its shaders draw. Fills
// `out` for its regions' first permutations' most detailed geometries. False with `why` when it can't be measured (a
// shader or tag that can't be read, a model bone the animations don't move): then nothing should be taken out.
// `pictures` (tests): the views of the first frame of up to 8 animations, w x h each (0x00RRGGBB, top row first),
// what's never seen red.
bool measure(const std::string &model, const std::string &animations, const Find &find, const lod::LookUp &look, lod::Seen &out,
             std::string *why = nullptr, std::vector<std::vector<uint32_t>> *pictures = nullptr, int w = 320, int h = 188);

}   // namespace fpview
