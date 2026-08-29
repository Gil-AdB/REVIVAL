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

# Visibility research instruments (--xfrm_pass_prof, --mirror_cull_census;
# docs/VISIBILITY_PLAN.md §8). COMPILE-TIME gated, not just flag-gated: this
# build is -O3 -flto -ffp-contract=fast, and merely carrying the never-taken
# branches inside Transform_Objects measurably changes which expressions the
# compiler contracts into FMAs in the surrounding vertex/face work — the city
# pin moved with every flag OFF. With this OFF the preprocessor removes them
# and Transform_Objects is textually identical to the un-instrumented one.
# Measure with:  cmake -S . -B build-census -G Ninja -DFDS_VIS_CENSUS=ON
option(FDS_VIS_CENSUS "Compile the per-pass / mirror-clone visibility census instruments" OFF)
if(FDS_VIS_CENSUS)
    add_compile_definitions(FDS_VIS_CENSUS=1)
endif()

# The mirror-shard reflection bake's PER-FACE cone cull (--shard_cone_cull=2,
# FDS/RENDER/ReflFaceCull.cpp). COMPILE-TIME gated for exactly the reason above,
# and the reason is measured, not assumed: the cull itself is byte-identical to
# no cull (verified on the shard atlas AND the break+1 frame), but merely
# CARRYING its never-taken call inside Transform_Objects' per-mesh body moved
# ALL THREE scene pins — greets 778fa6ac→7a6370a1, fountain 8db68ccb→eebf68e6,
# city 3cbe42b1→80583b85 — on snapshots that never shatter a mirror. Bisected:
# the branch it adds to the face loop is byte-null, the CALL is not, at either
# call site tried. And it is not worth a re-pin: measured min-of-6 interleaved,
# the cull removes 12.5%% of the bake's face-list pushes and 0.1 ms of its
# 14-15 ms, because the bake is 77%% deferred lighting of the reflection's own
# pixels and only 4.5%% geometry front-end (see docs/OPTIMIZATION_BACKLOG.md).
# The same gate carries the [SHARD-PHASE] per-phase attribution that PROVED
# the verdict above, for the same reason: with the phase clocks present the
# shard atlas itself drifts (365 px of 1 048 576, FP-contraction noise, no
# pin covers it) — so with the gate OFF the whole shard bake is byte-
# identical to its parent, and with it ON every number in the commit
# message reproduces.
# Measure with:  cmake -S . -B build-lab -G Ninja -DFDS_SHARD_BAKE_LAB=ON
option(FDS_SHARD_BAKE_LAB "Compile the mirror-shard bake laboratory: the per-face cone cull (--shard_cone_cull=2) + the per-phase attribution" OFF)
if(FDS_SHARD_BAKE_LAB)
    add_compile_definitions(FDS_SHARD_BAKE_LAB=1)
endif()

# --shadow_tap_census's counter hooks sit in the deferred light loop's two
# innermost bodies (per pixel x light, per cube tap). Even fully predicted and
# never taken they cost ~1 % of lighting-w1's instructions, because the extra
# live state perturbs register allocation in the hottest loop in the engine —
# the same mechanism d9248f6d measured for the cube tap's FDS_DEV abort branch.
# So the hooks are COMPILED OUT by default and the shipping kernel is
# instruction-identical to the pre-census one (verified: renderFrame 5.167
# Ginstr/f both, greets t=3122 1512x848). The flag stays registered either way
# and says how to get the build when it cannot do anything.
# Measure with:  cmake -S . -B build-census -G Ninja -DFDS_SHADOW_TAP_CENSUS=ON
option(FDS_SHADOW_TAP_CENSUS "Compile the deferred shadow-tap census hooks (--shadow_tap_census / --shadow_tap_census_block)" OFF)
if(FDS_SHADOW_TAP_CENSUS)
    add_compile_definitions(FDS_SHADOW_TAP_CENSUS=1)
else()
    add_compile_definitions(FDS_SHADOW_TAP_CENSUS=0)
endif()

# --omni_census counts per (pixel x light) instead of per (tile x light): where
# in the reject chain each light dies, and the live-lights-per-pixel histogram.
# Same register-pressure reasoning as the tap census above, same default.
# Measure with:  cmake -S . -B build-census -G Ninja -DFDS_OMNI_CENSUS=ON
option(FDS_OMNI_CENSUS "Compile the deferred omni-loop per-pixel census hooks (--omni_census)" OFF)
if(FDS_OMNI_CENSUS)
    add_compile_definitions(FDS_OMNI_CENSUS=1)
