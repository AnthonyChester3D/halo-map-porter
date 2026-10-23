// Halo Map Porter - a sky's cost on the Xbox, and making it lighter without changing how it looks.
//
// The Xbox draws the sky first in every frame, over the whole view, before the level: each of its model's parts is a
// layer of transparent shader drawn over the ones before it (render_sky.c, models.c: regions in order, each part
// straight away), and the level then covers most of it. So a sky costs what its layers fill, wherever the player
// looks: Bungie's multiplayer skies fill 0.8 (Sidewinder) to 1.4 (Carousel, Boarding Action) layers per pixel,
// their densest campaign one (d40) 2.4. A PC sky of many full-dome layers (Precipice: 2.8) has the Xbox blend nearly
// three screenfuls in every frame before it draws anything else.
//
// lighten() makes such a sky cheaper with changes that leave the picture as it was:
//  - Drawing layers together: a layer whose transparency doesn't move (its clouds may slide) takes in the still layer
//    (no texture moves) drawn just before it on the same dome (the same triangles seen from the same direction), and
//    the still layers drawn just after it where its dome is in front of them (a haze band, sun rays: what's below the
//    dome's rim stays as it was). Their textures are combined into new ones, the moving maps kept, so that one pass
//    of up to four texture stages blends what the separate passes did (the chicago combiners' own arithmetic).
//    Precipice: its two cloud domes, haze band and sun rays in one pass.
//  - Trimming: the parts of a layer where its texture is clear (nothing drawn: the corners of a planet's square) are
//    cut away, on a finer grid of the same triangles.
#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace sky {

// Where a tag's file is to be read ("" if it isn't anywhere), by tag path with extension. It must find the tags
// written through Edit.
using Find = std::function<std::filesystem::path(const std::string &tag)>;
// Where to write a tag: an existing tag copied to where it can be edited first; a new one's place. "" if it can't be.
using Edit = std::function<std::filesystem::path(const std::string &tag)>;

struct Fill {
    bool ok = false;
    double layers = 0;   // layers drawn per pixel, over views all round the horizon, up and a little down
    double worst = 0;    // in the busiest of those views
    int parts = 0, triangles = 0;
};
// The sky as it is (`sky`: the .sky tag).
Fill measure(const std::string &sky, const Find &find);

struct Lightened {
    bool changed = false;
    Fill before, after;
    std::vector<std::string> merged;    // each pass that draws several layers: "cloudcover and highclouds, rays, cyl"
    std::vector<std::string> trimmed;   // each layer cut down, with how much of it went: "planet (20%)"
    std::vector<std::string> written;   // tags written (new ones too)
};
Lightened lighten(const std::string &sky, const Find &find, const Edit &edit);

}   // namespace sky
