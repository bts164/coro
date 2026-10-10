# Provides libwebsockets, and sets CORO_LIBWEBSOCKETS_TARGET to the target that
# coro links.
#
# CORO_LIBWEBSOCKETS_PROVIDER selects where it comes from:
#
#   package (default) — find_package(). The environment must already make a
#                       suitable libwebsockets findable; a Conan build does
#                       (conanfile.py's requirements() gives the version range).
#
# It is the only provider so far. One that downloads and builds a pinned
# libwebsockets (FetchContent) belongs here too, as a second branch below, so
# that the choice stays out of the root CMakeLists.txt.
set(CORO_LIBWEBSOCKETS_PROVIDER "package" CACHE STRING
    "Where libwebsockets comes from: package (find_package)")
set_property(CACHE CORO_LIBWEBSOCKETS_PROVIDER PROPERTY STRINGS package)

if(CORO_LIBWEBSOCKETS_PROVIDER STREQUAL "package")
    find_package(Libwebsockets REQUIRED)
    set(CORO_LIBWEBSOCKETS_TARGET websockets)
else()
    message(FATAL_ERROR "CORO_LIBWEBSOCKETS_PROVIDER is "
        "'${CORO_LIBWEBSOCKETS_PROVIDER}', expected 'package'")
endif()
