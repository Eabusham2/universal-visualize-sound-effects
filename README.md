# SoundOverlay

A native C / Win32 game audio visualizer that captures your PC's audio
output via WASAPI loopback and shows a transparent, click-through HUD
with directional markers for **footsteps**, **gunshots**, **vehicles**,
and **explosions**.

No external dependencies. Pure C, pure Win32. Single `.exe`.

## Supported Games (built-in profiles)

| Profile | Tuned for | Key features |
|---------|-----------|-------------|
| **Universal** | Any game | Balanced defaults |
| **Fortnite** | Fortnite | Wide footstep band, build/edit awareness |
| **Call of Duty: Warzone** | Warzone / MW | Heavy bass compensation, vehicle detection |
| **COD MW2 (Home Theater)** | MW2 with Home Theater audio | Slow noise floor for quiet steps in cinematic mix, tight 60-180 Hz foot band, high explosion gate to reject LFE bass, very-low vehicle band (15-55 Hz), bigger default overlay for living-room distance |
| **Valorant** | Valorant | Crisp footsteps, no vehicle noise |
| **Counter-Strike 2** | CS2 | Tight thresholds, fast cooldowns |
| **Apex Legends** | Apex | Legend-varied footsteps, vehicle rumble |
| **PUBG: Battlegrounds** | PUBG | Long-range shots, vehicle emphasis |
| **Rainbow Six Siege** | R6 Siege | Breach sounds, precise footsteps |
| **Overwatch 2** | OW2 | Ability-aware, varied heroes |
| **Escape from Tarkov** | Tarkov | Ultra-sensitive, realistic audio |

Each profile tunes frequency bands, detection thresholds, cooldowns,
noise-floor adaptation speed, and default overlay settings.

### MW2 Home Theater profile details

The "Home Theater" audio preset in COD MW2 is a wide, cinematic 5.1/7.1
mix with heavy sub-bass and large dynamic range. Footsteps are quiet
relative to gunfire and ambient sound. This profile is specifically tuned
for that mix:

- **Footstep band narrowed to 60-180 Hz** — avoids sub-50 Hz LFE bleed
  that would cause false triggers from the cinema bass
- **Low footstep threshold (2.8x)** — compensates for quiet steps
- **Slow noise-floor adaptation (alpha 0.012)** — prevents the floor
  from rising to swallow footsteps during loud firefights
- **Longer warmup (60 frames)** — the mix is loud and varied at match
  start; more time to stabilize
- **High explosion threshold (5.0x)** — the heavy LFE content in Home
  Theater mode would otherwise constantly trigger explosion markers
- **Vehicle band at 15-55 Hz** — MW2 vehicle rumble is very low and
  distinct from the 60-180 Hz footstep band
- **Bigger default overlay (380 px)** — Home Theater implies a living-room
  setup with more viewing distance
- **Slightly more sensitive default (0.85x)** — because footsteps are
  genuinely quieter in this mix mode

## Features

- **WASAPI loopback** capture of any render endpoint — no virtual cable
  needed
- **4 event types**: footsteps (green), gunshots (red), vehicles (blue),
  explosions (orange) with distinct HUD markers
- **FFT-based detection** with spectral flux onset detection, adaptive
  per-band noise floors, and refractory periods
- **11 game profiles** with per-game tuned frequency bands and thresholds
- **Launcher / settings GUI**:
  - Game selection buttons (click to switch profile, even while running)
  - Audio device picker
  - Sensitivity slider (0.5x - 2.0x)
  - Overlay size slider (200 - 600 px)
  - Position selector (5 positions)
  - Per-event-type enable/disable checkboxes
  - Show/hide overlay toggle
  - Minimize-to-tray option
  - Live event log with timestamps, direction and loudness (clearable)
  - Running detection stats and session timer
  - Audio device list refreshes itself when you plug in / unplug a
    headset or DAC (no restart needed)
  - Only one launcher runs at a time; starting a second copy just brings the
    existing window to the front
- **Per-band stereo localization**: each event's direction is computed from
  the L/R energy *within its own frequency band*, so a footstep on the left
  is not pulled toward center by centered music or gunfire
- **Proximity rendering**: louder (closer) events are drawn nearer the HUD
  center, quieter ones near the rim
- **Settings persistence**: profile, device, sensitivity, size, position,
  and toggles are saved to `%APPDATA%\SoundOverlay\settings.ini` on exit (and
  on logoff/shutdown) and restored on the next launch. The profile is stored
  by its stable id (`mw2_ht`, `cs2`, …), so reordering or adding profiles
  never switches your game
- **Device-loss recovery**: if the audio device is unplugged or becomes
  unavailable mid-session, the pipeline stops cleanly, the device list is
  refreshed, and you're prompted to reconnect
- **Transparent overlay**: `WS_EX_LAYERED | WS_EX_TRANSPARENT |
  WS_EX_TOPMOST` with chroma-key, double-buffered GDI at ~30 fps, placed
  inside the work area of the monitor it is on (never under the taskbar,
  wherever it is docked)
- **System tray**: right-click for Show/Hide, Start/Stop, Exit
- **Global hotkeys**:
  - `Ctrl + F10` — quit from anywhere
  - `Ctrl + F9` — toggle the overlay on/off
  - `Ctrl + F8` — start/stop detection

