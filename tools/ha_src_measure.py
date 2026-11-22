#!/usr/bin/env python3
"""
Helyi SRC-mérőeszköz (V4) a tesztcsomagok (32 bites és 64 bites lebegőpontos)
objektív, reprodukálható előméréséhez és a változatok összehasonlításához.

Használat:
    python tools/ha_src_measure.py <bemeneti_mappa> <kimeneti_mappa> [--json eredmeny.json]
"""
import argparse
import json
import os
import struct
import sys
import numpy as np


def read_wav(path):
    """WAV beolvasása float64 tömbbe (16/24/32 int, 32/64 float, WAVE_FORMAT_EXTENSIBLE)."""
    with open(path, "rb") as fh:
        raw = fh.read()
    if len(raw) < 12 or raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise ValueError(f"Érvénytelen WAV fejléc: {path}")
    pos = 12
    fmt = ch = sr = bits = 0
    data = None
    while pos + 8 <= len(raw):
        cid = raw[pos:pos + 4]
        sz = struct.unpack("<I", raw[pos + 4:pos + 8])[0]
        body = raw[pos + 8:pos + 8 + sz]
        if cid == b"fmt " and len(body) >= 16:
            fmt, ch, sr, _, _, bits = struct.unpack("<HHIIHH", body[:16])
            if fmt == 0xFFFE and len(body) >= 26:
                fmt = struct.unpack("<H", body[24:26])[0]
        elif cid == b"data":
            data = body
        pos += 8 + sz + (sz & 1)
    if data is None or ch == 0 or sr == 0:
        raise ValueError(f"Hiányos WAV adatok: {path}")

    if fmt == 3:
        dtype = "<f4" if bits == 32 else "<f8"
        x = np.frombuffer(data, dtype=dtype).astype(np.float64)
    elif fmt == 1 and bits == 16:
        x = np.frombuffer(data, dtype="<i2").astype(np.float64) / 32768.0
    elif fmt == 1 and bits == 24:
        a = np.frombuffer(data, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = a[:, 0] | (a[:, 1] << 8) | (a[:, 2] << 16)
        x = np.where(v >= (1 << 23), v - (1 << 24), v).astype(np.float64) / float(1 << 23)
    elif fmt == 1 and bits == 32:
        x = np.frombuffer(data, dtype="<i4").astype(np.float64) / float(1 << 31)
    else:
        raise ValueError(f"Nem támogatott WAV formátum (fmt={fmt}, bits={bits}): {path}")

    frames = len(x) // ch
    x = x[:frames * ch].reshape(frames, ch)
    return x[:, 0], sr, fmt, bits, ch


def db(val):
    return 20.0 * np.log10(np.maximum(np.abs(val), 1e-30))


def detect_suffix(directory):
    """Felismeri a csomag fájlnév-utótagját (-64bitfloat vagy -32bitfloat)."""
    for cand in ("-64bitfloat.wav", "-32bitfloat.wav"):
        if os.path.isfile(os.path.join(directory, "sweep_1_to_44KHz" + cand)):
            return cand
    return None


def analytic_signal(x):
    n = len(x)
    X = np.fft.fft(x)
    h = np.zeros(n)
    h[0] = 1.0
    if n % 2 == 0:
        h[n // 2] = 1.0
        h[1:n // 2] = 2.0
    else:
        h[1:(n + 1) // 2] = 2.0
    return np.fft.ifft(X * h)


def measure_sweep(in_path, out_path):
    xi, si, _, _, _ = read_wav(in_path)
    yo, so, fmt_o, bits_o, _ = read_wav(out_path)
    ai, ao = analytic_signal(xi), analytic_signal(yo)
    env_in, env_out = np.abs(ai), np.abs(ao)
    inst_f = np.diff(np.unwrap(np.angle(ai))) * si / (2.0 * np.pi)
    t_in = np.arange(len(xi)) / si
    t_out = np.arange(len(yo)) / so
    f_at_out = np.interp(t_out, t_in[1:], inst_f)
    ratio = db(env_out / np.maximum(np.interp(t_out, t_in, env_in), 1e-30))

    edge = int(0.05 * so)
    fo_ = f_at_out[edge:-edge]
    ro_ = ratio[edge:-edge]
    eo_ = env_out[edge:-edge]

    points = {}
    for target in (1000, 10000, 18000, 20000, 21000, 21500, 21800, 21900, 21950, 21990, 22030):
        k = np.nonzero(np.abs(fo_ - target) < 3.0)[0]
        if len(k):
            points[str(target)] = float(np.median(ro_[k]))

    nyq = so / 2.0
    bw = {}
    for lim in (-0.1, -3.0):
        ok = fo_[(ro_ > lim) & (fo_ < nyq)]
        hz = float(ok.max()) if len(ok) else 0.0
        bw[f"{lim:+.1f}dB_hz"] = hz
        bw[f"{lim:+.1f}dB_pct"] = 100.0 * hz / nyq

    above = fo_ > (nyq + 250.0)
    above_far = fo_ > (nyq + 1500.0)
    stopband_max_db = float(db(np.max(eo_[above]))) if above.any() else -300.0
    stopband_far_max_db = float(db(np.max(eo_[above_far]))) if above_far.any() else -300.0
    return {
        "out_bits": bits_o,
        "out_fmt": fmt_o,
        "points_db": points,
        "bandwidth": bw,
        "stopband_max_db": stopband_max_db,
        "stopband_far_max_db": stopband_far_max_db,
    }


def measure_aliasing(in_path, out_path):
    xa, sa, _, _, _ = read_wav(in_path)
    ya, so, _, bits_o, _ = read_wav(out_path)
    mid = ya[int(1.0 * so):int(9.0 * so)]
    if len(mid) < 1024:
        mid = ya
    win = np.blackman(len(mid))
    Xo = np.abs(np.fft.rfft(mid * win)) / (np.sum(win) / 2.0)
    fo = np.fft.rfftfreq(len(mid), 1.0 / so)

    peak_idx = int(np.argmax(Xo))
    # 23 kHz visszahajlása 44,1 kHz-re: 44100 - 23000 = 21100 Hz
    alias_f = abs(so - 23000.0)
    alias_mask = np.abs(fo - alias_f) <= 2.0
    alias_tone_db = float(db(np.max(Xo[alias_mask]))) if alias_mask.any() else -300.0
    return {
        "out_bits": bits_o,
        "whole_peak_db": float(db(np.max(np.abs(ya)))),
        "whole_rms_db": float(db(np.sqrt(np.mean(ya ** 2)))),
        "mid_peak_db": float(db(np.max(np.abs(mid)))),
        "mid_rms_db": float(db(np.sqrt(np.mean(mid ** 2)))),
        "mid_fft_peak_db": float(db(Xo[peak_idx])),
        "mid_fft_peak_hz": float(fo[peak_idx]),
        "mid_fft_median_db": float(db(np.median(Xo))),
        "alias_tone_hz": float(alias_f),
        "alias_tone_db": alias_tone_db,
    }


def measure_intermod(in_path, out_path):
    xm, sm, _, _, _ = read_wav(in_path)
    ym, so, _, bits_o, _ = read_wav(out_path)

    def analyze_spec(x, sr):
        n = min(1 << 18, len(x) // 2)
        start = len(x) // 4
        # Blackman ablak: a távoli oldalsávok 18 dB/oktávval esnek, így -290 dB-ig mérhető a 64 bites zajpadló
        win = np.blackman(n)
        S = np.abs(np.fft.rfft(x[start:start + n] * win)) / (np.sum(win) / 2.0)
        Sd = db(S)
        fr = np.fft.rfftfreq(n, 1.0 / sr)
        return Sd, fr

    Sd_in, fr_in = analyze_spec(xm, sm)
    Sd_out, fr_out = analyze_spec(ym, so)

    # Két legerősebb bemeneti alaphang keresése (> 50 Hz távolsággal)
    idx_in = np.argsort(Sd_in)[::-1]
    primaries = []
    for i in idx_in:
        if all(abs(fr_in[i] - p[0]) > 50.0 for p in primaries):
            primaries.append((float(fr_in[i]), float(Sd_in[i])))
        if len(primaries) == 2:
            break

    # Kimenet: alaphangok kimaszkolása (±120 Hz, hogy a Blackman főcsúcs szoknyája ne zavarjon be)
    mask = np.ones(len(fr_out), dtype=bool)
    mask &= fr_out > 20.0  # DC kizárása
    for pf, _ in primaries:
        if pf < so / 2.0:
            mask &= np.abs(fr_out - pf) > 120.0

    rem_db = Sd_out[mask]
    rem_fr = fr_out[mask]
    worst_i = int(np.argmax(rem_db))

    return {
        "out_bits": bits_o,
        "input_primaries": primaries,
        "non_primary_peak_db": float(rem_db[worst_i]),
        "non_primary_peak_hz": float(rem_fr[worst_i]),
        "noise_floor_median_db": float(np.median(rem_db)),
    }


def measure_impulse(in_path, out_path):
    xi, si, _, _, _ = read_wav(in_path)
    yi, so, _, bits_o, _ = read_wav(out_path)
    nz = np.nonzero(np.abs(xi) > 0.01)[0]
    groups = np.split(nz, np.nonzero(np.diff(nz) > 1000)[0] + 1)
    first_in = int(groups[0][np.argmax(np.abs(xi[groups[0]]))])
    if len(groups) > 1:
        lim = int((groups[0][0] + (groups[1][0] - groups[0][0]) / 2.0) * so / si)
    else:
        lim = len(yi)
    pk = int(np.argmax(np.abs(yi[:lim])))
    expected_pk = first_in * so / si
    delay_samples = pk - expected_pk

    peak_val = float(np.abs(yi[pk]))
    seg_pre = yi[:pk]
    seg_post = yi[pk + 1:lim]
    pre_ms = {}
    post_ms = {}
    for thr in (-60, -100, -140, -180):
        idx_pre = np.nonzero(db(seg_pre / peak_val) > thr)[0]
        pre_ms[str(thr)] = float((len(seg_pre) - idx_pre[0]) / (so / 1000.0)) if len(idx_pre) else 0.0
        idx_post = np.nonzero(db(seg_post / peak_val) > thr)[0]
        post_ms[str(thr)] = float((idx_post[-1] + 1) / (so / 1000.0)) if len(idx_post) else 0.0

    # Impulzus-frekvencia: az első impulzus körüli ablak spektrumának lapossága 0..20 kHz-en
    half_w = min(pk, lim - pk - 1, 8192)
    imp_win = yi[pk - half_w:pk + half_w]
    Spec = db(np.abs(np.fft.rfft(imp_win)))
    freqs = np.fft.rfftfreq(len(imp_win), 1.0 / so)
    pass_band = (freqs >= 100.0) & (freqs <= 20000.0)
    pass_ref = float(np.median(Spec[pass_band]))
    imp_freq_ripple_db = float(np.max(np.abs(Spec[pass_band] - pass_ref)))

    return {
        "out_bits": bits_o,
        "peak_index": pk,
        "expected_peak": float(expected_pk),
        "delay_samples": float(delay_samples),
        "pre_ringing_ms": pre_ms,
        "post_ringing_ms": post_ms,
        "impulse_freq_max_dev_20k_db": imp_freq_ripple_db,
    }


def measure_gapless(in_dir, out_dir, suffix):
    """Hézagmentesség mérése a helyes (s -> m) sorrendben."""
    is_, si, _, _, _ = read_wav(os.path.join(in_dir, "gaplesstest_s" + suffix))
    im_, _, _, _, _ = read_wav(os.path.join(in_dir, "gaplesstest_m" + suffix))
    gs, so, _, bits_o, _ = read_wav(os.path.join(out_dir, "gaplesstest_s" + suffix))
    gm, _, _, _, _ = read_wav(os.path.join(out_dir, "gaplesstest_m" + suffix))

    joined = np.concatenate([gs, gm])
    seam = len(gs)
    diffs = np.abs(np.diff(joined))
    seam_jump = float(diffs[seam - 1])
    neigh_max = float(max(diffs[max(0, seam - 20):seam - 1].max(), diffs[seam:min(len(diffs), seam + 20)].max()))

    # Bemenet folytonos illesztése és összehasonlítása (lineáris interpolációval csak a bemeneti ugrás ellenőrzéséhez)
    in_seam_jump = float(abs(im_[0] - is_[-1]))
    # Harmadrendű (lokális polinomiális) folytonossági hiba a kimeneti illesztésnél:
    # ha a jel sima a határon, a 3. differencia kicsi; ha lépés/hullámzás van, kiugrik.
    d3 = np.abs(np.diff(joined, n=3))
    d3_seam = float(np.max(d3[max(0, seam - 6):min(len(d3), seam + 6)]))
    d3_interior = float(np.median(d3[200:seam - 200])) if seam > 450 else float(np.median(d3))

    # Bemenet s+m közvetlen FFT-alapú referencia (közelítő ideális 96->44.1 sávkorlátozott jel a belső szakaszon)
    in_joined = np.concatenate([is_, im_])
    expected_total = len(in_joined) * so / si

    return {
        "out_bits": bits_o,
        "in_len_s": len(is_),
        "in_len_m": len(im_),
        "out_len_s": len(gs),
        "out_len_m": len(gm),
        "out_total": len(joined),
        "expected_total": float(expected_total),
        "in_seam_jump": in_seam_jump,
        "out_seam_jump": seam_jump,
        "out_neighbor_max_step": neigh_max,
        "seam_d3_max": d3_seam,
        "interior_d3_median": d3_interior,
        "seam_d3_ratio_db": float(db(d3_seam / max(d3_interior, 1e-18))),
    }


def measure_bitdepth(in_dir, out_dir):
    results = {}
    for tag in ("16bitint", "24bitint", "32bitint", "32bitfloat", "64bitfloat"):
        for prefix in ("bitdepth_", "bitdepthtest_"):
            name = f"{prefix}{tag}.wav"
            ip = os.path.join(in_dir, name)
            op = os.path.join(out_dir, name)
            if os.path.isfile(ip) and os.path.isfile(op):
                xi, si, fmt_i, bits_i, _ = read_wav(ip)
                yo, so, fmt_o, bits_o, _ = read_wav(op)
                in_rms = float(np.sqrt(np.mean(xi ** 2)))
                out_rms = float(np.sqrt(np.mean(yo ** 2)))
                results[name] = {
                    "in_bits": bits_i,
                    "out_bits": bits_o,
                    "out_fmt": fmt_o,
                    "in_rms_db": float(db(in_rms)),
                    "out_rms_db": float(db(out_rms)),
                    "rms_diff_db": float(db(out_rms) - db(in_rms)),
                    "out_peak_db": float(db(np.max(np.abs(yo)))),
                }
    return results


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description="Helyi SRC közelítő mérőeszköz (V4)")
    ap.add_argument("in_dir", help="Bemeneti tesztcsomag mappa (pl. 96 kHz)")
    ap.add_argument("out_dir", help="Átalakított kimeneti mappa (pl. 44,1 kHz)")
    ap.add_argument("--json", dest="json_out", help="Mérési eredmény mentése JSON fájlba")
    args = ap.parse_args()

    suffix = detect_suffix(args.in_dir)
    report = {"in_dir": args.in_dir, "out_dir": args.out_dir, "suffix": suffix}

    if suffix:
        report["sweep"] = measure_sweep(
            os.path.join(args.in_dir, "sweep_1_to_44KHz" + suffix),
            os.path.join(args.out_dir, "sweep_1_to_44KHz" + suffix),
        )
        report["aliasing"] = measure_aliasing(
            os.path.join(args.in_dir, "aliasing150db" + suffix),
            os.path.join(args.out_dir, "aliasing150db" + suffix),
        )
        report["intermod"] = measure_intermod(
            os.path.join(args.in_dir, "intermodulation_sine" + suffix),
            os.path.join(args.out_dir, "intermodulation_sine" + suffix),
        )
        report["impulse"] = measure_impulse(
            os.path.join(args.in_dir, "impulse" + suffix),
            os.path.join(args.out_dir, "impulse" + suffix),
        )
        report["gapless"] = measure_gapless(args.in_dir, args.out_dir, suffix)

    bd = measure_bitdepth(args.in_dir, args.out_dir)
    if bd:
        report["bitdepth"] = bd

    # Áttekinthető szöveges összefoglaló
    print(f"=== SRC helyi mérés ({suffix or 'csak bitmélység'}) ===")
    if "sweep" in report:
        sw = report["sweep"]
        print(f"1. Sávszélesség ({sw['out_bits']} bit float):")
        print(f"   -0.1 dB: {sw['bandwidth']['-0.1dB_hz']:.0f} Hz ({sw['bandwidth']['-0.1dB_pct']:.2f}% Nyquist)")
        print(f"   -3.0 dB: {sw['bandwidth']['-3.0dB_hz']:.0f} Hz ({sw['bandwidth']['-3.0dB_pct']:.2f}% Nyquist)")
        print(f"   >22.3 kHz csúcs: {sw['stopband_max_db']:.1f} dBFS, >23.55 kHz csúcs: {sw['stopband_far_max_db']:.1f} dBFS")
    if "aliasing" in report:
        al = report["aliasing"]
        print(f"2. Visszahajlás (1–9 s): csúcs {al['mid_peak_db']:.1f} dBFS, RMS {al['mid_rms_db']:.1f} dBFS, "
              f"FFT csúcs {al['mid_fft_peak_db']:.1f} dB ({al['mid_fft_peak_hz']:.0f} Hz), medián {al['mid_fft_median_db']:.1f} dB, "
              f"visszahajlási tónus ({al['alias_tone_hz']:.0f} Hz): {al['alias_tone_db']:.1f} dB")
    if "intermod" in report:
        im = report["intermod"]
        print(f"3. Intermoduláció: nem-alaphang csúcs {im['non_primary_peak_db']:.1f} dB ({im['non_primary_peak_hz']:.0f} Hz), "
              f"zajpadló medián {im['noise_floor_median_db']:.1f} dB")
    if "impulse" in report:
        ip = report["impulse"]
        print(f"4. Impulzus: késleltetés {ip['delay_samples']:+.2f} minta, 0–20 kHz max eltérés {ip['impulse_freq_max_dev_20k_db']:.4f} dB")
        print("   előcsengés (ms):", ", ".join(f"{k} dB: {v:.2f}" for k, v in ip["pre_ringing_ms"].items()))
        print("   utócsengés (ms):", ", ".join(f"{k} dB: {v:.2f}" for k, v in ip["post_ringing_ms"].items()))
    if "gapless" in report:
        gl = report["gapless"]
        print(f"5. Hézagmentesség (s -> m): hossz {gl['out_len_s']}+{gl['out_len_m']}={gl['out_total']} "
              f"(várt {gl['expected_total']:.2f}), ugrás {gl['out_seam_jump']:.5f} (szomszéd max {gl['out_neighbor_max_step']:.5f}), "
              f"d3 arány {gl['seam_d3_ratio_db']:+.1f} dB")
    if "bitdepth" in report:
        print("6. Bitmélység-fájlok:")
        for name, info in report["bitdepth"].items():
            print(f"   {name:28s} {info['in_bits']}b -> {info['out_bits']}b (fmt {info['out_fmt']}), "
                  f"be RMS {info['in_rms_db']:.1f} dB -> ki RMS {info['out_rms_db']:.1f} dB (eltérés {info['rms_diff_db']:+.2f} dB)")

    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(report, fh, indent=2, ensure_ascii=False)
        print(f"JSON mentve: {args.json_out}")


if __name__ == "__main__":
    main()
