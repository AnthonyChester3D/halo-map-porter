// Halo Map Porter - campaign levels: finding a level's opening and closing cutscenes in its scripts, and splitting
// them off into maps of their own (each loads the next with map_name, as a campaign's levels do), so each piece
// needs only what it uses of the Xbox's 22 MB of tag memory.
//
// The opening cutscene: the part of a startup script a player skips with the cinematic skip, the
// (if (cinematic_skip_start) ...) Bungie's levels start with (a10's, Lumoria's). The level without it is the level as
// a skip leaves it. The closing cutscene: a script's last stretch, from after its last sleep_until before its
// cinematic_start through the map_name (or game_won) it ends the level with (Lumoria's end_poodles).
//
// A piece (the opening or the closing) keeps the scripts and globals that stretch uses, its BSPs (renumbered: the
// first one, which a map starts in, and those it switches to), the objects its scripts name and the scenery that
// stands in its BSPs, the encounters and conversations it names and the actors they need. The level drops the
// objects, encounters, conversations and BSPs only the pieces used.
#pragma once
#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace campaign {
namespace fs = std::filesystem;

// A script's text as Halo Script reads it: a list (with its parts) or an atom (a word, a number, a "string"), and
// where it is in the text ([b, e)).
struct Node {
    bool list = false;
    std::string atom;   // (lowercase, quotes taken off a string)
    size_t b = 0, e = 0;
    std::vector<Node> kids;
};
std::vector<Node> parse(const std::string &text);
// The atoms of a text (lowercase), every one.
std::set<std::string> atoms(const std::string &text);

struct File { fs::path path; std::string text; std::vector<Node> forms; };
// A script or global: kind "script" (type startup, dormant, continuous, static, stub) or "global".
struct Def { std::string kind, type, name; size_t file = 0, form = 0; };
// A cutscene found: in a script (`script`, its statements [first, last] of the form `form` in file `file`, or for an
// opening, one statement: the skippable if), what it plays (`body`), what goes before it in a piece (`preamble`), the
// BSPs it switches to, and a line for people.
struct Cut {
    bool found = false;
    size_t file = 0, form = 0;
    size_t b = 0, e = 0;          // what's taken out of the level's script
    std::string script, body, preamble, about;
    std::set<int> bsps;
};
struct Analysis {
    std::vector<File> files;
    std::vector<Def> defs;
    Cut opening, closing;
    std::string report;           // for people: the scripts, the cutscenes found and what they switch to
};
// The scripts of a data folder (every .hsc under it, global_scripts.hsc included), and the cutscenes in them.
bool load(const fs::path &data, Analysis &a, std::string &why);
void analyze(Analysis &a);

// The level's scripts with the pieces taken out (`opening`: as a skip leaves it; `closing`: its last stretch becomes a
// map_name of `closing_next`), and the scripts nothing runs any more left out: each file's new text. `removed`: the atoms
// only what went used; `kept`: the atoms of what's left.
std::vector<std::pair<fs::path, std::string>> level_texts(const Analysis &a, bool opening, bool closing,
                                                          const std::string &closing_next, std::set<std::string> &removed,
                                                          std::set<std::string> &kept);
// A piece's script (one file): the globals and scripts its stretch uses, and a startup script playing it (the opening
// still skippable, then map_name `next`; the closing ends as the level did).
// `start_bsp` > 0: the piece switches there first (a closing cutscene plays where the level was).
std::string piece_text(const Analysis &a, bool opening, const std::string &next, int start_bsp = 0);
// The scripts with every script and global that names one of `gone` (an encounter, "enc/squad" too, or a
// conversation) left out, and those naming what was left out, until none is: each file's new text. `dropped`: how many.
std::vector<std::pair<fs::path, std::string>> texts_without(const Analysis &a, const std::set<std::string> &gone, int &dropped);
// The names object_..._containing calls are given (the objects with any of them in their names are theirs).
std::vector<std::string> containing_patterns(const std::string &text);
// The cutscene camera points a script sets, in order.
std::vector<std::string> camera_points_set(const std::string &text);

// The BSPs a script switches to or tests (switch_bsp N, structure_bsp_index compared with N).
std::set<int> bsps_used(const std::string &text);
// Those numbers changed (`to`: old index -> new).
std::string renumber_bsps(const std::string &text, const std::map<int, int> &to);

// The scenario cut down (`scenario`, saved in place). `keep_bsps`: the BSPs left, in order (renumbered from 0).
// `piece`: everything not named in `names` goes, but scenery standing in a kept BSP; else (the level) only what's named
// in `drop` and not in `names`. `patterns`: object_..._containing's (names with any of them in are kept).
struct Strip {
    std::vector<int> keep_bsps;
    // Where placements stand (their "bsp indices" aren't filled in until a map is built): for each point, bit 0 inside
    // a kept BSP, bit 1 inside one that goes. Not given: placements are kept wherever they are.
    std::function<std::vector<int>(const std::vector<std::array<double, 3>> &)> inside;
    std::set<std::string> names, drop;
    std::vector<std::string> patterns;
    bool piece = false;
    // (the level only) Encounters by where they are too: one whose starting locations and firing positions are all in
    // BSPs that go, goes (and the conversations whose participants are its).
    bool by_place = false;
};
struct Stripped {
    int placements = 0, palette = 0, encounters = 0, conversations = 0, actors = 0, bsps = 0;
    std::set<std::string> gone_ai;   // (by_place: the encounters and conversations that went, lowercase)
    std::string why;
};
bool strip_scenario(const fs::path &scenario, const Strip &s, Stripped &out);

}   // namespace campaign
