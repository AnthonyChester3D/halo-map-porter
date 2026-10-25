// Halo Map Porter - reading Halo 1 Unlimited's perf log (see perflog.hpp).
#include "perflog.hpp"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>

namespace perflog {
namespace fs = std::filesystem;

double Log::num(const Row &r, const char *field) const {
    auto it = column.find(field);
    if (it == column.end() || it->second >= (int)r.v.size() || r.v[(size_t)it->second].empty()) return NAN;
    const std::string &s = r.v[(size_t)it->second];
    char *end = nullptr;
    double d = strtod(s.c_str(), &end);
    return end && *end == 0 ? d : NAN;
}

std::string Log::text(const Row &r, const char *field) const {
    auto it = column.find(field);
    return it == column.end() || it->second >= (int)r.v.size() ? std::string() : r.v[(size_t)it->second];
}

static std::vector<std::string> split(const std::string &s) {
    std::vector<std::string> out;
    size_t a = 0;
    for (;;) {
        size_t b = s.find(',', a);
        out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return out;
}

bool load(const fs::path &csv, Log &out, std::string &err) {
    out = Log();
    out.file = csv;
    std::ifstream f(csv, std::ios::binary);
    if (!f) { err = "Couldn't open " + csv.filename().u8string() + "."; return false; }
    std::string line;
    bool ours = false;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;
        if (line[0] == '#') {
            if (line.rfind("# Halo 1 Unlimited perf log", 0) == 0) ours = true;
            else if (line.rfind("# build ", 0) == 0) out.build = line.substr(8);
            else if (line.rfind("# map ", 0) == 0) out.map = line.substr(6, line.find(' ', 6) == std::string::npos ? std::string::npos : line.find(' ', 6) - 6);
            else if (line.rfind("# started ", 0) == 0) out.started = line.substr(10, line.find(';') == std::string::npos ? std::string::npos : line.find(';') - 10);
            else if (line.rfind("# fields ", 0) == 0) {
                out.fields = split(line.substr(9));
                out.column.clear();
                for (size_t i = 0; i < out.fields.size(); i++) out.column[out.fields[i]] = (int)i;
            }
            continue;
        }
        if (out.fields.empty()) continue;
        std::vector<std::string> v = split(line);
        if (v.empty() || v[0].size() != 1) continue;
        if (v.size() > out.fields.size()) {   // (a tour view's label may have commas: the rest is the label)
            std::string last = v[out.fields.size() - 1];
            for (size_t i = out.fields.size(); i < v.size(); i++) last += "," + v[i];
            v.resize(out.fields.size());
            v.back() = last;
        }
        Row r;
        r.kind = v[0][0];
        r.v = std::move(v);
        out.rows.push_back(std::move(r));
    }
    if (!ours || out.fields.empty() || !out.has("avg_ms")) {
        err = csv.filename().u8string() + " isn't a Halo 1 Unlimited perf log (perf_log = 1 in opence.ini writes them, a CSV "
              "a map session, in the perf folder beside the XBE).";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- figures

namespace {

std::string fmt(const char *f, ...) {
    char b[1024];
    va_list a;
    va_start(a, f);
    vsnprintf(b, sizeof b, f, a);
    va_end(a);
    return b;
}

std::string thousands(double v) {
    long long n = (long long)std::llround(v);
    std::string s = std::to_string(n < 0 ? -n : n), out;
    for (size_t i = 0; i < s.size(); i++) {
        if (i && (s.size() - i) % 3 == 0) out += ',';
        out += s[i];
    }
    return (n < 0 ? "-" : "") + out;
}

std::string short_name(std::string tag) {   // altis\levels\...\warthog\warthog -> warthog
    size_t p = tag.find_last_of("\\/");
    return p == std::string::npos ? tag : tag.substr(p + 1);
}

// The rows of one kind worth reading: in a game, drawing the level (not the loading screen or a menu).
std::vector<const Row *> playing(const Log &log, char kind) {
    std::vector<const Row *> out;
    for (auto &r : log.rows) {
        if (r.kind != kind) continue;
        double frames = log.num(r, "frames"), ms = log.num(r, "avg_ms"), level = log.num(r, "level_triangles");
        if (!(frames > 0) || !(ms > 0) || ms > 250 || !(level > 0)) continue;   // (a load's hitch, a menu)
        out.push_back(&r);
    }
    return out;
}

double avg(const Log &log, const std::vector<const Row *> &rows, const char *field) {
    double s = 0;
    int n = 0;
    for (auto *r : rows) {
        double v = log.num(*r, field);
        if (std::isnan(v)) continue;
        s += v;
        n++;
    }
    return n ? s / n : 0;
}

double sum_avg(const Log &log, const std::vector<const Row *> &rows, std::initializer_list<const char *> fields) {
    double s = 0;
    for (auto f : fields) s += avg(log, rows, f);
    return s;
}

double ticks_per_frame(const Log &log, const std::vector<const Row *> &rows) {
    double t = 0, f = 0;
    for (auto *r : rows) {
        double gt = log.num(*r, "game_ticks"), fr = log.num(*r, "frames");
        if (std::isnan(gt) || std::isnan(fr)) continue;
        t += gt;
        f += fr;
    }
    return f > 0 && t > 0 ? t / f : 1.0;
}

// What limits a second (or a tour view).
enum Limit { kHeld, kDraw, kGame, kGpu };
Limit limit_of(const Log &log, const Row &r) {
    double ms = log.num(r, "avg_ms");
    if (ms <= 34.0) return kHeld;
    double present = log.num(r, "present_ms"), gpu = log.num(r, "gpu_busy_pct");
    if ((!std::isnan(present) && present >= 2.0) || (!std::isnan(gpu) && gpu >= 92)) return kGpu;
    double draw = log.num(r, "draw_ms"), tick = log.num(r, "tick_ms");
    return (std::isnan(tick) || draw >= tick) ? kDraw : kGame;
}
const char *limit_name(Limit l) {
    switch (l) {
    case kHeld: return "30 fps";
    case kDraw: return "CPU drawing";
    case kGame: return "CPU game";
    default: return "graphics chip";
    }
}

struct Part { const char *label; std::vector<const char *> fields; };
// Drawing's steps (they add up to the drawing time; the lights' level passes are part of their steps).
const std::vector<Part> &draw_parts() {
    static const std::vector<Part> p = {
        {"objects", {"render_objects_ms"}},
        {"dynamic lights (diffuse and shine passes)", {"render_diffuse_lights_ms", "render_specular_lights_ms"}},
        {"dynamic lights' set-up", {"render_lights_ms"}},
        {"the level's lightmaps", {"render_structure_lightmaps_ms"}},
        {"the level's textures", {"render_structure_diffuse_textures_ms"}},
        {"working out what's visible", {"render_visibility_ms"}},
        {"see-through objects and effects", {"render_transparent_ms"}},
        {"particles", {"render_particles_ms"}},
        {"the HUD", {"render_hud_ms"}},
        {"lens flares", {"render_lens_flares_ms"}},
        {"fog", {"render_structure_fog_ms"}},
        {"object shadows", {"render_object_shadows_ms"}},
        {"weather", {"render_weather_ms"}},
        {"reflections", {"render_structure_reflections_ms"}},
        {"first-person weapon", {"render_first_person_weapon_ms"}},
        {"sky", {"render_sky_ms"}},
        {"the level's see-through surfaces", {"render_structure_transparent_geometry_ms"}},
        {"decals", {"render_decals_ms"}},
        {"contrails", {"render_contrails_ms"}},
        {"detail objects", {"render_detail_objects_ms"}},
    };
    return p;
}
// A tick's parts: the objects by type (each with what's attached to it), and the tick's other steps.
const std::vector<Part> &tick_parts() {
    static const std::vector<Part> p = {
        {"machines (with what's attached to them)", {"obj_machine_ms"}},
        {"players and other bipeds", {"obj_biped_ms"}},
        {"vehicles (with their riders)", {"obj_vehicle_ms"}},
        {"weapons", {"obj_weapon_ms"}},
        {"scenery", {"obj_scenery_ms"}},
        {"projectiles", {"obj_projectile_ms"}},
        {"equipment", {"obj_equipment_ms"}},
        {"controls", {"obj_control_ms"}},
        {"light fixtures", {"obj_light_fixture_ms"}},
        {"sound scenery", {"obj_sound_scenery_ms"}},
        {"garbage", {"obj_garbage_type_ms"}},
        {"AI", {"tick_ai_ms"}},
        {"effects", {"tick_effects_ms"}},
        {"scripts", {"tick_hs_ms"}},
        {"units", {"tick_units_ms"}},
        {"players", {"tick_players_before_ms", "tick_players_after_ms"}},
        {"the game's rules and scores", {"tick_game_engine_ms"}},
        {"first person", {"tick_first_person_ms"}},
        {"the HUD", {"tick_hud_ms"}},
        {"antennas", {"tick_antennas_ms"}},
    };
    return p;
}

struct Item { std::string label; double ms; };
std::vector<Item> biggest(const Log &log, const std::vector<const Row *> &rows, const std::vector<Part> &parts, double scale,
                          size_t most) {
    std::vector<Item> out;
    for (auto &p : parts) {
        double ms = 0;
        for (auto f : p.fields) ms += avg(log, rows, f);
        if (ms * scale >= 0.05) out.push_back({p.label, ms * scale});
    }
    std::sort(out.begin(), out.end(), [](const Item &a, const Item &b) { return a.ms > b.ms; });
    if (out.size() > most) out.resize(most);
    return out;
}

void table(std::string &s, const std::vector<Item> &items) {
    for (auto &i : items) s += fmt("    %-44s %6.1f\n", i.label.c_str(), i.ms);
}

}   // namespace

// ---------------------------------------------------------------- the report

std::string report(const Log &log) {
    std::string s;
    std::vector<const Row *> rows = playing(log, 'P'), tour = playing(log, 'T');
    s += "Perf log: " + (log.map.empty() ? log.file.stem().u8string() : log.map) + (log.build.empty() ? "" : ", build " + log.build) + "\n";
    s += "  " + log.file.filename().u8string() + (log.started.empty() ? "" : ", started " + log.started) + "\n";
    if (rows.empty()) {
        s += "\nNo seconds of play in it (only loading or menus).\n";
        return s;
    }
    std::vector<const Row *> held, slow;
    int by_limit[4] = {};
    for (auto *r : rows) {
        Limit l = limit_of(log, *r);
        by_limit[l]++;
        (l == kHeld ? held : slow).push_back(r);
    }
    s += fmt("  %d seconds of play%s.\n\n", (int)rows.size(), tour.empty() ? "" : fmt(", %d camera tour views", (int)tour.size()).c_str());

    // Frame rate.
    s += "Frame rate\n";
    s += fmt("  At 30 fps (the most the Xbox draws, a frame every 33.3 ms): %.0f%% of the seconds.\n", 100.0 * held.size() / rows.size());
    if (!slow.empty()) {
        double worst = 0;
        for (auto *r : slow) worst = std::max(worst, log.num(*r, "avg_ms"));
        double a = avg(log, slow, "avg_ms");
        s += fmt("  The slower seconds: %d, %.1f ms a frame on average (%.0f fps); the slowest %.1f ms (%.0f fps).\n", (int)slow.size(), a,
                 1000.0 / a, worst, 1000.0 / worst);
    } else {
        s += "  It held 30 fps the whole time.\n";
    }

    // What limits it.
    s += "\nWhat limits it, second by second\n";
    s += fmt("    held at 30 fps                 %4d\n", by_limit[kHeld]);
    s += fmt("    the processor, drawing         %4d\n", by_limit[kDraw]);
    s += fmt("    the processor, game ticks      %4d\n", by_limit[kGame]);
    s += fmt("    the graphics chip              %4d\n", by_limit[kGpu]);
    s += "  (A slow second is the graphics chip's when the processor waited for it, or the chip was busy 92% of the time or\n"
         "  more; otherwise it's the processor's, drawing or running the game's ticks, whichever took longer.)\n";

    // The graphics chip. Its busy share counts its wait for the screen's refresh too (a frame finished early waits for
    // it): Sidewinder, a stock map that runs smoothly, shows 81% at 30 fps with the processor mostly idle. So its work
    // a frame is read where the processor is late (no waiting): busy share x frame time.
    const bool gpu_known = log.has("gpu_busy_pct") && avg(log, rows, "gpu_busy_pct") > 0;
    double gpu_held = held.empty() ? 0 : avg(log, held, "gpu_busy_pct"), gpu_work = 0;
    if (gpu_known) {
        s += "\nThe graphics chip\n";
        if (!slow.empty()) {
            for (auto *r : slow) gpu_work += log.num(*r, "gpu_busy_pct") * log.num(*r, "avg_ms") / 100;
            gpu_work /= slow.size();
            s += fmt("  In the slower seconds (the processor late, nothing waiting for the screen) it had about %.0f ms of work a "
                     "frame: %s\n", gpu_work, gpu_work > 33.3 ? "more than a 30 fps frame has." :
                     gpu_work > 28 ? "close to the 33.3 ms a 30 fps frame has." : "room to spare at 30 fps.");
        }
        if (!held.empty())
            s += fmt("  At 30 fps it was busy %.0f%% of the time (that counts its wait for the screen's refresh; Sidewinder, a stock map "
                     "that runs smoothly: 81%%).\n", gpu_held);
    }

    // The slower seconds (or a typical one), in detail.
    const std::vector<const Row *> &look = slow.size() >= 3 ? slow : rows;
    const bool slowly = &look == &slow;
    double tpf = ticks_per_frame(log, look);
    double draw = avg(log, look, "draw_ms"), tick = avg(log, look, "tick_ms"), other = avg(log, look, "other_ms"),
           present = avg(log, look, "present_ms"), ms = avg(log, look, "avg_ms");
    s += slowly ? fmt("\nThe slower seconds (%d, %.1f ms a frame)\n", (int)look.size(), ms)
                : fmt("\nA typical second (%.1f ms a frame)\n", ms);
    s += fmt("  The processor, each frame: drawing %.1f ms, the game %.1f ms (%.2f ticks a frame: %.1f ms a tick), the rest %.1f ms",
             draw, tick, tpf, tick / tpf, other);
    s += present >= 0.5 ? fmt(", waiting for the graphics chip %.1f ms.\n", present) : std::string(".\n");
    double need = tick / tpf + draw + other;
    if (slowly) {
        s += fmt("  At 30 fps it has 33.3 ms a frame with one tick in it: this needs about %.1f ms, %.1f ms over.\n", need,
                 std::max(0.0, need - 33.3));
        if (tpf > 1.05)
            s += "  (A slow frame runs more than one tick to keep the game's time: cutting a frame's cost cuts its ticks too.)\n";
    }
    s += "  Drawing, its biggest parts (ms a frame):\n";
    table(s, biggest(log, look, draw_parts(), 1.0, 8));
    s += "  A game tick, its biggest parts (ms a tick):\n";
    table(s, biggest(log, look, tick_parts(), 1.0 / tpf, 6));
    {   // the costliest objects (each row names its top 3)
        std::map<std::string, std::pair<double, int>> top;
        for (auto *r : look)
            for (int k = 1; k <= 3; k++) {
                std::string name = log.text(*r, ("top_object_" + std::to_string(k)).c_str());
                double v = log.num(*r, ("top_object_" + std::to_string(k) + "_ms").c_str());
                if (name.empty() || std::isnan(v)) continue;
                top[name].first += v;
                top[name].second++;
            }
        std::vector<std::pair<std::string, std::pair<double, int>>> v(top.begin(), top.end());
        std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.second.first > b.second.first; });
        if (!v.empty()) {
            s += "  The costliest objects to update, with what's attached to them (ms a tick, when among a second's top 3):\n";
            for (size_t i = 0; i < v.size() && i < 4; i++)
                s += fmt("    %-44s %6.1f   (%d of %d seconds)\n", short_name(v[i].first).c_str(),
                         v[i].second.first / v[i].second.second / tpf, v[i].second.second, (int)look.size());
        }
    }
    double otri = avg(log, look, "dynamic_triangles"), ltri = avg(log, look, "level_triangles"), drawn = avg(log, look, "objects_drawn"),
           lights = avg(log, look, "dynamic_lights"), parts = avg(log, look, "particles_alive"), drops = avg(log, look, "weather_particles");
    s += fmt("  On screen: %s objects drawn (%s triangles), %s of the level's triangles, %.1f dynamic lights, %s particles",
             thousands(drawn).c_str(), thousands(otri).c_str(), thousands(ltri).c_str(), lights, thousands(parts).c_str());
    s += drops > 0 ? ", " + thousands(drops) + " rain or snow drops.\n" : std::string(".\n");
    if (slowly && !held.empty())
        s += fmt("  (At 30 fps: %s objects, %s triangles, %s of the level's, %.1f dynamic lights.)\n", thousands(avg(log, held, "objects_drawn")).c_str(),
                 thousands(avg(log, held, "dynamic_triangles")).c_str(), thousands(avg(log, held, "level_triangles")).c_str(),
                 avg(log, held, "dynamic_lights"));

    // What to try.
    s += "\nWhat to try\n";
    int hints = 0;
    auto hint = [&](const std::string &h) { s += "  - " + h + "\n"; hints++; };
    double lightpass = sum_avg(log, look, {"render_diffuse_lights_ms", "render_specular_lights_ms"}),
           shine = avg(log, look, "render_specular_lights_ms");
    if (lightpass >= 1.0)
        hint(fmt("Dynamic lights: %.1f ms of drawing a frame (%.1f in view on average), %.1f ms of it their shine. The map's objects' "
                 "lights are always on: Level window, Lights (No specular drops the shine pass; Off drops the light).",
                 lightpass, lights, shine));
    double objects = avg(log, look, "render_objects_ms");
    if (objects >= 3.0)
        hint(fmt("Objects: %.1f ms drawing %s triangles a frame. The LODs window trims the busiest models' most detailed "
                 "versions and makes lower ones; the Level window's Hide scenery stops drawing the far-off specks.",
                 objects, thousands(otri).c_str()));
    double level = sum_avg(log, look, {"render_structure_lightmaps_ms", "render_structure_diffuse_textures_ms", "render_structure_fog_ms"});
    if (level >= 4.0)
        hint(fmt("The level: %.1f ms drawing %s of its triangles a frame (each twice or more: its lightmap, then its textures). "
                 "Portals (Xbox tab) let walls hide what's behind them; fog that's opaque sooner (Level window) ends the view.",
                 level, thousands(ltri).c_str()));
    double vis = avg(log, look, "render_visibility_ms");
    if (vis >= 2.0) hint(fmt("Working out what's visible: %.1f ms a frame. A level divided into rooms (portals, Xbox tab) usually needs less.", vis));
    double refl = avg(log, look, "render_structure_reflections_ms");
    if (refl >= 0.8) hint(fmt("Reflections: %.1f ms a frame. The Level window can drop the ones that cost most for what they show.", refl));
    double see = sum_avg(log, look, {"render_transparent_ms", "render_particles_ms", "render_weather_ms", "render_contrails_ms"});
    if (see >= 3.0)
        hint(fmt("See-through objects, effects, particles and weather: %.1f ms a frame (%s particles%s). Level window: Effects "
                 "(Lighter) and Weather.", see, thousands(parts).c_str(), drops > 0 ? (", " + thousands(drops) + " drops").c_str() : ""));
    double flares = avg(log, look, "render_lens_flares_ms");
    if (flares >= 1.0) hint(fmt("Lens flares: %.1f ms a frame (the map's lights' flares, each tested for being in view).", flares));
    double shadows = avg(log, look, "render_object_shadows_ms");
    if (shadows >= 1.0) hint(fmt("Object shadows: %.1f ms a frame.", shadows));
    double machines = avg(log, look, "obj_machine_ms") / tpf, mcount = avg(log, look, "obj_machine_count") / tpf;
    if (machines >= 2.0)
        hint(fmt("Machines: %.1f ms a tick (%.0f updated), with anything attached to them. Every machine is updated every tick, "
                 "moving or not; one that never moves can be scenery (the app does that for props).", machines, mcount));
    double children = avg(log, look, "obj_children_ms") / tpf, ccount = avg(log, look, "obj_children_count") / tpf;
    if (children >= 1.0)
        hint(fmt("Objects attached to others: %.1f ms a tick (%.0f of them; counted in their parent's figure above too). Units "
                 "given AI and riding along: Level window, Riders (No AI).", children, ccount));
    double ai = avg(log, look, "tick_ai_ms") / tpf;
    if (ai >= 1.0) hint(fmt("AI: %.1f ms a tick (%.0f actors).", ai, avg(log, look, "actors_active")));
    double hs = avg(log, look, "tick_hs_ms") / tpf;
    if (hs >= 0.4) hint(fmt("Scripts: %.1f ms a tick.", hs));
    double fx = avg(log, look, "tick_effects_ms") / tpf;
    if (fx >= 1.0) hint(fmt("Effects: %.1f ms a tick (%.0f alive). Level window: Effects.", fx, avg(log, look, "effects_alive")));
    double weapons = avg(log, look, "obj_weapon_ms") / tpf;
    if (weapons >= 1.0) hint(fmt("Weapons: %.1f ms a tick (%.0f updated).", weapons, avg(log, look, "obj_weapon_count") / tpf));
    if (gpu_known && gpu_work > 28)
        hint(fmt("The graphics chip: about %.0f ms of work a frame when it's slow, so once the processor catches up it's the "
                 "next limit. What lowers both: fewer light passes, fewer triangles, less see-through on screen.", gpu_work));
    if (!hints) s += "  Nothing stands out.\n";

    // The camera tour.
    if (!tour.empty()) {
        s += "\nCamera tour (a view a line: ms a frame, what limits it, its biggest costs in ms)\n";
        for (auto *r : tour) {
            std::vector<const Row *> one{r};
            Limit l = limit_of(log, *r);
            double rt = ticks_per_frame(log, one);
            std::vector<Item> items = biggest(log, one, draw_parts(), 1.0, 3);
            items.push_back({"the game (a tick)", log.num(*r, "tick_ms") / rt});
            std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.ms > b.ms; });
            items.resize(std::min<size_t>(items.size(), 3));
            std::string costs;
            for (auto &i : items) costs += (costs.empty() ? "" : ", ") + i.label + " " + fmt("%.1f", i.ms);
            std::string label = log.text(*r, "tour_label");
            s += fmt("  %s.%s %-22s %5.1f ms %-14s %s\n", log.text(*r, "tour_stop").c_str(), log.text(*r, "tour_view").c_str(),
                     label.substr(0, 22).c_str(), log.num(*r, "avg_ms"), limit_name(l), costs.c_str());
        }
    }
    return s;
}

