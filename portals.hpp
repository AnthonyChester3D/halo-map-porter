// Halo Map Porter - portals for a level that has few or none.
//
// A level (structure BSP) is drawn by clusters: the engine starts in the cluster the camera is in and goes on into
// the clusters it can see through the portals between them (an opening's outline: the view is narrowed to it), so
// what's behind walls isn't drawn. A level made without portals is one cluster: everything in view is drawn, walls
// or not.
//
// This finds the open space's rooms and the narrow openings between them (doorways, corridor and tunnel mouths)
// and makes those rooms clusters and those openings portals. The level's own geometry, collision and lightmaps
// stay exactly as they are: only which cluster each region of open space (BSP leaf) is in, the portals, and what
// goes with clusters (their surfaces, visibility and sound data) change.
//
//   1. The open space's convex regions (the collision BSP's leaves) and the openings between them (leaf portals,
//      found by cutting each BSP node's plane down to the space it divides), and the openings collision closes but
//      the eye sees through (glass, grates, invisible walls, hairline seams: hidden_openings in portals.cpp).
//   2. Rooms: a field of how far each point of the open space is from the nearest wall, sampled on a grid. Rooms
//      are its basins around the widest spots (watershed); two basins are one room unless the opening between
//      them is clearly narrower than both.
//   3. Each leaf goes to the room most of it is in (keep_clusters: within the cluster it was in). Where two rooms
//      meet, the boundary moves onto a wall's plane near it when one parts them (a doorway's opening).
//   4. The leaf portals between rooms become the cluster portals (coplanar neighbours merged), within the engine's
//      limits. add_portals writes them into a level that has one cluster, when estimate() says they help.
#pragma once
#include <array>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace portals {

using V3 = std::array<double, 3>;

// An opening between two leaves of the open space (or a leaf and solid: a wall).
struct LeafPortal {
    int front = -1, back = -1;   // leaf on the plane's front side, back side (-1: solid)
    int plane = -1;              // the collision BSP plane it's on
    std::vector<V3> points;      // a convex polygon, wound counterclockwise seen from the front
};

struct Leaves {
    int count = 0;
    std::vector<LeafPortal> portals;     // leaf-leaf and leaf-solid
    std::vector<int> cluster;            // each leaf's cluster now
    V3 lo{}, hi{};                       // the world bounds
};

// Whether a level shader can be seen through (glass, water, grates and fences drawn with alpha testing). `shader`:
// the reference's path without its extension; `group`: its tag group's four letters ("senv", "sgla", "schi"...).
using SeeThrough = std::function<bool(const std::string &shader, const std::string &group)>;

// The leaves of a structure BSP tag and the openings between them, including the ones the collision BSP closes but
// the eye sees through (a seam of solid thinner than a hair between two pieces of the level, an invisible wall,
// glass, a grate: no solid-looking triangle between the two sides). False if it can't be read.
bool leaf_portals(const std::filesystem::path &bsp, Leaves &out, std::string *why = nullptr, const SeeThrough &see_through = nullptr);

// Where players play: the box around the scenario's player spawns, flags, item spawns and vehicles, with room to
// move around them (a level's floors far outside it, like the ground of a backdrop, aren't sampled). `set` false:
// the scenario couldn't be read (every floor is sampled).
// Where players play: a box around the scenario's spawns, flags, equipment and vehicles, less the trigger volumes
// its name says kill whoever enters (a "kill_volume" the scripts test players against: falling off Deathtrap's
// vehicle onto the plains below).
struct PlayArea {
    bool set = false;
    V3 lo{}, hi{};
    struct Box { V3 corner{}, forward{}, left{}, up{}, extents{}; };   // (as the engine tests a trigger volume)
    std::vector<Box> kill;
    bool killed(const V3 &p) const;
};
PlayArea play_area(const std::filesystem::path &scenario);

struct Options {
    double ratio = 0.6;        // two basins are separate rooms when the opening is at most this share of the narrower one
    double min_room = 0.0;     // rooms smaller than this (cubic world units) join a neighbour (0: automatic)
    bool keep_clusters = true; // divide the map's own clusters only (keeps their sky, fog, sound and weather)
    double min_cell = 0.1;     // the distance grid's finest spacing (world units)
    double max_cells = 6e6;    // and its most cells (a bigger level gets a coarser grid)
    double max_cell = 0.75;    // a level that would need a coarser grid than this is left alone (too big)
    bool use_max = true;       // compare an opening with the wider basin (else the narrower)
    int max_pieces = 100000;   // two clusters whose opening takes more portals than this join (not a doorway)
    bool refine = true;        // move room boundaries onto one plane (a doorway's) when one is near
    int reach = 12;            // how far (in leaves) from where two rooms meet that plane may be
    double refine_seconds = 20;   // time for that at most (the rest of the boundaries stay as they are)
    SeeThrough see_through;    // which level shaders can be seen through (none given: all but shader_environment)
    PlayArea area;             // where players play (play_area), for estimate()
};

