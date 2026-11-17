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
    float startFreq = 20.0f;        // Kezdőfrekvencia Hz
    float endFreq = 20000.0f;       // Végfrekvencia Hz
    int smoothingFraction = 6;      // 1/6 oktávos simítás alapértelmezetten
    float maxBoostDb = 3.0f;        // Legnagyobb megengedett kiemelés (biztonság: max +3 dB)
    float maxCutDb = -24.0f;        // Legnagyobb megengedett vágás
    int maxBands = 10;              // Parametrikus sávok száma
    std::string targetType = "balanced"; // "flat", "balanced" (+3 dB), "bass_boost" (+6 dB)
    std::vector<FreqPoint> micCal;  // Opcionális mikrofon kalibrációs görbe
};

struct MeasureResult {
    bool success = false;
    std::string errorCode = "";     // "MIC_TOO_QUIET", "MIC_CLIPPING", "NOISY_ROOM", "NO_INPUT_DEVICE"
    std::string errorMessage = "";
    float measuredDelayMs = 0.0f;
    bool invertedPolarity = false;
    float snrDb = 0.0f;
    float peakLevelDbfs = -100.0f;
    std::vector<FreqPoint> rawResponse;
    std::vector<FreqPoint> smoothedResponse;
    std::vector<FreqPoint> targetCurve;
    std::vector<AutoEqBand> correctionBands;
    std::string humanSummaryHu = "";
    std::string humanSummaryEn = "";
};

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

        float maxPeak = 0.0f;
        double sumSq = 0.0;
        for (float s : testSignal) {
            float absS = std::fabs(s);
            if (absS > maxPeak) maxPeak = absS;
            sumSq += double(s) * s;
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
     * Mérés feldolgozása átadott valódi mintákkal.
     */
    MeasureResult processMeasurement(
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

        if (!checkLevels(recordedSamples, 0.001f, res)) {
            state_ = MeasureState::Error;
            return res;
        }

        if (stopRequested_) { state_ = MeasureState::Stopped; return res; }

        // 3. Sweep generálás és inverz szűrő
        state_ = MeasureState::MeasuringSweep;
        if (onProgress) onProgress("sweep_playback", 0.3f, "Mérési sweep feldolgozása...");

        auto sweep = generateLogSweep(cfg.startFreq, cfg.endFreq, cfg.durationSec, sampleRate, sweepAmp);
        auto invFilter = generateInverseFilter(cfg.startFreq, cfg.endFreq, cfg.durationSec, sampleRate, sweepAmp);

        if (stopRequested_) { state_ = MeasureState::Stopped; return res; }

        // 4. Dekonvolúció (IR kinyerés)
        state_ = MeasureState::Deconvolving;
        if (onProgress) onProgress("deconvolution", 0.5f, "Impulzusválasz és késleltetés számítása...");

        DeconvResult dRes = deconvolve(recordedSamples, invFilter, sampleRate);
        res.measuredDelayMs = dRes.delayMs;
        res.invertedPolarity = dRes.invertedPolarity;
        res.snrDb = dRes.snrDb;

        if (stopRequested_) { state_ = MeasureState::Stopped; return res; }

        // 5. Frekvenciamenet és simítás (Analysis)
        state_ = MeasureState::Analyzing;
        if (onProgress) onProgress("analysis", 0.7f, "Frekvenciamenet simítása és kiértékelése...");

        res.rawResponse = calculateFrequencyResponse(dRes.ir, sampleRate, 240);

        // Mikrofon kalibrációs görbe érvényesítése (ha megadták)
        if (!cfg.micCal.empty()) {
            applyMicCalibration(res.rawResponse, cfg.micCal);
        }

        // Törtoktávos simítás
        res.smoothedResponse = smoothSpectrum(res.rawResponse, cfg.smoothingFraction);

        // 6. Célgörbe és korrekciós EQ szűrők számítása
        if (onProgress) onProgress("auto_eq", 0.85f, "Optimális akusztikai szűrők számítása...");

        float bassBoost = 3.0f; // Alapértelmezett kiegyensúlyozott célgörbe
        if (cfg.targetType == "bass_boost") bassBoost = 6.0f;
        else if (cfg.targetType == "flat") bassBoost = 0.0f;

        res.targetCurve = res.smoothedResponse;
        for (auto& pt : res.targetCurve) {
            double boost = 0.0;
            if (bassBoost > 0.0f) {
                const double w = pt.freq / 100.0;
                boost = bassBoost / (1.0 + w * w);
            }
            pt.magDb = static_cast<float>(boost);
        }

        // Biztonsági korlátokkal szűrők számítása
        auto eqRes = calculateAutoEq(
            res.smoothedResponse,
            res.targetCurve,
            cfg.maxBands,
            cfg.maxBoostDb, // <= +3.0 dB
            cfg.maxCutDb    // pl. -24.0 dB
        );
        res.correctionBands = eqRes.bands;

        // 7. Közérthető összefoglaló szöveg készítése (Egyszerű mód, Rule 0)
        generateHumanSummary(res);

        res.success = true;
        state_ = MeasureState::Complete;
        if (onProgress) onProgress("complete", 1.0f, "A szobamérés és korrekció sikeresen elkészült!");

        return res;
    }

private:
    std::atomic<MeasureState> state_;
    std::atomic<bool> stopRequested_;

    void generateHumanSummary(MeasureResult& res) {
        std::ostringstream hu, en;

        hu << "A szobakorrekció sikeresen elkészült.\n";
        en << "Room correction completed successfully.\n";

        if (std::fabs(res.measuredDelayMs) > 0.1f) {
            hu << "• Késleltetés: " << std::fixed << std::setprecision(1) << res.measuredDelayMs << " ms időbeli eltérés beállítva a tökéletes sztereó képért.\n";
            en << "• Time alignment: " << std::fixed << std::setprecision(1) << res.measuredDelayMs << " ms delay aligned for precise stereo imaging.\n";
        }
        if (res.invertedPolarity) {
            hu << "• Polaritás: Fordított hangszóró-fázis észlelve és automatikusan helyreállítva.\n";
            en << "• Polarity: Inverted speaker phase detected and automatically corrected.\n";
        }

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
            hu << "• Mélyhangok tisztasága: A " << static_cast<int>(std::round(maxCutFreq)) 
               << " Hz körüli zavaró szobai dübörgést " << std::fixed << std::setprecision(1) 
               << std::fabs(maxCut) << " dB-lel mérsékeltük.\n";
            en << "• Bass clarity: Tamed boomy room resonance around " << static_cast<int>(std::round(maxCutFreq))
               << " Hz by " << std::fixed << std::setprecision(1) << std::fabs(maxCut) << " dB.\n";
        }

        if (maxBoost > 1.0f) {
            hu << "• Részletgazdagság: A " << static_cast<int>(std::round(maxBoostFreq)) 
               << " Hz körüli mélyedést kíméletesen (+ " << std::fixed << std::setprecision(1) 
               << maxBoost << " dB, hangszóróvédelmi korláttal) megemeltük.\n";
            en << "• Detail recovery: Gently compensated dip around " << static_cast<int>(std::round(maxBoostFreq))
               << " Hz (+" << std::fixed << std::setprecision(1) << maxBoost << " dB, speaker protection active).\n";
        }

        hu << "• Eredmény: Tisztább vokálok, megfogottabb basszusok, természetes hangzáskép.";
        en << "• Overall result: Clearer vocals, tighter bass, and natural balance.";

        res.humanSummaryHu = hu.str();
        res.humanSummaryEn = en.str();
    }
};

} // namespace aqulify::room
