// Halo Map Porter - the map list settings window (see maplist_win.hpp)
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "maplist_win.hpp"
#include "maplist_edit.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

namespace maplist {
namespace {
enum {
    IDC_DIR = 3101, IDC_DIR_BROWSE, IDC_LOAD, IDC_STATS, IDC_STATS_BROWSE, IDC_HINT, IDC_LIST, IDC_GENRE, IDC_GENRE_ADD,
    IDC_GENRE_DEL, IDC_FAV, IDC_HIDE, IDC_SHOW, IDC_SAVE, IDC_STATUS, IDC_LBL, IDC_ALL, IDC_SCAN, IDC_REPLACE,
};
constexpr int kMenuFav = 4100;
constexpr UINT WM_SCANNED = WM_APP + 30, WM_SCAN_DONE = WM_APP + 31;
struct Scanned { size_t row; bool ok; Scan scan; std::string err; };

std::wstring W(const std::string &s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
std::string N(const std::wstring &w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

struct Win {
    HWND wnd = nullptr, owner = nullptr;
    HFONT font = nullptr, bold = nullptr;
    int dpi = 96;
    List list;
    bool loaded = false, done = false;
    std::wstring *dir = nullptr, *stats = nullptr;
    Analyzer analyze;
    std::thread scanner;
    std::atomic<bool> stop{false};
    bool scanning = false;
    size_t scan_n = 0, scan_k = 0, scan_changed = 0, scan_failed = 0;
    std::vector<std::wstring> scan_said;
};
Win *M = nullptr;
int S(int v) { return MulDiv(v, M->dpi, 96); }
HWND item(int id) { return GetDlgItem(M->wnd, id); }
std::wstring text(int id) {
    int n = GetWindowTextLengthW(item(id));
    std::wstring s(n + 1, 0);
    GetWindowTextW(item(id), &s[0], n + 1);
    s.resize(n);
    return s;
}
HWND add(const wchar_t *cls, const wchar_t *t, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0, HFONT f = nullptr) {
    HWND c = CreateWindowExW(ex, cls, t, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), M->wnd, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(f ? f : M->font), TRUE);
    return c;
}
HWND lv() { return item(IDC_LIST); }

void set_cell(int i, int col, const std::wstring &s) { ListView_SetItemText(lv(), i, col, (LPWSTR)s.c_str()); }
void fill_row(int i) {
    const Row &r = M->list.rows[(size_t)i];
    std::wstring key = (r.txt_dirty || r.stats_dirty || r.players_dirty || r.gametypes_dirty ? L"* " : L"") + W(r.key);
    set_cell(i, 0, key);
    set_cell(i, 1, W(r.name));
    set_cell(i, 2, W(r.players));
    set_cell(i, 3, W(r.gametypes));
    set_cell(i, 4, W(join_list(r.genres())));
    set_cell(i, 5, r.fav ? L"yes" : L"");
    set_cell(i, 6, r.hide ? L"hidden" : L"");
    set_cell(i, 7, r.in_stats || r.plays ? std::to_wstring(r.plays) : L"");
}
std::vector<int> selected() {
    std::vector<int> v;
    for (int i = ListView_GetNextItem(lv(), -1, LVNI_SELECTED); i >= 0; i = ListView_GetNextItem(lv(), i, LVNI_SELECTED))
        v.push_back(i);
    return v;
}
void status() {
    int sel = (int)selected().size(), ch = M->list.changed();
    std::wstring s;
    if (!M->loaded) s = L"Pick the maps folder and click Load.";
    else if (M->scanning) s = L"Scanning " + std::to_wstring(M->scan_k + 1) + L" of " + std::to_wstring(M->scan_n) + L"...";
    else {
        s = std::to_wstring(M->list.rows.size()) + L" maps, " + std::to_wstring(sel) + L" selected";
        if (ch) s += L"  \x00b7  " + std::to_wstring(ch) + L" changed, not saved yet";
        if (M->list.stats_file.empty()) s += L"  \x00b7  no hmp_stats.txt: favorites / hidden off";
    }
    SetWindowTextW(item(IDC_STATUS), s.c_str());
    bool any = M->loaded && sel > 0 && !M->scanning, st = any && !M->list.stats_file.empty();
    for (int id : {IDC_GENRE_ADD, IDC_GENRE_DEL}) EnableWindow(item(id), any);
    for (int id : {IDC_FAV, IDC_HIDE, IDC_SHOW}) EnableWindow(item(id), st);
    for (int id : {IDC_LOAD, IDC_DIR_BROWSE, IDC_STATS_BROWSE, IDC_REPLACE}) EnableWindow(item(id), !M->scanning);
    EnableWindow(item(IDC_SAVE), ch > 0 && !M->scanning);
    EnableWindow(item(IDC_SCAN), M->loaded && (bool)M->analyze);
    SetWindowTextW(item(IDC_SCAN), M->scanning ? L"Stop scanning" : sel > 1 ? L"Scan selected maps" : L"Scan all maps");
}
void fill_genres() {
    HWND cb = item(IDC_GENRE);
    std::wstring cur = text(IDC_GENRE);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    for (auto &g : M->list.genre_names()) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)W(g).c_str());
    SetWindowTextW(cb, cur.c_str());
}
void fill() {
    ListView_DeleteAllItems(lv());
    for (size_t i = 0; i < M->list.rows.size(); i++) {
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        it.pszText = (LPWSTR)L"";
        ListView_InsertItem(lv(), &it);
        fill_row((int)i);
    }
    fill_genres();
    status();
}
bool keep_changes_ok() {
    if (!M->loaded || !M->list.changed()) return true;
    int a = MessageBoxW(M->wnd, L"Save the changes first?", L"Map list", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (a == IDCANCEL) return false;
    if (a == IDNO) return true;
    std::vector<std::string> done;
    std::string err;
    if (save(M->list, done, err)) return true;
    MessageBoxW(M->wnd, W(err).c_str(), L"Map list", MB_ICONWARNING);
    return false;
}
void do_load() {
    if (!keep_changes_ok()) return;
    std::string err;
    List l;
    std::wstring d = text(IDC_DIR), s = text(IDC_STATS);
    if (!load(std::filesystem::path(d), std::filesystem::path(s), l, err)) {
        MessageBoxW(M->wnd, W(err).c_str(), L"Map list", MB_ICONINFORMATION);
        return;
    }
    if (!s.empty() && !l.stats_exists &&
        MessageBoxW(M->wnd, (s + L" doesn't exist yet. Make a new one when you save?").c_str(), L"Map list",
                    MB_YESNO | MB_ICONQUESTION) != IDYES)
        l.stats_file.clear();
    M->list = std::move(l);
    M->loaded = true;
    *M->dir = d;
    *M->stats = M->list.stats_file.empty() ? L"" : s;
    fill();
}
void do_save() {
    std::vector<std::string> done;
    std::string err;
    bool ok = save(M->list, done, err);
    for (size_t i = 0; i < M->list.rows.size(); i++) fill_row((int)i);
    status();
    if (!ok) { MessageBoxW(M->wnd, W(err).c_str(), L"Map list", MB_ICONWARNING); return; }
    std::wstring msg = L"Saved:\n";
    for (size_t i = 0; i < done.size() && i < 20; i++) msg += L"  " + W(done[i]) + L"\n";
    if (done.size() > 20) msg += L"  and " + std::to_wstring(done.size() - 20) + L" more\n";
    if (!M->list.stats_file.empty())
        msg += L"\nCopy hmp_stats.txt back to E:\\TDATA\\4d530004\\ with Halo switched off (Halo rewrites it while it runs).";
    MessageBoxW(M->wnd, msg.c_str(), L"Map list", MB_ICONINFORMATION);
}
template <class F> void on_selected(F f) {
    for (int i : selected()) { f(M->list.rows[(size_t)i]); fill_row(i); }
    fill_genres();
    status();
}
std::wstring browse_folder(const std::wstring &start) {
    std::wstring result;
    IFileOpenDialog *dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) {
        DWORD opts;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        if (!start.empty()) {
            IShellItem *it;
            if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&it)))) { dlg->SetFolder(it); it->Release(); }
        }
        if (SUCCEEDED(dlg->Show(M->wnd))) {
            IShellItem *it;
            if (SUCCEEDED(dlg->GetResult(&it))) {
                PWSTR p;
                if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { result = p; CoTaskMemFree(p); }
                it->Release();
            }
        }
        dlg->Release();
    }
    return result;
}
// Scan: each folder map's gametypes, a player guess and genre suggestions (porter::analyze_map_traits), on a worker
// thread; the rows get what they don't have yet (or, with "Replace", new Gametypes / Players).
void start_scan() {
    std::vector<std::pair<size_t, std::filesystem::path>> todo;
    auto sel = selected();
    if (sel.size() > 1) { for (int i : sel) if (!M->list.rows[(size_t)i].stock) todo.push_back({(size_t)i, M->list.rows[(size_t)i].map}); }
    else for (size_t i = 0; i < M->list.rows.size(); i++) if (!M->list.rows[i].stock) todo.push_back({i, M->list.rows[i].map});
    if (todo.empty()) { MessageBoxW(M->wnd, L"There are no map folders to scan here.", L"Map list", MB_ICONINFORMATION); return; }
    M->scanning = true;
    M->stop = false;
    M->scan_n = todo.size();
    M->scan_k = M->scan_changed = M->scan_failed = 0;
    M->scan_said.clear();
    status();
    HWND wnd = M->wnd;
    Analyzer analyze = M->analyze;
    std::atomic<bool> *stop = &M->stop;
    M->scanner = std::thread([todo, wnd, analyze, stop] {
        for (auto &t : todo) {
            if (*stop) break;
            auto *r = new Scanned{t.first, false, Scan(), std::string()};
            r->ok = !t.second.empty() && analyze(t.second, r->scan, r->err);
            if (t.second.empty()) r->err = "no .map in the folder";
            PostMessageW(wnd, WM_SCANNED, 0, (LPARAM)r);
        }
        PostMessageW(wnd, WM_SCAN_DONE, 0, 0);
    });
}
void scanned(Scanned *r) {
    Row &row = M->list.rows[r->row];
    M->scan_k++;
    if (!r->ok) {
        M->scan_failed++;
        M->scan_said.push_back(W(row.key) + L": couldn't read it (" + W(r->err) + L")");
    } else {
        std::string said = apply_scan(row, r->scan, IsDlgButtonChecked(M->wnd, IDC_REPLACE) == BST_CHECKED);
        if (!said.empty()) {
            M->scan_changed++;
            M->scan_said.push_back(W(row.key) + L": " + W(said));
        }
        fill_row((int)r->row);
    }
    delete r;
    status();
}
void scan_done() {
    if (M->scanner.joinable()) M->scanner.join();
    bool stopped = M->stop;
    M->scanning = false;
    fill_genres();
    status();
    std::wstring msg = std::wstring(stopped ? L"Stopped after " : L"Scanned ") + std::to_wstring(M->scan_k) + L" map" +
                       (M->scan_k == 1 ? L"" : L"s") + L": " + std::to_wstring(M->scan_changed) + L" got new lines" +
                       (M->scan_failed ? L", " + std::to_wstring(M->scan_failed) + L" couldn't be read" : L"") + L".";
    if (M->scan_changed) msg += L" Click Save to write them into the info.txt files.";
    msg += L"\n\n";
    for (size_t i = 0; i < M->scan_said.size() && i < 25; i++) msg += M->scan_said[i] + L"\n";
    if (M->scan_said.size() > 25) msg += L"and " + std::to_wstring(M->scan_said.size() - 25) + L" more\n";
    MessageBoxW(M->wnd, msg.c_str(), L"Map list", MB_ICONINFORMATION);
}
void create_controls() {
    const int L = 20;
    add(L"STATIC", L"Xbox maps folder:", 0, L, 17, 120, 20, IDC_LBL);
    add(L"EDIT", M->dir->c_str(), WS_TABSTOP | ES_AUTOHSCROLL, L + 124, 14, 606, 24, IDC_DIR, WS_EX_CLIENTEDGE);
    add(L"BUTTON", L"Browse...", WS_TABSTOP, L + 736, 13, 84, 26, IDC_DIR_BROWSE);
    add(L"BUTTON", L"Load", WS_TABSTOP | BS_DEFPUSHBUTTON, L + 826, 13, 94, 26, IDC_LOAD);
    add(L"STATIC", L"hmp_stats.txt:", 0, L, 49, 120, 20, IDC_LBL);
    add(L"EDIT", M->stats->c_str(), WS_TABSTOP | ES_AUTOHSCROLL, L + 124, 46, 606, 24, IDC_STATS, WS_EX_CLIENTEDGE);
    add(L"BUTTON", L"Browse...", WS_TABSTOP, L + 736, 45, 84, 26, IDC_STATS_BROWSE);
    add(L"STATIC",
        L"Genres, players and gametypes are saved in each map's info.txt (Halo's own maps' genres: in the folder's genres.txt). "
        L"Favorites and hidden maps live on the Xbox in E:\\TDATA\\4d530004\\hmp_stats.txt: copy it here over FTP, change it, "
        L"and copy it back, with Halo switched off (Halo rewrites it while it runs). Leave it empty to change the rest only.",
        0, L, 76, 900, 48, IDC_HINT);
    HWND l = add(WC_LISTVIEWW, L"", WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SHOWSELALWAYS, L, 128, 900, 380, IDC_LIST);
    ListView_SetExtendedListViewStyle(l, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
    const struct { const wchar_t *t; int w; } cols[] = {{L"Map", 140}, {L"Name", 150}, {L"Players", 80}, {L"Gametypes", 156},
                                                       {L"Genres", 188}, {L"Favorite", 56}, {L"Hidden", 52}, {L"Plays", 44}};
    for (int i = 0; i < 8; i++) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = (LPWSTR)cols[i].t;
        c.cx = S(cols[i].w);
        ListView_InsertColumn(l, i, &c);
    }
    const int B = 518;
    add(L"STATIC", L"Genre:", 0, L, B + 4, 46, 20, IDC_LBL);
    add(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL, L + 48, B, 170, 300, IDC_GENRE);
    add(L"BUTTON", L"Add", WS_TABSTOP, L + 224, B - 1, 60, 26, IDC_GENRE_ADD);
    add(L"BUTTON", L"Remove", WS_TABSTOP, L + 290, B - 1, 70, 26, IDC_GENRE_DEL);
    add(L"BUTTON", L"Favorite...", WS_TABSTOP, L + 384, B - 1, 84, 26, IDC_FAV);
    add(L"BUTTON", L"Hide", WS_TABSTOP, L + 474, B - 1, 60, 26, IDC_HIDE);
    add(L"BUTTON", L"Show", WS_TABSTOP, L + 540, B - 1, 60, 26, IDC_SHOW);
    add(L"BUTTON", L"Select all", WS_TABSTOP, L + 624, B - 1, 84, 26, IDC_ALL);
    add(L"BUTTON", L"Save", WS_TABSTOP, L + 826, B - 1, 94, 26, IDC_SAVE);
    add(L"BUTTON", L"Scan all maps", WS_TABSTOP, L, B + 33, 150, 26, IDC_SCAN);
    add(L"BUTTON", L"Replace Players / Gametypes lines they already have", WS_TABSTOP | BS_AUTOCHECKBOX, L + 160, B + 34, 340, 24,
        IDC_REPLACE);
    add(L"STATIC",
        L"Scan reads each map (the selected ones, or all): its gametypes, a player count guessed from its spawn points and "
        L"vehicles, and genre suggestions, for the maps that don't have them yet.",
        0, L + 160, B + 62, 650, 36, IDC_HINT);
    add(L"STATIC", L"", SS_ENDELLIPSIS, L + 510, B + 38, 300, 20, IDC_STATUS);
    add(L"BUTTON", L"Close", WS_TABSTOP, L + 826, B + 33, 94, 26, IDCANCEL);
}
int popup(int id, const std::vector<std::wstring> &items, int base) {
    HMENU m = CreatePopupMenu();
    for (size_t k = 0; k < items.size(); k++) AppendMenuW(m, MF_STRING, base + (int)k, items[k].c_str());
    RECT r;
    GetWindowRect(item(id), &r);
    int pick = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, 0, M->wnd, nullptr);
    DestroyMenu(m);
    return pick >= base && pick < base + (int)items.size() ? pick - base : -1;
}
void close() {
    if (M->scanning) {   // (stop the scan first: its results would have nowhere to go)
        M->stop = true;
        if (M->scanner.joinable()) M->scanner.join();
        MSG m;
        while (PeekMessageW(&m, M->wnd, WM_SCANNED, WM_SCAN_DONE, PM_REMOVE))
            if (m.message == WM_SCANNED) scanned((Scanned *)m.lParam);
        M->scanning = false;
    }
    if (!keep_changes_ok()) return;
    M->done = true;
}
LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        if (GetDlgCtrlID((HWND)lp) == IDC_HINT) SetTextColor((HDC)wp, RGB(96, 96, 96));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_SCANNED: scanned((Scanned *)lp); return 0;
    case WM_SCAN_DONE: if (M->scanning) scan_done(); return 0;
    case WM_NOTIFY: {
        auto *nh = (NMHDR *)lp;
        if (nh->idFrom == IDC_LIST && nh->code == LVN_ITEMCHANGED) status();
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        switch (id) {
        case IDC_DIR_BROWSE: {
            std::wstring d = browse_folder(text(IDC_DIR));
            if (!d.empty()) { SetWindowTextW(item(IDC_DIR), d.c_str()); do_load(); }
            return 0;
        }
        case IDC_STATS_BROWSE: {
            wchar_t buf[MAX_PATH] = L"hmp_stats.txt";
            std::wstring cur = text(IDC_STATS);
            if (!cur.empty() && cur.size() < MAX_PATH) wcscpy(buf, cur.c_str());
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof ofn;
            ofn.hwndOwner = h;
            ofn.lpstrFilter = L"hmp_stats.txt\0hmp_stats.txt\0Text files (*.txt)\0*.txt\0";
            ofn.lpstrFile = buf;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&ofn)) { SetWindowTextW(item(IDC_STATS), buf); if (M->loaded) do_load(); }
            return 0;
        }
        case IDC_LOAD: do_load(); return 0;
        case IDC_SAVE: do_save(); return 0;
        case IDC_ALL:
            ListView_SetItemState(lv(), -1, LVIS_SELECTED, LVIS_SELECTED);
            SetFocus(lv());
            return 0;
        case IDC_GENRE_ADD: case IDC_GENRE_DEL: {
            std::string g = N(text(IDC_GENRE));
            g.erase(0, g.find_first_not_of(' '));
            while (!g.empty() && g.back() == ' ') g.pop_back();
            if (g.empty() || g.find(',') != std::string::npos || g.find(':') != std::string::npos) {
                MessageBoxW(h, L"Type or pick one genre (no commas or colons).", L"Map list", MB_ICONINFORMATION);
                return 0;
            }
            on_selected([&](Row &r) { if (id == IDC_GENRE_ADD) add_genre(M->list, r, g); else remove_genre(M->list, r, g); });
            return 0;
        }
        case IDC_SCAN:
            if (M->scanning) M->stop = true;
            else start_scan();
            return 0;
        case IDC_FAV: {
            int k = popup(id, {L"Favorite", L"Not a favorite"}, kMenuFav);
            if (k >= 0) on_selected([&](Row &r) { set_fav(r, k == 0); });
            return 0;
        }
        case IDC_HIDE: on_selected([](Row &r) { set_hide(r, true); }); return 0;
        case IDC_SHOW: on_selected([](Row &r) { set_hide(r, false); }); return 0;
        case IDCANCEL: close(); return 0;
        }
        return 0;
    }
    case WM_CLOSE: close(); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
}   // namespace

