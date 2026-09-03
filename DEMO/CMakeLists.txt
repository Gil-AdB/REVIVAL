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
    CloakTest.cpp
    DisplaceTest.cpp
    CRASH.CPP
    CRASH.H
    PBRTEST.CPP
    PBRTEST.H
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
    BlasterBolts.cpp
    BlasterBolts.h
    ChaseEvents.cpp
    ChaseEvents.h
    MainLoop.cpp
    MainLoop.h
    ImageCompression.cpp
    ImageCompression.h
    MeshOps.cpp
    MeshOps.h
    DisplaceRebuild.cpp
    SeamPhase.cpp
    MeshDump.cpp
    DisplaceRebuild.h
    MaterialEditor.cpp
    MaterialEditor.h
    MaterialImport.cpp
    MaterialImport.h
    ProceduralWater.cpp
    ProceduralWater.h
    MirrorTestDriver.cpp
    MirrorTestDriver.h
    PhotonTracer.cpp
    PhotonTracer.h
    Raytracer.cpp
    Raytracer.h
    ReproHarness.cpp
    ReproHarness.h
    Resize.cpp
    Resize.h
    REV.CPP
    Rev.h
    Scenes.h
    SceneBuilder.cpp
    SceneBuilder.h
    SceneDriver.cpp
    SceneTick.h
    SDL2.cpp
    SDL2.h
    Snapshot.cpp
    Snapshot.h
    V4Bake.cpp
    V4Border.cpp
    V4Bake.h)

source_group("" FILES ${DEMO_SOURCES})

add_executable(${PROJECT_NAME} ${DEMO_SOURCES})

target_compile_definitions(${PROJECT_NAME} PRIVATE
    SIMDE_ENABLE_NATIVE_ALIASES)

