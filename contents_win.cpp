// Halo Map Porter - the Xbox tab's "Weapons & vehicles" window (Windows). See contents_win.hpp.
#include "contents_win.hpp"

#include <commctrl.h>

#include "modelview_win.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace contentsui {
using namespace porter;

namespace {

enum {
    IDC_SUMMARY = 300, IDC_STATUS, IDC_LIST, IDC_REPL_LBL, IDC_REPL, IDC_KEEPALL, IDC_TOTAL, IDC_FILE, IDC_REMEASURE,
    IDC_OKB, IDC_CANCELB, IDC_VIEW, IDC_ABOUT,
};
#define WM_CU_LOG (WM_APP + 40)
#define WM_CU_DONE (WM_APP + 41)

const COLORREF kOk = RGB(16, 124, 16), kBad = RGB(196, 43, 28), kGrey = RGB(96, 96, 96);

struct State {
    HWND wnd = nullptr, list = nullptr, view = nullptr;
    int dpi = 96;
    HFONT font = nullptr, bold = nullptr;
    bool done = false, ok = false;
    std::shared_ptr<ContentAnalysis> an;
    std::map<std::string, std::string> picks;
    std::string key;
    Analyze analyze;
    std::vector<int> row_item;             // list row -> item index
    std::vector<std::string> combo_tags;   // "Replace with" entries -> tag ("\x01" = keep)
    COLORREF total_color = 0;
};
State *St = nullptr;

// Measurements, kept while the app runs and on disk (one file per map); one measurement at a time.
std::mutex g_lock;
std::map<std::string, std::shared_ptr<ContentAnalysis>> g_cache;
std::atomic<bool> g_measuring{false};
std::string g_measuring_key;
std::atomic<HWND> g_target{nullptr};
fs::path g_disk;

std::wstring W(const std::string &s);
uint64_t fnv(const std::string &s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}
std::string hex16(uint64_t v) {
    char b[24];
    snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
    return b;
}
// <map stem>_<hash of the map's path>_ : the prefix of that map's file; then a hash of the whole key.
std::wstring disk_prefix(const std::string &key) {
    std::string path = key.substr(0, key.find('|'));
    std::wstring stem = fs::path(W(path)).stem().wstring();
    return stem + L"_" + W(hex16(fnv(path))) + L"_";
}
fs::path disk_file(const std::string &key) { return g_disk / (disk_prefix(key) + W(hex16(fnv(key))) + L".txt"); }

void disk_forget(const std::string &key) {   // every file for this map
    if (g_disk.empty()) return;
    std::error_code ec;
    std::wstring pre = disk_prefix(key);
    for (auto it = fs::directory_iterator(g_disk, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->path().filename().wstring().rfind(pre, 0) == 0) fs::remove(it->path(), ec);
}
void disk_save(const std::string &key, const ContentAnalysis &a) {
    if (g_disk.empty()) return;
    disk_forget(key);
    save_content_analysis(a, disk_file(key));
}
// From memory, else from disk (then kept in memory). Call with g_lock held.
std::shared_ptr<ContentAnalysis> cached_locked(const std::string &key) {
    auto it = g_cache.find(key);
    if (it != g_cache.end()) return it->second;
    if (g_disk.empty()) return nullptr;
    auto a = std::make_shared<ContentAnalysis>();
    if (!load_content_analysis(*a, disk_file(key))) return nullptr;
    g_cache[key] = a;
    return a;
}

int S(int v) { return MulDiv(v, St->dpi, 96); }
std::wstring W(const std::string &s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
HWND item(int id) { return GetDlgItem(St->wnd, id); }
std::wstring mb(double v, bool plus = false) {
    wchar_t b[32];
    if (v > -0.005 && v < 0.005) return v == 0 ? L"0" : (plus ? L"+<0.01" : L"<0.01");
    swprintf(b, 32, plus ? L"+%.2f" : L"%.2f", v);
    return b;
}

const ContentItem *find_item(const std::string &tag) {
    if (!St->an) return nullptr;
    for (auto &it : St->an->items) if (it.tag == tag) return &it;
    return nullptr;
}
std::wstring item_label(const ContentItem &it) { return (it.in_map ? L"" : L"Xbox ") + W(it.name); }
// The map's vehicles that are pieces of `tag` (its scripts make them and attach them to it).
std::vector<const ContentItem *> pieces_of(const std::string &tag) {
    std::vector<const ContentItem *> v;
    if (St->an)
        for (auto &it : St->an->items)
            if (it.in_map && it.piece_of == tag) v.push_back(&it);
    return v;
}
std::wstring thousands(long long n) {
    std::wstring s = std::to_wstring(n);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, L",");
    return s;
}
std::wstring pick_text(const ContentItem &it) {
    if (!it.in_map) return L"";
    auto p = St->picks.find(it.tag);
    if (p == St->picks.end()) return L"Keep";
    if (p->second.empty()) return L"None (removed)";
    const ContentItem *t = find_item(p->second);
    return t ? item_label(*t) : W(p->second);
}

void set_row_pick(int row) {
    const ContentItem &it = St->an->items[St->row_item[row]];
    std::wstring s = pick_text(it);
    ListView_SetItemText(St->list, row, 4, (LPWSTR)s.c_str());
}

void update_totals() {
    if (!St->an) return;
    const ContentAnalysis &a = *St->an;
    ContentEstimate e = estimate_contents(a, St->picks);
    // Without the optimize rebuild the figure is exact; what that rebuild saves is an estimate (within ~0.1 MB).
    std::wstring t = L"With your picks: " + mb(e.memory_mb) + L" MB of the " + mb(a.limit_mb) + L" MB";
    bool opt = a.optimize_saves_mb > 0 && e.memory_mb > a.limit_mb;
    if (opt) t += L" (about " + mb(e.memory_optimized_mb) + L" MB after the optimize rebuild)";
    if (e.memory_mb <= a.limit_mb) {
        t += L" - fits";
        St->total_color = kOk;
    } else if (opt && e.memory_optimized_mb <= a.limit_mb - 0.2) {
        t += L" - should fit";
        St->total_color = kOk;
    } else if (opt && e.memory_optimized_mb <= a.limit_mb) {
        t += L" - right at the limit, may not fit: free a little more to be safe";
        St->total_color = RGB(170, 92, 0);
    } else {
        t += L" - still about " + mb((opt ? e.memory_optimized_mb : e.memory_mb) - a.limit_mb) + L" MB over";
        St->total_color = kBad;
    }
    SetWindowTextW(item(IDC_TOTAL), t.c_str());
    std::wstring f = L"Map file: " + mb(e.file_mb) + L" MB of " + mb(a.file_limit_mb) + L" MB";
    if (e.file_mb > a.file_limit_mb) f += L" - over the size limit (shrinking textures can help with this one)";
    SetWindowTextW(item(IDC_FILE), f.c_str());
    InvalidateRect(item(IDC_TOTAL), nullptr, TRUE);
}

std::vector<int> selected_rows() {
    std::vector<int> rows;
    int r = -1;
    while ((r = ListView_GetNextItem(St->list, r, LVNI_SELECTED)) >= 0) rows.push_back(r);
    return rows;
}

// The "Replace with" choices for the selected rows (all weapons or all vehicles from the map).
void update_combo() {
    HWND c = item(IDC_REPL);
    SendMessageW(c, CB_RESETCONTENT, 0, 0);
    St->combo_tags.clear();
    std::vector<int> rows = selected_rows();
    int kind = -1;   // 0 weapons, 1 vehicles, 2 mixed / none usable
    std::set<std::string> sel;
    bool only_pieces = true;   // pieces of a vehicle: kept or removed, not swapped for a working vehicle
    for (int r : rows) {
        const ContentItem &it = St->an->items[St->row_item[r]];
        if (!it.in_map) continue;
        int k = it.vehicle ? 1 : 0;
        kind = kind < 0 ? k : (kind == k ? k : 2);
        sel.insert(it.tag);
        only_pieces = only_pieces && !it.piece_of.empty();
    }
    bool usable = kind == 0 || kind == 1;
    EnableWindow(c, usable);
    EnableWindow(item(IDC_REPL_LBL), usable);
    if (!usable) return;
    auto add = [&](const std::wstring &text, const std::string &tag) {
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)text.c_str());
        St->combo_tags.push_back(tag);
    };
    add(L"Keep", "\x01");
    add(L"None (remove it)", "");
    for (auto &it : St->an->items) {
        if (only_pieces) break;
        if (it.vehicle != (kind == 1) || sel.count(it.tag) || !it.piece_of.empty()) continue;
        if (it.in_map && St->picks.count(it.tag)) continue;   // one that's being replaced itself
        add(it.in_map ? W(it.name) : L"Xbox " + W(it.name) + L" (adds " + mb(it.memory_mb) + L" MB)", it.tag);
    }
    // Show the current pick when all selected rows share it.
    std::string common = "\x02";
    for (auto &t : sel) {
        auto p = St->picks.find(t);
        std::string v = p == St->picks.end() ? "\x01" : p->second;
        if (common == "\x02") common = v;
        else if (common != v) { common = "\x03"; break; }
    }
    for (size_t i = 0; i < St->combo_tags.size(); i++)
        if (St->combo_tags[i] == common) SendMessageW(c, CB_SETCURSEL, i, 0);
}

