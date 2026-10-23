// Halo Map Porter - conversion engine (portable: Windows + Linux for testing)
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace porter {

namespace fs = std::filesystem;

enum class Game { MCC = 0, CE = 1, Xbox = 2, Unknown = 3 };

const char *game_name(Game g);
const char *game_engine(Game g); // invader-build -g value

using Log = std::function<void(const std::string &)>;

struct Config {
    fs::path invader_dir;  // folder containing invader-*.exe
    fs::path ce_dir;       // Halo Custom Edition install (has maps\)
    fs::path mcc_maps_dir; // MCC ...\halo1\maps
    fs::path ek_dir;       // H1A-EK root (has tags\ and data\)
    fs::path xbox_tags_dir; // Xbox tagset (e.g. haloxbox-revised), preferred
    fs::path xbox_maps_dir; // Xbox stock maps folder (fallback source of stock tags)
    fs::path cache_dir;    // app cache (stock tags, work dirs, reports)
    // Xbox-only build options
    bool xbox_shrink = false;  // halve the map's own textures until it fits the Xbox size limit
    double xbox_max_mb = 0;    // 0 = the standard Xbox limit (47 MB); otherwise a modded Xbox's bigger limit
    // Texture memory target (MB): the memory all the map's textures take (stock weapons, vehicles and HUD included),
    // plus the tag memory the map leaves free, which OpenCE (test build 24 and later) lends to its texture cache.
    // Over it, "Shrink textures to fit" halves the map's own textures until they fit. 0 = no target.
    double xbox_texture_target_mb = 20;
    // Xbox multiplayer slots. The Xbox engine only spawns the vehicles in the first three entries of the
    // globals multiplayer vehicle list. Empty = keep the Xbox default for that slot.
    std::string xbox_vehicle_slots[3];
    // Halo Universal XBE: every placed vehicle can spawn (with the gametype's vehicles on Default), so keep
    // vehicles that have no slot of their own type instead of swapping them for a slotted one or removing them.
    bool xbox_keep_all_vehicles = false;
    // PC-only weapons to keep on Xbox instead of swapping them for an Xbox weapon (weapon tag paths).
    std::vector<std::string> xbox_keep_weapons;
    // PC-only weapons that are NOT kept: weapon tag path -> Xbox weapon name (e.g. "shotgun").
    // Missing entries use the default replacement (flamethrower -> shotgun, fuel rod -> rocket launcher).
    std::map<std::string, std::string> xbox_weapon_replacements;
    // Swap the map's own copies of stock weapons, vehicles and the player model (e.g. a custom tagset's
    // HD sniper rifle) for the Xbox originals that every Xbox multiplayer map already carries.
    bool xbox_use_stock_versions = true;
    // Models with lower-detail versions but no switch distances always draw in full; the map's own get stock-style
    // distances. This also does it for stock Xbox models (36 ship that way, mostly trees, rocks and plants).
    bool xbox_lod_stock_models = false;
    // Make lower-detail versions (LODs) for the map's own models that have none (lod.hpp), kept in the LOD library
    // (empty: cache_dir/lod_library) so the same model in another map reuses them.
    bool xbox_make_lods = true;
    fs::path lod_library;
    // The most detailed version of the map's own heavy models made lighter (lod.hpp trim_top), before any LODs are
    // made from it: 0 as made, 1 trimmed so they look the same (shape kept within 0.25% of the model's size, 0.18% for
    // characters, 0.12% for first-person weapons), 2 trimmed harder (0.6%, 0.4%, 0.25%: small details may change). Only
    // models over their kind's budget (vehicles and scenery 4,000 triangles, characters and first-person weapons 3,000,
    // weapons and items 2,000), and never below it.
    int xbox_trim_models = 1;
    // Store the map's animations (its own and the Xbox's it uses) the way Bungie stored the Xbox's cinematics: as
    // keyframes ("compressed", animcomp.hpp), about half the tag memory for first-person animations, a quarter less
    // for characters. 0 as made; 1 compress (every bone within the rounding the Xbox's 6-byte rotations make anyway);
    // 2 compress harder (fast moves can be a few millimetres off).
    int xbox_compress_animations = 1;
    // Collision of the map's own vehicles, scenery and devices made lighter (collision.hpp): first, parts that match a
    // stock vehicle's (by name) take Bungie's own collision where it sits on the model as well as on Bungie's (most
    // custom Warthogs and their wrecks); then scenery and devices still too dense for bipeds to walk against are made
    // again the way Bungie made theirs: a few convex pieces with big flat faces, just outside the model, gaps and small
    // details filled over, nothing thin. 0 as made; 1 when one costs more than Bungie's densest (about 130,000 steps of
    // a biped's collision test); 2 harder (90,000). The window sets it with the heavy-models choice.
    int xbox_collision = 1;
    // Give a level made without portals (one cluster: everything in view is drawn, walls or not) clusters and portals
    // at its doorways and corridors (portals.hpp), when that cuts what the Xbox draws.
    bool xbox_add_portals = true;
    // And divide further the clusters of levels made with portals of their own, when that draws 15% less (each keeps
    // its cluster's sky, fog, sound and weather); also a second time over a level the app just gave portals.
    bool xbox_replan_portals = false;
    // The level's fog (its skies' outdoor fog): 0 automatic: fog that's nearly opaque (90% or more) is made fully
    // opaque, so the Xbox draws nothing past it (the engine moves the view's far end to the fog's opaque distance
    // only at full density); 1 as the map has it; 2 fully opaque at `xbox_fog_scale` x its own opaque distance; 3
    // fully opaque at `xbox_fog_distance` world units.
    int xbox_fog = 0;
    double xbox_fog_scale = 1.0;
    double xbox_fog_distance = 0;
    // The level's rain or snow (perf::weather_lighten): 0 as made, 1 lighter (as dense up close, ending at 5 units), 2
    // half the drops, 3 none.
    int xbox_weather = 0;
    // Units the scripts give AI while attaching them to another object (perf::script_riders): their ai_attach calls
    // are left out, so they sit there without thinking.
    bool xbox_rider_ai_off = false;
    // Small scenery with very detailed collision (perf::kDetailedProp...): its collision left out (players and shots
    // pass through it).
    bool xbox_detailed_props_off = false;
    // With an Xbox build: a camera tour for Halo 1 Unlimited's perf log (its spawns, busiest and typical views),
    // written to perf\tours\<map>.txt beside the map.
    bool write_perf_tour = false;
    // Scenery and devices not drawn once smaller on screen than this many pixels across (their models' smallest
    // switch size raised to it; 0: as the map has them).
    double xbox_scenery_pixels = 0;
    // Level shaders' reflections (levelshaders.hpp): kept unless this map's choice in the level shaders window says
    // otherwise: drop the suggested ones (the ones that show least for the Xbox passes they cost, while the level costs
    // more than stock ones), or `xbox_reflection_picks` (exactly these shader tags lose their reflections).
    bool xbox_drop_reflections = false;
    bool xbox_reflection_picks_set = false;
    std::set<std::string> xbox_reflection_picks;
    // Only measure the level's shaders (no build): Result::shaders is the file for the level shaders window.
    bool level_shaders_only = false;
    // Custom weapons with no Xbox original: empty = keep them, else an Xbox weapon name (e.g. "sniper rifle").
    std::string xbox_other_weapons;
    // Largest side for the map's own world textures (stock Xbox maps stay at 512) and for lightmaps.
    // 0 = no limit. HUD/menu textures and stock tags are never scaled.
    int xbox_max_texture = 512;
    int xbox_max_lightmap = 0;
    double xbox_level_texture_mb = 6;  // level (BSP) texture budget in MB, like stock maps; 0 = none
    int xbox_max_object_texture = 256; // props, vehicles, effects (not the level or sky); 0 = same as xbox_max_texture
    // Weapons & vehicles window picks: weapon/vehicle tag -> replacement tag ("" = none).
    std::map<std::string, std::string> xbox_replace;
};

// A model to show in the weapons & vehicles window: its most detailed version (each region's first permutation),
// with each part's base texture made small.
struct ModelPreview {
    std::vector<float> pos, uv;    // per vertex: x y z (world units, z up); u v (texture repeats, as Halo maps it)
    std::vector<uint32_t> tris;    // 3 vertex indices per triangle, in Halo's winding
    struct Part { uint32_t first = 0, count = 0; int texture = -1; };   // a run of triangles and its texture (-1 none)
    std::vector<Part> parts;
    struct Texture { int w = 0, h = 0; std::vector<uint8_t> rgba; };   // top row first
    std::vector<Texture> textures;
    int triangles() const { return (int)(tris.size() / 3); }
};

// The Xbox tab's "Weapons & vehicles" window: each weapon and vehicle the map can give a player, what it costs
// in the Xbox's 22 MB of tag memory and in the map file, and what the picks (replace with another one, or none)
// would save. Costs come from an analysis build of the map (Invader with the tag space limit lifted), read back
// tag by tag, so shared parts (a family of weapons using one model) are only counted once.
struct ContentItem {
    std::string tag;       // tag path with extension, forward slashes
    std::string name;      // short name to show
    bool vehicle = false;
    bool in_map = true;    // false: an Xbox stock weapon/vehicle offered as a replacement
    std::string used_by;   // e.g. "18 spawn points, weapon list"
    double memory_mb = 0;  // tag memory freed if only this one is replaced with none (a stock one: what it adds)
    double file_mb = 0;    // map file size freed (or added)
    int seats = -1;            // a vehicle's seats (-1: a weapon, or unknown)
    std::string seat_roles;    // e.g. "driver, gunner, 2 passengers"
    std::string piece_of;      // a vehicle with no seats the map's scripts create and attach to this one (its tag)
    int preview = -1;          // its model in ContentAnalysis::previews (-1: none)
};
struct ContentAnalysis {
    std::string error;
    std::string map_name;
    double limit_mb = 22, file_limit_mb = 47;
    double used_mb = 0;            // tag memory as the map converts now (before Invader's optimize)
    double optimize_saves_mb = 0;  // what the optimize rebuild saved on top, when it's needed
    double file_mb = 0;            // uncompressed map size
    std::vector<ContentItem> items;
    // For estimate(): the map's tags and what they use.
    std::vector<std::string> tags;
    std::vector<uint64_t> bytes, file_bytes;
    std::vector<std::vector<int>> deps;
    std::vector<int> roots;
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> geometry;   // model vertex/index ranges per tag
    double fixed_mb = 0, fixed_file_mb = 0;                              // what no pick changes (level BSP, ...)
    std::vector<ModelPreview> previews;                                  // the items' models (shared ones once)
};
struct ContentEstimate { double memory_mb = 0, memory_optimized_mb = 0, file_mb = 0; };   // optimized: roughly, after Invader's optimize
// Tag memory and file size with these picks (weapon/vehicle tag -> replacement tag, "" = none).
ContentEstimate estimate_contents(const ContentAnalysis &a, const std::map<std::string, std::string> &picks);
// Runs the Xbox preparation of a map (input = a .map, or a .scenario in a tags folder when in_game is Unknown)
// and measures it. Takes about as long as a conversion.
bool analyze_xbox_contents(const Config &cfg, const fs::path &input, Game in_game, ContentAnalysis &out, const Log &log);
// Prepares the map for the Xbox as a conversion would (without building it) and measures its level's shaders:
// `file` the level shaders file (levelshaders.hpp save()). `in_game` Unknown: `input` is a scenario in a tags folder.
bool analyze_level_shaders(const Config &cfg, const fs::path &input, Game in_game, fs::path &file, std::string &error, const Log &log);
// A measurement saved to / read from a file (read: false if missing, damaged or from another version). The models
// go in a second file beside it (`file` + ".models"); without it the measurement still reads, with no models.
bool save_content_analysis(const ContentAnalysis &a, const fs::path &file);
bool load_content_analysis(ContentAnalysis &a, const fs::path &file);

// An Xbox build's models whose levels of detail it changed (lower-detail versions made, or switch distances given),
// to look at in the "LODs" window: each detail level drawn, and where the game switches to it.
struct LodView {
    struct Model {
        std::string tag;                 // the model tag
        std::string name;                // the object it's drawn for ("mp_boulder_granite_medium")
        char kind = 's';                 // v vehicle, s scenery (and machines...), w weapon or item, f first person
        std::string change;              // "made" (lower-detail versions), "library" (reused from the LOD library),
                                         // "distances" (switch distances for its own), "stock" (a stock model's),
                                         // "trimmed" (only its most detailed version made lighter)
        bool undone = false;             // taken out again: the map didn't fit with them
        int placed = 0;                  // how many are on the map (placed, or at weapon/item spawn points)
        double cutoffs[5] = {0, 0, 0, 0, 0};   // switch distances, pixels across (super high, high, medium, low, super low)
        double radius = 0;               // the model's size (world units), for the distances in metres
        int level[5] = {-1, -1, -1, -1, -1};   // each level's drawing in `previews` (levels sharing a version share it)
        int trimmed_from = 0;            // its most detailed version was trimmed: its triangles as the map made it
        int as_made = -1;                // and that version's drawing in `previews`
    };
    std::string map_name;
    std::vector<Model> models;
    std::vector<ModelPreview> previews;
};
bool save_lod_view(const LodView &v, const fs::path &file);   // (and `file` + ".models")
bool load_lod_view(LodView &v, const fs::path &file);

struct Result {
    bool ok = false;
    fs::path lods;                     // an Xbox build: its LodView, when it changed any model's levels of detail
    fs::path shaders;                  // an Xbox build: its level shaders (levelshaders.hpp, for the window)
    std::string memory;                // an Xbox build: its tag memory and texture memory, for the log and report
    std::string error;                 // human readable reason when !ok
    std::vector<std::string> changes;  // automatic fixes
    std::vector<std::string> warnings; // build warnings
    std::string info;                  // invader-info output
    fs::path output;
    fs::path report;
};

// What a built map offers, for the Halo Universal XBE's map browser (build 20): the info.txt lines "Gametypes:" and
// "Players:", and genre suggestions.
struct MapTraits {
    std::string gametypes;               // "CTF, King, Oddball, Race, Slayer" (Slayer always; the rest from the map's flags)
    int pmin = 0, pmax = 0;              // a guess from the spawn points and vehicles (0 = fewer than 2 spawn points)
    int spawns = 0, vehicles = 0;        // player spawn points; placed vehicles
    double spread[3] = {0, 0, 0};        // the box the spawn points fill (m)
    std::string players_why;             // what the guess is based on, for the log
    std::vector<std::string> genres;     // suggestions: "Custom vehicles", "Custom weapons", "Total conversions"
    std::vector<std::string> custom_vehicles, custom_weapons;   // short names, for the log
    std::string character;               // the multiplayer character, when it isn't Halo's
    std::string why_genres;              // a line for the log
};
// Reads the scenario, globals and item collections of a built map (any game). False when it can't.
bool analyze_map_traits(const Config &cfg, const fs::path &map, MapTraits &out, std::string *err = nullptr);
// Whether a map's scripts hand out the globals' cheat power-ups (the Xbox trim leaves them out otherwise):
// 1 they call cheat_all_powerups, 0 they don't (or there are no scripts), -1 can't tell (scripts without source).
int scripts_use_cheat_powerups(const std::string &script_text, int script_count);

// Reads the cache file header. Returns Unknown if not a Halo CE-family map.
Game detect_game(const fs::path &map, std::string *scenario_name = nullptr);

bool invader_ok(const Config &cfg);

// Convert a compiled map from one game to another.
// `measure` (Xbox): when the map doesn't fit (tag memory or file size) and no weapons & vehicles picks are applied,
// it's also measured for the weapons & vehicles window from the tags already prepared (map_name set = measured).
Result convert_map(const Config &cfg, const fs::path &input, Game in_game, Game out_game,
                   const fs::path &output, bool share_resources, const Log &log, ContentAnalysis *measure = nullptr);

// Build a scenario from a tags folder (e.g. H1A-EK\tags) for a target game. `measure`: as for convert_map.
Result build_from_tags(const Config &cfg, const fs::path &scenario_file, Game out_game,
                       const fs::path &output, bool share_resources, const Log &log, ContentAnalysis *measure = nullptr);

// Rewrites HSC script text for the target game (functions/globals it lacks get equivalents or stand-ins).
std::string convert_script_text(Game target, const std::string &text, std::vector<std::string> *notes);

// Runs a process, returns exit code, captures combined stdout/stderr.
int run_process(const std::vector<std::string> &args, std::string &output);

std::string u8(const fs::path &p);
fs::path from_u8(const std::string &s);

} // namespace porter
