# Bakes the source provenance of the binary (git HEAD, dirty flag, branch,
# configure timestamp, tree paths, build type) into a generated header so a
# render dumped by ./DEMO can name the exact source it came from.
#
# WHEN IS IT STAMPED?  At cmake CONFIGURE time. To stop the stamp going stale
# after a commit we register the git HEAD file (and, for a checked-out branch,
# the ref file HEAD points at) as a CMAKE_CONFIGURE_DEPENDS: ninja re-runs
# cmake when HEAD moves, so the next build re-stamps automatically.  This is a
# git-worktree-safe lookup (`git rev-parse --git-path`), which matters here —
# the campaign runs ~20 worktrees off one .git.
#
# The DIRTY flag covers TRACKED modifications only (--untracked-files=no).
# Untracked files (docs/img renders, scratch PPMs) are noise in this tree and
# would pin every build to +dirty; they cannot change the compiled source.

function(fds_generate_build_info OUT_HEADER)
    set(_sha    "0000000000000000000000000000000000000000")
    set(_dirty  0)
    set(_branch "unknown")

    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}" rev-parse HEAD
            OUTPUT_VARIABLE _sha_out RESULT_VARIABLE _sha_rc
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(_sha_rc EQUAL 0 AND _sha_out MATCHES "^[0-9a-f]+$")
            set(_sha "${_sha_out}")
        endif()

        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}" status --porcelain --untracked-files=no
            OUTPUT_VARIABLE _st_out RESULT_VARIABLE _st_rc
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(_st_rc EQUAL 0 AND NOT _st_out STREQUAL "")
            set(_dirty 1)
        endif()

        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}" rev-parse --abbrev-ref HEAD
            OUTPUT_VARIABLE _br_out RESULT_VARIABLE _br_rc
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(_br_rc EQUAL 0 AND NOT _br_out STREQUAL "")
            set(_branch "${_br_out}")
        endif()

        # Re-configure when HEAD moves, so the stamp cannot go stale silently.
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}" rev-parse --git-path HEAD
            OUTPUT_VARIABLE _head_path RESULT_VARIABLE _hp_rc
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(_hp_rc EQUAL 0 AND EXISTS "${CMAKE_SOURCE_DIR}/${_head_path}")
            set(_head_abs "${CMAKE_SOURCE_DIR}/${_head_path}")
        elseif(_hp_rc EQUAL 0 AND EXISTS "${_head_path}")
            set(_head_abs "${_head_path}")
        endif()
        if(DEFINED _head_abs)
            set_property(DIRECTORY "${CMAKE_SOURCE_DIR}"
                         APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_head_abs}")
            # ...and on the ref HEAD names, so a commit ON a branch re-stamps too.
            file(READ "${_head_abs}" _head_txt)
            string(STRIP "${_head_txt}" _head_txt)
            if(_head_txt MATCHES "^ref: (.+)$")
                execute_process(
                    COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}" rev-parse --git-path "${CMAKE_MATCH_1}"
                    OUTPUT_VARIABLE _ref_path RESULT_VARIABLE _rp_rc
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
                if(_rp_rc EQUAL 0)
                    if(NOT IS_ABSOLUTE "${_ref_path}")
                        set(_ref_path "${CMAKE_SOURCE_DIR}/${_ref_path}")
                    endif()
                    if(EXISTS "${_ref_path}")
                        set_property(DIRECTORY "${CMAKE_SOURCE_DIR}"
                                     APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_ref_path}")
                    endif()
                endif()
            endif()
        endif()
    endif()

    string(TIMESTAMP _stamp "%Y-%m-%dT%H:%M:%SZ" UTC)

    set(FDS_BUILD_GIT_SHA     "${_sha}")
    set(FDS_BUILD_GIT_DIRTY   "${_dirty}")
    set(FDS_BUILD_GIT_BRANCH  "${_branch}")
    set(FDS_BUILD_CONFIGURED  "${_stamp}")
    set(FDS_BUILD_SOURCE_DIR  "${CMAKE_SOURCE_DIR}")
    set(FDS_BUILD_BINARY_DIR  "${CMAKE_BINARY_DIR}")
    set(FDS_BUILD_TYPE        "${CMAKE_BUILD_TYPE}")

    configure_file("${CMAKE_SOURCE_DIR}/cmake/BuildInfo.h.in" "${OUT_HEADER}" @ONLY)
    if(_dirty)
        message(STATUS "BuildInfo: ${_sha}+dirty (${_branch})")
    else()
        message(STATUS "BuildInfo: ${_sha} (${_branch})")
    endif()
endfunction()
