#pragma once
/**
 * @file mac_updater.hpp
 * @brief Frissítés macOS-en (a felület „Frissítés” gombjára). Letölti a kiadás lemezképét (DMG) a saját GitHub-kiadásból
 *        (rögzített cím: update_logic.hpp), egyezteti a kiadás .sha256 ellenőrzőösszegével, majd megnyitja a Finderben.
 *        Az alkalmazást a felhasználó húzza át az Alkalmazások mappába, mint első telepítéskor: a futó alkalmazást nem
 *        cseréljük ki automatikusan (aláírás és notarizáció nélkül ez nem biztonságos). A letöltött fájl karanténjelzés
 *        nélkül marad; az épségét a SHA-256 egyeztetés adja.
 *
 * Az állapotot (fázis, letöltött bájtok, hiba) a motor az állapotüzenet "update" mezőjében küldi (statusJson); az a híd
 * saját szálán épül. Egyszerre egy frissítés fut. Csak Objective-C++ (.mm) fájlból használható.
 */
#import <Cocoa/Cocoa.h>

#include <atomic>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sha256.hpp"
#include "update_logic.hpp"

namespace ee_analyzer::mac {

class Updater {
public:
    ~Updater() { stop(); }

    /** Frissítés a megadott verzióra; false: érvénytelen verzió (error = "version") vagy már fut ("busy"). */
    bool start(const std::string& version, std::string& error) {
        if (!update::validVersion(version)) {
            error = "version";
            return false;
        }
        std::lock_guard<std::mutex> lk(m_);
        if (busy_) {
            error = "busy";
            return false;
        }
        if (thread_.joinable()) thread_.join();   // az előző, már befejezett futás
        busy_ = true;
        cancel_ = false;
        version_ = version;
        phase_ = "download";
        error_.clear();
        got_ = total_ = 0;
        thread_ = std::thread([this, version] { run(version); });
        return true;
    }

    /** Az alkalmazás kilépésekor: a letöltés megszakad, a szál véget ér. */
    void stop() {
        cancel_ = true;
        if (thread_.joinable()) thread_.join();
    }

    /** Az állapotüzenet "update" mezője (vesszővel kezdve); üres, ha ebben a futásban nem volt frissítés. */
    std::string statusJson() const {
        std::lock_guard<std::mutex> lk(m_);
        if (phase_.empty()) return {};
        return ",\"update\":{\"version\":\"" + version_ + "\",\"phase\":\"" + phase_ + "\",\"got\":" + std::to_string(got_) +
               ",\"total\":" + std::to_string(total_) + ",\"error\":\"" + error_ + "\"}";
    }

private:
    void set(const char* phase, uint64_t got, uint64_t total) {
        std::lock_guard<std::mutex> lk(m_);
        phase_ = phase;
        got_ = got;
        total_ = total;
    }

    static NSString* ns(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

    /** Letöltés NSURLSession-nel a dest fájlba (az átirányításokat követi); false: hiba vagy megszakítás. */
    bool download(const std::string& url, const std::string& dest, bool reportProgress) {
        __block bool ok = false;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        NSURL* src = [NSURL URLWithString:ns(url)];
        NSURL* dst = [NSURL fileURLWithPath:ns(dest)];
        NSURLSessionDownloadTask* task =
            [[NSURLSession sharedSession] downloadTaskWithURL:src
                                            completionHandler:^(NSURL* location, NSURLResponse* resp, NSError* err) {
                const NSInteger code = [resp isKindOfClass:[NSHTTPURLResponse class]] ? ((NSHTTPURLResponse*)resp).statusCode : 0;
                if (!err && location && code == 200) {
                    [[NSFileManager defaultManager] removeItemAtURL:dst error:nil];
                    ok = [[NSFileManager defaultManager] moveItemAtURL:location toURL:dst error:nil];
                }
                dispatch_semaphore_signal(done);
            }];
        [task resume];
        while (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 200 * NSEC_PER_MSEC)) != 0) {
            if (cancel_) {
                [task cancel];   // a befejező blokk ekkor is lefut (hibával)
            } else if (reportProgress) {
                const int64_t total = task.countOfBytesExpectedToReceive;
                set("download", static_cast<uint64_t>(task.countOfBytesReceived), total > 0 ? static_cast<uint64_t>(total) : 0);
            }
        }
        return ok && !cancel_;
    }

