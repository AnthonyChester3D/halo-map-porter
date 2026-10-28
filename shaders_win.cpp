// Halo Map Porter - the Xbox tab's "Level shaders" window (Windows). See shaders_win.hpp.
#include "shaders_win.hpp"

#include <commctrl.h>
#include <windowsx.h>

#include "botnodes.hpp"
#include "glpreview_win.hpp"
#include "levelshaders.hpp"
#include "showcase.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace shadersui {
namespace fs = std::filesystem;

namespace {

enum {
    IDC_MODE0 = 400, IDC_MODE1, IDC_MODE2, IDC_STATUS, IDC_LIST, IDC_VIEW, IDC_PREV, IDC_NEXT, IDC_BEFORE, IDC_AFTER,
    IDC_CHANGES, IDC_HILITE, IDC_COST, IDC_REMEASURE, IDC_OKB, IDC_CANCELB, IDC_ABOUT, IDC_PORTALS, IDC_FOG_LBL, IDC_FOG,
    IDC_FAR_LBL, IDC_FAR, IDC_FOG_DIST, IDC_FOG_UNITS, IDC_WEATHER_LBL, IDC_WEATHER, IDC_RIDERS_LBL, IDC_RIDERS, IDC_PROPS_LBL,
    IDC_PROPS, IDC_SKY_LBL, IDC_SKY, IDC_FX_LBL, IDC_FX, IDC_LIGHTS_LBL, IDC_LIGHTS,
    IDC_SHOWCASE, IDC_SC_LIST, IDC_SC_PLAY, IDC_SC_PLAYSHOT, IDC_SC_STOP, IDC_SC_NEW, IDC_SC_ADD, IDC_SC_UNDO, IDC_SC_SECS_LBL,
    IDC_SC_SECS, IDC_SC_LOOK, IDC_SC_DELETE, IDC_SC_UP, IDC_SC_DOWN, IDC_SC_REMAKE, IDC_SC_NAME_LBL, IDC_SC_NAME,
    IDC_SC_KEYS_LBL, IDC_SC_KPREV, IDC_SC_KEY, IDC_SC_KNEXT, IDC_SC_KGO, IDC_SC_KMOVE, IDC_SC_ORBIT_LBL, IDC_SC_ORAD_LBL,
    IDC_SC_ORAD, IDC_SC_OHGT_LBL, IDC_SC_OHGT, IDC_SC_OAR_LBL, IDC_SC_OAR, IDC_SC_ORBIT, IDC_SC_SAVE, IDC_SC_PACE, IDC_SC_HINT,
    IDC_FOG_START, IDC_FOG_TO, IDC_OBJECTS,
    IDC_BOTS, IDC_BOTS_TEXT, IDC_BOTS_LIST, IDC_BOTS_DEL, IDC_BOTS_CLEAR, IDC_BOTS_NEXT, IDC_BOTS_THROUGH, IDC_BOTS_GOOD,
    IDC_BOTS_HINT,
};
#define WM_LS_LOG (WM_APP + 60)
#define WM_LS_DONE (WM_APP + 61)

struct Data {
    lshade::Analysis a;
    lshade::Preview p;
};

struct State {
    HWND wnd = nullptr, list = nullptr, view = nullptr;
    int dpi = 96;
    HFONT font = nullptr, bold = nullptr;
    bool done = false, ok = false, filling = false;
    std::string key;
    Measure measure;
    Picks picks;
    SaveShowcase save_showcase;
    std::shared_ptr<Data> data;
    std::vector<int> row_shader;   // list row -> shader
    int selected = -1;             // shader
    std::vector<int> views;        // the views the arrows go through
    size_t view_at = 0;
    int show = 1;                  // 0 as made, 1 as converted, 2 what changes
    bool highlight = true, portals = false;
    bool objects = true;   // (the objects in the level drawn; remembered while the app runs: g_objects)
    HWND tips = nullptr;
    // the camera: the view's spot, turned by dragging and flown with the keys
    lshade::Camera cam;
    bool dragging = false;
    POINT drag_from{};
    // the picture: on the graphics card, or (without OpenGL 2) drawn here
    std::unique_ptr<glpreview::Renderer> gl;
    std::vector<uint32_t> px;
    int pw = 0, ph = 0;
    // the frame loop (while the camera moves): a timer that moves it by the keys held and redraws
    bool ticking = false, dirty = false, coarse = false;
    LARGE_INTEGER last_tick{};
    bool keep_camera = false;   // (a shader selected by double-clicking the picture: the view stays where it is)
    // The showcase flythrough (showcase.hpp; the "Showcase flythrough" button swaps the list for its shots): made from
    // the level, or this map's own once changed here (`custom`); one shot or all of them played in the picture.
    bool sc_mode = false, sc_ready = false, sc_custom = false, sc_filling = false;
    showcase::Showcase sc;
    std::string sc_report;
    int sc_sel = -1;
    int sc_key = 0;   // (the selected shot's key being changed: in Shot::keys)
    // The orbit boxes: "New orbit here" makes one with them, and they change the selected shot when it's an orbit.
    double orbit_radius = 5, orbit_height = 2, orbit_around = 100;
    HWND sc_list = nullptr;
    bool playing = false;
    int play_shot = -1;   // (-1: all of them, round and round)
    LARGE_INTEGER play_start{};
    fs::path file;   // the measurement shown
    // The bots' waypoints ("Bot waypoints..." swaps the list for them; botnodes.hpp): the map's points as the
    // measurement found them (in the bots' order; yours go in at `bot_insert`, after its item spawn points), its level's
    // collision, and the bots' waypoints and links with yours, as a conversion makes them.
    bool bots_mode = false, bots_loaded = false, bots_through = true, bots_good = true;
    std::vector<botnodes::V3> bot_points;
    std::vector<std::string> bot_kinds;
    int bot_tele = 0;
    size_t bot_insert = 0;
    fs::path bot_bsp;
    std::string bot_why;   // (none: why)
    botnodes::Result bot_res;
    std::vector<int> bot_mine_at;   // each of yours: its waypoint (in bot_res.waypoints), or -1
    std::vector<char> bot_mine_merged;   // (that waypoint an earlier point's: within 1 unit of it)
    int bot_sel = -1;               // (one of yours, picked)
    size_t bot_next = 0;            // ("Next problem": the last one shown)
    bool click = false;             // (a press in the picture that hasn't moved: a click)
    POINT click_at{};
};
State *St = nullptr;

std::mutex g_lock;
std::map<std::string, fs::path> g_cache;   // key -> level shaders file
std::atomic<bool> g_measuring{false};
std::atomic<HWND> g_target{nullptr};
std::string g_measuring_key;   // (under g_lock)

// What the measuring thread sends the window: a line of its log, or that it's done.
struct Note { std::string key, text; fs::path file; };
void post(UINT msg, WPARAM wp, Note *n) {
    HWND w = g_target;
    if (!w || !PostMessageW(w, msg, wp, (LPARAM)n)) delete n;
}
fs::path g_disk;
bool g_objects = true;   // (State::objects, for the next window)

int S(int v) { return MulDiv(v, St->dpi, 96); }
std::wstring W(const std::string &s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
std::string U(const std::wstring &w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
HWND item(int id) { return GetDlgItem(St->wnd, id); }
std::wstring thousands(long long n) {
    std::wstring s = std::to_wstring(n < 0 ? -n : n);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, L",");
    return (n < 0 ? L"-" : L"") + s;
}
std::wstring pct(double v) {
    wchar_t b[32];
    if (v <= 0) return L"0%";
    if (v < 0.001) return L"<0.1%";
    swprintf(b, 32, v < 0.1 ? L"%.1f%%" : L"%.0f%%", v * 100);
    return b;
}

// Measurements on disk: <hash of key>.lsh with the key itself in <hash>.key next to it.
fs::path disk_name(const std::string &key) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : key) { h ^= c; h *= 1099511628211ull; }
    char hex[24];
    snprintf(hex, sizeof hex, "%016llx", (unsigned long long)h);
    return g_disk / (std::string(hex) + ".lsh");
}
fs::path cached_locked(const std::string &key) {
    auto it = g_cache.find(key);
    if (it != g_cache.end() && fs::exists(it->second)) return it->second;
    if (g_disk.empty()) return {};
    fs::path lsh = disk_name(key), kf = lsh;
    kf.replace_extension(".key");
    std::ifstream f(kf, std::ios::binary);
    std::string k((std::istreambuf_iterator<char>(f)), {});
    if (k != key || !fs::exists(lsh)) return {};
    g_cache[key] = lsh;
    return lsh;
}
// Keeps a copy of a measurement (the conversion's own file is overwritten by the next one with that scenario name).
void keep_locked(const std::string &key, const fs::path &file) {
    if (g_disk.empty()) { g_cache[key] = file; return; }
    std::error_code ec;
    fs::create_directories(g_disk, ec);
    fs::path lsh = disk_name(key), kf = lsh;
    kf.replace_extension(".key");
    if (!fs::copy_file(file, lsh, fs::copy_options::overwrite_existing, ec)) { g_cache[key] = file; return; }
    for (const char *side : {".bots", ".bots.scenario_structure_bsp"}) {   // (the bots' waypoints' data beside it)
        fs::path from = file, to = lsh;
        from += side;
        to += side;
        if (fs::exists(from, ec)) fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
        else fs::remove(to, ec);
    }
    std::ofstream(kf, std::ios::binary) << key;
    g_cache[key] = lsh;
}

// Whether shader `s`'s reflection goes, as the window's choices stand.
bool dropped(int s) {
    const auto &sh = St->data->a.shaders[(size_t)s];
    if (sh.reflection_passes == 0) return false;
    if (St->picks.mode == 0) return sh.suggested;
    if (St->picks.mode == 1) return St->picks.drops.count(sh.tag) > 0;
    return false;
}
std::vector<char> dropped_all() {
    std::vector<char> d(St->data->a.shaders.size(), 0);
    for (size_t s = 0; s < d.size(); s++) d[s] = dropped((int)s);
    return d;
}

// The fog the picture shows from `c`: the level's (its room's sky's, else the indoor fog), as the conversion leaves it
// with the window's fog choice (xbox_fog in core.cpp).
lshade::Fog fog_for(const lshade::Camera &c) {
    bool outdoor = false;
    lshade::Fog f = St->data->p.fog_at(c.eye, &outdoor);
    if (!f.any() || St->picks.fog == 1) return f;
    if (St->picks.fog == 2 && outdoor) {
        f.density = 1;
        f.opaque = std::max(f.opaque * (float)std::clamp(St->picks.fog_scale, 0.1, 1.0), f.start + 1.0f);
    } else if (St->picks.fog == 3 && outdoor) {
        f.density = 1;
        float o = St->picks.fog_distance > 0 ? (float)St->picks.fog_distance : f.opaque;
        if (St->picks.fog_start >= 0) f.start = std::clamp((float)St->picks.fog_start, 0.0f, std::max(0.0f, o - 1.0f));
        f.opaque = std::max(o, f.start + 1.0f);
    } else if (f.density >= 0.9f && f.density < 1.0f) {
        f.density = 1;
    }
    return f;
}

void bots_frame(glpreview::Frame &f);
void set_bots_mode(bool on);

// Draws the picture: on the graphics card when it can (fast enough to fly through), else here (`fast`: at half the
// size, while the camera moves).
void render(bool fast) {
    if (!St->data) return;
    std::vector<char> all(St->data->a.shaders.size(), 1), refl = all;
    if (St->show != 0)
        for (size_t s = 0; s < refl.size(); s++) refl[s] = !dropped((int)s);
    lshade::Camera c = St->cam;
    if (St->gl) {
        glpreview::Frame f;
        f.camera = c;
        f.made = all;
        f.converted = refl;
        f.what_it_loses = St->show == 2;
        f.highlight = St->highlight ? St->selected : -1;
        f.portals = St->portals;
        f.objects = St->objects;
        f.fog_on = true;
        f.fog = fog_for(c);
        if (St->sc_mode) {   // (the showcase: the objects' boxes, and the selected shot's path, keys and look point)
            f.boxes = &St->data->p.boxes;
            if (!St->playing && St->sc_sel >= 0 && (size_t)St->sc_sel < St->sc.shots.size()) {
                const showcase::Shot &shot = St->sc.shots[(size_t)St->sc_sel];
                for (auto &q : showcase::path(shot, 0.1)) f.path.push_back({(float)q[0], (float)q[1], (float)q[2]});
                for (auto &k : showcase::path_keys(shot)) f.keys.push_back({(float)k.p[0], (float)k.p[1], (float)k.p[2]});
                f.key = St->sc_key >= 0 && (size_t)St->sc_key < showcase::path_keys(shot).size() ? St->sc_key : -1;
                f.look = shot.look;
                f.look_at = {(float)shot.at[0], (float)shot.at[1], (float)shot.at[2]};
            }
        }
        if (St->bots_mode) bots_frame(f);
        St->gl->draw(f);
        ValidateRect(St->view, nullptr);
        return;
    }
    RECT rc;
    GetClientRect(St->view, &rc);
    int w = rc.right, h = rc.bottom;
    if (fast) { w /= 2; h /= 2; }
    lshade::Extras ex;
    ex.portals = St->portals;
    ex.objects = St->objects;
    ex.fog_on = true;
    ex.fog = fog_for(c);
    if (St->show == 2) {
        // What changes: the converted picture dimmed, with what it loses (the dropped reflections) brightened on top.
        lshade::Extras fog_only = ex;
        fog_only.portals = false;
        std::vector<uint32_t> made;
        lshade::draw(St->data->p, c, all, -1, w, h, made, nullptr, nullptr, &fog_only);
        lshade::draw(St->data->p, c, refl, -1, w, h, St->px, nullptr, nullptr, &fog_only);
        for (size_t i = 0; i < St->px.size() && i < made.size(); i++) {
            uint32_t a = made[i], b = St->px[i], o = 0;
            for (int sh = 0; sh < 24; sh += 8) {
                int ca = (int)(a >> sh & 255), cb = (int)(b >> sh & 255);
                int v = cb * 3 / 10 + std::abs(ca - cb) * 4;
                o |= (uint32_t)std::min(v, 255) << sh;
            }
            St->px[i] = o;
        }
    } else {
        lshade::draw(St->data->p, c, refl, St->highlight ? St->selected : -1, w, h, St->px, nullptr, nullptr, &ex);
    }
    St->pw = w;
    St->ph = h;
    InvalidateRect(St->view, nullptr, FALSE);
}

