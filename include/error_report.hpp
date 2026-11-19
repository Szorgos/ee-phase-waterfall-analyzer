#pragma once
/**
 * @file error_report.hpp
 * @brief A helyi hibajelentés összeállítása fájlokból (állapotfájl, összeomlás-jel, napló) és a tisztítás.
 *
 * - build(): a motor diagnosztikai mappájából (state.json, crash.txt) és a naplóból áll össze a jelentés: JSON és
 *   olvasható előnézet. A futó motortól és a hídjától független (fájlokból dolgozik), ezért beragadt híd mellett,
 *   és a 2. szakaszban a felület gazdaprogramjából is hívható.
 * - scrub(): az ELKÉSZÜLT szövegen fut (nem csak egyes mezőkön): fájlútvonalak → <path>; felhasználónév, gépnév, a
 *   felhasználó által adott eszköznevek, IP- és MAC-címek, e-mail-címek → kitörölve. A hangeszközök gyári típusneve
 *   (pl. „FiiO BTR7”) adat, megmarad.
 * - Hálózati küldés nincs (az a 3. szakasz).
 */
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "mini_json.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

namespace aq::report {

/** A naplóban minden motorindulás ezzel a sorral kezdődik (Windows: LogSink, Linux: a napló-tükör). */
inline constexpr const char* kLogBanner = "===== Aqulify motor indul";

struct ScrubContext {
    std::vector<std::string> users;   // felhasználónevek (és a teljes név, ha ismert)
    std::vector<std::string> hosts;   // gépnév
    std::vector<std::string> homes;   // otthoni mappák
    std::vector<std::string> names;   // a felhasználó által adott nevek (párosított eszközök, zónák)
};

namespace detail {

inline bool isAsciiAlnum(unsigned char c) { return c < 128 && std::isalnum(c); }
inline bool isHex(unsigned char c) { return c < 128 && std::isxdigit(c); }
inline bool isDigit(unsigned char c) { return c >= '0' && c <= '9'; }
/** Szóhatárhoz: ASCII betű/szám, aláhúzás és minden UTF-8 bájt (ékezetes betűk) a szó része. */
inline bool isWordish(unsigned char c) { return isAsciiAlnum(c) || c == '_' || c >= 0x80; }
inline char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

inline bool equalsNoCaseAt(const std::string& s, size_t pos, const std::string& needle) {
    if (pos + needle.size() > s.size()) return false;
    for (size_t k = 0; k < needle.size(); ++k)
        if (lowerAscii(s[pos + k]) != lowerAscii(needle[k])) return false;
    return true;
}

/** A needle minden előfordulása (kis- és nagybetűtől függetlenül) → repl; wordBoundary: csak egész szóként. */
inline void replaceNoCase(std::string& s, const std::string& needle, const std::string& repl, bool wordBoundary) {
    if (needle.size() < 3) return;   // túl rövid név: nem törölhető biztonságosan (pl. „a”)
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (equalsNoCaseAt(s, i, needle)) {
            const bool leftOk = !wordBoundary || i == 0 || !isWordish(static_cast<unsigned char>(s[i - 1]));
            const size_t e = i + needle.size();
            const bool rightOk = !wordBoundary || e >= s.size() || !isWordish(static_cast<unsigned char>(s[e]));
            if (leftOk && rightOk) {
                out += repl;
                i = e;
                continue;
            }
        }
        out += s[i++];
    }
    s.swap(out);
}

inline std::string doubledBackslashes(const std::string& in) {
    std::string o;
    for (char c : in) { o += c; if (c == '\\') o += '\\'; }
    return o;
}

/** Útvonal végének keresése: sorvég, idézőjel; JSON-módban egy (nem \\ és nem \/) escape is. */
inline size_t pathEnd(const std::string& s, size_t i, bool jsonMode) {
    while (i < s.size()) {
        const char c = s[i];
        if (c == '\n' || c == '\r' || c == '"' || c == '\'' || c == '\t') break;
        if (c == '\\' && jsonMode) {
            if (i + 1 < s.size() && (s[i + 1] == '\\' || s[i + 1] == '/')) { i += 2; continue; }
            break;
        }
        ++i;
    }
    return i;
}

inline bool pathStartOk(const std::string& s, size_t i) {
    if (i == 0) return true;
    const unsigned char p = static_cast<unsigned char>(s[i - 1]);
    return !(isWordish(p) || p == '.' || p == '-' || p == '/' || p == '\\' || p == '~');
}

inline void scrubPaths(std::string& s, bool jsonMode) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        // Egy már kicserélt otthoni mappa (<path>) folytatása is az útvonal része: <path>/Zene/a.flac, <path>\Music\a.flac
        if (s.compare(i, 6, "<path>") == 0) {
            out += "<path>";
            i += 6;
            if (i < s.size() && (s[i] == '/' || s[i] == '\\')) i = pathEnd(s, i, jsonMode);
            continue;
        }
        // Helyi visszacsatolási URL (ws://127.0.0.1:47899, http://localhost:8080): a gép és a port nem személyes, marad;
        // az utána következő útvonal (/zene/…) viszont <path>
        if (c == '/' && i > 0 && s[i - 1] == ':' && s.compare(i, 2, "//") == 0 &&
            (s.compare(i + 2, 4, "127.") == 0 || s.compare(i + 2, 9, "localhost") == 0 || s.compare(i + 2, 5, "[::1]") == 0)) {
            size_t e = i + 2;
            while (e < s.size() && (isAsciiAlnum(static_cast<unsigned char>(s[e])) || s[e] == '.' || s[e] == ':' || s[e] == '[' ||
                                    s[e] == ']' || s[e] == '-'))
                ++e;
            out.append(s, i, e - i);
            i = e;
            if (i < s.size() && s[i] == '/') {
                const size_t pe = pathEnd(s, i, jsonMode);
                if (pe - i > 1) { out += "<path>"; i = pe; }
            }
            continue;
        }
        // Unix: /a/b… vagy ~/…
        if ((c == '/' || (c == '~' && i + 1 < s.size() && s[i + 1] == '/')) && pathStartOk(s, i)) {
            const size_t e = pathEnd(s, i, jsonMode);
            const std::string cand = s.substr(i, e - i);
            if (c == '~' || std::count(cand.begin(), cand.end(), '/') >= 2) {
                out += "<path>";
                i = e;
                continue;
            }
        }
        // Windows: C:\… vagy C:/…
        if (i + 2 < s.size() && std::isalpha(static_cast<unsigned char>(c)) && static_cast<unsigned char>(c) < 128 &&
            s[i + 1] == ':' && (s[i + 2] == '\\' || s[i + 2] == '/') && pathStartOk(s, i)) {
            out += "<path>";
            i = pathEnd(s, i + 2, jsonMode);
            continue;
        }
        // UNC: \\gép\megosztás (JSON-ban \\\\gép\\megosztás)
        if (c == '\\' && pathStartOk(s, i)) {
            const size_t need = jsonMode ? 4 : 2;
            bool unc = i + need < s.size();
            for (size_t k = 0; unc && k < need; ++k) unc = s[i + k] == '\\';
            if (unc && isWordish(static_cast<unsigned char>(s[i + need]))) {
                out += "<path>";
                i = pathEnd(s, i + need, jsonMode);
                continue;
            }
        }
        out += c;
        ++i;
    }
    s.swap(out);
}

