// Halo Map Porter - route points for Halo 1 Unlimited's multiplayer bots, added where they need them.
//
// The bots (Halo 1 Unlimited's bots.c) go between waypoints made from a map's player spawns, item spawn points and
// netgame flags: each dropped onto the ground under it (3 units at most), teleporters first, then spawns, item spots,
// the other flags; one within 1 unit of another is that one; 512 at most, the rest left out. Each links both ways to
// its nearest (a floor up or down counting double), up to 8 within 40 units and at least 2. A hop between two is
// walked with Halo's AI path search (path.c): a best-first search over the level's walkable collision surfaces, from
// one to the next across their shared edges, that opens 1,024 surfaces at most, goes no further than twice the hop
// plus 10 units (60 at most), and stops when it gets no closer. When it doesn't reach the waypoint's surface, the bot
// walks to the closest spot it found and searches again from there: on a level cut into many small surfaces, a hop
// from the ground up to a walkway runs out before it finds the stairs, and the bot stands under its waypoint. Shorter
// hops fix that, and an item spawn point with every game type "none" (it never spawns anything) is a waypoint too.
//
// plan() makes the waypoints and links as the bots do, runs each hop's search as the game does (both ways), and for
// a hop the search can't finish but the level's walkable surfaces do join, follows their shortest way and adds
// waypoints along it, each as far along as a search from the last still finishes (with room to spare), until the
// rest of the way finishes too. Again with the new waypoints' own links, a few times.
#pragma once
#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace botnodes {

using V3 = std::array<double, 3>;

struct Options {
    int budget = 512;          // the bots' waypoints
    int spare = 8;             // left free of that (for the map's later changes)
    int max_add = 48;          // nodes added at most
    int node_limit = 1024;     // the game's search's surfaces at most (PATH_NODE_LIST_SIZE)
    int plan_limit = 640;      // a hop with a node added is planned to open no more than this (room to spare)
    double radius = 0.25;      // the bots' pathfinding radius (a biped's collision radius and a little)
    double jump = 0.8;         // a ledge this high a search's end can be straight from (the cyborg's crouch-jump, 9 in 10)
    double drop = 2.7;         // and a drop this far down (the globals' harmless fall: 3 units, 9 in 10 of it)
    int rounds = 3;            // plans again with the nodes' own links
    bool features = true;      // waypoints along the level's ramps and narrow ways (doorways, tunnels, bridges), and on
                               // open floor far from every waypoint
    int max_features = 200;    // those at most
    std::vector<char> walk_from;   // per point given: bots walk on from it (empty: every point). Not an item spawn point:
                                   // one on a platform only a ladder reaches (Snow Space's sniper rifles) would have the
                                   // floor up there taken as floor bots walk
};

struct Hop {                   // a link, walked one way
    int a = -1, b = -1;        // waypoints (indices into Result::waypoints)
    bool ok = false;           // the game's search finishes it (or ends where a jump or drop does)
    bool joined = false;       // the level's walkable surfaces join its ends (a search with no limits would)
    bool around = false;       // it fails, but the links that work go from a to b another way
    int opened = 0;            // surfaces the game's search opened
    double straight = 0;
};

struct Result {
    bool ok = false;
    std::string why;           // not ok: why
    int given = 0;             // the map's own waypoints (after merging; on the ground)
    int dropped = 0;           // of the points given, ones with no ground under them, or past the budget
    int merged = 0;            // of them, ones within 1 unit of an earlier one's ground (that waypoint; past the
                               // budget, one already counted there)
    int no_ground = 0;         // ones with no ground within 3.5 units under them (no waypoint)
    std::vector<int> left_out; // the points (indices into those given) that would be waypoints but for the budget
    std::vector<int> point_waypoint;   // each point given: its waypoint (into `waypoints`; an earlier point's when
                                       // merged), or -1
    std::vector<V3> waypoints; // the map's, then the nodes added (on the ground)
    std::vector<V3> added;     // the nodes as placed in the scenario (a little above the ground)
    std::vector<int> added_kind;   // each one's: 0 where the search can't finish a hop, 1 a ramp, 2 a narrow way, 3 open
                                   // floor far from every waypoint
    int on_ramps = 0, in_narrow = 0, on_open = 0;
    std::vector<Hop> hops;     // the links (both ways) with the nodes added
    int failing_before = 0, failing_after = 0;   // hops the search can't finish, before and after the nodes
    int stuck_before = 0, stuck_after = 0;       // of those, ones the links that work don't go around either
    int unjoined = 0;          // of those after, ones no walk joins (a jump, a ladder, a lift, a teleporter)
    long reach_before = 0, reach_after = 0;   // of the map's waypoints, pairs (a to b) the links that work join
    std::vector<std::vector<V3>> walks;   // the ways nodes were added along
    int walkable = 0, surfaces = 0;       // the level's walkable collision surfaces, of all of them
};

// `points`: the map's points as the bots take them (in their order: teleporters, spawns, item spots, other flags);
// `teleporters`: how many of them are teleporter ends at the start (way in, its way out, ...: pairs).
// Nothing is added past the budget.
Result plan(const std::filesystem::path &bsp, const std::vector<V3> &points, int teleporters, const Options &options = Options());

// A picture of it from above (a .bmp): the walkable floors by height, the waypoints (blue), the nodes added
// (orange), the ways they were added along (yellow) and the hops still failing (red).
bool picture(const std::filesystem::path &bsp, const Result &r, const std::filesystem::path &bmp);

}   // namespace botnodes
