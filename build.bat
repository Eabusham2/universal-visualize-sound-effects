@echo off
REM Build SoundOverlay.exe with MSVC (cl.exe).
REM Run from a Visual Studio "x64 Native Tools Command Prompt".

setlocal
set OUT=SoundOverlay.exe

where cl >nul 2>&1
if errorlevel 1 (
    echo.
    echo cl.exe not found on PATH. Open a "Developer Command Prompt for VS"
    echo or run vcvars64.bat first, then re-run build.bat.
    exit /b 1
)

if not exist build mkdir build

echo Compiling resources (manifest, icon, version info)...
rc /nologo /fo build\resource.res resource.rc
if errorlevel 1 goto :fail

pushd build

cl /nologo /O2 /W3 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN ^
   /I.. ^
   ../main.c ../overlay.c ../audio.c ../detector.c ../fft.c ../profiles.c ../settings.c ^
   resource.res ^
   /Fe%OUT% /link /SUBSYSTEM:WINDOWS /MANIFEST:NO ^
   user32.lib gdi32.lib ole32.lib oleaut32.lib ^
   avrt.lib comctl32.lib uuid.lib shell32.lib
if errorlevel 1 (
    popd
    goto :fail
)

echo.
echo Built: build\%OUT%
popd
del /q build\*.obj >nul 2>&1
endlocal
goto :eof

:fail
echo.
echo *** Build failed. ***
exit /b 1
