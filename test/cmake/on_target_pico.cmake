# On-target test firmware for the Raspberry Pi Pico: the ON_TARGET files of
# tests.cmake compiled for the board, linked with GoogleTest and a runner
# (target/pico/main.cpp) that talks to target/run_on_target.py over the serial port.
# See doc/design/on_target_tests.md.
#
# Included from test/CMakeLists.txt after pico_sdk_init() and find_package(coro).

# gtest's headers read these, so the tests must see the same values the library was
# built with.
set(_CORO_GTEST_DEFINITIONS
    GTEST_HAS_PTHREAD=0              # bare metal: no threads
    GTEST_HAS_STREAM_REDIRECTION=0   # needs dup()/dup2() and temporary files
    GTEST_HAS_FILE_SYSTEM=0          # no XML/JSON report, no flag files
    GTEST_HAS_POSIX_RE=0             # newlib here has no regcomp(); use gtest's own
    GTEST_HAS_RTTI=0                 # the Pico SDK compiles with -fno-rtti
)

add_library(coro_test_gtest INTERFACE)
target_compile_definitions(coro_test_gtest INTERFACE ${_CORO_GTEST_DEFINITIONS})

# Set by conanfile.py's gtest_from_conan option. Not a plain "use it if found": a
# GoogleTest installed on the development machine must never be picked up here.
option(CORO_TEST_GTEST_FROM_CONAN "Link the Conan GoogleTest package" OFF)
if(CORO_TEST_GTEST_FROM_CONAN)
    find_package(GTest CONFIG REQUIRED)
    target_link_libraries(coro_test_gtest INTERFACE GTest::gtest GTest::gmock)
else()
    # GoogleTest compiled here from its single amalgamated source rather than
    # through its own CMakeLists.txt: that one looks for a threads package and sets
    # warning flags chosen for hosted builds. SOURCE_SUBDIR names a directory that
    # does not exist, so FetchContent downloads the sources and adds nothing to the
    # build.
    #
    # gmock is built in as well: a few shared tests define a mock (test_stream.cpp's
    # MockWaker). The linker drops it from an image whose tests use none of it.
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  sources_only_no_cmake
    )
    FetchContent_MakeAvailable(googletest)

    add_library(gtest_target STATIC
        ${googletest_SOURCE_DIR}/googletest/src/gtest-all.cc
        ${googletest_SOURCE_DIR}/googlemock/src/gmock-all.cc
    )
    target_include_directories(gtest_target
        PUBLIC  ${googletest_SOURCE_DIR}/googletest/include
                ${googletest_SOURCE_DIR}/googlemock/include
        PRIVATE ${googletest_SOURCE_DIR}/googletest
                ${googletest_SOURCE_DIR}/googlemock
    )
    target_compile_definitions(gtest_target PRIVATE ${_CORO_GTEST_DEFINITIONS})
    target_compile_options(gtest_target PRIVATE
        -fno-rtti -ffunction-sections -fdata-sections)
    # GNU extensions must stay on: gtest calls fileno(), fdopen(), strdup() and
    # isatty(), which newlib hides under a strict -std=c++NN.
    set_target_properties(gtest_target PROPERTIES CXX_EXTENSIONS ON)
    target_compile_features(gtest_target PUBLIC cxx_std_17)
    target_link_libraries(coro_test_gtest INTERFACE gtest_target)
endif()

# Call once after tests.cmake: creates one firmware image per ON_TARGET name, the
# run_on_target target that flashes and runs them all in turn, and the
# run_in_emulator target that runs them in an emulated RP2040 instead.
function(coro_add_on_target_images)
    get_property(images GLOBAL PROPERTY CORO_ON_TARGET_IMAGES)
    list(REMOVE_DUPLICATES images)

    set(uf2_files)
    set(image_targets)
    foreach(image IN LISTS images)
        set(target coro_target_${image})
        get_property(sources GLOBAL PROPERTY CORO_ON_TARGET_SOURCES_${image})

        add_executable(${target} ${CORO_TEST_ROOT}/target/pico/main.cpp ${sources})
        # with_sanitize=ubsan: the tests and the runner, like the library itself.
        # On the sources, not the target, which would also instrument the SDK
        # sources compiled into the image. See cmake/Sanitize.cmake.
        if(CORO_SANITIZE_OPTIONS)
            set_source_files_properties(
                ${CORO_TEST_ROOT}/target/pico/main.cpp ${sources}
                PROPERTIES COMPILE_OPTIONS "${CORO_SANITIZE_OPTIONS}")
        endif()
        target_include_directories(${target} PRIVATE ${CORO_TEST_ROOT})
        target_compile_definitions(${target} PRIVATE
            # The main stack is all of the 4 KB region the linker script gives
            # it (the SDK's default is half of that), with a guard below it: the
            # SDK sets up the memory protection unit so that a write to the 32
            # bytes under the stack is a hard fault, not silent corruption of
            # whatever lies there. Fiber stacks are coro's own and are not
            # covered.
            PICO_STACK_SIZE=0x1000
            PICO_USE_STACK_GUARDS=1
        )
        target_link_libraries(${target} PRIVATE
            coro::pico
            coro_test_gtest
            pico_stdlib
            # coro::pico needs it to link. Only the networking tests start lwIP,
            # and none starts the radio.
            pico_cyw43_arch_lwip_poll
        )
        # Both outputs carry the same text. USB needs only the cable the board is
        # flashed with; the UART is for a debug probe, and keeps working if USB
        # stops responding.
        pico_enable_stdio_usb(${target} 1)
        pico_enable_stdio_uart(${target} 1)
        pico_add_extra_outputs(${target})   # .uf2 / .bin / .hex

        list(APPEND image_targets ${target})
        list(APPEND uf2_files ${CMAKE_CURRENT_BINARY_DIR}/${target}.uf2)
    endforeach()

    # cmake --build <dir> --target run_on_target
    add_custom_target(run_on_target
        COMMAND python3 ${CORO_TEST_ROOT}/target/run_on_target.py ${uf2_files}
        DEPENDS ${image_targets}
        USES_TERMINAL
        VERBATIM)

    # cmake --build <dir> --target run_in_emulator
    add_custom_target(run_in_emulator
        COMMAND python3 ${CORO_TEST_ROOT}/target/run_on_target.py --emulator ${uf2_files}
        DEPENDS ${image_targets}
        USES_TERMINAL
        VERBATIM)
endfunction()
