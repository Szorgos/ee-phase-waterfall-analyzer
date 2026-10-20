cmake_minimum_required(VERSION 3.20)
project(aetheraudio 
    VERSION 1.3.0
    DESCRIPTION "AetherAudio - Real-time Phase, Group Delay & Acoustic Waterfall DSP Suite"
    LANGUAGES CXX
)

# C++20 szabvány megkövetelése
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Fordítási figyelmeztetések és optimalizációk
if(MSVC)
    add_compile_options(/W4 /O2 /fp:fast)
else()
    add_compile_options(-Wall -Wextra -O3 -ffast-math)
endif()

# MinGW alatt minden bináris statikus C++ futtatókörnyezettel készül (nincs extra DLL-függőség)
if(MINGW)
    add_link_options(-static)
endif()

# Fejlécek elérési útvonala
set(EE_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

# 1. DSP Mag Könyvtár (Platformfüggetlen Interface könyvtár)
add_library(ee_dsp_core INTERFACE)
target_include_directories(ee_dsp_core INTERFACE ${EE_INCLUDE_DIR})

# 2. Tesztalkalmazás: Analitikus Biquad & Fáziskalkulátor
add_executable(test_math src/test_math.cpp)
target_link_libraries(test_math PRIVATE ee_dsp_core)

# 3. Tesztalkalmazás: Teljes DSP Pipeline (EQ + Crossfeed + Limiter)
add_executable(test_pipeline src/test_pipeline.cpp)
target_link_libraries(test_pipeline PRIVATE ee_dsp_core)

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
    # Windows: AetherAudio.exe motor + AetherAudio.dll (Equalizer APO plugin, VST 2.4 ABI)
    # ------------------------------------------------------------------
    option(BUILD_WINDOWS_ENGINE "AetherAudio.exe Windows motor (WASAPI loopback + plugin híd)" ON)
    option(BUILD_EQAPO_PLUGIN "AetherAudio.dll Equalizer APO plugin (VST 2.4 bináris interfész)" ON)
    set(BUILD_PIPEWIRE_PLUGIN OFF CACHE BOOL "" FORCE)

    enable_language(RC)
    set(EE_RES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/windows/resources)
    string(REPLACE "." "," AA_VER_COMMA "${PROJECT_VERSION},0")

    # Verzióinformáció fájlonként
    function(ee_version_rc out name file desc filetype)
        set(AA_NAME "${name}")
        set(AA_FILE "${file}")
        set(AA_DESC "${desc}")
        set(AA_FILETYPE "${filetype}")
        configure_file(${EE_RES_DIR}/version.rc.in ${CMAKE_CURRENT_BINARY_DIR}/${out} @ONLY)
    endfunction()
    ee_version_rc(engine_version.rc "AetherAudio" "AetherAudio.exe" "AetherAudio motor (tálcás háttéralkalmazás)" "VFT_APP")
    ee_version_rc(plugin_version.rc "AetherAudio" "AetherAudio.dll" "AetherAudio Equalizer APO plugin (VST 2.4)" "VFT_DLL")
    ee_version_rc(gui_version.rc "AetherAudio" "AetherAudioGui.exe" "AetherAudio felület (WebView2 ablak)" "VFT_APP")
    ee_version_rc(setup_version.rc "AetherAudio-Setup" "AetherAudio-Setup.exe" "AetherAudio telepítő" "VFT_APP")

    if(BUILD_WINDOWS_ENGINE)
        add_executable(AetherAudio WIN32 windows/engine/aether_engine.cpp ${EE_RES_DIR}/engine.rc)
        target_include_directories(AetherAudio PRIVATE ${EE_RES_DIR} ${CMAKE_CURRENT_BINARY_DIR})
        target_link_libraries(AetherAudio PRIVATE ee_dsp_core ws2_32 iphlpapi ole32 oleaut32 shell32 advapi32 user32 uuid)
        target_compile_definitions(AetherAudio PRIVATE UNICODE _UNICODE)
        if(MSVC)
            target_link_options(AetherAudio PRIVATE /ENTRY:mainCRTStartup /MANIFEST:NO)
            set_property(TARGET AetherAudio PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
        endif()
    endif()

    # Grafikus felület saját ablakban (WebView2; a webview/webview 0.12.0 beépített betöltőjével, WebView2Loader.dll nélkül)
    if(BUILD_WINDOWS_ENGINE)
        add_executable(AetherAudioGui WIN32 windows/gui/aether_gui.cpp ${EE_RES_DIR}/gui.rc)
        target_include_directories(AetherAudioGui PRIVATE ${EE_RES_DIR} ${CMAKE_CURRENT_BINARY_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/windows/third_party ${CMAKE_CURRENT_SOURCE_DIR}/windows/third_party/webview2)
        target_compile_definitions(AetherAudioGui PRIVATE UNICODE _UNICODE)
        target_link_libraries(AetherAudioGui PRIVATE advapi32 ole32 oleaut32 shell32 shlwapi user32 version dwmapi uuid)
        if(MINGW)
            target_link_options(AetherAudioGui PRIVATE -municode -static)
        endif()
        if(MSVC)
            target_link_options(AetherAudioGui PRIVATE /MANIFEST:NO)
            set_property(TARGET AetherAudioGui PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
        endif()
    endif()

    option(BUILD_WINDOWS_SETUP "AetherAudio-Setup.exe telepítő (a motort és a plugint is beágyazza)" ON)

    if(BUILD_EQAPO_PLUGIN)
        add_library(AetherAudioPlugin SHARED windows/plugin/aether_vst2.cpp windows/plugin/aether_vst2.def ${EE_RES_DIR}/plugin.rc)
        target_include_directories(AetherAudioPlugin PRIVATE ${EE_RES_DIR} ${CMAKE_CURRENT_BINARY_DIR})
        set_target_properties(AetherAudioPlugin PROPERTIES OUTPUT_NAME "AetherAudio" PREFIX "")
        target_link_libraries(AetherAudioPlugin PRIVATE ee_dsp_core advapi32)
        if(MSVC)
            # Statikus MSVC futtatókörnyezet (/MT): az audiodg.exe-be töltött DLL ne függjön a VC++ redisttől
            set_property(TARGET AetherAudioPlugin PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
        endif()
    endif()

    # Teszt-hoszt: az Equalizer APO betöltési sorrendjét utánozza (a plugin önálló kipróbálásához)
    add_executable(eqapo_host_sim windows/tools/eqapo_host_sim.cpp)

    if(BUILD_WINDOWS_SETUP AND BUILD_WINDOWS_ENGINE AND BUILD_EQAPO_PLUGIN)
        # A beágyazandó fájlok egy átmeneti mappába kerülnek; a .rc fix útvonalakkal hivatkozik rájuk
        set(EE_PAYLOAD_DIR ${CMAKE_CURRENT_BINARY_DIR}/payload)
        set(EE_PAYLOAD_SOURCES
            ${CMAKE_CURRENT_SOURCE_DIR}/gui_integrated_system.html
            ${CMAKE_CURRENT_SOURCE_DIR}/autoeq_index.js
            ${CMAKE_CURRENT_SOURCE_DIR}/windows/third_party/three/three.min.js
            ${CMAKE_CURRENT_SOURCE_DIR}/windows/third_party/three/LICENSE
            ${CMAKE_CURRENT_SOURCE_DIR}/windows/README_WINDOWS.md)
        add_custom_command(
            OUTPUT ${EE_PAYLOAD_DIR}/payload.stamp
            COMMAND ${CMAKE_COMMAND} -E make_directory ${EE_PAYLOAD_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:AetherAudio> ${EE_PAYLOAD_DIR}/engine.bin
            COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:AetherAudioPlugin> ${EE_PAYLOAD_DIR}/plugin.bin
            COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:AetherAudioGui> ${EE_PAYLOAD_DIR}/guiapp.bin
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/gui_integrated_system.html ${EE_PAYLOAD_DIR}/gui.bin
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/autoeq_index.js ${EE_PAYLOAD_DIR}/autoeq.bin
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/windows/third_party/three/three.min.js ${EE_PAYLOAD_DIR}/three.bin
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/windows/third_party/three/LICENSE ${EE_PAYLOAD_DIR}/threelic.bin
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/windows/README_WINDOWS.md ${EE_PAYLOAD_DIR}/readme.bin
            COMMAND ${CMAKE_COMMAND} -E touch ${EE_PAYLOAD_DIR}/payload.stamp
            DEPENDS AetherAudio AetherAudioPlugin AetherAudioGui ${EE_PAYLOAD_SOURCES}
            COMMENT "Telepítő csomag összeállítása")
        file(WRITE ${EE_PAYLOAD_DIR}/index.txt
            "FILE_ENGINE|AetherAudio.exe\n"
            "FILE_GUIAPP|AetherAudioGui.exe\n"
            "FILE_PLUGIN|plugin\\AetherAudio.dll\n"
            "FILE_GUI|gui\\gui_integrated_system.html\n"
            "FILE_AUTOEQ|gui\\autoeq_index.js\n"
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
            "FILE_THREE RCDATA \"${EE_PAYLOAD_DIR}/three.bin\"\n"
            "FILE_THREELIC RCDATA \"${EE_PAYLOAD_DIR}/threelic.bin\"\n"
            "FILE_README RCDATA \"${EE_PAYLOAD_DIR}/readme.bin\"\n")
        set_source_files_properties(${EE_RES_DIR}/setup.rc PROPERTIES OBJECT_DEPENDS ${EE_PAYLOAD_DIR}/payload.stamp)
        add_custom_target(AetherAudioPayload DEPENDS ${EE_PAYLOAD_DIR}/payload.stamp)

        add_executable(AetherAudioSetup WIN32 windows/installer/setup.cpp ${EE_RES_DIR}/setup.rc)
        add_dependencies(AetherAudioSetup AetherAudioPayload)
        set_target_properties(AetherAudioSetup PROPERTIES OUTPUT_NAME "AetherAudio-Setup")
        target_include_directories(AetherAudioSetup PRIVATE ${EE_RES_DIR} ${CMAKE_CURRENT_BINARY_DIR})
        target_compile_definitions(AetherAudioSetup PRIVATE UNICODE _UNICODE)
        target_link_libraries(AetherAudioSetup PRIVATE comctl32 ole32 oleaut32 shell32 shlwapi winhttp advapi32 user32 gdi32 uuid)
        if(MINGW)
            target_link_options(AetherAudioSetup PRIVATE -municode)
        endif()
        if(MSVC)
            target_link_options(AetherAudioSetup PRIVATE /MANIFEST:NO)
            set_property(TARGET AetherAudioSetup PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
        endif()
    endif()
endif()

# Opciók későbbi modulokhoz (Linux PipeWire)
option(BUILD_PIPEWIRE_PLUGIN "PipeWire SPA modul fordítása Linuxra" ON)

if(BUILD_PIPEWIRE_PLUGIN AND NOT WIN32)
    message(STATUS "PipeWire SPA plugin építése engedélyezve")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
    if(PIPEWIRE_FOUND)
        add_executable(pipewire_plugin src/pipewire_plugin.cpp)
        target_include_directories(pipewire_plugin PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
        target_link_libraries(pipewire_plugin PRIVATE ee_dsp_core ${PIPEWIRE_LIBRARIES} Threads::Threads)
    endif()
endif()

message(STATUS "AetherAudio C++ DSP Engine sikeresen konfigurálva.")
