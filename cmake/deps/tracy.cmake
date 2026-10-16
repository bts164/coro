# Provides the Tracy client, and sets CORO_TRACY_TARGET to the target that coro
# links. Included only when CORO_TRACING is "tracy" (doc/design/tracing.md).
#
# CORO_TRACY_PROVIDER selects where it comes from:
#
#   package (default) — find_package(). The environment must already make
#                       Tracy findable; a Conan build does (conanfile.py's
#                       requirements() gives the version and its options).
#
# It is the only provider so far; see cmake/deps/libwebsockets.cmake.
set(CORO_TRACY_PROVIDER "package" CACHE STRING
    "Where Tracy comes from: package (find_package)")
set_property(CACHE CORO_TRACY_PROVIDER PROPERTY STRINGS package)

if(CORO_TRACY_PROVIDER STREQUAL "package")
    find_package(Tracy REQUIRED)
    set(CORO_TRACY_TARGET Tracy::TracyClient)
else()
    message(FATAL_ERROR "CORO_TRACY_PROVIDER is "
        "'${CORO_TRACY_PROVIDER}', expected 'package'")
endif()
