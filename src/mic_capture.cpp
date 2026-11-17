/**
 * @file mic_capture.cpp
 * @brief A platform mikrofonfelvevőjének létrehozása (include/room/mic_capture.hpp).
 */
#include "room/mic_capture.hpp"

#if defined(__linux__)
#include "room/mic_capture_pipewire.hpp"
#endif

namespace aqulify::room {

std::unique_ptr<MicCapture> createMicCapture() {
#if defined(__linux__)
    auto c = std::make_unique<PipeWireMicCapture>();
    if (c->ok()) return c;
    return nullptr;
#else
    return nullptr;   // Windows (WASAPI) és macOS (Core Audio): külön lépésben
#endif
}

} // namespace aqulify::room