    static std::string readSmall(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        std::string s;
        if (f) s.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        return s.size() > 4096 ? std::string() : s;
    }

    static std::string fileSha256(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return {};
        Sha256 h;
        std::vector<char> buf(64 * 1024);
        while (f) {
            f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            if (f.gcount() > 0) h.update(buf.data(), static_cast<size_t>(f.gcount()));
        }
        return h.hexDigest();
    }

    /** A lemezkép megnyitása a fő szálon (a Finder csatolja, és megmutatja); nem várunk rá holtpontot okozó módon. */
    bool openInFinder(const std::string& path) {
        __block int result = -1;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        NSURL* url = [NSURL fileURLWithPath:ns(path)];
        dispatch_async(dispatch_get_main_queue(), ^{
            result = [[NSWorkspace sharedWorkspace] openURL:url] ? 1 : 0;
            dispatch_semaphore_signal(done);
        });
        while (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 200 * NSEC_PER_MSEC)) != 0)
            if (cancel_) return false;   // kilépés közben (a fő szál a stop()-ban vár)
        return result == 1;
    }

    /** A letöltés, az ellenőrzés és a megnyitás; üres: sikerült, különben a hiba kódja. */
    std::string work(const std::string& v, const std::string& dmg, const std::string& sum) {
        if (!download(update::dmgChecksumUrl(v), sum, false)) return cancel_ ? "cancelled" : "download";
        const std::string want = update::parseChecksum(readSmall(sum));
        if (want.empty()) return "checksum";
        if (!download(update::dmgUrl(v), dmg, true)) return cancel_ ? "cancelled" : "download";
        set("verify", 0, 0);
        if (fileSha256(dmg) != want) return "checksum";
        set("open", 0, 0);
        if (!openInFinder(dmg)) return cancel_ ? "cancelled" : "open";
        return {};
    }

    void run(const std::string& v) {
        @autoreleasepool {
            NSString* dir = [NSTemporaryDirectory() stringByAppendingPathComponent:@"Aqulify-update"];
            [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
            const std::string base = std::string(dir.UTF8String) + "/Aqulify-" + v + "-macOS.dmg";
            const std::string sum = base + ".sha256";
            [[NSFileManager defaultManager] removeItemAtPath:ns(base) error:nil];
            [[NSFileManager defaultManager] removeItemAtPath:ns(sum) error:nil];
            std::cout << "[Frissítés] " << v << ": letöltés innen: " << update::dmgUrl(v) << std::endl;
            const std::string err = work(v, base, sum);
            [[NSFileManager defaultManager] removeItemAtPath:ns(sum) error:nil];
            if (err.empty()) {
                std::cout << "[Frissítés] A lemezkép megnyílt; az alkalmazást a felhasználó húzza át az Alkalmazások mappába."
                          << std::endl;
            } else {
                [[NSFileManager defaultManager] removeItemAtPath:ns(base) error:nil];
                std::cout << "[Frissítés] Nem sikerült: " << err << "." << std::endl;
            }
            std::lock_guard<std::mutex> lk(m_);
            phase_ = err.empty() ? "opened" : "error";
            error_ = err;
            busy_ = false;
        }
    }

    mutable std::mutex m_;
    std::thread thread_;
    bool busy_ = false;
    std::atomic<bool> cancel_{false};
    std::string version_, phase_, error_;
    uint64_t got_ = 0, total_ = 0;
};

} // namespace ee_analyzer::mac
