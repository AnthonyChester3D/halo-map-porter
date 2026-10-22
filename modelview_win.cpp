// Halo Map Porter - a small model viewer (see modelview_win.hpp)
#include "modelview_win.hpp"

#include <windowsx.h>

#include <algorithm>
#include <vector>

#include "render.hpp"

namespace modelview {

namespace {

struct View {
    const porter::ModelPreview *model = nullptr;
    std::wstring empty;
    render::View view;
    bool dragging = false;
    POINT last{};
    std::vector<uint32_t> px;   // the last drawing
    int w = 0, h = 0;
    bool stale = true;
    HFONT font = nullptr;
};

View *of(HWND h) { return (View *)GetWindowLongPtrW(h, GWLP_USERDATA); }

void paint(HWND h, View *v) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    int w = rc.right, hh = rc.bottom;
    if (w > 0 && hh > 0) {
        if (v->stale || v->w != w || v->h != hh) {
            static const porter::ModelPreview none;
            render::draw(v->model ? *v->model : none, v->view, w, hh, v->px);
            v->w = w;
            v->h = hh;
            v->stale = false;
        }
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -hh;   // top row first
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        // Through a memory bitmap, so the text goes on before it's shown (no flicker).
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, w, hh);
        HGDIOBJ old = SelectObject(mem, bmp);
        SetDIBitsToDevice(mem, 0, 0, w, hh, 0, 0, 0, hh, v->px.data(), &bi, DIB_RGB_COLORS);
        SetBkMode(mem, TRANSPARENT);
        HGDIOBJ oldf = SelectObject(mem, v->font ? v->font : GetStockObject(DEFAULT_GUI_FONT));
        if (!v->model) {
            SetTextColor(mem, RGB(200, 204, 212));
            RECT t = rc, need = rc;
            InflateRect(&t, -12, -12);
            need = t;
            DrawTextW(mem, v->empty.c_str(), -1, &need, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
            OffsetRect(&t, 0, std::max(0, (int)((t.bottom - t.top) - (need.bottom - need.top)) / 2));   // (in the middle)
            DrawTextW(mem, v->empty.c_str(), -1, &t, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        } else {
            SetTextColor(mem, RGB(150, 156, 168));
            RECT t = rc;
            t.bottom -= 4;
            DrawTextW(mem, L"Drag to turn it, wheel to zoom", -1, &t, DT_CENTER | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
        }
        SelectObject(mem, oldf);
        BitBlt(dc, 0, 0, w, hh, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
    }
    EndPaint(h, &ps);
}

void redraw(HWND h, View *v) {
    v->stale = true;
    InvalidateRect(h, nullptr, FALSE);
}

LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    View *v = of(h);
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR) new View());
        break;
    case WM_NCDESTROY:
        delete v;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        break;
    case WM_SETFONT:
        if (v) v->font = (HFONT)wp;
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (v) { paint(h, v); return 0; }
        break;
    case WM_LBUTTONDOWN:
        if (!v) break;
        SetFocus(h);   // (the mouse wheel goes to the window with the focus)
        if (!v->model) return 0;
        v->dragging = true;
        v->last = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        SetCapture(h);
        return 0;
    case WM_MOUSEMOVE:
        if (v && v->dragging) {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            v->view.yaw += (p.x - v->last.x) * 0.012f;
            v->view.pitch = std::clamp(v->view.pitch + (p.y - v->last.y) * 0.012f, -1.45f, 1.45f);
            v->last = p;
            redraw(h, v);
            UpdateWindow(h);
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (v && v->dragging) {
            v->dragging = false;
            if (msg == WM_LBUTTONUP) ReleaseCapture();
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (v && v->model) {
            float steps = GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA;
            float z = v->view.zoom;
            for (int i = 0; i < (int)std::abs(steps); i++) z *= steps > 0 ? 1.15f : 1 / 1.15f;
            v->view.zoom = std::clamp(z, 0.4f, 8.0f);
            redraw(h, v);
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        if (v) {
            bool wire = v->view.wire;
            v->view = render::View();
            v->view.wire = wire;
            redraw(h, v);
        }
        return 0;
    case WM_GETDLGCODE:
        return DLGC_STATIC;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}   // namespace

HWND create(HWND parent, int x, int y, int w, int h, int id) {
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_DBLCLKS;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_SIZEALL);
        wc.lpfnWndProc = proc;
        wc.lpszClassName = L"HMPModelView";
        RegisterClassExW(&wc);
        registered = true;
    }
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"HMPModelView", L"", WS_CHILD | WS_VISIBLE, x, y, w, h, parent,
                           (HMENU)(INT_PTR)id, inst, nullptr);
}

void show(HWND h, const porter::ModelPreview *m, const std::wstring &empty_text, bool keep_view) {
    View *v = of(h);
    if (!v) return;
    if (v->model != m && !keep_view) {
        bool wire = v->view.wire;
        v->view = render::View();
        v->view.wire = wire;
    }
    v->model = m;
    v->empty = empty_text;
    redraw(h, v);
}

void show_triangles(HWND h, bool on) {
    View *v = of(h);
    if (!v || v->view.wire == on) return;
    v->view.wire = on;
    redraw(h, v);
}

}   // namespace modelview
