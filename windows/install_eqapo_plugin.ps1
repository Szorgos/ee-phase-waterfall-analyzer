# AetherAudio – Equalizer APO plugin telepítő
# Használat (rendszergazdaként fut, ha kell, magától kér emelést):
#   jobb klikk -> "Futtatás PowerShell-lel"      (telepítés)
#   powershell -ExecutionPolicy Bypass -File install_eqapo_plugin.ps1 -Uninstall   (eltávolítás)
param([switch]$Uninstall)

$ErrorActionPreference = 'Stop'

# --- Rendszergazdai jog -------------------------------------------------------
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    if ($Uninstall) { $argList += '-Uninstall' }
    Start-Process -FilePath 'powershell.exe' -ArgumentList $argList -Verb RunAs
    exit
}

# --- Equalizer APO helye --------------------------------------------------------
$eqapo = $null
foreach ($key in 'HKLM:\SOFTWARE\EqualizerAPO', 'HKLM:\SOFTWARE\WOW6432Node\EqualizerAPO') {
    try { $eqapo = (Get-ItemProperty -Path $key -ErrorAction Stop).InstallPath } catch {}
    if ($eqapo) { break }
}
if (-not $eqapo) { $eqapo = Join-Path $env:ProgramFiles 'EqualizerAPO' }
if (-not (Test-Path (Join-Path $eqapo 'Editor.exe'))) {
    Write-Host "Az Equalizer APO nem található itt: $eqapo" -ForegroundColor Red
    Write-Host "Telepítsd: https://sourceforge.net/projects/equalizerapo/"
    Read-Host 'Enter a kilépéshez'
    exit 1
}

$pluginDir = Join-Path $eqapo 'VSTPlugins\AetherAudio'
$dataDir = Join-Path $env:ProgramData 'AetherAudio'

if ($Uninstall) {
    if (Test-Path $pluginDir) { Remove-Item -Recurse -Force $pluginDir }
    Write-Host 'A plugin fájlja törölve. A Configuration Editorban vedd ki a VST plugin sort is!' -ForegroundColor Yellow
    Write-Host "A mentett beállítások itt maradtak (kézzel törölhető): $dataDir"
    Read-Host 'Enter a kilépéshez'
    exit 0
}

# --- Plugin másolása -------------------------------------------------------------
$src = Join-Path $PSScriptRoot 'AetherAudio.dll'
if (-not (Test-Path $src)) {
    Write-Host "Nem találom az AetherAudio.dll-t a szkript mellett: $src" -ForegroundColor Red
    Read-Host 'Enter a kilépéshez'
    exit 1
}
New-Item -ItemType Directory -Force -Path $pluginDir | Out-Null
Copy-Item -Force $src (Join-Path $pluginDir 'AetherAudio.dll')
Write-Host "Plugin másolva: $pluginDir\AetherAudio.dll" -ForegroundColor Green

# --- Közös beállítás-mappa (motor ír, audiodg/LOCAL SERVICE olvas és naplóz) ------
New-Item -ItemType Directory -Force -Path $dataDir | Out-Null
& icacls $dataDir /grant '*S-1-5-19:(OI)(CI)M' '*S-1-5-4:(OI)(CI)M' '*S-1-5-32-545:(OI)(CI)RX' | Out-Null
Write-Host "Beállítás-mappa előkészítve: $dataDir" -ForegroundColor Green

Write-Host ''
Write-Host 'Következő lépések:' -ForegroundColor Cyan
Write-Host '  1. Nyisd meg az Equalizer APO Configuration Editort.'
Write-Host '  2. A lánc VÉGÉRE (a mérni kívánt pontra) add hozzá: "+" -> Plugins -> VST plugin.'
Write-Host "  3. Válaszd ki: $pluginDir\AetherAudio.dll"
Write-Host '  4. Mentés (Ctrl+S), majd indítsd az AetherAudio.exe-t.'
Write-Host ''
Write-Host 'Diagnosztika: C:\ProgramData\AetherAudio\plugin_status.txt'
Read-Host 'Enter a kilépéshez'
