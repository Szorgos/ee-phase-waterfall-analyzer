/**
 * @file aether_gui_linux.cpp
 * @brief AetherAudio felület saját ablakban Linuxon (WebKitGTK, a webview/webview 0.12.0 könyvtárral).
 *
 * A gui_integrated_system.html-t keresi (sorrendben): $AETHERAUDIO_GUI_DIR, <bináris>/../share/aetheraudio/gui,
 * ~/.local/share/aetheraudio/gui, <bináris> mappája. Ha a WebKitGTK ablak nem indul, az alapértelmezett
 * böngészőben nyitja meg (xdg-open).
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

int openInBrowser(const std::string& url) {
    const std::string cmd = "xdg-open '" + url + "' >/dev/null 2>&1 &";
    return std::system(cmd.c_str()) == 0 ? 0 : 1;
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
        if (auto win = w.window(); win.ok())
            gtk_window_set_icon_name(GTK_WINDOW(win.value()), "aetheraudio");
        w.navigate(url);
        w.run();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[Info] A saját ablak nem indult (" << e.what() << "), megnyitás böngészőben.\n";
        return openInBrowser(url);
    }
}