void apply_combo() {
    int k = (int)SendMessageW(item(IDC_REPL), CB_GETCURSEL, 0, 0);
    if (k < 0 || k >= (int)St->combo_tags.size()) return;
    std::string to = St->combo_tags[k];
    for (int r : selected_rows()) {
        const ContentItem &it = St->an->items[St->row_item[r]];
        if (!it.in_map) continue;
        if (to == "\x01") St->picks.erase(it.tag);
        else St->picks[it.tag] = to;
        // Its pieces go with it (and come back with it): the scripts would attach them to whatever replaces it.
        for (const ContentItem *p : pieces_of(it.tag)) {
            if (to == "\x01") St->picks.erase(p->tag);
            else St->picks[p->tag] = "";
        }
        // Others that were replaced with this one follow it.
        for (auto &[from, t] : St->picks)
            if (t == it.tag && from != it.tag) t = to == "\x01" ? it.tag : to;
    }
    for (auto it = St->picks.begin(); it != St->picks.end();)   // "replaced with itself" = keep
        it = it->first == it->second ? St->picks.erase(it) : std::next(it);
    int n = ListView_GetItemCount(St->list);
    for (int r = 0; r < n; r++) set_row_pick(r);
    update_totals();
}

// The model of the first selected row, and what it is.
void update_preview() {
    if (!St->view) return;
    std::vector<int> rows = St->an ? selected_rows() : std::vector<int>();
    if (rows.empty()) {
        modelview::show(St->view, nullptr, St->an ? L"Click a weapon or vehicle to see it here." : L"");
        SetWindowTextW(item(IDC_ABOUT), L"");
        return;
    }
    const ContentAnalysis &a = *St->an;
    const ContentItem &it = a.items[St->row_item[rows[0]]];
    const ModelPreview *m = it.preview >= 0 && it.preview < (int)a.previews.size() ? &a.previews[it.preview] : nullptr;
    modelview::show(St->view, m, L"No model to show for this one.");
    std::wstring t = item_label(it) + L"\r\n";
    if (!it.vehicle) t += L"Weapon";
    else if (it.seats > 0) t += L"Vehicle, " + std::to_wstring(it.seats) + (it.seats == 1 ? L" seat" : L" seats") +
                               (it.seat_roles.empty() ? L"" : L": " + W(it.seat_roles));
    else if (it.seats == 0) t += L"Vehicle with no seats: nobody can drive it or ride in it";
    else t += L"Vehicle";
    if (m) t += L"\r\n" + thousands(m->triangles()) + L" triangles";
    if (!it.in_map) t += L"\r\nAn Xbox one the map doesn't have: picking it adds what it costs.";
    if (!it.piece_of.empty()) {
        std::wstring parent = W(fs::path(it.piece_of).stem().u8string());
        if (const ContentItem *p = find_item(it.piece_of)) parent = W(p->name);
        t += L"\r\nA piece of " + parent + L": the map's scripts make it (usually when " + parent +
             L" is destroyed) and attach it. Removing " + parent + L" removes its pieces too.";
    } else if (it.in_map) {
        auto ps = pieces_of(it.tag);
        if (!ps.empty())
            t += L"\r\nThe map's scripts make " + std::to_wstring(ps.size()) + (ps.size() == 1 ? L" piece" : L" pieces") +
                 L" of it (listed below it), removed along with it.";
    }
    SetWindowTextW(item(IDC_ABOUT), t.c_str());
}

