#include "overlay.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define OVERLAY_CLASS  L"SoundOverlayHUD"
#define EVENT_FADE_SEC 1.4f
#define EVENT_RING_CAP 64
#define OVERLAY_TIMER  1

static const COLORREF CHROMA_KEY = RGB(1, 2, 3);

static const OverlayColors DEFAULT_COLORS = {
    RGB(34,230,130), RGB(255,64,80), RGB(60,140,255), RGB(255,170,40)
};

typedef struct {
    SoundEvent ev;
    double     added_sec;
} TimedEvent;

struct Overlay {
    HINSTANCE      inst;
    HWND           hwnd;
    int            size;
    OverlayPosition pos;
    OverlayColors  colors;

    CRITICAL_SECTION lock;
    TimedEvent       events[EVENT_RING_CAP];
    int              event_count;

    double qpc_freq;
};

static double ov_now(const Overlay *o) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / o->qpc_freq;
}

/* Position the HUD inside the *work area* of the monitor it lives on, so it
 * never sits under the taskbar wherever that is docked (the old code assumed
 * a 40 px bar along the bottom of the primary monitor). */
static void place_window(Overlay *o) {
    RECT wa;
    MONITORINFO mi;
    HMONITOR mon = MonitorFromWindow(o->hwnd, MONITOR_DEFAULTTOPRIMARY);
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi)) {
        wa = mi.rcWork;
    } else if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
        wa.left = wa.top = 0;
        wa.right  = GetSystemMetrics(SM_CXSCREEN);
        wa.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    int ww = wa.right - wa.left, wh = wa.bottom - wa.top;
    int m = 24, x = wa.left + m, y = wa.top + m;
    switch (o->pos) {
        case OP_TOP_LEFT:     x = wa.left + m;                   y = wa.top + m; break;
        case OP_TOP_RIGHT:    x = wa.right - o->size - m;        y = wa.top + m; break;
        case OP_BOTTOM_LEFT:  x = wa.left + m;                   y = wa.bottom - o->size - m; break;
        case OP_BOTTOM_RIGHT: x = wa.right - o->size - m;        y = wa.bottom - o->size - m; break;
        case OP_CENTER:       x = wa.left + (ww - o->size) / 2;  y = wa.top + (wh - o->size) / 2; break;
    }
    /* Keep it on screen even when the HUD is bigger than the work area. */
    if (x + o->size > wa.right)  x = wa.right - o->size;
    if (y + o->size > wa.bottom) y = wa.bottom - o->size;
    if (x < wa.left) x = wa.left;
    if (y < wa.top)  y = wa.top;
    SetWindowPos(o->hwnd, HWND_TOPMOST, x, y, o->size, o->size,
                 SWP_NOACTIVATE);
}

/* ---- drawing helpers ------------------------------------------------- */

static COLORREF fade_color(COLORREF base, float alpha) {
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    int r = (int)(GetRValue(base) * alpha);
    int g = (int)(GetGValue(base) * alpha);
    int b = (int)(GetBValue(base) * alpha);
    if (r == 1 && g == 2 && b == 3) r = 2;
    if (r == 0 && g == 0 && b == 0) { r = 1; g = 1; b = 1; }
    return RGB(r, g, b);
}

static HFONT mk_label_font(void) {
    return CreateFontW(12, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

static void draw_compass(HDC dc, int cx, int cy, int radius) {
    HPEN ring = CreatePen(PS_SOLID, 2, RGB(50, 50, 50));
    HPEN inner = CreatePen(PS_SOLID, 1, RGB(30, 30, 30));
    HGDIOBJ op = SelectObject(dc, ring);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));

    Ellipse(dc, cx - radius, cy - radius, cx + radius, cy + radius);
    SelectObject(dc, inner);
    int hr = radius / 2;
    Ellipse(dc, cx - hr, cy - hr, cx + hr, cy + hr);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(90, 90, 90));
    HFONT f = CreateFontW(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ of = SelectObject(dc, f);

    static const struct { const wchar_t *l; int a; } labels[] = {
        {L"F",-90}, {L"R",0}, {L"B",90}, {L"L",180}
    };
    for (int i = 0; i < 4; ++i) {
        double a = labels[i].a * M_PI / 180.0;
        int tx = cx + (int)((radius + 10) * cos(a));
        int ty = cy + (int)((radius + 10) * sin(a));
        RECT r = { tx - 10, ty - 8, tx + 10, ty + 8 };
        DrawTextW(dc, labels[i].l, 1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    SelectObject(dc, of); DeleteObject(f);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(ring); DeleteObject(inner);
}

static void draw_footstep(HDC dc, int x, int y, float str, float alpha, COLORREF base) {
    COLORREF col = fade_color(base, alpha);
    int sz = (int)(10 + 4 * str);
    HBRUSH b = CreateSolidBrush(col);
    HPEN p = CreatePen(PS_SOLID, 2, col);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, p);
    Ellipse(dc, x - sz, y - sz, x + sz, y + sz);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, x - sz - 5, y - sz - 5, x + sz + 5, y + sz + 5);
    /* Deselect before deleting: DeleteObject fails on an object still
     * selected into a DC, leaking the handle on every one of the ~30
     * repaints per second until the process hits its GDI quota. */
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(b); DeleteObject(p);

    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, col);
    HFONT f = mk_label_font(); HGDIOBJ of = SelectObject(dc, f);
    RECT r = { x - 30, y - sz - 22, x + 30, y - sz - 8 };
    DrawTextW(dc, L"STEP", 4, &r, DT_CENTER | DT_SINGLELINE);
    SelectObject(dc, of); DeleteObject(f);
}

