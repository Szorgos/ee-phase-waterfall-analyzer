#pragma once
/**
 * @file device_follow.hpp
 * @brief Az alapértelmezett lejátszóeszköz követése (platformfüggetlen döntési logika, a Windows-lejátszó használja).
 *
 * A beépített lejátszó az alapértelmezett lejátszóeszközre szól. Ha a felhasználó bedug egy USB DAC-ot, és a rendszer
 * azt teszi alapértelmezetté, a régi eszköz (pl. a beépített hangkártya) érvényes marad, így magától nem derül ki,
 * hogy át kell váltani: a lejátszó ezért időnként megnézi, mi az alapértelmezett eszköz (observe), és ha az tartósan
 * eltér attól, amelyiken a hangfolyam nyitva van, újranyitást kér.
 *
 * Bedugáskor a rendszer gyakran többször oda-vissza vált (a 2.7.0 egyik hibajelentésében: DAC → beépített → DAC
 * néhány másodpercen belül); a lejátszó csak akkor vált, ha ugyanaz az új eszköz legalább `settle` ideje az
 * alapértelmezett. Minden újabb változás újraindítja a várakozást. Üres azonosító (nincs alapértelmezett eszköz)
 * nem vált ki semmit: ilyenkor a régi hangfolyam marad, amíg az eszköz meg nem szűnik.
 */
#include <chrono>

namespace ee_analyzer::player {

template <class Id>
class DefaultDeviceFollow {
public:
    using Clock = std::chrono::steady_clock;

    explicit DefaultDeviceFollow(std::chrono::milliseconds settle = std::chrono::milliseconds(1500)) : settle_(settle) {}

    /** A hangfolyam megnyílt ezen az eszközön (a függő váltás törlődik). */
    void opened(const Id& id) {
        current_ = id;
        pending_ = Id{};
        hasPending_ = false;
    }

    /** Amelyik eszközön most nyitva van (üres: még semmin). */
    const Id& current() const { return current_; }

    /**
     * A most alapértelmezett eszköz azonosítója a `now` pillanatban. true: váltani kell (az új eszköz a `pending()`);
     * a hívó újranyitja a hangfolyamot, majd az `opened()`-del jelzi, hol nyílt meg.
     */
    bool observe(const Id& defaultId, Clock::time_point now) {
        if (defaultId == Id{} || current_ == Id{} || defaultId == current_) {
            pending_ = Id{};
            hasPending_ = false;
            return false;
        }
        if (!hasPending_ || pending_ != defaultId) {   // új jelölt: innen számít a várakozás
            pending_ = defaultId;
            since_ = now;
            hasPending_ = true;
            return false;
        }
        return now - since_ >= settle_;
    }

    /** A váltás jelöltje (observe() true után ez lesz az új eszköz). */
    const Id& pending() const { return pending_; }

private:
    std::chrono::milliseconds settle_;
    Id current_{};
    Id pending_{};
    bool hasPending_ = false;
    Clock::time_point since_{};
};

}  // namespace ee_analyzer::player
