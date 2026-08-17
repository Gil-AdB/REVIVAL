set(PROJECT_NAME Modplayer)

# The heavy lifting is in a Rust workspace tracked as a git submodule at
# Modplayer/modplayer. Override with -DMODPLAYER_DIR=... to point at a
# sibling checkout instead (useful for cross-repo development).
set(MODPLAYER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/modplayer" CACHE PATH
    "Path to the modplayer Rust workspace")

if(NOT EXISTS "${MODPLAYER_DIR}/Cargo.toml")
    message(FATAL_ERROR
        "modplayer not found at '${MODPLAYER_DIR}'.\n"
        "Run `git submodule update --init --recursive` from the repo root, "
        "or pass -DMODPLAYER_DIR=/path/to/modplayer to point at a sibling "
        "checkout.")
endif()

find_program(CARGO_EXECUTABLE cargo REQUIRED)

# Rust profile: release unless the user asked for a debug CMake build.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_cargo_profile_flag "")
    set(_cargo_profile_dir "debug")
else()
    set(_cargo_profile_flag "--release")
    set(_cargo_profile_dir "release")
endif()

# On Emscripten the bundled sdl2 crate collides with emcc's -sUSE_SDL=2,
# so we build with --no-default-features --features external-audio. The C++
# side opens its own SDL_AudioDevice and pulls samples via Modplayer_FillBuffer.
#
# On Windows the same crate builds unchanged, but two things differ and both
# are decided here rather than at the call site:
#   * the static library is named `modplayer.lib` under the MSVC ABI and
#     `libmodplayer.a` under the GNU (MinGW-w64) ABI;
#   * a CROSS build needs `--target <triple>`, which also moves the artifact
#     into `target/<triple>/<profile>/` instead of `target/<profile>/`.
# RUST_TARGET is set by cmake/toolchain-mingw-w64.cmake and may be set by
# hand for any other cross combination; leaving it empty builds for the host.
set(RUST_TARGET "" CACHE STRING
    "cargo --target triple (empty = build for the host)")

if(EMSCRIPTEN)
    set(_cargo_target_triple "wasm32-unknown-emscripten")
    set(_cargo_features_flag --no-default-features --features external-audio)
    set(_modplayer_lib_name "libmodplayer.a")
else()
    set(_cargo_target_triple "${RUST_TARGET}")
    set(_cargo_features_flag "")
    # MSVC-ABI targets produce a MSVC-style .lib; everything else (unix,
    # MinGW-w64) produces the unix-style archive.
    if(MSVC AND NOT _cargo_target_triple MATCHES "windows-gnu")
        set(_modplayer_lib_name "modplayer.lib")
    else()
        set(_modplayer_lib_name "libmodplayer.a")
    endif()
endif()

if(_cargo_target_triple)
    set(_cargo_target_flag --target ${_cargo_target_triple})
    set(_modplayer_rust_lib
        "${MODPLAYER_DIR}/target/${_cargo_target_triple}/${_cargo_profile_dir}/${_modplayer_lib_name}")
else()
    set(_cargo_target_flag "")
    set(_modplayer_rust_lib
        "${MODPLAYER_DIR}/target/${_cargo_profile_dir}/${_modplayer_lib_name}")
endif()

# Cargo must run on EVERY build: the .a used to be the only OUTPUT with no
# source DEPENDS, so once it existed Ninja never re-ran cargo, and Rust-side
# changes (e.g. a submodule bump adding an FFI export) silently never reached
# the link — the stale-lib `undefined symbol: Modplayer_FillBufferPlanar`
# wasm break. The second, never-created OUTPUT below keeps the edge
# permanently dirty (so cargo — which tracks its own deps and no-ops in
# ~0.06 s — runs each build), while Ninja's restat on custom-command edges
# re-stats the .a afterwards: unchanged mtime = no downstream relink.
# (A plain always-run add_custom_target doesn't work: its edges skip
# restat, so every build relinked the ~7 s wasm executables.)
# The `sdl2` crate is pulled in with features = ["bundled", "static-link"], so
# cargo builds the vendored SDL2 *C* sources through cmake as part of
# `sdl2-sys`. GCC 15 — the MinGW-w64 Homebrew ships, and the current MSYS2
# one — defaults to `-std=gnu23`, where `bool`/`true`/`false` are keywords;
# SDL 2.32's SDL_hidapi_steam.c still writes `typedef enum { false, true }
# bool;` and fails to compile. Pinning the C dialect for that sub-build is the
# whole fix. MSVC does not default to C23, so this is MinGW-only, and it is
# scoped to the cargo invocation — no C++ flag in this tree is touched.
set(_cargo_extra_env "")
if(WIN32 AND NOT MSVC)
    list(APPEND _cargo_extra_env "CFLAGS=-std=gnu17")
