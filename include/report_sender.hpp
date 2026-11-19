#pragma once
/**
 * @file report_sender.hpp
 * @brief Tisztított hibajelentések küldése a Cloudflare Worker POST /report végpontjára.
 *
 * - Cím: fordítási állandó AQ_REPORT_URL (alapérték: üres → nincs küldés). Token és kulcs nincs.
 * - Szállítás: Linux / Pi és macOS: rendszer curl (törzs ideiglenes fájlból, --proto =https, 15 s);
 *   Windows: WinHTTP (win::postJson, https, 15 s).
 * - Hozzájárulás: csak akkor küld, ha (a) az errorReports beállítás be van kapcsolva (AutoEnabled), vagy
 *   (b) a felhasználó kézzel jóváhagyta (ManualOneTime). Különben egyáltalán nem kapcsolódik.
 * - Méretkorlát: legfeljebb 65 536 bájt; ha nagyobb, a log legrégebbi sorait elhagyja, amíg belefér.
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "error_report.hpp"
#include "mini_json.hpp"

#ifdef _WIN32
#include "../windows/common/win_download.hpp"
#else
#include <unistd.h>
#endif

#ifndef AQ_REPORT_URL
#define AQ_REPORT_URL "https://aqulify-hibajelento.gergoeagle.workers.dev/report"
#endif

namespace aq::report {

inline constexpr size_t kMaxBodyBytes = 65536;
inline constexpr int kTimeoutSec = 15;
inline constexpr int kMaxAutoAttempts = 3;

enum class Consent {
    None = 0,          // nincs hozzájárulás → tilos küldeni
    AutoEnabled = 1,   // a felhasználó bekapcsolta a névtelen hibajelentést
    ManualOneTime = 2  // egyszeri kézi küldés a „Hibát jelentek” ablakból
};

struct HttpResponse {
    int status = 0;            // HTTP státuszkód (0: hálózati hiba / időtúllépés)
    std::string body;          // válasz törzse
    int retryAfterSec = 0;     // Retry-After fejléc (másodperc), ha volt
    bool networkError = false; // kapcsolódási / szállítási hiba
};

using TransportFn = std::function<HttpResponse(const std::string& url, const std::string& jsonBody)>;

struct SendResult {
    bool ok = false;
    long long id = 0;
    int httpStatus = 0;
    int retryAfterSec = 0;
    bool retryable = false;
    std::string error;
};

namespace sender_detail {

inline bool isSafeUrlChar(unsigned char c) {
    return c > 0x20 && c < 0x7f && c != '\'' && c != '"' && c != '\\' && c != '`' && c != '$';
}

inline bool isAllowedUrl(const std::string& url, bool allowLocalHttp) {
    if (url.empty() || url.size() > 2048) return false;
    for (unsigned char c : url) {
        if (!isSafeUrlChar(c)) return false;
    }
    if (url.rfind("https://", 0) == 0) return true;
    if (allowLocalHttp &&
        (url.rfind("http://127.0.0.1:", 0) == 0 || url.rfind("http://127.0.0.1/", 0) == 0 ||
         url.rfind("http://localhost:", 0) == 0 || url.rfind("http://localhost/", 0) == 0)) {
        return true;
    }
    return false;
}

/** JSON string átugrása a záró idézőjel utáni pozícióra (i a nyitó '"' helye). */
inline size_t skipJsonString(const std::string& s, size_t i) {
    if (i >= s.size() || s[i] != '"') return i;
    ++i;
    while (i < s.size()) {
        if (s[i] == '\\') { i += 2; continue; }
        if (s[i] == '"') return i + 1;
        ++i;
    }
    return s.size();
}

/**
 * Megkeresi a felső szintű "log":[...] tömböt a format 1 JSON-ban.
 * Siker esetén outArrStart a '[' indexe, outArrEnd a ']' utáni index.
 */
