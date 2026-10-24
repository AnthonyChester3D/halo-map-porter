// Halo Map Porter - Win32 GUI
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <urlmon.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <tuple>
#include <thread>
#include <vector>

#include "core.hpp"
#include "contents_win.hpp"
#include "lodview_win.hpp"
#include "mapcard_win.hpp"
#include "maplist_win.hpp"
#include "mappack.hpp"
#include "shaders_win.hpp"

using namespace porter;

// ---------------------------------------------------------------- ids
enum {
    ID_TAB = 100,
    ID_LOG,
    ID_STATUS,
    ID_OPEN_FOLDER,
    ID_OPEN_REPORT,
    ID_SEE_LODS,
    // convert page
    ID_IN_LBL, ID_IN, ID_IN_BROWSE, ID_FROM_LBL, ID_FROM, ID_DETECT, ID_TO_LBL, ID_TO, ID_OUT_LBL, ID_OUT,
    ID_OUT_BROWSE, ID_SHARE, ID_CONVERT, ID_CONV_HINT,
    // build page
    ID_SCEN_LBL, ID_SCEN, ID_SCEN_BROWSE, ID_B_MCC, ID_B_MCC_OUT, ID_B_MCC_BROWSE, ID_B_CE, ID_B_CE_OUT,
    ID_B_CE_BROWSE, ID_B_XBOX, ID_B_XBOX_NOTE, ID_BUILD, ID_BUILD_HINT, ID_B_SHARE,
    // settings page
    ID_S_CE_LBL, ID_S_CE, ID_S_CE_BROWSE, ID_S_MCC_LBL, ID_S_MCC, ID_S_MCC_BROWSE, ID_S_EK_LBL, ID_S_EK,
    ID_S_EK_BROWSE, ID_S_INV_LBL, ID_S_INV, ID_S_INV_BROWSE, ID_S_INV_DL, ID_S_INV_STATUS, ID_S_HINT,
    // added for Xbox (each *_BROWSE directly follows its edit box)
    ID_B_XBOX_OUT, ID_B_XBOX_BROWSE, ID_S_XT_LBL, ID_S_XT, ID_S_XT_BROWSE, ID_S_XM_LBL, ID_S_XM, ID_S_XM_BROWSE, ID_B_XBOX_NAME_LBL, ID_B_XBOX_NAME, ID_XONLY, ID_XSHRINK, ID_B_XONLY, ID_B_XSHRINK,
    // Xbox tab
    ID_X_SHRINK, ID_X_VLBL, ID_X_V0_LBL, ID_X_V0, ID_X_V1_LBL, ID_X_V1, ID_X_V2_LBL, ID_X_V2, ID_X_VHINT,
    ID_X_WLBL, ID_X_W0_LBL, ID_X_W0, ID_X_W1_LBL, ID_X_W1, ID_X_WHINT,
    // Xbox map names
    ID_X_MAPLIST_LBL, ID_X_MAPLIST, ID_X_MAPLIST_HINT, ID_C_XNAME_LBL, ID_C_XNAME, ID_X_LIMIT_LBL, ID_X_LIMIT,
    ID_X_STOCK, ID_X_W2_LBL, ID_X_W2, ID_X_W2_HINT, ID_X_TEX_LBL, ID_X_TEX, ID_X_LM_LBL, ID_X_LM, ID_X_TARGET,
    // map cards (Halo Universal XBE map folders)
    ID_B_CARD, ID_C_CARD,
    // Halo Universal XBE: every placed vehicle spawns
    ID_X_ALLVEH,
    // the weapons & vehicles window
    ID_X_CONTENTS,
    // Map packs tab
    ID_PK_IN_LBL, ID_PK_IN, ID_PK_IN_BROWSE, ID_PK_NAME_LBL, ID_PK_NAME, ID_PK_LOGO_LBL, ID_PK_LOGO, ID_PK_LOGO_BROWSE,
    ID_PK_LOGO_NONE, ID_PK_LOGO_MENU, ID_PK_SIZE_LBL, ID_PK_SIZE, ID_PK_CORNER_LBL, ID_PK_CORNER, ID_PK_HINT, ID_PK_LIST, ID_PK_PIC,
    ID_PK_MNAME_LBL, ID_PK_MNAME, ID_PK_DESC, ID_PK_OUT_LBL, ID_PK_OUT, ID_PK_OUT_BROWSE, ID_PK_CREATE,
    // the Halo Universal XBE's map list settings (build 20)
    ID_PK_MAPLIST,
    // stock models' unused levels of detail; storing animations as keyframes
    ID_X_LODSTOCK, ID_X_ANIM_LBL, ID_X_ANIM,
    // making lower-detail versions, and the LOD library's folder (each *_BROWSE directly follows its edit box)
    ID_X_MAKELOD, ID_X_TRIM_LBL, ID_X_TRIM, ID_S_LOD_LBL, ID_S_LOD, ID_S_LOD_BROWSE, ID_S_LOD_HINT, ID_S_TOUR,
    // portals for levels made without them
    ID_X_PORTALS,
    // the level shaders window
    ID_X_SHADERS,
    // portals: a choice of three (was a tick box)
    ID_X_PORTALS_LBL,
};

#define WM_APP_LOG (WM_APP + 1)
#define WM_APP_DONE (WM_APP + 2)

static HWND g_wnd, g_tab, g_log;
static HFONT g_font, g_mono, g_bold;
static int g_dpi = 96;
static std::vector<int> g_pages[5];   // Convert, Build, Xbox, Map packs, Settings
static std::atomic<bool> g_busy{false};
static bool g_out_manual = false;
static std::wstring g_last_output, g_last_report, g_last_lods;
static bool g_last_was_build = false;   // (the LODs window's "Convert again": which one)
static int S(int v) { return MulDiv(v, g_dpi, 96); }

// ---------------------------------------------------------------- strings
static std::wstring W(const std::string &s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
static std::string N(const std::wstring &w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
static std::wstring get_text(int id) {
    HWND h = GetDlgItem(g_wnd, id);
    int n = GetWindowTextLengthW(h);
    std::wstring s(n, 0);
    GetWindowTextW(h, s.data(), n + 1);
    return s;
}
static void set_text(int id, const std::wstring &s) { SetDlgItemTextW(g_wnd, id, s.c_str()); }
static bool checked(int id) { return IsDlgButtonChecked(g_wnd, id) == BST_CHECKED; }
static void set_check(int id, bool v) { CheckDlgButton(g_wnd, id, v ? BST_CHECKED : BST_UNCHECKED); }

// ---------------------------------------------------------------- paths & settings
static fs::path appdata_dir(int csidl) {
    wchar_t buf[MAX_PATH];
    SHGetFolderPathW(nullptr, csidl, nullptr, 0, buf);
    fs::path p = fs::path(buf) / L"HaloMapPorter";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}
static fs::path ini_path() { return appdata_dir(CSIDL_APPDATA) / L"settings.ini"; }
static std::wstring ini_get(const wchar_t *key, const wchar_t *def = L"") {
    wchar_t buf[2048];
    GetPrivateProfileStringW(L"porter", key, def, buf, 2048, ini_path().c_str());
    return buf;
}
static void ini_set(const wchar_t *key, const std::wstring &v) {
    WritePrivateProfileStringW(L"porter", key, v.c_str(), ini_path().c_str());
}

static const struct { int id; const wchar_t *key; } kSaved[] = {
    {ID_S_CE, L"ce_dir"},       {ID_S_MCC, L"mcc_maps"},       {ID_S_EK, L"ek_dir"},
    {ID_S_INV, L"invader_dir"}, {ID_IN, L"last_input"},        {ID_SCEN, L"scenario"},
    {ID_B_MCC_OUT, L"b_mcc_out"}, {ID_B_CE_OUT, L"b_ce_out"}, {ID_B_XBOX_OUT, L"b_xbox_out"},
    {ID_S_XT, L"xbox_tags"},    {ID_S_XM, L"xbox_maps"},     {ID_S_LOD, L"lod_library"},
    {ID_PK_IN, L"pk_in"},       {ID_PK_OUT, L"pk_out"},        {ID_PK_LOGO, L"pk_logo"},
};

static int sel(int id);
static std::wstring chosen_map(int id);

static void save_settings() {
    for (auto &s : kSaved) ini_set(s.key, get_text(s.id));
    ini_set(L"b_mcc", checked(ID_B_MCC) ? L"1" : L"0");
    ini_set(L"b_ce", checked(ID_B_CE) ? L"1" : L"0");
    ini_set(L"b_xbox", checked(ID_B_XBOX) ? L"1" : L"0");
    ini_set(L"b_xbox_name", chosen_map(ID_B_XBOX_NAME));
    ini_set(L"x_maplist", std::to_wstring(std::max(0, sel(ID_X_MAPLIST))));
    ini_set(L"x_limit", std::to_wstring(sel(ID_X_LIMIT) == 1 ? 1 : 0));
    ini_set(L"x_stock", checked(ID_X_STOCK) ? L"1" : L"0");
    ini_set(L"x_allveh", checked(ID_X_ALLVEH) ? L"1" : L"0");
    ini_set(L"x_lodstock", checked(ID_X_LODSTOCK) ? L"1" : L"0");
    ini_set(L"x_makelod", checked(ID_X_MAKELOD) ? L"1" : L"0");
    ini_set(L"x_trim", std::to_wstring(std::max(0, sel(ID_X_TRIM))));
    ini_set(L"x_anim", std::to_wstring(std::max(0, sel(ID_X_ANIM))));
    ini_set(L"x_portals", std::to_wstring(std::max(0, sel(ID_X_PORTALS))));
    ini_set(L"x_tex2", std::to_wstring(sel(ID_X_TEX)));
    ini_set(L"x_lm", std::to_wstring(sel(ID_X_LM)));
    ini_set(L"x_w2", std::to_wstring(sel(ID_X_W2)));
    ini_set(L"share", checked(ID_SHARE) ? L"1" : L"0");
    ini_set(L"x_shrink", checked(ID_X_SHRINK) ? L"1" : L"0");
    ini_set(L"write_tour", checked(ID_S_TOUR) ? L"1" : L"0");
    ini_set(L"x_target", std::to_wstring(std::max(0, sel(ID_X_TARGET))));
    ini_set(L"x_v0", std::to_wstring(sel(ID_X_V0)));
    ini_set(L"x_v1", std::to_wstring(sel(ID_X_V1)));
    ini_set(L"x_v2", std::to_wstring(sel(ID_X_V2)));
    ini_set(L"x_w0", std::to_wstring(sel(ID_X_W0)));
    ini_set(L"x_w1", std::to_wstring(sel(ID_X_W1)));
    ini_set(L"pk_size", std::to_wstring(std::max(0, sel(ID_PK_SIZE))));
    ini_set(L"pk_corner", std::to_wstring(std::max(0, sel(ID_PK_CORNER))));
}

static Config make_config() {
    Config c;
    c.ce_dir = fs::path(get_text(ID_S_CE));
    c.mcc_maps_dir = fs::path(get_text(ID_S_MCC));
    c.ek_dir = fs::path(get_text(ID_S_EK));
    c.xbox_tags_dir = fs::path(get_text(ID_S_XT));
    c.xbox_maps_dir = fs::path(get_text(ID_S_XM));
    c.invader_dir = fs::path(get_text(ID_S_INV));
    c.cache_dir = appdata_dir(CSIDL_LOCAL_APPDATA);
    c.lod_library = fs::path(get_text(ID_S_LOD));
    return c;
}

static Game combo_game(int id) {
    int i = (int)SendDlgItemMessageW(g_wnd, id, CB_GETCURSEL, 0, 0);
    return i < 0 ? Game::Unknown : (Game)i;
}

static fs::path default_dir(Game g) {
    switch (g) {
    case Game::CE: return fs::path(get_text(ID_S_CE)) / L"maps";
    case Game::MCC: return fs::path(get_text(ID_S_MCC));
    default: return {};
    }
}

// ---------------------------------------------------------------- dialogs
static std::wstring pick_folder(const std::wstring &start) {
    std::wstring result;
    IFileOpenDialog *dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) {
        DWORD opts;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        if (!start.empty()) {
            IShellItem *item;
            if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
                dlg->SetFolder(item);
                item->Release();
            }
        }
        if (SUCCEEDED(dlg->Show(g_wnd))) {
            IShellItem *item;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR p;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { result = p; CoTaskMemFree(p); }
                item->Release();
            }
        }
        dlg->Release();
    }
    return result;
}

static std::wstring pick_file(const std::wstring &start, const wchar_t *filter, bool save, const wchar_t *defext) {
    wchar_t buf[MAX_PATH] = {};
    fs::path sp(start);
    std::wstring dir;
    if (!start.empty()) {
        if (fs::is_directory(sp)) dir = start;
        else { wcsncpy(buf, sp.filename().c_str(), MAX_PATH - 1); dir = sp.parent_path().wstring(); }
    }
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_wnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    ofn.lpstrDefExt = defext;
    ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    return ok ? buf : L"";
}

// ---------------------------------------------------------------- log
// While a job runs and the log has been quiet for a while, its last line says it's still working (and for how
// long); the next real line replaces it.
static const UINT_PTR kHeartbeat = 77;
static ULONGLONG g_last_log = 0;
static int g_hb_at = -1;   // where the "still working" line starts, or -1
static void clear_heartbeat() {
    if (g_hb_at < 0) return;
    SendMessageW(g_log, EM_SETSEL, g_hb_at, GetWindowTextLengthW(g_log));
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)L"");
    g_hb_at = -1;
}
static void append_log(const std::wstring &text) {
    clear_heartbeat();
    std::wstring t;
    for (wchar_t c : text) { if (c == L'\n') t += L"\r\n"; else t += c; }
    t += L"\r\n";
    int len = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, len, len);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)t.c_str());
    g_last_log = GetTickCount64();
}
static void heartbeat() {
    ULONGLONG quiet = (GetTickCount64() - g_last_log) / 1000;
    if (quiet < 15) return;
    wchar_t t[96];
    swprintf(t, 96, L"  ... still working (%llu:%02llu since the last line)", quiet / 60, quiet % 60);
    clear_heartbeat();
    g_hb_at = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, g_hb_at, g_hb_at);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)t);
}
static void post_log(const std::string &s) { PostMessageW(g_wnd, WM_APP_LOG, 0, (LPARAM) new std::wstring(W(s))); }

// ---------------------------------------------------------------- UI helpers
static HWND add(int page, const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id,
                DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | style, S(x), S(y), S(w), S(h), g_wnd, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    if (page >= 0) g_pages[page].push_back(id);
    else ShowWindow(c, SW_SHOW);
    return c;
}
static void label(int page, const wchar_t *t, int x, int y, int w, int id) { add(page, L"STATIC", t, 0, x, y + 3, w, 20, id); }
static void edit(int page, int x, int y, int w, int id) {
    add(page, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, x, y, w, 24, id, WS_EX_CLIENTEDGE);
}
static void button(int page, const wchar_t *t, int x, int y, int w, int id, bool def = false) {
    add(page, L"BUTTON", t, WS_TABSTOP | (def ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON), x, y, w, 26, id);
}
static void check(int page, const wchar_t *t, int x, int y, int w, int id) {
    add(page, L"BUTTON", t, WS_TABSTOP | BS_AUTOCHECKBOX, x, y, w, 22, id);
}
static void combo(int page, int x, int y, int w, int id) {
    HWND c = add(page, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, x, y, w, 200, id);
    for (auto s : {L"MCC (Halo CE Anniversary)", L"Halo Custom Edition", L"Xbox"})
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)s);
}

