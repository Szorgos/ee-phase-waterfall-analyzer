/**
 * @file aether_gui_linux.cpp
 * @brief Aqulify felület saját ablakban Linuxon (WebKitGTK, a webview/webview 0.12.0 könyvtárral).
 *
 * A gui_integrated_system.html-t keresi (sorrendben): $AQULIFY_GUI_DIR, <bináris>/../share/aqulify/gui,
 * ~/.local/share/aqulify/gui, <bináris> mappája. Ha a WebKitGTK ablak nem indul, Chromium alapú
 * böngésző alkalmazásablakában (--app), végső esetben az alapértelmezett böngészőben nyitja meg (xdg-open).
 */
#include "webview/webview.h"

#include <glib-unix.h>

#include <arpa/inet.h>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <spawn.h>

#include "aq_i18n.hpp"

extern char** environ;

/**
 * Ébren tartás (burn-in): a systemd-inhibit alvás-zárat tart, amíg a gyerekfolyamata fut. Csak az alvást tiltja,
 * a kijelző elalhat. A gyerek a `tail --pid=<felület>` : a felület kilépésekor (összeomláskor is) magától leáll,
 * így a zár nem marad meg. posix_spawn (nem fork + exec, ami többszálú GTK-programban nem biztonságos); a saját
 * folyamatcsoportot a spawn állítja be, így a leállítás nem versenyez vele.
 */
static pid_t g_keepAwakePid = -1;
static void setKeepAwakeLinux(bool on) {
    if (on && g_keepAwakePid < 0) {
        const std::string pidArg = "--pid=" + std::to_string(getpid());
        const char* argv[] = { "systemd-inhibit", "--what=sleep", "--who=Aqulify", "--why=Burn-in",
                               "tail", pidArg.c_str(), "-f", "/dev/null", nullptr };
        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
        posix_spawnattr_setpgroup(&attr, 0);
        pid_t pid = -1;
        if (posix_spawnp(&pid, "systemd-inhibit", nullptr, &attr, const_cast<char* const*>(argv), environ) == 0)
            g_keepAwakePid = pid;   // nem systemd-s rendszeren a spawn sikertelen: nincs ébren tartás
        posix_spawnattr_destroy(&attr);
    } else if (!on && g_keepAwakePid > 0) {
        kill(-g_keepAwakePid, SIGTERM);
        waitpid(g_keepAwakePid, nullptr, 0);
        g_keepAwakePid = -1;
    }
}

#ifndef AQULIFY_VERSION
#define AQULIFY_VERSION "0.0.0"
#endif

