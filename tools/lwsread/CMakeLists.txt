cmake_minimum_required(VERSION 3.16)
project(lwsread CXX)

# Standalone host tool — LightWave (.lws/.lwo) -> Flood (.fld) scene converter.
#
# Ported from Original/READERS/LWREAD (1998 DOS / 2020 MSVC "LWS_Reader" solution)
# to native POSIX / LP64. The 2020 port already de-DOSified the sources; the
# remaining gap was that the on-disk chunk length was typed `unsigned long`,
# which is 8 bytes under LP64 (macOS/Linux) but 4 bytes on the original 32-bit
# target — see LWOREAD.CPP ChunkType. Fixed to a fixed-width uint32_t.
#
# Decoupled from the demo build on purpose (no SDL/FDS deps). Build with:
#   cmake -S tools/lwsread -B tools/lwsread/build
#   cmake --build tools/lwsread/build
# Use:
#   ./lwsread MyScene.LWS NewScene.FLD     # .lwo objects must be in the CWD

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(lwsread
    LWSREAD.CPP
    LWOREAD.CPP
    FLDSAVE.CPP
)
