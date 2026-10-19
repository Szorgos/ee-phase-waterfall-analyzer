# EasyEffects Valós Idejű Fázis- és Akusztikai Analizátor Plugin Tervezet
**Projekt munkacím:** `ee-phase-waterfall-analyzer`  
**Cél:** Az EasyEffects meglévő alapszintű spektrum-vizualizációjának leváltása egy valós idejű, hardveresen gyorsított fázis- és idő-frekvencia (vízesés/wavelet) analizátor modullal.

---

## 1. Rendszerarchitektúra és Megközelítés

A rendszer alapelve a **hangfeldolgozás (PipeWire) és a grafikai megjelenítés (GPU) szigorú szétválasztása**, garantálva, hogy a grafikus terhelés vagy lebegőpontos számítás soha ne okozhasson hangkimaradást (xrun).

```
   [Tidal / Hanglejátszó]
             │
             ▼
    ┌─────────────────┐
    │ PipeWire Audio  │ ──► [Kimenet a DAC felé]
    └────────┬────────┘
             │ (zéró-másolású gyűrűpuffer / Ringbuffer)
             ▼
   ┌──────────────────────────────────────────────────┐
   │         EasyEffects Analizátor Modul             │
   │                                                  │
   │  ┌──────────────────────┐  ┌──────────────────┐  │
   │  │ 1. Mód: Analitikus   │  │ 2. Mód: Élő 3D   │  │
   │  │    Fázis / Késleltetés│  │    Vízesés       │  │
   │  │    (2D XY Görbe)     │  │    (GPU Shader)  │  │
   │  └──────────┬───────────┘  └─────────┬────────┘  │
   └─────────────┼────────────────────────┼───────────┘
                 │                        │
                 ▼                        ▼
       [GTK4 Cairo / Path]      [OpenGL / Vulkan Mesh]
                 └───────────┬────────────┘
                             ▼
               [EasyEffects GUI Megjelenítő]
```

---

## 2. Két Funkcionális Mód

### A) 2D Analitikus Fázis- és Csoportkésleltetés Görbe (XY Nézet)
* **Feladat:** Megmutatni, hogy az aktív pluginlánc (EQ, Convolver, Filterek) hol és mekkora fázistolást, illetve csoportkésleltetést okoz.
* **Működési elv:** 
  * Nem a valós zenei jelet elemzi, hanem közvetlenül az aktív szűrők átviteli függvényét ($H(z)$).
  * A sávok frekvencia, erősítés (gain) és jósági tényező (Q) paramétereiből zárt matematikai képlettel azonnal kiszámítja a csoportkésleltetést:
    $$\tau_g(\omega) = -\frac{d\phi(\omega)}{d\omega}$$
* **Számítási igény:** Gyakorlatilag **0% CPU / GPU** terhelés. Azonnal reagál az EQ potméterek húzására.
* **Megjelenítés:**
  * **X tengely:** Frekvencia (20 Hz – 20 kHz, logaritmikus).
  * **Y tengely:** Késleltetés ezredmásodpercben (ms) vagy fázisszög fokban (-180° és +180° között).
  * **Referenciavonal:** 0 ms (ideális lineáris fázis).

---

### B) 3D Valós Idejű Akusztikai Vízesés / Spektrogram (Waterfall Nézet)
* **Feladat:** Élő Tidal / rendszerhang zenelejátszás közben valós időben kimutatni:
  * A szobarezonanciák lecsengési idejét (búgó basszusok megmaradása az időben),
  * Az éles FIR szűrők által keltett előcsengést (pre-ringing a tranziensek előtt).
* **Működési elv:**
  * Többpályás STFT (Short-Time Fourier Transform) vagy Morlet-Wavelet transzformáció.
  * A mélyhangoknál (20–200 Hz) nagyobb időbeli felbontás a pontosságért, a magas hangoknál gyorsabb reakció.
* **Számítási és Grafikai Architektúra:**
  * **Adatátvitel:** Lock-free atomic ringbufferen keresztül másolódik a hangpuffer az elemző szálba.
  * **Renderelés:** `GtkGLArea` (OpenGL) vagy Vulkan háttérmotor.
  * A 3D háló magasságát és színezését (hőtérkép: kék $\to$ zöld $\to$ narancs $\to$ vörös) pixel shader végzi a GPU-n.
* **Megjelenítés:**
  * **X tengely:** Frekvencia (Hz).
  * **Y tengely:** Idő (lefelé gördülő idővonal az elmúlt 500–1000 ms-ról).
  * **Z tengely / Szín:** Hangnyomásszint (dBFS).

---

## 3. Technológiai Stack és Függőségek

| Komponens | Kiválasztott Technológia | Indoklás |
| :--- | :--- | :--- |
| **GUI Keretrendszer** | GTK4 / Libadwaita | Natív EasyEffects ökoszisztéma illeszkedés |
| **Audió bemenet** | PipeWire SPA Plugin / Ringbuffer | Zéró másolás, valós idejű biztonság |
| **Fázisszámítás** | Analitikus IIR/FIR Biquad Math | Mikroszekundumos válaszidő, minimális CPU |
| **Idő-frekvencia FFT** | PocketFFT / VkFFT / FFTW3 | Gyors SIMD vagy GPU-alapú spektrumszámítás |
| **3D Grafika** | Modern OpenGL (Core 3.3+) / Shader | 60 FPS folyamatos gördülés GPU terheléssel |

---

## 4. Fejlesztési Mérföldkövek (Roadmap)

1. **Mérföldkő: Matematikai Modell és Analitikus Fáziskalkulátor**
   * Biquad szűrők (Peaking, Low-shelf, High-pass stb.) átviteli függvényének leprogramozása.
   * Kaszkádosított (egymás után fűzött) szűrőlánc eredő fázismenete és csoportkésleltetése.
2. **Mérföldkő: 2D XY Vektoros Megjelenítő**
   * GTK4 felületen sima, dinamikus fázisgörbe kirajzolása valós idejű paraméterváltozásra.
3. **Mérföldkő: PipeWire Ringbuffer és GPU Shader Pipeline**
   * Zenei adat átvétele a háttérszálba a PipeWire-ból.
   * OpenGL felület integrálása a GTK ablakba (GtkGLArea).
4. **Mérföldkő: 3D Vízesés Háló és Hőtérkép Shader**
   * Folyamatosan gördülő 3D felület létrehozása állítható lecsengési időablakkal.
5. **Mérföldkő: EasyEffects Plugin Integráció**
   * UI kapcsoló az RTA (hagyományos spektrum), a 2D Fázis és a 3D Vízesés nézetek között.
