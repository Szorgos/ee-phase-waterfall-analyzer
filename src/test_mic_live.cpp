/**
 * @file test_mic_live.cpp
 * @brief A PipeWire-es mikrofonfelvevő élő tesztje (tests/live/mic_live.sh indítja elszigetelt PipeWire-ben).
 *
 * Visszacsatolt hurok: egy logaritmikus sweepet lejátszunk a `dac_a` null-kimenetre, és a felvevő a `dac_a`
 * monitorjáról vesz fel – a valódi felvételi út fut (PipeWire stream, valós idejű szál, puffer), csak mikrofon
 * helyett a kimenet hangja a bemenet. Ellenőrzés: hossz, szint, a felvett jel egyezése a lejátszottal
 * (normalizált keresztkorreláció), a néma `dac_b` monitorja csendes, nem létező eszköz elutasítva.
 */
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "room/mic_capture.hpp"

using namespace aqulify::room;

static int g_fail = 0;
// A feltétel egyszer értékelődik ki (mellékhatásos hívások – start, takePeak – miatt)
#define CHECK(c, ...) do { const bool ok_ = static_cast<bool>(c); std::printf("  [%s] ", ok_ ? "OK" : "HIBA"); std::printf(__VA_ARGS__); std::printf("\n"); if (!ok_) ++g_fail; } while (0)

static constexpr uint32_t kSr = 48000;

/** Logaritmikus sweep 20 Hz – 20 kHz, megadott hosszal és amplitúdóval, rövid be- és kifutással. */
static std::vector<float> logSweep(double seconds, float amp) {
    const size_t n = static_cast<size_t>(seconds * kSr);
    std::vector<float> s(n);
    const double f1 = 20.0, f2 = 20000.0, k = std::log(f2 / f1);
    const size_t fade = kSr / 100;
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kSr;
        double v = std::sin(2.0 * M_PI * f1 * seconds / k * (std::exp(t / seconds * k) - 1.0));
        if (i < fade) v *= static_cast<double>(i) / fade;
        if (i + fade > n) v *= static_cast<double>(n - i) / fade;
        s[i] = static_cast<float>(v * amp);
    }
    return s;
}

/** Teszt-lejátszó: a jel egy adott kimenetre (target.object), monó, 48 kHz. */
class TestPlayer {
public:
    bool play(const std::string& target, const std::vector<float>& sig) {
        sig_ = &sig;
        pos_ = 0;
        tl_ = pw_thread_loop_new("test-play", nullptr);
        ctx_ = pw_context_new(pw_thread_loop_get_loop(tl_), nullptr, 0);
        pw_thread_loop_start(tl_);
        pw_thread_loop_lock(tl_);
        core_ = pw_context_connect(ctx_, nullptr, 0);
        if (!core_) { pw_thread_loop_unlock(tl_); return false; }
        pw_properties* p = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
                                             PW_KEY_NODE_NAME, "aq_test_player", nullptr);
#ifdef PW_KEY_TARGET_OBJECT
        pw_properties_set(p, PW_KEY_TARGET_OBJECT, target.c_str());
#else
        pw_properties_set(p, PW_KEY_NODE_TARGET, target.c_str());
#endif
        st_ = pw_stream_new(core_, "aq-test-play", p);
        static const pw_stream_events ev = [] { pw_stream_events e{}; e.version = PW_VERSION_STREAM_EVENTS; e.process = onProcess; return e; }();
        pw_stream_add_listener(st_, &lis_, &ev, this);
        uint8_t buf[512];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
        spa_audio_info_raw info{};
        info.format = SPA_AUDIO_FORMAT_F32;
        info.channels = 1;
        info.rate = kSr;
        info.position[0] = SPA_AUDIO_CHANNEL_MONO;
        const spa_pod* params[1] = { spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info) };
        pw_stream_connect(st_, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                          static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
                          params, 1);
        pw_thread_loop_unlock(tl_);
        // a jel + 0,5 s csend végéig
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(sig.size() * 1000 / kSr) + 3000);
        while (pos_.load() < sig.size() + kSr / 2 && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        pw_thread_loop_lock(tl_);
        spa_hook_remove(&lis_);
        pw_stream_destroy(st_);
        pw_core_disconnect(core_);
        pw_thread_loop_unlock(tl_);
        pw_thread_loop_stop(tl_);
        pw_context_destroy(ctx_);
        pw_thread_loop_destroy(tl_);
        return pos_.load() >= sig.size();
    }

