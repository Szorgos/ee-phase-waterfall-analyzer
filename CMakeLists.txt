cmake_minimum_required(VERSION 3.20)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
project(aqulify 
    VERSION 2.8.4
    DESCRIPTION "Aqulify - Real-time Phase, Group Delay & Acoustic Waterfall DSP Suite"
    LANGUAGES C CXX
)

# C++20 szabvány megkövetelése
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Az EasyEffects DSP könyvtárai (LSP, Calf, libbs2b, SpeexDSP, zita-convolver, FFTW) a saját
# fordítási kapcsolóikkal, ezért a projekt kapcsolói előtt kerülnek be
if(MINGW)
    add_link_options(-static)
endif()
add_subdirectory(third_party)

# Fordítási figyelmeztetések és optimalizációk. A -ffast-math (/fp:fast) szándékosan nincs: Apple Silicon-on (clang)
# a tesztben elhagyott két ParamData-mezőírást (a Bass Enhancer így 0 Hz-es szűrőt kapott), és a NaN-ellenőrzéseket
# (sanitizeParams: v == v) is kiiktatja. A teljes lánc CPU-igénye nélküle kb. 3 %-kal több (0,86 -> 0,89 % egy magon).
if(MSVC)
    add_compile_options(/W4 /O2)
else()
    add_compile_options(-Wall -Wextra -O3)
endif()

# MinGW alatt minden bináris statikus C++ futtatókörnyezettel készül (nincs extra DLL-függőség): lásd fent

