// Halo Map Porter - animations stored the way the Xbox's own are: keyframes ("compressed"). Each animated node keeps
// only the frames where its motion changes; the engine blends in between (model_animations.c,
// animation_get_keyframe_rotation and friends). Rotations take 6 bytes (four 12-bit components) instead of 8 a frame,
// translations and scales keep their floats, and a node that never moves takes nothing beyond its one default.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace tagfile { struct TagNode; }

namespace animcomp {

struct Pose { double q[4] = {0, 0, 0, 1}; double t[3] = {0, 0, 0}; double s = 1; };   // a node's local orientation

// An animation's frames, every node's pose in every frame (frames x nodes, frame-major).
struct Clip {
    int frames = 0, nodes = 0;
    uint32_t rot[2] = {0, 0}, tr[2] = {0, 0}, sc[2] = {0, 0};   // which nodes animate rotation, translation, scale
    std::vector<Pose> p;
    Pose &at(int f, int n) { return p[(size_t)f * nodes + n]; }
    const Pose &at(int f, int n) const { return p[(size_t)f * nodes + n]; }
};

static inline bool flag(const uint32_t f[2], int n) { return n < 64 && ((f[n >> 5] >> (n & 31)) & 1); }

// A tag's uncompressed animation (HEK: big endian): `frame_data` holds each frame's animated values node by node
// (rotation 4 x int16, translation 3 floats, scale float, each if the node animates it), `default_data` the rest
// once. False if the sizes don't add up.
bool decode_tag_frames(const std::string &default_data, const std::string &frame_data, Clip &c);
// The compressed data (little endian, as the Xbox reads it) as the engine samples it at each whole frame.
bool decode_compressed(const std::string &blob, Clip &c);

// Compressed data for a clip. A track gets a keyframe wherever leaving it out would move the node, between the
// keyframes around it, more than `angle` (radians) / `units` / `scale`. Empty if the clip can't be stored this way
// (more than 4,095 keyframes on a node; a still node scaled other than 1, which the format can't hold).
std::string encode(const Clip &c, double angle, double units, double scale);

// The farthest any node, or a point a bone's length out from it along each of its axes, ends up from where `a` puts it
// in any frame, in world units, with the nodes posed down the hierarchy (`parent`: each node's parent, -1 for a root).
// `lever`: the length to measure each node's turn at (its bones' length).
double world_error(const Clip &a, const Clip &b, const std::vector<int> &parent, const std::vector<double> &lever);

// Each node's lever for world_error: the distance to its farthest child at rest (frame 0 of `c`), or for a node with no
// children its own distance from its parent (a finger's last joint), at least `minimum`.
std::vector<double> levers(const Clip &c, const std::vector<int> &parent, double minimum);

// A .model_animations tag's animations that aren't compressed yet, compressed, in an editable copy of the tag (its
// root). Each keeps its frames as they were (and the tag stays readable everywhere): the compressed data follows them
// in its frame data, and the animation is flagged compressed, with "offset to compressed data" pointing at it, as
// Bungie's tools save it; a map build keeps only the compressed data. `level` 1: every track within 0.05 degrees /
// 0.0001 world units of its frames (the 6-byte rotations themselves round to about that), and no bone end (world_error)
// more than 0.0003 units further off than the rounding alone puts it; 2: 0.2 degrees / 0.0004, 0.002 units. Tighter
// tolerances are tried before an animation is left as it is, as it also is if it would come out no smaller.
struct GraphResult {
    int animations = 0, compressed = 0;   // uncompressed animations before; compressed now
    size_t before = 0, after = 0;         // their data in a map (frame info, default data, frames / the compressed data)
    double worst = 0;                     // the largest world_error of those compressed
};
GraphResult compress_graph(tagfile::TagNode &root, int level);

}   // namespace animcomp