// ---- the showcase flythrough (showcase.hpp): its shots in place of the list, played and changed in the picture
void start_ticking();
double deg_of(double r) { return r * 180 / 3.14159265358979; }
double rad_of(double d) { return d * 3.14159265358979 / 180; }
showcase::Key key_here() {
    showcase::Key k;
    k.p = St->cam.eye;
    k.yaw = deg_of(St->cam.yaw);
    k.pitch = deg_of(St->cam.pitch);
    return k;
}
void camera_to(const showcase::View &v) {
    St->cam.eye = v.eye;
    St->cam.yaw = rad_of(v.yaw);
    St->cam.pitch = std::clamp(rad_of(v.pitch), -1.5, 1.5);
}
std::wstring secs(double v) {
    wchar_t b[32];
    swprintf(b, 32, L"%.1f", v);
    return b;
}
std::wstring sc_summary() {
    return std::to_wstring(St->sc.shots.size()) + (St->sc.shots.size() == 1 ? L" shot, " : L" shots, ") +
           secs(St->sc.length()) + L" seconds";
}
void sc_status() {
    std::wstring t;
    if (St->sc.shots.empty())
        t = St->sc_custom ? L"No shots: add some with \"New shot here\", or \"Make again\" for shots made from the level."
                          : L"No shots could be made from the level (" + W(St->sc_report) + L"): add your own with \"New shot here\".";
    else if (St->sc_custom)
        t = L"This map's own showcase flythrough: " + sc_summary() + L" (changed here).";
    else
        t = L"Showcase flythrough made from the level: " + W(St->sc_report) + L".";
    SetWindowTextW(item(IDC_STATUS), t.c_str());
}
void sc_select(int row, bool move_camera);
void sc_fill() {
    St->sc_filling = true;
    ListView_DeleteAllItems(St->sc_list);
    for (size_t i = 0; i < St->sc.shots.size(); i++) {
        const showcase::Shot &shot = St->sc.shots[i];
        std::wstring name = std::to_wstring(i + 1) + L". " + W(shot.label.empty() ? std::string("shot") : shot.label);
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        it.pszText = (LPWSTR)name.c_str();
        ListView_InsertItem(St->sc_list, &it);
        std::wstring cells[3] = {secs(shot.seconds), std::to_wstring(showcase::path_keys(shot).size()),
                                 shot.orbit.on ? L"circling it" : shot.look ? L"a point" : L""};
        for (int c = 0; c < 3; c++) ListView_SetItemText(St->sc_list, (int)i, c + 1, (LPWSTR)cells[c].c_str());
    }
    if (St->sc_sel >= (int)St->sc.shots.size()) St->sc_sel = (int)St->sc.shots.size() - 1;
    if (St->sc_sel >= 0) {
        ListView_SetItemState(St->sc_list, St->sc_sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(St->sc_list, St->sc_sel, FALSE);
    }
    St->sc_filling = false;
    sc_select(St->sc_sel, false);
}
showcase::Shot *sc_shot() { return St->sc_sel >= 0 && (size_t)St->sc_sel < St->sc.shots.size() ? &St->sc.shots[(size_t)St->sc_sel] : nullptr; }
std::wstring num_text(double v) {
    wchar_t b[32];
    swprintf(b, 32, L"%g", std::round(v * 10) / 10);
    return b;
}
// The selected key: which of the shot's it is, and when.
void sc_key_show() {
    showcase::Shot *shot = sc_shot();
    const std::vector<showcase::Key> *keys = shot ? &showcase::path_keys(*shot) : nullptr;
    bool have = keys && !keys->empty();
    if (have) St->sc_key = std::clamp(St->sc_key, 0, (int)keys->size() - 1);
    std::wstring t = have ? L"Key " + std::to_wstring(St->sc_key + 1) + L" of " + std::to_wstring(keys->size()) + L", at " +
                                secs((*keys)[(size_t)St->sc_key].t) + L" s"
                          : L"";
    SetWindowTextW(item(IDC_SC_KEY), t.c_str());
    EnableWindow(item(IDC_SC_KPREV), have && St->sc_key > 0);
    EnableWindow(item(IDC_SC_KNEXT), have && St->sc_key + 1 < (int)keys->size());
    for (int id : {IDC_SC_KGO, IDC_SC_KMOVE, IDC_SC_ADD}) EnableWindow(item(id), have);
    EnableWindow(item(IDC_SC_UNDO), have && keys->size() > 1);
}
// The orbit boxes: the selected shot's when it's an orbit, else the ones "New orbit here" uses.
void sc_orbit_show() {
    showcase::Shot *shot = sc_shot();
    if (shot && shot->orbit.on) {
        St->orbit_radius = shot->orbit.radius;
        St->orbit_height = shot->orbit.height;
        St->orbit_around = shot->orbit.sweep;
    }
    St->sc_filling = true;
    SetWindowTextW(item(IDC_SC_ORAD), num_text(St->orbit_radius).c_str());
    SetWindowTextW(item(IDC_SC_OHGT), num_text(St->orbit_height).c_str());
    SetWindowTextW(item(IDC_SC_OAR), num_text(St->orbit_around).c_str());
    St->sc_filling = false;
    SetWindowTextW(item(IDC_SC_ORBIT_LBL), shot && shot->orbit.on ? L"This orbit:" : L"Orbit:");
}
// A shot changed here: what's wrong with it, if anything, in the status line (else the usual line).
void sc_check(const showcase::Shot &shot) {
    std::string t = showcase::trouble(St->data->p, shot);
    if (t.empty()) return;
    std::wstring line = L"Shot " + std::to_wstring(St->sc_sel + 1) + L": " + W(t) + L".";
    SetWindowTextW(item(IDC_STATUS), line.c_str());
}
// The selected shot: its name, seconds, look, keys and orbit in the boxes under the list; the camera to its start.
void sc_select(int row, bool move_camera) {
    if (row != St->sc_sel) St->sc_key = 0;
    St->sc_sel = row;
    showcase::Shot *shot = sc_shot();
    bool have = shot != nullptr;
    St->sc_filling = true;
    SetWindowTextW(item(IDC_SC_SECS), have ? secs(shot->seconds).c_str() : L"");
    SetWindowTextW(item(IDC_SC_NAME), have ? W(shot->label).c_str() : L"");
    CheckDlgButton(St->wnd, IDC_SC_LOOK, have && shot->look ? BST_CHECKED : BST_UNCHECKED);
    {
        int pace = !have ? -1 : shot->pace == showcase::Pace::even ? 0 : shot->pace == showcase::Pace::in ? 1
                              : shot->pace == showcase::Pace::out ? 2 : shot->pace == showcase::Pace::both ? 3 : 4;
        SendMessageW(item(IDC_SC_PACE), CB_SETCURSEL, (WPARAM)pace, 0);
    }
    St->sc_filling = false;
    for (int id : {IDC_SC_PLAYSHOT, IDC_SC_SECS, IDC_SC_NAME, IDC_SC_LOOK, IDC_SC_DELETE, IDC_SC_UP, IDC_SC_DOWN, IDC_SC_PACE})
        EnableWindow(item(id), have);
    EnableWindow(item(IDC_SC_PLAY), !St->sc.shots.empty());
    sc_key_show();
    sc_orbit_show();
    if (have && move_camera && !St->playing) camera_to(showcase::at(*shot, 0));
    render(false);
}
// A change made here: the shots are this map's own from now on.
void sc_changed() {
    St->sc_custom = true;
    sc_fill();
    sc_status();
}
void sc_prepare() {
    if (St->sc_ready || !St->data) return;
    showcase::Showcase own;
    if (!St->picks.showcase.empty() && showcase::read(St->picks.showcase, own)) {
        St->sc = own;
        St->sc_custom = true;
    } else {
        St->sc = showcase::generate(St->data->p, &St->sc_report);
        St->sc_custom = false;
    }
    St->sc_sel = St->sc.shots.empty() ? -1 : 0;
    St->sc_ready = true;
}
void sc_stop() {
    if (!St->playing) return;
    St->playing = false;
    EnableWindow(item(IDC_SC_STOP), FALSE);
    sc_status();
    render(false);
}
void sc_play(int shot) {
    if (St->sc.shots.empty()) return;
    St->playing = true;
    St->play_shot = shot;
    QueryPerformanceCounter(&St->play_start);
    EnableWindow(item(IDC_SC_STOP), TRUE);
    SetFocus(St->view);
    start_ticking();
}
// Each frame while playing: the camera where the shot has it now (one shot once; all of them round and round).
void sc_tick() {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    double t = (double)(now.QuadPart - St->play_start.QuadPart) / (double)freq.QuadPart;
    const auto &shots = St->sc.shots;
    size_t i = 0;
    if (St->play_shot >= 0 && (size_t)St->play_shot < shots.size()) {
        i = (size_t)St->play_shot;
        if (t >= shots[i].seconds) {
            camera_to(showcase::at(shots[i], shots[i].seconds));
            St->dirty = true;
            sc_stop();
            return;
        }
    } else {
        double total = St->sc.length();
        if (total <= 0) { sc_stop(); return; }
        t = std::fmod(t, total);
        while (i + 1 < shots.size() && t >= shots[i].seconds) t -= shots[i++].seconds;
    }
    camera_to(showcase::at(shots[i], t));
    St->dirty = true;
    static int shown = -1;
    int tenth = (int)(t * 4) + (int)i * 100000;
    if (tenth != shown) {
        shown = tenth;
        std::wstring line = L"Playing shot " + std::to_wstring(i + 1) + L" of " + std::to_wstring(shots.size()) + L", " +
                            W(shots[i].label) + L": " + secs(t) + L" of " + secs(shots[i].seconds) + L" seconds. "
                            L"Click the picture or Stop to stop.";
        SetWindowTextW(item(IDC_STATUS), line.c_str());
    }
}
std::wstring g_status_text;   // (the list's status line, back when leaving the showcase)
void fog_cut_status();
void set_sc_mode(bool on) {
    if (on && !St->data) return;
    if (on && St->bots_mode) set_bots_mode(false);
    if (on) sc_prepare();
    else sc_stop();
    St->sc_mode = on;
    ShowWindow(St->list, on ? SW_HIDE : SW_SHOW);
    for (int id = IDC_SC_LIST; id <= IDC_SC_HINT; id++) ShowWindow(item(id), on ? SW_SHOW : SW_HIDE);
    SetWindowTextW(item(IDC_SHOWCASE), on ? L"Back to the reflections" : L"Showcase flythrough...");
    if (on) {
        sc_fill();
        sc_status();
        sc_select(St->sc_sel, true);
        EnableWindow(item(IDC_SC_STOP), FALSE);
    } else {
        SetWindowTextW(item(IDC_STATUS), g_status_text.c_str());
        fog_cut_status();
        render(false);
    }
}
// ---- the bots' waypoints (botnodes.hpp): in place of the list, drawn in the picture, yours added by clicking it
const uint32_t kMapMark = 0xFF4A90FF, kMineMark = 0xFF36F07A, kAutoMark = 0xFFFF8C1A, kPickedMark = 0xFFFFFFFF;
const uint32_t kGoodLine = 0x9050D070, kRoundLine = 0xB0F0B030, kStuckLine = 0xFFFF3A3A;

// A point's place in the picture (false: behind the camera), as lshade::draw projects.
bool project(const botnodes::V3 &p, int w, int h, double &sx, double &sy) {
    const double tx = std::tan(35.0 * 3.14159265358979 / 180), ty = tx * h / std::max(1, w);
    const auto &c = St->cam;
    double fwd[3] = {std::cos(c.yaw) * std::cos(c.pitch), std::sin(c.yaw) * std::cos(c.pitch), std::sin(c.pitch)};
    double right[3] = {fwd[1], -fwd[0], 0};
    double rl = std::sqrt(right[0] * right[0] + right[1] * right[1]);
    if (rl < 1e-9) return false;
    right[0] /= rl;
    right[1] /= rl;
    double up[3] = {right[1] * fwd[2] - right[2] * fwd[1], right[2] * fwd[0] - right[0] * fwd[2], right[0] * fwd[1] - right[1] * fwd[0]};
    double d[3] = {p[0] - c.eye[0], p[1] - c.eye[1], p[2] - c.eye[2]};
    double z = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
    if (z < 0.05) return false;
    sx = ((d[0] * right[0] + d[1] * right[1]) / z / tx * 0.5 + 0.5) * w;
    sy = (0.5 - (d[0] * up[0] + d[1] * up[1] + d[2] * up[2]) / z / ty * 0.5) * h;
    return true;
}

// The two ends of a link (one each way), and how it goes: 0 both ways walked, 1 a way the search can't finish but the
// bots go round, 2 one with nothing round it.
struct BotLink { int a, b, how; };
std::vector<BotLink> bot_links() {
    std::map<std::pair<int, int>, int> how;
    for (auto &h : St->bot_res.hops) {
        auto k = std::make_pair(std::min(h.a, h.b), std::max(h.a, h.b));
        int v = h.ok ? 0 : h.around ? 1 : 2;
        auto it = how.find(k);
        if (it == how.end()) how[k] = v;
        else it->second = std::max(it->second, v);
    }
    std::vector<BotLink> out;
    for (auto &[k, v] : how) out.push_back({k.first, k.second, v});
    return out;
}

void bots_frame(glpreview::Frame &f) {
    if (!St->bots_loaded || !St->bot_res.ok) return;
    const auto &r = St->bot_res;
    f.through = St->bots_through;
    auto up = [](const botnodes::V3 &p, double dz) { return std::array<float, 3>{(float)p[0], (float)p[1], (float)(p[2] + dz)}; };
    for (auto &l : bot_links()) {
        if (l.a >= (int)r.waypoints.size() || l.b >= (int)r.waypoints.size()) continue;
        if (l.how == 0 && !St->bots_good) continue;
        uint32_t c = l.how == 0 ? kGoodLine : l.how == 1 ? kRoundLine : kStuckLine;
        f.lines.push_back({up(r.waypoints[(size_t)l.a], 0.3), up(r.waypoints[(size_t)l.b], 0.3), c, l.how == 2 ? 3.0f : 1.5f});
    }
    std::vector<char> mine(r.waypoints.size(), 0);
    for (size_t k = 0; k < St->bot_mine_at.size(); k++) {
        int i = St->bot_mine_at[k];
        if (i >= 0 && (size_t)i < mine.size() && !St->bot_mine_merged[k]) mine[(size_t)i] = 1;
    }
    for (size_t i = 0; i < r.waypoints.size(); i++) {
        uint32_t c = mine[i] ? kMineMark : (int)i < r.given ? kMapMark : kAutoMark;
        f.marks.push_back({up(r.waypoints[i], 0.3), c, mine[i] ? 11.0f : 9.0f});
    }
    if (St->bot_sel >= 0 && (size_t)St->bot_sel < St->bot_mine_at.size()) {
        int i = St->bot_mine_at[(size_t)St->bot_sel];
        botnodes::V3 p = i >= 0 && (size_t)i < r.waypoints.size() ? r.waypoints[(size_t)i] : St->picks.bots[(size_t)St->bot_sel];
        f.marks.push_back({up(p, 0.3), kPickedMark, 15.0f});
    }
}

std::wstring bots_where(const botnodes::V3 &p) {
    wchar_t b[96];
    swprintf(b, 96, L"(%.1f, %.1f, %.1f)", p[0], p[1], p[2]);
    return b;
}

void bots_text() {
    if (!St->bots_loaded) {
        SetWindowTextW(item(IDC_BOTS_TEXT), W(St->bot_why).c_str());
        return;
    }
    const auto &r = St->bot_res;
    if (!r.ok) {
        SetWindowTextW(item(IDC_BOTS_TEXT), (L"The bots' waypoints couldn't be worked out: " + W(r.why) + L".").c_str());
        return;
    }
    int mine = 0;
    for (size_t k = 0; k < St->bot_mine_at.size(); k++) mine += St->bot_mine_at[k] >= 0 && !St->bot_mine_merged[k];
    int good = 0, round = 0, stuck = 0;
    for (auto &l : bot_links()) (l.how == 0 ? good : l.how == 1 ? round : stuck)++;
    auto pct = [&](long v) { return std::to_wstring((int)std::lround(100.0 * v / std::max(1, r.given * (r.given - 1)))); };
    const int total = (int)r.waypoints.size();
    std::wstring t = L"Waypoints: " + std::to_wstring(total) + L" of the bots' 192: " + std::to_wstring(r.given - mine) +
                     L" from the map's spawns, item spots and flags (blue), " + std::to_wstring(mine) + L" of yours (green)";
    if (!r.added.empty()) t += L", " + std::to_wstring(r.added.size()) + L" a conversion adds (orange)";
    t += L".";
    if (r.merged) t += L" " + std::to_wstring(r.merged) + L" of the map's points are within 1 unit of another and make one with it.";
    if (!r.left_out.empty()) {
        std::vector<std::string> kinds = St->bot_kinds;   // (as the points were planned: yours in)
        size_t at = std::min(St->bot_insert, kinds.size());
        kinds.insert(kinds.begin() + (std::ptrdiff_t)at, St->picks.bots.size(), "waypoint of yours");
        std::map<std::string, int> by;
        for (int i : r.left_out) by[i >= 0 && (size_t)i < kinds.size() ? kinds[(size_t)i] : std::string("point")]++;
        std::wstring list;
        for (auto &[k, c] : by) {
            std::string name = k;
            if (c != 1)
                name = k == "item spawn point that spawns nothing" ? "item spawn points that spawn nothing"
                       : k == "waypoint of yours"                  ? "waypoints of yours"
                                                                   : k + "s";
            list += (list.empty() ? L"" : L", ") + std::to_wstring(c) + L" " + W(name);
        }
        t += L" Past the 192, the game leaves out " + std::to_wstring(r.left_out.size()) + L" (" + list + L"): bots never go to them.";
    }
    t += L"\r\n\r\nLinks: " + std::to_wstring(good) + L" the bots walk (green), " + std::to_wstring(round) +
         L" their search can't finish one way but they go round (amber), " + std::to_wstring(stuck) + L" with nothing round (red)";
    {
        std::set<std::pair<int, int>> joined, red;   // (red links: whether a walk joins their ends)
        for (auto &h : r.hops)
            if (!h.ok && !h.around) {
                auto k = std::make_pair(std::min(h.a, h.b), std::max(h.a, h.b));
                red.insert(k);
                if (h.joined) joined.insert(k);
            }
        size_t none = red.size() - joined.size();
        if (none) t += L"; " + std::to_wstring(none) + L" of those no walk joins (a jump, ladder or lift: no waypoint helps)";
    }
    t += L". The working links join " + pct(r.reach_after) + L"% of the ways between the map's own waypoints";
    if (!r.added.empty()) t += L" (" + pct(r.reach_before) + L"% without the orange ones)";
    t += L".";
    SetWindowTextW(item(IDC_BOTS_TEXT), t.c_str());
}

void bots_fill_list() {
    HWND l = item(IDC_BOTS_LIST);
    SendMessageW(l, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < St->picks.bots.size(); i++) {
        std::wstring s = std::to_wstring(i + 1) + L"   " + bots_where(St->picks.bots[i]);
        if (i < St->bot_mine_at.size() && St->bot_mine_at[i] < 0) s += L"   (no waypoint: no ground under it, or past 192)";
        else if (i < St->bot_mine_merged.size() && St->bot_mine_merged[i]) s += L"   (within 1 unit of another: one waypoint with it)";
        SendMessageW(l, LB_ADDSTRING, 0, (LPARAM)s.c_str());
    }
    if (St->bot_sel >= 0) SendMessageW(l, LB_SETCURSEL, (WPARAM)St->bot_sel, 0);
    EnableWindow(item(IDC_BOTS_DEL), St->bot_sel >= 0);
    EnableWindow(item(IDC_BOTS_CLEAR), !St->picks.bots.empty());
}

// The bots' waypoints and links with yours, as a conversion makes them.
void bots_plan() {
    if (!St->bots_loaded) return;
    std::vector<botnodes::V3> pts = St->bot_points;
    size_t at = std::min(St->bot_insert, pts.size());
    pts.insert(pts.begin() + (std::ptrdiff_t)at, St->picks.bots.begin(), St->picks.bots.end());
    botnodes::Options o;
    if (!St->picks.bots_auto) o.max_add = 0;
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    St->bot_res = botnodes::plan(St->bot_bsp, pts, St->bot_tele, o);
    SetCursor(old);
    // (each of yours: the waypoint it makes, or the one it's within 1 unit of; and whether that one is the map's)
    St->bot_mine_at.assign(St->picks.bots.size(), -1);
    St->bot_mine_merged.assign(St->picks.bots.size(), 0);
    const auto &r = St->bot_res;
    std::vector<int> first(r.waypoints.size(), -1);   // (each waypoint: the first point that made it)
    for (size_t i = 0; i < r.point_waypoint.size(); i++) {
        int k = r.point_waypoint[i];
        if (k >= 0 && (size_t)k < first.size() && first[(size_t)k] < 0) first[(size_t)k] = (int)i;
    }
    for (size_t k = 0; k < St->picks.bots.size(); k++) {
        size_t i = at + k;
        if (i >= r.point_waypoint.size()) continue;
        int wp = r.point_waypoint[i];
        St->bot_mine_at[k] = wp;
        if (wp >= 0 && first[(size_t)wp] != (int)i) St->bot_mine_merged[k] = 1;
    }
    bots_text();
    bots_fill_list();
    render(false);
}

void bots_loaded_now() {
    St->bots_loaded = false;
    St->bot_points.clear();
    St->bot_kinds.clear();
    fs::path t = St->file, b = St->file;
    t += ".bots";
    b += ".bots.scenario_structure_bsp";
    std::ifstream f(t, std::ios::binary);
    std::string l;
    bool head = false;
    while (std::getline(f, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (l == "HMP bots 1") { head = true; continue; }
        size_t tab = l.find('\t');
        if (!head || tab == std::string::npos) continue;
        std::string k = l.substr(0, tab), v = l.substr(tab + 1);
        if (k == "teleporters") St->bot_tele = atoi(v.c_str());
        else if (k == "insert") St->bot_insert = (size_t)std::max(0, atoi(v.c_str()));
        else if (k == "p") {
            botnodes::V3 p{};
            char *e = nullptr;
            const char *c = v.c_str();
            for (int i = 0; i < 3; i++) { p[(size_t)i] = strtod(c, &e); c = *e ? e + 1 : e; }
            St->bot_points.push_back(p);
            St->bot_kinds.push_back(c);
        }
    }
    std::error_code ec;
    if (!head || !fs::exists(b, ec)) {
        St->bot_why = "This measurement has no bot waypoints: the map isn't a multiplayer map, or it was measured before the "
                      "app could show them. Press \"Measure again\".";
        St->bot_res = botnodes::Result();
        bots_text();
        bots_fill_list();
        render(false);
        return;
    }
    St->bot_bsp = b;
    St->bots_loaded = true;
    bots_plan();
}

void set_bots_mode(bool on) {
    if (on && !St->data) return;
    if (on && St->sc_mode) set_sc_mode(false);
    St->bots_mode = on;
    ShowWindow(St->list, on ? SW_HIDE : SW_SHOW);
    for (int id = IDC_BOTS_TEXT; id <= IDC_BOTS_HINT; id++) ShowWindow(item(id), on ? SW_SHOW : SW_HIDE);
    SetWindowTextW(item(IDC_BOTS), on ? L"Back to the reflections" : L"Bot waypoints...");
    if (on) {
        SetWindowTextW(item(IDC_STATUS), L"Bot waypoints: click the level to put one there; click one of yours (green) to pick it.");
        if (!St->bots_loaded) bots_loaded_now();
        else bots_plan();
    } else {
        SetWindowTextW(item(IDC_STATUS), g_status_text.c_str());
        fog_cut_status();
        render(false);
    }
}

// A click in the picture: one of yours within 12 pixels of it picked, else a new one on the surface there.
void bots_click(int x, int y) {
    RECT rc;
    GetClientRect(St->view, &rc);
    int best = -1;
    double best_d = 12.0 * St->dpi / 96;
    for (size_t k = 0; k < St->picks.bots.size(); k++) {
        int i = k < St->bot_mine_at.size() ? St->bot_mine_at[k] : -1;
        botnodes::V3 p = i >= 0 && (size_t)i < St->bot_res.waypoints.size() ? St->bot_res.waypoints[(size_t)i] : St->picks.bots[k];
        p[2] += 0.3;
        double sx, sy;
        if (!project(p, rc.right, rc.bottom, sx, sy)) continue;
        double d = std::hypot(sx - x, sy - y);
        if (d < best_d) { best_d = d; best = (int)k; }
    }
    if (best >= 0) {
        St->bot_sel = best;
        bots_fill_list();
        SetWindowTextW(item(IDC_STATUS), (L"Picked your waypoint " + std::to_wstring(best + 1) + L" " +
                                          bots_where(St->picks.bots[(size_t)best]) + L": Delete removes it.").c_str());
        render(false);
        return;
    }
    std::array<double, 3> at{};
    int t = lshade::pick(St->data->p, St->cam, rc.right, rc.bottom, x, y, &at, nullptr);
    if (t < 0) {
        SetWindowTextW(item(IDC_STATUS), L"Nothing of the level there to put a waypoint on.");
        return;
    }
    St->picks.bots.push_back({at[0], at[1], at[2] + 0.25});   // (a little above the floor: the bots drop it down)
    St->bot_sel = (int)St->picks.bots.size() - 1;
    bots_plan();
    int i = St->bot_mine_at.back();
    std::wstring s = L"Added waypoint " + std::to_wstring(St->picks.bots.size()) + L" " + bots_where(St->picks.bots.back());
    if (i < 0) s += L": no waypoint comes of it (no ground under it, or the map is past 192).";
    else if (St->bot_mine_merged.back()) s += L": it's within 1 unit of a waypoint already there, so the bots make them one.";
    else if ((size_t)i < St->bot_res.waypoints.size() && std::fabs(St->bot_res.waypoints[(size_t)i][2] - at[2]) > 0.6)
        s += L": the bots drop it onto the floor below.";
    else s += L".";
    SetWindowTextW(item(IDC_STATUS), s.c_str());
}

void bots_delete() {
    if (St->bot_sel < 0 || (size_t)St->bot_sel >= St->picks.bots.size()) return;
    St->picks.bots.erase(St->picks.bots.begin() + St->bot_sel);
    St->bot_sel = -1;
    bots_plan();
    SetWindowTextW(item(IDC_STATUS), L"Removed it.");
}

// "Next problem": the picture looks at the next red link, from above and behind it.
void bots_next() {
    std::vector<BotLink> stuck;
    for (auto &l : bot_links())
        if (l.how == 2) stuck.push_back(l);
    if (stuck.empty()) {
        SetWindowTextW(item(IDC_STATUS), L"No red links: every waypoint the bots can't walk to directly, they go round to.");
        return;
    }
    St->bot_next = (St->bot_next + 1) % stuck.size();
    const auto &l = stuck[St->bot_next];
    const auto &r = St->bot_res;
    const auto &a = r.waypoints[(size_t)l.a], &b = r.waypoints[(size_t)l.b];
    double mid[3] = {(a[0] + b[0]) / 2, (a[1] + b[1]) / 2, (a[2] + b[2]) / 2};
    double span = std::max(4.0, std::hypot(a[0] - b[0], a[1] - b[1]) * 0.9 + 3);
    double yaw = St->cam.yaw;
    St->cam.eye = {mid[0] - std::cos(yaw) * span, mid[1] - std::sin(yaw) * span, mid[2] + span * 0.6};
    St->cam.pitch = -std::atan2(span * 0.6, span);
    render(false);
    std::wstring s = L"Red link " + std::to_wstring(St->bot_next + 1) + L" of " + std::to_wstring(stuck.size()) + L": " +
                     bots_where(a) + L" to " + bots_where(b) + L". ";
    bool joined = false;
    for (auto &h : r.hops)
        if ((h.a == l.a && h.b == l.b) || (h.a == l.b && h.b == l.a))
            if (!h.ok && !h.around) joined = joined || h.joined;
    s += joined ? L"The floor joins them: waypoints along the way the bots walk (a few units apart) can let them through."
                : L"No walk joins them (a jump, a ladder, a lift, or stairs the AI's floor doesn't join): no waypoint helps.";
    SetWindowTextW(item(IDC_STATUS), s.c_str());
}

// The point in the middle of the picture (what a shot looks at), if there's a surface there.
bool middle_point(std::array<double, 3> &at) {
    RECT rc;
    GetClientRect(St->view, &rc);
    int object = -1;
    int t = lshade::pick(St->data->p, St->cam, rc.right, rc.bottom, rc.right / 2.0, rc.bottom / 2.0, &at,
                         St->objects ? &object : nullptr);
    return t >= 0 || object >= 0;
}

void go_to_view() {
    if (!St->data || St->views.empty()) return;
    int v = St->views[St->view_at % St->views.size()];
    const auto &sv = St->data->p.views[(size_t)v];
    St->cam.eye = sv.eye;
    St->cam.yaw = sv.yaw;
    St->cam.pitch = -0.08;
    render(false);
}

// The views the arrows go through: the ones that see most, where the selected shader's reflection shows.
void pick_views() {
    St->views.clear();
    St->view_at = 0;
    if (!St->data) return;
    const auto &a = St->data->a;
    size_t S = a.shaders.size();
    std::vector<std::pair<double, int>> v;
    for (int i = 0; i < a.rendered; i++) {
        // (the views that see most of the level; with a shader selected, of those, where its reflection shows: not
        // the ones with a wall of it filling the screen)
        double sc = 0;
        for (size_t s = 0; s < S; s++) sc += a.drawn[(size_t)i * S + s];
        if (St->selected >= 0) sc *= a.shows[(size_t)i * S + (size_t)St->selected];
        if (sc > 0 || St->selected < 0) v.push_back({sc, i});
    }
    std::sort(v.rbegin(), v.rend());
    for (size_t k = 0; k < v.size() && k < 24; k++) St->views.push_back(v[k].second);
    if (St->views.empty())
        for (int i = 0; i < std::min(a.views, 24); i++) St->views.push_back(i);
}

void update_cost() {
    if (!St->data) return;
    const auto &a = St->data->a;
    std::vector<char> none(a.shaders.size(), 0);
    lshade::Cost before = lshade::cost(a, none), after = lshade::cost(a, dropped_all()), stock = lshade::stock_cost();
    int reflecting = 0, going = 0;
    for (size_t s = 0; s < a.shaders.size(); s++) {
        if (a.shaders[s].reflection_passes > 0) reflecting++;
        if (dropped((int)s)) going++;
    }
    auto ms = [](double passes) {
        wchar_t b[16];
        swprintf(b, 16, L"%.1f", passes * lshade::kXboxCpuMsPer1000Passes / 1000);
        return std::wstring(b);
    };
    std::wstring t = L"Level triangle passes, typical / busy view: " + thousands((long long)before.typical) + L" / " +
                     thousands((long long)before.busy) + L" as made";
    if (going) {
        t += L" \u2192 " + thousands((long long)after.typical) + L" / " + thousands((long long)after.busy) + L" with " +
             std::to_wstring(going) + L" of " + std::to_wstring(reflecting) + L" reflections dropped (about " +
             ms(before.typical - after.typical) + L" / " + ms(before.busy - after.busy) + L" ms less processor time a frame)";
    } else if (reflecting) {
        std::vector<char> all(a.shaders.size(), 0);
        for (size_t s = 0; s < a.shaders.size(); s++) all[s] = a.shaders[s].reflection_passes > 0;
        lshade::Cost bare = lshade::cost(a, all);
        t += L" (its reflections: about " + ms(before.typical - bare.typical) + L" / " + ms(before.busy - bare.busy) +
             L" ms of the Xbox's processor a frame)";
    }
    t += L". Sidewinder: " + thousands((long long)stock.typical) + L" / " + thousands((long long)stock.busy) + L".";
    SetWindowTextW(item(IDC_COST), t.c_str());
}

void set_row(int row) {
    int s = St->row_shader[(size_t)row];
    const auto &sh = St->data->a.shaders[(size_t)s];
    St->filling = true;
    ListView_SetCheckState(St->list, row, dropped(s));
    St->filling = false;
    std::wstring sug = sh.suggested ? L"Drop" : L"Keep";
    ListView_SetItemText(St->list, row, 5, (LPWSTR)sug.c_str());
}

// The fog and tiny scenery choices (Picks::fog, fog_scale, fog_distance, fog_start, tiny_px): the fog's described with
// the level's own numbers; the last fog choice is two distances typed into the boxes beside it, where it starts and
// where it's fully opaque.
const double kTiny[] = {0, 4, 8, 12, 16};
const int kTinyCount = 5;
// The effects choices, in the order listed, and the Picks::effects each one is.
const wchar_t *const kEffectsItems[3] = {L"No busier than Bungie's (the default)", L"Lighter (looping ones: a quarter)", L"As made"};
const int kEffectsPick[3] = {0, 2, 1};
const double kFogScale[] = {1.0, 1.0, 1.0, 0.75, 0.5, 1.0};   // (by item: automatic, as made, opaque at 1, 3/4, 1/2 of its distance; typed)
const int kFogOther = 5;
int fog_item() {
    if (St->picks.fog == 3) return kFogOther;
    return St->picks.fog == 0 ? 0 : St->picks.fog == 1 ? 1 : St->picks.fog_scale >= 0.875 ? 2 : St->picks.fog_scale >= 0.625 ? 3 : 4;
}
// The level's outdoor fog as the map has it (the first sky's; else its indoor fog).
lshade::Fog level_fog() {
    lshade::Fog f;
    if (St->data && !St->data->p.sky_fog.empty()) f = St->data->p.sky_fog[0];
    if (St->data && !f.any()) f = St->data->p.indoor_fog;
    return f;
}
// The typed distances' boxes: show the distances in use (the outdoor fog's as the conversion leaves it), and take typing
// only with "Other" chosen.
void show_fog_distance() {
    lshade::Fog f = level_fog();
    double start = f.start, d = f.opaque;
    if (St->picks.fog == 3) {
        d = St->picks.fog_distance > 0 ? St->picks.fog_distance : f.opaque;
        if (St->picks.fog_start >= 0) start = St->picks.fog_start;
    } else if (St->picks.fog == 2) {
        d = std::max(f.opaque * std::clamp(St->picks.fog_scale, 0.1, 1.0), f.start + 1.0);
    }
    wchar_t b[32] = L"", a[32] = L"";
    if (d > 0) swprintf(b, 32, L"%.0f", d);
    if (f.any()) swprintf(a, 32, L"%.0f", start);
    St->filling = true;
    SetWindowTextW(item(IDC_FOG_DIST), b);
    SetWindowTextW(item(IDC_FOG_START), a);
    St->filling = false;
    bool on = St->data && f.any() && St->picks.fog == 3;
    for (int id : {IDC_FOG_START, IDC_FOG_TO, IDC_FOG_DIST, IDC_FOG_UNITS}) EnableWindow(item(id), on);
}
// When the fog in use is fully opaque, what its end cuts through (lshade::fog_cuts: a treeline or mountains round the
// level's edge, cut off flat where they cross it), on the status line; else the level's line back.
void fog_cut_status() {
    if (!St->data || St->sc_mode) return;
    lshade::Fog f = level_fog();
    double d = f.opaque;
    bool opaque = f.any() && (St->picks.fog == 2 || St->picks.fog == 3 || (St->picks.fog == 0 && f.density >= 0.9f) || f.density >= 1);
    if (St->picks.fog == 3) d = St->picks.fog_distance > 0 ? St->picks.fog_distance : f.opaque;
    else if (St->picks.fog == 2) d = std::max(f.opaque * std::clamp(St->picks.fog_scale, 0.1, 1.0), f.start + 1.0);
    std::vector<lshade::FogCut> cuts = opaque && d > 0 ? lshade::fog_cuts(St->data->p, d) : std::vector<lshade::FogCut>();
    if (cuts.empty()) {
        SetWindowTextW(item(IDC_STATUS), g_status_text.c_str());
        return;
    }
    double lo = 1e30, hi = 0;
    std::wstring names;
    for (size_t i = 0; i < cuts.size() && i < 2; i++) names += (i ? L", " : L"") + fs::path(W(cuts[i].tag)).stem().wstring();
    for (auto &c : cuts) { lo = std::min(lo, c.nearest); hi = std::max(hi, c.farthest); }
    wchar_t b[400];
    swprintf(b, 400, L"The fog's end (%.0f units) cuts through %ls (%.0f to %.0f units from players): the Xbox draws nothing "
             L"past it, so it's chopped off flat where it crosses. Under %.0f hides it; past %.0f keeps it whole.",
             d, names.c_str(), lo, hi, std::max(1.0, std::floor(lo * 0.77)), std::ceil(hi * 1.03));
    SetWindowTextW(item(IDC_STATUS), b);
}
void fill_fog() {
    HWND c = item(IDC_FOG);
    SendMessageW(c, CB_RESETCONTENT, 0, 0);
    lshade::Fog f = level_fog();
    if (!St->data || !f.any()) {
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"The level has no fog");
        SendMessageW(c, CB_SETCURSEL, 0, 0);
        EnableWindow(c, FALSE);
        show_fog_distance();
        return;
    }
    EnableWindow(c, TRUE);
    auto units = [](double u) {
        wchar_t b[48];
        swprintf(b, 48, L"%.0f units (%.0f m)", u, u * 3.048);
        return std::wstring(b);
    };
    wchar_t pc[16];
    swprintf(pc, 16, L"%.0f%%", f.density * 100);
    std::wstring a = f.density >= 1 ? L"Automatic: it's fully opaque at " + units(f.opaque)
                     : f.density >= 0.9f ? L"Automatic: made fully opaque at " + units(f.opaque) + L" (it's " + pc + L")"
                                         : L"Automatic: left as it is (" + std::wstring(pc) + L" at " + units(f.opaque) + L")";
    std::wstring items[6] = {a, L"As the map has it (" + std::wstring(pc) + L" at " + units(f.opaque) + L")",
                             L"Fully opaque at " + units(f.opaque), L"Fully opaque nearer, at " + units(f.opaque * 0.75),
                             L"Fully opaque nearer still, at " + units(f.opaque * 0.5),
                             L"Other: type the distances \u2192"};
    for (auto &t : items) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)t.c_str());
    SendMessageW(c, CB_SETDROPPEDWIDTH, S(380), 0);
    SendMessageW(c, CB_SETCURSEL, fog_item(), 0);
    show_fog_distance();
    fog_cut_status();
}

