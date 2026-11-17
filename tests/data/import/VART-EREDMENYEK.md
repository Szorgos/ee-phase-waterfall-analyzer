# Import-mintafájlok és a várt eredmények

**Kitalált (szintetikus) fájlok**, a nyilvánosan leírt formátumok szerint; egyik sem egy konkrét program valódi
kimenete. Előállítás: `python -I generate.py` (újrafuttatható, ugyanazt adja). Valódi mintákat a tulajdonos később
tesz mellé, ha lesz.

Jelölés: „MA” = a `b33ce08` feldolgozó mért viselkedése (Claude, böngészőben, 2026-11-16); „CÉL” = amit a
16d átadó után kell tudnia. A tesztek a CÉL-t ellenőrzik, a fájlokat a tesztből olvasva.

| Fájl | MA | CÉL |
|---|---|---|
| `01-parametrikus.txt` | 4 sáv, preamp −6,2; az OFF sor megnevezve kimarad | ugyanez (regresszió) |
| `02-szoba-bal-jobb.txt` | 1 sáv (a közös HSC), preamp −5; L / R szűrők és a `Delay:` megnevezve kimarad | **külön bal / jobb EQ**: L = PK 42 / 87 / 210 + HSC 8000; R = PK 45 / 160 + HSC 8000; késleltetés L 0,35 ms (48 kHz-en 16,8 → a kerekítést írd le), R 0; preamp −5 |
| `03-azonos-bal-jobb.txt` | 1 sáv (PK 60), az `LS 6dB` megnevezve kimarad | 2 sáv mindkét csatornán: PK 60 Hz −6 dB Q 4 és 6 dB/okt-os alsó polc 120 Hz +2 dB |
| `04-meredekseg.txt` | 2 sáv, 3 megnevezve kimarad | mind az 5: HP 25 (12 dB/okt, Q 0,707), LP 18 kHz Q 0,71, LS 12 dB/okt 80 Hz +3, HS 6 dB/okt 6 kHz −2, HP 24 dB/okt 30 Hz (két másodfokú kaszkád) |
| `05-grafikus.txt` | felismeri, nem tölti be | illesztett parametrikus sávok (≤ 32, ajánlott ≤ 10), a cél és az eredmény eltérése kijelezve (dB, 20 Hz–20 kHz) |
| `06-include/fo.txt` | — | preamp −3; PK 50 −5 Q 3 és HSC 9000 −2 a két alfájlból |
| `06-include/kilepo.txt` | — | mindhárom `Include:` **elutasítva** (kilép a mappából / abszolút út), megnevezve; fájlt nem olvas |
| `06-include/kor-a.txt` | — | körkörös hivatkozás felismerve, megnevezve; a PK 100 betöltődik; nincs végtelen ciklus |
| `07-konvolucio/eq.txt` | — | bal: `ir-bal.wav` (0. minta, 0,5); jobb: `ir-jobb.wav` (48. minta = 1 ms, 1,0); preamp −2 |
| `07-konvolucio/hianyzo.txt` | — | „a fájl nem található: nincs-ilyen.wav”, semmi más nem változik |
| `08-ir/sztereo-float.wav` | 2 cs., csúcs L 0 / R 10 (−1) | ugyanez; a jobb csatorna polaritása fordított |
| `08-ir/sztereo-pcm16.wav`, `-pcm24.wav`, `-extensible.wav` | helyes | ugyanez |
| `08-ir/sztereo-44k1.wav` | 44,1 kHz | a motor újramintavételez; a csúcs helye 48 kHz-en arányos |
| `08-ir/true-stereo-4ch.wav` | 4 cs.: elutasítja (b33ce08) | **4 ágú konvolúció**: LL és RR Dirac 0-n, LR és RL 0,3 a 20. mintán (áthallás) |
| `08-ir/mono-bal.wav` + `mono-jobb.wav` | egyszerre egy | együtt kiválasztva: bal = 0. minta, jobb = 24. minta |
| `08-ir/harom-csatorna.wav` | elutasítja | elutasítja, érthető üzenettel |
| `08-ir/serult.wav` | nem olvasható | nem olvasható, érthető üzenettel, összeomlás nélkül |
| `09-copy.txt` | — | bal ↔ jobb csere, majd PK 100 mindkét csatornán |
| `09-copy-bonyolult.txt` | — | a `Copy:` sor megnevezve kimarad (keverés nem támogatott); a PK 100 betöltődik |
| `09-egyeb-sorok.txt` | 1 sáv; Device / Stage / Eval megnevezve, az XYZ típus és az értelmezhetetlen sor is | ugyanez |
| `10-meres.csv` | — | mérésként betöltve (1/24 okt. pontok, fázissal); a csúcs ~45 Hz-en +6 dB |
| `10-mikrofon-kalibracio.txt` | — | kalibrációként: az első (idézőjeles) sor kimarad, 20–30 Hz +0,8 dB, 15 kHz felett −1,2 dB |
| `10-celgorbe.txt` | — | célgörbeként (a `*` megjegyzés kimarad), a pontok között a frekvencia logaritmusa szerint interpolál |
| `11-camilladsp.yml` | — | bal: −5 dB, HP 25, PK 44 −7 Q 5, késleltetés 0,35 ms, HS 8000 −1,5; jobb: −5 dB, HP 25, PK 47 −6 Q 5, HS 8000 −1,5; a `room_l` (Conv) nincs a láncban → nem kerül be; a `DiffEq` megnevezve kimarad |

**Export–visszaimport:** a 02, 03, 04, 07 és 11 betöltése után az Aqulify exportja (szöveg + WAV) visszaolvasva
ugyanazt a beállítást adja (sávonként frekvencia, erősítés, Q; késleltetés mintára; IR mintára).