inline bool findTopLevelLogArray(const std::string& json, size_t& outArrStart, size_t& outArrEnd) {
    int depth = 0;
    size_t i = 0;
    while (i < json.size()) {
        const char c = json[i];
        if (c == '"') {
            const size_t next = skipJsonString(json, i);
            if (depth == 1 && next - i == 5 && json.compare(i, 5, "\"log\"") == 0) {
                size_t j = next;
                while (j < json.size() && (json[j] == ' ' || json[j] == '\t' || json[j] == '\n' || json[j] == '\r')) ++j;
                if (j < json.size() && json[j] == ':') {
                    ++j;
                    while (j < json.size() && (json[j] == ' ' || json[j] == '\t' || json[j] == '\n' || json[j] == '\r')) ++j;
                    if (j < json.size() && json[j] == '[') {
                        const size_t arrStart = j;
                        int arrDepth = 0;
                        while (j < json.size()) {
                            if (json[j] == '"') { j = skipJsonString(json, j); continue; }
                            if (json[j] == '[') ++arrDepth;
                            else if (json[j] == ']') {
                                --arrDepth;
                                if (arrDepth == 0) {
                                    outArrStart = arrStart;
                                    outArrEnd = j + 1;
                                    return true;
                                }
                            }
                            ++j;
                        }
                        return false;
                    }
                }
            }
            i = next;
            continue;
        }
        if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') --depth;
        ++i;
    }
    return false;
}

/** Egy JSON-tömb (nyitó '[' és záró ']' között) string elemeinek kinyerése nyers JSON-alakban. */
inline std::vector<std::string> splitRawJsonArray(const std::string& json, size_t arrStart, size_t arrEnd) {
    std::vector<std::string> items;
    if (arrEnd <= arrStart + 1) return items;
    size_t i = arrStart + 1;
    const size_t end = arrEnd - 1;
    while (i < end) {
        while (i < end && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r' || json[i] == ',')) ++i;
        if (i >= end) break;
        if (json[i] == '"') {
            const size_t next = skipJsonString(json, i);
            items.push_back(json.substr(i, next - i));
            i = next;
        } else {
            // nem várt elem: ugorjuk a következő vesszőig
            size_t j = i;
            while (j < end && json[j] != ',') ++j;
            items.push_back(json.substr(i, j - i));
            i = j;
        }
    }
    return items;
}

#ifndef _WIN32
struct TempFileGuard {
    std::string path;
    ~TempFileGuard() {
        if (!path.empty()) ::unlink(path.c_str());
    }
};

inline int parseRetryAfterFromHeaders(const std::string& headers) {
    size_t pos = 0;
    while (pos < headers.size()) {
        size_t nl = headers.find('\n', pos);
        if (nl == std::string::npos) nl = headers.size();
        std::string line = headers.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Kis- és nagybetű független "retry-after:" keresés
        if (line.size() > 12) {
            std::string low = line.substr(0, 12);
            for (char& ch : low) ch = report::detail::lowerAscii(ch);
            if (low == "retry-after:") {
                std::string val = line.substr(12);
                size_t s = 0;
                while (s < val.size() && (val[s] == ' ' || val[s] == '\t')) ++s;
                const long sec = std::strtol(val.c_str() + s, nullptr, 10);
                if (sec > 0 && sec < 86400) return static_cast<int>(sec);
            }
        }
        pos = nl + 1;
    }
    return 0;
}
#endif

} // namespace sender_detail

/**
 * A JSON-törzs méretének korlátozása maxBytes bájtra a "log" tömb legrégebbi sorainak elhagyásával.
 * Ha már üres naplóval sem fér bele, üres stringet ad vissza.
 */
inline std::string trimLogToMaxBytes(const std::string& json, size_t maxBytes = kMaxBodyBytes) {
    if (json.size() <= maxBytes) return json;
    size_t arrStart = 0, arrEnd = 0;
    if (!sender_detail::findTopLevelLogArray(json, arrStart, arrEnd)) return {};
    const std::string prefix = json.substr(0, arrStart + 1); // tartalmazza a '[' karaktert
    const std::string suffix = json.substr(arrEnd - 1);      // tartalmazza a ']' karaktert
    if (prefix.size() + suffix.size() > maxBytes) return {};

    const std::vector<std::string> items = sender_detail::splitRawJsonArray(json, arrStart, arrEnd);
    const size_t avail = maxBytes - prefix.size() - suffix.size();
    // Hátulról (a legfrissebb soroktól) gyűjtjük, amíg belefér
    size_t used = 0;
    size_t keepFrom = items.size();
    for (size_t idx = items.size(); idx-- > 0;) {
        const size_t add = items[idx].size() + (keepFrom < items.size() ? 1 : 0);
        if (used + add > avail) break;
        used += add;
        keepFrom = idx;
    }
    std::string out;
    out.reserve(prefix.size() + used + suffix.size());
    out += prefix;
    for (size_t i = keepFrom; i < items.size(); ++i) {
        if (i > keepFrom) out += ',';
        out += items[i];
    }
    out += suffix;
    return out;
}

