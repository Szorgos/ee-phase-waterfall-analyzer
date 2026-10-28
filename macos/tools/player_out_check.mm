/**
 * @file player_out_check.mm
 * @brief A beépített lejátszó macOS-es hangkimenetének (CoreAudio) próbája egy NÉMA fájllal: megnyitás az
 *        alapértelmezett kimeneti eszközön, a pozíció valós idejű haladása, szünet, tekerés, a szám vége.
 *        Hang nem szól (a minták nullák), így bármikor futtatható. Hangeszköz nélkül (pl. CI) kihagyja a próbát.
 *
 *   player_out_check [mintavételi frekvencia, alapból 44100]
 */
#import <Foundation/Foundation.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include "player_output_coreaudio.hpp"

using namespace ee_analyzer::player;

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_fail; std::printf("  [HIBA] " __VA_ARGS__); } else { std::printf("  [OK]   " __VA_ARGS__); } std::printf("\n"); } while (0)

int main(int argc, char** argv) {
    @autoreleasepool {
        const uint32_t sr = argc > 1 ? static_cast<uint32_t>(std::atoi(argv[1])) : 44100;
        std::printf("[CoreAudio] A lejátszó hangkimenete, néma, 3 s-os, %u Hz-es fájllal\n", sr);

        // Néma, 3 s-os sztereó float WAV
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

        FilePlayer p(std::make_unique<CoreAudioPlayerOutput>());
        std::string err;
        if (!p.open(path, &err)) {
            std::printf("  [KIHAGYVA] nincs használható lejátszóeszköz: %s\n", err.c_str());
            std::error_code ec;
            std::filesystem::remove(path, ec);
            return 0;
        }

        const auto t0 = std::chrono::steady_clock::now();
        auto since = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        const double p1 = p.position(), w1 = since();
        CHECK(p.state() == FilePlayer::State::Playing && std::abs(p1 - w1) < 0.25,
              "pozíció valós időben halad: %.3f s (eltelt %.3f s, eltérés %.3f s)", p1, w1, p1 - w1);

        p.pause();
        const double pPause = p.position();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        CHECK(p.state() == FilePlayer::State::Paused && std::abs(p.position() - pPause) < 0.05,
              "szünetben a pozíció áll (%.3f s)", p.position());

        p.seek(2.5);
        p.play();
        CHECK(std::abs(p.position() - 2.5) < 0.15, "tekerés után a pozíció %.3f s (cél 2.5 s)", p.position());

        // A fájl végéig (kb. 0,5 s) várunk, legfeljebb 1,5 s
        const auto waitEnd = std::chrono::steady_clock::now();
        while (p.state() == FilePlayer::State::Playing &&
               std::chrono::duration<double>(std::chrono::steady_clock::now() - waitEnd).count() < 1.5) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        CHECK(p.state() == FilePlayer::State::Ended, "a szám végén Ended állapotba lép");

        p.stop();
        std::error_code ec;
        std::filesystem::remove(path, ec);

        if (g_fail == 0) {
            std::printf("[CoreAudio] Sikeres ellenőrzés (0 hiba).\n");
        } else {
            std::printf("[CoreAudio] Sikertelen ellenőrzés (%d hiba).\n", g_fail);
        }
        return g_fail;
    }
}
