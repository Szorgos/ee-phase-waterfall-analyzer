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
#include <vector>
#include "shared_state.hpp"
#include "ir_data.hpp"

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
        new (&state_->ir.seq) std::atomic<uint32_t>(0);
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
constexpr uint32_t kParamFileVersion = 2;

struct ParamFile {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    ParamData data;
};

/** C:\ProgramData\AetherAudio mappa (opcionálisan létrehozva a megfelelő jogosultságokkal). */
inline std::wstring dataDirectory(bool createDir);

/** C:\ProgramData\AetherAudio\params.bin – a motor írja, a plugin (LOCAL SERVICE) is olvashatja. */
inline std::wstring paramFilePath(bool createDir) {
    return dataDirectory(createDir) + L"\\params.bin";
}

inline std::wstring dataDirectory(bool createDir) {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring(L"C:\\ProgramData");
    dir += L"\\AetherAudio";
    if (createDir) {
        // LS (LOCAL SERVICE = audiodg.exe) is írhat ide, hogy a plugin állapotnaplót hagyhasson.
        ScopedSecurity sec(L"D:(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;IU)(A;OICI;FA;;;LS)(A;OICI;FR;;;WD)");
        CreateDirectoryW(dir.c_str(), sec.get());
    }
    return dir;
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

namespace ee_analyzer::win {

// ---------------------------------------------------------------- impulzusválasz (konvolver)

/** Seqlock írás a közös IR blokkba (motor oldal, nem valós idejű). */
inline void writeIrBlock(IrBlock& blk, const IrData& ir) {
    const uint32_t s = blk.seq.load(std::memory_order_relaxed);
    blk.seq.store(s | 1u, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    const uint32_t frames = static_cast<uint32_t>(std::min<size_t>(ir.frames(), kMaxIrFrames));
    const uint32_t ch = ir.channels();
    blk.sampleRate = ir.sampleRate;
    blk.channels = ch;
    blk.frames = frames;
    std::memset(blk.name, 0, kIrNameLen);
    std::memcpy(blk.name, ir.name.data(), std::min<size_t>(ir.name.size(), kIrNameLen - 1));
    for (uint32_t i = 0; i < frames; ++i) {
        blk.data[static_cast<size_t>(i) * ch] = ir.left[i];
        if (ch == 2) blk.data[static_cast<size_t>(i) * 2 + 1] = ir.right[i];
    }
    std::atomic_thread_fence(std::memory_order_release);
    blk.seq.store((s | 1u) + 1u, std::memory_order_release);
}

/** Seqlock olvasás (plugin háttérszál). @return true, ha konzisztens másolat készült. */
inline bool readIrBlock(const IrBlock& blk, IrData& out, uint32_t& outSeq) {
    const uint32_t s1 = blk.seq.load(std::memory_order_acquire);
    if (s1 & 1u) return false;
    const uint32_t frames = std::min(blk.frames, kMaxIrFrames);
    const uint32_t ch = std::min<uint32_t>(blk.channels, 2);
    const uint32_t sr = blk.sampleRate;
    char name[kIrNameLen];
    std::memcpy(name, blk.name, kIrNameLen);
    name[kIrNameLen - 1] = '\0';
    std::vector<float> data(static_cast<size_t>(frames) * (ch ? ch : 1));
    if (frames && ch) std::memcpy(data.data(), blk.data, data.size() * sizeof(float));
    std::atomic_thread_fence(std::memory_order_acquire);
    if (blk.seq.load(std::memory_order_relaxed) != s1) return false;
    out = (frames && ch) ? IrData::fromInterleaved(data.data(), frames, ch, sr, name) : IrData{};
    outSeq = s1;
    return true;
}

constexpr uint32_t kIrFileMagic = 0x46524941; // 'AIRF'

/** C:\ProgramData\AetherAudio\ir.bin – a GUI-ból betöltött IR, hogy a plugin újraindítás után is megkapja. */
inline bool saveIrFile(const IrData& ir) {
    const std::wstring path = dataDirectory(true) + L"\\ir.bin";
    if (ir.empty()) { DeleteFileW(path.c_str()); return true; }
    const std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const uint32_t hdr[6] = { kIrFileMagic, 1u, ir.sampleRate, ir.channels(), static_cast<uint32_t>(ir.frames()),
                              static_cast<uint32_t>(ir.name.size()) };
    DWORD w = 0;
    bool ok = WriteFile(f, hdr, sizeof(hdr), &w, nullptr) && w == sizeof(hdr);
    ok = ok && WriteFile(f, ir.name.data(), static_cast<DWORD>(ir.name.size()), &w, nullptr);
    ok = ok && WriteFile(f, ir.left.data(), static_cast<DWORD>(ir.left.size() * 4), &w, nullptr);
    if (!ir.right.empty()) ok = ok && WriteFile(f, ir.right.data(), static_cast<DWORD>(ir.right.size() * 4), &w, nullptr);
    CloseHandle(f);
    return ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

inline bool loadIrFile(IrData& out) {
    const std::wstring path = dataDirectory(false) + L"\\ir.bin";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    uint32_t hdr[6] = {};
    DWORD r = 0;
    bool ok = ReadFile(f, hdr, sizeof(hdr), &r, nullptr) && r == sizeof(hdr) && hdr[0] == kIrFileMagic &&
              hdr[1] == 1u && (hdr[3] == 1 || hdr[3] == 2) && hdr[4] > 0 && hdr[4] <= kMaxIrFrames && hdr[5] < 4096;
    IrData ir;
    if (ok) {
        ir.sampleRate = hdr[2];
        ir.name.resize(hdr[5]);
        ok = ReadFile(f, ir.name.data(), hdr[5], &r, nullptr) && r == hdr[5];
        ir.left.resize(hdr[4]);
        ok = ok && ReadFile(f, ir.left.data(), hdr[4] * 4, &r, nullptr) && r == hdr[4] * 4;
        if (ok && hdr[3] == 2) {
            ir.right.resize(hdr[4]);
            ok = ReadFile(f, ir.right.data(), hdr[4] * 4, &r, nullptr) && r == hdr[4] * 4;
        }
    }
    CloseHandle(f);
    if (ok) out = std::move(ir);
    return ok;
}

} // namespace ee_analyzer::win