static void draw_gunshot(HDC dc, int x, int y, float str, float alpha, COLORREF base) {
    COLORREF col = fade_color(base, alpha);
    int sz = (int)(14 + 6 * str);
    HPEN p = CreatePen(PS_SOLID, 3, col);
    HGDIOBJ op = SelectObject(dc, p);
    for (int k = 0; k < 8; ++k) {
        double a = k * 45.0 * M_PI / 180.0;
        MoveToEx(dc, x, y, NULL);
        LineTo(dc, x + (int)(sz * cos(a)), y + (int)(sz * sin(a)));
    }
    HBRUSH b = CreateSolidBrush(col);
    HGDIOBJ ob = SelectObject(dc, b);
    Ellipse(dc, x - 4, y - 4, x + 4, y + 4);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(p); DeleteObject(b);

    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, col);
    HFONT f = mk_label_font(); HGDIOBJ of = SelectObject(dc, f);
    RECT r = { x - 30, y - sz - 20, x + 30, y - sz - 6 };
    DrawTextW(dc, L"SHOT", 4, &r, DT_CENTER | DT_SINGLELINE);
    SelectObject(dc, of); DeleteObject(f);
}

static void draw_vehicle(HDC dc, int x, int y, float str, float alpha, COLORREF base) {
    COLORREF col = fade_color(base, alpha);
    int sz = (int)(12 + 5 * str);
    /* Diamond shape */
    POINT pts[4] = {
        { x, y - sz }, { x + sz, y }, { x, y + sz }, { x - sz, y }
    };
    HBRUSH b = CreateSolidBrush(col);
    HPEN p = CreatePen(PS_SOLID, 2, col);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, p);
    Polygon(dc, pts, 4);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(b); DeleteObject(p);

    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, col);
    HFONT f = mk_label_font(); HGDIOBJ of = SelectObject(dc, f);
    RECT r = { x - 30, y - sz - 22, x + 30, y - sz - 8 };
    DrawTextW(dc, L"VEH", 3, &r, DT_CENTER | DT_SINGLELINE);
    SelectObject(dc, of); DeleteObject(f);
}

static void draw_explosion(HDC dc, int x, int y, float str, float alpha, COLORREF base) {
    COLORREF col = fade_color(base, alpha);
    int sz = (int)(16 + 7 * str);
    /* Multi-ray starburst (12 rays, alternating lengths) */
    HPEN p = CreatePen(PS_SOLID, 3, col);
    HGDIOBJ op = SelectObject(dc, p);
    for (int k = 0; k < 12; ++k) {
        double a = k * 30.0 * M_PI / 180.0;
        int len = (k % 2 == 0) ? sz : (int)(sz * 0.6);
        MoveToEx(dc, x, y, NULL);
        LineTo(dc, x + (int)(len * cos(a)), y + (int)(len * sin(a)));
    }
    HBRUSH b = CreateSolidBrush(col);
    HGDIOBJ ob = SelectObject(dc, b);
    Ellipse(dc, x - 6, y - 6, x + 6, y + 6);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(p); DeleteObject(b);

    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, col);
    HFONT f = mk_label_font(); HGDIOBJ of = SelectObject(dc, f);
    RECT r = { x - 30, y - sz - 20, x + 30, y - sz - 6 };
    DrawTextW(dc, L"BOOM", 4, &r, DT_CENTER | DT_SINGLELINE);
    SelectObject(dc, of); DeleteObject(f);
}

/* ---- paint ----------------------------------------------------------- */

