# cmake/platforms/pico_mqtt.cmake
#
# Defines coro_pico_mqtt — optional coroutine wrapper over lwIP's apps/mqtt
# client. Must be included AFTER pico.cmake so that coro_pico exists.
#
# Requires LWIP_MQTT 1 in the application's lwipopts.h.
#
# Usage:
#   include(${CORO_ROOT}/cmake/platforms/pico.cmake)
#   include(${CORO_ROOT}/cmake/platforms/pico_mqtt.cmake)
#   target_link_libraries(my_firmware PRIVATE coro_pico coro_pico_mqtt)

include_guard(GLOBAL)

if(NOT DEFINED CORO_ROOT)
    set(CORO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
endif()

add_library(coro_pico_mqtt STATIC
    ${CORO_ROOT}/src/pico/mqtt.cpp
)
target_include_directories(coro_pico_mqtt PRIVATE ${CORO_ROOT}/src/io/lwip)
target_link_libraries(coro_pico_mqtt PUBLIC coro_pico)
