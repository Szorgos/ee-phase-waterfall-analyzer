cmake_minimum_required(VERSION 3.20)
project(aetheraudio 
    VERSION 1.0.0
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

if(WIN32)
    # ------------------------------------------------------------------
    # Windows: AetherAudio.exe motor + AetherAudio.dll (Equalizer APO plugin, VST 2.4 ABI)
    # ------------------------------------------------------------------
    option(BUILD_WINDOWS_ENGINE "AetherAudio.exe Windows motor (WASAPI loopback + plugin híd)" ON)
    option(BUILD_EQAPO_PLUGIN "AetherAudio.dll Equalizer APO plugin (VST 2.4 bináris interfész)" ON)
    set(BUILD_PIPEWIRE_PLUGIN OFF CACHE BOOL "" FORCE)

    if(BUILD_WINDOWS_ENGINE)
        add_executable(AetherAudio windows/engine/aether_engine.cpp)
        target_link_libraries(AetherAudio PRIVATE ee_dsp_core ws2_32 ole32 oleaut32 shell32 advapi32)
        target_compile_definitions(AetherAudio PRIVATE UNICODE _UNICODE)
    endif()

    if(BUILD_EQAPO_PLUGIN)
        add_library(AetherAudioPlugin SHARED windows/plugin/aether_vst2.cpp windows/plugin/aether_vst2.def)
        set_target_properties(AetherAudioPlugin PROPERTIES OUTPUT_NAME "AetherAudio" PREFIX "")
        target_link_libraries(AetherAudioPlugin PRIVATE ee_dsp_core advapi32)
        if(MSVC)
            # Statikus MSVC futtatókörnyezet (/MT)
            set_property(TARGET AetherAudioPlugin PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
            set_property(TARGET AetherAudio PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
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
