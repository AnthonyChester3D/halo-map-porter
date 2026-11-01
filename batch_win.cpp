// Halo Map Porter - the batch window (batch_win.hpp).
#include "batch_win.hpp"

#include <commctrl.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <set>

namespace batchui {
namespace fs = std::filesystem;

namespace {

enum { IDC_INTRO = 400, IDC_LIST, IDC_FIND, IDC_ADD, IDC_REMOVE, IDC_ALL, IDC_NONE, IDC_GO, IDC_STOP, IDC_CLOSE, IDC_STATUS };
const UINT WM_B_PROGRESS = WM_APP + 60, WM_B_DONE = WM_APP + 61;

struct State {
    HWND wnd = nullptr, list = nullptr;
    int dpi = 96;
    HFONT font = nullptr;
    fs::path file;
    Hooks hooks;
    std::vector<Item> items;
    bool running = false, closing = false;
};
State *St = nullptr;
HWND g_target = nullptr;   // (the open window, for progress from the worker)

int S(int v) { return MulDiv(v, St->dpi, 96); }
HWND item(int id) { return GetDlgItem(St->wnd, id); }

std::string N(const std::wstring &w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
std::wstring W(const std::string &s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
std::wstring lower(std::wstring s) {
    for (auto &c : s) c = (wchar_t)towlower(c);
    return s;
}

void load() {
    St->items.clear();
    std::ifstream f(St->file, std::ios::binary);
    std::string l;
    while (std::getline(f, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        size_t a = l.find('\t'), b = a == std::string::npos ? a : l.find('\t', a + 1);
        if (b == std::string::npos) continue;
        Item it;
        it.on = l.substr(0, a) == "1";
        it.in = W(l.substr(a + 1, b - a - 1));
        it.out = W(l.substr(b + 1));
        St->items.push_back(it);
    }
}
void save() {
    std::error_code ec;
    fs::create_directories(St->file.parent_path(), ec);
    std::ofstream f(St->file, std::ios::binary | std::ios::trunc);
    for (auto &it : St->items) f << (it.on ? "1" : "0") << '\t' << N(it.in) << '\t' << N(it.out) << '\n';
}

bool filling = false;   // (ticks set while filling aren't the user's)

void fill() {
    filling = true;
    SendMessageW(St->list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(St->list);
    for (size_t i = 0; i < St->items.size(); i++) {
        auto &it = St->items[i];
        std::wstring name = fs::path(it.in).filename().wstring(), game = St->hooks.game_of ? St->hooks.game_of(it.in) : L"";
        LVITEMW lv{};
        lv.mask = LVIF_TEXT;
        lv.iItem = (int)i;
        lv.pszText = (LPWSTR)name.c_str();
        int row = ListView_InsertItem(St->list, &lv);
        ListView_SetItemText(St->list, row, 1, (LPWSTR)game.c_str());
        ListView_SetItemText(St->list, row, 2, (LPWSTR)it.out.c_str());
        ListView_SetItemText(St->list, row, 3, (LPWSTR)it.status.c_str());
        ListView_SetCheckState(St->list, row, it.on);
    }
    SendMessageW(St->list, WM_SETREDRAW, TRUE, 0);
    filling = false;
}

void update_buttons() {
    int ticked = 0;
    for (auto &it : St->items) ticked += it.on;
    for (int id : {IDC_FIND, IDC_ADD, IDC_REMOVE, IDC_ALL, IDC_NONE, IDC_CLOSE}) EnableWindow(item(id), !St->running);
    EnableWindow(item(IDC_GO), !St->running && ticked > 0);
    ShowWindow(item(IDC_STOP), St->running ? SW_SHOW : SW_HIDE);
    wchar_t b[200];
    if (!St->running)
        swprintf(b, 200, L"%d maps in the list, %d ticked.", (int)St->items.size(), ticked);
    else
        b[0] = 0;
    if (!St->running) SetWindowTextW(item(IDC_STATUS), b);
}

void add_items(const std::vector<Item> &found, int &added) {
    std::set<std::wstring> have;
    for (auto &it : St->items) have.insert(lower(it.out));
    for (auto &f : found)
        if (have.insert(lower(f.out)).second) {
            St->items.push_back(f);
            added++;
        }
}

LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_B_PROGRESS: {
        std::unique_ptr<std::wstring> s((std::wstring *)lp);
        int i = (int)wp;
        if (St && i >= 0 && i < (int)St->items.size()) {
            St->items[(size_t)i].status = *s;
            ListView_SetItemText(St->list, i, 3, (LPWSTR)St->items[(size_t)i].status.c_str());
            ListView_EnsureVisible(St->list, i, FALSE);
            SetWindowTextW(item(IDC_STATUS), (fs::path(St->items[(size_t)i].in).filename().wstring() + L": " + *s).c_str());
        }
        return 0;
    }
    case WM_B_DONE: {
        std::unique_ptr<std::wstring> s((std::wstring *)lp);
        St->running = false;
        update_buttons();
        SetWindowTextW(item(IDC_STATUS), s->c_str());
        FLASHWINFO fi{sizeof fi, GetAncestor(h, GA_ROOTOWNER), FLASHW_ALL | FLASHW_TIMERNOFG, 0, 0};
        FlashWindowEx(&fi);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_NOTIFY: {
        NMHDR *n = (NMHDR *)lp;
        if (n->idFrom == IDC_LIST && n->code == LVN_ITEMCHANGED && !filling) {
            NMLISTVIEW *v = (NMLISTVIEW *)lp;
            if ((v->uChanged & LVIF_STATE) && v->iItem >= 0 && v->iItem < (int)St->items.size()) {
                bool on = ListView_GetCheckState(St->list, v->iItem) != 0;
                if (St->items[(size_t)v->iItem].on != on) {
                    St->items[(size_t)v->iItem].on = on;
                    save();
                    update_buttons();
                }
            }
        }
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_FIND: {
            if (!St->hooks.find) return 0;
            HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
            int added = 0;
            add_items(St->hooks.find(), added);
            SetCursor(old);
            save();
            fill();
            update_buttons();
            SetWindowTextW(item(IDC_STATUS), (added ? L"Found " + std::to_wstring(added) + L" more of your earlier conversions."
                                                    : std::wstring(L"No more earlier conversions found."))
                                                 .c_str());
            return 0;
        }
        case IDC_ADD: {
            Item it;
            if (!St->hooks.current || !St->hooks.current(it)) {
                SetWindowTextW(item(IDC_STATUS), L"Pick a map and where to save it on the Convert tab first.");
                return 0;
            }
            int added = 0;
            add_items({it}, added);
            if (!added)
                for (auto &e : St->items)
                    if (lower(e.out) == lower(it.out)) e.in = it.in;   // (the same output: from this map now)
            save();
            fill();
            update_buttons();
            return 0;
        }
        case IDC_REMOVE: {
            std::vector<Item> keep;
            for (int i = 0; i < (int)St->items.size(); i++)
                if (!(ListView_GetItemState(St->list, i, LVIS_SELECTED) & LVIS_SELECTED)) keep.push_back(St->items[(size_t)i]);
            St->items = keep;
            save();
            fill();
            update_buttons();
            return 0;
        }
        case IDC_ALL:
        case IDC_NONE:
            for (auto &it : St->items) it.on = LOWORD(wp) == IDC_ALL;
            save();
            fill();
            update_buttons();
            return 0;
        case IDC_GO: {
            std::vector<int> which;
            for (int i = 0; i < (int)St->items.size(); i++)
                if (St->items[(size_t)i].on) {
                    which.push_back(i);
                    St->items[(size_t)i].status = L"Waiting";
                } else {
                    St->items[(size_t)i].status.clear();
                }
            if (which.empty() || !St->hooks.start) return 0;
            fill();
            St->running = true;
            update_buttons();
            if (!St->hooks.start(St->items, which)) {
                St->running = false;
                for (auto &it : St->items) it.status.clear();
                fill();
                update_buttons();
            }
            return 0;
        }
        case IDC_STOP:
            if (St->hooks.stop) St->hooks.stop();
            SetWindowTextW(item(IDC_STATUS), L"Stopping after the map being converted...");
            EnableWindow(item(IDC_STOP), FALSE);
            return 0;
        case IDC_CLOSE:
        case IDCANCEL:
            if (!St->running) St->closing = true;
            return 0;
        }
        break;
    case WM_CLOSE:
        if (!St->running) St->closing = true;
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
    add(L"STATIC",
        L"Converts the ticked maps for the Xbox one after another, each to where it's saved, with the Xbox tab's options as "
        L"they are now (Perfect Conversion included) and each map's own picks: weapons & vehicles, Level, scripts. Every map's "
        L"report, pair check and link file are written as for a single conversion.",
        SS_NOPREFIX, 16, 12, 948, 52, IDC_INTRO);
    St->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS,
                               S(16), S(70), S(948), S(400), St->wnd, (HMENU)(INT_PTR)IDC_LIST, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(St->list, WM_SETFONT, (WPARAM)St->font, TRUE);
    ListView_SetExtendedListViewStyle(St->list, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    struct { const wchar_t *t; int w; } cols[] = {{L"Map", 190}, {L"From", 110}, {L"Saved to", 400}, {L"Last run", 220}};
    for (int i = 0; i < 4; i++) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = (LPWSTR)cols[i].t;
        c.cx = S(cols[i].w);
        ListView_InsertColumn(St->list, i, &c);
    }
    add(L"BUTTON", L"Find my earlier conversions", WS_TABSTOP, 16, 480, 210, 28, IDC_FIND);
    add(L"BUTTON", L"Add the Convert tab's map", WS_TABSTOP, 234, 480, 200, 28, IDC_ADD);
    add(L"BUTTON", L"Remove selected", WS_TABSTOP, 442, 480, 140, 28, IDC_REMOVE);
    add(L"BUTTON", L"Tick all", WS_TABSTOP, 590, 480, 90, 28, IDC_ALL);
    add(L"BUTTON", L"Tick none", WS_TABSTOP, 688, 480, 90, 28, IDC_NONE);
    add(L"STATIC", L"", SS_NOPREFIX | SS_ENDELLIPSIS, 16, 524, 560, 20, IDC_STATUS);
    add(L"BUTTON", L"Stop after this map", WS_TABSTOP, 590, 518, 150, 30, IDC_STOP);
    add(L"BUTTON", L"Convert ticked", WS_TABSTOP | BS_DEFPUSHBUTTON, 748, 518, 120, 30, IDC_GO);
    add(L"BUTTON", L"Close", WS_TABSTOP, 876, 518, 88, 30, IDC_CLOSE);
}

}   // namespace

void progress(int index, const std::wstring &status) {
    if (HWND t = g_target) PostMessageW(t, WM_B_PROGRESS, (WPARAM)index, (LPARAM) new std::wstring(status));
}
void done(const std::wstring &summary) {
    if (HWND t = g_target) PostMessageW(t, WM_B_DONE, 0, (LPARAM) new std::wstring(summary));
}

void open(HWND owner, const fs::path &list_file, Hooks hooks) {
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
        wc.lpszClassName = L"HMPBatch";
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
    st.file = list_file;
    st.hooks = std::move(hooks);
    load();
    RECT r{0, 0, S(980), S(562)};
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
    st.wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"HMPBatch", L"Batch convert for the Xbox", style, x, y, w, hgt, owner,
                             nullptr, inst, nullptr);
    create_controls();
    fill();
    update_buttons();
    if (st.items.empty()) SetWindowTextW(item(IDC_STATUS), L"Empty: \"Find my earlier conversions\" lists the maps you've converted.");
    g_target = st.wnd;
    EnableWindow(owner, FALSE);
    ShowWindow(st.wnd, SW_SHOW);
    MSG m;
    while (!st.closing) {
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
    St = nullptr;
}

}   // namespace batchui
