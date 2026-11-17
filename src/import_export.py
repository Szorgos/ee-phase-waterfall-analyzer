#!/usr/bin/env python3
"""
Aqulify Import/Export és Sávcsökkentő modul.
Támogatja:
- Equalizer APO (.txt): Preamp, Filter, Channel (L/R/all), Delay (ms/samples),
  Convolution (.wav), GraphicEQ, Include (sandboxing, ciklusvédelem), Copy
- CamillaDSP (.yml, .yaml): Biquad, Delay, Gain, Conv, pipeline leképezés
- Mérések, kalibrációk, célgörbék (.csv, .txt, .frd, .cal)
- Sávcsökkentő (Band reducer): sűrű mérésekből optimális parametrikus sávok
- Export: Equalizer APO (.txt) és IR (.wav)
"""
import os
import re
import math
import wave
import struct

APO_TYPES = {
    "PK": 1, "PEAK": 1, "PEAKING": 1,
    "HP": 2, "HIGHPASS": 2, "HPQ": 2,
    "HS": 3, "HSC": 3, "HIGHSHELF": 3,
    "LP": 4, "LOWPASS": 4, "LPQ": 4,
    "LS": 5, "LSC": 5, "LOWSHELF": 5,
    "NO": 6, "NOTCH": 6,
    "AP": 8, "ALLPASS": 8,
    "BP": 9, "BANDPASS": 9
}

class EqBand:
    def __init__(self, type_id=1, freq=1000.0, gain=0.0, q=1.0, slope=0, mode=6, width=4.0):
        self.type = int(type_id)
        self.freq = float(freq)
        self.gain = float(gain)
        self.q = float(q)
        self.slope = int(slope)
        self.mode = int(mode)
        self.width = float(width)

    def to_dict(self):
        return {
            "type": self.type, "freq": round(self.freq, 2), "gain": round(self.gain, 2),
            "q": round(self.q, 3), "slope": self.slope, "mode": self.mode, "width": round(self.width, 3)
        }

    def eval_gain_db(self, f):
        """Biquad átvitel abszolút erősítése dB-ben analóg/bilineáris közelítéssel."""
        if self.type == 0:
            return 0.0
        f = max(1.0, float(f))
        f0 = max(1.0, self.freq)
        q = max(0.05, self.q)
        g = self.gain
        w = f / f0
        
        if self.type == 1: # Peaking
            # Egyszerű analóg prototípus dB formula
            # H(s) = (s^2 + s*(A/Q) + 1) / (s^2 + s/(A*Q) + 1) ahol A = 10^(g/40)
            A = 10.0 ** (g / 40.0)
            num_re = 1.0 - w * w
            num_im = w * (A / q)
            den_re = 1.0 - w * w
            den_im = w / (A * q)
            mag2 = (num_re * num_re + num_im * num_im) / max(1e-12, (den_re * den_re + den_im * den_im))
            return 10.0 * math.log10(max(1e-12, mag2))
        elif self.type == 2: # Highpass
            order = 1 if self.slope == 0 and abs(q - 0.707) > 0.2 else (4 if self.slope == 3 else 2)
            # 12 dB/oct Butterworth standard: |H|^2 = w^4 / (1 + w^4)
            if order == 2:
                mag2 = (w ** 4) / (1.0 + (1.0 / (q * q) - 2.0) * (w ** 2) + (w ** 4))
            elif order == 4:
                mag2 = (w ** 8) / (1.0 + w ** 8)
            else:
                mag2 = (w ** 2) / (1.0 + w ** 2)
            return 10.0 * math.log10(max(1e-12, mag2))
        elif self.type == 4: # Lowpass
            order = 1 if self.slope == 0 and abs(q - 0.707) > 0.2 else (4 if self.slope == 3 else 2)
            if order == 2:
                mag2 = 1.0 / (1.0 + (1.0 / (q * q) - 2.0) * (w ** 2) + (w ** 4))
            elif order == 4:
                mag2 = 1.0 / (1.0 + w ** 8)
            else:
                mag2 = 1.0 / (1.0 + w ** 2)
            return 10.0 * math.log10(max(1e-12, mag2))
        elif self.type == 3: # Highshelf
            A = 10.0 ** (g / 40.0)
            # Highshelf analóg átvitel
            num = A * A * (w ** 2) + 1.0
            den = (w ** 2) + A * A
            # skálázva
            return g * (w ** 2) / (1.0 + w ** 2)
        elif self.type == 5: # Lowshelf
            return g / (1.0 + w ** 2)
        elif self.type == 6: # Notch
            num = abs(1.0 - w * w)
            den = math.hypot(1.0 - w * w, w / q)
            return 20.0 * math.log10(max(1e-4, num / max(1e-12, den)))
        return 0.0

