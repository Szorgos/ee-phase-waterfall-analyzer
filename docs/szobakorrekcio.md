# Aqulify Szobakorrekció és Mérés (Room Correction & Measurement)

Dokumentáció és technikai útmutató az Aqulify szobamérési, akusztikai korrekciós és import/export funkcióihoz.  
*Documentation and technical guide for Aqulify room measurement, acoustic correction, and import/export capabilities.*

---

## 1. Magyar nyelvű leírás (Hungarian)

### 1.1. Alapelvek és Egyszerűség (Rule 0)
Az Aqulify szobakorrekciós rendszere úgy lett megtervezve, hogy a zenehallgatóknak egyértelmű, azonnal hallható javulást nyújtson anélkül, hogy akusztikai mérnöki ismeretekre lenne szükségük.
- **Alapértelmezett varázsló mód (Simple Wizard):** Egy 5 lépéses folyamat vezeti végig a felhasználót.
- **Közérthető nyelvezet:** Zavaró szakkifejezések helyett a hallható hatást magyarázzuk el (pl. „A 60 Hz körüli dübörgést csökkentettük, a vokálok tisztábbak lettek”).
- **Biztonság és védelem:** 
  - A mérési jelszint gyárilag halkról indul (-20 dBFS), és szoftveresen korlátozva van (legfeljebb -6 dBFS), így sem a hangszórók, sem a felhasználó hallása nem sérülhet.
  - A korrekciós kiemelések biztonsági limitje legfeljebb **+3.0 dB** (nem emelünk ki mély akusztikai kioltásokat, elkerülve a hangszóró túlterhelését és a torzítást). A szobai rezonanciák (kiemelések) csillapítása viszont erőteljes (akár -24 dB).
- **Egygombos vezérlés:** Egygombos alkalmazás, egygombos A/B összehasonlítás, azonnali visszavonás és profilként mentés.
- **Haladó mód (Advanced):** Szakmai részletek (egyedi biquad szűrők, simítás 1/1..1/48 oktáv, célgörbe-szerkesztő, sávcsökkentő) csak a Haladó nézetben jelennek meg.

### 1.2. Szakirodalmi források és tisztaszobás (Clean-room) megvalósítás
A szobakorrekciós mag semmilyen zárt forráskódú vagy nem-GPL licencköteles külső szoftverből nem tartalmaz kódot vagy származtatott logikát. Kizárólag elismert, publikus szakirodalomból és matematikai képletekből épül fel:
1. **Farina, Angelo (2000):** *"Simultaneous Measurement of Impulse Response and Distortion with a Swept-Sine Technique"*, 108th AES Convention. (Logaritmikus szinusz sweep és inverz szűrő generálása, dekonvolúció).
2. **Olive, Sean E. et al. (2009):** *"The Subjective and Objective Evaluation of Room Correction Products"*, AES 127th Convention. (Szobai célgörbék hallgatósági preferenciái és kiegyenlített frekvenciamenete).
3. **Olive, Sean E., Welti, Todd, & McMullin, Elisabeth (2013):** *"Listener Preferences for In-Room Loudspeaker and Headphone Target Responses"*, AES 135th Convention. (A természetes szobai célgörbe: enyhe mélykiemelés és lefelé lejtő magas tartomány).
4. **Linkwitz, Siegfried & Riley, Russ (1976):** *"Active Crossover Networks for Noncoincident Drivers"*, JAES. (LR2, LR4, LR8 aktív fázishelyes keresztváltó szűrők).
5. **Müller, Swen & Massarani, Paulo (2001):** *"Transfer-Function Measurement with Sweeps"*, JAES. (Törtoktávos energiaspektrum-simítás és ablakozás).
6. **Equalizer APO formátum specifikáció:** Hivatalos Equalizer APO dokumentáció (Preamp, Filter Peaking/Shelf/Pass/Notch, Channel, Delay, GraphicEQ, Include, Copy).
7. **CamillaDSP konfigurációs specifikáció:** Hivatalos CamillaDSP YAML dokumentáció (Biquad, Gain, Delay, Conv).