private:
    static void onProcess(void* d) {
        auto* s = static_cast<TestPlayer*>(d);
        pw_buffer* b = pw_stream_dequeue_buffer(s->st_);
        if (!b) return;
        spa_buffer* sb = b->buffer;
        auto* out = static_cast<float*>(sb->datas[0].data);
        if (!out) { pw_stream_queue_buffer(s->st_, b); return; }
        uint32_t n = sb->datas[0].maxsize / sizeof(float);
        if (b->requested && b->requested < n) n = static_cast<uint32_t>(b->requested);
        size_t pos = s->pos_.load();
        for (uint32_t i = 0; i < n; ++i, ++pos) out[i] = pos < s->sig_->size() ? (*s->sig_)[pos] : 0.0f;
        s->pos_.store(pos);
        sb->datas[0].chunk->offset = 0;
        sb->datas[0].chunk->stride = sizeof(float);
        sb->datas[0].chunk->size = n * sizeof(float);
        pw_stream_queue_buffer(s->st_, b);
    }
    const std::vector<float>* sig_ = nullptr;
    std::atomic<size_t> pos_{0};
    pw_thread_loop* tl_ = nullptr;
    pw_context* ctx_ = nullptr;
    pw_core* core_ = nullptr;
    pw_stream* st_ = nullptr;
    spa_hook lis_{};
};

/** A felvett jel helye és egyezése a lejátszottal: a késés (minta) és a normalizált korreláció (0..1). */
static void bestMatch(const std::vector<float>& ref, const std::vector<float>& rec, long& lag, double& corr) {
    lag = -1;
    corr = 0.0;
    // durva keresés egy rövid szakasszal (a sweep 0,25–0,35 s-e), legfeljebb 1 s késésig
    const size_t a = kSr / 4, len = kSr / 10, maxLag = kSr;
    double best = 0.0;
    for (size_t L = 0; L + a + len <= rec.size() && L <= maxLag; ++L) {
        double s = 0.0;
        for (size_t i = 0; i < len; ++i) s += static_cast<double>(ref[a + i]) * rec[L + a + i];
        if (s > best) { best = s; lag = static_cast<long>(L); }
    }
    if (lag < 0) return;
    // finomítás a teljes jelen (a mély szakaszon a durva keresés egy-két mintát tévedhet)
    const long coarse = lag;
    for (long L = std::max(0L, coarse - 4); L <= coarse + 4; ++L) {
        if (static_cast<size_t>(L) + ref.size() > rec.size()) continue;
        double sxy = 0.0, sxx = 0.0, syy = 0.0;
        for (size_t i = 0; i < ref.size(); ++i) {
            const double x = ref[i], y = rec[static_cast<size_t>(L) + i];
            sxy += x * y; sxx += x * x; syy += y * y;
        }
        const double c = (sxx > 0 && syy > 0) ? sxy / std::sqrt(sxx * syy) : 0.0;
        if (c > corr) { corr = c; lag = L; }
    }
}

