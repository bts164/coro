# Re-establishes the hardware_dma/hardware_irq/hardware_pio link requirement
# that coro_pico_hal has PUBLIC in-tree (cmake/platforms/pico_hal.cmake),
# which a packaged Conan cpp_info can't express on its own: these are raw
# Pico SDK CMake targets, freshly created by each consuming project's own
# pico_sdk_init() call, not anything coro's package exports. CMakeDeps
# include()s this from coro-config.cmake, right after coroTargets.cmake
# declares coro::pico_hal (see coro's package_info()/layout(), root-level
# "cmake_build_modules" property — CMakeDeps only supports this at the
# package level, not per-component), so any consumer just needs
# find_package(coro) after its own pico_sdk_init() — no manual relinking
# required, including for modules (like PIO) that coro_pico_hal doesn't
# itself have a wrapper for yet but blanket-links anyway — see
# pico_hal.cmake's comment on why this isn't gated per-module.
if(TARGET coro::pico_hal)
    if(TARGET hardware_dma)
        target_link_libraries(coro::pico_hal INTERFACE hardware_dma)
    endif()
    if(TARGET hardware_irq)
        target_link_libraries(coro::pico_hal INTERFACE hardware_irq)
    endif()
    if(TARGET hardware_pio)
        target_link_libraries(coro::pico_hal INTERFACE hardware_pio)
    endif()
endif()
