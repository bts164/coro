# coro_derive_version(<out_var>)
#
# Sets <out_var> to coro's version, for a build that Conan is not driving. (A
# Conan build passes CORO_VERSION instead; see conanfile.py's generate().)
#
# This is the same derivation as conan_version.py, which is the reference: keep
# the two in step. It reads, in order:
#
#   1. .git_archival.txt, in a source archive. git wrote its `git describe`
#      result into it while making the archive (.gitattributes, export-subst).
#   2. `git describe`, in a git checkout.
#
# and gives "unknown" when neither is available. conan_version.py's overrides
# (CORO_VERSION_OVERRIDE, .coro_version_override) have no counterpart here:
# pass -DCORO_VERSION=<version> to set the version by hand.
#
# See doc/versioning.md.
function(coro_derive_version out_var)
    set(${out_var} "unknown" PARENT_SCOPE)
    get_filename_component(source_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)

    # 1. A source archive. In a git checkout the file still holds its
    #    unexpanded "$Format:...$" placeholder, which is skipped.
    set(describe "")
    if(EXISTS "${source_dir}/.git_archival.txt")
        file(STRINGS "${source_dir}/.git_archival.txt" line REGEX "^describe:" LIMIT_COUNT 1)
        string(REGEX REPLACE "^describe:" "" value "${line}")
        string(STRIP "${value}" value)
        string(FIND "${value}" "Format:" placeholder)
        if(placeholder EQUAL -1)
            set(describe "${value}")
        endif()
    endif()

    # 2. A git checkout. Only where this directory is itself the top of one, so
    #    that a source tree unpacked inside some other repository is not
    #    described by that repository's tags.
    set(from_git FALSE)
    if(describe STREQUAL "" AND EXISTS "${source_dir}/.git")
        find_program(CORO_GIT_EXECUTABLE git)
        mark_as_advanced(CORO_GIT_EXECUTABLE)
        if(CORO_GIT_EXECUTABLE)
            execute_process(
                COMMAND "${CORO_GIT_EXECUTABLE}" describe --tags --long --dirty
                        --match "v[0-9]*.[0-9]*.[0-9]*"
                WORKING_DIRECTORY "${source_dir}"
                OUTPUT_VARIABLE describe
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
                RESULT_VARIABLE result)
            if(result EQUAL 0)
                set(from_git TRUE)
            else()
                set(describe "")
            endif()
        endif()
    endif()

    set(dirty FALSE)
    if("${describe}" MATCHES "-dirty$")
        set(dirty TRUE)
        string(REGEX REPLACE "-dirty$" "" describe "${describe}")
    endif()

    # vX.Y.Z or vX.Y.Z-rc.N, then "-<count>-g<sha>". `git describe --long`
    # always appends that part; the archived form has it only when the commit
    # is not itself tagged.
    if(NOT "${describe}" MATCHES
            "^v([0-9]+)\\.([0-9]+)\\.([0-9]+)(-rc\\.([0-9]+))?(-([0-9]+)-g([0-9a-f]+))?$")
        return()
    endif()
    set(v_major "${CMAKE_MATCH_1}")
    set(v_minor "${CMAKE_MATCH_2}")
    set(v_patch "${CMAKE_MATCH_3}")
    set(rc    "${CMAKE_MATCH_5}")
    set(count "${CMAKE_MATCH_7}")
    set(sha   "${CMAKE_MATCH_8}")
    if(count STREQUAL "")
        set(count 0)
    endif()

    set(exact FALSE)
    if(count EQUAL 0 AND NOT dirty)
        set(exact TRUE)
    endif()

    if(NOT rc STREQUAL "")
        # An rc tag already names the pending release: no bump.
        set(version "${v_major}.${v_minor}.${v_patch}-rc.${rc}")
        if(NOT exact)
            string(APPEND version ".dev.${count}+g${sha}")
        endif()
    elseif(exact)
        set(version "${v_major}.${v_minor}.${v_patch}")
    else()
        # conandata.yml's next_bump says which component the next release bumps.
        set(next_bump "patch")
        if(EXISTS "${source_dir}/conandata.yml")
            file(STRINGS "${source_dir}/conandata.yml" line REGEX "^next_bump:" LIMIT_COUNT 1)
            if(NOT line STREQUAL "")
                string(REGEX REPLACE "^next_bump:[ \t]*([A-Za-z]*).*$" "\\1" next_bump "${line}")
            endif()
        endif()
        if(next_bump STREQUAL "major")
            math(EXPR v_major "${v_major} + 1")
            set(v_minor 0)
            set(v_patch 0)
        elseif(next_bump STREQUAL "minor")
            math(EXPR v_minor "${v_minor} + 1")
            set(v_patch 0)
        elseif(next_bump STREQUAL "patch")
            math(EXPR v_patch "${v_patch} + 1")
        else()
            message(FATAL_ERROR "coro: conandata.yml's next_bump is '${next_bump}', "
                "expected 'patch', 'minor', or 'major'")
        endif()
        set(version "${v_major}.${v_minor}.${v_patch}-dev.${count}+g${sha}")
    endif()

    if(NOT exact)
        # Build metadata: the branch (a checkout only; an archive has none),
        # then the dirty flag. The CI-provided ref comes first, as in
        # conan_version.py's _branch_metadata().
        set(branch "")
        if(from_git)
            if(NOT "$ENV{GITHUB_HEAD_REF}" STREQUAL "")
                set(branch "$ENV{GITHUB_HEAD_REF}")
            elseif(NOT "$ENV{GITHUB_REF_NAME}" STREQUAL "")
                set(branch "$ENV{GITHUB_REF_NAME}")
            else()
                execute_process(
                    COMMAND "${CORO_GIT_EXECUTABLE}" branch --show-current
                    WORKING_DIRECTORY "${source_dir}"
                    OUTPUT_VARIABLE branch
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
            endif()
        endif()
        if(NOT branch STREQUAL "")
            string(REGEX REPLACE "[^0-9A-Za-z-]" "-" branch "${branch}")
            string(SUBSTRING "${branch}" 0 12 branch)
            string(REGEX REPLACE "^-+" "" branch "${branch}")
            string(REGEX REPLACE "-+$" "" branch "${branch}")
        endif()
        if(NOT branch STREQUAL "")
            string(APPEND version ".${branch}")
        endif()
        if(dirty)
            string(APPEND version ".dirty")
        endif()
    endif()

    set(${out_var} "${version}" PARENT_SCOPE)
endfunction()
