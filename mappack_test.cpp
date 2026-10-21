// Linux test driver for map packs.
// usage: mappack_test read <pack folder> [dump dir]          list what's in a pack (and dump the previews)
//        mappack_test write <pack folder> <pack name> <maps dir> [logo.rgba w h box corner]
//        mappack_test stamp <preview.bin> <series.bin> <out.bin>
//        mappack_test logo <logo.rgba> <w> <h> <box> <corner> <out series.bin>
//        mappack_test menulogo <pack folder> <out dir> [box corner]   the logos on the pack's main menu (and stamped previews)
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"
#define STB_DXT_IMPLEMENTATION
#include "stb_dxt.h"
#include "mappack.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>

static std::string slurp(const char *p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static void spit(const std::string &p, const std::string &d) { std::ofstream f(p, std::ios::binary); f << d; }
static mappack::Logo logo_from(const char *file, int w, int h, int box) {
    mapcard::Image img;
    img.w = w; img.h = h;
    std::string d = slurp(file);
    img.rgba.assign(d.begin(), d.end());
    return mappack::fit_logo(img, box);
}

int main(int argc, char **argv) {
    std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "read" && argc >= 3) {
        mappack::Pack p;
        std::string err;
        if (!mappack::read_pack(argv[2], p, err)) { std::cerr << "ERROR " << err << "\n"; return 1; }
        mappack::assign_folders(p, p.suggested_name);
        std::cout << "suggested name: " << p.suggested_name << "\nmaps dir: " << p.maps_dir.u8string() << "\n";
        for (auto &i : p.info) std::cout << "info: " << i << "\n";
        int k = 0;
        for (auto &e : p.maps) {
            std::string d = e.desc;
            for (auto &c : d) if (c == '\n') c = '|';
            std::cout << "map: " << (e.slot.empty() ? "-" : e.slot) << " | " << e.map_file.filename().u8string() << " | " << e.name
                      << " | " << d << " | folder=" << e.folder << " | preview=" << e.preview.size() << " | include=" << e.include
                      << " | " << e.note << "\n";
            if (argc >= 4 && !e.preview.empty()) spit(std::string(argv[3]) + "/" + std::to_string(k) + ".bin", e.preview);
            k++;
        }
        return 0;
    }
    if (cmd == "write" && argc >= 5) {
        mappack::Pack p;
        std::string err, series;
        if (!mappack::read_pack(argv[2], p, err)) { std::cerr << "ERROR " << err << "\n"; return 1; }
        mappack::assign_folders(p, argv[3]);
        if (argc >= 10)
            series = mappack::series_bin(logo_from(argv[5], atoi(argv[6]), atoi(argv[7]), atoi(argv[8])), (mappack::Corner)atoi(argv[9]), 4);
        if (!mappack::write_pack(p, argv[3], argv[4], series, [](const std::string &s) { std::cout << s << "\n"; }, err)) {
            std::cerr << "ERROR " << err << "\n"; return 1;
        }
        return 0;
    }
    if (cmd == "stamp" && argc >= 5) {
        spit(argv[4], mappack::stamp_preview(slurp(argv[2]), slurp(argv[3])));
        return 0;
    }
    if (cmd == "logo" && argc >= 8) {
        spit(argv[7], mappack::series_bin(logo_from(argv[2], atoi(argv[3]), atoi(argv[4]), atoi(argv[5])), (mappack::Corner)atoi(argv[6]), 4));
        return 0;
    }
    if (cmd == "menulogo" && argc >= 4) {   // menulogo <pack> <out dir> [box corner]: logos as .rgba, stamped previews as .rgba
        mappack::Pack p;
        std::string err;
        if (!mappack::read_pack(argv[2], p, err)) { std::cerr << "ERROR " << err << "\n"; return 1; }
        for (auto &i : p.info) std::cout << "info: " << i << "\n";
        int box = argc >= 5 ? atoi(argv[4]) : 64, corner = argc >= 6 ? atoi(argv[5]) : 0;
        for (size_t k = 0; k < p.menu_logos.size(); k++) {
            auto &m = p.menu_logos[k];
            std::cout << "logo " << k << ": " << m.tag << " | " << m.widget << " | " << m.image.w << "x" << m.image.h << (m.faded ? " | faded" : "") << "\n";
            spit(std::string(argv[3]) + "/logo" + std::to_string(k) + "_" + std::to_string(m.image.w) + "x" + std::to_string(m.image.h) + ".rgba",
                 std::string(m.image.rgba.begin(), m.image.rgba.end()));
            std::string ser = mappack::series_bin(mappack::fit_logo(m.image, box, true), (mappack::Corner)corner, 4);
            for (size_t i = 0; i < p.maps.size() && i < 4; i++) {
                if (p.maps[i].preview.empty()) continue;
                auto px = mapcard::decode_preview(mappack::stamp_preview(p.maps[i].preview, ser));
                spit(std::string(argv[3]) + "/stamp" + std::to_string(k) + "_" + std::to_string(i) + ".rgba", std::string(px.begin(), px.end()));
            }
        }
        return 0;
    }
    if (cmd == "clean" && argc >= 3) { std::cout << "[" << mappack::clean_name(argv[2], 30) << "]\n"; return 0; }
    std::cerr << "bad args\n";
    return 2;
}