else()
    add_compile_definitions(FDS_OMNI_CENSUS=0)
endif()

# Wave-2 (checkerboard fill) census: where each odd cell goes — averaged,
# full-shade fallback, or dropped. Shares the --omni_census runtime gate.
# OuterVec wave-1 instruments (city / fountain / crash). Same compile-time shape
# and the same reason: a runtime predicate in this pixel body costs about what a
# micro-mechanism saves (2026-08-16h/16l).
#   cmake -S . -B build-ovc -G Ninja -DFDS_OVEC_CENSUS=ON      (runtime --omni_census)
#   cmake -S . -B build-abl -G Ninja -DFDS_OVEC_ABLATE=8       (per-group ladder)
#   cmake -S . -B build-abl -G Ninja -DFDS_OVEC_OMNI_ABLATE=3  (per-light ladder)
set(FDS_OVEC_ENVDIAG "0" CACHE STRING "OuterVec env-lane cost ladder stage (0 = shipping, BYTE-CHANGING otherwise)")
add_compile_definitions(FDS_OVEC_ENVDIAG=${FDS_OVEC_ENVDIAG})
set(FDS_SSAO_DIAG "0" CACHE STRING "GTAO slice-setup cost ladder (0 = shipping, BYTE-CHANGING otherwise)")
add_compile_definitions(FDS_SSAO_DIAG=${FDS_SSAO_DIAG})
option(FDS_SSAO_VECGATHER "Vectorise the GTAO depth gather (REFUTED: -9.8% instructions, +2.5% cycles)" OFF)
if(FDS_SSAO_VECGATHER)
    add_compile_definitions(FDS_SSAO_VECGATHER=1)
else()
    add_compile_definitions(FDS_SSAO_VECGATHER=0)
endif()
option(FDS_SSAO_VERIFY "Compile the GTAO slice-setup scalar-behind-vector verify (--omni_census)" OFF)
if(FDS_SSAO_VERIFY)
    add_compile_definitions(FDS_SSAO_VERIFY=1)
else()
    add_compile_definitions(FDS_SSAO_VERIFY=0)
endif()
option(FDS_SSAO_CENSUS "Compile the GTAO march census hooks (--omni_census)" OFF)
if(FDS_SSAO_CENSUS)
    add_compile_definitions(FDS_SSAO_CENSUS=1)
else()
    add_compile_definitions(FDS_SSAO_CENSUS=0)
endif()
option(FDS_OVEC_HATCH "Keep the OuterVec C3/C4/C7 levers as runtime flags (measurement only)" OFF)
if(FDS_OVEC_HATCH)
    add_compile_definitions(FDS_OVEC_HATCH=1)
else()
    add_compile_definitions(FDS_OVEC_HATCH=0)
endif()
option(FDS_OVEC_CENSUS "Compile the OuterVec wave-1 census hooks (--omni_census)" OFF)
if(FDS_OVEC_CENSUS)
    add_compile_definitions(FDS_OVEC_CENSUS=1)
else()
    add_compile_definitions(FDS_OVEC_CENSUS=0)
endif()
set(FDS_OVEC_ABLATE "0" CACHE STRING "OuterVec per-group ablation stage (0 = shipping)")
add_compile_definitions(FDS_OVEC_ABLATE=${FDS_OVEC_ABLATE})
set(FDS_OVEC_OMNI_ABLATE "0" CACHE STRING "OuterVec per-light ablation stage (0 = shipping)")
add_compile_definitions(FDS_OVEC_OMNI_ABLATE=${FDS_OVEC_OMNI_ABLATE})

option(FDS_W2_CENSUS "Compile the deferred wave-2 fill census hooks (--omni_census)" OFF)
if(FDS_W2_CENSUS)
    add_compile_definitions(FDS_W2_CENSUS=1)
else()
    add_compile_definitions(FDS_W2_CENSUS=0)
endif()