inline void scrubEmails(std::string& s) {
    size_t at = 0;
    while ((at = s.find('@', at)) != std::string::npos) {
        size_t l = at;
        while (l > 0 && (isAsciiAlnum(static_cast<unsigned char>(s[l - 1])) || std::string("._%+-").find(s[l - 1]) != std::string::npos)) --l;
        size_t r = at + 1;
        bool dot = false;
        while (r < s.size() && (isAsciiAlnum(static_cast<unsigned char>(s[r])) || s[r] == '.' || s[r] == '-')) { dot |= s[r] == '.'; ++r; }
        while (r > at + 1 && s[r - 1] == '.') --r;   // a mondatvégi pont nem a domain része
        if (l < at && dot && r > at + 3) {
            s.replace(l, r - l, "<email>");
            at = l + 7;
        } else {
            ++at;
        }
    }
}

inline void scrubMacs(std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (i + 17 <= s.size() && (i == 0 || !(isAsciiAlnum(static_cast<unsigned char>(s[i - 1])) || s[i - 1] == ':' || s[i - 1] == '-'))) {
            const char sep = s[i + 2];
            bool ok = (sep == ':' || sep == '-' || sep == '_');
            for (int g = 0; ok && g < 6; ++g) {
                const size_t p = i + static_cast<size_t>(g) * 3;
                ok = isHex(static_cast<unsigned char>(s[p])) && isHex(static_cast<unsigned char>(s[p + 1])) && (g == 5 || s[p + 2] == sep);
            }
            const size_t e = i + 17;
            if (ok && (e >= s.size() || !(isAsciiAlnum(static_cast<unsigned char>(s[e])) || s[e] == sep))) {
                out += "<mac>";
                i = e;
                continue;
            }
        }
        out += s[i++];
    }
    s.swap(out);
}

