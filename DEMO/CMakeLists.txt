set(PROJECT_NAME DEMO)

find_library(SDL2_LIBRARY NAMES SDL2 PATHS /opt/homebrew/lib)
if (NOT SDL2_LIBRARY)
    message(FATAL_ERROR "SDL2 library not found in /opt/homebrew/lib")
endif()
include_directories(/opt/homebrew/include/SDL2)

################################################################################
# Source groups
################################################################################
set(no_group_source_files
    "CHASE.CPP"
    "CHASE.H"
    "CITY.CPP"
    "CITY.H"
    "Config.cpp"
    "Config.h"
    "CRASH.CPP"
    "CRASH.H"
    "CREDITS.CPP"
    "CREDITS.H"
    "END.CPP"
    "FillerTest.cpp"
    "FillerTest.h"
    "FOUNTAIN.CPP"
    "FOUNTAIN.H"
    # "Gdi.cpp"
    # "Gdi.h"
    "Glat.cpp"
    "GLAT.H"
    "GREETS.CPP"
    "GREETS.H"
    "ImageCompression.cpp"
    "ImageCompression.h"
#    "Koch.cpp"
#    "Koch.h"
#    "Nova.cpp"
#    "Nova.h"x
    "PhotonTracer.cpp"
    "PhotonTracer.h"
    "Raytracer.cpp"
    "Raytracer.h"
    "REV.CPP"
    "Rev.h"
    "SDL2.cpp"
    "SDL2.h"
    "Win32Display.h"
)
source_group("" FILES ${no_group_source_files})

set(ALL_FILES
    ${no_group_source_files}
)

################################################################################
# Target
################################################################################
add_executable(${PROJECT_NAME} ${ALL_FILES})

use_props(${PROJECT_NAME} "${CMAKE_CONFIGURATION_TYPES}" "${DEFAULT_CXX_PROPS}")
set(ROOT_NAMESPACE DEMO)

target_include_directories(${PROJECT_NAME} PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Users/gil-ad.benor/Work/REVIVAL/FDS"
)

################################################################################
# Compile definitions
################################################################################
target_compile_definitions(${PROJECT_NAME} PRIVATE
    "PORTABLE_CODE=1"
    "SIMDE_ENABLE_NATIVE_ALIASES"
)

################################################################################
# Post build events
################################################################################
# if("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "Win32")
#     add_custom_command_if(
#         TARGET ${PROJECT_NAME}
#         POST_BUILD
#         COMMANDS
#         COMMAND   $<CONFIG:Debug> cp DEMO/DEMO ../Runtime
#         COMMAND   $<CONFIG:Debug> cp DEMO/DEMO ../Runtime
#         COMMAND    $<CONFIG:llvm> cp $<SHELL_PATH:${OUTPUT_DIRECTORY}>${TARGET_NAME}$ENV{TargetExt} ..\\Runtime
#         COMMAND $<CONFIG:Release> cp $<SHELL_PATH:${OUTPUT_DIRECTORY}>${TARGET_NAME}$ENV{TargetExt} ..\\Runtime
#     )
# elseif("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "x64")
#     add_custom_command_if(
#         TARGET ${PROJECT_NAME}
#         POST_BUILD
#         COMMANDS
#         COMMAND   $<CONFIG:Debug> cp $<SHELL_PATH:${OUTPUT_DIRECTORY}>/DEMO ../Runtime
#         COMMAND   $<CONFIG:Debug> cp $<SHELL_PATH:${OUTPUT_DIRECTORY}>/DEMO ../Runtime
#         COMMAND    $<CONFIG:llvm> cp $<SHELL_PATH:${OUTPUT_DIRECTORY}>${TARGET_NAME}$ENV{TargetExt} ..\\Runtime
#         COMMAND $<CONFIG:Release> cp $<SHELL_PATH:${OUTPUT_DIRECTORY}>${TARGET_NAME}$ENV{TargetExt} ..\\Runtime
#     )
# endif()

################################################################################
# Dependencies
################################################################################
# Link with other targets.
target_link_libraries(${PROJECT_NAME} PRIVATE
    FDS
    Modplayer
    "-framework CoreAudio"
    "-framework AudioUnit"
    "-framework CoreFoundation"
    "-framework IOKit"
    "-framework Carbon"
    "-framework AppKit"
    "-framework ForceFeedback"
    "-framework GameController"
    "-framework CoreHaptics"
    "-framework Metal"
    "-framework QuartzCore"
    "-framework AudioToolbox"
    ${SDL2_LIBRARY}
    "-liconv"
)




install(TARGETS ${PROJECT_NAME}
        RUNTIME DESTINATION "../Runtime")

