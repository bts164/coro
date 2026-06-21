# cmake/platforms/pico_mqtt.cmake
#
# Defines coro_pico_mqtt — optional coroutine wrapper over lwIP's apps/mqtt
# client. Must be included AFTER pico.cmake so that coro_pico (and
# CORO_PICO_LWIPOPTS_INCLUDE_DIR) exist.
#
# Requires CORO_PICO_WITH_MQTT ON (or a custom CORO_PICO_LWIPOPTS_DIR that
# defines LWIP_MQTT 1 itself) when pico.cmake was included — lwipopts.h is
# shared by every target in the build, so this can't set LWIP_MQTT on its
# own without risking a binary built against two different lwIP configs.
#
# Usage:
#   set(CORO_PICO_WITH_MQTT ON)
#   include(${CORO_ROOT}/cmake/platforms/pico.cmake)
#   include(${CORO_ROOT}/cmake/platforms/pico_mqtt.cmake)
#   target_link_libraries(my_firmware PRIVATE coro_pico coro_pico_mqtt)

include_guard(GLOBAL)

if(NOT DEFINED CORO_ROOT)
    set(CORO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
endif()

if(NOT DEFINED CORO_PICO_LWIPOPTS_INCLUDE_DIR)
    message(FATAL_ERROR "pico_mqtt.cmake must be included after pico.cmake (CORO_PICO_LWIPOPTS_INCLUDE_DIR is not set).")
endif()

add_library(coro_pico_mqtt STATIC
    ${CORO_ROOT}/src/pico/mqtt.cpp
)
target_include_directories(coro_pico_mqtt PRIVATE ${CORO_ROOT}/src/io/lwip ${CORO_PICO_LWIPOPTS_INCLUDE_DIR})
target_link_libraries(coro_pico_mqtt PUBLIC coro_pico)