namespace {

bool fileExists(const std::string& p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string exeDir() {
    char buf[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return ".";
    std::string p(buf, static_cast<size_t>(n));
    const size_t sl = p.find_last_of('/');
    return sl == std::string::npos ? "." : p.substr(0, sl);
}

std::string findGuiFolder() {
    std::string candidates[5];
    int k = 0;
    if (const char* env = std::getenv("AQULIFY_GUI_DIR"); env && *env) candidates[k++] = env;
    const std::string dir = exeDir();
    candidates[k++] = dir + "/../share/aqulify/gui";
    if (const char* home = std::getenv("HOME"); home && *home) candidates[k++] = std::string(home) + "/.local/share/aqulify/gui";
    candidates[k++] = dir;
    candidates[k++] = dir + "/..";
    for (int i = 0; i < k; ++i)
        if (fileExists(candidates[i] + "/gui_integrated_system.html")) return candidates[i];
    return {};
}

/** Program keresése a PATH-ban. */
bool inPath(const std::string& prog) {
    const char* path = std::getenv("PATH");
    std::string p = path ? path : "/usr/local/bin:/usr/bin:/bin";
    size_t start = 0;
    while (start <= p.size()) {
        const size_t end = p.find(':', start);
        const std::string dir = p.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty() && access((dir + "/" + prog).c_str(), X_OK) == 0) return true;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

/**
 * Tartalék WebKitGTK nélkül: saját alkalmazásablak Chromium alapú böngészővel (--app: nincsenek fülek és
 * címsor); csak ha ilyen nincs telepítve, akkor nyílik meg az alapértelmezett böngészőben (xdg-open).
 */
int openInBrowser(const std::string& url) {
    for (const char* b : { "chromium", "chromium-browser", "google-chrome", "google-chrome-stable",
                           "brave-browser", "microsoft-edge", "vivaldi" }) {
        if (!inPath(b)) continue;
        const std::string cmd = std::string(b) + " --app='" + url + "' --class=Aqulify --window-size=1480,940 >/dev/null 2>&1 &";
        if (std::system(cmd.c_str()) == 0) return 0;
    }
    const std::string cmd = "xdg-open '" + url + "' >/dev/null 2>&1 &";
    return std::system(cmd.c_str()) == 0 ? 0 : 1;
}


/** JS-ből hívható külső link (pl. támogatás): csak engedélyezett https címeket nyit meg a rendszer böngészőjében. */
std::string allowedExternalUrl(const std::string& req) {
    // req: JSON tömb, pl. ["https://revolut.me/gergoqhg6"]
    const size_t a = req.find('"');
    const size_t b = a == std::string::npos ? std::string::npos : req.find('"', a + 1);
    if (b == std::string::npos) return {};
    const std::string url = req.substr(a + 1, b - a - 1);
    for (const char* ok : { "https://revolut.me/", "https://github.com/Szorgos/" })
        if (url.rfind(ok, 0) == 0 && url.find_first_of("'\\ \"<>`$;&|") == std::string::npos) return url;
    return {};
}


/** Fut-e már a motor (figyel-e valami a WebSocket-portok valamelyikén a 127.0.0.1-en)? */
bool engineReachable() {
    for (int port : { 47810, 47811, 47812, 47813, 47814, 8080 }) {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) continue;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<uint16_t>(port));
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const bool ok = connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
        close(fd);
        if (ok) return true;
    }
    return false;
}

/**
 * A felület indításakor a motort is elindítja, ha nem fut (pl. közvetlenül a csomag telepítése után,
 * amikor a systemd felhasználói szolgáltatás csak a következő bejelentkezéskor indulna magától).
 */
void ensureEngineRunning() {
    if (engineReachable()) return;
    if (std::system("systemctl --user start aqulify.service >/dev/null 2>&1") != 0) {
        // Nincs systemd felhasználói szolgáltatás: a motor közvetlen indítása a háttérben
        std::string engine = exeDir() + "/aqulify-engine";
        if (access(engine.c_str(), X_OK) != 0) engine = "aqulify-engine";
        const pid_t pid = fork();
        if (pid == 0) {
            setsid();
            if (fork() == 0) {
                execlp(engine.c_str(), engine.c_str(), static_cast<char*>(nullptr));
            }
            _exit(0);
        }
        if (pid > 0) waitpid(pid, nullptr, 0);
    }
    for (int i = 0; i < 30 && !engineReachable(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

/** A GTK címsor (kliensoldali dekoráció) a felület témájának színeit kapja (a felület szól, ha téma vált). */
void applyTitlebar(bool light) {
    static GtkCssProvider* css = nullptr;
    if (!css) {
        css = gtk_css_provider_new();
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
                                                  GTK_STYLE_PROVIDER_PRIORITY_USER);
    }
    const char* bg = light ? "#E9EEF4" : "#0A0F15";
    const char* fg = light ? "#0F172A" : "#E8EEF4";
    const char* line = light ? "#D8E0E9" : "#1F2C3A";
    const char* btn = light ? "#475569" : "#A3B3C2";
    const char* hover = light ? "rgba(0,0,0,0.06)" : "rgba(255,255,255,0.06)";
    const std::string rules =
        std::string("headerbar, .titlebar, .titlebar:backdrop, headerbar:backdrop {") +
        "  background: " + bg + "; background-image: none; color: " + fg + ";" +
        "  border-bottom: 1px solid " + line + "; box-shadow: none; }" +
        "headerbar .title, .titlebar .title { color: " + fg + "; }" +
        "headerbar button, .titlebar button { color: " + btn + "; background: transparent; border: none; box-shadow: none; }" +
        "headerbar button:hover, .titlebar button:hover { background: " + hover + "; color: " + fg + "; }" +
        "decoration { box-shadow: 0 3px 12px rgba(0,0,0," + (light ? "0.18" : "0.5") + "); }";
    gtk_css_provider_load_from_data(css, rules.c_str(), -1, nullptr);
}

/**
 * Bezáráskor a WebKit tartalomfolyamatát (WebKitWebProcess) is leállítjuk. Ha a felület kilépésekor éppen
 * egy hosszabb rajzolást végez, a WebKit 10 másodperc múlva szándékosan összeomlasztaná (g_error, core dump,
 * hibajelentés); így ehelyett azonnal és csendben leáll.
 */
gboolean onWindowDelete(GtkWidget*, GdkEvent*, gpointer view) {
#if WEBKIT_CHECK_VERSION(2, 34, 0)
    if (view) webkit_web_view_terminate_web_process(WEBKIT_WEB_VIEW(view));
#else
    (void)view;
#endif
    return FALSE;   // a bezárás folytatódik (az ablak megszűnik, a program kilép)
}

/** SIGTERM / SIGINT: ugyanaz, mint az ablak bezárása. */
gboolean onQuitSignal(gpointer window) {
    gtk_window_close(GTK_WINDOW(window));
    return G_SOURCE_REMOVE;
}

} // namespace

int main() {
    const std::string folder = findGuiFolder();
    if (folder.empty()) {
        aq_i18n::init();
        std::cerr << aq_i18n::tr(aq_i18n::Msg::linux_guiMissing) << "\n";
        return 1;
    }
    char real[PATH_MAX];
    const std::string abs = realpath(folder.c_str(), real) ? std::string(real) : folder;
    const std::string url = "file://" + abs + "/gui_integrated_system.html";

    ensureEngineRunning();

    try {
        webview::webview w(false, nullptr);
        w.set_title("Aqulify " AQULIFY_VERSION);   // az ablak címsora a verziószámmal
        w.set_size(1480, 940, WEBVIEW_HINT_NONE);
        w.set_size(1000, 640, WEBVIEW_HINT_MIN);
        if (auto win = w.window(); win.ok()) {
            GtkSettings* settings = gtk_settings_get_default();
            if (settings) {
                g_object_set(settings, "gtk-application-prefer-dark-theme", TRUE, nullptr);
            }
            applyTitlebar(false);   // a felület betöltése után a saját témája szerint (aetherSetTheme)
            gtk_window_set_icon_name(GTK_WINDOW(win.value()), "aqulify");
            std::string iconFile = folder + "/../../icons/hicolor/256x256/apps/aqulify.png";
            if (fileExists(iconFile)) {
                gtk_window_set_icon_from_file(GTK_WINDOW(win.value()), iconFile.c_str(), nullptr);
            } else {
                std::string iconLocal = folder + "/../linux/aqulify.png";
                if (fileExists(iconLocal)) gtk_window_set_icon_from_file(GTK_WINDOW(win.value()), iconLocal.c_str(), nullptr);
            }
            auto view = w.browser_controller();
            if (view.ok()) {
                // betöltés előtt is a felület háttérszíne (nincs fehér villanás)
                const GdkRGBA bg{ 10 / 255.0, 15 / 255.0, 21 / 255.0, 1.0 };
                webkit_web_view_set_background_color(WEBKIT_WEB_VIEW(view.value()), &bg);
            }
            g_signal_connect(G_OBJECT(win.value()), "delete-event", G_CALLBACK(onWindowDelete),
                             view.ok() ? view.value() : nullptr);
            g_unix_signal_add(SIGTERM, onQuitSignal, win.value());
            g_unix_signal_add(SIGINT, onQuitSignal, win.value());
        }
        // A felület témája (req: ["light"] vagy ["dark"]): a címsor ugyanazt a színt kapja
        w.bind("aetherSetTheme", [](const std::string& req) -> std::string {
            applyTitlebar(req.find("\"light\"") != std::string::npos);
            return "null";
        });
        w.bind("aetherOpenUrl", [](const std::string& req) -> std::string {
            const std::string target = allowedExternalUrl(req);
            if (!target.empty()) g_app_info_launch_default_for_uri(target.c_str(), nullptr, nullptr);
            return "null";
        });
        // Burn-in alatt nincs alvás: a systemd-inhibit zárat tart, amíg a gyerekfolyamat (sleep infinity) fut.
        // Nem systemd-s rendszeren a hívás nem tesz semmit (az exec sikertelen).
        w.bind("aetherKeepAwake", [](const std::string& req) -> std::string {
            setKeepAwakeLinux(req.find("true") != std::string::npos);
            return "null";
        });
        w.navigate(url);
        w.run();
        return 0;
    } catch (const std::exception& e) {
        aq_i18n::init();
        std::cerr << aq_i18n::fmt(aq_i18n::Msg::linux_browserFallback, { { "err", e.what() } }) << "\n";
        return openInBrowser(url);
    }
}