class ParsedConfig:
    def __init__(self):
        self.preamp = 0.0
        self.has_preamp = False
        self.eq_split = False
        self.bands_l = []
        self.bands_r = []
        self.delay_l_ms = 0.0
        self.delay_r_ms = 0.0
        self.conv_l = ""
        self.conv_r = ""
        self.conv_4ch = False
        self.skipped = [] # list of (line, reason)
        self.errors = []
        self.points = [] # list of (freq, db, phase) for CSV/GraphicEQ

def resolve_safe_path(base_dir, rel_path):
    """Path traversal védelem: csak a base_dir alatt lévő fájlok engedélyezettek."""
    norm_base = os.path.abspath(base_dir)
    # Ha abszolút útvonalat adtak meg (pl. C:\... vagy /etc/...)
    if os.path.isabs(rel_path):
        return None
    target = os.path.abspath(os.path.join(norm_base, rel_path))
    # Ellenőrzés: target a base_dir alatt van-e
    common = os.path.commonpath([norm_base, target])
    if common != norm_base:
        return None
    return target

def reduce_bands(freq_response_points, max_bands=10, min_f=20.0, max_f=20000.0, max_boost=None, min_cut=None):
    """
    Sávcsökkentő (Band reducer):
    Sűrű frekvenciaválasz pontokból (freq, db) legfeljebb max_bands darab
    parametrikus peaking és shelf szűrőt illeszt.
    Visszatér: (bands, peak_error_db, rms_error_db)
    """
    if not freq_response_points:
        return [], 0.0, 0.0
    
    # Pontok szűrése az audió sávra
    pts = [(f, db) for f, db in freq_response_points if min_f <= f <= max_f]
    if not pts:
        pts = freq_response_points[:]
    pts.sort(key=lambda x: x[0])
    
    # Reziduális hiba kezdetben a cél
    residual = [p[1] for p in pts]
    freqs = [p[0] for p in pts]
    bands = []
    
    for _ in range(max_bands):
        # Keressük a legnagyobb abszolút hibát
        max_idx = 0
        max_val = 0.0
        for i, r in enumerate(residual):
            if abs(r) > abs(max_val):
                max_val = r
                max_idx = i
                
        if abs(max_val) < 0.25: # Elértük a kellő pontosságot
            break
            
        peak_f = freqs[max_idx]
        peak_gain = max_val
        
        # Becsüljük a Q értéket a félérték-szélességből
        half_val = peak_gain * 0.5
        left_f = min_f
        right_f = max_f
        
        # Keressük balra
        for i in range(max_idx - 1, -1, -1):
            if (peak_gain > 0 and residual[i] <= half_val) or (peak_gain < 0 and residual[i] >= half_val):
                left_f = freqs[i]
                break
        # Keressük jobbra
        for i in range(max_idx + 1, len(freqs)):
            if (peak_gain > 0 and residual[i] <= half_val) or (peak_gain < 0 and residual[i] >= half_val):
                right_f = freqs[i]
                break
                
        bw_oct = math.log2(max(1.05, right_f / max(1.0, left_f)))
        if bw_oct < 0.1:
            bw_oct = 0.1
        elif bw_oct > 4.0:
            bw_oct = 4.0
        q = math.sqrt(2.0 ** bw_oct) / max(0.01, (2.0 ** bw_oct - 1.0))
        q = max(0.5, min(10.0, q))
        
        # Finomhangolás kis lépésekben a lokális RMS csökkentésére
        best_g = peak_gain
        best_q = q
        best_rms = float("inf")
        
        max_allowed_g = max(p[1] for p in pts)
        min_allowed_g = min(p[1] for p in pts)
        candidates = [peak_gain * 0.85, peak_gain * 0.95, peak_gain]
        if peak_gain > 0:
            candidates.append(min(peak_gain * 1.05, max_allowed_g))
        else:
            candidates.append(max(peak_gain * 1.05, min_allowed_g))
        for g_try in candidates:
            for q_try in [q * 0.7, q, q * 1.4]:
                band_candidate = EqBand(type_id=1, freq=peak_f, gain=g_try, q=q_try)
                err_sq = 0.0
                for i, f in enumerate(freqs):
                    diff = residual[i] - band_candidate.eval_gain_db(f)
                    err_sq += diff * diff
                if err_sq < best_rms:
                    best_rms = err_sq
                    best_g = g_try
                    best_q = q_try
                    
        if max_boost is not None:
            best_g = min(best_g, max_boost)
        if min_cut is not None:
            best_g = max(best_g, min_cut)
        new_band = EqBand(type_id=1, freq=peak_f, gain=best_g, q=best_q)
        bands.append(new_band)
        
        # Reziduum frissítése
        for i, f in enumerate(freqs):
            residual[i] -= new_band.eval_gain_db(f)
            
    # Hiba kiszámítása
    errs = [abs(r) for r in residual]
    peak_err = max(errs) if errs else 0.0
    rms_err = math.sqrt(sum(r * r for r in residual) / max(1, len(residual)))
    return bands, peak_err, rms_err

