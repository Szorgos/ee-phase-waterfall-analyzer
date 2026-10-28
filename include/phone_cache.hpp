#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <chrono>

namespace ee_analyzer::player {

namespace fs = std::filesystem;

inline std::string toUtf8Path(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

inline fs::path fromUtf8Path(const std::string& s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

/**
 * AEPF (Aqulify Engine Phone File) bináris keret adatai.
 * 28 bájtos fix fejléc:
 * 0..3:   "AEPF"
 * 4:      verzió (1)
 * 5:      flags (0x01 = EOF)
 * 6:      idLen (1..64)
 * 7:      extLen (1..16)
 * 8..15:  offset (uint64 little-endian)
 * 16..23: totalSize (uint64 little-endian)
 * 24..27: dataLen (uint32 little-endian)
 * 28..:   id + ext + nyers audio adat
 */
struct AepfHeader {
    uint8_t version = 1;
    uint8_t flags = 0;       // bit 0 = 0x01 (EOF)
    std::string id;
    std::string ext;
    uint64_t offset = 0;
    uint64_t totalSize = 0;
    const char* data = nullptr;
    size_t dataLen = 0;
    bool eof() const { return (flags & 0x01) != 0; }
};

inline bool parseAepfFrame(const std::string& buf, AepfHeader& out) {
    if (buf.size() < 28) return false;
    if (std::memcmp(buf.data(), "AEPF", 4) != 0) return false;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(buf.data());
    out.version = p[4];
    if (out.version != 1) return false;
    out.flags = p[5];
    const uint8_t idLen = p[6];
    const uint8_t extLen = p[7];
    if (idLen == 0 || idLen > 64 || extLen == 0 || extLen > 16) return false;

    auto readU64 = [](const uint8_t* b) -> uint64_t {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(b[i]) << (i * 8);
        return v;
    };
    auto readU32 = [](const uint8_t* b) -> uint32_t {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(b[i]) << (i * 8);
        return v;
    };

    out.offset = readU64(p + 8);
    out.totalSize = readU64(p + 16);
    const uint32_t dataLen = readU32(p + 24);

    const size_t headerSize = 28 + idLen + extLen;
    if (buf.size() < headerSize + dataLen) return false;

    out.id.assign(reinterpret_cast<const char*>(p + 28), idLen);
    out.ext.assign(reinterpret_cast<const char*>(p + 28 + idLen), extLen);
    out.data = reinterpret_cast<const char*>(p + headerSize);
    out.dataLen = dataLen;
    return true;
}

inline std::string buildAepfFrame(uint8_t flags, const std::string& id, const std::string& ext,
                                 uint64_t offset, uint64_t totalSize, const char* data, size_t dataLen) {
    std::string out;
    const uint8_t idLen = static_cast<uint8_t>(std::min<size_t>(id.size(), 64));
    const uint8_t extLen = static_cast<uint8_t>(std::min<size_t>(ext.size(), 16));
    const size_t headerSize = 28 + idLen + extLen;
    out.resize(headerSize + dataLen);
    uint8_t* p = reinterpret_cast<uint8_t*>(out.data());
    std::memcpy(p, "AEPF", 4);
    p[4] = 1; // version
    p[5] = flags;
    p[6] = idLen;
    p[7] = extLen;

    auto writeU64 = [](uint8_t* b, uint64_t v) {
        for (int i = 0; i < 8; ++i) b[i] = static_cast<uint8_t>((v >> (i * 8)) & 0xFF);
    };
    auto writeU32 = [](uint8_t* b, uint32_t v) {
        for (int i = 0; i < 4; ++i) b[i] = static_cast<uint8_t>((v >> (i * 8)) & 0xFF);
    };

    writeU64(p + 8, offset);
    writeU64(p + 16, totalSize);
    writeU32(p + 24, static_cast<uint32_t>(dataLen));

    std::memcpy(p + 28, id.data(), idLen);
    std::memcpy(p + 28 + idLen, ext.data(), extLen);
    if (dataLen > 0 && data) {
        std::memcpy(p + headerSize, data, dataLen);
    }
    return out;
}

/**
 * Telefonos zeneátvitel gyorsítótára (4b).
 *
 * A telefonról érkező bitpontos hangfájlok átmeneti tárolása a lemezen
 * (~/.config/aqulify/phone-cache vagy %APPDATA%\Aqulify\phone-cache).
 * LRU méretkorláttal (alapértelmezés: 2 GB).
 */
class PhoneCache {
public:
    static constexpr size_t kMaxCacheBytes = 2ull * 1024 * 1024 * 1024; // 2 GB
    static constexpr size_t kMaxTrackBytes = 2ull * 1024 * 1024 * 1024; // 2 GB / fájl

    explicit PhoneCache(std::string cacheDir = "") : dir_(std::move(cacheDir)) {
        if (!dir_.empty()) {
            std::error_code ec;
            fs::create_directories(fromUtf8Path(dir_), ec);
            cleanStalePartsLocked();
        }
    }

    void setDirectory(std::string dir) {
        std::lock_guard<std::mutex> lk(m_);
        dir_ = std::move(dir);
        if (!dir_.empty()) {
            std::error_code ec;
            fs::create_directories(fromUtf8Path(dir_), ec);
            cleanStalePartsLocked();
        }
    }

    std::string directory() const {
        std::lock_guard<std::mutex> lk(m_);
        return dir_;
    }

    static bool sanitizeId(const std::string& id) {
        if (id.empty() || id.size() > 64) return false;
        for (char c : id) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
                return false;
        }
        return true;
    }

    static bool sanitizeExt(const std::string& ext) {
        for (const char* ok : { "flac", "wav", "mp3", "ogg", "m4a", "aac", "opus", "wv" }) {
            if (ext == ok) return true;
        }
        return false;
    }

    bool hasTrack(const std::string& id, const std::string& ext, uint64_t expectedSize) const {
        std::lock_guard<std::mutex> lk(m_);
        if (dir_.empty() || !sanitizeId(id) || !sanitizeExt(ext)) return false;
        fs::path p = fromUtf8Path(dir_) / (id + "." + ext);
        std::error_code ec;
        if (fs::exists(p, ec) && fs::is_regular_file(p, ec)) {
            if (expectedSize == 0 || fs::file_size(p, ec) == expectedSize) return true;
        }
        return false;
    }

    bool hasTrack(const std::string& id) const {
        std::string dummy;
        return resolve(id, dummy);
    }

    void setAlias(const std::string& phoneId, const std::string& targetPath) {
        std::lock_guard<std::mutex> lk(m_);
        aliases_[phoneId] = targetPath;
        cv_.notify_all();
    }

    bool resolveAlias(const std::string& phoneId, std::string& outPath) const {
        std::lock_guard<std::mutex> lk(m_);
        return resolveAliasLocked(phoneId, outPath);
    }

    bool resolve(const std::string& id, std::string& outPath) const {
        std::lock_guard<std::mutex> lk(m_);
        return resolveLocked(id, outPath);
    }

    bool waitForTrack(const std::string& id, std::chrono::milliseconds timeout, std::string& outPath) const {
        std::unique_lock<std::mutex> lk(m_);
        if (resolveLocked(id, outPath)) return true;
        cv_.wait_for(lk, timeout, [this, &id, &outPath] {
            return resolveLocked(id, outPath);
        });
        return resolveLocked(id, outPath);
    }

    struct ChunkResult {
        bool ok = false;
        bool completed = false;
        std::string finalPath;
        std::string error;
    };

    ChunkResult writeChunk(const std::string& id, const std::string& ext,
                           uint64_t offset, uint64_t totalSize,
                           const char* data, size_t dataLen, bool eof) {
        std::lock_guard<std::mutex> lk(m_);
        ChunkResult res;
        if (dir_.empty() || !sanitizeId(id) || !sanitizeExt(ext)) {
            res.error = "invalid_id_or_ext";
            return res;
        }
        if (totalSize > kMaxTrackBytes) {
            res.error = "file_too_large";
            return res;
        }
        fs::path partPath = fromUtf8Path(dir_) / (id + "." + ext + ".part");
        fs::path finalPath = fromUtf8Path(dir_) / (id + "." + ext);

        std::fstream file(partPath, std::ios::in | std::ios::out | std::ios::binary);
        if (!file.is_open()) {
            file.clear();
            file.open(partPath, std::ios::out | std::ios::binary);
        }
        if (!file.is_open()) {
            res.error = "cannot_open_part_file";
            return res;
        }
        file.seekp(static_cast<std::streamoff>(offset));
        if (dataLen > 0 && data) {
            file.write(data, static_cast<std::streamsize>(dataLen));
        }
        file.flush();
        file.close();

        std::error_code ec;
        uint64_t curSize = fs::file_size(partPath, ec);
        if (eof || (totalSize > 0 && curSize >= totalSize)) {
            fs::rename(partPath, finalPath, ec);
            if (!ec) {
                res.ok = true;
                res.completed = true;
                res.finalPath = toUtf8Path(finalPath);
                cleanLruLocked();
                cv_.notify_all();
                return res;
            }
            res.error = "cannot_rename_final";
            return res;
        }
        res.ok = true;
        res.completed = false;
        return res;
    }

    void clear() {
        std::lock_guard<std::mutex> lk(m_);
        if (dir_.empty()) return;
        std::error_code ec;
        for (const auto& it : fs::directory_iterator(fromUtf8Path(dir_), ec)) {
            fs::remove(it.path(), ec);
        }
        aliases_.clear();
    }

private:
    bool resolveAliasLocked(const std::string& phoneId, std::string& outPath) const {
        auto it = aliases_.find(phoneId);
        if (it != aliases_.end()) {
            std::error_code ec;
            if (fs::exists(fromUtf8Path(it->second), ec)) {
                outPath = it->second;
                return true;
            }
        }
        return false;
    }

    bool resolveLocked(const std::string& id, std::string& outPath) const {
        if (!sanitizeId(id)) return false;
        if (!dir_.empty()) {
            for (const char* ext : { "flac", "wav", "mp3", "ogg", "m4a", "aac", "opus" }) {
                fs::path p = fromUtf8Path(dir_) / (id + "." + ext);
                std::error_code ec;
                if (fs::exists(p, ec) && fs::is_regular_file(p, ec)) {
                    outPath = toUtf8Path(p);
                    return true;
                }
            }
        }
        return resolveAliasLocked(id, outPath);
    }

    void cleanStalePartsLocked() {
        if (dir_.empty()) return;
        std::error_code ec;
        const auto now = fs::file_time_type::clock::now();
        for (const auto& it : fs::directory_iterator(fromUtf8Path(dir_), ec)) {
            if (it.is_regular_file(ec)) {
                const std::string fn = toUtf8Path(it.path().filename());
                if (fn.size() > 5 && fn.substr(fn.size() - 5) == ".part") {
                    const auto mtime = it.last_write_time(ec);
                    if (!ec && std::chrono::duration_cast<std::chrono::hours>(now - mtime).count() >= 2) {
                        fs::remove(it.path(), ec);
                    }
                }
            }
        }
    }

    void cleanLruLocked() {
        std::error_code ec;
        uint64_t total = 0;
        struct Entry { fs::path path; fs::file_time_type mtime; uint64_t size; };
        std::vector<Entry> files;
        for (const auto& it : fs::directory_iterator(fromUtf8Path(dir_), ec)) {
            if (it.is_regular_file(ec)) {
                uint64_t sz = it.file_size(ec);
                total += sz;
                files.push_back({ it.path(), it.last_write_time(ec), sz });
            }
        }
        if (total > kMaxCacheBytes) {
            std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.mtime < b.mtime; });
            for (const auto& f : files) {
                fs::remove(f.path, ec);
                total -= f.size;
                if (total <= kMaxCacheBytes * 3 / 4) break;
            }
        }
    }

    mutable std::mutex m_;
    mutable std::condition_variable cv_;
    std::unordered_map<std::string, std::string> aliases_;
    std::string dir_;
};

} // namespace ee_analyzer::player
