@echo off
setlocal
chcp 65001 >nul
title AetherAudio DSP Suite - Indítás

set "PROJECT_DIR=%~dp0"
cd /d "%PROJECT_DIR%"

echo ====================================================================
echo   AetherAudio DSP Suite - Indítás Windows alatt
echo ====================================================================
echo.

:: 1. Megpróbáljuk elindítani a Python helyi webszervert, ha elérhető (Chrome/Edge CORS-mentes betöltéshez)
where python >nul 2>nul
if %errorlevel% equ 0 (
    echo [OK] Python észlelve, helyi szerver indítása a 3000-es porton...
    start /b python -m http.server 3000 --directory "%PROJECT_DIR%" >nul 2>nul
    timeout /t 1 >nul
    echo [OK] Böngésző megnyitása: http://localhost:3000/gui_integrated_system.html
    start "" "http://localhost:3000/gui_integrated_system.html"
    goto END
)

:: 2. Ha nincs Python, közvetlenül a fájlt nyitjuk meg az alapértelmezett böngészőben
echo [Infó] Közvetlen megnyitás az alapértelmezett böngészőben...
start "" "%PROJECT_DIR%gui_integrated_system.html"

:END
echo.
echo [Kész] Az AetherAudio elindult a böngésződben.
timeout /t 3 >nul
exit /b 0
