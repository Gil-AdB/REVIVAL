# GpuBench — standalone GPU deferred renderer, used as a BENCHMARK and
# ground-truth instrument. NOT a shipping backend. See docs/GPU_BENCHMARK_PLAN.md.
#
# Only configured when -DFDS_GPU_BENCH=ON (see top-level CMakeLists).

set(PROJECT_NAME GpuBench)

if(APPLE)
    file(GLOB GPUBENCH_SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.metal")
    set(GPUBENCH_PLATFORM_SOURCES
        GpuBenchMain.mm
        Deferred.mm
        Deferred.h
    )
elseif(WIN32)
    file(GLOB GPUBENCH_SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.*")
    set(GPUBENCH_PLATFORM_SOURCES
        GpuBenchMainWin.cpp
        DeferredWin.cpp
        DeferredWin.h
    )
endif()

add_executable(${PROJECT_NAME}
    ${GPUBENCH_PLATFORM_SOURCES}
    SceneIngest.cpp
    SceneIngest.h
    ParticleReplay.cpp
    ParticleReplay.h)

# Stage shaders alongside binary
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/.shaders.stamp"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/shaders"
    COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/.shaders.stamp"
    DEPENDS ${GPUBENCH_SHADERS}
    COMMENT "Staging GpuBench shader sources")
add_custom_target(GpuBenchShaders ALL
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/.shaders.stamp")
add_dependencies(GpuBenchShaders ${PROJECT_NAME})

target_compile_definitions(${PROJECT_NAME} PRIVATE SIMDE_ENABLE_NATIVE_ALIASES)

find_package(SDL2 REQUIRED)
target_include_directories(${PROJECT_NAME} PRIVATE ${SDL2_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR}/FDS ${CMAKE_SOURCE_DIR})

if(APPLE)
    set_source_files_properties(GpuBenchMain.mm Deferred.mm PROPERTIES
        LANGUAGE CXX
        COMPILE_OPTIONS "-ObjC++;-fobjc-arc")

    find_library(GPUBENCH_FOUNDATION Foundation REQUIRED)
    find_library(GPUBENCH_METAL      Metal      REQUIRED)
    find_library(GPUBENCH_QUARTZCORE QuartzCore REQUIRED)

    target_link_libraries(${PROJECT_NAME} PRIVATE
        FDS
        ${SDL2_LIBRARIES}
        ${GPUBENCH_FOUNDATION}
        ${GPUBENCH_METAL}
        ${GPUBENCH_QUARTZCORE})
elseif(WIN32)
    if(MSVC)
        target_compile_options(${PROJECT_NAME} PRIVATE /arch:AVX2 /FIcmath /FIintrin.h)
    endif()
    target_link_libraries(${PROJECT_NAME} PRIVATE
        FDS
        ${SDL2_LIBRARIES}
        d3d11
        d3dcompiler
        dxgi
    )
endif()
