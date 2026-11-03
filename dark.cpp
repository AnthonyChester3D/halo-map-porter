// Halo Map Porter - the dark theme (dark.hpp).
#include "dark.hpp"

#include <commctrl.h>
#include <uxtheme.h>

#include <cwchar>
#include <map>

namespace dark {
namespace {

bool g_on = false;
HHOOK g_hook = nullptr;

const COLORREF kWindow = RGB(32, 32, 32);     // window and field backgrounds
const COLORREF kFace = RGB(45, 45, 45);       // raised: read-only boxes, the picked tab
const COLORREF kText = RGB(232, 232, 232);
const COLORREF kGray = RGB(140, 140, 140);
const COLORREF kEdge = RGB(80, 80, 80);       // the tabs' outlines

// Windows' dark styles, by the ordinals uxtheme.dll has them under (Windows 10 1809 and later; missing: light).
using AllowDarkModeForWindowFn = bool(WINAPI *)(HWND, bool);
using SetPreferredAppModeFn = int(WINAPI *)(int);
using FlushMenuThemesFn = void(WINAPI *)();
AllowDarkModeForWindowFn g_allow = nullptr;

using DwmSetWindowAttributeFn = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
DwmSetWindowAttributeFn g_dwm = nullptr;

using SetWindowThemeFn = HRESULT(WINAPI *)(HWND, LPCWSTR, LPCWSTR);
SetWindowThemeFn g_theme = nullptr;

void theme(HWND h, const wchar_t *app, const wchar_t *ids = nullptr) {
    if (g_allow) g_allow(h, true);
    if (g_theme) g_theme(h, app, ids);
}

bool windows_is_dark() {
    DWORD v = 1, size = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS)
        return false;
    return v == 0;
}

// A window's colour requests (WM_CTLCOLOR...): light text on the dark background, unless the window picks its own
// colours (a status line's red, a hint's grey: those stay; a brush of its own stays too).
LRESULT CALLBACK colors(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN: case WM_CTLCOLORDLG: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, kText);
        SetBkColor(dc, kWindow);
        LRESULT r = DefSubclassProc(h, msg, wp, lp);
        // (a colour the window picked for white: a hint's dark grey, a status's dark green; brightened to read on dark)
        COLORREF t = GetTextColor(dc);
        if (0.299 * GetRValue(t) + 0.587 * GetGValue(t) + 0.114 * GetBValue(t) < 130)
            SetTextColor(dc, RGB(GetRValue(t) + (255 - GetRValue(t)) * 5 / 10, GetGValue(t) + (255 - GetGValue(t)) * 5 / 10,
                                 GetBValue(t) + (255 - GetBValue(t)) * 5 / 10));
        HBRUSH b = (HBRUSH)r;
        if (!b || b == GetSysColorBrush(COLOR_WINDOW) || b == GetSysColorBrush(COLOR_3DFACE) ||
            b == (HBRUSH)GetStockObject(WHITE_BRUSH) || b == (HBRUSH)GetStockObject(LTGRAY_BRUSH))
            return (LRESULT)sysbrush(COLOR_WINDOW);
        return r;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, colors, 1);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// Tabs, drawn dark (the tab control has no dark style of its own).
LRESULT CALLBACK tabs(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(dc, &rc, sysbrush(COLOR_WINDOW));
        HBRUSH edge = CreateSolidBrush(kEdge);
        RECT page = rc;
        TabCtrl_AdjustRect(h, FALSE, &page);
        InflateRect(&page, 2, 2);
        FrameRect(dc, &page, edge);
        HGDIOBJ old = SelectObject(dc, (HFONT)SendMessageW(h, WM_GETFONT, 0, 0));
        SetBkMode(dc, TRANSPARENT);
        const int n = TabCtrl_GetItemCount(h), sel = TabCtrl_GetCurSel(h);
        for (int i = 0; i < n; i++) {
            RECT r;
            if (!TabCtrl_GetItemRect(h, i, &r)) continue;
            if (i == sel) InflateRect(&r, 2, 2);
            FillRect(dc, &r, i == sel ? sysbrush(COLOR_3DFACE) : sysbrush(COLOR_WINDOW));
            FrameRect(dc, &r, edge);
            wchar_t t[128] = {};
            TCITEMW it{};
            it.mask = TCIF_TEXT;
            it.pszText = t;
            it.cchTextMax = 127;
            TabCtrl_GetItem(h, i, &it);
            SetTextColor(dc, i == sel ? kText : RGB(190, 190, 190));
            DrawTextW(dc, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        SelectObject(dc, old);
        DeleteObject(edge);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, tabs, 2);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// A list view's column titles: its header draws them dark on the dark style; light instead (the header asks the list
// view, its parent, how to draw each title).
LRESULT CALLBACK list_titles(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_NOTIFY) {
        NMHDR *n = (NMHDR *)lp;
        if (n->code == NM_CUSTOMDRAW && n->hwndFrom == ListView_GetHeader(h)) {
            NMCUSTOMDRAW *cd = (NMCUSTOMDRAW *)lp;
            if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                SetTextColor(cd->hdc, kText);
                return CDRF_DODEFAULT;
            }
        }
    } else if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(h, list_titles, 3);
    }
    return DefSubclassProc(h, msg, wp, lp);
}

