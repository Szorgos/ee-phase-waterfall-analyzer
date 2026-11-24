#pragma once
/**
 * @file measurement_manager.hpp
 * @brief Szobamérési folyamatvezérlő (audio I/O vezérlés, biztonsági limitek, állapotgép, haladás, hibakezelés).
 *
 * Működés és biztonsági elvek:
 * - A mérés szigorúan helyi gépről vagy admin klienstől indítható.
 * - Lejátszási szint maximálva: alapértelmezett -20 dBFS, szigorú felső limit -6 dBFS (hangszóró- és hallásvédelem).
 * - Azonnali leállítási lehetőség (stop_measurement).
 * - Standardizált hibakódok (MIC_TOO_QUIET, MIC_CLIPPING, NOISY_ROOM, NO_INPUT_DEVICE).
 * - Közérthető eredmény-összefoglaló egyszerű módban, részletes spektrumok haladó módban.
 */

#include "sweep.hpp"
#include "deconv.hpp"
#include "smoothing.hpp"
#include "auto_eq.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <functional>
#include <iomanip>
#include <memory>
#include <mutex>
#include <string>
#include <sstream>
#include <vector>

namespace aqulify::room {

enum class MeasureState {
    Idle,
    LevelCheck,
    MeasuringSweep,
    Deconvolving,
    Analyzing,
    Complete,
    Error,
    Stopped
};

struct MeasureConfig {
    std::string channel = "both";   // "left", "right", "both", vagy "xover_0".."xover_3"
    int xoverBand = -1;             // -1: normál kimenet, 0..3: keresztváltó sáv
    float levelDb = -20.0f;         // Lejátszási szint dBFS-ben (max -6 dBFS)
    float durationSec = 3.0f;       // Sweep hossza (másodperc)
    float startFreq = 10.0f;        // A sweep kezdőfrekvenciája Hz (egy oktávval a kiértékelt 20 Hz alatt: a sávszél
                                    // be- és kifutása így nem torzítja a mély tartományt)
    float endFreq = 20000.0f;       // Végfrekvencia Hz
    int smoothingFraction = 6;      // 1/6 oktávos simítás alapértelmezetten (-1: változó simítás)
    float maxBoostDb = 3.0f;        // Legnagyobb megengedett kiemelés (összegzetten is, alapból +3 dB)
    float maxCutDb = -24.0f;        // Legnagyobb megengedett vágás
    int maxBands = 10;              // Parametrikus sávok száma
    std::string targetType = "room_balanced"; // "flat", "room_balanced"/"balanced", "room_warm"/"bass_boost"
    float targetBassDb = 4.0f;      // 20 Hz-es mély cél (dB, -6..+10)
    float targetTrebleDb = -3.0f;   // 20 kHz-es magas cél (dB, -6..+3)
    float targetKneeHz = 160.0f;    // Mély töréspont (Hz, 60..250)
    bool customTargetParams = false;// true: a targetBassDb / targetTrebleDb / targetKneeHz közvetlenül érvényes
    AutoEqLimits eqLimits{};        // Korrekciós biztonsági határok
    std::vector<FreqPoint> micCal;  // Opcionális mikrofon kalibrációs görbe
    float noiseRms = 0.0f;          // a háttérzaj RMS-e (a lejátszás előtti csendből); 0: nincs zajellenőrzés
};

struct MeasureResult {
    bool success = false;
    std::string errorCode = "";     // "MIC_TOO_QUIET", "MIC_CLIPPING", "NOISY_ROOM", "NO_INPUT_DEVICE"
    std::string errorMessage = "";
    // A hang érkezése a felvétel elejéhez képest (ms). Önmagában nem késleltetés-korrekció (benne van a hangkártya
    // és a rendszer saját késése is); csak két csatorna ugyanazzal az úttal mért értékeinek különbsége értelmes.
    float measuredDelayMs = 0.0f;
    // A csúcs előjele. A mikrofon / előerősítő polaritása ismeretlen, így ez is csak csatornák között hasonlítható.
    bool invertedPolarity = false;
    float snrDb = 0.0f;
    float peakLevelDbfs = -100.0f;
    float recommendedPreampDb = 0.0f;
    float speakerLowCutoffHz = 20.0f;
    std::vector<float> skippedDips;
    std::vector<FreqPoint> rawResponse;
    std::vector<FreqPoint> smoothedResponse;
    std::vector<FreqPoint> targetCurve;
    std::vector<AutoEqBand> correctionBands;
    std::string humanSummaryHu = "";
    std::string humanSummaryEn = "";
};

/**
 * Egységes szobai célgörbe generálása a motorban (log-frekvencián szakaszonként egyenes):
 *  - "flat": 0 dB mindenhol;
 *  - egyébként: 20 Hz -> targetBassDb, lineárisan (log f) 0 dB-ig a targetKneeHz töréspontig,
 *    targetKneeHz .. 1000 Hz között 0 dB, 1000 Hz .. 20 kHz között lineárisan (log f) targetTrebleDb-ig.
 */
inline std::vector<FreqPoint> buildTargetCurve(const std::vector<FreqPoint>& referenceCurve,
                                               const MeasureConfig& cfg) {
    std::vector<FreqPoint> out = referenceCurve;
    if (cfg.targetType == "flat") {
        for (auto& pt : out) pt.magDb = 0.0f;
        return out;
    }

    const float bassDb = cfg.customTargetParams
        ? std::clamp(cfg.targetBassDb, -6.0f, 10.0f)
        : ((cfg.targetType == "room_warm" || cfg.targetType == "bass_boost") ? 6.0f : 4.0f);
    const float trebleDb = cfg.customTargetParams
        ? std::clamp(cfg.targetTrebleDb, -6.0f, 3.0f)
        : -3.0f;
    const float kneeHz = std::clamp(cfg.targetKneeHz, 60.0f, 250.0f);

    const double log20 = std::log10(20.0);
    const double logKnee = std::log10(static_cast<double>(kneeHz));
    const double log1k = std::log10(1000.0);
    const double log20k = std::log10(20000.0);

    for (auto& pt : out) {
        const double f = pt.freq;
        double tg = 0.0;
        if (f <= 20.0) {
            tg = bassDb;
        } else if (f < kneeHz) {
            const double ratio = (logKnee - std::log10(f)) / std::max(1e-6, logKnee - log20);
            tg = bassDb * ratio;
        } else if (f <= 1000.0) {
            tg = 0.0;
        } else {
            const double ratio = std::min(1.0, (std::log10(f) - log1k) / (log20k - log1k));
            tg = trebleDb * ratio;
        }
        pt.magDb = static_cast<float>(tg);
    }
    return out;
}

class MeasurementManager {
public:
    using ProgressCallback = std::function<void(const std::string& stepId, float progress, const std::string& statusText)>;
    using CompleteCallback = std::function<void(const MeasureResult& result)>;

