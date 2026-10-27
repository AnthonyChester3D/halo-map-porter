// Linux test driver for the conversion engine.
// usage: cli_test convert <in.map> <MCC|CE|Xbox> <out.map>
//        cli_test build <file.scenario> <MCC|CE|Xbox> <out.map>
#include "core.hpp"
#include "render.hpp"
#include "perflog.hpp"
#include "showcase.hpp"
#include "levelshaders.hpp"
#include <cmath>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <iostream>
#include <sstream>
#include <cstring>
namespace porter {
std::string compare_icon_sheets(const std::filesystem::path &a, const std::filesystem::path &b);   // core.cpp (tests)
}
using namespace porter;

// XPREVIEW=<folder>: each item's model drawn to <folder>/<name>.ppm (two views side by side), for checking.
static void write_previews(const ContentAnalysis &a) {
    const char *dir = getenv("XPREVIEW");
    if (!dir) return;
    for (auto &it : a.items) {
        if (it.preview < 0) continue;
        const int w = 320, h = 280;
        std::vector<uint32_t> p1, p2;
        render::View v1, v2;
        v2.yaw = 3.6f;
        v2.pitch = 0.15f;
        render::draw(a.previews[it.preview], v1, w, h, p1);
        render::draw(a.previews[it.preview], v2, w, h, p2);
        std::string name = it.name;
        for (auto &c : name) if (c == '/' || c == ' ') c = '_';
        FILE *f = fopen((std::string(dir) + "/" + name + ".ppm").c_str(), "wb");
        if (!f) continue;
        fprintf(f, "P6\n%d %d\n255\n", w * 2, h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w * 2; x++) {
                uint32_t q = x < w ? p1[(size_t)y * w + x] : p2[(size_t)y * w + x - w];
                unsigned char rgb[3] = {(unsigned char)(q >> 16), (unsigned char)(q >> 8), (unsigned char)q};
                fwrite(rgb, 1, 3, f);
            }
        fclose(f);
    }
}

static void print_items(const ContentAnalysis &a) {
    for (auto &it : a.items)
        printf("%s %-34s %7.3f MB mem %7.3f MB file  %-40s %s seats=%d [%s] piece_of=%s preview=%d tris=%d tex=%zu\n",
               it.vehicle ? "V" : "W", it.name.c_str(), it.memory_mb, it.file_mb, it.used_by.c_str(), it.in_map ? "" : "[stock]",
               it.seats, it.seat_roles.c_str(), it.piece_of.c_str(), it.preview,
               it.preview >= 0 ? a.previews[it.preview].triangles() : 0,
               it.preview >= 0 ? a.previews[it.preview].textures.size() : (size_t)0);
}

static Game parse(const std::string &s) {
    if (s == "MCC") return Game::MCC;
    if (s == "CE") return Game::CE;
    if (s == "Xbox") return Game::Xbox;
    return Game::Unknown;
}

