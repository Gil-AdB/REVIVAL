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
    SDL2.h)

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
        -sPTHREAD_POOL_SIZE=navigator.hardwareConcurrency
        -sALLOW_MEMORY_GROWTH=1
        -sINITIAL_MEMORY=128MB
        -sASSERTIONS=1
        -sEXIT_RUNTIME=1
        -sUSE_SDL=2
        --preload-file "${CMAKE_SOURCE_DIR}/Runtime@/"
    )
    set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX ".html")
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE
        FDS
        Modplayer
        SDL2::SDL2)

    install(TARGETS ${PROJECT_NAME}
            RUNTIME DESTINATION "${CMAKE_SOURCE_DIR}/Runtime")
endif()