static void paint(Overlay *o, HDC win_dc, RECT *client) {
    int W = client->right - client->left;
    int H = client->bottom - client->top;

    HDC mem = CreateCompatibleDC(win_dc);
    HBITMAP bmp = CreateCompatibleBitmap(win_dc, W, H);
    HGDIOBJ ob = SelectObject(mem, bmp);

    HBRUSH key_brush = CreateSolidBrush(CHROMA_KEY);
    RECT full = { 0, 0, W, H };
    FillRect(mem, &full, key_brush);
    DeleteObject(key_brush);

    int cx = W / 2, cy = H / 2;
    int radius = (W < H ? W : H) / 2 - 22;
    if (radius < 20) radius = 20;

    draw_compass(mem, cx, cy, radius);

    TimedEvent local[EVENT_RING_CAP];
    int n = 0;
    EnterCriticalSection(&o->lock);
    double t_now = ov_now(o);
    int keep = 0;
    for (int i = 0; i < o->event_count; ++i) {
        if ((float)(t_now - o->events[i].added_sec) < EVENT_FADE_SEC) {
            local[n++] = o->events[i];
            o->events[keep++] = o->events[i];
        }
    }
    o->event_count = keep;
    OverlayColors cols = o->colors;
    LeaveCriticalSection(&o->lock);

    for (int i = 0; i < n; ++i) {
        TimedEvent *te = &local[i];
        float age = (float)(t_now - te->added_sec);
        float alpha = 1.0f - age / EVENT_FADE_SEC;
        if (alpha < 0.0f) alpha = 0.0f;

        double ang_deg = -90.0 + te->ev.pan * 90.0;
        double a = ang_deg * M_PI / 180.0;
        /* Proximity: louder (closer) events sit nearer the center, quieter
         * ones near the rim. distance 0..1 -> 40%..92% of the ring radius. */
        float dist = te->ev.distance;
        if (dist < 0.0f) dist = 0.0f;
        if (dist > 1.0f) dist = 1.0f;
        float rr = (radius - 6) * (0.40f + 0.52f * dist);
        int px = cx + (int)(rr * cos(a));
        int py = cy + (int)(rr * sin(a));

        switch (te->ev.kind) {
            case SE_FOOTSTEP:  draw_footstep(mem,  px, py, te->ev.strength, alpha, cols.foot); break;
            case SE_GUNSHOT:   draw_gunshot(mem,   px, py, te->ev.strength, alpha, cols.gun);  break;
            case SE_VEHICLE:   draw_vehicle(mem,   px, py, te->ev.strength, alpha, cols.veh);  break;
            case SE_EXPLOSION: draw_explosion(mem, px, py, te->ev.strength, alpha, cols.expl); break;
        }
    }

    BitBlt(win_dc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
}

/* ---- wndproc --------------------------------------------------------- */

static LRESULT CALLBACK overlay_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Overlay *o = (Overlay *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_PAINT: {
            if (!o) break;
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);
            paint(o, dc, &rc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER:
            if (wp == OVERLAY_TIMER) InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_DESTROY:
            KillTimer(hwnd, OVERLAY_TIMER);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void register_class_once(HINSTANCE inst) {
    static int done = 0;
    if (done) return;
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = overlay_wndproc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = OVERLAY_CLASS;
    RegisterClassExW(&wc);
    done = 1;
}

/* ---- public API ------------------------------------------------------ */

Overlay *overlay_create(HINSTANCE inst, int size_px, OverlayPosition pos,
                        const OverlayColors *colors) {
    register_class_once(inst);
    Overlay *o = (Overlay *)calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->inst = inst;
    o->size = size_px;
    o->pos  = pos;
    o->colors = colors ? *colors : DEFAULT_COLORS;
    InitializeCriticalSection(&o->lock);
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    o->qpc_freq = (double)f.QuadPart;

    DWORD ex = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST
             | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;

    o->hwnd = CreateWindowExW(ex, OVERLAY_CLASS, L"SoundOverlay", WS_POPUP,
                              0, 0, size_px, size_px, NULL, NULL, inst, NULL);
    if (!o->hwnd) {
        DeleteCriticalSection(&o->lock);
        free(o);
        return NULL;
    }
    SetWindowLongPtrW(o->hwnd, GWLP_USERDATA, (LONG_PTR)o);
    SetLayeredWindowAttributes(o->hwnd, CHROMA_KEY, 0, LWA_COLORKEY);

    place_window(o);
    SetTimer(o->hwnd, OVERLAY_TIMER, 33, NULL);
    return o;
}

void overlay_destroy(Overlay *o) {
    if (!o) return;
    if (o->hwnd) DestroyWindow(o->hwnd);
    DeleteCriticalSection(&o->lock);
    free(o);
}

void overlay_add_event(Overlay *o, const SoundEvent *e) {
    if (!o || !e) return;
    EnterCriticalSection(&o->lock);
    if (o->event_count < EVENT_RING_CAP) {
        o->events[o->event_count].ev = *e;
        o->events[o->event_count].added_sec = ov_now(o);
        o->event_count++;
    } else {
        memmove(&o->events[0], &o->events[1],
                sizeof(TimedEvent) * (EVENT_RING_CAP - 1));
        o->events[EVENT_RING_CAP - 1].ev = *e;
        o->events[EVENT_RING_CAP - 1].added_sec = ov_now(o);
    }
    LeaveCriticalSection(&o->lock);
}

void overlay_reconfigure(Overlay *o, int size_px, OverlayPosition pos) {
    if (!o) return;
    o->size = size_px;
    o->pos = pos;
    place_window(o);
    InvalidateRect(o->hwnd, NULL, TRUE);
}

void overlay_set_colors(Overlay *o, const OverlayColors *c) {
    if (!o || !c) return;
    EnterCriticalSection(&o->lock);
    o->colors = *c;
    LeaveCriticalSection(&o->lock);
}

void overlay_show(Overlay *o) {
    if (o) ShowWindow(o->hwnd, SW_SHOWNOACTIVATE);
}

void overlay_hide(Overlay *o) {
    if (o) ShowWindow(o->hwnd, SW_HIDE);
}

HWND overlay_hwnd(const Overlay *o) {
    return o ? o->hwnd : NULL;
}
