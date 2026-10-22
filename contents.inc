// Halo Map Porter - the weapons & vehicles window (Xbox): measuring what each weapon and vehicle costs,
// and applying the picks (replace with another one, or with none) when converting.
// Included by core.cpp (uses its Job and helpers).

// Xbox stock weapons and vehicles offered as replacements (when the Xbox tags have them).
static const char *kStockWeaponChoices[] = {
    "weapons/assault rifle/assault rifle.weapon", "weapons/pistol/pistol.weapon",
    "weapons/sniper rifle/sniper rifle.weapon",   "weapons/rocket launcher/rocket launcher.weapon",
    "weapons/shotgun/shotgun.weapon",             "weapons/plasma pistol/plasma pistol.weapon",
    "weapons/plasma rifle/plasma rifle.weapon",   "weapons/needler/needler.weapon"};
static const char *kStockVehicleChoices[] = {
    "vehicles/warthog/warthog.vehicle", "vehicles/ghost/ghost_mp.vehicle", "vehicles/scorpion/scorpion_mp.vehicle",
    "vehicles/rwarthog/rwarthog.vehicle", "vehicles/banshee/banshee_mp.vehicle"};

static uint32_t rd32(const std::string &s, size_t o) {
    if (o + 4 > s.size()) return 0;
    return (uint32_t)(uint8_t)s[o] | (uint32_t)(uint8_t)s[o + 1] << 8 | (uint32_t)(uint8_t)s[o + 2] << 16 |
           (uint32_t)(uint8_t)s[o + 3] << 24;
}
static uint16_t rd16(const std::string &s, size_t o) {
    if (o + 2 > s.size()) return 0;
    return (uint16_t)((uint8_t)s[o] | (uint8_t)s[o + 1] << 8);
}
static uint32_t fourcc(const char *c) { return (uint32_t)c[0] << 24 | (uint32_t)c[1] << 16 | (uint32_t)c[2] << 8 | (uint32_t)c[3]; }

static std::string class_extension(uint32_t c) {
    static const std::map<uint32_t, const char *> ext = {
        {fourcc("weap"), "weapon"}, {fourcc("vehi"), "vehicle"}, {fourcc("itmc"), "item_collection"},
        {fourcc("bipd"), "biped"}, {fourcc("eqip"), "equipment"}, {fourcc("mode"), "model"},
        {fourcc("mod2"), "gbxmodel"}, {fourcc("scnr"), "scenario"}, {fourcc("matg"), "globals"},
        {fourcc("sbsp"), "scenario_structure_bsp"}, {fourcc("bitm"), "bitmap"}, {fourcc("snd!"), "sound"},
        {fourcc("antr"), "model_animations"}, {fourcc("coll"), "model_collision_geometry"},
        {fourcc("effe"), "effect"}, {fourcc("proj"), "projectile"}, {fourcc("scen"), "scenery"},
        {fourcc("garb"), "garbage"}, {fourcc("ustr"), "unicode_string_list"}, {fourcc("tagc"), "tag_collection"}};
    auto it = ext.find(c);
    if (it != ext.end()) return it->second;
    std::string s;
    for (int i = 3; i >= 0; i--) { char ch = (char)(c >> (i * 8)); if (ch != ' ') s += ch; }
    return s;
}

struct CacheTag { std::string path; uint32_t cls; uint32_t id, path_ptr, data; };

// An Xbox cache file, decompressed, with its tag index.
static bool read_xbox_cache(const fs::path &file, std::string &f, std::vector<CacheTag> &tags, uint32_t &base,
                            uint32_t &tdo, uint32_t &tds, std::string &err) {
    std::ifstream in(file, std::ios::binary);
    std::string d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (d.size() < 0x800 || d.compare(0, 4, "daeh") != 0 || rd32(d, 4) != 5) { err = "not an Xbox map"; return false; }
    uint32_t dec = rd32(d, 8);
    std::string body;
    if (!inflate::zlib((const uint8_t *)d.data() + 0x800, d.size() - 0x800, body, dec > 0x800 ? dec - 0x800 : 0)) {
        err = "couldn't decompress the analysis map";
        return false;
    }
    f = d.substr(0, 0x800) + body;
    tdo = rd32(f, 0x10);
    tds = rd32(f, 0x14);
    uint32_t tag_array = rd32(f, tdo), count = rd32(f, tdo + 12);
    base = tag_array - 0x24;
    if ((uint64_t)tdo + tds > f.size() || count > 65535) { err = "bad tag data"; return false; }
    tags.resize(count);
    for (uint32_t i = 0; i < count; i++) {
        size_t e = tdo + (size_t)(tag_array - base) + i * 32;
        CacheTag &t = tags[i];
        t.cls = rd32(f, e);
        t.id = rd32(f, e + 12);
        t.path_ptr = rd32(f, e + 16);
        t.data = rd32(f, e + 20);
        size_t po = tdo + (size_t)(t.path_ptr - base);
        if (po < f.size()) t.path = std::string(f.c_str() + po, strnlen(f.c_str() + po, std::min<size_t>(256, f.size() - po)));
        std::replace(t.path.begin(), t.path.end(), '\\', '/');
        t.path += "." + class_extension(t.cls);
    }
    return true;
}

