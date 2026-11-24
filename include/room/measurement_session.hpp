#pragma once
/**
 * @file measurement_session.hpp
 * @brief A szobamérés lefolytatása a motorban: mérőjel lejátszása a motor saját lejátszóján, közben felvétel a
 *        mikrofonról, majd kiértékelés (measurement_manager.hpp).
 *
 * Menete (startMeasurement):
 *  1. a gépen szóló zene szünetel; a mikrofon felvétele indul (a lejátszás előtti rövid csendből a háttérzaj mérhető);
 *  2. a DSP-lánc mérési módba kerül (bypassChain(true): az EQ, a konvolver, a hangszínező modulok és a kimeneti
 *     késleltetés ki; a keresztváltó és a limiter marad – a hangszórók védelme);
 *  3. a mérőjel (log sweep egy ideiglenes WAV fájlból) a meglévő lejátszón szól, a választott csatorná(ko)n;
 *  4. a lejátszás és a felvétel leáll, a lánc visszaáll (bypassChain(false)) – hiba és leállítás esetén is;
 *  5. a felvétel kiértékelése a háttérszálon; az eredmény JSON-üzenetként a post-on át a felületre megy.
 *
 * Szintellenőrzés (startLevelCheck): rózsaszín zaj a mérés szintjén, közben a mikrofon szintje 100 ms-onként
 * ("mic_level"); legfeljebb 15 s, vagy amíg a felület le nem állítja.
 *
 * Minden visszahívás a munkaszálról hívódik: a post-nak szálbiztosnak kell lennie (ws_bridge: postBroadcast).
 * Platformfüggő rész nincs: a mikrofont a makeMic adja (mic_capture.hpp), a lejátszást a playFile.
 */
#include "measurement_manager.hpp"
#include "mic_capture.hpp"
#include "sweep.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace aqulify::room {

struct SessionDeps {
    std::function<void(bool on)> bypassChain;                                  // mérési mód be / ki
    std::function<bool(const std::string& path, std::string& error)> playFile; // a motor lejátszója
    std::function<void()> stopPlayback;
    std::function<void()> pauseMedia;                                          // a szóló zene szüneteltetése
    std::function<std::unique_ptr<MicCapture>()> makeMic;
    std::function<void(const std::string& json)> post;                         // szálbiztos küldés a felületre
    std::string tempDir;                                                       // üres: a rendszer ideiglenes mappája
};

/** JSON-szöveg idézőjelek közé, a szükséges escape-ekkel. */
inline std::string jsonStr(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
                else o += static_cast<char>(c);
        }
    }
    return o + "\"";
}

/** 32 bites lebegőpontos, sztereó WAV írása. */
inline bool writeStereoFloatWav(const std::string& path, const std::vector<float>& interleaved, uint32_t sr) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    const uint32_t dataBytes = static_cast<uint32_t>(interleaved.size() * sizeof(float));
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4); u32(36 + dataBytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(3); u16(2); u32(sr); u32(sr * 8); u16(8); u16(32);
    f.write("data", 4); u32(dataBytes);
    f.write(reinterpret_cast<const char*>(interleaved.data()), dataBytes);
    return static_cast<bool>(f);
}

class MeasurementSession {
public:
    static constexpr uint32_t kRate = 48000;
    static constexpr double kLeadSec = 0.5;        // csend a mérőjel előtt (a lejátszás indulásához)
    static constexpr double kTailSec = 1.5;        // a mérőjel utáni felvétel (lecsengés + rendszerkésés)
    static constexpr double kStartTrimSec = 0.20;  // az USB-mikrofon indítási kattanásának eldobása (200 ms)
    static constexpr double kNoiseWindowSec = 0.20;// a háttérzaj a lejátszás előtti utolsó 200 ms-ból

    explicit MeasurementSession(SessionDeps d) : d_(std::move(d)) {}
    ~MeasurementSession() { stop(); join(); }
    MeasurementSession(const MeasurementSession&) = delete;
    MeasurementSession& operator=(const MeasurementSession&) = delete;

    bool busy() const { return busy_.load(); }

