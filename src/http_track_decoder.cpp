/**
 * @file http_track_decoder.cpp
 * @brief A hálózati szám letöltése a memóriába (HTTP Range-dzsel folytatva). A leírás: http_track_decoder.hpp.
 */
#include "http_track_decoder.hpp"
#include "upnp_soap.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace ee_analyzer::upnp {

namespace fs = std::filesystem;

namespace {

fs::path pathFromUtf8(const std::string& s) {
#if defined(__cpp_char8_t)
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
#else
    return fs::u8path(s);
#endif
}
std::string pathToUtf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

/** A letöltések helye: Linuxon a RAM-ban lévő futásidejű mappa, máshol (és ha ott nincs elég hely) a rendszer ideiglenes mappája. */
fs::path runtimeBase() {
    std::error_code ec;
    if (const char* x = std::getenv("XDG_RUNTIME_DIR"); x && *x && fs::is_directory(x, ec)) return fs::path(x) / "aqulify-upnp";
    return fs::temp_directory_path(ec) / "aqulify-upnp";
}
fs::path tempBase() {
    std::error_code ec;
    return fs::temp_directory_path(ec) / "aqulify-upnp";
}

bool enoughSpace(const fs::path& dir, uint64_t need) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::space_info si = fs::space(dir, ec);
    if (ec) return need == 0;
    return si.available > need + (64ull << 20);   // 64 MB tartalék
}

/** "bytes a-b/total" kezdete és teljes hossza (0: ismeretlen, '*'). */
bool parseContentRange(const std::string& v, uint64_t& start, uint64_t& total) {
    unsigned long long a = 0, b = 0;
    char tot[32] = { 0 };
    if (std::sscanf(v.c_str(), "bytes %llu-%llu/%31s", &a, &b, tot) != 3) return false;
    start = a;
    total = 0;
    if (tot[0] != '*') {
        unsigned long long t = 0;
        if (std::sscanf(tot, "%llu", &t) != 1) return false;
        total = t;
    }
    return b >= a;
}

bool parseU64(const std::string& s, uint64_t& out) {
    if (s.empty() || s.size() > 19) return false;
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    out = v;
    return true;
}

std::atomic<uint64_t> g_counter{ 0 };

} // namespace

std::string HttpTrack::safeFileName(const std::string& title) {
    std::string out;
    for (unsigned char c : title) {
        if (c < 0x20 || c == 0x7F || std::strchr("/\\:*?\"<>|", c)) out += ' ';
        else out += static_cast<char>(c);
    }
    out = trim(out);
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();   // Windows: a végén nem lehet pont
    while (!out.empty() && out.front() == '.') out.erase(0, 1);
    if (out.size() > 80) {
        size_t cut = 80;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
        out.resize(cut);
        while (!out.empty() && out.back() == ' ') out.pop_back();
    }
    return out.empty() ? std::string("UPnP") : out;
}

void HttpTrack::cleanupStale() {
    std::error_code ec;
    fs::remove_all(runtimeBase(), ec);
    fs::remove_all(tempBase(), ec);
}

std::shared_ptr<HttpTrack> HttpTrack::start(Config c) {
    std::shared_ptr<HttpTrack> t(new HttpTrack(std::move(c)));
    t->th_ = std::thread([p = t.get()] { p->run(); });
    return t;
}

HttpTrack::~HttpTrack() {
    cancel();
    if (th_.joinable()) th_.join();
    std::error_code ec;
    if (!dir_.empty()) fs::remove_all(pathFromUtf8(dir_), ec);
}

HttpTrack::State HttpTrack::state() const { std::lock_guard<std::mutex> lk(m_); return state_; }
std::string HttpTrack::error() const { std::lock_guard<std::mutex> lk(m_); return err_; }
std::string HttpTrack::path() const { std::lock_guard<std::mutex> lk(m_); return path_; }

bool HttpTrack::wait(int ms) const {
    std::unique_lock<std::mutex> lk(m_);
    return cv_.wait_for(lk, std::chrono::milliseconds(ms), [this] { return state_ != State::Loading; });
}

void HttpTrack::cancel() {
    cancel_ = true;
    std::lock_guard<std::mutex> lk(m_);
    if (conn_) conn_->abort();
}

void HttpTrack::finish(State s, const std::string& err) {
    {
        std::lock_guard<std::mutex> lk(m_);
        state_ = s;
        err_ = err;
    }
    cv_.notify_all();
}

