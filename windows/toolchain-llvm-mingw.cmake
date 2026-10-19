# Keresztfordítás Linuxról Windowsra (llvm-mingw, x86_64, UCRT).
# Használat:
#   cmake -B build-win -S . -DCMAKE_TOOLCHAIN_FILE=windows/toolchain-llvm-mingw.cmake -DLLVM_MINGW_ROOT=/opt/llvm-mingw
#   cmake --build build-win
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
if(NOT LLVM_MINGW_ROOT)
    set(LLVM_MINGW_ROOT "/opt/llvm-mingw")
endif()
set(CMAKE_C_COMPILER   "${LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-clang")
set(CMAKE_CXX_COMPILER "${LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-clang++")
set(CMAKE_RC_COMPILER  "${LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-windres")
set(CMAKE_FIND_ROOT_PATH "${LLVM_MINGW_ROOT}/x86_64-w64-mingw32")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
