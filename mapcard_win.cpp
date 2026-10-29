// Halo Map Porter - Xbox map card editor window (Windows). See mapcard_win.hpp.
#define UNICODE
#define _UNICODE
#include "mapcard_win.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace mapcard {

// ---------------------------------------------------------------- images (WIC)
bool load_image(const fs::path &file, Image &out, int max_side, bool keep_alpha) {
    IWICImagingFactory *fac = nullptr;
    IWICBitmapDecoder *dec = nullptr;
    IWICBitmapFrameDecode *frame = nullptr;
    IWICBitmapScaler *scaler = nullptr;
    IWICFormatConverter *conv = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac))) &&
        SUCCEEDED(fac->CreateDecoderFromFilename(file.wstring().c_str(), nullptr, GENERIC_READ,
                                                 WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame))) {
        UINT w = 0, h = 0;
        frame->GetSize(&w, &h);
        IWICBitmapSource *src = frame;
        if (w && h && (int)std::max(w, h) > max_side) {
            double k = (double)max_side / std::max(w, h);
            UINT nw = std::max(1u, (UINT)std::lround(w * k)), nh = std::max(1u, (UINT)std::lround(h * k));
            if (SUCCEEDED(fac->CreateBitmapScaler(&scaler)) &&
                SUCCEEDED(scaler->Initialize(frame, nw, nh, WICBitmapInterpolationModeFant))) {
                src = scaler; w = nw; h = nh;
            }
        }
        if (w && h && SUCCEEDED(fac->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(src, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                       WICBitmapPaletteTypeCustom))) {
            out.w = (int)w; out.h = (int)h;
            out.rgba.resize((size_t)w * h * 4);
            ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)out.rgba.size(), out.rgba.data()));
            if (ok && !keep_alpha) for (size_t i = 3; i < out.rgba.size(); i += 4) out.rgba[i] = 255;   // pictures are opaque
        }
    }
    for (IUnknown *u : {(IUnknown *)conv, (IUnknown *)scaler, (IUnknown *)frame, (IUnknown *)dec, (IUnknown *)fac})
        if (u) u->Release();
    if (!ok) out = Image();
    return ok;
}

bool card_image(const fs::path &dir, const Card &c, Image &out) {
    if (c.image.empty()) return false;
    return load_image(dir / fs::u8path(c.image), out);
}

// ---------------------------------------------------------------- helpers
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
static std::wstring W32(const std::u32string &s) {
    std::wstring o;
    for (char32_t c : s) {
        if (c < 0x10000) o += (wchar_t)c;
        else { c -= 0x10000; o += (wchar_t)(0xd800 + (c >> 10)); o += (wchar_t)(0xdc00 + (c & 0x3ff)); }
    }
    return o;
}
static std::wstring window_text(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n, 0);
    GetWindowTextW(h, s.data(), n + 1);
    return s;
}
static std::string from_edit(HWND h) {
    std::wstring s = window_text(h), o;
    for (wchar_t c : s) if (c != L'\r') o += c;
    return N(o);
}
static std::wstring to_edit(const std::string &s) {
    std::wstring w = W(s), o;
    for (wchar_t c : w) { if (c == L'\n') o += L"\r\n"; else o += c; }
    return o;
}

// ---------------------------------------------------------------- editor state
enum {
    IDC_FOLDER = 2001, IDC_NAME, IDC_DESC, IDC_CROP, IDC_PREVIEW, IDC_ZOOM, IDC_CHOOSE, IDC_FIT, IDC_FILL, IDC_REMOVE,
    IDC_TV43, IDC_TV169, IDC_BOUNDS, IDC_ST_FOLDER, IDC_CNT_FOLDER, IDC_ST_NAME, IDC_CNT_NAME, IDC_ST_DESC,
    IDC_CNT_DESC, IDC_PICINFO, IDC_OUTFILES, IDC_WHERE, IDC_LBL, IDC_PLAYERS, IDC_GAMETYPES, IDC_GENRES, IDC_GENRE_ADD, IDC_VERSION,
    IDC_ST_BROWSER,
};
constexpr int kGenreMenu = 3000;   // + index into kDefaultGenres
static const COLORREF kOk = RGB(16, 124, 16), kWarn = RGB(170, 92, 0), kBad = RGB(196, 43, 28), kGrey = RGB(96, 96, 96);

struct Editor {
    HWND wnd = nullptr;
    HFONT font = nullptr, bold = nullptr, mono = nullptr;
    int dpi = 96;
    fs::path dir;
    std::wstring map_file;
    std::function<std::wstring(const Card &)> where;
    Card card;
    Image img;
    HDC src_dc = nullptr;
    HBITMAP src_bmp = nullptr, src_old = nullptr;
    std::vector<uint8_t> pic_bgra;   // DXT1-decoded 140x114, BGRA
    HDC tv_dc = nullptr;
    HBITMAP tv_bmp = nullptr, tv_old = nullptr;
    HFONT f_large = nullptr, f_small = nullptr, f_header = nullptr, f_hint = nullptr, f_q = nullptr;
    bool wide = false, bounds = true, done = false, loading = false;
    bool dragging = false;
    POINT drag_pt{};
    double drag_cx = 0, drag_cy = 0;
    COLORREF tone[64] = {};
    WNDPROC desc_old = nullptr;
};
static Editor *E = nullptr;

static int S(int v) { return MulDiv(v, E->dpi, 96); }
static HWND item(int id) { return GetDlgItem(E->wnd, id); }

