/**
 * @file test_headless_pairing.cpp
 * @brief Képernyő nélküli (headless) Raspberry Pi párosítás, claim-biztonság, migráció, hangerő-őrző
 *        és aqulify-reset egység- és élő socketes tesztek.
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "claim_audio.hpp"
#include "headless_reset.hpp"
#include "mdns_announcer.hpp"
#include "mdns_browser.hpp"
#include "mini_json.hpp"
#include "net_compat.hpp"
#include "pair_cli.hpp"
#include "ws_bridge.hpp"

using namespace ee_analyzer;

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool cond, const std::string& msg) {
    if (cond) {
        ++g_pass;
    } else {
        ++g_fail;
        std::cerr << "[FAIL] " << msg << "\n";
    }
}

/** Egyszerű szinkron TCP kliens a 127.0.0.1:<port> végpontra. */
net::socket_t connectLoopback(int port) {
    net::socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == net::kInvalidSocket) return net::kInvalidSocket;
#ifdef _WIN32
    DWORD tv = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    timeval tv{ 3, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        net::closeSocket(s);
        return net::kInvalidSocket;
    }
    return s;
}

bool sendAll(net::socket_t s, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const long long n = ::send(s, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

/** HTTP kérés küldése és teljes válasz beolvasása. */
std::string httpRequest(int port, const std::string& rawReq) {
    net::socket_t s = connectLoopback(port);
    if (s == net::kInvalidSocket) return {};
    if (!sendAll(s, rawReq)) {
        net::closeSocket(s);
        return {};
    }
    std::string resp;
    char buf[4096];
    for (;;) {
        const long long n = ::recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, static_cast<size_t>(n));
    }
    net::closeSocket(s);
    return resp;
}

struct WsClient {
    net::socket_t sock = net::kInvalidSocket;
    paircli::FrameReader reader;
    std::string handshakeResp;

    ~WsClient() { close(); }

    bool connect(int port, const std::string& pathAndQuery) {
        close();
        sock = connectLoopback(port);
        if (sock == net::kInvalidSocket) return false;
        const std::string req = "GET " + pathAndQuery + " HTTP/1.1\r\n"
                                "Host: 127.0.0.1\r\n"
                                "Upgrade: websocket\r\n"
                                "Connection: Upgrade\r\n"
                                "Sec-WebSocket-Key: QXF1bGlmeUhlYWRsZXNzVGVzdA==\r\n"
                                "Sec-WebSocket-Version: 13\r\n\r\n";
        if (!sendAll(sock, req)) { close(); return false; }
        handshakeResp.clear();
        char buf[2048];
        while (handshakeResp.find("\r\n\r\n") == std::string::npos) {
            const long long n = ::recv(sock, buf, sizeof(buf), 0);
            if (n <= 0 || handshakeResp.size() > 8192) break;
            handshakeResp.append(buf, static_cast<size_t>(n));
        }
        if (handshakeResp.rfind("HTTP/1.1 101", 0) != 0) {
            close();
            return false;
        }
        const size_t end = handshakeResp.find("\r\n\r\n") + 4;
        if (end < handshakeResp.size()) {
            reader.push(handshakeResp.data() + end, handshakeResp.size() - end);
        }
        return true;
    }

    bool sendText(const std::string& text) {
        if (sock == net::kInvalidSocket) return false;
        const uint8_t mask[4] = { 0x37, 0xFA, 0x21, 0x3D };
        return sendAll(sock, paircli::encodeClientText(text, mask));
    }

    bool recvText(std::string& out, int timeoutMs = 2000) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            bool closed = false;
            if (reader.next(out, &closed)) return true;
            if (closed || std::chrono::steady_clock::now() >= deadline) return false;
            char buf[8192];
            const long long n = ::recv(sock, buf, sizeof(buf), 0);
            if (n > 0) reader.push(buf, static_cast<size_t>(n));
            else return false;
        }
    }

    void close() {
        if (sock != net::kInvalidSocket) {
            net::closeSocket(sock);
            sock = net::kInvalidSocket;
        }
    }
};

