# Terv: EasyEffects plugin-paritás

**Állapot:** csak feljegyzés, a megvalósítás később jön.
**Felvéve:** 2026-10-20

## Cél

Az AetherAudio tudjon mindent, amit az EasyEffects: minden EasyEffects plugin legyen meg az AetherAudio DSP-láncában.

## Kötelező szabály minden új pluginra

- **Linuxon és Windowson is** el kell készülnie, egyszerre.
- A DSP kód közös, az `include/` mappába kerül (header-only, mint a meglévő `bass_enhancer.hpp`, `crossfeed.hpp`, `limiter.hpp`).
- A kezelőfelület közös: `gui_integrated_system.html`.
- A paraméterek a `param_block.hpp` / `param_apply.hpp` útján jutnak el a lánchoz (`aether_chain.hpp`), így a Linux PipeWire csomópont és a Windows EQ APO VST2 plugin ugyanazt kapja.
- Minden plugin legyen ki-be kapcsolható és megkerülhető (bypass), kattanás nélkül.

## Jelenlegi lánc (v1.4.0)

```
Preamp -> EQ (IIR / FIR: lineáris, köztes, minimálfázis) -> Bass Enhancer -> Crossfeed
       -> Convolver (partícionált FFT, IR) -> Limiter (true peak, oversampling)
```

## Az EasyEffects pluginjai

Jelölés: ✅ megvan, 🟡 részben megvan, ⬜ még nincs.

| # | EasyEffects plugin | Rövid leírás | AetherAudio |
|---|---|---|---|
| 1 | Autogain | Automatikus hangerő-kiegyenlítés a mért hangosság (LUFS, EBU R128) alapján. | ⬜ |
| 2 | Bass Enhancer | Felharmonikusokat ad a mélyekhez, hogy kis hangszórón is hallatszódjon a basszus. | ✅ `bass_enhancer.hpp` |
| 3 | Bass Loudness | Hangerőfüggő mély-kiemelés: halk hallgatásnál több basszus. | ⬜ |
| 4 | Compressor | Dinamikatömörítő (küszöb, arány, attack, release, knee, oldallánc). | ⬜ |
| 5 | Convolver | Impulzusválasz (IR) alapú konvolúció: térhatás, fejhallgató- vagy teremkorrekció. | ✅ `partitioned_convolver.hpp` |
| 6 | Crossfeed | Fejhallgatón a bal/jobb csatorna részleges áthallatása, természetesebb sztereó kép. | ✅ `crossfeed.hpp` |
| 7 | Crystalizer | Sávonkénti dinamika-kiterjesztés, „élénkebbé” teszi a tömörített zenét. | ⬜ |
| 8 | Deesser | Az éles sziszegő hangok (sz, s, cs) csillapítása. | ⬜ |
| 9 | Delay | Csatornánkénti késleltetés (ms-ban), pl. hangszóró-időzítéshez. | 🟡 csak a crossfeeden belül |
| 10 | Echo Canceller | Visszhangszűrés mikrofonhoz (főleg bemeneti oldal). | ⬜ |
| 11 | Equalizer | Parametrikus / grafikus EQ, sok sáv, AutoEQ profilok. | ✅ IIR + FIR, AutoEQ |
| 12 | Exciter | Felharmonikusokat ad a magasakhoz, „fényesebb” hang. | ⬜ |
| 13 | Filter | Egyszerű alul-, felül-, sáváteresztő és sávzáró szűrő. | 🟡 EQ-sávokkal megoldható |
| 14 | Gate | Zajkapu: a küszöb alatti jelet elnémítja vagy csillapítja. | ⬜ |
| 15 | Level Meter | Hangosságmérés (LUFS, true peak, loudness range). | 🟡 van spektrum/fázis analízis, LUFS nincs |
| 16 | Limiter | Csúcskorlátozó, a túlvezérlés ellen. | ✅ `true_peak_limiter.hpp` |
| 17 | Loudness | ISO 226 szerinti hangerő-kompenzáció (egyenlő hangosságú görbék). | ⬜ |
| 18 | Maximizer | Agresszív hangosító limiter, a jelet a plafonig emeli. | ⬜ |
| 19 | Multiband Compressor | Több frekvenciasávra bontott kompresszor. | ⬜ |
| 20 | Multiband Gate | Több frekvenciasávra bontott zajkapu / expander. | ⬜ |
| 21 | Noise Reduction | Gépi tanulásos zajszűrés (RNNoise), főleg mikrofonhoz. | ⬜ |
| 22 | Deep Noise Remover | DeepFilterNet alapú, erősebb gépi tanulásos zajszűrés. | ⬜ |
| 23 | Pitch | Hangmagasság-eltolás tempóváltoztatás nélkül. | ⬜ |
| 24 | Reverberation | Mesterséges zengető (teremhatás). | ⬜ |
| 25 | Speech Processor | Beszédjavító csomag mikrofonhoz (zajszűrés, AGC, visszhangszűrés). | ⬜ |
| 26 | Stereo Tools | Sztereó szélesség, balansz, mono, csatornacsere, fázisfordítás, M/S. | ⬜ |

Összesen 26 plugin, ebből 5 megvan, 3 részben.

## Javasolt sorrend (később pontosítandó)

A kimeneti (lejátszási) lánc pluginjai előbb jönnek, mert az AetherAudio jelenleg kimenetet dolgoz fel.

1. **Könnyű, sokat hozó:** Stereo Tools, Delay (önálló), Filter (önálló), Level Meter (LUFS), Autogain.
2. **Dinamika:** Compressor, Gate, Deesser, Maximizer; utána a Multiband Compressor és a Multiband Gate (közös sávszétválasztóval).
3. **Hangszín:** Loudness, Bass Loudness, Exciter, Crystalizer.
4. **Effektek:** Reverberation, Pitch.
5. **Bemeneti (mikrofon) oldal:** Echo Canceller, Noise Reduction, Deep Noise Remover, Speech Processor. Ehhez mikrofon-bemeneti lánc kell mindkét platformon (Windowson EQ APO capture eszközre).

## Nyitott kérdések

- A plugin-sorrend legyen-e szabadon átrendezhető a felületen (mint az EasyEffectsben), vagy fix lánc?
- A gépi tanulásos zajszűrők (RNNoise, DeepFilterNet) külső függőséget hoznak: kell-e, és hogyan csomagoljuk Windowsra?
- Mennyi CPU-t engedünk meg a teljes láncnak a valós idejű szálon?
