// Halo Map Porter - the batch window (batch_win.hpp).
#include "batch_win.hpp"
#include "dark.hpp"

#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <set>

namespace batchui {
namespace fs = std::filesystem;

namespace {

enum { IDC_INTRO = 400, IDC_LIST, IDC_FIND, IDC_ADD, IDC_REMOVE, IDC_ALL, IDC_NONE, IDC_GO, IDC_STOP, IDC_CLOSE, IDC_STATUS,
       IDC_BROWSE, IDC_AT_ONCE_LBL, IDC_AT_ONCE, IDC_ROOT_LBL, IDC_ROOT, IDC_ROOT_BROWSE, IDC_PACKS_LBL, IDC_PACK_CROSS,
       IDC_PACK_CE, IDC_PACK_XBOX };
struct Progress { std::wstring status, size, out; };
const UINT WM_B_PROGRESS = WM_APP + 60, WM_B_DONE = WM_APP + 61;

struct State {
    HWND wnd = nullptr, list = nullptr;
    int dpi = 96;
    HFONT font = nullptr;
    fs::path file;
    Hooks hooks;
    std::vector<Item> items;
    bool running = false, closing = false;
    int sort_col = -1;
    bool sort_up = true;
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
        size_t c = l.find('\t', b + 1);   // (then the last run's result, if any)
        Item it;
        it.on = l.substr(0, a) == "1";
        it.in = W(l.substr(a + 1, b - a - 1));
        it.out = W(l.substr(b + 1, c == std::string::npos ? std::string::npos : c - b - 1));
        if (c != std::string::npos) {   // (then its size)
            size_t d = l.find('\t', c + 1);
            it.status = W(l.substr(c + 1, d == std::string::npos ? std::string::npos : d - c - 1));
            if (d != std::string::npos) it.size = W(l.substr(d + 1));
        }
        St->items.push_back(it);
    }
}
void save() {
    std::error_code ec;
    fs::create_directories(St->file.parent_path(), ec);
    std::ofstream f(St->file, std::ios::binary | std::ios::trunc);
    for (auto &it : St->items) {
        std::string st = N(it.status);
        for (auto &ch : st)
            if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
        f << (it.on ? "1" : "0") << '\t' << N(it.in) << '\t' << N(it.out) << '\t' << st << '\t' << N(it.size) << '\n';
    }
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
        ListView_SetItemText(St->list, row, 3, (LPWSTR)it.size.c_str());
        ListView_SetItemText(St->list, row, 4, (LPWSTR)it.status.c_str());
        std::wstring card = St->hooks.card_of ? St->hooks.card_of(it) : L"";
        ListView_SetItemText(St->list, row, 5, (LPWSTR)card.c_str());
        ListView_SetCheckState(St->list, row, it.on);
    }
    SendMessageW(St->list, WM_SETREDRAW, TRUE, 0);
    filling = false;
}

// Sorted by a column (clicked again: the other way): map, game, where it's saved, last run.
void sort_by(int col) {
    if (St->running) return;
    St->sort_up = col == St->sort_col ? !St->sort_up : true;
    St->sort_col = col;
    std::vector<std::wstring> keys;
    for (auto &it : St->items) {
        std::wstring k = col == 0 ? fs::path(it.in).filename().wstring()
                         : col == 1 ? (St->hooks.game_of ? St->hooks.game_of(it.in) : L"")
                         : col == 2 ? it.out : col == 4 ? it.status
                         : col == 5 ? (St->hooks.card_of ? St->hooks.card_of(it) : L"") : L"";
        if (col == 3) {   // (by size: as a number, padded)
            wchar_t b[32];
            swprintf(b, 32, L"%012.2f", _wtof(it.size.c_str()));
            k = b;
        }
        keys.push_back(lower(k) + L"\x01" + lower(fs::path(it.in).filename().wstring()));
    }
    std::vector<size_t> order(St->items.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return St->sort_up ? keys[a] < keys[b] : keys[b] < keys[a]; });
    std::vector<Item> sorted;
    for (size_t i : order) sorted.push_back(St->items[i]);
    St->items.swap(sorted);
    save();
}

std::wstring text_of(int id) {
    wchar_t b[1024];
    GetWindowTextW(item(id), b, 1024);
    std::wstring s = b;
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
    return s;
}
void tell_layout() {
    if (St->hooks.layout_changed)
        St->hooks.layout_changed(text_of(IDC_ROOT), text_of(IDC_PACK_CROSS), text_of(IDC_PACK_CE), text_of(IDC_PACK_XBOX));
}

