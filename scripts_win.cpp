// Halo Map Porter - the scripts window (see scripts_win.hpp)
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "scripts_win.hpp"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <memory>
#include <thread>

namespace scriptsui {
namespace {
namespace fs = std::filesystem;
using porter::ScriptFile;

enum {
    IDC_VIEW_LBL = 3301, IDC_VIEW, IDC_FILE_LBL, IDC_FILE, IDC_STATE, IDC_TEXT, IDC_NOTES, IDC_FIND_LBL, IDC_FIND,
    IDC_FIND_NEXT, IDC_LINE_LBL, IDC_LINE, IDC_GO, IDC_POS, IDC_CHECK, IDC_REVERT, IDC_FOLDER, IDC_SAVE, IDC_CLOSE,
};
enum View { kMine = 0, kXbox = 1, kMap = 2 };
constexpr UINT WM_LOADED = WM_APP + 50;
const char *const kNewFile = "scripts.hsc";   // (a map without scripts gets one to start)

struct Loaded { bool ok = false; std::vector<ScriptFile> files; std::string err; std::vector<std::string> notes; };

std::wstring W(const std::string &s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) {   // (not UTF-8: as Windows-1252)
        n = MultiByteToWideChar(1252, 0, s.data(), (int)s.size(), nullptr, 0);
        std::wstring w(n, 0);
        MultiByteToWideChar(1252, 0, s.data(), (int)s.size(), &w[0], n);
        return w;
    }
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
// Line ends as kept (\n) and as the box shows them (\r\n).
std::string no_cr(std::string s) {
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}
std::wstring crlf(const std::string &s) {
    std::wstring w = W(no_cr(s)), out;
    out.reserve(w.size() + w.size() / 20);
    for (wchar_t c : w) {
        if (c == L'\n') out += L'\r';
        out += c;
    }
    return out;
}
std::vector<ScriptFile> clean(std::vector<ScriptFile> v) {
    for (auto &f : v) f.text = no_cr(f.text);
    return v;
}
bool same(const std::vector<ScriptFile> &a, const std::vector<ScriptFile> &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (a[i].name != b[i].name || a[i].text != b[i].text) return false;
    return true;
}
bool blank(const std::vector<ScriptFile> &v) {
    for (auto &f : v)
        if (f.text.find_first_not_of(" \t\r\n") != std::string::npos) return false;
    return true;
}

// The edits kept in `dir` (its .hsc files), by name.
std::vector<ScriptFile> read_kept(const fs::path &dir) {
    std::vector<ScriptFile> v;
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::wstring ext = it->path().extension().wstring();
        for (auto &c : ext) c = (wchar_t)towlower(c);
        if (!it->is_regular_file(ec) || ext != L".hsc") continue;
        std::ifstream f(it->path(), std::ios::binary);
        v.push_back({it->path().filename().u8string(), no_cr(std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()))});
    }
    std::sort(v.begin(), v.end(), [](const ScriptFile &a, const ScriptFile &b) { return a.name < b.name; });
    return v;
}

struct Win {
    HWND wnd = nullptr, owner = nullptr;
    HFONT font = nullptr, mono = nullptr;
    int dpi = 96;
    fs::path dir;
    Loader load;
    Converter convert;
    std::vector<ScriptFile> mine;    // your scripts as edited here (the shown file's text is the box's until synced)
    std::vector<ScriptFile> saved;   // as kept: your edits on disk, else the map's own
    std::vector<ScriptFile> map;     // the map's own, once read
    bool have_map = false, loading = true, has_edits = false, edited = false, done = false, filling = false;
    std::string map_error;
    std::vector<std::string> map_notes;   // (old network tricks in the map's own scripts)
    int view = kMine, file = 0;
    std::shared_ptr<std::atomic<HWND>> post_to;   // where the loader's thread says it's done (null once closed)
};
Win *M = nullptr;
int S(int v) { return MulDiv(v, M->dpi, 96); }
HWND item(int id) { return GetDlgItem(M->wnd, id); }
HWND box() { return item(IDC_TEXT); }
std::wstring text(int id) {
    int n = GetWindowTextLengthW(item(id));
    std::wstring s(n + 1, 0);
    GetWindowTextW(item(id), &s[0], n + 1);
    s.resize(n);
    return s;
}
HWND add(const wchar_t *cls, const wchar_t *t, DWORD style, int id, DWORD ex = 0, HFONT f = nullptr) {
    HWND c = CreateWindowExW(ex, cls, t, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, M->wnd, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(f ? f : M->font), TRUE);
    return c;
}
void place(int id, int x, int y, int w, int h) { MoveWindow(item(id), x, y, std::max(w, 0), std::max(h, 0), FALSE); }

