# Aqulify Fejlesztési Ütemterv (Roadmap)

**Állapot:** Folyamatosan karbantartott fejlesztési ütemterv  
**Utolsó frissítés:** 2026-10-22  
**Irányelv:** Moduláris, inkrementális fejlesztés (lépésről lépésre, egyeztetett token- és erőforrás-felhasználással).

---

## 1. Ütem: EQ profil viselkedés és felületi tisztítás ✅ (KÉSZ)
- [x] **Üres profil állapot:** Ha nincs kiválasztva EQ profil, a 2D vásznon hullámos görbe és fogantyúk helyett egyenes, diszkrét 0 dB kék vonal jelenik meg.
- [x] **Profil deselect:** A profilválasztóban mindig elérhető a *- nincs kiválasztva -* opció; kiválasztásakor az EQ modul bypass állapotba kerül, a sávok kiürülnek, a preamp 0.0 dB-re áll.
- [x] **Szellemprofil-mentesítés:** IIR / FIR / FFT / SPM és Decramp módváltás nem indít mentési időzítőt (`suppressAutosave: true`), üres állapotban nem generál *Saját profil*-t, és AutoEq profilnál nem készít felesleges *- módosított* másolatot.

---

## 2. Ütem: Csúszó animációk a felületen ✅ (KÉSZ)
- [x] **Tab Glider (Gombsor csúszka):** A lekerekített aktív négyzet finom lendülettel (`cubic-bezier(0.16, 1, 0.3, 1)`, 300 ms) átcsúszik az újonnan kiválasztott fül alá, felvéve annak méreteit.
- [x] **Kezelőfelületek oldalra csúszása (`tabPanesViewport`):** Az alsó modulok (EQ, Crossfeed, Convolver, Limiter, Bass Enhancer, Magic Touch) között irányhelyes (balról jobbra / jobbról balra) 80 px-es enyhe csúszással és opacity átmenettel vált a felület.
- [x] **Dinamikus igazodás:** Ablakméretezéskor (`resize`) és nyelvváltáskor azonnali, ugrásmentes pozicionálás.

---

## 3. Ütem: Nemzetköziesítés (i18n) bővítése ⏳ (Későbbi szakasz)
*Cél: A jelenlegi magyar és angol nyelv mellett a legfontosabb nemzetközi audiofil és felhasználói piacok lefedése.*

### Célnyelvek és prioritások:
1. **Német (`de`):** Németország, Ausztria, Svájc – rendkívül erős stúdiótechnikai, fejhallgatós és Linux/PipeWire közösség.
2. **Francia (`fr`):** Franciaország, Belgium, Kanada – kiterjedt európai piac.
3. **Spanyol (`es`):** Spanyolország, Latin-Amerika – hatalmas globális felhasználói bázis.
4. **Orosz (`ru`):** Kelet-Európa és FÁK térség – nagyon aktív DSP, foobar2000 és EasyEffects felhasználói réteg.

### Technikai felépítés:
- A meglévő `tools/i18n_build.py` pipeline használata (`i18n/gui/<nyelv>.json`, `i18n/native/<nyelv>.json`, `i18n/scripts/<nyelv>.json`).
- Automatikus ellenőrzés: hiányzó kulcsok, `{változók}` és HTML tagek paritása.
- C++ felületen (Win32 / GTK) tiszta UTF-8 és UTF-16 cirill karaktertámogatás.

---

## 4. Ütem: macOS támogatás ✅ (KÉSZ)
*Felhasználói engedélyezéssel és BlackHole fallbackkel jelenleg késznek tekintjük.*
- [x] **Rendszerhang rögzítés macOS alatt:** Core Audio Tap (macOS 14.2+) rendszerhang-rögzítési engedéllyel driver nélkül; régebbi verziókon (macOS 12–14.1) vagy engedély hiányában BlackHole 2ch virtuális eszközzel automatikusan működik.
- [x] **Megjelenítő réteg:** Natív menüsor-ikon (`NSStatusItem`) és beágyazott Cocoa WebKit ablak (`WKWebView`).
- [x] **DSP motor:** Közös `AetherChain` lebegőpontos feldolgozás Apple Silicon (ARM64) és Intel (x86_64) architektúrán.

---

## 5. Ütem: Magic Touch restaurációs és adaptív zenei motor 📋
*Részletes műszaki terv: [`docs/plans/magic-touch.md`](file:///docs/plans/magic-touch.md)*
- [ ] **Valós idejű diagnosztika:** Spektrális egyensúly, dobozhang (150–300 Hz), levegőhiány (10–16 kHz), sziszegés és zajszint figyelése.
- [ ] **Restoration elő-szakasz:** Valós idejű de-noise (RNNoise / SpeexDSP integráció), de-hum (50/60 Hz harmonikus notch szűrők), de-click.
- [ ] **Adaptív korrekció:** Dinamikus EQ, visszafogott többsávos korrekció és mikrodinamika-élénkítés (nem bináris ki/be, hanem fokozatos 0–100% intenzitásvezérlés).
- [ ] **Perceptuális védelem:** True-peak limiter és loudness matching (a kimeneti hangosság pontosan egyezik a bemenetivel, elkerülve a „hangosabb = jobbnak hallatszik” pszichoakusztikai csapdát).

---

## 6. Ütem: További EasyEffects plugin-paritás 📋
*Részletes követés: [`docs/plans/easyeffects-plugins.md`](file:///docs/plans/easyeffects-plugins.md)*
- [ ] Autogain (EBU R128 / LUFS mérés alapján)
- [ ] Compressor & Deesser
- [ ] Exciter & Crystalizer
- [ ] Multiband dinamikai modulok
