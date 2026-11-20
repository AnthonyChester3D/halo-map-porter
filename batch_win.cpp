// Halo Map Porter - the batch window (batch_win.hpp).
#include "batch_win.hpp"
#include "dark.hpp"

#include <map>
#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <set>

namespace batchui {
namespace fs = std::filesystem;

namespace {

enum { IDC_INTRO = 400, IDC_STATS = 499, IDC_LIST, IDC_FIND, IDC_ADD, IDC_REMOVE, IDC_ALL, IDC_NONE, IDC_GO, IDC_STOP, IDC_CLOSE, IDC_STATUS,
       IDC_BROWSE, IDC_AT_ONCE_LBL, IDC_AT_ONCE, IDC_ROOT_LBL, IDC_ROOT, IDC_ROOT_BROWSE, IDC_PACKS_LBL, IDC_PACK_CROSS,
       IDC_PACK_CE, IDC_PACK_XBOX, IDC_WORKSHOP, IDC_PACK_MCC, IDC_SHOW_LBL, IDC_SHOW, IDC_SHOWN, IDC_EXTRAS };
// What the list shows ("Show:"), by each map's last run, game and kind
enum Show { SHOW_ALL, SHOW_TICKED, SHOW_UNTICKED, SHOW_DONE, SHOW_LINKED, SHOW_NOT_LINKED, SHOW_FAILED, SHOW_HEAVY, SHOW_FIXABLE,
            SHOW_NOT_RUN, SHOW_MCC, SHOW_CE, SHOW_MP, SHOW_CAMPAIGN, SHOW_COUNT };
const wchar_t *kShow[SHOW_COUNT] = {L"All maps", L"Ticked", L"Not ticked", L"Done (all)", L"Done, linked", L"Done, not linked",
                                    L"Failed (all)", L"Failed: too heavy for the Xbox", L"Failed: everything else",
                                    L"Not run yet (or stopped)", L"MCC maps", L"Custom Edition maps", L"Multiplayer", L"Campaign"};
const int kCols = 8;
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
    std::vector<std::wstring> packs;   // (each item's pack, as the hook says: refreshed by fill)
    std::vector<std::wstring> games, kinds;   // (each item's game and kind, read once a map)
    std::map<std::wstring, std::pair<std::wstring, std::wstring>> known;   // (by map: its game and kind)
    int show = SHOW_ALL;
    std::vector<int> rows, row_of;   // (the list's rows: each one's item; each item's row, -1 when it isn't shown)
    // (each control's place as laid out at the design size, 980 x 598, and how it follows the window's size: x 0 stays
    // left, 1 stays right, 2 stretches; y 0 stays at the top, 1 at the bottom, 2 stretches)
    struct Anchor { int id, x, y, w, h, ax, ay; };
    std::vector<Anchor> anchors;
};
State *St = nullptr;
HWND g_target = nullptr;   // (the open window, for progress from the worker)

int S(int v) { return MulDiv(v, St->dpi, 96); }
constexpr int kDesignW = 980, kDesignH = 598;
void layout();
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

// A last run's result, sorted: what the "Show:" filters go by.
enum Outcome { NOT_RUN, LINKED, NOT_LINKED, HEAVY, FIXABLE, SKIPPED };
Outcome outcome(const std::wstring &status) {
    const std::wstring s = lower(status);
    if (s.rfind(L"done, linked", 0) == 0) return LINKED;
    if (s.rfind(L"done", 0) == 0) return NOT_LINKED;
    if (s.rfind(L"skipped", 0) == 0) return SKIPPED;
    if (s.rfind(L"failed", 0) == 0 || s.rfind(L"protected", 0) == 0) {
        // (what no fix of the app's can bring under the Xbox's limits: its memory, its map size, its script nodes)
        for (const wchar_t *h : {L"needs more memory", L"too big for", L"script node"})
            if (s.find(h) != std::wstring::npos) return HEAVY;
        return FIXABLE;
    }
    return NOT_RUN;   // (never run, or waiting / converting when a run stopped)
}
bool shown(size_t i) {
    const Item &it = St->items[i];
    const Outcome o = outcome(it.status);
    switch (St->show) {
    case SHOW_TICKED: return it.on;
    case SHOW_UNTICKED: return !it.on;
    case SHOW_DONE: return o == LINKED || o == NOT_LINKED;
    case SHOW_LINKED: return o == LINKED;
    case SHOW_NOT_LINKED: return o == NOT_LINKED;
    case SHOW_FAILED: return o == HEAVY || o == FIXABLE;
    case SHOW_HEAVY: return o == HEAVY;
    case SHOW_FIXABLE: return o == FIXABLE;
    case SHOW_NOT_RUN: return o == NOT_RUN;
    case SHOW_MCC: return St->games[i] == L"MCC";
    case SHOW_CE: return St->games[i] == L"Custom Edition";
    case SHOW_MP: return St->kinds[i] == L"Multiplayer";
    case SHOW_CAMPAIGN: return St->kinds[i] == L"Campaign";
    default: return true;
    }
}
// Each item's game and kind (each map's header read once while the window's open).
void read_kinds() {
    St->games.resize(St->items.size());
    St->kinds.resize(St->items.size());
    for (size_t i = 0; i < St->items.size(); i++) {
        const std::wstring &in = St->items[i].in;
        auto k = St->known.find(in);
        if (k == St->known.end())
            k = St->known.emplace(in, std::make_pair(St->hooks.game_of ? St->hooks.game_of(in) : L"",
                                                     St->hooks.kind_of ? St->hooks.kind_of(in) : L"")).first;
        St->games[i] = k->second.first;
        St->kinds[i] = k->second.second;
    }
}

void fill() {
    filling = true;
    St->packs = St->hooks.packs ? St->hooks.packs(St->items) : std::vector<std::wstring>();
    St->packs.resize(St->items.size());
    read_kinds();
    St->rows.clear();
    St->row_of.assign(St->items.size(), -1);
    // (a pack's size beside its name, so the column says how many belong together)
    std::map<std::wstring, int> pack_n;
    for (auto &p : St->packs) if (!p.empty()) pack_n[lower(p)]++;
    SendMessageW(St->list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(St->list);
    for (size_t i = 0; i < St->items.size(); i++) {
        if (!shown(i)) continue;
        auto &it = St->items[i];
        std::wstring name = fs::path(it.in).filename().wstring(), game = St->games[i];
        LVITEMW lv{};
        lv.mask = LVIF_TEXT;
        lv.iItem = (int)St->rows.size();
        lv.pszText = (LPWSTR)name.c_str();
        int row = ListView_InsertItem(St->list, &lv);
        St->row_of[i] = row;
        St->rows.push_back((int)i);
        ListView_SetItemText(St->list, row, 1, (LPWSTR)game.c_str());
        ListView_SetItemText(St->list, row, 2, (LPWSTR)it.out.c_str());
        ListView_SetItemText(St->list, row, 3, (LPWSTR)it.size.c_str());
        ListView_SetItemText(St->list, row, 4, (LPWSTR)it.status.c_str());
        std::wstring card = St->hooks.card_of ? St->hooks.card_of(it) : L"";
        ListView_SetItemText(St->list, row, 5, (LPWSTR)card.c_str());
        std::wstring pack = St->packs[i].empty() ? L"" : St->packs[i] + L" (" + std::to_wstring(pack_n[lower(St->packs[i])]) + L")";
        ListView_SetItemText(St->list, row, 6, (LPWSTR)pack.c_str());
        ListView_SetItemText(St->list, row, 7, (LPWSTR)St->kinds[i].c_str());
        ListView_SetCheckState(St->list, row, it.on);
    }
    SendMessageW(St->list, WM_SETREDRAW, TRUE, 0);
    filling = false;
    if (St->items.empty()) SetWindowTextW(item(IDC_SHOWN), L"");
    else SetWindowTextW(item(IDC_SHOWN), (St->show == SHOW_ALL ? L"" : L"Showing " + std::to_wstring(St->rows.size()) + L" of " +
                                          std::to_wstring(St->items.size()) + L" (\"Tick all\" and \"Tick none\": these)").c_str());
}
// The item a row shows (-1 none).
int item_at(int row) { return row >= 0 && row < (int)St->rows.size() ? St->rows[(size_t)row] : -1; }

// A name's key with its numbers padded, so map2 comes before map10 (and a10, a30, b30... in order).
std::wstring natural(const std::wstring &s) {
    std::wstring k;
    for (size_t i = 0; i < s.size();) {
        if (iswdigit(s[i])) {
            size_t j = i;
            while (j < s.size() && iswdigit(s[j])) j++;
            std::wstring n = s.substr(i, j - i);
            while (n.size() > 1 && n[0] == L'0') n.erase(0, 1);
            k += std::wstring(n.size() < 12 ? 12 - n.size() : 0, L'0') + n;
            i = j;
        } else k += (wchar_t)towlower(s[i++]);
    }
    return k;
}

// Sorted by a column (clicked again: the other way): map, game, where it's saved, last run. By Type: multiplayer maps,
// then campaigns (each pack's chapters together, in order), then the rest.
void sort_by(int col) {
    if (St->running) return;
    St->sort_up = col == St->sort_col ? !St->sort_up : true;
    St->sort_col = col;
    std::vector<std::wstring> keys;
    for (size_t i = 0; i < St->items.size(); i++) {
        auto &it = St->items[i];
        std::wstring k = col == 7 ? std::wstring(St->kinds[i] == L"Multiplayer" ? L"1" : St->kinds[i] == L"Campaign" ? L"2" : L"3") +
                                        L"\x01" + natural(i < St->packs.size() ? St->packs[i] : L"")
                         : col == 6 ? (i < St->packs.size() ? St->packs[i] : L"")
                         : col == 0 ? fs::path(it.in).filename().wstring()
                         : col == 1 ? St->games[i]
                         : col == 2 ? it.out : col == 4 ? it.status
                         : col == 5 ? (St->hooks.card_of ? St->hooks.card_of(it) : L"") : L"";
        if (col == 3) {   // (by size: as a number, padded)
            wchar_t b[32];
            swprintf(b, 32, L"%012.2f", _wtof(it.size.c_str()));
            k = b;
        }
        keys.push_back(lower(k) + L"\x01" + natural(fs::path(it.in).filename().wstring()));
    }
    std::vector<size_t> order(St->items.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return St->sort_up ? keys[a] < keys[b] : keys[b] < keys[a]; });
    std::vector<Item> sorted;
    for (size_t i : order) sorted.push_back(St->items[i]);
    St->items.swap(sorted);
    read_kinds();
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
        St->hooks.layout_changed(text_of(IDC_ROOT), text_of(IDC_PACK_CROSS), text_of(IDC_PACK_CE), text_of(IDC_PACK_MCC), text_of(IDC_PACK_XBOX));
}

void update_buttons() {
    int ticked = 0;
    for (auto &it : St->items) ticked += it.on;
    for (int id : {IDC_FIND, IDC_ADD, IDC_BROWSE, IDC_WORKSHOP, IDC_REMOVE, IDC_ALL, IDC_NONE, IDC_CLOSE, IDC_AT_ONCE, IDC_ROOT, IDC_ROOT_BROWSE,
                   IDC_PACK_CROSS, IDC_PACK_CE, IDC_PACK_MCC, IDC_PACK_XBOX})
        EnableWindow(item(id), !St->running);
    EnableWindow(item(IDC_GO), !St->running && ticked > 0);
    EnableWindow(item(IDC_EXTRAS), !St->running && ticked > 0);
    ShowWindow(item(IDC_STOP), St->running ? SW_SHOW : SW_HIDE);
    ShowWindow(item(IDC_EXTRAS), St->running ? SW_HIDE : SW_SHOW);
    wchar_t b[200];
    if (!St->running)
    {
        int hidden = 0;
        for (size_t i = 0; i < St->items.size(); i++) hidden += St->items[i].on && i < St->row_of.size() && St->row_of[i] < 0;
        if (hidden) swprintf(b, 200, L"%d maps in the list, %d ticked (%d of them not shown).", (int)St->items.size(), ticked, hidden);
        else swprintf(b, 200, L"%d maps in the list, %d ticked.", (int)St->items.size(), ticked);
    }
    else
        b[0] = 0;
    if (!St->running) SetWindowTextW(item(IDC_STATUS), b);
}

void add_items(const std::vector<Item> &found, int &added) {
    std::set<std::wstring> have, ins;
    for (auto &it : St->items) have.insert(lower(it.out)), ins.insert(lower(it.in));
    for (Item f : found) {
        if (have.count(lower(f.out))) {
            if (ins.count(lower(f.in))) continue;   // (the same map: already in the list)
            // another map of the same name (two Workshop items' "precipice.map"): <name>_2, in a folder of its own
            // when it was to have one
            const fs::path o(f.out);
            const std::wstring stem = o.stem().wstring();
            const bool own = lower(o.parent_path().filename().wstring()) == lower(stem);
            for (int n = 2;; n++) {
                const std::wstring s = stem + L"_" + std::to_wstring(n);
                const fs::path c = own ? o.parent_path().parent_path() / s / (s + o.extension().wstring()) : o.parent_path() / (s + o.extension().wstring());
                if (!have.count(lower(c.wstring()))) { f.out = c.wstring(); break; }
            }
        }
        have.insert(lower(f.out));
        ins.insert(lower(f.in));
        St->items.push_back(f);
        added++;
    }
}

// Maps added from files (Browse..., MCC Workshop...), each where default_out puts it; the status says how many. `what`:
// what they are for the status (" maps" when empty), with what was left out after it.
void add_files(const std::vector<std::wstring> &files, const std::wstring &what) {
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
    std::wstring t = L"Added " + std::to_wstring(added) + L" of " + std::to_wstring(files.size()) + (what.empty() ? L" maps" : what);
    if (added < (int)found.size()) t += L" (" + std::to_wstring(found.size() - added) + L" already in the list)";
    if (unknown) t += L"; " + std::to_wstring(unknown) + L" left out: set \"Save as\" on the Convert tab first (new maps go beside it, each in a folder of its own)";
    SetWindowTextW(item(IDC_STATUS), (t + L".").c_str());
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
            if (m->out != L"\x01") St->items[(size_t)i].out = m->out;
            save();
            // (in its row when it's shown: the list isn't filtered again while it runs)
            const int r = i < (int)St->row_of.size() ? St->row_of[(size_t)i] : -1;
            if (r >= 0) {
                ListView_SetItemText(St->list, r, 2, (LPWSTR)St->items[(size_t)i].out.c_str());
                ListView_SetItemText(St->list, r, 3, (LPWSTR)St->items[(size_t)i].size.c_str());
                ListView_SetItemText(St->list, r, 4, (LPWSTR)St->items[(size_t)i].status.c_str());
                ListView_EnsureVisible(St->list, r, FALSE);
            }
            SetWindowTextW(item(IDC_STATUS), (fs::path(St->items[(size_t)i].in).filename().wstring() + L": " + *s).c_str());
        }
        return 0;
    }
    case WM_B_DONE: {
        std::unique_ptr<std::wstring> s((std::wstring *)lp);
        St->running = false;
        if (St->show != SHOW_ALL) fill();   // (what "Show:" picked, by the results now)
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
            int r = item_at(((NMITEMACTIVATE *)lp)->iItem);
            if (r >= 0) {
                St->hooks.use(St->items[(size_t)r]);
                St->closing = true;
            }
            return 0;
        }
        if (n->idFrom == IDC_LIST && n->code == NM_RCLICK) {   // (right click: its map card, or use it)
            const int row = ((NMITEMACTIVATE *)lp)->iItem, r = item_at(row);
            if (r >= 0) {
                HMENU m = CreatePopupMenu();
                if (St->hooks.use)
                    AppendMenuW(m, MF_STRING | (St->running ? MF_GRAYED : 0), 2, L"Use on the Convert tab");
                if (St->hooks.open_card) AppendMenuW(m, MF_STRING, 1, L"Open map card...");
                // (its pack's maps: ticked alone, or as well as what's ticked)
                const std::wstring pack = (size_t)r < St->packs.size() ? St->packs[(size_t)r] : L"";
                int pn = 0;
                for (auto &p : St->packs) pn += !pack.empty() && lower(p) == lower(pack);
                if (!pack.empty()) {
                    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
                    AppendMenuW(m, MF_STRING | (St->running ? MF_GRAYED : 0), 3,
                                (L"Tick only " + pack + L"'s " + std::to_wstring(pn) + L" maps").c_str());
                    AppendMenuW(m, MF_STRING | (St->running ? MF_GRAYED : 0), 4, (L"Tick " + pack + L"'s maps too").c_str());
                }
                POINT pt;
                GetCursorPos(&pt);
                int pick = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, nullptr);
                Item it = St->items[(size_t)r];
                if (pick == 1) {
                    St->hooks.open_card(h, it);
                    std::wstring card = St->hooks.card_of ? St->hooks.card_of(it) : L"";
                    ListView_SetItemText(St->list, row, 5, (LPWSTR)card.c_str());
                } else if (pick == 2 && !St->running) {
                    St->hooks.use(it);
                    St->closing = true;
                } else if ((pick == 3 || pick == 4) && !St->running) {
                    filling = true;
                    for (size_t i = 0; i < St->items.size(); i++) {
                        const bool in_pack = i < St->packs.size() && lower(St->packs[i]) == lower(pack);
                        if (pick == 3) St->items[i].on = in_pack;
                        else if (in_pack) St->items[i].on = true;
                        if (St->row_of[i] >= 0) ListView_SetCheckState(St->list, St->row_of[i], St->items[i].on);
                    }
                    filling = false;
                    save();
                    update_buttons();
                    SetWindowTextW(item(IDC_STATUS), (L"Ticked " + pack + L"'s " + std::to_wstring(pn) + L" maps" +
                                                      (pick == 3 ? L" (and only them)." : L".")).c_str());
                }
                DestroyMenu(m);
            }
            return 0;
        }
        if (n->idFrom == IDC_LIST && n->code == LVN_ITEMCHANGED && !filling) {
            NMLISTVIEW *v = (NMLISTVIEW *)lp;
            const int vi = item_at(v->iItem);
            if ((v->uChanged & LVIF_STATE) && vi >= 0) {
                bool on = ListView_GetCheckState(St->list, v->iItem) != 0;
                if (St->items[(size_t)vi].on != on) {
                    St->items[(size_t)vi].on = on;
                    // A tick box of one of several rows selected: every selected row the same way.
                    if (ListView_GetItemState(St->list, v->iItem, LVIS_SELECTED) & LVIS_SELECTED) {
                        filling = true;
                        for (int r = -1; (r = ListView_GetNextItem(St->list, r, LVNI_SELECTED)) >= 0;)
                            if (r != v->iItem && item_at(r) >= 0) {
                                St->items[(size_t)item_at(r)].on = on;
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
        case IDC_STATS:
            if (St->hooks.stats) St->hooks.stats(h);
            return 0;
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
            bool same_out = false;
            for (auto &e : St->items)
                if (lower(e.out) == lower(it.out)) e.in = it.in, same_out = true;   // (the same output: from this map now)
            if (!same_out) add_items({it}, added);
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
            add_files(files, L"");
            return 0;
        }
        case IDC_WORKSHOP: {
            if (!St->hooks.workshop) return 0;
            std::wstring note;
            HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
            SetWindowTextW(item(IDC_STATUS), L"Looking through the MCC Workshop's maps...");
            std::vector<std::wstring> files = St->hooks.workshop(h, note);
            SetCursor(old);
            if (files.empty()) {
                SetWindowTextW(item(IDC_STATUS), note.c_str());
                return 0;
            }
            add_files(files, L" Halo 1 maps from the MCC Workshop" + note);
            return 0;
        }
        case IDC_REMOVE: {
            std::vector<char> gone(St->items.size(), 0);
            for (int r = -1; (r = ListView_GetNextItem(St->list, r, LVNI_SELECTED)) >= 0;)
                if (item_at(r) >= 0) gone[(size_t)item_at(r)] = 1;
            std::vector<Item> keep;
            for (size_t i = 0; i < St->items.size(); i++)
                if (!gone[i]) keep.push_back(St->items[i]);
            St->items = keep;
            save();
            fill();
            update_buttons();
            return 0;
        }
        case IDC_ALL:
        case IDC_NONE:
            // (the maps shown: all of them, or what "Show:" picked)
            for (int i : St->rows) St->items[(size_t)i].on = LOWORD(wp) == IDC_ALL;
            save();
            fill();
            update_buttons();
            return 0;
        case IDC_GO:
        case IDC_EXTRAS: {
            const bool extras = LOWORD(wp) == IDC_EXTRAS;
            tell_layout();
            std::vector<int> which;
            std::vector<std::wstring> was;
            for (auto &it : St->items) was.push_back(it.status);
            for (int i = 0; i < (int)St->items.size(); i++)
                if (St->items[(size_t)i].on) {
                    which.push_back(i);
                    St->items[(size_t)i].status = L"Waiting";
                }   // (the others keep their last result)
            if (which.empty() || !(extras ? (bool)St->hooks.extras : (bool)St->hooks.start)) return 0;
            // (the rows as they are: what "Show:" picked stays in the list while it runs, "Waiting" or not)
            for (int i : which)
                if (St->row_of[(size_t)i] >= 0) ListView_SetItemText(St->list, St->row_of[(size_t)i], 4, (LPWSTR)St->items[(size_t)i].status.c_str());
            St->running = true;
            update_buttons();
            int at_once = (int)SendMessageW(item(IDC_AT_ONCE), CB_GETCURSEL, 0, 0) + 1;
            if (!(extras ? St->hooks.extras(St->items, which) : St->hooks.start(St->items, which, std::clamp(at_once, 1, kMaxAtOnce)))) {
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
        case IDC_ROOT: case IDC_PACK_CROSS: case IDC_PACK_CE: case IDC_PACK_MCC: case IDC_PACK_XBOX:
            if (HIWORD(wp) == EN_KILLFOCUS) tell_layout();
            return 0;
        case IDC_STOP:
            if (St->hooks.stop) St->hooks.stop();
            SetWindowTextW(item(IDC_STATUS), L"Stopping once the maps being converted are done...");
            EnableWindow(item(IDC_STOP), FALSE);
            return 0;
        case IDC_SHOW:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                St->show = std::clamp((int)SendMessageW(item(IDC_SHOW), CB_GETCURSEL, 0, 0), 0, SHOW_COUNT - 1);
                fill();
                update_buttons();
            }
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
    case WM_SIZE:
        if (St && St->list && wp != SIZE_MINIMIZED) layout();
        return 0;
    case WM_GETMINMAXINFO: {
        if (!St) break;
        RECT m{0, 0, S(kDesignW), S(kDesignH)};
        AdjustWindowRect(&m, (DWORD)GetWindowLongPtrW(h, GWL_STYLE), FALSE);
        auto *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize = {m.right - m.left, m.bottom - m.top};
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// The controls placed for the window's size now; the list's columns share the width it gained.
void layout() {
    RECT rc;
    GetClientRect(St->wnd, &rc);
    const int dw = std::max(0, (int)rc.right - S(kDesignW)), dh = std::max(0, (int)rc.bottom - S(kDesignH));
    HDWP d = BeginDeferWindowPos((int)St->anchors.size());
    for (auto &a : St->anchors) {
        HWND c = GetDlgItem(St->wnd, a.id);
        if (!c) continue;
        int x = S(a.x), y = S(a.y), w = S(a.w), h = S(a.h);
        if (a.ax == 1) x += dw; else if (a.ax == 2) w += dw;
        if (a.ay == 1) y += dh; else if (a.ay == 2) h += dh;
        if (d) d = DeferWindowPos(d, c, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (d) EndDeferWindowPos(d);
    // (Map, Saved to and Last run widen with the window)
    const int base[] = {170, 90, 236, 66, 252, 110, 140, 84};
    const double share[] = {0.15, 0, 0.35, 0, 0.2, 0.15, 0.15, 0};
    for (int i = 0; i < kCols; i++) ListView_SetColumnWidth(St->list, i, S(base[i]) + (int)(dw * share[i]));
    InvalidateRect(St->wnd, nullptr, TRUE);
}

HWND add(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), St->wnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)St->font, TRUE);
    // (the intro and status stretch; the bottom rows stay at the bottom, their right-hand controls at the right)
    int ax = 0, ay = y >= 470 ? 1 : 0;
    if (id == IDC_INTRO || id == IDC_STATUS || id == IDC_ROOT || id == IDC_SHOWN) ax = 2;   // (Save to grows; the packs stay right of it)
    if (id == IDC_ROOT_BROWSE || id == IDC_PACKS_LBL || id == IDC_PACK_CROSS || id == IDC_PACK_CE || id == IDC_PACK_MCC || id == IDC_PACK_XBOX) ax = 1;
    if (id == IDC_AT_ONCE_LBL || id == IDC_AT_ONCE || id == IDC_STOP || id == IDC_GO || id == IDC_CLOSE || id == IDC_EXTRAS) ax = 1;
    St->anchors.push_back({id, x, y, w, h, ax, ay});
    return c;
}

void create_controls() {
    add(L"STATIC",
        L"Converts the ticked maps for the Xbox one after another, with the Xbox tab's options as they are now (Perfect "
        L"Conversion included) and each map's own picks. With a \"Save to\" folder, everything for the Xbox goes in its "
        L"\"Copy to Xbox maps\" (copy what's in it into the Xbox's maps folder): maps on their own in the Crossplay pack (linked) "
        L"or the Xbox one, each pack (a campaign, an MCC item's maps) in its own; the PC maps they're linked to in \"PC side\". "
        L"A pack fails whole. Protected maps' originals move into \"Map Protected (Fail)\"; \"Batch report.csv\" lists them all.",
        SS_NOPREFIX, 16, 12, 948, 52, IDC_INTRO);
    St->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS,
                               S(16), S(102), S(948), S(368), St->wnd, (HMENU)(INT_PTR)IDC_LIST, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(St->list, WM_SETFONT, (WPARAM)St->font, TRUE);
    St->anchors.push_back({IDC_LIST, 16, 102, 948, 368, 2, 2});
    add(L"STATIC", L"Show:", SS_NOPREFIX, 16, 74, 44, 20, IDC_SHOW_LBL);
    HWND show = add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 62, 70, 220, 400, IDC_SHOW);
    for (auto *t : kShow) SendMessageW(show, CB_ADDSTRING, 0, (LPARAM)t);
    SendMessageW(show, CB_SETCURSEL, (WPARAM)St->show, 0);
    add(L"STATIC", L"", SS_NOPREFIX | SS_ENDELLIPSIS, 294, 74, 670, 20, IDC_SHOWN);
    ListView_SetExtendedListViewStyle(St->list, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    struct { const wchar_t *t; int w; } cols[] = {{L"Map", 170}, {L"From", 90}, {L"Saved to", 236}, {L"Size", 66}, {L"Last run", 252},
                                                  {L"Map card", 110}, {L"Pack", 140}, {L"Type", 84}};
    for (int i = 0; i < kCols; i++) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.fmt = i == 3 ? LVCFMT_RIGHT : LVCFMT_LEFT;
        c.pszText = (LPWSTR)cols[i].t;
        c.cx = S(cols[i].w);
        ListView_InsertColumn(St->list, i, &c);
    }
    // (the pack shown second, beside the map's name)
    int order[] = {0, 6, 7, 1, 2, 3, 4, 5};
    ListView_SetColumnOrderArray(St->list, kCols, order);
    add(L"BUTTON", L"Find my earlier conversions", WS_TABSTOP, 16, 480, 180, 28, IDC_FIND);
    add(L"BUTTON", L"Add the Convert tab's map", WS_TABSTOP, 204, 480, 170, 28, IDC_ADD);
    add(L"BUTTON", L"Browse...", WS_TABSTOP, 382, 480, 80, 28, IDC_BROWSE);
    add(L"BUTTON", L"MCC Workshop...", WS_TABSTOP, 470, 480, 116, 28, IDC_WORKSHOP);
    add(L"BUTTON", L"Remove selected", WS_TABSTOP, 594, 480, 116, 28, IDC_REMOVE);
    add(L"BUTTON", L"Tick all", WS_TABSTOP, 718, 480, 70, 28, IDC_ALL);
    add(L"BUTTON", L"Tick none", WS_TABSTOP, 796, 480, 78, 28, IDC_NONE);
    add(L"BUTTON", L"Map stats", WS_TABSTOP, 882, 480, 82, 28, IDC_STATS);
    // Where the maps go: one folder, and the map pack folders in it
    add(L"STATIC", L"Save to:", SS_NOPREFIX, 16, 522, 60, 20, IDC_ROOT_LBL);
    add(L"EDIT", St->hooks.root.c_str(), WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 78, 518, 200, 24, IDC_ROOT);
    add(L"BUTTON", L"...", WS_TABSTOP, 282, 517, 30, 26, IDC_ROOT_BROWSE);
    add(L"STATIC", L"Pack folders:", SS_NOPREFIX | SS_RIGHT, 316, 522, 92, 20, IDC_PACKS_LBL);
    add(L"EDIT", St->hooks.crossplay.c_str(), WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 412, 518, 130, 24, IDC_PACK_CROSS);
    add(L"EDIT", St->hooks.crossplay_ce.c_str(), WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 546, 518, 160, 24, IDC_PACK_CE);
    add(L"EDIT", St->hooks.crossplay_mcc.c_str(), WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 710, 518, 130, 24, IDC_PACK_MCC);
    add(L"EDIT", St->hooks.xbox.c_str(), WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 844, 518, 120, 24, IDC_PACK_XBOX);
    add(L"STATIC", L"", SS_NOPREFIX | SS_ENDELLIPSIS, 16, 560, 390, 20, IDC_STATUS);
    // (each map at once is an Invader build of its own: about one per two processor cores)
    add(L"STATIC", L"Maps at once:", SS_NOPREFIX | SS_RIGHT, 410, 560, 96, 20, IDC_AT_ONCE_LBL);
    HWND at = add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 512, 556, 64, 240, IDC_AT_ONCE);
    for (int i = 1; i <= kMaxAtOnce; i++) SendMessageW(at, CB_ADDSTRING, 0, (LPARAM)std::to_wstring(i).c_str());
    SendMessageW(at, CB_SETCURSEL, (WPARAM)(std::clamp(St->hooks.at_once, 1, kMaxAtOnce) - 1), 0);
    add(L"BUTTON", L"Stop after this map", WS_TABSTOP, 590, 554, 150, 30, IDC_STOP);
    // (in Stop's place while nothing runs)
    add(L"BUTTON", L"Showcase && bots only", WS_TABSTOP, 590, 554, 150, 30, IDC_EXTRAS);
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
    // (wider than its design size to start, as the monitor allows; any size down to that)
    RECT r{0, 0, S(1280), S(700)};
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN | WS_THICKFRAME | WS_MAXIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    RECT orc;
    GetWindowRect(owner, &orc);
    int w = r.right - r.left, hgt = r.bottom - r.top;
    int x = orc.left + ((orc.right - orc.left) - w) / 2, y = orc.top + ((orc.bottom - orc.top) - hgt) / 2;
    HMONITOR mon = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    if (GetMonitorInfoW(mon, &mi)) {
        RECT m{0, 0, S(kDesignW), S(kDesignH)};
        AdjustWindowRect(&m, style, FALSE);
        w = std::max((int)(m.right - m.left), std::min(w, (int)(mi.rcWork.right - mi.rcWork.left)));
        hgt = std::max((int)(m.bottom - m.top), std::min(hgt, (int)(mi.rcWork.bottom - mi.rcWork.top)));
        x = orc.left + ((orc.right - orc.left) - w) / 2;
        y = orc.top + ((orc.bottom - orc.top) - hgt) / 2;
        x = std::clamp(x, (int)mi.rcWork.left, std::max((int)mi.rcWork.left, (int)mi.rcWork.right - w));
        y = std::clamp(y, (int)mi.rcWork.top, std::max((int)mi.rcWork.top, (int)mi.rcWork.bottom - hgt));
    }
    st.wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"HMPBatch", L"Batch convert for the Xbox", style, x, y, w, hgt, owner,
                             nullptr, inst, nullptr);
    create_controls();
    layout();
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
