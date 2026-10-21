# Keresztfordítás Linuxról 64 bites ARM Linuxra (pl. Raspberry Pi 4 / 5, 64 bites Raspberry Pi OS).
# Debian / Ubuntu gépen:
#   sudo apt install g++-aarch64-linux-gnu
#   sudo dpkg --add-architecture arm64   (Ubuntun az arm64 csomagok a ports.ubuntu.com tárolóból jönnek)
#   sudo apt install libfftw3-dev:arm64 libpipewire-0.3-dev:arm64
#   cmake -B build-arm64 -S . -DCMAKE_TOOLCHAIN_FILE=linux/toolchain-aarch64-linux-gnu.cmake -DBUILD_LINUX_GUI=OFF
#   cmake --build build-arm64
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu /usr/lib/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
# a pkg-config az arm64 csomagok .pc fájljait keresse
set(ENV{PKG_CONFIG_LIBDIR} /usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig)
