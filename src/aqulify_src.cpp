// aqulify-src: fájlból fájlba mintavétel-átalakítás a beépített lejátszó kódútján (include/src_convert.hpp).
// Mérőeszköz (pl. független SRC-összehasonlításhoz): a csomagokba nem kerül be.
//
//   aqulify-src [beállítások] <bemenet.wav> <kimenet.wav>
//   aqulify-src [beállítások] --dir <bemeneti mappa> <kimeneti mappa>   (minden .wav, azonos fájlnévvel)
//
// Beállítások:
//   --rate N          célmintavétel (alap: 44100)
//   --preset P        reference | linear | intermediate | minimum (alap: reference)
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
                 "Használat: aqulify-src [--rate N] [--preset reference|linear|intermediate|minimum] [--headroom 0|-1|-3|-6]\n"
                 "                       <bemenet> <kimenet>  |  --dir <bemeneti mappa> <kimeneti mappa>\n");
    return 64;
}

static int convertOne(const std::string& in, const std::string& out, uint32_t rate, const audioopt::AudioOptions& o) {
    std::vector<float> data;
    srcconv::Result r;
    std::string err;
    if (!srcconv::convert(in, rate, o, data, r, &err)) { std::fprintf(stderr, "HIBA: %s\n", err.c_str()); return 1; }
    if (!srcconv::writeFloatWav(out, data, r.channels, rate, &err)) { std::fprintf(stderr, "HIBA: %s\n", err.c_str()); return 1; }
    const double expect = static_cast<double>(r.inFrames) * rate / r.srcRate;
    std::printf("%s: %u Hz -> %u Hz, %u csat., %llu -> %llu keret (elvárt %.1f), a végén kiürítve %llu (%d lépés), maradék %llu, ±1 fölött %llu%s\n",
                fs::path(in).filename().string().c_str(), r.srcRate, rate, r.channels,
                static_cast<unsigned long long>(r.inFrames), static_cast<unsigned long long>(r.outFrames), expect,
                static_cast<unsigned long long>(r.flushFrames), r.flushSteps, static_cast<unsigned long long>(r.leftoverFrames),
                static_cast<unsigned long long>(r.clipped), r.monoMismatch ? ", FIGYELEM: mono bal != jobb" : "");
    return (r.leftoverFrames || r.monoMismatch) ? 2 : 0;
}

int main(int argc, char** argv) {
    uint32_t rate = 44100;
    std::string preset = "reference";
    float headroom = 0.0f;
    int threads = 1;
    bool dir = false;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--rate") { const char* v = next(); if (!v) return usage(); rate = static_cast<uint32_t>(std::strtoul(v, nullptr, 10)); }
        else if (a == "--preset") { const char* v = next(); if (!v) return usage(); preset = v; }
        else if (a == "--headroom") { const char* v = next(); if (!v) return usage(); headroom = std::strtof(v, nullptr); }
        else if (a == "--threads") { const char* v = next(); if (!v) return usage(); threads = std::atoi(v); }
        else if (a == "--dir") dir = true;
        else if (a == "-h" || a == "--help") return usage();
        else pos.push_back(a);
    }
    if (pos.size() != 2 || rate < 8000 || rate > 768000) return usage();
    int p = -1;
    if (preset == "linear") p = 0; else if (preset == "intermediate") p = 1; else if (preset == "minimum") p = 2;
    else if (preset == "reference") p = audioopt::kUpsamplePresetReference;
    if (p < 0) return usage();

    audioopt::AudioOptions o;
    audioopt::applyUpsamplePreset(o, p);
    o.upsampleHeadroomDb = headroom;
    o.upsampleThreads = threads;
    audioopt::sanitize(o);
    std::printf("Beállítás: %s, sávszélesség %.1f%%, záró sáv %.0f%%, %d bit, fázis %.0f, fejtér %.0f dB\n", preset.c_str(),
                o.upsamplePassbandEnd * 100.0, o.upsampleStopbandBegin * 100.0, o.upsamplePrecision, o.upsamplePhase,
                static_cast<double>(o.upsampleHeadroomDb));

    if (!dir) return convertOne(pos[0], pos[1], rate, o);
    std::error_code ec;
    fs::create_directories(pos[1], ec);
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(pos[0], ec))
        if (e.is_regular_file() && (e.path().extension() == ".wav" || e.path().extension() == ".WAV")) files.push_back(e.path());
    if (ec || files.empty()) { std::fprintf(stderr, "HIBA: nincs .wav a mappában: %s\n", pos[0].c_str()); return 1; }
    std::sort(files.begin(), files.end());
    int worst = 0;
    for (const auto& f : files) {
        const int rc = convertOne(f.string(), (fs::path(pos[1]) / f.filename()).string(), rate, o);
        worst = std::max(worst, rc);
    }
    return worst;
}
