// Linux test driver for the conversion engine.
// usage: cli_test convert <in.map> <MCC|CE|Xbox> <out.map>
//        cli_test build <file.scenario> <MCC|CE|Xbox> <out.map>
#include "core.hpp"
#include "render.hpp"
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <iostream>
#include <sstream>
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
    Log log = [](const std::string &s) { std::cout << s << std::endl; };
    std::string mode = argv[1];
    Result r;
    if (mode == "traits") {   // cli_test traits <built.map> x x
        MapTraits t;
        std::string err;
        if (!analyze_map_traits(cfg, argv[2], t, &err)) { std::cout << "FAILED: " << err << "\n"; return 1; }
        std::cout << "Gametypes: " << t.gametypes << "\nPlayers: " << t.pmin << "-" << t.pmax << "\nGenres:";
        for (auto &g : t.genres) std::cout << " [" << g << "]";
        std::cout << "\nwhy: " << t.why_genres << "\nplayers: " << t.players_why << "\n";
        return 0;
    }
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
        if (getenv("XDUMP")) {   // for the window test
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
        if (!cfg.xbox_replace.empty()) {
            ContentEstimate e = estimate_contents(an, cfg.xbox_replace);
            printf("with picks: %.3f MB mem (%.3f after optimize), %.2f MB file\n", e.memory_mb, e.memory_optimized_mb, e.file_mb);
        }
        return 0;
    }
    if (mode == "convert") {
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
    if (!r.ok) { std::cout << "FAILED: " << r.error << "\n"; return 1; }
    std::cout << r.info << "\nfixes=" << r.changes.size() << " warnings=" << r.warnings.size()
              << "\nreport=" << r.report << "\n";
    return 0;
}
