@echo off
REM Alternative build using mingw-w64 (GCC for Windows). Equivalent to
REM `mingw32-make`; kept for people without make on PATH.

setlocal
where gcc >nul 2>&1
if errorlevel 1 (
    echo.
    echo gcc not found on PATH. Install mingw-w64 or use build.bat instead.
    exit /b 1
)
where windres >nul 2>&1
if errorlevel 1 (
    echo.
    echo windres not found on PATH - it ships with mingw-w64 next to gcc.
    exit /b 1
)

if not exist build mkdir build

windres -i resource.rc -o build\resource.o
if errorlevel 1 goto :fail

gcc -O2 -Wall -Wextra -municode -mwindows -static ^
    -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN ^
    main.c overlay.c audio.c detector.c fft.c profiles.c settings.c ^
    build\resource.o ^
    -o build\SoundOverlay.exe ^
    -luser32 -lgdi32 -lole32 -loleaut32 -lavrt -lcomctl32 -luuid -lshell32
if errorlevel 1 goto :fail

echo.
echo Built: build\SoundOverlay.exe
endlocal
goto :eof

:fail
echo.
echo *** Build failed. ***
exit /b 1
