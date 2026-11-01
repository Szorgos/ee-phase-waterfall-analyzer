"""A két hangkártya felvétele közti időeltolódás (minta) kereszt-korrelációval, zajjelen.

Használat: lag_probe.py a.wav b.wav            (a két fájl bal csatornája)
           lag_probe.py ab.wav ab.wav stereo   (egy fájl bal és jobb csatornája)
Kimenet: "<késés mintában> <mintavételi frekvencia> <korrelációs csúcs/átlag arány>"; pozitív: b később szól.
"""
import sys, wave
import numpy as np


def load(p, chan=0):
    w = wave.open(p)
    sr, ch = w.getframerate(), w.getnchannels()
    a = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64)[chan::ch] / 32768.0
    w.close()
    return a, sr


stereo = len(sys.argv) > 3 and sys.argv[3] == "stereo"
a, sr = load(sys.argv[1], 0)
b, _ = load(sys.argv[2], 1 if stereo else 0)
n = min(len(a), len(b))
a, b = a[:n], b[:n]
# a zajos szakasz: ahol a 10 ms-os RMS a csúcs felét eléri; abból a középső 2 s
win = sr // 100
env = np.sqrt(np.convolve(a * a, np.ones(win) / win, mode="same"))
loud = np.where(env > 0.5 * env.max())[0]
if len(loud) < sr:
    print(99999, sr, 0)
    sys.exit(0)
mid = (loud[0] + loud[-1]) // 2
s0, s1 = max(0, mid - sr), min(n, mid + sr)
A, B = a[s0:s1], b[s0:s1]
m = 1 << int(np.ceil(np.log2(2 * len(A))))
c = np.fft.irfft(np.fft.rfft(B, m) * np.conj(np.fft.rfft(A, m)), m)
maxlag = sr // 5
lags = np.concatenate([np.arange(0, maxlag + 1), np.arange(-maxlag, 0)])
vals = np.concatenate([c[:maxlag + 1], c[-maxlag:]])
i = int(np.argmax(vals))
ratio = float(vals[i] / (np.mean(np.abs(vals)) + 1e-12))
print(int(lags[i]), sr, round(ratio, 1))
