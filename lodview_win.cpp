// Halo Map Porter - the "LODs" window (see lodview_win.hpp)
#include "lodview_win.hpp"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <string>
#include <vector>

#include "core.hpp"
#include "lod.hpp"
#include "modelview_win.hpp"

namespace lodui {
using namespace porter;

namespace {

enum { IDC_INTRO = 400, IDC_LIST, IDC_VIEW, IDC_LEVEL0, IDC_LEVEL4 = IDC_LEVEL0 + 4, IDC_WIRE, IDC_ASMADE, IDC_ABOUT,
       IDC_SKIP, IDC_LIBRARY, IDC_CLOSE, IDC_SHOW_MODEL, IDC_SHOW_COLL, IDC_SHOW_COLL_MADE };

struct State {
    HWND wnd = nullptr, list = nullptr, view = nullptr, tips = nullptr;
    std::wstring tip;             // the skip button's tooltip, as last asked for
    int dpi = 96;
    HFONT font = nullptr;
    bool done = false;
    LodView v;
    fs::path library;
    int model = -1;               // the model shown
    std::vector<int> levels;      // its drawings, most detailed first (each one's first level, tag order)
    int level = 0;                // the drawing shown (index in `levels`)
    int show = 0;                 // 0 the model, 1 its collision as converted, 2 its collision as the map made it
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
    return t;
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

bool in_skip(const LodView::Model &m) {
    lod::Settings s;
    s.library = St->library;
    return lod::skipped(s, m.tag, m.name);
}

// The "never make LODs" button, for the model shown.
void update_skip() {
    bool skip = St->model >= 0 && in_skip(St->v.models[St->model]);
    bool trimmed = St->model >= 0 && St->v.models[St->model].trimmed_from > 0;
    bool lods = St->model >= 0 && lod_changed(St->v.models[St->model]);
    EnableWindow(item(IDC_SKIP), lods && !skip && !St->library.empty());
    SetWindowTextW(item(IDC_SKIP), skip      ? L"Left as made (in skip.txt)"
                                   : trimmed ? L"Leave this model as made"
                                             : L"Never make LODs for this model");
}

// Its tooltip: what pressing it does (and that doing nothing keeps what's shown), or how to undo it.
const wchar_t *skip_tip() {
    if (St->library.empty()) return L"Set the LOD library folder on the Settings tab first: skip.txt is kept there.";
    if (St->model < 0) return L"Click a model in the list first.";
    const LodView::Model &m = St->v.models[St->model];
    if (!lod_changed(m)) return L"Only this model's collision changed; skip.txt is for its levels of detail.";
    if (in_skip(m))
        St->tip = W(m.name) + L" is in skip.txt in the LOD library: conversions leave it as the map has it. To undo, "
                  L"click \"Open the LOD library folder\" and delete its line from skip.txt (the line above it names the "
                  L"model and the map).";
    else if (m.trimmed_from > 0)
        St->tip = L"Adds " + W(m.name) + L" to skip.txt in the LOD library, so conversions leave it exactly as the map has "
                  L"it, for this map or any other that uses it: its most detailed version untrimmed, and no lower-detail "
                  L"versions or switch distances made for it.\n\nTo keep what's shown here, don't press this: the trimmed "
                  L"version is saved in the LOD library and reused every time.\n\nThe map you have now keeps it until you "
                  L"convert it again.";
    else if (m.change == "made" || m.change == "library")
        St->tip = L"Adds " + W(m.name) + L" to skip.txt in the LOD library, so no conversion makes lower-detail versions "
                  L"of it again, for this map or any other that uses it: it's drawn in full at every distance.\n\n"
                  L"To keep the versions shown here, don't press this: they're saved in the LOD library and reused every "
                  L"time.\n\nThe map you have now keeps them until you convert it again.";
    else
        St->tip = L"Adds " + W(m.name) + L" to skip.txt in the LOD library, so conversions stop giving it switch "
                  L"distances, for this map or any other that uses it: the game draws its most detailed level at every "
                  L"distance, as the map had it.\n\nTo keep the distances shown here, don't press this.";
    return St->tip.c_str();
}

// Tooltips: one on the button and one over its spot in the window (a disabled button gets no mouse messages, so
// the window shows the tip then).
void add_tips() {
    St->tips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT,
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, St->wnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(St->tips, TTM_SETMAXTIPWIDTH, 0, S(360));
    SendMessageW(St->tips, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
    HWND b = item(IDC_SKIP);
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = St->wnd;
    ti.uId = (UINT_PTR)b;
    ti.lpszText = LPSTR_TEXTCALLBACKW;
    SendMessageW(St->tips, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    TOOLINFOW tw{};
    tw.cbSize = sizeof(tw);
    tw.uFlags = TTF_SUBCLASS;
    tw.hwnd = St->wnd;
    tw.uId = IDC_SKIP;
    GetWindowRect(b, &tw.rect);
    MapWindowPoints(nullptr, St->wnd, (POINT *)&tw.rect, 2);
    tw.lpszText = LPSTR_TEXTCALLBACKW;
    SendMessageW(St->tips, TTM_ADDTOOLW, 0, (LPARAM)&tw);
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
        EnableWindow(item(IDC_SKIP), FALSE);
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

    std::wstring t = W(m.name) + L": " + kind_text(m.kind);
    if (m.placed) t += L", " + std::to_wstring(m.placed) + L" on the map";
    if (std::wstring c = change_text(m); !c.empty()) t += L"\r\n" + c + L".";
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
    update_skip();
}

void fill_list() {
    HWND l = St->list;
    ListView_DeleteAllItems(l);
    for (size_t i = 0; i < St->v.models.size(); i++) {
        const LodView::Model &m = St->v.models[i];
        LVITEMW lv{};
        lv.mask = LVIF_TEXT | LVIF_PARAM;
        lv.iItem = (int)i;
        std::wstring name = W(m.name);
        lv.pszText = (LPWSTR)name.c_str();
        lv.lParam = (LPARAM)i;
        int row = ListView_InsertItem(l, &lv);
        std::wstring kind = kind_text(m.kind), change = change_short(m), tris, placed = m.placed ? std::to_wstring(m.placed) : L"";
        std::vector<int> seen;
        for (int p : m.level) {
            if (p < 0 || std::find(seen.begin(), seen.end(), p) != seen.end()) continue;
            seen.push_back(p);
            tris += (tris.empty() ? L"" : L" > ") + thousands(triangles(p));
        }
        ListView_SetItemText(l, row, 1, (LPWSTR)kind.c_str());
        ListView_SetItemText(l, row, 2, (LPWSTR)change.c_str());
        ListView_SetItemText(l, row, 3, (LPWSTR)tris.c_str());
        ListView_SetItemText(l, row, 4, (LPWSTR)placed.c_str());
    }
}

LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_NOTIFY: {
        NMHDR *n = (NMHDR *)lp;
        if (n->code == TTN_GETDISPINFOW && St && n->hwndFrom == St->tips) {
            ((NMTTDISPINFOW *)lp)->lpszText = (LPWSTR)skip_tip();
            return 0;
        }
        if (n->idFrom == IDC_LIST && n->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *v = (NMLISTVIEW *)lp;
            if ((v->uChanged & LVIF_STATE) && (v->uNewState & LVIS_SELECTED) && !(v->uOldState & LVIS_SELECTED))
                select_model((int)v->lParam);
        }
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
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
        case IDC_SKIP:
            if (St->model >= 0) {
                const LodView::Model &m = St->v.models[St->model];
                lod::Settings s;
                s.library = St->library;
                if (!lod::add_skip(s, m.tag, m.name + " (" + St->v.map_name + ")")) {
                    MessageBoxW(St->wnd, L"Couldn't write skip.txt in the LOD library folder.", L"Halo Map Porter", MB_ICONWARNING);
                    return 0;
                }
                update_skip();
                std::wstring note = L"Added " + W(m.name) + L" to skip.txt: conversions won't make LODs for it again (the "
                                    L"map you have now still has them; convert it again to drop them).";
                if (m.trimmed_from > 0)
                    note = L"Added " + W(m.name) + L" to skip.txt: conversions will leave it as made (the map you have "
                           L"now still has it trimmed; convert it again to keep it whole).";
                else if (m.change != "made" && m.change != "library")
                    note = L"Added " + W(m.name) + L" to skip.txt: conversions won't give it switch distances again.";
                SetWindowTextW(item(IDC_INTRO), note.c_str());
            }
            return 0;
        case IDC_LIBRARY: {
            std::error_code ec;
            fs::create_directories(St->library, ec);
            ShellExecuteW(St->wnd, L"open", St->library.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        case IDC_CLOSE:
        case IDOK:
        case IDCANCEL:
            St->done = true;
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        St->done = true;
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

void create_controls() {
    add(L"STATIC", L"The models whose levels of detail, most detailed version or collision this conversion changed. Click one "
                   L"to see each of its levels (a lower one is drawn once the model is far enough away to look small), or its "
                   L"collision.",
        SS_NOPREFIX, 16, 12, 1048, 36, IDC_INTRO);
    St->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT |
                               LVS_SINGLESEL | LVS_SHOWSELALWAYS, S(16), S(56), S(600), S(480), St->wnd, (HMENU)(INT_PTR)IDC_LIST,
                               GetModuleHandleW(nullptr), nullptr);
    SendMessageW(St->list, WM_SETFONT, (WPARAM)St->font, TRUE);
    ListView_SetExtendedListViewStyle(St->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    struct { const wchar_t *t; int w; int fmt; } cols[] = {{L"Model", 150, LVCFMT_LEFT}, {L"Kind", 64, LVCFMT_LEFT},
                                                          {L"What changed", 140, LVCFMT_LEFT},
                                                          {L"Triangles by level", 172, LVCFMT_LEFT},
                                                          {L"On map", 52, LVCFMT_RIGHT}};
    for (int i = 0; i < 5; i++) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.pszText = (LPWSTR)cols[i].t;
        c.cx = S(cols[i].w);
        c.fmt = cols[i].fmt;
        ListView_InsertColumn(St->list, i, &c);
    }
    St->view = modelview::create(St->wnd, S(632), S(56), S(432), S(292), IDC_VIEW);
    SendMessageW(St->view, WM_SETFONT, (WPARAM)St->font, TRUE);
    // the model or its collision (each part where the model's rest pose puts it, coloured by material, outlined)
    add(L"BUTTON", L"The model", BS_AUTORADIOBUTTON | BS_PUSHLIKE | WS_GROUP | WS_TABSTOP, 632, 354, 140, 24, IDC_SHOW_MODEL);
    add(L"BUTTON", L"Its collision", BS_AUTORADIOBUTTON | BS_PUSHLIKE, 776, 354, 140, 24, IDC_SHOW_COLL);
    add(L"BUTTON", L"Collision as made", BS_AUTORADIOBUTTON | BS_PUSHLIKE, 920, 354, 144, 24, IDC_SHOW_COLL_MADE);
    for (int i = 0; i < 5; i++)   // one per level (as many as the model has)
        add(L"BUTTON", L"", BS_AUTORADIOBUTTON | BS_PUSHLIKE | (i == 0 ? WS_GROUP | WS_TABSTOP : 0), 632 + i * 87, 384, 84, 26,
            IDC_LEVEL0 + i);
    add(L"BUTTON", L"Show the triangles", BS_AUTOCHECKBOX | WS_TABSTOP, 632, 414, 200, 22, IDC_WIRE);
    add(L"BUTTON", L"As the map made it", BS_AUTOCHECKBOX | WS_TABSTOP, 840, 414, 224, 22, IDC_ASMADE);
    add(L"STATIC", L"", SS_NOPREFIX, 632, 440, 432, 100, IDC_ABOUT);
    add(L"BUTTON", L"Never make LODs for this model", WS_TABSTOP, 16, 548, 260, 30, IDC_SKIP);
    add(L"BUTTON", L"Open the LOD library folder", WS_TABSTOP, 288, 548, 220, 30, IDC_LIBRARY);
    add(L"BUTTON", L"Close", WS_TABSTOP | BS_DEFPUSHBUTTON, 948, 548, 116, 30, IDC_CLOSE);
    for (int i = 0; i < 5; i++) ShowWindow(item(IDC_LEVEL0 + i), SW_HIDE);
    EnableWindow(item(IDC_SKIP), FALSE);
    EnableWindow(item(IDC_ASMADE), FALSE);
    add_tips();
}

}   // namespace

void show(HWND owner, const std::filesystem::path &file, const std::filesystem::path &library) {
    State st;
    if (!load_lod_view(st.v, file)) {
        MessageBoxW(owner, L"The levels of detail of that conversion can't be shown (the file is missing or from another "
                           L"version of the app).", L"Halo Map Porter", MB_ICONINFORMATION);
        return;
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
    RECT r{0, 0, S(1080), S(594)};
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
    fill_list();
    select_model(-1);
    if (!st.v.models.empty()) ListView_SetItemState(st.list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
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
    St = nullptr;
}

}   // namespace lodui
