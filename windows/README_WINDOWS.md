# Aqulify • Windows 11 verzió

## Telepítés (egy fájl, minden automatikus)

Futtasd az **`Aqulify-Setup.exe`**-t. A varázsló:

1. bemásolja a programot (alapból `C:\Program Files\Aqulify`);
2. felismeri az **Equalizer APO**-t. Ha nincs telepítve, felajánlja a letöltését (sourceforge.net) és elindítja a telepítőjét. Ott jelöld be azt a hangeszközt, amin zenét hallgatsz, a végén pedig indítsd újra a gépet;
3. bemásolja a plugint az EQ APO `VSTPlugins\Aqulify` mappájába, és a `config.txt` **végére** egyetlen sort tesz: `Include: Aqulify.txt`. A saját szűrőid érintetlenek maradnak, a régi `config.txt`-ről mentés készül (`config.txt.aqulify-backup`);
4. Start menü és asztali parancsikont, opcionálisan automatikus indítást hoz létre;
5. bejegyzi magát a *Gépház → Alkalmazások* (Programok és szolgáltatások) listába, rendes eltávolítóval.

Frissítéskor ugyanezt a telepítőt kell futtatni. A futó motort bezárja, és a zenelejátszás közben zárolt plugin-DLL-t is kicseréli: a régi példány félreáll, és újraindításkor törlődik.

Az **eltávolító** visszavesz mindent: az `Include` sort, a plugint, a parancsikonokat, az automatikus indítást és a programmappát. A mentett beállítások törléséről külön rákérdez. Az Equalizer APO megmarad.

Csendes telepítés (rendszergazdáknak):

```
Aqulify-Setup.exe /S [/D=C:\Mappa] [/NOAPO] [/DOWNLOADAPO]
"C:\Program Files\Aqulify\Uninstall.exe" /uninstall /S
```