void add_group(int id, const wchar_t *text) {
    LVGROUP g{};
    g.cbSize = sizeof(g);
    g.mask = LVGF_HEADER | LVGF_GROUPID;
    g.pszHeader = (LPWSTR)text;
    g.iGroupId = id;
    ListView_InsertGroup(St->list, -1, &g);
}

void fill_list() {
    HWND l = St->list;
    SendMessageW(l, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(l);
    ListView_RemoveAllGroups(l);
    St->row_item.clear();
    const ContentAnalysis &a = *St->an;
    ListView_EnableGroupView(l, TRUE);
    add_group(1, L"Weapons in the map");
    add_group(2, L"Vehicles in the map");
    add_group(3, L"Xbox weapons and vehicles you can replace them with (what each would add)");
    // A vehicle's pieces right below it.
    std::vector<size_t> order;
    std::vector<char> placed(a.items.size(), 0);
    auto has_parent = [&](const ContentItem &it) {
        if (it.piece_of.empty()) return false;
        for (auto &x : a.items)
            if (x.tag == it.piece_of && x.in_map) return true;
        return false;
    };
    for (size_t i = 0; i < a.items.size(); i++) {
        if (placed[i] || has_parent(a.items[i])) continue;
        order.push_back(i);
        placed[i] = 1;
        for (size_t k = 0; k < a.items.size(); k++)
            if (!placed[k] && a.items[k].in_map && a.items[k].piece_of == a.items[i].tag && a.items[i].in_map) {
                order.push_back(k);
                placed[k] = 1;
            }
    }
    for (size_t i : order) {
        const ContentItem &it = a.items[i];
        LVITEMW lv{};
        lv.mask = LVIF_TEXT | LVIF_GROUPID | LVIF_PARAM;
        lv.iItem = (int)St->row_item.size();
        std::wstring name = (has_parent(it) ? L"    \u2514 " : L"") + W(it.name);
        lv.pszText = (LPWSTR)name.c_str();
        lv.iGroupId = !it.in_map ? 3 : it.vehicle ? 2 : 1;
        lv.lParam = (LPARAM)i;
        int row = ListView_InsertItem(l, &lv);
        St->row_item.push_back((int)i);
        std::wstring used = W(it.used_by), mem = mb(it.memory_mb, !it.in_map), file = mb(it.file_mb, !it.in_map);
        ListView_SetItemText(l, row, 1, (LPWSTR)used.c_str());
        ListView_SetItemText(l, row, 2, (LPWSTR)mem.c_str());
        ListView_SetItemText(l, row, 3, (LPWSTR)file.c_str());
        set_row_pick(row);
    }
    SendMessageW(l, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(l, nullptr, TRUE);

    std::wstring s = L"It needs " + mb(a.used_mb) + L" MB of the Xbox's " + mb(a.limit_mb) + L" MB of tag memory";
    double need = a.used_mb - a.optimize_saves_mb;
    if (a.optimize_saves_mb > 0) s += L" (about " + mb(need) + L" MB after the optimize rebuild the app does when a map is over)";
    if (need > a.limit_mb) s += L", so about " + mb(need - a.limit_mb) + L" MB has to go.";
    else s += L", so it fits as it is.";
    s += L"\r\nMemory / Map size: what replacing a weapon or vehicle with none frees, in MB. Parts several share "
         L"(one model for a whole family) only free up when all of them go: pick them together.";
    SetWindowTextW(item(IDC_SUMMARY), s.c_str());
    SetWindowTextW(item(IDC_STATUS), L"Select one or more rows (Ctrl/Shift for several), then choose what replaces them.");
    update_combo();
    update_totals();
    update_preview();
}

void set_busy(bool busy) {
    EnableWindow(item(IDC_REMEASURE), !busy);
    EnableWindow(item(IDC_KEEPALL), !busy);
    EnableWindow(St->list, !busy);
    if (busy) {
        EnableWindow(item(IDC_REPL), FALSE);
        SetWindowTextW(item(IDC_SUMMARY), L"Measuring the map: the app prepares it for the Xbox and builds a test copy, then "
                                          L"reads back what each part takes. This takes about as long as converting it.");
        SetWindowTextW(item(IDC_TOTAL), L"");
        SetWindowTextW(item(IDC_FILE), L"");
    }
}

void start_measure() {
    if (g_measuring.exchange(true)) {   // another window's measurement is still running: wait for it
        set_busy(true);
        return;
    }
    {
        std::lock_guard<std::mutex> g(g_lock);
        g_measuring_key = St->key;
    }
    set_busy(true);
    Analyze fn = St->analyze;
    std::string key = St->key;
    std::thread([fn, key] {
        auto a = std::make_shared<ContentAnalysis>();
        Log log = [](const std::string &s) {
            HWND w = g_target.load();
            if (w) PostMessageW(w, WM_CU_LOG, 0, (LPARAM) new std::wstring(W(s)));
        };
        bool ok = fn(*a, log);
        if (!ok && a->error.empty()) a->error = "The map couldn't be measured.";
        {
            std::lock_guard<std::mutex> g(g_lock);
            if (ok) { g_cache[key] = a; disk_save(key, *a); }
            g_measuring_key.clear();
        }
        g_measuring = false;
        HWND w = g_target.load();
        if (w) PostMessageW(w, WM_CU_DONE, 0, (LPARAM) new std::pair<std::string, std::shared_ptr<ContentAnalysis>>(key, a));
    }).detach();
}

LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CU_LOG: {
        std::unique_ptr<std::wstring> s((std::wstring *)lp);
        if (St && St->wnd == h) {
            std::wstring t = *s;
            while (!t.empty() && (t[0] == L' ')) t.erase(0, 1);
            SetWindowTextW(item(IDC_STATUS), t.c_str());
        }
        return 0;
    }
    case WM_CU_DONE: {
        std::unique_ptr<std::pair<std::string, std::shared_ptr<ContentAnalysis>>> r(
            (std::pair<std::string, std::shared_ptr<ContentAnalysis>> *)lp);
        if (!St || St->wnd != h) return 0;
        if (r->first != St->key) {   // an earlier window's measurement finished: now ours
            std::shared_ptr<ContentAnalysis> cached;
            {
                std::lock_guard<std::mutex> g(g_lock);
                cached = cached_locked(St->key);
            }
            if (cached) { St->an = cached; set_busy(false); fill_list(); }
            else start_measure();
            return 0;
        }
        set_busy(false);
        if (!r->second->error.empty()) {
            SetWindowTextW(item(IDC_SUMMARY), (L"Couldn't measure the map:\r\n" + W(r->second->error)).c_str());
            SetWindowTextW(item(IDC_STATUS), L"");
            return 0;
        }
        St->an = r->second;
        // Picks for weapons the map no longer has are dropped.
        for (auto it = St->picks.begin(); it != St->picks.end();)
            it = find_item(it->first) ? std::next(it) : St->picks.erase(it);
        fill_list();
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        if ((HWND)lp == item(IDC_TOTAL)) SetTextColor(dc, St->total_color);
        else if ((HWND)lp == item(IDC_STATUS) || (HWND)lp == item(IDC_FILE)) SetTextColor(dc, kGrey);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_NOTIFY: {
        NMHDR *n = (NMHDR *)lp;
        if (n->idFrom == IDC_LIST && n->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *v = (NMLISTVIEW *)lp;
            if (v->uChanged & LVIF_STATE) {
                update_combo();
                update_preview();
            }
        }
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_REPL:
            if (HIWORD(wp) == CBN_SELCHANGE) apply_combo();
            return 0;
        case IDC_KEEPALL:
            St->picks.clear();
            if (St->an) { int n = ListView_GetItemCount(St->list); for (int r = 0; r < n; r++) set_row_pick(r); update_combo(); update_totals(); }
            return 0;
        case IDC_REMEASURE:
            {
                std::lock_guard<std::mutex> g(g_lock);
                g_cache.erase(St->key);
                disk_forget(St->key);
            }
            modelview::show(St->view, nullptr);   // (its model goes with the measurement)
            SetWindowTextW(item(IDC_ABOUT), L"");
            St->an.reset();
            ListView_DeleteAllItems(St->list);
            start_measure();
            return 0;
        case IDC_OKB:
        case IDOK:
            St->ok = true;
            St->done = true;
            return 0;
        case IDC_CANCELB:
        case IDCANCEL:
            St->done = true;
            return 0;
        }
        break;
    case WM_CLOSE:
        St->done = true;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

HWND add(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), St->wnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)St->font, TRUE);
    return c;
}

