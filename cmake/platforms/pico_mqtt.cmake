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
# Needed for <lwip/apps/mqtt.h> et al. — same as coro_pico's own PRIVATE link
# below; coro_pico doesn't re-export these (they're PRIVATE there too), so
# coro_pico_mqtt must link them itself rather than relying on coro_pico to
# pass them through.
target_link_libraries(coro_pico_mqtt PRIVATE pico_stdlib pico_cyw43_arch_lwip_poll)
# pico_lwip_mqtt is an INTERFACE library that attaches lwIP's apps/mqtt.c
# (mqtt_client_new/connect/publish/sub_unsub/disconnect/free) as sources —
# linking it PRIVATE compiles that implementation straight into
# libcoro_pico_mqtt.a, so consumers only need coro::pico_mqtt, not knowledge
# of this raw Pico SDK target.
target_link_libraries(coro_pico_mqtt PRIVATE pico_lwip_mqtt)
