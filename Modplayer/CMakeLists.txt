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
if(EMSCRIPTEN)
    set(_cargo_target_flag --target wasm32-unknown-emscripten)
    set(_cargo_features_flag --no-default-features --features external-audio)
    set(_modplayer_rust_lib
        "${MODPLAYER_DIR}/target/wasm32-unknown-emscripten/${_cargo_profile_dir}/libmodplayer.a")
else()
    set(_cargo_target_flag "")
    set(_cargo_features_flag "")
    set(_modplayer_rust_lib
        "${MODPLAYER_DIR}/target/${_cargo_profile_dir}/libmodplayer.a")
endif()

if(EMSCRIPTEN)
    # The C++ side links with -pthread (--shared-memory). Rust objects must be
    # built with the matching wasm features, otherwise wasm-ld rejects them.
    # Rebuild std too (-Z build-std) so its objects also carry +atomics.
    add_custom_command(
        OUTPUT "${_modplayer_rust_lib}"
        COMMAND ${CMAKE_COMMAND} -E env
                CMAKE_POLICY_VERSION_MINIMUM=3.5
                "RUSTFLAGS=-C target-feature=+atomics,+bulk-memory"
                "${CARGO_EXECUTABLE}" build ${_cargo_profile_flag}
                ${_cargo_target_flag} ${_cargo_features_flag}
                -Z build-std=std,panic_abort
                --package modplayer-lib
        WORKING_DIRECTORY "${MODPLAYER_DIR}"
        COMMENT "Building modplayer (Rust wasm, ${_cargo_profile_dir})"
        USES_TERMINAL)
else()
    add_custom_command(
        OUTPUT "${_modplayer_rust_lib}"
        # CMAKE_POLICY_VERSION_MINIMUM=3.5 lets the vendored sdl2-sys build under
        # CMake >= 4.0, which dropped compatibility with its older cmake_minimum_required.
        COMMAND ${CMAKE_COMMAND} -E env CMAKE_POLICY_VERSION_MINIMUM=3.5
                "${CARGO_EXECUTABLE}" build ${_cargo_profile_flag}
                --package modplayer-lib
        WORKING_DIRECTORY "${MODPLAYER_DIR}"
        COMMENT "Building modplayer (Rust, ${_cargo_profile_dir})"
        USES_TERMINAL)
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
