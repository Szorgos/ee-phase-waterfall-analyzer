#pragma once
/**
 * @file media_smtc.hpp
 * @brief „Most szól” és lejátszásvezérlés Windowson: a rendszer médiavezérlése (System Media Transport Controls).
 *
 * Ugyanaz a felület, amit a Windows hangerő-menüje és a billentyűzet médiagombjai használnak: a TIDAL asztali
 * alkalmazás, a Spotify, a böngészők stb. itt jelentik, mi szól, és innen fogadják a lejátszás / szünet /
 * következő / előző / tekerés parancsot. A Windows.Media.Control WinRT osztályokat közvetlenül az ABI-n át érjük el
 * (a felületazonosítók és a metódusok sorrendje a Windows SDK metaadataiból, ahogy a windows-rs is generálja); a
 * combase.dll / shcore.dll függvényeit futás közben töltjük be, így a motor ezek nélkül is elindul.
 */
#include <windows.h>
#include <objidl.h>
#include <inspectable.h>
#include <asyncinfo.h>
#include <hstring.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "media_session.hpp"

namespace ee_analyzer::media {

namespace smtc {

typedef unsigned char boolean8;   // WinRT boolean

// Windows.Media.Control (vtable-sorrend: IInspectable után, a metaadatok szerint)
struct DECLSPEC_NOVTABLE IAsyncOp : IInspectable {   // IAsyncOperation<T>
    virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown** handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(void* result) = 0;
};

struct DECLSPEC_NOVTABLE IVectorView : IInspectable {   // IVectorView<T>
    virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32 index, void* item) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32* size) = 0;
};

struct DECLSPEC_NOVTABLE IStreamReference : IInspectable {   // Windows.Storage.Streams.IRandomAccessStreamReference
    virtual HRESULT STDMETHODCALLTYPE OpenReadAsync(IAsyncOp** op) = 0;
};