def parse_apo_file(file_path, base_dir=None, visited_files=None):
    """
    Equalizer APO konfiguráció rekurzív parszolása Include, Channel, Delay, GraphicEQ kezeléssel.
    """
    if base_dir is None:
        base_dir = os.path.dirname(os.path.abspath(file_path))
    if visited_files is None:
        visited_files = set()
        
    cfg = ParsedConfig()
    norm_path = os.path.abspath(file_path)
    if norm_path in visited_files:
        cfg.skipped.append((f"Include: {os.path.basename(file_path)}", f"Körkörös Include hivatkozás: {os.path.basename(file_path)}"))
        return cfg
    visited_files.add(norm_path)
    
    if not os.path.exists(file_path):
        cfg.errors.append(f"A fájl nem található: {os.path.basename(file_path)}")
        cfg.skipped.append((f"Fájl nem található", f"A fájl nem található: {os.path.basename(file_path)}"))
        return cfg
        
    with open(file_path, "r", encoding="utf-8", errors="replace") as f:
        lines = f.readlines()
        
    current_channel = "all" # "all", "L", "R"
    
    for raw_line in lines:
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
            
        # Preamp: -6.2 dB
        m = re.match(r"^Preamp:\s*([+-]?\d+(?:\.\d+)?)\s*dB", line, re.I)
        if m:
            cfg.preamp = float(m.group(1))
            cfg.has_preamp = True
            continue
            
        # Channel: L | R | L R | all
        m = re.match(r"^Channel:\s*(.*)$", line, re.I)
        if m:
            ch_str = m.group(1).strip().upper()
            toks = ch_str.split()
            if "ALL" in toks or ("L" in toks and "R" in toks) or ("1" in toks and "2" in toks):
                current_channel = "all"
            elif "L" in toks or "1" in toks:
                current_channel = "L"
            elif "R" in toks or "2" in toks:
                current_channel = "R"
            else:
                current_channel = "all"
            continue
            
        # Delay: 0.35 ms | 17 samples
        m = re.match(r"^Delay:\s*([+-]?\d+(?:\.\d+)?)\s*(ms|samples?)", line, re.I)
        if m:
            val = float(m.group(1))
            unit = m.group(2).lower()
            delay_ms = val if "ms" in unit else (val / 48000.0 * 1000.0)
            if current_channel == "L":
                cfg.delay_l_ms = delay_ms
                cfg.eq_split = True
            elif current_channel == "R":
                cfg.delay_r_ms = delay_ms
                cfg.eq_split = True
            else:
                cfg.delay_l_ms = delay_ms
                cfg.delay_r_ms = delay_ms
            continue
            
        # Convolution: ir.wav
        m = re.match(r"^Convolution:\s*[\"']?([^\"'\r\n]+)[\"']?", line, re.I)
        if m:
            ir_rel = m.group(1).strip()
            ir_abs = resolve_safe_path(base_dir, ir_rel)
            if not ir_abs or not os.path.exists(ir_abs):
                cfg.skipped.append((line, f"A fájl nem található: {ir_rel}"))
            else:
                if current_channel == "L":
                    cfg.conv_l = ir_abs
                    cfg.eq_split = True
                elif current_channel == "R":
                    cfg.conv_r = ir_abs
                    cfg.eq_split = True
                else:
                    cfg.conv_l = ir_abs
                    cfg.conv_r = ir_abs
            continue
            
        # Include: fajl.txt
        m = re.match(r"^Include:\s*[\"']?([^\"'\r\n]+)[\"']?", line, re.I)
        if m:
            inc_rel = m.group(1).strip()
            inc_safe = resolve_safe_path(base_dir, inc_rel)
            if not inc_safe:
                cfg.skipped.append((line, f"Tiltott Include elérési út (kilép a könyvtárból): {inc_rel}"))
                continue
            # Rekurzív olvasás
            sub_cfg = parse_apo_file(inc_safe, base_dir, visited_files)
            cfg.skipped.extend(sub_cfg.skipped)
            cfg.errors.extend(sub_cfg.errors)
            if sub_cfg.has_preamp and not cfg.has_preamp:
                cfg.preamp = sub_cfg.preamp
                cfg.has_preamp = True
            if sub_cfg.eq_split:
                cfg.eq_split = True
            cfg.bands_l.extend(sub_cfg.bands_l)
            cfg.bands_r.extend(sub_cfg.bands_r)
            continue
            
        # Copy: L=R R=L vagy Copy: L=0.5*L+0.5*R
        m = re.match(r"^Copy:\s*(.*)$", line, re.I)
        if m:
            copy_expr = m.group(1).replace(" ", "").upper()
            if copy_expr == "L=RR=L" or copy_expr == "R=LL=R":
                # Bal és jobb csatorna felcserélése
                cfg.eq_split = True
                cfg.bands_l, cfg.bands_r = cfg.bands_r, cfg.bands_l
                cfg.delay_l_ms, cfg.delay_r_ms = cfg.delay_r_ms, cfg.delay_l_ms
                cfg.conv_l, cfg.conv_r = cfg.conv_r, cfg.conv_l
            else:
                cfg.skipped.append((line, f"Csatornakeverés nem támogatott (csak felcserélés): {line}"))
            continue
            
        # GraphicEQ: 20 -0.0; 25 -0.2; ...
        m = re.match(r"^GraphicEQ:\s*(.*)$", line, re.I)
        if m:
            pairs = m.group(1).split(";")
            pts = []
            for p in pairs:
                p = p.strip()
                if not p:
                    continue
                parts = p.split()
                if len(parts) >= 2:
                    try:
                        pts.append((float(parts[0]), float(parts[1])))
                    except ValueError:
                        pass
            cfg.points = pts
            reduced_bands, _, _ = reduce_bands(pts, max_bands=10)
            if current_channel == "L":
                cfg.bands_l.extend(reduced_bands)
                cfg.eq_split = True
            elif current_channel == "R":
                cfg.bands_r.extend(reduced_bands)
                cfg.eq_split = True
            else:
                cfg.bands_l.extend(reduced_bands)
                if cfg.eq_split:
                    cfg.bands_r.extend(reduced_bands)
            continue
            
        # Nem támogatott vagy egyéb direktívák
        m = re.match(r"^(Device|Stage|Eval|If|ElseIf|Else|EndIf)\s*:", line, re.I)
        if m:
            cfg.skipped.append((line, f"Direktíva figyelmen kívül hagyva: {m.group(1)}"))
            continue
            
        # Filter sorok
        # Példák:
        # Filter 1: ON PK Fc 65 Hz Gain 5.8 dB Q 1.41
        # Filter 5: OFF PK Fc 3000 Hz Gain 4.0 dB Q 2.00
        # Filter 2: ON LS 6dB Fc 120 Hz Gain 2.0 dB
        # Filter 1: ON HP Fc 25 Hz
        # Filter 2: ON LPQ Fc 18000 Hz Q 0.71
        # Filter 3: ON LS 12dB Fc 80 Hz Gain 3.0 dB
        # Filter 5: ON HP 24 dB Fc 30 Hz
        m = re.match(r"^Filter\s*\d*\s*:\s*(ON|OFF)\s+([A-Za-z]+)(?:\s+(\d+(?:\.\d+)?)\s*dB)?\s+Fc\s+(\d+(?:\.\d+)?)\s*Hz(?:\s+Gain\s+([+-]?\d+(?:\.\d+)?)\s*dB)?(?:\s+(?:Q\s+(\d+(?:\.\d+)?)|BW\s+Oct\s+(\d+(?:\.\d+)?)))?", line, re.I)
        if m:
            is_on = m.group(1).upper() == "ON"
            if not is_on:
                cfg.skipped.append((line, "Kikapcsolt (OFF) szűrő figyelmen kívül hagyva"))
                continue
            ft = m.group(2).upper()
            slope_str = m.group(3)
            freq = float(m.group(4))
            gain = float(m.group(5)) if m.group(5) is not None else 0.0
            q_val = float(m.group(6)) if m.group(6) is not None else None
            bw_val = float(m.group(7)) if m.group(7) is not None else None
            
            # Szűrőtípus azonosítása
            type_id = APO_TYPES.get(ft)
            if type_id is None:
                cfg.skipped.append((line, f"Ismeretlen szűrőtípus: {ft}"))
                continue
                
            # Q számítás ha BW van
            if q_val is None and bw_val is not None:
                b = 2.0 ** bw_val
                q_val = math.sqrt(b) / (b - 1.0)
            if q_val is None:
                q_val = 0.7071 # Butterworth alapértelmezés
                
            # Meredekség
            slope = 0
            if slope_str:
                sl_db = float(slope_str)
                if sl_db >= 24: slope = 3 # 24 dB/oct
                elif sl_db >= 18: slope = 2 # 18 dB/oct
                elif sl_db >= 12: slope = 1 # 12 dB/oct
                else: slope = 0 # 6 dB/oct
            elif ft in ("HP", "LP", "LS", "HS"):
                slope = 0
                
            band = EqBand(type_id=type_id, freq=freq, gain=gain, q=q_val, slope=slope)
            if current_channel == "L":
                cfg.bands_l.append(band)
                cfg.eq_split = True
            elif current_channel == "R":
                cfg.bands_r.append(band)
                cfg.eq_split = True
            else:
                cfg.bands_l.append(band)
                if cfg.eq_split:
                    cfg.bands_r.append(band)
            continue
            
        # Ismeretlen sor
        cfg.skipped.append((line, "Értelmezhetetlen sor figyelmen kívül hagyva"))
        
    # Ha volt szétválasztott csatorna és mindkettőnek van sávja:
    if cfg.bands_r and not cfg.bands_l:
        cfg.eq_split = True
    elif cfg.bands_l and not cfg.bands_r and cfg.eq_split:
        pass
    return cfg

