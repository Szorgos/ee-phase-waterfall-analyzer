cmake_minimum_required(VERSION 3.20)
project(aqulify 
    VERSION 1.7.2
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

# Fordítási figyelmeztetések és optimalizációk
if(MSVC)
    add_compile_options(/W4 /O2 /fp:fast)
else()
    add_compile_options(-Wall -Wextra -O3 -ffast-math)
endif()

# MinGW alatt minden bináris statikus C++ futtatókörnyezettel készül (nincs extra DLL-függőség): lásd fent

# Fejlécek elérési útvonala
set(EE_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

# 1. DSP Mag Könyvtár (Platformfüggetlen Interface könyvtár)
add_library(aqulify_ee STATIC src/ee_dsp_lsp.cpp src/ee_dsp_misc.cpp src/ee_dsp_convolver.cpp src/rnn_denoise.cpp)
target_include_directories(aqulify_ee PUBLIC ${EE_INCLUDE_DIR})
target_link_libraries(aqulify_ee PUBLIC aq_lsp aq_calf aq_bs2b aq_speex aq_zita)
target_link_libraries(aqulify_ee PRIVATE aq_rnnoise)
set_target_properties(aqulify_ee PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
    # A könyvtárakból átvett jelfeldolgozó kód az eredetivel azonos lebegőpontos szabályokkal fusson
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
target_link_libraries(test_core PRIVATE ee_dsp_core Threads::Threads)

# 6. Tesztalkalmazás: DSP modulok (partícionált konvolver, FIR EQ, Bass Enhancer, teljes lánc)
add_executable(test_dsp src/test_dsp.cpp)
target_link_libraries(test_dsp PRIVATE ee_dsp_core Threads::Threads)

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
        add_executable(Aqulify WIN32 windows/engine/aether_engine.cpp ${EE_RES_DIR}/engine.rc)
        target_include_directories(Aqulify PRIVATE ${EE_RES_DIR} ${CMAKE_CURRENT_BINARY_DIR})
        target_link_libraries(Aqulify PRIVATE ee_dsp_core ws2_32 iphlpapi ole32 oleaut32 shell32 advapi32 user32 uuid)
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
        target_compile_definitions(AqulifyGui PRIVATE UNICODE _UNICODE)
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
            ${CMAKE_CURRENT_SOURCE_DIR}/windows/third_party/three/three.min.js
            ${CMAKE_CURRENT_SOURCE_DIR}/windows/third_party/three/LICENSE
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
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/windows/third_party/three/three.min.js ${EE_PAYLOAD_DIR}/three.bin
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/windows/third_party/three/LICENSE ${EE_PAYLOAD_DIR}/threelic.bin
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
            "FILE_THREE|gui\\three.min.js\n"
            "FILE_THREELIC|gui\\three.js-LICENSE.txt\n"
            "FILE_README|OLVASS_EL.md\n")
        file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/setup_payload.rc
            "PAYLOAD_INDEX RCDATA \"${EE_PAYLOAD_DIR}/index.txt\"\n"
            "FILE_ENGINE RCDATA \"${EE_PAYLOAD_DIR}/engine.bin\"\n"
            "FILE_GUIAPP RCDATA \"${EE_PAYLOAD_DIR}/guiapp.bin\"\n"
            "FILE_PLUGIN RCDATA \"${EE_PAYLOAD_DIR}/plugin.bin\"\n"
            "FILE_GUI RCDATA \"${EE_PAYLOAD_DIR}/gui.bin\"\n"
            "FILE_AUTOEQ RCDATA \"${EE_PAYLOAD_DIR}/autoeq.bin\"\n"
            "FILE_I18N RCDATA \"${EE_PAYLOAD_DIR}/i18n.bin\"\n"
            "FILE_THREE RCDATA \"${EE_PAYLOAD_DIR}/three.bin\"\n"
            "FILE_THREELIC RCDATA \"${EE_PAYLOAD_DIR}/threelic.bin\"\n"
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
        ${CMAKE_CURRENT_SOURCE_DIR}/windows/third_party/three/three.min.js)
    add_executable(Aqulify MACOSX_BUNDLE macos/app/main.mm macos/engine/mac_engine.mm ${AQ_MAC_GUI}
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
    target_include_directories(Aqulify PRIVATE macos/engine)
    target_compile_options(Aqulify PRIVATE -fobjc-arc)
    target_compile_definitions(Aqulify PRIVATE AQULIFY_VERSION="${PROJECT_VERSION}")
    target_link_libraries(Aqulify PRIVATE ee_dsp_core Threads::Threads
        "-framework AppKit" "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreFoundation"
        "-framework Foundation" "-framework ServiceManagement" "-framework WebKit")
    add_custom_command(TARGET Aqulify POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/windows/third_party/three/LICENSE
                $<TARGET_BUNDLE_CONTENT_DIR:Aqulify>/Resources/gui/three.js-LICENSE.txt)
endif()

# Linux: PipeWire motor (virtuális "Aqulify" kimeneti eszköz) és saját ablakos felület
option(BUILD_PIPEWIRE_PLUGIN "Aqulify PipeWire motor Linuxra" ON)
option(BUILD_LINUX_GUI "Saját ablakos felület (WebKitGTK), ha a fejlesztői csomag elérhető" ON)

if(BUILD_PIPEWIRE_PLUGIN AND NOT WIN32 AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
    add_executable(aqulify-engine src/pipewire_plugin.cpp)
    target_include_directories(aqulify-engine PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
    target_link_directories(aqulify-engine PRIVATE ${PIPEWIRE_LIBRARY_DIRS})
    target_link_libraries(aqulify-engine PRIVATE ee_dsp_core ${PIPEWIRE_LIBRARIES} Threads::Threads)
    target_compile_definitions(aqulify-engine PRIVATE AQULIFY_VERSION="${PROJECT_VERSION}")

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
    configure_file(linux/aqulify.service.in ${CMAKE_CURRENT_BINARY_DIR}/aqulify.service @ONLY)
    set(VERSION "${PROJECT_VERSION}")
    set(DATE "${AQ_RELEASE_DATE}")
    configure_file(linux/io.github.szorgos.Aqulify.metainfo.xml ${CMAKE_CURRENT_BINARY_DIR}/io.github.szorgos.Aqulify.metainfo.xml @ONLY)

    install(TARGETS aqulify-engine RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    if(TARGET aqulify-gui)
        install(TARGETS aqulify-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
    install(FILES gui_integrated_system.html autoeq_index.js gui_i18n.js windows/third_party/three/three.min.js
            DESTINATION ${CMAKE_INSTALL_DATADIR}/aqulify/gui)
    install(FILES windows/third_party/three/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/aqulify/gui RENAME three.js-LICENSE.txt)
    install(FILES linux/io.github.szorgos.Aqulify.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/io.github.szorgos.Aqulify.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
    install(FILES linux/aqulify.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/aqulify.service DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/user)
    install(FILES linux/80-aqulify.preset DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/user-preset)
endif()

message(STATUS "Aqulify C++ DSP Engine sikeresen konfigurálva.")