### 1.3. A Szobagörbe Modellje, Célgörbék és Korrekciós Határok
A hangszórós szobakorrekcióhoz egyetlen, a motorban számított szobagörbe modellt használunk (a felület mindig a motor által visszaadott célgörbét és sávokat jeleníti meg):
- **Logaritmikus szakaszonként egyenes görbe:**
  - **Mély tartomány:** A beállítható töréspont alatt (alapérték 160 Hz, állítható 60–250 Hz között) a görbe 20 Hz felé egyenletesen emelkedik (alapérték +4,0 dB, állítható −6 dB .. +10 dB között).
  - **Középtartomány:** A töréspont és 1000 Hz között egyenes (0 dB).
  - **Magastartomány:** 1000 Hz felett 20 000 Hz felé egyenletesen esik (alapérték −3,0 dB 20 kHz-nél, állítható −6 dB .. +3 dB között).
- **Választható célgörbe modellek:**
  - **Kiegyensúlyozott szobahang (Olive 2009 / 2013):** Természetes, zenei hangzás szobai környezetben (+4 dB mély, −3 dB magas).
  - **Meleg szobahang:** Erőteljesebb basszus alap (+6 dB mélykiemelés, −3 dB magas).
  - **Lineáris / Egyenes (Flat):** Szigorúan egyenes 0 dB válasz (stúdiós kontrollhoz).
- **Korrekciós határok:**
  - Teljes erejű korrekció 500 Hz alatt; fölötte legfeljebb széles (Q ≤ 1,0) és enyhe (±2,5 dB) hangszínbeli igazítás.
  - A hangszóró saját alsó határa alatt (−6 dB pont a 300 Hz – 3 kHz átlaghoz képest) a motor nem emel.
  - Mély, keskeny kioltásokat (Q > 3,0) a motor kihagy (és erről az összefoglalóban szól).
  - A sávok **összegzett** emelése sem lépheti túl a +3,0 dB-t.
- **Változó simítás (Variable Smoothing):** 200 Hz alatt 1/24 oktáv (a szobamódusok pontos megfogásához), 2 kHz fölött 1/3 oktáv (a magas frekvenciás fésűszűrés kisimításához), közte egyenletes logaritmikus átmenettel.
- **Csatornánkénti (bal/jobb) auto-EQ:** „Mindkettő” (sztereó) mérésnél a motor a közös átlag mellett a bal és a jobb csatornára **külön-külön** is kiszámolja a korrekciós sávokat ugyanazzal a célgörbével, simítással és korrekciós határokkal (`bands_left`, `bands_right`). Mindkét csatorna görbéjét a saját 300 Hz – 3 kHz átlagához igazítja 0 dB-re, így az EQ nem próbálja szűrőkkel kiegyenlíteni a két hangfal hangerő-különbségét; az esetleges szintkülönbséget (`lr_level_diff_db`) kizárólag tájékoztatásul jelzi (1 dB fölött a felületen is megjelenik, a hangerő-egyensúlyt a felhasználó állíthatja). A 4. lépésben alapértelmezett a *„Csatornánként (ajánlott)”* mód, de egy kattintással átváltható *„Közös korrekció”* módra. Egycsatornás („Bal” / „Jobb”) mérés alkalmazásakor, ha előtte közös EQ volt aktív, a másik csatorna megörökli a korábbi közös sávokat.
- **Hangerő-tartalék (Headroom) és közös előerősítés:** Alkalmazáskor a program egyetlen, mindkét csatornára közös előerősítést (preamp) állít be – csatornánkénti korrekciónál a két csatorna összegzett pozitív EQ-csúcsa közül a **nagyobb** (vagyis a negatívabb előerősítés) és a választott tartalék (−12…−1 dB, alapérték −4,0 dB) alapján, hogy a bal–jobb egyensúly ne tolódjon el –, és szükség esetén bekapcsolja a limitert. Ha az összegzett EQ-görbének egyik csatornán sincs pozitív csúcsa (≤ +0,05 dB, azaz csak vágó sávok vannak), az előerősítés **0 dB** marad (tartalék nélkül). Az A/B kapcsoló mellett 1 dB-nél nagyobb 200 Hz–5 kHz átlagos hangerő-eltérésnél tájékoztató sor jelenik meg. Az A/B kapcsoló és a Visszavonás a szétválasztott (split) állapotot és az előerősítést is pontosan visszaállítja.
- **Többpontos mérés:** A motor tárolja a munkamenet pontjait (3–5 pont ajánlott a hallgatási hely körül), **teljesítményben (energiában)** átlagolja őket, és bármelyik rossz vagy zajos pont külön törölhető. Zajos szobában és egyetlen mérőpontból a keskenyebb vágások gyakran csak arra a helyre érvényesek – csendben, 3–5 pontból megbízhatóbb a korrekció.
- **Mérés közbeni automatikus bypass:** A mérés (és a szintpróba) idejére az Aqulify kikapcsolja a saját effektláncát (EQ, konvolver, hangszínező modulok, előerősítés, kimeneti késleltetés / szint / polaritás), hogy a mérés tisztán a hangsugárzók és a szoba akusztikáját tükrözze; a végén – leállításkor és hiba esetén is – visszaállítja. A **keresztváltó és a limiter bekapcsolva marad**: a magassugárzó így mérés közben sem kap mélyhangot. A varázsló a fő sztereó (Bal / Jobb / Mindkettő) kimenetet méri; az útonkénti EQ külön, a *Kimenetek* fülről hangolható.
- **Indítási védelem:** A felvétel első ~200 ms-a eldobásra kerül (USB-mikrofon indítási kattanása ellen), a szint- és zajszámítás előtt pedig DC-eltávolítás fut.
- **Hogyan szól a mérőjel:** a motor saját lejátszója játssza le (a gépen szóló zene közben szünetel), a mikrofon felvétele ezzel egy időben fut; az időzítést a dekonvolúció a felvételből határozza meg.

