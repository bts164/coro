# Provides libwebsockets, and sets CORO_LIBWEBSOCKETS_TARGET to the target that
# coro links and CORO_LIBWEBSOCKETS_USES_LIBCAP to whether that libwebsockets
# was built with libcap.
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

# libwebsockets links libcap whenever its own build finds it, and records that
# in its lws_config.h as LWS_HAVE_LIBCAP. Its Conan package doesn't declare it,
# so coro links libcap itself. A shared coro has the static libwebsockets linked
# into it, so it then records libcap as its own dependency, matching the
# libwebsockets it was built with. conanfile.py's package_info() does the same
# for the libwebsockets a consumer links.
#
# The check compiles against the libwebsockets target, so it reads the same
# lws_config.h that coro builds with, and it reruns on every configure, since a
# reused build folder may see a rebuilt libwebsockets.
function(coro_libwebsockets_uses_libcap out)
    include(CheckCXXSourceCompiles)
    include(CMakePushCheckState)
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_LIBRARIES ${CORO_LIBWEBSOCKETS_TARGET})
    # Only the header matters, so the check doesn't link.
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    # Conan's targets give their include directories only under the build's own
    # configuration ($<CONFIG:Release>), which the check has to match.
    if(CMAKE_BUILD_TYPE)
        set(CMAKE_TRY_COMPILE_CONFIGURATION ${CMAKE_BUILD_TYPE})
    endif()

    unset(CORO_LWS_CONFIG_FOUND CACHE)
    check_cxx_source_compiles("#include <lws_config.h>
        int main() { return 0; }" CORO_LWS_CONFIG_FOUND)
    if(NOT CORO_LWS_CONFIG_FOUND)
        message(FATAL_ERROR "Cannot include libwebsockets' lws_config.h through "
            "${CORO_LIBWEBSOCKETS_TARGET}, so cannot tell whether it needs libcap")
    endif()

    unset(CORO_LWS_HAVE_LIBCAP CACHE)
    check_cxx_source_compiles("#include <lws_config.h>
        #ifndef LWS_HAVE_LIBCAP
        #error libwebsockets was built without libcap
        #endif
        int main() { return 0; }" CORO_LWS_HAVE_LIBCAP)
    cmake_pop_check_state()
    set(${out} ${CORO_LWS_HAVE_LIBCAP} PARENT_SCOPE)
endfunction()
coro_libwebsockets_uses_libcap(CORO_LIBWEBSOCKETS_USES_LIBCAP)