static void free_source() {
    if (E->src_dc) { SelectObject(E->src_dc, E->src_old); DeleteDC(E->src_dc); E->src_dc = nullptr; }
    if (E->src_bmp) { DeleteObject(E->src_bmp); E->src_bmp = nullptr; }
}
static void make_source() {
    free_source();
    if (E->img.w <= 0) return;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = E->img.w;
    bi.bmiHeader.biHeight = -E->img.h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void *bits = nullptr;
    HDC screen = GetDC(nullptr);
    E->src_bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    E->src_dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!E->src_bmp || !bits) { free_source(); return; }
    uint8_t *d = (uint8_t *)bits;
    const uint8_t *s = E->img.rgba.data();
    for (size_t i = 0, n = (size_t)E->img.w * E->img.h; i < n; i++, d += 4, s += 4) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255; }
    E->src_old = (HBITMAP)SelectObject(E->src_dc, E->src_bmp);
}

static void refresh_picture() {
    E->pic_bgra.clear();
    if (E->img.w <= 0) return;
    std::vector<uint8_t> tex = decode_preview(preview_bin(render_picture(E->img, E->card)));
    E->pic_bgra.resize((size_t)PIC_W * PIC_H * 4);
    for (int y = 0; y < PIC_H; y++)
        for (int x = 0; x < PIC_W; x++) {
            const uint8_t *s = &tex[((size_t)y * TEX_W + x) * 4];
            uint8_t *d = &E->pic_bgra[((size_t)y * PIC_W + x) * 4];
            d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255;
        }
}

// zoom slider: logarithmic between "fit whole image" and 4x
static double slider_to_zoom(int pos, double zmin) { return zmin * std::pow(4.0 / zmin, pos / 1000.0); }
static int zoom_to_slider(double z, double zmin) {
    if (zmin >= 4) return 0;
    return std::clamp((int)std::lround(std::log(z / zmin) / std::log(4.0 / zmin) * 1000.0), 0, 1000);
}
static void sync_slider() {
    HWND s = item(IDC_ZOOM);
    EnableWindow(s, E->img.w > 0);
    if (E->img.w <= 0) { SendMessageW(s, TBM_SETPOS, TRUE, 0); return; }
    Crop g = crop_geometry(E->img.w, E->img.h, E->card.zoom, E->card.cx, E->card.cy);
    SendMessageW(s, TBM_SETPOS, TRUE, zoom_to_slider(g.z, g.zmin));
}
// Clamp the stored crop to what's reachable, so the card file matches what's shown.
static void normalize_crop() {
    if (E->img.w <= 0) return;
    Crop g = crop_geometry(E->img.w, E->img.h, E->card.zoom, E->card.cx, E->card.cy);
    E->card.zoom = g.z;
    E->card.cx = g.cx / E->img.w;
    E->card.cy = g.cy / E->img.h;
}

static void set_status(int id, const std::wstring &text, COLORREF c) {
    E->tone[id - 2000 < 64 ? id - 2000 : 0] = c;
    SetWindowTextW(item(id), text.c_str());
    InvalidateRect(item(id), nullptr, TRUE);
}

static void update_texts() {
    const Card &c = E->card;
    std::string fp = folder_problem(c.folder);
    set_status(IDC_ST_FOLDER, fp.empty() ? L"Good folder name" : W(fp), fp.empty() ? kOk : kBad);
    set_status(IDC_CNT_FOLDER, std::to_wstring(utf8_to_u32(c.folder).size()) + L" / 30", kGrey);

    std::u32string miss;
    std::string shown = c.name.find_first_not_of(' ') == std::string::npos ? c.folder : c.name;
    auto nl = wrap(utf8_to_u32(shown), LARGE, NAME_TEXT_W, &miss);
    std::wstring ns;
    COLORREF nt = kOk;
    if (c.name.find_first_not_of(' ') == std::string::npos) { ns = L"Empty: the folder name is shown instead"; nt = kWarn; }
    else if (nl.size() == 1) ns = L"Fits on one line";
    else if (nl.size() == 2) ns = L"Two lines in the map list (one in the lobby)";
    else { ns = L"Too long: " + std::to_wstring(nl.size()) + L" lines, Halo shows 2"; nt = kBad; }
    if (!miss.empty()) { ns = L"Halo's font has no " + W32(miss); nt = kBad; }
    set_status(IDC_ST_NAME, ns, nt);
    set_status(IDC_CNT_NAME, std::to_wstring(utf8_to_u32(c.name).size()) + L" / 63", kGrey);

    miss.clear();
    std::string d = c.desc;
    while (!d.empty() && (d.back() == '\n' || d.back() == ' ')) d.pop_back();
    std::wstring ds;
    COLORREF dt = kOk;
    if (d.find_first_not_of(" \n") == std::string::npos) { ds = L"Empty"; dt = kWarn; }
    else {
        auto dl = wrap(utf8_to_u32(d), SMALL, DESC_TEXT_W, &miss);
        if ((int)dl.size() <= DESC_LINES) ds = L"Uses " + std::to_wstring(dl.size()) + L" of 6 lines";
        else {
            int over = (int)dl.size() - DESC_LINES;
            ds = std::to_wstring(over) + (over == 1 ? L" line is" : L" lines are") + L" cut off (6 fit)";
            dt = kBad;
        }
        if (!miss.empty()) { ds = L"Halo's font has no " + W32(miss); dt = kBad; }
    }
    set_status(IDC_ST_DESC, ds, dt);
    set_status(IDC_CNT_DESC, std::to_wstring(utf8_to_u32(c.desc).size()) + L" / 254", kGrey);

    std::wstring folder = c.folder.find_first_not_of(' ') == std::string::npos ? L"[folder]" : W(c.folder);
    std::wstring files = L"maps\\" + folder + L"\\\r\n    " + E->map_file + L"\r\n    info.txt\r\n    " +
                         (E->img.w > 0 ? L"preview.bin" : L"(no picture yet: the Xbox shows its unknown-map picture)");
    SetWindowTextW(item(IDC_OUTFILES), files.c_str());
    if (E->where) SetWindowTextW(item(IDC_WHERE), E->where(c).c_str());
    std::wstring pi = L"No picture yet";
    if (E->img.w > 0) pi = W(c.image) + L"  \x00b7  " + std::to_wstring(E->img.w) + L" \x00d7 " + std::to_wstring(E->img.h);
    SetWindowTextW(item(IDC_PICINFO), pi.c_str());
    for (int id : {IDC_FIT, IDC_FILL, IDC_REMOVE}) EnableWindow(item(id), E->img.w > 0);

    int lo, hi;
    std::wstring bs = L"The Xbox's map list sorts by these. They go at the end of info.txt and aren't shown. A build fills "
                      L"in the gametypes and a player guess, and suggests genres; what you type here stays.";
    COLORREF bt = kGrey;
    if (c.players.find_first_not_of(' ') != std::string::npos && !parse_players(c.players, lo, hi)) {
        bs = L"Players: write a number or a range from 1 to 16, like 4-16.";
        bt = kBad;
    } else if (c.players.find_first_not_of(' ') == std::string::npos && desc_players(c.desc, lo, hi)) {
        bs = L"Players: the description's \"" + std::to_wstring(lo) + (lo == hi ? L"" : L"-" + std::to_wstring(hi)) +
             L" players\" is used. " + bs;
    }
    set_status(IDC_ST_BROWSER, bs, bt);
    InvalidateRect(item(IDC_PREVIEW), nullptr, FALSE);
}