## Source layout

```
fft.c/h         Radix-2 in-place FFT
audio.c/h       WASAPI loopback capture, downmix, resample, ring buffer
detector.c/h    FFT analysis + 4-type classifier with profile params
overlay.c/h     Layered click-through HUD with 4 marker shapes
profiles.c/h    11 game profiles with tuned detection parameters
settings.c/h    Persistent settings (%APPDATA%\SoundOverlay\settings.ini)
main.c          Launcher GUI, tray, event log, pipeline orchestration
app.manifest    Common Controls v6 (themed controls) + supported-OS list
resource.rc     Embeds the manifest, the icon and version info
assets/icon.ico App icon (generated by scripts/make_icon.py, no deps)
tests/fft_test.c  Host-side unit test for the FFT (`make test`)
Makefile        MinGW / cross build + tests
build.bat       MSVC build
build-mingw.bat mingw-w64 build without make
```

## Download a prebuilt .exe

You don't have to build it yourself — GitHub Actions cross-compiles the
executable on every push.

- **Latest build**: Actions tab → newest **Build SoundOverlay** run →
  **Artifacts** → `SoundOverlay` (artifacts expire after 90 days)
- **Tagged release**: pushing a `v*` tag (e.g. `git tag v1.0 && git push
  origin v1.0`) publishes a GitHub Release with `SoundOverlay.exe`
  attached, giving a permanent download link

## Building

### MSVC (recommended)

Open an **x64 Native Tools Command Prompt** for VS 2019/2022:

```
build.bat
```

Output: `build\SoundOverlay.exe` (static CRT, no console, no dependencies).

### mingw-w64

From MSYS2 mingw64 or any shell with `gcc` = `x86_64-w64-mingw32-gcc`:

```
mingw32-make          # or: build-mingw.bat (no make needed)
```

Cross-compile from Linux/macOS:

```
make CC=x86_64-w64-mingw32-gcc WINDRES=x86_64-w64-mingw32-windres
```

All builds embed `app.manifest` (themed Common Controls) and the icon via
`resource.rc`. Regenerate the icon with `python3 scripts/make_icon.py`.

### Tests

The FFT core is plain C and is checked against a reference DFT on any OS:

```
make test
```

CI runs the tests, then cross-builds with MinGW and also validates the MSVC
`build.bat` path on Windows.

## Usage

1. Launch `SoundOverlay.exe`
2. Click a **game profile** button (or leave on Universal)
3. Pick your **audio device** (the one your game plays through)
4. Adjust **sensitivity** if needed (lower = more triggers)
5. Toggle which event types to detect
6. Click **Start**

The compass-shaped overlay appears with:
- **STEP** (green circle) — footsteps
- **SHOT** (red starburst) — gunshots
- **VEH** (blue diamond) — vehicles
- **BOOM** (orange multi-ray) — explosions

All markers are positioned on the compass ring based on stereo pan
(left/right) and fade out over 1.4 seconds.

The **event log** in the launcher shows every detection with timestamp,
type, direction and loudness (the percentage: quiet/far events near 33%,
loud/close ones up to 100%). **Clear log** empties it. Stats at the bottom
track total counts and session duration.

### Controls

| Action | How |
|--------|-----|
| Quit | Ctrl + F10 (global) or close the window |
| Toggle overlay | Ctrl + F9 (global) |
| Start / stop detection | Ctrl + F8 (global) or the Start/Stop buttons |
| Minimize to tray | Check "Minimize to tray", then minimize |
| Tray menu | Right-click the tray icon |
| Switch game mid-session | Click a different game button |
| Adjust sensitivity live | Drag the slider while running |

Your last-used profile, device, sensitivity, overlay size/position, and
detection toggles are saved automatically and restored the next time you
launch.

## How detection works

1. **Capture**: WASAPI loopback at 48 kHz stereo, 1-second ring buffer
2. **Window**: 2048-sample Hann window, 1024-sample hop (50% overlap)
3. **FFT**: Radix-2 in-place, magnitude spectrum
4. **Bands**: Profile-defined frequency ranges for each event type
5. **Noise floor**: Per-band exponential moving average (profile-tuned alpha)
6. **Classification**:
   - Gunshot = high spectral flux + elevated gun band + ultra band + loudness gate
   - Explosion = elevated explosion band + positive onset flux + high absolute energy
   - Footstep = elevated foot band + onset + low-dominance over gun band
   - Vehicle = sustained sub-bass rumble above threshold
7. **Direction**: Separate L/R channel FFTs; pan is computed from the L/R
   energy *within each event's own frequency band* → angle on the compass.
   This keeps concurrent sounds in different bands from smearing each
   other's direction.
8. **Proximity**: Event loudness maps to radial distance — louder events are
   drawn closer to the HUD center, quieter ones toward the rim.

## Tuning tips

- **Too many false positives**: raise sensitivity toward 1.5x
- **Missing events**: lower toward 0.7x
- **Specific event noise**: uncheck that event type
- **Best direction**: use stereo output, disable surround virtualization
- Switch profiles when switching games — each profile's frequency bands
  match that game's audio mix

## Known limitations

- Stereo cannot resolve front vs. back (only left/right/center)
- Heuristic classification has inherent false positives on unusual audio
- Windows only (WASAPI + Win32 layered windows)
