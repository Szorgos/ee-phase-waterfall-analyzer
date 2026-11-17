/**
 * @file mic_capture.cpp
 * @brief A platform mikrofonfelvevőjének létrehozása (include/room/mic_capture.hpp).
 */
#include "room/mic_capture.hpp"

#if defined(_WIN32)
#include "room/mic_capture_wasapi.hpp"
#elif defined(__APPLE__)
#include "room/mic_capture_coreaudio.hpp"
#elif defined(__linux__)
#include "room/mic_capture_pipewire.hpp"
#endif

namespace aqulify::room {

std::unique_ptr<MicCapture> createMicCapture() {
#if defined(_WIN32)
    return std::make_unique<WasapiMicCapture>();
#elif defined(__APPLE__)
    return std::make_unique<CoreAudioMicCapture>();
#elif defined(__linux__)
    auto c = std::make_unique<PipeWireMicCapture>();
    if (c->ok()) return c;
    return nullptr;
#else
    return nullptr;
#endif
}

} // namespace aqulify::room