    MeasurementManager() : state_(MeasureState::Idle), stopRequested_(false) {}

    ~MeasurementManager() {
        stop();
    }

    /**
     * Mérés azonnali leállítása és állapot törlése.
     */
    void stop() {
        stopRequested_ = true;
        state_ = MeasureState::Stopped;
    }

    bool isRunning() const {
        return state_ != MeasureState::Idle && state_ != MeasureState::Complete && 
               state_ != MeasureState::Error && state_ != MeasureState::Stopped;
    }

    MeasureState state() const { return state_; }

    /**
     * Szintellenőrzés végrehajtása rögzített mintákon.
     * Ellenőrzi: jelszint megléte, túl halk jel, túlvezérlés (clipping), zajszint.
     */
    bool checkLevels(const std::vector<float>& testSignal, float noiseThresholdRms, MeasureResult& out) {
        if (testSignal.empty()) {
            out.success = false;
            out.errorCode = "NO_INPUT_DEVICE";
            out.errorMessage = "Nincs rögzített hangjel. Ellenőrizd a mikrofon csatlakozását.";
            return false;
        }

        // DC-eltolás levonása a szint- és zajszámítás előtt (egyes USB-mikrofonok DC-vel indulnak)
        double mean = 0.0;
        for (float s : testSignal) mean += static_cast<double>(s);
        mean /= static_cast<double>(testSignal.size());

        float maxPeak = 0.0f;
        double sumSq = 0.0;
        for (float s : testSignal) {
            const double ac = static_cast<double>(s) - mean;
            const float absS = static_cast<float>(std::fabs(ac));
            if (absS > maxPeak) maxPeak = absS;
            sumSq += ac * ac;
        }
        float rms = static_cast<float>(std::sqrt(sumSq / testSignal.size()));
        float peakDb = maxPeak > 1e-6f ? 20.0f * std::log10(maxPeak) : -120.0f;
        out.peakLevelDbfs = peakDb;

        // 1. Nincs jel (extrém alacsony)
        if (rms < 0.00005f) { // -86 dBFS
            out.success = false;
            out.errorCode = "NO_INPUT_DEVICE";
            out.errorMessage = "A mikrofonból nem érkezik jel. Kérjük válaszd ki a megfelelő bemenetet.";
            return false;
        }

        // 2. Túl halk jel
        if (peakDb < -45.0f) {
            out.success = false;
            out.errorCode = "MIC_TOO_QUIET";
            out.errorMessage = "A mikrofon jele túl halk. Növeld a mikrofon érzékenységét vagy a hangerőt.";
            return false;
        }

        // 3. Túlvezérlés (clipping veszély)
        if (peakDb > -0.5f) {
            out.success = false;
            out.errorCode = "MIC_CLIPPING";
            out.errorMessage = "A bemeneti jel túlvezérelt (clipping). Csökkentsd a mikrofon vagy a lejátszás szintjét!";
            return false;
        }

        // 4. Túl zajos környezet
        if (noiseThresholdRms > 0.0f && rms > 0.0f) {
            float snr = 20.0f * std::log10(std::max(1e-5f, rms / noiseThresholdRms));
            out.snrDb = snr;
            if (snr < 12.0f) {
                out.success = false;
                out.errorCode = "NOISY_ROOM";
                out.errorMessage = "A szoba háttérzaja túl magas a pontos méréshez. Kérjük csökkentsd a zajforrásokat.";
                return false;
            }
        }

        return true;
    }