# Fejlécek elérési útvonala
set(EE_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

# 1. DSP Mag Könyvtár (Platformfüggetlen Interface könyvtár)
add_library(aqulify_ee STATIC src/ee_dsp_lsp.cpp src/ee_dsp_loudness.cpp src/ee_dsp_stereo_tools.cpp src/ee_dsp_misc.cpp src/ee_dsp_convolver.cpp src/rnn_denoise.cpp)
target_include_directories(aqulify_ee PUBLIC ${EE_INCLUDE_DIR})
target_link_libraries(aqulify_ee PUBLIC aq_lsp aq_calf aq_bs2b aq_speex aq_zita aq_ebur128)
target_link_libraries(aqulify_ee PRIVATE aq_rnnoise)
set_target_properties(aqulify_ee PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
    # A könyvtárakból átvett jelfeldolgozó kód az eredetivel azonos lebegőpontos szabályokkal fusson (biztosíték:
    # a -ffast-math globálisan sincs bekapcsolva)
    target_compile_options(aqulify_ee PRIVATE -fno-fast-math)
endif()

add_library(ee_dsp_core INTERFACE)
target_include_directories(ee_dsp_core INTERFACE ${EE_INCLUDE_DIR})
target_link_libraries(ee_dsp_core INTERFACE aqulify_ee)

# 4. Tesztalkalmazás: Zármentes Körpuffer (Valós idejű Audio -> 3D Grafika szálátvitel)
find_package(Threads REQUIRED)
add_executable(test_ringbuffer src/test_ringbuffer.cpp)
target_link_libraries(test_ringbuffer PRIVATE ee_dsp_core Threads::Threads)

# 5. Tesztalkalmazás: közös mag (FFT spektrum, paraméterblokk, tap körpuffer, JSON)
add_executable(test_core src/test_core.cpp)
target_compile_definitions(test_core PRIVATE AQULIFY_VERSION="${PROJECT_VERSION}")
target_link_libraries(test_core PRIVATE ee_dsp_core Threads::Threads)
if(WIN32)
    target_link_libraries(test_core PRIVATE ws2_32 iphlpapi)
endif()

# 6. Tesztalkalmazás: DSP modulok (partícionált konvolver, FIR EQ, Bass Enhancer, teljes lánc)
add_executable(test_dsp src/test_dsp.cpp)
target_link_libraries(test_dsp PRIVATE ee_dsp_core Threads::Threads)

# 7. Tesztalkalmazás: mintavételi frekvencia × blokkméret mátrix (átlátszóság, blokkfüggetlenség, szintek)
add_executable(test_matrix src/test_matrix.cpp)
target_link_libraries(test_matrix PRIVATE ee_dsp_core Threads::Threads)
add_executable(test_multiout src/test_multiout.cpp)
target_link_libraries(test_multiout PRIVATE ee_dsp_core Threads::Threads)

# 8. Tesztalkalmazás: ébren tartás (burn-in) a Windows-os hídon; nem Windows-on kihagyva
add_executable(test_keepawake src/test_keepawake.cpp)
target_link_libraries(test_keepawake PRIVATE ee_dsp_core)

# 8b. Tesztalkalmazás: szobamérő mag (log sweep, dekonvolúció, simítás, célgörbe, auto-EQ)
add_executable(test_room src/test_room.cpp)
target_link_libraries(test_room PRIVATE ee_dsp_core Threads::Threads)
target_compile_definitions(test_room PRIVATE AQ_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")

# 2.x: beépített zenelejátszó (dekóderek: third_party/decoders, megengedő licencek; a hangkimenet platformonként)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/decoders/opus/opus EXCLUDE_FROM_ALL)
set_target_properties(opus PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(aq_decoders STATIC
    src/decoders_impl.c
    third_party/decoders/alac/ALACDecoder.cpp
    third_party/decoders/opus/ogg/bitwise.c
    third_party/decoders/opus/ogg/framing.c
    third_party/decoders/opus/opusfile/opusfile.c
    third_party/decoders/opus/opusfile/info.c
    third_party/decoders/opus/opusfile/internal.c
    third_party/decoders/opus/opusfile/stream.c
    third_party/decoders/wavpack/src/common_utils.c
    third_party/decoders/wavpack/src/decorr_utils.c
    third_party/decoders/wavpack/src/entropy_utils.c
    third_party/decoders/wavpack/src/extra1.c
    third_party/decoders/wavpack/src/extra2.c
    third_party/decoders/wavpack/src/open_filename.c
    third_party/decoders/wavpack/src/open_legacy.c
    third_party/decoders/wavpack/src/open_raw.c
    third_party/decoders/wavpack/src/open_utils.c
    third_party/decoders/wavpack/src/read_words.c
    third_party/decoders/wavpack/src/tag_utils.c
    third_party/decoders/wavpack/src/tags.c
    third_party/decoders/wavpack/src/unpack.c
    third_party/decoders/wavpack/src/unpack_floats.c
    third_party/decoders/wavpack/src/unpack_seek.c
    third_party/decoders/wavpack/src/unpack_utils.c
    third_party/decoders/wavpack/src/unpack3.c
    third_party/decoders/wavpack/src/unpack3_open.c
    third_party/decoders/wavpack/src/unpack3_seek.c
)
target_include_directories(aq_decoders SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/decoders
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/decoders/alac
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/decoders/opus/ogg
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/decoders/opus/opusfile
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/decoders/opus/opus/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/decoders/wavpack/include
)
target_compile_definitions(aq_decoders PRIVATE OP_DISABLE_HTTP=1 ENABLE_LEGACY=1 HAVE___BUILTIN_CLZ=1)
set_target_properties(aq_decoders PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
target_compile_options(aq_decoders PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/w,-w>)
target_link_libraries(aq_decoders PUBLIC opus)
if(NOT WIN32)
    target_link_libraries(aq_decoders PUBLIC m)
endif()
# AAC-dekóder az internetrádióhoz: FAAD2 2.11.4 (third_party/faad2, GPL-2.0-or-later), lebegőpontos változat, DRM nélkül
file(GLOB AQ_FAAD_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/faad2/libfaad/*.c)
add_library(aq_faad STATIC ${AQ_FAAD_SOURCES})
target_include_directories(aq_faad PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/faad2/libfaad)
target_include_directories(aq_faad SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/faad2/include)
target_compile_definitions(aq_faad PRIVATE HAVE_INTTYPES_H=1 HAVE_MEMCPY=1 HAVE_STRING_H=1 HAVE_STRINGS_H=1
    HAVE_SYS_STAT_H=1 HAVE_SYS_TYPES_H=1 HAVE_LRINTF=1 APPLY_DRC PACKAGE_VERSION="2.11.4")
set_target_properties(aq_faad PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
target_compile_options(aq_faad PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/w,-w>)
if(NOT WIN32)
    target_link_libraries(aq_faad PUBLIC m)
endif()

add_library(aq_player STATIC src/audio_decoder.cpp src/file_player.cpp src/play_queue.cpp src/playlists.cpp src/play_stats.cpp src/smart_lists.cpp src/music_library.cpp
    src/radio_stream.cpp)
target_include_directories(aq_player PUBLIC ${EE_INCLUDE_DIR})
target_link_libraries(aq_player PUBLIC aq_decoders aq_faad soxr Threads::Threads)
if(WIN32)
    target_link_libraries(aq_player PUBLIC shell32 ole32)   # a Zene ismert mappa (SHGetKnownFolderPath)
    target_link_libraries(aq_player PUBLIC winhttp ws2_32)  # internetrádió (WinHTTP, névfeloldás)
endif()
target_compile_definitions(aq_player PRIVATE AQULIFY_VERSION="${PROJECT_VERSION}")   # a rádió-katalógus User-Agentje
set_target_properties(aq_player PROPERTIES POSITION_INDEPENDENT_CODE ON)

# 9r. Tesztalkalmazás: internetrádió (helyi próbaszerverrel: ICY, átirányítás, megszakadás, tiltott címek)
add_executable(test_radio src/test_radio.cpp)
target_link_libraries(test_radio PRIVATE aq_player ee_dsp_core Threads::Threads)
if(WIN32)
    target_link_libraries(test_radio PRIVATE ws2_32 iphlpapi)
endif()
target_compile_definitions(test_radio PRIVATE AQ_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")

# 9. Tesztalkalmazás: a beépített lejátszó (dekóderek, címkék, tekerés, a lejátszás vége)
add_executable(test_player src/test_player.cpp)
target_link_libraries(test_player PRIVATE aq_player ee_dsp_core Threads::Threads)   # ee_dsp_core: a híd fejlécei (zenetár-üzenetek)
if(WIN32)
    target_link_libraries(test_player PRIVATE ws2_32 iphlpapi)
endif()
target_compile_definitions(test_player PRIVATE AQ_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")

# 9c. Tesztalkalmazás: felmintavételezés (libsoxr)
add_executable(test_upsample src/test_upsample.cpp)
target_link_libraries(test_upsample PRIVATE aq_player ee_dsp_core soxr Threads::Threads)
if(WIN32)
    target_link_libraries(test_upsample PRIVATE ws2_32 iphlpapi)
endif()

# Nagy felbontás, A rész: DSP-korlát 384 kHz fölött, 352,8–768 kHz, két szálas felmintavételezés, terhelés-tanácsadó
add_executable(test_hires src/test_hires.cpp)
target_link_libraries(test_hires PRIVATE aq_player ee_dsp_core soxr Threads::Threads)
target_include_directories(test_hires PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(WIN32)
    target_link_libraries(test_hires PRIVATE ws2_32 iphlpapi)
endif()
# Nagy felbontás, B rész: DSD512 / DSD1024 (fájlok, DSD→PCM minőség és sebesség, natív DSD-csomagolás)
add_executable(test_hires_b src/test_hires_b.cpp)
target_link_libraries(test_hires_b PRIVATE aq_player ee_dsp_core Threads::Threads)
target_include_directories(test_hires_b PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(WIN32)
    target_link_libraries(test_hires_b PRIVATE ws2_32 iphlpapi)
endif()
# SRC „Referencia”: parancssori átalakító a lejátszó kódútján (mérőeszköz, nem települ) és a tesztje
add_executable(aqulify-src src/aqulify_src.cpp)
target_link_libraries(aqulify-src PRIVATE aq_player soxr Threads::Threads)
target_include_directories(aqulify-src PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_executable(test_src_ref src/test_src_ref.cpp)
target_link_libraries(test_src_ref PRIVATE aq_player soxr Threads::Threads)
target_include_directories(test_src_ref PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(WIN32)
    target_link_libraries(aqulify-src PRIVATE ws2_32 iphlpapi)
    target_link_libraries(test_src_ref PRIVATE ws2_32 iphlpapi)
endif()


# Hibajelentő végpont (Cloudflare Worker POST /report). Üresre állítva a küldés kikapcsol, a felület ezt őszintén jelzi.
set(AQ_REPORT_URL "https://aqulify-hibajelento.gergoeagle.workers.dev/report" CACHE STRING "Hibajelento vegpont URL (ures: nincs kuldes)")
add_compile_definitions(AQ_REPORT_URL="${AQ_REPORT_URL}")

# Beépített hibajelentés, 1. szakasz: őrszem (szívverés), állapotfájl, helyi jelentés, tisztítás, rendellenes vég
add_executable(test_watchdog src/test_watchdog.cpp)
target_link_libraries(test_watchdog PRIVATE ee_dsp_core Threads::Threads)
target_include_directories(test_watchdog PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(WIN32)
    target_link_libraries(test_watchdog PRIVATE ws2_32 iphlpapi shell32 ole32)
endif()

# Beépített hibajelentés, küldő: hozzájárulás, méretkorlát, tisztítás, helyi HTTP-próba (curl / WinHTTP)
add_executable(test_report_sender src/test_report_sender.cpp)
target_link_libraries(test_report_sender PRIVATE ee_dsp_core Threads::Threads)
target_include_directories(test_report_sender PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(WIN32)
    target_link_libraries(test_report_sender PRIVATE ws2_32 iphlpapi winhttp)
endif()

# Képernyő nélküli (headless) Raspberry Pi párosítás, claim-biztonság, migráció, hangerő-őrző és aqulify-reset
add_executable(test_headless_pairing src/test_headless_pairing.cpp)
target_compile_definitions(test_headless_pairing PRIVATE AQULIFY_VERSION="${PROJECT_VERSION}")
target_link_libraries(test_headless_pairing PRIVATE ee_dsp_core Threads::Threads)
target_include_directories(test_headless_pairing PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(WIN32)
    target_link_libraries(test_headless_pairing PRIVATE ws2_32 iphlpapi)
endif()

# Linux párosítási mód (headless/normál), localAddresses() rangsorolás és Bluetooth platform-jelzés
add_executable(test_linux_pairing src/test_linux_pairing.cpp)
target_compile_definitions(test_linux_pairing PRIVATE AQULIFY_VERSION="${PROJECT_VERSION}")
target_link_libraries(test_linux_pairing PRIVATE ee_dsp_core Threads::Threads)
target_include_directories(test_linux_pairing PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(WIN32)
    target_link_libraries(test_linux_pairing PRIVATE ws2_32 iphlpapi)
endif()

# Linux Bluetooth RFCOMM szerver (BlueZ Profile1 a D-Bus-on): üzenetkódolás, állapotgép, hamis démon, híd
add_executable(test_bt_bluez src/test_bt_bluez.cpp)
target_compile_definitions(test_bt_bluez PRIVATE AQULIFY_VERSION="${PROJECT_VERSION}")
target_link_libraries(test_bt_bluez PRIVATE ee_dsp_core Threads::Threads)
target_include_directories(test_bt_bluez PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(WIN32)
    target_link_libraries(test_bt_bluez PRIVATE ws2_32 iphlpapi)
endif()

# MPRIS lejátszásvezérlés metódus-választás, fallback és PII-mentes napló
add_executable(test_mpris_control src/test_mpris_control.cpp)
target_include_directories(test_mpris_control PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Állapotváltás-napló hiszterézis / debounce teszt
add_executable(test_log_debounce src/test_log_debounce.cpp)
target_include_directories(test_log_debounce PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# 9b. Tesztalkalmazás: DSD (.dsf/.dff) fejléc, FIR szűrő, PCM dekódolás, tekerés, ID3v2, DST elutasítás, DoP 1.1
add_executable(test_dsd src/test_dsd.cpp)
target_link_libraries(test_dsd PRIVATE aq_player Threads::Threads)

if(WIN32)
    # ------------------------------------------------------------------
    # Windows: Aqulify.exe motor + Aqulify.dll (Equalizer APO plugin, VST 2.4 ABI)
    # ------------------------------------------------------------------
    option(BUILD_WINDOWS_ENGINE "Aqulify.exe Windows motor (WASAPI loopback + plugin híd)" ON)
    option(BUILD_EQAPO_PLUGIN "Aqulify.dll Equalizer APO plugin (VST 2.4 bináris interfész)" ON)
    set(BUILD_PIPEWIRE_PLUGIN OFF CACHE BOOL "" FORCE)

    enable_language(RC)
    # Az .rc fájlok UTF-8 kódolásúak (ékezetes szövegek): az llvm-windres alapból ANSI kódlapot feltételez,
    # és a beemelt (#include) fájlokban a "#pragma code_page" nem hat
    if(MINGW)
        string(APPEND CMAKE_RC_FLAGS " -c 65001")
    endif()
    set(EE_RES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/windows/resources)
    string(REPLACE "." "," AA_VER_COMMA "${PROJECT_VERSION},0")

    # Verzióinformáció fájlonként
    function(ee_version_rc out name file desc desc_hu filetype)
        set(AA_NAME "${name}")
        set(AA_FILE "${file}")
        set(AA_DESC "${desc}")
        set(AA_DESC_HU "${desc_hu}")
        set(AA_FILETYPE "${filetype}")
        configure_file(${EE_RES_DIR}/version.rc.in ${CMAKE_CURRENT_BINARY_DIR}/${out} @ONLY)
    endfunction()
    ee_version_rc(engine_version.rc "Aqulify" "Aqulify.exe" "Aqulify engine (tray app)" "Aqulify motor (tálcás háttéralkalmazás)" "VFT_APP")
    ee_version_rc(plugin_version.rc "Aqulify" "Aqulify.dll" "Aqulify Equalizer APO plugin (VST 2.4)" "Aqulify Equalizer APO plugin (VST 2.4)" "VFT_DLL")
    ee_version_rc(gui_version.rc "Aqulify" "AqulifyGui.exe" "Aqulify window (WebView2)" "Aqulify felület (WebView2 ablak)" "VFT_APP")
    ee_version_rc(setup_version.rc "Aqulify-Setup" "Aqulify-Setup.exe" "Aqulify Setup" "Aqulify telepítő" "VFT_APP")

    if(BUILD_WINDOWS_ENGINE)
        add_executable(Aqulify WIN32 windows/engine/aether_engine.cpp src/mic_capture.cpp ${EE_RES_DIR}/engine.rc)
        # Mikrofonfelvétel (WASAPI) alaptesztje – hangeszköz nélküli CI-gépen is fut
        add_executable(test_mic_basic src/test_mic_basic.cpp src/mic_capture.cpp)
        target_include_directories(test_mic_basic PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_link_libraries(test_mic_basic PRIVATE ole32 oleaut32 uuid)
        target_include_directories(Aqulify PRIVATE ${EE_RES_DIR} ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asiosdk/common)   # ASIO-kimenet (Steinberg ASIO SDK 2.3.4, GPLv3)
        target_link_libraries(Aqulify PRIVATE ee_dsp_core aq_player ws2_32 iphlpapi ole32 oleaut32 shell32 shlwapi advapi32 user32 uuid avrt winhttp)
        # 2.x: a beépített lejátszó parancssori indítója (konzolos program)
        add_executable(aqulify-play src/aqulify_play.cpp)
        target_link_libraries(aqulify-play PRIVATE ws2_32)
        target_compile_definitions(aqulify-play PRIVATE UNICODE _UNICODE)
        if(MINGW)
            target_link_options(aqulify-play PRIVATE -municode)   # wmain: Unicode parancssor
        endif()
        # A lejátszó WASAPI-kimenetének próbája néma fájllal (hangeszköz nélkül kihagyja)
        add_executable(player_out_check windows/tools/player_out_check.cpp)
        target_include_directories(player_out_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/windows/engine)
        target_link_libraries(player_out_check PRIVATE aq_player ee_dsp_core ole32 uuid avrt)
        target_compile_definitions(Aqulify PRIVATE UNICODE _UNICODE AQULIFY_VERSION="${PROJECT_VERSION}")
        if(MSVC)
            target_link_options(Aqulify PRIVATE /ENTRY:mainCRTStartup /MANIFEST:NO)
            set_property(TARGET Aqulify PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
        endif()
    endif()

    # Grafikus felület saját ablakban (WebView2; a webview/webview 0.12.0 beépített betöltőjével, WebView2Loader.dll nélkül)
    if(BUILD_WINDOWS_ENGINE)
        add_executable(AqulifyGui WIN32 windows/gui/aether_gui.cpp ${EE_RES_DIR}/gui.rc)
        target_include_directories(AqulifyGui PRIVATE ${EE_RES_DIR} ${CMAKE_CURRENT_BINARY_DIR} ${EE_INCLUDE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/windows/third_party ${CMAKE_CURRENT_SOURCE_DIR}/windows/third_party/webview2)
        target_compile_definitions(AqulifyGui PRIVATE UNICODE _UNICODE AQULIFY_VERSION="${PROJECT_VERSION}")
        target_link_libraries(AqulifyGui PRIVATE advapi32 ole32 oleaut32 shell32 shlwapi user32 version dwmapi uuid)
        if(MINGW)
            target_link_options(AqulifyGui PRIVATE -municode -static)
        endif()
        if(MSVC)
            target_link_options(AqulifyGui PRIVATE /MANIFEST:NO)
            set_property(TARGET AqulifyGui PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
        endif()
    endif()

    option(BUILD_WINDOWS_SETUP "Aqulify-Setup.exe telepítő (a motort és a plugint is beágyazza)" ON)

    if(BUILD_EQAPO_PLUGIN)
        add_library(AqulifyPlugin SHARED windows/plugin/aether_vst2.cpp windows/plugin/aether_vst2.def ${EE_RES_DIR}/plugin.rc)
        target_include_directories(AqulifyPlugin PRIVATE ${EE_RES_DIR} ${CMAKE_CURRENT_BINARY_DIR})
        set_target_properties(AqulifyPlugin PROPERTIES OUTPUT_NAME "Aqulify" PREFIX "")
        target_link_libraries(AqulifyPlugin PRIVATE ee_dsp_core advapi32)
        if(MSVC)
            # Statikus MSVC futtatókörnyezet (/MT): az audiodg.exe-be töltött DLL ne függjön a VC++ redisttől
            set_property(TARGET AqulifyPlugin PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
        endif()
    endif()

    # Teszt-hoszt: az Equalizer APO betöltési sorrendjét utánozza (a plugin önálló kipróbálásához)
    add_executable(eqapo_host_sim windows/tools/eqapo_host_sim.cpp)

    if(BUILD_WINDOWS_SETUP AND BUILD_WINDOWS_ENGINE AND BUILD_EQAPO_PLUGIN)
        # A beágyazandó fájlok egy átmeneti mappába kerülnek; a .rc fix útvonalakkal hivatkozik rájuk
        set(EE_PAYLOAD_DIR ${CMAKE_CURRENT_BINARY_DIR}/payload)
        # Aláírt kiadáshoz: a motor, a felület és a plugin a SignPath által aláírt példányból kerül a telepítőbe
        set(EE_SIGNED_BINARIES_DIR "" CACHE PATH "Aláírt Aqulify.exe / AqulifyGui.exe / Aqulify.dll mappája (üres: a most fordított fájlok)")
        if(EE_SIGNED_BINARIES_DIR)
            set(EE_SRC_ENGINE ${EE_SIGNED_BINARIES_DIR}/Aqulify.exe)
            set(EE_SRC_PLUGIN ${EE_SIGNED_BINARIES_DIR}/Aqulify.dll)
            set(EE_SRC_GUIAPP ${EE_SIGNED_BINARIES_DIR}/AqulifyGui.exe)
        else()
            set(EE_SRC_ENGINE $<TARGET_FILE:Aqulify>)
            set(EE_SRC_PLUGIN $<TARGET_FILE:AqulifyPlugin>)
            set(EE_SRC_GUIAPP $<TARGET_FILE:AqulifyGui>)
        endif()
        set(EE_PAYLOAD_SOURCES
            ${CMAKE_CURRENT_SOURCE_DIR}/gui_integrated_system.html
            ${CMAKE_CURRENT_SOURCE_DIR}/autoeq_index.js
            ${CMAKE_CURRENT_SOURCE_DIR}/gui_i18n.js
            ${CMAKE_CURRENT_SOURCE_DIR}/gui_player.js
            ${CMAKE_CURRENT_SOURCE_DIR}/import_parser.js
            ${CMAKE_CURRENT_SOURCE_DIR}/windows/third_party/three/three.min.js
            ${CMAKE_CURRENT_SOURCE_DIR}/windows/third_party/three/LICENSE
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/soxr/LICENCE
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/faad2/COPYING
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asiosdk/LICENSE.txt
            ${CMAKE_CURRENT_SOURCE_DIR}/windows/README_WINDOWS.md)
        add_custom_command(
            OUTPUT ${EE_PAYLOAD_DIR}/payload.stamp
            COMMAND ${CMAKE_COMMAND} -E make_directory ${EE_PAYLOAD_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy ${EE_SRC_ENGINE} ${EE_PAYLOAD_DIR}/engine.bin
            COMMAND ${CMAKE_COMMAND} -E copy ${EE_SRC_PLUGIN} ${EE_PAYLOAD_DIR}/plugin.bin
            COMMAND ${CMAKE_COMMAND} -E copy ${EE_SRC_GUIAPP} ${EE_PAYLOAD_DIR}/guiapp.bin
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/gui_integrated_system.html ${EE_PAYLOAD_DIR}/gui.bin
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/autoeq_index.js ${EE_PAYLOAD_DIR}/autoeq.bin
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/gui_i18n.js ${EE_PAYLOAD_DIR}/i18n.bin
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/gui_player.js ${EE_PAYLOAD_DIR}/player.bin
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/import_parser.js ${EE_PAYLOAD_DIR}/importparser.bin
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/windows/third_party/three/three.min.js ${EE_PAYLOAD_DIR}/three.bin
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/windows/third_party/three/LICENSE ${EE_PAYLOAD_DIR}/threelic.bin
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/third_party/soxr/LICENCE ${EE_PAYLOAD_DIR}/soxrlic.bin
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/third_party/faad2/COPYING ${EE_PAYLOAD_DIR}/faadlic.bin
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asiosdk/LICENSE.txt ${EE_PAYLOAD_DIR}/asiolic.bin
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/windows/README_WINDOWS.md ${EE_PAYLOAD_DIR}/readme.bin
            COMMAND ${CMAKE_COMMAND} -E touch ${EE_PAYLOAD_DIR}/payload.stamp
            DEPENDS Aqulify AqulifyPlugin AqulifyGui ${EE_PAYLOAD_SOURCES}
            COMMENT "Telepítő csomag összeállítása")
        file(WRITE ${EE_PAYLOAD_DIR}/index.txt
            "FILE_ENGINE|Aqulify.exe\n"
            "FILE_GUIAPP|AqulifyGui.exe\n"
            "FILE_PLUGIN|plugin\\Aqulify.dll\n"
            "FILE_GUI|gui\\gui_integrated_system.html\n"
            "FILE_AUTOEQ|gui\\autoeq_index.js\n"
            "FILE_I18N|gui\\gui_i18n.js\n"
            "FILE_PLAYERJS|gui\\gui_player.js\n"
            "FILE_IMPORTJS|gui\\import_parser.js\n"
            "FILE_THREE|gui\\three.min.js\n"
            "FILE_THREELIC|gui\\three.js-LICENSE.txt\n"
            "FILE_SOXRLIC|gui\\libsoxr-LICENSE.txt\n"
            "FILE_FAADLIC|gui\\faad2-COPYING.txt\n"
            "FILE_ASIOLIC|gui\\asiosdk-LICENSE.txt\n"
            "FILE_README|OLVASS_EL.md\n")
        file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/setup_payload.rc
            "PAYLOAD_INDEX RCDATA \"${EE_PAYLOAD_DIR}/index.txt\"\n"
            "FILE_ENGINE RCDATA \"${EE_PAYLOAD_DIR}/engine.bin\"\n"
            "FILE_GUIAPP RCDATA \"${EE_PAYLOAD_DIR}/guiapp.bin\"\n"
            "FILE_PLUGIN RCDATA \"${EE_PAYLOAD_DIR}/plugin.bin\"\n"
            "FILE_GUI RCDATA \"${EE_PAYLOAD_DIR}/gui.bin\"\n"
            "FILE_AUTOEQ RCDATA \"${EE_PAYLOAD_DIR}/autoeq.bin\"\n"
            "FILE_I18N RCDATA \"${EE_PAYLOAD_DIR}/i18n.bin\"\n"
            "FILE_PLAYERJS RCDATA \"${EE_PAYLOAD_DIR}/player.bin\"\n"
            "FILE_IMPORTJS RCDATA \"${EE_PAYLOAD_DIR}/importparser.bin\"\n"
            "FILE_THREE RCDATA \"${EE_PAYLOAD_DIR}/three.bin\"\n"
            "FILE_THREELIC RCDATA \"${EE_PAYLOAD_DIR}/threelic.bin\"\n"
            "FILE_SOXRLIC RCDATA \"${EE_PAYLOAD_DIR}/soxrlic.bin\"\n"
            "FILE_FAADLIC RCDATA \"${EE_PAYLOAD_DIR}/faadlic.bin\"\n"
            "FILE_ASIOLIC RCDATA \"${EE_PAYLOAD_DIR}/asiolic.bin\"\n"
            "FILE_README RCDATA \"${EE_PAYLOAD_DIR}/readme.bin\"\n")
        set_source_files_properties(${EE_RES_DIR}/setup.rc PROPERTIES OBJECT_DEPENDS ${EE_PAYLOAD_DIR}/payload.stamp)
        add_custom_target(AqulifyPayload DEPENDS ${EE_PAYLOAD_DIR}/payload.stamp)

        add_executable(AqulifySetup WIN32 windows/installer/setup.cpp ${EE_RES_DIR}/setup.rc)
        add_dependencies(AqulifySetup AqulifyPayload)
        set_target_properties(AqulifySetup PROPERTIES OUTPUT_NAME "Aqulify-Setup")
        target_include_directories(AqulifySetup PRIVATE ${EE_RES_DIR} ${CMAKE_CURRENT_BINARY_DIR} ${EE_INCLUDE_DIR})
        target_compile_definitions(AqulifySetup PRIVATE UNICODE _UNICODE AQULIFY_VERSION="${PROJECT_VERSION}")
        target_link_libraries(AqulifySetup PRIVATE comctl32 ole32 oleaut32 shell32 shlwapi winhttp advapi32 user32 gdi32 uuid)
        if(MINGW)
            target_link_options(AqulifySetup PRIVATE -municode)
        endif()
        if(MSVC)
            target_link_options(AqulifySetup PRIVATE /MANIFEST:NO)
            set_property(TARGET AqulifySetup PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
        endif()
    endif()
endif()

# macOS: Aqulify.app (menüsor-ikon, saját ablak és a Core Audio motor egy alkalmazásban)
if(APPLE)
    if(PROJECT_VERSION VERSION_LESS 1.7.1)
        message(FATAL_ERROR "A macOS változat az 1.7.1-gyel indult: régebbi verzió nem fordítható macOS-re")
    endif()
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 20)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    set(AQ_MACOS_MIN "${CMAKE_OSX_DEPLOYMENT_TARGET}")   # az Info.plist LSMinimumSystemVersion értéke
    if(NOT AQ_MACOS_MIN)
        set(AQ_MACOS_MIN 12.0)
    endif()
    set(AQ_MAC_RES ${CMAKE_CURRENT_SOURCE_DIR}/macos/resources)
    set(AQ_MAC_GUI
        ${CMAKE_CURRENT_SOURCE_DIR}/gui_integrated_system.html
        ${CMAKE_CURRENT_SOURCE_DIR}/autoeq_index.js
        ${CMAKE_CURRENT_SOURCE_DIR}/gui_i18n.js
        ${CMAKE_CURRENT_SOURCE_DIR}/gui_player.js
        ${CMAKE_CURRENT_SOURCE_DIR}/import_parser.js
        ${CMAKE_CURRENT_SOURCE_DIR}/windows/third_party/three/three.min.js)
    # Mikrofonfelvétel (Core Audio) alaptesztje – hangeszköz nélküli CI-gépen is fut
    add_executable(test_mic_basic src/test_mic_basic.cpp src/mic_capture.cpp)
    target_include_directories(test_mic_basic PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(test_mic_basic PRIVATE "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreFoundation")
    add_executable(Aqulify MACOSX_BUNDLE macos/app/main.mm macos/engine/mac_engine.mm macos/engine/mac_media.mm src/mic_capture.cpp ${AQ_MAC_GUI}
        ${AQ_MAC_RES}/Aqulify.icns ${AQ_MAC_RES}/en.lproj/InfoPlist.strings ${AQ_MAC_RES}/hu.lproj/InfoPlist.strings)
    set_source_files_properties(${AQ_MAC_GUI} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/gui)
    set_source_files_properties(${AQ_MAC_RES}/Aqulify.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_source_files_properties(${AQ_MAC_RES}/en.lproj/InfoPlist.strings PROPERTIES MACOSX_PACKAGE_LOCATION Resources/en.lproj)
    set_source_files_properties(${AQ_MAC_RES}/hu.lproj/InfoPlist.strings PROPERTIES MACOSX_PACKAGE_LOCATION Resources/hu.lproj)
    set_target_properties(Aqulify PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in
        MACOSX_BUNDLE_GUI_IDENTIFIER io.github.szorgos.Aqulify
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
    target_include_directories(Aqulify PRIVATE macos/engine ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_options(Aqulify PRIVATE -fobjc-arc)
    target_compile_definitions(Aqulify PRIVATE AQULIFY_VERSION="${PROJECT_VERSION}")
    target_link_libraries(Aqulify PRIVATE ee_dsp_core aq_player Threads::Threads
        "-framework AppKit" "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreFoundation"
        "-framework Foundation" "-framework ServiceManagement" "-framework WebKit")
    add_custom_command(TARGET Aqulify POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/windows/third_party/three/LICENSE
                $<TARGET_BUNDLE_CONTENT_DIR:Aqulify>/Resources/gui/three.js-LICENSE.txt
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/third_party/soxr/LICENCE
                $<TARGET_BUNDLE_CONTENT_DIR:Aqulify>/Resources/gui/libsoxr-LICENSE.txt
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/third_party/faad2/COPYING
                $<TARGET_BUNDLE_CONTENT_DIR:Aqulify>/Resources/gui/faad2-COPYING.txt)

    # 2.x: a beépített lejátszó parancssori indítója macOS-en is
    add_executable(aqulify-play src/aqulify_play.cpp)
    target_link_libraries(aqulify-play PRIVATE Threads::Threads)

    # A lejátszó CoreAudio-kimenetének próbája néma fájllal (hangeszköz nélkül kihagyja)
    add_executable(player_out_check macos/tools/player_out_check.mm)
    target_include_directories(player_out_check PRIVATE macos/engine ${EE_INCLUDE_DIR})
    target_compile_options(player_out_check PRIVATE -fobjc-arc)
    target_link_libraries(player_out_check PRIVATE aq_player ee_dsp_core Threads::Threads
        "-framework AudioToolbox" "-framework CoreAudio" "-framework Foundation")
endif()

# Linux: PipeWire motor (virtuális "Aqulify" kimeneti eszköz) és saját ablakos felület
option(BUILD_PIPEWIRE_PLUGIN "Aqulify PipeWire motor Linuxra" ON)
option(BUILD_LINUX_GUI "Saját ablakos felület (WebKitGTK), ha a fejlesztői csomag elérhető" ON)

if(BUILD_PIPEWIRE_PLUGIN AND NOT WIN32 AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
    add_executable(aqulify-engine src/pipewire_plugin.cpp src/mic_capture.cpp)
    target_include_directories(aqulify-engine PRIVATE ${PIPEWIRE_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/include)

    # Mikrofonfelvétel a szobaméréshez (PipeWire): élő teszt elszigetelt PipeWire-ben – tests/live/mic_live.sh
    add_executable(test_mic_live src/test_mic_live.cpp src/mic_capture.cpp)
    target_include_directories(test_mic_live PRIVATE ${PIPEWIRE_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_directories(test_mic_live PRIVATE ${PIPEWIRE_LIBRARY_DIRS})
    target_link_libraries(test_mic_live PRIVATE ${PIPEWIRE_LIBRARIES} Threads::Threads)
    # A teljes szobamérés élő tesztje (a motor lejátszója + felvevő + kiértékelés): tests/live/measure_live.sh
    add_executable(test_measure_live src/test_measure_live.cpp src/mic_capture.cpp)
    target_include_directories(test_measure_live PRIVATE ${PIPEWIRE_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_directories(test_measure_live PRIVATE ${PIPEWIRE_LIBRARY_DIRS})
    target_link_libraries(test_measure_live PRIVATE aq_player ee_dsp_core ${PIPEWIRE_LIBRARIES} Threads::Threads)
    target_link_directories(aqulify-engine PRIVATE ${PIPEWIRE_LIBRARY_DIRS})
    target_link_libraries(aqulify-engine PRIVATE ee_dsp_core aq_player ${PIPEWIRE_LIBRARIES} Threads::Threads)
    target_compile_definitions(aqulify-engine PRIVATE AQULIFY_VERSION="${PROJECT_VERSION}")
    # Csak tesztfordításhoz (alapból KI, kiadásba nem kerül): az élő hibajelentés-próba szándékosan beragaszthatja a híd
    # szálát (AQ_TEST_STALL_PULL_AFTER=<s> környezeti változó) – tests/live/hibajelento_live.py
    option(AQ_TEST_HOOKS "Tesztfordítás: szándékos beragasztás az élő őrszem-próbához" OFF)
    if(AQ_TEST_HOOKS)
        target_compile_definitions(aqulify-engine PRIVATE AQ_TEST_HOOKS=1)
    endif()

    # Tálcaikon Linuxon (StatusNotifierItem, sd-bus): csak ha a libsystemd fejléce megvan; nélküle a motor tálca nélkül fut
    pkg_check_modules(SYSTEMD libsystemd)
    if(SYSTEMD_FOUND)
        target_compile_definitions(aqulify-engine PRIVATE AQ_HAVE_SYSTEMD=1)
        target_include_directories(aqulify-engine PRIVATE ${SYSTEMD_INCLUDE_DIRS})
        target_link_directories(aqulify-engine PRIVATE ${SYSTEMD_LIBRARY_DIRS})
        target_link_libraries(aqulify-engine PRIVATE ${SYSTEMD_LIBRARIES})
        message(STATUS "Linux tálcaikon: libsystemd ${SYSTEMD_VERSION}")
    else()
        message(STATUS "Linux tálcaikon: nincs libsystemd, a motor tálca nélkül épül")
    endif()

    # Közvetlen ALSA-kimenet a beépített lejátszóhoz (kizárólagos, bitpontos mód): csak ha az ALSA fejléce megvan
    pkg_check_modules(ALSA alsa)
    if(ALSA_FOUND)
        target_compile_definitions(aqulify-engine PRIVATE AQ_HAVE_ALSA=1)
        target_include_directories(aqulify-engine PRIVATE ${ALSA_INCLUDE_DIRS})
        target_link_directories(aqulify-engine PRIVATE ${ALSA_LIBRARY_DIRS})
        target_link_libraries(aqulify-engine PRIVATE ${ALSA_LIBRARIES})
        message(STATUS "Közvetlen ALSA-kimenet: alsa ${ALSA_VERSION}")
        # a B rész natív DSD-csomagolásának tesztje az ALSA-fejléccel
        target_compile_definitions(test_hires_b PRIVATE AQ_TEST_ALSA=1)
        target_include_directories(test_hires_b PRIVATE ${ALSA_INCLUDE_DIRS})
        target_link_directories(test_hires_b PRIVATE ${ALSA_LIBRARY_DIRS})
        target_link_libraries(test_hires_b PRIVATE ${ALSA_LIBRARIES})
        if(SYSTEMD_FOUND)
            target_compile_definitions(test_hires_b PRIVATE AQ_HAVE_SYSTEMD=1)
            target_include_directories(test_hires_b PRIVATE ${SYSTEMD_INCLUDE_DIRS})
            target_link_libraries(test_hires_b PRIVATE ${SYSTEMD_LIBRARIES})
        endif()
        # Teszt: az ALSA „null” eszközével (hangkártya nélkül is fut)
        add_executable(test_alsa src/test_alsa.cpp)
        target_include_directories(test_alsa PRIVATE ${ALSA_INCLUDE_DIRS})
        target_link_directories(test_alsa PRIVATE ${ALSA_LIBRARY_DIRS})
        target_link_libraries(test_alsa PRIVATE aq_player ee_dsp_core ${ALSA_LIBRARIES} Threads::Threads)
        target_compile_definitions(test_alsa PRIVATE AQ_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
        if(SYSTEMD_FOUND)
            target_compile_definitions(test_alsa PRIVATE AQ_HAVE_SYSTEMD=1)
            target_include_directories(test_alsa PRIVATE ${SYSTEMD_INCLUDE_DIRS})
            target_link_libraries(test_alsa PRIVATE ${SYSTEMD_LIBRARIES})
        endif()
    else()
        message(STATUS "Közvetlen ALSA-kimenet: nincs alsa fejlesztői csomag, a lejátszó csak a PipeWire-en át szól")
    endif()

    # Parancssori indító a beépített lejátszóhoz: aqulify-play <fájl> | play | pause | stop
    add_executable(aqulify-play src/aqulify_play.cpp)

    # PipeWire lejátszó stream újracsatlakozási politika egységtesztje
    add_executable(test_pw_reconnect src/test_pw_reconnect.cpp)
    target_include_directories(test_pw_reconnect PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

    # Linux egyetlen-példány zár (flock) és két telepítés felismerése egységtesztje
    add_executable(test_linux_lock src/test_linux_lock.cpp)
    target_include_directories(test_linux_lock PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

    if(BUILD_LINUX_GUI)
        pkg_check_modules(WEBKITGTK IMPORTED_TARGET webkit2gtk-4.1)
        if(NOT WEBKITGTK_FOUND)
            pkg_check_modules(WEBKITGTK IMPORTED_TARGET webkit2gtk-4.0)
        endif()
        pkg_check_modules(GTK3 IMPORTED_TARGET gtk+-3.0)
        if(WEBKITGTK_FOUND AND GTK3_FOUND)
            add_executable(aqulify-gui src/aether_gui_linux.cpp)
            target_include_directories(aqulify-gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/windows/third_party ${EE_INCLUDE_DIR})
            target_link_libraries(aqulify-gui PRIVATE PkgConfig::GTK3 PkgConfig::WEBKITGTK dl)
            target_compile_definitions(aqulify-gui PRIVATE AQULIFY_VERSION="${PROJECT_VERSION}")
            message(STATUS "Saját ablakos felület (WebKitGTK) építése engedélyezve")
        else()
            message(STATUS "WebKitGTK nem található: a felület böngészőben nyílik (aqulify-gui.sh)")
        endif()
    endif()
endif()

# ------------------------------------------------------------------ Linux rendszerszintű telepítés (RPM / cmake --install)
if(BUILD_PIPEWIRE_PLUGIN AND NOT WIN32 AND NOT APPLE)
    include(GNUInstallDirs)
    string(TIMESTAMP AQ_RELEASE_DATE "%Y-%m-%d" UTC)
    set(BINDIR "${CMAKE_INSTALL_FULL_BINDIR}")
    set(RESET_HELPER "${CMAKE_INSTALL_FULL_LIBDIR}/aqulify/aqulify-reset-check")
    configure_file(linux/aqulify.service.in ${CMAKE_CURRENT_BINARY_DIR}/aqulify.service @ONLY)
    configure_file(linux/aqulify-reset.service.in ${CMAKE_CURRENT_BINARY_DIR}/aqulify-reset.service @ONLY)
    set(VERSION "${PROJECT_VERSION}")
    set(DATE "${AQ_RELEASE_DATE}")
    configure_file(linux/io.github.szorgos.Aqulify.metainfo.xml ${CMAKE_CURRENT_BINARY_DIR}/io.github.szorgos.Aqulify.metainfo.xml @ONLY)

    install(TARGETS aqulify-engine aqulify-play RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    if(TARGET aqulify-gui)
        install(TARGETS aqulify-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
    install(FILES gui_integrated_system.html autoeq_index.js gui_i18n.js gui_player.js import_parser.js windows/third_party/three/three.min.js
            DESTINATION ${CMAKE_INSTALL_DATADIR}/aqulify/gui)
    install(FILES windows/third_party/three/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/aqulify/gui RENAME three.js-LICENSE.txt)
    install(FILES third_party/soxr/LICENCE DESTINATION ${CMAKE_INSTALL_DATADIR}/aqulify/gui RENAME libsoxr-LICENSE.txt)
install(FILES third_party/faad2/COPYING DESTINATION ${CMAKE_INSTALL_DATADIR}/aqulify/gui RENAME faad2-COPYING.txt)
    install(FILES linux/io.github.szorgos.Aqulify.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/io.github.szorgos.Aqulify.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
    install(FILES linux/aqulify.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/aqulify.service DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/user)
    install(FILES linux/80-aqulify.preset DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/user-preset)
    install(PROGRAMS linux/aqulify-reset-check.sh DESTINATION ${CMAKE_INSTALL_LIBDIR}/aqulify RENAME aqulify-reset-check)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/aqulify-reset.service DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/system)
    install(DIRECTORY data/claim_audio DESTINATION ${CMAKE_INSTALL_DATADIR}/aqulify)
endif()

message(STATUS "Aqulify C++ DSP Engine sikeresen konfigurálva.")

# ------------------------------------------------------------------
# Windows-only: ASIO-kimenet motor tesztje (Steinberg ASIO SDK 2.3.4, GPLv3)
# ------------------------------------------------------------------
if(WIN32)
    add_executable(test_asio src/test_asio.cpp)
    target_include_directories(test_asio PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/windows/engine
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asiosdk/common
    )
    target_link_libraries(test_asio PRIVATE aq_player ee_dsp_core ole32 uuid advapi32 avrt)
    # A motor Windowsos kimenet-váltója (WASAPI / ASIO): hamis WASAPI-kimenettel és saját hamis ASIO-meghajtóval
    add_executable(test_winout src/test_winout.cpp)
    target_include_directories(test_winout PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/windows/engine
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asiosdk/common
    )
    target_link_libraries(test_winout PRIVATE aq_player ee_dsp_core ole32 uuid advapi32 avrt)
endif()
