#pragma once
/**
 * @file play_stats.hpp
 * @brief A 2.x lejátszási naplója (az okos listákhoz): számonként a lejátszások száma és az utolsó lejátszás ideje
 *        (stats.json, csak a gépen; a hálózatra nem megy ki, csak az okos listák eredménye).
 *
 * Egy lejátszás akkor számít, ha a szám legalább 30 másodpercig, illetve a hossza feléig szólt (a kettő közül a
 * rövidebbig): a ListenTracker a ténylegesen hallott időt méri, az előre tekerés nem számít bele, az újrakezdés (az
 * „előző”, az egy szám ismétlése, visszatekerés a szám elejére) új lejátszás.
 */
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ee_analyzer::player {

struct TrackStats {
    uint32_t plays = 0;
    int64_t last = 0;      // az utolsó lejátszás Unix-ideje (s); 0: még nem szólt
};

class PlayStats {
public:
    using Save = std::function<void(const std::string& doc)>;
    explicit PlayStats(Save save = nullptr);

    /** A mentett napló betöltése (mentés nélkül). false: hibás dokumentum (a napló üres marad). */
    bool load(const std::string& doc);
    std::string doc() const;
    /** Egy lejátszás feljegyzése (when: Unix-idő, 0: most), utána mentés. */
    void record(const std::string& id, int64_t when = 0);
    TrackStats get(const std::string& id) const;
    std::unordered_map<std::string, TrackStats> snapshot() const;
    /** A feljegyzések száma (a kliens ebből látja, hogy az okos listák tartalma változhatott). */
    uint64_t rev() const;

private:
    std::string docLocked() const;

    Save save_;
    mutable std::mutex m_;
    std::unordered_map<std::string, TrackStats> stats_;
    uint64_t rev_ = 0;
};

/** A hallott idő mérése egy számra (tiszta logika, a tesztekhez is); a sor 0,25 s-onként hívja. */
class ListenTracker {
public:
    /** Ennyi hallott idő után számít egy lejátszás (rövidebb számnál a hossz fele). */
    static constexpr double kCountSeconds = 30.0;
    /**
     * token: a hallott tétel jele (0: nincs, vagy nem a sorból), position / duration: s, playing: szól-e, dt: az előző
     * hívás óta eltelt idő (s). true: ezzel a hívással vált a lejátszás feljegyzendővé (lejátszásonként egyszer).
     */
    bool update(uint64_t token, double position, double duration, bool playing, double dt);

private:
    uint64_t token_ = 0;
    double lastPos_ = 0.0, heard_ = 0.0;
    bool counted_ = false;
};

} // namespace ee_analyzer::player