inline void scrubIpv4(std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (isDigit(c) && (i == 0 || !(isWordish(static_cast<unsigned char>(s[i - 1])) || s[i - 1] == '.'))) {
            size_t j = i;
            int parts = 0;
            bool ok = true;
            int first = -1, oct[4] = { 0, 0, 0, 0 };
            while (parts < 4) {
                size_t k = j;
                int v = 0;
                while (k < s.size() && isDigit(static_cast<unsigned char>(s[k])) && k - j < 4) v = v * 10 + (s[k++] - '0');
                if (k == j || k - j > 3 || v > 255) { ok = false; break; }
                oct[parts] = v;
                if (first < 0) first = v;
                ++parts;
                j = k;
                if (parts < 4) {
                    if (j < s.size() && s[j] == '.') ++j;
                    else { ok = false; break; }
                }
            }
            const bool endOk = j >= s.size() ||
                               !(isWordish(static_cast<unsigned char>(s[j])) ||
                                 (s[j] == '.' && j + 1 < s.size() && isDigit(static_cast<unsigned char>(s[j + 1]))));
            // Négyrészes kernelverzió (6.6.87.2-microsoft-standard-WSL2): a négyes után „-szó-” következik. Egy címhez
            // fűzött magyar rag (192.168.1.23-hoz) után nincs második kötőjel, az továbbra is cím.
            bool kernel = false;
            if (ok && parts == 4 && j + 1 < s.size() && s[j] == '-' && std::isalpha(static_cast<unsigned char>(s[j + 1])) &&
                static_cast<unsigned char>(s[j + 1]) < 128) {
                size_t k = j + 1;
                while (k < s.size() && isAsciiAlnum(static_cast<unsigned char>(s[k]))) ++k;
                kernel = k + 1 < s.size() && s[k] == '-' && isAsciiAlnum(static_cast<unsigned char>(s[k + 1]));
            }
            if (ok && parts == 4 && endOk && !kernel) {
                const bool loopback = oct[0] == 127 || (oct[0] == 0 && oct[1] == 0 && oct[2] == 0 && oct[3] == 0);
                out += loopback ? s.substr(i, j - i) : std::string("<ip>");
                i = j;
                continue;
            }
            // nem cím: a számjegysor egyben megy tovább (ne kezdődjön új keresés a közepén)
            size_t k = i;
            while (k < s.size() && (isDigit(static_cast<unsigned char>(s[k])) || s[k] == '.')) ++k;
            out.append(s, i, k - i);
            i = k;
            continue;
        }
        out += s[i++];
    }
    s.swap(out);
}

inline void scrubIpv6(std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    auto inRun = [](unsigned char c) { return isHex(c) || c == ':'; };
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (inRun(c) && (i == 0 || !(isWordish(static_cast<unsigned char>(s[i - 1])) || s[i - 1] == '.' || s[i - 1] == ':'))) {
            size_t j = i;
            while (j < s.size() && inRun(static_cast<unsigned char>(s[j]))) ++j;
            const std::string run = s.substr(i, j - i);
            const auto colons = std::count(run.begin(), run.end(), ':');
            const bool dbl = run.find("::") != std::string::npos;
            const bool letter = std::any_of(run.begin(), run.end(), [](char ch) { return std::isalpha(static_cast<unsigned char>(ch)) != 0; });
            const bool hexDigit = std::any_of(run.begin(), run.end(), [](char ch) { return std::isxdigit(static_cast<unsigned char>(ch)) != 0; });
            size_t e = j;
            if (e < s.size() && s[e] == '%') {   // zónaazonosító (fe80::1%eth0)
                ++e;
                while (e < s.size() && isWordish(static_cast<unsigned char>(s[e]))) ++e;
            }
            const bool endOk = e >= s.size() ||
                               !(isWordish(static_cast<unsigned char>(s[e])) ||
                                 (s[e] == '.' && e + 1 < s.size() && isDigit(static_cast<unsigned char>(s[e + 1]))));
            const bool looksV6 = colons >= 2 && hexDigit && (dbl || letter || colons >= 3) && endOk;
            if (looksV6 && run != "::1" && run != "::") {
                out += "<ip>";
                i = e;
                continue;
            }
            out.append(s, i, j - i);
            i = j;
            continue;
        }
        out += s[i++];
    }
    s.swap(out);
}

