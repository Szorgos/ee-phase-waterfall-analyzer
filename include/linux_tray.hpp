#pragma once
/**
 * @file linux_tray.hpp
 * @brief Tálcaikon Linuxon: StatusNotifierItem (a KDE/Plasma tálca natívan, a GNOME-on az AppIndicator-kiterjesztés)
 *        és a hozzá tartozó DBusMenu („Megnyitás”, „Kilépés”), sd-bus-szal (libsystemd).
 *
 * A tálca a motorhoz tartozik: a motor ablak nélkül is fut, ezért a tálcaikon is ezt jelzi. A háttérszál kezeli a
 * D-Bus-üzeneteket; a műveletek (megnyitás, kilépés) a hívó callbackjei, amelyek szálbiztosak kell legyenek.
 * Ha nincs session-busz, vagy nincs StatusNotifierWatcher (tálcagazda), a tálca nem jelenik meg; a motor ettől
 * még fut. A regisztrációt addig ismétli, amíg a tálcagazda meg nem jelenik (pl. a bejelentkezés után később indul).
 */
#if defined(AQ_HAVE_SYSTEMD)

#include <systemd/sd-bus.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>

namespace ee_analyzer {

class LinuxTray {
public:
    struct Labels { std::string open, quit; };
    struct Callbacks { std::function<void()> open, quit; };

    ~LinuxTray() { stop(); }

    /** Elindítja a tálcaelemet. false: nincs session-busz vagy nem sikerült az exportálás (ilyenkor nincs tálca). */
    bool start(Labels labels, Callbacks cb) {
        if (running_) return true;
        labels_ = std::move(labels);
        cb_ = std::move(cb);
        if (sd_bus_open_user(&bus_) < 0) { bus_ = nullptr; return false; }
        service_ = std::string("org.kde.StatusNotifierItem-") + std::to_string(static_cast<long>(getpid())) + "-1";
        sd_bus_set_method_call_timeout(bus_, 3000000);   // a tálcagazda nem válaszol: 3 s, ne 25 s
        if (sd_bus_add_object_vtable(bus_, nullptr, kItemPath, kItemIface, itemVtable(), this) < 0 ||
            sd_bus_add_object_vtable(bus_, nullptr, kMenuPath, kMenuIface, menuVtable(), this) < 0 ||
            sd_bus_request_name(bus_, service_.c_str(), 0) < 0) {
            sd_bus_flush_close_unref(bus_);
            bus_ = nullptr;
            return false;
        }
        running_ = true;
        thread_ = std::thread([this] { loop(); });
        return true;
    }

    /** Leállítja a háttérszálat és bontja a kapcsolatot. */
    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        if (bus_) { sd_bus_flush_close_unref(bus_); bus_ = nullptr; }
        registered_ = false;
    }

    /** Be van-e jegyezve a tálcagazdánál (a felület ettől tudja, lehet-e háttérbe zárni). */
    bool registered() const { return registered_.load(); }

private:
    static constexpr const char* kItemPath = "/StatusNotifierItem";
    static constexpr const char* kItemIface = "org.kde.StatusNotifierItem";
    static constexpr const char* kMenuPath = "/MenuBar";
    static constexpr const char* kMenuIface = "com.canonical.dbusmenu";
    static constexpr const char* kWatcherName = "org.kde.StatusNotifierWatcher";
    static constexpr const char* kWatcherPath = "/StatusNotifierWatcher";
    static constexpr const char* kWatcherIface = "org.kde.StatusNotifierWatcher";
    static constexpr int kIdOpen = 1, kIdQuit = 2;

    static LinuxTray* self(void* ud) { return static_cast<LinuxTray*>(ud); }

    void loop() {
        auto nextTry = std::chrono::steady_clock::now();
        while (running_) {
            if (!registered_ && std::chrono::steady_clock::now() >= nextTry) {
                registered_ = registerItem();
                nextTry = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            }
            const int r = sd_bus_process(bus_, nullptr);
            if (r < 0) break;
            if (r > 0) continue;
            sd_bus_wait(bus_, 250000);   // 250 ms: a leállítást és az újrapróbálkozást ennyi késleltetéssel látja
        }
    }

    bool registerItem() {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        sd_bus_message* reply = nullptr;
        const int r = sd_bus_call_method(bus_, kWatcherName, kWatcherPath, kWatcherIface, "RegisterStatusNotifierItem",
                                         &err, &reply, "s", service_.c_str());
        sd_bus_message_unref(reply);
        sd_bus_error_free(&err);
        return r >= 0;
    }

