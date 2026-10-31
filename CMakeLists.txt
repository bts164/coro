cmake_minimum_required(VERSION 3.21)

# CORO_PLATFORM selects which target this build produces:
#   desktop (default) — the epoll-based "coro" library
#   pico               — coro_pico/coro_pico_hal[/coro_pico_mqtt] for RP2040/RP2350
# Set via -DCORO_PLATFORM=pico, or automatically by conanfile.py when
# settings.os == "baremetal". See doc/design/lwip_config.md and
# doc/design/pico_led_conan_packaging.md.
set(CORO_PLATFORM "desktop" CACHE STRING "Target platform: desktop or pico")
set_property(CACHE CORO_PLATFORM PROPERTY STRINGS desktop pico)

# CORO_VERSION is the version embedded in the built binary (coro/version.h). A
# Conan build passes it in: conanfile.py's generate() sets it to the package
# version. Any other build derives it here, from the source archive's stamp or
# from git, and gets "unknown" when neither is there. -DCORO_VERSION=<version>
# sets it by hand. See doc/versioning.md.
if(NOT DEFINED CORO_VERSION)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CoroVersion.cmake)
    coro_derive_version(CORO_VERSION)
endif()
message(STATUS "coro version: ${CORO_VERSION}")
# project(VERSION) takes only the numeric part.
if(CORO_VERSION MATCHES "^([0-9]+\\.[0-9]+\\.[0-9]+)")
    set(CORO_NUMERIC_VERSION "${CMAKE_MATCH_1}")
else()
    set(CORO_NUMERIC_VERSION "0.0.0")
endif()

if(CORO_PLATFORM STREQUAL "pico")
    # pico_sdk_init() must run before project() — see doc/design/pico_port.md.
    if(NOT DEFINED ENV{PICO_SDK_PATH} OR "$ENV{PICO_SDK_PATH}" STREQUAL "")
        message(FATAL_ERROR "PICO_SDK_PATH environment variable is not set. "
            "Export it in the shell running cmake/conan, or add it under "
            "[buildenv] in the Conan host profile.")
    endif()
    include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
    project(coro VERSION ${CORO_NUMERIC_VERSION} LANGUAGES CXX C ASM)
    set(PICO_CXX_ENABLE_EXCEPTIONS 1)
    pico_sdk_init()
else()
    project(coro VERSION ${CORO_NUMERIC_VERSION} LANGUAGES CXX)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CORO_GENERATED_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/coro_version.h.in
    ${CORO_GENERATED_INCLUDE_DIR}/coro/version.h
    @ONLY)

if(CORO_PLATFORM STREQUAL "pico")
    # CORO_PICO_WITH_MQTT (declared as an option() inside pico.cmake) also
    # controls LWIP_MQTT in the bundled default lwipopts.h that pico.cmake
    # generates — see that file's comments and doc/design/lwip_config.md.
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/platforms/pico.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/platforms/pico_hal.cmake)
    if(CORO_PICO_WITH_MQTT)
        include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/platforms/pico_mqtt.cmake)
    endif()

    # Empty unless this is a UBSan build; see cmake/Sanitize.cmake.
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Sanitize.cmake)
    if(CORO_SANITIZE_OPTIONS)
        target_compile_options(coro_pico     PRIVATE ${CORO_SANITIZE_OPTIONS})
        target_compile_options(coro_pico_hal PRIVATE ${CORO_SANITIZE_OPTIONS})
        if(CORO_PICO_WITH_MQTT)
            target_compile_options(coro_pico_mqtt PRIVATE ${CORO_SANITIZE_OPTIONS})
        endif()
    endif()

    target_include_directories(coro_pico PUBLIC ${CORO_GENERATED_INCLUDE_DIR})
    install(FILES ${CORO_GENERATED_INCLUDE_DIR}/coro/version.h DESTINATION include/coro)

    install(TARGETS coro_pico coro_pico_hal)
    if(CORO_PICO_WITH_MQTT)
        install(TARGETS coro_pico_mqtt)
    endif()
    # The following are POSIX/libwebsockets-only, or are desktop-only Executor
    # implementations other than CurrentThreadExecutor (the only Executor pico uses).
    # Excluded so they can't be included at all on the pico package, rather than
    # relying on a header guard to produce a clean error if a user includes one by
    # mistake. The I/O driver, the sockets and the sys seams are shared, so they are
    # installed. The patterns match by file name, so "signal.h", "file.h" and
    # "pipe.h" also leave out the desktop-only sys seams of those names.
    install(DIRECTORY include/coro
        DESTINATION include
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
        PATTERN "signal.h" EXCLUDE
        PATTERN "ws_stream.h" EXCLUDE
        PATTERN "ws_listener.h" EXCLUDE
        PATTERN "file.h" EXCLUDE
        PATTERN "file.hpp" EXCLUDE
        PATTERN "work_sharing_executor.h" EXCLUDE
        PATTERN "work_stealing_executor.h" EXCLUDE
        PATTERN "pipe.h" EXCLUDE
        PATTERN "pipe.hpp" EXCLUDE
        # spawn_blocking.h declares BlockingPool unconditionally (no CORO_PICO guard)
        # and calls Runtime::blocking_pool(), which only exists in the non-CORO_PICO
        # Runtime — desktop-only, would fail to compile on pico. blocking_task.h and
        # blocking_cancel.h are its (and blocking_wait's) desktop-only support.
        PATTERN "spawn_blocking.h" EXCLUDE
        PATTERN "blocking_task.h" EXCLUDE
        PATTERN "blocking_cancel.h" EXCLUDE)
