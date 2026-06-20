# Shared sanitizer configuration for the coro library and test builds.
#
# Exposes a single tri-state cache variable instead of two independent bools
# (ENABLE_ASAN/ENABLE_TSAN) so an invalid combination can't be selected.
# Conan's generate() step sets WITH_SANITIZE from the `with_sanitize` recipe
# option (see conanfile.py), which itself defaults from the CORO_SANITIZE
# environment variable — see .envrc.sample.

set(WITH_SANITIZE "none" CACHE STRING "Sanitizer to build with: none, asan, tsan")
set_property(CACHE WITH_SANITIZE PROPERTY STRINGS none asan tsan)

if(WITH_SANITIZE STREQUAL "asan")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
elseif(WITH_SANITIZE STREQUAL "tsan")
    add_compile_options(-fsanitize=thread -g)
    add_link_options(-fsanitize=thread)
elseif(NOT WITH_SANITIZE STREQUAL "none")
    message(FATAL_ERROR "WITH_SANITIZE must be one of: none, asan, tsan (got '${WITH_SANITIZE}')")
endif()
