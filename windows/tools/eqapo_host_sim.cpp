/**
 * @file eqapo_host_sim.cpp
 * @brief Teszt-hoszt: úgy tölti be az Aqulify.dll-t, ahogy az Equalizer APO (VSTPluginInstance.cpp):
 *        VSTPluginMain -> effOpen -> paraméterek név szerint -> effSetSampleRate/BlockSize -> effMainsChanged(1)
 *        -> effStartProcess -> processReplacing (valós időben ütemezve) -> effStopProcess -> effClose.
 *
 * Használat: eqapo_host_sim.exe <Aqulify.dll> [másodperc=10] [mintavétel=48000]
 * Másodpercenként kiírja a kimenő jel szintjét 100 Hz / 1 kHz / 5 kHz-en (bemenet: három szinusz, -20 dBFS).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../plugin/vst2_abi.hpp"

static intptr_t hostCallback(vst2::AEffect*, int32_t opcode, int32_t, intptr_t, void*, float) {
    return opcode == vst2::audioMasterVersion ? 2400 : 0;
}

static double toneDb(const std::vector<float>& x, double f, double sr) {
    std::complex<double> acc(0, 0);
    const size_t n = x.size();
    for (size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2 * 3.14159265358979 * i / (n - 1));
        acc += w * x[i] * std::polar(1.0, -2 * 3.14159265358979 * f * i / sr);
    }
    const double amp = 2.0 * std::abs(acc) / (n * 0.5);
    return 20 * std::log10(std::max(amp, 1e-9) / 0.1);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("Használat: %s <Aqulify.dll> [másodperc] [mintavétel]\n", argv[0]);
        return 2;
    }
    const int seconds = argc > 2 ? std::atoi(argv[2]) : 10;
    const double sr = argc > 3 ? std::atof(argv[3]) : 48000.0;
    HMODULE lib = LoadLibraryA(argv[1]);
    if (!lib) { std::printf("HIBA: LoadLibrary (%lu)\n", GetLastError()); return 1; }
    using MainFn = vst2::AEffect* (*)(vst2::HostCallback);
    auto mainFn = reinterpret_cast<MainFn>(reinterpret_cast<void*>(GetProcAddress(lib, "VSTPluginMain")));
    if (!mainFn) { std::printf("HIBA: nincs VSTPluginMain export\n"); return 1; }
    vst2::AEffect* fx = mainFn(&hostCallback);
    if (!fx || fx->magic != vst2::kEffectMagic) { std::printf("HIBA: érvénytelen AEffect\n"); return 1; }
    fx->user = reinterpret_cast<void*>(0x1234); // az EQ APO ide teszi a saját mutatóját
    fx->dispatcher(fx, vst2::effOpen, 0, 0, nullptr, 0.0f);

    char name[256] = {};
    fx->dispatcher(fx, vst2::effGetEffectName, 0, 0, name, 0.0f);
    std::printf("Plugin: %s, %d be / %d ki, %d paraméter, flags=0x%x, canReplacing=%d\n", name, fx->numInputs,
                fx->numOutputs, fx->numParams, fx->flags, (fx->flags & vst2::effFlagsCanReplacing) ? 1 : 0);
    for (int i = 0; i < fx->numParams; ++i) {
        char pn[256] = {};
        fx->dispatcher(fx, vst2::effGetParamName, i, 0, pn, 0.0f);
        // mint az EQ APO config sora: "Bypass 0 Remote 1"
        if (std::strcmp(pn, "Bypass") == 0) fx->setParameter(fx, i, 0.0f);
        if (std::strcmp(pn, "Remote") == 0) fx->setParameter(fx, i, 1.0f);
        char disp[64] = {};
        fx->dispatcher(fx, vst2::effGetParamDisplay, i, 0, disp, 0.0f);
        std::printf("  paraméter %d: %s = %s\n", i, pn, disp);
    }
    const int block = static_cast<int>(sr / 100); // 10 ms, mint a Windows audio motor
    fx->dispatcher(fx, vst2::effSetSampleRate, 0, 0, nullptr, static_cast<float>(sr));
    fx->dispatcher(fx, vst2::effSetBlockSize, 0, block, nullptr, 0.0f);
    fx->dispatcher(fx, vst2::effMainsChanged, 0, 1, nullptr, 0.0f);
    fx->dispatcher(fx, vst2::effStartProcess, 0, 0, nullptr, 0.0f);
    std::fflush(stdout);

    std::vector<float> inL(block), inR(block), outL(block), outR(block), secOut;
    float* ins[2] = { inL.data(), inR.data() };
    float* outs[2] = { outL.data(), outR.data() };
    double ph1 = 0, ph2 = 0, ph3 = 0;
    LARGE_INTEGER freq, t0, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    long long processed = 0;
    for (int s = 0; s < seconds; ++s) {
        secOut.clear();
        for (int b = 0; b < 100; ++b) {
            for (int i = 0; i < block; ++i) {
                const float v = static_cast<float>(0.1 / 3.0 * (std::sin(ph1) + std::sin(ph2) + std::sin(ph3)));
                inL[i] = inR[i] = v;
                ph1 += 2 * 3.14159265358979 * 100 / sr;
                ph2 += 2 * 3.14159265358979 * 1000 / sr;
                ph3 += 2 * 3.14159265358979 * 5000 / sr;
            }
            fx->processReplacing(fx, ins, outs, block);
            secOut.insert(secOut.end(), outL.begin(), outL.end());
            processed += block;
            // valós idejű ütemezés
            for (;;) {
                QueryPerformanceCounter(&now);
                const double el = static_cast<double>(now.QuadPart - t0.QuadPart) / freq.QuadPart;
                if (el * sr >= processed) break;
                Sleep(1);
            }
        }
        // bemenet: mindhárom szinusz 0.1/3 amplitúdó -> referencia -9.54 dB a 0.1-hez képest
        const double ref = 20 * std::log10(1.0 / 3.0);
        std::printf("t=%2ds  100Hz %+6.2f dB  1kHz %+6.2f dB  5kHz %+6.2f dB\n", s + 1,
                    toneDb(secOut, 100, sr) - ref, toneDb(secOut, 1000, sr) - ref, toneDb(secOut, 5000, sr) - ref);
        std::fflush(stdout);
    }
    fx->dispatcher(fx, vst2::effStopProcess, 0, 0, nullptr, 0.0f);
    fx->dispatcher(fx, vst2::effMainsChanged, 0, 0, nullptr, 0.0f);
    fx->dispatcher(fx, vst2::effClose, 0, 0, nullptr, 0.0f);
    FreeLibrary(lib);
    std::printf("Kész.\n");
    return 0;
}
