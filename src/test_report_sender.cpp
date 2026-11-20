/**
 * @file test_report_sender.cpp
 * @brief Valódi viselkedést ellenőrző tesztek a hibajelentés-küldőhöz (report_sender.hpp, error_report.hpp).
 *
 * Helyi TCP visszacsatolási HTTP-szerverrel (127.0.0.1) teszteli a valódi szállítást (Linux/macOS: curl,
 * Windows: WinHTTP): 201, 429 + Retry-After, 413 -> rövidítés + újra, 400/415, 500, hálózati hiba;
 * kikapcsolt állapotban 0 kérés; 65 536 bájtos korlát; a szabad szöveg (note) tisztítása; egyedi fájlnév
 * (nincs felülírás); automatikus küldés és previous-run.json törlése 201 esetén.
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "net_compat.hpp"
#include "report_sender.hpp"

namespace fs = std::filesystem;
using namespace ee_analyzer::net;

static int g_failures = 0;

#define CHECK(cond, msg)                                                                        \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " – " << (msg) << "\n";    \
            ++g_failures;                                                                       \
        }                                                                                       \
    } while (0)

struct CapturedRequest {
    std::string rawHeaders;
    std::string body;
};

struct MockResponse {
    int status = 201;
    std::string extraHeaders;
    std::string body = "{\"ok\":true,\"id\":42}";
};

/** Kicsi, platformfüggetlen helyi HTTP-szerver a valódi curl / WinHTTP kérések fogadásához. */
class LocalHttpServer {
public:
    using Handler = std::function<MockResponse(const CapturedRequest&)>;

    explicit LocalHttpServer(Handler h) : handler_(std::move(h)) {}
    ~LocalHttpServer() { stop(); }

