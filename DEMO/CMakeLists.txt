set(PROJECT_NAME DEMO)

if(NOT EMSCRIPTEN)
    find_package(SDL2 REQUIRED)
endif()

set(DEMO_SOURCES
    CHASE.CPP
    CHASE.H
    CITY.CPP
    CITY.H
    CityPanoramaCache.cpp
    CityPanoramaCache.h
    Config.cpp
    Config.h
    CRASH.CPP
    CRASH.H
    CREDITS.CPP
    CREDITS.H
    FillerTest.cpp
    FillerTest.h
    FOUNTAIN.CPP
    FOUNTAIN.H
    FrameProfiler.cpp
    FrameProfiler.h
    Glat.cpp
    GLAT.H
    GREETS.CPP
    GREETS.H
    GreetsDisco.cpp
    GreetsDisco.h
    GreetsMirror.cpp
    GreetsMirror.h
    BlasterBolts.cpp
    BlasterBolts.h
    MainLoop.cpp
    MainLoop.h
    ImageCompression.cpp
    ImageCompression.h
    MeshOps.cpp
    MeshOps.h
    ProceduralWater.cpp
    ProceduralWater.h
    MirrorShatter.cpp
    MirrorShatter.h
    MirrorTestDriver.cpp
    MirrorTestDriver.h
    PhotonTracer.cpp
    PhotonTracer.h
    Raytracer.cpp
    Raytracer.h
    Resize.cpp
    Resize.h
    REV.CPP
    Rev.h
    Scenes.h
    SceneBuilder.cpp
    SceneBuilder.h
    SceneDriver.cpp
    SceneTick.h
    SpotlightCones.cpp
    SpotlightCones.h
    SDL2.cpp
    SDL2.h
    Snapshot.cpp
    Snapshot.h)

source_group("" FILES ${DEMO_SOURCES})

add_executable(${PROJECT_NAME} ${DEMO_SOURCES})

target_compile_definitions(${PROJECT_NAME} PRIVATE
    SIMDE_ENABLE_NATIVE_ALIASES)

# -ffp-contract=fast: NOT applied to any DEMO file.
#
# Bisect on 2026-05-31 traced the city-horizon smear artifact to
# FOUNTAIN.CPP getting -fast. Root cause is a template-instantiation
# ODR issue: FOUNTAIN.CPP, Snapshot.cpp, PREPROC.CPP, RenderInner.cpp,
# and FillerTest.cpp each instantiate TheOtherBarry<TBlendMode, TTextureMode>
# variants (forward rasterizer template). When one TU is built with
# -fast and others with -on, the linker silently picks one — if it
# picks the -fast version, every caller (including city) ends up using
# the FMA-fused edge functions whose 1-ULP shifts cause stretched-
# triangle artifacts at frustum boundaries.
#
# Re-enabling -fast for the perf-relevant hot-loop files (FILLERS.CPP,
# Mekalele.cpp, DeferredLighting.cpp) requires either making ALL
# TheOtherBarry/Mekalele template instantiators -fast together, or
# converting to explicit template instantiation in one TU with
# `extern template` declarations elsewhere. Tracked as follow-up;
# meanwhile only RENDER/Transform.cpp gets -fast (see FDS/CMakeLists).

