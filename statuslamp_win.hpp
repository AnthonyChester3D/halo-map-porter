// Halo Map Porter - a bar of colour that says whether the app is working, done or failed, so you can tell at a glance
// when it's your turn without reading the log: amber with a light sweeping along it while working, green when done,
// red when it failed, grey before anything has run. Done or failed also flashes the taskbar button when another app
// is in front. Windows only; header-only (the class registers itself).
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace statuslamp {

enum State { kIdle = 0, kWorking = 1, kDone = 2, kFailed = 3 };
const UINT kSetState = WM_USER + 1;   // wParam: a State

inline LRESULT CALLBACK lamp_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case kSetState:
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)wp);
        if (wp == kWorking) SetTimer(h, 1, 40, nullptr);
        else KillTimer(h, 1);
        SetPropW(h, L"HMPLampT", (HANDLE)0);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_TIMER:
        SetPropW(h, L"HMPLampT", (HANDLE)((INT_PTR)GetPropW(h, L"HMPLampT") + 1));
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        int state = (int)GetWindowLongPtrW(h, GWLP_USERDATA);
        COLORREF c = state == kWorking ? RGB(232, 168, 32) : state == kDone ? RGB(46, 170, 70)
                     : state == kFailed ? RGB(210, 50, 50) : RGB(205, 205, 205);
        HBRUSH b = CreateSolidBrush(c);
        FillRect(dc, &rc, b);
        DeleteObject(b);
        if (state == kWorking && rc.right > 0) {   // (a lighter band sweeping along it: it's alive)
            int w = rc.right, band = w / 5 + 1, t = (int)(INT_PTR)GetPropW(h, L"HMPLampT");
            int x = (t * 8) % (w + band) - band;
            RECT r2{x < 0 ? 0 : x, 0, x + band > w ? w : x + band, rc.bottom};
            HBRUSH l = CreateSolidBrush(RGB(250, 214, 120));
            FillRect(dc, &r2, l);
            DeleteObject(l);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(h, 1);
        RemovePropW(h, L"HMPLampT");
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// A lamp at (x, y), w x h pixels, in `parent`.
inline HWND create(HWND parent, int x, int y, int w, int h, int id) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = lamp_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"HMPStatusLamp";
        RegisterClassW(&wc);
        registered = true;
    }
    return CreateWindowExW(0, L"HMPStatusLamp", L"", WS_CHILD | WS_VISIBLE, x, y, w, h, parent, (HMENU)(INT_PTR)id,
                           GetModuleHandleW(nullptr), nullptr);
}

// Done or failed: the app's taskbar button flashes until you come back to it (when you're in another app).
inline void set(HWND lamp, State s) {
    if (!lamp) return;
    SendMessageW(lamp, kSetState, (WPARAM)s, 0);
    if (s == kDone || s == kFailed) {
        HWND top = GetAncestor(lamp, GA_ROOTOWNER);   // (the window with the taskbar button: the main one)
        HWND fg = GetForegroundWindow();
        if (top && (!fg || GetAncestor(fg, GA_ROOTOWNER) != top)) {
            FLASHWINFO fi{sizeof fi, top, FLASHW_ALL | FLASHW_TIMERNOFG, 0, 0};
            FlashWindowEx(&fi);
        }
    }
}

}   // namespace statuslamp
