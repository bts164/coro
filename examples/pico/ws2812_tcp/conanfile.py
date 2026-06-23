from conan import ConanFile
from conan.tools.cmake import cmake_layout, CMake, CMakeDeps, CMakeToolchain


class PicoWs2812TcpRecipe(ConanFile):
    """WS2812B LED controller example for the Pico W.

    Same Conan/Pico SDK coexistence approach as ../pico_led/conanfile.py:
    CMakeToolchain is pared down via the profile's
    tools.cmake.cmaketoolchain:enabled_blocks conf to just "find_paths", so
    pico_sdk_import.cmake/pico_sdk_init() remains the sole owner of the
    compiler/toolchain. Depends on coro/0.1.0 built for settings.os ==
    "baremetal" (coro::pico, coro::pico_hal) and pico_led/0.1.0 (the LED
    driver + effects library, including the nanopb-generated ws2812.pb.h),
    both found via CMakeDeps.
    """

    name = "pico_ws2812_tcp"
    version = "0.1.0"
    package_type = "application"

    settings = "os", "compiler", "build_type", "arch"

    def requirements(self):
        self.requires("coro/0.1.0")
        self.requires("pico_led/0.1.0")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.generate()

        CMakeDeps(self).generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