static void picture_changed() {
    normalize_crop();
    refresh_picture();
    sync_slider();
    InvalidateRect(item(IDC_CROP), nullptr, FALSE);
    InvalidateRect(item(IDC_PREVIEW), nullptr, FALSE);
}

static void set_image_from(const fs::path &file) {
    Image img;
    if (!load_image(file, img)) {
        MessageBoxW(E->wnd, L"Couldn't read that image. PNG, JPEG, BMP, GIF and TIFF work.", L"Map card", MB_ICONWARNING);
        return;
    }
    std::error_code ec;
    fs::create_directories(E->dir, ec);
    std::wstring ext = file.extension().wstring();
    for (auto &ch : ext) ch = towlower(ch);
    fs::path dst = E->dir / (L"source" + ext);
    // Keep a copy in the card, so moving or deleting the original doesn't lose the picture.
    fs::path tmp = E->dir / (L"incoming" + ext);
    fs::copy_file(file, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        MessageBoxW(E->wnd, (L"Couldn't copy the image into the card: " + W(ec.message())).c_str(), L"Map card", MB_ICONWARNING);
        return;
    }
    std::vector<fs::path> old;
    for (auto &e : fs::directory_iterator(E->dir, ec))
        if (e.path().stem() == L"source") old.push_back(e.path());
    for (auto &p : old) fs::remove(p, ec);
    fs::rename(tmp, dst, ec);
    if (ec) {
        MessageBoxW(E->wnd, (L"Couldn't copy the image into the card: " + W(ec.message())).c_str(), L"Map card", MB_ICONWARNING);
        return;
    }
    E->img = std::move(img);
    E->card.image = dst.filename().u8string();
    E->card.zoom = 1;
    E->card.cx = E->card.cy = 0.5;
    make_source();
    save_card(E->dir, E->card);
    picture_changed();
    update_texts();
}

// ---------------------------------------------------------------- crop control
static void draw_crop(HDC dc, RECT rc) {
    int W0 = rc.right, H0 = rc.bottom;
    HBRUSH bg = CreateSolidBrush(RGB(11, 13, 16));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    if (E->img.w > 0 && E->src_dc) {
        Crop g = crop_geometry(E->img.w, E->img.h, E->card.zoom, E->card.cx, E->card.cy);
        double k = W0 / g.cw, left = g.cx - g.cw / 2, top = g.cy - g.ch / 2;
        double sx0 = std::max(0.0, left), sx1 = std::min((double)E->img.w, left + g.cw);
        double sy0 = std::max(0.0, top), sy1 = std::min((double)E->img.h, top + g.ch);
        int dx0 = (int)std::lround((sx0 - left) * k), dx1 = (int)std::lround((sx1 - left) * k);
        int dy0 = (int)std::lround((sy0 - top) * k), dy1 = (int)std::lround((sy1 - top) * k);
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchBlt(dc, dx0, dy0, dx1 - dx0, dy1 - dy0, E->src_dc, (int)sx0, (int)sy0, (int)std::lround(sx1 - sx0),
                   (int)std::lround(sy1 - sy0), SRCCOPY);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(150, 150, 150));
        HPEN op = (HPEN)SelectObject(dc, pen);
        for (int i = 1; i < 3; i++) {
            MoveToEx(dc, W0 * i / 3, 0, nullptr); LineTo(dc, W0 * i / 3, H0);
            MoveToEx(dc, 0, H0 * i / 3, nullptr); LineTo(dc, W0, H0 * i / 3);
        }
        SelectObject(dc, op);
        DeleteObject(pen);
    } else {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(200, 205, 212));
        HFONT of = (HFONT)SelectObject(dc, E->bold);
        RECT t = rc;
        t.top = H0 / 2 - S(20);
        DrawTextW(dc, L"Drop an image here", -1, &t, DT_CENTER | DT_SINGLELINE);
        SelectObject(dc, E->font);
        SetTextColor(dc, RGB(150, 158, 168));
        t.top = H0 / 2 + S(2);
        DrawTextW(dc, L"or use Choose image. A screenshot of the map works well.", -1, &t, DT_CENTER | DT_SINGLELINE);
        SelectObject(dc, of);
    }
    if (GetFocus() == item(IDC_CROP)) {
        HBRUSH fb = CreateSolidBrush(RGB(61, 155, 255));
        RECT f = rc;
        FrameRect(dc, &f, fb);
        InflateRect(&f, -1, -1);
        FrameRect(dc, &f, fb);
        DeleteObject(fb);
    }
}