/**
 * Platformfüggő HTTP POST JSON küldés (Linux/Pi, macOS: rendszer curl ideiglenes fájllal; Windows: WinHTTP).
 * allowLocalHttp = true csak teszteléshez enged helyi (127.0.0.1 / localhost) HTTP-t.
 */
inline HttpResponse postJsonNative(const std::string& url, const std::string& jsonBody, bool allowLocalHttp = false) {
    HttpResponse res;
    if (!sender_detail::isAllowedUrl(url, allowLocalHttp)) {
        res.networkError = true;
        return res;
    }
#ifdef _WIN32
    const std::wstring wurl(url.begin(), url.end());
    const ee_analyzer::win::PostJsonResult wr =
        ee_analyzer::win::postJson(wurl, jsonBody, kTimeoutSec * 1000, allowLocalHttp);
    res.status = wr.status;
    res.body = wr.body;
    res.retryAfterSec = wr.retryAfterSec;
    res.networkError = wr.networkError;
    return res;
#else
    sender_detail::TempFileGuard bodyGuard, hdrGuard;
    char bodyTmpl[] = "/tmp/aqulify-rep-XXXXXX";
    const int bfd = ::mkstemp(bodyTmpl);
    if (bfd < 0) { res.networkError = true; return res; }
    bodyGuard.path = bodyTmpl;
    size_t written = 0;
    while (written < jsonBody.size()) {
        const ssize_t w = ::write(bfd, jsonBody.data() + written, jsonBody.size() - written);
        if (w <= 0) { ::close(bfd); res.networkError = true; return res; }
        written += static_cast<size_t>(w);
    }
    ::close(bfd);

    char hdrTmpl[] = "/tmp/aqulify-hdr-XXXXXX";
    const int hfd = ::mkstemp(hdrTmpl);
    if (hfd < 0) { res.networkError = true; return res; }
    ::close(hfd);
    hdrGuard.path = hdrTmpl;

    const std::string proto = allowLocalHttp ? "=http,https" : "=https";
    const std::string cmd = "curl -sS --proto " + proto + " --max-time " + std::to_string(kTimeoutSec) +
                            " -X POST -H 'Content-Type: application/json; charset=utf-8' --data-binary '@" +
                            bodyGuard.path + "' -D '" + hdrGuard.path + "' -w '\\n%{http_code}' '" + url +
                            "' 2>/dev/null";
    FILE* p = ::popen(cmd.c_str(), "r");
    if (!p) { res.networkError = true; return res; }
    std::string out;
    char buf[4096];
    constexpr size_t kMaxResp = 20 * 1024;
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) {
        if (out.size() < kMaxResp) {
            const size_t take = std::min(n, kMaxResp - out.size());
            out.append(buf, take);
        }
    }
    const int rc = ::pclose(p);
    // Az utolsó sor a HTTP státuszkód
    const size_t nl = out.rfind('\n');
    if (nl != std::string::npos) {
        const std::string codeStr = out.substr(nl + 1);
        res.status = std::atoi(codeStr.c_str());
        res.body = out.substr(0, nl);
    } else if (!out.empty()) {
        res.status = std::atoi(out.c_str());
    }
    if (res.status <= 0 || (rc != 0 && res.status == 0)) {
        res.networkError = true;
        res.status = 0;
    }
    const std::string hdrs = report::detail::readAll(hdrGuard.path, 16 * 1024);
    res.retryAfterSec = sender_detail::parseRetryAfterFromHeaders(hdrs);
    return res;
#endif
}

/**
 * Hibajelentés-küldő logika: hozzájárulás-ellenőrzés, méretkorlát, válaszkezelés (201, 413, 429, 400/415, 5xx).
 */
class Sender {
public:
    explicit Sender(std::string url = AQ_REPORT_URL, TransportFn transport = nullptr)
        : url_(std::move(url)),
          transport_(transport ? std::move(transport)
                               : [](const std::string& u, const std::string& b) { return postJsonNative(u, b, false); }) {}

    bool configured() const { return !url_.empty(); }
    const std::string& url() const { return url_; }