void edit_map_list(HWND owner, std::wstring &maps_dir, std::wstring &stats_file, Analyzer analyze) {
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!registered) {
        INITCOMMONCONTROLSEX icc{sizeof icc, ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&icc);
        WNDCLASSEXW wc{sizeof wc};
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpfnWndProc = proc;
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wc.lpszClassName = L"HMPMapList";
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        RegisterClassExW(&wc);
        registered = true;
    }
    Win w;
    M = &w;
    w.owner = owner;
    w.dir = &maps_dir;
    w.stats = &stats_file;
    w.analyze = std::move(analyze);
    HDC dc = GetDC(nullptr);
    w.dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);
    w.font = CreateFontW(-MulDiv(9, w.dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    RECT r{0, 0, S(940), S(620)};
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    AdjustWindowRect(&r, style, FALSE);
    RECT orc;
    GetWindowRect(owner, &orc);
    int ww = r.right - r.left, hh = r.bottom - r.top;
    int x = orc.left + ((orc.right - orc.left) - ww) / 2, y = orc.top + ((orc.bottom - orc.top) - hh) / 2;
    MONITORINFO mi{sizeof mi};
    if (GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &mi)) {
        x = std::clamp(x, (int)mi.rcWork.left, std::max((int)mi.rcWork.left, (int)mi.rcWork.right - ww));
        y = std::clamp(y, (int)mi.rcWork.top, std::max((int)mi.rcWork.top, (int)mi.rcWork.bottom - hh));
    }
    w.wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"HMPMapList", L"Map list - players, gametypes, genres, favorites, hidden maps (Halo Universal XBE)",
                            style, x, y, ww, hh, owner, nullptr, inst, nullptr);
    create_controls();
    status();
    EnableWindow(owner, FALSE);
    ShowWindow(w.wnd, SW_SHOW);
    std::error_code ec;
    if (!maps_dir.empty() && std::filesystem::is_directory(std::filesystem::path(maps_dir), ec)) do_load();
    SetFocus(item(IDC_DIR));
    MSG m;
    while (!w.done) {
        BOOL got = GetMessageW(&m, nullptr, 0, 0);
        if (got <= 0) { PostQuitMessage(got == 0 ? (int)m.wParam : 0); break; }
        if (!IsDialogMessageW(w.wnd, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    DestroyWindow(w.wnd);
    DeleteObject(w.font);
    M = nullptr;
}
}   // namespace maplist
