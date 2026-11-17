"""
Szobamérési protokoll és állapotgép tesztek (Task E & H ellenőrzése).
"""
import math
import unittest

def db_to_amp(db):
    return 10.0 ** (db / 20.0)

def amp_to_db(amp):
    return 20.0 * math.log10(max(1e-12, amp))

class LevelCheckSimulator:
    @staticmethod
    def evaluate(samples, noise_threshold_rms=0.001):
        if not samples:
            return False, "NO_INPUT_DEVICE", "Nincs rögzített hangjel."
        
        max_peak = max(abs(s) for s in samples)
        rms = math.sqrt(sum(s * s for s in samples) / len(samples))
        peak_db = amp_to_db(max_peak)

        if rms < 0.00005:
            return False, "NO_INPUT_DEVICE", "A mikrofonból nem érkezik jel."
        if peak_db < -45.0:
            return False, "MIC_TOO_QUIET", "A mikrofon jele túl halk."
        if peak_db > -0.5:
            return False, "MIC_CLIPPING", "A bemeneti jel túlvezérelt (clipping)."
        
        if noise_threshold_rms > 0.0 and rms > 0.0:
            snr = amp_to_db(rms / noise_threshold_rms)
            if snr < 12.0:
                return False, "NOISY_ROOM", "A szoba háttérzaja túl magas."

        return True, "OK", f"Szint rendben: csúcs {peak_db:.1f} dBFS"

class AutoProfileMatcher:
    def __init__(self, config):
        self.enabled = config.get("enabled", False)
        self.mappings = config.get("mappings", {})

    def on_device_change(self, sink_name):
        if not self.enabled:
            return None
        sink_lower = sink_name.lower()
        if "headphone" in sink_lower or "fejhallgato" in sink_lower:
            return self.mappings.get("headphones")
        elif "dac" in sink_lower or "usb" in sink_lower:
            return self.mappings.get("usb_dac")
        else:
            return self.mappings.get("speakers")

class TestMeasurementProtocol(unittest.TestCase):
    def test_level_check_quiet(self):
        # -50 dBFS szintű teszthang
        samples = [db_to_amp(-50.0) * math.sin(i * 0.1) for i in range(1000)]
        ok, code, msg = LevelCheckSimulator.evaluate(samples)
        self.assertFalse(ok)
        self.assertEqual(code, "MIC_TOO_QUIET")

    def test_level_check_clipping(self):
        # 0.0 dBFS feletti túlvezérelt jel
        samples = [1.05 * math.sin(i * 0.1) for i in range(1000)]
        ok, code, msg = LevelCheckSimulator.evaluate(samples)
        self.assertFalse(ok)
        self.assertEqual(code, "MIC_CLIPPING")

    def test_level_check_no_signal(self):
        # Néma bemenet (zéró minták)
        samples = [0.0] * 1000
        ok, code, msg = LevelCheckSimulator.evaluate(samples)
        self.assertFalse(ok)
        self.assertEqual(code, "NO_INPUT_DEVICE")

    def test_level_check_noisy(self):
        # Erős háttérzajhoz képest alacsony SNR (< 12 dB), normál jelszinttel (-20 dBFS)
        samples = [db_to_amp(-20.0) * math.sin(i * 0.1) for i in range(1000)]
        ok, code, msg = LevelCheckSimulator.evaluate(samples, noise_threshold_rms=db_to_amp(-28.0))
        self.assertFalse(ok)
        self.assertEqual(code, "NOISY_ROOM")

    def test_level_check_normal(self):
        # Megfelelő szint: -18 dBFS, jó SNR
        samples = [db_to_amp(-18.0) * math.sin(i * 0.1) for i in range(1000)]
        ok, code, msg = LevelCheckSimulator.evaluate(samples, noise_threshold_rms=0.0001)
        self.assertTrue(ok)
        self.assertEqual(code, "OK")

    def test_safe_level_clamp(self):
        # A megadott szint nem lépheti túl a -6 dBFS felső korlátot
        requested_levels = [0.0, -3.0, -6.0, -20.0, -80.0]
        clamped = [max(-60.0, min(-6.0, lvl)) for lvl in requested_levels]
        self.assertEqual(clamped[0], -6.0)
        self.assertEqual(clamped[1], -6.0)
        self.assertEqual(clamped[2], -6.0)
        self.assertEqual(clamped[3], -20.0)
        self.assertEqual(clamped[4], -60.0)

    def test_auto_profile_switching(self):
        cfg = {
            "enabled": True,
            "mappings": {
                "headphones": "Sennheiser_HD650",
                "speakers": "Szoba_Nappali_Harman",
                "usb_dac": "Topping_DX3Pro_Direct"
            }
        }
        matcher = AutoProfileMatcher(cfg)

        # 1. Fejhallgató csatlakoztatása
        p1 = matcher.on_device_change("Realtek High Definition Audio (Headphones)")
        self.assertEqual(p1, "Sennheiser_HD650")

        # 2. USB DAC csatlakoztatása
        p2 = matcher.on_device_change("XMOS USB Audio DAC Output")
        self.assertEqual(p2, "Topping_DX3Pro_Direct")

        # 3. Hangszórók
        p3 = matcher.on_device_change("Speakers (Realtek Audio)")
        self.assertEqual(p3, "Szoba_Nappali_Harman")

        # 4. Kikapcsolt automatikus profilváltás
        cfg_disabled = {"enabled": False, "mappings": cfg["mappings"]}
        matcher_disabled = AutoProfileMatcher(cfg_disabled)
        self.assertIsNone(matcher_disabled.on_device_change("Headphones"))

if __name__ == "__main__":
    unittest.main()
