set(PROJECT_NAME DEMO)

if(NOT EMSCRIPTEN)
    find_package(SDL2 REQUIRED)
endif()

set(DEMO_SOURCES
    CHASE.CPP
    CHASE.H
    CITY.CPP
    CITY.H
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
    Glat.cpp
    GLAT.H
    GREETS.CPP
    GREETS.H
    ImageCompression.cpp
    ImageCompression.h
    PhotonTracer.cpp
    PhotonTracer.h
    Raytracer.cpp
    Raytracer.h
    REV.CPP
    Rev.h
    Scenes.h
    SceneTick.h
    SDL2.cpp
    SDL2.h
    Snapshot.cpp
    Snapshot.h)

source_group("" FILES ${DEMO_SOURCES})

add_executable(${PROJECT_NAME} ${DEMO_SOURCES})

target_compile_definitions(${PROJECT_NAME} PRIVATE
    SIMDE_ENABLE_NATIVE_ALIASES)

if(EMSCRIPTEN)
    # Emscripten brings its own SDL2 via -sUSE_SDL=2; don't require
    # find_package(SDL2) on this target.
    target_link_libraries(${PROJECT_NAME} PRIVATE
        FDS
        Modplayer)

    # -pthread + PROXY_TO_PTHREAD: run main() on a pthread worker so the
    # existing blocking patterns (ThreadPool condvar waits, SDL_WaitEvent)
    # work unchanged without freezing the browser UI thread.
    # --preload-file Runtime: embed the asset tree so the demo can read
    # rev.cfg / SCENES / TEXTURES / FONTS relative to "/".
    target_compile_options(${PROJECT_NAME} PRIVATE -pthread)
    target_link_options(${PROJECT_NAME} PRIVATE
        -pthread
        -sPROXY_TO_PTHREAD=1
        # Pool size: main-proxy + CodeEntry + nested init thread + ThreadPool
        # render workers. navigator.hardwareConcurrency alone isn't enough;
        # add headroom and allow on-demand creation above the pool.
        -sPTHREAD_POOL_SIZE=navigator.hardwareConcurrency+4
        # Emscripten's default pthread stack is 64KB which overflows during
        # XM module parsing (and probably elsewhere — the engine wasn't
        # written with such a tight stack in mind). Match a typical native
        # default of ~4MB for both the proxy main thread and pthread workers.
        -sSTACK_SIZE=4MB
        -sDEFAULT_PTHREAD_STACK_SIZE=4MB
        -sALLOW_BLOCKING_ON_MAIN_THREAD=1
        -sALLOW_MEMORY_GROWTH=1
        -sINITIAL_MEMORY=128MB
        -sASSERTIONS=1
        -sEXIT_RUNTIME=1
        -sUSE_SDL=2
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
endif()