else()
    option(WITH_GPERFTOOLS "Build with gperftools" OFF)
    option(CORO_USE_LOCAL_RUN_QUEUE "Use lock-free local run queue in WorkStealingExecutor (experimental)" ON)
    # Backend for the runtime's trace points (doc/design/tracing.md). "none" compiles
    # them out.
    set(CORO_TRACING "none" CACHE STRING "Tracing backend: none or tracy")
    set_property(CACHE CORO_TRACING PROPERTY STRINGS none tracy)
    if(NOT CORO_TRACING MATCHES "^(none|tracy)$")
        message(FATAL_ERROR "CORO_TRACING is '${CORO_TRACING}', expected 'none' or 'tracy'")
    endif()

    # Each dependency has a script in cmake/deps/ that provides it and names
    # the target to link. CORO_<DEPENDENCY>_PROVIDER selects how, one
    # dependency at a time.
    if(WITH_GPERFTOOLS)
        include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/deps/gperftools.cmake)
    endif()
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/deps/libwebsockets.cmake)
    if(CORO_TRACING STREQUAL "tracy")
        include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/deps/tracy.cmake)
    endif()

    file(GLOB_RECURSE CORO_PUBLIC_HEADERS "include/coro/*.h" "include/coro/*.hpp")
    message(STATUS ${CORO_PUBLIC_HEADERS})
    if(WIN32)
        # The IOCP/AFD sys backend, console control events and Win32 fibers. Pipe is
        # not ported yet; see doc/design/windows_port.md, "Scope".
        set(CORO_PLATFORM_SOURCES
            src/detail/fiber_context_windows.cpp
            src/detail/sys/windows/win.cpp
            src/detail/sys/windows/poller_iocp.cpp
            src/detail/sys/windows/socket_windows.cpp
            src/detail/sys/windows/tcp_windows.cpp
            src/detail/sys/windows/udp_windows.cpp
            src/detail/sys/windows/dns_windows.cpp
            src/detail/sys/windows/file_windows.cpp
            src/detail/sys/windows/signal_windows.cpp
            src/io/signal.cpp
        )
    else()
        set(CORO_PLATFORM_SOURCES
            src/detail/fiber_context_posix.cpp
            src/detail/sys/posix/dns_posix.cpp
            src/detail/sys/posix/file_posix.cpp
            src/detail/sys/posix/notify_posix.cpp
            src/detail/sys/posix/pipe_posix.cpp
            src/detail/sys/posix/signal_posix.cpp
            src/detail/sys/posix/poller_epoll.cpp
            src/detail/sys/posix/socket_posix.cpp
            src/detail/sys/posix/tcp_posix.cpp
            src/detail/sys/posix/udp_posix.cpp
            src/io/pipe.cpp
            src/io/signal.cpp
        )
    endif()

    add_library(coro
        # runtime/
        src/runtime/runtime.cpp
        src/runtime/executor.cpp
        src/runtime/current_thread_executor.cpp
        src/runtime/work_sharing_executor.cpp
        src/runtime/work_stealing_executor.cpp
        src/runtime/io_driver.cpp
        # task/
        src/task/task.cpp
        src/task/waker.cpp
        src/task/blocking_pool.cpp
        src/task/context.cpp
        src/task/fiber.cpp
        # detail/
        src/detail/socket_state.cpp
        src/detail/timer_queue.cpp
        # sync/
        src/sync/sleep.cpp
        # io/
        src/io/tcp_stream.cpp
        src/io/tcp_listener.cpp
        src/io/ws_stream.cpp
        src/io/ws_listener.cpp
        src/io/ws_service.cpp
        src/io/file.cpp
        src/io/lookup_host.cpp
        src/io/circular_byte_buffer.cpp
        src/io/socket_address.cpp
        src/io/udp_socket.cpp
        # platform: sys backend and the parts not yet ported everywhere
        ${CORO_PLATFORM_SOURCES}
    )
    if(WIN32)
        # Windows 10 or later. Winsock is the only library linked: the NT calls the
        # poller needs are looked up in ntdll at run time (src/detail/sys/windows/nt.h).
        target_compile_definitions(coro PRIVATE
            WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
        target_link_libraries(coro PUBLIC ws2_32)
    endif()
    if(MSVC)
        # Standard conformance, a __cplusplus that tells the truth, UTF-8 sources.
        # C4324 ("structure was padded due to alignment specifier") is off: the padding
        # is the point of every alignas in coro, which keeps hot fields on their own
        # cache line (TaskBase, OwnedTasks::Shard, the work-stealing queues).
        target_compile_options(coro PRIVATE /permissive- /Zc:__cplusplus /utf-8 /EHsc /W4
                                            /wd4324)
    endif()
    if(CORO_USE_LOCAL_RUN_QUEUE)
        target_compile_definitions(coro PUBLIC CORO_USE_LOCAL_RUN_QUEUE)
    endif()
    target_include_directories(coro PUBLIC include ${CORO_GENERATED_INCLUDE_DIR})
    target_link_libraries(coro PUBLIC
        $<$<BOOL:${WITH_GPERFTOOLS}>:${CORO_GPERFTOOLS_TARGET}>
        ${CORO_LIBWEBSOCKETS_TARGET}
        $<$<BOOL:${CORO_LIBWEBSOCKETS_USES_LIBCAP}>:cap>)
    if(WITH_GPERFTOOLS AND MSVC)
        # A static tcmalloc on Windows works by patching the CRT's malloc at startup,
        # which nothing references, so the linker drops it unless each executable
        # forces it in, as gperftools' own CMakeLists.txt does. conanfile.py's
        # package_info() passes the same flag to consumers of the package.
        target_link_options(coro INTERFACE "/INCLUDE:__tcmalloc")
    endif()
    if(CORO_TRACING STREQUAL "tracy")
        # PRIVATE definition: only src/detail/trace.h reads it, and no public header
        # includes that. The link is PUBLIC so that a program adding its own zones
        # uses the same Tracy client as the library.
        target_compile_definitions(coro PRIVATE CORO_TRACE_TRACY)
        target_link_libraries(coro PUBLIC ${CORO_TRACY_TARGET})
    endif()

    install(TARGETS coro)
    install(FILES ${CORO_GENERATED_INCLUDE_DIR}/coro/version.h DESTINATION include/coro)
    # include/coro/pico/ is intentionally excluded from the installed package.
    # Those headers require CORO_PICO and the Pico SDK — they have no use on
    # desktop platforms and would fail to compile without the SDK headers.
    # The pico unit tests (test/pico/) are a source-build-only artifact: anyone
    # running them is building from source and has the headers directly. Consumers
    # of the installed Conan package never need them.
    #
    # include/coro/io/lwip/ is excluded for the same reason, just not caught by the
    # "pico" pattern above since it lives directly under io/ rather than under pico/.
    # lwip_callback_result.h backs pico/mqtt.cpp, which has no use here either.
    install(DIRECTORY include/coro
        DESTINATION include
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
        PATTERN "lwip" EXCLUDE
        # isr_event.h is whole-file #ifdef CORO_PICO — harmless no-op if included on
        # desktop, but has no use here (no hardware ISRs), so excluded for consistency
        # with the rest of this list rather than relying on the guard.
        PATTERN "isr_event.h" EXCLUDE
        PATTERN "pico" EXCLUDE
    )
endif()
