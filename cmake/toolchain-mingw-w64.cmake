# CMake toolchain for cross-compiling REVIVAL to 64-bit Windows with
# MinGW-w64 (x86_64-w64-mingw32) from macOS or Linux.
#
#   brew install mingw-w64            # macOS
#   rustup target add x86_64-pc-windows-gnu
#
# Then point CMAKE_PREFIX_PATH at the x86_64-w64-mingw32 subtree of the
# official SDL2 MinGW development package
# (SDL2-devel-<ver>-mingw.tar.gz from github.com/libsdl-org/SDL/releases):
#
#   cmake -S . -B build-win -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64.cmake \
#     -DCMAKE_PREFIX_PATH=/path/to/SDL2-2.32.10/x86_64-w64-mingw32
#
# See docs/BUILDING_WINDOWS.md (Path B) for the full recipe.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32 CACHE STRING "MinGW-w64 target triple")

find_program(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc REQUIRED)
find_program(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++ REQUIRED)
find_program(CMAKE_RC_COMPILER  ${TOOLCHAIN_PREFIX}-windres)

# Rust cross target that matches this ABI (Modplayer/CMakeLists.txt reads it).
set(RUST_TARGET "x86_64-pc-windows-gnu" CACHE STRING "cargo --target triple")

# Search host paths for programs (cmake, ninja, cargo) but only the target
# sysroot + CMAKE_PREFIX_PATH for headers and libraries.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BEFORE)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