// The weather choices (Picks::weather), with the level's own drops.
void fill_weather() {
    HWND c = item(IDC_WEATHER);
    SendMessageW(c, CB_RESETCONTENT, 0, 0);
    const lshade::Weather w = St->data ? St->data->p.weather : lshade::Weather();
    if (!St->data || !w.systems || w.made < 1) {
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)(St->data ? L"The level has no rain or snow" : L""));
        SendMessageW(c, CB_SETCURSEL, 0, 0);
        EnableWindow(c, FALSE);
        return;
    }
    EnableWindow(c, TRUE);
    auto n = [](float v) { return thousands((long long)std::lround(v)); };
    wchar_t e[24], le[24];
    swprintf(e, 24, L"%g", std::round(w.end * 10) / 10);
    swprintf(le, 24, L"%g", std::round(w.lighter_end * 10) / 10);
    std::wstring items[4] = {
        L"As made: about " + n(w.made) + L" drops around you, ending " + e + L" units away" +
            (w.made >= 511.5f ? L" (the engine's most)" : L""),
        w.lighter < w.made - 0.5f ? L"Lighter: just as dense up close, ending " + std::wstring(le) + L" units away: about " + n(w.lighter) + L" drops"
                                  : L"Lighter: no change (it already ends within " + std::wstring(le) + L" units)",
        L"Half the drops: about " + n(w.half),
        L"Off: no rain or snow (the wind stays)"};
    for (auto &t : items) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)t.c_str());
    SendMessageW(c, CB_SETDROPPEDWIDTH, S(540), 0);
    SendMessageW(c, CB_SETCURSEL, std::clamp(St->picks.weather, 0, 3), 0);
}