    bool start() {
        listenSock_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listenSock_ == kInvalidSocket) return false;
        int one = 1;
        ::setsockopt(listenSock_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0; // a rendszer választ szabad portot
        if (::bind(listenSock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            closeSocket(listenSock_);
            listenSock_ = kInvalidSocket;
            return false;
        }
        socklen_t len = sizeof(addr);
        if (::getsockname(listenSock_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            closeSocket(listenSock_);
            listenSock_ = kInvalidSocket;
            return false;
        }
        port_ = ntohs(addr.sin_port);
        if (::listen(listenSock_, 8) != 0) {
            closeSocket(listenSock_);
            listenSock_ = kInvalidSocket;
            return false;
        }
        setNonBlocking(listenSock_, true);
        running_ = true;
        thread_ = std::thread(&LocalHttpServer::loop, this);
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (thread_.joinable()) thread_.join();
        if (listenSock_ != kInvalidSocket) {
            closeSocket(listenSock_);
            listenSock_ = kInvalidSocket;
        }
    }

    int port() const { return port_; }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/report"; }

    int requestCount() const {
        std::lock_guard<std::mutex> lk(mu_);
        return static_cast<int>(requests_.size());
    }

    std::vector<CapturedRequest> requests() const {
        std::lock_guard<std::mutex> lk(mu_);
        return requests_;
    }

private:
    void loop() {
        while (running_) {
            sockaddr_in cli{};
            socklen_t clen = sizeof(cli);
            socket_t s = ::accept(listenSock_, reinterpret_cast<sockaddr*>(&cli), &clen);
            if (s == kInvalidSocket) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            setNonBlocking(s, false);
            handleClient(s);
            closeSocket(s);
        }
    }

    static size_t parseContentLength(const std::string& hdrs) {
        size_t pos = 0;
        while (pos < hdrs.size()) {
            size_t nl = hdrs.find('\n', pos);
            if (nl == std::string::npos) nl = hdrs.size();
            std::string line = hdrs.substr(pos, nl - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.size() > 15) {
                std::string low = line.substr(0, 15);
                for (char& c : low) c = aq::report::detail::lowerAscii(c);
                if (low == "content-length:") return static_cast<size_t>(std::strtoull(line.c_str() + 15, nullptr, 10));
            }
            pos = nl + 1;
        }
        return 0;
    }

    void handleClient(socket_t s) {
        std::string buf;
        char chunk[4096];
        size_t hdrEnd = std::string::npos;
        size_t contentLen = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto n = ::recv(s, chunk, sizeof(chunk), 0);
            if (n <= 0) break;
            buf.append(chunk, static_cast<size_t>(n));
            if (hdrEnd == std::string::npos) {
                const size_t p = buf.find("\r\n\r\n");
                if (p != std::string::npos) {
                    hdrEnd = p + 4;
                    contentLen = parseContentLength(buf.substr(0, p));
                }
            }
            if (hdrEnd != std::string::npos && buf.size() >= hdrEnd + contentLen) break;
        }
        if (hdrEnd == std::string::npos) return;
        CapturedRequest req;
        req.rawHeaders = buf.substr(0, hdrEnd);
        req.body = buf.substr(hdrEnd, contentLen);
        {
            std::lock_guard<std::mutex> lk(mu_);
            requests_.push_back(req);
        }
        const MockResponse resp = handler_(req);
        std::string out = "HTTP/1.1 " + std::to_string(resp.status) + " OK\r\n"
                          "Content-Type: application/json\r\n"
                          "Content-Length: " + std::to_string(resp.body.size()) + "\r\n"
                          "Connection: close\r\n" + resp.extraHeaders + "\r\n" + resp.body;
        size_t off = 0;
        while (off < out.size()) {
            const auto w = ::send(s, out.data() + off, static_cast<int>(out.size() - off), 0);
            if (w <= 0) break;
            off += static_cast<size_t>(w);
        }
    }

    Handler handler_;
    socket_t listenSock_ = kInvalidSocket;
    int port_ = 0;
    std::atomic<bool> running_{ false };
    std::thread thread_;
    mutable std::mutex mu_;
    std::vector<CapturedRequest> requests_;
};

static fs::path makeTempDir(const std::string& tag) {
    const auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path d = fs::temp_directory_path() / ("aq-test-rep-" + tag + "-" + std::to_string(ts));
    fs::create_directories(d);
    return d;
}

static std::string sampleReportJson(int logLines = 5, int lineLen = 40) {
    std::string j = "{\"format\":1,\"kind\":\"manual\",\"symptom\":\"noSound\",\"created_unix\":1795100000,"
                    "\"state\":{\"version\":\"2.6.2\",\"platform\":\"linux\"},\"crash\":\"\",\"log\":[";
    for (int i = 0; i < logLines; ++i) {
        if (i) j += ',';
        std::string line = "sor_" + std::to_string(i) + "_";
        if (static_cast<int>(line.size()) < lineLen) line.append(static_cast<size_t>(lineLen) - line.size(), 'x');
        j += "\"" + line + "\"";
    }
    j += "]}";
    return j;
}

int main() {
    startup();

    // 1. Hozzájárulás-ellenőrzés: Consent::None -> 0 hálózati hívás; üres URL -> 0 hálózati hívás
    {
        std::atomic<int> calls{ 0 };
        aq::report::Sender sender("https://example.invalid/report", [&](const std::string&, const std::string&) {
            ++calls;
            return aq::report::HttpResponse{ 201, "{\"ok\":true,\"id\":1}", 0, false };
        });
        const auto r = sender.send(sampleReportJson(), aq::report::Consent::None);
        CHECK(!r.ok && r.error == "disabled", "Consent::None eseten disabled hibaval kell megallni");
        CHECK(calls.load() == 0, "Consent::None eseten 0 halozati hivas lehet");

        aq::report::Sender unconfigured("", [&](const std::string&, const std::string&) {
            ++calls;
            return aq::report::HttpResponse{ 201, "{\"ok\":true,\"id\":1}", 0, false };
        });
        CHECK(!unconfigured.configured(), "ures URL-nel configured() hamis");
        const auto r2 = unconfigured.send(sampleReportJson(), aq::report::Consent::ManualOneTime);
        CHECK(!r2.ok && r2.error == "not_configured", "ures URL-nel not_configured");
        CHECK(calls.load() == 0, "ures URL-nel 0 halozati hivas lehet");
    }

    // 2. Méretkorlát (65 536 bájt): legrégebbi naplósorok elhagyása, a legfrissebbek megmaradnak
    {
        const std::string big = sampleReportJson(400, 250); // ~100 KB
        CHECK(big.size() > aq::report::kMaxBodyBytes, "a kiindulo jelentes > 64 KB");
        const std::string trimmed = aq::report::trimLogToMaxBytes(big, aq::report::kMaxBodyBytes);
        CHECK(!trimmed.empty() && trimmed.size() <= aq::report::kMaxBodyBytes, "roviditett torzs <= 65536 bajt");
        ee_analyzer::json::Value v;
        CHECK(ee_analyzer::json::parse(trimmed, v), "roviditett torzs ervenyes JSON");
        const auto* logArr = v.get("log");
        CHECK(logArr && logArr->type == ee_analyzer::json::Value::Type::Array && !logArr->arr.empty(), "log tomb megmaradt");
        if (logArr && !logArr->arr.empty()) {
            CHECK(logArr->arr.back().str.rfind("sor_399_", 0) == 0, "a legfrissebb naplosor megmaradt");
            CHECK(logArr->arr.front().str.rfind("sor_0_", 0) != 0, "a legregebbi naplosor el lett hagyva");
        }
    }

    // 3. Szabad szöveg (note) tisztítása: felhasználónév, HOME, e-mail, IP, MAC, útvonal nem maradhat benne
    {
        const fs::path dir = makeTempDir("scrub");
        const fs::path logPath = dir / "engine.log";
        {
            std::ofstream f(logPath);
            f << "Indulas rendben\n";
        }
        aq::report::ScrubContext ctx;
        ctx.users = { "tesztfelhasznalo" };
        ctx.hosts = { "tesztgep" };
        ctx.homes = { "/home/tesztfelhasznalo", "C:\\Users\\tesztfelhasznalo" };
        const std::string dirtyNote =
            "A /home/tesztfelhasznalo/Zene/dal.flac es C:\\Users\\tesztfelhasznalo\\a.wav lejatszasakor "
            "tesztfelhasznalo@example.com 192.168.1.44 AA:BB:CC:DD:EE:FF tesztgep megakadt";
        const aq::report::Report rep =
            aq::report::build(dir, logPath, ctx, "manual", "noSound!@#", 50, false, dirtyNote);

        CHECK(rep.json.find("tesztfelhasznalo") == std::string::npos, "felhasznalonev nem maradhat a JSON-ban");
        CHECK(rep.json.find("tesztgep") == std::string::npos, "gepnev nem maradhat a JSON-ban");
        CHECK(rep.json.find("example.com") == std::string::npos, "e-mail nem maradhat a JSON-ban");
        CHECK(rep.json.find("192.168.1.44") == std::string::npos, "IP nem maradhat a JSON-ban");
        CHECK(rep.json.find("AA:BB:CC:DD:EE:FF") == std::string::npos, "MAC nem maradhat a JSON-ban");
        CHECK(rep.json.find("dal.flac") == std::string::npos, "utvonal nem maradhat a JSON-ban");
        CHECK(rep.json.find("[Felhasználó]") != std::string::npos, "[Felhasználó] elotag szerepel a logban");
        CHECK(rep.json.find("\"symptom\":\"noSound\"") != std::string::npos, "symptom tisztitva");
        CHECK(rep.preview.find("tesztfelhasznalo") == std::string::npos, "felhasznalonev nem maradhat az elonezetben");

        // Egyedi fájlnév (soha nem ír felül, és nincs benne útvonal)
        std::string f1, f2;
        CHECK(aq::report::saveReportUnique(dir, rep.json, f1), "elso mentes sikeres");
        CHECK(aq::report::saveReportUnique(dir, rep.json, f2), "masodik mentes sikeres");
        CHECK(!f1.empty() && !f2.empty() && f1 != f2, "ket mentes ket kulonbozo fajlnev");
        CHECK(f1.find('/') == std::string::npos && f1.find('\\') == std::string::npos, "fajlnevben nincs utvonal");
        fs::remove_all(dir);
    }

    // 4. Valódi helyi HTTP-szerver (curl Linux/macOS-en, WinHTTP Windowson): 201, 429, 413, 400, 500, hálózati hiba
    {
        std::atomic<int> mode{ 201 };
        LocalHttpServer srv([&](const CapturedRequest& req) -> MockResponse {
            const int m = mode.load();
            if (m == 201) return { 201, "", "{\"ok\":true,\"id\":123}" };
            if (m == 429) return { 429, "Retry-After: 90\r\n", "{\"ok\":false,\"error\":\"rate_limited\"}" };
            if (m == 413) {
                if (req.body.size() > 800) return { 413, "", "{\"ok\":false,\"error\":\"too_large\",\"max_bytes\":800}" };
                return { 201, "", "{\"ok\":true,\"id\":413}" };
            }
            if (m == 400) return { 400, "", "{\"ok\":false,\"error\":\"bad_schema\",\"field\":\"kind\"}" };
            if (m == 500) return { 500, "", "{\"ok\":false,\"error\":\"storage\"}" };
            return { 503, "", "{\"ok\":false,\"error\":\"not_configured\"}" };
        });
        CHECK(srv.start(), "helyi HTTP tesztszerver elindult");

        auto localTransport = [](const std::string& u, const std::string& b) {
            return aq::report::postJsonNative(u, b, true);
        };
        aq::report::Sender sender(srv.url(), localTransport);

        // 201 siker
        mode = 201;
        {
            const auto r = sender.send(sampleReportJson(), aq::report::Consent::ManualOneTime);
            CHECK(r.ok && r.id == 123 && r.httpStatus == 201, "201 sikeres kuldes id=123");
            const auto reqs = srv.requests();
            CHECK(!reqs.empty(), "szerver megkapta a kerest");
            if (!reqs.empty()) {
                CHECK(reqs.back().rawHeaders.find("POST /report") != std::string::npos, "POST /report");
                CHECK(reqs.back().rawHeaders.find("application/json") != std::string::npos, "Content-Type JSON");
                CHECK(reqs.back().rawHeaders.find("Content-Encoding") == std::string::npos, "nincs Content-Encoding");
            }
        }

        // 429 + Retry-After
        mode = 429;
        {
            const auto r = sender.send(sampleReportJson(), aq::report::Consent::ManualOneTime);
            CHECK(!r.ok && r.httpStatus == 429 && r.retryable && r.retryAfterSec == 90, "429 Retry-After=90");
        }

        // 413 -> rövidítés és egyszeri újraküldés -> 201
        mode = 413;
        {
            const std::string medium = sampleReportJson(40, 40); // ~2 KB > 800 B
            const int before = srv.requestCount();
            const auto r = sender.send(medium, aq::report::Consent::ManualOneTime);
            CHECK(r.ok && r.id == 413, "413 utan roviditessel sikeres ujrakuldes");
            CHECK(srv.requestCount() == before + 2, "413 pontosan egyszer probalkozott ujra");
        }

        // 400 -> kliens-hiba, nem újrapróbálható
        mode = 400;
        {
            const auto r = sender.send(sampleReportJson(), aq::report::Consent::ManualOneTime);
            CHECK(!r.ok && r.httpStatus == 400 && !r.retryable && r.error == "bad_schema", "400 nem ujraprobalhato");
        }

        // 500 -> újrapróbálható
        mode = 500;
        {
            const auto r = sender.send(sampleReportJson(), aq::report::Consent::ManualOneTime);
            CHECK(!r.ok && r.httpStatus == 500 && r.retryable, "500 ujraprobalhato");
        }

        // 5. Service: kikapcsolva 0 kérés, bekapcsolva previous-run.json elküldve és törölve
        const fs::path dir = makeTempDir("svc");
        const fs::path prevJson = dir / "previous-run.json";
        const fs::path prevTxt = dir / "previous-run.txt";
        {
            std::ofstream f(prevJson); f << sampleReportJson();
            std::ofstream t(prevTxt); t << "preview text";
        }
        mode = 201;
        const int beforeOff = srv.requestCount();
        {
            aq::report::Service svc;
            svc.start(dir, dir / "engine.log", {}, false, srv.url(), localTransport);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            CHECK(srv.requestCount() == beforeOff, "kikapcsolt Service nem kuld previous-run.json-t");
            CHECK(fs::exists(prevJson), "kikapcsolva a previous-run.json megmarad");
            svc.setEnabled(true);
            // Várakozás a háttérszál küldésére (legfeljebb 3 s)
            for (int i = 0; i < 60 && fs::exists(prevJson); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            CHECK(srv.requestCount() == beforeOff + 1, "bekapcsolva elkuldte a previous-run.json-t");
            CHECK(!fs::exists(prevJson) && !fs::exists(prevTxt), "201 utan torlodott a previous-run.json/.txt");
            svc.stop();
        }
        fs::remove_all(dir);

        // 6. Napló olvasása távirányítóról (Service::readLog): tisztítva, sorszám-korláttal, hálózati kérés nélkül
        {
            const fs::path ldir = makeTempDir("logread");
            {
                std::ofstream f(ldir / "engine.log", std::ios::binary);
                for (int i = 1; i <= 1200; ++i) f << "sor " << i << "\n";
                f << "Kapcsolat: valaki@pelda.hu 192.168.1.23 eszkoz: NappaliTelefon\n";
                f << "utolso sor\n";
            }
            const int beforeLog = srv.requestCount();
            aq::report::Service svc;
            svc.start(ldir, ldir / "engine.log", { "NappaliTelefon" }, false, srv.url(), localTransport);
            int got = 0;
            const std::string def = svc.readLog(0, got);
            CHECK(got == aq::report::kLogReadDefaultLines, "readLog(0): alapertelmezett sorszam");
            CHECK(def.find("utolso sor\n") != std::string::npos, "readLog: az utolso sor benne van");
            CHECK(def.find("sor 1\n") == std::string::npos, "readLog: a legregebbi sorok kimaradnak");
            CHECK(def.find("valaki@pelda.hu") == std::string::npos && def.find("<email>") != std::string::npos,
                  "readLog: e-mail-cim tisztitva");
            CHECK(def.find("192.168.1.23") == std::string::npos && def.find("<ip>") != std::string::npos,
                  "readLog: IP-cim tisztitva");
            CHECK(def.find("NappaliTelefon") == std::string::npos && def.find("<name>") != std::string::npos,
                  "readLog: parositott eszkoz neve tisztitva");
            svc.readLog(5000, got);
            CHECK(got == aq::report::kLogReadMaxLines, "readLog: a keres a felso korlatra szorul");
            svc.readLog(3, got);
            CHECK(got == 3, "readLog(3): pontosan 3 sor");
            CHECK(srv.requestCount() == beforeLog, "readLog: nincs halozati keres");
            svc.stop();
            int none = -1;
            CHECK(svc.readLog(10, none).empty() && none == 0, "leallitott Service: ures naplo");
            fs::remove_all(ldir);
        }

        // Hálózati hiba (leállított szerver portja)
        const std::string deadUrl = srv.url();
        srv.stop();
        aq::report::Sender deadSender(deadUrl, localTransport);
        const auto netRes = deadSender.send(sampleReportJson(), aq::report::Consent::ManualOneTime);
        CHECK(!netRes.ok && netRes.retryable && netRes.error == "network", "halozati hiba retryable=true");
    }

    cleanup();
    if (g_failures > 0) {
        std::cerr << "[test_report_sender] " << g_failures << " hiba.\n";
        return 1;
    }
    std::cout << "[test_report_sender] Minden teszt zöld.\n";
    return 0;
}
