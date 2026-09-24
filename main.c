/* SoundOverlay Launcher — full settings GUI with game profiles,
 * event log, detection toggles, system tray, and stats.
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define COBJMACROS

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <dbt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "detector.h"
#include "overlay.h"
#include "profiles.h"
#include "settings.h"

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#define APP_CLASS   L"SoundOverlayLauncher"
#define APP_TITLE   L"SoundOverlay"
#define APP_MUTEX   L"Local\\SoundOverlayLauncher.SingleInstance"
#define IDI_APPICON 101              /* keep in sync with resource.rc */

/* Control IDs */
#define ID_CB_DEVICE      1001
#define ID_SL_SENS        1002
#define ID_SL_SIZE        1003
#define ID_CB_POS         1004
#define ID_CK_SHOW        1005
#define ID_BTN_START      1006
#define ID_BTN_STOP       1007
#define ID_CK_FOOT        1010
#define ID_CK_GUN         1011
#define ID_CK_VEH         1012
#define ID_CK_EXPL        1013
#define ID_CK_TRAY        1014
#define ID_LBL_SENS       1020
#define ID_LBL_SIZE       1021
#define ID_LBL_STATUS     1022
#define ID_LBL_STATS      1023
#define ID_LIST_LOG       1030
#define ID_BTN_CLEARLOG   1031
#define ID_BTN_GAME_BASE  2000   /* 2000 .. 2000+PROFILE_COUNT-1 */

/* Tray */
#define WM_TRAY           (WM_APP + 10)
#define TRAY_ID           1
#define IDM_TRAY_SHOW     3001
#define IDM_TRAY_STARTSTOP 3002
#define IDM_TRAY_EXIT     3003

/* Cross-thread messages */
#define WM_APP_EVENT      (WM_APP + 2)   /* detector posted an event       */
#define WM_APP_DEVLOST    (WM_APP + 3)   /* capture thread died unexpectedly */

/* Timers */
#define TIMER_STATS       2   /* 1 s session clock / counters while running */
#define TIMER_DEVREFRESH  3   /* coalesces bursts of WM_DEVICECHANGE        */

/* Global hotkey ids */
#define HK_QUIT           1   /* Ctrl+F10 */
#define HK_OVERLAY        2   /* Ctrl+F9  */
#define HK_STARTSTOP      3   /* Ctrl+F8  */

/* Event log capacity */
#define LOG_MAX_LINES     200

typedef struct {
    HINSTANCE        inst;
    HWND             main_wnd;

    /* Game selection (max 16 to allow headroom beyond PROFILE_COUNT) */
    HWND             game_btns[16];
    int              active_profile;

    /* Controls */
    HWND             cb_device, sl_sens, sl_size, cb_pos;
    HWND             ck_show, ck_foot, ck_gun, ck_veh, ck_expl, ck_tray;
    HWND             btn_start, btn_stop;
    HWND             lbl_sens, lbl_size, lbl_status, lbl_stats;
    HWND             list_log;

    AudioDeviceInfo  devices[AUDIO_MAX_DEVICES];
    int              device_count;

    Overlay         *overlay;
    AudioCapture    *capture;
    SoundDetector    detector;
    int              detector_ready;

    HANDLE           det_thread;
    volatile LONG    det_running;
    volatile LONG    det_stop;

    /* Stats */
    volatile LONG    stat_foot;
    volatile LONG    stat_gun;
    volatile LONG    stat_veh;
    volatile LONG    stat_expl;
    DWORD            start_tick;

    /* Tray */
    NOTIFYICONDATAW  nid;
    int              tray_added;

    int              hotkeys_registered;

    HFONT            ui_font;      /* system message font for every control */
    HICON            icon_big, icon_small;
} App;

static App g_app;

/* ---- event log ------------------------------------------------------- */

static void log_event(App *a, const SoundEvent *e) {
    const wchar_t *kind = L"???";
    const wchar_t *dir  = L"C";
    switch (e->kind) {
        case SE_FOOTSTEP:  kind = L"STEP"; break;
        case SE_GUNSHOT:   kind = L"SHOT"; break;
        case SE_VEHICLE:   kind = L"VEH";  break;
        case SE_EXPLOSION: kind = L"BOOM"; break;
    }
    if (e->pan < -0.3f)      dir = L"L";
    else if (e->pan > 0.3f)  dir = L"R";
    else if (e->pan < -0.1f) dir = L"CL";
    else if (e->pan > 0.1f)  dir = L"CR";

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[128];
    swprintf(buf, 128, L"%02d:%02d:%02d  %-4ls  %ls  (%.0f%%)",
             st.wHour, st.wMinute, st.wSecond,
             kind, dir, e->strength * 33.3f);

    int count = (int)SendMessageW(a->list_log, LB_GETCOUNT, 0, 0);
    if (count >= LOG_MAX_LINES)
        SendMessageW(a->list_log, LB_DELETESTRING, 0, 0);
    int idx = (int)SendMessageW(a->list_log, LB_ADDSTRING, 0, (LPARAM)buf);
    SendMessageW(a->list_log, LB_SETTOPINDEX, idx, 0);
}