// The riders and detailed props choices (Picks::riders, props), with what the map has.
void fill_content() {
    const lshade::Content c = St->data ? St->data->p.content : lshade::Content();
    auto fill = [](int id, bool any, const std::wstring &none, const std::wstring &keep, const std::wstring &change, int pick) {
        HWND h = item(id);
        SendMessageW(h, CB_RESETCONTENT, 0, 0);
        if (!any) {
            SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)(St->data ? none.c_str() : L""));
            SendMessageW(h, CB_SETCURSEL, 0, 0);
            EnableWindow(h, FALSE);
            return;
        }
        EnableWindow(h, TRUE);
        SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)keep.c_str());
        SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)change.c_str());
        SendMessageW(h, CB_SETDROPPEDWIDTH, S(460), 0);
        SendMessageW(h, CB_SETCURSEL, pick ? 1 : 0, 0);
    };
    fill(IDC_RIDERS, c.riders > 0, L"No units ride other objects", L"Keep their AI (as made)",
         L"No AI for the " + std::to_wstring(c.riders) + L" riding other objects", St->picks.riders);
    std::string names(c.prop_names);
    fill(IDC_PROPS, c.detailed_props > 0, L"No small props with very detailed collision", L"Keep their collision (as made)",
         L"No collision: " + W(names), St->picks.props);
    // (the lights: three choices)
    HWND h = item(IDC_LIGHTS);
    SendMessageW(h, CB_RESETCONTENT, 0, 0);
    if (c.object_lights <= 0) {
        SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)(St->data ? L"No dynamic lights on the map's objects" : L""));
        SendMessageW(h, CB_SETCURSEL, 0, 0);
        EnableWindow(h, FALSE);
        return;
    }
    EnableWindow(h, TRUE);
    std::wstring ln = W(std::string(c.light_names));
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)(L"Keep them (as made): " + ln).c_str());
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"No specular: the light stays, without its shine (half the cost or more)");
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"Off: no light (a lens flare stays)");
    SendMessageW(h, CB_SETDROPPEDWIDTH, S(520), 0);
    SendMessageW(h, CB_SETCURSEL, std::clamp(St->picks.lights, 0, 2), 0);
}

void fill_list() {
    ListView_DeleteAllItems(St->list);
    St->row_shader.clear();
    if (!St->data) return;
    const auto &a = St->data->a;
    std::vector<int> order;
    for (size_t s = 0; s < a.shaders.size(); s++)
        if (a.shaders[s].reflection_passes > 0) order.push_back((int)s);
    std::sort(order.begin(), order.end(), [&](int x, int y) {
        return a.shaders[(size_t)x].drawn * a.shaders[(size_t)x].reflection_passes > a.shaders[(size_t)y].drawn * a.shaders[(size_t)y].reflection_passes;
    });
    St->filling = true;
    for (size_t r = 0; r < order.size(); r++) {
        const auto &sh = a.shaders[(size_t)order[r]];
        std::wstring name = fs::path(W(sh.tag)).stem().wstring();
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = (int)r;
        it.pszText = (LPWSTR)name.c_str();
        ListView_InsertItem(St->list, &it);
        std::wstring cells[5] = {thousands((long long)std::lround(sh.drawn * sh.reflection_passes)), pct(sh.shows), pct(sh.coverage),
                                 thousands(sh.triangles), L""};
        for (int c = 0; c < 4; c++) ListView_SetItemText(St->list, (int)r, c + 1, (LPWSTR)cells[c].c_str());
        St->row_shader.push_back(order[r]);
    }
    St->filling = false;
    for (size_t r = 0; r < order.size(); r++) set_row((int)r);
    std::wstring s = (order.size() == a.shaders.size() ? L"All " + std::to_wstring(order.size()) + L" of the level's shaders reflect"
                                                       : std::to_wstring(order.size()) + L" of the level's " + std::to_wstring(a.shaders.size()) +
                                                             L" shaders reflect") +
                     L". Ticked: its reflection goes when you convert the map. Click one to see where it shows.";
    const auto &pv = St->data->p;
    s += L" The level has " + std::to_wstring(pv.rooms) + (pv.rooms == 1 ? L" room (cluster)" : L" rooms (clusters)") + L" and " +
         std::to_wstring(pv.portals.size()) + (pv.portals.size() == 1 ? L" portal." : L" portals.");
    g_status_text = s;
    if (!St->sc_mode) SetWindowTextW(item(IDC_STATUS), s.c_str());
    update_cost();
    fill_fog();
    fill_weather();
    fill_content();
    pick_views();
    go_to_view();
}

void set_mode(int mode) {
    St->picks.mode = mode;
    CheckRadioButton(St->wnd, IDC_MODE0, IDC_MODE2, IDC_MODE0 + mode);
    if (St->data)
        for (size_t r = 0; r < St->row_shader.size(); r++) set_row((int)r);
    update_cost();
    render(false);
}

void start_measure() {
    if (g_measuring.exchange(true)) {
        // (closed and opened again while this map was measured: its result comes here)
        std::lock_guard<std::mutex> g(g_lock);
        bool same = g_measuring_key == St->key;
        SetWindowTextW(item(IDC_STATUS), same ? L"Measuring the map's level..." : L"Another map is being measured; press \"Measure again\" in a moment.");
        if (same) EnableWindow(item(IDC_REMEASURE), FALSE);
        return;
    }
    {
        std::lock_guard<std::mutex> g(g_lock);
        g_measuring_key = St->key;
    }
    SetWindowTextW(item(IDC_STATUS), L"Measuring the map's level (it's prepared as for a conversion, without building it)...");
    EnableWindow(item(IDC_REMEASURE), FALSE);
    Measure fn = St->measure;
    std::string key = St->key;
    std::thread([fn, key] {
        fs::path file;
        std::string err;
        bool ok = fn(file, err, [key](const std::string &l) { post(WM_LS_LOG, 0, new Note{key, l, {}}); });
        if (ok) {
            std::lock_guard<std::mutex> g(g_lock);
            keep_locked(key, file);
            file = g_cache[key];
        }
        {
            std::lock_guard<std::mutex> g(g_lock);
            g_measuring_key.clear();
        }
        g_measuring = false;
        post(WM_LS_DONE, ok, new Note{key, err, file});
    }).detach();
}

void bots_loaded_now();
bool load_file(const fs::path &f) {
    auto d = std::make_shared<Data>();
    if (!lshade::load(d->a, d->p, f)) return false;
    St->data = d;
    St->file = f;
    St->bots_loaded = false;
    if (St->bots_mode) bots_loaded_now();
    if (St->gl) St->gl->set_scene(&d->p);
    // (picks made against an older measurement keep only the shaders still there)
    std::set<std::string> tags;
    for (auto &s : d->a.shaders) tags.insert(s.tag);
    for (auto it = St->picks.drops.begin(); it != St->picks.drops.end();) it = tags.count(*it) ? std::next(it) : St->picks.drops.erase(it);
    fill_list();
    return true;
}

// The fly camera. Held keys move it every frame, by how long the frame took: W A S D forward, back and sideways (where
// you're looking), Q / E down and up, the arrows turn; Shift faster, Ctrl slower.
const UINT_PTR kTick = 1;
bool key(int vk) { return GetKeyState(vk) < 0; }
bool moving_keys() {
    for (int vk : {(int)'W', (int)'A', (int)'S', (int)'D', (int)'Q', (int)'E', VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT})
        if (key(vk)) return true;
    return false;
}
void start_ticking() {
    if (St->ticking) return;
    St->ticking = true;
    QueryPerformanceCounter(&St->last_tick);
    SetTimer(St->view, kTick, 30, nullptr);   // (a backstop: the window's loop draws a frame each time round)
}
void tick() {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    double dt = std::clamp((double)(now.QuadPart - St->last_tick.QuadPart) / (double)freq.QuadPart, 0.0, 0.1);
    St->last_tick = now;
    bool keys = GetFocus() == St->view && moving_keys();
    if (St->playing && keys) sc_stop();
    if (St->playing && St->data) sc_tick();
    if (keys && St->data) {
        double speed = 3.0 * (key(VK_SHIFT) ? 3.0 : 1.0) * (key(VK_CONTROL) ? 0.3 : 1.0) * dt;   // (world units: 3 m)
        lshade::Camera &c = St->cam;
        if (key(VK_LEFT)) c.yaw += 1.6 * dt;
        if (key(VK_RIGHT)) c.yaw -= 1.6 * dt;
        double fw[3] = {std::cos(c.yaw) * std::cos(c.pitch), std::sin(c.yaw) * std::cos(c.pitch), std::sin(c.pitch)};
        double rt[2] = {std::sin(c.yaw), -std::cos(c.yaw)};
        double f = (key('W') || key(VK_UP) ? 1 : 0) - (key('S') || key(VK_DOWN) ? 1 : 0);
        double r = (key('D') ? 1 : 0) - (key('A') ? 1 : 0), u = (key('E') ? 1 : 0) - (key('Q') ? 1 : 0);
        for (int k = 0; k < 3; k++) c.eye[(size_t)k] += fw[k] * f * speed;
        c.eye[0] += rt[0] * r * speed;
        c.eye[1] += rt[1] * r * speed;
        c.eye[2] += u * speed;
        St->dirty = true;
    }
    if (St->dirty) {
        bool coarse = !St->gl && (keys || St->dragging);   // (drawn here: smaller while it moves)
        render(coarse);
        St->coarse = coarse;
        St->dirty = false;
        return;
    }
    if (St->coarse) {   // (stopped: the full picture)
        render(false);
        St->coarse = false;
    }
    if (!keys && !St->dragging && !St->playing) {
        KillTimer(St->view, kTick);
        St->ticking = false;
    }
}