if(EMSCRIPTEN)
    # Emscripten brings its own SDL2 via -sUSE_SDL=2; don't require
    # find_package(SDL2) on this target.
    target_link_libraries(${PROJECT_NAME} PRIVATE
        FDS
        Modplayer)

    # main() runs on the actual browser main thread (no PROXY_TO_PTHREAD).
    # The blocking demo loop has been replaced with a state machine in
    # MainLoop.cpp driven by emscripten_set_main_loop. WebGL on browser-
    # main works fine; the OffscreenCanvas + commit_frame path from a
    # pthread does not (emscripten#17816 / #23806). Heavy init still
    # happens off-main: MainLoop spawns a single std::thread that runs
    # all Initialize_* in sequence, gating each scene's start on the
    # corresponding atomic flag.
    # --preload-file Runtime: embed the asset tree so the demo can read
    # rev.cfg / SCENES / TEXTURES / FONTS relative to "/".
    target_compile_options(${PROJECT_NAME} PRIVATE -pthread)
    target_link_options(${PROJECT_NAME} PRIVATE
        -pthread
        # Pool size: init thread + ThreadPool render workers + audio
        # worker + a couple of headroom slots. Up from native concurrency
        # because pthread workers are actual web workers — not free.
        -sPTHREAD_POOL_SIZE=navigator.hardwareConcurrency+4
        # Emscripten's default pthread stack is 64KB which overflows during
        # XM module parsing (and probably elsewhere — the engine wasn't
        # written with such a tight stack in mind). Match a typical native
        # default of ~4MB.
        -sSTACK_SIZE=4MB
        -sDEFAULT_PTHREAD_STACK_SIZE=4MB
        # Rasterizer dispatches tile work to ThreadPool workers and waits
        # on a condvar for them to finish — that's a sync block on main.
        # Modern browsers tolerate it; emscripten still warns unless we
        # opt in explicitly.
        -sALLOW_BLOCKING_ON_MAIN_THREAD=1
        -sALLOW_MEMORY_GROWTH=1
        -sINITIAL_MEMORY=128MB
        -sASSERTIONS=1
        # EXIT_RUNTIME=0: main() returns immediately after registering the
        # rAF callback; the runtime must stay alive for the callback to
        # keep firing. emscripten_cancel_main_loop tears down at DONE.
        -sEXIT_RUNTIME=0
        -sUSE_SDL=2
        # Emit a wasm name section so browser stack traces show function
        # names instead of wasm-function[N]. Cheap (no codegen impact) and
        # essential for diagnosing OOB / unreachable traps.
        --profiling-funcs
        --preload-file "${CMAKE_SOURCE_DIR}/Runtime@/"
        --shell-file "${CMAKE_CURRENT_SOURCE_DIR}/shell.html"
    )
    set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX ".html")

    # Copy the COOP/COEP service worker next to the generated HTML so it's
    # served at the same scope (needed when hosting on plain static servers
    # like GitHub Pages that don't send the headers themselves).
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/coi-serviceworker.js"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/coi-serviceworker.js")
    # Audio worklet — fed by Modplayer_FillBuffer via Module.canvas's
    # parent JS. Replaces SDL2's deprecated ScriptProcessorNode path on
    # emscripten so audio playback runs on its own thread, immune to
    # main-thread stalls.
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/audio-worklet.js"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/audio-worklet.js")

    # Headless wasm snapshot binary, runs under node:
    #   cd Runtime && node ../build-wasm/DEMO/DEMO_snapshot.js \
    #     --snapshot=city --out=snapshots
    # Same source as DEMO but stripped of browser-only scaffolding so it
    # exits cleanly after the snapshot path returns from main().
    add_executable(DEMO_snapshot ${DEMO_SOURCES})
    target_compile_definitions(DEMO_snapshot PRIVATE
        SIMDE_ENABLE_NATIVE_ALIASES)
    target_link_libraries(DEMO_snapshot PRIVATE
        FDS
        Modplayer)
    target_compile_options(DEMO_snapshot PRIVATE -pthread)
    target_link_options(DEMO_snapshot PRIVATE
        -pthread
        -sENVIRONMENT=node
        # NODERAWFS gives fopen() direct access to the host filesystem so
        # PPM/PGM/CSV outputs land on disk and the demo's relative-path
        # asset loads (rev.cfg, SCENES/, ...) work when run from Runtime/.
        -sNODERAWFS=1
        -sPTHREAD_POOL_SIZE=navigator.hardwareConcurrency+4
        -sSTACK_SIZE=4MB
        -sDEFAULT_PTHREAD_STACK_SIZE=4MB
        -sALLOW_BLOCKING_ON_MAIN_THREAD=1
        -sALLOW_MEMORY_GROWTH=1
        -sINITIAL_MEMORY=128MB
        -sASSERTIONS=1
        -sEXIT_RUNTIME=1
        -sUSE_SDL=2
    )
    set_target_properties(DEMO_snapshot PROPERTIES SUFFIX ".js")
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE
        FDS
        Modplayer
        SDL2::SDL2)

    install(TARGETS ${PROJECT_NAME}
            RUNTIME DESTINATION "${CMAKE_SOURCE_DIR}/Runtime")

    # Auto-install on every native rebuild so `cd Runtime && ./DEMO` always
    # picks up the freshly-linked binary. ONLY for the canonical `build/`
    # tree — secondary trees (build-asan, build-tsan, build-strip) MUST NOT
    # clobber Runtime/DEMO, since users tend to run from Runtime and would
    # silently end up on an ASan/TSan-instrumented exe that's 100× slower
    # and prints sanitizer races at exit. Bit us repeatedly during the
    # gbuffer perf push.
    if(CMAKE_BINARY_DIR STREQUAL "${CMAKE_SOURCE_DIR}/build")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:${PROJECT_NAME}>"
                    "${CMAKE_SOURCE_DIR}/Runtime/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
            COMMENT "Updating Runtime/DEMO with the freshly-linked binary")
    else()
        message(STATUS "Skipping Runtime/DEMO auto-copy (binary dir is ${CMAKE_BINARY_DIR}, not the canonical build/)")
    endif()
endif()
