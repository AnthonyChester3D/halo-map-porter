// Halo Map Porter - what an effect costs the Xbox, next to Bungie's own, and bringing the map's own down to them.
//
// An effect's events make particles (each event's spread over its duration), lights, decals and other effects; its
// particles may make effects of their own when they hit something or die. What a frame pays for is what's alive at
// once: the particles the engine moves and draws (it keeps at most 1,024, every effect's together) and the sprites'
// area, which the Xbox blends over whatever is behind them.
//  - An effect made once (a muzzle flash, an impact, an explosion) costs most at its busiest moment. Bungie's Xbox
//    multiplayer effects have at most about 200 particles alive at once (a charged plasma bolt hitting stone) and 188
//    square units of sprites (the rocket's explosion).
//  - An effect attached to an object loops while the object exists (a fire on a prop, a beam, a burning wreck): it
//    costs what it keeps alive, steadily. Bungie's busiest emitters keep about 300 alive (the a30 beam emitter, the
//    teleporters), and up to 1,150 square units of sprites (the plasma flame on scenery/emitters, the a30 beam).
// Bungie's levels place many of their emitters; the engine only runs an attached effect while its spot can be seen
// (a potentially visible cluster), so what counts is each emitter's own cost.
#pragma once
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>

namespace fx {

// Where a tag's file is ("" if it isn't anywhere), by tag path with extension.
using Find = std::function<std::filesystem::path(const std::string &tag)>;

struct Cost {
    double peak_alive = 0, peak_area = 0;       // made once: at its busiest moment (area: square world units of sprites)
    double steady_alive = 0, steady_area = 0;   // looping: kept alive on average, over its loop
    double cycle = 0;                           // the loop's length, seconds
    double particles = 0;                       // made in one run, nested effects' too
};

// Bungie's busiest (measured from the Xbox tags, see above).
struct Caps {
    double peak_alive = 204, peak_area = 188;
    double steady_alive = 300, steady_area = 1150;
};

class Analyzer {
public:
    explicit Analyzer(Find find);
    ~Analyzer();
    Cost cost(const std::string &effect);
    // The effects an effect's parts make alongside it (its particles' collision and death effects come with its
    // particles: fewer particles, fewer of those).
    std::set<std::string> part_effects(const std::string &effect);

private:
    struct Impl;
    Impl *d;
};

// The share of its particles an effect may keep to stay within the caps (1: it fits). `looping`: attached to an
// object.
double keep_share(const Cost &c, bool looping, const Caps &caps = Caps());

// Scales the particle counts of each event of an effect tag (at `file`) by `share`, keeping at least one where there
// was one. False if it can't be read or written, or nothing changed.
bool scale_particles(const std::filesystem::path &file, double share);

}   // namespace fx