    /**
     * Egy felvétel elemzése: szintek, dekonvolúció, frekvenciamenet, kalibráció, simítás, 0 dB-re igazítás.
     * success = true: az elemzés sikerült (javaslat még nincs: finalize).
     */
    MeasureResult analyzeRecording(
        const MeasureConfig& cfg,
        uint32_t sampleRate,
        const std::vector<float>& recordedSamples,
        ProgressCallback onProgress = nullptr
    ) {
        MeasureResult res;
        stopRequested_ = false;

        // 1. Biztonsági jelszint korlát érvényesítése (max -6 dBFS)
        float safeLevelDb = std::clamp(cfg.levelDb, -60.0f, -6.0f);
        float sweepAmp = static_cast<float>(std::pow(10.0, safeLevelDb / 20.0));

        // 2. Szintellenőrzés (LevelCheck)
        state_ = MeasureState::LevelCheck;
        if (onProgress) onProgress("level_check", 0.1f, "Bemeneti jelszint ellenőrzése...");

        if (recordedSamples.empty()) {
            res.success = false;
            res.errorCode = "NO_INPUT_DEVICE";
            res.errorMessage = "Nem érkezett felvett hangadat.";
            state_ = MeasureState::Error;
            return res;
        }

        // DC-eltávolítás a szintellenőrzés és dekonvolúció előtt
        std::vector<float> cleanSamples(recordedSamples.size());
        {
            double mean = 0.0;
            for (float s : recordedSamples) mean += static_cast<double>(s);
            mean /= static_cast<double>(recordedSamples.size());
            for (size_t i = 0; i < recordedSamples.size(); ++i) {
                cleanSamples[i] = static_cast<float>(static_cast<double>(recordedSamples[i]) - mean);
            }
        }

        if (!checkLevels(cleanSamples, cfg.noiseRms, res)) {
            state_ = MeasureState::Error;
            return res;
        }

        if (stopRequested_) { state_ = MeasureState::Stopped; return res; }

        // 3. Sweep generálás és inverz szűrő
        state_ = MeasureState::MeasuringSweep;
        if (onProgress) onProgress("sweep_playback", 0.3f, "Mérési sweep feldolgozása...");

        auto invFilter = generateInverseFilter(cfg.startFreq, cfg.endFreq, cfg.durationSec, sampleRate, sweepAmp);

        if (stopRequested_) { state_ = MeasureState::Stopped; return res; }

        // 4. Dekonvolúció (IR kinyerés): a felvétel és az inverz szűrő konvolúciója (FFT), ablakozva
        state_ = MeasureState::Deconvolving;
        if (onProgress) onProgress("deconvolution", 0.5f, "Impulzusválasz számítása...");

        FarinaResult dRes = farinaDeconvolve(cleanSamples, invFilter, sampleRate);
        if (!dRes.ok) {
            res.success = false;
            res.errorCode = "NO_SIGNAL";
            res.errorMessage = "A felvételben nem található a mérőjel.";
            state_ = MeasureState::Error;
            return res;
        }
        res.measuredDelayMs = static_cast<float>(dRes.arrivalSamples * 1000.0 / sampleRate);
        res.invertedPolarity = dRes.peakValue < 0.0f;
        // A jel-zaj viszony a felvétel lejátszás előtti csendjéből (checkLevels, noiseRms) megbízható; az IR csúcs
        // előtti szakaszában a sávszélek (20 Hz) utólengése is benne van, ezért az csak tartalék, enyhébb határral.
        if (cfg.noiseRms <= 0.0f) res.snrDb = dRes.snrDb;
        if (cfg.noiseRms <= 0.0f && dRes.snrDb < 10.0f) {
            res.success = false;
            res.errorCode = "NOISY_ROOM";
            res.errorMessage = "A mérőjel alig emelkedik ki a zajból. Növeld a hangerőt, vagy csökkentsd a zajt.";
            state_ = MeasureState::Error;
            return res;
        }

        if (stopRequested_) { state_ = MeasureState::Stopped; return res; }

        // 5. Frekvenciamenet és simítás (Analysis)
        state_ = MeasureState::Analyzing;
        if (onProgress) onProgress("analysis", 0.7f, "Frekvenciamenet simítása és kiértékelése...");

        res.rawResponse = calculateFrequencyResponse(dRes.ir, sampleRate, 240);
        // Csak a sweep tartományán belül (a kezdete fölött legalább egy oktávval) érvényes az adat
        const float evalLow = std::max(20.0f, cfg.startFreq * 2.0f);
        res.rawResponse.erase(std::remove_if(res.rawResponse.begin(), res.rawResponse.end(), [&](const FreqPoint& p) {
            return p.freq < evalLow || p.freq > cfg.endFreq;
        }), res.rawResponse.end());

        // Mikrofon kalibrációs görbe érvényesítése (ha megadták)
        if (!cfg.micCal.empty()) {
            applyMicCalibration(res.rawResponse, cfg.micCal);
        }

        // Törtoktávos simítás
        res.smoothedResponse = smoothSpectrum(res.rawResponse, cfg.smoothingFraction);

        // A szint önkényes (mikrofonerősítés, hangerő): a görbét a 300 Hz – 3 kHz átlagához igazítjuk (0 dB)
        {
            double sum = 0.0;
            int cnt = 0;
            for (const auto& p : res.smoothedResponse)
                if (p.freq >= 300.0f && p.freq <= 3000.0f) { sum += p.magDb; ++cnt; }
            const float ref = cnt > 0 ? static_cast<float>(sum / cnt) : 0.0f;
            for (auto& p : res.smoothedResponse) p.magDb -= ref;
            for (auto& p : res.rawResponse) p.magDb -= ref;
        }
        res.success = true;
        return res;
    }