def parse_csv_points(file_path):
    """
    Mérések, kalibrációk, célgörbék olvasása (.csv, .txt, .frd, .cal).
    Kihagyja az idézőjeles és komment sorokat (* vagy #).
    """
    pts = []
    with open(file_path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            l = line.strip()
            if not l or l.startswith("#") or l.startswith("*") or l.startswith('"'):
                continue
            # Vessző, pontosvessző vagy tabulátor/szóköz tagolás
            clean = re.sub(r"[,;\t]+", " ", l)
            parts = clean.split()
            if len(parts) >= 2:
                try:
                    freq = float(parts[0])
                    db = float(parts[1])
                    ph = float(parts[2]) if len(parts) >= 3 else 0.0
                    pts.append((freq, db, ph))
                except ValueError:
                    continue
    return pts

def parse_camilladsp_yaml(file_path):
    """
    CamillaDSP konfigurációs fájl (.yml / .yaml) kézi parszolása külső függőség nélkül.
    Kinyeri a szűrőket, delayt, gaineket és a pipeline csatorna-leképezést.
    """
    cfg = ParsedConfig()
    with open(file_path, "r", encoding="utf-8", errors="replace") as f:
        content = f.read()
        
    # Szűrő definíciók kinyerése
    # Egyszerű állapotgép a YAML struktúra beolvasásához
    filters = {}
    lines = content.splitlines()
    in_filters = False
    in_pipeline = False
    cur_filter_name = None
    cur_filter_data = {}
    pipeline_steps = []
    cur_step = {}
    
    for line in lines:
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
            
        indent = len(line) - len(line.lstrip())
        
        if line.startswith("filters:"):
            in_filters = True
            in_pipeline = False
            continue
        elif line.startswith("pipeline:"):
            in_filters = False
            in_pipeline = True
            if cur_filter_name:
                filters[cur_filter_name] = cur_filter_data
                cur_filter_name = None
            continue
        elif not line.startswith(" ") and not line.startswith("\t"):
            in_filters = False
            in_pipeline = False
            if cur_filter_name:
                filters[cur_filter_name] = cur_filter_data
                cur_filter_name = None
            continue
            
        if in_filters:
            if indent == 2 and line.strip().endswith(":"):
                if cur_filter_name:
                    filters[cur_filter_name] = cur_filter_data
                cur_filter_name = line.strip()[:-1]
                cur_filter_data = {}
            elif indent >= 4 and cur_filter_name:
                # parameters: {type: Peaking, freq: 44, gain: -7.0, q: 5.0} vagy típus sorok
                if ":" in stripped:
                    k, v = stripped.split(":", 1)
                    k = k.strip()
                    v = v.strip()
                    if k == "parameters" and v.startswith("{") and v.endswith("}"):
                        inner = v[1:-1]
                        for item in inner.split(","):
                            if ":" in item:
                                ik, iv = item.split(":", 1)
                                iks = ik.strip()
                                ivs = iv.strip()
                                if iks == "type":
                                    cur_filter_data["sub_type"] = ivs
                                else:
                                    cur_filter_data[iks] = ivs
                    else:
                        cur_filter_data[k] = v
                        
        if in_pipeline:
            # - type: Filter
            #   channels: [0]
            #   names: [vol, hp, bass_l, delay_l, shelf]
            if stripped.startswith("- "):
                if cur_step:
                    pipeline_steps.append(cur_step)
                cur_step = {}
                stripped = stripped[2:]
            if ":" in stripped:
                pk, pv = stripped.split(":", 1)
                pk = pk.strip()
                pv = pv.strip()
                if pv.startswith("[") and pv.endswith("]"):
                    items = [x.strip() for x in pv[1:-1].split(",") if x.strip()]
                    cur_step[pk] = items
                else:
                    cur_step[pk] = pv
                    
    if cur_filter_name:
        filters[cur_filter_name] = cur_filter_data
    if cur_step:
        pipeline_steps.append(cur_step)
        
    # Pipeline feldolgozása csatornánként (0 = Bal, 1 = Jobb)
    # Csak azok a szűrők kerülnek be, amelyek a pipeline-ban szerepelnek!
    chan_filters = {0: [], 1: []}
    used_filter_names = set()
    
    for step in pipeline_steps:
        if step.get("type") == "Filter":
            channels = [int(c) for c in step.get("channels", [])]
            names = step.get("names", [])
            for ch in channels:
                if ch in chan_filters:
                    for n in names:
                        chan_filters[ch].append(n)
                        used_filter_names.add(n)
                        
    # Nem használt vagy ismeretlen szűrők jelzése
    for fname, fdata in filters.items():
        ftype = fdata.get("type")
        if fname not in used_filter_names:
            if ftype in ("DiffEq",):
                cfg.skipped.append((f"filter: {fname}", f"Nem támogatott szűrőtípus: {ftype}"))
            continue
            
    # Szűrők leképezése
    for ch_idx, flt_list in chan_filters.items():
        is_r = (ch_idx == 1)
        for fname in flt_list:
            if fname not in filters:
                continue
            fdata = filters[fname]
            ftype = fdata.get("type")
            
            if ftype == "Gain":
                gain_val = float(fdata.get("gain", 0.0))
                cfg.preamp = gain_val
                cfg.has_preamp = True
            elif ftype == "Delay":
                d_val = float(fdata.get("delay", 0.0))
                d_unit = fdata.get("unit", "ms").lower()
                d_ms = d_val if d_unit == "ms" else (d_val / 48000.0 * 1000.0)
                if is_r:
                    cfg.delay_r_ms = d_ms
                    cfg.eq_split = True
                else:
                    cfg.delay_l_ms = d_ms
            elif ftype == "Biquad":
                sub_type = fdata.get("sub_type", fdata.get("type", ""))
                freq = float(fdata.get("freq", 1000.0))
                gain = float(fdata.get("gain", 0.0))
                q = float(fdata.get("q", 0.7071))
                
                type_id = 1
                if sub_type in ("Peaking", "Peak"): type_id = 1
                elif sub_type in ("Highpass", "HP"): type_id = 2
                elif sub_type in ("Highshelf", "HS"): type_id = 3
                elif sub_type in ("Lowpass", "LP"): type_id = 4
                elif sub_type in ("Lowshelf", "LS"): type_id = 5
                
                band = EqBand(type_id=type_id, freq=freq, gain=gain, q=q)
                if is_r:
                    cfg.bands_r.append(band)
                else:
                    cfg.bands_l.append(band)
            elif ftype == "Conv":
                # Konvolúció
                filename = fdata.get("filename", "")
                if is_r:
                    cfg.conv_r = filename
                    cfg.eq_split = True
                else:
                    cfg.conv_l = filename
            else:
                cfg.skipped.append((f"filter: {fname}", f"Nem támogatott szűrőtípus: {ftype}"))
                
    if cfg.bands_r or cfg.delay_r_ms > 0:
        cfg.eq_split = True
        
    return cfg

def export_apo_config(cfg):
    """
    Konfiguráció exportálása Equalizer APO .txt formátumba.
    Csatornánkénti elválasztással ha splitelt, delay és preamp értékekkel.
    """
    lines = ["# Aqulify Equalizer APO Export"]
    if cfg.has_preamp or cfg.preamp != 0.0:
        lines.append(f"Preamp: {cfg.preamp:.1f} dB")
        
    if cfg.eq_split:
        # Bal csatorna
        lines.append("Channel: L")
        if cfg.delay_l_ms > 0.0:
            lines.append(f"Delay: {cfg.delay_l_ms:.2f} ms")
        if cfg.conv_l:
            lines.append(f"Convolution: {os.path.basename(cfg.conv_l)}")
        for i, b in enumerate(cfg.bands_l, 1):
            type_name = "PK"
            if b.type == 2: type_name = "HP"
            elif b.type == 3: type_name = "HSC"
            elif b.type == 4: type_name = "LP"
            elif b.type == 5: type_name = "LSC"
            elif b.type == 6: type_name = "NO"
            lines.append(f"Filter {i}: ON {type_name} Fc {b.freq:.1f} Hz Gain {b.gain:.2f} dB Q {b.q:.3f}")
            
        # Jobb csatorna
        lines.append("Channel: R")
        if cfg.delay_r_ms > 0.0:
            lines.append(f"Delay: {cfg.delay_r_ms:.2f} ms")
        if cfg.conv_r:
            lines.append(f"Convolution: {os.path.basename(cfg.conv_r)}")
        for i, b in enumerate(cfg.bands_r, 1):
            type_name = "PK"
            if b.type == 2: type_name = "HP"
            elif b.type == 3: type_name = "HSC"
            elif b.type == 4: type_name = "LP"
            elif b.type == 5: type_name = "LSC"
            elif b.type == 6: type_name = "NO"
            lines.append(f"Filter {i}: ON {type_name} Fc {b.freq:.1f} Hz Gain {b.gain:.2f} dB Q {b.q:.3f}")
    else:
        if cfg.delay_l_ms > 0.0:
            lines.append(f"Delay: {cfg.delay_l_ms:.2f} ms")
        if cfg.conv_l:
            lines.append(f"Convolution: {os.path.basename(cfg.conv_l)}")
        for i, b in enumerate(cfg.bands_l, 1):
            type_name = "PK"
            if b.type == 2: type_name = "HP"
            elif b.type == 3: type_name = "HSC"
            elif b.type == 4: type_name = "LP"
            elif b.type == 5: type_name = "LSC"
            elif b.type == 6: type_name = "NO"
            lines.append(f"Filter {i}: ON {type_name} Fc {b.freq:.1f} Hz Gain {b.gain:.2f} dB Q {b.q:.3f}")
            
    return "\n".join(lines) + "\n"

def export_ir_wav(file_path, channels_data, sample_rate=48000):
    """
    WAV fájl exportálása 32-bites IEEE lebegőpontos formátumban.
    channels_data: lista tömbökből [ch0, ch1, ...] (2 csatorna sztereó, vagy 4 csatorna true stereo)
    """
    num_channels = len(channels_data)
    num_frames = len(channels_data[0])
    
    with open(file_path, "wb") as f:
        # RIFF header
        byte_rate = sample_rate * num_channels * 4
        block_align = num_channels * 4
        data_size = num_frames * block_align
        
        f.write(b"RIFF")
        f.write(struct.pack("<I", 36 + data_size))
        f.write(b"WAVE")
        
        # fmt chunk
        f.write(b"fmt ")
        f.write(struct.pack("<I", 16)) # Subchunk1Size
        f.write(struct.pack("<H", 3))  # AudioFormat = 3 (IEEE Float)
        f.write(struct.pack("<H", num_channels))
        f.write(struct.pack("<I", sample_rate))
        f.write(struct.pack("<I", byte_rate))
        f.write(struct.pack("<H", block_align))
        f.write(struct.pack("<H", 32)) # BitsPerSample
        
        # data chunk
        f.write(b"data")
        f.write(struct.pack("<I", data_size))
        for i in range(num_frames):
            for ch in range(num_channels):
                val = float(channels_data[ch][i])
                f.write(struct.pack("<f", val))