inline void collapseRepeats(std::string& s) {
    for (const char* tok : { "<path>", "<ip>", "<mac>" }) {
        const std::string twice = std::string(tok) + tok;
        size_t p;
        while ((p = s.find(twice)) != std::string::npos) s.erase(p, std::char_traits<char>::length(tok));
    }
}

inline std::string jsonEsc(const std::string& in) {
    std::string o;
    o.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) o += ' ';
                else o += c;
        }
    }
    return o;
}

inline std::string readAll(const std::filesystem::path& path, size_t maxBytes) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size <= 0) return {};
    const std::streamoff start = size > static_cast<std::streamoff>(maxBytes) ? size - static_cast<std::streamoff>(maxBytes) : 0;
    f.seekg(start);
    std::string data(static_cast<size_t>(size - start), '\0');
    f.read(&data[0], static_cast<std::streamsize>(data.size()));
    data.resize(static_cast<size_t>(f.gcount()));
    return data;
}

inline std::vector<std::string> splitLines(const std::string& data) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : data) {
        if (c == '\n') { lines.push_back(cur); cur.clear(); }
        else if (c != '\r') cur += c;
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

/** Egy JSON-érték olvasható, lapos alakja az előnézethez („kulcs: érték” sorok). */
inline void flatten(const ee_analyzer::json::Value& v, const std::string& prefix, std::string& out) {
    using T = ee_analyzer::json::Value::Type;
    switch (v.type) {
        case T::Object:
            for (const auto& [k, x] : v.obj) flatten(x, prefix.empty() ? k : prefix + "." + k, out);
            break;
        case T::Array:
            for (size_t i = 0; i < v.arr.size(); ++i) flatten(v.arr[i], prefix + "[" + std::to_string(i) + "]", out);
            break;
        case T::String: out += "  " + prefix + ": " + v.str + "\n"; break;
        case T::Bool: out += "  " + prefix + ": " + (v.b ? "true" : "false") + "\n"; break;
        case T::Number: {
            char b[48];
            std::snprintf(b, sizeof(b), "%g", v.num);
            out += "  " + prefix + ": " + b + "\n";
            break;
        }
        case T::Null: out += "  " + prefix + ": null\n"; break;
    }
}

} // namespace detail

/** A szöveg tisztítása (jsonMode: a szöveg JSON, a visszaperjelek escape-elve vannak). */
inline std::string scrub(const std::string& text, const ScrubContext& ctx, bool jsonMode = false) {
    std::string s = text;
    // 1. otthoni mappák (sima és JSON-escape-elt alakban, / elválasztóval is)
    for (const auto& h : ctx.homes) {
        if (h.size() < 3) continue;
        detail::replaceNoCase(s, h, "<path>", false);
        if (h.find('\\') != std::string::npos) {
            detail::replaceNoCase(s, detail::doubledBackslashes(h), "<path>", false);
            std::string fwd = h;
            std::replace(fwd.begin(), fwd.end(), '\\', '/');
            detail::replaceNoCase(s, fwd, "<path>", false);
        }
    }
    // 2. nevek (egész szóként, kis- és nagybetűtől függetlenül)
    for (const auto& n : ctx.names) detail::replaceNoCase(s, n, "<name>", true);
    for (const auto& h : ctx.hosts) detail::replaceNoCase(s, h, "<host>", true);
    for (const auto& u : ctx.users) detail::replaceNoCase(s, u, "<user>", true);
    // 3. címek
    detail::scrubEmails(s);
    detail::scrubMacs(s);
    detail::scrubIpv4(s);
    detail::scrubIpv6(s);
    // 4. fájlútvonalak
    detail::scrubPaths(s, jsonMode);
    detail::collapseRepeats(s);
    return s;
}