// Xbox tab choices (order = combo box order).
static const struct { const wchar_t *label; const char *tag; } kVehicleChoices[] = {
    {L"Xbox warthog", "vehicles/warthog/warthog.vehicle"},
    {L"PC warthog", "vehicles/warthog/mp_warthog.vehicle"},
    {L"Rocket warthog", "vehicles/rwarthog/rwarthog.vehicle"},
    {L"Ghost", "vehicles/ghost/ghost_mp.vehicle"},
    {L"Banshee", "vehicles/banshee/banshee_mp.vehicle"},
    {L"Scorpion", "vehicles/scorpion/scorpion_mp.vehicle"},
    {L"Covenant turret (untested)", "vehicles/c gun turret/c gun turret_mp.vehicle"},
};
static const int kVehicleDefaults[3] = {0, 3, 5};
static const struct { const wchar_t *label; const char *name; } kWeaponChoices[] = {
    {L"Keep it (works on Xbox)", ""},       {L"Assault rifle", "assault rifle"}, {L"Pistol", "pistol"},
    {L"Sniper rifle", "sniper rifle"},      {L"Rocket launcher", "rocket launcher"}, {L"Shotgun", "shotgun"},
    {L"Plasma pistol", "plasma pistol"},    {L"Plasma rifle", "plasma rifle"},   {L"Needler", "needler"},
};
static const char *kPcWeaponTags[2] = {"weapons/flamethrower/flamethrower.weapon", "weapons/plasma_cannon/plasma_cannon.weapon"};

static void list_combo(int page, int x, int y, int w, int id, const std::vector<const wchar_t *> &items) {
    HWND c = add(page, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, x, y, w, 300, id);
    for (auto s : items) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)s);
}
static int sel(int id) { return (int)SendDlgItemMessageW(g_wnd, id, CB_GETCURSEL, 0, 0); }

// Xbox map names. Halo on Xbox only loads a map whose file name matches its internal name, and the
// multiplayer menu only lists the names it knows, so a ported map has to take one of these names.
static const wchar_t *kStandardMaps[] = {
    L"beavercreek", L"bloodgulch", L"boardingaction", L"carousel", L"chillout", L"damnation", L"hangemhigh",
    L"longest",     L"prisoner",   L"putput",         L"ratrace",  L"sidewinder", L"wizard"};
// 27-map packs, in their menu order (the number shown is the menu position, not part of the name).
static const wchar_t *kExpandedMaps[] = {
    L"chillout", L"hangemhigh", L"prisoner", L"dammy",     L"derelict", L"downrush",       L"badcreek",
    L"decidia",  L"doubletake", L"redshift", L"outbnd",    L"exhibit",  L"hotbox",         L"ratrace",
    L"zerohour", L"wizard",     L"atlas",    L"bloodgulch", L"boardingaction", L"imminent", L"sidewinder",
    L"longest",  L"putput",     L"madhouse", L"calamity",  L"overflow", L"tinker"};
static std::wstring g_conv_own, g_build_own; // the input map's / scenario's own name

static bool expanded_list() { return sel(ID_X_MAPLIST) == 1; }
// Halo Universal XBE: every map in its own folder with a map card (name, description, picture).
static bool folder_mode() { return sel(ID_X_MAPLIST) == 2; }
static std::vector<std::wstring> map_names() {
    std::vector<std::wstring> v;
    if (expanded_list()) for (auto n : kExpandedMaps) v.push_back(n);
    else for (auto n : kStandardMaps) v.push_back(n);
    return v;
}
static const wchar_t *map_list_name() { return expanded_list() ? L"Expanded map pack" : L"Standard"; }
static int find_map(const std::wstring &name) {
    auto v = map_names();
    for (size_t i = 0; i < v.size(); i++)
        if (_wcsicmp(v[i].c_str(), name.c_str()) == 0) return (int)i;
    return -1;
}
static std::wstring lower(std::wstring s) {
    for (auto &c : s) c = towlower(c);
    return s;
}
// Selected name in an Xbox name combo; empty = keep the map's own name. Read from the item text
// ("21. longest" -> "longest") so it still works while the list is being switched.
static std::wstring chosen_map(int id) {
    int k = sel(id);
    if (k <= 0) return L"";
    wchar_t buf[256] = {};
    if (SendDlgItemMessageW(g_wnd, id, CB_GETLBTEXTLEN, k, 0) >= 255) return L"";
    SendDlgItemMessageW(g_wnd, id, CB_GETLBTEXT, k, (LPARAM)buf);
    std::wstring s = buf;
    size_t dot = s.find(L". ");
    if (dot != std::wstring::npos && dot > 0 && iswdigit(s[0])) s = s.substr(dot + 2);
    return s;
}
// (Re)fill an Xbox name combo: entry 0 keeps the map's own name, then the current list.
// `pick` is selected if it's in the list; otherwise entry 0, or nothing if `none_if_missing`.
static void fill_map_combo(int id, const std::wstring &own, const std::wstring &pick, bool none_if_missing = false) {
    HWND c = GetDlgItem(g_wnd, id);
    SendMessageW(c, CB_RESETCONTENT, 0, 0);
    std::wstring first = own.empty() ? L"(its own name)" : L"(own name: " + lower(own) + L")";
    SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)first.c_str());
    auto v = map_names();
    for (size_t i = 0; i < v.size(); i++) {
        std::wstring item = expanded_list() ? std::to_wstring(i) + L". " + v[i] : v[i];
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)item.c_str());
    }
    int k = pick.empty() ? 0 : find_map(pick) + 1;
    if (k == 0 && !pick.empty() && none_if_missing) k = -1;
    SendMessageW(c, CB_SETCURSEL, k, 0);
}

static void apply_xbox_options(Config &cfg) {
    cfg.xbox_shrink = checked(ID_X_SHRINK);
    cfg.write_perf_tour = checked(ID_S_TOUR);
    cfg.xbox_max_mb = sel(ID_X_LIMIT) == 1 ? 115 : 0; // 0 = standard 47 MB
    {
        static const double target[3] = {22, 20, 18};   // the texture memory target: Max, Recommended, Safe
        int k = sel(ID_X_TARGET);
        cfg.xbox_texture_target_mb = target[k >= 0 && k < 3 ? k : 1];
    }
    cfg.xbox_use_stock_versions = checked(ID_X_STOCK);
    cfg.xbox_keep_all_vehicles = folder_mode() && checked(ID_X_ALLVEH);
    cfg.xbox_lod_stock_models = checked(ID_X_LODSTOCK);
    cfg.xbox_make_lods = checked(ID_X_MAKELOD);
    cfg.xbox_trim_models = std::clamp(sel(ID_X_TRIM), 0, 3);
    cfg.xbox_collision = std::min(cfg.xbox_trim_models, 2);   // (heavy models: their collision too)
    cfg.xbox_compress_animations = std::clamp(sel(ID_X_ANIM), 0, 2);
    cfg.xbox_add_portals = sel(ID_X_PORTALS) >= 1;
    cfg.xbox_replan_portals = sel(ID_X_PORTALS) == 2;
    {
        static const int tex[4] = {512, 512, 1024, 0}, obj[4] = {256, 0, 0, 0}, lm[3] = {0, 512, 256};
        int t = sel(ID_X_TEX), l = sel(ID_X_LM);
        if (t < 0 || t > 3) t = 0;
        cfg.xbox_max_texture = tex[t];
        cfg.xbox_max_object_texture = obj[t];
        cfg.xbox_level_texture_mb = t == 0 ? 6 : 0;
        cfg.xbox_max_lightmap = lm[l >= 0 && l < 3 ? l : 0];
    }
    {
        int k = sel(ID_X_W2);
        cfg.xbox_other_weapons = k > 0 && k < (int)(sizeof(kWeaponChoices) / sizeof(kWeaponChoices[0])) ? kWeaponChoices[k].name : "";
    }
    const int vids[3] = {ID_X_V0, ID_X_V1, ID_X_V2};
    for (int i = 0; i < 3; i++) {
        int k = sel(vids[i]);
        if (k >= 0 && k < (int)(sizeof(kVehicleChoices) / sizeof(kVehicleChoices[0]))) cfg.xbox_vehicle_slots[i] = kVehicleChoices[k].tag;
    }
    const int wids[2] = {ID_X_W0, ID_X_W1};
    for (int i = 0; i < 2; i++) {
        int k = sel(wids[i]);
        if (k <= 0) cfg.xbox_keep_weapons.push_back(kPcWeaponTags[i]);
        else cfg.xbox_weapon_replacements[kPcWeaponTags[i]] = kWeaponChoices[k].name;
    }
}

// The vehicle texts depend on whether every vehicle is kept (map folders = the Halo Universal XBE).
static void update_vehicle_texts() {
    bool f = folder_mode(), all = f && checked(ID_X_ALLVEH);
    EnableWindow(GetDlgItem(g_wnd, ID_X_ALLVEH), f);
    set_text(ID_X_VLBL, all ? L"Vehicle slots - what the gametype's Warthogs / Ghosts / Scorpions options spawn:"
                            : L"Vehicle slots - the Xbox only spawns these three. The gametype's vehicle options pick them by slot:");
    set_text(ID_X_VHINT,
             all ? L"Every vehicle the map places spawns when the gametype's Vehicles option is All. A vehicle of a "
                   L"slot's type becomes the slotted one."
             : f ? L"Placed vehicles without a slot become a slotted vehicle of the same kind (any warthog -> the slotted "
                   L"warthog, Banshee <-> ghost), or are removed."
                 : L"Placed vehicles without a slot become a slotted vehicle of the same kind, or are removed. Keeping "
                   L"every vehicle needs the Halo Universal XBE (Map folders).");
}

// Map folder mode swaps the "Xbox name" pickers for "Map card..." buttons.
static void apply_mode_visibility(int p) {
    update_vehicle_texts();
    bool f = folder_mode();
    auto vis = [&](int id, int page, bool on) { ShowWindow(GetDlgItem(g_wnd, id), p == page && on ? SW_SHOW : SW_HIDE); };
    vis(ID_B_XBOX_NAME_LBL, 1, !f);
    vis(ID_B_XBOX_NAME, 1, !f);
    vis(ID_B_CARD, 1, f);
    vis(ID_C_XNAME_LBL, 0, !f);
    vis(ID_C_XNAME, 0, !f);
    vis(ID_C_CARD, 0, f);
    set_text(ID_X_MAPLIST_HINT,
             f ? L"Each map goes in its own folder with a name, description and picture: \"Map card...\" on the Build and "
                 L"Convert tabs."
               : L"Which names the \"Xbox name\" lists offer. The map you port takes the name of the map it replaces.");
}

static void show_page(int p) {
    for (int i = 0; i < 5; i++)
        for (int id : g_pages[i]) ShowWindow(GetDlgItem(g_wnd, id), i == p ? SW_SHOW : SW_HIDE);
    apply_mode_visibility(p);
}

// ---------------------------------------------------------------- map cards
static fs::path cards_root() { return appdata_dir(CSIDL_LOCAL_APPDATA) / L"cards"; }
static fs::path build_card_dir() { return mapcard::card_dir(cards_root(), fs::path(get_text(ID_SCEN))); }
// The Convert tab's card: the one this map was written with if it's one of your Xbox outputs, else one per input.
static std::wstring g_conv_card_in = L"\x1";
static fs::path g_conv_card;
static fs::path conv_card_dir() {
    fs::path in(get_text(ID_IN));
    if (in.wstring() != g_conv_card_in) {
        g_conv_card_in = in.wstring();
        g_conv_card = mapcard::find_card_by_output(cards_root(), in);
        if (g_conv_card.empty()) g_conv_card = mapcard::card_dir(cards_root(), in);
    }
    return g_conv_card;
}
static std::wstring build_map_name() { return lower(fs::path(get_text(ID_SCEN)).stem().wstring()); }
static std::wstring conv_map_name() {
    return lower(g_conv_own.empty() ? fs::path(get_text(ID_IN)).stem().wstring() : g_conv_own);
}
static mapcard::Card build_card() { return mapcard::load_card(build_card_dir(), N(build_map_name())); }
// A new card's starting name. Older Xbox ports all use stock internal names (that was the only way to get them
// into the menu), so for an Xbox map the file name says more.
static std::wstring conv_card_name() {
    return combo_game(ID_FROM) == Game::Xbox ? lower(fs::path(get_text(ID_IN)).stem().wstring()) : conv_map_name();
}
static mapcard::Card conv_card() { return mapcard::load_card(conv_card_dir(), N(conv_card_name())); }

// Write info.txt / preview.bin next to the map. Returns false (after telling the user) if it can't.
static bool write_card(const fs::path &card_dir, const std::wstring &map_name, const fs::path &map_path,
                       std::wstring &note) {
    mapcard::Card c = mapcard::load_card(card_dir, N(map_name));
    mapcard::Image img;
    bool pic = mapcard::card_image(card_dir, c, img);
    std::string err;
    fs::path folder = map_path.parent_path();
    if (!mapcard::write_folder_files(c, pic ? &img : nullptr, folder, err)) {
        MessageBoxW(g_wnd, W(err).c_str(), L"Halo Map Porter", MB_ICONWARNING);
        return false;
    }
    note = L"Map card: \"" + W(c.name) + L"\" -> " + folder.wstring() + L"\\ (info.txt" + (pic ? L", preview.bin)" : L"; no picture yet)");
    c.output = u8(map_path);   // so opening that map later finds this card
    mapcard::save_card(card_dir, c);
    std::error_code ec;
    for (auto &e : fs::directory_iterator(folder, ec)) {
        auto n = e.path().filename().wstring();
        if (_wcsicmp(e.path().extension().wstring().c_str(), L".map") == 0 && _wcsicmp(n.c_str(), map_path.filename().wstring().c_str()) != 0)
            note += L"\nNote: this folder also has " + n + L". The Xbox loads whichever .map it finds first, so delete the other one.";
    }
    return true;
}
// Halo Universal XBE build 20: what a map offers goes into its card's map browser lines (gametypes, a player guess,
// genre suggestions) unless you set them yourself. `rewrite`: info.txt next to the map is written again. Returns a
// line for the log ("" = nothing changed). Safe on the worker thread (no windows, no pictures).
static std::string card_traits(const Config &cfg, const fs::path &card_dir, const std::string &map_name, const fs::path &map,
                               const fs::path &rewrite) {
    porter::MapTraits t;
    std::string err;
    if (!porter::analyze_map_traits(cfg, map, t, &err)) return "Map card: couldn't read what the map offers (" + err + ").";
    mapcard::Card c = mapcard::load_card(card_dir, map_name);
    std::string said = mapcard::apply_analysis(c, t.gametypes, t.pmin, t.pmax, t.genres, t.players_why);
    if (said.empty()) return "";
    mapcard::save_card(card_dir, c);
    if (!rewrite.empty() && !mapcard::write_info_txt(c, rewrite, err)) return "Map card: " + err;
    if (said.find("suggested Genres") != std::string::npos && !t.why_genres.empty()) said += " (" + t.why_genres + ")";
    return "Map card, for the Halo Universal XBE's map list: " + said + ". Change them under \"Map card...\".";
}
static bool check_card_folder(const mapcard::Card &c) {
    std::string p = mapcard::folder_problem(c.folder);
    if (p.empty()) return true;
    MessageBoxW(g_wnd, (L"The map card's folder name needs fixing: " + W(p) + L".\n\nOpen \"Map card...\" to change it.").c_str(),
                L"Halo Map Porter", MB_ICONINFORMATION);
    return false;
}
// Convert tab, map folders: the input is already an Xbox map (someone else's port, or one of your builds),
// so nothing gets converted. "Save map card" puts the map in its folder and writes the card.
static bool card_only() {
    return combo_game(ID_TO) == Game::Xbox && folder_mode() && combo_game(ID_FROM) == Game::Xbox;
}
static bool same_file(const fs::path &a, const fs::path &b) {
    std::error_code ec;
    return !a.empty() && !b.empty() && fs::exists(a, ec) && fs::exists(b, ec) && fs::equivalent(a, b, ec);
}
// After the card editor closes: if the map is already in the card's folder, refresh info.txt and
// preview.bin there right away, so a card change never needs a rebuild.
static void refresh_card_files(const fs::path &card_dir, const std::wstring &map_name, const fs::path &map_path) {
    std::error_code ec;
    if (map_path.empty() || !fs::is_regular_file(map_path, ec)) return;
    if (!mapcard::folder_problem(mapcard::load_card(card_dir, N(map_name)).folder).empty()) return;
    std::wstring note;
    if (!write_card(card_dir, map_name, map_path, note)) return;
    append_log(L"Updated the map card next to " + map_path.filename().wstring() + L" (no rebuild needed).\n" + note);
}
// The editor's line about where its files go: `map_path` is where the map is or will be for this card.
static std::wstring card_where(const fs::path &map_path, const wchar_t *later) {
    std::error_code ec;
    if (map_path.empty()) return L"";
    if (fs::is_regular_file(map_path, ec))
        return L"The map is already in " + map_path.parent_path().wstring() + L"\\, so Done updates the card there.";
    return std::wstring(L"Goes in ") + map_path.parent_path().wstring() + L"\\ " + later;
}

