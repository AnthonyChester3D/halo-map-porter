// Halo Map Porter - campaign levels' cutscenes, split off into maps of their own (campaign.hpp).
#include "campaign.hpp"
#include "tagfile.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <iterator>
#include <sstream>

namespace campaign {

namespace {
std::string lower(std::string s) {
    for (auto &c : s) c = (char)tolower((unsigned char)c);
    return s;
}
std::string read_file(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
bool is_int(const std::string &s) {
    if (s.empty()) return false;
    size_t i = s[0] == '-' ? 1 : 0;
    if (i >= s.size()) return false;
    for (; i < s.size(); i++) if (!isdigit((unsigned char)s[i])) return false;
    return true;
}
const std::string &head(const Node &n) {
    static const std::string none;
    return n.list && !n.kids.empty() && !n.kids[0].list ? n.kids[0].atom : none;
}
void walk(const Node &n, const std::function<void(const Node &)> &f) {
    f(n);
    for (auto &k : n.kids) walk(k, f);
}
// Where a script's statements start among its form's parts: (script static <type> name ...) / (script <type> name ...).
size_t body_start(const Node &form) {
    if (form.kids.size() < 3) return form.kids.size();
    const std::string &t = form.kids[1].atom;
    return t == "static" || t == "stub" ? 4 : 3;
}
std::string slice(const std::string &text, size_t b, size_t e) { return e > b && e <= text.size() ? text.substr(b, e - b) : std::string(); }
}   // namespace

std::vector<Node> parse(const std::string &t) {
    std::vector<Node> top;
    std::vector<Node> stack;
    size_t i = 0;
    while (i < t.size()) {
        char c = t[i];
        if (c == ';') {
            while (i < t.size() && t[i] != '\n') i++;
            continue;
        }
        if (isspace((unsigned char)c)) { i++; continue; }
        if (c == '(') {
            Node n;
            n.list = true;
            n.b = i++;
            stack.push_back(std::move(n));
            continue;
        }
        if (c == ')') {
            i++;
            if (stack.empty()) continue;   // (one too many: left alone)
            Node n = std::move(stack.back());
            stack.pop_back();
            n.e = i;
            (stack.empty() ? top : stack.back().kids).push_back(std::move(n));
            continue;
        }
        Node a;
        a.b = i;
        if (c == '"') {
            size_t j = i + 1;
            while (j < t.size() && t[j] != '"') j++;
            a.atom = lower(t.substr(i + 1, j - i - 1));
            i = j < t.size() ? j + 1 : j;
        } else {
            size_t j = i;
            while (j < t.size() && !isspace((unsigned char)t[j]) && t[j] != '(' && t[j] != ')' && t[j] != ';') j++;
            a.atom = lower(t.substr(i, j - i));
            i = j;
        }
        a.e = i;
        (stack.empty() ? top : stack.back().kids).push_back(std::move(a));
    }
    while (!stack.empty()) {   // (unclosed: kept as far as it goes)
        Node n = std::move(stack.back());
        stack.pop_back();
        n.e = t.size();
        (stack.empty() ? top : stack.back().kids).push_back(std::move(n));
    }
    return top;
}

std::set<std::string> atoms(const std::string &text) {
    std::set<std::string> out;
    for (auto &f : parse(text))
        walk(f, [&](const Node &n) { if (!n.list && !n.atom.empty()) out.insert(n.atom); });
    return out;
}

bool load(const fs::path &data, Analysis &a, std::string &why) {
    a = Analysis();
    std::error_code ec;
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(data, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->path().extension() == ".hsc") files.push_back(it->path());
    if (files.empty()) { why = "the map has no scripts"; return false; }
    std::sort(files.begin(), files.end());
    for (auto &p : files) {
        File f;
        f.path = p;
        f.text = read_file(p);
        f.forms = parse(f.text);
        a.files.push_back(std::move(f));
    }
    analyze(a);
    return true;
}

namespace {
void find_defs(Analysis &a) {
    a.defs.clear();
    for (size_t fi = 0; fi < a.files.size(); fi++)
        for (size_t k = 0; k < a.files[fi].forms.size(); k++) {
            const Node &f = a.files[fi].forms[k];
            if (!f.list || f.kids.size() < 3) continue;
            Def d;
            d.file = fi;
            d.form = k;
            if (head(f) == "script") {
                d.kind = "script";
                d.type = f.kids[1].atom;
                size_t at = d.type == "static" || d.type == "stub" ? 3 : 2;
                if (at >= f.kids.size()) continue;
                d.name = f.kids[at].atom;
            } else if (head(f) == "global") {
                d.kind = "global";
                d.type = f.kids[1].atom;
                d.name = f.kids[2].atom;
            } else {
                continue;
            }
            a.defs.push_back(d);
        }
}
bool skip_if(const Node &s) {
    return head(s) == "if" && s.kids.size() >= 3 && head(s.kids[1]) == "cinematic_skip_start";
}
// The statements of a (begin ...) or a single statement, as text.
std::string statements_text(const std::string &text, const Node &n) {
    if (head(n) == "begin") {
        std::string o;
        for (size_t i = 1; i < n.kids.size(); i++) o += "    " + slice(text, n.kids[i].b, n.kids[i].e) + "\n";
        return o;
    }
    return "    " + slice(text, n.b, n.e) + "\n";
}
const Def *def_named(const Analysis &a, const std::string &name) {
    for (auto &d : a.defs) if (d.name == name) return &d;
    return nullptr;
}
}   // namespace

void analyze(Analysis &a) {
    find_defs(a);
    a.opening = Cut();
    a.closing = Cut();
    static const std::set<std::string> setup = {"show_hud", "camera_control", "cinematic_start", "cinematic_show_letterbox",
                                                "fade_out", "player_enable_input", "cinematic_screen_effect_start",
                                                "sound_class_set_gain", "player_camera_control"};
    // The opening: a startup script's skippable stretch (or one in a static script it calls first).
    for (auto &d : a.defs) {
        if (a.opening.found) break;
        if (d.kind != "script" || d.type != "startup") continue;
        const File &f = a.files[d.file];
        const Node &form = f.forms[d.form];
        std::string pre;
        for (size_t i = body_start(form); i < form.kids.size() && !a.opening.found; i++) {
            const Node &s = form.kids[i];
            const Node *found = nullptr;
            size_t found_file = d.file, found_form = d.form;
            if (skip_if(s)) {
                found = &s;
            } else if (s.list && !head(s).empty()) {
                const Def *called = def_named(a, head(s));
                if (called && called->kind == "script" && called->type == "static") {
                    const Node &cf = a.files[called->file].forms[called->form];
                    for (size_t j = body_start(cf); j < cf.kids.size(); j++)
                        if (skip_if(cf.kids[j])) { found = &cf.kids[j]; found_file = called->file; found_form = called->form; break; }
                }
            }
            if (!found) {
                if (setup.count(head(s))) pre += "    " + slice(f.text, s.b, s.e) + "\n";
                if (head(s) == "sleep_until") break;   // (gameplay before it: not an opening)
                continue;
            }
            Cut &c = a.opening;
            c.found = true;
            c.file = found_file;
            c.form = found_form;
            c.b = found->b;
            c.e = found->e;
            c.script = d.name;
            c.preamble = pre;
            const std::string &ft = a.files[found_file].text;
            for (size_t k = 2; k < found->kids.size(); k++) c.body += statements_text(ft, found->kids[k]);
            c.bsps = bsps_used(c.body);
        }
    }
    // The closing: a script whose last stretch is a cutscene ending the level (map_name or game_won).
    for (auto &d : a.defs) {
        if (a.closing.found) break;
        if (d.kind != "script" || (d.type != "dormant" && d.type != "startup")) continue;
        const File &f = a.files[d.file];
        const Node &form = f.forms[d.form];
        size_t first = body_start(form), end_at = 0, cine = 0;
        bool ends = false, has_cine = false;
        for (size_t i = first; i < form.kids.size(); i++) {
            const std::string &h = head(form.kids[i]);
            if (h == "map_name" || h == "game_won") { ends = true; end_at = i; }
            if (h == "cinematic_start" && !has_cine) { has_cine = true; cine = i; }
        }
        if (!ends || !has_cine || cine > end_at) continue;
        size_t start = first;
        for (size_t i = first; i < cine; i++)
            if (head(form.kids[i]) == "sleep_until") start = i + 1;
        Cut &c = a.closing;
        c.found = true;
        c.file = d.file;
        c.form = d.form;
        c.b = form.kids[start].b;
        c.e = form.kids.back().e;
        c.script = d.name;
        // (what a map just loaded has nothing to do with: the level's AI, gone already)
        static const std::set<std::string> pointless = {"ai_erase", "ai_erase_all", "ai_kill", "ai_kill_silent"};
        for (size_t i = start; i < form.kids.size(); i++)
            if (!pointless.count(head(form.kids[i]))) c.body += "    " + slice(f.text, form.kids[i].b, form.kids[i].e) + "\n";
        c.bsps = bsps_used(c.body);
    }
    auto bsp_list = [](const std::set<int> &s) {
        std::string o;
        for (int b : s) o += (o.empty() ? "" : ", ") + std::to_string(b);
        return o.empty() ? std::string("none") : o;
    };
    int scripts = 0, globals = 0;
    for (auto &d : a.defs) (d.kind == "script" ? scripts : globals)++;
    std::ostringstream r;
    r << scripts << " scripts, " << globals << " globals in " << a.files.size() << " file" << (a.files.size() == 1 ? "" : "s") << ".\n";
    if (a.opening.found) {
        a.opening.about = "the skippable part of " + a.opening.script + " (switches to BSP " + bsp_list(a.opening.bsps) + ")";
        r << "Opening cutscene: " << a.opening.about << ".\n";
    } else {
        r << "Opening cutscene: none found (no startup script with a skippable (cinematic_skip_start) part).\n";
    }
    if (a.closing.found) {
        a.closing.about = "the end of " + a.closing.script + " (switches to BSP " + bsp_list(a.closing.bsps) + ")";
        r << "Closing cutscene: " << a.closing.about << ".\n";
    } else {
        r << "Closing cutscene: none found (no script ending in a cutscene and map_name or game_won).\n";
    }
    a.report = r.str();
}

std::vector<std::string> containing_patterns(const std::string &text) {
    std::set<std::string> out;
    for (auto &f : parse(text))
        walk(f, [&](const Node &n) {
            const std::string &h = head(n);
            if (h.size() > 11 && h.compare(h.size() - 11, 11, "_containing") == 0 && n.kids.size() >= 2 && !n.kids[1].list)
                out.insert(n.kids[1].atom);
        });
    return std::vector<std::string>(out.begin(), out.end());
}

std::vector<std::string> camera_points_set(const std::string &text) {
    std::vector<std::string> out;
    for (auto &f : parse(text))
        walk(f, [&](const Node &n) {
            if (head(n) == "camera_set" && n.kids.size() >= 2 && !n.kids[1].list) out.push_back(n.kids[1].atom);
        });
    return out;
}

std::set<int> bsps_used(const std::string &text) {
    std::set<int> out;
    for (auto &f : parse(text))
        walk(f, [&](const Node &n) {
            if (!n.list) return;
            if (head(n) == "switch_bsp" && n.kids.size() >= 2 && is_int(n.kids[1].atom)) out.insert(std::stoi(n.kids[1].atom));
            bool tests = false;
            for (auto &k : n.kids) if (head(k) == "structure_bsp_index") tests = true;
            if (tests)
                for (auto &k : n.kids) if (!k.list && is_int(k.atom)) out.insert(std::stoi(k.atom));
        });
    return out;
}

std::string renumber_bsps(const std::string &text, const std::map<int, int> &to) {
    std::vector<std::pair<size_t, size_t>> at;   // (atoms to change: their places)
    std::vector<std::string> with;
    auto change = [&](const Node &k) {
        if (k.list || !is_int(k.atom)) return;
        auto it = to.find(std::stoi(k.atom));
        if (it == to.end()) return;
        at.push_back({k.b, k.e});
        with.push_back(std::to_string(it->second));
    };
    for (auto &f : parse(text))
        walk(f, [&](const Node &n) {
            if (!n.list) return;
            if (head(n) == "switch_bsp" && n.kids.size() >= 2) change(n.kids[1]);
            bool tests = false;
            for (auto &k : n.kids) if (head(k) == "structure_bsp_index") tests = true;
            if (tests) for (auto &k : n.kids) change(k);
        });
    std::vector<size_t> order(at.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t x, size_t y) { return at[x].first > at[y].first; });
    std::string out = text;
    for (size_t i : order) out.replace(at[i].first, at[i].second - at[i].first, with[i]);
    return out;
}

std::vector<std::pair<fs::path, std::string>> level_texts(const Analysis &a, bool opening, bool closing,
                                                          const std::string &closing_next, std::set<std::string> &removed,
                                                          std::set<std::string> &kept) {
    removed.clear();
    kept.clear();
    // 1. The pieces taken out.
    std::vector<std::string> texts;
    for (auto &f : a.files) texts.push_back(f.text);
    struct Edit { size_t file, b, e; std::string with; };
    std::vector<Edit> edits;
    if (opening && a.opening.found) {
        edits.push_back({a.opening.file, a.opening.b, a.opening.e, ""});
        for (auto &x : atoms(slice(a.files[a.opening.file].text, a.opening.b, a.opening.e))) removed.insert(x);
    }
    if (closing && a.closing.found) {
        edits.push_back({a.closing.file, a.closing.b, a.closing.e,
                         "(fade_out 0 0 0 0)\n    (map_name \"" + closing_next + "\")"});
        for (auto &x : atoms(slice(a.files[a.closing.file].text, a.closing.b, a.closing.e))) removed.insert(x);
    }
    std::sort(edits.begin(), edits.end(), [](const Edit &x, const Edit &y) { return x.file != y.file ? x.file < y.file : x.b > y.b; });
    for (auto &e : edits) texts[e.file].replace(e.b, e.e - e.b, e.with);
    // 2. The scripts nothing runs any more (dormant and static ones no script left wakes or calls) left out, again and
    // again until none is.
    for (int round = 0; round < 20; round++) {
        Analysis b;
        for (size_t i = 0; i < a.files.size(); i++) {
            File f;
            f.path = a.files[i].path;
            f.text = texts[i];
            f.forms = parse(f.text);
            b.files.push_back(std::move(f));
        }
        find_defs(b);
        std::set<std::string> reached;
        std::vector<const Def *> todo;
        for (auto &d : b.defs)
            if (d.kind == "global" || d.type == "startup" || d.type == "continuous") { reached.insert(d.name); todo.push_back(&d); }
        while (!todo.empty()) {
            const Def *d = todo.back();
            todo.pop_back();
            const Node &form = b.files[d->file].forms[d->form];
            for (auto &x : atoms(slice(b.files[d->file].text, form.b, form.e)))
                if (!reached.count(x))
                    if (const Def *o = def_named(b, x)) { reached.insert(x); todo.push_back(o); }
        }
        std::vector<Edit> gone;
        for (auto &d : b.defs)
            if (d.kind == "script" && !reached.count(d.name)) {
                const Node &form = b.files[d.file].forms[d.form];
                gone.push_back({d.file, form.b, form.e, ""});
                for (auto &x : atoms(slice(b.files[d.file].text, form.b, form.e))) removed.insert(x);
            }
        if (gone.empty()) break;
        std::sort(gone.begin(), gone.end(), [](const Edit &x, const Edit &y) { return x.file != y.file ? x.file < y.file : x.b > y.b; });
        for (auto &e : gone) texts[e.file].replace(e.b, e.e - e.b, e.with);
    }
    std::vector<std::pair<fs::path, std::string>> out;
    for (size_t i = 0; i < a.files.size(); i++) {
        for (auto &x : atoms(texts[i])) kept.insert(x);
        out.push_back({a.files[i].path, texts[i]});
    }
    for (auto it = removed.begin(); it != removed.end();) it = kept.count(*it) ? removed.erase(it) : std::next(it);
    return out;
}

std::string piece_text(const Analysis &a, bool opening, const std::string &next, int start_bsp) {
    const Cut &c = opening ? a.opening : a.closing;
    if (!c.found) return "";
    std::string body = start_bsp > 0 ? "    (switch_bsp " + std::to_string(start_bsp) + ")\n" : "";
    body += c.preamble;
    if (opening) {
        // (still skippable: a skip goes straight on to the level)
        body += "    (if (cinematic_skip_start)\n        (begin\n";
        std::istringstream in(c.body);
        std::string line;
        while (std::getline(in, line)) body += "        " + line + "\n";
        body += "        )\n    )\n    (cinematic_skip_stop)\n    (fade_out 0 0 0 0)\n    (map_name \"" + next + "\")\n";
    } else {
        body += c.body;
    }
    // What it uses: the globals and the scripts it calls or wakes (and theirs).
    std::set<std::string> want = atoms(body), have;
    std::vector<std::string> names(want.begin(), want.end());
    for (size_t i = 0; i < names.size(); i++) {
        const Def *d = def_named(a, names[i]);
        if (!d || have.count(d->name)) continue;
        if (d->kind == "script" && (d->type == "startup" || d->type == "continuous")) continue;
        have.insert(d->name);
        const Node &form = a.files[d->file].forms[d->form];
        for (auto &x : atoms(slice(a.files[d->file].text, form.b, form.e)))
            if (!want.count(x)) { want.insert(x); names.push_back(x); }
    }
    std::string out = ";; " + std::string(opening ? "The opening cutscene" : "The closing cutscene") +
                      ", split off by Halo Map Porter (" + c.about + ")\n\n";
    for (int pass = 0; pass < 2; pass++)   // (globals first, then scripts, in their files' order)
        for (auto &d : a.defs) {
            if (!have.count(d.name) || (pass == 0) != (d.kind == "global")) continue;
            const Node &form = a.files[d.file].forms[d.form];
            out += slice(a.files[d.file].text, form.b, form.e) + "\n\n";
        }
    out += "(script startup campaign_piece\n" + body + ")\n";
    return out;
}

// ---------------------------------------------------------------- the scenario
namespace {
using tagfile::TagNode;
double num(const TagNode &n, const char *f, int k = 0) { return tagfile::get_num(n, f, k); }
}   // namespace

bool strip_scenario(const fs::path &scenario, const Strip &s, Stripped &out) {
    out = Stripped();
    tagfile::EditTag et;
    if (!et.load(scenario)) { out.why = "the scenario couldn't be read"; return false; }
    TagNode &root = et.root();
    std::vector<TagNode> *bsps = tagfile::block(root, "structure bsps");
    if (!bsps) { out.why = "the scenario has no BSP list"; return false; }
    // The BSPs: old index -> new (-1: gone).
    std::vector<int> to(bsps->size(), -1);
    {
        std::vector<TagNode> kept;
        for (int b : s.keep_bsps)
            if (b >= 0 && (size_t)b < bsps->size() && to[(size_t)b] < 0) {
                to[(size_t)b] = (int)kept.size();
                kept.push_back((*bsps)[(size_t)b]);
            }
        if (kept.empty()) { out.why = "no BSP left"; return false; }
        out.bsps = (int)(bsps->size() - kept.size());
        *bsps = std::move(kept);
    }
    auto new_bsp = [&](double v) -> int {   // (an index field: none stays none)
        int o = (int)v;
        if (o < 0 || o >= 65535) return o;
        return (size_t)o < to.size() ? to[(size_t)o] : -1;
    };
    std::set<std::string> also;   // (objects the conversations kept name: placed too)
    auto named = [&](const std::string &n) {
        if (n.empty()) return false;
        if (s.names.count(n) || also.count(n)) return true;
        for (auto &p : s.patterns) if (!p.empty() && n.find(p) != std::string::npos) return true;
        return false;
    };
    // The object names.
    std::vector<std::string> object_names;
    if (auto *on = tagfile::block(root, "object names"))
        for (auto &n : *on) object_names.push_back(lower(tagfile::get_str(n, "name")));
    auto ai_named = [&](const std::string &n) {
        if (n.empty()) return false;
        if (s.names.count(n)) return true;
        for (auto &x : s.names) if (x.size() > n.size() && x.compare(0, n.size(), n) == 0 && x[n.size()] == '/') return true;
        return false;
    };
    // (a piece with no AI left: its vehicle palette goes as the others do; AI picks vehicles by their place in it)
    bool no_ai = s.piece;
    if (auto *enc = tagfile::block(root, "encounters"))
        for (auto &e : *enc) if (ai_named(lower(tagfile::get_str(e, "name")))) no_ai = false;
    // The conversations kept: their participants' objects ("use this object", "set new name") stay placed.
    auto conversation_kept = [&](const std::string &n) {
        return s.piece ? s.names.count(n) > 0 : !(s.drop.count(n) && !s.names.count(n));
    };
    if (auto *conv = tagfile::block(root, "ai conversations"))
        for (auto &c : *conv) {
            if (!conversation_kept(lower(tagfile::get_str(c, "name")))) continue;
            if (auto *pp = tagfile::block(c, "participants"))
                for (auto &p : *pp)
                    for (const char *f : {"use this object", "set new name"}) {
                        int i = (int)num(p, f);
                        if (i >= 0 && (size_t)i < object_names.size() && !object_names[(size_t)i].empty()) also.insert(object_names[(size_t)i]);
                    }
        }
    // The placements, and their palettes' entries no placement uses any more.
    struct Kind { const char *block, *palette; bool stands, prune; };
    static const Kind kinds[] = {
        {"scenery", "scenery palette", true, true},           {"bipeds", "biped palette", true, true},
        {"vehicles", "vehicle palette", false, false},        {"equipment", "equipment palette", false, true},
        {"weapons", "weapon palette", false, true},           {"machines", "machine palette", true, true},
        {"controls", "control palette", true, true},          {"light fixtures", "light fixture palette", true, true},
        {"sound scenery", "sound scenery palette", true, true}};
    for (auto &k : kinds) {
        std::vector<TagNode> *items = tagfile::block(root, k.block), *pal = tagfile::block(root, k.palette);
        if (!items || !pal) continue;
        std::vector<TagNode> left;
        size_t before = items->size();
        // where each stands (the BSPs: kept, gone), when its own bsp indices don't say
        std::vector<int> where(items->size(), 1);
        if (s.inside) {
            std::vector<std::array<double, 3>> pts;
            for (auto &p : *items) pts.push_back({num(p, "position", 0), num(p, "position", 1), num(p, "position", 2)});
            std::vector<int> got = s.inside(pts);
            if (got.size() == where.size()) where = got;
        }
        for (size_t pi = 0; pi < items->size(); pi++) {
            TagNode &p = (*items)[pi];
            int ni = (int)num(p, "name");
            std::string nm = ni >= 0 && (size_t)ni < object_names.size() ? object_names[(size_t)ni] : "";
            unsigned bits = (unsigned)num(p, "bsp indices");
            bool in_kept = false, in_gone = false;
            unsigned nb = 0;
            for (size_t b = 0; b < to.size() && b < 16; b++)
                if (bits & (1u << b)) {
                    if (to[b] >= 0) { in_kept = true; nb |= 1u << to[b]; }
                    else in_gone = true;
                }
            if (!bits) { in_kept = (where[pi] & 1) != 0; in_gone = (where[pi] & 2) != 0; }
            bool keep;
            // (a piece: what its scripts name, and what stands in its BSPs; the level: all but what only the pieces
            // named, and what stands only in the BSPs that went)
            if (s.piece) keep = !nm.empty() ? named(nm) : (k.stands && in_kept);
            else keep = named(nm) || (!(!nm.empty() && s.drop.count(nm)) && (in_kept || !in_gone));
            if (!keep) continue;
            if (bits) tagfile::set_num(p, "bsp indices", nb);
            left.push_back(p);
        }
        const size_t gone_here = before - left.size();
        out.placements += (int)gone_here;
        *items = std::move(left);
        const bool prune = k.prune || (no_ai && std::string(k.palette) == "vehicle palette");
        if (!prune || (!s.piece && gone_here == 0)) continue;   // (the level: only palettes it took placements from)
        std::vector<int> used(pal->size(), 0), remap(pal->size(), -1);
        for (auto &p : *items) {
            int t = (int)num(p, "type");
            if (t >= 0 && (size_t)t < used.size()) used[(size_t)t] = 1;
        }
        std::vector<TagNode> kept_pal;
        for (size_t i = 0; i < pal->size(); i++)
            if (used[i]) { remap[i] = (int)kept_pal.size(); kept_pal.push_back((*pal)[i]); }
        if (kept_pal.size() == pal->size()) continue;
        out.palette += (int)(pal->size() - kept_pal.size());
        *pal = std::move(kept_pal);
        for (auto &p : *items) {
            int t = (int)num(p, "type");
            if (t >= 0 && (size_t)t < remap.size()) tagfile::set_num(p, "type", remap[(size_t)t]);
        }
    }
    // Encounters and conversations: by name ("enc_k_covenant/boss" names its encounter).
    auto ai_dropped = [&](const std::string &n) {
        if (s.drop.count(n)) return true;
        for (auto &x : s.drop) if (x.size() > n.size() && x.compare(0, n.size(), n) == 0 && x[n.size()] == '/') return true;
        return false;
    };
    if (auto *enc = tagfile::block(root, "encounters")) {
        std::vector<TagNode> left;
        for (auto &e : *enc) {
            std::string n = lower(tagfile::get_str(e, "name"));
            bool keep = s.piece ? ai_named(n) : !(ai_dropped(n) && !ai_named(n));
            if (!keep) { out.encounters++; continue; }
            int mb = (int)num(e, "manual bsp index");
            if (mb >= 0 && mb < 65535) tagfile::set_num(e, "manual bsp index", std::max(0, new_bsp(mb)));
            left.push_back(e);
        }
        *enc = std::move(left);
    }
    if (auto *conv = tagfile::block(root, "ai conversations")) {
        std::vector<TagNode> left;
        for (auto &c : *conv) {
            std::string n = lower(tagfile::get_str(c, "name"));
            if (!conversation_kept(n)) { out.conversations++; continue; }
            left.push_back(c);
        }
        *conv = std::move(left);
    }
    // The actors the encounters left use (their squads' and starting locations'; a conversation's participant's
    // "actor type" is a kind of actor, not one of these).
    if (auto *actors = tagfile::block(root, "actor palette")) {
        std::vector<int> used(actors->size(), 0);
        auto mark = [&](TagNode &n) {
            int t = (int)num(n, "actor type");
            if (t >= 0 && (size_t)t < used.size()) used[(size_t)t] = 1;
        };
        if (auto *enc = tagfile::block(root, "encounters"))
            for (auto &e : *enc)
                if (auto *sq = tagfile::block(e, "squads"))
                    for (auto &q : *sq) {
                        mark(q);
                        if (auto *st = tagfile::block(q, "starting locations")) for (auto &l : *st) mark(l);
                    }
        std::vector<int> remap(actors->size(), -1);
        std::vector<TagNode> kept;
        for (size_t i = 0; i < actors->size(); i++)
            if (used[i]) { remap[i] = (int)kept.size(); kept.push_back((*actors)[i]); }
        if (kept.size() < actors->size()) {
            out.actors = (int)(actors->size() - kept.size());
            *actors = std::move(kept);
            auto fix = [&](TagNode &n) {
                int t = (int)num(n, "actor type");
                if (t >= 0 && (size_t)t < remap.size()) tagfile::set_num(n, "actor type", remap[(size_t)t] < 0 ? 65535 : remap[(size_t)t]);
            };
            if (auto *enc = tagfile::block(root, "encounters"))
                for (auto &e : *enc)
                    if (auto *sq = tagfile::block(e, "squads"))
                        for (auto &q : *sq) {
                            fix(q);
                            if (auto *st = tagfile::block(q, "starting locations")) for (auto &l : *st) fix(l);
                        }
        }
    }
    // What else names a BSP by number.
    if (auto *sw = tagfile::block(root, "bsp switch trigger volumes")) {
        std::vector<TagNode> left;
        if (!s.piece)
            for (auto &v : *sw) {
                int a = new_bsp(num(v, "source")), b = new_bsp(num(v, "destination"));
                if (a < 0 || b < 0) continue;
                tagfile::set_num(v, "source", a);
                tagfile::set_num(v, "destination", b);
                left.push_back(v);
            }
        *sw = std::move(left);
    }
    if (auto *ps = tagfile::block(root, "player starting locations"))
        for (auto &p : *ps) {
            int b = (int)num(p, "bsp index");
            if (b >= 0 && b < 65535) tagfile::set_num(p, "bsp index", std::max(0, new_bsp(b)));
        }
    if (auto *cl = tagfile::block(root, "command lists"))
        for (auto &c : *cl)
            for (const char *f : {"manual bsp index", "precomputed bsp index"}) {
                int b = (int)num(c, f);
                if (b >= 0 && b < 65535) {
                    int nb = new_bsp(b);
                    tagfile::set_num(c, f, nb < 0 ? 65535 : nb);
                }
            }
    if (!et.save(scenario)) { out.why = "the scenario couldn't be saved"; return false; }
    return true;
}

}   // namespace campaign
