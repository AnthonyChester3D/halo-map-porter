// Halo Map Porter - the Xbox tab's "Level shaders" window (Windows). See shaders_win.hpp.
#include "shaders_win.hpp"

#include <commctrl.h>
#include <windowsx.h>

#include "glpreview_win.hpp"
#include "levelshaders.hpp"

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
    IDC_CHANGES, IDC_HILITE, IDC_COST, IDC_REMEASURE, IDC_OKB, IDC_CANCELB, IDC_ABOUT, IDC_PORTALS,
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
    std::shared_ptr<Data> data;
    std::vector<int> row_shader;   // list row -> shader
    int selected = -1;             // shader
    std::vector<int> views;        // the views the arrows go through
    size_t view_at = 0;
    int show = 1;                  // 0 as made, 1 as converted, 2 what changes
    bool highlight = true, portals = false;
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

int S(int v) { return MulDiv(v, St->dpi, 96); }
std::wstring W(const std::string &s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
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
        St->gl->draw(f);
        ValidateRect(St->view, nullptr);
        return;
    }
    RECT rc;
    GetClientRect(St->view, &rc);
    int w = rc.right, h = rc.bottom;
    if (fast) { w /= 2; h /= 2; }
    if (St->show == 2) {
        // What changes: the converted picture dimmed, with what it loses (the dropped reflections) brightened on top.
        std::vector<uint32_t> made;
        lshade::draw(St->data->p, c, all, -1, w, h, made);
        lshade::draw(St->data->p, c, refl, -1, w, h, St->px);
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
        lshade::draw(St->data->p, c, refl, St->highlight ? St->selected : -1, w, h, St->px, nullptr, nullptr, St->portals);
    }
    St->pw = w;
    St->ph = h;
    InvalidateRect(St->view, nullptr, FALSE);
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
    std::wstring t = L"Triangle passes the Xbox draws of the level in a typical / busy view: " + thousands((long long)before.typical) + L" / " +
                     thousands((long long)before.busy) + L" as made";
    if (going) t += L" \u2192 " + thousands((long long)after.typical) + L" / " + thousands((long long)after.busy) + L" with " +
                    std::to_wstring(going) + L" of its " + std::to_wstring(reflecting) + L" reflections dropped";
    t += L". Stock Xbox maps: Sidewinder " + thousands((long long)stock.typical) + L" / " + thousands((long long)stock.busy) +
         L" (among the busiest).\r\nEvery level triangle is drawn at least twice (lightmap, textures); a reflection draws it once or "
         L"twice more. Dropping one also frees its cube map's memory.";
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
    SetWindowTextW(item(IDC_STATUS), s.c_str());
    update_cost();
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

bool load_file(const fs::path &f) {
    auto d = std::make_shared<Data>();
    if (!lshade::load(d->a, d->p, f)) return false;
    St->data = d;
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
    if (!keys && !St->dragging) {
        KillTimer(St->view, kTick);
        St->ticking = false;
    }
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
        start_ticking();
        return 0;
    case WM_MOUSEMOVE:
        if (St && St->dragging) {
            // Turns the camera itself, so the keys move it the way you look now.
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
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
    St->list = add(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, 12, 68, 548, 556, IDC_LIST);
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
    St->view = CreateWindowExW(WS_EX_CLIENTEDGE, L"HMPLevelView", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_TABSTOP,
                               S(572), S(68), S(596), S(447), St->wnd,
                               (HMENU)(INT_PTR)IDC_VIEW, GetModuleHandleW(nullptr), nullptr);
    add(L"BUTTON", L"\u2190", BS_PUSHBUTTON | WS_TABSTOP, 572, 522, 36, 26, IDC_PREV);
    add(L"BUTTON", L"\u2192", BS_PUSHBUTTON | WS_TABSTOP, 612, 522, 36, 26, IDC_NEXT);
    add(L"BUTTON", L"As made", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 662, 524, 76, 22, IDC_BEFORE);
    add(L"BUTTON", L"As converted", BS_AUTORADIOBUTTON, 742, 524, 100, 22, IDC_AFTER);
    add(L"BUTTON", L"What it loses", BS_AUTORADIOBUTTON, 846, 524, 104, 22, IDC_CHANGES);
    add(L"BUTTON", L"Tint selected", BS_AUTOCHECKBOX | WS_GROUP | WS_TABSTOP, 960, 524, 100, 22, IDC_HILITE);
    add(L"BUTTON", L"Show portals", BS_AUTOCHECKBOX | WS_TABSTOP, 1066, 524, 102, 22, IDC_PORTALS);
    add(L"STATIC", L"", 0, 572, 556, 596, 64, IDC_COST);
    add(L"BUTTON", L"Measure again", BS_PUSHBUTTON | WS_TABSTOP, 12, 636, 120, 26, IDC_REMEASURE);
    add(L"STATIC", L"Shows: how much of what you see is its reflection. Covers: how much of the screen it is. Passes: triangles drawn again per view.",
        0, 140, 640, 740, 20, IDC_ABOUT);
    add(L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, 966, 636, 96, 26, IDC_OKB);
    add(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 1072, 636, 96, 26, IDC_CANCELB);
    CheckRadioButton(St->wnd, IDC_MODE0, IDC_MODE2, IDC_MODE0 + St->picks.mode);
    CheckRadioButton(St->wnd, IDC_BEFORE, IDC_CHANGES, IDC_AFTER);
    CheckDlgButton(St->wnd, IDC_HILITE, BST_CHECKED);

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
    tip(IDC_VIEW, L"Click here, then fly: drag with the mouse to look around, W A S D to move where you look, Q / E to go "
                  L"down and up (Shift faster, Ctrl slower), the mouse wheel to step forward and back.");
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
                go_to_view();
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
        case IDC_PORTALS:
            St->portals = IsDlgButtonChecked(h, IDC_PORTALS) == BST_CHECKED;
            render(false);
            return 0;
        case IDC_HILITE:
            St->highlight = IsDlgButtonChecked(h, IDC_HILITE) == BST_CHECKED;
            render(false);
            return 0;
        case IDC_REMEASURE:
            start_measure();
            return 0;
        case IDC_OKB:
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

bool edit(HWND owner, const std::wstring &title, const std::string &key, Measure measure, Picks &picks) {
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
        vc.style = CS_OWNDC;   // (OpenGL draws on it)
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

    RECT r{0, 0, S(1180), S(674)};
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
