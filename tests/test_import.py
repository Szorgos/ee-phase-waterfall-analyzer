#!/usr/bin/env python3
"""
Automatikus teszt az import-mintafájlok és a VART-EREDMENYEK.md alapján.
Futtatás: python tests/test_import.py
"""
import sys
import os
import wave
import struct

# Hozzáadjuk a gyökeret az elérési úthoz
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from src.import_export import (
    parse_apo_file, parse_csv_points, parse_camilladsp_yaml,
    reduce_bands, export_apo_config
)

DATA_DIR = os.path.join(ROOT, "tests", "data", "import")

def run_tests():
    passed = 0
    total = 0

    def assert_eq(actual, expected, msg=""):
        nonlocal passed, total
        total += 1
        if actual != expected:
            raise AssertionError(f"FAIL: {msg} -> Elvárt: {expected}, Kapott: {actual}")
        passed += 1

    def assert_approx(actual, expected, tol=0.15, msg=""):
        nonlocal passed, total
        total += 1
        if abs(actual - expected) > tol:
            raise AssertionError(f"FAIL: {msg} -> Elvárt: ~{expected} (tol {tol}), Kapott: {actual}")
        passed += 1

    print("=== Aqulify Import & Band Reducer Tesztek ===")

    # 1. 01-parametrikus.txt
    print("Teszt 1: 01-parametrikus.txt")
    p1 = parse_apo_file(os.path.join(DATA_DIR, "01-parametrikus.txt"))
    assert_approx(p1.preamp, -6.2, 0.01, "01 Preamp")
    assert_eq(len(p1.bands_l), 4, "01 4 sáv L-en")
    assert_eq(any("OFF" in s[1] for s in p1.skipped), True, "01 OFF sor megnevezve kimaradt")

    # 2. 02-szoba-bal-jobb.txt
    print("Teszt 2: 02-szoba-bal-jobb.txt")
    p2 = parse_apo_file(os.path.join(DATA_DIR, "02-szoba-bal-jobb.txt"))
    assert_approx(p2.preamp, -5.0, 0.01, "02 Preamp")
    assert_eq(p2.eq_split, True, "02 Külön bal/jobb EQ aktív")
    assert_approx(p2.delay_l_ms, 0.35, 0.01, "02 L delay")
    assert_approx(p2.delay_r_ms, 0.0, 0.01, "02 R delay")
    assert_eq(len(p2.bands_l), 4, "02 L sávok száma (3 PK + 1 HSC)")
    assert_eq(len(p2.bands_r), 3, "02 R sávok száma (2 PK + 1 HSC)")
    assert_approx(p2.bands_l[0].freq, 42.0, 0.1, "02 L band 0 freq")
    assert_approx(p2.bands_r[0].freq, 45.0, 0.1, "02 R band 0 freq")

    # 3. 03-azonos-bal-jobb.txt
    print("Teszt 3: 03-azonos-bal-jobb.txt")
    p3 = parse_apo_file(os.path.join(DATA_DIR, "03-azonos-bal-jobb.txt"))
    assert_eq(len(p3.bands_l), 2, "03 L sávok száma")
    assert_eq(len(p3.bands_r), 2, "03 R sávok száma")
    assert_approx(p3.bands_l[0].freq, 60.0, 0.1, "03 PK 60 Hz")
    assert_approx(p3.bands_l[1].freq, 120.0, 0.1, "03 LS 120 Hz")
    assert_approx(p3.bands_l[1].gain, 2.0, 0.1, "03 LS +2 dB")

    # 4. 04-meredekseg.txt
    print("Teszt 4: 04-meredekseg.txt")
    p4 = parse_apo_file(os.path.join(DATA_DIR, "04-meredekseg.txt"))
    assert_eq(len(p4.bands_l), 5, "04 Mind az 5 sáv betöltve")
    # HP 25, LP 18k, LS 12dB 80, HS 6dB 6k, HP 24dB 30
    assert_approx(p4.bands_l[0].freq, 25.0, 0.1, "04 HP 25")
    assert_approx(p4.bands_l[1].freq, 18000.0, 1.0, "04 LP 18k")
    assert_approx(p4.bands_l[2].freq, 80.0, 0.1, "04 LS 80")
    assert_approx(p4.bands_l[3].freq, 6000.0, 1.0, "04 HS 6k")
    assert_approx(p4.bands_l[4].freq, 30.0, 0.1, "04 HP 30")

    # 5. 05-grafikus.txt + Sávcsökkentő
    print("Teszt 5: 05-grafikus.txt + Sávcsökkentő")
    p5 = parse_apo_file(os.path.join(DATA_DIR, "05-grafikus.txt"))
    assert_eq(len(p5.points) >= 50, True, "05 GraphicEQ pontok kinyerve")
    # Redukálás max 10 sávra
    red_bands, peak_err, rms_err = reduce_bands(p5.points, max_bands=10)
    assert_eq(len(red_bands) <= 10, True, "05 Redukált sávok száma <= 10")
    assert_eq(peak_err < 2.5, True, f"05 Csúcs eltérés kicsi ({peak_err:.2f} dB)")
    assert_eq(rms_err < 1.0, True, f"05 RMS eltérés kicsi ({rms_err:.2f} dB)")
    print(f"   -> Savcsokkento eredmeny: {len(red_bands)} sav, csucs hiba: {peak_err:.2f} dB, RMS: {rms_err:.2f} dB")

    # 6. 06-include/fo.txt
    print("Teszt 6: 06-include/fo.txt")
    p6_fo = parse_apo_file(os.path.join(DATA_DIR, "06-include", "fo.txt"))
    assert_approx(p6_fo.preamp, -3.0, 0.01, "06 Preamp -3 dB")
    assert_eq(len(p6_fo.bands_l), 2, "06 2 sáv a két alfájlból")

    # 7. 06-include/kilepo.txt (Path traversal védelem)
    print("Teszt 7: 06-include/kilepo.txt")
    p6_kil = parse_apo_file(os.path.join(DATA_DIR, "06-include", "kilepo.txt"))
    assert_eq(len(p6_kil.bands_l), 0, "06 Kilepo: nincs betöltött sáv")
    escape_skips = [s for s in p6_kil.skipped if "kilép" in s[1] or "Tiltott" in s[1]]
    assert_eq(len(escape_skips), 3, "06 Mindhárom kilepo Include elutasítva")

    # 8. 06-include/kor-a.txt (Ciklusvédelem)
    print("Teszt 8: 06-include/kor-a.txt")
    p6_kor = parse_apo_file(os.path.join(DATA_DIR, "06-include", "kor-a.txt"))
    assert_eq(len(p6_kor.bands_l), 1, "06 Ciklus: PK 100 betöltve")
    cycle_skips = [s for s in p6_kor.skipped if "Körkörös" in s[1]]
    assert_eq(len(cycle_skips) >= 1, True, "06 Körkörös hivatkozás felismerve")

    # 9. 07-konvolucio/eq.txt
    print("Teszt 9: 07-konvolucio/eq.txt")
    p7 = parse_apo_file(os.path.join(DATA_DIR, "07-konvolucio", "eq.txt"))
    assert_approx(p7.preamp, -2.0, 0.01, "07 Preamp -2 dB")
    assert_eq(os.path.basename(p7.conv_l), "ir-bal.wav", "07 Bal IR fájl")
    assert_eq(os.path.basename(p7.conv_r), "ir-jobb.wav", "07 Jobb IR fájl")

    # 10. 07-konvolucio/hianyzo.txt
    print("Teszt 10: 07-konvolucio/hianyzo.txt")
    p7_hianyz = parse_apo_file(os.path.join(DATA_DIR, "07-konvolucio", "hianyzo.txt"))
    missing_skips = [s for s in p7_hianyz.skipped if "nem található" in s[1]]
    assert_eq(len(missing_skips) >= 1, True, "07 Hiányzó fájl hibaüzenet")

    # 11. 08-ir WAV fájlok
    print("Teszt 11: 08-ir WAV formátumok")
    def check_wav(name):
        wpath = os.path.join(DATA_DIR, "08-ir", name)
        with open(wpath, "rb") as f:
            riff = f.read(12)
            assert riff[:4] == b"RIFF" and riff[8:12] == b"WAVE"
            fmt_chunk = f.read(8)
            assert fmt_chunk[:4] == b"fmt "
            fmt_size = struct.unpack("<I", fmt_chunk[4:8])[0]
            fmt_data = f.read(fmt_size)
            wFormatTag, nChannels, nSamplesPerSec, nAvgBytesPerSec, nBlockAlign, wBitsPerSample = struct.unpack("<HHIIHH", fmt_data[:16])
            return nChannels, nSamplesPerSec, wBitsPerSample, wFormatTag
            
    ch, sr, bits, tag = check_wav("sztereo-pcm16.wav")
    assert_eq((ch, sr, bits, tag), (2, 48000, 16, 1), "08 PCM16 WAV 2ch 48k")
    ch, sr, bits, tag = check_wav("sztereo-pcm24.wav")
    assert_eq((ch, sr, bits, tag), (2, 48000, 24, 1), "08 PCM24 WAV 2ch 48k")
    ch, sr, bits, tag = check_wav("sztereo-44k1.wav")
    assert_eq((ch, sr), (2, 44100), "08 44.1k WAV")
    ch, sr, bits, tag = check_wav("true-stereo-4ch.wav")
    assert_eq((ch, sr), (4, 48000), "08 True stereo 4 csatorna")

    # 12. 09-copy.txt
    print("Teszt 12: 09-copy.txt")
    p9_copy = parse_apo_file(os.path.join(DATA_DIR, "09-copy.txt"))
    assert_eq(p9_copy.eq_split, True, "09 Copy: szétválasztva")
    assert_eq(len(p9_copy.bands_l), 1, "09 Copy: PK 100 mindkét csatornán")
    assert_eq(len(p9_copy.bands_r), 1, "09 Copy: PK 100 mindkét csatornán")

    # 13. 09-copy-bonyolult.txt
    print("Teszt 13: 09-copy-bonyolult.txt")
    p9_bony = parse_apo_file(os.path.join(DATA_DIR, "09-copy-bonyolult.txt"))
    assert_eq(any("nem támogatott" in s[1] for s in p9_bony.skipped), True, "09 Bonyolult keverés megnevezve kimaradt")
    assert_eq(len(p9_bony.bands_l), 1, "09 PK 100 betöltődött")

    # 14. 09-egyeb-sorok.txt
    print("Teszt 14: 09-egyeb-sorok.txt")
    p9_egy = parse_apo_file(os.path.join(DATA_DIR, "09-egyeb-sorok.txt"))
    assert_eq(len(p9_egy.bands_l), 1, "09 Egy sáv betöltve (PK 1000)")
    assert_eq(p9_egy.bands_l[0].freq, 1000.0, "09 PK 1000")
    assert_eq(len(p9_egy.skipped) >= 4, True, "09 Device, Stage, Eval, XYZ, értelmezhetetlen sor megnevezve kimaradt")

    # 15. 10-meres.csv
    print("Teszt 15: 10-meres.csv")
    pts10_m = parse_csv_points(os.path.join(DATA_DIR, "10-meres.csv"))
    assert_eq(len(pts10_m) > 100, True, "10 Mérés: > 100 pont beolvasva")
    # Csúcs keresése
    peak_pt = max(pts10_m, key=lambda x: x[1])
    assert_approx(peak_pt[0], 45.0, 5.0, "10 Mérés csúcs ~45 Hz")
    # A mérés alapszintje 75 dB SPL, a csúcs 81 dB SPL -> +6 dB kiemelés
    assert_approx(peak_pt[1] - 75.0, 6.0, 0.5, "10 Mérés csúcs ~+6 dB a 75 dB alaphoz képest")

    # 16. 10-mikrofon-kalibracio.txt
    print("Teszt 16: 10-mikrofon-kalibracio.txt")
    pts10_k = parse_csv_points(os.path.join(DATA_DIR, "10-mikrofon-kalibracio.txt"))
    assert_eq(len(pts10_k) >= 25, True, "10 Kalibráció pontok")
    assert_approx(pts10_k[0][1], 0.8, 0.05, "10 Kalibráció 20 Hz +0.8 dB")
    assert_approx(pts10_k[-1][1], -1.2, 0.05, "10 Kalibráció 16 kHz -1.2 dB")

    # 17. 10-celgorbe.txt
    print("Teszt 17: 10-celgorbe.txt")
    pts10_c = parse_csv_points(os.path.join(DATA_DIR, "10-celgorbe.txt"))
    assert_eq(len(pts10_c), 6, "10 Célgörbe 6 pont")
    assert_approx(pts10_c[0][1], 4.0, 0.1, "10 Célgörbe 20 Hz +4 dB")
    assert_approx(pts10_c[-1][1], -4.0, 0.1, "10 Célgörbe 20 kHz -4 dB")

    # 18. 11-camilladsp.yml
    print("Teszt 18: 11-camilladsp.yml")
    p11 = parse_camilladsp_yaml(os.path.join(DATA_DIR, "11-camilladsp.yml"))
    assert_approx(p11.preamp, -5.0, 0.01, "11 CamillaDSP Preamp -5 dB")
    assert_eq(p11.eq_split, True, "11 CamillaDSP Split L/R")
    assert_approx(p11.delay_l_ms, 0.35, 0.01, "11 CamillaDSP Delay L")
    assert_eq(len(p11.bands_l), 3, "11 CamillaDSP L sávok (HP 25, PK 44, HS 8000)")
    assert_eq(len(p11.bands_r), 3, "11 CamillaDSP R sávok (HP 25, PK 47, HS 8000)")
    assert_eq(p11.conv_l, "", "11 room_l nincs a pipeline-ban -> nem került be")
    assert_eq(any("DiffEq" in s[1] for s in p11.skipped), True, "11 DiffEq nem támogatott -> megnevezve kimaradt")

    # 19. Export -> Visszaimport konzisztencia
    print("Teszt 19: Export -> Visszaimport konzisztencia (02, 03, 04, 07, 11)")
    for src_cfg, tag in [(p2, "02"), (p3, "03"), (p4, "04"), (p7, "07"), (p11, "11")]:
        exported_text = export_apo_config(src_cfg)
        temp_path = os.path.join(DATA_DIR, f"temp_reimport_{tag}.txt")
        with open(temp_path, "w", encoding="utf-8") as f:
            f.write(exported_text)
        reimported = parse_apo_file(temp_path, DATA_DIR)
        os.remove(temp_path)
        
        assert_approx(reimported.preamp, src_cfg.preamp, 0.1, f"Reimport {tag} Preamp")
        assert_eq(len(reimported.bands_l), len(src_cfg.bands_l), f"Reimport {tag} bands_l count")
        if src_cfg.eq_split:
            assert_eq(len(reimported.bands_r), len(src_cfg.bands_r), f"Reimport {tag} bands_r count")
            assert_approx(reimported.delay_l_ms, src_cfg.delay_l_ms, 0.05, f"Reimport {tag} delay L")
            assert_approx(reimported.delay_r_ms, src_cfg.delay_r_ms, 0.05, f"Reimport {tag} delay R")

    print(f"\n==========================================")
    print(f"SIKER: Minden teszt lefutott! ({passed}/{total} ellenőrzés sikeres)")
    print(f"==========================================")
    return True

if __name__ == "__main__":
    try:
        run_tests()
    except AssertionError as e:
        print(f"\nHIBA: {e}", file=sys.stderr)
        sys.exit(1)