void update_buttons() {
    int ticked = 0;
    for (auto &it : St->items) ticked += it.on;
    for (int id : {IDC_FIND, IDC_ADD, IDC_BROWSE, IDC_REMOVE, IDC_ALL, IDC_NONE, IDC_CLOSE, IDC_AT_ONCE, IDC_ROOT, IDC_ROOT_BROWSE,
                   IDC_PACK_CROSS, IDC_PACK_CE, IDC_PACK_XBOX})
        EnableWindow(item(id), !St->running);
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
        std::unique_ptr<Progress> m((Progress *)lp);
        const std::wstring *s = &m->status;
        int i = (int)wp;
        if (St && i >= 0 && i < (int)St->items.size()) {
            St->items[(size_t)i].status = *s;
            if (m->size != L"\x01") St->items[(size_t)i].size = m->size;
            if (m->out != L"\x01") {
                St->items[(size_t)i].out = m->out;
                ListView_SetItemText(St->list, i, 2, (LPWSTR)St->items[(size_t)i].out.c_str());
            }
            save();
            ListView_SetItemText(St->list, i, 3, (LPWSTR)St->items[(size_t)i].size.c_str());
            ListView_SetItemText(St->list, i, 4, (LPWSTR)St->items[(size_t)i].status.c_str());
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
        return (LRESULT)dark::sysbrush(COLOR_WINDOW);
    case WM_NOTIFY: {
        NMHDR *n = (NMHDR *)lp;
        if (n->idFrom == IDC_LIST && n->code == LVN_COLUMNCLICK && !St->running) {
            sort_by(((NMLISTVIEW *)lp)->iSubItem);
            fill();
            return 0;
        }
        if (n->idFrom == IDC_LIST && n->code == NM_DBLCLK && St->hooks.use && !St->running) {   // (double click: use it)
            int r = ((NMITEMACTIVATE *)lp)->iItem;
            if (r >= 0 && r < (int)St->items.size()) {
                St->hooks.use(St->items[(size_t)r]);
                St->closing = true;
            }
            return 0;
        }
        if (n->idFrom == IDC_LIST && n->code == NM_RCLICK) {   // (right click: its map card, or use it)
            int r = ((NMITEMACTIVATE *)lp)->iItem;
            if (r >= 0 && r < (int)St->items.size()) {
                HMENU m = CreatePopupMenu();
                if (St->hooks.use)
                    AppendMenuW(m, MF_STRING | (St->running ? MF_GRAYED : 0), 2, L"Use on the Convert tab");
                if (St->hooks.open_card) AppendMenuW(m, MF_STRING, 1, L"Open map card...");
                POINT pt;
                GetCursorPos(&pt);
                int pick = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, nullptr);
                Item it = St->items[(size_t)r];
                if (pick == 1) {
                    St->hooks.open_card(h, it);
                    std::wstring card = St->hooks.card_of ? St->hooks.card_of(it) : L"";
                    if (r < (int)St->items.size()) ListView_SetItemText(St->list, r, 5, (LPWSTR)card.c_str());
                } else if (pick == 2 && !St->running) {
                    St->hooks.use(it);
                    St->closing = true;
                }
                DestroyMenu(m);
            }
            return 0;
        }
        if (n->idFrom == IDC_LIST && n->code == LVN_ITEMCHANGED && !filling) {
            NMLISTVIEW *v = (NMLISTVIEW *)lp;
            if ((v->uChanged & LVIF_STATE) && v->iItem >= 0 && v->iItem < (int)St->items.size()) {
                bool on = ListView_GetCheckState(St->list, v->iItem) != 0;
                if (St->items[(size_t)v->iItem].on != on) {
                    St->items[(size_t)v->iItem].on = on;
                    // A tick box of one of several rows selected: every selected row the same way.
                    if (ListView_GetItemState(St->list, v->iItem, LVIS_SELECTED) & LVIS_SELECTED) {
                        filling = true;
                        for (int r = -1; (r = ListView_GetNextItem(St->list, r, LVNI_SELECTED)) >= 0;)
                            if (r != v->iItem && r < (int)St->items.size()) {
                                St->items[(size_t)r].on = on;
                                ListView_SetCheckState(St->list, r, on);
                            }
                        filling = false;
                    }
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
        case IDC_BROWSE: {
            std::vector<wchar_t> buf(64 * 1024, 0);
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof ofn;
            ofn.hwndOwner = h;
            ofn.lpstrFilter = L"Halo maps (*.map)\0*.map\0";
            ofn.lpstrFile = buf.data();
            ofn.nMaxFile = (DWORD)buf.size();
            ofn.lpstrTitle = L"Maps to add to the batch (Ctrl or Shift to pick several)";
            ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (!GetOpenFileNameW(&ofn)) return 0;
            std::vector<std::wstring> files;
            std::wstring first = buf.data();
            const wchar_t *p = buf.data() + first.size() + 1;
            if (!*p) files.push_back(first);   // (one file: its full path)
            for (; *p; p += wcslen(p) + 1) files.push_back((fs::path(first) / p).wstring());
            std::vector<Item> found;
            int unknown = 0;
            HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
            for (auto &f : files) {
                std::wstring out = St->hooks.default_out ? St->hooks.default_out(f) : L"";
                if (out.empty()) { unknown++; continue; }
                found.push_back({f, out, true, L""});
            }
            SetCursor(old);
            int added = 0;
            add_items(found, added);
            save();
            fill();
            update_buttons();
            std::wstring t = L"Added " + std::to_wstring(added) + L" of " + std::to_wstring(files.size()) + L" maps";
            if (added < (int)found.size()) t += L" (" + std::to_wstring(found.size() - added) + L" already in the list)";
            if (unknown) t += L"; " + std::to_wstring(unknown) + L" left out: set \"Save as\" on the Convert tab first (new maps go beside it, each in a folder of its own)";
            SetWindowTextW(item(IDC_STATUS), (t + L".").c_str());
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
            tell_layout();
            std::vector<int> which;
            std::vector<std::wstring> was;
            for (auto &it : St->items) was.push_back(it.status);
            for (int i = 0; i < (int)St->items.size(); i++)
                if (St->items[(size_t)i].on) {
                    which.push_back(i);
                    St->items[(size_t)i].status = L"Waiting";
                }   // (the others keep their last result)
            if (which.empty() || !St->hooks.start) return 0;
            fill();
            St->running = true;
            update_buttons();
            int at_once = (int)SendMessageW(item(IDC_AT_ONCE), CB_GETCURSEL, 0, 0) + 1;
            if (!St->hooks.start(St->items, which, std::clamp(at_once, 1, kMaxAtOnce))) {
                St->running = false;
                for (size_t i = 0; i < St->items.size(); i++) St->items[i].status = was[i];
                fill();
                update_buttons();
            }
            return 0;
        }
        case IDC_ROOT_BROWSE:
            if (St->hooks.pick_folder) {
                std::wstring f = St->hooks.pick_folder(St->wnd, text_of(IDC_ROOT));
                if (!f.empty()) {
                    SetWindowTextW(item(IDC_ROOT), f.c_str());
                    tell_layout();
                }
            }
            return 0;
        case IDC_ROOT: case IDC_PACK_CROSS: case IDC_PACK_CE: case IDC_PACK_XBOX:
            if (HIWORD(wp) == EN_KILLFOCUS) tell_layout();
            return 0;
        case IDC_STOP:
            if (St->hooks.stop) St->hooks.stop();
            SetWindowTextW(item(IDC_STATUS), L"Stopping once the maps being converted are done...");
            EnableWindow(item(IDC_STOP), FALSE);
            return 0;
        case IDC_AT_ONCE:
            if (HIWORD(wp) == CBN_SELCHANGE && St->hooks.at_once_changed)
                St->hooks.at_once_changed((int)SendMessageW(item(IDC_AT_ONCE), CB_GETCURSEL, 0, 0) + 1);
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
        L"Converts the ticked maps for the Xbox one after another, with the Xbox tab's options as they are now (Perfect "
        L"Conversion included) and each map's own picks. With a \"Save to\" folder, linked pairs go in its first map pack "
        L"(their Custom Edition maps copied into the second), maps that aren't linked in the third, protected maps' originals "
        L"move into \"Map Protected (Fail)\" beside them, other failures leave nothing; \"Batch report.csv\" lists them all.",
        SS_NOPREFIX, 16, 12, 948, 52, IDC_INTRO);
    St->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS,
                               S(16), S(70), S(948), S(400), St->wnd, (HMENU)(INT_PTR)IDC_LIST, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(St->list, WM_SETFONT, (WPARAM)St->font, TRUE);
    ListView_SetExtendedListViewStyle(St->list, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    struct { const wchar_t *t; int w; } cols[] = {{L"Map", 170}, {L"From", 90}, {L"Saved to", 236}, {L"Size", 66}, {L"Last run", 252},
                                                  {L"Map card", 110}};
    for (int i = 0; i < 6; i++) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.fmt = i == 3 ? LVCFMT_RIGHT : LVCFMT_LEFT;
        c.pszText = (LPWSTR)cols[i].t;
        c.cx = S(cols[i].w);
        ListView_InsertColumn(St->list, i, &c);
    }
    add(L"BUTTON", L"Find my earlier conversions", WS_TABSTOP, 16, 480, 210, 28, IDC_FIND);
    add(L"BUTTON", L"Add the Convert tab's map", WS_TABSTOP, 234, 480, 190, 28, IDC_ADD);
    add(L"BUTTON", L"Browse...", WS_TABSTOP, 432, 480, 100, 28, IDC_BROWSE);
    add(L"BUTTON", L"Remove selected", WS_TABSTOP, 540, 480, 130, 28, IDC_REMOVE);
    add(L"BUTTON", L"Tick all", WS_TABSTOP, 678, 480, 90, 28, IDC_ALL);
    add(L"BUTTON", L"Tick none", WS_TABSTOP, 776, 480, 90, 28, IDC_NONE);
    // Where the maps go: one folder, and the map pack folders in it
    add(L"STATIC", L"Save to:", SS_NOPREFIX, 16, 522, 60, 20, IDC_ROOT_LBL);
    add(L"EDIT", St->hooks.root.c_str(), WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 78, 518, 270, 24, IDC_ROOT);
    add(L"BUTTON", L"...", WS_TABSTOP, 352, 517, 30, 26, IDC_ROOT_BROWSE);
    add(L"STATIC", L"Packs (linked, their CE maps, not linked):", SS_NOPREFIX | SS_RIGHT, 388, 522, 140, 20, IDC_PACKS_LBL);
    add(L"EDIT", St->hooks.crossplay.c_str(), WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 532, 518, 130, 24, IDC_PACK_CROSS);
    add(L"EDIT", St->hooks.crossplay_ce.c_str(), WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 666, 518, 180, 24, IDC_PACK_CE);
    add(L"EDIT", St->hooks.xbox.c_str(), WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 850, 518, 114, 24, IDC_PACK_XBOX);
    add(L"STATIC", L"", SS_NOPREFIX | SS_ENDELLIPSIS, 16, 560, 390, 20, IDC_STATUS);
    // (each map at once is an Invader build of its own: about one per two processor cores)
    add(L"STATIC", L"Maps at once:", SS_NOPREFIX | SS_RIGHT, 410, 560, 96, 20, IDC_AT_ONCE_LBL);
    HWND at = add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 512, 556, 64, 240, IDC_AT_ONCE);
    for (int i = 1; i <= kMaxAtOnce; i++) SendMessageW(at, CB_ADDSTRING, 0, (LPARAM)std::to_wstring(i).c_str());
    SendMessageW(at, CB_SETCURSEL, (WPARAM)(std::clamp(St->hooks.at_once, 1, kMaxAtOnce) - 1), 0);
    add(L"BUTTON", L"Stop after this map", WS_TABSTOP, 590, 554, 150, 30, IDC_STOP);
    add(L"BUTTON", L"Convert ticked", WS_TABSTOP | BS_DEFPUSHBUTTON, 748, 554, 120, 30, IDC_GO);
    add(L"BUTTON", L"Close", WS_TABSTOP, 876, 554, 88, 30, IDC_CLOSE);
}

}   // namespace

void progress(int index, const std::wstring &status, const std::wstring &size, const std::wstring &out) {
    if (HWND t = g_target)
        PostMessageW(t, WM_B_PROGRESS, (WPARAM)index, (LPARAM) new Progress{status, size, out});
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
        wc.hbrBackground = dark::sysbrush(COLOR_WINDOW);
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
    RECT r{0, 0, S(980), S(598)};
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
