@echo off
setlocal
chcp 65001 >nul
title AetherAudio DSP Suite - Windows Telepítő

echo ====================================================================
echo   AetherAudio • Valós Idejű Fázis & Akusztikai Vízesés DSP Csomag
echo                     Windows Telepítő & Beállító
echo ====================================================================
echo.

set "SCRIPT_DIR=%~dp0"
set "TARGET_BAT=%SCRIPT_DIR%run_windows.bat"
set "SHORTCUT_PATH=%USERPROFILE%\Desktop\AetherAudio DSP Suite.lnk"

echo [1/2] Windows indítószkript ellenőrzése...
if not exist "%TARGET_BAT%" (
    echo [Hiba] Nem található a run_windows.bat fájl!
    pause
    exit /b 1
)

echo [2/2] Asztali parancsikon (Desktop Shortcut) létrehozása...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$ws = New-Object -ComObject WScript.Shell; " ^
    "$s = $ws.CreateShortcut('%SHORTCUT_PATH%'); " ^
    "$s.TargetPath = '%TARGET_BAT%'; " ^
    "$s.WorkingDirectory = '%SCRIPT_DIR%'; " ^
    "$s.Description = 'AetherAudio - Real-time Phase & Acoustic Waterfall DSP Suite'; " ^
    "$s.IconLocation = 'shell32.dll,168'; " ^
    "$s.Save();"

if exist "%SHORTCUT_PATH%" (
    echo.
    echo ====================================================================
    echo   SIKERES TELEPÍTÉS!
    echo   Az AetherAudio parancsikonja kikerült az Asztalra:
    echo   "%SHORTCUT_PATH%"
    echo ====================================================================
) else (
    echo [Figyelem] A parancsikont nem sikerült automatikusan létrehozni.
    echo Közvetlenül indítható: run_windows.bat
)

echo.
echo Nyomj egy gombot az AetherAudio azonnali elindításához, vagy zárd be az ablakot!
pause >nul
start "" "%TARGET_BAT%"
