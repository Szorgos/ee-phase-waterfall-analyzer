"""Hangfájl szintje adott frekvenciákon (dBFS, a fájl közepének 1 s-ján)."""
import sys, wave, struct, math
def levels(path, freqs):
    w = wave.open(path); sr = w.getframerate(); ch = w.getnchannels(); sw = w.getsampwidth()
    raw = w.readframes(w.getnframes()); w.close()
    if sw == 4: vals = struct.unpack("<%di" % (len(raw)//4), raw); scale = 2**31
    else: vals = struct.unpack("<%dh" % (len(raw)//2), raw); scale = 2**15
    left = [vals[i] / scale for i in range(0, len(vals), ch)]
    n = len(left); a = max(0, n//2 - sr//2); seg = left[a:a+sr]
    out = {}
    for f in freqs:
        re = im = 0.0
        for i, x in enumerate(seg):
            wv = 0.5 - 0.5*math.cos(2*math.pi*i/(len(seg)-1))
            re += wv*x*math.cos(2*math.pi*f*i/sr); im -= wv*x*math.sin(2*math.pi*f*i/sr)
        amp = 2*math.hypot(re, im)/(len(seg)*0.5)
        out[f] = 20*math.log10(max(amp, 1e-9))
    return out, len(left)/sr
if __name__ == "__main__":
    lv, dur = levels(sys.argv[1], [float(x) for x in sys.argv[2:]])
    print(dur, " ".join(f"{k:g}:{v:.1f}" for k, v in lv.items()))