static void pan_by(double dx_img, double dy_img) {
    Crop g = crop_geometry(E->img.w, E->img.h, E->card.zoom, E->card.cx, E->card.cy);
    E->card.cx = (g.cx + dx_img) / E->img.w;
    E->card.cy = (g.cy + dy_img) / E->img.h;
    picture_changed();
}
static void zoom_by(double f) {
    Crop g = crop_geometry(E->img.w, E->img.h, E->card.zoom, E->card.cx, E->card.cy);
    E->card.zoom = std::clamp(g.z * f, g.zmin, 4.0);
    picture_changed();
}

static LRESULT CALLBACK crop_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HBITMAP ob = (HBITMAP)SelectObject(mem, bmp);
        draw_crop(mem, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN:
        SetFocus(h);
        if (E->img.w > 0) {
            Crop g = crop_geometry(E->img.w, E->img.h, E->card.zoom, E->card.cx, E->card.cy);
            E->dragging = true;
            E->drag_pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
            E->drag_cx = g.cx; E->drag_cy = g.cy;
            SetCapture(h);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (E->dragging) {
            RECT rc;
            GetClientRect(h, &rc);
            Crop g = crop_geometry(E->img.w, E->img.h, E->card.zoom, E->card.cx, E->card.cy);
            double k = g.cw / std::max(1L, rc.right);
            int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
            E->card.cx = (E->drag_cx - (x - E->drag_pt.x) * k) / E->img.w;
            E->card.cy = (E->drag_cy - (y - E->drag_pt.y) * k) / E->img.h;
            picture_changed();
        }
        return 0;
    case WM_LBUTTONUP: case WM_CAPTURECHANGED:
        if (E->dragging) { E->dragging = false; if (msg == WM_LBUTTONUP) ReleaseCapture(); }
        return 0;
    case WM_MOUSEWHEEL:
        if (E->img.w > 0) zoom_by(std::pow(1.1, GET_WHEEL_DELTA_WPARAM(wp) / 120.0));
        return 0;
    case WM_KEYDOWN:
        if (E->img.w > 0) {
            Crop g = crop_geometry(E->img.w, E->img.h, E->card.zoom, E->card.cx, E->card.cy);
            double step = g.cw / ((GetKeyState(VK_SHIFT) & 0x8000) ? 4 : 20);
            switch (wp) {
            case VK_LEFT: pan_by(-step, 0); return 0;
            case VK_RIGHT: pan_by(step, 0); return 0;
            case VK_UP: pan_by(0, -step); return 0;
            case VK_DOWN: pan_by(0, step); return 0;
            case VK_ADD: case VK_OEM_PLUS: zoom_by(1.1); return 0;
            case VK_SUBTRACT: case VK_OEM_MINUS: zoom_by(1 / 1.1); return 0;
            }
        }
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------------------------------------------------------- Xbox preview
// Halo's map select screen in its 640x480 menu space (positions from the stock ui widget tags).
struct Item { int box[4], pic[2], name[4], desc[4]; };
static const Item kItems[3] = {
    {{72, 75, 165, 330}, {85, 87}, {83, 204, 144, 43}, {83, 258, 144, 136}},
    {{239, 75, 163, 326}, {252, 87}, {250, 204, 145, 43}, {250, 258, 145, 136}},
    {{406, 75, 162, 326}, {419, 87}, {417, 204, 146, 43}, {417, 258, 146, 136}},
};
static const COLORREF kHaloBlue = RGB(40, 150, 255);
static const XFORM kBase = {2, 0, 0, 2, 0, 0};   // the preview bitmap is 1280x960

static void fill(HDC dc, int x, int y, int w, int h, COLORREF c) {
    RECT r{x, y, x + w, y + h};
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}
static void frame(HDC dc, int x, int y, int w, int h, COLORREF c, int width = 1, int style = PS_SOLID) {
    HPEN p = CreatePen(style, width, c);
    HPEN op = (HPEN)SelectObject(dc, p);
    HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, x, y, x + w, y + h);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(p);
}
static void draw_unknown(HDC dc, int x, int y) {
    fill(dc, x, y, PIC_W, PIC_H, RGB(10, 26, 46));
    HPEN p = CreatePen(PS_SOLID, 1, RGB(18, 52, 88));
    HPEN op = (HPEN)SelectObject(dc, p);
    for (int i = 10; i < PIC_W; i += 10) { MoveToEx(dc, x + i, y, nullptr); LineTo(dc, x + i, y + PIC_H); }
    for (int i = 10; i < PIC_H; i += 10) { MoveToEx(dc, x, y + i, nullptr); LineTo(dc, x + PIC_W, y + i); }
    SelectObject(dc, op);
    DeleteObject(p);
    frame(dc, x, y, PIC_W, PIC_H, RGB(30, 110, 190));
    HFONT of = (HFONT)SelectObject(dc, E->f_q);
    SetTextColor(dc, kHaloBlue);
    SetTextAlign(dc, TA_CENTER | TA_BASELINE);
    TextOutW(dc, x + PIC_W / 2, y + PIC_H / 2 + 20, L"?", 1);
    SetTextAlign(dc, TA_LEFT | TA_BASELINE);
    SelectObject(dc, of);
}
// One wrapped text box, scaling each line to Halo's exact width.
static void draw_lines(HDC dc, const std::vector<std::u32string> &lines, const int *box, Font f, int max_lines, bool overflow,
                       COLORREF color) {
    const int asc = f == LARGE ? 17 : 14, lh = f == LARGE ? 21 : 20;
    HFONT of = (HFONT)SelectObject(dc, f == LARGE ? E->f_large : E->f_small);
    SetTextAlign(dc, TA_LEFT | TA_BASELINE);
    for (size_t i = 0; i < lines.size(); i++) {
        bool over = (int)i >= max_lines;
        if (over && !overflow) break;
        std::wstring s = W32(lines[i]);
        if (s.empty()) continue;
        SIZE sz{};
        GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
        double sx = sz.cx > 0 ? (double)text_width(f, lines[i]) / sz.cx : 1.0;
        float x = (float)(box[0] + (f == LARGE ? NAME_LEAD : DESC_LEAD)), y = (float)(box[1] + asc + (int)i * lh);
        for (int pass = over ? 1 : 0; pass < 2; pass++) {
            XFORM xf = {(float)(2 * sx), 0, 0, 2, 2 * (x + (pass ? 0 : 1)), 2 * (y + (pass ? 0 : 1))};
            SetWorldTransform(dc, &xf);
            SetTextColor(dc, pass == 0 ? RGB(0, 0, 0) : over ? RGB(255, 110, 90) : color);
            TextOutW(dc, 0, 0, s.c_str(), (int)s.size());
        }
        SetWorldTransform(dc, &kBase);
    }
    SelectObject(dc, of);
}

static void render_tv() {
    HDC dc = E->tv_dc;
    SetGraphicsMode(dc, GM_ADVANCED);
    SetWorldTransform(dc, &kBase);
    SetBkMode(dc, TRANSPARENT);
    fill(dc, 0, 0, 640, 480, RGB(5, 10, 18));
    fill(dc, 0, 70, 640, 1, RGB(10, 22, 38));
    HFONT of = (HFONT)SelectObject(dc, E->f_header);
    SetTextAlign(dc, TA_LEFT | TA_BASELINE);
    SetTextColor(dc, kHaloBlue);
    TextOutW(dc, 40, 54, L"SELECT MAP", 10);
    SelectObject(dc, of);

    const Card &c = E->card;
    std::string shown = c.name.find_first_not_of(' ') == std::string::npos ? c.folder : c.name;
    std::string d = c.desc;
    while (!d.empty() && (d.back() == '\n' || d.back() == ' ')) d.pop_back();
    const char *neighbours[3] = {"Longest", nullptr, "Next folder map"};
    for (int i = 0; i < 3; i++) {
        const Item &it = kItems[i];
        bool focus = i == 1;
        fill(dc, it.box[0], it.box[1], it.box[2], it.box[3], focus ? RGB(14, 38, 70) : RGB(9, 20, 36));
        frame(dc, it.box[0] + 1, it.box[1] + 1, it.box[2] - 2, it.box[3] - 2, focus ? RGB(90, 180, 255) : RGB(22, 62, 108),
              focus ? 2 : 1);
        if (focus) {
            if (!E->pic_bgra.empty()) {
                BITMAPINFO bi{};
                bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
                bi.bmiHeader.biWidth = PIC_W;
                bi.bmiHeader.biHeight = -PIC_H;
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                SetStretchBltMode(dc, COLORONCOLOR);
                StretchDIBits(dc, it.pic[0], it.pic[1], PIC_W, PIC_H, 0, 0, PIC_W, PIC_H, E->pic_bgra.data(), &bi,
                              DIB_RGB_COLORS, SRCCOPY);
            } else {
                draw_unknown(dc, it.pic[0], it.pic[1]);
            }
            draw_lines(dc, wrap(utf8_to_u32(shown), LARGE, it.name[2] - NAME_LEAD), it.name, LARGE, NAME_LINES, E->bounds, kHaloBlue);
            if (d.find_first_not_of(" \n") != std::string::npos)
                draw_lines(dc, wrap(utf8_to_u32(d), SMALL, it.desc[2] - DESC_LEAD), it.desc, SMALL, DESC_LINES, E->bounds, kHaloBlue);
            if (E->bounds) {
                frame(dc, it.name[0], it.name[1], it.name[2] + 1, it.name[3] + 1, RGB(255, 200, 80), 1, PS_DOT);
                frame(dc, it.desc[0], it.desc[1], it.desc[2] + 1, it.desc[3] + 1, RGB(255, 200, 80), 1, PS_DOT);
            }
        } else {
            draw_unknown(dc, it.pic[0], it.pic[1]);
            draw_lines(dc, wrap(utf8_to_u32(neighbours[i]), LARGE, it.name[2] - NAME_LEAD), it.name, LARGE, NAME_LINES, false,
                       RGB(30, 112, 192));
        }
    }
    // button hints
    of = (HFONT)SelectObject(dc, E->f_hint);
    const struct { const wchar_t *k, *label; int x; COLORREF col; } hints[2] = {
        {L"A", L"Select", 470, RGB(90, 200, 90)}, {L"B", L"Back", 548, RGB(230, 80, 70)}};
    for (auto &hn : hints) {
        HBRUSH b = CreateSolidBrush(hn.col);
        HBRUSH ob = (HBRUSH)SelectObject(dc, b);
        HPEN op = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, hn.x - 9, 431, hn.x + 10, 450);
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(b);
        SetTextColor(dc, RGB(5, 8, 13));
        SetTextAlign(dc, TA_CENTER | TA_BASELINE);
        TextOutW(dc, hn.x, 446, hn.k, 1);
        SetTextColor(dc, kHaloBlue);
        SetTextAlign(dc, TA_LEFT | TA_BASELINE);
        TextOutW(dc, hn.x + 15, 446, hn.label, (int)wcslen(hn.label));
    }
    SelectObject(dc, of);
    ModifyWorldTransform(dc, nullptr, MWT_IDENTITY);
}