bool is(const wchar_t *cls, const wchar_t *name) { return _wcsicmp(cls, name) == 0; }

// Each window as it's made (after its WM_CREATE).
void made(HWND h) {
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    const LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE);
    if (is(cls, L"#32770") || is(cls, L"tooltips_class32") || is(cls, L"IME")) return;   // (Windows' own)
    if (!(style & WS_CHILD)) {   // a window of the app's own: its title bar, and the colours its controls ask for
        BOOL yes = TRUE;
        if (g_allow) g_allow(h, true);
        if (g_dwm && FAILED(g_dwm(h, 20, &yes, sizeof yes))) g_dwm(h, 19, &yes, sizeof yes);   // (DWMWA_USE_IMMERSIVE_DARK_MODE)
        SetWindowSubclass(h, colors, 1, 0);
        return;
    }
    if (is(cls, L"Button")) {
        const LONG_PTR type = style & BS_TYPEMASK;
        const bool box = type == BS_CHECKBOX || type == BS_AUTOCHECKBOX || type == BS_RADIOBUTTON || type == BS_3STATE ||
                         type == BS_AUTO3STATE || type == BS_AUTORADIOBUTTON || type == BS_GROUPBOX;
        // (tick boxes, option buttons and group boxes in the plain style, whose text takes the colours asked for;
        // Windows' dark style draws their text black)
        if (box && !(style & BS_PUSHLIKE)) theme(h, L"", L"");
        else theme(h, L"DarkMode_Explorer");
    } else if (is(cls, L"Edit") || is(cls, L"ListBox") || is(cls, L"ComboLBox") || is(cls, L"ScrollBar") ||
               is(cls, L"SysTreeView32")) {
        theme(h, L"DarkMode_Explorer");
    } else if (is(cls, L"ComboBox")) {
        theme(h, L"DarkMode_CFD");
    } else if (is(cls, WC_LISTVIEWW)) {
        theme(h, L"DarkMode_Explorer");
        ListView_SetBkColor(h, kWindow);
        ListView_SetTextBkColor(h, kWindow);
        ListView_SetTextColor(h, kText);
        SetWindowSubclass(h, list_titles, 3, 0);
    } else if (is(cls, WC_HEADERW)) {
        theme(h, L"ItemsView");
    } else if (is(cls, WC_TABCONTROLW)) {
        SetWindowSubclass(h, tabs, 2, 0);
    } else if (!is(cls, L"Static") && wcsncmp(cls, L"msctls_", 7) != 0 && wcsncmp(cls, L"RichEdit", 8) != 0) {
        SetWindowSubclass(h, colors, 1, 0);   // (a panel of the app's own, with controls of its own)
    }
}

LRESULT CALLBACK hook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const CWPRETSTRUCT *c = (const CWPRETSTRUCT *)lp;
        if (c->message == WM_CREATE && c->lResult != -1) made(c->hwnd);
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

}   // namespace

void init(int mode) {
    g_on = mode == 1 || (mode == 2 && windows_is_dark());
    if (!g_on) return;
    if (HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        g_allow = (AllowDarkModeForWindowFn)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(133));
        g_theme = (SetWindowThemeFn)(void *)GetProcAddress(ux, "SetWindowTheme");
        if (auto pref = (SetPreferredAppModeFn)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(135))) pref(2);   // (force dark)
        if (auto flush = (FlushMenuThemesFn)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(136))) flush();
    }
    if (HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
        g_dwm = (DwmSetWindowAttributeFn)(void *)GetProcAddress(dwm, "DwmSetWindowAttribute");
    g_hook = SetWindowsHookExW(WH_CALLWNDPROCRET, hook, nullptr, GetCurrentThreadId());
}

bool on() { return g_on; }

COLORREF syscolor(int index) {
    if (!g_on) return GetSysColor(index);
    switch (index) {
    case COLOR_WINDOW: case COLOR_MENU: case COLOR_APPWORKSPACE: return kWindow;
    case COLOR_3DFACE: case COLOR_INFOBK: return kFace;
    case COLOR_WINDOWTEXT: case COLOR_BTNTEXT: case COLOR_MENUTEXT: case COLOR_INFOTEXT: return kText;
    case COLOR_GRAYTEXT: return kGray;
    default: return GetSysColor(index);
    }
}

HBRUSH sysbrush(int index) {
    if (!g_on) return GetSysColorBrush(index);
    static std::map<COLORREF, HBRUSH> made;
    COLORREF c = syscolor(index);
    auto it = made.find(c);
    if (it != made.end()) return it->second;
    return made[c] = CreateSolidBrush(c);
}

}   // namespace dark