/** A gép valódi felhasználóneve, teljes neve, gépneve és otthoni mappája. */
inline ScrubContext systemScrubContext() {
    ScrubContext c;
    auto add = [](std::vector<std::string>& v, const std::string& x) {
        if (x.size() >= 3 && std::find(v.begin(), v.end(), x) == v.end()) v.push_back(x);
    };
    for (const char* e : { "USER", "LOGNAME", "USERNAME" })
        if (const char* v = std::getenv(e); v && *v) add(c.users, v);
    for (const char* e : { "HOME", "USERPROFILE" })
        if (const char* v = std::getenv(e); v && *v) add(c.homes, v);
#ifdef _WIN32
    char host[256] = {};
    DWORD n = sizeof(host);
    if (GetComputerNameA(host, &n)) add(c.hosts, host);
    if (const char* v = std::getenv("COMPUTERNAME"); v && *v) add(c.hosts, v);
#else
    if (const passwd* pw = getpwuid(getuid())) {
        if (pw->pw_name) add(c.users, pw->pw_name);
        if (pw->pw_dir) add(c.homes, pw->pw_dir);
        if (pw->pw_gecos && *pw->pw_gecos) {
            std::string full = pw->pw_gecos;
            full = full.substr(0, full.find(','));
            add(c.users, full);
        }
    }
    char host[256] = {};
    if (gethostname(host, sizeof(host) - 1) == 0 && host[0]) {
        std::string h = host;
        add(c.hosts, h);
        add(c.hosts, h.substr(0, h.find('.')));
    }
#endif
    // a rövid (3 karakternél rövidebb) nevek nem törölhetők biztonságosan: ezeket a hívó látja (a teszt kiírja)
    return c;
}

/** A „name” mezők egy JSON-dokumentumból (remote.json: a párosított eszközök neve). */
inline std::vector<std::string> namesFromJson(const std::string& doc) {
    std::vector<std::string> out;
    ee_analyzer::json::Value v;
    if (doc.empty() || !ee_analyzer::json::parse(doc, v)) return out;
    std::vector<const ee_analyzer::json::Value*> stack{ &v };
    while (!stack.empty()) {
        const auto* x = stack.back();
        stack.pop_back();
        using T = ee_analyzer::json::Value::Type;
        if (x->type == T::Object) {
            for (const auto& [k, y] : x->obj) {
                if (k == "name" && y.type == T::String && !y.str.empty() &&
                    std::find(out.begin(), out.end(), y.str) == out.end())
                    out.push_back(y.str);
                stack.push_back(&y);
            }
        } else if (x->type == T::Array) {
            for (const auto& y : x->arr) stack.push_back(&y);
        }
    }
    return out;
}

/**
 * A napló utolsó n sora. beforeLastBanner: csak az utolsó indulássor előttiek (az előző futás naplója, amikor az
 * új futás már írt a naplóba). A forgatott napló (<napló>.1) elejét is figyelembe veszi.
 */
inline std::vector<std::string> tailLines(const std::filesystem::path& path, int n, bool beforeLastBanner = false) {
    if (path.empty() || n <= 0) return {};
    std::filesystem::path rotated = path;
    rotated += ".1";
    std::string data = detail::readAll(rotated, 256 * 1024) + detail::readAll(path, 1024 * 1024);
    std::vector<std::string> lines = detail::splitLines(data);
    if (beforeLastBanner) {
        for (size_t i = lines.size(); i-- > 0;) {
            if (lines[i].find(kLogBanner) != std::string::npos) { lines.resize(i); break; }
        }
    }
    if (lines.size() > static_cast<size_t>(n)) lines.erase(lines.begin(), lines.end() - n);
    for (auto& l : lines)
        if (l.size() > 400) l = l.substr(0, 400) + "…";
    return lines;
}

struct Report {
    std::string json;      // a küldhető / menthető jelentés (tisztítva)
    std::string preview;   // ugyanez olvasható formában (tisztítva)
    bool hasState = false; // volt-e állapotfájl
};

/**
 * A jelentés összeállítása a diagnosztikai mappából és a naplóból.
 * kind: "manual" (a felhasználó kéri), "previous_abnormal" (az előző futás rendellenes vége), "stall".
 * symptom: a felhasználó választotta tünet kulcsa (2. szakasz), lehet üres.
 */