// Bytes covered by a set of [start, end) ranges.
static uint64_t range_union(std::vector<std::pair<uint32_t, uint32_t>> r) {
    std::sort(r.begin(), r.end());
    uint64_t total = 0;
    uint32_t cs = 0, ce = 0;
    bool open = false;
    for (auto &[s, e] : r) {
        if (!open || s > ce) {
            if (open) total += ce - cs;
            cs = s; ce = e; open = true;
        } else if (e > ce) ce = e;
    }
    if (open) total += ce - cs;
    return total;
}

// Tags reachable from the roots (plus `extra`), with picked weapons/vehicles redirected (or dropped).
static std::vector<char> reach_set(const ContentAnalysis &a, const std::map<int, int> &redirect, const std::vector<int> &extra) {
    std::vector<char> seen(a.tags.size(), 0);
    std::vector<int> stack;
    auto visit = [&](int t) {
        auto it = redirect.find(t);
        if (it != redirect.end()) t = it->second;
        if (t < 0 || seen[t]) return;
        seen[t] = 1;
        stack.push_back(t);
    };
    for (int r : a.roots) visit(r);
    for (int r : extra) visit(r);
    while (!stack.empty()) {
        int t = stack.back();
        stack.pop_back();
        for (int d : a.deps[t]) visit(d);
    }
    return seen;
}

static ContentEstimate estimate_reach(const ContentAnalysis &a, const std::map<int, int> &redirect, const std::vector<int> &extra) {
    std::vector<char> seen = reach_set(a, redirect, extra);
    uint64_t mem = 0, file = 0;
    std::vector<std::pair<uint32_t, uint32_t>> geo;
    for (size_t i = 0; i < a.tags.size(); i++)
        if (seen[i]) {
            mem += a.bytes[i];
            file += a.file_bytes[i];
            geo.insert(geo.end(), a.geometry[i].begin(), a.geometry[i].end());
        }
    uint64_t g = range_union(geo);
    ContentEstimate e;
    e.memory_mb = a.fixed_mb + (mem + g) / 1048576.0;
    e.file_mb = a.fixed_file_mb + (file + g) / 1048576.0;
    // The optimize saving shrinks with the tag data it works on (the level BSP isn't part of it).
    double data_now = a.used_mb - a.fixed_mb, data = e.memory_mb - a.fixed_mb;
    e.memory_optimized_mb = e.memory_mb - (data_now > 0 ? a.optimize_saves_mb * data / data_now : 0);
    return e;
}

static int tag_index_of(const ContentAnalysis &a, const std::string &tag) {
    auto it = std::find(a.tags.begin(), a.tags.end(), tag);
    return it == a.tags.end() ? -1 : (int)(it - a.tags.begin());
}

static ContentEstimate estimate_contents_impl(const ContentAnalysis &a, const std::map<std::string, std::string> &picks) {
    std::map<int, int> redirect;
    for (auto &[from, to] : picks) {
        int f = tag_index_of(a, from);
        if (f < 0) continue;
        redirect[f] = to.empty() ? -1 : tag_index_of(a, to);
    }
    return estimate_reach(a, redirect, {});
}

// Offer the Xbox's own weapons and vehicles as replacements: add them to the scenario's palettes (in the
// analysis job's own copy, nothing placed from them) so the analysis build measures them too. Returns those added.
static std::set<std::string> add_stock_choices_for_analysis(Job &job) {
    std::set<std::string> added;
    if (job.overlay.empty()) return added;
    const Config &cfg = job.cfg;
    std::string scen = job.scenario + ".scenario";
    fs::path sdir = job.owner(scen);
    if (sdir.empty() || (sdir == job.overlay && (!job.to_fixes(scen, sdir) || (sdir = job.fixes).empty()))) return added;
    std::set<std::string> present;
    for (auto &[k, v] : list_values(cfg, sdir, scen)) present.insert(v);
    auto count = [&](const std::string &key) {
        std::string v;
        run_process({u8(tool(cfg, "invader-edit")), "-t", u8(sdir), scen, "-C", key}, v);
        return std::atoi(v.c_str());
    };
    int nw = count("weapon_palette"), nv = count("vehicle_palette");
    std::error_code ec;
    fs::copy_file(sdir / from_u8(scen), job.work / "choices_backup.scenario", fs::copy_options::overwrite_existing, ec);
    job.choices_scenario = sdir / from_u8(scen);
    std::vector<std::string> cmd{u8(tool(cfg, "invader-edit")), "-t", u8(sdir), scen};
    for (const char *c : kStockWeaponChoices)
        if (!present.count(c) && fs::exists(job.overlay / from_u8(c))) {
            cmd.insert(cmd.end(), {"-I", "weapon_palette", "1", "end", "-S", "weapon_palette[" + std::to_string(nw++) + "].name", c});
            added.insert(c);
        }
    for (const char *c : kStockVehicleChoices)
        if (!present.count(c) && fs::exists(job.overlay / from_u8(c))) {
            cmd.insert(cmd.end(), {"-I", "vehicle_palette", "1", "end", "-S", "vehicle_palette[" + std::to_string(nv++) + "].name", c});
            added.insert(c);
        }
    std::string out;
    if (added.empty() || run_process(cmd, out) != 0) added.clear();
    return added;
}