void layout() {
    RECT rc;
    GetClientRect(M->wnd, &rc);
    int W = rc.right, H = rc.bottom, g = S(10), row = S(24), by = H - g - S(26);
    place(IDC_VIEW_LBL, g, g + S(4), S(40), S(20));
    place(IDC_VIEW, g + S(42), g, S(300), S(200));
    place(IDC_FILE_LBL, g + S(356), g + S(4), S(30), S(20));
    place(IDC_FILE, g + S(388), g, S(190), S(200));
    place(IDC_STATE, g + S(592), g + S(4), W - g - (g + S(592)), S(20));
    int top = g + row + S(8), notes_h = S(92);
    place(IDC_TEXT, g, top, W - 2 * g, by - S(8) - notes_h - S(6) - top);
    place(IDC_NOTES, g, by - S(8) - notes_h, W - 2 * g, notes_h);
    place(IDC_FIND_LBL, g, by + S(4), S(34), S(20));
    place(IDC_FIND, g + S(36), by + S(1), S(160), S(24));
    place(IDC_FIND_NEXT, g + S(200), by, S(80), S(26));
    place(IDC_LINE_LBL, g + S(292), by + S(4), S(32), S(20));
    place(IDC_LINE, g + S(326), by + S(1), S(54), S(24));
    place(IDC_GO, g + S(384), by, S(40), S(26));
    place(IDC_POS, g + S(434), by + S(4), S(120), S(20));
    int x = W - g;
    const std::pair<int, int> right[] = {{IDC_CLOSE, 70}, {IDC_SAVE, 70}, {IDC_FOLDER, 96}, {IDC_REVERT, 124}, {IDC_CHECK, 70}};
    for (auto &[id, w] : right) {
        x -= S(w);
        place(id, x, by, S(w), S(26));
        x -= S(6);
    }
    InvalidateRect(M->wnd, nullptr, TRUE);
}

// The box's text back into `mine` (your scripts' view only).
void sync() {
    if (M->view != kMine || M->file < 0 || M->file >= (int)M->mine.size()) return;
    int n = GetWindowTextLengthW(box());
    std::wstring s(n + 1, 0);
    GetWindowTextW(box(), &s[0], n + 1);
    s.resize(n);
    M->mine[(size_t)M->file].text = no_cr(N(s));
}
bool dirty() {
    sync();
    return !same(M->mine, M->saved);
}

void set_notes(const std::wstring &s) { SetWindowTextW(item(IDC_NOTES), s.c_str()); }
std::wstring join(const std::vector<std::string> &v) {
    std::wstring s;
    for (auto &l : v) s += (s.empty() ? L"" : L"\r\n") + W(l);
    return s;
}

// The converter's notes: what it changes, and (after an empty line) how they'll run on the Xbox
void split_notes(const std::vector<std::string> &n, std::vector<std::string> &changes, std::vector<std::string> &runs) {
    auto gap = std::find(n.begin(), n.end(), std::string());
    changes.assign(n.begin(), gap);
    runs.assign(gap == n.end() ? gap : gap + 1, n.end());
}

