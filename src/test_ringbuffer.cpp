#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include <cassert>
#include "ringbuffer.hpp"

using namespace ee_analyzer;

int main() {
    std::cout << "========================================================\n";
    std::cout << " EasyEffects Zármentes (Lock-free) Ringbuffer Teszt\n";
    std::cout << " (Valós idejű Audio szál -> 3D Grafikai szál szimuláció)\n";
    std::cout << "========================================================\n\n";

    LockFreeRingBuffer<float> ringBuffer(8192);

    const size_t totalSamples = 48000 * 2; // 2 másodpercnyi hang (96 000 minta)
    const size_t blockSize = 128;          // Tipikus PipeWire audio blokkméret

    std::atomic<bool> producerFinished{false};
    std::atomic<size_t> totalReceived{0};

    // 1. Audio Szál (Író / Producer)
    std::thread audioThread([&]() {
        std::vector<float> block(blockSize);
        size_t samplesSent = 0;

        while (samplesSent < totalSamples) {
            for (size_t i = 0; i < blockSize; ++i) {
                block[i] = static_cast<float>(samplesSent + i);
            }
            // Zármentes beírás a gyűrűpufferbe
            size_t written = ringBuffer.write(block.data(), blockSize);
            samplesSent += written;

            // Szimuláljuk a valós idejű órajelet (~2.6 ms egy 128-as blokk 48 kHz-en)
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
        producerFinished.store(true);
    });

    // 2. Grafikai / 3D Vízesés Szál (Olvasó / Consumer)
    std::thread guiThread([&]() {
        std::vector<float> readBuffer(256);
        size_t lastVal = 0;
        bool first = true;

        while (!producerFinished.load() || ringBuffer.getAvailableRead() > 0) {
            size_t readCount = ringBuffer.read(readBuffer.data(), readBuffer.size());
            if (readCount > 0) {
                for (size_t i = 0; i < readCount; ++i) {
                    size_t val = static_cast<size_t>(readBuffer[i]);
                    if (!first) {
                        assert(val == lastVal + 1 && "Adatintegritási hiba a gyűrűpufferben!");
                    }
                    lastVal = val;
                    first = false;
                    totalReceived.fetch_add(1);
                }
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }
    });

    audioThread.join();
    guiThread.join();

    std::cout << "Elküldött minták:  " << totalSamples << "\n";
    std::cout << "Fogadott minták:   " << totalReceived.load() << "\n";
    std::cout << "Kiesett minták:    " << (totalSamples - totalReceived.load()) << " (Zéró xrun)\n\n";

    if (totalReceived.load() == totalSamples) {
        std::cout << ">> SIKER: A zármentes körpuffer 100%-os adatintegritással átvitte a hangmintákat!\n";
        return 0;
    } else {
        std::cerr << ">> HIBA: Mintaeltérés történt!\n";
        return 1;
    }
}