static void update_stats(App *a) {
    DWORD elapsed = (GetTickCount() - a->start_tick) / 1000;
    int m = (int)(elapsed / 60), s = (int)(elapsed % 60);
    wchar_t buf[256];
    swprintf(buf, 256,
        L"Steps: %ld   Shots: %ld   Veh: %ld   Boom: %ld   |   %02d:%02d",
        a->stat_foot, a->stat_gun, a->stat_veh, a->stat_expl, m, s);
    SetWindowTextW(a->lbl_stats, buf);
}

/* ---- detector thread ------------------------------------------------- */

static DWORD WINAPI detector_thread(LPVOID arg) {
    App *a = (App *)arg;
    float *left  = (float *)malloc(sizeof(float) * AUDIO_FFT_FRAMES);
    float *right = (float *)malloc(sizeof(float) * AUDIO_FFT_FRAMES);
    if (!left || !right) { free(left); free(right); return 1; }

    while (!InterlockedCompareExchange(&a->det_stop, 0, 0)) {
        int rc = audio_capture_next_window(a->capture, 200, left, right);
        if (rc < 0) {
            /* Capture died unexpectedly (e.g. device unplugged). Notify the
             * UI thread so it can reset, unless we are stopping on purpose. */
            if (!InterlockedCompareExchange(&a->det_stop, 0, 0))
                PostMessageW(a->main_wnd, WM_APP_DEVLOST, 0, 0);
            break;
        }
        if (rc == 0) continue;

        SoundEvent ev[SE_KIND_COUNT];
        int n = detector_analyze(&a->detector, left, right, ev, SE_KIND_COUNT);
        for (int i = 0; i < n; ++i) {
            overlay_add_event(a->overlay, &ev[i]);
            switch (ev[i].kind) {
                case SE_FOOTSTEP:  InterlockedIncrement(&a->stat_foot); break;
                case SE_GUNSHOT:   InterlockedIncrement(&a->stat_gun);  break;
                case SE_VEHICLE:   InterlockedIncrement(&a->stat_veh);  break;
                case SE_EXPLOSION: InterlockedIncrement(&a->stat_expl); break;
            }
            /* pan (-1000..1000) in the low word, strength x100 in the high
             * word; the log used to receive a constant strength of 1.0 and
             * therefore printed "(33%)" for every event. */
            PostMessageW(a->main_wnd, WM_APP_EVENT, (WPARAM)ev[i].kind,
                         MAKELPARAM((WORD)(short)(int)(ev[i].pan * 1000.0f),
                                    (WORD)(int)(ev[i].strength * 100.0f + 0.5f)));
        }
    }
    free(left); free(right);
    InterlockedExchange(&a->det_running, 0);
    return 0;
}

/* ---- helpers --------------------------------------------------------- */

static void set_status(App *a, const wchar_t *s) {
    SetWindowTextW(a->lbl_status, s);
}

static int selected_device_index(App *a) {
    int idx = (int)SendMessageW(a->cb_device, CB_GETCURSEL, 0, 0);
    if (idx < 0 || idx >= a->device_count) return 0;
    return idx;
}

static OverlayPosition selected_position(App *a) {
    int idx = (int)SendMessageW(a->cb_pos, CB_GETCURSEL, 0, 0);
    switch (idx) {
        case 0: return OP_TOP_RIGHT;
        case 1: return OP_TOP_LEFT;
        case 2: return OP_BOTTOM_RIGHT;
        case 3: return OP_BOTTOM_LEFT;
        case 4: return OP_CENTER;
        default: return OP_TOP_RIGHT;
    }
}

static float slider_sensitivity(App *a) {
    int v = (int)SendMessageW(a->sl_sens, TBM_GETPOS, 0, 0);
    if (v < 5)  v = 5;
    if (v > 20) v = 20;
    return (float)v / 10.0f;
}

static int slider_size(App *a) {
    int v = (int)SendMessageW(a->sl_size, TBM_GETPOS, 0, 0);
    if (v < 200) v = 200;
    if (v > 600) v = 600;
    return v;
}

static void update_slider_labels(App *a) {
    wchar_t buf[64];
    swprintf(buf, 64, L"%.1fx", slider_sensitivity(a));
    SetWindowTextW(a->lbl_sens, buf);
    swprintf(buf, 64, L"%d px", slider_size(a));
    SetWindowTextW(a->lbl_size, buf);
}

static void populate_devices(App *a) {
    SendMessageW(a->cb_device, CB_RESETCONTENT, 0, 0);
    a->device_count = audio_list_devices(a->devices, AUDIO_MAX_DEVICES);
    for (int i = 0; i < a->device_count; ++i) {
        wchar_t label[320];
        if (a->devices[i].is_default)
            swprintf(label, 320, L"[default] %ls", a->devices[i].name);
        else
            swprintf(label, 320, L"%ls", a->devices[i].name);
        SendMessageW(a->cb_device, CB_ADDSTRING, 0, (LPARAM)label);
    }
    if (a->device_count > 0)
        SendMessageW(a->cb_device, CB_SETCURSEL, 0, 0);
}

/* Re-enumerate endpoints (e.g. after a headset was plugged in) without
 * losing the device the user had picked. Used while the pipeline is idle. */
