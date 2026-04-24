set(PROJECT_NAME Modplayer)

# The heavy lifting is in a Rust workspace tracked as a git submodule at
# Modplayer/modplayer-2. Override with -DMODPLAYER2_DIR=... to point at a
# sibling checkout instead (useful for cross-repo development).
set(MODPLAYER2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/modplayer" CACHE PATH
    "Path to the modplayer-2 Rust workspace")

if(NOT EXISTS "${MODPLAYER2_DIR}/Cargo.toml")
    message(FATAL_ERROR
        "modplayer-2 not found at '${MODPLAYER2_DIR}'.\n"
        "Run `git submodule update --init --recursive` from the repo root, "
        "or pass -DMODPLAYER2_DIR=/path/to/modplayer-2 to point at a sibling "
        "checkout.")
endif()

if(EMSCRIPTEN)
    # Music is stubbed out in the Emscripten build (see Modplayer.h) — skip
    # the Rust build entirely. The stubs are inline in the header, so the
    # INTERFACE target just needs to propagate the include directory.
    add_library(${PROJECT_NAME} INTERFACE)
    target_include_directories(${PROJECT_NAME} INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
    return()
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

set(_modplayer_rust_lib
    "${MODPLAYER2_DIR}/target/${_cargo_profile_dir}/libmodplayer.a")

add_custom_command(
    OUTPUT "${_modplayer_rust_lib}"
    # CMAKE_POLICY_VERSION_MINIMUM=3.5 lets the vendored sdl2-sys build under
    # CMake >= 4.0, which dropped compatibility with its older cmake_minimum_required.
    COMMAND ${CMAKE_COMMAND} -E env CMAKE_POLICY_VERSION_MINIMUM=3.5
            "${CARGO_EXECUTABLE}" build ${_cargo_profile_flag} --package modplayer-lib
    WORKING_DIRECTORY "${MODPLAYER2_DIR}"
    COMMENT "Building modplayer-2 (Rust, ${_cargo_profile_dir})"
    USES_TERMINAL)

add_custom_target(modplayer_rust_build DEPENDS "${_modplayer_rust_lib}")

add_library(modplayer_rust STATIC IMPORTED GLOBAL)
set_target_properties(modplayer_rust PROPERTIES
    IMPORTED_LOCATION "${_modplayer_rust_lib}")
add_dependencies(modplayer_rust modplayer_rust_build)

# Header-only C++ facade — all implementation lives in the Rust lib.
add_library(${PROJECT_NAME} INTERFACE)
target_include_directories(${PROJECT_NAME} INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(${PROJECT_NAME} INTERFACE modplayer_rust)