    /**
     * Egy jelentés elküldése. Hozzájárulás (Consent::AutoEnabled vagy ManualOneTime) és beállított URL nélkül
     * nem végez hálózati hívást.
     */
    SendResult send(const std::string& json, Consent consent) const {
        SendResult res;
        if (consent == Consent::None) {
            res.error = "disabled";
            return res;
        }
        if (url_.empty()) {
            res.error = "not_configured";
            return res;
        }
        std::string payload = trimLogToMaxBytes(json, kMaxBodyBytes);
        if (payload.empty()) {
            res.error = "too_large";
            return res;
        }

        HttpResponse http = transport_(url_, payload);
        // 413 esetén a napló további rövidítése és egyetlen újrapróbálás
        if (!http.networkError && http.status == 413) {
            size_t limit = payload.size() > 2048 ? (payload.size() / 2) : 0;
            ee_analyzer::json::Value v;
            if (!http.body.empty() && ee_analyzer::json::parse(http.body, v)) {
                if (const auto* mb = v.get("max_bytes"); mb && mb->type == ee_analyzer::json::Value::Type::Number) {
                    const size_t srvMax = static_cast<size_t>(mb->num);
                    if (srvMax > 0 && srvMax < payload.size()) limit = srvMax;
                }
            }
            if (limit > 0) {
                const std::string smaller = trimLogToMaxBytes(payload, limit);
                if (!smaller.empty() && smaller.size() < payload.size()) {
                    payload = smaller;
                    http = transport_(url_, payload);
                }
            }
        }

        res.httpStatus = http.status;
        if (http.networkError || http.status == 0) {
            res.retryable = true;
            res.error = "network";
            return res;
        }

        ee_analyzer::json::Value v;
        const bool parsed = !http.body.empty() && ee_analyzer::json::parse(http.body, v);
        std::string srvErr;
        if (parsed) {
            if (const auto* e = v.get("error"); e && e->type == ee_analyzer::json::Value::Type::String) srvErr = e->str;
        }

        if (http.status == 201) {
            res.ok = true;
            if (parsed) {
                if (const auto* id = v.get("id"); id && id->type == ee_analyzer::json::Value::Type::Number)
                    res.id = static_cast<long long>(id->num);
            }
            return res;
        }
        if (http.status == 429) {
            res.retryable = true;
            res.retryAfterSec = http.retryAfterSec > 0 ? http.retryAfterSec : 60;
            res.error = srvErr.empty() ? "rate_limited" : srvErr;
            return res;
        }
        if (http.status == 413) {
            res.retryable = false;
            res.error = "too_large";
            return res;
        }
        if (http.status == 400 || http.status == 415 || http.status == 404 || http.status == 405) {
            res.retryable = false;
            res.error = srvErr.empty() ? ("http_" + std::to_string(http.status)) : srvErr;
            return res;
        }
        if (http.status >= 500) {
            res.retryable = true;
            res.error = srvErr.empty() ? ("http_" + std::to_string(http.status)) : srvErr;
            return res;
        }
        res.retryable = false;
        res.error = srvErr.empty() ? ("http_" + std::to_string(http.status)) : srvErr;
        return res;
    }

private:
    std::string url_;
    TransportFn transport_;
};

/**
 * Háttérben futó hibajelentő szolgáltatás a három motorhoz (Linux, Windows, macOS).
 * A küldés külön szálon fut, a leállítás (stop) soha nem vár hálózati időkorlátra.
 */
class Service {
public:
    Service() = default;
    ~Service() { stop(); }
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    void start(const std::filesystem::path& diagDir, const std::filesystem::path& logPath,
               std::vector<std::string> scrubNames, bool enabled, std::string url = AQ_REPORT_URL,
               TransportFn transport = nullptr) {
        stop();
        auto st = std::make_shared<State>(std::move(url), std::move(transport));
        st->diagDir = diagDir;
        st->logPath = logPath;
        st->scrubNames = std::move(scrubNames);
        st->enabled.store(enabled);
        {
            std::lock_guard<std::mutex> lk(mu_);
            state_ = st;
        }
        if (enabled) triggerAutoSendPrevious();
    }

    void setEnabled(bool on) {
        std::shared_ptr<State> st = getState();
        if (!st) return;
        st->enabled.store(on);
        if (on) triggerAutoSendPrevious();
    }

