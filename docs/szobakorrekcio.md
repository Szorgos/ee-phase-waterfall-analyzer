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
A szobakorrekciós mag semmilyen zárt forráskódú vagy licencköteles szoftverből (így a Cavern forráskódjából sem) nem tartalmaz kódot vagy származtatott logikát. Kizárólag elismert, publikus szakirodalomból és matematikai képletekből épül fel:
1. **Farina, Angelo (2000):** *"Simultaneous Measurement of Impulse Response and Distortion with a Swept-Sine Technique"*, 108th AES Convention. (Logaritmikus szinusz sweep és inverz szűrő generálása, dekonvolúció).
2. **Olive, Sean E. et al. (2009):** *"The Subjective and Objective Evaluation of Room Correction Products"*, AES 127th Convention. (Szobai célgörbék hallgatósági preferenciái és kiegyenlített frekvenciamenete).
3. **Olive, Sean E., Welti, Todd, & McMullin, Elisabeth (2013):** *"Listener Preferences for In-Room Loudspeaker and Headphone Target Responses"*, AES 135th Convention. (A természetes szobai célgörbe: enyhe mélykiemelés és lefelé lejtő magas tartomány).
4. **Linkwitz, Siegfried & Riley, Russ (1976):** *"Active Crossover Networks for Noncoincident Drivers"*, JAES. (LR2, LR4, LR8 aktív fázishelyes keresztváltó szűrők).
5. **Müller, Swen & Massarani, Paulo (2001):** *"Transfer-Function Measurement with Sweeps"*, JAES. (Törtoktávos komplex energiaspektrum-simítás és ablakozás).
6. **Equalizer APO formátum specifikáció:** Hivatalos Equalizer APO dokumentáció (Preamp, Filter Peaking/Shelf/Pass/Notch, Channel, Delay, GraphicEQ, Include, Copy).
7. **CamillaDSP konfigurációs specifikáció:** Hivatalos CamillaDSP YAML dokumentáció (Biquad, Gain, Delay, Conv).

### 1.3. A Szobagörbe Modellje és Célgörbék
A hangszórós szobakorrekcióhoz a szakirodalomban bizonyított természetes szobagörbe modellt alkalmazzuk:
- **Logaritmikus szakaszonként egyenes görbe:**
  - **Mély tartomány:** A beállítható térdpont alatt (alapérték 160 Hz, állítható 60–250 Hz között) a görbe 20 Hz felé egyenletesen emelkedik (alapérték +4.0 dB, állítható -6 dB .. +10 dB között).
  - **Középtartomány:** A térdpont és 1000 Hz között egyenes (0 dB).
  - **Magastartomány:** 1000 Hz felett 20 000 Hz felé egyenletesen esik (alapérték -3.0 dB 20 kHz-nél, állítható -6 dB .. +3 dB között).
- **Választható célgörbe modellek:**
  - **Kiegyensúlyozott szobahang (Olive 2009 / 2013):** Természetes, zenei hangzás szobai környezetben (+4 dB mély, -3 dB magas).
  - **Meleg szobahang:** Erőteljesebb basszus alap (+6 dB mélykiemelés).
  - **Lineáris / Egyenes (Flat):** Szigorúan egyenes 0 dB válasz (stúdiós kontrollhoz).
  - **Csak a csatornák kiegyenlítése (Channel Matching):** Nem alkalmaz abszolút célgörbét, csupán a bal és jobb csatorna akusztikai eltéréseit egyenlíti ki egymáshoz képest. Kiemelten ajánlott akkor, ha a felhasználónak nincs gyári kalibrációs fájlja a mikrofonjához.
