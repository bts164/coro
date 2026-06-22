# Re-establishes the pico_stdlib/pico_cyw43_arch_lwip_poll link requirement
# that coro_pico_mqtt has PRIVATE in-tree (cmake/platforms/pico_mqtt.cmake) —
# needed for <lwip/apps/mqtt.h> et al. — which a packaged Conan cpp_info
# can't express on its own: these are raw Pico SDK CMake targets, freshly
# created by each consuming project's own pico_sdk_init() call, not anything
# coro's package exports. Same situation as pico_hal_build_module.cmake's
# hardware_dma/hardware_irq/hardware_pio re-link, for the same reason.
# CMakeDeps include()s this from coro-config.cmake, right after
# coroTargets.cmake declares coro::pico_mqtt (see coro's package_info()/
# layout(), root-level "cmake_build_modules" property — CMakeDeps only
# supports this at the package level, not per-component), so any consumer
# just needs find_package(coro) after its own pico_sdk_init() — no manual
# relinking required.
if(TARGET coro::pico_mqtt)
    if(TARGET pico_stdlib)
        target_link_libraries(coro::pico_mqtt INTERFACE pico_stdlib)
    endif()
    if(TARGET pico_cyw43_arch_lwip_poll)
        target_link_libraries(coro::pico_mqtt INTERFACE pico_cyw43_arch_lwip_poll)
    endif()
endif()
