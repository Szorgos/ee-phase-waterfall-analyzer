#pragma once
/**
 * @file player_output_select.hpp
 * @brief Választó a beépített lejátszó két kimenete között: megosztott (Linux: PipeWire, az „Aqulify” eszközre, a
 *        rendszer DSP-útján) és kizárólagos (Linux: közvetlen ALSA). A választás a következő hangfolyam megnyitásától
 *        hat; ha a kizárólagos kimenet nem nyitható meg (foglalt eszköz, nem támogatott mintavétel…), a lejátszó a
 *        megosztottra lép vissza, és az okot megőrzi (fallback()).
 */
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "file_player.hpp"

namespace ee_analyzer::player {

class SelectablePlayerOutput : public PlayerOutput {
public:
    SelectablePlayerOutput(std::unique_ptr<PlayerOutput> shared, std::unique_ptr<PlayerOutput> exclusive)
        : shared_(std::move(shared)), exclusive_(std::move(exclusive)) {}
    ~SelectablePlayerOutput() override { close(); }

    /** A kizárólagos kimenet kérése (a következő megnyitástól). */
    void setExclusive(bool on) { want_ = on; }
    bool wantExclusive() const { return want_.load(); }
    /** Most a kizárólagos kimenet szól-e (nyitott hangfolyam). */
    bool exclusiveActive() const { return cur_.load() == exclusive_.get() && cur_.load() != nullptr; }
    /** A legutóbbi visszalépés oka (üres: nem volt). */
    std::string fallback() const { std::lock_guard<std::mutex> lk(m_); return fallback_; }
    PlayerOutput* exclusiveOutput() { return exclusive_.get(); }

    bool open(uint32_t sampleRate, RenderFn render, std::string* error) override {
        close();
        if (want_.load() && exclusive_) {
            std::string why;
            if (exclusive_->open(sampleRate, render, &why)) {
                setFallback("");
                cur_ = exclusive_.get();
                return true;
            }
            setFallback(why.empty() ? "a kizárólagos kimenet nem nyitható meg" : why);
        } else {
            setFallback("");
        }
        if (!shared_->open(sampleRate, std::move(render), error)) return false;
        cur_ = shared_.get();
        return true;
    }

    bool openDop(uint32_t dopSampleRate, RenderDopFn render, std::string* error) override {
        close();
        if (want_.load() && exclusive_) {
            std::string why;
            if (exclusive_->openDop(dopSampleRate, std::move(render), &why)) {
                setFallback("");
                cur_ = exclusive_.get();
                return true;
            }
            // DoP esetén nincs visszalépés a megosztott kimenetre (az nem támogatja a DoP-ot)
            const std::string fb = why.empty() ? "format" : why;
            setFallback(fb);
            if (error) *error = fb;
            return false;
        }
        setFallback("");
        if (error) *error = "notAllowed";
        return false;
    }
    void close() override {
        if (PlayerOutput* c = cur_.exchange(nullptr)) c->close();
    }
    void setActive(bool active) override { if (PlayerOutput* c = cur_.load()) c->setActive(active); }
    void lock() override { if (PlayerOutput* c = cur_.load()) c->lock(); else idle_.lock(); }
    void unlock() override { if (PlayerOutput* c = cur_.load()) c->unlock(); else idle_.unlock(); }
    void flush() override { if (PlayerOutput* c = cur_.load()) c->flush(); }
    uint64_t consumedFrames() const override { PlayerOutput* c = cur_.load(); return c ? c->consumedFrames() : UINT64_MAX; }
    bool settlesOnRateChange() const override { PlayerOutput* c = cur_.load(); return c && c->settlesOnRateChange(); }

private:
    void setFallback(const std::string& s) { std::lock_guard<std::mutex> lk(m_); fallback_ = s; }

    std::unique_ptr<PlayerOutput> shared_, exclusive_;
    std::atomic<PlayerOutput*> cur_{nullptr};
    std::atomic<bool> want_{false};
    std::mutex idle_;
    mutable std::mutex m_;
    std::string fallback_;
};

} // namespace ee_analyzer::player
