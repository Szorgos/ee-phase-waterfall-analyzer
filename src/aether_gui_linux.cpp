/**
 * @file aether_gui_linux.cpp
 * @brief AetherAudio felület saját ablakban Linuxon (WebKitGTK, a webview/webview 0.12.0 könyvtárral).
 *
 * A gui_integrated_system.html-t keresi (sorrendben): $AETHERAUDIO_GUI_DIR, <bináris>/../share/aetheraudio/gui,
 * ~/.local/share/aetheraudio/gui, <bináris> mappája. Ha a WebKitGTK ablak nem indul, Chromium alapú
 * böngésző alkalmazásablakában (--app), végső esetben az alapértelmezett böngészőben nyitja meg (xdg-open).
 */
#include "webview/webview.h"

#include <climits>
#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

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
    if (const char* env = std::getenv("AETHERAUDIO_GUI_DIR"); env && *env) candidates[k++] = env;
    const std::string dir = exeDir();
    candidates[k++] = dir + "/../share/aetheraudio/gui";
    if (const char* home = std::getenv("HOME"); home && *home) candidates[k++] = std::string(home) + "/.local/share/aetheraudio/gui";
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
        const std::string cmd = std::string(b) + " --app='" + url + "' --class=AetherAudio --window-size=1480,940 >/dev/null 2>&1 &";
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

} // namespace

int main() {
    const std::string folder = findGuiFolder();
    if (folder.empty()) {
        std::cerr << "[Hiba] A gui_integrated_system.html nem található (telepítsd újra: ./install.sh).\n";
        return 1;
    }
    char real[PATH_MAX];
    const std::string abs = realpath(folder.c_str(), real) ? std::string(real) : folder;
    const std::string url = "file://" + abs + "/gui_integrated_system.html";

    try {
        webview::webview w(false, nullptr);
        w.set_title("AetherAudio");
        w.set_size(1480, 940, WEBVIEW_HINT_NONE);
        w.set_size(1000, 640, WEBVIEW_HINT_MIN);
        if (auto win = w.window(); win.ok()) {
            GtkSettings* settings = gtk_settings_get_default();
            if (settings) {
                g_object_set(settings, "gtk-application-prefer-dark-theme", TRUE, nullptr);
            }
            gtk_window_set_icon_name(GTK_WINDOW(win.value()), "aetheraudio");
            std::string iconFile = folder + "/../../icons/hicolor/256x256/apps/aetheraudio.png";
            if (fileExists(iconFile)) {
                gtk_window_set_icon_from_file(GTK_WINDOW(win.value()), iconFile.c_str(), nullptr);
            } else {
                std::string iconLocal = folder + "/../linux/aetheraudio.png";
                if (fileExists(iconLocal)) gtk_window_set_icon_from_file(GTK_WINDOW(win.value()), iconLocal.c_str(), nullptr);
            }
        }
        w.bind("aetherOpenUrl", [](const std::string& req) -> std::string {
            const std::string target = allowedExternalUrl(req);
            if (!target.empty()) g_app_info_launch_default_for_uri(target.c_str(), nullptr, nullptr);
            return "null";
        });
        w.navigate(url);
        w.run();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[Info] A saját ablak nem indult (" << e.what() << "), megnyitás böngészőben.\n";
        return openInBrowser(url);
    }
}