static double tag_space_used(const std::string &build_out) {
    static const std::regex re(R"(Tag space:\s+([\d.]+)\s*/)");
    std::smatch m;
    return std::regex_search(build_out, m, re) ? std::atof(m[1].str().c_str()) : 0;
}

// Vehicles the map's scripts create and attach to another vehicle as its pieces (a jet's wings and engines when it's
// shot down, a mech's wreck): vehicle tag -> the vehicle (tag) they're pieces of. `seatless`: the possible pieces
// (no seats, or a wreck by its name); `with_seats`: the vehicles they can be pieces of. Found in the script
// source: (objects_attach <vehicle> <marker> <piece> <marker>), else the vehicle a script that creates the piece
// (object_create, object_create_anew) also names. Placements are known by their object names.
static std::map<std::string, std::string> script_pieces(Job &job, const std::set<std::string> &seatless,
                                                        const std::set<std::string> &with_seats) {
    std::map<std::string, std::string> out;
    if (seatless.empty() || with_seats.empty()) return out;
    std::string scen = job.scenario + ".scenario";
    fs::path sdir = job.owner(scen);
    tagfile::Tag st;
    if (sdir.empty() || !st.load(sdir / from_u8(scen))) return out;
    tagfile::Elem r = st.root();
    tagfile::Block names = r.block("object names"), placed = r.block("vehicles"), palette = r.block("vehicle palette");
    std::map<std::string, std::string> vehicle_of;   // object name (lowercase) -> vehicle tag
    for (size_t i = 0; i < placed.size(); i++) {
        long long n = placed[i].integer("name"), ty = placed[i].integer("type");
        if (n < 0 || n >= (long long)names.size() || ty < 0 || ty >= (long long)palette.size()) continue;
        std::string tag = palette[(size_t)ty].ref("name");
        if (!tag.empty()) vehicle_of[lc(names[(size_t)n].str("name"))] = tag;
    }
    if (vehicle_of.empty()) return out;
    std::string text = map_script_text(job, sdir, scen);
    // Tokens: "(", ")" and atoms (lowercase), comments left out.
    std::vector<std::string> tok;
    for (size_t i = 0; i < text.size();) {
        char c = text[i];
        if (c == ';') { while (i < text.size() && text[i] != '\n') i++; continue; }
        if (isspace((unsigned char)c)) { i++; continue; }
        if (c == '(' || c == ')') { tok.push_back(std::string(1, c)); i++; continue; }
        size_t j = i;
        if (c == '"') { j++; while (j < text.size() && text[j] != '"') j++; if (j < text.size()) j++; }
        else while (j < text.size() && !isspace((unsigned char)text[j]) && text[j] != '(' && text[j] != ')' && text[j] != ';') j++;
        tok.push_back(lc(text.substr(i, j - i)));
        i = j;
    }
    auto vehicle = [&](const std::string &name) -> std::string {
        auto it = vehicle_of.find(name);
        return it == vehicle_of.end() ? std::string() : it->second;
    };
    std::map<std::string, std::map<std::string, int>> votes;   // piece -> vehicle -> times
    for (size_t i = 0; i + 5 < tok.size(); i++) {
        if (tok[i] != "(" || tok[i + 1] != "objects_attach") continue;
        bool atoms = true;
        for (size_t k = i + 2; k < i + 6; k++) atoms = atoms && tok[k] != "(" && tok[k] != ")";
        if (!atoms) continue;
        std::string parent = vehicle(tok[i + 2]), piece = vehicle(tok[i + 4]);
        if (seatless.count(piece) && with_seats.count(parent)) votes[piece][parent]++;
    }
    // Not attached: the vehicle a script that creates it also names.
    for (size_t i = 0; i < tok.size();) {
        if (tok[i] != "(") { i++; continue; }
        size_t end = i;
        for (int depth = 0; end < tok.size(); end++) {
            if (tok[end] == "(") depth++;
            else if (tok[end] == ")" && --depth == 0) break;
        }
        std::set<std::string> created, others;
        for (size_t k = i; k + 1 < end; k++) {
            if (tok[k] == "(" && (tok[k + 1] == "object_create" || tok[k + 1] == "object_create_anew") && k + 2 < end) {
                std::string v = vehicle(tok[k + 2]);
                if (seatless.count(v) && !votes.count(v)) created.insert(v);
            }
            std::string v = vehicle(tok[k]);
            if (with_seats.count(v)) others.insert(v);
        }
        if (others.size() == 1)
            for (auto &c : created) votes[c][*others.begin()]++;
        i = end + 1;
    }
    for (auto &[piece, v] : votes) {
        auto best = std::max_element(v.begin(), v.end(), [](auto &x, auto &y) { return x.second < y.second; });
        out[piece] = best->first;
    }
    return out;
}