    /** {"type":"audio_devices","inputs":[{id,name,default}]} – a monitorok nélkül. */
    std::string listInputsJson() {
        std::ostringstream o;
        o << "{\"type\":\"audio_devices\",\"inputs\":[";
        auto mic = d_.makeMic ? d_.makeMic() : nullptr;
        bool first = true;
        if (mic) {
            for (const auto& in : mic->listInputs(false)) {
                if (!first) o << ',';
                first = false;
                o << "{\"id\":" << jsonStr(in.id) << ",\"name\":" << jsonStr(in.name)
                  << ",\"default\":" << (in.isDefault ? "true" : "false") << '}';
            }
        }
        o << "],\"available\":" << (mic ? "true" : "false") << '}';
        return o.str();
    }

    /** Mérés indítása a háttérben; false: már fut egy mérés (error = "BUSY"). append=true: hozzáfűzi a pontokhoz. */
    bool startMeasurement(const MeasureConfig& cfg, const std::string& device, std::string& error, bool append = false) {
        if (!begin(error)) return false;
        worker_ = std::thread([this, cfg, device, append] { runMeasurement(cfg, device, append); finish(); });
        return true;
    }

    /** A tárolt mérési pontok teljesítményátlagának, célgörbéjének és EQ-sávjainak újraszámítása. */
    bool recompute(const MeasureConfig& cfg, std::string& error) {
        if (busy()) { error = "BUSY"; return false; }
        if (!recomputeAndPost(cfg, true, nullptr)) { error = "NO_POINTS"; return false; }
        return true;
    }

    /** Egy rossz mérési pont törlése ID alapján; ha marad pont, újraszámol és küldi az új eredményt. */
    bool deletePoint(int pointId, const MeasureConfig& cfg, std::string& error) {
        if (busy()) { error = "BUSY"; return false; }
        bool found = false;
        bool emptyAfter = false;
        {
            std::lock_guard<std::mutex> lk(pointsMu_);
            auto it = std::remove_if(points_.begin(), points_.end(),
                                     [pointId](const StoredPoint& p) { return p.id == pointId; });
            if (it != points_.end()) {
                points_.erase(it, points_.end());
                found = true;
            }
            emptyAfter = points_.empty();
        }
        if (!found) { error = "NOT_FOUND"; return false; }
        if (emptyAfter) {
            post("{\"type\":\"measurement_points_updated\",\"point_count\":0,\"points\":[]}");
            return true;
        }
        recomputeAndPost(cfg, true, nullptr);
        return true;
    }

    /** Összes tárolt mérési pont törlése (új munkamenet indításához). */
    void clearPoints() {
        std::lock_guard<std::mutex> lk(pointsMu_);
        points_.clear();
    }

    size_t pointCount() const {
        std::lock_guard<std::mutex> lk(pointsMu_);
        return points_.size();
    }

    /** Szintellenőrzés: rózsaszín zaj a megadott szinten és csatornán, közben "mic_level" üzenetek. */
    bool startLevelCheck(const std::string& device, float levelDb, const std::string& channel, std::string& error) {
        if (!begin(error)) return false;
        worker_ = std::thread([this, device, levelDb, channel] { runLevelCheck(device, levelDb, channel); finish(); });
        return true;
    }

    /** Leállítás (a munkaszál a lánc visszaállítása után ér véget). */
    void stop() { stopReq_ = true; }

private:
    struct LrCompare { bool valid = false; double delayMs = 0.0; bool polarityDiffers = false; };
    struct StoredPoint {
        int id = 0;
        std::string channel;
        std::vector<FreqPoint> rawResponse;
        std::vector<FreqPoint> rawLeft;
        std::vector<FreqPoint> rawRight;
        float snrDb = 0.0f;
        float peakLevelDbfs = -100.0f;
        LrCompare lr{};
    };

    SessionDeps d_;
    std::thread worker_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> stopReq_{false};
    mutable std::mutex pointsMu_;
    std::vector<StoredPoint> points_;
    int nextPointId_ = 1;

