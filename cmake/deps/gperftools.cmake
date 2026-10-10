# Provides gperftools, and sets CORO_GPERFTOOLS_TARGET to the target that coro
# links. Included only when WITH_GPERFTOOLS is on.
#
# CORO_GPERFTOOLS_PROVIDER selects where it comes from:
#
#   package (default) — find_package(). The environment must already make
#                       gperftools findable; a Conan build does (conanfile.py's
#                       requirements() gives the version).
#
# It is the only provider so far; see cmake/deps/libwebsockets.cmake.
set(CORO_GPERFTOOLS_PROVIDER "package" CACHE STRING
    "Where gperftools comes from: package (find_package)")
set_property(CACHE CORO_GPERFTOOLS_PROVIDER PROPERTY STRINGS package)

if(CORO_GPERFTOOLS_PROVIDER STREQUAL "package")
    find_package(gperftools REQUIRED)
    set(CORO_GPERFTOOLS_TARGET gperftools::gperftools)
else()
    message(FATAL_ERROR "CORO_GPERFTOOLS_PROVIDER is "
        "'${CORO_GPERFTOOLS_PROVIDER}', expected 'package'")
endif()
