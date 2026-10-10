# Shared sanitizer configuration for the coro library and test builds.
#
# Exposes a single cache variable instead of independent bools
# (ENABLE_ASAN/ENABLE_TSAN) so an invalid combination can't be selected.
# Conan's generate() step sets WITH_SANITIZE from the `with_sanitize` recipe
# option (see conanfile.py), which itself defaults from the CORO_SANITIZE
# environment variable — see .envrc.sample.
#
# Desktop: the flags are added to everything this build compiles and links.
#
# Pico (CORO_PLATFORM "pico"): only `ubsan` exists, and nothing is added globally.
# ASan and TSan need a runtime library, shadow memory and (TSan) threads, none of
# which a microcontroller has. UBSan runs in trap mode instead, which needs no
# runtime: a violation executes an undefined instruction, so the firmware stops
# in a hard fault at the offending line. The flags are returned in
# CORO_SANITIZE_OPTIONS for the caller to put on coro's own targets and sources;
# adding them globally would also instrument the Pico SDK, lwIP and the USB
# stack, whose findings are not coro's to fix.

set(WITH_SANITIZE "none" CACHE STRING "Sanitizer to build with: none, asan, tsan, ubsan")
set_property(CACHE WITH_SANITIZE PROPERTY STRINGS none asan tsan ubsan)

set(CORO_SANITIZE_OPTIONS)

if(CORO_PLATFORM STREQUAL "pico")
    if(WITH_SANITIZE STREQUAL "ubsan")
        # vptr is left out: it needs RTTI and a runtime, and cannot trap.
        set(CORO_SANITIZE_OPTIONS
            -fsanitize=undefined -fno-sanitize=vptr -fsanitize-trap=undefined)
    elseif(NOT WITH_SANITIZE STREQUAL "none")
        message(FATAL_ERROR
            "WITH_SANITIZE on Pico must be none or ubsan (got '${WITH_SANITIZE}')")
    endif()
elseif(WITH_SANITIZE STREQUAL "asan")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
elseif(WITH_SANITIZE STREQUAL "tsan")
    add_compile_options(-fsanitize=thread -g)
    add_link_options(-fsanitize=thread)
elseif(WITH_SANITIZE STREQUAL "ubsan")
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=undefined)
elseif(NOT WITH_SANITIZE STREQUAL "none")
    message(FATAL_ERROR
        "WITH_SANITIZE must be one of: none, asan, tsan, ubsan (got '${WITH_SANITIZE}')")
endif()
