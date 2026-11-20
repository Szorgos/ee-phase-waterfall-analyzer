#pragma once
/**
 * @file headless_reset.hpp
 * @brief Képernyő nélküli (headless) Raspberry Pi boot-partíciós jelzőfájlok kezelése:
 *        - `aqulify-reset`: az összes párosítás törlése és visszatérés gazdátlan (claimable) állapotba;
 *        - `aqulify-claim-no-audio`: fizikai hozzáférés igazolása hangkimenet nélküli első beállításhoz.
 *
 * Útvonalak: `/boot/firmware/` (újabb Raspberry Pi OS) és `/boot/` (régebbi lemezképek).
 */

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "mini_json.hpp"

namespace ee_analyzer::headless {

inline constexpr const char* kResetFlagName = "aqulify-reset";
inline constexpr const char* kNoAudioFlagName = "aqulify-claim-no-audio";

/** Alapértelmezett boot-partíció útvonalak (tesztben AQULIFY_BOOT_DIR felülírhatja). */
inline std::vector<std::string> defaultBootDirs() {
    if (const char* env = std::getenv("AQULIFY_BOOT_DIR"); env && env[0] != '\0') {
        return { std::string(env) };
    }
#if defined(__linux__)
    return { "/boot/firmware", "/boot" };
#else
    return {};
#endif
}

/** Megkeresi a megadott jelzőfájlt a boot-könyvtárakban; üres string, ha nem létezik. */
inline std::string findFlagFile(const std::vector<std::string>& bootDirs, const char* fileName) {
    for (const auto& dir : bootDirs) {
        if (dir.empty()) continue;
        std::error_code ec;
        std::filesystem::path p = std::filesystem::path(dir) / fileName;
        if (std::filesystem::exists(p, ec) && !ec && std::filesystem::is_regular_file(p, ec) && !ec) {
            return p.string();
        }
    }
    return {};
}

/** Igaz, ha a felhasználó fizikai hozzáféréssel létrehozta az `aqulify-claim-no-audio` fájlt. */
inline bool hasNoAudioClaimFlag(const std::vector<std::string>& bootDirs) {
    return !findFlagFile(bootDirs, kNoAudioFlagName).empty();
}

/** Az `aqulify-claim-no-audio` fájl eltávolítása sikeres első beállítás után (legjobb próbálkozás). */
inline bool removeNoAudioClaimFlag(const std::vector<std::string>& bootDirs, std::string* errOut = nullptr) {
    const std::string p = findFlagFile(bootDirs, kNoAudioFlagName);
    if (p.empty()) return true;
    errno = 0;
    if (std::remove(p.c_str()) == 0) return true;
    const int err = errno;
    if (errOut) {
        *errOut = "Az aqulify-claim-no-audio fájl nem törölhető (" + p + "): " + std::strerror(err);
    }
    return false;
}

/**
 * Párosítások törlése egy remote.json tartalomból úgy, hogy a machine/port/bluetooth beállítás megmarad,
 * az `enabled` pedig true lesz (gazdátlan, hálózaton megtalálható állapot).
 */
inline std::string clearDevicesInRemoteJson(const std::string& existingDoc) {
    std::string machine;
    int port = 47820;
    bool bt = true;
    json::Value v;
    if (!existingDoc.empty() && json::parse(existingDoc, v) && v.type == json::Value::Type::Object) {
        if (const json::Value* m = v.get("machine"); m && m->type == json::Value::Type::String) {
            machine = m->str;
        }
        if (const json::Value* p = v.get("port")) {
            const int pv = static_cast<int>(p->numberOr(47820.0));
            if (pv >= 1024 && pv <= 65500) port = pv;
        }
        if (const json::Value* b = v.get("bluetooth")) {
            bt = b->numberOr(1.0) != 0.0;
        }
    }
    std::string out = "{\"enabled\":true,\"bluetooth\":";
    out += bt ? "true" : "false";
    out += ",\"port\":" + std::to_string(port);
    if (!machine.empty()) out += ",\"machine\":\"" + machine + "\"";
    out += ",\"devices\":[]}";
    return out;
}

struct ResetOutcome {
    bool triggered = false;      // létezett az aqulify-reset fájl
    bool configCleared = false;  // a remote.json párosításait sikerült törölni
    bool flagRemoved = false;    // az aqulify-reset fájlt sikerült törölni
    std::string flagPath;        // a megtalált reset fájl teljes útvonala
    std::string error;           // emberi hibaüzenet (pl. írásvédett boot partíció)
};

/**
 * Ellenőrzi az `aqulify-reset` fájlt, törli a párosításokat a `remoteJsonPath` fájlból,
 * és **akkor is** megpróbálja törölni a reset fájlt, ha a párosítások törlése hibázott.
 */
inline ResetOutcome applyBootReset(const std::vector<std::string>& bootDirs, const std::string& remoteJsonPath) {
    ResetOutcome res;
    res.flagPath = findFlagFile(bootDirs, kResetFlagName);
    if (res.flagPath.empty()) return res;
    res.triggered = true;

    std::string cfgErr;
    try {
        std::string oldDoc;
        {
            std::ifstream in(remoteJsonPath, std::ios::binary);
            if (in) oldDoc.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        const std::string newDoc = clearDevicesInRemoteJson(oldDoc);

        std::filesystem::path target(remoteJsonPath);
        std::error_code ec;
        if (target.has_parent_path()) {
            std::filesystem::create_directories(target.parent_path(), ec);
        }
        const std::string tmpPath = remoteJsonPath + ".reset.tmp";
        {
            std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
            if (!out) {
                cfgErr = "A remote.json nem írható (" + remoteJsonPath + ").";
            } else {
                out.write(newDoc.data(), static_cast<std::streamsize>(newDoc.size()));
                out.flush();
                if (!out.good()) {
                    cfgErr = "Írási hiba a remote.json frissítésekor (" + remoteJsonPath + ").";
                }
            }
        }
        if (cfgErr.empty()) {
#ifndef _WIN32
            chmod(tmpPath.c_str(), 0600);
#endif
            std::filesystem::rename(tmpPath, target, ec);
            if (ec) {
                std::remove(tmpPath.c_str());
                cfgErr = "A remote.json cseréje sikertelen (" + remoteJsonPath + "): " + ec.message();
            } else {
#ifndef _WIN32
                chmod(remoteJsonPath.c_str(), 0600);
#endif
                res.configCleared = true;
            }
        }
    } catch (const std::exception& ex) {
        cfgErr = std::string("Kivétel a remote.json törlésekor: ") + ex.what();
    }

    // A reset fájlt akkor is töröljük, ha a remote.json törlése hibázott, nehogy végtelen reset-ciklus legyen.
    errno = 0;
    if (std::remove(res.flagPath.c_str()) == 0) {
        res.flagRemoved = true;
    } else {
        const int err = errno;
        std::string flagErr;
        if (err == EROFS) {
            flagErr = "A boot partíció írásvédett, az aqulify-reset fájl nem törölhető (" + res.flagPath +
                      "). Távolítsd el kézzel az SD-kártyáról, különben a következő indításkor is törlődnek a párosítások.";
        } else if (err == EACCES || err == EPERM) {
            flagErr = "Nincs írási jogosultság az aqulify-reset fájl törléséhez (" + res.flagPath +
                      "). Ellenőrizd az aqulify-reset szolgáltatást vagy töröld a fájlt az SD-kártyáról.";
        } else {
            flagErr = "Az aqulify-reset fájl nem törölhető (" + res.flagPath + "): " + std::strerror(err);
        }
        res.error = cfgErr.empty() ? flagErr : (cfgErr + " | " + flagErr);
    }

    if (res.error.empty() && !cfgErr.empty()) {
        res.error = cfgErr;
    }
    return res;
}

} // namespace ee_analyzer::headless
