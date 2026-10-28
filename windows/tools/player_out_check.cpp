/**
 * @file player_out_check.cpp
 * @brief A beépített lejátszó Windows-os hangkimenetének (WASAPI) próbája egy NÉMA fájllal: megnyitás az
 *        alapértelmezett lejátszóeszközön, a pozíció valós idejű haladása, szünet, tekerés, a szám vége.
 *        Hang nem szól (a minták nullák), így bármikor futtatható. Hangeszköz nélkül (pl. CI) kihagyja a próbát.
 *
 *   player_out_check.exe [mintavételi frekvencia, alapból 44100]
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include "../engine/player_output_wasapi.hpp"

using namespace ee_analyzer::player;

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_fail; std::printf("  [HIBA] " __VA_ARGS__); } else { std::printf("  [OK]   " __VA_ARGS__); } std::printf("\n"); } while (0)

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    const uint32_t sr = argc > 1 ? static_cast<uint32_t>(std::atoi(argv[1])) : 44100;
    std::printf("[WASAPI] A lejátszó hangkimenete, néma, 3 s-os, %u Hz-es fájllal\n", sr);
    // néma, 3 s-os sztereó float WAV
    const std::string path = (std::filesystem::temp_directory_path() / "aq_player_silent.wav").string();
    {
        std::ofstream f(path, std::ios::binary);
        auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
        auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
        const uint32_t frames = 3 * sr, data = frames * 8;
        f.write("RIFF", 4); u32(36 + data); f.write("WAVEfmt ", 8); u32(16); u16(3); u16(2); u32(sr); u32(sr * 8); u16(8); u16(32);
        f.write("data", 4); u32(data);
        std::vector<char> zeros(data, 0);
        f.write(zeros.data(), data);
    }
    FilePlayer p(std::make_unique<WasapiPlayerOutput>());
    std::string err;
    if (!p.open(path, &err)) {
        std::printf("  [KIHAGYVA] nincs használható lejátszóeszköz: %s\n", err.c_str());
        std::filesystem::remove(path);
        return 0;
    }
    const auto t0 = std::chrono::steady_clock::now();
    auto since = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    const double p1 = p.position(), w1 = since();
    CHECK(p.state() == FilePlayer::State::Playing && std::abs(p1 - w1) < 0.25,
          "1 s után a pozíció %.3f s (eltelt %.3f s): valós időben halad", p1, w1);
    p.pause();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    CHECK(std::abs(p.position() - p1) < 0.15 && !p.info().playing, "szünet: a pozíció %.3f s marad", p.position());
    p.seek(2.0);
    p.play();
    const auto t1 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(std::abs(p.position() - 2.4) < 0.25, "tekerés 2,0 s-ra, 0,4 s lejátszás után %.3f s", p.position());
    bool ended = false;
    for (int i = 0; i < 300 && !ended; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        ended = p.state() == FilePlayer::State::Ended;
    }
    const double tail = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
    CHECK(ended && std::abs(p.position() - 3.0) < 1e-6 && tail > 0.8 && tail < 1.5,
          "a fájl végén vége-állapot (a maradék 1,0 s %.2f s alatt szólt el), pozíció %.3f s", tail, p.position());
    p.stop();
    std::filesystem::remove(path);
    std::printf("\n%s (%d hiba)\n", g_fail ? "SIKERTELEN" : "MINDEN PRÓBA SIKERES", g_fail);
    return g_fail ? 1 : 0;
}
