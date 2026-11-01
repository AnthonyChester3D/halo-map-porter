// Halo Map Porter - Custom Edition map vs its Xbox port, under Perfect Conversion's rule (see pairs.hpp).
#include "pairs.hpp"
#include "tagedit.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <set>

namespace pairs {
namespace {
namespace fs = std::filesystem;

using Flat = std::vector<std::pair<std::string, std::string>>;

bool starts(const std::string &s, const char *p) { return s.rfind(p, 0) == 0; }
bool ends(const std::string &s, const std::string &e) {
    return s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0;
}

// The group a file's extension names, with gbxmodel as model and every shader class as shader: the same tag to the
// network, whichever form it takes on each side.
std::string group_key(std::string ext) {
    if (ext == "gbxmodel") return "model";
    if (starts(ext, "shader")) return "shader";
    return ext;
}

// A reference's value ("@path.ext") as the network sees it: the path, with the group as group_key has it.
std::string dep_key(const std::string &v) {
    size_t dot = v.rfind('.');
    if (v.size() < 2 || dot == std::string::npos || v.find('/', dot) != std::string::npos) return v;
    return v.substr(0, dot + 1) + group_key(v.substr(dot + 1));
}
std::string dep_ext(const std::string &v) {
    size_t dot = v.rfind('.');
    return dot == std::string::npos ? "" : v.substr(dot + 1);
}

// Groups that only look or sound: a reference to one of these may change or go.
bool visual_group(const std::string &ext) {
    static const std::set<std::string> v = {
        "bitmap", "sound", "sound_looping", "contrail", "light", "light_volume", "particle", "particle_system", "decal",
        "lens_flare", "weapon_hud_interface", "unit_hud_interface", "grenade_hud_interface", "hud_globals",
        "hud_message_text", "hud_number", "font", "unicode_string_list", "string_list", "ui_widget_definition",
        "ui_widget_collection", "sky", "wind", "weather_particle_system", "fog", "glow", "color_table", "meter",
        "material_effects", "sound_environment", "dialogue", "detail_object_collection", "antenna", "flag",
        "input_device_defaults", "virtual_keyboard", "multiplayer_scenario_description", "sound_scenery_effect"};
    return v.count(ext) || starts(ext, "shader");
}
bool object_group(const std::string &g) {
    static const std::set<std::string> o = {"biped", "vehicle", "weapon", "equipment", "garbage", "projectile",
                                            "scenery", "device_machine", "device_control", "device_light_fixture",
                                            "placeholder", "sound_scenery"};
    return o.count(g) > 0;
}
// Compared whole (apart from references to visual or audio tags).
bool whole_group(const std::string &g) {
    static const std::set<std::string> w = {"model_collision_geometry", "model_animations", "physics", "point_physics",
                                            "camera_track", "damage_effect", "continuous_damage_effect",
                                            "item_collection", "actor", "actor_variant"};
    return w.count(g) > 0;
}
// Tags the network names or gameplay rests on: they must be in both maps.
bool needed_group(const std::string &g) {
    return object_group(g) || whole_group(g) || g == "model" || g == "effect" || g == "scenario" ||
           g == "scenario_structure_bsp" || g == "globals";
}

// "a[3].b[12].c" -> "a[].b[].c"
std::string generic(const std::string &k) {
    std::string g;
    bool in = false;
    for (char c : k) {
        if (c == '[') { in = true; g += "[]"; continue; }
        if (c == ']') { in = false; continue; }
        if (!in) g += c;
    }
    return g;
}
bool under(const std::string &g, const char *p) {
    size_t n = strlen(p);
    return g.compare(0, n, p) == 0 && (g.size() == n || g[n] == '.' || g[n] == '[');
}

// The element of block `block` (generic, e.g. "events[].parts") that key `k` is in: the concrete block path
// ("events[2].parts") and the index, or false when k isn't in it.
bool element_of(const std::string &k, const std::string &block, std::string &where, size_t &index, std::string &rest) {
    std::string g;
    for (size_t i = 0; i < k.size(); i++) {
        if (k[i] == '[') {
            size_t e = k.find(']', i);
            if (e == std::string::npos) return false;
            if (g == block) {
                where = k.substr(0, i);
                index = std::stoul(k.substr(i + 1, e - i - 1));
                rest = k.substr(e + 1);
                return true;
            }
            g += "[]";
            i = e;
            continue;
        }
        g += k[i];
    }
    return false;
}

// Drops the elements of `block` whose reference `field` is to a visual or audio tag, and numbers the rest again
// (an object's attachments, an effect's parts: their lights and particles may go, the rest stays in order).
void keep_gameplay_elements(Flat &f, const std::string &block, const std::string &field) {
    std::map<std::string, std::set<size_t>> drop;
    std::string where, rest;
    size_t i;
    for (auto &[k, v] : f)
        if (element_of(k, block, where, i, rest) && rest == "." + field && v.size() > 1 && visual_group(dep_ext(v)))
            drop[where].insert(i);
    if (drop.empty()) return;
    Flat out;
    for (auto &[k, v] : f) {
        if (ends(k, ".size") && drop.count(k.substr(0, k.size() - 5))) {
            out.push_back({k, std::to_string(std::stoul(v) - drop[k.substr(0, k.size() - 5)].size())});
            continue;
        }
        if (element_of(k, block, where, i, rest) && drop.count(where)) {
            auto &d = drop[where];
            if (d.count(i)) continue;
            size_t below = std::distance(d.begin(), d.lower_bound(i));
            out.push_back({where + "[" + std::to_string(i - below) + "]" + rest, v});
            continue;
        }
        out.push_back({k, v});
    }
    f.swap(out);
}

std::string strip_word(std::string v, const std::string &w) {
    size_t p = v.find(w);
    if (p == std::string::npos) return v;
    v.erase(p, w.size());
    while (!v.empty() && v.back() == ' ') v.pop_back();
    while (!v.empty() && v[0] == ' ') v.erase(0, 1);
    size_t dd = v.find("  ");
    if (dd != std::string::npos) v.erase(dd, 1);
    return v;
}

// Which check a field belongs to in a tag of group `g`, or "" when it's free to differ. `note`: a difference only
// reported (allowed).
std::string check_for(const std::string &g, const std::string &k, bool &note) {
    note = false;
    // MCC's metagame scoring (what a kill of it scores): neither Custom Edition nor the Xbox reads it, and Invader
    // versions fill it in differently.
    std::string last = k.substr(k.rfind('.') == std::string::npos ? 0 : k.rfind('.') + 1);
    if (last.rfind("metagame_", 0) == 0) return "";
    if (object_group(g)) {
        if (under(k, "predicted_resources") || under(k, "hud_text_message_index") || under(k, "widgets")) return "";
        return "gameplay-tags";
    }
    if (whole_group(g)) {
        if (g == "model_animations" && under(k, "sound_references")) return "";
        return "gameplay-tags";
    }
    if (g == "model") {
        if (under(k, "nodes") || under(k, "node_list_checksum") || k == "regions.size" || k == "regions[].name" ||
            k == "regions[].permutations.size" || k == "regions[].permutations[].name" ||
            under(k, "regions[].permutations[].markers"))
            return "models";
        return "";
    }
    if (g == "shader") return k == "material_type" ? "shaders" : "";
    if (g == "effect") {
        if (k == "events.size" || under(k, "events[].delay_bounds") || under(k, "events[].duration_bounds") ||
            under(k, "events[].skip_fraction") || under(k, "events[].parts") || under(k, "locations") ||
            k == "flags" || k == "loop_start_event" || k == "loop_stop_event")
            return "gameplay-tags";
        return "";
    }
    if (g == "scenario_structure_bsp") {
        static const char *keep[] = {"collision_bsp", "collision_materials", "breakable_surfaces", "cluster_portals",
                                     "leaves", "leaf_surfaces", "nodes", "pathfinding_surfaces", "pathfinding_edges",
                                     "markers", "world_bounds_x", "world_bounds_y", "world_bounds_z",
                                     "vehicle_floor", "vehicle_ceiling"};
        for (auto p : keep)
            if (under(k, p)) return "collision";
        if (k == "clusters.size" || under(k, "clusters[].portals")) return "collision";
        return "";
    }
    if (g == "scenario") {
        static const char *free_[] = {"comments", "decal_palette", "decals", "detail_object_collection_palette",
                                      "skies", "source_files", "editor_scenario_data", "references", "hud_messages",
                                      "ingame_help_text", "cutscene_titles"};
        for (auto p : free_)
            if (under(k, p)) return "";
        if (under(k, "scripts") || under(k, "globals") || under(k, "script_syntax_data") ||
            under(k, "script_string_data"))
            return "scripts";
        return "placements";
    }
    if (g == "globals") {
        if (under(k, "player_control")) { note = true; return "globals"; }
        static const char *keep[] = {"player_information", "falling_damage", "materials", "grenades", "difficulty",
                                     "cheat_powerups", "weapon_list", "multiplayer_information[].flag",
                                     "multiplayer_information[].ball", "multiplayer_information[].unit",
                                     "multiplayer_information[].vehicles"};
        for (auto p : keep)
            if (under(k, p)) return "globals";
        return "";
    }
    return "";
}

struct TagFile { std::string file, ext; };

std::map<std::string, TagFile> scan(const std::string &root, int &count) {
    std::map<std::string, TagFile> m;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(fs::u8path(root), ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        fs::path rel = fs::relative(it->path(), fs::u8path(root));
        std::string ext = rel.extension().u8string();
        if (ext.size() < 2) continue;
        ext = ext.substr(1);
        std::string stem = (rel.parent_path() / rel.stem()).generic_u8string();
        m[stem + "." + group_key(ext)] = {it->path().u8string(), ext};
        count++;
    }
    return m;
}

void compare(const std::string &tag, const std::string &g, Flat a, Flat b, Result &r, std::set<std::string> &hit) {
    if (object_group(g)) {
        keep_gameplay_elements(a, "attachments", "type");
        keep_gameplay_elements(b, "attachments", "type");
    }
    if (g == "effect") {
        keep_gameplay_elements(a, "events[].parts", "type");
        keep_gameplay_elements(b, "events[].parts", "type");
    }
    std::map<std::string, std::string> A(a.begin(), a.end()), B(b.begin(), b.end());
    std::vector<std::string> keys;
    for (auto &kv : a) keys.push_back(kv.first);
    for (auto &kv : b)
        if (!A.count(kv.first)) keys.push_back(kv.first);
    std::set<std::string> counted;
    for (auto &k : keys) {
        bool note;
        std::string gk = generic(k), c = check_for(g, gk, note);
        if (c.empty()) continue;
        if (!counted.count(c)) { counted.insert(c); r.checked[c]++; }
        auto ia = A.find(k), ib = B.find(k);
        std::string va = ia == A.end() ? "(none)" : ia->second, vb = ib == B.end() ? "(none)" : ib->second;
        bool dep = (!va.empty() && va[0] == '@') || (!vb.empty() && vb[0] == '@');
        if (dep) {
            bool keep_shader = g == "scenario_structure_bsp" && under(gk, "collision_materials");
            if (!keep_shader && ((va.size() > 1 && visual_group(dep_ext(va))) || (vb.size() > 1 && visual_group(dep_ext(vb)))))
                continue;
            if (dep_key(va) == dep_key(vb)) continue;
        } else if (va != vb) {
            // Invader sets this one itself when it builds for Custom Edition (projectiles the client works out itself)
            if (g == "weapon" && gk == "triggers[].flags" &&
                strip_word(va, "projectile_is_client_side_only") == strip_word(vb, "projectile_is_client_side_only"))
                continue;
        } else {
            continue;
        }
        auto clean = [](const std::string &v) { return !v.empty() && v[0] == '@' ? v.substr(1) : v; };
        Diff d{c, tag, k, clean(va), clean(vb)};
        // A value that's none of the field's choices (an enum out of range, as some of Gearbox's stock tags have):
        // Invader can't build it as it is (the first choice instead). Reported, not counted.
        if (va == "?") {
            d.ce = "(not one of its choices)";
            note = true;
        }
        if (note) {
            r.notes.push_back(d);
        } else {
            r.diffs.push_back(d);
            if (!hit.count(c + "|" + tag)) { hit.insert(c + "|" + tag); r.failed[c]++; }
        }
    }
}

}   // namespace

Result check(const std::string &ce_dir, const std::string &xbox_dir) {
    Result r;
    auto A = scan(ce_dir, r.ce_tags), B = scan(xbox_dir, r.xbox_tags);
    if (A.empty() || B.empty()) {   // (nothing to compare is no match)
        r.diffs.push_back({"unreadable", A.empty() ? ce_dir : xbox_dir, "", "(no tags there)", ""});
        return r;
    }
    std::set<std::string> hit;
    for (auto &[key, fa] : A) {
        std::string g = key.substr(key.rfind('.') + 1);
        auto it = B.find(key);
        if (it == B.end()) {
            if (needed_group(g)) {
                r.diffs.push_back({"missing", key, "", "(in the map)", "(not in the map)"});
                r.failed["missing"]++;
            }
            continue;
        }
        r.matched++;
        if (!needed_group(g) && g != "shader") continue;
        Flat a, b;
        if (!tagedit::flatten(fa.file, a) || !tagedit::flatten(it->second.file, b)) {
            r.diffs.push_back({"unreadable", key, "", "", ""});
            continue;
        }
        compare(key, g, std::move(a), std::move(b), r, hit);
    }
    for (auto &[key, fb] : B) {
        std::string g = key.substr(key.rfind('.') + 1);
        if (!A.count(key) && needed_group(g)) r.notes.push_back({"extra", key, "", "(not in the map)", "(in the map)"});
    }
    for (auto &d : r.diffs)
        if (d.check == "scripts") r.scripts_differ = true;
    return r;
}

bool gameplay_group(const std::string &ext) { return needed_group(group_key(ext)); }
bool look_or_sound_group(const std::string &ext) { return visual_group(ext); }

std::string report(const Result &r, size_t per_tag) {
    std::string s;
    s += "Tags: " + std::to_string(r.ce_tags) + " in the Custom Edition map, " + std::to_string(r.xbox_tags) +
         " in the Xbox map, " + std::to_string(r.matched) + " in both\n";
    std::set<std::string> names;
    for (auto &kv : r.checked) names.insert(kv.first);
    for (auto &kv : r.failed) names.insert(kv.first);
    for (auto &c : names) {
        auto ic = r.checked.find(c), ifl = r.failed.find(c);
        int n = ic == r.checked.end() ? 0 : ic->second, f = ifl == r.failed.end() ? 0 : ifl->second;
        s += "  " + c + ": " + (c == "missing" ? std::to_string(f) + " gameplay tags missing from the Xbox map"
                                               : std::to_string(n) + " compared, " + std::to_string(f) + " differ") +
             "\n";
    }
    s += r.ok() ? "Result: the pair matches\n" : "Result: " + std::to_string(r.diffs.size()) + " differences\n";
    auto list = [&](std::vector<Diff> v, const char *title) {
        if (v.empty()) return;
        std::stable_sort(v.begin(), v.end(), [](const Diff &a, const Diff &b) {
            bool ma = a.field.empty(), mb = b.field.empty();   // (whole tags, missing or extra, last)
            if (ma != mb) return mb;
            return a.tag != b.tag ? a.tag < b.tag : a.check < b.check;
        });
        s += std::string("\n") + title + "\n";
        std::string last;
        size_t n = 0;
        for (auto &d : v) {
            if (d.field.empty()) {   // (a whole tag: one line each, under one heading)
                std::string head = d.check == "missing" ? "missing from the Xbox map:" :
                                   d.check == "extra" ? "only in the Xbox map:" : d.check + ":";
                if (head != last) { s += head + "\n"; last = head; }
                s += "    " + d.tag + "\n";
                continue;
            }
            std::string head = d.check + "  " + d.tag;
            if (head != last) { s += head + "\n"; last = head; n = 0; }
            if (++n > per_tag) {
                if (n == per_tag + 1) s += "    ...\n";
                continue;
            }
            s += "    " + d.field + ": " + d.ce + "  |  " + d.xbox + "\n";
        }
    };
    list(r.diffs, "Differences (Custom Edition | Xbox):");
    list(r.notes, "Allowed, but worth knowing:");
    return s;
}

}   // namespace pairs