endif()

set(_cargo_force_run "${CMAKE_CURRENT_BINARY_DIR}/cargo_always_run")
if(EMSCRIPTEN)
    # The C++ side links with -pthread (--shared-memory). Rust objects must be
    # built with the matching wasm features, otherwise wasm-ld rejects them.
    # Rebuild std too (-Z build-std) so its objects also carry +atomics.
    add_custom_command(
        OUTPUT "${_modplayer_rust_lib}" "${_cargo_force_run}"
        COMMAND ${CMAKE_COMMAND} -E env
                CMAKE_POLICY_VERSION_MINIMUM=3.5
                "RUSTFLAGS=-C target-feature=+atomics,+bulk-memory"
                "${CARGO_EXECUTABLE}" build ${_cargo_profile_flag}
                ${_cargo_target_flag} ${_cargo_features_flag}
                -Z build-std=std,panic_abort
                --package modplayer-lib
        WORKING_DIRECTORY "${MODPLAYER_DIR}"
        COMMENT "Building modplayer (Rust wasm, ${_cargo_profile_dir})")
else()
    add_custom_command(
        OUTPUT "${_modplayer_rust_lib}" "${_cargo_force_run}"
        # CMAKE_POLICY_VERSION_MINIMUM=3.5 lets the vendored sdl2-sys build under
        # CMake >= 4.0, which dropped compatibility with its older cmake_minimum_required.
        COMMAND ${CMAKE_COMMAND} -E env CMAKE_POLICY_VERSION_MINIMUM=3.5
                ${_cargo_extra_env}
                "${CARGO_EXECUTABLE}" build ${_cargo_profile_flag}
                ${_cargo_target_flag} ${_cargo_features_flag}
                --package modplayer-lib
        WORKING_DIRECTORY "${MODPLAYER_DIR}"
        COMMENT "Building modplayer (Rust, ${_cargo_profile_dir}${_cargo_target_triple})")
endif()

add_custom_target(modplayer_rust_build DEPENDS "${_modplayer_rust_lib}")

add_library(modplayer_rust STATIC IMPORTED GLOBAL)
set_target_properties(modplayer_rust PROPERTIES
    IMPORTED_LOCATION "${_modplayer_rust_lib}")
add_dependencies(modplayer_rust modplayer_rust_build)

# Header-only C++ facade — all implementation lives in the Rust lib.
add_library(${PROJECT_NAME} INTERFACE)
target_include_directories(${PROJECT_NAME} INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(${PROJECT_NAME} INTERFACE modplayer_rust)

# On Windows `libmodplayer.a` is not just Rust: the `sdl2` crate is built with
# features = ["bundled", "static-link"], so the archive also carries SDL2's own
# Windows objects (SDL_hidapi.c, SDL_windowskeyboard.c, ...) plus Rust std's
# Win32 layer. Those pull in system import libraries that nothing else in this
# link line references, and without them the DEMO link fails with a wall of
# `undefined reference to __imp_SetupDi*` / `Imm*` / `NtReadFile`.
#
# This list is SDL2's own documented static-link set (what `sdl2-config
# --static-libs` emits on MinGW) plus the three Rust std needs (ntdll, bcrypt,
# userenv). Discovered empirically: each name below was added in response to a
# real undefined symbol from the cross-link.
if(WIN32)
    target_link_libraries(${PROJECT_NAME} INTERFACE
        setupapi imm32 version winmm ole32 oleaut32 uuid
        gdi32 user32 advapi32 shell32 cfgmgr32
        dinput8 dxguid
        ntdll bcrypt userenv ws2_32)
endif()
