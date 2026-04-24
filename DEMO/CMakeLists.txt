set(PROJECT_NAME DEMO)

find_package(SDL2 REQUIRED)

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
    SceneTick.h
    SDL2.cpp
    SDL2.h)

source_group("" FILES ${DEMO_SOURCES})

add_executable(${PROJECT_NAME} ${DEMO_SOURCES})

target_compile_definitions(${PROJECT_NAME} PRIVATE
    SIMDE_ENABLE_NATIVE_ALIASES)

target_link_libraries(${PROJECT_NAME} PRIVATE
    FDS
    Modplayer
    SDL2::SDL2)

install(TARGETS ${PROJECT_NAME}
        RUNTIME DESTINATION "${CMAKE_SOURCE_DIR}/Runtime")
