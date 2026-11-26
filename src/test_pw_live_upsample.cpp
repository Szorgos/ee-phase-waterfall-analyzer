/**
 * Egységteszt: PipeWire saját motoros rendszerhang-felmintavételezés ráta-invariáns őrzője (#51, #52).
 *
 * Ellenőrzi:
 *  [1] Sebesség- és hangmagasság-invariáns: #52 (capture=96k, playback=96k, híd=48->96k -> 0,5x) és
 *      #51 (capture=48k, playback=48k, híd=48->96k -> 0,5x) azonnali felismerése és tiltása.
 *  [2] Valódi LiveUpsampleBridge frekvencia-szimuláció: az őrző nélkül 96 kHz bemenettel 500 Hz (0,5x) szól,
 *      egyező 48->96 kHz rátákkal pontos 1000 Hz (0 overrun).
 *  [3] Fizikai cél visszacsatolás-védelme: üres, aqulify_sink, auto_null, dummy elutasítva; valódi DAC elfogadva.
 *  [4] RateInvariantGuard állapotgép: #52 capture-eltérés, #51 hardveres 48 kHz-en ragadó DAC (sof-hda-dsp),
 *      ping-pong blokkolás, majd feloldás kimenetváltáskor (dac_a -> dac_b) és rateMode-váltáskor (system -> follow).
 */
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "audio_options.hpp"
#include "live_upsample_bridge.hpp"
#include "pw_keepalive_policy.hpp"
#include "pw_live_upsample_policy.hpp"

using namespace ee_analyzer;
using namespace ee_analyzer::pw_live_ups;

static int g_fail = 0;
#define CHECK(cond, ...)                                                           \
    do {                                                                           \
        if (!(cond)) {                                                             \
            ++g_fail;                                                              \
            std::printf("  [HIBA] " __VA_ARGS__);                                  \
        } else {                                                                   \
            std::printf("  [OK]   " __VA_ARGS__);                                  \
        }                                                                          \
        std::printf("\n");                                                         \
    } while (0)

static constexpr double kPi = 3.14159265358979323846;

static double db(double v) { return 20.0 * std::log10(std::max(v, 1e-30)); }

/** Hann-ablakolt egypontos DFT amplitúdó sztereó bal csatornán. */
static double toneAmp(const std::vector<float>& stereo, size_t startFrame, size_t lenFrames,
                      double freqHz, double rateHz) {
    double cr = 0.0, ci = 0.0, wsum = 0.0;
    for (size_t i = 0; i < lenFrames; ++i) {
        const double w = 0.5 * (1.0 - std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(lenFrames)));
        const double x = static_cast<double>(stereo[(startFrame + i) * 2]) * w;
        const double ph = 2.0 * kPi * freqHz * static_cast<double>(i) / rateHz;
        cr += x * std::cos(ph);
        ci -= x * std::sin(ph);
        wsum += w;
    }
    return 2.0 * std::hypot(cr, ci) / std::max(wsum, 1e-12);
}