    /**
     * Mérés feldolgozása átadott valódi mintákkal: elemzés (analyzeRecording) és javaslat (finalize).
     */
    MeasureResult processMeasurement(
        const MeasureConfig& cfg,
        uint32_t sampleRate,
        const std::vector<float>& recordedSamples,
        ProgressCallback onProgress = nullptr
    ) {
        MeasureResult res = analyzeRecording(cfg, sampleRate, recordedSamples, onProgress);
        if (res.success) finalize(cfg, res, onProgress);
        return res;
    }

    /**
     * A célgörbe, a korrekciós javaslat és az összefoglaló a (simított, 0 dB-re igazított) görbéből. Több mérés
     * (pl. bal és jobb) átlagára is hívható: ilyenkor a res.smoothedResponse az átlag.
     */
    void finalize(const MeasureConfig& cfg, MeasureResult& res, ProgressCallback onProgress = nullptr) {
        // 6. Célgörbe és korrekciós EQ szűrők számítása
        if (onProgress) onProgress("auto_eq", 0.85f, "Optimális akusztikai szűrők számítása...");

        res.targetCurve = buildTargetCurve(res.smoothedResponse, cfg);

        // Biztonsági korlátokkal szűrők számítása
        auto eqRes = calculateAutoEq(
            res.smoothedResponse,
            res.targetCurve,
            cfg.maxBands,
            cfg.maxBoostDb, // <= +3.0 dB (összegzetten is)
            cfg.maxCutDb,   // pl. -24.0 dB
            cfg.eqLimits
        );
        res.correctionBands = eqRes.bands;
        res.recommendedPreampDb = eqRes.targetPreampDb;
        res.speakerLowCutoffHz = eqRes.speakerLowCutoffHz;
        res.skippedDips = eqRes.skippedDips;

        // 7. Közérthető összefoglaló szöveg készítése (Egyszerű mód, Rule 0)
        generateHumanSummary(res);

        res.success = true;
        state_ = MeasureState::Complete;
        if (onProgress) onProgress("complete", 1.0f, "A mérés elkészült.");
    }

private:
    std::atomic<MeasureState> state_;
    std::atomic<bool> stopRequested_;