# Wave-2 fill oct-decode pairing (2026-08-16o). MODE 4 = one 4-wide
# meka::oct_decode_u32_x4 for the checkerboard cell's two NEIGHBOURS and its
# CENTRE; 2 = the two neighbours only (2026-08-16h's named candidate); 0 = the
# parent's three scalar decodes. HATCH=ON keeps --deferred_fill_oct_pair as a
# runtime flag so a round can price parent / OFF / ON — the shipping form is
# OFF (flagless), because a flag in this pixel body costs what it saves.
set(FDS_W2_OCTPAIR_MODE "4" CACHE STRING "Wave-2 fill oct-decode pairing width: 0|2|4")
set_property(CACHE FDS_W2_OCTPAIR_MODE PROPERTY STRINGS 0 2 4)
add_compile_definitions(FDS_W2_OCTPAIR_MODE=${FDS_W2_OCTPAIR_MODE})
option(FDS_W2_OCTPAIR_HATCH "Keep --deferred_fill_oct_pair as a runtime flag (measurement only)" OFF)
if(FDS_W2_OCTPAIR_HATCH)
    add_compile_definitions(FDS_W2_OCTPAIR_HATCH=1)
else()
    add_compile_definitions(FDS_W2_OCTPAIR_HATCH=0)
endif()
# Runs the SCALAR oct_decode_u32 behind every lane of the wide one and counts
# bit-pattern disagreements (--omni_census gates the print). The landing bar is
# 0 mismatches. Compile-time for the same reason the wave-2 census is.
option(FDS_W2_OCTPAIR_VERIFY "Compile the wave-2 oct-pair bit-pattern verify counters (--omni_census)" OFF)
if(FDS_W2_OCTPAIR_VERIFY)
    add_compile_definitions(FDS_W2_OCTPAIR_VERIFY=1)
else()
    add_compile_definitions(FDS_W2_OCTPAIR_VERIFY=0)
endif()

# --shadow_bbox_cull's DIRECT verify, and the reason the flag could be defaulted
# ON. The reject's byte-exactness argument is "a face whose screen bbox misses
# this tile's rect produces no polygon here" — this build makes the walk compute
# the reject but NOT apply it, then counts, per face, whether the clipper handed
# MekaleleShadowDepth a polygon anyway. A single non-zero is a counter-example.
# Compile-time because the probe sits at the top of the shadow raster, which is
# called ~40 000 times a frame at greets. Landing bar is 0 violations.
option(FDS_SHADOW_BBOX_VERIFY "Compile the shadow bbox pre-reject counter-example probe" OFF)
if(FDS_SHADOW_BBOX_VERIFY)
    add_compile_definitions(FDS_SHADOW_BBOX_VERIFY=1)