    bool begin(std::string& error) {
        bool expected = false;
        if (!busy_.compare_exchange_strong(expected, true)) { error = "BUSY"; return false; }
        join();
        stopReq_ = false;
        return true;
    }
    void finish() { busy_ = false; }
    void join() { if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) worker_.join(); }

    void post(const std::string& s) { if (d_.post) d_.post(s); }

    void postError(const std::string& step, const std::string& code, const std::string& msg) {
        post("{\"type\":\"measurement_error\",\"step_id\":" + jsonStr(step) + ",\"error\":" + jsonStr(code) +
             ",\"message\":" + jsonStr(msg) + "}");
    }

    std::string tempPath(const char* name) const {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path dir = d_.tempDir.empty() ? fs::temp_directory_path(ec) : fs::path(d_.tempDir);
        return (dir / name).string();
    }

    static bool wantL(const std::string& ch) { return ch != "right"; }
    static bool wantR(const std::string& ch) { return ch != "left"; }

    /** A mikrofonhiba-kódok közérthető magyarázata (a felület a kód szerint fordít). */
    static std::string micErrorText(const std::string& code) {
        if (code == "no_device") return "A kiválasztott mikrofon nem található.";
        if (code == "busy") return "A mikrofont egy másik program használja.";
        if (code == "permission") return "A mikrofon használata nincs engedélyezve (rendszerbeállítások / adatvédelem).";
        if (code == "format") return "A mikrofon nem tud 48 kHz-en felvenni.";
        return "A mikrofon nem indítható.";
    }

    /**
     * Közös rész: felvétel indul, kis csend (háttérzaj), mérési mód, lejátszás; onTick 100 ms-onként (false: vége).
     * Visszaadja a felvételt és a lejátszás indulásakor felvett minták számát; hiba esetén üres.
     */
    bool playAndRecord(const std::string& device, const std::string& wavPath, double recordSec,
                       const std::function<bool(double elapsed, float peak)>& onTick,
                       std::vector<float>& recorded, size_t& framesAtPlay, std::string& step,
                       int preSilentMs = 450) {
        if (d_.pauseMedia) d_.pauseMedia();
        auto mic = d_.makeMic ? d_.makeMic() : nullptr;
        if (!mic) {
            postError("microphone", "NO_INPUT_DEVICE", "Ezen a gépen a mikrofonfelvétel nem érhető el.");
            return false;
        }
        std::string err;
        if (!mic->start(device == "default" ? std::string() : device, kRate, recordSec + 3.0, err)) {
            postError("microphone", "MIC_" + err, micErrorText(err));
            return false;
        }
        // Indítási stabilizálódás + háttérzaj (mérésnél 450 ms, szintpróbánál 200 ms)
        if (preSilentMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(preSilentMs));
        mic->takePeak();
        if (d_.bypassChain) d_.bypassChain(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(120));   // a paraméterek érvénybe lépése
        framesAtPlay = mic->recordedFrames();
        std::string perr;
        const bool played = d_.playFile && d_.playFile(wavPath, perr);
        bool ok = played;
        if (!played) postError("playback", "PLAYBACK_FAILED", "A mérőjel nem játszható le: " + perr);
        const auto t0 = std::chrono::steady_clock::now();
        while (ok) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (stopReq_) { ok = false; break; }
            const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (!mic->running()) {
                postError("microphone", "MIC_STOPPED", "A mikrofon felvétele megszakadt.");
                ok = false;
                break;
            }
            if (!onTick(el, mic->takePeak())) break;
        }
        if (d_.stopPlayback) d_.stopPlayback();
        if (d_.bypassChain) d_.bypassChain(false);
        recorded = mic->stop();
        step = ok ? "done" : (stopReq_ ? "stopped" : "error");
        if (stopReq_) post("{\"type\":\"measurement_stopped\"}");
        return ok;
    }

    /**
     * Mérés. "left" / "right": egy sweep azon a csatornán. "both": egy felvételben előbb a bal, kGapSec szünet után
     * a jobb csatorna sweepje – a két sweep távolsága a fájlban mintapontos, így a bal–jobb érkezési különbség és
     * polaritás összevethető (két külön mérésnél a lejátszás indulásának ütemezése ezt elrontaná). Pontosság: a
     * mikrofon és a hangkártya órájának eltérése (tipikusan 20–100 ppm) a két sweep kezdete közti ~(T + 1) s alatt
     * legfeljebb néhány tized ms.
     */
    static constexpr double kGapSec = 1.0;

    void runMeasurement(MeasureConfig cfg, const std::string& device, bool append) {
        const float amp = static_cast<float>(std::pow(10.0, std::clamp(cfg.levelDb, -60.0f, -6.0f) / 20.0));
        const auto sweep = generateLogSweep(cfg.startFreq, cfg.endFreq, cfg.durationSec, kRate, amp);
        const bool both = cfg.channel != "left" && cfg.channel != "right";
        const size_t lead = static_cast<size_t>(kLeadSec * kRate);
        const size_t gap = static_cast<size_t>(kGapSec * kRate);
        const size_t secondStart = lead + sweep.size() + gap;          // a jobb sweep kezdete a fájlban ("both")
        const size_t signalEnd = both ? secondStart + sweep.size() : lead + sweep.size();
        // A fájl hosszabb, mint a felvétel: a lejátszást a felvétel végén mi állítjuk le (nincs fájlvége-esemény)
        const size_t total = signalEnd + static_cast<size_t>((kTailSec + 3.0) * kRate);
        std::vector<float> st(total * 2, 0.0f);
        for (size_t i = 0; i < sweep.size(); ++i) {
            if (both) {
                st[(lead + i) * 2] = sweep[i];
                st[(secondStart + i) * 2 + 1] = sweep[i];
            } else {
                st[(lead + i) * 2 + (cfg.channel == "right" ? 1 : 0)] = sweep[i];
            }
        }
        const std::string wav = tempPath("aqulify-meresi-jel.wav");
        if (!writeStereoFloatWav(wav, st, kRate)) {
            postError("preparing", "TEMP_FILE", "Az ideiglenes mérőjel-fájl nem írható.");
            return;
        }
        const double recordSec = static_cast<double>(signalEnd) / kRate + kTailSec;
        std::vector<float> rec;
        size_t atPlay = 0;
        std::string step;
        const bool ok = playAndRecord(device, wav, recordSec, [&](double el, float peak) {
            const float pr = static_cast<float>(std::min(1.0, el / recordSec));
            std::ostringstream o;
            o << "{\"type\":\"measurement_progress\",\"step_id\":\"sweep_playback\",\"progress\":" << (0.05f + 0.4f * pr)
              << ",\"level_db\":" << peakDbfs(peak) << ",\"status\":" << jsonStr("Mérőjel lejátszása és felvétele...") << "}";
            post(o.str());
            return el < recordSec;
        }, rec, atPlay, step);
        std::error_code ec;
        std::filesystem::remove(wav, ec);
        if (!ok) return;

        // Induláskori mikrofon-kattanás eldobása (első ~200 ms)
        const size_t trim = static_cast<size_t>(kStartTrimSec * kRate);
        if (rec.size() > trim + kRate / 2) {
            rec.erase(rec.begin(), rec.begin() + static_cast<std::ptrdiff_t>(trim));
            atPlay = (atPlay > trim) ? (atPlay - trim) : 0;
        }

        // Háttérzaj a lejátszás előtti utolsó ~200 ms-ból, DC-eltávolítással
        const size_t nn = std::min(atPlay, rec.size());
        const size_t noiseWin = static_cast<size_t>(kNoiseWindowSec * kRate);
        const size_t nStart = nn > noiseWin ? (nn - noiseWin) : 0;
        const size_t nCount = nn - nStart;
        if (nCount > 0) {
            double mean = 0.0;
            for (size_t i = nStart; i < nn; ++i) mean += rec[i];
            mean /= static_cast<double>(nCount);
            double ns = 0.0;
            for (size_t i = nStart; i < nn; ++i) {
                const double d = static_cast<double>(rec[i]) - mean;
                ns += d * d;
            }
            cfg.noiseRms = static_cast<float>(std::sqrt(ns / static_cast<double>(nCount)));
        } else {
            cfg.noiseRms = 0.0f;
        }
        cfg.durationSec = static_cast<float>(sweep.size()) / kRate;

        MeasurementManager mgr;
        auto progress = [&](const std::string& s, float p, const std::string& txt) {
            std::ostringstream o;
            o << "{\"type\":\"measurement_progress\",\"step_id\":" << jsonStr(s) << ",\"progress\":" << (0.45f + 0.5f * p)
              << ",\"status\":" << jsonStr(txt) << "}";
            post(o.str());
        };

        StoredPoint pt;
        pt.channel = cfg.channel;
        if (!both) {
            MeasureResult res = mgr.analyzeRecording(cfg, kRate, rec, progress);
            if (!res.success) {
                postError("analysis", res.errorCode.empty() ? "ANALYSIS_FAILED" : res.errorCode, res.errorMessage);
                return;
            }
            pt.rawResponse = std::move(res.rawResponse);
            pt.snrDb = res.snrDb;
            pt.peakLevelDbfs = res.peakLevelDbfs;
        } else {
            // "both": a felvétel kettévágása a két sweep közti szünet közepénél (a rendszerkésés < 0,5 s)
            const size_t split = std::min(rec.size(), atPlay + lead + sweep.size() + gap / 2);
            std::vector<float> recL(rec.begin(), rec.begin() + static_cast<std::ptrdiff_t>(split));
            std::vector<float> recR(rec.begin() + static_cast<std::ptrdiff_t>(split), rec.end());
            MeasureResult rl = mgr.analyzeRecording(cfg, kRate, recL, progress);
            if (!rl.success) {
                postError("analysis", rl.errorCode.empty() ? "ANALYSIS_FAILED" : rl.errorCode, "Bal csatorna: " + rl.errorMessage);
                return;
            }
            MeasureConfig cr = cfg;
            cr.noiseRms = 0.0f;   // a jobb szakasz elején a bal lecsengése szól: a zajszint a bal elejéről már ismert
            MeasureResult rr = mgr.analyzeRecording(cr, kRate, recR, progress);
            if (!rr.success) {
                postError("analysis", rr.errorCode.empty() ? "ANALYSIS_FAILED" : rr.errorCode, "Jobb csatorna: " + rr.errorMessage);
                return;
            }
            // Érkezési különbség: a jobb a fájlban (secondStart − lead) mintával később indult
            const double arrL = rl.measuredDelayMs;
            const double arrR = rr.measuredDelayMs + (static_cast<double>(split) - static_cast<double>(secondStart - lead)) * 1000.0 / kRate;
            pt.lr = LrCompare{ true, arrR - arrL, rl.invertedPolarity != rr.invertedPolarity };
            // A bal–jobb relatív szintkülönbséget megőrizzük a tárolt pontban, hogy többpontos átlag után
            // pontosan kiszámítható legyen a lr_level_diff_db, és az egyes csatornák külön normalizálhatók legyenek.
            const float commonMid = 0.5f * (rl.midbandLevelDb + rr.midbandLevelDb);
            const float offL = rl.midbandLevelDb - commonMid;
            const float offR = rr.midbandLevelDb - commonMid;
            pt.rawLeft = std::move(rl.rawResponse);
            pt.rawRight = std::move(rr.rawResponse);
            for (auto& p : pt.rawLeft) p.magDb += offL;
            for (auto& p : pt.rawRight) p.magDb += offR;
            // Bal + jobb egyesítése teljesítményátlaggal (energiában)
            pt.rawResponse = powerAveragePositions({ pt.rawLeft, pt.rawRight });
            pt.snrDb = std::min(rl.snrDb, rr.snrDb);
            pt.peakLevelDbfs = std::max(rl.peakLevelDbfs, rr.peakLevelDbfs);
        }

        {
            std::lock_guard<std::mutex> lk(pointsMu_);
            if (!append) points_.clear();
            pt.id = nextPointId_++;
            points_.push_back(std::move(pt));
        }
        recomputeAndPost(cfg, false, progress);
    }

    /** A tárolt pontok simítása, teljesítményátlagolása, 0 dB-re igazítása és kiértékelése (finalize). */
    bool recomputeAndPost(const MeasureConfig& cfg, bool isRecompute,
                          MeasurementManager::ProgressCallback progress) {
        std::vector<StoredPoint> pts;
        {
            std::lock_guard<std::mutex> lk(pointsMu_);
            if (points_.empty()) return false;
            pts = points_;
        }

        std::vector<std::vector<FreqPoint>> smoothedAll;
        std::vector<std::vector<FreqPoint>> smoothedL, smoothedR;
        smoothedAll.reserve(pts.size());
        float minSnr = pts[0].snrDb;
        float maxPeak = pts[0].peakLevelDbfs;
        bool allHaveLr = true;
        double sumLrDelay = 0.0;
        bool anyPolarityDiff = false;

        for (const auto& pt : pts) {
            smoothedAll.push_back(smoothSpectrum(pt.rawResponse, cfg.smoothingFraction));
            if (!pt.rawLeft.empty() && !pt.rawRight.empty()) {
                smoothedL.push_back(smoothSpectrum(pt.rawLeft, cfg.smoothingFraction));
                smoothedR.push_back(smoothSpectrum(pt.rawRight, cfg.smoothingFraction));
            }
            minSnr = std::min(minSnr, pt.snrDb);
            maxPeak = std::max(maxPeak, pt.peakLevelDbfs);
            if (pt.lr.valid) {
                sumLrDelay += pt.lr.delayMs;
                anyPolarityDiff = anyPolarityDiff || pt.lr.polarityDiffers;
            } else {
                allHaveLr = false;
            }
        }

        MeasureResult avg;
        avg.smoothedResponse = powerAveragePositions(smoothedAll);
        avg.snrDb = minSnr;
        avg.peakLevelDbfs = maxPeak;

        // A 300 Hz – 3 kHz középtartomány 0 dB-re igazítása (többpontos átlag után is pontos)
        const float refDb = midbandMeanDb(avg.smoothedResponse);
        for (auto& p : avg.smoothedResponse) p.magDb -= refDb;

        MeasureResult avgL, avgR;
        const MeasureResult* pL = nullptr;
        const MeasureResult* pR = nullptr;
        float lrLevelDiffDb = 0.0f;
        if (smoothedL.size() == pts.size() && smoothedR.size() == pts.size()) {
            avgL.smoothedResponse = powerAveragePositions(smoothedL);
            avgR.smoothedResponse = powerAveragePositions(smoothedR);
            avgL.snrDb = minSnr;
            avgL.peakLevelDbfs = maxPeak;
            avgR.snrDb = minSnr;
            avgR.peakLevelDbfs = maxPeak;
            // Csatornánkénti normalizálás a saját 300 Hz – 3 kHz átlaghoz, hogy az EQ ne egyenlítse ki a szinteltérést
            const float refL = midbandMeanDb(avgL.smoothedResponse);
            const float refR = midbandMeanDb(avgR.smoothedResponse);
            lrLevelDiffDb = refR - refL;
            for (auto& p : avgL.smoothedResponse) p.magDb -= refL;
            for (auto& p : avgR.smoothedResponse) p.magDb -= refR;
            pL = &avgL;
            pR = &avgR;
        }

        LrCompare lr{};
        const LrCompare* pLr = nullptr;
        if (allHaveLr && !pts.empty()) {
            lr.valid = true;
            lr.delayMs = sumLrDelay / static_cast<double>(pts.size());
            lr.polarityDiffers = anyPolarityDiff;
            pLr = &lr;
        }

        MeasurementManager mgr;
        MeasureConfig effCfg = cfg;
        if (effCfg.channel.empty()) effCfg.channel = pts.back().channel;
        mgr.finalize(effCfg, avg, progress);
        if (pL && pR) {
            mgr.finalize(effCfg, avgL, nullptr);
            mgr.finalize(effCfg, avgR, nullptr);
            // Közös előerősítés a két csatornára: a kettő közül a negatívabb, hogy az L/R egyensúly ne tolódjon el
            avg.recommendedPreampDb = std::min(avgL.recommendedPreampDb, avgR.recommendedPreampDb);
        }
        post(resultJson(effCfg, avg, pLr, pL, pR, lrLevelDiffDb, pts, isRecompute));
        return true;
    }

    void runLevelCheck(const std::string& device, float levelDb, const std::string& channel) {
        const double secs = 15.0;
        const float amp = static_cast<float>(std::pow(10.0, std::clamp(levelDb, -60.0f, -6.0f) / 20.0));
        // Rózsaszín zaj (Paul Kellet-féle szűrő), a csúcs a mérés szintjére normálva
        std::mt19937 rng(12345);
        std::uniform_real_distribution<float> u(-1.0f, 1.0f);
        const size_t n = static_cast<size_t>((secs + 3.0) * kRate);
        std::vector<float> pink(n);
        float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0, pk = 0;
        for (size_t i = 0; i < n; ++i) {
            const float w = u(rng);
            b0 = 0.99886f * b0 + w * 0.0555179f; b1 = 0.99332f * b1 + w * 0.0750759f;
            b2 = 0.96900f * b2 + w * 0.1538520f; b3 = 0.86650f * b3 + w * 0.3104856f;
            b4 = 0.55000f * b4 + w * 0.5329522f; b5 = -0.7616f * b5 - w * 0.0168980f;
            pink[i] = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362f;
            b6 = w * 0.115926f;
            pk = std::max(pk, std::fabs(pink[i]));
        }
        const size_t fade = kRate / 10;
        // 2,2 s .. 4,2 s között −6 dB lépcső (20 ms átmenettel) az automatikus erősítés (AGC) észlelésére
        const size_t stepStart = static_cast<size_t>(2.2 * kRate);
        const size_t stepEnd = static_cast<size_t>(4.2 * kRate);
        const size_t ramp = kRate / 50; // 20 ms
        const float halfGain = 0.5011872f; // -6.0 dB
        std::vector<float> st(n * 2, 0.0f);
        for (size_t i = 0; i < n; ++i) {
            float g = 1.0f;
            if (i >= stepStart && i < stepEnd) {
                if (i < stepStart + ramp) {
                    const float t = static_cast<float>(i - stepStart) / ramp;
                    g = 1.0f - (1.0f - halfGain) * t;
                } else if (i + ramp > stepEnd) {
                    const float t = static_cast<float>(stepEnd - i) / ramp;
                    g = 1.0f - (1.0f - halfGain) * t;
                } else {
                    g = halfGain;
                }
            }
            float v = pink[i] / pk * amp * g;
            if (i < fade) v *= static_cast<float>(i) / fade;
            if (wantL(channel)) st[i * 2] = v;
            if (wantR(channel)) st[i * 2 + 1] = v;
        }
        const std::string wav = tempPath("aqulify-szintproba.wav");
        if (!writeStereoFloatWav(wav, st, kRate)) {
            postError("level_check", "TEMP_FILE", "Az ideiglenes tesztjel-fájl nem írható.");
            return;
        }
        std::vector<float> rec;
        size_t atPlay = 0;
        std::string step;
        float hold = 0.0f;
        std::vector<float> hiDbs, loDbs;
        bool agcChecked = false;
        bool agcWarning = false;
        float measuredDropDb = 6.0f;
        auto medianOf = [](std::vector<float>& v) -> float {
            std::sort(v.begin(), v.end());
            return v[v.size() / 2];
        };
        playAndRecord(device, wav, secs, [&](double el, float peak) {
            hold = std::max(peak, hold * 0.7f);
            const float curDb = peakDbfs(peak);
            if (el >= 0.6 && el < 2.0) hiDbs.push_back(curDb);
            else if (el >= 2.8 && el < 4.0) loDbs.push_back(curDb);
            if (!agcChecked && el >= 4.0 && hiDbs.size() >= 5 && loDbs.size() >= 5) {
                agcChecked = true;
                const float hiMed = medianOf(hiDbs);
                const float loMed = medianOf(loDbs);
                if (hiMed > -45.0f) {
                    measuredDropDb = hiMed - loMed;
                    if (std::fabs(measuredDropDb - 6.0f) > 1.5f) {
                        agcWarning = true;
                    }
                }
            }
            std::ostringstream o;
            o << "{\"type\":\"mic_level\",\"peak_db\":" << curDb << ",\"hold_db\":" << peakDbfs(hold)
              << ",\"remaining\":" << std::max(0.0, secs - el);
            if (agcWarning) o << ",\"agc_warning\":true,\"agc_drop_db\":" << measuredDropDb;
            o << "}";
            post(o.str());
            return el < secs;
        }, rec, atPlay, step, 200);
        std::error_code ec;
        std::filesystem::remove(wav, ec);
        if (step == "done") post("{\"type\":\"mic_level_done\"}");
    }

    static void appendCurve(std::ostringstream& o, const char* key, const std::vector<FreqPoint>& c) {
        o << ",\"" << key << "\":[";
        for (size_t i = 0; i < c.size(); ++i) o << (i ? "," : "") << c[i].magDb;
        o << ']';
    }

    static void appendBands(std::ostringstream& o, const char* key, const std::vector<AutoEqBand>& bs) {
        o << ",\"" << key << "\":[";
        for (size_t i = 0; i < bs.size(); ++i) {
            const auto& b = bs[i];
            o << (i ? "," : "") << "{\"type\":" << b.type << ",\"freq\":" << b.freq
              << ",\"gainDb\":" << b.gainDb << ",\"q\":" << b.q << '}';
        }
        o << ']';
    }

    static void appendFloats(std::ostringstream& o, const char* key, const std::vector<float>& vs) {
        o << ",\"" << key << "\":[";
        for (size_t i = 0; i < vs.size(); ++i) o << (i ? "," : "") << vs[i];
        o << ']';
    }

    /**
     * Az eredmény JSON-ja a felület formátumában (freqs, measured_dbs, target_dbs, bands, ...). "both" mérésnél a
     * két csatorna görbéje és csatornánkénti sávjai is (measured_left_dbs / measured_right_dbs, bands_left / bands_right,
     * lr_level_diff_db) és a bal–jobb összevetés (lr_*).
     */
    std::string resultJson(const MeasureConfig& cfg, const MeasureResult& r, const LrCompare* lr,
                           const MeasureResult* rl, const MeasureResult* rr, float lrLevelDiffDb,
                           const std::vector<StoredPoint>& pts, bool isRecompute) {
        std::ostringstream o;
        o.setf(std::ios::fixed);
        o.precision(2);
        o << "{\"type\":\"measurement_result\",\"channel\":" << jsonStr(cfg.channel) << ",\"snr_db\":" << r.snrDb
          << ",\"peak_db\":" << r.peakLevelDbfs
          << ",\"recommended_preamp_db\":" << r.recommendedPreampDb
          << ",\"speaker_low_cutoff_hz\":" << r.speakerLowCutoffHz
          << ",\"point_count\":" << pts.size()
          << ",\"recomputed\":" << (isRecompute ? "true" : "false")
          << ",\"points\":[";
        for (size_t i = 0; i < pts.size(); ++i) {
            o << (i ? "," : "") << "{\"id\":" << pts[i].id << ",\"snr_db\":" << pts[i].snrDb
              << ",\"peak_db\":" << pts[i].peakLevelDbfs << '}';
        }
        o << ']';
        appendFloats(o, "skipped_dips", r.skippedDips);
        o << ",\"freqs\":[";
        for (size_t i = 0; i < r.smoothedResponse.size(); ++i) o << (i ? "," : "") << r.smoothedResponse[i].freq;
        o << ']';
        appendCurve(o, "measured_dbs", r.smoothedResponse);
        appendCurve(o, "target_dbs", r.targetCurve);
        if (rl && rr) {
            appendCurve(o, "measured_left_dbs", rl->smoothedResponse);
            appendCurve(o, "measured_right_dbs", rr->smoothedResponse);
            appendBands(o, "bands_left", rl->correctionBands);
            appendBands(o, "bands_right", rr->correctionBands);
            appendFloats(o, "skipped_dips_left", rl->skippedDips);
            appendFloats(o, "skipped_dips_right", rr->skippedDips);
            o << ",\"speaker_low_cutoff_left_hz\":" << rl->speakerLowCutoffHz
              << ",\"speaker_low_cutoff_right_hz\":" << rr->speakerLowCutoffHz
              << ",\"lr_level_diff_db\":" << lrLevelDiffDb;
        }
        appendBands(o, "bands", r.correctionBands);
        // Egy csatorna mérésének késleltetése / polaritása nem korrekció (a rendszer késése és a mikrofon polaritása
        // ismeretlen): a felület 0-t kap. A bal–jobb különbség ("both") külön mezőben, tájékoztatásul.
        o << ",\"delay_ms\":0,\"inverted\":false";
        std::string hu = r.humanSummaryHu, en = r.humanSummaryEn;
        if (lr && lr->valid) {
            const double diff = lr->delayMs;   // pozitív: a jobb hang később érkezik
            o << ",\"lr_delay_ms\":" << diff << ",\"lr_polarity_differs\":" << (lr->polarityDiffers ? "true" : "false");
            std::ostringstream h, e;
            h.setf(std::ios::fixed); h.precision(2); e.setf(std::ios::fixed); e.precision(2);
            const int cm = static_cast<int>(std::round(std::fabs(diff) * 34.3));
            if (std::fabs(diff) >= 0.1) {
                h << "\n• Bal–jobb összevetés: a " << (diff > 0 ? "jobb" : "bal") << " hangszóró hangja kb. "
                  << std::fabs(diff) << " ms-mal (kb. " << cm << " cm-rel) később ér a mikrofonhoz.";
                e << "\n• Left–right comparison: the " << (diff > 0 ? "right" : "left") << " speaker arrives about "
                  << std::fabs(diff) << " ms (about " << cm << " cm) later at the microphone.";
            } else {
                h << "\n• Bal–jobb összevetés: a két hangszóró hangja egyszerre érkezik.";
                e << "\n• Left–right comparison: both speakers arrive at the same time.";
            }
            if (lr->polarityDiffers) {
                h << "\n• Figyelem: a két hangszóró polaritása eltér – ellenőrizd a hangszórókábelek bekötését (+/−).";
                e << "\n• Warning: the two speakers have opposite polarity – check the speaker wiring (+/−).";
            }
            hu += h.str();
            en += e.str();
        }
        o << ",\"human_summary_hu\":" << jsonStr(hu) << ",\"human_summary_en\":" << jsonStr(en) << '}';
        return o.str();
    }
};

} // namespace aqulify::room
