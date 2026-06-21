cmake_minimum_required(VERSION 3.21)

# CORO_PLATFORM selects which target this build produces:
#   desktop (default) — the libuv-based "coro" library
#   pico               — coro_pico/coro_pico_hal[/coro_pico_mqtt] for RP2040/RP2350
# Set via -DCORO_PLATFORM=pico, or automatically by conanfile.py when
# settings.os == "baremetal". See doc/design/lwip_config.md and
# doc/design/pico_led_conan_packaging.md.
set(CORO_PLATFORM "desktop" CACHE STRING "Target platform: desktop or pico")
set_property(CACHE CORO_PLATFORM PROPERTY STRINGS desktop pico)

if(CORO_PLATFORM STREQUAL "pico")
    # pico_sdk_init() must run before project() — see doc/design/pico_port.md.
    if(NOT DEFINED ENV{PICO_SDK_PATH} OR "$ENV{PICO_SDK_PATH}" STREQUAL "")
        message(FATAL_ERROR "PICO_SDK_PATH environment variable is not set. "
            "Export it in the shell running cmake/conan, or add it under "
            "[buildenv] in the Conan host profile.")
    endif()
    include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
    project(coro VERSION 0.1.0 LANGUAGES CXX C ASM)
    set(PICO_CXX_ENABLE_EXCEPTIONS 1)
    pico_sdk_init()
else()
    project(coro VERSION 0.1.0 LANGUAGES CXX)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CORO_PLATFORM STREQUAL "pico")
    # CORO_PICO_WITH_MQTT (declared as an option() inside pico.cmake) also
    # controls LWIP_MQTT in the bundled default lwipopts.h that pico.cmake
    # generates — see that file's comments and doc/design/lwip_config.md.
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/platforms/pico.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/platforms/pico_hal.cmake)
    if(CORO_PICO_WITH_MQTT)
        include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/platforms/pico_mqtt.cmake)
    endif()

    install(TARGETS coro_pico coro_pico_hal)
    if(CORO_PICO_WITH_MQTT)
        install(TARGETS coro_pico_mqtt)
    endif()
    install(DIRECTORY include/coro
        DESTINATION include
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
else()
    option(WITH_GPERFTOOLS "Build with gperftools" OFF)
    option(CORO_USE_LOCAL_RUN_QUEUE "Use lock-free local run queue in WorkStealingExecutor (experimental)" ON)

    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Sanitize.cmake)

    if(WITH_GPERFTOOLS)
        find_package(gperftools REQUIRED)
    endif()
    find_package(libuv REQUIRED)
    find_package(Libwebsockets REQUIRED)

    file(GLOB_RECURSE CORO_PUBLIC_HEADERS "include/coro/*.h" "include/coro/*.hpp")
    message(STATUS ${CORO_PUBLIC_HEADERS})
    add_library(coro
        # runtime/
        src/runtime/runtime.cpp
        src/runtime/executor.cpp
        src/runtime/single_threaded_uv_executor.cpp
        src/runtime/single_threaded_executor.cpp
        src/runtime/current_thread_executor.cpp
        src/runtime/work_sharing_executor.cpp
        src/runtime/work_stealing_executor.cpp
        # task/
        src/task/task.cpp
        src/task/waker.cpp
        src/task/blocking_pool.cpp
        src/task/context.cpp
        # sync/
        src/sync/cancellation_token.cpp
        # io/
        src/io/tcp_stream.cpp
        src/io/tcp_listener.cpp
        src/io/ws_stream.cpp
        src/io/ws_listener.cpp
        src/io/file.cpp
        #src/io/pipe.cpp
        src/io/circular_byte_buffer.cpp
    )
    if(CORO_USE_LOCAL_RUN_QUEUE)
        target_compile_definitions(coro PUBLIC CORO_USE_LOCAL_RUN_QUEUE)
    endif()
    target_include_directories(coro PUBLIC include)
    target_link_libraries(coro PUBLIC
        libuv::uv_a
        $<$<BOOL:${WITH_GPERFTOOLS}>:gperftools::gperftools>
        websockets)

    install(TARGETS coro)
    # include/coro/pico/ is intentionally excluded from the installed package.
    # Those headers require CORO_PICO and the Pico SDK — they have no use on
    # desktop platforms and would fail to compile without the SDK headers.
    # The pico unit tests (test/pico/) are a source-build-only artifact: anyone
    # running them is building from source and has the headers directly. Consumers
    # of the installed Conan package never need them.
    install(DIRECTORY include/coro
        DESTINATION include
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
        PATTERN "pico" EXCLUDE
    )
endif()
