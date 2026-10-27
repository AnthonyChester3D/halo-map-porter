// Halo Map Porter - conversion engine
#include "core.hpp"
#include "animcomp.hpp"
#include "collision.hpp"
#include "effects.hpp"
#include "fpview.hpp"
#include "inflate.hpp"
#include "lod.hpp"
#include "perf.hpp"
#include "levelshaders.hpp"
#include "showcase.hpp"
#include "portals.hpp"
#include "preview.hpp"
#include "sky.hpp"
#include "tagedit.hpp"
#include "tagfile.hpp"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <array>
#include <cstring>
#include <cmath>
#include <chrono>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <ctime>
#include <sstream>
#include <thread>
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

// invader-edit, invader-dependency and invader-refactor are done in this process (tagedit.hpp) when they can be:
// starting the tool costs more than the work, and a conversion asks them hundreds of times. HMP_NO_TAGEDIT set: always
// run the tools.
static bool in_process(const std::vector<std::string> &args, std::string &output, int &code) {
    if (args.empty()) return false;
    static const bool off = getenv("HMP_NO_TAGEDIT") != nullptr;
    if (off) return false;
    std::string exe = fs::path(args[0]).filename().u8string();
    for (auto &c : exe) c = (char)tolower((unsigned char)c);
    if (exe.size() > 4 && exe.compare(exe.size() - 4, 4, ".exe") == 0) exe.resize(exe.size() - 4);
    std::vector<std::string> rest(args.begin() + 1, args.end());
    if (exe == "invader-edit") return tagedit::run_edit(rest, output, code);
    if (exe == "invader-dependency") return tagedit::run_dependency(rest, output, code);
    if (exe == "invader-refactor") return tagedit::run_refactor(rest, output, code);
    return false;
}

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
    {
        int code;
        if (in_process(args, output, code)) return code;
    }
    std::wstring cmd;
    for (auto &a : args) { if (!cmd.empty()) cmd += L' '; cmd += quote_arg(widen(a)); }

    // (one tool started at a time: a tool started while another's pipe is being set up would inherit that pipe, and
    // its reader would wait for both to finish)
    static std::mutex start_lock;
    std::unique_lock<std::mutex> starting(start_lock);
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
    starting.unlock();
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
    {
        int code;
        if (in_process(args, output, code)) return code;
    }
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
int convert_bc7_kept(const Config &cfg, const fs::path &p);
std::string norm_rel(std::string s);
std::vector<std::pair<std::string, std::string>> list_values(const Config &cfg, const fs::path &tags, const std::string &tag);

struct Job {
    const Config &cfg;
    const Log &log;
    Game target;
    fs::path work, fixes;
    fs::path overlay;  // stock tags of target game (optional)
    std::set<std::string> alpha_tested;   // bitmaps a shader alpha-tests (find_alpha_tested: halve_cutout)
    std::string perf_tour;                // a camera tour for the map (make_perf_tour), written beside it
    std::string showcase;                 // its showcase flythrough (showcase.hpp), written into its map folder
    std::string showcase_note;            // (what it is, for the log)
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
    std::vector<fs::path> resource_files; // convert: the bitmaps.map/sounds.map/loc.map files read
    std::shared_ptr<void> extracted;      // convert: keeps the kept extraction it reads (find_extracted) while it runs
    ContentAnalysis *measure = nullptr;   // convert: measure here too when the map doesn't fit (see convert_map)
    // Models the app made lower-detail versions for, as they were before (to take them out again when the map
    // doesn't fit the Xbox), and what those versions take in the Xbox map.
    std::vector<std::pair<fs::path, std::string>> lod_backups;
    std::vector<std::pair<fs::path, std::string>> portal_backups;   // levels the app gave portals, as they were
    size_t lod_xbox_bytes = 0;
    std::vector<LodView::Model> lod_changes;   // the models whose levels of detail changed (for the LODs window)
    std::map<std::string, std::string> trim_originals;   // trimmed models (tag) as the map made them, for that window
    // For "Update the map" (update_xbox_models): what xbox_lods was given, and each model file it changed as it was
    // before (path -> bytes), kept with the work folder; `update`: this run only redoes that on the tags prepared.
    std::vector<std::string> lod_deps;
    std::map<fs::path, std::string> model_originals;
    std::set<fs::path> model_created;   // (copies xbox_lods made in fixes: taken out again rather than restored)
    bool update = false;
    bool optimize = false;   // update: the conversion built with Invader's --optimize
    std::string only_model;  // xbox_lods: this model alone (the LODs window's "Make again")
    bool had_picks = false;  // update: the conversion used weapons & vehicles picks
    bool textures_fitted = false;   // the texture memory target was tried (once is enough: "Update the map" keeps it)
    // A build cut the prepared tags down to fit (textures halved, kept vehicles dropped): building them again isn't
    // what a conversion from scratch would do with other options (build_prepared won't).
    bool cut = false;
    // The scripts window's edits the Xbox build's scripts came from (user_scripts_hash; "" the map's own): "Update the
    // map" and Convert from prepared tags bring them in again when they changed since (xbox_scripts).
    std::string scripts_hash;
    struct CollisionChange {   // a model whose collision changed, for that window too
        std::string model, name, text;   // its model (tag), the object it's for, how it changed
        char kind = 's';
        ModelPreview made, now;          // its collision drawn as made, as converted
    };
    std::vector<CollisionChange> collision_changes;
    // What merge_map_globals added to the Xbox's HUD lists from the map's, for trim_pickup_extras to keep only what
    // the map's weapons, items and vehicles still use: the map's lists now used (item_message_text and
    // alternate_icon_text: tag paths, "" none) and how many entries the Xbox's own have; the icon sheet made from the
    // Xbox's and the map's (shrunk) copy, how many sequences the Xbox's has, and which of the map's follow them.
    struct PickupExtras {
        std::string hud_globals;          // the HUD globals that use them (the Xbox's, edited)
        std::string lists[2];
        int xbox_count[2] = {0, 0};
        std::string sheet, xbox_sheet_tag;
        fs::path xbox_sheet, map_sheet;
        uint32_t xbox_seqs = 0;
        std::vector<int> src;             // the map's sequences after the Xbox's (merge_icon_sheets_from)
    } pickup;
    // The HUD tags' canvas sizes as the map has them (MCC: 1 = 960p, positions and sizes doubled; 0 = 480p), by tag
    // path; known: read from the map (or your tags), else hud_960 guesses from the stock HUDs.
    std::map<std::string, int> hud_canvas;
    bool hud_canvas_known = false;
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
            if (!own.empty() && (own == fixes || to_fixes(tag, own)) && convert_bc7_kept(cfg, fixes / from_u8(tag)) > 0) {
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
size_t shrink_textures_to_fit(Job &job, size_t needed, int &count, bool hard = true);
void restrict_to_xbox_mp(Job &job, const fs::path &xsrc);
void drop_dead_placements(Job &job, const fs::path &xsrc);
void trim_machines(Job &job, const fs::path &xsrc);
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
static void tidy_weapon_list(Job &job);
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
void reorder_multipurpose_maps(Job &job, const std::vector<std::string> &deps);
std::map<std::string, bool> level_bump_maps(Job &job, const std::vector<std::string> &deps);
std::vector<std::string> tag_dependencies(Job &job, const std::string &tag, bool recursive);
void xbox_lods(Job &job, const std::vector<std::string> &deps);
void xbox_portals(Job &job);
void water_volumes(Job &job);
void xbox_lights(Job &job, const std::vector<std::string> &deps);
void xbox_materials(Job &job, const std::vector<std::string> &deps);
void xbox_reflections(Job &job);
void xbox_fog(Job &job);
void xbox_sky(Job &job);
void xbox_scenery(Job &job, const std::vector<std::string> &deps);
void xbox_scenery_sizes(Job &job, const std::vector<std::string> &deps);
void xbox_weather(Job &job);
void xbox_props(Job &job);
static int g_cutout_textures = 0;   // cut-out textures halve_cutout made smaller (for the log)
static std::string thousands(long long v);
static std::vector<std::string> performance_lines(Job &job);
static LodView make_lod_view(Job &job, const std::string &map_name);
static void convert_scripts(Job &job, const fs::path &dir, Game target);
static void xbox_scripts(Job &job, const fs::path &xsrc);
static std::string user_scripts_hash(const Config &cfg);
static void pc_user_scripts(Job &job, const std::string &scenario);
static void note_script_tricks(Job &job, const fs::path &xsrc);
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

// What "Update the map" (and the LODs window's "Make again", and the weapons & vehicles window measuring the
// prepared tags) need, kept with the work folder: the tag folders, the scenario and output, what xbox_lods was given,
// and the models it changed as they were (each in originals/).
static void save_update(Job &job, const fs::path &output, const fs::path &lods, bool share, bool optimized) {
    std::error_code ec;
    fs::path dir = job.work / "originals";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    std::ostringstream f;
    f << "HaloMapPorter update 1\n";
    auto line = [&](const char *k, const std::string &v) { f << k << "\t" << v << "\n"; };
    line("work", u8(job.work));
    line("fixes", u8(job.fixes));
    line("overlay", u8(job.overlay));
    line("source", u8(job.source));
    line("user_tags", u8(job.user_tags));
    line("scenario", job.scenario);
    line("output", u8(output));
    line("lods", u8(lods));
    line("share", share ? "1" : "0");
    line("optimized", optimized ? "1" : "0");
    line("from", std::to_string((int)job.from));
    line("picks", (job.update ? job.had_picks : !job.cfg.xbox_replace.empty()) ? "1" : "0");
    line("textures_fitted", job.textures_fitted ? "1" : "0");
    line("cut", job.cut ? "1" : "0");
    line("scripts", job.scripts_hash);
    for (auto &a : job.extra) line("extra", a);
    for (auto &d : job.lod_deps) line("dep", d);
    int n = 0;
    for (auto &[file, bytes] : job.model_originals) {
        std::string blob = std::to_string(n++) + ".bin";
        std::ofstream(dir / blob, std::ios::binary) << bytes;
        f << "orig\t" << u8(file) << "\t" << blob << "\n";
    }
    for (auto &file : job.model_created) line("made", u8(file));
    f << "end\n";
    std::ofstream(job.work / "update.txt", std::ios::binary | std::ios::trunc) << f.str();
}

// A conversion's update.txt (save_update) read into `job` (job.update set). False (`err`) when it, or the tags it
// names, are gone (another conversion or build since).
struct UpdateFile { std::string scenario; fs::path output, lods; bool share = false, picks = false; };
static bool load_update(Job &job, const fs::path &file, UpdateFile &u, std::string &err) {
    const char *gone = "The tags the conversion prepared are gone (another conversion or build since): convert the map again.";
    std::ifstream f(file, std::ios::binary);
    std::string l;
    if (!std::getline(f, l) || (l != "HaloMapPorter update 1" && l != "HaloMapPorter update 1\r")) { err = gone; return false; }
    job.update = true;
    bool end = false;
    while (std::getline(f, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (l == "end") { end = true; break; }
        size_t t = l.find('\t');
        if (t == std::string::npos) continue;
        std::string k = l.substr(0, t), v = l.substr(t + 1);
        if (k == "work") job.work = from_u8(v);
        else if (k == "fixes") job.fixes = from_u8(v);
        else if (k == "overlay") job.overlay = from_u8(v);
        else if (k == "source") job.source = from_u8(v);
        else if (k == "user_tags") job.user_tags = from_u8(v);
        else if (k == "scenario") u.scenario = v;
        else if (k == "output") u.output = from_u8(v);
        else if (k == "lods") u.lods = from_u8(v);
        else if (k == "share") u.share = v == "1";
        else if (k == "optimized") job.optimize = v == "1";
        else if (k == "from") job.from = (Game)std::atoi(v.c_str());
        else if (k == "picks") u.picks = job.had_picks = v == "1";
        else if (k == "textures_fitted") job.textures_fitted = v == "1";
        else if (k == "cut") job.cut = v == "1";
        else if (k == "scripts") job.scripts_hash = v;
        else if (k == "extra") job.extra.push_back(v);
        else if (k == "dep") job.lod_deps.push_back(v);
        else if (k == "made") job.model_created.insert(from_u8(v));
        else if (k == "orig") {
            size_t t2 = v.find('\t');
            if (t2 == std::string::npos) { err = gone; return false; }
            std::ifstream b(job.work / "originals" / from_u8(v.substr(t2 + 1)), std::ios::binary);
            if (!b) { err = gone; return false; }
            job.model_originals[from_u8(v.substr(0, t2))] =
                std::string((std::istreambuf_iterator<char>(b)), std::istreambuf_iterator<char>());
        }
    }
    if (!end || u.scenario.empty() || u.output.empty() || !fs::exists(job.fixes) || !fs::exists(job.source) ||
        !fs::exists(job.source / from_u8(u.scenario + ".scenario")) || (!job.overlay.empty() && !fs::exists(job.overlay))) {
        err = gone;
        return false;
    }
    for (auto &[p, b] : job.model_originals)
        if (!fs::exists(p.parent_path())) { err = gone; return false; }
    job.scenario = u.scenario;
    return true;
}

// A LodView's drawings kept only where a model refers to them (renumbered), so one drawn again doesn't keep the old.
static void compact_lod_view(LodView &v) {
    std::map<int, int> keep;
    auto use = [&](int &i) {
        if (i < 0 || i >= (int)v.previews.size()) { i = -1; return; }
        auto it = keep.find(i);
        if (it == keep.end()) it = keep.emplace(i, (int)keep.size()).first;
        i = it->second;
    };
    for (auto &m : v.models) {
        for (int &k : m.level) use(k);
        use(m.as_made);
        use(m.coll_made);
        use(m.coll_now);
    }
    std::vector<ModelPreview> p(keep.size());
    for (auto &[from, to] : keep) p[(size_t)to] = std::move(v.previews[(size_t)from]);
    v.previews = std::move(p);
}

// A model's collision as the window had it (its text and drawings: they come from the conversion, which "Update the
// map" and "Make again" don't redo), carried into `now` from `before`.
static void keep_collision(LodView &now, const LodView &before) {
    for (auto &b : before.models) {
        if (b.collision.empty()) continue;
        auto it = std::find_if(now.models.begin(), now.models.end(), [&](const LodView::Model &m) { return m.tag == b.tag; });
        if (it == now.models.end()) {
            LodView::Model m = b;
            for (int &k : m.level) k = -1;
            m.as_made = -1;
            now.models.push_back(m);
            it = now.models.end() - 1;
        }
        it->collision = b.collision;
        it->coll_made = it->coll_now = -1;
        if (b.coll_made >= 0 && b.coll_made < (int)before.previews.size()) {
            it->coll_made = (int)now.previews.size();
            now.previews.push_back(before.previews[(size_t)b.coll_made]);
        }
        if (b.coll_now >= 0 && b.coll_now < (int)before.previews.size()) {
            it->coll_now = (int)now.previews.size();
            now.previews.push_back(before.previews[(size_t)b.coll_now]);
        }
    }
}

static void level_extras_for_update(Job &job);

Result run_build(Job &job, const std::string &scenario, const fs::path &output, bool share) {
    job.scenario = scenario;
    Result r;
    const Config &cfg = job.cfg;
    std::error_code ec;
    fs::create_directories(output.parent_path(), ec);
    fs::path tmp_out = job.work / "out.map";
    if (job.update) {
        // "Update the map": the tags the conversion prepared, its models as they were before their levels of detail
        // and trims, those done again (the LODs window's choices and the Xbox tab's options now), and the build.
        job.log("Redoing the models' levels of detail and trims on the tags the conversion prepared...");
        std::error_code rec;
        for (auto &[file, bytes] : job.model_originals)
            if (!job.model_created.count(file)) std::ofstream(file, std::ios::binary | std::ios::trunc) << bytes;
        for (auto &file : job.model_created) fs::remove(file, rec);
        std::vector<std::string> deps = job.lod_deps;
        job.model_originals.clear();
        job.model_created.clear();
        xbox_scenery_sizes(job, deps);
        xbox_lods(job, deps);
        xbox_scenery(job, deps);   // (its copies were taken out above with the levels of detail's)
        if (!job.analysis && !cfg.xbox_prepare_only) level_extras_for_update(job);
        // The scripts window's edits changed since the tags were prepared (or were taken back): the scripts again, as
        // they are now (the rest of the map as prepared).
        if (!job.analysis && !cfg.xbox_prepare_only && user_scripts_hash(cfg) != job.scripts_hash) {
            job.log(job.scripts_hash.empty() && !user_scripts_hash(cfg).empty()
                        ? "Bringing in your edited scripts (the scripts window's)..."
                        : user_scripts_hash(cfg).empty() ? "Going back to the map's own scripts (your edits were taken back)..."
                                                         : "Bringing in your edited scripts as they are now...");
            xbox_scripts(job, job.source);
        }
    } else {
        keep_map_level_tags(job);
        std::string err;
        if (job.target == Game::Xbox) {
            if (!prepare_for_xbox(job, err)) { r.error = err; return r; }
        } else {
            if (!prepare_models_for_pc(job, err)) { r.error = err; return r; }
        }
        if (!job.user_tags.empty() && !job.overlay.empty() && job.target != Game::MCC) keep_user_edits(job);
        if (job.target != Game::MCC) convert_bc7_textures(job);
        if (job.target == Game::Xbox) xbox_content_passes(job);
        else if (job.from == Game::Xbox) reorder_multipurpose_maps(job, tag_dependencies(job, job.scenario + ".scenario", true));
        water_volumes(job);
        place_kept_level_tags(job);
    }
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
    if (job.target == Game::Xbox && !job.analysis &&
        (!job.lod_changes.empty() || !job.collision_changes.empty() || cfg.xbox_prepare_only)) {
        lod_view = make_lod_view(job, output.stem().u8string());
        lod_file = job.cfg.cache_dir / "lods" / (output.stem().u8string() + ".txt");
        LodView before;
        if (job.update && load_lod_view(before, lod_file)) keep_collision(lod_view, before);   // (not redone)
        if (save_lod_view(lod_view, lod_file)) r.lods = lod_file;
    }

    std::string stem = output.stem().u8string();
    bool optimized = job.optimize;   // Invader's --optimize: identical data stored once (only when the tag space is short)
    // What "Update the map" needs (update_xbox_models), kept with the work folder whatever the build's outcome: the
    // tag folders, the scenario and output, what xbox_lods was given and the models it changed as they were.
    struct KeepUpdate {
        Job &job;
        const fs::path &output, &lods;
        bool share;
        const bool &optimized;
        ~KeepUpdate() {
            if (job.target != Game::Xbox || job.analysis || job.lod_deps.empty() || job.cfg.level_shaders_only) return;
            save_update(job, output, lods, share, optimized);
        }
    } keep_update{job, output, lod_file, share, optimized};
    if (job.target == Game::Xbox && !job.analysis && !job.lod_deps.empty() && !cfg.level_shaders_only)
        r.update = job.work / "update.txt";
    if (cfg.xbox_prepare_only && job.target == Game::Xbox && !job.analysis) {   // (as far as the LODs window)
        job.log("Prepared: the map isn't built yet (Convert, or the LODs window's \"Build the map\", builds it from these tags).");
        r.ok = true;
        return r;
    }
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
            // (the map's header says it: Invader writes the size it decompresses to there)
            double mb = 0;
            {
                std::ifstream hf(tmp_out, std::ios::binary);
                std::string h(0x800, '\0');
                hf.read(&h[0], 0x800);
                auto u32 = [&](size_t o) {
                    const unsigned char *q = (const unsigned char *)h.data() + o;
                    return (uint32_t)q[0] | q[1] << 8 | q[2] << 16 | (uint32_t)q[3] << 24;
                };
                if (hf.gcount() == 0x800 && h.compare(0, 4, "daeh") == 0 && h.compare(0x7FC, 4, "toof") == 0 && u32(8) >= 0x800)
                    mb = u32(8) / 1048576.0;
            }
            if (mb == 0) {
                std::string sz;
                run_process({u8(tool(cfg, "invader-info")), "-T", "uncompressed_size", u8(tmp_out)}, sz);
                mb = std::atof(sz.c_str()) / 1048576.0;
            }
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
                // (the conversion's own build tried already: "Update the map" keeps its textures as they are, rather
                // than halving them again each time it's used)
                if (target > 0 && use.texture_mb > budget + 0.05) {
                    if (cfg.xbox_shrink && texture_rounds < 6 && !texture_hopeless && !job.textures_fitted) {
                        size_t need = (size_t)((use.texture_mb - budget) * 1048576.0) + 256 * 1024;
                        int n = 0;
                        g_cutout_textures = 0;
                        size_t saved = shrink_textures_to_fit(job, need, n, false);
                        if (saved > 0) {
                            job.cut = true;
                            texture_rounds++;
                            snprintf(buf, sizeof buf, "[textures] %.1f MB of textures is over the %.1f MB target; halved %d of "
                                     "your textures (saved %.1f MB%s)", use.texture_mb, budget, n, saved / 1048576.0,
                                     g_cutout_textures ? "; see-through cut-outs kept as solid" : "");
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
                job.textures_fitted = true;   // (as far as it goes)
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
            g_cutout_textures = 0;
            size_t saved = shrink_textures_to_fit(job, need, n);
            if (saved > 0) {
                job.cut = true;
                shrink_rounds++;
                job.note("[shrink] " + tb[1].str().substr(0, 5) + " MB is over the limit; halved " + std::to_string(n) +
                         " of your textures (saved " + std::to_string(saved / 1048576.0).substr(0, 4) + " MB" +
                         (g_cutout_textures ? "; see-through cut-outs kept as solid" : "") + ")");
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
            job.cut = true;
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
    if (job.target == Game::Xbox && !job.perf_tour.empty()) {
        // Named as Halo 1 Unlimited names the map's logs: its internal name (the file's, lower case), letters, digits
        // and - kept, anything else _.
        std::string name = stem;
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        for (auto &c : name)
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) c = '_';
        name = name.substr(0, 31);
        fs::path tour = output.parent_path() / "perf" / "tours" / from_u8(name + ".txt");
        fs::create_directories(tour.parent_path(), ec);
        std::ofstream tf(tour, std::ios::binary);
        if (tf << job.perf_tour) {
            int n = 0;
            for (size_t i = 0; i < job.perf_tour.size(); i++)
                if (job.perf_tour[i] == '\n' && i + 1 < job.perf_tour.size() && job.perf_tour[i + 1] != '#' &&
                    job.perf_tour.compare(i + 1, 5, "start") && job.perf_tour.compare(i + 1, 6, "settle") && job.perf_tour.compare(i + 1, 4, "hold"))
                    n++;
            job.note("[tour] wrote a camera tour for Halo 1 Unlimited's perf log (" + std::to_string(n) + " stops): " + u8(tour) +
                     ". Copy it to perf\\tours beside the XBE. The log names the map by how the Xbox lists it (in a folder "
                     "\"_Unsorted\": _Unsorted_" + name + "), so rename the file to match the CSV's name if it differs");
        }
    }
    if (job.target == Game::Xbox && !job.showcase.empty() && fs::exists(output.parent_path() / "info.txt", ec)) {
        // Into the map's folder, beside info.txt (Halo 1 Unlimited's showcase screensaver reads it there), unless one
        // the app didn't write is there already (a map maker's own).
        fs::path sf = output.parent_path() / "showcase.txt";
        std::string had;
        {
            std::ifstream in(sf, std::ios::binary);
            if (in) had.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }
        if (!had.empty() && !showcase::ours(had)) {
            job.note("[showcase] kept the map folder's own showcase.txt (not one this app wrote): " + u8(sf));
        } else {
            std::ofstream of(sf, std::ios::binary | std::ios::trunc);
            if (of << job.showcase)
                job.note("[showcase] wrote the flythrough Halo 1 Unlimited's showcase screensaver flies on this map, " +
                         job.showcase_note + ": " + u8(sf));
        }
    }
    r.changes = job.changes;

    fs::path reports = cfg.cache_dir / "reports";
    fs::create_directories(reports, ec);
    r.report = reports / (stem + " - " + game_name(job.target) + ".txt");
    if (job.update) {   // (the conversion's report, with what the update changed after it)
        std::ofstream rep(r.report, std::ios::app);
        std::time_t now = std::time(nullptr);
        char when[32];
        std::strftime(when, sizeof when, "%Y-%m-%d %H:%M", std::localtime(&now));
        rep << "\nUpdated from the LODs window (" << when << "): levels of detail and trims done again\n";
        if (!r.memory.empty()) rep << "Xbox memory: " << r.memory << "\n";
        for (auto &c : r.changes) rep << "  " << c << "\n";
        for (auto &w : r.warnings) rep << "  " << w << "\n";
        return r;
    }
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
// The files are copied on several threads at once (a map's tags are thousands of small files).
bool copy_tree(const fs::path &from, const fs::path &to, bool overwrite, std::error_code &ec) {
    ec.clear();
    fs::create_directories(to, ec);
    if (ec) return false;
    std::vector<std::pair<fs::path, fs::path>> files;
    for (auto it = fs::recursive_directory_iterator(from, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        fs::path dst = to / fs::relative(it->path(), from);
        if (it->is_directory()) {
            fs::create_directories(dst, ec);
        } else if (it->is_regular_file()) {
            if (!overwrite && fs::exists(dst)) continue;
            files.push_back({it->path(), dst});
        }
        if (ec) return false;
    }
    if (ec) return false;
    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    std::mutex err_lock;
    auto work = [&] {
        for (size_t i; !failed && (i = next++) < files.size();) {
            std::error_code e;
            fs::copy_file(files[i].first, files[i].second, fs::copy_options::overwrite_existing, e);
            if (e) {
                std::lock_guard<std::mutex> g(err_lock);
                if (!failed) ec = e;
                failed = true;
            }
        }
    };
    unsigned n = std::clamp(std::thread::hardware_concurrency(), 2u, 8u);
    if (files.size() < 64) n = 1;
    std::vector<std::thread> threads;
    for (unsigned t = 1; t < n; t++) threads.emplace_back(work);
    work();
    for (auto &t : threads) t.join();
    return !failed;
}

// ---------------------------------------------------------------- results kept between conversions

// Slow steps that turn tags into others the same way every time (re-encoding a sound for the Xbox, a BC7 texture as DXT,
// compressing a texture, packing a bump map, lightening the sky, measuring the level) keep what they made in
// cache_dir/results, by a hash of the tags they started from and how they were done; the same tags the next time (the
// same map converted again, or another map with them) get that copy instead. The least recently used results go
// first once they take more than kResultsKeptBytes.
namespace {
const uintmax_t kResultsKeptBytes = 768ull << 20;
std::mutex g_results_lock;

uint64_t mix64(uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27; x *= 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}
uint64_t hash64(const std::string &s, uint64_t seed) {
    uint64_t h = mix64(seed ^ s.size());
    size_t i = 0;
    for (; i + 8 <= s.size(); i += 8) {
        uint64_t w;
        memcpy(&w, s.data() + i, 8);
        h = mix64(h ^ w) * 0x9e3779b97f4a7c15ull;
    }
    uint64_t w = 0;
    memcpy(&w, s.data() + i, s.size() - i);
    return mix64(h ^ w ^ (uint64_t)(s.size() - i) << 56);
}
std::string read_all(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
bool write_all(const fs::path &p, const std::string &b) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    return (bool)f.write(b.data(), (std::streamsize)b.size());
}
}   // namespace

// What a result made from `input` by step `what` is kept under (32 hex characters). The app's build goes into it, so
// a new version of the app makes its results again.
static std::string result_key(const std::string &what, const std::string &input) {
    std::string k = std::string(__DATE__ " " __TIME__ "\n") + what + "\n" + input;
    char hex[40];
    snprintf(hex, sizeof hex, "%016llx%016llx", (unsigned long long)hash64(k, 0x243f6a8885a308d3ull),
             (unsigned long long)hash64(k, 0x13198a2e03707344ull));
    return hex;
}
static fs::path results_dir(const Config &cfg) { return cfg.cache_dir / "results"; }

// What's kept under `key` (and the note kept with it). False if there is nothing.
static bool load_blob(const Config &cfg, const std::string &key, std::string &b, std::string &note) {
    if (cfg.cache_dir.empty()) return false;
    fs::path r = results_dir(cfg) / (key + ".tag");
    std::error_code ec;
    std::lock_guard<std::mutex> g(g_results_lock);
    if (!fs::is_regular_file(r, ec)) return false;
    b = read_all(r);
    note = read_all(results_dir(cfg) / (key + ".note"));
    fs::last_write_time(r, fs::file_time_type::clock::now(), ec);   // (recently used)
    return !b.empty();
}

// The tag kept under `key` written to `p`, with the note kept with it. False if there is none.
static bool load_result(const Config &cfg, const std::string &key, const fs::path &p, std::string &note) {
    std::string b;
    return load_blob(cfg, key, b, note) && write_all(p, b);
}

static void store_blob(const Config &cfg, const std::string &key, const std::string &b, const std::string &note);

// Keeps the tag `p` as the result under `key`, with a note (e.g. how many textures it re-encoded).
static void store_result(const Config &cfg, const std::string &key, const fs::path &p, const std::string &note) {
    std::string b = read_all(p);
    if (!b.empty()) store_blob(cfg, key, b, note);
}

static void store_blob(const Config &cfg, const std::string &key, const std::string &b, const std::string &note) {
    if (cfg.cache_dir.empty() || b.empty()) return;
    std::lock_guard<std::mutex> g(g_results_lock);
    std::error_code ec;
    fs::path dir = results_dir(cfg);
    fs::create_directories(dir, ec);
    fs::path tmp = dir / (key + ".tmp");
    if (!write_all(tmp, b) || !write_all(dir / (key + ".note"), note)) { fs::remove(tmp, ec); return; }
    fs::rename(tmp, dir / (key + ".tag"), ec);
    if (ec) { fs::remove(tmp, ec); return; }
    // Within the size kept: the least recently used go.
    std::vector<std::pair<fs::file_time_type, fs::path>> all;
    uintmax_t total = 0;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (it->path().extension() != ".tag") continue;
        std::error_code e;
        uintmax_t s = it->file_size(e);
        total += e ? 0 : s;
        all.push_back({it->last_write_time(e), it->path()});
    }
    if (total <= kResultsKeptBytes) return;
    std::sort(all.begin(), all.end());
    for (auto &[t, f] : all) {
        if (total <= kResultsKeptBytes * 3 / 4) break;
        std::error_code e;
        uintmax_t s = fs::file_size(f, e);
        fs::path n = f;
        n.replace_extension(".note");
        if (fs::remove(f, e)) total -= s;
        fs::remove(n, e);
    }
}

// Records kept as results: texts, each its length and a newline before it.
static void put_text(std::string &b, const std::string &s) { b += std::to_string(s.size()) + "\n" + s; }
static bool get_text(const std::string &b, size_t &pos, std::string &s) {
    size_t nl = b.find('\n', pos);
    if (nl == std::string::npos) return false;
    size_t n = std::strtoull(b.c_str() + pos, nullptr, 10);
    if (nl + 1 + n > b.size()) return false;
    s = b.substr(nl + 1, n);
    pos = nl + 1 + n;
    return true;
}

// The identity of one of Invader's tools (its file's size and time), so a result made by an older version is made
// again.
static std::string tool_identity(const fs::path &exe) {
    std::error_code ec;
    auto size = fs::file_size(exe, ec);
    auto time = fs::last_write_time(exe, ec);
    return std::to_string((unsigned long long)size) + "/" + std::to_string((long long)time.time_since_epoch().count());
}

// A BC7 bitmap tag re-encoded as DXT (convert_bc7_bitmap_tag), kept: how many textures it re-encoded.
int convert_bc7_kept(const Config &cfg, const fs::path &p) {
    std::string key = result_key("bc7 as dxt 1", read_all(p)), note;
    if (load_result(cfg, key, p, note)) return std::atoi(note.c_str());
    int n = convert_bc7_bitmap_tag(p, true);
    if (n > 0) store_result(cfg, key, p, std::to_string(n));
    return n;
}

// A texture compressed to DXT (compress_color_bitmap_tag) and a bump map packed into less memory
// (compact_bump_bitmap_tag), kept the same way (the tag as it was then made, and what the step returned; a tag the
// step left as it was is kept too, so it isn't tried again).
int compress_color_bitmap_tag(const fs::path &p, bool write);
int compact_bump_bitmap_tag(const fs::path &p, bool alpha_tested, int *fmt_out);
static int compress_color_kept(const Config &cfg, const fs::path &p) {
    std::string key = result_key("compress color 1", read_all(p)), note;
    if (load_result(cfg, key, p, note)) return std::atoi(note.c_str());
    int n = compress_color_bitmap_tag(p, true);
    store_result(cfg, key, p, std::to_string(n));
    return n;
}
static int compact_bump_kept(const Config &cfg, const fs::path &p, bool alpha_tested, int *fmt_out) {
    std::string key = result_key(std::string("compact bump 1 ") + (alpha_tested ? "alpha tested" : "opaque"), read_all(p)), note;
    int n = 0, fmt = 0;
    if (load_result(cfg, key, p, note) && sscanf(note.c_str(), "%d %d", &n, &fmt) == 2) {
        if (fmt_out) *fmt_out = fmt;
        return n;
    }
    n = compact_bump_bitmap_tag(p, alpha_tested, &fmt);
    store_result(cfg, key, p, std::to_string(n) + " " + std::to_string(fmt));
    if (fmt_out) *fmt_out = fmt;
    return n;
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
static bool reencode_sound_now(Job &job, const fs::path &tags, const std::string &rel, std::string &why) {
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

// (The re-encoded tag is kept, unless it was made from the user's WAVs: the same sound tag again gets that copy.)
bool reencode_sound_for_xbox(Job &job, const fs::path &tags, const std::string &rel, std::string &why) {
    fs::path p = tags / from_u8(rel);
    std::error_code ec;
    bool wavs = !job.user_data.empty() && fs::is_directory(job.user_data / from_u8(rel.substr(0, rel.size() - 6)), ec);
    std::string key, note;
    if (!wavs) {
        key = result_key("xbox adpcm sound 1 " + tool_identity(tool(job.cfg, "invader-sound")), read_all(p));
        if (load_result(job.cfg, key, p, note)) return true;
    }
    if (!reencode_sound_now(job, tags, rel, why)) return false;
    if (!wavs) store_result(job.cfg, key, p, "");
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

// ---- Textures with see-through cut-outs (leaves, fences, grates, chain link)
// An alpha-tested surface draws a pixel only where its alpha is at least half. A smaller version made by averaging
// (the texture's own smaller mip levels, or a 2x2 box filter) blurs each cut-out edge into partly transparent pixels,
// most of them under half: leaves and grates thin out, and a couple of halvings down they all but vanish. A texture
// whose alpha is nearly all fully transparent or fully opaque is taken as cut out: when it's made smaller, each
// level is made again from the one above, its alpha scaled so the same share of it stays solid as in the full-size
// texture (alpha coverage, as texture tools do for foliage).

// The Xbox's bump maps are palettized (P8): a byte per pixel, picking one of 256 directions from the palette the Xbox
// sets for every texture stage at startup (rasterizer_xbox.c, global_vector_palette in bitmaps.c; Invader's p8_palette
// is the same). Bungie's PC copies are 32-bit, a quarter of the memory per pixel for the Xbox's. The first 249 are
// normals (alpha opaque); the rest are see-through (the last a flat normal).
static const uint32_t kXboxBumpPalette[256] = {
    0xFF7A19CC, 0xFF7E19CC, 0xFF8019CC, 0xFF8119CC, 0xFF8519CC, 0xFF742FE2, 0xFF7A2FE2, 0xFF7E2FE2,
    0xFF802FE2, 0xFF812FE2, 0xFF852FE2, 0xFF8B2FE2, 0xFF6B42ED, 0xFF7442EE, 0xFF7A42EF, 0xFF7E42EF,
    0xFF8042EF, 0xFF8142EF, 0xFF8542EF, 0xFF8B42EE, 0xFF9442ED, 0xFF6052F2, 0xFF6B52F5, 0xFF7452F6,
    0xFF7A52F7, 0xFF7E52F7, 0xFF8052F7, 0xFF8152F7, 0xFF8552F7, 0xFF8B52F6, 0xFF9452F5, 0xFF9F52F2,
    0xFF5260F2, 0xFF6060F7, 0xFF6B60F9, 0xFF7460FB, 0xFF7A60FB, 0xFF7E60FB, 0xFF8060FB, 0xFF8160FB,
    0xFF8560FB, 0xFF8B60FB, 0xFF9460F9, 0xFF9F60F7, 0xFFAD60F2, 0xFF426BED, 0xFF526BF5, 0xFF606BF9,
    0xFF6B6BFC, 0xFF746BFD, 0xFF7A6BFD, 0xFF7E6BFD, 0xFF806BFD, 0xFF816BFD, 0xFF856BFD, 0xFF8B6BFD,
    0xFF946BFC, 0xFF9F6BF9, 0xFFAD6BF5, 0xFFBD6BED, 0xFF2F74E2, 0xFF4274EE, 0xFF5274F6, 0xFF6074FB,
    0xFF6B74FD, 0xFF7474FE, 0xFF7A74FE, 0xFF7E74FE, 0xFF8074FE, 0xFF8174FE, 0xFF8574FE, 0xFF8B74FE,
    0xFF9474FD, 0xFF9F74FB, 0xFFAD74F6, 0xFFBD74EE, 0xFFD074E2, 0xFF197ACC, 0xFF2F7AE2, 0xFF427AEF,
    0xFF527AF7, 0xFF607AFB, 0xFF6B7AFD, 0xFF747AFE, 0xFF7A7AFF, 0xFF7E7AFF, 0xFF807AFF, 0xFF817AFF,
    0xFF857AFF, 0xFF8B7AFE, 0xFF947AFD, 0xFF9F7AFB, 0xFFAD7AF7, 0xFFBD7AEF, 0xFFD07AE2, 0xFFE57ACC,
    0xFF197ECC, 0xFF2F7EE2, 0xFF427EEF, 0xFF527EF7, 0xFF607EFB, 0xFF6B7EFD, 0xFF747EFE, 0xFF7A7EFF,
    0xFF7E7EFF, 0xFF807EFF, 0xFF817EFF, 0xFF857EFF, 0xFF8B7EFE, 0xFF947EFD, 0xFF9F7EFB, 0xFFAD7EF7,
    0xFFBD7EEF, 0xFFD07EE2, 0xFFE57ECC, 0xFF1980CC, 0xFF2F80E2, 0xFF4280EF, 0xFF5280F7, 0xFF6080FB,
    0xFF6B80FD, 0xFF7480FE, 0xFF7A80FF, 0xFF7E80FF, 0xFF8080FF, 0xFF8180FF, 0xFF8580FF, 0xFF8B80FE,
    0xFF9480FD, 0xFF9F80FB, 0xFFAD80F7, 0xFFBD80EF, 0xFFD080E2, 0xFFE580CC, 0xFF1981CC, 0xFF2F81E2,
    0xFF4281EF, 0xFF5281F7, 0xFF6081FB, 0xFF6B81FD, 0xFF7481FE, 0xFF7A81FF, 0xFF7E81FF, 0xFF8081FF,
    0xFF8181FF, 0xFF8581FF, 0xFF8B81FE, 0xFF9481FD, 0xFF9F81FB, 0xFFAD81F7, 0xFFBD81EF, 0xFFD081E2,
    0xFFE581CC, 0xFF1985CC, 0xFF2F85E2, 0xFF4285EF, 0xFF5285F7, 0xFF6085FB, 0xFF6B85FD, 0xFF7485FE,
    0xFF7A85FF, 0xFF7E85FF, 0xFF8085FF, 0xFF8185FF, 0xFF8585FF, 0xFF8B85FE, 0xFF9485FD, 0xFF9F85FB,
    0xFFAD85F7, 0xFFBD85EF, 0xFFD085E2, 0xFFE585CC, 0xFF2F8BE2, 0xFF428BEE, 0xFF528BF6, 0xFF608BFB,
    0xFF6B8BFD, 0xFF748BFE, 0xFF7A8BFE, 0xFF7E8BFE, 0xFF808BFE, 0xFF818BFE, 0xFF858BFE, 0xFF8B8BFE,
    0xFF948BFD, 0xFF9F8BFB, 0xFFAD8BF6, 0xFFBD8BEE, 0xFFD08BE2, 0xFF4294ED, 0xFF5294F5, 0xFF6094F9,
    0xFF6B94FC, 0xFF7494FD, 0xFF7A94FD, 0xFF7E94FD, 0xFF8094FD, 0xFF8194FD, 0xFF8594FD, 0xFF8B94FD,
    0xFF9494FC, 0xFF9F94F9, 0xFFAD94F5, 0xFFBD94ED, 0xFF529FF2, 0xFF609FF7, 0xFF6B9FF9, 0xFF749FFB,
    0xFF7A9FFB, 0xFF7E9FFB, 0xFF809FFB, 0xFF819FFB, 0xFF859FFB, 0xFF8B9FFB, 0xFF949FF9, 0xFF9F9FF7,
    0xFFAD9FF2, 0xFF60ADF2, 0xFF6BADF5, 0xFF74ADF6, 0xFF7AADF7, 0xFF7EADF7, 0xFF80ADF7, 0xFF81ADF7,
    0xFF85ADF7, 0xFF8BADF6, 0xFF94ADF5, 0xFF9FADF2, 0xFF6BBDED, 0xFF74BDEE, 0xFF7ABDEF, 0xFF7EBDEF,
    0xFF80BDEF, 0xFF81BDEF, 0xFF85BDEF, 0xFF8BBDEE, 0xFF94BDED, 0xFF74D0E2, 0xFF7AD0E2, 0xFF7ED0E2,
    0xFF80D0E2, 0xFF81D0E2, 0xFF85D0E2, 0xFF8BD0E2, 0xFF7AE5CC, 0xFF7EE5CC, 0xFF80E5CC, 0xFF81E5CC,
    0xFF85E5CC, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x008080FF,
};

// The palette entry nearest a normal (by red and green, as Bungie's tools and Invader pick it), red * 256 + green.
static const std::vector<uint8_t> &p8_nearest() {
    static std::vector<uint8_t> nearest;
    if (nearest.empty()) {
        nearest.resize(65536);
        for (int r = 0; r < 256; r++)
            for (int g = 0; g < 256; g++) {
                int best = 0, bd = INT32_MAX;
                for (int k = 0; k < 249; k++) {
                    int dr = (int)((kXboxBumpPalette[k] >> 16) & 255) - r, dg = (int)((kXboxBumpPalette[k] >> 8) & 255) - g;
                    if (dr * dr + dg * dg < bd) { bd = dr * dr + dg * dg; best = k; }
                }
                nearest[r * 256 + g] = (uint8_t)best;
            }
    }
    return nearest;
}

// One level's pixels as RGBA (A8 / Y8 / AY8: the one channel in R; A8Y8: R and G), in any of halve_pixels' formats,
// and P8 (kXboxBumpPalette). DXT levels smaller than a block are read from their one block.
static bool decode_level(const std::string &d, int w, int h, int fmt, std::vector<unsigned char> &rgba) {
    rgba.assign((size_t)w * h * 4, 0);
    if (fmt == 14 || fmt == 15 || fmt == 16 || fmt == 18) {
        int bs = fmt == 14 ? 8 : 16, bw = std::max(1, (w + 3) / 4), bh = std::max(1, (h + 3) / 4);
        if (d.size() < (size_t)bw * bh * bs) return false;
        unsigned char blk[64];
        for (int by = 0; by < bh; by++)
            for (int bx = 0; bx < bw; bx++) {
                const char *src = d.data() + ((size_t)by * bw + bx) * bs;
                if (fmt == 14) bcdec_bc1(src, blk, 16);
                else if (fmt == 15) bcdec_bc2(src, blk, 16);
                else if (fmt == 16) bcdec_bc3(src, blk, 16);
                else bcdec_bc7(src, blk, 16);   // MCC's BC7
                for (int r = 0; r < 4; r++)
                    for (int c = 0; c < 4; c++) {
                        int x = bx * 4 + c, y = by * 4 + r;
                        if (x < w && y < h) memcpy(&rgba[((size_t)y * w + x) * 4], blk + (r * 4 + c) * 4, 4);
                    }
            }
        return true;
    }
    if (fmt == 17) {   // P8, the Xbox's bump format: its palette's directions (and see-through entries)
        if (d.size() < (size_t)w * h) return false;
        for (size_t i = 0; i < (size_t)w * h; i++) {
            uint32_t c = kXboxBumpPalette[(unsigned char)d[i]];
            unsigned char *q = &rgba[i * 4];
            q[0] = (unsigned char)(c >> 16); q[1] = (unsigned char)(c >> 8); q[2] = (unsigned char)c; q[3] = (unsigned char)(c >> 24);
        }
        return true;
    }
    int bpp = fmt <= 2 ? 1 : (fmt == 3 || fmt == 6 || fmt == 8 || fmt == 9) ? 2 : (fmt == 10 || fmt == 11) ? 4 : 0;
    if (!bpp || d.size() < (size_t)w * h * bpp) return false;
    const unsigned char *p = (const unsigned char *)d.data();
    for (size_t i = 0; i < (size_t)w * h; i++) {
        unsigned char *q = &rgba[i * 4];
        unsigned v = bpp == 2 ? p[i * 2] | (p[i * 2 + 1] << 8) : 0;
        switch (fmt) {
        case 0: case 1: case 2: q[0] = p[i]; break;
        case 3: q[0] = p[i * 2]; q[1] = p[i * 2 + 1]; break;
        case 6: q[0] = (v >> 11) * 255 / 31; q[1] = ((v >> 5) & 63) * 255 / 63; q[2] = (v & 31) * 255 / 31; q[3] = 255; break;
        case 8: q[3] = (v >> 15) * 255; q[0] = ((v >> 10) & 31) * 255 / 31; q[1] = ((v >> 5) & 31) * 255 / 31;
                q[2] = (v & 31) * 255 / 31; break;
        case 9: q[3] = (v >> 12) * 17; q[0] = ((v >> 8) & 15) * 17; q[1] = ((v >> 4) & 15) * 17; q[2] = (v & 15) * 17; break;
        default: q[0] = p[i * 4 + 2]; q[1] = p[i * 4 + 1]; q[2] = p[i * 4]; q[3] = fmt == 10 ? 255 : p[i * 4 + 3]; break;   // B,G,R,A
        }
    }
    return true;
}

// A DXT1 block with see-through pixels (its 3-colour mode: the 4th index is transparent black). The colours come from
// stb's fit of the solid pixels (the see-through ones given their average), put in the order that selects the mode.
static void dxt1_cutout_block(const unsigned char *blk, unsigned char out[8]) {
    unsigned char solid[64];
    int sum[3] = {0, 0, 0}, n = 0;
    for (int k = 0; k < 16; k++)
        if (blk[k * 4 + 3] >= 128) { for (int c = 0; c < 3; c++) sum[c] += blk[k * 4 + c]; n++; }
    for (int k = 0; k < 16; k++) {
        bool on = blk[k * 4 + 3] >= 128;
        for (int c = 0; c < 3; c++) solid[k * 4 + c] = on ? blk[k * 4 + c] : (unsigned char)(n ? sum[c] / n : 0);
        solid[k * 4 + 3] = 255;
    }
    unsigned char enc[8];
    stb_compress_dxt_block(enc, solid, 0, STB_DXT_HIGHQUAL);
    unsigned c0 = enc[0] | enc[1] << 8, c1 = enc[2] | enc[3] << 8;
    if (c0 > c1) std::swap(c0, c1);   // c0 <= c1: 3 colours and transparent
    auto rgb = [](unsigned c, int *o) { o[0] = (c >> 11) * 255 / 31; o[1] = ((c >> 5) & 63) * 255 / 63; o[2] = (c & 31) * 255 / 31; };
    int pal[3][3];
    rgb(c0, pal[0]);
    rgb(c1, pal[1]);
    for (int c = 0; c < 3; c++) pal[2][c] = (pal[0][c] + pal[1][c]) / 2;
    uint32_t idx = 0;
    for (int k = 0; k < 16; k++) {
        unsigned sel = 3;
        if (blk[k * 4 + 3] >= 128) {
            int best = INT32_MAX;
            for (unsigned s = 0; s < 3; s++) {
                int d = 0;
                for (int c = 0; c < 3; c++) d += (blk[k * 4 + c] - pal[s][c]) * (blk[k * 4 + c] - pal[s][c]);
                if (d < best) { best = d; sel = s; }
            }
        }
        idx |= sel << (k * 2);
    }
    out[0] = (unsigned char)(c0 & 255); out[1] = (unsigned char)(c0 >> 8);
    out[2] = (unsigned char)(c1 & 255); out[3] = (unsigned char)(c1 >> 8);
    for (int k = 0; k < 4; k++) out[4 + k] = (unsigned char)(idx >> (k * 8));
}

// RGBA pixels back into a level of `fmt` (decode_level's channels). DXT1 keeps see-through pixels in its own
// 3-colour mode; DXT levels smaller than a block fill their one block.
static std::string encode_level(const std::vector<unsigned char> &rgba, int w, int h, int fmt) {
    std::string out;
    if (fmt == 14 || fmt == 15 || fmt == 16) {
        int bw = std::max(1, (w + 3) / 4), bh = std::max(1, (h + 3) / 4);
        for (int by = 0; by < bh; by++)
            for (int bx = 0; bx < bw; bx++) {
                unsigned char blk[64], enc[16];
                bool see_through = false;
                for (int r = 0; r < 4; r++)
                    for (int c = 0; c < 4; c++) {
                        int x = std::min(w - 1, bx * 4 + c), y = std::min(h - 1, by * 4 + r);
                        memcpy(blk + (r * 4 + c) * 4, &rgba[((size_t)y * w + x) * 4], 4);
                        see_through = see_through || blk[(r * 4 + c) * 4 + 3] < 128;
                    }
                if (fmt == 14) {
                    if (see_through) dxt1_cutout_block(blk, enc);
                    else stb_compress_dxt_block(enc, blk, 0, STB_DXT_HIGHQUAL);
                    out.append((const char *)enc, 8);
                } else if (fmt == 15) {   // DXT3: explicit 4-bit alpha, then the colour block
                    stb_compress_dxt_block(enc, blk, 1, STB_DXT_HIGHQUAL);
                    unsigned char a[8] = {};
                    for (int k = 0; k < 16; k++) a[k / 2] |= (unsigned char)(((blk[k * 4 + 3] * 15 + 127) / 255) << ((k & 1) * 4));
                    out.append((const char *)a, 8);
                    out.append((const char *)enc + 8, 8);
                } else {
                    stb_compress_dxt_block(enc, blk, 1, STB_DXT_HIGHQUAL);
                    out.append((const char *)enc, 16);
                }
            }
        return out;
    }
    for (size_t i = 0; i < (size_t)w * h; i++) {
        const unsigned char *q = &rgba[i * 4];
        unsigned v;
        switch (fmt) {
        case 0: case 1: case 2: out += (char)q[0]; break;
        case 17: out += (char)(q[3] < 128 ? 255 : p8_nearest()[q[0] * 256 + q[1]]); break;   // (255: see-through)
        case 3: out += (char)q[0]; out += (char)q[1]; break;
        case 6: v = ((q[0] * 31 + 127) / 255) << 11 | ((q[1] * 63 + 127) / 255) << 5 | ((q[2] * 31 + 127) / 255);
                out += (char)(v & 255); out += (char)(v >> 8); break;
        case 8: v = (q[3] >= 128 ? 1u : 0u) << 15 | ((q[0] * 31 + 127) / 255) << 10 | ((q[1] * 31 + 127) / 255) << 5 |
                    ((q[2] * 31 + 127) / 255);
                out += (char)(v & 255); out += (char)(v >> 8); break;
        case 9: v = ((q[3] + 8) / 17) << 12 | ((q[0] + 8) / 17) << 8 | ((q[1] + 8) / 17) << 4 | ((q[2] + 8) / 17);
                out += (char)(v & 255); out += (char)(v >> 8); break;
        default: { char px[4] = {(char)q[2], (char)q[1], (char)q[0], (char)(fmt == 10 ? 255 : q[3])}; out.append(px, 4); break; }
        }
    }
    return out;
}

// A 2x2 box filter (a side of 1 stays 1).
static void box_half(const std::vector<unsigned char> &in, int w, int h, std::vector<unsigned char> &out) {
    int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
    out.assign((size_t)nw * nh * 4, 0);
    for (int y = 0; y < nh; y++)
        for (int x = 0; x < nw; x++)
            for (int c = 0; c < 4; c++) {
                int sx = std::min(w - 1, x * 2), sy = std::min(h - 1, y * 2), sx1 = std::min(w - 1, sx + 1), sy1 = std::min(h - 1, sy + 1);
                int sum = in[((size_t)sy * w + sx) * 4 + c] + in[((size_t)sy * w + sx1) * 4 + c] + in[((size_t)sy1 * w + sx) * 4 + c] +
                          in[((size_t)sy1 * w + sx1) * 4 + c];
                out[((size_t)y * nw + x) * 4 + c] = (unsigned char)((sum + 2) / 4);
            }
}

static bool alpha_format(int fmt) { return fmt == 14 || fmt == 15 || fmt == 16 || fmt == 8 || fmt == 9 || fmt == 11 || fmt == 17; }
// The share of pixels at least half opaque, with alpha scaled by `s`.
static double alpha_coverage(const std::vector<unsigned char> &rgba, double s) {
    size_t n = rgba.size() / 4, on = 0;
    for (size_t i = 0; i < n; i++) on += rgba[i * 4 + 3] * s >= 127.5;
    return n ? (double)on / n : 0;
}
// Some of it see-through where alpha-tested (under half), and some solid.
static bool see_through(const std::vector<unsigned char> &rgba) {
    size_t n = rgba.size() / 4, clear = 0;
    for (size_t i = 0; i < n; i++) clear += rgba[i * 4 + 3] < 128;
    return n && clear >= n / 100 && clear <= n - n / 100;
}
// Scales alpha so `target` of the pixels are at least half opaque.
static void keep_coverage(std::vector<unsigned char> &rgba, double target) {
    if (alpha_coverage(rgba, 1.0) >= target) return;
    double lo = 1.0, hi = 16.0;
    for (int k = 0; k < 20; k++) {
        double mid = (lo + hi) / 2;
        (alpha_coverage(rgba, mid) < target ? lo : hi) = mid;
    }
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = (unsigned char)std::min(255.0, rgba[i] * hi + 0.5);
}

// One texture of a bitmap a shader alpha-tests (find_alpha_tested), halved (all its levels: a mip level fewer) if it
// has see-through parts and the usual way (its own next level, or a plain 2x2 average) would leave less than 90% as
// much of it solid: each level made from the one above and kept as solid as the full-size texture. False (data
// unchanged) otherwise.
static bool halve_cutout(std::string &data, int &w, int &h, int &fmt, int &mips) {
    size_t m0 = mip_bytes(w, h, fmt);
    if (!alpha_format(fmt) || !m0 || data.size() < m0 || fmt == 18) return false;   // (BC7 can't be written)
    std::vector<unsigned char> cur, usual;
    if (!decode_level(data.substr(0, m0), w, h, fmt, cur) || !see_through(cur)) return false;
    double target = alpha_coverage(cur, 1.0);
    int uw = std::max(1, w / 2), uh = std::max(1, h / 2);
    if (mips >= 1 && data.size() > m0) {
        if (!decode_level(data.substr(m0), uw, uh, fmt, usual)) return false;
    } else {
        box_half(cur, w, h, usual);
    }
    if (alpha_coverage(usual, 1.0) >= target * 0.9) return false;
    int nmips = std::max(0, mips - 1), cw = w, ch = h;
    std::string out;
    for (int k = 0; k <= nmips; k++) {
        std::vector<unsigned char> next;
        box_half(cur, cw, ch, next);
        cw = std::max(1, cw / 2);
        ch = std::max(1, ch / 2);
        keep_coverage(next, target);
        out += encode_level(next, cw, ch, fmt);
        cur.swap(next);
    }
    data = out;
    w = std::max(1, w / 2);
    h = std::max(1, h / 2);
    mips = nmips;
    g_cutout_textures++;
    return true;
}

// A texture's own next level, used as its new top when it's halved, unless it looks unlike the texture: a level a tool
// faded out (detail maps fade toward grey in their smaller levels by design), or one whose alpha thinned or is broken.
// Then the new top level is a 2x2 average of the full-size one, and the smaller levels stay as they were. True if so
// (data: the new top level and the rest; mips one fewer).
static int g_remade_tops = 0;   // (for the log)
static bool halve_unlike_mip(std::string &data, int w, int h, int fmt, int &mips) {
    size_t m0 = mip_bytes(w, h, fmt), m1 = mip_bytes(std::max(1, w / 2), std::max(1, h / 2), fmt);
    if (!m0 || mips < 1 || data.size() < m0 + m1 || fmt == 18 || fmt == 17) return false;   // (BC7 can't be written; P8 bumps keep theirs)
    std::vector<unsigned char> top, own, avg;
    if (!decode_level(data.substr(0, m0), w, h, fmt, top) || !decode_level(data.substr(m0, m1), std::max(1, w / 2), std::max(1, h / 2), fmt, own))
        return false;
    box_half(top, w, h, avg);
    double diff[4] = {0, 0, 0, 0};
    size_t n = avg.size() / 4;
    for (size_t i = 0; i < n; i++)
        for (int c = 0; c < 4; c++) diff[c] += std::abs((int)avg[i * 4 + c] - (int)own[i * 4 + c]);
    double worst = 0;
    for (int c = 0; c < 4; c++) worst = std::max(worst, n ? diff[c] / n : 0);
    if (worst <= 24) return false;   // (a different filter or compression differs by a few levels at most)
    data = encode_level(avg, std::max(1, w / 2), std::max(1, h / 2), fmt) + data.substr(m0 + m1);
    mips--;
    g_remade_tops++;
    return true;
}

// Halve a .bitmap tag's 2D textures by dropping their largest mip level (cut-out ones: halve_cutout; a next level
// unlike the texture: halve_unlike_mip).
// Layout: 64-byte header, Bitmap (108), color plate data, pixel data, sequences (64 each),
// their sprites (32 each), bitmap data entries (48 each). Returns bytes saved (0 = unchanged).
size_t shrink_bitmap_tag(const fs::path &p, bool write, bool alpha_tested = false) {
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
                int nw = w, nh = h, nf = fmt, nm = mips;
                std::string cd = data;
                if (write && alpha_tested && halve_cutout(cd, nw, nh, nf, nm)) {   // (counting: about as much as a level dropped)
                    saved += data.size() > cd.size() ? data.size() - cd.size() : 0;
                    data = cd;
                } else if (write && halve_unlike_mip(cd = data, w, h, fmt, nm = mips)) {
                    saved += data.size() > cd.size() ? data.size() - cd.size() : 0;
                    data = cd;
                } else {
                    data.erase(0, m0);
                    saved += m0;
                }
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
size_t shrink_textures_to_fit(Job &job, size_t needed, int &count, bool hard);

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
// GPU reads it). DXT1 keeps its see-through pixels in its own 3-colour mode. False = unsupported.
bool halve_pixels(const std::string &d, int w, int h, int &fmt, std::string &out) {
    if ((fmt == 14 || fmt == 15 || fmt == 16 || fmt == 18) && !(w % 4 || h % 4 || std::max(1, w / 2) % 4 || std::max(1, h / 2) % 4)) {
        std::vector<unsigned char> rgba, half;
        if (!decode_level(d, w, h, fmt, rgba)) return false;
        box_half(rgba, w, h, half);
        if (fmt == 18) {   // BC7 (MCC) can't be written: DXT5, or DXT1 when nothing is see-through
            fmt = 14;
            for (size_t i = 3; i < half.size() && fmt == 14; i += 4)
                if (half[i] != 255) fmt = 16;
        }
        out = encode_level(half, std::max(1, w / 2), std::max(1, h / 2), fmt);
        return true;
    }
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

// A model shader's multipurpose map packs four masks in its channels, in an order that differs between the games
// (Bungie's Xbox tags against Gearbox's PC copies of them, channel by channel; rasterizer_xbox_models.c reads red
// for the reflection mask, green for self-illumination, blue for the colour change and alpha for the auxiliary mask):
//   Xbox:            R reflection   G self-illumination   B colour change   A auxiliary
//   PC (CE, MCC):    R auxiliary    G self-illumination   B reflection      A colour change
// (MCC's "use xbox multipurpose channel order" flag has it read the Xbox's order.) Rewrites a multipurpose bitmap
// from one order to the other, every mip level: `to_xbox` from PC's order to the Xbox's, else back. `alpha_matters`:
// a shader reads the channel that lands in alpha (the auxiliary mask on the Xbox; the colour change on PC); else a
// format with no alpha keeps its format and the alpha is left opaque. A format that can't hold what lands in its
// channels becomes one that can (DXT1 with an alpha that matters: DXT5; 8-bit grey: A8R8G8B8). Returns how many of
// its textures were rewritten (0: none it could), -1 if the tag can't be read.
int reorder_multipurpose_bitmap_tag(const fs::path &p, bool to_xbox, bool alpha_matters) {
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
    int done = 0, tag_fmt = -1;
    for (uint32_t i = 0; i < nbd; i++) {
        size_t e = bd_off + 48 * i;
        // Each entry's data runs from its offset to the next entry's (extracted tags store size 0).
        uint32_t off = offs[i], end = pp;
        for (auto x : offs)
            if (x > off && x < end) end = x;
        if (off > pix.size() || end > pix.size()) return -1;
        std::string data = pix.substr(off, end - off);
        int w = be16(b, e + 4), h = be16(b, e + 6), type = be16(b, e + 10), fmt = be16(b, e + 12), flags = be16(b, e + 14);
        int levels = be16(b, e + 20) + 1;
        bool ok = type == 0 && !(flags & 8) && w > 0 && h > 0 && fmt != 17 && fmt != 18 && mip_bytes(1, 1, fmt) > 0;
        // Each level as RGBA, channels moved.
        std::vector<std::vector<unsigned char>> lv;
        size_t at = 0;
        bool opaque = true;   // what lands in alpha is opaque everywhere
        for (int k = 0; ok && k < levels; k++) {
            int lw = std::max(1, w >> k), lh = std::max(1, h >> k);
            size_t n = mip_bytes(lw, lh, fmt);
            if (at + n > data.size()) { ok = false; break; }
            std::string d = data.substr(at, n);
            at += n;
            std::vector<unsigned char> rgba;
            if (fmt <= 3) {   // A8 white with alpha, Y8 grey, AY8 grey and alpha alike, A8Y8 grey with alpha
                rgba.assign((size_t)lw * lh * 4, 255);
                for (size_t q = 0; q < (size_t)lw * lh; q++) {
                    unsigned char *c = &rgba[q * 4];
                    unsigned char y = (unsigned char)d[fmt == 3 ? q * 2 : q];
                    if (fmt == 0) c[3] = y;
                    else c[0] = c[1] = c[2] = y;
                    if (fmt == 2) c[3] = y;
                    if (fmt == 3) c[3] = (unsigned char)d[q * 2 + 1];
                }
            } else if (!decode_level(d, lw, lh, fmt, rgba)) {
                ok = false;
                break;
            }
            for (size_t q = 0; q < (size_t)lw * lh; q++) {
                unsigned char *c = &rgba[q * 4], s[4] = {c[0], c[1], c[2], c[3]};
                if (to_xbox) { c[0] = s[2]; c[1] = s[1]; c[2] = s[3]; c[3] = s[0]; }   // R = PC's B, B = PC's A, A = PC's R
                else { c[0] = s[3]; c[1] = s[1]; c[2] = s[0]; c[3] = s[2]; }           // R = Xbox's A, B = Xbox's R, A = Xbox's B
                opaque = opaque && c[3] == 255;
            }
            lv.push_back(std::move(rgba));
        }
        if (ok) {
            bool keep_alpha = alpha_matters && !opaque;
            int nf = fmt;
            if (fmt <= 3) nf = 11;
            else if (fmt == 14 && keep_alpha) nf = 16;
            else if ((fmt == 6 || fmt == 8 || fmt == 10) && keep_alpha) nf = 11;
            std::string enc;
            for (int k = 0; k < (int)lv.size(); k++) {
                int lw = std::max(1, w >> k), lh = std::max(1, h >> k);
                if (!keep_alpha && (nf == 14 || nf == 6 || nf == 10 || nf == 8))
                    for (size_t q = 0; q < (size_t)lw * lh; q++) lv[k][q * 4 + 3] = 255;
                enc += encode_level(lv[k], lw, lh, nf);
            }
            enc += data.substr(std::min(at, data.size()));   // (anything after the levels, as it was)
            data = enc;
            if (nf != fmt) {
                put_be16(b, e + 12, (uint16_t)nf);
                tag_fmt = nf;
            }
            done++;
        }
        put_be32(b, e + 24, (uint32_t)out_pix.size());
        put_be32(b, e + 28, (uint32_t)data.size());
        out_pix += data;
    }
    if (!done) return 0;
    if (tag_fmt >= 0) put_be16(b, S + 2, tag_fmt == 16 ? 2 : 4);   // tag-level format: interpolated alpha / 32-bit colour
    std::string out = b.substr(0, pp_off) + out_pix + b.substr(pp_off + pp);
    put_be32(out, S + 48, (uint32_t)out_pix.size());
    put_be32(out, 40, ~crc32_of(out.data() + 64, out.size() - 64)); // HEK tag checksum
    std::ofstream(p, std::ios::binary) << out;
    return done;
}

// A level's bump map made smaller in memory without losing pixels: in the Xbox's own bump format (P8,
// kXboxBumpPalette: each pixel the palette normal nearest its own, by red and green as Bungie's tools and Invader pick
// it), or as DXT1, whichever keeps its directions closer (the root mean square of the angle each pixel turns, over its
// full-size level). The palette reaches 53 degrees from flat, so steep bumps (the edges of curbs and grooves in
// Halo 2-style normal maps) keep their slopes better as DXT1; gentle ones are closer as P8. Either is a quarter of a
// 32-bit map's memory or less (P8 a quarter, DXT1 an eighth; DXT1 half a DXT5's), and turns each pixel a few degrees,
// where halving it would turn them several times as far. With `alpha_tested` (a shader alpha-tests through it: the
// Xbox's lightmap pass tests the bump map's alpha) a pixel the test drops (alpha under half) stays see-through.
// `fmt` gets what it became (17 P8, 14 DXT1). Returns how many of its textures were rewritten (0: none could gain),
// -1 if the tag can't be read.
int compact_bump_bitmap_tag(const fs::path &p, bool alpha_tested, int *fmt_out = nullptr) {
    const std::vector<uint8_t> &nearest = p8_nearest();   // red * 256 + green -> palette entry
    auto p8_of = [&](const unsigned char *c) -> uint8_t { return alpha_tested && c[3] < 128 ? 255 : nearest[c[0] * 256 + c[1]]; };
    // The angle (degrees, squared) between two encoded normals.
    auto turn2 = [](const unsigned char *a, const unsigned char *b) {
        double x[3], y[3], la = 0, lb = 0, d = 0;
        for (int k = 0; k < 3; k++) {
            x[k] = a[k] / 127.5 - 1; y[k] = b[k] / 127.5 - 1;
            la += x[k] * x[k]; lb += y[k] * y[k]; d += x[k] * y[k];
        }
        double c = d / (std::sqrt(la * lb) + 1e-12), t = std::acos(std::max(-1.0, std::min(1.0, c))) * 57.29578;
        return t * t;
    };
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
    int done = 0, chosen = 0;
    for (uint32_t i = 0; i < nbd; i++) {
        size_t e = bd_off + 48 * i;
        uint32_t off = offs[i], end = pp;
        for (auto x : offs)
            if (x > off && x < end) end = x;
        if (off > pix.size() || end > pix.size()) return -1;
        std::string data = pix.substr(off, end - off);
        int w = be16(b, e + 4), h = be16(b, e + 6), type = be16(b, e + 10), fmt = be16(b, e + 12), flags = be16(b, e + 14);
        int levels = be16(b, e + 20) + 1;
        // (normal maps come as 32-bit, 16-bit or block formats; P8 and DXT1 have nothing to gain, grey ones aren't normals)
        bool ok = type == 0 && !(flags & 8) && w > 0 && h > 0 && fmt != 17 && fmt != 14 && fmt > 3 && mip_bytes(1, 1, fmt) > 0;
        std::vector<std::vector<unsigned char>> lv;
        size_t at = 0;
        for (int k = 0; ok && k < levels; k++) {
            int lw = std::max(1, w >> k), lh = std::max(1, h >> k);
            size_t n = mip_bytes(lw, lh, fmt);
            std::vector<unsigned char> rgba;
            if (at + n > data.size() || !decode_level(data.substr(at, n), lw, lh, fmt, rgba)) { ok = false; break; }
            at += n;
            if (!alpha_tested)
                for (size_t q = 0; q < (size_t)lw * lh; q++) rgba[q * 4 + 3] = 255;
            lv.push_back(std::move(rgba));
        }
        if (!ok) {
            put_be32(b, e + 24, (uint32_t)out_pix.size());
            put_be32(b, e + 28, (uint32_t)data.size());
            out_pix += data;
            continue;
        }
        // P8 only gains on a 16- or 32-bit map; DXT1 needs sides that are multiples of 4.
        bool can_p8 = mip_bytes(4, 4, fmt) > 16, can_dxt1 = w % 4 == 0 && h % 4 == 0;
        double e_p8 = 1e30, e_dxt1 = 1e30;
        std::string top_dxt1;
        const auto &top = lv[0];
        size_t px = (size_t)w * h;
        if (can_p8) {
            double sum = 0;
            for (size_t q = 0; q < px; q++) {
                if (alpha_tested && top[q * 4 + 3] < 128) continue;
                uint32_t c = kXboxBumpPalette[nearest[top[q * 4] * 256 + top[q * 4 + 1]]];
                unsigned char n[3] = {(unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c};
                sum += turn2(&top[q * 4], n);
            }
            e_p8 = sum / px;
        }
        if (can_dxt1) {
            top_dxt1 = encode_level(top, w, h, 14);
            std::vector<unsigned char> back;
            if (decode_level(top_dxt1, w, h, 14, back)) {
                double sum = 0;
                for (size_t q = 0; q < px; q++)
                    if (!(alpha_tested && top[q * 4 + 3] < 128)) sum += turn2(&top[q * 4], &back[q * 4]);
                e_dxt1 = sum / px;
            }
        }
        if (!can_p8 && !can_dxt1) {
            put_be32(b, e + 24, (uint32_t)out_pix.size());
            put_be32(b, e + 28, (uint32_t)data.size());
            out_pix += data;
            continue;
        }
        int nf = e_p8 <= e_dxt1 ? 17 : 14;
        std::string enc;
        for (int k = 0; k < (int)lv.size(); k++) {
            int lw = std::max(1, w >> k), lh = std::max(1, h >> k);
            if (nf == 17)
                for (size_t q = 0; q < (size_t)lw * lh; q++) enc += (char)p8_of(&lv[k][q * 4]);
            else
                enc += k == 0 ? top_dxt1 : encode_level(lv[k], lw, lh, 14);
        }
        data = enc;
        put_be16(b, e + 12, (uint16_t)nf);
        put_be16(b, e + 14, (uint16_t)(nf == 17 ? ((flags & ~2) | 4) : ((flags & ~4) | 2)));   // palettized / compressed
        chosen = nf;
        done++;
        put_be32(b, e + 24, (uint32_t)out_pix.size());
        put_be32(b, e + 28, (uint32_t)data.size());
        out_pix += data;
    }
    if (!done) return 0;
    put_be16(b, S + 6, (uint16_t)(be16(b, S + 6) & ~2));   // (its "disable height map compression" off, as Bungie's)
    put_be16(b, S + 2, chosen == 17 ? 4 : 0);              // (its format: 32-bit, as Bungie's P8 ones say; or DXT1)
    std::string out = b.substr(0, pp_off) + out_pix + b.substr(pp_off + pp);
    put_be32(out, S + 48, (uint32_t)out_pix.size());
    put_be32(out, 40, ~crc32_of(out.data() + 64, out.size() - 64)); // HEK tag checksum
    std::ofstream(p, std::ios::binary) << out;
    if (fmt_out) *fmt_out = chosen;
    return done;
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
int cap_bitmap_tag(const fs::path &p, int max_side, bool alpha_tested = false) {
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
        // not swizzled (palettized: mips only), nor linear (an interface bitmap, drawn in pixels)
        while (type == 0 && !(flags & 8) && !(flags & 16) && std::max(w, h) > max_side) {
            size_t m0 = mip_bytes(w, h, fmt);
            if (!m0 || (block_format(fmt) && !block_halvable(w, h))) break;   // e.g. a 1024x4 DXT strip stays as is
            std::string nd = data;
            int cw = w, chh = h, cf = fmt, cm = mips;
            if (alpha_tested && halve_cutout(nd, cw, chh, cf, cm)) {
                mips = cm;
            } else if (halve_unlike_mip(nd = data, w, h, fmt, cm = mips)) {
                mips = cm;
            } else if (mips >= 1 && data.size() > m0) {
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

// A HUD bitmap made smaller: every texture in the tag halved the same number of times, up to `times` (for
// HUD bitmaps MCC or Halo PC draws at half size, "half hud scale", so the Xbox draws them at the size MCC
// does; and fit_hud_textures). Sprite rectangles are fractions of the sheet, so they stay right. Returns how
// many halvings were done for all of the tag's textures (0 = none: unreadable, or a texture that can't be
// halved).
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
        if (be16(nb, S + 2) == 6) {   // tag-level format BC7: DXT5/DXT1 once no texture is BC7 any more
            bool bc7 = false, alpha = false;
            for (uint32_t i = 0; i < nbd; i++) {
                int f = be16(nb, bd_off + 48 * i + 12);
                bc7 = bc7 || f == 18;
                alpha = alpha || f == 15 || f == 16;
            }
            if (!bc7) put_be16(nb, S + 2, alpha ? 2 : 0);
        }
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

// A HUD bitmap keeping only what the HUD can show of it. `drop_mips`: its smaller copies go (an element that always
// draws it at a texel to a pixel or bigger never reads them). `max_w`/`max_h` > 0: each texture is cut to at most that
// many texels across / down, keeping its middle (a scope's overlay, drawn centred at full size, bigger than the
// screen: its edges never reach it); only where every picture is a whole texture (no sprites, or each sprite the
// whole page), and only with no smaller copies left. Sides stay powers of two and whole 4x4 blocks. Returns the
// bytes of pixel data saved (0: nothing changed).
static size_t trim_hud_bitmap_tag(const fs::path &p, bool drop_mips, int max_w, int max_h) {
    BitmapParts bp;
    if (!bp.read(p) || !bp.nbd) return 0;
    std::string &b = bp.b;
    auto rf = [&](size_t o) { uint32_t u = be32(b, o); float f; memcpy(&f, &u, 4); return f; };
    bool can_crop = max_w > 0 || max_h > 0;
    for (uint32_t s = 0; s < bp.nseq && can_crop; s++) {
        size_t sp = bp.sprite_at(s);
        for (uint32_t k = 0; k < bp.sprites(s) && can_crop; k++, sp += 32)
            if (rf(sp + 8) > 0.001f || rf(sp + 12) < 0.999f || rf(sp + 16) > 0.001f || rf(sp + 20) < 0.999f) can_crop = false;
    }
    std::string pix = b.substr(bp.pp_off, bp.pp), out_pix;
    std::vector<uint32_t> offs;
    for (uint32_t i = 0; i < bp.nbd; i++) offs.push_back(be32(b, bp.bd_off + 48 * i + 24));
    bool changed = false;
    for (uint32_t i = 0; i < bp.nbd; i++) {
        size_t e = bp.bd_off + 48 * i;
        uint32_t off = offs[i], end = bp.pp;
        for (auto x : offs)
            if (x > off && x < end) end = x;
        if (off > pix.size() || end > pix.size()) return 0;
        std::string data = pix.substr(off, end - off);
        int w = be16(b, e + 4), h = be16(b, e + 6), type = be16(b, e + 10), fmt = be16(b, e + 12), flags = be16(b, e + 14);
        int mips = be16(b, e + 20);
        const size_t m0 = mip_bytes(w, h, fmt);
        const bool plain = (type == 0 || type == 3 || type == 4) && !(flags & 8) && m0 && data.size() >= m0;   // (not swizzled)
        if (plain && drop_mips && mips > 0) {
            data.resize(m0);
            mips = 0;
            changed = true;
        }
        int nw = max_w > 0 ? std::min(w, max_w) : w, nh = max_h > 0 ? std::min(h, max_h) : h;
        if (plain && type != 4 && can_crop && mips == 0 && (nw < w || nh < h)) {   // (an interface bitmap's in pixels)
            const bool blocks = block_format(fmt);
            const int cell = blocks ? 4 : 1;
            const size_t unit = blocks ? mip_bytes(4, 4, fmt) : mip_bytes(1, 1, fmt);
            int x0 = (w - nw) / 2, y0 = (h - nh) / 2;
            if (unit && nw % cell == 0 && nh % cell == 0 && x0 % cell == 0 && y0 % cell == 0) {
                const int cols = w / cell, ncols = nw / cell, nrows = nh / cell;
                std::string nd;
                nd.reserve((size_t)ncols * nrows * unit);
                for (int r = 0; r < nrows; r++)
                    nd.append(data, ((size_t)(y0 / cell + r) * cols + x0 / cell) * unit, (size_t)ncols * unit);
                data = nd;
                put_be16(b, e + 16, (uint16_t)((int16_t)be16(b, e + 16) - x0));   // registration point, in pixels
                put_be16(b, e + 18, (uint16_t)((int16_t)be16(b, e + 18) - y0));
                w = nw, h = nh;
                changed = true;
            }
        }
        put_be16(b, e + 4, (uint16_t)w);
        put_be16(b, e + 6, (uint16_t)h);
        put_be16(b, e + 20, (uint16_t)mips);
        put_be32(b, e + 24, (uint32_t)out_pix.size());
        put_be32(b, e + 28, (uint32_t)data.size());
        out_pix += data;
    }
    if (!changed || out_pix.size() >= bp.pp) return 0;
    std::string out = b.substr(0, bp.pp_off) + out_pix + b.substr(bp.pp_off + bp.pp);
    put_be32(out, 64 + 48, (uint32_t)out_pix.size());
    put_be32(out, 40, ~crc32_of(out.data() + 64, out.size() - 64));
    std::ofstream(p, std::ios::binary) << out;
    return bp.pp - out_pix.size();
}

// One of a sheet's pictures (a sequence's first sprite, or its first texture whole), in a form another sheet's can be
// compared with: how solid each pixel is, fitted into a 24x24 square (centred, its shape kept), less its mean and
// scaled to length 1. `aspect`: its width over its height. False if it can't be read (or is blank).
static bool icon_picture(const BitmapParts &b, uint32_t seq, std::vector<float> &v, double &aspect) {
    if (seq >= b.nseq) return false;
    auto rf = [&](size_t o) { uint32_t u = be32(b.b, o); float f; memcpy(&f, &u, 4); return f; };
    int page;
    double l = 0, r = 1, t = 0, bo = 1;
    if (b.sprites(seq)) {
        size_t sp = b.sprite_at(seq);
        page = (int16_t)be16(b.b, sp);
        l = rf(sp + 8), r = rf(sp + 12), t = rf(sp + 16), bo = rf(sp + 20);
    } else {
        page = (int16_t)be16(b.b, b.seq_off + 64 * (size_t)seq + 32);
    }
    if (page < 0 || page >= (int)b.nbd) return false;
    size_t e = b.bd_off + 48 * (size_t)page;
    int w = be16(b.b, e + 4), h = be16(b.b, e + 6), fmt = be16(b.b, e + 12);
    uint32_t off = be32(b.b, e + 24);
    size_t n = mip_bytes(w, h, fmt);
    if (!n || be16(b.b, e + 10) != 0 || (be16(b.b, e + 14) & 8) || (size_t)off + n > b.pp) return false;
    std::vector<unsigned char> rgba;
    if (!decode_level(b.b.substr(b.pp_off + off, n), w, h, fmt, rgba)) return false;
    int x0 = std::clamp((int)std::floor(l * w), 0, w), x1 = std::clamp((int)std::ceil(r * w), 0, w);
    int y0 = std::clamp((int)std::floor(t * h), 0, h), y1 = std::clamp((int)std::ceil(bo * h), 0, h);
    if (x1 - x0 < 2 || y1 - y0 < 2) return false;
    const int N = 24;
    aspect = double(x1 - x0) / (y1 - y0);
    double s = double(N) / std::max(x1 - x0, y1 - y0);
    double ox = (N - (x1 - x0) * s) / 2, oy = (N - (y1 - y0) * s) / 2;
    std::vector<double> sum(N * N, 0), cnt(N * N, 0);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            const unsigned char *q = &rgba[((size_t)y * w + x) * 4];
            double c;
            if (fmt <= 2) c = q[0];                                  // A8, Y8, AY8: the one channel
            else if (fmt == 3) c = q[0] * q[1] / 255.0;              // A8Y8
            else c = q[3] * std::max({q[0], q[1], q[2]}) / 255.0;    // colour: alpha times brightness
            int cx = std::clamp((int)(ox + (x - x0 + 0.5) * s), 0, N - 1), cy = std::clamp((int)(oy + (y - y0 + 0.5) * s), 0, N - 1);
            sum[cy * N + cx] += c;
            cnt[cy * N + cx] += 1;
        }
    v.assign(N * N, 0);
    double mean = 0;
    for (int k = 0; k < N * N; k++) mean += v[k] = cnt[k] ? float(sum[k] / cnt[k]) : 0.f;
    mean /= N * N;
    double len = 0;
    for (auto &f : v) len += (f - mean) * (f - mean);
    if (len < 1e-6) return false;
    len = std::sqrt(len);
    for (auto &f : v) f = float((f - mean) / len);
    return true;
}

// Whether a picture on one sheet is the same icon as one on another (at another size or in another format): about
// the same shape, and their pixels alike (correlated). False if either can't be read.
static bool same_icon(const BitmapParts &a, uint32_t sa, const BitmapParts &b, uint32_t sb) {
    std::vector<float> va, vb;
    double ra, rb;
    if (!icon_picture(a, sa, va, ra) || !icon_picture(b, sb, vb, rb)) return false;
    if (std::max(ra, rb) / std::min(ra, rb) > 1.3) return false;
    double c = 0;
    for (size_t k = 0; k < va.size(); k++) c += va[k] * vb[k];
    return c > 0.85;
}
// For tests: for each of a sheet's sequences, whether the other sheet's at that number is the same icon ('=') or not ('x').
std::string icon_sheet_matches(const fs::path &a, const fs::path &b) {
    BitmapParts x, y;
    if (!x.read(a) || !y.read(b)) return "unreadable";
    std::string r;
    for (uint32_t i = 0; i < std::min(x.nseq, y.nseq); i++) {
        std::vector<float> va, vb;
        double ra = 0, rb = 0, c = 0;
        bool ok = icon_picture(x, i, va, ra) && icon_picture(y, i, vb, rb);
        for (size_t k = 0; ok && k < va.size(); k++) c += va[k] * vb[k];
        char line[96];
        snprintf(line, sizeof line, "%u %c aspect %.2f/%.2f correlation %.2f\n", i, same_icon(x, i, y, i) ? '=' : 'x', ra, rb, c);
        r += line;
    }
    return r;
}

// A HUD message icon sheet for the Xbox: the Xbox's own sheet, unchanged (its controller-button glyphs and the
// stock weapons' icons keep their sequence numbers), followed by sequences of the map's: the one after the Xbox's
// last is the map's sequence src[0], the next src[1], and so on (-1: left empty), whose textures are appended.
// Writes `out`. False if either tag can't be read.
// `keep` (numbers in the new sheet): only those of the added sequences (the others are left empty, keeping every
// number; none after the last kept), with only their pictures: cut out of the map's textures (to whole 4x4 blocks,
// so compressed ones are copied as they are) and packed onto new, smaller ones, one per pixel format.
bool merge_icon_sheets_from(const fs::path &xbox_sheet, const fs::path &map_sheet, std::vector<int> src,
                            const fs::path &out, const std::set<uint32_t> *keep = nullptr) {
    BitmapParts x, m;
    if (!x.read(xbox_sheet) || !m.read(map_sheet)) return false;
    for (int &s : src)
        if (s >= (int)m.nseq) s = -1;
    auto kept = [&](uint32_t j) { return src[j] >= 0 && (!keep || keep->count(x.nseq + j) > 0); };
    while (!src.empty() && !kept((uint32_t)src.size() - 1)) src.pop_back();   // (none empty at the end)
    const uint32_t end = (uint32_t)src.size();
    std::string seqs = x.b.substr(x.seq_off, 64 * (size_t)x.nseq);
    std::string sprs = x.b.substr(x.spr_off, x.bd_off - x.spr_off);
    std::string bds = x.b.substr(x.bd_off, 48 * (size_t)x.nbd);
    std::string mpix;
    uint32_t added_bd = 0;
    std::vector<uint32_t> offs;
    for (uint32_t i = 0; i < m.nbd; i++) offs.push_back(be32(m.b, m.bd_off + 48 * (size_t)i + 24));
    auto texture = [&](uint32_t bi) {   // one of the map's textures' pixel data (each runs to the next one's)
        uint32_t off = offs[bi], stop = m.pp;
        for (auto o2 : offs)
            if (o2 > off && o2 < stop) stop = o2;
        return off > m.pp ? std::string() : m.b.substr(m.pp_off + off, stop - off);
    };
    auto rf = [](const std::string &b, size_t o) { uint32_t u = be32(b, o); float v; memcpy(&v, &u, 4); return v; };
    auto wf = [](std::string &b, size_t o, float v) { uint32_t u; memcpy(&u, &v, 4); put_be32(b, o, u); };

    // The kept sprites cut out and packed: where each one goes (`placed`), and the new textures (`bds`, `mpix`).
    struct Piece { uint32_t seq, k; int page, x0, y0, x1, y1, nx = 0, ny = 0, out = -1; };
    std::vector<Piece> pieces;
    bool cut = keep != nullptr;
    if (cut) {
        for (uint32_t j = 0; j < end && cut; j++) {
            if (!kept(j)) continue;
            const uint32_t i = (uint32_t)src[j];
            size_t so = m.sprite_at(i);
            for (uint32_t k = 0; k < m.sprites(i); k++) {
                size_t sp = so + 32 * (size_t)k;
                int bi = (int16_t)be16(m.b, sp);
                if (bi < 0 || bi >= (int)m.nbd) continue;
                size_t e = m.bd_off + 48 * (size_t)bi;
                int w = be16(m.b, e + 4), h = be16(m.b, e + 6), fmt = be16(m.b, e + 12), a = block_format(fmt) ? 4 : 1;
                if (!mip_bytes(w, h, fmt) || be16(m.b, e + 10) != 0) { cut = false; break; }   // (2D textures, known formats)
                Piece p{j, k, bi, 0, 0, 0, 0};
                p.x0 = std::max(0, (int)std::floor(rf(m.b, sp + 8) * w) / a * a);
                p.x1 = std::min(w, ((int)std::ceil(rf(m.b, sp + 12) * w) + a - 1) / a * a);
                p.y0 = std::max(0, (int)std::floor(rf(m.b, sp + 16) * h) / a * a);
                p.y1 = std::min(h, ((int)std::ceil(rf(m.b, sp + 20) * h) + a - 1) / a * a);
                if (p.x1 > p.x0 && p.y1 > p.y0) pieces.push_back(p);
            }
        }
    }
    if (cut) {
        // one new texture per pixel format, its pieces packed in rows (tallest first) on the smallest power-of-two square
        std::map<int, std::vector<size_t>> by_format;
        for (size_t n = 0; n < pieces.size(); n++) by_format[be16(m.b, m.bd_off + 48 * (size_t)pieces[n].page + 12)].push_back(n);
        for (auto &[fmt, list] : by_format) {
            std::sort(list.begin(), list.end(), [&](size_t a, size_t b) {
                return pieces[a].y1 - pieces[a].y0 > pieces[b].y1 - pieces[b].y0;
            });
            int maxw = 4;
            for (size_t n : list) maxw = std::max(maxw, pieces[n].x1 - pieces[n].x0);
            int best_w = 0, best_h = 0;
            for (int W = 4; W <= 16384; W *= 2) {
                if (W < maxw) continue;
                int px = 0, py = 0, row = 0;
                for (size_t n : list) {
                    int pw = pieces[n].x1 - pieces[n].x0, ph = pieces[n].y1 - pieces[n].y0;
                    if (px + pw > W) { py += row; px = 0; row = 0; }
                    px += pw;
                    row = std::max(row, ph);
                }
                int H = 4;
                while (H < py + row) H *= 2;
                if (!best_w || (long long)W * H < (long long)best_w * best_h) best_w = W, best_h = H;
            }
            int W = best_w, H = best_h, px = 0, py = 0, row = 0;
            for (size_t n : list) {
                Piece &p = pieces[n];
                int pw = p.x1 - p.x0, ph = p.y1 - p.y0;
                if (px + pw > W) { py += row; px = 0; row = 0; }
                p.nx = px;
                p.ny = py;
                p.out = (int)(x.nbd + added_bd);
                px += pw;
                row = std::max(row, ph);
            }
            // the pixels: whole 4x4 blocks of a compressed texture, rows of pixels otherwise
            bool blocks = block_format(fmt);
            int a = blocks ? 4 : 1;
            size_t unit = blocks ? mip_bytes(4, 4, fmt) : mip_bytes(1, 1, fmt);
            std::string pix(mip_bytes(W, H, fmt), '\0');
            for (size_t n : list) {
                const Piece &p = pieces[n];
                size_t e = m.bd_off + 48 * (size_t)p.page;
                int sw = be16(m.b, e + 4);
                std::string src = texture((uint32_t)p.page);
                size_t src_row = (size_t)std::max(1, sw / a) * unit, dst_row = (size_t)(W / a) * unit;
                for (int y = p.y0; y < p.y1; y += a) {
                    size_t so = (size_t)(y / a) * src_row + (size_t)(p.x0 / a) * unit;
                    size_t d = (size_t)((p.ny + y - p.y0) / a) * dst_row + (size_t)(p.nx / a) * unit;
                    size_t len = (size_t)((p.x1 - p.x0) / a) * unit;
                    if (so + len <= src.size() && d + len <= pix.size()) memcpy(&pix[d], src.data() + so, len);
                }
            }
            std::string e = m.b.substr(m.bd_off + 48 * (size_t)pieces[list[0]].page, 48);
            put_be16(e, 4, (uint16_t)W);
            put_be16(e, 6, (uint16_t)H);
            put_be16(e, 20, 0);   // (no smaller mip levels: the HUD draws icons a texel to a pixel)
            put_be32(e, 24, x.pp + (uint32_t)mpix.size());
            put_be32(e, 28, (uint32_t)pix.size());
            mpix += pix;
            bds += e;
            added_bd++;
        }
    } else {
        pieces.clear();
        // Only the map's textures that its added sequences use, renumbered after the Xbox's.
        std::map<int, int> remap;
        auto use = [&](int bi) {
            if (bi >= 0 && bi < (int)m.nbd && !remap.count(bi)) remap[bi] = 0;
        };
        for (uint32_t j = 0; j < end; j++) {
            if (!kept(j)) continue;
            const uint32_t i = (uint32_t)src[j];
            int16_t first = (int16_t)be16(m.b, m.seq_off + 64 * (size_t)i + 32), count = (int16_t)be16(m.b, m.seq_off + 64 * (size_t)i + 34);
            for (int k = 0; first >= 0 && k < count; k++) use(first + k);
            size_t so = m.sprite_at(i);
            for (uint32_t k = 0; k < m.sprites(i); k++) use((int16_t)be16(m.b, so + 32 * (size_t)k));
        }
        int next = (int)x.nbd;
        for (auto &[bi, ni] : remap) ni = next++;
        for (auto &[bi, ni] : remap) {
            std::string e = m.b.substr(m.bd_off + 48 * (size_t)bi, 48);
            std::string px = texture((uint32_t)bi);
            if (offs[bi] > m.pp) return false;
            put_be32(e, 24, x.pp + (uint32_t)mpix.size());
            put_be32(e, 28, (uint32_t)px.size());
            mpix += px;
            bds += e;
        }
        added_bd = (uint32_t)remap.size();
        for (auto &[bi, ni] : remap)   // (as pieces: whole textures, renumbered)
            pieces.push_back(Piece{UINT32_MAX, 0, bi, 0, 0, 0, 0, 0, 0, ni});
    }
    auto new_page = [&](int bi) {
        for (auto &p : pieces)
            if (p.seq == UINT32_MAX && p.page == bi) return p.out;
        return bi;
    };

    for (uint32_t j = 0; j < end; j++) {
        std::string sq = src[j] >= 0 ? m.b.substr(m.seq_off + 64 * (size_t)src[j], 64) : std::string(64, '\0');
        if (!kept(j)) {   // left empty: no pictures
            put_be16(sq, 32, 0xFFFF);
            put_be16(sq, 34, 0);
            put_be32(sq, 52, 0);
            seqs += sq;
            continue;
        }
        const uint32_t i = (uint32_t)src[j];
        size_t so = m.sprite_at(i);
        int first_out = -1;
        for (uint32_t k = 0; k < m.sprites(i); k++) {
            std::string sp = m.b.substr(so + 32 * (size_t)k, 32);
            int bi = (int16_t)be16(sp, 0);
            if (cut) {
                const Piece *p = nullptr;
                for (auto &q : pieces)
                    if (q.seq == j && q.k == k) p = &q;
                if (p) {
                    size_t e = m.bd_off + 48 * (size_t)p->page;
                    int w = be16(m.b, e + 4), h = be16(m.b, e + 6);
                    size_t ne = 48 * (size_t)p->out;
                    int W = be16(bds, ne + 4), H = be16(bds, ne + 6);
                    put_be16(sp, 0, (uint16_t)p->out);
                    wf(sp, 8, (float)((p->nx + rf(sp, 8) * w - p->x0) / W));
                    wf(sp, 12, (float)((p->nx + rf(sp, 12) * w - p->x0) / W));
                    wf(sp, 16, (float)((p->ny + rf(sp, 16) * h - p->y0) / H));
                    wf(sp, 20, (float)((p->ny + rf(sp, 20) * h - p->y0) / H));
                    wf(sp, 24, (float)(rf(sp, 24) * w / W));   // registration point: the same in pixels
                    wf(sp, 28, (float)(rf(sp, 28) * h / H));
                    if (first_out < 0) first_out = p->out;
                } else {
                    put_be16(sp, 0, 0xFFFF);
                }
            } else if (bi >= 0) {
                put_be16(sp, 0, (uint16_t)new_page(bi));
            }
            sprs += sp;
        }
        int16_t first = (int16_t)be16(sq, 32);
        if (cut) {
            put_be16(sq, 32, first_out < 0 ? 0xFFFF : (uint16_t)first_out);
            put_be16(sq, 34, first_out < 0 ? 0 : 1);
        } else if (first >= 0) {
            put_be16(sq, 32, (uint16_t)new_page(first));
        }
        seqs += sq;
    }
    const size_t S = 64;
    std::string o = x.b.substr(0, x.pp_off) + x.b.substr(x.pp_off, x.pp) + mpix + seqs + sprs + bds;
    put_be32(o, S + 48, x.pp + (uint32_t)mpix.size());
    put_be32(o, S + 84, x.nseq + end);
    put_be32(o, S + 96, x.nbd + added_bd);
    put_be32(o, 40, ~crc32_of(o.data() + 64, o.size() - 64));
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    f << o;
    return (bool)f;
}
// The same, adding the map's sequences from number `from` on, in order (`keep`: the map's numbers).
bool merge_icon_sheets(const fs::path &xbox_sheet, const fs::path &map_sheet, uint32_t from, const fs::path &out,
                       const std::set<uint32_t> *keep = nullptr) {
    BitmapParts x, m;
    if (!x.read(xbox_sheet) || !m.read(map_sheet) || from > m.nseq) return false;
    std::vector<int> src;
    for (uint32_t i = from; i < m.nseq; i++) src.push_back((int)i);
    std::set<uint32_t> k;
    if (keep)
        for (uint32_t i : *keep)
            if (i >= from) k.insert(x.nseq + (i - from));
    return merge_icon_sheets_from(xbox_sheet, map_sheet, src, out, keep ? &k : nullptr);
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

// Model shaders' multipurpose maps in the target game's channel order (reorder_multipurpose_bitmap_tag). Xbox
// output: the map's own multipurpose maps from a PC or MCC map are in PC's order (unless their shaders have MCC's
// "use xbox multipurpose channel order" flag), and every model shader on the Xbox reads the Xbox's, so they're
// rewritten; one also used another way (a base map, or by a shader that has the flag) gets a rewritten copy for the
// shaders that need it. Custom Edition output from an Xbox map: the other way round. MCC output from an Xbox map: the
// shaders get the flag instead (nothing to rewrite).
void reorder_multipurpose_maps(Job &job, const std::vector<std::string> &deps) {
    enum { kToXbox, kToPc, kFlag } mode;
    if (job.target == Game::Xbox) {
        if (job.from == Game::Xbox) return;
        mode = kToXbox;
    } else {
        if (job.from != Game::Xbox) return;
        mode = job.target == Game::MCC ? kFlag : kToPc;
    }
    // A tag of the map's own: not the target's stock copy, nor one the app copied from it.
    auto from_map = [&](const std::string &t) {
        fs::path own = job.owner(t);
        if (own.empty() || own == job.overlay) return false;
        if (own == job.fixes)
            return fs::exists(job.source / from_u8(t)) || (!job.user_tags.empty() && fs::exists(job.user_tags / from_u8(t)));
        return true;
    };
    struct Use { std::string shader; bool xbox_order = false, aux = false, change_color = false; };
    std::map<std::string, std::vector<Use>> uses;   // multipurpose map -> model shaders reading it
    std::set<std::string> other;                     // bitmaps shaders use some other way
    for (auto &dep : deps) {
        std::string ext = fs::path(from_u8(dep)).extension().u8string();
        if (ext.compare(0, 7, ".shader") != 0) continue;
        fs::path own = job.owner(dep);
        tagfile::Tag t;
        if (own.empty() || !t.load(own / from_u8(dep))) continue;
        tagfile::Elem r = t.root();
        auto add_other = [&](const std::string &m) { if (!m.empty()) other.insert(m); };
        if (ext == ".shader_model") {
            std::string mp = r.ref("multipurpose map");
            for (const char *f : {"base map", "detail map", "reflection cube map"}) add_other(r.ref(f));
            if (mp.empty()) continue;
            Use u;
            u.shader = dep;
            // (a stock shader of the target has no such flag: it reads the target's order)
            u.xbox_order = own != job.overlay && ((long long)r.num("shader model flags") & 64);
            int mask = (int)r.integer("detail mask");
            u.aux = mask == 7 || mask == 8;
            u.change_color = r.integer("change color source") != 0;
            uses[mp].push_back(u);
        } else if (ext == ".shader_environment") {
            for (const char *f : {"base map", "primary detail map", "secondary detail map", "micro detail map", "bump map", "map",
                                  "reflection cube map"})
                add_other(r.ref(f));
        } else {
            for (const char *blk : {"maps", "maps 4 stage", "maps 2 stage"}) {
                tagfile::Block mb = r.block(blk);
                for (size_t i = 0; i < mb.size(); i++) add_other(mb[i].ref("map"));
            }
        }
    }
    int maps = 0, copies = 0, shaders = 0, flagged = 0;
    for (auto &[bm, us] : uses) {
        if (!has_suffix(bm, ".bitmap") || !from_map(bm)) continue;   // the target's own (in its order), or one the app made
        if (mode == kFlag) {
            for (auto &u : us) {
                if (!from_map(u.shader) || u.xbox_order) continue;
                fs::path ed = editable_owner(job, u.shader);
                tagfile::EditTag et;
                if (ed.empty() || !et.load(ed / from_u8(u.shader))) continue;
                tagfile::set_num(et.root(), "shader model flags", (double)((long long)tagfile::get_num(et.root(), "shader model flags") | 64));
                if (et.save(ed / from_u8(u.shader))) flagged++;
            }
            continue;
        }
        // The shaders that read it in the other order than it's in: on the Xbox every one without the flag (with it,
        // the map is already in the Xbox's order for that shader); on Custom Edition, from an Xbox map, all of them.
        std::vector<const Use *> need;
        bool shared = other.count(bm) > 0;
        for (auto &u : us) {
            if (mode == kToXbox && u.xbox_order) { shared = true; continue; }
            need.push_back(&u);
        }
        if (need.empty()) continue;
        bool alpha = false;
        for (auto *u : need) alpha = alpha || (mode == kToXbox ? u->aux : u->change_color);
        std::string target = bm;
        fs::path dir;
        if (shared) {   // a copy for the shaders that need it
            target = bm.substr(0, bm.size() - 7) + (mode == kToXbox ? " xbox order" : " pc order") + ".bitmap";
            fs::path own = job.owner(bm);
            std::error_code ec;
            fs::create_directories((job.fixes / from_u8(target)).parent_path(), ec);
            fs::copy_file(own / from_u8(bm), job.fixes / from_u8(target), fs::copy_options::overwrite_existing, ec);
            if (ec) continue;
            dir = job.fixes;
        } else {
            dir = editable_owner(job, bm);
            if (dir.empty()) continue;
        }
        if (reorder_multipurpose_bitmap_tag(dir / from_u8(target), mode == kToXbox, alpha) <= 0) {
            if (shared) { std::error_code ec; fs::remove(dir / from_u8(target), ec); }
            continue;
        }
        maps++;
        if (!shared) { shaders += (int)need.size(); continue; }
        copies++;
        std::string path = target.substr(0, target.size() - 7);
        std::replace(path.begin(), path.end(), '/', '\\');
        for (auto *u : need) {
            fs::path ed = editable_owner(job, u->shader, true);
            tagfile::EditTag et;
            if (ed.empty() || !et.load(ed / from_u8(u->shader))) continue;
            std::string *ref = tagfile::ref_path(et.root(), "multipurpose map");
            if (!ref) continue;
            *ref = path;
            if (et.save(ed / from_u8(u->shader))) shaders++;
        }
    }
    if (flagged)
        job.note("[pc] flagged " + std::to_string(flagged) + " model shader" + (flagged == 1 ? "" : "s") +
                 " to read their multipurpose maps in the Xbox's channel order (reflection in red, colour change in blue), "
                 "as the Xbox map has them");
    if (!maps) return;
    std::string n = std::to_string(maps) + " multipurpose map" + (maps == 1 ? "" : "s") + " (" + std::to_string(shaders) +
                    " model shader" + (shaders == 1 ? "" : "s") + (copies ? ", " + std::to_string(copies) + " of them copies" : "") + ")";
    if (mode == kToXbox)
        job.note("[shaders] put " + n + " in the Xbox's channel order: PC keeps the reflection mask in blue and the colour "
                 "change in alpha, the Xbox reads them from red and blue, so reflections and team colours showed in the wrong "
                 "places, or not at all");
    else
        job.note("[pc] put " + n + " in PC's channel order: the Xbox keeps the reflection mask in red and the colour change "
                 "in blue, PC reads them from blue and alpha");
}

// The map's own bitmaps its level's shaders use as bump maps (compact_bump_bitmap_tag can store them smaller), each
// with whether a shader alpha-tests through it. Not one a shader reads as a specular mask ("bump map is specular
// mask"), nor one also used some other way, nor Bungie's (already in their Xbox format).
std::map<std::string, bool> level_bump_maps(Job &job, const std::vector<std::string> &deps) {
    auto from_map = [&](const std::string &t) {
        fs::path own = job.owner(t);
        if (own.empty() || own == job.overlay) return false;
        if (own == job.fixes)
            return fs::exists(job.source / from_u8(t)) || (!job.user_tags.empty() && fs::exists(job.user_tags / from_u8(t)));
        return true;
    };
    std::map<std::string, bool> bumps;
    std::set<std::string> other;
    for (auto &dep : deps) {
        std::string ext = fs::path(from_u8(dep)).extension().u8string();
        if (ext.compare(0, 7, ".shader") != 0) continue;
        fs::path own = job.owner(dep);
        tagfile::Tag t;
        if (own.empty() || !t.load(own / from_u8(dep))) continue;
        tagfile::Elem r = t.root();
        auto add_other = [&](const std::string &m) { if (!m.empty()) other.insert(m); };
        if (ext == ".shader_environment") {
            int flags = (int)r.integer("shader environment flags");
            std::string bm = r.ref("bump map");
            if (!bm.empty()) {
                if (flags & 2) other.insert(bm);   // (bump map is specular mask)
                else bumps[bm] = bumps[bm] || (flags & 1);
            }
            for (const char *f : {"base map", "primary detail map", "secondary detail map", "micro detail map", "map",
                                  "reflection cube map"})
                add_other(r.ref(f));
        } else if (ext == ".shader_model") {
            for (const char *f : {"base map", "multipurpose map", "detail map", "reflection cube map"}) add_other(r.ref(f));
        } else {
            for (const char *blk : {"maps", "maps 4 stage", "maps 2 stage"}) {
                tagfile::Block mb = r.block(blk);
                for (size_t i = 0; i < mb.size(); i++) add_other(mb[i].ref("map"));
            }
        }
    }
    for (auto it = bumps.begin(); it != bumps.end();)
        it = !has_suffix(it->first, ".bitmap") || other.count(it->first) || !from_map(it->first) ? bumps.erase(it) : std::next(it);
    return bumps;
}

// A new 2D bitmap tag of RGBA8 pixels (w x h, mip levels down to 1x1 by 2x2 averaging), A8R8G8B8, made from `like` (an
// existing bitmap tag, for the rest of its fields).
static bool write_rgba_bitmap_tag(const fs::path &like, const fs::path &out, int w, int h, const std::vector<unsigned char> &rgba) {
    tagfile::EditTag t;
    if (!t.load(like)) return false;
    tagfile::TagNode &r = t.root();
    std::string pix;
    std::vector<unsigned char> lv = rgba;
    int cw = w, ch = h, mips = 0;
    for (;;) {
        for (size_t i = 0; i < (size_t)cw * ch; i++) {
            const unsigned char *q = &lv[i * 4];
            char px[4] = {(char)q[2], (char)q[1], (char)q[0], (char)q[3]};   // B G R A
            pix.append(px, 4);
        }
        if (cw == 1 && ch == 1) break;
        std::vector<unsigned char> half;
        box_half(lv, cw, ch, half);
        lv.swap(half);
        cw = std::max(1, cw / 2);
        ch = std::max(1, ch / 2);
        mips++;
    }
    tagfile::set_num(r, "type", 0);
    tagfile::set_num(r, "encoding format", 4);
    tagfile::set_num(r, "usage", 1);   // default
    tagfile::set_num(r, "color plate width", 0);
    tagfile::set_num(r, "color plate height", 0);
    if (std::string *cp = tagfile::data(r, "compressed color plate data")) cp->clear();
    std::string *pp = tagfile::data(r, "processed pixel data");
    auto *bd = tagfile::block(r, "bitmap data");
    auto *seq = tagfile::block(r, "bitmap group sequence");
    if (!pp || !bd || bd->empty()) return false;
    *pp = pix;
    bd->resize(1);
    tagfile::TagNode &e = (*bd)[0];
    tagfile::set_num(e, "width", w);
    tagfile::set_num(e, "height", h);
    tagfile::set_num(e, "depth", 1);
    tagfile::set_num(e, "type", 0);
    tagfile::set_num(e, "format", 11);
    tagfile::set_num(e, "flags", 1);   // power-of-two sides
    tagfile::set_num(e, "registration point", w / 2, 0);
    tagfile::set_num(e, "registration point", h / 2, 1);
    tagfile::set_num(e, "mipmap count", mips);
    tagfile::set_num(e, "pixel data offset", 0);
    tagfile::set_num(e, "pixel data size", (double)pix.size());
    if (seq && seq->size() != 1) seq->clear();
    if (seq && seq->size() == 1) {
        tagfile::set_num((*seq)[0], "first bitmap index", 0);
        tagfile::set_num((*seq)[0], "bitmap count", 1);
        if (auto *sprites = tagfile::block((*seq)[0], "sprites")) sprites->clear();
    }
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    return t.save(out);
}

// Whether a bitmap tag's first texture is opaque all over (alpha 255 at full size). False if it can't be read.
static bool bitmap_opaque(const fs::path &p) {
    tagfile::Tag t;
    if (!t.load(p)) return false;
    tagfile::Elem r = t.root();
    tagfile::Block bd = r.block("bitmap data");
    if (!bd.size()) return false;
    int w = (int)bd[0].integer("width"), h = (int)bd[0].integer("height"), fmt = (int)bd[0].integer("format");
    if (fmt == 1 || fmt == 6 || fmt == 10) return true;   // Y8, R5G6B5, X8R8G8B8: no alpha
    std::string pix = r.data("processed pixel data");
    size_t off = (size_t)bd[0].integer("pixel data offset"), n = mip_bytes(w, h, fmt);
    if (!n || off + n > pix.size()) return false;
    std::string d = pix.substr(off, n);
    if (fmt == 0 || fmt == 2 || fmt == 3) {   // A8, AY8 (alpha is the value), A8Y8 (alpha the second byte)
        for (size_t i = 0; i < (size_t)w * h; i++)
            if ((unsigned char)d[fmt == 3 ? i * 2 + 1 : i] != 255) return false;
        return true;
    }
    std::vector<unsigned char> rgba;
    if (!decode_level(d, w, h, fmt, rgba)) return false;
    for (size_t i = 0; i < (size_t)w * h; i++)
        if (rgba[i * 4 + 3] != 255) return false;
    return true;
}

// MCC's compatibility flags the Xbox doesn't read, done the Xbox's way where they can be (the map's own shaders):
//  - "custom edition blending" on a transparent shader whose framebuffer blend is multiply or double multiply: MCC
//    then draws it as Custom Edition did (chicago_shader.psh in MCC's mod tools), fading it the wrong way: towards
//    its own colour as it shows, so where nothing fades it, multiply leaves what's behind as it was (white) and double
//    multiply draws the average of its colour and white, (1 + c) / 2. The Xbox fades towards those as it fades out
//    (lerp(1, c, f), lerp(0.5, c, f)). A stage added after its last map gives the Xbox what MCC draws where it's fully
//    shown (no fog, no fade): white (the stage takes the next map, plain white), or the average with white (it blends in
//    the next map by that map's alpha, white at half). In fog the two still differ (MCC's darkens, the Xbox's fades).
//    A shader with four maps already has no stage to spare and stays as made.
//  - "use alternate normal type blending" on a level shader of the normal type: its detail maps blended by the base
//    map's alpha (Custom Edition's way, Chimera's map hacks: the Xbox's normal type blends them by the secondary detail
//    map's alpha). The Xbox's "blended" type does the same blend; its specular mask from the detail maps by the base
//    alpha, where the normal type's also multiplies by it, so where both detail maps are opaque "blended base
//    specular" (the base alpha alone) gives the normal type's mask exactly.
//  - "use alternate bump attenuation" (MCC's bump lighting relative to a flat surface, env_lm_normal.psh:
//    1 + dot(bump, incident) - dot(incident, up), where the Xbox's is saturate(dot(bump, incident))): a sum the Xbox's
//    lightmap pass can't do; counted, left as it is.
void mcc_flags_for_xbox(Job &job, const std::vector<std::string> &deps) {
    if (job.from == Game::Xbox) return;
    auto from_map = [&](const std::string &t) {
        fs::path own = job.owner(t);
        if (own.empty() || own == job.overlay) return false;
        if (own == job.fixes)
            return fs::exists(job.source / from_u8(t)) || (!job.user_tags.empty() && fs::exists(job.user_tags / from_u8(t)));
        return true;
    };
    const std::string white_tag = "halo map porter/white half alpha.bitmap";
    bool white_made = false;
    int mul = 0, dmul = 0, full = 0, normal_blended = 0, normal_base_spec = 0, alt_bump = 0;
    for (auto &dep : deps) {
        bool chicago = has_suffix(dep, ".shader_transparent_chicago"), env = has_suffix(dep, ".shader_environment");
        if ((!chicago && !env) || !from_map(dep)) continue;
        fs::path own = job.owner(dep);
        tagfile::Tag t;
        if (!t.load(own / from_u8(dep))) continue;
        tagfile::Elem r = t.root();
        if (chicago) {
            int fb = (int)r.integer("framebuffer blend function"), xf = (int)r.integer("extra flags");
            if (!(xf & 4) || (fb != 1 && fb != 2)) continue;
            tagfile::Block mb = r.block("maps");
            if (mb.size() == 0) continue;
            if (mb.size() >= 4) { full++; continue; }
            if (!white_made) {
                std::string like = mb[0].ref("map");
                fs::path lo = like.empty() ? fs::path() : job.owner(like);
                std::vector<unsigned char> px(4 * 4 * 4, 255);
                for (int i = 0; i < 16; i++) px[i * 4 + 3] = 128;   // white, half alpha
                if (lo.empty() || !write_rgba_bitmap_tag(lo / from_u8(like), job.fixes / from_u8(white_tag), 4, 4, px)) continue;
                white_made = true;
            }
            fs::path ed = editable_owner(job, dep);
            tagfile::EditTag et;
            if (ed.empty() || !et.load(ed / from_u8(dep))) continue;
            auto *maps = tagfile::block(et.root(), "maps");
            if (!maps || maps->empty() || maps->size() >= 4) continue;
            tagfile::TagNode &last = maps->back();
            // (its "alpha replicate" would read the added map's alpha as its colour; with no map after it, it read nothing)
            tagfile::set_num(last, "flags", (double)((int)tagfile::get_num(last, "flags") & ~2));
            tagfile::set_num(last, "color function", fb == 1 ? 1 : 11);   // next map / blend next map alpha
            tagfile::set_num(last, "alpha function", 0);                   // current
            // (a copy of its first map, for the reference's bitmap group, with every value set back to none)
            tagfile::TagNode m = maps->front();
            for (const char *f : {"flags", "color function", "alpha function", "map u offset", "map v offset", "map rotation",
                                  "mipmap bias", "u animation source", "u animation function", "u animation period",
                                  "u animation phase", "u animation scale", "v animation source", "v animation function",
                                  "v animation period", "v animation phase", "v animation scale", "rotation animation source",
                                  "rotation animation function", "rotation animation period", "rotation animation phase",
                                  "rotation animation scale"})
                tagfile::set_num(m, f, 0);
            tagfile::set_num(m, "rotation animation center", 0, 0);
            tagfile::set_num(m, "rotation animation center", 0, 1);
            tagfile::set_num(m, "map u scale", 1);
            tagfile::set_num(m, "map v scale", 1);
            if (std::string *ref = tagfile::ref_path(m, "map")) {
                std::string path = white_tag.substr(0, white_tag.size() - 7);
                std::replace(path.begin(), path.end(), '/', '\\');
                *ref = path;
            }
            maps->push_back(m);
            if (et.save(ed / from_u8(dep))) (fb == 1 ? mul : dmul)++;
        } else {
            int flags = (int)r.integer("shader environment flags"), type = (int)r.integer("shader environment type");
            if (flags & 8) alt_bump++;
            if (!(flags & 16) || type != 0) continue;
            bool opaque = true;
            for (const char *f : {"primary detail map", "secondary detail map"}) {
                std::string b = r.ref(f);
                fs::path bo = b.empty() ? fs::path() : job.owner(b);
                if (!b.empty() && (bo.empty() || !bitmap_opaque(bo / from_u8(b)))) opaque = false;
            }
            fs::path ed = editable_owner(job, dep);
            tagfile::EditTag et;
            if (ed.empty() || !et.load(ed / from_u8(dep))) continue;
            tagfile::set_num(et.root(), "shader environment type", opaque ? 2 : 1);
            if (et.save(ed / from_u8(dep))) (opaque ? normal_base_spec : normal_blended)++;
        }
    }
    if (mul || dmul)
        job.note("[shaders] drew " + std::to_string(mul + dmul) + " of the map's transparent shaders flagged with MCC's "
                 "\"custom edition blending\" as MCC draws them where they're fully shown (a stage added after their maps): " +
                 std::to_string(mul) + " multiply ones leave what's behind as it is, " + std::to_string(dmul) +
                 " double multiply ones draw the average of their colour and white. In fog they still fade the Xbox's way");
    if (full)
        job.note("[shaders] " + std::to_string(full) + " transparent shaders flagged with MCC's \"custom edition blending\" "
                 "already use all four maps, so they blend the Xbox's way (their multiply darker than in MCC)");
    if (normal_blended || normal_base_spec)
        job.note("[shaders] gave " + std::to_string(normal_blended + normal_base_spec) + " level shaders flagged with MCC's "
                 "\"alternate normal type blending\" the type that blends their detail maps by the base map's alpha, as MCC "
                 "does: " + std::to_string(normal_base_spec) + " \"blended base specular\" (opaque detail maps: the same "
                 "specular mask), " + std::to_string(normal_blended) + " \"blended\"");
    if (alt_bump)
        job.note("[shaders] " + std::to_string(alt_bump) + " level shaders use MCC's \"alternate bump attenuation\", which "
                 "the Xbox doesn't have: their bumps are lit the Xbox's way (surfaces lit from a low angle look darker "
                 "than in MCC, where the flat parts of a bump map are as bright as the lightmap)");
}

// The map's own effects brought down to Bungie's busiest (effects.hpp): an effect made once (a muzzle flash, an
// impact, an explosion) at its busiest moment, one attached to an object (a fire on a prop, a beam, a burning wreck)
// steadily, in particles alive and in the area of their sprites. One past those keeps the share of its particles that
// fits (its particle counts scaled; at least one where there was one); within them it stays as made. Stock effects are
// left alone. The level window can instead keep them all as made, or keep a quarter of the particles of every looping
// effect on an object (the porter's rule before it measured Bungie's).
static std::string pct(double share) {
    char b[16];
    snprintf(b, sizeof b, "%.0f%%", share * 100);
    return b;
}
void cap_effects(Job &job, const std::vector<std::string> &deps) {
    const int mode = job.cfg.xbox_effects;   // 0 Bungie's limits, 1 as made, 2 lighter
    if (mode == 1) return;
    static const char *objs[] = {".scenery", ".device_machine", ".device_control", ".device_light_fixture", ".vehicle",
                                 ".biped", ".weapon", ".equipment", ".garbage", ".projectile", ".placeholder",
                                 ".sound_scenery"};
    fx::Find find = [&](const std::string &t) {
        fs::path o = job.owner(t);
        return o.empty() ? fs::path() : o / from_u8(t);
    };
    // Effects attached to objects (they loop while the object exists).
    std::set<std::string> attached;
    for (auto &dep : deps) {
        bool obj = false;
        for (auto o : objs) obj = obj || has_suffix(dep, o);
        if (!obj) continue;
        fs::path own = job.owner(dep);
        tagfile::Tag t;
        if (own.empty() || !t.load(own / from_u8(dep))) continue;
        tagfile::Block at = t.root().block("attachments");
        for (size_t i = 0; i < at.size(); i++) {
            std::string g, e = at[i].ref("type", &g);
            if (g == "effe" && !e.empty()) attached.insert(e);
        }
    }
    fx::Analyzer an(find);
    fx::Caps caps;
    std::map<std::string, double> share;   // the map's own effects that keep less than all
    for (auto &dep : deps) {
        if (!has_suffix(dep, ".effect")) continue;
        fs::path own = job.owner(dep);
        if (own.empty() || own == job.overlay) continue;
        bool loops = attached.count(dep) > 0;
        double s = fx::keep_share(an.cost(dep), loops, caps);
        if (mode == 2 && loops) s = std::min(s, 0.25);
        if (s < 1) share[dep] = s;
    }
    // The effects one's parts make alongside it keep no more than it does (they're part of its cost).
    for (bool more = true; more;) {
        more = false;
        for (auto &[e, s] : share)
            for (auto &c : an.part_effects(e)) {
                fs::path own = job.owner(c);
                if (own.empty() || own == job.overlay) continue;
                auto it = share.find(c);
                if (it == share.end() || it->second > s + 1e-9) {
                    share[c] = s;
                    more = true;
                }
            }
    }
    std::vector<std::string> done;
    for (auto &[e, s] : share) {
        fs::path ed = editable_owner(job, e);
        if (ed.empty() || !fx::scale_particles(ed / from_u8(e), s)) continue;
        done.push_back(fs::path(from_u8(e)).stem().u8string() + " " + pct(s));
    }
    if (done.empty()) return;
    std::string list;
    for (size_t i = 0; i < done.size(); i++) list += (i ? ", " : "") + done[i];
    if (mode == 2)
        job.note("[effects] lightened " + std::to_string(done.size()) + " of the map's effects (looping ones on objects to a quarter "
                 "of their particles, the rest to Bungie's busiest), each keeping this share of its particles: " + list);
    else
        job.note("[effects] brought " + std::to_string(done.size()) + " of the map's effects down to Bungie's busiest (made once: 204 "
                 "particles and 188 square units of sprites at once, the rocket's explosion; looping on an object: 300 particles "
                 "and 1,150 square units, the plasma flame and beam emitters), each keeping this share of its particles: " + list);
}

// Xbox maps: keep texture memory close to stock maps. The map's own textures (stock tags, HUD and menu
// art are left alone) are compressed if they're uncompressed 32-bit, and capped: the level and sky at
// xbox_max_texture (512, like stock maps); an object's own textures (its model's, and a weapon's first-person
// model's) as large as Bungie's for its kind: vehicles, weapons, characters and scenery at xbox_max_texture (512,
// their largest), their bump, detail and cube maps at 256, small items (equipment, garbage, projectiles) at 256;
// everything else (effects, decals, particles) at xbox_max_object_texture (256); lightmaps at xbox_max_lightmap.
// The bitmaps whose alpha a shader alpha-tests (a pixel drawn only where it's over half): a model shader's base map
// unless it's "not alpha tested" or an alpha-blended decal, an environment shader's bump map when "alpha tested" (its
// lightmap pass draws with the bump map in the first stage and tests its alpha; the base map's alpha is a detail
// blend or specular mask there, on PC and MCC too), a transparent shader's first map when "alpha tested"
// (rasterizer_xbox_models.c, _environment.c, _transparent_geometry.c). Their see-through parts are what halve_cutout
// keeps when they're made smaller.
static void find_alpha_tested(Job &job, const std::vector<std::string> &deps) {
    for (auto &dep : deps) {
        std::string ext = fs::path(from_u8(dep)).extension().u8string();
        if (ext != ".shader_model" && ext != ".shader_environment" && ext != ".shader_transparent_chicago" &&
            ext != ".shader_transparent_chicago_extended" && ext != ".shader_transparent_generic")
            continue;
        fs::path own = job.owner(dep);
        tagfile::Tag t;
        if (own.empty() || !t.load(own / from_u8(dep))) continue;
        tagfile::Elem r = t.root();
        std::string map;
        if (ext == ".shader_model") {
            long long f = (long long)r.num("shader model flags");
            if (!(f & 4) && !(f & 8)) map = r.ref("base map");
        } else if (ext == ".shader_environment") {
            long long f = (long long)r.num("shader environment flags");
            if ((f & 1) && !(f & 2)) map = r.ref("bump map");   // (2: "bump map is specular mask": none in that stage)
        } else {
            const char *flags = ext == ".shader_transparent_chicago" ? "shader transparent chicago flags"
                              : ext == ".shader_transparent_generic" ? "shader transparent generic flags"
                                                                      : "shader transparent chicago extended flags";
            if ((long long)r.num(flags) & 1) {
                tagfile::Block maps = r.block(ext == ".shader_transparent_chicago_extended" ? "maps 4 stage" : "maps");
                if (!maps.size() && ext == ".shader_transparent_chicago_extended") maps = r.block("maps 2 stage");
                if (maps.size()) map = maps[0].ref("map");
            }
        }
        if (!map.empty()) job.alpha_tested.insert(map);
    }
}

// The map's objects' own textures (their models', a weapon's first-person model's, the map's own first-person
// arms'), by kind: the largest side Bungie's Xbox textures of the kind have (vehicles, weapons, first-person
// weapons, characters and scenery up to 512, their bump and detail maps up to 128; ammo and other pickups up to
// 256; effects mostly up to 256), and whether they're seen up close (vehicles, characters, weapons, first person).
struct ModelTexture { int cap = 0; bool close = false; };
static std::string map_first_person_hands(Job &job);
static std::map<std::string, ModelTexture> model_textures(Job &job, const std::vector<std::string> &deps) {
    static const std::tuple<const char *, int, bool> kinds[] = {
        {".vehicle", 512, true}, {".biped", 512, true}, {".weapon", 512, true}, {".scenery", 512, false},
        {".device_machine", 512, false}, {".device_control", 512, false}, {".device_light_fixture", 512, false},
        {".equipment", 256, false}, {".garbage", 256, false}, {".projectile", 256, false}};
    std::map<std::string, ModelTexture> models, out;
    auto add = [](std::map<std::string, ModelTexture> &m, const std::string &k, int cap, bool close) {
        ModelTexture &t = m[k];
        t.cap = std::max(t.cap, cap);
        t.close = t.close || close;
    };
    for (auto &dep : deps)
        for (auto &[ext, cap, close] : kinds) {
            if (!has_suffix(dep, ext)) continue;
            fs::path own = job.owner(dep);
            if (own.empty()) break;
            std::string m = perf::object_model(own / from_u8(dep));
            if (!m.empty()) add(models, m, cap, close);
            if (std::string(ext) == ".weapon") {
                std::string fp = perf::weapon_first_person_model(own / from_u8(dep));
                if (!fp.empty()) add(models, fp, cap, true);
            }
            break;
        }
    std::string hands = map_first_person_hands(job);
    if (!hands.empty()) add(models, hands, 512, true);
    for (auto &[m, t] : models)
        for (auto &x : tag_dependencies(job, m, true))
            if (has_suffix(x, ".bitmap")) add(out, x, t.cap, t.close);
    return out;
}

void cap_texture_sizes(Job &job, const std::vector<std::string> &deps) {
    const Config &cfg = job.cfg;
    find_alpha_tested(job, deps);
    if (cfg.xbox_max_texture <= 0 && cfg.xbox_max_lightmap <= 0) return;
    g_cutout_textures = 0;
    g_remade_tops = 0;
    std::set<std::string> world, level; // textures the level (BSP) and sky use; the BSP alone
    for (auto &d : tag_dependencies(job, job.scenario + ".scenario", false))
        if (has_suffix(d, ".scenario_structure_bsp") || has_suffix(d, ".sky")) {
            bool bsp = has_suffix(d, ".scenario_structure_bsp");
            for (auto &x : tag_dependencies(job, d, true)) {
                world.insert(x);
                if (bsp) level.insert(x);
            }
        }
    std::map<std::string, ModelTexture> model_cap;   // the map's objects' own textures (model_textures)
    if (cfg.xbox_max_object_texture > 0 && cfg.xbox_max_texture > 0) model_cap = model_textures(job, deps);
    int textures = 0, objects = 0, lightmaps = 0, compressed = 0, model_textures = 0;
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
        const int usage = be16(head, 64 + 4), type = be16(head, 64);
        bool lightmap = usage == 4; // usage: light map
        bool object = !world.count(dep);
        int cap = lightmap ? cfg.xbox_max_lightmap
                : (object && cfg.xbox_max_object_texture > 0 && cfg.xbox_max_texture > 0)
                    ? std::min(cfg.xbox_max_object_texture, cfg.xbox_max_texture)
                    : cfg.xbox_max_texture;
        bool model = false;
        if (!lightmap && object && cfg.xbox_max_object_texture > 0 && cfg.xbox_max_texture > 0) {
            auto it = model_cap.find(dep);
            if (it != model_cap.end()) {
                // an object's own texture: its kind's size (bump, detail and vector maps, and cube maps, smaller; not
                // a bump map a shader alpha-tests, which is the surface's shape)
                bool shape = usage == 2 && job.alpha_tested.count(dep);
                int c = (usage == 2 && !shape) || usage == 3 || usage == 5 || type == 2 ? std::min(it->second.cap, 256) : it->second.cap;
                cap = std::min(std::max(cap, c), cfg.xbox_max_texture);
                model = true;
            }
        }
        if (!lightmap && cfg.xbox_max_texture > 0) {
            int c = compress_color_kept(cfg, own / from_u8(dep));
            if (c > 0) compressed += c;
        }
        if (cap <= 0) continue;
        // A bump map a shader alpha-tests is its surface's shape (a grate's holes, a railing's bars): capped by area
        // instead, as many pixels as a square of the cap, so a long strip keeps its detail along its length (Bungie's
        // a10 "alpha test bump", which Deathtrap's grates and railings use upscaled to 2048x512, is 1024x256 on the
        // Xbox).
        if (usage == 2 && job.alpha_tested.count(dep)) {
            BitmapParts bp;
            if (bp.read(own / from_u8(dep)) && bp.nbd > 0) {
                long long w = be16(bp.b, bp.bd_off + 4), h = be16(bp.b, bp.bd_off + 6), lo = std::min(w, h), hi = std::max(w, h);
                long long side = cap;
                while (side * 2 <= hi && lo > 0 && side * 2 * (side * 2 * lo / hi) <= (long long)cap * cap) side *= 2;
                cap = (int)side;
            }
        }
        int n = cap_bitmap_tag(own / from_u8(dep), cap, job.alpha_tested.count(dep) > 0);
        if (n > 0) (lightmap ? lightmaps : model ? model_textures : object && cap < cfg.xbox_max_texture ? objects : textures) += n;
    }
    // An effect's bitmap (a particle's, a particle system's, a contrail's, weather's) as a whole: Bungie's biggest is
    // 336 KB (the pistol's muzzle flash: two 256x256 frames), and all of theirs together 4.6 MB. A map's own particle
    // animation with dozens of 256x256 frames is several times that (CMT's beam cannon's plasma arcs: 30 frames,
    // 1.2 MB), so every frame of one bigger than Bungie's biggest is halved until the whole bitmap fits it (still down
    // to 32 px at most): small on screen, where they're seen, they look about the same.
    const size_t kBungieEffectBitmap = 344064;
    int effect_bitmaps = 0;
    size_t fx_before = 0, fx_after = 0, fx_top = 0;
    std::string fx_top_name;
    int fx_top_frames = 0;
    if (cfg.xbox_max_object_texture > 0 && cfg.xbox_max_texture > 0) {
        std::set<std::string> fx;
        for (auto &dep : deps)
            if (has_suffix(dep, ".particle") || has_suffix(dep, ".particle_system") || has_suffix(dep, ".contrail") ||
                has_suffix(dep, ".weather_particle_system"))
                for (auto &x : tag_dependencies(job, dep, false))
                    if (has_suffix(x, ".bitmap")) fx.insert(x);
        for (auto &bm : fx) {
            if (world.count(bm) || model_cap.count(bm)) continue;   // (a level's or an object's own texture too)
            fs::path own = job.owner(bm);
            bool ours = own == job.fixes || (own == job.source && job.source != job.user_tags);
            std::string l = "/" + bm;
            for (auto &c : l) c = (char)tolower((unsigned char)c);
            if (own.empty() || !ours || l.find("/ui/") != std::string::npos || l.find("/hud") != std::string::npos) continue;
            fs::path p = own / from_u8(bm);
            int side = 0;
            const size_t was = bitmap_tag_memory(p, &side);
            size_t now = was;
            while (now > kBungieEffectBitmap && side > 32) {
                if (cap_bitmap_tag(p, side / 2, job.alpha_tested.count(bm) > 0) <= 0) break;
                int s2 = 0;
                size_t n2 = bitmap_tag_memory(p, &s2);
                if (n2 >= now) break;
                now = n2, side = s2;
            }
            if (now >= was) continue;
            effect_bitmaps++;
            fx_before += was, fx_after += now;
            if (was > fx_top) {
                BitmapParts bp;
                fx_top = was, fx_top_name = fs::path(from_u8(bm)).stem().u8string();
                fx_top_frames = bp.read(p) ? (int)bp.nbd : 0;
            }
        }
    }
    // Level texture budget: stock outdoor maps use about 3-6 MB of level textures (Blood Gulch 3.3 MB,
    // Sidewinder 5.3 MB, not counting lightmaps). The Xbox loads textures as they come into view, and a
    // level far over that makes it stutter (or drop HUD textures) wherever you can see most of the map.
    // Uncompressed ones are compressed first, then the biggest are halved, bump and detail maps first,
    // until the level fits. Stock level textures count too (each Xbox map has its own copy).
    int halved = 0, bumps_p8 = 0, bumps_dxt1 = 0;
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
                int c = ed.empty() ? 0 : compress_color_kept(cfg, ed / from_u8(dep));
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
        std::map<std::string, bool> bumps = level_bump_maps(job, deps);   // the map's own bump maps
        std::set<std::string> packed_tried;
        // A bump map a shader alpha-tests is the surface's shape: stored smaller without losing pixels straight away,
        // and halved only once everything else is as small as it goes.
        auto shape = [&](const Tex &t) { return bumps.count(t.tag) && bumps[t.tag]; };
        for (auto &t : tex) {
            if (total <= budget || !shape(t)) continue;
            fs::path ed = editable_owner(job, t.tag, true);
            int nf = 0;
            if (ed.empty() || !packed_tried.insert(t.tag).second || compact_bump_kept(cfg, ed / from_u8(t.tag), true, &nf) <= 0) continue;
            int side = 0;
            size_t now = bitmap_tag_memory(ed / from_u8(t.tag), &side);
            if (now >= t.bytes) continue;
            total -= t.bytes - now;
            t.bytes = now;
            (nf == 17 ? bumps_p8 : bumps_dxt1)++;
        }
        while (total > budget) {
            // Bump, detail and vector maps first (down to 128 px), then the biggest color textures
            // (down to 128 px), then alpha-tested bump maps. Ties go to the widest texture. A bump map of the map's
            // own is first stored smaller without losing pixels (compact_bump_bitmap_tag), and only halved after that.
            Tex *pick = nullptr;
            for (int pass = 0; pass < 3 && !pick; pass++)
                for (auto &t : tex) {
                    bool cheap = !shape(t) && (t.usage == 2 || t.usage == 3 || t.usage == 5 || bumps.count(t.tag));
                    if (t.done || t.side <= 128 || (pass == 0 && !cheap) || (pass == 1 && shape(t)) || (pass == 2 && !shape(t))) continue;
                    if (!pick || t.bytes > pick->bytes || (t.bytes == pick->bytes && t.side > pick->side)) pick = &t;
                }
            if (!pick) break;
            fs::path ed = editable_owner(job, pick->tag, true);
            if (!ed.empty() && bumps.count(pick->tag) && packed_tried.insert(pick->tag).second) {
                int nf = 0;
                if (compact_bump_kept(cfg, ed / from_u8(pick->tag), bumps[pick->tag], &nf) > 0) {
                    int side = 0;
                    size_t now = bitmap_tag_memory(ed / from_u8(pick->tag), &side);
                    if (now < pick->bytes) {
                        total -= pick->bytes - now;
                        pick->bytes = now;
                        (nf == 17 ? bumps_p8 : bumps_dxt1)++;
                        continue;
                    }
                }
            }
            if (ed.empty() || cap_bitmap_tag(ed / from_u8(pick->tag), pick->side / 2, job.alpha_tested.count(pick->tag) > 0) <= 0) {
                pick->done = true;
                continue;
            }
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
    if (bumps_p8 || bumps_dxt1)
        job.note("[textures] packed " + std::to_string(bumps_p8 + bumps_dxt1) + " of the map's bump maps into less memory "
                 "before halving any textures: " + std::to_string(bumps_p8) + " in the Xbox's own bump format (P8: a byte per "
                 "pixel from Bungie's palette of 256 directions, as their levels have them), " + std::to_string(bumps_dxt1) +
                 " as DXT1 (bumps steeper than that palette reaches), whichever turned each one's pixels least (a few "
                 "degrees; halving turns them several times as far)");
    if (halved || bumps_p8 || bumps_dxt1) {
        char buf[64];
        snprintf(buf, sizeof buf, "%.1f MB -> %.1f MB", level_before / 1048576.0, level_after / 1048576.0);
        job.note("[textures] level textures " + std::string(buf) + (halved ? " (halved the biggest " + std::to_string(halved) +
                 " times, bump and detail maps first)" : std::string(" (no texture halved)")) +
                 " to stay near stock maps' texture memory");
    }
    if (textures)
        job.note("[textures] scaled " + std::to_string(textures) + " of your level textures down to " +
                 std::to_string(cfg.xbox_max_texture) + " px (largest side), like stock Xbox maps");
    if (model_textures)
        job.note("[textures] scaled " + std::to_string(model_textures) + " textures of the map's vehicles, weapons, characters, " +
                 "scenery and items down to the largest Bungie's of their kind have (" + std::to_string(cfg.xbox_max_texture) +
                 " px; bump, detail and cube maps and small items 256 px), so the Xbox doesn't stutter loading them as " +
                 "they come into view");
    if (objects)
        job.note("[textures] scaled " + std::to_string(objects) + " effect and other object textures down to " +
                 std::to_string(std::min(cfg.xbox_max_object_texture, cfg.xbox_max_texture)) +
                 " px, so the Xbox doesn't stutter loading them as they come into view");
    if (lightmaps)
        job.note("[textures] scaled " + std::to_string(lightmaps) + " lightmap pages down to " +
                 std::to_string(cfg.xbox_max_lightmap) + " px");
    if (effect_bitmaps) {
        char buf[200];
        const std::string frames = fx_top_frames > 1 ? " in " + std::to_string(fx_top_frames) + " frames" : "";
        snprintf(buf, sizeof buf, "%.1f MB now %.1f MB; the biggest, %s, %.2f MB%s", fx_before / 1048576.0, fx_after / 1048576.0,
                 fx_top_name.c_str(), fx_top / 1048576.0, frames.c_str());
        job.note("[textures] halved the frames of " + std::to_string(effect_bitmaps) + " effect bitmap" +
                 (effect_bitmaps == 1 ? "" : "s") + " bigger than Bungie's biggest (336 KB, the pistol's muzzle flash) until " +
                 (effect_bitmaps == 1 ? "it fits" : "each fits") + ": " + buf);
    }
    if (g_cutout_textures)
        job.note("[textures] " + std::to_string(g_cutout_textures) + " of those halvings were of see-through cut-out textures " +
                 "(leaves, fences, grates): made again from the full-size texture, keeping as much of them solid, instead of " +
                 "using their own smaller versions, which fade away");
    if (g_remade_tops)
        job.note("[textures] " + std::to_string(g_remade_tops) + " of those halvings averaged the full-size texture instead of " +
                 "using its own smaller version, which looked unlike it (faded out, or its alpha thinned)");
}

// All Xbox-only content passes that need the finished tag set.
// The map's animations stored as keyframes, the way Bungie stored the Xbox's cinematics (animcomp.hpp): its own, and
// the Xbox's it uses (copied to fixes). Also when measuring for the weapons & vehicles window: the map has them so.
static void xbox_animations(Job &job, const std::vector<std::string> &deps) {
    const int level = job.cfg.xbox_compress_animations;
    int graphs = 0, anims = 0, stock = 0, restored = 0;
    size_t before = 0, after = 0;
    double worst = 0;
    for (auto &dep : deps) {
        if (!has_suffix(dep, ".model_animations")) continue;
        fs::path own = job.owner(dep);
        tagfile::EditTag ed;
        if (own.empty() || !ed.load(own / from_u8(dep))) continue;
        animcomp::GraphResult r = animcomp::compress_graph(ed.root(), level);
        if (!r.compressed && !r.overlays_restored) continue;
        fs::path dst = editable_owner(job, dep, true);
        if (dst.empty() || !ed.save(dst / from_u8(dep))) continue;
        restored += r.overlays_restored;
        if (!r.compressed) continue;
        graphs++;
        anims += r.compressed;
        before += r.before;
        after += r.after;
        worst = std::max(worst, r.worst);
        if (own == job.overlay) stock++;
    }
    if (restored)
        job.note("[anim] gave " + std::to_string(restored) + " compressed overlay animations their frames back: the Xbox " +
                 "blends a compressed overlay's turns only at whole frames when it plays one by a value (a vehicle's " +
                 "velocity, steering and suspension, a rider's acceleration), so they snapped from pose to pose");
    if (!graphs) return;
    auto size = [](size_t b) {
        char t[32];
        if (b >= 1048576) snprintf(t, sizeof t, "%.1f MB", b / 1048576.0);
        else snprintf(t, sizeof t, "%d KB", (int)((b + 1023) / 1024));
        return std::string(t);
    };
    char mm[32];
    snprintf(mm, sizeof mm, "%.1f mm", worst * 3048.0);   // (a world unit is 10 feet)
    job.note("[anim] stored " + std::to_string(anims) + " animations of " + std::to_string(graphs) + " animation graphs" +
             (stock ? " (" + std::to_string(stock) + " of them the Xbox's own)" : "") +
             " as keyframes, the Xbox's compressed format" + (level >= 2 ? ", compressed harder" : "") + ": " +
             size(before) + " of tag memory now " + size(after) + " (the farthest any bone end moves: " + mm + ")");
}

// Collision of the map's own vehicles, scenery and devices made lighter (collision.hpp).
// First, Bungie's own where it fits: most custom vehicles and their wrecks are iterations of a stock one (the same
// skeleton, about the same size), so each part (node) whose name a stock vehicle's collision has takes the stock part,
// where it sits on this model as well as on Bungie's (the Coldsnap swarthog's 570 surfaces, the Warthog's 202). Only
// when that's lighter.
// Then, collision still too dense for bipeds is made again, simpler: a biped's movement gathers every surface, edge
// and corner within its reach, each checked against all those gathered so far, so a dense shape costs it the square of
// its detail (Deathtrap's pilot chair, 416 surfaces all in reach at once, cost each biped near it ten times Bungie's
// densest scenery). Only scenery and machines, which bipeds walk and vehicles drive into (vehicles they meet as physics
// spheres, controls not at all), and not a collision model a vehicle, character or item also uses.
// Never a collision model a character, weapon or item uses (their hit boxes), and never the Xbox's own.
// A collision model drawn for the LODs window: each part's BSP where the model's rest pose puts it, its surfaces
// coloured by material (the same colour for the same material, as made and as converted), their outlines drawn.
static ModelPreview collision_drawing(const std::vector<coll::NodeBsps> &nodes, const coll::Rig &rig) {
    static const uint8_t kColour[][3] = {{192, 80, 77}, {79, 129, 189}, {155, 187, 89}, {128, 100, 162}, {247, 150, 70},
                                         {75, 172, 198}, {227, 119, 194}, {160, 160, 160}};
    std::map<int, std::vector<std::vector<std::array<double, 3>>>> by_material;
    for (auto &n : nodes) {
        if (n.bsps.empty()) continue;
        std::array<double, 12> m{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
        for (size_t i = 0; i < rig.names.size(); i++)
            if (rig.names[i] == n.name) m = rig.world[i];
        for (auto &q : coll::polygons(n.bsps[0])) {
            std::vector<std::array<double, 3>> w;
            for (auto &p : q.pts) {
                std::array<double, 3> o;
                for (int r = 0; r < 3; r++) o[r] = m[9 + r] + m[r * 3] * p[0] + m[r * 3 + 1] * p[1] + m[r * 3 + 2] * p[2];
                w.push_back(o);
            }
            by_material[q.material == 0xFFFF ? -1 : q.material].push_back(w);
        }
    }
    ModelPreview p;
    for (auto &[mat, polys] : by_material) {
        ModelPreview::Part part;
        part.first = (uint32_t)(p.tris.size() / 3);
        part.texture = (int)p.textures.size();
        ModelPreview::Texture t;
        t.w = t.h = 4;
        const uint8_t *c = kColour[(mat < 0 ? 7 : mat) % 8];
        for (int k = 0; k < 16; k++) t.rgba.insert(t.rgba.end(), {c[0], c[1], c[2], 255});
        p.textures.push_back(std::move(t));
        for (auto &w : polys)
            for (size_t k = 1; k + 1 < w.size(); k++) {
                // (Halo's winding: corners clockwise seen from outside, the BSP's counterclockwise)
                uint32_t base = (uint32_t)(p.pos.size() / 3);
                for (const auto *v : {&w[0], &w[k + 1], &w[k]}) {
                    p.pos.insert(p.pos.end(), {(float)(*v)[0], (float)(*v)[1], (float)(*v)[2]});
                    p.uv.insert(p.uv.end(), {0.5f, 0.5f});
                }
                p.tris.insert(p.tris.end(), {base, base + 1, base + 2});
                // outline: (first, k+1) is the polygon's last edge, (k+1, k) always, (k, first) its first
                p.edges.push_back((uint8_t)((k + 2 == w.size() ? 1 : 0) | 2 | (k == 1 ? 4 : 0)));
                part.count++;
            }
        p.parts.push_back(part);
    }
    return p;
}

static void xbox_collision(Job &job, const std::vector<std::string> &deps) {
    const int level = job.cfg.xbox_collision;
    if (level <= 0 || job.analysis) return;
    const long budget = level >= 2 ? 90000 : 130000, bungie = 115000;   // (Bungie's densest MP scenery: about 115,000)
    struct Use { bool placed = true, vehicle = true; std::set<std::string> models; std::string name; };
    std::map<std::string, Use> uses;   // collision model -> what uses it
    for (auto &dep : deps) {
        bool place = has_suffix(dep, ".scenery") || has_suffix(dep, ".device_machine"), vehicle = has_suffix(dep, ".vehicle");
        bool other = has_suffix(dep, ".biped") || has_suffix(dep, ".weapon") || has_suffix(dep, ".equipment") || has_suffix(dep, ".garbage") ||
                     has_suffix(dep, ".projectile") || has_suffix(dep, ".device_control") || has_suffix(dep, ".device_light_fixture") ||
                     has_suffix(dep, ".placeholder");
        if (!place && !vehicle && !other) continue;
        fs::path own = job.owner(dep);
        tagfile::Tag ot;
        if (own.empty() || !ot.load(own / from_u8(dep))) continue;
        std::string cm = ot.root().ref("collision model");
        if (cm.empty()) continue;
        Use &u = uses[cm];
        u.placed = u.placed && place;
        u.vehicle = u.vehicle && (vehicle || place);
        if (std::string m = ot.root().ref("model"); !m.empty()) u.models.insert(m);
        if ((place || vehicle) && u.name.empty()) u.name = fs::path(from_u8(dep)).stem().u8string();
    }
    // Bungie's vehicles, to fit
    struct Stock { std::string name; std::vector<coll::NodeBsps> nodes; coll::Rig rig; };
    std::vector<Stock> stock;
    if (!job.overlay.empty())
        for (const char *v : {"warthog", "ghost", "scorpion", "banshee", "wraith", "c gun turret"}) {
            std::string base = std::string("vehicles/") + v + "/" + v;
            tagfile::Tag ct, mt;
            if (!ct.load(job.overlay / from_u8(base + ".model_collision_geometry")) || !mt.load(job.overlay / from_u8(base + ".model"))) continue;
            Stock st{v, coll::read_nodes(ct.root()), {}};
            if (coll::read_rig(mt.root(), st.rig)) stock.push_back(std::move(st));
        }
    auto times = [&](long work) {   // as a multiple of Bungie's densest
        char t[32];
        snprintf(t, sizeof t, "%.1fx", (double)work / bungie);
        return std::string(t);
    };
    std::vector<std::string> fitted, done, left;
    for (auto &[cm, use] : uses) {
        if (!use.vehicle) continue;   // (a character's, weapon's or item's too)
        fs::path own = job.owner(cm);
        if (own.empty() || own == job.overlay) continue;
        tagfile::Tag t;
        tagfile::EditTag ed;
        if (!t.load(own / from_u8(cm)) || !ed.load(own / from_u8(cm))) continue;
        std::vector<coll::NodeBsps> nodes = coll::read_nodes(t.root());
        const std::vector<coll::NodeBsps> as_made = nodes;
        std::string how;   // (for the LODs window)
        std::vector<tagfile::TagNode> *enodes = tagfile::block(ed.root(), "nodes");
        if (!enodes || enodes->size() != nodes.size()) continue;
        const std::string &name = use.name.empty() ? fs::path(from_u8(cm)).stem().u8string() : use.name;
        bool changed = false;
        auto write = [&](size_t i, size_t j, const coll::Bsp &b) {
            std::vector<tagfile::TagNode> *eb = tagfile::block((*enodes)[i], "bsps");
            if (!eb || j >= eb->size()) return false;
            coll::write_bsp((*eb)[j], b);
            nodes[i].bsps[j] = b;
            return changed = true;
        };
        // Bungie's own, where it fits: the stock vehicle whose collision shares most of this one's parts' names, judged
        // against every model this collision is drawn with
        {
            const Stock *best = nullptr;
            int best_shared = 0, with_bsps = 0;
            for (auto &n : nodes) with_bsps += !n.bsps.empty();
            for (auto &st : stock) {
                int shared = 0;
                for (auto &n : nodes)
                    if (!n.bsps.empty())
                        for (auto &sn : st.nodes)
                            if (sn.name == n.name && !sn.bsps.empty()) { shared++; break; }
                if (shared > best_shared) { best_shared = shared; best = &st; }
            }
            if (best && best_shared * 2 >= with_bsps && !use.models.empty()) {
                std::vector<coll::StockFit> fits;
                for (auto &m : use.models) {
                    fs::path mo = job.owner(m);
                    tagfile::Tag mt;
                    coll::Rig rig;
                    if (mo.empty() || !mt.load(mo / from_u8(m)) || !coll::read_rig(mt.root(), rig)) { fits.clear(); break; }
                    fits.push_back(coll::fit_stock(nodes, rig, best->nodes, best->rig));
                }
                size_t before = 0, after = 0;
                int parts = 0;
                std::vector<size_t> take;
                for (size_t i = 0; !fits.empty() && i < nodes.size(); i++) {
                    bool all = true;
                    for (auto &f : fits) all = all && !f.bsps[i].empty();
                    if (!all) continue;
                    take.push_back(i);
                    for (auto &b : nodes[i].bsps) before += b.surfaces.size();
                    for (auto &b : fits[0].bsps[i]) after += b.surfaces.size();
                }
                if (!take.empty() && after < before) {
                    for (size_t i : take) {
                        for (size_t j = 0; j < nodes[i].bsps.size(); j++) write(i, j, fits[0].bsps[i][j]);
                        parts++;
                    }
                    std::string what = "the " + best->name + "'s on " + std::to_string(parts) + " part" + (parts == 1 ? "" : "s") + ": " +
                                       std::to_string(before) + " surfaces now " + std::to_string(after);
                    fitted.push_back(name + " (" + what + ")");
                    how = "Bungie's " + best->name + " collision where it fits, on " + std::to_string(parts) + " part" + (parts == 1 ? "" : "s") +
                          ": " + std::to_string(before) + " surfaces now " + std::to_string(after) + ".";
                }
            }
        }
        // still too dense for bipeds: made again (scenery and machines)
        if (use.placed) {
            size_t s0 = 0, s1 = 0;
            long w0 = 0, w1 = 0;
            int pieces = 0;
            double moved = 0;
            bool lightened = false;
            for (size_t i = 0; i < nodes.size(); i++)
                for (size_t j = 0; j < nodes[i].bsps.size(); j++) {
                    const coll::Bsp &b = nodes[i].bsps[j];
                    if (b.surfaces.empty()) continue;
                    coll::Bsp o;
                    coll::Lightened r = coll::lighten(b, budget, o);
                    if (!r.changed) {
                        if (r.why != "light enough") left.push_back(name + " (" + times(r.work_before) + "; " + r.why + ")");
                        continue;
                    }
                    if (!write(i, j, o)) continue;
                    lightened = true;
                    s0 += r.surfaces_before;
                    s1 += r.surfaces_after;
                    w0 = std::max(w0, r.work_before);
                    w1 = std::max(w1, r.work_after);
                    pieces += r.pieces;
                    moved = std::max(moved, r.moved);
                }
            if (lightened) {
                char cmv[32];
                snprintf(cmv, sizeof cmv, "%.0f cm", moved * 304.8);   // (a world unit is 10 feet)
                std::string what = std::to_string(s0) + " surfaces now " + std::to_string(s1) +
                                   (pieces ? " in " + std::to_string(pieces) + " pieces" : " in a leaner BSP") + ", " + times(w0) + " now " +
                                   times(w1) + (pieces ? ", within " + std::string(cmv) + " of before" : "");
                done.push_back(name + " (" + what + ")");
                how += std::string(how.empty() ? "" : " ") + "Made again for bipeds: " + std::to_string(s0) + " surfaces now " +
                       std::to_string(s1) + (pieces ? " in " + std::to_string(pieces) + " pieces" : " (a leaner BSP)") +
                       "; walking into it costs " + times(w0) + " Bungie's densest scenery, now " + times(w1) +
                       (pieces ? "; within " + std::string(cmv) + " of before" : "") + ".";
            }
        }
        if (!changed) continue;
        fs::path dst = editable_owner(job, cm);
        if (dst.empty() || !ed.save(dst / from_u8(cm))) continue;
        // for the LODs window: drawn as made and as converted, in the model's rest pose
        Job::CollisionChange c;
        c.model = use.models.empty() ? cm : *use.models.begin();
        c.name = name;
        c.text = how;
        c.kind = use.placed ? 's' : 'v';
        coll::Rig rig;
        {
            fs::path mo = use.models.empty() ? fs::path() : job.owner(*use.models.begin());
            tagfile::Tag mt;
            if (!mo.empty() && mt.load(mo / from_u8(*use.models.begin()))) coll::read_rig(mt.root(), rig);
        }
        c.made = collision_drawing(as_made, rig);
        c.now = collision_drawing(nodes, rig);
        job.collision_changes.push_back(std::move(c));
    }
    auto join = [](const std::vector<std::string> &v) {
        std::string list;
        for (size_t i = 0; i < v.size(); i++) list += (i ? "; " : "") + v[i];
        return list;
    };
    if (!fitted.empty())
        job.note("[collision] gave " + std::to_string(fitted.size()) + " model" + (fitted.size() == 1 ? "" : "s") +
                 " Bungie's own collision where it sits on them as well as on Bungie's vehicle: " + join(fitted));
    if (!done.empty())
        job.note("[collision] made " + std::to_string(done.size()) + " dense scenery collision model" + (done.size() == 1 ? "" : "s") +
                 " simpler, as Bungie made theirs (a biped's collision work next to it, as a multiple of Bungie's densest scenery): " + join(done));
    if (!left.empty()) job.note("[collision] left as made: " + join(left));
}

// The map's own first-person arms (use_map_first_person_hands), if the globals the build uses have them: the
// model's tag path, or "" (the Xbox's own, or none).
static std::string map_first_person_hands(Job &job) {
    const std::string g = "globals/globals.globals";
    fs::path gd = job.owner(g);
    if (gd.empty() || job.overlay.empty()) return "";
    std::string v;
    run_process({u8(tool(job.cfg, "invader-edit")), "-t", u8(gd), g, "-G", "first_person_interface[0].first_person_hands"}, v);
    v = norm_rel(v);
    if (v.empty()) return "";
    if (fs::path(from_u8(v)).extension().empty()) v += ".model";
    fs::path own = job.owner(v);
    return own.empty() || own == job.overlay ? "" : v;
}

// The scenario's tags, and the map's own first-person arms' (the globals' only other content of the map's own: the
// passes below treat them as the weapons' first-person models are, their shaders and textures included).
static std::vector<std::string> content_dependencies(Job &job) {
    auto deps = tag_dependencies(job, job.scenario + ".scenario", true);
    std::string hands = map_first_person_hands(job);
    if (!hands.empty()) {
        std::set<std::string> have(deps.begin(), deps.end());
        std::vector<std::string> more = tag_dependencies(job, hands, true);
        more.push_back(hands);
        for (auto &d : more)
            if (have.insert(d).second) deps.push_back(d);
    }
    return deps;
}

void xbox_content_passes(Job &job) {
    auto deps = content_dependencies(job);
    flatten_layered_shaders(job, deps);
    if (!job.cfg.level_shaders_only) reorder_multipurpose_maps(job, deps);   // (before textures are capped)
    if (!job.cfg.level_shaders_only) mcc_flags_for_xbox(job, deps);
    if (!job.cfg.level_shaders_only) xbox_sky(job);   // (before textures are capped: the ones it makes are capped too)
    deps = content_dependencies(job); // flattened layers (and layers drawn together) drop out
    cap_effects(job, deps);
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
    if (!job.cfg.level_shaders_only) {
        xbox_materials(job, deps);
        xbox_scenery_sizes(job, deps);   // (before the levels of detail, xbox_scenery after them)
        xbox_lods(job, deps);
        xbox_animations(job, deps);
        xbox_collision(job, deps);
    }
    xbox_lights(job, deps);
    xbox_portals(job);
    xbox_reflections(job);
    if (!job.cfg.level_shaders_only) {   // (after the level is measured: the window shows the fog as the map has it)
        xbox_fog(job);
        xbox_scenery(job, deps);
        xbox_weather(job);
        xbox_props(job);
    }
}

// A camera tour for Halo 1 Unlimited's perf log (its README_OpenCE_test_34: perf\tours\<map>.txt): the view flies to
// each stop, waits, measures a few seconds and writes a T row, so every run of a map measures the same views. Stops:
// up to 4 spawns (spread out, each the farthest from those already picked), the level's 6 busiest views from where
// players stand (the most level triangles drawn, at least 6 units apart), 2 typical ones (around the median), and a
// look around from the middle of where players stand. Positions at eye height (0.62 above the floor, as the views
// are); yaw in degrees, 0 = +x, counterclockwise, as the log's.
static std::string make_perf_tour(const lshade::Analysis &a, const lshade::Preview &p, const tagfile::Elem &scen, const std::string &title) {
    struct Stop { double x = 0, y = 0, z = 0, yaw = 0, pitch = 0; bool around = false; std::string label; };
    auto deg = [](double rad) {
        double d = std::fmod(rad * 180.0 / 3.14159265358979, 360.0);
        return d < 0 ? d + 360 : d;
    };
    auto dist = [](const Stop &s, const Stop &t) { return std::sqrt((s.x - t.x) * (s.x - t.x) + (s.y - t.y) * (s.y - t.y) + (s.z - t.z) * (s.z - t.z)); };
    std::vector<Stop> stops;
    // Spawns
    std::vector<Stop> spawns;
    tagfile::Block sl = scen.block("player starting locations");
    for (size_t i = 0; i < sl.size(); i++) {
        Stop s;
        s.x = sl[i].num("position", 0);
        s.y = sl[i].num("position", 1);
        s.z = sl[i].num("position", 2) + 0.62;
        s.yaw = deg(sl[i].num("facing"));
        spawns.push_back(s);
    }
    std::vector<Stop> picked;
    while (picked.size() < 4 && picked.size() < spawns.size()) {
        const Stop *best = nullptr;
        double farthest = -1;
        for (auto &s : spawns) {
            double d = picked.empty() ? 0 : 1e9;
            for (auto &q : picked) d = std::min(d, dist(s, q));
            if (picked.empty() ? !best : d > farthest) { farthest = d; best = &s; }
        }
        if (!best || (!picked.empty() && farthest < 3)) break;
        picked.push_back(*best);
        picked.back().label = "spawn " + std::to_string(picked.size());
    }
    stops.insert(stops.end(), picked.begin(), picked.end());
    // Views from where players stand, by level triangles drawn
    size_t S = a.shaders.size(), n = std::min((size_t)a.views, p.views.size());
    std::vector<std::pair<double, size_t>> score;
    for (size_t v = 0; v < n; v++) {
        double t = v < a.other.size() ? a.other[v] : 0;
        for (size_t s = 0; s < S && v * S + s < a.drawn.size(); s++) t += a.drawn[v * S + s];
        score.push_back({t, v});
    }
    auto view_stop = [&](size_t v) {
        Stop s;
        s.x = p.views[v].eye[0];
        s.y = p.views[v].eye[1];
        s.z = p.views[v].eye[2];
        s.yaw = deg(p.views[v].yaw);
        s.pitch = -4.6;   // (as the level window's views)
        return s;
    };
    std::vector<Stop> views;
    auto apart = [&](const Stop &s) {
        for (auto &q : views) if (dist(s, q) < 6) return false;
        return true;
    };
    std::sort(score.rbegin(), score.rend());
    for (auto &[t, v] : score) {
        if (views.size() >= 6) break;
        Stop s = view_stop(v);
        if (!apart(s)) continue;
        s.label = "busiest view " + std::to_string(views.size() + 1);
        views.push_back(s);
    }
    if (!score.empty()) {
        size_t mid = score.size() / 2, typical = 0;
        for (size_t k = 0; k < score.size() && typical < 2; k++) {
            size_t i = (k % 2) ? mid + (k + 1) / 2 : mid - std::min(mid, k / 2);   // (outward from the median)
            if (i >= score.size()) continue;
            Stop s = view_stop(score[i].second);
            if (!apart(s)) continue;
            s.label = "typical view " + std::to_string(++typical);
            views.push_back(s);
        }
        // a look around from the middle of where players stand
        double cx = 0, cy = 0, cz = 0;
        for (size_t v = 0; v < n; v++) { cx += p.views[v].eye[0]; cy += p.views[v].eye[1]; cz += p.views[v].eye[2]; }
        cx /= n; cy /= n; cz /= n;
        size_t best = 0;
        double bd = 1e18;
        for (size_t v = 0; v < n; v++) {
            double d = (p.views[v].eye[0] - cx) * (p.views[v].eye[0] - cx) + (p.views[v].eye[1] - cy) * (p.views[v].eye[1] - cy) +
                       (p.views[v].eye[2] - cz) * (p.views[v].eye[2] - cz);
            if (d < bd) { bd = d; best = v; }
        }
        Stop s = view_stop(best);
        s.around = true;
        s.pitch = 0;
        s.label = "middle, all around";
        views.push_back(s);
    }
    stops.insert(stops.end(), views.begin(), views.end());
    if (stops.empty()) return "";
    std::string out = "# Halo 1 Unlimited camera tour v1 (README_OpenCE_test_34.txt) - " + title + ", written by Halo Map Porter\r\n"
                      "# Its spawns, the level's busiest and typical views from where players stand, and a look around.\r\n"
                      "# The tour moves the view, not your player: tick_ms and the obj_* columns follow where your player stands.\r\n"
                      "start 5\r\nsettle 2\r\nhold 3\r\n";
    char line[160];
    for (auto &s : stops) {
        std::string label = s.label.substr(0, 31);
        if (s.around) snprintf(line, sizeof line, "%.2f %.2f %.2f * %.1f %s\r\n", s.x, s.y, s.z, s.pitch, label.c_str());
        else snprintf(line, sizeof line, "%.2f %.2f %.2f %.1f %.1f %s\r\n", s.x, s.y, s.z, s.yaw, s.pitch, label.c_str());
        out += line;
    }
    return out;
}

// The level's reflections (levelshaders.hpp): the Xbox draws a reflecting surface once or twice more than any other,
// so the ones that show least for what they cost lose their reflection while the level costs more than stock ones
// (or this map's own picks from the level shaders window). The measurement is kept for that window either way.
// lshade::analyze, kept (results kept between conversions): what it measured (its .lsh file) and the tags it read, by
// content. The same level read from the same tags (the Level window measured it, then the conversion does) is
// measured once.
static bool analyze_level_kept(Job &job, const fs::path &bsp, const fs::path &scenario, const lshade::Find &find,
                               lshade::Analysis &a, lshade::Preview &p, std::string *why) {
    const std::string key = result_key("level shaders 1", read_all(bsp) + "\n" + read_all(scenario));
    auto content_key = [&](const fs::path &f) {
        std::error_code ec;
        return f.empty() || !fs::is_regular_file(f, ec) ? std::string("-") : result_key("", read_all(f));
    };
    fs::path tmp = job.work / "level_kept.lsh";
    std::string b, note;
    if (load_blob(job.cfg, key, b, note)) {
        size_t pos = 0;
        std::string t, h, n, lsh;
        bool ok = get_text(b, pos, t) && t == "HMP level 1" && get_text(b, pos, n);
        for (int k = ok ? std::atoi(n.c_str()) : 0; ok && k > 0; k--)
            ok = get_text(b, pos, t) && get_text(b, pos, h) && content_key(find(t)) == h;
        if (ok && get_text(b, pos, lsh) && write_all(tmp, lsh) && lshade::load(a, p, tmp)) return true;
    }
    std::map<std::string, std::string> reads;
    lshade::Find rfind = [&](const std::string &t) {
        fs::path f = find(t);
        if (!reads.count(t)) reads[t] = content_key(f);
        return f;
    };
    if (!lshade::analyze(bsp, scenario, rfind, a, p, why)) return false;
    if (lshade::save(a, p, tmp)) {
        std::string rec;
        put_text(rec, "HMP level 1");
        put_text(rec, std::to_string(reads.size()));
        for (auto &[t, h] : reads) { put_text(rec, t); put_text(rec, h); }
        put_text(rec, read_all(tmp));
        store_blob(job.cfg, key, rec, "");
    }
    return true;
}

// A build from prepared tags (job.update): the showcase flythrough and perf tour a conversion makes while it measures
// the level (xbox_reflections, which isn't done again), made from the level as the prepared tags have it, so the map
// folder gets them as from a conversion. Your own showcase needs no measuring.
static void level_extras_for_update(Job &job) {
    const Config &cfg = job.cfg;
    bool show = cfg.xbox_showcase, tour = cfg.write_perf_tour;
    if (show && !cfg.xbox_showcase_text.empty()) {
        showcase::Showcase own;
        if (showcase::read(cfg.xbox_showcase_text, own) && !own.shots.empty()) {
            job.showcase = cfg.xbox_showcase_text;
            char b[120];
            snprintf(b, sizeof b, "%zu shots, %.0f seconds", own.shots.size(), own.length());
            job.showcase_note = std::string("your own from the Level window (") + b + ")";
            show = false;
        }
    }
    if (!show && !tour) return;
    std::string scen = job.scenario + ".scenario";
    fs::path sdir = job.owner(scen);
    tagfile::Tag stag;
    if (sdir.empty() || !stag.load(sdir / from_u8(scen))) return;
    lshade::Find find = [&job](const std::string &t) {
        fs::path o = job.owner(t);
        return o.empty() ? fs::path() : o / from_u8(t);
    };
    tagfile::Block bsps = stag.root().block("structure bsps");
    std::string b = bsps.size() ? bsps[0].ref("structure bsp") : std::string();   // (the first level: the one a match loads)
    fs::path own = b.empty() ? fs::path() : job.owner(b);
    if (own.empty()) return;
    lshade::Analysis a;
    lshade::Preview p;
    std::string why;
    if (!analyze_level_kept(job, own / from_u8(b), sdir / from_u8(scen), find, a, p, &why)) return;
    a.map_name = fs::path(from_u8(job.scenario)).filename().u8string() + (bsps.size() > 1 ? " - " + fs::path(from_u8(b)).stem().u8string() : "");
    if (tour) job.perf_tour = make_perf_tour(a, p, stag.root(), a.map_name);
    if (show) {
        std::string report;
        showcase::Showcase made = showcase::generate(p, &report);
        if (!made.shots.empty())
            job.showcase = showcase::write(made, fs::path(from_u8(job.scenario)).filename().u8string(), "generated from the level");
        job.showcase_note = "made from the level: " + report;
    }
}

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
        if (!analyze_level_kept(job, own / from_u8(b), sdir / from_u8(scen), find, a, p, &why)) {
            job.log("  [reflections] the level" + name + " couldn't be measured: it " + why);
            continue;
        }
        a.map_name = fs::path(from_u8(job.scenario)).filename().u8string() + (bsps.size() > 1 ? " - " + fs::path(from_u8(b)).stem().u8string() : "");
        lshade::suggest(a);
        if (job.shaders_file.empty()) {   // (the window shows the first level)
            fs::path f = cfg.cache_dir / "levels" / (fs::path(from_u8(job.scenario)).filename().u8string() + ".lsh");
            if (lshade::save(a, p, f)) job.shaders_file = f;
        }
        if (cfg.write_perf_tour && !cfg.level_shaders_only && job.perf_tour.empty())   // (the first level: the one a match loads)
            job.perf_tour = make_perf_tour(a, p, stag.root(), a.map_name);
        if (cfg.xbox_showcase && !cfg.level_shaders_only && job.showcase.empty() && !job.update) {
            showcase::Showcase own;
            std::string why;
            if (!cfg.xbox_showcase_text.empty() && showcase::read(cfg.xbox_showcase_text, own, &why) && !own.shots.empty()) {
                job.showcase = cfg.xbox_showcase_text;
                char b[120];
                snprintf(b, sizeof b, "%zu shots, %.0f seconds", own.shots.size(), own.length());
                job.showcase_note = std::string("your own from the Level window (") + b + ")";
            } else {
                std::string report;
                showcase::Showcase made = showcase::generate(p, &report);
                if (!made.shots.empty())
                    job.showcase = showcase::write(made, fs::path(from_u8(job.scenario)).filename().u8string(), "generated from the level");
                job.showcase_note = "made from the level: " + report;
            }
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
// that; one an effect makes is dynamic whatever its flag, so it's taken out of the effect). A light the map doesn't
// place (a weapon's or projectile's effects: where it goes off isn't known) is kept to Bungie's biggest radius (the
// Scorpion shell's explosion, 15). The rest of the lights on the map's objects, always on (Deathtrap's warthogs'
// headlights: about 3.4 ms of a frame with two in view, and 1.2 ms setting the lights up), are the level window's
// choice (Config::xbox_object_lights).
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
    // The effects that make each light: an effect's lights are dynamic whatever their flag says (effects.c makes them
    // with light_new_unattached), so one goes by taking it out of those effects (a radius of 0 is built as 1).
    std::map<std::string, std::set<std::string>> effect_made;
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
                        if (std::string l = parts[k].ref("type"); has_suffix(l, ".light")) {
                            out.push_back(l);
                            effect_made[l].insert(a);
                        }
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
    // Takes a light out of the map's own effects that make it (their events' parts). False if one can't be changed.
    auto drop_from_effects = [&](const std::string &light) {
        auto em = effect_made.find(light);
        if (em == effect_made.end()) return true;
        std::string ref = light.substr(0, light.size() - 6);   // (no extension, '\\' separators)
        std::replace(ref.begin(), ref.end(), '/', '\\');
        bool all = true;
        for (auto &fx : em->second) {
            fs::path ed = editable_owner(job, fx);
            tagfile::EditTag et;
            if (ed.empty() || !et.load(ed / from_u8(fx))) { all = false; continue; }
            if (auto *ev = tagfile::block(et.root(), "events"))
                for (auto &e : *ev)
                    if (auto *parts = tagfile::block(e, "parts"))
                        parts->erase(std::remove_if(parts->begin(), parts->end(), [&](tagfile::TagNode &pn) {
                                         std::string *r = tagfile::ref_path(pn, "type");
                                         auto low = [](std::string x) {
                                             for (auto &c : x) c = (char)tolower((unsigned char)c);
                                             return x;
                                         };
                                         return r && low(*r) == low(ref);
                                     }), parts->end());
            all = et.save(ed / from_u8(fx)) && all;
        }
        return all;
    };
    std::string removed, shrunk, object_lights;
    int n_removed = 0, n_shrunk = 0, n_flares = 0, n_object_lights = 0;
    for (auto &dep : deps) {
        if (!has_suffix(dep, ".light")) continue;
        fs::path own = job.owner(dep);
        if (own.empty() || own == job.overlay) continue;   // (Bungie's are as Bungie made them)
        tagfile::Tag lt;
        if (!lt.load(own / from_u8(dep))) continue;
        tagfile::Elem l = lt.root();
        long long flags = (long long)l.num("flags");
        if (!(flags & 1) && !effect_made.count(dep)) continue;   // (not dynamic: only a lens flare, or for the lightmaps)
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
            if (most <= kMostOfLevel) {
                // Always on where the map has them: the level window's choice (Config::xbox_object_lights).
                const int mode = job.cfg.xbox_object_lights;
                if (mode != 1 && mode != 2) continue;
                if (mode == 1 && (flags & 2)) continue;   // (no specular already)
                fs::path ed = editable_owner(job, dep);
                if (ed.empty()) continue;
                tagfile::EditTag et;
                if (!et.load(ed / from_u8(dep))) continue;
                bool ok = mode == 1 ? tagfile::set_num(et.root(), "flags", (double)(flags | 2))
                                    : tagfile::set_num(et.root(), "flags", (double)(flags & ~1LL));
                if (!ok || !et.save(ed / from_u8(dep)) || (mode == 2 && !drop_from_effects(dep))) continue;
                n_object_lights++;
                object_lights += (object_lights.empty() ? "" : ", ") + name + " (" + std::to_string(it->second.size()) + " placed)";
                continue;
            }
            fs::path ed = editable_owner(job, dep);
            if (ed.empty()) continue;
            tagfile::EditTag et;
            if (!et.load(ed / from_u8(dep)) || !tagfile::set_num(et.root(), "flags", (double)(flags & ~1LL)) ||
                !et.save(ed / from_u8(dep)) || !drop_from_effects(dep))
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
    if (n_object_lights)
        job.note("[lights] " + std::string(job.cfg.xbox_object_lights == 1 ? "no specular for " : "turned off ") +
                 std::to_string(n_object_lights) + (n_object_lights == 1 ? " dynamic light" : " dynamic lights") +
                 " on the map's objects (each one in view had the Xbox draw what's around it again, " +
                 (job.cfg.xbox_object_lights == 1 ? "for its diffuse light and again for its shine; the shine pass is gone" :
                                                    "twice; a lens flare stays") + "): " + object_lights);
}

// ---- Materials whose alpha doesn't match how they use it
// An alpha-tested surface draws a pixel on the Xbox only where its mask's alpha is above half (rasterizer_xbox: alpha
// reference 0x7F, "greater"): a shader_model's base map unless "not alpha tested" or "alpha blended decal"; a
// shader_environment's bump map when "alpha tested" (its lightmap pass tests the bump map's alpha, as PC and MCC do;
// with "bump map is specular mask" there's nothing to test, so it's left alone). A blended one (shader_transparent_*)
// is drawn with the see-through surfaces, sorted, not hiding what's behind it. Fixed, as the map looks elsewhere:
// - alpha-tested with nothing see-through in the texture (no alpha, or all of it above half): the test never cuts a
//   pixel, so it's turned off (one less test each time it's drawn, and in the shadows);
// - alpha-tested where almost nothing reaches half (1% or less) and next to nothing is clear: the Xbox would leave the
//   surface out, while Halo PC and MCC draw it (MCC BC7 textures whose alpha stays between 6% and 55%): drawn solid
//   instead (alpha test off; any fully clear specks are filled in).
// Noted only (they look as made either way): alpha-tested ones that hardly draw but are mostly clear, and blended ones
// whose first map has nothing see-through (could be solid) or is a cut-out (could be alpha-tested, which is cheaper).
struct AlphaShare {
    bool ok = false;
    long long px = 0, pass = 0, clear = 0, soft = 0;   // pass: above half; clear: 8/255 or less; soft: between, not clear
};

static AlphaShare texture_alpha(const fs::path &file) {
    AlphaShare a;
    tagfile::Tag t;
    if (!t.load(file)) return a;
    tagfile::Elem r = t.root();
    std::string pix = r.data("processed pixel data");
    tagfile::Block bd = r.block("bitmap data");
    for (size_t i = 0; i < bd.size(); i++) {
        tagfile::Elem b = bd[i];
        if (b.integer("type") != 0) continue;   // (2D textures)
        int w = (int)b.integer("width"), h = (int)b.integer("height"), fmt = (int)b.integer("format");
        size_t off = (size_t)b.integer("pixel data offset"), n = mip_bytes(w, h, fmt);
        if (!n || w <= 0 || h <= 0 || off + n > pix.size()) return AlphaShare();
        std::vector<unsigned char> rgba;
        if (!decode_level(pix.substr(off, n), w, h, fmt, rgba)) return AlphaShare();
        for (size_t k = 0; k < (size_t)w * h; k++) {
            unsigned al = fmt == 0 || fmt == 2 ? rgba[k * 4] : fmt == 3 ? rgba[k * 4 + 1] : fmt == 1 ? 255u : rgba[k * 4 + 3];
            a.px++;
            if (al > 127) a.pass++;
            if (al <= 8) a.clear++;
            else if (al < 247) a.soft++;
        }
        a.ok = true;
    }
    return a;
}

void xbox_materials(Job &job, const std::vector<std::string> &deps) {
    if (job.analysis) return;
    std::map<std::string, AlphaShare> seen;
    auto alpha_of = [&](const std::string &bm) {
        auto it = seen.find(bm);
        if (it != seen.end()) return it->second;
        fs::path own = job.owner(bm);
        return seen[bm] = own.empty() ? AlphaShare() : texture_alpha(own / from_u8(bm));
    };
    auto pct = [](long long a, long long b) {
        char t[16];
        snprintf(t, sizeof t, "%.0f%%", b ? 100.0 * a / b : 0.0);
        if (a && b && 100.0 * a / b < 0.5) snprintf(t, sizeof t, "under 1%%");
        return std::string(t);
    };
    std::vector<std::string> untested, solid, hardly, blend_none, blend_cut;
    for (auto &dep : deps) {
        const bool env = has_suffix(dep, ".shader_environment"), model = has_suffix(dep, ".shader_model");
        const bool chicago = has_suffix(dep, ".shader_transparent_chicago"), generic = has_suffix(dep, ".shader_transparent_generic"),
                   extended = has_suffix(dep, ".shader_transparent_chicago_extended");
        if (!env && !model && !chicago && !generic && !extended) continue;
        fs::path own = job.owner(dep);
        if (own.empty() || own == job.overlay) continue;   // (Bungie's are as Bungie made them)
        tagfile::Tag st;
        if (!st.load(own / from_u8(dep))) continue;
        tagfile::Elem r = st.root();
        const std::string name = fs::path(from_u8(dep)).stem().u8string();
        if (env || model) {
            const char *field = env ? "shader environment flags" : "shader model flags";
            const int f = (int)r.integer(field);
            const bool tested = env ? (f & 1) != 0 && !(f & 2) : !(f & 4) && !(f & 8);
            std::string bm = r.ref(env ? "bump map" : "base map");
            if (!tested || bm.empty()) continue;
            AlphaShare a = alpha_of(bm);
            if (!a.ok || !a.px) continue;
            const bool cuts_nothing = a.pass == a.px;
            const bool hardly_draws = a.pass * 100 <= a.px;
            if (!cuts_nothing && !(hardly_draws && a.clear * 10 <= a.px)) {
                if (hardly_draws) hardly.push_back(name + " (" + pct(a.pass, a.px) + " drawn, " + pct(a.clear, a.px) + " clear)");
                continue;
            }
            fs::path ed = editable_owner(job, dep);
            if (ed.empty()) continue;
            tagfile::EditTag et;
            if (!et.load(ed / from_u8(dep)) || !tagfile::set_num(et.root(), field, (double)(env ? (f & ~1) : (f | 4))) ||
                !et.save(ed / from_u8(dep)))
                continue;
            if (cuts_nothing) untested.push_back(name);
            else solid.push_back(name + " (" + pct(a.pass, a.px) + " would draw" + (a.clear ? ", " + pct(a.clear, a.px) + " clear" : "") + ")");
            continue;
        }
        if ((int)r.integer("framebuffer blend function") != 0) continue;   // (alpha blend; add, multiply... don't use alpha so)
        tagfile::Block maps = r.block(extended ? "maps 4 stage" : "maps");
        if (extended && !maps.size()) maps = r.block("maps 2 stage");
        std::string bm = maps.size() ? maps[0].ref("map") : "";
        if (bm.empty()) continue;
        AlphaShare a = alpha_of(bm);
        if (!a.ok || !a.px) continue;
        if (a.pass == a.px && !a.soft) blend_none.push_back(name);
        else if (a.clear && a.soft * 25 <= a.px) blend_cut.push_back(name);
    }
    auto list = [](const std::vector<std::string> &v) {
        std::string s;
        for (size_t i = 0; i < v.size() && i < 12; i++) s += (i ? ", " : "") + v[i];
        if (v.size() > 12) s += " and " + std::to_string(v.size() - 12) + " more";
        return s;
    };
    auto count = [](size_t n, const char *one, const char *many) { return std::to_string(n) + " " + (n == 1 ? one : many); };
    if (!solid.empty())
        job.note("[materials] " + count(solid.size(), "alpha-tested material", "alpha-tested materials") +
                 " would draw almost nothing on the Xbox (it draws a pixel only where the alpha is above half; Halo PC and MCC "
                 "draw them): drawn solid instead, alpha test off: " + list(solid));
    if (!untested.empty())
        job.note("[materials] alpha test off for " + count(untested.size(), "material", "materials") +
                 " whose textures have nothing see-through (it never cut a pixel; one less test each time they're drawn, "
                 "shadows too): " + list(untested));
    if (!hardly.empty())
        job.note("[materials] " + count(hardly.size(), "alpha-tested material", "alpha-tested materials") +
                 " hardly draw on the Xbox, left as made (much of their texture is clear): " + list(hardly));
    if (!blend_none.empty())
        job.note("[materials] " + count(blend_none.size(), "blended material has", "blended materials have") +
                 (blend_none.size() == 1 ? " nothing see-through in its first map" : " nothing see-through in their first map") +
                 " (drawn with the see-through surfaces, sorted, though " + (blend_none.size() == 1 ? "it" : "they") +
                 " could be solid; left as made): " + list(blend_none));
    if (!blend_cut.empty())
        job.note("[materials] " + count(blend_cut.size(), "blended material's", "blended materials'") +
                 " first map is a cut-out (fully clear or solid; alpha-testing would be cheaper; left as made): " + list(blend_cut));
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

// Water without a fog plane (portals::water_volumes): the clusters inside the scenario's "water..." trigger volumes
// are made underwater, so water at more than one height (a river between two lakes) works. Every build: compiling
// the level again in Tool resets its clusters' fog.
void water_volumes(Job &job) {
    if (job.analysis || job.cfg.level_shaders_only) return;
    std::string scen = job.scenario + ".scenario";
    fs::path sdir = job.owner(scen);
    tagfile::Tag stag;
    if (sdir.empty() || !stag.load(sdir / from_u8(scen))) return;
    auto is_water = [&job](const std::string &fog) {
        fs::path own = job.owner(fog);
        if (own.empty()) return false;
        std::ifstream f(own / from_u8(fog), std::ios::binary);
        unsigned char h[68];
        if (!f.read((char *)h, sizeof h)) return false;
        if (memcmp(h + 36, "fog ", 4) != 0) return false;
        return (h[67] & 1) != 0;   // (the flags, big endian: "is water")
    };
    auto fmt = [](double v) {
        char b[32];
        snprintf(b, sizeof b, "%.1f", v);
        return std::string(b);
    };
    auto where = [&](const portals::WaterCluster &w) {
        return "around (" + fmt(w.rest[0]) + ", " + fmt(w.rest[1]) + ", " + fmt(w.rest[2]) + "), heights " + fmt(w.rest_lo) +
               " to " + fmt(w.rest_hi);
    };
    tagfile::Block bsps = stag.root().block("structure bsps");
    std::set<std::string> done, touched, all;
    bool any = false;
    for (size_t i = 0; i < bsps.size(); i++) {
        std::string b = bsps[i].ref("structure bsp");
        if (b.empty() || !done.insert(b).second) continue;
        fs::path own = job.owner(b);
        if (own.empty()) continue;
        std::string name = bsps.size() > 1 ? " (" + fs::path(from_u8(b)).stem().u8string() + ")" : "";
        portals::WaterResult wr = portals::water_volumes(own / from_u8(b), sdir / from_u8(scen), is_water, false);
        if (!wr.any) return;   // (no water volumes)
        if (!any) job.log("Making the water volumes' clusters underwater...");
        any = true;
        for (auto &v : wr.untouched) all.insert(v);
        if (!wr.why.empty()) {
            job.note("[water] the level" + name + "'s water volumes weren't used: it " + wr.why);
            continue;
        }
        if (wr.needs) {
            fs::path ed = editable_owner(job, b, true);
            if (ed.empty()) {
                job.note("[water] the level" + name + " can't be changed here (a stock level), so its water volumes weren't used");
                continue;
            }
            wr = portals::water_volumes(ed / from_u8(b), sdir / from_u8(scen), is_water, true);
            if (!wr.why.empty()) {
                job.note("[water] the level" + name + "'s water volumes weren't used: it " + wr.why);
                continue;
            }
        }
        for (auto &v : wr.volumes) {
            touched.insert(v.name);
            std::string l = "[water] " + v.name + name + ": ";
            if (v.water.empty()) {
                l += "no cluster is mostly inside it, so none is underwater.";
            } else {
                l += v.water.size() == 1 ? "cluster " : "clusters ";
                for (size_t k = 0; k < v.water.size(); k++)
                    l += (k ? (k + 1 == v.water.size() ? " and " : ", ") : "") + std::to_string(v.water[k].cluster);
                l += std::string(v.water.size() == 1 ? " is" : " are") + " underwater (the fog " + v.fog + ", no fog plane)" +
                     (wr.region_added ? "; a fog region was added for it" : "") + ".";
            }
            for (auto &w : v.partly) {
                int pct = (int)std::lround(w.inside * 100);
                if (w.inside >= 0.5)
                    l += " Cluster " + std::to_string(w.cluster) + " is only " + std::to_string(pct) + "% inside, so its other " +
                         std::to_string(100 - pct) + "% (" + where(w) + ") is underwater too.";
                else
                    l += " Cluster " + std::to_string(w.cluster) + " is " + std::to_string(pct) +
                         "% inside, so it stays as it was" + (w.was_water ? " (water already)" : " (dry)") + ": the water stops at it.";
            }
            if (!v.partly.empty())
                l += " A cluster is all water or none: put a portal where the volume's edge crosses one (the water's surface, "
                     "where the river ends), or move the volume's edge onto a portal.";
            job.note(l);
        }
    }
    for (auto &v : all)
        if (!touched.count(v)) job.note("[water] " + v + " has no open space of the level inside it (it's inside walls or outside the level), so it does nothing");
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
            if (!m.empty() && !object_name.count(m)) object_name[m] = fs::path(from_u8(dep)).stem().u8string() + " (first person)";
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
static std::map<std::string, int> object_counts(Job &job, const tagfile::Elem &s);

// How many of each model are on the map (its objects placed, or at weapon/item spawn points), and the biggest bounding
// radius of the objects that draw it.
static std::map<std::string, int> models_on_map(Job &job, std::map<std::string, double> *radius = nullptr) {
    std::map<std::string, int> per_model;
    std::string scen = job.scenario + ".scenario";
    fs::path sdir = job.owner(scen);
    tagfile::Tag st;
    if (sdir.empty() || !st.load(sdir / from_u8(scen))) return per_model;
    for (auto &[o, n] : object_counts(job, st.root())) {
        fs::path own = job.owner(o);
        tagfile::Tag ot;
        if (own.empty() || !ot.load(own / from_u8(o))) continue;
        std::string m = ot.root().ref("model");
        if (m.empty()) continue;
        per_model[m] += n;
        if (radius) (*radius)[m] = std::max((*radius)[m], ot.root().num("bounding radius"));
    }
    return per_model;
}

// How a model's shader draws (lod.hpp ShaderLook), by its tag path; a model's reference may leave out which kind of
// shader it is, and each kind is tried. Cut out: drawn alpha-tested (as find_alpha_tested has it) with a texture that
// has see-through parts (a swizzled one, which this can't read, counts as one).
static lod::ShaderLook shader_look(Job &job, const std::string &tag) {
    lod::ShaderLook l;
    static const char *const kinds[] = {".shader_model", ".shader_environment", ".shader_transparent_chicago",
                                        ".shader_transparent_chicago_extended", ".shader_transparent_generic",
                                        ".shader_transparent_glass", ".shader_transparent_meter", ".shader_transparent_plasma",
                                        ".shader_transparent_water"};
    std::string path = tag;
    fs::path own = job.owner(path);
    if (own.empty() || !fs::is_regular_file(own / from_u8(path)))
        for (const char *k : kinds) {
            own = job.owner(tag + k);
            if (!own.empty()) { path = tag + k; break; }
        }
    if (own.empty()) return l;
    std::string ext = fs::path(from_u8(path)).extension().u8string();
    if (ext == ".shader_transparent_glass" || ext == ".shader_transparent_meter" || ext == ".shader_transparent_plasma" ||
        ext == ".shader_transparent_water") {   // (blended, both sides: nothing more to read)
        l.known = l.two_sided = true;
        l.kind = lod::ShaderLook::kBlended;
        return l;
    }
    tagfile::Tag t;
    if (!t.load(own / from_u8(path))) return l;
    l.known = true;
    tagfile::Elem r = t.root();
    // (and where: l.mask, a grid over the texture, up to 64 cells a side, a cell set when any pixel in it is clear;
    // `us`, `vs`: how many times the texture repeats across the model)
    auto see_through_map = [&](std::string bm, double us = 1, double vs = 1) {
        if (bm.empty()) return false;
        if (fs::path(from_u8(bm)).extension() != ".bitmap") bm += ".bitmap";
        fs::path bo = job.owner(bm);
        tagfile::Tag b;
        if (bo.empty() || !b.load(bo / from_u8(bm))) return true;   // (unreadable: as if it had)
        tagfile::Block bd = b.root().block("bitmap data");
        if (!bd.size()) return true;
        int w = (int)bd[0].integer("width"), h = (int)bd[0].integer("height"), fmt = (int)bd[0].integer("format");
        if ((long long)bd[0].integer("flags") & 8) return true;   // (swizzled)
        std::string pix = b.root().data("processed pixel data");
        size_t off = (size_t)bd[0].integer("pixel data offset"), n = mip_bytes(w, h, fmt);
        std::vector<unsigned char> rgba;
        if (!n || off + n > pix.size() || !decode_level(pix.substr(off, n), w, h, fmt, rgba)) return true;
        if (!see_through(rgba)) return false;
        auto m = std::make_shared<lod::ClearMask>();
        m->w = std::min(64, std::max(1, w));
        m->h = std::min(64, std::max(1, h));
        m->u_scale = us == 0 ? 1 : us;
        m->v_scale = vs == 0 ? 1 : vs;
        m->clear.assign((size_t)m->w * m->h, 0);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                if (rgba[((size_t)y * w + x) * 4 + 3] < 128) m->clear[(size_t)(y * m->h / h) * m->w + x * m->w / w] = 1;
        l.mask = m;
        return true;
    };
    if (ext == ".shader_model") {
        long long f = (long long)r.num("shader model flags");
        l.two_sided = (f & 2) != 0;
        if (f & 8) l.kind = lod::ShaderLook::kBlended;   // (an alpha-blended decal)
        else if (!(f & 4) && see_through_map(r.ref("base map"), r.num("map u scale"), r.num("map v scale"))) l.kind = lod::ShaderLook::kCutOut;
    } else if (ext == ".shader_environment") {
        long long f = (long long)r.num("shader environment flags");
        if ((f & 1) && !(f & 2) && see_through_map(r.ref("bump map"))) l.kind = lod::ShaderLook::kCutOut;
        l.mask = nullptr;   // (its bump map's own scale: the whole part counts)
    } else {
        l.two_sided = true;
        l.kind = lod::ShaderLook::kBlended;
        const char *flags = ext == ".shader_transparent_chicago" ? "shader transparent chicago flags"
                            : ext == ".shader_transparent_generic" ? "shader transparent generic flags"
                            : ext == ".shader_transparent_chicago_extended" ? "shader transparent chicago extended flags"
                                                                            : nullptr;
        if (flags && ((long long)r.num(flags) & 1)) {   // (alpha tested: cut out, not blended)
            tagfile::Block maps = r.block(ext == ".shader_transparent_chicago_extended" ? "maps 4 stage" : "maps");
            if (!maps.size() && ext == ".shader_transparent_chicago_extended") maps = r.block("maps 2 stage");
            if (maps.size() && see_through_map(maps[0].ref("map"), maps[0].num("map u scale"), maps[0].num("map v scale")))
                l.kind = lod::ShaderLook::kCutOut;
        }
    }
    return l;
}

// A first-person model as it's seen in its weapon's first-person animations (fpview.hpp), kept between conversions
// (by the model's, the animations' and its shaders' content). False with `why` when it can't be measured.
static bool first_person_seen(Job &job, const std::string &model, const std::string &animations, const lod::LookUp &look,
                              lod::Seen &out, std::string &why) {
    fs::path mo = job.owner(model), ao = job.owner(animations);
    if (mo.empty() || ao.empty()) { why = "its first-person animations couldn't be found"; return false; }
    std::string looks;
    {
        tagfile::Tag t;
        if (t.load(mo / from_u8(model))) {
            tagfile::Block sh = t.root().block("shaders");
            for (size_t i = 0; i < sh.size(); i++) {
                lod::ShaderLook l = look(sh[i].ref("shader"));
                looks += std::to_string((int)l.kind) + (l.two_sided ? "t" : "o") + (l.known ? "k" : "u") + ",";
            }
        }
    }
    const std::string key = result_key("first person seen 1\n" + looks, read_all(mo / from_u8(model)) + "\n" + read_all(ao / from_u8(animations)));
    std::string b, note;
    if (load_blob(job.cfg, key, b, note) && b.compare(0, 9, "HMPSEEN1\n") == 0) {   // (kept: geometry, parts, triangles' shares)
        std::istringstream in(b.substr(9));
        std::getline(in, out.about);
        int geos = 0;
        bool ok = (bool)(in >> geos) && geos >= 0 && geos < 1000;
        for (int g = 0; ok && g < geos; g++) {
            int id = 0, parts = 0;
            ok = (bool)(in >> id >> parts) && parts >= 0 && parts < 10000;
            auto &v = out.geometry[id];
            v.resize((size_t)std::max(0, parts));
            for (int p = 0; ok && p < parts; p++) {
                int n = 0;
                ok = (bool)(in >> n) && n >= 0 && n < 1000000;
                v[(size_t)p].resize((size_t)std::max(0, n));
                for (int k = 0; ok && k < n; k++) ok = (bool)(in >> v[(size_t)p][(size_t)k]);
            }
        }
        if (ok) return true;
        out = lod::Seen();
    }
    auto find = [&](const std::string &tag) { fs::path o = job.owner(tag); return o.empty() ? fs::path() : o / from_u8(tag); };
    if (!fpview::measure(model, animations, find, look, out, &why)) return false;
    std::ostringstream o;
    o << "HMPSEEN1\n" << out.about << "\n" << out.geometry.size() << "\n";
    for (auto &[id, parts] : out.geometry) {
        o << id << " " << parts.size() << "\n";
        for (auto &p : parts) {
            o << p.size();
            for (float x : p) o << " " << x;
            o << "\n";
        }
    }
    store_blob(job.cfg, key, o.str(), "");
    return true;
}

// Levels of detail. First, the map's own heavy models (over their kind's budget) get a lighter most detailed version
// (trimmed: Xbox tab option, on by default), first-person weapons and characters included. Then a model drawn in the
// world (not first-person) of the map's own with no lower-detail versions gets them made (lod.hpp; Xbox tab option,
// on by default), kept in the LOD library so another map with the same model reuses them. Characters too: every
// vertex keeps its bones, so they bend as before (lod.cpp, Bones). Then models with lower-detail versions but no
// switch distances get some: they always draw their most detailed level otherwise (the engine draws a level once the model
// is at least that many pixels across, and with every distance 0 that's always the top one). The distances are like
// the stock Xbox models of the kind (Bungie's own, in tag order super high, high, medium, low, super low; super low
// stays 0, so nothing is left out). The map's own models always; Bungie's (36 stock models ship like this, mostly
// trees, rocks and plants) with the Xbox tab's option. Only models an object draws in the world: the engine draws
// first-person models (weapons, hands) as 0 pixels across, so with switch distances they'd always be at their least
// detailed.
void xbox_lods(Job &job, const std::vector<std::string> &deps) {
    job.lod_deps = deps;
    // A model file as it was before this changed it (for "Update the map": done again from there).
    // (measuring prepared tags, measure_prepared, notes them too: the next build from those tags puts them back)
    const bool keep = !job.analysis || job.update;
    auto keep_original = [&](const fs::path &file, const std::string &bytes) {
        if (keep && !job.model_originals.count(file)) job.model_originals[file] = bytes;
    };
    auto editable = [&](const std::string &dep, bool stock_too) {   // (editable_owner, noting the copies it makes)
        bool was = fs::exists(job.fixes / from_u8(dep));
        fs::path ed = editable_owner(job, dep, stock_too);
        if (!was && !ed.empty() && ed == job.fixes && keep) job.model_created.insert(job.fixes / from_u8(dep));
        return ed;
    };
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
    // (how each shader draws: cut-out parts stay as made in every version)
    std::map<std::string, lod::ShaderLook> looks;
    lod_settings.look = [&](const std::string &tag) {
        auto it = looks.find(tag);
        if (it == looks.end()) it = looks.emplace(tag, shader_look(job, tag)).first;
        return it->second;
    };
    // Each first-person model's animations (its weapon's).
    std::map<std::string, std::string> fp_animations;
    for (auto &dep : deps) {
        if (!has_suffix(dep, ".weapon")) continue;
        fs::path own = job.owner(dep);
        tagfile::Tag wt;
        if (own.empty() || !wt.load(own / from_u8(dep))) continue;
        std::string m = wt.root().ref("first person model"), a = wt.root().ref("first person animations");
        if (!m.empty() && !a.empty() && !fp_animations.count(m)) fp_animations[m] = a;
    }
    // A model's entry in the LODs window (one per model: trimmed, then given LODs or distances, is one entry).
    auto record = [&](const std::string &dep, char kind, const std::string &change) -> LodView::Model & {
        for (auto &m : job.lod_changes)
            if (m.tag == dep) {
                m.change = change;
                return m;
            }
        LodView::Model lm;
        lm.tag = dep;
        lm.name = name_of(dep);
        lm.kind = kind;
        lm.change = change;
        job.lod_changes.push_back(lm);
        return job.lod_changes.back();
    };
    // Why something wasn't done to one (the window says so).
    auto note = [&](const std::string &dep, const std::string &why) {
        if (job.analysis) return;
        for (auto &m : job.lod_changes)
            if (m.tag == dep) m.note += (m.note.empty() ? "" : "; ") + why;
    };

    // The map's own models (drawn in the world, or first-person weapons), each with its choices (lod.hpp Choice: the
    // LODs window's, or skip.txt). The window lists them all, changed or not, with what Bungie's of its kind and size
    // have (x1.5).
    std::vector<std::string> own_models;
    std::map<std::string, lod::Choice> chosen;
    for (auto &dep : deps) {
        if (!has_suffix(dep, ".model") && !has_suffix(dep, ".gbxmodel")) continue;
        fs::path own = job.owner(dep);
        bool fp = first_person.count(dep) > 0;
        if (own.empty() || own == job.overlay || (!fp && !kinds.count(dep))) continue;
        if (!job.only_model.empty() && dep != job.only_model) continue;
        own_models.push_back(dep);
        chosen[dep] = lod::choice(lod_settings, dep, name_of(dep));
        if (job.analysis) continue;
        char kind = fp ? 'f' : kinds[dep];
        LodView::Model &m = record(dep, kind, "");
        double size = lod::model_size(own / from_u8(dep));
        if (size > 0) m.density = (int)std::lround(1.5 * lod::stock_triangles(kind, size));
    }
    // Bungie's models drawn in the world (the Xbox tags'): those with no lower-detail versions of their own get them
    // with the Xbox tab's option for Bungie's models, or a model's own choice (edited copies: your tags stay as they
    // are); the window lists them.
    std::vector<std::string> stock_models;
    for (auto &dep : deps) {
        if (!has_suffix(dep, ".model") && !has_suffix(dep, ".gbxmodel")) continue;
        fs::path own = job.owner(dep);
        if (own.empty() || own != job.overlay || !kinds.count(dep) || first_person.count(dep)) continue;
        if (!job.only_model.empty() && dep != job.only_model) continue;
        stock_models.push_back(dep);
        chosen[dep] = lod::choice(lod_settings, dep, name_of(dep));
    }

    // 0. Trimming: the most detailed version of the map's own heavy models made lighter (lod.hpp trim_top), first, so
    // the lower-detail versions are made from the lighter one. Also when measuring for the weapons & vehicles window:
    // the map has them so. First-person weapons too (drawn whenever they're held, always in full, and close up: kept
    // closer to their shape), and characters (they bend as before: lod.cpp, Bones). A model's own choice first: kept
    // as made, or trimmed to the triangles chosen (allowed to move 5% of its size to get there).
    {
        const int mode = job.cfg.xbox_trim_models;
        bool harder = mode >= 2, by_density = false, by_choice = false, by_setting = false, by_first_person = false;
        std::vector<std::string> trimmed, floor;
        size_t saved = 0;
        for (auto &dep : own_models) {
            fs::path own = job.owner(dep);
            bool fp = first_person.count(dep) > 0;
            const lod::Choice &ch = chosen[dep];
            char kind = fp ? 'f' : kinds[dep];
            if (ch.top == 'k') {
                if (!ch.skip) note(dep, "most detailed version kept as made (your choice)");
                continue;
            }
            lod::TrimSettings ts;
            bool mine = ch.top == 't';
            if (mine) {
                ts.budget = ch.top_triangles;
                ts.max_change = 0.05;
                ts.max_units = 1;
            } else if (mode <= 0) {
                continue;
            } else if (mode >= 3) {   // 1.5x Bungie's for its kind and size, when over that by a quarter
                ts.density = 1.5;
                ts.kind = kind;
                ts.max_change = 0.01;
                ts.max_units = 0.03;
                ts.slack = 1.25;
            } else {
                ts.budget = kind == 'f' || kind == 'b' ? 3000 : kind == 'w' ? 2000 : 4000;
                // (first-person weapons are seen closest; characters up close too, and their fingers thin out first)
                ts.max_change = fp ? (harder ? 0.0025 : 0.0012) : kind == 'b' ? (harder ? 0.004 : 0.0018) : (harder ? 0.006 : 0.0025);
                ts.max_units = harder ? 0.015 : 0.006;
            }
            // A first-person model: what's on screen in its animations, so what never is goes (even within its budget)
            // and what's seen least is simplified first. Not one that's its weapon's model in the world too (some
            // tagsets draw the same model in both): other players and the ground see it from every side.
            lod::Seen seen;
            bool measured = false;
            if (fp && kinds.count(dep)) note(dep, "drawn in the world too (its weapon's own model): nothing left out for first person");
            else if (fp && fp_animations.count(dep)) {
                std::string why;
                measured = first_person_seen(job, dep, fp_animations[dep], lod_settings.look, seen, why);
                if (!measured) note(dep, "not measured in first person: " + why);
            }
            if (measured) ts.seen = &seen;
            perf::ModelStats ms = perf::model_stats(own / from_u8(dep));
            if (!ms.ok || (!measured && ts.density <= 0 && ms.triangles <= ts.budget)) continue;
            if (!measured && ts.density > 0 &&
                ms.triangles <= ts.density * lod::stock_triangles(kind, lod::model_size(own / from_u8(dep))) * ts.slack)
                continue;   // (within its budget: trim_top would say so too)
            fs::path ed = editable(dep, false);
            if (ed.empty()) continue;
            std::string before;
            {
                std::ifstream in(ed / from_u8(dep), std::ios::binary);
                before.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            }
            keep_original(ed / from_u8(dep), before);
            lod::Trim r = lod::trim_top(ed / from_u8(dep), ts, lod_settings, dep);
            if (!r.changed) {
                if (r.before <= r.budget * ts.slack) continue;   // (within its budget)
                floor.push_back(name_of(dep) + " " + thousands(r.before));
                note(dep, "can't lose triangles without changing its shape or how its textures sit");
                continue;
            }
            saved += r.xbox_bytes_saved;
            std::string about = mine ? " (your choice" : r.before <= r.budget * ts.slack ? " (" : r.after > r.budget * 1.1 ? " ("
                              : mode >= 3 ? " (its target" : " (its budget";
            if (r.unseen > 0)
                about += std::string(about.size() > 2 ? "; " : "") + thousands(r.unseen) + " never on screen in its animations";
            about += ")";
            trimmed.push_back(name_of(dep) + " " + thousands(r.before) + " > " + thousands(r.after) + (about == " ()" ? "" : about));
            if (measured) by_first_person = true;
            (mine ? by_choice : mode >= 3 ? by_density : by_setting) = true;
            if (!job.analysis) {
                job.trim_originals[dep] = std::move(before);
                record(dep, kind, "trimmed").trimmed_from = r.before;
            }
        }
        if (!trimmed.empty()) {
            char size[32];
            if (saved >= 1048576) snprintf(size, sizeof size, "%.1f MB", saved / 1048576.0);
            else snprintf(size, sizeof size, "%d KB", (int)((saved + 1023) / 1024));
            std::string how = by_density ? ", to about 1.5x the triangles Bungie's models of their kind and size have"
                              : by_setting ? (harder ? ", trimmed harder so small details may change" : ", so they look the same")
                                           : ", to the triangles chosen in the LODs window";
            if (by_first_person)
                how += "; first-person weapons first lose what's never on screen in their first-person animations, then "
                       "what's seen least";
            job.note("[heavy] made the most detailed version of " + std::to_string(trimmed.size()) +
                     (trimmed.size() == 1 ? " model" : " models") + " lighter" + how + " (" + size +
                     " less of the Xbox's tag memory and map file; triangles as made > now): " + list(trimmed));
        }
        if (!floor.empty())
            job.note("[heavy] left as they are (every triangle shows, or keeps a texture in place): " + list(floor, 4));
    }

    // 1. Making lower-detail versions: the Xbox tab's options (the map's own models; Bungie's), or a model's own choice
    // (made with the triangles chosen, however few it has; or not made). Automatically for a model of 1,000 triangles
    // or more, or of 300 or more with 5,000 or more on the map in all (a tree placed 58 times). Variants that draw
    // another variant at a distance get lower levels of their own (lod.hpp add_lods). Not when measuring for the
    // weapons & vehicles window: it shows what the map needs to fit, and the conversion takes them out again when they
    // don't fit.
    if (!job.analysis) {
        const lod::Settings &ls = lod_settings;
        std::vector<std::string> made, reused, left;
        size_t bytes = 0;
        std::map<std::string, int> placed = models_on_map(job);
        std::vector<std::string> candidates = own_models;
        candidates.insert(candidates.end(), stock_models.begin(), stock_models.end());
        for (auto &dep : candidates) {
            if (first_person.count(dep)) {
                note(dep, "first person: always drawn in full");
                continue;
            }
            fs::path own = job.owner(dep);
            bool stock = own == job.overlay;
            const lod::Choice &ch = chosen[dep];
            perf::ModelStats ms = perf::model_stats(own / from_u8(dep));
            if (!ms.ok) continue;
            if (ms.lods) {
                for (auto &m : job.lod_changes)
                    if (m.tag == dep) m.had_lods = true;
                continue;
            }
            if (stock) {
                LodView::Model &m = record(dep, kinds[dep], "");
                m.stock = true;
            }
            int on_map = placed.count(dep) ? placed[dep] : 0;
            bool option = stock ? job.cfg.xbox_lod_stock_models : job.cfg.xbox_make_lods;
            bool make = ch.lods == 'm' || (ch.lods == 'a' && option);
            bool heavy = ms.triangles >= ls.min_triangles || (ms.triangles >= 300 && (long long)ms.triangles * on_map >= 5000);
            if (!make) {
                if (ch.skip) note(dep, "in skip.txt");
                else if (ch.lods == 'n') note(dep, "no lower-detail versions (your choice)");
                else if (stock && heavy) note(dep, "no lower-detail versions: Bungie's, so only with the Xbox tab's option for Bungie's models or your choice");
                else if (stock) note(dep, "no lower-detail versions (Bungie's)");
                else note(dep, "no lower-detail versions (the Xbox tab's option is off)");
                if (ch.lods == 'n' && option && heavy)
                    left.push_back(name_of(dep) + (ch.skip ? " (listed in the LOD library's skip.txt)" : " (your choice)"));
                continue;
            }
            if (ch.lods != 'm' && !heavy) {
                note(dep, "no lower-detail versions: only " + thousands(ms.triangles) + (ms.triangles == 1 ? " triangle" : " triangles") +
                              (on_map > 1 ? ", " + thousands((long long)ms.triangles * on_map) + " on the map" : std::string()) +
                              " (made from 1,000, or 300 with 5,000 on the map)");
                continue;
            }
            fs::path ed = editable(dep, stock);
            if (ed.empty()) continue;
            double px[5];
            for (int i = 0; i < 5; i++) px[i] = cut_for(dep)->px[i];
            std::string before;
            {
                std::ifstream in(ed / from_u8(dep), std::ios::binary);
                before.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            }
            lod::Settings s = ls;
            s.min_triangles = 0;   // (decided above)
            int targets[2] = {0, 0};
            if (ch.lods == 'm') {
                targets[0] = ch.medium;
                targets[1] = ch.low;
            }
            keep_original(ed / from_u8(dep), before);
            lod::Result r = lod::add_lods(ed / from_u8(dep), px, s, dep, targets);
            if (!r.changed) {
                left.push_back(name_of(dep) + " (" + r.why + ")");
                note(dep, "no lower-detail versions: " + r.why);
                continue;
            }
            job.lod_backups.push_back({ed / from_u8(dep), std::move(before)});
            job.lod_xbox_bytes += r.xbox_bytes;
            bytes += r.xbox_bytes;
            std::string l = name_of(dep) + " " + thousands(r.triangles);
            for (int k : r.level_triangles)
                if (k) l += " > " + thousands(k);
            std::vector<std::string> about;
            if (ch.lods == 'm' && (ch.medium > 0 || ch.low > 0)) about.push_back("your choice");
            if (stock) about.push_back("Bungie's");
            if (on_map > 1) about.push_back(std::to_string(on_map) + " on the map");
            for (size_t k = 0; k < about.size(); k++) l += (k ? ", " : " (") + about[k] + (k + 1 == about.size() ? ")" : "");
            (r.from_library ? reused : made).push_back(l);
            record(dep, kinds[dep], r.from_library ? "library" : "made").stock = stock;
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
        if (!job.only_model.empty() && dep != job.only_model) continue;
        perf::ModelStats ms = perf::model_stats(own / from_u8(dep));
        if (!ms.ok || !ms.lods || ms.borrowed || !ms.no_switches()) continue;
        lod::Choice ch = chosen.count(dep) ? chosen[dep] : lod::choice(lod_settings, dep, name_of(dep));
        if (ch.lods == 'n') continue;   // (none: chosen, or listed in the LOD library's skip.txt)
        fs::path ed = editable(dep, stock);
        if (ed.empty()) continue;
        std::vector<std::string> args;
        for (int i = 0; i < 4; i++) args.insert(args.end(), {"-S", keys[i], std::to_string(cut_for(dep)->px[i])});
        // (super low: its own when it has one, e.g. the Level window's tiny scenery)
        args.insert(args.end(), {"-S", keys[4], std::to_string(std::max<double>(ms.cutoff[4], cut_for(dep)->px[4]))});
        {
            std::ifstream in(ed / from_u8(dep), std::ios::binary);
            keep_original(ed / from_u8(dep), std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
        }
        bool ok = false;
        edit_lines(job, ed, dep, args, &ok);
        if (ok) {
            (stock ? stock_done : own_done).push_back(name_of(dep));
            LodView::Model &m = record(dep, kinds[dep], stock ? "stock" : "distances");
            m.stock = stock;
            m.had_lods = true;
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

// The level's fog. Halo's fog (a sky's outdoor and indoor fog) is linear in distance from the camera: none at its start
// distance, rising evenly to its maximum density at its opaque distance, that density past it (rasterizer_xbox.c's fog
// constants). The engine moves the view's far end to the opaque distance only when the fog is fully opaque there
// (maximum density exactly 1: render.c), and then draws nothing past it: the level, objects, effects. Fog that's nearly
// so (90% or more) hides what's past it anyway, so by default it's made fully opaque (Deathtrap's, Precipice's sky: 92%
// at 110 units, past which the airbase and many trees were still drawn). The level window can instead make the outdoor
// fog fully opaque nearer (a share of its distance) or between distances typed in (where it starts, where it's fully
// opaque). The map's own skies only (Bungie's levels keep theirs); a sky's indoor fog too (rooms with no sky).
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
    double first_end = 0;   // (the first sky's outdoor fog's opaque distance, once it's fully opaque)
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
            double nd = density, no = opaque, ns = start;
            if (cfg.xbox_fog == 2 && k == "outdoor") {
                nd = 1.0;
                no = std::max(opaque * std::clamp(cfg.xbox_fog_scale, 0.1, 1.0), start + 1.0);
            } else if (cfg.xbox_fog == 3 && k == "outdoor") {
                nd = 1.0;
                no = cfg.xbox_fog_distance > 0 ? cfg.xbox_fog_distance : opaque;
                // (a start typed in stays short of the opaque distance: the engine divides by the distance between them)
                if (cfg.xbox_fog_start >= 0) ns = std::clamp(cfg.xbox_fog_start, 0.0, std::max(0.0, no - 1.0));
                no = std::max(no, ns + 1.0);
            } else if (density >= 0.9 && density < 1.0) {
                nd = 1.0;
            }
            if (k == "outdoor" && nd >= 1.0 && first_end <= 0 && i == 0) first_end = no;
            if (nd == density && no == opaque && ns == start) continue;
            tagfile::set_num(r, fd.c_str(), nd);
            tagfile::set_num(r, fo.c_str(), no);
            if (ns != start) tagfile::set_num(r, fs_.c_str(), ns);
            char buf[220];
            if (ns != start)
                snprintf(buf, sizeof buf, "%s fog from %s to %s units, fully opaque there (%s to %s m; it was %.0f%% from %s to %s)",
                         kind, fmt_num(ns).c_str(), fmt_num(no).c_str(), fmt_num(ns * 3.048).c_str(), fmt_num(no * 3.048).c_str(),
                         density * 100, fmt_num(start).c_str(), fmt_num(opaque).c_str());
            else
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
    // What its end cuts through: a treeline or mountains round the level's edge cut off along a line (lshade::fog_cuts).
    lshade::Analysis la;
    lshade::Preview lp;
    if (first_end > 0 && !job.shaders_file.empty() && lshade::load(la, lp, job.shaders_file)) {
        std::string t = lshade::fog_cut_text(lshade::fog_cuts(lp, first_end), first_end);
        if (!t.empty()) job.note("[fog] " + t);
    }
}

// The map's own skies drawn in fewer layers, looking the same (sky.hpp): a still layer drawn in the pass of the one
// after it on the same dome, still layers over a dome drawn by it where it's in front of them, and the clear parts
// of layers cut away. The Xbox draws the whole sky first in every frame, over the whole view, so each layer it saves is
// a screenful of blending. Bungie's skies are as Bungie made them.
static std::string layers_text(double v) {
    char b[32];
    snprintf(b, sizeof b, "%.1f", v);
    return b;
}
// sky::lighten, kept (results kept between conversions): the tags it read (by content) and wrote, and what it found.
// The same sky read from the same tags the next time writes what it wrote then.
static std::string fill_text(const sky::Fill &f) {
    char b[200];
    snprintf(b, sizeof b, "%d %.17g %.17g %d %d", f.ok ? 1 : 0, f.layers, f.worst, f.parts, f.triangles);
    return b;
}
static bool fill_of_text(const std::string &t, sky::Fill &f) {
    int ok = 0;
    if (sscanf(t.c_str(), "%d %lg %lg %d %d", &ok, &f.layers, &f.worst, &f.parts, &f.triangles) != 5) return false;
    f.ok = ok != 0;
    return true;
}
static sky::Lightened lighten_kept(Job &job, const std::string &s, const fs::path &file, const sky::Find &find,
                                   const sky::Edit &edit) {
    const std::string key = result_key("sky lighten 1\n" + s, read_all(file));
    auto content_key = [&](const fs::path &p) {
        std::error_code ec;
        return p.empty() || !fs::is_regular_file(p, ec) ? std::string("-") : result_key("", read_all(p));
    };
    std::string b, note;
    if (load_blob(job.cfg, key, b, note)) {
        sky::Lightened r;
        size_t pos = 0;
        std::string t, h, n, bytes;
        bool ok = get_text(b, pos, t) && t == "HMP sky 1";
        std::vector<std::pair<std::string, std::string>> writes;
        if (ok && get_text(b, pos, n)) {
            for (int k = std::atoi(n.c_str()); ok && k > 0; k--)
                ok = get_text(b, pos, t) && get_text(b, pos, h) && content_key(find(t)) == h;   // (read the same?)
        } else ok = false;
        if (ok && get_text(b, pos, n)) {
            for (int k = std::atoi(n.c_str()); ok && k > 0; k--) {
                ok = get_text(b, pos, t) && get_text(b, pos, bytes);
                if (ok) writes.push_back({t, bytes});
            }
        } else ok = false;
        std::string ch, before, after;
        ok = ok && get_text(b, pos, ch) && get_text(b, pos, before) && get_text(b, pos, after) &&
             fill_of_text(before, r.before) && fill_of_text(after, r.after);
        for (auto *list : {&r.merged, &r.trimmed}) {
            if (!ok || !get_text(b, pos, n)) { ok = false; break; }
            for (int k = std::atoi(n.c_str()); ok && k > 0; k--) {
                ok = get_text(b, pos, t);
                if (ok) list->push_back(t);
            }
        }
        if (ok) {
            r.changed = ch == "1";
            for (auto &[w, wb] : writes) {
                fs::path p = edit(w);
                std::error_code ec;
                if (p.empty()) { ok = false; break; }
                fs::create_directories(p.parent_path(), ec);
                if (!write_all(p, wb)) { ok = false; break; }
                r.written.push_back(w);
            }
            if (ok) return r;
        }
    }
    // (made now, noting what it reads: a tag read after it was written is what it made, not what it read)
    std::map<std::string, std::string> reads;
    std::set<std::string> edited;
    sky::Find rfind = [&](const std::string &t) {
        fs::path p = find(t);
        if (!edited.count(t) && !reads.count(t)) reads[t] = content_key(p);
        return p;
    };
    sky::Edit redit = [&](const std::string &t) {
        edited.insert(t);
        return edit(t);
    };
    sky::Lightened r = sky::lighten(s, rfind, redit);
    std::string rec;
    put_text(rec, "HMP sky 1");
    put_text(rec, std::to_string(reads.size()));
    for (auto &[t, h] : reads) { put_text(rec, t); put_text(rec, h); }
    put_text(rec, std::to_string(r.written.size()));
    for (auto &w : r.written) {
        fs::path p = find(w);
        std::error_code ec;
        if (p.empty() || !fs::is_regular_file(p, ec)) return r;   // (not kept)
        put_text(rec, w);
        put_text(rec, read_all(p));
    }
    put_text(rec, r.changed ? "1" : "0");
    put_text(rec, fill_text(r.before));
    put_text(rec, fill_text(r.after));
    for (auto *list : {&r.merged, &r.trimmed}) {
        put_text(rec, std::to_string(list->size()));
        for (auto &x : *list) put_text(rec, x);
    }
    store_blob(job.cfg, key, rec, "");
    return r;
}

void xbox_sky(Job &job) {
    if (job.cfg.xbox_sky == 1 || job.analysis) return;
    std::string scen = job.scenario + ".scenario";
    fs::path sdir = job.owner(scen);
    tagfile::Tag stag;
    if (sdir.empty() || !stag.load(sdir / from_u8(scen))) return;
    tagfile::Block skies = stag.root().block("skies");
    sky::Find find = [&](const std::string &t) {
        fs::path o = job.owner(t);
        return o.empty() ? fs::path() : o / from_u8(t);
    };
    sky::Edit edit = [&](const std::string &t) {
        if (job.owner(t).empty()) return job.fixes / from_u8(t);   // a new tag
        fs::path d = editable_owner(job, t);
        return d.empty() ? fs::path() : d / from_u8(t);
    };
    std::set<std::string> done;
    for (size_t i = 0; i < skies.size(); i++) {
        std::string s = skies[i].ref("sky");
        if (s.empty() || !done.insert(s).second) continue;
        fs::path own = job.owner(s);
        if (own.empty() || own == job.overlay) continue;
        tagfile::Tag st;
        if (!st.load(own / from_u8(s))) continue;
        std::string model = st.root().ref("model");
        fs::path mown = model.empty() ? fs::path() : job.owner(model);
        if (mown.empty() || mown == job.overlay) continue;
        sky::Lightened r = lighten_kept(job, s, own / from_u8(s), find, edit);
        if (!r.changed) continue;
        std::string what;
        if (!r.merged.empty()) {
            what = "drew ";
            for (size_t k = 0; k < r.merged.size(); k++) what += (k ? "; " : "") + r.merged[k];
            what += r.merged.size() == 1 ? " in one pass" : " in one pass each";
        }
        if (!r.trimmed.empty()) {
            what += what.empty() ? "cut away the clear parts of " : "; cut away the clear parts of ";
            for (size_t k = 0; k < r.trimmed.size(); k++) what += (k ? ", " : "") + r.trimmed[k];
        }
        job.note("[sky] " + fs::path(from_u8(s)).stem().u8string() + ": " + layers_text(r.before.layers) + " layers drawn per pixel, now " +
                 layers_text(r.after.layers) + " (" + layers_text(r.before.worst) + " and " + layers_text(r.after.worst) +
                 " looking up), looking the same (Bungie's multiplayer skies draw 0.8 to 1.4): " + what);
    }
}

// The scenery and devices the map places: their models -> the largest bounding radius of the objects drawing them.
static std::map<std::string, double> placed_scenery_models(Job &job) {
    std::map<std::string, double> models;
    std::string scen = job.scenario + ".scenario";
    fs::path sdir = job.owner(scen);
    tagfile::Tag st;
    if (sdir.empty() || !st.load(sdir / from_u8(scen))) return models;
    for (auto &[o, n] : object_counts(job, st.root())) {
        if (!(has_suffix(o, ".scenery") || has_suffix(o, ".device_machine") || has_suffix(o, ".device_control") ||
              has_suffix(o, ".device_light_fixture")))
            continue;
        fs::path own = job.owner(o);
        tagfile::Tag ot;
        if (own.empty() || !ot.load(own / from_u8(o))) continue;
        std::string m = ot.root().ref("model");
        if (!m.empty()) models[m] = std::max(models[m], ot.root().num("bounding radius"));
    }
    return models;
}

// A model's editable copy for the scenery passes, a new copy in fixes noted like xbox_lods' (Update the map and Make
// again take it out before redoing the levels of detail: a copy of Bungie's model there would count as the map's own).
static fs::path scenery_editable(Job &job, const std::string &m, bool stock_too) {
    bool was = fs::exists(job.fixes / from_u8(m));
    fs::path ed = editable_owner(job, m, stock_too);
    if (!was && !ed.empty() && ed == job.fixes && !job.analysis) job.model_created.insert(job.fixes / from_u8(m));
    return ed;
}

static std::string short_list(const std::vector<std::string> &v, size_t most = 10) {
    std::string l;
    for (size_t i = 0; i < v.size() && i < most; i++) l += (i ? ", " : "") + v[i];
    if (v.size() > most) l += " and " + std::to_string(v.size() - most) + " more";
    return l;
}

// Scenery and devices whose own smallest size only hides them: a model with a super low size but no switch distances
// (Deathtrap's phone booth: 0/0/0/0/35) only ever draws its most detailed version, so that size does nothing but make
// it vanish, and on the Xbox's 480 lines that's much nearer than on a PC's screen (the 1-unit booth at 35 pixels:
// about 13 units away, 6.5 in widescreen). Lowered to the level window's tiny scenery size (0, drawn at any distance,
// when that's off). The map's own models (Bungie's never do this). Before the levels of detail, which keep a model's
// super low when they give it switch distances.
void xbox_scenery_sizes(Job &job, const std::vector<std::string> &deps) {
    const double px = std::max(0.0, job.cfg.xbox_scenery_pixels);
    if (job.analysis) return;
    std::set<std::string> first_person;
    std::map<std::string, std::string> names;
    std::map<std::string, char> kinds = model_kinds(job, deps, first_person, names);
    std::vector<std::string> lowered;
    for (auto &[m, radius] : placed_scenery_models(job)) {
        if (!kinds.count(m) || kinds[m] != 's' || first_person.count(m)) continue;
        fs::path own = job.owner(m);
        if (own.empty() || own == job.overlay) continue;
        perf::ModelStats ms = perf::model_stats(own / from_u8(m));
        if (!ms.ok || !ms.no_switches() || ms.cutoff[4] <= px) continue;
        fs::path ed = scenery_editable(job, m, false);
        if (ed.empty()) continue;
        bool ok = false;
        edit_lines(job, ed, m, {"-S", "super_low_detail_cutoff", std::to_string(px)}, &ok);
        if (!ok) continue;
        // Where its own size hid it: 457 x the diameter / the pixels (lodview_win.cpp's distance).
        double units = radius > 0 ? 457.0 * 2 * radius / ms.cutoff[4] : 0;
        char at[96] = "";
        if (units > 0) snprintf(at, sizeof at, ": gone from about %.*f units, %.*f in widescreen", units < 10 ? 1 : 0, units,
                                units < 20 ? 1 : 0, units / 2);
        lowered.push_back((names.count(m) ? names[m] : fs::path(from_u8(m)).stem().u8string()) + " (" +
                          fmt_num(ms.cutoff[4]) + " px" + at + ")");
    }
    if (!lowered.empty())
        job.note("[scenery] " + std::to_string(lowered.size()) +
                 (lowered.size() == 1 ? " scenery or device model had" : " scenery and device models had") +
                 " a smallest size (\"super low\") but no switch distances, so it only hid " +
                 (lowered.size() == 1 ? "it" : "them") + ", much nearer on the Xbox's 480 lines than on a PC; " +
                 (px > 0 ? "lowered to the Level window's " + fmt_num(px) + " pixels" : std::string("now drawn at any distance")) +
                 ": " + short_list(lowered));
}

// Tiny scenery and devices not drawn (the level window's choice): the engine draws nothing of a model smaller on
// screen than its smallest switch size ("super low", the bounding sphere's pixels across), so that's raised to the
// choice. A size, not a distance: big things stay drawn as far as anyone can see them, and what goes is specks.
// (The engine measures an object's size from how far ahead of the camera it is, not how far away, so a cutoff at a
// distance pops things in and out at the screen's edges as the camera turns: Bungie's own scenery never vanishes.
// Halo 1 Unlimited halves every size in widescreen, so there things go at twice the size.) Vehicles, characters and
// weapons are left alone (players need to see them). After the levels of detail (a copy of Bungie's model this
// makes would count as the map's own there), and again when they're redone.
void xbox_scenery(Job &job, const std::vector<std::string> &deps) {
    const double px = job.cfg.xbox_scenery_pixels;
    if (px <= 0 || job.analysis) return;
    std::set<std::string> first_person;
    std::map<std::string, std::string> names;
    std::map<std::string, char> kinds = model_kinds(job, deps, first_person, names);
    std::vector<std::string> done;
    for (auto &[m, radius] : placed_scenery_models(job)) {
        if (!kinds.count(m) || kinds[m] != 's' || first_person.count(m)) continue;
        fs::path own = job.owner(m);
        if (own.empty()) continue;
        perf::ModelStats ms = perf::model_stats(own / from_u8(m));
        if (!ms.ok || ms.cutoff[4] >= px) continue;
        fs::path ed = scenery_editable(job, m, own == job.overlay);
        if (ed.empty()) continue;
        bool ok = false;
        edit_lines(job, ed, m, {"-S", "super_low_detail_cutoff", std::to_string(px)}, &ok);
        if (ok) done.push_back((names.count(m) ? names[m] : fs::path(from_u8(m)).stem().u8string()));
    }
    if (!done.empty())
        job.note("[scenery] scenery and devices aren't drawn once smaller on screen than " + fmt_num(px) + " pixels across (" +
                 fmt_num(px * 2) + " in widescreen; " + std::to_string(done.size()) + (done.size() == 1 ? " model's" : " models'") +
                 " smallest switch size raised to it): " + short_list(done));
}

// The level's rain or snow, lighter (the level window's choice: perf::weather_lighten). The engine moves, clips and
// draws every drop each frame, indoors too (Deathtrap's: 2.5 ms of an Xbox's 33, every frame), and keeps at most
// 512. Every weather system the level's BSPs use, stock ones too (a copy for this map).
void xbox_weather(Job &job) {
    const int mode = job.cfg.xbox_weather;
    if (mode < 1 || mode > 3 || job.analysis) return;
    std::string scen = job.scenario + ".scenario";
    fs::path sdir = job.owner(scen);
    tagfile::Tag st;
    if (sdir.empty() || !st.load(sdir / from_u8(scen))) return;
    std::set<std::string> systems;
    tagfile::Block bsps = st.root().block("structure bsps");
    for (size_t i = 0; i < bsps.size(); i++) {
        std::string b = bsps[i].ref("structure bsp");
        fs::path own = b.empty() ? fs::path() : job.owner(b);
        tagfile::Tag bt;
        if (own.empty() || !bt.load(own / from_u8(b))) continue;
        tagfile::Block pal = bt.root().block("weather palette");
        for (size_t k = 0; k < pal.size(); k++)
            if (auto w = pal[k].ref("particle system"); !w.empty()) systems.insert(w);
    }
    std::vector<std::string> changed;
    for (auto &w : systems) {
        fs::path ed = editable_owner(job, w, true);
        if (ed.empty()) continue;
        tagfile::EditTag et;
        if (!et.load(ed / from_u8(w))) continue;
        std::vector<tagfile::TagNode> *types = tagfile::block(et.root(), "particle types");
        if (!types || types->empty()) continue;
        std::vector<perf::WeatherType> v;
        for (auto &n : *types) {
            perf::WeatherType t;
            t.count_lo = tagfile::get_num(n, "particle count", 0);
            t.count_hi = tagfile::get_num(n, "particle count", 1);
            t.fade_in_start = tagfile::get_num(n, "fade in start distance");
            t.fade_in_end = tagfile::get_num(n, "fade in end distance");
            t.fade_out_start = tagfile::get_num(n, "fade out start distance");
            t.fade_out_end = tagfile::get_num(n, "fade out end distance");
            v.push_back(t);
        }
        double before = perf::weather_drops(v), end_before = 0;
        for (auto &t : v) end_before = std::max(end_before, t.fade_out_end);
        if (!perf::weather_lighten(v, mode)) continue;
        for (size_t i = 0; i < v.size(); i++) {
            tagfile::TagNode &n = (*types)[i];
            tagfile::set_num(n, "particle count", v[i].count_lo, 0);
            tagfile::set_num(n, "particle count", v[i].count_hi, 1);
            tagfile::set_num(n, "fade in start distance", v[i].fade_in_start);
            tagfile::set_num(n, "fade in end distance", v[i].fade_in_end);
            tagfile::set_num(n, "fade out start distance", v[i].fade_out_start);
            tagfile::set_num(n, "fade out end distance", v[i].fade_out_end);
        }
        if (!et.save(ed / from_u8(w))) continue;
        double after = perf::weather_drops(v), end_after = 0;
        for (auto &t : v) end_after = std::max(end_after, t.fade_out_end);
        std::string what = fs::path(from_u8(w)).stem().u8string() + ": about " + thousands(std::lround(before)) + " drops";
        if (mode == 3) what += ", now none";
        else {
            what += " -> " + thousands(std::lround(after));
            if (end_after < end_before - 0.01)
                what += " (ending " + fmt_num(end_after) + " units away instead of " + fmt_num(end_before) + ")";
        }
        changed.push_back(what);
    }
    if (!changed.empty()) {
        std::string list;
        for (auto &c : changed) list += (list.empty() ? "" : "; ") + c;
        const char *how = mode == 1 ? "lighter (just as dense up close, ending nearer)" : mode == 2 ? "half as heavy" : "off";
        job.note(std::string("[weather] made the level's rain/snow ") + how + ": " + list);
    }
}

// Small scenery with very detailed collision (the level window's choice): its collision model is left out, so
// players and shots pass through it. A player's movement is tested against every surface of the collision models
// it's near, each tick: standing by one of Deathtrap's pilot chairs (416 surfaces for a 1-unit object; a warthog's
// chassis has 47) cost an Xbox 5-6 ms a tick, half the frame's game time.
void xbox_props(Job &job) {
    if (!job.cfg.xbox_detailed_props_off || job.analysis) return;
    std::string scen = job.scenario + ".scenario";
    fs::path sdir = job.owner(scen);
    tagfile::Tag st;
    if (sdir.empty() || !st.load(sdir / from_u8(scen))) return;
    tagfile::Block placed = st.root().block("scenery"), pal = st.root().block("scenery palette");
    std::map<long long, int> count;
    for (size_t i = 0; i < placed.size(); i++) count[placed[i].integer("type")]++;
    std::vector<std::string> done;
    for (auto &[k, n] : count) {
        if (k < 0 || (size_t)k >= pal.size()) continue;
        std::string ref = pal[(size_t)k].ref("name");
        fs::path own = ref.empty() ? fs::path() : job.owner(ref);
        tagfile::Tag ot;
        if (own.empty() || !ot.load(own / from_u8(ref))) continue;
        std::string coll = ot.root().ref("collision model");
        if (coll.empty() || ot.root().num("bounding radius") > perf::kDetailedPropRadius) continue;
        fs::path cown = job.owner(coll);
        int surfaces = cown.empty() ? 0 : perf::collision_surfaces(cown / from_u8(coll));
        if (surfaces < perf::kDetailedPropSurfaces) continue;
        fs::path ed = editable_owner(job, ref, true);
        tagfile::EditTag et;
        if (ed.empty() || !et.load(ed / from_u8(ref))) continue;
        std::string *path = tagfile::ref_path(et.root(), "collision model");
        if (!path) continue;
        path->clear();
        if (!et.save(ed / from_u8(ref))) continue;
        done.push_back(fs::path(from_u8(ref)).stem().u8string() + " x" + std::to_string(n) + " (" + std::to_string(surfaces) +
                       " surfaces)");
    }
    if (!done.empty()) {
        std::string list;
        for (auto &d : done) list += (list.empty() ? "" : ", ") + d;
        job.note("[props] left out the collision of small props whose collision is very detailed (players and shots "
                 "pass through them; a player near one cost the Xbox several ms a tick): " + list);
    }
}

// The LODs window's models (job.lod_changes): how many are on the map, their switch distances and size, and each
// detail level drawn (levels that use the same version share a drawing).
static LodView make_lod_view(Job &job, const std::string &map_name) {
    LodView v;
    v.map_name = map_name;
    v.update = job.work / "update.txt";
    std::map<std::string, double> radius_of;           // model -> its object's bounding radius
    std::map<std::string, int> per_model = models_on_map(job, &radius_of);   // model -> on the map
    preview::Find find = [&](const std::string &tag) {
        fs::path o = job.owner(tag);
        return o.empty() ? fs::path() : o / from_u8(tag);
    };
    auto add_model = [&](LodView::Model m) {
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
        auto orig = job.trim_originals.find(m.tag);
        if (m.trimmed_from > 0 && orig != job.trim_originals.end()) {   // its most detailed version as the map made it
            fs::path tmp = job.cfg.cache_dir / "lods" / ("as_made" + fs::path(from_u8(m.tag)).extension().u8string());
            std::error_code ec;
            fs::create_directories(tmp.parent_path(), ec);
            std::ofstream(tmp, std::ios::binary | std::ios::trunc) << orig->second;
            preview::Find made = [&](const std::string &tag) { return tag == m.tag ? tmp : find(tag); };
            ModelPreview p;
            if (preview::build(m.tag, made, p, 128, 0)) {
                m.as_made = (int)v.previews.size();
                v.previews.push_back(std::move(p));
            }
            fs::remove(tmp, ec);
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
    };
    for (const LodView::Model &c : job.lod_changes) add_model(c);
    // models whose collision changed: with their other changes, or on their own
    for (auto &cc : job.collision_changes) {
        auto it = std::find_if(v.models.begin(), v.models.end(), [&](const LodView::Model &m) { return m.tag == cc.model; });
        if (it == v.models.end()) {
            LodView::Model m;
            m.tag = cc.model;
            m.name = cc.name;
            m.kind = cc.kind;
            m.change = "";
            add_model(m);
            it = v.models.end() - 1;
        }
        it->collision = cc.text;
        it->coll_made = (int)v.previews.size();
        v.previews.push_back(cc.made);
        it->coll_now = (int)v.previews.size();
        v.previews.push_back(cc.now);
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
    // Its skies: the Xbox draws a sky's every layer over the whole view first in each frame.
    {
        tagfile::Block skies = s.block("skies");
        std::set<std::string> seen;
        sky::Find find = [&](const std::string &t) {
            fs::path o = job.owner(t);
            return o.empty() ? fs::path() : o / from_u8(t);
        };
        for (size_t i = 0; i < skies.size(); i++) {
            std::string sk = skies[i].ref("sky");
            if (sk.empty() || !seen.insert(sk).second) continue;
            sky::Fill f = sky::measure(sk, find);
            if (!f.ok) continue;
            char buf[200];
            snprintf(buf, sizeof buf, "Sky (%s): %.1f layers drawn per pixel, %.1f looking up. ", fs::path(from_u8(sk)).stem().u8string().c_str(),
                     f.layers, f.worst);
            lines.push_back(std::string(buf) + "Bungie's multiplayer skies: Sidewinder 0.8 (1.6 looking up), Blood Gulch 1.1 (2.4), "
                                               "Carousel 1.4 (2.4).");
        }
    }
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
        else if (x.st.lods && !x.st.borrowed && x.st.no_switches()) unused.push_back({(long long)x.st.triangles * x.placed, m});
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
        int n = convert_bc7_kept(job.cfg, own / from_u8(dep));
        if (n > 0) { tags++; bitmaps += n; }
    }
    if (tags)
        job.note("[bc7] re-encoded " + std::to_string(bitmaps) + " BC7 textures in " + std::to_string(tags) +
                 " bitmaps as DXT (" + game_name(job.target) + " can't use MCC's BC7 format)");
}

// ---------------------------------------------------------------- Xbox

std::string norm_rel(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
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
    // Short variables that hold the game's time (wide_numbers): their declarations become long.
    std::vector<std::string> texts;   // every file's text (collect_user_names), for that
    bool planned = false;
    std::set<std::string> wide;       // user globals
    std::map<std::string, std::pair<int, std::string>> fixes; // name -> (count, what) (not for a missing name)
    // Units given AI while attached to another object (perf::script_riders): their (ai_attach ...) left out
    // (Config::xbox_rider_ai_off).
    bool rider_ai_off = false;
    std::set<std::string> riders;
    // Continuous scripts that only test players against trigger volumes, each player every tick (Deathtrap's kill
    // volume: 16 players' tests, every tick): they first wait, testing every 5 ticks, until any player is in one
    // (Xbox). The static scripts that are just "player N" ((unit (list_get (players) N))).
    bool poll_volumes = false;
    std::set<std::string> player_scripts;
    static bool player_expr(const hsc::Node &n) {   // (unit (list_get (players) N)) or (list_get (players) N)
        if (!n.list || n.kids.empty() || n.kids[0].list) return false;
        std::string h = hsc::lower(n.kids[0].atom);
        if (h == "unit" && n.kids.size() == 2) return player_expr(n.kids[1]);
        return h == "list_get" && n.kids.size() == 3 && n.kids[1].list && n.kids[1].kids.size() == 1 && !n.kids[1].kids[0].list &&
               hsc::lower(n.kids[1].kids[0].atom) == "players" && !n.kids[2].list && !n.kids[2].str &&
               n.kids[2].atom.find_first_not_of("0123456789") == std::string::npos;
    }
    bool is_player(const hsc::Node &n) const {
        if (player_expr(n)) return true;
        return n.list && n.kids.size() == 1 && !n.kids[0].list && player_scripts.count(hsc::lower(n.kids[0].atom));
    }

    // ---- Shallower calls, and switches. The Xbox runs each script on a stack of 512 bytes (ScriptStack): a call
    // takes 16-30 of them while it runs, and one that would go past the end stops the script for good.
    // What a script does, through every script it calls: the variables it sets (a parameter as the global holding it;
    // calling a script with parameters sets those), the scripts it calls, and whether it can sleep (others run then).
    struct Effects { std::set<std::string> sets, calls; bool sleeps = false; };
    struct UserScript { Effects direct, all; };
    std::map<std::string, UserScript> scripts;      // lowercase name ->
    std::map<std::string, std::string> var_types;   // the map's globals, and the parameters' globals: name -> type
    std::set<std::string> engine_globals;
    bool analyzed = false;
    std::set<std::string> user0;   // (as planned: each run starts from these)
    int param_globals0 = 0;
    std::map<std::string, std::pair<int, std::string>> fixes0;
    // Calls of scripts with parameters whose parameters are set just before the statement they're in: they're only
    // (name), not (begin (set ...) (name)) (a call less, and a frame less on the stack, at each of them)
    std::set<const hsc::Node *> bare;
    const hsc::Node *void_stmt = nullptr;   // the statement being written, when its value isn't used
    std::string cur_script;                  // the script being written
    size_t cur_file = 0;
    // Switches: (if (= v 0) ...) (if (= v 1) ...) ... (Inferno's maps' lookups in their 16-slot "arrays": each test
    // made every time), or the same as else-ifs, (if (= v 0) a (if (= v 1) b ...)) (Digsite's 30-minute announcer:
    // 30 ifs inside each other, too deep for the stack). Written as a tree that tests halves first, (if (< v 8) ...),
    // where the stack allows (convert_texts tries them): shapes[(file, where)].
    bool switches = false;
    struct Shape { int splits = 0; bool flat = true; };
    struct Candidate { std::pair<size_t, size_t> key; bool chain = false, flattenable = false; size_t n = 0; std::string script; };
    std::map<std::pair<size_t, size_t>, Shape> shapes;
    std::vector<Candidate> cands;
    std::set<const hsc::Node *> chain_inner;   // (else-ifs of a switch found from its first if)
    struct Case { double k = 0; std::string ktext; const hsc::Node *test = nullptr, *body = nullptr, *whole = nullptr; };

    static bool number(const std::string &a, double &v) {
        static const std::regex num(R"(^-?[0-9]+(\.[0-9]+)?$)");
        if (!std::regex_match(a, num)) return false;
        v = std::strtod(a.c_str(), nullptr);
        return true;
    }
    // The variable an atom names (a parameter: the global holding it), or "" (a number, a name of something)
    std::string variable(const hsc::Node &a) const {
        if (a.list || a.str) return "";
        std::string l = hsc::lower(a.atom);
        if (subst) {
            auto it = subst->find(l);
            if (it != subst->end()) return it->second;
        }
        return var_types.count(l) || engine_globals.count(l) ? l : "";
    }
    const std::vector<Param> *param_call(const hsc::Node &n) const {
        if (!n.list || n.kids.empty() || n.kids[0].list || n.kids[0].str) return nullptr;
        auto ps = param_scripts.find(hsc::lower(n.kids[0].atom));
        return ps != param_scripts.end() && n.kids.size() - 1 == ps->second.size() ? &ps->second : nullptr;
    }
    void walk_effects(const hsc::Node &n, const std::map<std::string, std::string> &params, Effects &e) const {
        if (!n.list || n.kids.empty()) return;
        if (!n.kids[0].list && !n.kids[0].str) {
            std::string h = hsc::lower(n.kids[0].atom);
            if (h == "set" && n.kids.size() >= 2 && !n.kids[1].list) {
                std::string t = hsc::lower(n.kids[1].atom);
                auto p = params.find(t);
                e.sets.insert(p == params.end() ? t : p->second);
            }
            if (h == "sleep" || h == "sleep_until" || h == "sleep_forever") e.sleeps = true;
            if (scripts.count(h)) e.calls.insert(h);
        }
        for (auto &k : n.kids) walk_effects(k, params, e);
    }
    // Through the scripts it calls
    void close(Effects &e) const {
        std::set<std::string> direct = e.calls;
        for (auto &c : direct) {
            auto u = scripts.find(c);
            if (u == scripts.end()) continue;
            e.sets.insert(u->second.all.sets.begin(), u->second.all.sets.end());
            e.calls.insert(u->second.all.calls.begin(), u->second.all.calls.end());
            e.sleeps = e.sleeps || u->second.all.sleeps;
        }
        for (auto &c : e.calls) {
            auto ps = param_scripts.find(c);
            if (ps != param_scripts.end())
                for (auto &p : ps->second) e.sets.insert(p.global);
        }
    }
    Effects effects_of(const hsc::Node &n, Effects *direct = nullptr) const {
        static const std::map<std::string, std::string> none;
        Effects e;
        walk_effects(n, subst ? *subst : none, e);
        if (direct) *direct = e;
        close(e);
        return e;
    }
    // (after the parameters' globals are named)
    void analyze() {
        if (analyzed) return;
        analyzed = true;
        for (auto &n : kScriptNames)
            if (n.global) engine_globals.insert(hsc::lower(n.name));
        for (auto &[sn, ps] : param_scripts)
            for (auto &p : ps) var_types[p.global] = p.type;
        std::vector<std::vector<hsc::Node>> trees;
        for (auto &s : texts) {
            trees.emplace_back();
            size_t i = 0;
            if (!hsc::parse(s, i, trees.back(), false)) trees.back().clear();
        }
        // (script <kind> <name> body...) / (script static|stub <type> <name> body...); name: (name (type p) ...) with parameters
        struct Def { std::string name; const hsc::Node *n; size_t body; };
        std::vector<Def> defs;
        for (auto &top : trees)
            for (auto &n : top) {
                if (!n.list || n.kids.size() < 3 || n.kids[0].list) continue;
                std::string h = hsc::lower(n.kids[0].atom);
                if (h == "global" && !n.kids[1].list && !n.kids[2].list) {
                    var_types[hsc::lower(n.kids[2].atom)] = hsc::lower(n.kids[1].atom);
                    continue;
                }
                if (h != "script" || n.kids[1].list) continue;
                std::string kind = hsc::lower(n.kids[1].atom);
                size_t at = (kind == "static" || kind == "stub") ? 3 : 2;
                if (n.kids.size() <= at) continue;
                const hsc::Node &nm = n.kids[at];
                std::string name = nm.list ? (nm.kids.empty() || nm.kids[0].list ? "" : nm.kids[0].atom) : nm.atom;
                if (name.empty()) continue;
                defs.push_back({hsc::lower(name), &n, at + 1});
                scripts[hsc::lower(name)];
            }
        for (auto &d : defs) {
            std::map<std::string, std::string> params;
            auto ps = param_scripts.find(d.name);
            if (ps != param_scripts.end())
                for (auto &p : ps->second) params[p.name] = p.global;
            for (size_t k = d.body; k < d.n->kids.size(); k++) walk_effects(d.n->kids[k], params, scripts[d.name].direct);
        }
        for (auto &[name, u] : scripts) u.all = u.direct;
        for (bool more = true; more;) {
            more = false;
            for (auto &[name, u] : scripts) {
                size_t sets = u.all.sets.size(), calls = u.all.calls.size();
                bool sleeps = u.all.sleeps;
                std::set<std::string> cs = u.all.calls;
                for (auto &c : cs) {
                    auto &o = scripts[c].all;
                    u.all.sets.insert(o.sets.begin(), o.sets.end());
                    u.all.calls.insert(o.calls.begin(), o.calls.end());
                    u.all.sleeps = u.all.sleeps || o.sleeps;
                }
                more = more || sets != u.all.sets.size() || calls != u.all.calls.size() || sleeps != u.all.sleeps;
            }
        }
    }

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
        texts.push_back(s);
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
                if (type == "static" && n.kids.size() == at + 2 && !n.kids[at].list && player_expr(n.kids[at + 1]))
                    player_scripts.insert(hsc::lower(n.kids[at].atom));
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

    // A call of a script with parameters: " (set <its global> <argument>) ..." (temporaries first when an argument
    // calls another such script, so all arguments are taken before any is set)
    std::string param_sets(const std::string &s, const hsc::Node &n, const std::vector<Param> &ps) {
        std::string r;
        bool nested = false;
        for (size_t k = 1; k < n.kids.size(); k++) nested = nested || calls_param_script(n.kids[k]);
        std::string head = hsc::lower(n.kids[0].atom);
        std::vector<std::string> temps;
        for (size_t k = 1; k < n.kids.size(); k++) {
            auto &p = ps[k - 1];
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
        for (size_t k = 0; k < temps.size(); k++) r += " (set " + ps[k].global + " " + temps[k] + ")";
        return r;
    }

    static std::string indent_of(const std::string &s, size_t at) {
        size_t line = s.rfind('\n', at ? at - 1 : 0);
        line = line == std::string::npos ? 0 : line + 1;
        size_t e = line;
        while (e < at && (s[e] == ' ' || s[e] == '\t')) e++;
        return s.substr(line, e - line);
    }

    // Variables calling script x can change: what it sets, and the parameters of each script with parameters it calls
    // (itself too, unless `self`)
    std::set<std::string> writes(const std::string &x, bool self) {
        std::set<std::string> w = scripts[x].all.sets;
        auto add = [&](const std::string &t) {
            auto ps = param_scripts.find(t);
            if (ps != param_scripts.end())
                for (auto &p : ps->second) w.insert(p.global);
        };
        if (self) add(x);
        for (auto &t : scripts[x].all.calls) add(t);
        return w;
    }
    // The calls of scripts with parameters in statement c that can have their parameters set before c, so they're just
    // (name): each argument a number or a name, or a variable; c can't sleep (another script could call the same one
    // meanwhile); and, as c is worked out (left to right, a call's arguments before it), nothing before the call
    // changes those variables or the parameters (a call of the same script with others, one through another script).
    // Returns the sets to put before c, and marks the calls (bare).
    std::string hoists(const std::string &s, const hsc::Node &c) {
        if (param_scripts.empty()) return "";
        Effects e = effects_of(c);
        if (e.sleeps) return "";
        // What c does, in order: (node, a call of a script of the map's / a set, outside c's own sequences)
        struct Event { const hsc::Node *n; std::string script, set; bool outer; };
        std::vector<Event> events;
        bool random = false;
        std::function<void(const hsc::Node &, bool)> walk = [&](const hsc::Node &n, bool outer) {
            if (!n.list || n.kids.empty()) return;
            std::string h = !n.kids[0].list && !n.kids[0].str ? hsc::lower(n.kids[0].atom) : "";
            if (h == "begin") outer = false;   // (written by seq on its own)
            if (h == "begin_random") random = true;
            for (auto &k : n.kids) walk(k, outer);
            if (h == "set" && n.kids.size() >= 2 && !n.kids[1].list) {
                std::string v = variable(n.kids[1]);
                events.push_back({&n, "", v.empty() ? hsc::lower(n.kids[1].atom) : v, outer});
            } else if (scripts.count(h)) {
                events.push_back({&n, h, "", outer});
            }
        };
        walk(c, true);
        if (random) return "";
        std::string r;
        std::set<std::string> done;
        for (auto &first : events) {
            const std::vector<Param> *ps = first.script.empty() || !first.outer || first.n == &c ? nullptr : param_call(*first.n);
            if (!ps || done.count(first.script)) continue;
            const std::string &S = first.script;
            done.insert(S);
            // its arguments: numbers, names, variables
            std::set<std::string> vars, globals;
            std::string args;
            bool ok = true;
            for (size_t k = 1; k < first.n->kids.size(); k++) {
                const hsc::Node &a = first.n->kids[k];
                ok = ok && !a.list;
                std::string v = variable(a);
                if (!v.empty()) vars.insert(v);
                args += " " + (a.str ? a.atom : hsc::lower(a.atom));
            }
            if (!ok) continue;
            for (auto &p : *ps) globals.insert(p.global);
            std::set<std::string> own = writes(S, false);
            bool changes_own = false, changes_args = false;
            for (auto &g : globals) changes_own = changes_own || own.count(g);
            for (auto &v : vars) changes_args = changes_args || own.count(v);
            std::vector<const hsc::Node *> mine;
            bool clean = true;
            for (auto &ev : events) {
                if (!ev.set.empty()) {
                    if (vars.count(ev.set) || globals.count(ev.set)) clean = false;
                    continue;
                }
                if (ev.script == S) {
                    std::string a;
                    bool atoms = true;
                    for (size_t k = 1; k < ev.n->kids.size(); k++) {
                        const hsc::Node &x = ev.n->kids[k];
                        atoms = atoms && !x.list;
                        a += " " + (x.str ? x.atom : hsc::lower(x.atom));
                    }
                    bool same = atoms && a == args;
                    if (clean && same && ev.outer && ev.n != &c) mine.push_back(ev.n);
                    else if (!same) clean = false;   // (it sets the parameters to its own)
                    if (changes_own || changes_args) clean = false;
                    continue;
                }
                if (scripts[ev.script].all.calls.count(S)) clean = false;
                for (auto &v : vars)
                    if (writes(ev.script, true).count(v)) clean = false;
            }
            if (mine.empty()) continue;
            for (size_t k = 1; k < first.n->kids.size(); k++)
                r += (r.empty() ? "" : " ") + std::string("(set ") + (*ps)[k - 1].global + " " + emit(s, first.n->kids[k]) + ")";
            for (auto *x : mine) bare.insert(x);
        }
        return r;
    }

    // The statements n.kids[from...] (a script's or a (begin ...)'s), with the spaces and comments between them.
    // `void_end`: the last one's value isn't used either (a script that returns nothing).
    std::string seq(const std::string &s, const hsc::Node &n, size_t from, bool void_end) {
        std::string r;
        size_t no_run = 0;   // (a run of tests found here and left as it is: not looked at again from inside it)
        for (size_t k = from; k < n.kids.size(); k++) {
            const hsc::Node &c = n.kids[k];
            if (k > from) r += s.substr(n.kids[k - 1].e, c.b - n.kids[k - 1].e);
            if (switches && k >= no_run) {
                std::vector<Case> cs;
                std::string var;
                const hsc::Node *vn = nullptr;
                size_t len = switch_run(n, k, cs, var, vn);
                if (len >= 6 && (k + len < n.kids.size() || void_end)) {
                    bool ok = true;
                    for (auto &x : cs) {
                        Effects e = effects_of(*x.body);
                        ok = ok && !e.sets.count(var) && !e.sleeps;
                    }
                    if (ok) {
                        std::pair<size_t, size_t> key{cur_file, c.b};
                        cands.push_back({key, false, true, cs.size(), cur_script});
                        auto sh = shapes.find(key);
                        if (sh != shapes.end()) {
                            r += switch_text(s, *vn, var, cs, sh->second, nullptr, indent_of(s, c.b), false);
                            k += len - 1;
                            continue;
                        }
                    }
                    no_run = k + len;
                }
            }
            const hsc::Node *saved = void_stmt;
            void_stmt = k + 1 < n.kids.size() || void_end ? &c : nullptr;
            std::string pre = hoists(s, c);
            std::string body;
            const std::vector<Param> *ps = bare.count(&c) ? nullptr : param_call(c);
            if (ps) body = param_sets(s, c, *ps).substr(1) + " (" + c.kids[0].atom + ")";   // (no (begin ...) around it)
            else body = emit(s, c);
            void_stmt = saved;
            if (!pre.empty()) r += pre + "\n" + indent_of(s, c.b);
            r += body;
        }
        return r;
    }

    // (= v K) or (= K v): v a variable holding a number, K a number
    bool case_test(const hsc::Node &t, std::string &var, const hsc::Node *&vn, std::string &ktext, double &k) const {
        if (!t.list || t.kids.size() != 3 || t.kids[0].list || t.kids[0].atom != "=") return false;
        for (int o = 0; o < 2; o++) {
            const hsc::Node &a = t.kids[1 + o], &b = t.kids[2 - o];
            if (a.list || a.str || b.list || b.str || !number(b.atom, k)) continue;
            std::string v = variable(a);
            auto ty = var_types.find(v);
            if (v.empty() || ty == var_types.end() || (ty->second != "short" && ty->second != "long" && ty->second != "real")) continue;
            var = v;
            vn = &a;
            ktext = b.atom;
            return true;
        }
        return false;
    }
    // A run of (if (= v K) body) from n.kids[k], each with another K: how many
    size_t switch_run(const hsc::Node &n, size_t k, std::vector<Case> &cs, std::string &var, const hsc::Node *&vn) const {
        std::set<double> seen;
        size_t j = k;
        for (; j < n.kids.size(); j++) {
            const hsc::Node &c = n.kids[j];
            if (!c.list || c.kids.size() != 3 || c.kids[0].list || hsc::lower(c.kids[0].atom) != "if") break;
            std::string v, kt;
            const hsc::Node *vv = nullptr;
            double kv = 0;
            if (!case_test(c.kids[1], v, vv, kt, kv) || (j > k && v != var) || seen.count(kv)) break;
            if (j == k) { var = v; vn = vv; }
            seen.insert(kv);
            cs.push_back({kv, kt, &c.kids[1], &c.kids[2], &c});
        }
        return j - k;
    }
    // How many tests a lookup makes at most, and the levels of ifs it goes through
    static int switch_tests(size_t n, const Shape &sh, bool chain_kept) {
        if (chain_kept) return (int)n;
        size_t leaf = n;
        for (int i = 0; i < sh.splits && leaf > 1; i++) leaf = (leaf + 1) / 2;
        return sh.splits + (int)leaf;
    }
    // On one line when it has no comments (spaces in strings kept)
    static std::string one_line(const std::string &t) {
        if (t.find(';') != std::string::npos) return t;
        std::string r;
        bool str = false, space = false;
        for (char ch : t) {
            if (ch == '"') str = !str;
            if (!str && isspace((unsigned char)ch)) { space = true; continue; }
            if (space && !r.empty() && r.back() != '(' && ch != ')') r += ' ';
            space = false;
            r += ch;
        }
        return r;
    }
    std::string case_if(const std::string &s, const Case &c) {
        if (c.whole) return one_line(emit(s, *c.whole));
        return "(if " + one_line(emit(s, *c.test)) + " " + one_line(emit(s, *c.body)) + ")";
    }
    std::string switch_tree(const std::string &s, const std::string &vtext, const std::vector<Case> &cs, size_t lo, size_t hi, int splits,
                            bool flat, const std::string &els, const std::string &ind) {
        std::string in = ind + "    ";
        if (splits == 0 || hi - lo <= 1) {
            if (flat) {
                if (hi - lo == 1) return case_if(s, cs[lo]);
                std::string r = "(begin";
                for (size_t i = lo; i < hi; i++) r += "\n" + in + case_if(s, cs[i]);
                return r + ")";
            }
            std::string r, close;
            for (size_t i = lo; i < hi; i++) {
                r += (i > lo ? "\n" + in : "") + "(if " + one_line(emit(s, *cs[i].test)) + " " + one_line(emit(s, *cs[i].body));
                close += ")";
            }
            if (!els.empty()) r += "\n" + in + els;
            return r + close;
        }
        size_t mid = lo + (hi - lo) / 2;
        return "(if (< " + vtext + " " + cs[mid].ktext + ")\n" + in + switch_tree(s, vtext, cs, lo, mid, splits - 1, flat, els, in) + "\n" +
               in + switch_tree(s, vtext, cs, mid, hi, splits - 1, flat, els, in) + ")";
    }
    std::string switch_text(const std::string &s, const hsc::Node &vn, const std::string &var, std::vector<Case> cs, const Shape &sh,
                            const hsc::Node *els, const std::string &ind, bool chain) {
        size_t n = cs.size();
        std::sort(cs.begin(), cs.end(), [](const Case &a, const Case &b) { return a.k < b.k; });
        std::string e = els ? emit(s, *els) : "";
        std::string r = switch_tree(s, emit(s, vn), cs, 0, n, sh.splits, sh.flat, e, ind);
        auto &f = fixes[cur_script + " (" + vn.atom + ")"];
        f.first++;
        std::string what = chain ? std::to_string(n) + " else-ifs on " + vn.atom + " (an if inside each other: as many levels of the "
                                   "script's stack)"
                                 : "a " + std::to_string(n) + "-way lookup on " + vn.atom + " (an if for each value, all tested each time)";
        f.second = what + ": now tests halves first, at most " + std::to_string(switch_tests(n, sh, false)) + " tests" +
                   (chain ? " and as few levels" : " instead of " + std::to_string(n));
        (void)var;
        return r;
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
        if (bare.count(&n)) return "(" + n.kids[0].atom + ")";   // (its parameters set before its statement: hoists)
        if (head == "script" && n.kids.size() >= 3 && !n.kids[1].list) {
            std::string kind = hsc::lower(n.kids[1].atom);
            size_t at = (kind == "static" || kind == "stub") ? 3 : 2;
            if (n.kids.size() > at) {
                const hsc::Node &nm = n.kids[at];
                cur_script = nm.list ? (nm.kids.empty() || nm.kids[0].list ? "" : nm.kids[0].atom) : nm.atom;
            }
        }
        // (if (= v 0) a (if (= v 1) b ...)): a switch (see `switches`)
        if (switches && head == "if" && n.kids.size() == 4 && !chain_inner.count(&n)) {
            std::vector<Case> cs;
            std::string var;
            const hsc::Node *vn = nullptr, *els = nullptr, *cur = &n;
            std::set<double> seen;
            std::vector<const hsc::Node *> inner;
            for (;;) {
                std::string v, kt;
                const hsc::Node *vv = nullptr;
                double kv = 0;
                if (!cur->list || (cur->kids.size() != 3 && cur->kids.size() != 4) || cur->kids[0].list ||
                    hsc::lower(cur->kids[0].atom) != "if" || !case_test(cur->kids[1], v, vv, kt, kv) || (!cs.empty() && v != var) ||
                    seen.count(kv)) {
                    els = cur;
                    break;
                }
                if (cs.empty()) { var = v; vn = vv; }
                else inner.push_back(cur);
                seen.insert(kv);
                cs.push_back({kv, kt, &cur->kids[1], &cur->kids[2], nullptr});
                if (cur->kids.size() == 3) break;
                cur = &cur->kids[3];
            }
            chain_inner.insert(inner.begin(), inner.end());
            if (cs.size() >= 4 && (!els || els->e - els->b <= 200)) {
                bool flat = &n == void_stmt && !els;
                for (auto &x : cs) {
                    if (!flat) break;
                    Effects e = effects_of(*x.body);
                    flat = !e.sets.count(var) && !e.sleeps;
                }
                std::pair<size_t, size_t> key{cur_file, n.b};
                cands.push_back({key, true, flat, cs.size(), cur_script});
                auto sh = shapes.find(key);
                if (sh != shapes.end() && (flat || !sh->second.flat))
                    return switch_text(s, *vn, var, cs, sh->second, els, indent_of(s, n.b), true);
            }
        }
        // (script continuous <name> (if (volume_test_object <volume> <player>) ...) ...) -> waits first until any player
        // is in one of its volumes, testing every 5 ticks
        if (poll_volumes && head == "script" && n.kids.size() >= 4 && !n.kids[1].list && hsc::lower(n.kids[1].atom) == "continuous" &&
            !n.kids[2].list) {
            std::vector<std::string> vols;
            bool ok = true;
            for (size_t k = 3; k < n.kids.size() && ok; k++) {
                const hsc::Node &c = n.kids[k];
                ok = c.list && c.kids.size() == 3 && !c.kids[0].list && hsc::lower(c.kids[0].atom) == "if";
                if (!ok) break;
                const hsc::Node &t = c.kids[1];
                ok = t.list && t.kids.size() == 3 && !t.kids[0].list && hsc::lower(t.kids[0].atom) == "volume_test_object" &&
                     !t.kids[1].list && !t.kids[1].str && is_player(t.kids[2]);
                if (ok && std::find(vols.begin(), vols.end(), t.kids[1].atom) == vols.end()) vols.push_back(t.kids[1].atom);
            }
            if (ok && !vols.empty() && vols.size() <= 8) {
                std::string gate = vols.size() == 1 ? "(volume_test_objects " + vols[0] + " (players))" : "(or";
                if (vols.size() > 1) {
                    for (auto &v : vols) gate += " (volume_test_objects " + v + " (players))";
                    gate += ")";
                }
                std::string r = s.substr(n.b, n.kids[3].b - n.b) + "(sleep_until " + gate + " 5)\n    " + seq(s, n, 3, true);
                size_t pos = n.kids.back().e;
                std::string list;
                for (auto &v : vols) list += (list.empty() ? "" : ", ") + v;
                auto &f = fixes[n.kids[2].atom];
                f.first++;
                f.second = "tests each player against " + list + " only once one of them is in it (checked every 5 ticks, "
                           "a sixth of a second), not every player every tick";
                return r + s.substr(pos, n.e - pos);
            }
        }
        // (ai_attach <a rider> <encounter>) -> nothing: it sits on another object without thinking
        if (head == "ai_attach" && !riders.empty() && n.kids.size() >= 2 && !n.kids[1].list && riders.count(hsc::lower(n.kids[1].atom))) {
            auto &f = fixes["ai_attach"];
            f.first++;
            f.second = "units riding other objects left without AI (" + std::to_string(f.first) + ": they sit there as before, without " +
                       "thinking every tick)";
            return stand_in('v');
        }
        // (global short <one that holds the game's time> ...) -> long (wide_numbers)
        if (head == "global" && n.kids.size() >= 3 && !n.kids[1].list && !n.kids[2].list && hsc::lower(n.kids[1].atom) == "short" &&
            wide.count(hsc::lower(n.kids[2].atom))) {
            std::string r = s.substr(n.b, n.kids[1].b - n.b) + "long";
            size_t pos = n.kids[1].e;
            for (size_t k = 2; k < n.kids.size(); k++) {
                r += s.substr(pos, n.kids[k].b - pos) + emit(s, n.kids[k]);
                pos = n.kids[k].e;
            }
            return r + s.substr(pos, n.e - pos);
        }
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
                        bool void_end = at == 2 || n.kids[2].list || hsc::lower(n.kids[2].atom) == "void";
                        if (n.kids.size() > at + 1) {
                            r += s.substr(pos, n.kids[at + 1].b - pos) + seq(s, n, at + 1, void_end);
                            pos = n.kids.back().e;
                        }
                        subst = nullptr;
                        count(ps->first, "parameters turned into globals (not supported in " + std::string(game_name(target)) + ")");
                        return r + s.substr(pos, n.e - pos);
                    }
                }
            }
            // Call: (name a b) -> (begin (set global_a a) (set global_b b) (name))
            // (In a script's or a (begin ...)'s statements, seq writes it without the begin, and hoists can set the
            // parameters before the statement a call is in: then it's just (name).)
            auto ps = param_scripts.find(head);
            if (ps != param_scripts.end() && n.kids.size() - 1 == ps->second.size()) {
                std::string r = "(begin" + param_sets(s, n, ps->second) + " (" + n.kids[0].atom + "))";
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
        // (= (vehicle_script x) none): the call's (begin ...) as a comparison's first operand, of a script returning an
        // object of a kind no function passes on as it is (Cairo's (get_cargo pelican)): an object, through list_get
        // (the vehicle as a list of one). (Elsewhere the begin takes the type its place wants.)
        static const std::set<std::string> compares{"=", "!=", "<", ">", "<=", ">="};
        if (compares.count(head) && n.kids.size() == 3 && param_call(n.kids[1]) && !bare.count(&n.kids[1])) {
            auto rt = param_returns.find(hsc::lower(n.kids[1].kids[0].atom));
            std::string t = rt == param_returns.end() ? "" : rt->second;
            if (t == "vehicle" || t == "weapon" || t == "scenery" || t == "device") {
                std::string r = s.substr(n.b, n.kids[1].b - n.b) + "(list_get " + emit(s, n.kids[1]) + " 0)";
                r += s.substr(n.kids[1].e, n.kids[2].b - n.kids[1].e) + emit(s, n.kids[2]);
                return r + s.substr(n.kids[2].e, n.e - n.kids[2].e);
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
        // A script's statements, and a (begin ...)'s (seq: calls with parameters written without a begin of their own)
        size_t body = 0;
        bool void_end = false;
        if (head == "script" && n.kids.size() >= 3 && !n.kids[1].list) {
            std::string kind = hsc::lower(n.kids[1].atom);
            body = (kind == "static" || kind == "stub") ? 4 : 3;
            void_end = body == 3 || n.kids[2].list || hsc::lower(n.kids[2].atom) == "void";
        } else if (head == "begin" && !user.count(head)) {
            body = 1;
        }
        if (body && n.kids.size() > body) {
            std::string r = s.substr(n.b, n.kids[0].e - n.b);
            size_t pos = n.kids[0].e;
            for (size_t k = 1; k < body; k++) {
                r += s.substr(pos, n.kids[k].b - pos) + emit(s, n.kids[k]);
                pos = n.kids[k].e;
            }
            r += s.substr(pos, n.kids[body].b - pos) + seq(s, n, body, void_end);
            return r + s.substr(n.kids.back().e, n.e - n.kids.back().e);
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

    // A short (16-bit) variable given the game's time, (game_time), wraps negative 32,768 ticks (18 min 12 s) into a
    // game and keeps the wrong value until it wraps back at 36 minutes: Deathtrap's (modulo (game_time) 1800), whose
    // parameters are shorts, stopped its lightning and jerked its warthogs about. Each short global, and each short
    // parameter of a script with parameters, that's set from the time, or from another one that holds it, becomes a
    // long (32 bits, half a year of ticks). Longs and shorts convert to each other where a function wants either.
    void wide_numbers() {
        if (planned) return;
        planned = true;
        if (rider_ai_off) {
            std::string all;
            for (auto &s : texts) all += s + "\n";
            for (auto &r : perf::script_riders(all)) riders.insert(r);
        }
        std::set<std::string> shorts;   // short globals
        struct Assign { std::string target; bool time = false; std::set<std::string> refs; };
        std::vector<Assign> as;
        auto key = [](const std::string &script, size_t k) { return "#" + script + ":" + std::to_string(k); };
        // (what an expression reads: the time, and names; a parameter as its key)
        std::function<void(const hsc::Node &, const std::map<std::string, std::string> &, Assign &)> reads =
            [&](const hsc::Node &n, const std::map<std::string, std::string> &params, Assign &a) {
                if (!n.list) {
                    if (n.str) return;
                    std::string l = hsc::lower(n.atom);
                    auto p = params.find(l);
                    a.refs.insert(p == params.end() ? l : p->second);
                    return;
                }
                if (!n.kids.empty() && !n.kids[0].list) {
                    std::string h = hsc::lower(n.kids[0].atom);
                    if (h == "game_time" || h == "game_time_authoritative") a.time = true;
                }
                for (auto &k : n.kids) reads(k, params, a);
            };
        std::function<void(const hsc::Node &, const std::map<std::string, std::string> &)> walk =
            [&](const hsc::Node &n, const std::map<std::string, std::string> &params) {
                if (!n.list || n.kids.empty()) return;
                if (!n.kids[0].list) {
                    std::string h = hsc::lower(n.kids[0].atom);
                    if (h == "set" && n.kids.size() >= 3 && !n.kids[1].list) {
                        Assign a;
                        std::string t = hsc::lower(n.kids[1].atom);
                        auto p = params.find(t);
                        a.target = p == params.end() ? t : p->second;
                        reads(n.kids[2], params, a);
                        as.push_back(a);
                    }
                    auto ps = param_scripts.find(h);
                    if (ps != param_scripts.end() && n.kids.size() - 1 == ps->second.size())
                        for (size_t k = 1; k < n.kids.size(); k++) {
                            Assign a;
                            a.target = key(h, k - 1);
                            reads(n.kids[k], params, a);
                            as.push_back(a);
                        }
                }
                for (auto &k : n.kids) walk(k, params);
            };
        for (auto &s : texts) {
            std::vector<hsc::Node> top;
            size_t i = 0;
            if (!hsc::parse(s, i, top, false)) continue;
            for (auto &n : top) {
                if (!n.list || n.kids.size() < 3 || n.kids[0].list) continue;
                std::string h = hsc::lower(n.kids[0].atom);
                if (h == "global" && !n.kids[1].list && hsc::lower(n.kids[1].atom) == "short" && !n.kids[2].list)
                    shorts.insert(hsc::lower(n.kids[2].atom));
                std::map<std::string, std::string> params;   // inside a script with parameters: name -> key
                if (h == "script") {
                    std::string type = n.kids[1].list ? "" : hsc::lower(n.kids[1].atom);
                    size_t at = (type == "static" || type == "stub") ? 3 : 2;
                    if (n.kids.size() > at && n.kids[at].list && !n.kids[at].kids.empty() && !n.kids[at].kids[0].list) {
                        std::string sn = hsc::lower(n.kids[at].kids[0].atom);
                        auto ps = param_scripts.find(sn);
                        if (ps != param_scripts.end())
                            for (size_t k = 0; k < ps->second.size(); k++) params[ps->second[k].name] = key(sn, k);
                    }
                }
                walk(n, params);
            }
        }
        auto is_short = [&](const std::string &t) {
            if (t.empty() || t[0] != '#') return shorts.count(t) > 0;
            size_t c = t.rfind(':');
            auto ps = param_scripts.find(t.substr(1, c - 1));
            size_t k = std::stoul(t.substr(c + 1));
            return ps != param_scripts.end() && k < ps->second.size() && ps->second[k].type == "short";
        };
        std::set<std::string> grown;
        for (bool more = true; more;) {
            more = false;
            for (auto &a : as) {
                if (grown.count(a.target) || !is_short(a.target)) continue;
                bool time = a.time;
                for (auto &r : a.refs) time = time || grown.count(r);
                if (time) { grown.insert(a.target); more = true; }
            }
        }
        for (auto &t : grown) {
            if (t[0] != '#') {
                wide.insert(t);
                auto &f = fixes[t];
                f.first++;
                f.second = "a short holding the game's time (it went wrong 18 minutes into a game): now a long";
                continue;
            }
            size_t c = t.rfind(':');
            std::string sn = t.substr(1, c - 1);
            auto &p = param_scripts[sn][std::stoul(t.substr(c + 1))];
            p.type = "long";
            auto &f = fixes[sn + " (" + p.name + ")"];
            f.first++;
            f.second = "a short parameter given the game's time (it went wrong 18 minutes into a game): now a long";
        }
    }

    // Everything worked out from all the files (collect_user_names) before any is converted.
    void plan() {
        if (analyzed) return;
        wide_numbers();
        for (auto &[script, ps] : param_scripts)
            for (auto &p : ps)
                if (p.global.empty()) {
                    std::string g;
                    do g = ("p" + std::to_string(++param_globals) + "_" + script + "_" + p.name).substr(0, 31);
                    while (user.count(g));
                    p.global = g;
                    user.insert(g);
                }
        analyze();
        user0 = user;
        param_globals0 = param_globals;
        fixes0 = fixes;
    }
    // Before converting the files again (convert_texts' tries): as planned
    void restart() {
        changes.clear();
        fixes = fixes0;
        user = user0;
        param_globals = param_globals0;
        extra_globals.clear();
        cands.clear();
    }

    // Converts one file. Returns false if it can't be parsed (left unchanged).
    bool convert(std::string &text) {
        plan();
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
        bare.clear();
        chain_inner.clear();
        void_stmt = nullptr;
        return true;
    }
};

// ---------------------------------------------------------------- how deep scripts go, and what they cost a tick
// Script definitions in parsed files: (script <kind> <name> body...) / (script static|stub <type> <name> body...)
struct ScriptDefs {
    struct Def { std::string kind; const hsc::Node *n = nullptr; size_t body = 0; };
    std::vector<std::vector<hsc::Node>> trees;
    std::map<std::string, Def> defs;   // lowercase name ->
    explicit ScriptDefs(const std::vector<std::string> &texts) {
        trees.reserve(texts.size());
        for (auto &s : texts) {
            trees.emplace_back();
            size_t i = 0;
            if (!hsc::parse(s, i, trees.back(), false)) trees.back().clear();
        }
        for (auto &top : trees)
            for (auto &n : top) {
                if (!n.list || n.kids.size() < 4 || n.kids[0].list || n.kids[1].list || hsc::lower(n.kids[0].atom) != "script") continue;
                std::string kind = hsc::lower(n.kids[1].atom);
                size_t at = (kind == "static" || kind == "stub") ? 3 : 2;
                if (n.kids.size() <= at + 1) continue;
                const hsc::Node &nm = n.kids[at];
                std::string name = nm.list ? (nm.kids.empty() || nm.kids[0].list ? "" : nm.kids[0].atom) : nm.atom;
                if (name.empty()) continue;
                auto &d = defs[hsc::lower(name)];
                if (d.n && d.kind == "static" && kind == "stub") continue;   // (a stub only stands in for one)
                d = {kind, &n, at + 1};
            }
    }
    static bool is_static(const Def &d) { return d.kind == "static" || d.kind == "stub"; }
};

// How deep a script's calls nest, in bytes of the 512 each script has for them on the Xbox (and Halo PC: the
// engine's hs_runtime.c). A call takes a frame of 16 bytes and what its function keeps while it works (an if 12, a
// begin 8, a set 4, (= a b) 14, an ordinary function 4 for each argument and 6, a script 4), and the calls in its
// arguments go on top of it. A script that would go past the end is stopped there, for good.
struct ScriptStack : ScriptDefs {
    static constexpr int kSize = 512;
    std::map<std::string, std::pair<int, std::vector<std::string>>> memo;   // static script: (bytes from its call, path)
    using ScriptDefs::ScriptDefs;

    // Bytes that working out n takes from where its frame starts (0: a value, no frame), and the static scripts it
    // goes through at its deepest
    int depth(const hsc::Node &n, std::vector<std::string> &path, int guard) {
        path.clear();
        if (!n.list || n.kids.empty() || n.kids[0].list || n.kids[0].str) return 0;
        std::string h = hsc::lower(n.kids[0].atom);
        size_t argc = n.kids.size() - 1;
        if (h == "begin" && argc == 1) return depth(n.kids[1], path, guard);   // (Invader leaves out a begin of one)
        if (h == "cond") return cond(n, 1, path, guard);
        auto d = defs.find(h);
        if (d != defs.end() && is_static(d->second)) {
            if (guard > 40) return 20;
            auto m = memo.find(h);
            if (m == memo.end()) {
                memo[h] = {20, {}};   // (a script calling itself: not again)
                std::vector<std::string> p;
                int b = 20 + body(d->second, p, guard + 1);
                m = memo.insert_or_assign(h, std::make_pair(b, p)).first;
            }
            path.push_back(h);
            path.insert(path.end(), m->second.second.begin(), m->second.second.end());
            return m->second.first;
        }
        static const std::map<std::string, int> keeps{
            {"begin", 8}, {"if", 12}, {"and", 9}, {"or", 9}, {"set", 4}, {"=", 14}, {"!=", 14}, {"<", 14}, {">", 14}, {"<=", 14},
            {">=", 14}, {"+", 14}, {"-", 14}, {"*", 14}, {"/", 14}, {"min", 14}, {"max", 14}, {"sleep", 10}, {"sleep_until", 18},
            {"begin_random", 10}, {"unit", 4}, {"inspect", 4}};
        auto k = keeps.find(h);
        int frame = 16 + (k != keeps.end() ? k->second : 4 * (int)argc + 6), best = frame;
        for (size_t i = 1; i < n.kids.size(); i++) {
            std::vector<std::string> p;
            int x = frame + depth(n.kids[i], p, guard);
            if (x > best) { best = x; path = p; }
        }
        return best;
    }
    // (cond (test body...) ...): (if test (begin body...) (if ...))
    int cond(const hsc::Node &n, size_t i, std::vector<std::string> &path, int guard) {
        path.clear();
        while (i < n.kids.size() && (!n.kids[i].list || n.kids[i].kids.empty())) i++;
        if (i >= n.kids.size()) return 0;
        const hsc::Node &c = n.kids[i];
        int best = 28;
        std::vector<std::string> p;
        auto take = [&](int x) { if (x > best) { best = x; path = p; } };
        take(28 + depth(c.kids[0], p, guard));
        if (c.kids.size() == 2) take(28 + depth(c.kids[1], p, guard));
        for (size_t k = 1; c.kids.size() > 2 && k < c.kids.size(); k++) take(28 + 24 + depth(c.kids[k], p, guard));
        take(28 + cond(n, i + 1, p, guard));
        return best;
    }
    int body(const Def &d, std::vector<std::string> &path, int guard) {
        path.clear();
        if (d.n->kids.size() == d.body + 1) return depth(d.n->kids[d.body], path, guard);
        int best = 24;
        for (size_t k = d.body; k < d.n->kids.size(); k++) {
            std::vector<std::string> p;
            int x = 24 + depth(d.n->kids[k], p, guard);
            if (x > best) { best = x; path = p; }
        }
        return best;
    }
    struct Use { int bytes = 0; std::vector<std::string> path; };
    // Each script that runs on its own (not static): the deepest its calls go (its thread's own frame: 20 bytes)
    std::map<std::string, Use> tops() {
        std::map<std::string, Use> r;
        for (auto &[name, d] : defs) {
            if (is_static(d)) continue;
            Use u;
            u.bytes = 20 + body(d, u.path, 0);
            r[name] = u;
        }
        return r;
    }
};

// Roughly what a map's scripts do each tick, in calls (a global read or a comparison: 1; heavier ones more, by
// kCallWeights), and in ms on the Xbox (0.0036 ms a call: Deathtrap's scripts, measured). A continuous script's
// statements run once a round, the round lasting its sleeps (at least a tick); a sleep_until tests its condition every
// period it's given. "Typical": an if's branches count only when it has an else (half each: a switch's tree), and an
// and/or only its first operand; "most": all an if or and/or can run. Static scripts count where they're called
// (not one that sleeps: that runs now and then, not each tick).
struct ScriptCost : ScriptDefs {
    static constexpr double kMsPerCall = 0.0036;
    using Heavy = std::map<std::string, double>;
    bool typical = true;
    bool own_only = false;   // (the static scripts called not counted: own())
    // What a static script's own statements cost, without the static scripts it calls
    double own(const std::string &name) {
        auto d = defs.find(name);
        if (d == defs.end()) return 0;
        own_only = true;
        Heavy h;
        double c = 0;
        for (size_t k = d->second.body; k < d->second.n->kids.size(); k++) c += cost(d->second.n->kids[k], h);
        own_only = false;
        return c;
    }
    std::map<std::pair<std::string, bool>, std::pair<double, Heavy>> memo;
    std::map<std::string, int> sleeps_memo;
    using ScriptDefs::ScriptDefs;

    static double weight(const std::string &f) {
        static const std::map<std::string, double> w{
            {"object_create_anew_containing", 40}, {"object_create_containing", 40}, {"object_destroy_containing", 40},
            {"object_create_anew", 15}, {"object_create", 15}, {"object_destroy", 8}, {"object_destroy_all", 60},
            {"ai_place", 30}, {"ai_erase", 10}, {"ai_kill", 10}, {"ai_go_to_vehicle", 12}, {"ai_magically_see_encounter", 12},
            {"ai_magically_see_players", 10}, {"ai_command_list", 10}, {"ai_command_list_by_unit", 8}, {"ai_attach", 8},
            {"ai_migrate", 10}, {"ai_vehicle_enterable_actors", 8}, {"ai_vehicle_enterable_distance", 8}, {"ai_living_count", 6},
            {"ai_strength", 6}, {"ai_actors", 6}, {"ai_allegiance", 4}, {"effect_new", 8}, {"effect_new_on_object_marker", 8},
            {"damage_new", 10}, {"damage_object", 8}, {"sound_impulse_start", 6}, {"sound_looping_start", 6},
            {"sound_impulse_predict", 4}, {"objects_can_see_object", 15}, {"objects_can_see_flag", 15},
            {"objects_distance_to_object", 6}, {"objects_distance_to_flag", 6}, {"volume_test_objects", 3},
            {"volume_test_objects_all", 3}, {"volume_test_object", 2}, {"volume_teleport_players_not_inside", 6}, {"players", 2},
            {"object_teleport", 6}, {"objects_attach", 6}, {"objects_detach", 4}, {"object_set_scale", 4},
            {"object_set_permutation", 3}, {"device_set_position", 2}, {"device_set_position_immediate", 3},
            {"device_set_power", 2}, {"device_group_set", 3}, {"unit_kill", 6}, {"unit_enter_vehicle", 8}, {"vehicle_load_magic", 8},
            {"vehicle_unload", 6}, {"unit_exit_vehicle", 6}, {"vehicle_test_seat", 2}, {"vehicle_test_seat_list", 4},
            {"vehicle_riders", 3}, {"vehicle_driver", 2}, {"unit_set_current_vitality", 3}, {"object_set_shield", 2},
            {"camera_set", 4}, {"cinematic_start", 4}, {"print", 2}, {"sv_say", 4}, {"begin_random", 2},
            {"scenery_animation_start", 6}, {"custom_animation", 6}, {"object_create_folder", 30}, {"object_destroy_folder", 30}};
        auto it = w.find(f);
        return it == w.end() ? 1 : it->second;
    }
    static bool is_sleep(const std::string &h) { return h == "sleep" || h == "sleep_until" || h == "sleep_forever"; }
    bool sleeps(const std::string &name, int guard = 0) {
        auto m = sleeps_memo.find(name);
        if (m != sleeps_memo.end()) return m->second == 1;
        auto d = defs.find(name);
        if (d == defs.end() || guard > 40) return false;
        sleeps_memo[name] = 0;
        bool r = false;
        std::function<void(const hsc::Node &)> walk = [&](const hsc::Node &n) {
            if (r || !n.list || n.kids.empty()) return;
            if (!n.kids[0].list && !n.kids[0].str) {
                std::string h = hsc::lower(n.kids[0].atom);
                auto c = defs.find(h);
                if (is_sleep(h) || (c != defs.end() && is_static(c->second) && sleeps(h, guard + 1))) { r = true; return; }
            }
            for (auto &k : n.kids) walk(k);
        };
        for (size_t k = d->second.body; k < d->second.n->kids.size(); k++) walk(d->second.n->kids[k]);
        sleeps_memo[name] = r ? 1 : 0;
        return r;
    }
    double cost(const hsc::Node &n, Heavy &heavy, int guard = 0) {
        if (!n.list || n.kids.empty()) return 0;
        if (n.kids[0].list || n.kids[0].str) {
            double t = 0;
            for (auto &k : n.kids) t += cost(k, heavy, guard);
            return t;
        }
        std::string h = hsc::lower(n.kids[0].atom);
        if (is_sleep(h)) return 0;   // (a round's sleeps: rounds())
        double w = weight(h), t = w;
        if (w >= 3) heavy[h] += 1;
        auto d = defs.find(h);
        if (d != defs.end() && is_static(d->second) && guard < 30 && !sleeps(h) && !own_only) {
            heavy["(" + h + ")"] += 1;   // (a static script: how often, for script_cost_notes)
            auto key = std::make_pair(h, typical);
            auto m = memo.find(key);
            if (m == memo.end()) {
                memo[key] = {0, {}};   // (guard)
                Heavy hv;
                double c = 0;
                for (size_t k = d->second.body; k < d->second.n->kids.size(); k++) c += cost(d->second.n->kids[k], hv, guard + 1);
                m = memo.insert_or_assign(key, std::make_pair(c, hv)).first;
            }
            t += m->second.first;
            for (auto &[k, v] : m->second.second) heavy[k] += v;
        }
        auto add = [&](const Heavy &from, double f) { for (auto &[k, v] : from) heavy[k] += v * f; };
        if (h == "if" && n.kids.size() >= 3) {
            t += cost(n.kids[1], heavy, guard);
            Heavy a, b;
            double ca = cost(n.kids[2], a, guard), cb = n.kids.size() > 3 ? cost(n.kids[3], b, guard) : 0;
            if (typical) {
                if (n.kids.size() > 3) { t += (ca + cb) / 2; add(a, 0.5); add(b, 0.5); }
            } else if (ca >= cb) { t += ca; add(a, 1); }
            else { t += cb; add(b, 1); }
            return t;
        }
        if ((h == "and" || h == "or") && n.kids.size() >= 2) {
            t += cost(n.kids[1], heavy, guard);
            for (size_t k = 2; k < n.kids.size() && !typical; k++) t += cost(n.kids[k], heavy, guard);
            return t;
        }
        if (h == "cond") {
            for (size_t k = 1; k < n.kids.size(); k++) {
                const hsc::Node &c = n.kids[k];
                if (!c.list || c.kids.empty()) continue;
                t += cost(c.kids[0], heavy, guard);
                for (size_t j = 1; j < c.kids.size() && !typical; j++) t += cost(c.kids[j], heavy, guard);
            }
            return t;
        }
        for (size_t k = 1; k < n.kids.size(); k++) t += cost(n.kids[k], heavy, guard);
        return t;
    }
    struct Row { std::string name; double calls = 0; Heavy heavy; std::string gate; };   // calls a tick
    std::vector<Row> rounds(bool typ) {
        typical = typ;
        std::vector<Row> rows;
        for (auto &[name, d] : defs) {
            if (d.kind != "continuous") continue;
            Row r;
            r.name = d.n->kids[2].atom;
            double work = 0, ticks = 0, polls = 0;
            Heavy hw;
            for (size_t k = d.body; k < d.n->kids.size(); k++) {
                const hsc::Node &st = d.n->kids[k];
                std::string h = st.list && !st.kids.empty() && !st.kids[0].list ? hsc::lower(st.kids[0].atom) : "";
                double v = 0;
                if (h == "sleep") {
                    ticks += st.kids.size() > 1 && !st.kids[1].list && number(st.kids[1].atom, v) ? std::max(1.0, v) : 30;
                    continue;
                }
                if (h == "sleep_until") {
                    double p = st.kids.size() > 2 && !st.kids[2].list && number(st.kids[2].atom, v) ? std::max(1.0, v) : 30;
                    Heavy hp;
                    polls += cost(st.kids.size() > 1 ? st.kids[1] : st, hp) / p;
                    for (auto &[f, c] : hp) r.heavy[f] += c / p;
                    ticks += p;
                    continue;
                }
                work += cost(st, hw);
            }
            ticks = std::max(1.0, ticks);
            r.calls = work / ticks + polls;
            for (auto &[f, c] : hw) r.heavy[f] += c / ticks;
            // (if <X isn't there> (sleep -1)) first: it stops for good unless X is there (a network trick's object)
            const hsc::Node &f = d.n->kids[d.body];
            if (f.list && f.kids.size() == 3 && !f.kids[0].list && hsc::lower(f.kids[0].atom) == "if" && f.kids[2].list &&
                !f.kids[2].kids.empty() && !f.kids[2].kids[0].list && is_sleep(hsc::lower(f.kids[2].kids[0].atom)) &&
                (f.kids[2].kids.size() == 1 || (!f.kids[2].kids[1].list && f.kids[2].kids[1].atom.rfind("-1", 0) == 0))) {
                std::function<void(const hsc::Node &)> find = [&](const hsc::Node &x) {
                    if (!r.gate.empty() || !x.list || x.kids.size() < 2) return;
                    if (!x.kids[0].list && hsc::lower(x.kids[0].atom) == "unit_get_health" && !x.kids[1].list) { r.gate = x.kids[1].atom; return; }
                    for (auto &k : x.kids) find(k);
                };
                find(f.kids[1]);
                if (r.gate.empty()) r.gate = "a test";
            }
            rows.push_back(r);
        }
        std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.calls > b.calls; });
        return rows;
    }
    static bool number(const std::string &a, double &v) {
        char *end = nullptr;
        v = std::strtod(a.c_str(), &end);
        return !a.empty() && end && *end == 0;
    }
};

static std::string ms_text(double ms) {
    char b[32];
    snprintf(b, sizeof b, ms < 0.095 ? "%.2f" : ms < 9.95 ? "%.1f" : "%.0f", ms);
    return b;
}

// What the scripts cost a tick on the Xbox, as the map made them and as converted
static std::vector<std::string> script_cost_notes(const std::vector<std::string> &made, const std::vector<std::string> &conv) {
    std::vector<std::string> r;
    ScriptCost a(made), b(conv), bm(conv);
    auto ra = a.rounds(true), rb = b.rounds(true), rm = bm.rounds(false);
    auto total = [](const std::vector<ScriptCost::Row> &rows, bool gated) {
        double t = 0;
        for (auto &x : rows) if (x.gate.empty() != gated) t += x.calls;
        return t * ScriptCost::kMsPerCall;
    };
    double ta = total(ra, false), tb = total(rb, false), tm = total(rm, false);
    if (std::max(ta, tb) < 0.25) {
        r.push_back("script work each tick on the Xbox: light (under 0.25 ms of a tick's 33)");
        return r;
    }
    std::string line = "script work each tick on the Xbox: about " + ms_text(tb) + " ms of a tick's 33";
    if (std::fabs(ta - tb) >= 0.05) line += " (" + ms_text(ta) + " ms as the map made them)";
    if (tm > tb * 1.3 + 0.1 && tm < tb * 4) line += ", up to " + ms_text(tm) + " ms when everything they test for is happening";
    r.push_back(line);
    int shown = 0;
    for (auto &x : rb) {
        double ms = x.calls * ScriptCost::kMsPerCall;
        if (!x.gate.empty() || ms < 0.1 || shown == 3) continue;
        shown++;
        // what takes it: heavy functions, and the static scripts it calls (each with all it does)
        std::vector<std::pair<double, std::string>> hv;
        for (auto &[f, c] : x.heavy) {
            if (c < 0.5) continue;
            if (f[0] == '(') {
                std::string sn = f.substr(1, f.size() - 2);
                hv.push_back({c * (1 + b.own(sn)), sn + " x" + std::to_string((int)std::lround(c))});
            } else {
                hv.push_back({c * ScriptCost::weight(f), f + " x" + std::to_string((int)std::lround(c))});
            }
        }
        std::sort(hv.rbegin(), hv.rend());
        std::string what;
        for (size_t i = 0; i < hv.size() && i < 4; i++)
            if (hv[i].first >= x.calls * 0.05) what += (what.empty() ? "" : ", ") + hv[i].second;
        r.push_back("  " + x.name + ": about " + ms_text(ms) + " ms" + (what.empty() ? "" : " (a tick: " + what + ")"));
    }
    double tg = total(rb, true);
    if (tg >= 0.1) {
        std::string names;
        for (auto &x : rb)
            if (!x.gate.empty() && x.calls * ScriptCost::kMsPerCall >= 0.05) names += (names.empty() ? "" : ", ") + x.name;
        r.push_back("  and " + ms_text(tg) + " ms more in scripts that stop at once unless an object is there (" + names + ")");
    }
    return r;
}

// Scripts whose calls go deeper than the Xbox's 512 bytes for them: stopped there
static std::vector<std::string> script_stack_notes(const std::vector<std::string> &texts) {
    std::vector<std::string> r;
    ScriptStack st(texts);
    for (auto &[name, u] : st.tops()) {
        if (u.bytes <= ScriptStack::kSize) continue;
        std::string path = st.defs[name].n->kids[2].atom;
        for (auto &p : u.path) path += " > " + p;
        r.push_back(st.defs[name].n->kids[2].atom + " can go " + std::to_string(u.bytes) + " bytes deep in calls (" + path +
                    "), past the 512 a script has for them: the Xbox would stop it there, for good");
    }
    return r;
}

// Converts one map's scripts, every file (false for one that can't be read: left as it is). For the Xbox, each switch
// (ScriptConverter::switches) is tried as a tree in turn, kept in the shape that tests least if no script then goes
// deeper in calls than kStackSafe bytes (or than it went before, when it already did); else as it is.
static std::vector<bool> convert_texts(ScriptConverter &conv, std::vector<std::string> &texts) {
    static constexpr int kStackSafe = 480;   // (of 512: a little to spare)
    conv.plan();
    auto run = [&](std::vector<std::string> &out) {
        conv.restart();
        out = texts;
        std::vector<bool> ok;
        for (size_t i = 0; i < out.size(); i++) {
            conv.cur_file = i;
            ok.push_back(conv.convert(out[i]));
        }
        return ok;
    };
    std::vector<std::string> out;
    std::vector<bool> ok = run(out);
    if (conv.switches && !conv.cands.empty()) {
        auto cands = conv.cands;
        std::stable_sort(cands.begin(), cands.end(), [](const auto &a, const auto &b) { return a.chain > b.chain; });
        std::set<std::pair<size_t, size_t>> seen;
        auto stack_of = [](const std::vector<std::string> &t) {
            std::map<std::string, int> m;
            for (auto &[k, u] : ScriptStack(t).tops()) m[k] = u.bytes;
            return m;
        };
        auto over = [](const std::map<std::string, int> &m) {
            long o = 0;
            for (auto &[k, v] : m) o += std::max(0, v - ScriptStack::kSize);
            return o;
        };
        auto acc = stack_of(out);
        for (auto &c : cands) {
            if (!seen.insert(c.key).second) continue;
            std::vector<ScriptConverter::Shape> tries;
            int most = 1;
            while ((size_t(1) << (most + 1)) < c.n) most++;
            if (!c.chain) tries = {{2, true}, {1, true}};
            else {
                if (c.flattenable)
                    for (int k = 0; k <= std::min(3, most); k++) tries.push_back({k, true});
                for (int k = 1; k <= most; k++) tries.push_back({k, false});
            }
            auto tests = [&](const ScriptConverter::Shape &sh) {   // (about how many a lookup makes)
                double leaf = (double)c.n;
                for (int i = 0; i < sh.splits; i++) leaf = std::ceil(leaf / 2);
                return sh.splits + (sh.flat ? leaf : (leaf + 1) / 2);
            };
            std::pair<long, double> best{over(acc), c.chain ? (c.n + 1) / 2.0 : (double)c.n};
            bool found = false;
            ScriptConverter::Shape pick;
            std::map<std::string, int> pick_m;
            for (auto &sh : tries) {
                conv.shapes[c.key] = sh;
                std::vector<std::string> t;
                run(t);
                auto m = stack_of(t);
                bool fine = true;
                for (auto &[k, v] : m) {
                    auto was = acc.find(k);
                    if (v > std::max(kStackSafe, was == acc.end() ? 0 : was->second)) fine = false;
                }
                std::pair<long, double> score{over(m), tests(sh)};
                if (fine && score < best) { best = score; pick = sh; pick_m = m; found = true; }
            }
            if (found) {
                conv.shapes[c.key] = pick;
                acc = pick_m;
            } else {
                conv.shapes.erase(c.key);
            }
        }
        ok = run(out);
    }
    texts = out;
    return ok;
}

// Convert every .hsc under `dir` for the target game; notes what changed.
static void convert_scripts(Job &job, const fs::path &dir, Game target) {
    if (!fs::exists(dir)) return;
    std::vector<fs::path> files;
    for (auto &e : fs::recursive_directory_iterator(dir))
        if (e.path().extension() == ".hsc") files.push_back(e.path());
    ScriptConverter conv(target);
    conv.rider_ai_off = target == Game::Xbox && job.cfg.xbox_rider_ai_off;
    conv.poll_volumes = target == Game::Xbox;
    conv.switches = target == Game::Xbox;
    std::sort(files.begin(), files.end());
    std::vector<std::string> texts, made;
    for (auto &f : files) {
        std::ifstream in(f, std::ios::binary);
        texts.push_back(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
        conv.collect_user_names(texts.back());
    }
    made = texts;
    std::vector<bool> ok = convert_texts(conv, texts);
    for (size_t i = 0; i < files.size(); i++) {
        if (!ok[i]) {
            job.note("[scripts] couldn't read " + files[i].filename().u8string() + " to check it for " + game_name(target));
            continue;
        }
        if (texts[i] != made[i]) std::ofstream(files[i], std::ios::binary) << texts[i];
    }
    for (auto &[name, c] : conv.changes)
        job.note("[scripts] " + name + " (not in " + game_name(target) + "): " + c.second + " (" +
                 std::to_string(c.first) + "x)");
    for (auto &[name, c] : conv.fixes) job.note("[scripts] " + name + ": " + c.second);
    if (target != Game::MCC)
        for (auto &l : script_stack_notes(texts)) job.note("[scripts] " + l);
    if (target == Game::Xbox)
        for (auto &l : script_cost_notes(made, texts)) job.note("[scripts] " + l);
}

// Prepare the user's/converted tags for Xbox: only tags the map uses are copied to a scratch
// folder, then models/shaders/HUDs/sounds/scripts are converted there. Stock Xbox tags (overlay)
// take precedence at build time.
// ---------------------------------------------------------------- HUD layout
static bool ends_with(const std::string &s, const std::string &suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

// The HUD tags' canvas sizes, read from an MCC map itself (Invader's extraction leaves the field out: it's unused on
// Halo PC). Each HUD tag (unit, weapon and grenade HUD interfaces, HUD globals) has one: 960p (1) lays it out on a
// canvas twice the Xbox's screen size, 480p (0) on one the Xbox's size. Nothing is known if the map can't be read.
void read_hud_canvases(Job &job, const fs::path &map) {
    std::ifstream f(map, std::ios::binary);
    std::string h(0x800, '\0');
    if (!f.read(&h[0], (std::streamsize)h.size())) return;
    auto u32 = [](const std::string &b, size_t o) -> uint32_t {
        if (o + 4 > b.size()) return 0;
        const unsigned char *p = (const unsigned char *)b.data() + o;
        return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
    };
    std::error_code ec;
    if (h.compare(0, 4, "daeh") != 0 || u32(h, 4) != 13 || u32(h, 8) != fs::file_size(map, ec)) return;   // (MCC, not compressed)
    uint32_t tdo = u32(h, 0x10), tds = u32(h, 0x14);
    if (tds < 0x28 || tds > (256u << 20)) return;
    std::string d(tds, '\0');
    f.seekg(tdo);
    if (!f.read(&d[0], (std::streamsize)d.size())) return;
    uint32_t base = u32(d, 0) - 0x28, count = u32(d, 12);
    auto at = [&](uint32_t p) -> size_t { return p >= base && p - base < tds ? p - base : std::string::npos; };
    size_t arr = at(u32(d, 0));
    if (arr == std::string::npos || count > 65535 || arr + (size_t)count * 32 > d.size()) return;
    std::map<std::string, int> canvas;
    for (uint32_t i = 0; i < count; i++) {
        size_t e = arr + (size_t)i * 32;
        std::string cls{d[e + 3], d[e + 2], d[e + 1], d[e]};
        const char *ext = cls == "unhi" ? "unit_hud_interface" : cls == "wphi" ? "weapon_hud_interface" :
                          cls == "grhi" ? "grenade_hud_interface" : cls == "hudg" ? "hud_globals" : nullptr;
        if (!ext) continue;
        size_t name = at(u32(d, e + 16)), data = at(u32(d, e + 20));
        size_t field = data == std::string::npos ? data : data + (cls == "wphi" ? 62 : 2);
        if (name == std::string::npos || field == std::string::npos || field + 2 > d.size()) continue;
        size_t z = d.find('\0', name);
        if (z == std::string::npos) continue;
        canvas[norm_rel(d.substr(name, z - name)) + "." + ext] = (unsigned char)d[field] | (unsigned char)d[field + 1] << 8;
    }
    if (canvas.empty()) return;
    job.hud_canvas = std::move(canvas);
    job.hud_canvas_known = true;
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

// The field holding the bitmap a HUD element draws (element keys start with `base`): its own interface/meter
// bitmap, or for crosshair overlays, overlay items and a grenade HUD's overlays, their parent's. "" = none
// (numbers, the motion sensor centre).
static std::string hud_element_bitmap_key(const std::map<std::string, std::string> &vals, const std::string &base) {
    for (auto f : {"interface_bitmap", "meter_bitmap"})
        if (vals.count(base + f)) return base + f;
    std::smatch m;
    static const std::regex nested(R"(^(.*\]\.)[a-z_]+\[\d+\]\.$)");    // crosshairs[0].crosshair_overlays[1].
    if (std::regex_match(base, m, nested)) {
        std::string p = m[1];
        for (auto it = vals.lower_bound(p); it != vals.end() && it->first.compare(0, p.size(), p) == 0; ++it)
            if (it->first.find('.', p.size()) == std::string::npos && ends_with(it->first, "_bitmap")) return it->first;
    }
    static const std::regex grenade(R"(^([a-z_]+)s\[\d+\]\.$)");         // total_grenades_overlays[0].
    if (std::regex_match(base, m, grenade) && vals.count(m[1].str() + "_bitmap")) return m[1].str() + "_bitmap";
    return "";
}

// The bitmap a HUD element draws (see hud_element_bitmap_key). "" = none.
static std::string hud_element_bitmap(const std::map<std::string, std::string> &vals, const std::string &base) {
    std::string k = hud_element_bitmap_key(vals, base);
    return k.empty() ? "" : vals.at(k);
}

// Whether a HUD tag is laid out on MCC's 960p canvas (read_hud_canvases): as the map has it when that's known,
// else `guess` (mcc_hud_layout, from the stock HUDs).
static bool hud_960(const Job &job, const std::string &tag, bool guess) {
    if (!job.hud_canvas_known) return guess;
    auto it = job.hud_canvas.find(tag);
    return it != job.hud_canvas.end() && it->second == 1;
}

// The size (texels) of the picture a HUD element draws: its sequence's first sprite, or that sequence's first
// texture whole. False if the bitmap can't be read.
static bool hud_picture_size(const fs::path &file, int seq, double &w, double &h) {
    BitmapParts b;
    if (!b.read(file) || !b.nbd) return false;
    if (seq < 0 || seq >= (int)b.nseq) seq = 0;
    auto rf = [&](size_t o) { uint32_t u = be32(b.b, o); float f; memcpy(&f, &u, 4); return (double)f; };
    int page = 0;
    double l = 0, r = 1, t = 0, bo = 1;
    if (seq < (int)b.nseq && b.sprites((uint32_t)seq)) {
        size_t sp = b.sprite_at((uint32_t)seq);
        page = (int16_t)be16(b.b, sp);
        l = rf(sp + 8), r = rf(sp + 12), t = rf(sp + 16), bo = rf(sp + 20);
    } else if (seq < (int)b.nseq) {
        page = (int16_t)be16(b.b, b.seq_off + 64 * (size_t)seq + 32);
    }
    if (page < 0 || page >= (int)b.nbd) page = 0;
    size_t e = b.bd_off + 48 * (size_t)page;
    w = (r - l) * be16(b.b, e + 4);
    h = (bo - t) * be16(b.b, e + 6);
    return w > 0 && h > 0;
}

// Where the Xbox's HUD corners are on its 640x480 screen: its title-safe frame's (rasterizer_xbox.c,
// RASTERIZER_FRAME_BOUNDS: 48, 36 to 592, 444). MCC's are the screen's own.
static const int kXboxFrameX = 48, kXboxFrameY = 36;

// HUD anchors: the Xbox's five, then MCC's four (top centre, bottom centre, left centre, right centre).
static const char *const kHudAnchors[9] = {"top_left", "top_right", "bottom_left", "bottom_right", "center",
                                           "top_center", "bottom_center", "left_center", "right_center"};
static const char *const kHudAnchorWords[9] = {"top left", "top right", "bottom left", "bottom right", "centre",
                                               "top centre", "bottom centre", "left centre", "right centre"};
// The anchor an enum field has (`key` "anchor" -> "anchor.top_left" = "*"): 0-8 as kHudAnchors, -1 "from parent"
// (an element of a weapon HUD drawn from its HUD's anchor) or none.
static int hud_anchor_of(const std::map<std::string, std::string> &vals, const std::string &key) {
    for (int i = 0; i < 9; i++) {
        auto it = vals.find(key + "." + kHudAnchors[i]);
        if (it != vals.end() && it->second == "*") return i;
    }
    return -1;
}

// The map's own HUD tags, sized and placed for the Xbox (its 640x480 screen, a texel to a pixel at scale 1):
//  - A HUD laid out on a 480p canvas (Halo PC's, and most MCC tags': read_hud_canvases) is placed as it is.
//    "Use high res scale" (on an element) and "half hud scale" / "force hud use highres scale" (on the bitmap it
//    draws) each halve an element (once, whichever is set); the Xbox ignores them, so the element's scale is halved.
//  - One on MCC's 960p canvas has every position halved and every element drawing a bitmap half the size (a
//    quarter with "half hud scale"); "use high res scale" changes nothing there.
//  - MCC places numbers (number elements, a grenade HUD's count) on its 960p canvas whatever the HUD's: on a 480p
//    one from MCC their offsets are halved too (CMT's grenade count under its panel, its ammo count over the bullets).
//    (fit_hud_textures then shrinks bitmaps drawn smaller than they're made.)
//  - MCC's anchors at the middle of an edge (top centre, bottom centre, left centre, right centre) are the Xbox
//    corner (or centre) beside them, each element's offset moved so it lands where MCC puts it on a full screen. A
//    weapon HUD's elements anchored apart from their HUD (MCC's per-element anchors: a visor's pieces along the top
//    and bottom of the screen) go into copies of the HUD, one per anchor, chained after it as its child HUDs (the
//    Xbox draws the whole chain, each from its own anchor), so they stay at their edges on a split screen too.
// Copies made are added to `hud_tags`.
void convert_hud_layout(Job &job, const fs::path &xsrc, std::vector<std::string> &hud_tags) {
    if (hud_tags.empty()) return;
    const Config &cfg = job.cfg;
    const bool guess = mcc_hud_layout(job, !job.user_tags.empty() ? job.user_tags : job.source);
    auto in_overlay = [&](const std::string &rel) { return !job.overlay.empty() && fs::exists(job.overlay / from_u8(rel)); };
    std::string out;
    // Your own tags keep the canvas size (Invader's extraction doesn't): read it from them when any has 960p.
    if (!job.hud_canvas_known && !job.user_tags.empty()) {
        std::map<std::string, int> canvas;
        bool any = false;
        for (auto &tag : hud_tags) {
            tagfile::Tag t;
            if (!t.load(xsrc / from_u8(tag))) continue;
            int c = (int)t.root().integer("canvas size");
            canvas[tag] = c;
            any = any || c == 1;
        }
        if (any) job.hud_canvas = canvas, job.hud_canvas_known = true;
    }

    struct Elem {
        std::string base, bitmap;
        bool flag = false;
        int seq = 0;
        std::string anchor_key;   // the field of the anchor it's drawn from ("" = fixed: the motion sensor, crosshairs)
        bool own = false;         // anchor_key is the element's own (a weapon HUD's per-element anchor)
    };
    std::map<std::string, bool> half;   // bitmap -> flagged "half hud scale" / "force hud use highres scale"
    int scaled = 0, high_res = 0, moved = 0, numbers = 0, anchored = 0, copies = 0, tags960 = 0, tags480 = 0, to_edges = 0;
    // A map from MCC (its HUDs' canvases were read from it, or its HUDs are laid out as MCC's).
    const bool mcc = job.from == Game::MCC || job.hud_canvas_known || guess;
    std::vector<std::string> made;
    for (size_t ti = 0, n0 = hud_tags.size(); ti < n0; ti++) {
        const std::string tag = hud_tags[ti];
        const std::string ext = fs::path(from_u8(tag)).extension().u8string();
        const bool unit = ext == ".unit_hud_interface", weapon = ext == ".weapon_hud_interface";
        const bool c960 = hud_960(job, tag, guess);
        (c960 ? tags960 : tags480)++;
        auto kv = list_values(cfg, xsrc, tag);
        std::map<std::string, std::string> vals(kv.begin(), kv.end());
        std::vector<Elem> elems;
        for (auto &[key, v] : kv) {
            const std::string suffix = "scaling_flags.use_high_res_scale";
            if (!ends_with(key, suffix)) continue;
            Elem e;
            e.base = key.substr(0, key.size() - suffix.size());
            e.flag = v == "1";
            e.bitmap = norm_rel(hud_element_bitmap(vals, e.base));
            if (!e.bitmap.empty() && !ends_with(e.bitmap, ".bitmap")) e.bitmap += ".bitmap";
            auto sq = vals.find(e.base + "sequence_index");
            e.seq = sq == vals.end() ? 0 : std::atoi(sq->second.c_str());
            if (unit) {
                if (e.base.rfind("motion_sensor_", 0) == 0) e.anchor_key = "";            // (always the bottom left)
                else if (e.base.rfind("overlays[", 0) == 0) e.anchor_key = "auxiliary_overlay_anchor";
                else e.anchor_key = "anchor";
            } else if (weapon) {
                std::smatch m;
                static const std::regex in_overlay_elem(R"(^(overlay_elements\[\d+\]\.)overlays\[\d+\]\.$)");
                if (e.base.rfind("crosshairs[", 0) == 0) e.anchor_key = "";               // (always the centre)
                else if (std::regex_match(e.base, m, in_overlay_elem)) e.anchor_key = m[1].str() + "anchor", e.own = true;
                else e.anchor_key = e.base + "anchor", e.own = true;
                if (e.own && !vals.count(e.anchor_key + ".from_parent")) e.anchor_key = "anchor", e.own = false;
            } else {
                e.anchor_key = "anchor";
            }
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
            elems.push_back(e);
        }

        std::vector<std::pair<std::string, std::string>> sets;
        auto num = [&](const std::string &k, double dflt) {
            auto it = vals.find(k);
            return it == vals.end() ? dflt : std::atof(it->second.c_str());
        };
        // Sizes.
        std::map<std::string, double> scale_now;   // element base -> its width and height scale once set
        for (auto &e : elems) {
            double factor = 1;
            if (e.flag) {
                sets.push_back({e.base + "scaling_flags.use_high_res_scale", "0"});
                high_res++;
            }
            if (!e.bitmap.empty()) {
                if (c960) factor = half[e.bitmap] ? 0.25 : 0.5;
                else if (e.flag || half[e.bitmap]) factor = 0.5;
            } else if (e.flag && !c960) {
                factor = 0.5;
            }
            for (auto s : {"width_scale", "height_scale"}) {
                double v = num(e.base + s, 1);
                if (v == 0) v = 1;
                scale_now[e.base + s] = v * factor;
            }
            if (factor != 1) {
                bool any = false;
                for (auto s : {"width_scale", "height_scale"})
                    if (vals.count(e.base + s)) {
                        sets.push_back({e.base + s, std::to_string(scale_now[e.base + s])});
                        any = true;
                    }
                if (any) scaled++;
            }
        }
        // Positions: halved from a 960p canvas. MCC places a HUD's numbers (ammo and grenade counts) on that canvas
        // whatever the HUD's own: their offsets are halved from a 480p one too (map_hud_digits does their digits).
        std::map<std::string, std::pair<int, int>> offset;   // element base -> its offset once set
        for (auto &[key, v] : kv) {
            if (!ends_with(key, "anchor_offset")) continue;
            int x = 0, y = 0;
            if (sscanf(v.c_str(), "%d,%d", &x, &y) != 2) continue;
            const bool number = key.rfind("number_elements[", 0) == 0 || key == "total_grenades_numbers_anchor_offset";
            if (c960 || (number && mcc)) {
                x = (int)std::lround(x / 2.0), y = (int)std::lround(y / 2.0);
                sets.push_back({key, std::to_string(x) + "," + std::to_string(y)});
                (c960 ? moved : numbers)++;
            }
            offset[key.substr(0, key.size() - 13)] = {x, y};
        }
        // Anchors: each element's MCC anchor as an Xbox one, its offset moved to match. The Xbox's corners are those
        // of the TV's title-safe frame, 48 pixels in from the sides and 36 from the top and bottom
        // (rasterizer_xbox.c: RASTERIZER_FRAME_BOUNDS), where MCC's are the screen's: an element from the middle of the
        // top or bottom edge, drawn from the Xbox corner beside it, is moved back by those 48 so it's centred on the
        // screen (CMT's health bar was 48 right of the middle). And an element with an anchor of its own, apart from
        // its HUD's (MCC's per-element anchors: CMT's visor, pieces along the screen's edges), is moved out by the whole
        // frame, to the screen's edges where MCC draws it (on the Xbox they sat 48 and 36 pixels nearer the middle).
        std::map<int, std::vector<size_t>> groups;   // Xbox anchor -> the elements drawn from it
        std::map<std::string, int> field_to;         // an interface anchor field -> the Xbox anchor it becomes
        for (size_t i = 0; i < elems.size(); i++) {
            Elem &e = elems[i];
            if (e.anchor_key.empty()) continue;
            int a = hud_anchor_of(vals, e.anchor_key);
            const bool apart = e.own && a >= 0;                      // its own anchor
            if (a < 0 && e.own) a = hud_anchor_of(vals, "anchor");   // from its HUD's
            if (a < 0) a = 0;
            int x = a;
            if (a >= 5 || (apart && a != 4)) {
                double w = 0, h = 0;
                fs::path dir = !e.bitmap.empty() && in_overlay(e.bitmap) ? job.overlay : xsrc;
                if (!e.bitmap.empty() && hud_picture_size(dir / from_u8(e.bitmap), e.seq, w, h)) {
                    w *= scale_now[e.base + "width_scale"];
                    h *= scale_now[e.base + "height_scale"];
                }
                auto it = offset.find(e.base);
                if (it != offset.end()) {
                    auto [ox, oy] = it->second;
                    double nx = ox;
                    int ny = oy;
                    switch (a) {
                    case 0: case 1: case 2: case 3: nx = ox - kXboxFrameX; ny = oy - kXboxFrameY; break;   // (apart)
                    case 5: x = 0; nx = 320 - kXboxFrameX + ox - w / 2; break;   // top centre: from the top left
                    case 6: x = 2; nx = 320 - kXboxFrameX + ox - w / 2; break;   // bottom centre: from the bottom left
                    case 7: x = 4; nx = ox + w / 2 - 320; break;                 // left centre: from the centre
                    case 8: x = 4; nx = 320 - ox - w / 2; break;                 // right centre: from the centre
                    }
                    if (apart && (a == 5 || a == 6)) ny = oy - kXboxFrameY;
                    sets.push_back({e.base + "anchor_offset", std::to_string((int)std::lround(nx)) + "," + std::to_string(ny)});
                    if (apart) to_edges++;
                } else if (a >= 5) {
                    x = a == 5 ? 0 : a == 6 ? 2 : 4;
                }
                if (a >= 5) anchored++;
            }
            if (!e.own) field_to[e.anchor_key] = x;
            groups[x].push_back(i);
        }
        if (!weapon) {   // one anchor for all (the unit HUD's auxiliary overlays have their own)
            for (auto &[field, x] : field_to)
                if (hud_anchor_of(vals, field) != x) sets.push_back({field, kHudAnchors[x]});
        }
        if (!sets.empty()) {
            std::vector<std::string> cmd{u8(tool(cfg, "invader-edit")), "-t", u8(xsrc), tag};
            for (auto &[k, v] : sets) cmd.insert(cmd.end(), {"-S", k, v});
            if (run_process(cmd, out) != 0)   // one bad key shouldn't lose the rest
                for (auto &[k, v] : sets) run_process({u8(tool(cfg, "invader-edit")), "-t", u8(xsrc), tag, "-S", k, v}, out);
        }
        if (!weapon) continue;

        // A weapon HUD: drawn from one anchor, its own; elements anchored apart go into chained copies.
        int own_anchor = hud_anchor_of(vals, "anchor");
        if (own_anchor < 0) own_anchor = 0;
        bool apart = false;
        for (auto &e : elems)
            if (e.own && hud_anchor_of(vals, e.anchor_key) >= 0) apart = true;
        if (!apart && own_anchor < 5) continue;
        tagfile::EditTag et;
        if (!et.load(xsrc / from_u8(tag))) continue;
        // The groups in order: the HUD keeps the one drawn from its own anchor (or else the first), the rest are copies.
        int keep = groups.count(own_anchor < 5 ? own_anchor : -1) ? own_anchor : groups.empty() ? 0 : groups.begin()->first;
        std::vector<int> order{keep};
        for (auto &[x, list] : groups)
            if (x != keep) order.push_back(x);
        // Which element (block, index) is in which group.
        static const std::regex where(R"(^(static_elements|meter_elements|number_elements|overlay_elements)\[(\d+)\]\.)");
        static const std::map<std::string, const char *> block_names = {
            {"static_elements", "static elements"}, {"meter_elements", "meter elements"},
            {"number_elements", "number elements"}, {"overlay_elements", "overlay elements"}};
        std::map<std::pair<std::string, int>, int> group_of;
        for (auto &[x, list] : groups)
            for (size_t i : list) {
                std::smatch m;
                if (std::regex_search(elems[i].base, m, where)) group_of[{m[1].str(), std::atoi(m[2].str().c_str())}] = x;
            }
        const std::string stem = tag.substr(0, tag.size() - ext.size());
        std::string *child = tagfile::ref_path(et.root(), "child hud");
        if (!child) continue;
        const std::string last_child = *child;
        std::vector<std::pair<std::string, tagfile::EditTag>> outs;   // (path, tag) for the HUD, then each copy
        for (size_t gi = 0; gi < order.size(); gi++) {
            tagfile::EditTag t = et;
            tagfile::TagNode &r = t.root();
            tagfile::set_num(r, "anchor", order[gi]);
            for (auto &[blk, name] : block_names) {
                std::vector<tagfile::TagNode> *b = tagfile::block(r, name);
                if (!b) continue;
                std::vector<tagfile::TagNode> kept;
                for (size_t k = 0; k < b->size(); k++) {
                    auto g = group_of.find({blk, (int)k});
                    if ((g == group_of.end() ? keep : g->second) != order[gi]) continue;
                    kept.push_back((*b)[k]);
                    tagfile::set_num(kept.back(), "anchor", 0);   // from its HUD's
                }
                *b = kept;
            }
            if (gi > 0) {   // a copy: only those elements (the crosshairs and the screen effect stay in the HUD)
                for (auto name : {"crosshairs", "screen effect"})
                    if (auto *b = tagfile::block(r, name)) b->clear();
            }
            std::string path = gi == 0 ? tag : stem + "_" + std::string(kHudAnchors[order[gi]]) + ext;
            outs.push_back({path, std::move(t)});
        }
        bool ok = true;
        for (size_t gi = 0; gi < outs.size() && ok; gi++) {
            tagfile::TagNode &r = outs[gi].second.root();
            std::string *c = tagfile::ref_path(r, "child hud");
            if (!c) { ok = false; break; }
            if (gi + 1 < outs.size()) {
                std::string next = outs[gi + 1].first.substr(0, outs[gi + 1].first.size() - ext.size());
                std::replace(next.begin(), next.end(), '/', '\\');
                *c = next;
                if (r.fixed.size() >= 4) r.fixed.replace(0, 4, "wphi");   // (the reference's tag group)
            } else {
                *c = last_child;
            }
        }
        for (size_t gi = outs.size(); gi-- > 0 && ok;) {   // (the copies first: the HUD then refers to tags that exist)
            fs::path f = xsrc / from_u8(outs[gi].first);
            std::error_code ec;
            fs::create_directories(f.parent_path(), ec);
            ok = outs[gi].second.save(f);
            if (ok && gi > 0) made.push_back(outs[gi].first);
        }
        if (ok) copies += (int)outs.size() - 1;
    }
    for (auto &m : made) hud_tags.push_back(m);
    if (tags960)
        job.note("[xbox] " + std::to_string(tags960) + " of this map's HUDs are laid out for MCC's double-size HUD: halved " +
                 std::to_string(moved) + " HUD positions and resized " + std::to_string(scaled) + " HUD elements to the Xbox's");
    else if (high_res || scaled)
        job.note("[xbox] converted " + std::to_string(std::max(high_res, scaled)) + " HUD elements from high-res scaling "
                 "(drawn at half size on Halo PC and MCC)");
    if (numbers)
        job.note("[xbox] placed " + std::to_string(numbers) + " HUD numbers (ammo and grenade counts) where MCC draws them: "
                 "MCC lays numbers out on its double-size HUD canvas, so their offsets are halved (they sat beside or "
                 "below their panels)");
    if (anchored || copies)
        job.note("[xbox] placed " + std::to_string(anchored) + " HUD elements anchored where the Xbox has no anchor (the middle "
                 "of a screen edge) from the corner or centre beside it" +
                 (copies ? ", and put the elements of weapon HUDs anchored apart from their HUD into " + std::to_string(copies) +
                               " copies of those HUDs chained after them, each drawn from its own anchor" : "") +
                 " (centred on the screen, not on the Xbox's title-safe frame, which starts 48 pixels in from the left)");
    if (to_edges)
        job.note("[xbox] moved " + std::to_string(to_edges) + " HUD element" + (to_edges == 1 ? "" : "s") + " with an anchor of "
                 "their own (MCC's per-element anchors, such as a visor's pieces along the screen's edges) out to the screen's "
                 "edges, where MCC draws them: the Xbox draws HUDs from its title-safe frame, 48 pixels in from the sides and "
                 "36 from the top and bottom");
}

// The map's own HUD bitmaps at the resolution the Xbox shows them. The Xbox draws its HUD on a 640x480 screen,
// a texel to a pixel at scale 1. A HUD made for big screens draws big textures at a fraction of that (an MCC
// tagset's often at a quarter), so most of their texels never reach the screen, and without mipmaps the ones
// that do shimmer. A bitmap that only HUD elements draw is halved while every element still draws it at no
// more than a texel to a pixel, and the elements' scales are doubled to match: the same size on screen, from a
// quarter of the memory per halving.
// A weapon's screen effect mask (a scope's dark surround) is drawn a texel to a pixel, centred on the view
// (rasterizer_xbox_screen_effect.c), not stretched: one made for a 1080p screen (1024 rows) is bigger than the
// whole 480-row Xbox screen, so the scope's window covers all of it and the surround never shows. It is halved
// while it still covers the screen (the split-screen view, for a split-screen-only mask), which frames the
// view about as it does on the screen it was made for.
// Then what never reaches the screen goes (trim_hud_bitmap_tag): the smaller copies (mipmaps) of a bitmap every
// element draws at a texel to a pixel or bigger, and of a mask; and the edges of one only drawn centred as a crosshair
// overlay and bigger than the 640x480 screen (CMT's focus rifle scope overlay: 1024x1024 at full size, cut to the
// middle 1024x512; a split-screen view is smaller still, so it's covered too).
static void fit_hud_textures(Job &job, const fs::path &xsrc, const std::vector<std::string> &hud_tags) {
    if (hud_tags.empty()) return;
    const Config &cfg = job.cfg;
    auto in_overlay = [&](const std::string &rel) { return !job.overlay.empty() && fs::exists(job.overlay / from_u8(rel)); };
    auto bitmap_path = [](const std::string &v) {
        std::string b = norm_rel(v);
        if (!b.empty() && !ends_with(b, ".bitmap")) b += ".bitmap";
        return b;
    };
    struct Draw { std::string tag, base; double w, h; bool centred; };   // centred: a crosshair overlay at the middle
    std::map<std::string, std::vector<Draw>> draws;   // bitmap -> the elements that draw it
    std::map<std::string, int> mask_rows;             // mask -> the screen rows it must cover (480; split screen 240)
    std::set<std::string> other;                      // anything else a HUD refers to (those bitmaps are left alone)
    const std::set<std::string> huds(hud_tags.begin(), hud_tags.end());
    for (auto &tag : hud_tags) {
        auto kv = list_values(cfg, xsrc, tag);
        std::map<std::string, std::string> vals(kv.begin(), kv.end());
        std::set<std::string> drawn;   // the fields elements draw from
        for (auto &[key, v] : kv) {
            const std::string suffix = "scaling_flags.use_high_res_scale";
            if (!ends_with(key, suffix)) continue;
            std::string base = key.substr(0, key.size() - suffix.size()), bk = hud_element_bitmap_key(vals, base);
            if (bk.empty() || vals[bk].empty()) continue;
            drawn.insert(bk);
            auto w = vals.find(base + "width_scale"), h = vals.find(base + "height_scale");
            if (w == vals.end() || h == vals.end()) {
                other.insert(bitmap_path(vals[bk]));
                continue;
            }
            auto at = vals.find(base + "anchor_offset");
            Draw d{tag, base, std::atof(w->second.c_str()), std::atof(h->second.c_str()),
                   base.rfind("crosshairs[", 0) == 0 && at != vals.end() && at->second == "0,0"};
            if (d.w == 0) d.w = 1;   // 0 draws at 1
            if (d.h == 0) d.h = 1;
            draws[bitmap_path(vals[bk])].push_back(d);
        }
        for (auto &[key, v] : kv) {
            if (v.empty() || drawn.count(key)) continue;
            if (ends_with(key, "mask_fullscreen")) mask_rows[bitmap_path(v)] = 480;
            else if (ends_with(key, "mask_splitscreen")) mask_rows[bitmap_path(v)] = std::max(mask_rows[bitmap_path(v)], 240);
            else other.insert(bitmap_path(v));   // every other value: a reference in a field this doesn't know
        }
    }
    std::set<std::string> cands;
    for (auto &[bm, d] : draws)
        if (!mask_rows.count(bm)) cands.insert(bm);
    for (auto &[bm, r] : mask_rows)
        if (!draws.count(bm)) cands.insert(bm);
    for (auto it = cands.begin(); it != cands.end();)
        if (other.count(*it) || in_overlay(*it) || !fs::exists(xsrc / from_u8(*it))) it = cands.erase(it);
        else ++it;
    if (cands.empty()) return;

    // The HUD globals the build uses: the Xbox's (merge_map_globals copies what it needs from the map's), unless
    // the map's own wins. Another one (the map's, now unused) can refer to the HUD's bitmaps without drawing them.
    std::string hud_globals;
    {
        const std::string g = "globals/globals.globals";
        fs::path gd = job.owner(g);
        if (gd.empty() && fs::exists(xsrc / from_u8(g))) gd = xsrc;
        std::string v;
        if (!gd.empty()) run_process({u8(tool(cfg, "invader-edit")), "-t", u8(gd), g, "-G", "interface_bitmaps[0].hud_globals"}, v);
        hud_globals = norm_rel(v);
        if (!hud_globals.empty() && !ends_with(hud_globals, ".hud_globals")) hud_globals += ".hud_globals";
    }
    auto users = find_referrers({xsrc}, cands);
    size_t hud_before = 0, hud_after = 0, mask_before = 0, mask_after = 0, trim_before = 0, trim_after = 0;
    int fitted = 0, masks = 0, cropped = 0, unmipped = 0;
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> sets;   // HUD tag -> (field, value)
    for (auto &bm : cands) {
        bool hud_only = true;
        for (auto &u : users[bm])
            if (!huds.count(norm_rel(u)) && !(ends_with(u, ".hud_globals") && norm_rel(u) != hud_globals && !hud_globals.empty()))
                hud_only = false;
        if (!hud_only) continue;
        fs::path p = xsrc / from_u8(bm);
        BitmapParts parts;
        if (!parts.read(p) || !parts.nbd) continue;
        const bool mask = mask_rows.count(bm) > 0;
        int want = 0;
        if (mask) {
            for (int rows = be16(parts.b, parts.bd_off + 6); rows / 2 >= mask_rows[bm] && want < 4; rows /= 2) want++;
        } else {
            double s = 0;
            for (auto &d : draws[bm]) s = std::max({s, std::fabs(d.w), std::fabs(d.h)});
            for (; s > 0 && s * 2 <= 1.0001 && want < 4; s *= 2) want++;
        }
        int k = want ? shrink_hud_bitmap_tag(p, want) : 0;
        BitmapParts now;
        if (k && now.read(p)) {
            (mask ? mask_before : hud_before) += parts.pp;
            (mask ? mask_after : hud_after) += now.pp;
            (mask ? masks : fitted)++;
            if (!mask)
                for (auto &d : draws[bm]) {
                    sets[d.tag].push_back({d.base + "width_scale", std::to_string(d.w * (1 << k))});
                    sets[d.tag].push_back({d.base + "height_scale", std::to_string(d.h * (1 << k))});
                }
        } else {
            k = 0;
        }
        // Then what never reaches the screen goes: the smaller copies of a bitmap always drawn at a texel to a pixel
        // or bigger (a mask's too), and the edges of one drawn centred bigger than the screen (a scope's overlay).
        double smin = 1e9, sx = 0, sy = 0;
        bool centred = !mask && !draws[bm].empty();
        for (auto &d : draws[bm]) {
            double dw = std::fabs(d.w) * (1 << k), dh = std::fabs(d.h) * (1 << k);
            smin = std::min({smin, dw, dh});
            sx = std::max(sx, dw), sy = std::max(sy, dh);
            centred = centred && d.centred;
        }
        auto pow2 = [](double v) { int n = 4; while (n < v && n < 8192) n *= 2; return n; };
        const bool drop = mask || smin >= 0.999;
        const int mw = centred && sx > 0 ? pow2(std::ceil(640 / sx - 1e-6)) : 0, mh = centred && sy > 0 ? pow2(std::ceil(480 / sy - 1e-6)) : 0;
        BitmapParts before;
        if ((drop || mw || mh) && before.read(p)) {
            size_t saved = trim_hud_bitmap_tag(p, drop, mw, mh);
            BitmapParts after;
            if (saved && after.read(p)) {
                trim_before += before.pp;
                trim_after += after.pp;
                const bool cut = be16(before.b, before.bd_off + 4) != be16(after.b, after.bd_off + 4) ||
                                 be16(before.b, before.bd_off + 6) != be16(after.b, after.bd_off + 6);
                (cut ? cropped : unmipped)++;
            }
        }
    }
    std::string out;
    for (auto &[tag, s] : sets) {
        std::vector<std::string> cmd{u8(tool(cfg, "invader-edit")), "-t", u8(xsrc), tag};
        for (auto &[k, v] : s) cmd.insert(cmd.end(), {"-S", k, v});
        if (run_process(cmd, out) != 0)   // one bad key shouldn't lose the rest
            for (auto &[k, v] : s) run_process({u8(tool(cfg, "invader-edit")), "-t", u8(xsrc), tag, "-S", k, v}, out);
    }
    auto mb = [](size_t b) {
        char t[32];
        snprintf(t, sizeof t, b >= 1048576 ? "%.1f MB" : "%.0f KB", b >= 1048576 ? b / 1048576.0 : b / 1024.0);
        return std::string(t);
    };
    if (fitted)
        job.note("[xbox] fitted " + std::to_string(fitted) + " HUD bitmap" + (fitted == 1 ? "" : "s") +
                 " to the resolution the Xbox draws " + (fitted == 1 ? "it" : "them") + " at (a texel to a screen pixel; "
                 "same size on screen): " + mb(hud_before) + " of textures now " + mb(hud_after));
    if (masks)
        job.note("[xbox] scaled " + std::to_string(masks) + " weapon screen mask" + (masks == 1 ? "" : "s") +
                 " (scope surrounds) made for bigger screens to the Xbox's, which draws them a texel to a pixel: "
                 "they frame the view as made instead of covering the whole screen with the window (" +
                 mb(mask_before) + " now " + mb(mask_after) + ")");
    if (cropped || unmipped) {
        std::string what;
        if (unmipped)
            what = "left out the smaller copies (mipmaps) of " + std::to_string(unmipped) + " HUD bitmap" + (unmipped == 1 ? "" : "s") +
                   " the HUD always draws at a texel to a pixel or bigger, which never reads them";
        if (cropped)
            what += std::string(what.empty() ? "" : ", and ") + "cut " + std::to_string(cropped) + " drawn centred bigger than the "
                    "screen (scope overlays) down to the middle the screen shows, without their smaller copies";
        job.note("[xbox] " + what + ": " + mb(trim_before) + " of textures now " + mb(trim_after) + " (the same on screen)");
    }
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
    std::set<uint32_t> pictures;   // the map's pictures its added arrows use (only those join the Xbox's sheet)
    for (int i : add)
        for (auto &[f, v] : ma[i])
            if (is_seq(f) && !v.empty() && std::atoi(v.c_str()) != 65535 && std::atoi(v.c_str()) >= 0) {
                need = std::max(need, std::atoi(v.c_str()) + 1);
                pictures.insert((uint32_t)std::atoi(v.c_str()));
            }
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
            if (!ec && got == shrink && merge_icon_sheets(xadir / from_u8(xab), tmp, (uint32_t)have, xsrc / from_u8(merged), &pictures) &&
                job.edit(xhg, "arrow_bitmap", merged)) {
                int n = 0;
                for (uint32_t q : pictures) n += (int)q >= have;
                sheet_note = ", with their " + std::to_string(n) + " picture" + (n == 1 ? "" : "s") +
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

// The map's own HUD digits (the globals' "hud digits definition", a .hud_number: the digits' sheet and their widths)
// made the Xbox's size, for the Xbox's globals to use. The map's number elements are placed for its own digits: a
// digit is a sprite drawn from the number's anchor (right-aligned at a right anchor), and the next one a digit's
// screen width along, so with the Xbox's digits (another font, another width) its counts landed beside their panels,
// over the bullets. The Xbox draws a digit a texel to a pixel and steps the screen width, at any element scale. MCC
// draws HUD digits at half that (the screen width halved too), and a sheet flagged "half hud scale" or "force hud use
// highres scale" at half again for each: a copy of the sheet is shrunk to the size MCC shows (the map's other HUD
// elements may draw the same sheet), and the widths scaled to match. False if it can't be done (the Xbox's digits stay).
static bool map_hud_digits(Job &job, const fs::path &xsrc, const std::string &md, std::string &how) {
    const Config &cfg = job.cfg;
    auto kv = list_values(cfg, xsrc, md);
    std::map<std::string, std::string> v(kv.begin(), kv.end());
    std::string bm = norm_rel(v["digits_bitmap"]);
    if (bm.empty()) return false;
    if (!ends_with(bm, ".bitmap")) bm += ".bitmap";
    const bool stock = !job.overlay.empty() && fs::exists(job.overlay / from_u8(bm));
    fs::path bdir = stock ? job.overlay : xsrc;
    if (!fs::exists(bdir / from_u8(bm))) return false;
    int halvings = job.from == Game::MCC || mcc_hud_layout(job, !job.user_tags.empty() ? job.user_tags : job.source) ? 1 : 0;
    const double widths = halvings ? 0.5 : 1.0;
    for (auto k : {"flags.half_hud_scale", "flags.force_hud_use_highres_scale"}) {
        std::string g;
        run_process({u8(tool(cfg, "invader-edit")), "-t", u8(bdir), bm, "-G", k}, g);
        if (g.rfind("1", 0) == 0) halvings++;
    }
    std::string sheet = bm;
    if (halvings) {
        sheet = bm.substr(0, bm.size() - 7) + "_xbox.bitmap";
        fs::path to = xsrc / from_u8(sheet);
        std::error_code ec;
        fs::create_directories(to.parent_path(), ec);
        fs::copy_file(bdir / from_u8(bm), to, fs::copy_options::overwrite_existing, ec);
        if (ec || shrink_hud_bitmap_tag(to, halvings) != halvings) {
            fs::remove(to, ec);
            return false;
        }
    }
    std::vector<std::string> cmd{u8(tool(cfg, "invader-edit")), "-t", u8(xsrc), md};
    if (sheet != bm) cmd.insert(cmd.end(), {"-S", "digits_bitmap", sheet});
    auto scaled = [&](const char *f, double by) {
        auto it = v.find(f);
        if (it == v.end() || it->second.empty()) return;
        cmd.insert(cmd.end(), {"-S", f, std::to_string((int)std::lround(std::atof(it->second.c_str()) * by))});
    };
    scaled("bitmap_digit_width", std::ldexp(1.0, -halvings));
    for (auto f : {"screen_digit_width", "x_offset", "y_offset", "decimal_point_width", "colon_width"}) scaled(f, widths);
    std::string out;
    if (cmd.size() > 4 && run_process(cmd, out) != 0) return false;
    char buf[160];
    snprintf(buf, sizeof buf, "its HUD digits (%s: its own font%s)", fs::path(from_u8(md)).filename().u8string().c_str(),
             halvings ? ", at the size MCC shows them" : "");
    how = buf;
    return true;
}

// The Xbox's own globals (and HUD globals) win over the map's copies, which keeps the Xbox menus, fonts and
// multiplayer sounds. Bring over what is the map's own content:
//  - the HUD name lists, when the map's are longer (its custom weapons' pickup names and vehicles' names);
//  - its own nav point arrows (its scripts name them), after the Xbox's (merge_nav_arrows);
//  - unless "use the Xbox's own weapons" is on: its own HUD digits (map_hud_digits; its weapons' HUDs are the ones
//    drawn then), its grenades, its weapon list (gametype weapon sets) and its
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
            int list_index = 0;
            for (auto [key, label] : {std::pair<const char *, const char *>{"item_message_text", "weapon and item pickup names"},
                                      {"alternate_icon_text", "vehicle names"}}) {
                int li = list_index++;
                std::string ml = mhv[key], xl = xhv[key];
                if (ml.empty() || !fs::exists(xsrc / from_u8(ml))) continue;
                fs::path xldir = xl.empty() ? fs::path() : job.owner(xl);
                int mc = string_count(cfg, xsrc, ml), xc = xldir.empty() ? 0 : string_count(cfg, xldir, xl);
                if (mc <= xc) continue;
                bool ok = true;
                if (in_overlay(ml)) ok = job.to_fixes(ml, xsrc);   // a stock path: the map's version wins
                if (ok && ml != xl) ok = job.edit(xhg, key, ml);
                if (ok) {
                    what.push_back("its " + std::string(label) + " (" + std::to_string(mc) + ", the Xbox has " + std::to_string(xc) + ")");
                    job.pickup.hud_globals = norm_rel(xhg);
                    job.pickup.lists[li] = norm_rel(ml);
                    job.pickup.xbox_count[li] = xc;
                }
            }
            // Pickup icons: when the map's icon sheet has more than the Xbox's, or other pictures at the numbers the
            // map's own weapon and grenade HUDs use (a sheet laid out its own way), use a sheet with both: the Xbox's
            // own icons and button glyphs first, then the map's extra icons, then the map's own pictures for those
            // numbers (the map's HUDs pointed at them), shrunk to the size the Xbox draws.
            std::string mi = norm_rel(mhv["icon_bitmap"]), xi = norm_rel(xhv["icon_bitmap"]);
            if (!mi.empty() && !xi.empty() && fs::exists(xsrc / from_u8(mi)) && fs::exists(job.overlay / from_u8(xi))) {
                BitmapParts mb, xb;
                int mc = mb.read(xsrc / from_u8(mi)) ? (int)mb.nseq : 0, xc = xb.read(job.overlay / from_u8(xi)) ? (int)xb.nseq : 0;
                // The map's own weapon and grenade HUDs (not the Xbox's: those use its sheet) and the icons they show.
                std::map<std::string, int> huds;   // tag -> its icon number
                if (mc > 0 && xc > 0)
                    for (auto &e : fs::recursive_directory_iterator(xsrc)) {
                        std::string ext = e.path().extension().u8string();
                        if (ext != ".weapon_hud_interface" && ext != ".grenade_hud_interface") continue;
                        std::string rel = fs::relative(e.path(), xsrc).generic_u8string();
                        if (in_overlay(rel)) continue;
                        tagfile::Tag t;
                        if (!t.load(e.path())) continue;
                        long long q = t.root().integer("messaging information sequence index");
                        if (q >= 0 && q < mc) huds[rel] = (int)q;
                    }
                std::vector<int> own;   // the map's numbers below the Xbox's count that show another picture than the Xbox's
                {
                    std::set<int> seen;
                    for (auto &[rel, q] : huds)
                        if (q < xc && !seen.count(q)) {
                            seen.insert(q);
                            if (!same_icon(mb, (uint32_t)q, xb, (uint32_t)q)) own.push_back(q);
                        }
                }
                if (mc > xc || !own.empty()) {
                    bool flagged = false;
                    for (auto k : {"flags.half_hud_scale", "flags.force_hud_use_highres_scale"}) {
                        std::string v;
                        run_process({u8(tool(cfg, "invader-edit")), "-t", u8(xsrc), mi, "-G", k}, v);
                        flagged = flagged || v.rfind("1", 0) == 0;
                    }
                    // Halvings to the Xbox's size: its HUD globals' canvas (MCC's 960p doubles everything), and the
                    // icons' high-res scale (the HUD globals' message placement, or the sheet's own flags).
                    bool hg_flag = mhv["scaling_flags.use_high_res_scale"] == "1";
                    int want = hud_960(job, norm_rel(mhg), mcc_hud_layout(job, !job.user_tags.empty() ? job.user_tags : job.source))
                                   ? 1 + (flagged ? 1 : 0) : (flagged || hg_flag ? 1 : 0);
                    fs::path tmp = job.work / "map_icons.bitmap";
                    std::error_code ec;
                    fs::copy_file(xsrc / from_u8(mi), tmp, fs::copy_options::overwrite_existing, ec);
                    int got = !ec && want ? shrink_hud_bitmap_tag(tmp, want) : 0;
                    std::string merged = xi.substr(0, xi.size() - 7) + "_with_map.bitmap";
                    std::vector<int> src;
                    for (int q = xc; q < mc; q++) src.push_back(q);
                    std::map<int, int> renumber;   // the map's number -> the new sheet's
                    for (int q : own) {
                        renumber[q] = xc + (int)src.size();
                        src.push_back(q);
                    }
                    if (!ec && got == want && !in_overlay(merged) &&
                        merge_icon_sheets_from(job.overlay / from_u8(xi), tmp, src, xsrc / from_u8(merged)) &&
                        job.edit(xhg, "icon_bitmap", merged)) {
                        int pointed = 0;
                        for (auto &[rel, q] : huds) {
                            auto it = renumber.find(q);
                            if (it == renumber.end()) continue;
                            fs::path own_dir = fs::exists(job.fixes / from_u8(rel)) ? job.fixes : xsrc;
                            tagfile::EditTag t;
                            if (t.load(own_dir / from_u8(rel)) &&
                                tagfile::set_num(t.root(), "messaging information sequence index", it->second) &&
                                t.save(own_dir / from_u8(rel)))
                                pointed++;
                        }
                        if (mc > xc) what.push_back("its " + std::to_string(mc - xc) + " extra pickup icons (added after the Xbox's own)");
                        if (pointed)
                            what.push_back("its own pickup icons for " + std::to_string(pointed) + " of its weapons and grenades " +
                                           "(its icon sheet is laid out its own way: they showed the Xbox's pictures at those numbers)");
                        job.pickup.hud_globals = norm_rel(xhg);
                        job.pickup.sheet = merged;
                        job.pickup.xbox_sheet_tag = xi;
                        job.pickup.xbox_sheet = job.overlay / from_u8(xi);
                        job.pickup.map_sheet = tmp;
                        job.pickup.xbox_seqs = (uint32_t)xc;
                        job.pickup.src = src;
                    }
                }
            }
            merge_nav_arrows(job, xsrc, mh, xhg, what);
        }
    }

    if (!cfg.xbox_use_stock_versions) {
        // 1b. HUD digits.
        const std::string dk = "interface_bitmaps[0].hud_digits_definition";
        std::string md = norm_rel(mv[dk]), xd = norm_rel(xv[dk]), how;
        if (!md.empty() && md != xd && !in_overlay(md) && fs::exists(xsrc / from_u8(md)) && map_hud_digits(job, xsrc, md, how)) {
            sets.push_back({dk, md});
            what.push_back(how);
        }
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

// Once the map's weapons, items and vehicles are settled (the Xbox's own versions, replacements, vehicles left out),
// the pickup names and icons merge_map_globals added from the map keep only what's still used: weapons' and items'
// pickup names (and a weapon's ammo pickup line after it), the names of units and their seats, the icons (and text)
// of weapon and grenade HUD interfaces and of the HUD globals' button prompts. An unused name is left empty and an
// unused icon without a picture, so every number stays as it was, and none come after the last one used; the icons
// kept are cut out of the map's sheet onto a small texture (merge_icon_sheets), or with none the Xbox's own sheet
// stays as it is. The Xbox's own entries all stay. Nothing changes when a tag that could use one can't be read.
static void trim_pickup_extras(Job &job, const fs::path &xsrc) {
    Job::PickupExtras &px = job.pickup;
    if ((px.lists[0].empty() && px.lists[1].empty() && px.sheet.empty()) || px.hud_globals.empty()) return;
    const Config &cfg = job.cfg;
    // The tags as the build will have them: the map's own are the Xbox copies (xsrc) by now, never the tags they were
    // copied from (the kept extraction, or your tags folder).
    struct Source {
        Job &job;
        fs::path was;
        ~Source() { job.source = was; }
    } source{job, job.source};
    job.source = xsrc;
    std::set<std::string> deps;   // what the map has now: the scenario's tags and the globals'
    for (const std::string root : {job.scenario + ".scenario", std::string("globals/globals.globals")}) {
        std::vector<std::string> cmd{u8(tool(cfg, "invader-dependency")), "-r"};
        for (auto &d : job.dirs()) cmd.insert(cmd.end(), {"-t", u8(d)});
        cmd.push_back(root);
        std::string out;
        run_process(cmd, out);
        std::istringstream ss(out);
        std::string l;
        while (std::getline(ss, l)) deps.insert(norm_rel(l));
        deps.insert(root);
    }
    if (!deps.count(px.hud_globals)) return;
    std::set<long long> used[2];   // item names, alternate icon text
    std::set<uint32_t> icons;
    auto add = [](std::set<long long> &s, long long v) {
        if (v >= 0 && v < 65535) s.insert(v);
    };
    for (auto &dep : deps) {
        std::string ext = fs::path(from_u8(dep)).extension().u8string();
        bool item = ext == ".weapon" || ext == ".equipment", unit = ext == ".biped" || ext == ".vehicle";
        bool hud = ext == ".weapon_hud_interface" || ext == ".grenade_hud_interface";
        if (!item && !unit && !hud && ext != ".garbage" && dep != px.hud_globals) continue;
        fs::path own = job.owner(dep);
        tagfile::Tag t;
        if (own.empty() || !t.load(own / from_u8(dep))) return;   // (can't tell what it uses)
        tagfile::Elem r = t.root();
        if (item) {
            long long i = r.integer("pickup text index");
            add(used[0], i);
            if (ext == ".weapon" && r.block("magazines").size() > 0 && i >= 0 && i < 65535) add(used[0], i + 1);
        }
        if (!hud && dep != px.hud_globals) add(used[1], r.integer("hud text message index"));
        if (unit) {
            tagfile::Block seats = r.block("seats");
            for (size_t k = 0; k < seats.size(); k++) add(used[1], seats[k].integer("hud text message index"));
        }
        if (hud) {
            long long q = r.integer("messaging information sequence index");
            if (q >= 0 && q < 65535) icons.insert((uint32_t)q);
            add(used[1], r.integer("messaging information text index"));
        }
        if (dep == px.hud_globals) {
            tagfile::Block b = r.block("button icons");
            for (size_t k = 0; k < b.size(); k++) {
                long long q = b[k].integer("sequence index");
                if (q >= 0 && q < 65535) icons.insert((uint32_t)q);
                add(used[1], b[k].integer("text index"));
            }
        }
    }

    std::vector<std::string> what;
    size_t tag_bytes = 0, texture_bytes = 0;
    static const char *labels[2] = {"pickup names", "vehicle names"};
    for (int li = 0; li < 2; li++) {
        if (px.lists[li].empty()) continue;
        const std::string &tag = px.lists[li];
        fs::path own = job.owner(tag);
        if (own != job.fixes && own != xsrc) continue;   // (only copies of the app's own)
        tagfile::EditTag t;
        std::vector<tagfile::TagNode> *strings = t.load(own / from_u8(tag)) ? tagfile::block(t.root(), "strings") : nullptr;
        if (!strings) continue;
        int n = (int)strings->size(), xc = std::min(px.xbox_count[li], n), keep_n = xc, kept = 0;
        for (long long i : used[li])
            if (i < n) keep_n = std::max(keep_n, (int)i + 1);
        size_t before = 0, after = 0;
        for (int i = 0; i < n; i++) {
            std::string *d = tagfile::data((*strings)[(size_t)i], "string");
            if (!d) return;
            before += d->size() + 20;
            if (i >= xc && used[li].count(i)) kept++;
            if (i >= xc && i < keep_n && !used[li].count(i) && d->size() > 2) *d = std::string(2, '\0');
            if (i < keep_n) after += d->size() + 20;
        }
        if (after >= before) continue;
        strings->resize((size_t)keep_n);
        if (!t.save(own / from_u8(tag))) continue;
        tag_bytes += before - after;
        what.push_back(std::to_string(kept) + " of its " + std::to_string(n - xc) + " extra " + labels[li]);
    }
    if (!px.sheet.empty()) {
        std::set<uint32_t> keep;
        BitmapParts was, xb;
        fs::path sheet = job.owner(px.sheet);
        if ((sheet == job.fixes || sheet == xsrc) && was.read(sheet / from_u8(px.sheet)) && xb.read(px.xbox_sheet)) {
            for (uint32_t i : icons)
                if (i >= px.xbox_seqs && i < was.nseq && i - px.xbox_seqs < px.src.size()) keep.insert(i);
            bool done = false;
            uint32_t pixels = was.pp;
            if (keep.empty()) {   // none of the map's: the Xbox's own sheet
                done = job.edit(px.hud_globals, "icon_bitmap", px.xbox_sheet_tag);
                pixels = xb.pp;
            } else {
                BitmapParts now;
                done = merge_icon_sheets_from(px.xbox_sheet, px.map_sheet, px.src, sheet / from_u8(px.sheet), &keep) &&
                       now.read(sheet / from_u8(px.sheet));
                if (done) pixels = now.pp;
            }
            if (done) {
                texture_bytes += was.pp > pixels ? was.pp - pixels : 0;
                what.push_back(std::to_string(keep.size()) + " of the " + std::to_string(px.src.size()) + " pickup icons added from its sheet");
            }
        }
    }
    if (what.empty()) return;
    std::string list;
    for (size_t i = 0; i < what.size(); i++) list += (i ? (i + 1 == what.size() ? " and " : ", ") : "") + what[i];
    char saved[96];
    snprintf(saved, sizeof saved, "%.1f KB less tag memory, %.0f KB less of the map's textures", tag_bytes / 1024.0, texture_bytes / 1024.0);
    job.note("[xbox-mp] kept only the pickup names and icons the map's weapons, items and vehicles still use: " + list + " (" +
             saved + ")");
}

// With the map's own weapons and player (not the Xbox's own versions), its first-person arms too: the globals'
// hands (the Xbox's otherwise: merge_map_globals keeps the Xbox's globals). A weapon's first-person animations find
// the hands' bones by name, and one whose animations miss any of them isn't drawn, so the map's arms are used unless
// fewer of the map's weapons would be drawn with them than with the Xbox's. After the models are converted (the
// map's hands are an Xbox model by then).
static void use_map_first_person_hands(Job &job, const fs::path &xsrc) {
    const Config &cfg = job.cfg;
    if (cfg.xbox_use_stock_versions || job.overlay.empty()) return;
    const std::string g = "globals/globals.globals", key = "first_person_interface[0].first_person_hands";
    struct Source {   // (the tags as the build will have them: the map's own are the Xbox copies by now)
        Job &job;
        fs::path was;
        ~Source() { job.source = was; }
    } source{job, job.source};
    job.source = xsrc;
    fs::path gdir = job.owner(g);
    if (gdir.empty() || !fs::exists(xsrc / from_u8(g))) return;
    auto get = [&](const fs::path &dir) {
        std::string v;
        run_process({u8(tool(cfg, "invader-edit")), "-t", u8(dir), g, "-G", key}, v);
        v = norm_rel(v);
        if (!v.empty() && fs::path(from_u8(v)).extension().empty()) v += ".model";
        return v;
    };
    std::string mh = get(xsrc), xh = get(gdir);
    if (mh.empty() || mh == xh || fs::exists(job.overlay / from_u8(mh)) || !fs::exists(xsrc / from_u8(mh))) return;
    auto bones = [&](const std::string &tag) {
        std::set<std::string> r;
        fs::path own = job.owner(tag);
        tagfile::Tag t;
        if (own.empty() || !t.load(own / from_u8(tag))) return r;
        tagfile::Block n = t.root().block("nodes");
        for (size_t i = 0; i < n.size(); i++) r.insert(n[i].str("name"));
        return r;
    };
    std::set<std::string> mb = bones(mh), xb = xh.empty() ? std::set<std::string>() : bones(xh);
    if (mb.empty()) return;
    int with_map = 0, with_xbox = 0, weapons = 0;
    if (mb != xb) {   // other bones: count the weapons each set of arms works with
        std::set<std::string> deps;
        for (const std::string root : {job.scenario + ".scenario", g}) {
            std::vector<std::string> cmd{u8(tool(cfg, "invader-dependency")), "-r"};
            for (auto &d : job.dirs()) cmd.insert(cmd.end(), {"-t", u8(d)});
            cmd.push_back(root);
            std::string out;
            run_process(cmd, out);
            std::istringstream ss(out);
            std::string l;
            while (std::getline(ss, l)) deps.insert(norm_rel(l));
        }
        for (auto &dep : deps) {
            if (fs::path(from_u8(dep)).extension() != ".weapon") continue;
            fs::path own = job.owner(dep);
            tagfile::Tag t;
            if (own.empty() || !t.load(own / from_u8(dep))) continue;
            std::string anim = t.root().ref("first person animations");
            if (anim.empty()) continue;
            std::set<std::string> nodes = bones(anim);
            if (nodes.empty()) continue;
            weapons++;
            with_map += std::includes(nodes.begin(), nodes.end(), mb.begin(), mb.end());
            with_xbox += !xb.empty() && std::includes(nodes.begin(), nodes.end(), xb.begin(), xb.end());
        }
        if (with_map < with_xbox) {
            job.note("[xbox-mp] kept the Xbox's first-person arms: the map's (" + mh + ") have other bones, and only " +
                     std::to_string(with_map) + " of the " + std::to_string(weapons) + " weapons' first-person animations "
                     "move them (" + std::to_string(with_xbox) + " move the Xbox's)");
            return;
        }
    }
    if (!job.edit(g, key, mh)) return;
    std::string why = mb == xb ? "" : " (" + std::to_string(with_map) + " of its " + std::to_string(weapons) +
                                          " weapons' first-person animations move them)";
    job.note("[xbox-mp] using the map's own first-person arms (" + mh + ") with its own weapons and player" + why);
}

// ---------------------------------------------------------------- the scripts window's edits

// A folder's .hsc files (not its subfolders'), by name.
static std::vector<fs::path> hsc_files_in(const fs::path &dir) {
    std::vector<fs::path> v;
    std::error_code ec;
    if (dir.empty()) return v;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec) && hsc::lower(it->path().extension().u8string()) == ".hsc") v.push_back(it->path());
    std::sort(v.begin(), v.end());
    return v;
}

// The scripts window's edits in use (Config::user_scripts): a hash of their names and text, "" if there are none.
static std::string user_scripts_hash(const Config &cfg) {
    std::vector<fs::path> files = hsc_files_in(cfg.user_scripts);
    if (files.empty()) return "";
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const std::string &t) {
        for (unsigned char c : t) h = (h ^ c) * 1099511628211ull;
        h = (h ^ 0x100) * 1099511628211ull;
    };
    for (auto &f : files) {
        std::ifstream in(f, std::ios::binary);
        mix(hsc::lower(f.filename().u8string()));
        mix(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
    }
    char b[24];
    snprintf(b, sizeof b, "%016llx", (unsigned long long)h);
    return b;
}

// The scripts window's edits copied into a data folder's scripts folder (`dir`). False if there are none.
static bool copy_user_scripts(const Config &cfg, const fs::path &dir) {
    std::vector<fs::path> files = hsc_files_in(cfg.user_scripts);
    std::error_code ec;
    if (files.empty()) return false;
    fs::create_directories(dir, ec);
    for (auto &f : files) fs::copy_file(f, dir / f.filename(), fs::copy_options::overwrite_existing, ec);
    return true;
}

// The Xbox build's scripts, in the work folder's xbox_data and compiled from there (job.extra): the scripts window's
// edits (Config::user_scripts), else a build's own data folder's, else the map's as invader-recover gets them back
// out of its scenario tag (in `xsrc`); converted for the Xbox (convert_scripts).
static void xbox_scripts(Job &job, const fs::path &xsrc) {
    const Config &cfg = job.cfg;
    std::error_code ec;
    std::string out;
    fs::path xdata = job.work / "xbox_data";
    fs::path scen_dir = from_u8(job.scenario).parent_path();
    fs::path data = job.user_data;   // (a build's; "Update the map" knows its tags folder, beside it)
    if (data.empty() && !job.user_tags.empty() && fs::is_directory(job.user_tags.parent_path() / "data", ec))
        data = job.user_tags.parent_path() / "data";
    fs::path user_scripts = data / scen_dir / "scripts";
    fs::remove_all(xdata, ec);
    job.extra.clear();
    job.scripts_hash = user_scripts_hash(cfg);
    if (!job.scripts_hash.empty()) {
        copy_user_scripts(cfg, xdata / scen_dir / "scripts");
        job.log("  Scripts: your edits from the scripts window, instead of the map's own");
    } else if (!data.empty() && fs::exists(user_scripts, ec)) {
        fs::create_directories(xdata / scen_dir, ec);
        copy_tree(user_scripts, xdata / scen_dir / "scripts", true, ec);
    } else {
        fs::create_directories(xdata, ec); // invader-recover requires the data folder to exist
        run_process({u8(tool(cfg, "invader-recover")), "-t", u8(xsrc), "-d", u8(xdata), job.scenario + ".scenario"}, out);
    }
    bool any_hsc = false;
    if (fs::exists(xdata, ec))
        for (auto &e : fs::recursive_directory_iterator(xdata, ec))
            if (e.path().extension() == ".hsc") any_hsc = true;
    if (any_hsc) {
        convert_scripts(job, xdata, Game::Xbox);
        job.extra = {"-d", u8(xdata), "-S", "data"};
    }
}

// Another game's build with the scripts window's edits (Config::user_scripts): those compiled, converted for it,
// instead of the map's own (job.extra). Nothing without edits.
static void pc_user_scripts(Job &job, const std::string &scenario) {
    std::error_code ec;
    fs::path sdata = job.work / "script_data";
    fs::remove_all(sdata, ec);
    if (!copy_user_scripts(job.cfg, sdata / from_u8(scenario).parent_path() / "scripts")) return;
    job.log("Scripts: your edits from the scripts window, instead of the map's own");
    convert_scripts(job, sdata, job.target);
    job.extra = {"-d", u8(sdata), "-S", "data"};
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
    drop_dead_placements(job, xsrc);
    restrict_to_xbox_mp(job, xsrc);
    apply_xbox_slots(job, xsrc);
    merge_map_globals(job, xsrc);
    ensure_player_animations(job, xsrc);
    fix_kept_weapons(job, xsrc);
    correct_xbox_tagset(job);
    trim_unobtainable(job, xsrc);
    trim_machines(job, xsrc);
    apply_replacements(job, xsrc);
    tidy_weapon_list(job);
    fix_seat_labels(job, xsrc);
    trim_pickup_extras(job, xsrc);

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
    use_map_first_person_hands(job, xsrc);

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
    // Now that each HUD points at the bitmaps it will really draw (PC copies, meter copies), at their sizes.
    fit_hud_textures(job, xsrc, hud_tags);
    std::map<std::string, std::vector<std::string>> reencoded;   // reason -> sounds
    if (cfg.level_shaders_only) sound_tags.clear();   // (measuring the level: its sounds don't change what's measured)
    if (sound_tags.size() > 5) job.log("Re-encoding " + std::to_string(sound_tags.size()) + " sounds for the Xbox...");
    {
        // (several at once: each is its own tag and its own folder of WAVs)
        std::vector<std::string> list(sound_tags.begin(), sound_tags.end()), reasons(list.size()), whys(list.size());
        std::vector<char> ok(list.size(), 0);
        std::atomic<size_t> next{0};
        auto work = [&] {
            for (size_t i; (i = next++) < list.size();) {
                reasons[i] = sound_reencode_reason(xsrc / from_u8(list[i]));
                ok[i] = reencode_sound_for_xbox(job, xsrc, list[i], whys[i]);
            }
        };
        unsigned n = std::clamp(std::thread::hardware_concurrency(), 1u, 6u);
        std::vector<std::thread> threads;
        for (unsigned t = 1; t < n && t < list.size(); t++) threads.emplace_back(work);
        work();
        for (auto &t : threads) t.join();
        for (size_t i = 0; i < list.size(); i++) {
            if (!ok[i]) {
                err = "Couldn't re-encode " + list[i] + " for Xbox (" + whys[i] + ").";
                return false;
            }
            sounds++;
            reencoded[reasons[i]].push_back(list[i]);
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
        if (cfg.level_shaders_only) break;
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
    xbox_scripts(job, xsrc);
    if (!job.analysis) note_script_tricks(job, xsrc);

    job.source = xsrc;
    return true;
}

// The map's own textures halved until `needed` bytes are saved (the map file's limit, or the texture memory
// target): the ones that save most first, but what's seen up close (vehicles, characters, weapons, first-person
// weapons and arms: model_textures) only after everything else, and never HUD or menu art (fitted to the screen).
// Not `hard` (the texture target, which a map may go over): the close-up ones only if that gets it there.
size_t shrink_textures_to_fit(Job &job, size_t needed, int &count, bool hard) {
    // Only textures the map actually uses.
    std::vector<std::string> deps = content_dependencies(job);
    std::map<std::string, ModelTexture> kinds = model_textures(job, deps);
    struct Cand { bool close; size_t saves; std::string tag; };
    std::vector<Cand> cands;
    for (auto &dep : deps) {
        if (!has_suffix(dep, ".bitmap")) continue;
        if (fs::exists(job.fixes / from_u8(dep))) continue;
        if (!job.overlay.empty() && fs::exists(job.overlay / from_u8(dep))) continue; // stock: leave alone
        std::string l = "/" + dep;
        for (auto &c : l) c = (char)tolower((unsigned char)c);
        if (l.find("/ui/") != std::string::npos || l.find("/hud") != std::string::npos) continue; // HUD and menus
        fs::path p = job.source / from_u8(dep);
        if (!fs::exists(p)) continue;
        size_t s = shrink_bitmap_tag(p, false);
        auto k = kinds.find(dep);
        if (s) cands.push_back({k != kinds.end() && k->second.close, s, dep});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) {
        return a.close != b.close ? !a.close : a.saves != b.saves ? a.saves > b.saves : a.tag < b.tag;
    });
    size_t rest = 0, close = 0;
    for (auto &c : cands) (c.close ? close : rest) += c.saves;
    const bool spare_close = !hard && rest < needed && rest + close < needed;   // (it can't get there either way)
    size_t saved = 0;
    for (auto &c : cands) {
        if (saved >= needed || (c.close && spare_close)) break;
        saved += shrink_bitmap_tag(job.source / from_u8(c.tag), true, job.alpha_tested.count(c.tag) > 0);
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
    if (!user_scripts_hash(job.cfg).empty()) {   // (the scripts window's edits: what's compiled)
        from = job.cfg.user_scripts;
    } else if (!job.user_data.empty() && fs::exists(user_scripts)) {
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

// ---------------------------------------------------------------- old network tricks in a map's scripts

// Halo PC's netcode sends the clients its units and items, but not what the host's scripts do with devices, nor
// whether the host is the host: maps made for it signal with units. A vehicle placed out of play exists on the
// host only (its clients get only what the host sends of the game), so "does it exist" (unit_get_health is -1 for
// none) tells a script it runs on the host; the host drops a vehicle onto a biped placed out of play (or puts a
// gametype-only item spawn on it), and every console's scripts wait for its death, the deaths being sent.
// Precipice syncs its beam that way, Coldsnap learns the game type (Halo 1's scripts can't ask it).
// The original Xbox's system link runs the same game on every console, and OpenCE places the map's own objects on
// every console: there, every console counts as the host, and a trick for the clients does nothing.
struct TrickUnit {
    std::string name, kind, type;   // its name, "vehicle" or "biped", its tag's name
    double x = 0, y = 0, z = 0;
};

// Notes on the tricks in `text` (the map's scripts), given its named units and its player spawns. Empty: none.
static std::vector<std::string> script_trick_notes(const std::string &text, const std::vector<TrickUnit> &units,
                                                   const std::vector<std::array<double, 3>> &spawns) {
    std::vector<std::string> notes;
    std::vector<hsc::Node> top;
    size_t at = 0;
    if (units.empty() || spawns.empty() || !hsc::parse(text, at, top, false)) return notes;
    std::map<std::string, const TrickUnit *> by_name;
    for (auto &u : units) by_name[hsc::lower(u.name)] = &u;
    double lo = 1e30, hi = -1e30;
    for (auto &p : spawns) { lo = std::min(lo, p[2]); hi = std::max(hi, p[2]); }
    // Out of play: well below every spawn (a room under the map), or far across from all of them.
    auto out_of_play = [&](const TrickUnit &u) {
        double closest = 1e30;   // (not "near": a macro on Windows)
        for (auto &p : spawns) closest = std::min(closest, std::hypot(u.x - p[0], u.y - p[1]));
        return u.z < lo - 25 || u.z > hi + 150 || closest > 300;
    };
    struct Use {
        std::set<std::string> calls, scripts;   // what it's given to, in which scripts
        std::set<std::string> exists_in, dies_in, dropped_in;   // scripts that test whether it exists / wait for its
        std::string dropped_onto;                                // death; teleport it (onto a flag)
    };
    std::map<std::string, Use> use;
    std::map<std::string, std::set<std::string>> global_from;    // a global set from whether these exist
    auto num = [](const hsc::Node &n, double &v) {
        if (n.list || n.str || n.atom.empty()) return false;
        char *e = nullptr;
        v = strtod(n.atom.c_str(), &e);
        return e && *e == 0;
    };
    // (unit_get_health X) compared: 'e' X exists, 'n' it doesn't, 'd' it's dead or dying, 0 neither.
    auto health_test = [&](const hsc::Node &cmp, std::string &who) -> char {
        if (!cmp.list || cmp.kids.size() != 3 || cmp.kids[0].list) return 0;
        std::string op = cmp.kids[0].atom;
        const hsc::Node *h = nullptr, *k = nullptr;
        bool flip = false;
        for (int i = 1; i <= 2; i++) {
            const hsc::Node &c = cmp.kids[(size_t)i];
            if (c.list && c.kids.size() == 2 && !c.kids[0].list && hsc::lower(c.kids[0].atom) == "unit_get_health" && !c.kids[1].list) {
                h = &c;
                k = &cmp.kids[(size_t)(3 - i)];
                flip = i == 2;
            }
        }
        double v = 0;
        if (!h || !num(*k, v)) return 0;
        who = hsc::lower(h->kids[1].atom);
        if (flip) op = op == "<" ? ">" : op == ">" ? "<" : op == "<=" ? ">=" : op == ">=" ? "<=" : op;
        if ((op == "!=" && v == -1) || (op == ">" && v == -1) || (op == ">=" && v >= -0.5 && v <= 0)) return 'e';
        if ((op == "=" && v == -1) || (op == "<" && v == 0)) return 'n';
        if (((op == "<=" || op == "<") && v >= 0 && v <= 0.15) || (op == "=" && v == 0)) return 'd';
        return 0;
    };
    std::function<void(const hsc::Node &, const std::string &, const std::set<std::string> &)> walk =
        [&](const hsc::Node &n, const std::string &script, const std::set<std::string> &cond_exists) {
            if (!n.list || n.kids.empty()) return;
            std::string f = n.kids[0].list ? "" : hsc::lower(n.kids[0].atom);
            for (size_t i = 1; i < n.kids.size(); i++) {
                const hsc::Node &a = n.kids[i];
                if (a.list || a.str) continue;
                auto u = by_name.find(hsc::lower(a.atom));
                if (u == by_name.end()) continue;
                use[u->first].calls.insert(f);
                use[u->first].scripts.insert(script);
                if (f == "object_teleport" && i == 1 && n.kids.size() > 2) {
                    use[u->first].dropped_in.insert(script);
                    if (!n.kids[2].list) use[u->first].dropped_onto = n.kids[2].atom;
                }
            }
            std::string who;
            if (char t = health_test(n, who); t && by_name.count(who))
                (t == 'd' ? use[who].dies_in : use[who].exists_in).insert(script);
            // (if <does X exist> ... (set G ...)): G says whether X exists; (set G <does X exist>) too
            std::set<std::string> inner = cond_exists;
            if (f == "if" && n.kids.size() > 2) {
                std::string w;
                if (char t = health_test(n.kids[1], w); (t == 'e' || t == 'n') && by_name.count(w)) inner.insert(w);
            }
            if (f == "set" && n.kids.size() == 3 && !n.kids[1].list) {
                std::string w;
                if (char t = health_test(n.kids[2], w); (t == 'e' || t == 'n') && by_name.count(w))
                    global_from[hsc::lower(n.kids[1].atom)].insert(w);
                for (auto &c : cond_exists) global_from[hsc::lower(n.kids[1].atom)].insert(c);
            }
            for (size_t i = 1; i < n.kids.size(); i++) walk(n.kids[i], script, i == 2 && f == "if" ? inner : cond_exists);
        };
    for (auto &t : top) {
        std::string name = t.list && t.kids.size() > 2 && !t.kids[0].list && hsc::lower(t.kids[0].atom) == "script"
                               ? (t.kids.size() > 3 && !t.kids[2].list ? t.kids[2].atom : "") : "";
        if (t.list && t.kids.size() > 3 && !t.kids[3].list && hsc::lower(t.kids[0].atom) == "script" &&
            (hsc::lower(t.kids[1].atom) == "static" || hsc::lower(t.kids[1].atom) == "stub"))
            name = t.kids[3].atom;   // (script static <type> <name> ...)
        for (auto &k : t.kids) walk(k, name, {});
    }
    // The units out of play the scripts use only to signal (or don't use at all).
    static const std::set<std::string> signal_calls = {"unit_get_health", "object_teleport", "object_destroy",
        "object_create", "object_create_anew", "object_create_containing", "object_create_anew_containing",
        "object_destroy_containing", "unit_kill", "damage_object", "objects_attach", "objects_detach"};
    std::vector<const TrickUnit *> props;
    for (auto &u : units) {
        if (!out_of_play(u)) continue;
        auto it = use.find(hsc::lower(u.name));
        bool only_signals = true;
        if (it != use.end())
            for (auto &c : it->second.calls) only_signals &= signal_calls.count(c) > 0;
        bool tested = it != use.end() && (!it->second.exists_in.empty() || !it->second.dies_in.empty() || !it->second.dropped_in.empty());
        bool unused = it == use.end();
        if (only_signals && (tested || unused)) props.push_back(&u);
    }
    bool any_test = false;
    for (auto *u : props) {
        auto it = use.find(hsc::lower(u->name));
        any_test |= it != use.end() && (!it->second.exists_in.empty() || !it->second.dies_in.empty());
    }
    if (props.empty() || !any_test) return notes;
    auto join = [](const std::set<std::string> &v) {
        std::string s;
        size_t i = 0;
        for (auto &x : v) s += (i++ ? (i == v.size() ? " and " : ", ") : "") + (x.empty() ? std::string("a script") : x);
        return s;
    };
    auto stem = [](const std::string &t) { return fs::path(from_u8(t)).stem().u8string(); };
    int vehicles = 0, bipeds = 0;
    double zmin = 1e30;
    bool all_under = true;
    std::vector<std::string> what;
    bool gametype = false;
    for (auto *u : props) {
        (u->kind == "vehicle" ? vehicles : bipeds)++;
        zmin = std::min(zmin, u->z);
        all_under &= u->z < lo - 25;
        auto it = use.find(hsc::lower(u->name));
        std::string w = u->name + " (" + stem(u->type) + ": ";
        if (it == use.end()) w += "no script uses it";
        else {
            std::vector<std::string> parts;
            const Use &x = it->second;
            if (!x.exists_in.empty()) {
                std::set<std::string> globals;
                for (auto &[g, from] : global_from) if (from.count(hsc::lower(u->name))) globals.insert(g);
                bool one = x.exists_in.size() == 1;
                parts.push_back(join(x.exists_in) + (globals.empty() ? std::string(one ? " tests" : " test") + " whether it exists"
                                                                      : (one ? " sets " : " set ") + join(globals) + " from whether it exists"));
            }
            if (!x.dropped_in.empty())
                parts.push_back(join(x.dropped_in) + " teleport" + (x.dropped_in.size() == 1 ? "s" : "") + " it" +
                                (x.dropped_onto.empty() ? std::string() : " to " + x.dropped_onto));
            if (!x.dies_in.empty()) {
                parts.push_back(join(x.dies_in) + " wait" + (x.dies_in.size() == 1 ? "s" : "") + " for it to die");
                std::string l = hsc::lower(u->name);
                for (const char *g : {"ctf", "slayer", "race", "oddball", "king", "koth"}) gametype |= l == g;
            }
            if (parts.empty()) w += "no script tests it";
            for (size_t i = 0; i < parts.size(); i++) w += (i ? "; " : "") + parts[i];
        }
        what.push_back(w + ")");
    }
    std::string list;
    for (size_t i = 0; i < what.size(); i++) list += (i ? ", " : "") + what[i];
    char where[96];
    snprintf(where, sizeof where, "%.0f", zmin);
    std::string count = std::to_string(props.size()) + " unit" + (props.size() == 1 ? "" : "s") + " (" +
                        (vehicles ? std::to_string(vehicles) + " vehicle" + (vehicles == 1 ? "" : "s") : std::string()) +
                        (vehicles && bipeds ? ", " : "") +
                        (bipeds ? std::to_string(bipeds) + " biped" + (bipeds == 1 ? "" : "s") : std::string()) + ")";
    if (gametype)
        notes.push_back("game type trick: " + count + " placed out of play only for the scripts, which learn the game "
                        "type from which one a gametype-only spawn kills (Halo 1's scripts can't ask it): " + list +
                        ". It works on every console, so it stays; the units cost little.");
    else
        notes.push_back("network trick, potential saving: " + count + " placed out of play (" +
                        (all_under ? "under the map, down to " + std::string(where) + "; players spawn from " +
                                         std::to_string((int)std::floor(lo)) + " to " + std::to_string((int)std::ceil(hi))
                                   : std::string("far from where players spawn")) +
                        ") only for Halo PC's netcode: " + list +
                        ". On the original Xbox every console runs the same game, and OpenCE places " +
                        (props.size() == 1 ? "it" : "them") + " on every console (each counts as the host), so the "
                        "trick does nothing there: the scripts can do without it (the scripts window) and " +
                        (props.size() == 1 ? "the unit" : "the units") + " can go.");
    return notes;
}

// script_trick_notes for `text` (the map's scripts) with the named vehicles and bipeds and the player spawns of the
// scenario tag `scen` (in `sdir`).
static std::vector<std::string> trick_notes_from_tag(const Config &cfg, const fs::path &sdir, const std::string &scen,
                                                     const std::string &text) {
    if (text.find("unit_get_health") == std::string::npos) return {};
    std::map<int, std::string> names;
    std::map<std::string, std::map<int, std::string>> pal;
    struct P { int name = -1, type = -1; double x = 0, y = 0, z = 0; };
    std::map<std::string, std::map<int, P>> placed;
    std::map<int, std::array<double, 3>> spawns;
    static const std::regex obj_re(R"(^object_names\[(\d+)\]\.name$)"), pal_re(R"(^(vehicle|biped)_palette\[(\d+)\]\.name$)"),
        place_re(R"(^(vehicles|bipeds)\[(\d+)\]\.(name|type|position)$)"),
        spawn_re(R"(^player_starting_locations\[(\d+)\]\.position$)");
    auto xyz = [](const std::string &v, double &x, double &y, double &z) {
        return sscanf(v.c_str(), "%lf,%lf,%lf", &x, &y, &z) == 3;
    };
    for (auto &[k, v] : list_values(cfg, sdir, scen)) {
        std::smatch m;
        if (std::regex_match(k, m, obj_re)) names[std::stoi(m[1])] = v;
        else if (std::regex_match(k, m, pal_re)) pal[m[1]][std::stoi(m[2])] = norm_rel(v);
        else if (std::regex_match(k, m, place_re)) {
            P &p = placed[m[1]][std::stoi(m[2])];
            if (m[3] == "position") xyz(v, p.x, p.y, p.z);
            else if (!v.empty() && isdigit((unsigned char)v[0])) (m[3] == "name" ? p.name : p.type) = std::atoi(v.c_str());
        } else if (std::regex_match(k, m, spawn_re)) {
            std::array<double, 3> a{};
            if (xyz(v, a[0], a[1], a[2])) spawns[std::stoi(m[1])] = a;
        }
    }
    std::vector<TrickUnit> units;
    for (auto &[block, list] : placed)
        for (auto &[i, p] : list) {
            if (!names.count(p.name)) continue;
            std::string kind = block == "vehicles" ? "vehicle" : "biped";
            TrickUnit u{names[p.name], kind, pal[kind].count(p.type) ? pal[kind][p.type] : std::string(), p.x, p.y, p.z};
            units.push_back(u);
        }
    std::vector<std::array<double, 3>> sp;
    for (auto &[i, a] : spawns) sp.push_back(a);
    return script_trick_notes(text, units, sp);
}

// The old network tricks in the scripts the Xbox build compiles, in the log (an Xbox conversion's).
static void note_script_tricks(Job &job, const fs::path &xsrc) {
    std::string scen = job.scenario + ".scenario";
    fs::path sdir = fs::exists(job.fixes / from_u8(scen)) ? job.fixes : xsrc;
    for (auto &n : trick_notes_from_tag(job.cfg, sdir, scen, map_script_text(job, sdir, scen))) job.note("[scripts] " + n);
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
// What a multiplayer match can never make, taken out before anything else looks at the map's vehicles and weapons
// (the slots, the weapons & vehicles window), so it isn't offered, slotted or built:
//  - a vehicle placement with none of its multiplayer spawn flags set (no game type's "default" bit, nor the
//    "allowed" one a custom vehicle set uses): Halo PC and MCC never spawn it. The Xbox doesn't read those flags
//    (Warthog reads them only for Custom Edition maps: game_engine_ce_vehicle_placement_allowed), so on the Xbox it
//    would spawn in every game (Precipice's Scorpion). Only when the map's other vehicle placements have flags: a map
//    with none set anywhere never had them (tags from an Xbox map), and spawns them all;
//  - an item spawn point (netgame equipment) whose four game types are all "none": no game type matches it
//    (match_game_type);
//  - a placed vehicle, weapon or equipment marked "not placed: automatically" that has no name: nothing makes it
//    (a script makes an object the map doesn't place by its name).
// Named placements stay (a script can make them: Precipice's beam_attach_hog), and so does everything else. A palette
// entry left with nothing placed from it goes too (not the vehicles' when the map has AI, whose starting locations
// use them), and with the item spawn points' item collections, whatever only they used drops out of the build.
void drop_dead_placements(Job &job, const fs::path &xsrc) {
    const Config &cfg = job.cfg;
    const std::string scen = job.scenario + ".scenario";
    struct Kind { const char *block, *palette; };
    static const Kind kinds[] = {{"vehicles", "vehicle_palette"}, {"weapons", "weapon_palette"}, {"equipment", "equipment_palette"}};
    static const char *spawn_bits[] = {"slayer_default", "ctf_default", "king_default", "oddball_default",
                                       "slayer_allowed", "ctf_allowed", "king_allowed", "oddball_allowed"};
    std::vector<std::string> said;
    int said_items = 0;
    // The scenario's copies: the fixes' (which the build uses) and the prepared one (which the Xbox steps read).
    for (const fs::path &dir : {job.fixes, xsrc}) {
        if (dir.empty() || !fs::exists(dir / from_u8(scen))) continue;
        auto kv = list_values(cfg, dir, scen);
        std::map<std::string, std::string> vals(kv.begin(), kv.end());
        if (vals["type"] != "multiplayer" && vals["type.multiplayer"] != "*") continue;
        auto count = [&](const std::string &block) {
            int n = 0;
            while (vals.count(block + "[" + std::to_string(n) + "].type") || vals.count(block + "[" + std::to_string(n) + "].type_0")) n++;
            return n;
        };
        auto num = [&](const std::string &k) {
            auto it = vals.find(k);
            return it == vals.end() || it->second.empty() || !isdigit((unsigned char)it->second[0]) ? -1 : std::atoi(it->second.c_str());
        };
        const int names = [&] { int n = 0; while (vals.count("object_names[" + std::to_string(n) + "].name")) n++; return n; }();
        const bool has_ai = vals.count("encounters[0].name") > 0;
        std::vector<std::string> edits, renumber, drop_palette;
        std::map<std::string, int> gone;   // palette entry's tag -> placements taken out
        for (const Kind &k : kinds) {
            const std::string b = k.block, pal = k.palette;
            const int n = count(b), np = [&] { int c = 0; while (vals.count(pal + "[" + std::to_string(c) + "].name")) c++; return c; }();
            bool any_flags = false;
            if (b == "vehicles")
                for (int i = 0; i < n && !any_flags; i++)
                    for (const char *f : spawn_bits)
                        if (vals[b + "[" + std::to_string(i) + "].multiplayer_spawn_flags." + f] == "1") { any_flags = true; break; }
            std::vector<int> dead;
            std::vector<int> left(np, 0), was(np, 0);
            for (int i = 0; i < n; i++) {
                const std::string e = b + "[" + std::to_string(i) + "]";
                const int t = num(e + ".type"), nm = num(e + ".name");
                if (t >= 0 && t < np) was[t]++;
                if (nm >= 0 && nm < names) { if (t >= 0 && t < np) left[t]++; continue; }   // named: a script may make it
                bool never = vals[e + ".not_placed.automatically"] == "1";
                if (!never && b == "vehicles" && any_flags) {
                    never = true;
                    for (const char *f : spawn_bits)
                        if (vals[e + ".multiplayer_spawn_flags." + f] == "1") { never = false; break; }
                }
                if (never) {
                    dead.push_back(i);
                    if (t >= 0 && t < np) gone[norm_rel(vals[pal + "[" + std::to_string(t) + "].name"])]++;
                } else if (t >= 0 && t < np) {
                    left[t]++;
                }
            }
            for (auto it = dead.rbegin(); it != dead.rend(); ++it) edits.insert(edits.end(), {"-E", b + "[" + std::to_string(*it) + "]"});
            // Palette entries whose every placement went: renumber what stays placed, then take them out.
            if (b == "vehicles" && has_ai) continue;
            std::vector<int> empty;
            for (int t = 0; t < np; t++) if (was[t] > 0 && left[t] == 0) empty.push_back(t);
            if (empty.empty()) continue;
            int kept_i = 0;
            for (int i = 0; i < n; i++) {
                if (std::binary_search(dead.begin(), dead.end(), i)) continue;
                const int t = num(b + "[" + std::to_string(i) + "].type");
                const int shift = (int)std::count_if(empty.begin(), empty.end(), [&](int u) { return u < t; });
                if (t >= 0 && t < np && shift)
                    renumber.insert(renumber.end(), {"-S", b + "[" + std::to_string(kept_i) + "].type", std::to_string(t - shift)});
                kept_i++;
            }
            for (auto it = empty.rbegin(); it != empty.rend(); ++it) drop_palette.insert(drop_palette.end(), {"-E", pal + "[" + std::to_string(*it) + "]"});
        }
        std::vector<int> dead_items;
        const int ne = count("netgame_equipment");
        std::map<std::string, int> items_gone;
        for (int i = 0; i < ne; i++) {
            const std::string e = "netgame_equipment[" + std::to_string(i) + "]";
            bool none = true;
            for (int t = 0; t < 4 && none; t++) none = vals[e + ".type_" + std::to_string(t) + ".none"] == "*";
            if (!none) continue;
            dead_items.push_back(i);
            items_gone[fs::path(from_u8(norm_rel(vals[e + ".item_collection"]))).stem().u8string()]++;
        }
        for (auto it = dead_items.rbegin(); it != dead_items.rend(); ++it) edits.insert(edits.end(), {"-E", "netgame_equipment[" + std::to_string(*it) + "]"});
        if (edits.empty()) continue;
        std::string out;
        std::vector<std::string> cmd{u8(tool(cfg, "invader-edit")), "-t", u8(dir), scen};
        cmd.insert(cmd.end(), edits.begin(), edits.end());
        cmd.insert(cmd.end(), renumber.begin(), renumber.end());
        cmd.insert(cmd.end(), drop_palette.begin(), drop_palette.end());
        if (run_process(cmd, out) != 0) {
            job.log("  couldn't take out the placements no game can use: " + out);
            continue;
        }
        if (!said.empty() || said_items) continue;   // (the same map in both copies: said once)
        for (auto &[tag, c] : gone) said.push_back(fs::path(from_u8(tag)).stem().u8string() + " (" + std::to_string(c) + " placed)");
        for (auto &[ic, c] : items_gone)
            said.push_back(ic + " (" + std::to_string(c) + " spawn point" + (c == 1 ? "" : "s") + ")");
        said_items = (int)dead_items.size();
    }
    if (said.empty()) return;
    std::string list = said[0];
    for (size_t i = 1; i < said.size(); i++) list += ", " + said[i];
    job.note("[trim] took out placements no game type can ever spawn (vehicles with none of their gametype flags set, "
             "which the Xbox ignores and would spawn in every game; item spawn points with every gametype \"none\"; "
             "unnamed objects marked not placed): " + list + ". A vehicle or weapon nothing else uses drops out of the map "
             "(one in a vehicle slot stays when the map has Race vehicle spawns: Race spawns the slots' vehicles)");
}

// Machines (device_machine) cost the Xbox more every tick than anything else a map places that doesn't move by
// itself: each updates its device, recomputes its dynamic lights and its bones, and an elevator looks for riders
// (Deathtrap's 74 machines: 6.3 ms of every 33 ms tick in Halo 1 Unlimited's perf log, scenery 0.3 ms for 49). Two
// things here change nothing anyone can see:
//  - an elevator nobody can stand on (no collision model) keeps the flag but loses its elevator node: every tick
//    the node moved, the machine searched a sphere its size for bipeds riding it, and only a biped standing on its
//    collision rides it (machine_elevator_carry_riders; the engine skips an elevator without a node). The flag also
//    decides how its lighting is sampled (sideways, and again as it moves: object_lights.c), so it stays.
//    Deathtrap's trees, rocks and airbase pieces scroll past as elevators with no collision: 14 kinds, 62 placed;
//  - a machine that's only a prop becomes scenery: one with no animation graph (it never changes pose), no
//    device functions (its shaders and attachments read nothing a device sets), no device sounds or effects,
//    and not an elevator (sampled as scenery is), placed without a name (no script can reach it) and in no device
//    group (no control or script changes its power or position). Its look, collision, lighting and functions are
//    the scenery's too (scenery_new: like a machine, shadowless; the object's own random constant and other inputs
//    the same), and scenery skips the device's and the dynamic lights' work every tick. A machine kind with
//    other placements keeps them. Precipice's 42 holo panels are machines like this.
void trim_machines(Job &job, const fs::path &xsrc) {
    const std::string scen = job.scenario + ".scenario";
    auto file_of = [](std::string ref, const char *ext) {
        std::replace(ref.begin(), ref.end(), '\\', '/');
        return ref.empty() ? ref : ref + ext;
    };
    std::vector<fs::path> copies;   // the scenario's copies: the fixes' (which the build uses) and the prepared one
    for (const fs::path &d : {job.fixes, xsrc})
        if (!d.empty() && fs::exists(d / from_u8(scen))) copies.push_back(d);
    if (copies.empty()) return;
    tagfile::EditTag sc;
    if (!sc.load(copies[0] / from_u8(scen))) return;
    if (tagfile::get_num(sc.root(), "type") != 1) return;   // multiplayer
    std::vector<tagfile::TagNode> *mpal = tagfile::block(sc.root(), "machine palette");
    if (!mpal || mpal->empty()) return;
    std::vector<std::string> kinds;
    for (auto &p : *mpal) {
        std::string *r = tagfile::ref_path(p, "name");
        kinds.push_back(r ? file_of(*r, ".device_machine") : "");
    }
    auto winning = [&](const std::string &tag) {
        for (const fs::path &d : {job.fixes, xsrc})
            if (!d.empty() && !tag.empty() && fs::exists(d / from_u8(tag))) return d;
        return fs::path();
    };
    std::map<int, int> placed;   // machine palette index -> placements
    if (auto *m = tagfile::block(sc.root(), "machines"))
        for (auto &e : *m) placed[(int)tagfile::get_num(e, "type")]++;

    // 1. Elevators nobody can stand on: no node.
    int lifts = 0, lifts_placed = 0;
    std::vector<std::string> lift_names;
    std::set<int> prop_kinds;   // machine palette entries that are only props (2)
    for (size_t k = 0; k < kinds.size(); k++) {
        const std::string &tag = kinds[k];
        fs::path win = winning(tag);
        if (win.empty()) continue;
        tagfile::EditTag mt;
        if (!mt.load(win / from_u8(tag))) continue;
        tagfile::TagNode &r = mt.root();
        const int mflags = (int)tagfile::get_num(r, "machine flags");
        std::string *coll = tagfile::ref_path(r, "collision model"), *graph = tagfile::ref_path(r, "animation graph");
        const bool no_collision = !coll || coll->empty();
        if ((mflags & 4) && no_collision && (int)tagfile::get_num(r, "elevator node") != 0xFFFF) {
            bool done = false;
            for (const fs::path &d : {job.fixes, xsrc}) {   // every copy (the fixes' wins in the build)
                if (d.empty() || !fs::exists(d / from_u8(tag))) continue;
                tagfile::EditTag c;
                if (!c.load(d / from_u8(tag))) continue;
                tagfile::set_num(c.root(), "elevator node", 0xFFFF);
                done = c.save(d / from_u8(tag)) || done;
            }
            if (done) {
                lifts++;
                lifts_placed += placed[(int)k];
                lift_names.push_back(fs::path(from_u8(tag)).stem().u8string());
            }
        }
        bool prop = (!graph || graph->empty()) && !(mflags & 4) && !(mflags & 2);
        for (const char *f : {"device a in", "device b in", "device c in", "device d in"})
            if (tagfile::get_num(r, f) != 0) prop = false;
        for (const char *f : {"open", "close", "opened", "closed", "depowered", "repowered", "delay effect"}) {
            std::string *p = tagfile::ref_path(r, f);
            if (p && !p->empty()) prop = false;
        }
        if (prop) prop_kinds.insert((int)k);
    }
    if (lifts) {
        std::string list = lift_names[0];
        for (size_t i = 1; i < lift_names.size() && i < 10; i++) list += ", " + lift_names[i];
        if (lift_names.size() > 10) list += " and " + std::to_string(lift_names.size() - 10) + " more";
        job.note("[machines] " + std::to_string(lifts) + " elevator machine" + (lifts == 1 ? "" : "s") + " with no collision (" +
                 std::to_string(lifts_placed) + " placed) no longer look for riders every tick, which only a biped standing "
                 "on their collision can be: " + list + " (their lighting stays as it was)");
    }

    // 2. Props: those placements become scenery.
    if (prop_kinds.empty()) return;
    // A scenery tag to start from, for its header: one the map has, or any of the Xbox's.
    fs::path templ;
    if (auto *sp = tagfile::block(sc.root(), "scenery palette"))
        for (auto &p : *sp) {
            std::string *r = tagfile::ref_path(p, "name");
            std::string t = r ? file_of(*r, ".scenery") : "";
            for (const fs::path &d : {job.fixes, xsrc, job.overlay})
                if (templ.empty() && !d.empty() && !t.empty() && fs::exists(d / from_u8(t))) templ = d / from_u8(t);
        }
    if (templ.empty() && !job.overlay.empty() && fs::exists(job.overlay / "scenery")) {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(job.overlay / "scenery", ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            if (it->path().extension() == ".scenery") { templ = it->path(); break; }
    }
    if (templ.empty()) return;
    std::map<int, std::string> scenery_of;   // machine palette index -> its scenery tag (no extension, '\\')
    for (int k : prop_kinds) {
        const std::string &tag = kinds[k];
        fs::path win = winning(tag);
        tagfile::EditTag mt, st;
        if (win.empty() || !mt.load(win / from_u8(tag)) || !st.load(templ)) continue;
        tagfile::TagNode &m = mt.root();
        tagfile::TagNode n = tagfile::new_node(st.root().sid);
        if (n.fixed.size() < 380 || m.fixed.size() < 380) continue;
        n.fixed.replace(0, 380, m.fixed, 0, 380);   // the object's own fields, laid out alike
        tagfile::set_num(n, "object type", tagfile::get_num(st.root(), "object type"));
        int of = (int)tagfile::get_num(n, "flags");
        if (!((int)tagfile::get_num(m, "machine flags") & 1)) of |= 8;   // not a pathfinding obstacle, as the machine wasn't
        tagfile::set_num(n, "flags", of);
        for (const char *f : {"model", "animation graph", "collision model", "physics", "modifier shader", "creation effect"}) {
            std::string *a = tagfile::ref_path(m, f), *b = tagfile::ref_path(n, f);
            if (a && b) *b = *a;
        }
        for (const char *f : {"attachments", "widgets", "functions", "change colors", "predicted resources"}) {
            std::vector<tagfile::TagNode> *a = tagfile::block(m, f), *b = tagfile::block(n, f);
            if (a && b) *b = *a;
        }
        // The fixed bytes of a reference hold its tag group: the machine's are the object's, the same.
        std::string base = tag.substr(0, tag.size() - std::string(".device_machine").size()), out = base + ".scenery";
        for (int i = 2; fs::exists(xsrc / from_u8(out)) || (!job.overlay.empty() && fs::exists(job.overlay / from_u8(out))); i++)
            out = base + "_prop" + (i > 2 ? std::to_string(i) : "") + ".scenery";
        st.root() = n;
        std::error_code ec;
        fs::create_directories((xsrc / from_u8(out)).parent_path(), ec);
        if (!st.save(xsrc / from_u8(out))) continue;
        std::string ref = out.substr(0, out.size() - 8);
        std::replace(ref.begin(), ref.end(), '/', '\\');
        scenery_of[k] = ref;
    }
    if (scenery_of.empty()) return;
    int moved = 0;
    std::map<std::string, int> moved_of;
    for (size_t ci = 0; ci < copies.size(); ci++) {
        tagfile::EditTag s;
        if (!s.load(copies[ci] / from_u8(scen))) continue;
        tagfile::TagNode &root = s.root();
        std::vector<tagfile::TagNode> *machines = tagfile::block(root, "machines"), *scenery = tagfile::block(root, "scenery"),
                                      *spal = tagfile::block(root, "scenery palette"), *mp = tagfile::block(root, "machine palette");
        if (!machines || !scenery || !spal || !mp) continue;
        const int names = tagfile::block(root, "object names") ? (int)tagfile::block(root, "object names")->size() : 0;
        std::map<int, int> spal_of;   // machine palette index -> scenery palette index
        std::vector<tagfile::TagNode> keep;
        int here = 0;
        std::map<std::string, int> here_of;
        for (auto &e : *machines) {
            const int t = (int)tagfile::get_num(e, "type"), nm = (int)tagfile::get_num(e, "name");
            const bool grouped = (int)tagfile::get_num(e, "power group") != 0xFFFF || (int)tagfile::get_num(e, "position group") != 0xFFFF;
            if (!scenery_of.count(t) || (nm != 0xFFFF && nm < names) || grouped || t >= (int)mp->size()) {
                keep.push_back(e);
                continue;
            }
            if (!spal_of.count(t)) {
                tagfile::TagNode pe = spal->empty() ? tagfile::new_node(tagfile::block_struct(root, "scenery palette")) : (*spal)[0];
                if (spal->empty()) {   // the reference's group ('scen') and no tag ID
                    pe.fixed.replace(0, 4, "scen");
                    pe.fixed.replace(12, 4, std::string(4, '\xff'));
                }
                if (std::string *r = tagfile::ref_path(pe, "name")) *r = scenery_of[t];
                spal_of[t] = (int)spal->size();
                spal->push_back(pe);
            }
            tagfile::TagNode se = tagfile::new_node(tagfile::block_struct(root, "scenery"));
            if (se.fixed.size() < 40 || e.fixed.size() < 40) { keep.push_back(e); continue; }
            se.fixed.replace(0, 40, e.fixed, 0, 40);   // type, name, not placed, permutation, position, rotation, appearance
            se.fixed.replace(32, 4, std::string(4, '\0'));
            tagfile::set_num(se, "type", spal_of[t]);
            scenery->push_back(se);
            here++;
            here_of[fs::path(from_u8(file_of(scenery_of[t], ""))).filename().u8string()]++;
        }
        if (!here) continue;
        // Machine kinds left with nothing placed go from the palette (the rest renumbered).
        std::vector<int> count(mp->size(), 0);
        for (auto &e : keep) {
            int t = (int)tagfile::get_num(e, "type");
            if (t >= 0 && t < (int)count.size()) count[t]++;
        }
        std::vector<int> drop;
        for (auto &[t, si] : spal_of) if (count[t] == 0) drop.push_back(t);
        for (auto &e : keep) {
            int t = (int)tagfile::get_num(e, "type");
            int shift = (int)std::count_if(drop.begin(), drop.end(), [&](int d) { return d < t; });
            if (t >= 0 && t < (int)count.size() && shift) tagfile::set_num(e, "type", t - shift);
        }
        std::sort(drop.rbegin(), drop.rend());
        for (int d : drop) mp->erase(mp->begin() + d);
        *machines = keep;
        if (!s.save(copies[ci] / from_u8(scen))) continue;
        if (!moved) { moved = here; moved_of = here_of; }
    }
    if (!moved) return;
    std::string list;
    for (auto &[n, c] : moved_of) list += (list.empty() ? "" : ", ") + n + " (" + std::to_string(c) + ")";
    job.note("[machines] " + std::to_string(moved) + " machine" + (moved == 1 ? "" : "s") + " that are only props (no animation, "
             "device functions, sounds or effects, no name, in no device group) placed as scenery instead, which the Xbox "
             "doesn't update as a device every tick, looking the same: " + list);
}

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
        const std::string stand_in = "";   // (an empty slot: the list is by position, and nothing gets a weapon from one;
                                           // the gravity rifle used to stand in, a cut weapon that then came along)
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
    job.resource_files.clear();
    for (auto &[n, s] : from) job.resource_files.push_back(s / n);
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

// For tests (cli_test icons): icon_sheet_matches.
std::string compare_icon_sheets(const fs::path &a, const fs::path &b) { return icon_sheet_matches(a, b); }

int scripts_use_cheat_powerups(const std::string &script_text, int script_count) {
    return cheat_powerup_use(script_text, script_count);
}

// ---------------------------------------------------------------- extracted tags, kept

// The last few maps' tags as invader-extract left them (cache_dir/extracted), so converting a map again (the Level
// and Weapons & vehicles windows measure it first, and settings change between conversions) starts from them instead
// of extracting it again. An entry is the map's tags (tags/), what the extraction printed, a list of its files as they
// were (so one changed by mistake is noticed and made again) and the key it was made for: the map file, the
// bitmaps.map/sounds.map/loc.map it read and Invader's extractor (paths, sizes and times). Only the most recent
// kExtractedKept are kept.
namespace {
const int kExtractedKept = 3;
std::mutex g_extracted_lock;
std::map<std::string, int> g_extracted_in_use;   // entry folder -> jobs reading it

struct ExtractedLease {
    std::string dir;
    ~ExtractedLease() {
        std::lock_guard<std::mutex> g(g_extracted_lock);
        if (--g_extracted_in_use[dir] <= 0) g_extracted_in_use.erase(dir);
    }
};
std::shared_ptr<void> lease_extracted(const fs::path &entry) {
    auto l = std::make_shared<ExtractedLease>();
    l->dir = u8(entry);
    g_extracted_in_use[l->dir]++;   // (g_extracted_lock held by the caller)
    return l;
}

std::string file_identity(const fs::path &p) {
    std::error_code ec;
    fs::path a = fs::absolute(p, ec);
    auto size = fs::file_size(p, ec);
    if (ec) return u8(a) + "\tmissing";
    auto time = fs::last_write_time(p, ec);
    return u8(a) + "\t" + std::to_string((unsigned long long)size) + "\t" +
           std::to_string((long long)time.time_since_epoch().count());
}

std::string manifest_of(const fs::path &dir) {
    std::vector<std::string> lines;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        auto size = it->file_size(ec);
        auto time = it->last_write_time(ec);
        lines.push_back(fs::relative(it->path(), dir, ec).generic_u8string() + "\t" + std::to_string((unsigned long long)size) +
                        "\t" + std::to_string((long long)time.time_since_epoch().count()));
    }
    if (ec) return "?";
    std::sort(lines.begin(), lines.end());
    std::string m;
    for (auto &l : lines) m += l + "\n";
    return m;
}

std::string slurp_text(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

fs::path extracted_root(const Config &cfg) { return cfg.cache_dir / "extracted"; }

fs::path entry_for(const Config &cfg, const std::string &key) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : key) { h ^= c; h *= 1099511628211ull; }
    char hex[24];
    snprintf(hex, sizeof hex, "%016llx", (unsigned long long)h);
    return extracted_root(cfg) / hex;
}

// Keeps the most recently used entries (and those in use); removes the rest and any left half made.
void trim_extracted(const Config &cfg) {
    std::error_code ec;
    std::vector<std::pair<long long, fs::path>> entries;
    for (auto it = fs::directory_iterator(extracted_root(cfg), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        fs::path p = it->path();
        if (g_extracted_in_use.count(u8(p))) continue;
        std::error_code e;
        if (!fs::exists(p / "key.txt", e)) { fs::remove_all(p, e); continue; }   // (left half made)
        auto t = fs::last_write_time(p / "used", e);
        entries.push_back({e ? 0 : (long long)t.time_since_epoch().count(), p});
    }
    std::sort(entries.begin(), entries.end(), [](auto &a, auto &b) { return a.first > b.first; });
    int keep = kExtractedKept - (int)g_extracted_in_use.size();
    for (size_t i = std::max(0, keep); i < entries.size(); i++) fs::remove_all(entries[i].second, ec);
}

void mark_used(const fs::path &entry) {
    std::ofstream(entry / "used", std::ios::binary | std::ios::trunc) << "used";
}
}   // namespace

// The extracted tags of `input` from an earlier conversion, if it and what it was read with haven't changed: the
// folder of its tags (in use by `job` until it ends), and what the extraction printed. Empty if there are none.
static fs::path find_extracted(Job &job, const std::string &key, std::string &printed) {
    std::shared_ptr<void> lease;   // (given to the job once the lock is let go: a lease ends under the lock)
    fs::path entry = entry_for(job.cfg, key);
    {
        std::lock_guard<std::mutex> g(g_extracted_lock);
        std::error_code ec;
        if (!fs::exists(entry / "key.txt", ec) || slurp_text(entry / "key.txt") != key) return {};
        if (manifest_of(entry / "tags") != slurp_text(entry / "manifest.txt")) {   // (changed since: made again)
            if (!g_extracted_in_use.count(u8(entry))) fs::remove_all(entry, ec);
            return {};
        }
        printed = slurp_text(entry / "output.txt");
        mark_used(entry);
        lease = lease_extracted(entry);
    }
    job.extracted = lease;
    return entry / "tags";
}

// Extracts the map with `cmd` (invader-extract's arguments, without -t) into a new entry. Returns its tags folder (in
// use by `job`), or empty if the extraction failed (`printed` says why).
static fs::path extract_kept(Job &job, const std::string &key, std::vector<std::string> cmd, std::string &printed) {
    fs::path entry = entry_for(job.cfg, key), tmp = entry;
    tmp += ".tmp";
    std::error_code ec;
    {
        std::lock_guard<std::mutex> g(g_extracted_lock);
        fs::remove_all(tmp, ec);
        fs::create_directories(tmp / "tags", ec);
        g_extracted_in_use[u8(tmp)]++;   // (so trimming leaves it alone while it's made)
    }
    cmd.insert(cmd.begin() + 1, {"-t", u8(tmp / "tags")});
    int rc = run_process(cmd, printed);
    std::shared_ptr<void> lease;   // (given to the job once the lock is let go)
    fs::path tags;
    {
        std::lock_guard<std::mutex> g(g_extracted_lock);
        if (--g_extracted_in_use[u8(tmp)] <= 0) g_extracted_in_use.erase(u8(tmp));
        if (rc != 0) { fs::remove_all(tmp, ec); return {}; }
        std::ofstream(tmp / "output.txt", std::ios::binary) << printed;
        std::ofstream(tmp / "manifest.txt", std::ios::binary) << manifest_of(tmp / "tags");
        std::ofstream(tmp / "key.txt", std::ios::binary) << key;   // (last: the entry is complete)
        mark_used(tmp);
        fs::path keep = tmp;
        if (!g_extracted_in_use.count(u8(entry))) {   // (else an older copy is still being read: this one isn't kept)
            fs::remove_all(entry, ec);
            fs::rename(tmp, entry, ec);
            if (!ec) keep = entry;
        }
        lease = lease_extracted(keep);
        if (keep == entry) trim_extracted(job.cfg);
        tags = keep / "tags";
    }
    job.extracted = lease;
    return tags;
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
    if (in_game == Game::MCC && out_game == Game::Xbox && !an) read_hud_canvases(job, input);
    job.measure = measure;
    fresh_work(job, an ? "analyze" : cfg.level_shaders_only ? "shaders" : "convert");
    std::error_code ec;

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

    // The tags as extracted before, if the map (and what it's read with) hasn't changed since.
    std::string key = "HaloMapPorter extracted 1\n" + std::string(game_name(in_game)) + "\n" + file_identity(input) + "\n" +
                      file_identity(tool(cfg, "invader-extract")) + "\n";
    for (auto &f : job.resource_files) key += file_identity(f) + "\n";
    std::string out;
    fs::path extracted = find_extracted(job, key, out);
    if (!extracted.empty()) {
        log("Reusing the tags extracted from " + input.filename().u8string() + " before (the map hasn't changed)");
    } else {
        // Protected maps have their tag names scrambled, so there's nothing usable to extract.
        std::string path;
        run_process({u8(tool(cfg, "invader-info")), "-T", "scenario_path", u8(input)}, path);
        if (path.find("<protected>") != std::string::npos) {
            r.error = "This map is protected: its author scrambled its tag names so it can't be taken apart, and the "
                      "app needs those to rebuild it for another game. Use an unprotected copy of the map (for example "
                      "the MCC release, or ask the author).";
            return r;
        }
        log("Extracting tags from " + input.filename().u8string() + "...");
        std::vector<std::string> cmd{u8(tool(cfg, "invader-extract"))};
        if (!res_dir.empty()) cmd.insert(cmd.end(), {"-m", u8(res_dir)});
        cmd.push_back(u8(input));
        extracted = extract_kept(job, key, cmd, out);
        if (extracted.empty()) {
            r.error = "Couldn't extract tags from the map:\n" + out.substr(out.size() > 2000 ? out.size() - 2000 : 0);
            if (in_game == Game::CE && res_dir.empty())
                r.error += "\nTip: set your Custom Edition folder in Settings so its bitmaps/sounds.map can be read.";
            return r;
        }
    }
    // An Xbox conversion copies the tags before it changes any; the others change them where they are, so they get
    // a copy of their own.
    if (out_game == Game::Xbox) {
        job.source = extracted;
    } else {
        job.source = job.work / "source_tags";
        if (!copy_tree(extracted, job.source, true, ec)) { r.error = "Couldn't copy the map's tags: " + ec.message(); return r; }
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
    if (out_game != Game::Xbox) pc_user_scripts(job, tag_path_noext(scen));   // (the Xbox's: prepare_for_xbox)
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
    if (out_game != Game::Xbox) pc_user_scripts(job, scenario);   // (the Xbox's: prepare_for_xbox)
    if (out_game != Game::Xbox && job.extra.empty() && fs::exists(scripts)) {
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
    conv.poll_volumes = target == Game::Xbox;
    conv.switches = target == Game::Xbox;
    conv.collect_user_names(text);
    std::vector<std::string> t{text};
    if (!convert_texts(conv, t)[0] && notes) notes->push_back("could not parse");
    if (notes) {
        for (auto &[name, c] : conv.changes) notes->push_back(name + ": " + c.second + " (" + std::to_string(c.first) + "x)");
        for (auto &[name, c] : conv.fixes) notes->push_back(name + ": " + c.second);
        if (target != Game::MCC)
            for (auto &l : script_stack_notes(t)) notes->push_back(l);
        if (target == Game::Xbox)
            for (auto &l : script_cost_notes({text}, t)) notes->push_back(l);
    }
    return t[0];
}

// ---------------------------------------------------------------- the scripts window (scripts_win.hpp)

// The .hsc files under `dir` (its subfolders too), by name.
static std::vector<ScriptFile> read_script_tree(const fs::path &dir) {
    std::vector<ScriptFile> files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec) && hsc::lower(it->path().extension().u8string()) == ".hsc") {
            std::ifstream f(it->path(), std::ios::binary);
            files.push_back({it->path().filename().u8string(),
                             std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>())});
        }
    std::sort(files.begin(), files.end(), [](const ScriptFile &a, const ScriptFile &b) { return hsc::lower(a.name) < hsc::lower(b.name); });
    return files;
}

bool read_map_scripts(const Config &cfg, const fs::path &input, std::vector<ScriptFile> &files, std::string &err,
                      std::vector<std::string> *tricks) {
    files.clear();
    if (!invader_ok(cfg)) { err = "Invader wasn't found. Set it up in Settings."; return false; }
    std::error_code ec;
    if (!fs::exists(input, ec)) { err = "The map isn't there any more: " + u8(input); return false; }
    fs::path tmp = cfg.cache_dir / "work" / "scripts_read";
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp / "tags", ec);
    fs::create_directories(tmp / "data", ec);
    std::string out;
    if (hsc::lower(input.extension().u8string()) == ".scenario") {
        // A scenario in a tags folder: the scripts its build compiles (its data folder's), else the tag's own.
        fs::path tags;
        for (fs::path p = input.parent_path(); !p.empty() && p != p.root_path(); p = p.parent_path())
            if (hsc::lower(p.filename().u8string()) == "tags") { tags = p; break; }
        if (tags.empty()) { err = "The scenario needs to be inside a folder named \"tags\"."; return false; }
        fs::path rel = fs::relative(input, tags), own = tags.parent_path() / "data" / rel.parent_path() / "scripts";
        files = read_script_tree(own);
        if (files.empty())
            run_process({u8(tool(cfg, "invader-recover")), "-t", u8(tags), "-d", u8(tmp / "data"), tag_path_noext(rel) + ".scenario"}, out);
        if (files.empty()) files = read_script_tree(tmp / "data");
        if (tricks) {
            std::string all;
            for (auto &f : files) all += f.text + "\n";
            *tricks = trick_notes_from_tag(cfg, tags, tag_path_noext(rel) + ".scenario", all);
        }
    } else {
        // A map: its scenario tag alone (no resource maps needed for it), and its scripts got back out of that.
        int rc = run_process({u8(tool(cfg, "invader-extract")), "-G", "-t", u8(tmp / "tags"), "-s", "*.scenario", u8(input)}, out);
        fs::path scen = rc == 0 ? find_scenario(tmp / "tags") : fs::path();
        if (scen.empty()) {
            err = "Couldn't read the map's scenario tag:\n" + out.substr(out.size() > 1500 ? out.size() - 1500 : 0);
            fs::remove_all(tmp, ec);
            return false;
        }
        run_process({u8(tool(cfg, "invader-recover")), "-t", u8(tmp / "tags"), "-d", u8(tmp / "data"), tag_path_noext(scen) + ".scenario"}, out);
        files = read_script_tree(tmp / "data");
        if (tricks) {
            std::string all;
            for (auto &f : files) all += f.text + "\n";
            *tricks = trick_notes_from_tag(cfg, tmp / "tags", tag_path_noext(scen) + ".scenario", all);
        }
    }
    fs::remove_all(tmp, ec);
    if (files.empty()) { err = "This map has no scripts."; return false; }
    return true;
}

std::vector<ScriptFile> convert_script_files(const Config &cfg, Game target, const std::vector<ScriptFile> &files,
                                             std::vector<std::string> &notes) {
    // (as convert_scripts does a build's)
    ScriptConverter conv(target);
    conv.rider_ai_off = target == Game::Xbox && cfg.xbox_rider_ai_off;
    conv.poll_volumes = target == Game::Xbox;
    conv.switches = target == Game::Xbox;
    std::vector<std::string> made, texts;
    for (auto &f : files) {
        conv.collect_user_names(f.text);
        made.push_back(f.text);
    }
    texts = made;
    std::vector<bool> ok = convert_texts(conv, texts);
    std::vector<ScriptFile> out = files;
    for (size_t i = 0; i < out.size(); i++) {
        out[i].text = texts[i];
        if (!ok[i]) notes.push_back(out[i].name + " can't be read (see Check), so nothing in it was converted");
    }
    for (auto &[name, c] : conv.changes)
        notes.push_back(name + " (not in " + game_name(target) + "): " + c.second + " (" + std::to_string(c.first) + "x)");
    for (auto &[name, c] : conv.fixes) notes.push_back(name + ": " + c.second);
    std::vector<std::string> more;   // (after an empty line: what they'll do there)
    if (target != Game::MCC) more = script_stack_notes(texts);
    if (target == Game::Xbox)
        for (auto &l : script_cost_notes(made, texts)) more.push_back(l);
    if (!more.empty()) {
        notes.push_back("");
        notes.insert(notes.end(), more.begin(), more.end());
    }
    return out;
}

std::vector<std::string> script_stack_report(const std::vector<std::string> &texts) {
    std::vector<std::string> r;
    ScriptStack st(texts);
    for (auto &[name, u] : st.tops()) {
        std::string l = name + " " + std::to_string(u.bytes);
        for (size_t i = 0; i < u.path.size(); i++) l += (i ? " > " : " ") + u.path[i];
        r.push_back(l);
    }
    return r;
}

std::string script_syntax_problem(const std::string &s) {
    auto line_of = [&](size_t at) {
        return 1 + (int)std::count(s.begin(), s.begin() + (std::ptrdiff_t)std::min(at, s.size()), '\n');
    };
    std::vector<size_t> open;   // where each ( still open is
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == ';') {
            if (i + 1 < s.size() && s[i + 1] == '*') {
                size_t end = s.find("*;", i + 2);
                if (end == std::string::npos)
                    return "Line " + std::to_string(line_of(i)) + ": this comment (;*) is never closed with *;";
                i = end + 1;
            } else {
                while (i < s.size() && s[i] != '\n') i++;
            }
        } else if (c == '"') {
            size_t end = s.find('"', i + 1), eol = s.find('\n', i + 1);
            if (end == std::string::npos || (eol != std::string::npos && eol < end))
                return "Line " + std::to_string(line_of(i)) + ": this \" isn't closed on its line";
            i = end;
        } else if (c == '(') {
            if (open.empty()) {   // (a script or a global: anything else out here means a ) above closed one early)
                size_t w = s.find_first_not_of(" \t\r\n", i + 1), we = w;
                while (we < s.size() && !isspace((unsigned char)s[we]) && s[we] != '(' && s[we] != ')') we++;
                std::string word = w == std::string::npos ? "" : hsc::lower(s.substr(w, we - w));
                if (word != "script" && word != "global")
                    return "Line " + std::to_string(line_of(i)) + ": (" + s.substr(w == std::string::npos ? i : w, std::min<size_t>(we - w, 40)) +
                           " ...) is outside any (script ...) or (global ...): is there a ) too many above it?";
            }
            open.push_back(i);
        } else if (c == ')') {
            if (open.empty()) return "Line " + std::to_string(line_of(i)) + ": this ) closes nothing (one too many)";
            open.pop_back();
            // A script or global starts at the left edge: one that starts while another is still open means a )
            // is missing above it.
        }
        if (i < s.size() && s[i] == '\n' && i + 1 < s.size() && s[i + 1] == '(' && !open.empty()) {
            static const char *tops[] = {"(script ", "(global "};
            for (const char *t : tops)
                if (s.compare(i + 1, strlen(t), t) == 0)
                    return "Line " + std::to_string(line_of(open.back())) + ": this ( is never closed (line " +
                           std::to_string(line_of(i + 1)) + " starts another " + std::string(t + 1, strlen(t) - 2) +
                           " inside it)";
        }
    }
    if (!open.empty()) return "Line " + std::to_string(line_of(open.back())) + ": this ( is never closed";
    std::vector<hsc::Node> top;
    size_t i = 0;
    if (!hsc::parse(s, i, top, false)) return "The scripts can't be read (a bracket or quote out of place)";
    for (auto &n : top)
        if (!n.list) return "Line " + std::to_string(line_of(n.b)) + ": \"" + n.atom.substr(0, 40) +
                            "\" is outside any (script ...) or (global ...)";
    return "";
}

} // namespace porter

namespace porter {
Result update_xbox_models(const Config &cfg, const fs::path &file, const Log &log) {
    Result r;
    Job job(cfg, log, Game::Xbox);
    UpdateFile u;
    if (!load_update(job, file, u, r.error)) return r;
    bool built = fs::exists(u.output);
    log(std::string(built ? "Updating " : "Building ") + u.output.filename().u8string() +
        " (the models' levels of detail and trims; everything else as converted)");
    return run_build(job, u.scenario, u.output, u.share);
}

std::string xbox_prepare_key(const Config &c) {
    std::ostringstream k;
    k << "HMP prepare 2\n";   // (2: automatic picks, the gravity rifle and spare needlers)
    auto path = [&](const char *n, const fs::path &p) { k << n << "=" << u8(p) << "\n"; };
    path("invader", c.invader_dir);
    path("ce", c.ce_dir);
    path("mcc", c.mcc_maps_dir);
    path("ek", c.ek_dir);
    path("xtags", c.xbox_tags_dir);
    path("xmaps", c.xbox_maps_dir);
    path("cache", c.cache_dir);
    k << "shrink=" << c.xbox_shrink << " max=" << c.xbox_max_mb << " texmb=" << c.xbox_texture_target_mb << "\n";
    for (auto &v : c.xbox_vehicle_slots) k << "slot=" << v << "\n";
    k << "allveh=" << c.xbox_keep_all_vehicles << " stock=" << c.xbox_use_stock_versions << "\n";
    for (auto &w : c.xbox_keep_weapons) k << "keep=" << w << "\n";
    for (auto &[w, r] : c.xbox_weapon_replacements) k << "repl=" << w << "=" << r << "\n";
    k << "other=" << c.xbox_other_weapons << "\n";
    for (auto &[w, r] : c.xbox_replace) k << "pick=" << w << "=" << r << "\n";
    k << "anim=" << c.xbox_compress_animations << " coll=" << c.xbox_collision << " portals=" << c.xbox_add_portals
      << c.xbox_replan_portals << "\n";
    k << "fog=" << c.xbox_fog << " " << c.xbox_fog_scale << " " << c.xbox_fog_distance << " " << c.xbox_fog_start << "\n";
    k << "fx=" << c.xbox_effects << " sky=" << c.xbox_sky << " weather=" << c.xbox_weather << " riders=" << c.xbox_rider_ai_off
      << " props=" << c.xbox_detailed_props_off << " lights=" << c.xbox_object_lights << "\n";
    k << "refl=" << c.xbox_drop_reflections << c.xbox_reflection_picks_set << "\n";
    for (auto &t : c.xbox_reflection_picks) k << "drop=" << t << "\n";
    k << "tex=" << c.xbox_max_texture << " lm=" << c.xbox_max_lightmap << " lvl=" << c.xbox_level_texture_mb
      << " obj=" << c.xbox_max_object_texture << "\n";
    // The models' options too: a build from prepared tags does their levels of detail and trims again, but not what
    // was worked out from them while preparing (collision fitted to a model's shape), and tiny scenery only ever
    // raises the switch sizes the preparation set.
    k << "tiny=" << c.xbox_scenery_pixels << " lods=" << c.xbox_make_lods << c.xbox_lod_stock_models << " trim="
      << c.xbox_trim_models << "\n";
    path("lodlib", c.lod_library);
    return k.str();
}

bool can_build_prepared(const fs::path &update, const fs::path &output, bool share, std::string &why) {
    Config cfg;
    const Log quiet = [](const std::string &) {};
    Job job(cfg, quiet, Game::Xbox);
    UpdateFile u;
    if (!load_update(job, update, u, why)) return false;
    auto norm = [](const fs::path &p) {
        std::error_code ec;
        std::string t = u8(fs::weakly_canonical(p, ec).make_preferred());
        if (ec) t = u8(p);
        for (auto &ch : t) ch = (char)tolower((unsigned char)ch);   // (Windows paths: any case)
        return t;
    };
    if (u.output.empty() || norm(u.output) != norm(output)) {
        why = "they were prepared for another map file (" + u8(u.output.filename()) + ")";
        return false;
    }
    if (u.share != share) { why = "they were prepared with the other \"shared bitmaps / sounds\" choice"; return false; }
    if (job.cut) { why = "a build since cut them down to fit (textures halved or vehicles dropped)"; return false; }
    return true;
}

Result build_prepared(const Config &cfg, const fs::path &update, const Log &log) {
    Result r;
    Job job(cfg, log, Game::Xbox);
    UpdateFile u;
    if (!load_update(job, update, u, r.error)) return r;
    log("Building " + u.output.filename().u8string() + " from the tags prepared before (the map and every option they "
        "were prepared with are as they were; the models are done again with the Xbox tab's options and the LODs "
        "window's choices now). The report keeps what preparing them changed.");
    return run_build(job, u.scenario, u.output, u.share);
}

Result redo_xbox_models(const Config &cfg_in, const fs::path &update, const Log &log) {
    Config cfg = cfg_in;
    cfg.xbox_prepare_only = true;
    Result r;
    Job job(cfg, log, Game::Xbox);
    UpdateFile u;
    if (!load_update(job, update, u, r.error)) return r;
    return run_build(job, u.scenario, u.output, u.share);
}

Result redo_model_lods(const Config &cfg, const fs::path &view_file, const std::string &model, const Log &log) {
    Result r;
    LodView v;
    if (!load_lod_view(v, view_file)) { r.error = "The LODs window's file can't be read: convert the map again."; return r; }
    Job job(cfg, log, Game::Xbox);
    UpdateFile u;
    if (!load_update(job, v.update, u, r.error)) return r;
    // The model as the map made it (and a copy made of Bungie's taken out again), then done again on its own.
    std::error_code ec;
    for (auto &[file, bytes] : job.model_originals)
        if (ends_with(file.generic_u8string(), "/" + model) && !job.model_created.count(file))
            std::ofstream(file, std::ios::binary | std::ios::trunc) << bytes;
    for (auto it = job.model_created.begin(); it != job.model_created.end();)
        if (ends_with(it->generic_u8string(), "/" + model)) {
            fs::remove(*it, ec);
            job.model_originals.erase(*it);
            it = job.model_created.erase(it);
        } else {
            ++it;
        }
    log("Making " + model + " again (its trim and lower-detail versions, with its choice now)...");
    std::vector<std::string> deps = job.lod_deps;
    job.only_model = model;
    xbox_scenery_sizes(job, deps);
    xbox_lods(job, deps);
    xbox_scenery(job, deps);
    save_update(job, u.output, u.lods, u.share, job.optimize);
    // Its entry in the window, drawn again (its collision as the conversion had it).
    LodView now = make_lod_view(job, v.map_name);
    auto old = std::find_if(v.models.begin(), v.models.end(), [&](const LodView::Model &m) { return m.tag == model; });
    auto made = std::find_if(now.models.begin(), now.models.end(), [&](const LodView::Model &m) { return m.tag == model; });
    if (made != now.models.end()) {
        LodView::Model m = *made;
        size_t base = v.previews.size();
        for (auto &p : now.previews) v.previews.push_back(std::move(p));
        for (int &k : m.level)
            if (k >= 0) k += (int)base;
        if (m.as_made >= 0) m.as_made += (int)base;
        m.coll_made = m.coll_now = -1;
        if (old != v.models.end()) {
            m.collision = old->collision;
            m.coll_made = old->coll_made;
            m.coll_now = old->coll_now;
            *old = m;
        } else {
            v.models.push_back(m);
        }
    }
    compact_lod_view(v);
    if (!save_lod_view(v, view_file)) { r.error = "The LODs window's file can't be written."; return r; }
    r.ok = true;
    r.lods = view_file;
    r.update = v.update;
    return r;
}

Result convert_map(const Config &cfg, const fs::path &input, Game in_game, Game out_game,
                   const fs::path &output, bool share, const Log &log, ContentAnalysis *measure) {
    return convert_map_impl(cfg, input, in_game, out_game, output, share, log, nullptr, measure);
}
Result build_from_tags(const Config &cfg, const fs::path &scenario_file, Game out_game,
                       const fs::path &output, bool share, const Log &log, ContentAnalysis *measure) {
    return build_from_tags_impl(cfg, scenario_file, out_game, output, share, log, nullptr, measure);
}
std::map<std::string, std::string> automatic_picks(const std::map<std::string, bool> &weapons) { return automatic_picks_impl(weapons); }
std::map<std::string, std::string> effective_picks(const ContentAnalysis &a, const std::map<std::string, std::string> &picks) {
    return effective_picks_impl(a, picks);
}
ContentEstimate estimate_contents(const ContentAnalysis &a, const std::map<std::string, std::string> &picks) {
    return estimate_contents_impl(a, picks);
}
static const char *kAnalysisMagic = "HaloMapPorter contents 2";
static const char kPreviewMagic[8] = {'H', 'M', 'P', 'M', 'O', 'D', 'L', '3'};   // (2: without edges)

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
        vec(m.edges);
    }
    return (bool)f;
}
static bool load_previews(std::vector<ModelPreview> &out, const fs::path &file) {
    std::ifstream f(file, std::ios::binary);
    std::string b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t o = 8;
    bool ok = b.size() >= 12 && memcmp(b.data(), kPreviewMagic, 7) == 0 && (b[7] == '2' || b[7] == '3');
    const bool with_edges = ok && b[7] == '3';
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
        if (with_edges) vec(m.edges);
        // Indices in range, or the model is left out.
        size_t verts = m.pos.size() / 3;
        bool good = ok && m.uv.size() == verts * 2 && m.tris.size() % 3 == 0;
        for (uint32_t x : m.tris) good = good && x < verts;
        for (auto &p : m.parts) good = good && (uint64_t)p.first + p.count <= m.tris.size() / 3 && p.texture < (int)m.textures.size();
        for (auto &t : m.textures) good = good && t.w > 0 && t.h > 0 && t.rgba.size() == (size_t)t.w * t.h * 4;
        if (!m.edges.empty() && m.edges.size() != m.tris.size() / 3) m.edges.clear();
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
    set_automatic_picks(a);   // (from what each row says uses it: the same for a file measured before)
    for (auto &it : a.items)
        if (it.preview < -1 || it.preview >= (int)a.previews.size() || (it.preview >= 0 && a.previews[it.preview].tris.empty()))
            it.preview = -1;
    out = std::move(a);
    return true;
}

// The LODs window's file (text) and its models (file + ".models").
static const char *kLodViewMagic = "HaloMapPorter lods 4";   // (1: without collision; 2: without notes; 3: stock)
bool save_lod_view(const LodView &v, const fs::path &file) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path models = file;
    models += ".models";
    if (!save_previews(v.previews, models)) return false;
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    f.precision(9);
    f << kLodViewMagic << "\n" << v.map_name << "\n" << u8(v.update) << "\n" << v.models.size() << "\n";
    for (auto &m : v.models) {
        f << m.tag << "\n" << m.name << "\n" << m.change << "\n" << m.kind << " " << m.undone << " " << m.placed << " " << m.radius;
        for (double c : m.cutoffs) f << " " << c;
        for (int l : m.level) f << " " << l;
        f << " " << m.trimmed_from << " " << m.as_made << " " << m.coll_made << " " << m.coll_now << " " << m.density << " " << m.had_lods << " " << m.stock;
        f << "\n" << m.collision << "\n" << m.note << "\n";
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
    if (!text(line) || (line != kLodViewMagic && line != "HaloMapPorter lods 3" && line != "HaloMapPorter lods 2" &&
                        line != "HaloMapPorter lods 1"))
        return false;
    const bool v4 = line == kLodViewMagic, v3 = v4 || line == "HaloMapPorter lods 3", v2 = v3 || line == "HaloMapPorter lods 2";
    if (!text(v.map_name)) return false;
    if (v4) {
        if (!text(line)) return false;
        v.update = from_u8(line);
    }
    if (!text(line)) return false;
    n = std::strtoul(line.c_str(), nullptr, 10);
    if (n > 100000) return false;
    v.models.resize(n);
    for (auto &m : v.models) {
        if (!text(m.tag) || !text(m.name) || !text(m.change) || !text(line)) return false;
        std::istringstream ss(line);
        if (!(ss >> m.kind >> m.undone >> m.placed >> m.radius)) return false;
        for (double &c : m.cutoffs) if (!(ss >> c)) return false;
        for (int &l : m.level) if (!(ss >> l)) return false;
        if (!(ss >> m.trimmed_from >> m.as_made)) { m.trimmed_from = 0; m.as_made = -1; }
        if (v2 && !(ss >> m.coll_made >> m.coll_now)) return false;
        if (v3 && !(ss >> m.density >> m.had_lods)) return false;
        if (v4 && !(ss >> m.stock)) return false;
        if (!v4 && m.change == "stock") m.stock = true;
        if (v2 && !text(m.collision)) return false;
        if (v3 && !text(m.note)) return false;
    }
    if (!text(line) || line != "end") return false;
    fs::path models = file;
    models += ".models";
    if (!load_previews(v.previews, models)) v.previews.clear();
    for (auto &m : v.models) {
        for (int &l : m.level)
            if (l < -1 || l >= (int)v.previews.size() || (l >= 0 && v.previews[l].tris.empty())) l = -1;
        for (int *a : {&m.as_made, &m.coll_made, &m.coll_now})
            if (*a < -1 || *a >= (int)v.previews.size() || (*a >= 0 && v.previews[*a].tris.empty())) *a = -1;
    }
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

bool measure_prepared(const Config &cfg_in, const fs::path &update, ContentAnalysis &out, const Log &log) {
    Config cfg = cfg_in;
    cfg.xbox_replace.clear();
    cfg.xbox_prepare_only = false;
    out = ContentAnalysis();
    Job job(cfg, log, Game::Xbox);
    UpdateFile u;
    std::string err;
    if (!load_update(job, update, u, err)) { out.error = err; return false; }
    if (u.picks) { out.error = "The conversion used your weapons & vehicles picks."; return false; }
    const bool optimized = job.optimize;
    job.optimize = false;   // (the window works out what Invader's optimize option saves itself)
    log("Measuring the map for the weapons & vehicles window, from the tags the conversion prepared...");
    // As the window measures a map (the models as the conversion trimmed them, without lower-detail versions), under
    // the scenario's own name; then the scenario as the conversion had it (the Xbox's weapons and vehicles are added
    // to its palettes to be measured as replacements).
    job.analysis = &out;
    fs::path dummy = job.work / "measure" / (fs::path(from_u8(u.scenario)).filename().u8string() + ".map");
    Result r = run_build(job, u.scenario, dummy, u.share);
    std::error_code ec;
    fs::path bak = job.work / "choices_backup.scenario";
    if (!job.choices_scenario.empty() && fs::exists(bak)) fs::copy_file(bak, job.choices_scenario, fs::copy_options::overwrite_existing, ec);
    fs::remove_all(job.work / "measure", ec);
    // (the models were done again with the options now: what's kept for the next build from these tags says so, so
    // it takes them back to as the map made them, copies included)
    if (!job.lod_deps.empty()) save_update(job, u.output, u.lods, u.share, optimized);
    if (!r.ok) {
        out.error = r.error.empty() ? "The map couldn't be measured." : r.error;
        return false;
    }
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