### 1.4. A Varázsló 5 lépése
1. **Mikrofon és kalibráció:** Mérőmikrofon kiválasztása (a gép valódi bemenetei), opcionális kalibrációs fájl (.cal, .txt, .frd, .csv) betöltése logaritmikus frekvencia-interpolációval (mikrofononként megjegyezve, 0°/90° jelzéssel), és mérendő csatorna (Bal / Jobb / Mindkettő).
2. **Szintellenőrzés (Level Check):** Rózsaszín zaj a mérés szintjén (legfeljebb 15 s, leállítható, a próba közepén 6 dB-es lépcsővel az automatikus erősítés / AGC észlelésére), közben a mikrofon valódi szintje: −40 dBFS alatt „túl halk”, −6 dBFS fölött „túl hangos”. A mérésnél automatikus hibavédelem: `NO_INPUT_DEVICE`, `MIC_TOO_QUIET`, `MIC_CLIPPING`, `NOISY_ROOM` (a háttérzajt a lejátszás előtti utolsó ~200 ms csendből méri).
3. **Akusztikai mérés (Sweep):** Logaritmikus sweep (10 Hz – 20 kHz; a kiértékelés 20 Hz-től) lejátszása és felvétele, folyamatjelzővel, élő mikrofonszinttel, többpontos méréssel (pontszám, rossz pont törlése) és azonnali leállító gombbal. „Mindkettő” esetén egy felvételben előbb a bal, 1 s szünet után a jobb csatorna szól: így a két hangszóró érkezési ideje és polaritása összevethető.
4. **Eredmény és kiértékelés:** Közérthető összefoglaló a javasolt korrekcióról (és az esetleg kihagyott mély kioltásokról), grafikus mért / cél / korrigált görbe (sztereó mérésnél csatornánként külön L/R görbével és Haladó szűrőlistával, választható *Csatornánként* / *Közös korrekció* móddal), motoroldali újraszámítás a célgörbe csúszkáinak vagy a simításnak a változtatásakor. „Mindkettő” mérésnél a bal–jobb érkezési különbség (ms és cm), a szintkülönbség (1 dB fölött) és az eltérő polaritás jelzése (bekötési hiba) – ezek tájékoztatások, a program egyiket sem állítja át magától.
5. **Alkalmazás és teszt:** Egy kattintásos érvénybe léptetés (csatornánkénti vagy közös EQ-sávok + közös biztonságos előerősítés), egygombos A/B összehasonlítás a natív és korrigált hangzás között, egygombos visszavonás (a split-állapottal és előerősítéssel együtt) és mentés profilként.

