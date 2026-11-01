// Halo Map Porter - checks a Custom Edition map against its Xbox port, tag by tag, under Perfect Conversion's rule:
// gameplay data the same on both sides, visual and audio data free to differ. Works on the two maps' extracted tags
// (invader-extract), matched by path and group (gbxmodel and model are one group, and so are all the shader classes).
#pragma once
#include <map>
#include <string>
#include <vector>

namespace pairs {

struct Diff {
    std::string check;   // "gameplay-tags", "models", "shaders", "collision", "placements", "scripts", "globals", "missing"
    std::string tag;     // path.group (the CE side's)
    std::string field;   // "triggers[0].flags", or "" for the whole tag
    std::string ce, xbox;   // the two values
};

struct Result {
    std::vector<Diff> diffs;            // everything that breaks the rule
    std::vector<Diff> notes;            // differences the rule allows but worth knowing (player control, extra tags)
    std::map<std::string, int> checked; // check -> how many tags it compared
    std::map<std::string, int> failed;  // check -> how many tags differ
    int ce_tags = 0, xbox_tags = 0, matched = 0;
    bool scripts_differ = false;        // (only the scripts: an optimized Xbox side, "check scripts optimized")
    bool ok() const { return diffs.empty(); }
    // Linkable: nothing differs but the scripts (an Xbox side whose scripts were converted: "check scripts optimized").
    bool linkable() const {
        for (auto &d : diffs)
            if (d.check != "scripts") return false;
        return true;
    }
};

// A tag group (its file extension) whose tags the network names or gameplay rests on: Perfect Conversion takes them
// from the Custom Edition map, never from the Xbox's tagset.
bool gameplay_group(const std::string &ext);

// `ce_dir` and `xbox_dir`: the folders the two maps were extracted to.
Result check(const std::string &ce_dir, const std::string &xbox_dir);

// A readable report: a summary per check, then each difference (at most `per_tag` lines for one tag).
std::string report(const Result &r, size_t per_tag = 12);

}   // namespace pairs