    void dispatch(int32_t id) {
        if (id == kIdOpen && cb_.open) cb_.open();
        else if (id == kIdQuit && cb_.quit) cb_.quit();
    }

    // ---- StatusNotifierItem tulajdonságai (állandóak)
    static int getCategory(sd_bus*, const char*, const char*, const char*, sd_bus_message* r, void*, sd_bus_error*) {
        return sd_bus_message_append(r, "s", "ApplicationStatus");
    }
    static int getId(sd_bus*, const char*, const char*, const char*, sd_bus_message* r, void*, sd_bus_error*) {
        return sd_bus_message_append(r, "s", "aqulify");
    }
    static int getTitle(sd_bus*, const char*, const char*, const char*, sd_bus_message* r, void*, sd_bus_error*) {
        return sd_bus_message_append(r, "s", "Aqulify");
    }
    static int getStatus(sd_bus*, const char*, const char*, const char*, sd_bus_message* r, void*, sd_bus_error*) {
        return sd_bus_message_append(r, "s", "Active");
    }
    static int getIconName(sd_bus*, const char*, const char*, const char*, sd_bus_message* r, void*, sd_bus_error*) {
        return sd_bus_message_append(r, "s", "aqulify");
    }
    static int getItemIsMenu(sd_bus*, const char*, const char*, const char*, sd_bus_message* r, void*, sd_bus_error*) {
        return sd_bus_message_append(r, "b", 0);
    }
    static int getMenu(sd_bus*, const char*, const char*, const char*, sd_bus_message* r, void*, sd_bus_error*) {
        return sd_bus_message_append(r, "o", kMenuPath);
    }

    // ---- StatusNotifierItem metódusok: a bal kattintás (Activate) megnyitja a felületet; a többi nem tesz semmit
    static int onActivate(sd_bus_message* m, void* ud, sd_bus_error*) {
        self(ud)->dispatch(kIdOpen);
        return sd_bus_reply_method_return(m, "");
    }
    static int onNoop(sd_bus_message* m, void*, sd_bus_error*) {
        return sd_bus_reply_method_return(m, "");
    }

    // ---- DBusMenu tulajdonságok
    static int getMenuVersion(sd_bus*, const char*, const char*, const char*, sd_bus_message* r, void*, sd_bus_error*) {
        return sd_bus_message_append(r, "u", 3u);
    }
    static int getMenuTextDirection(sd_bus*, const char*, const char*, const char*, sd_bus_message* r, void*, sd_bus_error*) {
        return sd_bus_message_append(r, "s", "ltr");
    }
    static int getMenuStatus(sd_bus*, const char*, const char*, const char*, sd_bus_message* r, void*, sd_bus_error*) {
        return sd_bus_message_append(r, "s", "normal");
    }

    /** Egy menüelem: v(ia{sv}av) – azonosító és címke. */
    static int appendItem(sd_bus_message* m, int32_t id, const std::string& label) {
        int r = sd_bus_message_open_container(m, 'v', "(ia{sv}av)");
        if (r < 0) return r;
        r = sd_bus_message_open_container(m, 'r', "ia{sv}av");
        if (r < 0) return r;
        r = sd_bus_message_append(m, "i", id);
        if (r < 0) return r;
        r = sd_bus_message_open_container(m, 'a', "{sv}");
        if (r < 0) return r;
        r = sd_bus_message_append(m, "{sv}", "label", "s", label.c_str());
        if (r < 0) return r;
        r = sd_bus_message_close_container(m);   // a{sv}
        if (r < 0) return r;
        r = sd_bus_message_open_container(m, 'a', "v");   // nincs almenü
        if (r < 0) return r;
        r = sd_bus_message_close_container(m);
        if (r < 0) return r;
        r = sd_bus_message_close_container(m);   // r
        if (r < 0) return r;
        return sd_bus_message_close_container(m);   // v
    }