// Double-clicking the picture: the shader of the surface there, selected in the list (the view stays), or named in the
// status line when the list doesn't have it (it doesn't reflect, or it's see-through).
// (in the showcase: a key of the selected shot drawn within 12 pixels of it picked instead)
bool pick_key(int x, int y, int w, int h) {
    showcase::Shot *shot = sc_shot();
    if (!St->sc_mode || St->playing || !shot || w <= 0 || h <= 0) return false;
    const double tx = std::tan(35.0 * 3.14159265358979 / 180), ty = tx * h / w;
    const auto &c = St->cam;
    double fwd[3] = {std::cos(c.yaw) * std::cos(c.pitch), std::sin(c.yaw) * std::cos(c.pitch), std::sin(c.pitch)};
    double right[3] = {fwd[1], -fwd[0], 0};
    double rl = std::sqrt(right[0] * right[0] + right[1] * right[1]);
    if (rl < 1e-9) return false;
    right[0] /= rl; right[1] /= rl;
    double up[3] = {right[1] * fwd[2] - right[2] * fwd[1], right[2] * fwd[0] - right[0] * fwd[2], right[0] * fwd[1] - right[1] * fwd[0]};
    int best = -1;
    double best_d = 12.0 * St->dpi / 96;
    const auto &pk = showcase::path_keys(*shot);
    for (size_t k = 0; k < pk.size(); k++) {
        double d[3] = {pk[k].p[0] - c.eye[0], pk[k].p[1] - c.eye[1], pk[k].p[2] - c.eye[2]};
        double z = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
        if (z < 0.05) continue;
        double sx = ((d[0] * right[0] + d[1] * right[1]) / z / tx * 0.5 + 0.5) * w;
        double sy = (0.5 - (d[0] * up[0] + d[1] * up[1] + d[2] * up[2]) / z / ty * 0.5) * h;
        double dist = std::hypot(sx - x, sy - y);
        if (dist < best_d) { best_d = dist; best = (int)k; }
    }
    if (best < 0) return false;
    St->sc_key = best;
    sc_key_show();
    std::wstring line = L"Picked key " + std::to_wstring(best + 1) + L" of shot " + std::to_wstring(St->sc_sel + 1) +
                        L": Go to it, or fly somewhere and Move it here.";
    SetWindowTextW(item(IDC_STATUS), line.c_str());
    render(false);
    return true;
}
void pick_at(int x, int y) {
    const lshade::Preview &p = St->data->p;
    RECT rc;
    GetClientRect(St->view, &rc);
    if (pick_key(x, y, rc.right, rc.bottom)) return;
    int object = -1, part = -1;
    int t = lshade::pick(p, St->cam, rc.right, rc.bottom, x, y, nullptr, St->objects ? &object : nullptr, &part);
    if (object >= 0) {   // (an object: named, with its shader, in the status line)
        const lshade::Object &o = p.objects[(size_t)object];
        const lshade::Model &m = p.models[(size_t)o.model];
        std::wstring what = L"Double-clicked: " + fs::path(W(o.tag)).stem().wstring() + L" (" + W(lshade::object_kind(o.kind)) + L")";
        if (part >= 0 && (size_t)part < m.parts.size()) {
            const lshade::ModelPart &mp = m.parts[(size_t)part];
            if (mp.shader >= 0 && (size_t)mp.shader < m.shaders.size())
                what += L", shader " + fs::path(W(m.shaders[(size_t)mp.shader])).filename().wstring();
            if (mp.flags & lshade::ModelPart::kCutout) what += L" (alpha-tested: cut out where its base map's alpha is under half)";
            else if (mp.flags & lshade::ModelPart::kSeeThrough) what += L" (see-through)";
        }
        what += L". " + W(o.tag);
        if (!o.lit) what += L". Nothing lit is below it: it has the level's default lighting.";
        SetWindowTextW(item(IDC_STATUS), what.c_str());
        return;
    }
    if (t < 0) {
        SetWindowTextW(item(IDC_STATUS), St->objects ? L"Double-clicked: nothing there (the sky)."
                                                     : L"Double-clicked: no surface of the level there (the sky, or an object: "
                                                       L"tick Objects to draw them).");
        return;
    }
    int m = p.triangles[(size_t)t].material;
    const lshade::Material &mt = p.materials[(size_t)m];
    std::string tag = (size_t)m < p.material_tags.size() ? p.material_tags[(size_t)m] : "";
    std::wstring name = tag.empty() ? L"(no shader)" : fs::path(W(tag)).stem().wstring();
    int row = -1;
    for (size_t r = 0; r < St->row_shader.size() && mt.shader >= 0; r++)
        if (St->row_shader[r] == mt.shader) row = (int)r;
    std::wstring what = L"Double-clicked: " + name;
    St->keep_camera = true;
    if (row >= 0) {
        ListView_SetItemState(St->list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(St->list, row, FALSE);
        what += L", selected in the list.";
    } else {
        ListView_SetItemState(St->list, -1, 0, LVIS_SELECTED);
        St->selected = mt.shader;
        pick_views();
        render(false);
        what += mt.see_through ? L" (see-through; the list has the shaders that reflect)."
                               : L" (it doesn't reflect, so it isn't in the list; tinted in the picture).";
    }
    St->keep_camera = false;
    if (mt.cutout >= 0) what += L" It's alpha-tested: cut out where its bump map's alpha is under half.";
    SetWindowTextW(item(IDC_STATUS), what.c_str());
}

LRESULT CALLBACK view_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        if (St && St->gl && St->data) {
            render(false);
            return 0;
        }
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        if (St && !St->px.empty()) {
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = St->pw;
            bi.bmiHeader.biHeight = -St->ph;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            SetStretchBltMode(dc, COLORONCOLOR);
            StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, St->pw, St->ph, St->px.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
        } else {
            FillRect(dc, &rc, (HBRUSH)GetStockObject(DKGRAY_BRUSH));
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER:
        if (St && wp == kTick) tick();
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        if (!St || !St->data) return 0;
        if (St->playing) sc_stop();
        if (St->tips) {   // (the how-to-fly tip: not again once you're flying)
            TOOLINFOW ti{};
            ti.cbSize = sizeof(ti);
            ti.hwnd = St->wnd;
            ti.uId = (UINT_PTR)h;
            SendMessageW(St->tips, TTM_DELTOOLW, 0, (LPARAM)&ti);
        }
        SetCapture(h);
        SetFocus(h);
        St->dragging = true;
        St->drag_from = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        St->click = msg == WM_LBUTTONDOWN;
        St->click_at = St->drag_from;
        start_ticking();
        return 0;
    case WM_LBUTTONDBLCLK:
        if (St && St->data && St->bots_mode) {   // (a second click: as the first)
            St->click = true;
            St->click_at = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            SetCapture(h);
            St->dragging = true;
            St->drag_from = St->click_at;
            return 0;
        }
        if (St && St->data) pick_at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_MOUSEMOVE:
        if (St && St->dragging) {
            // Turns the camera itself, so the keys move it the way you look now.
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (St->click && std::abs(pt.x - St->click_at.x) + std::abs(pt.y - St->click_at.y) <= 3) return 0;   // (still a click)
            St->click = false;
            St->cam.yaw -= (pt.x - St->drag_from.x) * 0.005;
            St->cam.pitch = std::clamp(St->cam.pitch - (pt.y - St->drag_from.y) * 0.005, -1.5, 1.5);
            St->drag_from = pt;
            St->dirty = true;
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_CAPTURECHANGED:
        if (St && St->dragging) {
            St->dragging = false;
            if (msg != WM_CAPTURECHANGED) ReleaseCapture();
            St->dirty = true;
            if (msg == WM_LBUTTONUP && St->click && St->bots_mode && St->data) {
                St->click = false;
                bots_click(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            }
            St->click = false;
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (St && St->data) {
            double step = GET_WHEEL_DELTA_WPARAM(wp) / 120.0 * 0.5 * (key(VK_SHIFT) ? 3.0 : 1.0);   // (half a world unit a notch)
            const lshade::Camera &c = St->cam;
            St->cam.eye[0] += std::cos(c.yaw) * std::cos(c.pitch) * step;
            St->cam.eye[1] += std::sin(c.yaw) * std::cos(c.pitch) * step;
            St->cam.eye[2] += std::sin(c.pitch) * step;
            St->dirty = true;
            start_ticking();
        }
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_KEYDOWN:
        switch (wp) {
        case 'W': case 'A': case 'S': case 'D': case 'Q': case 'E': case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
            if (St && St->data) start_ticking();
            return 0;
        case VK_DELETE: case VK_BACK:
            if (St && St->bots_mode) bots_delete();
            return 0;
        }
        break;
    case WM_CHAR:
        return 0;   // (the movement keys: no beep)
    }
    return DefWindowProcW(h, msg, wp, lp);
}

HWND add(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), St->wnd, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)St->font, TRUE);
    return c;
}

