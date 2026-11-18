# Felmintavételezés a beépített lejátszóban

A felmintavételezés (upsampling) a beépített zenelejátszóban a dekódolt PCM hanganyag mintavételi frekvenciáját lejátszás előtt magasabb, a kimeneti eszköz (DAC) által támogatott mintavételre emeli (például 44,1 kHz → 88,2 kHz vagy 176,4 kHz).

Alapértelmezés szerint **ki van kapcsolva**: kikapcsolt állapotban a hang a fájl eredeti mintavételén, bitpontosan jut a kimenetre.

## Mikor van értelme?

A modern DAC-ok túlnyomó többsége belül is felmintavételezi a beérkező PCM-jelet saját beépített digitális szűrőjével. Szoftveres felmintavételezéssel a szűrés egy része vagy egésze a számítógépre helyezhető át, ahol pontosabb és szabadon hangolható (lineáris, köztes vagy minimumfázisú) szűrő választható.

- A hallható különbség erősen **DAC-függő** és szakmailag is vitatott.
- Csak akkor van valódi hatása, ha a hangrendszer ténylegesen a megemelt mintavételi frekvencián továbbítja a jelet a DAC-nak (lásd lentebb a rendszer-visszaalakításról szóló részt).

## Hol állítható be?

A felső sáv **Kimenet** menüjében, a **Hangkimenet (haladó) → Beépített lejátszó → Felmintavételezés (beépített lejátszó)** blokkban.

### Szabályok (célmintavétel)

A lejátszó **lefelé soha nem alakít**, és mindig figyelembe veszi a kimenet által támogatott mintavételi frekvenciákat:

- **Kikapcsolva** – nincs átalakítás, eredeti bitpontos kimenet.
- **×2** – a forrás kétszerese (pl. 44,1 → 88,2 kHz, 48 → 96 kHz, 96 → 192 kHz).
- **Legnagyobb ×2ⁿ** – a forrás legnagyobb, a DAC által támogatott kettő-hatvány többszöröse (44,1-es család a 44,1-es családban marad, 48-as a 48-asban).
- **Kimenet maximuma** – a kimenet által támogatott legmagasabb mintavételi frekvencia.
- **Egyéni táblázat** – forrásmintavételenként külön megadható célfrekvencia (ha a cél nem támogatott vagy kisebb a forrásnál, a fájl eredeti frekvencián szól).

### Ajánlott beállítási példák

- **Kiegyensúlyozott kiindulópont:** szabály **×2**, előbeállítás **Köztes (ajánlott)** (fázis: 25, 28 bit, 95 % sávszélesség, 100 % záró sáv), fejtér **−1 dB**.
- **Előrezgés-mentes hangzás (minimumfázis):** szabály **×2** vagy **Legnagyobb ×2ⁿ**, előbeállítás **Minimumfázis** (fázis: 0), fejtér **−1 dB**.
- **Szimmetrikus fázisválasz:** szabály **×2**, előbeállítás **Lineáris fázis** (fázis: 50), fejtér **−1 dB**.
- **Referencia (a legpontosabb):** előbeállítás **Referencia** (lineáris fázis, 32 bit, 99,7 % sávszélesség, 100 % záró sáv). A szűrő 44,1 kHz-es forrásnál kb. 21,99 kHz-ig sík; az ára a hosszabb szűrő: több számítás, és a lineáris fázis miatt hosszabb előcsengés. Gyengébb gépen a terhelés-tanácsadó szól, ha nem bírja.

### Haladó szűrőbeállítások és fejtér

- **Fázis (0–50):** a `libsoxr` saját skálája (`0` = minimumfázis, `25` = köztes, `50` = lineáris fázis). 50 fölötti (maximumfázisú) érték nem választható.
- **Sávszélesség (80,0 – 99,7 %):** az áteresztő sáv vége a forrás Nyquist-frekvenciájához viszonyítva (alap: 95 %).
- **Záró sáv kezdete (100 – 150 %):** 100 % esetén a szűrő a Nyquist-frekvenciánál teljesen zár; 100 % fölött lankásabb lefutás érhető el némi szándékos tükörkép (aliasing) árán. A sávszélességnek mindig kisebbnek kell lennie a záró sáv kezdeténél.
- **Pontosság:** 20 / 24 / 28 / 32 bit. A szűrőhosszt és a belső fokozatot a motor automatikusan számolja a fenti értékekből.
- **Fejtér (0 / −1 / −3 / −6 dB):** a mintavételek közti csúcsok (intersample peaks) miatt a felmintavételezett jel 0 dBFS fölé mehet. Az alapértelmezett **−1 dB** biztonsági tartalékot ad; ha mégis keletkezik 1,0 fölötti minta, az állapotsor kiírja a túlvezérelt minták pontos számát.

## Mi a teendő, ha a rendszer visszaalakít?

Az állapotsor mindig a **valódi** kimeneti mintavételt mutatja:

- Ha például 44,1 → 88,2 kHz a cél, de az állapotsor figyelmeztetést jelez (*„Figyelem: a rendszer 48 kHz frekvenciára alakítja vissza”*), akkor az operációs rendszer hangkeverője visszaalakítja a jelet, így a felmintavételezésnek nincs haszna.
- **Linux / Raspberry Pi:** a *Hangkimenet (haladó)* résznél válaszd a **Mintavétel → A forrást követi** beállítást, vagy kapcsold be a **Közvetlen ALSA-kimenet (kizárólagos mód)** opciót.
- **Windows:** kapcsold be a **Kizárólagos mód (WASAPI exclusive)** opciót (megosztott módban a Windows keverője a rendszer-mintavételre alakít).
- **macOS:** kapcsold be a kizárólagos (hog) módot, vagy állítsd a kimeneti eszközt a kívánt mintavételre.

## Korlátok

- **Csak a beépített lejátszóra hat:** külső alkalmazások hangjára és a rendszerszintű DSP-lánc bemenetére nem alkalmazódik.
- **DSD / DoP és 192 kHz fölötti forrás:** natív DSD (DoP) lejátszásnál, DSD-fájloknál és 192 kHz-nél magasabb mintavételű forrásoknál a felmintavételezés automatikusan kimarad (nincs DSD-re való felmintavételezés).
- **Szobamérés:** a szobakorrekciós mérőjel mindig a megkerülő úton, felmintavételezés nélkül szól.
- **Feldolgozás:** az átalakítás a dekóder-szálon, előre fut (a hangszálon nincs memóriafoglalás). Azonos forrásmintavételű egymást követő számoknál a lejátszás szünetmentesen, folytonos szűrőállapottal folytatódik.

## Licenc és forrásmegjelölés

A felmintavételezést a **libsoxr** (0.1.3) könyvtár végzi, **LGPL-2.1+** licenc alatt. A forrás és a licencszöveg megtalálható a `third_party/soxr` mappában és a telepítőcsomagok licencfájljai között.
