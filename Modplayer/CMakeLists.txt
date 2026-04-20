set(PROJECT_NAME Modplayer)

# The heavy lifting is in a sibling Rust workspace. Default to ../modplayer-2
# (relative to the revival repo root); override with -DMODPLAYER2_DIR=... .
set(MODPLAYER2_DIR "${CMAKE_SOURCE_DIR}/../modplayer-2" CACHE PATH
    "Path to the modplayer-2 Rust workspace")

if(NOT EXISTS "${MODPLAYER2_DIR}/Cargo.toml")
    message(FATAL_ERROR
        "modplayer-2 not found at '${MODPLAYER2_DIR}'.\n"
        "Clone https://<...>/modplayer-2 next to this repo or pass "
        "-DMODPLAYER2_DIR=/path/to/modplayer-2.")
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
    COMMAND "${CARGO_EXECUTABLE}" build ${_cargo_profile_flag} --package modplayer-lib
    WORKING_DIRECTORY "${MODPLAYER2_DIR}"
    COMMENT "Building modplayer-2 (Rust, ${_cargo_profile_dir})"
    USES_TERMINAL)

add_custom_target(modplayer_rust_build DEPENDS "${_modplayer_rust_lib}")

add_library(modplayer_rust STATIC IMPORTED GLOBAL)
set_target_properties(modplayer_rust PROPERTIES
    IMPORTED_LOCATION "${_modplayer_rust_lib}")
add_dependencies(modplayer_rust modplayer_rust_build)

# Thin C++ wrapper.
add_library(${PROJECT_NAME} STATIC Modplayer.cpp)
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(${PROJECT_NAME} PUBLIC modplayer_rust)
