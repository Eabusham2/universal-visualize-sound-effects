#include "settings.h"
#include "profiles.h"

#include <shlobj.h>
#include <stdio.h>
#include <string.h>

#ifdef _MSC_VER
#pragma comment(lib, "shell32.lib")
#endif

#define APP_SECTION L"SoundOverlay"

/* Build "%APPDATA%\SoundOverlay\settings.ini", creating the folder.
 * Returns 1 on success. */
static int settings_path(wchar_t *out, size_t cap) {
    wchar_t appdata[MAX_PATH];
    /* S_FALSE means "path is valid but does not exist yet" — still usable,
     * since CreateDirectoryW below creates it. Testing != S_OK would reject
     * that case and silently disable settings persistence entirely. */
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appdata)))
        return 0;

    wchar_t dir[MAX_PATH];
    if (swprintf(dir, MAX_PATH, L"%ls\\SoundOverlay", appdata) < 0)
        return 0;
    CreateDirectoryW(dir, NULL);   /* ignore "already exists" */

    if (swprintf(out, cap, L"%ls\\settings.ini", dir) < 0)
        return 0;
    return 1;
}

void settings_defaults(Settings *s) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->profile_index    = 0;
    s->profile_id[0]    = L'\0';
    s->device_id[0]     = L'\0';
    s->sensitivity_tick = 10;   /* 1.0x */
    s->size             = 320;
    s->position_index   = 0;    /* top right */
    s->enable_foot      = 1;
    s->enable_gun       = 1;
    s->enable_veh       = 1;
    s->enable_expl      = 1;
    s->show_overlay     = 1;
    s->minimize_tray    = 0;
}

int settings_load(Settings *s) {
    if (!s) return 0;
    wchar_t path[MAX_PATH];
    if (!settings_path(path, MAX_PATH)) return 0;

    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
        return 0;  /* no file yet; caller keeps defaults */

    s->profile_index    = GetPrivateProfileIntW(APP_SECTION, L"profile",     s->profile_index,    path);
    s->sensitivity_tick = GetPrivateProfileIntW(APP_SECTION, L"sensitivity", s->sensitivity_tick, path);
    s->size             = GetPrivateProfileIntW(APP_SECTION, L"size",        s->size,             path);
    s->position_index   = GetPrivateProfileIntW(APP_SECTION, L"position",    s->position_index,   path);
    s->enable_foot      = GetPrivateProfileIntW(APP_SECTION, L"foot",        s->enable_foot,      path);
    s->enable_gun       = GetPrivateProfileIntW(APP_SECTION, L"gun",         s->enable_gun,       path);
    s->enable_veh       = GetPrivateProfileIntW(APP_SECTION, L"veh",         s->enable_veh,       path);
    s->enable_expl      = GetPrivateProfileIntW(APP_SECTION, L"expl",        s->enable_expl,      path);
    s->show_overlay     = GetPrivateProfileIntW(APP_SECTION, L"show",        s->show_overlay,     path);
    s->minimize_tray    = GetPrivateProfileIntW(APP_SECTION, L"tray",        s->minimize_tray,    path);

    GetPrivateProfileStringW(APP_SECTION, L"device", L"",
                             s->device_id, 256, path);

    /* Prefer the stable id over the numeric index: adding or reordering a
     * profile in profiles.c must never silently switch the user's game.
     * Files written before profile_id existed fall back to the index. */
    GetPrivateProfileStringW(APP_SECTION, L"profile_id", L"",
                             s->profile_id, 64, path);
    if (s->profile_id[0]) {
        int idx = profile_index_of(s->profile_id);
        /* profile_index_of() returns 0 for unknown ids; only trust it when
         * the id really matches, otherwise keep the index we already read. */
        if (lstrcmpiW(profile_by_index(idx)->id, s->profile_id) == 0)
            s->profile_index = idx;
    }
    if (s->profile_index < 0 || s->profile_index >= PROFILE_COUNT)
        s->profile_index = 0;
    return 1;
}

int settings_save(const Settings *s) {
    if (!s) return 0;
    wchar_t path[MAX_PATH];
    if (!settings_path(path, MAX_PATH)) return 0;

    wchar_t buf[32];
    #define WR_INT(key, val) \
        do { swprintf(buf, 32, L"%d", (val)); \
             WritePrivateProfileStringW(APP_SECTION, (key), buf, path); } while (0)

    WR_INT(L"profile",     s->profile_index);
    WR_INT(L"sensitivity", s->sensitivity_tick);
    WR_INT(L"size",        s->size);
    WR_INT(L"position",    s->position_index);
    WR_INT(L"foot",        s->enable_foot);
    WR_INT(L"gun",         s->enable_gun);
    WR_INT(L"veh",         s->enable_veh);
    WR_INT(L"expl",        s->enable_expl);
    WR_INT(L"show",        s->show_overlay);
    WR_INT(L"tray",        s->minimize_tray);
    #undef WR_INT

    WritePrivateProfileStringW(APP_SECTION, L"device",
                               s->device_id[0] ? s->device_id : L"", path);
    WritePrivateProfileStringW(APP_SECTION, L"profile_id",
                               profile_by_index(s->profile_index)->id, path);

    /* Flush the cached writes to disk. */
    WritePrivateProfileStringW(NULL, NULL, NULL, path);
    return 1;
}
