# coro_test() — registers one test source file with every build it applies to.
# tests.cmake calls it once per file; that list is the only place a test file is
# named. See doc/design/on_target_tests.md.
#
#   coro_test(<source> [DESKTOP] [PICO_STUB] [HOST_LINK <lib>...] [ON_TARGET <image>])
#
#   DESKTOP            Its own executable on the development machine, linked with
#                      the desktop coro library. Named after the file.
#   PICO_STUB          Part of test_pico_suite: one executable on the development
#                      machine, built from the Pico configuration of the core
#                      (CORO_PICO, current-thread executor only) with the SDK calls
#                      stubbed out.
#   HOST_LINK <lib>... Its own executable on the development machine, linked with
#                      the given libraries instead of the desktop coro library: the
#                      Pico configuration against stubs or a host lwIP
#                      (cmake/host_pico_support.cmake). Named after the file, or
#                      <file>_pico when the file is also DESKTOP.
#   ON_TARGET <image>  Part of the firmware image coro_target_<image>, run on the
#                      board. Files naming the same image share it.
#
# A file may combine any of these. Which tests inside a file apply to a platform is
# decided in the source, with the preprocessor.
#
# CORO_PLATFORM selects what is built: "desktop" creates the DESKTOP, PICO_STUB and
# HOST_LINK targets, "pico" the ON_TARGET images.

function(_coro_host_test name)
    cmake_parse_arguments(PARSE_ARGV 1 A "" "" "SOURCES;LIBS")
    add_executable(${name} ${A_SOURCES})
    target_include_directories(${name} PRIVATE ${CORO_TEST_ROOT})
    target_link_libraries(${name} PRIVATE
        ${A_LIBS} GTest::gtest GTest::gtest_main GTest::gmock)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

function(coro_test source)
    cmake_parse_arguments(PARSE_ARGV 1 T "DESKTOP;PICO_STUB" "ON_TARGET" "HOST_LINK")
    if(T_UNPARSED_ARGUMENTS OR T_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "coro_test(${source}): bad arguments: "
            "${T_UNPARSED_ARGUMENTS} ${T_KEYWORDS_MISSING_VALUES}")
    endif()
    if(NOT (T_DESKTOP OR T_PICO_STUB OR T_HOST_LINK OR T_ON_TARGET))
        message(FATAL_ERROR "coro_test(${source}): not assigned to any build")
    endif()
    if(NOT EXISTS ${CORO_TEST_ROOT}/${source})
        message(FATAL_ERROR "coro_test(${source}): no such file under ${CORO_TEST_ROOT}")
    endif()

    cmake_path(GET source STEM name)
    set(path ${CORO_TEST_ROOT}/${source})

    if(CORO_PLATFORM STREQUAL "pico")
        if(T_ON_TARGET)
            set_property(GLOBAL APPEND PROPERTY CORO_ON_TARGET_IMAGES ${T_ON_TARGET})
            set_property(GLOBAL APPEND PROPERTY
                CORO_ON_TARGET_SOURCES_${T_ON_TARGET} ${path})
        endif()
        return()
    endif()

    if(T_DESKTOP)
        _coro_host_test(${name} SOURCES ${path} LIBS coro::coro)
    endif()
    if(T_HOST_LINK)
        if(T_DESKTOP)
            set(name ${name}_pico)
        endif()
        _coro_host_test(${name} SOURCES ${path} LIBS ${T_HOST_LINK})
    endif()
    if(T_PICO_STUB)
        set_property(GLOBAL APPEND PROPERTY CORO_PICO_STUB_SOURCES ${path})
    endif()
endfunction()

# Call once after tests.cmake, in a desktop build: creates test_pico_suite from
# the PICO_STUB files. CORO_PICO narrows AllExecutors (executor_traits.h) to the
# current-thread executor, so that is the only one exercised.
function(coro_add_pico_stub_suite)
    get_property(sources GLOBAL PROPERTY CORO_PICO_STUB_SOURCES)
    _coro_host_test(test_pico_suite SOURCES ${sources} LIBS coro_pico_core)
endfunction()