void HttpTrack::run() {
    radio::RadioUrl u;
    std::string perr;
    if (!radio::parseRadioUrl(cfg_.url, u, &perr)) { finish(State::Failed, perr); return; }
    radio::NetOptions net = cfg_.net;
    net.icyMetaData = false;

    std::string ext = extForMime(cfg_.mimeHint);
    if (ext == "-") ext.clear();
    fs::path dir, part;
    std::ofstream out;
    uint64_t have = 0, total = 0;
    bool knownLength = false;
    int resumes = 0, hops = 0;
    std::vector<uint8_t> buf(256 * 1024);
    bool sniffed = false;

    for (;;) {
        if (cancel_) { finish(State::Failed, "aborted"); return; }
        net.extraHeaders = cfg_.net.extraHeaders;
        if (have > 0) net.extraHeaders.push_back("Range: bytes=" + std::to_string(have) + "-");
        radio::HttpStream conn;
        {
            std::lock_guard<std::mutex> lk(m_);
            conn_ = &conn;
        }
        struct Unreg {
            HttpTrack* t;
            ~Unreg() { std::lock_guard<std::mutex> lk(t->m_); t->conn_ = nullptr; }
        } unreg{ this };
        if (cancel_) { finish(State::Failed, "aborted"); return; }
        radio::HttpHead head;
        std::string err;
        if (!conn.open(u, net, head, err)) {
            // megszakadt folytatás: újrapróbálás (a hibás cím / tiltott cím / DNS végleges)
            if (have > 0 && resumes < kMaxResumes && (err == "connect" || err == "timeout")) {
                ++resumes;
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
            finish(State::Failed, cancel_ ? "aborted" : err);
            return;
        }
        const int st = head.status;
        if (st == 301 || st == 302 || st == 303 || st == 307 || st == 308) {
            radio::RadioUrl next;
            if (++hops > 5 || !radio::resolveLocation(u, head.get("location"), next, nullptr)) {
                finish(State::Failed, "redirect");
                return;
            }
            u = next;
            continue;
        }
        if (st == 206 && have > 0) {
            uint64_t from = 0, tot = 0;
            if (!parseContentRange(head.get("content-range"), from, tot) || from != have) {
                finish(State::Failed, "range");
                return;
            }
            if (tot) { total = tot; knownLength = true; }
        } else if (st == 200 || st == 206) {
            // első kérés, vagy a szerver a Range-et nem ismeri: elölről
            have = 0;
            done_ = 0;
            if (out.is_open()) {
                out.close();
                out.open(part, std::ios::binary | std::ios::trunc);
            }
            uint64_t len = 0;
            knownLength = parseU64(head.get("content-length"), len) && len > 0;
            total = knownLength ? len : 0;
            if (st == 206) {
                uint64_t from = 0, tot = 0;
                if (parseContentRange(head.get("content-range"), from, tot) && from == 0 && tot) total = tot;
            }
            if (!knownLength && !cfg_.finite) { finish(State::Live, ""); return; }
            if (ext.empty()) {
                const std::string ct = head.get("content-type");
                std::string m = ct.substr(0, ct.find(';'));
                for (char& c : m) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                ext = extForMime(trim(m));
                if (ext == "-") { finish(State::Failed, "format"); return; }
            }
        } else if (st == 416 && have > 0 && knownLength && have >= total) {
            break;   // már minden megvan
        } else {
            finish(State::Failed, st == 404 || st == 410 ? "notfound" : "http");
            return;
        }
        total_ = total;
        if (total > cfg_.maxBytes) { finish(State::Failed, "size"); return; }
        if (!out.is_open()) {
            const uint64_t need = total ? total : (256ull << 20);
            fs::path base = runtimeBase();
            if (!enoughSpace(base, need)) base = tempBase();
            if (!enoughSpace(base, need)) { finish(State::Failed, "space"); return; }
            dir = base / ("t" + std::to_string(g_counter.fetch_add(1)));
            std::error_code ec;
            fs::create_directories(dir, ec);
            {
                std::lock_guard<std::mutex> lk(m_);
                dir_ = pathToUtf8(dir);
            }
            part = dir / "download.part";
            out.open(part, std::ios::binary | std::ios::trunc);
            if (!out) { finish(State::Failed, "io"); return; }
        }
        // a törzs
        for (;;) {
            if (cancel_) { finish(State::Failed, "aborted"); return; }
            size_t want = buf.size();
            if (knownLength) want = static_cast<size_t>(std::min<uint64_t>(want, total - have));
            if (want == 0) break;
            const size_t n = conn.read(buf.data(), want);
            if (n == 0) break;
            if (!sniffed && have == 0) {
                sniffed = true;
                const std::string s = sniffExt(buf.data(), n);
                if (!s.empty()) ext = s;   // a tartalom felülírja a MIME-típust
            }
            out.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(n));
            if (!out) { finish(State::Failed, "io"); return; }
            have += n;
            done_ = have;
            if (!knownLength && have > cfg_.maxBytes) { finish(State::Failed, "size"); return; }
        }
        if (!knownLength) break;            // a kapcsolat vége a fájl vége (finite)
        if (have >= total) break;
        if (cancel_) { finish(State::Failed, "aborted"); return; }
        if (++resumes > kMaxResumes) { finish(State::Failed, "connect"); return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    out.close();
    if (have == 0) { finish(State::Failed, "http"); return; }
    if (ext.empty()) { finish(State::Failed, "format"); return; }
    const fs::path final = dir / pathFromUtf8(safeFileName(cfg_.title) + "." + ext);
    std::error_code ec;
    fs::rename(part, final, ec);
    if (ec) { finish(State::Failed, "io"); return; }
    {
        std::lock_guard<std::mutex> lk(m_);
        path_ = pathToUtf8(final);
    }
    finish(State::Ready, "");
}

} // namespace ee_analyzer::upnp