static void refresh_devices_keep_selection(App *a) {
    wchar_t keep[256];
    int di = selected_device_index(a);
    keep[0] = L'\0';
    if (a->device_count > 0 && di >= 0 && di < a->device_count)
        lstrcpynW(keep, a->devices[di].id, 256);
    int before = a->device_count;
    populate_devices(a);
    if (keep[0]) {
        for (int i = 0; i < a->device_count; ++i) {
            if (lstrcmpW(a->devices[i].id, keep) == 0) {
                SendMessageW(a->cb_device, CB_SETCURSEL, i, 0);
                break;
            }
        }
    }
    if (a->device_count != before)
        set_status(a, a->device_count > before
                      ? L"New audio device detected - device list updated."
                      : L"An audio device was removed - device list updated.");
}

static OverlayColors colors_from_profile(const GameProfile *p) {
    OverlayColors c;
    c.foot = p->color_foot;
    c.gun  = p->color_gun;
    c.veh  = p->color_veh;
    c.expl = p->color_expl;
    return c;
}

static void select_profile(App *a, int idx) {
    if (idx < 0 || idx >= PROFILE_COUNT) idx = 0;
    a->active_profile = idx;
    const GameProfile *p = profile_by_index(idx);

    /* Highlight selected game button. */
    for (int i = 0; i < PROFILE_COUNT && i < 16; ++i) {
        if (a->game_btns[i]) {
            EnableWindow(a->game_btns[i], i != idx ? TRUE : FALSE);
        }
    }

    /* Apply defaults from profile. */
    int sens_tick = (int)(p->default_sensitivity * 10.0f);
    if (sens_tick < 5) sens_tick = 5;
    if (sens_tick > 20) sens_tick = 20;
    SendMessageW(a->sl_sens, TBM_SETPOS, TRUE, sens_tick);
    SendMessageW(a->sl_size, TBM_SETPOS, TRUE, p->default_size);

    SendMessageW(a->ck_foot, BM_SETCHECK, p->enable_foot ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(a->ck_gun,  BM_SETCHECK, p->enable_gun  ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(a->ck_veh,  BM_SETCHECK, p->enable_veh  ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(a->ck_expl, BM_SETCHECK, p->enable_expl ? BST_CHECKED : BST_UNCHECKED, 0);

    update_slider_labels(a);

    /* If running, hot-swap profile on detector + overlay. */
    if (a->detector_ready) {
        detector_apply_profile(&a->detector, p);
        detector_set_sensitivity(&a->detector, slider_sensitivity(a));
    }
    if (a->overlay) {
        OverlayColors c = colors_from_profile(p);
        overlay_set_colors(a->overlay, &c);
        overlay_reconfigure(a->overlay, slider_size(a), selected_position(a));
    }

    wchar_t status[256];
    swprintf(status, 256, L"Profile: %ls", p->display_name);
    set_status(a, status);
}

/* ---- settings persistence ------------------------------------------- */

static void position_to_combo_index(App *a, int pos_index) {
    if (pos_index < 0 || pos_index > 4) pos_index = 0;
    SendMessageW(a->cb_pos, CB_SETCURSEL, pos_index, 0);
}

static void select_device_by_id(App *a, const wchar_t *id) {
    if (!id || !id[0]) return;
    for (int i = 0; i < a->device_count; ++i) {
        if (lstrcmpW(a->devices[i].id, id) == 0) {
            SendMessageW(a->cb_device, CB_SETCURSEL, i, 0);
            return;
        }
    }
}

/* Apply loaded settings to the controls. Profile defaults are applied
 * first (via select_profile), then the user's saved tweaks override. */
static void apply_settings(App *a, const Settings *s) {
    select_profile(a, s->profile_index);

    int st = s->sensitivity_tick;
    if (st < 5) st = 5;
    if (st > 20) st = 20;
    SendMessageW(a->sl_sens, TBM_SETPOS, TRUE, st);

    int sz = s->size;
    if (sz < 200) sz = 200;
    if (sz > 600) sz = 600;
    SendMessageW(a->sl_size, TBM_SETPOS, TRUE, sz);

    position_to_combo_index(a, s->position_index);
    select_device_by_id(a, s->device_id);

    SendMessageW(a->ck_foot, BM_SETCHECK, s->enable_foot ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(a->ck_gun,  BM_SETCHECK, s->enable_gun  ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(a->ck_veh,  BM_SETCHECK, s->enable_veh  ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(a->ck_expl, BM_SETCHECK, s->enable_expl ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(a->ck_show, BM_SETCHECK, s->show_overlay ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(a->ck_tray, BM_SETCHECK, s->minimize_tray ? BST_CHECKED : BST_UNCHECKED, 0);

    update_slider_labels(a);
}

/* Read the current control state into a Settings struct for saving. */
static void gather_settings(App *a, Settings *s) {
    settings_defaults(s);
    s->profile_index    = a->active_profile;
    s->sensitivity_tick = (int)SendMessageW(a->sl_sens, TBM_GETPOS, 0, 0);
    s->size             = slider_size(a);
    s->position_index   = (int)SendMessageW(a->cb_pos, CB_GETCURSEL, 0, 0);
    if (s->position_index < 0) s->position_index = 0;
    s->enable_foot   = SendMessageW(a->ck_foot, BM_GETCHECK, 0, 0) == BST_CHECKED;
    s->enable_gun    = SendMessageW(a->ck_gun,  BM_GETCHECK, 0, 0) == BST_CHECKED;
    s->enable_veh    = SendMessageW(a->ck_veh,  BM_GETCHECK, 0, 0) == BST_CHECKED;
    s->enable_expl   = SendMessageW(a->ck_expl, BM_GETCHECK, 0, 0) == BST_CHECKED;
    s->show_overlay  = SendMessageW(a->ck_show, BM_GETCHECK, 0, 0) == BST_CHECKED;
    s->minimize_tray = SendMessageW(a->ck_tray, BM_GETCHECK, 0, 0) == BST_CHECKED;

    int di = selected_device_index(a);
    if (di >= 0 && di < a->device_count)
        lstrcpynW(s->device_id, a->devices[di].id, 256);
    else
        s->device_id[0] = L'\0';
}

static void save_current_settings(App *a) {
    Settings s;
    gather_settings(a, &s);
    settings_save(&s);
}

/* ---- tray ------------------------------------------------------------ */

static void tray_add(App *a) {
    if (a->tray_added) return;
    memset(&a->nid, 0, sizeof(a->nid));
    a->nid.cbSize = sizeof(a->nid);
    a->nid.hWnd = a->main_wnd;
    a->nid.uID = TRAY_ID;
    a->nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    a->nid.uCallbackMessage = WM_TRAY;
    a->nid.hIcon = a->icon_small ? a->icon_small : LoadIcon(NULL, IDI_APPLICATION);
    lstrcpynW(a->nid.szTip, L"SoundOverlay", 64);
    Shell_NotifyIconW(NIM_ADD, &a->nid);
    a->tray_added = 1;
}

static void tray_remove(App *a) {
    if (!a->tray_added) return;
    Shell_NotifyIconW(NIM_DELETE, &a->nid);
    a->tray_added = 0;
}

static void tray_show_menu(App *a) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_TRAY_SHOW,
                IsWindowVisible(a->main_wnd) ? L"Hide" : L"Show");
    AppendMenuW(menu, MF_STRING, IDM_TRAY_STARTSTOP,
                a->det_running ? L"Stop" : L"Start");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(a->main_wnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, a->main_wnd, NULL);
    /* Required so clicking away dismisses the menu when the owner window is
     * not foreground (the usual case while minimized to the tray). */
    PostMessageW(a->main_wnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

/* ---- pipeline -------------------------------------------------------- */

static void stop_pipeline(App *a) {
    InterlockedExchange(&a->det_stop, 1);
    /* Clear here rather than relying on the thread: if CreateThread failed in
     * start_pipeline, det_running would otherwise stay 1 forever and every
     * Start affordance (tray menu, Ctrl+F8) would keep routing to Stop. */
    InterlockedExchange(&a->det_running, 0);
    if (a->det_thread) {
        /* Wait indefinitely. A bounded wait that timed out would close the
         * handle and let us free the capture, detector, and overlay out from
         * under a thread that is still using them. The loop's capture call
         * has its own 200 ms timeout, so a live thread exits promptly. */
        WaitForSingleObject(a->det_thread, INFINITE);
        CloseHandle(a->det_thread);
        a->det_thread = NULL;
    }
    if (a->capture) {
        audio_capture_stop(a->capture);
        audio_capture_destroy(a->capture);
        a->capture = NULL;
    }
    if (a->detector_ready) {
        detector_free(&a->detector);
        a->detector_ready = 0;
    }
    if (a->overlay) overlay_hide(a->overlay);
    EnableWindow(a->btn_start, TRUE);
    EnableWindow(a->btn_stop,  FALSE);
    set_status(a, L"Stopped.");
}

static int start_pipeline(App *a) {
    /* Refuse to start over a pipeline that is still partly alive. After a
     * device loss the thread has exited but capture/detector still exist
     * until the queued WM_APP_DEVLOST is handled; starting in that window
     * (Ctrl+F8, or the tray, from inside a modal loop) would re-init the
     * detector and orphan the old capture thread and its COM objects. */
    if (a->det_thread || a->capture || a->detector_ready) return -1;

    if (a->device_count <= 0) {
        set_status(a, L"No audio devices found.");
        return -1;
    }
    const GameProfile *p = profile_by_index(a->active_profile);

    if (detector_init(&a->detector, p, slider_sensitivity(a)) != 0) {
        set_status(a, L"Detector init failed.");
        return -1;
    }
    a->detector_ready = 1;

    /* Apply checkbox overrides. */
    detector_set_enable(&a->detector, SE_FOOTSTEP,
        SendMessageW(a->ck_foot, BM_GETCHECK, 0, 0) == BST_CHECKED);
    detector_set_enable(&a->detector, SE_GUNSHOT,
        SendMessageW(a->ck_gun,  BM_GETCHECK, 0, 0) == BST_CHECKED);
    detector_set_enable(&a->detector, SE_VEHICLE,
        SendMessageW(a->ck_veh,  BM_GETCHECK, 0, 0) == BST_CHECKED);
    detector_set_enable(&a->detector, SE_EXPLOSION,
        SendMessageW(a->ck_expl, BM_GETCHECK, 0, 0) == BST_CHECKED);

    int di = selected_device_index(a);
    a->capture = audio_capture_create(a->devices[di].id);
    if (!a->capture || audio_capture_start(a->capture) != 0) {
        set_status(a, L"Audio capture failed.");
        detector_free(&a->detector); a->detector_ready = 0;
        if (a->capture) { audio_capture_destroy(a->capture); a->capture = NULL; }
        return -1;
    }

    OverlayColors cols = colors_from_profile(p);
    if (!a->overlay) {
        a->overlay = overlay_create(a->inst, slider_size(a),
                                    selected_position(a), &cols);
    } else {
        overlay_set_colors(a->overlay, &cols);
        overlay_reconfigure(a->overlay, slider_size(a), selected_position(a));
    }
    if (!a->overlay) {
        set_status(a, L"Overlay creation failed.");
        audio_capture_stop(a->capture);
        audio_capture_destroy(a->capture); a->capture = NULL;
        detector_free(&a->detector); a->detector_ready = 0;
        return -1;
    }

    if (SendMessageW(a->ck_show, BM_GETCHECK, 0, 0) == BST_CHECKED)
        overlay_show(a->overlay);
    else
        overlay_hide(a->overlay);

    a->stat_foot = a->stat_gun = a->stat_veh = a->stat_expl = 0;
    a->start_tick = GetTickCount();

    a->det_stop = 0;
    InterlockedExchange(&a->det_running, 1);
    a->det_thread = CreateThread(NULL, 0, detector_thread, a, 0, NULL);
    if (!a->det_thread) {
        stop_pipeline(a);
        set_status(a, L"Thread creation failed.");
        return -1;
    }

    EnableWindow(a->btn_start, FALSE);
    EnableWindow(a->btn_stop,  TRUE);
    wchar_t buf[256];
    swprintf(buf, 256, L"Listening (%ls)  |  Ctrl+F10 to quit",
             p->display_name);
    set_status(a, buf);
    /* Start stats timer. */
    SetTimer(a->main_wnd, TIMER_STATS, 1000, NULL);
    return 0;
}

/* ---- UI construction ------------------------------------------------- */

static HWND mk_static(HWND p, const wchar_t *t, int x, int y, int w, int h,
                       HINSTANCE inst) {
    return CreateWindowExW(0, L"STATIC", t, WS_CHILD | WS_VISIBLE,
                           x, y, w, h, p, NULL, inst, NULL);
}

static HWND mk_groupbox(HWND p, const wchar_t *t, int x, int y, int w, int h,
                         HINSTANCE inst) {
    return CreateWindowExW(0, L"BUTTON", t,
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        x, y, w, h, p, NULL, inst, NULL);
}

static BOOL CALLBACK set_font_cb(HWND child, LPARAM font) {
    SendMessageW(child, WM_SETFONT, (WPARAM)font, TRUE);
    return TRUE;
}

static void apply_ui_font(App *a) {
    NONCLIENTMETRICSW ncm;
    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    BOOL ok = SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, ncm.cbSize, &ncm, 0);
    if (!ok) {
        /* Pre-Vista layout of the struct (no iPaddedBorderWidth). */
        ncm.cbSize = sizeof(ncm) - sizeof(int);
        ok = SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, ncm.cbSize, &ncm, 0);
    }
    if (ok) a->ui_font = CreateFontIndirectW(&ncm.lfMessageFont);
    HFONT f = a->ui_font ? a->ui_font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    EnumChildWindows(a->main_wnd, set_font_cb, (LPARAM)f);
}

static void create_controls(App *a) {
    HINSTANCE inst = a->inst;
    HWND p = a->main_wnd;

    /* ---- Game Selection (grid of buttons, 6 per row) ---- */
    mk_groupbox(p, L" Select Game ", 10, 6, 700, 94, inst);
    {
        int bw = 108, bh = 26, gap = 5;
        int x0 = 20, y0 = 24;
        for (int i = 0; i < PROFILE_COUNT && i < 16; ++i) {
            int col = i % 6, row = i / 6;
            int bx = x0 + col * (bw + gap);
            int by = y0 + row * (bh + gap);
            a->game_btns[i] = CreateWindowExW(0, L"BUTTON",
                profile_by_index(i)->short_name,
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                bx, by, bw, bh, p,
                (HMENU)(INT_PTR)(ID_BTN_GAME_BASE + i), inst, NULL);
        }
    }

    int LX = 18;   /* left column x */
    int RX = 460;  /* right column x (event log) */
    int y = 106;

    /* ---- Left column: settings ---- */
    mk_groupbox(p, L" Audio ", LX - 8, y - 4, 430, 60, inst);
    mk_static(p, L"Device:", LX, y + 14, 50, 18, inst);
    a->cb_device = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
        LX + 52, y + 12, 366, 200, p,
        (HMENU)(INT_PTR)ID_CB_DEVICE, inst, NULL);
    y += 64;

    /* Sensitivity */
    mk_static(p, L"Sensitivity (lower = more sensitive)", LX, y, 260, 16, inst);
    a->lbl_sens = mk_static(p, L"1.0x", LX + 350, y, 60, 16, inst);
    a->sl_sens = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
        LX, y + 18, 420, 26, p, (HMENU)(INT_PTR)ID_SL_SENS, inst, NULL);
    SendMessageW(a->sl_sens, TBM_SETRANGE, TRUE, MAKELPARAM(5, 20));
    SendMessageW(a->sl_sens, TBM_SETTICFREQ, 1, 0);
    SendMessageW(a->sl_sens, TBM_SETPOS, TRUE, 10);
    y += 48;

    /* Overlay size */
    mk_static(p, L"Overlay size (px)", LX, y, 260, 16, inst);
    a->lbl_size = mk_static(p, L"320 px", LX + 350, y, 60, 16, inst);
    a->sl_size = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
        LX, y + 18, 420, 26, p, (HMENU)(INT_PTR)ID_SL_SIZE, inst, NULL);
    SendMessageW(a->sl_size, TBM_SETRANGE, TRUE, MAKELPARAM(200, 600));
    SendMessageW(a->sl_size, TBM_SETTICFREQ, 50, 0);
    SendMessageW(a->sl_size, TBM_SETPOS, TRUE, 320);
    y += 48;

    /* Position + Show */
    mk_static(p, L"Position:", LX, y + 2, 60, 18, inst);
    a->cb_pos = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
        LX + 62, y, 160, 160, p, (HMENU)(INT_PTR)ID_CB_POS, inst, NULL);
    SendMessageW(a->cb_pos, CB_ADDSTRING, 0, (LPARAM)L"Top right");
    SendMessageW(a->cb_pos, CB_ADDSTRING, 0, (LPARAM)L"Top left");
    SendMessageW(a->cb_pos, CB_ADDSTRING, 0, (LPARAM)L"Bottom right");
    SendMessageW(a->cb_pos, CB_ADDSTRING, 0, (LPARAM)L"Bottom left");
    SendMessageW(a->cb_pos, CB_ADDSTRING, 0, (LPARAM)L"Center");
    SendMessageW(a->cb_pos, CB_SETCURSEL, 0, 0);

    a->ck_show = CreateWindowExW(0, L"BUTTON", L"Show overlay",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        LX + 240, y + 2, 120, 20, p,
        (HMENU)(INT_PTR)ID_CK_SHOW, inst, NULL);
    SendMessageW(a->ck_show, BM_SETCHECK, BST_CHECKED, 0);
    y += 32;

    /* Detection toggles */
    mk_groupbox(p, L" Detection ", LX - 8, y - 2, 430, 50, inst);
    a->ck_foot = CreateWindowExW(0, L"BUTTON", L"Footsteps",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        LX + 4, y + 18, 90, 20, p,
        (HMENU)(INT_PTR)ID_CK_FOOT, inst, NULL);
    a->ck_gun = CreateWindowExW(0, L"BUTTON", L"Gunshots",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        LX + 100, y + 18, 90, 20, p,
        (HMENU)(INT_PTR)ID_CK_GUN, inst, NULL);
    a->ck_veh = CreateWindowExW(0, L"BUTTON", L"Vehicles",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        LX + 196, y + 18, 90, 20, p,
        (HMENU)(INT_PTR)ID_CK_VEH, inst, NULL);
    a->ck_expl = CreateWindowExW(0, L"BUTTON", L"Explosions",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        LX + 292, y + 18, 100, 20, p,
        (HMENU)(INT_PTR)ID_CK_EXPL, inst, NULL);
    SendMessageW(a->ck_foot, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(a->ck_gun,  BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(a->ck_veh,  BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(a->ck_expl, BM_SETCHECK, BST_CHECKED, 0);
    y += 56;

    /* Tray + Start/Stop */
    a->ck_tray = CreateWindowExW(0, L"BUTTON", L"Minimize to tray",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        LX, y + 4, 140, 20, p,
        (HMENU)(INT_PTR)ID_CK_TRAY, inst, NULL);

    a->btn_start = CreateWindowExW(0, L"BUTTON", L"  Start  ",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        LX + 200, y, 100, 32, p,
        (HMENU)(INT_PTR)ID_BTN_START, inst, NULL);
    a->btn_stop = CreateWindowExW(0, L"BUTTON", L"  Stop  ",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        LX + 310, y, 100, 32, p,
        (HMENU)(INT_PTR)ID_BTN_STOP, inst, NULL);
    EnableWindow(a->btn_stop, FALSE);
    y += 40;

    /* Status + stats */
    a->lbl_status = mk_static(p, L"Select a game profile and click Start.",
                              LX, y, 420, 20, inst);
    y += 22;
    a->lbl_stats = mk_static(p, L"", LX, y, 420, 18, inst);

    /* ---- Right column: event log ---- */
    mk_groupbox(p, L" Event Log ", RX - 8, 100, 252, 310, inst);
    a->list_log = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOSEL | LBS_HASSTRINGS,
        RX, 118, 234, 284, p,
        (HMENU)(INT_PTR)ID_LIST_LOG, inst, NULL);

    CreateWindowExW(0, L"BUTTON", L"Clear log",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        RX, 414, 100, 22, p,
        (HMENU)(INT_PTR)ID_BTN_CLEARLOG, inst, NULL);

    /* Every control is created with the legacy bitmap "System" font; give
     * them the system message font (Segoe UI on modern Windows) instead. */
    apply_ui_font(a);

    /* Set a monospace font on the log. */
    HFONT mono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             FIXED_PITCH | FF_MODERN, L"Consolas");
    if (mono) SendMessageW(a->list_log, WM_SETFONT, (WPARAM)mono, TRUE);

    populate_devices(a);
    update_slider_labels(a);
}

/* ---- WM_COMMAND ------------------------------------------------------ */

static void on_command(App *a, WPARAM wp) {
    WORD id   = LOWORD(wp);
    WORD code = HIWORD(wp);

    /* Game buttons */
    if (id >= ID_BTN_GAME_BASE && id < ID_BTN_GAME_BASE + PROFILE_COUNT
        && id < ID_BTN_GAME_BASE + 16 && code == BN_CLICKED) {
        select_profile(a, id - ID_BTN_GAME_BASE);
        return;
    }

    if (id == ID_BTN_START && code == BN_CLICKED) {
        start_pipeline(a);
    } else if (id == ID_BTN_STOP && code == BN_CLICKED) {
        stop_pipeline(a);
        KillTimer(a->main_wnd, TIMER_STATS);
    } else if (id == ID_BTN_CLEARLOG && code == BN_CLICKED) {
        SendMessageW(a->list_log, LB_RESETCONTENT, 0, 0);
    } else if (id == ID_CK_SHOW && code == BN_CLICKED) {
        if (a->overlay) {
            if (SendMessageW(a->ck_show, BM_GETCHECK, 0, 0) == BST_CHECKED)
                overlay_show(a->overlay);
            else
                overlay_hide(a->overlay);
        }
    } else if (id == ID_CB_POS && code == CBN_SELCHANGE) {
        if (a->overlay)
            overlay_reconfigure(a->overlay, slider_size(a), selected_position(a));
    } else if ((id == ID_CK_FOOT || id == ID_CK_GUN ||
                id == ID_CK_VEH  || id == ID_CK_EXPL) && code == BN_CLICKED) {
        if (a->detector_ready) {
            SoundEventKind kinds[] = { SE_FOOTSTEP, SE_GUNSHOT, SE_VEHICLE, SE_EXPLOSION };
            HWND cks[] = { a->ck_foot, a->ck_gun, a->ck_veh, a->ck_expl };
            int idx = id - ID_CK_FOOT;
            if (idx >= 0 && idx < 4)
                detector_set_enable(&a->detector, kinds[idx],
                    SendMessageW(cks[idx], BM_GETCHECK, 0, 0) == BST_CHECKED);
        }
    }
    /* Tray menu */
    else if (id == IDM_TRAY_SHOW) {
        if (IsWindowVisible(a->main_wnd)) {
            ShowWindow(a->main_wnd, SW_HIDE);
        } else {
            ShowWindow(a->main_wnd, SW_SHOW);
            SetForegroundWindow(a->main_wnd);
        }
    } else if (id == IDM_TRAY_STARTSTOP) {
        if (a->det_running) {
            stop_pipeline(a);
            KillTimer(a->main_wnd, TIMER_STATS);   /* match every other stop path */
        } else {
            start_pipeline(a);
        }
    } else if (id == IDM_TRAY_EXIT) {
        PostMessageW(a->main_wnd, WM_CLOSE, 0, 0);
    }
}

static void on_hscroll(App *a, HWND from) {
    update_slider_labels(a);
    if (from == a->sl_sens && a->detector_ready)
        detector_set_sensitivity(&a->detector, slider_sensitivity(a));
    else if (from == a->sl_size && a->overlay)
        overlay_reconfigure(a->overlay, slider_size(a), selected_position(a));
}

/* ---- wndproc --------------------------------------------------------- */

static LRESULT CALLBACK main_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App *a = &g_app;
    switch (msg) {
        case WM_COMMAND:
            on_command(a, wp);
            return 0;
        case WM_HSCROLL:
            on_hscroll(a, (HWND)lp);
            return 0;
        case WM_TIMER:
            if (wp == TIMER_STATS && a->det_running) {
                update_stats(a);
            } else if (wp == TIMER_DEVREFRESH) {
                KillTimer(hwnd, TIMER_DEVREFRESH);
                /* Only while idle: a running capture owns its endpoint, and
                 * losing it is reported through WM_APP_DEVLOST instead. */
                if (!a->det_thread && !a->capture)
                    refresh_devices_keep_selection(a);
            }
            return 0;
        case WM_DEVICECHANGE:
            /* Hot-plugging a headset/DAC used to require a restart before it
             * showed up in the device list. Coalesce the burst of
             * notifications Windows sends into one refresh. */
            if (wp == DBT_DEVNODES_CHANGED)
                SetTimer(hwnd, TIMER_DEVREFRESH, 700, NULL);
            return TRUE;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            /* Logoff/shutdown never delivers WM_CLOSE: persist the settings
             * and release the audio endpoint before the process is torn down. */
            if (wp) {
                save_current_settings(a);
                stop_pipeline(a);
                KillTimer(hwnd, TIMER_STATS);
                tray_remove(a);
            }
            return 0;
        case WM_HOTKEY:
            if ((int)wp == HK_QUIT) {
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            } else if ((int)wp == HK_OVERLAY) {
                if (a->overlay) {
                    BOOL checked = SendMessageW(a->ck_show, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    SendMessageW(a->ck_show, BM_SETCHECK, checked ? BST_UNCHECKED : BST_CHECKED, 0);
                    if (checked) overlay_hide(a->overlay);
                    else         overlay_show(a->overlay);
                }
            } else if ((int)wp == HK_STARTSTOP) {
                if (a->det_running) {
                    stop_pipeline(a);
                    KillTimer(a->main_wnd, TIMER_STATS);
                } else {
                    start_pipeline(a);
                }
            }
            return 0;
        case WM_TRAY:
            if (lp == WM_RBUTTONUP)
                tray_show_menu(a);
            else if (lp == WM_LBUTTONDBLCLK) {
                ShowWindow(a->main_wnd, SW_SHOW);
                SetForegroundWindow(a->main_wnd);
            }
            return 0;
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED &&
                SendMessageW(a->ck_tray, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                tray_add(a);
                ShowWindow(hwnd, SW_HIDE);
            }
            return 0;
        case WM_APP_EVENT: {
            SoundEvent ev;
            ev.kind = (SoundEventKind)(int)wp;
            ev.pan = (float)(short)LOWORD(lp) / 1000.0f;
            ev.strength = (float)HIWORD(lp) / 100.0f;
            ev.distance = 0;
            ev.timestamp = 0;
            log_event(a, &ev);
            return 0;
        }
        case WM_APP_DEVLOST:
            /* Fully settle state BEFORE the dialog. MessageBoxW runs a nested
             * message loop, so hotkeys and tray commands are dispatched from
             * inside it; anything done afterwards could stomp a pipeline the
             * user started from within the modal loop. */
            stop_pipeline(a);
            KillTimer(hwnd, TIMER_STATS);
            populate_devices(a);
            set_status(a, L"Audio device lost! Reconnect and click Start.");
            MessageBoxW(hwnd, L"The audio device was disconnected or became "
                        L"unavailable.\nPlease reconnect and click Start.",
                        APP_TITLE, MB_ICONWARNING | MB_OK);
            return 0;
        case WM_CLOSE:
            save_current_settings(a);
            stop_pipeline(a);
            KillTimer(hwnd, TIMER_STATS);
            tray_remove(a);
            if (a->overlay) { overlay_destroy(a->overlay); a->overlay = NULL; }
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (a->hotkeys_registered) {
                UnregisterHotKey(hwnd, HK_QUIT);
                UnregisterHotKey(hwnd, HK_OVERLAY);
                UnregisterHotKey(hwnd, HK_STARTSTOP);
            }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- entry point ----------------------------------------------------- */

static void register_main_class(HINSTANCE inst) {
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = main_wndproc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = APP_CLASS;
    wc.hIcon         = g_app.icon_big   ? g_app.icon_big   : LoadIcon(NULL, IDI_APPLICATION);
    wc.hIconSm       = g_app.icon_small ? g_app.icon_small : LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassExW(&wc);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show) {
    (void)prev; (void)cmdline;
    memset(&g_app, 0, sizeof(g_app));
    g_app.inst = inst;

    /* Single instance: two launchers would fight over the hotkeys and stack
     * two HUDs. A second launch just brings the existing window forward
     * (it may be hidden in the tray). */
    HANDLE instance_mutex = CreateMutexW(NULL, FALSE, APP_MUTEX);
    if (instance_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND prev_wnd = FindWindowW(APP_CLASS, NULL);
        if (prev_wnd) {
            ShowWindow(prev_wnd, SW_SHOW);
            ShowWindow(prev_wnd, SW_RESTORE);
            SetForegroundWindow(prev_wnd);
        }
        CloseHandle(instance_mutex);
        return 0;
    }

    INITCOMMONCONTROLSEX icc = { sizeof(icc),
        ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);

    g_app.icon_big   = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON),
                                         IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    g_app.icon_small = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON),
                                         IMAGE_ICON, 16, 16, LR_SHARED);

    register_main_class(inst);

    RECT target = { 0, 0, 720, 440 };
    AdjustWindowRect(&target, WS_OVERLAPPEDWINDOW, FALSE);

    g_app.main_wnd = CreateWindowExW(0, APP_CLASS, APP_TITLE,
        WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME),
        CW_USEDEFAULT, CW_USEDEFAULT,
        target.right - target.left, target.bottom - target.top,
        NULL, NULL, inst, NULL);
    if (!g_app.main_wnd) return 1;

    create_controls(&g_app);

    /* Load persisted settings (or fall back to defaults). */
    {
        Settings cfg;
        settings_defaults(&cfg);
        settings_load(&cfg);
        apply_settings(&g_app, &cfg);
    }

    /* Register global hotkeys: Ctrl+F10=quit, Ctrl+F9=overlay, Ctrl+F8=start/stop.
     * These can legitimately fail when another process already owns the combo,
     * so report it rather than leaving the user wondering why nothing happens. */
    {
        BOOL ok_quit = RegisterHotKey(g_app.main_wnd, HK_QUIT,
                                      MOD_CONTROL | MOD_NOREPEAT, VK_F10);
        BOOL ok_ovl  = RegisterHotKey(g_app.main_wnd, HK_OVERLAY,
                                      MOD_CONTROL | MOD_NOREPEAT, VK_F9);
        BOOL ok_ss   = RegisterHotKey(g_app.main_wnd, HK_STARTSTOP,
                                      MOD_CONTROL | MOD_NOREPEAT, VK_F8);
        g_app.hotkeys_registered = (ok_quit || ok_ovl || ok_ss);
        if (!ok_quit || !ok_ovl || !ok_ss) {
            wchar_t hb[256];
            swprintf(hb, 256,
                     L"Hotkey conflict:%ls%ls%ls already in use by another app.",
                     ok_quit ? L"" : L" Ctrl+F10",
                     ok_ovl  ? L"" : L" Ctrl+F9",
                     ok_ss   ? L"" : L" Ctrl+F8");
            set_status(&g_app, hb);
        }
    }

    ShowWindow(g_app.main_wnd, show);
    UpdateWindow(g_app.main_wnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(g_app.main_wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (g_app.ui_font) DeleteObject(g_app.ui_font);
    if (instance_mutex) CloseHandle(instance_mutex);
    return (int)msg.wParam;
}