- **Változó simítás (Variable Smoothing):** A mély tartományban finomabb felbontású (1/12 oktáv) a szobamódusok pontos megfogásához, a magas tartomány felé haladva fokozatosan simább (1/3 oktáv) a felesleges fésűs szűrések elkerülésére.
- **Hangerő-tartalék (Headroom) csúszka:** Lehetővé teszi az egyensúlyt a „Dinamika megőrzése” (nagyobb biztonsági tartalék a túlvezérlés ellen) és a „Hangerő megőrzése” (kisebb korrekciós szintcsökkenés) között. Alapérték: biztonságos -4.0 dB.
- **Mérés közbeni automatikus bypass:** A mérés (és a szintpróba) idejére az Aqulify kikapcsolja a saját effektláncát (EQ, konvolver, hangszínező modulok, előerősítés, kimeneti késleltetés / szint / polaritás), hogy a mérés tisztán a hangsugárzók és a szoba akusztikáját tükrözze; a végén – leállításkor és hiba esetén is – visszaállítja. A **keresztváltó és a limiter bekapcsolva marad**: a magassugárzó így mérés közben sem kap mélyhangot.
- **Hogyan szól a mérőjel:** a motor saját lejátszója játssza le (a gépen szóló zene közben szünetel), a mikrofon felvétele ezzel egy időben fut; az időzítést a dekonvolúció a felvételből határozza meg.

### 1.3. A Varázsló 5 lépése
1. **Mikrofon és kalibráció:** Mérőmikrofon kiválasztása (a gép valódi bemenetei), opcionális kalibrációs fájl (.cal, .txt, .frd, .csv) betöltése logaritmikus frekvencia-interpolációval (mikrofononként megjegyezve), és mérendő csatorna (Bal / Jobb / Mindkettő).
2. **Szintellenőrzés (Level Check):** Rózsaszín zaj a mérés szintjén (legfeljebb 15 s, leállítható), közben a mikrofon valódi szintje: −40 dBFS alatt „túl halk”, −6 dBFS fölött „túl hangos”. A mérésnél automatikus hibavédelem: `NO_INPUT_DEVICE`, `MIC_TOO_QUIET`, `MIC_CLIPPING`, `NOISY_ROOM` (a háttérzajt a lejátszás előtti csendből méri).
3. **Akusztikai mérés (Sweep):** Logaritmikus sweep (10 Hz – 20 kHz; a kiértékelés 20 Hz-től) lejátszása és felvétele, folyamatjelzővel, élő mikrofonszinttel és azonnali leállító gombbal. „Mindkettő” esetén egy felvételben előbb a bal, 1 s szünet után a jobb csatorna szól: így a két hangszóró érkezési ideje és polaritása összevethető.
4. **Eredmény és kiértékelés:** Közérthető összefoglaló a javasolt korrekcióról, grafikus mért / cél / korrigált görbe. „Mindkettő” mérésnél a bal–jobb érkezési különbség (ms és cm) és az eltérő polaritás jelzése (bekötési hiba) – ez tájékoztatás, a késleltetést a program nem állítja be magától (egyetlen mérésből a hangkártya saját késése miatt nem is lehetne).
5. **Alkalmazás és teszt:** Egy kattintásos érvénybe léptetés, egygombos A/B összehasonlítás a natív és korrigált hangzás között, egygombos visszavonás és mentés profilként.

### 1.4. Új képességek
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
The room correction core contains zero proprietary or licensed third-party code (clean-room design; no Cavern code inspected or used). Algorithms are built strictly from public academic literature and mathematical specs:
1. **Farina, Angelo (2000):** *"Simultaneous Measurement of Impulse Response and Distortion with a Swept-Sine Technique"*, 108th AES Convention.
2. **Linkwitz, Siegfried & Riley, Russ (1976):** *"Active Crossover Networks for Noncoincident Drivers"*, JAES.
3. **Müller, Swen & Massarani, Paulo (2001):** *"Transfer-Function Measurement with Sweeps"*, JAES.
4. **Equalizer APO Specification:** Official syntax and file format.
5. **CamillaDSP Specification:** Official YAML configuration structure.

### 2.3. The 5 Wizard Steps
1. **Microphone & Calibration:** Select microphone, load optional calibration file (.cal, .txt, .frd, .csv) with log-frequency interpolation, choose channel.
2. **Level Check:** Quiet test tone to verify background noise and mic gain. Standard error codes: `NO_INPUT_DEVICE`, `MIC_TOO_QUIET`, `MIC_CLIPPING`, `NOISY_ROOM`.
3. **Measurement:** Synchronized logarithmic sweep and recording, progress bar, emergency Stop button, multi-point measurement with energy power averaging.
4. **Results:** Plain-language summary, time delay and polarity detection, visual before/after frequency response curve.
5. **Apply & Test:** One-click apply, instantaneous A/B comparison, undo, and save to profile list.