void create_controls() {
    add(L"STATIC", L"When converting:", 0, 12, 14, 104, 20, IDC_ABOUT);
    add(L"BUTTON", L"Keep every reflection (the default)", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 118, 12, 230, 22, IDC_MODE2);
    add(L"BUTTON", L"Drop the suggested ones (cost most, show least; flatter in places)", BS_AUTORADIOBUTTON, 352, 12, 400, 22,
        IDC_MODE0);
    add(L"BUTTON", L"Drop the ones I tick", BS_AUTORADIOBUTTON, 756, 12, 160, 22, IDC_MODE1);
    add(L"STATIC", L"", 0, 12, 42, 1156, 20, IDC_STATUS);
    St->list = add(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, 12, 68, 548, 646, IDC_LIST);
    ListView_SetExtendedListViewStyle(St->list, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER);
    struct Col { const wchar_t *name; int w; } cols[] = {{L"Shader (ticked: reflection goes)", 184}, {L"Extra passes", 84},
                                                        {L"Shows", 52}, {L"Covers", 54}, {L"Triangles", 64}, {L"Suggested", 74}};
    for (int i = 0; i < 6; i++) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.fmt = i ? LVCFMT_RIGHT : LVCFMT_LEFT;
        c.cx = S(cols[i].w);
        c.pszText = (LPWSTR)cols[i].name;
        ListView_InsertColumn(St->list, i, &c);
    }
    // The showcase flythrough: its shots in place of the list (hidden until "Showcase flythrough...").
    add(L"BUTTON", L"Showcase flythrough...", BS_PUSHBUTTON | WS_TABSTOP, 968, 10, 200, 26, IDC_SHOWCASE);
    St->sc_list = add(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, 12, 68, 548, 236, IDC_SC_LIST);
    ListView_SetExtendedListViewStyle(St->sc_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    {
        struct Col { const wchar_t *name; int w; } scols[] = {{L"Shot", 290}, {L"Seconds", 74}, {L"Keys", 56}, {L"Looks at", 100}};
        for (int i = 0; i < 4; i++) {
            LVCOLUMNW c{};
            c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
            c.fmt = i ? LVCFMT_RIGHT : LVCFMT_LEFT;
            c.cx = S(scols[i].w);
            c.pszText = (LPWSTR)scols[i].name;
            ListView_InsertColumn(St->sc_list, i, &c);
        }
    }
    add(L"STATIC", L"Name:", SS_NOTIFY, 12, 315, 40, 20, IDC_SC_NAME_LBL);
    add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 54, 312, 250, 22, IDC_SC_NAME, WS_EX_CLIENTEDGE);
    SendMessageW(item(IDC_SC_NAME), EM_SETLIMITTEXT, 31, 0);
    add(L"STATIC", L"Seconds:", SS_NOTIFY, 316, 315, 56, 20, IDC_SC_SECS_LBL);
    add(L"EDIT", L"", ES_RIGHT | ES_AUTOHSCROLL | WS_TABSTOP, 374, 312, 58, 22, IDC_SC_SECS, WS_EX_CLIENTEDGE);
    SendMessageW(item(IDC_SC_SECS), EM_SETLIMITTEXT, 6, 0);
    add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 440, 312, 120, 200, IDC_SC_PACE);
    for (const wchar_t *p : {L"Even speed", L"Slow in", L"Slow out", L"Slow in and out", L"Times as keyed"})
        SendMessageW(item(IDC_SC_PACE), CB_ADDSTRING, 0, (LPARAM)p);
    add(L"BUTTON", L"Keep looking at what's in the middle of the picture", BS_AUTOCHECKBOX | WS_TABSTOP, 12, 340, 420, 22, IDC_SC_LOOK);
    add(L"BUTTON", L"Play them all", BS_PUSHBUTTON | WS_TABSTOP, 12, 368, 116, 26, IDC_SC_PLAY);
    add(L"BUTTON", L"Play this shot", BS_PUSHBUTTON | WS_TABSTOP, 134, 368, 116, 26, IDC_SC_PLAYSHOT);
    add(L"BUTTON", L"Stop", BS_PUSHBUTTON | WS_TABSTOP, 256, 368, 80, 26, IDC_SC_STOP);
    add(L"BUTTON", L"Save to map folder", BS_PUSHBUTTON | WS_TABSTOP, 428, 368, 132, 26, IDC_SC_SAVE);
    add(L"BUTTON", L"New shot here", BS_PUSHBUTTON | WS_TABSTOP, 12, 400, 116, 26, IDC_SC_NEW);
    add(L"BUTTON", L"Delete shot", BS_PUSHBUTTON | WS_TABSTOP, 134, 400, 96, 26, IDC_SC_DELETE);
    add(L"BUTTON", L"Move up", BS_PUSHBUTTON | WS_TABSTOP, 236, 400, 80, 26, IDC_SC_UP);
    add(L"BUTTON", L"Move down", BS_PUSHBUTTON | WS_TABSTOP, 322, 400, 90, 26, IDC_SC_DOWN);
    add(L"BUTTON", L"Make again", BS_PUSHBUTTON | WS_TABSTOP, 444, 400, 116, 26, IDC_SC_REMAKE);
    add(L"STATIC", L"Keys:", SS_NOTIFY, 12, 444, 40, 20, IDC_SC_KEYS_LBL);
    add(L"BUTTON", L"\u2190", BS_PUSHBUTTON | WS_TABSTOP, 54, 440, 30, 26, IDC_SC_KPREV);
    add(L"STATIC", L"", SS_CENTER | SS_NOTIFY, 88, 444, 168, 20, IDC_SC_KEY);
    add(L"BUTTON", L"\u2192", BS_PUSHBUTTON | WS_TABSTOP, 258, 440, 30, 26, IDC_SC_KNEXT);
    add(L"BUTTON", L"Go to it", BS_PUSHBUTTON | WS_TABSTOP, 296, 440, 90, 26, IDC_SC_KGO);
    add(L"BUTTON", L"Add key here", BS_PUSHBUTTON | WS_TABSTOP, 54, 472, 116, 26, IDC_SC_ADD);
    add(L"BUTTON", L"Move it here", BS_PUSHBUTTON | WS_TABSTOP, 176, 472, 116, 26, IDC_SC_KMOVE);
    add(L"BUTTON", L"Remove it", BS_PUSHBUTTON | WS_TABSTOP, 298, 472, 90, 26, IDC_SC_UNDO);
    add(L"STATIC", L"Orbit:", SS_NOTIFY, 12, 516, 70, 20, IDC_SC_ORBIT_LBL);
    add(L"STATIC", L"radius", SS_NOTIFY, 84, 516, 40, 20, IDC_SC_ORAD_LBL);
    add(L"EDIT", L"", ES_RIGHT | ES_AUTOHSCROLL | WS_TABSTOP, 126, 513, 44, 22, IDC_SC_ORAD, WS_EX_CLIENTEDGE);
    add(L"STATIC", L"height", SS_NOTIFY, 178, 516, 42, 20, IDC_SC_OHGT_LBL);
    add(L"EDIT", L"", ES_RIGHT | ES_AUTOHSCROLL | WS_TABSTOP, 222, 513, 44, 22, IDC_SC_OHGT, WS_EX_CLIENTEDGE);
    add(L"STATIC", L"turn", SS_NOTIFY, 274, 516, 32, 20, IDC_SC_OAR_LBL);
    add(L"EDIT", L"", ES_RIGHT | ES_AUTOHSCROLL | WS_TABSTOP, 308, 513, 50, 22, IDC_SC_OAR, WS_EX_CLIENTEDGE);
    for (int id : {IDC_SC_ORAD, IDC_SC_OHGT, IDC_SC_OAR}) SendMessageW(item(id), EM_SETLIMITTEXT, 6, 0);
    add(L"BUTTON", L"New orbit here", BS_PUSHBUTTON | WS_TABSTOP, 444, 511, 116, 26, IDC_SC_ORBIT);
    add(L"STATIC",
        L"These shots play in order in Halo 1 Unlimited's showcase screensaver. New shot here starts one where the picture "
        L"is; fly on and click Add key here at each next spot. The camera goes along them at an even speed, over the shot's "
        L"Seconds (more seconds: slower); Slow in or out starts it from still or brings it to rest. The "
        L"arrows pick a key (or double-click a yellow one in the picture): Go to it, then fly somewhere and Move it here, or "
        L"Add key here to put a new one after it. New orbit here circles what's in the middle of the picture, passing where "
        L"you are halfway through: radius units across from it, height above it, turning that many degrees (negative: the "
        L"other way). Tick the box while aiming at something to keep it in view. Cyan: the shot's path; orange: the picked "
        L"key. Grey: boxes the camera keeps clear of. Changed shots are this map's own: each Xbox conversion puts them in the "
        L"map folder's showcase.txt, and Save to map folder writes it now, without converting.",
        0, 12, 552, 548, 160, IDC_SC_HINT);
    for (int id = IDC_SC_LIST; id <= IDC_SC_HINT; id++) ShowWindow(item(id), SW_HIDE);
    // The bots' waypoints: in place of the list too (hidden until "Bot waypoints...").
    add(L"STATIC", L"", 0, 12, 68, 548, 150, IDC_BOTS_TEXT);
    add(L"BUTTON", L"Show them through walls (faintly)", BS_AUTOCHECKBOX | WS_TABSTOP, 12, 224, 262, 22, IDC_BOTS_THROUGH);
    add(L"BUTTON", L"Show the links that work (green)", BS_AUTOCHECKBOX | WS_TABSTOP, 280, 224, 280, 22, IDC_BOTS_GOOD);
    CheckDlgButton(St->wnd, IDC_BOTS_THROUGH, St->bots_through ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(St->wnd, IDC_BOTS_GOOD, St->bots_good ? BST_CHECKED : BST_UNCHECKED);
    add(L"BUTTON", L"Next red link", BS_PUSHBUTTON | WS_TABSTOP, 12, 252, 130, 26, IDC_BOTS_NEXT);
    add(L"BUTTON", L"Delete", BS_PUSHBUTTON | WS_TABSTOP, 148, 252, 100, 26, IDC_BOTS_DEL);
    add(L"BUTTON", L"Clear mine", BS_PUSHBUTTON | WS_TABSTOP, 254, 252, 110, 26, IDC_BOTS_CLEAR);
    add(L"LISTBOX", L"", LBS_NOTIFY | WS_VSCROLL | WS_BORDER | WS_TABSTOP | LBS_NOINTEGRALHEIGHT, 12, 286, 548, 180, IDC_BOTS_LIST);
    add(L"STATIC",
        L"Halo 1 Unlimited's bots go between these waypoints, walking each link with Halo's AI path search. Click the level to "
        L"put one of yours there (green); click one of yours, or pick it in the list, to Delete it. Yours go into each Xbox "
        L"conversion as item spawn points that never spawn anything. Blue: the map's own (its spawns, item spots and flags; "
        L"one within 1 unit of another makes one with it). Orange: the ones a conversion adds where the search can't "
        L"finish a link and the others don't go round (Xbox tab: \"Add waypoints for Halo 1 Unlimited's bots\"). Lines: "
        L"green, a link the bots walk; amber, one their search can't finish one way, but they learn to go round it; red, one "
        L"with nothing round it. Put yours a few units apart along the way the bots should walk: at the foot, landings and "
        L"top of stairs and ramps, and along walkways up to a raised spot. The search only walks floor the level's AI data "
        L"joins, so a red link across a jump, ladder or lift stays red.",
        0, 12, 474, 548, 240, IDC_BOTS_HINT);
    for (int id = IDC_BOTS_TEXT; id <= IDC_BOTS_HINT; id++) ShowWindow(item(id), SW_HIDE);
    St->view = CreateWindowExW(WS_EX_CLIENTEDGE, L"HMPLevelView", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_TABSTOP,
                               S(572), S(68), S(596), S(447), St->wnd,
                               (HMENU)(INT_PTR)IDC_VIEW, GetModuleHandleW(nullptr), nullptr);
    add(L"BUTTON", L"\u2190", BS_PUSHBUTTON | WS_TABSTOP, 572, 522, 36, 26, IDC_PREV);
    add(L"BUTTON", L"\u2192", BS_PUSHBUTTON | WS_TABSTOP, 612, 522, 36, 26, IDC_NEXT);
    add(L"BUTTON", L"As made", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 658, 524, 72, 22, IDC_BEFORE);
    add(L"BUTTON", L"As converted", BS_AUTORADIOBUTTON, 732, 524, 96, 22, IDC_AFTER);
    add(L"BUTTON", L"What it loses", BS_AUTORADIOBUTTON, 830, 524, 100, 22, IDC_CHANGES);
    add(L"BUTTON", L"Tint selected", BS_AUTOCHECKBOX | WS_GROUP | WS_TABSTOP, 936, 524, 94, 22, IDC_HILITE);
    add(L"BUTTON", L"Portals", BS_AUTOCHECKBOX | WS_TABSTOP, 1034, 524, 64, 22, IDC_PORTALS);
    add(L"BUTTON", L"Objects", BS_AUTOCHECKBOX | WS_TABSTOP, 1100, 524, 68, 22, IDC_OBJECTS);
    add(L"STATIC", L"", SS_NOTIFY, 572, 554, 596, 36, IDC_COST);
    add(L"STATIC", L"Fog:", SS_NOTIFY, 572, 600, 34, 20, IDC_FOG_LBL);
    add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 608, 596, 166, 300, IDC_FOG);
    add(L"EDIT", L"", ES_NUMBER | ES_RIGHT | ES_AUTOHSCROLL | WS_TABSTOP, 778, 597, 38, 22, IDC_FOG_START, WS_EX_CLIENTEDGE);
    SendMessageW(item(IDC_FOG_START), EM_SETLIMITTEXT, 5, 0);
    add(L"STATIC", L"to", SS_NOTIFY | SS_CENTER, 816, 600, 18, 20, IDC_FOG_TO);
    add(L"EDIT", L"", ES_NUMBER | ES_RIGHT | ES_AUTOHSCROLL | WS_TABSTOP, 834, 597, 38, 22, IDC_FOG_DIST, WS_EX_CLIENTEDGE);
    SendMessageW(item(IDC_FOG_DIST), EM_SETLIMITTEXT, 5, 0);
    add(L"STATIC", L"units", SS_NOTIFY, 876, 600, 30, 20, IDC_FOG_UNITS);
    add(L"STATIC", L"Hide scenery:", SS_NOTIFY, 908, 600, 76, 20, IDC_FAR_LBL);
    add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 986, 596, 182, 300, IDC_FAR);
    SendMessageW(item(IDC_FAR), CB_SETDROPPEDWIDTH, S(200), 0);
    {
        const wchar_t *tiny_items[kTinyCount] = {L"Never (the default)", L"Below 4 px (8 widescreen)", L"Below 8 px (16 widescreen)",
                                                 L"Below 12 px (24 widescreen)", L"Below 16 px (32 widescreen)"};
        for (auto t : tiny_items) SendMessageW(item(IDC_FAR), CB_ADDSTRING, 0, (LPARAM)t);
    }
    add(L"STATIC", L"Weather:", SS_NOTIFY, 572, 630, 56, 20, IDC_WEATHER_LBL);
    add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 630, 626, 244, 300, IDC_WEATHER);
    add(L"STATIC", L"Sky:", SS_NOTIFY, 884, 630, 42, 20, IDC_SKY_LBL);
    add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 928, 626, 240, 300, IDC_SKY);
    SendMessageW(item(IDC_SKY), CB_ADDSTRING, 0, (LPARAM)L"Fewer layers (looks the same)");
    SendMessageW(item(IDC_SKY), CB_ADDSTRING, 0, (LPARAM)L"As made");
    SendMessageW(item(IDC_SKY), CB_SETCURSEL, std::clamp(St->picks.sky, 0, 1), 0);
    add(L"STATIC", L"Riders:", SS_NOTIFY, 572, 660, 46, 20, IDC_RIDERS_LBL);
    add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 620, 656, 254, 300, IDC_RIDERS);
    add(L"STATIC", L"Props:", SS_NOTIFY, 884, 660, 42, 20, IDC_PROPS_LBL);
    add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 928, 656, 240, 300, IDC_PROPS);
    add(L"STATIC", L"Effects:", SS_NOTIFY, 572, 690, 50, 20, IDC_FX_LBL);
    add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 624, 686, 250, 300, IDC_FX);
    for (auto t : kEffectsItems) SendMessageW(item(IDC_FX), CB_ADDSTRING, 0, (LPARAM)t);
    for (int i = 0; i < 3; i++)
        if (kEffectsPick[i] == St->picks.effects) SendMessageW(item(IDC_FX), CB_SETCURSEL, i, 0);
    add(L"STATIC", L"Lights:", SS_NOTIFY, 884, 690, 42, 20, IDC_LIGHTS_LBL);
    add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 928, 686, 240, 300, IDC_LIGHTS);
    add(L"BUTTON", L"Measure again", BS_PUSHBUTTON | WS_TABSTOP, 12, 726, 120, 26, IDC_REMEASURE);
    add(L"BUTTON", L"Bot waypoints...", BS_PUSHBUTTON | WS_TABSTOP, 138, 726, 150, 26, IDC_BOTS);
    add(L"STATIC", L"Shows: how much of the view is its reflection. Covers: how much of the screen. Passes: triangles drawn again.",
        0, 298, 730, 660, 20, IDC_ABOUT);
    add(L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, 966, 726, 96, 26, IDC_OKB);
    add(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 1072, 726, 96, 26, IDC_CANCELB);
    CheckRadioButton(St->wnd, IDC_MODE0, IDC_MODE2, IDC_MODE0 + St->picks.mode);
    CheckRadioButton(St->wnd, IDC_BEFORE, IDC_CHANGES, IDC_AFTER);
    CheckDlgButton(St->wnd, IDC_HILITE, BST_CHECKED);
    St->objects = g_objects;
    CheckDlgButton(St->wnd, IDC_OBJECTS, St->objects ? BST_CHECKED : BST_UNCHECKED);
    {
        int k = 0;   // (the nearest of the choices)
        for (int i = 1; i < kTinyCount; i++)
            if (St->picks.tiny_px > 0 && std::fabs(kTiny[i] - St->picks.tiny_px) < std::fabs(kTiny[k] - St->picks.tiny_px)) k = i;
        if (St->picks.tiny_px > 0 && k == 0) k = 1;
        SendMessageW(item(IDC_FAR), CB_SETCURSEL, k, 0);
    }
    fill_fog();
    fill_weather();
    fill_content();

    // Tooltips.
    St->tips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT,
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, St->wnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(St->tips, TTM_SETMAXTIPWIDTH, 0, S(380));
    SendMessageW(St->tips, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
    auto tip = [](int id, const wchar_t *text) {
        TOOLINFOW ti{};
        ti.cbSize = sizeof(ti);
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = St->wnd;
        ti.uId = (UINT_PTR)item(id);
        ti.lpszText = (LPWSTR)text;
        SendMessageW(St->tips, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    };
    tip(IDC_MODE2, L"The level keeps every reflection, as the map has it.");
    tip(IDC_MODE0, L"Drops the reflections that cost the Xbox the most passes for how little they show, until the level "
                   L"costs about what Sidewinder does. Cheaper to draw, but surfaces look flatter in places: check with "
                   L"\"What it loses\" before you convert.");
    tip(IDC_MODE1, L"Exactly the shaders you tick lose their reflection. Ticking or unticking one switches to this.");
    tip(IDC_BEFORE, L"The level with every reflection, as the map has it.");
    tip(IDC_AFTER, L"The level with the reflections your choice above keeps.");
    tip(IDC_CHANGES, L"The converted picture dimmed, with the reflections that go lit up: what the conversion takes away.");
    tip(IDC_HILITE, L"Tints the shader selected in the list pink, so you can find its surfaces.");
    tip(IDC_PORTALS, L"Outlines each portal in yellow (the openings the engine looks through from one room to the next), "
                     L"glowing orange where you can see through it, and tints each room (cluster) a colour of its own. The engine draws the room you're in, then only the rooms "
                     L"it can see through portals, so walls between rooms hide what's behind them.");
    tip(IDC_SC_NAME, L"The shot's name, for people (Halo 1 Unlimited's file keeps 31 characters).");
    tip(IDC_SC_KPREV, L"The key before: picked, and the picture goes to it.");
    tip(IDC_SC_KNEXT, L"The key after: picked, and the picture goes to it.");
    tip(IDC_SC_KEY, L"The picked key (orange in the picture). Double-click a yellow key in the picture to pick it.");
    tip(IDC_SC_KGO, L"Puts the picture where the picked key is, looking as it does.");
    tip(IDC_SC_KMOVE, L"Moves the picked key to where the picture is now, looking the way it does (at the same time in the shot).");
    tip(IDC_SC_ADD, L"A new key where the picture is, after the picked one. At an even speed the shot's timing follows (a new "
                    L"last key makes it 3 seconds longer); with times as keyed, halfway to the next key, or 3 seconds after "
                    L"the last.");
    tip(IDC_SC_PACE, L"How the shot spends its seconds. Even speed: the camera moves along its keys at one speed all the way, "
                     L"however close or far apart the keys are, the seconds setting how fast. Slow in: it starts from still "
                     L"(up to 2 seconds of speeding up), for a held start; Slow out: it comes to rest at the end; or both. "
                     L"Times as keyed: each key's own time, as the file had them (a key every so often: even time between "
                     L"keys, not even speed).");
    tip(IDC_SC_UNDO, L"Removes the picked key. Without the first, the shot starts at the next one; without the last, it ends at "
                     L"the one before.");
    tip(IDC_SC_SAVE, L"Writes these shots into the map folder's showcase.txt now (beside the converted map and its info.txt, "
                     L"where Halo 1 Unlimited reads them), without converting the map again. They're kept for the next "
                     L"conversion too.");
    const wchar_t *orbit_tip = L"New orbit here: a shot circling what's in the middle of the picture, looking at it, passing "
                               L"where you are halfway through (it starts half the turn before). Radius: how far from it the camera is, across (world units; 1 unit = 10 "
                               L"feet, about 3 m). Height: how far above it. Turn: how many degrees round it the shot goes "
                               L"(360 a whole circle; negative: clockwise). With an orbit picked in the list, these change it.";
    for (int id : {IDC_SC_ORBIT, IDC_SC_ORBIT_LBL, IDC_SC_ORAD, IDC_SC_ORAD_LBL, IDC_SC_OHGT, IDC_SC_OHGT_LBL, IDC_SC_OAR, IDC_SC_OAR_LBL})
        tip(id, orbit_tip);
    tip(IDC_OBJECTS, L"Draws the objects in the level when a game starts: the scenery, machines, controls, light fixtures, "
                     L"vehicles, weapons and equipment the map places, and an item on each multiplayer spawn point (its "
                     L"collection's first). Each is lit as the engine lights an object: from the lightmap straight below it "
                     L"(an ambient light, the light from the direction the lightmap's comes from there, and its surface's "
                     L"colour bounced up). Double-click one to name it and its shader.");
    tip(IDC_COST, L"Every level triangle is drawn at least twice (its lightmap, then its textures); a reflection draws it once "
                  L"or twice more. That's the processor's time as well as the graphics chip's: it sends the level again. "
                  L"Measured on an Xbox, about 0.45 ms of each 33 ms frame per 1,000 passes (Deathtrap's reflections: 1.4 ms "
                  L"a frame on average, 3.2 ms in its busiest view). Dropping one also frees its cube map's memory. Typical: "
                  L"half of the views from where players stand draw more; busy: one in ten does. Sidewinder is among the "
                  L"busiest stock maps that still run smoothly.");
    const wchar_t *fog_tip = L"Halo's fog rises evenly with distance: none before its start distance, then thicker and thicker up "
                             L"to its opaque distance, as thick as it gets from there on (the two boxes show the level's). The Xbox "
                             L"stops drawing anything (level, objects, effects) past the opaque distance, but only when the fog is "
                             L"fully opaque there: at 99% it still draws everything. Automatic makes fog that's nearly opaque (90% "
                             L"or more) fully so. Nearer opaque fog draws less, but the level looks foggier. Other: type both "
                             L"distances yourself. The picture shows the fog as the conversion leaves it.";
    tip(IDC_FOG, fog_tip);
    tip(IDC_FOG_LBL, fog_tip);
    const wchar_t *start_tip = L"Where the fog starts, in world units (1 unit = 10 feet, about 3 m), with \"Other\" chosen: "
                               L"nothing nearer is fogged, and it thickens evenly from there. 0 starts it at the camera. It "
                               L"stays short of where it's fully opaque. The picture follows as you type.";
    tip(IDC_FOG_START, start_tip);
    tip(IDC_FOG_TO, start_tip);
    const wchar_t *dist_tip = L"Where the fog turns fully opaque, in world units (1 unit = 10 feet, about 3 m), with \"Other\" "
                              L"chosen: nothing past it is drawn. It can't be nearer than where the fog starts. The picture "
                              L"follows as you type.";
    tip(IDC_FOG_DIST, dist_tip);
    tip(IDC_FOG_UNITS, dist_tip);
    const wchar_t *far_tip = L"Scenery and devices (trees, rocks, props, machines; not vehicles, characters or weapons) aren't "
                             L"drawn once they're smaller on screen than this, across: only specks go. A 1-unit prop goes at 8 px "
                             L"about 57 units away; a 16-unit tree not until about 900. Halo 1 Unlimited counts models at half "
                             L"their size in widescreen, so there they go at twice the size. The engine measures size by how far "
                             L"ahead of the camera something is, so things at the screen's edges stay a little longer than ones "
                             L"you look at; at these sizes that's hard to see. The picture draws every object however small.";
    tip(IDC_FAR, far_tip);
    tip(IDC_FAR_LBL, far_tip);
    const wchar_t *weather_tip = L"Rain and snow cost the Xbox's processor every frame, indoors too: every drop is moved, clipped "
                                 L"and drawn (Deathtrap's rain: 2.5 ms of a 33 ms frame). The engine keeps at most 512 drops. "
                                 L"Lighter keeps the rain just as dense around you but ends it 5 units away (Bungie's rain on "
                                 L"343 Guilty Spark ends there), so far fewer drops; half the drops makes it thinner everywhere; "
                                 L"off removes it. The picture doesn't show weather.";
    tip(IDC_WEATHER, weather_tip);
    tip(IDC_WEATHER_LBL, weather_tip);
    const wchar_t *sky_tip = L"The Xbox draws the sky first in every frame, over the whole view, before the level covers most of "
                             L"it: each layer of it is a screenful of blending. Bungie's multiplayer skies draw 0.8 to 1.4 layers "
                             L"per pixel; Precipice's draws 2.8. Fewer layers: layers on the same dome drawn in one pass (their "
                             L"textures combined; moving clouds keep moving) and clear parts cut away, so the picture stays the "
                             L"same (Precipice: 1.6). The map's own skies only; the conversion's report gives the numbers.";
    tip(IDC_SKY, sky_tip);
    tip(IDC_SKY_LBL, sky_tip);
    const wchar_t *fx_tip = L"Every particle alive is moved and drawn each frame, and its sprite blended over what's behind it "
                            L"(the engine keeps at most 1,024, every effect's together). Bungie's busiest Xbox multiplayer "
                            L"effects: made once (an explosion, an impact), about 200 particles and 188 square units of sprites "
                            L"at once (the rocket's explosion); looping on an object (a fire, a beam), 300 particles and 1,150 "
                            L"square units kept alive (the plasma flame). No busier than Bungie's: a map's own effect past "
                            L"those makes just enough fewer particles to fit (each the same size as before); the rest stay as "
                            L"made. Lighter: looping ones on objects keep a quarter of their particles too. The map's own "
                            L"effects only; the conversion's report names them. The picture doesn't show effects.";
    tip(IDC_FX, fx_tip);
    tip(IDC_FX_LBL, fx_tip);
    const wchar_t *riders_tip = L"Units the scripts give AI and then attach to another object, like Deathtrap's marines sitting in its "
                                L"moving warthogs: they can't go anywhere, but their AI still thinks every tick (Deathtrap's 9: 1.5 ms "
                                L"of each Xbox tick). Without AI they sit where they're attached, as before. Try the map once: a rider "
                                L"meant to shoot at players would stop.";
    tip(IDC_RIDERS, riders_tip);
    tip(IDC_RIDERS_LBL, riders_tip);
    const wchar_t *props_tip = L"Small scenery (1 unit or less) whose collision model is very detailed (300 surfaces or more; "
                               L"Deathtrap's pilot chairs have 416, a warthog's whole chassis 47). Each tick a player's movement is "
                               L"tested against the collision models it's near: standing by one of Deathtrap's chairs cost 5-6 ms a "
                               L"tick. Without collision, players and shots pass through them.";
    tip(IDC_PROPS, props_tip);
    tip(IDC_PROPS_LBL, props_tip);
    const wchar_t *lights_tip = L"Dynamic lights on the objects the map places, always on: Deathtrap's three warthogs each carry a "
                                L"headlight. Each one in view has the Xbox draw the level and the objects around it again, twice: "
                                L"once for its light and once for its shine (specular). Measured on Deathtrap: about 3.4 ms of a "
                                L"frame with two in view (the shine 2.4 of it), and 1.2 ms setting the lights up. No specular keeps "
                                L"the light and drops the shine pass; Off drops the light (a lens flare stays).";
    tip(IDC_LIGHTS, lights_tip);
    tip(IDC_LIGHTS_LBL, lights_tip);
    tip(IDC_VIEW, L"Click here, then fly: drag with the mouse to look around, W A S D to move where you look, Q / E to go "
                  L"down and up (Shift faster, Ctrl slower), the mouse wheel to step forward and back. Double-click a "
                  L"surface to find its shader.");
    tip(IDC_PREV, L"The previous view (the selected shader's, or the ones that see most of the level).");
    tip(IDC_NEXT, L"The next view.");
}

LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (!St) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_LS_LOG: {
        std::unique_ptr<Note> n((Note *)lp);
        if (n->key != St->key) return 0;
        std::string s = n->text;
        while (!s.empty() && (s[0] == ' ')) s.erase(0, 1);
        if (!s.empty()) SetWindowTextW(item(IDC_STATUS), (L"Measuring: " + W(s)).c_str());
        return 0;
    }
    case WM_LS_DONE: {
        std::unique_ptr<Note> n((Note *)lp);
        EnableWindow(item(IDC_REMEASURE), TRUE);
        if (n->key != St->key) {
            if (!St->data) SetWindowTextW(item(IDC_STATUS), L"The other map is measured: press \"Measure again\" for this one.");
            return 0;
        }
        if (!wp || !load_file(n->file))
            SetWindowTextW(item(IDC_STATUS), (L"The level couldn't be measured: " + W(n->text.empty() ? "its file couldn't be read." : n->text)).c_str());
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR *n = (NMHDR *)lp;
        if (n->idFrom == IDC_SC_LIST && n->code == LVN_ITEMCHANGED && !St->sc_filling) {
            NMLISTVIEW *lv = (NMLISTVIEW *)lp;
            if ((lv->uChanged & LVIF_STATE) && (lv->uNewState & LVIS_SELECTED) && !(lv->uOldState & LVIS_SELECTED)) {
                sc_stop();
                sc_select(lv->iItem, true);
            }
            return 0;
        }
        if (n->idFrom == IDC_LIST && n->code == LVN_ITEMCHANGED && !St->filling && St->data) {
            NMLISTVIEW *lv = (NMLISTVIEW *)lp;
            if (lv->iItem < 0 || (size_t)lv->iItem >= St->row_shader.size()) return 0;
            int s = St->row_shader[(size_t)lv->iItem];
            if ((lv->uChanged & LVIF_STATE) && ((lv->uNewState ^ lv->uOldState) & LVIS_STATEIMAGEMASK)) {
                bool tick = ListView_GetCheckState(St->list, lv->iItem) != 0;
                if (tick != dropped(s)) {
                    // A tick changed: from here on these are your picks (starting from what was shown).
                    if (St->picks.mode != 1) {
                        std::set<std::string> now;
                        for (size_t k = 0; k < St->data->a.shaders.size(); k++)
                            if (dropped((int)k)) now.insert(St->data->a.shaders[k].tag);
                        St->picks.drops = now;
                    }
                    const std::string &tag = St->data->a.shaders[(size_t)s].tag;
                    if (tick) St->picks.drops.insert(tag);
                    else St->picks.drops.erase(tag);
                    set_mode(1);
                }
            }
            if ((lv->uChanged & LVIF_STATE) && (lv->uNewState & LVIS_SELECTED) && !(lv->uOldState & LVIS_SELECTED)) {
                St->selected = s;
                pick_views();
                if (St->keep_camera) render(false);
                else go_to_view();
            }
        }
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        switch (id) {
        case IDC_MODE0: case IDC_MODE1: case IDC_MODE2:
            if (HIWORD(wp) == BN_CLICKED) {
                int mode = id - IDC_MODE0;
                if (mode == 1 && St->picks.mode != 1 && St->data && St->picks.drops.empty())   // (start from what's shown)
                    for (size_t k = 0; k < St->data->a.shaders.size(); k++)
                        if (dropped((int)k)) St->picks.drops.insert(St->data->a.shaders[k].tag);
                set_mode(mode);
            }
            return 0;
        case IDC_PREV: case IDC_NEXT:
            if (!St->views.empty()) {
                St->view_at = (St->view_at + (id == IDC_NEXT ? 1 : St->views.size() - 1)) % St->views.size();
                go_to_view();
            }
            return 0;
        case IDC_BEFORE: case IDC_AFTER: case IDC_CHANGES:
            St->show = id == IDC_BEFORE ? 0 : id == IDC_AFTER ? 1 : 2;
            render(false);
            return 0;
        case IDC_FOG:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int k = (int)SendMessageW(item(IDC_FOG), CB_GETCURSEL, 0, 0);
                if (k >= 0 && k <= kFogOther) {
                    St->picks.fog = k <= 1 ? k : k == kFogOther ? 3 : 2;
                    St->picks.fog_scale = kFogScale[k];
                    if (k == kFogOther && St->picks.fog_distance <= 0) St->picks.fog_distance = std::round(level_fog().opaque);
                    show_fog_distance();
                    if (k == kFogOther) {
                        SetFocus(item(IDC_FOG_START));
                        SendMessageW(item(IDC_FOG_START), EM_SETSEL, 0, -1);
                    }
                    fog_cut_status();
                    render(false);
                }
            }
            return 0;
        case IDC_FOG_DIST:
            if (HIWORD(wp) == EN_CHANGE && !St->filling && St->picks.fog == 3) {
                wchar_t b[16] = L"";
                GetWindowTextW(item(IDC_FOG_DIST), b, 16);
                double d = wcstod(b, nullptr);
                if (d >= 1) {   // (an empty box, or 0, while typing: the last distance stays)
                    St->picks.fog_distance = d;
                    fog_cut_status();
                    render(false);
                }
            }
            return 0;
        case IDC_FOG_START:
            if (HIWORD(wp) == EN_CHANGE && !St->filling && St->picks.fog == 3) {
                wchar_t b[16] = L"";
                GetWindowTextW(item(IDC_FOG_START), b, 16);
                if (b[0]) {   // (an empty box while typing: the last start stays)
                    St->picks.fog_start = wcstod(b, nullptr);
                    render(false);
                }
            }
            return 0;
        case IDC_FAR:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int k = (int)SendMessageW(item(IDC_FAR), CB_GETCURSEL, 0, 0);
                if (k >= 0 && k < kTinyCount) St->picks.tiny_px = kTiny[k];
            }
            return 0;
        case IDC_WEATHER:
            if (HIWORD(wp) == CBN_SELCHANGE && St->data && St->data->p.weather.systems) {
                int k = (int)SendMessageW(item(IDC_WEATHER), CB_GETCURSEL, 0, 0);
                if (k >= 0 && k <= 3) St->picks.weather = k;
            }
            return 0;
        case IDC_SKY:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int k = (int)SendMessageW(item(IDC_SKY), CB_GETCURSEL, 0, 0);
                if (k == 0 || k == 1) St->picks.sky = k;
            }
            return 0;
        case IDC_FX:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int k = (int)SendMessageW(item(IDC_FX), CB_GETCURSEL, 0, 0);
                if (k >= 0 && k < 3) St->picks.effects = kEffectsPick[k];
            }
            return 0;
        case IDC_LIGHTS:
            if (HIWORD(wp) == CBN_SELCHANGE && St->data && St->data->p.content.object_lights > 0) {
                int k = (int)SendMessageW(item(IDC_LIGHTS), CB_GETCURSEL, 0, 0);
                if (k >= 0 && k <= 2) St->picks.lights = k;
            }
            return 0;
        case IDC_RIDERS: case IDC_PROPS:
            if (HIWORD(wp) == CBN_SELCHANGE && St->data) {
                int k = (int)SendMessageW(item(id), CB_GETCURSEL, 0, 0);
                if (k == 0 || k == 1) (id == IDC_RIDERS ? St->picks.riders : St->picks.props) = k;
            }
            return 0;
        case IDC_PORTALS:
            St->portals = IsDlgButtonChecked(h, IDC_PORTALS) == BST_CHECKED;
            render(false);
            return 0;
        case IDC_OBJECTS:
            St->objects = g_objects = IsDlgButtonChecked(h, IDC_OBJECTS) == BST_CHECKED;
            render(false);
            return 0;
        case IDC_HILITE:
            St->highlight = IsDlgButtonChecked(h, IDC_HILITE) == BST_CHECKED;
            render(false);
            return 0;
        case IDC_REMEASURE:
            start_measure();
            return 0;
        case IDC_SHOWCASE:
            set_sc_mode(!St->sc_mode);
            return 0;
        case IDC_BOTS:
            set_bots_mode(!St->bots_mode);
            return 0;
        case IDC_BOTS_THROUGH:
            St->bots_through = IsDlgButtonChecked(h, IDC_BOTS_THROUGH) == BST_CHECKED;
            render(false);
            return 0;
        case IDC_BOTS_GOOD:
            St->bots_good = IsDlgButtonChecked(h, IDC_BOTS_GOOD) == BST_CHECKED;
            render(false);
            return 0;
        case IDC_BOTS_NEXT:
            bots_next();
            return 0;
        case IDC_BOTS_DEL:
            bots_delete();
            return 0;
        case IDC_BOTS_CLEAR:
            if (!St->picks.bots.empty() &&
                MessageBoxW(h, L"Remove all of your waypoints for this map?", L"Bot waypoints", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                St->picks.bots.clear();
                St->bot_sel = -1;
                bots_plan();
            }
            return 0;
        case IDC_BOTS_LIST:
            if (HIWORD(wp) == LBN_SELCHANGE) {
                int k = (int)SendMessageW(item(IDC_BOTS_LIST), LB_GETCURSEL, 0, 0);
                St->bot_sel = k >= 0 && (size_t)k < St->picks.bots.size() ? k : -1;
                EnableWindow(item(IDC_BOTS_DEL), St->bot_sel >= 0);
                if (St->bot_sel >= 0) {   // (the picture looks at it)
                    const auto &p = St->picks.bots[(size_t)St->bot_sel];
                    double yaw = St->cam.yaw;
                    St->cam.eye = {p[0] - std::cos(yaw) * 5, p[1] - std::sin(yaw) * 5, p[2] + 3};
                    St->cam.pitch = -std::atan2(3.0, 5.0);
                }
                render(false);
            }
            return 0;
        case IDC_SC_PLAY: case IDC_SC_PLAYSHOT:
            sc_play(id == IDC_SC_PLAY ? -1 : St->sc_sel);
            return 0;
        case IDC_SC_STOP:
            sc_stop();
            return 0;
        case IDC_SC_NEW: {
            if (!St->data) return 0;
            sc_stop();
            showcase::Shot shot;
            shot.label = "shot " + std::to_string(St->sc.shots.size() + 1);
            shot.seconds = 4;
            shot.pace = showcase::Pace::even;   // (its keys timed along the path: add keys, set the seconds)
            shot.via.push_back(key_here());
            showcase::retime(shot);
            size_t at = St->sc_sel >= 0 ? (size_t)St->sc_sel + 1 : St->sc.shots.size();
            St->sc.shots.insert(St->sc.shots.begin() + (long)std::min(at, St->sc.shots.size()), shot);
            St->sc_sel = (int)std::min(at, St->sc.shots.size() - 1);
            St->sc_key = 0;
            sc_changed();
            return 0;
        }
        case IDC_SC_DELETE: case IDC_SC_UP: case IDC_SC_DOWN: {
            int sel = St->sc_sel;
            if (sel < 0 || (size_t)sel >= St->sc.shots.size()) return 0;
            sc_stop();
            auto &shots = St->sc.shots;
            if (id == IDC_SC_DELETE) {
                shots.erase(shots.begin() + sel);
                St->sc_sel = std::min(sel, (int)shots.size() - 1);
                St->sc_key = 0;
            } else if (id == IDC_SC_UP) {
                if (sel == 0) return 0;
                std::swap(shots[(size_t)sel], shots[(size_t)sel - 1]);
                St->sc_sel = sel - 1;
            } else {
                if ((size_t)sel + 1 >= shots.size()) return 0;
                std::swap(shots[(size_t)sel], shots[(size_t)sel + 1]);
                St->sc_sel = sel + 1;
            }
            sc_changed();
            return 0;
        }
        // The selected shot's keys: pick one, go to it, move it, add one after it, remove it.
        case IDC_SC_KPREV: case IDC_SC_KNEXT: case IDC_SC_KGO: {
            showcase::Shot *shot = sc_shot();
            if (!shot || showcase::path_keys(*shot).empty()) return 0;
            sc_stop();
            const auto &pk = showcase::path_keys(*shot);
            if (id != IDC_SC_KGO) St->sc_key = std::clamp(St->sc_key + (id == IDC_SC_KNEXT ? 1 : -1), 0, (int)pk.size() - 1);
            sc_key_show();
            camera_to(showcase::at(*shot, pk[(size_t)St->sc_key].t));
            render(false);
            return 0;
        }
        case IDC_SC_ADD: case IDC_SC_KMOVE: case IDC_SC_UNDO: {
            showcase::Shot *shot = sc_shot();
            if (!shot) return 0;
            sc_stop();
            const bool paced = shot->pace != showcase::Pace::keyed;   // (then its keys' times come from the path: retime)
            auto &keys = showcase::path_keys(*shot);
            int k = keys.empty() ? -1 : std::clamp(St->sc_key, 0, (int)keys.size() - 1);
            if (id == IDC_SC_ADD && paced) {   // (after the picked key; a new last one makes the shot 3 seconds longer)
                showcase::Key nk = key_here();
                if (k >= 0 && (size_t)k + 1 == keys.size()) shot->seconds = std::min(600.0, shot->seconds + 3);
                keys.insert(keys.begin() + (k + 1), nk);
                St->sc_key = k + 1;
            } else if (id == IDC_SC_UNDO && paced) {
                if (k < 0 || keys.size() <= 1) return 0;
                keys.erase(keys.begin() + k);
                St->sc_key = std::min(k, (int)keys.size() - 1);
            } else if (id == IDC_SC_ADD) {   // (after the picked key: halfway to the next one, or 3 seconds after the last)
                showcase::Key nk = key_here();
                if (k < 0) {
                    nk.t = 0;
                    keys.push_back(nk);
                    St->sc_key = 0;
                } else if ((size_t)k + 1 < keys.size()) {
                    nk.t = (keys[(size_t)k].t + keys[(size_t)k + 1].t) / 2;
                    keys.insert(keys.begin() + k + 1, nk);
                    St->sc_key = k + 1;
                } else {
                    nk.t = keys[(size_t)k].t + 3;
                    keys.push_back(nk);
                    shot->seconds = std::max(nk.t, 1.0);
                    St->sc_key = k + 1;
                }
            } else if (id == IDC_SC_KMOVE) {   // (where the picture is now, at the same time in the shot)
                if (k < 0) return 0;
                showcase::Key nk = key_here();
                nk.t = keys[(size_t)k].t;
                nk.fov = keys[(size_t)k].fov;
                keys[(size_t)k] = nk;
            } else {   // (the first one gone: the shot starts at the next; the last one gone: it ends at the one before)
                if (k < 0 || keys.size() <= 1) return 0;
                keys.erase(keys.begin() + k);
                if (k == 0) {
                    double shift = keys[0].t;
                    for (auto &q : keys) q.t -= shift;
                    shot->seconds = std::max(0.5, shot->seconds - shift);
                } else if ((size_t)k == keys.size() && keys.back().t > 0) {
                    shot->seconds = keys.back().t;
                }
                St->sc_key = std::min(k, (int)keys.size() - 1);
            }
            shot->orbit.on = false;   // (its own keys now)
            showcase::retime(*shot);
            sc_changed();
            sc_check(*shot);
            return 0;
        }
        case IDC_SC_PACE:
            if (HIWORD(wp) == CBN_SELCHANGE && !St->sc_filling && sc_shot()) {
                static const showcase::Pace paces[5] = {showcase::Pace::even, showcase::Pace::in, showcase::Pace::out,
                                                        showcase::Pace::both, showcase::Pace::keyed};
                int sel = (int)SendMessageW(item(IDC_SC_PACE), CB_GETCURSEL, 0, 0);
                if (sel < 0 || sel > 4) return 0;
                showcase::Shot &shot = *sc_shot();
                showcase::Pace was = shot.pace, now = paces[sel];
                if (now == was) return 0;
                sc_stop();
                if (was == showcase::Pace::keyed) shot.via = shot.keys;   // (its keys become the path, timed along it)
                shot.pace = now;
                if (now == showcase::Pace::keyed) shot.via.clear();   // (the keys as they're timed now, each its own time)
                else showcase::retime(shot);
                St->sc_key = std::min(St->sc_key, std::max(0, (int)showcase::path_keys(shot).size() - 1));
                sc_changed();
                sc_check(shot);
            }
            return 0;
        case IDC_SC_NAME:
            if (HIWORD(wp) == EN_CHANGE && !St->sc_filling && sc_shot()) {
                wchar_t b[64] = {};
                GetWindowTextW(item(IDC_SC_NAME), b, 64);
                std::string name = showcase::clean_label(U(b));
                showcase::Shot &shot = *sc_shot();
                if (name == shot.label) return 0;
                shot.label = name;
                St->sc_custom = true;
                std::wstring row = std::to_wstring(St->sc_sel + 1) + L". " + W(name.empty() ? std::string("shot") : name);
                ListView_SetItemText(St->sc_list, St->sc_sel, 0, (LPWSTR)row.c_str());
                sc_status();
            }
            return 0;
        // The orbit boxes: typed into, they change the selected orbit (else they're for the next "New orbit here").
        case IDC_SC_ORAD: case IDC_SC_OHGT: case IDC_SC_OAR:
            if (HIWORD(wp) == EN_CHANGE && !St->sc_filling) {
                wchar_t b[16] = {};
                GetWindowTextW(item(id), b, 16);
                wchar_t *end = nullptr;
                double v = wcstod(b, &end);
                if (end == b) return 0;   // (empty, or only a sign, while typing)
                if (id == IDC_SC_ORAD) { if (v < 0.5 || v > 200) return 0; St->orbit_radius = v; }
                else if (id == IDC_SC_OHGT) { if (v < -50 || v > 100) return 0; St->orbit_height = v; }
                else { if (std::fabs(v) < 5 || std::fabs(v) > 1080) return 0; St->orbit_around = v; }
                showcase::Shot *shot = sc_shot();
                if (!shot || !shot->orbit.on) return 0;
                sc_stop();
                // (round the same middle: the side it was made from)
                double middle = shot->orbit.start + shot->orbit.sweep / 2;
                showcase::Shot o = showcase::orbit_shot(shot->at, St->orbit_radius, St->orbit_height, middle - St->orbit_around / 2,
                                                        St->orbit_around, shot->seconds, shot->label);
                if (shot->pace != showcase::Pace::keyed) {   // (its pace kept: the circle is the path)
                    o.pace = shot->pace;
                    o.via = o.keys;
                    showcase::retime(o);
                }
                *shot = o;
                St->sc_custom = true;
                ListView_SetItemText(St->sc_list, St->sc_sel, 2, (LPWSTR)std::to_wstring(showcase::path_keys(*shot).size()).c_str());
                St->sc_key = (int)showcase::path_keys(*shot).size() / 2;
                sc_key_show();
                camera_to(showcase::at(*shot, shot->seconds / 2));
                sc_status();
                sc_check(*shot);
                render(false);
            }
            return 0;
        case IDC_SC_ORBIT: {   // (round what's in the middle of the picture, the camera's side of it halfway through)
            if (!St->data) return 0;
            sc_stop();
            std::array<double, 3> at{};
            if (!middle_point(at)) {
                SetWindowTextW(item(IDC_STATUS), L"Nothing in the middle of the picture to circle: aim at it first.");
                return 0;
            }
            double middle = deg_of(std::atan2(St->cam.eye[1] - at[1], St->cam.eye[0] - at[0]));
            showcase::Shot shot = showcase::orbit_shot(at, St->orbit_radius, St->orbit_height, middle - St->orbit_around / 2,
                                                       St->orbit_around, 12, "orbit " + std::to_string(St->sc.shots.size() + 1));
            size_t pos = St->sc_sel >= 0 ? (size_t)St->sc_sel + 1 : St->sc.shots.size();
            pos = std::min(pos, St->sc.shots.size());
            St->sc.shots.insert(St->sc.shots.begin() + (long)pos, shot);
            St->sc_sel = (int)pos;
            St->sc_key = (int)shot.keys.size() / 2;
            sc_changed();
            camera_to(showcase::at(St->sc.shots[pos], shot.seconds / 2));   // (where it was looked at from)
            render(false);
            sc_check(St->sc.shots[pos]);
            return 0;
        }
        case IDC_SC_SECS:
            if (HIWORD(wp) == EN_CHANGE && !St->sc_filling && St->sc_sel >= 0 && (size_t)St->sc_sel < St->sc.shots.size()) {
                wchar_t b[16] = {};
                GetWindowTextW(item(IDC_SC_SECS), b, 16);
                double v = _wtof(b);
                showcase::Shot &shot = St->sc.shots[(size_t)St->sc_sel];
                if (v < 0.5 || v > 600 || std::fabs(v - shot.seconds) < 1e-9) return 0;
                // (the keys spread out or closer together with it: the same path, slower or faster)
                if (shot.pace != showcase::Pace::keyed) {   // (the same path, at the pace the seconds give)
                    shot.seconds = v;
                    showcase::retime(shot);
                } else if (shot.orbit.on) {
                    shot = showcase::orbit_shot(shot.at, shot.orbit.radius, shot.orbit.height, shot.orbit.start, shot.orbit.sweep, v,
                                                shot.label);
                    ListView_SetItemText(St->sc_list, St->sc_sel, 2, (LPWSTR)std::to_wstring(shot.keys.size()).c_str());
                } else {
                    if (shot.keys.size() > 1 && shot.seconds > 0)
                        for (auto &k : shot.keys) k.t *= v / shot.seconds;
                    shot.seconds = v;
                }
                St->sc_custom = true;
                ListView_SetItemText(St->sc_list, St->sc_sel, 1, (LPWSTR)secs(v).c_str());
                sc_status();
                sc_key_show();
                render(false);
            }
            return 0;
        case IDC_SC_LOOK:
            if (HIWORD(wp) == BN_CLICKED && !St->sc_filling && St->sc_sel >= 0 && (size_t)St->sc_sel < St->sc.shots.size()) {
                showcase::Shot &shot = St->sc.shots[(size_t)St->sc_sel];
                if (IsDlgButtonChecked(h, IDC_SC_LOOK) == BST_CHECKED) {
                    std::array<double, 3> at{};
                    if (!middle_point(at)) {
                        CheckDlgButton(h, IDC_SC_LOOK, BST_UNCHECKED);
                        SetWindowTextW(item(IDC_STATUS), L"Nothing of the level in the middle of the picture to look at: aim at it first.");
                        return 0;
                    }
                    shot.look = true;
                    shot.at = at;
                } else {
                    shot.look = false;
                }
                shot.orbit.on = false;   // (an orbit looks at its middle: a new point, or none, and it's keys of its own)
                showcase::retime(shot);
                sc_changed();
                sc_check(shot);
            }
            return 0;
        case IDC_SC_REMAKE:
            if (!St->data) return 0;
            if (St->sc_custom && MessageBoxW(h, L"Replace this map's own shots with ones made from the level?", L"Showcase flythrough",
                                             MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;
            sc_stop();
            St->sc = showcase::generate(St->data->p, &St->sc_report);
            St->sc_custom = false;
            St->sc_sel = St->sc.shots.empty() ? -1 : 0;
            sc_fill();
            sc_status();
            sc_select(St->sc_sel, true);
            return 0;
        case IDC_SC_SAVE: {   // (showcase.txt written now: Picks::showcase as OK leaves it, and the map folder's file)
            if (!St->sc_ready || !St->data || !St->save_showcase) return 0;
            sc_stop();
            std::string text = showcase::write(St->sc, St->data->a.map_name,
                                               St->sc_custom ? "made in its Level window" : "generated from the level");
            std::string said = St->save_showcase(text, St->sc_custom);
            if (St->sc_custom) St->picks.showcase = text;
            SetWindowTextW(item(IDC_STATUS), W(said).c_str());
            return 0;
        }
        case IDC_OKB:
            if (St->sc_ready)   // (shots changed here are the map's own; untouched, the conversion makes them)
                St->picks.showcase = St->sc_custom ? showcase::write(St->sc, St->data ? St->data->a.map_name : std::string(),
                                                                       "made in its Level window")
                                                   : std::string();
            St->ok = true;
            St->done = true;
            return 0;
        case IDC_CANCELB: case IDCANCEL:
            St->done = true;
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        St->done = true;
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}   // namespace

void set_cache_folder(const fs::path &dir) { g_disk = dir; }

void remember(const std::string &key, const fs::path &file) {
    if (key.empty() || file.empty() || !fs::exists(file)) return;
    std::lock_guard<std::mutex> g(g_lock);
    keep_locked(key, file);
}

bool edit(HWND owner, const std::wstring &title, const std::string &key, Measure measure, Picks &picks,
          SaveShowcase save_showcase) {
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!registered) {
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
        InitCommonControlsEx(&icc);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpfnWndProc = proc;
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wc.lpszClassName = L"HMPLevelShaders";
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        RegisterClassExW(&wc);
        WNDCLASSEXW vc{sizeof(vc)};
        vc.hInstance = inst;
        vc.style = CS_OWNDC | CS_DBLCLKS;   // (OpenGL draws on it; double-clicks pick a surface)
        vc.hCursor = LoadCursor(nullptr, IDC_HAND);
        vc.lpfnWndProc = view_proc;
        vc.hbrBackground = nullptr;
        vc.lpszClassName = L"HMPLevelView";
        RegisterClassExW(&vc);
        registered = true;
    }
    State st;
    St = &st;
    {
        HDC dc = GetDC(nullptr);
        st.dpi = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(nullptr, dc);
    }
    st.font = CreateFontW(-MulDiv(9, st.dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    st.bold = CreateFontW(-MulDiv(9, st.dpi, 72), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    st.key = key;
    st.measure = std::move(measure);
    st.picks = picks;
    st.save_showcase = std::move(save_showcase);

    RECT r{0, 0, S(1180), S(764)};
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    AdjustWindowRect(&r, style, FALSE);
    RECT orc;
    GetWindowRect(owner, &orc);
    int w = r.right - r.left, hgt = r.bottom - r.top;
    int x = orc.left + ((orc.right - orc.left) - w) / 2, y = orc.top + ((orc.bottom - orc.top) - hgt) / 2;
    HMONITOR mon = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    if (GetMonitorInfoW(mon, &mi)) {
        x = std::clamp(x, (int)mi.rcWork.left, std::max((int)mi.rcWork.left, (int)mi.rcWork.right - w));
        y = std::clamp(y, (int)mi.rcWork.top, std::max((int)mi.rcWork.top, (int)mi.rcWork.bottom - hgt));
    }
    st.wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"HMPLevelShaders", title.c_str(), style, x, y, w, hgt, owner, nullptr, inst, nullptr);
    create_controls();
    // (none, or HMP_SOFTWARE_PREVIEW set: drawn here, slower)
    if (!GetEnvironmentVariableW(L"HMP_SOFTWARE_PREVIEW", nullptr, 0)) st.gl = glpreview::Renderer::create(st.view);
    g_target = st.wnd;
    {
        fs::path cached;
        {
            std::lock_guard<std::mutex> g(g_lock);
            cached = cached_locked(key);
        }
        if (cached.empty() || !load_file(cached)) start_measure();
    }
    EnableWindow(owner, FALSE);
    ShowWindow(st.wnd, SW_SHOW);
    SetFocus(st.list);

    // While the camera moves, a frame each time round (on the graphics card, at the screen's refresh: SwapBuffers
    // waits for it), else waiting for messages.
    MSG m;
    bool quit = false;
    while (!st.done && !quit) {
        if (st.ticking) {
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                if (m.message == WM_QUIT) { PostQuitMessage((int)m.wParam); quit = true; break; }
                if (!IsDialogMessageW(st.wnd, &m)) {
                    TranslateMessage(&m);
                    DispatchMessageW(&m);
                }
            }
            if (!quit && !st.done && st.ticking) tick();
            continue;
        }
        BOOL got = GetMessageW(&m, nullptr, 0, 0);
        if (got <= 0) { PostQuitMessage(got == 0 ? (int)m.wParam : 0); break; }
        if (!IsDialogMessageW(st.wnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    g_target = nullptr;
    KillTimer(st.view, kTick);
    st.gl.reset();
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    DestroyWindow(st.wnd);
    DeleteObject(st.font);
    DeleteObject(st.bold);
    St = nullptr;
    if (st.ok) picks = st.picks;
    return st.ok;
}

}   // namespace shadersui
