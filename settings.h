/* Persistent settings stored in %APPDATA%\SoundOverlay\settings.ini */
#ifndef SOUND_OVERLAY_SETTINGS_H
#define SOUND_OVERLAY_SETTINGS_H

#include <windows.h>

typedef struct {
    int     profile_index;     /* index into g_profiles (resolved from profile_id on load) */
    wchar_t profile_id[64];    /* stable profile key ("mw2_ht", ...) - survives reordering */
    wchar_t device_id[256];    /* WASAPI endpoint id ("" = system default) */
    int     sensitivity_tick;  /* 5..20  (0.5x .. 2.0x) */
    int     size;              /* 200..600 px */
    int     position_index;    /* matches the position combobox order */
    int     enable_foot;
    int     enable_gun;
    int     enable_veh;
    int     enable_expl;
    int     show_overlay;
    int     minimize_tray;
} Settings;

/* Fill `s` with sane defaults. */
void settings_defaults(Settings *s);

/* Load from disk into `s`. Missing keys keep their current value, so call
 * settings_defaults() first. Returns 1 if a settings file existed. */
int  settings_load(Settings *s);

/* Persist `s` to disk. Returns 1 on success. */
int  settings_save(const Settings *s);

#endif