int main() {
    std::printf("[1] Felvételi puffer (foglalás nélküli írás, túlcsordulás, csúcs)\n");
    {
        CaptureBuffer cb;
        cb.reset(10);
        const float a[6] = { 0.1f, -0.5f, 0.2f, 0.0f, 0.3f, -0.1f };
        cb.push(a, 6);
        CHECK(cb.written() == 6 && !cb.overflowed(), "6 minta beírva, túlcsordulás nincs");
        CHECK(std::fabs(cb.takePeak() - 0.5f) < 1e-6f && cb.takePeak() == 0.0f, "csúcs 0,5, kiolvasás után nullázva");
        const float st[10] = { 1, 9, 2, 9, 3, 9, 4, 9, 5, 9 };   // sztereó interleaved: csak az első csatorna kell
        cb.push(st, 5, 2);                                         // 6 + 5 > 10: az utolsó nem fér el
        CHECK(cb.written() == 10 && cb.overflowed(), "a kapacitásnál megáll, túlcsordulás jelezve");
        const auto v = cb.take();
        CHECK(v.size() == 10 && v[6] == 1 && v[7] == 2 && v[8] == 3 && v[9] == 4, "interleaved bemenetből az első csatorna");
    }

    std::printf("[2] Élő felvétel PipeWire-ben (visszacsatolt hurok a dac_a monitorján)\n");
    auto cap = createMicCapture();
    CHECK(cap != nullptr, "a PipeWire-es felvevő létrejön");
    if (!cap) { std::printf("\nSIKERTELEN (%d hiba)\n", g_fail); return 1; }

    const auto devs = cap->listInputs(true);
    bool hasA = false, hasB = false;
    for (const auto& d : devs) {
        std::printf("     eszköz: %s (%s)%s\n", d.id.c_str(), d.name.c_str(), d.isMonitor ? " [monitor]" : "");
        hasA |= d.id == "dac_a" && d.isMonitor;
        hasB |= d.id == "dac_b" && d.isMonitor;
    }
    CHECK(hasA && hasB, "az eszközlistában a dac_a és dac_b monitor");
    CHECK(cap->listInputs(false).size() + 2 <= devs.size(), "monitorok nélkül kevesebb (csak valódi bemenetek)");

    std::string err;
    CHECK(!cap->start("nincs-ilyen-eszkoz", kSr, 2.0, err) && !err.empty(),
          "nem létező eszköz elutasítva, nem esik vissza az alapértelmezettre (%s)", err.c_str());

    const auto sweep = logSweep(2.0, 0.5f);
    err.clear();
    CHECK(cap->start("dac_a", kSr, 6.0, err), "felvétel indul a dac_a monitoráról (%s)", err.c_str());
    TestPlayer player;
    const bool played = player.play("dac_a", sweep);
    CHECK(played, "a sweep lejátszva (2 s)");
    const float peak = cap->takePeak();
    const auto rec = cap->stop();
    CHECK(!cap->running(), "a felvétel leállt");
    std::printf("     felvett: %zu minta (%.2f s), csúcs %.1f dBFS\n", rec.size(), rec.size() / double(kSr), peakDbfs(peak));
    CHECK(rec.size() >= sweep.size(), "a felvétel legalább olyan hosszú, mint a sweep");
    CHECK(peakDbfs(peak) > -9.0 && peakDbfs(peak) < 0.1, "a szint a lejátszotthoz illik (−6 dBFS körül): %.1f dBFS", peakDbfs(peak));
    long lag = -1;
    double corr = 0.0;
    bestMatch(sweep, rec, lag, corr);
    CHECK(lag >= 0, "a sweep megtalálható a felvételben (késés: %ld minta = %.1f ms)", lag, lag * 1000.0 / kSr);
    CHECK(corr > 0.99, "a felvett jel egyezik a lejátszottal (normalizált korreláció %.5f)", corr);
    // Diagnosztika: szakaszonként a legjobb késés a teljes késés körül (kimaradt minták / ugrás kimutatása)
    if (lag >= 0) {
        const size_t seg = sweep.size() / 8;
        for (size_t k = 0; k < 8; ++k) {
            long bestL = 0;
            double bestC = -2.0;
            for (long d = -3000; d <= 3000; ++d) {
                const long L = lag + d;
                if (L < 0 || static_cast<size_t>(L) + (k + 1) * seg > rec.size()) continue;
                double sxy = 0, sxx = 0, syy = 0;
                for (size_t i = k * seg; i < (k + 1) * seg; i += 4) {
                    const double x = sweep[i], y = rec[static_cast<size_t>(L) + i];
                    sxy += x * y; sxx += x * x; syy += y * y;
                }
                const double c = (sxx > 0 && syy > 0) ? sxy / std::sqrt(sxx * syy) : 0;
                if (c > bestC) { bestC = c; bestL = d; }
            }
            std::printf("     szakasz %zu: eltolás %+ld minta, korreláció %.4f\n", k, bestL, bestC);
        }
    }

    std::printf("[3] Néma kimenet monitora: csend\n");
    err.clear();
    CHECK(cap->start("dac_b", kSr, 2.0, err), "felvétel indul a dac_b monitoráról (%s)", err.c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    const float silentPeak = cap->takePeak();
    const auto silent = cap->stop();
    CHECK(!silent.empty() && peakDbfs(silentPeak) < -90.0, "%zu minta, csúcs %.1f dBFS", silent.size(), peakDbfs(silentPeak));

    std::printf("\n%s (%d hiba)\n", g_fail ? "SIKERTELEN" : "MINDEN TESZT SIKERES", g_fail);
    return g_fail ? 1 : 0;
}
