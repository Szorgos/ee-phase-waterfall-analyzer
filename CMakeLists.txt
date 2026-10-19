cmake_minimum_required(VERSION 3.20)
project(ee_phase_waterfall_analyzer 
    VERSION 1.0.0
    DESCRIPTION "EasyEffects Real-time Phase, Group Delay & Acoustic Waterfall Analyzer"
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
    add_compile_options(-Wall -Wextra -Wpedantic -O3 -ffast-math)
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

# Opciók későbbi modulokhoz (Linux PipeWire vagy Windows VST3)
option(BUILD_PIPEWIRE_PLUGIN "PipeWire SPA modul fordítása Linuxra" OFF)
option(BUILD_VST3_PLUGIN "VST3 beépülő modul fordítása" OFF)

if(BUILD_PIPEWIRE_PLUGIN)
    message(STATUS "PipeWire SPA plugin építése engedélyezve")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
endif()

if(BUILD_VST3_PLUGIN)
    message(STATUS "VST3 plugin építése engedélyezve")
endif()

message(STATUS "EasyEffects C++ DSP Engine sikeresen konfigurálva.")