void create_controls() {
    add(L"STATIC", L"", 0, 16, 12, 1136, 66, IDC_SUMMARY);
    add(L"STATIC", L"", SS_ENDELLIPSIS, 16, 80, 1136, 20, IDC_STATUS);
    // The selected one's model and what it is.
    St->view = modelview::create(St->wnd, S(856), S(102), S(296), S(256), IDC_VIEW);
    SendMessageW(St->view, WM_SETFONT, (WPARAM)St->font, TRUE);
    add(L"STATIC", L"", SS_NOPREFIX, 856, 364, 296, 120, IDC_ABOUT);
    St->list = add(WC_LISTVIEWW, L"", WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SHOWSELALWAYS, 16, 102, 828, 382, IDC_LIST);
    ListView_SetExtendedListViewStyle(St->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    struct { const wchar_t *t; int w; int fmt; } cols[] = {{L"Weapon / vehicle", 190, LVCFMT_LEFT},
                                                          {L"Where it's used", 270, LVCFMT_LEFT},
                                                          {L"Memory", 70, LVCFMT_RIGHT},
                                                          {L"Map size", 74, LVCFMT_RIGHT},
                                                          {L"Replace with", 196, LVCFMT_LEFT}};
    for (int i = 0; i < 5; i++) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.pszText = (LPWSTR)cols[i].t;
        c.cx = S(cols[i].w);
        c.fmt = cols[i].fmt;
        ListView_InsertColumn(St->list, i, &c);
    }
    add(L"STATIC", L"Replace the selected ones with:", 0, 16, 499, 190, 20, IDC_REPL_LBL);
    add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 208, 496, 300, 300, IDC_REPL);
    add(L"BUTTON", L"Keep everything", WS_TABSTOP, 520, 495, 140, 28, IDC_KEEPALL);
    add(L"BUTTON", L"Measure again", WS_TABSTOP, 704, 495, 140, 28, IDC_REMEASURE);
    add(L"STATIC", L"", 0, 16, 534, 1136, 20, IDC_TOTAL);
    SendMessageW(item(IDC_TOTAL), WM_SETFONT, (WPARAM)St->bold, TRUE);
    add(L"STATIC", L"", 0, 16, 556, 1136, 20, IDC_FILE);
    add(L"STATIC", L"Your picks are saved for this map and used every time you convert it for the Xbox.", 0, 16, 590, 520, 20, 0);
    add(L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, 908, 584, 116, 30, IDC_OKB);
    add(L"BUTTON", L"Cancel", WS_TABSTOP, 1036, 584, 116, 30, IDC_CANCELB);
}

} // namespace