std::string makeTempDir(const std::string& tag) {
    auto p = std::filesystem::temp_directory_path() /
             ("aq_headless_test_" + tag + "_" + bridge_detail::randomHex(4));
    std::filesystem::create_directories(p);
    return p.string();
}

void testMdnsClaimableUpdate() {
    std::cout << "[1] mDNS claimable TXT frissítés...\n";
    MdnsAnnouncer an;
    an.start(47951, "PiTest", { "127.0.0.1" }, "0123456789abcdef", /*claimable=*/true);
    const auto p1 = an.buildPacket(120);
    std::string s1(p1.begin(), p1.end());
    check(s1.find("claimable=1") != std::string::npos, "mDNS kezdetben claimable=1");
    check(an.isClaimable(), "MdnsAnnouncer::isClaimable() igaz");

    an.setClaimable(false);
    const auto p2 = an.buildPacket(120);
    std::string s2(p2.begin(), p2.end());
    check(s2.find("claimable=0") != std::string::npos, "mDNS setClaimable(false) után claimable=0");
    check(s2.find("claimable=1") == std::string::npos, "mDNS setClaimable(false) után nincs claimable=1");

    an.setClaimable(true);
    const auto p3 = an.buildPacket(120);
    std::string s3(p3.begin(), p3.end());
    check(s3.find("claimable=1") != std::string::npos, "mDNS setClaimable(true) után újra claimable=1");
    an.stop();
}

void testVolumeGuardRaii() {
    std::cout << "[2] ClaimVolumeGuard RAII hangerő-visszaállítás...\n";
    double curLvl = 0.72;
    int curMute = 1;
    std::atomic<bool> active{ false };

    {
        claim_audio::ClaimVolumeGuard g(
            [&](double& l, int& m) { l = curLvl; m = curMute; return true; },
            [&](double l, int m) { curLvl = l; curMute = m; },
            &active, 0.45);
        check(active.load(), "claimVolumeActive igaz a felmondás alatt");
        check(curLvl > 0.44 && curLvl < 0.46 && curMute == 0, "claim alatt fix mérsékelt hangerő, némítás ki");
    }
    check(!active.load(), "claimVolumeActive hamis kilépés után");
    check(curLvl > 0.71 && curLvl < 0.73 && curMute == 1, "normál kilépésnél hangerő + némítás visszaáll");

    // Kivétel útvonalon is vissza kell állnia
    try {
        claim_audio::ClaimVolumeGuard g(
            [&](double& l, int& m) { l = curLvl; m = curMute; return true; },
            [&](double l, int m) { curLvl = l; curMute = m; },
            &active, 0.45);
        throw std::runtime_error("szimulált lejátszási hiba");
    } catch (const std::runtime_error&) {
    }
    check(!active.load(), "kivétel után claimVolumeActive hamis");
    check(curLvl > 0.71 && curLvl < 0.73 && curMute == 1, "kivétel után is visszaáll a hangerő");
}