// Read the analysis build back: each tag's bytes (its data, its entry in the tag array and its path), the model
// geometry each model uses (Invader stores identical geometry once, so shared parts count once), the level's
// BSPs and every tag reference, then list the weapons and vehicles a player can get.
static bool analyze_built_map(Job &job, const fs::path &map, const std::set<std::string> &added, ContentAnalysis &a,
                              std::string &err) {
    std::string f;
    std::vector<CacheTag> ct;
    uint32_t base = 0, tdo = 0, tds = 0;
    if (!read_xbox_cache(map, f, ct, base, tdo, tds, err)) return false;
    const size_t n = ct.size();
    a.tags.resize(n);
    a.bytes.assign(n, 0);
    a.file_bytes.assign(n, 0);
    a.deps.assign(n, {});
    a.geometry.assign(n, {});
    for (size_t i = 0; i < n; i++) a.tags[i] = ct[i].path;
    std::unordered_map<uint32_t, int> by_id;
    for (size_t i = 0; i < n; i++) by_id[ct[i].id] = (int)i;
    auto rel = [&](uint32_t ptr) -> int64_t { return (int64_t)ptr - (int64_t)base; };
    auto in_td = [&](uint32_t ptr) { int64_t r = rel(ptr); return r >= 0 && r < (int64_t)tds; };

    // Where each tag's data ends: the next tag's data, or the model geometry Invader puts among the tags.
    std::vector<uint32_t> bounds;
    for (auto &t : ct) if (in_td(t.data)) bounds.push_back((uint32_t)rel(t.data));
    uint32_t parts = rd32(f, tdo + 0x10), varr = (uint32_t)rel(rd32(f, tdo + 0x14)), iarr = (uint32_t)rel(rd32(f, tdo + 0x1C));
    if (parts > 0 && parts < 1000000) {
        bounds.insert(bounds.end(), {varr, varr + 12 * parts, iarr, iarr + 12 * parts});
        uint32_t vmin = UINT32_MAX, imin = UINT32_MAX;
        for (uint32_t k = 0; k < parts; k++) {
            vmin = std::min(vmin, (uint32_t)rel(rd32(f, tdo + varr + 12 * k + 4)));
            imin = std::min(imin, (uint32_t)rel(rd32(f, tdo + iarr + 12 * k + 4)));
        }
        if (vmin < tds) bounds.push_back(vmin);
        if (imin < tds) bounds.push_back(imin);
    }
    bounds.push_back(tds);
    std::sort(bounds.begin(), bounds.end());
    bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());
    std::vector<std::pair<size_t, size_t>> region(n, {0, 0});   // file offset, size
    for (size_t i = 0; i < n; i++) {
        if (!in_td(ct[i].data)) continue;
        uint32_t r = (uint32_t)rel(ct[i].data);
        auto it = std::upper_bound(bounds.begin(), bounds.end(), r);
        uint32_t end = it == bounds.end() ? tds : *it;
        region[i] = {tdo + (size_t)r, end - r};
    }

    // The level's BSPs live outside the tag data; only the biggest is in memory at a time.
    uint64_t max_bsp = 0, all_bsp = 0;
    std::vector<char> is_bsp(n, 0);
    auto sit = by_id.find(rd32(f, tdo + 4));
    if (sit != by_id.end() && in_td(ct[sit->second].data)) {
        size_t s = tdo + (size_t)rel(ct[sit->second].data);
        uint32_t cnt = rd32(f, s + 1444), ptr = rd32(f, s + 1448);
        for (uint32_t j = 0; j < cnt && j < 64 && in_td(ptr); j++) {
            size_t e = tdo + (size_t)rel(ptr) + 32 * j;
            uint32_t start = rd32(f, e), size = rd32(f, e + 4);
            auto bt = by_id.find(rd32(f, e + 28));
            if (bt == by_id.end() || (uint64_t)start + size > f.size()) continue;
            region[bt->second] = {start, size};
            is_bsp[bt->second] = 1;
            max_bsp = std::max<uint64_t>(max_bsp, size);
            all_bsp += size;
        }
    }

    // Tag references: [class][path pointer][0][tag id] in the cache.
    for (size_t i = 0; i < n; i++) {
        auto [off, size] = region[i];
        std::set<int> seen;
        for (size_t k = off + 12; k + 4 <= off + size; k++) {
            auto it = by_id.find(rd32(f, k));
            if (it == by_id.end() || it->second == (int)i) continue;
            if (rd32(f, k - 8) != ct[it->second].path_ptr) continue;
            if (seen.insert(it->second).second) a.deps[i].push_back(it->second);
        }
    }
    // The stock choices were only added to the scenario's palettes for measuring.
    int si = tag_index_of(a, job.scenario + ".scenario");
    if (si >= 0)
        a.deps[si].erase(std::remove_if(a.deps[si].begin(), a.deps[si].end(), [&](int d) { return added.count(a.tags[d]) > 0; }),
                         a.deps[si].end());

    // Model geometry ranges (compressed vertices are 32 bytes, triangle strip indices 2 bytes).
    for (size_t i = 0; i < n; i++) {
        if (ct[i].cls != fourcc("mode") || !in_td(ct[i].data)) continue;
        size_t m = tdo + (size_t)rel(ct[i].data);
        uint32_t gc = rd32(f, m + 208), gp = rd32(f, m + 212);
        for (uint32_t gk = 0; gk < gc && gk < 64 && in_td(gp); gk++) {
            size_t geo = tdo + (size_t)rel(gp) + 48 * gk;
            uint32_t pc = rd32(f, geo + 36), pp = rd32(f, geo + 40);
            for (uint32_t pk = 0; pk < pc && pk < 512 && in_td(pp); pk++) {
                size_t part = tdo + (size_t)rel(pp) + 104 * pk;
                uint32_t tri = rd16(f, part + 72), tptr = rd32(f, part + 76), vcount = rd16(f, part + 88), ventry = rd32(f, part + 100);
                if (in_td(tptr)) a.geometry[i].push_back({(uint32_t)rel(tptr), (uint32_t)rel(tptr) + (tri + 2) * 2});
                if (in_td(ventry)) {
                    uint32_t vptr = rd32(f, tdo + (size_t)rel(ventry) + 4);
                    if (in_td(vptr)) a.geometry[i].push_back({(uint32_t)rel(vptr), (uint32_t)rel(vptr) + vcount * 32});
                }
            }
        }
    }

    // Bytes per tag; textures and sounds also take their pixel/sample data in the map file (their tag file size).
    uint64_t sum = 0, sum_file = 0;
    std::vector<std::pair<uint32_t, uint32_t>> all_geo;
    for (size_t i = 0; i < n; i++) {
        uint64_t b = 32 + ct[i].path.size() + 1;
        if (!is_bsp[i]) b += region[i].second;
        a.bytes[i] = b;
        uint64_t fb = b + (is_bsp[i] ? region[i].second : 0);
        if (ct[i].cls == fourcc("bitm") || ct[i].cls == fourcc("snd!")) {
            fs::path own = job.owner(ct[i].path);
            std::error_code ec;
            if (!own.empty()) fb += fs::file_size(own / from_u8(ct[i].path), ec);
        }
        a.file_bytes[i] = fb;
        sum += b;
        sum_file += fb;
        all_geo.insert(all_geo.end(), a.geometry[i].begin(), a.geometry[i].end());
    }
    uint64_t geo = range_union(all_geo);
    a.fixed_mb = ((double)tds + max_bsp - (double)sum - (double)geo) / 1048576.0;
    a.fixed_file_mb = std::max(0.0, ((double)rd32(f, 8) - (double)sum_file - (double)geo) / 1048576.0);

    std::vector<int> indeg(n, 0);
    for (auto &d : a.deps) for (int x : d) indeg[x]++;
    for (size_t i = 0; i < n; i++) if (!indeg[i] && !added.count(a.tags[i])) a.roots.push_back((int)i);   // the stock choices aren't roots
    ContentEstimate now = estimate_reach(a, {}, {});
    a.used_mb = now.memory_mb;
    a.file_mb = now.file_mb;

    // Where the map uses each weapon and vehicle (spawn points, placements, starting weapons, lists).
    std::string scen = job.scenario + ".scenario";
    fs::path sdir = job.owner(scen);
    std::map<std::string, int> spawns_of_collection, placed;
    std::set<std::string> starting;
    if (!sdir.empty()) {
        static const std::regex ne(R"(^netgame_equipment\[\d+\]\.item_collection$)");
        static const std::regex se(R"(^starting_equipment\[\d+\]\.item_collection_\d$)");
        static const std::regex pal(R"(^(weapon|vehicle)_palette\[(\d+)\]\.name$)");
        static const std::regex plc(R"(^(weapons|vehicles)\[\d+\]\.type$)");
        std::map<std::string, std::string> palette;   // "weapon 3" -> tag
        std::map<std::string, int> uses;              // "weapon 3" -> placements
        for (auto &[k, v] : list_values(job.cfg, sdir, scen)) {
            std::smatch m;
            if (v.empty()) continue;
            if (std::regex_match(k, ne)) spawns_of_collection[v]++;
            else if (std::regex_match(k, se)) starting.insert(v);
            else if (std::regex_match(k, m, pal)) palette[m[1].str() + " " + m[2].str()] = v;
            else if (std::regex_match(k, m, plc) && isdigit((unsigned char)v[0]))
                uses[(m[1].str() == "weapons" ? "weapon " : "vehicle ") + v]++;
        }
        for (auto &[key, cnt] : uses) if (palette.count(key)) placed[palette[key]] += cnt;
    }
    std::vector<char> in_map = reach_set(a, {}, {});   // a stock choice the map already uses isn't a "choice"
    std::vector<std::vector<int>> parents(n);
    for (size_t i = 0; i < n; i++) for (int d : a.deps[i]) parents[d].push_back((int)i);
    std::map<std::string, int> stem_count;
    auto stem = [](const std::string &t) { return fs::path(from_u8(t)).stem().u8string(); };
    std::vector<ContentItem> items;
    for (size_t i = 0; i < n; i++) {
        bool weap = ct[i].cls == fourcc("weap"), vehi = ct[i].cls == fourcc("vehi");
        if (!weap && !vehi) continue;
        std::string s = stem(ct[i].path);
        if (weap && (s == "ball" || s == "flag")) continue;   // the Oddball and CTF flag
        bool candidate = added.count(ct[i].path) > 0 && !in_map[i];
        int spawns = 0, plc = placed.count(ct[i].path) ? placed[ct[i].path] : 0;
        bool start = false, list = false, spawn_weapon = false, relevant = candidate || plc > 0;
        for (int p : parents[i]) {
            uint32_t pc = ct[p].cls;
            if (pc == fourcc("itmc")) {
                relevant = true;
                spawns += spawns_of_collection.count(ct[p].path) ? spawns_of_collection[ct[p].path] : 0;
                if (starting.count(ct[p].path)) start = true;
            } else if (pc == fourcc("matg")) { relevant = true; list = true; }
            else if (pc == fourcc("bipd") && weap) { relevant = true; spawn_weapon = true; }
            else if (pc == fourcc("scnr")) relevant = true;
        }
        if (!relevant) continue;
        ContentItem it;
        it.tag = ct[i].path;
        it.name = s;
        it.vehicle = vehi;
        it.in_map = !candidate;
        std::vector<std::string> u;
        if (spawns) u.push_back(std::to_string(spawns) + " spawn point" + (spawns == 1 ? "" : "s"));
        if (plc) u.push_back("placed " + std::to_string(plc) + "x");
        if (start) u.push_back("starting weapon");
        if (spawn_weapon) u.push_back("players spawn with it");
        if (list && !candidate) u.push_back(vehi ? "vehicle slot" : "weapon list");
        if (candidate) u.push_back("Xbox " + std::string(vehi ? "vehicle" : "weapon") + " (not in the map)");
        for (size_t k = 0; k < u.size(); k++) it.used_by += (k ? ", " : "") + u[k];
        if (candidate) {
            ContentEstimate with = estimate_reach(a, {}, {(int)i});
            it.memory_mb = with.memory_mb - now.memory_mb;
            it.file_mb = with.file_mb - now.file_mb;
        } else {
            ContentEstimate without = estimate_reach(a, {{(int)i, -1}}, {});
            it.memory_mb = now.memory_mb - without.memory_mb;
            it.file_mb = now.file_mb - without.file_mb;
        }
        stem_count[s]++;
        items.push_back(it);
    }
    for (auto &it : items)   // two weapons with the same name: say which folder
        if (stem_count[it.name] > 1) it.name += " (" + fs::path(from_u8(it.tag)).parent_path().filename().u8string() + ")";

    // Each one's model (to show it), a vehicle's seats, and which vehicles are pieces of others.
    preview::Find find = [&](const std::string &tag) {
        fs::path o = job.owner(tag);
        return o.empty() ? fs::path() : o / from_u8(tag);
    };
    std::map<std::string, int> preview_of;   // model tag -> preview
    std::set<std::string> seatless, with_seats;
    auto wreck = [](const std::string &tag) {   // a vehicle named as a wreck (a folder counts: ".../burnt/front")
        std::string t = lc(tag);
        for (auto w : {"burnt", "burned", "destroyed", "wreck", "debris", "broken", "dead", "damaged", "crashed"})
            if (t.find(w) != std::string::npos) return true;
        return false;
    };
    for (auto &it : items) {
        fs::path of = find(it.tag);
        if (of.empty()) continue;
        if (it.vehicle) {
            it.seats = preview::seats(of, &it.seat_roles);
            if ((it.seats == 0 || wreck(it.tag)) && it.in_map) seatless.insert(it.tag);
            else if (it.seats > 0) with_seats.insert(it.tag);
        }
        std::string model = perf::object_model(of);
        if (model.empty()) continue;
        auto pv = preview_of.find(model);
        if (pv == preview_of.end()) {
            ModelPreview mp;
            int idx = preview::build(model, find, mp) ? (int)a.previews.size() : -1;
            if (idx >= 0) a.previews.push_back(std::move(mp));
            pv = preview_of.emplace(model, idx).first;
        }
        it.preview = pv->second;
    }
    std::map<std::string, std::string> pieces = script_pieces(job, seatless, with_seats);
    for (auto &it : items) {
        auto p = pieces.find(it.tag);
        if (p != pieces.end()) {
            it.piece_of = p->second;
            std::string parent = fs::path(from_u8(p->second)).stem().u8string();
            for (auto &x : items)
                if (x.tag == p->second) parent = x.name;
            it.used_by = "piece of " + parent + (it.seats == 0 ? ", no seats" : "");
        } else if (it.vehicle && it.seats == 0) {
            it.used_by += it.used_by.empty() ? "no seats" : ", no seats";
        }
    }
    std::stable_sort(items.begin(), items.end(), [](const ContentItem &x, const ContentItem &y) {
        if (x.vehicle != y.vehicle) return !x.vehicle;
        if (x.in_map != y.in_map) return x.in_map;
        return x.memory_mb > y.memory_mb;
    });
    a.items = items;
    return true;
}