    void generateHumanSummary(MeasureResult& res) {
        std::ostringstream hu, en;

        // Csak azt írjuk, amit a mérés valóban alátámaszt: a késleltetést és a polaritást egyetlen mérés nem
        // mutatja meg (a hangkártya késése és a mikrofon polaritása ismeretlen) – ezt a csatornák összevetése adja.
        hu << "A mérés elkészült (jel-zaj viszony: " << static_cast<int>(std::round(res.snrDb)) << " dB).\n";
        en << "Measurement complete (signal-to-noise ratio: " << static_cast<int>(std::round(res.snrDb)) << " dB).\n";

        float maxCut = 0.0f;
        float maxCutFreq = 0.0f;
        float maxBoost = 0.0f;
        float maxBoostFreq = 0.0f;

        for (const auto& b : res.correctionBands) {
            if (b.gainDb < maxCut) {
                maxCut = b.gainDb;
                maxCutFreq = b.freq;
            }
            if (b.gainDb > maxBoost) {
                maxBoost = b.gainDb;
                maxBoostFreq = b.freq;
            }
        }

        if (maxCut < -2.0f) {
            hu << "• Javaslat: a " << static_cast<int>(std::round(maxCutFreq))
               << " Hz körüli szobai kiemelés csökkentése " << std::fixed << std::setprecision(1)
               << std::fabs(maxCut) << " dB-lel.\n";
            en << "• Proposed: reduce the room boost around " << static_cast<int>(std::round(maxCutFreq))
               << " Hz by " << std::fixed << std::setprecision(1) << std::fabs(maxCut) << " dB.\n";
        }

        if (maxBoost > 1.0f) {
            hu << "• Javaslat: a " << static_cast<int>(std::round(maxBoostFreq))
               << " Hz körüli mélyedés kíméletes emelése (+" << std::fixed << std::setprecision(1)
               << maxBoost << " dB, hangszóróvédelmi korláttal).\n";
            en << "• Proposed: gently lift the dip around " << static_cast<int>(std::round(maxBoostFreq))
               << " Hz (+" << std::fixed << std::setprecision(1) << maxBoost << " dB, speaker protection active).\n";
        }
        if (!res.skippedDips.empty()) {
            hu << "• A " << static_cast<int>(std::round(res.skippedDips.front()))
               << " Hz körüli mély kioltást kihagytuk – ezt EQ nem javítja; próbáld a hangszóró vagy a hallgatási hely mozgatását.\n";
            en << "• Skipped the deep dip around " << static_cast<int>(std::round(res.skippedDips.front()))
               << " Hz – EQ cannot fix this; try moving the speakers or the listening position.\n";
        }
        if (res.correctionBands.empty()) {
            hu << "• A mért görbe közel van a célhoz; korrekció nem szükséges.\n";
            en << "• The measured response is close to the target; no correction needed.\n";
        }
        hu << "A javaslat az „Alkalmazás” lépésben kapcsolható be, és egy kattintással visszavonható.";
        en << "Turn the proposal on in the “Apply” step; it can be undone with one click.";

        res.humanSummaryHu = hu.str();
        res.humanSummaryEn = en.str();
    }
};

} // namespace aqulify::room