> A telepítő jelenleg nincs digitálisan aláírva. A Windows SmartScreen figyelmeztethet (*További információ → Futtatás mindenképp*), a bekapcsolt Intelligens alkalmazásvezérlés (Smart App Control) pedig le is tiltja a programot. Lépésenkénti leírás, ellenőrzőösszeg és a Smart App Control kezelése: [README – A telepítő nincs digitálisan aláírva](../README.md#️-a-telepítő-nincs-digitálisan-aláírva--így-indíthatod-el).

## Használat

Az Aqulify a **tálcán** (értesítési területen) fut, konzolablak nélkül:

- **kattintás / dupla kattintás** az ikonon: megnyílik a felület a böngészőben;
- **jobb kattintás**: aktuális jelforrás, *Indítás a Windows-zal* ki/be, *Napló megnyitása*, *Kilépés*.

A felület jelvénye mutatja, honnan jön a jel:

| Jelvény | Jelentés |
|---|---|
| **Élő – EQ APO plugin • 48.0 kHz** | Teljes működés: a vízesés a lánc végén mér, a felületen állított DSP élőben szól |
| **Élő analízis – WASAPI loopback** | Nincs aktív plugin: csak analízis, a DSP beállítások nem hatnak a hangra |
| **Motor Offline (Szimuláció)** | Nem fut az `Aqulify.exe` |

A beállítások (EQ, modulok, betöltött impulzusválasz) a `C:\ProgramData\Aqulify\` mappába mentődnek. A plugin onnan tölti be őket, így a lánc akkor is él, ha a motor nem fut.

## Felület

A felület saját ablakban fut (`AqulifyGui.exe`, Microsoft WebView2 – a Windows 11 része). A motor tálcaikonjáról vagy a Start menüből nyitható meg; egyszerre egy ablak lehet nyitva. Az ablak bezárása nem állítja le a hangfeldolgozást. Ha a gépen nincs WebView2 futtatókörnyezet, a felület az alapértelmezett böngészőben nyílik meg.

## DSP modulok

| Modul | Megvalósítás |
|---|---|
| **EQ – IIR** | Biquad lánc (RBJ), minimálfázisú, elhanyagolható többletkésleltetés |
| **EQ – lineáris fázisú FIR** | A teljes EQ görbéből tervezett szimmetrikus FIR (8192 tap 48 kHz-en). Késleltetés ~85 ms; a videó hangja ennyit csúszhat |
| **EQ – köztes fázisú FIR** | Részben minimálfázisú FIR, ~8,5 ms késleltetés, mérsékelt előcsengés |
| **EQ – minimálfázisú FIR** | Minimálfázisú FIR (cepstrum módszer), elhanyagolható késleltetés, nincs előcsengés |
| **EQ Decramp** | Ki / 2x / 4x / 8x: az IIR biquadok N-szeres mintavételen futnak (Kaiser-sinc fel/lemintavételezés, ~1,3 ms), így a Nyquist közeli menet nem „nyomódik össze”; FIR módban a cél-görbe kramp-mentes, többletköltség nélkül |
| **Crossfeed** | Bauer / Meier / Stúdió preset, vagy egyedi vágási frekvencia, szint és ITD (fej-késleltetés, tört mintás késleltetővonal) |
| **Convolver** | Egyenletesen partícionált FFT konvolver, 0 többletkésleltetéssel (az első 256 minta közvetlen konvolúcióval). WAV/IRS betöltés (16/24/32 bit, float), automatikus újramintavételezés a lejátszás mintavételére, opcionális minimálfázisúvá alakítás, csúcsra normalizálás + trim gain. Max. 262 144 minta |
| **Bass Enhancer** | Pszichoakusztikai mélyhang: 2./3. felharmonikus generálás burkoló-normalizált Csebisev-polinomokkal, sávszűrés, 4. rendű mélyvágás |
| **Limiter** | True peak: 1x / 2x / 4x / 8x túlmintavételezett (Lanczos) csúcsdetektor, 0,5–10 ms lookahead (csúcstartás + simítás: a beavatkozás a csúcs előtt kész), exponenciális felengedés, biztonsági vágás a plafonnál. Késleltetés = lookahead + 3 minta |

A FIR tervezés és az impulzusválasz-előkészítés külön háttérszálon fut. A kész szűrőt a hangszál zármentesen veszi át, egy blokknyi keresztúsztatással, így váltáskor nincs kattanás.

## Hibaelhárítás

- **Napló:** tálcaikon → *Napló megnyitása* (`%LOCALAPPDATA%\Aqulify\engine.log`). A plugin naplója: `C:\ProgramData\Aqulify\plugin_status.txt`. Itt látszik, hogy az `audiodg.exe` betöltötte-e, és milyen beállítással.
- **„Élő analízis – WASAPI loopback” a plugin telepítése után:** frissen telepített Equalizer APO után újraindítás kell. Az EQ APO *Device Selector*-ában ellenőrizd, hogy a használt eszköz be van-e jelölve.
- **A felület böngészőben nyílik meg saját ablak helyett:** hiányzik a WebView2 futtatókörnyezet. Telepíthető a Microsoft oldaláról („Evergreen Bootstrapper”).
- **Lejátszás szünetében** az EQ APO nem hívja a plugint; a vízesés ilyenkor lecseng, és ez nem hiba.
- A motor csak a `127.0.0.1` címen figyel (az első szabad port a 47810–47814 tartományból, végső tartalékként 8080), tűzfalszabály nem kell. A felület magától megtalálja. Ha mind foglalt, az indítási üzenet megnevezi a portot használó programot.

## Fordítás

**Linuxról keresztfordítva** ([llvm-mingw](https://github.com/mstorsjo/llvm-mingw)):
```bash
cmake -B build-win -S . -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=windows/toolchain-llvm-mingw.cmake -DLLVM_MINGW_ROOT=/opt/llvm-mingw
cmake --build build-win      # -> Aqulify-Setup.exe, Aqulify.exe, Aqulify.dll, tesztek
```

**Windowson** (Visual Studio 2022 + CMake):
```powershell
cmake -B build -S . -A x64
cmake --build build --config Release
```

Kipróbálás EQ APO nélkül: az `eqapo_host_sim.exe <Aqulify.dll>` úgy tölti be a plugint, ahogy az Equalizer APO. Három tesztszinuszt küld át rajta, és másodpercenként kiírja a kimenő szinteket. Közben a motor felülete élőben vezérelhető.

## Ismert korlátok

- Csak sztereó feldolgozás: többcsatornás eszköznél az EQ APO a plugint csatornapáronként példányosítja, az analizátor az első párt (L/R) mutatja.
- A késleltetéssel járó beállítások (lineáris fázisú FIR EQ, limiter lookahead, decramp) összeadódnak; videónézéshez az IIR EQ és rövid lookahead ajánlott.
- Aláíratlan kiadásnál az Intelligens alkalmazásvezérlés letilthatja a programot (lásd fent).
