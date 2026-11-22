#pragma once
/**
 * @file log_debounce.hpp
 * @brief Állapotváltás-naplózás hiszterézissel (debounce), a rövid ki/be villogások elnyomására.
 *
 * Az első állapot azonnal naplózódik; későbbi állapotváltás csak akkor, ha legalább debounceMs
 * ideig fennmarad (ha közben visszatér a korábban naplózott állapotba, egyáltalán nem naplóz).
 */
#include <cstdint>

namespace aq::log {

class StableStateLogger {
public:
    explicit StableStateLogger(uint64_t debounceMs = 2500) : debounceMs_(debounceMs) {}

    /**
     * Állapot frissítése az aktuális monoton időbélyeggel (ms).
     * Igazzal tér vissza, ha most kell naplózni az outLoggedState értéket.
     */
    bool update(int state, uint64_t nowMs, int& outLoggedState) {
        if (!hasLogged_) {
            hasLogged_ = true;
            loggedState_ = state;
            pending_ = false;
            outLoggedState = state;
            return true;
        }
        if (state == loggedState_) {
            pending_ = false;
            return false;
        }
        if (!pending_ || pendingState_ != state) {
            pending_ = true;
            pendingState_ = state;
            pendingSinceMs_ = nowMs;
            return false;
        }
        if (nowMs >= pendingSinceMs_ && (nowMs - pendingSinceMs_) >= debounceMs_) {
            loggedState_ = state;
            pending_ = false;
            outLoggedState = state;
            return true;
        }
        return false;
    }

    bool hasLogged() const { return hasLogged_; }
    int loggedState() const { return loggedState_; }

private:
    uint64_t debounceMs_ = 2500;
    bool hasLogged_ = false;
    int loggedState_ = 0;
    bool pending_ = false;
    int pendingState_ = 0;
    uint64_t pendingSinceMs_ = 0;
};

} // namespace aq::log
