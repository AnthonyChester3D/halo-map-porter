// Halo Map Porter - the "LODs" window (see lodview_win.hpp)
#include "lodview_win.hpp"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core.hpp"
#include "lod.hpp"
#include "modelview_win.hpp"

namespace lodui {
using namespace porter;

namespace {

enum { IDC_INTRO = 400, IDC_FILTER_LBL, IDC_FILTER, IDC_LIST, IDC_VIEW, IDC_LEVEL0, IDC_LEVEL4 = IDC_LEVEL0 + 4, IDC_WIRE,
       IDC_ASMADE, IDC_ABOUT, IDC_LIBRARY, IDC_CLOSE, IDC_SHOW_MODEL, IDC_SHOW_COLL, IDC_SHOW_COLL_MADE, IDC_GROUP,
       IDC_LODS_LBL, IDC_LODS, IDC_MED_LBL, IDC_MED, IDC_LOW_LBL, IDC_LOW, IDC_TRIS_LBL, IDC_TOP_LBL, IDC_TOP, IDC_TOPN,
       IDC_TOPN_LBL, IDC_STATUS, IDC_AGAIN };

const UINT WM_UPDATED = WM_APP + 41;   // (the update's thread: done; lParam a Result*)

struct State {
    HWND wnd = nullptr, list = nullptr, view = nullptr, tips = nullptr;
    fs::path file;                // the LodView shown
    std::function<Result()> update;   // "Update the map" (runs on a thread of its own)
    bool updating = false;
    int dpi = 96;
    HFONT font = nullptr, bold = nullptr;
    bool done = false, again = false;
    LodView v;
    fs::path library;
    int model = -1;               // the model shown
    std::vector<int> levels;      // its drawings, most detailed first (each one's first level, tag order)
    int level = 0;                // the drawing shown (index in `levels`)
    int show = 0;                 // 0 the model, 1 its collision as converted, 2 its collision as the map made it
    std::vector<int> rows;        // the list's models (index in v.models), as shown: filtered and sorted
    int filter = 0;               // 0 every model, 1 those made without lower-detail versions, 2 those this conversion changed
    int sort_col = 3;             // sorted by this column (3: most detailed triangles), heaviest first
    bool sort_up = false;
    bool loading = false;         // filling in the choice controls (not a change to save)
    lod::Choice saved;            // the shown model's choice, as saved
};
State *St = nullptr;

int S(int v) { return MulDiv(v, St->dpi, 96); }
HWND item(int id) { return GetDlgItem(St->wnd, id); }
std::wstring W(const std::string &s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
std::wstring thousands(long long n) {
    std::wstring s = std::to_wstring(n);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, L",");
    return s;
}
const wchar_t *kind_text(char k) {
    return k == 'v' ? L"Vehicle" : k == 'w' ? L"Weapon or item" : k == 'b' ? L"Character" : k == 'f' ? L"First person" : L"Scenery";
}
bool lod_changed(const LodView::Model &m) { return !m.change.empty() || m.trimmed_from > 0; }   // (not only its collision)
std::wstring change_short(const LodView::Model &m) {   // (for the list)
    std::wstring t = m.change.empty() ? L"" : m.change == "made" ? L"Made" : m.change == "library" ? L"From the LOD library"
                     : m.change == "stock" ? L"Distances (stock)" : m.change == "trimmed" ? L"" : L"Switch distances";
    if (m.undone) t += L", taken out";
    if (m.trimmed_from > 0) t = t.empty() ? L"Trimmed" : L"Trimmed; " + t;
    if (!m.collision.empty()) t = t.empty() ? L"Collision" : t + L"; collision";
    return t.empty() ? L"As made" : t;
}
std::wstring change_text(const LodView::Model &m) {
    std::wstring t = m.change.empty()        ? L""
                     : m.change == "made"    ? L"Lower-detail versions made"
                     : m.change == "library" ? L"Lower-detail versions (from the LOD library)"
                     : m.change == "stock"   ? L"Switch distances (stock model)"
                     : m.change == "trimmed" ? L""
                                             : L"Switch distances for its own";
    if (m.undone) t += L", taken out again";
    if (m.trimmed_from > 0) {
        std::wstring trim = L"Most detailed version trimmed for the Xbox (" + thousands(m.trimmed_from) + L" triangles as made)";
        t = t.empty() ? trim : trim + L"; " + (wchar_t)towlower(t[0]) + t.substr(1);
    }
    return t;
}
const wchar_t *level_name(int tag_level) {
    static const wchar_t *n[5] = {L"Most detailed", L"High", L"Medium", L"Low", L"Lowest"};
    return n[std::clamp(tag_level, 0, 4)];
}
int triangles(int preview) { return preview >= 0 && preview < (int)St->v.previews.size() ? St->v.previews[preview].triangles() : 0; }

// Where the game switches to a level: the model's on-screen size (pixels across, on the Xbox's 480 lines with Halo's
// field of view) is 457 x its diameter / its distance, so the switch at `px` pixels is that far away. 1 unit = 10 ft.
std::wstring distance(const LodView::Model &m, double px) {
    if (px <= 0 || m.radius <= 0) return L"";
    double units = 457.0 * 2 * m.radius / px, metres = units * 3.048, feet = units * 10;
    wchar_t b[64];
    swprintf(b, 64, L"about %.0f m (%.0f ft)", metres, feet);
    return b;
}

lod::Settings lib() {
    lod::Settings s;
    s.library = St->library;
    return s;
}
bool is_stock(const LodView::Model &m) { return m.stock || m.change == "stock"; }   // (Bungie's: only its levels of detail)
std::wstring kind_label(const LodView::Model &m) {   // (for the list)
    if (!is_stock(m)) return kind_text(m.kind);
    std::wstring k = kind_text(m.kind);
    k[0] = (wchar_t)towlower(k[0]);
    return L"Bungie's " + k;
}

// A model's choice, short (the list's "Your choice"; empty: automatic).
std::wstring choice_short(const LodView::Model &m) {
    if (St->library.empty()) return L"";
    lod::Choice c = lod::choice(lib(), m.tag, m.name);
    if (c.skip && !c.chosen) return L"As made (skip.txt)";
    std::wstring t;
    if (c.lods == 'n') t = L"No LODs";
    else if (c.lods == 'm')
        t = c.medium > 0 || c.low > 0 ? L"LODs " + (c.medium > 0 ? thousands(c.medium) : L"auto") + L" / " +
                                            (c.low > 0 ? thousands(c.low) : L"auto")
                                      : L"LODs";
    if (c.top == 'k') t += (t.empty() ? L"" : L", ") + std::wstring(L"top as made");
    else if (c.top == 't') t += std::wstring(t.empty() ? L"" : L", ") + L"top " + thousands(c.top_triangles);
    return t;
}

void status(const std::wstring &t) { SetWindowTextW(item(IDC_STATUS), t.c_str()); }

int number(int id) {
    wchar_t b[32] = {};
    GetWindowTextW(item(id), b, 32);
    return std::max(0, _wtoi(b));
}

// The choice as the controls have it.
lod::Choice read_choice() {
    lod::Choice c;
    int li = (int)SendMessageW(item(IDC_LODS), CB_GETCURSEL, 0, 0), ti = (int)SendMessageW(item(IDC_TOP), CB_GETCURSEL, 0, 0);
    c.lods = li == 1 ? 'm' : li == 2 ? 'n' : 'a';
    if (c.lods == 'm') {
        c.medium = number(IDC_MED);
        c.low = number(IDC_LOW);
    }
    c.top = ti == 1 ? 'k' : ti == 2 ? 't' : 'a';
    if (c.top == 't') c.top_triangles = number(IDC_TOPN);
    if (c.top == 't' && c.top_triangles <= 0) c.top = St->saved.top == 't' ? 'a' : St->saved.top;   // (no number yet)
    c.skip = St->saved.skip;
    return c;
}
bool same(const lod::Choice &a, const lod::Choice &b) {
    return a.lods == b.lods && a.top == b.top && (a.lods != 'm' || (a.medium == b.medium && a.low == b.low)) &&
           (a.top != 't' || a.top_triangles == b.top_triangles);
}

// Which choice controls apply to the model shown.
void enable_choice() {
    const LodView::Model *m = St->model >= 0 ? &St->v.models[St->model] : nullptr;
    bool any = m && !St->library.empty(), fp = m && m->kind == 'f', stock = m && is_stock(*m);
    int li = (int)SendMessageW(item(IDC_LODS), CB_GETCURSEL, 0, 0), ti = (int)SendMessageW(item(IDC_TOP), CB_GETCURSEL, 0, 0);
    bool lods = any && !fp, make = lods && li == 1 && !m->had_lods, top = any && !stock;
    for (int id : {IDC_LODS_LBL, IDC_LODS}) EnableWindow(item(id), lods);
    for (int id : {IDC_MED_LBL, IDC_MED, IDC_LOW_LBL, IDC_LOW, IDC_TRIS_LBL}) EnableWindow(item(id), make);
    for (int id : {IDC_TOP_LBL, IDC_TOP}) EnableWindow(item(id), top);
    EnableWindow(item(IDC_TOPN), top && ti == 2);
    EnableWindow(item(IDC_TOPN_LBL), top);
}

// The shown model's choice into the controls.
void load_choice() {
    St->loading = true;
    const LodView::Model *m = St->model >= 0 ? &St->v.models[St->model] : nullptr;
    lod::Choice c = m && !St->library.empty() ? lod::choice(lib(), m->tag, m->name) : lod::Choice();
    St->saved = c;
    SendMessageW(item(IDC_LODS), CB_SETCURSEL, c.lods == 'm' ? 1 : c.lods == 'n' ? 2 : 0, 0);
    SendMessageW(item(IDC_TOP), CB_SETCURSEL, c.top == 'k' ? 1 : c.top == 't' ? 2 : 0, 0);
    SetWindowTextW(item(IDC_MED), c.lods == 'm' && c.medium > 0 ? std::to_wstring(c.medium).c_str() : L"");
    SetWindowTextW(item(IDC_LOW), c.lods == 'm' && c.low > 0 ? std::to_wstring(c.low).c_str() : L"");
    SetWindowTextW(item(IDC_TOPN), c.top == 't' ? std::to_wstring(c.top_triangles).c_str() : L"");
    std::wstring g = m ? L"What every conversion does with " + W(m->name) + L" from now on (on any map that uses it)"
                       : L"What every conversion does with the model, from now on";
    SetWindowTextW(item(IDC_GROUP), g.c_str());
    std::wstring tl = L"triangles";
    if (m && m->density > 0) tl += L" (1.5x what Bungie's models of its kind and size have: " + thousands(m->density) + L")";
    SetWindowTextW(item(IDC_TOPN_LBL), tl.c_str());
    St->loading = false;
    enable_choice();
}

// The list row showing model `i` (-1: not shown).
int row_of(int i) {
    for (size_t r = 0; r < St->rows.size(); r++)
        if (St->rows[r] == i) return (int)r;
    return -1;
}

// The controls' choice, saved when it changed (choices.txt in the LOD library).
void save_choice() {
    if (St->loading || St->model < 0 || St->library.empty()) return;
    const LodView::Model &m = St->v.models[St->model];
    lod::Choice c = read_choice();
    if (same(c, St->saved)) return;
    if (!lod::set_choice(lib(), m.tag, c, m.name + " (" + St->v.map_name + ")")) {
        status(L"Couldn't write choices.txt in the LOD library folder.");
        return;
    }
    St->saved = lod::choice(lib(), m.tag, m.name);
    if (int r = row_of(St->model); r >= 0) {
        std::wstring t = choice_short(m);
        ListView_SetItemText(St->list, r, 6, (LPWSTR)t.c_str());
    }
    std::wstring t = L"Saved for " + W(m.name) + L". \"Update the map\" puts it in the map (a few seconds).";
    int top = triangles(m.level[0]);
    if (m.trimmed_from > 0) top = m.trimmed_from;
    if (c.lods == 'm' && m.had_lods) t = L"Saved, but " + W(m.name) + L" has lower-detail versions of its own: the app keeps them.";
    else if (c.lods == 'm' && c.medium > 0 && top > 0 && c.medium >= top)
        t += L" Medium has to be fewer triangles than the most detailed version (" + thousands(top) + L") to be made.";
    else if (c.lods == 'm' && c.low > 0 && c.medium > 0 && c.low >= c.medium)
        t += L" Low has to be fewer triangles than medium to be made.";
    status(t);
}

void show_level(int k) {
    for (int i = 0; i < 3; i++) SendMessageW(item(IDC_SHOW_MODEL + i), BM_SETCHECK, i == St->show ? BST_CHECKED : BST_UNCHECKED, 0);
    for (int i = 0; i < 5; i++) EnableWindow(item(IDC_LEVEL0 + i), St->show == 0);
    if (St->model >= 0 && St->show > 0) {   // its collision, as converted or as made
        const LodView::Model &m = St->v.models[St->model];
        int p = St->show == 1 ? m.coll_now : m.coll_made;
        EnableWindow(item(IDC_ASMADE), FALSE);
        modelview::show(St->view, p >= 0 ? &St->v.previews[p] : nullptr, L"No drawing of its collision.", true);
        return;
    }
    if (St->model < 0 || k < 0 || k >= (int)St->levels.size()) {
        modelview::show(St->view, nullptr, L"Click a model to see it here.");
        EnableWindow(item(IDC_ASMADE), FALSE);
        return;
    }
    St->level = k;
    const LodView::Model &m = St->v.models[St->model];
    int p = m.level[St->levels[k]];
    // A trimmed model's most detailed level can be shown as the map made it, to compare.
    bool can = k == 0 && m.as_made >= 0;
    EnableWindow(item(IDC_ASMADE), can);
    if (can && SendMessageW(item(IDC_ASMADE), BM_GETCHECK, 0, 0) == BST_CHECKED) p = m.as_made;
    modelview::show(St->view, p >= 0 ? &St->v.previews[p] : nullptr, L"No drawing of this level.", true);
    for (int i = 0; i < 5; i++) SendMessageW(item(IDC_LEVEL0 + i), BM_SETCHECK, i == k ? BST_CHECKED : BST_UNCHECKED, 0);
}

void select_model(int i) {
    St->model = i;
    St->levels.clear();
    for (int i2 = 0; i2 < 5; i2++) ShowWindow(item(IDC_LEVEL0 + i2), SW_HIDE);
    if (i < 0 || i >= (int)St->v.models.size()) {
        St->model = -1;
        St->show = 0;
        for (int k = 0; k < 3; k++) EnableWindow(item(IDC_SHOW_MODEL + k), FALSE);
        show_level(-1);
        SetWindowTextW(item(IDC_ABOUT), L"");
        load_choice();
        return;
    }
    const LodView::Model &m = St->v.models[i];
    std::vector<int> seen;
    for (int lv = 0; lv < 5; lv++) {
        int p = m.level[lv];
        if (p < 0 || std::find(seen.begin(), seen.end(), p) != seen.end()) continue;
        seen.push_back(p);
        St->levels.push_back(lv);
    }
    for (size_t k = 0; k < St->levels.size() && k < 5; k++) {
        SetWindowTextW(item(IDC_LEVEL0 + (int)k), level_name(St->levels[k]));
        ShowWindow(item(IDC_LEVEL0 + (int)k), SW_SHOW);
    }
    // A new model starts at the usual view, at its most detailed level (or its collision, if that's what was shown
    // and it has one).
    bool coll = !m.collision.empty();
    for (int k = 0; k < 3; k++) EnableWindow(item(IDC_SHOW_MODEL + k), coll);
    if (!coll) St->show = 0;
    modelview::show(St->view, nullptr, L"");
    show_level(0);

    std::wstring t = W(m.name) + L": " + kind_label(m);
    if (m.placed) t += L", " + std::to_wstring(m.placed) + L" on the map";
    if (is_stock(m)) t += L" (the map gets an edited copy; your Xbox tags stay as they are)";
    if (std::wstring c = change_text(m); !c.empty()) t += L"\r\n" + c + L".";
    if (!m.note.empty()) {
        std::wstring n = W(m.note);
        n[0] = (wchar_t)towupper(n[0]);
        t += L"\r\n" + n + L".";
    }
    if (coll) t += L"\r\nCollision: " + W(m.collision);
    if (m.undone) t += L" The map didn't fit the Xbox with them, so the map you got doesn't have them.";
    // When each one is drawn: a level is used once the model is smaller on screen than the switch distance of
    // the level above it (tag order: super high, high, medium, low, super low).
    auto tris = [&](int lv) { return L" (" + thousands(triangles(m.level[lv])) + L" triangles)"; };
    if (St->levels.size() > 1) {
        t += L"\r\n" + std::wstring(level_name(St->levels[0])) + tris(St->levels[0]) + L" up close";
        for (size_t k = 1; k < St->levels.size(); k++) {
            int lv = St->levels[k];
            std::wstring d = distance(m, m.cutoffs[lv - 1]), name = level_name(lv);
            if (!name.empty()) name[0] = (wchar_t)towlower(name[0]);
            t += L"; " + name + tris(lv) + (d.empty() ? L"" : L" from " + d + L" away");
        }
        t += L".";
    } else if (!St->levels.empty()) {
        t += L"\r\nOne level" + tris(St->levels[0]) + L".";
    }
    if (m.cutoffs[4] > 0) t += L" Not drawn at all from " + distance(m, m.cutoffs[4]) + L".";
    if (m.as_made >= 0) t += L" Tick \"As the map made it\" to compare its most detailed level with the original.";
    SetWindowTextW(item(IDC_ABOUT), t.c_str());
    if (!St->library.empty()) status(L"");
    load_choice();
}

// Its most detailed triangles as the conversion left it, and each level's.
int top_triangles(const LodView::Model &m) { return triangles(m.level[0]); }
std::wstring level_triangles(const LodView::Model &m) {
    std::wstring tris;
    std::vector<int> seen;
    for (int p : m.level) {
        if (p < 0 || std::find(seen.begin(), seen.end(), p) != seen.end()) continue;
        seen.push_back(p);
        tris += (tris.empty() ? L"" : L" > ") + thousands(triangles(p));
    }
    return tris;
}

bool shown_by_filter(const LodView::Model &m) {
    if (St->filter == 1) return !m.had_lods && m.kind != 'f';
    if (St->filter == 2) return lod_changed(m) || !m.collision.empty();
    return true;
}

void fill_list() {
    HWND l = St->list;
    SendMessageW(l, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(l);
    St->rows.clear();
    for (size_t i = 0; i < St->v.models.size(); i++)
        if (shown_by_filter(St->v.models[i])) St->rows.push_back((int)i);
    std::vector<std::wstring> choice(St->v.models.size());
    for (int i : St->rows) choice[i] = choice_short(St->v.models[i]);
    auto key_less = [&](int a, int b) {
        const LodView::Model &x = St->v.models[a], &y = St->v.models[b];
        switch (St->sort_col) {
        case 0: return _wcsicmp(W(x.name).c_str(), W(y.name).c_str()) < 0;
        case 1: return kind_label(x) < kind_label(y);
        case 2: return change_short(x) < change_short(y);
        case 4: return x.density < y.density;
        case 5: return x.placed < y.placed;
        case 6: return choice[a] < choice[b];
        default: return top_triangles(x) < top_triangles(y);
        }
    };
    std::stable_sort(St->rows.begin(), St->rows.end(), [&](int a, int b) { return St->sort_up ? key_less(a, b) : key_less(b, a); });
    for (size_t r = 0; r < St->rows.size(); r++) {
        int i = St->rows[r];
        const LodView::Model &m = St->v.models[i];
        LVITEMW lv{};
        lv.mask = LVIF_TEXT | LVIF_PARAM;
        lv.iItem = (int)r;
        std::wstring name = W(m.name);
        lv.pszText = (LPWSTR)name.c_str();
        lv.lParam = (LPARAM)i;
        int row = ListView_InsertItem(l, &lv);
        std::wstring kind = kind_label(m), change = change_short(m), tris = level_triangles(m),
                     target = m.density > 0 ? thousands(m.density) : L"", placed = m.placed ? std::to_wstring(m.placed) : L"";
        ListView_SetItemText(l, row, 1, (LPWSTR)kind.c_str());
        ListView_SetItemText(l, row, 2, (LPWSTR)change.c_str());
        ListView_SetItemText(l, row, 3, (LPWSTR)tris.c_str());
        ListView_SetItemText(l, row, 4, (LPWSTR)target.c_str());
        ListView_SetItemText(l, row, 5, (LPWSTR)placed.c_str());
        ListView_SetItemText(l, row, 6, (LPWSTR)choice[i].c_str());
    }
    SendMessageW(l, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(l, nullptr, TRUE);
    // the model shown stays selected when it's still listed
    int r = row_of(St->model);
    if (r >= 0) {
        ListView_SetItemState(l, r, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(l, r, FALSE);
    } else if (!St->rows.empty()) {
        ListView_SetItemState(l, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    } else {
        select_model(-1);
    }
}

void close_window(bool again) {
    if (St->updating) return;   // (the update finishes first)
    save_choice();
    St->again = again;
    St->done = true;
}

// While the map is updated: nothing to change.
void enable_all(bool on) {
    for (int id : {IDC_LIST, IDC_FILTER, IDC_AGAIN, IDC_CLOSE, IDC_LIBRARY}) EnableWindow(item(id), on);
    if (on) enable_choice();
    else
        for (int id : {IDC_LODS, IDC_MED, IDC_LOW, IDC_TOP, IDC_TOPN}) EnableWindow(item(id), FALSE);
}

// "Update the map": the choices saved, then the map's models done again and the map built again (core.hpp
// update_xbox_models) on a thread; the list shows the result when it's done.
void start_update() {
    if (St->updating) return;
    save_choice();
    if (!St->update) return;
    St->updating = true;
    enable_all(false);
    status(L"Updating the map: the models' levels of detail and trims, then the build (a few seconds)...");
    HWND wnd = St->wnd;
    std::function<Result()> fn = St->update;
    std::thread([wnd, fn] {
        Result *r = new Result(fn());
        if (!PostMessageW(wnd, WM_UPDATED, 0, (LPARAM)r)) delete r;
    }).detach();
}

void finish_update(Result *res) {
    std::unique_ptr<Result> r(res);
    St->updating = false;
    enable_all(true);
    if (!r->ok && r->lods.empty()) {
        std::wstring e = W(r->error);
        if (e.find(L"convert the map again") != std::wstring::npos &&
            MessageBoxW(St->wnd, (e + L"\n\nConvert it again now?").c_str(), L"Halo Map Porter", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            close_window(true);
            return;
        }
        status(L"Not updated: " + e);
        return;
    }
    // the list again, from the update's file, the same model shown
    std::string shown = St->model >= 0 ? St->v.models[St->model].tag : std::string();
    LodView v;
    if (load_lod_view(v, r->lods.empty() ? St->file : r->lods)) {
        select_model(-1);   // (nothing drawn from the old one)
        St->v = std::move(v);
        int keep = -1;
        for (size_t i = 0; i < St->v.models.size(); i++)
            if (St->v.models[i].tag == shown) keep = (int)i;
        St->model = keep;
        fill_list();
        if (keep >= 0) select_model(keep);
    }
    std::wstring t = r->ok ? L"Updated the map" : L"The map didn't build: " + W(r->error);
    if (r->ok && !r->memory.empty()) t += L" (" + W(r->memory) + L")";
    status(t + L".");
}

LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        if (!IsWindowEnabled((HWND)lp)) SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_NOTIFY: {
        NMHDR *n = (NMHDR *)lp;
        if (n->idFrom == IDC_LIST && n->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *v = (NMLISTVIEW *)lp;
            if ((v->uChanged & LVIF_STATE) && (v->uNewState & LVIS_SELECTED) && !(v->uOldState & LVIS_SELECTED) &&
                (int)v->lParam != St->model) {
                save_choice();   // (the one shown before, typed but not yet saved)
                select_model((int)v->lParam);
            }
        }
        if (n->idFrom == IDC_LIST && n->code == LVN_COLUMNCLICK) {
            int col = ((NMLISTVIEW *)lp)->iSubItem;
            if (col == St->sort_col) St->sort_up = !St->sort_up;
            else St->sort_col = col, St->sort_up = col <= 2 || col == 6;   // (names A-Z; numbers biggest first)
            fill_list();
        }
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id >= IDC_LEVEL0 && id <= IDC_LEVEL4) {
            show_level(id - IDC_LEVEL0);
            return 0;
        }
        if (id >= IDC_SHOW_MODEL && id <= IDC_SHOW_COLL_MADE) {
            St->show = id - IDC_SHOW_MODEL;
            show_level(St->level);
            return 0;
        }
        switch (id) {
        case IDC_WIRE:
            modelview::show_triangles(St->view, SendMessageW(item(IDC_WIRE), BM_GETCHECK, 0, 0) == BST_CHECKED);
            return 0;
        case IDC_ASMADE:
            show_level(St->level);
            return 0;
        case IDC_FILTER:
            if (code == CBN_SELCHANGE) {
                save_choice();
                St->filter = (int)SendMessageW(item(IDC_FILTER), CB_GETCURSEL, 0, 0);
                fill_list();
            }
            return 0;
        case IDC_LODS:
        case IDC_TOP:
            if (code == CBN_SELCHANGE && !St->loading) {
                // A model with lower-detail versions of its own keeps them (only "don't" changes anything: no switch
                // distances).
                if (id == IDC_LODS && SendMessageW(item(IDC_LODS), CB_GETCURSEL, 0, 0) == 1 && St->model >= 0 &&
                    St->v.models[St->model].had_lods) {
                    SendMessageW(item(IDC_LODS), CB_SETCURSEL, St->saved.lods == 'n' ? 2 : 0, 0);
                    status(W(St->v.models[St->model].name) + L" has lower-detail versions of its own, which the app keeps; "
                           L"\"Don't make them\" leaves it without switch distances.");
                    return 0;
                }
                // "Trim it to": starting from 1.5x Bungie's for its kind and size (or half its triangles)
                if (id == IDC_TOP && SendMessageW(item(IDC_TOP), CB_GETCURSEL, 0, 0) == 2 && number(IDC_TOPN) <= 0 &&
                    St->model >= 0) {
                    const LodView::Model &m = St->v.models[St->model];
                    int top = m.trimmed_from > 0 ? m.trimmed_from : top_triangles(m);
                    int n = m.density > 0 && m.density < top ? m.density : top / 2;
                    if (n > 0) SetWindowTextW(item(IDC_TOPN), std::to_wstring(n).c_str());
                }
                enable_choice();
                save_choice();
            }
            return 0;
        case IDC_MED:
        case IDC_LOW:
        case IDC_TOPN:
            if (code == EN_KILLFOCUS) save_choice();
            return 0;
        case IDC_LIBRARY: {
            std::error_code ec;
            fs::create_directories(St->library, ec);
            ShellExecuteW(St->wnd, L"open", St->library.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        case IDOK: {   // (Enter: in a number box, saves it)
            HWND f = GetFocus();
            if (f == item(IDC_MED) || f == item(IDC_LOW) || f == item(IDC_TOPN)) {
                save_choice();
                return 0;
            }
            close_window(false);
            return 0;
        }
        case IDC_AGAIN:
            start_update();
            return 0;
        case IDC_CLOSE:
        case IDCANCEL:
            close_window(false);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        close_window(false);
        return 0;
    case WM_UPDATED:
        finish_update((Result *)lp);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

HWND add(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), St->wnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)St->font, TRUE);
    return c;
}

void tip(int id, const wchar_t *text) {
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = St->wnd;
    ti.uId = (UINT_PTR)item(id);
    ti.lpszText = (LPWSTR)text;
    SendMessageW(St->tips, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

void create_controls() {
    add(L"STATIC", L"The map's own models, Bungie's that have no lower-detail versions, and others this conversion changed. "
                   L"Click one to see each of its levels (a lower "
                   L"one is drawn once the model is far enough away to look small) or its collision, and choose below what "
                   L"conversions do with it. Click a column's title to sort by it.",
        SS_NOPREFIX, 16, 12, 1148, 36, IDC_INTRO);
    add(L"STATIC", L"Show:", SS_NOPREFIX, 16, 59, 40, 20, IDC_FILTER_LBL);
    HWND f = add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 58, 55, 360, 200, IDC_FILTER);
    for (auto t : {L"Every model listed", L"Models made without lower-detail versions (LODs)", L"Models this conversion changed"})
        SendMessageW(f, CB_ADDSTRING, 0, (LPARAM)t);
    SendMessageW(f, CB_SETCURSEL, 0, 0);
    St->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT |
                               LVS_SINGLESEL | LVS_SHOWSELALWAYS, S(16), S(84), S(700), S(452), St->wnd, (HMENU)(INT_PTR)IDC_LIST,
                               GetModuleHandleW(nullptr), nullptr);
    SendMessageW(St->list, WM_SETFONT, (WPARAM)St->font, TRUE);
    ListView_SetExtendedListViewStyle(St->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    struct { const wchar_t *t; int w; int fmt; } cols[] = {{L"Model", 124, LVCFMT_LEFT}, {L"Kind", 100, LVCFMT_LEFT},
                                                          {L"What changed", 96, LVCFMT_LEFT},
                                                          {L"Triangles by level", 136, LVCFMT_LEFT},
                                                          {L"1.5x Bungie", 80, LVCFMT_RIGHT},
                                                          {L"On map", 56, LVCFMT_RIGHT},
                                                          {L"Your choice", 104, LVCFMT_LEFT}};
    for (int i = 0; i < 7; i++) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.pszText = (LPWSTR)cols[i].t;
        c.cx = S(cols[i].w);
        c.fmt = cols[i].fmt;
        ListView_InsertColumn(St->list, i, &c);
    }
    const int X = 732;
    St->view = modelview::create(St->wnd, S(X), S(56), S(432), S(262), IDC_VIEW);
    SendMessageW(St->view, WM_SETFONT, (WPARAM)St->font, TRUE);
    // the model or its collision (each part where the model's rest pose puts it, coloured by material, outlined)
    add(L"BUTTON", L"The model", BS_AUTORADIOBUTTON | BS_PUSHLIKE | WS_GROUP | WS_TABSTOP, X, 324, 140, 24, IDC_SHOW_MODEL);
    add(L"BUTTON", L"Its collision", BS_AUTORADIOBUTTON | BS_PUSHLIKE, X + 144, 324, 140, 24, IDC_SHOW_COLL);
    add(L"BUTTON", L"Collision as made", BS_AUTORADIOBUTTON | BS_PUSHLIKE, X + 288, 324, 144, 24, IDC_SHOW_COLL_MADE);
    for (int i = 0; i < 5; i++)   // one per level (as many as the model has)
        add(L"BUTTON", L"", BS_AUTORADIOBUTTON | BS_PUSHLIKE | (i == 0 ? WS_GROUP | WS_TABSTOP : 0), X + i * 87, 352, 84, 26,
            IDC_LEVEL0 + i);
    add(L"BUTTON", L"Show the triangles", BS_AUTOCHECKBOX | WS_TABSTOP, X, 382, 200, 22, IDC_WIRE);
    add(L"BUTTON", L"As the map made it", BS_AUTOCHECKBOX | WS_TABSTOP, X + 208, 382, 224, 22, IDC_ASMADE);
    add(L"STATIC", L"", SS_NOPREFIX, X, 408, 432, 132, IDC_ABOUT);

    // The choices for the model shown (lod.hpp Choice), saved as they change.
    add(L"STATIC", L"", SS_ETCHEDHORZ, 16, 546, 1148, 2, 0);
    SendMessageW(add(L"STATIC", L"", SS_NOPREFIX, 16, 552, 1148, 20, IDC_GROUP), WM_SETFONT, (WPARAM)St->bold, TRUE);
    add(L"STATIC", L"Lower-detail versions:", SS_NOPREFIX, 16, 581, 160, 20, IDC_LODS_LBL);
    HWND lc = add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 180, 577, 250, 200, IDC_LODS);
    for (auto t : {L"As the Xbox tab says", L"Make them, aiming at", L"Don't make them (drawn in full)"})
        SendMessageW(lc, CB_ADDSTRING, 0, (LPARAM)t);
    add(L"STATIC", L"Medium:", SS_NOPREFIX | SS_RIGHT, 436, 581, 60, 20, IDC_MED_LBL);
    add(L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL, 500, 578, 72, 23, IDC_MED);
    add(L"STATIC", L"Low:", SS_NOPREFIX | SS_RIGHT, 576, 581, 38, 20, IDC_LOW_LBL);
    add(L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL, 618, 578, 72, 23, IDC_LOW);
    add(L"STATIC", L"triangles (empty: about half and a fifth of the most detailed)", SS_NOPREFIX, 698, 581, 466, 20,
        IDC_TRIS_LBL);
    add(L"STATIC", L"Most detailed version:", SS_NOPREFIX, 16, 613, 160, 20, IDC_TOP_LBL);
    HWND tc = add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 180, 609, 250, 200, IDC_TOP);
    for (auto t : {L"As \"Heavy models\" says", L"Keep it as made", L"Trim it to"}) SendMessageW(tc, CB_ADDSTRING, 0, (LPARAM)t);
    add(L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL, 500, 610, 72, 23, IDC_TOPN);
    add(L"STATIC", L"triangles", SS_NOPREFIX, 580, 613, 584, 20, IDC_TOPN_LBL);
    for (int id : {IDC_MED, IDC_LOW}) SendMessageW(item(id), EM_SETCUEBANNER, TRUE, (LPARAM)L"auto");

    add(L"BUTTON", L"Open the LOD library folder", WS_TABSTOP, 16, 650, 220, 30, IDC_LIBRARY);
    add(L"STATIC", L"", SS_NOPREFIX, 248, 650, 630, 34, IDC_STATUS);
    add(L"BUTTON", L"Update the map", WS_TABSTOP, 888, 650, 150, 30, IDC_AGAIN);
    add(L"BUTTON", L"Close", WS_TABSTOP | BS_DEFPUSHBUTTON, 1048, 650, 116, 30, IDC_CLOSE);
    for (int i = 0; i < 5; i++) ShowWindow(item(IDC_LEVEL0 + i), SW_HIDE);
    EnableWindow(item(IDC_ASMADE), FALSE);
    if (St->library.empty()) status(L"Set the LOD library folder on the Settings tab to keep choices for models.");

    St->tips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT,
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, St->wnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(St->tips, TTM_SETMAXTIPWIDTH, 0, S(380));
    SendMessageW(St->tips, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
    tip(IDC_LODS, L"As the Xbox tab says: made when \"Make lower-detail versions\" is on (Bungie's models: \"Also make and use "
                  L"LODs for Bungie's models\") and the model has 1,000 triangles or more, or 300 or more and 5,000 or more on "
                  L"the map in all.\n\nMake them: made whatever the Xbox tab says, however few triangles it has, aiming at the medium "
                  L"and low triangles you give (the shape may move further to get there; empty: about half and a fifth).\n\n"
                  L"Don't make them: drawn in full at every distance; a model with lower-detail versions of its own gets "
                  L"no switch distances either.\n\nKept in choices.txt in the LOD library, for every map that uses the model.");
    tip(IDC_TOP, L"As \"Heavy models\" says: the Xbox tab's choice.\n\nKeep it as made: never trimmed.\n\nTrim it to: aims at "
                 L"that many triangles, whatever the Xbox tab says (the shape may move up to 5% of the model's size to get "
                 L"there). It starts at 1.5x what Bungie's models of its kind and size have.\n\nKept in choices.txt in the "
                 L"LOD library, for every map that uses the model.");
    tip(IDC_AGAIN, L"Puts your choices in the map you have: its models' levels of detail and trims are done again on the "
                   L"tags the conversion prepared, and the map is built again (a few seconds; the rest of the map stays "
                   L"as converted). The list then shows the map as updated.");
}

}   // namespace

bool show(HWND owner, const std::filesystem::path &file, const std::filesystem::path &library,
          const std::function<porter::Result()> &update) {
    State st;
    st.file = file;
    st.update = update;
    if (!load_lod_view(st.v, file)) {
        MessageBoxW(owner, L"The levels of detail of that conversion can't be shown (the file is missing or from another "
                           L"version of the app).", L"Halo Map Porter", MB_ICONINFORMATION);
        return false;
    }
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
        wc.lpszClassName = L"HMPLods";
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        RegisterClassExW(&wc);
        registered = true;
    }
    St = &st;
    st.library = library;
    {
        HDC dc = GetDC(nullptr);
        st.dpi = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(nullptr, dc);
    }
    st.font = CreateFontW(-MulDiv(9, st.dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    st.bold = CreateFontW(-MulDiv(9, st.dpi, 72), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    RECT r{0, 0, S(1180), S(696)};
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    AdjustWindowRect(&r, style, FALSE);
    RECT orc;
    GetWindowRect(owner, &orc);
    int w = r.right - r.left, hgt = r.bottom - r.top;
    int x = orc.left + ((orc.right - orc.left) - w) / 2, y = orc.top + ((orc.bottom - orc.top) - hgt) / 2;
    MONITORINFO mi{sizeof(mi)};
    if (GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &mi)) {
        x = std::clamp(x, (int)mi.rcWork.left, std::max((int)mi.rcWork.left, (int)mi.rcWork.right - w));
        y = std::clamp(y, (int)mi.rcWork.top, std::max((int)mi.rcWork.top, (int)mi.rcWork.bottom - hgt));
    }
    std::wstring title = L"Levels of detail and collision - " + W(st.v.map_name);
    st.wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"HMPLods", title.c_str(), style, x, y, w, hgt, owner, nullptr, inst, nullptr);
    create_controls();
    select_model(-1);
    fill_list();
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
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    DestroyWindow(st.wnd);
    DeleteObject(st.font);
    DeleteObject(st.bold);
    St = nullptr;
    return st.again;
}

}   // namespace lodui