    bool configured() const {
        std::shared_ptr<State> st = getState();
        return st && st->sender.configured();
    }

    bool hasPreviousReport() const {
        std::shared_ptr<State> st = getState();
        if (!st || st->diagDir.empty()) return false;
        std::error_code ec;
        return std::filesystem::exists(st->diagDir / "previous-run.json", ec);
    }

    /** Az állapotüzenet része (vesszővel kezdve): ,"error_report":{"configured":…,"has_previous":…}. */
    std::string statusJson() const {
        std::shared_ptr<State> st = getState();
        if (!st) return {};
        return std::string(",\"error_report\":{\"configured\":") + (st->sender.configured() ? "true" : "false") +
               ",\"has_previous\":" + (hasPreviousReport() ? "true" : "false") + "}";
    }

    /** Előnézet készítése a felülethez (kind = "manual" vagy "previous"). */
    void buildPreview(const std::string& kind, const std::string& symptom, const std::string& note,
                      std::string& outPreview, size_t& outBytes, bool& outConfigured) const {
        outPreview.clear();
        outBytes = 0;
        outConfigured = false;
        std::shared_ptr<State> st = getState();
        if (!st) return;
        outConfigured = st->sender.configured();
        if (kind == "previous") {
            const std::string j = report::detail::readAll(st->diagDir / "previous-run.json", 512 * 1024);
            const std::string p = report::detail::readAll(st->diagDir / "previous-run.txt", 512 * 1024);
            const std::string trimmed = trimLogToMaxBytes(j, kMaxBodyBytes);
            outBytes = trimmed.empty() ? j.size() : trimmed.size();
            outPreview = !p.empty() ? p : j;
            return;
        }
        const ScrubContext ctx = makeCtx(*st);
        const Report r = build(st->diagDir, st->logPath, ctx, "manual", symptom, 200, false, note);
        const std::string trimmed = trimLogToMaxBytes(r.json, kMaxBodyBytes);
        outBytes = trimmed.empty() ? r.json.size() : trimmed.size();
        outPreview = r.preview;
    }

    /** Mentés fájlba a diag/ mappába (új név, soha nem ír felül; outFilename csak a fájlnév). */
    bool saveReport(const std::string& kind, const std::string& symptom, const std::string& note,
                    std::string& outFilename, std::string& outError) const {
        outFilename.clear();
        outError.clear();
        std::shared_ptr<State> st = getState();
        if (!st) { outError = "not_ready"; return false; }
        std::string json;
        if (kind == "previous") {
            json = report::detail::readAll(st->diagDir / "previous-run.json", 512 * 1024);
            if (json.empty()) { outError = "no_previous"; return false; }
        } else {
            const ScrubContext ctx = makeCtx(*st);
            json = build(st->diagDir, st->logPath, ctx, "manual", symptom, 200, false, note).json;
        }
        if (!saveReportUnique(st->diagDir, json, outFilename)) {
            outError = "write_failed";
            return false;
        }
        return true;
    }

    /** Kézi küldés külön szálon (egyszeri hozzájárulás). Az onDone a küldő szálról hívódik (ha a szolgáltatás még él). */
    void sendAsync(const std::string& kind, const std::string& symptom, const std::string& note,
                   std::function<void(const SendResult&)> onDone) {
        std::shared_ptr<State> st = getState();
        if (!st) {
            if (onDone) { SendResult r; r.error = "not_ready"; onDone(r); }
            return;
        }
        if (!st->sender.configured()) {
            if (onDone) { SendResult r; r.error = "not_configured"; onDone(r); }
            return;
        }
        if (st->manualBusy.exchange(true)) {
            if (onDone) { SendResult r; r.error = "busy"; onDone(r); }
            return;
        }
        std::thread([st, kind, symptom, note, onDone = std::move(onDone)] {
            struct BusyGuard {
                std::atomic<bool>& b;
                ~BusyGuard() { b.store(false); }
            } guard{ st->manualBusy };

            std::string json;
            if (kind == "previous") {
                json = report::detail::readAll(st->diagDir / "previous-run.json", 512 * 1024);
                if (json.empty()) {
                    SendResult r;
                    r.error = "no_previous";
                    if (!st->stopRequested.load() && onDone) onDone(r);
                    return;
                }
            } else {
                const ScrubContext ctx = makeCtx(*st);
                json = build(st->diagDir, st->logPath, ctx, "manual", symptom, 200, false, note).json;
            }
            const SendResult res = st->sender.send(json, Consent::ManualOneTime);
            if (res.ok && kind == "previous") removePreviousFiles(*st);
            if (res.ok) {
                std::cout << "[Hibajelentés] Kézi jelentés elküldve (AQ-" << res.id << ").\n";
            } else {
                std::cout << "[Hibajelentés] Kézi küldés sikertelen: " << res.error << ".\n";
            }
            if (!st->stopRequested.load() && onDone) onDone(res);
        }).detach();
    }

