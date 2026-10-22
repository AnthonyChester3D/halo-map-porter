// Halo Map Porter - conversion engine
#include "core.hpp"
#include "inflate.hpp"
#include "lod.hpp"
#include "perf.hpp"
#include "levelshaders.hpp"
#include "portals.hpp"
#include "preview.hpp"
#include "tagfile.hpp"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <chrono>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"
#define STB_DXT_IMPLEMENTATION
#include "stb_dxt.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace porter {

const char *game_name(Game g) {
    switch (g) {
    case Game::MCC: return "MCC";
    case Game::CE: return "Custom Edition";
    case Game::Xbox: return "Xbox";
    default: return "Unknown";
    }
}

const char *game_engine(Game g) {
    switch (g) {
    case Game::MCC: return "mcc-cea";
    case Game::CE: return "gbx-custom";
    case Game::Xbox: return "xbox-ntsc";
    default: return "";
    }
}

std::string u8(const fs::path &p) { return p.u8string(); }
fs::path from_u8(const std::string &s) { return fs::u8path(s); }

// ---------------------------------------------------------------- processes

#ifdef _WIN32
static std::wstring widen(const std::string &s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

static std::wstring quote_arg(const std::wstring &a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring r = L"\"";
    for (size_t i = 0;; i++) {
        size_t bs = 0;
        while (i < a.size() && a[i] == L'\\') { i++; bs++; }
        if (i == a.size()) { r.append(bs * 2, L'\\'); break; }
        if (a[i] == L'"') { r.append(bs * 2 + 1, L'\\'); r.push_back(L'"'); }
        else { r.append(bs, L'\\'); r.push_back(a[i]); }
    }
    r.push_back(L'"');
    return r;
}

int run_process(const std::vector<std::string> &args, std::string &output) {
    output.clear();
    std::wstring cmd;
    for (auto &a : args) { if (!cmd.empty()) cmd += L' '; cmd += quote_arg(widen(a)); }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                             nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); output = "Could not start " + args[0]; return -1; }
    char chunk[4096];
    DWORD n;
    while (ReadFile(rd, chunk, sizeof(chunk), &n, nullptr) && n > 0) output.append(chunk, n);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    output.erase(std::remove(output.begin(), output.end(), '\r'), output.end());
    return (int)code;
}
#else
int run_process(const std::vector<std::string> &args, std::string &output) {
    output.clear();
    std::string cmd;
    for (auto &a : args) {
        std::string q = "'";
        for (char c : a) { if (c == '\'') q += "'\\''"; else q += c; }
        cmd += q + "' ";
    }
    cmd += "2>&1";
    static const char *prof = getenv("HMP_PROF");   // tests: time every tool run
    auto t0 = std::chrono::steady_clock::now();
    FILE *p = popen(cmd.c_str(), "r");
    if (!p) return -1;
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), p)) > 0) output.append(chunk, n);
    int st = pclose(p);
    if (prof) {
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::string what = fs::path(args[0]).filename().string();
        for (size_t i = 1; i < args.size() && i < 8; i++) what += " " + args[i];
        if (FILE *f = fopen(prof, "a")) { fprintf(f, "%.1f\t%s\n", ms, what.c_str()); fclose(f); }
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
#endif

static fs::path tool(const Config &cfg, const char *name) {
#ifdef _WIN32
    return cfg.invader_dir / (std::string(name) + ".exe");
#else
    return cfg.invader_dir / name;
#endif
}

bool invader_ok(const Config &cfg) {
    for (auto n : {"invader-build", "invader-extract", "invader-edit", "invader-info", "invader-convert",
                   "invader-refactor", "invader-sound", "invader-recover", "invader-dependency"})
        if (!fs::exists(tool(cfg, n))) return false;
    return true;
}

// ---------------------------------------------------------------- headers

Game detect_game(const fs::path &map, std::string *scenario_name) {
    std::ifstream f(map, std::ios::binary);
    char h[0x800] = {};
    if (!f.read(h, sizeof(h))) return Game::Unknown;
    if (std::memcmp(h, "daeh", 4) != 0) return Game::Unknown;
    uint32_t engine;
    std::memcpy(&engine, h + 4, 4);
    if (scenario_name) *scenario_name = std::string(h + 0x20, strnlen(h + 0x20, 32));
    switch (engine) {
    case 5: return Game::Xbox;
    case 7:
    case 609: return Game::CE;
    case 13: return Game::MCC;
    default: return Game::Unknown;
    }
}

// ---------------------------------------------------------------- log parsing

static std::vector<std::string> unwrap(const std::string &text) {
    static const std::regex starts(
        R"(^(FATAL ERROR|ERROR|WARNING|Script compilation error|\.\.\.in |Failed to |Reading |Extracted |Built |tag data|Error))");
    std::vector<std::string> out;
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        if (!out.empty() && !line.empty() && !std::regex_search(line, starts)) {
            size_t a = line.find_first_not_of(' ');
            out.back() += " " + (a == std::string::npos ? "" : line.substr(a));
        } else {
            out.push_back(line);
        }
    }
    return out;
}

struct Failure {
    std::string msg, tag;
    std::vector<std::string> path; // e.g. events[1], particles[3]
    std::string script_error;
    // Invader's own check of a script's names (nav points, objects, tags...), printed as
    // "extracted.hsc:1492:43: error: can't find navpoint 'orange_landingpad'" before a FATAL ERROR
    // "Failed to compile scripts: tag data is invalid" for the scenario.
    std::string hsc_error;
};

static bool parse_failure(const std::string &log_text, Failure &f) {
    auto lines = unwrap(log_text);
    static const std::regex fail_re(R"(^Failed to compile (\w+)::(\w+) #(\d+))");
    static const std::regex hsc_re(R"(([^\s:]+\.hsc:\d+:\d+: error: .*))");
    for (size_t i = 0; i < lines.size(); i++) {
        auto &l = lines[i];
        std::smatch hm;
        if (f.hsc_error.empty() && std::regex_search(l, hm, hsc_re)) {
            f.hsc_error = hm[1].str();
            while (!f.hsc_error.empty() && (f.hsc_error.back() == '\r' || f.hsc_error.back() == ' ')) f.hsc_error.pop_back();
        }
        if (l.rfind("Script compilation error:", 0) == 0) {
            f.script_error = l.substr(25);
            size_t a = f.script_error.find_first_not_of(' ');
            if (a != std::string::npos) f.script_error = f.script_error.substr(a);
        }
        if (l.rfind("FATAL ERROR:", 0) == 0 || l.rfind("ERROR:", 0) == 0) {
            f.msg = l.substr(l.find(':') + 1);
            size_t a = f.msg.find_first_not_of(' ');
            if (a != std::string::npos) f.msg = f.msg.substr(a);
            size_t j = i + 1;
            while (j < lines.size() && lines[j].rfind("...in ", 0) == 0) {
                if (f.tag.empty()) f.tag = lines[j].substr(6);
                j++;
            }
            while (j < lines.size() && lines[j].rfind("Failed to compile", 0) == 0) {
                std::smatch m;
                if (std::regex_search(lines[j], m, fail_re))
                    f.path.insert(f.path.begin(), m[2].str() + "[" + m[3].str() + "]");
                else if (lines[j].rfind("Failed to compile tag", 0) == 0)
                    break;
                j++;
            }
            while (!f.tag.empty() && (f.tag.back() == ' ' || f.tag.back() == '\r')) f.tag.pop_back();
            std::replace(f.tag.begin(), f.tag.end(), '\\', '/');
            return true;
        }
    }
    return !f.script_error.empty();
}

// ---------------------------------------------------------------- job

namespace {

int convert_bc7_bitmap_tag(const fs::path &p, bool write);
std::string norm_rel(std::string s);
std::vector<std::pair<std::string, std::string>> list_values(const Config &cfg, const fs::path &tags, const std::string &tag);

struct Job {
    const Config &cfg;
    const Log &log;
    Game target;
    fs::path work, fixes;
    fs::path overlay;  // stock tags of target game (optional)
    fs::path source;   // tags of the map being converted / user's tags
    fs::path user_tags; // build-from-tags: the user's own tags folder (never modified)
    fs::path user_data; // build-from-tags: the user's data folder (scripts, sounds)
    std::string scenario;
    std::vector<std::string> extra; // extra invader-build args (scripts)
    std::vector<std::string> changes;
    std::set<std::string> swapped, borrowed;
    std::string xbox_slots[3]; // the vehicle each Xbox slot ends up with (restrict_to_xbox_mp)
    std::vector<std::string> kept_vehicles; // vehicles with no slot kept for the Halo Universal XBE
    std::map<std::string, int> fix_count;
    std::set<std::string> range_checked; // tag kinds (".sound"...) already scanned for out-of-range values
    Game from = Game::Unknown;            // the game the tags came from (convert), if known
    std::set<std::string> keep_source;    // stock paths whose map version wins anyway (scenario, BSPs, lightmaps)
    ContentAnalysis *analysis = nullptr;  // set: measure the map for the weapons & vehicles window instead of converting
    fs::path choices_scenario;            // analysis: the scenario copy the stock choices were added to (backup alongside)
    std::string resources_from;           // convert: where the map's bitmaps.map/sounds.map were read from (for messages)
    ContentAnalysis *measure = nullptr;   // convert: measure here too when the map doesn't fit (see convert_map)
    // Models the app made lower-detail versions for, as they were before (to take them out again when the map
    // doesn't fit the Xbox), and what those versions take in the Xbox map.
    std::vector<std::pair<fs::path, std::string>> lod_backups;
    std::vector<std::pair<fs::path, std::string>> portal_backups;   // levels the app gave portals, as they were
    size_t lod_xbox_bytes = 0;
    std::vector<LodView::Model> lod_changes;   // the models whose levels of detail changed (for the LODs window)
    fs::path shaders_file;                     // the level shaders file (levelshaders.hpp), for the window
    std::string shaders_line;                  // the report's line on the level's shader passes

    Job(const Config &c, const Log &l, Game t) : cfg(c), log(l), target(t) {}

    std::vector<fs::path> dirs() const {
        std::vector<fs::path> d{fixes};
        if (!overlay.empty()) d.push_back(overlay);
        d.push_back(source);
        return d;
    }

    fs::path owner(const std::string &tag) const {
        if (keep_source.count(tag) && !fs::exists(fixes / from_u8(tag)) && fs::exists(source / from_u8(tag)))
            return source;
        for (auto &d : dirs())
            if (fs::exists(d / from_u8(tag))) return d;
        return {};
    }

    bool quiet = false;   // fixes made only for a measurement aren't the convert's changes
    void note(const std::string &s) {
        if (quiet) return;
        changes.push_back(s);
        log("  " + s);
    }

    bool to_fixes(const std::string &tag, const fs::path &from) {
        std::error_code ec;
        fs::path dst = fixes / from_u8(tag);
        fs::create_directories(dst.parent_path(), ec);
        fs::copy_file(from / from_u8(tag), dst, fs::copy_options::overwrite_existing, ec);
        return !ec;
    }

    bool edit(const std::string &tag, const std::string &key, const std::string &value) {
        fs::path own = owner(tag);
        if (own.empty()) return false;
        if (own != fixes && !to_fixes(tag, own)) return false;
        std::string out;
        return run_process({u8(tool(cfg, "invader-edit")), "-t", u8(fixes), tag, "-S", key, value}, out) == 0;
    }

    bool swap_to_source(const std::string &tag, const std::string &why) {
        if (overlay.empty() || swapped.count(tag)) return false;
        if (!fs::exists(overlay / from_u8(tag))) return false;
        // Prefer the prepared source copy; fall back to the user's own tags folder.
        fs::path from = fs::exists(source / from_u8(tag)) ? source
                      : (!user_tags.empty() && fs::exists(user_tags / from_u8(tag))) ? user_tags : fs::path();
        if (from.empty() || !to_fixes(tag, from)) return false;
        swapped.insert(tag);
        note("[swap] " + tag + ": the stock copy is invalid (" + why +
             "); using your map's version instead");
        return true;
    }

    // A value outside the range Invader allows (MCC's own tools accept some of these, e.g. a sound's
    // gain modifiers above 1.0): clamp it to the nearest allowed value with
    // invader-bludgeon. Every tag of the same kind is checked the first time, so a map with many such
    // tags needs one rebuild instead of one per tag. Edited copies go to the fixes folder.
    bool clamp_out_of_range(const std::string &tag, const std::string &key, const std::string &msg) {
        std::string ext = from_u8(tag).extension().u8string();
        if (ext.empty()) return false;
        std::vector<std::string> todo{tag};
        if (!range_checked.count(ext)) {
            range_checked.insert(ext);
            static const std::regex detected(R"(^(.+): Detected out-of-range\s*$)");
            for (const fs::path &d : {fixes, source}) {
                if (d.empty() || !fs::exists(d)) continue;
                std::string out;
                run_process({u8(tool(cfg, "invader-bludgeon")), "-t", u8(d), "-b", "*" + ext}, out);
                std::istringstream ss(out);
                std::string line;
                std::smatch m;
                while (std::getline(ss, line)) {
                    if (!std::regex_search(line, m, detected)) continue;
                    std::string t = m[1].str();
                    std::replace(t.begin(), t.end(), '\\', '/');
                    if (from_u8(t).extension().u8string() != ext) t += ext;
                    // Only the copy the build actually uses (stock tags win over the map's own).
                    if (owner(t) == d && std::find(todo.begin(), todo.end(), t) == todo.end()) todo.push_back(t);
                }
            }
        }
        static const std::regex limit(R"(must (be )?(no more than|at least) (-?[\d.]+))");
        std::smatch lm;
        const std::string game = game_name(target);
        std::string why = "was outside the range " + game + " maps allow";
        // (Invader's message for a range's upper limit prints the lower one, so a "no more than 0" is ignored.)
        if (std::regex_search(msg, lm, limit) && !(lm[2] == "no more than" && std::atof(lm[3].str().c_str()) <= 0))
            why = std::string("was ") + (lm[2] == "no more than" ? "above " : "below ") + lm[3].str() + ", the " +
                  (lm[2] == "no more than" ? "highest" : "lowest") + " value " + game + " maps allow";
        int fixed = 0;
        for (auto &t : todo) {
            fs::path own = owner(t);
            if (own.empty() || (own != fixes && !to_fixes(t, own))) continue;
            std::string out;
            if (run_process({u8(tool(cfg, "invader-bludgeon")), "-t", u8(fixes), "-T", "out-of-range", t}, out) != 0) continue;
            fixed++;
            if (t == tag)
                note("[fix]  " + tag + ": " + key + " " + why + "; clamped it (and any other out-of-range values in the tag)");
            else
                note("[fix]  " + t + ": values outside the range " + game + " maps allow; clamped");
        }
        return fixed > 0;
    }

    // A particle type drawn 'rotational' also reads the sprite sequence after each state's one. MCC tolerates a
    // bitmap without it; the original engine reads past the end (Invader refuses). Those types are drawn 'simple'
    // instead. Every particle system the build uses is checked at once, so one rebuild covers them all.
    int fix_rotational_particles() {
        int fixed = 0;
        std::set<std::string> seen;
        std::map<std::string, int> sequences;   // bitmap -> sequence count
        auto edit_get = [&](const fs::path &d, const std::string &tag, const char *op, const std::string &key) {
            std::string v;
            run_process({u8(tool(cfg, "invader-edit")), "-t", u8(d), tag, op, key}, v);
            while (!v.empty() && (v.back() == '\n' || v.back() == ' ')) v.pop_back();
            return v;
        };
        static const std::regex state_re(R"(^particle_types\[(\d+)\]\.particle_states\[(\d+)\]\.(bitmaps|sequence_index)$)");
        for (const fs::path &d : {fixes, source}) {
            if (d.empty() || !fs::exists(d)) continue;
            std::error_code ec;
            for (auto it = fs::recursive_directory_iterator(d, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (it->path().extension() != ".particle_system") continue;
                std::string rel = fs::relative(it->path(), d).generic_u8string();
                if (!seen.insert(rel).second || owner(rel) != d) continue;   // only the copy the build uses
                int types = std::atoi(edit_get(d, rel, "-C", "particle_types").c_str());
                std::map<int, std::map<int, std::pair<std::string, int>>> states;   // type -> state -> (bitmap, sequence)
                bool listed = false;
                for (int i = 0; i < types; i++) {
                    if (edit_get(d, rel, "-G", "particle_types[" + std::to_string(i) + "].complex_sprite_render_mode") != "rotational") continue;
                    if (!listed) {
                        listed = true;
                        for (auto &[k, v] : list_values(cfg, d, rel)) {
                            std::smatch m;
                            if (!std::regex_match(k, m, state_re)) continue;
                            auto &st = states[std::stoi(m[1])][std::stoi(m[2])];
                            if (m[3] == "bitmaps") st.first = v; else st.second = std::atoi(v.c_str());
                        }
                    }
                    bool bad = false;
                    for (auto &[j, st] : states[i]) {
                        if (st.first.empty()) continue;
                        auto sc = sequences.find(st.first);
                        if (sc == sequences.end()) {
                            fs::path bo = owner(st.first);
                            sc = sequences.emplace(st.first, bo.empty() ? 0 : std::atoi(edit_get(bo, st.first, "-C", "bitmap_group_sequence").c_str())).first;
                        }
                        if (st.second + 1 >= sc->second) { bad = true; break; }
                    }
                    if (bad && edit(rel, "particle_types[" + std::to_string(i) + "].complex_sprite_render_mode", "simple")) {
                        fixed++;
                        note("[fix]  " + rel + ": particle type #" + std::to_string(i) + " is drawn 'rotational', which needs a second "
                             "sprite sequence its bitmap doesn't have; now drawn 'simple'");
                    }
                }
            }
        }
        return fixed;
    }

    // A transparent (chicago) shader with no maps: MCC draws nothing with it (mods use one as an "invisible"
    // shader); Invader needs at least one map. It gets one: a black texture added to the screen, which still
    // draws nothing. Every such shader the build uses is fixed at once.
    int fix_empty_chicago() {
        int fixed = 0;
        const std::string black = "ui/shell/bitmaps/black.bitmap";
        bool have_black = !owner(black).empty();
        std::set<std::string> seen;
        for (const fs::path &d : {fixes, source}) {
            if (d.empty() || !fs::exists(d)) continue;
            std::error_code ec;
            for (auto it = fs::recursive_directory_iterator(d, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
                std::string ext = it->path().extension().u8string();
                bool ext4 = ext == ".shader_transparent_chicago_extended";
                if (ext != ".shader_transparent_chicago" && !ext4) continue;
                std::string rel = fs::relative(it->path(), d).generic_u8string();
                if (!seen.insert(rel).second || owner(rel) != d) continue;   // only the copy the build uses
                std::string block = ext4 ? "maps_4_stage" : "maps", out;
                run_process({u8(tool(cfg, "invader-edit")), "-t", u8(d), rel, "-C", block}, out);
                if (std::atoi(out.c_str()) != 0 || out.find_first_of("0123456789") == std::string::npos) continue;
                if (d != fixes && !to_fixes(rel, d)) continue;
                std::vector<std::string> cmd{u8(tool(cfg, "invader-edit")), "-t", u8(fixes), rel, "-I", block, "1", "end",
                                             "-S", "framebuffer_blend_function", "add"};
                if (have_black) cmd.insert(cmd.end(), {"-S", block + "[0].map", black});
                if (run_process(cmd, out) == 0) {
                    fixed++;
                    note("[fix]  " + rel + ": a transparent shader with no maps (draws nothing); gave it one that still draws "
                         "nothing (black, added to the screen)");
                }
            }
        }
        return fixed;
    }

    // Animation frame indices past the animation's last frame (an MCC mod's animation shortened without
    // updating them): MCC never reaches such a key frame; Invader refuses. Key frames go to the frame count (still
    // never reached), the sound frame to 0. Every animation tag the build uses is checked at once.
    int fix_animation_frames() {
        int fixed = 0;
        std::set<std::string> seen;
        static const std::regex key_re(R"(^animations\[(\d+)\]\.(frame_count|key_frame_index|second_key_frame_index|sound_frame_index)$)");
        for (const fs::path &d : {fixes, source}) {
            if (d.empty() || !fs::exists(d)) continue;
            std::error_code ec;
            for (auto it = fs::recursive_directory_iterator(d, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (it->path().extension() != ".model_animations") continue;
                std::string rel = fs::relative(it->path(), d).generic_u8string();
                if (!seen.insert(rel).second || owner(rel) != d) continue;   // only the copy the build uses
                std::map<int, std::map<std::string, long>> anim;
                for (auto &[k, v] : list_values(cfg, d, rel)) {
                    std::smatch m;
                    if (std::regex_match(k, m, key_re) && !v.empty() && isdigit((unsigned char)v[0]))
                        anim[std::stoi(m[1])][m[2]] = std::atol(v.c_str());
                }
                std::vector<std::string> sets;
                int bad = 0;
                for (auto &[i, a] : anim) {
                    if (!a.count("frame_count") || a["frame_count"] <= 0) continue;
                    long fc = a["frame_count"];
                    std::string pre = "animations[" + std::to_string(i) + "].";
                    bool any = false;
                    for (const char *k : {"key_frame_index", "second_key_frame_index"})
                        if (a.count(k) && a[k] > fc) { sets.insert(sets.end(), {"-S", pre + k, std::to_string(fc)}); any = true; }
                    if (a.count("sound_frame_index") && a["sound_frame_index"] >= fc) { sets.insert(sets.end(), {"-S", pre + "sound_frame_index", "0"}); any = true; }
                    bad += any;
                }
                if (sets.empty()) continue;
                if (d != fixes && !to_fixes(rel, d)) continue;
                std::vector<std::string> cmd{u8(tool(cfg, "invader-edit")), "-t", u8(fixes), rel};
                cmd.insert(cmd.end(), sets.begin(), sets.end());
                std::string out;
                if (run_process(cmd, out) == 0) {
                    fixed++;
                    note("[fix]  " + rel + ": " + std::to_string(bad) + " animation" + (bad == 1 ? "" : "s") +
                         " had key/sound frames past their last frame (never reached); moved to the end");
                }
            }
        }
        return fixed;
    }

    // A shader map pointing at the wrong kind of bitmap (a flat texture where a cube map is expected...): MCC's tools
    // let it through; Invader refuses, and the Xbox would read the texture as the wrong kind. The reference is
    // cleared, so the engine uses its default texture of the right kind. Every shader the build uses is checked at once.
    int fix_bitmap_types() {
        enum { T2D = 0, T3D = 1, CUBE = 2 };
        static const std::map<std::string, std::vector<std::pair<std::string, int>>> fields = {
            {".shader_environment", {{"base_map", T2D}, {"primary_detail_map", T2D}, {"secondary_detail_map", T2D},
                                     {"micro_detail_map", T2D}, {"bump_map", T2D}, {"map", T2D}, {"reflection_cube_map", CUBE}}},
            {".shader_model", {{"base_map", T2D}, {"multipurpose_map", T2D}, {"detail_map", T2D}, {"reflection_cube_map", CUBE}}},
            {".shader_transparent_water", {{"base_map", T2D}, {"reflection_map", CUBE}, {"ripple_maps", T2D}}},
            {".shader_transparent_glass", {{"bump_map", T2D}, {"diffuse_map", T2D}, {"diffuse_detail_map", T2D},
                                           {"specular_detail_map", T2D}, {"specular_map", CUBE}, {"reflection_map", CUBE}}},
            {".shader_transparent_meter", {{"map", T2D}}},
            {".shader_transparent_plasma", {{"primary_noise_map", T3D}, {"secondary_noise_map", T3D}}},
        };
        static const std::map<std::string, std::vector<std::string>> map_blocks = {
            {".shader_transparent_generic", {"maps"}},
            {".shader_transparent_chicago", {"maps"}},
            {".shader_transparent_chicago_extended", {"maps_4_stage", "maps_2_stage"}},
        };
        static const char *kind[] = {"a flat (2D) texture", "a 3D texture", "a cube map", "a sprite sheet", "an interface bitmap"};
        auto get = [&](const fs::path &d, const std::string &tag, const std::vector<std::string> &ops) {
            std::vector<std::string> cmd{u8(tool(cfg, "invader-edit")), "-t", u8(d), tag};
            cmd.insert(cmd.end(), ops.begin(), ops.end());
            std::string out;
            std::vector<std::string> lines;
            if (run_process(cmd, out) != 0) return lines;
            std::istringstream ss(out);
            for (std::string l; std::getline(ss, l);) {
                while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
                std::replace(l.begin(), l.end(), '\\', '/');
                lines.push_back(l);
            }
            return lines;
        };
        std::map<std::string, int> types;   // bitmap -> type (-1 unknown)
        auto type_of = [&](const std::string &bitmap) {
            auto it = types.find(bitmap);
            if (it != types.end()) return it->second;
            int t = -1;
            fs::path own = owner(bitmap);
            if (!own.empty()) {
                std::ifstream f(own / from_u8(bitmap), std::ios::binary);
                unsigned char b[66];
                if (f.read(reinterpret_cast<char *>(b), sizeof b)) t = (b[64] << 8) | b[65];   // bitmap type, after the 64-byte tag header
                if (t > 4) t = -1;
            }
            return types[bitmap] = t;
        };
        int fixed = 0;
        std::set<std::string> seen;
        for (const fs::path &d : {fixes, source}) {
            if (d.empty() || !fs::exists(d)) continue;
            std::error_code ec;
            for (auto it = fs::recursive_directory_iterator(d, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
                std::string ext = it->path().extension().u8string();
                auto fl = fields.find(ext);
                auto mb = map_blocks.find(ext);
                if (fl == fields.end() && mb == map_blocks.end()) continue;
                std::string rel = fs::relative(it->path(), d).generic_u8string();
                if (!seen.insert(rel).second || owner(rel) != d) continue;   // only the copy the build uses
                std::vector<std::pair<std::string, int>> refs;   // key -> expected type
                if (fl != fields.end()) {
                    refs = fl->second;
                }
                else {
                    std::vector<std::string> ops{"-G", "first_map_type"};
                    for (auto &b : mb->second) ops.insert(ops.end(), {"-C", b});
                    auto v = get(d, rel, ops);
                    if (v.size() != ops.size() / 2) continue;
                    bool cube_first = v[0] != "2d_map";
                    for (size_t b = 0; b < mb->second.size(); b++) {
                        int n = std::atoi(v[b + 1].c_str());
                        for (int i = 0; i < n; i++)
                            refs.push_back({mb->second[b] + "[" + std::to_string(i) + "].map", (i == 0 && cube_first) ? CUBE : T2D});
                    }
                }
                if (refs.empty()) continue;
                std::vector<std::string> ops;
                for (auto &r : refs) ops.insert(ops.end(), {"-G", r.first});
                auto v = get(d, rel, ops);
                if (v.size() != refs.size()) continue;
                std::vector<std::string> sets, what;
                for (size_t i = 0; i < refs.size(); i++) {
                    const std::string &bm = v[i];
                    if (bm.size() <= 7 || bm.compare(bm.size() - 7, 7, ".bitmap") != 0) continue;   // none (".bitmap")
                    int t = type_of(bm);
                    if (t < 0 || t == refs[i].second) continue;
                    sets.insert(sets.end(), {"-S", refs[i].first, ""});
                    what.push_back(refs[i].first + " was " + kind[t] + " where " + kind[refs[i].second] + " belongs");
                }
                if (sets.empty()) continue;
                if (d != fixes && !to_fixes(rel, d)) continue;
                std::vector<std::string> cmd{u8(tool(cfg, "invader-edit")), "-t", u8(fixes), rel};
                cmd.insert(cmd.end(), sets.begin(), sets.end());
                std::string out;
                if (run_process(cmd, out) == 0) {
                    fixed++;
                    std::string list;
                    for (auto &w : what) list += (list.empty() ? "" : "; ") + w;
                    note("[fix]  " + rel + ": " + list + "; cleared (the game uses its default texture of the right kind)");
                }
            }
        }
        return fixed;
    }

    // Enum values the game never uses, which Custom Edition's tools let through and Invader refuses: netgame flags
    // of the unused kinds ("vegas bank", "ctf vehicle") are removed, and spawn points' unused gametype kinds
    // ("stub", "ignored") become "none" (no gametype spawns there either way). Invader's listing marks no value for
    // these, which is how they're found. The whole tag at once.
    int fix_unused_enum_values(const std::string &tag) {
        fs::path own = owner(tag);
        if (own.empty()) return 0;
        static const std::regex opt(
            R"(^(netgame_flags\[(\d+)\]\.type|(?:player_starting_locations|netgame_equipment)\[\d+\]\.type_[0-3])(\.\w+)?$)");
        std::map<std::string, bool> marked;   // "netgame_flags[3].type" -> has a value Invader knows
        std::map<std::string, int> flag_of;
        for (auto &[k, v] : list_values(cfg, own, tag)) {
            std::smatch m;
            if (!std::regex_match(k, m, opt)) continue;
            bool &b = marked[m[1]];
            if (m[3].matched && v == "*") b = true;
            if (m[2].matched) flag_of[m[1]] = std::stoi(m[2]);
        }
        std::vector<int> drop;
        std::vector<std::string> sets;
        for (auto &[k, ok] : marked) {
            if (ok) continue;
            if (flag_of.count(k)) drop.push_back(flag_of[k]);
            else sets.insert(sets.end(), {"-S", k, "none"});
        }
        if (drop.empty() && sets.empty()) return 0;
        if (own != fixes && !to_fixes(tag, own)) return 0;
        std::sort(drop.rbegin(), drop.rend());
        std::vector<std::string> cmd{u8(tool(cfg, "invader-edit")), "-t", u8(fixes), tag};
        for (int i : drop) cmd.insert(cmd.end(), {"-E", "netgame_flags[" + std::to_string(i) + "]"});
        cmd.insert(cmd.end(), sets.begin(), sets.end());
        std::string out;
        if (run_process(cmd, out) != 0) return 0;
        if (!drop.empty())
            note("[fix]  " + tag + ": removed " + std::to_string(drop.size()) + " netgame flag" + (drop.size() == 1 ? "" : "s") +
                 " of a kind the game never uses (Vegas bank / CTF vehicle)");
        if (!sets.empty())
            note("[fix]  " + tag + ": " + std::to_string(sets.size() / 3) + " spawn point or item gametype" + (sets.size() == 3 ? "" : "s") +
                 " of a kind the game never uses (stub / ignored) set to none");
        return (int)(drop.size() + sets.size() / 3);
    }

    // An object whose animations were made for a model at a stock path, stamped with a different node checksum than
    // the target game's copy of that model has (the MCC/PC model's tools leave its checksum at 0, which skips the
    // check; the Xbox's has one). When both have the same bones (names and parents, in the same order) the
    // animations work on it: they get the model's checksum. Otherwise the object gets the map's own version of the
    // model, as a copy beside it.
    bool fix_node_checksums(const std::string &tag, const std::string &model, const std::string &anim) {
        fs::path md = owner(model), ad = owner(anim);
        if (md.empty() || ad.empty()) return false;
        static const std::regex node_re(R"(^nodes\[(\d+)\]\.(name|parent_node_index)$)");
        static const std::regex sum_re(R"(^animations\[(\d+)\]\.node_list_checksum$)");
        struct Node { std::string name, parent; };
        auto bones = [&](const fs::path &d, const std::string &t, std::string *checksum, std::map<int, std::string> *sums) {
            std::vector<Node> v;
            for (auto &[k, val] : list_values(cfg, d, t)) {
                std::smatch m;
                if (std::regex_match(k, m, node_re)) {
                    size_t i = std::stoul(m[1]);
                    if (v.size() <= i) v.resize(i + 1);
                    if (m[2] == "name") {
                        std::string n = val;
                        for (auto &c : n) c = (char)tolower((unsigned char)c);
                        v[i].name = n;
                    } else v[i].parent = val;
                } else if (checksum && k == "node_list_checksum") *checksum = val;
                else if (sums && std::regex_match(k, m, sum_re)) (*sums)[std::stoi(m[1])] = val;
            }
            if (!v.empty() && (v[0].parent == "0" || v[0].parent == "65535")) v[0].parent = "root";
            return v;
        };
        std::string msum;
        std::map<int, std::string> sums;
        auto mb = bones(md, model, &msum, nullptr), ab = bones(ad, anim, nullptr, &sums);
        bool same = !mb.empty() && mb.size() == ab.size();
        for (size_t i = 0; same && i < mb.size(); i++) same = mb[i].name == ab[i].name && mb[i].parent == ab[i].parent;
        if (same && !msum.empty() && msum != "0") {
            std::vector<std::string> sets;
            for (auto &[i, s] : sums)
                if (s != "0" && s != msum) sets.insert(sets.end(), {"-S", "animations[" + std::to_string(i) + "].node_list_checksum", msum});
            if (!sets.empty() && (ad == fixes || to_fixes(anim, ad))) {
                std::vector<std::string> cmd{u8(tool(cfg, "invader-edit")), "-n", "-t", u8(fixes), anim};   // (-n: a read-only field)
                cmd.insert(cmd.end(), sets.begin(), sets.end());
                std::string out;
                if (run_process(cmd, out) == 0) {
                    note("[fix]  " + anim + ": made for the same bones as " + model + " but stamped with another node "
                         "checksum; now has the model's (" + std::to_string(sets.size() / 3) + " animation" +
                         (sets.size() == 3 ? "" : "s") + ")");
                    return true;
                }
            }
        }
        // Different bones: the object keeps the map's own version of the model (if the map has one).
        if (md == source || !fs::exists(source / from_u8(model))) return false;
        fs::path tdir = fs::path(from_u8(tag)).parent_path();
        std::string stem = fs::path(from_u8(model)).stem().u8string(), copy;
        for (int k = 0; k < 10 && copy.empty(); k++) {
            std::string c = (tdir / (stem + (k ? "_map" + std::to_string(k) : std::string()) + ".model")).generic_u8string();
            if (c != model && owner(c).empty()) copy = c;
        }
        if (copy.empty()) return false;
        std::error_code ec;
        fs::create_directories((fixes / from_u8(copy)).parent_path(), ec);
        fs::copy_file(source / from_u8(model), fixes / from_u8(copy), fs::copy_options::overwrite_existing, ec);
        if (ec || !edit(tag, "model", copy)) return false;
        note("[fix]  " + tag + ": its animations don't fit the target game's " + model + " (different bones); it uses the "
             "map's own version of the model, copied to " + copy);
        return true;
    }

    bool fix(const Failure &f) {
        std::string prefix;
        for (auto &p : f.path) prefix += (prefix.empty() ? "" : ".") + p;
        auto join = [&](const std::string &k) { return prefix.empty() ? k : prefix + "." + k; };
        std::smatch m;
        const std::string &msg = f.msg;
        const std::string &tag = f.tag;
        if (++fix_count[tag + "|" + prefix + "|" + msg] > 2) return false; // same error 3x = fixes not working

        static const std::regex bitmap_type(R"(references \S+ bitmap '.+' where a \S+ was expected)");
        if (std::regex_search(msg, m, bitmap_type) && fix_bitmap_types() > 0) return true;
        static const std::regex anim_frames(R"(Animation #\d+ has an invalid (key frame|second key frame|sound frame) index)");
        if (std::regex_search(msg, m, anim_frames) && fix_animation_frames() > 0) return true;
        static const std::regex empty_chicago(R"(ShaderTransparentChicago(Extended)?::maps\w* must have at least 1 block)");
        if (std::regex_search(msg, m, empty_chicago) && fix_empty_chicago() > 0) return true;
        static const std::regex rotational(R"(Particle type #(\d+) uses complex sprite render mode 'rotational' that requires extra sequence)");
        if (std::regex_search(msg, m, rotational)) {
            if (fix_rotational_particles() > 0) return true;
            if (edit(tag, "particle_types[" + m[1].str() + "].complex_sprite_render_mode", "simple")) {
                note("[fix]  " + tag + ": particle type #" + m[1].str() + " drawn 'simple' (its bitmap lacks the second sequence 'rotational' needs)");
                return true;
            }
        }
        static const std::regex bad_index(R"((\w+)::(\w+) references an invalid index of \S+ \((\d+) >= (\d+)\))");
        if (std::regex_search(msg, m, bad_index)) {
            std::string key = join(m[2]);
            if (edit(tag, key, std::stoi(m[4]) > 0 ? "0" : "null")) {
                note("[fix]  " + tag + ": " + key + " pointed at missing entry #" + m[3].str() + "; reset to 0");
                return true;
            }
        }
        static const std::regex bad_bitmap(R"(Bitmap #(\d+) in .* referenced in (\w+) #(\d+) is out of bounds \(>= (\d+)\))");
        if (std::regex_search(msg, m, bad_bitmap)) {
            std::string key = join(m[2].str() + "s[" + m[3].str() + "].bitmap_index");
            if (edit(tag, key, "0")) {
                note("[fix]  " + tag + ": " + key + " used bitmap #" + m[1].str() + " but only " +
                     m[4].str() + " exist; reset to 0");
                return true;
            }
        }
        static const std::regex node_sums(R"('([^']+)\.(?:gbx)?model' and '([^']+)\.model_animations' animation #\d+ node list checksums are mismatched)");
        if (std::regex_search(msg, m, node_sums) &&
            fix_node_checksums(tag, norm_rel(m[1].str()) + ".model", norm_rel(m[2].str()) + ".model_animations"))
            return true;
        static const std::regex unused_enum(R"((\w+)::(\w+) has an invalid value \((\d+)\))");
        if (std::regex_search(msg, m, unused_enum)) {
            if (fix_unused_enum_values(tag) > 0) return true;
            std::string key = join(m[2]);
            if (edit(tag, key, "0")) {
                note("[fix]  " + tag + ": " + key + " had a value the game never uses (" + m[3].str() + "); reset to default");
                return true;
            }
        }
        static const std::regex bad_enum(R"((\w+)::(\w+) is out of range \((\d+) >= (\d+)\))");
        if (std::regex_search(msg, m, bad_enum)) {
            if (swap_to_source(tag, msg)) return true;
            // Both copies corrupt: borrow a clean copy from another configured tag set
            // (H1A-EK tags or the Xbox tagset), if it has one.
            if (!borrowed.count(tag)) {
                for (fs::path alt : {cfg.ek_dir.empty() ? fs::path() : cfg.ek_dir / "tags", cfg.xbox_tags_dir}) {
                    if (alt.empty() || !fs::exists(alt / from_u8(tag)) || alt == overlay || alt == source) continue;
                    if (to_fixes(tag, alt)) {
                        borrowed.insert(tag);
                        note("[swap] " + tag + ": every copy is corrupt (" + msg + "); using the clean copy from " + u8(alt));
                        return true;
                    }
                }
            }
            std::string key = join(m[2]);
            // Still corrupt: reset to the enum's first value (invader-edit needs its name).
            static const std::map<std::string, std::string> first_value{
                {"grenade_type", "human_fragmentation"}, {"trajectory_type", "toss"},
                {"movement_type", "always_run"}, {"metagame_type", "brute"}};
            auto fv = first_value.find(m[2].str());
            if (fv != first_value.end() && edit(tag, key, fv->second)) {
                note("[fix]  " + tag + ": " + key + " had invalid value " + m[3].str() + "; reset to " + fv->second);
                return true;
            }
            if (edit(tag, key, "0")) {
                note("[fix]  " + tag + ": " + key + " had invalid value " + m[3].str() + "; reset to default");
                return true;
            }
            // Too damaged for invader-edit to open (e.g. scrambled on purpose): let invader-bludgeon reset it.
            fs::path own = owner(tag);
            if (!own.empty() && (own == fixes || to_fixes(tag, own))) {
                std::string out;
                if (run_process({u8(tool(cfg, "invader-bludgeon")), "-t", u8(fixes), "-T", "invalid-enums", tag}, out) == 0) {
                    note("[fix]  " + tag + ": " + key + " had invalid value " + m[3].str() + "; invalid values reset");
                    return true;
                }
            }
        }
        // A sequence index past the end of its bitmap: reset the fields that point into that bitmap.
        static const std::regex bad_seq(R"(Sequence #(\d+) in '([^']+)'\s+referenced\s+(?:by|in)\s.*out of bounds \(>= (\d+)\))");
        if (std::regex_search(msg, m, bad_seq) && std::stoi(m[3]) > 0) {
            std::string idx = m[1], bm = norm_rel(m[2]);
            fs::path own = owner(tag);
            if (!own.empty()) {
                auto kv = list_values(cfg, own, tag);
                auto parent = [](const std::string &k) { size_t d = k.rfind('.'); return d == std::string::npos ? std::string() : k.substr(0, d); };
                auto leaf = [](const std::string &k) { size_t d = k.rfind('.'); return d == std::string::npos ? k : k.substr(d + 1); };
                std::set<std::string> uses;
                for (auto &[k, v] : kv)
                    if (leaf(k).find("bitmap") != std::string::npos && v == bm) uses.insert(parent(k));
                std::string keys;
                for (auto &[k, v] : kv) {
                    std::string l = leaf(k);
                    if (v == idx && l.size() >= 14 && l.compare(l.size() - 14, 14, "sequence_index") == 0 && uses.count(parent(k)) &&
                        edit(tag, k, "0"))
                        keys += (keys.empty() ? "" : ", ") + k;
                }
                if (!keys.empty()) {
                    note("[fix]  " + tag + ": " + keys + " used sequence #" + idx + " of " + bm + ", which only has " +
                         m[3].str() + "; reset to 0");
                    return true;
                }
            }
        }
        static const std::regex out_of_range(R"((\w+)::(\w+) must (?:be )?(?:no more than|at least) )");
        if (std::regex_search(msg, m, out_of_range) && clamp_out_of_range(tag, join(m[2]), msg)) return true;
        static const std::regex bc7(R"(is BC7 which is not implemented)");
        if (std::regex_search(msg, bc7)) {
            fs::path own = owner(tag);
            if (!own.empty() && (own == fixes || to_fixes(tag, own)) && convert_bc7_bitmap_tag(fixes / from_u8(tag), true) > 0) {
                note("[bc7]  " + tag + ": re-encoded its BC7 textures as DXT");
                return true;
            }
        }
        return swap_to_source(tag, msg);
    }
};

std::string friendly_script_error(const std::string &e, Game target) {
    static const std::regex fn(R"(function '([^']+)' is not defined)");
    static const std::regex gl(R"(global '([^']+)' is not defined)");
    std::smatch m;
    if (std::regex_search(e, m, fn))
        return "Your map's scripts call '" + m[1].str() + "', which doesn't exist in " + game_name(target) +
               ". Remove or guard that call in your .hsc script, then try again.\n(" + e + ")";
    if (std::regex_search(e, m, gl))
        return "Your map's scripts use the global '" + m[1].str() + "', which doesn't exist in " +
               game_name(target) + ".\n(" + e + ")";
    static const std::regex nf(R"(can't find (\w+(?: \w+)*?) (?:tag )?['"]([^'"]+)['"])");
    if (std::regex_search(e, m, nf))
        return "Your map's scripts name the " + m[1].str() + " '" + m[2].str() + "', which the " + game_name(target) +
               " build of this map doesn't have.\n(" + e + ")";
    return "Your map's scripts don't compile for " + std::string(game_name(target)) + ": " + e;
}

// Extract stock tags from a folder of maps once and cache them.
fs::path extract_stock(const Config &cfg, const Log &log, const fs::path &maps, const std::string &cache_name,
                       const std::set<std::string> &only, const fs::path &resource_dir, const std::string &label) {
    fs::path dst = cfg.cache_dir / cache_name;
    std::vector<fs::path> stock;
    std::string sig;
    for (auto &e : fs::directory_iterator(maps)) {
        std::string stem = e.path().stem().u8string(), ext = e.path().extension().u8string();
        std::transform(stem.begin(), stem.end(), stem.begin(), ::tolower);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".map" || stem == "bitmaps" || stem == "sounds" || stem == "loc") continue;
        if (!only.empty() && !only.count(stem)) continue;
        stock.push_back(e.path());
    }
    std::sort(stock.begin(), stock.end());
    for (auto &p : stock) sig += p.filename().u8string() + ":" + std::to_string(fs::file_size(p)) + "\n";
    fs::path stamp = dst / ".porter_stamp";
    {
        std::ifstream st(stamp);
        std::stringstream b;
        b << st.rdbuf();
        if (b.str() == sig && !sig.empty()) return dst;
    }
    log("Extracting stock " + label + " tags (one-time, may take a minute or two)...");
    std::error_code ec;
    fs::remove_all(dst, ec);
    fs::create_directories(dst, ec);
    for (auto &p : stock) {
        std::string out;
        std::vector<std::string> cmd{u8(tool(cfg, "invader-extract")), "-t", u8(dst)};
        if (!resource_dir.empty()) cmd.insert(cmd.end(), {"-m", u8(resource_dir)});
        cmd.push_back(u8(p));
        run_process(cmd, out);
    }
    std::ofstream(stamp) << sig;
    return dst;
}

fs::path stock_ce_tags(const Config &cfg, const Log &log, std::string &err) {
    fs::path maps = cfg.ce_dir / "maps";
    if (!fs::exists(maps / "bitmaps.map")) {
        err = "Custom Edition folder isn't set (or has no maps\\bitmaps.map). Set it in Settings.";
        return {};
    }
    static const std::set<std::string> stock_names{
        "beavercreek", "bloodgulch", "boardingaction", "carousel", "chillout", "damnation",
        "dangercanyon", "deathisland", "gephyrophobia", "hangemhigh", "icefields", "infinity",
        "longest", "prisoner", "putput", "ratrace", "sidewinder", "timberland", "wizard", "ui"};
    return extract_stock(cfg, log, maps, "stock_ce_tags", stock_names, maps, "Custom Edition");
}

// Xbox stock tags: the user's Xbox tagset if set, otherwise extracted from Xbox stock maps.
fs::path stock_xbox_tags(const Config &cfg, const Log &log, std::string &err) {
    if (!cfg.xbox_tags_dir.empty() && fs::exists(cfg.xbox_tags_dir / "globals" / "globals.globals"))
        return cfg.xbox_tags_dir;
    if (!cfg.xbox_maps_dir.empty() && fs::exists(cfg.xbox_maps_dir)) {
        for (auto &e : fs::directory_iterator(cfg.xbox_maps_dir))
            if (e.path().extension() == ".map" && detect_game(e.path()) == Game::Xbox)
                return extract_stock(cfg, log, cfg.xbox_maps_dir, "stock_xbox_tags", {}, {}, "Xbox");
    }
    err = "Xbox output needs Xbox stock tags. In Settings, set the Xbox tags folder (e.g. the haloxbox-revised "
          "tagset) or a folder with your Xbox stock maps.";
    return {};
}

// Tags in the user's folder that are stock paths but edited since the kit was installed.
std::vector<std::string> edited_stock_tags(const fs::path &stock, const fs::path &user) {
    std::vector<std::pair<std::string, long long>> both;
    std::map<long long, int> buckets;
    for (auto &e : fs::recursive_directory_iterator(stock)) {
        if (!e.is_regular_file()) continue;
        std::string rel = fs::relative(e.path(), stock).u8string();
        if (rel.rfind(".porter", 0) == 0) continue;
        fs::path u = user / from_u8(rel);
        if (!fs::exists(u)) {
            if (u.extension() == ".model") u.replace_extension(".gbxmodel");
            else if (u.extension() == ".gbxmodel") u.replace_extension(".model");
        }
        std::error_code ec;
        auto t = fs::last_write_time(u, ec);
        if (ec) continue;
        long long secs = std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
        both.push_back({rel, secs});
        buckets[secs / 3600]++;
    }
    std::vector<std::string> edited;
    if (both.empty()) return edited;
    long long base = std::max_element(buckets.begin(), buckets.end(),
                                      [](auto &a, auto &b) { return a.second < b.second; })->first * 3600;
    for (auto &b : both)
        if (b.second > base + 86400) edited.push_back(b.first);
    return edited;
}

fs::path find_scenario(const fs::path &tags) {
    for (auto &e : fs::recursive_directory_iterator(tags))
        if (e.path().extension() == ".scenario") return fs::relative(e.path(), tags);
    return {};
}

std::string tag_path_noext(const fs::path &rel) {
    std::string s = rel.u8string();
    std::replace(s.begin(), s.end(), '\\', '/');
    size_t dot = s.rfind('.');
    return dot == std::string::npos ? s : s.substr(0, dot);
}

bool prepare_for_xbox(Job &job, std::string &err);
size_t shrink_textures_to_fit(Job &job, size_t needed, int &count);
void restrict_to_xbox_mp(Job &job, const fs::path &xsrc);
bool drop_kept_vehicles(Job &job);
void use_stock_versions(Job &job, const fs::path &xsrc);
int guess_vehicle_type(const std::string &tag);
void apply_xbox_slots(Job &job, const fs::path &xsrc);
void ensure_player_animations(Job &job, const fs::path &xsrc);
void fix_kept_weapons(Job &job, const fs::path &xsrc);
void correct_xbox_tagset(Job &job);
void trim_unobtainable(Job &job, const fs::path &xsrc);
void fix_seat_labels(Job &job, const fs::path &xsrc);
static void apply_replacements(Job &job, const fs::path &xsrc);
static bool fix_script_none(Job &job, const std::string &err, std::string &what);
static bool fix_script_shadow(Job &job, const std::string &err, std::string &what);
static bool fix_script_navpoint(Job &job, const std::string &err, std::string &what);
static std::string script_error_line(Job &job, const std::string &err);
static std::set<std::string> add_stock_choices_for_analysis(Job &job);
static bool analyze_built_map(Job &job, const fs::path &map, const std::set<std::string> &added, ContentAnalysis &a,
                              std::string &err);
static double tag_space_used(const std::string &build_out);

// What a built Xbox map asks of the console's memory, read from the .map: its tag data and structure BSPs in the
// 22 MB of tag memory (at 0x803A6000; the tag data from the start, each BSP at its own address near the top), the
// memory between them that OpenCE (test build 24 and later) lends to its texture cache for the map (from the tag
// data's end to the lowest BSP, in whole 16 KB pages, when it's 1 MB or more), and the memory all the map's
// textures take (each bitmap's pixel data).
struct XboxMemoryUse { double tag_mb = 0, bsp_mb = 0, lend_mb = 0, texture_mb = 0; int textures = 0; };
static bool measure_xbox_memory(const fs::path &map, XboxMemoryUse &u) {
    const uint32_t base = 0x803A6000, top = base + 0x1600000, page = 16384;
    std::ifstream f(map, std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (raw.size() < 0x800) return false;
    auto u32 = [](const std::string &b, size_t o) -> uint32_t {
        if (o + 4 > b.size()) return 0;
        const unsigned char *p = (const unsigned char *)b.data() + o;
        return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
    };
    uint32_t file_size = u32(raw, 8), td_off = u32(raw, 0x10), td_size = u32(raw, 0x14);
    std::string body;
    if (raw.size() < file_size) {
        body = raw.substr(0, 0x800);
        std::string rest;
        if (!inflate::zlib((const uint8_t *)raw.data() + 0x800, raw.size() - 0x800, rest, file_size - 0x800)) return false;
        body += rest;
    } else body.swap(raw);
    if (td_size < 16 || (size_t)td_off + td_size > body.size()) return false;
    std::string td = body.substr(td_off, td_size);
    auto at = [&](uint32_t addr) -> size_t { return addr >= base && addr < base + td_size ? addr - base : (size_t)-1; };
    size_t index = at(u32(td, 0));
    uint32_t count = u32(td, 12);
    if (index == (size_t)-1 || count > 65535) return false;
    uint32_t lowest = top;
    double bsp_bytes = 0, tex_bytes = 0;
    for (uint32_t i = 0; i < count; i++) {
        size_t e = index + (size_t)i * 32;
        if (e + 32 > td.size()) return false;
        std::string cls = td.substr(e, 4);
        size_t data = at(u32(td, e + 20));
        if (data == (size_t)-1) continue;
        if (cls == "rncs") {                                          // the scenario: its structure BSPs
            uint32_t n = u32(td, data + 0x5A4);
            size_t list = at(u32(td, data + 0x5A8));
            for (uint32_t k = 0; list != (size_t)-1 && k < n && k < 64; k++) {
                uint32_t size = u32(td, list + k * 32 + 4), address = u32(td, list + k * 32 + 8);
                bsp_bytes += size;
                if (address >= base && address < lowest) lowest = address;
            }
        } else if (cls == "mtib") {                                   // a bitmap: its textures' pixel data
            uint32_t n = u32(td, data + 0x60);
            size_t list = at(u32(td, data + 0x64));
            for (uint32_t k = 0; list != (size_t)-1 && k < n && k < 4096; k++) {
                tex_bytes += u32(td, list + k * 0x30 + 0x1C);
                u.textures++;
            }
        }
    }
    uint32_t from = (base + td_size + page - 1) & ~(page - 1), to = lowest & ~(page - 1);
    double lend = to > from ? (double)(to - from) : 0;
    u.tag_mb = td_size / 1048576.0;
    u.bsp_mb = (lowest < top ? top - lowest : bsp_bytes) / 1048576.0;   // (the BSPs' room: from the lowest to the top)
    u.lend_mb = lend >= 1048576 ? lend / 1048576.0 : 0;
    u.texture_mb = tex_bytes / 1048576.0;
    return true;
}
void convert_bc7_textures(Job &job);
void xbox_content_passes(Job &job);
void xbox_lods(Job &job, const std::vector<std::string> &deps);
void xbox_portals(Job &job);
void xbox_lights(Job &job, const std::vector<std::string> &deps);
void xbox_reflections(Job &job);
void xbox_fog(Job &job);
void xbox_scenery(Job &job, const std::vector<std::string> &deps);
static std::string thousands(long long v);
static std::vector<std::string> performance_lines(Job &job);
static LodView make_lod_view(Job &job, const std::string &map_name);
static void convert_scripts(Job &job, const fs::path &dir, Game target);
bool prepare_models_for_pc(Job &job, std::string &err);

void keep_user_edits(Job &job);

// The tags a tag file refers to (paths with extension, forward slashes), read straight from its bytes: a reference
// is a 16-byte record (class, 0, path length, 0xFFFFFFFF) and its path comes later in the file, NUL-terminated.
// Abstract classes (object, unit, item, device, shader) give every concrete kind; callers check what exists.
static std::vector<std::string> tag_file_references(const std::string &data) {
    static const std::map<std::string, std::vector<const char *>> kinds = {
        {"actr", {"actor"}}, {"actv", {"actor_variant"}}, {"ant!", {"antenna"}}, {"antr", {"model_animations"}},
        {"bipd", {"biped"}}, {"bitm", {"bitmap"}}, {"boom", {"spheroid"}}, {"cdmg", {"continuous_damage_effect"}},
        {"coll", {"model_collision_geometry"}}, {"colo", {"color_table"}}, {"cont", {"contrail"}},
        {"ctrl", {"device_control"}}, {"deca", {"decal"}}, {"DeLa", {"ui_widget_definition"}},
        {"devc", {"input_device_defaults"}}, {"dobc", {"detail_object_collection"}}, {"effe", {"effect"}},
        {"elec", {"lightning"}}, {"eqip", {"equipment"}}, {"flag", {"flag"}}, {"fog ", {"fog"}}, {"font", {"font"}},
        {"foot", {"material_effects"}}, {"garb", {"garbage"}}, {"glw!", {"glow"}}, {"grhi", {"grenade_hud_interface"}},
        {"hmt ", {"hud_message_text"}}, {"hud#", {"hud_number"}}, {"hudg", {"hud_globals"}},
        {"itmc", {"item_collection"}}, {"jpt!", {"damage_effect"}}, {"lens", {"lens_flare"}},
        {"lifi", {"device_light_fixture"}}, {"ligh", {"light"}}, {"lsnd", {"sound_looping"}}, {"mach", {"device_machine"}},
        {"matg", {"globals"}}, {"metr", {"meter"}}, {"mgs2", {"light_volume"}}, {"mod2", {"gbxmodel"}}, {"mode", {"model"}},
        {"mply", {"multiplayer_scenario_description"}}, {"ngpr", {"preferences_network_game"}}, {"part", {"particle"}},
        {"pctl", {"particle_system"}}, {"phys", {"physics"}}, {"plac", {"placeholder"}}, {"pphy", {"point_physics"}},
        {"proj", {"projectile"}}, {"rain", {"weather_particle_system"}}, {"sbsp", {"scenario_structure_bsp"}},
        {"scen", {"scenery"}}, {"scex", {"shader_transparent_chicago_extended"}}, {"schi", {"shader_transparent_chicago"}},
        {"scnr", {"scenario"}}, {"senv", {"shader_environment"}}, {"sgla", {"shader_transparent_glass"}},
        {"sky ", {"sky"}}, {"smet", {"shader_transparent_meter"}}, {"snd!", {"sound"}}, {"snde", {"sound_environment"}},
        {"soso", {"shader_model"}}, {"sotr", {"shader_transparent_generic"}}, {"Soul", {"ui_widget_collection"}},
        {"spla", {"shader_transparent_plasma"}}, {"ssce", {"sound_scenery"}}, {"str#", {"string_list"}},
        {"swat", {"shader_transparent_water"}}, {"tagc", {"tag_collection"}}, {"trak", {"camera_track"}},
        {"udlg", {"dialogue"}}, {"unhi", {"unit_hud_interface"}}, {"ustr", {"unicode_string_list"}},
        {"vcky", {"virtual_keyboard"}}, {"vehi", {"vehicle"}}, {"weap", {"weapon"}}, {"wind", {"wind"}},
        {"wphi", {"weapon_hud_interface"}},
        {"obje", {"biped", "vehicle", "weapon", "equipment", "garbage", "projectile", "scenery", "device_machine",
                  "device_control", "device_light_fixture", "placeholder", "sound_scenery"}},
        {"unit", {"biped", "vehicle"}}, {"item", {"weapon", "equipment", "garbage"}},
        {"devi", {"device_machine", "device_control", "device_light_fixture"}},
        {"shdr", {"shader_environment", "shader_model", "shader_transparent_chicago", "shader_transparent_chicago_extended",
                  "shader_transparent_generic", "shader_transparent_glass", "shader_transparent_meter",
                  "shader_transparent_plasma", "shader_transparent_water"}}};
    auto be = [&](size_t o) {
        return (uint32_t)(uint8_t)data[o] << 24 | (uint32_t)(uint8_t)data[o + 1] << 16 | (uint32_t)(uint8_t)data[o + 2] << 8 |
               (uint32_t)(uint8_t)data[o + 3];
    };
    std::map<size_t, std::set<const std::vector<const char *> *>> by_length;   // path length -> kinds it may be
    static const std::string ff("\xff\xff\xff\xff", 4);
    for (size_t at = data.find(ff, 12); at != std::string::npos; at = data.find(ff, at + 1)) {
        size_t p = at - 12;
        uint32_t len = be(p + 8);
        if (be(p + 4) != 0 || len == 0 || len > 255) continue;
        auto k = kinds.find(data.substr(p, 4));
        if (k != kinds.end()) by_length[len].insert(&k->second);
    }
    std::set<std::string> out;
    if (by_length.empty()) return {};
    for (size_t i = 0, start = 0; i < data.size(); i++) {
        unsigned char c = (unsigned char)data[i];
        if (c >= 0x20 && c < 0x7f) continue;
        if (c == 0 && i > start)
            for (auto &[len, ks] : by_length) {
                if (len > i - start) break;
                std::string path = data.substr(i - len, len);
                std::replace(path.begin(), path.end(), '\\', '/');
                for (auto *k : ks)
                    for (auto *ext : *k) out.insert(path + "." + ext);
            }
        start = i + 1;
    }
    return std::vector<std::string>(out.begin(), out.end());
}

// Which tags refer to each of `targets` (tag paths with extension), read straight from the tag files: a reference
// is stored as the tag's path (backslashes, NUL-terminated). One pass over the files instead of
// `invader-dependency -R` per tag, which parses every tag file each time (with a mod's own bitmaps.map/sounds.map
// extracted that's hundreds of MB per call, minutes on Windows). `dirs` in build order: the first copy of a tag is
// the one read. Kinds of tags that can't refer to the targets' kinds (bitmaps, sounds, models...) are skipped. May
// list a tag whose reference only ends the same way (a longer path): callers check the actual values.
// With `roots` (the scenario, the globals), only tags the map uses are read: found by following references out
// from the roots. Without it every tag in `dirs` is read, which with a full Xbox tagset (the campaign's levels,
// characters...) is tens of thousands of files: minutes on a hard disk.
std::map<std::string, std::set<std::string>> find_referrers(const std::vector<fs::path> &dirs, const std::set<std::string> &targets,
                                                            const std::vector<std::string> &roots = {}) {
    std::map<std::string, std::set<std::string>> found;
    if (targets.empty()) return found;
    static const std::set<std::string> never = {".bitmap", ".sound", ".model_animations", ".model_collision_geometry", ".physics",
                                                ".unicode_string_list", ".string_list"};
    // ...and these can't refer to an object (weapon, vehicle, ...): textures, geometry, effects' visual parts.
    static const std::set<std::string> no_objects = {
        ".gbxmodel", ".model", ".scenario_structure_bsp", ".sound_environment", ".sound_looping", ".font", ".hud_message_text",
        ".camera_track", ".dialogue", ".particle", ".decal", ".lens_flare", ".light", ".glow", ".contrail", ".lightning",
        ".light_volume", ".wind", ".weather_particle_system", ".meter", ".point_physics", ".antenna", ".flag", ".sky", ".fog",
        ".detail_object_collection"};
    static const std::set<std::string> objects = {".weapon", ".vehicle", ".equipment", ".biped", ".scenery", ".garbage",
                                                  ".projectile", ".device_machine", ".device_control", ".device_light_fixture",
                                                  ".placeholder", ".sound_scenery"};
    bool only_objects = true;
    std::vector<std::pair<std::string, std::string>> needles;   // (lowercase "path\\x\0", target)
    for (auto &t : targets) {
        std::string ext = fs::path(from_u8(t)).extension().u8string();
        if (!objects.count(ext)) only_objects = false;
        std::string n = t.substr(0, t.size() - ext.size());
        std::replace(n.begin(), n.end(), '/', '\\');
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        n.push_back('\0');
        needles.push_back({n, t});
    }
    auto candidate = [&](const std::string &ext) {
        return !never.count(ext) && !(only_objects && (no_objects.count(ext) || ext.rfind(".shader", 0) == 0));
    };
    std::string data, low;
    auto search = [&](const std::string &rel) {
        low = data;
        std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        for (auto &[n, t] : needles)
            if (rel != t && low.find(n) != std::string::npos) found[t].insert(rel);
    };
    auto read = [&](const fs::path &f) {
        std::ifstream in(f, std::ios::binary);
        data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };

    if (!roots.empty()) {
        // Kinds whose references can't lead to anything that refers to an object (a particle can: its effects).
        auto follow = [&](const std::string &ext) {
            if (ext == ".bitmap" || ext == ".sound") return false;
            if (!only_objects || ext == ".particle") return true;
            return !never.count(ext) && !no_objects.count(ext) && ext.rfind(".shader", 0) != 0;
        };
        // Whether a tag file exists, from listings of the folders asked about (most candidate paths read from a
        // file aren't tags: one listing per folder instead of a failed lookup each).
        std::map<fs::path, std::set<std::string>> listing;   // folder -> lowercase file names
        auto lower = [](std::string x) {
            std::transform(x.begin(), x.end(), x.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            return x;
        };
        auto has = [&](const fs::path &d, const std::string &rel) {
            if (d.empty()) return false;
            fs::path full = d / from_u8(rel), parent = full.parent_path();
            auto it = listing.find(parent);
            if (it == listing.end()) {
                std::set<std::string> names;
                std::error_code ec;
                for (auto e = fs::directory_iterator(parent, ec); !ec && e != fs::directory_iterator(); e.increment(ec))
                    names.insert(lower(e->path().filename().u8string()));
                it = listing.emplace(parent, std::move(names)).first;
            }
            return it->second.count(lower(full.filename().u8string())) > 0;
        };
        std::set<std::string> queued;
        std::vector<std::string> todo;
        auto add = [&](const std::string &rel) {
            if (!queued.insert(rel).second) return;
            for (auto &d : dirs)
                if (has(d, rel)) { todo.push_back(rel); return; }
        };
        for (auto &r : roots) add(r);
        while (!todo.empty()) {
            std::string rel = todo.back();
            todo.pop_back();
            std::string ext = fs::path(from_u8(rel)).extension().u8string();
            bool cand = candidate(ext), fol = follow(ext), first = true;
            if (!cand && !fol) continue;
            for (auto &d : dirs) {   // every copy: which one the build reads differs for the scenario and BSPs
                if (!has(d, rel)) continue;
                read(d / from_u8(rel));
                if (first && cand) search(rel);
                first = false;
                if (fol)
                    for (auto &r : tag_file_references(data)) add(r);
            }
        }
        return found;
    }

    std::set<std::string> seen;
    for (auto &d : dirs) {
        if (d.empty() || !fs::exists(d)) continue;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(d, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            std::string ext = it->path().extension().u8string();
            if (!candidate(ext)) continue;
            std::string rel = fs::relative(it->path(), d, ec).generic_u8string();
            if (!seen.insert(rel).second) continue;   // an earlier folder's copy is the one the build reads
            read(it->path());
            search(rel);
        }
    }
    return found;
}

// Where a map's size goes (approximate: tag file sizes of everything the map uses), for size errors.
std::string size_breakdown(Job &job) {
    std::string out;
    std::vector<std::string> cmd{u8(tool(job.cfg, "invader-dependency")), "-r"};
    for (auto &d : job.dirs()) cmd.insert(cmd.end(), {"-t", u8(d)});
    cmd.push_back(job.scenario + ".scenario");
    run_process(cmd, out);
    std::map<std::string, double> mb;
    double bsp = 0;
    std::istringstream ss(out + "\n" + job.scenario + ".scenario");
    std::string dep;
    std::set<std::string> seen;
    while (std::getline(ss, dep)) {
        dep = norm_rel(dep);
        if (dep.empty() || !seen.insert(dep).second) continue;
        fs::path own = job.owner(dep);
        if (own.empty()) continue;
        std::error_code ec;
        double size = (double)fs::file_size(own / from_u8(dep), ec) / 1048576.0;
        if (ec) continue;
        std::string ext = fs::path(from_u8(dep)).extension().u8string(), what = "other";
        if (own == job.overlay) what = "stock content";
        else if (ext == ".bitmap") what = "textures";
        else if (ext == ".sound") what = "sounds";
        else if (ext == ".model" || ext == ".gbxmodel") what = "models";
        else if (ext == ".scenario_structure_bsp") { what = "level geometry (BSP)"; bsp = std::max(bsp, size); }
        else if (ext == ".model_animations") what = "animations";
        mb[what] += size;
    }
    std::vector<std::pair<double, std::string>> parts;
    for (auto &[k, v] : mb) parts.push_back({v, k});
    std::sort(parts.rbegin(), parts.rend());
    std::string r = "\nWhere the size goes (approximate, from tag file sizes): ";
    for (size_t i = 0; i < parts.size() && i < 6; i++) {
        char buf[64];
        snprintf(buf, sizeof buf, "%.1f MB", parts[i].first);
        r += (i ? ", " : "") + parts[i].second + " " + buf;
    }
    r += ".";
    (void)bsp;
    return r;
}

// Recover the map's scripts from its scenario tag, convert them for the target and compile those
// instead. Returns false if there was nothing to recover.
bool recover_and_convert_scripts(Job &job) {
    fs::path own = job.owner(job.scenario + ".scenario");
    if (own.empty()) return false;
    fs::path sdata = job.work / "script_data";
    std::error_code ec;
    fs::remove_all(sdata, ec);
    fs::create_directories(sdata, ec);
    std::string out;
    run_process({u8(tool(job.cfg, "invader-recover")), "-t", u8(own), "-d", u8(sdata), job.scenario + ".scenario"}, out);
    bool any = false;
    for (auto &e : fs::recursive_directory_iterator(sdata, ec))
        if (e.path().extension() == ".hsc") any = true;
    if (!any) return false;
    job.log("  Converting the map's scripts for " + std::string(game_name(job.target)) + "...");
    convert_scripts(job, sdata, job.target);
    job.extra = {"-d", u8(sdata), "-S", "data"};
    return true;
}

uint32_t crc32_of(const char *d, size_t n);

// Invader leaves the checksum in an Xbox map's header at 0xFFFFFFFF, so every build of a map has the
// same header checksum. Stamp a CRC32 of the map's body there instead: a rebuilt map then always has a
// different header, which is what the Xbox (with the cache-refresh XBE) uses to tell that its hard-drive
// cache copy is stale. Nothing on the Xbox verifies this value against the map's contents.
bool stamp_xbox_checksum(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    std::string b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (b.size() <= 0x800 || b.compare(0, 4, "daeh") != 0 || b.compare(0x7FC, 4, "toof") != 0) return false;
    uint32_t crc = crc32_of(b.data() + 0x800, b.size() - 0x800);
    if (crc == 0xFFFFFFFFu) crc = 0xFFFFFFFEu;
    char le[4] = {(char)crc, (char)(crc >> 8), (char)(crc >> 16), (char)(crc >> 24)};
    std::fstream out(p, std::ios::in | std::ios::out | std::ios::binary);
    if (!out) return false;
    out.seekp(0x64);
    out.write(le, 4);
    return (bool)out;
}

// The map's own scenario and BSPs (with each BSP's lightmaps, and the scenario's own text) are used even when
// they sit at a stock path:
// they're what make the level, and a ported map has nearly always changed them. Without this the target
// game's stock copies of those paths would win, like other stock tags do.
void keep_map_level_tags(Job &job) {
    if (job.overlay.empty()) return;
    std::string scen = job.scenario + ".scenario";
    if (!fs::exists(job.source / from_u8(scen))) return;
    // Single-value lookups: a full listing (invader-edit -L) of a BSP takes minutes on a big level.
    auto get = [&](const std::string &tag, const std::string &key) {
        std::string v;
        if (run_process({u8(tool(job.cfg, "invader-edit")), "-t", u8(job.source), tag, "-G", key}, v) != 0) return std::string();
        while (!v.empty() && (v.back() == '\n' || v.back() == ' ')) v.pop_back();
        if (v.empty() || v[0] == '.' || v.find('\n') != std::string::npos) return std::string(); // null reference
        return norm_rel(v);
    };
    std::vector<std::string> tags{scen};
    // The scenario's own text, which its scripts refer to by name.
    for (const char *k : {"hud_messages", "custom_object_names", "ingame_help_text"}) {
        std::string v = get(scen, k);
        if (!v.empty()) tags.push_back(v);
    }
    std::string count;
    run_process({u8(tool(job.cfg, "invader-edit")), "-t", u8(job.source), scen, "-C", "structure_bsps"}, count);
    int bsps = std::atoi(count.c_str());
    for (int i = 0; i < bsps && i < 64; i++) {
        std::string v = get(scen, "structure_bsps[" + std::to_string(i) + "].structure_bsp");
        if (v.empty()) continue;
        tags.push_back(v);
        if (!fs::exists(job.source / from_u8(v))) continue;
        std::string lm = get(v, "lightmaps_bitmap");
        if (!lm.empty()) tags.push_back(lm);
    }
    std::vector<std::string> kept;
    for (auto &t : tags)
        if (fs::exists(job.source / from_u8(t)) && fs::exists(job.overlay / from_u8(t)) && job.keep_source.insert(t).second)
            kept.push_back(t);
    if (kept.empty()) return;
    std::string list = kept[0];
    for (size_t i = 1; i < kept.size(); i++) list += ", " + kept[i];
    job.note("[keep] the map's own level tags are used even though stock " + std::string(game_name(job.target)) +
             " has tags at the same path: " + list);
}

// After the preparation passes: put those map versions where the build finds them first.
void place_kept_level_tags(Job &job) {
    for (auto &t : job.keep_source)
        if (!fs::exists(job.fixes / from_u8(t)) && fs::exists(job.source / from_u8(t))) job.to_fixes(t, job.source);
}

int untyped_script_nodes(const Job &job, const fs::path &map);

Result run_build(Job &job, const std::string &scenario, const fs::path &output, bool share) {
    job.scenario = scenario;
    Result r;
    const Config &cfg = job.cfg;
    std::error_code ec;
    fs::create_directories(output.parent_path(), ec);
    fs::path tmp_out = job.work / "out.map";
    keep_map_level_tags(job);

    if (job.target == Game::Xbox) {
        std::string err;
        if (!prepare_for_xbox(job, err)) { r.error = err; return r; }
    } else {
        std::string err;
        if (!prepare_models_for_pc(job, err)) { r.error = err; return r; }
    }
    if (!job.user_tags.empty() && !job.overlay.empty() && job.target != Game::MCC) keep_user_edits(job);
    if (job.target != Game::MCC) convert_bc7_textures(job);
    if (job.target == Game::Xbox) xbox_content_passes(job);
    place_kept_level_tags(job);
    std::vector<std::string> performance;   // the report's "Performance" section (Xbox)
    if (job.target == Game::Xbox && !job.analysis) {
        performance = performance_lines(job);
        if (!performance.empty()) {
            job.log("Performance (what the Xbox has to draw):");
            for (auto &l : performance) job.log("  " + l);
        }
    }
    r.shaders = job.shaders_file;
    if (cfg.level_shaders_only) {   // (measured: nothing more to do)
        r.ok = !job.shaders_file.empty();
        if (!r.ok) r.error = "The map's level couldn't be measured.";
        return r;
    }
    // The models whose levels of detail changed, for the LODs window (drawn now, while the made versions are in).
    LodView lod_view;
    fs::path lod_file;
    if (job.target == Game::Xbox && !job.analysis && !job.lod_changes.empty()) {
        lod_view = make_lod_view(job, output.stem().u8string());
        lod_file = job.cfg.cache_dir / "lods" / (output.stem().u8string() + ".txt");
        if (save_lod_view(lod_view, lod_file)) r.lods = lod_file;
    }

    std::string stem = output.stem().u8string();
    bool optimized = false;   // Invader's --optimize: identical data stored once (only when the tag space is short)
    auto make_cmd = [&] {
        std::vector<std::string> cmd{u8(tool(cfg, "invader-build")), "-g", game_engine(job.target), "-H"};
        if (optimized) cmd.push_back("-O");
        if (job.target == Game::CE && share) {
            cmd.insert(cmd.end(), {"-r", "check", "-R", u8(cfg.ce_dir / "maps")});
        }
        for (auto &d : job.dirs()) cmd.insert(cmd.end(), {"-t", u8(d)});
        cmd.insert(cmd.end(), job.extra.begin(), job.extra.end());
        if (job.target == Game::Xbox && cfg.xbox_max_mb > 0) cmd.push_back("-E"); // we check the bigger limit ourselves
        if (job.analysis) cmd.insert(cmd.end(), {"-E", "-T", "128M", "-l", "0"});  // measure: no limits, quick
        std::string scen_name = from_u8(scenario).filename().u8string();
        // The map's internal name must match its file name (Halo compares them case-insensitively),
        // and tag paths must be lowercase, so rename with the lowercased file name.
        std::string lower = stem;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower != scen_name) cmd.insert(cmd.end(), {"-N", lower});
        cmd.insert(cmd.end(), {"-o", u8(tmp_out), scenario});
        return cmd;
    };
    std::vector<std::string> cmd = make_cmd();
    bool scripts_tried = false;
    int none_fixes = 0;
    // The map doesn't fit the Xbox with the lower-detail versions the app made: take them out again (they're for
    // speed; the map's content comes first). `why` empty: quietly (measuring the map as it is without them).
    auto undo_lods = [&](const std::string &why) {
        if (job.lod_backups.empty()) return false;
        for (auto &[p, b] : job.lod_backups) std::ofstream(p, std::ios::binary | std::ios::trunc) << b;
        char mb[32];
        snprintf(mb, sizeof mb, "%.1f", job.lod_xbox_bytes / 1048576.0);
        job.lod_backups.clear();
        job.lod_xbox_bytes = 0;
        if (!lod_file.empty()) {   // the LODs window shows they were taken out
            for (auto &m : lod_view.models)
                if (m.change == "made" || m.change == "library") m.undone = true;
            save_lod_view(lod_view, lod_file);
        }
        if (!why.empty()) {
            job.note("[lod] the map " + why + ", so the lower-detail versions made for it (" + mb + " MB) were taken out again");
            performance = performance_lines(job);   // the report's, with those models as they are
        }
        return true;
    };
    std::set<std::string> stock_choices;   // analysis: Xbox weapons/vehicles added to be measured as replacements
    if (job.analysis) {
        stock_choices = add_stock_choices_for_analysis(job);
        cmd = make_cmd();
    }

    // Reads an analysis build (`mcmd`, built to `mout`, output `bout`) back into `a`.
    auto read_measurement = [&](ContentAnalysis &a, const std::vector<std::string> &mcmd, const fs::path &mout,
                                const std::string &bout, std::string &err) {
        a.map_name = stem;
        a.file_limit_mb = cfg.xbox_max_mb > 0 ? cfg.xbox_max_mb : 47;
        if (!analyze_built_map(job, mout, stock_choices, a, err)) return false;
        if (a.used_mb > a.limit_mb) {   // what the optimize rebuild (used when a map is over) would save
            job.log("Measuring what Invader's optimize option saves...");
            double before = tag_space_used(bout);
            std::vector<std::string> oc = mcmd;
            oc.insert(oc.end() - 3, "-O");
            std::string oout, nout;
            // Both builds without the stock choices, so the difference is the map's own.
            fs::path sc = job.choices_scenario, bak = job.work / "choices_backup.scenario", with = job.work / "choices_with.scenario";
            std::error_code ec;
            bool swapped = !sc.empty() && fs::exists(bak);
            if (swapped) { fs::copy_file(sc, with, fs::copy_options::overwrite_existing, ec); fs::copy_file(bak, sc, fs::copy_options::overwrite_existing, ec); }
            if (swapped && run_process(mcmd, nout) == 0) before = tag_space_used(nout);
            if (run_process(oc, oout) == 0 && before > 0) a.optimize_saves_mb = std::max(0.0, before - tag_space_used(oout));
            if (swapped) fs::copy_file(with, sc, fs::copy_options::overwrite_existing, ec);
        }
        return true;
    };
    // A convert that doesn't fit: measure it for the weapons & vehicles window now, from the tags already prepared,
    // so the window opens without preparing the map again. Only without picks (the window measures the map as it
    // is) and before anything removes content (dropping kept vehicles). The convert's own tags are left as they were.
    bool measured = false;
    auto measure_for_window = [&] {
        if (!job.measure || measured || job.analysis || job.target != Game::Xbox || !cfg.xbox_replace.empty()) return;
        measured = true;
        undo_lods("");   // as the window measures a map: without the lower-detail versions the app makes
        job.log("Measuring the map for the weapons & vehicles window...");
        job.analysis = job.measure;   // analysis build flags
        stock_choices = add_stock_choices_for_analysis(job);
        bool saved_opt = optimized;
        optimized = false;
        std::vector<std::string> mcmd = make_cmd();
        optimized = saved_opt;
        job.analysis = nullptr;
        fs::path mout = job.work / "measure.map";
        auto o = std::find(mcmd.begin(), mcmd.end(), u8(tmp_out));
        if (o != mcmd.end()) *o = u8(mout);
        // Under the scenario's own name, as the window's measurement is (a renamed scenario would hide which tag it is).
        auto nn = std::find(mcmd.begin(), mcmd.end(), std::string("-N"));
        if (nn != mcmd.end() && nn + 1 != mcmd.end()) mcmd.erase(nn, nn + 2);
        std::string bout, err;
        ContentAnalysis a;
        // The Xbox weapons/vehicles added as choices may need the same automatic fixes as the map's own tags.
        job.quiet = true;
        bool built = false;
        for (int t = 0; t < 40 && !built; t++) {
            built = run_process(mcmd, bout) == 0;
            if (built) break;
            Failure mf;
            if (!parse_failure(bout, mf) || !mf.script_error.empty() || mf.tag.empty() || !job.fix(mf)) {
                err = !mf.msg.empty() ? mf.msg : "the measuring build failed";
                std::ofstream(job.work / "measure.log") << bout;
                break;
            }
        }
        bool ok = built && read_measurement(a, mcmd, mout, bout, err);
        job.quiet = false;
        // Put the scenario back as the convert had it.
        std::error_code ec;
        fs::path bak = job.work / "choices_backup.scenario";
        if (!job.choices_scenario.empty() && fs::exists(bak)) fs::copy_file(bak, job.choices_scenario, fs::copy_options::overwrite_existing, ec);
        job.choices_scenario.clear();
        stock_choices.clear();
        fs::remove(mout, ec);
        if (ok) {
            a.map_name = from_u8(job.scenario).filename().u8string();
            *job.measure = std::move(a);
        }
        else job.log("  (couldn't measure it" + (err.empty() ? std::string() : ": " + err) + ")");
    };

    job.log(std::string(job.analysis ? "Measuring the map for " : "Building for ") + game_name(job.target) + "...");
    std::string out;
    int shrink_rounds = 0;
    bool shrink_hopeless = false;
    int texture_rounds = 0;
    bool texture_hopeless = false;
    std::string texture_warning;
    for (int attempt = 0; attempt < 200; attempt++) {
        int rc = run_process(cmd, out);
        if (rc == 0 && job.analysis) {
            std::ofstream(job.work / "build.log") << out;
            std::string err;
            if (!read_measurement(*job.analysis, cmd, tmp_out, out, err)) { r.error = "Couldn't measure the map: " + err; return r; }
            r.ok = true;
            return r;
        }
        if (rc == 0 && job.target == Game::Xbox && cfg.xbox_max_mb > 0) {
            // Modded Xbox with a bigger map limit: Invader was told not to check, so check against that limit.
            std::string sz;
            run_process({u8(tool(cfg, "invader-info")), "-T", "uncompressed_size", u8(tmp_out)}, sz);
            double mb = std::atof(sz.c_str()) / 1048576.0;
            if (mb > cfg.xbox_max_mb) {
                char buf[160];
                snprintf(buf, sizeof buf, "\nFATAL ERROR: Map file exceeds maximum size for the target engine when uncompressed "
                                          "(%.4f MiB > %.4f MiB)\n", mb, cfg.xbox_max_mb);
                out += buf;
                rc = 1;
            }
        }
        std::ofstream(job.work / "build.log") << out;
        if (rc == 0 && job.target == Game::Xbox) {
            // The texture memory target: all the map's textures should fit the target plus the tag memory the map
            // leaves free (OpenCE lends that to its texture cache). Over it, halve the map's own textures and build again.
            XboxMemoryUse use;
            if (measure_xbox_memory(tmp_out, use)) {
                double target = cfg.xbox_texture_target_mb, budget = target + use.lend_mb;
                char buf[400];
                snprintf(buf, sizeof buf, "tag memory %.1f MB of 22 (tag data %.1f MB, BSP %.1f MB); textures %.1f MB",
                         use.tag_mb + use.bsp_mb, use.tag_mb, use.bsp_mb, use.texture_mb);
                r.memory = buf;
                if (target > 0) {
                    if (use.lend_mb > 0)
                        snprintf(buf, sizeof buf, "; target %.0f MB + %.1f MB of free tag memory (OpenCE lends it) = %.1f MB",
                                 target, use.lend_mb, budget);
                    else snprintf(buf, sizeof buf, "; target %.0f MB (the map leaves less than 1 MB of tag memory free)", target);
                    r.memory += buf;
                }
                if (target > 0 && use.texture_mb > budget + 0.05) {
                    if (cfg.xbox_shrink && texture_rounds < 6 && !texture_hopeless) {
                        size_t need = (size_t)((use.texture_mb - budget) * 1048576.0) + 256 * 1024;
                        int n = 0;
                        size_t saved = shrink_textures_to_fit(job, need, n);
                        if (saved > 0) {
                            texture_rounds++;
                            snprintf(buf, sizeof buf, "[textures] %.1f MB of textures is over the %.1f MB target; halved %d of "
                                     "your textures (saved %.1f MB)", use.texture_mb, budget, n, saved / 1048576.0);
                            job.note(buf);
                            if (saved < need && saved / 3 < need - saved) texture_hopeless = true;
                            continue;
                        }
                    }
                    snprintf(buf, sizeof buf, "WARNING: the map's textures take %.1f MB, over the %.1f MB texture target%s: "
                             "they may not all fit the Xbox's texture memory at once (slowdowns where many are in view)",
                             use.texture_mb, budget, !cfg.xbox_shrink ? " (\"Shrink textures to fit\" is off)"
                             : texture_rounds ? " even with your textures shrunk" : "; your own textures can't shrink further");
                    texture_warning = buf;
                }
            }
        }
        if (rc == 0) break;
        Failure f;
        // An error in a level the app gave portals: the level goes back as it was, and the map builds again.
        if (!job.portal_backups.empty()) {
            bool bsp_error = false;
            std::istringstream ls(out);
            for (std::string l; std::getline(ls, l);)
                if (l.find("ERROR") != std::string::npos && l.find(".scenario_structure_bsp") != std::string::npos) bsp_error = true;
            if (bsp_error) {
                for (auto &[p, b] : job.portal_backups) std::ofstream(p, std::ios::binary | std::ios::trunc) << b;
                job.portal_backups.clear();
                job.note("[portals] Invader didn't accept the level with the portals the app added, so it was built as it was");
                performance = performance_lines(job);
                cmd = make_cmd();
                continue;
            }
        }
        static const std::regex too_big(R"(exceeds (?:the )?maximum size[^(]*\(([\d.]+) (?:MiB )?> ([\d.]+))");
        std::smatch tb;
        // Too big with the lower-detail versions the app made, and taking them out is enough: they come out (before
        // any texture is halved). Shrinking textures may get it close enough for that later.
        if (std::regex_search(out, tb, too_big) && job.target == Game::Xbox && !job.lod_backups.empty() &&
            (std::atof(tb[1].str().c_str()) - std::atof(tb[2].str().c_str())) * 1048576.0 < job.lod_xbox_bytes * 0.9 &&
            undo_lods("is " + tb[1].str().substr(0, 5) + " MB, over the Xbox's " + tb[2].str().substr(0, 5) + " MB limit")) {
            cmd = make_cmd();
            continue;
        }
        if (std::regex_search(out, tb, too_big) && job.target == Game::Xbox && cfg.xbox_shrink && shrink_rounds < 6 &&
            !shrink_hopeless) {
            double over = std::atof(tb[1].str().c_str()) - std::atof(tb[2].str().c_str());
            size_t need = (size_t)(over * 1048576.0) + 256 * 1024;
            int n = 0;
            size_t saved = shrink_textures_to_fit(job, need, n);
            if (saved > 0) {
                shrink_rounds++;
                job.note("[shrink] " + tb[1].str().substr(0, 5) + " MB is over the limit; halved " + std::to_string(n) +
                         " of your textures (saved " + std::to_string(saved / 1048576.0).substr(0, 4) + " MB)");
                // Halving again saves about a quarter as much each time; stop if that can't cover the rest.
                if (saved < need && saved / 3 < need - saved) shrink_hopeless = true;
                continue;
            }
        }
        if (std::regex_search(out, tb, too_big)) {
            measure_for_window();
            r.error = std::string("The map is too big for ") + game_name(job.target) + ": " + tb[1].str().substr(0, 5) +
                      " MB, but the limit is " + tb[2].str().substr(0, 5) + " MB.\n";
            if (job.target == Game::Xbox)
                r.error += "Xbox maps can't share textures or sounds with other maps, and stock multiplayer content "
                           "alone uses about 42 MB of that.";
            if (job.target == Game::Xbox && !cfg.xbox_shrink)
                r.error += " Try ticking \"Shrink textures to fit\".";
            else if (job.target == Game::Xbox)
                r.error += " Even with the size options it doesn't fit, so the map's own content needs trimming.";
            r.error += size_breakdown(job);
            return r;
        }
        static const std::regex tag_space(R"(Maximum tag space exceeded \(([\d.]+) MiB > ([\d.]+) MiB\))");
        // Over the tag memory limit: first store identical data only once (loses nothing; the build is slower).
        if (std::regex_search(out, tb, tag_space) && job.target == Game::Xbox && !optimized) {
            optimized = true;
            job.note("[optimize] the tag data needed " + tb[1].str().substr(0, 5) + " MB of the " + tb[2].str().substr(0, 5) +
                     " MB limit; rebuilding with Invader's optimize option, which stores identical data only once");
            cmd = make_cmd();
            continue;
        }
        // Still over the tag memory limit: the lower-detail versions the app made come out before anything of the
        // map's is given up (and the numbers in the message are then the map's own).
        if (std::regex_search(out, tb, tag_space) && job.target == Game::Xbox &&
            undo_lods("needs " + tb[1].str().substr(0, 5) + " MB of the Xbox's " + tb[2].str().substr(0, 5) + " MB of tag memory")) {
            cmd = make_cmd();
            continue;
        }
        if (std::regex_search(out, tb, tag_space) && job.target == Game::Xbox) measure_for_window();
        if (std::regex_search(out, tb, tag_space) && job.target == Game::Xbox && drop_kept_vehicles(job)) {
            cmd = make_cmd();
            continue;
        }
        if (std::regex_search(out, tb, tag_space)) {
            r.error = std::string("The map needs more memory than ") + game_name(job.target) + " has for it: its level geometry " +
                      "(BSP) and other tag data need " + tb[1].str().substr(0, 5) + " MB" +
                      (optimized ? " even with identical data stored once" : "") + ", but the limit is " +
                      tb[2].str().substr(0, 5) + " MB (the same on every Xbox, modded or not). Textures and sounds don't " +
                      "count toward this, so shrinking them doesn't help.";
            if (job.target == Game::Xbox)
                r.error += "\nTo save memory, on the Xbox tab: keep \"Use the Xbox's own weapons, vehicles and player model\" "
                           "ticked, and set \"Other custom ones\" to an Xbox weapon (each custom weapon's model, effects, projectiles "
                           "and animations take memory). "
                           "If that isn't enough, the map needs a simpler BSP or fewer custom objects.";
            r.error += size_breakdown(job);
            return r;
        }
        if (job.from != Game::Unknown) {
            // A converted map's texture or sound that extraction couldn't read: its data is in a resource map we lacked.
            static const std::regex not_found(R"(^Failed to find (.+\.(bitmap|sound))\s*$)");
            std::smatch nm;
            for (auto &l : unwrap(out)) {
                if (!std::regex_match(l, nm, not_found)) continue;
                std::string what = nm[2] == "bitmap" ? "bitmaps.map" : "sounds.map";
                r.error = "The map's " + nm[1].str() + " couldn't be read: its data is kept in a " + what + ", and ";
                if (job.resources_from.empty())
                    r.error += "none was found. Set the " + std::string(job.from == Game::CE ? "Custom Edition folder" : "MCC maps folder") +
                               " in Settings.";
                else
                    r.error += "the one read (" + job.resources_from + ") doesn't have it.";
                if (job.from == Game::MCC && job.resources_from.find("custom_edition") != std::string::npos)
                    r.error += "\nThat's MCC's custom_edition folder, with Custom Edition's files (for classic Custom Edition maps). "
                               "MCC maps use MCC's own: set the MCC maps folder in Settings to ...\\Halo The Master Chief "
                               "Collection\\halo1\\maps.";
                r.error += "\nMods that ship their own bitmaps.map/sounds.map keep them in the mod's halo1\\maps folder: convert the "
                           "map from inside the mod's folder, as Steam installs it, and the app finds them."
                           "\nFull log: " + u8(job.work / "build.log");
                return r;
            }
        }
        if (!parse_failure(out, f)) {
            r.error = "Build failed:\n" + out.substr(out.size() > 3000 ? out.size() - 3000 : 0);
            return r;
        }
        if (!f.script_error.empty()) {
            // MCC's "none" team (no team): the original engine can't parse it.
            std::string what;
            if (!job.extra.empty() && none_fixes < 40 &&
                (fix_script_none(job, f.script_error, what) || fix_script_shadow(job, f.script_error, what))) {
                none_fixes++;
                job.note("[scripts] " + what);
                continue;
            }
            // The map's compiled-in scripts use something the target lacks: convert a copy and retry.
            if (!scripts_tried && job.extra.empty()) {
                scripts_tried = true;
                if (recover_and_convert_scripts(job)) {
                    cmd = make_cmd();
                    continue;
                }
            }
            r.error = friendly_script_error(f.script_error, job.target);
            std::string line = script_error_line(job, f.script_error);
            if (!line.empty()) r.error += "\nThe line: " + line;
            return r;
        }
        if (!f.hsc_error.empty()) {
            // A name in the map's scripts that the built map doesn't have (a nav point arrow its HUD lacks...).
            std::string what;
            if (!job.extra.empty() && none_fixes < 40 && fix_script_navpoint(job, f.hsc_error, what)) {
                none_fixes++;
                job.note("[scripts] " + what);
                continue;
            }
        }
        if (f.tag.empty() || !job.fix(f)) {
            if (!f.hsc_error.empty()) {
                r.error = friendly_script_error(f.hsc_error, job.target);
                std::string line = script_error_line(job, f.hsc_error);
                if (!line.empty()) r.error += "\nThe line: " + line;
                r.error += "\nFull log: " + u8(job.work / "build.log");
                return r;
            }
            r.error = "Build failed and no automatic fix applies.\nError: " + f.msg + "\nTag: " + f.tag +
                      "\nFull log: " + u8(job.work / "build.log");
            return r;
        }
    }
    for (auto &l : unwrap(out))
        if (l.rfind("WARNING", 0) == 0) r.warnings.push_back(l);
    if (!texture_warning.empty()) r.warnings.push_back(texture_warning);
    if (!r.memory.empty()) job.log("  Xbox memory: " + r.memory);
    if (int untyped = untyped_script_nodes(job, tmp_out))
        r.warnings.push_back("WARNING: " + std::to_string(untyped) + " expression" + (untyped == 1 ? "" : "s") +
                             " in the map's scripts compiled without a type; the game can crash when one runs (if the "
                             "map crashes as a match starts, this is the likely reason)");
    if (job.target == Game::Xbox && !stamp_xbox_checksum(tmp_out))
        r.warnings.push_back("WARNING: couldn't write the map's header checksum (the Xbox may keep using an old cached copy)");

    fs::copy_file(tmp_out, output, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        r.error = "Built, but couldn't write " + u8(output) + ": " + ec.message() +
                  "\n(Is the game running with this map loaded?)";
        return r;
    }
    run_process({u8(tool(cfg, "invader-info")), u8(output)}, r.info);
    r.ok = true;
    r.output = output;
    r.changes = job.changes;

    fs::path reports = cfg.cache_dir / "reports";
    fs::create_directories(reports, ec);
    r.report = reports / (stem + " - " + game_name(job.target) + ".txt");
    std::ofstream rep(r.report);
    rep << "Halo Map Porter report\nOutput: " << u8(output) << "\n\n" << r.info << "\n";
    if (!r.memory.empty()) rep << "Xbox memory: " << r.memory << "\n\n";
    rep << "Automatic changes (" << r.changes.size() << "):\n";
    for (auto &c : r.changes) rep << "  " << c << "\n";
    if (r.changes.empty()) rep << "  none\n";
    rep << "\nBuild warnings (" << r.warnings.size() << "):\n";
    for (auto &w : r.warnings) rep << "  " << w << "\n";
    if (!performance.empty()) {
        rep << "\nPerformance (what the Xbox has to draw):\n";
        for (auto &l : performance) rep << "  " << l << "\n";
    }
    return r;
}

// Parse `invader-edit -L` tree output into full key -> value text.
std::vector<std::pair<std::string, std::string>> list_values(const Config &cfg, const fs::path &tags,
                                                             const std::string &tag) {
    std::string out;
    run_process({u8(tool(cfg, "invader-edit")), "-t", u8(tags), tag, "-L"}, out);
    std::vector<std::pair<std::string, std::string>> kv;
    struct Node { std::string name; bool array; };
    std::vector<Node> stack;
    std::istringstream ss(out);
    std::string line;
    while (std::getline(ss, line)) {
        // Column of the label = codepoints of tree-drawing prefix.
        size_t i = 0, col = 0;
        while (i < line.size()) {
            unsigned char c = line[i];
            if (c == ' ') { i++; col++; continue; }
            if (c == 0xE2 && i + 2 < line.size() && (unsigned char)line[i + 1] == 0x94) { i += 3; col++; continue; }
            break;
        }
        if (i >= line.size()) continue;
        std::string rest = line.substr(i);
        size_t gap = rest.find("  ");
        std::string label = rest.substr(0, gap);
        std::string type;
        if (gap != std::string::npos) {
            type = rest.substr(gap);
            type.erase(0, type.find_first_not_of(' '));
        }
        std::string name = label, value;
        size_t paren = label.find(" (");
        if (paren != std::string::npos) {
            name = label.substr(0, paren);
            value = label.substr(paren + 2);
            if (!value.empty() && value.back() == ')') value.pop_back();
            std::replace(value.begin(), value.end(), '\\', '/'); // Windows Invader prints tag paths with '\\'
        }
        size_t depth = col / 4;
        if (stack.size() > depth) stack.resize(depth);
        while (stack.size() < depth) stack.push_back({"", true});
        std::string key;
        for (auto &n : stack)
            if (!n.array && !n.name.empty()) key += n.name + ".";
        key += name;
        kv.push_back({key, value});
        stack.push_back({name, type == "array"});
    }
    return kv;
}

// Copy a folder tree file by file. (std::filesystem::copy with `recursive` fails with
// "File exists" on Windows/MinGW when the destination folder already exists.)
bool copy_tree(const fs::path &from, const fs::path &to, bool overwrite, std::error_code &ec) {
    ec.clear();
    fs::create_directories(to, ec);
    if (ec) return false;
    for (auto it = fs::recursive_directory_iterator(from, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        fs::path dst = to / fs::relative(it->path(), from);
        if (it->is_directory()) {
            fs::create_directories(dst, ec);
        } else if (it->is_regular_file()) {
            if (!overwrite && fs::exists(dst)) continue;
            fs::copy_file(it->path(), dst, fs::copy_options::overwrite_existing, ec);
        }
        if (ec) return false;
    }
    return !ec;
}

// ---------------------------------------------------------------- sounds

uint32_t be32(const std::string &b, size_t o) {
    return (uint32_t)(unsigned char)b[o] << 24 | (uint32_t)(unsigned char)b[o + 1] << 16 |
           (uint32_t)(unsigned char)b[o + 2] << 8 | (uint32_t)(unsigned char)b[o + 3];
}
uint16_t be16(const std::string &b, size_t o) { return (uint16_t)((unsigned char)b[o] << 8 | (unsigned char)b[o + 1]); }

// Script expressions the built map's compiled scripts leave untyped ("passthrough"). Halo's own compiler never
// leaves one, and the game crashes running some of them: a comparison whose first operand is untyped crashed
// Deathtrap on the Xbox as its first tick ran. Returns how many there are (0 = none, or no scripts).
int untyped_script_nodes(const Job &job, const fs::path &map) {
    fs::path dir = job.work / "script_check";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    std::string out;
    run_process({u8(tool(job.cfg, "invader-extract")), "-G", "-n", "-t", u8(dir), "-s", "*.scenario", u8(map)}, out);
    int n = 0;
    for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->path().extension() != ".scenario") continue;
        std::ifstream f(it->path(), std::ios::binary);
        std::string d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        size_t h = d.find(std::string("script node\0", 12));   // the compiled script's node array (big endian in a tag)
        if (h == std::string::npos || h + 0x38 > d.size()) continue;
        unsigned count = be16(d, h + 0x2e);
        for (unsigned k = 0; k < count && h + 0x38 + 20 * (size_t)(k + 1) <= d.size(); k++)
            if (be16(d, h + 0x38 + 20 * (size_t)k + 4) == 3) n++;   // node type 3: passthrough
    }
    fs::remove_all(dir, ec);
    return n;
}

struct SndPerm { std::string name; int format; int next; std::string data; };
struct SndRange { std::string name; int actual; std::vector<SndPerm> perms; };
struct SndTag { int channels; int rate; std::vector<SndRange> ranges; int cls = 0; };

// Reads a HEK .sound tag (big endian). Layout: 64-byte header, Sound (164), promotion
// dependency path, pitch ranges (72 each), then per range: permutations (124 each) and their data.
bool read_sound_tag(const fs::path &p, SndTag &t) {
    std::ifstream f(p, std::ios::binary);
    std::string b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 64 + 164 || b.compare(36, 4, "snd!") != 0) return false;
    size_t o = 64;
    t.channels = be16(b, o + 108) == 1 ? 2 : 1;
    t.rate = be16(b, o + 6) == 1 ? 44100 : 22050;
    t.cls = be16(b, o + 4);
    uint32_t dep_len = be32(b, o + 112 + 8), pr_count = be32(b, o + 152);
    o += 164;
    if (dep_len) o += dep_len + 1;
    size_t pr_base = o;
    o += 72 * (size_t)pr_count;
    for (uint32_t i = 0; i < pr_count; i++) {
        size_t P = pr_base + 72 * i;
        if (P + 72 > b.size()) return false;
        SndRange r;
        r.name = std::string(b.data() + P, strnlen(b.data() + P, 32));
        r.actual = be16(b, P + 44);
        uint32_t pc = be32(b, P + 60);
        size_t pbase = o;
        o += 124 * (size_t)pc;
        for (uint32_t j = 0; j < pc; j++) {
            size_t Q = pbase + 124 * j;
            if (Q + 124 > b.size()) return false;
            SndPerm pm;
            pm.name = std::string(b.data() + Q, strnlen(b.data() + Q, 32));
            pm.format = be16(b, Q + 40);
            pm.next = (int16_t)be16(b, Q + 42);
            uint32_t sz[3] = {be32(b, Q + 64), be32(b, Q + 84), be32(b, Q + 104)};
            if (o + sz[0] + sz[1] + sz[2] > b.size()) return false;
            pm.data = b.substr(o, sz[0]);
            o += (size_t)sz[0] + sz[1] + sz[2];
            r.perms.push_back(std::move(pm));
        }
        t.ranges.push_back(std::move(r));
    }
    return o == b.size();
}

bool write_wav(const fs::path &p, int channels, int rate, const std::vector<int16_t> &pcm) {
    std::ofstream f(p, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write((const char *)&v, 4); };
    auto u16 = [&](uint16_t v) { f.write((const char *)&v, 2); };
    uint32_t bytes = (uint32_t)(pcm.size() * 2);
    f.write("RIFF", 4); u32(36 + bytes); f.write("WAVEfmt ", 8); u32(16); u16(1); u16((uint16_t)channels);
    u32((uint32_t)rate); u32((uint32_t)rate * channels * 2); u16((uint16_t)(channels * 2)); u16(16);
    f.write("data", 4); u32(bytes);
    f.write((const char *)pcm.data(), bytes);
    return (bool)f;
}

// Xbox ADPCM (IMA ADPCM, 36-byte blocks per channel: 4-byte header with the first sample and step
// index, then 32 bytes of 4-bit codes interleaved per channel in 4-byte chunks). Same as Invader's reader:
// the header sample plus 63 codes = 64 samples per channel per block.
void decode_xbox_adpcm(const std::string &d, int ch, std::vector<int16_t> &pcm) {
    static const int index_table[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
    static const int step_table[89] = {
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
        107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
        876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
        4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
        22385, 24623, 27086, 29794, 32767};
    if (ch < 1 || ch > 2) return;
    size_t block = 36 * (size_t)ch;
    for (size_t b = 0; b + block <= d.size(); b += block) {
        const unsigned char *p = (const unsigned char *)d.data() + b;
        int sample[2], index[2];
        std::vector<int16_t> out[2];
        for (int c = 0; c < ch; c++) {
            sample[c] = (int16_t)(p[4 * c] | (p[4 * c + 1] << 8));
            index[c] = std::min(88, std::max(0, (int)p[4 * c + 2]));
            out[c].push_back((int16_t)sample[c]);
        }
        const unsigned char *codes = p + 4 * ch;
        for (int chunk = 0; chunk < 8; chunk++)
            for (int c = 0; c < ch; c++) {
                const unsigned char *q = codes + (chunk * ch + c) * 4;
                for (int k = 0; k < 8 && out[c].size() < 64; k++) {
                    int code = (q[k / 2] >> ((k & 1) * 4)) & 15, step = step_table[index[c]];
                    int delta = step >> 3;
                    if (code & 4) delta += step;
                    if (code & 2) delta += step >> 1;
                    if (code & 1) delta += step >> 2;
                    if (code & 8) delta = -delta;
                    sample[c] = std::max(-32768, std::min(32767, sample[c] + delta));
                    index[c] = std::min(88, std::max(0, index[c] + index_table[code]));
                    out[c].push_back((int16_t)sample[c]);
                }
            }
        for (size_t i = 0; i < out[0].size(); i++)
            for (int c = 0; c < ch; c++) pcm.push_back(out[c][i]);
    }
}

// Decode one permutation chain (split sounds are chained via next index) to PCM.
bool decode_chain(const SndTag &t, const SndRange &r, int first, std::vector<int16_t> &pcm, int &ch, int &rate) {
    ch = t.channels; rate = t.rate;
    std::set<int> seen;
    for (int i = first; i >= 0 && i < (int)r.perms.size() && !seen.count(i); i = r.perms[i].next) {
        seen.insert(i);
        const SndPerm &pm = r.perms[i];
        if (pm.format == 3) { // Ogg Vorbis
            int c = 0, sr = 0;
            short *out = nullptr;
            int n = stb_vorbis_decode_memory((const unsigned char *)pm.data.data(), (int)pm.data.size(), &c, &sr, &out);
            if (n < 0 || !out) return false;
            pcm.insert(pcm.end(), out, out + (size_t)n * c);
            free(out);
            ch = c; rate = sr;
        } else if (pm.format == 0) { // 16-bit PCM, big endian
            for (size_t k = 0; k + 1 < pm.data.size(); k += 2)
                pcm.push_back((int16_t)be16(pm.data, k));
        } else if (pm.format == 1) { // Xbox ADPCM (re-encoded only to split it into smaller pieces)
            decode_xbox_adpcm(pm.data, t.channels, pcm);
        } else {
            return false;
        }
    }
    return true;
}

// Dialogue classes (unit dialog, scripted dialog player/other/force unspatialized): the engine plays each line as
// one permutation (mouth data and timing go with it), so these can't be split into chained pieces.
bool sound_is_dialogue(const SndTag &t) { return t.cls == 19 || t.cls == 44 || t.cls == 46 || t.cls == 47; }

// A piece longer than the 227.5 KiB the original Halo engine can load at once.
const size_t XBOX_MAX_SOUND_PIECE = 232960;
bool sound_has_long_piece(const fs::path &p) {
    SndTag t;
    if (!read_sound_tag(p, t)) return false;
    for (auto &r : t.ranges)
        for (auto &pm : r.perms)
            if (pm.data.size() > XBOX_MAX_SOUND_PIECE) return true;
    return false;
}

// Re-encode a sound tag (in `tags`) as Xbox ADPCM. Uses the user's WAVs when available.
bool reencode_sound_for_xbox(Job &job, const fs::path &tags, const std::string &rel, std::string &why) {
    const Config &cfg = job.cfg;
    std::string noext = rel.substr(0, rel.size() - 6);
    std::string out;
    SndTag t;
    if (!read_sound_tag(tags / from_u8(rel), t)) { why = "couldn't read the sound tag"; return false; }
    // The Xbox has no channel for mono 44.1 kHz sounds: those become 22.05 kHz.
    std::vector<std::string> rate;
    if (t.channels == 1 && t.rate == 44100) rate = {"-r", "22050"};
    // -s: split into 227.5 KiB pieces; the original Halo engine crashes on longer unsplit sounds. invader-sound
    // refuses to split dialogue, so dialogue stays whole (a line longer than that gets a note from the caller).
    std::vector<std::string> split;
    if (!sound_is_dialogue(t)) split = {"-s"};
    if (!job.user_data.empty() && fs::is_directory(job.user_data / from_u8(noext))) {
        std::vector<std::string> cmd{u8(tool(cfg, "invader-sound")), "-t", u8(tags), "-d", u8(job.user_data), "-F",
                                     "xbox_adpcm"};
        cmd.insert(cmd.end(), split.begin(), split.end());
        cmd.insert(cmd.end(), rate.begin(), rate.end());
        cmd.push_back(noext);
        if (run_process(cmd, out) == 0) return true;
    }
    fs::path data = job.work / "sound_data";
    fs::path dir = data / from_u8(noext);
    std::error_code ec;
    fs::remove_all(dir, ec);
    std::set<std::string> range_names;
    for (size_t i = 0; i < t.ranges.size(); i++) {
        SndRange &r = t.ranges[i];
        std::string rn = r.name;
        if (rn.empty() || range_names.count(rn)) {
            rn = "range" + std::to_string(i);
            run_process({u8(tool(cfg, "invader-edit")), "-t", u8(tags), rel, "-S",
                         "pitch_ranges[" + std::to_string(i) + "].name", rn}, out);
        }
        range_names.insert(rn);
        fs::create_directories(dir / from_u8(rn), ec);
        std::set<std::string> names;
        int actual = r.actual > 0 ? std::min<int>(r.actual, (int)r.perms.size()) : (int)r.perms.size();
        for (int j = 0; j < actual; j++) {
            std::vector<int16_t> pcm;
            int ch, rate;
            if (!decode_chain(t, r, j, pcm, ch, rate)) { why = "unsupported sample format"; return false; }
            std::string pn = r.perms[j].name.empty() ? "perm" + std::to_string(j) : r.perms[j].name;
            while (names.count(pn)) pn += "_";
            names.insert(pn);
            write_wav(dir / from_u8(rn) / from_u8(pn + ".wav"), ch, rate, pcm);
        }
    }
    std::vector<std::string> cmd{u8(tool(cfg, "invader-sound")), "-t", u8(tags), "-d", u8(data), "-F", "xbox_adpcm"};
    cmd.insert(cmd.end(), split.begin(), split.end());
    cmd.insert(cmd.end(), rate.begin(), rate.end());
    cmd.push_back(noext);
    if (run_process(cmd, out) != 0) {
        why = "invader-sound failed: " + out.substr(out.size() > 400 ? out.size() - 400 : 0);
        return false;
    }
    return true;
}

// Sounds the Xbox can't play as they are. Its sound channels are all Xbox ADPCM ones: mono 22 kHz (2D and 3D),
// stereo 22 kHz and stereo 44 kHz. So: Ogg Vorbis and uncompressed 16-bit PCM (MCC and Halo PC have channels
// for both; on the Xbox they're silent), mono 44.1 kHz (no channel either), and a piece longer than the
// 227.5 KiB the original Halo engine can load at once (MCC doesn't split long sounds; playing one crashes the Xbox).
std::string sound_reencode_reason(const fs::path &p) {
    SndTag t;
    if (!read_sound_tag(p, t)) return "";
    std::string why;
    for (auto &r : t.ranges)
        for (auto &pm : r.perms) {
            if (pm.format == 3) return "Ogg Vorbis";
            if (pm.format == 0) why = "uncompressed";
            // (dialogue can't be split, so re-encoding wouldn't shorten it)
            if (why.empty() && pm.format == 1 && pm.data.size() > XBOX_MAX_SOUND_PIECE && !sound_is_dialogue(t))
                why = "too long to load at once";
        }
    if (why.empty() && t.channels == 1 && t.rate == 44100) why = "mono 44.1 kHz";
    return why;
}
bool sound_needs_reencode(const fs::path &p) { return !sound_reencode_reason(p).empty(); }

// ---------------------------------------------------------------- textures

void put_be32(std::string &b, size_t o, uint32_t v) {
    b[o] = (char)(v >> 24); b[o + 1] = (char)(v >> 16); b[o + 2] = (char)(v >> 8); b[o + 3] = (char)v;
}
void put_be16(std::string &b, size_t o, uint16_t v) { b[o] = (char)(v >> 8); b[o + 1] = (char)v; }

uint32_t crc32_of(const char *d, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ (unsigned char)d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// Bytes for one mip level; 0 if the format isn't handled.
size_t mip_bytes(int w, int h, int fmt) {
    switch (fmt) {
    case 14: return (size_t)std::max(1, (w + 3) / 4) * std::max(1, (h + 3) / 4) * 8;   // DXT1
    case 15: case 16: case 18: return (size_t)std::max(1, (w + 3) / 4) * std::max(1, (h + 3) / 4) * 16; // DXT3/5, BC7
    case 0: case 1: case 2: case 17: return (size_t)w * h;      // A8, Y8, AY8, P8
    case 3: case 6: case 8: case 9: return (size_t)w * h * 2;   // A8Y8, R5G6B5, A1R5G5B5, A4R4G4B4
    case 10: case 11: return (size_t)w * h * 4;                 // X8R8G8B8, A8R8G8B8
    default: return 0;
    }
}

// DXT and BC7 store 4x4-pixel blocks, so a texture in one of them has to stay a multiple of 4 pixels on
// each side. One that isn't is outside what stock maps contain (Invader calls such a tag corrupt), e.g.
// Precipice's 1024x4 DXT3 sky lightning strip halved to 512x2; such textures are left at their size.
static bool block_format(int fmt) { return fmt == 14 || fmt == 15 || fmt == 16 || fmt == 18; }
static bool block_halvable(int w, int h) { return w >= 8 && h >= 8 && (w / 2) % 4 == 0 && (h / 2) % 4 == 0; }

// Halve a .bitmap tag's 2D textures by dropping their largest mip level.
// Layout: 64-byte header, Bitmap (108), color plate data, pixel data, sequences (64 each),
// their sprites (32 each), bitmap data entries (48 each). Returns bytes saved (0 = unchanged).
size_t shrink_bitmap_tag(const fs::path &p, bool write) {
    std::string b;
    {
        std::ifstream f(p, std::ios::binary);
        b.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    const size_t S = 64;
    if (b.size() < S + 108 || b.compare(36, 4, "bitm") != 0) return 0;
    if (be16(b, S) != 0) return 0; // only "2d textures" (skip sprites, interface, cube maps, 3D)
    uint32_t cp = be32(b, S + 28), pp = be32(b, S + 48), nseq = be32(b, S + 84), nbd = be32(b, S + 96);
    size_t pp_off = S + 108 + (size_t)cp, seq_off = pp_off + pp, o = seq_off + 64 * (size_t)nseq;
    if (o > b.size()) return 0;
    for (uint32_t i = 0; i < nseq; i++) o += 32 * (size_t)be32(b, seq_off + 64 * i + 52);
    size_t bd_off = o;
    if (bd_off + 48 * (size_t)nbd != b.size()) return 0;

    std::string pix = b.substr(pp_off, pp), out_pix;
    size_t saved = 0;
    for (uint32_t i = 0; i < nbd; i++) {
        size_t e = bd_off + 48 * i;
        int w = be16(b, e + 4), h = be16(b, e + 6), type = be16(b, e + 10), fmt = be16(b, e + 12), flags = be16(b, e + 14);
        int mips = be16(b, e + 20);
        uint32_t off = be32(b, e + 24), sz = be32(b, e + 28);
        size_t full = 0;
        if (type == 0 && mip_bytes(4, 4, fmt))
            for (int k = 0; k <= mips; k++) full += mip_bytes(std::max(1, w >> k), std::max(1, h >> k), fmt);
        if (full && (sz == 0 || sz > full)) sz = (uint32_t)full;
        if (off + (size_t)sz > pix.size()) return 0;
        std::string data = pix.substr(off, sz);
        bool pow2 = flags & 1, swizzled = flags & 8, linear = flags & 16;
        if (full && pow2 && !swizzled && !linear && mips >= 1 && w >= 64 && h >= 64) {
            size_t m0 = mip_bytes(w, h, fmt);
            if (m0 < data.size()) {
                data.erase(0, m0);
                saved += m0;
                put_be16(b, e + 4, (uint16_t)(w / 2));
                put_be16(b, e + 6, (uint16_t)(h / 2));
                put_be16(b, e + 20, (uint16_t)(mips - 1));
            }
        }
        put_be32(b, e + 24, (uint32_t)out_pix.size());
        put_be32(b, e + 28, (uint32_t)data.size());
        out_pix += data;
    }
    if (!saved || !write) return saved;
    std::string out = b.substr(0, pp_off) + out_pix + b.substr(pp_off + pp);
    put_be32(out, S + 48, (uint32_t)out_pix.size());
    put_be32(out, 40, ~crc32_of(out.data() + 64, out.size() - 64)); // HEK tag checksum
    std::ofstream(p, std::ios::binary) << out;
    return saved;
}

// Halve the map's own (non-stock) textures, largest first, until `needed` bytes are saved.
size_t shrink_textures_to_fit(Job &job, size_t needed, int &count);

// MCC (H1A-EK) can store textures as BC7, which Xbox and Custom Edition don't support. Both BC7 and
// DXT store 4x4-pixel blocks, so each block is decoded and re-encoded on its own: as DXT5, or as DXT1
// (half the size) when the texture has no transparency. Returns how many bitmaps were converted
// (`write` false = just count), or -1 if the tag couldn't be read.
int convert_bc7_bitmap_tag(const fs::path &p, bool write) {
    std::string b;
    {
        std::ifstream f(p, std::ios::binary);
        b.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    const size_t S = 64;
    if (b.size() < S + 108 || b.compare(36, 4, "bitm") != 0) return -1;
    uint32_t cp = be32(b, S + 28), pp = be32(b, S + 48), nseq = be32(b, S + 84), nbd = be32(b, S + 96);
    size_t pp_off = S + 108 + (size_t)cp, seq_off = pp_off + pp, o = seq_off + 64 * (size_t)nseq;
    if (o > b.size()) return -1;
    for (uint32_t i = 0; i < nseq; i++) o += 32 * (size_t)be32(b, seq_off + 64 * i + 52);
    size_t bd_off = o;
    if (bd_off + 48 * (size_t)nbd != b.size()) return -1;
    int bc7 = 0;
    for (uint32_t i = 0; i < nbd; i++) bc7 += be16(b, bd_off + 48 * i + 12) == 18;
    if (!bc7 || !write) return bc7;

    // Each entry's data runs from its offset to the next entry's (extracted tags store size 0).
    std::string pix = b.substr(pp_off, pp), out_pix;
    std::vector<uint32_t> offs;
    for (uint32_t i = 0; i < nbd; i++) offs.push_back(be32(b, bd_off + 48 * i + 24));
    bool any_alpha = false;
    for (uint32_t i = 0; i < nbd; i++) {
        size_t e = bd_off + 48 * i;
        uint32_t off = offs[i], end = pp;
        for (auto x : offs)
            if (x > off && x < end) end = x;
        if (off > pix.size() || end > pix.size()) return -1;
        std::string data = pix.substr(off, end - off);
        if (be16(b, e + 12) == 18) {
            if (data.size() % 16) return -1;
            size_t blocks = data.size() / 16;
            std::vector<unsigned char> rgba(blocks * 64);
            bool alpha = false;
            for (size_t k = 0; k < blocks; k++) {
                bcdec_bc7(data.data() + 16 * k, rgba.data() + 64 * k, 16);
                for (int t = 3; t < 64 && !alpha; t += 4) alpha = rgba[64 * k + t] != 255;
            }
            std::string enc(blocks * (alpha ? 16 : 8), '\0');
            for (size_t k = 0; k < blocks; k++)
                stb_compress_dxt_block((unsigned char *)&enc[k * (alpha ? 16 : 8)], rgba.data() + 64 * k, alpha ? 1 : 0,
                                       STB_DXT_HIGHQUAL);
            data = enc;
            put_be16(b, e + 12, alpha ? 16 : 14); // DXT5 : DXT1
            any_alpha = any_alpha || alpha;
        }
        put_be32(b, e + 24, (uint32_t)out_pix.size());
        put_be32(b, e + 28, (uint32_t)data.size());
        out_pix += data;
    }
    if (be16(b, S + 2) == 6) put_be16(b, S + 2, any_alpha ? 2 : 0); // tag-level format: BC7 -> DXT5/DXT1
    std::string out = b.substr(0, pp_off) + out_pix + b.substr(pp_off + pp);
    put_be32(out, S + 48, (uint32_t)out_pix.size());
    put_be32(out, 40, ~crc32_of(out.data() + 64, out.size() - 64));
    std::ofstream(p, std::ios::binary) << out;
    return bc7;
}

// Halve an image (2x2 box filter) in one of Halo's pixel formats. Pixel data is little endian (as the
// GPU reads it). DXT1 with transparent pixels comes back as DXT5 (`fmt` is updated). False = unsupported.
bool halve_pixels(const std::string &d, int w, int h, int &fmt, std::string &out) {
    int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
    std::vector<unsigned char> rgba((size_t)w * h * 4), half((size_t)nw * nh * 4);
    bool dxt = fmt == 14 || fmt == 15 || fmt == 16;
    if (dxt) {
        if (w % 4 || h % 4 || nw % 4 || nh % 4) return false;
        int bs = fmt == 14 ? 8 : 16, bw = w / 4, bh = h / 4;
        if (d.size() < (size_t)bw * bh * bs) return false;
        for (int by = 0; by < bh; by++)
            for (int bx = 0; bx < bw; bx++) {
                const char *src = d.data() + ((size_t)by * bw + bx) * bs;
                unsigned char *dst = &rgba[((size_t)by * 4 * w + bx * 4) * 4];
                if (fmt == 14) bcdec_bc1(src, dst, w * 4);
                else if (fmt == 15) bcdec_bc2(src, dst, w * 4);
                else bcdec_bc3(src, dst, w * 4);
            }
    } else {
        int bpp = fmt <= 2 ? 1 : (fmt == 3 || fmt == 6 || fmt == 8 || fmt == 9) ? 2 : (fmt == 10 || fmt == 11) ? 4 : 0;
        if (!bpp || d.size() < (size_t)w * h * bpp) return false;
        const unsigned char *p = (const unsigned char *)d.data();
        for (size_t i = 0; i < (size_t)w * h; i++) {
            unsigned char *q = &rgba[i * 4];
            unsigned v = bpp == 2 ? p[i * 2] | (p[i * 2 + 1] << 8) : 0;
            switch (fmt) {
            case 0: case 1: case 2: q[0] = p[i]; break;                      // A8 / Y8 / AY8: one channel
            case 3: q[0] = p[i * 2]; q[1] = p[i * 2 + 1]; break;             // A8Y8: two byte channels
            case 6: q[0] = (v >> 11) * 255 / 31; q[1] = ((v >> 5) & 63) * 255 / 63; q[2] = (v & 31) * 255 / 31; break;
            case 8: q[3] = (v >> 15) * 255; q[0] = ((v >> 10) & 31) * 255 / 31; q[1] = ((v >> 5) & 31) * 255 / 31;
                    q[2] = (v & 31) * 255 / 31; break;
            case 9: q[3] = (v >> 12) * 17; q[0] = ((v >> 8) & 15) * 17; q[1] = ((v >> 4) & 15) * 17; q[2] = (v & 15) * 17; break;
            default: memcpy(q, p + i * 4, 4); break;                          // X8R8G8B8 / A8R8G8B8 (BGRA bytes)
            }
        }
    }
    for (int y = 0; y < nh; y++)
        for (int x = 0; x < nw; x++)
            for (int c = 0; c < 4; c++) {
                int sx = std::min(w - 1, x * 2), sy = std::min(h - 1, y * 2), sx1 = std::min(w - 1, sx + 1), sy1 = std::min(h - 1, sy + 1);
                int sum = rgba[((size_t)sy * w + sx) * 4 + c] + rgba[((size_t)sy * w + sx1) * 4 + c] +
                          rgba[((size_t)sy1 * w + sx) * 4 + c] + rgba[((size_t)sy1 * w + sx1) * 4 + c];
                half[((size_t)y * nw + x) * 4 + c] = (unsigned char)((sum + 2) / 4);
            }
    out.clear();
    if (dxt) {
        bool alpha = fmt != 14;
        if (fmt == 14) // DXT1 with cut-out transparency: keep it, as DXT5
            for (size_t i = 3; i < half.size() && !alpha; i += 4) alpha = half[i] < 128;
        if (fmt == 14 && alpha) fmt = 16;
        for (int by = 0; by < nh / 4; by++)
            for (int bx = 0; bx < nw / 4; bx++) {
                unsigned char blk[64], enc[16];
                for (int r = 0; r < 4; r++) memcpy(blk + r * 16, &half[((size_t)(by * 4 + r) * nw + bx * 4) * 4], 16);
                stb_compress_dxt_block(enc, blk, alpha ? 1 : 0, STB_DXT_HIGHQUAL);
                if (fmt == 15) { // DXT3: explicit 4-bit alpha + the color block
                    unsigned char a[8] = {};
                    for (int k = 0; k < 16; k++) a[k / 2] |= (unsigned char)(((blk[k * 4 + 3] * 15 + 127) / 255) << ((k & 1) * 4));
                    out.append((const char *)a, 8);
                    out.append((const char *)enc + 8, 8);
                } else {
                    out.append((const char *)enc, alpha ? 16 : 8);
                }
            }
        return true;
    }
    for (size_t i = 0; i < (size_t)nw * nh; i++) {
        const unsigned char *q = &half[i * 4];
        unsigned v;
        switch (fmt) {
        case 0: case 1: case 2: out += (char)q[0]; break;
        case 3: out += (char)q[0]; out += (char)q[1]; break;
        case 6: v = ((q[0] * 31 + 127) / 255) << 11 | ((q[1] * 63 + 127) / 255) << 5 | ((q[2] * 31 + 127) / 255);
                out += (char)(v & 255); out += (char)(v >> 8); break;
        case 8: v = (q[3] >= 128 ? 1u : 0u) << 15 | ((q[0] * 31 + 127) / 255) << 10 | ((q[1] * 31 + 127) / 255) << 5 |
                    ((q[2] * 31 + 127) / 255);
                out += (char)(v & 255); out += (char)(v >> 8); break;
        case 9: v = ((q[3] + 8) / 17) << 12 | ((q[0] + 8) / 17) << 8 | ((q[1] + 8) / 17) << 4 | ((q[2] + 8) / 17);
                out += (char)(v & 255); out += (char)(v >> 8); break;
        default: out.append((const char *)q, 4); break;
        }
    }
    return true;
}

// HUD meters fill up by comparing each pixel's gradient value with the meter's value. PC meter bitmaps
// keep that gradient in the alpha channel and the meter's shape in the color channels; the Xbox meter
// shader reads it the other way round (gradient from color, shape from alpha, as in the Xbox's own
// meter sheets). Rewrite a PC meter bitmap with the channels swapped. A8R8G8B8, A8Y8 and A4R4G4B4 are
// swapped in place; DXT3/DXT5 are decoded and stored as A8R8G8B8. Returns how many bitmaps were
// converted (0 = none in a format that carries a gradient, -1 = unreadable).
int xbox_meter_bitmap_tag(const fs::path &p) {
    std::string b;
    {
        std::ifstream f(p, std::ios::binary);
        b.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    const size_t S = 64;
    if (b.size() < S + 108 || b.compare(36, 4, "bitm") != 0) return -1;
    uint32_t cp = be32(b, S + 28), pp = be32(b, S + 48), nseq = be32(b, S + 84), nbd = be32(b, S + 96);
    size_t pp_off = S + 108 + (size_t)cp, seq_off = pp_off + pp, o = seq_off + 64 * (size_t)nseq;
    if (pp_off + pp > b.size() || o > b.size()) return -1;
    for (uint32_t i = 0; i < nseq; i++) o += 32 * (size_t)be32(b, seq_off + 64 * i + 52);
    size_t bd_off = o;
    if (bd_off + 48 * (size_t)nbd != b.size()) return -1;

    std::string pix = b.substr(pp_off, pp), out_pix;
    std::vector<uint32_t> offs;
    for (uint32_t i = 0; i < nbd; i++) offs.push_back(be32(b, bd_off + 48 * i + 24));
    int converted = 0;
    bool to_32bit = false;
    auto swap_rgba = [](unsigned char *q) { // q = R,G,B,A
        unsigned char shape = std::max(q[0], std::max(q[1], q[2])), grad = q[3];
        q[0] = q[1] = q[2] = grad;
        q[3] = shape;
    };
    for (uint32_t i = 0; i < nbd; i++) {
        size_t e = bd_off + 48 * i;
        // Each entry's data runs from its offset to the next entry's (extracted tags store size 0).
        uint32_t off = offs[i], end = pp;
        for (auto x : offs)
            if (x > off && x < end) end = x;
        if (off > pix.size() || end > pix.size()) return -1;
        std::string data = pix.substr(off, end - off);
        int w = be16(b, e + 4), h = be16(b, e + 6), type = be16(b, e + 10), fmt = be16(b, e + 12), flags = be16(b, e + 14);
        int mips = be16(b, e + 20);
        bool swizzled = flags & 8;
        unsigned char *d = (unsigned char *)&data[0];
        if (swizzled || type != 0) {
            // leave as is
        } else if (fmt == 11) {                                   // A8R8G8B8: B,G,R,A bytes
            for (size_t k = 0; k + 4 <= data.size(); k += 4) {
                unsigned char q[4] = {d[k + 2], d[k + 1], d[k], d[k + 3]};
                swap_rgba(q);
                d[k] = q[2]; d[k + 1] = q[1]; d[k + 2] = q[0]; d[k + 3] = q[3];
            }
            converted++;
        } else if (fmt == 3) {                                    // A8Y8: Y,A bytes
            for (size_t k = 0; k + 2 <= data.size(); k += 2) std::swap(d[k], d[k + 1]);
            converted++;
        } else if (fmt == 9) {                                    // A4R4G4B4
            for (size_t k = 0; k + 2 <= data.size(); k += 2) {
                unsigned v = d[k] | d[k + 1] << 8;
                unsigned a = v >> 12, r = (v >> 8) & 15, g = (v >> 4) & 15, bl = v & 15;
                unsigned shape = std::max(r, std::max(g, bl));
                v = shape << 12 | a << 8 | a << 4 | a;
                d[k] = (unsigned char)(v & 255); d[k + 1] = (unsigned char)(v >> 8);
            }
            converted++;
        } else if ((fmt == 15 || fmt == 16) && w % 4 == 0 && h % 4 == 0) {   // DXT3 / DXT5 -> A8R8G8B8
            std::string enc;
            size_t pos = 0;
            bool ok = true;
            for (int k = 0; k <= mips && ok; k++) {
                int mw = std::max(1, w >> k), mh = std::max(1, h >> k);
                int bw = std::max(1, (mw + 3) / 4), bh = std::max(1, (mh + 3) / 4);
                if (pos + (size_t)bw * bh * 16 > data.size()) { ok = false; break; }
                std::vector<unsigned char> rgba((size_t)bw * 4 * bh * 4 * 4);
                for (int by = 0; by < bh; by++)
                    for (int bx = 0; bx < bw; bx++) {
                        const char *src = data.data() + pos + ((size_t)by * bw + bx) * 16;
                        unsigned char *dst = &rgba[((size_t)by * 4 * bw * 4 + bx * 4) * 4];
                        if (fmt == 15) bcdec_bc2(src, dst, bw * 4 * 4);
                        else bcdec_bc3(src, dst, bw * 4 * 4);
                    }
                pos += (size_t)bw * bh * 16;
                for (int y = 0; y < mh; y++)
                    for (int x = 0; x < mw; x++) {
                        unsigned char *q = &rgba[((size_t)y * bw * 4 + x) * 4];
                        swap_rgba(q);
                        char px[4] = {(char)q[2], (char)q[1], (char)q[0], (char)q[3]};
                        enc.append(px, 4);
                    }
            }
            if (ok) {
                data = enc;
                put_be16(b, e + 12, 11);
                put_be16(b, e + 14, flags & ~2);   // no longer compressed
                to_32bit = true;
                converted++;
            }
        }
        put_be32(b, e + 24, (uint32_t)out_pix.size());
        put_be32(b, e + 28, (uint32_t)data.size());
        out_pix += data;
    }
    if (!converted) return 0;
    if (to_32bit) put_be16(b, S + 2, 4); // tag-level format: 32-bit color
    std::string out = b.substr(0, pp_off) + out_pix + b.substr(pp_off + pp);
    put_be32(out, S + 48, (uint32_t)out_pix.size());
    put_be32(out, 40, ~crc32_of(out.data() + 64, out.size() - 64)); // HEK tag checksum
    std::ofstream(p, std::ios::binary) << out;
    return converted;
}

// DXT textures whose sides aren't multiples of 4 pixels (see block_format) stored as A8R8G8B8 instead,
// cropped from the decoded blocks, mip levels kept. Returns how many textures need it (`write` false =
// just count) or did, -1 if the tag can't be read.
int unblock_unaligned_dxt_bitmap_tag(const fs::path &p, bool write) {
    std::string b;
    {
        std::ifstream f(p, std::ios::binary);
        b.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    const size_t S = 64;
    if (b.size() < S + 108 || b.compare(36, 4, "bitm") != 0) return -1;
    uint32_t cp = be32(b, S + 28), pp = be32(b, S + 48), nseq = be32(b, S + 84), nbd = be32(b, S + 96);
    size_t pp_off = S + 108 + (size_t)cp, seq_off = pp_off + pp, o = seq_off + 64 * (size_t)nseq;
    if (pp_off + pp > b.size() || o > b.size()) return -1;
    for (uint32_t i = 0; i < nseq; i++) o += 32 * (size_t)be32(b, seq_off + 64 * i + 52);
    size_t bd_off = o;
    if (bd_off + 48 * (size_t)nbd != b.size()) return -1;
    auto unaligned = [&](size_t e) {
        int w = be16(b, e + 4), h = be16(b, e + 6), type = be16(b, e + 10), fmt = be16(b, e + 12), flags = be16(b, e + 14);
        return type == 0 && !(flags & 8) && (fmt == 14 || fmt == 15 || fmt == 16) && (w % 4 || h % 4);
    };
    int n = 0;
    for (uint32_t i = 0; i < nbd; i++) n += unaligned(bd_off + 48 * i);
    if (!n || !write) return n;

    std::string pix = b.substr(pp_off, pp), out_pix;
    std::vector<uint32_t> offs;
    for (uint32_t i = 0; i < nbd; i++) offs.push_back(be32(b, bd_off + 48 * i + 24));
    int done = 0;
    for (uint32_t i = 0; i < nbd; i++) {
        size_t e = bd_off + 48 * i;
        uint32_t off = offs[i], end = pp;   // each entry's data runs to the next entry's (extracted tags store size 0)
        for (auto x : offs)
            if (x > off && x < end) end = x;
        if (off > pix.size() || end > pix.size()) return -1;
        std::string data = pix.substr(off, end - off);
        if (unaligned(e)) {
            int w = be16(b, e + 4), h = be16(b, e + 6), fmt = be16(b, e + 12), flags = be16(b, e + 14), mips = be16(b, e + 20);
            int bs = fmt == 14 ? 8 : 16;
            std::string enc;
            size_t pos = 0;
            bool ok = true;
            for (int k = 0; k <= mips && ok; k++) {
                int mw = std::max(1, w >> k), mh = std::max(1, h >> k);
                int bw = (mw + 3) / 4, bh = (mh + 3) / 4;
                if (pos + (size_t)bw * bh * bs > data.size()) { ok = false; break; }
                std::vector<unsigned char> rgba((size_t)bw * 4 * bh * 4 * 4);
                for (int by = 0; by < bh; by++)
                    for (int bx = 0; bx < bw; bx++) {
                        const char *src = data.data() + pos + ((size_t)by * bw + bx) * bs;
                        unsigned char *dst = &rgba[((size_t)by * 4 * bw * 4 + bx * 4) * 4];
                        if (fmt == 14) bcdec_bc1(src, dst, bw * 4 * 4);
                        else if (fmt == 15) bcdec_bc2(src, dst, bw * 4 * 4);
                        else bcdec_bc3(src, dst, bw * 4 * 4);
                    }
                pos += (size_t)bw * bh * bs;
                for (int y = 0; y < mh; y++)
                    for (int x = 0; x < mw; x++) {
                        const unsigned char *q = &rgba[((size_t)y * bw * 4 + x) * 4];
                        char px[4] = {(char)q[2], (char)q[1], (char)q[0], (char)q[3]};   // B,G,R,A
                        enc.append(px, 4);
                    }
            }
            if (ok) {
                data = enc;
                put_be16(b, e + 12, 11);
                put_be16(b, e + 14, flags & ~2);   // no longer compressed
                done++;
            }
        }
        put_be32(b, e + 24, (uint32_t)out_pix.size());
        put_be32(b, e + 28, (uint32_t)data.size());
        out_pix += data;
    }
    if (!done) return 0;
    std::string out = b.substr(0, pp_off) + out_pix + b.substr(pp_off + pp);
    put_be32(out, S + 48, (uint32_t)out_pix.size());
    put_be32(out, 40, ~crc32_of(out.data() + 64, out.size() - 64));
    std::ofstream(p, std::ios::binary) << out;
    return done;
}

// Scale a .bitmap tag's 2D textures down until their largest side is at most `max_side`: drop the top
// mip level where there is one, otherwise resample. Returns how many textures were scaled (-1 = unreadable).
int cap_bitmap_tag(const fs::path &p, int max_side) {
    std::string b;
    {
        std::ifstream f(p, std::ios::binary);
        b.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    const size_t S = 64;
    if (b.size() < S + 108 || b.compare(36, 4, "bitm") != 0) return -1;
    if (be16(b, S) != 0 && be16(b, S) != 3) return 0; // 2D textures and sprite sheets (not interface bitmaps, cube maps or 3D)
    uint32_t cp = be32(b, S + 28), pp = be32(b, S + 48), nseq = be32(b, S + 84), nbd = be32(b, S + 96);
    size_t pp_off = S + 108 + (size_t)cp, seq_off = pp_off + pp, o = seq_off + 64 * (size_t)nseq;
    if (o > b.size()) return -1;
    for (uint32_t i = 0; i < nseq; i++) o += 32 * (size_t)be32(b, seq_off + 64 * i + 52);
    size_t bd_off = o;
    if (bd_off + 48 * (size_t)nbd != b.size()) return -1;
    bool any = false;
    for (uint32_t i = 0; i < nbd; i++)
        any = any || std::max(be16(b, bd_off + 48 * i + 4), be16(b, bd_off + 48 * i + 6)) > max_side;
    if (!any) return 0;
    std::string pix = b.substr(pp_off, pp), out_pix;
    std::vector<uint32_t> offs;
    for (uint32_t i = 0; i < nbd; i++) offs.push_back(be32(b, bd_off + 48 * i + 24));
    int scaled = 0;
    for (uint32_t i = 0; i < nbd; i++) {
        size_t e = bd_off + 48 * i;
        uint32_t off = offs[i], end = pp;
        for (auto x : offs)
            if (x > off && x < end) end = x;
        if (off > pix.size() || end > pix.size()) return -1;
        std::string data = pix.substr(off, end - off);
        int w = be16(b, e + 4), h = be16(b, e + 6), type = be16(b, e + 10), fmt = be16(b, e + 12), flags = be16(b, e + 14);
        int mips = be16(b, e + 20);
        bool did = false;
        while (type == 0 && !(flags & 8) && std::max(w, h) > max_side) { // not swizzled (palettized: mips only)
            size_t m0 = mip_bytes(w, h, fmt);
            if (!m0 || (block_format(fmt) && !block_halvable(w, h))) break;   // e.g. a 1024x4 DXT strip stays as is
            std::string nd;
            if (mips >= 1 && data.size() > m0) {
                nd = data.substr(m0);
                mips--;
            } else {
                std::string top = data.substr(0, std::min(data.size(), m0));
                int nf = fmt;
                if (!halve_pixels(top, w, h, nf, nd)) break;
                fmt = nf;
                mips = 0;
            }
            data = nd;
            w = std::max(1, w / 2);
            h = std::max(1, h / 2);
            did = true;
        }
        if (did) {
            int w0 = be16(b, e + 4), h0 = be16(b, e + 6); // sprite sheets: registration point is in pixels
            if (w0 > 0 && h0 > 0) {
                put_be16(b, e + 16, (uint16_t)((int16_t)be16(b, e + 16) * w / w0));
                put_be16(b, e + 18, (uint16_t)((int16_t)be16(b, e + 18) * h / h0));
            }
            put_be16(b, e + 4, (uint16_t)w);
            put_be16(b, e + 6, (uint16_t)h);
            put_be16(b, e + 12, (uint16_t)fmt);
            put_be16(b, e + 20, (uint16_t)mips);
            scaled++;
        }
        put_be32(b, e + 24, (uint32_t)out_pix.size());
        put_be32(b, e + 28, (uint32_t)data.size());
        out_pix += data;
    }
    if (!scaled) return 0;
    std::string out = b.substr(0, pp_off) + out_pix + b.substr(pp_off + pp);
    put_be32(out, S + 48, (uint32_t)out_pix.size());
    put_be32(out, 40, ~crc32_of(out.data() + 64, out.size() - 64));
    std::ofstream(p, std::ios::binary) << out;
    return scaled;
}

// One texture's pixel data (all its mip levels) halved `times` times: the top mip level is dropped
// where there is a smaller one, otherwise the top level is resampled. False if it can't be done.
static bool halve_texture(std::string &data, int &w, int &h, int &fmt, int &mips, int times) {
    for (int k = 0; k < times; k++) {
        size_t m0 = mip_bytes(w, h, fmt);
        if (!m0 || w < 2 || h < 2 || (block_format(fmt) && !block_halvable(w, h))) return false;
        std::string nd;
        if (mips >= 1 && data.size() > m0) {
            nd = data.substr(m0);
            mips--;
        } else {
            std::string top = data.substr(0, std::min(data.size(), m0));
            int nf = fmt;
            if (!halve_pixels(top, w, h, nf, nd)) return false;
            fmt = nf;
            mips = 0;
        }
        data = nd;
        w = std::max(1, w / 2);
        h = std::max(1, h / 2);
    }
    return true;
}

// HUD bitmaps that MCC (or Halo PC) draws at half size ("half hud scale"): shrink every texture in the
// tag the same number of times, up to `times`, so the Xbox draws them at the size MCC does. Sprite
// rectangles are fractions of the sheet, so they stay right. Returns how many halvings were done for all
// of the tag's textures (0 = none: unreadable, or a texture that can't be halved).
int shrink_hud_bitmap_tag(const fs::path &p, int times) {
    std::string b;
    {
        std::ifstream f(p, std::ios::binary);
        b.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    const size_t S = 64;
    if (b.size() < S + 108 || b.compare(36, 4, "bitm") != 0) return 0;
    uint32_t cp = be32(b, S + 28), pp = be32(b, S + 48), nseq = be32(b, S + 84), nbd = be32(b, S + 96);
    size_t pp_off = S + 108 + (size_t)cp, seq_off = pp_off + pp, o = seq_off + 64 * (size_t)nseq;
    if (pp_off + pp > b.size() || o > b.size() || !nbd) return 0;
    for (uint32_t i = 0; i < nseq; i++) o += 32 * (size_t)be32(b, seq_off + 64 * i + 52);
    size_t bd_off = o;
    if (bd_off + 48 * (size_t)nbd != b.size()) return 0;
    std::string pix = b.substr(pp_off, pp);
    std::vector<uint32_t> offs;
    for (uint32_t i = 0; i < nbd; i++) offs.push_back(be32(b, bd_off + 48 * i + 24));
    for (int k = times; k >= 1; k--) {
        std::string nb = b, out_pix;
        bool ok = true;
        for (uint32_t i = 0; i < nbd && ok; i++) {
            size_t e = bd_off + 48 * i;
            uint32_t off = offs[i], end = pp;
            for (auto x : offs)
                if (x > off && x < end) end = x;
            if (off > pix.size() || end > pix.size()) return 0;
            std::string data = pix.substr(off, end - off);
            int w = be16(b, e + 4), h = be16(b, e + 6), type = be16(b, e + 10), fmt = be16(b, e + 12), flags = be16(b, e + 14);
            int mips = be16(b, e + 20), w0 = w, h0 = h;
            if (type != 0 || (flags & 8) || !halve_texture(data, w, h, fmt, mips, k)) { ok = false; break; }
            put_be16(nb, e + 16, (uint16_t)((int16_t)be16(b, e + 16) * w / w0));   // registration point, in pixels
            put_be16(nb, e + 18, (uint16_t)((int16_t)be16(b, e + 18) * h / h0));
            put_be16(nb, e + 4, (uint16_t)w);
            put_be16(nb, e + 6, (uint16_t)h);
            put_be16(nb, e + 12, (uint16_t)fmt);
            put_be16(nb, e + 20, (uint16_t)mips);
            put_be32(nb, e + 24, (uint32_t)out_pix.size());
            put_be32(nb, e + 28, (uint32_t)data.size());
            out_pix += data;
        }
        if (!ok) continue;
        put_be16(nb, S + 6, (uint16_t)(be16(nb, S + 6) & ~0x90));   // the half-scale flags are done with
        std::string out = nb.substr(0, pp_off) + out_pix + nb.substr(pp_off + pp);
        put_be32(out, S + 48, (uint32_t)out_pix.size());
        put_be32(out, 40, ~crc32_of(out.data() + 64, out.size() - 64));
        std::ofstream(p, std::ios::binary) << out;
        return k;
    }
    return 0;
}

// A bitmap tag's parts. Layout: 64-byte header, Bitmap (108), color plate data, pixel data, sequences (64 each),
// their sprites (32 each), bitmap data entries (48 each).
struct BitmapParts {
    std::string b;
    size_t pp_off = 0, seq_off = 0, spr_off = 0, bd_off = 0;
    uint32_t cp = 0, pp = 0, nseq = 0, nbd = 0;
    bool read(const fs::path &p) {
        std::ifstream f(p, std::ios::binary);
        b.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        const size_t S = 64;
        if (b.size() < S + 108 || b.compare(36, 4, "bitm") != 0) return false;
        cp = be32(b, S + 28); pp = be32(b, S + 48); nseq = be32(b, S + 84); nbd = be32(b, S + 96);
        pp_off = S + 108 + (size_t)cp; seq_off = pp_off + pp; spr_off = seq_off + 64 * (size_t)nseq;
        if (spr_off > b.size()) return false;
        size_t o = spr_off;
        for (uint32_t i = 0; i < nseq; i++) o += 32 * (size_t)be32(b, seq_off + 64 * i + 52);
        bd_off = o;
        return bd_off + 48 * (size_t)nbd == b.size();
    }
    uint32_t sprites(uint32_t seq) const { return be32(b, seq_off + 64 * seq + 52); }
    size_t sprite_at(uint32_t seq) const {   // where sequence `seq`'s sprites start
        size_t o = spr_off;
        for (uint32_t i = 0; i < seq; i++) o += 32 * (size_t)sprites(i);
        return o;
    }
};

// A HUD message icon sheet for the Xbox: the Xbox's own sheet, unchanged (its controller-button glyphs and the
// stock weapons' icons keep their sequence numbers), followed by the map's sequences from number `from` on
// (its custom weapons' icons), whose textures are appended. Writes `out`. False if either tag can't be read.
bool merge_icon_sheets(const fs::path &xbox_sheet, const fs::path &map_sheet, uint32_t from, const fs::path &out) {
    BitmapParts x, m;
    if (!x.read(xbox_sheet) || !m.read(map_sheet) || from > m.nseq) return false;
    // Only the map's textures that its added sequences use, renumbered after the Xbox's.
    std::map<int, int> remap;
    auto use = [&](int bi) {
        if (bi >= 0 && bi < (int)m.nbd && !remap.count(bi)) remap[bi] = 0;
    };
    for (uint32_t i = from; i < m.nseq; i++) {
        int16_t first = (int16_t)be16(m.b, m.seq_off + 64 * (size_t)i + 32), count = (int16_t)be16(m.b, m.seq_off + 64 * (size_t)i + 34);
        for (int k = 0; first >= 0 && k < count; k++) use(first + k);
        size_t so = m.sprite_at(i);
        for (uint32_t k = 0; k < m.sprites(i); k++) use((int16_t)be16(m.b, so + 32 * (size_t)k));
    }
    int next = (int)x.nbd;
    for (auto &[bi, ni] : remap) ni = next++;
    auto renumber = [&](int bi) { auto it = remap.find(bi); return it == remap.end() ? bi : it->second; };

    std::string seqs = x.b.substr(x.seq_off, 64 * (size_t)x.nseq);
    std::string sprs = x.b.substr(x.spr_off, x.bd_off - x.spr_off);
    std::string bds = x.b.substr(x.bd_off, 48 * (size_t)x.nbd);
    for (uint32_t i = from; i < m.nseq; i++) {
        std::string s = m.b.substr(m.seq_off + 64 * (size_t)i, 64);
        int16_t first = (int16_t)be16(s, 32);
        if (first >= 0) put_be16(s, 32, (uint16_t)renumber(first));
        seqs += s;
        size_t so = m.sprite_at(i);
        for (uint32_t k = 0; k < m.sprites(i); k++) {
            std::string sp = m.b.substr(so + 32 * (size_t)k, 32);
            int16_t bi = (int16_t)be16(sp, 0);
            if (bi >= 0) put_be16(sp, 0, (uint16_t)renumber(bi));
            sprs += sp;
        }
    }
    std::vector<uint32_t> offs;
    for (uint32_t i = 0; i < m.nbd; i++) offs.push_back(be32(m.b, m.bd_off + 48 * (size_t)i + 24));
    std::string mpix;
    for (auto &[bi, ni] : remap) {
        std::string e = m.b.substr(m.bd_off + 48 * (size_t)bi, 48);
        uint32_t off = offs[bi], end = m.pp;   // each texture's data runs to the next one's (sizes may be 0)
        for (auto o2 : offs)
            if (o2 > off && o2 < end) end = o2;
        if (off > m.pp) return false;
        put_be32(e, 24, x.pp + (uint32_t)mpix.size());
        put_be32(e, 28, end - off);
        mpix += m.b.substr(m.pp_off + off, end - off);
        bds += e;
    }
    const size_t S = 64;
    std::string o = x.b.substr(0, x.pp_off) + x.b.substr(x.pp_off, x.pp) + mpix + seqs + sprs + bds;
    put_be32(o, S + 48, x.pp + (uint32_t)mpix.size());
    put_be32(o, S + 84, x.nseq + (m.nseq - from));
    put_be32(o, S + 96, x.nbd + (uint32_t)remap.size());
    put_be32(o, 40, ~crc32_of(o.data() + 64, o.size() - 64));
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    f << o;
    return (bool)f;
}

// Re-encode a .bitmap tag's uncompressed 16/32-bit color textures (2D and sprite sheets) as DXT1, or DXT5
// when they use transparency: a quarter of the texture memory or less (`write` false = just count). Bump, height and light maps are
// left alone. Returns how many textures were compressed (-1 = unreadable).
int compress_color_bitmap_tag(const fs::path &p, bool write) {
    std::string b;
    {
        std::ifstream f(p, std::ios::binary);
        b.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    const size_t S = 64;
    if (b.size() < S + 108 || b.compare(36, 4, "bitm") != 0) return -1;
    int ttype = be16(b, S), usage = be16(b, S + 4);
    if ((ttype != 0 && ttype != 3) || usage == 2 || usage == 4 || usage == 5) return 0;
    uint32_t cp = be32(b, S + 28), pp = be32(b, S + 48), nseq = be32(b, S + 84), nbd = be32(b, S + 96);
    size_t pp_off = S + 108 + (size_t)cp, seq_off = pp_off + pp, o = seq_off + 64 * (size_t)nseq;
    if (o > b.size()) return -1;
    for (uint32_t i = 0; i < nseq; i++) o += 32 * (size_t)be32(b, seq_off + 64 * i + 52);
    size_t bd_off = o;
    if (bd_off + 48 * (size_t)nbd != b.size()) return -1;
    auto eligible = [&](size_t e) {
        int fmt = be16(b, e + 12), flags = be16(b, e + 14);
        return be16(b, e + 10) == 0 && (fmt == 6 || fmt == 8 || fmt == 9 || fmt == 10 || fmt == 11) && (flags & 1) &&
               !(flags & 8);
    };
    int count = 0;
    for (uint32_t i = 0; i < nbd; i++) count += eligible(bd_off + 48 * i);
    if (!count || !write) return count;
    std::string pix = b.substr(pp_off, pp), out_pix;
    std::vector<uint32_t> offs;
    for (uint32_t i = 0; i < nbd; i++) offs.push_back(be32(b, bd_off + 48 * i + 24));
    int done = 0;
    bool any_alpha = false;
    for (uint32_t i = 0; i < nbd; i++) {
        size_t e = bd_off + 48 * i;
        uint32_t off = offs[i], end = pp;
        for (auto x : offs)
            if (x > off && x < end) end = x;
        if (off > pix.size() || end > pix.size()) return -1;
        std::string data = pix.substr(off, end - off);
        if (eligible(e)) {
            int w = be16(b, e + 4), h = be16(b, e + 6), fmt = be16(b, e + 12), mips = be16(b, e + 20);
            int bpp = fmt >= 10 ? 4 : 2;
            size_t need = 0;
            for (int k = 0; k <= mips; k++) need += (size_t)std::max(1, w >> k) * std::max(1, h >> k) * bpp;
            // One pixel as R G B A (32-bit data is stored B G R A; 16-bit little endian).
            auto rgba = [&](const unsigned char *q, unsigned char *d) {
                if (bpp == 4) { d[0] = q[2]; d[1] = q[1]; d[2] = q[0]; d[3] = fmt == 11 ? q[3] : 255; return; }
                unsigned v = q[0] | (q[1] << 8);
                if (fmt == 6) { d[0] = (v >> 11) * 255 / 31; d[1] = ((v >> 5) & 63) * 255 / 63; d[2] = (v & 31) * 255 / 31; d[3] = 255; }
                else if (fmt == 8) { d[3] = (v >> 15) * 255; d[0] = ((v >> 10) & 31) * 255 / 31; d[1] = ((v >> 5) & 31) * 255 / 31; d[2] = (v & 31) * 255 / 31; }
                else { d[3] = (v >> 12) * 17; d[0] = ((v >> 8) & 15) * 17; d[1] = ((v >> 4) & 15) * 17; d[2] = (v & 15) * 17; }
            };
            if (data.size() >= need) {
                const unsigned char *px = (const unsigned char *)data.data();
                bool alpha = false;
                for (size_t q = 0; q < (size_t)w * h && !alpha && (fmt == 8 || fmt == 9 || fmt == 11); q++) {
                    unsigned char c[4];
                    rgba(px + q * bpp, c);
                    alpha = c[3] != 255;
                }
                std::string enc;
                size_t lvl = 0;
                for (int k = 0; k <= mips; k++) {
                    int mw = std::max(1, w >> k), mh = std::max(1, h >> k);
                    const unsigned char *src = px + lvl;
                    for (int by = 0; by < (mh + 3) / 4; by++)
                        for (int bx = 0; bx < (mw + 3) / 4; bx++) {
                            unsigned char blk[64], out[16];
                            for (int y = 0; y < 4; y++)
                                for (int x = 0; x < 4; x++) {
                                    int sx = std::min(mw - 1, bx * 4 + x), sy = std::min(mh - 1, by * 4 + y);
                                    rgba(src + ((size_t)sy * mw + sx) * bpp, blk + (y * 4 + x) * 4);
                                }
                            stb_compress_dxt_block(out, blk, alpha ? 1 : 0, STB_DXT_HIGHQUAL);
                            enc.append((const char *)out, alpha ? 16 : 8);
                        }
                    lvl += (size_t)mw * mh * bpp;
                }
                data = enc;
                put_be16(b, e + 12, alpha ? 16 : 14);
                put_be16(b, e + 14, (uint16_t)(be16(b, e + 14) | 2)); // compressed
                any_alpha = any_alpha || alpha;
                done++;
            }
        }
        put_be32(b, e + 24, (uint32_t)out_pix.size());
        put_be32(b, e + 28, (uint32_t)data.size());
        out_pix += data;
    }
    if (!done) return 0;
    if (be16(b, S + 2) == 3 || be16(b, S + 2) == 4) put_be16(b, S + 2, any_alpha ? 2 : 0); // tag format: 16/32-bit -> DXT
    std::string out = b.substr(0, pp_off) + out_pix + b.substr(pp_off + pp);
    put_be32(out, S + 48, (uint32_t)out_pix.size());
    put_be32(out, 40, ~crc32_of(out.data() + 64, out.size() - 64));
    std::ofstream(p, std::ios::binary) << out;
    return done;
}

// Texture memory a .bitmap tag needs on the Xbox (all entries and mip levels); `largest` = biggest side.
size_t bitmap_tag_memory(const fs::path &p, int *largest = nullptr) {
    std::string b;
    {
        std::ifstream f(p, std::ios::binary);
        b.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    const size_t S = 64;
    if (largest) *largest = 0;
    if (b.size() < S + 108 || b.compare(36, 4, "bitm") != 0) return 0;
    uint32_t cp = be32(b, S + 28), pp = be32(b, S + 48), nseq = be32(b, S + 84), nbd = be32(b, S + 96);
    size_t seq_off = S + 108 + (size_t)cp + pp, o = seq_off + 64 * (size_t)nseq;
    if (o > b.size()) return 0;
    for (uint32_t i = 0; i < nseq; i++) o += 32 * (size_t)be32(b, seq_off + 64 * i + 52);
    if (o + 48 * (size_t)nbd > b.size()) return 0;
    size_t total = 0;
    for (uint32_t i = 0; i < nbd; i++) {
        size_t e = o + 48 * i;
        int w = be16(b, e + 4), h = be16(b, e + 6), d = std::max(1, (int)be16(b, e + 8)), type = be16(b, e + 10),
            fmt = be16(b, e + 12), mips = be16(b, e + 20);
        if (largest) *largest = std::max(*largest, std::max(w, h));
        for (int k = 0; k <= mips; k++)
            total += mip_bytes(std::max(1, w >> k), std::max(1, h >> k), fmt) * std::max(1, d >> k) * (type == 2 ? 6 : 1);
    }
    return total;
}

bool has_suffix(const std::string &s, const char *suf) {
    size_t n = strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

// Tags `tag` depends on (paths with extensions), optionally recursively.
std::vector<std::string> tag_dependencies(Job &job, const std::string &tag, bool recursive) {
    std::string out;
    std::vector<std::string> cmd{u8(tool(job.cfg, "invader-dependency"))};
    if (recursive) cmd.push_back("-r");
    for (auto &d : job.dirs()) cmd.insert(cmd.end(), {"-t", u8(d)});
    cmd.push_back(tag);
    run_process(cmd, out);
    std::vector<std::string> v;
    std::istringstream ss(out);
    std::string l;
    while (std::getline(ss, l)) {
        l = norm_rel(l);
        if (!l.empty() && l.find('.') != std::string::npos) v.push_back(l);
    }
    return v;
}

// Where a tag can be edited: the app's own copies in place; anything from your tags folder is copied to
// fixes first. Stock tags (and missing ones) give an empty path.
fs::path editable_owner(Job &job, const std::string &tag, bool stock_too = false) {
    fs::path own = job.owner(tag);
    if (own.empty() || (own == job.overlay && !stock_too)) return {};
    if (own == job.fixes || (own == job.source && job.source != job.user_tags)) return own;
    return job.to_fixes(tag, own) ? job.fixes : fs::path();
}

std::vector<std::string> edit_lines(Job &job, const fs::path &dir, const std::string &tag, const std::vector<std::string> &args,
                                    bool *ok = nullptr) {
    std::vector<std::string> cmd{u8(tool(job.cfg, "invader-edit")), "-t", u8(dir), tag};
    cmd.insert(cmd.end(), args.begin(), args.end());
    std::string out;
    int rc = run_process(cmd, out);
    if (ok) *ok = rc == 0;
    std::vector<std::string> v;
    std::istringstream ss(out);
    std::string l;
    while (std::getline(ss, l)) {
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        v.push_back(l);
    }
    return v;
}

// The Xbox hangs drawing transparent ("chicago") shaders that stack extra layers, such as the layered
// holograms from MCC-era tagsets, so those are flattened to their base layer.
void flatten_layered_shaders(Job &job, const std::vector<std::string> &deps) {
    int n = 0;
    for (auto &dep : deps) {
        if (!has_suffix(dep, ".shader_transparent_chicago")) continue;
        fs::path own = job.owner(dep);
        if (own.empty() || own == job.overlay) continue;
        auto c = edit_lines(job, own, dep, {"-C", "extra_layers"});
        if (c.empty() || std::atoi(c[0].c_str()) <= 0) continue;
        fs::path ed = editable_owner(job, dep);
        if (ed.empty()) continue;
        bool ok = false;
        edit_lines(job, ed, dep, {"-E", "extra_layers[*]"}, &ok);
        if (ok) n++;
    }
    if (n)
        job.note("[shaders] flattened " + std::to_string(n) +
                 " layered transparent shaders to their base layer (the Xbox hangs drawing extra shader layers, "
                 "e.g. holograms)");
}

// Looping effects attached to objects (fires, smoke, sparks, wrecks) are the Xbox's main framerate cost
// on PC/MCC maps: each particle count is cut to a quarter, and very fast-repeating events repeat a third
// as often. Stock effects are left alone.
void thin_attached_effects(Job &job, const std::vector<std::string> &deps) {
    static const char *objs[] = {".scenery", ".device_machine", ".device_control", ".device_light_fixture", ".vehicle",
                                 ".biped", ".weapon", ".equipment", ".garbage", ".projectile", ".placeholder",
                                 ".sound_scenery"};
    std::set<std::string> effects;
    for (auto &dep : deps) {
        bool obj = false;
        for (auto o : objs) obj = obj || has_suffix(dep, o);
        if (!obj) continue;
        fs::path own = job.owner(dep);
        if (own.empty() || own == job.overlay) continue;
        for (auto &l : edit_lines(job, own, dep, {"-G", "attachments[*].type"})) {
            std::string t = norm_rel(l);
            if (has_suffix(t, ".effect") && t[0] != '.') effects.insert(t);
        }
    }
    int thinned = 0;
    for (auto &fx : effects) {
        fs::path own = job.owner(fx);
        if (own.empty() || own == job.overlay) continue;
        auto ne = edit_lines(job, own, fx, {"-C", "events"});
        int events = ne.empty() ? 0 : std::atoi(ne[0].c_str());
        if (events <= 0) continue;
        std::vector<std::string> q;
        for (int e = 0; e < events; e++) {
            q.insert(q.end(), {"-C", "events[" + std::to_string(e) + "].particles"});
            q.insert(q.end(), {"-G", "events[" + std::to_string(e) + "].duration_bounds"});
        }
        auto info = edit_lines(job, own, fx, q);
        if ((int)info.size() < 2 * events) continue;
        std::vector<std::string> g;
        std::vector<int> counts;
        for (int e = 0; e < events; e++) {
            int n = std::atoi(info[2 * e].c_str());
            counts.push_back(n);
            for (int i = 0; i < n; i++)
                g.insert(g.end(), {"-G", "events[" + std::to_string(e) + "].particles[" + std::to_string(i) + "].count"});
        }
        auto vals = g.empty() ? std::vector<std::string>() : edit_lines(job, own, fx, g);
        if (vals.size() * 2 != g.size()) continue;
        std::vector<std::string> sets;
        size_t vi = 0;
        for (int e = 0; e < events; e++) {
            double d0 = 0, d1 = 0;
            sscanf(info[2 * e + 1].c_str(), "%lf,%lf", &d0, &d1);
            bool fast = std::max(d0, d1) > 0 && std::max(d0, d1) < 0.2;
            bool big = false;
            std::vector<std::pair<double, double>> c;
            for (int i = 0; i < counts[e]; i++, vi++) {
                double a = 0, b2 = 0;
                sscanf(vals[vi].c_str(), "%lf,%lf", &a, &b2);
                c.push_back({a, b2});
                big = big || b2 >= 4;
            }
            if (!fast && !big) continue;
            for (int i = 0; i < counts[e]; i++) {
                int na = std::max(1, (int)std::ceil(c[i].first / 4)), nb = std::max(1, (int)std::ceil(c[i].second / 4));
                if (c[i].second <= 1) continue;
                sets.insert(sets.end(), {"-S", "events[" + std::to_string(e) + "].particles[" + std::to_string(i) + "].count",
                                         std::to_string(na) + "," + std::to_string(nb)});
            }
            if (fast) {
                char buf[64];
                snprintf(buf, sizeof buf, "%g,%g", d0 * 3, d1 * 3);
                sets.insert(sets.end(), {"-S", "events[" + std::to_string(e) + "].duration_bounds", buf});
            }
        }
        if (sets.empty()) continue;
        fs::path ed = editable_owner(job, fx);
        if (ed.empty()) continue;
        bool ok = false;
        edit_lines(job, ed, fx, sets, &ok);
        if (ok) thinned++;
    }
    if (thinned)
        job.note("[effects] lightened " + std::to_string(thinned) +
                 " looping effects on props (fires, smoke, sparks) to a quarter of their particles for the Xbox's framerate");
}

// Xbox maps: keep texture memory close to stock maps. The map's own textures (stock tags, HUD and menu
// art are left alone) are compressed if they're uncompressed 32-bit, and capped: the level and sky at
// xbox_max_texture (512, like stock maps), props/vehicles/effects at xbox_max_object_texture (256),
// lightmaps at xbox_max_lightmap.
void cap_texture_sizes(Job &job, const std::vector<std::string> &deps) {
    const Config &cfg = job.cfg;
    if (cfg.xbox_max_texture <= 0 && cfg.xbox_max_lightmap <= 0) return;
    std::set<std::string> world, level; // textures the level (BSP) and sky use; the BSP alone
    for (auto &d : tag_dependencies(job, job.scenario + ".scenario", false))
        if (has_suffix(d, ".scenario_structure_bsp") || has_suffix(d, ".sky")) {
            bool bsp = has_suffix(d, ".scenario_structure_bsp");
            for (auto &x : tag_dependencies(job, d, true)) {
                world.insert(x);
                if (bsp) level.insert(x);
            }
        }
    int textures = 0, objects = 0, lightmaps = 0, compressed = 0;
    for (auto &dep : deps) {
        if (!has_suffix(dep, ".bitmap")) continue;
        fs::path own = job.owner(dep);
        bool ours = own == job.fixes || (own == job.source && job.source != job.user_tags);
        if (own.empty() || !ours) continue; // stock (or your tags folder): leave alone
        std::string l = "/" + dep;
        for (auto &c : l) c = (char)tolower((unsigned char)c);
        if (l.find("/ui/") != std::string::npos || l.find("/hud") != std::string::npos) continue; // HUD and menus
        std::string head;
        {
            std::ifstream f(own / from_u8(dep), std::ios::binary);
            head.resize(64 + 8);
            f.read(&head[0], (std::streamsize)head.size());
            if (f.gcount() < (std::streamsize)head.size()) continue;
        }
        bool lightmap = be16(head, 64 + 4) == 4; // usage: light map
        bool object = !world.count(dep);
        int cap = lightmap ? cfg.xbox_max_lightmap
                : (object && cfg.xbox_max_object_texture > 0 && cfg.xbox_max_texture > 0)
                    ? std::min(cfg.xbox_max_object_texture, cfg.xbox_max_texture)
                    : cfg.xbox_max_texture;
        if (!lightmap && cfg.xbox_max_texture > 0) {
            int c = compress_color_bitmap_tag(own / from_u8(dep), true);
            if (c > 0) compressed += c;
        }
        if (cap <= 0) continue;
        int n = cap_bitmap_tag(own / from_u8(dep), cap);
        if (n > 0) (lightmap ? lightmaps : object && cap < cfg.xbox_max_texture ? objects : textures) += n;
    }
    // Level texture budget: stock outdoor maps use about 3-6 MB of level textures (Blood Gulch 3.3 MB,
    // Sidewinder 5.3 MB, not counting lightmaps). The Xbox loads textures as they come into view, and a
    // level far over that makes it stutter (or drop HUD textures) wherever you can see most of the map.
    // Uncompressed ones are compressed first, then the biggest are halved, bump and detail maps first,
    // until the level fits. Stock level textures count too (each Xbox map has its own copy).
    int halved = 0;
    size_t level_before = 0, level_after = 0;
    if (cfg.xbox_level_texture_mb > 0 && cfg.xbox_max_texture > 0) {
        struct Tex { std::string tag; size_t bytes; int side, usage; bool done; };
        std::vector<Tex> tex;
        for (auto &dep : level) {
            if (!has_suffix(dep, ".bitmap")) continue;
            fs::path own = job.owner(dep);
            if (own.empty()) continue;
            std::string head;
            {
                std::ifstream f(own / from_u8(dep), std::ios::binary);
                head.resize(64 + 8);
                f.read(&head[0], (std::streamsize)head.size());
                if (f.gcount() < (std::streamsize)head.size()) continue;
            }
            int usage = be16(head, 64 + 4);
            if (usage == 4) continue; // lightmaps have their own setting
            if (compress_color_bitmap_tag(own / from_u8(dep), false) > 0) {
                fs::path ed = editable_owner(job, dep, true);
                int c = ed.empty() ? 0 : compress_color_bitmap_tag(ed / from_u8(dep), true);
                if (c > 0) compressed += c;
                own = job.owner(dep);
            }
            int side = 0;
            size_t bytes = bitmap_tag_memory(own / from_u8(dep), &side);
            tex.push_back({dep, bytes, side, usage, false});
            level_before += bytes;
        }
        // level_before is after compression; the budget applies from here.
        size_t budget = (size_t)(cfg.xbox_level_texture_mb * 1048576), total = level_before;
        while (total > budget) {
            // Bump, detail and vector maps first (down to 128 px), then the biggest color textures
            // (down to 128 px). Ties go to the widest texture.
            Tex *pick = nullptr;
            for (int pass = 0; pass < 2 && !pick; pass++)
                for (auto &t : tex) {
                    bool cheap = t.usage == 2 || t.usage == 3 || t.usage == 5;
                    if (t.done || t.side <= 128 || (pass == 0 && !cheap)) continue;
                    if (!pick || t.bytes > pick->bytes || (t.bytes == pick->bytes && t.side > pick->side)) pick = &t;
                }
            if (!pick) break;
            fs::path ed = editable_owner(job, pick->tag, true);
            if (ed.empty() || cap_bitmap_tag(ed / from_u8(pick->tag), pick->side / 2) <= 0) { pick->done = true; continue; }
            int side = 0;
            size_t now = bitmap_tag_memory(ed / from_u8(pick->tag), &side);
            if (now >= pick->bytes) { pick->done = true; continue; }
            total -= pick->bytes - now;
            pick->bytes = now;
            pick->side = side;
            halved++;
        }
        level_after = total;
    }
    if (compressed)
        job.note("[textures] compressed " + std::to_string(compressed) +
                 " uncompressed textures to DXT (a quarter of the memory or less)");
    if (halved) {
        char buf[64];
        snprintf(buf, sizeof buf, "%.1f MB -> %.1f MB", level_before / 1048576.0, level_after / 1048576.0);
        job.note("[textures] level textures " + std::string(buf) + " (halved the biggest " + std::to_string(halved) +
                 " times, bump and detail maps first) to stay near stock maps' texture memory");
    }
    if (textures)
        job.note("[textures] scaled " + std::to_string(textures) + " of your level textures down to " +
                 std::to_string(cfg.xbox_max_texture) + " px (largest side), like stock Xbox maps");
    if (objects)
        job.note("[textures] scaled " + std::to_string(objects) + " prop/vehicle/effect textures down to " +
                 std::to_string(std::min(cfg.xbox_max_object_texture, cfg.xbox_max_texture)) +
                 " px, so the Xbox doesn't stutter loading them as they come into view");
    if (lightmaps)
        job.note("[textures] scaled " + std::to_string(lightmaps) + " lightmap pages down to " +
                 std::to_string(cfg.xbox_max_lightmap) + " px");
}

// All Xbox-only content passes that need the finished tag set.
void xbox_content_passes(Job &job) {
    auto deps = tag_dependencies(job, job.scenario + ".scenario", true);
    flatten_layered_shaders(job, deps);
    deps = tag_dependencies(job, job.scenario + ".scenario", true); // flattened layers drop out
    thin_attached_effects(job, deps);
    cap_texture_sizes(job, deps);
    int unaligned = 0, flares = 0;
    for (auto &dep : deps) {
        fs::path own = job.owner(dep);
        if (own.empty() || own == job.overlay) continue; // the Xbox's own tags are fine as they are
        if (has_suffix(dep, ".bitmap")) {
            if (unblock_unaligned_dxt_bitmap_tag(own / from_u8(dep), false) <= 0) continue;
            fs::path ed = editable_owner(job, dep);
            int n = ed.empty() ? 0 : unblock_unaligned_dxt_bitmap_tag(ed / from_u8(dep), true);
            if (n > 0) unaligned += n;
        } else if (has_suffix(dep, ".lens_flare")) {
            // Reflections with no bitmap have nothing to draw with (Invader warns about them); removed so the
            // Xbox never tries to draw one.
            auto g = edit_lines(job, own, dep, {"-G", "bitmap"});
            auto c = edit_lines(job, own, dep, {"-C", "reflections"});
            if (g.empty() || c.empty() || (g[0] != "" && g[0] != ".bitmap") || c[0] == "0" || c[0].empty()) continue;
            fs::path ed = editable_owner(job, dep);
            bool ok = false;
            if (!ed.empty()) edit_lines(job, ed, dep, {"-E", "reflections[*]"}, &ok);
            if (ok) flares++;
        }
    }
    if (unaligned)
        job.note("[textures] stored " + std::to_string(unaligned) +
                 " DXT textures whose sides aren't multiples of 4 pixels uncompressed (the Xbox can't draw them as DXT)");
    if (flares)
        job.note("[xbox] removed the reflections from " + std::to_string(flares) +
                 " lens flares that have no bitmap to draw them with");
    if (!job.cfg.level_shaders_only) xbox_lods(job, deps);
    xbox_lights(job, deps);
    xbox_portals(job);
    xbox_reflections(job);
    if (!job.cfg.level_shaders_only) {   // (after the level is measured: the window shows the fog as the map has it)
        xbox_fog(job);
        xbox_scenery(job, deps);
    }
}

// The level's reflections (levelshaders.hpp): the Xbox draws a reflecting surface once or twice more than any other,
// so the ones that show least for what they cost lose their reflection while the level costs more than stock ones
// (or this map's own picks from the level shaders window). The measurement is kept for that window either way.
void xbox_reflections(Job &job) {
    const Config &cfg = job.cfg;
    if (job.analysis) return;
    std::string scen = job.scenario + ".scenario";
    fs::path sdir = job.owner(scen);
    tagfile::Tag stag;
    if (sdir.empty() || !stag.load(sdir / from_u8(scen))) return;
    lshade::Find find = [&job](const std::string &t) {
        fs::path o = job.owner(t);
        return o.empty() ? fs::path() : o / from_u8(t);
    };
    tagfile::Block bsps = stag.root().block("structure bsps");
    std::set<std::string> done;
    for (size_t i = 0; i < bsps.size(); i++) {
        std::string b = bsps[i].ref("structure bsp");
        if (b.empty() || !done.insert(b).second) continue;
        fs::path own = job.owner(b);
        if (own.empty()) continue;
        std::string name = bsps.size() > 1 ? " (" + fs::path(from_u8(b)).stem().u8string() + ")" : "";
        job.log("Measuring the level's shaders" + name + "...");
        lshade::Analysis a;
        lshade::Preview p;
        std::string why;
        if (!lshade::analyze(own / from_u8(b), sdir / from_u8(scen), find, a, p, &why)) {
            job.log("  [reflections] the level" + name + " couldn't be measured: it " + why);
            continue;
        }
        a.map_name = fs::path(from_u8(job.scenario)).filename().u8string() + (bsps.size() > 1 ? " - " + fs::path(from_u8(b)).stem().u8string() : "");
        lshade::suggest(a);
        if (job.shaders_file.empty()) {   // (the window shows the first level)
            fs::path f = cfg.cache_dir / "levels" / (fs::path(from_u8(job.scenario)).filename().u8string() + ".lsh");
            if (lshade::save(a, p, f)) job.shaders_file = f;
        }
        std::vector<char> none(a.shaders.size(), 0), drop(a.shaders.size(), 0);
        int reflective = 0;
        for (size_t s = 0; s < a.shaders.size(); s++) {
            if (a.shaders[s].reflection_passes > 0) reflective++;
            if (a.shaders[s].reflection_passes == 0) continue;
            drop[s] = cfg.xbox_reflection_picks_set ? cfg.xbox_reflection_picks.count(a.shaders[s].tag) > 0
                                                    : cfg.xbox_drop_reflections && a.shaders[s].suggested;
        }
        lshade::Cost before = lshade::cost(a, none), after = lshade::cost(a, drop), stock = lshade::stock_cost();
        char buf[300];
        snprintf(buf, sizeof buf, "about %s triangle passes in a typical view, %s in a busy one",
                 thousands((long long)before.typical).c_str(), thousands((long long)before.busy).c_str());
        std::string line = "Level shaders" + name + ": " + buf;
        if (cfg.level_shaders_only) continue;
        std::vector<std::string> dropped;
        for (size_t s = 0; s < a.shaders.size(); s++) {
            if (!drop[s]) continue;
            const std::string &t = a.shaders[s].tag;
            fs::path ed = editable_owner(job, t, true);
            if (!ed.empty() && lshade::drop_reflection(ed / from_u8(t))) dropped.push_back(fs::path(from_u8(t)).stem().u8string());
        }
        if (!dropped.empty()) {
            snprintf(buf, sizeof buf, "%s / %s -> %s / %s", thousands((long long)before.typical).c_str(), thousands((long long)before.busy).c_str(),
                     thousands((long long)after.typical).c_str(), thousands((long long)after.busy).c_str());
            std::string list;
            for (size_t k = 0; k < dropped.size() && k < 8; k++) list += (k ? ", " : "") + dropped[k];
            if (dropped.size() > 8) list += " and " + std::to_string(dropped.size() - 8) + " more";
            job.note("[reflections] dropped the reflections of " + std::to_string(dropped.size()) + " of the level" + name + "'s " +
                     std::to_string(reflective) + " reflecting shaders (" +
                     (cfg.xbox_reflection_picks_set ? std::string("your picks") : std::string("the ones that show least for what they cost")) +
                     "): " + list + ". The Xbox draws a reflecting surface once or twice more than others; the level's triangle "
                     "passes in a typical / busy view: " + buf);
            snprintf(buf, sizeof buf, " (%s / %s with the reflections it had)", thousands((long long)before.typical).c_str(), thousands((long long)before.busy).c_str());
            line = "Level shaders" + name + ": about " + thousands((long long)after.typical) + " triangle passes in a typical view, " +
                   thousands((long long)after.busy) + " in a busy one" + buf;
        }
        snprintf(buf, sizeof buf, "; Sidewinder's %s / %s.", thousands((long long)stock.typical).c_str(), thousands((long long)stock.busy).c_str());
        line += buf;
        if (dropped.empty() && reflective > 0 && (before.busy > stock.busy * 1.15 || before.typical > stock.typical * 1.15))
            line += " " + std::to_string(reflective) + " of its shaders reflect, which the Xbox draws again: the Xbox tab's "
                    "\"Level...\" window shows what dropping reflections would save and how it would look.";
        job.shaders_line += (job.shaders_line.empty() ? "" : "\n") + line;
    }
}

// The map's own dynamic lights that would have the Xbox draw most of the level again. A dynamic light lights the level
// by drawing again what's in view of the pieces of it the light touches, twice (its diffuse and specular passes), out
// of 32,768 triangles a frame for everything; and the pieces are whole subclusters whose bounds touch the box around
// the light's sphere (portals::LevelChunks), up to 4,096 surfaces a pass. Deathtrap's lightning (radius 25, three at
// once, high above the level) takes nearly all of it and stops the Xbox; made smaller, it no longer reaches anything
// it could be seen on, yet still takes thousands. So a light that takes more than one pass can (kMostOfLevel) where
// the map places it goes: its "dynamic" flag off, so the engine makes no light for it (one with a lens flare keeps
// that). A light the map doesn't place (a weapon's or projectile's effects: where it goes off isn't known) is kept
// to Bungie's biggest radius (the Scorpion shell's explosion, 15).
static std::string fmt_num(double v) {   // 25, 8.9
    char b[32];
    snprintf(b, sizeof b, "%.1f", v);
    std::string t = b;
    if (t.size() > 2 && t.compare(t.size() - 2, 2, ".0") == 0) t.resize(t.size() - 2);
    return t;
}

void xbox_lights(Job &job, const std::vector<std::string> &deps) {
    const double kMaxRadius = 15.0;
    const int kMostOfLevel = 4096;
    if (job.analysis) return;
    std::string scen = job.scenario + ".scenario";
    fs::path sdir = job.owner(scen);
    tagfile::Tag stag;
    if (sdir.empty() || !stag.load(sdir / from_u8(scen))) return;
    tagfile::Elem s = stag.root();
    // The level (every BSP).
    std::vector<portals::LevelChunks> levels;
    tagfile::Block bsps = s.block("structure bsps");
    for (size_t i = 0; i < bsps.size(); i++) {
        std::string b = bsps[i].ref("structure bsp");
        fs::path own = b.empty() ? fs::path() : job.owner(b);
        portals::LevelChunks lc;
        if (!own.empty() && lc.load(own / from_u8(b))) levels.push_back(std::move(lc));
    }
    // The lights each object makes: attached directly, or through an attached effect.
    std::map<std::string, std::vector<std::string>> lights_of;
    auto lights_for = [&](const std::string &object) -> const std::vector<std::string> & {
        auto it = lights_of.find(object);
        if (it != lights_of.end()) return it->second;
        std::vector<std::string> &out = lights_of[object];
        fs::path own = job.owner(object);
        tagfile::Tag ot;
        if (own.empty() || !ot.load(own / from_u8(object))) return out;
        tagfile::Block at = ot.root().block("attachments");
        for (size_t i = 0; i < at.size(); i++) {
            std::string a = at[i].ref("type");
            if (has_suffix(a, ".light")) out.push_back(a);
            else if (has_suffix(a, ".effect")) {
                fs::path eo = job.owner(a);
                tagfile::Tag et;
                if (eo.empty() || !et.load(eo / from_u8(a))) continue;
                tagfile::Block ev = et.root().block("events");
                for (size_t e = 0; e < ev.size(); e++) {
                    tagfile::Block parts = ev[e].block("parts");
                    for (size_t k = 0; k < parts.size(); k++)
                        if (std::string l = parts[k].ref("type"); has_suffix(l, ".light")) out.push_back(l);
                }
            }
        }
        return out;
    };
    // Where the map places each light.
    std::map<std::string, std::vector<portals::V3>> placed_at;
    static const char *const kinds[][2] = {{"scenery", "scenery palette"}, {"machines", "machine palette"},
                                           {"controls", "control palette"}, {"light fixtures", "light fixture palette"}};
    for (auto &k : kinds) {
        tagfile::Block items = s.block(k[0]), pal = s.block(k[1]);
        for (size_t i = 0; i < items.size(); i++) {
            long long t = items[i].integer("type");
            if (t < 0 || t >= (long long)pal.size()) continue;
            std::string o = pal[(size_t)t].ref("name");
            if (o.empty()) continue;
            portals::V3 p{items[i].num("position", 0), items[i].num("position", 1), items[i].num("position", 2)};
            for (auto &l : lights_for(o)) {
                auto &at = placed_at[l];
                if (std::find(at.begin(), at.end(), p) == at.end()) at.push_back(p);   // (an effect may make it twice)
            }
        }
    }
    std::string removed, shrunk;
    int n_removed = 0, n_shrunk = 0, n_flares = 0;
    for (auto &dep : deps) {
        if (!has_suffix(dep, ".light")) continue;
        fs::path own = job.owner(dep);
        if (own.empty() || own == job.overlay) continue;   // (Bungie's are as Bungie made them)
        tagfile::Tag lt;
        if (!lt.load(own / from_u8(dep))) continue;
        tagfile::Elem l = lt.root();
        long long flags = (long long)l.num("flags");
        if (!(flags & 1)) continue;   // (not dynamic: only a lens flare, or for the lightmaps)
        double r = l.num("radius"), scale = std::max(1.0, l.num("radius modifer", 1));
        if (r <= 0) continue;
        std::string name = fs::path(from_u8(dep)).stem().u8string();
        auto it = placed_at.find(dep);
        if (it != placed_at.end() && !levels.empty()) {
            int least = INT_MAX, most = 0, of = 0;
            for (auto &p : it->second)
                for (auto &lv : levels) {
                    int g = lv.gathered(p, r * scale);
                    least = std::min(least, g);
                    if (g > most) { most = g; of = lv.surface_count; }
                }
            if (most <= kMostOfLevel) continue;
            fs::path ed = editable_owner(job, dep);
            if (ed.empty()) continue;
            tagfile::EditTag et;
            if (!et.load(ed / from_u8(dep)) || !tagfile::set_num(et.root(), "flags", (double)(flags & ~1LL)) ||
                !et.save(ed / from_u8(dep)))
                continue;
            n_removed++;
            if (!l.ref("lens flare").empty()) n_flares++;
            std::string where = it->second.size() == 1 ? "where the map places it, " + thousands(most)
                                                       : "at the " + std::to_string(it->second.size()) + " places the map has it, " +
                                                             (least == most ? thousands(most) : thousands(least) + " to " + thousands(most));
            removed += (removed.empty() ? "" : "; ") + name + " (radius " + fmt_num(r * scale) + "; " + where + " of the level's " +
                       thousands(of) + " triangles)";
            continue;
        }
        if (r * scale <= kMaxRadius + 1e-3) continue;
        double nr = kMaxRadius / scale;
        fs::path ed = editable_owner(job, dep);
        if (ed.empty()) continue;
        tagfile::EditTag et;
        if (!et.load(ed / from_u8(dep)) || !tagfile::set_num(et.root(), "radius", nr) || !et.save(ed / from_u8(dep))) continue;
        n_shrunk++;
        shrunk += (shrunk.empty() ? "" : ", ") + name + " (radius " + fmt_num(r * scale) + " -> " + fmt_num(kMaxRadius) + ")";
    }
    if (n_removed)
        job.note("[lights] turned off " + std::to_string(n_removed) + (n_removed == 1 ? " dynamic light" : " dynamic lights") +
                 " that would have the Xbox draw most of the level again each time it lit up (the engine redraws whole pieces "
                 "of the level a light's box touches, not just what it reaches; too much at once stops the game): " + removed +
                 (n_removed == 1 ? ". What makes it still plays (sounds, particles)" : ". What makes them still plays (sounds, particles)") +
                 (n_flares ? std::string(", and a light's lens flare stays.") : std::string(".")));
    if (n_shrunk)
        job.note("[lights] shrank " + std::to_string(n_shrunk) + (n_shrunk == 1 ? " dynamic light" : " dynamic lights") +
                 " of the map's weapons or effects to Bungie's biggest (radius 15, the Scorpion shell's explosion): " + shrunk);
}

// A level made without portals is one cluster: the Xbox draws everything of it in view, walls or not. Its rooms and
// the openings between them (doorways, corridors) become clusters and portals (portals.hpp), when that draws less
// from where players are. Levels with clusters of their own, and the Xbox's own levels, are left as they are.
void xbox_portals(Job &job) {
    if (!job.cfg.xbox_add_portals || job.analysis) return;
    std::string scen = job.scenario + ".scenario";
    fs::path sdir = job.owner(scen);
    tagfile::Tag stag;
    if (sdir.empty() || !stag.load(sdir / from_u8(scen))) return;
    tagfile::Block bsps = stag.root().block("structure bsps");
    std::set<std::string> done;
    for (size_t i = 0; i < bsps.size(); i++) {
        std::string b = bsps[i].ref("structure bsp");
        if (b.empty() || !done.insert(b).second) continue;
        fs::path own = job.owner(b);
        if (own.empty() || own == job.overlay) continue;
        size_t own_clusters = 0;
        {
            tagfile::Tag t;
            if (!t.load(own / from_u8(b))) continue;
            own_clusters = t.root().block("clusters").size();
            if (own_clusters == 0 || (own_clusters > 1 && !job.cfg.xbox_replan_portals)) continue;
        }
        std::string name = bsps.size() > 1 ? " (" + fs::path(from_u8(b)).stem().u8string() + ")" : "";
        job.log("Finding the level's rooms and doorways" + name + "...");
        fs::path ed = editable_owner(job, b);
        if (ed.empty()) continue;
        fs::path file = ed / from_u8(b);
        std::string before;
        {
            std::ifstream f(file, std::ios::binary);
            before.assign(std::istreambuf_iterator<char>(f), {});
        }
        // What can be seen through: glass, water and the like; of the level's own shaders, the alpha-tested ones
        // (grates, fences).
        portals::Options po;
        po.area = portals::play_area(sdir / from_u8(scen));
        po.see_through = [&job](const std::string &shader, const std::string &group) {
            if (group != "senv") return group != "soso";
            std::string t = shader + ".shader_environment";
            fs::path own = job.owner(t);
            tagfile::Tag st;
            if (own.empty() || !st.load(own / from_u8(t))) return false;
            return ((long long)st.root().num("shader environment flags") & 1) != 0;   // alpha tested
        };
        po.replan = job.cfg.xbox_replan_portals;
        portals::Result pr = portals::add_portals(file, po);
        auto drawn = [&](const portals::Result &x) {
            return "about " + thousands((long long)x.drawn_after) + " of its triangles drawn in a typical view (was " +
                   thousands((long long)x.drawn_before) + "), " + thousands((long long)x.drawn_high_after) + " in a busy one (was " +
                   thousands((long long)x.drawn_high_before) + ")";
        };
        if (pr.changed && own_clusters == 1) {
            job.portal_backups.push_back({file, before});
            std::string note = "[portals] the level" + name + " had no portals, so the Xbox drew everything of it in view, walls or not. Its " +
                               std::to_string(pr.clusters_after) + " rooms are now clusters with " + std::to_string(pr.portals_after) +
                               " portals between them: " + drawn(pr);
            // (and, re-planning, its new clusters divided further when that draws less again)
            if (po.replan) {
                portals::Result again = portals::add_portals(file, po);
                if (again.changed)
                    note += "; divided further into " + std::to_string(again.clusters_after) + " clusters with " +
                            std::to_string(again.portals_after) + " portals: " + drawn(again);
            }
            job.note(note);
        } else if (pr.changed) {
            job.portal_backups.push_back({file, before});
            job.note("[portals] divided the level" + name + "'s own " + std::to_string(pr.clusters_before) + " clusters further, into " +
                     std::to_string(pr.clusters_after) + " (each keeps its cluster's sky, fog, sound and weather) with " +
                     std::to_string(pr.portals_after) + " portals (it had " + std::to_string(pr.portals_before) + "): " + drawn(pr));
        } else if (own_clusters == 1) {
            job.log("  [portals] the level" + name + " has no portals; none added: it " + pr.why);
        } else {
            job.log("  [portals] the level" + name + "'s own portals kept: it " + pr.why);
        }
    }
}

// The kind of object each model is drawn for in the world (v vehicle, b biped, s scenery and devices, w weapons and
// other items), from the object tags among `deps`; and the weapons' first-person models.
static std::map<std::string, char> model_kinds(Job &job, const std::vector<std::string> &deps,
                                               std::set<std::string> &first_person,
                                               std::map<std::string, std::string> &object_name) {
    std::map<std::string, char> kind;
    for (auto &dep : deps)
        if (has_suffix(dep, ".weapon")) {
            fs::path own = job.owner(dep);
            std::string m = own.empty() ? "" : perf::weapon_first_person_model(own / from_u8(dep));
            if (!m.empty()) first_person.insert(m);
        }
    static const std::pair<const char *, char> kinds[] = {
        {".vehicle", 'v'}, {".biped", 'b'}, {".scenery", 's'}, {".device_machine", 's'}, {".device_control", 's'},
        {".device_light_fixture", 's'}, {".sound_scenery", 's'}, {".placeholder", 's'}, {".weapon", 'w'},
        {".equipment", 'w'}, {".garbage", 'w'}, {".projectile", 'w'}};
    static const std::string rank = "vbsw";   // a model several kinds use: the first of these
    for (auto &dep : deps)
        for (auto &[ext, k] : kinds) {
            if (!has_suffix(dep, ext)) continue;
            fs::path own = job.owner(dep);
            std::string m = own.empty() ? "" : perf::object_model(own / from_u8(dep));
            if (m.empty()) break;
            auto it = kind.find(m);
            if (it == kind.end() || rank.find(k) < rank.find(it->second)) kind[m] = k;
            if (!object_name.count(m)) object_name[m] = fs::path(from_u8(dep)).stem().u8string();
            break;
        }
    return kind;
}

static std::string thousands(long long v);

// Levels of detail. A model drawn in the world (not first-person) of the map's own with no lower-detail versions gets
// them made (lod.hpp; Xbox tab option, on by default), kept in the LOD library so another map with the same model
// reuses them; characters are left alone (bones bend them). Then models with lower-detail versions but no switch
// distances get some: they always draw their most detailed level otherwise (the engine draws a level once the model
// is at least that many pixels across, and with every distance 0 that's always the top one). The distances are like
// the stock Xbox models of the kind (Bungie's own, in tag order super high, high, medium, low, super low; super low
// stays 0, so nothing is left out). The map's own models always; Bungie's (36 stock models ship like this, mostly
// trees, rocks and plants) with the Xbox tab's option. Only models an object draws in the world: the engine draws
// first-person models (weapons, hands) as 0 pixels across, so with switch distances they'd always be at their least
// detailed.
void xbox_lods(Job &job, const std::vector<std::string> &deps) {
    struct Cut { char kind; const char *label; int px[5]; };
    static const Cut cuts[] = {{'v', "vehicles", {200, 100, 50, 20, 0}},
                               {'b', "characters", {280, 200, 80, 30, 0}},
                               {'s', "scenery", {300, 90, 30, 5, 0}},
                               {'w', "weapons and items", {100, 50, 25, 10, 0}}};
    static const char *keys[5] = {"super_high_detail_cutoff", "high_detail_cutoff", "medium_detail_cutoff",
                                  "low_detail_cutoff", "super_low_detail_cutoff"};
    std::set<std::string> first_person;
    std::map<std::string, std::string> names;
    std::map<std::string, char> kinds = model_kinds(job, deps, first_person, names);
    auto cut_for = [&](const std::string &dep) {
        const Cut *c = &cuts[0];
        for (auto &x : cuts)
            if (x.kind == kinds[dep]) c = &x;
        return c;
    };
    auto name_of = [&](const std::string &dep) { return names.count(dep) ? names[dep] : fs::path(from_u8(dep)).stem().u8string(); };
    auto list = [](const std::vector<std::string> &v, size_t most = 8) {
        std::string s;
        for (size_t i = 0; i < v.size() && i < most; i++) s += (i ? ", " : "") + v[i];
        if (v.size() > most) s += " and " + std::to_string(v.size() - most) + " more";
        return s;
    };

    lod::Settings lod_settings;
    lod_settings.library = !job.cfg.lod_library.empty() ? job.cfg.lod_library : job.cfg.cache_dir / "lod_library";

    // 1. Making lower-detail versions. Not when measuring for the weapons & vehicles window: it shows what the map
    // needs to fit, and the conversion takes them out again when they don't fit.
    if (job.cfg.xbox_make_lods && !job.analysis) {
        const lod::Settings &ls = lod_settings;
        std::vector<std::string> made, reused, left;
        size_t bytes = 0;
        for (auto &dep : deps) {
            if (!has_suffix(dep, ".model") && !has_suffix(dep, ".gbxmodel")) continue;
            fs::path own = job.owner(dep);
            if (own.empty() || own == job.overlay || !kinds.count(dep) || first_person.count(dep) || kinds[dep] == 'b') continue;
            perf::ModelStats ms = perf::model_stats(own / from_u8(dep));
            if (!ms.ok || ms.lods || ms.triangles < ls.min_triangles) continue;
            if (ms.borrowed && ms.no_cutoffs()) {   // switch distances would turn its variants into one another
                left.push_back(name_of(dep) + " (its variants share their lower levels, so they'd change shape at a distance)");
                continue;
            }
            if (lod::skipped(ls, dep, name_of(dep))) {
                left.push_back(name_of(dep) + " (listed in the LOD library's skip.txt)");
                continue;
            }
            fs::path ed = editable_owner(job, dep);
            if (ed.empty()) continue;
            double px[5];
            for (int i = 0; i < 5; i++) px[i] = cut_for(dep)->px[i];
            std::string before;
            {
                std::ifstream in(ed / from_u8(dep), std::ios::binary);
                before.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            }
            lod::Result r = lod::add_lods(ed / from_u8(dep), px, ls, dep);
            if (!r.changed) {
                left.push_back(name_of(dep) + " (" + r.why + ")");
                continue;
            }
            job.lod_backups.push_back({ed / from_u8(dep), std::move(before)});
            job.lod_xbox_bytes += r.xbox_bytes;
            bytes += r.xbox_bytes;
            std::string l = name_of(dep) + " " + thousands(r.triangles);
            for (int k : r.level_triangles)
                if (k) l += " > " + thousands(k);
            (r.from_library ? reused : made).push_back(l);
            LodView::Model lm;
            lm.tag = dep;
            lm.name = name_of(dep);
            lm.kind = kinds[dep];
            lm.change = r.from_library ? "library" : "made";
            job.lod_changes.push_back(lm);
        }
        if (!made.empty() || !reused.empty()) {
            char mb[32];
            snprintf(mb, sizeof mb, "%.1f", bytes / 1048576.0);
            std::vector<std::string> all = made;
            all.insert(all.end(), reused.begin(), reused.end());
            job.note("[lod] made lower-detail versions of " + std::to_string(all.size()) + " of the map's models that had none (" +
                     mb + " MB more of the Xbox's tag memory and map file; triangles, most detailed > medium > low): " + list(all) +
                     (reused.empty() ? "" : " (" + std::to_string(reused.size()) + " reused from the LOD library)"));
        }
        if (!left.empty()) job.note("[lod] left as they are: " + list(left, 4));
    }

    // 2. Switch distances.
    std::vector<std::string> own_done, stock_done;
    for (auto &dep : deps) {
        if (!has_suffix(dep, ".model") && !has_suffix(dep, ".gbxmodel")) continue;
        fs::path own = job.owner(dep);
        if (own.empty()) continue;
        bool stock = own == job.overlay;
        if (stock && !job.cfg.xbox_lod_stock_models) continue;
        if (!kinds.count(dep) || first_person.count(dep)) continue;
        perf::ModelStats ms = perf::model_stats(own / from_u8(dep));
        if (!ms.ok || !ms.lods || ms.borrowed || !ms.no_cutoffs()) continue;
        if (lod::skipped(lod_settings, dep, name_of(dep))) continue;   // (listed in the LOD library's skip.txt)
        fs::path ed = editable_owner(job, dep, stock);
        if (ed.empty()) continue;
        std::vector<std::string> args;
        for (int i = 0; i < 5; i++) args.insert(args.end(), {"-S", keys[i], std::to_string(cut_for(dep)->px[i])});
        bool ok = false;
        edit_lines(job, ed, dep, args, &ok);
        if (ok) {
            (stock ? stock_done : own_done).push_back(name_of(dep));
            LodView::Model lm;
            lm.tag = dep;
            lm.name = name_of(dep);
            lm.kind = kinds[dep];
            lm.change = stock ? "stock" : "distances";
            job.lod_changes.push_back(lm);
        }
    }
    if (!own_done.empty())
        job.note("[lod] " + std::to_string(own_done.size()) + " of the map's models had lower-detail versions they never "
                 "used (no switch distances, so always drawn in full); gave them stock-style ones: " + list(own_done));
    if (!stock_done.empty())
        job.note("[lod] gave " + std::to_string(stock_done.size()) + " stock models switch distances, so their "
                 "lower-detail versions get used (Xbox tab option): " + list(stock_done));
}

static std::string thousands(long long v) {
    std::string s = std::to_string(v < 0 ? -v : v);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, ",");
    return (v < 0 ? "-" : "") + s;
}

// How many times each object is on the map (the scenario `s`): placed, or at a weapon/item spawn point.
static std::map<std::string, int> object_counts(Job &job, const tagfile::Elem &s) {
    std::map<std::string, int> placed;
    static const char *const kinds[][2] = {
        {"scenery", "scenery palette"}, {"bipeds", "biped palette"}, {"vehicles", "vehicle palette"},
        {"equipment", "equipment palette"}, {"weapons", "weapon palette"}, {"machines", "machine palette"},
        {"controls", "control palette"}, {"light fixtures", "light fixture palette"},
        {"sound scenery", "sound scenery palette"}};
    for (auto &k : kinds) {
        tagfile::Block items = s.block(k[0]), pal = s.block(k[1]);
        for (size_t i = 0; i < items.size(); i++) {
            long long t = items[i].integer("type");
            if (t < 0 || t >= (long long)pal.size()) continue;
            std::string o = pal[(size_t)t].ref("name");
            if (!o.empty()) placed[o]++;
        }
    }
    tagfile::Block spawns = s.block("netgame equipment");
    std::map<std::string, std::vector<std::string>> collections;
    for (size_t i = 0; i < spawns.size(); i++) {
        std::string ic = spawns[i].ref("item collection");
        if (ic.empty()) continue;
        if (!collections.count(ic)) {
            auto &items = collections[ic];
            fs::path own = job.owner(ic);
            tagfile::Tag t;
            if (!own.empty() && t.load(own / from_u8(ic))) {
                tagfile::Block perms = t.root().block("permutations");
                for (size_t k = 0; k < perms.size(); k++)
                    if (auto it = perms[k].ref("item"); !it.empty()) items.push_back(it);
            }
        }
        for (auto &it : collections[ic]) placed[it]++;
    }
    return placed;
}

// The level's fog. The engine moves the view's far end to the fog's opaque distance only when the fog is fully
// opaque there (maximum density exactly 1: render.c), and then draws nothing past it: the level, objects, effects.
// Fog that's nearly so (90% or more) hides what's past it anyway, so by default it's made fully opaque (Deathtrap's,
// Precipice's sky: 92% at 110 units, past which the airbase and many trees were still drawn). The level window can
// instead make the outdoor fog fully opaque nearer (a share of its distance) or at a distance typed in. The map's own
// skies only (Bungie's levels keep theirs); a sky's indoor fog too (rooms with no sky).
void xbox_fog(Job &job) {
    const Config &cfg = job.cfg;
    if (cfg.xbox_fog == 1 || job.analysis) return;
    std::string scen = job.scenario + ".scenario";
    fs::path sdir = job.owner(scen);
    tagfile::Tag stag;
    if (sdir.empty() || !stag.load(sdir / from_u8(scen))) return;
    tagfile::Block skies = stag.root().block("skies");
    std::set<std::string> done;
    std::vector<std::string> changed;
    for (size_t i = 0; i < skies.size(); i++) {
        std::string sky = skies[i].ref("sky");
        if (sky.empty() || !done.insert(sky).second) continue;
        fs::path own = job.owner(sky);
        if (own.empty() || own == job.overlay) continue;
        fs::path ed = editable_owner(job, sky);
        if (ed.empty()) continue;
        tagfile::EditTag et;
        if (!et.load(ed / from_u8(sky))) continue;
        tagfile::TagNode &r = et.root();
        std::string what;
        for (const char *kind : {"outdoor", "indoor"}) {
            std::string k = kind;
            std::string fd = k + " fog maximum density", fs_ = k + " fog start distance", fo = k + " fog opaque distance";
            double density = tagfile::get_num(r, fd.c_str()), start = tagfile::get_num(r, fs_.c_str()),
                   opaque = tagfile::get_num(r, fo.c_str());
            if (opaque <= 0 || density <= 0) continue;
            double nd = density, no = opaque;
            if (cfg.xbox_fog == 2 && k == "outdoor") {
                nd = 1.0;
                no = std::max(opaque * std::clamp(cfg.xbox_fog_scale, 0.1, 1.0), start + 1.0);
            } else if (cfg.xbox_fog == 3 && k == "outdoor" && cfg.xbox_fog_distance > 0) {
                nd = 1.0;
                no = std::max(cfg.xbox_fog_distance, start + 1.0);
            } else if (density >= 0.9 && density < 1.0) {
                nd = 1.0;
            }
            if (nd == density && no == opaque) continue;
            tagfile::set_num(r, fd.c_str(), nd);
            tagfile::set_num(r, fo.c_str(), no);
            char buf[160];
            snprintf(buf, sizeof buf, "%s fog fully opaque at %s units (%s m; it was %.0f%% at %s)", kind, fmt_num(no).c_str(),
                     fmt_num(no * 3.048).c_str(), density * 100, fmt_num(opaque).c_str());
            what += (what.empty() ? "" : ", ") + std::string(buf);
        }
        if (what.empty() || !et.save(ed / from_u8(sky))) continue;
        changed.push_back(fs::path(from_u8(sky)).stem().u8string() + ": " + what);
    }
    if (!changed.empty()) {
        std::string list;
        for (auto &c : changed) list += (list.empty() ? "" : "; ") + c;
        job.note("[fog] made the level's fog fully opaque, so the Xbox draws nothing past it (it moves the view's far end "
                 "there only at full density): " + list);
    }
}

// Tiny scenery and devices not drawn (the level window's choice): the engine draws nothing of a model smaller on
// screen than its smallest switch size ("super low", the bounding sphere's pixels across), so that's raised to the
// choice. A size, not a distance: big things stay drawn as far as anyone can see them, and what goes is specks.
// (The engine measures an object's size from how far ahead of the camera it is, not how far away, so a cutoff at a
// distance pops things in and out at the screen's edges as the camera turns: Bungie's own scenery never vanishes.
// Halo 1 Unlimited halves every size in widescreen, so there things go at twice the size.) Vehicles, characters and
// weapons are left alone (players need to see them).
void xbox_scenery(Job &job, const std::vector<std::string> &deps) {
    const double px = job.cfg.xbox_scenery_pixels;
    if (px <= 0 || job.analysis) return;
    std::string scen = job.scenario + ".scenario";
    fs::path sdir = job.owner(scen);
    tagfile::Tag st;
    if (sdir.empty() || !st.load(sdir / from_u8(scen))) return;
    std::set<std::string> first_person;
    std::map<std::string, std::string> names;
    std::map<std::string, char> kinds = model_kinds(job, deps, first_person, names);
    std::set<std::string> models;   // the scenery and devices the map places
    for (auto &[o, n] : object_counts(job, st.root())) {
        if (!(has_suffix(o, ".scenery") || has_suffix(o, ".device_machine") || has_suffix(o, ".device_control") ||
              has_suffix(o, ".device_light_fixture")))
            continue;
        fs::path own = job.owner(o);
        tagfile::Tag ot;
        if (own.empty() || !ot.load(own / from_u8(o))) continue;
        std::string m = ot.root().ref("model");
        if (!m.empty()) models.insert(m);
    }
    std::vector<std::string> done;
    for (auto &m : models) {
        if (!kinds.count(m) || kinds[m] != 's' || first_person.count(m)) continue;
        fs::path own = job.owner(m);
        if (own.empty()) continue;
        perf::ModelStats ms = perf::model_stats(own / from_u8(m));
        if (!ms.ok || ms.cutoff[4] >= px) continue;
        fs::path ed = editable_owner(job, m, own == job.overlay);
        if (ed.empty()) continue;
        bool ok = false;
        edit_lines(job, ed, m, {"-S", "super_low_detail_cutoff", std::to_string(px)}, &ok);
        if (ok) done.push_back((names.count(m) ? names[m] : fs::path(from_u8(m)).stem().u8string()));
    }
    if (!done.empty()) {
        std::string list;
        for (size_t i = 0; i < done.size() && i < 10; i++) list += (i ? ", " : "") + done[i];
        if (done.size() > 10) list += " and " + std::to_string(done.size() - 10) + " more";
        job.note("[scenery] scenery and devices aren't drawn once smaller on screen than " + fmt_num(px) + " pixels across (" +
                 fmt_num(px * 2) + " in widescreen; " + std::to_string(done.size()) + (done.size() == 1 ? " model's" : " models'") +
                 " smallest switch size raised to it): " + list);
    }
}

// The LODs window's models (job.lod_changes): how many are on the map, their switch distances and size, and each
// detail level drawn (levels that use the same version share a drawing).
static LodView make_lod_view(Job &job, const std::string &map_name) {
    LodView v;
    v.map_name = map_name;
    std::map<std::string, int> per_model;              // model -> on the map
    std::map<std::string, double> radius_of;           // model -> its object's bounding radius
    std::string scen = job.scenario + ".scenario";
    fs::path sdir = job.owner(scen);
    tagfile::Tag st;
    if (!sdir.empty() && st.load(sdir / from_u8(scen)))
        for (auto &[o, n] : object_counts(job, st.root())) {
            fs::path own = job.owner(o);
            tagfile::Tag ot;
            if (own.empty() || !ot.load(own / from_u8(o))) continue;
            std::string m = ot.root().ref("model");
            if (m.empty()) continue;
            per_model[m] += n;
            radius_of[m] = std::max(radius_of[m], ot.root().num("bounding radius"));
        }
    preview::Find find = [&](const std::string &tag) {
        fs::path o = job.owner(tag);
        return o.empty() ? fs::path() : o / from_u8(tag);
    };
    for (const LodView::Model &c : job.lod_changes) {
        LodView::Model m = c;
        m.placed = per_model.count(m.tag) ? per_model[m.tag] : 0;
        fs::path mf = find(m.tag);
        perf::ModelStats ms = mf.empty() ? perf::ModelStats() : perf::model_stats(mf);
        for (int i = 0; i < 5; i++) m.cutoffs[i] = ms.cutoff[i];
        m.radius = radius_of.count(m.tag) ? radius_of[m.tag] : 0;
        std::vector<std::vector<int>> seen;   // each drawing's geometries
        std::vector<int> index;
        for (int lv = 0; lv < 5; lv++) {
            std::vector<int> geos;
            ModelPreview p;
            bool ok = preview::build(m.tag, find, p, 128, lv, &geos);
            auto it = std::find(seen.begin(), seen.end(), geos);
            if (it != seen.end()) { m.level[lv] = index[it - seen.begin()]; continue; }
            m.level[lv] = ok ? (int)v.previews.size() : -1;
            seen.push_back(geos);
            index.push_back(m.level[lv]);
            if (ok) v.previews.push_back(std::move(p));
        }
        if (m.radius <= 0 && m.level[0] >= 0) {   // no bounding radius: the model's own size
            const ModelPreview &p = v.previews[m.level[0]];
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            for (size_t i = 0; i + 2 < p.pos.size(); i += 3)
                for (int k = 0; k < 3; k++) { lo[k] = std::min(lo[k], p.pos[i + k]); hi[k] = std::max(hi[k], p.pos[i + k]); }
            m.radius = 0.5 * std::sqrt((double)(hi[0] - lo[0]) * (hi[0] - lo[0]) + (double)(hi[1] - lo[1]) * (hi[1] - lo[1]) +
                                       (double)(hi[2] - lo[2]) * (hi[2] - lo[2]));
        }
        v.models.push_back(std::move(m));
    }
    return v;
}

// The report's "Performance" section: what the Xbox has to draw. The level: its triangles, and how many of them
// the engine draws (clusters and portals, and what's on screen) in a typical view and a busy one. The models:
// the ones with no lower-detail versions (drawn in full at any distance), most triangles on the map first, and the
// ones whose lower-detail versions are never used.
static std::vector<std::string> performance_lines(Job &job) {
    std::vector<std::string> lines;
    std::string scen = job.scenario + ".scenario";
    fs::path sdir = job.owner(scen);
    tagfile::Tag stag;
    if (sdir.empty() || !stag.load(sdir / from_u8(scen))) return lines;
    tagfile::Elem s = stag.root();

    tagfile::Block bsps = s.block("structure bsps");
    portals::PlayArea area = portals::play_area(sdir / from_u8(scen));   // (where players play: views from there)
    for (size_t i = 0; i < bsps.size(); i++) {
        std::string b = bsps[i].ref("structure bsp");
        fs::path own = b.empty() ? fs::path() : job.owner(b);
        if (own.empty()) continue;
        perf::BspStats st = perf::bsp_stats(own / from_u8(b));
        if (!st.ok) continue;
        std::string name = bsps.size() > 1 ? " (" + fs::path(from_u8(b)).stem().u8string() + ")" : "";
        std::string l = "Level" + name + ": " + thousands(st.triangles) + " triangles";
        if (st.clusters <= 1 || st.portals == 0) l += ", no portals (everything in view is drawn, walls or not)";
        else l += ", " + thousands(st.clusters) + " clusters, " + thousands(st.portals) + (st.portals == 1 ? " portal" : " portals");
        // What's drawn from where players stand, looking around (portals.hpp: the engine's own visibility).
        portals::Drawn d;
        if (portals::estimate(own / from_u8(b), portals::Arrangement::Own, nullptr, nullptr, d, area) && d.samples > 0)
            l += ": about " + thousands((long long)d.median) + " drawn in a typical view, " + thousands((long long)d.high) + " in a busy one.";
        else if (st.clusters <= 1 || st.portals == 0) l += ".";
        else l += ". About " + thousands(st.median_visible) + " can be drawn from a typical spot, " + thousands(st.worst_visible) +
                  " from the worst one.";
        lines.push_back(l);
    }
    if (!lines.empty())
        lines.push_back("  Stock Xbox maps, typical / busy view (busier than 9 views in 10): Blood Gulch 588 / 2,033, "
                        "Damnation 419 / 2,134, Sidewinder 784 / 2,115, Prisoner (the most) 893 / 2,433.");
    // Its shaders: every level triangle is drawn twice or more (lightmap, textures, reflections).
    if (!job.shaders_line.empty()) {
        std::istringstream sl(job.shaders_line);
        for (std::string l; std::getline(sl, l);) lines.push_back(l);
    }

    std::map<std::string, int> placed = object_counts(job, s);

    struct M { int placed = 0; perf::ModelStats st; std::string object; };
    std::map<std::string, M> models;
    for (auto &[o, n] : placed) {
        fs::path own = job.owner(o);
        std::string m = own.empty() ? "" : perf::object_model(own / from_u8(o));
        if (m.empty()) continue;
        auto it = models.find(m);
        if (it == models.end()) {
            fs::path mown = job.owner(m);
            M x;
            if (!mown.empty()) x.st = perf::model_stats(mown / from_u8(m));
            x.object = o;
            it = models.emplace(m, x).first;
        }
        it->second.placed += n;
    }
    // Each model by the name of the object it's drawn for (as in the weapons & vehicles window), with the object's
    // folder when two share a name: "hull (destroyed)".
    std::map<std::string, int> stems;
    for (auto &[m, x] : models) stems[fs::path(from_u8(x.object)).stem().u8string()]++;
    auto label = [&](const std::string &m) {
        fs::path o = from_u8(models[m].object);
        std::string s = o.stem().u8string();
        if (stems[s] > 1) s += " (" + o.parent_path().filename().u8string() + ")";
        return s;
    };
    std::vector<std::pair<long long, std::string>> full, unused;
    for (auto &[m, x] : models) {
        if (!x.st.ok) continue;
        if (!x.st.lods && x.st.triangles >= 500) full.push_back({(long long)x.st.triangles * x.placed, m});
        else if (x.st.lods && !x.st.borrowed && x.st.no_cutoffs()) unused.push_back({(long long)x.st.triangles * x.placed, m});
    }
    std::sort(full.rbegin(), full.rend());
    std::sort(unused.rbegin(), unused.rend());
    if (!full.empty()) {
        lines.push_back("Models with no lower-detail versions (drawn in full at any distance), most triangles on the map first:");
        for (size_t i = 0; i < full.size() && i < 8; i++) {
            const M &x = models[full[i].second];
            lines.push_back("  " + label(full[i].second) + ": " + thousands(x.st.triangles) + " triangles x " +
                            std::to_string(x.placed) + " on the map");
        }
        if (full.size() > 8) lines.push_back("  and " + std::to_string(full.size() - 8) + " more");
    }
    if (!unused.empty()) {
        std::string l;
        for (size_t i = 0; i < unused.size() && i < 8; i++) l += (i ? ", " : "") + label(unused[i].second);
        if (unused.size() > 8) l += " and " + std::to_string(unused.size() - 8) + " more";
        lines.push_back("Lower-detail versions never used (no switch distances): " + l);
    }
    return lines;
}

// Re-encode every BC7 texture the map uses (Xbox/Custom Edition targets). Tags in the app's own
// scratch folders are converted in place; anything else (your tags folder) is copied to fixes first.
void convert_bc7_textures(Job &job) {
    job.log("Checking the map's textures...");
    std::string out;
    std::vector<std::string> cmd{u8(tool(job.cfg, "invader-dependency")), "-r"};
    for (auto &d : job.dirs()) cmd.insert(cmd.end(), {"-t", u8(d)});
    cmd.push_back(job.scenario + ".scenario");
    run_process(cmd, out);
    std::istringstream ss(out);
    std::string dep;
    int tags = 0, bitmaps = 0;
    while (std::getline(ss, dep)) {
        dep = norm_rel(dep);
        if (dep.size() < 7 || dep.compare(dep.size() - 7, 7, ".bitmap") != 0) continue;
        fs::path own = job.owner(dep);
        if (own.empty() || own == job.overlay || convert_bc7_bitmap_tag(own / from_u8(dep), false) <= 0) continue;
        bool ours = own == job.fixes || (own == job.source && job.source != job.user_tags);
        if (!ours) {
            if (!job.to_fixes(dep, own)) continue;
            own = job.fixes;
        }
        int n = convert_bc7_bitmap_tag(own / from_u8(dep), true);
        if (n > 0) { tags++; bitmaps += n; }
    }
    if (tags)
        job.note("[bc7] re-encoded " + std::to_string(bitmaps) + " BC7 textures in " + std::to_string(tags) +
                 " bitmaps as DXT (" + game_name(job.target) + " can't use MCC's BC7 format)");
}

// ---------------------------------------------------------------- Xbox

std::string norm_rel(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

// ---------------------------------------------------------------- script conversion
// Scripts are compiled for one engine. Functions/globals the target engine doesn't have are swapped
// for an equivalent where there is one, or a stand-in of the same type (void calls become a no-op).
#include "script_compat.inc"

static unsigned engine_bit(Game g) { return g == Game::Xbox ? 1 : g == Game::CE ? 2 : g == Game::MCC ? 4 : 7; }

namespace hsc {
struct Node {
    bool list = false, str = false;
    size_t b = 0, e = 0;             // span in the source text
    std::string atom;                // atoms/strings
    std::vector<Node> kids;
};

// Parses HSC text into nodes (comments and whitespace stay in the text between spans).
static bool parse(const std::string &s, size_t &i, std::vector<Node> &out, bool nested) {
    while (i < s.size()) {
        char c = s[i];
        if (isspace((unsigned char)c)) { i++; continue; }
        if (c == ';') {
            if (i + 1 < s.size() && s[i + 1] == '*') {
                size_t end = s.find("*;", i + 2);
                i = end == std::string::npos ? s.size() : end + 2;
            } else {
                while (i < s.size() && s[i] != '\n') i++;
            }
            continue;
        }
        if (c == ')') {
            if (!nested) return false;
            return true;
        }
        Node n;
        n.b = i;
        if (c == '(') {
            n.list = true;
            i++;
            if (!parse(s, i, n.kids, true) || i >= s.size()) return false;
            i++; // ')'
        } else if (c == '"') {
            n.str = true;
            size_t end = s.find('"', i + 1);
            if (end == std::string::npos) return false;
            i = end + 1;
            n.atom = s.substr(n.b, i - n.b);
        } else {
            while (i < s.size() && !isspace((unsigned char)s[i]) && s[i] != '(' && s[i] != ')' && s[i] != ';' && s[i] != '"') i++;
            n.atom = s.substr(n.b, i - n.b);
        }
        n.e = i;
        out.push_back(std::move(n));
    }
    return !nested;
}

static std::string lower(std::string s) {
    for (auto &c : s) c = (char)tolower((unsigned char)c);
    return s;
}
} // namespace hsc

// The .hsc file, line and column an Invader script error points at.
static bool script_error_place(Job &job, const std::string &err, fs::path &file, int &line, int &col) {
    static const std::regex at(R"(([^\s:]+\.hsc):(\d+):(\d+):)");
    std::smatch m;
    if (!std::regex_search(err, m, at)) return false;
    std::string name = m[1].str();
    line = std::stoi(m[2]);
    col = std::stoi(m[3]);
    for (size_t k = 0; k + 1 < job.extra.size(); k++) {
        if (job.extra[k] != "-d") continue;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(from_u8(job.extra[k + 1]), ec); it != fs::recursive_directory_iterator(); it.increment(ec))
            if (it->path().filename().u8string() == name) { file = it->path(); return true; }
    }
    return false;
}

static std::string script_error_line(Job &job, const std::string &err) {
    fs::path file;
    int line = 0, col = 0;
    if (!script_error_place(job, err, file, line, col)) return "";
    std::ifstream in(file, std::ios::binary);
    std::string l;
    for (int i = 1; std::getline(in, l); i++)
        if (i == line) {
            while (!l.empty() && (l.back() == '\r' || l.back() == ' ' || l.back() == '\t')) l.pop_back();
            size_t a = l.find_first_not_of(" \t");
            return a == std::string::npos ? "" : l.substr(a, 300);
        }
    return "";
}

// MCC scripts can pass "none" as a team (no team); the original engine only parses team names. Use unused9, a
// team nothing is on (allying with it, or comparing with it, does nothing). Fixes the spot the error points at
// and every other call of the same function with "none" in the same place, then the build is tried again.
static bool fix_script_none(Job &job, const std::string &err, std::string &what) {
    static const std::regex tok(R"(cannot parse token '([^']+)' as team)");
    std::smatch m;
    if (!std::regex_search(err, m, tok) || hsc::lower(m[1].str()) != "none") return false;
    fs::path file;
    int line = 0, col = 0;
    if (!script_error_place(job, err, file, line, col)) return false;
    std::ifstream in(file, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    size_t off = 0;
    for (int l = 1; l < line && off < s.size(); off++) if (s[off] == '\n') l++;
    off += col > 0 ? col - 1 : 0;
    std::vector<hsc::Node> top;
    size_t i = 0;
    if (!hsc::parse(s, i, top, false)) return false;
    // The list holding the "none" at (or right around) that spot.
    const hsc::Node *parent = nullptr;
    size_t index = 0;
    std::function<void(const std::vector<hsc::Node> &, const hsc::Node *)> find = [&](const std::vector<hsc::Node> &ns, const hsc::Node *p) {
        for (size_t k = 0; k < ns.size() && !parent; k++) {
            const hsc::Node &n = ns[k];
            if (n.list) { if (n.b <= off && off < n.e) find(n.kids, &n); }
            else if (!n.str && p && hsc::lower(n.atom) == "none" && n.b <= off + 2 && off < n.e + 2) { parent = p; index = k; }
        }
    };
    find(top, nullptr);
    if (!parent || parent->kids.empty() || parent->kids[0].list) return false;
    std::string head = hsc::lower(parent->kids[0].atom);
    std::string same1 = parent->kids.size() > 1 && !parent->kids[1].list ? hsc::lower(parent->kids[1].atom) : "";
    std::vector<size_t> spots;
    std::function<void(const std::vector<hsc::Node> &)> walk = [&](const std::vector<hsc::Node> &ns) {
        for (auto &n : ns) {
            if (!n.list) continue;
            walk(n.kids);
            if (n.kids.size() <= index || n.kids[0].list || hsc::lower(n.kids[0].atom) != head) continue;
            const hsc::Node &a = n.kids[index];
            if (a.list || a.str || hsc::lower(a.atom) != "none") continue;
            // (set g none) / (global team g none): only the same global / team globals.
            if ((head == "set" || head == "global") && (n.kids.size() < 2 || n.kids[1].list || hsc::lower(n.kids[1].atom) != same1)) continue;
            spots.push_back(a.b);
        }
    };
    walk(top);
    if (spots.empty()) return false;
    std::sort(spots.rbegin(), spots.rend());
    spots.erase(std::unique(spots.begin(), spots.end()), spots.end());
    for (size_t p : spots) s.replace(p, 4, "unused9");
    std::ofstream(file, std::ios::binary) << s;
    what = "MCC's \"none\" team isn't valid on " + std::string(game_name(job.target)) + ": used unused9 (a team nothing is on) in " +
           std::to_string(spots.size()) + " place" + (spots.size() == 1 ? "" : "s") + " (" + head + ")";
    return true;
}

// A script named like one of the game's functions (Dogs of War has a continuous script "ai_go_to_vehicle" that
// calls the function ai_go_to_vehicle): Halo's own compiler took a call with arguments for the function, Invader
// takes the name for the script, which has none ("takes at most 0 parameter(s)"). The script gets a name of its own
// (calls of it without arguments follow), and the build is tried again.
static bool fix_script_shadow(Job &job, const std::string &err, std::string &what) {
    static const std::regex tok(R"(function '([^']+)' takes at most 0 parameter\(s\) but extraneous)");
    std::smatch m;
    if (!std::regex_search(err, m, tok)) return false;
    std::string name = hsc::lower(m[1].str());
    fs::path file;
    int line = 0, col = 0;
    if (!script_error_place(job, err, file, line, col)) return false;
    std::ifstream in(file, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::vector<hsc::Node> top;
    size_t i = 0;
    if (!hsc::parse(s, i, top, false)) return false;
    std::set<std::string> taken;
    const hsc::Node *def = nullptr;
    for (auto &n : top) {
        if (!n.list || n.kids.size() < 3 || n.kids[0].list || hsc::lower(n.kids[0].atom) != "script") continue;
        // (script <kind> <name> ...) or (script static|stub <type> <name> ...)
        std::string kind = n.kids[1].list ? "" : hsc::lower(n.kids[1].atom);
        size_t at = (kind == "static" || kind == "stub") && n.kids.size() > 3 ? 3 : 2;
        if (n.kids[at].list) continue;
        std::string nm = hsc::lower(n.kids[at].atom);
        taken.insert(nm);
        if (nm == name && !def) def = &n.kids[at];
    }
    if (!def) return false;
    std::string renamed = name + "_script";
    for (int k = 2; taken.count(renamed); k++) renamed = name + "_script" + std::to_string(k);
    // the definition, and calls of it with no arguments: (name)
    std::vector<std::pair<size_t, size_t>> spots{{def->b, def->e - def->b}};
    std::function<void(const std::vector<hsc::Node> &)> walk = [&](const std::vector<hsc::Node> &ns) {
        for (auto &n : ns) {
            if (!n.list) continue;
            if (n.kids.size() == 1 && !n.kids[0].list && !n.kids[0].str && hsc::lower(n.kids[0].atom) == name)
                spots.push_back({n.kids[0].b, n.kids[0].e - n.kids[0].b});
            walk(n.kids);
        }
    };
    walk(top);
    std::sort(spots.rbegin(), spots.rend());
    spots.erase(std::unique(spots.begin(), spots.end()), spots.end());
    for (auto &sp : spots) s.replace(sp.first, sp.second, renamed);
    std::ofstream(file, std::ios::binary) << s;
    what = "the map's script \"" + name + "\" has the name of a game function its own lines call: renamed it \"" +
           renamed + "\"";
    return true;
}

// A nav point type (a waypoint arrow of the HUD globals, e.g. Coldsnap's "orange_landingpad") that the built map's
// HUD globals doesn't have. merge_map_globals adds the map's own arrows to the Xbox's HUD globals; this is for
// what that can't cover (no room for more arrows, or the map's HUD globals is missing). The calls naming it use
// "default" (the plain arrow) instead, then the build is tried again.
static bool fix_script_navpoint(Job &job, const std::string &err, std::string &what) {
    static const std::regex tok(R"(can't find navpoint '([^']+)')");
    std::smatch m;
    if (!std::regex_search(err, m, tok)) return false;
    std::string name = hsc::lower(m[1].str());
    if (name == "default") return false;
    fs::path file;
    int line = 0, col = 0;
    if (!script_error_place(job, err, file, line, col)) return false;
    std::ifstream in(file, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    size_t off = 0;
    for (int l = 1; l < line && off < s.size(); off++) if (s[off] == '\n') l++;
    off += col > 0 ? col - 1 : 0;
    std::vector<hsc::Node> top;
    size_t i = 0;
    if (!hsc::parse(s, i, top, false)) return false;
    auto text = [](const hsc::Node &n) {
        std::string t = n.atom;
        if (n.str && t.size() >= 2) t = t.substr(1, t.size() - 2);
        return hsc::lower(t);
    };
    // Every nav point call's first argument with that name ((activate_team_nav_point_flag orange_landingpad ...)),
    // and the spot the error points at.
    std::vector<std::pair<size_t, size_t>> spots;
    std::function<void(const std::vector<hsc::Node> &)> walk = [&](const std::vector<hsc::Node> &ns) {
        for (auto &n : ns) {
            if (!n.list) {
                if (n.b <= off && off < n.e && text(n) == name) spots.push_back({n.b, n.e - n.b});
                continue;
            }
            walk(n.kids);
            if (n.kids.size() < 2 || n.kids[0].list || n.kids[1].list) continue;
            if (hsc::lower(n.kids[0].atom).find("nav_point") == std::string::npos || text(n.kids[1]) != name) continue;
            spots.push_back({n.kids[1].b, n.kids[1].e - n.kids[1].b});
        }
    };
    walk(top);
    if (spots.empty()) return false;
    std::sort(spots.rbegin(), spots.rend());
    spots.erase(std::unique(spots.begin(), spots.end()), spots.end());
    for (auto &sp : spots) s.replace(sp.first, sp.second, "default");
    std::ofstream(file, std::ios::binary) << s;
    what = "the nav point arrow \"" + m[1].str() + "\" isn't in this build's HUD, so the " + std::to_string(spots.size()) +
           " nav point" + (spots.size() == 1 ? "" : "s") + " the scripts set with it use the plain arrow (\"default\")";
    return true;
}

struct ScriptConverter {
    Game target;
    unsigned bit;
    std::set<std::string> user;                        // the map's own scripts and globals
    std::map<std::string, const ScriptName *> missing; // names the target doesn't have
    std::map<std::string, std::pair<int, std::string>> changes; // name -> (count, what happened)
    // Scripts with parameters (MCC only): name -> [(type, param name, global that stands in for it)]
    struct Param { std::string type, name, global; };
    std::map<std::string, std::vector<Param>> param_scripts;
    std::map<std::string, std::string> param_returns;  // such a script's return type
    const std::map<std::string, std::string> *subst = nullptr; // param name -> global, inside such a script
    int param_globals = 0;
    std::string extra_globals; // temporaries for nested calls, declared at the top of the file

    static std::string default_value(const std::string &type) {
        static const std::map<std::string, std::string> defaults{
            {"real", "0"}, {"short", "0"}, {"long", "0"}, {"boolean", "false"}, {"string", "\"\""},
            {"team", "player"}, {"game_difficulty", "normal"}, {"navpoint", "default"}};
        auto d = defaults.find(type);
        return d == defaults.end() ? "none" : d->second;
    }
    bool calls_param_script(const hsc::Node &n) const {
        if (!n.list) return false;
        if (!n.kids.empty() && !n.kids[0].list && param_scripts.count(hsc::lower(n.kids[0].atom))) return true;
        for (auto &k : n.kids)
            if (calls_param_script(k)) return true;
        return false;
    }

    explicit ScriptConverter(Game t) : target(t), bit(engine_bit(t)) {
        for (auto &n : kScriptNames)
            if (!(n.engines & bit)) missing[hsc::lower(n.name)] = &n;
    }

    void collect_user_names(const std::string &s) {
        std::vector<hsc::Node> top;
        size_t i = 0;
        if (!hsc::parse(s, i, top, false)) return;
        for (auto &n : top) {
            if (!n.list || n.kids.size() < 3 || n.kids[0].list) continue;
            std::string h = hsc::lower(n.kids[0].atom);
            const hsc::Node *name = nullptr;
            std::string ret;   // a static script's return type
            if (h == "global") name = &n.kids[2];
            else if (h == "script") {
                std::string type = hsc::lower(n.kids[1].atom);
                size_t at = (type == "static" || type == "stub") ? 3 : 2;
                if (n.kids.size() > at) name = &n.kids[at];
                if (at == 3 && !n.kids[2].list) ret = hsc::lower(n.kids[2].atom);
            }
            if (name && name->list && !name->kids.empty()) {
                // (script static void (name (type param) ...) ...): parameters, which only MCC supports
                const hsc::Node *decl = name;
                name = &name->kids[0];
                if (!(bit & 4) && !name->list && decl->kids.size() > 1) {
                    std::vector<Param> ps;
                    for (size_t k = 1; k < decl->kids.size(); k++) {
                        auto &pn = decl->kids[k];
                        if (!pn.list || pn.kids.size() != 2 || pn.kids[0].list || pn.kids[1].list) { ps.clear(); break; }
                        ps.push_back({hsc::lower(pn.kids[0].atom), hsc::lower(pn.kids[1].atom), ""});
                    }
                    if (!ps.empty()) {
                        param_scripts[hsc::lower(name->atom)] = ps;
                        if (!ret.empty()) param_returns[hsc::lower(name->atom)] = ret;
                    }
                }
            }
            if (name && !name->list) user.insert(hsc::lower(name->atom));
        }
    }

    static const char *stand_in(char type) {
        switch (type) {
        case 'v': return "(not false)";     // a call with no effect
        case 'b': return "(not true)";      // false
        case 'o': return "(ai_actors none)"; // empty object list
        default: return "(+ 0 0)";          // 0
        }
    }

    void count(const std::string &name, const std::string &what) {
        auto &c = changes[name];
        c.first++;
        c.second = what;
    }

    const ScriptName *lookup(const std::string &atom) {
        std::string l = hsc::lower(atom);
        if (user.count(l)) return nullptr;
        auto it = missing.find(l);
        return it == missing.end() ? nullptr : it->second;
    }

    std::string emit(const std::string &s, const hsc::Node &n) {
        if (!n.list) {
            if (n.str) return n.atom;
            if (subst) {
                auto it = subst->find(hsc::lower(n.atom));
                if (it != subst->end()) return it->second;
            }
            const ScriptName *g = lookup(n.atom);
            if (g && g->global) {
                count(hsc::lower(n.atom), std::string("read as ") + (g->type == 'b' ? "false" : "0"));
                return stand_in(g->type);
            }
            return s.substr(n.b, n.e - n.b);
        }
        // Rebuild the list from its (possibly rewritten) children, keeping spacing and comments.
        auto rebuild = [&](const std::string &new_head) {
            if (n.kids.empty()) return s.substr(n.b, n.e - n.b);
            std::string r = s.substr(n.b, n.kids[0].b - n.b);
            r += n.kids[0].list ? emit(s, n.kids[0]) : new_head;
            size_t pos = n.kids[0].e;
            for (size_t k = 1; k < n.kids.size(); k++) {
                r += s.substr(pos, n.kids[k].b - pos) + emit(s, n.kids[k]);
                pos = n.kids[k].e;
            }
            return r + s.substr(pos, n.e - pos);
        };
        if (n.kids.empty() || n.kids[0].list || n.kids[0].str) return rebuild("");
        std::string head = hsc::lower(n.kids[0].atom);
        if (!param_scripts.empty()) {
            // Definition: (script static void (name (type p) ...) body) -> parameters become globals
            if (head == "script" && n.kids.size() >= 3) {
                std::string type = n.kids[1].list ? "" : hsc::lower(n.kids[1].atom);
                size_t at = (type == "static" || type == "stub") ? 3 : 2;
                if (n.kids.size() > at && n.kids[at].list && !n.kids[at].kids.empty() && !n.kids[at].kids[0].list) {
                    auto ps = param_scripts.find(hsc::lower(n.kids[at].kids[0].atom));
                    if (ps != param_scripts.end()) {
                        std::map<std::string, std::string> m;
                        std::string decls;
                        for (auto &p : ps->second) {
                            m[p.name] = p.global;
                            decls += "(global " + p.type + " " + p.global + " " + default_value(p.type) + ")\n";
                        }
                        std::string r = decls + s.substr(n.b, n.kids[at].b - n.b) + n.kids[at].kids[0].atom;
                        size_t pos = n.kids[at].e;
                        subst = &m;
                        for (size_t k = at + 1; k < n.kids.size(); k++) {
                            r += s.substr(pos, n.kids[k].b - pos) + emit(s, n.kids[k]);
                            pos = n.kids[k].e;
                        }
                        subst = nullptr;
                        count(ps->first, "parameters turned into globals (not supported in " + std::string(game_name(target)) + ")");
                        return r + s.substr(pos, n.e - pos);
                    }
                }
            }
            // Call: (name a b) -> (begin (set global_a a) (set global_b b) (name))
            auto ps = param_scripts.find(head);
            if (ps != param_scripts.end() && n.kids.size() - 1 == ps->second.size()) {
                std::string r = "(begin";
                bool nested = false; // an argument calls a script with parameters: evaluate all arguments first
                for (size_t k = 1; k < n.kids.size(); k++) nested = nested || calls_param_script(n.kids[k]);
                std::vector<std::string> temps;
                for (size_t k = 1; k < n.kids.size(); k++) {
                    auto &p = ps->second[k - 1];
                    std::string dst = p.global;
                    if (nested) {
                        do dst = ("t" + std::to_string(++param_globals) + "_" + head + "_" + p.name).substr(0, 31);
                        while (user.count(dst));
                        user.insert(dst);
                        extra_globals += "(global " + p.type + " " + dst + " " + default_value(p.type) + ")\n";
                        temps.push_back(dst);
                    }
                    r += " (set " + dst + " " + emit(s, n.kids[k]) + ")";
                }
                for (size_t k = 0; k < temps.size(); k++) r += " (set " + ps->second[k].global + " " + temps[k] + ")";
                r += " (" + n.kids[0].atom + "))";
                // A (begin ...) has no type of its own. Where nothing around it gives one, as in the first operand
                // of (= ...) or (< ...), the compiled script keeps it untyped ("passthrough"), and the Xbox crashes
                // comparing an untyped value (Deathtrap's lightning: (= (modulo (game_time) 1800) 1740)). So the
                // value is passed through a function with a typed parameter of the script's return type.
                auto rt = param_returns.find(head);
                std::string t = rt == param_returns.end() ? "" : rt->second;
                if (t == "real" || t == "short" || t == "long") return "(+ 0 " + r + ")";
                if (t == "boolean") return "(and " + r + ")";
                if (t == "unit") return "(unit " + r + ")";
                if (t == "object") return "(list_get " + r + " 0)";
                return r;
            }
        }
        // (= (begin ...) 5): the same untyped first operand, written by the map's author (see above).
        static const std::set<std::string> compare{"=", "!=", "<", ">", "<=", ">="};
        if (compare.count(head) && n.kids.size() == 3 && n.kids[1].list && !n.kids[1].kids.empty() && !n.kids[1].kids[0].list) {
            static const std::set<std::string> untyped{"begin", "begin_random", "if", "cond"};
            static const std::set<std::string> numeric{"+", "-", "*", "/", "min", "max", "real_random_range", "random_range"};
            const hsc::Node &a = n.kids[1], &b = n.kids[2];
            std::string other = b.list ? (!b.kids.empty() && !b.kids[0].list ? hsc::lower(b.kids[0].atom) : "") : hsc::lower(b.atom);
            bool number = !b.list && !b.str && !other.empty() &&
                          other.find_first_not_of("0123456789.-") == std::string::npos && other.find_first_of("0123456789") != std::string::npos;
            std::string wrap = number || (b.list && numeric.count(other)) ? "(+ 0 " : (other == "true" || other == "false") ? "(and " : "";
            if (untyped.count(hsc::lower(a.kids[0].atom)) && !wrap.empty()) {
                count(head, "untyped first operand given a type (the Xbox crashes comparing an untyped value)");
                std::string r = s.substr(n.b, n.kids[0].e - n.b);
                r += s.substr(n.kids[0].e, a.b - n.kids[0].e) + wrap + emit(s, a) + ")";
                r += s.substr(a.e, b.b - a.e) + emit(s, b);
                return r + s.substr(b.e, n.e - b.e);
            }
        }
        // (set <global the target doesn't have> value) -> no-op
        if (head == "set" && n.kids.size() >= 2 && !n.kids[1].list) {
            const ScriptName *g = lookup(n.kids[1].atom);
            if (g && g->global) {
                count(hsc::lower(n.kids[1].atom), "setting it removed");
                return stand_in('v');
            }
        }
        const ScriptName *f = lookup(head);
        if (!f || f->global) return rebuild(n.kids[0].atom);

        // Equivalents the target has.
        auto has = [&](const std::string &name) { return missing.find(name) == missing.end(); };
        static const std::map<std::string, std::pair<std::string, std::string>> renames{
            {"player_effect_set_max_vibrate", {"player_effect_set_max_rumble", "renamed to player_effect_set_max_rumble"}},
            {"breadcrumbs_activate_team_nav_point_flag", {"activate_team_nav_point_flag", "now activate_team_nav_point_flag"}},
            {"breadcrumbs_activate_team_nav_point_object", {"activate_team_nav_point_object", "now activate_team_nav_point_object"}},
            {"breadcrumbs_deactivate_team_nav_point_flag", {"deactivate_team_nav_point_flag", "now deactivate_team_nav_point_flag"}},
            {"breadcrumbs_deactivate_team_nav_point_object", {"deactivate_team_nav_point_object", "now deactivate_team_nav_point_object"}},
            {"game_time_authoritative", {"game_time", "now game_time"}},
            {"local_players", {"players", "now players (all players)"}},
            {"list_count_not_dead", {"list_count", "now list_count (also counts dead ones)"}},
        };
        auto rn = renames.find(head);
        if (rn != renames.end() && has(rn->second.first)) {
            count(head, rn->second.second);
            return rebuild(rn->second.first);
        }
        std::vector<std::string> args;
        for (size_t k = 1; k < n.kids.size(); k++) args.push_back(emit(s, n.kids[k]));
        size_t argc = args.size();
        if (head == "sleep_forever" && argc <= 1) {
            count(head, "now (sleep -1)");
            return "(sleep -1" + (argc ? " " + args[0] : std::string()) + ")";
        }
        if (head == "pin" && argc == 3 && has("min") && has("max")) {
            count(head, "now (min (max ...))");
            return "(min (max " + args[0] + " " + args[1] + ") " + args[2] + ")";
        }
        if ((head == "abs_real" || head == "abs_integer") && argc == 1 && has("max")) {
            count(head, "now (max x (* -1 x))");
            return "(max " + args[0] + " (* -1 " + args[0] + "))";
        }
        if (head == "game_is_authoritative" && argc == 0) {
            count(head, "always true");
            return "(not false)";
        }
        if (head.rfind("objects_distance_to_", 0) == 0) {
            count(head, "always 1000 (far away)");
            return "(+ 1000 0)";
        }
        if ((head == "sound_get_master_gain" || head == "sound_get_music_gain" || head == "sound_get_effects_gain" ||
             head == "physics_get_gravity") && argc == 0) {
            count(head, "always 1");
            return "(+ 1 0)";
        }
        switch (f->type) {
        case 'v': count(head, "removed"); break;
        case 'b': count(head, "always false"); break;
        case 'o': count(head, "always an empty list"); break;
        default: count(head, "always 0"); break;
        }
        return stand_in(f->type);
    }

    // Converts one file. Returns false if it can't be parsed (left unchanged).
    bool convert(std::string &text) {
        for (auto &[script, ps] : param_scripts)
            for (auto &p : ps)
                if (p.global.empty()) {
                    std::string g;
                    do g = ("p" + std::to_string(++param_globals) + "_" + script + "_" + p.name).substr(0, 31);
                    while (user.count(g));
                    p.global = g;
                    user.insert(g);
                }
        std::vector<hsc::Node> top;
        size_t i = 0;
        if (!hsc::parse(text, i, top, false)) return false;
        std::string out;
        size_t pos = 0;
        for (auto &n : top) {
            out += text.substr(pos, n.b - pos) + emit(text, n);
            pos = n.e;
        }
        out += text.substr(pos);
        text = extra_globals + out;
        extra_globals.clear();
        return true;
    }
};

// Convert every .hsc under `dir` for the target game; notes what changed.
static void convert_scripts(Job &job, const fs::path &dir, Game target) {
    if (!fs::exists(dir)) return;
    std::vector<fs::path> files;
    for (auto &e : fs::recursive_directory_iterator(dir))
        if (e.path().extension() == ".hsc") files.push_back(e.path());
    ScriptConverter conv(target);
    std::map<fs::path, std::string> texts;
    for (auto &f : files) {
        std::ifstream in(f, std::ios::binary);
        texts[f] = std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        conv.collect_user_names(texts[f]);
    }
    for (auto &[f, t] : texts) {
        std::string before = t;
        if (!conv.convert(t)) {
            job.note("[scripts] couldn't read " + f.filename().u8string() + " to check it for " + game_name(target));
            continue;
        }
        if (t != before) std::ofstream(f, std::ios::binary) << t;
    }
    for (auto &[name, c] : conv.changes)
        job.note("[scripts] " + name + " (not in " + game_name(target) + "): " + c.second + " (" +
                 std::to_string(c.first) + "x)");
}

// Prepare the user's/converted tags for Xbox: only tags the map uses are copied to a scratch
// folder, then models/shaders/HUDs/sounds/scripts are converted there. Stock Xbox tags (overlay)
// take precedence at build time.
// ---------------------------------------------------------------- HUD layout
static bool ends_with(const std::string &s, const std::string &suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

// Is the map's HUD laid out for MCC? MCC draws the HUD on a canvas twice the Xbox's size: its HUD tags
// (343's copies of the stock ones too) have every position doubled, and its stock HUD sheets are 4x the
// Xbox's resolution, flagged "half hud scale". Compare a stock HUD that both tag sets have. `tags` must be the
// map's own tags as extracted: the scratch copy can hold stock PC HUDs taken from Custom Edition's maps along with
// a borrowed vehicle (a rocket warthog), which match the Xbox's and made an MCC map look like a PC one.
static bool mcc_hud_layout(Job &job, const fs::path &tags) {
    const Config &cfg = job.cfg;
    static const std::pair<const char *, const char *> refs[] = {
        {"ui/hud/master rounds.weapon_hud_interface", "static_elements[0].anchor_offset"},
        {"ui/hud/master.weapon_hud_interface", "crosshairs[0].crosshair_overlays[0].anchor_offset"},
        {"ui/hud/cyborg.unit_hud_interface", "health_panel_meter_anchor_offset"}};
    auto get = [&](const fs::path &dir, const std::string &tag, const std::string &key) {
        std::string v;
        run_process({u8(tool(cfg, "invader-edit")), "-t", u8(dir), tag, "-G", key}, v);
        int x = 0, y = 0;
        return sscanf(v.c_str(), "%d,%d", &x, &y) == 2 ? std::make_pair(x, y) : std::make_pair(INT_MIN, INT_MIN);
    };
    if (!job.overlay.empty())
        for (auto &[tag, key] : refs) {
            if (!fs::exists(tags / from_u8(tag)) || !fs::exists(job.overlay / from_u8(tag))) continue;
            auto m = get(tags, tag, key), x = get(job.overlay, tag, key);
            if (m.first == INT_MIN || x.first == INT_MIN || (x.first == 0 && x.second == 0)) continue;
            if (m.first == 2 * x.first && m.second == 2 * x.second) return true;
            if (m == x) return false;
        }
    return job.from == Game::MCC;
}

// The bitmap a HUD element draws (element keys start with `base`): its own interface/meter bitmap, or for
// crosshair overlays, overlay items and a grenade HUD's overlays, their parent's. "" = none (numbers,
// the motion sensor centre).
static std::string hud_element_bitmap(const std::map<std::string, std::string> &vals, const std::string &base) {
    for (auto f : {"interface_bitmap", "meter_bitmap"}) {
        auto it = vals.find(base + f);
        if (it != vals.end()) return it->second;
    }
    std::smatch m;
    static const std::regex nested(R"(^(.*\]\.)[a-z_]+\[\d+\]\.$)");    // crosshairs[0].crosshair_overlays[1].
    if (std::regex_match(base, m, nested)) {
        std::string p = m[1];
        for (auto it = vals.lower_bound(p); it != vals.end() && it->first.compare(0, p.size(), p) == 0; ++it)
            if (it->first.find('.', p.size()) == std::string::npos && ends_with(it->first, "_bitmap")) return it->second;
    }
    static const std::regex grenade(R"(^([a-z_]+)s\[\d+\]\.$)");         // total_grenades_overlays[0].
    if (std::regex_match(base, m, grenade)) {
        auto it = vals.find(m[1].str() + "_bitmap");
        if (it != vals.end()) return it->second;
    }
    return "";
}

// The map's own HUD tags, sized and placed for the Xbox:
//  - "use high res scale" (on an element) and "half hud scale" / "force hud use highres scale" (on the bitmap
//    it draws) each halve an element on PC and MCC (once, whichever is set). The Xbox ignores both. Flagged
//    bitmaps used only by HUDs are shrunk to match (less texture memory too); otherwise the element's scale
//    is halved.
//  - MCC's double-size canvas: positions are halved, and so are the sizes of elements drawing the map's
//    bitmaps (numbers use the Xbox's own digits, which are already the right size). Elements MCC anchors to
//    the screen centre get an offset from the HUD's top-left corner instead.
void convert_hud_layout(Job &job, const fs::path &xsrc, const std::vector<std::string> &hud_tags) {
    if (hud_tags.empty()) return;
    const Config &cfg = job.cfg;
    const bool mcc = mcc_hud_layout(job, !job.user_tags.empty() ? job.user_tags : job.source);
    auto in_overlay = [&](const std::string &rel) { return !job.overlay.empty() && fs::exists(job.overlay / from_u8(rel)); };
    std::string out;

    struct Elem { std::string base, bitmap; bool flag; };
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> kvs;
    std::map<std::string, std::vector<Elem>> elems;
    std::map<std::string, bool> half;   // bitmap -> flagged "half hud scale" / "force hud use highres scale"
    for (auto &tag : hud_tags) {
        auto kv = list_values(cfg, xsrc, tag);
        std::map<std::string, std::string> vals(kv.begin(), kv.end());
        for (auto &[key, v] : kv) {
            const std::string suffix = "scaling_flags.use_high_res_scale";
            if (!ends_with(key, suffix)) continue;
            Elem e{key.substr(0, key.size() - suffix.size()), "", v == "1"};
            e.bitmap = norm_rel(hud_element_bitmap(vals, e.base));
            if (!e.bitmap.empty() && !ends_with(e.bitmap, ".bitmap")) e.bitmap += ".bitmap";
            if (!e.bitmap.empty() && !half.count(e.bitmap)) {
                fs::path dir = in_overlay(e.bitmap) ? job.overlay : xsrc;   // the copy the build uses
                bool f = false;
                for (auto k : {"flags.half_hud_scale", "flags.force_hud_use_highres_scale"}) {
                    std::string g;
                    run_process({u8(tool(cfg, "invader-edit")), "-t", u8(dir), e.bitmap, "-G", k}, g);
                    f = f || g.rfind("1", 0) == 0;
                }
                half[e.bitmap] = f;
            }
            elems[tag].push_back(e);
        }
        kvs[tag] = std::move(kv);
    }

    // Shrink flagged bitmaps that only HUDs use: once for the flag, once more for MCC's canvas.
    std::map<std::string, int> shrunk;
    int shrunk_count = 0;
    std::set<std::string> flagged;
    for (auto &[bm, f] : half)
        if (f && !in_overlay(bm) && fs::exists(xsrc / from_u8(bm))) flagged.insert(bm);
    auto users_of = find_referrers({xsrc}, flagged);
    for (auto &bm : flagged) {
        bool hud_only = true;
        for (auto &u : users_of[bm]) {
            std::string ext = fs::path(from_u8(u)).extension().u8string();
            if (ext != ".unit_hud_interface" && ext != ".weapon_hud_interface" && ext != ".grenade_hud_interface" &&
                ext != ".hud_globals")
                hud_only = false;
        }
        if (!hud_only) continue;
        int k = shrink_hud_bitmap_tag(xsrc / from_u8(bm), mcc ? 2 : 1);
        shrunk[bm] = k;
        if (k) shrunk_count++;
    }

    int scaled = 0, high_res = 0, moved = 0, centred = 0;
    for (auto &tag : hud_tags) {
        auto &kv = kvs[tag];
        std::map<std::string, std::string> vals(kv.begin(), kv.end());
        std::vector<std::pair<std::string, std::string>> sets;
        std::set<std::string> centre;   // element bases MCC anchors to the screen centre
        std::string anchor = vals.count("anchor.top_left") && vals["anchor.top_left"] == "*" ? "top_left" : "";
        for (auto &e : elems[tag]) {
            double factor = 1;
            if (e.flag) {
                sets.push_back({e.base + "scaling_flags.use_high_res_scale", "0"});
                high_res++;
            }
            if (!e.bitmap.empty()) {
                if (mcc) factor *= 0.5;
                if (e.flag || half[e.bitmap]) factor *= 0.5;
                factor *= 1 << shrunk[e.bitmap];
            } else if (e.flag) {
                factor = 0.5;
            }
            if (factor != 1) {
                bool any = false;
                for (auto s : {"width_scale", "height_scale"}) {
                    auto it = vals.find(e.base + s);
                    if (it == vals.end()) continue;
                    double v = std::atof(it->second.c_str());
                    if (v == 0) v = 1;
                    sets.push_back({e.base + s, std::to_string(v * factor)});
                    any = true;
                }
                if (any) scaled++;
            }
            if (mcc && vals.count(e.base + "anchor.center") && vals[e.base + "anchor.center"] == "*" && anchor == "top_left")
                centre.insert(e.base);
        }
        if (mcc)
            for (auto &[key, v] : kv) {
                if (!ends_with(key, "anchor_offset")) continue;
                int x = 0, y = 0;
                if (sscanf(v.c_str(), "%d,%d", &x, &y) != 2) continue;
                std::string base = key.substr(0, key.size() - 13);
                if (centre.count(base)) {   // centre of MCC's 1280x960 canvas, from the top-left corner
                    x += 640;
                    y += 480;
                    sets.push_back({base + "anchor", "from_parent"});
                    centred++;
                }
                sets.push_back({key, std::to_string((int)std::lround(x / 2.0)) + "," + std::to_string((int)std::lround(y / 2.0))});
                moved++;
            }
        if (sets.empty()) continue;
        std::vector<std::string> cmd{u8(tool(cfg, "invader-edit")), "-t", u8(xsrc), tag};
        for (auto &[k, v] : sets) cmd.insert(cmd.end(), {"-S", k, v});
        if (run_process(cmd, out) != 0)   // one bad key shouldn't lose the rest
            for (auto &[k, v] : sets) run_process({u8(tool(cfg, "invader-edit")), "-t", u8(xsrc), tag, "-S", k, v}, out);
    }
    if (mcc)
        job.note("[xbox] this map's HUD is laid out for MCC's double-size HUD: halved " + std::to_string(moved) +
                 " HUD positions and resized " + std::to_string(scaled) + " HUD elements to the Xbox's" +
                 (centred ? " (" + std::to_string(centred) + " anchored to the screen centre now count from the corner)" : ""));
    else if (high_res || scaled)
        job.note("[xbox] converted " + std::to_string(std::max(high_res, scaled)) + " HUD elements from PC high-res scaling");
    if (shrunk_count)
        job.note("[xbox] shrank " + std::to_string(shrunk_count) + " HUD bitmaps that " + (mcc ? "MCC" : "PC") +
                 " draws at half size to the size the Xbox draws them (and a quarter of the texture memory or less)");
}

// ---------------------------------------------------------------- the map's globals
static int string_count(const Config &cfg, const fs::path &dir, const std::string &tag) {
    std::string c;
    run_process({u8(tool(cfg, "invader-edit")), "-t", u8(dir), tag, "-C", "strings"}, c);
    return std::atoi(c.c_str());
}

// How wide a bitmap sequence's first picture is, in pixels (a sprite's share of its sheet, or the whole texture).
static double sequence_width(const BitmapParts &p, uint32_t seq) {
    if (seq >= p.nseq) return 0;
    auto bitmap_width = [&](int bi) { return bi >= 0 && bi < (int)p.nbd ? (double)be16(p.b, p.bd_off + 48 * (size_t)bi + 4) : 0.0; };
    if (p.sprites(seq)) {
        size_t so = p.sprite_at(seq);
        auto f = [&](size_t o) { uint32_t u = be32(p.b, o); float v; memcpy(&v, &u, 4); return (double)v; };
        return (f(so + 12) - f(so + 8)) * bitmap_width((int16_t)be16(p.b, so));
    }
    return bitmap_width((int16_t)be16(p.b, p.seq_off + 64 * (size_t)seq + 32));
}

// Nav point arrows (the HUD globals' waypoint arrows): scripts name them ((activate_team_nav_point_flag
// orange_landingpad player o_pad 5)), and a name the built map's HUD globals lacks stops the build. The map's arrows
// the Xbox's HUD globals doesn't have are added after its own (the Xbox's keep their numbers). Their pictures:
// when they use sequences past the end of the Xbox's arrow sheet, a copy of the Xbox's sheet gets the map's
// sequences from there on (drawn at the size the Xbox draws the Xbox's own arrows).
static void merge_nav_arrows(Job &job, const fs::path &xsrc, const std::vector<std::pair<std::string, std::string>> &mh,
                             const std::string &xhg, std::vector<std::string> &what) {
    const Config &cfg = job.cfg;
    fs::path xhdir = job.owner(xhg);
    if (xhdir.empty()) return;
    auto xh = list_values(cfg, xhdir, xhg);
    std::map<std::string, std::string> mhv(mh.begin(), mh.end()), xhv(xh.begin(), xh.end());
    static const std::regex field(R"(^waypoint_arrows\[(\d+)\]\.(.+)$)");
    auto arrows = [&](const std::vector<std::pair<std::string, std::string>> &kv) {
        std::map<int, std::vector<std::pair<std::string, std::string>>> a;   // index -> (field, value) in tag order
        std::smatch m;
        for (auto &[k, v] : kv)
            if (std::regex_match(k, m, field)) a[std::stoi(m[1])].push_back({m[2].str(), v});
        return a;
    };
    auto value_of = [](const std::vector<std::pair<std::string, std::string>> &fs_, const std::string &f) {
        for (auto &[k, v] : fs_)
            if (k == f) return v;
        return std::string();
    };
    auto ma = arrows(mh), xa = arrows(xh);
    if (xa.empty()) return;
    std::set<std::string> xnames;
    int plain = xa.begin()->first;   // the plain arrow ("default"): stands in for a picture the sheet lacks
    for (auto &[i, fs_] : xa) {
        xnames.insert(hsc::lower(value_of(fs_, "name")));
        if (hsc::lower(value_of(fs_, "name")) == "default") plain = i;
    }
    std::vector<int> add;
    for (auto &[i, fs_] : ma) {
        std::string n = hsc::lower(value_of(fs_, "name"));
        if (!n.empty() && !xnames.count(n)) {
            add.push_back(i);
            xnames.insert(n);
        }
    }
    if (add.empty()) return;
    const int kMaxArrows = 16;   // hud_globals' limit
    int xc = (int)xa.size(), room = std::max(0, kMaxArrows - xc);
    std::vector<std::string> left_out;
    while ((int)add.size() > room) {
        left_out.push_back(value_of(ma[add.back()], "name"));
        add.pop_back();
    }
    std::reverse(left_out.begin(), left_out.end());
    std::string out;
    auto seqs = [&](const fs::path &dir, const std::string &t) {
        std::string c;
        run_process({u8(tool(cfg, "invader-edit")), "-t", u8(dir), t, "-C", "bitmap_group_sequence"}, c);
        return std::atoi(c.c_str());
    };
    auto is_seq = [](const std::string &f) { return f.size() > 14 && f.compare(f.size() - 14, 14, "sequence_index") == 0; };

    // The sheet: how many sequences the arrows can use, and whether they need the map's.
    std::string mab = norm_rel(mhv["arrow_bitmap"]), xab = norm_rel(xhv["arrow_bitmap"]);
    fs::path xadir = xab.empty() ? fs::path() : job.owner(xab);
    int have = xadir.empty() ? 0 : seqs(xadir, xab), need = 0;
    for (int i : add)
        for (auto &[f, v] : ma[i])
            if (is_seq(f) && !v.empty() && std::atoi(v.c_str()) != 65535 && std::atoi(v.c_str()) >= 0)
                need = std::max(need, std::atoi(v.c_str()) + 1);
    std::string sheet_note;
    if (need > have && have > 0 && !mab.empty() && fs::exists(xsrc / from_u8(mab))) {
        int mseq = seqs(xsrc, mab);
        std::string merged = xab.substr(0, xab.size() - 7) + "_with_map.bitmap";
        BitmapParts mp, xp;
        if (mseq >= need && !fs::exists(job.overlay / from_u8(merged)) && mp.read(xsrc / from_u8(mab)) &&
            xp.read(xadir / from_u8(xab))) {
            // Draw the map's arrows at the size the Xbox draws its own: compare the plain arrow ("default") in
            // both sheets, and halve the map's sheet until they match.
            int shrink = 0;
            std::string dseq = value_of(xa[plain], "on_screen_sequence_index");
            if (!dseq.empty()) {
                double mw = sequence_width(mp, (uint32_t)std::atoi(dseq.c_str())), xw = sequence_width(xp, (uint32_t)std::atoi(dseq.c_str()));
                while (xw > 0 && mw > xw * 1.5 && shrink < 2) { mw /= 2; shrink++; }
            }
            fs::path tmp = job.work / "map_waypoints.bitmap";
            std::error_code ec;
            fs::copy_file(xsrc / from_u8(mab), tmp, fs::copy_options::overwrite_existing, ec);
            int got = !ec && shrink ? shrink_hud_bitmap_tag(tmp, shrink) : 0;
            if (!ec && got == shrink && merge_icon_sheets(xadir / from_u8(xab), tmp, (uint32_t)have, xsrc / from_u8(merged)) &&
                job.edit(xhg, "arrow_bitmap", merged)) {
                sheet_note = ", with their " + std::to_string(mseq - have) + " picture" + (mseq - have == 1 ? "" : "s") +
                             " added to the Xbox's arrow sheet";
                have = mseq;
            }
        }
    }

    // The arrows, after the Xbox's own.
    fs::path own = job.owner(xhg);
    if (own != job.fixes && !job.to_fixes(xhg, own)) return;
    if (run_process({u8(tool(cfg, "invader-edit")), "-t", u8(job.fixes), xhg, "-I", "waypoint_arrows",
                     std::to_string(add.size()), "end"}, out) != 0)
        return;
    std::vector<std::pair<std::string, std::string>> sets;
    std::vector<std::string> names;
    for (size_t n = 0; n < add.size(); n++) {
        std::string base = "waypoint_arrows[" + std::to_string(xc + (int)n) + "].";
        for (auto &[f, v] : ma[add[n]]) {
            if (v.empty()) continue;
            std::string val = v;
            // A picture the sheet doesn't have: the Xbox's plain arrow's.
            if (is_seq(f) && std::atoi(v.c_str()) != 65535 && std::atoi(v.c_str()) >= have) val = value_of(xa[plain], f);
            if (val == "*") {   // an enum's chosen option: set the enum to it
                size_t d = f.rfind('.');
                if (d != std::string::npos) sets.push_back({base + f.substr(0, d), f.substr(d + 1)});
                continue;
            }
            if (!val.empty()) sets.push_back({base + f, val});
        }
        names.push_back(value_of(ma[add[n]], "name"));
    }
    std::vector<std::string> cmd{u8(tool(cfg, "invader-edit")), "-t", u8(job.fixes), xhg};
    for (auto &[k, v] : sets) cmd.insert(cmd.end(), {"-S", k, v});
    if (run_process(cmd, out) != 0)   // one bad key shouldn't lose the rest
        for (auto &[k, v] : sets) run_process({u8(tool(cfg, "invader-edit")), "-t", u8(job.fixes), xhg, "-S", k, v}, out);
    std::string list;
    for (size_t n = 0; n < names.size(); n++) list += (n ? ", " : "") + names[n];
    what.push_back("its " + std::to_string(names.size()) + " nav point arrow" + (names.size() == 1 ? "" : "s") + " (" + list +
                   ", added after the Xbox's own" + sheet_note + ")");
    if (!left_out.empty()) {
        std::string lo;
        for (size_t n = 0; n < left_out.size(); n++) lo += (n ? ", " : "") + left_out[n];
        job.note("[xbox-mp] the HUD has room for " + std::to_string(kMaxArrows) + " nav point arrows, so the map's " + lo +
                 " didn't fit (scripts that use " + (left_out.size() == 1 ? "it" : "them") + " get the plain arrow)");
    }
}

// The Xbox's own globals (and HUD globals) win over the map's copies, which keeps the Xbox menus, fonts and
// multiplayer sounds. Bring over what is the map's own content:
//  - the HUD name lists, when the map's are longer (its custom weapons' pickup names and vehicles' names);
//  - its own nav point arrows (its scripts name them), after the Xbox's (merge_nav_arrows);
//  - unless "use the Xbox's own weapons" is on: its grenades, its weapon list (gametype weapon sets) and its
//    multiplayer player biped (players spawn with that biped's weapons), and its own tuning of fall damage
//    and the player (Custom Edition's stock values match the Xbox's, so a difference is the map's).
void merge_map_globals(Job &job, const fs::path &xsrc) {
    const Config &cfg = job.cfg;
    const std::string g = "globals/globals.globals";
    if (job.overlay.empty() || !fs::exists(xsrc / from_u8(g)) || !fs::exists(job.overlay / from_u8(g))) return;
    auto in_overlay = [&](const std::string &rel) { return fs::exists(job.overlay / from_u8(rel)); };
    auto mkv = list_values(cfg, xsrc, g);
    std::map<std::string, std::string> mv(mkv.begin(), mkv.end());
    fs::path gdir = fs::exists(job.fixes / from_u8(g)) ? job.fixes : job.overlay;
    auto xkv = list_values(cfg, gdir, g);
    std::map<std::string, std::string> xv(xkv.begin(), xkv.end());
    std::string out;
    std::vector<std::pair<std::string, std::string>> sets;
    std::vector<std::string> what;

    // 1. HUD name lists.
    std::string mhg = mv["interface_bitmaps[0].hud_globals"], xhg = xv["interface_bitmaps[0].hud_globals"];
    if (!mhg.empty() && !xhg.empty() && fs::exists(xsrc / from_u8(mhg))) {
        auto mh = list_values(cfg, xsrc, mhg);
        std::map<std::string, std::string> mhv(mh.begin(), mh.end());
        fs::path xhdir = job.owner(xhg);
        if (!xhdir.empty()) {
            auto xh = list_values(cfg, xhdir, xhg);
            std::map<std::string, std::string> xhv(xh.begin(), xh.end());
            for (auto [key, label] : {std::pair<const char *, const char *>{"item_message_text", "weapon and item pickup names"},
                                      {"alternate_icon_text", "vehicle names"}}) {
                std::string ml = mhv[key], xl = xhv[key];
                if (ml.empty() || !fs::exists(xsrc / from_u8(ml))) continue;
                fs::path xldir = xl.empty() ? fs::path() : job.owner(xl);
                int mc = string_count(cfg, xsrc, ml), xc = xldir.empty() ? 0 : string_count(cfg, xldir, xl);
                if (mc <= xc) continue;
                bool ok = true;
                if (in_overlay(ml)) ok = job.to_fixes(ml, xsrc);   // a stock path: the map's version wins
                if (ok && ml != xl) ok = job.edit(xhg, key, ml);
                if (ok) what.push_back("its " + std::string(label) + " (" + std::to_string(mc) + ", the Xbox has " + std::to_string(xc) + ")");
            }
            // Pickup icons: when the map's icon sheet has more than the Xbox's, use a sheet with both: the Xbox's
            // own icons and button glyphs first, then the map's extra icons, shrunk to the size the Xbox draws.
            std::string mi = norm_rel(mhv["icon_bitmap"]), xi = norm_rel(xhv["icon_bitmap"]);
            if (!mi.empty() && !xi.empty() && fs::exists(xsrc / from_u8(mi)) && fs::exists(job.overlay / from_u8(xi))) {
                auto seqs = [&](const fs::path &dir, const std::string &t) {
                    std::string c;
                    run_process({u8(tool(cfg, "invader-edit")), "-t", u8(dir), t, "-C", "bitmap_group_sequence"}, c);
                    return std::atoi(c.c_str());
                };
                int mc = seqs(xsrc, mi), xc = seqs(job.overlay, xi);
                if (mc > xc && xc > 0) {
                    bool flagged = false;
                    for (auto k : {"flags.half_hud_scale", "flags.force_hud_use_highres_scale"}) {
                        std::string v;
                        run_process({u8(tool(cfg, "invader-edit")), "-t", u8(xsrc), mi, "-G", k}, v);
                        flagged = flagged || v.rfind("1", 0) == 0;
                    }
                    int want = (flagged ? 1 : 0) + (mcc_hud_layout(job, !job.user_tags.empty() ? job.user_tags : job.source) ? 1 : 0);
                    fs::path tmp = job.work / "map_icons.bitmap";
                    std::error_code ec;
                    fs::copy_file(xsrc / from_u8(mi), tmp, fs::copy_options::overwrite_existing, ec);
                    int got = !ec && want ? shrink_hud_bitmap_tag(tmp, want) : 0;
                    std::string merged = xi.substr(0, xi.size() - 7) + "_with_map.bitmap";
                    if (!ec && got == want && !in_overlay(merged) &&
                        merge_icon_sheets(job.overlay / from_u8(xi), tmp, (uint32_t)xc, xsrc / from_u8(merged)) &&
                        job.edit(xhg, "icon_bitmap", merged))
                        what.push_back("its " + std::to_string(mc - xc) + " extra pickup icons (added after the Xbox's own)");
                }
            }
            merge_nav_arrows(job, xsrc, mh, xhg, what);
        }
    }

    if (!cfg.xbox_use_stock_versions) {
        // 2. Grenades.
        for (int i = 0; i < 2; i++) {
            std::string p = "grenades[" + std::to_string(i) + "].";
            if (mv[p + "equipment"] == xv[p + "equipment"] && mv[p + "projectile"] == xv[p + "projectile"]) continue;
            if (mv[p + "equipment"].empty()) continue;
            for (auto f : {"maximum_count", "mp_spawn_default", "throwing_effect", "hud_interface", "equipment", "projectile"})
                if (mv.count(p + f) && xv.count(p + f) && mv[p + f] != xv[p + f]) sets.push_back({p + f, mv[p + f]});
            what.push_back("its " + fs::path(from_u8(mv[p + "equipment"])).stem().u8string() + " grenades");
        }
        // 3. Weapon list, when it has the map's own weapons.
        static const std::regex wl(R"(^weapon_list\[(\d+)\]\.weapon$)");
        std::vector<std::string> list;
        bool custom = false;
        for (auto &[k, v] : mkv) {
            std::smatch m;
            if (!std::regex_match(k, m, wl)) continue;
            list.push_back(v);
            if (!v.empty() && !in_overlay(v)) custom = true;
        }
        if (custom) {
            int xn = 0;
            for (auto &[k, v] : xkv) xn += std::regex_match(k, wl) ? 1 : 0;
            int mn = (int)list.size();
            if (!fs::exists(job.fixes / from_u8(g))) job.to_fixes(g, job.overlay);
            if (mn > xn)
                run_process({u8(tool(cfg, "invader-edit")), "-t", u8(job.fixes), g, "-I", "weapon_list", std::to_string(mn - xn), "end"}, out);
            else if (mn < xn)
                run_process({u8(tool(cfg, "invader-edit")), "-t", u8(job.fixes), g, "-E",
                             "weapon_list[" + std::to_string(mn) + "-" + std::to_string(xn - 1) + "]"}, out);
            for (int i = 0; i < mn; i++) sets.push_back({"weapon_list[" + std::to_string(i) + "].weapon", list[i]});
            what.push_back("its weapon list (" + std::to_string(mn) + " weapons)");
        }
        // 4. The multiplayer player biped.
        std::string mu = mv["multiplayer_information[0].unit"];
        if (!mu.empty() && mu != xv["multiplayer_information[0].unit"] && !in_overlay(mu) && fs::exists(xsrc / from_u8(mu))) {
            sets.push_back({"multiplayer_information[0].unit", mu});
            what.push_back("its player biped " + fs::path(from_u8(mu)).stem().u8string() + " (players spawn with its weapons)");
        }
        // 5. Fall damage and player tuning.
        int tuned = 0;
        for (auto &[k, v] : mkv)
            if ((k.rfind("falling_damage[0].", 0) == 0 || k.rfind("player_information[0].", 0) == 0 ||
                 k.rfind("player_control[0].", 0) == 0) &&
                xv.count(k) && xv[k] != v && v.find('/') == std::string::npos && !v.empty() && v != "*") {
                sets.push_back({k, v});
                tuned++;
            }
        if (tuned) what.push_back("its fall damage and player tuning (" + std::to_string(tuned) + " values)");
    }
    if (!sets.empty()) {
        if (!fs::exists(job.fixes / from_u8(g))) job.to_fixes(g, job.overlay);
        std::vector<std::string> cmd{u8(tool(cfg, "invader-edit")), "-t", u8(job.fixes), g};
        for (auto &[k, v] : sets) cmd.insert(cmd.end(), {"-S", k, v});
        if (run_process(cmd, out) != 0)
            for (auto &[k, v] : sets) run_process({u8(tool(cfg, "invader-edit")), "-t", u8(job.fixes), g, "-S", k, v}, out);
    }
    if (!what.empty()) {
        std::string list;
        for (size_t i = 0; i < what.size(); i++) list += (i ? (i + 1 == what.size() ? " and " : ", ") : "") + what[i];
        job.note("[xbox-mp] kept the map's own globals settings over the Xbox's: " + list);
    }
}

bool prepare_for_xbox(Job &job, std::string &err) {
    const Config &cfg = job.cfg;
    fs::path xsrc = job.work / "xbox_src";
    std::error_code ec;
    fs::create_directories(xsrc, ec);
    std::string out;
    auto in_overlay = [&](const std::string &rel) { return !job.overlay.empty() && fs::exists(job.overlay / from_u8(rel)); };

    job.log("Preparing tags for Xbox...");
    if (!job.user_tags.empty()) {
        // Only the scenario's dependencies (plus any stock tags the user edited).
        std::vector<std::string> roots{job.scenario + ".scenario"};
        if (!job.overlay.empty())
            for (auto &t : edited_stock_tags(job.overlay, job.user_tags)) {
                std::string u = t;
                if (!fs::exists(job.user_tags / from_u8(u)) && fs::path(u).extension() == ".model")
                    u = u.substr(0, u.size() - 6) + ".gbxmodel";
                if (fs::exists(job.user_tags / from_u8(u))) roots.push_back(u);
            }
        std::set<std::string> files;
        for (auto &root : roots) {
            files.insert(root);
            run_process({u8(tool(cfg, "invader-dependency")), "-r", "-t", u8(job.user_tags), root}, out);
            std::istringstream ss(out);
            std::string line;
            while (std::getline(ss, line)) {
                line = norm_rel(line);
                if (!line.empty() && fs::exists(job.user_tags / from_u8(line))) files.insert(line);
            }
        }
        for (auto &f : files) {
            fs::create_directories((xsrc / from_u8(f)).parent_path(), ec);
            fs::copy_file(job.user_tags / from_u8(f), xsrc / from_u8(f), fs::copy_options::overwrite_existing, ec);
        }
        job.log("  copied the " + std::to_string(files.size()) + " tags your map uses");
    } else {
        copy_tree(job.source, xsrc, true, ec);
        if (ec) { err = "Couldn't copy tags: " + ec.message(); return false; }
    }

    // The stock Xbox engine only spawns its own multiplayer vehicles (the first three in the globals list:
    // warthog, ghost, scorpion; the Halo Universal XBE spawns any) and replaces PC-only weapons, so use
    // the Xbox equivalents.
    use_stock_versions(job, xsrc);
    restrict_to_xbox_mp(job, xsrc);
    apply_xbox_slots(job, xsrc);
    merge_map_globals(job, xsrc);
    ensure_player_animations(job, xsrc);
    fix_kept_weapons(job, xsrc);
    correct_xbox_tagset(job);
    trim_unobtainable(job, xsrc);
    apply_replacements(job, xsrc);
    fix_seat_labels(job, xsrc);

    // Models and extended shaders
    int gbx = 0;
    for (auto &e : fs::recursive_directory_iterator(xsrc))
        if (e.path().extension() == ".gbxmodel") gbx++;
    run_process({u8(tool(cfg, "invader-convert")), "-t", u8(xsrc), "-g", "gbxmodel", "model", "-b", "*"}, out);
    run_process({u8(tool(cfg, "invader-convert")), "-t", u8(xsrc), "-g", "shader_transparent_chicago_extended",
                 "shader_transparent_chicago", "-b", "*"}, out);
    if (run_process({u8(tool(cfg, "invader-refactor")), "-t", u8(xsrc), "-M", "no-move", "-g", "gbxmodel", "model",
                     "-g", "shader_transparent_chicago_extended", "shader_transparent_chicago"}, out) != 0) {
        err = "Couldn't convert model/shader references for Xbox:\n" + out.substr(out.size() > 1500 ? out.size() - 1500 : 0);
        return false;
    }
    if (gbx) job.note("[xbox] converted " + std::to_string(gbx) + " Gearbox models to Xbox models");

    // The map's own HUDs (PC high-res scaling, MCC's double-size layout) and Ogg sounds (Xbox can't play them)
    int sounds = 0;
    std::vector<std::string> hud_tags, sound_tags;
    for (auto &e : fs::recursive_directory_iterator(xsrc)) {
        std::string ext = e.path().extension().u8string();
        std::string rel = fs::relative(e.path(), xsrc).generic_u8string();
        if (in_overlay(rel)) continue;
        if (ext == ".unit_hud_interface" || ext == ".weapon_hud_interface" || ext == ".grenade_hud_interface")
            hud_tags.push_back(rel);
        else if (ext == ".sound" && sound_needs_reencode(e.path()))
            sound_tags.push_back(rel);
    }

    // PC/MCC HUDs were drawn against the PC versions of the stock HUD bitmaps, which have extra
    // sequences (e.g. the rocket warthog reticle) and different resolutions. Keep each PC HUD paired
    // with its own bitmap, renamed to *_pc so it doesn't clash with the Xbox stock copy.
    std::set<std::string> pc_bitmaps;
    for (auto &tag : hud_tags) {
        run_process({u8(tool(cfg, "invader-dependency")), "-t", u8(xsrc), tag}, out);
        std::istringstream ss(out);
        std::string dep;
        while (std::getline(ss, dep)) {
            dep = norm_rel(dep);
            if (dep.size() < 7 || dep.compare(dep.size() - 7, 7, ".bitmap") != 0) continue;
            if (!in_overlay(dep) || !fs::exists(xsrc / from_u8(dep))) continue;
            std::string pc = dep.substr(0, dep.size() - 7) + "_pc.bitmap";
            if (!fs::exists(xsrc / from_u8(pc)))
                fs::copy_file(xsrc / from_u8(dep), xsrc / from_u8(pc), fs::copy_options::overwrite_existing, ec);
            std::string rout;
            if (run_process({u8(tool(cfg, "invader-refactor")), "-t", u8(xsrc), "-M", "no-move", "-s", tag, "-T", dep, pc},
                            rout) == 0)
                pc_bitmaps.insert(dep);
        }
    }
    if (!pc_bitmaps.empty())
        job.note("[xbox] kept the PC versions of " + std::to_string(pc_bitmaps.size()) +
                 " HUD bitmaps for HUDs that came from your map (saved as *_pc)");
    // Sizes and positions, now that each HUD points at the bitmaps it will really draw.
    convert_hud_layout(job, xsrc, hud_tags);

    // PC meter bitmaps keep the gradient that drives the fill in alpha and the shape in color; the Xbox
    // reads them the other way round, so a PC meter shows its full shape and never fills or drains with
    // its value (heat, shields, ...). Point each such meter at a copy of its bitmap (*_meter) with the
    // channels swapped. Meters flagged "use xbox shading" already have Xbox-style bitmaps.
    {
        std::map<std::string, std::string> meter_copy; // bitmap -> converted copy ("" = can't convert)
        int meters = 0;
        for (auto &tag : hud_tags) {
            auto kv = list_values(cfg, xsrc, tag);
            std::map<std::string, std::string> vals(kv.begin(), kv.end());
            for (auto &[key, v] : kv) {
                const std::string suffix = "meter_bitmap";
                if (v.empty() || key.size() < suffix.size() ||
                    key.compare(key.size() - suffix.size(), suffix.size(), suffix) != 0)
                    continue;
                std::string base = key.substr(0, key.size() - suffix.size());
                auto fl = vals.find(base + "flags.use_xbox_shading");
                if (fl != vals.end() && fl->second == "1") continue;
                std::string bm = norm_rel(v);
                if (bm.size() < 7 || bm.compare(bm.size() - 7, 7, ".bitmap") != 0) bm += ".bitmap";
                if (in_overlay(bm) || !fs::exists(xsrc / from_u8(bm))) continue; // a stock Xbox sheet
                if (!meter_copy.count(bm)) {
                    std::string m = bm.substr(0, bm.size() - 7) + "_meter.bitmap";
                    fs::copy_file(xsrc / from_u8(bm), xsrc / from_u8(m), fs::copy_options::overwrite_existing, ec);
                    if (!ec && xbox_meter_bitmap_tag(xsrc / from_u8(m)) > 0) {
                        meter_copy[bm] = m;
                    } else {
                        fs::remove(xsrc / from_u8(m), ec);
                        meter_copy[bm] = "";
                        job.note("[xbox] " + bm + ": HUD meter bitmap is in a format that can't be converted to the "
                                 "Xbox meter layout; its meters may not fill correctly");
                    }
                }
                const std::string &m = meter_copy[bm];
                if (m.empty()) continue;
                if (run_process({u8(tool(cfg, "invader-edit")), "-t", u8(xsrc), tag, "-S", key, m}, out) == 0)
                    meters++;
            }
        }
        if (meters)
            job.note("[xbox] converted " + std::to_string(meters) + " HUD meters to the Xbox meter layout so they "
                     "fill and drain with their value (PC meter bitmaps store the fill gradient in alpha)");
    }
    std::map<std::string, std::vector<std::string>> reencoded;   // reason -> sounds
    if (sound_tags.size() > 5) job.log("Re-encoding " + std::to_string(sound_tags.size()) + " sounds for the Xbox...");
    for (auto &rel : sound_tags) {
        std::string why, reason = sound_reencode_reason(xsrc / from_u8(rel));
        if (reencode_sound_for_xbox(job, xsrc, rel, why)) {
            sounds++;
            reencoded[reason].push_back(rel);
        } else {
            err = "Couldn't re-encode " + rel + " for Xbox (" + why + ").";
            return false;
        }
    }
    for (auto &[reason, list] : reencoded) {
        std::string what = reason == "Ogg Vorbis" ? "Ogg Vorbis, which the Xbox can't play"
                         : reason == "uncompressed" ? "uncompressed 16-bit PCM, which the Xbox doesn't play (it was silent)"
                         : reason == "mono 44.1 kHz" ? "mono 44.1 kHz, which the Xbox doesn't play (now 22.05 kHz)"
                         : "too long for the Xbox to load at once (now split)";
        if (list.size() <= 3)
            for (auto &rel : list) job.note("[xbox] re-encoded " + rel + " as Xbox ADPCM: " + what);
        else
            job.note("[xbox] re-encoded " + std::to_string(list.size()) + " sounds as Xbox ADPCM: " + what + " (" +
                     list[0] + ", " + list[1] + " and " + std::to_string(list.size() - 2) + " more)");
    }
    // Dialogue lines can't be split: list the ones still longer than the Xbox loads at once.
    std::vector<std::string> long_dialogue;
    for (auto &e : fs::recursive_directory_iterator(xsrc)) {
        if (e.path().extension() != ".sound") continue;
        std::string rel = fs::relative(e.path(), xsrc).generic_u8string();
        if (in_overlay(rel)) continue;
        SndTag t;
        if (read_sound_tag(e.path(), t) && sound_is_dialogue(t) && sound_has_long_piece(e.path()))
            long_dialogue.push_back(rel);
    }
    if (!long_dialogue.empty()) {
        std::string list = long_dialogue[0];
        for (size_t i = 1; i < long_dialogue.size() && i < 3; i++) list += ", " + long_dialogue[i];
        if (long_dialogue.size() > 3) list += " and " + std::to_string(long_dialogue.size() - 3) + " more";
        job.note("[xbox] " + std::to_string(long_dialogue.size()) + " dialogue sound" +
                 (long_dialogue.size() == 1 ? " has a line" : "s have lines") + " longer than the 227.5 KiB the Xbox "
                 "loads at once (dialogue can't be split); untested on the Xbox, may not play or may crash: " + list);
    }

    // Scripts: PC renamed player_effect_set_max_rumble to ..._vibrate; Xbox only knows rumble.
    fs::path xdata = job.work / "xbox_data";
    fs::path scen_dir = from_u8(job.scenario).parent_path();
    fs::path user_scripts = job.user_data / scen_dir / "scripts";
    if (!job.user_data.empty() && fs::exists(user_scripts)) {
        fs::create_directories(xdata / scen_dir, ec);
        copy_tree(user_scripts, xdata / scen_dir / "scripts", true, ec);
    } else {
        fs::create_directories(xdata, ec); // invader-recover requires the data folder to exist
        run_process({u8(tool(cfg, "invader-recover")), "-t", u8(xsrc), "-d", u8(xdata), job.scenario + ".scenario"}, out);
    }
    bool any_hsc = false;
    if (fs::exists(xdata))
        for (auto &e : fs::recursive_directory_iterator(xdata))
            if (e.path().extension() == ".hsc") any_hsc = true;
    if (any_hsc) {
        convert_scripts(job, xdata, Game::Xbox);
        job.extra = {"-d", u8(xdata), "-S", "data"};
    }

    job.source = xsrc;
    return true;
}

size_t shrink_textures_to_fit(Job &job, size_t needed, int &count) {
    const Config &cfg = job.cfg;
    // Only textures the map actually uses.
    std::string out;
    std::vector<std::string> cmd{u8(tool(cfg, "invader-dependency")), "-r"};
    for (auto &d : job.dirs()) cmd.insert(cmd.end(), {"-t", u8(d)});
    cmd.push_back(job.scenario + ".scenario");
    run_process(cmd, out);
    std::vector<std::pair<size_t, std::string>> cands;
    std::istringstream ss(out);
    std::string dep;
    while (std::getline(ss, dep)) {
        dep = norm_rel(dep);
        if (dep.size() < 7 || dep.compare(dep.size() - 7, 7, ".bitmap") != 0) continue;
        if (fs::exists(job.fixes / from_u8(dep))) continue;
        if (!job.overlay.empty() && fs::exists(job.overlay / from_u8(dep))) continue; // stock: leave alone
        fs::path p = job.source / from_u8(dep);
        if (!fs::exists(p)) continue;
        size_t s = shrink_bitmap_tag(p, false);
        if (s) cands.push_back({s, dep});
    }
    std::sort(cands.rbegin(), cands.rend());
    size_t saved = 0;
    for (auto &c : cands) {
        if (saved >= needed) break;
        saved += shrink_bitmap_tag(job.source / from_u8(c.second), true);
        count++;
    }
    return saved;
}

// Multiplayer vehicles the app knows about. A placed vehicle without a slot of its own becomes the
// slotted vehicle of the same type, else one of the same family (warthogs; ghost/Banshee; scorpion;
// turret), or is removed.
enum { V_WARTHOG, V_RWARTHOG, V_GHOST, V_BANSHEE, V_SCORPION, V_TURRET };
struct MpVehicle { const char *tag, *name; int type; };
static const MpVehicle kMpVehicles[] = {
    {"vehicles/warthog/warthog.vehicle", "Xbox warthog", V_WARTHOG},
    {"vehicles/warthog/mp_warthog.vehicle", "PC warthog", V_WARTHOG},
    {"vehicles/rwarthog/rwarthog.vehicle", "rocket warthog", V_RWARTHOG},
    {"vehicles/ghost/ghost_mp.vehicle", "ghost", V_GHOST},
    {"vehicles/banshee/banshee_mp.vehicle", "Banshee", V_BANSHEE},
    {"vehicles/scorpion/scorpion_mp.vehicle", "scorpion", V_SCORPION},
    {"vehicles/c gun turret/c gun turret_mp.vehicle", "Covenant turret", V_TURRET},
};
static int vehicle_family(int type) {
    switch (type) {
    case V_WARTHOG: case V_RWARTHOG: return 0;
    case V_GHOST: case V_BANSHEE: return 1;
    case V_SCORPION: return 2;
    case V_TURRET: return 3;
    default: return -1;
    }
}
static const char *kXboxDefaultSlots[3] = {"vehicles/warthog/warthog.vehicle", "vehicles/ghost/ghost_mp.vehicle",
                                           "vehicles/scorpion/scorpion_mp.vehicle"};
const MpVehicle *mp_vehicle(const std::string &tag) {
    for (auto &v : kMpVehicles)
        if (tag == v.tag) return &v;
    return nullptr;
}
std::string slot_vehicle(const Config &cfg, int i) {
    return cfg.xbox_vehicle_slots[i].empty() ? kXboxDefaultSlots[i] : cfg.xbox_vehicle_slots[i];
}

struct PcWeapon { const char *name, *weapon, *collection, *def; };
static const PcWeapon kPcWeapons[] = {
    {"flamethrower", "weapons/flamethrower/flamethrower.weapon",
     "item collections/single weapons/flame thrower.item_collection", "shotgun"},
    {"fuel rod gun", "weapons/plasma_cannon/plasma_cannon.weapon",
     "item collections/single weapons/plasma_cannon.item_collection", "rocket launcher"},
};

// What a custom tag stands for, from its file name ("cmt/.../_ghost_mp/ghost_mp.vehicle" -> ghost).
static std::string name_words(const std::string &tag) {
    std::string n = fs::path(from_u8(tag)).stem().u8string();
    for (auto &c : n) c = c == '_' || c == '-' ? ' ' : (char)tolower((unsigned char)c);
    return " " + n + " ";
}
int guess_vehicle_type(const std::string &tag) {
    std::string n = name_words(tag);
    bool hog = n.find("warthog") != std::string::npos || n.find(" hog") != std::string::npos;
    if (n.find("rwarthog") != std::string::npos || (hog && n.find("rocket") != std::string::npos)) return V_RWARTHOG;
    if (hog) return V_WARTHOG;
    if (n.find("banshee") != std::string::npos) return V_BANSHEE;
    if (n.find("ghost") != std::string::npos) return V_GHOST;
    if (n.find("scorpion") != std::string::npos || n.find("tank") != std::string::npos) return V_SCORPION;
    if (n.find("turret") != std::string::npos || n.find("shade") != std::string::npos) return V_TURRET;
    return -1;
}
// The type of a known vehicle, or what a custom one's name says it is.
static int vehicle_type(const std::string &tag) {
    const MpVehicle *mv = mp_vehicle(tag);
    return mv ? mv->type : guess_vehicle_type(tag);
}
static std::string vehicle_label(const std::string &tag) {
    const MpVehicle *mv = mp_vehicle(tag);
    return mv ? std::string(mv->name) : "custom " + fs::path(from_u8(tag)).stem().u8string();
}
// Stock Xbox weapon for a custom weapon/item collection name, or "" if none fits.
static std::string stock_weapon_name(const std::string &tag) {
    std::string n = name_words(tag);
    static const std::pair<const char *, const char *> keys[] = {
        {"plasma pistol", "plasma pistol"}, {"plasma rifle", "plasma rifle"}, {"plasma grenade", "plasma grenades"},
        {"frag", "frag grenades"}, {"assault rifle", "assault rifle"}, {"sniper", "sniper rifle"},
        {"rocket", "rocket launcher"}, {"shotgun", "shotgun"}, {"needler", "needler"}, {"pistol", "pistol"},
        {"magnum", "pistol"}};
    for (auto &k : keys)
        if (n.find(k.first) != std::string::npos) return k.second;
    return "";
}
static std::string stock_powerup_name(const std::string &tag) {
    std::string n = name_words(tag);
    if (n.find("health") != std::string::npos) return "powerup health pack";
    if (n.find("invis") != std::string::npos || n.find("camo") != std::string::npos) return "powerup invisibility";
    if (n.find("shield") != std::string::npos) return "powerup super shield";
    return "";
}

// Maps built with custom tagsets often carry their own (high-resolution) copies of the stock weapons,
// vehicles and player model. Every Xbox multiplayer map already contains the originals, so point the
// map at those instead (custom vehicles are mapped by restrict_to_xbox_mp).
void use_stock_versions(Job &job, const fs::path &xsrc) {
    const Config &cfg = job.cfg;
    if (job.overlay.empty()) return;
    auto in_overlay = [&](const std::string &t) { return fs::exists(job.overlay / from_u8(t)); };
    auto is_pc_weapon = [&](const std::string &t) {
        for (auto &pw : kPcWeapons)
            if (t == pw.collection || t == pw.weapon) return true;
        return false;
    };
    std::string out;
    run_process({u8(tool(cfg, "invader-dependency")), "-t", u8(xsrc), job.scenario + ".scenario"}, out);
    std::vector<std::string> cmd{u8(tool(cfg, "invader-refactor")), "-t", u8(xsrc), "-M", "no-move", "-U"};
    std::map<std::string, std::vector<std::string>> swapped; // stock name -> custom tags
    std::vector<std::string> other;
    std::istringstream ss(out);
    std::string dep;
    while (std::getline(ss, dep)) {
        dep = norm_rel(dep);
        if (dep.empty() || in_overlay(dep) || is_pc_weapon(dep)) continue;
        std::string ext = fs::path(from_u8(dep)).extension().u8string(), to, what;
        if (ext == ".item_collection" && cfg.xbox_use_stock_versions) {
            bool powerup = dep.find("powerup") != std::string::npos;
            what = powerup ? stock_powerup_name(dep) : stock_weapon_name(dep);
            if (!what.empty())
                to = std::string("item collections/") + (powerup ? "powerups/" : "single weapons/") + what + ".item_collection";
        } else if (ext == ".weapon" && cfg.xbox_use_stock_versions) {
            what = stock_weapon_name(dep);
            if (!what.empty() && what.find("grenade") == std::string::npos) to = "weapons/" + what + "/" + what + ".weapon";
        } else if (ext == ".biped" && cfg.xbox_use_stock_versions && name_words(dep).find("cyborg") != std::string::npos) {
            what = "player model";
            to = "characters/cyborg_mp/cyborg_mp.biped";
        }
        if (!to.empty() && in_overlay(to)) {
            cmd.insert(cmd.end(), {"-T", dep, to});
            swapped[what].push_back(dep);
            continue;
        }
        // A custom weapon with no Xbox original (its collection holds a weapon tag the Xbox doesn't have).
        if (ext == ".item_collection" && !cfg.xbox_other_weapons.empty()) {
            std::string items;
            run_process({u8(tool(cfg, "invader-dependency")), "-t", u8(xsrc), dep}, items);
            if (items.find(".weapon") == std::string::npos) continue;
            std::string w = cfg.xbox_other_weapons;
            std::string rc = "item collections/single weapons/" + w + ".item_collection";
            if (!in_overlay(rc)) continue;
            cmd.insert(cmd.end(), {"-T", dep, rc});
            other.push_back(fs::path(from_u8(dep)).stem().u8string() + " -> " + w);
        }
    }
    if (cmd.size() <= 6) return;
    run_process(cmd, out);
    if (!swapped.empty()) {
        std::string list;
        for (auto &[k, v] : swapped) list += (list.empty() ? "" : ", ") + k;
        job.note("[xbox-mp] using the Xbox's own " + list + " instead of the map's custom versions");
    }
    for (auto &o : other) job.note("[xbox-mp] custom weapon " + o);
}

// Take a stock PC multiplayer tag (e.g. the Banshee, which Xbox multiplayer doesn't have) from Custom
// Edition's own maps into the scratch tags. Tags the Xbox tagset also has keep using the Xbox copies;
// the rest (model, weapons, HUD) are converted like the map's own. Returns whether it worked.
bool borrow_from_ce(Job &job, const fs::path &xsrc, const std::string &tag) {
    const Config &cfg = job.cfg;
    if (cfg.ce_dir.empty()) return false;
    fs::path maps = cfg.ce_dir / "maps", tmp = job.work / "ce_borrow";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    auto bs = [](std::string t) { std::replace(t.begin(), t.end(), '/', '\\'); return t; };
    std::vector<fs::path> sources;
    for (const char *m : {"bloodgulch", "sidewinder", "deathisland", "dangercanyon", "icefields", "infinity", "timberland",
                          "gephyrophobia"})
        if (fs::exists(maps / (std::string(m) + ".map"))) sources.push_back(maps / (std::string(m) + ".map"));
    std::string out;
    for (auto &m : sources) {
        run_process({u8(tool(cfg, "invader-extract")), "-m", u8(maps), "-t", u8(tmp), "-r", "-s", bs(tag), u8(m)}, out);
        if (fs::exists(tmp / from_u8(tag))) break;
    }
    if (!fs::exists(tmp / from_u8(tag))) return false;
    // Invader can't extract a few stock tags as part of a recursive extraction; fetch those one by one.
    for (int round = 0; round < 4; round++) {
        std::vector<std::string> cmd{u8(tool(cfg, "invader-dependency")), "-r", "-t", u8(tmp)};
        if (!job.overlay.empty()) cmd.insert(cmd.end(), {"-t", u8(job.overlay)});
        cmd.push_back(tag);
        run_process(cmd, out);
        std::istringstream ss(out);
        std::string l;
        std::vector<std::string> broken;
        while (std::getline(ss, l)) {
            while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
            size_t b = l.find(" [BROKEN]");
            if (b != std::string::npos) broken.push_back(norm_rel(l.substr(0, b)));
        }
        if (broken.empty()) break;
        for (auto &t : broken)
            for (auto &m : sources) {
                run_process({u8(tool(cfg, "invader-extract")), "-m", u8(maps), "-t", u8(tmp), "-s", bs(t), u8(m)}, out);
                if (fs::exists(tmp / from_u8(t))) break;
            }
    }
    for (auto it = fs::recursive_directory_iterator(tmp, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        fs::path rel = fs::relative(it->path(), tmp, ec), dst = xsrc / rel;
        if (fs::exists(dst)) continue; // the map's own copy wins
        fs::create_directories(dst.parent_path(), ec);
        fs::copy_file(it->path(), dst, ec);
    }
    fs::remove_all(tmp, ec);
    return fs::exists(xsrc / from_u8(tag));
}

// ---------------------------------------------------------------- vehicle seats and the player
static std::string lc(std::string s) {
    for (auto &c : s) c = (char)tolower((unsigned char)c);
    return s;
}
static std::string tag_get(const Config &cfg, const fs::path &dir, const std::string &tag, const std::string &key) {
    if (dir.empty() || tag.empty()) return "";
    std::string v;
    if (run_process({u8(tool(cfg, "invader-edit")), "-t", u8(dir), tag, "-G", key}, v) != 0) return "";
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
    return v.find('\n') == std::string::npos ? v : "";
}

// An animation graph's unit blocks (one per seat or stance), with the animations each one uses (its own and
// its weapons'), to tell which stock seat a custom one was made from.
struct GraphUnits {
    std::vector<std::string> labels;           // as written, in order
    std::map<std::string, size_t> index;       // lower-case label -> unit
    std::vector<std::vector<int>> anims;       // per unit
    std::vector<std::string> names;            // animations[i].name, lower case
};
static GraphUnits read_graph_units(const Config &cfg, const fs::path &dir, const std::string &graph) {
    GraphUnits g;
    if (graph.empty() || dir.empty() || !fs::exists(dir / from_u8(graph))) return g;
    static const std::regex label_re(R"(^units\[(\d+)\]\.label$)");
    static const std::regex anim_re(R"(^units\[(\d+)\]\.(?:weapons\[\d+\]\.)?animations\[\d+\]\.animation$)");
    static const std::regex name_re(R"(^animations\[(\d+)\]\.name$)");
    for (auto &[k, v] : list_values(cfg, dir, graph)) {
        std::smatch m;
        if (std::regex_match(k, m, label_re) || std::regex_match(k, m, anim_re)) {
            size_t u = std::stoul(m[1]);
            if (g.labels.size() <= u) { g.labels.resize(u + 1); g.anims.resize(u + 1); }
            if (k.compare(k.size() - 6, 6, ".label") == 0) g.labels[u] = v;
            else if (!v.empty() && isdigit((unsigned char)v[0]) && std::atoi(v.c_str()) != 65535) g.anims[u].push_back(std::atoi(v.c_str()));
        } else if (std::regex_match(k, m, name_re)) {
            size_t a = std::stoul(m[1]);
            if (g.names.size() <= a) g.names.resize(a + 1);
            g.names[a] = lc(v);
        }
    }
    for (size_t u = 0; u < g.labels.size(); u++)
        if (!g.labels[u].empty() && !g.index.count(lc(g.labels[u]))) g.index[lc(g.labels[u])] = u;
    return g;
}
// The animations of the map's own multiplayer player (before any Xbox tags replace it).
static GraphUnits map_player_graph(Job &job) {
    const Config &cfg = job.cfg;
    fs::path src = !job.user_tags.empty() ? job.user_tags : job.source;
    const std::string g = "globals/globals.globals";
    if (!fs::exists(src / from_u8(g))) return {};
    std::string mp = norm_rel(tag_get(cfg, src, g, "multiplayer_information[0].unit"));
    if (mp.empty() || !fs::exists(src / from_u8(mp))) return {};
    std::string graph = norm_rel(tag_get(cfg, src, mp, "animation_graph"));
    return read_graph_units(cfg, src, graph);
}

struct Seat { std::string label; bool driver = false, gunner = false, weapons = false; };
static std::vector<Seat> vehicle_seats(const Config &cfg, const fs::path &dir, const std::string &vehicle) {
    std::vector<Seat> seats;
    static const std::regex re(R"(^seats\[(\d+)\]\.(label|flags\.driver|flags\.gunner|flags\.allows_weapons)$)");
    for (auto &[k, v] : list_values(cfg, dir, vehicle)) {
        std::smatch m;
        if (!std::regex_match(k, m, re)) continue;
        size_t i = std::stoul(m[1]);
        if (seats.size() <= i) seats.resize(i + 1);
        std::string f = m[2];
        if (f == "label") seats[i].label = v;
        else if (f == "flags.driver") seats[i].driver = v == "1";
        else if (f == "flags.gunner") seats[i].gunner = v == "1";
        else seats[i].weapons = v == "1";
    }
    return seats;
}

// Whether players can drive or ride a vehicle: wrecks and props (no seat, no seat the map's own player has
// animations for, or "destroyed"/"wreck" in the name) are scenery that moves, never a working vehicle.
static bool rideable(const Config &cfg, const fs::path &dir, const std::string &vehicle, const GraphUnits &player) {
    std::string n = name_words(vehicle);
    for (auto w : {"destroyed", "wreck", "broken", "debris"})
        if (n.find(w) != std::string::npos) return false;
    auto seats = vehicle_seats(cfg, dir, vehicle);
    if (player.index.empty()) return !seats.empty();
    for (auto &s : seats)
        if (player.index.count(lc(s.label))) return true;
    return false;
}

// Custom vehicles the map's scripts need as they are: named placements whose health or shields the scripts
// read or set, that they attach objects to by marker, or that they put units into by seat name. The Xbox's
// own version has a different damage model, markers and seats. (A Scorpion the map wrecks by script when its
// shields run out: the Xbox's has no shields, so the script wrecks and respawns it over and over.)
// The map's script source: the user's own .hsc files when building from tags, otherwise what invader-recover gets
// back out of the scenario tag (its source text, which invader-extract decompiles from the compiled scripts).
// "" = none.
static std::string map_script_text(Job &job, const fs::path &sdir, const std::string &scen) {
    fs::path user_scripts = job.user_data / from_u8(job.scenario).parent_path() / "scripts";
    fs::path scan = job.work / "script_scan", from = scan;
    std::error_code ec;
    std::string out, text;
    if (!job.user_data.empty() && fs::exists(user_scripts)) {
        from = user_scripts;
    } else {
        fs::remove_all(scan, ec);
        fs::create_directories(scan, ec);
        run_process({u8(tool(job.cfg, "invader-recover")), "-t", u8(sdir), "-d", u8(scan), scen}, out);
    }
    for (auto it = fs::recursive_directory_iterator(from, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->path().extension() == ".hsc") {
            std::ifstream f(it->path(), std::ios::binary);
            text.append(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            text += "\n";
        }
    if (from == scan) fs::remove_all(scan, ec);
    return text;
}

static std::set<std::string> script_bound_vehicles(Job &job, const fs::path &xsrc) {
    const Config &cfg = job.cfg;
    std::set<std::string> bound;
    std::string scen = job.scenario + ".scenario", out;
    fs::path sdir = fs::exists(job.fixes / from_u8(scen)) ? job.fixes : xsrc;
    std::string text = map_script_text(job, sdir, scen);
    static const std::regex call_re(
        R"(\((unit_get_shield|unit_get_health|unit_set_current_vitality|unit_set_maximum_vitality|object_set_shield|)"
        R"(objects_attach|objects_detach|object_set_permutation|object_set_region_state|unit_enter_vehicle|)"
        R"(vehicle_load_magic|vehicle_unload)\s+([^()]*)\))");
    std::set<std::string> names;
    for (std::sregex_iterator it(text.begin(), text.end(), call_re), end; it != end; ++it) {
        std::istringstream ss((*it)[2].str());
        std::string tok;
        while (ss >> tok) names.insert(lc(tok));
    }
    if (names.empty()) return bound;
    std::map<int, std::string> obj, pal;
    std::map<int, int> name_of, type_of;
    static const std::regex obj_re(R"(^object_names\[(\d+)\]\.name$)"), pal_re(R"(^vehicle_palette\[(\d+)\]\.name$)"),
        place_re(R"(^vehicles\[(\d+)\]\.(name|type)$)");
    for (auto &[k, v] : list_values(cfg, sdir, scen)) {
        std::smatch m;
        if (std::regex_match(k, m, obj_re)) obj[std::stoi(m[1])] = lc(v);
        else if (std::regex_match(k, m, pal_re)) pal[std::stoi(m[1])] = norm_rel(v);
        else if (std::regex_match(k, m, place_re) && !v.empty() && isdigit((unsigned char)v[0]))
            (m[2] == "name" ? name_of : type_of)[std::stoi(m[1])] = std::atoi(v.c_str());
    }
    for (auto &[i, n] : name_of)
        if (obj.count(n) && names.count(obj[n]) && type_of.count(i) && pal.count(type_of[i])) bound.insert(pal[type_of[i]]);
    return bound;
}

// The seat in `have` (the player's animations) closest to the map player's seat `label` in `own`: the one whose
// animations the map's seat was made from (its idle pose counts most), then the usual seat for its role.
static std::string stand_in_seat(const GraphUnits &own, const GraphUnits &have, const Seat &seat) {
    static const std::set<std::string> not_seats = {"stand", "crouch", "alert"};
    std::vector<std::string> prefs;
    if (seat.driver) prefs = {"w-driver", "g-driver", "scorpion-driver", "s-driver", "r-driver", "p-driver", "ct-driver", "b-driver"};
    else if (seat.weapons) prefs = {"w-passenger", "scorpionlf", "scorpionrf", "scorpionlb", "scorpionrb", "gt-gunner"};
    else if (seat.gunner) prefs = {"w-gunner", "s-gunner", "gt-gunner", "w-passenger"};
    else prefs = {"w-passenger", "p-riderlf", "p-riderrf", "w-rider", "s-rider"};
    std::map<std::string, int> score;
    auto it = own.index.find(lc(seat.label));
    if (it != own.index.end())
        for (int a : own.anims[it->second]) {
            if (a < 0 || (size_t)a >= own.names.size()) continue;
            const std::string &n = own.names[a];
            std::string best;
            for (auto &h : have.labels) {
                std::string hl = lc(h);
                if (hl.empty() || not_seats.count(hl) || hl.size() <= best.size()) continue;
                if (n.compare(0, hl.size() + 1, hl + " ") == 0) best = hl;
            }
            if (!best.empty()) score[best] += ends_with(n, " idle") ? 3 : ends_with(n, " aim-still") ? 2 : 1;
        }
    std::string pick;
    int best_score = -1;
    size_t best_rank = SIZE_MAX;
    for (auto &h : have.labels) {
        std::string hl = lc(h);
        if (hl.empty() || not_seats.count(hl)) continue;
        int s = score.count(hl) ? score[hl] : 0;
        size_t r = (size_t)(std::find(prefs.begin(), prefs.end(), hl) - prefs.begin());
        if (s > best_score || (s == best_score && r < best_rank)) { pick = h; best_score = s; best_rank = r; }
    }
    return pick;
}

// Players can only get into a seat their animations cover (matched by the seat's label, any case). With "use the
// Xbox's own versions" the players are the Xbox's, whose animations have none for the custom seats of the map's
// own vehicles (a Falcon's pilot, a Mongoose...), even though the map's own player has them: players couldn't get
// in. Those seats take the label of the closest seat the Xbox player has.
void fix_seat_labels(Job &job, const fs::path &xsrc) {
    const Config &cfg = job.cfg;
    auto build_dir = [&](const std::string &rel) -> fs::path {
        if (rel.empty()) return {};
        if (fs::exists(job.fixes / from_u8(rel))) return job.fixes;
        if (!job.overlay.empty() && fs::exists(job.overlay / from_u8(rel)) && !job.keep_source.count(rel)) return job.overlay;
        if (fs::exists(xsrc / from_u8(rel))) return xsrc;
        return {};
    };
    const std::string g = "globals/globals.globals", scen = job.scenario + ".scenario";
    std::string mp = norm_rel(tag_get(cfg, build_dir(g), g, "multiplayer_information[0].unit"));
    std::string graph = norm_rel(tag_get(cfg, build_dir(mp), mp, "animation_graph"));
    GraphUnits have = read_graph_units(cfg, build_dir(graph), graph);
    GraphUnits own = map_player_graph(job);
    if (have.index.empty() || own.index.empty()) return;

    static const std::regex pal_re(R"(^vehicle_palette\[(\d+)\]\.name$)");
    std::vector<std::string> changed;
    int count = 0;
    for (auto &[k, v] : list_values(cfg, build_dir(scen), scen)) {
        if (!std::regex_match(k, pal_re) || v.empty()) continue;
        std::string veh = norm_rel(v);
        fs::path vdir = build_dir(veh);
        if (vdir.empty() || vdir == job.overlay) continue;   // a stock vehicle: stock seats
        auto seats = vehicle_seats(cfg, vdir, veh);
        std::vector<std::string> sets, what;
        for (size_t i = 0; i < seats.size(); i++) {
            const std::string l = lc(seats[i].label);
            if (l.empty() || have.index.count(l) || !own.index.count(l)) continue;   // fine, or never usable
            std::string to = stand_in_seat(own, have, seats[i]);
            if (to.empty()) continue;
            sets.insert(sets.end(), {"-S", "seats[" + std::to_string(i) + "].label", to});
            what.push_back(seats[i].label + " -> " + to);
        }
        if (sets.empty()) continue;
        // Edited where it is: the map's own tags in xsrc still get their model references converted later.
        std::vector<std::string> cmd{u8(tool(cfg, "invader-edit")), "-t", u8(vdir), veh};
        cmd.insert(cmd.end(), sets.begin(), sets.end());
        std::string out;
        if (run_process(cmd, out) != 0) continue;
        count += (int)what.size();
        std::string list;
        for (auto &w : what) list += (list.empty() ? "" : ", ") + w;
        changed.push_back(fs::path(from_u8(veh)).stem().u8string() + " (" + list + ")");
    }
    if (changed.empty()) return;
    std::string list;
    for (auto &c : changed) list += (list.empty() ? "" : "; ") + c;
    job.note("[seats] the Xbox player has no animations for " + std::to_string(count) + " seat" + (count == 1 ? "" : "s") +
             " in the map's own vehicles, so players couldn't get in; they now use the closest Xbox seat: " + list);
}

static void remove_scenario_vehicle(Job &job, const fs::path &xsrc, const std::string &r);

// Apply the vehicle slots / weapon choices to the map's placed content.
void restrict_to_xbox_mp(Job &job, const fs::path &xsrc) {
    const Config &cfg = job.cfg;
    auto have = [&](const std::string &t) {
        return fs::exists(xsrc / from_u8(t)) || (!job.overlay.empty() && fs::exists(job.overlay / from_u8(t)));
    };
    std::vector<std::string> cmd{u8(tool(cfg, "invader-refactor")), "-t", u8(xsrc), "-M", "no-move", "-U"};
    std::vector<std::string> notes, remove;
    std::string scen = job.scenario + ".scenario", out;
    // A stock map (e.g. Custom Edition's Blood Gulch): the Xbox's own scenario wins over the converted
    // one, so its vehicles and items are the Xbox ones and nothing here would take effect.
    if (!job.overlay.empty() && fs::exists(job.overlay / from_u8(scen)) && !fs::exists(job.fixes / from_u8(scen)) &&
        !job.keep_source.count(scen)) {
        job.note("[xbox-mp] " + fs::path(from_u8(job.scenario)).filename().u8string() +
                 " is a stock Xbox map, so the Xbox's own version of its scenario is used (with the Xbox's vehicles)");
        return;
    }

    // Vehicles placed in the scenario.
    std::set<std::string> placed;
    static const std::regex pal_re(R"(^vehicle_palette\[(\d+)\]\.name$)");
    for (auto &[k, v] : list_values(cfg, xsrc, scen))
        if (std::regex_match(k, pal_re) && !v.empty()) placed.insert(v);
    const GraphUnits own_player = map_player_graph(job);
    auto rides = [&](const std::string &v) { return rideable(cfg, xsrc, v, own_player); };
    // The slots as they'll end up. A chosen stock vehicle that isn't in any tags uses the map's own vehicle
    // of that type if it has one (e.g. its custom Banshee), else the slot keeps the Xbox default.
    static const char *slot_names[] = {"Warthog", "Ghost", "Scorpion"};
    fs::path from = !job.user_tags.empty() ? job.user_tags : job.source;
    for (int i = 0; i < 3; i++) {
        std::string want = slot_vehicle(cfg, i);
        job.xbox_slots[i] = want;
        if (want == kXboxDefaultSlots[i] || have(want) || fs::exists(from / from_u8(want))) continue;
        // Not in any tags: with "use the Xbox's own versions", take the stock PC one from Custom Edition.
        if (cfg.xbox_use_stock_versions && mp_vehicle(want) && borrow_from_ce(job, xsrc, want)) {
            job.note("[slots] took the stock " + vehicle_label(want) + " for the \"" + slot_names[i] +
                     "\" slot from Custom Edition's maps (Xbox multiplayer doesn't have one)");
            continue;
        }
        int t = vehicle_type(want);
        std::string own;
        for (auto &v : placed)
            if (!mp_vehicle(v) && fs::exists(xsrc / from_u8(v)) && guess_vehicle_type(v) == t && rides(v)) { own = v; break; }
        if (!own.empty()) {
            job.xbox_slots[i] = own;
            job.note(std::string("[slots] there's no stock ") + vehicle_label(want) + " in these tags, so the \"" +
                     slot_names[i] + "\" slot uses the map's own " + fs::path(from_u8(own)).stem().u8string() + " (" + own + ")");
        } else {
            job.xbox_slots[i] = kXboxDefaultSlots[i];
            job.note(std::string("[slots] ") + fs::path(from_u8(want)).stem().u8string() +
                     " isn't available in this map's tags, so the \"" + slot_names[i] + "\" slot keeps the Xbox default");
        }
    }
    const std::string *slots = job.xbox_slots;
    std::vector<std::string> kept, wrecks, own_kept;
    const std::set<std::string> scripted =
        cfg.xbox_use_stock_versions && cfg.xbox_keep_all_vehicles ? script_bound_vehicles(job, xsrc) : std::set<std::string>{};
    for (auto &v : placed) {
        if (v == slots[0] || v == slots[1] || v == slots[2]) continue;
        // A custom vehicle: with "use the Xbox's own versions", treat it as the stock vehicle its name says
        // it is (on a stock XBE it can't spawn anyway unless it's in a slot); otherwise leave it alone.
        int type = mp_vehicle(v) ? vehicle_type(v) : cfg.xbox_use_stock_versions ? guess_vehicle_type(v) : -1;
        if (type < 0) continue;
        // ...except a wreck ("scorpion_destroyed": scenery the scripts swap in), which must not become a working
        // vehicle, and one the map's scripts depend on (with the Halo Universal XBE it spawns as it is).
        if (!mp_vehicle(v) && !rides(v)) { wrecks.push_back(v); continue; }
        if (!mp_vehicle(v) && scripted.count(v)) { own_kept.push_back(v); continue; }
        // Keeping every vehicle: only a vehicle of exactly a slot's type becomes the slotted one (so the
        // Warthogs/Ghosts/Scorpions gametypes still use its placements); the rest stay as they are.
        int passes = cfg.xbox_keep_all_vehicles ? 1 : 2;
        std::string to;
        for (int pass = 0; pass < passes && to.empty(); pass++)
            for (int i = 0; i < 3; i++) {
                int st = vehicle_type(slots[i]);
                if (st < 0 || !have(slots[i])) continue;
                if (pass == 0 ? st == type : vehicle_family(st) == vehicle_family(type)) { to = slots[i]; break; }
            }
        if (!to.empty()) {
            cmd.insert(cmd.end(), {"-T", v, to});
            notes.push_back(vehicle_label(v) + " -> " + (mp_vehicle(to) ? vehicle_label(to) : "the map's own " + fs::path(from_u8(to)).stem().u8string()));
        } else if (cfg.xbox_keep_all_vehicles) {
            // A custom copy of a stock PC vehicle (a tagset's rocket warthog, Banshee, turret...): with "use
            // the Xbox's own versions", use the stock one instead (far less tag memory), taken from Custom
            // Edition's maps if needed. Otherwise keep the vehicle as it is.
            std::string stock;
            if (!mp_vehicle(v) && cfg.xbox_use_stock_versions)
                for (auto &mv : kMpVehicles)
                    if (mv.type == type && (have(mv.tag) || borrow_from_ce(job, xsrc, mv.tag))) { stock = mv.tag; break; }
            if (!stock.empty()) {
                cmd.insert(cmd.end(), {"-T", v, stock});
                notes.push_back("custom " + fs::path(from_u8(v)).stem().u8string() + " -> " + vehicle_label(stock));
            }
            std::string k = stock.empty() ? v : stock;
            if (std::find(job.kept_vehicles.begin(), job.kept_vehicles.end(), k) == job.kept_vehicles.end()) {
                kept.push_back(k);
                job.kept_vehicles.push_back(k);
            }
        } else {
            remove.push_back(v);
        }
    }
    if (!kept.empty()) {
        std::string list;
        for (auto &v : kept) list += (list.empty() ? "" : ", ") + (mp_vehicle(v) ? vehicle_label(v) : fs::path(from_u8(v)).stem().u8string());
        job.note("[xbox-mp] kept " + list + ": with the Halo Universal XBE they spawn when the gametype's Vehicles "
                 "option is All (a stock XBE leaves them out)");
    }
    auto stems = [](const std::vector<std::string> &v) {
        std::string list;
        for (auto &t : v) list += (list.empty() ? "" : ", ") + fs::path(from_u8(t)).stem().u8string();
        return list;
    };
    if (!own_kept.empty())
        job.note("[xbox-mp] kept the map's own " + stems(own_kept) + " instead of the Xbox's: the map's scripts use "
                 "its health, shields, attachment points or seats, which the Xbox's version doesn't have");
    if (!wrecks.empty())
        job.note("[xbox-mp] left " + stems(wrecks) + " as the map has " + (wrecks.size() == 1 ? "it" : "them") +
                 ": a wreck or prop nobody can ride, not a working vehicle");

    // PC-only weapons that aren't kept.
    for (auto &pw : kPcWeapons) {
        if (!fs::exists(xsrc / from_u8(pw.weapon)) && !fs::exists(xsrc / from_u8(pw.collection))) continue;
        if (std::find(cfg.xbox_keep_weapons.begin(), cfg.xbox_keep_weapons.end(), pw.weapon) != cfg.xbox_keep_weapons.end())
            continue;
        std::string rep = pw.def;
        auto it = cfg.xbox_weapon_replacements.find(pw.weapon);
        if (it != cfg.xbox_weapon_replacements.end() && !it->second.empty()) rep = it->second;
        std::string rw = "weapons/" + rep + "/" + rep + ".weapon";
        std::string rc = "item collections/single weapons/" + rep + ".item_collection";
        bool any = false;
        if (fs::exists(xsrc / from_u8(pw.collection)) && have(rc)) { cmd.insert(cmd.end(), {"-T", pw.collection, rc}); any = true; }
        if (fs::exists(xsrc / from_u8(pw.weapon)) && have(rw)) { cmd.insert(cmd.end(), {"-T", pw.weapon, rw}); any = true; }
        if (any) notes.push_back(std::string(pw.name) + " -> " + rep);
    }
    if (cmd.size() > 6) run_process(cmd, out);
    for (auto &n : notes) job.note("[xbox-mp] " + n);

    for (auto &r : remove) remove_scenario_vehicle(job, xsrc, r);
}

// Delete a vehicle's placements and palette entry, then renumber the remaining placements (Halo doesn't
// allow empty palette entries).
static void remove_scenario_vehicle(Job &job, const fs::path &xsrc, const std::string &r) {
    const Config &cfg = job.cfg;
    std::string scen = job.scenario + ".scenario", out;
    static const std::regex pal_re(R"(^vehicle_palette\[(\d+)\]\.name$)");
    auto edit = [&](std::vector<std::string> args) {
        std::vector<std::string> c{u8(tool(cfg, "invader-edit")), "-t", u8(xsrc), scen};
        c.insert(c.end(), args.begin(), args.end());
        return run_process(c, out) == 0;
    };
    static const std::regex place_re(R"(^vehicles\[(\d+)\]\.type$)");
    {
        int pal = -1;
        for (auto &[key, v] : list_values(cfg, xsrc, scen)) {
            std::smatch m;
            if (v == r && std::regex_match(key, m, pal_re)) { pal = std::stoi(m[1]); break; }
        }
        if (pal < 0) return;
        std::vector<int> drop;
        for (auto &[key, v] : list_values(cfg, xsrc, scen)) {
            std::smatch m;
            if (!std::regex_match(key, m, place_re) || v.empty() || !isdigit((unsigned char)v[0])) continue;
            if (std::stoi(v) == pal) drop.push_back(std::stoi(m[1]));
        }
        std::sort(drop.rbegin(), drop.rend());
        for (int i : drop) edit({"-E", "vehicles[" + std::to_string(i) + "]"});
        edit({"-E", "vehicle_palette[" + std::to_string(pal) + "]"});
        for (auto &[key, v] : list_values(cfg, xsrc, scen)) {
            std::smatch m;
            if (!std::regex_match(key, m, place_re) || v.empty() || !isdigit((unsigned char)v[0])) continue;
            int t = std::stoi(v);
            if (t > pal && t != 65535) edit({"-S", key, std::to_string(t - 1)});
        }
        job.note("[xbox-mp] removed " + vehicle_label(r) + " (" + std::to_string(drop.size()) +
                 " placed; no vehicle slot of its kind)");
    }
}

// The kept vehicles made the map too big for Xbox memory: give up the one whose models, collision and
// animations take the most tag memory, treating it like a stock XBE build does (a slotted vehicle of the
// same family, else removed). The build then retries, so as many extra vehicles stay as fit.
bool drop_kept_vehicles(Job &job) {
    if (job.kept_vehicles.empty()) return false;
    const Config &cfg = job.cfg;
    fs::path xsrc = job.work / "xbox_src";
    auto have = [&](const std::string &t) {
        return fs::exists(xsrc / from_u8(t)) || (!job.overlay.empty() && fs::exists(job.overlay / from_u8(t)));
    };
    std::string out;
    // tag-memory weight of each kept vehicle: its own model-ish tags (textures and sounds don't count)
    size_t best_i = 0;
    uintmax_t best_w = 0;
    for (size_t k = 0; k < job.kept_vehicles.size(); k++) {
        std::string deps;
        run_process({u8(tool(cfg, "invader-dependency")), "-r", "-t", u8(xsrc), job.kept_vehicles[k]}, deps);
        std::istringstream ss(deps);
        std::string d;
        uintmax_t w = 0;
        std::error_code ec;
        while (std::getline(ss, d)) {
            d = norm_rel(d);
            std::string ext = fs::path(from_u8(d)).extension().u8string();
            if (ext == ".gbxmodel" || ext == ".model" || ext == ".model_collision_geometry" || ext == ".model_animations" ||
                ext == ".physics" || ext == ".vehicle") {
                uintmax_t sz = fs::file_size(xsrc / from_u8(d), ec);
                if (!ec) w += sz;
            }
        }
        if (k == 0 || w > best_w) { best_i = k; best_w = w; }
    }
    std::string v = job.kept_vehicles[best_i];
    job.kept_vehicles.erase(job.kept_vehicles.begin() + (long)best_i);
    std::string label = mp_vehicle(v) ? vehicle_label(v) : fs::path(from_u8(v)).stem().u8string();
    int type = mp_vehicle(v) ? vehicle_type(v) : guess_vehicle_type(v);
    std::string to;
    for (int i = 0; i < 3 && to.empty(); i++) {
        int st = vehicle_type(job.xbox_slots[i]);
        if (st >= 0 && have(job.xbox_slots[i]) && vehicle_family(st) == vehicle_family(type)) to = job.xbox_slots[i];
    }
    job.note("[xbox-mp] the map didn't fit in Xbox memory with all the extra vehicles, so " + label +
             " (the biggest) is handled like on a stock XBE");
    if (!to.empty()) {
        run_process({u8(tool(cfg, "invader-refactor")), "-t", u8(xsrc), "-M", "no-move", "-U", "-T", v, to}, out);
        job.note("[xbox-mp] " + label + " -> " + vehicle_label(to));
    } else {
        remove_scenario_vehicle(job, xsrc, v);
    }
    return true;
}

// Put the chosen vehicles/weapons into the Xbox globals' multiplayer slots.
void apply_xbox_slots(Job &job, const fs::path &xsrc) {
    const Config &cfg = job.cfg;
    bool any = false;
    for (int i = 0; i < 3; i++) {
        if (job.xbox_slots[i].empty()) job.xbox_slots[i] = slot_vehicle(cfg, i);
        any = any || job.xbox_slots[i] != kXboxDefaultSlots[i];
    }
    if (!any || job.overlay.empty()) return;
    const std::string g = "globals/globals.globals";
    if (!fs::exists(job.fixes / from_u8(g))) {
        if (!fs::exists(job.overlay / from_u8(g))) return;
        job.to_fixes(g, job.overlay);
    }
    std::string out;
    static const char *slot_names[] = {"Warthog", "Ghost", "Scorpion"};
    for (int i = 0; i < 3; i++) {
        std::string v = job.xbox_slots[i];
        if (v == kXboxDefaultSlots[i]) continue;
        // A slotted vehicle the map doesn't place yet: bring it (and what it uses) into the scratch tags.
        fs::path from = !job.user_tags.empty() ? job.user_tags : job.source;
        if (!fs::exists(xsrc / from_u8(v)) && !fs::exists(job.overlay / from_u8(v)) && fs::exists(from / from_u8(v))) {
            std::string deps;
            run_process({u8(tool(cfg, "invader-dependency")), "-r", "-t", u8(from), v}, deps);
            std::istringstream ss(deps + "\n" + v);
            std::string d;
            std::error_code ec;
            while (std::getline(ss, d)) {
                d = norm_rel(d);
                if (d.empty() || !fs::exists(from / from_u8(d)) || fs::exists(xsrc / from_u8(d))) continue;
                fs::create_directories((xsrc / from_u8(d)).parent_path(), ec);
                fs::copy_file(from / from_u8(d), xsrc / from_u8(d), ec);
            }
        }
        if (!fs::exists(xsrc / from_u8(v)) && !fs::exists(job.overlay / from_u8(v))) {
            job.note(std::string("[slots] ") + fs::path(from_u8(v)).stem().u8string() +
                     " isn't available in this map's tags, so the \"" + slot_names[i] + "\" slot keeps the Xbox default");
            continue;
        }
        if (run_process({u8(tool(cfg, "invader-edit")), "-t", u8(job.fixes), g, "-S",
                         "multiplayer_information[0].vehicles[" + std::to_string(i) + "].vehicle", v}, out) == 0) {
            job.note(std::string("[slots] \"") + slot_names[i] + "\" slot = " + vehicle_label(v));
        }
    }
}

// Halo only lets players pick up weapons their animation graph has animations for (matched by the
// weapon's label). The Xbox player animations lack some PC weapons (e.g. the fuel rod, "pc"), so if
// the map keeps such a weapon, use the PC player animations instead.
void ensure_player_animations(Job &job, const fs::path &xsrc) {
    const Config &cfg = job.cfg;
    const std::string graph = "characters/cyborg/cyborg.model_animations";
    if (job.overlay.empty() || !fs::exists(job.overlay / from_u8(graph)) || !fs::exists(xsrc / from_u8(graph))) return;
    auto labels_of = [&](const fs::path &dir) {
        std::set<std::string> r;
        for (auto &[k, v] : list_values(cfg, dir, graph))
            if (k.size() > 6 && k.compare(k.size() - 6, 6, ".label") == 0 && !v.empty()) r.insert(v);
        return r;
    };
    std::string out;
    run_process({u8(tool(cfg, "invader-dependency")), "-r", "-t", u8(xsrc), job.scenario + ".scenario"}, out);
    std::istringstream ss(out);
    std::string dep;
    std::set<std::string> xbox_labels = labels_of(job.overlay), missing;
    while (std::getline(ss, dep)) {
        dep = norm_rel(dep);
        if (dep.size() < 7 || dep.compare(dep.size() - 7, 7, ".weapon") != 0) continue;
        fs::path owner = fs::exists(job.overlay / from_u8(dep)) ? job.overlay : xsrc;
        std::string lab;
        run_process({u8(tool(cfg, "invader-edit")), "-t", u8(owner), dep, "-G", "label"}, lab);
        while (!lab.empty() && (lab.back() == '\n' || lab.back() == '\r')) lab.pop_back();
        if (!lab.empty() && lab.find(' ') == std::string::npos && !xbox_labels.count(lab)) missing.insert(lab);
    }
    if (missing.empty()) return;
    std::set<std::string> pc_labels = labels_of(xsrc);
    std::string names;
    for (auto &m : missing)
        if (pc_labels.count(m)) names += (names.empty() ? "" : ", ") + m;
    if (names.empty()) return;
    job.to_fixes(graph, xsrc);
    job.note("[xbox] using the PC player animations so these weapons can be picked up (labels: " + names + ")");
}

// Make kept PC-only weapons behave on Xbox:
//  - The Xbox gametype weapon sets remap weapons by their position in the globals weapon list
//    ("Normal" turns the flamethrower's entry into a rocket launcher). Weapons not in the list spawn as
//    themselves, so swap a kept weapon's entry for a stand-in that's already in the list.
//  - The pickup prompt icon comes from the HUD message icon sheet; the fuel rod's icon only exists in
//    the PC sheet, which is otherwise identical to the Xbox one, so use the PC sheet when needed.
void fix_kept_weapons(Job &job, const fs::path &xsrc) {
    const Config &cfg = job.cfg;
    if (cfg.xbox_keep_weapons.empty() || job.overlay.empty()) return;
    std::string out;
    run_process({u8(tool(cfg, "invader-dependency")), "-r", "-t", u8(xsrc), job.scenario + ".scenario"}, out);
    std::set<std::string> deps;
    {
        std::istringstream ss(out);
        std::string l;
        while (std::getline(ss, l)) deps.insert(norm_rel(l));
    }
    std::vector<std::string> kept;
    for (auto &w : cfg.xbox_keep_weapons)
        if (deps.count(w)) kept.push_back(w);
    if (kept.empty()) return;
    auto get = [&](const fs::path &dir, const std::string &tag, const std::string &key) {
        std::string v;
        run_process({u8(tool(cfg, "invader-edit")), "-t", u8(dir), tag, "-G", key}, v);
        while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
        std::replace(v.begin(), v.end(), '\\', '/');
        return v;
    };

    // 1. Weapon list entries.
    const std::string g = "globals/globals.globals";
    if (fs::exists(job.overlay / from_u8(g))) {
        const std::string stand_in = "weapons/gravity rifle/gravity rifle.weapon";
        static const std::regex wep_re(R"(^weapon_list\[(\d+)\]\.weapon$)");
        fs::path gdir = fs::exists(job.fixes / from_u8(g)) ? job.fixes : job.overlay;
        for (auto &[k, v] : list_values(cfg, gdir, g)) {
            std::smatch m;
            if (!std::regex_match(k, m, wep_re) || std::find(kept.begin(), kept.end(), v) == kept.end()) continue;
            if (!fs::exists(job.fixes / from_u8(g))) job.to_fixes(g, job.overlay);
            if (run_process({u8(tool(cfg, "invader-edit")), "-t", u8(job.fixes), g, "-S", k, stand_in}, out) == 0)
                job.note("[xbox] " + fs::path(from_u8(v)).stem().u8string() + ": removed from the Xbox weapon list (entry #" +
                         m[1].str() + ") so gametype weapon sets don't replace it");
        }
    }

    // 2. HUD. The Xbox tagset carries placeholder HUDs for weapons Xbox multiplayer never used (the
    // flamethrower's "master fuel" has no elements at all), and stock tags win over the map's copies,
    // so a kept weapon showed no ammo counter, meter or crosshair. Copy the map's (PC) HUD tags to
    // *_pc names and point the weapon at them. Being the map's own tags, the HUD pass below then
    // converts them like any PC HUD (high-res scaling, paired with the PC HUD bitmaps).
    {
        auto count = [&](const fs::path &dir, const std::string &tag) {
            int n = 0;
            for (auto k : {"static_elements", "meter_elements", "number_elements", "crosshairs", "overlay_elements"}) {
                std::string c;
                run_process({u8(tool(cfg, "invader-edit")), "-t", u8(dir), tag, "-C", k}, c);
                n += std::atoi(c.c_str());
            }
            return n;
        };
        auto pc_name = [](const std::string &tag) {
            fs::path p = from_u8(tag);
            return (p.parent_path() / (p.stem().u8string() + "_pc" + p.extension().u8string())).generic_u8string();
        };
        for (auto &w : kept) {
            fs::path wown = job.owner(w);
            if (wown.empty()) continue;
            std::vector<std::string> chain;   // HUD tags whose Xbox copy is a stand-in for the PC one
            std::string h = get(wown, w, "hud_interface");
            for (int depth = 0; depth < 4 && !h.empty(); depth++) {
                if (!fs::exists(job.overlay / from_u8(h)) || !fs::exists(xsrc / from_u8(h))) break;
                int xc = count(job.overlay, h), pc = count(xsrc, h);
                if (pc <= xc) break;
                chain.push_back(h);
                h = get(xsrc, h, "child_hud");
            }
            if (chain.empty()) continue;
            std::error_code ec;
            for (auto &c : chain)
                fs::copy_file(xsrc / from_u8(c), xsrc / from_u8(pc_name(c)), fs::copy_options::overwrite_existing, ec);
            for (size_t i = 0; i + 1 < chain.size(); i++)
                run_process({u8(tool(cfg, "invader-edit")), "-t", u8(xsrc), pc_name(chain[i]), "-S", "child_hud",
                             pc_name(chain[i + 1])}, out);
            fs::path wdir = wown;
            if (wown == job.overlay && job.to_fixes(w, job.overlay)) wdir = job.fixes;
            if (wdir == job.overlay) continue;
            if (run_process({u8(tool(cfg, "invader-edit")), "-t", u8(wdir), w, "-S", "hud_interface", pc_name(chain[0])}, out) == 0) {
                std::string names;
                for (auto &c : chain) names += (names.empty() ? "" : ", ") + fs::path(from_u8(c)).stem().u8string();
                job.note("[xbox] " + fs::path(from_u8(w)).stem().u8string() + ": using the PC HUD (" + names +
                         "); the Xbox's copies are empty placeholders, so it had no ammo counter, meter or crosshair");
            }
        }
    }

    // 2b. Where shots and flames come from. The Xbox tagset's flamethrower was cut before release: its
    // trigger fires along the third-person gun's own axis instead of where the player aims, and its
    // third-person flames, particle stream and fire sound hang off a marker ("primary trigger fp") that
    // floats beside the gun. On PC the shots are aimed and start at the nozzle. So for a kept Xbox weapon,
    // take the map's (PC) trigger aiming flags, and put any "<marker> fp" the PC model doesn't have onto
    // "<marker>". The Xbox model, animations and the functions behind its built-in ammo display stay.
    for (auto &w : kept) {
        fs::path wown = job.owner(w);
        if (wown.empty() || !fs::exists(xsrc / from_u8(w)) || !fs::exists(job.overlay / from_u8(w))) continue;
        std::string model = get(wown, w, "model");
        if (model.size() < 6 || model.compare(model.size() - 6, 6, ".model") != 0 || !fs::exists(job.overlay / from_u8(model)))
            continue; // not the Xbox weapon (e.g. the user's own edited PC copy)
        std::string label = fs::path(from_u8(w)).stem().u8string();
        auto cnt = [&](const fs::path &dir, const std::string &tag, const std::string &key) {
            std::string c;
            run_process({u8(tool(cfg, "invader-edit")), "-t", u8(dir), tag, "-C", key}, c);
            return std::atoi(c.c_str());
        };
        std::vector<std::string> what;
        int nt = std::min(cnt(wown, w, "triggers"), cnt(xsrc, w, "triggers"));
        bool aim = false;
        for (int i = 0; i < nt; i++)
            for (auto f : {"projectiles_use_weapon_origin", "projectile_vector_cannot_be_adjusted"}) {
                std::string k = "triggers[" + std::to_string(i) + "].flags." + f;
                std::string pv = get(xsrc, w, k), xv = get(job.owner(w), w, k);
                if ((pv == "0" || pv == "1") && pv != xv && job.edit(w, k, pv)) aim = true;
            }
        if (aim) what.push_back("shots now start at the nozzle and go where you aim (PC trigger settings)");

        // Markers: Xbox models list them per permutation, HEK-style tags in one top-level block.
        fs::path mown = job.owner(model);
        std::string pmodel = get(xsrc, w, "model");
        std::set<std::string> pc_markers;
        if (!pmodel.empty() && fs::exists(xsrc / from_u8(pmodel)))
            for (auto &[k, v] : list_values(cfg, xsrc, pmodel))
                if (k.size() > 5 && k.compare(k.size() - 5, 5, ".name") == 0 && k.find("markers[") != std::string::npos)
                    pc_markers.insert(v);
        auto mkv = list_values(cfg, mown, model);
        std::map<std::string, std::string> mvals(mkv.begin(), mkv.end());
        static const std::regex perm_re(R"(^(regions\[\d+\]\.permutations\[\d+\]\.)markers\[(\d+)\]\.name$)");
        static const std::regex top_re(R"(^markers\[(\d+)\]\.name$)");
        std::map<std::string, std::map<std::string, std::string>> groups; // group -> marker name -> key prefix
        for (auto &[k, v] : mkv) {
            std::smatch m;
            if (std::regex_match(k, m, perm_re)) groups[m[1].str()][v] = m[1].str() + "markers[" + m[2].str() + "]";
            else if (std::regex_match(k, m, top_re)) groups[""][v] = "markers[" + m[1].str() + "]";
        }
        std::vector<std::pair<std::string, std::string>> sets; // key -> value
        std::set<std::string> moved;
        for (auto &[grp, names] : groups)
            for (auto &[name, pre] : names) {
                if (name.size() < 4 || name.compare(name.size() - 3, 3, " fp") != 0 || pc_markers.count(name)) continue;
                auto base = names.find(name.substr(0, name.size() - 3));
                if (base == names.end()) continue;
                if (!grp.empty() || !mvals.count(pre + ".instances[0].translation")) {
                    for (auto f : {".node_index", ".translation", ".rotation"})
                        if (mvals.count(base->second + f)) sets.push_back({pre + f, mvals[base->second + f]});
                } else { // top-level markers: copy the instance in the same region/permutation
                    for (int i = 0; mvals.count(pre + ".instances[" + std::to_string(i) + "].translation"); i++) {
                        std::string ip = pre + ".instances[" + std::to_string(i) + "]", src;
                        for (int j = 0; mvals.count(base->second + ".instances[" + std::to_string(j) + "].translation"); j++) {
                            std::string jp = base->second + ".instances[" + std::to_string(j) + "]";
                            if (src.empty()) src = jp;
                            if (mvals[jp + ".region_index"] == mvals[ip + ".region_index"] &&
                                mvals[jp + ".permutation_index"] == mvals[ip + ".permutation_index"]) { src = jp; break; }
                        }
                        if (src.empty()) continue;
                        for (auto f : {".node_index", ".translation", ".rotation"})
                            if (mvals.count(src + f)) sets.push_back({ip + f, mvals[src + f]});
                    }
                }
                moved.insert(name);
            }
        if (!sets.empty() && (mown == job.fixes || job.to_fixes(model, mown))) {
            bool ok = true;
            for (auto &[k, v] : sets)
                ok &= run_process({u8(tool(cfg, "invader-edit")), "-n", "-t", u8(job.fixes), model, "-S", k, v}, out) == 0;
            if (ok) {
                std::string names;
                for (auto &n : moved) names += (names.empty() ? "\"" : ", \"") + n + "\"";
                what.push_back("its third-person flames and particle stream now come out of the nozzle (moved the " +
                               names + " marker there, as on PC)");
            }
        }
        for (auto &s : what) job.note("[xbox] " + label + ": " + s);
    }

    // 3. Pickup icons (unless the map's own icons were already added to a sheet of their own).
    const std::string icons = "ui/hud/bitmaps/combined/hud_msg_icons.bitmap";
    if (!fs::exists(job.overlay / from_u8(icons)) || !fs::exists(xsrc / from_u8(icons))) return;
    {
        fs::path gd = fs::exists(job.fixes / from_u8(g)) ? job.fixes : job.overlay;
        std::string hg = get(gd, g, "interface_bitmaps[0].hud_globals");
        fs::path hd = hg.empty() ? fs::path() : job.owner(hg);
        if (!hd.empty() && get(hd, hg, "icon_bitmap") != icons) return;
    }
    int xbox_count = std::atoi(get(job.overlay, icons, "bitmap_group_sequence").c_str());
    {
        std::string c;
        run_process({u8(tool(cfg, "invader-edit")), "-t", u8(job.overlay), icons, "-C", "bitmap_group_sequence"}, c);
        xbox_count = std::atoi(c.c_str());
        run_process({u8(tool(cfg, "invader-edit")), "-t", u8(xsrc), icons, "-C", "bitmap_group_sequence"}, c);
        int pc_count = std::atoi(c.c_str());
        bool need = false;
        std::string which;
        for (auto &w : kept) {
            fs::path wdir = fs::exists(xsrc / from_u8(w)) ? xsrc : job.overlay;
            std::string hud = get(wdir, w, "hud_interface");
            if (hud.empty()) continue;
            fs::path hdir = fs::exists(xsrc / from_u8(hud)) ? xsrc : job.overlay;
            int idx = std::atoi(get(hdir, hud, "messaging_information_sequence_index").c_str());
            if (idx >= xbox_count && idx < pc_count) {
                need = true;
                which += (which.empty() ? "" : ", ") + fs::path(from_u8(w)).stem().u8string();
            }
        }
        if (!need) return;
        // Only swap in the PC sheet if its first icons match the Xbox sheet exactly.
        auto seqs = [&](const fs::path &dir) {
            std::map<std::string, std::string> r;
            for (auto &[k, v] : list_values(cfg, dir, icons))
                if (k.rfind("bitmap_group_sequence[", 0) == 0) r[k] = v;
            return r;
        };
        auto xs = seqs(job.overlay), ps = seqs(xsrc);
        // Compare icon rectangles (sheet index + left/right/top/bottom); names, anchor points and the
        // sheet's pixel format may differ harmlessly.
        auto rect_key = [](const std::string &k) {
            for (auto suf : {".bitmap_index", ".left", ".right", ".top", ".bottom"}) {
                size_t n = strlen(suf);
                if (k.size() > n && k.compare(k.size() - n, n, suf) == 0) return true;
            }
            return false;
        };
        for (auto &[k, v] : xs)
            if (rect_key(k) && (!ps.count(k) || ps[k] != v)) return;
        job.to_fixes(icons, xsrc);
        job.note("[xbox] using the PC HUD message icons so pickup prompts show an icon for: " + which);
    }
}

// Known mistakes in Xbox tagsets, checked against the original Xbox maps. Each one is only corrected
// if the tagset still has the wrong value, so a fixed tagset is left alone.
struct TagsetCorrection { const char *tag, *key, *bad, *good, *what; };
static const TagsetCorrection kXboxTagsetCorrections[] = {
    // haloxbox-revised: the pause menu's middle fill is 8 pixels narrower than the original, which
    // leaves a gap before the right-hand border.
    {"ui/shell/solo_game/pause_game/pausebox2_left_center.ui_widget_definition", "bounds", "0,0,159,186",
     "0,0,159,194", "pause menu box: middle fill was 8 pixels too narrow (gap before the right border)"},
};

// Content nothing in a multiplayer match can give a player, which would only use up the Xbox's 22 MB of tag
// memory (the engine's source shows how each is used):
//  - palette entries the scenario never places (weapons, equipment, vehicles);
//  - the single-player character and the weapons it carries: in a match the engine spawns players as the
//    multiplayer character, but needs the single-player one set, so it becomes the multiplayer one;
//  - the multiplayer vehicle slots (Warthog, Ghost, Scorpion) when the map places none of that vehicle and has
//    no race vehicle spawns (Race is the only game type that spawns them on its own). The slots stay, empty:
//    the engine skips an empty one;
//  - the cheat power-ups (the globals list only the debug cheats and the script command cheat_all_powerups use;
//    kept when the map's scripts call that, or when its scripts can't be read back to check). A map that hands
//    out power-ups its own way (Orange Box's potions spawn equipment from an effect) keeps those: they're not
//    the globals list's tags, or if they are, whatever else uses them still brings them in.
// Weapons in the globals weapon list, the multiplayer character's weapons, grenades, item collections
// (spawn points, starting equipment) and anything the map's own weapons are built from all stay.
// Whether a map's scripts hand out the globals' cheat power-ups: 1 they call cheat_all_powerups (the one script
// command that reads that list), 0 they don't (or there are none), -1 can't tell (scripts, but no source to read).
int cheat_powerup_use(const std::string &script_text, int script_count) {
    static const std::regex call(R"(\(\s*cheat_all_powerups[\s)])", std::regex::icase);
    if (std::regex_search(script_text, call)) return 1;
    if (script_text.find_first_not_of(" \t\r\n") == std::string::npos && script_count > 0) return -1;
    return 0;
}

void trim_unobtainable(Job &job, const fs::path &xsrc) {
    const Config &cfg = job.cfg;
    std::string scen = job.scenario + ".scenario", out;
    fs::path sdir = fs::exists(job.fixes / from_u8(scen)) ? job.fixes : xsrc;
    if (!fs::exists(sdir / from_u8(scen))) return;
    auto getv = [&](const fs::path &dir, const std::string &tag, const std::string &op, const std::string &key) {
        std::string v;
        if (run_process({u8(tool(cfg, "invader-edit")), "-t", u8(dir), tag, op, key}, v) != 0) return std::string();
        while (!v.empty() && (v.back() == '\n' || v.back() == ' ')) v.pop_back();
        return v;
    };
    auto dep = [&](const fs::path &dir, const std::string &tag, const std::string &key) {
        std::string v = getv(dir, tag, "-G", key);
        return v.empty() || v[0] == '.' || v.find('\n') != std::string::npos ? std::string() : norm_rel(v);
    };
    auto count = [&](const fs::path &dir, const std::string &tag, const std::string &key) {
        return std::atoi(getv(dir, tag, "-C", key).c_str());
    };
    if (getv(sdir, scen, "-G", "type") != "multiplayer") return;
    auto range = [](const std::string &block, int n) {
        return block + "[" + (n == 1 ? std::string("0") : "0-" + std::to_string(n - 1)) + "]";
    };

    // 1. Palette entries nothing is placed from.
    std::vector<std::string> dropped_palette;
    const bool has_ai = count(sdir, scen, "encounters") > 0; // AI starting locations refer to vehicle palette slots
    struct Pal { const char *palette, *block; };
    // (An unplaced biped palette entry is often the map's own player, there for MCC: on Xbox, with the Xbox's own
    // player, it would only take up tag memory.)
    for (Pal pl : {Pal{"weapon_palette", "weapons"}, Pal{"equipment_palette", "equipment"}, Pal{"vehicle_palette", "vehicles"},
                   Pal{"biped_palette", "bipeds"}, Pal{"scenery_palette", "scenery"}}) {
        int n = count(sdir, scen, pl.palette);
        if (n <= 0 || (has_ai && std::string(pl.palette) == "vehicle_palette")) continue;
        std::regex type_re("^" + std::string(pl.block) + R"(\[(\d+)\]\.type$)");
        std::vector<std::pair<int, int>> placed;   // placement index, palette index
        std::set<int> used;
        for (auto &[k, v] : list_values(cfg, sdir, scen)) {
            std::smatch m;
            if (!std::regex_match(k, m, type_re) || v.empty() || !isdigit((unsigned char)v[0])) continue;
            int ti = std::atoi(v.c_str());
            placed.push_back({std::stoi(m[1]), ti});
            if (ti >= 0 && ti < n) used.insert(ti);
        }
        std::vector<int> unused;
        for (int i = 0; i < n; i++) if (!used.count(i)) unused.push_back(i);
        if (unused.empty()) continue;
        std::vector<std::string> cmd{u8(tool(cfg, "invader-edit")), "-t", u8(sdir), scen};
        for (auto &[k, ti] : placed) {
            if (ti < 0 || ti >= n) continue;
            int shift = (int)std::count_if(unused.begin(), unused.end(), [&](int u) { return u < ti; });
            if (shift) cmd.insert(cmd.end(), {"-S", std::string(pl.block) + "[" + std::to_string(k) + "].type",
                                              std::to_string(ti - shift)});
        }
        std::vector<std::string> names;
        for (auto it = unused.rbegin(); it != unused.rend(); ++it) {
            std::string nm = dep(sdir, scen, std::string(pl.palette) + "[" + std::to_string(*it) + "].name");
            if (!nm.empty()) names.push_back(fs::path(from_u8(nm)).stem().u8string());
            cmd.insert(cmd.end(), {"-E", std::string(pl.palette) + "[" + std::to_string(*it) + "]"});
        }
        if (run_process(cmd, out) == 0)
            dropped_palette.insert(dropped_palette.end(), names.rbegin(), names.rend());
    }
    if (!dropped_palette.empty()) {
        std::string list = dropped_palette[0];
        for (size_t i = 1; i < dropped_palette.size() && i < 12; i++) list += ", " + dropped_palette[i];
        if (dropped_palette.size() > 12) list += " and " + std::to_string(dropped_palette.size() - 12) + " more";
        job.note("[trim] left out palette entries the map never places: " + list);
    }

    // 2. Globals.
    const std::string g = "globals/globals.globals";
    fs::path gdir = job.owner(g);
    if (gdir.empty()) return;
    std::vector<std::string> edits, notes;
    std::string sp = dep(gdir, g, "player_information[0].unit"), mp = dep(gdir, g, "multiplayer_information[0].unit");
    if (!sp.empty() && !mp.empty() && sp != mp) {
        edits.insert(edits.end(), {"-S", "player_information[0].unit", mp});
        notes.push_back("the single-player character (" + fs::path(from_u8(sp)).stem().u8string() +
                        ") and the weapons it carries; multiplayer uses " + fs::path(from_u8(mp)).stem().u8string());
    }
    bool race_vehicles = false;
    int flags = count(sdir, scen, "netgame_flags");
    for (int i = 0; i < flags && !race_vehicles; i++)
        race_vehicles = getv(sdir, scen, "-G", "netgame_flags[" + std::to_string(i) + "].type") == "race_vehicle";
    if (!race_vehicles) {
        std::set<std::string> placed_vehicles;
        int vp = count(sdir, scen, "vehicle_palette");
        std::set<int> vused;
        static const std::regex vtype_re(R"(^vehicles\[\d+\]\.type$)");
        if (vp > 0)
            for (auto &[k, v] : list_values(cfg, sdir, scen))
                if (std::regex_match(k, vtype_re) && !v.empty() && isdigit((unsigned char)v[0])) vused.insert(std::atoi(v.c_str()));
        for (int i : vused)
            if (i < vp) placed_vehicles.insert(dep(sdir, scen, "vehicle_palette[" + std::to_string(i) + "].name"));
        int nv = count(gdir, g, "multiplayer_information[0].vehicles");
        std::vector<std::string> gone;
        for (int i = 0; i < nv; i++) {
            std::string key = "multiplayer_information[0].vehicles[" + std::to_string(i) + "].vehicle";
            std::string v = dep(gdir, g, key);
            if (v.empty() || placed_vehicles.count(v)) continue;
            edits.insert(edits.end(), {"-S", key, ""});
            gone.push_back(fs::path(from_u8(v)).stem().u8string());
        }
        if (!gone.empty()) {
            std::string list = gone[0];
            for (size_t i = 1; i < gone.size(); i++) list += ", " + gone[i];
            notes.push_back("the multiplayer vehicles " + list + " (the map places none and has no race vehicle spawns)");
        }
    }
    int cheats = count(gdir, g, "cheat_powerups");
    if (cheats > 0) {
        switch (cheat_powerup_use(map_script_text(job, sdir, scen), count(sdir, scen, "scripts"))) {
        case 0:
            edits.insert(edits.end(), {"-E", range("cheat_powerups", cheats)});
            notes.push_back("the cheat power-ups");
            break;
        case 1:
            job.note("[trim] kept the cheat power-ups: the map's scripts call cheat_all_powerups, which gives them out");
            break;
        default:
            job.note("[trim] kept the cheat power-ups: the map's scripts couldn't be read back to check whether they "
                     "call cheat_all_powerups");
        }
    }
    if (edits.empty()) return;
    if (gdir != job.fixes && !job.to_fixes(g, gdir)) return;
    std::vector<std::string> cmd{u8(tool(cfg, "invader-edit")), "-t", u8(job.fixes), g};
    cmd.insert(cmd.end(), edits.begin(), edits.end());
    if (run_process(cmd, out) != 0) { job.log("  (couldn't trim the globals: " + out + ")"); return; }
    for (auto &n : notes) job.note("[trim] left out " + n + ": nothing in a match can give them to a player");
}

void correct_xbox_tagset(Job &job) {
    if (job.overlay.empty()) return;
    for (auto &c : kXboxTagsetCorrections) {
        fs::path own = fs::exists(job.fixes / from_u8(c.tag)) ? job.fixes : job.overlay;
        if (!fs::exists(own / from_u8(c.tag))) continue;
        std::string v, out;
        run_process({u8(tool(job.cfg, "invader-edit")), "-t", u8(own), c.tag, "-G", c.key}, v);
        while (!v.empty() && (v.back() == '\n' || v.back() == ' ')) v.pop_back();
        if (v != c.bad) continue;
        if (own != job.fixes && !job.to_fixes(c.tag, own)) continue;
        if (run_process({u8(tool(job.cfg, "invader-edit")), "-t", u8(job.fixes), c.tag, "-S", c.key, c.good}, out) == 0)
            job.note(std::string("[xbox] fixed the Xbox tagset's ") + c.what);
    }
}

// Stock tags the user edited win over the target game's stock copies.
void keep_user_edits(Job &job) {
    for (auto &t : edited_stock_tags(job.overlay, job.user_tags)) {
        std::string rel = t;
        if (!fs::exists(job.source / from_u8(rel))) {
            fs::path alt = from_u8(rel);
            if (alt.extension() == ".model") alt.replace_extension(".gbxmodel");
            else if (alt.extension() == ".gbxmodel") alt.replace_extension(".model");
            rel = alt.generic_u8string();
        }
        if (!fs::exists(job.source / from_u8(rel))) continue;
        job.to_fixes(rel, job.source);
        job.note("[keep] " + rel + ": you edited this stock tag, so your version is used");
    }
}

// Xbox maps use .model; PC/MCC need .gbxmodel. Only does work when .model tags exist.
bool prepare_models_for_pc(Job &job, std::string &err) {
    bool any = false;
    for (auto &e : fs::recursive_directory_iterator(job.source))
        if (e.path().extension() == ".model") { any = true; break; }
    if (!any) return true;
    const Config &cfg = job.cfg;
    fs::path merged = job.work / "pc_tags";
    std::error_code ec;
    copy_tree(job.source, merged, false, ec);
    if (ec) { err = "Couldn't copy tags: " + ec.message(); return false; }
    std::string out;
    run_process({u8(tool(cfg, "invader-convert")), "-t", u8(merged), "-g", "model", "gbxmodel", "-b", "*"}, out);
    if (run_process({u8(tool(cfg, "invader-refactor")), "-t", u8(merged), "-M", "no-move", "-g", "model", "gbxmodel"}, out) != 0) {
        err = "Couldn't convert Xbox models for PC:\n" + out.substr(out.size() > 1500 ? out.size() - 1500 : 0);
        return false;
    }
    job.note("[pc] converted Xbox models to Gearbox models");
    job.source = merged;
    return true;
}

void fresh_work(Job &job, const std::string &name) {
    std::error_code ec;
    job.work = job.cfg.cache_dir / "work" / name;
    fs::remove_all(job.work, ec);
    job.fixes = job.work / "fixes";
    fs::create_directories(job.fixes, ec);
}

// The resource maps (bitmaps.map, sounds.map; loc.map for Custom Edition) a map's textures and sounds are read from.
// MCC mods can ship their own, which the map points into: they're looked for in the map's folder, then in
// <folder>/halo1/maps and <folder>/maps one and two folders up (a workshop item keeps its maps in maps\ and its
// resource maps in halo1\maps\). Files the mod doesn't have come from the first of `stocks` that has them (the
// game's own maps folder; for a Custom Edition map also MCC's custom_edition folder, which holds the same files).
// An MCC map's textures point into MCC's own bitmaps.map by position: Custom Edition's (or another mod's) reads
// without an error but gives the wrong pictures, so an MCC map never falls back to those. Returns the folder for
// invader-extract -m (assembled in the work folder when the files come from more than one place), or empty.
fs::path resource_maps_for(Job &job, const fs::path &input, Game in_game, const std::vector<fs::path> &stocks) {
    std::vector<std::string> names;
    if (in_game == Game::MCC) names = {"bitmaps.map", "sounds.map"};          // (Invader reads loc.map for CE maps only)
    else if (in_game == Game::CE) names = {"bitmaps.map", "sounds.map", "loc.map"};
    else return {};
    std::error_code ec;
    auto same = [&](const fs::path &a, const fs::path &b) {
        std::error_code e;
        return !a.empty() && !b.empty() && fs::exists(a, e) && fs::exists(b, e) && fs::equivalent(a, b, e);
    };
    auto is_stock = [&](const fs::path &d) {
        for (auto &st : stocks) if (same(d, st)) return true;
        return false;
    };
    auto has_any = [&](const fs::path &d) {
        for (auto &n : names) if (fs::is_regular_file(d / n, ec)) return true;
        return false;
    };
    fs::path dir = fs::absolute(input, ec).parent_path(), mod;
    std::vector<fs::path> cands{dir};
    fs::path p = dir;
    for (int up = 0; up < 2 && p.has_parent_path() && p.parent_path() != p; up++) {
        p = p.parent_path();
        cands.push_back(p / "halo1" / "maps");
        cands.push_back(p / "maps");
    }
    for (auto &c : cands)
        if (!is_stock(c) && has_any(c)) { mod = c; break; }

    // Where each file comes from: the mod, else the first stock folder that has it.
    std::vector<std::pair<std::string, fs::path>> from;
    for (auto &n : names) {
        fs::path src;
        if (!mod.empty() && fs::is_regular_file(mod / n, ec)) src = mod;
        else for (auto &st : stocks) if (!st.empty() && fs::is_regular_file(st / n, ec)) { src = st; break; }
        if (!src.empty()) from.push_back({n, src});
    }
    if (from.empty()) return {};
    auto list_of = [&](const fs::path &d) {
        std::vector<std::string> v;
        for (auto &[n, s] : from) if (s == d) v.push_back(n);
        std::string out;
        for (size_t i = 0; i < v.size(); i++) out += (i ? (i + 1 == v.size() ? " and " : ", ") : "") + v[i];
        return out;
    };
    std::vector<fs::path> used;
    for (auto &[n, s] : from) if (std::find(used.begin(), used.end(), s) == used.end()) used.push_back(s);
    auto describe = [&](const fs::path &d) {
        if (d == mod) return "the map's own " + list_of(d) + " (" + u8(d) + ")";
        if (in_game == Game::CE && stocks.size() > 1 && d == stocks.back())
            return "MCC's copy of Custom Edition's " + list_of(d) + " (" + u8(d) + ")";
        return "the game's " + list_of(d);
    };
    job.resources_from.clear();
    for (auto &d : used) job.resources_from += (job.resources_from.empty() ? "" : " and ") + u8(d);
    if (used.size() == 1) {
        if (used[0] == mod || (stocks.size() > 1 && used[0] == stocks.back()))
            job.log("Using " + describe(used[0]));
        return used[0];
    }
    // From more than one folder: put them side by side (links where possible, copies otherwise).
    fs::path both = job.work / "resource_maps";
    fs::remove_all(both, ec);
    fs::create_directories(both, ec);
    auto place = [&](const fs::path &d, const std::string &n) {
        std::error_code e;
        fs::create_hard_link(d / n, both / n, e);
        if (e) { e.clear(); fs::create_symlink(d / n, both / n, e); }
        if (e) { e.clear(); fs::copy_file(d / n, both / n, fs::copy_options::overwrite_existing, e); }
        return !e;
    };
    bool ok = true;
    for (auto &[n, s] : from) ok = place(s, n) && ok;
    if (!ok) {
        job.log("Note: couldn't put the resource maps side by side; using " + describe(used[0]) + " only");
        job.resources_from = u8(used[0]);
        return used[0];
    }
    std::string msg;
    for (size_t i = 0; i < used.size(); i++) msg += (i ? " with " : "") + describe(used[i]);
    job.log("Using " + msg);
    return both;
}

#include "contents.inc"

} // namespace

int scripts_use_cheat_powerups(const std::string &script_text, int script_count) {
    return cheat_powerup_use(script_text, script_count);
}

// ---------------------------------------------------------------- public API

static Result convert_map_impl(const Config &cfg, const fs::path &input, Game in_game, Game out_game,
                               const fs::path &output, bool share, const Log &log, ContentAnalysis *an,
                               ContentAnalysis *measure = nullptr) {
    Result r;
    if (!invader_ok(cfg)) { r.error = "Invader wasn't found. Set it up in Settings."; return r; }
    if (out_game == Game::Unknown) { r.error = "Pick an output game."; return r; }

    Job job(cfg, log, out_game);
    job.from = in_game;
    job.analysis = an;
    job.measure = measure;
    fresh_work(job, an ? "analyze" : cfg.level_shaders_only ? "shaders" : "convert");
    job.source = job.work / "source_tags";
    std::error_code ec;
    fs::create_directories(job.source, ec);

    // Protected maps have their tag names scrambled, so there's nothing usable to extract.
    {
        std::string path;
        run_process({u8(tool(cfg, "invader-info")), "-T", "scenario_path", u8(input)}, path);
        if (path.find("<protected>") != std::string::npos) {
            r.error = "This map is protected: its author scrambled its tag names so it can't be taken apart, and the "
                      "app needs those to rebuild it for another game. Use an unprotected copy of the map (for example "
                      "the MCC release, or ask the author).";
            return r;
        }
    }
    log("Extracting tags from " + input.filename().u8string() + "...");
    std::vector<std::string> cmd{u8(tool(cfg, "invader-extract")), "-t", u8(job.source)};
    // MCC keeps Custom Edition's resource maps (for classic Custom Edition maps) in halo1\maps\custom_edition and its
    // own in halo1\maps. MCC maps need MCC's own, so an MCC maps folder set to custom_edition means the one above it.
    fs::path mcc_dir = cfg.mcc_maps_dir, mcc_ce;
    if (!mcc_dir.empty() && mcc_dir.filename().empty()) mcc_dir = mcc_dir.parent_path();   // a trailing slash
    {
        std::string leaf = mcc_dir.filename().u8string();
        for (auto &c : leaf) c = (char)tolower((unsigned char)c);
        if (leaf == "custom_edition") {
            mcc_ce = mcc_dir;
            mcc_dir = mcc_dir.parent_path();
            if (in_game == Game::MCC && fs::is_regular_file(mcc_dir / "bitmaps.map", ec))
                log("Note: the MCC maps folder in Settings is MCC's custom_edition folder, which has Custom Edition's "
                    "bitmaps.map and sounds.map (for classic Custom Edition maps). MCC maps use MCC's own, one folder up: " +
                    u8(mcc_dir));
            else mcc_dir = mcc_ce;
        } else if (!mcc_dir.empty()) {
            mcc_ce = mcc_dir / "custom_edition";
        }
    }
    std::vector<fs::path> stocks;
    if (in_game == Game::CE) stocks = {cfg.ce_dir.empty() ? fs::path() : cfg.ce_dir / "maps", mcc_ce};
    else if (in_game == Game::MCC) stocks = {mcc_dir};
    fs::path res_dir = resource_maps_for(job, input, in_game, stocks);
    if (!res_dir.empty()) cmd.insert(cmd.end(), {"-m", u8(res_dir)});
    cmd.push_back(u8(input));
    std::string out;
    if (run_process(cmd, out) != 0) {
        r.error = "Couldn't extract tags from the map:\n" + out.substr(out.size() > 2000 ? out.size() - 2000 : 0);
        if (in_game == Game::CE && res_dir.empty())
            r.error += "\nTip: set your Custom Edition folder in Settings so its bitmaps/sounds.map can be read.";
        return r;
    }
    {
        // Textures/sounds whose data wasn't in the resource maps read: stock ones are covered by the target game's own
        // tags; any other the build needs is reported then.
        static const std::regex errors(R"(with (\d+) errors?)");
        std::smatch em;
        if (std::regex_search(out, em, errors) && std::stoi(em[1]) > 0)
            log("Note: " + em[1].str() + " of the map's textures/sounds weren't in " +
                (job.resources_from.empty() ? std::string("any bitmaps.map/sounds.map (none found)")
                                            : "the bitmaps.map/sounds.map read (" + job.resources_from + ")") +
                "; stock ones come from the target game's tags");
    }

    // Stock tags of the target game win over the converted copies.
    std::string err;
    if (out_game == Game::CE) {
        job.overlay = stock_ce_tags(cfg, log, err);
        if (job.overlay.empty()) log("Note: " + err + " Building without stock CE tags.");
    } else if (out_game == Game::MCC && in_game != Game::MCC) {
        // MCC's own stock tags (H1A-EK) if available; otherwise Custom Edition's, which MCC also accepts.
        if (fs::exists(cfg.ek_dir / "tags")) job.overlay = cfg.ek_dir / "tags";
        else job.overlay = stock_ce_tags(cfg, log, err);
    } else if (out_game == Game::Xbox) {
        job.overlay = stock_xbox_tags(cfg, log, err);
        if (job.overlay.empty()) { r.error = err; return r; }
    }

    fs::path scen = find_scenario(job.source);
    if (scen.empty()) {
        r.error = "No scenario tag found in the map. It may be protected or damaged; try an unprotected copy.";
        return r;
    }
    return run_build(job, tag_path_noext(scen), output, share);
}

static Result build_from_tags_impl(const Config &cfg, const fs::path &scenario_file, Game out_game,
                                   const fs::path &output, bool share, const Log &log, ContentAnalysis *an,
                                   ContentAnalysis *measure = nullptr) {
    Result r;
    if (!invader_ok(cfg)) { r.error = "Invader wasn't found. Set it up in Settings."; return r; }
    // Find the enclosing "tags" folder.
    fs::path tags;
    for (fs::path p = scenario_file.parent_path(); !p.empty() && p != p.root_path(); p = p.parent_path()) {
        std::string n = p.filename().u8string();
        std::transform(n.begin(), n.end(), n.begin(), ::tolower);
        if (n == "tags") { tags = p; break; }
    }
    if (tags.empty()) { r.error = "The scenario needs to be inside a folder named \"tags\"."; return r; }

    Job job(cfg, log, out_game);
    job.analysis = an;
    job.measure = measure;
    fresh_work(job, an ? std::string("analyze") : cfg.level_shaders_only ? std::string("shaders") : std::string("build_") + game_engine(out_game));
    job.source = tags;
    job.user_tags = tags;
    fs::path rel = fs::relative(scenario_file, tags);
    std::string scenario = tag_path_noext(rel);
    fs::path data = tags.parent_path() / "data";
    if (fs::exists(data)) job.user_data = data;

    std::string err;
    if (out_game == Game::CE) {
        job.overlay = stock_ce_tags(cfg, log, err);
        if (job.overlay.empty()) log("Note: " + err + " Building without stock CE tags.");
    } else if (out_game == Game::Xbox) {
        job.overlay = stock_xbox_tags(cfg, log, err);
        if (job.overlay.empty()) { r.error = err; return r; }
    }

    fs::path scripts = data / rel.parent_path() / "scripts";
    if (out_game != Game::Xbox && fs::exists(scripts)) {
        // Compile a converted copy, so functions the target game doesn't have don't stop the build.
        fs::path sdata = job.work / "script_data";
        std::error_code ec;
        fs::create_directories(sdata / rel.parent_path() / "scripts", ec);
        copy_tree(scripts, sdata / rel.parent_path() / "scripts", true, ec);
        log("Compiling scripts from " + u8(scripts));
        convert_scripts(job, sdata, out_game);
        job.extra = {"-d", u8(sdata), "-S", "data"};
    }
    return run_build(job, scenario, output, share);
}

// Single-text version (used by the test driver).
std::string convert_script_text(Game target, const std::string &text, std::vector<std::string> *notes) {
    ScriptConverter conv(target);
    conv.collect_user_names(text);
    std::string t = text;
    if (!conv.convert(t) && notes) notes->push_back("could not parse");
    if (notes)
        for (auto &[name, c] : conv.changes) notes->push_back(name + ": " + c.second + " (" + std::to_string(c.first) + "x)");
    return t;
}

} // namespace porter

namespace porter {
Result convert_map(const Config &cfg, const fs::path &input, Game in_game, Game out_game,
                   const fs::path &output, bool share, const Log &log, ContentAnalysis *measure) {
    return convert_map_impl(cfg, input, in_game, out_game, output, share, log, nullptr, measure);
}
Result build_from_tags(const Config &cfg, const fs::path &scenario_file, Game out_game,
                       const fs::path &output, bool share, const Log &log, ContentAnalysis *measure) {
    return build_from_tags_impl(cfg, scenario_file, out_game, output, share, log, nullptr, measure);
}
ContentEstimate estimate_contents(const ContentAnalysis &a, const std::map<std::string, std::string> &picks) {
    return estimate_contents_impl(a, picks);
}
static const char *kAnalysisMagic = "HaloMapPorter contents 2";
static const char kPreviewMagic[8] = {'H', 'M', 'P', 'M', 'O', 'D', 'L', '2'};

// The items' models, in a file of their own (binary, a few MB; textures stored as DXT1, an eighth of their size).
static std::string dxt1_of(const ModelPreview::Texture &t) {
    int bw = (t.w + 3) / 4, bh = (t.h + 3) / 4;
    std::string out((size_t)bw * bh * 8, '\0');
    unsigned char blk[64];
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) {
            for (int k = 0; k < 16; k++) {
                int x = std::min(t.w - 1, bx * 4 + k % 4), y = std::min(t.h - 1, by * 4 + k / 4);
                memcpy(blk + k * 4, &t.rgba[((size_t)y * t.w + x) * 4], 4);
                blk[k * 4 + 3] = 255;
            }
            stb_compress_dxt_block((unsigned char *)&out[((size_t)by * bw + bx) * 8], blk, 0, STB_DXT_NORMAL);
        }
    return out;
}
static void rgba_of_dxt1(const std::string &d, ModelPreview::Texture &t) {
    int bw = (t.w + 3) / 4, bh = (t.h + 3) / 4;
    t.rgba.assign((size_t)t.w * t.h * 4, 255);
    if (d.size() < (size_t)bw * bh * 8) return;
    unsigned char blk[64];
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) {
            bcdec_bc1(d.data() + ((size_t)by * bw + bx) * 8, blk, 16);
            for (int k = 0; k < 16; k++) {
                int x = bx * 4 + k % 4, y = by * 4 + k / 4;
                if (x < t.w && y < t.h) memcpy(&t.rgba[((size_t)y * t.w + x) * 4], blk + k * 4, 4);
            }
        }
}
static bool save_previews(const std::vector<ModelPreview> &v, const fs::path &file) {
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    auto u32 = [&](uint32_t x) { f.write((const char *)&x, 4); };
    auto vec = [&](const auto &x) {
        u32((uint32_t)x.size());
        if (!x.empty()) f.write((const char *)x.data(), (std::streamsize)(x.size() * sizeof(x[0])));
    };
    f.write(kPreviewMagic, 8);
    u32((uint32_t)v.size());
    for (auto &m : v) {
        vec(m.pos);
        vec(m.uv);
        vec(m.tris);
        u32((uint32_t)m.parts.size());
        for (auto &p : m.parts) { u32(p.first); u32(p.count); u32((uint32_t)p.texture); }
        u32((uint32_t)m.textures.size());
        for (auto &t : m.textures) {
            u32((uint32_t)t.w);
            u32((uint32_t)t.h);
            vec(dxt1_of(t));
        }
    }
    return (bool)f;
}
static bool load_previews(std::vector<ModelPreview> &out, const fs::path &file) {
    std::ifstream f(file, std::ios::binary);
    std::string b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t o = 8;
    bool ok = b.size() >= 12 && memcmp(b.data(), kPreviewMagic, 8) == 0;
    auto u32 = [&]() -> uint32_t {
        uint32_t x = 0;
        if (o + 4 > b.size()) { ok = false; return 0; }
        memcpy(&x, b.data() + o, 4);
        o += 4;
        return x;
    };
    auto vec = [&](auto &x) {
        uint32_t n = u32();
        size_t bytes = (size_t)n * sizeof(x[0]);
        if (!ok || o + bytes > b.size()) { ok = false; return; }
        x.resize(n);
        if (n) memcpy(x.data(), b.data() + o, bytes);
        o += bytes;
    };
    std::vector<ModelPreview> v(ok ? u32() : 0);
    for (auto &m : v) {
        if (!ok) break;
        vec(m.pos);
        vec(m.uv);
        vec(m.tris);
        m.parts.resize(std::min<uint32_t>(u32(), 100000));
        for (auto &p : m.parts) { p.first = u32(); p.count = u32(); p.texture = (int)u32(); }
        m.textures.resize(std::min<uint32_t>(u32(), 10000));
        for (auto &t : m.textures) {
            t.w = (int)u32();
            t.h = (int)u32();
            std::string d;
            vec(d);
            if (ok && t.w > 0 && t.h > 0 && t.w <= 4096 && t.h <= 4096) rgba_of_dxt1(d, t);
            else t.rgba.clear();
        }
        // Indices in range, or the model is left out.
        size_t verts = m.pos.size() / 3;
        bool good = ok && m.uv.size() == verts * 2 && m.tris.size() % 3 == 0;
        for (uint32_t x : m.tris) good = good && x < verts;
        for (auto &p : m.parts) good = good && (uint64_t)p.first + p.count <= m.tris.size() / 3 && p.texture < (int)m.textures.size();
        for (auto &t : m.textures) good = good && t.w > 0 && t.h > 0 && t.rgba.size() == (size_t)t.w * t.h * 4;
        if (!good) m = ModelPreview();
    }
    if (!ok) return false;
    out = std::move(v);
    return true;
}
bool save_content_analysis(const ContentAnalysis &a, const fs::path &file) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path part = file;
    part += ".part";
    {
        std::ofstream f(part, std::ios::binary);
        if (!f) return false;
        f.precision(9);
        f << kAnalysisMagic << "\n" << a.map_name << "\n" << a.limit_mb << " " << a.file_limit_mb << " " << a.used_mb << " "
          << a.optimize_saves_mb << " " << a.file_mb << " " << a.fixed_mb << " " << a.fixed_file_mb << "\n" << a.tags.size() << "\n";
        for (size_t i = 0; i < a.tags.size(); i++) {
            f << a.tags[i] << "\n" << a.bytes[i] << " " << a.file_bytes[i] << " " << a.deps[i].size();
            for (int d : a.deps[i]) f << " " << d;
            f << " " << a.geometry[i].size();
            for (auto &g : a.geometry[i]) f << " " << g.first << " " << g.second;
            f << "\n";
        }
        f << a.roots.size();
        for (int r : a.roots) f << " " << r;
        f << "\n" << a.items.size() << "\n";
        for (auto &it : a.items)
            f << it.tag << "\n" << it.name << "\n" << it.used_by << "\n" << it.seat_roles << "\n" << it.piece_of << "\n"
              << it.vehicle << " " << it.in_map << " " << it.memory_mb << " " << it.file_mb << " " << it.seats << " "
              << it.preview << "\n";
        f << "end\n";
        if (!f) return false;
    }
    fs::path models = file;
    models += ".models";
    if (!save_previews(a.previews, models)) fs::remove(models, ec);
    fs::rename(part, file, ec);
    if (ec) { fs::remove(part, ec); return false; }
    return true;
}
bool load_content_analysis(ContentAnalysis &out, const fs::path &file) {
    std::ifstream f(file, std::ios::binary);
    std::string line;
    if (!std::getline(f, line) || line != kAnalysisMagic) return false;
    ContentAnalysis a;
    auto text = [&](std::string &s) { if (!std::getline(f, s)) return false; if (!s.empty() && s.back() == '\r') s.pop_back(); return true; };
    auto nums = [&](std::istringstream &ss) { ss.str(line); ss.clear(); };
    std::istringstream ss;
    size_t n = 0;
    if (!text(a.map_name) || !text(line)) return false;
    nums(ss);
    if (!(ss >> a.limit_mb >> a.file_limit_mb >> a.used_mb >> a.optimize_saves_mb >> a.file_mb >> a.fixed_mb >> a.fixed_file_mb)) return false;
    if (!text(line)) return false;
    nums(ss);
    if (!(ss >> n) || n > 1000000) return false;
    a.tags.resize(n); a.bytes.resize(n); a.file_bytes.resize(n); a.deps.resize(n); a.geometry.resize(n);
    for (size_t i = 0; i < n; i++) {
        size_t nd = 0, ng = 0;
        if (!text(a.tags[i]) || !text(line)) return false;
        nums(ss);
        if (!(ss >> a.bytes[i] >> a.file_bytes[i] >> nd) || nd > n) return false;
        a.deps[i].resize(nd);
        for (auto &d : a.deps[i]) if (!(ss >> d) || d < 0 || (size_t)d >= n) return false;
        if (!(ss >> ng) || ng > 10000000) return false;
        a.geometry[i].resize(ng);
        for (auto &g : a.geometry[i]) if (!(ss >> g.first >> g.second)) return false;
    }
    if (!text(line)) return false;
    nums(ss);
    size_t nr = 0;
    if (!(ss >> nr) || nr > n) return false;
    a.roots.resize(nr);
    for (auto &r : a.roots) if (!(ss >> r) || r < 0 || (size_t)r >= n) return false;
    if (!text(line)) return false;
    nums(ss);
    size_t ni = 0;
    if (!(ss >> ni) || ni > 100000) return false;
    a.items.resize(ni);
    for (auto &it : a.items) {
        if (!text(it.tag) || !text(it.name) || !text(it.used_by) || !text(it.seat_roles) || !text(it.piece_of) || !text(line))
            return false;
        nums(ss);
        if (!(ss >> it.vehicle >> it.in_map >> it.memory_mb >> it.file_mb >> it.seats >> it.preview)) return false;
    }
    if (!text(line) || line != "end") return false;
    fs::path models = file;
    models += ".models";
    if (!load_previews(a.previews, models)) a.previews.clear();
    for (auto &it : a.items)
        if (it.preview < -1 || it.preview >= (int)a.previews.size() || (it.preview >= 0 && a.previews[it.preview].tris.empty()))
            it.preview = -1;
    out = std::move(a);
    return true;
}

// The LODs window's file (text) and its models (file + ".models").
static const char *kLodViewMagic = "HaloMapPorter lods 1";
bool save_lod_view(const LodView &v, const fs::path &file) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path models = file;
    models += ".models";
    if (!save_previews(v.previews, models)) return false;
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    f.precision(9);
    f << kLodViewMagic << "\n" << v.map_name << "\n" << v.models.size() << "\n";
    for (auto &m : v.models) {
        f << m.tag << "\n" << m.name << "\n" << m.change << "\n" << m.kind << " " << m.undone << " " << m.placed << " " << m.radius;
        for (double c : m.cutoffs) f << " " << c;
        for (int l : m.level) f << " " << l;
        f << "\n";
    }
    f << "end\n";
    return (bool)f;
}
bool load_lod_view(LodView &out, const fs::path &file) {
    std::ifstream f(file, std::ios::binary);
    std::string line;
    auto text = [&](std::string &s) { if (!std::getline(f, s)) return false; if (!s.empty() && s.back() == '\r') s.pop_back(); return true; };
    LodView v;
    size_t n = 0;
    if (!text(line) || line != kLodViewMagic || !text(v.map_name) || !text(line)) return false;
    n = std::strtoul(line.c_str(), nullptr, 10);
    if (n > 100000) return false;
    v.models.resize(n);
    for (auto &m : v.models) {
        if (!text(m.tag) || !text(m.name) || !text(m.change) || !text(line)) return false;
        std::istringstream ss(line);
        if (!(ss >> m.kind >> m.undone >> m.placed >> m.radius)) return false;
        for (double &c : m.cutoffs) if (!(ss >> c)) return false;
        for (int &l : m.level) if (!(ss >> l)) return false;
    }
    if (!text(line) || line != "end") return false;
    fs::path models = file;
    models += ".models";
    if (!load_previews(v.previews, models)) v.previews.clear();
    for (auto &m : v.models)
        for (int &l : m.level)
            if (l < -1 || l >= (int)v.previews.size() || (l >= 0 && v.previews[l].tris.empty())) l = -1;
    out = std::move(v);
    return true;
}

// ---------------------------------------------------------------- the map browser's info.txt lines (Halo Universal XBE build 20)
namespace {
const char *const kStockVehicles[] = {
    "vehicles/warthog/warthog", "vehicles/warthog/mp_warthog", "vehicles/rwarthog/rwarthog", "vehicles/scorpion/scorpion",
    "vehicles/scorpion/scorpion_mp", "vehicles/ghost/ghost", "vehicles/ghost/ghost_mp", "vehicles/banshee/banshee",
    "vehicles/banshee/banshee_mp", "vehicles/c gun turret/c gun turret", "vehicles/c gun turret/c gun turret_mp",
    "vehicles/wraith/wraith", "vehicles/pelican/pelican", "vehicles/c_dropship/c_dropship",
    "vehicles/fighterbomber/fighterbomber", "vehicles/lifepod/lifepod",
};
const char *const kStockWeapons[] = {
    "weapons/assault rifle/assault rifle", "weapons/pistol/pistol", "weapons/plasma pistol/plasma pistol",
    "weapons/plasma rifle/plasma rifle", "weapons/needler/needler", "weapons/needler/mp_needler", "weapons/shotgun/shotgun",
    "weapons/sniper rifle/sniper rifle", "weapons/rocket launcher/rocket launcher", "weapons/plasma_cannon/plasma_cannon",
    "weapons/flamethrower/flamethrower", "weapons/gravity rifle/gravity rifle", "weapons/energy sword/energy sword",
    "weapons/ball/ball", "weapons/flag/flag",
};
const char *const kStockCharacters[] = {"characters/cyborg_mp/cyborg_mp", "characters/cyborg/cyborg"};
bool listed(const char *const *list, size_t n, const std::string &p) {
    for (size_t i = 0; i < n; i++) if (p == list[i]) return true;
    return false;
}
// "Weapons/Foo/Bar.weapon" -> "weapons/foo/bar"; "" for an empty reference
std::string ref_path(std::string v, const char *ext) {
    for (auto &c : v) c = (char)tolower((unsigned char)c);
    std::replace(v.begin(), v.end(), '\\', '/');
    std::string e = ext;
    if (v.size() <= e.size() || v.compare(v.size() - e.size(), e.size(), e) != 0) return std::string();
    return v.substr(0, v.size() - e.size());
}
std::string short_name(const std::string &p) {
    size_t sl = p.rfind('/');
    return sl == std::string::npos ? p : p.substr(sl + 1);
}
std::string name_list(const std::vector<std::string> &v, size_t most = 4) {
    std::string o;
    for (size_t i = 0; i < v.size() && i < most; i++) o += (i ? ", " : "") + v[i];
    if (v.size() > most) o += " and " + std::to_string(v.size() - most) + " more";
    return o;
}
}   // namespace

bool analyze_map_traits(const Config &cfg, const fs::path &map, MapTraits &t, std::string *err) {
    t = MapTraits();
    std::error_code ec;
    fs::path dir = cfg.cache_dir / "work" / "traits";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    std::string out;
    auto fail = [&](const std::string &why) {
        if (err) *err = why;
        fs::remove_all(dir, ec);
        return false;
    };
    if (run_process({u8(tool(cfg, "invader-extract")), "-G", "-n", "-t", u8(dir), "-s", "*.scenario", "-s",
                     "globals/globals.globals", "-s", "*.item_collection", u8(map)}, out) != 0)
        return fail("couldn't read the map's scenario: " + out.substr(out.size() > 300 ? out.size() - 300 : 0));
    std::string scen;
    std::vector<std::string> collections;
    for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::string rel = it->path().lexically_relative(dir).generic_u8string();
        if (it->path().extension() == ".scenario" && scen.empty()) scen = rel;
        else if (it->path().extension() == ".item_collection") collections.push_back(rel);
    }
    if (scen.empty()) return fail("the map has no scenario tag");
    auto lines = [&](const std::string &tag, const std::string &key) {
        std::vector<std::string> r;
        std::string v;
        if (run_process({u8(tool(cfg, "invader-edit")), "-t", u8(dir), tag, "-G", key}, v) != 0) return r;
        std::istringstream ss(v);
        std::string l;
        while (std::getline(ss, l)) {
            while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
            r.push_back(l);
        }
        return r;
    };

    // Gametypes: Halo needs the map's markers for each (Slayer only needs spawn points).
    auto types = lines(scen, "netgame_flags[*].type");
    auto usage = lines(scen, "netgame_flags[*].usage_id");
    bool red = false, blue = false;
    int ctf = 0, hill = 0, ball = 0, race = 0;
    for (size_t i = 0; i < types.size(); i++) {
        const std::string &ty = types[i];
        if (ty == "ctf_flag") {
            ctf++;
            if (i < usage.size()) { red |= usage[i] == "0"; blue |= usage[i] == "1"; }
        } else if (ty == "hill_flag") hill++;
        else if (ty == "oddball_ball_spawn") ball++;
        else if (ty == "race_track") race++;
    }
    if (usage.size() != types.size()) red = blue = ctf >= 2;   // (no team numbers to read)
    auto add_type = [&](const char *n) { t.gametypes += (t.gametypes.empty() ? "" : ", ") + std::string(n); };
    if (red && blue) add_type("CTF");
    if (hill) add_type("King");
    if (ball) add_type("Oddball");
    if (race >= 2) add_type("Race");
    add_type("Slayer");

    // Players: a guess from the map's size and what it has, against Halo's 19 multiplayer maps (the Xbox's 13 and Custom
    // Edition's 6, with the player counts their menus give):
    //  - how far apart the spawn points are (the box they fill), the strongest sign: a vehicle map or a box over 7,000
    //    cubic metres is a 4-16 map, 2,000-7,000 a 4-8 one, smaller a 2-8 one;
    //  - placed vehicles: 4 or more make it a big team map;
    //  - the number of spawn points: no more players than that.
    // That gives Bungie's range for 16 of the 19 (it misses Rat Race and Chiron TL34, 2-6, and Danger Canyon, 2-12).
    double lo[3] = {1e30, 1e30, 1e30}, hi[3] = {-1e30, -1e30, -1e30};
    for (auto &p : lines(scen, "player_starting_locations[*].position")) {
        double v[3];
        if (sscanf(p.c_str(), "%lf,%lf,%lf", &v[0], &v[1], &v[2]) != 3) continue;
        t.spawns++;
        for (int k = 0; k < 3; k++) { lo[k] = std::min(lo[k], v[k]); hi[k] = std::max(hi[k], v[k]); }
    }
    {
        std::string v;
        run_process({u8(tool(cfg, "invader-edit")), "-t", u8(dir), scen, "-C", "vehicles"}, v);
        t.vehicles = std::atoi(v.c_str());
    }
    if (t.spawns >= 2) {
        for (int k = 0; k < 3; k++) t.spread[k] = hi[k] - lo[k];
        double vol = t.spread[0] * t.spread[1] * std::max(2.0, t.spread[2]);
        if (t.vehicles >= 4 || vol >= 7000) { t.pmin = 4; t.pmax = 16; }
        else if (vol >= 2000) { t.pmin = 4; t.pmax = 8; }
        else { t.pmin = 2; t.pmax = 8; }
        if (t.spawns < t.pmax) t.pmax = std::max(2, t.spawns);
        t.pmin = std::min(t.pmin, t.pmax);
        // the stock maps it's closest to (box size and vehicles), to show what the guess is based on
        struct Stock { const char *name; double vol; int vehicles; };
        static const Stock stock[] = {
            {"Battle Creek", 999, 0}, {"Sidewinder", 41427, 8}, {"Damnation", 5043, 0}, {"Rat Race", 2623, 0}, {"Prisoner", 1716, 0},
            {"Hang 'Em High", 7893, 0}, {"Chill Out", 1238, 0}, {"Derelict", 2152, 0}, {"Boarding Action", 10764, 0},
            {"Blood Gulch", 18903, 8}, {"Wizard", 1072, 0}, {"Chiron TL34", 6178, 0}, {"Longest", 1269, 0},
            {"Danger Canyon", 107471, 18}, {"Death Island", 175454, 46}, {"Gephyrophobia", 116585, 24}, {"Ice Fields", 135024, 24},
            {"Infinity", 264074, 40}, {"Timberland", 85621, 24}};
        std::vector<std::pair<double, const char *>> closest;
        for (auto &m : stock) {
            double a = 3 * (std::log10(std::max(vol, 1.0)) - std::log10(m.vol));
            double b = 0.5 * (std::log2(1.0 + t.vehicles) - std::log2(1.0 + m.vehicles));
            closest.push_back({a * a + b * b, m.name});
        }
        std::sort(closest.begin(), closest.end());
        char why[200];
        snprintf(why, sizeof why, "spawn points spread over %.0f x %.0f x %.0f m, %d spawn point%s, %d vehicle%s; most like %s, %s and %s",
                 t.spread[0], t.spread[1], t.spread[2], t.spawns, t.spawns == 1 ? "" : "s", t.vehicles, t.vehicles == 1 ? "" : "s",
                 closest[0].second, closest[1].second, closest[2].second);
        t.players_why = why;
    }

    // What the players get: weapons placed, in item collections and in the starting profiles; vehicles placed or
    // in the multiplayer vehicle list; the multiplayer character.
    std::set<std::string> weapons, vehicles;
    for (auto &v : lines(scen, "weapon_palette[*].name")) if (auto r = ref_path(v, ".weapon"); !r.empty()) weapons.insert(r);
    for (const char *k : {"primary_weapon", "secondary_weapon"})
        for (auto &v : lines(scen, std::string("player_starting_profile[*].") + k))
            if (auto r = ref_path(v, ".weapon"); !r.empty()) weapons.insert(r);
    for (auto &ic : collections)
        for (auto &v : lines(ic, "permutations[*].item")) if (auto r = ref_path(v, ".weapon"); !r.empty()) weapons.insert(r);
    for (auto &v : lines(scen, "vehicle_palette[*].name")) if (auto r = ref_path(v, ".vehicle"); !r.empty()) vehicles.insert(r);
    const std::string g = "globals/globals.globals";
    if (fs::exists(dir / g))
        for (auto &v : lines(g, "multiplayer_information[0].vehicles[*].vehicle"))
            if (auto r = ref_path(v, ".vehicle"); !r.empty()) vehicles.insert(r);
    int stock_guns = 0;
    for (auto &w : weapons) {
        if (!listed(kStockWeapons, std::size(kStockWeapons), w)) t.custom_weapons.push_back(short_name(w));
        else if (short_name(w) != "ball" && short_name(w) != "flag") stock_guns++;
    }
    for (auto &v : vehicles)
        if (!listed(kStockVehicles, std::size(kStockVehicles), v)) t.custom_vehicles.push_back(short_name(v));
    if (fs::exists(dir / g)) {
        auto u = lines(g, "multiplayer_information[0].unit");
        std::string r = u.empty() ? std::string() : ref_path(u[0], ".biped");
        if (!r.empty() && !listed(kStockCharacters, std::size(kStockCharacters), r)) t.character = short_name(r);
    }
    std::vector<std::string> why;
    if (!t.custom_vehicles.empty()) {
        t.genres.push_back("Custom vehicles");
        why.push_back("vehicles " + name_list(t.custom_vehicles));
    }
    if (!t.custom_weapons.empty()) {
        t.genres.push_back("Custom weapons");
        why.push_back("weapons " + name_list(t.custom_weapons));
    }
    bool tc = !t.character.empty() || (t.custom_weapons.size() >= 4 && stock_guns <= 1);
    if (tc) {
        t.genres.push_back("Total conversions");
        why.push_back(!t.character.empty() ? "its own multiplayer character (" + t.character + ")"
                                           : "hardly any of Halo's weapons");
    }
    for (size_t i = 0; i < why.size(); i++) t.why_genres += (i ? "; " : "") + why[i];
    fs::remove_all(dir, ec);
    return true;
}

bool analyze_level_shaders(const Config &cfg_in, const fs::path &input, Game in_game, fs::path &file, std::string &error, const Log &log) {
    Config cfg = cfg_in;
    cfg.level_shaders_only = true;
    file.clear();
    fs::path dummy = cfg.cache_dir / "shaders" / (input.stem().u8string() + ".map");
    Result r = in_game == Game::Unknown ? build_from_tags_impl(cfg, input, Game::Xbox, dummy, false, log, nullptr)
                                        : convert_map_impl(cfg, input, in_game, Game::Xbox, dummy, false, log, nullptr);
    if (!r.ok) { error = r.error.empty() ? "The map couldn't be measured." : r.error; return false; }
    file = r.shaders;
    return true;
}

bool analyze_xbox_contents(const Config &cfg_in, const fs::path &input, Game in_game, ContentAnalysis &out, const Log &log) {
    Config cfg = cfg_in;
    cfg.xbox_replace.clear();   // measure the map as it is, without the picks
    out = ContentAnalysis();
    fs::path dummy = cfg.cache_dir / "analyze" / (input.stem().u8string() + ".map");
    Result r = in_game == Game::Unknown ? build_from_tags_impl(cfg, input, Game::Xbox, dummy, false, log, &out)
                                        : convert_map_impl(cfg, input, in_game, Game::Xbox, dummy, false, log, &out);
    if (!r.ok) out.error = r.error.empty() ? "The map couldn't be measured." : r.error;
    return r.ok;
}
} // namespace porter