void update_pos() {
    DWORD a = 0, b = 0;
    SendMessageW(box(), EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
    int line = (int)SendMessageW(box(), EM_LINEFROMCHAR, b, 0);
    int col = (int)b - (int)SendMessageW(box(), EM_LINEINDEX, line, 0);
    SetWindowTextW(item(IDC_POS), (L"Line " + std::to_wstring(line + 1) + L", col " + std::to_wstring(col + 1)).c_str());
}

void state() {
    std::wstring s;
    if (M->view == kMine && M->edited) s = L"Changed, not saved yet.";
    else if (M->has_edits) s = L"Your edits are kept for this map: conversions compile them instead of the map's own.";
    else if (M->have_map) s = L"The map's own scripts, as it has them (no edits).";
    else if (M->loading) s = L"Reading the map's scripts...";
    else s = L"No edits.";
    SetWindowTextW(item(IDC_STATE), s.c_str());
    EnableWindow(item(IDC_REVERT), M->have_map && (M->has_edits || M->edited || !same(M->mine, M->map)));
    EnableWindow(item(IDC_FOLDER), M->has_edits);
    EnableWindow(item(IDC_SAVE), M->view == kMine && !M->mine.empty());
    EnableWindow(item(IDC_CHECK), !M->mine.empty());
}

// The file list (your scripts' names: the map's own when it has others).
void fill_files() {
    HWND cb = item(IDC_FILE);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    const auto &v = M->view == kMap ? M->map : M->mine;
    for (auto &f : v) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)W(f.name).c_str());
    if (M->file >= (int)v.size()) M->file = 0;
    SendMessageW(cb, CB_SETCURSEL, v.empty() ? -1 : M->file, 0);
    EnableWindow(cb, v.size() > 1);
}

// What the box shows: your scripts (to edit), the Xbox's version of them, or the map's own (both read-only).
void show() {
    M->filling = true;
    fill_files();
    std::wstring t, notes;
    bool ro = M->view != kMine;
    if (M->view == kMine) {
        if (M->mine.empty())
            notes = M->loading ? L"Reading the map's scripts..." : W(M->map_error);
        else {
            t = crlf(M->mine[(size_t)M->file].text);
            notes = L"Edit the scripts here. Save keeps them for this map: Convert (and Build, and the LODs window's Build "
                    L"the map) compiles them instead of the map's own, converted for the Xbox as the map's are (\"As the "
                    L"Xbox gets them\" shows how). The build checks them against the map's objects and tags, and says what's "
                    L"wrong with the line. Ctrl+F finds, F3 finds the next, Ctrl+G goes to a line, Ctrl+S saves.";
            if (!M->have_map && !M->map_error.empty() && !M->has_edits)
                notes = W(M->map_error) + L" You can write scripts for it here: Save keeps them, and conversions compile "
                        L"them into the map.";
        }
    } else if (M->view == kXbox) {
        std::vector<std::string> n, changes, runs;
        auto conv = M->mine.empty() ? std::vector<ScriptFile>() : M->convert(M->mine, n);
        if (M->file < (int)conv.size()) t = crlf(conv[(size_t)M->file].text);
        split_notes(n, changes, runs);
        notes = runs.empty() ? L"" : join(runs) + L"\r\n\r\n";   // (what they cost a tick first: it's what's looked for)
        notes += changes.empty() ? std::wstring(L"Nothing in them needs changing for the Xbox.")
                                 : L"What the Xbox build changes (read-only here: edit your scripts, and this follows):\r\n" + join(changes);
    } else {
        if (M->have_map && M->file < (int)M->map.size()) {
            t = crlf(M->map[(size_t)M->file].text);
            notes = L"The map's own scripts, as Invader gets them back out of the map (read-only).";
            if (!M->map_notes.empty()) notes += L"\r\n" + join(M->map_notes);
        } else {
            notes = M->loading ? L"Reading the map's scripts..." : W(M->map_error);
        }
    }
    SetWindowTextW(box(), t.c_str());
    SendMessageW(box(), EM_SETREADONLY, ro || M->mine.empty(), 0);
    SendMessageW(box(), EM_SETSEL, 0, 0);
    SendMessageW(box(), EM_SCROLLCARET, 0, 0);
    set_notes(notes);
    M->filling = false;
    update_pos();
    state();
}

void switch_to(int view, int file) {
    sync();
    M->view = view;
    M->file = file;
    SendMessageW(item(IDC_VIEW), CB_SETCURSEL, view, 0);
    show();
}

