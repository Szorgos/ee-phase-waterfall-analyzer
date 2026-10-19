#pragma once
/**
 * @file win_shared.hpp
 * @brief Windows-specifikus segédek: közös memória (plugin <-> motor) és a beállítások tartós tárolása.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#include <new>
#include <cstring>
#include <string>
#include "shared_state.hpp"

namespace ee_analyzer::win {

/**
 * Biztonsági leíró a közös memóriához:
 *  - mindenki (WD), a SYSTEM (SY) és a LOCAL SERVICE (LS) teljes hozzáférést kap,
 *  - alacsony integritási címke (LW, no-write-up): így a normál felhasználói jogú AetherAudio.exe
 *    is írhatja az audiodg.exe (szolgáltatás) által létrehozott objektumot.
 */
struct ScopedSecurity {
    SECURITY_ATTRIBUTES sa{};
    PSECURITY_DESCRIPTOR sd = nullptr;
    explicit ScopedSecurity(const wchar_t* sddl) {
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = FALSE;
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr)) {
            sa.lpSecurityDescriptor = sd;
        }
    }
    ~ScopedSecurity() { if (sd) LocalFree(sd); }
    SECURITY_ATTRIBUTES* get() { return sa.lpSecurityDescriptor ? &sa : nullptr; }
};

class SharedMapping {
public:
    SharedMapping() = default;
    ~SharedMapping() { close(); }
    SharedMapping(const SharedMapping&) = delete;
    SharedMapping& operator=(const SharedMapping&) = delete;

    /**
     * Plugin oldal: létrehozás vagy csatlakozás. Előbb a Global\, majd a Local\ névteret próbálja.
     * @param createdNew true, ha ez a hívás hozta létre (ekkor inicializálni kell)
     */
    bool create(bool& createdNew) {
        close();
        ScopedSecurity sec(L"D:(A;;GA;;;WD)(A;;GA;;;SY)(A;;GA;;;LS)S:(ML;;NW;;;LW)");
        const DWORD size = static_cast<DWORD>(sizeof(AetherSharedState));
        for (const wchar_t* name : { kSharedNameGlobal, kSharedNameLocal }) {
            HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, sec.get(), PAGE_READWRITE, 0, size, name);
            if (!h) continue;
            createdNew = GetLastError() != ERROR_ALREADY_EXISTS;
            if (mapView(h)) {
                isGlobal_ = (name == kSharedNameGlobal);
                if (createdNew) initialize();
                return true;
            }
            CloseHandle(h);
        }
        return false;
    }

    /** Motor oldal: csatlakozás egy már létező leképezéshez (Global\, majd Local\). */
    bool open() {
        close();
        for (const wchar_t* name : { kSharedNameGlobal, kSharedNameLocal }) {
            HANDLE h = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
            if (!h) continue;
            if (mapView(h)) {
                if (state_->magic == kSharedMagic && state_->version == kSharedVersion) {
                    isGlobal_ = (name == kSharedNameGlobal);
                    return true;
                }
            }
            close();
        }
        return false;
    }

    void close() {
        if (state_) UnmapViewOfFile(state_);
        state_ = nullptr;
        if (handle_) CloseHandle(handle_);
        handle_ = nullptr;
    }

    AetherSharedState* state() const { return state_; }
    bool isGlobal() const { return isGlobal_; }

private:
    bool mapView(HANDLE h) {
        void* view = MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(AetherSharedState));
        if (!view) return false;
        handle_ = h;
        state_ = static_cast<AetherSharedState*>(view);
        return true;
    }

    void initialize() {
        // A CreateFileMapping nullázott lapokat ad; az atomikat placement new-val "élesítjük".
        new (&state_->sampleRate) std::atomic<uint32_t>(48000);
        new (&state_->ownerToken) std::atomic<uint32_t>(0);
        new (&state_->ownerTickMs) std::atomic<uint64_t>(0);
        new (&state_->activeInstances) std::atomic<uint32_t>(0);
        new (&state_->gainReductionDb) std::atomic<float>(0.0f);
        new (&state_->tap.writePos) std::atomic<uint64_t>(0);
        new (&state_->params.seq) std::atomic<uint32_t>(0);
        state_->version = kSharedVersion;
        std::atomic_thread_fence(std::memory_order_release);
        state_->magic = kSharedMagic;
    }

    HANDLE handle_ = nullptr;
    AetherSharedState* state_ = nullptr;
    bool isGlobal_ = false;
};

// ---------------------------------------------------------------- tartós beállítások

constexpr uint32_t kParamFileMagic = 0x50454141; // 'AAEP'
constexpr uint32_t kParamFileVersion = 1;

struct ParamFile {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    ParamData data;
};

/** C:\ProgramData\AetherAudio\params.bin – a motor írja, a plugin (LOCAL SERVICE) is olvashatja. */
inline std::wstring paramFilePath(bool createDir) {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring(L"C:\\ProgramData");
    dir += L"\\AetherAudio";
    if (createDir) {
        // LS (LOCAL SERVICE = audiodg.exe) is írhat ide, hogy a plugin állapotnaplót hagyhasson.
        ScopedSecurity sec(L"D:(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;IU)(A;OICI;FA;;;LS)(A;OICI;FR;;;WD)");
        CreateDirectoryW(dir.c_str(), sec.get());
    }
    return dir + L"\\params.bin";
}

inline bool loadParamFile(ParamData& out) {
    const std::wstring path = paramFilePath(false);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    ParamFile pf{};
    DWORD read = 0;
    const BOOL ok = ReadFile(f, &pf, sizeof(pf), &read, nullptr);
    CloseHandle(f);
    if (!ok || read != sizeof(pf) || pf.magic != kParamFileMagic || pf.version != kParamFileVersion ||
        pf.size != sizeof(ParamData)) {
        return false;
    }
    if (pf.data.numBands > kMaxEqBands) pf.data.numBands = kMaxEqBands;
    out = pf.data;
    return true;
}

inline bool saveParamFile(const ParamData& d) {
    const std::wstring path = paramFilePath(true);
    const std::wstring tmp = path + L".tmp";
    ParamFile pf{ kParamFileMagic, kParamFileVersion, static_cast<uint32_t>(sizeof(ParamData)), d };
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(f, &pf, sizeof(pf), &written, nullptr);
    CloseHandle(f);
    if (!ok || written != sizeof(pf)) return false;
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

} // namespace ee_analyzer::win