static void update_invader_status() {
    Config c = make_config();
    set_text(ID_S_INV_STATUS, invader_ok(c) ? L"Invader found - you're all set" : L"Invader not found - click Download or Browse");
}

static void set_busy(bool b) {
    g_busy = b;
    if (b) {
        g_last_log = GetTickCount64();
        SetTimer(g_wnd, kHeartbeat, 1000, nullptr);
    } else {
        KillTimer(g_wnd, kHeartbeat);
        clear_heartbeat();
    }
    for (int id : {ID_CONVERT, ID_BUILD, ID_S_INV_DL, ID_PK_CREATE}) EnableWindow(GetDlgItem(g_wnd, id), !b);
    if (b) {
        ShowWindow(GetDlgItem(g_wnd, ID_OPEN_FOLDER), SW_HIDE);
        ShowWindow(GetDlgItem(g_wnd, ID_OPEN_REPORT), SW_HIDE);
        ShowWindow(GetDlgItem(g_wnd, ID_SEE_LODS), SW_HIDE);
    }
}

static bool same_path(const fs::path &a, const fs::path &b) { return _wcsicmp(a.wstring().c_str(), b.wstring().c_str()) == 0; }

// Convert tab, Xbox output: the file name is the map's name on the Xbox.
static std::wstring conv_xbox_name() {
    std::wstring n = chosen_map(ID_C_XNAME);
    return n.empty() ? lower(g_conv_own) : n;
}

static void update_conv_hint() {
    std::wstring h;
    SetWindowTextW(GetDlgItem(g_wnd, ID_CONVERT), card_only() ? L"Save map card" : L"Convert");
    set_text(ID_XSHRINK, card_only() ? L"The map is used as it is, so the Xbox tab's conversion options don't apply to it."
                                     : L"Xbox output uses the options on the Xbox tab (map list, vehicle slots, PC weapons, size).");
    if (card_only()) {
        fs::path in(get_text(ID_IN)), out(get_text(ID_OUT));
        std::error_code ec;
        if (same_file(in, out))
            h = L"Already in its folder. \"Save map card\" rewrites its info.txt and preview.bin. Nothing is converted.";
        else if (!out.empty() && fs::exists(out, ec))
            h = out.parent_path().filename().wstring() + L"\\ already has " + out.filename().wstring() +
                L". \"Save map card\" replaces it and rewrites the card.";
        else if (!out.empty())
            h = L"Already an Xbox map. \"Save map card\" copies it into " + out.parent_path().filename().wstring() +
                L"\\ with its card. Nothing is converted.";
    } else if (combo_game(ID_TO) == Game::Xbox && folder_mode()) {
        fs::path out(get_text(ID_OUT));
        if (!out.empty())
            h = L"Goes in its own folder, " + out.parent_path().filename().wstring() +
                L"\\, with its map card (Halo Universal XBE map folders).";
    } else if (combo_game(ID_TO) == Game::Xbox) {
        std::wstring stem = lower(fs::path(get_text(ID_OUT)).stem().wstring());
        int k = find_map(stem);
        if (stem.empty())
            h = L"";
        else if (k >= 0)
            h = L"Replaces " + stem + (expanded_list() ? L" (#" + std::to_wstring(k) + L" in the map pack)" : L"") +
                L" on the Xbox. Don't rename the file afterwards.";
        else
            h = L"\"" + stem + L"\" isn't in the " + map_list_name() +
                L" map list, so it won't replace a map in the Xbox menu. Pick one under \"Xbox name\".";
    }
    set_text(ID_CONV_HINT, h);
}

// Save as was changed by hand: select the matching Xbox name (or none if it isn't one).
static void sync_conv_name_from_out() {
    if (combo_game(ID_TO) != Game::Xbox || folder_mode()) return;
    std::wstring stem = lower(fs::path(get_text(ID_OUT)).stem().wstring());
    int k = find_map(stem);
    int s = k >= 0 ? k + 1 : (!stem.empty() && stem == lower(g_conv_own)) ? 0 : -1;
    SendDlgItemMessageW(g_wnd, ID_C_XNAME, CB_SETCURSEL, s, 0);
}

static void update_out_path() {
    Game to = combo_game(ID_TO);
    EnableWindow(GetDlgItem(g_wnd, ID_SHARE), to == Game::CE);
    EnableWindow(GetDlgItem(g_wnd, ID_C_XNAME), to == Game::Xbox);
    EnableWindow(GetDlgItem(g_wnd, ID_C_XNAME_LBL), to == Game::Xbox);
    EnableWindow(GetDlgItem(g_wnd, ID_C_CARD), to == Game::Xbox);
    fs::path in(get_text(ID_IN));
    if (g_out_manual) {
        if (to == Game::Xbox) sync_conv_name_from_out();
    } else if (!in.empty()) {
        fs::path dir = default_dir(to);
        if (to == Game::Xbox) dir = fs::path(get_text(ID_B_XBOX_OUT)); // the Build tab's Xbox folder, if set
        if (dir.empty() || !fs::is_directory(dir)) dir = in.parent_path();
        fs::path out;
        if (card_only()) {
            // keep the file name; next to the input when it already sits in a folder with the card's name
            std::wstring folder = W(conv_card().folder);
            fs::path xo(get_text(ID_B_XBOX_OUT)), base;
            if (!xo.empty() && fs::is_directory(xo)) base = xo;
            else if (_wcsicmp(in.parent_path().filename().c_str(), folder.c_str()) == 0) base = in.parent_path().parent_path();
            else base = in.parent_path();
            out = base / folder / in.filename();
        } else if (to == Game::Xbox && folder_mode()) {
            out = dir / W(conv_card().folder) / (conv_map_name() + L".map");
        } else if (to == Game::Xbox) {
            std::wstring name = conv_xbox_name();
            out = dir / (name + L".map");
            if (same_path(out, in)) out = dir / L"xbox" / (name + L".map"); // never next to the input under its name
        } else {
            out = dir / in.filename();
            if (out == in) out = in.parent_path() / (in.stem().wstring() + L"_" + (to == Game::CE ? L"ce" : L"mcc") + L".map");
        }
        set_text(ID_OUT, out.wstring());
    }
    update_conv_hint();
}

// Xbox name picked: the output file takes that name (in the same folder).
static void on_conv_name_chosen() {
    if (!g_out_manual) { update_out_path(); return; }
    if (sel(ID_C_XNAME) < 0) return;
    fs::path out(get_text(ID_OUT));
    if (!out.empty()) set_text(ID_OUT, (out.parent_path() / (conv_xbox_name() + L".map")).wstring());
    update_conv_hint();
}

static void refresh_build_names() {
    g_build_own = lower(fs::path(get_text(ID_SCEN)).stem().wstring());
    fill_map_combo(ID_B_XBOX_NAME, g_build_own, chosen_map(ID_B_XBOX_NAME));
}

// The Standard/Expanded choice changed: refill both name lists, keeping the picks that still exist.
static void on_map_list_changed() {
    std::wstring b = chosen_map(ID_B_XBOX_NAME), c = chosen_map(ID_C_XNAME);
    bool custom = sel(ID_C_XNAME) < 0;
    fill_map_combo(ID_B_XBOX_NAME, g_build_own, b);
    fill_map_combo(ID_C_XNAME, g_conv_own, c);
    if (custom || !g_out_manual) update_out_path();
    else on_conv_name_chosen();
    update_conv_hint();
}

static std::wstring g_last_in = L"\x1";

static void on_input_chosen() {
    fs::path in(get_text(ID_IN));
    if (in.wstring() == g_last_in) return; // unchanged (e.g. the box just lost focus)
    g_last_in = in.wstring();
    g_conv_card_in = L"\x1";   // look the card up again
    std::string name;
    Game g = fs::exists(in) ? detect_game(in, &name) : Game::Unknown;
    g_conv_own = g != Game::Unknown && !name.empty() ? W(name) : in.stem().wstring();
    fill_map_combo(ID_C_XNAME, g_conv_own, L"");
    if (g != Game::Unknown) {
        SendDlgItemMessageW(g_wnd, ID_FROM, CB_SETCURSEL, (int)g, 0);
        std::wstring msg = L"Detected: " + W(game_name(g)) + L" map \"" + W(name) + L"\"";
        std::wstring stem = in.stem().wstring();
        if (_wcsicmp(stem.c_str(), W(name).c_str()) != 0 && !(g == Game::Xbox && folder_mode()))   // map folders load any file name
            msg = W(game_name(g)) + L" map \"" + W(name) + L"\" - renamed, won't load as \"" + stem + L"\"";
        set_text(ID_DETECT, msg);
        Game to = g == Game::CE ? Game::MCC : Game::CE;
        if (g == Game::Xbox && folder_mode()) to = Game::Xbox;   // adding someone's Xbox map to a map folder build
        SendDlgItemMessageW(g_wnd, ID_TO, CB_SETCURSEL, (int)to, 0);
        // already in a map folder? start its card from that folder's info.txt / preview.bin
        fs::path grand = in.parent_path().parent_path(), xo(get_text(ID_B_XBOX_OUT));
        bool in_maps = g == Game::Xbox && (_wcsicmp(grand.filename().c_str(), L"maps") == 0 || same_path(grand, xo));
        std::string what;
        if (mapcard::import_card(conv_card_dir(), in, in_maps))
            append_log(L"Map card started from the map's folder, " + in.parent_path().filename().wstring() + L"\\.");
        // from an MCC mod (Steam Workshop item)? start its card from the mod's title, description and thumbnail
        else if (mapcard::import_mcc_mod_card(conv_card_dir(), in, N(conv_card_name()), what) && folder_mode())
            append_log(W(what) + L" \"Map card...\" to change it.");
    } else if (fs::exists(in)) {
        set_text(ID_DETECT, L"Couldn't recognize this file as a Halo CE map");
    }
    g_out_manual = false;
    update_out_path();
}

// ---------------------------------------------------------------- jobs
struct Done { bool ok; std::wstring msg, output, report, lods; };

static void finish(const Result &r, const std::string &what) {
    auto *d = new Done{r.ok, L"", W(u8(r.output)), W(u8(r.report)), W(u8(r.lods))};
    if (r.ok) {
        std::ostringstream s;
        s << "DONE: " << what << " -> " << u8(r.output) << "\n   " << r.changes.size() << " automatic fixes, "
          << r.warnings.size() << " warnings. Report: " << u8(r.report);
        d->msg = W(s.str());
    } else {
        d->msg = W("FAILED: " + what + " failed:\n" + r.error);
    }
    PostMessageW(g_wnd, WM_APP_DONE, 0, (LPARAM)d);
}