// Selects line `n` (1-based) of the box and shows it.
void go_line(int n) {
    int count = (int)SendMessageW(box(), EM_GETLINECOUNT, 0, 0);
    n = std::clamp(n, 1, std::max(count, 1));
    int a = (int)SendMessageW(box(), EM_LINEINDEX, n - 1, 0);
    if (a < 0) return;
    int len = (int)SendMessageW(box(), EM_LINELENGTH, a, 0);
    SendMessageW(box(), EM_SETSEL, a, a + len);
    // (a few lines above it in view, not at the very top)
    int first = (int)SendMessageW(box(), EM_GETFIRSTVISIBLELINE, 0, 0);
    SendMessageW(box(), EM_LINESCROLL, 0, std::max(0, n - 6) - first);
    SendMessageW(box(), EM_SCROLLCARET, 0, 0);
    SetFocus(box());
    update_pos();
}

void find_next() {
    std::wstring what = text(IDC_FIND);
    if (what.empty()) { SetFocus(item(IDC_FIND)); return; }
    int n = GetWindowTextLengthW(box());
    std::wstring all(n + 1, 0);
    GetWindowTextW(box(), &all[0], n + 1);
    all.resize(n);
    auto low = [](std::wstring s) { for (auto &c : s) c = (wchar_t)towlower(c); return s; };
    std::wstring l = low(all), w = low(what);
    DWORD a = 0, b = 0;
    SendMessageW(box(), EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
    size_t at = l.find(w, b);
    if (at == std::wstring::npos) at = l.find(w);   // (from the top again)
    if (at == std::wstring::npos) {
        MessageBeep(MB_ICONASTERISK);
        SetWindowTextW(item(IDC_POS), L"Not found");
        return;
    }
    SendMessageW(box(), EM_SETSEL, at, at + w.size());
    SendMessageW(box(), EM_SCROLLCARET, 0, 0);
    update_pos();
}

// What stops your scripts being read, by file ("" each when nothing does).
std::vector<std::string> problems() {
    sync();
    std::vector<std::string> v;
    for (auto &f : M->mine) v.push_back(porter::script_syntax_problem(f.text));
    return v;
}
// "Line 12: ..." -> 12 (0 if none).
int problem_line(const std::string &p) {
    return p.rfind("Line ", 0) == 0 ? atoi(p.c_str() + 5) : 0;
}

void check() {
    auto ps = problems();
    std::vector<std::string> lines;
    int bad = -1;
    for (size_t i = 0; i < ps.size(); i++)
        if (!ps[i].empty()) {
            lines.push_back((M->mine.size() > 1 ? M->mine[i].name + ": " : std::string()) + ps[i]);
            if (bad < 0) bad = (int)i;
        }
    if (bad >= 0) {
        switch_to(kMine, bad);
        if (int l = problem_line(ps[(size_t)bad])) go_line(l);
        set_notes(L"These can't be read as they are (the build would stop here):\r\n" + join(lines));
        return;
    }
    std::vector<std::string> n, changes, runs;
    M->convert(M->mine, n);
    split_notes(n, changes, runs);
    lines.push_back("Nothing stops them being read. The build compiles them against the map's objects, AI and tags, and "
                    "says what's wrong there (a name the map doesn't have, a wrong number of arguments), with the line.");
    if (!changes.empty()) {
        lines.push_back("For the Xbox, the build changes:");
        for (auto &l : changes) lines.push_back("  " + l);
    }
    for (auto &l : runs) lines.push_back(l);
    set_notes(join(lines));
}

// Keeps your scripts for this map (the folder), or none when they're the map's own. False if you didn't go ahead.
bool save() {
    auto ps = problems();
    for (size_t i = 0; i < ps.size(); i++)
        if (!ps[i].empty()) {
            std::wstring q = L"The scripts can't be read as they are:\n\n" + W((M->mine.size() > 1 ? M->mine[i].name + ": " : std::string()) + ps[i]) +
                             L"\n\nA conversion would stop at it. Save them anyway?";
            if (MessageBoxW(M->wnd, q.c_str(), L"Scripts", MB_YESNO | MB_ICONWARNING) != IDYES) {
                switch_to(kMine, (int)i);
                if (int l = problem_line(ps[i])) go_line(l);
                return false;
            }
            break;
        }
    std::error_code ec;
    bool none = M->have_map ? same(M->mine, M->map) : blank(M->mine);
    if (none) {
        fs::remove_all(M->dir, ec);
        M->has_edits = false;
        set_notes(M->have_map ? L"These are the map's own scripts: nothing kept, the map's own are used."
                              : L"Nothing to keep: no scripts.");
    } else {
        fs::create_directories(M->dir, ec);
        for (auto &f : read_kept(M->dir)) {   // (files no longer there)
            bool still = false;
            for (auto &m : M->mine) still |= m.name == f.name;
            if (!still) fs::remove(M->dir / fs::u8path(f.name), ec);
        }
        bool ok = !ec;
        for (auto &f : M->mine) {
            std::ofstream o(M->dir / fs::u8path(f.name), std::ios::binary | std::ios::trunc);
            o << f.text;
            ok &= (bool)o;
        }
        if (!ok) {
            MessageBoxW(M->wnd, (L"Couldn't write the scripts to " + M->dir.wstring()).c_str(), L"Scripts", MB_ICONERROR);
            return false;
        }
        M->has_edits = true;
        set_notes(L"Saved. The next conversion of this map compiles these instead of the map's own (Convert brings them "
                  L"into the tags it prepared, without preparing the map again).");
    }
    M->saved = M->mine;
    M->edited = false;
    state();
    return true;
}

void revert() {
    if (!M->have_map) return;
    if (MessageBoxW(M->wnd, L"Throw away your edits and use the map's own scripts again?", L"Scripts",
                    MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    std::error_code ec;
    fs::remove_all(M->dir, ec);
    M->has_edits = false;
    M->mine = M->saved = M->map;
    M->edited = false;
    M->view = kMine;
    M->file = 0;
    SendMessageW(item(IDC_VIEW), CB_SETCURSEL, kMine, 0);
    show();
    set_notes(L"Back to the map's own scripts: your edits are gone, and conversions use the map's.");
}

void close() {
    if (dirty()) {
        int a = MessageBoxW(M->wnd, L"Save your changes to the scripts?", L"Scripts", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (a == IDCANCEL) return;
        if (a == IDYES && !save()) return;
    }
    M->done = true;
}

void loaded(Loaded *l) {
    std::unique_ptr<Loaded> own(l);
    M->loading = false;
    M->map_notes = l->notes;
    if (l->ok) {
        M->have_map = true;
        M->map = clean(l->files);
    } else {
        M->map_error = l->err;
    }
    if (M->view == kMine && !M->mine.empty()) sync();   // (edits made while it read: kept)
    if (!M->has_edits && M->mine.empty()) {
        M->mine = M->have_map ? M->map : std::vector<ScriptFile>{{kNewFile, ""}};
        M->saved = M->mine;
        M->edited = false;
        M->file = 0;
    }
    show();
}

void create_controls() {
    add(L"STATIC", L"Show:", 0, IDC_VIEW_LBL);
    HWND v = add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST, IDC_VIEW);
    for (auto s : {L"Your scripts (edit them here)", L"As the Xbox gets them (converted, read-only)", L"The map's own (read-only)"})
        SendMessageW(v, CB_ADDSTRING, 0, (LPARAM)s);
    SendMessageW(v, CB_SETCURSEL, 0, 0);
    add(L"STATIC", L"File:", 0, IDC_FILE_LBL);
    add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, IDC_FILE);
    add(L"STATIC", L"", SS_ENDELLIPSIS, IDC_STATE);
    HWND t = add(L"EDIT", L"", WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL |
                                ES_WANTRETURN | ES_NOHIDESEL, IDC_TEXT, WS_EX_CLIENTEDGE, M->mono);
    SendMessageW(t, EM_SETLIMITTEXT, 0, 0);
    HWND n = add(L"EDIT", L"", WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, IDC_NOTES, WS_EX_CLIENTEDGE);
    SendMessageW(n, EM_SETLIMITTEXT, 0, 0);
    add(L"STATIC", L"Find:", 0, IDC_FIND_LBL);
    add(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, IDC_FIND, WS_EX_CLIENTEDGE);
    add(L"BUTTON", L"Find next", WS_TABSTOP | BS_PUSHBUTTON, IDC_FIND_NEXT);
    add(L"STATIC", L"Line:", 0, IDC_LINE_LBL);
    add(L"EDIT", L"", WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, IDC_LINE, WS_EX_CLIENTEDGE);
    add(L"BUTTON", L"Go", WS_TABSTOP | BS_PUSHBUTTON, IDC_GO);
    add(L"STATIC", L"", 0, IDC_POS);
    add(L"BUTTON", L"Check", WS_TABSTOP | BS_PUSHBUTTON, IDC_CHECK);
    add(L"BUTTON", L"Back to the map's", WS_TABSTOP | BS_PUSHBUTTON, IDC_REVERT);
    add(L"BUTTON", L"Open folder", WS_TABSTOP | BS_PUSHBUTTON, IDC_FOLDER);
    add(L"BUTTON", L"Save", WS_TABSTOP | BS_PUSHBUTTON, IDC_SAVE);
    add(L"BUTTON", L"Close", WS_TABSTOP | BS_PUSHBUTTON, IDC_CLOSE);
    layout();
}

LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (!M) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_SIZE: layout(); return 0;
    case WM_GETMINMAXINFO: {
        auto *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = S(1000);
        mm->ptMinTrackSize.y = S(420);
        return 0;
    }
    case WM_LOADED: loaded((Loaded *)lp); return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        switch (id) {
        case IDC_VIEW:
            if (code == CBN_SELCHANGE) switch_to((int)SendMessageW(item(IDC_VIEW), CB_GETCURSEL, 0, 0), M->file);
            return 0;
        case IDC_FILE:
            if (code == CBN_SELCHANGE) switch_to(M->view, (int)SendMessageW(item(IDC_FILE), CB_GETCURSEL, 0, 0));
            return 0;
        case IDC_TEXT:
            if (code == EN_CHANGE && !M->filling && M->view == kMine && !M->edited) {
                M->edited = true;
                state();
            }
            return 0;
        case IDC_FIND_NEXT: find_next(); return 0;
        case IDC_GO: {
            int n = _wtoi(text(IDC_LINE).c_str());
            if (n > 0) go_line(n);
            return 0;
        }
        case IDC_CHECK: check(); return 0;
        case IDC_REVERT: revert(); return 0;
        case IDC_FOLDER:
            if (M->has_edits) ShellExecuteW(M->wnd, L"open", M->dir.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        case IDC_SAVE: save(); return 0;
        case IDC_CLOSE:
        case IDCANCEL: close(); return 0;
        }
        break;
    }
    case WM_CTLCOLORSTATIC: {
        HWND c = (HWND)lp;
        HDC dc = (HDC)wp;
        if (c == box()) {   // (read-only: grey)
            SetBkColor(dc, GetSysColor(COLOR_3DFACE));
            return (LRESULT)GetSysColorBrush(COLOR_3DFACE);
        }
        if (c == item(IDC_NOTES)) {
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        SetBkMode(dc, TRANSPARENT);
        if (c == item(IDC_STATE) && M->view == kMine && M->edited) SetTextColor(dc, RGB(170, 70, 0));   // (not saved yet)
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_CLOSE: close(); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Keys the window takes before the dialog keys (Tab, Enter, Escape) do: true when used.
bool keys(const MSG &m) {
    if (m.message != WM_KEYDOWN) return false;
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    HWND f = GetFocus();
    if (m.wParam == VK_TAB && f == box() && !ctrl && !shift && M->view == kMine && !M->mine.empty()) {
        SendMessageW(box(), EM_REPLACESEL, TRUE, (LPARAM)L"    ");   // (the scripts indent with spaces)
        return true;
    }
    if (m.wParam == VK_RETURN && f == item(IDC_FIND)) { find_next(); return true; }
    if (m.wParam == VK_RETURN && f == item(IDC_LINE)) { SendMessageW(M->wnd, WM_COMMAND, IDC_GO, 0); return true; }
    if (m.wParam == VK_F3) { find_next(); return true; }
    if (ctrl && m.wParam == 'S') { if (M->view == kMine) save(); return true; }
    if (ctrl && m.wParam == 'F') {
        SetFocus(item(IDC_FIND));
        SendMessageW(item(IDC_FIND), EM_SETSEL, 0, -1);
        return true;
    }
    if (ctrl && m.wParam == 'G') {
        SetFocus(item(IDC_LINE));
        SendMessageW(item(IDC_LINE), EM_SETSEL, 0, -1);
        return true;
    }
    if (ctrl && m.wParam == 'A' && f == box()) { SendMessageW(box(), EM_SETSEL, 0, -1); return true; }
    return false;
}
}   // namespace

bool edit(HWND owner, const std::wstring &title, const std::filesystem::path &dir, Loader load, Converter convert) {
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc{sizeof wc};
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpfnWndProc = proc;
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wc.lpszClassName = L"HMPScripts";
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        RegisterClassExW(&wc);
        registered = true;
    }
    Win w;
    M = &w;
    w.owner = owner;
    w.dir = dir;
    w.load = std::move(load);
    w.convert = std::move(convert);
    w.mine = read_kept(dir);
    w.has_edits = !w.mine.empty();
    w.saved = w.mine;
    HDC dc = GetDC(nullptr);
    w.dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);
    w.font = CreateFontW(-MulDiv(9, w.dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    w.mono = CreateFontW(-MulDiv(10, w.dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                         FIXED_PITCH | FF_MODERN, L"Consolas");
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX | WS_CLIPCHILDREN;
    RECT r{0, 0, S(1100), S(720)};
    AdjustWindowRect(&r, style, FALSE);
    int ww = r.right - r.left, hh = r.bottom - r.top;
    RECT orc;
    GetWindowRect(owner, &orc);
    int x = orc.left + ((orc.right - orc.left) - ww) / 2, y = orc.top + ((orc.bottom - orc.top) - hh) / 2;
    MONITORINFO mi{sizeof mi};
    if (GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &mi)) {
        ww = std::min(ww, (int)(mi.rcWork.right - mi.rcWork.left));
        hh = std::min(hh, (int)(mi.rcWork.bottom - mi.rcWork.top));
        x = std::clamp(x, (int)mi.rcWork.left, std::max((int)mi.rcWork.left, (int)mi.rcWork.right - ww));
        y = std::clamp(y, (int)mi.rcWork.top, std::max((int)mi.rcWork.top, (int)mi.rcWork.bottom - hh));
    }
    w.wnd = CreateWindowExW(0, L"HMPScripts", title.c_str(), style, x, y, ww, hh, owner, nullptr, inst, nullptr);
    create_controls();
    show();

    // The map's own scripts, read on a thread (a few seconds: Invader reads the map).
    w.post_to = std::make_shared<std::atomic<HWND>>(w.wnd);
    {
        auto to = w.post_to;
        Loader ld = w.load;
        std::thread([to, ld] {
            auto *l = new Loaded;
            l->ok = ld(l->files, l->err, l->notes);
            HWND h = to->load();
            if (!h || !PostMessageW(h, WM_LOADED, 0, (LPARAM)l)) delete l;
        }).detach();
    }

    EnableWindow(owner, FALSE);
    ShowWindow(w.wnd, SW_SHOW);
    SetFocus(box());
    MSG m;
    while (!w.done) {
        BOOL got = GetMessageW(&m, nullptr, 0, 0);
        if (got <= 0) { PostQuitMessage(got == 0 ? (int)m.wParam : 0); break; }
        if (IsChild(w.wnd, m.hwnd) || m.hwnd == w.wnd) {
            if (keys(m)) continue;
            if (!IsDialogMessageW(w.wnd, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
            if (m.hwnd == box() && (m.message == WM_KEYUP || m.message == WM_LBUTTONUP || m.message == WM_KEYDOWN))
                update_pos();
        } else {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    w.post_to->store(nullptr);
    // (a result still on its way to the window is let go of with it)
    MSG left;
    while (PeekMessageW(&left, w.wnd, WM_LOADED, WM_LOADED, PM_REMOVE)) delete (Loaded *)left.lParam;
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    DestroyWindow(w.wnd);
    DeleteObject(w.font);
    DeleteObject(w.mono);
    bool edits = w.has_edits;
    M = nullptr;
    return edits;
}
}   // namespace scriptsui
