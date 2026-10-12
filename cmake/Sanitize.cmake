# UBSan for the Pico build (CORO_PLATFORM "pico"). Desktop sanitizer builds need
# nothing from coro's CMake: the Conan profile passes -fsanitize to everything it
# builds (docker/profiles/asan, tsan).
#
# A Pico build can't take flags that way. ASan and TSan need a runtime library,
# shadow memory and (TSan) threads, none of which a microcontroller has. UBSan runs
# in trap mode instead, which needs no runtime: a violation executes an undefined
# instruction, so the firmware stops in a hard fault at the offending line. And the
# flags go on coro's own targets and sources only; adding them globally would also
# instrument the Pico SDK, lwIP and the USB stack, whose findings are not coro's
# to fix.
#
# So the flags are a variable, CORO_SANITIZE_OPTIONS, which the callers put on those
# targets and sources. docker/profiles/ubsan sets it, through the toolchain file
# Conan generates; without Conan, pass -DCORO_SANITIZE_OPTIONS="..." yourself. It is
# empty in a plain build.
if(NOT DEFINED CORO_SANITIZE_OPTIONS)
    set(CORO_SANITIZE_OPTIONS "" CACHE STRING
        "Compile options added to coro's own Pico sources (UBSan in trap mode)")
endif()