// ---------------------------------------------------------------- applying the picks

struct PalBlock { const char *palette, *block; };

// An item collection that no longer holds anything: the scenario's spawn points for it go, and it's taken out
// of the starting equipment.
static void remove_item_collection_uses(Job &job, const fs::path &xsrc, const std::string &collection) {
    const Config &cfg = job.cfg;
    std::string scen = job.scenario + ".scenario", out;
    fs::path d = fs::exists(job.fixes / from_u8(scen)) ? job.fixes : xsrc;
    if (!fs::exists(d / from_u8(scen))) return;
    static const std::regex ne(R"(^netgame_equipment\[(\d+)\]\.item_collection$)");
    static const std::regex se(R"(^starting_equipment\[\d+\]\.item_collection_\d$)");
    std::vector<int> drop;
    std::vector<std::string> cmd{u8(tool(cfg, "invader-edit")), "-t", u8(d), scen};
    for (auto &[k, v] : list_values(cfg, d, scen)) {
        if (v != collection) continue;
        std::smatch m;
        if (std::regex_match(k, m, ne)) drop.push_back(std::stoi(m[1]));
        else if (std::regex_match(k, se)) cmd.insert(cmd.end(), {"-S", k, ""});
    }
    std::sort(drop.rbegin(), drop.rend());
    for (int i : drop) cmd.insert(cmd.end(), {"-E", "netgame_equipment[" + std::to_string(i) + "]"});
    if (cmd.size() > 4) run_process(cmd, out);
}

