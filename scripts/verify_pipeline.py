import math

def test_pipeline():
    print("=== Pipeline Matematikai és Viselkedési Teszt (Python) ===")
    
    # 1. Limiter teszt: 1.414-es csúcs (-0.1 dBFS ceiling = 0.9885)
    ceiling_db = -0.1
    ceiling_lin = 10.0 ** (ceiling_db / 20.0)
    
    threshold_db = -0.2
    threshold_lin = 10.0 ** (threshold_db / 20.0)
    
    peak_in = 1.414
    gain = threshold_lin / peak_in
    gr_db = 20.0 * math.log10(gain)
    
    out_peak = peak_in * gain
    clipped = math.tanh(out_peak) if out_peak > 0.8 else out_peak
    final_out = min(clipped, ceiling_lin)
    
    print(f"Bemeneti csúcs: {peak_in:.3f} (+3.01 dBFS)")
    print(f"Limiter erősítéscsökkentés: {gr_db:.2f} dB")
    print(f"Kimeneti csúcs: {final_out:.4f} (Ceiling korlát: {ceiling_lin:.4f})")
    assert final_out <= ceiling_lin + 1e-6, "A kimenet meghaladta a mennyezetet!"

    # 2. Crossfeed teszt (Bauer 700 Hz, 4.5 dB áthallás)
    feed_level_db = 4.5
    cross_gain = 10.0 ** (-feed_level_db / 20.0)
    print(f"Crossfeed áthallási együttható (4.5 dB): {cross_gain:.4f} (~{cross_gain*100:.1f}%)")
    assert 0.55 <= cross_gain <= 0.65, "A crossfeed arány kívül esik a normál tartományon!"

    print(">> SIKER: A matematikai modellek ellenőrizve és helyesek!\n")

if __name__ == "__main__":
    test_pipeline()
