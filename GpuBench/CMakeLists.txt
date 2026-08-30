# GpuBench — standalone Metal deferred renderer, used as a BENCHMARK and
# ground-truth instrument. NOT a shipping backend. See docs/GPU_BENCHMARK_PLAN.md.
#
# Only configured when -DFDS_GPU_BENCH=ON (see the top-level CMakeLists), so the
# default `cmake --build build` is completely unaffected.
#
# Links FDS purely for LOADING (FLD/LWO/textures/materials/lights/camera). It
# pulls in no SDL and no rasterizer: FDS is a static library with zero undefined
# SDL_ symbols (verified with `nm -u`), so the linker only takes the loader
# objects the tool actually references.

set(PROJECT_NAME GpuBench)

add_executable(${PROJECT_NAME}
    GpuBenchMain.mm
    Deferred.mm
    Deferred.h
    SceneIngest.cpp
    SceneIngest.h
    ParticleReplay.cpp
    ParticleReplay.h)

# This machine has Command Line Tools only, so there is no offline `metal`
# compiler; shaders are compiled at RUNTIME from source (measured working). They
# are therefore data, not build inputs — copy them next to the binary.
#
# The copy is its OWN target with the .metal files as DEPENDS, not a POST_BUILD
# on GpuBench. POST_BUILD only fires when the executable relinks, so editing a
# shader alone left a STALE copy staged next to the binary — the runtime then
# compiled the old source and reported errors against line numbers that no longer
# existed. Cost a confusing detour; this makes an edited shader always restage.
file(GLOB GPUBENCH_SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.metal")
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/.shaders.stamp"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/shaders"
    COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/.shaders.stamp"
    DEPENDS ${GPUBENCH_SHADERS}
    COMMENT "Staging GpuBench Metal shader sources")
add_custom_target(GpuBenchShaders ALL
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/.shaders.stamp")
add_dependencies(GpuBenchShaders ${PROJECT_NAME})

# The .mm files are Objective-C++ and must be compiled as such. The old form
# here (LANGUAGE CXX + a manual -ObjC++ flag) broke on newer CMake: since
# CMP0119 (CMake >= 3.20, default NEW in 4.x with a fresh cache), LANGUAGE CXX
# emits an explicit `-x c++`, which overrides the .mm extension, makes clang
# warn "argument unused during compilation: '-ObjC++'" and then reject every
# @class in the Foundation headers (first seen on the M5's CLT setup,
# 2026-08-31). The supported way: enable the OBJCXX language HERE, in this
# subdirectory only — GpuBench is gated behind -DFDS_GPU_BENCH=ON, so the
# default configure is still unperturbed, which is what the old hack was for.
enable_language(OBJCXX)
set_source_files_properties(GpuBenchMain.mm Deferred.mm PROPERTIES
    COMPILE_OPTIONS "-fobjc-arc")

target_compile_definitions(${PROJECT_NAME} PRIVATE SIMDE_ENABLE_NATIVE_ALIASES)

# find_library, not a raw "-framework X" link option: CMake de-duplicates and
# reorders bare option lists and silently collapses the `-framework NAME` pairs.
# SDL2 for the INTERACTIVE window only (--window). Chosen over raw AppKit/Cocoa
# because: it is already a dependency of DEMO and installed here (2.32.10), it
# ships SDL_metal.h so a CAMetalLayer is three calls rather than an
# NSApplication + NSWindow + delegate, and it gives keyboard/mouse capture that
# works identically to the rest of the project. Cost: one extra link dependency
# on a target that otherwise links only Metal/Foundation/QuartzCore. The
# offscreen benchmark path never touches it.
find_package(SDL2 REQUIRED)

find_library(GPUBENCH_FOUNDATION Foundation REQUIRED)
find_library(GPUBENCH_METAL      Metal      REQUIRED)
find_library(GPUBENCH_QUARTZCORE QuartzCore REQUIRED)

target_include_directories(${PROJECT_NAME} PRIVATE ${SDL2_INCLUDE_DIRS})

target_link_libraries(${PROJECT_NAME} PRIVATE
    FDS
    ${SDL2_LIBRARIES}
    ${GPUBENCH_FOUNDATION}
    ${GPUBENCH_METAL}
    ${GPUBENCH_QUARTZCORE})
