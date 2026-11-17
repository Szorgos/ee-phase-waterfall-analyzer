#!/usr/bin/env python3
"""
Szobamérő mag tesztjei Pythonban (log sweep, dekonvolúció, simítás, szintetikus szoba, auto-EQ).
Futtatás: python tests/test_room.py
"""
import sys
import os
import math

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

def generate_log_sweep(f1, f2, dur, sr, amp=0.1, fade_ms=20.0):
    n = int(round(dur * sr))
    sweep = [0.0] * n
    w1 = 2.0 * math.pi * f1
    w2 = 2.0 * math.pi * f2
    log_ratio = math.log(w2 / w1)

    for i in range(n):
        t = i / sr
        phase = (w1 * dur / log_ratio) * (math.exp(t / dur * log_ratio) - 1.0)
        sweep[i] = amp * math.sin(phase)

    fade_n = int(round(fade_ms * 0.001 * sr))
    for i in range(min(fade_n, n // 2)):
        w = 0.5 * (1.0 - math.cos(math.pi * i / fade_n))
        sweep[i] *= w
        sweep[n - 1 - i] *= w
    return sweep

def generate_inverse_filter(f1, f2, dur, sr, amp=0.1, fade_ms=20.0):
    sweep = generate_log_sweep(f1, f2, dur, sr, 1.0, fade_ms)
    n = len(sweep)
    inv = [0.0] * n
    log_ratio = math.log(f2 / f1)

    for i in range(n):
        t = i / sr
        env = math.exp(-t / dur * log_ratio)
        inv[n - 1 - i] = sweep[i] * env

    energy = sum(sweep[i] * inv[i] for i in range(n))
    scale = 1.0 / (energy * amp) if abs(energy) > 1e-12 else 1.0
    return [v * scale for v in inv]

def deconvolve(recorded, ref_sweep, sr):
    rec_n = len(recorded)
    ref_n = len(ref_sweep)
    ir_len = rec_n - ref_n + 1 if rec_n >= ref_n else 0
    ir = [0.0] * ir_len

    ref_energy = sum(x * x for x in ref_sweep)
    inv_energy = 1.0 / ref_energy if ref_energy > 1e-12 else 1.0

    for i in range(ir_len):
        s = 0.0
        for k in range(ref_n):
            s += recorded[i + k] * ref_sweep[k]
        ir[i] = s * inv_energy

    peak_idx = 0
    peak_val = 0.0
    for i in range(ir_len):
        if abs(ir[i]) > abs(peak_val):
            peak_val = ir[i]
            peak_idx = i

    delay_ms = (peak_idx / sr) * 1000.0
    inverted = (peak_val < 0.0)
    return ir, peak_idx, peak_val, delay_ms, inverted

def run_room_tests():
    print("=== Aqulify Szobamero Mag Tesztek ===")
    sr = 48000
    dur = 0.1 # 0.1 masodperc
    f1, f2 = 20.0, 20000.0

    # 1. Sweep es inverz szuro
    print("Teszt 1: Log sine sweep es inverz szuro")
    sw = generate_log_sweep(f1, f2, dur, sr, amp=0.2)
    inv = generate_inverse_filter(f1, f2, dur, sr, amp=0.2)
    assert len(sw) == len(inv)

    dot = sum(sw[i] * inv[i] for i in range(len(sw)))
    diff = abs(dot - 1.0)
    assert diff < 0.05, f"Dirac csucs elteres tul nagy: {diff}"
    print(f"   -> Dirac csucs: {dot:.4f} (elteres: {diff:.4f} < 0.05)")

    # 2. Szintetikus szoba (15 ms kesleltetes, forditott polaritas)
    print("Teszt 2: Szintetikus szoba, kesleltetes es polaritas")
    delay_ms_target = 15.0
    delay_samples = int(round(delay_ms_target * 0.001 * sr))
    recorded = [0.0] * (len(sw) + delay_samples + 2000)

    # Elhelyezzuk a forditott polaritasu sweepet
    for i in range(len(sw)):
        recorded[i + delay_samples] = -1.0 * sw[i]

    ir, peak_idx, peak_val, delay_ms, inverted = deconvolve(recorded, sw, sr)
    measured_diff = abs(delay_ms - delay_ms_target)
    assert measured_diff < 0.05, f"Kesleltetes hiba: mert {delay_ms:.2f} ms vs vart {delay_ms_target} ms"
    assert inverted, "Forditott polaritas nem lett felismerve"
    assert peak_val < 0.0, f"Csucs elojele nem negativ: {peak_val}"
    assert abs(peak_val - (-1.0)) < 0.01, f"Csucs erteke nem -1.0: {peak_val}"
    print(f"   -> Mert kesleltetes: {delay_ms:.2f} ms (hiba: {measured_diff:.3f} ms < 0.05 ms)")
    print(f"   -> Forditott polaritas: felismerve (csucs: {peak_val:.4f})")

    # 3. Auto-EQ celgorbe illesztes es gain limiter (+3 dB max emeles)
    print("Teszt 3: Auto-EQ celgorbe es biztonsagi gain limiter (+3 dB)")
    from src.import_export import reduce_bands
    # Szintetikus szobai rezonancia: 45 Hz-en +6 dB emeles, 120 Hz-en -6 dB dip
    pts = []
    for i in range(60):
        f = 20.0 * (1000.0 ** (i / 59.0))
        w45 = f / 45.0
        peak45 = 6.0 / (1.0 + 25.0 * (w45 - 1.0 / w45) ** 2)
        w120 = f / 120.0
        dip120 = -6.0 / (1.0 + 9.0 * (w120 - 1.0 / w120) ** 2)
        room_resp = peak45 + dip120
        # Korrekcio: az ellenkezoje, DE a dips korlatozva max +3 dB-re!
        needed_corr = -room_resp
        if needed_corr > 3.0:
            needed_corr = 3.0
        pts.append((f, needed_corr))

    red_bands, peak_err, rms_err = reduce_bands(pts, max_bands=10, max_boost=3.0)
    assert len(red_bands) <= 10
    # Ellenorizzuk, hogy nincs +3 dB-nel nagyobb boost
    for b in red_bands:
        assert b.gain <= 3.05, f"Tullepte a +3 dB biztonsagi korlatot: {b.gain}"
    # Ellenorizzuk, hogy a 45 Hz-es szobamodust csillapitja
    found_mode = any(abs(b.freq - 45.0) < 8.0 and b.gain < -3.0 for b in red_bands)
    assert found_mode, "Nem talalta meg a 45 Hz-es szobamodus csillapitasat"
    print(f"   -> Redukalt savok: {len(red_bands)}, peak err: {peak_err:.2f} dB, RMS: {rms_err:.2f} dB")
    print(f"   -> 45 Hz modus visszavagva, max boost <= 3 dB biztonsagosan betartva")

    print("\n==========================================")
    print("SIKER: Minden szobamero mag teszt sikeres!")
    print("==========================================")
    return True

if __name__ == "__main__":
    run_room_tests()