// A portal between two clusters as Halo stores it: the "front" cluster is behind its plane (a collision BSP plane),
// the "back" one in front; its corners go counterclockwise around the plane's normal.
struct ClusterPortal {
    int front = -1, back = -1, plane = -1;
    std::vector<V3> points;
};
// The portals between the clusters of leaves (`cl`: each leaf's cluster): the leaf portals between two clusters,
// coplanar neighbours merged. `planes_n`/`planes_d`: the BSP's planes (else each portal's own).
void cluster_portals(const Leaves &leaves, const std::vector<int> &cl, std::vector<ClusterPortal> &out,
                     const std::vector<V3> *planes_n = nullptr, const std::vector<double> *planes_d = nullptr);

// How many of the level's triangles the engine draws, from spots a player's eyes can be (above floors), looking
// around level: from the camera's cluster through the portals it can see (each narrowing the view), as the
// engine's visibility does. A triangle counts when it's in a reached cluster and on screen within that cluster's
// view. `Own`: the tag's clusters and portals; `None`: one cluster (no portals); `Given`: `cl` and `portals`.
enum class Arrangement { Own, None, Given };
struct Drawn { double median = 0, high = 0, mean = 0; int samples = 0, triangles = 0; };   // high: 9 in 10 views draw fewer
bool estimate(const std::filesystem::path &bsp, Arrangement which, const std::vector<int> *cl, const std::vector<ClusterPortal> *portals,
              Drawn &out, const PlayArea &area = PlayArea());

// Views from where players play (as estimate() samples them, with the tag's own clusters and portals): where the
// eyes are, which way they look (level), and the level's surfaces the engine draws.
struct SampleView { V3 eye{}; double yaw = 0; std::vector<int> drawn; };
bool sample_views(const std::filesystem::path &bsp, const PlayArea &area, int count, std::vector<SampleView> &out);

// The clusters add_portals would make: each leaf's new cluster. `report`: what it did, or why it couldn't.
bool plan(const std::filesystem::path &bsp, const Options &options, Leaves &leaves, std::vector<int> &cluster_of_leaf,
          std::string *report = nullptr);

struct Result {
    bool changed = false;
    std::string why;                       // nothing changed: why
    int clusters_before = 0, clusters_after = 0, portals_before = 0, portals_after = 0;
    double drawn_before = 0, drawn_after = 0;             // level triangles drawn from a typical spot (estimate)
    double drawn_high_before = 0, drawn_high_after = 0;   // and the most in 9 views out of 10
};

// A level's triangles, for measuring how much of it a light reaches (the Xbox draws every level triangle a dynamic
// light reaches again, for each light, out of 32,768 triangles a frame).
struct LevelTriangles {
    std::vector<V3> centre;
    std::vector<double> radius;   // each triangle's bounding sphere
    bool load(const std::filesystem::path &bsp);
    int within(const V3 &p, double r) const;   // triangles within r of p
};

// What a dynamic light makes the engine draw again of a level (structure_visibility_build_surfaces): a light of
// radius 2 or more takes whole subclusters (a cluster's pieces of up to about 100 triangles, some spanning much of
// the level) whose bounds touch the box around its sphere, not just the triangles it reaches; under 2, the
// triangles near it. Of those, the ones in view are drawn again, for its diffuse and its specular pass.
struct LevelChunks {
    struct Chunk { V3 lo{}, hi{}; std::vector<int> surfaces; };
    std::vector<Chunk> chunks;
    LevelTriangles triangles;
    int surface_count = 0;
    bool load(const std::filesystem::path &bsp);
    int gathered(const V3 &p, double r) const;   // the level triangles a light of radius r at p takes
};

// Adds portals to a structure BSP tag with one cluster (saved in place), when they'd cut what's drawn by at least
// 15% (estimate()). Its clusters get the one cluster's settings; every cluster may see every other (the portals do
// the culling); the sound data is each pair's distance through the portals.
Result add_portals(const std::filesystem::path &bsp, const Options &options = Options());

}   // namespace portals
