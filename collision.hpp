// Halo Map Porter - object collision (.model_collision_geometry): its BSPs read and written, tested the way the engine
// tests them (physics/collision_bsp.c), simplified, and built again.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace tagfile { class Elem; struct TagNode; }

namespace coll {

const uint32_t kNone = 0xFFFFFFFFu, kLeaf = 0x80000000u;

struct Plane { double n[3] = {0, 0, 1}, d = 0; };
struct Node3 { uint32_t plane = 0, child[2] = {kNone, kNone}; };   // child[0] behind the plane, [1] in front; leaf | kLeaf; kNone solid
struct Leaf { uint16_t flags = 0, refs = 0; uint32_t first = 0; };   // flags 1: contains two-sided surfaces
struct Ref { uint32_t plane = 0, root = kNone; };   // plane | kLeaf: the leaf is behind it; root: a 2D node, or surface | kLeaf
struct Node2 { double n[2] = {1, 0}, d = 0; uint32_t child[2] = {kNone, kNone}; };   // child[0] distance < 0
struct Surface { uint32_t plane = 0, first_edge = 0; uint8_t flags = 0; int8_t breakable = -1; uint16_t material = 0xFFFF; };
struct Edge { uint32_t v[2] = {0, 0}, next[2] = {0, 0}, s[2] = {0, 0}; };   // s[0] the left surface (v[0] -> v[1]), s[1] the right
struct Vertex { double p[3] = {0, 0, 0}; uint32_t first_edge = 0; };
enum SurfaceFlags { kTwoSided = 1, kInvisible = 2, kClimbable = 4, kBreakable = 8 };

struct Bsp {
    std::vector<Node3> nodes;
    std::vector<Plane> planes;
    std::vector<Leaf> leaves;
    std::vector<Ref> refs;
    std::vector<Node2> nodes2;
    std::vector<Surface> surfaces;
    std::vector<Edge> edges;
    std::vector<Vertex> vertices;
    size_t bytes() const;   // in a map
};

bool read_bsp(const tagfile::Elem &bsp, Bsp &out);
void write_bsp(tagfile::TagNode &bsp, const Bsp &b);   // replaces the element's blocks

// A surface's corners in order (its edge ring), empty if the ring is broken.
std::vector<uint32_t> surface_ring(const Bsp &b, uint32_t surface);

// collision_bsp_test_vector: the first surface a segment from p along v (0..max_t of v) hits, front faces as the
// engine's usual tests (_collision_test_front_facing_surfaces). surface kNone: none.
struct Hit { uint32_t surface = kNone; double t = 1; };
Hit test_vector(const Bsp &b, const double p[3], const double v[3], double max_t = 1, bool back_faces = false);

// collision_bsp_test_sphere: the features (surfaces, edges, vertices) a sphere gathers, as a biped's physics does each
// tick; each kind holds at most 256 (add_feature).
struct Features { int surfaces = 0, edges = 0, vertices = 0, leaves = 0; long ops = 0; };   // ops: about how much work it took
Features test_sphere(const Bsp &b, const double c[3], double radius);

// A collision surface as a polygon: corners in order (seen from the front, counterclockwise), and what it is.
struct Poly {
    std::vector<std::array<double, 3>> pts;
    uint16_t material = 0xFFFF;
    uint8_t flags = 0;
    int8_t breakable = -1;
    int group = -1;   // (for building: surfaces of one closed shape among several share edges only with each other)
};
std::vector<Poly> polygons(const Bsp &b);

// The first front face a segment from p along v (0..1) crosses, by testing every polygon (the truth a BSP should give).
Hit first_face(const std::vector<Poly> &polys, const double p[3], const double v[3]);

// How often a BSP's vector test gives what first_face does for its own surfaces, on `rays` random segments through its
// box (the share that agree, 0..1).
double self_consistency(const Bsp &b, int rays = 2000, unsigned seed = 1);

// A BSP for polygons (convex, at most 8 corners, closed or with two-sided ones), as Bungie's tool makes them: solid
// behind the surfaces, empty leaves in front, each leaf knowing the surfaces on the planes around it. False if it can't.
bool build(const std::vector<Poly> &polys, Bsp &out, std::string *why = nullptr);

// How much a biped's physics gathers at worst touching the shape: the most features (surfaces, edges and vertices)
// any sphere of `radius` resting a little in front of a surface collects.
int worst_features(const Bsp &b, double radius = 0.25, long *worst_ops = nullptr);

// How much work a biped's movement does at worst against it (the sphere it gathers features in, resting a little in
// front of each surface): the shape's cost to the game. Bungie's own scenery is at most about 115,000.
long sphere_work(const Bsp &b);

// A lighter copy of a dense shape: the same surfaces in a leaner BSP if that's enough; else made again the way Bungie
// made collision for their vehicles: a few convex pieces with big flat faces on the model's own planes, sitting just
// outside it; hollows, gaps and small details filled over; nothing thin (thin colliders let vehicles slice in and
// jitter); as many pieces as stay within `budget`. Not for two-sided or breakable surfaces.
struct Lightened {
    bool changed = false;
    long work_before = 0, work_after = 0;
    size_t surfaces_before = 0, surfaces_after = 0;
    int pieces = 0;                     // convex pieces it was made of (0: the same surfaces, a leaner BSP)
    double moved = 0, moved_most = 0;   // how far the new surfaces lie from the old: 99% of them within, and at most
    double lost = 0;                    // 99% of the old surfaces within this of the new (details filled over show here)
    std::string why;                    // why not, when not changed
};
Lightened lighten(const Bsp &in, long budget, Bsp &out);

// ---- Bungie's own collision where it fits: most custom vehicles (and their wrecks) are iterations of a stock one.

// A collision model's nodes: names and BSPs.
struct NodeBsps { std::string name; std::vector<Bsp> bsps; };
std::vector<NodeBsps> read_nodes(const tagfile::Elem &collision);

// A model at rest: its nodes' names and places (object space: a 3x3 rotation, row by row, then a translation), and the
// visual triangles each node moves (object space, its most detailed version of each region's base permutation).
struct Rig {
    std::vector<std::string> names;
    std::vector<std::array<double, 12>> world;
    std::vector<std::vector<std::array<double, 9>>> tris;
};
bool read_rig(const tagfile::Elem &model, Rig &out);

// For each of a custom collision model's nodes with BSPs, the stock model's BSPs of the node of the same name, where
// they sit on this model's visual triangles of that node no worse than on the stock model's own (within a quarter or
// 2 cm, both ways: the collision's distance to what's drawn, and what's drawn to the collision). Placed by this
// model's node (as the game does), each surface given the material of the nearest of the node's own surfaces.
// Nodes the stock collision doesn't have, has no BSP for, or that move too few triangles to judge, keep their own.
struct StockFit {
    std::vector<std::vector<Bsp>> bsps;   // per custom node: its new BSPs (empty: kept)
    std::vector<std::string> taken;       // the nodes that took the stock collision
    size_t surfaces_before = 0, surfaces_after = 0;   // of the nodes taken
};
StockFit fit_stock(const std::vector<NodeBsps> &own, const Rig &own_rig, const std::vector<NodeBsps> &stock, const Rig &stock_rig);

// From points spread over the surfaces, the distance straight in to the far side (sorted): how thin the shape is.
std::vector<double> thickness_profile(const Bsp &b, int samples = 3000);

}   // namespace coll