struct DECLSPEC_NOVTABLE IMediaProperties : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Title(HSTRING* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Subtitle(HSTRING* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AlbumArtist(HSTRING* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Artist(HSTRING* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AlbumTitle(HSTRING* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_TrackNumber(INT32* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Genres(IInspectable** v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AlbumTrackCount(INT32* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PlaybackType(IInspectable** v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Thumbnail(IStreamReference** v) = 0;
};

struct DECLSPEC_NOVTABLE IPlaybackControls : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_IsPlayEnabled(boolean8* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPauseEnabled(boolean8* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsStopEnabled(boolean8* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsRecordEnabled(boolean8* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsFastForwardEnabled(boolean8* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsRewindEnabled(boolean8* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsNextEnabled(boolean8* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPreviousEnabled(boolean8* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsChannelUpEnabled(boolean8* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsChannelDownEnabled(boolean8* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPlayPauseToggleEnabled(boolean8* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsShuffleEnabled(boolean8* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsRepeatEnabled(boolean8* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPlaybackRateEnabled(boolean8* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPlaybackPositionEnabled(boolean8* v) = 0;
};

struct DECLSPEC_NOVTABLE IPlaybackInfo : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Controls(IPlaybackControls** v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PlaybackStatus(INT32* v) = 0;   // 4: lejátszás, 5: szünet
};

struct DECLSPEC_NOVTABLE ITimeline : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_StartTime(INT64* v) = 0;        // TimeSpan: 100 ns
    virtual HRESULT STDMETHODCALLTYPE get_EndTime(INT64* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MinSeekTime(INT64* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MaxSeekTime(INT64* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Position(INT64* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_LastUpdatedTime(INT64* v) = 0;  // DateTime: 100 ns 1601 óta (UTC)
};

struct DECLSPEC_NOVTABLE ISession : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_SourceAppUserModelId(HSTRING* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryGetMediaPropertiesAsync(IAsyncOp** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetTimelineProperties(ITimeline** v) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPlaybackInfo(IPlaybackInfo** v) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryPlayAsync(IAsyncOp** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryPauseAsync(IAsyncOp** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryStopAsync(IAsyncOp** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryRecordAsync(IAsyncOp** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryFastForwardAsync(IAsyncOp** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryRewindAsync(IAsyncOp** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TrySkipNextAsync(IAsyncOp** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TrySkipPreviousAsync(IAsyncOp** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangeChannelUpAsync(IAsyncOp** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangeChannelDownAsync(IAsyncOp** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryTogglePlayPauseAsync(IAsyncOp** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangeAutoRepeatModeAsync(INT32 mode, IAsyncOp** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangePlaybackRateAsync(DOUBLE rate, IAsyncOp** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangeShuffleActiveAsync(boolean8 on, IAsyncOp** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangePlaybackPositionAsync(INT64 position, IAsyncOp** op) = 0;
};

struct DECLSPEC_NOVTABLE IManager : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSession(ISession** v) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSessions(IVectorView** v) = 0;
};

struct DECLSPEC_NOVTABLE IManagerStatics : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE RequestAsync(IAsyncOp** op) = 0;
};

// IGlobalSystemMediaTransportControlsSessionManagerStatics
constexpr GUID kIidManagerStatics = { 0x2050c4ee, 0x11a0, 0x57de, { 0xae, 0xd7, 0xc9, 0x7c, 0x70, 0x33, 0x82, 0x45 } };
constexpr GUID kIidAsyncInfo = { 0x00000036, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
constexpr GUID kIidStream = { 0x0000000c, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };   // IStream

template <typename T>
struct Ref {
    T* p = nullptr;
    Ref() = default;
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;
    ~Ref() { reset(); }
    void reset() { if (p) { p->Release(); p = nullptr; } }
    T** put() { reset(); return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

/** A combase.dll / shcore.dll függvényei (Windows 8-tól; a motor nélkülük is fut, csak a „most szól” nem). */
struct Api {
    HRESULT(WINAPI* RoInitialize)(int) = nullptr;
    void(WINAPI* RoUninitialize)() = nullptr;
    HRESULT(WINAPI* RoGetActivationFactory)(HSTRING, REFIID, void**) = nullptr;
    HRESULT(WINAPI* WindowsCreateString)(const wchar_t*, UINT32, HSTRING*) = nullptr;
    HRESULT(WINAPI* WindowsDeleteString)(HSTRING) = nullptr;
    const wchar_t*(WINAPI* WindowsGetStringRawBuffer)(HSTRING, UINT32*) = nullptr;
    HRESULT(WINAPI* CreateStreamOverRandomAccessStream)(IUnknown*, REFIID, void**) = nullptr;

    bool load() {
        HMODULE combase = LoadLibraryW(L"combase.dll");
        if (!combase) return false;
        RoInitialize = reinterpret_cast<decltype(RoInitialize)>(GetProcAddress(combase, "RoInitialize"));
        RoUninitialize = reinterpret_cast<decltype(RoUninitialize)>(GetProcAddress(combase, "RoUninitialize"));
        RoGetActivationFactory = reinterpret_cast<decltype(RoGetActivationFactory)>(GetProcAddress(combase, "RoGetActivationFactory"));
        WindowsCreateString = reinterpret_cast<decltype(WindowsCreateString)>(GetProcAddress(combase, "WindowsCreateString"));
        WindowsDeleteString = reinterpret_cast<decltype(WindowsDeleteString)>(GetProcAddress(combase, "WindowsDeleteString"));
        WindowsGetStringRawBuffer = reinterpret_cast<decltype(WindowsGetStringRawBuffer)>(GetProcAddress(combase, "WindowsGetStringRawBuffer"));
        if (HMODULE shcore = LoadLibraryW(L"shcore.dll"))
            CreateStreamOverRandomAccessStream = reinterpret_cast<decltype(CreateStreamOverRandomAccessStream)>(
                GetProcAddress(shcore, "CreateStreamOverRandomAccessStream"));
        return RoInitialize && RoGetActivationFactory && WindowsCreateString && WindowsDeleteString && WindowsGetStringRawBuffer;
    }
};

inline std::string utf8(const wchar_t* w, UINT32 n) {
    if (!w || n == 0) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(n), s.data(), len, nullptr, nullptr);
    return s;
}

} // namespace smtc

class SmtcSession {
public:
    SmtcSession() = default;
    ~SmtcSession() { stop(); }
    SmtcSession(const SmtcSession&) = delete;
    SmtcSession& operator=(const SmtcSession&) = delete;

    void start() {
        if (running_.exchange(true)) return;
        thread_ = std::thread([this] { loop(); });
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (thread_.joinable()) thread_.join();
    }

    Info snapshot() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return info_;
    }

    std::string json() const { return toJson(snapshot()); }

    /** Parancs: a médiaszál hajtja végre (a hívó nem vár rá). */
    void command(const std::string& action, double value) {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (pending_.size() < 16) pending_.push_back({ action, value });
        }
        wake_ = true;
    }

    bool art(const std::string& id, std::string& mime, std::string& bytes, std::string& /*url*/) const {
        std::lock_guard<std::mutex> lk(mutex_);
        if (id.empty() || id != info_.artId || artBytes_.empty()) return false;
        bytes = artBytes_;
        mime = artMime_;
        return true;
    }

private:
    std::string str(HSTRING h) const {
        if (!h) return {};
        UINT32 n = 0;
        const wchar_t* w = api_.WindowsGetStringRawBuffer(h, &n);
        std::string s = smtc::utf8(w, n);
        api_.WindowsDeleteString(h);
        return s;
    }

    /** Egy aszinkron művelet megvárása (legfeljebb timeoutMs), majd az eredménye. */
    bool await(smtc::IAsyncOp* op, void* result, int timeoutMs = 2000) const {
        if (!op) return false;
        smtc::Ref<IAsyncInfo> info;
        if (FAILED(op->QueryInterface(smtc::kIidAsyncInfo, reinterpret_cast<void**>(info.put())))) return false;
        AsyncStatus st = AsyncStatus::Started;
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (SUCCEEDED(info->get_Status(&st)) && st == AsyncStatus::Started) {
            if (std::chrono::steady_clock::now() > until) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        if (st != AsyncStatus::Completed) return false;
        return !result || SUCCEEDED(op->GetResults(result));
    }

    bool initManager() {
        if (manager_) return true;
        HSTRING name = nullptr;
        static const wchar_t kClass[] = L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager";
        if (FAILED(api_.WindowsCreateString(kClass, static_cast<UINT32>(wcslen(kClass)), &name))) return false;
        smtc::Ref<smtc::IManagerStatics> statics;
        const HRESULT hr = api_.RoGetActivationFactory(name, smtc::kIidManagerStatics, reinterpret_cast<void**>(statics.put()));
        api_.WindowsDeleteString(name);
        if (FAILED(hr) || !statics) return false;
        smtc::Ref<smtc::IAsyncOp> op;
        if (FAILED(statics->RequestAsync(op.put()))) return false;
        return await(op.p, &manager_.p, 5000) && manager_;
    }

    /** A választott munkamenet: ami épp szól (a legutóbbi előnyben), különben a rendszer „aktuális” munkamenete. */
    bool pickSession(smtc::Ref<smtc::ISession>& out) {
        smtc::Ref<smtc::IVectorView> list;
        UINT32 n = 0;
        if (SUCCEEDED(manager_->GetSessions(list.put())) && list && SUCCEEDED(list->get_Size(&n))) {
            smtc::ISession* playing = nullptr;
            for (UINT32 i = 0; i < n && i < 32; ++i) {
                smtc::ISession* s = nullptr;
                if (FAILED(list->GetAt(i, &s)) || !s) continue;
                smtc::Ref<smtc::IPlaybackInfo> pi;
                INT32 status = 0;
                if (SUCCEEDED(s->GetPlaybackInfo(pi.put())) && pi) pi->get_PlaybackStatus(&status);
                HSTRING h = nullptr;
                const std::string id = SUCCEEDED(s->get_SourceAppUserModelId(&h)) ? str(h) : std::string();
                if (status == 4 && (!playing || id == lastActive_)) {
                    if (playing) playing->Release();
                    playing = s;
                    lastActiveCandidate_ = id;
                } else {
                    s->Release();
                }
            }
            if (playing) {
                out.reset();
                out.p = playing;
                lastActive_ = lastActiveCandidate_;
                return true;
            }
        }
        return SUCCEEDED(manager_->GetCurrentSession(out.put())) && out;
    }

    void readArt(smtc::IMediaProperties* props, const std::string& artId) {
        if (artId == loadedArt_) return;
        loadedArt_ = artId;
        std::string bytes, mime;
        smtc::Ref<smtc::IStreamReference> ref;
        if (api_.CreateStreamOverRandomAccessStream && SUCCEEDED(props->get_Thumbnail(ref.put())) && ref) {
            smtc::Ref<smtc::IAsyncOp> op;
            smtc::Ref<IUnknown> ras;
            if (SUCCEEDED(ref->OpenReadAsync(op.put())) && await(op.p, &ras.p) && ras) {
                smtc::Ref<IStream> stream;
                if (SUCCEEDED(api_.CreateStreamOverRandomAccessStream(ras.p, smtc::kIidStream, reinterpret_cast<void**>(stream.put()))) && stream) {
                    char buf[16384];
                    ULONG got = 0;
                    while (SUCCEEDED(stream->Read(buf, sizeof(buf), &got)) && got > 0 && bytes.size() < (2u << 20))
                        bytes.append(buf, got);
                }
            }
        }
        if (bytes.size() >= (2u << 20)) bytes.clear();
        if (!bytes.empty())
            mime = static_cast<unsigned char>(bytes[0]) == 0x89 ? "image/png" : bytes.compare(0, 4, "RIFF") == 0 ? "image/webp" : "image/jpeg";
        std::lock_guard<std::mutex> lk(mutex_);
        artBytes_ = std::move(bytes);
        artMime_ = mime;
    }

    void refresh() {
        Info m;
        smtc::Ref<smtc::ISession> s;
        if (initManager() && pickSession(s)) {
            HSTRING h = nullptr;
            const std::string aumid = SUCCEEDED(s->get_SourceAppUserModelId(&h)) ? str(h) : std::string();
            m.available = true;
            m.app = friendlyApp(aumid);

            smtc::Ref<smtc::IPlaybackInfo> pi;
            if (SUCCEEDED(s->GetPlaybackInfo(pi.put())) && pi) {
                INT32 status = 0;
                pi->get_PlaybackStatus(&status);
                m.playing = status == 4;
                smtc::Ref<smtc::IPlaybackControls> c;
                if (SUCCEEDED(pi->get_Controls(c.put())) && c) {
                    smtc::boolean8 b = 0;
                    m.canPlay = SUCCEEDED(c->get_IsPlayEnabled(&b)) && b;
                    m.canPause = SUCCEEDED(c->get_IsPauseEnabled(&b)) && b;
                    m.canNext = SUCCEEDED(c->get_IsNextEnabled(&b)) && b;
                    m.canPrevious = SUCCEEDED(c->get_IsPreviousEnabled(&b)) && b;
                    m.canSeek = SUCCEEDED(c->get_IsPlaybackPositionEnabled(&b)) && b;
                }
            }

            smtc::Ref<smtc::ITimeline> tl;
            if (SUCCEEDED(s->GetTimelineProperties(tl.put())) && tl) {
                INT64 start = 0, end = 0, pos = 0, updated = 0;
                tl->get_StartTime(&start);
                tl->get_EndTime(&end);
                tl->get_Position(&pos);
                tl->get_LastUpdatedTime(&updated);
                m.duration = end > start ? static_cast<double>(end - start) / 1e7 : 0.0;
                double p = static_cast<double>(pos - start) / 1e7;
                // a pozíció a lejátszó utolsó jelentésekor érvényes: a lejátszás alatt eltelt idővel előre
                FILETIME ft;
                GetSystemTimeAsFileTime(&ft);
                const INT64 now = (static_cast<INT64>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
                if (m.playing && updated > 0 && now > updated) p += static_cast<double>(now - updated) / 1e7;
                m.position = p;
                m.positionAt = std::chrono::steady_clock::now();
            }

            smtc::Ref<smtc::IAsyncOp> op;
            smtc::Ref<smtc::IMediaProperties> props;
            if (SUCCEEDED(s->TryGetMediaPropertiesAsync(op.put())) && await(op.p, &props.p) && props) {
                HSTRING v = nullptr;
                if (SUCCEEDED(props->get_Title(&v))) m.title = str(v);
                v = nullptr;
                if (SUCCEEDED(props->get_Artist(&v))) m.artist = str(v);
                v = nullptr;
                if (SUCCEEDED(props->get_AlbumTitle(&v))) m.album = str(v);
                m.artId = hashId(aumid + "\n" + m.title + "\n" + m.artist + "\n" + m.album);
                readArt(props.p, m.artId);
            }
            session_.reset();
            session_.p = s.p;
            s.p = nullptr;
        } else {
            session_.reset();
        }
        std::lock_guard<std::mutex> lk(mutex_);
        if (artBytes_.empty()) m.artId.clear();
        info_ = m;
    }

    void execute(const std::string& action, double value) {
        if (!session_) return;
        smtc::Ref<smtc::IAsyncOp> op;
        HRESULT hr = E_FAIL;
        if (action == "play") hr = session_->TryPlayAsync(op.put());
        else if (action == "pause") hr = session_->TryPauseAsync(op.put());
        else if (action == "toggle") hr = session_->TryTogglePlayPauseAsync(op.put());
        else if (action == "next") hr = session_->TrySkipNextAsync(op.put());
        else if (action == "previous") hr = session_->TrySkipPreviousAsync(op.put());
        else if (action == "seek") {
            INT64 start = 0;
            smtc::Ref<smtc::ITimeline> tl;
            if (SUCCEEDED(session_->GetTimelineProperties(tl.put())) && tl) tl->get_StartTime(&start);
            hr = session_->TryChangePlaybackPositionAsync(start + static_cast<INT64>(std::max(0.0, value) * 1e7), op.put());
        }
        if (SUCCEEDED(hr)) {
            smtc::boolean8 ok = 0;
            await(op.p, &ok, 1500);
        }
    }

    void loop() {
        if (!api_.load()) {
            std::cout << "[Média] A rendszer médiavezérlése nem érhető el (combase.dll), a „most szól” nem működik.\n";
            return;
        }
        const HRESULT init = api_.RoInitialize(1);   // RO_INIT_MULTITHREADED
        while (running_) {
            std::vector<std::pair<std::string, double>> todo;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                todo.swap(pending_);
            }
            for (const auto& [a, v] : todo) execute(a, v);
            if (!todo.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(150));   // a lejátszó frissítse az állapotát
            refresh();
            for (int i = 0; i < 16 && running_ && !wake_.exchange(false); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));   // kb. 0,8 s-onként, parancs után azonnal
        }
        session_.reset();
        manager_.reset();
        if (SUCCEEDED(init) && api_.RoUninitialize) api_.RoUninitialize();
    }

    smtc::Api api_;
    smtc::Ref<smtc::IManager> manager_;
    smtc::Ref<smtc::ISession> session_;
    std::string lastActive_, lastActiveCandidate_, loadedArt_;

    std::atomic<bool> running_{false};
    std::atomic<bool> wake_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    Info info_;
    std::string artBytes_, artMime_;
    std::vector<std::pair<std::string, double>> pending_;
};

} // namespace ee_analyzer::media