else()
    add_compile_definitions(FDS_SHADOW_BBOX_VERIFY=0)
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
#
# PORTABILITY: MSVC's driver spells this /fp:contract; clang-cl takes the
# clang spelling only through the /clang: passthrough. The plain form below
# is what AppleClang, ordinary clang and GCC (incl. MinGW-w64) all take, so
# the macOS command line is unchanged.
if(MSVC AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_compile_options(/clang:-ffp-contract=fast)
    else()
        add_compile_options(/fp:contract)
    endif()
else()
    add_compile_options(-ffp-contract=fast)
endif()

# ----------------------------------------------------------------------------
# x86-64 instruction set floor: AVX2 + FMA.
#
# The rasterizers and the deferred lighting kernels are written against
# `_mm256_*` intrinsics and Agner Fog's vectorclass. On arm64 those reach
# NEON through the vendored simde shim, which needs no flag. On x86-64
# SIMDE_ENABLE_NATIVE_ALIASES resolves them to the REAL Intel intrinsics,
# and GCC/clang refuse to inline an AVX2 intrinsic into a function compiled
# without the corresponding -m flag ("target specific option mismatch"), so
# an x86-64 build does not merely run slower without this — it does not
# compile. MSVC does not gate intrinsics that way, but /arch:AVX2 is still
# what lets it emit VEX encodings (and it implies FMA, which is why the
# GCC/clang arm asks for -mfma alongside -mavx2).
#
# Consequence for the shipped binary: an x86-64 build requires a Haswell
# (2013) or newer CPU. That is already true of the engine's design.
# arm64 (Apple Silicon, Windows-on-ARM) is untouched by this block.
if(NOT EMSCRIPTEN)
    set(_fds_x86 FALSE)
    if(APPLE)
        # Multi-arch capable; key off the requested architectures.
        if(CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
            set(_fds_x86 TRUE)
        endif()
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|x64)$")
        set(_fds_x86 TRUE)
    endif()

    if(_fds_x86)
        if(MSVC AND NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
            add_compile_options(/arch:AVX2)
        else()
            add_compile_options(-mavx2 -mfma)
        endif()
        message(STATUS "x86-64 target: AVX2 + FMA baseline enabled")
    endif()
endif()

# Windows hygiene — applies to MSVC, clang-cl AND MinGW-w64 alike.
if(WIN32)
    # windows.h is reachable from DEMO/REV.CPP (GetModuleFileNameA) and from
    # SDL.h. Its min/max macros collide with std::min/std::max, which this
    # tree uses everywhere; M_PI is not declared by <cmath> on MSVC without
    # _USE_MATH_DEFINES.
    # NOGDI: wingdi.h #defines TRANSPARENT and OPAQUE, which collide with
    # barry::TBlendMode's enumerators of the same name and turn every
    # TheOtherBarry<> instantiation into a parse error. Nothing in this tree
    # uses GDI. NOMINMAX: windows.h's min/max macros vs std::min/std::max.
    # _USE_MATH_DEFINES: MSVC's <cmath> hides M_PI without it (EnvBake.cpp).
    add_compile_definitions(NOMINMAX NOGDI WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES)
    # SDL2 on Windows #defines main to SDL_main and expects to link
    # SDL2main's WinMain. DEMO's main() is `int main(int, const char**)` —
    # a signature SDL_main's prototype rejects — and the demo is a console
    # program by design (it prints diagnostics throughout). So we opt out of
    # the hijack and call SDL_SetMainReady() ourselves (DEMO/REV.CPP).
    add_compile_definitions(SDL_MAIN_HANDLED)
endif()

# MSVC-only driver hygiene.
if(MSVC)
    # 1998-era sources with big switch bodies and heavy template
    # instantiation blow the 65 279-section object limit without /bigobj.
    add_compile_options(/bigobj)
    # The engine uses standard C++ exceptions; without /EHsc MSVC warns on
    # every <vector>/<string> include and generates no unwind tables.
    add_compile_options(/EHsc)
    # The tree already defines this per-target; make it global so DEMO and
    # the tests get it too (fopen/sprintf/strcpy are used throughout).
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    # Source files are UTF-8 and some carry non-ASCII in comments/strings;
    # without this MSVC reads them in the system codepage and warns (C4819).
    add_compile_options(/utf-8)
endif()

# ----------------------------------------------------------------------------
# Profile-Guided Optimization (PGO) — clang IR-based, 2-build flow.
#
# RESULT (2026-05-31, arm64 + thin LTO + -O3, baseline already hand-tuned
# SIMD): IR PGO did NOT pay off on our workload.
#   - city deferred:   baseline 19.04 ms/iter, PGO ~22 ms (worse 2 of 3 runs).
#   - greets deferred: baseline 22.68 ms,      PGO 21.98 ms (−3%).
# Likely reasons: thin LTO already inlined the cross-module hot paths, and
# IR PGO's block-layout rearrangements can pessimize tight vectorized loops
# where the "cold" branch the profile predicts doesn't actually exist on
# the hot path. Frontend-instr PGO was tried first and slowed the
# instrumented build by ~96x (24 ms -> 2.4 s/iter), making capture
# impractical regardless of USE-phase quality.
#
# Sample-based PGO (-fprofile-sample-use) was considered but rejected: the
# macOS toolchain doesn't ship `llvm-profgen`, and the xctrace -> perf-
# script -> llvm-profgen converter is third-party. Given IR PGO's small
# upside, the setup cost wasn't justified.
#
# The infrastructure is kept as a switch (`pgo_flags` interface lib + the
# PGO_PHASE cache var) so a future workload or toolchain change can revisit
# without recreating the plumbing. Default is OFF; nothing changes unless
# you opt in.
#
# Drive via the PGO_PHASE cache variable:
#   -DPGO_PHASE=OFF                   (default — no PGO flags)
#   -DPGO_PHASE=INSTRUMENT            (build an instrumented binary)
#   -DPGO_PHASE=USE=<merged.profdata> (rebuild with profile feedback)
#
# Composes with the thin LTO setup above (LTO is enabled per-config and we
# don't touch it here). PGO flags are applied to compile + link via the
# `pgo_flags` INTERFACE target — DEMO and FDS link it; Modplayer (Rust)
# does not, so cargo's link is unaffected.
#
# Usage example:
#   cmake -S . -B build-pgo-instrument -G Ninja -DPGO_PHASE=INSTRUMENT
#   cmake --build build-pgo-instrument
#   cd Runtime && LLVM_PROFILE_FILE='../build-pgo-instrument/pgo-prof/run-%p-%m.profraw' \
#       ../build-pgo-instrument/DEMO/DEMO --bench=...
#   xcrun llvm-profdata merge -o build-pgo-instrument/pgo-prof/merged.profdata \
#       build-pgo-instrument/pgo-prof/*.profraw
#   cmake -S . -B build-pgo-use -G Ninja \
#       -DPGO_PHASE=USE=$(pwd)/build-pgo-instrument/pgo-prof/merged.profdata
#   cmake --build build-pgo-use
#
# Caveat for the INSTRUMENT phase: greets crashes in static destructors on
# exit (libc++abi terminate); the atexit-based profile flush is lost. If
# you re-enable this path, gate `__llvm_profile_write_file()` on a CMake
# define (e.g. FDS_PGO_INSTRUMENT) and call it at the end of RunSceneBench.
# ----------------------------------------------------------------------------
set(PGO_PHASE "OFF" CACHE STRING
    "PGO phase: OFF | INSTRUMENT | USE=<abs-path-to-merged.profdata>")

add_library(pgo_flags INTERFACE)

if(PGO_PHASE STREQUAL "OFF")
    # Nothing to do.
elseif(PGO_PHASE STREQUAL "INSTRUMENT")
    set(_pgo_profdir "${CMAKE_BINARY_DIR}/pgo-prof")
    file(MAKE_DIRECTORY "${_pgo_profdir}")
    # %p (pid) + %m (binary signature) let one run produce multiple
    # .profraw files without clobbering. LLVM_PROFILE_FILE at run-time
    # overrides the baked-in fallback path.
    set(_pgo_instr_pattern "${_pgo_profdir}/profile-%p-%m.profraw")
    message(STATUS "PGO: INSTRUMENT phase (IR-based), raw profiles -> ${_pgo_instr_pattern}")
    target_compile_options(pgo_flags INTERFACE
        "-fprofile-generate=${_pgo_profdir}")
    target_link_options(pgo_flags INTERFACE
        "-fprofile-generate=${_pgo_profdir}")
elseif(PGO_PHASE MATCHES "^USE=(.+)$")
    set(_pgo_profdata "${CMAKE_MATCH_1}")
    if(NOT IS_ABSOLUTE "${_pgo_profdata}")
        message(FATAL_ERROR "PGO_PHASE=USE=<path> requires an ABSOLUTE path to merged.profdata (got '${_pgo_profdata}')")
    endif()
    if(NOT EXISTS "${_pgo_profdata}")
        message(FATAL_ERROR "PGO_PHASE=USE: profdata file does not exist: ${_pgo_profdata}")
    endif()
    message(STATUS "PGO: USE phase (IR-based), feeding back ${_pgo_profdata}")
    target_compile_options(pgo_flags INTERFACE
        "-fprofile-use=${_pgo_profdata}")
    # No link-time PGO flag needed for the USE phase.
else()
    message(FATAL_ERROR "PGO_PHASE must be OFF | INSTRUMENT | USE=<abs-path-to-merged.profdata> (got '${PGO_PHASE}')")
endif()

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

# Standalone Metal GPU benchmark / ground-truth instrument (macOS only).
# NOT a shipping backend, NOT part of the demo — see docs/GPU_BENCHMARK_PLAN.md.
# Default OFF so `cmake --build build` and DEMO are untouched:
#   cmake -S . -B build-gpu -G Ninja -DFDS_GPU_BENCH=ON
option(FDS_GPU_BENCH "Build the standalone Metal deferred benchmark (macOS only)" OFF)
if(FDS_GPU_BENCH)
    if(APPLE AND NOT EMSCRIPTEN)
        add_subdirectory(GpuBench)
    else()
        message(WARNING "FDS_GPU_BENCH=ON ignored: the GPU benchmark is macOS/Metal only")
    endif()
endif()

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
