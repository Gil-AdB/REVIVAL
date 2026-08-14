add_executable(clipper_test clipper_test.cpp)
target_link_libraries(clipper_test PRIVATE FDS)
target_compile_definitions(clipper_test PRIVATE SIMDE_ENABLE_NATIVE_ALIASES)

# PBR import unit tests. Compiles the two DEMO translation units under test
# directly (MeshOps for MakeChannel8, MaterialImport for the classifier's public
# seam) rather than linking the DEMO executable — there is no DEMO library
# target, and main() lives in REV.CPP.
#
# REGISTERED WITH CTEST. It was not, once: a test that no `ctest` run executes
# is a file, not a test.
add_executable(pbr_import_test
    pbr_import_test.cpp
    ${CMAKE_SOURCE_DIR}/DEMO/MeshOps.cpp
    ${CMAKE_SOURCE_DIR}/DEMO/MaterialImport.cpp)
target_link_libraries(pbr_import_test PRIVATE FDS)
target_include_directories(pbr_import_test PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/DEMO)
target_compile_definitions(pbr_import_test PRIVATE SIMDE_ENABLE_NATIVE_ALIASES)

if(NOT EMSCRIPTEN)
    add_test(NAME pbr_import COMMAND pbr_import_test)
    set_tests_properties(pbr_import PROPERTIES TIMEOUT 60)
endif()

# Rendering regression: a CONDUCTOR under the analytic studio dome. Asserts on
# PIXELS, not on byte-identity — the failure mode it targets (a metal that goes
# black because the wrong map, or the wrong channel of the right map, reached
# MetallicMap) is perfectly deterministic and a hash pin sails straight through
# it. Needs the DEMO binary and the in-repo blue_metal_plate set; see the
# script's header for the tolerances and how to re-baseline them.
if(NOT EMSCRIPTEN)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME pbr_studio_render
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/pbr_studio_parity.py
                    --demo $<TARGET_FILE:DEMO>
                    --runtime ${CMAKE_SOURCE_DIR}/Runtime)
        set_tests_properties(pbr_studio_render PROPERTIES TIMEOUT 300)
    else()
        message(STATUS "python3 not found — skipping the pbr_studio_render test")
    endif()
endif()