### 1.5. Új képességek
- **Csatornánkénti (L/R) EQ és késleltetés:** Független 32-32 sávos EQ, mintapontos késleltetés (0–50 ms), szint- és polaritásállítás a bal és jobb csatornára külön-külön.
- **Sávcsökkentő (Band Reducer):** Intelligens algoritmus, amely a sok sávos (pl. 32 sávos vagy GraphicEQ) beállítást a felhasználó által kívánt kevesebb (pl. 5-7) sávra egyszerűsíti a legfontosabb szobarezonanciák megtartásával, kijelezve a maximális és RMS dB hibát.
- **Valódi sztereó (4 csatornás) konvolúció:** LL, LR, RL, RR impulzusválasz mátrix keresztcsatolás (cross-talk) támogatással, vagy két különálló mono fájlpár egyesítése.
- **Keresztváltó és útonkénti EQ:** Aktív több-utas rendszerekben minden kimeneti út a Linkwitz–Riley (12/24/48 dB/okt) sávfelosztás után saját, nullakésleltetésű **útonkénti EQ**-val hangolható a *„Kimenetek”* fül **„EQ”** gombjával (pl. a mélysugárzó szobai dübörgésének vagy a magassugárzó kiemelésének finomhangolására; legfeljebb 10 sáv/út, Bell / Low shelf / High shelf, 20 Hz – 20 kHz, −18…+6 dB, Q 0,3–10, A/B kapcsolóval, kattanásmentes átmenettel). A közös EQ és a többi modul továbbra is a közös láncban fut a sávfelosztás előtt; részletek: [keresztvalto.md](keresztvalto.md).
- **Automatikus profilváltás:** Kimeneti eszközváltáskor (fejhallgató / hangszóró / USB DAC) automatikusan betöltődik a hozzá rendelt hangzásprofil.

---

## 2. English Description

### 2.1. Philosophy & Simplicity (Rule 0)
Aqulify Room Correction is designed to deliver immediate, audible improvements without requiring audio engineering expertise:
- **Default Wizard Mode:** Simple 5-step guided workflow.
- **Jargon-free Explanations:** Real-world auditory impact explained clearly (e.g., "Tamed boomy 60 Hz bass resonance; vocals are clearer").
- **Safety First:** Test signals start softly (-20 dBFS default) with hard upper ceiling at -6 dBFS. Max boost is capped at **+3.0 dB** to prevent driver distortion and protect speakers.
- **Single-click Actions:** One-click apply, one-button A/B comparison, one-click undo, and save as profile.
- **Advanced Mode:** Deep technical controls (raw biquads, fractional smoothing 1/1..1/48 octave, target curves, band reducer) available on demand.

### 2.2. Literature References & Clean-room Design
The room correction core contains zero proprietary or non-GPL licensed third-party code (clean-room design). Algorithms are built strictly from public academic literature and mathematical specs:
1. **Farina, Angelo (2000):** *"Simultaneous Measurement of Impulse Response and Distortion with a Swept-Sine Technique"*, 108th AES Convention.
2. **Olive, Sean E. et al. (2009 / 2013):** In-room loudspeaker target response preferences (AES 127th & 135th Conventions).
3. **Linkwitz, Siegfried & Riley, Russ (1976):** *"Active Crossover Networks for Noncoincident Drivers"*, JAES.
4. **Müller, Swen & Massarani, Paulo (2001):** *"Transfer-Function Measurement with Sweeps"*, JAES.
5. **Equalizer APO Specification:** Official syntax and file format.
6. **CamillaDSP Specification:** Official YAML configuration structure.

### 2.3. The 5 Wizard Steps
1. **Microphone & Calibration:** Select microphone, load optional calibration file (.cal, .txt, .frd, .csv) with log-frequency interpolation and 0°/90° detection, choose channel (Left / Right / Both).
2. **Level Check:** Pink noise test tone (with a 6 dB step to detect OS automatic gain control / enhancements) to verify mic gain. Standard error codes: `NO_INPUT_DEVICE`, `MIC_TOO_QUIET`, `MIC_CLIPPING`, `NOISY_ROOM`.
3. **Measurement:** Synchronized logarithmic sweep and recording (startup click trim + DC removal), progress bar, emergency Stop button, multi-point measurement (3–5 points recommended) with energy power averaging and per-point deletion. In a noisy room and from a single measurement point, narrower cuts often apply only to that exact spot – measuring in a quiet room across 3–5 points gives a more reliable correction.
4. **Results:** Plain-language summary (including skipped deep narrow nulls and the calibration file used), left–right arrival time, level difference (> 1 dB, informational only) and polarity comparison, per-channel (default) or shared correction mode for stereo measurements (each channel normalized to its own 300 Hz – 3 kHz average), visual before/target/corrected L/R curves, and live engine-side recomputation when adjusting target sliders or smoothing (variable 1/24 low – 1/3 high, or fixed fractional octave).
5. **Apply & Test:** One-click apply (per-channel or shared EQ bands + a single shared safe preamp taken from the larger positive peak across both channels so L/R balance stays intact; 0 dB with no extra margin when all bands are cuts only), instantaneous A/B comparison (with a loudness-difference note when the 200 Hz – 5 kHz average differs by at least 1 dB), one-click undo (restoring split state and preamp too), and save to profile list.