static LRESULT CALLBACK preview_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        render_tv();
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HBITMAP ob = (HBITMAP)SelectObject(mem, bmp);
        FillRect(mem, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        int w = rc.right, hh = E->wide ? rc.right * 9 / 16 : rc.bottom;
        int y = E->wide ? (rc.bottom - hh) / 2 : 0;
        SetStretchBltMode(mem, HALFTONE);
        SetBrushOrgEx(mem, 0, 0, nullptr);
        StretchBlt(mem, 0, y, w, hh, E->tv_dc, 0, 0, 1280, 960, SRCCOPY);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------------------------------------------------------- the window
static LRESULT CALLBACK desc_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_GETDLGCODE) {
        LRESULT r = CallWindowProcW(E->desc_old, h, msg, wp, lp);
        MSG *m = (MSG *)lp;
        if (m && m->message == WM_KEYDOWN && m->wParam == VK_TAB) r &= ~(DLGC_WANTALLKEYS | DLGC_WANTTAB);
        return r;
    }
    return CallWindowProcW(E->desc_old, h, msg, wp, lp);
}

static HWND add(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0,
                HFONT font = nullptr) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), E->wnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(font ? font : E->font), TRUE);
    return c;
}

static void create_controls() {
    const int L = 20, LW = 340, R = 384, RW = 596;
    add(L"STATIC", L"Folder name", 0, L, 14, LW, 20, IDC_LBL, 0, E->bold);
    HWND folder = add(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, L, 36, LW, 24, IDC_FOLDER, WS_EX_CLIENTEDGE);
    SendMessageW(folder, EM_LIMITTEXT, FOLDER_MAX, 0);
    add(L"STATIC", L"", SS_ENDELLIPSIS, L, 63, 266, 18, IDC_ST_FOLDER);
    add(L"STATIC", L"", SS_RIGHT, L + 270, 63, 70, 18, IDC_CNT_FOLDER);

    add(L"STATIC", L"Map name", 0, L, 90, LW, 20, IDC_LBL, 0, E->bold);
    HWND name = add(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, L, 112, LW, 24, IDC_NAME, WS_EX_CLIENTEDGE);
    SendMessageW(name, EM_LIMITTEXT, NAME_MAX, 0);
    add(L"STATIC", L"", SS_ENDELLIPSIS, L, 139, 266, 18, IDC_ST_NAME);
    add(L"STATIC", L"", SS_RIGHT, L + 270, 139, 70, 18, IDC_CNT_NAME);

    add(L"STATIC", L"Description", 0, L, 166, LW, 20, IDC_LBL, 0, E->bold);
    HWND desc = add(L"EDIT", L"", WS_TABSTOP | ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | WS_VSCROLL, L, 188, LW, 92,
                    IDC_DESC, WS_EX_CLIENTEDGE);
    SendMessageW(desc, EM_LIMITTEXT, DESC_MAX, 0);
    E->desc_old = (WNDPROC)SetWindowLongPtrW(desc, GWLP_WNDPROC, (LONG_PTR)desc_proc);
    add(L"STATIC", L"", SS_ENDELLIPSIS, L, 283, 266, 18, IDC_ST_DESC);
    add(L"STATIC", L"", SS_RIGHT, L + 270, 283, 70, 18, IDC_CNT_DESC);

    add(L"STATIC", L"Picture", 0, L, 312, 80, 20, IDC_LBL, 0, E->bold);
    add(L"STATIC", L"", SS_RIGHT | SS_ENDELLIPSIS, L + 84, 313, LW - 84, 18, IDC_PICINFO);
    add(L"BUTTON", L"Choose image...", WS_TABSTOP | BS_PUSHBUTTON, L, 336, 104, 28, IDC_CHOOSE);
    add(L"BUTTON", L"Fit whole", WS_TABSTOP | BS_PUSHBUTTON, L + 110, 336, 76, 28, IDC_FIT);
    add(L"BUTTON", L"Fill frame", WS_TABSTOP | BS_PUSHBUTTON, L + 192, 336, 76, 28, IDC_FILL);
    add(L"BUTTON", L"Remove", WS_TABSTOP | BS_PUSHBUTTON, L + 274, 336, 66, 28, IDC_REMOVE);
    add(L"HMPCardCrop", L"", WS_TABSTOP, L, 370, LW, 277, IDC_CROP);
    add(L"STATIC", L"Zoom", 0, L, 657, 44, 20, IDC_LBL);
    HWND zoom = add(TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS, L + 46, 653, LW - 46, 28, IDC_ZOOM);
    SendMessageW(zoom, TBM_SETRANGE, TRUE, MAKELPARAM(0, 1000));
    SendMessageW(zoom, TBM_SETPAGESIZE, 0, 50);

    add(L"STATIC", L"On the Xbox", 0, R, 14, 150, 20, IDC_LBL, 0, E->bold);
    add(L"BUTTON", L"4:3 TV", WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON, R + 316, 12, 66, 24, IDC_TV43);
    add(L"BUTTON", L"16:9 TV", BS_AUTORADIOBUTTON, R + 386, 12, 74, 24, IDC_TV169);
    add(L"BUTTON", L"Show text boxes", WS_TABSTOP | WS_GROUP | BS_AUTOCHECKBOX, R + 466, 12, 130, 24, IDC_BOUNDS);
    add(L"HMPCardPreview", L"", 0, R, 40, RW, 447, IDC_PREVIEW);
    add(L"STATIC",
        L"Halo's menus don't wrap text, so these line breaks (worked out with Halo's own letter widths) are written "
        L"into info.txt. The lettering is a stand-in for Halo's font. A 16:9 TV stretches the menus sideways.",
        0, R, 494, RW, 36, IDC_LBL);
    add(L"STATIC", L"Written to the Xbox", 0, R, 538, RW, 20, IDC_LBL, 0, E->bold);
    add(L"STATIC", L"", 0, R, 560, RW, 80, IDC_OUTFILES, 0, E->mono);
    add(L"STATIC", L"", 0, R, 646, RW, 40, IDC_WHERE);

    // the Halo Universal XBE's map browser (build 20)
    const int B = 694;
    add(L"STATIC", L"Map list sorting (Halo Universal XBE build 20)", 0, L, B, 600, 20, IDC_LBL, 0, E->bold);
    add(L"STATIC", L"Players", 0, L, B + 27, 52, 20, IDC_LBL);
    HWND pl = add(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, L + 54, B + 24, 64, 24, IDC_PLAYERS, WS_EX_CLIENTEDGE);
    SendMessageW(pl, EM_LIMITTEXT, 8, 0);
    add(L"STATIC", L"Gametypes", 0, L + 132, B + 27, 72, 20, IDC_LBL);
    HWND gt = add(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, L + 206, B + 24, 260, 24, IDC_GAMETYPES, WS_EX_CLIENTEDGE);
    SendMessageW(gt, EM_LIMITTEXT, 100, 0);
    add(L"STATIC", L"Genres", 0, L + 480, B + 27, 50, 20, IDC_LBL);
    HWND gn = add(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, L + 532, B + 24, 234, 24, IDC_GENRES, WS_EX_CLIENTEDGE);
    SendMessageW(gn, EM_LIMITTEXT, 120, 0);
    add(L"STATIC", L"Version", 0, L + 774, B + 27, 48, 20, IDC_LBL);
    HWND vn = add(L"EDIT", L"", WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, L + 822, B + 24, 36, 24, IDC_VERSION, WS_EX_CLIENTEDGE);
    SendMessageW(vn, EM_LIMITTEXT, 4, 0);
    add(L"BUTTON", L"Add genre...", WS_TABSTOP | BS_PUSHBUTTON, L + 864, B + 22, 96, 28, IDC_GENRE_ADD);
    add(L"STATIC", L"", 0, L, B + 56, 830, 36, IDC_ST_BROWSER);
    add(L"BUTTON", L"Done", WS_TABSTOP | BS_DEFPUSHBUTTON, L + 860, B + 58, 100, 30, IDOK);
    SetWindowTextW(pl, W(E->card.players).c_str());
    SetWindowTextW(gt, W(E->card.gametypes).c_str());
    SetWindowTextW(gn, W(E->card.genres).c_str());
    if (E->card.version > 0) SetWindowTextW(vn, std::to_wstring(E->card.version).c_str());

    SetWindowTextW(folder, W(E->card.folder).c_str());
    SetWindowTextW(name, W(E->card.name).c_str());
    SetWindowTextW(desc, to_edit(E->card.desc).c_str());
    CheckRadioButton(E->wnd, IDC_TV43, IDC_TV169, E->wide ? IDC_TV169 : IDC_TV43);
    CheckDlgButton(E->wnd, IDC_BOUNDS, E->bounds ? BST_CHECKED : BST_UNCHECKED);
}

