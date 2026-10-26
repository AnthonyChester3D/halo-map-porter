// Halo Map Porter - the level's shaders on the Xbox: what each costs, what its reflection adds to what you see, and
// a software preview of the level (textures, lightmaps, reflections) to compare them with and without.
//
// The Xbox draws every level surface at least twice (its lightmap, then its textures). A shader with a reflection
// (cube map, perpendicular or parallel brightness above 0) draws it again; once more when its lightmap brightness
// scale is under 1 (a mask darkening the lightmap under the reflection); and once more for a specular lightmap
// (rasterizer_xbox_environment.c). On a level where most surfaces reflect, that's most of its triangles drawn 3-4
// times. Dropping a reflection (both brightnesses 0, no cube map) saves those passes and the cube map's memory.
#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "portals.hpp"

namespace lshade {

// Where a tag's file is ("" when it isn't anywhere), by tag path with its extension.
using Find = std::function<std::filesystem::path(const std::string &tag)>;

struct Shader {
    std::string tag;                 // the shader_environment tag ("levels/x/shaders/metal.shader_environment")
    int triangles = 0;               // level triangles drawn with it
    int extra_passes = 0;            // passes each of its triangles costs beyond the two every surface gets
    int reflection_passes = 0;       // of those, the ones its reflection costs (dropping it saves them)
    float perpendicular = 0, parallel = 0;   // reflection brightness looking straight at it, along it
    std::string cube_map;
    double drawn = 0;                // its triangles the engine draws in an average view
    double shows = 0;                // how much of what you see is its reflection (share of the picture's brightness)
    double strength = 0;             // how much its reflection adds where it is (share of those pixels' brightness)
    double coverage = 0;             // how much of the screen it covers in an average view
    bool suggested = false;          // dropping its reflection is suggested
};

// The level's shaders and what the Xbox draws of them from where players play.
struct Analysis {
    std::string map_name;
    std::vector<Shader> shaders;     // shader_environment ones (the others, see-through, are counted in `other`)
    int views = 0;
    std::vector<int> drawn;          // views x shaders: triangles of each shader the engine draws in that view
    std::vector<int> other;          // per view: other level triangles drawn (see-through shaders: one pass each)
    std::vector<float> shows;        // rendered views x shaders: its reflection's share of that view's brightness
    int rendered = 0;                // views rendered for `shows` (the first ones of `views`)
};

// The cost of the level with the reflections of `dropped` (one flag per shader) gone: triangle passes the Xbox
// draws in a typical view (half of the views draw more) and a busy one (one in ten draws more).
struct Cost { double typical = 0, busy = 0; };
Cost cost(const Analysis &a, const std::vector<char> &dropped);

// Stock Xbox maps' levels, the same way (for comparison): Sidewinder's, among the busiest that still run smoothly.
Cost stock_cost();

// The Xbox's processor time a reflection pass costs, as well as the graphics chip's: the level's triangles are sent
// again. Measured on an Xbox (Halo 1 Unlimited's perf log, Deathtrap as made and with its reflections dropped, at
// the same spots): 2,912 / 8,346 fewer passes in a typical / busy view took 1.4 ms (on average) / 3.2 ms (its busiest
// view) off each frame's drawing, about 0.45 ms per 1,000 passes.
constexpr double kXboxCpuMsPer1000Passes = 0.45;

// The level's geometry and textures, for drawing it.
struct Texture { int w = 0, h = 0; std::vector<uint8_t> rgba; };
struct Material {
    int shader = -1;                 // in Analysis::shaders (-1: a see-through shader)
    int base = -1, detail = -1, cube = -1;   // textures (cube: six faces side by side, +x -x +y -y +z -z)
    float detail_scale = 1;
    bool mask_by_base_alpha = false; // the reflection is masked by the base map's alpha
    float perpendicular[3] = {0, 0, 0}, parallel[3] = {0, 0, 0};   // reflection colour x brightness
    float lightmap_scale = 1;        // how much the reflection keeps where the lightmap is dark (1: all of it)
    bool see_through = false;
    // Alpha-tested: its cut-out, the bump map's alpha (drawn only where it's over half, as the game draws it), and
    // the bump map's scale on the texture coordinates.
    int cutout = -1;
    float cutout_scale = 1;
};
struct Vertex { float p[3], n[3], uv[2], luv[2]; };
struct Triangle { Vertex v[3]; int material = -1, lightmap = -1; };   // lightmap: texture of its lightmap page
// A portal between two of the level's rooms (clusters): the opening the engine looks through from one to the other.
struct Portal { std::vector<std::array<float, 3>> points; int front = -1, back = -1; };

// A sky's fog: from `start` to `opaque` units in front of the camera it thickens to `density` of `color`. At density
// 1 (and only then) the engine draws nothing past `opaque`: it moves the view's far end there (render.c).
struct Fog {
    float color[3] = {0, 0, 0};
    float density = 0, start = 0, opaque = 0;
    bool any() const { return density > 0 && opaque > 0; }
    bool cuts() const { return density >= 1.0f && opaque > 0; }
};
// The level's heaviest rain or snow (its BSP's weather palette): the drops the engine keeps around the camera as made
// and with each of the level window's weather choices (perf::weather_lighten), and how far away it ends.
struct Weather {
    int32_t systems = 0;
    float made = 0, lighter = 0, half = 0;
    float end = 0, lighter_end = 0;
};
// What else of the map's costs the level window offers to change: the units its scripts give AI while attached to
// another object (perf::script_riders), and the small props with very detailed collision (perf::kDetailedProp...).
struct Content {
    int32_t riders = 0;
    int32_t detailed_props = 0;          // placed
    char prop_names[160] = {};           // "chair pilot (416 surfaces), ..."
    // The dynamic lights on the objects the map places (Config::xbox_object_lights): attached to them, or made by an
    // effect attached to them; how many objects carry one.
    int32_t object_lights = 0;
    char light_names[160] = {};          // "headlights light (3 placed), ..."
};
// What the showcase's shots look at and keep clear of (showcase.hpp), from the scenario: its CTF flags and power items
// (rocket launchers, sniper rifles, fuel rods, flamethrowers, overshields, camouflage), the other places players are
// (where they spawn, the other items' spawn points, the other game types' flags: hills, teleporters, race points, the
// vehicles a game type spawns), and the boxes around the objects it places (scenery, machines, controls, light
// fixtures: each model's box, turned as placed), which the level's own triangles don't have.
struct Mark {
    enum Kind : int32_t { kFlag = 0, kPower = 1, kSpawn = 2, kItem = 3, kPlace = 4, kVehicle = 5 };
    float p[3] = {0, 0, 0};
    int32_t kind = kFlag;
    char name[32] = {};
};
struct Box { float c[3] = {0, 0, 0}, axis[2][2] = {{1, 0}, {0, 1}}, half[3] = {0, 0, 0}; };   // (axes: turned about z only)
// The objects in the level when a game starts, drawn where they are: what the scenario places (scenery, machines,
// controls, light fixtures, vehicles a multiplayer game type spawns, weapons, equipment) and the items on its
// multiplayer spawn points (each one's item collection's first item). Each draws its model's most detailed geometry
// (each region's first permutation) with its shaders' base maps. Lit as the engine lights an object (object_lights.c,
// lights_prepare_for_object_static): straight down from its bounding sphere's centre and from four points around it,
// the level's lightmap where each ray meets it, as an ambient light and two distant ones (one from where the lightmap's
// light comes from there, one bounced up off the surface in its colour), averaged; the level's default lighting where
// none meets a lit surface.
struct ModelVertex { float p[3], n[3], uv[2]; };   // in the model's own space (x forward, y left, z up)
struct ModelPart {
    enum : int32_t { kCutout = 1, kTwoSided = 2, kSeeThrough = 4, kAdd = 8, kMultiply = 16, kDoubleMultiply = 32 };
    int32_t first = 0, count = 0;   // its triangles (Model::tris, 3 corners each, wound counterclockwise seen from the front)
    int32_t texture = -1;           // its base map (Preview::textures; a transparent shader's first map)
    int32_t flags = 0;              // cut out where its base map's alpha is half or less (alpha tested), drawn from both
                                    // sides, see-through (a transparent shader, or an alpha-blended decal): blended by
                                    // its alpha, or as its shader's framebuffer blend function says (added, multiplied,
                                    // multiplied twice)
    int32_t shader = -1;            // Model::shaders
};
struct Model {
    std::string tag;
    std::vector<ModelVertex> vertices;
    std::vector<uint32_t> tris;
    std::vector<ModelPart> parts;
    std::vector<std::string> shaders;   // its parts' shader tags
};
struct ObjectLight {   // the engine's render_lighting: an ambient colour and two distant lights (colour, direction it shines)
    float ambient[3] = {0.2f, 0.2f, 0.2f};
    float color[2][3] = {{1, 1, 1}, {0.4f, 0.4f, 0.5f}};
    float direction[2][3] = {{-0.577f, -0.577f, -0.577f}, {0, 0, 1}};
    // The light on a surface facing `n` (world space, unit): ambient, plus each distant light where it faces it.
    void at(const float n[3], float out[3]) const;
};
struct Object {
    enum Kind : int32_t { kScenery, kMachine, kControl, kLightFixture, kVehicle, kWeapon, kEquipment, kItem, kKinds };
    int32_t model = -1;              // Preview::models
    int32_t kind = kScenery;
    float axes[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};   // its forward, left and up in the world
    float p[3] = {0, 0, 0};
    ObjectLight light;
    int32_t lit = 0;                 // lightmap samples it was lit by (0: the level's default lighting)
    char tag[96] = {};               // its object tag ("vehicles/warthog/warthog.vehicle")
};
const char *object_kind(int kind);   // "scenery", "a machine", ... (for people)
struct Preview {
    std::vector<Triangle> triangles;
    std::vector<Material> materials;
    std::vector<std::string> material_tags;   // per material: its shader's tag path
    std::vector<Texture> textures;
    std::vector<portals::SampleView> views;   // from where players play (drawn surfaces left out to keep it small)
    std::vector<Portal> portals;
    std::vector<int16_t> room;                // per triangle: its cluster (-1: none)
    int rooms = 0;                            // clusters
    // Fog: each of the scenario's skies' outdoor fog, the first one's indoor fog (rooms with no sky), each room's sky.
    std::vector<Fog> sky_fog;
    Fog indoor_fog;
    std::vector<int16_t> room_sky;
    Weather weather;
    Content content;
    std::vector<Mark> marks;
    std::vector<Box> boxes;
    std::vector<Model> models;
    std::vector<Object> objects;
    // Which room a point is in: the level's BSP tree (child: a node, ~leaf for a leaf (-2 and less), -1 solid).
    struct Node { float n[3] = {0, 0, 0}; float d = 0; int32_t front = -1, back = -1; };
    std::vector<Node> tree;
    std::vector<int16_t> leaf_room;
    int room_at(const std::array<double, 3> &p) const;   // -1: in a wall, or outside the level
    // The fog the engine uses with the camera at `eye`: its room's sky's outdoor fog (`outdoor` true), else the
    // indoor fog.
    Fog fog_at(const std::array<double, 3> &eye, bool *outdoor = nullptr) const;
};

// Reads the level `bsp` (and the scenario, for where players play): its shaders, what's drawn of each from player
// spots, and its preview. False with `why` when it can't be read.
bool analyze(const std::filesystem::path &bsp, const std::filesystem::path &scenario, const Find &find, Analysis &a,
             Preview &p, std::string *why = nullptr);

// Which reflections to drop: ones that barely show, then, while the level costs more than stock ones
// (stock_cost()), the ones that show least for what they cost. Sets Shader::suggested.
void suggest(Analysis &a);

// Draws the level from a view (eye, yaw and pitch in radians, horizontal field of view 70 degrees) into `px`
// (w x h, top row first, 0x00RRGGBB). `reflections`: per shader, whether its reflection is drawn. `highlight`: a
// shader to tint (-1 none). `shown`: if given, per shader, its pixels, their brightness and its reflection's part of
// it; `total` the whole picture's brightness. Extras: `portals` outlined (where nothing is in front of them) and each
// room tinted a colour of its own; `fog` (by distance in front of the camera, as the engine fogs; nothing past its
// opaque distance when it cuts); `objects` drawn (Preview::objects).
struct Camera { std::array<double, 3> eye{}; double yaw = 0, pitch = 0; };
struct Shown { double reflection = 0, brightness = 0; int pixels = 0; };
struct Extras { bool portals = false; bool fog_on = false; Fog fog; bool objects = false; };
void draw(const Preview &p, const Camera &c, const std::vector<char> &reflections, int highlight, int w, int h,
          std::vector<uint32_t> &px, std::vector<Shown> *shown = nullptr, double *total = nullptr,
          const Extras *extras = nullptr);

// The surface at pixel (x, y) of a w x h picture drawn from `c` (as draw and the window's picture see it: the nearest
// triangle there that's drawn, facing the camera unless see-through, not cut away by its cut-out): its triangle, or -1.
// `hit`: where the ray meets it. `object` (given: the objects are in the picture): the object there, in front of the
// level (its index in Preview::objects, and `part` its model's part), else -1; the triangle is then -1.
int pick(const Preview &p, const Camera &c, int w, int h, double x, double y, std::array<double, 3> *hit = nullptr,
         int *object = nullptr, int *part = nullptr);

// The analysis and preview in a file (for the level shaders window), and back.
bool save(const Analysis &a, const Preview &p, const std::filesystem::path &file);
bool load(Analysis &a, Preview &p, const std::filesystem::path &file);

// Drops the reflection of a shader_environment tag (saved in place): both brightnesses 0, no cube map.
bool drop_reflection(const std::filesystem::path &shader);

}   // namespace lshade