int main(int argc, char **argv) {
    if (argc >= 3 && std::string(argv[1]) == "pace") {   // cli_test pace <showcase.txt>: each shot timed again, its speed as played
        std::ifstream in(argv[2], std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()), err;
        showcase::Showcase sc;
        if (!showcase::read(text, sc, &err)) { std::cout << "can't read: " << err << "\n"; return 1; }
        for (auto &s : sc.shots) {
            showcase::retime(s);
            std::cout << s.label << " (" << showcase::pace_name(s.pace) << ", " << s.keys.size() << " keys):";
            showcase::V3 last = showcase::at(s, 0).eye;
            for (double t = 0.25; t <= s.seconds + 1e-9; t += 0.25) {
                showcase::V3 e = showcase::at(s, t).eye;
                double d = std::hypot(std::hypot(e[0] - last[0], e[1] - last[1]), e[2] - last[2]);
                printf(" %.1f", d / 0.25);
                last = e;
            }
            std::cout << "\n";
        }
        return 0;
    }
    if (argc >= 3 && std::string(argv[1]) == "showcase") {   // cli_test showcase <level.lsh> [out.txt]: the generated flythrough
        lshade::Analysis a;
        lshade::Preview p;
        if (!lshade::load(a, p, argv[2])) { std::cout << "can't read " << argv[2] << "\n"; return 1; }
        std::string report;
        showcase::Showcase sc = showcase::generate(p, &report);
        std::string text = showcase::write(sc, a.map_name, "generated from the level");
        showcase::Showcase back;
        std::string err;
        bool ok = showcase::read(text, back, &err) && back.shots.size() == sc.shots.size();
        std::cout << report << "\n" << p.marks.size() << " marks, " << p.boxes.size() << " boxes; read back " << (ok ? "ok" : "FAILED " + err) << "\n";
        if (argc >= 4) std::ofstream(argv[3], std::ios::binary) << text;
        else std::cout << text;
        return ok ? 0 : 1;
    }
    if (argc >= 3 && std::string(argv[1]) == "perflog") { // cli_test perflog <log.csv> [more.csv...]: the perf log report
        std::vector<perflog::Log> logs;
        for (int i = 2; i < argc; i++) {
            perflog::Log l;
            std::string err;
            if (!perflog::load(argv[i], l, err)) { std::cout << "FAILED: " << err << "\n"; return 1; }
            std::cout << perflog::report(l) << "\n";
            logs.push_back(std::move(l));
        }
        if (logs.size() > 1) std::cout << perflog::compare(logs);
        return 0;
    }
    if (argc >= 3 && std::string(argv[1]) == "stack") { // cli_test stack <file.hsc>...: script_stack_report
        std::vector<std::string> texts;
        for (int i = 2; i < argc; i++) {
            std::ifstream in(argv[i], std::ios::binary);
            texts.push_back(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
        }
        for (auto &l : script_stack_report(texts)) std::cout << l << "\n";
        return 0;
    }
    if (argc == 4 && std::string(argv[1]) == "hsc") { // cli_test hsc <MCC|CE|Xbox> <file.hsc>
        std::ifstream in(argv[3], std::ios::binary);
        std::string t((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::vector<std::string> notes;
        std::cout << convert_script_text(parse(argv[2]), t, &notes);
        for (auto &n : notes) std::cerr << "NOTE " << n << "\n";
        return 0;
    }
    if (argc == 4 && std::string(argv[1]) == "cheats") { // cli_test cheats <file.hsc|-> <script count>
        std::string t;
        if (std::string(argv[2]) != "-") {
            std::ifstream in(argv[2], std::ios::binary);
            t.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        std::cout << scripts_use_cheat_powerups(t, std::atoi(argv[3])) << "\n";
        return 0;
    }
    if (argc < 5) { std::cerr << "bad args\n"; return 2; }
    Config cfg;
    cfg.invader_dir = getenv("INVADER");
    cfg.ce_dir = getenv("CE_DIR");
    if (getenv("MCC_MAPS")) cfg.mcc_maps_dir = getenv("MCC_MAPS");
    if (getenv("EK_DIR")) cfg.ek_dir = getenv("EK_DIR");
    if (getenv("XBOX_TAGS")) cfg.xbox_tags_dir = getenv("XBOX_TAGS");
    if (getenv("XBOX_MAPS")) cfg.xbox_maps_dir = getenv("XBOX_MAPS");
    cfg.xbox_shrink = getenv("XSHRINK") != nullptr;
    if (getenv("XLIMIT")) cfg.xbox_max_mb = std::atof(getenv("XLIMIT"));
    if (getenv("XSTOCK")) cfg.xbox_use_stock_versions = std::string(getenv("XSTOCK")) != "0";
    if (getenv("XOTHER")) cfg.xbox_other_weapons = getenv("XOTHER");
    if (getenv("XMAXTEX")) cfg.xbox_max_texture = std::atoi(getenv("XMAXTEX"));
    if (getenv("XMAXLM")) cfg.xbox_max_lightmap = std::atoi(getenv("XMAXLM"));
    for (int i = 0; i < 3; i++) {
        std::string k = "XV" + std::to_string(i);
        if (getenv(k.c_str())) cfg.xbox_vehicle_slots[i] = getenv(k.c_str());
    }
    cfg.xbox_keep_all_vehicles = getenv("XALLVEH") != nullptr;
    cfg.xbox_lod_stock_models = getenv("XLODSTOCK") != nullptr;
    cfg.xbox_make_lods = getenv("XNOLOD") == nullptr;
    if (getenv("XTRIM")) cfg.xbox_trim_models = atoi(getenv("XTRIM"));             // 0 as made, 1 trim, 2 harder, 3 density
    cfg.xbox_prepare_only = getenv("XPREPARE") != nullptr;   // as far as the LODs window
    if (getenv("XANIM")) cfg.xbox_compress_animations = atoi(getenv("XANIM"));     // 0 as made, 1 compress, 2 harder
    if (getenv("XCOLL")) cfg.xbox_collision = atoi(getenv("XCOLL"));               // 0 as made, 1 simpler, 2 harder
    if (getenv("LODLIB")) cfg.lod_library = getenv("LODLIB");
    if (getenv("XKEEP")) { // comma-separated weapon tags to keep
        std::string w = getenv("XKEEP"), item;
        std::stringstream ss(w);
        while (std::getline(ss, item, ',')) cfg.xbox_keep_weapons.push_back(item);
    }
    cfg.cache_dir = getenv("CACHE");
    if (getenv("XPICKS")) { // from=to;from=  (empty to = none)
        std::string w = getenv("XPICKS"), item;
        std::stringstream ss(w);
        while (std::getline(ss, item, ';')) {
            size_t eq = item.find('=');
            if (eq != std::string::npos) cfg.xbox_replace[item.substr(0, eq)] = item.substr(eq + 1);
        }
    }
    if (getenv("XNOPORTALS")) cfg.xbox_add_portals = false;
    if (getenv("XREPLAN")) cfg.xbox_replan_portals = true;
    if (const char *x = getenv("XFOG")) {   // 0 automatic, 1 as made, 2[:scale] opaque at scale x its distance,
        cfg.xbox_fog = atoi(x);              // 3:units[:start] opaque at units (starting at start)
        if (const char *c = strchr(x, ':')) {
            (cfg.xbox_fog == 3 ? cfg.xbox_fog_distance : cfg.xbox_fog_scale) = atof(c + 1);
            if (const char *c2 = strchr(c + 1, ':'); c2 && cfg.xbox_fog == 3) cfg.xbox_fog_start = atof(c2 + 1);
        }
    }
    if (getenv("XSCENERY")) cfg.xbox_scenery_pixels = atof(getenv("XSCENERY"));   // pixels across
    if (getenv("XWEATHER")) cfg.xbox_weather = atoi(getenv("XWEATHER"));          // 1 lighter, 2 half, 3 off
    if (getenv("XSKY")) cfg.xbox_sky = atoi(getenv("XSKY"));                      // 1 the skies as made
    if (getenv("XFX")) cfg.xbox_effects = atoi(getenv("XFX"));                    // 1 as made, 2 lighter
    cfg.xbox_rider_ai_off = getenv("XRIDERS") != nullptr;                          // riders without AI
    if (getenv("XSCRIPTS")) cfg.user_scripts = getenv("XSCRIPTS");                 // the scripts window's edits (a folder)
    cfg.xbox_detailed_props_off = getenv("XPROPS") != nullptr;                     // detailed props without collision
    if (getenv("XLIGHTS")) cfg.xbox_object_lights = atoi(getenv("XLIGHTS"));     // lights on the map's objects: 1 no specular, 2 off
    cfg.write_perf_tour = getenv("XTOUR") != nullptr;                              // a perf log camera tour beside the map
    if (const char *x = getenv("XREFL")) {   // keep | tag,tag,... (the level shaders window's picks)
        std::string w = x, item;
        if (w == "keep") cfg.xbox_drop_reflections = false;
        else {
            cfg.xbox_reflection_picks_set = true;
            std::stringstream ss(w);
            while (std::getline(ss, item, ',')) if (!item.empty()) cfg.xbox_reflection_picks.insert(item);
        }
    }
    Log log = [](const std::string &s) { std::cout << s << std::endl; };
    std::string mode = argv[1];
    Result r;
    if (mode == "icons") {   // cli_test icons <sheet.bitmap> <other sheet.bitmap> x: which pictures match
        std::cout << porter::compare_icon_sheets(argv[2], argv[3]);
        return 0;
    }
    if (mode == "traits") {   // cli_test traits <built.map> x x
        MapTraits t;
        std::string err;
        if (!analyze_map_traits(cfg, argv[2], t, &err)) { std::cout << "FAILED: " << err << "\n"; return 1; }
        std::cout << "Gametypes: " << t.gametypes << "\nPlayers: " << t.pmin << "-" << t.pmax << "\nGenres:";
        for (auto &g : t.genres) std::cout << " [" << g << "]";
        std::cout << "\nwhy: " << t.why_genres << "\nplayers: " << t.players_why << "\n";
        return 0;
    }
    if (mode == "shaders") {   // cli_test shaders <in.map|scenario> x x: the level shaders window's measurement
        std::string name, err;
        Game in = detect_game(argv[2], &name);
        std::filesystem::path file;
        if (!analyze_level_shaders(cfg, argv[2], in, file, err, log)) { std::cout << "FAILED: " << err << "\n"; return 1; }
        std::cout << "level shaders file: " << file.u8string() << "\n";
        return 0;
    }
    // XDUMP=<file>: a measurement's tags, their sizes and what they use, and its items (for the window test)
    auto dump = [](const ContentAnalysis &an) {
        if (!getenv("XDUMP")) return;
        {
            FILE *f = fopen(getenv("XDUMP"), "wb");
            fprintf(f, "%s\n%.6f %.6f %.6f %.6f %.6f %.6f %.6f\n", an.map_name.c_str(), an.limit_mb, an.file_limit_mb, an.used_mb,
                    an.optimize_saves_mb, an.file_mb, an.fixed_mb, an.fixed_file_mb);
            fprintf(f, "%zu\n", an.tags.size());
            for (size_t i = 0; i < an.tags.size(); i++) {
                fprintf(f, "%s\n%llu %llu %zu", an.tags[i].c_str(), (unsigned long long)an.bytes[i], (unsigned long long)an.file_bytes[i], an.deps[i].size());
                for (int d : an.deps[i]) fprintf(f, " %d", d);
                fprintf(f, " %zu", an.geometry[i].size());
                for (auto &g : an.geometry[i]) fprintf(f, " %u %u", g.first, g.second);
                fprintf(f, "\n");
            }
            fprintf(f, "%zu", an.roots.size());
            for (int r : an.roots) fprintf(f, " %d", r);
            fprintf(f, "\n%zu\n", an.items.size());
            for (auto &it : an.items)
                fprintf(f, "%s\n%s\n%s\n%d %d %.6f %.6f\n", it.tag.c_str(), it.name.c_str(), it.used_by.c_str(), it.vehicle, it.in_map, it.memory_mb, it.file_mb);
            fclose(f);
        }
    };
    if (mode == "analyze") {   // cli_test analyze <in.map|scenario> x x
        std::string name;
        Game in = detect_game(argv[2], &name);
        ContentAnalysis an;
        if (!analyze_xbox_contents(cfg, argv[2], in, an, log)) { std::cout << "FAILED: " << an.error << "\n"; return 1; }
        printf("used %.3f MB (optimize saves %.3f), limit %.2f; file %.2f MB (limit %.0f); fixed %.3f; tags %zu roots %zu\n",
               an.used_mb, an.optimize_saves_mb, an.limit_mb, an.file_mb, an.file_limit_mb, an.fixed_mb, an.tags.size(), an.roots.size());
        print_items(an);
        write_previews(an);
        if (getenv("XSAVE")) save_content_analysis(an, getenv("XSAVE"));
        dump(an);
        if (!cfg.xbox_replace.empty()) {
            ContentEstimate e = estimate_contents(an, cfg.xbox_replace);
            printf("with picks: %.3f MB mem (%.3f after optimize), %.2f MB file\n", e.memory_mb, e.memory_optimized_mb, e.file_mb);
        }
        return 0;
    }
    if (mode == "measured") {   // cli_test measured <cache>/work/convert/update.txt x x: weapons & vehicles from the prepared tags
        ContentAnalysis an;
        if (!measure_prepared(cfg, argv[2], an, log)) { std::cout << "FAILED: " << an.error << "\n"; return 1; }
        printf("used %.3f MB (optimize saves %.3f), limit %.2f; file %.2f MB (limit %.0f); fixed %.3f; tags %zu roots %zu\n",
               an.used_mb, an.optimize_saves_mb, an.limit_mb, an.file_mb, an.file_limit_mb, an.fixed_mb, an.tags.size(), an.roots.size());
        print_items(an);
        dump(an);
        return 0;
    }
    if (mode == "scripts") {   // cli_test scripts <map|scenario> <out folder> x: the scripts window's texts
        std::vector<ScriptFile> files;
        std::string err;
        if (!read_map_scripts(cfg, argv[2], files, err)) { std::cout << "ERROR: " << err << "\n"; return 1; }
        std::vector<std::string> notes;
        auto conv = convert_script_files(cfg, Game::Xbox, files, notes);
        std::filesystem::create_directories(argv[3]);
        for (size_t i = 0; i < files.size(); i++) {
            std::ofstream(std::filesystem::path(argv[3]) / ("map_" + files[i].name), std::ios::binary) << files[i].text;
            std::ofstream(std::filesystem::path(argv[3]) / ("xbox_" + files[i].name), std::ios::binary) << conv[i].text;
            std::string p = script_syntax_problem(files[i].text);
            std::cout << files[i].name << ": " << files[i].text.size() << " bytes, syntax: " << (p.empty() ? "ok" : p) << "\n";
        }
        for (auto &n : notes) std::cout << "  note: " << n << "\n";
        return 0;
    }
    if (mode == "tricks") {   // cli_test tricks <map|scenario> x x: the old network tricks in its scripts
        std::vector<ScriptFile> files;
        std::vector<std::string> tricks;
        std::string err;
        if (!read_map_scripts(cfg, argv[2], files, err, &tricks)) { std::cout << "(" << err << ")\n"; return 0; }
        for (auto &t : tricks) std::cout << t << "\n";
        if (tricks.empty()) std::cout << "(none)\n";
        return 0;
    }
    if (mode == "syntax") {   // cli_test syntax <file.hsc> x x: script_syntax_problem
        std::ifstream in(argv[2], std::ios::binary);
        std::string t((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string p = script_syntax_problem(t);
        std::cout << (p.empty() ? "ok" : p) << "\n";
        return 0;
    }
    if (mode == "prepkey") {   // cli_test prepkey x x x: xbox_prepare_key with these options
        std::cout << xbox_prepare_key(cfg);
        return 0;
    }
    if (mode == "update") {   // cli_test update <cache>/work/convert/update.txt x x: the LODs window's "Update the map"
        r = update_xbox_models(cfg, argv[2], log);
    } else if (mode == "buildprep") {   // cli_test buildprep <update.txt> <output.map> x: Convert from the tags Unpack prepared
        std::string why;
        if (!can_build_prepared(argv[2], argv[3], true, why)) { std::cout << "CAN'T: " << why << "\n"; return 1; }
        r = build_prepared(cfg, argv[2], log);
    } else if (mode == "redomodels") {   // cli_test redomodels <update.txt> x x: the models done again for the LODs window
        r = redo_xbox_models(cfg, argv[2], log);
    } else if (mode == "redo") {   // cli_test redo <lods view file> <model tag> x: the LODs window's "Make again"
        r = redo_model_lods(cfg, argv[2], argv[3], log);
    } else if (mode == "convert") {
        std::string name;
        Game in = detect_game(argv[2], &name);
        std::cout << "Detected " << game_name(in) << " map '" << name << "'\n";
        ContentAnalysis m;
        r = convert_map(cfg, argv[2], in, parse(argv[3]), argv[4], true, log, getenv("XMEASURE") ? &m : nullptr);
        if (!m.map_name.empty()) {   // measured for the weapons & vehicles window
            save_content_analysis(m, getenv("XMEASURE"));
            ContentAnalysis back;
            bool ok = load_content_analysis(back, getenv("XMEASURE"));
            printf("measured: used %.3f MB (optimize saves %.3f); file %.2f MB; tags %zu items %zu; reload %s\n", m.used_mb,
                   m.optimize_saves_mb, m.file_mb, m.tags.size(), m.items.size(),
                   ok && back.tags == m.tags && back.items.size() == m.items.size() && back.deps == m.deps && back.geometry == m.geometry &&
                   back.previews.size() == m.previews.size() ? "ok" : "MISMATCH");
            print_items(back);
            write_previews(back);
        }
    } else {
        r = build_from_tags(cfg, argv[2], parse(argv[3]), argv[4], true, log);
    }
    if (!r.lods.empty()) {   // the LODs window's file: what's in it (XLODPREVIEW=<folder>: each model's levels drawn)
        LodView v;
        bool ok = load_lod_view(v, r.lods);
        printf("lods: %s, %zu models, %zu drawings\n", ok ? "read back" : "UNREADABLE", v.models.size(), v.previews.size());
        for (auto &m : v.models) {
            printf("  %-30s %c %-9s%s placed %d radius %.2f cutoffs %g/%g/%g/%g/%g levels %d %d %d %d %d tris", m.name.c_str(), m.kind,
                   m.change.c_str(), m.undone ? " UNDONE" : "", m.placed, m.radius, m.cutoffs[0], m.cutoffs[1], m.cutoffs[2],
                   m.cutoffs[3], m.cutoffs[4], m.level[0], m.level[1], m.level[2], m.level[3], m.level[4]);
            for (int l : m.level) printf(" %d", l >= 0 ? v.previews[l].triangles() : 0);
            printf("\n");
            if (const char *dir = getenv("XLODPREVIEW")) {
                std::vector<int> shown;
                for (int l : m.level) if (l >= 0 && std::find(shown.begin(), shown.end(), l) == shown.end()) shown.push_back(l);
                const int w = 300, h = 240;
                std::vector<std::vector<uint32_t>> px(shown.size());
                render::View rv;
                rv.wire = getenv("XWIRE") != nullptr;
                for (size_t k = 0; k < shown.size(); k++) render::draw(v.previews[shown[k]], rv, w, h, px[k]);
                std::string n = m.name;
                for (auto &c : n) if (c == '/' || c == ' ') c = '_';
                FILE *f = fopen((std::string(dir) + "/" + n + ".ppm").c_str(), "wb");
                if (!f || shown.empty()) { if (f) fclose(f); continue; }
                fprintf(f, "P6\n%d %d\n255\n", w * (int)shown.size(), h);
                for (int y = 0; y < h; y++)
                    for (size_t k = 0; k < shown.size(); k++)
                        for (int x = 0; x < w; x++) {
                            uint32_t q = px[k][(size_t)y * w + x];
                            unsigned char c[3] = {(unsigned char)(q >> 16), (unsigned char)(q >> 8), (unsigned char)q};
                            fwrite(c, 1, 3, f);
                        }
                fclose(f);
            }
        }
    }
    if (!r.ok) { std::cout << "FAILED: " << r.error << "\n"; return 1; }
    std::cout << r.info << "\nfixes=" << r.changes.size() << " warnings=" << r.warnings.size()
              << "\nreport=" << r.report << "\n";
    return 0;
}