void testBootReset() {
    std::cout << "[3] aqulify-reset (normál, hibás config út, írásvédett boot)...\n";
    const std::string dir = makeTempDir("reset");
    const std::string bootDir = dir + "/boot";
    const std::string cfgPath = dir + "/remote.json";
    std::filesystem::create_directories(bootDir);

    // 3a. Normál eset
    {
        std::ofstream f(cfgPath);
        f << "{\"enabled\":true,\"port\":47820,\"machine\":\"0123456789abcdef\","
             "\"devices\":[{\"id\":\"aaaa1111\",\"name\":\"Tel\",\"hash\":\"0123456789abcdef0123456789abcdef01234567\",\"owner\":true,\"admin\":true}]}";
    }
    {
        std::ofstream f(bootDir + "/aqulify-reset");
    }
    auto r1 = headless::applyBootReset({ bootDir }, cfgPath);
    check(r1.triggered && r1.configCleared && r1.flagRemoved && r1.error.empty(),
          "aqulify-reset sikeresen törölte a párosításokat és a jelzőfájlt");
    check(!std::filesystem::exists(bootDir + "/aqulify-reset"), "aqulify-reset fájl eltűnt");
    {
        std::ifstream f(cfgPath);
        std::string doc((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        check(doc.find("\"devices\":[]") != std::string::npos, "remote.json devices tömbje üres");
        check(doc.find("\"enabled\":true") != std::string::npos, "remote.json enabled=true maradt");
        check(doc.find("0123456789abcdef") != std::string::npos, "machineId megmaradt");
    }

    // 3b. Hibás remote.json cél (könyvtár, nem fájl) -> a reset fájlnak AKKOR IS törlődnie kell!
    const std::string badCfgDir = dir + "/bad_cfg_dir";
    std::filesystem::create_directories(badCfgDir);
    {
        std::ofstream f(bootDir + "/aqulify-reset");
    }
    auto r2 = headless::applyBootReset({ bootDir }, badCfgDir);
    check(r2.triggered && !r2.configCleared && r2.flagRemoved,
          "hibás config-írásnál is törlődik az aqulify-reset fájl");
    check(!std::filesystem::exists(bootDir + "/aqulify-reset"), "aqulify-reset fájl hibás config mellett is eltűnt");

#ifndef _WIN32
    // 3c. Írásvédett boot mappa -> értelmes hibaüzenet
    const std::string roBoot = dir + "/ro_boot";
    std::filesystem::create_directories(roBoot);
    {
        std::ofstream f(roBoot + "/aqulify-reset");
    }
    chmod(roBoot.c_str(), 0500);
    auto r3 = headless::applyBootReset({ roBoot }, cfgPath);
    chmod(roBoot.c_str(), 0700);
    if (geteuid() != 0) {
        check(r3.triggered && !r3.flagRemoved && !r3.error.empty(),
              "írásvédett boot partíciónál értelmes hibaüzenetet ad");
    }
#endif

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void testMigrationLegacyRemoteJson() {
    std::cout << "[4] Migráció régi remote.json-ból (owner mező nélkül)...\n";
    // 4a. Van admin -> legrégebbi admin lesz owner
    {
        std::string savedDoc =
            "{\"enabled\":true,\"port\":47961,\"machine\":\"1111222233334444\",\"devices\":["
            "{\"id\":\"aa001111\",\"name\":\"RegiNemAdmin\",\"hash\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created\":100,\"last\":100},"
            "{\"id\":\"bb002222\",\"name\":\"UjabbAdmin\",\"hash\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"admin\":true,\"created\":300,\"last\":300},"
            "{\"id\":\"cc003333\",\"name\":\"RegebbiAdmin\",\"hash\":\"cccccccccccccccccccccccccccccccccccccccc\",\"admin\":true,\"created\":200,\"last\":200}"
            "]}";
        BridgeCallbacks cb;
        cb.pullAudio = [](float*, size_t) -> size_t { return 0; };
        cb.sampleRate = [] { return 48000.0; };
        cb.getParams = [] { return transparentParams(); };
        cb.setParams = [](const ParamData&) {};
        cb.loadRemote = [&] { return savedDoc; };
        cb.saveRemote = [&](const std::string& d) { savedDoc = d; return true; };
        WebSocketBridge b(47960);
        check(b.start(cb), "migrációs bridge 4a elindult");
        b.stop();

        json::Value v;
        check(json::parse(savedDoc, v), "migrált remote.json 4a érvényes");
        const auto* devs = v.get("devices");
        check(devs && devs->arr.size() == 3, "mind a 3 eszköz megmaradt");
        bool dev1Owner = false, dev2Owner = false, dev3Owner = false;
        if (devs) {
            for (const auto& e : devs->arr) {
                const std::string id = e.get("id") ? e.get("id")->stringOr("") : "";
                const bool ow = e.get("owner") && e.get("owner")->numberOr(0.0) != 0.0;
                if (id == "aa001111") dev1Owner = ow;
                if (id == "bb002222") dev2Owner = ow;
                if (id == "cc003333") dev3Owner = ow;
            }
        }
        check(!dev1Owner && !dev2Owner && dev3Owner, "migráció: a legrégebbi admin (cc003333) lett tulajdonos");
    }

    // 4b. Nincs admin -> legrégebbi eszköz lesz owner és admin
    {
        std::string savedDoc =
            "{\"enabled\":true,\"port\":47963,\"machine\":\"5555666677778888\",\"devices\":["
            "{\"id\":\"aa001111\",\"name\":\"Ujabb\",\"hash\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"created\":500,\"last\":500},"
            "{\"id\":\"bb002222\",\"name\":\"Legregebbi\",\"hash\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"created\":100,\"last\":100}"
            "]}";
        BridgeCallbacks cb;
        cb.pullAudio = [](float*, size_t) -> size_t { return 0; };
        cb.sampleRate = [] { return 48000.0; };
        cb.getParams = [] { return transparentParams(); };
        cb.setParams = [](const ParamData&) {};
        cb.loadRemote = [&] { return savedDoc; };
        cb.saveRemote = [&](const std::string& d) { savedDoc = d; return true; };
        WebSocketBridge b(47962);
        check(b.start(cb), "migrációs bridge 4b elindult");
        b.stop();

        json::Value v;
        check(json::parse(savedDoc, v), "migrált remote.json 4b érvényes");
        const auto* devs = v.get("devices");
        bool dev2Owner = false, dev2Admin = false, dev1Owner = false;
        if (devs) {
            for (const auto& e : devs->arr) {
                const std::string id = e.get("id") ? e.get("id")->stringOr("") : "";
                const bool ow = e.get("owner") && e.get("owner")->numberOr(0.0) != 0.0;
                const bool ad = e.get("admin") && e.get("admin")->numberOr(0.0) != 0.0;
                if (id == "aa001111") dev1Owner = ow;
                if (id == "bb002222") { dev2Owner = ow; dev2Admin = ad; }
            }
        }
        check(!dev1Owner && dev2Owner && dev2Admin, "migráció (admin nélkül): a legrégebbi eszköz (bb002222) lett owner+admin");
    }
}

void testLiveClaimSecurityAndOwnerFlow() {
    std::cout << "[5] Élő socketes claim biztonság, zárolás, tulajdonos nélküli út és jóváhagyás...\n";
    const std::string bootDir = makeTempDir("boot_live");
    std::string remoteStore = "{\"enabled\":true,\"port\":47971,\"machine\":\"abcdef0123456789\",\"devices\":[]}";

    std::mutex codeMu;
    std::string spokenCode;
    std::string spokenLang;
    int speakCalls = 0;

    BridgeCallbacks cb;
    cb.pullAudio = [](float*, size_t) -> size_t { return 0; };
    cb.sampleRate = [] { return 48000.0; };
    cb.getParams = [] { return transparentParams(); };
    cb.setParams = [](const ParamData&) {};
    cb.loadRemote = [&] { return remoteStore; };
    cb.saveRemote = [&](const std::string& d) { remoteStore = d; return true; };
    cb.bootDirs = [&] { return std::vector<std::string>{ bootDir }; };
    cb.speakClaimCode = [&](const std::string& code, const std::string& lang) {
        std::lock_guard<std::mutex> lk(codeMu);
        spokenCode = code;
        spokenLang = lang;
        ++speakCalls;
        return true;
    };

    WebSocketBridge bridge(47970);
    check(bridge.start(cb), "élő teszt bridge elindult");
    check(bridge.isClaimable(), "gazdátlan módban indul (isClaimable=true)");
    check(bridge.mdnsClaimable(), "mDNS claimable=1 gazdátlan módban");
    check(!bridge.isNoAudioFallbackActive(), "boot fájl nélkül no-audio tartalék KI");

    // 5a. Tulajdonos nélküli gépen a push-párosítás (másik telefon engedélyezése) NEM nyitható meg!
    {
        WsClient c;
        const bool ok = c.connect(47971, "/?pair=request&client=aqulify_remote&name=Tamado");
        check(!ok && c.handshakeResp.find("503") != std::string::npos &&
              c.handshakeResp.find("no-window") != std::string::npos,
              "tulajdonos nélkül pair=request 503 no-window");
    }

    // 5b. GET /claim/start -> 405, Origin fejléccel -> 403, hamis kliens -> 403
    {
        const std::string rGet = httpRequest(47971, "GET /claim/start?client=aqulify_remote HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
        check(rGet.find("405 Method Not Allowed") != std::string::npos, "GET /claim/start 405-tel elutasítva");

        const std::string rOrig = httpRequest(47971, "POST /claim/start?client=aqulify_remote HTTP/1.1\r\nHost: 127.0.0.1\r\nOrigin: http://evil.example\r\nContent-Length: 0\r\n\r\n");
        check(rOrig.find("403 Forbidden") != std::string::npos && rOrig.find("origin") != std::string::npos,
              "Origin fejléces POST /claim/start 403-mal elutasítva");

        const std::string rCli = httpRequest(47971, "POST /claim/start?client=other_app HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n");
        check(rCli.find("403 Forbidden") != std::string::npos && rCli.find("client") != std::string::npos,
              "hamis kliens POST /claim/start 403-mal elutasítva");
    }

    // 5c. Érvényes POST /claim/start -> 200 OK, nincs benne a kód; 2. azonnali hívás -> 429 cooldown
    std::string code1;
    {
        const std::string r1 = httpRequest(47971, "POST /claim/start?client=aqulify_remote&lang=hu HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n");
        check(r1.find("200 OK") != std::string::npos, "POST /claim/start 200 OK");
        check(r1.find("\"no_audio_fallback\":false") != std::string::npos, "no_audio_fallback=false boot fájl nélkül");

        for (int i = 0; i < 40; ++i) {
            { std::lock_guard<std::mutex> lk(codeMu); if (speakCalls >= 1) { code1 = spokenCode; break; } }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        check(code1.size() == 4, "4 jegyű kód generálva a hangos felmondáshoz");
        check(r1.find(code1) == std::string::npos, "a /claim/start válasza NEM tartalmazza a kódot");

        const std::string r2 = httpRequest(47971, "POST /claim/start?client=aqulify_remote HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n");
        check(r2.find("429 Too Many Requests") != std::string::npos && r2.find("cooldown") != std::string::npos,
              "30 mp-en belüli 2. POST /claim/start 429 cooldown");
    }

    // 5d. Új /claim/start (cooldown tesztben 0-ra állítva) érvényteleníti az előző kódot
    bridge.setClaimStartCooldownForTest(0);
    std::string code2;
    {
        const std::string r = httpRequest(47971, "POST /claim/start?client=aqulify_remote&lang=en HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n");
        check(r.find("200 OK") != std::string::npos, "2. POST /claim/start 200 OK");
        for (int i = 0; i < 40; ++i) {
            { std::lock_guard<std::mutex> lk(codeMu); if (speakCalls >= 2) { code2 = spokenCode; break; } }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        check(code2.size() == 4, "új 4 jegyű kód generálva");
    }

    // 5e. DAC / fájl nélkül fallback=true elutasítva!
    {
        WsClient c;
        check(c.connect(47971, "/?claim=1&client=aqulify_remote&name=NoAudioTry"), "claim WS csatlakozott");
        check(c.sendText("{\"type\":\"claim_submit\",\"fallback\":true}"), "fallback=true elküldve");
        std::string msg;
        check(c.recvText(msg) && msg.find("\"ok\":false") != std::string::npos,
              "boot fájl nélkül fallback=true elutasítva (1. hiba)");
    }

    // 5f. claimPending kliens nem küldhet get_state / set_preamp parancsot
    {
        WsClient c;
        check(c.connect(47971, "/?claim=1&client=aqulify_remote&name=BypassTry"), "bypass WS csatlakozott");
        c.sendText("{\"type\":\"get_state\"}");
        std::string msg;
        check(!c.recvText(msg, 400), "claimPending kliens nem kap választ get_state-re, bontva");
    }

    // 5g. További 4 hibás kód (összesen 5 hiba) -> 10 perc globális zárolás ÉS a helyes kód is érvénytelen!
    const std::string wrongCode = (code2 == "0000") ? "9999" : "0000";
    for (int attempt = 2; attempt <= 5; ++attempt) {
        WsClient c;
        check(c.connect(47971, "/?claim=1&client=aqulify_remote&name=Brute"), "hibás próba WS csatlakozott");
        c.sendText("{\"type\":\"claim_submit\",\"code\":\"" + wrongCode + "\"}");
        std::string msg;
        const bool got = c.recvText(msg);
        if (attempt < 5) {
            check(got && msg.find("\"ok\":false") != std::string::npos && msg.find("\"locked\":false") != std::string::npos,
                  "hibás próba #" + std::to_string(attempt) + " elutasítva, még nincs zár");
        } else {
            check(got && msg.find("\"ok\":false") != std::string::npos && msg.find("\"locked\":true") != std::string::npos,
                  "5. hibás próba -> globális zárolás (locked=true)");
        }
    }
    check(bridge.isClaimLocked(), "5. hibás próba után isClaimLocked() igaz");
    {
        WsClient c;
        const bool ok = c.connect(47971, "/?claim=1&client=aqulify_remote&name=AfterLock");
        check(!ok && c.handshakeResp.find("429") != std::string::npos, "zárolás alatt /?claim=1 429");
        const std::string rLock = httpRequest(47971, "POST /claim/start?client=aqulify_remote HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n");
        check(rLock.find("429") != std::string::npos, "zárolás alatt POST /claim/start is 429");
    }

    bridge.stop();

    // 5h. Friss indítás: sikeres claim -> tulajdonos + mDNS claimable=0 + 2. telefon jóváhagyása + revoke
    {
        WebSocketBridge b2(47972);
        remoteStore = "{\"enabled\":true,\"port\":47973,\"machine\":\"abcdef0123456789\",\"devices\":[]}";
        { std::lock_guard<std::mutex> lk(codeMu); speakCalls = 0; spokenCode.clear(); }
        check(b2.start(cb), "2. élő teszt bridge elindult");
        b2.setClaimStartCooldownForTest(0);

        const std::string rStart = httpRequest(47973, "POST /claim/start?client=aqulify_remote&lang=hu HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n");
        check(rStart.find("200 OK") != std::string::npos, "sikeres claimhez POST /claim/start 200 OK");
        std::string goodCode;
        for (int i = 0; i < 40; ++i) {
            { std::lock_guard<std::mutex> lk(codeMu); if (speakCalls >= 1) { goodCode = spokenCode; break; } }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        check(goodCode.size() == 4, "4 jegyű kód megvan");

        WsClient ownerWs;
        check(ownerWs.connect(47973, "/?claim=1&client=aqulify_remote&name=TulajTel&app=1111111111111111"),
              "tulajdonos claim WS csatlakozott");
        ownerWs.sendText("{\"type\":\"claim_submit\",\"code\":\"" + goodCode + "\"}");
        std::string pairedMsg, stateMsg;
        check(ownerWs.recvText(pairedMsg) && pairedMsg.find("\"type\":\"paired\"") != std::string::npos &&
              pairedMsg.find("\"owner\":true") != std::string::npos && pairedMsg.find("\"admin\":true") != std::string::npos,
              "helyes kódra paired + owner:true + admin:true");
        check(ownerWs.recvText(stateMsg) && stateMsg.find("\"type\":\"state\"") != std::string::npos,
              "claim után state üzenet megérkezett");

        json::Value pv;
        check(json::parse(pairedMsg, pv), "paired JSON érvényes");
        const std::string ownerToken = pv.get("token") ? pv.get("token")->stringOr("") : "";
        check(ownerToken.size() == 64, "64 hex karakteres token kiadva");
        check(!b2.isClaimable(), "tulajdonos után isClaimable=false");
        check(!b2.mdnsClaimable(), "tulajdonos után mDNS claimable=0");

        // Már van tulajdonos -> új POST /claim/start 409 Conflict
        const std::string rAgain = httpRequest(47973, "POST /claim/start?client=aqulify_remote HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n");
        check(rAgain.find("409 Conflict") != std::string::npos, "tulajdonos után POST /claim/start 409");

        // Második telefon jóváhagyása a tulajdonos telefonról
        std::string secondToken, secondId;
        std::thread secondThread([&]() {
            WsClient sec;
            if (sec.connect(47973, "/?pair=request&client=aqulify_remote&name=MasodikTel&app=2222222222222222")) {
                std::string m;
                if (sec.recvText(m, 3000)) {
                    json::Value v;
                    if (json::parse(m, v) && v.get("type") && v.get("type")->stringOr("") == "paired") {
                        secondToken = v.get("token") ? v.get("token")->stringOr("") : "";
                        secondId = v.get("id") ? v.get("id")->stringOr("") : "";
                    }
                }
            }
        });

        // A tulajdonos megkapja a pair_request értesítést és jóváhagyja
        std::string reqId;
        for (int i = 0; i < 20; ++i) {
            std::string m;
            if (!ownerWs.recvText(m, 500)) break;
            json::Value v;
            if (json::parse(m, v) && v.get("type") && v.get("type")->stringOr("") == "pair_request") {
                reqId = v.get("requestId") ? v.get("requestId")->stringOr("") : "";
                break;
            }
        }
        check(!reqId.empty(), "tulajdonos megkapta a pair_request értesítést");
        ownerWs.sendText("{\"type\":\"pair_approve\",\"requestId\":\"" + reqId + "\",\"approve\":true}");
        secondThread.join();
        check(!secondToken.empty() && !secondId.empty(), "második telefon jóváhagyva, tokent kapott");

        // Tulajdonos get_remote -> eszközlista megjön, de 6 jegyű titkos kód üres
        ownerWs.sendText("{\"type\":\"get_remote\"}");
        bool gotRemoteList = false;
        for (int i = 0; i < 15; ++i) {
            std::string m;
            if (!ownerWs.recvText(m, 500)) break;
            if (m.find("\"type\":\"remote\"") != std::string::npos) {
                gotRemoteList = true;
                check(m.find("\"code\":\"\"") != std::string::npos, "távoli tulajdonos nem kap 6 jegyű kódot");
                check(m.find(secondId) != std::string::npos, "eszközlistában szerepel a második telefon");
                break;
            }
        }
        check(gotRemoteList, "tulajdonos megkapta a remote eszközlistát");

        // Tulajdonos törli a második telefont (remote_revoke)
        ownerWs.sendText("{\"type\":\"remote_revoke\",\"id\":\"" + secondId + "\"}");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        {
            WsClient secAfter;
            check(!secAfter.connect(47973, "/?token=" + secondToken), "törölt második telefon tokenje érvénytelen");
        }

        ownerWs.close();
        b2.stop();

        // Újraindítás után a tulajdonos tokenje és owner joga megmarad
        WebSocketBridge b3(47974);
        check(b3.start(cb), "újraindított bridge elindult");
        check(!b3.isClaimable(), "újraindítás után sem gazdátlan");
        {
            WsClient reconnect;
            check(reconnect.connect(47973, "/?token=" + ownerToken), "tulajdonos tokenje újraindítás után is érvényes");
            std::string m;
            check(reconnect.recvText(m) && m.find("\"type\":\"machine\"") != std::string::npos &&
                  m.find("\"owner\":true") != std::string::npos && m.find("\"admin\":true") != std::string::npos,
                  "újraindítás után machine üzenetben owner:true és admin:true");
        }
        b3.stop();
    }

    // 5i. Boot-partíciós aqulify-claim-no-audio tartalék (10 percen belül OK, 10 perc után elutasítva)
    {
        const std::string flagPath = bootDir + "/aqulify-claim-no-audio";
        { std::ofstream f(flagPath); }
        remoteStore = "{\"enabled\":true,\"port\":47975,\"machine\":\"9999888877776666\",\"devices\":[]}";

        // 10 perc után: elutasítva
        WebSocketBridge bExp(47976);
        check(bExp.start(cb), "no-audio lejárt bridge elindult");
        bExp.setStartedAtOffsetSecondsForTest(601);
        check(!bExp.isNoAudioFallbackActive(), "10 perc után aqulify-claim-no-audio nem aktív");
        {
            WsClient c;
            check(c.connect(47975, "/?claim=1&client=aqulify_remote&name=Late"), "late claim WS");
            c.sendText("{\"type\":\"claim_submit\",\"fallback\":true}");
            std::string m;
            check(c.recvText(m) && m.find("\"ok\":false") != std::string::npos, "10 perc után fallback elutasítva");
        }
        bExp.stop();

        // 10 percen belül: elfogadva + jelzőfájl törölve
        WebSocketBridge bOk(47977);
        check(bOk.start(cb), "no-audio aktív bridge elindult");
        check(bOk.isNoAudioFallbackActive(), "10 percen belül aqulify-claim-no-audio aktív");
        const std::string rStart = httpRequest(47975, "POST /claim/start?client=aqulify_remote HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n");
        check(rStart.find("200 OK") != std::string::npos && rStart.find("\"no_audio_fallback\":true") != std::string::npos,
              "no-audio POST /claim/start jelzi a tartalék módot");
        {
            WsClient c;
            check(c.connect(47975, "/?claim=1&client=aqulify_remote&name=NoAudioOwner"), "no-audio claim WS");
            c.sendText("{\"type\":\"claim_submit\",\"fallback\":true}");
            std::string m;
            check(c.recvText(m) && m.find("\"type\":\"paired\"") != std::string::npos &&
                  m.find("\"owner\":true") != std::string::npos,
                  "no-audio tartalékkal sikeres claim, owner:true");
        }
        check(!std::filesystem::exists(flagPath), "sikeres no-audio claim után az aqulify-claim-no-audio fájl törlődött");
        bOk.stop();
    }

    std::error_code ec;
    std::filesystem::remove_all(bootDir, ec);
}

void testClaimAudioWavBuild() {
    std::cout << "[6] Számjegy-hangminták (0-9) összefűzése és angol fallback...\n";
    // A forrásfa data/claim_audio mappáját keressük (a teszt build-wsl-ből vagy gyökérből is futhat)
    std::vector<std::string> roots = { "data/claim_audio", "../data/claim_audio", "../../data/claim_audio" };
    const std::string huDir = claim_audio::resolveDigitDir(roots, "hu");
    const std::string deDir = claim_audio::resolveDigitDir(roots, "de");
    check(!huDir.empty() && huDir.find("hu") != std::string::npos, "magyar számjegy-hangminták megtalálhatók");
    check(!deDir.empty() && deDir.find("en") != std::string::npos, "nem létező nyelv (de) angolra esik vissza");

    const std::string tmpDir = makeTempDir("wav");
    const std::string wavHu = tmpDir + "/out_hu.wav";
    const std::string wavFb = tmpDir + "/out_fb.wav";
    const int durHu = claim_audio::buildCodeWav("4207", "hu", roots, wavHu);
    const int durFb = claim_audio::buildCodeWav("9183", "ja", roots, wavFb);
    check(durHu >= 1500 && durHu <= 8000, "magyar 4 jegyű WAV hossza ésszerű (1.5-8 s)");
    check(durFb >= 1500 && durFb <= 8000, "angol fallback 4 jegyű WAV hossza ésszerű (1.5-8 s)");

    std::vector<int16_t> pcm;
    uint32_t sr = 0;
    check(claim_audio::readPcm16MonoWav(wavHu, pcm, sr) && sr == 16000 && !pcm.empty(),
          "generált magyar WAV érvényes 16 kHz mono PCM16");
    check(claim_audio::readPcm16MonoWav(wavFb, pcm, sr) && sr == 16000 && !pcm.empty(),
          "generált angol fallback WAV érvényes 16 kHz mono PCM16");

    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);
}

} // namespace

int main() {
    net::startup();
    testMdnsClaimableUpdate();
    testVolumeGuardRaii();
    testBootReset();
    testMigrationLegacyRemoteJson();
    testLiveClaimSecurityAndOwnerFlow();
    testClaimAudioWavBuild();
    net::cleanup();

    std::cout << "\nEredmény: " << g_pass << " OK, " << g_fail << " HIBA.\n";
    return g_fail == 0 ? 0 : 1;
}