# V4Border.cpp: -ffp-contract=off, LOAD-BEARING (trap 607c7cd2c810).
# It holds the ONE function that produces a shared border's sample positions.
# Clang's default is -ffp-contract=on, which contracts a*s + b*t within a
# statement; when the same expression is later duplicated at another call site
# and contracted differently, the two sides of a shared border stop agreeing to
# the bit and the mesh cracks. Applied to the source file so BOTH the DEMO and
# DEMO_snapshot targets below inherit it.
set_source_files_properties(V4Border.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")

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
    # Stage ONLY the assets the demo reads into a clean dir, then preload that
    # whole dir at "/". Preloading Runtime/ directly bundled a 1.3 GB .data —
    # hundreds of MB of debug PPM/PGM snapshots + the 284 MB city panorama
    # cache/ (greets never needs it) — which the browser can't load. (emcc
    # mis-parses per-file `src@/dst` remaps; the bare `dir@/` form is what
    # works, so we curate the dir instead.)
    set(WASM_ASSET_STAGE "${CMAKE_CURRENT_BINARY_DIR}/wasm_assets")
    add_custom_command(TARGET ${PROJECT_NAME} PRE_LINK
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${WASM_ASSET_STAGE}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${WASM_ASSET_STAGE}"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/Runtime/rev.cfg"    "${WASM_ASSET_STAGE}/rev.cfg"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/Runtime/REVIVAL.XM" "${WASM_ASSET_STAGE}/REVIVAL.XM"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/Runtime/SCENES"   "${WASM_ASSET_STAGE}/SCENES"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/Runtime/FONTS"    "${WASM_ASSET_STAGE}/FONTS"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/Runtime/TEXTURES" "${WASM_ASSET_STAGE}/TEXTURES"
        COMMENT "Staging wasm assets (excludes debug PPMs + city cache)")

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
        # INITIAL_MEMORY=512MB (up from 128MB): the editor's texture IMPORT is
        # the crash trigger. Each import decodes a PNG + builds a tiled mip chain
        # + an 8/16-bit variant — several MB per map — and at 128MB a fresh scene
        # is already close to the ceiling, so nearly every import forced a HEAP
        # GROW. Growing SHARED (-pthread) memory while another thread (the audio
        # worklet, or an idle-but-present ThreadPool worker) is mid-atomic traps
        # as "unaligned memory access" (emscripten#17816 / #23806). 512MB gives
        # a typical editor session enough headroom that imports no longer grow
        # the heap → the grow-during-atomic race almost never arms. Cheap and
        # wasm-only (native + DEMO_snapshot unaffected); combined with the
        # import-texture dedup cache (MaterialImport.cpp) it removes both the
        # per-object re-decode and most of the residual grow pressure. Still
        # under MAXIMUM_MEMORY=4GB, so heavy scenes can grow further if needed.
        -sINITIAL_MEMORY=512MB
        # Greets with mirrors on (clones every mesh reflected) + the lightmap
        # bake + deferred buffers exceeds wasm's default 2GB cap. wasm32 can
        # address up to 4GB; raise the ceiling so the heaviest scene fits.
        -sMAXIMUM_MEMORY=4GB
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
        # Embind: exposes the surface-editor API to JS as
        # Module.editorGetSurfaces() / Module.editorSetSurfaceProp().
        -lembind
        # Curated asset dir (staged above) → mounted at "/". The bare `dir@/`
        # remap is the form emcc parses correctly.
        --preload-file "${WASM_ASSET_STAGE}@/"
        --shell-file "${CMAKE_CURRENT_SOURCE_DIR}/shell.html"
    )
    set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX ".html")
    # --shell-file is a link FLAG, so ninja doesn't know shell.html is an input;
    # editing it wouldn't relink and DEMO.html would stay stale (this bit us:
    # editor panel/JS edits never reached the browser). Declare it (+ the copied
    # sidecars) as link deps so a shell edit forces a regen.
    set_target_properties(${PROJECT_NAME} PROPERTIES
        LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shell.html;${CMAKE_CURRENT_SOURCE_DIR}/coi-serviceworker.js;${CMAKE_CURRENT_SOURCE_DIR}/audio-worklet.js")

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
        # Greets with mirrors on (clones every mesh reflected) + the lightmap
        # bake + deferred buffers exceeds wasm's default 2GB cap. wasm32 can
        # address up to 4GB; raise the ceiling so the heaviest scene fits.
        -sMAXIMUM_MEMORY=4GB
        -sASSERTIONS=1
        -sEXIT_RUNTIME=1
        -sUSE_SDL=2
        # The editor Embind surface (MaterialEditor/MainLoop) is compiled into
        # every wasm target that shares DEMO_SOURCES; node never calls it, but
        # the registration symbols must still resolve.
        -lembind
    )
    set_target_properties(DEMO_snapshot PROPERTIES SUFFIX ".js")
else()
    # SDL2 ON WINDOWS: HEADERS ONLY, symbols come from Modplayer.
    #
    # `modplayer-lib` depends on the `sdl2` crate with features
    # ["bundled", "static-link"], so cargo compiles a COMPLETE SDL2 from source
    # and archives it into libmodplayer.a. Linking SDL2::SDL2 as well puts a
    # SECOND SDL2 in the same executable, and the link dies with hundreds of
    # `multiple definition of SDL_CreateWindow` (etc.) — measured, not feared.
    #
    # There is exactly one SDL2 in the binary either way; this picks the one
    # that is already there. find_package(SDL2) is still what supplies the
    # headers the C++ side compiles against, so keep it pointed at an SDL2 of
    # the same major version as the crate's (2.x).
    #
    # The alternative — building modplayer with
    # `--no-default-features --features external-audio`, as the emscripten
    # build does — removes the crate's SDL2 instead, but the C++ side has no
    # native audio pump (DEMO/SDL2.cpp's Modplayer_FillBufferPlanar path is
    # inside `#ifdef __EMSCRIPTEN__`), so it would ship Windows without music.
    if(WIN32)
        target_link_libraries(${PROJECT_NAME} PRIVATE
            FDS
            Modplayer)
        target_include_directories(${PROJECT_NAME} PRIVATE
            $<TARGET_PROPERTY:SDL2::SDL2,INTERFACE_INCLUDE_DIRECTORIES>)
    else()
        target_link_libraries(${PROJECT_NAME} PRIVATE
            FDS
            Modplayer
            SDL2::SDL2)
    endif()

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