static void close_editor() {
    normalize_crop();
    save_card(E->dir, E->card);
    E->done = true;
}

static LRESULT CALLBACK editor_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        int id = GetDlgCtrlID((HWND)lp);
        SetBkMode(dc, TRANSPARENT);
        if (id >= 2000 && id < 2064 && E->tone[id - 2000]) SetTextColor(dc, E->tone[id - 2000]);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (E->loading) return 0;
        switch (id) {
        case IDC_FOLDER: case IDC_NAME: case IDC_DESC:
            if (code == EN_CHANGE) {
                std::string v = from_edit((HWND)lp);
                if (id == IDC_FOLDER) E->card.folder = v;
                else if (id == IDC_NAME) E->card.name = v;
                else E->card.desc = v;
                update_texts();
            }
            return 0;
        case IDC_VERSION:
            if (code == EN_CHANGE && GetFocus() == (HWND)lp) {   // (yours: the next export writes it as it is)
                E->card.version = std::max(0, atoi(from_edit((HWND)lp).c_str()));
                E->card.version_set = E->card.version > 0;
                update_texts();
            }
            return 0;
        case IDC_PLAYERS: case IDC_GAMETYPES: case IDC_GENRES:
            if (code == EN_CHANGE) {
                std::string v = from_edit((HWND)lp);
                const char *f = id == IDC_PLAYERS ? "players" : id == IDC_GAMETYPES ? "gametypes" : "genres";
                (id == IDC_PLAYERS ? E->card.players : id == IDC_GAMETYPES ? E->card.gametypes : E->card.genres) = v;
                mark_edited(E->card, f);   // yours now: a build leaves it alone
                update_texts();
            }
            return 0;
        case IDC_GENRE_ADD: {   // the Xbox's starting genres: tick to add, untick to take out
            auto has = [](const std::string &list, const std::string &g, size_t *at, size_t *len) {
                size_t p = 0;
                while (p <= list.size()) {
                    size_t e = list.find(',', p);
                    if (e == std::string::npos) e = list.size();
                    size_t a = list.find_first_not_of(' ', p), b = list.find_last_not_of(' ', e ? e - 1 : 0);
                    if (a != std::string::npos && a < e && b >= a && _stricmp(list.substr(a, b - a + 1).c_str(), g.c_str()) == 0) {
                        if (at) { *at = p; *len = e - p + (e < list.size()); }
                        return true;
                    }
                    p = e + 1;
                }
                return false;
            };
            HMENU m = CreatePopupMenu();
            for (int k = 0; k < 7; k++)
                AppendMenuW(m, MF_STRING | (has(E->card.genres, kDefaultGenres[k], nullptr, nullptr) ? MF_CHECKED : 0),
                            kGenreMenu + k, W(kDefaultGenres[k]).c_str());
            RECT r;
            GetWindowRect((HWND)lp, &r);
            int pick = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN, r.right, r.bottom, 0, h, nullptr);
            DestroyMenu(m);
            if (pick >= kGenreMenu && pick < kGenreMenu + 7) {
                std::string g = kDefaultGenres[pick - kGenreMenu], v = E->card.genres;
                size_t at, len;
                if (has(v, g, &at, &len)) v.erase(at, len);
                else v += (v.find_first_not_of(' ') == std::string::npos ? "" : ", ") + g;
                size_t a = v.find_first_not_of(" ,"), b = v.find_last_not_of(" ,");
                v = a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
                SetWindowTextW(item(IDC_GENRES), W(v).c_str());   // (EN_CHANGE stores it)
            }
            return 0;
        }
        case IDC_CHOOSE: {
            wchar_t buf[MAX_PATH] = {};
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = h;
            ofn.lpstrFilter = L"Images (*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff\0All files\0*.*\0";
            ofn.lpstrFile = buf;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&ofn)) set_image_from(buf);
            return 0;
        }
        case IDC_FIT:
            if (E->img.w > 0) {
                Crop g = crop_geometry(E->img.w, E->img.h, 1, 0.5, 0.5);
                E->card.zoom = g.zmin; E->card.cx = E->card.cy = 0.5;
                picture_changed();
            }
            return 0;
        case IDC_FILL:
            if (E->img.w > 0) { E->card.zoom = 1; E->card.cx = E->card.cy = 0.5; picture_changed(); }
            return 0;
        case IDC_REMOVE: {
            std::error_code ec;
            if (!E->card.image.empty()) fs::remove(E->dir / fs::u8path(E->card.image), ec);
            E->card.image.clear();
            E->img = Image();
            free_source();
            picture_changed();
            update_texts();
            save_card(E->dir, E->card);
            return 0;
        }
        case IDC_TV43: case IDC_TV169:
            E->wide = id == IDC_TV169;
            InvalidateRect(item(IDC_PREVIEW), nullptr, FALSE);
            return 0;
        case IDC_BOUNDS:
            E->bounds = IsDlgButtonChecked(h, IDC_BOUNDS) == BST_CHECKED;
            InvalidateRect(item(IDC_PREVIEW), nullptr, FALSE);
            return 0;
        case IDOK: case IDCANCEL:
            close_editor();
            return 0;
        }
        return 0;
    }
    case WM_HSCROLL:
        if ((HWND)lp == item(IDC_ZOOM) && E->img.w > 0) {
            Crop g = crop_geometry(E->img.w, E->img.h, E->card.zoom, E->card.cx, E->card.cy);
            int pos = (int)SendMessageW(item(IDC_ZOOM), TBM_GETPOS, 0, 0);
            E->card.zoom = slider_to_zoom(pos, g.zmin);
            E->card.cx = g.cx / E->img.w;
            E->card.cy = g.cy / E->img.h;
            normalize_crop();
            refresh_picture();
            InvalidateRect(item(IDC_CROP), nullptr, FALSE);
            InvalidateRect(item(IDC_PREVIEW), nullptr, FALSE);
        }
        return 0;
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        wchar_t buf[MAX_PATH];
        if (DragQueryFileW(drop, 0, buf, MAX_PATH)) set_image_from(buf);
        DragFinish(drop);
        return 0;
    }
    case WM_CLOSE:
        close_editor();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void edit_card(HWND owner, const fs::path &dir, const std::string &default_name, const std::wstring &map_file,
               const std::wstring &title, std::function<std::wstring(const Card &)> where) {
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!registered) {
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&icc);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpfnWndProc = editor_proc;
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wc.lpszClassName = L"HMPMapCard";
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        RegisterClassExW(&wc);
        wc.hIcon = nullptr;
        wc.hbrBackground = nullptr;
        wc.lpfnWndProc = crop_proc;
        wc.hCursor = LoadCursor(nullptr, IDC_SIZEALL);
        wc.lpszClassName = L"HMPCardCrop";
        RegisterClassExW(&wc);
        wc.lpfnWndProc = preview_proc;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"HMPCardPreview";
        RegisterClassExW(&wc);
        registered = true;
    }
    Editor ed;
    E = &ed;
    {
        HDC dc = GetDC(nullptr);
        ed.dpi = GetDeviceCaps(dc, LOGPIXELSY);
        // preview bitmap and its fonts (sizes in Halo's menu pixels; the bitmap is drawn at 2x)
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = 1280;
        bi.bmiHeader.biHeight = -960;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void *bits = nullptr;
        ed.tv_bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        ed.tv_dc = CreateCompatibleDC(dc);
        ed.tv_old = (HBITMAP)SelectObject(ed.tv_dc, ed.tv_bmp);
        ReleaseDC(nullptr, dc);
    }
    auto mk = [](int px, int weight, const wchar_t *face) {
        return CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, 0, ANTIALIASED_QUALITY, 0, face);
    };
    ed.f_large = mk(21, 600, L"Bahnschrift SemiCondensed");
    ed.f_small = mk(17, 500, L"Bahnschrift SemiCondensed");
    ed.f_header = mk(30, 600, L"Bahnschrift SemiCondensed");
    ed.f_hint = mk(15, 500, L"Bahnschrift SemiCondensed");
    ed.f_q = mk(64, 600, L"Bahnschrift SemiCondensed");
    ed.font = CreateFontW(-MulDiv(9, ed.dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    ed.bold = CreateFontW(-MulDiv(9, ed.dpi, 72), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    ed.mono = CreateFontW(-MulDiv(9, ed.dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");

    ed.dir = dir;
    ed.map_file = map_file;
    ed.where = std::move(where);
    ed.card = load_card(dir, default_name);
    if (card_image(dir, ed.card, ed.img)) make_source();
    else ed.card.image.clear();
    normalize_crop();
    refresh_picture();

    RECT r{0, 0, S(1000), S(790)};
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
    ed.loading = true;
    ed.wnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_ACCEPTFILES, L"HMPMapCard", title.c_str(), style, x, y, w, hgt, owner,
                             nullptr, inst, nullptr);
    create_controls();
    ed.loading = false;
    update_texts();
    sync_slider();
    EnableWindow(owner, FALSE);
    ShowWindow(ed.wnd, SW_SHOW);
    SetFocus(GetDlgItem(ed.wnd, IDC_FOLDER));

    MSG m;
    while (!ed.done) {
        BOOL got = GetMessageW(&m, nullptr, 0, 0);
        if (got <= 0) { PostQuitMessage(got == 0 ? (int)m.wParam : 0); break; }
        if (!IsDialogMessageW(ed.wnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    DestroyWindow(ed.wnd);
    free_source();
    SelectObject(ed.tv_dc, ed.tv_old);
    DeleteDC(ed.tv_dc);
    DeleteObject(ed.tv_bmp);
    for (HFONT f : {ed.f_large, ed.f_small, ed.f_header, ed.f_hint, ed.f_q, ed.font, ed.bold, ed.mono}) DeleteObject(f);
    E = nullptr;
}
}
