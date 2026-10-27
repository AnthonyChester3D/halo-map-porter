// Halo Map Porter - showcase flythroughs: the camera shots Halo 1 Unlimited's showcase screensaver flies through a map
// while bots play on it (the idle main menu, in place of the attract movie). A map's own folder on the Xbox holds them
// in showcase.txt, beside info.txt and preview.bin, so a map maker can ship theirs with the map; a map without one
// gets the shots generate() makes from its level (circling its flags and power items, along its game types' routes:
// flag to flag, hill to hill, oddball to oddball; at its vehicles, looking down on them from above, flying
// over it looking straight down, down the longest sight lines players have, gliding between where players stand,
// rising over its open ground).
//
// The file (plain text; this is what Halo 1 Unlimited reads):
//   # a comment (anything after # on a line)
//   fade 0.5                  seconds of the fade to black between shots (once, anywhere; 0.5 when left out)
//   shot 12 orbit: red flag   a shot: how many seconds it lasts, then its label (for people; up to 31 characters)
//   look 12.3 -4.2 1.1        (in a shot, optional) the camera keeps this point in the middle of the view
//   key 0 x y z yaw pitch     (in a shot) a keyframe: seconds into the shot, the camera's position (world units), then
//                             its direction in degrees (yaw 0 = +x, counterclockwise; pitch up positive), as the
//                             camera tour's stops; then the horizontal field of view, optional (70 when left out)
//   # orbit 5 2 30 100        (a comment, after a shot's look line) the Level window's orbit (Shot::orbit): radius,
//                             height, start and sweep, so it can change it again; the keys are the shot
//   # pace even               (a comment) the shot's pace (Shot::pace): even, in, out or both (slow in, slow out, both),
//   # via x y z yaw pitch [fov]  then the path's own keys (Shot::via), where the camera goes; the key lines are made
//                             from them (retime), so the Level window can change the path or the seconds and time it again
// Shots play in the file's order, then the list starts again. Between a shot's keys the camera moves on a curve
// through them in time (at(): Catmull-Rom tangents from the neighbouring keys, Hermite between two keys; yaw the
// short way round), and with "look" it turns to keep its point in the middle. A shot ends at its seconds (its last
// key holds if that's sooner).
#pragma once
#include <array>
#include <string>
#include <vector>

#include "levelshaders.hpp"

namespace showcase {

using V3 = std::array<double, 3>;
constexpr double kFov = 70;        // Halo's own horizontal field of view
constexpr double kFade = 0.5;

struct Key { double t = 0; V3 p{}; double yaw = 0, pitch = 0, fov = kFov; };
// How a shot spends its seconds. Keyed: at its keys' own times. The others: along its path (Shot::via) at an even pace,
// so the camera's speed doesn't change with how close its keys are, starting from still (in), coming to rest (out), or
// both: its seconds set how fast. Its keys are then made by retime().
enum class Pace { keyed, even, in, out, both };
struct Shot {
    std::string label;
    double seconds = 0;
    bool look = false;
    V3 at{};
    std::vector<Key> keys;
    // An orbit round its look point (orbit_shot), as the Level window made it (a comment in the file: "# orbit radius
    // height start sweep"): it can change it. Its own keys once they're changed one by one.
    struct Orbit { bool on = false; double radius = 0, height = 0, start = 0, sweep = 0; };
    Orbit orbit;
    Pace pace = Pace::keyed;
    std::vector<Key> via;   // (with a pace) where the camera goes and looks, in order; t: when it gets there (retime)
};
struct Showcase {
    std::vector<Shot> shots;
    double fade = kFade;
    double length() const;   // seconds, every shot once
};

// The file's text. `made`: what the header says made it ("generated from the level", "made in its Level window");
// a file whose header names Halo Map Porter is the app's own, written again with each build (ours()).
std::string write(const Showcase &s, const std::string &map, const std::string &made);
bool read(const std::string &text, Showcase &out, std::string *error = nullptr);
bool ours(const std::string &text);
// A label as the file can hold it: no #, tabs, commas or semicolons, at most 31 characters.
std::string clean_label(const std::string &label);

// Where the camera is `t` seconds into a shot (the playback rule above). Degrees.
struct View { V3 eye{}; double yaw = 0, pitch = 0, fov = kFov; };
View at(const Shot &s, double t);

// Shots made from a measured level (its triangles, rooms, the views from where players stand, the places the scenario
// says players are, the boxes around its placed objects), kept to where players play (only the ones looking down on
// them go outside it), each checked to keep the camera inside the level, clear of walls and objects, and its subject
// in view. `report`: what it made, for the log. Empty when the level has nothing to go on.
Showcase generate(const lshade::Preview &p, std::string *report = nullptr);

// A paced shot's keys made from its path and seconds (Shot::pace): a key every half second or less (closer where it's
// quick), at even speed along a smooth curve through its via keys, with a slow start and/or end (up to 2 seconds
// each) as its pace says. A keyed shot is left as it is.
void retime(Shot &s);
// The keys that say where a shot goes: its via keys with a pace, its keys without
inline std::vector<Key> &path_keys(Shot &s) { return s.pace == Pace::keyed ? s.keys : s.via; }
inline const std::vector<Key> &path_keys(const Shot &s) { return s.pace == Pace::keyed ? s.keys : s.via; }
// The pace's name in the file ("even", "in", "out", "both"), and back (false: not one).
const char *pace_name(Pace p);
bool pace_from(const std::string &name, Pace &p);

// The shot's camera path, sampled every `step` seconds (for drawing it).
std::vector<V3> path(const Shot &s, double step = 0.25);

// A shot circling `at`, looking at it: `radius` units from it across and `height` above it, from `start` degrees round
// it (0: on its +x side, counterclockwise) through `sweep` degrees (negative: clockwise), in `seconds`, easing in and
// out.
Shot orbit_shot(const V3 &at, double radius, double height, double start, double sweep, double seconds,
                const std::string &label);

// What's wrong with a shot in the level, for people ("the camera goes through a surface 4.5 seconds in"), or "".
std::string trouble(const lshade::Preview &p, const Shot &s);

}   // namespace showcase
