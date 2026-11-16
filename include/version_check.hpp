#pragma once
/**
 * @file version_check.hpp
 * @brief A legújabb kiadás verzióinak ellenőrzése (docs: aqulify-mobile/docs/plans/terv-verzioellenorzes-2026-11-16.md).
 *
 * Minden kiadás feltölt egy kis aqulify-versions.json fájlt ({"desktop":"2.3.0","android":"1.0.1","protocol":2,
 * "min_client_protocol":2}); a motor 6 óránként letölti a legújabb kiadásból (rögzített cím, csak a saját
 * GitHub-kiadásból), és ha a sajátjánál frissebb asztali verzió van, a naplóba ír, és az állapotüzenetben jelzi
 * ("update_available"), így a telefon és a vezérlő gép is látja (a fej nélküli Pi-nél is). Hálózat nélkül az
 * ellenőrzés csendben kimarad. A letöltést a platform adja (Windows: WinHTTP; Linux / Pi és macOS: a rendszer curl
 * programja); a feldolgozás itt van, rendszerfüggetlenül, hogy a tesztek is ellenőrizzék.
 */
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "mini_json.hpp"
#include "update_logic.hpp"

namespace ee_analyzer::vercheck {

/** A válasz legnagyobb elfogadott mérete (bájt); a valódi fájl néhány száz bájt. */
inline constexpr size_t kMaxBytes = 64 * 1024;

/** A legújabb kiadás verziófájljának címe (rögzített: csak a saját tároló kiadásaiból). */
inline std::string versionsUrl() {
    return std::string("https://github.com/") + update::kRepo + "/releases/latest/download/aqulify-versions.json";
}

struct Latest {
    std::string desktop;    // a legújabb asztali (és Pi-) kiadás, pl. 2.3.0
    std::string android;    // a legújabb telefonos app, pl. 1.0.1 (üres: a fájlban nem szerepel)
    int protocol = 0;
    int minClientProtocol = 0;
    bool ok = false;
};

/** Az aqulify-versions.json értelmezése; ok = false, ha nem érvényes (túl nagy, nem JSON, hibás verziószám). */
inline Latest parseVersions(const std::string& text) {
    Latest l;
    if (text.empty() || text.size() > kMaxBytes) return l;
    json::Value v;
    if (!json::parse(text, v) || v.type != json::Value::Type::Object) return l;
    const json::Value* d = v.get("desktop");
    l.desktop = d ? d->stringOr("") : "";
    if (!update::validVersion(l.desktop)) return Latest{};
    if (const json::Value* a = v.get("android")) {
        const std::string s = a->stringOr("");
        if (update::validVersion(s)) l.android = s;
    }
    auto num = [&](const char* k) {
        const json::Value* x = v.get(k);
        const double n = x ? x->numberOr(0.0) : 0.0;
        return (n >= 0.0 && n <= 1000.0) ? static_cast<int>(n) : 0;
    };
    l.protocol = num("protocol");
    l.minClientProtocol = num("min_client_protocol");
    l.ok = true;
    return l;
}

/** Igaz, ha a latest érvényes és frissebb a current-nél (mindkettő X.Y.Z). */
inline bool isNewer(const std::string& latest, const std::string& current) {
    return update::validVersion(latest) && update::validVersion(current) && update::compareVersions(latest, current) > 0;
}

/** A kliens által küldött verziószöveg: X.Y.Z, különben üres (a hibás vagy túl hosszú értéket nem tároljuk). */
inline std::string cleanClientVersion(const std::string& raw) { return update::validVersion(raw) ? raw : std::string(); }

/** A kliens platformja: "android" vagy "desktop", különben üres. */
inline std::string cleanPlatform(const std::string& raw) {
    return (raw == "android" || raw == "desktop") ? raw : std::string();
}

/**
 * Letöltés a rendszer curl programjával (Linux / Pi, macOS). A cím rögzített (versionsUrl), nincs benne idézőjel,
 * így a parancssorba nem kerülhet idegen szöveg. Csak https, legfeljebb kMaxBytes, 15 s. Üres: nincs curl, nincs
 * hálózat, vagy hiba.
 */
inline std::string fetchWithCurl(const std::string& url) {
#ifdef _WIN32
    (void)url;
    return {};
#else
    if (url.find('\'') != std::string::npos || url.rfind("https://", 0) != 0) return {};
    const std::string cmd = "curl -fsSL --proto =https --max-time 15 --max-filesize " + std::to_string(kMaxBytes) +
                            " '" + url + "' 2>/dev/null";
    FILE* p = ::popen(cmd.c_str(), "r");
    if (!p) return {};
    std::string out;
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) {
        out.append(buf, n);
        if (out.size() > kMaxBytes) break;
    }
    const int rc = ::pclose(p);
    if (rc != 0 || out.size() > kMaxBytes) return {};
    return out;
#endif
}

/**
 * Időszakos ellenőrzés háttérszálon. statusJson(): az állapotüzenet része (vesszővel kezdve), üres, ha még nincs
 * érvényes adat.
 */
class Checker {
public:
    using Fetch = std::function<std::string(const std::string& url)>;

    ~Checker() { stop(); }

    void start(Fetch fetch, std::string current, std::chrono::seconds period = std::chrono::hours(6),
               std::chrono::seconds firstDelay = std::chrono::seconds(30)) {
        stop();
        quit_ = false;
        current_ = std::move(current);
        thread_ = std::thread([this, fetch = std::move(fetch), period, firstDelay] {
            auto wait = firstDelay;
            for (;;) {
                {
                    std::unique_lock<std::mutex> lk(cvM_);
                    if (cv_.wait_for(lk, wait, [this] { return quit_; })) return;
                }
                const Latest l = parseVersions(fetch ? fetch(versionsUrl()) : std::string());
                if (l.ok) apply(l);
                wait = period;
            }
        });
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lk(cvM_);
            quit_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    /** Új adat feldolgozása (a szálról; a tesztek közvetlenül is hívják). */
    void apply(const Latest& l) {
        std::lock_guard<std::mutex> lk(m_);
        latest_ = l;
        if (isNewer(l.desktop, current_) && logged_ != l.desktop) {
            logged_ = l.desktop;
            std::cout << "[Verzió] Elérhető egy frissebb Aqulify: " << l.desktop << " (ez a gép: " << current_ << ").\n";
        }
    }

    void setCurrentForTest(const std::string& v) {
        std::lock_guard<std::mutex> lk(m_);
        current_ = v;
    }

    Latest latest() const {
        std::lock_guard<std::mutex> lk(m_);
        return latest_;
    }

    /** ",\"latest\":{…}" és ha van frissebb: ",\"update_available\":{\"ver\":…}"; üres, ha még nincs adat. */
    std::string statusJson() const {
        std::lock_guard<std::mutex> lk(m_);
        if (!latest_.ok) return {};
        std::string s = ",\"latest\":{\"desktop\":\"" + latest_.desktop + "\"";
        if (!latest_.android.empty()) s += ",\"android\":\"" + latest_.android + "\"";
        s += ",\"protocol\":" + std::to_string(latest_.protocol) +
             ",\"min_client_protocol\":" + std::to_string(latest_.minClientProtocol) + "}";
        if (isNewer(latest_.desktop, current_)) s += ",\"update_available\":{\"ver\":\"" + latest_.desktop + "\"}";
        return s;
    }

private:
    mutable std::mutex m_;
    Latest latest_;
    std::string current_, logged_;
    std::mutex cvM_;
    std::condition_variable cv_;
    bool quit_ = false;
    std::thread thread_;
};

} // namespace ee_analyzer::vercheck
