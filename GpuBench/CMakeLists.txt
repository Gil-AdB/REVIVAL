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
    SceneIngest.h)

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

# Compile the .mm as Objective-C++ without enabling the OBJCXX language
# project-wide (which would perturb the default configure).
set_source_files_properties(GpuBenchMain.mm Deferred.mm PROPERTIES
    LANGUAGE CXX
    COMPILE_OPTIONS "-ObjC++;-fobjc-arc")

target_compile_definitions(${PROJECT_NAME} PRIVATE SIMDE_ENABLE_NATIVE_ALIASES)

# find_library, not a raw "-framework X" link option: CMake de-duplicates and
# reorders bare option lists and silently collapses the `-framework NAME` pairs.
find_library(GPUBENCH_FOUNDATION Foundation REQUIRED)
find_library(GPUBENCH_METAL      Metal      REQUIRED)
find_library(GPUBENCH_QUARTZCORE QuartzCore REQUIRED)

target_link_libraries(${PROJECT_NAME} PRIVATE
    FDS
    ${GPUBENCH_FOUNDATION}
    ${GPUBENCH_METAL}
    ${GPUBENCH_QUARTZCORE})
