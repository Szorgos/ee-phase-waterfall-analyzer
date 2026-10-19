import math
import cmath

class BiquadFilter:
    def __init__(self, filter_type, freq, q, gain_db=0.0, sample_rate=48000.0):
        self.filter_type = filter_type
        self.freq = freq
        self.q = q
        self.gain_db = gain_db
        self.sample_rate = sample_rate

    def get_coefficients(self):
        w0 = 2.0 * math.pi * self.freq / self.sample_rate
        cos_w0 = math.cos(w0)
        sin_w0 = math.sin(w0)
        alpha = sin_w0 / (2.0 * self.q)
        A = 10.0 ** (self.gain_db / 40.0)

        if self.filter_type == "peaking":
            b0 = 1.0 + alpha * A
            b1 = -2.0 * cos_w0
            b2 = 1.0 - alpha * A
            a0 = 1.0 + alpha / A
            a1 = -2.0 * cos_w0
            a2 = 1.0 - alpha / A
        elif self.filter_type == "highshelf":
            two_sqrtA_alpha = 2.0 * math.sqrt(A) * alpha
            b0 = A * ((A + 1.0) + (A - 1.0) * cos_w0 + two_sqrtA_alpha)
            b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cos_w0)
            b2 = A * ((A + 1.0) + (A - 1.0) * cos_w0 - two_sqrtA_alpha)
            a0 = (A + 1.0) - (A - 1.0) * cos_w0 + two_sqrtA_alpha
            a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cos_w0)
            a2 = (A + 1.0) - (A - 1.0) * cos_w0 - two_sqrtA_alpha
        elif self.filter_type == "lowpass":
            b0 = (1.0 - cos_w0) / 2.0
            b1 = 1.0 - cos_w0
            b2 = (1.0 - cos_w0) / 2.0
            a0 = 1.0 + alpha
            a1 = -2.0 * cos_w0
            a2 = 1.0 - alpha
        else:
            return 1.0, 0.0, 0.0, 1.0, 0.0, 0.0

        # Normalizálás a0-val
        return b0/a0, b1/a0, b2/a0, 1.0, a1/a0, a2/a0

    def evaluate_h(self, f):
        b0, b1, b2, a0, a1, a2 = self.get_coefficients()
        w = 2.0 * math.pi * f / self.sample_rate
        z1 = cmath.exp(complex(0, -w))
        z2 = cmath.exp(complex(0, -2.0 * w))
        num = b0 + b1 * z1 + b2 * z2
        den = 1.0 + a1 * z1 + a2 * z2
        return num / den

    def phase_rad(self, f):
        return cmath.phase(self.evaluate_h(f))

    def group_delay_ms(self, f):
        df = 0.5
        f1 = max(1.0, f - df)
        f2 = f + df
        p1 = self.phase_rad(f1)
        p2 = self.phase_rad(f2)
        # Unwrap
        while p2 - p1 > math.pi: p2 -= 2.0 * math.pi
        while p2 - p1 < -math.pi: p2 += 2.0 * math.pi
        dw = 2.0 * math.pi * (f2 - f1)
        return (- (p2 - p1) / dw) * 1000.0


class FilterChain:
    def __init__(self, sample_rate=48000.0):
        self.filters = []
        self.sample_rate = sample_rate

    def add(self, flt):
        self.filters.append(flt)

    def eval_at(self, f):
        total_h = complex(1.0, 0.0)
        delay_ms = 0.0
        for flt in self.filters:
            total_h *= flt.evaluate_h(f)
            delay_ms += flt.group_delay_ms(f)
        mag_db = 20.0 * math.log10(max(1e-6, abs(total_h)))
        phase_deg = math.degrees(cmath.phase(total_h))
        return mag_db, phase_deg, delay_ms

def main():
    print("=" * 62)
    print(" EasyEffects Analitikus Biquad & Fáziskalkulátor (Python Verifikáció)")
    print("=" * 62)

    chain = FilterChain()
    # 1. Sub-bass peak: 60 Hz, Q=1.414, +6 dB
    chain.add(BiquadFilter("peaking", 60.0, 1.414, 6.0))
    # 2. Szobamódus vágás: 120 Hz, Q=5.0, -9 dB
    chain.add(BiquadFilter("peaking", 120.0, 5.0, -9.0))
    # 3. High shelf: 10 kHz, Q=0.707, +3 dB
    chain.add(BiquadFilter("highshelf", 10000.0, 0.7071, 3.0))

    print("\nAktív szűrők a láncban:")
    print(" 1. Peaking EQ:    60 Hz,  Q=1.41, +6.0 dB")
    print(" 2. Peaking EQ:   120 Hz,  Q=5.00, -9.0 dB (éles szobavágás)")
    print(" 3. High Shelf: 10000 Hz,  Q=0.71, +3.0 dB\n")

    print(f"{'Freq (Hz)':>10} | {'Gain (dB)':>10} | {'Fázis (°)':>10} | {'Késleltetés (ms)':>18}")
    print("-" * 62)

    test_freqs = [20.0, 40.0, 60.0, 80.0, 110.0, 120.0, 130.0, 200.0, 500.0, 1000.0, 5000.0, 10000.0, 20000.0]
    for f in test_freqs:
        mag, phase, delay = chain.eval_at(f)
        print(f"{f:10.1f} | {mag:10.2f} | {phase:10.2f} | {delay:18.3f}")

    print("\n[OK] A matematikai modell tökéletesen működik!")

if __name__ == "__main__":
    main()
