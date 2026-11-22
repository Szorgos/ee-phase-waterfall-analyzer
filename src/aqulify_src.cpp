// aqulify-src: fájlból fájlba mintavétel-átalakítás (include/src_convert.hpp).
// Mérőeszköz (pl. független SRC-összehasonlításhoz): a csomagokba nem kerül be.
//
//   aqulify-src [beállítások] <bemenet.wav> <kimenet.wav>
//   aqulify-src [beállítások] --dir <bemeneti mappa> <kimeneti mappa>   (minden .wav, azonos fájlnévvel)
//
// Beállítások:
//   --mode M          player (32 bit, a lejátszó kódútja; alap) | file (64 bites lebegőpontos fájlátalakító)
//   --rate N          célmintavétel (alap: 44100)
//   --preset P        reference | linear | intermediate | minimum (alap: reference)
//   --edge E          none | odd | lpc (csak --mode file esetén; alap: lpc)
//   --precision N     pontosság bitben (15..33; csak --mode file esetén, alap: a preset pontossága)
//   --passband P      sávszélesség % (50..99.7; csak --mode file esetén, alap: a preset értéke)
//   --stopband S      záró sáv kezdete % (100..150; csak --mode file esetén, alap: 100)
//   --phase PH        fázisválasz 0..100 (0=min, 25=köztes, 50=lineáris; csak --mode file esetén)
//   --headroom D      0 | -1 | -3 | -6 dB (alap: 0)
//   --threads N       1 | 2 (a lejátszó két szálas módja; a kimenet bitre azonos)
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "src_convert.hpp"

namespace fs = std::filesystem;
using namespace ee_analyzer;

static int usage() {
    std::fprintf(stderr,
                 "Használat: aqulify-src [--mode player|file] [--rate N] [--preset reference|linear|intermediate|minimum]\n"
                 "                       [--edge none|odd|lpc] [--precision 15..33] [--passband 50..99.7]\n"
                 "                       [--stopband 100..150] [--phase 0..100] [--headroom 0|-1|-3|-6]\n"
                 "                       <bemenet> <kimenet>  |  --dir <bemeneti mappa> <kimeneti mappa>\n");
    return 64;
}

static int convertOne(const std::string& in, const std::string& out, uint32_t rate, const audioopt::AudioOptions& o,
                      bool fileMode, const srcconv::FileOptions& fileOpts) {
    srcconv::Result r;
    std::string err;
    if (fileMode) {
        std::vector<double> data64;
        if (!srcconv::convertFile64(in, rate, o, data64, r, &err, fileOpts)) {
            std::fprintf(stderr, "HIBA: %s\n", err.c_str());
            return 1;
        }
        if (!srcconv::writeDoubleWav(out, data64, r.channels, rate, &err)) {
            std::fprintf(stderr, "HIBA: %s\n", err.c_str());
            return 1;
        }
    } else {
        std::vector<float> data;
        if (!srcconv::convert(in, rate, o, data, r, &err)) {
            std::fprintf(stderr, "HIBA: %s\n", err.c_str());
            return 1;
        }
        if (!srcconv::writeFloatWav(out, data, r.channels, rate, &err)) {
            std::fprintf(stderr, "HIBA: %s\n", err.c_str());
            return 1;
        }
    }
    const double expect = static_cast<double>(r.inFrames) * rate / r.srcRate;
    std::printf("%s: %u Hz -> %u Hz, %u csat., %u bit float, %llu -> %llu keret (elvárt %.1f), a végén kiürítve %llu (%d lépés), maradék %llu, ±1 fölött %llu%s\n",
                fs::path(in).filename().string().c_str(), r.srcRate, rate, r.channels, r.outBits,
                static_cast<unsigned long long>(r.inFrames), static_cast<unsigned long long>(r.outFrames), expect,
                static_cast<unsigned long long>(r.flushFrames), r.flushSteps, static_cast<unsigned long long>(r.leftoverFrames),
                static_cast<unsigned long long>(r.clipped), r.monoMismatch ? ", FIGYELEM: mono bal != jobb" : "");
    return (r.leftoverFrames || r.monoMismatch) ? 2 : 0;
}

