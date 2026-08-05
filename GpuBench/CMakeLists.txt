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
    SceneIngest.cpp
    SceneIngest.h)

# This machine has Command Line Tools only, so there is no offline `metal`
# compiler; shaders are compiled at RUNTIME from source (measured working). They
# are therefore data, not build inputs — copy them next to the binary.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/shaders"
    COMMENT "Staging GpuBench Metal shader sources")

# Compile the .mm as Objective-C++ without enabling the OBJCXX language
# project-wide (which would perturb the default configure).
set_source_files_properties(GpuBenchMain.mm PROPERTIES
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
