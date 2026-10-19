#pragma once
#include <vector>
#include <atomic>
#include <algorithm>
#include <cstddef>

namespace ee_analyzer {

/**
 * @brief Zármentes (Lock-free) Single-Producer Single-Consumer (SPSC) Körpuffer
 * Valós idejű audio szál $\to$ Grafikai analizátor szál közötti adatátvitelre.
 * Garantálja a zéró memóriafoglalást és a hangkimaradás-mentes (xrun-free) működést.
 */
template <typename T>
class LockFreeRingBuffer {
private:
    std::vector<T> buffer;
    size_t capacity = 0;
    
    // Atomi író és olvasó indexek memóriarendezési korlátokkal
    alignas(64) std::atomic<size_t> writeIndex{0};
    alignas(64) std::atomic<size_t> readIndex{0};

public:
    explicit LockFreeRingBuffer(size_t size = 16384)
        : buffer(size + 1), capacity(size + 1) {}

    /**
     * @brief Újraméretezés (csak inicializáláskor hívható, nem valós idejű szálban!)
     */
    void resize(size_t size) {
        capacity = size + 1;
        buffer.assign(capacity, T{});
        writeIndex.store(0, std::memory_order_relaxed);
        readIndex.store(0, std::memory_order_relaxed);
    }

    /**
     * @brief Elérhető (olvasható) elemek száma
     */
    size_t getAvailableRead() const {
        const size_t w = writeIndex.load(std::memory_order_acquire);
        const size_t r = readIndex.load(std::memory_order_relaxed);
        if (w >= r) {
            return w - r;
        }
        return capacity - (r - w);
    }

    /**
     * @brief Szabad hely (írható elemek száma)
     */
    size_t getAvailableWrite() const {
        return (capacity - 1) - getAvailableRead();
    }

    /**
     * @brief Elemek beírása a pufferbe (Audio Real-time szál hívja)
     * Ha nincs elég hely, annyit ír be amennyi elfér, de SOHA nem blokkolja a szálat!
     * @return A ténylegesen beírt elemek száma
     */
    size_t write(const T* data, size_t count) {
        const size_t available = getAvailableWrite();
        const size_t toWrite = std::min(count, available);
        if (toWrite == 0) return 0;

        size_t w = writeIndex.load(std::memory_order_relaxed);

        for (size_t i = 0; i < toWrite; ++i) {
            buffer[w] = data[i];
            w = (w + 1) % capacity;
        }

        // Felszabadító (release) memóriakorlát az olvasó szál felé
        writeIndex.store(w, std::memory_order_release);
        return toWrite;
    }

    /**
     * @brief Elemek kiolvasása a pufferből (Grafikai / FFT szál hívja)
     * @return A ténylegesen kiolvasott elemek száma
     */
    size_t read(T* dest, size_t count) {
        const size_t available = getAvailableRead();
        const size_t toRead = std::min(count, available);
        if (toRead == 0) return 0;

        size_t r = readIndex.load(std::memory_order_relaxed);

        for (size_t i = 0; i < toRead; ++i) {
            dest[i] = buffer[r];
            r = (r + 1) % capacity;
        }

        // Felszabadító memóriakorlát az író szál felé
        readIndex.store(r, std::memory_order_release);
        return toRead;
    }

    /**
     * @brief Puffer ürítése
     */
    void clear() {
        const size_t w = writeIndex.load(std::memory_order_relaxed);
        readIndex.store(w, std::memory_order_release);
    }
};

} // namespace ee_analyzer
