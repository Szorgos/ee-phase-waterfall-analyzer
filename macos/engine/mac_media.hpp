#pragma once
/**
 * @file mac_media.hpp
 * @brief „Most szól” és lejátszásvezérlés macOS-en (a rendszer MediaRemote szolgáltatása, amit a Vezérlőközpont
 *        „Most szól” panelje és a billentyűzet médiagombjai is használnak).
 *
 * A MediaRemote nem nyilvános keretrendszer: futás közben töltjük be, és ha egy függvénye hiányzik, a szolgáltatás
 * egyszerűen nem elérhető. Újabb macOS-en (15.4-től) a rendszer a „most szól” adatokat csak a saját programjainak
 * adja ki; ilyenkor a felület nem mutat lejátszót (a vezérlőgombok sem jelennek meg).
 */
#include <memory>
#include <string>

#include "media_session.hpp"

namespace ee_analyzer::media {

class MacMediaSession {
public:
    MacMediaSession();
    ~MacMediaSession();
    MacMediaSession(const MacMediaSession&) = delete;
    MacMediaSession& operator=(const MacMediaSession&) = delete;

    void start();
    void stop();
    Info snapshot() const;
    std::string json() const { return toJson(snapshot()); }
    void command(const std::string& action, double value);
    bool art(const std::string& id, std::string& mime, std::string& bytes, std::string& url) const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace ee_analyzer::media