inline Report build(const std::filesystem::path& dir, const std::filesystem::path& logPath, const ScrubContext& ctx, const std::string& kind,
                    const std::string& symptom, int logLines = 200, bool logBeforeLastBanner = false) {
    using ee_analyzer::json::Value;
    Report r;
    const std::string stateText = detail::readAll(dir / "state.json", 512 * 1024);
    Value state;
    r.hasState = !stateText.empty() && ee_analyzer::json::parse(stateText, state);
    const std::string crash = detail::readAll(dir / "crash.txt", 16 * 1024);
    const std::vector<std::string> log = tailLines(logPath, logLines, logBeforeLastBanner);
    const long long created = static_cast<long long>(std::time(nullptr));

    // JSON
    std::string j = "{\"format\":1,\"kind\":\"" + detail::jsonEsc(kind) + "\",\"symptom\":\"" + detail::jsonEsc(symptom) +
                    "\",\"created_unix\":" + std::to_string(created) + ",\"state\":";
    j += r.hasState ? stateText : std::string("null");
    j += ",\"crash\":\"" + detail::jsonEsc(crash) + "\",\"log\":[";
    for (size_t i = 0; i < log.size(); ++i) j += (i ? ",\"" : "\"") + detail::jsonEsc(log[i]) + "\"";
    j += "]}";
    r.json = scrub(j, ctx, true);

    // Olvasható előnézet
    std::string p = "Aqulify error report\n";
    p += "kind: " + kind + "\n";
    if (!symptom.empty()) p += "symptom: " + symptom + "\n";
    if (r.hasState) {
        auto str = [&](const char* k) { const Value* x = state.get(k); return x ? (x->type == Value::Type::String ? x->str : (x->type == Value::Type::Bool ? std::string(x->b ? "true" : "false") : std::to_string(static_cast<long long>(x->num)))) : std::string("?"); };
        p += "version: " + str("version") + "  platform: " + str("platform") + "  package: " + str("package") + "\n";
        p += "reports enabled: " + str("reports_enabled") + "  clean exit: " + str("clean_exit") + "  uptime s: " + str("uptime_s") + "\n";
        p += "\n[stalls]\n";
        if (const Value* st = state.get("stalls"); st && st->type == Value::Type::Array && !st->arr.empty()) {
            for (const auto& s : st->arr) {
                const Value* t = s.get("thread");
                const Value* l = s.get("label");
                const Value* sec = s.get("seconds");
                const Value* on = s.get("ongoing");
                char b[64];
                std::snprintf(b, sizeof(b), "%.1f s", sec ? sec->numberOr(0.0) : 0.0);
                p += "  " + (t ? t->str : std::string("?")) + " – last: " + (l ? l->str : std::string()) + " – " + b +
                     ((on && on->b) ? " (still stuck)" : "") + "\n";
            }
        } else {
            p += "  none\n";
        }
        p += "\n[threads]\n";
        if (const Value* hb = state.get("heartbeats"); hb && hb->type == Value::Type::Array) {
            for (const auto& h : hb->arr) {
                const Value* t = h.get("thread");
                const Value* l = h.get("label");
                const Value* a = h.get("age_ms");
                p += "  " + (t ? t->str : std::string("?")) + " – " + (l ? l->str : std::string()) + " – " +
                     std::to_string(static_cast<long long>(a ? a->numberOr(-1) : -1)) + " ms\n";
            }
        }
        p += "\n[process]\n";
        if (const Value* pr = state.get("process")) detail::flatten(*pr, "", p);
        p += "\n[engine]\n";
        if (const Value* se = state.get("sections")) detail::flatten(*se, "", p);
        if (const Value* sc = state.get("sound_cards"); sc && sc->type == Value::Type::String && !sc->str.empty())
            p += "\n[sound cards]\n" + sc->str + "\n";
    } else {
        p += "(no state file)\n";
    }
    if (!crash.empty()) p += "\n[crash]\n" + crash + (crash.back() == '\n' ? "" : "\n");
    p += "\n[log: last " + std::to_string(log.size()) + " lines]\n";
    for (const auto& l : log) p += l + "\n";
    r.preview = scrub(p, ctx, false);
    return r;
}

} // namespace aq::report
