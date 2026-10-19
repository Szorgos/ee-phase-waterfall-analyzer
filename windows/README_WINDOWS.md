# AetherAudio • Windows 11 verzió

A Linuxos PipeWire csomópont Windowsos párja. Két részből áll, amelyek külön-külön is működnek:

| Fájl | Mi ez | Mit tud |
|---|---|---|
| `AetherAudio.exe` | Motor (konzolos alkalmazás) | Valódi FFT-spektrum a 3D vízeséshez, WebSocket híd a GUI-hoz (`ws://127.0.0.1:8080`), beállítások mentése |
| `AetherAudio.dll` | Equalizer APO plugin (VST 2.4) | A DSP lánc (Preamp → EQ → Crossfeed → Limiter) az `audiodg.exe`-ben, élőben a GUI-ról vezérelve |
| `gui\gui_integrated_system.html` | A meglévő felület | Ugyanaz, mint Linuxon; magától csatlakozik a motorhoz |

> **Miért VST2 és nem VST3?** Az Equalizer APO csak VST 2.4 pluginokat tud betölteni. A plugin saját, a Steinberg SDK-tól független VST2 bináris interfészt használ (`windows/plugin/vst2_abi.hpp`).

## Két üzemmód

**1. Csak analízis (plugin nélkül).** Indítsd el az `AetherAudio.exe`-t. A motor WASAPI loopbackkel az alapértelmezett lejátszóeszköz kimenetét elemzi, eszközváltáskor automatikusan átáll. A GUI-n a jelvény: *„Élő analízis – WASAPI loopback”*. Ebben a módban a GUI DSP-beállításai **nem** hatnak a hangra (Windowson a rendszerhangot csak APO-n keresztül lehet módosítani).

**2. Teljes lánc (EQ APO pluginnal).** Ha az `AetherAudio.dll` benne van az Equalizer APO láncban, a motor automatikusan róla veszi a jelet. A vízesés pontosan a plugin helyén mér, a GUI-n állított preamp/EQ/crossfeed/limiter pedig élőben szól. Jelvény: *„Élő – EQ APO plugin • 48.0 kHz”*.

## Telepítés

1. Telepítsd az [Equalizer APO](https://sourceforge.net/projects/equalizerapo/)-t, és a Device Selectorban válaszd ki a lejátszóeszközt.
2. Futtasd a csomagban lévő `install_eqapo_plugin.ps1`-et (jobb klikk → *Futtatás PowerShell-lel*; rendszergazdai jogot kér). Ez:
   - bemásolja a DLL-t ide: `C:\Program Files\EqualizerAPO\VSTPlugins\AetherAudio\`
     (a felhasználói mappákat – pl. Letöltések – az `audiodg.exe` nem tudja olvasni, ezért kell ide);
   - előkészíti a `C:\ProgramData\AetherAudio\` mappát (beállítások + plugin napló).
3. Az Equalizer APO **Configuration Editorában**: `+` → *Plugins* → *VST plugin*, és válaszd ki a bemásolt `AetherAudio.dll`-t. Oda tedd a láncban, ahol mérni akarsz: a lánc **végére**, ha azt akarod látni, ami ténylegesen kimegy a DAC-ra. Mentés (Ctrl+S).
4. Indítsd az `AetherAudio.exe`-t. Megnyitja a GUI-t az alapértelmezett böngészőben.

A plugin alapállapotban **bit-pontosan átlátszó**: amíg a GUI-ban nem állítasz semmit, csak „lehallgat”. Ha az EQ APO-ban már van saját EQ-d (pl. AutoEq profil), ne kapcsold be mellé a GUI EQ-ját is, különben kétszer szól.

### Plugin paraméterek (EQ APO szerkesztőben)
- **Bypass** – a DSP lánc kikerülése (a jel ettől még megy az analizátorba).
- **Remote** – BE: a GUI vezérli a láncot (alapértelmezés). KI: a plugin a betöltéskori mentett állapotnál marad.

A GUI utolsó beállítása a `C:\ProgramData\AetherAudio\params.bin` fájlba mentődik; a plugin ezt tölti be indításkor, így a lánc akkor is él, ha az `AetherAudio.exe` nem fut.

## Használat

```
AetherAudio.exe            # motor + GUI megnyitása
AetherAudio.exe --no-gui   # csak motor (pl. automatikus indításhoz)
```

Automatikus indításhoz tegyél egy parancsikont a `--no-gui` kapcsolóval ide: `shell:startup`.

## Hibaelhárítás

- **A jelvény „Motor Offline”**: nem fut az `AetherAudio.exe`, vagy a 8080-as portot más program foglalja (a motor ezt kiírja).
- **Fut a plugin, de a motor loopbacket használ**: nézd meg a `C:\ProgramData\AetherAudio\plugin_status.txt` naplót. Itt látszik, hogy a plugin betöltődött-e, és a közös memóriát a *Global* (audiodg) vagy a *Local* névtérben hozta-e létre.
- **Csend esetén**: az EQ APO szünetben nem hívja a plugint. Ilyenkor a vízesés lecseng, a forrás 10 percig plugin marad.
- A híd csak a `127.0.0.1` címen figyel, tűzfalszabály nem kell, és a helyi hálózatról nem vezérelhető.

## Fordítás

**Linuxról keresztfordítva** ([llvm-mingw](https://github.com/mstorsjo/llvm-mingw)):
```bash
cmake -B build-win -S . -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=windows/toolchain-llvm-mingw.cmake -DLLVM_MINGW_ROOT=/opt/llvm-mingw
cmake --build build-win
```

**Windowson** (Visual Studio 2022 + CMake):
```powershell
cmake -B build -S . -A x64
cmake --build build --config Release
```

Mindkét esetben az eredmény: `AetherAudio.exe`, `AetherAudio.dll`, valamint a tesztek (`test_core.exe` stb.). A MinGW-s binárisok statikusan linkeltek, csak a Windows saját DLL-jeit (UCRT) használják.

## Ismert korlátok

- A **Bass Enhancer** és a **Convolver** (IR betöltés) csak a GUI-ban létezik, a C++ láncban még nincs megvalósítva; a kapcsolójuk jelenleg nem változtat a hangon.
- A GUI **FIR / lineáris fázisú** EQ módja és az előcsengés-jelölés szimuláció. Élő spektrumnál ezért nem rajzolódik rá a vízesésre.
- Csak sztereó (2 csatornás) eszközre tervezve; többcsatornás eszköznél az EQ APO `Channel:` parancsával szűkítsd a plugint L/R-re.