int main(int argc, char** argv) {
    uint32_t rate = 44100;
    std::string mode = "player";
    std::string preset = "reference";
    std::string edgeStr;
    double precisionOverride = 0.0;
    double passbandOverride = 0.0;
    double stopbandOverride = 0.0;
    double phaseOverride = -1.0;
    float headroom = 0.0f;
    int threads = 1;
    bool dir = false;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--mode") { const char* v = next(); if (!v) return usage(); mode = v; }
        else if (a == "--rate") { const char* v = next(); if (!v) return usage(); rate = static_cast<uint32_t>(std::strtoul(v, nullptr, 10)); }
        else if (a == "--preset") { const char* v = next(); if (!v) return usage(); preset = v; }
        else if (a == "--edge") { const char* v = next(); if (!v) return usage(); edgeStr = v; }
        else if (a == "--precision") { const char* v = next(); if (!v) return usage(); precisionOverride = std::strtod(v, nullptr); }
        else if (a == "--passband") { const char* v = next(); if (!v) return usage(); passbandOverride = std::strtod(v, nullptr); }
        else if (a == "--stopband") { const char* v = next(); if (!v) return usage(); stopbandOverride = std::strtod(v, nullptr); }
        else if (a == "--phase") { const char* v = next(); if (!v) return usage(); phaseOverride = std::strtod(v, nullptr); }
        else if (a == "--headroom") { const char* v = next(); if (!v) return usage(); headroom = std::strtof(v, nullptr); }
        else if (a == "--threads") { const char* v = next(); if (!v) return usage(); threads = std::atoi(v); }
        else if (a == "--dir") dir = true;
        else if (a == "-h" || a == "--help") return usage();
        else pos.push_back(a);
    }
    if (pos.size() != 2 || rate < 8000 || rate > 768000) return usage();
    if (mode != "player" && mode != "file") return usage();
    const bool fileMode = (mode == "file");
    if (passbandOverride > 1.0) passbandOverride /= 100.0;
    if (stopbandOverride > 2.0) stopbandOverride /= 100.0;

    if (!fileMode && (!edgeStr.empty() || precisionOverride != 0.0 || passbandOverride != 0.0 ||
                      stopbandOverride != 0.0 || phaseOverride >= 0.0)) return usage();
    if (precisionOverride != 0.0 && (precisionOverride < 15.0 || precisionOverride > 33.0)) return usage();
    if (passbandOverride != 0.0 && (passbandOverride < 0.5 || passbandOverride > 0.997)) return usage();
    if (stopbandOverride != 0.0 && (stopbandOverride < 1.0 || stopbandOverride > 1.5)) return usage();
    if (phaseOverride >= 0.0 && phaseOverride > 100.0) return usage();

    srcconv::EdgeMode edge = fileMode ? srcconv::EdgeMode::Lpc : srcconv::EdgeMode::None;
    if (!edgeStr.empty()) {
        if (edgeStr == "none") edge = srcconv::EdgeMode::None;
        else if (edgeStr == "odd") edge = srcconv::EdgeMode::Odd;
        else if (edgeStr == "lpc") edge = srcconv::EdgeMode::Lpc;
        else return usage();
    }

    int p = -1;
    if (preset == "linear") p = 0; else if (preset == "intermediate") p = 1; else if (preset == "minimum") p = 2;
    else if (preset == "reference") p = audioopt::kUpsamplePresetReference;
    if (p < 0) return usage();

    audioopt::AudioOptions o;
    audioopt::applyUpsamplePreset(o, p);
    o.upsampleHeadroomDb = headroom;
    o.upsampleThreads = threads;
    audioopt::sanitize(o);

    srcconv::FileOptions fileOpts;
    fileOpts.precision = precisionOverride;
    fileOpts.passbandEnd = passbandOverride;
    fileOpts.stopbandBegin = stopbandOverride;
    fileOpts.phase = phaseOverride;
    fileOpts.edge = edge;

    const double shownPrec = (fileMode && precisionOverride > 0.0) ? precisionOverride : static_cast<double>(o.upsamplePrecision);
    const double shownPb = (fileMode && passbandOverride > 0.0) ? passbandOverride : o.upsamplePassbandEnd;
    const double shownSb = (fileMode && stopbandOverride > 0.0) ? stopbandOverride : o.upsampleStopbandBegin;
    const double shownPh = (fileMode && phaseOverride >= 0.0) ? phaseOverride : o.upsamplePhase;
    const char* edgeName = (edge == srcconv::EdgeMode::Lpc) ? "lpc" : (edge == srcconv::EdgeMode::Odd ? "odd" : "none");
    std::printf("Mód: %s (%s, szél: %s), beállítás: %s, sávszélesség %.1f%%, záró sáv %.0f%%, %.0f bit, fázis %.0f, fejtér %.0f dB\n",
                mode.c_str(), fileMode ? "64 bit float WAV" : "32 bit float WAV", edgeName, preset.c_str(),
                shownPb * 100.0, shownSb * 100.0, shownPrec, shownPh, static_cast<double>(o.upsampleHeadroomDb));

    if (!dir) return convertOne(pos[0], pos[1], rate, o, fileMode, fileOpts);
    std::error_code ec;
    fs::create_directories(pos[1], ec);
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(pos[0], ec))
        if (e.is_regular_file() && (e.path().extension() == ".wav" || e.path().extension() == ".WAV")) files.push_back(e.path());
    if (ec || files.empty()) { std::fprintf(stderr, "HIBA: nincs .wav a mappában: %s\n", pos[0].c_str()); return 1; }
    std::sort(files.begin(), files.end());
    int worst = 0;
    for (const auto& f : files) {
        const int rc = convertOne(f.string(), (fs::path(pos[1]) / f.filename()).string(), rate, o, fileMode, fileOpts);
        worst = std::max(worst, rc);
    }
    return worst;
}