void set_cache_folder(const std::filesystem::path &dir) {
    std::lock_guard<std::mutex> g(g_lock);
    g_disk = dir;
}
void remember(const std::string &key, std::shared_ptr<ContentAnalysis> a) {
    if (!a) return;
    std::lock_guard<std::mutex> g(g_lock);
    g_cache[key] = a;
    disk_save(key, *a);
}

bool edit_picks(HWND owner, const std::wstring &title, const std::string &key, Analyze analyze,
                std::map<std::string, std::string> &picks) {
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!registered) {
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&icc);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpfnWndProc = proc;
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wc.lpszClassName = L"HMPContents";
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        RegisterClassExW(&wc);
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
    st.analyze = std::move(analyze);
    st.picks = picks;

    RECT r{0, 0, S(1168), S(628)};
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
    st.wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"HMPContents", title.c_str(), style, x, y, w, hgt, owner, nullptr, inst, nullptr);
    create_controls();
    g_target = st.wnd;
    {
        std::shared_ptr<ContentAnalysis> cached;
        {
            std::lock_guard<std::mutex> g(g_lock);
            cached = cached_locked(key);
        }
        if (cached) { st.an = cached; fill_list(); }
        else start_measure();
    }
    EnableWindow(owner, FALSE);
    ShowWindow(st.wnd, SW_SHOW);
    SetFocus(st.list);

    MSG m;
    while (!st.done) {
        BOOL got = GetMessageW(&m, nullptr, 0, 0);
        if (got <= 0) { PostQuitMessage(got == 0 ? (int)m.wParam : 0); break; }
        if (!IsDialogMessageW(st.wnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    g_target = nullptr;
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    DestroyWindow(st.wnd);
    DeleteObject(st.font);
    DeleteObject(st.bold);
    St = nullptr;
    if (st.ok) picks = st.picks;
    return st.ok;
}

} // namespace contentsui