    /** GetLayout(parentId, depth, propertyNames) -> (revision, layout): a gyökér két gyermekkel. */
    static int onGetLayout(sd_bus_message* m, void* ud, sd_bus_error*) {
        LinuxTray* t = self(ud);
        sd_bus_message* reply = nullptr;
        int r = sd_bus_message_new_method_return(m, &reply);
        if (r < 0) return r;
        r = sd_bus_message_append(reply, "u", 1u);
        if (r >= 0) r = sd_bus_message_open_container(reply, 'r', "ia{sv}av");
        if (r >= 0) r = sd_bus_message_append(reply, "i", 0);
        if (r >= 0) r = sd_bus_message_open_container(reply, 'a', "{sv}");
        if (r >= 0) r = sd_bus_message_close_container(reply);
        if (r >= 0) r = sd_bus_message_open_container(reply, 'a', "v");
        if (r >= 0) r = appendItem(reply, kIdOpen, t->labels_.open);
        if (r >= 0) r = appendItem(reply, kIdQuit, t->labels_.quit);
        if (r >= 0) r = sd_bus_message_close_container(reply);   // av
        if (r >= 0) r = sd_bus_message_close_container(reply);   // r
        if (r >= 0) r = sd_bus_send(sd_bus_message_get_bus(m), reply, nullptr);
        sd_bus_message_unref(reply);
        return r < 0 ? r : 1;
    }

    /** Event(id, eventId, data, timestamp): a „clicked” esemény a menüelem kiválasztása. */
    static int onEvent(sd_bus_message* m, void* ud, sd_bus_error*) {
        int32_t id = 0;
        const char* evt = nullptr;
        int r = sd_bus_message_read(m, "is", &id, &evt);
        if (r < 0) return r;
        r = sd_bus_message_skip(m, "vu");   // adat és időbélyeg: nem kell
        if (r < 0) return r;
        if (evt && std::strcmp(evt, "clicked") == 0) self(ud)->dispatch(id);
        return sd_bus_reply_method_return(m, "");
    }

    static int onAboutToShow(sd_bus_message* m, void*, sd_bus_error*) {
        return sd_bus_reply_method_return(m, "b", 0);   // a menü tartalma fix: nem kell frissíteni
    }

    static const sd_bus_vtable* itemVtable() {
        static const sd_bus_vtable vt[] = {
            SD_BUS_VTABLE_START(0),
            SD_BUS_PROPERTY("Category", "s", getCategory, 0, SD_BUS_VTABLE_PROPERTY_CONST),
            SD_BUS_PROPERTY("Id", "s", getId, 0, SD_BUS_VTABLE_PROPERTY_CONST),
            SD_BUS_PROPERTY("Title", "s", getTitle, 0, SD_BUS_VTABLE_PROPERTY_CONST),
            SD_BUS_PROPERTY("Status", "s", getStatus, 0, SD_BUS_VTABLE_PROPERTY_CONST),
            SD_BUS_PROPERTY("IconName", "s", getIconName, 0, SD_BUS_VTABLE_PROPERTY_CONST),
            SD_BUS_PROPERTY("ItemIsMenu", "b", getItemIsMenu, 0, SD_BUS_VTABLE_PROPERTY_CONST),
            SD_BUS_PROPERTY("Menu", "o", getMenu, 0, SD_BUS_VTABLE_PROPERTY_CONST),
            SD_BUS_METHOD("Activate", "ii", "", onActivate, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("SecondaryActivate", "ii", "", onNoop, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("ContextMenu", "ii", "", onNoop, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("Scroll", "is", "", onNoop, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_VTABLE_END
        };
        return vt;
    }

    static const sd_bus_vtable* menuVtable() {
        static const sd_bus_vtable vt[] = {
            SD_BUS_VTABLE_START(0),
            SD_BUS_PROPERTY("Version", "u", getMenuVersion, 0, SD_BUS_VTABLE_PROPERTY_CONST),
            SD_BUS_PROPERTY("TextDirection", "s", getMenuTextDirection, 0, SD_BUS_VTABLE_PROPERTY_CONST),
            SD_BUS_PROPERTY("Status", "s", getMenuStatus, 0, SD_BUS_VTABLE_PROPERTY_CONST),
            SD_BUS_METHOD("GetLayout", "iias", "u(ia{sv}av)", onGetLayout, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("Event", "isvu", "", onEvent, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("AboutToShow", "i", "b", onAboutToShow, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_VTABLE_END
        };
        return vt;
    }

    sd_bus* bus_ = nullptr;
    std::string service_;
    Labels labels_;
    Callbacks cb_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> registered_{false};
};

} // namespace ee_analyzer

#endif // AQ_HAVE_SYSTEMD
