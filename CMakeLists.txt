cmake_minimum_required(VERSION 3.16)

# CMP0069 NEW: honor CMAKE_INTERPROCEDURAL_OPTIMIZATION on all generators /
# compilers (clang/AppleClang/GCC), not just Intel. Required for the
# check_ipo_supported() probe + LTO enable below to actually take effect.
cmake_policy(SET CMP0069 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)

project(REVIVAL CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Thin LTO for Release / RelWithDebInfo only. Cross-TU inlining matters for
# the rasterizer + deferred lighting kernels that span FDS and DEMO. Skip
# Debug (LTO debug experience is poor) and Emscripten (wasm LTO has been
# finicky in this tree; enable later once we have a reason). Use the
# per-config CMAKE_INTERPROCEDURAL_OPTIMIZATION_<CONFIG> variables so a
# single-config Debug build never picks LTO up by accident.
if(NOT EMSCRIPTEN)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _ipo_supported OUTPUT _ipo_error)
    if(_ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
        message(STATUS "IPO/LTO enabled for Release / RelWithDebInfo")
    else()
        message(STATUS "IPO/LTO not supported by this toolchain: ${_ipo_error}")
    endif()
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    # Default to Release (-O3 -DNDEBUG) for max perf. Pair with thin LTO
    # (enabled above) and optional PGO (PGO_PHASE cache var). Pass
    # -DCMAKE_BUILD_TYPE=RelWithDebInfo at configure time when you need
    # debug info for lldb / sample / instruction-level profiling.
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "macOS architectures to build for")
endif()

option(WASM_PROFILE "Emit DWARF debug info in the wasm build for Chrome DevTools profiling" OFF)

# FDS_DEV gates dev-only flags and their call sites (viz/dump/debug helpers).
# Independent of CMAKE_BUILD_TYPE — release-with-FDS_DEV is the intended dev
# workflow (optimized + helpers); release-without-FDS_DEV is the stripped
# ship build. Default ON for Debug / RelWithDebInfo, OFF for Release.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(_FDS_DEV_DEFAULT OFF)
else()
    set(_FDS_DEV_DEFAULT ON)
endif()
option(FDS_DEV "Compile dev-only flags and their call sites" ${_FDS_DEV_DEFAULT})
if(FDS_DEV)
    add_compile_definitions(FDS_DEV=1)
else()
    add_compile_definitions(FDS_DEV=0)
endif()

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

# Cross-statement FMA contraction enabled globally. Unlocks vector
# FMLA on arm64 NEON for `a*b+c` chains in hot loops (per-vertex
# transform, lighting kernels, deferred shading, rasterizers).
# Saves ~1.5ms on greets bench at 1080p (originally 7e4c2ac).
#
# History: 7e4c2ac originally produced a city horizon shimmer that
# was misattributed to FMA-fused rasterizer per-pixel math. The real
# cause was a latent NaN bug in TheOtherBarry's forward fog code —
# `sqrtf(1 - invFarZ * z)` of a slightly-negative argument returns
# NaN (not negative), the `if (fogRate < 0)` check is a no-op against
# NaN, and the NaN propagates into vertex color → garbage at the
# horizon (where z ≈ farZ). FMA shifted the argument by 1 ULP at
# exactly that boundary, which is why it appeared FMA-related. Fixed
# in TheOtherBarry.h by gating the argument before sqrt (mirrors
# DeferredLighting.cpp:2099). Block-scoped FP_CONTRACT_OFF macros
# in FDS/Base/FPContract.h are preserved as infrastructure for any
# future real-FMA-sensitive site that's properly identified first.
add_compile_options(-ffp-contract=fast)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# CTest entry points — populated by tests/CMakeLists.txt + smoke checks
# below. Skip on emscripten (DEMO is a wasm artifact, not a runnable
# native binary, and the snapshot CLI doesn't make sense in the
# browser).
if(NOT EMSCRIPTEN)
    enable_testing()
endif()

add_subdirectory(Modplayer)
add_subdirectory(FDS)
add_subdirectory(DEMO)
add_subdirectory(tests)

# Smoke test: exercise the engine init path (FLD load, material census,
# cube-map bake, deferred + shadow rendering) via the existing snapshot
# CLI. Catches regressions that snapshot byte-identity misses — for
# instance the phase-6 `tVerts = tVerts;` self-assign that crashed
# RenderSkyCube but produced the expected PPM at city@t=1500.
#
# Each test writes its output to a unique CTest-managed temp dir, then
# asserts the PPM landed. A crash / hang during init fails the test.
if(NOT EMSCRIPTEN)
    set(SMOKE_OUTDIR "${CMAKE_BINARY_DIR}/smoke-out")
    add_test(NAME smoke_city_forward
        COMMAND DEMO --snapshot=city@t=1500 --out=${SMOKE_OUTDIR}/city_fwd
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/Runtime)
    set_tests_properties(smoke_city_forward PROPERTIES
        TIMEOUT 60
        PASS_REGULAR_EXPRESSION "wrote.*city_t001500_color\\.ppm")

    add_test(NAME smoke_greets_deferred_shadows
        COMMAND DEMO --snapshot=greets@t=2500 --out=${SMOKE_OUTDIR}/greets_def
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/Runtime)
    set_tests_properties(smoke_greets_deferred_shadows PROPERTIES
        TIMEOUT 60
        ENVIRONMENT "FDS_DEFERRED=1;FDS_SHADOWS=1"
        PASS_REGULAR_EXPRESSION "GREETSSNAP.*greets_t002500_color\\.ppm")
endif()