    /** Előző rendellenes futás jelentésének automatikus küldése (ha engedélyezve van és létezik). */
    void triggerAutoSendPrevious() {
        std::shared_ptr<State> st = getState();
        if (!st || !st->enabled.load() || !st->sender.configured()) return;
        std::error_code ec;
        if (!std::filesystem::exists(st->diagDir / "previous-run.json", ec)) return;
        if (st->autoBusy.exchange(true)) return;

        std::thread([st] {
            struct BusyGuard {
                std::atomic<bool>& b;
                ~BusyGuard() { b.store(false); }
            } guard{ st->autoBusy };

            for (int attempt = 0; attempt < kMaxAutoAttempts; ++attempt) {
                if (st->stopRequested.load() || !st->enabled.load()) return;
                const std::string json = report::detail::readAll(st->diagDir / "previous-run.json", 512 * 1024);
                if (json.empty()) return;
                const SendResult res = st->sender.send(json, Consent::AutoEnabled);
                if (res.ok) {
                    removePreviousFiles(*st);
                    std::cout << "[Hibajelentés] Előző rendellenes futás jelentése elküldve (AQ-" << res.id << ").\n";
                    return;
                }
                if (!res.retryable) {
                    std::cout << "[Hibajelentés] Automatikus küldés leállítva: " << res.error << ".\n";
                    return;
                }
                if (attempt + 1 >= kMaxAutoAttempts) return;
                const int waitSec = res.retryAfterSec > 0 ? std::min(res.retryAfterSec, 300) : (10 * (attempt + 1));
                std::unique_lock<std::mutex> lk(st->cvMu);
                st->cv.wait_for(lk, std::chrono::seconds(waitSec), [&] { return st->stopRequested.load(); });
            }
        }).detach();
    }

    /** A felhasználó elutasította az előző futás jelentésének küldését → helyi fájlok törlése. */
    void dismissPrevious() {
        std::shared_ptr<State> st = getState();
        if (st) removePreviousFiles(*st);
    }

    /** Nem blokkoló leállítás: a várakozó szálak azonnal kilépnek, futó hálózati hívásra nem vár. */
    void stop() {
        std::shared_ptr<State> st;
        {
            std::lock_guard<std::mutex> lk(mu_);
            st = std::move(state_);
        }
        if (!st) return;
        {
            std::lock_guard<std::mutex> lk(st->cvMu);
            st->stopRequested.store(true);
        }
        st->cv.notify_all();
    }

private:
    struct State {
        State(std::string url, TransportFn tr) : sender(std::move(url), std::move(tr)) {}
        Sender sender;
        std::filesystem::path diagDir;
        std::filesystem::path logPath;
        std::vector<std::string> scrubNames;
        std::atomic<bool> enabled{ false };
        std::atomic<bool> stopRequested{ false };
        std::atomic<bool> autoBusy{ false };
        std::atomic<bool> manualBusy{ false };
        std::mutex cvMu;
        std::condition_variable cv;
    };

    std::shared_ptr<State> getState() const {
        std::lock_guard<std::mutex> lk(mu_);
        return state_;
    }

    static ScrubContext makeCtx(const State& st) {
        ScrubContext ctx = systemScrubContext();
        ctx.names.insert(ctx.names.end(), st.scrubNames.begin(), st.scrubNames.end());
        return ctx;
    }

    static void removePreviousFiles(const State& st) {
        std::error_code ec;
        std::filesystem::remove(st.diagDir / "previous-run.json", ec);
        std::filesystem::remove(st.diagDir / "previous-run.txt", ec);
    }

    mutable std::mutex mu_;
    std::shared_ptr<State> state_;
};

} // namespace aq::report
