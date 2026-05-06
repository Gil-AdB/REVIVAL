cmake_minimum_required(VERSION 3.16)

project(REVIVAL CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "macOS architectures to build for")
endif()

option(WASM_PROFILE "Emit DWARF debug info in the wasm build for Chrome DevTools profiling" OFF)

# Selects a stubbed variant of TheOtherBarry::apply_exact for the synthetic
# rasterizer bench. `full` (default) is the production rasterizer; the others
# stub one inner-loop op so we can isolate its cost via `make bench-...`.
set(BENCH_VARIANT "full" CACHE STRING
    "Rasterizer bench variant: full|no-texture|no-perspective|no-maskstore|no-z|no-color")
set_property(CACHE BENCH_VARIANT PROPERTY STRINGS
    full no-texture no-perspective no-maskstore no-z no-color)
if(BENCH_VARIANT STREQUAL "no-texture")
    add_compile_definitions(BENCH_SKIP_TEXTURE=1)
elseif(BENCH_VARIANT STREQUAL "no-perspective")
    add_compile_definitions(BENCH_SKIP_PERSPECTIVE=1)
elseif(BENCH_VARIANT STREQUAL "no-maskstore")
    add_compile_definitions(BENCH_SKIP_MASKSTORE=1)
elseif(BENCH_VARIANT STREQUAL "no-z")
    add_compile_definitions(BENCH_SKIP_Z=1)
elseif(BENCH_VARIANT STREQUAL "no-color")
    add_compile_definitions(BENCH_SKIP_COLOR=1)
elseif(NOT BENCH_VARIANT STREQUAL "full")
    message(FATAL_ERROR "BENCH_VARIANT must be one of: full, no-texture, no-perspective, no-maskstore, no-z, no-color (got '${BENCH_VARIANT}')")
endif()

if(EMSCRIPTEN)
    add_compile_options(-msimd128 -sUSE_SDL=2)
    # The vendored Agner Fog vectorclass checks `EMSCRIPTEN` (no underscores)
    # to skip x86-specific inline asm and __m128/__m128i/__m128d overloads
    # that collapse when simde maps them all to the same wasm32 type. emcc
    # only defines `__EMSCRIPTEN__` itself, so we define the plain form too.
    add_compile_definitions(EMSCRIPTEN)
    if(APPLE)
        add_definitions(-DEMSCRIPTEN_MACOS=1)
    endif()
    if(WASM_PROFILE)
        # Profiling build with both:
        #   - C++ function names in the wasm `name` section, used by Chrome's
        #     Performance panel for flame chart labels
        #   - Full DWARF in a sidecar `.debug.wasm` file, picked up by the
        #     "C/C++ DevTools Support (DWARF)" extension for source-line
        #     attribution + variable inspection (does not bloat the main wasm)
        #
        # Chrome's Performance panel switches to "DWARF mode" when it sees
        # `.debug_*` sections embedded in the main wasm and stops reading
        # the `name` section directly — if DWARF resolution then fails for
        # any reason, the flame chart shows numeric function indices.
        # Putting DWARF in a sidecar avoids that switch.
        #
        # We deliberately do NOT prefix-map source paths: leave them
        # absolute. The right way to resolve them in DevTools is via
        # Sources → Workspace → "Add folder", which maps the absolute
        # DWARF paths to the local repo. Trying to make the paths
        # HTTP-resolvable produces awkward URLs the python http.server
        # can't satisfy.
        add_compile_options(-g3)
        add_link_options(-gseparate-dwarf --profiling-funcs)
    endif()
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

add_subdirectory(Modplayer)
add_subdirectory(FDS)
add_subdirectory(DEMO)