// Replace (or remove) the picked weapons and vehicles everywhere the map refers to them. Runs at the end of the
// Xbox preparation, on the tags the build uses.
static void apply_replacements(Job &job, const fs::path &xsrc) {
    const Config &cfg = job.cfg;
    if (cfg.xbox_replace.empty()) return;
    std::string out;
    std::vector<std::string> notes;
    int emptied_all = 0;
    job.log("Applying your weapons & vehicles picks...");
    // Tags that refer to each picked one (and each replacement, for a later pick of it), found in one pass over
    // the tags the map uses (following references from its scenario and globals, so a full Xbox tagset isn't read).
    std::set<std::string> targets;
    for (auto &[from, to] : cfg.xbox_replace) {
        if (!from.empty()) targets.insert(from);
        if (!to.empty()) targets.insert(to);
    }
    auto referrers = find_referrers({job.fixes, xsrc, job.overlay}, targets, {job.scenario + ".scenario", "globals/globals.globals"});
    for (auto &[from, to] : cfg.xbox_replace) {
        if (from.empty() || from == to) continue;
        bool stock_from = !job.overlay.empty() && fs::exists(job.overlay / from_u8(from));
        std::vector<std::string> refs;
        for (auto &l : referrers[from])
            if (fs::exists(job.fixes / from_u8(l)) || fs::exists(xsrc / from_u8(l)) || (stock_from && fs::exists(job.overlay / from_u8(l))))
                refs.push_back(l);
        // Edits go to the copy the build uses (during the preparation: fixes, then the prepared tags); a stock
        // one is copied to the fixes folder first.
        auto editable = [&](const std::string &tag) -> fs::path {
            if (fs::exists(job.fixes / from_u8(tag))) return job.fixes;
            if (fs::exists(xsrc / from_u8(tag))) return xsrc;
            if (!job.overlay.empty() && fs::exists(job.overlay / from_u8(tag)) && job.to_fixes(tag, job.overlay)) return job.fixes;
            return {};
        };
        std::string label = fs::path(from_u8(from)).stem().u8string();
        int same = 0;   // two picks with this name: say which folder
        for (auto &[f2, t2] : cfg.xbox_replace) same += fs::path(from_u8(f2)).stem().u8string() == label;
        if (same > 1) label += " (" + fs::path(from_u8(from)).parent_path().filename().u8string() + ")";
        if (!to.empty()) {
            int changed = 0;
            for (auto &r : refs) {
                fs::path d = editable(r);
                if (d.empty()) continue;
                if (run_process({u8(tool(cfg, "invader-refactor")), "-t", u8(d), "-M", "no-move", "-U", "-s", r, "-T", from, to}, out) == 0) {
                    changed++;
                    referrers[to].insert(r);   // now refers to the replacement
                }
            }
            if (changed) notes.push_back(label + " -> " + fs::path(from_u8(to)).stem().u8string());
            continue;
        }
        // None: take it out of each list that refers to it.
        auto values = [&](const fs::path &d, const std::string &tag) { return list_values(cfg, d, tag); };
        std::vector<std::string> left;
        int emptied = 0;   // its placements the scripts name, left with no type
        for (auto &r : refs) {
            fs::path d = editable(r);
            if (d.empty()) { left.push_back(r); continue; }
            std::string ext = fs::path(from_u8(r)).extension().u8string();
            std::vector<std::string> ed{u8(tool(cfg, "invader-edit")), "-t", u8(d), r};
            std::vector<std::string> erase;    // block elements to erase, highest index first
            bool other = false;
            static const std::regex itmc_re(R"(^permutations\[(\d+)\]\.item$)");
            static const std::regex wlist_re(R"(^weapon_list\[\d+\]\.weapon$)");
            static const std::regex mpveh_re(R"(^multiplayer_information\[0\]\.vehicles\[\d+\]\.vehicle$)");
            static const std::regex unitw_re(R"(^weapons\[(\d+)\]\.weapon$)");
            std::vector<int> idx;
            for (auto &[k, v] : values(d, r)) {
                if (v != from) continue;
                std::smatch m;
                if (ext == ".item_collection" && std::regex_match(k, m, itmc_re)) idx.push_back(std::stoi(m[1]));
                else if (ext == ".globals" && (std::regex_match(k, wlist_re) || std::regex_match(k, mpveh_re)))
                    ed.insert(ed.end(), {"-S", k, ""});   // the slot stays (lists are by position), empty
                else if (ext == ".biped" && std::regex_match(k, m, unitw_re)) idx.push_back(std::stoi(m[1]));
                else if (ext == ".scenario") {}         // palettes: below
                else other = true;
            }
            std::sort(idx.rbegin(), idx.rend());
            for (int i : idx) erase.push_back(ext == ".item_collection" ? "permutations[" + std::to_string(i) + "]"
                                                                        : "weapons[" + std::to_string(i) + "]");
            for (auto &e : erase) ed.insert(ed.end(), {"-E", e});
            if (ed.size() > 4) run_process(ed, out);
            if (ext == ".scenario") {
                // A palette entry: remove it and its placements, then shift the later placements down. Placements the
                // scripts know by name stay, with no type (the engine leaves those out): Invader only compiles scripts
                // whose object names belong to a placement. The scripts then find no object there (a jet's pieces
                // are never made, health reads -1), which they handle like an object that's been destroyed.
                for (PalBlock pl : {PalBlock{"weapon_palette", "weapons"}, PalBlock{"vehicle_palette", "vehicles"}, PalBlock{"equipment_palette", "equipment"}}) {
                    auto kv = values(d, r);
                    int pi = -1;
                    std::regex name_re("^" + std::string(pl.palette) + R"(\[(\d+)\]\.name$)");
                    for (auto &[k, v] : kv) { std::smatch m; if (v == from && std::regex_match(k, m, name_re)) { pi = std::stoi(m[1]); break; } }
                    if (pi < 0) continue;
                    std::regex type_re("^" + std::string(pl.block) + R"(\[(\d+)\]\.type$)");
                    std::regex objname_re("^" + std::string(pl.block) + R"(\[(\d+)\]\.name$)");
                    std::vector<int> drop;
                    std::vector<std::string> sc{u8(tool(cfg, "invader-edit")), "-t", u8(d), r};
                    std::set<int> named;   // placements with an object name (scripts create or refer to them by it)
                    for (auto &[k, v] : kv) {
                        std::smatch m;
                        if (std::regex_match(k, m, objname_re) && !v.empty() && isdigit((unsigned char)v[0]) && std::atoi(v.c_str()) != 65535)
                            named.insert(std::stoi(m[1]));
                    }
                    for (auto &[k, v] : kv) {
                        std::smatch m;
                        if (!std::regex_match(k, m, type_re) || v.empty() || !isdigit((unsigned char)v[0])) continue;
                        int ty = std::atoi(v.c_str());
                        if (ty == pi && named.count(std::stoi(m[1]))) {
                            sc.insert(sc.end(), {"-S", k, "65535"});
                            emptied++;
                        } else if (ty == pi) drop.push_back(std::stoi(m[1]));
                        else if (ty > pi && ty != 65535) sc.insert(sc.end(), {"-S", k, std::to_string(ty - 1)});
                    }
                    std::sort(drop.rbegin(), drop.rend());
                    for (int i : drop) sc.insert(sc.end(), {"-E", std::string(pl.block) + "[" + std::to_string(i) + "]"});
                    sc.insert(sc.end(), {"-E", std::string(pl.palette) + "[" + std::to_string(pi) + "]"});
                    run_process(sc, out);
                }
                // Script references and the like keep it.
                for (auto &[k, v] : values(d, r)) if (v == from) { other = true; break; }
            }
            // An item collection left with nothing: its spawn points and starting-weapon uses go too.
            if (ext == ".item_collection" && !idx.empty()) {
                std::string c;
                run_process({u8(tool(cfg, "invader-edit")), "-t", u8(d), r, "-C", "permutations"}, c);
                if (std::atoi(c.c_str()) == 0) remove_item_collection_uses(job, xsrc, r);
            }
            if (other) left.push_back(r);
        }
        notes.push_back(label + " -> none" + (left.empty() ? "" : " (still used by " + left[0] + ", so it stays in the map)"));
        emptied_all += emptied;
    }
    if (!notes.empty()) {
        std::string list = notes[0];
        for (size_t i = 1; i < notes.size(); i++) list += ", " + notes[i];
        job.note("[replace] your weapons & vehicles picks: " + list +
                 (emptied_all ? "; the map's scripts name " + std::to_string(emptied_all) + " of their placements, which stay " +
                                "empty so the scripts still work (they find nothing there)" : ""));
    }
}