int main() {
    std::printf("[1] Ráta-invariáns és sebességarány (#51, #52)\n");
    {
        CHECK(ratesMatchBridge(48000, 96000, 48000, 96000), "egyező ráták (48 -> 96 kHz): engedélyezett");
        CHECK(std::abs(effectiveSpeedRatio(48000, 96000, 48000, 96000) - 1.0) < 1e-9,
              "egyező rátáknál a sebességarány pontosan 1,0");

        // #52: a capture stream 96 kHz-re váltott a gráffal, miközben a híd 48->96 kHz
        CHECK(!ratesMatchBridge(96000, 96000, 48000, 96000),
              "#52: capture=96k, playback=96k, híd=48->96k -> elutasítva");
        CHECK(std::abs(effectiveSpeedRatio(96000, 96000, 48000, 96000) - 0.5) < 1e-9,
              "#52: az elmaradt védelem pontosan 0,5x sebességet (-1 oktáv) okozna");

        // #51: a playback stream 48 kHz-en maradt, miközben a híd 48->96 kHz
        CHECK(!ratesMatchBridge(48000, 48000, 48000, 96000),
              "#51: capture=48k, playback=48k, híd=48->96k -> elutasítva");
        CHECK(std::abs(effectiveSpeedRatio(48000, 48000, 48000, 96000) - 0.5) < 1e-9,
              "#51: 48 kHz-es playback + 48->96 híd pontosan 0,5x sebességet okozna");

        // Ismeretlen (0) vagy érvénytelen ráták
        CHECK(!ratesMatchBridge(0, 96000, 48000, 96000), "ismeretlen captureRate (0) -> elutasítva");
        CHECK(!ratesMatchBridge(48000, 0, 48000, 96000), "ismeretlen playbackRate (0) -> elutasítva");
        CHECK(!ratesMatchBridge(48000, 48000, 48000, 48000), "nem felmintavételező (48->48) -> elutasítva");
    }

    std::printf("[2] Valódi LiveUpsampleBridge frekvencia-ellenőrzés (48k vs hibás 96k betáplálás)\n");
    {
        audioopt::AudioOptions o;
        o.upsampleEngine = 0;
        o.upsamplePhase = 25.0;
        o.upsampleHeadroomDb = 0.0f;

        auto runSim = [&](uint32_t actualCaptureRate) {
            livebridge::LiveUpsampleBridge br;
            br.configure(48000, 96000, o, nullptr, /*enableDriftControl=*/false);
            const size_t playPerTick = 960; // 10 ms @ 96 kHz
            const size_t capPerTick = actualCaptureRate / 100;
            std::vector<float> inBlk(capPerTick * 2), outBlk(playPerTick * 2), tail;
            uint64_t idx = 0;
            for (int tick = 0; tick < 120; ++tick) {
                for (size_t i = 0; i < capPerTick; ++i) {
                    const float v = 0.5f * std::sin(2.0f * static_cast<float>(kPi) * 1000.0f *
                                                    static_cast<float>(idx++) / static_cast<float>(actualCaptureRate));
                    inBlk[2 * i] = v;
                    inBlk[2 * i + 1] = v;
                }
                br.pushInput(inBlk.data(), capPerTick);
                while (br.stepOnce()) {}
                br.pullOutput(outBlk.data(), playPerTick);
                if (tick >= 80) tail.insert(tail.end(), outBlk.begin(), outBlk.end());
            }
            const double a1k = toneAmp(tail, 0, 16384, 1000.0, 96000.0);
            const double a500 = toneAmp(tail, 0, 16384, 500.0, 96000.0);
            return std::pair<double, double>{ db(a1k / 0.5), db(a500 / 0.5) };
        };

        const auto [good1k, good500] = runSim(48000);
        CHECK(std::abs(good1k) <= 0.05 && good500 <= -80.0,
              "egyező 48 kHz capture -> 96 kHz playback: 1 kHz=%.2f dB, 500 Hz=%.1f dB", good1k, good500);

        const auto [bad1k, bad500] = runSim(96000);
        CHECK(std::abs(bad500) <= 0.05 && bad1k <= -80.0,
              "őrző nélküli #52 (96 kHz capture -> 48->96 híd): 1 kHz eltűnik (%.1f dB), 500 Hz=%.2f dB (-1 oktáv)",
              bad1k, bad500);
    }

    std::printf("[3] Fizikai cél visszacsatolás-védelme\n");
    {
        CHECK(!hasValidPhysicalTarget(""), "üres cél elutasítva");
        CHECK(!hasValidPhysicalTarget("aqulify_sink"), "saját aqulify_sink elutasítva");
        CHECK(!hasValidPhysicalTarget("aqulify_sink_output"), "saját aqulify_sink_output elutasítva");
        CHECK(!hasValidPhysicalTarget("auto_null"), "auto_null dummy nyelő elutasítva");
        CHECK(!hasValidPhysicalTarget("alsa_output.dummy"), "dummy nyelő elutasítva");
        CHECK(hasValidPhysicalTarget("alsa_output.pci-0000_00_1f.3-platform-skl_hda_dsp_generic.HiFi__Speaker__sink"),
              "Intel sof-hda-dsp hangszóró elfogadva");
        CHECK(hasValidPhysicalTarget("alsa_output.usb-Generic_USB_Audio-00.analog-stereo"),
              "USB DAC elfogadva");
        CHECK(hasValidPhysicalTarget("dac_a"), "teszt fizikai nyelő (dac_a) elfogadva");

        CHECK(!canUseDecoupledPlayback(true, 96000, ""), "üres céllal nem engedélyezett a szétválasztott mód");
        CHECK(!canUseDecoupledPlayback(true, 96000, "aqulify_sink"), "saját aqulify_sink céllal tilos a szétválasztott mód");
        CHECK(!canUseDecoupledPlayback(true, 96000, "auto_null"), "auto_null céllal tilos a szétválasztott mód");
        CHECK(!canUseDecoupledPlayback(false, 96000, "dac_a"), "kikapcsolva nem szétválasztott");
        CHECK(!canUseDecoupledPlayback(true, 0, "dac_a"), "0 célrátával nem szétválasztott");
        CHECK(canUseDecoupledPlayback(true, 96000, "dac_a"), "valid fizikai céllal (dac_a) engedélyezett");

        CHECK(needsPlaybackRebuildForTarget(true, "dac_a", "dac_b"),
              "kimenetváltás szétválasztott módban (dac_a -> dac_b) playback újraépítést kér");
        CHECK(!needsPlaybackRebuildForTarget(true, "dac_a", "dac_a"),
              "azonos cél (dac_a -> dac_a) nem építi újra a playback streamet");
        CHECK(!needsPlaybackRebuildForTarget(false, "dac_a", "dac_b"),
              "közvetlen módban nem a szétválasztott újraépítés fut");
    }

    std::printf("[4] RateInvariantGuard állapotgép (#52, #51, ping-pong védelem, kimenet- és módváltás)\n");
    {
        RateInvariantGuard g;
        g.resetSession(/*nowMs=*/1000, 48000, 96000);

        // #52 eset: a playback elindul 96 kHz-en, de a capture is 96 kHz-re vált
        CHECK(g.evaluate(1100, 96000, 96000, 96000, true) == GuardVerdict::WaitSettle,
              "#52: 1. eltérésnél megerősítésre vár (0 ms)");
        CHECK(g.evaluate(1300, 96000, 96000, 96000, true) == GuardVerdict::WaitSettle,
              "#52: 200 ms-nál még a megerősítési ablakon belül");
        const GuardVerdict v52 = g.evaluate(1500, 96000, 96000, 96000, true);
        CHECK(v52 == GuardVerdict::FallbackCaptureMismatch && std::string(verdictReasonCode(v52)) == "rate_mismatch",
              "#52: 400 ms után azonnali FallbackCaptureMismatch -> rate_mismatch");

        // Playback-formátum eltérés (48 kHz maradt 96 kHz helyett) -> szintén rate_mismatch
        RateInvariantGuard gPlay;
        gPlay.resetSession(/*nowMs=*/2000, 48000, 96000);
        gPlay.evaluate(2100, 48000, 48000, 48000, true);
        const GuardVerdict vPlay = gPlay.evaluate(2500, 48000, 48000, 48000, true);
        CHECK(vPlay == GuardVerdict::FallbackPlaybackMismatch && std::string(verdictReasonCode(vPlay)) == "rate_mismatch",
              "playback formátum-eltérés (48k != 96k): FallbackPlaybackMismatch -> rate_mismatch");

        // Ping-pong blokkolás az adott (speaker, 48k->96k, follow) kulcsra
        const FallbackKey kSpeakerFollow{ "alsa_output.sof.Speaker", 48000, 96000, 1 };
        g.blockKey(kSpeakerFollow, verdictReasonCode(v52), v52);
        CHECK(g.isBlocked(kSpeakerFollow) && g.lastReason() == "rate_mismatch",
              "ping-pong védelem: ugyanaz a (kimenet, 48->96, follow) kulcs blokkolva marad (rate_mismatch)");

        // Kimenetváltás USB DAC-ra (dac_a -> dac_b): a tiltás azonnal feloldódik!
        g.onTargetChanged("alsa_output.usb.DAC");
        CHECK(!g.isBlocked(kSpeakerFollow) && g.lastReason().empty(),
              "kimenetváltáskor (hangszóró -> USB DAC) a tiltás feloldódik");

        // #51 eset: capture=48k, playback=96k (PipeWire SPA adapter), DE a hardveres driverClockRate 48k marad!
        g.resetSession(/*nowMs=*/5000, 48000, 96000);
        CHECK(g.evaluate(5800, 48000, 96000, 48000, true) == GuardVerdict::WaitSettle,
              "#51 (sof-hda-dsp 48 kHz): türelmi ablakon belül (800 ms) még vár");
        const GuardVerdict v51 = g.evaluate(6600, 48000, 96000, 48000, true);
        CHECK(v51 == GuardVerdict::FallbackDacRateMismatch && std::string(verdictReasonCode(v51)) == "dac_unsupported",
              "#51 (sof-hda-dsp 48 kHz): 1600 ms után FallbackDacRateMismatch -> dac_unsupported (nincs dupla 48->96->48)");

        // rateMode váltás (system=0 -> follow=1): a tiltás feloldódik!
        const FallbackKey kSystem{ "dac_a", 48000, 96000, 0 };
        g.blockKey(kSystem, "dac_unsupported", v51);
        CHECK(g.isBlocked(kSystem), "system módban blokkolt kulcs");
        g.onRateModeChanged(1);
        CHECK(!g.isBlocked(kSystem), "rateMode váltáskor (system -> follow) a tiltás feloldódik");

        // Tökéletes szétválasztott állapot: capture=48k, playback=96k, driverClock=96k
        g.resetSession(/*nowMs=*/10000, 48000, 96000);
        CHECK(g.evaluate(10500, 48000, 96000, 96000, true) == GuardVerdict::Ok,
              "egyező rátáknál (capture=48k, playback=96k, clock=96k) végig Ok");
        CHECK(g.evaluate(25000, 48000, 96000, 96000, true) == GuardVerdict::Ok,
              "hosszú futás után is Ok marad");
    }

    std::printf("[5] Dummy-Driver hiánya (egyedi/minimál PipeWire-konfiguráció)\n");
    {
        CHECK(!canUseDecoupledPlayback(true, 96000, "dac_a", "aqulify_sink", /*hasDummyDriver=*/false),
              "Dummy-Driver nélkül tilos a szétválasztott mód (hasDummyDriver=false)");
        CHECK(canUseDecoupledPlayback(true, 96000, "dac_a", "aqulify_sink", /*hasDummyDriver=*/true),
              "Dummy-Driver meglétével és valid fizikai céllal engedélyezett");

        // Menet közben eltűnik a Dummy-Driver -> azonnali visszalépés közvetlen módba (rate_mismatch)
        RateInvariantGuard gDummy;
        gDummy.resetSession(/*nowMs=*/1000, 48000, 96000);
        CHECK(gDummy.evaluate(1200, 48000, 96000, 96000, true, /*hasDummyDriver=*/true) == GuardVerdict::Ok,
              "Dummy-Driverrel aktív marad");
        const GuardVerdict vNoDummy = gDummy.evaluate(1250, 48000, 96000, 96000, true, /*hasDummyDriver=*/false);
        CHECK(vNoDummy == GuardVerdict::FallbackMissingDummyDriver,
              "Dummy-Driver eltűnésekor azonnali FallbackMissingDummyDriver (türelmi ablak nélkül)");
        CHECK(std::string(verdictReasonCode(vNoDummy)) == "rate_mismatch",
              "Dummy-Driver hiányának okkódja a meglévő rate_mismatch");
        CHECK(std::string(verdictDetailHu(vNoDummy)) == "nincs Dummy-Driver a PipeWire-ben",
              "Dummy-Driver hiányának naplóoka pontosan 'nincs Dummy-Driver a PipeWire-ben'");
    }

    std::printf("[6] KeepAlivePolicy (#46, #53: csendes szünet, regiszter-újraszámolás, felső határ)\n");
    {
        using namespace ee_analyzer::pw_keepalive;

        // 1. Regiszter Stream/Output/Audio újraszámolása (nincs növelő/csökkentő számláló)
        std::unordered_map<uint32_t, RegistryStreamNode> reg;
        reg[10] = { 10, "aqulify_sink_output", "Aqulify" };
        reg[11] = { 11, "aqulify_sink_keepalive", "Aqulify" };
        reg[12] = { 12, "player_out", "Aqulify" };
        CHECK(!hasExternalAudioOutputStream(reg), "csak saját Aqulify streamek -> nincs külső forrás");
        reg[20] = { 20, "Brave", "Brave" };
        CHECK(hasExternalAudioOutputStream(reg), "külső Stream/Output/Audio (Brave) jelenléte felismerve");
        reg.erase(20);
        CHECK(!hasExternalAudioOutputStream(reg), "eltávolítás után azonnal nincs külső forrás (nem ragadhat be)");

        // 2. Felhasználói kapcsolás naplózása vs belső szünetek (0 naplózaj)
        KeepAlivePolicy kp;
        KeepAliveInputs in{};
        in.nowMs = 1000;
        in.userWanted = false;
        auto d = kp.evaluate(in);
        CHECK(!d.shouldRun && !d.logUserChange, "alapból kikapcsolva: nem fut és nem naplóz");

        in.nowMs = 1100;
        in.userWanted = true;
        in.rateFollow = true;
        d = kp.evaluate(in);
        CHECK(d.shouldRun && d.logUserChange && d.userEnabled,
              "felhasználói bekapcsolás: keepAlive indul és 1x naplóz (be)");

        // Beépített lejátszó nyit -> csendes szünet (0 naplósor)
        in.nowMs = 2000;
        in.playerSuppressed = true;
        d = kp.evaluate(in);
        CHECK(!d.shouldRun && !d.logUserChange, "beépített lejátszó alatt csendesen szünetel (nincs ki napló)");

        // Beépített lejátszó bezár -> csendes visszakapcsolás (0 naplósor)
        in.nowMs = 5000;
        in.playerSuppressed = false;
        d = kp.evaluate(in);
        CHECK(d.shouldRun && !d.logUserChange, "beépített lejátszó után csendesen visszakapcsol (nincs be napló)");

        // Saját motoros felmintavételezés (liveUpsActive) alatt végig szünetel
        in.nowMs = 6000;
        in.liveUpsampleActive = true;
        d = kp.evaluate(in);
        CHECK(!d.shouldRun && !d.logUserChange, "aktív saját motoros felmintavételezés alatt szünetel");
        in.nowMs = 12000;
        d = kp.evaluate(in);
        CHECK(!d.shouldRun && !d.logUserChange, "hosszú (6 s) saját motoros felmintavételezés alatt sem indul vissza");
        in.nowMs = 13000;
        in.liveUpsampleActive = false;
        d = kp.evaluate(in);
        CHECK(d.shouldRun && !d.logUserChange, "saját motor leállása után csendesen visszakapcsol");

        // 3. Aktív külső forrás (follow): amíg szól, nem indul vissza 1,5 s után sem; leállása után visszakapcsol
        in.nowMs = 20000;
        in.hasExternalStream = true;
        in.externalAudioActive = false; // épp most nyílt, még indul
        d = kp.evaluate(in);
        CHECK(!d.shouldRun && !d.logUserChange, "új külső forrásnál azonnal szünetel");
        in.nowMs = 20500;
        in.externalAudioActive = true;
        d = kp.evaluate(in);
        CHECK(!d.shouldRun, "aktív külső hangfolyamnál szünetel (0,5 s)");
        in.nowMs = 25000;
        d = kp.evaluate(in);
        CHECK(!d.shouldRun, "aktív külső hangfolyamnál 5 s után sem indul vissza (nincs 48<->96 oszcilláció)");
        // Külső forrás leáll
        in.nowMs = 26000;
        in.hasExternalStream = false;
        in.externalAudioActive = false;
        d = kp.evaluate(in);
        CHECK(!d.shouldRun, "külső forrás vége után rövid lecsengési ablak (számváltás ellen)");
        in.nowMs = 26000 + kExternalPauseGraceMs;
        d = kp.evaluate(in);
        CHECK(d.shouldRun && !d.logUserChange, "külső forrás leállása után a keepAlive csendesen visszakapcsol");

        // 4. Biztonsági felső határ: bent maradó külső stream (pl. PAUSED tab), de nincs aktív hang
        in.nowMs = 30000;
        in.hasExternalStream = true;
        in.externalAudioActive = false;
        d = kp.evaluate(in);
        CHECK(!d.shouldRun, "új külső stream hang nélkül: kezdeti ablakban szünetel");
        in.nowMs = 30000 + kExternalPauseGraceMs + kIdleSuppressCapMs - 100;
        d = kp.evaluate(in);
        CHECK(!d.shouldRun, "biztonsági felső határ előtt még szünetel");
        in.nowMs = 30000 + kExternalPauseGraceMs + kIdleSuppressCapMs + 100;
        d = kp.evaluate(in);
        CHECK(d.shouldRun && !d.logUserChange,
              "biztonsági felső határ: hang nélkül maradó szünet legkésőbb néhány mp múlva visszakapcsol");

        // 5. Felhasználói kikapcsolás -> pontosan 1x naplóz (ki)
        in.nowMs = 40000;
        in.userWanted = false;
        d = kp.evaluate(in);
        CHECK(!d.shouldRun && d.logUserChange && !d.userEnabled,
              "felhasználói kikapcsolás: leáll és 1x naplóz (ki)");
    }

    std::printf("\n%s (%d hiba)\n", g_fail ? "SIKERTELEN" : "MINDEN PW LIVE UPSAMPLE TESZT SIKERES", g_fail);
    return g_fail ? 1 : 0;
}