// "Save map card": an Xbox map goes into its folder as it is (copied unless it's already there) with its card.
static void save_map_card() {
    fs::path in(get_text(ID_IN)), out(get_text(ID_OUT));
    if (!fs::exists(in)) { MessageBoxW(g_wnd, L"Pick a map first.", L"Halo Map Porter", MB_ICONINFORMATION); return; }
    if (out.empty()) { MessageBoxW(g_wnd, L"Choose where the map goes (Save as).", L"Halo Map Porter", MB_ICONINFORMATION); return; }
    if (!check_card_folder(conv_card())) return;
    bool same = same_file(in, out);
    if (!same && fs::exists(out) &&
        MessageBoxW(g_wnd, (out.filename().wstring() + L" already exists in that folder. Replace it?").c_str(), L"Halo Map Porter",
                    MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    std::wstring note;
    std::string traits;
    {
        HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
        traits = card_traits(make_config(), conv_card_dir(), N(conv_card_name()), in, fs::path());
        SetCursor(old);
    }
    if (!write_card(conv_card_dir(), conv_card_name(), out, note)) return;
    save_settings();
    SetWindowTextW(g_log, L"");
    append_log(L"Map card for " + in.filename().wstring() + L" (already an Xbox map, so nothing is converted)");
    append_log(note);
    if (!traits.empty()) append_log(W(traits));
    if (same) {
        auto *d = new Done{true, L"DONE: Map card saved -> " + out.parent_path().wstring() + L"\\", out.wstring(), L""};
        PostMessageW(g_wnd, WM_APP_DONE, 0, (LPARAM)d);
        return;
    }
    set_busy(true);
    append_log(L"Copying " + in.filename().wstring() + L"...");
    std::thread([in, out] {
        std::error_code ec;
        fs::path part = out;
        part += L".part";
        fs::copy_file(in, part, fs::copy_options::overwrite_existing, ec);
        if (!ec && !MoveFileExW(part.c_str(), out.c_str(), MOVEFILE_REPLACE_EXISTING))
            ec = std::error_code((int)GetLastError(), std::system_category());
        Done *d;
        if (ec) {
            fs::remove(part, ec);
            d = new Done{false, L"FAILED: Couldn't copy the map into " + out.parent_path().wstring() + L"\\.", L"", L""};
        } else {
            d = new Done{true, L"DONE: Map card saved, map copied -> " + out.wstring(), out.wstring(), L""};
        }
        PostMessageW(g_wnd, WM_APP_DONE, 0, (LPARAM)d);
    }).detach();
}

// Weapons & vehicles picks, saved per input map (or scenario).
static fs::path picks_file(const fs::path &input) {
    std::wstring k = lower(input.wstring());
    uint64_t h = 1469598103934665603ull;
    for (wchar_t c : k) { h ^= (uint64_t)c; h *= 1099511628211ull; }
    wchar_t hex[24];
    swprintf(hex, 24, L"%016llx", (unsigned long long)h);
    return appdata_dir(CSIDL_LOCAL_APPDATA) / L"picks" / (lower(input.stem().wstring()) + L"_" + hex + L".txt");
}
static std::map<std::string, std::string> load_picks(const fs::path &input) {
    std::map<std::string, std::string> picks;
    std::ifstream f(picks_file(input));
    std::string l;
    while (std::getline(f, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        size_t tab = l.find('\t');
        if (tab != std::string::npos && tab > 0) picks[l.substr(0, tab)] = l.substr(tab + 1);
    }
    return picks;
}
static void save_picks(const fs::path &input, const std::map<std::string, std::string> &picks) {
    fs::path p = picks_file(input);
    std::error_code ec;
    if (picks.empty()) { fs::remove(p, ec); return; }
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    for (auto &[from, to] : picks) f << from << '\t' << to << '\n';
}

// The map the weapons & vehicles window is for: the Convert tab's map, else the Build tab's scenario.
static bool contents_input(fs::path &in, Game &game) {
    in = fs::path(get_text(ID_IN));
    if (!in.empty() && fs::exists(in)) {
        game = combo_game(ID_FROM);
        if (game == Game::Unknown || game == Game::Xbox) game = detect_game(in);
        return true;
    }
    in = fs::path(get_text(ID_SCEN));
    game = Game::Unknown;
    return !in.empty() && fs::exists(in);
}

// What changes a map's contents (the weapons & vehicles window's measurement): the input file and the Xbox tab's
// content options.
static std::string contents_key(const Config &cfg, const fs::path &in) {
    std::error_code ec;
    auto mt = fs::last_write_time(in, ec);
    std::string key = N(lower(in.wstring())) + "|" + std::to_string((long long)mt.time_since_epoch().count()) + "|" +
                      (cfg.xbox_use_stock_versions ? "s" : "-") + (cfg.xbox_keep_all_vehicles ? "a" : "-") +
                      (cfg.xbox_make_lods ? "l" : "-") + std::to_string(cfg.xbox_trim_models) + std::to_string(cfg.xbox_compress_animations) + "|" +
                      cfg.xbox_other_weapons + "|" + cfg.xbox_vehicle_slots[0] + "|" + cfg.xbox_vehicle_slots[1] + "|" +
                      cfg.xbox_vehicle_slots[2];
    for (auto &w : cfg.xbox_keep_weapons) key += "|k" + w;
    for (auto &[w, r] : cfg.xbox_weapon_replacements) key += "|r" + w + "=" + r;
    fs::path lib = !cfg.lod_library.empty() ? cfg.lod_library : cfg.cache_dir / "lod_library";   // (models' choices)
    for (auto f : {"choices.txt", "skip.txt"}) {
        auto t = fs::last_write_time(lib / f, ec);
        key += "|" + std::to_string(ec ? 0LL : (long long)t.time_since_epoch().count());
    }
    return key;
}

// A convert/build for the Xbox that didn't fit was measured for the window as well: keep that, and say so.
static void keep_measurement(Result &r, const std::string &key, const std::shared_ptr<ContentAnalysis> &m) {
    if (!m || m->map_name.empty()) return;
    contentsui::remember(key, m);
    if (!r.ok)
        r.error += "\nThe Xbox tab's \"Weapons & vehicles...\" window has already measured this map: it shows what each "
                   "weapon and vehicle costs and what you could replace them with.";
}

static void open_contents() {
    fs::path in;
    Game game;
    if (!contents_input(in, game)) {
        MessageBoxW(g_wnd, L"Pick a map on the Convert tab (or a scenario on the Build tab) first.", L"Halo Map Porter",
                    MB_ICONINFORMATION);
        return;
    }
    if (g_busy) {
        MessageBoxW(g_wnd, L"Wait for the conversion to finish first.", L"Halo Map Porter", MB_ICONINFORMATION);
        return;
    }
    save_settings();
    Config cfg = make_config();
    apply_xbox_options(cfg);
    std::string key = contents_key(cfg, in);
    auto picks = load_picks(in);
    bool ok = contentsui::edit_picks(
        g_wnd, L"Weapons & vehicles - " + in.filename().wstring(), key,
        [cfg, in, game](ContentAnalysis &a, const Log &log) { return analyze_xbox_contents(cfg, in, game, a, log); }, picks);
    if (ok) {
        save_picks(in, picks);
        append_log(picks.empty() ? L"Weapons & vehicles: no picks for " + in.filename().wstring() + L"."
                                 : L"Weapons & vehicles: " + std::to_wstring(picks.size()) + L" pick" + (picks.size() == 1 ? L"" : L"s") +
                                       L" saved for " + in.filename().wstring() + L"; they're used when you convert it for the Xbox.");
    }
}

// The level shaders window's choices, saved per input map (or scenario) next to its weapons & vehicles picks.
static fs::path shader_picks_file(const fs::path &input) {
    fs::path p = picks_file(input);
    p.replace_extension(L".shaders.txt");
    return p;
}
static shadersui::Picks load_shader_picks(const fs::path &input) {
    shadersui::Picks picks;
    std::ifstream f(shader_picks_file(input));
    std::string l;
    while (std::getline(f, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        size_t tab = l.find('\t');
        if (tab == std::string::npos) continue;
        std::string k = l.substr(0, tab), v = l.substr(tab + 1);
        if (k == "mode") picks.mode = std::clamp(atoi(v.c_str()), 0, 2);
        else if (k == "drop" && !v.empty()) picks.drops.insert(v);
        else if (k == "fogdist") picks.fog_distance = std::max(0.0, atof(v.c_str()));
        else if (k == "fog") {
            picks.fog = std::clamp(atoi(v.c_str()), 0, 3);
            size_t t2 = v.find('\t');
            if (t2 != std::string::npos) picks.fog_scale = std::clamp(atof(v.c_str() + t2 + 1), 0.1, 1.0);
        } else if (k == "tinypx") picks.tiny_px = std::max(0.0, atof(v.c_str()));
        else if (k == "weather") picks.weather = std::clamp(atoi(v.c_str()), 0, 3);
        else if (k == "riders") picks.riders = atoi(v.c_str()) ? 1 : 0;
        else if (k == "props") picks.props = atoi(v.c_str()) ? 1 : 0;
        else if (k == "sky") picks.sky = atoi(v.c_str()) ? 1 : 0;
        else if (k == "effects") picks.effects = std::clamp(atoi(v.c_str()), 0, 2);
        // (an older "scenery" line, a distance, is left out: it popped big things in and out of view)
    }
    return picks;
}
static void save_shader_picks(const fs::path &input, const shadersui::Picks &picks) {
    fs::path p = shader_picks_file(input);
    std::error_code ec;
    shadersui::Picks defaults;
    if (picks.mode == defaults.mode && picks.fog == defaults.fog && picks.tiny_px == defaults.tiny_px &&
        picks.weather == defaults.weather && picks.riders == defaults.riders && picks.props == defaults.props &&
        picks.sky == defaults.sky && picks.effects == defaults.effects) {   // (all as by default)
        fs::remove(p, ec);
        return;
    }
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    f << "mode\t" << picks.mode << '\n';
    for (auto &t : picks.drops) f << "drop\t" << t << '\n';
    f << "fog\t" << picks.fog << '\t' << picks.fog_scale << '\n';
    f << "fogdist\t" << picks.fog_distance << '\n';
    f << "tinypx\t" << picks.tiny_px << '\n';
    f << "weather\t" << picks.weather << '\n';
    f << "riders\t" << picks.riders << '\n';
    f << "props\t" << picks.props << '\n';
    f << "sky\t" << picks.sky << '\n';
    f << "effects\t" << picks.effects << '\n';
}
static void apply_shader_picks(Config &cfg, const fs::path &input) {
    shadersui::Picks picks = load_shader_picks(input);
    cfg.xbox_drop_reflections = picks.mode == 0;
    cfg.xbox_fog = picks.fog;
    cfg.xbox_fog_scale = picks.fog_scale;
    cfg.xbox_fog_distance = picks.fog_distance;
    cfg.xbox_scenery_pixels = picks.tiny_px;
    cfg.xbox_weather = picks.weather;
    cfg.xbox_rider_ai_off = picks.riders == 1;
    cfg.xbox_detailed_props_off = picks.props == 1;
    cfg.xbox_sky = picks.sky;
    cfg.xbox_effects = picks.effects;
    if (picks.mode == 1) {
        cfg.xbox_reflection_picks_set = true;
        cfg.xbox_reflection_picks = picks.drops;
    }
}

// What changes a map's level shaders measurement: the input file, and whether portals are added (what's drawn).
static std::string shaders_key(const Config &cfg, const fs::path &in) {
    std::error_code ec;
    auto mt = fs::last_write_time(in, ec);
    return N(lower(in.wstring())) + "|" + std::to_string((long long)mt.time_since_epoch().count()) + "|" +
           (cfg.xbox_add_portals ? "p" : "-") + (cfg.xbox_replan_portals ? "r" : "-");
}

static void open_shaders() {
    fs::path in;
    Game game;
    if (!contents_input(in, game)) {
        MessageBoxW(g_wnd, L"Pick a map on the Convert tab (or a scenario on the Build tab) first.", L"Halo Map Porter",
                    MB_ICONINFORMATION);
        return;
    }
    if (g_busy) {
        MessageBoxW(g_wnd, L"Wait for the conversion to finish first.", L"Halo Map Porter", MB_ICONINFORMATION);
        return;
    }
    save_settings();
    Config cfg = make_config();
    apply_xbox_options(cfg);
    std::string key = shaders_key(cfg, in);
    shadersui::Picks picks = load_shader_picks(in);
    bool ok = shadersui::edit(
        g_wnd, L"Level - " + in.filename().wstring(), key,
        [cfg, in, game](fs::path &file, std::string &error, const std::function<void(const std::string &)> &log) {
            return analyze_level_shaders(cfg, in, game, file, error, [log](const std::string &l) { log(l); });
        },
        picks);
    if (ok) {
        save_shader_picks(in, picks);
        std::wstring refl = picks.mode == 0   ? L"the suggested reflections go"
                            : picks.mode == 2 ? L"every reflection kept"
                                              : std::to_wstring(picks.drops.size()) + L" reflection" + (picks.drops.size() == 1 ? L"" : L"s") +
                                                    L" go (your picks)";
        wchar_t fog[80];
        if (picks.fog == 0) swprintf(fog, 80, L"automatic");
        else if (picks.fog == 1) swprintf(fog, 80, L"as the map has it");
        else if (picks.fog == 3) swprintf(fog, 80, L"fully opaque at %g units", picks.fog_distance);
        else swprintf(fog, 80, L"fully opaque at %.0f%% of its distance", picks.fog_scale * 100);
        wchar_t tiny[60];
        if (picks.tiny_px > 0) swprintf(tiny, 60, L"hidden below %g px", picks.tiny_px);
        else swprintf(tiny, 60, L"never hidden");
        append_log(L"Level choices for " + in.filename().wstring() + L", used when you convert it for the Xbox: " + refl +
                   L"; fog " + fog + L"; scenery " + tiny + L"; weather " +
                   (picks.weather == 1 ? L"lighter" : picks.weather == 2 ? L"half the drops" : picks.weather == 3 ? L"off" : L"as made") +
                   L"; sky " + (picks.sky ? L"as made" : L"in fewer layers") + L"; effects " +
                   (picks.effects == 1 ? L"as made" : picks.effects == 2 ? L"lighter" : L"no busier than Bungie's") +
                   (picks.riders ? L"; riders without AI" : L"") + (picks.props ? L"; detailed props without collision" : L"") + L".");
    }
}

// `again`: the LODs window's "Convert again" (the same map and output, so no questions about replacing it).
static void start_convert(bool again = false) {
    if (card_only()) { save_map_card(); return; }
    fs::path in(get_text(ID_IN)), out(get_text(ID_OUT));
    Game from = combo_game(ID_FROM), to = combo_game(ID_TO);
    if (!fs::exists(in)) { MessageBoxW(g_wnd, L"Pick a map to convert first.", L"Halo Map Porter", MB_ICONINFORMATION); return; }
    if (out.empty()) { MessageBoxW(g_wnd, L"Choose where to save the converted map.", L"Halo Map Porter", MB_ICONINFORMATION); return; }
    if (fs::exists(out) && fs::equivalent(in, out)) {
        MessageBoxW(g_wnd, L"The output would overwrite the input map. Pick a different output file.", L"Halo Map Porter", MB_ICONWARNING);
        return;
    }
    std::wstring card_note;
    if (to == Game::Xbox && folder_mode()) {
        if (!check_card_folder(conv_card())) return;
    } else if (to == Game::Xbox && !again && find_map(out.stem().wstring()) < 0 &&
        MessageBoxW(g_wnd, (L"\"" + lower(out.stem().wstring()) + L"\" isn't in the " + map_list_name() +
                            L" map list, so the Xbox won't show it in place of a map.\n\nTo replace a map, pick it under "
                            L"\"Xbox name\". Convert under this name anyway?").c_str(),
                    L"Halo Map Porter", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    if (!again && fs::exists(out) && MessageBoxW(g_wnd, (out.filename().wstring() + L" already exists in that folder. Replace it?").c_str(),
                                      L"Halo Map Porter", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    if (to == Game::Xbox && folder_mode() && !write_card(conv_card_dir(), conv_card_name(), out, card_note)) return;
    save_settings();
    g_last_was_build = false;
    Config cfg = make_config();
    apply_xbox_options(cfg);
    if (to == Game::Xbox) {
        cfg.xbox_replace = load_picks(in);
        apply_shader_picks(cfg, in);
    }
    bool share = checked(ID_SHARE);
    set_busy(true);
    SetWindowTextW(g_log, L"");
    append_log(L"Converting " + in.filename().wstring() + L": " + W(game_name(from)) + L" -> " + W(game_name(to)));
    if (!card_note.empty()) append_log(card_note);
    std::string key = to == Game::Xbox ? contents_key(cfg, in) : std::string();
    std::string lkey = to == Game::Xbox ? shaders_key(cfg, in) : std::string();
    fs::path card_dir = to == Game::Xbox && folder_mode() ? conv_card_dir() : fs::path();
    std::string card_name = N(conv_card_name());
    std::thread([=] {
        auto m = to == Game::Xbox ? std::make_shared<ContentAnalysis>() : nullptr;
        Result r = convert_map(cfg, in, from, to, out, share, post_log, m.get());
        keep_measurement(r, key, m);
        if (!lkey.empty()) shadersui::remember(lkey, r.shaders);
        if (r.ok && !card_dir.empty())
            if (std::string t = card_traits(cfg, card_dir, card_name, out, out.parent_path()); !t.empty()) post_log(t);
        finish(r, std::string("Converted to ") + game_name(to));
    }).detach();
}

static void start_build() {
    fs::path scen(get_text(ID_SCEN));
    if (!fs::exists(scen)) { MessageBoxW(g_wnd, L"Pick your .scenario tag first.", L"Halo Map Porter", MB_ICONINFORMATION); return; }
    std::vector<std::pair<Game, fs::path>> targets;
    if (checked(ID_B_MCC)) targets.push_back({Game::MCC, fs::path(get_text(ID_B_MCC_OUT))});
    if (checked(ID_B_CE)) targets.push_back({Game::CE, fs::path(get_text(ID_B_CE_OUT))});
    if (checked(ID_B_XBOX)) targets.push_back({Game::Xbox, fs::path(get_text(ID_B_XBOX_OUT))});
    if (targets.empty()) { MessageBoxW(g_wnd, L"Tick at least one game to build for.", L"Halo Map Porter", MB_ICONINFORMATION); return; }
    for (auto &t : targets)
        if (t.second.empty() || !fs::is_directory(t.second)) {
            MessageBoxW(g_wnd, (L"Choose an existing output folder for " + W(game_name(t.first)) + L".").c_str(),
                        L"Halo Map Porter", MB_ICONINFORMATION);
            return;
        }
    save_settings();
    g_last_was_build = true;
    Config cfg = make_config();
    apply_xbox_options(cfg);
    cfg.xbox_replace = load_picks(scen);   // used by the Xbox build only
    apply_shader_picks(cfg, scen);
    bool share = checked(ID_B_SHARE);
    std::wstring xbox_name = chosen_map(ID_B_XBOX_NAME); // empty = the scenario's own name
    std::wstring xbox_note, xbox_folder;
    if (checked(ID_B_XBOX) && folder_mode()) {
        mapcard::Card card = build_card();
        if (!check_card_folder(card)) return;
        xbox_name.clear();
        xbox_folder = W(card.folder);
        fs::path out = fs::path(get_text(ID_B_XBOX_OUT)) / xbox_folder / (build_map_name() + L".map");
        if (!write_card(build_card_dir(), build_map_name(), out, xbox_note)) return;
    } else if (checked(ID_B_XBOX) && xbox_name.empty() && find_map(scen.stem().wstring()) < 0)
        xbox_note = L"Note: \"" + lower(scen.stem().wstring()) + L"\" isn't in the " + map_list_name() +
                    L" map list, so the Xbox won't show it in place of a map. To replace one, pick it next to the "
                    L"Xbox folder.";
    set_busy(true);
    SetWindowTextW(g_log, L"");
    if (!xbox_note.empty()) append_log(xbox_note);
    std::string key = contents_key(cfg, scen), lkey = shaders_key(cfg, scen);
    fs::path card_dir = xbox_folder.empty() ? fs::path() : build_card_dir();
    std::string card_name = N(build_map_name());
    std::thread([=] {
        Result last;
        std::string summary;
        bool all_ok = true;
        for (auto &t : targets) {
            post_log(std::string("=== ") + game_name(t.first) + " ===");
            std::wstring name = scen.stem().wstring();
            if (t.first == Game::Xbox && !xbox_name.empty()) name = xbox_name;
            fs::path out = t.second / (name + L".map");
            if (t.first == Game::Xbox && !xbox_folder.empty()) out = t.second / xbox_folder / (lower(name) + L".map");
            auto m = t.first == Game::Xbox ? std::make_shared<ContentAnalysis>() : nullptr;
            Result r = build_from_tags(cfg, scen, t.first, out, share, post_log, m.get());
            keep_measurement(r, key, m);
            if (t.first == Game::Xbox) shadersui::remember(lkey, r.shaders);
            if (r.ok && t.first == Game::Xbox && !card_dir.empty())
                if (std::string tr = card_traits(cfg, card_dir, card_name, out, out.parent_path()); !tr.empty()) post_log(tr);
            if (r.ok)
                post_log("OK: " + u8(out) + "  (" + std::to_string(r.changes.size()) + " fixes, " +
                         std::to_string(r.warnings.size()) + " warnings)");
            else
                post_log(std::string("FAILED: ") + game_name(t.first) + " failed:\n" + r.error);
            all_ok = all_ok && r.ok;
            if (r.ok || last.output.empty()) last = r;
            if (!r.ok) last.error = r.error;
        }
        last.ok = all_ok;
        finish(last, all_ok ? "Build finished" : "Build");
    }).detach();
}

// ---------------------------------------------------------------- Invader download
static std::string read_file(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

static void start_download() {
    set_busy(true);
    SetWindowTextW(g_log, L"");
    fs::path dest = appdata_dir(CSIDL_LOCAL_APPDATA) / L"invader";
    std::thread([dest] {
        auto fail = [](const std::string &why) {
            Result r;
            r.error = why + "\nYou can download it yourself from https://github.com/SnowyMouse/invader/releases, "
                            "unzip it, and point the Invader folder setting at it.";
            finish(r, "Invader download");
        };
        post_log("Looking up the latest Invader release on GitHub...");
        fs::path tmp = appdata_dir(CSIDL_LOCAL_APPDATA) / L"release.json";
        if (URLDownloadToFileW(nullptr, L"https://api.github.com/repos/SnowyMouse/invader/releases/latest", tmp.c_str(), 0, nullptr) != S_OK)
            return fail("Couldn't reach GitHub.");
        std::string json = read_file(tmp);
        std::regex url_re("\"browser_download_url\"\\s*:\\s*\"([^\"]+\\.zip)\"");
        std::string best;
        int best_score = -1;
        for (std::sregex_iterator it(json.begin(), json.end(), url_re), end; it != end; ++it) {
            std::string u = (*it)[1], l = u;
            for (auto &c : l) c = (char)tolower(c);
            int score = (l.find("win") != std::string::npos) * 4 + (l.find("64") != std::string::npos) * 2 +
                        (l.find("x86_64") != std::string::npos || l.find("amd64") != std::string::npos);
            if (l.find("source") != std::string::npos || l.find("src") != std::string::npos) score -= 10;
            if (score > best_score) { best = u; best_score = score; }
        }
        if (best.empty()) return fail("The latest release has no Windows download.");
        post_log("Downloading " + best);
        fs::path zip = appdata_dir(CSIDL_LOCAL_APPDATA) / L"invader.zip";
        if (URLDownloadToFileW(nullptr, W(best).c_str(), zip.c_str(), 0, nullptr) != S_OK)
            return fail("Download failed.");
        std::error_code ec;
        fs::remove_all(dest, ec);
        fs::create_directories(dest, ec);
        post_log("Unpacking...");
        std::string out;
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        run_process({N(fs::path(sys) / L"tar.exe"), "-xf", u8(zip), "-C", u8(dest)}, out);
        fs::remove(zip, ec);
        for (auto &e : fs::recursive_directory_iterator(dest, ec)) {
            if (e.path().filename() == L"invader-build.exe") {
                Result r;
                r.ok = true;
                r.output = e.path().parent_path();
                PostMessageW(g_wnd, WM_APP_LOG, 1, (LPARAM) new std::wstring(r.output.wstring()));
                finish(r, "Invader installed");
                return;
            }
        }
        fail("Unpacked the download but couldn't find invader-build.exe in it.");
    }).detach();
}

// ---------------------------------------------------------------- window
// ---------------------------------------------------------------- map packs
// A mod pack's maps + its ui.map -> maps\<Pack>\<Map>\ folders for the Halo Universal XBE (see mappack.hpp).
static mappack::Pack g_pack;
static bool g_pack_loaded = false, g_pk_filling = false, g_pk_folders_edited = false;
static int g_pk_sel = -1;
static std::wstring g_pk_last_in;
static mapcard::Image g_logo;
static bool g_logo_menu = false;            // g_logo came from the pack's main menu
static std::wstring g_pk_logo_for;          // the pack the logo was last picked (or suggested) for
static std::string g_series;
static const std::wstring kFromMenu = L"ui.map: ";   // the logo box holds "ui.map: <bitmap>" for a menu logo
static const int kLogoSizes[] = {24, 32, 48, 64};
static HWND pk_list() { return GetDlgItem(g_wnd, ID_PK_LIST); }
static std::string pk_name() { return N(get_text(ID_PK_NAME)); }
static std::wstring crlf(const std::string &s) {
    std::wstring w = W(s), o;
    for (wchar_t c : w) { if (c == L'\n') o += L"\r\n"; else o += c; }
    return o;
}
static std::string no_cr(const std::wstring &w) {
    std::string s = N(w), o;
    for (char c : s) if (c != '\r') o += c;
    return o;
}

static void pk_update_series() {
    int k = sel(ID_PK_SIZE), c = sel(ID_PK_CORNER);
    g_series = g_logo.w > 0 ? mappack::series_bin(mappack::fit_logo(g_logo, kLogoSizes[k < 0 ? 1 : k], g_logo_menu), (mappack::Corner)(c < 0 ? 0 : c), 4)
                            : "";
    InvalidateRect(GetDlgItem(g_wnd, ID_PK_PIC), nullptr, FALSE);
}
static void pk_hint() {
    std::wstring t;
    if (!g_pack_loaded) t = L"Pick a mod pack: its folder, or the maps folder with its ui.map.";
    else {
        std::string why = mappack::pack_name_problem(pk_name(), g_pack);
        int n = 0;
        for (auto &e : g_pack.maps) n += e.include;
        t = !why.empty() ? W("Pack name: " + why)
                         : L"Writes maps\\" + get_text(ID_PK_NAME) + L"\\<folder>\\ for " + std::to_wstring(n) + (n == 1 ? L" map" : L" maps");
    }
    set_text(ID_PK_HINT, t);
}
static void pk_set_row(int i) {
    auto &e = g_pack.maps[i];
    std::string note = e.slot.empty() ? e.note : e.note.empty() ? "was " + e.slot : e.note + " (was " + e.slot + ")";
    std::wstring f = W(e.include ? e.folder : std::string()), n = W(e.name), t = W(note);
    ListView_SetItemText(pk_list(), i, 0, (LPWSTR)f.c_str());
    ListView_SetItemText(pk_list(), i, 1, (LPWSTR)n.c_str());
    ListView_SetItemText(pk_list(), i, 2, (LPWSTR)t.c_str());
}
static void pk_select(int i) {
    bool ok = i >= 0 && i < (int)g_pack.maps.size();
    g_pk_sel = ok ? i : -1;
    g_pk_filling = true;
    set_text(ID_PK_MNAME, ok ? W(g_pack.maps[i].name) : L"");
    set_text(ID_PK_DESC, ok ? crlf(g_pack.maps[i].desc) : L"");
    g_pk_filling = false;
    EnableWindow(GetDlgItem(g_wnd, ID_PK_MNAME), ok);
    EnableWindow(GetDlgItem(g_wnd, ID_PK_DESC), ok);
    InvalidateRect(GetDlgItem(g_wnd, ID_PK_PIC), nullptr, FALSE);
}
static void pk_fill() {
    HWND lv = pk_list();
    g_pk_filling = true;
    ListView_DeleteAllItems(lv);
    for (size_t i = 0; i < g_pack.maps.size(); i++) {
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        it.pszText = (LPWSTR)L"";
        ListView_InsertItem(lv, &it);
        ListView_SetCheckState(lv, (int)i, g_pack.maps[i].include);
        pk_set_row((int)i);
    }
    g_pk_filling = false;
    if (!g_pack.maps.empty()) ListView_SetItemState(lv, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    pk_select(g_pack.maps.empty() ? -1 : 0);
    pk_hint();
}
// a folder name for map i that no other included map uses
static void pk_auto_folder(int i) {
    std::string pn = pk_name();
    size_t max = mappack::KEY_MAX > pn.size() + 1 ? mappack::KEY_MAX - pn.size() - 1 : 1;
    auto &e = g_pack.maps[i];
    std::string base = mappack::clean_name(e.name, max);
    if (base.empty()) base = "Map";
    auto taken = [&](const std::string &f) {
        for (size_t k = 0; k < g_pack.maps.size(); k++)
            if ((int)k != i && g_pack.maps[k].include && _stricmp(g_pack.maps[k].folder.c_str(), f.c_str()) == 0) return true;
        return false;
    };
    std::string f = base;
    for (int n = 2; taken(f); n++) {
        std::string suffix = " " + std::to_string(n);
        f = mappack::clean_name(base, max > suffix.size() ? max - suffix.size() : 1) + suffix;
    }
    e.folder = f;
}
static bool pk_is_menu_text(const std::wstring &t) { return t.compare(0, kFromMenu.size(), kFromMenu) == 0; }
static int pk_menu_logo_index(const std::wstring &t) {   // which of this pack's menu logos the logo box names; -1 = none
    if (!pk_is_menu_text(t)) return -1;
    std::string tag = N(t.substr(kFromMenu.size()));
    for (size_t k = 0; k < g_pack.menu_logos.size(); k++)
        if (_stricmp(g_pack.menu_logos[k].tag.c_str(), tag.c_str()) == 0) return (int)k;
    return -1;
}
static void pk_load_logo(bool quiet) {
    std::wstring t = get_text(ID_PK_LOGO);
    g_logo = mapcard::Image();
    g_logo_menu = false;
    if (pk_is_menu_text(t)) {
        int k = pk_menu_logo_index(t);
        if (k >= 0) { g_logo = g_pack.menu_logos[k].image; g_logo_menu = true; }
        else if (!quiet && g_pack_loaded)
            MessageBoxW(g_wnd, (L"This pack's ui.map has no picture called " + t.substr(kFromMenu.size()) + L".").c_str(), L"Halo Map Porter", MB_ICONWARNING);
    } else {
        fs::path f(t);
        if (!f.empty() && !mapcard::load_image(f, g_logo, 1024, true) && !quiet)
            MessageBoxW(g_wnd, (L"Couldn't read " + f.filename().wstring() + L" as a picture.").c_str(), L"Halo Map Porter", MB_ICONWARNING);
    }
    pk_update_series();
}
static void pk_logo_chosen() {   // a logo (or none) was picked for the loaded pack: don't suggest over it next time
    if (!g_pack_loaded) return;
    g_pk_logo_for = get_text(ID_PK_IN);
    ini_set(L"pk_logo_for", g_pk_logo_for);
}
static void pk_use_menu_logo(int k, bool suggested) {
    const auto &m = g_pack.menu_logos[k];
    set_text(ID_PK_LOGO, kFromMenu + W(m.tag));
    pk_load_logo(true);
    size_t n = g_pack.menu_logos.size();
    std::wstring t = (suggested ? L"   Logo suggestion: " : L"Logo: ") + W(m.tag) + L", from the pack's main menu";
    if (n > 1) t += L" (" + std::to_wstring(k + 1) + L" of " + std::to_wstring(n) + L"; click \"Pack's logo\" again for the next)";
    append_log(t + L".");
    if (m.faded) append_log(L"   The menu draws it see-through, so the stamp keeps its lines solid. 48 or 64 pixels shows it best.");
    if (suggested) append_log(L"   \"No logo\" leaves the pictures as they are; Browse picks a PNG instead.");
}
static void pk_load() {
    fs::path in(get_text(ID_PK_IN));
    if (in.empty()) return;
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    mappack::Pack p;
    std::string err;
    bool ok = mappack::read_pack(in, p, err);
    SetCursor(old);
    if (!ok) {
        g_pack = mappack::Pack();
        g_pack_loaded = false;
        pk_fill();
        pk_load_logo(true);
        append_log(W("Map pack: " + err));
        MessageBoxW(g_wnd, W(err).c_str(), L"Halo Map Porter", MB_ICONWARNING);
        return;
    }
    g_pack = p;
    g_pack_loaded = true;
    g_pk_folders_edited = false;
    if (!same_path(in, fs::path(g_pk_last_in)) || get_text(ID_PK_NAME).empty()) set_text(ID_PK_NAME, W(p.suggested_name));
    g_pk_last_in = in.wstring();
    mappack::assign_folders(g_pack, pk_name());
    pk_fill();
    int n = 0;
    for (auto &e : g_pack.maps) n += e.include;
    SetWindowTextW(g_log, L"");
    append_log(L"Map pack: " + W(g_pack.maps_dir.u8string()));
    for (auto &line : g_pack.info) append_log(L"   " + W(line));
    append_log(L"   " + std::to_wstring(g_pack.maps.size()) + L" maps found, " + std::to_wstring(n) +
               L" ticked. Click a map to see its picture and text; double-click a folder name to change it.");
    // suggest the logo from the pack's own main menu, unless a logo was already picked for this pack or a PNG is set
    std::wstring logo = get_text(ID_PK_LOGO);
    if (!same_path(in, fs::path(g_pk_logo_for)) && (logo.empty() || pk_is_menu_text(logo))) {
        if (!g_pack.menu_logos.empty()) pk_use_menu_logo(0, true);
        else set_text(ID_PK_LOGO, L"");
        pk_logo_chosen();
    } else if (!logo.empty() && !pk_is_menu_text(logo) && !g_pack.menu_logos.empty()) {
        append_log(L"   This pack's main menu has its own logo: click \"Pack's logo\" to stamp that instead of " + fs::path(logo).filename().wstring() + L".");
    }
    pk_load_logo(true);
}
static void pk_draw(const DRAWITEMSTRUCT *di) {
    RECT r = di->rcItem;
    HDC dc = di->hDC;
    bool has = g_pk_sel >= 0 && g_pk_sel < (int)g_pack.maps.size() && g_pack.maps[g_pk_sel].preview.size() == 16384;
    if (!has) {
        FillRect(dc, &r, GetSysColorBrush(COLOR_BTNFACE));
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, g_font);
        RECT t = r;
        InflateRect(&t, -S(8), -S(8));
        DrawTextW(dc, g_pk_sel < 0 ? L"" : L"No picture: the Xbox shows its \"unknown map\" picture", -1, &t,
                  DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        return;
    }
    std::vector<uint8_t> rgba = mapcard::decode_preview(mappack::stamp_preview(g_pack.maps[g_pk_sel].preview, g_series));
    std::vector<uint8_t> bgra((size_t)mapcard::PIC_W * mapcard::PIC_H * 4);
    for (int y = 0; y < mapcard::PIC_H; y++)
        for (int x = 0; x < mapcard::PIC_W; x++) {
            const uint8_t *s = &rgba[((size_t)y * mapcard::TEX_W + x) * 4];
            uint8_t *d = &bgra[((size_t)y * mapcard::PIC_W + x) * 4];
            d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255;
        }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = mapcard::PIC_W;
    bi.bmiHeader.biHeight = -mapcard::PIC_H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, HALFTONE);
    StretchDIBits(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, 0, 0, mapcard::PIC_W, mapcard::PIC_H, bgra.data(), &bi,
                  DIB_RGB_COLORS, SRCCOPY);
}
static void pk_create() {
    if (!g_pack_loaded) { MessageBoxW(g_wnd, L"Pick a mod pack first.", L"Halo Map Porter", MB_ICONINFORMATION); return; }
    std::string name = pk_name();
    if (std::string why = mappack::pack_name_problem(name, g_pack); !why.empty()) {
        MessageBoxW(g_wnd, W("Pack name: " + why).c_str(), L"Halo Map Porter", MB_ICONINFORMATION);
        return;
    }
    int n = 0;
    for (size_t i = 0; i < g_pack.maps.size(); i++) {
        n += g_pack.maps[i].include;
        if (std::string why = mappack::folder_problem(g_pack, name, i); !why.empty()) {
            MessageBoxW(g_wnd, W(g_pack.maps[i].name + ": " + why).c_str(), L"Halo Map Porter", MB_ICONINFORMATION);
            return;
        }
    }
    if (!n) { MessageBoxW(g_wnd, L"Tick at least one map.", L"Halo Map Porter", MB_ICONINFORMATION); return; }
    fs::path out(get_text(ID_PK_OUT));
    if (out.empty()) {
        std::wstring f = pick_folder(L"");
        if (f.empty()) return;
        set_text(ID_PK_OUT, f);
        out = f;
    }
    fs::path root = out / W(name);
    if (fs::exists(root) &&
        MessageBoxW(g_wnd, (root.wstring() + L" already exists. Write into it? Maps with the same folder names are replaced.").c_str(),
                    L"Halo Map Porter", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    save_settings();
    set_busy(true);
    SetWindowTextW(g_log, L"");
    append_log(L"Map pack " + W(name) + L" -> " + root.wstring());
    if (g_series.empty()) append_log(L"No logo: the pictures stay as they are.");
    else append_log(L"Logo: " + (g_logo_menu ? get_text(ID_PK_LOGO).substr(kFromMenu.size()) + L" from the pack's main menu" : get_text(ID_PK_LOGO)));
    mappack::Pack pack = g_pack;
    std::string series = g_series;
    Config cfg = make_config();
    std::thread([pack, name, out, series, root, cfg]() mutable {
        std::string err;
        // the map browser's lines (Halo Universal XBE build 20): each map's gametypes and a player guess
        int n = 0;
        for (auto &e : pack.maps) {
            if (!e.include) continue;
            porter::MapTraits t;
            if (!porter::analyze_map_traits(cfg, e.map_file, t)) continue;
            mapcard::Card c;
            c.desc = e.desc;
            mapcard::apply_analysis(c, t.gametypes, t.pmin, t.pmax, t.genres);
            e.lines = mapcard::info_lines(c);
            n++;
        }
        if (n) post_log("Read each map's gametypes and spawn points for the map list's sorts (" + std::to_string(n) + " maps).");
        bool ok = mappack::write_pack(pack, name, out, series, post_log, err);
        auto *d = new Done{ok, ok ? L"DONE: Map pack written -> " + root.wstring() + L"\\  (copy the " + W(name) +
                                        L" folder into the Xbox's maps folder; it needs a Halo Universal XBE with map packs)"
                                  : W("FAILED: " + err),
                           ok ? (root / L"order.txt").wstring() : L"", L""};
        PostMessageW(g_wnd, WM_APP_DONE, 0, (LPARAM)d);
    }).detach();
}
static void create_pack_page() {
    const int P = 3;
    int y = 48;
    label(P, L"Mod pack:", 26, y, 76, ID_PK_IN_LBL);
    edit(P, 104, y, 526, ID_PK_IN);
    button(P, L"Browse...", 638, y - 1, 96, ID_PK_IN_BROWSE);
    y += 32;
    label(P, L"Pack name:", 26, y, 76, ID_PK_NAME_LBL);
    edit(P, 104, y, 110, ID_PK_NAME);
    label(P, L"Logo:", 226, y, 76, ID_PK_LOGO_LBL);
    edit(P, 304, y, 170, ID_PK_LOGO);
    button(P, L"Browse...", 480, y - 1, 76, ID_PK_LOGO_BROWSE);
    button(P, L"Pack's logo", 562, y - 1, 84, ID_PK_LOGO_MENU);
    button(P, L"No logo", 652, y - 1, 82, ID_PK_LOGO_NONE);
    y += 32;
    label(P, L"Logo size:", 26, y, 76, ID_PK_SIZE_LBL);
    list_combo(P, 104, y, 110, ID_PK_SIZE, {L"24 pixels", L"32 pixels", L"48 pixels", L"64 pixels"});
    label(P, L"Corner:", 226, y, 76, ID_PK_CORNER_LBL);
    list_combo(P, 304, y, 140, ID_PK_CORNER, {L"Bottom right", L"Bottom left", L"Top right", L"Top left"});
    add(P, L"STATIC", L"", SS_ENDELLIPSIS, 454, y + 3, 280, 20, ID_PK_HINT);
    y += 32;
    HWND lv = add(P, WC_LISTVIEWW, L"", WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL | LVS_EDITLABELS,
                  26, y, 432, 246, ID_PK_LIST);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const struct { const wchar_t *t; int w; } cols[] = {{L"Folder", 140}, {L"Menu name", 120}, {L"Note", 150}};
    for (int i = 0; i < 3; i++) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = (LPWSTR)cols[i].t;
        c.cx = S(cols[i].w);
        ListView_InsertColumn(lv, i, &c);
    }
    add(P, L"STATIC", L"", SS_OWNERDRAW, 470, y, 175, 143, ID_PK_PIC);   // 140x114, as on the Xbox (with the logo)
    HWND nm = add(P, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 470, y + 150, 264, 24, ID_PK_MNAME, WS_EX_CLIENTEDGE);
    SendMessageW(nm, EM_SETCUEBANNER, TRUE, (LPARAM)L"Menu name");
    add(P, L"EDIT", L"", WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, 470, y + 178, 264, 68, ID_PK_DESC,
        WS_EX_CLIENTEDGE);
    add(P, L"STATIC", L"As the Xbox shows it, with the logo.", 0, 652, y + 2, 82, 60, ID_PK_MNAME_LBL);
    y += 254;
    label(P, L"Save into:", 26, y, 76, ID_PK_OUT_LBL);
    edit(P, 104, y, 340, ID_PK_OUT);
    button(P, L"Browse...", 450, y - 1, 76, ID_PK_OUT_BROWSE);
    button(P, L"Map list...", 532, y - 1, 114, ID_PK_MAPLIST);
    button(P, L"Create pack", 652, y - 1, 82, ID_PK_CREATE, true);

    set_text(ID_PK_IN, ini_get(L"pk_in"));
    set_text(ID_PK_OUT, ini_get(L"pk_out"));
    set_text(ID_PK_LOGO, ini_get(L"pk_logo"));
    g_pk_logo_for = ini_get(L"pk_logo_for");
    SendDlgItemMessageW(g_wnd, ID_PK_SIZE, CB_SETCURSEL, std::clamp(_wtoi(ini_get(L"pk_size", L"1").c_str()), 0, 3), 0);
    SendDlgItemMessageW(g_wnd, ID_PK_CORNER, CB_SETCURSEL, std::clamp(_wtoi(ini_get(L"pk_corner", L"0").c_str()), 0, 3), 0);
    pk_load_logo(true);
    pk_select(-1);
    pk_hint();
}
// map packs: WM_NOTIFY from the list (true = handled; *res = the result)
static bool pk_notify(NMHDR *nh, LRESULT *res) {
    if (nh->idFrom != ID_PK_LIST) return false;
    *res = 0;
    if (nh->code == LVN_ITEMCHANGED && !g_pk_filling) {
        auto *nv = (NMLISTVIEW *)nh;
        int i = nv->iItem;
        if (i < 0 || i >= (int)g_pack.maps.size()) return true;
        if ((nv->uChanged & LVIF_STATE) && ((nv->uNewState ^ nv->uOldState) & LVIS_STATEIMAGEMASK)) {
            bool on = ListView_GetCheckState(pk_list(), i) != 0;
            if (on != g_pack.maps[i].include) {
                g_pack.maps[i].include = on;
                if (on) pk_auto_folder(i);
                pk_set_row(i);
                pk_hint();
            }
        }
        if ((nv->uChanged & LVIF_STATE) && (nv->uNewState & LVIS_SELECTED) && !(nv->uOldState & LVIS_SELECTED)) pk_select(i);
        return true;
    }
    if (nh->code == LVN_BEGINLABELEDITW) {
        auto *di = (NMLVDISPINFOW *)nh;
        *res = di->item.iItem < 0 || di->item.iItem >= (int)g_pack.maps.size() || !g_pack.maps[di->item.iItem].include;
        return true;
    }
    if (nh->code == LVN_ENDLABELEDITW) {
        auto *di = (NMLVDISPINFOW *)nh;
        int i = di->item.iItem;
        if (!di->item.pszText || i < 0 || i >= (int)g_pack.maps.size()) return true;
        std::string old = g_pack.maps[i].folder;
        g_pack.maps[i].folder = mappack::clean_name(N(di->item.pszText), mappack::KEY_MAX);
        if (std::string why = mappack::folder_problem(g_pack, pk_name(), i); !why.empty()) {
            g_pack.maps[i].folder = old;
            MessageBoxW(g_wnd, W(why).c_str(), L"Halo Map Porter", MB_ICONINFORMATION);
        } else {
            g_pk_folders_edited = true;
        }
        pk_set_row(i);
        return true;
    }
    return false;
}
// map packs: WM_COMMAND (true = handled)
static bool pk_command(int id, int code) {
    switch (id) {
    case ID_PK_IN_BROWSE: {
        std::wstring f = pick_folder(get_text(ID_PK_IN));
        if (!f.empty()) { set_text(ID_PK_IN, f); pk_load(); save_settings(); }
        return true;
    }
    case ID_PK_IN:
        if (code == EN_KILLFOCUS && get_text(ID_PK_IN) != g_pk_last_in) pk_load();
        return true;
    case ID_PK_NAME:
        if (code == EN_CHANGE) pk_hint();
        if (code == EN_KILLFOCUS) {
            std::string clean = mappack::clean_name(pk_name(), 20);   // "CEX+" -> "CEX"
            if (clean != pk_name()) set_text(ID_PK_NAME, W(clean));
            if (g_pack_loaded && !g_pk_folders_edited) {             // folders fit the new name
                mappack::assign_folders(g_pack, pk_name());
                for (size_t i = 0; i < g_pack.maps.size(); i++) pk_set_row((int)i);
            }
            pk_hint();
        }
        return true;
    case ID_PK_LOGO_BROWSE: {
        std::wstring f = pick_file(get_text(ID_PK_LOGO), L"Pictures (*.png;*.bmp;*.gif;*.jpg;*.tif)\0*.png;*.bmp;*.gif;*.jpg;*.jpeg;*.tif;*.tiff\0All files\0*.*\0",
                                   false, L"png");
        if (!f.empty()) { set_text(ID_PK_LOGO, f); pk_load_logo(false); pk_logo_chosen(); save_settings(); }
        return true;
    }
    case ID_PK_LOGO:
        if (code == EN_KILLFOCUS) pk_load_logo(true);
        return true;
    case ID_PK_LOGO_MENU: {
        if (!g_pack_loaded) { MessageBoxW(g_wnd, L"Pick a mod pack first.", L"Halo Map Porter", MB_ICONINFORMATION); return true; }
        if (g_pack.menu_logos.empty()) {
            MessageBoxW(g_wnd, L"Couldn't find a logo on this pack's main menu (it may still show the HALO logo). Browse for a PNG instead.",
                        L"Halo Map Porter", MB_ICONINFORMATION);
            return true;
        }
        int k = pk_menu_logo_index(get_text(ID_PK_LOGO));
        pk_use_menu_logo(k < 0 ? 0 : (k + 1) % (int)g_pack.menu_logos.size(), false);
        pk_logo_chosen();
        save_settings();
        return true;
    }
    case ID_PK_LOGO_NONE:
        set_text(ID_PK_LOGO, L"");
        pk_load_logo(true);
        pk_logo_chosen();
        save_settings();
        return true;
    case ID_PK_SIZE: case ID_PK_CORNER:
        if (code == CBN_SELCHANGE) { pk_update_series(); save_settings(); }
        return true;
    case ID_PK_MNAME:
        if (code == EN_CHANGE && !g_pk_filling && g_pk_sel >= 0) { g_pack.maps[g_pk_sel].name = N(get_text(ID_PK_MNAME)); pk_set_row(g_pk_sel); }
        return true;
    case ID_PK_DESC:
        if (code == EN_CHANGE && !g_pk_filling && g_pk_sel >= 0) g_pack.maps[g_pk_sel].desc = no_cr(get_text(ID_PK_DESC));
        return true;
    case ID_PK_OUT_BROWSE: {
        std::wstring f = pick_folder(get_text(ID_PK_OUT));
        if (!f.empty()) { set_text(ID_PK_OUT, f); save_settings(); }
        return true;
    }
    case ID_PK_MAPLIST: {   // genres, stars, favorites and hidden maps for a whole maps folder at once
        std::wstring dir = ini_get(L"ml_dir"), stats = ini_get(L"ml_stats");
        if (dir.empty()) dir = get_text(ID_PK_OUT);
        Config cfg = make_config();
        maplist::edit_map_list(g_wnd, dir, stats, [cfg](const fs::path &map, maplist::Scan &out, std::string &err) {
            porter::MapTraits t;
            if (!porter::analyze_map_traits(cfg, map, t, &err)) return false;
            out.gametypes = t.gametypes;
            out.pmin = t.pmin;
            out.pmax = t.pmax;
            out.genres = t.genres;
            out.why = t.players_why;
            return true;
        });
        ini_set(L"ml_dir", dir);
        ini_set(L"ml_stats", stats);
        return true;
    }
    case ID_PK_CREATE:
        if (!g_busy) pk_create();
        return true;
    }
    return false;
}

static void create_controls() {
    g_tab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, S(10), S(10), S(740), S(474),
                            g_wnd, (HMENU)ID_TAB, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(g_tab, WM_SETFONT, (WPARAM)g_font, TRUE);
    const wchar_t *tabs[] = {L"  Convert a map  ", L"  Build from my tags  ", L"  Xbox  ", L"  Map packs  ", L"  Settings  "};
    for (int i = 0; i < 5; i++) {
        TCITEMW ti{};
        ti.mask = TCIF_TEXT;
        ti.pszText = (LPWSTR)tabs[i];
        TabCtrl_InsertItem(g_tab, i, &ti);
    }

    // Convert page
    int y = 50;
    label(0, L"Map to convert:", 26, y, 110, ID_IN_LBL);
    edit(0, 140, y, 490, ID_IN);
    button(0, L"Browse...", 638, y - 1, 96, ID_IN_BROWSE);
    y += 36;
    label(0, L"From game:", 26, y, 110, ID_FROM_LBL);
    combo(0, 140, y, 220, ID_FROM);
    add(0, L"STATIC", L"", SS_ENDELLIPSIS, 372, y + 3, 360, 20, ID_DETECT);
    y += 36;
    label(0, L"To game:", 26, y, 110, ID_TO_LBL);
    combo(0, 140, y, 220, ID_TO);
    label(0, L"Xbox name:", 380, y, 70, ID_C_XNAME_LBL);
    {
        HWND c = add(0, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 454, y, 176, 400, ID_C_XNAME);
        SendMessageW(c, CB_SETDROPPEDWIDTH, S(230), 0);
    }
    button(0, L"Map card...", 380, y - 1, 250, ID_C_CARD);
    y += 36;
    label(0, L"Save as:", 26, y, 110, ID_OUT_LBL);
    edit(0, 140, y, 490, ID_OUT);
    button(0, L"Browse...", 638, y - 1, 96, ID_OUT_BROWSE);
    y += 34;
    check(0, L"Use Custom Edition's shared bitmaps / sounds / loc maps (much smaller map)", 140, y, 590, ID_SHARE);
    y += 28;
    add(0, L"STATIC", L"Xbox output uses the options on the Xbox tab (map list, vehicle slots, PC weapons, size).", 0, 140, y + 3, 590, 20, ID_XSHRINK);
    y += 34;
    button(0, L"Convert", 140, y, 140, ID_CONVERT, true);
    add(0, L"STATIC", L"", 0, 292, y - 1, 440, 36, ID_CONV_HINT);

    // Build page
    y = 50;
    label(1, L"Scenario tag:", 26, y, 110, ID_SCEN_LBL);
    edit(1, 140, y, 490, ID_SCEN);
    button(1, L"Browse...", 638, y - 1, 96, ID_SCEN_BROWSE);
    y += 40;
    check(1, L"MCC  \x2192", 26, y + 1, 110, ID_B_MCC);
    edit(1, 140, y, 490, ID_B_MCC_OUT);
    button(1, L"Folder...", 638, y - 1, 96, ID_B_MCC_BROWSE);
    y += 36;
    check(1, L"Custom Ed.  \x2192", 26, y + 1, 110, ID_B_CE);
    edit(1, 140, y, 490, ID_B_CE_OUT);
    button(1, L"Folder...", 638, y - 1, 96, ID_B_CE_BROWSE);
    y += 36;
    check(1, L"Xbox  \x2192", 26, y + 1, 110, ID_B_XBOX);
    edit(1, 140, y, 300, ID_B_XBOX_OUT);
    add(1, L"STATIC", L"named:", 0, 448, y + 3, 46, 20, ID_B_XBOX_NAME_LBL);
    {
        HWND c = add(1, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 496, y, 134, 400, ID_B_XBOX_NAME);
        SendMessageW(c, CB_SETDROPPEDWIDTH, S(230), 0);
    }
    button(1, L"Map card...", 448, y - 1, 182, ID_B_CARD);
    button(1, L"Folder...", 638, y - 1, 96, ID_B_XBOX_BROWSE);
    y += 36;
    check(1, L"Use Custom Edition's shared bitmaps / sounds / loc maps for the CE build (much smaller map)", 140, y, 590, ID_B_SHARE);
    y += 28;
    add(1, L"STATIC", L"The Xbox build uses the options on the Xbox tab (map list, vehicle slots, PC weapons, size).", 0, 140, y + 3, 590, 20, ID_B_XSHRINK);
    y += 34;
    button(1, L"Build", 140, y, 140, ID_BUILD, true);
    add(1, L"STATIC", L"Re-run after every change. Your tags folder is never modified.",
        0, 292, y + 5, 440, 20, ID_BUILD_HINT);

    // Xbox page
    y = 48;
    check(2, L"Shrink textures to fit:", 26, y, 168, ID_X_SHRINK);
    list_combo(2, 196, y - 1, 194, ID_X_TARGET, {L"22 MB - Max", L"20 MB - Recommended", L"18 MB - Safe"});
    label(2, L"Size limit:", 410, y, 70, ID_X_LIMIT_LBL);
    list_combo(2, 484, y - 1, 250, ID_X_LIMIT, {L"Standard Xbox (47 MB)", L"Big maps (115 MB, modded Xbox)"});
    y += 32;
    label(2, L"Map names:", 26, y, 140, ID_X_MAPLIST_LBL);
    list_combo(2, 170, y, 220, ID_X_MAPLIST,
               {L"Standard (13 stock maps)", L"Expanded map pack (27 maps)", L"Map folders (Halo Universal XBE)"});
    add(2, L"STATIC", L"Which names the \"Xbox name\" lists offer. The map you port takes the name of the map it replaces.",
        0, 410, y, 324, 36, ID_X_MAPLIST_HINT);
    y += 36;
    check(2, L"Use the Xbox's own weapons, vehicles and player model instead of the map's custom copies (much smaller)",
          26, y, 708, ID_X_STOCK);
    y += 30;
    label(2, L"Max texture size:", 26, y, 140, ID_X_TEX_LBL);
    list_combo(2, 170, y, 220, ID_X_TEX, {L"Stock-like budget (smoothest)", L"512 max (sharper)", L"1024", L"No limit"});
    label(2, L"Lightmaps:", 410, y, 70, ID_X_LM_LBL);
    list_combo(2, 484, y, 250, ID_X_LM, {L"Keep their size", L"Max 512", L"Max 256 (like stock maps)"});
    y += 32;
    add(2, L"STATIC", L"Vehicle slots - the Xbox only spawns these three. The gametype's vehicle options pick them by slot:",
        0, 26, y, 708, 20, ID_X_VLBL);
    y += 24;
    {
        std::vector<const wchar_t *> v;
        for (auto &c : kVehicleChoices) v.push_back(c.label);
        const wchar_t *names[3] = {L"\"Warthog\" slot:", L"\"Ghost\" slot:", L"\"Scorpion\" slot:"};
        const int lids[3] = {ID_X_V0_LBL, ID_X_V1_LBL, ID_X_V2_LBL}, vids[3] = {ID_X_V0, ID_X_V1, ID_X_V2};
        for (int i = 0; i < 3; i++) {
            label(2, names[i], 46, y, 120, lids[i]);
            list_combo(2, 170, y, 220, vids[i], v);
            y += 30;
        }
    }
    check(2, L"Keep every vehicle (Halo Universal XBE)", 410, y - 88, 324, ID_X_ALLVEH);
    add(2, L"STATIC", L"", 0, 410, y - 62, 324, 58, ID_X_VHINT);
    y += 2;
    add(2, L"STATIC", L"Weapons Xbox multiplayer doesn't have:", 0, 26, y, 480, 20, ID_X_WLBL);
    button(2, L"Weapons && vehicles...", 534, y - 4, 200, ID_X_CONTENTS);
    y += 24;
    {
        std::vector<const wchar_t *> w;
        for (auto &c : kWeaponChoices) w.push_back(c.label);
        label(2, L"Flamethrower:", 46, y, 120, ID_X_W0_LBL);
        list_combo(2, 170, y, 220, ID_X_W0, w);
        add(2, L"STATIC", L"Kept weapons get the PC pickup animations and icons automatically.", 0, 410, y + 3, 324, 40,
            ID_X_WHINT);
        y += 29;
        label(2, L"Fuel rod gun:", 46, y, 120, ID_X_W1_LBL);
        list_combo(2, 170, y, 220, ID_X_W1, w);
        y += 29;
        std::vector<const wchar_t *> o = w;
        o[0] = L"Keep them";
        label(2, L"Other custom ones:", 46, y, 124, ID_X_W2_LBL);
        list_combo(2, 170, y, 220, ID_X_W2, o);
        add(2, L"STATIC", L"e.g. a custom tagset's beam rifle. Replacing them saves memory on big maps.", 0, 410, y - 2, 324, 36,
            ID_X_W2_HINT);
    }
    y += 32;
    check(2, L"Make lower-detail versions (LODs) when a model has none", 26, y, 380, ID_X_MAKELOD);
    label(2, L"Heavy models:", 410, y - 2, 86, ID_X_TRIM_LBL);
    list_combo(2, 500, y - 2, 234, ID_X_TRIM,
               {L"Leave them as made", L"Trim (they look the same)", L"Trim harder (details change)",
                L"Trim to 1.5x Bungie's density"});
    y += 24;
    check(2, L"Also use LODs of Bungie's trees, rocks and plants (retail didn't)", 26, y, 380, ID_X_LODSTOCK);
    label(2, L"Animations:", 410, y - 2, 86, ID_X_ANIM_LBL);
    list_combo(2, 500, y - 2, 234, ID_X_ANIM,
               {L"Leave them as made", L"Compress (they move the same)", L"Compress harder (fast moves drift)"});
    y += 24;
    label(2, L"Portals:", 26, y - 2, 60, ID_X_PORTALS_LBL);
    list_combo(2, 90, y - 2, 430, ID_X_PORTALS,
               {L"Leave every level's as they are", L"Add them to levels made without any, so walls hide what's behind",
                L"Also divide up the rooms of levels that have them, when that draws less"});
    button(2, L"Level...", 534, y - 4, 200, ID_X_SHADERS);

    // Map packs page
    create_pack_page();

    // Settings page
    y = 50;
    label(4, L"Custom Edition folder:", 26, y, 160, ID_S_CE_LBL);
    edit(4, 190, y, 440, ID_S_CE);
    button(4, L"Browse...", 638, y - 1, 96, ID_S_CE_BROWSE);
    y += 36;
    label(4, L"MCC maps folder:", 26, y, 160, ID_S_MCC_LBL);
    edit(4, 190, y, 440, ID_S_MCC);
    button(4, L"Browse...", 638, y - 1, 96, ID_S_MCC_BROWSE);
    y += 36;
    label(4, L"H1A-EK folder (optional):", 26, y, 160, ID_S_EK_LBL);
    edit(4, 190, y, 440, ID_S_EK);
    button(4, L"Browse...", 638, y - 1, 96, ID_S_EK_BROWSE);
    y += 36;
    label(4, L"Xbox tags folder:", 26, y, 160, ID_S_XT_LBL);
    edit(4, 190, y, 440, ID_S_XT);
    button(4, L"Browse...", 638, y - 1, 96, ID_S_XT_BROWSE);
    y += 36;
    label(4, L"Xbox maps (if no tags):", 26, y, 160, ID_S_XM_LBL);
    edit(4, 190, y, 440, ID_S_XM);
    button(4, L"Browse...", 638, y - 1, 96, ID_S_XM_BROWSE);
    y += 36;
    label(4, L"Invader folder:", 26, y, 160, ID_S_INV_LBL);
    edit(4, 190, y, 440, ID_S_INV);
    button(4, L"Browse...", 638, y - 1, 96, ID_S_INV_BROWSE);
    y += 34;
    button(4, L"Download Invader", 190, y, 140, ID_S_INV_DL);
    add(4, L"STATIC", L"", 0, 342, y + 5, 390, 20, ID_S_INV_STATUS);
    y += 34;
    add(4, L"STATIC",
        L"Xbox tags folder: an Xbox tagset such as haloxbox-revised (recommended). Xbox maps: your own Xbox stock "
        L"maps, used only if no Xbox tags folder is set. See README.txt for details.",
        0, 26, y, 708, 36, ID_S_HINT);
    y += 46;
    label(4, L"LOD library folder:", 26, y, 160, ID_S_LOD_LBL);
    edit(4, 190, y, 440, ID_S_LOD);
    button(4, L"Browse...", 638, y - 1, 96, ID_S_LOD_BROWSE);
    y += 30;
    add(4, L"STATIC", L"The lower-detail versions the app makes are kept here, so the same model in another map reuses them, "
        L"with your choices for models from the LODs window (choices.txt).", 0, 190, y, 544, 36, ID_S_LOD_HINT);
    y += 44;
    check(4, L"With each Xbox build, write a camera tour for Halo 1 Unlimited's perf log (perf\\tours, beside the map)", 26, y,
          708, ID_S_TOUR);

    // Shared area
    g_log = add(-1, L"EDIT", L"", WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 10, 494, 740, 250, ID_LOG,
                WS_EX_CLIENTEDGE);
    SendMessageW(g_log, WM_SETFONT, (WPARAM)g_mono, TRUE);
    SendMessageW(g_log, EM_SETLIMITTEXT, 0, 0);
    add(-1, L"BUTTON", L"Open output folder", BS_PUSHBUTTON, 10, 752, 150, 26, ID_OPEN_FOLDER);
    add(-1, L"BUTTON", L"Open report", BS_PUSHBUTTON, 168, 752, 120, 26, ID_OPEN_REPORT);
    add(-1, L"BUTTON", L"LODs && collision", BS_PUSHBUTTON, 296, 752, 130, 26, ID_SEE_LODS);
    ShowWindow(GetDlgItem(g_wnd, ID_OPEN_FOLDER), SW_HIDE);
    ShowWindow(GetDlgItem(g_wnd, ID_OPEN_REPORT), SW_HIDE);
    ShowWindow(GetDlgItem(g_wnd, ID_SEE_LODS), SW_HIDE);

    // Load settings
    std::wstring ce = ini_get(L"ce_dir");
    if (ce.empty())
        for (auto c : {L"D:\\Games\\HaloCustomEdition", L"C:\\Program Files (x86)\\Microsoft Games\\Halo Custom Edition"})
            if (fs::exists(fs::path(c) / L"maps" / L"bitmaps.map")) { ce = c; break; }
    set_text(ID_S_CE, ce);
    std::wstring mcc = ini_get(L"mcc_maps");
    if (mcc.empty()) {
        fs::path m = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Halo The Master Chief Collection\\halo1\\maps";
        if (fs::exists(m)) mcc = m.wstring();
    }
    set_text(ID_S_MCC, mcc);
    set_text(ID_S_EK, ini_get(L"ek_dir"));
    std::wstring xt = ini_get(L"xbox_tags");
    if (xt.empty())
        for (auto c : {L"D:\\SteamLibrary\\steamapps\\common\\HCEEK\\tags_xbox",
                       L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\HCEEK\\tags_xbox"})
            if (fs::exists(fs::path(c) / L"globals" / L"globals.globals")) { xt = c; break; }
    set_text(ID_S_XT, xt);
    set_text(ID_S_XM, ini_get(L"xbox_maps"));
    set_text(ID_B_XBOX_OUT, ini_get(L"b_xbox_out"));
    SendDlgItemMessageW(g_wnd, ID_X_MAPLIST, CB_SETCURSEL, std::clamp(_wtoi(ini_get(L"x_maplist", L"0").c_str()), 0, 2), 0);
    SendDlgItemMessageW(g_wnd, ID_X_LIMIT, CB_SETCURSEL, ini_get(L"x_limit", L"0") == L"1" ? 1 : 0, 0);
    set_check(ID_X_STOCK, ini_get(L"x_stock", L"1") == L"1");
    set_check(ID_X_ALLVEH, ini_get(L"x_allveh", L"1") == L"1");
    set_check(ID_X_LODSTOCK, ini_get(L"x_lodstock", L"0") == L"1");
    set_check(ID_X_MAKELOD, ini_get(L"x_makelod", L"1") == L"1");
    SendDlgItemMessageW(g_wnd, ID_X_TRIM, CB_SETCURSEL, std::clamp(_wtoi(ini_get(L"x_trim", L"1").c_str()), 0, 3), 0);
    SendDlgItemMessageW(g_wnd, ID_X_ANIM, CB_SETCURSEL, std::clamp(_wtoi(ini_get(L"x_anim", L"1").c_str()), 0, 2), 0);
    SendDlgItemMessageW(g_wnd, ID_X_PORTALS, CB_SETCURSEL, std::clamp(_wtoi(ini_get(L"x_portals", L"1").c_str()), 0, 2), 0);
    {
        std::wstring lib = ini_get(L"lod_library");
        set_text(ID_S_LOD, lib.empty() ? (appdata_dir(CSIDL_LOCAL_APPDATA) / L"lod_library").wstring() : lib);
    }
    for (auto [id, key, mx, def] : {std::tuple<int, const wchar_t *, int, const wchar_t *>{ID_X_TEX, L"x_tex2", 3, L"0"},
                                    {ID_X_LM, L"x_lm", 2, L"2"}}) {
        int k = _wtoi(ini_get(key, def).c_str());
        SendDlgItemMessageW(g_wnd, id, CB_SETCURSEL, k < 0 || k > mx ? 0 : k, 0);
    }
    {
        int k = _wtoi(ini_get(L"x_w2", L"0").c_str());
        SendDlgItemMessageW(g_wnd, ID_X_W2, CB_SETCURSEL, k < 0 ? 0 : k, 0);
    }
    std::wstring inv = ini_get(L"invader_dir");
    if (inv.empty()) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        for (fs::path c : {fs::path(exe).parent_path() / L"invader", appdata_dir(CSIDL_LOCAL_APPDATA) / L"invader"}) {
            std::error_code ec;
            if (fs::exists(c))
                for (auto &e : fs::recursive_directory_iterator(c, ec))
                    if (e.path().filename() == L"invader-build.exe") { inv = e.path().parent_path().wstring(); break; }
            if (!inv.empty()) break;
        }
    }
    set_text(ID_S_INV, inv);
    set_text(ID_IN, ini_get(L"last_input"));
    set_text(ID_SCEN, ini_get(L"scenario"));
    g_build_own = lower(fs::path(get_text(ID_SCEN)).stem().wstring());
    fill_map_combo(ID_B_XBOX_NAME, g_build_own, ini_get(L"b_xbox_name"));
    fill_map_combo(ID_C_XNAME, g_conv_own, L"");
    set_text(ID_B_MCC_OUT, ini_get(L"b_mcc_out", mcc.c_str()));
    set_text(ID_B_CE_OUT, ini_get(L"b_ce_out", ce.empty() ? L"" : (fs::path(ce) / L"maps").c_str()));
    set_check(ID_B_MCC, ini_get(L"b_mcc", L"1") == L"1");
    set_check(ID_B_CE, ini_get(L"b_ce", L"1") == L"1");
    set_check(ID_SHARE, ini_get(L"share", L"1") == L"1");
    set_check(ID_B_SHARE, ini_get(L"share", L"1") == L"1");
    set_check(ID_X_SHRINK, ini_get(L"x_shrink", L"1") == L"1");
    set_check(ID_S_TOUR, ini_get(L"write_tour", L"0") == L"1");
    {
        int k = _wtoi(ini_get(L"x_target", L"1").c_str());
        SendDlgItemMessageW(g_wnd, ID_X_TARGET, CB_SETCURSEL, k >= 0 && k < 3 ? k : 1, 0);
    }
    {
        const int vids[3] = {ID_X_V0, ID_X_V1, ID_X_V2};
        for (int i = 0; i < 3; i++) {
            int k = _wtoi(ini_get((L"x_v" + std::to_wstring(i)).c_str(), std::to_wstring(kVehicleDefaults[i]).c_str()).c_str());
            SendDlgItemMessageW(g_wnd, vids[i], CB_SETCURSEL, k < 0 ? kVehicleDefaults[i] : k, 0);
        }
        const int wids[2] = {ID_X_W0, ID_X_W1};
        for (int i = 0; i < 2; i++) {
            int k = _wtoi(ini_get((L"x_w" + std::to_wstring(i)).c_str(), L"0").c_str());
            SendDlgItemMessageW(g_wnd, wids[i], CB_SETCURSEL, k < 0 ? 0 : k, 0);
        }
    }
    set_check(ID_B_XBOX, ini_get(L"b_xbox", L"0") == L"1");
    SendDlgItemMessageW(g_wnd, ID_FROM, CB_SETCURSEL, 0, 0);
    SendDlgItemMessageW(g_wnd, ID_TO, CB_SETCURSEL, 1, 0);
    if (!get_text(ID_IN).empty()) on_input_chosen();
    else update_out_path();
    update_invader_status();

    Config c = make_config();
    if (!invader_ok(c)) {
        TabCtrl_SetCurSel(g_tab, 4);
        show_page(4);
        append_log(L"Welcome! First, set up Invader (the map compiler) on the Settings tab: click \"Download Invader\".");
    } else {
        show_page(0);
        append_log(L"Ready. Pick a map to convert, or use \"Build from my tags\" while you work on a map.");
    }
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_wnd = h;
        create_controls();
        return 0;
    case WM_NOTIFY: {
        LRESULT res;
        if (pk_notify((NMHDR *)lp, &res)) return res;
        if (((NMHDR *)lp)->idFrom == ID_TAB && ((NMHDR *)lp)->code == TCN_SELCHANGE) {
            int pg = TabCtrl_GetCurSel(g_tab);
            show_page(pg);
            if (pg == 3 && !g_pack_loaded && !get_text(ID_PK_IN).empty() && fs::exists(fs::path(get_text(ID_PK_IN)))) pk_load();
            save_settings();
        }
        return 0;
    }
    case WM_DRAWITEM:
        if (wp == ID_PK_PIC) { pk_draw((const DRAWITEMSTRUCT *)lp); return TRUE; }
        break;
    case WM_CTLCOLORSTATIC: {
        // Plain window background for labels; keep the read-only log white.
        if ((HWND)lp == g_log) return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (pk_command(id, code)) return 0;
        switch (id) {
        case ID_IN_BROWSE: {
            auto f = pick_file(get_text(ID_IN), L"Halo maps (*.map)\0*.map\0All files\0*.*\0", false, L"map");
            if (!f.empty()) { set_text(ID_IN, f); on_input_chosen(); }
            break;
        }
        case ID_IN:
            if (code == EN_KILLFOCUS) on_input_chosen();
            break;
        case ID_TO: case ID_FROM:
            if (code == CBN_SELCHANGE) update_out_path();
            break;
        case ID_OUT:
            if (code == EN_CHANGE && GetFocus() == GetDlgItem(h, ID_OUT)) {
                g_out_manual = true;
                sync_conv_name_from_out();
                update_conv_hint();
            }
            break;
        case ID_OUT_BROWSE: {
            auto f = pick_file(get_text(ID_OUT), L"Halo maps (*.map)\0*.map\0", true, L"map");
            if (!f.empty()) {
                set_text(ID_OUT, f);
                g_out_manual = true;
                sync_conv_name_from_out();
                update_conv_hint();
            }
            break;
        }
        case ID_C_XNAME:
            if (code == CBN_SELCHANGE) on_conv_name_chosen();
            break;
        case ID_X_ALLVEH:
            if (code == BN_CLICKED) { update_vehicle_texts(); save_settings(); }
            break;
        case ID_X_LODSTOCK: case ID_X_MAKELOD:
            if (code == BN_CLICKED) save_settings();
            break;
        case ID_X_PORTALS: case ID_X_TRIM: case ID_X_ANIM:
            if (code == CBN_SELCHANGE) save_settings();
            break;
        case ID_X_CONTENTS:
            if (code == BN_CLICKED) open_contents();
            break;
        case ID_X_SHADERS:
            if (code == BN_CLICKED) open_shaders();
            break;
        case ID_X_MAPLIST:
            if (code == CBN_SELCHANGE) {
                on_map_list_changed();
                apply_mode_visibility(TabCtrl_GetCurSel(g_tab));
                save_settings();
            }
            break;
        case ID_B_CARD: {
            fs::path scen(get_text(ID_SCEN));
            if (scen.empty()) { MessageBoxW(h, L"Pick your .scenario tag first.", L"Halo Map Porter", MB_ICONINFORMATION); break; }
            fs::path xo(get_text(ID_B_XBOX_OUT));
            std::wstring name = build_map_name();
            auto target = [xo, name](const mapcard::Card &c) {
                return xo.empty() || !mapcard::folder_problem(c.folder).empty() ? fs::path() : xo / W(c.folder) / (name + L".map");
            };
            mapcard::edit_card(h, build_card_dir(), N(name), name + L".map", L"Map card - " + scen.stem().wstring(),
                               [target](const mapcard::Card &c) { return card_where(target(c), L"when you build."); });
            refresh_card_files(build_card_dir(), name, target(build_card()));
            break;
        }
        case ID_C_CARD: {
            fs::path in(get_text(ID_IN));
            if (in.empty() || !fs::exists(in)) { MessageBoxW(h, L"Pick a map to convert first.", L"Halo Map Porter", MB_ICONINFORMATION); break; }
            // where the map is or will be: the Save as folder when it was chosen by hand, else from the card's folder
            fs::path out(get_text(ID_OUT));
            bool manual = g_out_manual, only = card_only();
            fs::path base = out.parent_path().parent_path();
            std::wstring file = only ? in.filename().wstring() : conv_map_name() + L".map";
            auto target = [=](const mapcard::Card &c) {
                if (manual) return out;
                return out.empty() || !mapcard::folder_problem(c.folder).empty() ? fs::path() : base / W(c.folder) / file;
            };
            mapcard::edit_card(h, conv_card_dir(), N(conv_card_name()), file, L"Map card - " + in.filename().wstring(),
                               [=](const mapcard::Card &c) {
                                   return card_where(target(c), only ? L"when you click \"Save map card\"." : L"when you convert.");
                               });
            if (!g_out_manual) update_out_path();
            else update_conv_hint();
            refresh_card_files(conv_card_dir(), conv_card_name(), fs::path(get_text(ID_OUT)));
            break;
        }
        case ID_SCEN:
            if (code == EN_KILLFOCUS) refresh_build_names();
            break;
        case ID_SCEN_BROWSE: {
            std::wstring start = get_text(ID_SCEN);
            if (start.empty() && !get_text(ID_S_EK).empty()) start = (fs::path(get_text(ID_S_EK)) / L"tags").wstring();
            auto f = pick_file(start, L"Scenario tags (*.scenario)\0*.scenario\0", false, L"scenario");
            if (!f.empty()) { set_text(ID_SCEN, f); refresh_build_names(); }
            break;
        }
        case ID_B_MCC_BROWSE: case ID_B_CE_BROWSE: case ID_S_CE_BROWSE: case ID_S_MCC_BROWSE:
        case ID_B_XBOX_BROWSE: case ID_S_XT_BROWSE: case ID_S_XM_BROWSE:
        case ID_S_EK_BROWSE: case ID_S_INV_BROWSE: case ID_S_LOD_BROWSE: {
            int target = id - 1;
            auto f = pick_folder(get_text(target));
            if (!f.empty()) {
                set_text(target, f);
                if (id == ID_S_INV_BROWSE) update_invader_status();
                save_settings();
            }
            break;
        }
        case ID_S_INV:
            if (code == EN_KILLFOCUS) update_invader_status();
            break;
        case ID_CONVERT: if (!g_busy) start_convert(); break;
        case ID_BUILD: if (!g_busy) start_build(); break;
        case ID_S_INV_DL: if (!g_busy) start_download(); break;
        case ID_OPEN_FOLDER:
            if (!g_last_output.empty())
                ShellExecuteW(h, L"open", L"explorer.exe", (L"/select,\"" + g_last_output + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
            break;
        case ID_OPEN_REPORT:
            if (!g_last_report.empty()) ShellExecuteW(h, L"open", g_last_report.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        case ID_SEE_LODS:
            if (!g_last_lods.empty()) {
                Config c = make_config();
                if (lodui::show(h, fs::path(g_last_lods), !c.lod_library.empty() ? c.lod_library : c.cache_dir / "lod_library") &&
                    !g_busy) {
                    if (g_last_was_build) start_build();
                    else start_convert(true);
                }
            }
            break;
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == kHeartbeat) {
            if (g_busy) heartbeat();
            return 0;
        }
        break;
    case WM_APP_LOG: {
        auto *s = (std::wstring *)lp;
        if (wp == 1) { set_text(ID_S_INV, *s); save_settings(); update_invader_status(); }
        else append_log(*s);
        delete s;
        return 0;
    }
    case WM_APP_DONE: {
        auto *d = (Done *)lp;
        append_log(L"");
        append_log(d->msg);
        set_busy(false);
        if (d->ok && !d->output.empty()) {
            g_last_output = d->output;
            g_last_report = d->report;
            ShowWindow(GetDlgItem(h, ID_OPEN_FOLDER), SW_SHOW);
            ShowWindow(GetDlgItem(h, ID_OPEN_REPORT), d->report.empty() ? SW_HIDE : SW_SHOW);
        }
        g_last_lods = d->lods;   // (a map that didn't fit too: see what was made)
        ShowWindow(GetDlgItem(h, ID_SEE_LODS), d->lods.empty() ? SW_HIDE : SW_SHOW);
        update_invader_status();
        update_conv_hint();
        if (!d->ok) MessageBeep(MB_ICONWARNING);
        delete d;
        return 0;
    }
    case WM_CLOSE:
        if (g_busy && MessageBoxW(h, L"A job is still running. Quit anyway?", L"Halo Map Porter", MB_YESNO) != IDYES) return 0;
        save_settings();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_TAB_CLASSES | ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);
    contentsui::set_cache_folder(appdata_dir(CSIDL_LOCAL_APPDATA) / L"measurements");
    shadersui::set_cache_folder(appdata_dir(CSIDL_LOCAL_APPDATA) / L"levels");
    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);
    g_font = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_mono = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"HaloMapPorter";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);
    RECT r{0, 0, S(760), S(788)};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    HWND h = CreateWindowExW(0, L"HaloMapPorter", L"Halo Map Porter", style, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    ShowWindow(h, show);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (!IsDialogMessageW(h, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return 0;
}