std::string compare(const std::vector<Log> &logs) {
    std::string s = "Side by side\n";
    s += fmt("  %-34s %6s %6s %7s %7s %7s %7s %5s %8s\n", "log", "secs", "30fps", "avg ms", "slow ms", "draw", "game", "chip", "obj tris");
    for (auto &log : logs) {
        std::vector<const Row *> rows = playing(log, 'P'), slow;
        for (auto *r : rows) if (limit_of(log, *r) != kHeld) slow.push_back(r);
        const std::vector<const Row *> &look = slow.empty() ? rows : slow;
        std::string name = log.file.stem().u8string();
        if (name.size() > 34) name = name.substr(name.size() - 34);
        s += fmt("  %-34s %6d %5.0f%% %7.1f %7s %7.1f %7.1f %4.0f%% %8s\n", name.c_str(), (int)rows.size(),
                 rows.empty() ? 0.0 : 100.0 * (rows.size() - slow.size()) / rows.size(), avg(log, rows, "avg_ms"),
                 slow.empty() ? "-" : fmt("%.1f", avg(log, slow, "avg_ms")).c_str(), avg(log, look, "draw_ms"), avg(log, look, "tick_ms"),
                 avg(log, look, "gpu_busy_pct"), thousands(avg(log, look, "dynamic_triangles")).c_str());
    }
    s += "  (draw, game, chip and triangles: in the slower seconds, or all of them when none was slow)\n";
    // The tour, view for view.
    std::map<std::pair<int, int>, std::vector<double>> views;
    std::map<std::pair<int, int>, std::string> labels;
    for (size_t i = 0; i < logs.size(); i++)
        for (auto *r : playing(logs[i], 'T')) {
            std::pair<int, int> k{atoi(logs[i].text(*r, "tour_stop").c_str()), atoi(logs[i].text(*r, "tour_view").c_str())};
            auto &v = views[k];
            v.resize(logs.size(), NAN);
            v[i] = logs[i].num(*r, "avg_ms");
            labels[k] = logs[i].text(*r, "tour_label");
        }
    if (!views.empty()) {
        s += "\n  Camera tour, ms a frame at each view (in the order above):\n";
        for (auto &[k, v] : views) {
            std::string line = fmt("    %d.%d %-22s", k.first, k.second, labels[k].substr(0, 22).c_str());
            for (double x : v) line += std::isnan(x) ? "      -" : fmt(" %6.1f", x);
            s += line + "\n";
        }
    }
    return s;
}

}   // namespace perflog
